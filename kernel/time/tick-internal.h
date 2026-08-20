/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tick internal variable and functions used by low/high res code
 */
/*
 * tick 子系统内部契约汇总：把 clockevents 设备选择、周期/oneshot 本地 tick、广播、NO_HZ、hrtimer 与
 * timer wheel 的跨实现接口集中在同目录可见范围。调用者必须按配置使用真实实现或下方 inline 退化；本头文件
 * 不取得设备引用，也不提供额外锁。声明涉及的 per-CPU tick_device/hrtimer base 通常要求目标 CPU 稳定，
 * 编程设备常要求本地中断关闭，broadcast mask/全局负责人则由各实现自己的 raw lock 或原子发布协议保护。
 */
#include <linux/hrtimer.h>
#include <linux/tick.h>

#include "timekeeping.h"
#include "tick-sched.h"

struct timer_events {
	u64	local;
	u64	global;
};
/*
 * timer_events 是 NO_HZ/timer migration 的双期限结果：local 是只可由目标 CPU 执行的最早 timer，global 是
 * 可迁移到其他 CPU 代执行的最早 timer；调用者以 KTIME_MAX 初始化，无候选时保持该哨兵，单位均为纳秒。
 */

#ifdef CONFIG_GENERIC_CLOCKEVENTS

# define TICK_DO_TIMER_NONE	-1
# define TICK_DO_TIMER_BOOT	-2
/* NONE 表示暂无全局 do_timer 负责人，BOOT 表示启动期尚未完成首次选择；非负值才是 CPU 编号。 */

DECLARE_PER_CPU(struct tick_device, tick_cpu_device);
extern ktime_t tick_next_period;
extern int tick_do_timer_cpu __read_mostly;
/*
 * tick_cpu_device 记录每 CPU 当前 clockevent 借用指针与 PERIODIC/ONESHOT 模式；tick_next_period 是共享
 * jiffies 周期边界；tick_do_timer_cpu 唯一负责 do_timer/timekeeping。读写后两者须遵循实现中的 READ/WRITE_ONCE
 * 与 release/acquire 协议，CPU hotplug/NO_HZ 会转交负责人。
 */

/*
 * clockevents/tick-common 主接口：setup/handle 安装并执行周期 tick；check/install replacement 在设备注册、
 * rating/affinity 变化时替换 per-CPU 设备；offline/shutdown/suspend/resume 处理 CPU 或系统生命周期；
 * replacement 查询不提交，install 才交换设备。oneshot availability 和 tick_get_device 仅返回能力/借用指针。
 */
extern void tick_setup_periodic(struct clock_event_device *dev, int broadcast);
extern void tick_handle_periodic(struct clock_event_device *dev);
extern void tick_check_new_device(struct clock_event_device *dev);
extern void tick_offline_cpu(unsigned int cpu);
extern void tick_shutdown(void);
extern void tick_suspend(void);
extern void tick_resume(void);
extern bool tick_check_replacement(struct clock_event_device *curdev,
				   struct clock_event_device *newdev);
extern void tick_install_replacement(struct clock_event_device *dev);
extern int tick_is_oneshot_available(void);
extern struct tick_device *tick_get_device(int cpu);

extern int clockevents_tick_resume(struct clock_event_device *dev);
/* Check, if the device is functional or a dummy for broadcast */
/* 仅检查 DUMMY feature：返回 1 表示设备可本地编程，0 表示它只是要求广播代打 tick 的占位设备。 */
static inline int tick_device_is_functional(struct clock_event_device *dev)
{
	return !(dev->features & CLOCK_EVT_FEAT_DUMMY);
}

/* 读取 clockevent core 维护的状态缓存；调用者仍须满足设备注册/锁定生命周期，函数不访问硬件。 */
static inline enum clock_event_state clockevent_get_state(struct clock_event_device *dev)
{
	return dev->state_use_accessors;
}

/* 仅更新 state_use_accessors 缓存，供 clockevents 状态转换实现使用；不会调用驱动 set_state_* 回调。 */
static inline void clockevent_set_state(struct clock_event_device *dev,
					enum clock_event_state state)
{
	dev->state_use_accessors = state;
}

/*
 * clockevents core 接口：shutdown/exchange/switch 改变设备归属或状态；program_event 以 host ktime 编程下次
 * 到期并按 @force 决定过期时是否强制最小 delta；noop 吸收禁用设备事件；update_freq 重算 mult/shift 与
 * min/max delta。调用者必须持实现要求的 clockevents_lock/CPU hotplug 序列并保证设备存活。
 */
