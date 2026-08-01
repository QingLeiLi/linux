/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arm64 EL2 早期寄存器初始化宏学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 * 本汇编专用头由 head.S 与 hyp-stub.S 展开：CPU 从固件以 EL2 启动后，先把 HCR、
 * SCTLR、timer、debug、GIC、stage-2 和细粒度 trap 等寄存器归一化为安全的 nVHE
 * 基线；稍后若 VHE 可用，hyp-stub 再迁移内核状态。finalise_el2_state 则在能力和
 * command-line override 已确定后开放 MPAM、GCS、SVE、SME。
 *
 * 宏只改变当前 CPU 系统寄存器，没有内存对象 ownership、锁或错误返回；调用点处于
 * 极早期不可睡眠环境，每个 CPU 都必须执行。所有 feature register 检查都在访问
 * 可选寄存器之前，避免未实现访问；ISB 是系统寄存器写入对后续执行生效的提交边界。
 * x0-x2 是统一 scratch/clobber 契约，\@ 让每次宏展开的局部标签保持唯一。
 */
/*
 * Copyright (C) 2012,2013 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 */

#ifndef __ARM_KVM_INIT_H__
#define __ARM_KVM_INIT_H__

#ifndef __ASSEMBLER__
#error Assembly-only header
#endif
/* 该头含 .macro 和寄存器指令，若被 C 编译单元包含就在预处理期报错。 */

#include <asm/kvm_arm.h>
#include <asm/ptrace.h>
#include <asm/sysreg.h>
#include <linux/irqchip/arm-gic-v3.h>

/*
 * 以 @val 为 nVHE HCR 基线，并探测 E2H 是否实际为 RES1。输入是汇编常量，x0/x1
 * 被破坏；无返回值。结果保证以后读取 HCR.E2H 能真实区分 nVHE 与强制 VHE-only。
 */
.macro init_el2_hcr	val
	mov_q	x0, \val

	/*
	 * Compliant CPUs advertise their VHE-onlyness with
	 * ID_AA64MMFR4_EL1.E2H0 < 0. On such CPUs HCR_EL2.E2H is RES1, but it
	 * can reset into an UNKNOWN state and might not read as 1 until it has
	 * been initialized explicitly.
	 * Initialize HCR_EL2.E2H so that later code can rely upon HCR_EL2.E2H
	 * indicating whether the CPU is running in E2H mode.
	 */
	/*
	 * 合规 VHE-only CPU 以 E2H0<0 声明 HCR.E2H 为 RES1，但复位值可能 UNKNOWN；必须
	 * 显式置 E2H，后续代码才可把读回值当运行模式依据。
	 */
	mrs_s	x1, SYS_ID_AA64MMFR4_EL1
	sbfx	x1, x1, #ID_AA64MMFR4_EL1_E2H0_SHIFT, #ID_AA64MMFR4_EL1_E2H0_WIDTH
	cmp	x1, #0
	b.lt	.LnE2H0_\@

	/*
	 * Unfortunately, HCR_EL2.E2H can be RES1 even if not advertised
	 * as such via ID_AA64MMFR4_EL1.E2H0:
	 *
	 * - Fruity CPUs predate the !FEAT_E2H0 relaxation, and seem to
	 *   have HCR_EL2.E2H implemented as RAO/WI.
	 *
	 * - On CPUs that lack FEAT_FGT, a hypervisor can't trap guest
	 *   reads of ID_AA64MMFR4_EL1 to advertise !FEAT_E2H0. NV
	 *   guests on these hosts can write to HCR_EL2.E2H without
	 *   trapping to the hypervisor, but these writes have no
	 *   functional effect.
	 *
	 * Handle both cases by checking for an essential VHE property
	 * (system register remapping) to decide whether we're
	 * effectively VHE-only or not.
	 */
	/*
	 * 还有两类未可靠声明 E2H0 的实现：较早“Fruity”CPU 把 E2H 做成 RAO/WI；缺 FGT
	 * 的宿主又无法向嵌套 guest 陷获并伪造该 ID。这里利用 VHE 的寄存器重映射性质：
	 * 先按 nVHE 写 HCR，再分别写 FAR_EL1/FAR_EL2；若 EL1 视图读到 EL2 的最新值，
	 * 两者实际已别名，说明只能按 VHE-only 处理，否则 nVHE 确实可工作。
	 */
	msr_hcr_el2 x0		// Setup HCR_EL2 as nVHE
	/* 先尝试发布调用者给出的 nVHE HCR 基线。 */
	mov	x1, #1		// Write something to FAR_EL1
	msr	far_el1, x1
	isb
	mov	x1, #2		// Try to overwrite it via FAR_EL2
	msr	far_el2, x1
	isb
	mrs	x1, far_el1	// If we see the latest write in FAR_EL1,
	cmp	x1, #2		// we can safely assume we are VHE only.
	b.ne	.LnVHE_\@	// Otherwise, we know that nVHE works.
	/* 读回非 2 表示没有重映射，nVHE 可用；等于 2 则转而显式置 HCR_E2H。 */

.LnE2H0_\@:
	orr	x0, x0, #HCR_E2H
	msr_hcr_el2 x0
.LnVHE_\@:
.endm

/* 把 SCTLR_EL2 设为架构规定的 MMU-off 安全基线并 ISB；x0 被破坏，无返回。 */
.macro __init_el2_sctlr
	mov_q	x0, INIT_SCTLR_EL2_MMU_OFF
	msr	sctlr_el2, x0
	isb
