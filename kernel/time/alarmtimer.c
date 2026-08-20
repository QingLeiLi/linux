// SPDX-License-Identifier: GPL-2.0
/*
 * Alarmtimer interface
 *
 * This interface provides a timer which is similar to hrtimers,
 * but triggers a RTC alarm if the box is suspend.
 *
 * This interface is influenced by the Android RTC Alarm timer
 * interface.
 *
 * Copyright (C) 2010 IBM Corporation
 *
 * Author: John Stultz <john.stultz@linaro.org>
 */
/*
 * 学习总览：alarmtimer 用两层定时机制统一“系统运行中按高分辨率到期”和“系统挂起时由 RTC 唤醒”。
 * 该接口语义类似 hrtimer，并受 Android RTC Alarm timer 接口影响；原作者与版权信息见上方保留块。
 * 每个 alarm 同时含按 REALTIME/BOOTTIME 排序的 timerqueue_node 与运行期 hrtimer；base 锁保护软件队列，
 * RTC_CLASS 分支在 suspend 时从所有 base 及 freezer 记录中选最早期限，只向硬件编程一个唤醒闹钟。
 * POSIX_TIMERS 分支再把这套原语接入 CLOCK_REALTIME_ALARM/CLOCK_BOOTTIME_ALARM 的 timer 与 nanosleep。
 * alarm 对象及 callback data 的生命周期由调用者负责；取消后才能释放，队列状态和 hrtimer 状态需成对维护。
 */
#include <linux/time.h>
#include <linux/hrtimer.h>
#include <linux/timerqueue.h>
#include <linux/rtc.h>
#include <linux/sched/signal.h>
#include <linux/sched/debug.h>
#include <linux/alarmtimer.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/posix-timers.h>
#include <linux/workqueue.h>
#include <linux/freezer.h>
#include <linux/compat.h>
#include <linux/module.h>
#include <linux/time_namespace.h>

#include "posix-timers.h"

#define CREATE_TRACE_POINTS
#include <trace/events/alarmtimer.h>

/**
 * struct alarm_base - Alarm timer bases
 * @lock:		Lock for synchronized access to the base
 * @timerqueue:		Timerqueue head managing the list of events
 * @get_ktime:		Function to read the time correlating to the base
 * @get_timespec:	Function to read the namespace time correlating to the base
 * @base_clockid:	clockid for the base
 */
/*
 * alarm_base 是每种可用 alarm 时钟的静态常驻根：lock 串行 timerqueue 与 alarm->state，get_ktime 提供
 * root namespace 的绝对 ktime 坐标，get_timespec 提供面向当前任务时间命名空间的 timespec，base_clockid
 * 决定底层 hrtimer 的 clock base。alarm_bases 仅索引 [0, ALARM_NUMTYPE)，freezer 枚举值不能用于下标。
 */
static struct alarm_base {
	spinlock_t		lock;
	struct timerqueue_head	timerqueue;
	ktime_t			(*get_ktime)(void);
	void			(*get_timespec)(struct timespec64 *tp);
	clockid_t		base_clockid;
} alarm_bases[ALARM_NUMTYPE];

#if defined(CONFIG_POSIX_TIMERS) || defined(CONFIG_RTC_CLASS)
/* freezer information to handle clock_nanosleep triggered wakeups */
/* freezer 记录用于处理由 clock_nanosleep 触发的唤醒：保存冻结任务中最早的相对差、绝对期限和 trace 类型。 */
static enum alarmtimer_type freezer_alarmtype;
static ktime_t freezer_expires;
static ktime_t freezer_delta;
static DEFINE_SPINLOCK(freezer_delta_lock);
#endif

#ifdef CONFIG_RTC_CLASS
/* rtc timer and device for setting alarm wakeups at suspend */
/* rtctimer 是 suspend 阶段复用的一次性 RTC timer；rtcdev_lock 保护全局所选设备指针的发布与读取。 */
static struct rtc_timer		rtctimer;
static struct rtc_device	*rtcdev;
static DEFINE_SPINLOCK(rtcdev_lock);

/**
 * alarmtimer_get_rtcdev - Return selected rtcdevice
 *
 * This function returns the rtc device to use for wakealarms.
 */
/*
 * alarmtimer_get_rtcdev() - 在 irqsave 自旋锁下读取已选 RTC 设备。
 * 无参数；返回可空借用指针。成功选择设备时 add 回调额外持有 device 与 owner module 引用，因而调用者
 * 无需另取引用；本函数不睡眠、不转移 ownership，配置关闭时头文件桩直接返回 NULL。
 */
struct rtc_device *alarmtimer_get_rtcdev(void)
{
	struct rtc_device *ret;

	guard(spinlock_irqsave)(&rtcdev_lock);
	ret = rtcdev;

	return ret;
}
EXPORT_SYMBOL_GPL(alarmtimer_get_rtcdev);

/*
 * alarmtimer_rtc_add_device() - class interface 发现 RTC 时尝试把首个可唤醒 alarm 设备选为全局后端。
 * @dev 必须是 rtc_class 设备；拒绝已有后端、无 RTC_FEATURE_ALARM 或父设备不可 wakeup。先注册名为
 * alarmtimer 的 platform child 并启用其 wakeup，再在 rtcdev_lock 下二次确认唯一性并取得 owner module
 * 与 device 引用后发布 rtcdev；竞争失败则注销临时 child。成功返回 0，失败返回 -EBUSY 或当前实现的 -1。
 * class 回调可睡眠；成功引用在本文件生命周期内保持，接口没有设备移除时的重新选择协议。
 */
static int alarmtimer_rtc_add_device(struct device *dev)
{
	struct rtc_device *rtc = to_rtc_device(dev);
	struct platform_device *pdev;
	int ret = 0;

	if (rtcdev)
		return -EBUSY;

	if (!test_bit(RTC_FEATURE_ALARM, rtc->features))
		return -1;
	if (!device_may_wakeup(rtc->dev.parent))
		return -1;

	pdev = platform_device_register_data(dev, "alarmtimer",
					     PLATFORM_DEVID_AUTO, NULL, 0);
	if (!IS_ERR(pdev))
		device_init_wakeup(&pdev->dev, true);

	scoped_guard(spinlock_irqsave, &rtcdev_lock) {
		if (!IS_ERR(pdev) && !rtcdev && try_module_get(rtc->owner)) {
			rtcdev = rtc;
			/* hold a reference so it doesn't go away */
			/* 持有设备引用，保证发布到 rtcdev 后即使外部解绑也不会释放对象。 */
			get_device(dev);
			pdev = NULL;
		} else {
			ret = -1;
		}
	}

	platform_device_unregister(pdev);
	return ret;
}

