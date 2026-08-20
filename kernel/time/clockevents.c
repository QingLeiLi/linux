// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains functions which manage clock event devices.
 *
 * Copyright(C) 2005-2006, Linutronix GmbH, Thomas Gleixner <tglx@kernel.org>
 * Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 * Copyright(C) 2006-2007, Timesys Corp., Thomas Gleixner
 */
/*
 * 本文件管理 clock event 设备：把单调绝对到期时间换算为硬件 ticks，驱动 shutdown/periodic/oneshot
 * 状态机，处理过近事件的强制最小延迟与重试，并在注册、替换、解绑、CPU hotplug、suspend 和 sysfs
 * 路径中维持设备及模块生命周期。clocksource 回答“现在几点”，clockevent 则负责“何时产生下一次中断”。
 */

#include <linux/clockchips.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/device.h>

#include "tick-internal.h"

/* The registered clock event devices */
/*
 * clockevent_devices 链接已注册设备；clockevents_released 暂存刚被 tick 层替换、等待重新匹配角色的设备。
 * 两张链、设备状态与 tick 交换由 irq-safe clockevents_lock 保护。clockevents_mutex 串行化可睡眠的跨 CPU
 * 解绑，锁序为 mutex → clockevents_lock；链表只借用驱动静态/长期对象，成功解绑后才解除 core 约束。
 */
static LIST_HEAD(clockevent_devices);
static LIST_HEAD(clockevents_released);
/* Protection for the above */
/* raw spinlock 可由 tick 中断/CPU teardown 使用，持有期间不得睡眠。 */
static DEFINE_RAW_SPINLOCK(clockevents_lock);
/* Protection for unbind operations */
/* mutex 防止两个 sysfs/驱动解绑者同时跨 CPU 替换同一 tick 设备。 */
static DEFINE_MUTEX(clockevents_mutex);

/*
 * ce_unbind 是同步 smp_call 的栈上请求/应答：@ce 为调用者稳定的设备借用指针，@res 初始错误并由目标
 * CPU 在 clockevents_lock 下写回。wait=1 保证远端返回前栈对象不会失效，不涉及动态分配。
 */
struct ce_unbind {
	struct clock_event_device *ce;
	int res;
};

/*
 * cev_delta2ns() - 把硬件 tick 边界换算为不会越过设备约束的纳秒值。
 * @latch 是 min/max ticks；@evt 是尚在注册/配置期稳定的可写设备借用指针；@ismax 区分上界换算。
 * clc 保存 latch<<shift 的分子，rnd=mult-1 用于向上取整。mult=0 会告警并修成 1，左移回验失败则把
 * 分子饱和到 U64_MAX。min 边界允许加 rnd，保证 ns 再转 ticks 不低于 latch；max 在频率高于 1GHz 时
 * 省略加法，避免反向换算超过硬件上限；加法可能溢出也省略。除以 mult 后最低钳到 1000ns，过滤无意义
 * 的亚微秒噪声。纯整数运算、不睡眠；返回安全 ns，无 errno，唯一副作用是修复非法 mult。
 */
static u64 cev_delta2ns(unsigned long latch, struct clock_event_device *evt,
			bool ismax)
{
	u64 clc = (u64) latch << evt->shift;
	u64 rnd;

	if (WARN_ON(!evt->mult))
		evt->mult = 1;
	rnd = (u64) evt->mult - 1;

	/*
	 * Upper bound sanity check. If the backwards conversion is
	 * not equal latch, we know that the above shift overflowed.
	 */
	/* 上界健全性检查：若反向右移不能还原 latch，说明左移已溢出，改用饱和值继续安全除法。 */
	if ((clc >> evt->shift) != (u64)latch)
		clc = ~0ULL;

	/*
	 * Scaled math oddities:
	 *
	 * For mult <= (1 << shift) we can safely add mult - 1 to
	 * prevent integer rounding loss. So the backwards conversion
	 * from nsec to device ticks will be correct.
	 *
	 * For mult > (1 << shift), i.e. device frequency is > 1GHz we
	 * need to be careful. Adding mult - 1 will result in a value
	 * which when converted back to device ticks can be larger
	 * than latch by up to (mult - 1) >> shift. For the min_delta
	 * calculation we still want to apply this in order to stay
	 * above the minimum device ticks limit. For the upper limit
	 * we would end up with a latch value larger than the upper
	 * limit of the device, so we omit the add to stay below the
	 * device upper boundary.
	 *
	 * Also omit the add if it would overflow the u64 boundary.
	 */
	/*
	 * mult<=2^shift 时加 mult-1 是标准向上取整；频率高于 1GHz 时该加法可能令 max 反算 ticks 超界，
	 * 因而仅 min 保留向上取整。无论哪种边界，只要 u64 加法可能溢出就不加。
	 */
	if ((~0ULL - clc > rnd) &&
	    (!ismax || evt->mult <= (1ULL << evt->shift)))
		clc += rnd;

	do_div(clc, evt->mult);

	/* Deltas less than 1usec are pointless noise */
	/* 小于 1 微秒的设备边界被视为无意义噪声，统一提升到 1000ns。 */
	return clc > 1000 ? clc : 1000;
}

/**
 * clockevent_delta2ns - Convert a latch value (device ticks) to nanoseconds
 * @latch:	value to convert
 * @evt:	pointer to clock event device descriptor
 *
 * Math helper, returns latch value converted to nanoseconds (bound checked)
 */
/*
 * clockevent_delta2ns() - 对外提供 min 边界语义的 tick→ns 换算。
 * @latch/@evt 均按上方 helper 契约借用；固定 ismax=false，故采用向上取整以确保反向编程不少于输入 ticks。
 * 可在配置/驱动上下文调用，不睡眠；返回至少 1000ns，非法 mult 会告警并改为 1，无独立错误码。
 */
u64 clockevent_delta2ns(unsigned long latch, struct clock_event_device *evt)
{
	return cev_delta2ns(latch, evt, false);
}
EXPORT_SYMBOL_GPL(clockevent_delta2ns);

/*
 * __clockevents_switch_state() - 调用驱动状态回调，但不提交 core 的 state 字段。
 * @dev 是调用者在关中断和 tick/注册协议下稳定持有的可写设备；@state 是目标状态。DUMMY 直接成功且不改
 * forced 标志；普通设备先清 next_event_forced，再按状态调用 shutdown/periodic/oneshot/stopped 回调。
 * DETACHED 有意落入 SHUTDOWN；periodic/oneshot 缺 feature 返回 -ENOSYS；stopped 只允许从 ONESHOT 进入，
 * 前态错误告警并 -EINVAL，缺回调 -ENOSYS；未知状态 -ENOSYS。驱动 errno 原样返回，成功仍由外层提交
 * state；回调处于不可睡眠语境，设备/模块 ownership 不变。
 */
