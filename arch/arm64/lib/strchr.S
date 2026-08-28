/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/lib/strchr.S
 *
 * Copyright (C) 1995-2000 Russell King
 * Copyright (C) 2013 ARM Ltd.
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

/*
 * Find the first occurrence of a character in a string.
 *
 * Parameters:
 *	x0 - str
 *	x1 - c
 * Returns:
 *	x0 - address of first occurrence of 'c' or 0
 */
SYM_FUNC_START(__pi_strchr)
	/* C 的 int 参数可能高位有值；strchr 只比较其低 8 位字符值。 */
	and	w1, w1, #0xff
1:	/* 取一个无符号字节，并让 x0 提前指向下一字符。 */
	ldrb	w2, [x0], #1
	/* 先判断当前字符是否就是目标字符。 */
	cmp	w2, w1
	/*
	 * 仅在“不相等”(ne)时再比较当前字符和 NUL。若刚才相等，ccmp
	 * 直接写入 NZCV=#4（Z=1）；于是下面只有“既非目标也非 NUL”才循环。
	 */
	ccmp	w2, wzr, #4, ne
	b.ne	1b
	/* 后递增已越过当前字符，退回一字节得到候选返回地址。 */
	sub	x0, x0, #1
	/* 到这里可能因“命中目标”或“遇到 NUL”退出，重新区分二者。 */
	cmp	w2, w1
	/* 命中返回候选地址；只遇到 NUL 则返回 NULL（xzr 恒为 0）。 */
	csel	x0, x0, xzr, eq
	/* x0 是 AAPCS64 的返回值寄存器。 */
	ret
SYM_FUNC_END(__pi_strchr)

SYM_FUNC_ALIAS_WEAK(strchr, __pi_strchr)
EXPORT_SYMBOL_NOKASAN(strchr)
