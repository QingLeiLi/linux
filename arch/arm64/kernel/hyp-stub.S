/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arm64 EL2 hypervisor stub 学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 固件若把 CPU 交给 EL2，Linux 早期会安装这张极小的 EL2 向量表，然后降到 EL1
 * 完成常规启动。EL1 通过 HVC 回到 stub，可安装真正 hypervisor 向量、查询 GIC
 * 虚拟化能力、软重启，或在硬件支持 VHE 时把已经建立的 EL1 内核状态迁移到 EL2
 * host 语义。它不是完整 KVM hypervisor：除少数固定调用号外，其余 HVC 返回错误，
 * 异步异常向量也故意自旋，避免在没有完整保存/分派框架时继续执行。
 *
 * 主要生命周期是 head.S:init_kernel_el() 安装 stub -> 内核在 EL1 建立页表/栈 ->
 * finalise_el2() 发 HVC -> __finalise_el2/enter_vhe 迁移为 VHE，或后续 KVM 逐 CPU 用
 * __hyp_set_vectors() 替换 VBAR_EL2。状态由当前 CPU 的系统寄存器持有，不存在普通
 * 内存对象所有权和锁；每个 CPU 必须单独完成协议。MMU、TLB、异常返回模式的指令
 * 顺序就是硬件提交协议，任一中间状态都不能调用普通 C 代码或睡眠。
 */
/*
 * Hypervisor stub
 *
 * Copyright (C) 2012 ARM Ltd.
 * Author:	Marc Zyngier <marc.zyngier@arm.com>
 */
/* 极简 hypervisor 占位实现；保留上游版权与作者信息。 */

#include <linux/init.h>
#include <linux/linkage.h>

#include <asm/assembler.h>
#include <asm/el2_setup.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_asm.h>
#include <asm/ptrace.h>
#include <asm/virt.h>

	.text
	.pushsection	.hyp.text, "ax"
	/*
	 * `.text` 选择普通代码节，pushsection 再暂时切到可分配、可执行的 .hyp.text 并
	 * 保存原节；文件末尾 popsection 恢复。链接脚本据此把 EL2 stub 放入专用映射。
	 */

	.align 11
	/* VBAR_EL2 要求整张异常向量表按 2^11=2048 字节对齐。 */

SYM_CODE_START(__hyp_stub_vectors)
	/*
	 * EL2 向量表共 16 个 128 字节槽，按 EL2t、EL2h、64 位下级 EL、32 位下级 EL
	 * 分组。stub 只接受 EL2h 同步异常和来自 64 位 EL1 的同步 HVC，二者进入
	 * elx_sync；IRQ/FIQ/SError 及 32 位入口均视为不可能事件并永久停在 invalid。
	 */
	ventry	el2_sync_invalid		// Synchronous EL2t
	/* EL2t 同步异常无 stub 业务处理。 */
	ventry	el2_irq_invalid			// IRQ EL2t
	/* EL2t IRQ 无处理器。 */
	ventry	el2_fiq_invalid			// FIQ EL2t
	/* EL2t FIQ 无处理器。 */
	ventry	el2_error_invalid		// Error EL2t
	/* EL2t SError 无处理器。 */

	ventry	elx_sync			// Synchronous EL2h
	/* EL2h 同步入口允许 stub 自身发起/接收受支持的 HVC 协议。 */
	ventry	el2_irq_invalid			// IRQ EL2h
	/* EL2h IRQ 不由占位 stub 分派。 */
	ventry	el2_fiq_invalid			// FIQ EL2h
	/* EL2h FIQ 不由占位 stub 分派。 */
	ventry	el2_error_invalid		// Error EL2h
	/* EL2h SError 不由占位 stub 分派。 */

	ventry	elx_sync			// Synchronous 64-bit EL1
	/* 64 位 EL1 执行 HVC 后从这里进入 elx_sync。 */
	ventry	el1_irq_invalid			// IRQ 64-bit EL1
	/* 来自 64 位 EL1 的 IRQ 不应路由给 stub。 */
	ventry	el1_fiq_invalid			// FIQ 64-bit EL1
	/* 来自 64 位 EL1 的 FIQ 不应路由给 stub。 */
	ventry	el1_error_invalid		// Error 64-bit EL1
	/* 来自 64 位 EL1 的 SError 不应路由给 stub。 */

	ventry	el1_sync_invalid		// Synchronous 32-bit EL1
	/* stub 不提供 AArch32 HVC ABI。 */
	ventry	el1_irq_invalid			// IRQ 32-bit EL1
	/* 32 位下级 EL 的 IRQ 为无效入口。 */
	ventry	el1_fiq_invalid			// FIQ 32-bit EL1
	/* 32 位下级 EL 的 FIQ 为无效入口。 */
	ventry	el1_error_invalid		// Error 32-bit EL1
	/* 32 位下级 EL 的 SError 为无效入口。 */
