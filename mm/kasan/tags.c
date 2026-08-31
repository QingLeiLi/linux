// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains common tag-based KASAN code.
 *
 * Copyright (c) 2018 Google, Inc.
 * Copyright (c) 2020 Google, Inc.
 */

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/memory.h>
#include <linux/mm.h>
/* 栈仓库保存调用链，static key 则把关闭采集时的热路径成本降到最低。 */
#include <linux/sched/clock.h>
#include <linux/stackdepot.h>
#include <linux/static_key.h>
#include <linux/string.h>
#include <linux/types.h>

#include "kasan.h"
#include "../slab.h"

#define KASAN_STACK_RING_SIZE_DEFAULT (32 << 10)

/*
 * 启动参数的三态表示法把“用户未指定”与显式 on/off 区分开。这样默认值仍由
 * static key 的定义决定，命令行只在确有要求时覆盖它。
 */
enum kasan_arg_stacktrace {
	KASAN_ARG_STACKTRACE_DEFAULT,
	KASAN_ARG_STACKTRACE_OFF,
	KASAN_ARG_STACKTRACE_ON,
};

static enum kasan_arg_stacktrace kasan_arg_stacktrace __initdata;

/* Whether to collect alloc/free stack traces. */
/*
 * 热路径通过 static key 查询是否采集栈；默认开启，初始化阶段可把分支改写成
 * 几乎无开销的关闭路径。该开关同时约束栈环分配和后续 alloc/free 记录。
 */
DEFINE_STATIC_KEY_TRUE(kasan_flag_stacktrace);

/* Non-zero, as initial pointer values are 0. */
/* 1 不会与正常对象地址混淆，用作写者临时占有环槽的哨兵值。 */
#define STACK_RING_BUSY_PTR ((void *)1)

/*
 * 标签模式没有逐对象 KASAN 元数据，改用这个覆盖式全局环保存近期生命周期。
 * 报告端按对象地址回溯环；容量有限意味着旧记录允许被新记录淘汰。
 */
struct kasan_stack_ring stack_ring = {
	.lock = __RW_LOCK_UNLOCKED(stack_ring.lock)
};

/* kasan.stacktrace=off/on */
/*
 * early_kasan_flag_stacktrace() - 解析是否采集分配/释放栈的早期启动参数。
 *
 * early_param 在常规内存初始化前调用，因此这里只保存选择，真正修改 static key
 * 留给 kasan_init_tags()。空值或未知字符串返回 -EINVAL，由参数框架报告无效参数。
 */
static int __init early_kasan_flag_stacktrace(char *arg)
{
	if (!arg)
		return -EINVAL;

	if (!strcmp(arg, "off"))
		kasan_arg_stacktrace = KASAN_ARG_STACKTRACE_OFF;
	/* 只有两个精确字符串属于有效接口，避免拼写错误悄然改变诊断开销。 */
	else if (!strcmp(arg, "on"))
		kasan_arg_stacktrace = KASAN_ARG_STACKTRACE_ON;
	else
		return -EINVAL;

	return 0;
}
early_param("kasan.stacktrace", early_kasan_flag_stacktrace);

/* kasan.stack_ring_size=<number of entries> */
/*
 * 数值表示环中的条目数而非字节数；基数 0 允许常见的十进制/十六进制写法。
 * 解析结果先写入 size，初始化时再据此一次性从 memblock 分配连续存储。
 */
static int __init early_kasan_flag_stack_ring_size(char *arg)
{
	if (!arg)
		return -EINVAL;

	return kstrtoul(arg, 0, &stack_ring.size);
}
early_param("kasan.stack_ring_size", early_kasan_flag_stack_ring_size);