static int __clockevents_switch_state(struct clock_event_device *dev,
				      enum clock_event_state state)
{
	if (dev->features & CLOCK_EVT_FEAT_DUMMY)
		return 0;

	/* On state transitions clear the forced flag unconditionally */
	/* 普通状态切换都会结束“已强制最小事件”代次；DUMMY 在此前返回，不触碰字段。 */
	dev->next_event_forced = 0;

	/* Transition with new state-specific callbacks */
	/* 只执行硬件回调；外层确认成功后才更新软件 state，避免驱动失败造成状态镜像分裂。 */
	switch (state) {
	case CLOCK_EVT_STATE_DETACHED:
		/* The clockevent device is getting replaced. Shut it down. */
		/* 被替换设备先关闭硬件，再由交换路径移动链表并释放模块引用。 */

	case CLOCK_EVT_STATE_SHUTDOWN:
		if (dev->set_state_shutdown)
			return dev->set_state_shutdown(dev);
		return 0;

	case CLOCK_EVT_STATE_PERIODIC:
		/* Core internal bug */
		/* core 不应让未声明 PERIODIC 的设备进入周期模式；以 -ENOSYS 保留旧状态。 */
		if (!(dev->features & CLOCK_EVT_FEAT_PERIODIC))
			return -ENOSYS;
		if (dev->set_state_periodic)
			return dev->set_state_periodic(dev);
		return 0;

	case CLOCK_EVT_STATE_ONESHOT:
		/* Core internal bug */
		/* oneshot 同样必须先由 feature 声明；可选回调为空时视为无需额外硬件动作。 */
		if (!(dev->features & CLOCK_EVT_FEAT_ONESHOT))
			return -ENOSYS;
		if (dev->set_state_oneshot)
			return dev->set_state_oneshot(dev);
		return 0;

	case CLOCK_EVT_STATE_ONESHOT_STOPPED:
		/* Core internal bug */
		/* stopped 是 oneshot 的暂停子态，不允许从 detached/shutdown/periodic 直接跨入。 */
		if (WARN_ONCE(!clockevent_state_oneshot(dev),
			      "Current state: %d\n",
			      clockevent_get_state(dev)))
			return -EINVAL;

		if (dev->set_state_oneshot_stopped)
			return dev->set_state_oneshot_stopped(dev);
		else
			return -ENOSYS;

	default:
		return -ENOSYS;
	}
}

/**
 * clockevents_switch_state - set the operating state of a clock event device
 * @dev:	device to modify
 * @state:	new state
 *
 * Must be called with interrupts disabled !
 */
/*
 * clockevents_switch_state() - 原子地执行并提交 clockevent 目标状态。
 * @dev/@state 同内层；调用者必须关本地中断，保证 per-CPU tick 与硬件回调不并发。目标等于当前状态时
 * 幂等返回；否则先调用内层，失败时静默保留旧 state，成功才写新 state。进入 ONESHOT 后若 mult 为 0，
 * 告警并修为 1，防止后续 ns→cycle 乘法/除法崩溃。无直接返回值，因此调用者不能区分驱动失败，只能
 * 读取 state；函数不可睡眠，不改变设备 ownership。
 */
void clockevents_switch_state(struct clock_event_device *dev,
			      enum clock_event_state state)
{
	if (clockevent_get_state(dev) != state) {
		if (__clockevents_switch_state(dev, state))
			return;

		clockevent_set_state(dev, state);

		/*
		 * A nsec2cyc multiplicator of 0 is invalid and we'd crash
		 * on it, so fix it up and emit a warning:
		 */
		/* ns→cycle 乘数为 0 会破坏 oneshot 编程；状态已提交后以 1 兜底并留下告警。 */
		if (clockevent_state_oneshot(dev)) {
			if (WARN_ON(!dev->mult))
				dev->mult = 1;
		}
	}
}

/**
 * clockevents_shutdown - shutdown the device and clear next_event
 * @dev:	device to shutdown
 */
/*
 * clockevents_shutdown() - 关闭硬件并清除任何待编程事件镜像。
 * @dev 是关中断且生命周期稳定的设备借用指针；先尝试切到 SHUTDOWN，再无条件把 next_event 置 KTIME_MAX
 * 并清 forced。即使驱动 shutdown 回调失败、state 保持旧值，软件到期记录仍被清除。无返回值/错误传播，
 * 不释放模块或设备；调用者必须通过更高层协议处理硬件失败风险。
 */
void clockevents_shutdown(struct clock_event_device *dev)
{
	clockevents_switch_state(dev, CLOCK_EVT_STATE_SHUTDOWN);
	dev->next_event = KTIME_MAX;
	dev->next_event_forced = 0;
}

/**
 * clockevents_tick_resume -	Resume the tick device before using it again
 * @dev:			device to resume
 */
/*
 * clockevents_tick_resume() - 在重新使用 tick 设备前调用可选驱动恢复钩子。
 * @dev 是 suspend/resume 协议稳定持有的设备借用指针；调用者处于 tick 恢复上下文并负责中断/锁条件。
 * 无回调返回 0；有回调原样返回其 0 或 errno。函数自身不改变 core state、next_event 或 ownership，驱动
 * 失败由上层决定是否继续恢复。
 */
int clockevents_tick_resume(struct clock_event_device *dev)
{
	int ret = 0;

	if (dev->tick_resume)
		ret = dev->tick_resume(dev);

	return ret;
}

#ifdef CONFIG_GENERIC_CLOCKEVENTS_MIN_ADJUST

/* Limit min_delta to a jiffy */
/* 动态放宽的最小编程间隔最多到一个 jiffy；再失败就放弃该次事件。 */
#define MIN_DELTA_LIMIT		(NSEC_PER_SEC / HZ)

/**
 * clockevents_increase_min_delta - raise minimum delta of a clock event device
 * @dev:       device to increase the minimum delta
 *
 * Returns 0 on success, -ETIME when the minimum delta reached the limit.
 */
/*
 * clockevents_increase_min_delta() - 驱动连续拒绝近事件时逐步放宽其最小可编程纳秒间隔。
 * @dev 是当前 CPU 关中断语境下稳定的可写设备借用指针。已到一个 jiffy 上限时用 deferred printk 报警，
 * 把 next_event 清为 KTIME_MAX 并返回 -ETIME；否则不足 5us 先升到 5us，之后每次增加 50%，再钳到上限，
 * 打印新值并返回 0。函数不直接编程硬件、不睡眠；修改 min_delta_ns 会永久影响后续编程重试。
 */