.endm

/*
 * 若实现 HCRX_EL2，则开放内存复制、TCR2、扩展 FP；再按 GCS/LS64 能力开放对应
 * 指令。未实现时完全跳过寄存器访问。x0/x1 被破坏，无错误出口。
 */
.macro __init_el2_hcrx
	mrs	x0, id_aa64mmfr1_el1
	ubfx	x0, x0, #ID_AA64MMFR1_EL1_HCX_SHIFT, #4
	cbz	x0, .Lskip_hcrx_\@
	mov_q	x0, (HCRX_EL2_MSCEn | HCRX_EL2_TCR2En | HCRX_EL2_EnFPM)

        /* Enable GCS if supported */
	/* 仅 GCS feature 非零时设置 GCSEn，避免向未实现功能发布虚假可用状态。 */
	mrs_s	x1, SYS_ID_AA64PFR1_EL1
	ubfx	x1, x1, #ID_AA64PFR1_EL1_GCS_SHIFT, #4
	cbz	x1, .Lskip_gcs_hcrx_\@
	orr	x0, x0, #HCRX_EL2_GCSEn

.Lskip_gcs_hcrx_\@:
	/* Enable LS64, LS64_V if supported */
	/* LS64 开放原子 64 字节 load/store；达到 LS64_V 级别时再开放额外 store-release。 */
	mrs_s	x1, SYS_ID_AA64ISAR1_EL1
	ubfx	x1, x1, #ID_AA64ISAR1_EL1_LS64_SHIFT, #4
	cbz	x1, .Lset_hcrx_\@
	orr	x0, x0, #HCRX_EL2_EnALS
	cmp	x1, #ID_AA64ISAR1_EL1_LS64_LS64_V
	b.lt	.Lset_hcrx_\@
	orr	x0, x0, #HCRX_EL2_EnASR

.Lset_hcrx_\@:
	msr_s	SYS_HCRX_EL2, x0
.Lskip_hcrx_\@:
.endm

/* Check if running in host at EL2 mode, i.e., (h)VHE. Jump to fail if not. */
/*
 * 读取 HCR.E2H 判断当前是否为 VHE/hVHE host；为 0 跳 @fail。@tmp 是输出 scratch，
 * 宏不保留其旧值，也不改变其他状态。
 */
.macro __check_hvhe fail, tmp
	mrs	\tmp, hcr_el2
	and	\tmp, \tmp, #HCR_E2H
	cbz	\tmp, \fail
.endm

/*
 * Allow Non-secure EL1 and EL0 to access physical timer and counter.
 * This is not necessary for VHE, since the host kernel runs in EL2,
 * and EL0 accesses are configured in the later stage of boot process.
 * Note that when HCR_EL2.E2H == 1, CNTHCTL_EL2 has the same bit layout
 * as CNTKCTL_EL1, and CNTKCTL_EL1 accessing instructions are redefined
 * to access CNTHCTL_EL2. This allows the kernel designed to run at EL1
 * to transparently mess with the EL0 bits via CNTKCTL_EL1 access in
 * EL2.
 */
/*
 * nVHE 下开放非安全 EL1/EL0 的物理 timer/counter 并清 CNTVOFF。VHE host 稍后通过
 * 重映射后的 CNTKCTL_EL1 配置 EL0，因此这里按 E2H 调整位位置，避免错误开放；
 * x0/x1 被破坏，当前 CPU timer 可见性是唯一副作用。
 */
.macro __init_el2_timers
	mov	x0, #3				// Enable EL1 physical timers
	__check_hvhe .LnVHE_\@, x1
	lsl	x0, x0, #10
.LnVHE_\@:
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr		// Clear virtual offset
	/* 虚拟计数偏移归零，防止继承固件/旧 hypervisor 留下的 guest 时间偏置。 */
.endm

/* Branch to skip_label if SPE version is less than given version */
/* 从 ID_AA64DFR0 取 SPE 版本，低于 @version 跳 @skip_label；@tmp 被完全覆盖。 */
.macro __spe_vers_imp skip_label, version, tmp
    mrs    \tmp, id_aa64dfr0_el1
    ubfx   \tmp, \tmp, #ID_AA64DFR0_EL1_PMSVer_SHIFT, #4
    cmp    \tmp, \version
    b.lt   \skip_label
.endm

/*
 * 初始化 PMU、SPE 和 TRBE 的 EL2 trap/translation 归属。x0-x2 被破坏；不存在返回
 * 错误，缺失或 implementation-defined 的特性按跳过处理，最终一次写 MDCR_EL2。
 */
.macro __init_el2_debug
	mrs	x1, id_aa64dfr0_el1
	ubfx	x0, x1, #ID_AA64DFR0_EL1_PMUVer_SHIFT, #4
	cmp	x0, #ID_AA64DFR0_EL1_PMUVer_NI
	ccmp	x0, #ID_AA64DFR0_EL1_PMUVer_IMP_DEF, #4, ne
	b.eq	.Lskip_pmu_\@			// Skip if no PMU present or IMP_DEF
	/* 无 PMU或实现自定义版本时跳过计数器开放，避免解释未知 PMCR 格式。 */
	mrs	x0, pmcr_el0			// Disable debug access traps
	ubfx	x0, x0, #11, #5			// to EL2 and allow access to
