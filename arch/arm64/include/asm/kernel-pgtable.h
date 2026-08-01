/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arm64 早期内核页表容量计算学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 * 本文件不写页表，而是在编译/链接期计算 head.S 与 pi/map_*.c 必须静态预留多少
 * 页表页。输入来自页大小、VA_BITS、层数、镜像范围和可重定位配置；输出决定
 * init_pg_dir、init_idmap_pg_dir 及 FDT 临时表的尺寸。启动期没有分配器和失败回滚，
 * 因而公式宁可保守多留页，也不能低估跨层级/跨 block 边界的最坏情况。
 */
/*
 * Kernel page table mapping
 *
 * Copyright (C) 2015 ARM Ltd.
 */

#ifndef __ASM_KERNEL_PGTABLE_H
#define __ASM_KERNEL_PGTABLE_H

#include <asm/boot.h>
#include <asm/pgtable-hwdef.h>
#include <asm/sparsemem.h>

/*
 * The physical and virtual addresses of the start of the kernel image are
 * equal modulo 2 MiB (per the arm64 booting.txt requirements). Hence we can
 * use section mapping with 4K (section size = 2M) but not with 16K (section
 * size = 32M) or 64K (section size = 512M).
 */
/*
 * 启动协议只保证物理与虚拟起点模 2 MiB 相等：4K 页的 PMD block 正好是 2 MiB，
 * 可跳过最后一级；16K/64K 的 PMD block 更大，不能保证同时对齐，只能落到页级。
 */
#if defined(PMD_SIZE) && PMD_SIZE <= MIN_KIMG_ALIGN
#define SWAPPER_BLOCK_SHIFT	PMD_SHIFT
#define SWAPPER_SKIP_LEVEL	1
#else
#define SWAPPER_BLOCK_SHIFT	PAGE_SHIFT
#define SWAPPER_SKIP_LEVEL	0
#endif
#define SWAPPER_BLOCK_SIZE	(UL(1) << SWAPPER_BLOCK_SHIFT)
/* BLOCK_SHIFT/SKIP_LEVEL 共同决定正式内核映射的叶级粒度和实际需要遍历的层数。 */

#define SWAPPER_PGTABLE_LEVELS		(CONFIG_PGTABLE_LEVELS - SWAPPER_SKIP_LEVEL)
#define INIT_IDMAP_PGTABLE_LEVELS	(IDMAP_LEVELS - SWAPPER_SKIP_LEVEL)
/*
 * SWAPPER_PGTABLE_LEVELS 是 init_pg_dir 覆盖高地址内核镜像时实际需要分配的表级数，
 * 输入采用内核 CONFIG_PGTABLE_LEVELS；INIT_IDMAP_PGTABLE_LEVELS 则服务 TTBR0 的
 * 早期恒等映射，输入采用固定 48 位 IDMAP_LEVELS。两者都扣除可用 block 叶项跳过
 * 的一级，但地址空间宽度来源不同，不能互换，否则链接器预留区可能小于建表需求。
 */

#define IDMAP_VA_BITS		48
#define IDMAP_LEVELS		ARM64_HW_PGTABLE_LEVELS(IDMAP_VA_BITS)
#define IDMAP_ROOT_LEVEL	(4 - IDMAP_LEVELS)
/* idmap 固定按 48 位 VA 预算；ROOT_LEVEL 把“需要几层”换算为架构编号 -1..3 的起点。 */

/*
 * A relocatable kernel may execute from an address that differs from the one at
 * which it was linked. In the worst case, its runtime placement may intersect
 * with two adjacent PGDIR entries, which means that an additional page table
 * may be needed at each subordinate level.
 */
/* 可重定位镜像最坏会跨相邻 PGDIR 项，因此每个下级额外预留一页，避免 KASLR 后溢出。 */
#define EXTRA_PAGE	__is_defined(CONFIG_RELOCATABLE)

#define SPAN_NR_ENTRIES(vstart, vend, shift) \
	((((vend) - 1) >> (shift)) - ((vstart) >> (shift)) + 1)
/* 对半开区间 [vstart,vend) 计数；vend-1 防止恰落边界时误多算一个表项。 */
/*
 * vstart/vend 的单位都是字节地址，shift 是单个目标表项覆盖字节数的以 2 为底指数；
 * 返回覆盖该区间的表项个数而非字节数。调用者必须保证 vend>vstart，否则 vend-1
 * 会下溢，这一前置条件由所有启动镜像/FDT 范围调用点满足。
 */

#define EARLY_ENTRIES(lvl, vstart, vend) \
	SPAN_NR_ENTRIES(vstart, vend, SWAPPER_BLOCK_SHIFT + lvl * PTDESC_TABLE_SHIFT)

#define EARLY_LEVEL(lvl, lvls, vstart, vend, add) \
	((lvls) > (lvl) ? EARLY_ENTRIES(lvl, vstart, vend) + (add) : 0)
/*
 * EARLY_ENTRIES 的 lvl 是从最终 block/page 叶级向根方向计数的相对层号，返回半开
 * 地址区间 [vstart,vend) 在该层占用多少个父表项；每个这样的父表项都需要一张
 * 下一层页表。EARLY_LEVEL 再用 lvls 判断该层是否真实存在：存在便加入 add 张
 * 最坏情况余量，不存在则返回 0。四个参数均是纯编译期整数，没有运行期状态或失败值。
 */

