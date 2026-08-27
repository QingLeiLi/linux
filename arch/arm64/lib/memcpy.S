/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2012-2021, Arm Limited.
 *
 * Adapted from the original at:
 * https://github.com/ARM-software/optimized-routines/blob/afd6244a1f8d9229/string/aarch64/memcpy.S
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

/* Assumptions:
 *
 * ARMv8-a, AArch64, unaligned accesses.
 *
 */

#define L(label) .L ## label

#define dstin	x0
#define src	x1
#define count	x2
#define dst	x3
#define srcend	x4
#define dstend	x5
#define A_l	x6
#define A_lw	w6
#define A_h	x7
#define B_l	x8
#define B_lw	w8
#define B_h	x9
#define C_l	x10
#define C_lw	w10
#define C_h	x11
#define D_l	x12
#define D_h	x13
#define E_l	x14
#define E_h	x15
#define F_l	x16
#define F_h	x17
#define G_l	count
#define G_h	dst
#define H_l	src
#define H_h	srcend
#define tmp1	x14

/* This implementation handles overlaps and supports both memcpy and memmove
   from a single entry point.  It uses unaligned accesses and branchless
   sequences to keep the code small, simple and improve performance.

   Copies are split into 3 main cases: small copies of up to 32 bytes, medium
   copies of up to 128 bytes, and large copies.  The overhead of the overlap
   check is negligible since it is only required for large copies.

   Large copies use a software pipelined loop processing 64 bytes per iteration.
   The destination pointer is 16-byte aligned to minimize unaligned accesses.
   The loop tail is handled by always copying 64 bytes from the end.
*/

/*
 * 中文阅读地图：
 * - 0..32 字节：从首尾重叠取数，用长度的 bit3/bit2 选择 8/4 字节路径；
 * - 33..128 字节：固定读取首尾若干 16 字节块，覆盖整个范围；
 * - >128 字节：先用 dst-src 与 count 判断是否危险重叠。安全时向高地址复制，
 *   dst 落在源区内部时向低地址复制，这正是 memmove 保证不覆盖未读源数据的关键；
 * - 大循环每轮 64 字节，load 与 store 交错；最后固定从真正首/尾再取 64 字节，
 *   所以 count 的预减偏差不会形成复杂的零碎尾循环。
 *
 * 地址形如 [srcend,-16] 表示“最后 16 字节”。首块和尾块可以重叠，但都在
 * [src,src+count) 内。函数始终保留 dstin=x0，符合 memcpy 返回原目的指针。
 */

SYM_FUNC_START_LOCAL(__pi_memcpy_generic)
	add	srcend, src, count	/* 源的排他终点。 */
	add	dstend, dstin, count	/* 目的排他终点。 */
	cmp	count, 128
	b.hi	L(copy_long)
	cmp	count, 32
	b.hi	L(copy32_128)

	/* Small copies: 0..32 bytes.  */
	cmp	count, 16		/* 16..32：首尾各搬 16 字节。 */
	b.lo	L(copy16)
	ldp	A_l, A_h, [src]
	ldp	D_l, D_h, [srcend, -16]
	stp	A_l, A_h, [dstin]
	stp	D_l, D_h, [dstend, -16]
	ret

	/* Copy 8-15 bytes.  */
L(copy16):
	tbz	count, 3, L(copy8)	/* bit3=1 表示长度至少 8。 */
	ldr	A_l, [src]
	ldr	A_h, [srcend, -8]
	str	A_l, [dstin]
	str	A_h, [dstend, -8]
	ret

	.p2align 3
	/* Copy 4-7 bytes.  */
L(copy8):
	tbz	count, 2, L(copy4)	/* bit2=1 表示长度至少 4。 */
	ldr	A_lw, [src]
	ldr	B_lw, [srcend, -4]
	str	A_lw, [dstin]
	str	B_lw, [dstend, -4]
	ret

	/* Copy 0..3 bytes using a branchless sequence.  */
L(copy4):
	cbz	count, L(copy0)
	lsr	tmp1, count, 1		/* 中间索引 floor(count/2)。 */
	ldrb	A_lw, [src]
	ldrb	C_lw, [srcend, -1]
	ldrb	B_lw, [src, tmp1]
	strb	A_lw, [dstin]
	strb	B_lw, [dstin, tmp1]
	strb	C_lw, [dstend, -1]
L(copy0):
	ret

	.p2align 4
	/* Medium copies: 33..128 bytes.  */
L(copy32_128):
	ldp	A_l, A_h, [src]
	ldp	B_l, B_h, [src, 16]
	ldp	C_l, C_h, [srcend, -32]
	ldp	D_l, D_h, [srcend, -16]
	cmp	count, 64
	b.hi	L(copy128)
	stp	A_l, A_h, [dstin]
	stp	B_l, B_h, [dstin, 16]
	stp	C_l, C_h, [dstend, -32]
	stp	D_l, D_h, [dstend, -16]
	ret

	.p2align 4
	/* Copy 65..128 bytes.  */
L(copy128):
	ldp	E_l, E_h, [src, 32]
	ldp	F_l, F_h, [src, 48]
	cmp	count, 96
	b.ls	L(copy96)
	ldp	G_l, G_h, [srcend, -64]
	ldp	H_l, H_h, [srcend, -48]
	stp	G_l, G_h, [dstend, -64]
	stp	H_l, H_h, [dstend, -48]
L(copy96):
	stp	A_l, A_h, [dstin]
	stp	B_l, B_h, [dstin, 16]
	stp	E_l, E_h, [dstin, 32]
	stp	F_l, F_h, [dstin, 48]
	stp	C_l, C_h, [dstend, -32]
	stp	D_l, D_h, [dstend, -16]
	ret

	.p2align 4
	/* Copy more than 128 bytes.  */