.Lskip_pmu_\@:
	csel	x2, xzr, x0, eq			// all PMU counters from EL1
	/* x2 汇总 MDCR：有标准 PMU 时按 PMCR.N 开放全部计数器，否则从零基线开始。 */

	/* Statistical profiling */
	/* SPE 不存在则整段跳过；存在时按 PMBIDR.P 判断是否可配置 EL2 profiling 控制。 */
	__spe_vers_imp .Lskip_spe_\@, ID_AA64DFR0_EL1_PMSVer_IMP, x0 // Skip if SPE not present

	mrs_s	x0, SYS_PMBIDR_EL1              // If SPE available at EL2,
	and	x0, x0, #(1 << PMBIDR_EL1_P_SHIFT)
	cbnz	x0, .Lskip_spe_el2_\@		// then permit sampling of physical
	mov	x0, #(1 << PMSCR_EL2_PCT_SHIFT | \
		      1 << PMSCR_EL2_PA_SHIFT)
	msr_s	SYS_PMSCR_EL2, x0		// addresses and physical counter
	/* 条件允许时开放物理地址和物理计数器采样；否则保留固件/架构基线。 */
.Lskip_spe_el2_\@:
	mov	x0, #MDCR_EL2_E2PB_MASK
	orr	x2, x2, x0			// If we don't have VHE, then
						// use EL1&0 translation.
	/* nVHE 基线选择 EL1&0 翻译处理 profiling buffer；VHE 最终化时可再迁移。 */

.Lskip_spe_\@:
	/* Trace buffer */
	/* TRBE 同样先查 feature，再查是否由 EL2 独占；仅可下放时设置 E2TB 选择 EL1&0。 */
	ubfx	x0, x1, #ID_AA64DFR0_EL1_TraceBuffer_SHIFT, #4
	cbz	x0, .Lskip_trace_\@		// Skip if TraceBuffer is not present

	mrs_s	x0, SYS_TRBIDR_EL1
	and	x0, x0, TRBIDR_EL1_P
	cbnz	x0, .Lskip_trace_\@		// If TRBE is available at EL2

	mov	x0, #MDCR_EL2_E2TB_MASK
	orr	x2, x2, x0			// allow the EL1&0 translation
						// to own it.

.Lskip_trace_\@:
	msr	mdcr_el2, x2			// Configure debug traps
	/* 汇总后的 PMU/SPE/TRBE 策略在此发布到 MDCR_EL2。 */
.endm

/* LORegions */
/* 若实现 LORegions，清 LORC_EL1 关闭遗留区域控制；x0/x1 被破坏，缺特性时不访问。 */
.macro __init_el2_lor
	mrs	x1, id_aa64mmfr1_el1
	ubfx	x0, x1, #ID_AA64MMFR1_EL1_LO_SHIFT, 4
	cbz	x0, .Lskip_lor_\@
	msr_s	SYS_LORC_EL1, xzr
.Lskip_lor_\@:
.endm

/* Stage-2 translation */
/* 清 VTTBR_EL2，撤销固件遗留的 guest stage-2 根/VMID；无参数，保留 x0-x2。 */
.macro __init_el2_stage2
	msr	vttbr_el2, xzr
.endm

/* GICv3 system register access */
/*
 * 若 CPU 声明 GIC system-register interface，则启用 ICC_SRE_EL2 并读回确认实现接受；
 * 成功后清 ICH_HCR_EL2 虚拟中断控制，失败/缺特性直接跳过。x0 被破坏，无错误返回。
 */
.macro __init_el2_gicv3
	mrs	x0, id_aa64pfr0_el1
	ubfx	x0, x0, #ID_AA64PFR0_EL1_GIC_SHIFT, #4
	cbz	x0, .Lskip_gicv3_\@

	mrs_s	x0, SYS_ICC_SRE_EL2
	orr	x0, x0, #ICC_SRE_EL2_SRE	// Set ICC_SRE_EL2.SRE==1
	orr	x0, x0, #ICC_SRE_EL2_ENABLE	// Set ICC_SRE_EL2.Enable==1
	/* SRE 选择系统寄存器接口，ENABLE 允许下级使用；两位需作为同一策略写回。 */
	msr_s	SYS_ICC_SRE_EL2, x0
	isb					// Make sure SRE is now set
	/* ISB 后读回 SRE，只有 bit0 真正保持为 1 才能安全访问后续 ICH 寄存器。 */
	mrs_s	x0, SYS_ICC_SRE_EL2		// Read SRE back,
	tbz	x0, #0, .Lskip_gicv3_\@		// and check that it sticks
	msr_s	SYS_ICH_HCR_EL2, xzr		// Reset ICH_HCR_EL2 to defaults
	/* 清虚拟 GIC 控制，避免固件遗留 enable/trap 状态影响 Linux。 */
.Lskip_gicv3_\@:
.endm

/* GICv5 system register access */
/*
 * GICv5 GCIE 存在时关闭一组 instruction/read/write trap，并启用 vHPPI 选择，使下级
 * GIC CPU interface 按 Linux 预期直接工作。x0 被破坏；位图是成组策略而非资源分配。
 */