#define EARLY_PAGES(lvls, vstart, vend, add) (1 	/* PGDIR page */				\
	+ EARLY_LEVEL(3, (lvls), (vstart), (vend), add) /* each entry needs a next level page table */	\
	+ EARLY_LEVEL(2, (lvls), (vstart), (vend), add)	/* each entry needs a next level page table */	\
	+ EARLY_LEVEL(1, (lvls), (vstart), (vend), add))/* each entry needs a next level page table */
/* EARLY_PAGES 汇总一张根页及每个有效上层表项所需的下一层页；add 是重定位保守余量。 */
/*
 * lvls 决定参与求和的层数，vstart/vend 给出待映射半开区间，add 是每个存在层级
 * 额外增加的边界余量；返回值单位是“页表页”，包含根页。它只估算存储容量，不会
 * 创建描述符，也不包含被映射的数据页。调用者随后乘 PAGE_SIZE 交给链接脚本预留。
 */
#define INIT_DIR_SIZE (PAGE_SIZE * (EARLY_PAGES(SWAPPER_PGTABLE_LEVELS, KIMAGE_VADDR, _end, EXTRA_PAGE) \
				    + EARLY_SEGMENT_EXTRA_PAGES))
/*
 * INIT_DIR_SIZE 是 init_pg_dir 的字节容量：覆盖 KIMAGE_VADDR.._end，并叠加 KASLR
 * 跨 PGDIR 的 EXTRA_PAGE 和权限区段拆分余量。vmlinux.lds.S 用它界定
 * __pi_init_pg_dir..__pi_init_pg_end，早期 map_kernel() 从这块静态池顺序领取表页。
 */

#define INIT_IDMAP_DIR_PAGES	(EARLY_PAGES(INIT_IDMAP_PGTABLE_LEVELS, KIMAGE_VADDR, kimage_limit, 1))
#define INIT_IDMAP_DIR_SIZE	((INIT_IDMAP_DIR_PAGES + EARLY_IDMAP_EXTRA_PAGES) * PAGE_SIZE)
/*
 * INIT_IDMAP_DIR_PAGES 计算内核恒等映射本体所需页数，固定 add=1 以承受物理放置跨
 * 上层边界；INIT_IDMAP_DIR_SIZE 再加入镜像分段/未对齐余量并换算为字节。
 * vmlinux.lds.S 据此静态预留 init_idmap_pg_dir，CPU 开 MMU、切换 TTBR 以及 LPA2
 * 重建期间都依赖该区域，容量不足没有可恢复的运行期错误路径。
 */

#define INIT_IDMAP_FDT_PAGES	(EARLY_PAGES(INIT_IDMAP_PGTABLE_LEVELS, 0UL, UL(MAX_FDT_SIZE), 1) - 1)
#define INIT_IDMAP_FDT_SIZE	((INIT_IDMAP_FDT_PAGES + EARLY_IDMAP_EXTRA_FDT_PAGES) * PAGE_SIZE)
/*
 * INIT_IDMAP_FDT_PAGES 只预算把任意起点、最大 MAX_FDT_SIZE 的 FDT 接到既有
 * init_idmap_pg_dir 下方所需的子表，所以减去 EARLY_PAGES 已计入的根页；根页由前一
 * 组容量持有，不能在 map_fdt() 的临时池中重复计算。INIT_IDMAP_FDT_SIZE 再加入 FDT
 * 横跨 block 边界的余量并换算为字节，供 map_kernel.c 的 __initdata ptes[] 使用；
 * 启动参数解析结束后该临时池即可随 init 内存回收。
 */

/* The number of segments in the kernel image (text, rodata, inittext, initdata, data+bss) */
/* 五个长期权限区段分别为 text、rodata、inittext、initdata、data+bss。 */
#define KERNEL_SEGMENT_COUNT	5

#if SWAPPER_BLOCK_SIZE > SEGMENT_ALIGN
/*
 * KERNEL_SEGMENT_COUNT counts the permanent kernel VMAs. The early mapping
 * has one additional split, [_text, _stext). Reserve one more page for the
 * SWAPPER_BLOCK_SIZE-unaligned boundaries.
 */
/* 正式五段外，早期映射多一个 [_text,_stext) 切分；每个未对齐边界可能消耗额外页。 */
#define EARLY_SEGMENT_EXTRA_PAGES (KERNEL_SEGMENT_COUNT + 2)
/*
 * The initial ID map consists of the kernel image, mapped as two separate
 * segments, and may appear misaligned wrt the swapper block size. This means
 * we need 3 additional pages. The DT could straddle a swapper block boundary,
 * so it may need 2.
 */
/* idmap 的两段镜像最坏需三页边界余量；FDT 横跨一个 block 时最多需要两页。 */
#define EARLY_IDMAP_EXTRA_PAGES		3
#define EARLY_IDMAP_EXTRA_FDT_PAGES	2
#else
#define EARLY_SEGMENT_EXTRA_PAGES	0
#define EARLY_IDMAP_EXTRA_PAGES		0
#define EARLY_IDMAP_EXTRA_FDT_PAGES	0
#endif

#endif	/* __ASM_KERNEL_PGTABLE_H */
