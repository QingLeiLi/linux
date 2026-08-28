/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
 *
 * Copyright (C) 2017-2018 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <linux/linkage.h>

#include <asm/assembler.h>

/*
 * 编译器在目标 ISA 没有一条“128 位移位”指令时调用这些 helper。
 * AAPCS64 把 unsigned/signed __int128 拆为 x1:x0（高 64 位:低 64 位），
 * x2 是移位数，结果仍由 x1:x0 返回。C 语言只会为 0..127 的移位调用
 * 它们；因此这里不定义 >=128 的行为。标签 2 专门处理 64..127。
 */

SYM_FUNC_START(__ashlti3)
	cbz	x2, 1f		/* 移 0 位就是原值，直接返回。 */
	mov	x3, #64		/* 准备计算互补移位量 64-n。 */
	sub	x3, x3, x2	/* x3=64-n；其正负同时区分 n<64 与 n>=64。 */
	cmp	x3, #0		/* n>=64 时低半字完全被移出。 */
	b.le	2f
	lsl	x1, x1, x2	/* 新高半字的一部分：旧 high << n。 */
	lsr	x3, x0, x3	/* 跨 64 位边界的部分：旧 low >> (64-n)。 */
	lsl	x2, x0, x2	/* 新低半字：旧 low << n；借用已无用的 x2。 */
	orr	x1, x1, x3	/* 合并两部分，形成新高半字。 */
	mov	x0, x2		/* 把新低半字放回 ABI 返回寄存器 x0。 */
1:
	ret			/* 返回 x1:x0。 */
2:
	neg	w1, w3		/* w1=n-64；32 位写入会把 x1 高位清零。 */
	mov	x2, #0		/* n>=64 时结果低 64 位恒为 0。 */
	lsl	x1, x0, x1	/* 新高半字=旧低半字 << (n-64)。 */
	mov	x0, x2		/* 返回低半字 0。 */
	ret
SYM_FUNC_END(__ashlti3)
EXPORT_SYMBOL(__ashlti3)

SYM_FUNC_START(__ashrti3)
	cbz	x2, 1f		/* 移 0 位直接返回。 */
	mov	x3, #64
	sub	x3, x3, x2	/* x3=64-n。 */
	cmp	x3, #0
	b.le	2f		/* n>=64 走只依赖旧高半字的路径。 */
	lsr	x0, x0, x2	/* 新低半字的低部：旧 low 逻辑右移。 */
	lsl	x3, x1, x3	/* 跨边界部分：旧 high << (64-n)。 */
	asr	x2, x1, x2	/* 新高半字：旧 high 算术右移，复制符号位。 */
	orr	x0, x0, x3	/* 合成新低半字。 */
	mov	x1, x2		/* 放回新高半字。 */
1:
	ret
2:
	neg	w0, w3		/* w0=n-64。 */
	asr	x2, x1, #63	/* 新 high 全为原 128 位数的符号位。 */
	asr	x0, x1, x0	/* 新 low=旧 high 算术右移 (n-64)。 */
	mov	x1, x2		/* 返回符号扩展后的高半字。 */
	ret
SYM_FUNC_END(__ashrti3)
EXPORT_SYMBOL(__ashrti3)

SYM_FUNC_START(__lshrti3)
	cbz	x2, 1f		/* 移 0 位直接返回。 */
	mov	x3, #64
	sub	x3, x3, x2	/* x3=64-n。 */
	cmp	x3, #0
	b.le	2f		/* n>=64 时低半字只来自旧 high。 */
	lsr	x0, x0, x2	/* 新 low 的低部。 */
	lsl	x3, x1, x3	/* 从旧 high 跨边界进入新 low 的高部。 */
	lsr	x2, x1, x2	/* 无符号右移：新 high 左侧补 0。 */
	orr	x0, x0, x3	/* 合成新 low。 */
	mov	x1, x2		/* 放回新 high。 */
1:
	ret
2:
	neg	w0, w3		/* w0=n-64。 */
	mov	x2, #0		/* 逻辑右移后的新 high 恒为 0。 */
	lsr	x0, x1, x0	/* 新 low=旧 high >> (n-64)。 */
	mov	x1, x2		/* 返回高半字 0。 */
	ret
SYM_FUNC_END(__lshrti3)
EXPORT_SYMBOL(__lshrti3)
