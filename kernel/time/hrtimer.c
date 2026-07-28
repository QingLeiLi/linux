// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright(C) 2005-2006, Linutronix GmbH, Thomas Gleixner <tglx@kernel.org>
 *  Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 *  Copyright(C) 2006-2007  Timesys Corp., Thomas Gleixner
 *
 *  High-resolution kernel timers
 *
 *  In contrast to the low-resolution timeout API, aka timer wheel,
 *  hrtimers provide finer resolution and accuracy depending on system
 *  configuration and capabilities.
 *
 *  Started by: Thomas Gleixner and Ingo Molnar
 *
 *  Credits:
 *	Based on the original timer wheel code
 *
 *	Help, testing, suggestions, bugfixes, improvements were
 *	provided by:
 *
 *	George Anzinger, Andrew Morton, Steven Rostedt, Roman Zippel
 *	et. al.
 */

#include <linux/cpu.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/hrtimer.h>
#include <linux/notifier.h>
#include <linux/syscalls.h>
#include <linux/interrupt.h>
#include <linux/tick.h>
#include <linux/err.h>
#include <linux/debugobjects.h>
#include <linux/sched/signal.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/rt.h>
#include <linux/sched/deadline.h>
#include <linux/sched/nohz.h>
#include <linux/sched/debug.h>
#include <linux/sched/isolation.h>
#include <linux/timer.h>
#include <linux/freezer.h>
#include <linux/compat.h>

#include <linux/uaccess.h>

#include <trace/events/timer.h>

#include "tick-internal.h"

/*
 * Constants to set the queued state of the timer (INACTIVE, ENQUEUED)
 *
 * The callback state is kept separate in the CPU base because having it in
 * the timer would required touching the timer after the callback, which
 * makes it impossible to free the timer from the callback function.
 *
 * Therefore we track the callback state in:
 *
 *	timer->base->cpu_base->running == timer
 *
 * On SMP it is possible to have a "callback function running and enqueued"
 * status. It happens for example when a posix timer expired and the callback
 * queued a signal. Between dropping the lock which protects the posix timer
 * and reacquiring the base lock of the hrtimer, another CPU can deliver the
 * signal and rearm the timer.
 *
 * All state transitions are protected by cpu_base->lock.
 */
/*
 * hrtimer 状态模型：
 *
 * is_queued 只描述 timerqueue 红黑树中的成员关系，回调执行状态单独记录在
 * clock_base->running。分离的关键原因是 callback 可以释放包含 hrtimer 的
 * 对象，执行结束后不能再为清理状态而解引用 timer。删除/重启/迁移均在
 * cpu_base->lock 下线性化。
 *
 * SMP 上允许“正在 callback 且已再次入队”：回调释放锁后，另一 CPU 可在旧
 * callback 返回前重新启动同一 timer。hrtimer_active() 因而必须把 queued、
 * running 和 migration 三种状态作为一个 seqcount 快照观察，不能只看布尔位。
 */
#define HRTIMER_STATE_INACTIVE	false
#define HRTIMER_STATE_ENQUEUED	true

/*
 * The resolution of the clocks. The resolution value is returned in
 * the clock_getres() system call to give application programmers an
 * idea of the (in)accuracy of timers. Timer values are rounded up to
 * this resolution values.
 */
/* 高分辨率模式理论分辨率为 1ns；实际准确度仍受 clocksource/clockevent 硬件约束。 */
#define HIGH_RES_NSEC		1

/*
 * Masks for selecting the soft and hard context timers from
 * cpu_base->active
 */
/*
 * 每 CPU base 前半保存 hardirq 到期队列，后半保存 softirq 队列。active_bases
 * 的对应位让扫描跳过空树；这些 mask 选择本轮允许执行的上下文类别。
 */
#define MASK_SHIFT		(HRTIMER_BASE_MONOTONIC_SOFT)
#define HRTIMER_ACTIVE_HARD	((1U << MASK_SHIFT) - 1)
#define HRTIMER_ACTIVE_SOFT	(HRTIMER_ACTIVE_HARD << MASK_SHIFT)
#define HRTIMER_ACTIVE_ALL	(HRTIMER_ACTIVE_SOFT | HRTIMER_ACTIVE_HARD)

static void retrigger_next_event(void *arg);
static ktime_t __hrtimer_cb_get_time(clockid_t clock_id);

/*
 * The timer bases:
 *
 * There are more clockids than hrtimer bases. Thus, we index
 * into the timer bases by the hrtimer_base_type enum. When trying
 * to reach a base using a clockid, hrtimer_clockid_to_base()
 * is used to convert from clockid to the proper hrtimer_base_type.
 */
/*
 * clockid 与内部 base 不是一一对应：MONOTONIC、REALTIME、BOOTTIME、TAI 各有
 * hard/soft 两套树。hrtimer_clockid_to_base() 先映射时钟种类，mode 再决定
 * hard/soft 半区。所有 clock_base 共享同一 per-CPU raw lock 和硬件事件源。
 */

#define BASE_INIT(idx, cid)			\
	[idx] = { .index = idx, .clockid = cid }

DEFINE_PER_CPU(struct hrtimer_cpu_base, hrtimer_bases) =
{
	.lock = __RAW_SPIN_LOCK_UNLOCKED(hrtimer_bases.lock),
	.clock_base = {
		BASE_INIT(HRTIMER_BASE_MONOTONIC,	CLOCK_MONOTONIC),
		BASE_INIT(HRTIMER_BASE_REALTIME,	CLOCK_REALTIME),
		BASE_INIT(HRTIMER_BASE_BOOTTIME,	CLOCK_BOOTTIME),
		BASE_INIT(HRTIMER_BASE_TAI,		CLOCK_TAI),
		BASE_INIT(HRTIMER_BASE_MONOTONIC_SOFT,	CLOCK_MONOTONIC),
		BASE_INIT(HRTIMER_BASE_REALTIME_SOFT,	CLOCK_REALTIME),
		BASE_INIT(HRTIMER_BASE_BOOTTIME_SOFT,	CLOCK_BOOTTIME),
		BASE_INIT(HRTIMER_BASE_TAI_SOFT,	CLOCK_TAI),
	},
	.csd = CSD_INIT(retrigger_next_event, NULL)
};
/*
 * hrtimer_bases 是每 CPU 生命周期根。clock_base[] 按到期时间组织 timerqueue；
 * csd 用于远端 CPU 重算并重编程本地 clockevent。静态初始化只填不可变 clockid/
 * index，队列头、seqcount 和反向 cpu_base 指针在 hrtimers_prepare_cpu() 建立。
 */

/*
 * hrtimer_base_is_online() - 判断目标 base 是否仍接受新 timer。
 *
 * @base 为借用指针；返回布尔值，不加锁。无 CPU hotplug 时恒真；有 hotplug 时
 * online 在下线锁协议中发布，调用者仍需在持 base 锁后复核迁移决策。
 */
static inline bool hrtimer_base_is_online(struct hrtimer_cpu_base *base)
{
	if (!IS_ENABLED(CONFIG_HOTPLUG_CPU))
		return true;
	else
		return likely(base->online);
}

#ifdef CONFIG_HIGH_RES_TIMERS
DEFINE_STATIC_KEY_FALSE(hrtimer_highres_enabled_key);
/* 全局 static key 表示至少已完成异步 highres 切换发布，供热路径快速查询。 */

/*
 * hrtimer_hres_workfn() - 在 workqueue 进程上下文启用 highres static key。
 *
 * @work 仅为 ABI 参数；无返回值。static key 更新可能修改内核文本，故不在时钟
 * 切换的敏感上下文直接完成。
 */
static void hrtimer_hres_workfn(struct work_struct *work)
{
	static_branch_enable(&hrtimer_highres_enabled_key);
}

static DECLARE_WORK(hrtimer_hres_work, hrtimer_hres_workfn);
/* 静态 work 合并多个 CPU 的重复启用请求；对象无需动态释放。 */

/* 若全局 key 尚未发布，异步调度唯一 work；无返回值且可重复调用。 */
static inline void hrtimer_schedule_hres_work(void)
{
	if (!hrtimer_highres_enabled())
		schedule_work(&hrtimer_hres_work);
}
#else
/* 未配置 HIGH_RES_TIMERS 时没有 static key 更新工作。 */
static inline void hrtimer_schedule_hres_work(void) { }
#endif

/*
 * Functions and macros which are different for UP/SMP systems are kept in a
 * single place
 */
/* 中文说明：把 UP/SMP 差异集中在本段，后续启动、取消和执行路径保持同一协议。 */
#ifdef CONFIG_SMP
/*
 * We require the migration_base for lock_hrtimer_base()/switch_hrtimer_base()
 * such that hrtimer_callback_running() can unconditionally dereference
 * timer->base->cpu_base
 */
/*
 * migration_base 是换锁期间的稳定哨兵，不保存可执行 timer。timer->base 指向它
 * 时，查找者知道 timer 仍由迁移者独占，且 callback_running() 仍可无条件安全
 * 解引用 base->cpu_base。其生命周期为静态全局。
 */
static struct hrtimer_cpu_base migration_cpu_base = {
	.clock_base = {
		[0] = {
			.cpu_base = &migration_cpu_base,
			.seq      = SEQCNT_RAW_SPINLOCK_ZERO(migration_cpu_base.seq,
							     &migration_cpu_base.lock),
		},
	},
};

#define migration_base	migration_cpu_base.clock_base[0]

/*
 * We are using hashed locking: holding per_cpu(hrtimer_bases)[n].lock
 * means that all timers which are tied to this base via timer->base are
 * locked, and the base itself is locked too.
 *
 * So __run_timers/migrate_timers can safely modify all timers which could
 * be found on the lists/queues.
 *
 * When the timer's base is locked, and the timer removed from list, it is
 * possible to set timer->base = &migration_base and drop the lock: the timer
 * remains locked.
 */
/*
 * 中文锁协议：timer->base 是“锁地址的散列键”。读取 base、锁 cpu_base、再复核
 * timer->base 未变，才能确认拿到正确锁。迁移者先把 base 发布为 migration_base，
 * 释放旧锁并获取新锁后再发布目标 base；期间其他操作者只能重试。
 */
/*
 * lock_hrtimer_base() - 在并发迁移下锁住 timer 当前所属的 CPU base。
 *
 * @timer 为借用对象；@flags 输出 IRQ 状态，必须交给 unlock_hrtimer_base()。
 * 返回已加锁 clock_base；不睡眠。循环复核避免在 base 改变后拿旧锁操作新树。
 */
static struct hrtimer_clock_base *lock_hrtimer_base(const struct hrtimer *timer,
						    unsigned long *flags)
	__acquires(&timer->base->lock)
{
	for (;;) {
		struct hrtimer_clock_base *base = READ_ONCE(timer->base);

		if (likely(base != &migration_base)) {
			raw_spin_lock_irqsave(&base->cpu_base->lock, *flags);
			if (likely(base == timer->base))
				return base;
			/* The timer has migrated to another CPU: */
			/* 中文说明：锁前后 base 不一致，释放旧锁并等待迁移者完成发布。 */
			raw_spin_unlock_irqrestore(&base->cpu_base->lock, *flags);
		}
		cpu_relax();
	}
}

/*
 * Check if the elected target is suitable considering its next
 * event and the hotplug state of the current CPU.
 *
 * If the elected target is remote and its next event is after the timer
 * to queue, then a remote reprogram is necessary. However there is no
 * guarantee the IPI handling the operation would arrive in time to meet
 * the high resolution deadline. In this case the local CPU becomes a
 * preferred target, unless it is offline.
 *
 * High and low resolution modes are handled the same way for simplicity.
 *
 * Called with cpu_base->lock of target cpu held.
 */
/*
 * hrtimer_suitable_target() - 判断省电选出的目标 CPU 能否及时服务该期限。
 *
 * @timer/@new_base/@new_cpu_base/@this_cpu_base 均为借用指针，目标 base 锁已持有。
 * 本地目标总可直接重编程；远端若其已编程事件晚于新 timer，IPI 未必赶得上纳秒
 * deadline，在线本地 CPU 更安全。返回 true 表示保留候选，不改变 ownership。
 */
static bool hrtimer_suitable_target(struct hrtimer *timer, struct hrtimer_clock_base *new_base,
				    struct hrtimer_cpu_base *new_cpu_base,
				    struct hrtimer_cpu_base *this_cpu_base)
{
	ktime_t expires;

	/*
	 * The local CPU clockevent can be reprogrammed. Also get_target_base()
	 * guarantees it is online.
	 */
	/* 本地 clockevent 可同步重编程，且 get_target_base 已保证本地 online。 */
	if (new_cpu_base == this_cpu_base)
		return true;

	/*
	 * The offline local CPU can't be the default target if the
	 * next remote target event is after this timer. Keep the
	 * elected new base. An IPI will be issued to reprogram
	 * it as a last resort.
	 */
	/* 本地已下线时只能保留远端候选，即使最终需要 IPI 作为兜底。 */
	if (!hrtimer_base_is_online(this_cpu_base))
		return true;

	expires = ktime_sub(hrtimer_get_expires(timer), new_base->offset);

	return expires >= new_base->cpu_base->expires_next;
}

/*
 * get_target_base() - 为一次启动选择 CPU 级 hrtimer base。
 *
 * @base 通常是当前 CPU，@pinned 禁止迁移。当前 CPU 下线时选在线 housekeeping
 * CPU；NO_HZ migration 开启且非 pinned 时可选省电目标。返回静态 per-CPU 借用
 * 指针，最终适配性仍由持锁的 hrtimer_suitable_target() 验证。
 */
static inline struct hrtimer_cpu_base *get_target_base(struct hrtimer_cpu_base *base, bool pinned)
{
	if (!hrtimer_base_is_online(base)) {
		int cpu = cpumask_any_and(cpu_online_mask, housekeeping_cpumask(HK_TYPE_TIMER));

		return &per_cpu(hrtimer_bases, cpu);
	}

#if defined(CONFIG_SMP) && defined(CONFIG_NO_HZ_COMMON)
	if (static_branch_likely(&timers_migration_enabled) && !pinned)
		return &per_cpu(hrtimer_bases, get_nohz_timer_target());
#endif
	return base;
}

/*
 * We switch the timer base to a power-optimized selected CPU target,
 * if:
 *	- NO_HZ_COMMON is enabled
 *	- timer migration is enabled
 *	- the timer callback is not running
 *	- the timer is not the first expiring timer on the new target
 *
 * If one of the above requirements is not fulfilled we move the timer
 * to the current CPU or leave it on the previously assigned CPU if
 * the timer callback is currently running.
 */
/*
 * switch_hrtimer_base() - 在保持 timer 独占的同时完成跨 CPU base 换锁。
 *
 * @timer 当前已从队列摘除；@base 的 cpu_base 锁已持有；@pinned 决定能否省电
 * 迁移。返回仍加锁的新 clock_base。callback 正在运行时保持原 CPU，使取消者
 * 仍能通过 running 找到它；其余路径用 migration_base 封住换锁窗口。
 */
static inline struct hrtimer_clock_base *
switch_hrtimer_base(struct hrtimer *timer, struct hrtimer_clock_base *base, bool pinned)
{
	struct hrtimer_cpu_base *new_cpu_base, *this_cpu_base;
	struct hrtimer_clock_base *new_base;
	int basenum = base->index;

	this_cpu_base = this_cpu_ptr(&hrtimer_bases);
	new_cpu_base = get_target_base(this_cpu_base, pinned);
again:
	new_base = &new_cpu_base->clock_base[basenum];

	if (base != new_base) {
		/*
		 * We are trying to move timer to new_base. However we can't
		 * change timer's base while it is running, so we keep it on
		 * the same CPU. No hassle vs. reprogramming the event source
		 * in the high resolution case. The remote CPU will take care
		 * of this when the timer function has completed. There is no
		 * conflict as we hold the lock until the timer is enqueued.
		 */
		/*
		 * running 是 callback 生命周期锚点。若此时更换 base，同步取消者可能锁
		 * 新 CPU 却漏看旧 CPU 上尚未返回的 callback，因此必须留在旧 base。
		 */
		if (unlikely(hrtimer_callback_running(timer)))
			return base;

		/* See the comment in lock_hrtimer_base() */
		/* 先发布迁移哨兵，确保两把锁之间 timer 不会被第三方误操作。 */
		WRITE_ONCE(timer->base, &migration_base);
		raw_spin_unlock(&base->cpu_base->lock);
		raw_spin_lock(&new_base->cpu_base->lock);

		if (!hrtimer_suitable_target(timer, new_base, new_cpu_base, this_cpu_base)) {
			raw_spin_unlock(&new_base->cpu_base->lock);
			raw_spin_lock(&base->cpu_base->lock);
			new_cpu_base = this_cpu_base;
			WRITE_ONCE(timer->base, base);
			goto again;
		}
		WRITE_ONCE(timer->base, new_base);
	} else {
		if (!hrtimer_suitable_target(timer, new_base,  new_cpu_base, this_cpu_base)) {
			new_cpu_base = this_cpu_base;
			goto again;
		}
	}
	return new_base;
}