.macro __init_el2_gicv5
	mrs_s	x0, SYS_ID_AA64PFR2_EL1
	ubfx	x0, x0, #ID_AA64PFR2_EL1_GCIE_SHIFT, #4
	cbz	x0, .Lskip_gicv5_\@

	mov	x0, #(ICH_HFGITR_EL2_GICRCDNMIA		| \
		      ICH_HFGITR_EL2_GICRCDIA		| \
		      ICH_HFGITR_EL2_GICCDDI		| \
		      ICH_HFGITR_EL2_GICCDEOI		| \
		      ICH_HFGITR_EL2_GICCDHM		| \
		      ICH_HFGITR_EL2_GICCDRCFG		| \
		      ICH_HFGITR_EL2_GICCDPEND		| \
		      ICH_HFGITR_EL2_GICCDAFF		| \
		      ICH_HFGITR_EL2_GICCDPRI		| \
		      ICH_HFGITR_EL2_GICCDDIS		| \
		      ICH_HFGITR_EL2_GICCDEN)
	msr_s	SYS_ICH_HFGITR_EL2, x0		// Disable instruction traps
	/* HFGITR 中相应允许位一次开放列出的 GIC 指令。 */
	mov_q	x0, (ICH_HFGRTR_EL2_ICC_PPI_ACTIVERn_EL1	| \
		     ICH_HFGRTR_EL2_ICC_PPI_PRIORITYRn_EL1	| \
		     ICH_HFGRTR_EL2_ICC_PPI_PENDRn_EL1		| \
		     ICH_HFGRTR_EL2_ICC_PPI_ENABLERn_EL1	| \
		     ICH_HFGRTR_EL2_ICC_PPI_HMRn_EL1		| \
		     ICH_HFGRTR_EL2_ICC_IAFFIDR_EL1		| \
		     ICH_HFGRTR_EL2_ICC_ICSR_EL1		| \
		     ICH_HFGRTR_EL2_ICC_PCR_EL1			| \
		     ICH_HFGRTR_EL2_ICC_HPPIR_EL1		| \
		     ICH_HFGRTR_EL2_ICC_CR0_EL1			| \
		     ICH_HFGRTR_EL2_ICC_IDRn_EL1		| \
		     ICH_HFGRTR_EL2_ICC_APR_EL1)
	msr_s	SYS_ICH_HFGRTR_EL2, x0		// Disable reg read traps
	/* HFGRTR 开放列出的 ICC PPI/priority/pending/control 寄存器读取。 */
	mov_q	x0, (ICH_HFGWTR_EL2_ICC_PPI_ACTIVERn_EL1	| \
		     ICH_HFGWTR_EL2_ICC_PPI_PRIORITYRn_EL1	| \
		     ICH_HFGWTR_EL2_ICC_PPI_PENDRn_EL1		| \
		     ICH_HFGWTR_EL2_ICC_PPI_ENABLERn_EL1	| \
		     ICH_HFGWTR_EL2_ICC_ICSR_EL1		| \
		     ICH_HFGWTR_EL2_ICC_PCR_EL1			| \
		     ICH_HFGWTR_EL2_ICC_CR0_EL1			| \
		     ICH_HFGWTR_EL2_ICC_APR_EL1)
	msr_s	SYS_ICH_HFGWTR_EL2, x0		// Disable reg write traps
	/* HFGWTR 对应开放写访问；只列出架构允许下放的寄存器集合。 */
	mov	x0, #(ICH_VCTLR_EL2_En)
	msr_s	SYS_ICH_VCTLR_EL2, x0		// Enable vHPPI selection
	/* 最后启用虚拟高优先级 PPI 选择，之前 trap 策略已完整就绪。 */
.Lskip_gicv5_\@:
.endm

/* 清 HSTR，避免 AArch32 CP15 访问继承固件 trap；无参数和返回。 */
.macro __init_el2_hstr
	msr	hstr_el2, xzr			// Disable CP15 traps to EL2
.endm

/* Virtual CPU ID registers */
/* 把当前 CPU MIDR/MPIDR 复制为默认 guest 可见 VPIDR/VMPIDR；x0/x1 被破坏。 */
.macro __init_el2_nvhe_idregs
	mrs	x0, midr_el1
	mrs	x1, mpidr_el1
	msr	vpidr_el2, x0
	msr	vmpidr_el2, x1
.endm

/* Coprocessor traps */
/*
 * VHE/hVHE 通过 CPACR_EL1 开放 FP/SIMD；nVHE 清 CPTR_EL2 的通用 trap 基线。该宏
 * 只设置初始访问策略，具体 guest trap 由 KVM 切换；x0/x1 被破坏。
 */
.macro __init_el2_cptr
	__check_hvhe .LnVHE_\@, x1
	mov	x0, #CPACR_EL1_FPEN
	msr	cpacr_el1, x0
	b	.Lskip_set_cptr_\@
.LnVHE_\@:
	mov	x0, #0x33ff
	msr	cptr_el2, x0			// Disable copro. traps to EL2
	/* 0x33ff 是 nVHE 安全基线，关闭相关协处理器向 EL2 的陷获。 */
.Lskip_set_cptr_\@:
.endm

/*
 * Configure BRBE to permit recording cycle counts and branch mispredicts.
 *
 * At any EL, to record cycle counts BRBE requires that both BRBCR_EL2.CC=1 and
 * BRBCR_EL1.CC=1.
 *
 * At any EL, to record branch mispredicts BRBE requires that both
 * BRBCR_EL2.MPRED=1 and BRBCR_EL1.MPRED=1.
 *
 * Set {CC,MPRED} in BRBCR_EL2 in case nVHE mode is used and we are
 * executing in EL1.
 */
