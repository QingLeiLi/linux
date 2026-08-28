/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 Google LLC
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

/*
 * Report a tag mismatch detected by tag-based KASAN.
 *
 * A compiler-generated thunk calls this with a non-AAPCS calling
 * convention. Upon entry to this function, registers are as follows:
 *
 * x0:         fault address (see below for restore)
 * x1:         fault description (see below for restore)
 * x2 to x15:  callee-saved
 * x16 to x17: safe to clobber
 * x18 to x30: callee-saved
 * sp:         pre-decremented by 256 bytes (see below for restore)
 *
 * The caller has decremented the SP by 256 bytes, and created a
 * structure on the stack as follows:
 *
 * sp + 0..15:    x0 and x1 to be restored
 * sp + 16..231:  free for use
 * sp + 232..247: x29 and x30 (same as in GPRs)
 * sp + 248..255: free for use
 *
 * Note that this is not a struct pt_regs.
 *
 * To call a regular AAPCS function we must save x2 to x15 (which we can
 * store in the gaps), and create a frame record (for which we can use
 * x29 and x30 spilled by the caller as those match the GPRs).
 *
 * The caller expects x0 and x1 to be restored from the structure, and
 * for the structure to be removed from the stack (i.e. the SP must be
 * incremented by 256 prior to return).
 */
SYM_CODE_START(__hwasan_tag_mismatch)
	bti	c			/* BTI：只允许以函数调用类型的间接分支进入。 */
	add	x29, sp, #232		/* 建立指向调用者已保存 x29/x30 的帧记录。 */
	stp	x2, x3, [sp, #8 * 2]	/* 保存非标准约定要求保持的 x2、x3。 */
	stp	x4, x5, [sp, #8 * 4]
	stp	x6, x7, [sp, #8 * 6]
	stp	x8, x9, [sp, #8 * 8]
	stp	x10, x11, [sp, #8 * 10]
	stp	x12, x13, [sp, #8 * 12]
	stp	x14, x15, [sp, #8 * 14]	/* 至此 x2..x15 全部入栈。 */
#ifndef CONFIG_SHADOW_CALL_STACK
	str	x18, [sp, #8 * 18]	/* 无影子调用栈时，普通平台寄存器也需保存。 */
#endif

	mov	x2, x30		/* 常规 C 函数第 3 参数：发生问题处的返回 PC。 */
	bl	kasan_tag_mismatch	/* x0=地址、x1=描述，调用真正的报告函数。 */

	ldp	x0, x1, [sp]		/* 按 thunk 约定恢复最初的 x0、x1。 */
	ldp	x2, x3, [sp, #8 * 2]
	ldp	x4, x5, [sp, #8 * 4]
	ldp	x6, x7, [sp, #8 * 6]
	ldp	x8, x9, [sp, #8 * 8]
	ldp	x10, x11, [sp, #8 * 10]
	ldp	x12, x13, [sp, #8 * 12]
	ldp	x14, x15, [sp, #8 * 14]
#ifndef CONFIG_SHADOW_CALL_STACK
	ldr	x18, [sp, #8 * 18]
#endif
	ldp	x29, x30, [sp, #8 * 29]	/* 恢复帧指针和原返回地址。 */

	/* remove the structure from the stack */
	add	sp, sp, #256		/* 释放调用者预留的整块非标准保存区。 */
	ret				/* 返回编译器生成的检查 thunk。 */
SYM_CODE_END(__hwasan_tag_mismatch)
EXPORT_SYMBOL(__hwasan_tag_mismatch)