#else /* CONFIG_SMP */

/*
 * UP lock_hrtimer_base() - 无迁移版本：直接锁 timer->base 的 cpu_base。
 * @flags 输出 IRQ 状态；返回已锁借用 base。
 */
static inline struct hrtimer_clock_base *lock_hrtimer_base(const struct hrtimer *timer,
							   unsigned long *flags)
	__acquires(&timer->base->cpu_base->lock)
{
	struct hrtimer_clock_base *base = timer->base;

	raw_spin_lock_irqsave(&base->cpu_base->lock, *flags);
	return base;
}

# define switch_hrtimer_base(t, b, p)	(b)

#endif	/* !CONFIG_SMP */

/*
 * Functions for the union type storage format of ktime_t which are
 * too large for inlining:
 */
/* 32 位平台把过大的 ktime 运算放到非 inline helper，避免每个调用点膨胀。 */
#if BITS_PER_LONG < 64
/*
 * Divide a ktime value by a nanosecond value
 */
/*
 * __ktime_divns() - 32 位平台的有符号纳秒除法。
 *
 * @kt 是 ktime，@div 是纳秒除数且必须非零；返回有符号商。先保存符号，再把
 * 除数缩到 do_div() 可接受的 32 位，同时对被除数做相同比例缩放。
 */
s64 __ktime_divns(const ktime_t kt, s64 div)
{
	int sft = 0;
	s64 dclc;
	u64 tmp;

	dclc = ktime_to_ns(kt);
	tmp = dclc < 0 ? -dclc : dclc;

	/* Make sure the divisor is less than 2^32: */
	/* do_div 的除数是 u32；同步右移保持商的近似比例，避免链接 64 位除法 helper。 */
	while (div >> 32) {
		sft++;
		div >>= 1;
	}
	tmp >>= sft;
	do_div(tmp, (u32) div);
	return dclc < 0 ? -tmp : tmp;
}
EXPORT_SYMBOL_GPL(__ktime_divns);
#endif /* BITS_PER_LONG < 64 */

/*
 * Add two ktime values and do a safety check for overflow:
 */
/*
 * ktime_add_safe() - 饱和相加两个 ktime 纳秒值。
 *
 * @lhs/@rhs 为纯输入；返回精确和，若有符号溢出则返回用户 timespec 可表达的
 * KTIME_SEC_MAX。无锁无副作用，用饱和值避免溢出伪装成已过期负时间。
 */
ktime_t ktime_add_safe(const ktime_t lhs, const ktime_t rhs)
{
	ktime_t res = ktime_add_unsafe(lhs, rhs);

	/*
	 * We use KTIME_SEC_MAX here, the maximum timeout which we can
	 * return to user space in a timespec:
	 */
	/* 选择用户 ABI 上限，使随后 copyout 不会产生内核可表示、用户不可表示的期限。 */
	if (res < 0 || res < lhs || res < rhs)
		res = ktime_set(KTIME_SEC_MAX, 0);

	return res;
}

EXPORT_SYMBOL_GPL(ktime_add_safe);

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS

static const struct debug_obj_descr hrtimer_debug_descr;

/*
 * hrtimer_debug_hint() - 返回 timer 的业务 callback 地址供 debugobjects 报告。
 * @addr 为借用对象；返回值不取得引用。
 */
static void *hrtimer_debug_hint(void *addr)
{
	return ACCESS_PRIVATE((struct hrtimer *)addr, function);
}

/*
 * fixup_init is called when:
 * - an active object is initialized
 */
/* 中文说明：重新初始化 active timer 时先同步取消，再重置诊断状态；true 表示修复。 */
static bool hrtimer_fixup_init(void *addr, enum debug_obj_state state)
{
	struct hrtimer *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		hrtimer_cancel(timer);
		debug_object_init(timer, &hrtimer_debug_descr);
		return true;
	default:
		return false;
	}
}

/*
 * fixup_activate is called when:
 * - an active object is activated
 * - an unknown non-static object is activated
 */
/* 中文说明：重复激活只告警，不擅自改变正在使用的 timer；返回 false 表示未修复。 */
static bool hrtimer_fixup_activate(void *addr, enum debug_obj_state state)
{
	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		WARN_ON(1);
		fallthrough;
	default:
		return false;
	}
}

/*
 * fixup_free is called when:
 * - an active object is freed
 */
/* 中文说明：释放 active timer 前同步取消 callback，防止容器释放后的 UAF。 */
static bool hrtimer_fixup_free(void *addr, enum debug_obj_state state)
{
	struct hrtimer *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		hrtimer_cancel(timer);
		debug_object_free(timer, &hrtimer_debug_descr);
		return true;
	default:
		return false;
	}
}

/* Stub timer callback for improperly used timers. */
/* 非法 timer 的安全回调：告警并明确不重启，避免执行未知函数指针。 */
static enum hrtimer_restart stub_timer(struct hrtimer *unused)
{
	WARN_ON_ONCE(1);
	return HRTIMER_NORESTART;
}

/*
 * hrtimer_fixup_assert_init is called when:
 * - an untracked/uninit-ed object is found
 */
/* 中文说明：未知/未初始化对象被使用时安装 stub，保留可诊断且不会周期重启的状态。 */
static bool hrtimer_fixup_assert_init(void *addr, enum debug_obj_state state)
{
	struct hrtimer *timer = addr;

	switch (state) {
	case ODEBUG_STATE_NOTAVAILABLE:
		hrtimer_setup(timer, stub_timer, CLOCK_MONOTONIC, 0);
		return true;
	default:
		return false;
	}
}

static const struct debug_obj_descr hrtimer_debug_descr = {
	/* debugobjects 针对 hrtimer 的静态操作表；只读且不拥有业务 timer。 */
	.name			= "hrtimer",
	.debug_hint		= hrtimer_debug_hint,
	.fixup_init		= hrtimer_fixup_init,
	.fixup_activate		= hrtimer_fixup_activate,
	.fixup_free		= hrtimer_fixup_free,
	.fixup_assert_init	= hrtimer_fixup_assert_init,
};

/* 登记普通 timer 初始化；只影响 debugobjects 状态。 */
static inline void debug_hrtimer_init(struct hrtimer *timer)
{
	debug_object_init(timer, &hrtimer_debug_descr);
}

/* 栈对象使用专门登记，离开作用域前必须 destroy_hrtimer_on_stack()。 */
static inline void debug_hrtimer_init_on_stack(struct hrtimer *timer)
{
	debug_object_init_on_stack(timer, &hrtimer_debug_descr);
}

/* 入队前把诊断状态发布为 active；@mode 仅供一致的接口/追踪语义。 */
static inline void debug_hrtimer_activate(struct hrtimer *timer, enum hrtimer_mode mode)
{
	debug_object_activate(timer, &hrtimer_debug_descr);
}

/* 摘队列时把诊断状态恢复为 inactive，不负责树操作。 */
static inline void debug_hrtimer_deactivate(struct hrtimer *timer)
{
	debug_object_deactivate(timer, &hrtimer_debug_descr);
}

/* 公开操作入口断言 timer 已初始化；release 配置可为空。 */
static inline void debug_hrtimer_assert_init(struct hrtimer *timer)
{
	debug_object_assert_init(timer, &hrtimer_debug_descr);
}

/*
 * destroy_hrtimer_on_stack() - 撤销栈上 hrtimer 的 debugobjects 登记。
 * @timer 必须已取消且 callback 已结束；无返回值，不代替 hrtimer_cancel()。
 */