SYM_CODE_END(__hyp_stub_vectors)

	.align 11
	/* 向量槽全部展开后再次按 2 KiB 对齐，把普通分派代码隔离在向量表边界之外。 */

SYM_CODE_START_LOCAL(elx_sync)
	/*
	 * stub HVC 分派器。x0 是 HVC 功能号，x1..x4 是功能相关参数；返回时 x0 为状态，
	 * 查询功能还用 x1 返回数据。它运行在异常上下文、无栈保存框架，不能调用 C；
	 * 除 FINALISE/软重启的非普通出口外，统一以 ERET 回到发起 HVC 的上下文。
	 */
	cmp	x0, #HVC_SET_VECTORS
	b.ne	1f
	/* 安装调用者给出的 2 KiB 对齐 EL2 向量物理/执行地址；逐 CPU 写 VBAR_EL2。 */
	msr	vbar_el2, x1
	b	9f

1:	cmp	x0, #HVC_FINALISE_EL2
	/* finalise 会迁移整个执行级状态，因此尾跳到专用路径而非普通返回。 */
	b.eq	__finalise_el2

	cmp	x0, #HVC_GET_ICH_VTR_EL2
	b.ne	2f
	mrs_s	x1, SYS_ICH_VTR_EL2
	/* ICH_VTR_EL2 描述 GIC 虚拟化接口能力，经 x1 借给 EL1 调用者读取。 */
	b	9f

2:	cmp	x0, #HVC_SOFT_RESTART
	b.ne	3f
	mov	x0, x2
	mov	x2, x4
	mov	x4, x1
	mov	x1, x3
	br	x4				// no return
	/* 软重启按约定重排参数并 BR 到 x1 原先给出的入口；不写 LR，也不会回到 stub。 */

3:	cmp	x0, #HVC_RESET_VECTORS
	beq	9f				// Nothing to reset!
	/* stub 自身没有额外向量状态可恢复，RESET 在此实现为成功的空操作。 */

	/* Someone called kvm_call_hyp() against the hyp-stub... */
	/* 完整 KVM 调用错误地落到占位 stub：返回专用错误，禁止把未知功能误报为成功。 */
	mov_q	x0, HVC_STUB_ERR
	eret

9:	mov	x0, xzr
	/* 支持的普通功能统一以 x0=0 表示成功，ERET 恢复 HVC 前的 PC/PSTATE。 */
	eret
SYM_CODE_END(elx_sync)

SYM_CODE_START_LOCAL(__finalise_el2)
	/*
	 * HVC_FINALISE_EL2 的 EL2 内部实现。入口由 EL1 finalise_el2() 发起，CPU 当前在
	 * stub EL2 异常上下文，x0 不再需要保留。目标是在确认 MMU 关闭、CPU 支持 VHE
	 * 且策略允许内核运行于 VHE 后，把 EL12 视图中的栈、异常向量、页表和权限状态
	 * 搬到 VHE host 使用的寄存器，并伪造一次返回到 EL2h 的 ERET。失败以
	 * HVC_STUB_ERR 返回原 EL1，不产生部分可用的 VHE 环境。
	 */
	finalise_el2_state
	/* 宏先收尾通用 EL2 基线状态；后续只处理从 nVHE 风格 EL1 提升为 VHE 的迁移。 */

	// nVHE? No way! Give me the real thing!
	// 此路径不建立 nVHE host；只有完整 VHE 条件满足才继续。
	// Sanity check: MMU *must* be off
	// 切换寄存器解释和翻译级时 EL2 MMU 必须关闭，否则活动翻译可能按新旧语义混用。
	mrs	x1, sctlr_el2
	tbnz	x1, #0, 1f

	// Needs to be VHE capable, obviously
	// 读取带早期 override 的 VHE 能力；不支持或被策略禁用就跳到失败出口 1:。
	check_override id_aa64mmfr1 ID_AA64MMFR1_EL1_VH_SHIFT 0f 1f x1 x2

