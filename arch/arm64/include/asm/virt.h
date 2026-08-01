/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arm64 EL2 启动状态与 stub HVC 接口学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 * 本文件连接 head.S、hyp-stub.S 与 KVM：定义极早期 HVC ABI，保存所有 CPU 是否从
 * 同一异常级启动，并提供“EL2 是否可用、内核当前是否在 EL2、VHE/pKVM 能力是否
 * 生效”的查询。启动数组在 CPU bring-up 阶段写入，初始化完成后主要只读；pKVM
 * static key 则由其初始化路径一次发布。查询不取得对象引用，也不替代 CPU 热插拔
 * 或 KVM 自身的同步协议。
 */
/*
 * Copyright (C) 2012 ARM Ltd.
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 */

#ifndef __ASM__VIRT_H
#define __ASM__VIRT_H

/*
 * The arm64 hcall implementation uses x0 to specify the hcall
 * number. A value less than HVC_STUB_HCALL_NR indicates a special
 * hcall, such as set vector. Any other value is handled in a
 * hypervisor specific way.
 *
 * The hypercall is allowed to clobber any of the caller-saved
 * registers (x0-x18), so it is advisable to use it through the
 * indirection of a function call (as implemented in hyp-stub.S).
 */
/*
 * stub HVC 以 x0 为调用号，小于 HVC_STUB_HCALL_NR 的值由 hyp-stub.S 固定分派，其余
 * 留给具体 hypervisor。HVC 可破坏 x0-x18 全部 caller-saved 寄存器，所以应通过
 * 正规函数包装调用，让编译器按 AAPCS64 保存仍需使用的值，而非随意内联 hvc 指令。
 */

/*
 * HVC_SET_VECTORS - Set the value of the vbar_el2 register.
 *
 * @x1: Physical address of the new vector table.
 */
/* 调用号 0：x1 借用一张 2 KiB 对齐向量表的物理地址，逐 CPU 写入 VBAR_EL2。 */
#define HVC_SET_VECTORS 0

/*
 * HVC_SOFT_RESTART - CPU soft reset, used by the cpu_soft_restart routine.
 */
/* 调用号 1：由 cpu_soft_restart 重排参数并跳往新入口，成功路径不返回旧调用链。 */
#define HVC_SOFT_RESTART 1

/*
 * HVC_RESET_VECTORS - Restore the vectors to the original HYP stubs
 */
/* 调用号 2：撤销后装的 HYP 向量；原始 stub 本身将其实现为成功空操作。 */
#define HVC_RESET_VECTORS 2

/*
 * HVC_FINALISE_EL2 - Upgrade the CPU from EL1 to EL2, if possible
 */
/* 调用号 3：条件允许时把已在 EL1 建好的内核上下文迁移成 VHE EL2 host。 */
#define HVC_FINALISE_EL2	3

/*
 * HVC_GET_ICH_VTR_EL2 - Retrieve the ICH_VTR_EL2 value
 */
/* 调用号 4：由 EL2 读取 GIC 虚拟化能力寄存器并经 x1 返回。 */
#define HVC_GET_ICH_VTR_EL2	4

/* Max number of HYP stub hypercalls */
/* 固定 stub 调用号的开区间上界；不是完整 hypervisor 可支持功能的总数。 */
#define HVC_STUB_HCALL_NR 5

/* Error returned when an invalid stub number is passed into x0 */
/* 未知功能误入 stub 时返回的醒目哨兵，防止调用者把未执行操作当成功。 */
#define HVC_STUB_ERR	0xbadca11

#define BOOT_CPU_MODE_EL1	(0xe11)
#define BOOT_CPU_MODE_EL2	(0xe12)
/* 特意可辨识的 EL1/EL2 启动模式编码，同时写入 __boot_cpu_mode 两个半槽。 */

/*
 * Flags returned together with the boot mode, but not preserved in
 * __boot_cpu_mode. Used by the idreg override code to work out the
 * boot state.
 */