static int clockevents_increase_min_delta(struct clock_event_device *dev)
{
	/* Nothing to do if we already reached the limit */
	/* 上限处再失败没有更安全的近事件可试，清软件到期镜像并终止重试。 */
	if (dev->min_delta_ns >= MIN_DELTA_LIMIT) {
		printk_deferred(KERN_WARNING
				"CE: Reprogramming failure. Giving up\n");
		dev->next_event = KTIME_MAX;
		return -ETIME;
	}

	if (dev->min_delta_ns < 5000)
		dev->min_delta_ns = 5000;
	else
		dev->min_delta_ns += dev->min_delta_ns >> 1;

	if (dev->min_delta_ns > MIN_DELTA_LIMIT)
		dev->min_delta_ns = MIN_DELTA_LIMIT;

	printk_deferred(KERN_WARNING
			"CE: %s increased min_delta_ns to %llu nsec\n",
			dev->name ? dev->name : "?",
			(unsigned long long) dev->min_delta_ns);
	return 0;
}

/**
 * clockevents_program_min_delta - Set clock event device to the minimum delay.
 * @dev:	device to program
 *
 * Returns 0 on success, -ETIME when the retry loop failed.
 */
/*
 * clockevents_program_min_delta() - 在启用动态调整时把设备编程到当前/逐步放宽的最小延迟。
 * @dev 必须是 ONESHOT 设备且由当前 tick 路径在关中断下稳定持有；set_next_event 不得睡眠。delta 取当前
 * min_delta_ns，next_event 每次写为 ktime_get()+delta；若期间设备已 shutdown，硬件无需编程并返回 0。
 * clc 是 ns×mult>>shift 得到的 ticks，i 统计同一 min 值的失败次数，dev->retries 累计所有驱动调用。
 *
 * 每个 min 最多尝试 3 次；仍失败就调用 increase 并从新边界重试，直至驱动接受返回 0，或边界已到一个
 * jiffy 后返回 -ETIME。成功/失败均不清 retries；失败上限路径把 next_event 置 KTIME_MAX。无资源转移。
 */
static int clockevents_program_min_delta(struct clock_event_device *dev)
{
	unsigned long long clc;
	int64_t delta;
	int i;

	for (i = 0;;) {
		delta = dev->min_delta_ns;
		dev->next_event = ktime_add_ns(ktime_get(), delta);

		if (clockevent_state_shutdown(dev))
			return 0;

		dev->retries++;
		clc = ((unsigned long long) delta * dev->mult) >> dev->shift;
		if (dev->set_next_event((unsigned long) clc, dev) == 0)
			return 0;

		if (++i > 2) {
			/*
			 * We tried 3 times to program the device with the
			 * given min_delta_ns. Try to increase the minimum
			 * delta, if that fails as well get out of here.
			 */
			/* 同一边界三次均失败后扩大 min；扩大已到极限则把 -ETIME 传给事件编程调用者。 */
			if (clockevents_increase_min_delta(dev))
				return -ETIME;
			i = 0;
		}
	}
}

#else  /* CONFIG_GENERIC_CLOCKEVENTS_MIN_ADJUST */

/**
 * clockevents_program_min_delta - Set clock event device to the minimum delay.
 * @dev:	device to program
 *
 * Returns 0 on success, -ETIME when the retry loop failed.
 */
/*
 * clockevents_program_min_delta() - 在未启用动态调整时用 1..10 倍 min_delta_ns 做有限近事件重试。
 * @dev 的上下文、借用和回调约束同上。delta 从 0 开始，每轮累加 min，next_event 设为 now+delta；shutdown
 * 视为无需硬件编程而成功。每次换成 ticks 后递增 retries 并调用 set_next_event，任一次接受返回 0；十次
 * 都拒绝返回 -ETIME。它不修改长期 min_delta_ns，也不在失败时清 next_event，因此镜像保留最后一次候选
 * 到期时间；调用者据返回值决定是否报告事件已过期。
 */
static int clockevents_program_min_delta(struct clock_event_device *dev)
{
	unsigned long long clc;
	int64_t delta = 0;
	int i;

	for (i = 0; i < 10; i++) {
		delta += dev->min_delta_ns;
		dev->next_event = ktime_add_ns(ktime_get(), delta);

		if (clockevent_state_shutdown(dev))
			return 0;

		dev->retries++;
		clc = ((unsigned long long) delta * dev->mult) >> dev->shift;
		if (dev->set_next_event((unsigned long) clc, dev) == 0)
			return 0;
	}
	return -ETIME;
}

#endif /* CONFIG_GENERIC_CLOCKEVENTS_MIN_ADJUST */

#ifdef CONFIG_GENERIC_CLOCKEVENTS_COUPLED
#ifdef CONFIG_GENERIC_CLOCKEVENTS_COUPLED_INLINE
#include <asm/clock_inlined.h>
#else
/*
 * 未提供架构内联实现时的编译期占位。@cycles/@dev 均不使用、无副作用；只有 INLINE 配置为真时调用分支
 * 才会存活，因此该空实现不会替代普通 dev->set_next_coupled() 路径。
 */
static __always_inline void
arch_inlined_clockevent_set_next_coupled(u64 cycles, struct clock_event_device *dev) { }
#endif

/*
 * clockevent_set_next_coupled() - 尝试借助关联 clocksource 快照直接编程耦合 clockevent。
 * @dev 是当前 CPU 的 ONESHOT 设备借用指针；@expires 是单调绝对到期时间。未声明 COUPLED 特性或
 * ktime_expiry_to_cycles() 无法用 dev->cs_id 取得一致 cycle 时返回 false，调用者回退普通 now/delta 换算。
 * 成功得到 cycles 后，按配置调用架构强制内联 helper 或驱动 set_next_coupled，并返回 true。回调无错误
 * 返回，因此 true 表示已提交而非硬件可确认；函数在关中断原子路径运行、不睡眠、不改变 ownership。
 */
static inline bool clockevent_set_next_coupled(struct clock_event_device *dev, ktime_t expires)
{
	u64 cycles;

	if (unlikely(!(dev->features & CLOCK_EVT_FEAT_CLOCKSOURCE_COUPLED)))
		return false;

	if (unlikely(!ktime_expiry_to_cycles(dev->cs_id, expires, &cycles)))
		return false;

	if (IS_ENABLED(CONFIG_GENERIC_CLOCKEVENTS_COUPLED_INLINE))
		arch_inlined_clockevent_set_next_coupled(cycles, dev);
	else
		dev->set_next_coupled(cycles, dev);
	return true;
}

#else
/*
 * 未启用耦合支持时的同签名桩：@dev/@expires 不读取，固定 false，让所有调用退回通用 delta 编程路径；
 * 不睡眠、无状态或资源副作用。
 */
