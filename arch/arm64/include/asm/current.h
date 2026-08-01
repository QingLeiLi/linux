/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64 当前任务定位学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 文件职责：把“当前 CPU 正在执行哪个 task_struct”转换为低成本 C 接口
 * get_current()/current。arm64 在 EL1 运行内核代码时借用 SP_EL0 保存当前
 * task_struct 地址；异常入口和 cpu_switch_to() 负责维护该寄存器，本文件只读取，
 * 不负责创建、引用或切换任务。
 *
 * 主要路径：
 *   启动/异常入口或 cpu_switch_to() -> 写 SP_EL0 = next task
 *   内核 C 代码 -> current -> get_current() -> MRS SP_EL0
 *
 * 生命周期与并发：返回值是当前任务的借用指针，不增加 task_struct 引用。抢占或
 * 中断可以暂时运行别的上下文，但当前调用恢复执行时仍属于原任务；不能把裸指针
 * 当作跨越任意任务生命周期边界的持有引用。用户态可以改写自己的 SP_EL0 语义，
 * 因此 EL0 异常入口会从 per-CPU 影子恢复内核所需的 current 值。
 *
 * 方案权衡：专用系统寄存器读取避免从栈地址反推 thread_info，也让内核栈布局更
 * 自由；代价是所有入口、任务切换和特殊固件/虚拟化路径都必须严格保存恢复 SP_EL0。
 */
#ifndef __ASM_CURRENT_H
#define __ASM_CURRENT_H

/* include guard 只防止重复定义，不影响每次 current 展开时的寄存器读取。 */

#include <linux/compiler.h>
/* compiler.h 提供 __always_inline，确保这一热路径没有真实函数调用边界。 */

#ifndef __ASSEMBLER__
/* 汇编源只需要预处理本头文件时跳过下列 C 声明、类型转换和内联 asm。 */

struct task_struct;
/*
 * 前向声明足以表达返回类型：本文件只传递 task_struct 指针，不访问字段，因而
 * 无需包含庞大的 <linux/sched.h>，也避免形成循环头文件依赖。
 */

/*
 * We don't use read_sysreg() as we want the compiler to cache the value where
 * possible.
 */
/*
 * 这里刻意不用 read_sysreg()：希望编译器在同一段 C 控制流中复用已经读出的
 * SP_EL0，而不是把每个 current 都强制变成一次系统寄存器访问。对同一任务而言，
 * 即使中途被抢占，恢复到该调用点时 current 仍相同，因此这种缓存不破坏语义；
 * 真正的任务切换发生在调度器特殊上下文边界，由底层汇编维护寄存器。
 */
/*
 * get_current() - 返回当前执行任务的 task_struct 借用指针。
 *
 * 调用关系：几乎所有使用 current 的内核 C 代码经下方宏内联到这里；SP_EL0 的
 * 写端位于启动入口、EL0 异常入口和 cpu_switch_to()。
 *
 * 入参：无。可在进程、异常和中断上下文调用，不取锁、不睡眠、不失败。调用点
 * 必须处于 SP_EL0 已按内核约定装载 current 的 EL1 上下文。
 *
 * 返回：非 NULL 的当前 task_struct 裸指针；不增加引用、不转移 ownership，也不
 * 保证其字段不受并发修改。指针用于当前执行上下文通常天然具有存活保证，若要在
 * 当前任务之外或越过退出边界保存，仍须遵循相应 task 引用协议。
 */
static __always_inline struct task_struct *get_current(void)
{
	/* sp_el0 只在本次内联展开内承接系统寄存器中的地址，无独立生命周期。 */
	unsigned long sp_el0;

	/*
	 * MRS 把 SP_EL0 的 64 位原始值读入通用寄存器。asm 没有 volatile，允许编译器
	 * 消除同一控制流中的重复读取；它没有内存 clobber，也不提供内存屏障语义。
	 */
	asm ("mrs %0, sp_el0" : "=r" (sp_el0));

	/* 寄存器按约定存放 task_struct 地址；转换不获取引用，也不访问目标内存。 */
	return (struct task_struct *)sp_el0;
}

/*
 * current 保持跨体系结构统一的对象式写法，同时仍在使用点展开 get_current()。
 * 它不是全局变量：不同 CPU 同时求值得到各自正在执行的任务。
 */
#define current get_current()

#endif /* __ASSEMBLER__ */
/* 上方英文行尾注释说明 C 专用区域到此结束，汇编预处理不会看到其中定义。 */

#endif /* __ASM_CURRENT_H */

/* 上方英文行尾注释说明 __ASM_CURRENT_H include guard 到此结束。 */
