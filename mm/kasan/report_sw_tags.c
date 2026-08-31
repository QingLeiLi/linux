// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains software tag-based KASAN specific error reporting code.
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 * Author: Andrey Ryabinin <ryabinin.a.a@gmail.com>
 *
 * Some code borrowed from https://github.com/xairy/kasan-prototype by
 *        Andrey Konovalov <andreyknvl@gmail.com>
 */
/*
 * 本文件实现软件标签模式 KASAN 专用的错误报告数据提取；部分思路借鉴早期
 * kasan-prototype。通用 report.c 负责报告编排，这里把带标签指针转换为 shadow
 * 元数据、定位首个标签不匹配粒度并打印 SW_TAGS 特有信息，不改变被报告对象状态。
 */

#include <linux/bitops.h>
#include <linux/ftrace.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/printk.h>

/*
 * 调度与栈相关头提供 current、任务栈归属和 stack depot 诊断接口；它们只在
 * 报告路径读取故障上下文，不为 current 或已保存栈取得新的生命周期引用。
 */
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/kasan.h>
#include <linux/module.h>

/* 架构段边界用于判断地址类别；KASAN 私有头则给出 shadow/tag 与 slab 契约。 */
#include <asm/sections.h>

#include "kasan.h"
#include "../slab.h"

/*
 * kasan_find_first_bad_addr() - 找到访问范围内第一个标签不匹配的 KASAN 粒度。
 * 业务背景：通用 kasan_report() 已判定访问非法，调用这里把报告定位从请求起点
 * 收窄到第一个坏 granule，随后 metadata dump 以该地址为中心展示。
 * 入参：addr 是带软件标签的访问起始指针，纯输入且可指向非 KASAN 元数据区域；
 * size 是访问字节数，调用者已保证范围算术有效，本函数不取得对象引用。
 * 出参/返回：返回去标签后的首个不匹配地址；若地址无 shadow 则返回去标签起点，
 * 若整个范围标签一致则返回去标签的范围末端，不转移 ownership。
 * 注意事项：只读 shadow、不加锁且不睡眠；报告是故障时刻的尽力快照，并发释放或
 * 重新标记可改变元数据。循环按 KASAN_GRANULE_SIZE 前进，不逐字节定位。
 */
const void *kasan_find_first_bad_addr(const void *addr, size_t size)
{
	/* tag 是指针高位标签；p/end 是去标签后的扫描半开区间 [p, end)。 */
	u8 tag = get_tag(addr);
	void *p = kasan_reset_tag(addr);
	void *end = p + size;

	/* 非 KASAN 管理区没有可比较 shadow，起点就是报告能给出的最精确位置。 */
	if (!addr_has_metadata(p))
		return p;

	/* 跳过标签匹配的完整粒度；第一次不相等即为软件插桩判定非法的逻辑位置。 */
	while (p < end && tag == *(u8 *)kasan_mem_to_shadow(p))
		p += KASAN_GRANULE_SIZE;

	return p;
}

/*
 * kasan_get_alloc_size() - 从软件标签 shadow 估算 slab 对象当前有效的分配大小。
 * 业务背景：report.c 已由 slab 元数据定位 object/cache，调用本函数区分对象有效
 * granule 与 KASAN_TAG_INVALID 尾部，补充越界报告中的 alloc_size。
 * 入参：object 是去标签、非 NULL 的 slab 对象起点借用指针；cache 是其有效
 * kmem_cache 借用指针，object_size 给出最大扫描字节数，二者 ownership 不变。
 * 出参/返回：返回 [0, cache->object_size] 字节；首个 invalid shadow 前的粒度数，
 * 全部有效时返回 object_size。已释放对象通常返回 0，不产生副作用。
 * 注意事项：调用契约保证 slab 地址必有 metadata，故跳过范围检查；无锁读取可能
 * 与并发释放/重分配竞争，数值只用于诊断，不能作为安全访问长度。
 */