void __init kasan_init_tags(void)
{
	/* 先落实命令行覆盖；DEFAULT 保持 DEFINE_STATIC_KEY_TRUE 的默认策略。 */
	switch (kasan_arg_stacktrace) {
	case KASAN_ARG_STACKTRACE_DEFAULT:
		/* Default is specified by kasan_flag_stacktrace definition. */
		break;
	case KASAN_ARG_STACKTRACE_OFF:
		static_branch_disable(&kasan_flag_stacktrace);
		break;
	/* 显式 on 也执行 enable，以覆盖未来可能变化的编译期默认值。 */
	case KASAN_ARG_STACKTRACE_ON:
		static_branch_enable(&kasan_flag_stacktrace);
		break;
	}

	if (kasan_stack_collection_enabled()) {
		/* 0 表示未指定容量，而不是请求一个空环。 */
		if (!stack_ring.size)
			stack_ring.size = KASAN_STACK_RING_SIZE_DEFAULT;
		/*
		 * 此时伙伴系统尚未必可用，故从 memblock 分配并按缓存线对齐。
		 * 分配失败时必须关闭采集，否则热路径会对 NULL entries 取模寻址。
		 */
		stack_ring.entries = memblock_alloc(
			sizeof(stack_ring.entries[0]) * stack_ring.size,
			SMP_CACHE_BYTES);
		if (WARN_ON(!stack_ring.entries))
			static_branch_disable(&kasan_flag_stacktrace);
	}
}

/*
 * save_stack_info() - 将一次标签模式 slab 生命周期事件追加到栈环。
 *
 * cache/object 由当前分配或释放路径借用，调用期间必须有效；gfp_flags 约束 Stack
 * Depot 必要时的内部申请，is_free 区分事件类型。函数取得新 stack 句柄的引用，
 * 用原子哨兵仲裁并发写者，在 read-side 锁内整体替换一个槽，随后释放被覆盖的旧
 * stack 引用。固定环可能覆盖历史且繁忙槽会被跳过，所以保存是尽力而为的诊断
 * 记录，不参与对象本身的所有权或分配成功语义。
 */
static void save_stack_info(struct kmem_cache *cache, void *object,
			gfp_t gfp_flags, bool is_free)
{
	unsigned long flags;
	depot_stack_handle_t stack, old_stack;
	u64 pos;
	struct kasan_stack_ring_entry *entry;
	void *old_ptr;

	/*
	 * Stack Depot 的 GET 标志为新句柄取得引用；允许分配使分配路径沿用调用者
	 * 的 gfp 约束，释放路径传 0 则以最受限的标志尝试仓库内部申请。
	 */
	stack = kasan_save_stack(gfp_flags,
			STACK_DEPOT_FLAG_CAN_ALLOC | STACK_DEPOT_FLAG_GET);

	/*
	 * Prevent save_stack_info() from modifying stack ring
	 * when kasan_complete_mode_report_info() is walking it.
	 */
	read_lock_irqsave(&stack_ring.lock, flags);

next:
	/*
	 * pos 单调分配候选序号，取模形成覆盖式环。遇到并发写槽时跳到下一序号，
	 * 不等待持有者，避免 slab 热路径阻塞；因此允许出现被跳过的位置。
	 */
	pos = atomic64_fetch_add(1, &stack_ring.pos);
	entry = &stack_ring.entries[pos % stack_ring.size];

	/* Detect stack ring entry slots that are being written to. */
	old_ptr = READ_ONCE(entry->ptr);
	if (old_ptr == STACK_RING_BUSY_PTR)
		goto next; /* Busy slot. */
	if (!try_cmpxchg(&entry->ptr, &old_ptr, STACK_RING_BUSY_PTR))
		goto next; /* Busy slot. */

	/* 成功把 ptr 换成 BUSY 后，本写者独占该槽，可以读取并替换其余字段。 */
	old_stack = entry->track.stack;

	/* object_size 用于报告边界，track 同时记录新栈句柄及进程等上下文。 */
	entry->size = cache->object_size;
	kasan_set_track(&entry->track, stack);
	entry->is_free = is_free;

	/* 最后发布真实对象指针：报告端不会观察到半更新的有效条目。 */
	entry->ptr = object;

	read_unlock_irqrestore(&stack_ring.lock, flags);

	/* 槽内旧句柄已不可达，在锁外归还引用以缩短读锁/关中断区间。 */
	if (old_stack)
		stack_depot_put(old_stack);
}

/* 记录一次 slab 对象分配；调用者提供的 GFP 约束传递给栈仓库。 */
void kasan_save_alloc_info(struct kmem_cache *cache, void *object, gfp_t flags)
{
	save_stack_info(cache, object, flags, false);
}

/* 记录一次 slab 对象释放；is_free 使报告端把该条目解释为释放历史。 */
void kasan_save_free_info(struct kmem_cache *cache, void *object)
{
	save_stack_info(cache, object, 0, true);
}
