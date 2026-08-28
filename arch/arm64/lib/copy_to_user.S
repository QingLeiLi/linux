/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/linkage.h>

#include <asm/asm-uaccess.h>
#include <asm/assembler.h>
#include <asm/cache.h>

/*
 * Copy to user space from a kernel buffer (alignment handled by the hardware)
 *
 * Parameters:
 *	x0 - to
 *	x1 - from
 *	x2 - n
 * Returns:
 *	x0 - bytes not copied
 *
 * 与 copy_from_user.S 镜像：源在内核，加载不会 fault；目的在用户，所有
 * sttr/user_stp 均带异常修复。两文件包含同一模板，优化路径完全一致。
 */
	.macro ldrb1 reg, ptr, val
	ldrb  \reg, [\ptr], \val
	.endm

	.macro strb1 reg, ptr, val
	/* 用户侧单字节 store；fault 后跳 9998。 */
	user_ldst 9998f, sttrb, \reg, \ptr, \val
	.endm

	.macro ldrh1 reg, ptr, val
	ldrh  \reg, [\ptr], \val
	.endm

	.macro strh1 reg, ptr, val
	user_ldst 9997f, sttrh, \reg, \ptr, \val
	.endm

	.macro ldr1 reg, ptr, val
	ldr \reg, [\ptr], \val
	.endm

	.macro str1 reg, ptr, val
	user_ldst 9997f, sttr, \reg, \ptr, \val
	.endm

	.macro ldp1 reg1, reg2, ptr, val
	ldp \reg1, \reg2, [\ptr], \val
	.endm

	.macro stp1 reg1, reg2, ptr, val
	/* 用户侧成对 store，异常表修复标签为 9997。 */
	user_stp 9997f, \reg1, \reg2, \ptr, \val
	.endm

	.macro cpy1 dst, src, count
	.arch_extension mops
	USER_CPY(9997f, 1, cpyfpwt [\dst]!, [\src]!, \count!)
	USER_CPY(9996f, 1, cpyfmwt [\dst]!, [\src]!, \count!)
	USER_CPY(9996f, 1, cpyfewt [\dst]!, [\src]!, \count!)
	.endm

end	.req	x5
srcin	.req	x15
SYM_FUNC_START(__arch_copy_to_user)
	add	end, x0, x2		/* 保存用户目的范围终点。 */
	mov	srcin, x1		/* 保存内核源起点供最小粒度重试。 */
#include "copy_template.S"
	mov	x0, #0
	ret

	// Exception fixups
9996:	b.cs	9997f
	// Registers are in Option A format
	add	dst, dst, count
9997:	cmp	dst, dstin
	b.ne	9998f
	// Before being absolutely sure we couldn't copy anything, try harder
	ldrb	tmp1w, [srcin]		/* 内核读可靠；先准备一个待重试字节。 */
USER(9998f, sttrb tmp1w, [dst])
	add	dst, dst, #1			/* 仅在用户写成功后推进进度。 */
9998:	sub	x0, end, dst			/* 返回尚未复制的后缀长度。 */
	ret
SYM_FUNC_END(__arch_copy_to_user)
EXPORT_SYMBOL(__arch_copy_to_user)