void destroy_hrtimer_on_stack(struct hrtimer *timer)
{
	debug_object_free(timer, &hrtimer_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_hrtimer_on_stack);

#else

/* 无 DEBUG_OBJECTS 时以下 hook 均为空，业务生命周期不能依赖诊断配置。 */
static inline void debug_hrtimer_init(struct hrtimer *timer) { }
static inline void debug_hrtimer_init_on_stack(struct hrtimer *timer) { }
static inline void debug_hrtimer_activate(struct hrtimer *timer, enum hrtimer_mode mode) { }
static inline void debug_hrtimer_deactivate(struct hrtimer *timer) { }
static inline void debug_hrtimer_assert_init(struct hrtimer *timer) { }
#endif

/* debug_setup() 联合发布诊断初始化与 setup trace；不入队。 */
static inline void debug_setup(struct hrtimer *timer, clockid_t clockid, enum hrtimer_mode mode)
{
	debug_hrtimer_init(timer);
	trace_hrtimer_setup(timer, clockid, mode);
}

/* 栈对象版本的诊断/trace setup，ownership 仍归调用者。 */
static inline void debug_setup_on_stack(struct hrtimer *timer, clockid_t clockid,
					enum hrtimer_mode mode)
{
	debug_hrtimer_init_on_stack(timer);
	trace_hrtimer_setup(timer, clockid, mode);
}

/* 入队时统一激活 debugobjects 并记录是否曾 armed 的 trace。 */
static inline void debug_activate(struct hrtimer *timer, enum hrtimer_mode mode, bool was_armed)
{
	debug_hrtimer_activate(timer, mode);
	trace_hrtimer_start(timer, mode, was_armed);
}

#define for_each_active_base(base, cpu_base, active)					\
	for (unsigned int idx = ffs(active); idx--; idx = ffs((active)))		\
		for (bool done = false; !done; active &= ~(1U << idx))			\
			for (base = &cpu_base->clock_base[idx]; !done; done = true)

#define hrtimer_from_timerqueue_node(_n) container_of_const(_n, struct hrtimer, node)
/*
 * for_each_active_base 逐个消费 active 位并给出 clock_base；嵌套 for 保证宏体可
 * 作为普通循环使用。hrtimer_from_timerqueue_node 从嵌入节点恢复只读兼容容器。
 */

#if defined(CONFIG_NO_HZ_COMMON)
/*
 * Same as hrtimer_bases_next_event() below, but skips the excluded timer and
 * does not update cpu_base->next_timer/expires.
 */
/*
 * hrtimer_bases_next_event_without() - 计算排除一个 timer 后的最早单调时间。
 *
 * @cpu_base 已锁；@exclude 借用且通常属于当前 CPU；@active 选择 base；
 * @expires_next 是已有上界。返回非负期限或 KTIME_MAX，不更新任何 next 缓存。
 */
static ktime_t hrtimer_bases_next_event_without(struct hrtimer_cpu_base *cpu_base,
						const struct hrtimer *exclude,
						unsigned int active, ktime_t expires_next)
{
	struct hrtimer_clock_base *base;
	ktime_t expires;

	lockdep_assert_held(&cpu_base->lock);

	for_each_active_base(base, cpu_base, active) {
		expires = ktime_sub(base->expires_next, base->offset);
		if (expires >= expires_next)
			continue;

		/*
		 * If the excluded timer is the first on this base evaluate the
		 * next timer.
		 */
		/* 只有 exclude 位于树最左端时才需查看后继；否则该 base 的最早值不变。 */
		struct timerqueue_linked_node *node = timerqueue_linked_first(&base->active);

		if (unlikely(&exclude->node == node)) {
			node = timerqueue_linked_next(node);
			if (!node)
				continue;
			expires = ktime_sub(node->expires, base->offset);
			if (expires >= expires_next)
				continue;
		}
		expires_next = expires;
	}
	/* If base->offset changed, the result might be negative */
	/* clock_settime 可移动 offset；对外期限钳到 0 表示立即到期。 */
	return max(expires_next, 0);
}
#endif

/* 返回某 clock_base 红黑树最左 timer；调用者保证树非空且持 cpu_base 锁。 */
static __always_inline struct hrtimer *clock_base_next_timer(struct hrtimer_clock_base *base)
{
	struct timerqueue_linked_node *next = timerqueue_linked_first(&base->active);

	return hrtimer_from_timerqueue_node(next);
}

/* Find the base with the earliest expiry */
/*
 * 中文契约：扫描 @active 指定的非空 base，把跨 clockid 转换后的最早单调期限和
 * timer 借用指针写入输出参数。调用者持 cpu_base 锁并提供初始上界。
 */
static void hrtimer_bases_first(struct hrtimer_cpu_base *cpu_base,unsigned int active,
				ktime_t *expires_next, struct hrtimer **next_timer)
{
	struct hrtimer_clock_base *base;
	ktime_t expires;

	for_each_active_base(base, cpu_base, active) {
		expires = ktime_sub(base->expires_next, base->offset);
		if (expires < *expires_next) {
			*expires_next = expires;
			*next_timer = clock_base_next_timer(base);
		}
	}
}

/*
 * Recomputes cpu_base::*next_timer and returns the earliest expires_next
 * but does not set cpu_base::*expires_next, that is done by
 * hrtimer[_force]_reprogram and hrtimer_interrupt only. When updating
 * cpu_base::*expires_next right away, reprogramming logic would no longer
 * work.
 *
 * When a softirq is pending, we can ignore the HRTIMER_ACTIVE_SOFT bases,
 * those timers will get run whenever the softirq gets handled, at the end of
 * hrtimer_run_softirq(), hrtimer_update_softirq_timer() will re-add these bases.
 *
 * Therefore softirq values are those from the HRTIMER_ACTIVE_SOFT clock bases.
 * The !softirq values are the minima across HRTIMER_ACTIVE_ALL, unless an actual
 * softirq is pending, in which case they're the minima of HRTIMER_ACTIVE_HARD.
 *
 * @active_mask must be one of:
 *  - HRTIMER_ACTIVE_ALL,
 *  - HRTIMER_ACTIVE_SOFT, or
 *  - HRTIMER_ACTIVE_HARD.
 */
/*
 * __hrtimer_get_next_event() - 重算 hard/soft 指针缓存但不提交硬件期限。
 *
 * @cpu_base 已锁；@active_mask 必须是 ALL/SOFT/HARD。返回非负最早单调期限。
 * softirq 已 raise 时忽略 soft 树，因为它会在底半部执行；故 next_timer 与
 * softirq_next_timer 的缓存更新必须与 softirq_activated 状态一致。
 */
static ktime_t __hrtimer_get_next_event(struct hrtimer_cpu_base *cpu_base, unsigned int active_mask)
{
	struct hrtimer *next_timer = NULL;
	ktime_t expires_next = KTIME_MAX;
	unsigned int active;

	lockdep_assert_held(&cpu_base->lock);

	if (!cpu_base->softirq_activated && (active_mask & HRTIMER_ACTIVE_SOFT)) {
		active = cpu_base->active_bases & HRTIMER_ACTIVE_SOFT;
		if (active)
			hrtimer_bases_first(cpu_base, active, &expires_next, &next_timer);
		cpu_base->softirq_next_timer = next_timer;
	}

	if (active_mask & HRTIMER_ACTIVE_HARD) {
		active = cpu_base->active_bases & HRTIMER_ACTIVE_HARD;
		if (active)
			hrtimer_bases_first(cpu_base, active, &expires_next, &next_timer);
		cpu_base->next_timer = next_timer;
	}
	return max(expires_next, 0);
}

/*
 * hrtimer_update_next_event() - 合并 hard 与尚未激活的 soft 最早事件。
 *
 * @cpu_base 已锁；返回应编程的最早期限，并刷新 next_timer/softirq 缓存，但
 * expires_next 只由真正重编程路径提交，避免比较逻辑提前失去旧硬件基准。
 */
static ktime_t hrtimer_update_next_event(struct hrtimer_cpu_base *cpu_base)
{
	ktime_t expires_next, soft = KTIME_MAX;

	/*
	 * If the soft interrupt has already been activated, ignore the
	 * soft bases. They will be handled in the already raised soft
	 * interrupt.
	 */
	/* 已 raise 的 softirq 会自行重算，当前硬件选择不能重复代管同批 soft timer。 */
	if (!cpu_base->softirq_activated) {
		soft = __hrtimer_get_next_event(cpu_base, HRTIMER_ACTIVE_SOFT);
		/*
		 * Update the soft expiry time. clock_settime() might have
		 * affected it.
		 */
		/* offset 改变会移动绝对时钟 timer，故即使树不变也必须刷新 soft 期限。 */
		cpu_base->softirq_expires_next = soft;
	}

	expires_next = __hrtimer_get_next_event(cpu_base, HRTIMER_ACTIVE_HARD);
	/*
	 * If a softirq timer is expiring first, update cpu_base->next_timer
	 * and program the hardware with the soft expiry time.
	 */
	/* 硬件仍需在最早 soft 期限唤醒 CPU，以便 raise HRTIMER_SOFTIRQ。 */
	if (expires_next > soft) {
		cpu_base->next_timer = cpu_base->softirq_next_timer;
		expires_next = soft;
	}

	return expires_next;
}

/*
 * hrtimer_update_base() - 采样当前 MONOTONIC 并刷新 REAL/BOOT/TAI 偏移。
 *
 * @base 已锁；返回当前单调纳秒。timekeeping helper 通过序列号提供一致快照，
 * hard/soft 同 clockid 必须复制同一 offset，保证两套树的时间坐标一致。
 */
static inline ktime_t hrtimer_update_base(struct hrtimer_cpu_base *base)
{
	ktime_t *offs_real = &base->clock_base[HRTIMER_BASE_REALTIME].offset;
	ktime_t *offs_boot = &base->clock_base[HRTIMER_BASE_BOOTTIME].offset;
	ktime_t *offs_tai = &base->clock_base[HRTIMER_BASE_TAI].offset;

	ktime_t now = ktime_get_update_offsets_now(&base->clock_was_set_seq, offs_real,
						   offs_boot, offs_tai);

	base->clock_base[HRTIMER_BASE_REALTIME_SOFT].offset = *offs_real;
	base->clock_base[HRTIMER_BASE_BOOTTIME_SOFT].offset = *offs_boot;
	base->clock_base[HRTIMER_BASE_TAI_SOFT].offset = *offs_tai;

	return now;
}

/*
 * Is the high resolution mode active in the CPU base. This cannot use the
 * static key as the CPUs are switched to high resolution mode
 * asynchronously.
 */
/*
 * 中文说明：每 CPU 异步切换 highres，故全局 static key 不能证明某个 CPU 已完成
 * clockevent setup。返回该 cpu_base 的真实状态；无配置时恒 0。
 */
static inline int hrtimer_hres_active(struct hrtimer_cpu_base *cpu_base)
{
	return IS_ENABLED(CONFIG_HIGH_RES_TIMERS) ?
		cpu_base->hres_active : 0;
}

/*
 * hrtimer_rearm_event() - 记录并向本 CPU clockevent 编程绝对期限。
 *
 * @expires_next 为单调纳秒，@deferred 标识是否从延迟 rearm 路径提交。无返回值；
 * tick_program_event(force=1) 负责处理已过期/设备最小 delta。
 */
static inline void hrtimer_rearm_event(ktime_t expires_next, bool deferred)
{
	trace_hrtimer_rearm(expires_next, deferred);
	tick_program_event(expires_next, 1);
}

/*
 * __hrtimer_reprogram() - 提交 cpu_base 缓存并按需重编程硬件。
 *
 * @cpu_base 已锁且属于当前 CPU；@next_timer 是对应借用 timer；@expires_next 为
 * 单调期限。低分辨率或 hang 抑制状态只更新缓存，不碰硬件。
 */
static void __hrtimer_reprogram(struct hrtimer_cpu_base *cpu_base, struct hrtimer *next_timer,
				ktime_t expires_next)
{
	cpu_base->expires_next = expires_next;

	/*
	 * If hres is not active, hardware does not have to be
	 * reprogrammed yet.
	 *
	 * If a hang was detected in the last timer interrupt then we
	 * leave the hang delay active in the hardware. We want the
	 * system to make progress. That also prevents the following
	 * scenario:
	 * T1 expires 50ms from now
	 * T2 expires 5s from now
	 *
	 * T1 is removed, so this code is called and would reprogram
	 * the hardware to 5s from now. Any hrtimer_start after that
	 * will not reprogram the hardware due to hang_detected being
	 * set. So we'd effectively block all timers until the T2 event
	 * fires.
	 */
	/*
	 * hang_detected 时保留短暂的保护性硬件事件。若此处因删除 T1 直接改到很远的
	 * T2，而后续 start 又因 hang 标志拒绝重编程，所有新 timer 都会被阻塞到 T2；
	 * 因此只更新软件缓存，让保护事件先推动系统恢复。
	 */
	if (!hrtimer_hres_active(cpu_base) || cpu_base->hang_detected)
		return;

	hrtimer_rearm_event(expires_next, false);
}

/* Reprogram the event source with a evaluation of all clock bases */
/*
 * hrtimer_force_reprogram() - 重扫全部 base 后强制同步硬件。
 *
 * @cpu_base 已锁；@skip_equal=true 时新旧期限相同可省略设备写。无返回值。
 */
static void hrtimer_force_reprogram(struct hrtimer_cpu_base *cpu_base, bool skip_equal)
{
	ktime_t expires_next = hrtimer_update_next_event(cpu_base);

	if (skip_equal && expires_next == cpu_base->expires_next)
		return;

	__hrtimer_reprogram(cpu_base, cpu_base->next_timer, expires_next);
}

/* High resolution timer related functions */
/* 以下代码只在 HIGH_RES_TIMERS 配置下建立每 CPU oneshot clockevent 模式。 */
#ifdef CONFIG_HIGH_RES_TIMERS

/* High resolution timer enabled ? */
/* 启动参数控制策略值；resolution 在成功切换前仍是低分辨率 tick 粒度。 */
static bool hrtimer_hres_enabled __read_mostly  = true;
unsigned int hrtimer_resolution __read_mostly = LOW_RES_NSEC;
EXPORT_SYMBOL_GPL(hrtimer_resolution);

/* Enable / Disable high resolution mode */
/* setup_hrtimer_hres() 解析 highres= 布尔值；init 阶段调用，成功解析返回 1。 */
static int __init setup_hrtimer_hres(char *str)
{
	return (kstrtobool(str, &hrtimer_hres_enabled) == 0);
}
__setup("highres=", setup_hrtimer_hres);

/* hrtimer_high_res_enabled - query, if the highres mode is enabled */
/* 返回启动策略，不代表当前 CPU 已异步完成 highres 切换。 */
static inline bool hrtimer_is_hres_enabled(void)
{
	return hrtimer_hres_enabled;
}

/* Switch to high resolution mode */
/*
 * hrtimer_switch_to_hres() - 把当前 CPU 切换到 oneshot 高分辨率事件模式。
 *
 * 无入参/直接返回值；失败仅告警并保留 lowres。成功后发布 per-CPU hres_active、
 * 全局 1ns resolution，建立 sched tick，重算首事件并异步启用 static key。
 */
static void hrtimer_switch_to_hres(void)
{
	struct hrtimer_cpu_base *base = this_cpu_ptr(&hrtimer_bases);

	if (tick_init_highres()) {
		pr_warn("Could not switch to high resolution mode on CPU %u\n",	base->cpu);
		return;
	}
	base->hres_active = true;
	hrtimer_resolution = HIGH_RES_NSEC;

	tick_setup_sched_timer(true);
	/* "Retrigger" the interrupt to get things going */
	/* 新模式尚未编程首事件，立即重算才能避免已有 timer 丢失唤醒。 */
	retrigger_next_event(NULL);
	hrtimer_schedule_hres_work();
}

#else

/* 无 highres 配置时策略恒关，切换入口为空，调用点由编译器消除。 */
static inline bool hrtimer_is_hres_enabled(void) { return 0; }
static inline void hrtimer_switch_to_hres(void) { }

#endif /* CONFIG_HIGH_RES_TIMERS */

/*
 * Retrigger next event is called after clock was set with interrupts
 * disabled through an SMP function call or directly from low level
 * resume code.
 *
 * This is only invoked when:
 *	- CONFIG_HIGH_RES_TIMERS is enabled.
 *	- CONFIG_NO_HZ_COMMON is enabled
 *
 * For the other cases this function is empty and because the call sites
 * are optimized out it vanishes as well, i.e. no need for lots of
 * #ifdeffery.
 */
/*
 * retrigger_next_event() - 时钟偏移变化后在目标 CPU 重算下一事件。
 *
 * @arg 未使用；调用时 IRQ 关闭，可能来自 SMP call 或 resume。scope guard 自动
 * 获取/释放当前 cpu_base raw lock。highres 模式同步重编程；NO_HZ lowres 只更新
 * 缓存，由 SMP call 返回/idle 路径处理硬件；普通周期 tick 可自然刷新。
 */
static void retrigger_next_event(void *arg)
{
	struct hrtimer_cpu_base *base = this_cpu_ptr(&hrtimer_bases);

	/*
	 * When high resolution mode or nohz is active, then the offsets of
	 * CLOCK_REALTIME/TAI/BOOTTIME have to be updated. Otherwise the
	 * next tick will take care of that.
	 *
	 * If high resolution mode is active then the next expiring timer
	 * must be reevaluated and the clock event device reprogrammed if
	 * necessary.
	 *
	 * In the NOHZ case the update of the offset and the reevaluation
	 * of the next expiring timer is enough. The return from the SMP
	 * function call will take care of the reprogramming in case the
	 * CPU was in a NOHZ idle sleep.
	 *
	 * In periodic low resolution mode, the next softirq expiration
	 * must also be updated.
	 */
	/*
	 * 中文说明：REALTIME/TAI/BOOTTIME 与 MONOTONIC 的 offset 可能因 settime 或
	 * suspend 改变。必须先刷新 offset，再比较树的绝对期限；否则会按旧坐标编程。
	 */
	guard(raw_spinlock)(&base->lock);
	hrtimer_update_base(base);
	if (hrtimer_hres_active(base))
		hrtimer_force_reprogram(base, /* skip_equal */ false);
	else
		hrtimer_update_next_event(base);
}

/*
 * When a timer is enqueued and expires earlier than the already enqueued
 * timers, we have to check, whether it expires earlier than the timer for
 * which the clock event device was armed.
 *
 * Called with interrupts disabled and base->cpu_base.lock held
 */
/*
 * hrtimer_reprogram() - 新 timer 成为更早候选时更新 soft/hard 缓存和本地硬件。
 *
 * @timer 已入队且 base 锁/IRQ 均由调用者控制；@reprogram 决定是否立即碰设备。
 * 远端硬件不能直接重编程，故远端只维护 soft 缓存并依靠目标 CPU 的事件/IPI。
 */
static void hrtimer_reprogram(struct hrtimer *timer, bool reprogram)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	struct hrtimer_clock_base *base = timer->base;
	ktime_t expires = hrtimer_get_expires(timer);

	WARN_ON_ONCE(expires < 0);

	expires = ktime_sub(expires, base->offset);
	/*
	 * CLOCK_REALTIME timer might be requested with an absolute
	 * expiry time which is less than base->offset. Set it to 0.
	 */
	/* 绝对 REALTIME 早于其 offset 时换算会为负；0 表示立即到期而非环绕未来。 */
	if (expires < 0)
		expires = 0;

	if (timer->is_soft) {
		/*
		 * soft hrtimer could be started on a remote CPU. In this
		 * case softirq_expires_next needs to be updated on the
		 * remote CPU. The soft hrtimer will not expire before the
		 * first hard hrtimer on the remote CPU -
		 * hrtimer_check_target() prevents this case.
		 */
	/*
	 * 远端 soft timer 只有在不早于该 CPU 首个 hard 事件时才允许入队（目标选择
	 * 已保证），所以无需立即 IPI；这里只刷新 softirq 的最早指针/期限。
	 */
		struct hrtimer_cpu_base *timer_cpu_base = base->cpu_base;

		if (timer_cpu_base->softirq_activated)
			return;

		if (!ktime_before(expires, timer_cpu_base->softirq_expires_next))
			return;

		timer_cpu_base->softirq_next_timer = timer;
		timer_cpu_base->softirq_expires_next = expires;

		if (!ktime_before(expires, timer_cpu_base->expires_next) || !reprogram)
			return;
	}

	/*
	 * If the timer is not on the current cpu, we cannot reprogram
	 * the other cpus clock event device.
	 */
	/* per-CPU clockevent 只能由所属 CPU 编程；跨 CPU 写设备寄存器没有正确语义。 */
	if (base->cpu_base != cpu_base)
		return;

	if (expires >= cpu_base->expires_next)
		return;

	/* If a deferred rearm is pending skip reprogramming the device */
	/* IRQ exit 延迟提交会统一使用最新缓存，当前再次写硬件既昂贵又可能被覆盖。 */
	if (cpu_base->deferred_rearm)
		return;

	cpu_base->next_timer = timer;

	__hrtimer_reprogram(cpu_base, timer, expires);
}

/*
 * update_needs_ipi() - clock_was_set 时判断远端 CPU 是否必须立即响应。
 *
 * @cpu_base 已锁；@active 是受 offset 变化影响的 base 位图。返回 true 表示某
 * timer 被提前到现有硬件事件之前，需要 IPI；函数总会刷新 offset 和相关标志。
 */
static bool update_needs_ipi(struct hrtimer_cpu_base *cpu_base, unsigned int active)
{
	struct hrtimer_clock_base *base;
	unsigned int seq;
	ktime_t expires;

	/*
	 * Update the base offsets unconditionally so the following
	 * checks whether the SMP function call is required works.
	 *
	 * The update is safe even when the remote CPU is in the hrtimer
	 * interrupt or the hrtimer soft interrupt and expiring affected
	 * bases. Either it will see the update before handling a base or
	 * it will see it when it finishes the processing and reevaluates
	 * the next expiring timer.
	 */
	/*
	 * 远端可能正在 hard/soft 到期循环；两者都在同一锁下，于是要么本次先刷新，
	 * 要么远端处理结束后看到新序列并重算，不会使用半更新 offset。
	 */
	seq = cpu_base->clock_was_set_seq;
	hrtimer_update_base(cpu_base);

	/*
	 * If the sequence did not change over the update then the
	 * remote CPU already handled it.
	 */
	/* 序列未变说明该 CPU 已处理本次 clock set，无需重复中断。 */
	if (seq == cpu_base->clock_was_set_seq)
		return false;

	/* If a deferred rearm is pending the remote CPU will take care of it */
	/* 延迟提交路径会在退出前重算；只标记缓存失效即可。 */
	if (cpu_base->deferred_rearm) {
		cpu_base->deferred_needs_update = true;
		return false;
	}

	/*
	 * Walk the affected clock bases and check whether the first expiring
	 * timer in a clock base is moving ahead of the first expiring timer of
	 * @cpu_base. If so, the IPI must be invoked because per CPU clock
	 * event devices cannot be remotely reprogrammed.
	 */
	/*
	 * 仅树最左 timer 能改变 CPU 首事件；若新换算期限早于 expires_next 或未激活
	 * softirq 的 soft 最早期限，目标 CPU 必须被唤醒重编程。
	 */
	active &= cpu_base->active_bases;

	for_each_active_base(base, cpu_base, active) {
		struct timerqueue_linked_node *next;

		next = timerqueue_linked_first(&base->active);
		expires = ktime_sub(next->expires, base->offset);
		if (expires < cpu_base->expires_next)
			return true;

		/* Extra check for softirq clock bases */
		/* hard base 已由通用 expires_next 覆盖；soft base 还要维护独立缓存。 */
		if (base->index < HRTIMER_BASE_MONOTONIC_SOFT)
			continue;
		if (cpu_base->softirq_activated)
			continue;
		if (expires < cpu_base->softirq_expires_next)
			return true;
	}
	return false;
}

/*
 * Clock was set. This might affect CLOCK_REALTIME, CLOCK_TAI and
 * CLOCK_BOOTTIME (for late sleep time injection).
 *
 * This requires to update the offsets for these clocks
 * vs. CLOCK_MONOTONIC. When high resolution timers are enabled, then this
 * also requires to eventually reprogram the per CPU clock event devices
 * when the change moves an affected timer ahead of the first expiring
 * timer on that CPU. Obviously remote per CPU clock event devices cannot
 * be reprogrammed. The other reason why an IPI has to be sent is when the
 * system is in !HIGH_RES and NOHZ mode. The NOHZ mode updates the offsets
 * in the tick, which obviously might be stopped, so this has to bring out
 * the remote CPU which might sleep in idle to get this sorted.
 */
/*
 * clock_was_set() - 在 wall/TAI/boottime 坐标变化后更新所有 CPU hrtimer。
 *
 * @bases 是受影响 base 位图；可睡眠并可能分配 cpumask。先在锁内筛出真正需 IPI
 * 的 CPU，再批量同步调用 retrigger，避免无差别打断所有核。分配失败退化为
 * on_each_cpu，正确性优先。无论 hrtimer 模式如何，最终都通知 timerfd。
 */
void clock_was_set(unsigned int bases)
{
	cpumask_var_t mask;

	if (!hrtimer_highres_enabled() && !tick_nohz_is_active())
		goto out_timerfd;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL)) {
		on_each_cpu(retrigger_next_event, NULL, 1);
		goto out_timerfd;
	}

	/* Avoid interrupting CPUs if possible */
	/* cpus_read_lock 稳定 online 集合；逐 CPU 锁保护 offset、树首节点和缓存。 */
	scoped_guard(cpus_read_lock) {
		int cpu;

		for_each_online_cpu(cpu) {
			struct hrtimer_cpu_base *cpu_base = &per_cpu(hrtimer_bases, cpu);

			guard(raw_spinlock_irqsave)(&cpu_base->lock);
			if (update_needs_ipi(cpu_base, bases))
				cpumask_set_cpu(cpu, mask);
		}
		scoped_guard(preempt)
			smp_call_function_many(mask, retrigger_next_event, NULL, 1);
	}
	free_cpumask_var(mask);

