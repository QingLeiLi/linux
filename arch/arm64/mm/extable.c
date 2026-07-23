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
static bool cpy_faulted_on_uaccess(const struct exception_table_entry *ex,
				   unsigned long esr)
{
	bool uaccess_is_write = FIELD_GET(EX_DATA_UACCESS_WRITE, ex->data);
	bool fault_on_write = esr & ESR_ELx_WNR;

	return uaccess_is_write == fault_on_write;
}

/*
 * 判断 addr 指令发生的 abort 是否可能来自用户访问。esr 提供 WnR 方向；
 * 无表项返回 false，copy 类型还须方向匹配，其他可修复 uaccess 表项返回
 * true。函数只读链接表，供 fault 分类避免把内核自身错误误报为用户 fault。
 */
bool insn_may_access_user(unsigned long addr, unsigned long esr)
{
	const struct exception_table_entry *ex = search_exception_tables(addr);

	if (!ex)
		return false;

	switch (ex->type) {
	case EX_TYPE_UACCESS_CPY:
		return cpy_faulted_on_uaccess(ex, esr);
	default:
		return true;
	}
}

/* fixup 字段是相对字段自身地址的链接期偏移，转换为可写入 PC 的绝对地址。 */
static inline unsigned long
get_ex_fixup(const struct exception_table_entry *ex)
{
	return ((unsigned long)&ex->fixup + ex->fixup);
}

/*
 * 按表项编码把 reg_err 写 -EFAULT、reg_zero 清零，再跳到 fixup。regs 是
 * 当前异常现场并被原地修改；返回 true 表示异常已消费，可从新 PC 恢复。
 */
static bool ex_handler_uaccess_err_zero(const struct exception_table_entry *ex,
					struct pt_regs *regs)
{
	int reg_err = FIELD_GET(EX_DATA_REG_ERR, ex->data);
	int reg_zero = FIELD_GET(EX_DATA_REG_ZERO, ex->data);

	pt_regs_write_reg(regs, reg_err, -EFAULT);
	pt_regs_write_reg(regs, reg_zero, 0);

	regs->pc = get_ex_fixup(ex);
	return true;
}

/* copy helper 仅修复真正的 user half；内核地址一侧 fault 返回 false 触发 oops。 */
static bool ex_handler_uaccess_cpy(const struct exception_table_entry *ex,
				   struct pt_regs *regs, unsigned long esr)
{
	/* Do not fix up faults on kernel memory accesses */
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
static bool
ex_handler_load_unaligned_zeropad(const struct exception_table_entry *ex,
				  struct pt_regs *regs)
{
	int reg_data = FIELD_GET(EX_DATA_REG_DATA, ex->data);
	int reg_addr = FIELD_GET(EX_DATA_REG_ADDR, ex->data);
	unsigned long data, addr, offset;

	addr = pt_regs_read_reg(regs, reg_addr);

	offset = addr & 0x7UL;
	addr &= ~0x7UL;

	data = *(unsigned long*)addr;

#ifndef __AARCH64EB__
	data >>= 8 * offset;
#else
	data <<= 8 * offset;
#endif

	pt_regs_write_reg(regs, reg_data, data);

	regs->pc = get_ex_fixup(ex);
	return true;
}

/*
 * 异常表总分发。regs 是当前 CPU 可修改现场，esr 是同步异常原因；无匹配
 * 返回 false 让上层走正常 fault/oops，匹配并成功修复返回 true。未知类型
 * 表示内核与汇编元数据不一致，BUG 比静默返回到错误 PC 更安全。
 */
bool fixup_exception(struct pt_regs *regs, unsigned long esr)
{
	const struct exception_table_entry *ex;

	ex = search_exception_tables(instruction_pointer(regs));
	if (!ex)
		return false;

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

	BUG();
}
