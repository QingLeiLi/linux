// SPDX-License-Identifier: GPL-2.0
/*
 * ARM32 ELF 装载策略、进程 personality 与 FDPIC 地址布局学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 通用 binfmt_elf 负责读取文件、映射 PT_LOAD、建立栈和提交新 mm；本文件只
 * 提供 ARM 必须决定的架构策略：elf_check_arch() 在破坏旧进程映像前拒绝不
 * 兼容机器类型、入口状态和旧 ABI 浮点格式；elf_set_personality() 把 ELF ABI
 * 转换成任务地址限制及 iWMMXt 使用状态；arm_elf_read_implies_exec() 决定旧
 * 二进制是否需要 READ_IMPLIES_EXEC 兼容；FDPIC helper 固定主程序、解释器和栈
 * 的相对布局。成功路径为 load_elf_binary()->elf_check_arch()->begin_new_exec()
 * ->SET_PERSONALITY；校验失败统一让通用装载器返回 -ENOEXEC。
 *
 * 这些规则在兼容性与 W^X 之间取舍：现代 ARMv6+ 且有明确 non-exec stack 的
 * ELF 可保持“读不等于执行”，旧 CPU或缺少 PT_GNU_STACK 的历史程序则扩大执行
 * 权限以避免破坏 ABI。该兼容行为会增加攻击面，因此只能在代码明确列出的旧式
 * 条件下启用。函数不解析完整 ELF，也不持有文件/VMA 所有权。
 */
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/personality.h>
#include <linux/binfmts.h>
#include <linux/elf.h>
#include <linux/elf-fdpic.h>
#include <asm/system_info.h>

/*
 * 判断 ELF32 文件是否能在当前 ARM CPU/内核 ABI 上执行。
 *
 * x 指向通用装载器已读入的 ELF header，调用期间只读且不可为 NULL。返回 1
 * 表示机器类型、入口对齐/Thumb 能力及旧 ABI 浮点约定均兼容；返回 0 表示拒绝，
 * binfmt_elf/FDPIC 会转换为 -ENOEXEC。函数不验证 program header、签名或每条
 * 指令，也不修改 current；模块可调用该导出符号做相同架构筛选。
 */
int elf_check_arch(const struct elf32_hdr *x)
{
	unsigned int eflags;

	/* Make sure it's an ARM executable */
	/* e_machine 是第一道架构边界，其他机器的 e_flags 位含义不能按 ARM 解释。 */
	if (x->e_machine != EM_ARM)
		return 0;

	/* Make sure the entry address is reasonable */
	/* bit0=1 按 ABI 表示 Thumb 入口；ARM 状态入口则必须 4 字节对齐。 */
	if (x->e_entry & 1) {
		/* 没有 Thumb 硬件能力时即使地址编码合法也无法开始执行。 */
		if (!(elf_hwcap & HWCAP_THUMB))
			return 0;
	} else if (x->e_entry & 3)
		return 0;

	eflags = x->e_flags;
	/* EABI_UNKNOWN 即旧 OABI，只有此时 APCS26/VFP/SOFT_FLOAT 旧标志参与校验。 */
	if ((eflags & EF_ARM_EABI_MASK) == EF_ARM_EABI_UNKNOWN) {
		unsigned int flt_fmt;

		/* APCS26 is only allowed if the CPU supports it */
		/* 26-bit APCS 依赖老 CPU 模式，现代纯 32-bit CPU 必须拒绝而非勉强运行。 */
		if ((eflags & EF_ARM_APCS_26) && !(elf_hwcap & HWCAP_26BIT))
			return 0;

		flt_fmt = eflags & (EF_ARM_VFP_FLOAT | EF_ARM_SOFT_FLOAT);

		/* VFP requires the supporting code */
		/* 只有明确选择 VFP ABI 且 CPU/内核公开 HWCAP_VFP 时才接受硬浮点指令。 */
		if (flt_fmt == EF_ARM_VFP_FLOAT && !(elf_hwcap & HWCAP_VFP))
			return 0;
	}
	return 1;
}
EXPORT_SYMBOL(elf_check_arch);

/*
 * 根据即将执行的 ELF header 初始化 current 的 ARM personality。
 *
 * x 只读、不可为 NULL，通常在 begin_new_exec() 已成功、旧映像不可恢复后由
 * SET_PERSONALITY 调用。函数保留 personality 中 PER_MASK 以外的标志，再强制
 * 基础域为 PER_LINUX；OABI APCS26 与普通 32-bit ABI选择不同地址限制，并按
 * FPA/iWMMXt 协处理器冲突设置或清除 TIF_USING_IWMMXT。无返回值，可观察结果
 * 是 current->personality 和 thread flag；只操作当前任务，不需额外锁。
 */
