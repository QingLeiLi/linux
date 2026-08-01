/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64 指针认证汇编接口学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 文件职责：把 task_struct 中的 PAC key 快照装入系统寄存器，并在次级 CPU 启动
 * 时建立地址认证控制状态。C 版 helper 位于 pointer_auth.h；本文件服务于不能安全
 * 调用 C 的异常入口/返回、cpu_switch_to()、CPU resume 和早期次级 CPU 启动路径。
 *
 * 主要路径：
 *   EL0 exception entry -> 安装当前任务的内核 APIAKey
 *   EL0 return -> 最后阶段安装用户 APIAKey
 *   cpu_switch_to(prev, next) -> 安装 next 的内核 APIAKey
 *   secondary CPU boot -> 检测本 CPU -> 使能 PAC -> 安装 boot task 内核 key
 *   CPU resume -> 恢复 current 的内核 key
 *
 * 状态与并发：宏读取调用者已稳定的 task_struct 借用指针，并只修改当前 CPU 的
 * key/SCTLR 系统寄存器；不分配内存、不持有引用、不睡眠。tmp 参数全部是调用者
 * 允许破坏的临时通用寄存器。`nosync` 版本不提供 ISB，调用者必须在使用新 key
 * 前安排上下文同步；带同步版本将 ISB 作为本 CPU 发布边界。
 *
 * 方案权衡：alternative 在启动期把能力判断修补为直线指令或 NOP，避免每次异常
 * 和切换都读取特性寄存器；早期次级 CPU 尚处于能力最终确认边界，仍保留一次直接
 * ID 寄存器检测。配置关闭时生成空汇编宏，保持调用点和寄存器分配结构稳定。
 */
#ifndef __ASM_ASM_POINTER_AUTH_H
#define __ASM_ASM_POINTER_AUTH_H

/* include guard 防止同一汇编翻译单元重复定义这些 .macro 名称。 */

#include <asm/alternative.h>
#include <asm/asm-offsets.h>
#include <asm/cpufeature.h>
#include <asm/sysreg.h>
/*
 * alternative/cpufeature 提供启动期指令修补，asm-offsets 把 C 结构偏移导出给汇编，
 * sysreg 提供可兼容工具链的系统寄存器编码。这里只建立预处理依赖。
 */

#ifdef CONFIG_ARM64_PTR_AUTH_KERNEL