/* E2H 标志随 init_kernel_el() 的临时返回值传递，供特性 override 判断原始启动状态。 */
#define BOOT_CPU_FLAG_E2H	BIT_ULL(32)

#ifndef __ASSEMBLER__

#include <asm/ptrace.h>
#include <asm/sections.h>
#include <asm/sysreg.h>
#include <asm/cpufeature.h>

/*
 * __boot_cpu_mode records what mode CPUs were booted in.
 * A correctly-implemented bootloader must start all CPUs in the same mode:
 * In this case, both 32bit halves of __boot_cpu_mode will contain the
 * same value (either BOOT_CPU_MODE_EL1 if booted in EL1, BOOT_CPU_MODE_EL2 if
 * booted in EL2).
 *
 * Should the bootloader fail to do this, the two values will be different.
 * This allows the kernel to flag an error when the secondaries have come up.
 */
/*
 * __boot_cpu_mode[0/1] 是启动模式一致性记录：启动 CPU 与次级 CPU 路径把各自模式
 * 合并到两个 32 位槽；正确固件最终令两者相等。不同表示 CPU 被交付在不同 EL，
 * 内核在次级 CPU 到齐后据此报错。数组静态存在，不用引用计数；写入同步由 CPU
 * bring-up 协议负责，普通查询阶段只读。
 */
extern u32 __boot_cpu_mode[2];

/* Arm 异常向量表固定占 16*128=2 KiB，安装地址也必须按同一大小对齐。 */
#define ARM64_VECTOR_TABLE_LEN	SZ_2K

/*
 * 三个外部接口依次逐 CPU 安装/重置 EL2 向量并查询 KVM 全局初始化状态；向量地址
 * 只借用、不转移所有权，两个汇编包装在 C 层均无直接返回值。
 */
void __hyp_set_vectors(phys_addr_t phys_vector_base);
void __hyp_reset_vectors(void);
/* 无参数查询 KVM/arm 全局初始化是否完成，返回布尔值且不转移任何对象 ownership。 */
bool is_kvm_arm_initialised(void);

/* pKVM 初始化完成后启用的全局 static key；默认 false 让未启用系统走近零成本分支。 */
DECLARE_STATIC_KEY_FALSE(kvm_protected_mode_initialized);

/* 返回当前构建和 static key 是否共同表明 pKVM 已发布；无参数、无副作用、不可失败。 */
static inline bool is_pkvm_initialized(void)
{
	return IS_ENABLED(CONFIG_KVM) &&
	       static_branch_likely(&kvm_protected_mode_initialized);
}

#ifdef CONFIG_KVM
/*
 * KVM 构建中按 @phys 强制从 guest 回收页：hypervisor 清零内容并 poison stage-2 PTE，
 * 阻止同一 IPA 再映射；页仍 pinned 到 guest 销毁。true 表示成功或 -EAGAIN 已可视为
 * fault 被处理，false 表示其他失败。调用者只借用物理地址，不取得页引用。
 */
bool pkvm_force_reclaim_guest_page(phys_addr_t phys);
#else
/* 无 KVM 时同签名 stub 永远失败，不访问 @phys，也不产生硬件或所有权副作用。 */
static inline bool pkvm_force_reclaim_guest_page(phys_addr_t phys)
{
	return false;
}
#endif

/* Reports the availability of HYP mode */
/*
 * is_hyp_mode_available() 报告所有 CPU 是否具备内核可用的 EL2。pKVM 发布后新 CPU
 * 可能以 EL1 进入却由受保护 EL2 接管，旧启动数组不再代表当前事实，因此直接 true；
 * 否则要求两个模式槽都为 EL2。无参数、无锁、返回布尔快照且无副作用。
 */