void elf_set_personality(const struct elf32_hdr *x)
{
	unsigned int eflags = x->e_flags;
	unsigned int personality = current->personality & ~PER_MASK;

	/*
	 * We only support Linux ELF executables, so always set the
	 * personality to LINUX.
	 */
	/* 保留 READ_IMPLIES_EXEC 等独立 personality 标志，只替换低位执行域。 */
	personality |= PER_LINUX;

	/*
	 * APCS-26 is only valid for OABI executables
	 */
	/* 仅 OABI+APCS26 选择遗留 26-bit 地址语义，其他 ARM ELF 固定 32-bit 地址限制。 */
	if ((eflags & EF_ARM_EABI_MASK) == EF_ARM_EABI_UNKNOWN &&
	    (eflags & EF_ARM_APCS_26))
		personality &= ~ADDR_LIMIT_32BIT;
	else
		personality |= ADDR_LIMIT_32BIT;

	set_personality(personality);

	/*
	 * Since the FPA coprocessor uses CP1 and CP2, and iWMMXt uses CP0
	 * and CP1, we only enable access to the iWMMXt coprocessor if the
	 * binary is EABI or softfloat (and thus, guaranteed not to use
	 * FPA instructions.)
	 */
	/*
	 * iWMMXt 与旧 FPA 复用协处理器编号，不能让同一线程同时按两套 ABI 使用。
	 * EABI 不使用 FPA，OABI soft-float 也保证不发 FPA 指令，因此只有这两类
	 * 二进制在硬件支持 iWMMXt 时置线程标志；exec 其他 ELF 必须清旧标志。
	 */
	if (elf_hwcap & HWCAP_IWMMXT &&
	    eflags & (EF_ARM_EABI_MASK | EF_ARM_SOFT_FLOAT)) {
		set_thread_flag(TIF_USING_IWMMXT);
	} else {
		clear_thread_flag(TIF_USING_IWMMXT);
	}
}
EXPORT_SYMBOL(elf_set_personality);

/*
 * An executable for which elf_read_implies_exec() returns TRUE will
 * have the READ_IMPLIES_EXEC personality flag set automatically.
 *
 * The decision process for determining the results are:
 *
 *                 CPU: | lacks NX*  | has NX     |
 * ELF:                 |            |            |
 * ---------------------|------------|------------|
 * missing PT_GNU_STACK | exec-all   | exec-all   |
 * PT_GNU_STACK == RWX  | exec-all   | exec-stack |
 * PT_GNU_STACK == RW   | exec-all   | exec-none  |
 *
 *  exec-all  : all PROT_READ user mappings are executable, except when
 *              backed by files on a noexec-filesystem.
 *  exec-none : only PROT_EXEC user mappings are executable.
 *  exec-stack: only the stack and PROT_EXEC user mappings are executable.
 *
 *  *this column has no architectural effect: NX markings are ignored by
 *   hardware, but may have behavioral effects when "wants X" collides with
 *   "cannot be X" constraints in memory permission flags, as in
 *   https://lkml.kernel.org/r/20190418055759.GA3155@mellanox.com
 *
 */
/*
 * 上表的本地决策只回答“是否给任务设置 READ_IMPLIES_EXEC”：缺少
 * PT_GNU_STACK（EXSTACK_DEFAULT）为历史兼容始终返回 1；ARMv5 及更老 CPU
 * 没有有效 NX，也返回 1；ARMv6+ 且 ELF 明确声明栈策略时返回 0，由通用 ELF
 * 代码仅按 PT_GNU_STACK 设置栈权限。参数是 EXSTACK_* 枚举，返回 1/0，
 * 不直接修改 personality 或页表。
 */
int arm_elf_read_implies_exec(int executable_stack)
{
	/* 无 GNU stack note 的旧工具链默认假设可执行数据，维持 exec-all 兼容。 */
	if (executable_stack == EXSTACK_DEFAULT)
		return 1;
	/* ARMv6 前页表 NX 标记无架构效果，按可执行读取映射处理最符合实际硬件。 */
	if (cpu_architecture() < CPU_ARCH_ARMv6)
		return 1;
	return 0;
}
EXPORT_SYMBOL(arm_elf_read_implies_exec);

#if defined(CONFIG_MMU) && defined(CONFIG_BINFMT_ELF_FDPIC)

/*
 * 为 ARM FDPIC 主程序和动态解释器选择初始装载窗口。
 *
 * exec_params/interp_params 是通用 FDPIC loader 的可写规划对象；start_stack
 * 是输出的初始栈顶，start_brk 由通用层传入但 ARM 无需改写。函数先复用普通
 * ELF personality，再把主程序放在传统 0x8000、解释器放在 ELF_ET_DYN_BASE，
 * 栈顶留在 TASK_SIZE 下方 16 MiB，为向下增长、解释器和其他高端映射留空间。
 * 若文件允许各 PT_LOAD 独立摆放，ARM 将其收紧为 CONSTDISP，要求段间保持链接
 * 时固定差值，便于 FDPIC 函数描述符和重定位模型成立。无分配、无失败返回。
 */
void elf_fdpic_arch_lay_out_mm(struct elf_fdpic_params *exec_params,
			       struct elf_fdpic_params *interp_params,
			       unsigned long *start_stack,
			       unsigned long *start_brk)
{
	/* FDPIC exec 同样必须清理前一程序留下的 APCS/iWMMXt personality 状态。 */
	elf_set_personality(&exec_params->hdr);

	/* 这些只是 mapper 的选址提示；实际 VMA 建立和冲突处理仍由通用 FDPIC 完成。 */
	exec_params->load_addr = 0x8000;
	interp_params->load_addr = ELF_ET_DYN_BASE;
	*start_stack = TASK_SIZE - SZ_16M;

	/* ARM 不支持任意独立段位移，把最宽松请求规范化为保持常量段间位移。 */
	if ((exec_params->flags & ELF_FDPIC_FLAG_ARRANGEMENT) == ELF_FDPIC_FLAG_INDEPENDENT) {
		exec_params->flags &= ~ELF_FDPIC_FLAG_ARRANGEMENT;
		exec_params->flags |= ELF_FDPIC_FLAG_CONSTDISP;
	}
}

#endif