/* 初始化 suspend 路径复用的静态 rtc_timer；NULL callback 表示只依赖 RTC 唤醒系统。 */
static inline void alarmtimer_rtc_timer_init(void)
{
	rtc_timer_init(&rtctimer, NULL, NULL);
}

static struct class_interface alarmtimer_rtc_interface = {
	.add_dev = &alarmtimer_rtc_add_device,
};

/* 注册 rtc_class 观察者；注册过程会为已有/后续设备调用 add_dev，返回 0 或 class core 错误。 */
static int alarmtimer_rtc_interface_setup(void)
{
	alarmtimer_rtc_interface.class = &rtc_class;
	return class_interface_register(&alarmtimer_rtc_interface);
}
/* 撤销 class observer；仅用于 init 后续失败回滚，不释放已选择 RTC 的长期引用。 */
static void alarmtimer_rtc_interface_remove(void)
{
	class_interface_unregister(&alarmtimer_rtc_interface);
}
#else
/* RTC_CLASS 关闭时 setup 桩返回成功，使公共 init 可继续建立纯软件 alarm base。 */
static inline int alarmtimer_rtc_interface_setup(void) { return 0; }
/* RTC_CLASS 关闭时 remove 桩无资源可撤销。 */
static inline void alarmtimer_rtc_interface_remove(void) { }
/* RTC_CLASS 关闭时 init 桩不初始化共享硬件 timer。 */
static inline void alarmtimer_rtc_timer_init(void) { }
#endif

/**
 * alarmtimer_enqueue - Adds an alarm timer to an alarm_base timerqueue
 * @base: pointer to the base where the timer is being run
 * @alarm: pointer to alarm being enqueued.
 *
 * Adds alarm to a alarm_base timerqueue
 *
 * Must hold base->lock when calling.
 */
/*
 * alarmtimer_enqueue() - 在已持有 @base->lock 时把 @alarm 按 node.expires 排入该 base。
 * 若状态显示已入队，先删除旧位置以支持原对象重设期限；随后插入并置 ENQUEUED。无返回值、不可睡眠，
 * 调用者必须保证 alarm->type 与 base 匹配且对象活到 dequeue/callback 完成。
 */
static void alarmtimer_enqueue(struct alarm_base *base, struct alarm *alarm)
{
	if (alarm->state & ALARMTIMER_STATE_ENQUEUED)
		timerqueue_del(&base->timerqueue, &alarm->node);

	timerqueue_add(&base->timerqueue, &alarm->node);
	alarm->state |= ALARMTIMER_STATE_ENQUEUED;
}

/**
 * alarmtimer_dequeue - Removes an alarm timer from an alarm_base timerqueue
 * @base: pointer to the base where the timer is running
 * @alarm: pointer to alarm being removed
 *
 * Removes alarm to a alarm_base timerqueue
 *
 * Must hold base->lock when calling.
 */
/*
 * alarmtimer_dequeue() - 在已持有 @base->lock 时幂等移除 @alarm。
 * 未置 ENQUEUED 时直接返回；否则从 timerqueue 删除并清状态位。它只维护软件队列，不取消底层 hrtimer。
 */
static void alarmtimer_dequeue(struct alarm_base *base, struct alarm *alarm)
{
	if (!(alarm->state & ALARMTIMER_STATE_ENQUEUED))
		return;

	timerqueue_del(&base->timerqueue, &alarm->node);
	alarm->state &= ~ALARMTIMER_STATE_ENQUEUED;
}


/**
 * alarmtimer_fired - Handles alarm hrtimer being fired.
 * @timer: pointer to hrtimer being run
 *
 * When a alarm timer fires, this runs through the timerqueue to
 * see which alarms expired, and runs those. If there are more alarm
 * timers queued for the future, we set the hrtimer to fire when
 * the next future alarm timer expires.
 */
/*
 * alarmtimer_fired() - 单个 alarm 内嵌 hrtimer 的到期 callback。
 * @timer 反推 alarm/base；在 base irqsave 锁下先从 suspend 扫描使用的软件队列摘除，然后无锁调用可空
 * alarm->function(alarm, 当前 base 时间) 并 trace。callback 可由功能层自行 forward/rearm，但本层固定返回
 * HRTIMER_NORESTART。调用者不得在 callback/cancel 同步完成前释放 alarm；function 运行在 hrtimer 上下文。
 * 上方旧说明所称“遍历队列并设置下一个 hrtimer”不符合当前实现：每个 alarm 自有 hrtimer，队列仅供 suspend 扫描。
 */
static enum hrtimer_restart alarmtimer_fired(struct hrtimer *timer)
{
	struct alarm *alarm = container_of(timer, struct alarm, timer);
	struct alarm_base *base = &alarm_bases[alarm->type];

	scoped_guard(spinlock_irqsave, &base->lock)
		alarmtimer_dequeue(base, alarm);

	if (alarm->function)
		alarm->function(alarm, base->get_ktime());

	trace_alarmtimer_fired(alarm, base->get_ktime());
	return HRTIMER_NORESTART;
}

/*
 * alarm_expires_remaining() - 以 alarm 所属 base 的 root 时钟计算 node.expires-now。
 * @alarm 为稳定借用对象；返回有符号 ktime，可为负且不检查 ENQUEUED/hrtimer active，适合诊断或取消后
 * 计算经过时间。无锁读取 expires，调用者须避免与同一 alarm 的 start/forward 并发修改。
 */
ktime_t alarm_expires_remaining(const struct alarm *alarm)
{
	struct alarm_base *base = &alarm_bases[alarm->type];
	return ktime_sub(alarm->node.expires, base->get_ktime());
}
EXPORT_SYMBOL_GPL(alarm_expires_remaining);