/*
 * BRBE 要记录周期和分支误预测，EL1/EL2 两级控制位必须同时允许；此处仅在实现 BRBE
 * 时设置 EL2 的 CC/MPRED，为 nVHE 的 EL1 内核补齐上级许可。x0/x1 被破坏。
 */
.macro __init_el2_brbe
	mrs	x1, id_aa64dfr0_el1
	ubfx	x1, x1, #ID_AA64DFR0_EL1_BRBE_SHIFT, #4
	cbz	x1, .Lskip_brbe_\@

	mov_q	x0, BRBCR_ELx_CC | BRBCR_ELx_MPRED
	msr_s	SYS_BRBCR_EL2, x0
.Lskip_brbe_\@:
.endm

/* Disable any fine grained traps */
/*
 * __init_el2_fgt 按 SPE、BRBE、SME、PIE、POE、GCS、AMU 能力构造第一代 fine-grained
 * trap 允许位。FGT 寄存器多采用 nX=1 表示“不陷获”，所以置位是开放而非加 trap；
 * x0-x2 被破坏。未实现 FGT 时整段跳过，绝不访问可选寄存器。
 */
.macro __init_el2_fgt
	mrs	x1, id_aa64mmfr0_el1
	ubfx	x1, x1, #ID_AA64MMFR0_EL1_FGT_SHIFT, #4
	cbz	x1, .Lskip_fgt_\@

	mov	x0, xzr
	mov	x2, xzr
	/* If SPEv1p2 is implemented, */
	/* SPE v1.2 才有 PMSNEVFR，存在时同时开放其读写。 */
	__spe_vers_imp .Lskip_spe_fgt_\@, #ID_AA64DFR0_EL1_PMSVer_V1P2, x1
	/* Disable PMSNEVFR_EL1 read and write traps */
	/* nPMSNEVFR 位分别加入 debug read/write trap 位图。 */
	orr	x0, x0, #HDFGRTR_EL2_nPMSNEVFR_EL1_MASK
	orr	x2, x2, #HDFGWTR_EL2_nPMSNEVFR_EL1_MASK

.Lskip_spe_fgt_\@:
	mrs	x1, id_aa64dfr0_el1
	ubfx	x1, x1, #ID_AA64DFR0_EL1_BRBE_SHIFT, #4
	cbz	x1, .Lskip_brbe_fgt_\@

	/*
	 * Disable read traps for the following registers
	 *
	 * [BRBSRC|BRBTGT|RBINF]_EL1
	 * [BRBSRCINJ|BRBTGTINJ|BRBINFINJ|BRBTS]_EL1
	 */
	/* BRBE 数据与注入/时间戳寄存器读取直接下放给 EL1。 */
	orr	x0, x0, #HDFGRTR_EL2_nBRBDATA_MASK

	/*
	 * Disable write traps for the following registers
	 *
	 * [BRBSRCINJ|BRBTGTINJ|BRBINFINJ|BRBTS]_EL1
	 */
	/* 写侧仅开放注入和时间戳寄存器，不把只读捕获数据错误地列入。 */
	orr	x2, x2, #HDFGWTR_EL2_nBRBDATA_MASK

	/* Disable read and write traps for [BRBCR|BRBFCR]_EL1 */
	/* BRBE 控制寄存器读写成对开放。 */
	orr	x0, x0, #HDFGRTR_EL2_nBRBCTL_MASK
	orr	x2, x2, #HDFGWTR_EL2_nBRBCTL_MASK

	/* Disable read traps for BRBIDR_EL1 */
	/* BRBIDR 是能力只读寄存器，只需开放读取。 */
	orr	x0, x0, #HDFGRTR_EL2_nBRBIDR_MASK

.Lskip_brbe_fgt_\@:

.Lset_debug_fgt_\@:
	msr_s	SYS_HDFGRTR_EL2, x0
	msr_s	SYS_HDFGWTR_EL2, x2

	mov	x0, xzr
	mov	x2, xzr

	mrs	x1, id_aa64dfr0_el1
	ubfx	x1, x1, #ID_AA64DFR0_EL1_BRBE_SHIFT, #4
	cbz	x1, .Lskip_brbe_insn_fgt_\@

	/* Disable traps for BRBIALL instruction */
	/* 开放全量 invalidation 指令 BRBIALL。 */
	orr	x2, x2, #HFGITR_EL2_nBRBIALL_MASK

	/* Disable traps for BRBINJ instruction */
	/* 开放 branch-record injection 指令 BRBINJ。 */
	orr	x2, x2, #HFGITR_EL2_nBRBINJ_MASK

.Lskip_brbe_insn_fgt_\@:
	mrs	x1, id_aa64pfr1_el1
	ubfx	x1, x1, #ID_AA64PFR1_EL1_SME_SHIFT, #4
	cbz	x1, .Lskip_sme_fgt_\@

	/* Disable nVHE traps of TPIDR2 and SMPRI */
	/* SME 存在时允许 nVHE EL1 使用 TPIDR2 与 streaming priority。 */
	orr	x0, x0, #HFGRTR_EL2_nSMPRI_EL1_MASK
	orr	x0, x0, #HFGRTR_EL2_nTPIDR2_EL0_MASK