/* 本分支生成真实的内核 APIAKey 装载序列；关闭配置时生成同名空宏。 */

	/*
	 * __ptrauth_keys_install_kernel_nosync(tsk, tmp1, tmp2, tmp3)
	 *
	 * @tsk: 保存目标 task_struct 地址的输入寄存器，宏不改写它。
	 * @tmp1: 先承接 thread.keys_kernel 偏移，再成为该子结构地址。
	 * @tmp2/@tmp3: 承接 APIAKey 的 Lo/Hi 64 位并写系统寄存器。
	 *
	 * THREAD_KEYS_KERNEL 与 PTRAUTH_KERNEL_KEY_APIA 来自 asm-offsets.c，保证汇编布局
	 * 跟随当前 C 结构而非手写常量。宏无能力检测、无 ISB、无返回值；调用者必须
	 * 已确认 CPU 支持地址认证，并在后续 PAC 指令前完成同步。三个 tmp 均被破坏。
	 */
	.macro __ptrauth_keys_install_kernel_nosync tsk, tmp1, tmp2, tmp3
	/* 先定位 thread.keys_kernel，再成对读取完整 128 位 APIAKey。 */
	mov	\tmp1, #THREAD_KEYS_KERNEL
	add	\tmp1, \tsk, \tmp1
	ldp	\tmp2, \tmp3, [\tmp1, #PTRAUTH_KERNEL_KEY_APIA]
	msr_s	SYS_APIAKEYLO_EL1, \tmp2
	msr_s	SYS_APIAKEYHI_EL1, \tmp3
	.endm

	/*
	 * ptrauth_keys_install_kernel_nosync() 是带 alternative 能力门的热路径包装。
	 * ARM64_HAS_ADDRESS_AUTH 在系统能力最终确定后把支持机器修补为真实装载序列，
	 * 不支持机器修补为等长 NOP。参数/破坏寄存器与底层宏相同，仍不执行 ISB。
	 */
	.macro ptrauth_keys_install_kernel_nosync tsk, tmp1, tmp2, tmp3
alternative_if ARM64_HAS_ADDRESS_AUTH
	__ptrauth_keys_install_kernel_nosync \tsk, \tmp1, \tmp2, \tmp3
alternative_else_nop_endif
	.endm

	/*
	 * ptrauth_keys_install_kernel() 在同一能力门内装载 key 并执行 ISB。用于
	 * cpu_switch_to() 等下一条返回路径可能立即执行 PAC 认证的边界；ISB 确保本
	 * CPU 后续指令使用新 key，但不发布普通内存给其他 CPU。
	 */
	.macro ptrauth_keys_install_kernel tsk, tmp1, tmp2, tmp3
alternative_if ARM64_HAS_ADDRESS_AUTH
	__ptrauth_keys_install_kernel_nosync \tsk, \tmp1, \tmp2, \tmp3
	isb
alternative_else_nop_endif
	.endm

#else /* CONFIG_ARM64_PTR_AUTH_KERNEL */
/*
 * 上方英文标记表示内核返回地址 PAC 未编入。三个同名宏保留参数签名但不生成
 * 指令，也不破坏实参寄存器；用户态 PAC 仍可能由文件后半部分独立支持。
 */

	.macro __ptrauth_keys_install_kernel_nosync tsk, tmp1, tmp2, tmp3
	.endm

	.macro ptrauth_keys_install_kernel_nosync tsk, tmp1, tmp2, tmp3
	.endm

	.macro ptrauth_keys_install_kernel tsk, tmp1, tmp2, tmp3
	.endm

#endif /* CONFIG_ARM64_PTR_AUTH_KERNEL */
/* 上方英文行尾注释说明内核 key 装载宏的真实实现或空桩选择到此结束。 */

#ifdef CONFIG_ARM64_PTR_AUTH
/* 该分支生成用户 APIA 装载和次级 CPU 地址认证初始化序列。 */
/*
 * thread.keys_user.ap* as offset exceeds the #imm offset range
 * so use the base value of ldp as thread.keys_user and offset as
 * thread.keys_user.ap*.
 */
/*
 * thread.keys_user.ap* 距 task_struct 起点太远，超出 LDP 的立即数偏移编码范围；
 * 因此先用 THREAD_KEYS_USER 得到 keys_user 子结构基址，再用较小的
 * PTRAUTH_USER_KEY_APIA 偏移执行 LDP。两个常量都由 asm-offsets.c 从当前 C 布局
 * 生成，避免结构字段变化后汇编静默读取错误地址。
 */
	/*
	 * __ptrauth_keys_install_user(tsk, tmp1, tmp2, tmp3)
	 *
	 * @tsk: 目标任务 task_struct 地址输入寄存器，不被改写。
	 * @tmp1: 承接 keys_user 偏移与子结构地址；@tmp2/@tmp3 承接用户 APIA Lo/Hi。
	 *
	 * entry.S 在返回 EL0 的最后阶段调用，把用户 APIAKey 写入共享硬件寄存器；
	 * 之后不能再调用可能用内核 APIA 签名返回地址的 C 函数。宏无能力门、无 ISB，
	 * 外层 alternative 和异常返回负责条件选择与上下文同步；三个 tmp 均被破坏。
	 */
	.macro __ptrauth_keys_install_user tsk, tmp1, tmp2, tmp3
	/* 两级寻址绕过 LDP 从 task_struct 直接访问时的立即数范围限制。 */
	mov	\tmp1, #THREAD_KEYS_USER
	add	\tmp1, \tsk, \tmp1
	ldp	\tmp2, \tmp3, [\tmp1, #PTRAUTH_USER_KEY_APIA]
	msr_s	SYS_APIAKEYLO_EL1, \tmp2
	msr_s	SYS_APIAKEYHI_EL1, \tmp3
	.endm

	/*
	 * __ptrauth_keys_init_cpu(tsk, tmp1, tmp2, tmp3)
	 *
	 * @tsk: 次级 CPU 即将运行的 boot/idle task 地址；其内核 key 已在任务构造时生成。
	 * @tmp1/@tmp2: 读取特性字段、构造 SCTLR 掩码和完成读改写；@tmp3: key 临时量。
	 * 四个寄存器参数均由早期启动汇编提供，tmp1..tmp3 在返回时被破坏。
	 *
	 * 此时次级 CPU 尚未进入普通 C 启动路径，宏直接读取 ID_AA64ISAR1_EL1.APA 与
	 * ID_AA64ISAR2_EL1.APA3；任一非零即表示地址认证算法存在。支持时设置四个
	 * SCTLR_EL1 En* 位，装入任务的内核 APIAKey，并用 ISB 一次性提交控制位与 key。
	 * 不支持时跳到本次宏展开专属的 .Lno_addr_auth\@，完全不访问 key 寄存器。
	 */
	.macro __ptrauth_keys_init_cpu tsk, tmp1, tmp2, tmp3
	/* 汇总两代特性字段；orr 结果为零才表示没有任何地址认证算法。 */
	mrs	\tmp1, id_aa64isar1_el1
	ubfx	\tmp1, \tmp1, #ID_AA64ISAR1_EL1_APA_SHIFT, #8
	mrs_s	\tmp2, SYS_ID_AA64ISAR2_EL1
	ubfx	\tmp2, \tmp2, #ID_AA64ISAR2_EL1_APA3_SHIFT, #4
	orr	\tmp1, \tmp1, \tmp2
	cbz	\tmp1, .Lno_addr_auth\@
	/* 以读改写保留 SCTLR_EL1 其他控制位，只打开 IA/IB/DA/DB 认证。 */
	mov_q	\tmp1, (SCTLR_ELx_ENIA | SCTLR_ELx_ENIB | \
			SCTLR_ELx_ENDA | SCTLR_ELx_ENDB)
	mrs	\tmp2, sctlr_el1
	orr	\tmp2, \tmp2, \tmp1
	msr	sctlr_el1, \tmp2
	/* key 和控制位都写完后只需一个 ISB，避免在早期启动路径重复同步。 */
	__ptrauth_keys_install_kernel_nosync \tsk, \tmp1, \tmp2, \tmp3
	isb
.Lno_addr_auth\@:
	.endm

	/*
	 * ptrauth_keys_init_cpu() 用系统级 ARM64_HAS_ADDRESS_AUTH alternative 包装原始
	 * 初始化。最终能力不成立时，启动期修补为直接跳过；成立时继续执行底层宏，
	 * 底层仍核对当前次级 CPU 的 ID 字段，确保在该 CPU 可安全访问寄存器后才使能。
	 * 两层的 .Lno_addr_auth\@ 都因 \@ 获得每次展开唯一编号，不会标签冲突。
	 */
	.macro ptrauth_keys_init_cpu tsk, tmp1, tmp2, tmp3
alternative_if_not ARM64_HAS_ADDRESS_AUTH
	b	.Lno_addr_auth\@
alternative_else_nop_endif
	__ptrauth_keys_init_cpu \tsk, \tmp1, \tmp2, \tmp3
.Lno_addr_auth\@:
	.endm

#else /* !CONFIG_ARM64_PTR_AUTH */
/*
 * 上方英文标记表示用户态 PAC 完全关闭。保留空的用户 key 装载宏，使共享汇编调用
 * 点无需额外条件分支；宏不生成指令，也不破坏 @tsk/@tmp1..@tmp3。
 */

	.macro ptrauth_keys_install_user tsk, tmp1, tmp2, tmp3
	.endm

#endif /* CONFIG_ARM64_PTR_AUTH */
/* 上方英文行尾注释说明用户/CPU PAC 汇编实现或空桩选择到此结束。 */

#endif /* __ASM_ASM_POINTER_AUTH_H */

/* 上方英文行尾注释说明 __ASM_ASM_POINTER_AUTH_H include guard 到此结束。 */
