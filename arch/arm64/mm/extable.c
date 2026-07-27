// SPDX-License-Identifier: GPL-2.0
/*
 * arm64 异常表查询与可恢复内核 fault 分发。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * uaccess/BPF 等汇编在可能 fault 的指令旁生成 exception-table 项，编码
 * 原指令、fixup 相对地址、类型及寄存器元数据。异常入口查表后在 pt_regs
 * 中改返回 PC/结果寄存器，使 fault 像普通 -EFAULT 返回，而不是 oops。
 * 表在链接后只读，查询可并发；当前 CPU 的 regs 由异常上下文独占。
 */
/*
 * 本轮补充注释模型：OpenAI GPT-5.4（2026-07-27）。
 *
 * 文件边界与调用地图：
 *
 *   arch/arm64/mm/fault.c
 *     -> fixup_exception(regs, esr)
 *          -> search_exception_tables(regs->pc)
 *          -> EX_TYPE_* handler
 *               修改结果寄存器/PC，返回 true
 *          -> 无表项或 copy 方向不符，返回 false
 *     -> true：异常已消费，从修复后的现场返回
 *     -> false：继续 translation/MTE/permission/oops 等普通 fault 路径
 *
 *   用户地址权限分类
 *     -> insn_may_access_user(pc, esr)
 *          证明 EL1 对用户地址的访问来自允许的 uaccess 表项
 *
 * exception_table_entry 由链接器或 BPF JIT 拥有，本文件始终只借用读取。
 * pt_regs 属于当前 CPU 的异常入口，本 handler 在返回前独占修改；没有锁、
 * 引用计数或资源分配，也不得睡眠。内建表发布后只读，模块/BPF 查询的
 * 生命周期由通用 search_exception_tables() 及当前执行上下文稳定。
 *
 * 表驱动方案让正常 uaccess 不承担分支开销，只有 fault 才查表；type/data
 * 复用少量 C handler，减少离线 fixup 代码。代价是汇编生成宏、结构布局
 * 与 switch 必须严格保持 ABI 一致，未知 type 因而视作不可继续的内核 bug。
 */
/*
 * Based on arch/arm/mm/extable.c
 */
/* 实现沿袭 32 位 ARM 异常表框架，但 fixup 编码和寄存器现场按 arm64 ABI 解释。 */

#include <linux/bitfield.h>
#include <linux/extable.h>
#include <linux/uaccess.h>

#include <asm/asm-extable.h>
#include <asm/esr.h>
#include <asm/ptrace.h>

/* 判断 copy fault 的实际读写方向是否与表项标记的 uaccess 那半边一致。 */
/*
 * cpy_faulted_on_uaccess() - 判定 copy 双向访问中发生故障的是用户半边。
 *
 * @ex 是已命中的 EX_TYPE_UACCESS_CPY 借用表项，不可为 NULL；data 的
 * EX_DATA_UACCESS_WRITE 位说明用户半边是读还是写。@esr 是本次同步
 * data abort 的纯输入 ESR，WnR 位说明实际故障访问方向。
 *
 * 两个方向相等返回 true，否则返回 false。函数只读元数据、无副作用、
 * 不睡眠。该检查防止 copy_to/from_user 的内核缓冲区故障被当作 -EFAULT
 * 吞掉；方向不符必须交回上层 oops 路径暴露真实内核 bug。
 */
static bool cpy_faulted_on_uaccess(const struct exception_table_entry *ex,
				   unsigned long esr)
{
	/*
	 * uaccess_is_write/fault_on_write 分别代表表项声明方向和硬件报告方向；
	 * 后者只需布尔语义，无需保留 ESR_ELx_WNR 的原始位位置。
	 */
	bool uaccess_is_write = FIELD_GET(EX_DATA_UACCESS_WRITE, ex->data);
	bool fault_on_write = esr & ESR_ELx_WNR;

	return uaccess_is_write == fault_on_write;
}

/*
 * 判断 addr 指令发生的 abort 是否可能来自用户访问。esr 提供 WnR 方向；
 * 无表项返回 false，copy 类型还须方向匹配，其他可修复 uaccess 表项返回
 * true。函数只读链接表，供 fault 分类避免把内核自身错误误报为用户 fault。
 */
/*
 * 补充契约：@addr 是 EL1 fault 指令虚拟地址，@esr 是同一次异常的 ESR，
 * 二者均按值输入。调用者是 arm64 do_page_fault() 的权限检查：当 EL1
 * 访问用户地址时，它要求当前 PC 确实由异常表授权。
 *
 * 返回 true 只证明“表项允许把此次访问视作 uaccess”，不表示 fault 已经
 * 修复；无表项返回 false。普通类型只要有表项即可，copy 类型还要验证
 * WnR 方向。函数不修改 regs、不取得表项 ownership、不会睡眠。
 */