extern void clockevents_shutdown(struct clock_event_device *dev);
extern void clockevents_exchange_device(struct clock_event_device *old,
					struct clock_event_device *new);
extern void clockevents_switch_state(struct clock_event_device *dev,
				     enum clock_event_state state);
extern int clockevents_program_event(struct clock_event_device *dev,
				     ktime_t expires, bool force);
extern void clockevents_handle_noop(struct clock_event_device *dev);
extern int __clockevents_update_freq(struct clock_event_device *dev, u32 freq);

/* Broadcasting support */
/* 广播支持把无法在 idle 运行的本地设备转交共享 clockevent；mask 和 device getter 返回内部借用对象。 */
# ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
/*
 * uses/install/is 负责登记本地 CPU 或选择共享设备；suspend/resume 保存恢复广播硬件状态；resume_check 报告
 * 本 CPU 恢复时是否仍需广播；init 建立 masks；handler/update_freq/getters 分别配置回调、频率和只读状态。
 */
extern int tick_device_uses_broadcast(struct clock_event_device *dev, int cpu);
extern void tick_install_broadcast_device(struct clock_event_device *dev, int cpu);
extern int tick_is_broadcast_device(struct clock_event_device *dev);
extern void tick_suspend_broadcast(void);
extern void tick_resume_broadcast(void);
extern bool tick_resume_check_broadcast(void);
extern void tick_broadcast_init(void);
extern void tick_set_periodic_handler(struct clock_event_device *dev, int broadcast);
extern int tick_broadcast_update_freq(struct clock_event_device *dev, u32 freq);
extern struct tick_device *tick_get_broadcast_device(void);
extern struct cpumask *tick_get_broadcast_mask(void);
extern const struct clock_event_device *tick_get_wakeup_device(int cpu);
# else /* !CONFIG_GENERIC_CLOCKEVENTS_BROADCAST: */
/* 禁用广播时安装、查询、周期分发和 suspend/resume 均无副作用；uses/is 返回 0，resume_check 返回 false。 */
static inline void tick_install_broadcast_device(struct clock_event_device *dev, int cpu) { }
static inline int tick_is_broadcast_device(struct clock_event_device *dev) { return 0; }
static inline int tick_device_uses_broadcast(struct clock_event_device *dev, int cpu) { return 0; }
static inline void tick_do_periodic_broadcast(struct clock_event_device *d) { }
static inline void tick_suspend_broadcast(void) { }
static inline void tick_resume_broadcast(void) { }
static inline bool tick_resume_check_broadcast(void) { return false; }
static inline void tick_broadcast_init(void) { }
/* 无广播设备可调频，显式返回 -ENODEV，防止调用者把退化路径误判为成功。 */
static inline int tick_broadcast_update_freq(struct clock_event_device *dev, u32 freq) { return -ENODEV; }

/* Set the periodic handler in non broadcast mode */
/* 非广播构建直接把设备回调设为本地 tick_handle_periodic；@broadcast 在该配置下无意义。 */
static inline void tick_set_periodic_handler(struct clock_event_device *dev, int broadcast)
{
	dev->event_handler = tick_handle_periodic;
}
# endif /* !CONFIG_GENERIC_CLOCKEVENTS_BROADCAST */

#else /* !GENERIC_CLOCKEVENTS: */
/* 没有通用 clockevents 时系统 suspend/resume 不存在 tick 设备状态可保存或恢复。 */
static inline void tick_suspend(void) { }
static inline void tick_resume(void) { }
#endif /* !GENERIC_CLOCKEVENTS */

/* Oneshot related functions */
/* oneshot 接口把本地 clockevent 从周期模式切为逐期限编程，供高精度 timer 与 NO_HZ 共用。 */
#ifdef CONFIG_TICK_ONESHOT
/*
 * setup 安装 handler 和下一期限；program_event 返回设备编程错误；notify/clock_notify 置重评估标志；switch
 * 执行状态切换并返回 errno；resume 恢复设备；mode_active/possible 查询状态/编译能力；check_change 在中断
 * 尾部按 @allow_nohz 决定是否切换；init_highres 以 hrtimer handler 启用高精度模式。
 */