.Lskip_sme_fgt_\@:
	mrs_s	x1, SYS_ID_AA64MMFR3_EL1
	ubfx	x1, x1, #ID_AA64MMFR3_EL1_S1PIE_SHIFT, #4
	cbz	x1, .Lskip_pie_fgt_\@

	/* Disable trapping of PIR_EL1 / PIRE0_EL1 */
	/* PIE 存在时开放两个 permission indirection 寄存器。 */
	orr	x0, x0, #HFGRTR_EL2_nPIR_EL1
	orr	x0, x0, #HFGRTR_EL2_nPIRE0_EL1

.Lskip_pie_fgt_\@:
	mrs_s	x1, SYS_ID_AA64MMFR3_EL1
	ubfx	x1, x1, #ID_AA64MMFR3_EL1_S1POE_SHIFT, #4
	cbz	x1, .Lskip_poe_fgt_\@

	/* Disable trapping of POR_EL0 */
	/* POE 存在时允许访问 EL0 permission overlay 寄存器。 */
	orr	x0, x0, #HFGRTR_EL2_nPOR_EL0

.Lskip_poe_fgt_\@:
	/* GCS depends on PIE so we don't check it if PIE is absent */
	/* 架构保证 GCS 依赖 PIE，因此无需把前面的 PIE 检查结果再作为 GCS 的额外分支。 */
	mrs_s	x1, SYS_ID_AA64PFR1_EL1
	ubfx	x1, x1, #ID_AA64PFR1_EL1_GCS_SHIFT, #4
	cbz	x1, .Lskip_gce_fgt_\@

	/* Disable traps of access to GCS registers at EL0 and EL1 */
	/* 两个 mask 分别覆盖 EL1 与 EL0 GCS 状态访问。 */
	orr	x0, x0, #HFGRTR_EL2_nGCS_EL1_MASK
	orr	x0, x0, #HFGRTR_EL2_nGCS_EL0_MASK

.Lskip_gce_fgt_\@:

.Lset_fgt_\@:
	msr_s	SYS_HFGRTR_EL2, x0
	msr_s	SYS_HFGWTR_EL2, x0
	msr_s	SYS_HFGITR_EL2, x2

	mrs	x1, id_aa64pfr0_el1		// AMU traps UNDEF without AMU
	/* 未实现 AMU 时读取其 trap 寄存器会 UNDEF，必须先检查 ID 字段。 */
	ubfx	x1, x1, #ID_AA64PFR0_EL1_AMU_SHIFT, #4
	cbz	x1, .Lskip_amu_fgt_\@

	msr_s	SYS_HAFGRTR_EL2, xzr

.Lskip_amu_fgt_\@:

.Lskip_fgt_\@:
.endm

/*
 * FGT2 存在时初始化第二组 trap：按能力开放 PMUv3.9 fixed counter 与 SPE FDS，
 * 其余第二代通用读写/指令 trap 寄存器清为 Linux 基线。x0/x1 被破坏；每个可选
 * 寄存器访问都先由版本位守护。
 */
.macro __init_el2_fgt2
	mrs	x1, id_aa64mmfr0_el1
	ubfx	x1, x1, #ID_AA64MMFR0_EL1_FGT_SHIFT, #4
	cmp	x1, #ID_AA64MMFR0_EL1_FGT_FGT2
	b.lt	.Lskip_fgt2_\@

	mov	x0, xzr
	mrs	x1, id_aa64dfr0_el1
	ubfx	x1, x1, #ID_AA64DFR0_EL1_PMUVer_SHIFT, #4
	cmp	x1, #ID_AA64DFR0_EL1_PMUVer_V3P9
	b.lt	.Lskip_pmuv3p9_\@

	orr	x0, x0, #HDFGRTR2_EL2_nPMICNTR_EL0
	orr	x0, x0, #HDFGRTR2_EL2_nPMICFILTR_EL0
	orr	x0, x0, #HDFGRTR2_EL2_nPMUACR_EL1
.Lskip_pmuv3p9_\@:
	/* If SPE is implemented, */
	/* 先确认 SPE 存在，才可读取 PMSIDR。 */
	__spe_vers_imp .Lskip_spefds_\@, ID_AA64DFR0_EL1_PMSVer_IMP, x1
	/* we can read PMSIDR and */
	/* PMSIDR.FDS 决定是否实现数据源过滤寄存器。 */
	mrs_s	x1, SYS_PMSIDR_EL1
	and	x1, x1,  #PMSIDR_EL1_FDS
	/* if FEAT_SPE_FDS is implemented, */
	/* 缺 FDS 时跳过 PMSDSFR 位，避免声明不存在的下级能力。 */
	cbz	x1, .Lskip_spefds_\@
	/* disable traps of PMSDSFR to EL2. */
	/* 存在时设置 nPMSDSFR，使 EL1 访问不再陷入 EL2。 */
	orr	x0, x0, #HDFGRTR2_EL2_nPMSDSFR_EL1

.Lskip_spefds_\@:
	msr_s   SYS_HDFGRTR2_EL2, x0
	msr_s   SYS_HDFGWTR2_EL2, x0
	msr_s   SYS_HFGRTR2_EL2, xzr
	msr_s   SYS_HFGWTR2_EL2, xzr
	msr_s   SYS_HFGITR2_EL2, xzr
.Lskip_fgt2_\@:
.endm

