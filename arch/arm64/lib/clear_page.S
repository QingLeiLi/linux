/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/linkage.h>
#include <linux/const.h>
#include <asm/assembler.h>
#include <asm/page.h>

/*
 * Clear page @dest
 *
 * Parameters:
 *	x0 - dest
 *
 * 学习提示：调用者保证 x0 按 PAGE_SIZE 对齐。因此循环无需保存终点；只要
 * 每次前移固定块长，并检测地址的页内低位何时重新变为 0 即可。优先级为：
 * MOPS（新 CPU 的专用内存操作）> DC ZVA（按缓存块清零）> 普通成对存储。
 */
SYM_FUNC_START(__pi_clear_page)
#ifdef CONFIG_AS_HAS_MOPS
	.arch_extension mops		/* 允许汇编器识别 FEAT_MOPS 指令。 */
alternative_if_not ARM64_HAS_MOPS
	b	.Lno_mops		/* 启动时若 CPU 无 MOPS，替换为此分支。 */
alternative_else_nop_endif

	mov	x1, #PAGE_SIZE		/* MOPS 的长度/状态寄存器，初值为一页。 */
	setpn	[x0]!, x1!, xzr	/* Prologue：校验/规范化参数，填充值为 0。 */
	setmn	[x0]!, x1!, xzr	/* Main：硬件完成主体清零并更新地址、余量。 */
	seten	[x0]!, x1!, xzr	/* Epilogue：完成尾部并结束 MOPS 序列。 */
	ret				/* 整页已清零。 */
.Lno_mops:
#endif
	mrs	x1, dczid_el0		/* 读取 DC ZVA 能力与清零块大小。 */
	tbnz	x1, #4, 2f		/* DZP=1：内核禁止 DC ZVA，退回普通存储。 */
	and	w1, w1, #0xf		/* BS 字段编码 log2(块中字数)。 */
	mov	x2, #4			/* 一个“字”为 4 字节。 */
	lsl	x1, x2, x1		/* x1=4<<BS，即每次 ZVA 清零的字节数。 */

1:	dc	zva, x0			/* 将包含 x0 的一个 ZVA 对齐块全部写成 0。 */
	add	x0, x0, x1		/* 前进一个硬件清零块。 */
	tst	x0, #(PAGE_SIZE - 1)	/* 页内偏移是否已回到 0？ */
	b.ne	1b			/* 未到下一页边界，继续。 */
	ret				/* 恰好处理完一页。 */

2:	stnp	xzr, xzr, [x0]	/* 0..15：一次写两个 64 位零；non-temporal 提示。 */
	stnp	xzr, xzr, [x0, #16]	/* 16..31。 */
	stnp	xzr, xzr, [x0, #32]	/* 32..47。 */
	stnp	xzr, xzr, [x0, #48]	/* 48..63；一轮共清 64 字节。 */
	add	x0, x0, #64		/* 指向下一轮。 */
	tst	x0, #(PAGE_SIZE - 1)	/* 利用页对齐检测终点。 */
	b.ne	2b
	ret
SYM_FUNC_END(__pi_clear_page)
SYM_FUNC_ALIAS(clear_page, __pi_clear_page)
EXPORT_SYMBOL(clear_page)