static inline bool clockevent_set_next_coupled(struct clock_event_device *dev, ktime_t expires)
{
	return false;
}
#endif

/**
 * clockevents_program_event - Reprogram the clock event device.
 * @dev:	device to program
 * @expires:	absolute expiry time (monotonic clock)
 * @force:	program minimum delay if expires can not be set
 *
 * Returns 0 on success, -ETIME when the event is in the past.
 */
/*
 * clockevents_program_event() - 把单调绝对到期时间提交给当前 ONESHOT clockevent。
 *
 * 【参数与上下文】@dev 是当前 CPU/tick 或 broadcast 锁协议稳定持有的可写设备借用指针；@expires 是
 * CLOCK_MONOTONIC 绝对 ktime，负值非法；@force 允许“最小 ticks 也被驱动拒绝”后进入配置相关重试。
 * 调用者必须关本地中断，驱动回调不可睡眠。delta/cycles 分别保存相对纳秒与硬件 ticks。
 *
 * 【路径】先拒绝负 expires，再写 next_event；shutdown 设备无需编程即成功。非 ONESHOT 只告警继续。
 * HRTIMER 设备直接把绝对时间交给 set_next_ktime 并透传返回；耦合设备若能从关联 clocksource 得到 cycles，
 * 由专用回调提交并返回 0。普通路径以当前 ktime 求 delta：已过期且 !force 返回 -ETIME。大于 min 时钳到
 * max、换算 ticks 并先尝试精确编程，成功清 forced。过近或精确编程失败时，若已有 forced 最小事件在途，
 * 直接返回 0，因为即将到来的中断会促使 tick 层重评估刚写入的绝对 next_event。
 *
 * 【最小事件与返回】没有在途 forced 时先调用一次 min_delta_ticks；成功后置 forced=1。若驱动仍拒绝，
 * !force 直接 -ETIME；force=true 才调用 program_min_delta 做 10 次或动态放宽重试，失败同样 -ETIME。
 * 因此 force 不是“是否允许首次最小 ticks”，而是“首次最小 ticks 失败后是否继续强制重试”。0 表示已有
 * 或新事件足以触发后续处理；-ETIME 可能表示负/已过期输入或硬件拒绝，next_event 仍可能已被写入。
 */
int clockevents_program_event(struct clock_event_device *dev, ktime_t expires, bool force)
{
	int64_t delta;
	u64 cycles;

	if (WARN_ON_ONCE(expires < 0))
		return -ETIME;

	dev->next_event = expires;

	if (clockevent_state_shutdown(dev))
		return 0;

	/* We must be in ONESHOT state here */
	/* 非 oneshot 是调用协议错误，但告警后继续，便于暴露驱动回调/字段问题而非静默丢 tick。 */
	WARN_ONCE(!clockevent_state_oneshot(dev), "Current state: %d\n",
		  clockevent_get_state(dev));

	/* ktime_t based reprogramming for the broadcast hrtimer device */
	/* 软件 broadcast hrtimer 直接接收绝对 ktime，不经过硬件 mult/shift。 */
	if (unlikely(dev->features & CLOCK_EVT_FEAT_HRTIMER))
		return dev->set_next_ktime(expires, dev);

	if (likely(clockevent_set_next_coupled(dev, expires)))
		return 0;

	delta = ktime_to_ns(ktime_sub(expires, ktime_get()));

	/* Required for tick_periodic() during early boot */
	/* early boot 周期 tick 会以 force 请求已到期事件；普通调用则以 -ETIME 让上层立即处理过期。 */
	if (delta <= 0 && !force)
		return -ETIME;

	if (delta > (int64_t)dev->min_delta_ns) {
		/* 正常窗口先钳硬件最大 ns，再按设备比例换成 ticks；驱动 0 才是真正接受。 */
		delta = min(delta, (int64_t) dev->max_delta_ns);
		cycles = ((u64)delta * dev->mult) >> dev->shift;
		if (!dev->set_next_event((unsigned long) cycles, dev)) {
			dev->next_event_forced = 0;
			return 0;
		}
	}

	if (dev->next_event_forced)
		/* 先前最小事件仍会很快中断，无需反复改写硬件；软件 next_event 已更新为本次绝对目标。 */
		return 0;

	/* 先以驱动声明的精确最小 ticks 尝试一次；force 只控制该次失败后的扩展重试。 */
	if (dev->set_next_event(dev->min_delta_ticks, dev)) {
		if (!force || clockevents_program_min_delta(dev))
			return -ETIME;
	}
	dev->next_event_forced = 1;
	return 0;
}

/*
 * Called after a clockevent has been added which might
 * have replaced a current regular or broadcast device. A
 * released normal device might be a suitable replacement
 * for the current broadcast device. Similarly a released
 * broadcast device might be a suitable replacement for a
 * normal device.
 */
/*
 * clockevents_notify_released() - 反复把刚释放设备重新交给 tick 匹配，直至没有待处理对象。
 * 入参：无；调用者持 clockevents_lock 且关中断。dev 是 released 表头借用指针；先移回注册表，再调用
 * tick_check_new_device()。新设备可能替换 regular/broadcast，并把旧设备再次放入 released，因此必须循环
 * 到链表真正为空而不能只走一遍。无返回值、不睡眠；模块引用的取得/释放由 tick 交换路径配对。
 */
static void clockevents_notify_released(void)
{
	struct clock_event_device *dev;

	/*
	 * Keep iterating as long as tick_check_new_device()
	 * replaces a device.
	 */
	/* 每次替换都可能产生新的 released 设备；逐个回灌直到 regular/broadcast 两侧达到稳定匹配。 */
	while (!list_empty(&clockevents_released)) {
		dev = list_entry(clockevents_released.next,
				 struct clock_event_device, list);
		list_move(&dev->list, &clockevent_devices);
		tick_check_new_device(dev);
	}
}

/*
 * Try to install a replacement clock event device
 */
/*
 * clockevents_replace() - 为当前 CPU 正在使用的 @ced 查找并安装可替代的 detached 设备。
 * 调用者在目标 CPU 上持 clockevents_lock、关中断；@ced 为待解绑设备借用指针。dev 是注册表游标，newdev
 * 是当前最佳候选及其临时模块引用。跳过 ced/非 detached，依 tick_check_replacement 过滤能力、cpumask 和
 * rating；try_module_get 失败的卸载中驱动不可选。更优候选取得引用后释放旧候选引用。
 *
 * 命中时 tick_install_replacement() 把该引用交给新的 tick 角色，再将 ced 从注册表摘成空节点并返回 0；
 * 无候选返回 -EBUSY，ced 仍在使用。函数不可睡眠；失败不改变当前设备，候选临时引用均已配平。
 */