/**
 * Initialize EL2 registers to sane values. This should be called early on all
 * cores that were booted in EL2. Note that everything gets initialised as
 * if VHE was not available. The kernel context will be upgraded to VHE
 * if possible later on in the boot process
 *
 * Regs: x0, x1 and x2 are clobbered.
 */
/*
 * init_el2_state 是所有早期 EL2 CPU 的总编排宏。它按依赖顺序先关 MMU/设扩展控制，
 * 再归一化 timer、debug、stage-2、GIC、ID、协处理器与 trap；初始策略刻意按 nVHE
 * 建立，VHE 在后续 finalise 阶段升级。无参数和返回，x0-x2 全部被破坏；每 CPU
 * 执行一次，完成后固件遗留 EL2 状态不再影响 Linux。
 */
.macro init_el2_state
	__init_el2_sctlr
	__init_el2_hcrx
	__init_el2_timers
	__init_el2_debug
	__init_el2_brbe
	__init_el2_lor
	__init_el2_stage2
	__init_el2_gicv3
	__init_el2_gicv5
	__init_el2_hstr
	__init_el2_nvhe_idregs
	__init_el2_cptr
	__init_el2_fgt
	__init_el2_fgt2
.endm

#ifndef __KVM_NVHE_HYPERVISOR__
// This will clobber tmp1 and tmp2, and expect tmp1 to contain
// the id register value as read from the HW
	/*
	 * 普通内核版 __check_override：@tmp1 入口含硬件 ID，先抽取 @fld/@width；硬件不
	 * 支持直接 @fail。随后读取该 ID 的 override value/mask，mask 未覆盖时把硬件非零
	 * 视为通过，覆盖时以 override 值决定 @pass/@fail。tmp1/tmp2 均被破坏。
	 */
.macro __check_override idreg, fld, width, pass, fail, tmp1, tmp2
	ubfx	\tmp1, \tmp1, #\fld, #\width
	cbz	\tmp1, \fail

	adr_l	\tmp1, \idreg\()_override
	ldr	\tmp2, [\tmp1, FTR_OVR_VAL_OFFSET]
	ldr	\tmp1, [\tmp1, FTR_OVR_MASK_OFFSET]
	ubfx	\tmp2, \tmp2, #\fld, #\width
	ubfx	\tmp1, \tmp1, #\fld, #\width
	cmp	\tmp1, xzr
	and	\tmp2, \tmp2, \tmp1
	csinv	\tmp2, \tmp2, xzr, ne
	cbnz	\tmp2, \pass
	b	\fail
.endm

// This will clobber tmp1 and tmp2
	/* check_override 先读 @idreg_el1，再以固定 4 位字段调用上述核心判断。 */
.macro check_override idreg, fld, pass, fail, tmp1, tmp2
	mrs	\tmp1, \idreg\()_el1
	__check_override \idreg \fld 4 \pass \fail \tmp1 \tmp2
.endm
#else
// This will clobber tmp
	/*
	 * nVHE hyp 对象不能直接依赖普通内核 override 数据结构，改读预先导出的最终
	 * idreg sys_val；字段非零跳 @pass，否则 @fail。@ignore 保持统一调用签名。
	 */
.macro __check_override idreg, fld, width, pass, fail, tmp, ignore
	ldr_l	\tmp, \idreg\()_el1_sys_val
	ubfx	\tmp, \tmp, #\fld, #\width
	cbnz	\tmp, \pass
	b	\fail
.endm

.macro check_override idreg, fld, pass, fail, tmp, ignore
	/* nVHE 包装同样固定字段宽度为 4 位。 */
	__check_override \idreg \fld 4 \pass \fail \tmp \ignore
.endm
#endif

/*
 * finalise_el2_state 在 feature override 已确定后开放高级执行状态。它先处理 MPAM/GCS，
 * 再按 VHE/nVHE 分别关闭 SVE/SME trap并设置最大向量长度、FA64/ZT0 和 priority map。
 * 无参数和错误返回，x0-x2 被破坏；调用者必须已建立安全 EL2 基线且逐 CPU 执行。
 */
.macro finalise_el2_state
	check_override id_aa64pfr0, ID_AA64PFR0_EL1_MPAM_SHIFT, .Linit_mpam_\@, .Lmpam_minor_\@, x1, x2
.Lmpam_minor_\@:
	check_override id_aa64pfr1, ID_AA64PFR1_EL1_MPAM_frac_SHIFT, .Linit_mpam_\@, .Lskip_mpam_\@, x1, x2

.Linit_mpam_\@:
	mov	x0, #MPAM2_EL2_EnMPAMSM_MASK
	msr_s	SYS_MPAM2_EL2, x0		// use the default partition,
						// and disable lower traps
	/* 选择默认 MPAM partition 并关闭下级 trap，不在此分配或配置实际资源分区。 */
	mrs_s	x0, SYS_MPAMIDR_EL1
	tbz	x0, #MPAMIDR_EL1_HAS_HCR_SHIFT, .Lskip_mpam_\@  // skip if no MPAMHCR reg
	/* 只有 MPAMIDR 声明 HCR 存在才访问 MPAMHCR_EL2。 */
	msr_s   SYS_MPAMHCR_EL2, xzr		// clear TRAP_MPAMIDR_EL1 -> EL2
	/* 清 TRAP_MPAMIDR，让下级读取能力不再陷入 EL2。 */

.Lskip_mpam_\@:
	check_override id_aa64pfr1, ID_AA64PFR1_EL1_GCS_SHIFT, .Linit_gcs_\@, .Lskip_gcs_\@, x1, x2