out_timerfd:
	timerfd_clock_was_set();
}

/* workqueue 包装：在可睡眠上下文处理 CLOCK_SET_WALL 的延迟通知。 */
static void clock_was_set_work(struct work_struct *work)
{
	clock_was_set(CLOCK_SET_WALL);
}

static DECLARE_WORK(hrtimer_work, clock_was_set_work);

/*
 * Called from timekeeping code to reprogram the hrtimer interrupt device
 * on all cpus and to notify timerfd.
 */
/*
 * 中文契约：timekeeping 敏感路径只调度静态 work，避免当场分配/跨 CPU 等待；
 * 重复请求可合并，最终 clock_was_set() 使用最新 timekeeping 序列收敛。
 */
void clock_was_set_delayed(void)
{
	schedule_work(&hrtimer_work);
}

/*
 * Called during resume either directly from via timekeeping_resume()
 * or in the case of s2idle from tick_unfreeze() to ensure that the
 * hrtimers are up to date.
 */
/*
 * 中文契约：resume 时在本 CPU、IRQ 关闭条件下刷新 offset 与首事件。无返回值；
 * suspend 注入的 sleep time 可能使 BOOTTIME timer 立即到期。
 */
void hrtimers_resume_local(void)
{
	lockdep_assert_irqs_disabled();
	/* Retrigger on the local CPU */
	/* 只刷新当前 CPU；resume/hotplug 外层负责逐 CPU 调用或恢复顺序。 */
	retrigger_next_event(NULL);
}

/* Counterpart to lock_hrtimer_base above */
/* 释放 timer 当前 cpu_base 锁并恢复 @flags；base 在持锁协议内不会迁移。 */
static inline void unlock_hrtimer_base(const struct hrtimer *timer, unsigned long *flags)
	__releases(&timer->base->cpu_base->lock)
{
	raw_spin_unlock_irqrestore(&timer->base->cpu_base->lock, *flags);
}

/**
 * hrtimer_forward() - forward the timer expiry
 * @timer:	hrtimer to forward
 * @now:	forward past this time
 * @interval:	the interval to forward
 *
 * Forward the timer expiry so it will expire in the future.
 *
 * .. note::
 *  This only updates the timer expiry value and does not requeue the timer.
 *
 * There is also a variant of this function: hrtimer_forward_now().
 *
 * Context: Can be safely called from the callback function of @timer. If called
 *          from other contexts @timer must neither be enqueued nor running the
 *          callback and the caller needs to take care of serialization.
 *
 * Return: The number of overruns are returned.
 */
/*
 * 中文契约：把 @timer 的 expires 按正 @interval 推进到严格晚于 @now，并返回
 * 跨过的周期数；只改期限、不入队。callback 内可安全调用；其他上下文要求 timer
 * 既不 queued 也不 running 并自行串行化。过小 interval 钳到系统 resolution。
 */
u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval)
{
	ktime_t delta;
	u64 orun = 1;

	delta = ktime_sub(now, hrtimer_get_expires(timer));

	if (delta < 0)
		return 0;

	if (WARN_ON(timer->is_queued))
		return 0;

	if (interval < hrtimer_resolution)
		interval = hrtimer_resolution;

	if (unlikely(delta >= interval)) {
		s64 incr = ktime_to_ns(interval);

		orun = ktime_divns(delta, incr);
		hrtimer_add_expires_ns(timer, incr * orun);
		if (hrtimer_get_expires(timer) > now)
			return orun;
		/*
		 * This (and the ktime_add() below) is the
		 * correction for exact:
		 */
		/* 整除恰好落在 now 时还要多推进一周期，兑现“严格位于未来”。 */
		orun++;
	}
	hrtimer_add_expires(timer, interval);

	return orun;
}
EXPORT_SYMBOL_GPL(hrtimer_forward);

/*
 * enqueue_hrtimer - internal function to (re)start a timer
 *
 * The timer is inserted in expiry order. Insertion into the
 * red black tree is O(log(n)).
 *
 * Returns true when the new timer is the leftmost timer in the tree.
 */
/*
 * enqueue_hrtimer() - 在锁内把 timer 按 hard expires 插入有序 timerqueue。
 *
 * @timer 已绑定 @base；@mode 供 debug/trace；@was_armed 描述入口状态。返回 true
 * 表示成为树最左节点。先发布 active_bases/is_queued，再插树；WRITE_ONCE 与
 * hrtimer_is_queued() 无锁读配对，完整树不变量仍由 cpu_base->lock 保护。
 */
static bool enqueue_hrtimer(struct hrtimer *timer, struct hrtimer_clock_base *base,
			    enum hrtimer_mode mode, bool was_armed)
{
	lockdep_assert_held(&base->cpu_base->lock);

	debug_activate(timer, mode, was_armed);
	WARN_ON_ONCE(!base->cpu_base->online);

	base->cpu_base->active_bases |= 1 << base->index;

	/* Pairs with the lockless read in hrtimer_is_queued() */
	/* 只保证 queued 位无撕裂/不被编译器合并，不替代锁或 seqcount 快照。 */
	WRITE_ONCE(timer->is_queued, HRTIMER_STATE_ENQUEUED);

	if (!timerqueue_linked_add(&base->active, &timer->node))
		return false;

	base->expires_next = hrtimer_get_expires(timer);
	return true;
}

/* 重读树最左节点刷新 base->expires_next；空树写 KTIME_MAX 哨兵。 */
static inline void base_update_next_timer(struct hrtimer_clock_base *base)
{
	struct timerqueue_linked_node *next = timerqueue_linked_first(&base->active);

	base->expires_next = next ? next->expires : KTIME_MAX;
}

/*
 * __remove_hrtimer - internal function to remove a timer
 *
 * High resolution timer mode reprograms the clock event device when the
 * timer is the one which expires next. The caller can disable this by setting
 * reprogram to zero. This is useful, when the context does a reprogramming
 * anyway (e.g. timer interrupt)
 */
/*
 * __remove_hrtimer() - 从一颗已锁 timerqueue 摘除 timer 并维护所有首事件缓存。
 *
 * @newstate 决定摘除后的 queued 可见状态（迁移时仍标 ENQUEUED）；@reprogram
 * 决定是否可立即重编程本地硬件。非 queued 为空操作。只有删除树最左节点才需
 * 更新 expires_next；lazy timer 的硬件延后策略不在这里强制改写。
 */
static void __remove_hrtimer(struct hrtimer *timer, struct hrtimer_clock_base *base,
			     bool newstate, bool reprogram)
{
	struct hrtimer_cpu_base *cpu_base = base->cpu_base;
	bool was_first;

	lockdep_assert_held(&cpu_base->lock);

	if (!timer->is_queued)
		return;

	/* Pairs with the lockless read in hrtimer_is_queued() */
	/* 迁移者可保持 ENQUEUED，防止锁外观察者误判 inactive 后释放对象。 */
	WRITE_ONCE(timer->is_queued, newstate);

	was_first = !timerqueue_linked_prev(&timer->node);

	if (!timerqueue_linked_del(&base->active, &timer->node))
		cpu_base->active_bases &= ~(1 << base->index);

	/* Nothing to update if this was not the first timer in the base */
	/* 非最左节点不影响该 base 或 CPU 已编程的最早期限。 */
	if (!was_first)
		return;

	base_update_next_timer(base);

	/*
	 * If reprogram is false don't update cpu_base->next_timer and do not
	 * touch the clock event device.
	 *
	 * This happens when removing the first timer on a remote CPU, which
	 * will be handled by the remote CPU's interrupt. It also happens when
	 * a local timer is removed to be immediately restarted. That's handled
	 * at the call site.
	 */
	/*
	 * 远端删除由目标 CPU 已有中断最终重算；本地“摘除后立即重启”由调用点统一
	 * 编程。这里跳过可避免一次无意义的硬件写和跨 CPU IPI。
	 */
	if (!reprogram || timer != cpu_base->next_timer || timer->is_lazy)
		return;

	if (cpu_base->deferred_rearm)
		cpu_base->deferred_needs_update = true;
	else
		hrtimer_force_reprogram(cpu_base, /* skip_equal */ true);
}

/*
 * remove_hrtimer() - 公开内部删除包装，选择是否能本地重编程并更新 debug 状态。
 * 返回 true 表示确实 queued 并摘除，false 表示 inactive；调用者持 base 锁。
 */
static inline bool remove_hrtimer(struct hrtimer *timer, struct hrtimer_clock_base *base,
				  bool newstate)
{
	lockdep_assert_held(&base->cpu_base->lock);

	if (timer->is_queued) {
		bool reprogram;

		debug_hrtimer_deactivate(timer);

		/*
		 * Remove the timer and force reprogramming when high
		 * resolution mode is active and the timer is on the current
		 * CPU. If we remove a timer on another CPU, reprogramming is
		 * skipped. The interrupt event on this CPU is fired and
		 * reprogramming happens in the interrupt handler. This is a
		 * rare case and less expensive than a smp call.
		 */
		/*
		 * 远端删除不发昂贵 SMP call：目标 CPU 的已编程事件即使多触发一次，也会
		 * 在 interrupt 中发现树的新首节点并修正，正确性不受影响。
		 */
		reprogram = base->cpu_base == this_cpu_ptr(&hrtimer_bases);

		__remove_hrtimer(timer, base, newstate, reprogram);
		return true;
	}
	return false;
}

/*
 * Update in place has to retrieve the expiry times of the neighbour nodes
 * if they exist. That is cache line neutral because the dequeue/enqueue
 * operation is going to need the same cache lines. But there is a big win
 * when the dequeue/enqueue can be avoided because the RB tree does not
 * have to be rebalanced twice.
 */
/*
 * hrtimer_can_update_in_place() - 判断修改 expires 后红黑树顺序是否仍成立。
 *
 * @timer 当前 queued 于 @base；@expires 是新 hard expiry。只读相邻节点，返回
 * 布尔值。若仍位于 prev/next 之间可原位更新，省去两次 RB rebalance。
 */
static inline bool
hrtimer_can_update_in_place(struct hrtimer *timer, struct hrtimer_clock_base *base, ktime_t expires)
{
	struct timerqueue_linked_node *next = timerqueue_linked_next(&timer->node);
	struct timerqueue_linked_node *prev = timerqueue_linked_prev(&timer->node);

	/* If the new expiry goes behind the next timer, requeue is required */
	/* 晚于后继会破坏升序，必须摘除重插。 */
	if (next && expires > next->expires)
		return false;

	/* If this is the first timer, update in place */
	/* 无前驱时只要没越过后继，仍是合法最左节点。 */
	if (!prev)
		return true;

	/* Update in place when it does not go ahead of the previous one */
	/* 不早于前驱且不晚于后继即保持有序。 */
	return expires >= prev->expires;
}

/*
 * remove_and_enqueue_same_base() - 在同一已锁 base 上高效重设 timer。
 *
 * 若相邻顺序允许则原位改期限；否则摘树、更新 expires/slack 后重新插入。返回
 * true 表示最终成为最左 timer。整个过程不改变 base ownership，也不留可见空窗。
 */
static inline bool
remove_and_enqueue_same_base(struct hrtimer *timer, struct hrtimer_clock_base *base,
			     const enum hrtimer_mode mode, ktime_t expires, u64 delta_ns)
{
	bool was_first = false;

	/* Remove it from the timer queue if active */
	/* 记录旧最左身份，供重插后决定是否刷新 base 缓存。 */
	if (timer->is_queued) {
		was_first = !timerqueue_linked_prev(&timer->node);

		/* Try to update in place to avoid the de/enqueue dance */
		/* 原位路径仍更新 trace；若最左还需同步 base->expires_next。 */
		if (hrtimer_can_update_in_place(timer, base, expires)) {
			hrtimer_set_expires_range_ns(timer, expires, delta_ns);
			trace_hrtimer_start(timer, mode, true);
			if (was_first)
				base->expires_next = expires;
			return was_first;
		}

		debug_hrtimer_deactivate(timer);
		timerqueue_linked_del(&base->active, &timer->node);
	}

	/* Set the new expiry time */
	/* expires 与 slack 必须在重新发布 queued/树节点之前完整写好。 */
	hrtimer_set_expires_range_ns(timer, expires, delta_ns);

	debug_activate(timer, mode, timer->is_queued);
	base->cpu_base->active_bases |= 1 << base->index;

	/* Pairs with the lockless read in hrtimer_is_queued() */
	/* 发布 ENQUEUED 后树插入仍在锁保护内，锁外 active() 用 seqcount 排除中间态。 */
	WRITE_ONCE(timer->is_queued, HRTIMER_STATE_ENQUEUED);

	/* If it's the first expiring timer now or again, update base */
	/* 插入返回“新最左”；否则旧最左被移动时从树重新读取缓存。 */
	if (timerqueue_linked_add(&base->active, &timer->node)) {
		base->expires_next = expires;
		return true;
	}

	if (was_first)
		base_update_next_timer(base);

	return false;
}

/*
 * hrtimer_update_lowres() - 低分辨率系统补偿相对 timer 的一个 tick 粒度。
 *
 * @tim 是相对或绝对输入期限，@mode 判定语义；返回调整值并记录 timer->is_rel。
 * 绝对 timer 不补偿，因为其目标坐标已经明确。
 */
static inline ktime_t hrtimer_update_lowres(struct hrtimer *timer, ktime_t tim,
					    const enum hrtimer_mode mode)
{
#ifdef CONFIG_TIME_LOW_RES
	/*
	 * CONFIG_TIME_LOW_RES indicates that the system has no way to return
	 * granular time values. For relative timers we add hrtimer_resolution
	 * (i.e. one jiffy) to prevent short timeouts.
	 */
	/* 相对短延迟若不加一 jiffy，粗采样误差可能让它看起来已经到期而提前执行。 */
	timer->is_rel = mode & HRTIMER_MODE_REL;
	if (timer->is_rel)
		tim = ktime_add_safe(tim, hrtimer_resolution);
#endif
	return tim;
}

/*
 * hrtimer_update_softirq_timer() - softirq 执行完后重建下一 soft 事件并按需编程。
 * @cpu_base 已锁；@reprogram 控制设备写。无 soft timer 时保持 KTIME_MAX。
 */
static void hrtimer_update_softirq_timer(struct hrtimer_cpu_base *cpu_base, bool reprogram)
{
	ktime_t expires = __hrtimer_get_next_event(cpu_base, HRTIMER_ACTIVE_SOFT);

	/*
	 * Reprogramming needs to be triggered, even if the next soft
	 * hrtimer expires at the same time as the next hard
	 * hrtimer. cpu_base->softirq_expires_next needs to be updated!
	 */
	/* hard/soft 同时到期也必须走 reprogram，以刷新独立 softirq_expires_next。 */
	if (expires == KTIME_MAX)
		return;

	/*
	 * cpu_base->next_timer is recomputed by __hrtimer_get_next_event()
	 * cpu_base->expires_next is only set by hrtimer_reprogram()
	 */
	/* 指针已重算；真正 expires_next 仍由 hrtimer_reprogram() 的提交协议维护。 */
	hrtimer_reprogram(cpu_base->softirq_next_timer, reprogram);
}

#if defined(CONFIG_SMP) && defined(CONFIG_NO_HZ_COMMON)
/*
 * hrtimer_prefer_local() - 在 NO_HZ 省电迁移与本地低延迟之间做策略选择。
 *
 * 输入三个布尔值分别描述当前归属、是否 CPU 首事件、是否 pinned。返回 true
 * 表示保持本 CPU。首事件/固定 timer 留本地可直接编程；isolated/nohz_full CPU
 * 在允许时把非关键 timer 移给 housekeeping CPU。
 */
static __always_inline bool hrtimer_prefer_local(bool is_local, bool is_first, bool is_pinned)
{
	if (static_branch_likely(&timers_migration_enabled)) {
		/*
		 * If it is local and the first expiring timer keep it on the local
		 * CPU to optimize reprogramming of the clockevent device. Also
		 * avoid switch_hrtimer_base() overhead when local and pinned.
		 */
		/* 首事件迁移反而需要远端重编程；pinned 则语义上禁止迁移。 */
		if (!is_local)
			return false;
		if (is_first || is_pinned)
			return true;

		/* Honour the NOHZ full restrictions */
		/* 非 housekeeping CPU 应尽量避免非必要 kernel noise。 */
		if (!housekeeping_cpu(smp_processor_id(), HK_TYPE_KERNEL_NOISE))
			return false;

		/*
		 * If the tick is not stopped or need_resched() is set, then
		 * there is no point in moving the timer somewhere else.
		 */
		/* CPU 本就会保持活动/即将调度时，迁移不会减少一次唤醒。 */
		return !tick_nohz_tick_stopped() || need_resched();
	}
	return is_local;
}
#else
/* 无 SMP+NO_HZ 迁移能力时，只能保留本地归属。 */
static __always_inline bool hrtimer_prefer_local(bool is_local, bool is_first, bool is_pinned)
{
	return is_local;
}
#endif

