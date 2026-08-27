/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/lib/strrchr.S
 *
 * Copyright (C) 1995-2000 Russell King
 * Copyright (C) 2013 ARM Ltd.
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

/*
 * Find the last occurrence of a character in a string.
 *
 * Parameters:
 *	x0 - str
 *	x1 - c
 * Returns:
 *	x0 - address of last occurrence of 'c' or 0
 */
SYM_FUNC_START(__pi_strrchr)
	/* x3 保存“最近一次命中的地址”；0 表示至今未命中。 */
	mov	x3, #0
	/* C 的字符参数按 unsigned char 语义截成 8 位。 */
	and	w1, w1, #0xff
1:	/* 读取当前字节，x0 同时后移一字节。 */
	ldrb	w2, [x0], #1
	/* NUL 结束字符串；本实现不把终止 NUL 当作可匹配字符。 */
	cbz	w2, 2f
	/* 当前字节与目标字符比较。 */
	cmp	w2, w1
	/* 未命中就继续扫描。 */
	b.ne	1b
	/* 命中：后递增后的 x0 减 1 才是该字符地址。 */
	sub	x3, x0, #1
	/* 不立即返回，继续寻找位置更靠后的命中。 */
	b	1b
2:	/* 扫描结束，把最后命中地址（或 NULL）放入返回寄存器。 */
	mov	x0, x3
	ret			/* 返回调用者。 */
SYM_FUNC_END(__pi_strrchr)
SYM_FUNC_ALIAS_WEAK(strrchr, __pi_strrchr)
EXPORT_SYMBOL_NOKASAN(strrchr)
