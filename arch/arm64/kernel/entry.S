/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arm64 异常入口、返回与底层任务切换学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件处在硬件异常向量与 C 处理器之间：向量槽先验证栈并分配 pt_regs，
 * kernel_entry 保存被打断现场、恢复 current 和内核安全状态，具体 handler 处理
 * 异常，kernel_exit 再按 EL0/EL1 来源恢复寄存器并 ERET。后半还实现 KPTI 跳板、
 * Spectre/BHB 防护、cpu_switch_to、IRQ 栈和 SDEI 固件事件入口。
 *
 * 关键不变量：每个向量槽严格限制为 128 字节；异常入口在任意 C 代码前形成完整
 * pt_regs；来自 EL0 时 SP_EL0 不可信，必须从每 CPU __entry_task 恢复 current；
 * 安全缓解、PAC/MTE/GCS/SCS 和页表切换必须遵守既定屏障顺序。汇编宏的“参数”
 * 多为寄存器或生成期常量，调用者必须遵守 clobber 契约，不存在普通 C ABI 保护。
 *
 * 汇编阅读约定：
 *   x0-x30 是 64 位通用寄存器，w0-w30 是同一寄存器的低 32 位；写 wN 会把 xN
 *   的高 32 位清零。x30 通常作为链接寄存器 LR 保存返回地址，x29 通常作为帧
 *   指针 FP；sp 是当前栈顶，xzr/wzr 读取恒为 0、写入结果被丢弃。
 *   mrs/msr 在通用寄存器与系统寄存器之间搬运状态；ldr/str 读写一个值，
 *   ldp/stp 成对读写两个值。AArch64 栈向低地址增长，所以“sub sp”是在栈上
 *   分配空间，“add sp”是在释放空间。
 *   b 是无条件跳转，bl 会同时把下一条指令地址写入 LR，ret 跳到 LR，eret 则按
 *   ELR_EL1/SPSR_EL1 同时恢复异常返回地址与处理器状态。cbz/cbnz 按寄存器是否
 *   为零分支，tbz/tbnz 只测试一位，b.eq/b.ne 使用前一比较产生的条件码。
 *   形如 1:/1f/1b 的数字标签可重复：1f 指向下一个 1:，1b 指向上一个 1:；
 *   .Lxxx 是仅供本汇编单元使用的局部标签。宏参数以反斜杠引用，\@ 为每次宏
 *   展开生成唯一编号，避免多个向量槽的内部标签重名。
 *   .if/.rept/.irp/.macro 是“汇编时”生成指令的控制结构，并非 CPU 运行时分支；
 *   #ifdef 决定某配置是否编进内核，alternative_* 则预留等长指令区域，启动时按
 *   CPU 能力改写，因此阅读时要同时考虑未替换与替换后的两种指令序列。
 *
 * 异常现场布局：kernel_ventry 先令 sp -= PT_REGS_SIZE，随后 kernel_entry 按
 * struct pt_regs 的偏移保存 x0-x30、原 SP、ELR_EL1 和 SPSR_EL1。C handler 收到
 * 的 x0 实际是这个 pt_regs 的地址；返回路径反向装载同一布局。S_*、TSK_*、
 * THREAD_* 常量由 asm-offsets 从 C 结构体生成，保证汇编偏移与 C 布局一致。
 *
 * 从业务事件看，可以先把本文件理解为四条主干：用户系统调用、缺页或非法指令
 * 走 EL0 同步异常；定时器、设备中断和核间中断走 IRQ；内核访问异常、BUG/调试
 * 事件走 EL1 同步异常；平台异步硬件错误可走 SError，固件委托的高优先级事件则
 * 走文件末尾的 SDEI 入口。这里负责把这些来源不同的事件统一封装成 pt_regs，
 * 真正决定“调用系统调用、处理缺页、发信号还是分派中断”的策略主要位于 C 层。
 * pt_regs 不是单独分配或引用计数的对象，而是当前异常栈帧的一部分；一次嵌套异常
 * 会在当时生效的任务栈、IRQ 栈或 overflow 栈上再形成一份独立快照。
 */
/*
 * Low-level exception handling code
 *
 * Copyright (C) 2012 ARM Ltd.
 * Authors:	Catalin Marinas <catalin.marinas@arm.com>
 *		Will Deacon <will.deacon@arm.com>
 */
/* 低层异常处理代码；保留上游作者与版权信息。 */

#include <linux/arm-smccc.h>
#include <linux/init.h>
#include <linux/linkage.h>

#include <asm/alternative.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include <asm/asm_pointer_auth.h>
#include <asm/bug.h>
#include <asm/cpufeature.h>
#include <asm/errno.h>
#include <asm/esr.h>
#include <asm/irq.h>
#include <asm/memory.h>
#include <asm/mmu.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/scs.h>
#include <asm/stacktrace/frame.h>
#include <asm/thread_info.h>
#include <asm/asm-uaccess.h>
#include <asm/unistd.h>

	/* 清零 x0-x29；用于 EL0 入口消除用户寄存器中的推测执行秘密，LR 另行处理。 */
	.macro	clear_gp_regs
	/*
	 * .irp 会在汇编阶段让 n 依次取 0..29，并重复展开 mov x\n, xzr；最终机器码
	 * 是 30 条普通 mov，不存在运行时循环。x30 未清零，因为入口跳板可能借它
	 * 传递返回目标，且 kernel_entry 会单独保存 LR。
	 */
	.irp	n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29
	mov	x\n, xzr
	.endr
	.endm

	.macro kernel_ventry, el:req, ht:req, regsize:req, label:req
	/*
	 * 生成一个 128 字节向量槽。el/ht/regsize/label 在汇编期决定来源异常级、栈型、
	 * 寄存器宽度和处理类别。运行时先预留 pt_regs，并以不损坏通用寄存器的算术
	 * 检测任务/IRQ 栈溢出；正常分派到对应入口，溢出则切到每 CPU overflow 栈。
	 */
	.align 7
	/* 2^7=128 字节对齐；硬件用固定槽号进入，不能让某个槽跨过规定边界。 */
.Lventry_start\@:
	.if	\el == 0
	/*
	 * This must be the first instruction of the EL0 vector entries. It is
	 * skipped by the trampoline vectors, to trigger the cleanup.
	 */
	/* EL0 正式向量的首条指令必须是该分支；跳板向量跳过它，以触发残留状态清理。 */
	b	.Lskip_tramp_vectors_cleanup\@
	.if	\regsize == 64
	/*
	 * 64 位 KPTI 跳板曾把原 x30 暂存在 TPIDRRO_EL0，正式入口将其取回并清空；
	 * 非跳板入口先执行上面的 b，跳过这两条指令。32 位入口无需恢复用户 x30，
	 * 直接把它清零。这里的 .if 在汇编时选择代码，不是运行时检查 regsize。
	 */
	mrs	x30, tpidrro_el0
	msr	tpidrro_el0, xzr
	.else
	mov	x30, xzr
	.endif
.Lskip_tramp_vectors_cleanup\@:
	.endif

	/* 阶段 1：在当前任务栈或 IRQ 栈上预留一整个 struct pt_regs。 */
	sub	sp, sp, #PT_REGS_SIZE
	/*
	 * Test whether the SP has overflowed, without corrupting a GPR.
	 * Task and IRQ stacks are aligned so that SP & (1 << THREAD_SHIFT)
	 * should always be zero.
	 */
	/*
	 * 借 x0 与 SP 做可逆运算来检测栈越界，不先破坏任何 GPR。任务栈和 IRQ 栈按
	 * THREAD_SIZE 对齐，合法 SP 的 THREAD_SHIFT 位应为 0；正常路径完整还原 SP/x0。
	 */
	// 下列四步借 x0 做可逆 SP 运算：先构造 sp'，正常分支再精确恢复原 SP 与 x0。
	add	sp, sp, x0			// sp' = sp + x0
	sub	x0, sp, x0			// x0' = sp' - x0 = (sp + x0) - x0 = sp
	tbnz	x0, #THREAD_SHIFT, 0f
	/* tbnz 仅检查代表 THREAD_SIZE 边界的那一位；置位就跳到后面的 0: 溢出路径。 */
	sub	x0, sp, x0			// x0'' = sp' - x0' = (sp + x0) - sp = x0
	sub	sp, sp, x0			// sp'' = sp' - x0 = (sp + x0) - x0 = sp
	/*
	 * 宏拼接形成真实符号，例如 el0t_64_sync；\() 只是终止参数名，避免汇编器把
	 * 后续下划线当成参数名的一部分。正常路径跳到 entry_handler 生成的包装函数。
	 */
	b	el\el\ht\()_\regsize\()_\label

0:
	/*
	 * Either we've just detected an overflow, or we've taken an exception
	 * while on the overflow stack. Either way, we won't return to
	 * userspace, and can clobber EL0 registers to free up GPRs.
	 */
	/*
	 * 此处要么刚发现溢出，要么已在 overflow 栈上再次异常；两者都不会返回用户态，
	 * 因而可占用 EL0 寄存器保存原 SP/x0 并完成诊断。
	 */

	/* Stash the original SP (minus PT_REGS_SIZE) in tpidr_el0. */
	/* 暂存原 SP（已减 PT_REGS_SIZE）到此时可安全复用的 TPIDR_EL0。 */
	msr	tpidr_el0, x0

	/* Recover the original x0 value and stash it in tpidrro_el0 */
	/* 逆运算恢复原 x0，并暂存在 TPIDRRO_EL0。 */
	sub	x0, sp, x0
	msr	tpidrro_el0, x0

	/* Switch to the overflow stack */
	/* 切换到当前 CPU 的 overflow 栈顶，后续不再依赖已损坏的任务栈。 */
	adr_this_cpu sp, overflow_stack + OVERFLOW_STACK_SIZE, x0
	/* adr_this_cpu 计算“本 CPU”对象地址；加 SIZE 得到向下增长栈的初始栈顶。 */

	/*
	 * Check whether we were already on the overflow stack. This may happen
	 * after panic() re-enables interrupts.
	 */
	/* panic() 重新开中断后可能在 overflow 栈上再入；通过地址范围区分再入与坏 SP。 */
	// x0 依次表示被打断 SP、它到 overflow 栈顶的差值，以及范围检查结果。
	mrs	x0, tpidr_el0			// sp of interrupted context
	sub	x0, sp, x0			// delta with top of overflow stack
	tst	x0, #~(OVERFLOW_STACK_SIZE - 1)	// within range?
	/* 对齐的 overflow 栈内偏移只可能占低位；高位非零说明原 SP 不属于该栈。 */
	b.ne	__bad_stack			// no? -> bad stack pointer

	/* We were already on the overflow stack. Restore sp/x0 and carry on. */
	/* 若确为 overflow 栈再入，恢复被打断时的 SP/x0 并继续进入对应异常处理。 */
	sub	sp, sp, x0
	mrs	x0, tpidrro_el0
	b	el\el\ht\()_\regsize\()_\label
	// .org 同时把槽填满到 128 字节，并让汇编器在代码超出槽容量时报告错误。