static inline bool is_hyp_mode_available(void)
{
	/*
	 * If KVM protected mode is initialized, all CPUs must have been booted
	 * in EL2. Avoid checking __boot_cpu_mode as CPUs now come up in EL1.
	 */
	/* pKVM 已证明初始 CPU 曾在 EL2 并接管后续启动，不能再以 EL1 表象误判 EL2 不可用。 */
	if (is_pkvm_initialized())
		return true;

	return (__boot_cpu_mode[0] == BOOT_CPU_MODE_EL2 &&
		__boot_cpu_mode[1] == BOOT_CPU_MODE_EL2);
}

/* Check if the bootloader has booted CPUs in different modes */
/*
 * is_hyp_mode_mismatched() 检测固件是否混用 EL1/EL2 交付 CPU。pKVM 场景同样跳过已
 * 失真的数组；普通场景比较两个槽。无参数、无副作用，true 只表示启动契约违例。
 */
static inline bool is_hyp_mode_mismatched(void)
{
	/*
	 * If KVM protected mode is initialized, all CPUs must have been booted
	 * in EL2. Avoid checking __boot_cpu_mode as CPUs now come up in EL1.
	 */
	/* pKVM 已接管后，次级 CPU 从 EL1 进入是协议的一部分，不属于 bootloader mismatch。 */
	if (is_pkvm_initialized())
		return false;

	return __boot_cpu_mode[0] != __boot_cpu_mode[1];
}

/*
 * 读取 CurrentEL 判断普通内核是否正作为 EL2 host 运行。专用 VHE/nVHE hyp 对象禁止
 * 调用，BUILD_BUG_ON 在构建期拒绝这种层次混用；无参数、无副作用，返回当前 CPU 状态。
 */
static __always_inline bool is_kernel_in_hyp_mode(void)
{
	BUILD_BUG_ON(__is_defined(__KVM_NVHE_HYPERVISOR__) ||
		     __is_defined(__KVM_VHE_HYPERVISOR__));
	return read_sysreg(CurrentEL) == CurrentEL_EL2;
}

/*
 * has_vhe() 查询当前编译/运行上下文能否使用 VHE。专用 VHE/nVHE hyp 对象在编译期
 * 折叠为 true/false，普通内核读取最终 cpucap；无参数、无副作用且结果在能力
 * finalization 后稳定。
 */
static __always_inline bool has_vhe(void)
{
	/*
	 * Code only run in VHE/NVHE hyp context can assume VHE is present or
	 * absent. Otherwise fall back to caps.
	 * This allows the compiler to discard VHE-specific code from the
	 * nVHE object, reducing the number of external symbol references
	 * needed to link.
	 */
	/* 专用 hyp 对象使用编译期常量，既避免运行期查询，也减少 nVHE 链接外部符号。 */
	if (is_vhe_hyp_code())
		return true;
	else if (is_nvhe_hyp_code())
		return false;
	else
		return cpus_have_final_cap(ARM64_HAS_VIRT_HOST_EXTN);
}

/*
 * 判断 protected KVM 能力是否对当前上下文可用。VHE hyp 自身固定 false，其他构建
 * 读取最终能力位；无参数和副作用，不表示 pKVM 已完成初始化，后者应查 static key。
 */
static __always_inline bool is_protected_kvm_enabled(void)
{
	if (is_vhe_hyp_code())
		return false;
	else
		return cpus_have_final_cap(ARM64_KVM_PROTECTED_MODE);
}

/*
 * 查询 hVHE 能力。真正 VHE hyp 对象不需要也不声明 hVHE，普通/nVHE 代码读取最终
 * cpucap；无参数、无副作用，不能在能力 finalization 之前当作稳定系统结论。
 */
static __always_inline bool has_hvhe(void)
{
	if (is_vhe_hyp_code())
		return false;

	return cpus_have_final_cap(ARM64_KVM_HVHE);
}

/* 返回“系统有 EL2 但普通内核当前不在 EL2”，即典型 nVHE 布局；无参数、无副作用。 */
static inline bool is_hyp_nvhe(void)
{
	return is_hyp_mode_available() && !is_kernel_in_hyp_mode();
}

#endif /* __ASSEMBLER__ */

#endif /* ! __ASM__VIRT_H */
