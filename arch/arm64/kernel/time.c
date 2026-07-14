// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/kernel/time.c
 *
 * Copyright (C) 1991, 1992, 1995  Linus Torvalds
 * Modifications for ARM (C) 1994-2001 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/clockchips.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/timex.h>
#include <linux/errno.h>
#include <linux/profile.h>
#include <linux/stacktrace.h>
#include <linux/syscore_ops.h>
#include <linux/timer.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/clocksource.h>
#include <linux/of_clk.h>
#include <linux/acpi.h>

#include <clocksource/arm_arch_timer.h>

#include <asm/thread_info.h>
#include <asm/paravirt.h>

/*
 * profile_pc_cb - 栈回溯回调：找到第一个不在锁函数中的 PC 值
 * @arg: 指向 unsigned long，用于接收找到的 PC 值
 * @pc:  当前栈帧的程序计数器
 *
 * 被 arch_stack_walk() 逐帧回调。跳过内核锁相关函数（spinlock/mutex 等），
 * 找到第一个"真正在做业务"的调用者 PC，用于 profiling 采样点定位。
 * 返回 true 表示继续回溯，返回 false 表示找到目标 PC，停止回溯。
 */
static bool profile_pc_cb(void *arg, unsigned long pc)
{
	unsigned long *prof_pc = arg;

	/* 若当前 PC 在锁函数范围内（in_lock_functions），说明此帧是锁操作本身，
	 * 不是真正的业务代码，继续向上回溯。 */
	if (in_lock_functions(pc))
		return true;

	/* 找到第一个不在锁函数中的 PC，记录并停止回溯。 */
	*prof_pc = pc;
	return false;
}

/*
 * profile_pc - 获取 profiling 采样时的有效程序计数器
 * @regs: 采样时保存的寄存器现场
 *
 * 内核 profiling（如 oprofile/perf）在时钟中断时采样 PC，但如果被采样的代码
 * 恰好在执行锁操作，直接用 regs->pc 会把所有时间都归到锁函数，掩盖真实热点。
 * 此函数通过栈回溯跳过锁函数，返回调用锁的业务代码的 PC，使 profiling 数据
 * 更准确地反映真实的 CPU 时间分布。
 */
unsigned long profile_pc(struct pt_regs *regs)
{
	unsigned long prof_pc = 0;

	/* arch_stack_walk() 从 regs 描述的现场开始逐帧回溯，
	 * 对每帧调用 profile_pc_cb，直到回调返回 false 或栈顶。 */
	arch_stack_walk(profile_pc_cb, &prof_pc, current, regs);

	return prof_pc;
}
EXPORT_SYMBOL(profile_pc);

/*
 * time_init - arm64 时钟子系统初始化
 *
 * 调用时机：start_kernel() 中，调度器初始化之后，中断使能之前。
 * 完成后系统拥有可用的时钟源（clocksource）和时钟事件设备（clockevent），
 * 内核定时器、jiffies、高精度定时器（hrtimer）均可正常工作。
 *
 * 依赖关系：
 *   - 设备树（FDT）已解析（setup_arch 阶段）
 *   - IRQ 子系统已初始化（irqdomain 可用）
 *   - 时钟控制器驱动可被 of_clk_init 探测
 */
void __init time_init(void)
{
	u32 arch_timer_rate;

	/* of_clk_init(NULL)：遍历设备树中的时钟控制器节点（clocks 子树），
	 * 按照 CLK_OF_DECLARE 宏注册的驱动表逐一匹配并初始化时钟控制器。
	 * NULL 表示从根节点开始扫描整棵设备树。
	 * 完成后各外设的时钟源（如 PLL、分频器）可用，定时器才能获得正确的输入频率。
	 * ACPI 系统上此调用为空操作，时钟信息来自 ACPI 表。 */
	of_clk_init(NULL);

	/* timer_probe()：遍历设备树中的定时器节点，按照 TIMER_OF_DECLARE 宏
	 * 注册的驱动表（__timer_of_table section）逐一匹配并调用初始化函数。
	 * arm64 的 generic timer（架构定时器）驱动在此被探测和初始化，同时也
	 * 探测 SoC 上的其他定时器（如 SBSA generic watchdog timer）。
	 * ACPI 系统通过 acpi_probe_device_table(timer) 完成相同工作。
	 * 完成后 clocksource 和 clockevent 框架中已注册至少一个可用设备。 */
	timer_probe();

	/* tick_setup_hrtimer_broadcast()：为 nohz（tickless）系统建立广播定时器。
	 * 在 CONFIG_NO_HZ 配置下，CPU 进入 idle 时会关闭本地 tick（省电），
	 * 但某些定时器事件仍需要在 idle 期间触发（如 timer wheel 到期）。
	 * 广播定时器使用 hrtimer 实现，由一个始终运行的 CPU 代替 idle CPU
	 * 发送广播中断，唤醒目标 CPU 处理到期定时器。
	 * 无 nohz 支持的系统此调用为空操作。 */
	tick_setup_hrtimer_broadcast();

	/* arch_timer_get_rate()：读取 ARM 架构定时器的计数频率（CNTFRQ_EL0 寄存器值）。
	 * ARM 架构定时器以固定频率计数（通常 24MHz 或 100MHz），频率由固件写入
	 * CNTFRQ_EL0，内核通过读取此寄存器获得精确的计数速率。
	 * 返回 0 说明架构定时器未初始化或频率为 0，系统无法计时，直接 panic。 */
	arch_timer_rate = arch_timer_get_rate();
	if (!arch_timer_rate)
		panic("Unable to initialise architected timer.\n");

	/* Calibrate the delay loop directly
	 *
	 * lpj_fine（loops per jiffy，精确版）：udelay/ndelay 等忙等延迟函数的校准值。
	 * 传统做法是在启动时用定时器测量"1 jiffy 内 CPU 能执行多少次空循环"（bogomips），
	 * 耗时较长且不精确。
	 * arm64 有精确的架构定时器，可以直接计算：
	 *   arch_timer_rate（Hz）/ HZ（jiffies/s）= 每 jiffy 对应的定时器周期数
	 * 这个值直接作为 lpj_fine，比忙等测量更快更准确。
	 * 之后 udelay(N) 本质上是等待 N*lpj_fine/1000000 个定时器周期。 */
	lpj_fine = arch_timer_rate / HZ;

	/* pv_time_init()：初始化半虚拟化（paravirt）时间支持。
	 * 运行在虚拟机中时，通过 ARM SMCCC（Secure Monitor Call Calling Convention）
	 * 向 hypervisor 查询是否支持 PV_TIME_ST（Stolen Time，被偷时间）特性。
	 * 被偷时间：虚拟机等待 hypervisor 调度的时间，CPU 看起来在运行但实际被挂起。
	 * 若 hypervisor 支持，注册 stolen_time_cpu_online 热插拔回调，
	 * 并通过 static_call 将 pv_steal_clock 替换为读取共享内存页的实现，
	 * 使内核调度器能感知"被偷时间"并调整负载均衡决策。
	 * 裸机（bare metal）上 has_pv_steal_clock() 返回 false，此函数立即返回。 */
	pv_time_init();
}