static int clockevents_replace(struct clock_event_device *ced)
{
	struct clock_event_device *dev, *newdev = NULL;

	list_for_each_entry(dev, &clockevent_devices, list) {
		if (dev == ced || !clockevent_state_detached(dev))
			continue;

		if (!tick_check_replacement(newdev, dev))
			continue;

		if (!try_module_get(dev->owner))
			continue;

		if (newdev)
			module_put(newdev->owner);
		newdev = dev;
	}
	if (newdev) {
		tick_install_replacement(newdev);
		list_del_init(&ced->list);
	}
	return newdev ? 0 : -EBUSY;
}

/*
 * Called with clockevents_mutex and clockevents_lock held
 */
/*
 * __clockevents_try_unbind() - 在给定 CPU 视角快速判定/执行设备解绑。
 * @ced 是 mutex 保证不会消失的设备借用指针；@cpu 是要检查的 per-CPU tick 槽。调用者同时持
 * clockevents_mutex 和 clockevents_lock。DETACHED 设备未被 tick 使用，可直接 list_del_init 并返回 0；
 * 若 ced 正是该 CPU 当前 evtdev，返回 -EAGAIN 要求在目标 CPU 上安装替代；其余仍有角色的设备返回
 * -EBUSY（包括不支持移除的 broadcast 等）。函数不可睡眠，不管理模块引用。
 */
static int __clockevents_try_unbind(struct clock_event_device *ced, int cpu)
{
	/* Fast track. Device is unused */
	/* detached 已关闭且无 tick ownership，摘链后驱动即可按外层协议回收。 */
	if (clockevent_state_detached(ced)) {
		list_del_init(&ced->list);
		return 0;
	}

	return ced == per_cpu(tick_cpu_device, cpu).evtdev ? -EAGAIN : -EBUSY;
}

/*
 * SMP function call to unbind a device
 */
/*
 * __clockevents_unbind() - 在目标 CPU 的同步 IPI 上下文完成当前 tick 设备替换。
 * @arg 指向发起 CPU 栈上的 ce_unbind 借用对象；wait=1 保证整个回调期间有效。cu/res 分别是请求和本地
 * 结果。取得 clockevents_lock 后先尝试直接解绑；若返回 -EAGAIN，说明正是本 CPU evtdev，调用 replace
 * 寻找替代。最终 errno 写回 cu->res 后解锁。函数在原子/关中断语境不可睡眠，无直接返回或资源释放。
 */
static void __clockevents_unbind(void *arg)
{
	struct ce_unbind *cu = arg;
	int res;

	raw_spin_lock(&clockevents_lock);
	res = __clockevents_try_unbind(cu->ce, smp_processor_id());
	if (res == -EAGAIN)
		res = clockevents_replace(cu->ce);
	cu->res = res;
	raw_spin_unlock(&clockevents_lock);
}

/*
 * Issues smp function call to unbind a per cpu device. Called with
 * clockevents_mutex held.
 */
/*
 * clockevents_unbind() - 同步请求 @cpu 在本地视角解绑一个 per-CPU clockevent。
 * @ced 是 mutex 稳定的设备借用指针；@cpu 必须是可接收同步 smp call 的目标。调用者已持
 * clockevents_mutex，可睡眠等待远端；cu 是栈上请求，res 初始 -ENODEV，使 smp call 未执行回调时保留
 * 明确失败。wait=1 保证远端完成后才返回 cu.res（0、-EBUSY 或 -ENODEV 等）。函数本身不释放对象；
 * 目标 CPU 的 replace 路径负责必要的 tick/module 引用迁移。
 */
static int clockevents_unbind(struct clock_event_device *ced, int cpu)
{
	struct ce_unbind cu = { .ce = ced, .res = -ENODEV };

	smp_call_function_single(cpu, __clockevents_unbind, &cu, 1);
	return cu.res;
}

/*
 * Unbind a clockevents device.
 */
/*
 * clockevents_unbind_device() - 对驱动公开的串行 per-CPU clockevent 解绑入口。
 * @ced 是驱动拥有且在返回成功前保持有效的设备；@cpu 指定其当前 tick 使用 CPU。函数在可睡眠进程上下文
 * 取得 clockevents_mutex，再同步调用目标 CPU 解绑，最后原样返回 0、-EBUSY、-ENODEV 等结果。只有返回
 * 0 后设备节点已摘除，驱动才可释放存储/模块资源；失败时 core 仍可能借用它。
 */