/*
 * hrtimer_keep_base() - running timer 强制保留原 base，否则交给本地策略。
 * running 检查由 base 锁稳定，返回值只决定本次重启是否换锁/迁移。
 */
static inline bool hrtimer_keep_base(struct hrtimer *timer, bool is_local, bool is_first,
				     bool is_pinned)
{
	/* If the timer is running the callback it has to stay on its CPU base. */
	/* callback 结束前 running 是同步取消与生命周期判断的唯一稳定锚点。 */
	if (unlikely(timer->base->running == timer))
		return true;

	return hrtimer_prefer_local(is_local, is_first, is_pinned);
}

enum {
	/* 启动核心向 wrapper 返回的硬件动作：不编程、按 timer 编程、重扫后强制编程。 */
	HRTIMER_REPROGRAM_NONE,
	HRTIMER_REPROGRAM,
	HRTIMER_REPROGRAM_FORCE,
};

/*
 * __hrtimer_start_range_ns() - 已锁条件下完成启动/重启的全部状态转换。
 *
 * @timer 为调用者拥有且已初始化对象；@tim 是 abs 或 rel 纳秒；@delta_ns 是
 * slack；@mode 指定 ABS/REL/PINNED；@base 已锁。返回 HRTIMER_REPROGRAM_*，
 * 由外层在仍持锁时决定硬件动作。
 *
 * 阶段：选择是否保留 base -> 把相对期限转换为绝对并补偿 lowres -> 同 base
 * 原位/重排或以 migration_base 跨 CPU 换锁 -> 发布新首节点 -> 给出编程决策。
 */
static int __hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim, u64 delta_ns,
				    const enum hrtimer_mode mode, struct hrtimer_clock_base *base)
{
	struct hrtimer_cpu_base *this_cpu_base = this_cpu_ptr(&hrtimer_bases);
	bool is_pinned, first, was_first, keep_base = false;
	struct hrtimer_cpu_base *cpu_base = base->cpu_base;

	was_first = cpu_base->next_timer == timer;
	is_pinned = !!(mode & HRTIMER_MODE_PINNED);

	/*
	 * Don't keep it local if this enqueue happens on a unplugged CPU
	 * after hrtimer_cpu_dying() has been invoked.
	 */
	/* CPU dying 后不能再把新 timer 留在将被清空的 base，必须走远端目标选择。 */
	if (likely(this_cpu_base->online)) {
		bool is_local = cpu_base == this_cpu_base;

		keep_base = hrtimer_keep_base(timer, is_local, was_first, is_pinned);
	}

	/* Calculate absolute expiry time for relative timers */
	/* 使用 timer clockid 的当前坐标把相对 delta 固化为绝对 deadline。 */
	if (mode & HRTIMER_MODE_REL)
		tim = ktime_add_safe(tim, __hrtimer_cb_get_time(base->clockid));
	/* Compensate for low resolution granularity */
	/* lowres 相对 timer 加一 resolution，避免采样截断导致过早触发。 */
	tim = hrtimer_update_lowres(timer, tim, mode);

	/*
	 * Remove an active timer from the queue. In case it is not queued
	 * on the current CPU, make sure that remove_hrtimer() updates the
	 * remote data correctly.
	 *
	 * If it's on the current CPU and the first expiring timer, then
	 * skip reprogramming, keep the timer local and enforce
	 * reprogramming later if it was the first expiring timer.  This
	 * avoids programming the underlying clock event twice (once at
	 * removal and once after enqueue).
	 *
	 * @keep_base is also true if the timer callback is running on a
	 * remote CPU and for local pinned timers.
	 */
	/*
	 * 中文说明：同 CPU 首 timer 摘除后立刻重插时不应编程两次；保持 base 并在
	 * 最后强制重扫。远端 running/pinned 也必须保留 base。其余 timer 可迁到
	 * NO_HZ 目标，迁移期间 queued 状态保持可见，防止对象被误释放。
	 */
	if (likely(keep_base)) {
		first = remove_and_enqueue_same_base(timer, base, mode, tim, delta_ns);
	} else {
		/* Keep the ENQUEUED state in case it is queued */
		/* 跨 base 中间态仍标 active，配合 migration_base 封住取消/释放竞态。 */
		bool was_armed = remove_hrtimer(timer, base, HRTIMER_STATE_ENQUEUED);

		hrtimer_set_expires_range_ns(timer, tim, delta_ns);

		/* Switch the timer base, if necessary: */
		/* 返回时旧锁已换成目标锁，timer->base 与实际锁重新一致。 */
		base = switch_hrtimer_base(timer, base, is_pinned);
		cpu_base = base->cpu_base;

		first = enqueue_hrtimer(timer, base, mode, was_armed);
	}

	/* If a deferred rearm is pending skip reprogramming the device */
	/* IRQ 尾部会统一重算；仅标记缓存失效，避免当前路径写入即将被覆盖的事件。 */
	if (cpu_base->deferred_rearm) {
		cpu_base->deferred_needs_update = true;
		return HRTIMER_REPROGRAM_NONE;
	}

	if (!was_first || cpu_base != this_cpu_base) {
		/*
		 * If the current CPU base is online, then the timer is never
		 * queued on a remote CPU if it would be the first expiring
		 * timer there unless the timer callback is currently executed
		 * on the remote CPU. In the latter case the remote CPU will
		 * re-evaluate the first expiring timer after completing the
		 * callbacks.
		 */
		/*
		 * 在线本地 CPU 的目标适配检查保证：远端新首 timer 不会依赖迟到 IPI；
		 * 唯一例外是远端 callback 正运行，它结束时自然重算。
		 */
		if (likely(hrtimer_base_is_online(this_cpu_base)))
			return first ? HRTIMER_REPROGRAM : HRTIMER_REPROGRAM_NONE;

		/*
		 * Timer was enqueued remote because the current base is
		 * already offline. If the timer is the first to expire,
		 * kick the remote CPU to reprogram the clock event.
		 */
		/* 本地已 offline 的兜底：若成为远端首节点，用异步 CSD 请求目标重编程。 */
		if (first)
			smp_call_function_single_async(cpu_base->cpu, &cpu_base->csd);
		return HRTIMER_REPROGRAM_NONE;
	}

	/*
	 * Special case for the HRTICK timer. It is frequently rearmed and most
	 * of the time moves the expiry into the future. That's expensive in
	 * virtual machines and it's better to take the pointless already armed
	 * interrupt than reprogramming the hardware on every context switch.
	 *
	 * If the new expiry is before the armed time, then reprogramming is
	 * required.
	 */
	/*
	 * lazy HRTICK 常把期限后移；保留已经较早的硬件中断，虽会多一次空中断，却
	 * 避免 VM 每次 context switch 都产生昂贵的虚拟 clockevent 写。
	 */
	if (timer->is_lazy) {
		if (cpu_base->expires_next <= hrtimer_get_expires(timer))
			return HRTIMER_REPROGRAM_NONE;
	}

	/*
	 * Timer was the first expiring timer and forced to stay on the
	 * current CPU to avoid reprogramming on removal and enqueue. Force
	 * reprogram the hardware by evaluating the new first expiring
	 * timer.
	 */
	/* 旧首节点原地重启可能前移或后移，必须扫描所有 base 后再确定真正硬件期限。 */
	return HRTIMER_REPROGRAM_FORCE;
}

/*
 * hrtimer_start_range_ns_common() - 检查 mode 与初始化时 hard/soft 属性一致。
 *
 * 非 RT 默认 hard，SOFT 位必须与 is_soft 匹配；RT 默认 soft，显式 HARD 位与
 * is_hard 匹配。告警后仍进入核心，返回相同编程动作。
 */
static int hrtimer_start_range_ns_common(struct hrtimer *timer, ktime_t tim,
					 u64 delta_ns, const enum hrtimer_mode mode,
					 struct hrtimer_clock_base *base)
{
	/*
	 * Check whether the HRTIMER_MODE_SOFT bit and hrtimer.is_soft
	 * match on CONFIG_PREEMPT_RT = n. With PREEMPT_RT check the hard
	 * expiry mode because unmarked timers are moved to softirq expiry.
	 */
	/* setup 决定执行上下文，start 不能悄悄改变它，否则 callback 锁语义失效。 */
	if (!IS_ENABLED(CONFIG_PREEMPT_RT))
		WARN_ON_ONCE(!(mode & HRTIMER_MODE_SOFT) ^ !timer->is_soft);
	else
		WARN_ON_ONCE(!(mode & HRTIMER_MODE_HARD) ^ !timer->is_hard);

	return __hrtimer_start_range_ns(timer, tim, delta_ns, mode, base);
}

/**
 * hrtimer_start_range_ns - (re)start an hrtimer
 * @timer:	the timer to be added
 * @tim:	expiry time
 * @delta_ns:	"slack" range for the timer
 * @mode:	timer mode: absolute (HRTIMER_MODE_ABS) or
 *		relative (HRTIMER_MODE_REL), and pinned (HRTIMER_MODE_PINNED);
 *		softirq based mode is considered for debug purpose only!
 */
/*
 * 中文契约：启动或重启 @timer。@tim 按 @mode 为绝对/相对纳秒，@delta_ns 允许
 * 合并唤醒，PINNED 在本次启动生效。无直接返回值；函数锁定当前 base、完成可能
 * 的迁移/入树，并在线性化点后按核心返回值更新本 CPU clockevent。
 */
void hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim, u64 delta_ns,
			    const enum hrtimer_mode mode)
{
	struct hrtimer_clock_base *base;
	unsigned long flags;

	debug_hrtimer_assert_init(timer);

	base = lock_hrtimer_base(timer, &flags);

	switch (hrtimer_start_range_ns_common(timer, tim, delta_ns, mode, base)) {
	case HRTIMER_REPROGRAM:
		hrtimer_reprogram(timer, true);
		break;
	case HRTIMER_REPROGRAM_FORCE:
		hrtimer_force_reprogram(timer->base->cpu_base, 1);
		break;
	case HRTIMER_REPROGRAM_NONE:
		break;
	}

	unlock_hrtimer_base(timer, &flags);
}
EXPORT_SYMBOL_GPL(hrtimer_start_range_ns);

/*
 * hrtimer_check_user_timer() - 检查用户控制 timer 是否在锁内已经过期。
 *
 * @timer 已入队且 base 锁持有。返回 true 表示仍保留队列，false 表示已摘除并
 * 由调用者处理立即到期。使用 soft expiry 代表用户请求时间，而非 slack 后端点。
 */
static inline bool hrtimer_check_user_timer(struct hrtimer *timer)
{
	struct hrtimer_cpu_base *cpu_base = timer->base->cpu_base;
	ktime_t expires;

	/*
	 * This uses soft expires because that's the user provided
	 * expiry time, while expires can be further in the past
	 * due to a slack value added to the user expiry time.
	 */
	/* hard expires 可因 slack 更晚；判断用户 deadline 必须看 softexpires。 */
	expires = hrtimer_get_softexpires(timer);

	/* Convert to monotonic */
	/* 各 clockid 树内存本地坐标，减 offset 后才能与 CPU 硬件期限比较。 */
	expires = ktime_sub(expires, timer->base->offset);

	/*
	 * Check whether this timer will end up as the first expiring timer in
	 * the CPU base. If not, no further checks required as it's then
	 * guaranteed to expire in the future.
	 */
	/* 非 CPU 首事件必有更早事件负责唤醒，届时会再次扫描，因此无需当前采样。 */
	if (expires >= cpu_base->expires_next)
		return true;

	/* Validate that the expiry time is in the future. */
	/* 首事件已过期时摘除而不调用 callback，避免在未知调用者锁下死锁。 */
	if (expires > ktime_get())
		return true;

	debug_hrtimer_deactivate(timer);
	__remove_hrtimer(timer, timer->base, HRTIMER_STATE_INACTIVE, false);
	trace_hrtimer_start_expired(timer);
	return false;
}

/**
 * hrtimer_start_range_ns_user - (re)start an user controlled hrtimer
 * @timer:	the timer to be added
 * @tim:	expiry time
 * @delta_ns:	"slack" range for the timer
 * @mode:	timer mode: absolute (HRTIMER_MODE_ABS) or
 *		relative (HRTIMER_MODE_REL), and pinned (HRTIMER_MODE_PINNED);
 *		softirq based mode is considered for debug purpose only!
 *
 * Returns: True when the timer was queued, false if it was already expired
 *
 * This function cannot invoke the timer callback for expired timers as it might
 * be called under a lock which the timer callback needs to acquire. So the
 * caller has to handle that case.
 */
/*
 * 中文契约：用户输入版本的 start。参数语义同 hrtimer_start_range_ns()；返回
 * true 表示已排队，false 表示用户期限已过且 callback 未调用。调用者必须处理
 * false，例如直接完成系统调用；该设计避免 callback 获取调用者正持有的锁。
 */
bool hrtimer_start_range_ns_user(struct hrtimer *timer, ktime_t tim,
				 u64 delta_ns, const enum hrtimer_mode mode)
{
	struct hrtimer_clock_base *base;
	unsigned long flags;
	bool ret = true;

	debug_hrtimer_assert_init(timer);

	base = lock_hrtimer_base(timer, &flags);

	switch (hrtimer_start_range_ns_common(timer, tim, delta_ns, mode, base)) {
	case HRTIMER_REPROGRAM:
		ret = hrtimer_check_user_timer(timer);
		if (ret)
			hrtimer_reprogram(timer, true);
		break;
	case HRTIMER_REPROGRAM_FORCE:
		ret = hrtimer_check_user_timer(timer);
		/*
		 * The base must always be reevaluated, independent of the
		 * result above because the timer was the first pending timer.
		 */
		/* 原 timer 是 CPU 首节点，即使已过期被摘除，也必须找出并编程新的首节点。 */
		hrtimer_force_reprogram(timer->base->cpu_base, 1);
		break;
	case HRTIMER_REPROGRAM_NONE:
		break;
	}

	unlock_hrtimer_base(timer, &flags);
	return ret;
}
EXPORT_SYMBOL_GPL(hrtimer_start_range_ns_user);

/**
 * hrtimer_try_to_cancel - try to deactivate a timer
 * @timer:	hrtimer to stop
 *
 * Returns:
 *
 *  *  0 when the timer was not active
 *  *  1 when the timer was active
 *  * -1 when the timer is currently executing the callback function and
 *    cannot be stopped
 */
/*
 * 中文契约：非等待式取消。返回 0 表示 inactive，1 表示已摘除 active timer，
 * -1 表示 callback 正在运行而无法取消。@timer 对象仍归调用者；返回 0/1 时
 * 仅证明锁释放瞬间不 running/queued，不能阻止随后并发重启。
 */
int hrtimer_try_to_cancel(struct hrtimer *timer)
{
	struct hrtimer_clock_base *base;
	unsigned long flags;
	int ret = -1;

	/*
	 * Check lockless first. If the timer is not active (neither
	 * enqueued nor running the callback, nothing to do here.  The
	 * base lock does not serialize against a concurrent enqueue,
	 * so we can avoid taking it.
	 */
	/*
	 * fast path 允许与并发 enqueue 竞态，因为 base 锁本来也不能阻止调用者在
	 * 返回后重启；它只需避免 false negative，hrtimer_active() 用 seqcount 保证。
	 */
	if (!hrtimer_active(timer))
		return 0;

	base = lock_hrtimer_base(timer, &flags);

	if (!hrtimer_callback_running(timer)) {
		ret = remove_hrtimer(timer, base, HRTIMER_STATE_INACTIVE);
		if (ret)
			trace_hrtimer_cancel(timer);
	}

	unlock_hrtimer_base(timer, &flags);

	return ret;

}
EXPORT_SYMBOL_GPL(hrtimer_try_to_cancel);

#ifdef CONFIG_PREEMPT_RT
/*
 * RT expiry_lock helpers：softirq callback 整段持可睡眠锁，base raw lock 仍保护
 * 树和状态。init/lock/unlock 无返回值，供同步取消者避免优先级反转。
 */
static void hrtimer_cpu_base_init_expiry_lock(struct hrtimer_cpu_base *base)
{
	spin_lock_init(&base->softirq_expiry_lock);
}

static void hrtimer_cpu_base_lock_expiry(struct hrtimer_cpu_base *base)
	__acquires(&base->softirq_expiry_lock)
{
	spin_lock(&base->softirq_expiry_lock);
}

static void hrtimer_cpu_base_unlock_expiry(struct hrtimer_cpu_base *base)
	__releases(&base->softirq_expiry_lock)
{
	spin_unlock(&base->softirq_expiry_lock);
}