size_t kasan_get_alloc_size(void *object, struct kmem_cache *cache)
{
	/* size 以字节累计有效粒度；shadow 逐项对应 KASAN_GRANULE_SIZE 个对象字节。 */
	size_t size = 0;
	u8 *shadow;

	/*
	 * Skip the addr_has_metadata check, as this function only operates on
	 * slab memory, which must have metadata.
	 */
	/* 调用者只会为已有 cache/object 的 slab 地址进入此处，slab 必然配置 shadow。 */

	/*
	 * The loop below returns 0 for freed objects, for which KASAN cannot
	 * calculate the allocation size based on the metadata.
	 */
	/*
	 * 已释放对象从首粒度起标为 KASAN_TAG_INVALID，因此循环返回 0；这也说明
	 * shadow 无法在释放后恢复原分配大小，报告只能给出保守结果。
	 */
	shadow = (u8 *)kasan_mem_to_shadow(object);
	/* 每轮消费一个 granule；遇到 invalid 立即返回已经确认仍有效的前缀长度。 */
	while (size < cache->object_size) {
		if (*shadow != KASAN_TAG_INVALID)
			size += KASAN_GRANULE_SIZE;
		else
			return size;
		shadow++;
	}

	return cache->object_size;
}

/*
 * kasan_metadata_fetch_row() - 复制一行软件 shadow 元数据到报告缓冲区。
 * 业务背景：report.c 的 metadata printer 逐行调用，先取稳定的本地副本再格式化。
 * 入参：buffer 是调用者提供、至少 META_BYTES_PER_ROW 字节的纯输出缓冲区；row 是
 * 对应应用内存行的借用地址。出参/返回：无直接返回值；buffer 被完整覆盖，
 * row/shadow ownership 不变。注意事项：不加锁、不睡眠，结果是并发变化下的快照。
 */
void kasan_metadata_fetch_row(char *buffer, void *row)
{
	memcpy(buffer, kasan_mem_to_shadow(row), META_BYTES_PER_ROW);
}

/*
 * kasan_print_tags() - 打印故障指针标签与首坏地址对应的软件内存标签。
 * 业务背景：通用报告在确定 first_bad_addr 后调用，帮助判断陈旧指针或越界标签。
 * 入参：addr_tag 是从原访问指针提取的 8 位标签；addr 是存在 shadow 的借用地址。
 * 出参/返回：无直接返回值；向内核错误日志输出一行，不修改 shadow 或对象。
 * 注意事项：printk 可能序列化并污染时序，只用于错误路径；调用者须确保 addr 可转换。
 */
void kasan_print_tags(u8 addr_tag, const void *addr)
{
	u8 *shadow = (u8 *)kasan_mem_to_shadow(addr);

	pr_err("Pointer tag: [%02x], memory tag: [%02x]\n", addr_tag, *shadow);
}

#ifdef CONFIG_KASAN_STACK
/*
 * kasan_print_address_stack_frame() - 为栈上坏地址打印所属当前任务身份。
 * 业务背景：report.c 识别出栈访问后调用，SW_TAGS 无需解析具体 frame 元数据，先
 * 验证地址确属 current 的任务栈，再输出 comm/pid 连接到故障上下文。
 * 入参：addr 是借用的疑似栈地址，可为非法分类结果。出参/返回：无直接返回值；
 * 合法时打印 current 身份，非栈地址 WARN 并立即返回，不改变任务或地址 ownership。
 * 注意事项：只在 CONFIG_KASAN_STACK 编译；依赖报告发生于相关 current 上下文，
 * 不加锁，comm 只是诊断快照，函数不能证明具体局部变量或栈帧边界。
 */
void kasan_print_address_stack_frame(const void *addr)
{
	/* 防御通用分类与本模式假设不一致，避免为任意地址打印误导性的任务归属。 */
	if (WARN_ON(!object_is_on_stack(addr)))
		return;

	pr_err("The buggy address belongs to stack of task %s/%d\n",
	       current->comm, task_pid_nr(current));
}

/* 关闭 CONFIG_KASAN_STACK 时 kasan.h 提供空桩，本文件不生成该报告实现。 */
#endif