.org .Lventry_start\@ + 128	// Did we overflow the ventry slot?
	.endm

	.macro	tramp_alias, dst, sym
	/* 在 KPTI 跳板虚拟区中计算符号别名，用 movz/movk 构造完整地址到 dst。 */
	.set	.Lalias\@, TRAMP_VALIAS + \sym - .entry.tramp.text
	/*
	 * .set 只在汇编/链接阶段定义常量，不生成指令。movz 写入一个 16 位分片并把
	 * 其余位清零，两个 movk 再保留已有位、补入其余分片；三条指令共同构造
	 * 48 位规范虚拟地址，:abs_g*_... 重定位由链接器填入对应 16 位。
	 */
	movz	\dst, :abs_g2_s:.Lalias\@
	movk	\dst, :abs_g1_nc:.Lalias\@
	movk	\dst, :abs_g0_nc:.Lalias\@
	.endm

	/*
	 * This macro corrupts x0-x3. It is the caller's duty  to save/restore
	 * them if required.
	 */
	/* apply_ssbd 会破坏 x0-x3；调用者若仍需这些值，必须自行预先保存并事后恢复。 */
	.macro	apply_ssbd, state, tmp1, tmp2
	/*
	 * 按 current 的 TIF_SSBD 和启动期 alternative 结果，通过 SMCCC 固件开关
	 * Spectre-v4 缓解。state 是目标状态，tmp1/tmp2 是显式临时寄存器。
	 */
alternative_cb	ARM64_ALWAYS_SYSTEM, spectre_v4_patch_fw_mitigation_enable
	// 启动期 alternative 会把该分支改为 NOP，以决定是否继续调用固件。
	b	.L__asm_ssbd_skip\@		// Patched to NOP
