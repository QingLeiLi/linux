/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_SCS_H
#define _ASM_SCS_H

#ifdef __ASSEMBLER__

#include <asm/asm-offsets.h>
#include <asm/sysreg.h>

#ifdef CONFIG_SHADOW_CALL_STACK
	/*
	 * arm64 SCS 的硬件/ABI 约定：把 x18 固定为影子栈指针。
	 *
	 * 普通栈仍保存局部变量、被调用者保存寄存器等数据；编译器另外把
	 * 函数返回地址压入只保存返回地址的 Shadow Call Stack。攻击者即使
	 * 通过普通栈溢出覆盖了栈上的 LR，也不能直接改写 SCS 中真正用于
	 * 返回校验/恢复的地址。x18 因而不能再被普通内核代码当临时寄存器。
	 */
	scs_sp	.req	x18

	/*
	 * 为当前任务装入 SCS 的“基地址”而不是已推进的运行时栈顶。
	 * current 先得到 task_struct，TSK_TI_SCS_BASE 是 thread_info 中
	 * scs_base 的汇编偏移。该宏用于必须从影子栈起点开始的早期路径；
	 * 正常任务恢复应使用下面的 scs_load_current()。
	 *
	 * 宏没有参数和返回值；执行后 x18 被覆盖，调用者必须把它视作
	 * SCS 状态初始化，而不是普通的寄存器装载。
	 */
	.macro scs_load_current_base
	get_current_task scs_sp
	ldr	scs_sp, [scs_sp, #TSK_TI_SCS_BASE]
	.endm

	/*
	 * 从 current->thread_info.scs_sp 恢复当前任务上次保存的影子栈顶。
	 * 典型过程是：异常入口先保存旧任务的 x18，调度完成后用本宏装入
	 * 新 current 的 x18。不能只装入 base，否则嵌套调用已压入的返回
	 * 地址会被覆盖，影子栈的调用链随即损坏。
	 */
	.macro scs_load_current
	get_current_task scs_sp
	ldr	scs_sp, [scs_sp, #TSK_TI_SCS_SP]
	.endm

	/*
	 * scs_save(tsk) 把当前 x18 保存到指定任务的 thread_info.scs_sp。
	 * tsk 必须是已经放入通用寄存器的 task_struct 地址；宏的调用点
	 * 位于任务/异常上下文切换边界，那里 x18 即将被另一个任务接管。
	 * SCS 内存的分配释放由通用 SCS 代码负责，本宏只保存游标，不转移
	 * 影子栈所有权，也不需要单独加锁。
	 */
	.macro scs_save tsk
	str	scs_sp, [\tsk, #TSK_TI_SCS_SP]
	.endm
#else
	/*
	 * 未配置 SCS 时仍定义同名空宏，使 entry.S/head.S 等公共汇编路径
	 * 无需散布条件编译；此时 x18 不承担上述影子栈状态保存职责。
	 */
	.macro scs_load_current_base
	.endm

	.macro scs_load_current
	.endm

	.macro scs_save tsk
	.endm
#endif /* CONFIG_SHADOW_CALL_STACK */


#else

#include <linux/scs.h>
#include <asm/cpufeature.h>

#ifdef CONFIG_UNWIND_PATCH_PAC_INTO_SCS
/*
 * 完成动态 SCS 的运行时发布。
 *
 * 背景：支持 PAC 的 arm64 内核可先由位置无关的早期代码扫描 .eh_frame，
 * 把编译器生成的返回地址保护序列动态改写为 SCS 序列。早期补丁器只
 * 记录 __pi_dynamic_scs_is_enabled；等普通内核环境可用后，本函数才
 * 打开 dynamic_scs_enabled 静态键，让通用 SCS 代码走已启用分支。
 *
 * 这形成“先完成代码改写，后发布功能”的两阶段协议。优点是支持同一
 * 镜像依据 CPU 能力选择 PAC 或 SCS，且静态键关闭时热路径几乎没有
 * 分支成本；代价是启动期需要解析展开信息，补丁格式还必须严格校验。
 * setup_arch() 在体系结构初始化阶段调用它，此后键值不再反复切换。
 */
static inline void dynamic_scs_init(void)
{
	/* 由早期位置无关代码写入，前缀 __pi_ 表明它可在重定位前访问。 */
	extern bool __pi_dynamic_scs_is_enabled;

	if (__pi_dynamic_scs_is_enabled) {
		pr_info("Enabling dynamic shadow call stack\n");
		static_branch_enable(&dynamic_scs_enabled);
	}
}
#else
/* 配置未选择动态 PAC->SCS 补丁时保留空接口，调用者无需条件编译。 */
static inline void dynamic_scs_init(void) {}
#endif

/*
 * __pi_scs_patch() 的正错误码。补丁器运行在特殊的早期环境，调用方用
 * 负化后的这些值区分损坏的 CIE/FDE 与不支持的 CFA 指令，而不是只
 * 得到笼统的 -EINVAL：
 *
 * CIE 描述一组栈展开公共规则，FDE 描述某段函数地址范围；SDATA 和
 * augmentation data 都是 DWARF .eh_frame 内的变长数据。严格拒绝异常
 * 长度/操作码可避免补丁器越界读取或误改机器指令。
 */
enum {
	/* CIE 固定头字段或记录边界无效。 */
	EDYNSCS_INVALID_CIE_HEADER		= 1,
	/* CIE 中编码的字符串/补充数据长度越过记录范围。 */
	EDYNSCS_INVALID_CIE_SDATA_SIZE		= 2,
	/* FDE augmentation data 的声明长度无效。 */
	EDYNSCS_INVALID_FDE_AUGM_DATA_SIZE	= 3,
	/* 遇到补丁器不能安全解释的 CFA 展开操作码。 */
	EDYNSCS_INVALID_CFA_OPCODE		= 4,
};

/*
 * 扫描 eh_frame[0..size)，依据 CFA 规则定位 PAC 返回地址序列并改写为
 * SCS 序列。eh_frame 是只读的展开元数据，真正被改写的是规则指向的
 * 内核文本；size 必须是缓冲区的精确字节数。skip_dry_run 为 false 时
 * 可先做只校验不提交的预演，为 true 时跳过该阶段，适合模块装载等已
 * 有外层校验/事务边界的路径。成功返回 0，失败返回对应的负错误码。
 */
/*
 * 【对上段返回值与 dry-run 语义的精确补充】实现返回的是上面枚举的
 * 正错误码，并不取负。skip_dry_run=false 表示先完整 dry-run 校验，
 * 校验成功后再扫描一次并提交补丁；true 表示省略预演、在第一次扫描
 * 时直接补丁。也就是说 false 不是“只预演不提交”，而是更安全但多
 * 一次扫描的“先验证、后提交”。调用者必须按这个实际协议解释参数。
 */
int __pi_scs_patch(const u8 eh_frame[], int size, bool skip_dry_run);

#endif /* __ASSEMBLER__ */

#endif /* _ASM_SCS_H */