0:	// Check whether we only want the hypervisor to run VHE, not the kernel
	// HVHE override 可要求仅 hypervisor 使用 VHE、内核仍留在 EL1；该策略下拒绝提升。
	adr_l	x1, arm64_sw_feature_override
	ldr	x2, [x1, FTR_OVR_VAL_OFFSET]
	ldr	x1, [x1, FTR_OVR_MASK_OFFSET]
	and	x2, x2, x1
	ubfx	x2, x2, #ARM64_SW_FEATURE_OVERRIDE_HVHE, #4
	cbz	x2, 2f

1:	mov_q	x0, HVC_STUB_ERR
	/* 失败返回保持调用者继续在 EL1；x0 明确说明 stub 未完成最终化。 */
	eret
2:
	// Engage the VHE magic!
	// 设置 E2H/TGE 等 host VHE 标志，使后续 EL1 名称访问采用 VHE host 寄存器语义。
	mov_q	x0, HCR_HOST_VHE_FLAGS
	msr_hcr_el2 x0

	// Use the EL1 allocated stack
	// EL1 已准备好可信启动栈；搬到当前 SP 后，VHE EL2 host 延续同一栈对象。
	mrs	x0, sp_el1
	mov	sp, x0

	// FP configuration, vectors
	// 把 EL12 视图中的 FP/SIMD 访问策略和异常向量复制到 VHE host 的 EL1 命名寄存器。
	mrs_s	x0, SYS_CPACR_EL12
	msr	cpacr_el1, x0
	mrs_s	x0, SYS_VBAR_EL12
	msr	vbar_el1, x0

	// Use EL2 translations for SPE & TRBE and disable access from EL1
	/*
	 * 清 E2PB/E2TB 选择 EL2 翻译控制 SPE/TRBE 缓冲区，并阻止下级 EL1 直接访问；
	 * VHE host 因而统一拥有这些 profiling/trace 资源的翻译与隔离责任。
	 */
	mrs	x0, mdcr_el2
	bic	x0, x0, #MDCR_EL2_E2PB_MASK
	bic	x0, x0, #MDCR_EL2_E2TB_MASK
	msr	mdcr_el2, x0

	// Transfer the MM state from EL1 to EL2
	/*
	 * 把内核在 EL1 已建立的 TCR、TTBR0/1、MAIR 搬入 VHE host 当前使用的寄存器。
	 * 这些寄存器共同描述地址宽度、根页表和内存属性，必须作为一组迁移后再启用 MMU。
	 */
	mrs_s	x0, SYS_TCR_EL12
	msr	tcr_el1, x0
	mrs_s	x0, SYS_TTBR0_EL12
	msr	ttbr0_el1, x0
	mrs_s	x0, SYS_TTBR1_EL12
	msr	ttbr1_el1, x0
	mrs_s	x0, SYS_MAIR_EL12
	msr	mair_el1, x0
	mrs	x1, REG_ID_AA64MMFR3_EL1
	/* TCR2 是可选扩展；只有特性字段非零时访问，避免在不实现的 CPU 上陷入异常。 */
	ubfx	x1, x1, #ID_AA64MMFR3_EL1_TCRX_SHIFT, #4
	cbz	x1, .Lskip_tcr2
	mrs	x0, REG_TCR2_EL12
	msr	REG_TCR2_EL1, x0

	// Transfer permission indirection state
	/*
	 * 若实现 stage-1 permission indirection，同步搬迁 PIRE0/PIR；否则旧 EL1 页表的
	 * 间接权限索引会在 VHE host 下引用未初始化寄存器，产生错误访问权限。
	 */
	mrs	x1, REG_ID_AA64MMFR3_EL1
	ubfx	x1, x1, #ID_AA64MMFR3_EL1_S1PIE_SHIFT, #4
	cbz	x1, .Lskip_indirection
	mrs	x0, REG_PIRE0_EL12
	msr	REG_PIRE0_EL1, x0
	mrs	x0, REG_PIR_EL12
	msr	REG_PIR_EL1, x0