#ifdef CONFIG_RTC_CLASS
/**
 * alarmtimer_suspend - Suspend time callback
 * @dev: unused
 *
 * When we are going into suspend, we look through the bases
 * to see which is the soonest timer to expire. We then
 * set an rtc timer to fire that far into the future, which
 * will wake us from suspend.
 */
/*
 * alarmtimer_suspend() - PM suspend 回调，为最早 wake alarm 编程一次 RTC 唤醒。
 * @dev 是 alarmtimer platform child，用于发布临近期限的 wakeup event。先在 freezer_delta_lock 下取走并
 * 清空冻结任务候选，再逐个在 base irqsave 锁下仅复制队首 expires，解锁后换算 delta 并取最小值。
 * 无 RTC 或无候选返回 0；最早期限不足 2 秒（含已过期）发布 2 秒 wakeup event 并返回 -EBUSY 拒绝本次
 * suspend。否则取消旧 rtctimer，读取 RTC 墙钟，把相对 delta 按硬件上限裁剪后启动一次性 RTC timer。
 * rtc_timer_start 的 0/负错误原样返回，失败再发布 1 秒 wakeup event；扫描是逐 base 快照，不承诺全局原子性。
 * 因此上方旧 kernel-doc 的“@dev unused”已过时，当前两条临近/失败路径都会使用 @dev。
 */
static int alarmtimer_suspend(struct device *dev)
{
	ktime_t min, now, expires;
	struct rtc_device *rtc;
	struct rtc_time tm;
	int i, ret, type;

	scoped_guard(spinlock_irqsave, &freezer_delta_lock) {
		min = freezer_delta;
		expires = freezer_expires;
		type = freezer_alarmtype;
		freezer_delta = 0;
	}

	rtc = alarmtimer_get_rtcdev();
	/* If we have no rtcdev, just return */
	/* 没有可用 RTC 后端时无法提供挂起唤醒，但不阻止系统 suspend。 */
	if (!rtc)
		return 0;

	/* Find the soonest timer to expire */
	/* 分别读取各 base 队首；锁外计算允许并发重设，PM/freezer 协议负责限制实际竞态窗口。 */
	for (i = 0; i < ALARM_NUMTYPE; i++) {
		struct alarm_base *base = &alarm_bases[i];
		struct timerqueue_node *next;
		ktime_t next_expires;
		ktime_t delta;

		scoped_guard(spinlock_irqsave, &base->lock) {
			next = timerqueue_getnext(&base->timerqueue);
			if (next)
				next_expires = next->expires;
		}
		if (!next)
			continue;
		delta = ktime_sub(next_expires, base->get_ktime());
		if (!min || (delta < min)) {
			expires = next_expires;
			min = delta;
			type = i;
		}
	}
	if (min == 0)
		return 0;

	if (ktime_to_ns(min) < 2 * NSEC_PER_SEC) {
		pm_wakeup_event(dev, 2 * MSEC_PER_SEC);
		return -EBUSY;
	}

	trace_alarmtimer_suspend(expires, type);

	/* Setup an rtc timer to fire that far in the future */
	/* RTC 使用自身墙钟坐标，因此只携带软件候选的相对 delta，不直接复用 REALTIME/BOOTTIME expires。 */
	rtc_timer_cancel(rtc, &rtctimer);
	rtc_read_time(rtc, &tm);
	now = rtc_tm_to_ktime(tm);

	/*
	 * If the RTC alarm timer only supports a limited time offset, set the
	 * alarm time to the maximum supported value.
	 * The system may wake up earlier (possibly much earlier) than expected
	 * when the alarmtimer runs. This is the best the kernel can do if
	 * the alarmtimer exceeds the time that the rtc device can be programmed
	 * for.
	 */
	/* RTC 偏移能力不足时钳到最大值，代价是系统可能提前唤醒并在恢复后再次等待。 */
	min = rtc_bound_alarmtime(rtc, min);

	now = ktime_add(now, min);

	/* Set alarm, if in the past reject suspend briefly to handle */
	/* 启动失败（包括换算后已落在过去）时用 wakeup event 暂时阻止立即再次休眠。 */
	ret = rtc_timer_start(rtc, &rtctimer, now, 0);
	if (ret < 0)
		pm_wakeup_event(dev, MSEC_PER_SEC);
	return ret;
}

/*
 * alarmtimer_resume() - PM resume 回调撤销 suspend 阶段编程的共享 RTC timer。
 * @dev 未使用；若已有后端则 rtc_timer_cancel 在 RTC ops mutex 下同步摘除，最终总返回 0。
 */
static int alarmtimer_resume(struct device *dev)
{
	struct rtc_device *rtc;

	rtc = alarmtimer_get_rtcdev();
	if (rtc)
		rtc_timer_cancel(rtc, &rtctimer);
	return 0;
}

#else
/* RTC_CLASS 关闭时 suspend 桩忽略 @dev 并返回 0，不阻止系统进入低功耗状态。 */
static int alarmtimer_suspend(struct device *dev)
{
	return 0;
}

/* RTC_CLASS 关闭时 resume 桩忽略 @dev 并返回 0，没有硬件 timer 可撤销。 */
static int alarmtimer_resume(struct device *dev)
{
	return 0;
}
#endif

/*
 * __alarm_init() - 初始化与 hrtimer 无关的 alarm 公共字段。
 * @alarm 由调用者拥有，@type 必须是可用 base，@function 可空；重置 node、callback、类型和 inactive 状态，
 * 不初始化/销毁内嵌 hrtimer。调用者应在对象未排队且 callback 不运行时使用。
 */
static void
__alarm_init(struct alarm *alarm, enum alarmtimer_type type,
	     void (*function)(struct alarm *, ktime_t))
{
	timerqueue_init(&alarm->node);
	alarm->function = function;
	alarm->type = type;
	alarm->state = ALARMTIMER_STATE_INACTIVE;
}

/**
 * alarm_init - Initialize an alarm structure
 * @alarm: ptr to alarm to be initialized
 * @type: the type of the alarm
 * @function: callback that is run when the alarm fires
 */
