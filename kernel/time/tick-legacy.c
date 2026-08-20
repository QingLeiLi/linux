// SPDX-License-Identifier: GPL-2.0
/*
 * Timer tick function for architectures that lack generic clockevents,
 * consolidated here from m68k/ia64/parisc/arm.
 */
/*
 * 为尚未接入通用 clockevents 状态机的体系结构集中提供周期中断入口；该对象只在 LEGACY_TIMER_TICK=y 时
 * 链接，并使 GENERIC_CLOCKEVENTS 默认关闭。平台驱动仍负责硬件 ack、计算自上次调用遗漏的 @ticks，并在
 * IRQ 关闭时调用本函数；这里仅连接全局 jiffies/timekeeping、当前任务记账和 profiling，不选择或编程设备。
 */

#include <linux/irq.h>
#include <linux/profile.h>
#include <linux/timekeeper_internal.h>

#include "tick-internal.h"

/**
 * legacy_timer_tick() - advances the timekeeping infrastructure
 * @ticks:	number of ticks, that have elapsed since the last call.
 *
 * This is used by platforms that have not been converted to
 * generic clockevents.
 *
 * If 'ticks' is zero, the CPU is not handling timekeeping, so
 * only perform process accounting and profiling.
 *
 * Must be called with interrupts disabled.
 */
/*
 * legacy_timer_tick() - 推进 legacy 平台的一批周期 tick。
 * @ticks>0 表示当前 CPU 承担全局时间推进：持 jiffies_lock 并包围 jiffies_seq 写事务调用 do_timer，一次加入
 * 全部遗漏 tick；解锁后从 clocksource 推进 wall/aux timekeeper。@ticks=0 跳过这两步，表示本 CPU 只处理
 * 本地账务。两条路径都用被中断寄存器判断 user/system，执行一次 update_process_times，并采一个 CPU_PROFILING
 * 样本；因此 @ticks>1 不会把进程记账或 profile 回放多次。无返回值，必须在 IRQ-off 硬中断语境调用；函数
 * 不保存 irq_regs 指针，锁与 seqcount 在返回前闭合。
 *
 * 上方英文说明意为：此接口源自 m68k/ia64/parisc/arm 的合并实现，供未转换到 generic clockevents 的平台；
 * ticks 为距上次调用流逝的节拍数，零表示本 CPU 不负责 timekeeping，仅做进程记账与 profiling。
 */
void legacy_timer_tick(unsigned long ticks)
{
	if (ticks) {
		raw_spin_lock(&jiffies_lock);
		write_seqcount_begin(&jiffies_seq);
		do_timer(ticks);
		write_seqcount_end(&jiffies_seq);
		raw_spin_unlock(&jiffies_lock);
		update_wall_time();
	}
	update_process_times(user_mode(get_irq_regs()));
	profile_tick(CPU_PROFILING);
}