extern void tick_setup_oneshot(struct clock_event_device *newdev,
			       void (*handler)(struct clock_event_device *),
			       ktime_t nextevt);
extern int tick_program_event(ktime_t expires, int force);
extern void tick_oneshot_notify(void);
extern int tick_switch_to_oneshot(void (*handler)(struct clock_event_device *));
extern void tick_resume_oneshot(void);
/* 编译进完整 oneshot 支持即返回 true，不代表当前 CPU 已完成模式切换。 */
static inline bool tick_oneshot_possible(void) { return true; }
extern int tick_oneshot_mode_active(void);
extern void tick_clock_notify(void);
extern int tick_check_oneshot_change(int allow_nohz);
extern int tick_init_highres(void);
#else /* !CONFIG_TICK_ONESHOT: */
/*
 * 禁配时 setup/resume 是不可达契约，误调立即 BUG；program 返回 0 只供被编译保留但不可达的通用调用点消除
 * 分支。notify/clock_notify/check_change 无副作用，possible/mode_active 均为假，不会伪造已启用状态。
 */
static inline
void tick_setup_oneshot(struct clock_event_device *newdev,
			void (*handler)(struct clock_event_device *),
			ktime_t nextevt) { BUG(); }
static inline void tick_resume_oneshot(void) { BUG(); }
static inline int tick_program_event(ktime_t expires, int force) { return 0; }
static inline void tick_oneshot_notify(void) { }
static inline bool tick_oneshot_possible(void) { return false; }
static inline int tick_oneshot_mode_active(void) { return 0; }
static inline void tick_clock_notify(void) { }
static inline int tick_check_oneshot_change(int allow_nohz) { return 0; }
#endif /* !CONFIG_TICK_ONESHOT */

/* Functions related to oneshot broadcasting */
/* oneshot 广播按各 CPU 独立 deadline 维护共享设备，而非周期广播的统一节拍。 */
#if defined(CONFIG_GENERIC_CLOCKEVENTS_BROADCAST) && defined(CONFIG_TICK_ONESHOT)
/*
 * switch 切换广播状态；active/available 返回当前状态与设备能力；check_this_cpu 修正本 CPU 的广播归属；
 * oneshot mask getter 返回内部 cpumask 借用指针，调用者须在广播锁/CPU 稳定语境使用。
 */
extern void tick_broadcast_switch_to_oneshot(void);
extern int tick_broadcast_oneshot_active(void);
extern void tick_check_oneshot_broadcast_this_cpu(void);
bool tick_broadcast_oneshot_available(void);
extern struct cpumask *tick_get_broadcast_oneshot_mask(void);
#else /* !(BROADCAST && ONESHOT): */
/* 缺任一能力时切换/检查为空、active 为 0；available 退化成仅询问本地 oneshot 编译能力。 */
static inline void tick_broadcast_switch_to_oneshot(void) { }
static inline int tick_broadcast_oneshot_active(void) { return 0; }
static inline void tick_check_oneshot_broadcast_this_cpu(void) { }
static inline bool tick_broadcast_oneshot_available(void) { return tick_oneshot_possible(); }
#endif /* !(BROADCAST && ONESHOT) */

#if defined(CONFIG_GENERIC_CLOCKEVENTS_BROADCAST) && defined(CONFIG_HOTPLUG_CPU)
/* CPU offline 时从周期和 oneshot 广播 masks 移除 @cpu；调用者位于 hotplug 串行区。 */
extern void tick_broadcast_offline(unsigned int cpu);
#else
/* 无广播或 hotplug 时不存在需清理的动态 CPU membership。 */
static inline void tick_broadcast_offline(unsigned int cpu) { }
#endif

/* NO_HZ_FULL internal */
/* full dynticks 初始化隔离 CPU 的 tick 停止能力；禁配 stub 保持启动调用点统一。 */
#ifdef CONFIG_NO_HZ_FULL
extern void tick_nohz_init(void);
# else
static inline void tick_nohz_init(void) { }
#endif

#ifdef CONFIG_NO_HZ_COMMON
/* timers_update_nohz 通知静态分支/远端调度刷新；get_jiffies_update 返回上次更新的 monotonic ns 并输出同一快照的 jiffies。 */
extern void timers_update_nohz(void);
extern u64 get_jiffies_update(unsigned long *basej);
# ifdef CONFIG_SMP
extern struct static_key_false timers_migration_enabled;
/*
 * SMP timer migration 接口：fetch 在已锁定远端 bases 时填 local/global 期限；lock/unlock 成对并管理 IRQ；
 * base_is_idle 查询本 CPU base 状态；expire_remote 代目标 CPU 执行可迁移 timer。static key 控制迁移快路径。
 */
