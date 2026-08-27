/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2021 Arm Ltd.
 */

#include <linux/linkage.h>
#include <asm/asm-uaccess.h>

	.text

/* Prototype: int __arch_clear_user(void *addr, size_t sz)
 * Purpose  : clear some user memory
 * Params   : addr - user memory address to clear
 *          : sz   - number of bytes to clear
 * Returns  : number of bytes NOT cleared
 *
 * Alignment fixed up by hardware.
 *
 * 与 memset 不同，用户页可能在任意一次 sttr 时 fault。USER(fixup, insn)
 * 把该指令与修复标签登记到异常表；修复路径用原终点 x2 减当前进度 x0，
 * 返回尚未清零的字节数。正常成功时返回 0。
 */

SYM_FUNC_START(__arch_clear_user)
	add	x2, x0, x1		/* x2=end=addr+sz，fault 后仍需它算余量。 */

#ifdef CONFIG_AS_HAS_MOPS
	.arch_extension mops
alternative_if_not ARM64_HAS_MOPS
	b	.Lno_mops
alternative_else_nop_endif

USER(9f, setpt	[x0]!, x1!, xzr)
USER(6f, setmt	[x0]!, x1!, xzr)
USER(6f, setet	[x0]!, x1!, xzr)
	mov	x0, #0		/* MOPS 三阶段都成功：未完成数为 0。 */
	ret
.Lno_mops:
#endif

	subs	x1, x1, #8		/* 预减 8；负数说明总长不足 8。 */
	b.mi	2f			/* 0..7 字节走按位拆分尾部。 */

1:	.p2align 4
USER(9f, sttr	xzr, [x0])	/* 以非特权权限写 8 个零；fault -> 9。 */
	add	x0, x0, #8		/* 只有成功后才推进进度指针。 */
	subs	x1, x1, #8		/* 再为下一整块预减 8。 */
	b.hi	1b			/* 无符号 >0：至少还有完整中间块。 */
USER(9f, sttr	xzr, [x2, #-8]) /* 始终从 end-8 写尾块，允许与前块重叠。 */
	mov	x0, #0
	ret

2:	/* 因为此前 x1-=8，低 3 位仍等于原长度的低 3 位。 */
	tbz	x1, #2, 3f		/* 长度没有 4 字节分量则跳过。 */
USER(9f, sttr	wzr, [x0])
USER(8f, sttr	wzr, [x2, #-4])
	mov	x0, #0
	ret

3:	tbz	x1, #1, 4f		/* 测试 2 字节分量。 */
USER(9f, sttrh	wzr, [x0])
4:	tbz	x1, #0, 5f		/* 测试最后 1 字节分量。 */
USER(7f, sttrb	wzr, [x2, #-1])
5:	mov	x0, #0
	ret

	// Exception fixups
6:	b.cs	9f			/* MOPS fault 后若进度格式已可直接使用。 */
	// Registers are in Option A format
	add	x0, x0, x1		/* Option A：地址加剩余计数还原已完成边界。 */
	b	9f
7:	sub	x0, x2, #5	/* 最终单字节 fault：先定位到对应进度。 */
8:	add	x0, x0, #4	/* 第二个 4 字节写 fault：补偿第一个已写的 4 字节。 */
9:	sub	x0, x2, x0	/* end-current = 未清零字节数。 */
	ret
SYM_FUNC_END(__arch_clear_user)
EXPORT_SYMBOL(__arch_clear_user)