int clockevents_unbind_device(struct clock_event_device *ced, int cpu)
{
	int ret;

	mutex_lock(&clockevents_mutex);
	ret = clockevents_unbind(ced, cpu);
	mutex_unlock(&clockevents_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(clockevents_unbind_device);

/**
 * clockevents_register_device - register a clock event device
 * @dev:	device to register
 */
/*
 * clockevents_register_device() - 把配置完成的 clockevent 发布给 tick 选择器。
 * @dev 是驱动长期拥有的非 NULL 可写对象；调用前 list 节点必须未链接，回调/feature/rating/name 等字段已
 * 完整初始化，函数只借用对象。先把 state 设 DETACHED。cpumask 为空时仅适合 UP：SMP 会告警并回退当前
 * CPU；禁止直接使用可变语义的 cpu_all_mask 指针，告警后换成 cpu_possible_mask。
 *
 * 随后 irqsave 取得 clockevents_lock，把设备插入注册表，tick_check_new_device() 可能取得模块引用并安装
 * 为 regular/broadcast，又用 notify_released 递归稳定被替换设备。无返回值，注册/选择失败无法传播；返回
 * 后即使仍 DETACHED，驱动也必须先成功 unbind 才能释放。函数不可在持有相冲突 tick 锁时调用。
 */
void clockevents_register_device(struct clock_event_device *dev)
{
	unsigned long flags;

	/* Initialize state to DETACHED */
	/* 发布前先建立“已注册候选但未被 tick 使用”的软件状态。 */
	clockevent_set_state(dev, CLOCK_EVT_STATE_DETACHED);

	if (!dev->cpumask) {
		WARN_ON(num_possible_cpus() > 1);
		dev->cpumask = cpumask_of(smp_processor_id());
	}

	if (dev->cpumask == cpu_all_mask) {
		WARN(1, "%s cpumask == cpu_all_mask, using cpu_possible_mask instead\n",
		     dev->name);
		dev->cpumask = cpu_possible_mask;
	}

	raw_spin_lock_irqsave(&clockevents_lock, flags);

	/* list_add 是 core 借用对象的发布点；后续选择与释放链处理在同一锁代内完成。 */
	list_add(&dev->list, &clockevent_devices);
	tick_check_new_device(dev);
	clockevents_notify_released();

	raw_spin_unlock_irqrestore(&clockevents_lock, flags);
}
EXPORT_SYMBOL_GPL(clockevents_register_device);

/*
 * clockevents_config() - 按硬件频率计算 ONESHOT 设备的 ns→ticks 比例和安全边界。
 * @dev 是尚未发布或由外层 tick 协议独占的可写设备借用指针；@freq 是非零 Hz。非 ONESHOT 直接返回，
 * periodic 驱动可让 min/max ticks 为 0。sec 估算 max_delta_ticks 可覆盖的秒数，至少 1；宽于 32 位且超过
 * 10 分钟时钳 600 秒，以换取 mult/shift 精度。随后计算比例，并分别用 min/max 舍入语义生成
 * min_delta_ns/max_delta_ns。无返回值、不可睡眠；freq=0 会除零，调用者必须保证输入有效。
 */
static void clockevents_config(struct clock_event_device *dev, u32 freq)
{
	u64 sec;

	if (!(dev->features & CLOCK_EVT_FEAT_ONESHOT))
		return;

	/*
	 * Calculate the maximum number of seconds we can sleep. Limit
	 * to 10 minutes for hardware which can program more than
	 * 32bit ticks so we still get reasonable conversion values.
	 */
	/* 宽 counter 的理论跨度过大时只要求 600 秒安全范围，让 shift 保留更高的短期换算精度。 */
	sec = dev->max_delta_ticks;
	do_div(sec, freq);
	if (!sec)
		sec = 1;
	else if (sec > 600 && dev->max_delta_ticks > UINT_MAX)
		sec = 600;

	clockevents_calc_mult_shift(dev, freq, sec);
	dev->min_delta_ns = cev_delta2ns(dev->min_delta_ticks, dev, false);
	dev->max_delta_ns = cev_delta2ns(dev->max_delta_ticks, dev, true);
}

/**
 * clockevents_config_and_register - Configure and register a clock event device
 * @dev:	device to register
 * @freq:	The clock frequency
 * @min_delta:	The minimum clock ticks to program in oneshot mode
 * @max_delta:	The maximum clock ticks to program in oneshot mode
 *
 * min/max_delta can be 0 for devices which do not support oneshot mode.
 */
/*
 * clockevents_config_and_register() - 一次性写入硬件 tick 边界、计算比例并发布设备。
 * @dev 是驱动拥有的待注册对象；@freq 为非零 Hz；@min_delta/@max_delta 是 ONESHOT 可编程 ticks 边界，
 * 仅 periodic 设备可传 0。函数先提交两个 tick 字段，再调用 config 与 register；无返回值/回滚，返回后
 * core 已借用对象。运行在驱动初始化/CPU online 的可用上下文，不得预持 clockevents_lock。
 */
void clockevents_config_and_register(struct clock_event_device *dev,
				     u32 freq, unsigned long min_delta,
				     unsigned long max_delta)
{
	dev->min_delta_ticks = min_delta;
	dev->max_delta_ticks = max_delta;
	clockevents_config(dev, freq);
	clockevents_register_device(dev);
}
EXPORT_SYMBOL_GPL(clockevents_config_and_register);

/*
 * __clockevents_update_freq() - 重算设备比例并按当前运行状态恢复硬件编程。
 * @dev 是当前 CPU/tick 锁协议稳定的设备借用指针；@freq 是新非零 Hz。调用者必须关中断。config 先更新
 * mult/shift 与 ns 边界；ONESHOT 以原绝对 next_event、force=false 重编，返回 0 或已过期/驱动拒绝的
 * -ETIME；PERIODIC 直接再次调用状态回调刷新周期硬件，透传驱动 errno但不重写已是 periodic 的 state；
 * shutdown/detached 等返回 0。失败不会回滚新比例，调用者须处理“比例已更新但事件未重编”的部分提交。
 */
int __clockevents_update_freq(struct clock_event_device *dev, u32 freq)
{
	clockevents_config(dev, freq);

	if (clockevent_state_oneshot(dev))
		return clockevents_program_event(dev, dev->next_event, false);

	if (clockevent_state_periodic(dev))
		return __clockevents_switch_state(dev, CLOCK_EVT_STATE_PERIODIC);

	return 0;
}

/**
 * clockevents_update_freq - Update frequency and reprogram a clock event device.
 * @dev:	device to modify
 * @freq:	new device frequency
 *
 * Reconfigure and reprogram a clock event device in oneshot
 * mode. Must be called on the cpu for which the device delivers per
 * cpu timer events. If called for the broadcast device the core takes
 * care of serialization.
 *
 * Returns 0 on success, -ETIME when the event is in the past.
 */
/*
 * clockevents_update_freq() - 在正确序列化下更新普通或 broadcast clockevent 频率。
 * @dev 是调用 CPU 的 per-CPU 设备，或当前 broadcast 设备借用指针；@freq 为新非零 Hz。普通 per-CPU
 * 设备必须由其投递中断的 CPU 调用。函数保存并关闭本地中断，先让 tick_broadcast_update_freq 判断并在
 * broadcast 锁域更新；其返回 -ENODEV 表示 dev 非 broadcast，才回退 __clockevents_update_freq。
 * 返回 0 或底层 -ETIME/驱动 errno；恢复原 IRQ 状态。新比例可能已提交而重编失败，不转移 ownership。
 */
int clockevents_update_freq(struct clock_event_device *dev, u32 freq)
{
	unsigned long flags;
	int ret;

	local_irq_save(flags);
	ret = tick_broadcast_update_freq(dev, freq);
	if (ret == -ENODEV)
		ret = __clockevents_update_freq(dev, freq);
	local_irq_restore(flags);
	return ret;
}

/*
 * Noop handler when we shut down an event device
 */
/*
 * clockevents_handle_noop() - 关闭设备时使用的空 event_handler。
 * @dev 是触发陈旧/竞态中断的设备借用指针，当前不读取；可在硬中断上下文调用，不睡眠、无返回值或
 * 副作用。安装该 handler 可避免 shutdown 后残余中断跳入已释放的 tick 处理逻辑。
 */
void clockevents_handle_noop(struct clock_event_device *dev)
{
}

/**
 * clockevents_exchange_device - release and request clock devices
 * @old:	device to release (can be NULL)
 * @new:	device to request (can be NULL)
 *
 * Called from various tick functions with clockevents_lock held and
 * interrupts disabled.
 */
/*
 * clockevents_exchange_device() - 在 tick 角色切换时释放旧设备并准备新设备。
 * @old/@new 均为可空注册对象借用指针；调用者持 clockevents_lock 且关中断。old 非空时释放 tick 角色持有
 * 的模块引用，切到 DETACHED（硬件走 shutdown），再移入 released 临时链，稍后 notify 可把它匹配给另一
 * regular/broadcast 角色。new 非空必须仍 DETACHED，否则 BUG；随后 shutdown 清除遗留到期状态，实际
 * handler/state 安装由 tick 调用者继续完成。无返回值、不可睡眠；列表移动不改变驱动对象 ownership。
 */
void clockevents_exchange_device(struct clock_event_device *old,
				 struct clock_event_device *new)
{
	/*
	 * Caller releases a clock event device. We queue it into the
	 * released list and do a notify add later.
	 */
	/* 旧设备先脱离硬件角色，再暂存 released；不能立刻摘链，否则可能错过跨 regular/broadcast 复用。 */
	if (old) {
		module_put(old->owner);
		clockevents_switch_state(old, CLOCK_EVT_STATE_DETACHED);
		list_move(&old->list, &clockevents_released);
	}

	if (new) {
		BUG_ON(!clockevent_state_detached(new));
		clockevents_shutdown(new);
	}
}

/**
 * clockevents_suspend - suspend clock devices
 */
/*
 * clockevents_suspend() - 在系统 suspend 中按注册表反向暂停所有正在使用的 clockevent。
 * 入参：无；调用者已建立冻结/关中断语境，使注册表和 state 稳定，无需在此取得 clockevents_lock。dev 是
 * 列表借用游标；DETACHED 候选未启用硬件，跳过。链尾到链头的反向顺序与 resume 正向顺序
 * 配对。驱动 suspend 无返回值，错误不可传播；函数不改变 core state、链表或模块引用。
 */
void clockevents_suspend(void)
{
	struct clock_event_device *dev;

	list_for_each_entry_reverse(dev, &clockevent_devices, list)
		if (dev->suspend && !clockevent_state_detached(dev))
			dev->suspend(dev);
}

/**
 * clockevents_resume - resume clock devices
 */
/*
 * clockevents_resume() - 系统恢复时按注册表正向调用所有活动设备的 resume 钩子。
 * 入参、冻结语境和借用契约同 suspend；DETACHED 跳过。回调恢复硬件基础状态，具体 tick 模式/下一事件由
 * 后续 tick resume 路径重建。无返回值、错误传播或 ownership 变化；正向顺序与 suspend 反向配对。
 */
void clockevents_resume(void)
{
	struct clock_event_device *dev;

	list_for_each_entry(dev, &clockevent_devices, list)
		if (dev->resume && !clockevent_state_detached(dev))
			dev->resume(dev);
}

#ifdef CONFIG_HOTPLUG_CPU

/**
 * tick_offline_cpu - Shutdown all clock events related
 *                    to this CPU and take it out of the
 *                    broadcast mechanism.
 * @cpu:	The outgoing CPU
 *
 * Called by the dying CPU during teardown.
 */
/*
 * tick_offline_cpu() - 在 dying CPU 上关闭其 tick/broadcast 角色并摘除只属于它的 clockevent。
 * @cpu 是正在退出且仍执行 teardown 的 CPU；调用者的 hotplug 协议保证目标不再接收普通 tick 配置，函数在
 * 不可睡眠语境取得 clockevents_lock。先让 broadcast 层清 CPU 位并 shutdown 本地 tick；交换产生的
 * released 设备全部摘链。再扫描注册表：cpumask 包含该 CPU 且权重为 1 的非 broadcast 专用设备必须已
 * DETACHED，否则 BUG，随后摘链；共享多 CPU 和 broadcast 设备保留。
 *
 * 无返回值。list_del 只解除 clockevents core 借用，不释放驱动存储；CPU hotplug 驱动层在回调返回后管理
 * 设备资源。模块角色引用已由前面的 tick_shutdown/exchange 配对，持锁期间不睡眠。
 */
void tick_offline_cpu(unsigned int cpu)
{
	struct clock_event_device *dev, *tmp;

	raw_spin_lock(&clockevents_lock);

	tick_broadcast_offline(cpu);
	tick_shutdown();

	/*
	 * Unregister the clock event devices which were
	 * released above.
	 */
	/* tick_shutdown 刚释放的本 CPU 角色不再重新匹配，直接从 released 链摘除。 */
	list_for_each_entry_safe(dev, tmp, &clockevents_released, list)
		list_del(&dev->list);

	/*
	 * Now check whether the CPU has left unused per cpu devices
	 */
	/* 只删除唯一归属下线 CPU 且已 detached 的设备；共享/广播对象仍服务其余 CPU。 */
	list_for_each_entry_safe(dev, tmp, &clockevent_devices, list) {
		if (cpumask_test_cpu(cpu, dev->cpumask) &&
		    cpumask_weight(dev->cpumask) == 1 &&
		    !tick_is_broadcast_device(dev)) {
			BUG_ON(!clockevent_state_detached(dev));
			list_del(&dev->list);
		}
	}

	raw_spin_unlock(&clockevents_lock);
}
#endif

#ifdef CONFIG_SYSFS
/*
 * clockevents_subsys 为每个 possible CPU 创建 clockevent<ID> 设备，并可选创建 broadcast 设备；driver core
 * 管理 sysfs 可见性。tick_percpu_dev 是静态 per-CPU device 容器，tick_get_tick_dev 前置声明统一普通与
 * broadcast 的 show 路由；这些实体内核全生命周期常驻，不由属性回调释放。
 */
static const struct bus_type clockevents_subsys = {
	.name		= "clockevents",
	.dev_name       = "clockevent",
};

static DEFINE_PER_CPU(struct device, tick_percpu_dev);
static struct tick_device *tick_get_tick_dev(struct device *dev);

/*
 * current_device_show() - 输出指定 CPU 或 broadcast 当前 clockevent 名称。
 * @dev 是 sysfs device 借用指针，id 表示 CPU；@attr 未使用；@buf 是 PAGE_SIZE 输出缓冲。函数在可睡眠
 * sysfs read 上下文用 raw_spin_lock_irq 稳定 tick_device/evtdev，存在当前设备时写“name\n”，否则返回
 * 0 字节空内容。count 是写入长度；无 errno、引用或状态变化，持 raw lock 期间 sysfs_emit 仅做内存格式化。
 */
static ssize_t current_device_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct tick_device *td;
	ssize_t count = 0;

	raw_spin_lock_irq(&clockevents_lock);
	td = tick_get_tick_dev(dev);
	if (td && td->evtdev)
		count = sysfs_emit(buf, "%s\n", td->evtdev->name);
	raw_spin_unlock_irq(&clockevents_lock);
	return count;
}
/* 为每个 CPU 与 broadcast 设备生成只读 current_device 属性对象。 */
static DEVICE_ATTR_RO(current_device);

/* We don't support the abomination of removable broadcast devices */
/* broadcast sysfs 只发布 current_device，不发布本 store；用户不能从 sysfs 移除广播时钟事件设备。 */
/*
 * unbind_device_store() - 按名称从当前 sysfs CPU 槽解绑一个 clockevent。
 * @dev 的 id 是目标 CPU；@attr 未使用；@buf/@count 是未保证 NUL 结尾的用户输入借用缓冲。name 为栈上
 * 规范化名称，ce/iter 是 mutex 稳定的设备借用指针。解析失败返回 -EINVAL；锁内未命中返回 -ENODEV。
 *
 * 先按 mutex→raw spin 锁序遍历注册表并调用 try_unbind：detached 可当场摘链返回 0；目标 CPU 当前设备
 * 返回 -EAGAIN。释放 raw lock 后仍持 mutex，ce 生命周期稳定，再以同步 smp call 让目标 CPU 安装替代；
 * 其他活动角色返回 -EBUSY。最终成功返回原始 count，失败原样返回 errno。函数不释放驱动对象/模块，
 * 只有成功后外部 owner 才可回收；同名设备只处理注册表中的首个匹配项。
 */
static ssize_t unbind_device_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	char name[CS_NAME_LEN];
	ssize_t ret = sysfs_get_uname(buf, name, count);
	struct clock_event_device *ce = NULL, *iter;

	if (ret < 0)
		return ret;

	ret = -ENODEV;
	mutex_lock(&clockevents_mutex);
	raw_spin_lock_irq(&clockevents_lock);
	list_for_each_entry(iter, &clockevent_devices, list) {
		if (!strcmp(iter->name, name)) {
			ret = __clockevents_try_unbind(iter, dev->id);
			ce = iter;
			break;
		}
	}
	raw_spin_unlock_irq(&clockevents_lock);
	/*
	 * We hold clockevents_mutex, so ce can't go away
	 */
	/* spinlock 外可发同步 IPI；mutex 继续阻止其他解绑者让 ce 在请求期间失效。 */
	if (ret == -EAGAIN)
		ret = clockevents_unbind(ce, dev->id);
	mutex_unlock(&clockevents_mutex);
	return ret ? ret : count;
}
/* 普通 per-CPU sysfs 设备生成只写 unbind_device 属性；broadcast 不安装它。 */
static DEVICE_ATTR_WO(unbind_device);

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
/*
 * tick_bc_dev 是 id=0 的静态 broadcast sysfs device，只展示当前设备，不提供 unbind；bus 指向上方 subsystem，
 * driver core 注册后借用该常驻对象。
 */
