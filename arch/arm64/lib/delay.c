// SPDX-License-Identifier: GPL-2.0-only
/*
 * Delay loops based on the OpenRISC implementation.
 *
 * Copyright (C) 2012 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/timex.h>

#include <clocksource/arm_arch_timer.h>

#define USECS_TO_CYCLES(time_usecs)			\
	xloops_to_cycles((time_usecs) * 0x10C7UL)

/*
 * 内核 delay API 的 xloops 使用 2^-32 秒比例。loops_per_jiffy*HZ 是每秒
 * 校准循环数，乘后右移 32 位即可换算为体系计数器 tick。这里不是普通睡眠：
 * 调用者可能处于原子上下文，所以必须忙等，且很短延迟不能交给调度器。
 */
static inline unsigned long xloops_to_cycles(unsigned long xloops)
{
	return (xloops * loops_per_jiffy * HZ) >> 32;
}

/*
 * Force the use of CNTVCT_EL0 in order to have the same base as WFxT.
 * This avoids some annoying issues when CNTVOFF_EL2 is not reset 0 on a
 * KVM host running at EL1 until we do a vcpu_put() on the vcpu. When
 * running at EL2, the effective offset is always 0.
 *
 * Note that userspace cannot change the offset behind our back either,
 * as the vcpu mutex is held as long as KVM_RUN is in progress.
 */
static cycles_t notrace __delay_cycles(void)
{
	/* 读取期间禁止抢占，保证起止读数属于同一 vCPU/计数器上下文。 */
	guard(preempt_notrace)();
	return __arch_counter_get_cntvct_stable();
}

void __delay(unsigned long cycles)
{
	/* 用无符号差值 now-start 比较，可自然容忍固定宽度计数器回绕。 */
	cycles_t start = __delay_cycles();

	if (alternative_has_cap_unlikely(ARM64_HAS_WFXT)) {
		u64 end = start + cycles;

		/*
		 * Start with WFIT. If an interrupt makes us resume
		 * early, use a WFET loop to complete the delay.
		 */
		wfit(end);	/* 有 WFxT 时让处理器低功耗等待到绝对计数值 end。 */
		while ((__delay_cycles() - start) < cycles)
			wfet(end); /* 中断过早唤醒后再次等待，直至满足总时长。 */
	} else 	if (arch_timer_evtstrm_available()) {
		const cycles_t timer_evt_period =
			USECS_TO_CYCLES(ARCH_TIMER_EVT_STREAM_PERIOD_US);

		/* 只在离终点仍超过一个事件周期时 WFE，防止最后阶段等过头。 */
		while ((__delay_cycles() - start + timer_evt_period) < cycles)
			wfe();
	}

	/* 所有路径最后用精确轮询收尾；cpu_relax 也是自旋循环提示。 */
	while ((__delay_cycles() - start) < cycles)
		cpu_relax();
}
EXPORT_SYMBOL(__delay);

inline void __const_udelay(unsigned long xloops)
{
	__delay(xloops_to_cycles(xloops));
}
EXPORT_SYMBOL(__const_udelay);

void __udelay(unsigned long usecs)
{
	/* ceil(2^32/10^6)，向上取整确保不会短于请求时间。 */
	__const_udelay(usecs * 0x10C7UL); /* 2**32 / 1000000 (rounded up) */
}
EXPORT_SYMBOL(__udelay);

void __ndelay(unsigned long nsecs)
{
	/* ceil(2^32/10^9)=5；同样宁可略长，不能提前返回。 */
	__const_udelay(nsecs * 0x5UL); /* 2**32 / 1000000000 (rounded up) */
}
EXPORT_SYMBOL(__ndelay);