.Linit_gcs_\@:
	/* GCS 存在时先把 EL1/EL0 控制寄存器清为禁用基线，后续任务切换再装具体状态。 */
	msr_s	SYS_GCSCR_EL1, xzr
	msr_s	SYS_GCSCRE0_EL1, xzr

.Lskip_gcs_\@:
	check_override id_aa64pfr0, ID_AA64PFR0_EL1_SVE_SHIFT, .Linit_sve_\@, .Lskip_sve_\@, x1, x2

.Linit_sve_\@:	/* SVE register access */
	/* SVE 分支关闭上级 trap，并把 EL1 可用向量长度设为实现支持的最大值。 */
	__check_hvhe .Lcptr_nvhe_\@, x1

	// (h)VHE case
	// VHE/hVHE 下 CPACR_EL1 是 host 控制面，置 ZEN 开放 SVE。
	mrs	x0, cpacr_el1			// Disable SVE traps
	orr	x0, x0, #CPACR_EL1_ZEN
	msr	cpacr_el1, x0
	b	.Lskip_set_cptr_\@

.Lcptr_nvhe_\@: // nVHE case
	// nVHE 下由 CPTR_EL2.TZ 控制，清位允许 EL1 使用 SVE。
	mrs	x0, cptr_el2			// Disable SVE traps
	bic	x0, x0, #CPTR_EL2_TZ
	msr	cptr_el2, x0
.Lskip_set_cptr_\@:
	isb
	mov	x1, #ZCR_ELx_LEN_MASK		// SVE: Enable full vector
	msr_s	SYS_ZCR_EL2, x1			// length for EL1.
	/* ZCR_EL2 的 LEN 全 1 把全部实现向量长度暴露给 EL1。 */

.Lskip_sve_\@:
	check_override id_aa64pfr1, ID_AA64PFR1_EL1_SME_SHIFT, .Linit_sme_\@, .Lskip_sme_\@, x1, x2

.Linit_sme_\@:	/* SME register access and priority mapping */
	/* SME 同样先关闭 trap，再构造 SMCR_EL2 的最大长度与可选 FA64/ZT0 能力。 */
	__check_hvhe .Lcptr_nvhe_sme_\@, x1

	// (h)VHE case
	// VHE/hVHE 通过 CPACR_EL1.SMEN 开放 SME。
	mrs	x0, cpacr_el1			// Disable SME traps
	orr	x0, x0, #CPACR_EL1_SMEN
	msr	cpacr_el1, x0
	b	.Lskip_set_cptr_sme_\@

.Lcptr_nvhe_sme_\@: // nVHE case
	// nVHE 通过清 CPTR_EL2.TSM 允许下级 SME。
	mrs	x0, cptr_el2			// Disable SME traps
	bic	x0, x0, #CPTR_EL2_TSM
	msr	cptr_el2, x0
.Lskip_set_cptr_sme_\@:
	isb

	mrs	x1, sctlr_el2
	orr	x1, x1, #SCTLR_ELx_ENTP2	// Disable TPIDR2 traps
	/* ENTP2 允许 TPIDR2_EL0 访问，写后 ISB 使 trap 语义立即生效。 */
	msr	sctlr_el2, x1
	isb

	mov	x0, #0				// SMCR controls
	/* x0 从零开始汇总 SMCR，可选位只在对应 ID override 通过时加入。 */

	// Full FP in SM?
	// FA64 表示 streaming mode 是否支持完整 A64 FP 指令集。
	mrs_s	x1, SYS_ID_AA64SMFR0_EL1
	__check_override id_aa64smfr0, ID_AA64SMFR0_EL1_FA64_SHIFT, 1, .Linit_sme_fa64_\@, .Lskip_sme_fa64_\@, x1, x2

.Linit_sme_fa64_\@:
	orr	x0, x0, SMCR_ELx_FA64_MASK
.Lskip_sme_fa64_\@:

	// ZT0 available?
	// SME 版本字段非零时开放 ZT0 tile storage。
	mrs_s	x1, SYS_ID_AA64SMFR0_EL1
	__check_override id_aa64smfr0, ID_AA64SMFR0_EL1_SMEver_SHIFT, 4, .Linit_sme_zt0_\@, .Lskip_sme_zt0_\@, x1, x2
.Linit_sme_zt0_\@:
	orr	x0, x0, SMCR_ELx_EZT0_MASK
.Lskip_sme_zt0_\@:

	orr	x0, x0, #SMCR_ELx_LEN_MASK	// Enable full SME vector
	msr_s	SYS_SMCR_EL2, x0		// length for EL1.
	/* SMCR 最终一次发布 FA64/ZT0 与最大 streaming vector length。 */

	mrs_s	x1, SYS_SMIDR_EL1		// Priority mapping supported?
	/* 仅 SMIDR.SMPS 声明支持时访问 SMPRIMAP_EL2。 */
	ubfx    x1, x1, #SMIDR_EL1_SMPS_SHIFT, #1
	cbz     x1, .Lskip_sme_\@

	msr_s	SYS_SMPRIMAP_EL2, xzr		// Make all priorities equal
	/* 全零把所有 streaming priorities 归一，防止继承固件映射。 */
.Lskip_sme_\@:
.endm

#endif /* __ARM_KVM_INIT_H__ */