.Lskip_indirection:
.Lskip_tcr2:

	/* 系统寄存器迁移完成后以 ISB 丢弃旧执行上下文，后续指令按新的 VHE 状态解释。 */
	isb

	// Hack the exception return to stay at EL2
	/*
	 * 修改保存的返回 PSTATE 模式为 EL2h，使最终 ERET 不再降回 EL1，而是在 EL2
	 * 继续原调用链。这是从“临时陷入 stub”变成“内核常驻 VHE”的提交意图。
	 */
	mrs	x0, spsr_el1
	and	x0, x0, #~PSR_MODE_MASK
	mov	x1, #PSR_MODE_EL2h
	orr	x0, x0, x1
	msr	spsr_el1, x0

	b	enter_vhe
	/* 尾跳到 idmap 代码完成 MMU 所有权切换；此后不再返回本段。 */
SYM_CODE_END(__finalise_el2)

	// At the point where we reach enter_vhe(), we run with
	// the MMU off (which is enforced by __finalise_el2()).
	// We thus need to be in the idmap, or everything will
	// explode when enabling the MMU.
	/*
	 * 到达 enter_vhe() 时 MMU 必须关闭，这是 __finalise_el2() 强制检查的不变量。
	 * 因此代码必须放在 VA==PA 的 idmap 中；若仍从普通高地址文本执行，启用新 MMU
	 * 前后的取指地址无法连续解析，CPU 会在尚无可用异常环境时崩溃。
	 */

	.pushsection	.idmap.text, "ax"
	/* 临时切到恒等映射代码节；enter_vhe 完成后 popsection 回到 .hyp.text。 */

SYM_CODE_START_LOCAL(enter_vhe)
	/*
	 * VHE 提交阶段，位于 .idmap.text。入口已有迁移后的 EL2 host 页表寄存器、可信栈
	 * 和被修改为 EL2h 的返回状态；无参数、无普通返回。成功后 ERET 回原调用链但仍
	 * 在 EL2，x0=0；期间 MMU/TLB 状态不允许并发观察或调用 C。
	 */
	// Invalidate TLBs before enabling the MMU
	// 启用迁移后的 EL2 stage-1 前清全部 EL1/host 翻译，避免复用 EL1 时期的旧 TLB 项。
	tlbi	vmalle1
	dsb	nsh
	isb

	// Enable the EL2 S1 MMU, as set up from EL1
	// 读取先前 EL12 的 SCTLR 配置，并通过 helper 作为当前 VHE host SCTLR 安全启用 MMU。
	mrs_s	x0, SYS_SCTLR_EL12
	set_sctlr_el1	x0

	// Disable the EL1 S1 MMU for a good measure
	/*
	 * 再把真正下级 EL1 的 stage-1 置为 MMU-off 基线，避免未来 guest/下级上下文意外
	 * 继承 host 早期设置；host 的活动翻译此时由 EL2 VHE 语义拥有。
	 */
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF
	msr_s	SYS_SCTLR_EL12, x0

	mov	x0, xzr
	/* 成功状态放入 x0，ERET 使用已改写模式回到 EL2h 的内核后续地址。 */

	eret
SYM_CODE_END(enter_vhe)

	.popsection

.macro invalid_vector	label
	/*
	 * 为一个不受支持的向量生成同名自旋入口。异常已经发生且 stub 没有完整现场、
	 * 中断控制器或恢复策略，永久分支到自身比带着未知状态 ERET 更可诊断、更安全。
	 */
SYM_CODE_START_LOCAL(\label)
	b \label
SYM_CODE_END(\label)
.endm

	invalid_vector	el2_sync_invalid
	invalid_vector	el2_irq_invalid
	invalid_vector	el2_fiq_invalid
	invalid_vector	el2_error_invalid
	invalid_vector	el1_sync_invalid
	invalid_vector	el1_irq_invalid
	invalid_vector	el1_fiq_invalid
	invalid_vector	el1_error_invalid
	/* 上述八个符号分别被多个向量槽复用；任何到达都表示启动/路由不变量已破坏。 */

	.popsection