bool insn_may_access_user(unsigned long addr, unsigned long esr)
{
	/*
	 * ex 是当前查询窗口内借用的候选表项，NULL 表示该指令未声明
	 * 可恢复。
	 */
	const struct exception_table_entry *ex = search_exception_tables(addr);

	/* 未登记的 EL1 用户访问不能借 uaccess 名义绕过权限诊断。 */
	if (!ex)
		return false;

	/*
	 * copy helper 同时访问用户和内核缓冲区，必须额外区分故障半边；
	 * 其他已登记策略都明确标记了允许 fault 的指令。
	 */
	switch (ex->type) {
	case EX_TYPE_UACCESS_CPY:
		return cpy_faulted_on_uaccess(ex, esr);
	default:
		return true;
	}
}

/* fixup 字段是相对字段自身地址的链接期偏移，转换为可写入 PC 的绝对地址。 */
/*
 * get_ex_fixup() - 还原普通 arm64 表项的绝对恢复 PC。
 *
 * @ex 是已命中且其 type 使用“相对 fixup 地址”语义的借用只读表项，
 * 不可为 NULL。返回 `&ex->fixup + ex->fixup` 对应的虚拟地址；无副作用、
 * 不睡眠。EX_TYPE_BPF 会重用 fixup 为位域，必须由 BPF handler 解释，
 * 不能调用本 helper。
 */
static inline unsigned long
get_ex_fixup(const struct exception_table_entry *ex)
{
	return ((unsigned long)&ex->fixup + ex->fixup);
}

/*
 * 按表项编码把 reg_err 写 -EFAULT、reg_zero 清零，再跳到 fixup。regs 是
 * 当前异常现场并被原地修改；返回 true 表示异常已消费，可从新 PC 恢复。
 */
/*
 * 补充契约：@ex 是 UACCESS_ERR_ZERO 或 KACCESS_ERR_ZERO 的借用表项；
 * @regs 是当前 CPU 独占的借用可写异常现场。调用者已完成查表和 type
 * 分派，两个指针均不可为 NULL。
 *
 * data 的两个 5-bit 字段分别选择错误码和清零目标寄存器；编号 31 表示
 * XZR，pt_regs_write_reg() 会忽略该输出。函数最后把 PC 指向普通相对
 * fixup，返回 true。没有失败、分配或 ownership 转移，执行期间不能睡眠。
 */
static bool ex_handler_uaccess_err_zero(const struct exception_table_entry *ex,
					struct pt_regs *regs)
{
	/* reg_err/reg_zero 是架构寄存器编号，只在本次现场修复期间有效。 */
	int reg_err = FIELD_GET(EX_DATA_REG_ERR, ex->data);
	int reg_zero = FIELD_GET(EX_DATA_REG_ZERO, ex->data);

	/*
	 * 先建立 fixup 代码约定的输出寄存器状态，再发布新的 PC；虽然当前
	 * regs 由本 CPU 独占，保持该顺序仍让恢复点观察到完整结果。
	 */
	pt_regs_write_reg(regs, reg_err, -EFAULT);
	pt_regs_write_reg(regs, reg_zero, 0);

	regs->pc = get_ex_fixup(ex);
	return true;
}

/* copy helper 仅修复真正的 user half；内核地址一侧 fault 返回 false 触发 oops。 */
/*
 * ex_handler_uaccess_cpy() - 修复 copy helper 的用户地址一侧故障。
 *
 * @ex 是已命中的 EX_TYPE_UACCESS_CPY 借用表项；@regs 是当前 CPU 独占
 * 的借用可写异常现场；@esr 是同一次 data abort 的纯输入状态。
 *
 * 方向匹配时仅把 PC 改到相对 fixup 并返回 true；方向不符时保持整个现场
 * 不变并返回 false，让 __do_kernel_fault() 继续诊断内核缓冲区故障。
 * 函数不睡眠、不分配、不转移 ownership。
 */
static bool ex_handler_uaccess_cpy(const struct exception_table_entry *ex,
				   struct pt_regs *regs, unsigned long esr)
{
	/* Do not fix up faults on kernel memory accesses */
	/*
	 * 不修复发生在内核内存访问上的 fault。
	 * cpy_faulted_on_uaccess() 用表项方向与 ESR WnR 配对；失败出口
	 * 尚未修改 regs，可安全交回上层。
	 */
	/* 方向不符说明坏的是内核缓冲区，吞掉它会掩盖真实内核 bug。 */
	if (!cpy_faulted_on_uaccess(ex, esr))
		return false;

	regs->pc = get_ex_fixup(ex);
	return true;
}

/*
 * 修复跨 8 字节边界的容错 load：从向下对齐地址读取可访问部分，再按端序
 * 移位把 fault 一侧逻辑补零，写回表项指定寄存器并跳转 fixup。表项生成方
 * 保证对齐后的 word 可安全读；reg_data/reg_addr 是 pt_regs 寄存器编号。
 */