/*
 * alarm_init() - 初始化普通生命周期 alarm。
 * 先按 @type 对应 base_clockid 将内嵌 hrtimer 配成绝对模式/alarmtimer_fired，再初始化软件字段。
 * @alarm 的存储与释放仍归调用者；函数无失败返回，type 越界不会被检查，初始化后方可 start/cancel。
 */
void alarm_init(struct alarm *alarm, enum alarmtimer_type type,
		void (*function)(struct alarm *, ktime_t))
{
	hrtimer_setup(&alarm->timer, alarmtimer_fired, alarm_bases[type].base_clockid,
		      HRTIMER_MODE_ABS);
	__alarm_init(alarm, type, function);
}
EXPORT_SYMBOL_GPL(alarm_init);

/**
 * alarm_start_timer - Sets an alarm to fire
 * @alarm:	Pointer to alarm to set
 * @expires:	Expiry time
 * @relative:	True if @expires is relative
 *
 * Returns: True if the alarm was queued. False if it already expired
 */
/*
 * alarm_start_timer() - 设置或重设 @alarm 的到期时间。
 * @expires 在 @relative=true 时是相对所属 root base 当前时间的时长，否则是同一 base 的绝对值；使用
 * ktime_add_safe 防溢出。base irqsave 锁下先更新 node/软件队列，再启动绝对 user hrtimer；若期限已过，
 * hrtimer 接口返回 false 且不会执行 callback，本函数撤销软件入队并返回 false，调用者必须同步完成事件。
 * true 表示已排队。可替换已启动对象，但对象生命周期必须稳定；锁内不允许调用会睡眠的路径。
 */
bool alarm_start_timer(struct alarm *alarm, ktime_t expires, bool relative)
{
	struct alarm_base *base = &alarm_bases[alarm->type];

	if (relative)
		expires = ktime_add_safe(expires, base->get_ktime());

	trace_alarmtimer_start(alarm, base->get_ktime());

	guard(spinlock_irqsave)(&base->lock);
	alarm->node.expires = expires;
	alarmtimer_enqueue(base, alarm);
	if (!hrtimer_start_range_ns_user(&alarm->timer, expires, 0, HRTIMER_MODE_ABS)) {
		alarmtimer_dequeue(base, alarm);
		return false;
	}
	return true;
}
EXPORT_SYMBOL_GPL(alarm_start_timer);

/**
 * alarm_try_to_cancel - Tries to cancel an alarm timer
 * @alarm: ptr to alarm to be canceled
 *
 * Returns 1 if the timer was canceled, 0 if it was not running,
 * and -1 if the callback was running
 */
/*
 * alarm_try_to_cancel() - 非等待式取消 @alarm。
 * 在 base irqsave 锁下调用 hrtimer_try_to_cancel；返回 1 表示撤销 active timer，0 表示本就 inactive，负值
 * 表示 callback 正运行。仅在返回 >=0 时同步摘除软件队列，随后无锁 trace；不保证负值时 callback 已结束。
 */
int alarm_try_to_cancel(struct alarm *alarm)
{
	struct alarm_base *base = &alarm_bases[alarm->type];
	int ret;

	scoped_guard(spinlock_irqsave, &base->lock) {
		ret = hrtimer_try_to_cancel(&alarm->timer);
		if (ret >= 0)
			alarmtimer_dequeue(base, alarm);
	}

	trace_alarmtimer_cancel(alarm, base->get_ktime());
	return ret;
}
EXPORT_SYMBOL_GPL(alarm_try_to_cancel);


/**
 * alarm_cancel - Spins trying to cancel an alarm timer until it is done
 * @alarm: ptr to alarm to be canceled
 *
 * Returns 1 if the timer was canceled, 0 if it was not active.
 */
/*
 * alarm_cancel() - 同步取消 alarm，并等待可能正在执行的 hrtimer callback。
 * 循环调用 try；负值时在不持 base 锁的情况下 hrtimer_cancel_wait_running，再重试以封闭 callback 自重设等
 * 状态变化。返回最终 1/0；可睡眠，不能从该 alarm 自身 callback 或原子上下文调用。释放对象前调用者还须
 * 先阻止其他并发 start/rearm 来源，否则它们可在 cancel 返回后重新激活 timer。
 */
int alarm_cancel(struct alarm *alarm)
{
	for (;;) {
		int ret = alarm_try_to_cancel(alarm);
		if (ret >= 0)
			return ret;
		hrtimer_cancel_wait_running(&alarm->timer);
	}
}
EXPORT_SYMBOL_GPL(alarm_cancel);


/*
 * alarm_forward() - 把周期 alarm 的 node.expires 从旧相位推进到严格晚于 @now 的下一次期限。
 * @interval 必须为正，且调用者负责对同一 alarm 串行化；若 now 早于旧期限不修改并返回 0。否则用整除
 * 一次跨过完整周期，精确落在 now 时额外加一周期，ktime_add_safe 处理最终溢出。返回跨过/推进的周期数，
 * 至少为 1；它只修改软件 expires，不入队也不启动 hrtimer，通常随后调用 alarm_start_timer。
 */
u64 alarm_forward(struct alarm *alarm, ktime_t now, ktime_t interval)
{
	u64 overrun = 1;
	ktime_t delta;

	delta = ktime_sub(now, alarm->node.expires);

	if (delta < 0)
		return 0;

	if (unlikely(delta >= interval)) {
		s64 incr = ktime_to_ns(interval);

		overrun = ktime_divns(delta, incr);

		alarm->node.expires = ktime_add_ns(alarm->node.expires,
							incr*overrun);

		if (alarm->node.expires > now)
			return overrun;
		/*
		 * This (and the ktime_add() below) is the
		 * correction for exact:
		 */
		/* 整除结果让 expires 恰好等于 now 时，再计一次，确保新期限严格位于未来。 */
		overrun++;
	}

	alarm->node.expires = ktime_add_safe(alarm->node.expires, interval);
	return overrun;
}
EXPORT_SYMBOL_GPL(alarm_forward);

/*
 * alarm_forward_now() - 以 @alarm 所属 root base 的当前时间调用 alarm_forward。
 * @interval 同样必须为正；返回跨越周期数，仅更新 node.expires，不负责加锁、入队或启动。
 */