/*
 * __hyp_set_vectors: Call this after boot to set the initial hypervisor
 * vectors as part of hypervisor installation.  On an SMP system, this should
 * be called on each CPU.
 *
 * x0 must be the physical address of the new vector table, and must be
 * 2KB aligned.
 *
 * Before calling this, you must check that the stub hypervisor is installed
 * everywhere, by waiting for any secondary CPUs to be brought up and then
 * checking that is_hyp_mode_available() is true.
 *
 * If not, there is a pre-existing hypervisor, some CPUs failed to boot, or
 * something else went wrong... in such cases, trying to install a new
 * hypervisor is unlikely to work as desired.
 *
 * When you call into your shiny new hypervisor, sp_el2 will contain junk,
 * so you will need to set that to something sensible at the new hypervisor's
 * initialisation entry point.
 */
/*
 * __hyp_set_vectors() - 在完整 hypervisor 安装阶段替换当前 CPU 的 EL2 向量表。
 *
 * 原文要求 SMP 系统在每个 CPU 调用；@x0 必须是新向量表的 2 KiB 对齐物理地址，
 * 只借给 stub 写入 VBAR_EL2，不转移内存所有权。调用前必须等待次级 CPU 启动并确认
 * is_hyp_mode_available()，以证明所有 CPU 都装有兼容 stub；否则可能已有外部
 * hypervisor、CPU 启动失败或 EL2 状态不一致，继续安装无法形成全系统一致环境。
 *
 * 新 hypervisor 首次进入时 SP_EL2 仍是无意义值，必须在自己的初始化入口设置可信
 * 栈。函数运行在调用 CPU，不能代替跨 CPU 协调；stub ABI 把 x0 置 0，但 C 声明
 * 为 void，调用者不把该寄存器当可检查返回值。
 */

SYM_FUNC_START(__hyp_set_vectors)
	/* HVC ABI 以 x0 放功能号，所以先把调用者的向量地址移到参数 x1。 */
	mov	x1, x0
	mov	x0, #HVC_SET_VECTORS
	hvc	#0
	ret
SYM_FUNC_END(__hyp_set_vectors)

/*
 * __hyp_reset_vectors() - 请求当前 EL2 实现撤销/重置临时向量安装。
 *
 * 入参：无；C 级返回为 void，stub 虽把 x0 置 0，调用者也不消费它。对本 stub 而言
 * RESET 是成功空操作，因为没有额外状态可恢复；完整 hypervisor 可按同一 ABI 提供
 * 实际行为。函数不持锁、不睡眠，调用方仍需逐 CPU 协调，不能把一次 HVC 当广播。
 */
SYM_FUNC_START(__hyp_reset_vectors)
	mov	x0, #HVC_RESET_VECTORS
	hvc	#0
	ret
SYM_FUNC_END(__hyp_reset_vectors)

/*
 * Entry point to finalise EL2 and switch to VHE if deemed capable
 *
 * w0: boot mode, as returned by init_kernel_el()
 */
/*
 * finalise_el2() - 启动末期按条件请求 stub 把内核提升为 VHE EL2 host。
 *
 * head.S 的主 CPU和次级 CPU 路径在 init_kernel_el() 后调用。w0 是该函数返回的
 * boot mode，只读且仅 BOOT_CPU_MODE_EL2 有资格继续；函数还要求当前仍在 EL1，
 * 因为协议需要从 EL1 发 HVC 进入先前安装的 EL2 stub。条件不满足时安静返回，不是
 * 错误；满足时 HVC_FINALISE_EL2 可能成功迁移到 VHE，也可能在 x0 留下 stub 错误，
 * 但本薄包装不解析它。无内存 ownership、无锁、不可睡眠，所有效果均为本 CPU 的
 * 异常级和系统寄存器状态变化。
 */
SYM_FUNC_START(finalise_el2)
	// Need to have booted at EL2
	// 只有固件最初把 CPU 交给 EL2，才存在可响应本 HVC 的内核 stub。
	cmp	w0, #BOOT_CPU_MODE_EL2
	b.ne	1f

	// and still be at EL1
	// 此刻还必须运行于 EL1；已经在 EL2 或其他级别都不应再次执行提升协议。
	mrs	x0, CurrentEL
	cmp	x0, #CurrentEL_EL1
	b.ne	1f

	mov	x0, #HVC_FINALISE_EL2
	/* HVC 触发同步异常进入 elx_sync；成功可能以 VHE EL2 身份继续到下一条指令。 */
	hvc	#0
1:
	ret
SYM_FUNC_END(finalise_el2)
