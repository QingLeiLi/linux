/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TICK_SCHED_H
#define _TICK_SCHED_H

#include <linux/hrtimer.h>

/*
 * 本头文件定义 tick-common/oneshot/broadcast 与 tick-sched.c 共享的每 CPU 状态布局。对象由 per-CPU 静态存储
 * 长期拥有，evtdev 是 clockevents core 管理的借用指针，sched_timer 嵌入控制块。flags 通常只在本 CPU
 * IRQ-off 语境修改；统计/诊断字段可被远端无锁读取，只提供瞬时快照而非事务一致性。
 */

enum tick_device_mode {
	TICKDEV_MODE_PERIODIC,
	TICKDEV_MODE_ONESHOT,
};
/* PERIODIC 由设备连续产生节拍；ONESHOT 由 tick 层逐次编程绝对期限，供 highres 与 NO_HZ 共用。 */

struct tick_device {
	struct clock_event_device *evtdev;
	enum tick_device_mode mode;
};
/* 每 CPU tick_device 绑定当前借用的 clockevent 及 tick 层模式；mode 不保证驱动状态回调一定成功。 */

/* The CPU is in the tick idle mode */
/* idle loop 已进入 tick idle 协议，但周期 tick 不一定已经停止。 */
#define TS_FLAG_INIDLE		BIT(0)
/* The idle tick has been stopped */
/* sched tick 已改成一次性期限或彻底取消，恢复路径必须清除此位。 */
#define TS_FLAG_STOPPED		BIT(1)
/*
 * Indicator that the CPU is actively in the tick idle mode;
 * it is reset during irq handling phases.
 */
/* sched_clock 当前按 idle 方式记账；IRQ enter 清、IRQ exit 或 idle loop 再置，避免重复 wake/sleep 通知。 */
#define TS_FLAG_IDLE_ACTIVE	BIT(2)
/* CPU was the last one doing do_timer before going idle */
/* 本 CPU 停 tick 前最后承担全局 jiffies，若尚无人接管仍受 timekeeping max_deferment 限制。 */
#define TS_FLAG_DO_TIMER_LAST	BIT(3)
/* NO_HZ is enabled */
/* 本 CPU 已完成 NO_HZ 后端配置，可参与 idle/full tick stop；不同于全局启动参数 enabled。 */
#define TS_FLAG_NOHZ		BIT(4)
/* High resolution tick mode */
/* sched_timer 作为真实 pinned hard hrtimer 运行；未置时它只为 lowres clockevent 保存 callback/期限。 */
#define TS_FLAG_HIGHRES		BIT(5)

/**
 * struct tick_sched - sched tick emulation and no idle tick control/stats
 *
 * @flags:		State flags gathering the TS_FLAG_* features
 * @got_idle_tick:	Tick timer function has run with @inidle set
 * @stalled_jiffies:	Number of stalled jiffies detected across ticks
 * @last_tick_jiffies:	Value of jiffies seen on last tick
 * @sched_timer:	hrtimer to schedule the periodic tick in high
 *			resolution mode
 * @last_tick:		Store the last tick expiry time when the tick
 *			timer is modified for nohz sleeps. This is necessary
 *			to resume the tick timer operation in the timeline
 *			when the CPU returns from nohz sleep.
 * @next_tick:		Next tick to be fired when in dynticks mode.
 * @idle_waketime:	Time when the idle was interrupted
 * @idle_entrytime:	Time when the idle call was entered
 * @last_jiffies:	Base jiffies snapshot when next event was last computed
 * @timer_expires_base:	Base time clock monotonic for @timer_expires
 * @timer_expires:	Anticipated timer expiration time (in case sched tick is stopped)
 * @next_timer:		Expiry time of next expiring timer for debugging purpose only
 * @idle_expires:	Next tick in idle, for debugging purpose only
 * @idle_calls:		Total number of idle calls
 * @idle_sleeps:	Number of idle calls, where the sched tick was stopped
 * @tick_dep_mask:	Tick dependency mask - is set, if someone needs the tick
 * @check_clocks:	Notification mechanism about clocksource changes
 */
/*
 * tick_sched 同时保存四类状态：flags/依赖决定能否停 tick；sched_timer/last_tick/next_tick 保存周期相位与当前
 * 一次性期限；idle_entry/waketime 和 calls/sleeps 支持时间/调频统计；last_jiffies、timer_expires_base/
 * timer_expires 是 governor 预计算到 stop 提交之间的一次性同源快照。next_timer/idle_expires 仅供诊断。
 * got_idle_tick 是读后清 latch，check_clocks bit0 是异步可合并通知；tick_dep_mask 用 atomic 位支持跨上下文设置。
 */
struct tick_sched {
	/* Common flags */
	unsigned long			flags;

	/* Tick handling: jiffies stall check */
	unsigned int			stalled_jiffies;
	unsigned long			last_tick_jiffies;

	/* Tick handling */
	struct hrtimer			sched_timer;
	ktime_t				last_tick;
	ktime_t				next_tick;
	ktime_t				idle_waketime;
	unsigned int			got_idle_tick;

	/* Idle entry */
	ktime_t				idle_entrytime;

	/* Tick stop */
	unsigned long			last_jiffies;
	u64				timer_expires_base;
	u64				timer_expires;
	u64				next_timer;
	ktime_t				idle_expires;
	unsigned long			idle_calls;
	unsigned long			idle_sleeps;

	/* Full dynticks handling */
	atomic_t			tick_dep_mask;

	/* Clocksource changes */
	unsigned long			check_clocks;
};

/* 返回 @cpu 的静态 per-CPU tick_sched 借用指针；调用者负责 CPU 编号、hotplug 和并发语境。 */
extern struct tick_sched *tick_get_tick_sched(int cpu);

/* 初始化本 CPU sched_timer；@hrtimer 选择真实 highres hrtimer 或 lowres clockevent 后端，要求 IRQ-off。 */
extern void tick_setup_sched_timer(bool hrtimer);
#if defined CONFIG_TICK_ONESHOT
/* CPU teardown 在 hrtimer 迁移前取消 timer并清状态，保留累计 idle 计数。 */
extern void tick_sched_timer_dying(int cpu);
#else
/* 无 oneshot 时没有 per-CPU sched emulation timer 可清理。 */
static inline void tick_sched_timer_dying(int cpu) { }
#endif

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
/* 当前 CPU进入/退出 deep idle 的 oneshot broadcast 控制；返回 0 或 -EBUSY/-EINVAL/-ENODEV 等拒绝原因。 */
extern int __tick_broadcast_oneshot_control(enum tick_broadcast_state state);
#else
/* 无广播设施时始终以 -EBUSY 阻止依赖 broadcast 唤醒的 deep idle，不能伪装成功。 */
static inline int
__tick_broadcast_oneshot_control(enum tick_broadcast_state state)
{
	return -EBUSY;
}
#endif

#endif