/*
 * The counterpart to hrtimer_cancel_wait_running().
 *
 * If there is a waiter for cpu_base->expiry_lock, then it was waiting for
 * the timer callback to finish. Drop expiry_lock and reacquire it. That
 * allows the waiter to acquire the lock and make progress.
 */
/*
 * 中文说明：若取消者在等 softirq_expiry_lock，callback 完成后释放并重取两锁，
 * 让 waiter 获得进度。函数返回时恢复锁状态；@flags 用于正确恢复 IRQ。
 */
static void hrtimer_sync_wait_running(struct hrtimer_cpu_base *cpu_base, unsigned long flags)
{
	if (atomic_read(&cpu_base->timer_waiters)) {
		raw_spin_unlock_irqrestore(&cpu_base->lock, flags);
		spin_unlock(&cpu_base->softirq_expiry_lock);
		spin_lock(&cpu_base->softirq_expiry_lock);
		raw_spin_lock_irq(&cpu_base->lock);
	}
}

#ifdef CONFIG_SMP
/* SMP 下识别换锁哨兵；哨兵不对应真实执行上下文，等待者只能 relax 重试。 */
static __always_inline bool is_migration_base(struct hrtimer_clock_base *base)
{
	return base == &migration_base;
}
#else
/* UP 无迁移哨兵，恒 false。 */
static __always_inline bool is_migration_base(struct hrtimer_clock_base *base)
{
	return false;
}
#endif

/*
 * This function is called on PREEMPT_RT kernels when the fast path
 * deletion of a timer failed because the timer callback function was
 * running.
 *
 * This prevents priority inversion: if the soft irq thread is preempted
 * in the middle of a timer callback, then calling hrtimer_cancel() can
 * lead to two issues:
 *
 *  - If the caller is on a remote CPU then it has to spin wait for the timer
 *    handler to complete. This can result in unbound priority inversion.
 *
 *  - If the caller originates from the task which preempted the timer
 *    handler on the same CPU, then spin waiting for the timer handler to
 *    complete is never going to end.
 */
/*
 * hrtimer_cancel_wait_running() - RT 上等待一个 soft callback 临界区结束。
 *
 * @timer 为借用对象；hardirq timer 或 migration 中只 cpu_relax，soft timer 则
 * 通过 expiry_lock 睡眠等待，避免高优先级取消者忙等低优先级 softirq 线程，
 * 以及同 CPU 上取消者抢占 callback 后形成永久自旋。
 */
void hrtimer_cancel_wait_running(const struct hrtimer *timer)
{
	/* Lockless read. Prevent the compiler from reloading it below */
	/* 单次 base 快照保证 migration 判断与后续 cpu_base 选择来自同一版本。 */
	struct hrtimer_clock_base *base = READ_ONCE(timer->base);

	/*
	 * Just relax if the timer expires in hard interrupt context or if
	 * it is currently on the migration base.
	 */
	/* hardirq 会在开中断后推进；migration 哨兵没有可等待的真实 expiry_lock。 */
	if (!timer->is_soft || is_migration_base(base)) {
		cpu_relax();
		return;
	}

	/*
	 * Mark the base as contended and grab the expiry lock, which is
	 * held by the softirq across the timer callback. Drop the lock
	 * immediately so the softirq can expire the next timer. In theory
	 * the timer could already be running again, but that's more than
	 * unlikely and just causes another wait loop.
	 */
	/* waiter 计数要求 callback 端主动让锁；拿到即释放只充当完成/进度屏障。 */
	atomic_inc(&base->cpu_base->timer_waiters);
	spin_lock_bh(&base->cpu_base->softirq_expiry_lock);
	atomic_dec(&base->cpu_base->timer_waiters);
	spin_unlock_bh(&base->cpu_base->softirq_expiry_lock);
}
#else
/* 非 RT 不需要可睡眠 expiry 锁；取消循环通过 cpu_relax/再次检查收敛。 */
static inline void hrtimer_cpu_base_init_expiry_lock(struct hrtimer_cpu_base *base) { }
static inline void hrtimer_cpu_base_lock_expiry(struct hrtimer_cpu_base *base) { }
static inline void hrtimer_cpu_base_unlock_expiry(struct hrtimer_cpu_base *base) { }
static inline void hrtimer_sync_wait_running(struct hrtimer_cpu_base *base, unsigned long fl) { }
#endif

/**
 * hrtimer_cancel - cancel a timer and wait for the handler to finish.
 * @timer:	the timer to be cancelled
 *
 * Returns:
 *  0 when the timer was not active
 *  1 when the timer was active
 */
/*
 * 中文契约：循环取消直到 callback 不再运行。返回 1 表示摘除过 queued timer，
 * 0 表示最终 inactive。它不永久封死重启；调用者必须先停止所有 rearm 来源，
 * 才能把返回当作释放 timer 容器的生命周期屏障。RT 慢路径可能睡眠。
 */
int hrtimer_cancel(struct hrtimer *timer)
{
	int ret;

	do {
		ret = hrtimer_try_to_cancel(timer);

		if (ret < 0)
			hrtimer_cancel_wait_running(timer);
	} while (ret < 0);
	return ret;
}
EXPORT_SYMBOL_GPL(hrtimer_cancel);

/**
 * __hrtimer_get_remaining - get remaining time for the timer
 * @timer:	the timer to read
 * @adjust:	adjust relative timers when CONFIG_TIME_LOW_RES=y
 */
/*
 * 中文契约：在正确 base 锁下读取 @timer 剩余纳秒。@adjust=true 且 lowres 时
 * 撤销相对 timer 的一 tick 补偿，向调用者呈现原语义。返回可为负，表示已过期；
 * 不改变 timer 或 ownership。
 */
ktime_t __hrtimer_get_remaining(const struct hrtimer *timer, bool adjust)
{
	unsigned long flags;
	ktime_t rem;

	lock_hrtimer_base(timer, &flags);
	if (IS_ENABLED(CONFIG_TIME_LOW_RES) && adjust)
		rem = hrtimer_expires_remaining_adjusted(timer);
	else
		rem = hrtimer_expires_remaining(timer);
	unlock_hrtimer_base(timer, &flags);

	return rem;
}
EXPORT_SYMBOL_GPL(__hrtimer_get_remaining);

#ifdef CONFIG_NO_HZ_COMMON
/**
 * hrtimer_get_next_event - get the time until next expiry event
 *
 * Returns the next expiry time or KTIME_MAX if no timer is pending.
 */
/*
 * 中文契约：NO_HZ lowres 路径查询当前 CPU 下一 hrtimer 的绝对单调期限；无事件
 * 返回 KTIME_MAX。highres 已自行编程硬件时保持 KTIME_MAX，避免 tick 重复代管。
 */
ktime_t hrtimer_get_next_event(void)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	ktime_t expires = KTIME_MAX;

	guard(raw_spinlock_irqsave)(&cpu_base->lock);
	if (!hrtimer_hres_active(cpu_base))
		expires = __hrtimer_get_next_event(cpu_base, HRTIMER_ACTIVE_ALL);

	return expires;
}

/**
 * hrtimer_next_event_without - time until next expiry event w/o one timer
 * @exclude:	timer to exclude
 *
 * Returns the next expiry time over all timers except for the @exclude one or
 * KTIME_MAX if none of them is pending.
 */
/*
 * 中文契约：highres 模式下计算排除 @exclude 后当前 CPU 的下一期限，供停 tick/
 * 重新编程决策。返回 KTIME_MAX 表示无其他 timer；只读树，不更新缓存。
 */
ktime_t hrtimer_next_event_without(const struct hrtimer *exclude)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	ktime_t expires = KTIME_MAX;
	unsigned int active;

	guard(raw_spinlock_irqsave)(&cpu_base->lock);
	if (!hrtimer_hres_active(cpu_base))
		return expires;

	active = cpu_base->active_bases & HRTIMER_ACTIVE_SOFT;
	if (active && !cpu_base->softirq_activated)
		expires = hrtimer_bases_next_event_without(cpu_base, exclude, active, KTIME_MAX);

	active = cpu_base->active_bases & HRTIMER_ACTIVE_HARD;
	if (!active)
		return expires;
	return hrtimer_bases_next_event_without(cpu_base, exclude, active, expires);
}
#endif

/*
 * hrtimer_clockid_to_base() - 把支持的 clockid 映射到 hard base 前半区索引。
 * 无效 clockid 告警并回退 MONOTONIC；soft 偏移由 setup 另行添加。
 */
static inline int hrtimer_clockid_to_base(clockid_t clock_id)
{
	switch (clock_id) {
	case CLOCK_MONOTONIC:
		return HRTIMER_BASE_MONOTONIC;
	case CLOCK_REALTIME:
		return HRTIMER_BASE_REALTIME;
	case CLOCK_BOOTTIME:
		return HRTIMER_BASE_BOOTTIME;
	case CLOCK_TAI:
		return HRTIMER_BASE_TAI;
	default:
		WARN(1, "Invalid clockid %d. Using MONOTONIC\n", clock_id);
		return HRTIMER_BASE_MONOTONIC;
	}
}

/*
 * __hrtimer_cb_get_time() - 读取指定 clockid 的当前绝对 ktime。
 * 返回纳秒时间；无效 id 告警并回退 MONOTONIC。用于把相对期限转成绝对坐标。
 */
static ktime_t __hrtimer_cb_get_time(clockid_t clock_id)
{
	switch (clock_id) {
	case CLOCK_MONOTONIC:
		return ktime_get();
	case CLOCK_REALTIME:
		return ktime_get_real();
	case CLOCK_BOOTTIME:
		return ktime_get_boottime();
	case CLOCK_TAI:
		return ktime_get_clocktai();
	default:
		WARN(1, "Invalid clockid %d. Using MONOTONIC\n", clock_id);
		return ktime_get();
	}
}

/* 按 @timer 当前 base clockid 取 callback 可用的“现在”；返回值不稳定 base 生命周期。 */
ktime_t hrtimer_cb_get_time(const struct hrtimer *timer)
{
	return __hrtimer_cb_get_time(timer->base->clockid);
}
EXPORT_SYMBOL_GPL(hrtimer_cb_get_time);

/*
 * __hrtimer_setup() - 建立 hrtimer 的初始 inactive 状态和执行上下文属性。
 *
 * @timer 由调用者拥有且不可 active；@fn 是长期借用 callback；@clock_id 选择时间
 * 坐标；@mode 决定 hard/soft/lazy。无返回值，不入队。RT 默认把未显式 HARD 的
 * timer 放到 softirq；相对 REALTIME 按 POSIX 语义改用 MONOTONIC。
 */
static void __hrtimer_setup(struct hrtimer *timer, enum hrtimer_restart (*fn)(struct hrtimer *),
			    clockid_t clock_id, enum hrtimer_mode mode)
{
	bool softtimer = !!(mode & HRTIMER_MODE_SOFT);
	struct hrtimer_cpu_base *cpu_base;
	int base;

	/*
	 * On PREEMPT_RT enabled kernels hrtimers which are not explicitly
	 * marked for hard interrupt expiry mode are moved into soft
	 * interrupt context for latency reasons and because the callbacks
	 * can invoke functions which might sleep on RT, e.g. spin_lock().
	 */
	/* RT 上普通 spinlock 可能睡眠，故默认 callback 不能在 hardirq 执行。 */
	if (IS_ENABLED(CONFIG_PREEMPT_RT) && !(mode & HRTIMER_MODE_HARD))
		softtimer = true;

	memset(timer, 0, sizeof(struct hrtimer));

	cpu_base = raw_cpu_ptr(&hrtimer_bases);

	/*
	 * POSIX magic: Relative CLOCK_REALTIME timers are not affected by
	 * clock modifications, so they needs to become CLOCK_MONOTONIC to
	 * ensure POSIX compliance.
	 */
	/* wall clock 跳变只影响绝对 realtime timer；相对等待必须按单调经过时间计量。 */
	if (clock_id == CLOCK_REALTIME && mode & HRTIMER_MODE_REL)
		clock_id = CLOCK_MONOTONIC;

	base = softtimer ? HRTIMER_MAX_CLOCK_BASES / 2 : 0;
	base += hrtimer_clockid_to_base(clock_id);
	timer->is_soft = softtimer;
	timer->is_hard = !!(mode & HRTIMER_MODE_HARD);
	timer->is_lazy = !!(mode & HRTIMER_MODE_LAZY_REARM);
	timer->base = &cpu_base->clock_base[base];
	timerqueue_linked_init(&timer->node);

	if (WARN_ON_ONCE(!fn))
		ACCESS_PRIVATE(timer, function) = hrtimer_dummy_timeout;
	else
		ACCESS_PRIVATE(timer, function) = fn;
}

/**
 * hrtimer_setup - initialize a timer to the given clock
 * @timer:	the timer to be initialized
 * @function:	the callback function
 * @clock_id:	the clock to be used
 * @mode:       The modes which are relevant for initialization:
 *              HRTIMER_MODE_ABS, HRTIMER_MODE_REL, HRTIMER_MODE_ABS_SOFT,
 *              HRTIMER_MODE_REL_SOFT
 *
 *              The PINNED variants of the above can be handed in,
 *              but the PINNED bit is ignored as pinning happens
 *              when the hrtimer is started
 */
/*
 * 中文契约：初始化普通内存中的 inactive timer。全部参数均为输入，timer ownership
 * 不转移；PINNED 只在 start 时解释。无返回值，调用任何 start/cancel 前必须 setup。
 */
void hrtimer_setup(struct hrtimer *timer, enum hrtimer_restart (*function)(struct hrtimer *),
		   clockid_t clock_id, enum hrtimer_mode mode)
{
	debug_setup(timer, clock_id, mode);
	__hrtimer_setup(timer, function, clock_id, mode);
}
EXPORT_SYMBOL_GPL(hrtimer_setup);

/**
 * hrtimer_setup_on_stack - initialize a timer on stack memory
 * @timer:	The timer to be initialized
 * @function:	the callback function
 * @clock_id:	The clock to be used
 * @mode:       The timer mode
 *
 * Similar to hrtimer_setup(), except that this one must be used if struct hrtimer is in stack
 * memory.
 */
/*
 * 中文契约：栈对象版本的 setup，参数语义相同；离开作用域前必须 cancel 并调用
 * destroy_hrtimer_on_stack()，否则 debugobjects 会报告生命周期错误。
 */
void hrtimer_setup_on_stack(struct hrtimer *timer,
			    enum hrtimer_restart (*function)(struct hrtimer *),
			    clockid_t clock_id, enum hrtimer_mode mode)
{
	debug_setup_on_stack(timer, clock_id, mode);
	__hrtimer_setup(timer, function, clock_id, mode);
}
EXPORT_SYMBOL_GPL(hrtimer_setup_on_stack);

/*
 * A timer is active, when it is enqueued into the rbtree or the
 * callback function is running or it's in the state of being migrated
 * to another cpu.
 *
 * It is important for this function to not return a false negative.
 */
/*
 * hrtimer_active() - 无锁、无 false-negative 地判断 queued/running/migrating。
 *
 * @timer 为借用对象；返回 bool，不提供长期引用或后续状态冻结。seqcount 把
 * is_queued 与 base->running 绑定到同一执行阶段，并复核 base 未跨 CPU 改变。
 */
bool hrtimer_active(const struct hrtimer *timer)
{
	struct hrtimer_clock_base *base;
	unsigned int seq;

	do {
		base = READ_ONCE(timer->base);
		seq = raw_read_seqcount_begin(&base->seq);

		if (timer->is_queued || base->running == timer)
			return true;

	} while (read_seqcount_retry(&base->seq, seq) || base != READ_ONCE(timer->base));

	return false;
}
EXPORT_SYMBOL_GPL(hrtimer_active);

/*
 * The write_seqcount_barrier()s in __run_hrtimer() split the thing into 3
 * distinct sections:
 *
 *  - queued:	the timer is queued
 *  - callback:	the timer is being ran
 *  - post:	the timer is inactive or (re)queued
 *
 * On the read side we ensure we observe timer->is_queued and cpu_base->running
 * from the same section, if anything changed while we looked at it, we retry.
 * This includes timer->base changing because sequence numbers alone are
 * insufficient for that.
 *
 * The sequence numbers are required because otherwise we could still observe
 * a false negative if the read side got smeared over multiple consecutive
 * __run_hrtimer() invocations.
 */
/*
 * __run_hrtimer() - 在锁内摘除一个到期 timer，解锁执行 callback，再收敛重启状态。
 *
 * @cpu_base/@base 已锁；@timer 是树最左到期对象；@now 是该 base 坐标；@flags
 * 用于解锁恢复 IRQ。无返回值。两次 seqcount barrier 把 queued -> callback ->
 * post 切成三个版本，使 hrtimer_active() 不会跨连续 callback 拼出假 inactive。
 */
