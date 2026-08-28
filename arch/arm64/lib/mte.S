/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 ARM Ltd.
 */
#include <linux/linkage.h>

#include <asm/asm-uaccess.h>
#include <asm/assembler.h>
#include <asm/mte.h>
#include <asm/page.h>
#include <asm/sysreg.h>

	.arch	armv8.5-a+memtag

/*
 * multitag_transfer_size - set \reg to the block size that is accessed by the
 * LDGM/STGM instructions.
 */
	.macro	multitag_transfer_size, reg, tmp
	mrs_s	\reg, SYS_GMID_EL1	/* 读取 LDGM/STGM 的实现定义块大小。 */
	ubfx	\reg, \reg, #GMID_EL1_BS_SHIFT, #GMID_EL1_BS_WIDTH /* 提 BS。 */
	mov	\tmp, #4		/* 编码以 4 字节为基数。 */
	lsl	\reg, \tmp, \reg	/* reg=4<<BS，得到一次跨越的数据字节数。 */
	.endm

/*
 * Clear the tags in a page
 *   x0 - address of the page to be cleared
 */
SYM_FUNC_START(mte_clear_page_tags)
	multitag_transfer_size x1, x2 /* x1=一次 STGM 覆盖的数据范围。 */
1:	stgm	xzr, [x0]		/* 把一组全零 allocation tags 写入内存。 */
	add	x0, x0, x1		/* 前进一个多标签块。 */
	tst	x0, #(PAGE_SIZE - 1) /* 页内偏移回零即完成整页。 */
	b.ne	1b
	ret
SYM_FUNC_END(mte_clear_page_tags)

/*
 * Zero the page and tags at the same time
 *
 * Parameters:
 *	x0 - address to the beginning of the page
 */
SYM_FUNC_START(mte_zero_clear_page_tags)
	and	x0, x0, #(1 << MTE_TAG_SHIFT) - 1 /* 清指针 logical tag。 */
	mrs	x1, dczid_el0		/* 查询 GZVA/ZVA 是否允许以及块长。 */
	tbnz	x1, #4, 2f		/* 禁止时改用逐 granule 的 STZ2G。 */
	and	w1, w1, #0xf
	mov	x2, #4
	lsl	x1, x2, x1

1:	dc	gzva, x0		/* 一次清零数据并把对应 allocation tags 置零。 */
	add	x0, x0, x1
	tst	x0, #(PAGE_SIZE - 1)
	b.ne	1b
	ret

2:	stz2g	x0, [x0], #(MTE_GRANULE_SIZE * 2) /* 清两个 granule 及标签。 */
	tst	x0, #(PAGE_SIZE - 1)
	b.ne	2b
	ret
SYM_FUNC_END(mte_zero_clear_page_tags)

/*
 * Copy the tags from the source page to the destination one
 *   x0 - address of the destination page
 *   x1 - address of the source page
 */
SYM_FUNC_START(mte_copy_page_tags)
	mov	x2, x0		/* x2 是可递增的目的游标，保留参数寄存器语义。 */
	mov	x3, x1		/* x3 是源游标。 */
	multitag_transfer_size x5, x6
1:	ldgm	x4, [x3]		/* 将一组 4 位 tags 打包进 x4。 */
	stgm	x4, [x2]		/* 在目的页相同位置展开写入。 */
	add	x2, x2, x5
	add	x3, x3, x5
	tst	x2, #(PAGE_SIZE - 1)
	b.ne	1b
	ret
SYM_FUNC_END(mte_copy_page_tags)

/*
 * Read tags from a user buffer (one tag per byte) and set the corresponding
 * tags at the given kernel address. Used by PTRACE_POKEMTETAGS.
 *   x0 - kernel address (to)
 *   x1 - user buffer (from)
 *   x2 - number of tags/bytes (n)
 * Returns:
 *   x0 - number of tags read/set
 */
SYM_FUNC_START(mte_copy_tags_from_user)
	mov	x3, x1		/* 保存用户缓冲区起点，用于返回完成个数。 */
	cbz	x2, 2f		/* 请求 0 个标签时不访问内存。 */
1:
USER(2f, ldtrb	w4, [x1])	/* 从用户缓冲区读一个 tag；fault 则结束。 */
	lsl	x4, x4, #MTE_TAG_SHIFT	/* 把低 4 位移到指针 logical-tag 字段。 */
	stg	x4, [x0], #MTE_GRANULE_SIZE /* 写 allocation tag，内核地址进 16B。 */
	add	x1, x1, #1		/* 用户表示是一 tag 一字节。 */
	subs	x2, x2, #1		/* 剩余 tag 数减一。 */
	b.ne	1b

	// exception handling and function return
2:	sub	x0, x1, x3		/* 当前用户指针-起点=成功设置数。 */
	ret
SYM_FUNC_END(mte_copy_tags_from_user)

/*
 * Get the tags from a kernel address range and write the tag values to the
 * given user buffer (one tag per byte). Used by PTRACE_PEEKMTETAGS.
 *   x0 - user buffer (to)
 *   x1 - kernel address (from)
 *   x2 - number of tags/bytes (n)
 * Returns:
 *   x0 - number of tags read/set
 */
SYM_FUNC_START(mte_copy_tags_to_user)
	mov	x3, x0		/* 保存用户目的起点，稍后计算完成数。 */
	cbz	x2, 2f
1:
	ldg	x4, [x1]		/* 读取当前 granule 的 allocation tag。 */
	ubfx	x4, x4, #MTE_TAG_SHIFT, #MTE_TAG_SIZE /* 提取 4 位 tag。 */
USER(2f, sttrb	w4, [x0])
	add	x0, x0, #1
	add	x1, x1, #MTE_GRANULE_SIZE
	subs	x2, x2, #1
	b.ne	1b

	// exception handling and function return
2:	sub	x0, x0, x3		// update the number of tags copied
	ret
SYM_FUNC_END(mte_copy_tags_to_user)

/*
 * Save the tags in a page
 *   x0 - page address
 *   x1 - tag storage, MTE_PAGE_TAG_STORAGE bytes
 */
SYM_FUNC_START(mte_save_page_tags)
	multitag_transfer_size x7, x5 /* x7=每次 LDGM 涵盖字节数。 */
1:
	mov	x2, #0		/* 清空用于汇总 16 个 tag 的打包寄存器。 */
2:
	ldgm	x5, [x0]		/* 取一组打包标签。 */
	orr	x2, x2, x5		/* 各次负责不同 nibble，OR 成 64 位存储项。 */
	add	x0, x0, x7
	tst	x0, #0xFF		// 16 tag values fit in a register,
	b.ne	2b			// which is 16*16=256 bytes

	str	x2, [x1], #8		/* 保存 16 个 4 位标签，共 8 字节。 */

	tst	x0, #(PAGE_SIZE - 1)
	b.ne	1b

	ret
SYM_FUNC_END(mte_save_page_tags)

/*
 * Restore the tags in a page
 *   x0 - page address
 *   x1 - tag storage, MTE_PAGE_TAG_STORAGE bytes
 */
SYM_FUNC_START(mte_restore_page_tags)
	multitag_transfer_size x7, x5
1:
	ldr	x2, [x1], #8		/* 取回 16 个打包标签。 */
2:
	stgm	x2, [x0]		/* 将属于当前地址组的标签展开写回。 */
	add	x0, x0, x7
	tst	x0, #0xFF
	b.ne	2b

	tst	x0, #(PAGE_SIZE - 1)
	b.ne	1b

	ret
SYM_FUNC_END(mte_restore_page_tags)
