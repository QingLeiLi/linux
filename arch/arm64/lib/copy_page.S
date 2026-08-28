/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/linkage.h>
#include <linux/const.h>
#include <asm/assembler.h>
#include <asm/page.h>
#include <asm/cpufeature.h>
#include <asm/alternative.h>

/*
 * Copy a page from src to dest (both are page aligned)
 *
 * Parameters:
 *	x0 - dest
 *	x1 - src
 *
 * 两端都按页对齐且长度固定。通用路径先预取 128 字节到 x2..x17，随后
 * “存上一块、取下一块”交错执行，隐藏加载延迟。x0 预加 256 是为了让
 * 存储统一使用负偏移；当 x0 的页内低位变 0 时，寄存器里还压着最后
 * 128 字节，所以循环后另有一次最终写回。
 */
SYM_FUNC_START(__pi_copy_page)
#ifdef CONFIG_AS_HAS_MOPS
	.arch_extension mops
alternative_if_not ARM64_HAS_MOPS
	b	.Lno_mops
alternative_else_nop_endif

	mov	x2, #PAGE_SIZE		/* MOPS 长度。 */
	cpypwn	[x0]!, [x1]!, x2!	/* Prologue：建立本次页复制的 MOPS 状态。 */
	cpymwn	[x0]!, [x1]!, x2!	/* Main：复制主体。 */
	cpyewn	[x0]!, [x1]!, x2!	/* Epilogue：完成尾部。 */
	ret
.Lno_mops:
#endif
	ldp	x2, x3, [x1]		/* 预取源 0..15。 */
	ldp	x4, x5, [x1, #16]	/* 预取源 16..31。 */
	ldp	x6, x7, [x1, #32]	/* 预取源 32..47。 */
	ldp	x8, x9, [x1, #48]	/* 预取源 48..63。 */
	ldp	x10, x11, [x1, #64]	/* 预取源 64..79。 */
	ldp	x12, x13, [x1, #80]	/* 预取源 80..95。 */
	ldp	x14, x15, [x1, #96]	/* 预取源 96..111。 */
	ldp	x16, x17, [x1, #112]	/* 预取源 112..127。 */

	add	x0, x0, #256		/* 给负存储偏移预留基址。 */
	add	x1, x1, #128		/* 源指向下一块待读数据。 */
1:
	tst	x0, #(PAGE_SIZE - 1)	/* 先设置循环条件；中间指令不改标志位。 */

	stnp	x2, x3, [x0, #-256]
	ldp	x2, x3, [x1]
	stnp	x4, x5, [x0, #16 - 256]
	ldp	x4, x5, [x1, #16]
	stnp	x6, x7, [x0, #32 - 256]
	ldp	x6, x7, [x1, #32]
	stnp	x8, x9, [x0, #48 - 256]
	ldp	x8, x9, [x1, #48]
	stnp	x10, x11, [x0, #64 - 256]
	ldp	x10, x11, [x1, #64]
	stnp	x12, x13, [x0, #80 - 256]
	ldp	x12, x13, [x1, #80]
	stnp	x14, x15, [x0, #96 - 256]
	ldp	x14, x15, [x1, #96]
	stnp	x16, x17, [x0, #112 - 256]
	ldp	x16, x17, [x1, #112]

	add	x0, x0, #128		/* 目的滑到下一 128 字节窗口。 */
	add	x1, x1, #128		/* 源同样前移。 */

	b.ne	1b			/* 使用循环开头 tst 留下的条件码。 */

	stnp	x2, x3, [x0, #-256]
	stnp	x4, x5, [x0, #16 - 256]
	stnp	x6, x7, [x0, #32 - 256]
	stnp	x8, x9, [x0, #48 - 256]
	stnp	x10, x11, [x0, #64 - 256]
	stnp	x12, x13, [x0, #80 - 256]
	stnp	x14, x15, [x0, #96 - 256]
	stnp	x16, x17, [x0, #112 - 256]

	ret				/* 最后一批寄存器数据写完后返回。 */
SYM_FUNC_END(__pi_copy_page)
SYM_FUNC_ALIAS(copy_page, __pi_copy_page)
EXPORT_SYMBOL(copy_page)
