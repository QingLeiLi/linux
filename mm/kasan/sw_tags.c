// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains core software tag-based KASAN code.
 *
 * Copyright (c) 2018 Google, Inc.
 * Author: Andrey Konovalov <andreyknvl@google.com>
 */

#define pr_fmt(fmt) "kasan: " fmt

#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/kmemleak.h>
#include <linux/linkage.h>
#include <linux/memblock.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/stacktrace.h>
#include <linux/string.h>
#include <linux/string_choices.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/bug.h>

#include "kasan.h"
#include "../slab.h"

/*
 * prng_state - 软件标签模式下每 CPU 的伪随机数生成器状态
 *
 * sw-tags KASAN 在每次内存分配时生成一个随机标签（1 字节，0x01~0xFF），
 * 写入指针的高位字节，并在 shadow 内存中记录该标签。
 * 访问时将指针标签与 shadow 中记录的标签比较，不匹配则报错。
 *
 * 每个 CPU 维护独立的 PRNG 状态，避免多核并发时的竞争。
 * 使用廉价的 PRNG（非加密级别），因为标签是概率性检测工具，
 * 不需要密码学强度，只需足够随机以使相邻对象标签不同。
 */
static DEFINE_PER_CPU(u32, prng_state);

/*
 * kasan_init_sw_tags - 初始化软件标签（sw-tags）模式的 KASAN
 *
 * 在 smp_prepare_boot_cpu() 中调用，per-cpu 区域切换完成后执行。
 *
 * sw-tags 是 KASAN 的软件实现版本，不依赖 MTE 硬件，适用于所有架构：
 *   - 分配内存时生成随机标签，存入指针高位（top byte）和 shadow 内存
 *   - 访问内存时比较指针标签与 shadow 中存储的标签
 *   - 标签不匹配说明访问了错误的内存（越界、use-after-free 等）
 *
 * 与 hw-tags 相比，sw-tags 开销更高（每次访问需要读 shadow 内存），
 * 但兼容性更好，不依赖特定硬件。
 *
 * 必须在 per-cpu 区域初始化完成后调用，因为 prng_state 存储在 per-cpu 变量中。
 */
void __init kasan_init_sw_tags(void)
{
	int cpu;

	/* 用 get_cycles()（CPU 时钟周期计数器）为每个 CPU 的 PRNG 设置初始种子。
	 * get_cycles() 读取 CNTVCT_EL0（arm64）等硬件计数器，在不同时间点
	 * 调用会得到不同值，保证各 CPU 的种子不同，产生独立的随机标签序列。
	 * 此时 random 子系统尚未完全初始化，无法使用 get_random_u32()，
	 * 用硬件计数器是可用的最好方案。 */
	for_each_possible_cpu(cpu)
		per_cpu(prng_state, cpu) = (u32)get_cycles();

	/* 根据命令行参数初始化调用栈收集配置，与 hw-tags 共用同一实现。
	 * 若开启 stacktrace，从 memblock 分配 stack_ring 缓冲区存储调用栈。 */
	kasan_init_tags();

	/* 通过 static_branch_enable(&kasan_flag_enabled) 开启全局 KASAN 标志。
	 * 此调用之后，slab/slub 分配器开始为每次分配生成随机标签并写入 shadow。 */
	kasan_enable();

	pr_info("KernelAddressSanitizer initialized (sw-tags, stacktrace=%s)\n",
		str_on_off(kasan_stack_collection_enabled()));
}

/*
 * If a preemption happens between this_cpu_read and this_cpu_write, the only
 * side effect is that we'll give a few allocated in different contexts objects
 * the same tag. Since tag-based KASAN is meant to be used a probabilistic
 * bug-detection debug feature, this doesn't have significant negative impact.
 *
 * Ideally the tags use strong randomness to prevent any attempts to predict
 * them during explicit exploit attempts. But strong randomness is expensive,
 * and we did an intentional trade-off to use a PRNG. This non-atomic RMW
 * sequence has in fact positive effect, since interrupts that randomly skew
 * PRNG at unpredictable points do only good.
 */
