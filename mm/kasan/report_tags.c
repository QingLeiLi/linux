// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 * Copyright (c) 2020 Google, Inc.
 */

#include <linux/atomic.h>

#include "kasan.h"
#include "../slab.h"

/*
 * 标签模式 KASAN 的分配/释放栈环形缓冲区，由 tags.c 在启动期分配并由
 * save_stack_info() 持续写入。本文件只借用该全局对象补全报告；write lock 会
 * 暂停并发保存者，保证一次扫描看到结构完整的 entry，但历史仍可能已被覆盖。
 */
extern struct kasan_stack_ring stack_ring;

/*
 * get_common_bug_type() - 在没有可靠 slab 历史时给访问错误选择通用分类。
 * 业务背景：kasan_complete_mode_report_info() 找不到匹配环条目时调用，使报告仍有
 * out-of-bounds 或 invalid-access 标题，而不把尽力而为的历史缺失当成报告失败。
 * 入参：info 是调用者持有、非 NULL 的报告上下文借用指针；本函数只读取其中的
 * access_addr/access_size，size 可能以负数编码特殊访问。
 * 出参/返回：返回静态只读字符串，不需释放；地址加长度回绕时返回
 * "out-of-bounds"，否则返回 "invalid-access"，不修改 info 或 ownership。
 * 注意事项：这是算术启发式，不证明对象边界；不加锁、不睡眠，调用者负责有效范围。
 */
static const char *get_common_bug_type(struct kasan_report_info *info)
{
	/*
	 * If access_size is a negative number, then it has reason to be
	 * defined as out-of-bounds bug type.
	 *
	 * Casting negative numbers to size_t would indeed turn up as
	 * a large size_t and its value will be larger than ULONG_MAX/2,
	 * so that this can qualify as out-of-bounds.
	 */
	/*
	 * access_size 为负数时按 size_t/地址算术参与加法会变成巨大正值并产生回绕，
	 * 因而应归为越界；这避免负长度被误标为普通 invalid-access。
	 */
	if (info->access_addr + info->access_size < info->access_addr)
		return "out-of-bounds";

	return "invalid-access";
}

/*
 * kasan_complete_mode_report_info() - 用标签栈环历史补全 slab 故障类型及生命周期栈。
 *
 * 业务背景：report.c 已定位 first_bad_addr、cache 和 object，并为 invalid/double free
 * 预设 bug_type；标签模式在打印前调用本函数，反向扫描最近的 alloc/free 事件，
 * 将匹配 track 复制进 info 并推断 use-after-free 或 slab-out-of-bounds。
 * 入参：info 是调用者拥有的非 NULL 输入输出报告对象；普通非 slab 访问允许
 * cache/object 缺失并走通用分类；进入环扫描则要求二者均有效。bug_type 可由上游
 * 预置，access_addr 保留故障指针标签。本函数只借用 cache/object，并写
 * info->bug_type、alloc_track、free_track，不取得 slab 或 stack depot 引用。
 * 出参/返回：无直接返回值；至多填入最近一组分配/释放 track。没有可用对象历史时
 * 写通用分类；已有 bug_type 不被推断结果覆盖。所有字符串均为静态存储。
 * 注意事项：若上游预置 bug_type，仍须同时提供可扫描的 cache/object；
 * write_lock_irqsave 排斥 tags.c 中持 read lock 的并发保存者并关闭本地中断，持锁
 * 期间不得睡眠；固定容量、标签复用和对象重用使结果仅为 best effort。
 */
