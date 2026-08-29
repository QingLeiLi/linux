// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains hardware tag-based KASAN specific error reporting code.
 *
 * Copyright (c) 2020 Google, Inc.
 * Author: Andrey Konovalov <andreyknvl@google.com>
 */
/*
 * 本文件提供硬件标签型 KASAN 专用的报告取证步骤：从架构内存标签中确定首个
 * 坏地址、估算 slab 对象有效大小、生成标签转储行，并同时打印指针标签和内存
 * 标签。版权与作者信息保留原样；通用报告排版仍由 report.c 负责。
 */

#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/types.h>

#include "kasan.h"

/*
 * kasan_find_first_bad_addr() - 返回硬件标签故障已经精确定位的首个坏地址。
 * 业务背景：report.c 填充报告信息时需要统一的 mode helper；Generic/SW_TAGS
 * 可能扫描影子元数据，而 HW_TAGS 的普通访问 fault 已由硬件给出准确地址。
 * 入参：addr 是带逻辑指针标签、且已知发生标签不匹配的借用地址，不可为 NULL；
 * size 是本次访问字节数，仅为统一接口输入，本实现不使用且不改变任何对象。
 * 出参/返回：返回去掉逻辑标签后的同一地址，不取得引用，也不修改内存。
 * 注意事项：仅适用于 CONFIG_KASAN_HW_TAGS 的普通内存访问报告；正确性依赖调用者
 * 已确认地址存在内存标签，因此这里既不扫描 size，也不做 addr_has_metadata()。
 */
const void *kasan_find_first_bad_addr(const void *addr, size_t size)
{
	/*
	 * Hardware Tag-Based KASAN only calls this function for normal memory
	 * accesses, and thus addr points precisely to the first bad address
	 * with an invalid (and present) memory tag. Therefore:
	 * 1. Return the address as is without walking memory tags.
	 * 2. Skip the addr_has_metadata check.
	 */
	/*
	 * 硬件标签型 KASAN 只为普通内存访问调用本函数，因此 addr 精确指向第一个
	 * 坏地址，并且该地址带有无效但确实存在的内存标签。由此可以：
	 * 1. 不遍历内存标签，直接返回该地址；
	 * 2. 跳过 addr_has_metadata 检查。
	 * kasan_reset_tag() 只清除地址高位的逻辑标签，使后续报告使用规范化地址，
	 * 不会改变目标内存的 allocation tag。
	 */
	return kasan_reset_tag(addr);
}

/*
 * kasan_get_alloc_size() - 依据硬件内存标签估算 slab 对象仍有效的分配大小。
 * 业务背景：report.c 已定位 cache/object 后调用本函数补充报告；HW_TAGS 没有
 * Generic KASAN 的逐对象 metadata，因而按 KASAN granule 扫描有效标签。
 * 入参：object 是 slab 对象起点的借用指针，须按 granule 对齐且属于 cache；cache
 * 是描述 object_size 的借用指针，二者不可为 NULL，调用期间对象元数据须稳定。
 * 出参/返回：返回从 object 起连续非 INVALID 标签覆盖的字节数；完整有效时返回
 * cache->object_size，已释放对象通常返回 0；不转移引用且不修改标签。
 * 注意事项：读取架构标签无需睡眠、函数不取锁；结果是报告期快照，不能用来获得
 * 对象所有权。循环以 cache->object_size 为上界，单位均为字节。
 */
size_t kasan_get_alloc_size(void *object, struct kmem_cache *cache)
{
	/*
	 * size 是已确认有效的前缀字节数；i 是待检查 granule 的零基索引；memory_tag
	 * 保存该 granule 的 allocation tag，仅在本次循环迭代内有效。
	 */
	size_t size = 0;
	int i = 0;
	u8 memory_tag;

	/*
	 * Skip the addr_has_metadata check, as this function only operates on
	 * slab memory, which must have metadata.
	 */
	/*
	 * 跳过 addr_has_metadata 检查，因为本函数只处理 slab 内存，而这种内存在
	 * HW_TAGS KASAN 下必须带 allocation tag；若调用者传入其他地址，契约已被破坏。
	 */

	/*
	 * The loop below returns 0 for freed objects, for which KASAN cannot
	 * calculate the allocation size based on the metadata.
	 */
	/*
	 * 下列循环对已释放对象返回 0：释放路径把首个 granule 标成
	 * KASAN_TAG_INVALID，此后 KASAN 无法再从标签元数据还原原分配大小。
	 */
	/* 阶段 1：逐 granule 扫描连续有效前缀，遇到释放/红区标签立即返回边界。 */
	while (size < cache->object_size) {
		memory_tag = hw_get_mem_tag(object + i * KASAN_GRANULE_SIZE);
		if (memory_tag != KASAN_TAG_INVALID)
			size += KASAN_GRANULE_SIZE;
		else
			return size;
		i++;
	}

	/* 阶段 2：扫描到 cache 声明边界，向报告者承诺对象完整有效。 */
	return cache->object_size;
}

/*
 * kasan_metadata_fetch_row() - 把一行覆盖范围的硬件内存标签复制到报告缓冲区。
 * 业务背景：report.c 逐行生成地址周围的 metadata hexdump，本函数把统一的“一字节
 * metadata”接口映射为每个硬件 granule 的 allocation tag。
 * 入参：buffer 是至少 META_BYTES_PER_ROW 字节的纯输出借用缓冲区；row 是首个
 * granule 的借用地址，必须可查询标签且按 KASAN_GRANULE_SIZE 对齐，均不可为 NULL。
 * 出参/返回：无直接返回值；依次写满 buffer[0..META_BYTES_PER_ROW-1]，不转移所有权。
 * 注意事项：不取锁且不睡眠；得到的是故障报告时刻的标签快照，并发分配/释放可能
 * 使相邻 granule 在读取期间变化，因此它用于诊断而不是同步判定。
 */
void kasan_metadata_fetch_row(char *buffer, void *row)
{
	/* i 同时是输出字节下标和相对 row 的 granule 下标，生命周期仅限循环。 */
	int i;

	/* 一个输出字节对应 KASAN_GRANULE_SIZE 个真实内存字节。 */
	for (i = 0; i < META_BYTES_PER_ROW; i++)
		buffer[i] = hw_get_mem_tag(row + i * KASAN_GRANULE_SIZE);
}

/*
 * kasan_print_tags() - 打印故障指针标签及其目标地址当前的内存标签。
 * 业务背景：通用 KASAN 报告在确定 first_bad_addr 后调用本函数，直接展示硬件
 * 比较的两侧，帮助判断悬空指针、越界或释放后访问。
 * 入参：addr_tag 是从访问指针提取的 8 位逻辑标签值；addr 是待查询 allocation
 * tag 的借用地址，不可为 NULL，本函数不取得引用也不修改地址或内存。
 * 出参/返回：无直接返回值和输出参数；通过 pr_err 向内核日志追加一行诊断。
 * 注意事项：不取锁、不睡眠；memory_tag 是瞬时快照，报告期间对象若被并发复用，
 * 打印值可能反映较新的标签，但不能据此延长对象生命周期。
 */
void kasan_print_tags(u8 addr_tag, const void *addr)
{
	/* memory_tag 只保存 addr 所在 granule 在本次报告时读取到的 8 位标签。 */
	u8 memory_tag = hw_get_mem_tag((void *)addr);

	/* 方括号内并列展示访问携带的标签与内存当前标签，二者不等即触发检查。 */
	pr_err("Pointer tag: [%02x], memory tag: [%02x]\n",
		addr_tag, memory_tag);
}