static struct device tick_bc_dev = {
	.init_name	= "broadcast",
	.id		= 0,
	.bus		= &clockevents_subsys,
};

/*
 * tick_get_tick_dev() - 把 sysfs device 映射为 broadcast 或 per-CPU tick_device。
 * @dev 是已注册的静态 sysfs 对象借用指针。指针等于 tick_bc_dev 时返回 broadcast 层借用指针，否则按
 * dev->id 返回 per_cpu tick_cpu_device 地址。调用者以 clockevents_lock 稳定返回对象；无失败码、引用增加
 * 或副作用，错误 id 会越界，创建路径必须保证合法。
 */
static struct tick_device *tick_get_tick_dev(struct device *dev)
{
	return dev == &tick_bc_dev ? tick_get_broadcast_device() :
		&per_cpu(tick_cpu_device, dev->id);
}

/*
 * tick_broadcast_init_sysfs() - 注册 broadcast sysfs device 及其 current_device 属性。
 * 入参：无；在 device initcall 的可睡眠初始化上下文调用。先 device_register，成功后 create_file；返回
 * 首个 0/errno。属性创建失败时当前实现不注销已注册 device，留下部分可见对象且无本地回滚。
 */
static __init int tick_broadcast_init_sysfs(void)
{
	int err = device_register(&tick_bc_dev);

	if (!err)
		err = device_create_file(&tick_bc_dev, &dev_attr_current_device);
	return err;
}
#else
/*
 * 未启用 broadcast 时，tick_get_tick_dev() 只按 @dev->id 返回对应 per-CPU tick_device 借用指针；调用者
 * 持 clockevents_lock，创建路径保证 id 合法。无错误、引用或副作用。
 */