u64 alarm_forward_now(struct alarm *alarm, ktime_t interval)
{
	struct alarm_base *base = &alarm_bases[alarm->type];

	return alarm_forward(alarm, base->get_ktime(), interval);
}
EXPORT_SYMBOL_GPL(alarm_forward_now);

#ifdef CONFIG_POSIX_TIMERS

/*
 * alarmtimer_freezerset() - 记录因 freezer 中断的 alarm nanosleep 中最早唤醒候选。
 * @absexp 是对应 root alarm base 的绝对期限，@type 仅接受 REALTIME/BOOTTIME；转换为仅供 trace 的
 * *_FREEZER 类型，非法值 WARN_ONCE 后返回。用当前 base 时间求 delta，在 freezer_delta_lock 下以 0 为
 * “未记录”哨兵保留最小 delta 及配套 expires/type，供 suspend 一次性取走。可在冻结进程路径调用，不睡眠。
 */
static void alarmtimer_freezerset(ktime_t absexp, enum alarmtimer_type type)
{
	struct alarm_base *base;
	ktime_t delta;

	switch(type) {
	case ALARM_REALTIME:
		base = &alarm_bases[ALARM_REALTIME];
		type = ALARM_REALTIME_FREEZER;
		break;
	case ALARM_BOOTTIME:
		base = &alarm_bases[ALARM_BOOTTIME];
		type = ALARM_BOOTTIME_FREEZER;
		break;
	default:
		WARN_ONCE(1, "Invalid alarm type: %d\n", type);
		return;
	}

	delta = ktime_sub(absexp, base->get_ktime());

	guard(spinlock_irqsave)(&freezer_delta_lock);
	if (!freezer_delta || (delta < freezer_delta)) {
		freezer_delta = delta;
		freezer_expires = absexp;
		freezer_alarmtype = type;
	}
}

/**
 * clock2alarm - helper that converts from clockid to alarmtypes
 * @clockid: clockid.
 */
/*
 * clock2alarm() - 把 POSIX alarm clock id 映射为可索引 alarm type。
 * CLOCK_REALTIME_ALARM 返回 ALARM_REALTIME；其他值按 BOOTTIME 返回，但若并非 CLOCK_BOOTTIME_ALARM 会
 * WARN_ON_ONCE。调用链应先由 POSIX clock 表保证输入合法；函数无失败码，不能把任意 clockid 当校验接口。
 */
static enum alarmtimer_type clock2alarm(clockid_t clockid)
{
	if (clockid == CLOCK_REALTIME_ALARM)
		return ALARM_REALTIME;

	WARN_ON_ONCE(clockid != CLOCK_BOOTTIME_ALARM);
	return ALARM_BOOTTIME;
}

/**
 * alarm_handle_timer - Callback for posix timers
 * @alarm: alarm that fired
 * @now: time at the timer expiration
 *
 * Posix timer callback for expired alarm timers.
 */
/*
 * alarm_handle_timer() - POSIX alarm timer 的 hrtimer 到期功能 callback。
 * @alarm 嵌在 k_itimer，@now 本实现不用；反推容器后持 irqsave it_lock 调 posix_timer_queue_signal，后者按
 * interval 把状态置 REQUEUE_PENDING 或 DISARMED 并排队信号。运行于 hrtimer callback 上下文，不在此重装；
 * 周期 timer 等信号实际递送时再经 alarm_timer_rearm 恢复，避免未消费标准信号造成持续触发。
 */
static void alarm_handle_timer(struct alarm *alarm, ktime_t now)
{
	struct k_itimer *ptr = container_of(alarm, struct k_itimer, it.alarm.alarmtimer);

	guard(spinlock_irqsave)(&ptr->it_lock);
	posix_timer_queue_signal(ptr);
}

/**
 * alarm_timer_rearm - Posix timer callback for rearming timer
 * @timr:	Pointer to the posixtimer data struct
 */
/*
 * alarm_timer_rearm() - 信号递送路径在已持 @timr->it_lock 时推进并重装周期 alarm。
 * 以当前所属 base 时间 forward，跨过周期数累加到 it_overrun；再按更新后的绝对 expires 启动。返回 true
 * 表示排队成功，false 表示新期限也已过，POSIX core 会立即再次排信号。interval 必须由 core 保证为正。
 */
static bool alarm_timer_rearm(struct k_itimer *timr)
{
	struct alarm *alarm = &timr->it.alarm.alarmtimer;

	timr->it_overrun += alarm_forward_now(alarm, timr->it_interval);
	return alarm_start_timer(alarm, alarm->node.expires, false);
}

/**
 * alarm_timer_forward - Posix timer callback for forwarding timer
 * @timr:	Pointer to the posixtimer data struct
 * @now:	Current time to forward the timer against
 */
/*
 * alarm_timer_forward() - POSIX core 查询/整理状态时按给定 @now 推进 alarm 软件期限。
 * 调用者持 it_lock 并保证正 interval；返回跨过周期数，不启动底层 hrtimer，供 overrun 与 remaining 计算。
 */
static s64 alarm_timer_forward(struct k_itimer *timr, ktime_t now)
{
	struct alarm *alarm = &timr->it.alarm.alarmtimer;

	return alarm_forward(alarm, now, timr->it_interval);
}

/**
 * alarm_timer_remaining - Posix timer callback to retrieve remaining time
 * @timr:	Pointer to the posixtimer data struct
 * @now:	Current time to calculate against
 */
/*
 * alarm_timer_remaining() - 返回 node.expires-@now。
 * 调用者以同一 @now 完成可能的 forward，并以 it_lock 稳定 timr；结果可为负，函数不检查 timer 状态。
 */
static ktime_t alarm_timer_remaining(struct k_itimer *timr, ktime_t now)
{
	struct alarm *alarm = &timr->it.alarm.alarmtimer;

	return ktime_sub(alarm->node.expires, now);
}

/**
 * alarm_timer_try_to_cancel - Posix timer callback to cancel a timer
 * @timr:	Pointer to the posixtimer data struct
 */
/* alarm_timer_try_to_cancel() 把 k_itimer 适配到 alarm_try_to_cancel，完整保留 1/0/负 callback-running 语义。 */
static int alarm_timer_try_to_cancel(struct k_itimer *timr)
{
	return alarm_try_to_cancel(&timr->it.alarm.alarmtimer);
}

