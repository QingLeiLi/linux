/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2021 Arm Ltd.
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

/*
 * Find a character in an area of memory.
 *
 * Parameters:
 *	x0 - buf
 *	x1 - c
 *	x2 - n
 * Returns:
 *	x0 - address of first occurrence of 'c' or 0
 *
 * 核心技巧：把目标字节复制到 64 位字的每个字节槽，异或后“目标字节”
 * 就变成 0。表达式 (x-0x01..01) & ~(x|0x7f..7f) 会在含零字节的
 * 槽位产生最高位标记，从而一次检查 8 字节。该位技巧只找候选字，最终
 * 通过位位置算出第一个匹配字节地址。
 */

#define L(label) .L ## label

#define REP8_01 0x0101010101010101
#define REP8_7f 0x7f7f7f7f7f7f7f7f

#define srcin		x0
#define chrin		w1
#define cntin		x2

#define result		x0

#define wordcnt		x3
#define rep01		x4
#define repchr		x5
#define cur_word	x6
#define cur_byte	w6
#define tmp		x7
#define tmp2		x8

	.p2align 4
	nop
SYM_FUNC_START(__pi_memchr)
	and	chrin, chrin, #0xff	/* 截取字符低 8 位。 */
	lsr	wordcnt, cntin, #3	/* 完整 8 字节字的个数 n/8。 */
	cbz	wordcnt, L(byte_loop)	/* 少于 8 字节直接逐字节处理。 */
	mov	rep01, #REP8_01	/* 每字节为 1 的“广播乘数”。 */
	mul	repchr, x1, rep01	/* 例如 0x41 -> 0x4141414141414141。 */
	and	cntin, cntin, #7	/* 保存尾部字节数 n%8。 */
L(word_loop):
	ldr	cur_word, [srcin], #8	/* 读 8 字节，srcin 指向下一字。 */
	sub	wordcnt, wordcnt, #1	/* 消耗一个完整字。 */
	eor	cur_word, cur_word, repchr /* 相等槽变成零字节。 */
	sub	tmp, cur_word, rep01	/* 零槽借位，使该槽最高位变 1。 */
	orr	tmp2, cur_word, #REP8_7f /* 非最高位全部强制为 1。 */
	bics	tmp, tmp, tmp2		/* tmp=(x-1)&~(x|0x7f)，并设置 NZCV。 */
	b.ne	L(found_word)		/* 非零说明这 8 字节里有匹配。 */
	cbnz	wordcnt, L(word_loop)	/* 还有完整字则继续。 */
L(byte_loop):
	cbz	cntin, L(not_found)	/* 尾部耗尽：整个范围都未命中。 */
	ldrb	cur_byte, [srcin], #1	/* 读取一个尾部字节。 */
	sub	cntin, cntin, #1	/* 剩余长度减一。 */
	cmp	cur_byte, chrin		/* 比较目标字符。 */
	b.ne	L(byte_loop)
	sub	srcin, srcin, #1	/* 撤销后递增，得到命中地址。 */
	ret
L(found_word):
	/* 小端时先翻转字节，使“地址最小的命中”落在最高有效方向。 */
CPU_LE(	rev	tmp, tmp)
	clz	tmp, tmp			/* 数最高标记位之前的 0。 */
	sub	tmp, tmp, #64		/* 因 srcin 已经 +8，换成相对它的负位偏移。 */
	add	result, srcin, tmp, asr #3 /* 位偏移/8 得字节偏移并形成地址。 */
	ret
L(not_found):
	mov	result, #0			/* C API 约定未找到返回 NULL。 */
	ret
SYM_FUNC_END(__pi_memchr)
SYM_FUNC_ALIAS_WEAK(memchr, __pi_memchr)
EXPORT_SYMBOL_NOKASAN(memchr)