static struct tick_device *tick_get_tick_dev(struct device *dev)
{
	return &per_cpu(tick_cpu_device, dev->id);
}
/* broadcast 配置关闭时无需创建额外 sysfs 设备；入参：无，固定成功且无副作用。 */
static inline int tick_broadcast_init_sysfs(void) { return 0; }
#endif

/*
 * tick_init_sysfs() - 为每个 possible CPU 注册 sysfs device 和 current/unbind 两个属性，再初始化 broadcast。
 * 入参：无；运行在可睡眠初始化上下文。cpu 是 possible CPU 游标，dev 为其静态 per-CPU device 借用指针，
 * err 保存当前注册阶段结果。依次写 id/bus、注册 device、创建 current_device、创建 unbind_device；任一步
 * 失败立即返回 errno，不撤销此前 CPU 或当前 device 已成功的部分。全部 CPU 成功后返回 broadcast 初始化
 * 结果。设备/属性由 driver core 持有，文件没有 teardown 路径。
 */
static int __init tick_init_sysfs(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct device *dev = &per_cpu(tick_percpu_dev, cpu);
		int err;

		dev->id = cpu;
		dev->bus = &clockevents_subsys;
		err = device_register(dev);
		if (!err)
			err = device_create_file(dev, &dev_attr_current_device);
		if (!err)
			err = device_create_file(dev, &dev_attr_unbind_device);
		if (err)
			return err;
	}
	return tick_broadcast_init_sysfs();
}

/*
 * clockevents_init_sysfs() - 在 device_initcall 阶段发布 clockevents subsystem 及其 tick 设备树。
 * 入参：无；先 subsys_system_register，成功才调用 tick_init_sysfs，返回首个 0/errno。后阶段失败不会注销
 * 已发布 subsystem/设备，是启动期部分注册边界；函数可睡眠，无重试或动态资源 ownership 返回给调用者。
 */
static int __init clockevents_init_sysfs(void)
{
	int err = subsys_system_register(&clockevents_subsys, NULL);

	if (!err)
		err = tick_init_sysfs();
	return err;
}
/* sysfs 晚于核心 clockevent 注册启动，属性读取时以锁观察当前实际 tick 角色。 */
device_initcall(clockevents_init_sysfs);
#endif /* SYSFS */