/**
 * alarm_timer_wait_running - Posix timer callback to wait for a timer
 * @timr:	Pointer to the posixtimer data struct
 *
 * Called from the core code when timer cancel detected that the callback
 * is running. @timr is unlocked and rcu read lock is held to prevent it
 * from being freed.
 */
/*
 * alarm_timer_wait_running() - POSIX core 在放开 it_lock、持 RCU 防释放后等待 alarm callback 推进。
 * 函数可能在 RT 上阻塞；返回后 core 不能依赖 @timr 仍可解引用，实际重试由外层重新查找/加锁完成。
 */
static void alarm_timer_wait_running(struct k_itimer *timr)
{
	hrtimer_cancel_wait_running(&timr->it.alarm.alarmtimer.timer);
}

/**
 * alarm_timer_arm - Posix timer callback to arm a timer
 * @timr:	Pointer to the posixtimer data struct
 * @expires:	The new expiry time
 * @absolute:	Expiry value is absolute time
 * @sigev_none:	Posix timer does not deliver signals
 */
/*
 * alarm_timer_arm() - 把 POSIX 到期值提交给 alarm 原语。
 * @expires 在 @absolute=false 时先加 root base 当前值；absolute=true 表示 POSIX core 已把 time namespace
 * 绝对值转换到 host 坐标。SIGEV_NONE 不需要运行期/挂起唤醒，只记录 node.expires 并假装成功，供 gettime
 * 按需 forward；其余调用绝对 alarm_start_timer。返回 false 表示期限已过，core 随即排队到期信号。
 */
static bool alarm_timer_arm(struct k_itimer *timr, ktime_t expires,
			    bool absolute, bool sigev_none)
{
	struct alarm *alarm = &timr->it.alarm.alarmtimer;
	struct alarm_base *base = &alarm_bases[alarm->type];

	if (!absolute)
		expires = ktime_add_safe(expires, base->get_ktime());

	/*
	 * sigev_none needs to update the expires value and pretend
	 * that the timer is queued
	 */
	/* SIGEV_NONE 既不进入 software suspend queue，也不启动 hrtimer；状态由 POSIX core 解释。 */
	if (sigev_none) {
		alarm->node.expires = expires;
		return true;
	}
	return alarm_start_timer(&timr->it.alarm.alarmtimer, expires, false);
}

/**
 * alarm_clock_getres - posix getres interface
 * @which_clock: clockid
 * @tp: timespec to fill
 *
 * Returns the granularity of underlying alarm base clock
 */
/*
 * alarm_clock_getres() - 返回 alarm clock 的 hrtimer 软件分辨率。
 * @which_clock 不参与选择（仅由已注册 alarm_clock 调用），@tp 为输出；没有 RTC 后端返回 -EINVAL，否则
 * 写 0 秒/hrtimer_resolution 纳秒并返回 0。RTC 只决定该 wake clock 是否可用，分辨率不是 RTC 硬件粒度。
 */
static int alarm_clock_getres(const clockid_t which_clock, struct timespec64 *tp)
{
	if (!alarmtimer_get_rtcdev())
		return -EINVAL;

	tp->tv_sec = 0;
	tp->tv_nsec = hrtimer_resolution;
	return 0;
}

/**
 * alarm_clock_get_timespec - posix clock_get_timespec interface
 * @which_clock: clockid
 * @tp: timespec to fill.
 *
 * Provides the underlying alarm base time in a tasks time namespace.
 */
/*
 * alarm_clock_get_timespec() - 读取调用任务时间命名空间中的 alarm base 时间。
 * @which_clock 经 clock2alarm 选择 REALTIME/BOOTTIME，@tp 为输出；无 RTC 返回 -EINVAL且不写输出，否则
 * 调 base->get_timespec 后返回 0。BOOTTIME getter 显式叠加 timens offset；REALTIME 不由 time namespace 虚拟化。
 */
static int alarm_clock_get_timespec(clockid_t which_clock, struct timespec64 *tp)
{
	struct alarm_base *base = &alarm_bases[clock2alarm(which_clock)];

	if (!alarmtimer_get_rtcdev())
		return -EINVAL;

	base->get_timespec(tp);

	return 0;
}

/**
 * alarm_clock_get_ktime - posix clock_get_ktime interface
 * @which_clock: clockid
 *
 * Provides the underlying alarm base time in the root namespace.
 */
/*
 * alarm_clock_get_ktime() - 读取 alarm base 的 host/root namespace ktime。
 * 无 RTC 时以 ktime_t 形式返回负的 -EINVAL；否则返回 clock2alarm 所选 base->get_ktime。该值供 POSIX core
 * 内部绝对期限运算，区别于面向用户的 get_timespec 时间命名空间视图。
 */
static ktime_t alarm_clock_get_ktime(clockid_t which_clock)
{
	struct alarm_base *base = &alarm_bases[clock2alarm(which_clock)];

	if (!alarmtimer_get_rtcdev())
		return -EINVAL;

	return base->get_ktime();
}

/**
 * alarm_timer_create - posix timer_create interface
 * @new_timer: k_itimer pointer to manage
 *
 * Initializes the k_itimer structure.
 */
/*
 * alarm_timer_create() - 为 POSIX k_itimer 初始化内嵌 alarm。
 * @new_timer 由 POSIX core 分配并已设置 it_clock；无 RTC 返回 -EOPNOTSUPP，缺 CAP_WAKE_ALARM 返回 -EPERM。
 * 成功把 clockid 映射为 type，并以 alarm_handle_timer 初始化，返回 0；不启动 timer、不取得额外对象引用。
 */
static int alarm_timer_create(struct k_itimer *new_timer)
{
	enum  alarmtimer_type type;

	if (!alarmtimer_get_rtcdev())
		return -EOPNOTSUPP;

	if (!capable(CAP_WAKE_ALARM))
		return -EPERM;

	type = clock2alarm(new_timer->it_clock);
	alarm_init(&new_timer->it.alarm.alarmtimer, type, alarm_handle_timer);
	return 0;
}