L(copy_long):
	/* Use backwards copy if there is an overlap.  */
	sub	tmp1, dstin, src	/* 正向距离；无符号比较同时处理地址回绕方向。 */
	cbz	tmp1, L(copy0)
	cmp	tmp1, count		/* dst 落入 [src,src+count) 才必须反向。 */
	b.lo	L(copy_long_backwards)

	/* Copy 16 bytes and then align dst to 16-byte alignment.  */

	ldp	D_l, D_h, [src]
	and	tmp1, dstin, 15
	bic	dst, dstin, 15		/* dst 向下对齐；后续用正偏移 16 开始写。 */
	sub	src, src, tmp1
	add	count, count, tmp1	/* Count is now 16 too large.  */
	ldp	A_l, A_h, [src, 16]
	stp	D_l, D_h, [dstin]
	ldp	B_l, B_h, [src, 32]
	ldp	C_l, C_h, [src, 48]
	ldp	D_l, D_h, [src, 64]!
	subs	count, count, 128 + 16	/* Test and readjust count.  */
	b.ls	L(copy64_from_end)

L(loop64):
	stp	A_l, A_h, [dst, 16]
	ldp	A_l, A_h, [src, 16]
	stp	B_l, B_h, [dst, 32]
	ldp	B_l, B_h, [src, 32]
	stp	C_l, C_h, [dst, 48]
	ldp	C_l, C_h, [src, 48]
	stp	D_l, D_h, [dst, 64]!
	ldp	D_l, D_h, [src, 64]!
	subs	count, count, 64	/* 一轮提交 64B，条件码决定是否继续。 */
	b.hi	L(loop64)

	/* Write the last iteration and copy 64 bytes from the end.  */
L(copy64_from_end):
	ldp	E_l, E_h, [srcend, -64]
	stp	A_l, A_h, [dst, 16]
	ldp	A_l, A_h, [srcend, -48]
	stp	B_l, B_h, [dst, 32]
	ldp	B_l, B_h, [srcend, -32]
	stp	C_l, C_h, [dst, 48]
	ldp	C_l, C_h, [srcend, -16]
	stp	D_l, D_h, [dst, 64]
	stp	E_l, E_h, [dstend, -64]
	stp	A_l, A_h, [dstend, -48]
	stp	B_l, B_h, [dstend, -32]
	stp	C_l, C_h, [dstend, -16]
	ret

	.p2align 4

	/* Large backwards copy for overlapping copies.
	   Copy 16 bytes and then align dst to 16-byte alignment.  */
L(copy_long_backwards):
	ldp	D_l, D_h, [srcend, -16]
	and	tmp1, dstend, 15
	sub	srcend, srcend, tmp1
	sub	count, count, tmp1
	ldp	A_l, A_h, [srcend, -16]
	stp	D_l, D_h, [dstend, -16]
	ldp	B_l, B_h, [srcend, -32]
	ldp	C_l, C_h, [srcend, -48]
	ldp	D_l, D_h, [srcend, -64]!
	sub	dstend, dstend, tmp1
	subs	count, count, 128
	b.ls	L(copy64_from_start)

L(loop64_backwards):
	stp	A_l, A_h, [dstend, -16]
	ldp	A_l, A_h, [srcend, -16]
	stp	B_l, B_h, [dstend, -32]
	ldp	B_l, B_h, [srcend, -32]
	stp	C_l, C_h, [dstend, -48]
	ldp	C_l, C_h, [srcend, -48]
	stp	D_l, D_h, [dstend, -64]!
	ldp	D_l, D_h, [srcend, -64]!
	subs	count, count, 64
	b.hi	L(loop64_backwards)

	/* Write the last iteration and copy 64 bytes from the start.  */
L(copy64_from_start):
	ldp	G_l, G_h, [src, 48]
	stp	A_l, A_h, [dstend, -16]
	ldp	A_l, A_h, [src, 32]
	stp	B_l, B_h, [dstend, -32]
	ldp	B_l, B_h, [src, 16]
	stp	C_l, C_h, [dstend, -48]
	ldp	C_l, C_h, [src]
	stp	D_l, D_h, [dstend, -64]
	stp	G_l, G_h, [dstin, 48]
	stp	A_l, A_h, [dstin, 32]
	stp	B_l, B_h, [dstin, 16]
	stp	C_l, C_h, [dstin]
	ret
SYM_FUNC_END(__pi_memcpy_generic)

#ifdef CONFIG_AS_HAS_MOPS
	.arch_extension mops
SYM_FUNC_START(__pi_memcpy)
alternative_if_not ARM64_HAS_MOPS
	b	__pi_memcpy_generic
alternative_else_nop_endif

	mov	dst, dstin
	cpyp	[dst]!, [src]!, count!
	cpym	[dst]!, [src]!, count!
	cpye	[dst]!, [src]!, count!
	ret
SYM_FUNC_END(__pi_memcpy)
#else
SYM_FUNC_ALIAS(__pi_memcpy, __pi_memcpy_generic)
#endif

SYM_FUNC_ALIAS(__memcpy, __pi_memcpy)
EXPORT_SYMBOL(__memcpy)
SYM_FUNC_ALIAS_WEAK(memcpy, __memcpy)
EXPORT_SYMBOL(memcpy)

SYM_FUNC_ALIAS(__pi_memmove, __pi_memcpy)

SYM_FUNC_ALIAS(__memmove, __pi_memmove)
EXPORT_SYMBOL(__memmove)
SYM_FUNC_ALIAS_WEAK(memmove, __memmove)
EXPORT_SYMBOL(memmove)