static void __run_hrtimer(struct hrtimer_cpu_base *cpu_base, struct hrtimer_clock_base *base,
			  struct hrtimer *timer, ktime_t now, unsigned long flags)
	__must_hold(&cpu_base->lock)
{
	enum hrtimer_restart (*fn)(struct hrtimer *);
	bool expires_in_hardirq;
	int restart;

	lockdep_assert_held(&cpu_base->lock);

	debug_hrtimer_deactivate(timer);
	base->running = timer;

	/*
	 * Separate the ->running assignment from the ->is_queued assignment.
	 *
	 * As with a regular write barrier, this ensures the read side in
	 * hrtimer_active() cannot observe base->running == NULL &&
	 * timer->is_queued == INACTIVE.
	 */
	/* 先发布 running，再清 queued；读侧若跨越屏障会因 seqcount 变化重试。 */
	raw_write_seqcount_barrier(&base->seq);

	__remove_hrtimer(timer, base, HRTIMER_STATE_INACTIVE, false);
	fn = ACCESS_PRIVATE(timer, function);

	/*
	 * Clear the 'is relative' flag for the TIME_LOW_RES case. If the
	 * timer is restarted with a period then it becomes an absolute
	 * timer. If its not restarted it does not matter.
	 */
	/* 周期 callback 以后用绝对 expires 推进，不能重复叠加 lowres 的一 tick 补偿。 */
	if (IS_ENABLED(CONFIG_TIME_LOW_RES))
		timer->is_rel = false;

	/*
	 * The timer is marked as running in the CPU base, so it is
	 * protected against migration to a different CPU even if the lock
	 * is dropped.
	 */
	/* running 锚定 base 后可安全放锁调用任意 callback，同时禁止迁移改变同步位置。 */
	raw_spin_unlock_irqrestore(&cpu_base->lock, flags);
	trace_hrtimer_expire_entry(timer, now);
	expires_in_hardirq = lockdep_hrtimer_enter(timer);

	restart = fn(timer);

	lockdep_hrtimer_exit(expires_in_hardirq);
	trace_hrtimer_expire_exit(timer);
	raw_spin_lock_irq(&cpu_base->lock);

	/*
	 * Note: We clear the running state after enqueue_hrtimer and
	 * we do not reprogram the event hardware. Happens either in
	 * hrtimer_start_range_ns() or in hrtimer_interrupt()
	 *
	 * Note: Because we dropped the cpu_base->lock above,
	 * hrtimer_start_range_ns() can have popped in and enqueued the timer
	 * for us already.
	 */
	/*
	 * callback 返回前其他 CPU 可能已重启 timer；只有 RESTART 且尚未 queued 才由
	 * 本路径重插，避免双重入树。硬件由外层统一重编程。
	 */
	if (restart == HRTIMER_RESTART && !timer->is_queued)
		enqueue_hrtimer(timer, base, HRTIMER_MODE_ABS, false);

	/*
	 * Separate the ->running assignment from the ->is_queued assignment.
	 *
	 * As with a regular write barrier, this ensures the read side in
	 * hrtimer_active() cannot observe base->running.timer == NULL &&
	 * timer->is_queued == INACTIVE.
	 */
	/* 先确保可能的 requeue 已发布，再清 running；读侧始终至少看到一种 active。 */
	raw_write_seqcount_barrier(&base->seq);

	WARN_ON_ONCE(base->running != timer);
	base->running = NULL;
}

/* 安全读取树首 timer；空树返回 NULL。调用者持 cpu_base 锁。 */
static __always_inline struct hrtimer *clock_base_next_timer_safe(struct hrtimer_clock_base *base)
{
	struct timerqueue_linked_node *next = timerqueue_linked_first(&base->active);

	return next ? hrtimer_from_timerqueue_node(next) : NULL;
}

/*
 * __hrtimer_run_queues() - 扫描指定 hard/soft base 并执行所有 soft-expired timer。
 *
 * @cpu_base 已锁；@now 是单调时间；@flags 用于 callback 解锁；@active_mask 选择
 * 上下文。每个 base 把 now 加 offset 转到本地时钟。遇到首个 softexpires 尚未
 * 到达便停止，因为树按 hard expires 排序且该 timer 的未来唤醒会覆盖右侧 slack。
 */
static void __hrtimer_run_queues(struct hrtimer_cpu_base *cpu_base, ktime_t now,
				 unsigned long flags, unsigned int active_mask)
{
	unsigned int active = cpu_base->active_bases & active_mask;
	struct hrtimer_clock_base *base;

	for_each_active_base(base, cpu_base, active) {
		ktime_t basenow = ktime_add(now, base->offset);
		struct hrtimer *timer;

		while ((timer = clock_base_next_timer(base))) {
			/*
			 * The immediate goal for using the softexpires is
			 * minimizing wakeups, not running timers at the
			 * earliest interrupt after their soft expiration.
			 * This allows us to avoid using a Priority Search
			 * Tree, which can answer a stabbing query for
			 * overlapping intervals and instead use the simple
			 * BST we already have.
			 * We don't add extra wakeups by delaying timers that
			 * are right-of a not yet expired timer, because that
			 * timer will have to trigger a wakeup anyway.
			 */
			/*
			 * slack 形成 [soft, hard] 区间。本实现用按 hard expires 排序的普通
			 * BST，而非支持区间 stabbing query 的复杂树；若最左 hard timer 的
			 * soft 期限未到，右侧 timer 延后不会新增唤醒，因此可以整体停止。
			 */
			if (basenow < hrtimer_get_softexpires(timer))
				break;

			__run_hrtimer(cpu_base, base, timer, basenow, flags);
			if (active_mask == HRTIMER_ACTIVE_SOFT)
				hrtimer_sync_wait_running(cpu_base, flags);
		}
	}
}

/*
 * hrtimer_run_softirq() - 执行当前 CPU 的 soft hrtimer 队列。
 *
 * 无入参/返回；softirq 上下文。RT 上先持 expiry_lock 让同步取消者可等待整段
 * callback；raw lock 保护树。执行后清 activated，再重建下一 soft 期限并可能
 * 重编程硬件，最后按逆序释放锁。
 */
static __latent_entropy void hrtimer_run_softirq(void)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	unsigned long flags;
	ktime_t now;

	hrtimer_cpu_base_lock_expiry(cpu_base);
	raw_spin_lock_irqsave(&cpu_base->lock, flags);

	now = hrtimer_update_base(cpu_base);
	__hrtimer_run_queues(cpu_base, now, flags, HRTIMER_ACTIVE_SOFT);

	cpu_base->softirq_activated = false;
	hrtimer_update_softirq_timer(cpu_base, true);

	raw_spin_unlock_irqrestore(&cpu_base->lock, flags);
	hrtimer_cpu_base_unlock_expiry(cpu_base);
}

#ifdef CONFIG_HIGH_RES_TIMERS

/*
 * Very similar to hrtimer_force_reprogram(), except it deals with
 * deferred_rearm and hang_detected.
 */
/*
 * hrtimer_rearm() - interrupt 结束时提交新硬件事件并处理 hang 抑制。
 *
 * @cpu_base 已锁；@expires_next 为重算期限；@deferred 仅用于 trace。清除
 * deferred_rearm 后，若检测到中断循环则把事件推迟一个受 max_hang_time 限制的
 * 短窗口，让系统有机会执行其他工作。
 */
static void hrtimer_rearm(struct hrtimer_cpu_base *cpu_base, ktime_t expires_next, bool deferred)
{
	cpu_base->expires_next = expires_next;
	cpu_base->deferred_rearm = false;

	if (unlikely(cpu_base->hang_detected)) {
		/*
		 * Give the system a chance to do something else than looping
		 * on hrtimer interrupts.
		 */
		/* 最多延后 100ms；使用已观测最大 hang 时间避免立即重新陷入同一循环。 */
		expires_next = ktime_add_ns(ktime_get(),
					    min(100 * NSEC_PER_MSEC, cpu_base->max_hang_time));
	}
	hrtimer_rearm_event(expires_next, deferred);
}

#ifdef CONFIG_HRTIMER_REARM_DEFERRED
/*
 * __hrtimer_rearm_deferred() - 在 IRQ 退出的安全点完成延迟 clockevent 编程。
 *
 * 无入参/返回；仅当前 CPU。若期间 timer/offset 改变则锁内重算，否则复用 IRQ
 * 缓存；最终调用 hrtimer_rearm() 清状态并提交设备。
 */
void __hrtimer_rearm_deferred(void)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	ktime_t expires_next;

	if (!cpu_base->deferred_rearm)
		return;

	guard(raw_spinlock)(&cpu_base->lock);
	if (cpu_base->deferred_needs_update) {
		hrtimer_update_base(cpu_base);
		expires_next = hrtimer_update_next_event(cpu_base);
	} else {
		/* No timer added/removed. Use the cached value */
		/* 缓存来自刚结束的 interrupt 扫描，未失效时可避免再次遍历所有树。 */
		expires_next = cpu_base->deferred_expires_next;
	}
	hrtimer_rearm(cpu_base, expires_next, true);
}

/*
 * hrtimer_interrupt_rearm() - 延迟配置下缓存期限并设置线程标志。
 * IRQ 本轮刚完成重算，先清 needs_update；真正设备写推迟到返回路径。
 */
static __always_inline void
hrtimer_interrupt_rearm(struct hrtimer_cpu_base *cpu_base, ktime_t expires_next)
{
	/* hrtimer_interrupt() just re-evaluated the first expiring timer */
	/* 当前 expires_next 与树一致，后续修改者会重新置 deferred_needs_update。 */
	cpu_base->deferred_needs_update = false;
	/* Cache the expiry time */
	/* TIF_HRTIMER_REARM 使 IRQ exit 调用 __hrtimer_rearm_deferred()。 */
	cpu_base->deferred_expires_next = expires_next;
	set_thread_flag(TIF_HRTIMER_REARM);
}
#else  /* CONFIG_HRTIMER_REARM_DEFERRED */
/* 无延迟配置时在 hrtimer interrupt 内立即编程。 */
static __always_inline void
hrtimer_interrupt_rearm(struct hrtimer_cpu_base *cpu_base, ktime_t expires_next)
{
	hrtimer_rearm(cpu_base, expires_next, false);
}
#endif  /* !CONFIG_HRTIMER_REARM_DEFERRED */

/*
 * High resolution timer interrupt
 * Called with interrupts disabled
 */
/*
 * hrtimer_interrupt() - 高分辨率 clockevent 中断核心。
 *
 * @dev 是当前 CPU oneshot 设备借用指针；IRQ 已关闭，不可睡眠。函数锁内刷新时间，
 * raise 已到 softirq，执行所有 hard timer，重算首事件并立即或延迟 rearm。
 * 最多重试三次吸收 tracing/长 callback/VM 调度造成的“重算后仍过期”；持续失败
 * 进入 hang 保护，避免硬中断活锁。
 */
void hrtimer_interrupt(struct clock_event_device *dev)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	ktime_t expires_next, now, entry_time, delta;
	unsigned long flags;
	int retries = 0;

	BUG_ON(!cpu_base->hres_active);
	cpu_base->nr_events++;
	dev->next_event = KTIME_MAX;
	dev->next_event_forced = 0;

	raw_spin_lock_irqsave(&cpu_base->lock, flags);
	entry_time = now = hrtimer_update_base(cpu_base);
retry:
	cpu_base->deferred_rearm = true;
	/*
	 * Set expires_next to KTIME_MAX, which prevents that remote CPUs queue
	 * timers while __hrtimer_run_queues() is expiring the clock bases.
	 * Timers which are re/enqueued on the local CPU are not affected by
	 * this.
	 */
	/*
	 * 临时 KTIME_MAX 使目标适配逻辑不会把一个会成为更早首事件的 timer 远端放到
	 * 本 CPU；本地入队仍可由当前锁内/回调返回后的重算完整吸收。
	 */
	cpu_base->expires_next = KTIME_MAX;

	if (!ktime_before(now, cpu_base->softirq_expires_next)) {
		cpu_base->softirq_expires_next = KTIME_MAX;
		cpu_base->softirq_activated = true;
		raise_timer_softirq(HRTIMER_SOFTIRQ);
	}

	__hrtimer_run_queues(cpu_base, now, flags, HRTIMER_ACTIVE_HARD);

	/*
	 * The next timer was already expired due to:
	 * - tracing
	 * - long lasting callbacks
	 * - being scheduled away when running in a VM
	 *
	 * We need to prevent that we loop forever in the hrtiner interrupt
	 * routine. We give it 3 attempts to avoid overreacting on some
	 * spurious event.
	 */
	/*
	 * 中文说明：中断处理本身消耗时间，下一期限可能在处理期间又过去。三次重试
	 * 兼容偶发抖动；仍失败则记录最大 hang 与次数，并用保护性延迟打破无限循环。
	 */
	now = hrtimer_update_base(cpu_base);
	expires_next = hrtimer_update_next_event(cpu_base);
	cpu_base->hang_detected = false;
	if (expires_next < now) {
		if (++retries < 3)
			goto retry;

		delta = ktime_sub(now, entry_time);
		cpu_base->max_hang_time = max_t(unsigned int, cpu_base->max_hang_time, delta);
		cpu_base->nr_hangs++;
		cpu_base->hang_detected = true;
	}

	hrtimer_interrupt_rearm(cpu_base, expires_next);
	raw_spin_unlock_irqrestore(&cpu_base->lock, flags);
}

#endif /* !CONFIG_HIGH_RES_TIMERS */

/*
 * Called from run_local_timers in hardirq context every jiffy
 */
/*
 * hrtimer_run_queues() - 低分辨率模式下每个 jiffy 驱动 hrtimer。
 *
 * 无入参/返回，hardirq 上下文。若当前 CPU 已 highres 则独立设备中断负责。
 * 否则先检查能否切换 oneshot/highres，再刷新 offset、raise soft timer 并执行
 * hard timer。锁在 callback 周围由 __run_hrtimer() 暂时释放。
 */
void hrtimer_run_queues(void)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	unsigned long flags;
	ktime_t now;

	if (hrtimer_hres_active(cpu_base))
		return;

	/*
	 * This _is_ ugly: We have to check periodically, whether we
	 * can switch to highres and / or nohz mode. The clocksource
	 * switch happens with xtime_lock held. Notification from
	 * there only sets the check bit in the tick_oneshot code,
	 * otherwise we might deadlock vs. xtime_lock.
	 */
	/*
	 * clocksource 切换持 xtime_lock，不能从通知路径同步进入会反向取锁的 timer
	 * 逻辑；因此只置 check 位，由周期 tick 在无该锁时完成模式转换。
	 */
	if (tick_check_oneshot_change(!hrtimer_is_hres_enabled())) {
		hrtimer_switch_to_hres();
		return;
	}

	raw_spin_lock_irqsave(&cpu_base->lock, flags);
	now = hrtimer_update_base(cpu_base);

	if (!ktime_before(now, cpu_base->softirq_expires_next)) {
		cpu_base->softirq_expires_next = KTIME_MAX;
		cpu_base->softirq_activated = true;
		raise_timer_softirq(HRTIMER_SOFTIRQ);
	}

	__hrtimer_run_queues(cpu_base, now, flags, HRTIMER_ACTIVE_HARD);
	raw_spin_unlock_irqrestore(&cpu_base->lock, flags);
}

/*
 * Sleep related functions:
 */
/* 以下把 hrtimer callback 与 task 状态机组合为可被信号中断/重启的 nanosleep。 */
/*
 * hrtimer_wakeup() - sleeper timer callback。
 *
 * @timer 嵌入 hrtimer_sleeper；返回 NORESTART。先把 t->task 清 NULL 作为“期限已
 * 到”发布，再唤醒借用 task 指针，使睡眠循环能区分 timer 唤醒与信号/伪唤醒。
 */
static enum hrtimer_restart hrtimer_wakeup(struct hrtimer *timer)
{
	struct hrtimer_sleeper *t = container_of(timer, struct hrtimer_sleeper, timer);
	struct task_struct *task = t->task;

	t->task = NULL;
	if (task)
		wake_up_process(task);

	return HRTIMER_NORESTART;
}

/**
 * hrtimer_sleeper_start_expires - Start a hrtimer sleeper timer
 * @sl:		sleeper to be started
 * @mode:	timer mode abs/rel
 *
 * Wrapper around hrtimer_start_expires() for hrtimer_sleeper based timers
 * to allow PREEMPT_RT to tweak the delivery mode (soft/hardirq context)
 */
/*
 * 中文契约：按 sl->timer 已设置期限启动 sleeper。@sl 栈/调用者拥有，@mode 为
 * ABS/REL。无返回值；若用户期限已过，直接清 task 并恢复 current RUNNING，不在
 * 调用者可能持锁时执行 wakeup callback。RT 上保留 setup 决定的 HARD 属性。
 */