/**
 * alarmtimer_nsleep_wakeup - Wakeup function for alarm_timer_nsleep
 * @alarm: ptr to alarm that fired
 * @now: time at the timer expiration
 *
 * Wakes up the task that set the alarmtimer
 */
/*
 * alarmtimer_nsleep_wakeup() - 栈上 nanosleep alarm 到期时发布完成并唤醒睡眠任务。
 * @alarm->data 是由睡眠路径发布的 task 借用指针，@now 未使用；callback 先清 data 作为“期限已到”标志，
 * 再对非空 task wake_up_process。alarmtimer_do_nsleep 通过任务状态设置、同步 cancel 与销毁前等待避免漏唤醒
 * 和栈对象越期访问；本函数运行于 hrtimer callback 上下文，不取得 task 引用。
 */
static void alarmtimer_nsleep_wakeup(struct alarm *alarm, ktime_t now)
{
	struct task_struct *task = alarm->data;

	alarm->data = NULL;
	if (task)
		wake_up_process(task);
}

/**
 * alarmtimer_do_nsleep - Internal alarmtimer nsleep implementation
 * @alarm: ptr to alarmtimer
 * @absexp: absolute expiration time
 * @type: alarm type (BOOTTIME/REALTIME).
 *
 * Sets the alarm timer and sleeps until it is fired or interrupted.
 */
/*
 * alarmtimer_do_nsleep() - 用已初始化的栈上 @alarm 睡到 root base 绝对期限 @absexp。
 * 先把 current 写入 data；循环设 TASK_INTERRUPTIBLE、启动绝对 alarm，期限已过则自行清 data，否则 schedule，
 * 每次醒来同步 cancel。data 被 callback 清零表示正常到期；若是无信号的伪唤醒则重新排队，pending signal
 * 保留 data 并结束循环。离开后恢复 TASK_RUNNING、销毁栈 hrtimer；正常到期返回 0。
 *
 * 信号/冻结中断时，freezing 任务登记 suspend 候选。restart_block 带剩余时间指针时重新计算正剩余并由
 * nanosleep_copyout 返回 -ERESTART_RESTARTBLOCK 或 -EFAULT，期限已过返回 0；无指针直接返回
 * -ERESTART_RESTARTBLOCK。函数可调度，返回前 callback 已同步完成，不泄漏 alarm/task ownership。
 */
static int alarmtimer_do_nsleep(struct alarm *alarm, ktime_t absexp,
				enum alarmtimer_type type)
{
	struct restart_block *restart;
	alarm->data = (void *)current;
	do {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!alarm_start_timer(alarm, absexp, false))
			alarm->data = NULL;

		if (likely(alarm->data))
			schedule();

		alarm_cancel(alarm);
	} while (alarm->data && !signal_pending(current));

	__set_current_state(TASK_RUNNING);

	destroy_hrtimer_on_stack(&alarm->timer);

	if (!alarm->data)
		return 0;

	if (freezing(current))
		alarmtimer_freezerset(absexp, type);
	restart = &current->restart_block;
	if (restart->nanosleep.type != TT_NONE) {
		struct timespec64 rmt;
		ktime_t rem;

		rem = ktime_sub(absexp, alarm_bases[type].get_ktime());

		if (rem <= 0)
			return 0;
		rmt = ktime_to_timespec64(rem);

		return nanosleep_copyout(restart, &rmt);
	}
	return -ERESTART_RESTARTBLOCK;
}

/*
 * alarm_init_on_stack() - 初始化仅在当前调用栈存活的 alarm。
 * 参数语义同 alarm_init，但用 hrtimer_setup_on_stack 注册 debugobjects 栈属性；必须与
 * destroy_hrtimer_on_stack 配对，且返回前先 cancel/等待 callback，当前唯一消费者由 do_nsleep 完成清理。
 */
static void
alarm_init_on_stack(struct alarm *alarm, enum alarmtimer_type type,
		    void (*function)(struct alarm *, ktime_t))
{
	hrtimer_setup_on_stack(&alarm->timer, alarmtimer_fired, alarm_bases[type].base_clockid,
			       HRTIMER_MODE_ABS);
	__alarm_init(alarm, type, function);
}

/**
 * alarm_timer_nsleep_restart - restartblock alarmtimer nsleep
 * @restart: ptr to restart block
 *
 * Handles restarted clock_nanosleep calls
 */
/*
 * alarm_timer_nsleep_restart() - restart syscall 回调，以保存的 host 绝对期限继续相对 alarm sleep。
 * @restart->nanosleep.clockid 在此保存的是 alarmtimer_type 而非用户 clockid，expires 是首次调用算出的绝对值；
 * 在当前栈重建 alarm 后复用 do_nsleep，返回 0、-EFAULT 或再次 -ERESTART_RESTARTBLOCK。
 */
static long __sched alarm_timer_nsleep_restart(struct restart_block *restart)
{
	enum  alarmtimer_type type = restart->nanosleep.clockid;
	ktime_t exp = restart->nanosleep.expires;
	struct alarm alarm;

	alarm_init_on_stack(&alarm, type, alarmtimer_nsleep_wakeup);

	return alarmtimer_do_nsleep(&alarm, exp, type);
}

/**
 * alarm_timer_nsleep - alarmtimer nanosleep
 * @which_clock: clockid
 * @flags: determines abstime or relative
 * @tsreq: requested sleep time (abs or rel)
 *
 * Handles clock_nanosleep calls against _ALARM clockids
 */
/*
 * alarm_timer_nsleep() - CLOCK_REALTIME_ALARM/CLOCK_BOOTTIME_ALARM 的 clock_nanosleep 实现。
 * @flags 只允许 TIMER_ABSTIME，@tsreq 已由 POSIX core 校验；无 RTC 返回 -EOPNOTSUPP，缺
 * CAP_WAKE_ALARM 返回 -EPERM，非法 flag 返回 -EINVAL。相对值加 root base 当前值，绝对值从调用者时间
 * namespace 转 host 坐标，然后用栈 alarm 睡眠。
 *
 * 非 restart 错误/成功原样返回。被信号中断时，绝对 sleep 不复制剩余时间也不自动 restart，返回
 * -ERESTARTNOHAND；相对 sleep 把 type 与固定绝对期限写入 restart_block 并返回 -ERESTART_RESTARTBLOCK，
 * 从而重启时不会把已睡时间再次计入。所有路径的栈 hrtimer 已由 do_nsleep 销毁。
 */