extern void fetch_next_timer_interrupt_remote(unsigned long basej, u64 basem,
					      struct timer_events *tevt,
					      unsigned int cpu);
extern void timer_lock_remote_bases(unsigned int cpu);
extern void timer_unlock_remote_bases(unsigned int cpu);
extern bool timer_base_is_idle(void);
extern void timer_expire_remote(unsigned int cpu);
# endif
#else /* CONFIG_NO_HZ_COMMON */
/* 周期 tick 构建无需因 timer 变化重新计算停止期限。 */
static inline void timers_update_nohz(void) { }
#endif

DECLARE_PER_CPU(struct hrtimer_cpu_base, hrtimer_bases);
/* hrtimer_bases 是每 CPU 高精度队列/锁/活动 base 容器；跨 CPU 访问必须遵守 hrtimer 锁与迁移协议。 */

/*
 * get_next_timer_interrupt 合并本 CPU timer wheel 的下一期限；try_to_set_idle 在 IRQ-off 下尝试标记 bases idle，
 * 通过 @idle 返回是否成功并返回期限；timer_clear_idle 在退出 idle 时撤销标志。@basej/@basem 必须是同一快照。
 */
extern u64 get_next_timer_interrupt(unsigned long basej, u64 basem);
u64 timer_base_try_to_set_idle(unsigned long basej, u64 basem, bool *idle);
void timer_clear_idle(void);

#define CLOCK_SET_WALL							\
	(BIT(HRTIMER_BASE_REALTIME) | BIT(HRTIMER_BASE_REALTIME_SOFT) |	\
	 BIT(HRTIMER_BASE_TAI) | BIT(HRTIMER_BASE_TAI_SOFT))

#define CLOCK_SET_BOOT							\
	(BIT(HRTIMER_BASE_BOOTTIME) | BIT(HRTIMER_BASE_BOOTTIME_SOFT))
/* CLOCK_SET_WALL/BOOT 是 hrtimer base 位图：墙钟变化重算 REALTIME/TAI，suspend 增量另重算 BOOTTIME。 */

/*
 * clock_was_set 立即跨 CPU 重算 @bases 指定的 hrtimer offset/到期；delayed 在不宜同步 IPI 的上下文排工作；
 * hrtimers_resume_local 仅恢复当前 CPU 在 suspend 后的 hrtimer 状态。三者不改变用户设置的绝对期限。
 */
void clock_was_set(unsigned int bases);
void clock_was_set_delayed(void);

void hrtimers_resume_local(void);

/* Since jiffies uses a simple TICK_NSEC multiplier
 * conversion, the .shift value could be zero. However
 * this would make NTP adjustments impossible as they are
 * in units of 1/2^.shift. Thus we use JIFFIES_SHIFT to
 * shift both the nominator and denominator the same
 * amount, and give ntp adjustments in units of 1/2^8
 *
 * The value 8 is somewhat carefully chosen, as anything
 * larger can result in overflows. TICK_NSEC grows as HZ
 * shrinks, so values greater than 8 overflow 32bits when
 * HZ=100.
 */
/*
 * jiffies 用 TICK_NSEC 直接换算，本可让 shift=0；但 NTP 校正以 1/2^shift 为单位，必须同时左移分子与分母
 * 保留小数调频能力。默认 8 是精度与 32 位溢出的折中：HZ 越小 TICK_NSEC 越大，HZ<67/34 时依次降为
 * 7/6，避免乘法溢出。这里的 shift 是 jiffies clocksource 换算尺度，不是额外改变 tick 周期。
 */
#if HZ < 34
#define JIFFIES_SHIFT	6
#elif HZ < 67
#define JIFFIES_SHIFT	7
#else
#define JIFFIES_SHIFT	8
#endif

/* 把长度 @cnt、可不含 NUL 的 sysfs 输入规范化复制到 @dst；成功返回原始 cnt、失败 -EINVAL，供 clocksource/event 名称解析共用。 */
extern ssize_t sysfs_get_uname(const char *buf, char *dst, size_t cnt);