alternative_cb_end
	/*
	 * ldr_this_cpu 从每 CPU 变量读取“本 CPU 是否仍需固件回调”；tmp1 用于地址
	 * 计算，tmp2 接收布尔值。为 0 时 cbz 直接跳到唯一退出标签，不调用固件。
	 */
	ldr_this_cpu	\tmp2, arm64_ssbd_callback_required, \tmp1
	cbz	\tmp2,	.L__asm_ssbd_skip\@
	ldr	\tmp2, [tsk, #TSK_TI_FLAGS]
	/* TIF_SSBD 已置位表示任务策略不要求这次固件切换，因此按单 bit 测试退出。 */
	tbnz	\tmp2, #TIF_SSBD, .L__asm_ssbd_skip\@
	mov	w0, #ARM_SMCCC_ARCH_WORKAROUND_2
	mov	w1, #\state
	/* SMCCC 约定以 w0 放功能号、w1 放 enable/disable 参数；固件调用会破坏 x0-x3。 */
alternative_cb	ARM64_ALWAYS_SYSTEM, smccc_patch_fw_mitigation_conduit
	// alternative 按固件 conduit 把占位 NOP 改为 SMC #0 或 HVC #0。
	nop					// Patched to SMC/HVC #0
alternative_cb_end
.L__asm_ssbd_skip\@:
	.endm

	/* Check for MTE asynchronous tag check faults */
	/* 检查用户 TTBR0 访问产生的 MTE 异步 tag fault，并以原子置位记录在线程标志中。 */
	.macro check_mte_async_tcf, tmp, ti_flags, thread_sctlr
#ifdef CONFIG_ARM64_MTE
	.arch_extension lse
alternative_if_not ARM64_MTE
	/* 不具备 MTE 的 CPU 会把此处保留为直接跳到 1:，整个检查成为空操作。 */
	b	1f
alternative_else_nop_endif
	/*
	 * Asynchronous tag check faults are only possible in ASYNC (2) or
	 * ASYM (3) modes. In each of these modes bit 1 of SCTLR_EL1.TCF0 is
	 * set, so skip the check if it is unset.
	 */
	/* 仅 ASYNC/ASYM 模式可能产生异步故障，二者 TCF0 bit1 均为 1；否则跳过寄存器读。 */
	tbz	\thread_sctlr, #(SCTLR_EL1_TCF0_SHIFT + 1), 1f
	mrs_s	\tmp, SYS_TFSRE0_EL1
	/* mrs_s 是可访问编码形式系统寄存器的封装；读出的 TF0 位描述 EL0 地址空间。 */
	tbz	\tmp, #SYS_TFSR_EL1_TF0_SHIFT, 1f
	/* Asynchronous TCF occurred for TTBR0 access, set the TI flag */
	/* TFSRE0_EL1.TF0 表明 TTBR0 异步检查失败，原子设置任务待处理标志。 */
	mov	\tmp, #_TIF_MTE_ASYNC_FAULT
	add	\ti_flags, tsk, #TSK_TI_FLAGS
	stset	\tmp, [\ti_flags]
	/*
	 * stset 是 LSE 原子“读取并按位 OR 后写回”；即使异步异常记录与其他线程标志
	 * 更新并发，也不会因普通 load/store 覆盖而丢失别的 bit。这里无需使用旧值。
	 */
1:
#endif
	.endm

	/* Clear the MTE asynchronous tag check faults */
	/* 返回用户态前清除已经采集的 MTE 异步故障状态。 */
	.macro clear_mte_async_tcf thread_sctlr
#ifdef CONFIG_ARM64_MTE
alternative_if ARM64_MTE
	/* See comment in check_mte_async_tcf above. */
	/* 同上，仅 TCF0 bit1 表明可能有异步故障；DSB 后再清 TFSRE0_EL1。 */
	tbz	\thread_sctlr, #(SCTLR_EL1_TCF0_SHIFT + 1), 1f
	dsb	ish
	msr_s	SYS_TFSRE0_EL1, xzr
	/* DSB(Inner Shareable) 先让先前访问及其故障记录完成，再向寄存器写 0 清状态。 */
1:
alternative_else_nop_endif
#endif
	.endm

	.macro mte_set_gcr, mte_ctrl, tmp
	/* 从线程 MTE 控制字提取 16 位排除 mask，并启用硬件随机 tag 种子。 */
#ifdef CONFIG_ARM64_MTE
	ubfx	\tmp, \mte_ctrl, #MTE_CTRL_GCR_USER_EXCL_SHIFT, #16
	/* ubfx 从控制字指定起点抽取 16 位并零扩展；这些位决定哪些逻辑 tag 不参与生成。 */
	orr	\tmp, \tmp, #SYS_GCR_EL1_RRND
	msr_s	SYS_GCR_EL1, \tmp
#endif
	.endm

	.macro mte_set_kernel_gcr, tmp, tmp2
	/* HW_TAGS KASAN 启用时切换为内核 GCR；alternative 可在不适用系统上跳过。 */
#ifdef CONFIG_KASAN_HW_TAGS
alternative_cb	ARM64_ALWAYS_SYSTEM, kasan_hw_tags_enable
	b	1f
alternative_cb_end
	mov	\tmp, KERNEL_GCR_EL1
	msr_s	SYS_GCR_EL1, \tmp
1:
#endif
	.endm

	.macro mte_set_user_gcr, tsk, tmp, tmp2
	/* 从 tsk->thread.mte_ctrl 恢复用户 GCR；tmp/tmp2 由宏占用。 */
#ifdef CONFIG_KASAN_HW_TAGS
alternative_cb	ARM64_ALWAYS_SYSTEM, kasan_hw_tags_enable
	b	1f
alternative_cb_end
	ldr	\tmp, [\tsk, #THREAD_MTE_CTRL]

	mte_set_gcr \tmp, \tmp2
1:
#endif
	.endm

	.macro	kernel_entry, el, regsize = 64
	/*
	 * 把异常前现场保存为当前栈上的 pt_regs，并建立展开元数据。el=0 时还要清洗
	 * 用户寄存器、从 __entry_task 恢复 current、采集 MTE 故障、安装内核 PAC/MTE
	 * 状态和 SCS；el=1 时 current 已可信。宏退出后 x20-x23 含下方列出的暂存值。
	 */
	.if	\el == 0
	/*
	 * DIT=1 让部分数据相关指令的执行时间不依赖输入值，缩小侧信道；alternative
	 * 只在 CPU 声明 ARM64_HAS_DIT 时把 nop 替换为真正的 SET_PSTATE_DIT(1)。
	 */
	alternative_insn nop, SET_PSTATE_DIT(1), ARM64_HAS_DIT
	.endif
	.if	\regsize == 32
	// AArch32 入口先清 x0 高 32 位，避免把用户不可见的旧高位带入 pt_regs。
	mov	w0, w0				// zero upper 32 bits of x0
	.endif
	/*
	 * 阶段 1：按 pt_regs.regs[] 的连续布局保存 x0-x29。每个寄存器 8 字节，stp
	 * 一次写两个共 16 字节，所以偏移以 16 递增。此后这些寄存器可安全当临时量；
	 * x30/LR、原 SP、PC/PSTATE 因布局不同，在下面单独保存。
	 */
	stp	x0, x1, [sp, #16 * 0]
	stp	x2, x3, [sp, #16 * 1]
	stp	x4, x5, [sp, #16 * 2]
	stp	x6, x7, [sp, #16 * 3]
	stp	x8, x9, [sp, #16 * 4]
	stp	x10, x11, [sp, #16 * 5]
	stp	x12, x13, [sp, #16 * 6]
	stp	x14, x15, [sp, #16 * 7]
	stp	x16, x17, [sp, #16 * 8]
	stp	x18, x19, [sp, #16 * 9]
	stp	x20, x21, [sp, #16 * 10]
	stp	x22, x23, [sp, #16 * 11]
	stp	x24, x25, [sp, #16 * 12]
	stp	x26, x27, [sp, #16 * 13]
	stp	x28, x29, [sp, #16 * 14]

	.if	\el == 0
	/*
	 * 阶段 2A（来自用户态）：寄存器值已经落入 pt_regs，先清通用寄存器以降低
	 * 推测执行携带用户秘密的风险。硬件把用户 SP 放在 SP_EL0；用户曾可改写它，
	 * 因而先把它作为“被打断 SP”存入 x21，再从每 CPU 可信影子取 task_struct。
	 * Linux 在 EL1 约定 SP_EL0 保存 current，写回 tsk 后 get_current_task 才可靠。
	 */
	clear_gp_regs
	mrs	x21, sp_el0
	ldr_this_cpu	tsk, __entry_task, x20
	msr	sp_el0, tsk

	/*
	 * Ensure MDSCR_EL1.SS is clear, since we can unmask debug exceptions
	 * when scheduling.
	 */
	/* 调度期间可能重新开放调试异常，故入口必须清 MDSCR_EL1.SS 单步状态。 */
	ldr	x19, [tsk, #TSK_TI_FLAGS]
	disable_step_tsk x19, x20

	/* Check for asynchronous tag check faults in user space */
	/* 在覆盖用户控制状态前采集 EL0 的异步 MTE tag fault。 */
	ldr	x0, [tsk, THREAD_SCTLR_USER]
	/* THREAD_SCTLR_USER 是 task_struct::thread 内的软件快照，x0 后续也用于 PAC 判断。 */
	check_mte_async_tcf x22, x23, x0

#ifdef CONFIG_ARM64_PTR_AUTH
alternative_if ARM64_HAS_ADDRESS_AUTH
	/*
	 * Enable IA for in-kernel PAC if the task had it disabled. Although
	 * this could be implemented with an unconditional MRS which would avoid
	 * a load, this was measured to be slower on Cortex-A75 and Cortex-A76.
	 *
	 * Install the kernel IA key only if IA was enabled in the task. If IA
	 * was disabled on kernel exit then we would have left the kernel IA
	 * installed so there is no need to install it again.
	 */
	/*
	 * 若任务在 EL0 开启 IA，则安装任务的内核 IA key；若 EL0 禁用 IA，前次退出会
	 * 保留内核 key，只需重新置 SCTLR.EnIA。无条件 MRS 虽可省一次 load，但在
	 * Cortex-A75/A76 实测更慢，因此保留按线程快照分支。
	 */
	tbz	x0, SCTLR_ELx_ENIA_SHIFT, 1f
	/*
	 * tbz：用户快照 EnIA=0 时跳到 1:，仅把硬件 EnIA 重新打开；EnIA=1 时先安装
	 * 该任务专属的内核 APIA key。b 2f 用来跳过另一分支，二者在 2: 汇合。
	 */
	__ptrauth_keys_install_kernel_nosync tsk, x20, x22, x23
	b	2f
1:
	mrs	x0, sctlr_el1
	orr	x0, x0, SCTLR_ELx_ENIA
	msr	sctlr_el1, x0
2:
alternative_else_nop_endif
#endif

	apply_ssbd 1, x22, x23

	mte_set_kernel_gcr x22, x23
	/* 阶段 3：到这里 current、PAC、SSBD 和 MTE 已切到“可安全执行内核代码”的状态。 */

	/*
	 * Any non-self-synchronizing system register updates required for
	 * kernel entry should be placed before this point.
	 */
	/* 所有进入内核所需且不自同步的系统寄存器写必须放在这里之前，由后续 ISB 生效。 */
alternative_if ARM64_MTE
	/*
	 * PAC/MTE 系统寄存器写并非都自同步。若有 MTE，执行一次 ISB 后跳到共同标签；
	 * 否则只有 PAC 时执行下一个 ISB。这样任一能力存在都恰好同步一次，没有能力
	 * 时 alternative 把相关位置替换为空操作。
	 */
	isb
	b	1f
alternative_else_nop_endif
alternative_if ARM64_HAS_ADDRESS_AUTH
	isb
alternative_else_nop_endif
1:

	scs_load_current_base
	.else
	/*
	 * 阶段 2B（来自 EL1）：当前 SP 本来就是内核栈，预留 pt_regs 之前的 SP 等于
	 * sp + PT_REGS_SIZE；current 也未被用户破坏，可直接通过架构 helper 取得。
	 */
	add	x21, sp, #PT_REGS_SIZE
	get_current_task tsk
	.endif /* \el == 0 */
	mrs	x22, elr_el1
	mrs	x23, spsr_el1
	stp	lr, x21, [sp, #S_LR]
	/*
	 * 阶段 4：硬件异常入口已把返回 PC/PSTATE 放入 ELR_EL1/SPSR_EL1。x22/x23
	 * 读取它们，stp 则把 x30(LR) 与原 SP 写入 pt_regs 的相邻槽。
	 */

	/*
	 * Create a metadata frame record. The unwinder will use this to
	 * identify and unwind exception boundaries.
	 */
	/* 构造元数据帧，让展开器识别异常边界；EL0 是最终帧，EL1 可继续跨 pt_regs 回溯。 */
	stp	xzr, xzr, [sp, #S_STACKFRAME]
	/* 先把 frame_record 的 FP/LR 两字段清零，再在专用 type 字段标明如何继续展开。 */
	.if \el == 0
	mov	x0, #FRAME_META_TYPE_FINAL
	.else
	mov	x0, #FRAME_META_TYPE_PT_REGS
	.endif
	str	x0, [sp, #S_STACKFRAME_TYPE]
	add	x29, sp, #S_STACKFRAME
	/* x29 指向新 frame_record，使进入 C handler 后的回溯从异常边界开始。 */

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
alternative_if_not ARM64_HAS_PAN
	/* 无硬件 PAN 时以 BL 调软件 TTBR0-PAN helper；返回地址保存在 LR，现场已可覆盖。 */
	bl	__swpan_entry_el\el
alternative_else_nop_endif
#endif

	stp	x22, x23, [sp, #S_PC]

	/* Not in a syscall by default (el0_svc overwrites for real syscall) */
	/* EL0 异常默认不是系统调用；真正的 el0_svc 稍后覆盖 syscallno。 */
	.if	\el == 0
	mov	w21, #NO_SYSCALL
	str	w21, [sp, #S_SYSCALLNO]
	.endif

#ifdef CONFIG_ARM64_PSEUDO_NMI
alternative_if_not ARM64_HAS_GIC_PRIO_MASKING
	b	.Lskip_pmr_save\@
alternative_else_nop_endif

	mrs_s	x20, SYS_ICC_PMR_EL1
	str	w20, [sp, #S_PMR]
	/*
	 * pseudo-NMI 用 GIC PMR 表达优先级屏蔽。先把异常前 PMR 保存进 pt_regs，再写
	 * 内核入口值，使高优先级中断仍可突破普通 IRQ 屏蔽；不支持该能力时整段跳过。
	 */
	mov	x20, #GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET
	msr_s	SYS_ICC_PMR_EL1, x20

.Lskip_pmr_save\@:
#endif

	/*
	 * Registers that may be useful after this macro is invoked:
	 *
	 * x20 - ICC_PMR_EL1
	 * x21 - aborted SP
	 * x22 - aborted PC
	 * x23 - aborted PSTATE
	*/
	/* 宏返回时 x20-x23 分别保留 PMR、被打断 SP、PC、PSTATE，供具体入口继续使用。 */
	.endm

	.macro	kernel_exit, el
	/*
	 * 从栈上 pt_regs 恢复异常前现场并 ERET。el=0 路径还恢复用户 SP、MTE/PAC/GCR、
	 * SSBD、KPTI 跳板及必要的推测执行缓解；el=1 先屏蔽 DAIF。显式内存访问结束后
	 * 只允许规定的 barrier/eret 序列，避免恢复的用户寄存器再次被破坏。
	 */
	.if	\el != 0
	/* EL1 返回路径可能从允许更高优先级异常的状态到达；恢复现场时先统一屏蔽 DAIF。 */
	disable_daif
	.endif

#ifdef CONFIG_ARM64_PSEUDO_NMI
alternative_if_not ARM64_HAS_GIC_PRIO_MASKING
	b	.Lskip_pmr_restore\@
alternative_else_nop_endif

	ldr	w20, [sp, #S_PMR]
	msr_s	SYS_ICC_PMR_EL1, x20
	/* 写 w20 会自然忽略保存槽的高位；msr_s 再把异常前的 GIC 优先级阈值写回。 */

	/* Ensure priority change is seen by redistributor */
	/* 不支持 relaxed sync 时以全系统 DSB 保证 GIC redistributor 已观察到 PMR 恢复。 */
alternative_if_not ARM64_HAS_GIC_PRIO_RELAXED_SYNC
	dsb	sy
alternative_else_nop_endif

.Lskip_pmr_restore\@:
#endif

	// 从 pt_regs 取回返回 PC/PSTATE；EL0 分支还恢复保存的用户栈指针。
	ldp	x21, x22, [sp, #S_PC]		// load ELR, SPSR

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
alternative_if_not ARM64_HAS_PAN
	/* BL 调用与入口来源匹配的 __swpan_exit_el0/el1，可能同时修正保存的 x22 PSTATE。 */
	bl	__swpan_exit_el\el
alternative_else_nop_endif
#endif

	.if	\el == 0
	/*
	 * 阶段 1（准备 EL0 返回）：S_SP 保存用户栈指针，把它写回 SP_EL0。x22 是将写入
	 * SPSR_EL1 的用户 PSTATE；PSR_MODE32_BIT 置位表示 AArch32 compat 任务。
	 */
	ldr	x23, [sp, #S_SP]		// load return stack pointer
	msr	sp_el0, x23
	tst	x22, #PSR_MODE32_BIT		// native task?
	b.eq	3f
	/* b.eq 表示该 bit 为 0，即原生 AArch64；直接跳过仅针对 AArch32 的 erratum。 */

#ifdef CONFIG_ARM64_ERRATUM_845719
alternative_if ARM64_WORKAROUND_845719
#ifdef CONFIG_PID_IN_CONTEXTIDR
	/* 自读自写 CONTEXTIDR_EL1 强制产生一次寄存器更新序列，以满足 845719 workaround。 */
	mrs	x29, contextidr_el1
	msr	contextidr_el1, x29
#else
	msr contextidr_el1, xzr
#endif
alternative_else_nop_endif
#endif
3:
	/* Ignore asynchronous tag check faults in the uaccess routines */
	/* 清掉内核 uaccess 期间可能留下的异步 tag fault，避免错误归因给即将恢复的用户态。 */
	ldr	x0, [tsk, THREAD_SCTLR_USER]
	clear_mte_async_tcf x0
	/* x0 再次装入用户 SCTLR 快照，后续 PAC/MTE 都以同一返回策略为准。 */

#ifdef CONFIG_ARM64_PTR_AUTH
alternative_if ARM64_HAS_ADDRESS_AUTH
	/*
	 * IA was enabled for in-kernel PAC. Disable it now if needed, or
	 * alternatively install the user's IA. All other per-task keys and
	 * SCTLR bits were updated on task switch.
	 *
	 * No kernel C function calls after this.
	 */
	/*
	 * 内核中 EnIA 始终开启；退出时若用户启用 IA 就安装用户 IA key，否则清 EnIA。
	 * 其他每任务 key/SCTLR 位已在上下文切换处理。安装用户 key 后不得再调用内核 C，
	 * 否则内核返回地址认证将使用错误密钥。
	 */
	tbz	x0, SCTLR_ELx_ENIA_SHIFT, 1f
	/* EnIA=1 安装用户 APIA key；EnIA=0 跳到 1:，从真实 SCTLR_EL1 清除此能力位。 */
	__ptrauth_keys_install_user tsk, x0, x1, x2
	b	2f
1:
	mrs	x0, sctlr_el1
	bic	x0, x0, SCTLR_ELx_ENIA
	msr	sctlr_el1, x0
2:
alternative_else_nop_endif
#endif

	mte_set_user_gcr tsk, x0, x1

	apply_ssbd 0, x0, x1
	/*
	 * 阶段 2：MTE tag 生成策略和 Spectre-v4 固件状态都恢复为用户态要求。此后
	 * x0-x2 已可被这些宏破坏，因为真正用户值仍安全地保存在 pt_regs 中。
	 */
	.endif

	// 先设置 ERET 的 ELR/SPSR，再恢复全部通用寄存器。
	msr	elr_el1, x21			// set up the return data
	msr	spsr_el1, x22
	/*
	 * 阶段 3：ELR/SPSR 确定 eret 的目标 PC、异常级、条件码和屏蔽位；随后 15 条
	 * ldp 与入口 stp 完全对称，恢复 x0-x29。恢复后不能再把这些寄存器当临时量。
	 */
	ldp	x0, x1, [sp, #16 * 0]
	ldp	x2, x3, [sp, #16 * 1]
	ldp	x4, x5, [sp, #16 * 2]
	ldp	x6, x7, [sp, #16 * 3]
	ldp	x8, x9, [sp, #16 * 4]
	ldp	x10, x11, [sp, #16 * 5]
	ldp	x12, x13, [sp, #16 * 6]
	ldp	x14, x15, [sp, #16 * 7]
	ldp	x16, x17, [sp, #16 * 8]
	ldp	x18, x19, [sp, #16 * 9]
	ldp	x20, x21, [sp, #16 * 10]
	ldp	x22, x23, [sp, #16 * 11]
	ldp	x24, x25, [sp, #16 * 12]
	ldp	x26, x27, [sp, #16 * 13]
	ldp	x28, x29, [sp, #16 * 14]

	.if	\el == 0
#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
	/*
	 * ARM64_UNMAP_KERNEL_AT_EL0 是启动期能力。开启时 alternative 把首条 b 改成 nop，
	 * 继续执行 KPTI 退出；关闭时保留 b，直接跳到普通 LR/SP 恢复路径。
	 */
	alternative_insn "b .L_skip_tramp_exit_\@", nop, ARM64_UNMAP_KERNEL_AT_EL0

	/* x29 已恢复为用户值，先借 FAR_EL1 暂存；异常返回前不会再需要原 fault 地址。 */
	msr	far_el1, x29

	/*
	 * this_cpu_vector 给出 EL0 可见的跳板向量表；tramp_alias 构造 tramp_exit 在跳板
	 * 映射中的地址。br x29 不写 LR，因为 LR 此时必须保持待恢复的用户 x30。
	 */
	ldr_this_cpu	x30, this_cpu_vector, x29
	tramp_alias	x29, tramp_exit
	// KPTI 退出安装跳板向量，恢复 x30/SP 后跳入 tramp_exit 隐藏内核映射。
	msr		vbar_el1, x30		// install vector table
	ldr		lr, [sp, #S_LR]		// restore x30
	add		sp, sp, #PT_REGS_SIZE	// restore sp
	br		x29

.L_skip_tramp_exit_\@:
#endif
	.endif

	ldr	lr, [sp, #S_LR]
	add	sp, sp, #PT_REGS_SIZE		// restore sp
	/* 普通路径至此完成最后两项现场恢复：x30/LR 与分配 pt_regs 之前的内核 SP。 */

	.if \el == 0
	/* This must be after the last explicit memory access */
	/* 推测性非特权 load 缓解必须位于最后一次显式内存访问之后，防止形成新窗口。 */
alternative_if ARM64_WORKAROUND_SPECULATIVE_UNPRIV_LOAD
	/*
	 * TLBI VAE1 针对零地址发起失效，配合 DSB NSH 形成该 CPU 缺陷要求的序列；
	 * alternative 只在受影响 CPU 保留，其他 CPU 不承担这两条指令的成本。
	 */
	tlbi	vale1, xzr
	dsb	nsh
alternative_else_nop_endif
	.else
	/* Ensure any device/NC reads complete */
	/* EL1 返回时按 erratum 1508412 需要 DMB，确保设备/非缓存读取已经完成。 */
	alternative_insn nop, "dmb sy", ARM64_WORKAROUND_1508412
	.endif

	/*
	 * eret 原子地从 ELR_EL1 取 PC、从 SPSR_EL1 恢复 PSTATE/目标异常级。紧随其后的
	 * sb 是推测屏障：体系结构正常执行不会从 eret 顺序落到它，但处理异常返回
	 * 预测问题的 CPU 不能利用这里继续投机执行。
	 */
	eret
	sb
	.endm

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
	/*
	 * Set the TTBR0 PAN bit in SPSR. When the exception is taken from
	 * EL0, there is no need to check the state of TTBR0_EL1 since
	 * accesses are always enabled.
	 * Note that the meaning of this bit differs from the ARMv8.1 PAN
	 * feature as all TTBR0_EL1 accesses are disabled, not just those to
	 * user mappings.
	 */
	/*
	 * 软件 TTBR0 PAN 用保留 ASID 表示禁用全部 TTBR0_EL1 访问，并把模拟状态写入
	 * 保存的 SPSR。EL0 异常发生前 TTBR0 必然可访问，因此无需读取判断。它与
	 * ARMv8.1 PAN 只禁止用户映射的含义不同：这里会禁用 TTBR0 的所有访问。
	 */
SYM_CODE_START_LOCAL(__swpan_entry_el1)
	// 保留 ASID 表示 TTBR0 已禁用；据此在保存的 SPSR 中设置或清除模拟 PAN。
	mrs	x21, ttbr0_el1
	/* tst 等价于只更新条件码的 AND，不保存结果；这里检测 TTBR0 的 ASID 字段。 */
	tst	x21, #TTBRx_EL1_ASID_MASK	// Check for the reserved ASID
	orr	x23, x23, #PSR_PAN_BIT		// Set the emulated PAN in the saved SPSR
	b.eq	1f				// TTBR0 access already disabled
	and	x23, x23, #~PSR_PAN_BIT		// Clear the emulated PAN in the saved SPSR
SYM_INNER_LABEL(__swpan_entry_el0, SYM_L_LOCAL)
	/*
	 * EL0 入口直接落到内部标签；EL1 入口仅在原 TTBR0 可访问时也落到这里。
	 * __uaccess_ttbr0_disable 把 TTBR0 改成保留 ASID，从硬件层禁止误用用户地址。
	 */
	__uaccess_ttbr0_disable x21
1:	ret
SYM_CODE_END(__swpan_entry_el1)

	/*
	 * Restore access to TTBR0_EL1. If returning to EL0, no need for SPSR
	 * PAN bit checking.
	 */
	/* 返回时按保存的模拟 PAN 恢复 TTBR0；返回 EL0 必然需要开放，无需检查 SPSR 位。 */
SYM_CODE_START_LOCAL(__swpan_exit_el1)
	// 保存的 PAN 置位则维持禁用；无论如何都清掉 ARMv8.0 不识别的模拟位。
	tbnz	x22, #22, 1f			// Skip re-enabling TTBR0 access if the PSR_PAN_BIT is set
	__uaccess_ttbr0_enable x0, x1
1:	and	x22, x22, #~PSR_PAN_BIT		// ARMv8.0 CPUs do not understand this bit
	ret
SYM_CODE_END(__swpan_exit_el1)

SYM_CODE_START_LOCAL(__swpan_exit_el0)
	/* 返回用户态必须恢复真实 TTBR0，否则用户取指和数据访问都无法完成地址翻译。 */
	__uaccess_ttbr0_enable x0, x1
	/*
	 * Enable errata workarounds only if returning to user. The only
	 * workaround currently required for TTBR0_EL1 changes are for the
	 * Cavium erratum 27456 (broadcast TLBI instructions may cause I-cache
	 * corruption).
	 */
	/*
	 * 仅返回用户态才运行 TTBR0 更新缺陷 workaround；当前需要的是 Cavium 27456，
	 * 其广播 TLBI 可能破坏 I-cache。
	 */
	b	post_ttbr_update_workaround
	/* 用 b 而非 bl：workaround 完成后按其尾调用契约直接返回原调用者。 */
SYM_CODE_END(__swpan_exit_el0)
#endif

/* GPRs used by entry code */
/* 异常入口固定以 x28 作为 current task_struct 指针别名 tsk。 */
tsk	.req	x28		// current thread_info
	/* .req 给寄存器取汇编别名，不分配新存储；后文写 tsk 就是在读写 x28。 */

	.text

/*
 * Exception vectors.
 */
/*
 * ARM64 16 个硬件异常向量，按 EL1t、EL1h、64 位 EL0、32 位 EL0 四组排列，
 * 每组依次为同步、IRQ、FIQ、SError；整体按 2 KiB 对齐。
 *
 * “同步”表示事件可归因于被打断的那条指令：EL0 最常见的是 SVC 系统调用、
 * 用户缺页、未定义指令和调试异常，EL1 则可能是内核缺页、非法访问、BUG 或调试
 * 陷阱。“IRQ”通常承载时钟、外设和 IPI；FIQ 是体系结构预留的更高优先级中断类；
 * SError 用于无法精确归因到当前指令的异步系统错误。AArch32 compat 组仍处理相同
 * 生命周期，只是寄存器宽度、异常 syndrome 解码和最终 C 分派使用兼容路径。
 */
	.pushsection ".entry.text", "ax"
	/*
	 * pushsection 暂时把输出切到可分配(a)、可执行(x)的 .entry.text，并记住原节；
	 * 稍后的 popsection 会恢复。入口代码单独成节，便于链接脚本控制地址和映射。
	 */

	.align	11
	/* 2^11=2048 字节，是 AArch64 VBAR_EL1 向量表要求的整体对齐。 */
SYM_CODE_START(vectors)
	// 以下各行末尾英文依次标明同步/IRQ/FIQ/Error 及来源 EL/栈模式。
	kernel_ventry	1, t, 64, sync		// Synchronous EL1t
	kernel_ventry	1, t, 64, irq		// IRQ EL1t
	kernel_ventry	1, t, 64, fiq		// FIQ EL1t
	kernel_ventry	1, t, 64, error		// Error EL1t
	/*
	 * EL1t 表示异常前已经在 EL1 且使用 SP_EL0；EL1h 表示 EL1 使用 SP_EL1。
	 * sync 是由当前指令同步触发的异常，IRQ/FIQ 是两类中断，error 对应 SError。
	 */

	kernel_ventry	1, h, 64, sync		// Synchronous EL1h
	kernel_ventry	1, h, 64, irq		// IRQ EL1h
	kernel_ventry	1, h, 64, fiq		// FIQ EL1h
	kernel_ventry	1, h, 64, error		// Error EL1h

	kernel_ventry	0, t, 64, sync		// Synchronous 64-bit EL0
	kernel_ventry	0, t, 64, irq		// IRQ 64-bit EL0
	kernel_ventry	0, t, 64, fiq		// FIQ 64-bit EL0
	kernel_ventry	0, t, 64, error		// Error 64-bit EL0
	/* EL0 没有“h”栈模式，第三组处理 AArch64 用户程序，第四组处理 AArch32 compat。 */

	kernel_ventry	0, t, 32, sync		// Synchronous 32-bit EL0
	kernel_ventry	0, t, 32, irq		// IRQ 32-bit EL0
	kernel_ventry	0, t, 32, fiq		// FIQ 32-bit EL0
	kernel_ventry	0, t, 32, error		// Error 32-bit EL0
SYM_CODE_END(vectors)

SYM_CODE_START_LOCAL(__bad_stack)
	/*
	 * kernel_ventry 已切到 overflow 栈后到达此终止路径。这里重建 pt_regs，把原栈
	 * 指针写入现场并交给 handle_bad_stack() 报告；处理器不应返回，若返回则 BUG。
	 */
	/*
	 * We detected an overflow in kernel_ventry, which switched to the
	 * overflow stack. Stash the exception regs, and head to our overflow
	 * handler.
	 */
	/* 已检测到内核栈溢出并切至 overflow 栈，保存异常现场后进入致命错误处理器。 */

	/* Restore the original x0 value */
	/* kernel_ventry 已把原 x0 暂存在 TPIDRRO_EL0，此处先恢复。 */
	mrs	x0, tpidrro_el0

	/*
	 * Store the original GPRs to the new stack. The orginal SP (minus
	 * PT_REGS_SIZE) was stashed in tpidr_el0 by kernel_ventry.
	 */
	/* 在新栈保存原 GPR；原 SP-PT_REGS_SIZE 已由 kernel_ventry 放入 TPIDR_EL0。 */
	sub	sp, sp, #PT_REGS_SIZE
	kernel_entry 1
	/*
	 * 已经换到 overflow 栈，所以要在这里再次预留 pt_regs 并以 el=1 保存现场；
	 * kernel_entry 不知道原任务栈地址，随后从 TPIDR_EL0 取回并写入 S_SP 修正。
	 */
	mrs	x0, tpidr_el0
	add	x0, x0, #PT_REGS_SIZE
	str	x0, [sp, #S_SP]

	/* Stash the regs for handle_bad_stack */
	/* C 处理器的唯一参数 x0 指向刚构造的 pt_regs。 */
	mov	x0, sp

	/* Time to die */
	/* 栈完整性已丧失，进入不可恢复的致命路径。 */
	bl	handle_bad_stack
	ASM_BUG()
SYM_CODE_END(__bad_stack)


	.macro entry_handler el:req, ht:req, regsize:req, label:req
	/*
	 * 为一个向量类别生成薄包装：kernel_entry 建现场，x0 传 pt_regs 给对应 C
	 * handler，之后按来源 EL 跳往 ret_to_user 或 ret_to_kernel，绝不直接返回。
	 * 汇编层只保证现场和安全状态满足 C 代码的入口契约；具体业务由 entry-common.c
	 * 完成。例如 EL0 sync 根据 ESR_EL1 分派系统调用、数据/取指异常、FP/SVE、调试
	 * 或未定义指令，EL0 IRQ 经 irq_enter_rcu()/irq_exit_rcu() 调用体系结构中断控制器
	 * 的 handle_arch_irq，SError 则按 NMI 风格建立上下文后交给 do_serror()。
	 * 对 EL0 来源，C handler 返回前还会进入 exit-to-user 工作流程，处理需要在恢复
	 * 用户寄存器前完成的调度、信号及通知等事项；本宏随后只接上最终汇编出口。
	 */
SYM_CODE_START_LOCAL(el\el\ht\()_\regsize\()_\label)
	/*
	 * 以 el0t_64_sync 为例，本符号先展开 kernel_entry 0,64，再令 x0=sp 作为 AAPCS64
	 * 第一个参数，BL 调 el0t_64_sync_handler(struct pt_regs *)。BL 返回后不执行
	 * ret，而是 B 到公共退出路径，因为原 LR 属于被打断现场而非普通调用链。
	 */
	kernel_entry \el, \regsize
	mov	x0, sp
	bl	el\el\ht\()_\regsize\()_\label\()_handler
	.if \el == 0
	/* .if 在生成机器码时固定选择一个出口，每个包装函数最终只含一条对应的 b。 */
	b	ret_to_user
	.else
	b	ret_to_kernel
	.endif
SYM_CODE_END(el\el\ht\()_\regsize\()_\label)
	.endm

/*
 * Early exception handlers
 */
/*
 * 展开全部 EL/栈型/位宽/异常类别的早期处理包装，与 vectors 的排列逐项对应。
 * 前两组服务已经在内核执行的 EL1t/EL1h，后两组服务 64 位用户任务和 32 位 compat
 * 用户任务。四类包装共享保存/恢复骨架，却分别调用 sync/irq/fiq/error C handler，
 * 因而“同一张向量表”并不意味着系统调用、缺页和硬件中断共享业务处理函数。
 */
	entry_handler	1, t, 64, sync
	entry_handler	1, t, 64, irq
	entry_handler	1, t, 64, fiq
	entry_handler	1, t, 64, error

	entry_handler	1, h, 64, sync
	entry_handler	1, h, 64, irq
	entry_handler	1, h, 64, fiq
	entry_handler	1, h, 64, error

	entry_handler	0, t, 64, sync
	entry_handler	0, t, 64, irq
	entry_handler	0, t, 64, fiq
	entry_handler	0, t, 64, error

	entry_handler	0, t, 32, sync
	entry_handler	0, t, 32, irq
	entry_handler	0, t, 32, fiq
	entry_handler	0, t, 32, error

SYM_CODE_START_LOCAL(ret_to_kernel)
	/*
	 * EL1 handler 的公共返回点：异常业务和 IRQ/NMI 记账已由对应 C handler 收尾，
	 * 此处只恢复被打断的内核现场。它不会运行用户态 pending-work 循环，因为返回
	 * 目标仍是内核代码；若中断处理唤醒了更高优先级任务，抢占由适当的内核路径处理。
	 */
	kernel_exit 1
SYM_CODE_END(ret_to_kernel)

SYM_CODE_START_LOCAL(ret_to_user)
	/*
	 * EL0 公共返回点。到达这里前，普通异常 handler 已经通过 entry-common.c 的
	 * arm64_exit_to_user_mode()（系统调用使用其专用准备路径）消费退出工作标志；
	 * 其中可能发生重新调度、信号投递、系统调用重启和 notify/resume 类通知。
	 * ret_from_fork 等非异常入口则显式调用 asm_exit_to_user_mode() 接入同一套策略。
	 * 因此这里不是业务工作循环，而是“提交返回用户态”的最后汇编阶段：重查可能被
	 * C 路径或调度改变的单步状态、可选擦除栈，最后由 kernel_exit 0 恢复硬件现场。
	 */
	// 返回前重新读取 TIF_SINGLESTEP，因为 C handler 或调度可能已改变它。
	ldr	x19, [tsk, #TSK_TI_FLAGS]	// re-check for single-step
	enable_step_tsk x19, x2
#ifdef CONFIG_KSTACK_ERASE
	/* 擦栈 helper 是普通 C 调用，必须发生在 kernel_exit 恢复用户 PAC key/GPR 之前。 */
	bl	stackleak_erase_on_task_stack
#endif
	/*
	 * kernel_exit 0 将安装用户 PAC key、恢复用户 GPR/SP/PC/PSTATE 并 ERET；安装用户
	 * 密钥后不能再插入会按内核密钥签名返回地址的普通 C 调用，所以所有业务 helper
	 * 必须在这条宏调用之前完成。
	 */
	kernel_exit 0
SYM_CODE_END(ret_to_user)

	.popsection				// .entry.text

	// Move from tramp_pg_dir to swapper_pg_dir
	// 从仅映射跳板的 tramp_pg_dir 切换到完整内核 swapper_pg_dir。
	.macro tramp_map_kernel, tmp
	/* KPTI 入口使用固定偏移修正 TTBR1，并清用户 ASID 标记；tmp 被完全覆盖。 */
	mrs	\tmp, ttbr1_el1
	/*
	 * TTBR1_EL1 保存内核高地址页表基址和 ASID。跳板页表与完整页表按固定偏移布置，
	 * 所以 add 即可得到 swapper_pg_dir 版本；bic 清掉仅用于标记 EL0 隔离态的 bit。
	 */
	add	\tmp, \tmp, #TRAMP_SWAPPER_OFFSET
	bic	\tmp, \tmp, #USER_ASID_FLAG
	msr	ttbr1_el1, \tmp
	/* 写 TTBR1 改变后续高地址翻译；调用点负责以 ISB 等指令同步取指视图。 */
#ifdef CONFIG_QCOM_FALKOR_ERRATUM_1003
alternative_if ARM64_WORKAROUND_QCOM_FALKOR_E1003
	/* ASID already in \tmp[63:48] */
	/* ASID 已位于 tmp[63:48]，以下只构造用于定点 TLBI 的向量虚拟页号。 */
	movk	\tmp, #:abs_g2_nc:(TRAMP_VALIAS >> 12)
	movk	\tmp, #:abs_g1_nc:(TRAMP_VALIAS >> 12)
	/* 2MB boundary containing the vectors, so we nobble the walk cache */
	/* 对齐到包含向量的 2 MiB 边界并失效，规避 Falkor 页表 walk cache 缺陷。 */
	movk	\tmp, #:abs_g0_nc:((TRAMP_VALIAS & ~(SZ_2M - 1)) >> 12)
	isb
	tlbi	vae1, \tmp
	dsb	nsh
	/* ISB 先同步 TTBR 写，TLBI 失效指定 EL1 VA，DSB(NSH) 等待本 PE 范围维护完成。 */
alternative_else_nop_endif
#endif /* CONFIG_QCOM_FALKOR_ERRATUM_1003 */
	.endm

	// Move from swapper_pg_dir to tramp_pg_dir
	// 从完整内核 swapper_pg_dir 退回 EL0 可见的 tramp_pg_dir。
	.macro tramp_unmap_kernel, tmp
	/* 反向应用固定偏移并设置用户 ASID 标记，隐藏绝大部分内核映射。 */
	mrs	\tmp, ttbr1_el1
	sub	\tmp, \tmp, #TRAMP_SWAPPER_OFFSET
	orr	\tmp, \tmp, #USER_ASID_FLAG
	msr	ttbr1_el1, \tmp
	/* 退出后 TTBR1 只保留最小跳板映射；用户态不能借内核页表推测访问主体内核。 */
	/*
	 * We avoid running the post_ttbr_update_workaround here because
	 * it's only needed by Cavium ThunderX, which requires KPTI to be
	 * disabled.
	 */
	/* Cavium ThunderX workaround 只在 KPTI 禁用时需要，故此 KPTI 退场路径不执行。 */
	.endm

	.macro		tramp_data_read_var	dst, var
	/*
	 * 在跳板代码中安全取得内核变量地址。可重定位内核从专用 rodata 槽加载；固定
	 * 地址内核可用立即数构造，且高 16 位由 bit47 符号扩展省去一条指令。
	 */
#ifdef CONFIG_RELOCATABLE
	ldr		\dst, .L__tramp_data_\var
	/*
	 * 可重定位内核的最终地址要等链接/启动重定位后确定，因此从只读槽间接加载。
	 * .ifndef 保证同一个 var 的槽只生成一次；.quad 在 rodata 中预留 8 字节地址。
	 */
	.ifndef		.L__tramp_data_\var
	.pushsection	".entry.tramp.rodata", "a", %progbits
	.align		3
.L__tramp_data_\var:
	.quad		\var
	.popsection
	.endif
#else
	/*
	 * As !RELOCATABLE implies !RANDOMIZE_BASE the address is always a
	 * compile time constant (and hence not secret and not worth hiding).
	 *
	 * As statically allocated kernel code and data always live in the top
	 * 47 bits of the address space we can sign-extend bit 47 and avoid an
	 * instruction to load the upper 16 bits (which must be 0xFFFF).
	 */
	/*
	 * !RELOCATABLE 同时意味着无 KASLR，地址是非秘密编译期常量。内核代码/数据
	 * 位于地址空间最高 47 位，可符号扩展 bit47，省略装载恒为 0xFFFF 的高 16 位。
	 */
	movz		\dst, :abs_g2_s:\var
	movk		\dst, :abs_g1_nc:\var
	movk		\dst, :abs_g0_nc:\var
#endif
	.endm

#define BHB_MITIGATION_NONE	0
#define BHB_MITIGATION_LOOP	1
#define BHB_MITIGATION_FW	2
#define BHB_MITIGATION_INSN	3

/* 四个数字是汇编期策略枚举：无缓解、软件循环、固件调用、专用 clearbhb 指令。 */

	.macro tramp_ventry, vector_start, regsize, kpti, bhb
	/*
	 * 生成单个 KPTI/BHB 跳板向量槽：先保存 x30，按 bhb 选择 loop/clearbhb/固件
	 * 缓解，再按 kpti 映射完整内核并安装正式 VBAR，最后 RET 到对应正式向量。
	 */
	.align	7
1:
	.if	\regsize == 64
	// x30 在 kernel_ventry 中恢复，先暂存在用户不可直接写的 TPIDRRO_EL0。
	msr	tpidrro_el0, x30	// Restored in kernel_ventry
	.endif

	.if	\bhb == BHB_MITIGATION_LOOP
	/*
	 * This sequence must appear before the first indirect branch. i.e. the
	 * ret out of tramp_ventry. It appears here because x30 is free.
	 */
	/* BHB loop 必须位于首个间接分支 RET 之前；此处 x30 空闲，适合作为循环临时量。 */
	__mitigate_spectre_bhb_loop	x30
	.endif // \bhb == BHB_MITIGATION_LOOP

	.if	\bhb == BHB_MITIGATION_INSN
	/* clearbhb 清分支历史，紧随的 ISB 阻止后续取指越过这次状态变化。 */
	clearbhb
	isb
	.endif // \bhb == BHB_MITIGATION_INSN

	.if	\kpti == 1
	/*
	 * Defend against branch aliasing attacks by pushing a dummy
	 * entry onto the return stack and using a RET instruction to
	 * enter the full-fat kernel vectors.
	 */
	/* 压入伪返回栈项并用 RET 进入正式向量，抵御分支别名对返回预测器的攻击。 */
	bl	2f
	b	.
2:
	/*
	 * BL 2f 把“下一条 b .”压入硬件返回预测栈；顺序执行若意外到达 b . 会自旋，
	 * 正常代码从 2: 继续，最后 RET 使用经过设计的预测栈项进入正式向量。
	 */
	tramp_map_kernel	x30
alternative_insn isb, nop, ARM64_WORKAROUND_QCOM_FALKOR_E1003
	tramp_data_read_var	x30, vectors
alternative_if_not ARM64_WORKAROUND_CAVIUM_TX2_219_PRFM
	prfm	plil1strm, [x30, #(1b - \vector_start)]
	/* prfm 是性能提示而非有副作用的 load：预取即将进入的正式向量指令到 L1。 */
alternative_else_nop_endif

	msr	vbar_el1, x30
	isb
	/* VBAR_EL1 改指向完整向量表；ISB 保证再发生异常时硬件使用新基址。 */
	.else
	adr_l	x30, vectors
	.endif // \kpti == 1

	.if	\bhb == BHB_MITIGATION_FW
	/*
	 * The firmware sequence must appear before the first indirect branch.
	 * i.e. the ret out of tramp_ventry. But it also needs the stack to be
	 * mapped to save/restore the registers the SMC clobbers.
	 */
	/* 固件 BHB 序列也须早于首个 RET，但 SMC 会破坏寄存器，故必须先映射内核栈。 */
	__mitigate_spectre_bhb_fw
	.endif // \bhb == BHB_MITIGATION_FW

	add	x30, x30, #(1b - \vector_start + 4)
	/*
	 * 1b-vector_start 是当前槽在跳板表内的偏移，正式 vectors 使用相同布局；再加 4
	 * 跳过 kernel_ventry 的首条清理分支，使正式入口执行 TPIDRRO_EL0 恢复动作。
	 */
	ret
	// 与正式向量相同，每个跳板槽也必须严格限制为 128 字节。
.org 1b + 128	// Did we overflow the ventry slot?
	.endm

	.macro	generate_tramp_vector,	kpti, bhb
	/* 生成一整套 64/32 位 EL0 跳板槽；前 0x400 字节对应不经这些槽的 EL1 区域。 */
.Lvector_start\@:
	.space	0x400
	/* .space 生成 1024 字节占位，对应前 8 个 EL1 向量槽；跳板只接管后 8 个 EL0 槽。 */

	.rept	4
	/* .rept 在汇编期重复四次，依次占据 sync/IRQ/FIQ/SError 槽，不是运行时循环。 */
	tramp_ventry	.Lvector_start\@, 64, \kpti, \bhb
	.endr
	.rept	4
	tramp_ventry	.Lvector_start\@, 32, \kpti, \bhb
	.endr
	.endm

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
/*
 * Exception vectors trampoline.
 * The order must match __bp_harden_el1_vectors and the
 * arm64_bp_harden_el1_vectors enum.
 */
/*
 * KPTI 异常跳板只映射最小入口代码；各变体顺序必须与分支预测加固向量表及其枚举
 * 完全一致，运行时才能用索引选择正确的 BHB 缓解实现。
 */
	.pushsection ".entry.tramp.text", "ax"
	.align	11
SYM_CODE_START_LOCAL_NOALIGN(tramp_vectors)
#ifdef CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY
	generate_tramp_vector	kpti=1, bhb=BHB_MITIGATION_LOOP
	generate_tramp_vector	kpti=1, bhb=BHB_MITIGATION_FW
	generate_tramp_vector	kpti=1, bhb=BHB_MITIGATION_INSN
#endif /* CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY */
	generate_tramp_vector	kpti=1, bhb=BHB_MITIGATION_NONE
SYM_CODE_END(tramp_vectors)

SYM_CODE_START_LOCAL(tramp_exit)
	/* 隐藏内核页表、恢复借用 FAR_EL1 暂存的 x29，然后返回 EL0。 */
	tramp_unmap_kernel	x29
	// FAR_EL1 在 kernel_exit 中仅作为 x29 的临时保存槽，此处恢复。
	mrs		x29, far_el1		// restore x29
	/* 这里与 kernel_exit 尾部相同：eret 使用已设置的 ELR/SPSR，sb 阻断异常返回投机。 */
	eret
	sb
SYM_CODE_END(tramp_exit)
	.popsection				// .entry.tramp.text
#endif /* CONFIG_UNMAP_KERNEL_AT_EL0 */

/*
 * Exception vectors for spectre mitigations on entry from EL1 when
 * kpti is not in use.
 */
	/* 未启用 KPTI 时，EL1 入口仍可选择专用向量以应用 Spectre BHB 缓解。 */
	.macro generate_el1_vector, bhb
	/* 生成保留普通 EL1 槽、但为 EL0 区域加入指定 BHB 策略的一套向量。 */
.Lvector_start\@:
	// 行末英文说明每个槽的异常类别和来源模式；布局与主 vectors 相同。
	kernel_ventry	1, t, 64, sync		// Synchronous EL1t
	kernel_ventry	1, t, 64, irq		// IRQ EL1t
	kernel_ventry	1, t, 64, fiq		// FIQ EL1h
	kernel_ventry	1, t, 64, error		// Error EL1t

	kernel_ventry	1, h, 64, sync		// Synchronous EL1h
	kernel_ventry	1, h, 64, irq		// IRQ EL1h
	kernel_ventry	1, h, 64, fiq		// FIQ EL1h
	kernel_ventry	1, h, 64, error		// Error EL1h

	.rept	4
	tramp_ventry	.Lvector_start\@, 64, 0, \bhb
	.endr
	.rept 4
	tramp_ventry	.Lvector_start\@, 32, 0, \bhb
	.endr
	.endm

/* The order must match tramp_vecs and the arm64_bp_harden_el1_vectors enum. */
/* 变体顺序必须与 tramp_vecs 和 arm64_bp_harden_el1_vectors 枚举逐项匹配。 */
	.pushsection ".entry.text", "ax"
	.align	11
SYM_CODE_START(__bp_harden_el1_vectors)
	/* 每次 generate_el1_vector 都生成完整 2 KiB 表，多个表按策略枚举连续排列。 */
#ifdef CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY
	generate_el1_vector	bhb=BHB_MITIGATION_LOOP
	generate_el1_vector	bhb=BHB_MITIGATION_FW
	generate_el1_vector	bhb=BHB_MITIGATION_INSN
#endif /* CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY */
SYM_CODE_END(__bp_harden_el1_vectors)
	.popsection


/*
 * Register switch for AArch64. The callee-saved registers need to be saved
 * and restored. On entry:
 *   x0 = previous task_struct (must be preserved across the switch)
 *   x1 = next task_struct
 * Previous and next are guaranteed not to be the same.
 *
 */
/*
 * AArch64 最底层任务寄存器切换。x0=prev（切换期间保留并最终作为返回值），
 * x1=next，二者保证不同。函数在关中断窗口保存 prev 的 callee-saved 寄存器、
 * SP/LR，恢复 next 对应现场，更新 SP_EL0=current，安装 next 的内核 PAC key 和
 * shadow call stack，最后恢复原 IRQ 状态。新 SP 生效后执行流属于 next；RET 可能
 * 在很久以后 prev 再获调度时才返回到其 __switch_to()。
 */
SYM_FUNC_START(cpu_switch_to)
	save_and_disable_daif x11
	/*
	 * x11 保存进入时 DAIF，宏同时屏蔽调试/SError/IRQ/FIQ，避免 SP 和 current 已
	 * 部分切换时被异步路径观察。THREAD_CPU_CONTEXT 是 thread_struct 内偏移；
	 * x8 指向 prev->thread.cpu_context，x9 暂存当前 SP。
	 */
	mov	x10, #THREAD_CPU_CONTEXT
	add	x8, x0, x10
	mov	x9, sp
	// 先把 prev 的被调用者保存寄存器写入 cpu_context，再从 next 对称恢复。
	stp	x19, x20, [x8], #16		// store callee-saved registers
	stp	x21, x22, [x8], #16
	stp	x23, x24, [x8], #16
	stp	x25, x26, [x8], #16
	stp	x27, x28, [x8], #16
	stp	x29, x9, [x8], #16
	str	lr, [x8]
	/*
	 * [x8], #16 是“后索引”：先按旧 x8 存一对，再让 x8 前进 16 字节。最后保存
	 * FP 与 SP，再单独保存 LR。x0 没有被改写，所以返回时仍可代表 prev/last。
	 */
	add	x8, x1, x10
	ldp	x19, x20, [x8], #16		// restore callee-saved registers
	ldp	x21, x22, [x8], #16
	ldp	x23, x24, [x8], #16
	ldp	x25, x26, [x8], #16
	ldp	x27, x28, [x8], #16
	ldp	x29, x9, [x8], #16
	ldr	lr, [x8]
	/* next 的布局与 prev 完全对称；装载 LR 后，未来 ret 将进入 next 上次停下的位置。 */
	mov	sp, x9
	msr	sp_el0, x1
	/*
	 * mov sp 是真正的栈切换点：它之后局部变量/返回链属于 next。SP_EL0 按 Linux
	 * EL1 约定保存 current，因此写 x1；来自 EL0 的异常入口会再用影子值修复它。
	 */
	ptrauth_keys_install_kernel x1, x8, x9, x10
	/* next 的内核返回地址必须用 next 独有 PAC key 认证，安装后才可安全执行 RET。 */
	scs_save x0
	scs_load_current
	/* SCS 指针先保存回 prev，再由已经更新的 current/SP_EL0 装载 next 的 SCS。 */
	restore_irq x11
	ret
	/* 恢复切换前 DAIF 后 RET；“返回地址”来自 next->cpu_context.lr，不一定是本次调用者。 */
SYM_FUNC_END(cpu_switch_to)
NOKPROBE(cpu_switch_to)
	/* NOKPROBE 禁止 kprobe 在最脆弱的切栈序列插桩，避免递归异常看到半切换状态。 */

/*
 * This is how we return from a fork.
 */
/*
 * 新任务的共同首个 PC。schedule_tail() 完成调度器交接；x19 非零表示内核线程，
 * 以 x20 为参数调用其入口。用户任务（或内核线程入口返回）随后进入统一的
 * exit-to-user 工作循环，并由 ret_to_user 恢复 copy_thread() 构造的 pt_regs。
 */
SYM_CODE_START(ret_from_fork)
	bl	schedule_tail
	/* schedule_tail 完成新任务首次运行所需的调度器锁/统计交接；它是普通 C 调用。 */
	// x19 为 0 表示普通用户任务，跳过内核线程函数调用。
	cbz	x19, 1f				// not a kernel thread
	mov	x0, x20
	blr	x19
	/* BLR 与 BL 类似但目标来自寄存器：调用 copy_thread 放在 x19 的 fn(x20)。 */
1:	get_current_task tsk
	mov	x0, sp
	/* 新任务的 SP 正好指向 copy_thread 构造的 pt_regs，因此作为 x0 传给退出工作循环。 */
	bl	asm_exit_to_user_mode
	b	ret_to_user
SYM_CODE_END(ret_from_fork)
NOKPROBE(ret_from_fork)

/*
 * void call_on_irq_stack(struct pt_regs *regs,
 * 		          void (*func)(struct pt_regs *));
 *
 * Calls func(regs) using this CPU's irq stack and shadow irq stack.
 */
/*
 * call_on_irq_stack(regs, func) 在当前 CPU 专用 IRQ 栈调用 func(regs)。切栈前保存
 * DAIF、FP/LR 和可选 SCS，调用期间恢复原中断状态；返回后再次关中断以原子恢复
 * 原栈/SCS。IRQ 栈属每 CPU 资源，调用上下文必须保证不会并发复用同一栈。
 */
SYM_FUNC_START(call_on_irq_stack)
	save_and_disable_daif x9
	/* 切普通栈和 SCS 必须作为一个不可被中断观察的状态转换，x9 保存原 DAIF。 */
#ifdef CONFIG_SHADOW_CALL_STACK
	get_current_task x16
	scs_save x16
	ldr_this_cpu scs_sp, irq_shadow_call_stack_ptr, x17
	/* 保存任务 SCS 后，把专用寄存器 scs_sp 指向本 CPU IRQ shadow stack。 */
#endif

	/* Create a frame record to save our LR and SP (implicit in FP) */
	/* 在原栈建立帧记录；FP 间接保存旧 SP，记录本身保存 FP/LR。 */
	stp	x29, x30, [sp, #-16]!
	/* 前索引 [sp,#-16]! 先把 SP 下移 16 字节再存值，等价于向栈压入一帧。 */
	mov	x29, sp

	ldr_this_cpu x16, irq_stack_ptr, x17
	/* irq_stack_ptr 是每 CPU IRQ 栈基址，x17 仅供地址计算，x16 接收基址。 */

	/* Move to the new stack and call the function there */
	/* 把 SP 指到每 CPU IRQ 栈顶，恢复调用前 IRQ 状态，再间接调用 func。 */
	add	sp, x16, #IRQ_STACK_SIZE
	restore_irq x9
	blr	x1
	/* AAPCS64 中入口 x0 仍是 regs；x1 是函数指针，BLR 调用时自动把返回地址写入 LR。 */

	save_and_disable_daif x9
	/*
	 * Restore the SP from the FP, and restore the FP and LR from the frame
	 * record.
	 */
	/* 以 FP 找回原 SP，再从原栈帧恢复 FP/LR，随后恢复任务 SCS。 */
	mov	sp, x29
	ldp	x29, x30, [sp], #16
	/* 后索引形式先弹出 FP/LR，再令 SP 增加 16，回到调用本函数前的栈顶。 */
	scs_load_current
	restore_irq x9
	ret
SYM_FUNC_END(call_on_irq_stack)
NOKPROBE(call_on_irq_stack)

#ifdef CONFIG_ARM_SDE_INTERFACE

#include <asm/sdei.h>
#include <uapi/linux/arm_sdei.h>

/*
 * SDEI（Software Delegated Exception Interface）是 Arm 固件向操作系统委托事件的
 * ABI：固件在事件发生时进入注册好的内核入口，并在处理结束后通过 SMC/HVC 完成
 * 协议恢复，而不是像普通 GIC IRQ 那样从上面的 VBAR_EL1 向量进入并 ERET。平台可
 * 用它上报需要固件协同的紧急事件，例如某些 RAS 或 watchdog 类事件；事件含义由
 * 固件和注册驱动约定，本汇编只负责保存/切换执行上下文。
 *
 * SDEI 把事件分为 normal 与 critical 优先级。critical 可以在 normal 活跃期间到达，
 * 因而两级各有每 CPU 私有栈、SCS 和 active-event 槽，嵌套时不能复用或覆盖另一
 * 级的现场。其执行约束接近 NMI：入口可能打断用户态、内核态或一次尚未完成的
 * 异常转换，不能睡眠，也不能假定普通 IRQ 的屏蔽和记账状态已经建立。注册事件及
 * 每 CPU 缓冲区的所有权由 SDEI 核心代码管理；本文件只在事件存续期间借用这些
 * 记录，最终必须调用固件完成接口，不能用普通 RET/ERET 自行结束。
 */

.macro sdei_handler_exit exit_mode
	/* 按平台选定的 SMC/HVC conduit 调用 SDEI 完成接口；成功绝不返回。 */
	/* On success, this call never returns... */
	/* 固件成功接收完成请求后不会返回；意外返回则原地自旋，避免继续使用破坏的现场。 */
	cmp	\exit_mode, #SDEI_EXIT_SMC
	/* CMP 只设置条件码；不是 SMC 模式就 b.ne 到 99: 使用 HVC conduit。 */
	b.ne	99f
	smc	#0
	b	.
99:	hvc	#0
	b	.
.endm

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
/*
 * The regular SDEI entry point may have been unmapped along with the rest of
 * the kernel. This trampoline restores the kernel mapping to make the x1 memory
 * argument accessible.
 *
 * This clobbers x4, __sdei_handler() will restore this from firmware's
 * copy.
 */
/*
 * KPTI 可能把常规 SDEI 入口连同内核映射隐藏。此跳板先恢复完整 TTBR1，使 x1
 * 指向的注册事件内存可访问，并把原 TTBR1 状态记录到事件结构供退出恢复。
 * x4 会被占用，但固件保存副本稍后由 __sdei_asm_handler 恢复。
 */
.pushsection ".entry.tramp.text", "ax"
SYM_CODE_START(__sdei_asm_entry_trampoline)
	mrs	x4, ttbr1_el1
	tbz	x4, #USER_ASID_BIT, 1f
	/* USER_ASID_BIT=0 表示完整内核映射本来就在，直接保留原 TTBR1 到 1: 记录。 */

	tramp_map_kernel tmp=x4
	isb
	mov	x4, xzr
	/* 若本函数主动 map 内核，就以 0 作哨兵；退出看到 0 才重新隐藏内核映射。 */

	/*
	 * Remember whether to unmap the kernel on exit.
	 */
	/* 保存入口是否来自隐藏内核映射状态，退出跳板据此决定重新 unmap。 */
1:	str	x4, [x1, #(SDEI_EVENT_INTREGS + S_SDEI_TTBR1)]
	tramp_data_read_var     x4, __sdei_asm_handler
	br	x4
	/* BR 是不写 LR 的尾跳转；SDEI 不走普通函数返回，最终由固件完成接口恢复现场。 */
SYM_CODE_END(__sdei_asm_entry_trampoline)
NOKPROBE(__sdei_asm_entry_trampoline)

/*
 * Make the exit call and restore the original ttbr1_el1
 *
 * x0 & x1: setup for the exit API call
 * x2: exit_mode
 * x4: struct sdei_registered_event argument from registration time.
 */
/*
 * SDEI 退出跳板：x0/x1 已按完成 API 布置，x2 是 conduit 模式，x4 指向注册事件。
 * 若入口曾映射内核则恢复原 TTBR1 隔离状态，随后通过固件完成调用且不返回。
 */
SYM_CODE_START(__sdei_asm_exit_trampoline)
	ldr	x4, [x4, #(SDEI_EVENT_INTREGS + S_SDEI_TTBR1)]
	cbnz	x4, 1f
	/* 保存值非零表示入口未改页表，跳过 unmap；零哨兵表示需要撤销入口映射。 */

	tramp_unmap_kernel	tmp=x4

1:	sdei_handler_exit exit_mode=x2
SYM_CODE_END(__sdei_asm_exit_trampoline)
NOKPROBE(__sdei_asm_exit_trampoline)
.popsection		// .entry.tramp.text
#endif /* CONFIG_UNMAP_KERNEL_AT_EL0 */

/*
 * Software Delegated Exception entry point.
 *
 * x0: Event number
 * x1: struct sdei_registered_event argument from registration time.
 * x2: interrupted PC
 * x3: interrupted PSTATE
 * x4: maybe clobbered by the trampoline
 *
 * Firmware has preserved x0->x17 for us, we must save/restore the rest to
 * follow SMC-CC. We save (or retrieve) all the registers as the handler may
 * want them.
 */
/*
 * 软件委托异常 SDEI 的底层入口。固件给出事件号、注册事件、被打断 PC/PSTATE，
 * 并按 SMCCC 保存 x0-x17；内核仍需保存 x18-x30/SP 以及 handler 可能查询的全部
 * 寄存器。随后按 normal/critical 优先级切换独立每 CPU 栈与 SCS，恢复可信
 * current，调用 C handler，最后清活动事件并选择 COMPLETE 或
 * COMPLETE_AND_RESUME。注册事件结构由 SDEI 核心长期持有，覆盖整个固件回调。
 */
SYM_CODE_START(__sdei_asm_handler)
	/*
	 * 阶段 1：x1 指向注册事件内嵌的 intregs 保存区。成对存储 PC/PSTATE 与 x4-x29，
	 * 再把当前 SP 搬到 x4，与 LR 一起保存。x0-x17 的原值由固件副本负责恢复，
	 * 这里仍保存其中一部分是为了让 C handler 能检查完整被打断现场。
	 */
	stp     x2, x3, [x1, #SDEI_EVENT_INTREGS + S_PC]
	stp     x4, x5, [x1, #SDEI_EVENT_INTREGS + 16 * 2]
	stp     x6, x7, [x1, #SDEI_EVENT_INTREGS + 16 * 3]
	stp     x8, x9, [x1, #SDEI_EVENT_INTREGS + 16 * 4]
	stp     x10, x11, [x1, #SDEI_EVENT_INTREGS + 16 * 5]
	stp     x12, x13, [x1, #SDEI_EVENT_INTREGS + 16 * 6]
	stp     x14, x15, [x1, #SDEI_EVENT_INTREGS + 16 * 7]
	stp     x16, x17, [x1, #SDEI_EVENT_INTREGS + 16 * 8]
	stp     x18, x19, [x1, #SDEI_EVENT_INTREGS + 16 * 9]
	stp     x20, x21, [x1, #SDEI_EVENT_INTREGS + 16 * 10]
	stp     x22, x23, [x1, #SDEI_EVENT_INTREGS + 16 * 11]
	stp     x24, x25, [x1, #SDEI_EVENT_INTREGS + 16 * 12]
	stp     x26, x27, [x1, #SDEI_EVENT_INTREGS + 16 * 13]
	stp     x28, x29, [x1, #SDEI_EVENT_INTREGS + 16 * 14]
	mov	x4, sp
	stp     lr, x4, [x1, #SDEI_EVENT_INTREGS + S_LR]

	mov	x19, x1
	/* x19 是 callee-saved 寄存器，用它长期持有事件指针，跨越后面的 C 函数调用。 */

	/* Store the registered-event for crash_smp_send_stop() */
	/* 发布当前 normal/critical 活动事件，崩溃停机路径可据此识别固件上下文。 */
	ldrb	w4, [x19, #SDEI_EVENT_PRIORITY]
	/* ldrb 只读一个无符号字节并零扩展；0=normal，非零=critical。 */
	cbnz	w4, 1f
	adr_this_cpu dst=x5, sym=sdei_active_normal_event, tmp=x6
	b	2f
1:	adr_this_cpu dst=x5, sym=sdei_active_critical_event, tmp=x6
2:	str	x19, [x5]
	/* x5 是选中的每 CPU active slot 地址；写入事件裸指针，退出前必须对称清零。 */

	/*
	 * entry.S may have been using sp as a scratch register, find whether
	 * this is a normal or critical event and switch to the appropriate
	 * stack for this CPU.
	 */
	/* entry.S 可能把 SP 当临时寄存器，不能信任；按事件优先级选择本 CPU 专用栈。 */
	cbnz	w4, 1f
	ldr_this_cpu dst=x5, sym=sdei_stack_normal_ptr, tmp=x6
	b	2f
1:	ldr_this_cpu dst=x5, sym=sdei_stack_critical_ptr, tmp=x6
2:	mov	x6, #SDEI_STACK_SIZE
	add	x5, x5, x6
	mov	sp, x5
	/* 存储的是栈基址，加固定大小得到向下增长的栈顶，再一次性切换 SP。 */

#ifdef CONFIG_SHADOW_CALL_STACK
	/* Use a separate shadow call stack for normal and critical events */
	/* normal 与 critical 事件分别使用独立 SCS，允许高优先级事件安全嵌套。 */
	cbnz	w4, 3f
	ldr_this_cpu dst=scs_sp, sym=sdei_shadow_call_stack_normal_ptr, tmp=x6
	b	4f
3:	ldr_this_cpu dst=scs_sp, sym=sdei_shadow_call_stack_critical_ptr, tmp=x6
4:
#endif

	/*
	 * We may have interrupted userspace, or a guest, or exit-from or
	 * return-to either of these. We can't trust sp_el0, restore it.
	 */
	/*
	 * SDEI 可打断用户、guest 或两者的进出过渡，SP_EL0 因而不可信；先保存其值，
	 * 再由每 CPU __entry_task 恢复 current，离开 handler 前原样写回。
	 */
	mrs	x28, sp_el0
	ldr_this_cpu	dst=x0, sym=__entry_task, tmp=x1
	msr	sp_el0, x0
	/* x28 暂存进入前 SP_EL0；当前可信 task 写入后，普通 current helper 才可使用。 */

	/* If we interrupted the kernel point to the previous stack/frame. */
	/* 仅当被打断 EL 等于 CurrentEL 时，把旧 FP/ELR 接入回溯链；否则以 0 截断。 */
	and     x0, x3, #0xc
	mrs     x1, CurrentEL
	cmp     x0, x1
	/* PSTATE.M[3:2] 与 CurrentEL 使用可比较编码；相等表示 SDEI 打断的是当前内核 EL。 */
	// 同 EL 保留旧 FP/ELR，跨 EL 则选择 0 作为回溯终点。
	csel	x29, x29, xzr, eq	// fp, or zero
	csel	x4, x2, xzr, eq		// elr, or zero

	stp	x29, x4, [sp, #-16]!
	mov	x29, sp
	/* 在 SDEI 栈建立标准 FP/返回地址帧，令回溯可连接到被打断内核现场或在跨 EL 时终止。 */

	add	x0, x19, #SDEI_EVENT_INTREGS
	mov	x1, x19
	bl	__sdei_handler
	/* C ABI 参数：x0 指向保存的寄存器，x1 指向注册事件；返回 x0 决定固件完成方式。 */

	msr	sp_el0, x28
	/* restore regs >x17 that we clobbered */
	/* 恢复本入口自行破坏的 x18 以上寄存器、LR 与原 SP；x4 暂留事件指针给跳板。 */
	// x4 特意保留注册事件指针，供可能经过的 SDEI 退出跳板读取 TTBR1 记录。
	mov	x4, x19         // keep x4 for __sdei_asm_exit_trampoline
	ldp	x28, x29, [x4, #SDEI_EVENT_INTREGS + 16 * 14]
	ldp	x18, x19, [x4, #SDEI_EVENT_INTREGS + 16 * 9]
	ldp	lr, x1, [x4, #SDEI_EVENT_INTREGS + S_LR]
	mov	sp, x1

	// C 返回值同时作为 COMPLETE_AND_RESUME 的恢复地址参数。
	mov	x1, x0			// address to complete_and_resume
	/* x0 = (x0 <= SDEI_EV_FAILED) ?
	 * EVENT_COMPLETE:EVENT_COMPLETE_AND_RESUME
	 */
	/* C handler 返回失败及以下状态时只完成事件，否则请求固件完成并从给定地址恢复。 */
	cmp	x0, #SDEI_EV_FAILED
	mov_q	x2, SDEI_1_0_FN_SDEI_EVENT_COMPLETE
	mov_q	x3, SDEI_1_0_FN_SDEI_EVENT_COMPLETE_AND_RESUME
	csel	x0, x2, x3, ls
	/* CSEL 不跳转：无符号 <=(ls) 时选 COMPLETE 功能号，否则选 AND_RESUME。 */

	ldr_l	x2, sdei_exit_mode

	/* Clear the registered-event seen by crash_smp_send_stop() */
	/* 固件退出前撤销活动事件发布，避免崩溃路径以后观察到已结束的陈旧指针。 */
	ldrb	w3, [x4, #SDEI_EVENT_PRIORITY]
	cbnz	w3, 1f
	adr_this_cpu dst=x5, sym=sdei_active_normal_event, tmp=x6
	b	2f
1:	adr_this_cpu dst=x5, sym=sdei_active_critical_event, tmp=x6
2:	str	xzr, [x5]

alternative_if_not ARM64_UNMAP_KERNEL_AT_EL0
	/* 无 KPTI 时启动期 alternative 保留直接固件退出；有 KPTI 时此处成为空操作。 */
	sdei_handler_exit exit_mode=x2
alternative_else_nop_endif

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
	tramp_alias	dst=x5, sym=__sdei_asm_exit_trampoline
	br	x5
	/* KPTI 配置下尾跳到仍有映射的退出跳板，先隐藏内核页表再通知固件。 */
#endif
SYM_CODE_END(__sdei_asm_handler)
NOKPROBE(__sdei_asm_handler)

SYM_CODE_START(__sdei_handler_abort)
	/*
	 * 中止当前 SDEI handler，并让固件从本地标签 1 继续；完成调用会覆盖易失寄存器、
	 * PSTATE、ELR/SPSR，因此标签后的代码只能直接返回，不能依赖原异常现场。
	 */
	mov_q	x0, SDEI_1_0_FN_SDEI_EVENT_COMPLETE_AND_RESUME
	adr	x1, 1f
	/* ADR 形成当前代码附近标签 1: 的地址，不访问内存；固件把它作为恢复 PC。 */
	ldr_l	x2, sdei_exit_mode
	sdei_handler_exit exit_mode=x2
	// exit the handler and jump to the next instruction.
	// Exit will stomp x0-x17, PSTATE, ELR_ELx, and SPSR_ELx.
	// 退出 handler 后跳到下一条指令；固件退出会覆盖 x0-x17、PSTATE、ELR_ELx 和 SPSR_ELx。
1:	ret
SYM_CODE_END(__sdei_handler_abort)
NOKPROBE(__sdei_handler_abort)
#endif /* CONFIG_ARM_SDE_INTERFACE */