void kasan_complete_mode_report_info(struct kasan_report_info *info)
{
	/* flags 保存本地 IRQ 状态；pos 是扫描快照，entry 借用环槽；两个 bool 标记已命中类型。 */
	unsigned long flags;
	u64 pos;
	struct kasan_stack_ring_entry *entry;
	bool alloc_found = false, free_found = false;

	/* 非 slab 地址且上游未分类时没有可构造的环匹配键，直接给出通用类型。 */
	if ((!info->cache || !info->object) && !info->bug_type) {
		info->bug_type = get_common_bug_type(info);
		return;
	}

	/*
	 * 保存者以 read side 并行写不同槽并用 cmpxchg 仲裁；报告取得 write side，
	 * 因而扫描期间不会观察到 ptr/size/track/is_free 的半写入组合。
	 */
	write_lock_irqsave(&stack_ring.lock, flags);

	/* atomic pos 是下一个序号；锁稳定 entries 内容，原子读取保留与写入者的契约。 */
	pos = atomic64_read(&stack_ring.pos);

	/*
	 * The loop below tries to find stack ring entries relevant to the
	 * buggy object. This is a best-effort process.
	 *
	 * First, another object with the same tag can be allocated in place of
	 * the buggy object. Also, since the number of entries is limited, the
	 * entries relevant to the buggy object can be overwritten.
	 */
	/*
	 * 从最新序号反向检查固定容量的每个槽，尽力寻找当前坏对象相关记录。相同
	 * 地址/标签可能已分给新对象，容量有限也会覆盖旧历史，所以不能把未命中
	 * 解释为“从未分配/释放”，也不能把命中当成绝对时间证明。
	 */

	/* i 是单调回绕的逻辑序号，取模映射到固定环槽；恰好遍历 stack_ring.size 项。 */
	for (u64 i = pos - 1; i != pos - 1 - stack_ring.size; i--) {
		/* 已同时取得最近分配和释放快照，报告所需信息齐全，无需扫描更旧历史。 */
		if (alloc_found && free_found)
			break;

		entry = &stack_ring.entries[i % stack_ring.size];

		/*
		 * 匹配键同时要求去标签对象地址、故障指针标签和 cache 对象大小一致；
		 * 三者共同降低地址复用造成的误关联，任一不符就继续看更旧记录。
		 */
		if (kasan_reset_tag(entry->ptr) != info->object ||
		    get_tag(entry->ptr) != get_tag(info->access_addr) ||
		    info->cache->object_size != entry->size)
			continue;

		if (entry->is_free) {
			/*
			 * Second free of the same object.
			 * Give up on trying to find the alloc entry.
			 */
			/*
			 * 反向扫描若遇到第二条同对象 free，说明已跨过另一个生命周期边界；
			 * 再找更旧 alloc 可能配错对象，因此停止而不是拼接虚假历史。
			 */
			if (free_found)
				break;

			/* 在锁内复制 track 值；info 持有快照，不持有或释放 entry 槽本身。 */
			memcpy(&info->free_track, &entry->track,
			       sizeof(info->free_track));
			free_found = true;

			/*
			 * If a free entry is found first, the bug is likely
			 * a use-after-free.
			 */
			/* 最新匹配事件先是 free 时，故障最符合释放后继续使用。 */
			if (!info->bug_type)
				info->bug_type = "slab-use-after-free";
		} else {
			/* Second alloc of the same object. Give up. */
			/* 第二条 alloc 同样越过对象重用边界；停止以免关联更旧生命周期。 */
			if (alloc_found)
				break;

			/* 保存最近分配栈快照，后续通用 printer 用它解释对象来源。 */
			memcpy(&info->alloc_track, &entry->track,
			       sizeof(info->alloc_track));
			alloc_found = true;

			/*
			 * If an alloc entry is found first, the bug is likely
			 * an out-of-bounds.
			 */
			/* 最新匹配事件先是 alloc 且尚无上游分类时，更符合对象边界外访问。 */
			if (!info->bug_type)
				info->bug_type = "slab-out-of-bounds";
		}
	}

	/* 扫描完成后允许保存者继续覆盖环槽，并精确恢复进入函数前的 IRQ 状态。 */
	write_unlock_irqrestore(&stack_ring.lock, flags);

	/* Assign the common bug type if no entries were found. */
	/* 环历史未给出分类时使用地址算术回退，保证 bug_type 在成功返回后非 NULL。 */
	if (!info->bug_type)
		info->bug_type = get_common_bug_type(info);
}
