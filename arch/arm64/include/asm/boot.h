/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64 启动装载约束学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 * 本文件把 boot loader 与早期内核共同遵守的尺寸/对齐契约集中成常量。它不执行
 * 代码、不拥有内存；固件装载地址和 FDT 布局必须在进入 head.S 前满足这些约束，
 * 否则早期恒等映射的静态页表预算和 2 MiB 块映射假设可能失效。
 */

#ifndef __ASM_BOOT_H
#define __ASM_BOOT_H

#include <linux/sizes.h>

/*
 * arm64 requires the DTB to be 8 byte aligned and
 * not exceed 2MB in size.
 */
/* FDT 起点至少 8 字节对齐且可访问 blob 不超过 2 MiB，供 map_fdt() 预留固定页表池。 */
#define MIN_FDT_ALIGN		8
#define MAX_FDT_SIZE		SZ_2M

/*
 * arm64 requires the kernel image to placed at a 2 MB aligned base address
 */
/* Image 物理基址按 2 MiB 对齐，使物理/虚拟低位同余并允许早期使用 PMD block 映射。 */
#define MIN_KIMG_ALIGN		SZ_2M

#endif
