/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64 异常表运行期结构与修复接口学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5.4（2026-07-27）。
 *
 * asm/asm-extable.h 负责生成字节布局，本头文件则把该布局暴露给 C：
 *
 *   struct exception_table_entry
 *        -> lib/extable.c 排序/查找
 *        -> arch/arm64/mm/extable.c 按 type/data 修复 pt_regs
 *
 * insn 和普通架构表项的 fixup 是相对字段自身地址的 signed 32-bit
 * 偏移。表项被排序搬动时，swap_ex_entry_fixup() 必须补偿 fixup 的
 * 新基址，并把 type/data 与其原 insn 成组搬移。EX_TYPE_BPF 是例外：
 * JIT 直接把 handler 位域存入 fixup，并按指令地址顺序生成表，不把它
 * 交给通用 sorter。异常表发布后只读；查询返回借用指针，当前异常
 * handler 独占并原地修改本 CPU 的 pt_regs。
 *
 * CONFIG_BPF_JIT 只决定 EX_TYPE_BPF 是否有具体 handler。关闭时内联
 * stub 返回 false，让总分发保持统一接口；它不会把未知 BPF fault
 * 伪装成已修复。
 */
#ifndef __ASM_EXTABLE_H
#define __ASM_EXTABLE_H

/*
 * The exception table consists of pairs of relative offsets: the first
 * is the relative offset to an instruction that is allowed to fault,
 * and the second is the relative offset at which the program should
 * continue. No registers are modified, so it is entirely up to the
 * continuation code to figure out what to do.
 *
 * All the routines below use bits of fixup code that are out of line
 * with the main instruction path.  This means when everything is well,
 * we don't even have to jump over them.  Further, they do not intrude
 * on our cache or tlb entries.
 */
/*
 * 异常表由两对相对信息组成：第一项指向允许发生 fault 的指令，
 * 第二项指向恢复后应继续的位置。原英文描述的是基础两字段模型；
 * 当前 arm64 结构还增加 type/data，使通用 handler 可以按策略修改
 * 寄存器。
 *
 * fixup 代码放在主指令路径之外。正常执行不发生 fault 时既无需跳过恢复
 * 代码，也减少其对 I-cache/TLB 工作集的干扰；代价是异常发生时需要查表
 * 和一次非顺序控制流跳转。
 */

/*
 * exception_table_entry - 一个 arm64 可恢复指令及其处理策略的不可变记录。
 *
 * 表项由汇编宏写入 __ex_table，链接器合并，启动或模块加载阶段排序，
 * 随内建内核/模块/BPF 拥有者发布并最终一起回收。发布后 fault handler
 * 只借用读取，不单独引用或释放。
 *
 * @insn：相对 &insn 的 signed 32-bit 偏移，解析后是故障指令虚拟地址；
 * @fixup：通常是相对 &fixup 的恢复地址；EX_TYPE_BPF 会把它重用为 BPF
 *          handler 位域，因此必须结合 type 解释；
 * @type：asm-extable.h 定义的 16-bit 恢复策略编号；
 * @data：策略专用 16-bit 元数据，如寄存器编号或 copy 访问方向。
 *
 * insn 是排序键。fixup/type/data 必须始终与同一 insn 成组移动；
 * 表本身不含锁或引用计数，安全性来自“发布前排序、发布后只读”和
 * 拥有者生命周期。
 */
struct exception_table_entry
{
	int insn, fixup;
	short type, data;
};

/*
 * 告知通用 lib/extable.c：insn/fixup 是基于字段地址的相对编码，排序时
 * 必须使用定制交换函数，而不能执行不补偿偏移的普通结构体交换。
 */
#define ARCH_HAS_RELATIVE_EXTABLE

/*
 * swap_ex_entry_fixup - 完成 arm64 表项除 insn 外的成组交换。
 *
 * @a/@b 指向交换后的两个可写表项位置，@tmp 是交换前 a 的值副本，
 * @delta 是 b-a 的字节距离。宏把来自 b 的 fixup 搬到 a 时加 delta，
 * 把旧 a 搬到 b 时减 delta，使两者仍解析到原绝对 fixup；type/data 是
 * 非相对元数据，直接互换。
 *
 * 宏由 lib/extable.c 的 swap_ex() 在表发布前调用，无返回值和失败路径。
 * do-while(0) 让多条赋值在调用处表现为单一语句，避免条件语句展开歧义。
 * 当前调用对象是链接器/模块生成的普通相对 fixup 表；BPF JIT 表的 fixup
 * 是策略位域，依靠生成顺序保持有序，不能使用本交换宏重新排序。
 */
#define swap_ex_entry_fixup(a, b, tmp, delta)		\
do {							\
	(a)->fixup = (b)->fixup + (delta);		\
	(b)->fixup = (tmp).fixup - (delta);		\
	(a)->type = (b)->type;				\
	(b)->type = (tmp).type;				\
	(a)->data = (b)->data;				\
	(b)->data = (tmp).data;				\
} while (0)

/*
 * insn_may_access_user() - 判断给定 fault 是否可归因于用户地址访问。
 *
 * @addr 是故障指令虚拟地址；@esr 是该同步异常的 ESR_ELx 原始值，二者
 * 都按值输入。返回 true 表示有异常表项且其类型/方向允许视作 uaccess，
 * false 表示无表项或 copy 的读写方向指向内核半边。函数只读表和 ESR，
 * 不修复 regs、不睡眠，供 arm64 fault 分类路径决定是否报告用户访问。
 */
bool insn_may_access_user(unsigned long addr, unsigned long esr);

#ifdef CONFIG_BPF_JIT
/*
 * ex_handler_bpf() - 解释 BPF JIT 专用表项并修复当前异常现场。
 *
 * @ex 是已命中的借用只读 EX_TYPE_BPF 表项；@regs 是当前 CPU 异常现场的
 * 借用可写指针，由调用者独占。返回 true 表示已调整寄存器/PC、异常被
 * 消费，false 表示不能修复。具体 fixup 位域由 BPF JIT 实现定义。
 */
bool ex_handler_bpf(const struct exception_table_entry *ex,
		    struct pt_regs *regs);
#else /* !CONFIG_BPF_JIT */
/*
 * BPF JIT 关闭时没有合法 BPF 表项来源。stub 忽略借用的 ex/regs，
 * 不修改异常现场并返回 false，使上层不能误判为恢复成功。
 */
static inline
bool ex_handler_bpf(const struct exception_table_entry *ex,
		    struct pt_regs *regs)
{
	return false;
}
#endif /* !CONFIG_BPF_JIT */
/* 上述分支只切换 BPF handler 实现，调用签名和 false 失败语义保持一致。 */

/*
 * fixup_exception() - 查找当前 PC 的表项并分派 arm64 恢复策略。
 *
 * @regs 是当前异常的借用可写寄存器现场，不可为 NULL；@esr 是纯输入异常
 * 状态。返回 true 表示 handler 已消费 fault 并建立可恢复现场，false
 * 表示没有表项或选中策略拒绝修复，上层必须继续普通 fault/oops 流程。
 * 函数不取得对象引用、不睡眠；未知 type 表示生成端与运行期 ABI 失配，
 * 实现会 BUG 而不是带着损坏 PC 返回。
 */
bool fixup_exception(struct pt_regs *regs, unsigned long esr);
#endif
/* 结束 arm64 异常表运行期 ABI 声明保护。 */