static int alarm_timer_nsleep(const clockid_t which_clock, int flags,
			      const struct timespec64 *tsreq)
{
	enum  alarmtimer_type type = clock2alarm(which_clock);
	struct restart_block *restart = &current->restart_block;
	struct alarm alarm;
	ktime_t exp;
	int ret;

	if (!alarmtimer_get_rtcdev())
		return -EOPNOTSUPP;

	if (flags & ~TIMER_ABSTIME)
		return -EINVAL;

	if (!capable(CAP_WAKE_ALARM))
		return -EPERM;

	alarm_init_on_stack(&alarm, type, alarmtimer_nsleep_wakeup);

	exp = timespec64_to_ktime(*tsreq);
	/* Convert (if necessary) to absolute time */
	/* 相对请求固定为 host 绝对期限；绝对请求仅执行 time namespace 到 host 的坐标转换。 */
	if (flags != TIMER_ABSTIME) {
		ktime_t now = alarm_bases[type].get_ktime();

		exp = ktime_add_safe(now, exp);
	} else {
		exp = timens_ktime_to_host(which_clock, exp);
	}

	ret = alarmtimer_do_nsleep(&alarm, exp, type);
	if (ret != -ERESTART_RESTARTBLOCK)
		return ret;

	/* abs timers don't set remaining time or restart */
	/* POSIX 绝对 sleep 的中断语义：既不写 remaining，也不安装 restart_block。 */
	if (flags == TIMER_ABSTIME)
		return -ERESTARTNOHAND;

	restart->nanosleep.clockid = type;
	restart->nanosleep.expires = exp;
	set_restart_fn(restart, alarm_timer_nsleep_restart);
	return ret;
}

/*
 * alarm_clock 是 POSIX core 的静态常驻操作表：clock 读取/create/nsleep 使用本文件实现，set/get/del 复用
 * common core，arm/rearm/forward/remaining/cancel/wait 则适配 alarm。core 负责 it_lock、RCU、用户复制、
 * time namespace 绝对值转换和状态机；本表只在 CONFIG_POSIX_TIMERS 下存在。
 */
const struct k_clock alarm_clock = {
	.clock_getres		= alarm_clock_getres,
	.clock_get_ktime	= alarm_clock_get_ktime,
	.clock_get_timespec	= alarm_clock_get_timespec,
	.timer_create		= alarm_timer_create,
	.timer_set		= common_timer_set,
	.timer_del		= common_timer_del,
	.timer_get		= common_timer_get,
	.timer_arm		= alarm_timer_arm,
	.timer_rearm		= alarm_timer_rearm,
	.timer_forward		= alarm_timer_forward,
	.timer_remaining	= alarm_timer_remaining,
	.timer_try_to_cancel	= alarm_timer_try_to_cancel,
	.timer_wait_running	= alarm_timer_wait_running,
	.nsleep			= alarm_timer_nsleep,
};
#endif /* CONFIG_POSIX_TIMERS */


/* Suspend hook structures */
/* PM ops 与 platform driver 把系统 suspend/resume 调用接到共享 RTC 编程路径；对象均静态常驻。 */
static const struct dev_pm_ops alarmtimer_pm_ops = {
	.suspend = alarmtimer_suspend,
	.resume = alarmtimer_resume,
};

static struct platform_driver alarmtimer_driver = {
	.driver = {
		.name = "alarmtimer",
		.pm = &alarmtimer_pm_ops,
	}
};

/*
 * get_boottime_timespec() - 输出当前任务时间命名空间视图下的 BOOTTIME。
 * @tp 为输出；先取 host boottime，再叠加当前 time namespace 的 boottime offset。无失败返回、不持久化指针。
 */
static void get_boottime_timespec(struct timespec64 *tp)
{
	ktime_get_boottime_ts64(tp);
	timens_add_boottime(tp);
}

/**
 * alarmtimer_init - Initialize alarm timer code
 *
 * This function initializes the alarm bases and registers
 * the posix clock ids.
 */
/*
 * alarmtimer_init() - device_initcall 阶段初始化全部 alarm base 并注册 RTC/PM 集成。
 * 先初始化可选共享 rtc_timer；为 REALTIME/BOOTTIME 填 base clock/getter，随后对 [0, ALARM_NUMTYPE) 初始化
 * timerqueue 与锁。再注册 rtc class interface 以选择后端，失败直接返回；最后注册 alarmtimer platform driver，
 * 失败则撤销 class interface 并返回错误，成功返回 0。静态 base 在任何 alarm_init 前完成，不注册 POSIX id
 * 本身（alarm_clock 由 POSIX core 的固定表引用）；成功路径没有模块卸载清理，因为本代码为 built-in initcall。
 */
static int __init alarmtimer_init(void)
{
	int error;
	int i;

	alarmtimer_rtc_timer_init();

	/* Initialize alarm bases */
	/* 两个 base 的时钟坐标必须同时供 hrtimer、remaining、suspend delta 与用户 clock_gettime 使用。 */
	alarm_bases[ALARM_REALTIME].base_clockid = CLOCK_REALTIME;
	alarm_bases[ALARM_REALTIME].get_ktime = &ktime_get_real;
	alarm_bases[ALARM_REALTIME].get_timespec = ktime_get_real_ts64;
	alarm_bases[ALARM_BOOTTIME].base_clockid = CLOCK_BOOTTIME;
	alarm_bases[ALARM_BOOTTIME].get_ktime = &ktime_get_boottime;
	alarm_bases[ALARM_BOOTTIME].get_timespec = get_boottime_timespec;
	for (i = 0; i < ALARM_NUMTYPE; i++) {
		timerqueue_init_head(&alarm_bases[i].timerqueue);
		spin_lock_init(&alarm_bases[i].lock);
	}

	error = alarmtimer_rtc_interface_setup();
	if (error)
		return error;

	error = platform_driver_register(&alarmtimer_driver);
	if (error)
		goto out_if;

	return 0;
out_if:
	alarmtimer_rtc_interface_remove();
	return error;
}
device_initcall(alarmtimer_init);
