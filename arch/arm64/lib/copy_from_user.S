/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/linkage.h>

#include <asm/asm-uaccess.h>
#include <asm/assembler.h>
#include <asm/cache.h>

/*
 * Copy from user space to a kernel buffer (alignment handled by the hardware)
 *
 * Parameters:
 *	x0 - to
 *	x1 - from
 *	x2 - n
 * Returns:
 *	x0 - bytes not copied
 *
 * 本文件规定“从用户读、向内核写”的访存语义，主体算法来自
 * copy_template.S。带 user_* 的加载会登记异常表；内核目的写入无需修复。
 */

	.macro ldrb1 reg, ptr, val
	/* 用户侧 1 字节加载，fault 时跳到 9998 修复路径。 */
	user_ldst 9998f, ldtrb, \reg, \ptr, \val
	.endm

	.macro strb1 reg, ptr, val
	strb \reg, [\ptr], \val
	.endm

	.macro ldrh1 reg, ptr, val
	user_ldst 9997f, ldtrh, \reg, \ptr, \val
	.endm

	.macro strh1 reg, ptr, val
	strh \reg, [\ptr], \val
	.endm

	.macro ldr1 reg, ptr, val
	user_ldst 9997f, ldtr, \reg, \ptr, \val
	.endm

	.macro str1 reg, ptr, val
	str \reg, [\ptr], \val
	.endm

	.macro ldp1 reg1, reg2, ptr, val
	/* 成对用户加载；宏同时生成 fault 地址到 9997 的异常表项。 */
	user_ldp 9997f, \reg1, \reg2, \ptr, \val
	.endm

	.macro stp1 reg1, reg2, ptr, val
	stp \reg1, \reg2, [\ptr], \val
	.endm

	.macro cpy1 dst, src, count
	.arch_extension mops
	USER_CPY(9997f, 0, cpyfprt [\dst]!, [\src]!, \count!)
	USER_CPY(9996f, 0, cpyfmrt [\dst]!, [\src]!, \count!)
	USER_CPY(9996f, 0, cpyfert [\dst]!, [\src]!, \count!)
	.endm

end	.req	x5
srcin	.req	x15
SYM_FUNC_START(__arch_copy_from_user)
	add	end, x0, x2		/* 固定目的终点，用来算未复制数。 */
	mov	srcin, x1		/* 保留原用户源地址，供首字节重试。 */
#include "copy_template.S"
	mov	x0, #0			/* 模板正常落出表示全部完成。 */
	ret

	// Exception fixups
9996:	b.cs	9997f			/* MOPS 状态已是可直接解释的格式。 */
	// Registers are in Option A format
	add	dst, dst, count		/* Option A：由地址和余量还原完成边界。 */
9997:	cmp	dst, dstin
	b.ne	9998f
	// Before being absolutely sure we couldn't copy anything, try harder
USER(9998f, ldtrb tmp1w, [srcin])
	strb	tmp1w, [dst], #1		/* 用最小粒度挽救第一个字节。 */
9998:	sub	x0, end, dst			/* 未复制数=终点-已推进位置。 */
	ret
SYM_FUNC_END(__arch_copy_from_user)
EXPORT_SYMBOL(__arch_copy_from_user)
