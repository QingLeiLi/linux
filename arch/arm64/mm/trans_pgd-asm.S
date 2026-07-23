/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * arm64 过渡 EL2 vector stub 学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * kexec/hibernate 切换时原 EL2 vector 可能被覆盖，trans_pgd.c 会把这里
 * 固定 2 KiB 的最小 vector table 复制到安全页。它只接受两个来自 EL1
 * 的 HVC：安装新 VBAR_EL2，或跳入 soft-restart 入口；其他异常槽自旋，
 * 意外同步调用返回 HVC_STUB_ERR。该表不承担正常 KVM/中断服务。
 */

/*
 * Copyright (c) 2021, Microsoft Corporation.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */

#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/kvm_asm.h>

/* invalid_vector 不能使用普通 C 返回路径：意外进入的异常槽原地死循环。 */
.macro invalid_vector	label
SYM_CODE_START_LOCAL(\label)
	.align 7
	b	\label
SYM_CODE_END(\label)
.endm

/*
 * 生成唯一有效的 64-bit EL1 synchronous vector。
 * HVC ABI 输入位于 x0..x4：x0 是操作号；SET_VECTORS 用 x1 作为新 VBAR；
 * SOFT_RESTART 将 x1 入口和 x2/x3/x4 参数重排后 br，成功不返回。
 */
.macro el1_sync_vector
SYM_CODE_START_LOCAL(el1_sync)
	.align 7
	cmp	x0, #HVC_SET_VECTORS	/* Called from hibernate */
	/* hibernate 请求只更新 EL2 向量基址，x0=0 后 eret 回 EL1 表示成功。 */
	b.ne	1f
	msr	vbar_el2, x1
	mov	x0, xzr
	eret
1:	cmp	x0, #HVC_SOFT_RESTART	/* Called from kexec */
	/* kexec 把目标入口放 x1；重排寄存器以匹配 soft restart 汇编入口 ABI。 */
	b.ne	2f
	mov	x0, x2
	mov	x2, x4
	mov	x4, x1
	mov	x1, x3
	br	x4
2:	/* Unexpected argument, set an error */
	/* 未知功能号不改变控制状态，以固定负错误经 x0 返回。 */
	mov_q	x0, HVC_STUB_ERR
	eret
SYM_CODE_END(el1_sync)
.endm

/*
 * ARM vector table 固定 16 个 128-byte 槽：按当前 EL2 SP 模式、来源 EL、
 * AArch64/AArch32 与 sync/IRQ/FIQ/error 排列。仅“来自 64-bit EL1 的同步
 * 异常”安装上述 HVC handler，其余槽全部自旋，暴露过渡阶段不应发生的事件。
 */
SYM_CODE_START(trans_pgd_stub_vectors)
	invalid_vector	hyp_stub_el2t_sync_invalid	// Synchronous EL2t
	invalid_vector	hyp_stub_el2t_irq_invalid	// IRQ EL2t
	invalid_vector	hyp_stub_el2t_fiq_invalid	// FIQ EL2t
	invalid_vector	hyp_stub_el2t_error_invalid	// Error EL2t

	invalid_vector	hyp_stub_el2h_sync_invalid	// Synchronous EL2h
	invalid_vector	hyp_stub_el2h_irq_invalid	// IRQ EL2h
	invalid_vector	hyp_stub_el2h_fiq_invalid	// FIQ EL2h
	invalid_vector	hyp_stub_el2h_error_invalid	// Error EL2h

	el1_sync_vector					// Synchronous 64-bit EL1
	invalid_vector	hyp_stub_el1_irq_invalid	// IRQ 64-bit EL1
	invalid_vector	hyp_stub_el1_fiq_invalid	// FIQ 64-bit EL1
	invalid_vector	hyp_stub_el1_error_invalid	// Error 64-bit EL1

	invalid_vector	hyp_stub_32b_el1_sync_invalid	// Synchronous 32-bit EL1
	invalid_vector	hyp_stub_32b_el1_irq_invalid	// IRQ 32-bit EL1
	invalid_vector	hyp_stub_32b_el1_fiq_invalid	// FIQ 32-bit EL1
	invalid_vector	hyp_stub_32b_el1_error_invalid	// Error 32-bit EL1
	.align 11
SYM_INNER_LABEL(__trans_pgd_stub_vectors_end, SYM_L_LOCAL)
SYM_CODE_END(trans_pgd_stub_vectors)

# Check the trans_pgd_stub_vectors didn't overflow
/*
 * 链接期 .org 技巧强制整个对象恰为 SZ_2K；若向量实现超过 2 KiB，目标
 * 地址向后移动非法并让汇编失败，保证 trans_pgd.c 固定长度 memcpy 安全。
 */
.org . - (__trans_pgd_stub_vectors_end - trans_pgd_stub_vectors) + SZ_2K