/*
 * 补充契约：@ex 是 EX_TYPE_LOAD_UNALIGNED_ZEROPAD 借用表项；@regs 是
 * 当前 CPU 独占的借用可写异常现场，二者不可为 NULL。调用者来自
 * load_unaligned_zeropad() 的极少见跨页 fault：起始页可读，下一页缺失。
 *
 * 函数读取起始页内向下对齐的 unsigned long，按原地址低 3 位计算
 * 字节偏移；小端右移、大端左移，使缺失页一侧自然补 0。成功写目标
 * 寄存器和 fixup PC，固定返回 true。它不分配、不能睡眠；若
 * “起始对齐 word 可读”前置条件被破坏，handler 自身读取也会 fault，
 * 因此该类型只能由匹配的 word-at-a-time 生成宏使用。
 */
static bool
ex_handler_load_unaligned_zeropad(const struct exception_table_entry *ex,
				  struct pt_regs *regs)
{
	/*
	 * reg_data/reg_addr 是表项编码的寄存器号；data 保存重建值，
	 * addr 保存对齐后的读取地址，offset 是原地址在 8-byte word 内偏移。
	 */
	int reg_data = FIELD_GET(EX_DATA_REG_DATA, ex->data);
	int reg_addr = FIELD_GET(EX_DATA_REG_ADDR, ex->data);
	unsigned long data, addr, offset;

	/*
	 * 阶段 1：从异常现场恢复原指针，并拆出 word 对齐地址与
	 * 字节偏移。
	 */
	addr = pt_regs_read_reg(regs, reg_addr);

	offset = addr & 0x7UL;
	addr &= ~0x7UL;

	/*
	 * 只读取故障边界前已映射页内的对齐 word；该读取没有独立 extable，
	 * 安全性来自生成端仅为“跨到下一缺页”的场景选择此 handler。
	 */
	data = *(unsigned long*)addr;

#ifndef __AARCH64EB__
	/* 小端：丢弃地址之前的低位字节，高位由逻辑右移补零。 */
	data >>= 8 * offset;
#else
	/* 大端：丢弃地址之前的高位字节，低位由左移补零。 */
	data <<= 8 * offset;
#endif

	/* 阶段 2：提交恢复值，再把异常返回 PC 发布为表项 fixup。 */
	pt_regs_write_reg(regs, reg_data, data);

	regs->pc = get_ex_fixup(ex);
	return true;
}

/*
 * 异常表总分发。regs 是当前 CPU 可修改现场，esr 是同步异常原因；无匹配
 * 返回 false 让上层走正常 fault/oops，匹配并成功修复返回 true。未知类型
 * 表示内核与汇编元数据不一致，BUG 比静默返回到错误 PC 更安全。
 */
/*
 * 补充契约：@regs 是当前 EL1 data abort 的借用可写现场，不可为 NULL；
 * @esr 是同一次异常的只读 syndrome。__do_kernel_fault() 在排除
 * instruction abort 后调用，本函数不持锁、不能睡眠，也不取得表项引用。
 *
 * 阶段 1 按当前 PC 查询内建/模块/BPF 表；未命中返回 false，regs 不变。
 * 阶段 2 按 type 分派：BPF 可能自行拒绝，ERR_ZERO 写结果并跳 fixup，
 * CPY 只有用户半边可修复，ZEROPAD 重建部分 word。handler 返回值原样
 * 表示是否消费异常。未知 type 没有安全恢复契约，BUG 阻止返回损坏现场。
 */
bool fixup_exception(struct pt_regs *regs, unsigned long esr)
{
	/* ex 是本次 fault 对应的借用只读元数据，不跨越函数保存。 */
	const struct exception_table_entry *ex;

	/*
	 * 阶段 1：PC 是异常指令地址；NULL 明确把处理权交还普通
	 * fault 路径。
	 */
	ex = search_exception_tables(instruction_pointer(regs));
	if (!ex)
		return false;

	/* 阶段 2：type 是生成宏与这里共享的内部 ABI，决定 regs 修改协议。 */
	switch (ex->type) {
	case EX_TYPE_BPF:
		return ex_handler_bpf(ex, regs);
	case EX_TYPE_UACCESS_ERR_ZERO:
	case EX_TYPE_KACCESS_ERR_ZERO:
		return ex_handler_uaccess_err_zero(ex, regs);
	case EX_TYPE_UACCESS_CPY:
		return ex_handler_uaccess_cpy(ex, regs, esr);
	case EX_TYPE_LOAD_UNALIGNED_ZEROPAD:
		return ex_handler_load_unaligned_zeropad(ex, regs);
	}

	/*
	 * 有表项却没有已知处理类型说明内核/JIT 生成端与运行期
	 * 解释器失配；继续返回会重复 fault 或跳转到不可预测位置，
	 * 因此不可恢复。
	 */
	BUG();
}