void hrtimer_sleeper_start_expires(struct hrtimer_sleeper *sl, enum hrtimer_mode mode)
{
	/*
	 * Make the enqueue delivery mode check work on RT. If the sleeper
	 * was initialized for hard interrupt delivery, force the mode bit.
	 * This is a special case for hrtimer_sleepers because
	 * __hrtimer_setup_sleeper() determines the delivery mode on RT so the
	 * fiddling with this decision is avoided at the call sites.
	 */
	/* setup 已按调度策略选执行上下文，start 必须携带相同 HARD 位通过一致性检查。 */
	if (IS_ENABLED(CONFIG_PREEMPT_RT) && sl->timer.is_hard)
		mode |= HRTIMER_MODE_HARD;

	/* If already expired, clear the task pointer and set current state to running */
	/* false 表示 timer 未排队；模拟 callback 的完成发布，避免随后 schedule 睡死。 */
	if (!hrtimer_start_expires_user(&sl->timer, mode)) {
		sl->task = NULL;
		__set_current_state(TASK_RUNNING);
	}
}
EXPORT_SYMBOL_GPL(hrtimer_sleeper_start_expires);

/*
 * __hrtimer_setup_sleeper() - 初始化 sleeper timer 并绑定 current task。
 *
 * @sl 由调用者拥有；@clock_id/@mode 定义等待语义。RT 普通任务默认 softirq 以
 * 控制硬中断延迟，RT/DL 任务可用 hardirq 获得低唤醒延迟。无返回值、不启动。
 */
static void __hrtimer_setup_sleeper(struct hrtimer_sleeper *sl, clockid_t clock_id,
				    enum hrtimer_mode mode)
{
	/*
	 * On PREEMPT_RT enabled kernels hrtimers which are not explicitly
	 * marked for hard interrupt expiry mode are moved into soft
	 * interrupt context either for latency reasons or because the
	 * hrtimer callback takes regular spinlocks or invokes other
	 * functions which are not suitable for hard interrupt context on
	 * PREEMPT_RT.
	 *
	 * The hrtimer_sleeper callback is RT compatible in hard interrupt
	 * context, but there is a latency concern: Untrusted userspace can
	 * spawn many threads which arm timers for the same expiry time on
	 * the same CPU. That causes a latency spike due to the wakeup of
	 * a gazillion threads.
	 *
	 * OTOH, privileged real-time user space applications rely on the
	 * low latency of hard interrupt wakeups. If the current task is in
	 * a real-time scheduling class, mark the mode for hard interrupt
	 * expiry.
	 */
	/*
	 * 不可信用户可让大量 sleeper 同时到期；若全在 hardirq 唤醒会造成长中断尖峰。
	 * 但特权 RT/DL 应用依赖硬中断低延迟，因此按 current 调度策略做例外。
	 */
	if (IS_ENABLED(CONFIG_PREEMPT_RT)) {
		if (rt_or_dl_task_policy(current) && !(mode & HRTIMER_MODE_SOFT))
			mode |= HRTIMER_MODE_HARD;
	}

	__hrtimer_setup(&sl->timer, hrtimer_wakeup, clock_id, mode);
	sl->task = current;
}

/**
 * hrtimer_setup_sleeper_on_stack - initialize a sleeper in stack memory
 * @sl:		sleeper to be initialized
 * @clock_id:	the clock to be used
 * @mode:	timer mode abs/rel
 */
/*
 * 中文契约：初始化栈上 @sl 并绑定 current；不启动。调用者离开作用域前必须
 * cancel/destroy 内嵌 timer。clock_id/mode 语义同 hrtimer_setup()。
 */
void hrtimer_setup_sleeper_on_stack(struct hrtimer_sleeper *sl, clockid_t clock_id,
				    enum hrtimer_mode mode)
{
	debug_setup_on_stack(&sl->timer, clock_id, mode);
	__hrtimer_setup_sleeper(sl, clock_id, mode);
}
EXPORT_SYMBOL_GPL(hrtimer_setup_sleeper_on_stack);

/*
 * nanosleep_copyout() - 把中断后的剩余时间复制到 native/compat 用户指针。
 *
 * @restart 保存 ABI 类型与目标指针；@ts 为内核剩余时间。成功复制仍返回
 * -ERESTART_RESTARTBLOCK 交给信号框架决定重启，copy fault 返回 -EFAULT。
 */
int nanosleep_copyout(struct restart_block *restart, struct timespec64 *ts)
{
	switch(restart->nanosleep.type) {
#ifdef CONFIG_COMPAT_32BIT_TIME
	case TT_COMPAT:
		if (put_old_timespec32(ts, restart->nanosleep.compat_rmtp))
			return -EFAULT;
		break;
#endif
	case TT_NATIVE:
		if (put_timespec64(ts, restart->nanosleep.rmtp))
			return -EFAULT;
		break;
	default:
		BUG();
	}
	return -ERESTART_RESTARTBLOCK;
}

/*
 * do_nanosleep() - 驱动 sleeper 的“设状态、启动、调度、取消、判完成”循环。
 *
 * @t 为栈上 sleeper，@mode 首轮可 REL，重试后改 ABS 防止伪唤醒累计延长。
 * 返回 0 表示期限到；信号中断返回 restart errno，并在需要时 copy 剩余时间。
 * 每轮 cancel 保证没有 callback 留在栈对象之外运行。
 */
static int __sched do_nanosleep(struct hrtimer_sleeper *t, enum hrtimer_mode mode)
{
	struct restart_block *restart;

	do {
		/*
		 * 先发布可中断/可冻结状态，再启动 timer，封住“timer 先到而任务随后睡下”
		 * 的 lost wakeup；已过期路径会把状态恢复 RUNNING。
		 */
		set_current_state(TASK_INTERRUPTIBLE|TASK_FREEZABLE);
		hrtimer_sleeper_start_expires(t, mode);

		if (likely(t->task))
			schedule();

		/* 返回后同步取消，区分 timer 已清 task 与信号仍保留 task。 */
		hrtimer_cancel(&t->timer);
		mode = HRTIMER_MODE_ABS;

	} while (t->task && !signal_pending(current));

	/* 所有出口恢复 RUNNING；调度状态不能泄漏给系统调用返回路径。 */
	__set_current_state(TASK_RUNNING);

	if (!t->task)
		return 0;

	restart = &current->restart_block;
	if (restart->nanosleep.type != TT_NONE) {
		ktime_t rem = hrtimer_expires_remaining(&t->timer);
		struct timespec64 rmt;

		if (rem <= 0)
			return 0;
		rmt = ktime_to_timespec64(rem);

		return nanosleep_copyout(restart, &rmt);
	}
	return -ERESTART_RESTARTBLOCK;
}

/*
 * hrtimer_nanosleep_restart() - 信号处理后按保存的绝对期限恢复 nanosleep。
 *
 * @restart 属于 current；返回 do_nanosleep 结果。栈上 sleeper 在所有出口销毁，
 * 使用 ABS 防止信号处理时间被重复加入剩余等待。
 */
static long __sched hrtimer_nanosleep_restart(struct restart_block *restart)
{
	struct hrtimer_sleeper t;
	int ret;

	hrtimer_setup_sleeper_on_stack(&t, restart->nanosleep.clockid, HRTIMER_MODE_ABS);
	hrtimer_set_expires(&t.timer, restart->nanosleep.expires);
	ret = do_nanosleep(&t, HRTIMER_MODE_ABS);
	destroy_hrtimer_on_stack(&t.timer);
	return ret;
}

/*
 * hrtimer_nanosleep() - 通用高分辨率睡眠实现。
 *
 * @rqtp 是 mode 对应的期限/时长；@clockid 选择时间坐标。返回 0、用户 copy errno
 * 或 restart errno。timer slack 允许内核合并唤醒。相对等待被信号中断时保存绝对
 * expires 和 restart 函数；绝对等待按 POSIX 不写 rmtp，也不自动 restart。
 */
long hrtimer_nanosleep(ktime_t rqtp, const enum hrtimer_mode mode, const clockid_t clockid)
{
	struct restart_block *restart;
	struct hrtimer_sleeper t;
	int ret;

	hrtimer_setup_sleeper_on_stack(&t, clockid, mode);
	hrtimer_set_expires_range_ns(&t.timer, rqtp, current->timer_slack_ns);
	ret = do_nanosleep(&t, mode);
	if (ret != -ERESTART_RESTARTBLOCK)
		goto out;

	/* Absolute timers do not update the rmtp value and restart: */
	/* 绝对 sleep 的 deadline 不需剩余值；信号处理完成后由用户重新发起。 */
	if (mode == HRTIMER_MODE_ABS) {
		ret = -ERESTARTNOHAND;
		goto out;
	}

	restart = &current->restart_block;
	restart->nanosleep.clockid = t.timer.base->clockid;
	restart->nanosleep.expires = hrtimer_get_expires(&t.timer);
	set_restart_fn(restart, hrtimer_nanosleep_restart);
out:
	destroy_hrtimer_on_stack(&t.timer);
	return ret;
}

#ifdef CONFIG_64BIT

/*
 * nanosleep syscall（64 位 time ABI）：从用户复制相对 timespec，校验非负/规范化，
 * 设置 restart_block 的 native 剩余时间指针，再使用 MONOTONIC sleeper。返回
 * 0 或 -EFAULT/-EINVAL/restart errno；用户指针始终只通过 uaccess helper 访问。
 */
SYSCALL_DEFINE2(nanosleep, struct __kernel_timespec __user *, rqtp,
		struct __kernel_timespec __user *, rmtp)
{
	struct timespec64 tu;

	if (get_timespec64(&tu, rqtp))
		return -EFAULT;

	if (!timespec64_valid(&tu))
		return -EINVAL;

	current->restart_block.fn = do_no_restart_syscall;
	current->restart_block.nanosleep.type = rmtp ? TT_NATIVE : TT_NONE;
	current->restart_block.nanosleep.rmtp = rmtp;
	return hrtimer_nanosleep(timespec64_to_ktime(tu), HRTIMER_MODE_REL, CLOCK_MONOTONIC);
}

#endif

#ifdef CONFIG_COMPAT_32BIT_TIME

/* 32 位 time ABI 版本：流程相同，但用 old_timespec32 copy helper 与 compat 指针。 */
SYSCALL_DEFINE2(nanosleep_time32, struct old_timespec32 __user *, rqtp,
		struct old_timespec32 __user *, rmtp)
{
	struct timespec64 tu;

	if (get_old_timespec32(&tu, rqtp))
		return -EFAULT;

	if (!timespec64_valid(&tu))
		return -EINVAL;

	current->restart_block.fn = do_no_restart_syscall;
	current->restart_block.nanosleep.type = rmtp ? TT_COMPAT : TT_NONE;
	current->restart_block.nanosleep.compat_rmtp = rmtp;
	return hrtimer_nanosleep(timespec64_to_ktime(tu), HRTIMER_MODE_REL, CLOCK_MONOTONIC);
}
#endif

/*
 * Functions related to boot-time initialization:
 */
/* 以下建立 per-CPU base，并在 CPU hotplug 时迁移仍 pending 的高分辨率 timer。 */
/*
 * hrtimers_prepare_cpu() - 初始化 possible @cpu 的 clock_base 队列和同步对象。
 *
 * hotplug prepare 阶段无并发用户；返回 0。每个 base 建立 cpu_base 反向指针、
 * 与 raw lock 关联的 seqcount 和空 timerqueue；再初始化 CPU 身份与 RT expiry 锁。
 */
int hrtimers_prepare_cpu(unsigned int cpu)
{
	struct hrtimer_cpu_base *cpu_base = &per_cpu(hrtimer_bases, cpu);

	for (int i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++) {
		struct hrtimer_clock_base *clock_b = &cpu_base->clock_base[i];

		clock_b->cpu_base = cpu_base;
		seqcount_raw_spinlock_init(&clock_b->seq, &cpu_base->lock);
		timerqueue_linked_init_head(&clock_b->active);
	}

	cpu_base->cpu = cpu;
	hrtimer_cpu_base_init_expiry_lock(cpu_base);
	return 0;
}

/*
 * hrtimers_cpu_starting() - 清理一次旧下线遗留状态并发布当前 CPU online。
 *
 * @cpu 由 hotplug 框架传入，实际操作 this_cpu base；返回 0。树已在 prepare 阶段
 * 初始化/迁空，此处重置所有派生缓存、hang 和 highres 状态。
 */
int hrtimers_cpu_starting(unsigned int cpu)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);

	/* Clear out any left over state from a CPU down operation */
	/* online 最后置 true，避免其他 CPU 在缓存尚未重置时选择该 base。 */
	cpu_base->active_bases = 0;
	cpu_base->hres_active = false;
	cpu_base->hang_detected = false;
	cpu_base->next_timer = NULL;
	cpu_base->softirq_next_timer = NULL;
	cpu_base->expires_next = KTIME_MAX;
	cpu_base->softirq_expires_next = KTIME_MAX;
	cpu_base->softirq_activated = false;
	cpu_base->online = true;
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU

/*
 * migrate_hrtimer_list() - 把同 clockid 的整棵旧队列逐 timer 迁入新 CPU。
 *
 * 新旧 cpu_base 锁均持有；timer 对象 ownership 不变。迁移期间保持 ENQUEUED，
 * 改 base 后重新入树；硬件暂不编程，全部迁完由目标 CPU retrigger 统一处理。
 */
static void migrate_hrtimer_list(struct hrtimer_clock_base *old_base,
				struct hrtimer_clock_base *new_base)
{
	struct timerqueue_linked_node *node;
	struct hrtimer *timer;

	while ((node = timerqueue_linked_first(&old_base->active))) {
		timer = hrtimer_from_timerqueue_node(node);
		BUG_ON(hrtimer_callback_running(timer));
		debug_hrtimer_deactivate(timer);

		/*
		 * Mark it as ENQUEUED not INACTIVE otherwise the
		 * timer could be seen as !active and just vanish away
		 * under us on another CPU
		 */
		/* 若中间标 inactive，无锁观察者可能据此释放容器，迁移者随后产生 UAF。 */
		__remove_hrtimer(timer, old_base, HRTIMER_STATE_ENQUEUED, false);
		timer->base = new_base;
		/*
		 * Enqueue the timers on the new cpu. This does not
		 * reprogram the event device in case the timer
		 * expires before the earliest on this CPU, but we run
		 * hrtimer_interrupt after we migrated everything to
		 * sort out already expired timers and reprogram the
		 * event device.
		 */
		/*
		 * 批量阶段不逐个写 clockevent；最终 IPI 在完整目标树上执行过期 timer 并
		 * 一次性编程真实首事件，避免 O(n) 设备操作。
		 */
		enqueue_hrtimer(timer, new_base, HRTIMER_MODE_ABS, true);
	}
}

/*
 * hrtimers_cpu_dying() - 将 dying CPU 全部 hrtimer 迁到在线 housekeeping CPU。
 *
 * hotplug 全局串行；返回 0。按规定嵌套锁旧/新 base，逐 clockid 迁移，向目标 CPU
 * 同步 retrigger 后发布 old_base offline。callback 此时不得 running。
 */
int hrtimers_cpu_dying(unsigned int dying_cpu)
{
	int ncpu = cpumask_any_and(cpu_active_mask, housekeeping_cpumask(HK_TYPE_TIMER));
	struct hrtimer_cpu_base *old_base, *new_base;

	old_base = this_cpu_ptr(&hrtimer_bases);
	new_base = &per_cpu(hrtimer_bases, ncpu);

	/*
	 * The caller is globally serialized and nobody else
	 * takes two locks at once, deadlock is not possible.
	 */
	/* 普通路径一次只持一颗 cpu_base 锁，hotplug 串行保证这里的双锁不会 ABBA。 */
	raw_spin_lock(&old_base->lock);
	raw_spin_lock_nested(&new_base->lock, SINGLE_DEPTH_NESTING);

	for (int i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++)
		migrate_hrtimer_list(&old_base->clock_base[i], &new_base->clock_base[i]);

	/* Tell the other CPU to retrigger the next event */
	/* 同步 IPI 确保返回前目标已观察完整迁移树并更新本地 clockevent。 */
	smp_call_function_single(ncpu, retrigger_next_event, NULL, 0);

	raw_spin_unlock(&new_base->lock);
	old_base->online = false;
	raw_spin_unlock(&old_base->lock);

	return 0;
}

#endif /* CONFIG_HOTPLUG_CPU */

/*
 * hrtimers_init() - 启动 CPU 的 hrtimer 子系统入口。
 *
 * 无入参/返回，仅 init 阶段。先 prepare 队列/锁，再发布 CPU online，最后注册
 * HRTIMER_SOFTIRQ，使 soft 类 timer 拥有执行入口。
 */
void __init hrtimers_init(void)
{
	hrtimers_prepare_cpu(smp_processor_id());
	hrtimers_cpu_starting(smp_processor_id());
	open_softirq(HRTIMER_SOFTIRQ, hrtimer_run_softirq);
}