u8 kasan_random_tag(void)
{
	u32 state = this_cpu_read(prng_state);

	state = 1664525 * state + 1013904223;
	this_cpu_write(prng_state, state);

	return (u8)(state % (KASAN_TAG_MAX + 1));
}

bool kasan_check_range(const void *addr, size_t size, bool write,
			unsigned long ret_ip)
{
	u8 tag;
	u8 *shadow_first, *shadow_last, *shadow;
	void *untagged_addr;

	if (unlikely(size == 0))
		return true;

	if (unlikely(addr + size < addr))
		return !kasan_report(addr, size, write, ret_ip);

	tag = get_tag((const void *)addr);

	/*
	 * Ignore accesses for pointers tagged with 0xff (native kernel
	 * pointer tag) to suppress false positives caused by kmap.
	 *
	 * Some kernel code was written to account for archs that don't keep
	 * high memory mapped all the time, but rather map and unmap particular
	 * pages when needed. Instead of storing a pointer to the kernel memory,
	 * this code saves the address of the page structure and offset within
	 * that page for later use. Those pages are then mapped and unmapped
	 * with kmap/kunmap when necessary and virt_to_page is used to get the
	 * virtual address of the page. For arm64 (that keeps the high memory
	 * mapped all the time), kmap is turned into a page_address call.

	 * The issue is that with use of the page_address + virt_to_page
	 * sequence the top byte value of the original pointer gets lost (gets
	 * set to KASAN_TAG_KERNEL (0xFF)).
	 */
	if (tag == KASAN_TAG_KERNEL)
		return true;

	untagged_addr = kasan_reset_tag((const void *)addr);
	if (unlikely(!addr_has_metadata(untagged_addr)))
		return !kasan_report(addr, size, write, ret_ip);
	shadow_first = kasan_mem_to_shadow(untagged_addr);
	shadow_last = kasan_mem_to_shadow(untagged_addr + size - 1);
	for (shadow = shadow_first; shadow <= shadow_last; shadow++) {
		if (*shadow != tag) {
			return !kasan_report(addr, size, write, ret_ip);
		}
	}

	return true;
}

bool kasan_byte_accessible(const void *addr)
{
	u8 tag = get_tag(addr);
	void *untagged_addr = kasan_reset_tag(addr);
	u8 shadow_byte;

	if (!addr_has_metadata(untagged_addr))
		return false;

	shadow_byte = READ_ONCE(*(u8 *)kasan_mem_to_shadow(untagged_addr));
	return tag == KASAN_TAG_KERNEL || tag == shadow_byte;
}

#define DEFINE_HWASAN_LOAD_STORE(size)					\
	void __hwasan_load##size##_noabort(void *addr)			\
	{								\
		kasan_check_range(addr, size, false, _RET_IP_);		\
	}								\
	EXPORT_SYMBOL(__hwasan_load##size##_noabort);			\
	void __hwasan_store##size##_noabort(void *addr)			\
	{								\
		kasan_check_range(addr, size, true, _RET_IP_);		\
	}								\
	EXPORT_SYMBOL(__hwasan_store##size##_noabort)

DEFINE_HWASAN_LOAD_STORE(1);
DEFINE_HWASAN_LOAD_STORE(2);
DEFINE_HWASAN_LOAD_STORE(4);
DEFINE_HWASAN_LOAD_STORE(8);
DEFINE_HWASAN_LOAD_STORE(16);

void __hwasan_loadN_noabort(void *addr, ssize_t size)
{
	kasan_check_range(addr, size, false, _RET_IP_);
}
EXPORT_SYMBOL(__hwasan_loadN_noabort);

void __hwasan_storeN_noabort(void *addr, ssize_t size)
{
	kasan_check_range(addr, size, true, _RET_IP_);
}
EXPORT_SYMBOL(__hwasan_storeN_noabort);

void __hwasan_tag_memory(void *addr, u8 tag, ssize_t size)
{
	kasan_poison(addr, size, tag, false);
}
EXPORT_SYMBOL(__hwasan_tag_memory);

void kasan_tag_mismatch(void *addr, unsigned long access_info,
			unsigned long ret_ip)
{
	kasan_report(addr, 1 << (access_info & 0xf), access_info & 0x10,
		     ret_ip);
}
