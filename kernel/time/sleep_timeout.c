// SPDX-License-Identifier: GPL-2.0
/*
 *  Kernel internal schedule timeout and sleeping functions
 */
/*
 * 本文件提供两类可调度睡眠：timer wheel/jiffies 路径返回剩余 tick，hrtimer 路径以绝对/相对 ktime 和
 * slack 返回到期或提前唤醒状态；msleep/usleep_range 在其上封装“不因无关显式唤醒而缩短最小时长”的循环。
 * 所有接口都要求可睡眠进程上下文，并依赖调用者选择 TASK_* 状态决定信号/负载统计语义。
 */

#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/sched/signal.h>
#include <linux/sched/debug.h>

#include "tick-internal.h"

/*
 * Since schedule_timeout()'s timer is defined on the stack, it must store
 * the target task on the stack as well.
 */
/*
 * `process_timer` 与调用者栈同寿命：timer wheel 节点和目标 task 指针一起位于栈上，callback 不需要另找
 * 外部容器。schedule_timeout 返回前必须同步删除 timer，确保 callback 不再访问已失效栈/task 字段。
 */
struct process_timer {
	struct timer_list timer;
	struct task_struct *task;
};

/*
 * process_timeout() - jiffy timeout 到期时唤醒其栈上容器记录的目标任务。
 * @t 是 timer wheel 传入、仍有效的嵌入 timer；timeout 由 container helper 恢复，task 是借用指针。
 * callback 运行于 timer softirq/原子上下文，不睡眠、无返回值；只调用 wake_up_process，不释放 timer/task。
 */
static void process_timeout(struct timer_list *t)
{
	struct process_timer *timeout = timer_container_of(timeout, t, timer);

	wake_up_process(timeout->task);
}

/**
 * schedule_timeout - sleep until timeout
 * @timeout: timeout value in jiffies
 *
 * Make the current task sleep until @timeout jiffies have elapsed.
 * The function behavior depends on the current task state
 * (see also set_current_state() description):
 *
 * %TASK_RUNNING - the scheduler is called, but the task does not sleep
 * at all. That happens because sched_submit_work() does nothing for
 * tasks in %TASK_RUNNING state.
 *
 * %TASK_UNINTERRUPTIBLE - at least @timeout jiffies are guaranteed to
 * pass before the routine returns unless the current task is explicitly
 * woken up, (e.g. by wake_up_process()).
 *
 * %TASK_INTERRUPTIBLE - the routine may return early if a signal is
 * delivered to the current task or the current task is explicitly woken
 * up.
 *
 * The current task state is guaranteed to be %TASK_RUNNING when this
 * routine returns.
 *
 * Specifying a @timeout value of %MAX_SCHEDULE_TIMEOUT will schedule
 * the CPU away without a bound on the timeout. In this case the return
 * value will be %MAX_SCHEDULE_TIMEOUT.
 *
 * Returns: 0 when the timer has expired otherwise the remaining time in
 * jiffies will be returned. In all cases the return value is guaranteed
 * to be non-negative.
 */
/*
 * schedule_timeout() - 按 current 预先设置的任务状态最多睡眠 @timeout 个 jiffy，并返回剩余值。
 * @timeout=MAX_SCHEDULE_TIMEOUT 时不建 timer，只 `schedule()` 一次并原值返回；负值是调用错误，打印栈、
 * 强制 TASK_RUNNING 并返回 0。有限非负值计算绝对 expire，在当前栈构造 timer/task 容器后入 wheel，
 * 无条件 schedule；到期 callback 或其他 wakeup 都可使其返回。随后同步删 timer、销毁栈对象跟踪，按
 * `expire-jiffies` 算余量并钳到 >=0。返回时 current 为 TASK_RUNNING；函数可睡眠，不能在原子上下文调用。
 *
 * TASK_RUNNING 会经过调度器但不阻塞；UNINTERRUPTIBLE 忽略普通信号但仍可被显式唤醒；INTERRUPTIBLE
 * 可被信号或显式唤醒。0 返回只能说明到期/已过期，正值表示提前唤醒，不直接编码唤醒原因。
 */
signed long __sched schedule_timeout(signed long timeout)
{
	struct process_timer timer;
	unsigned long expire;

	switch (timeout) {
	case MAX_SCHEDULE_TIMEOUT:
		/*
		 * These two special cases are useful to be comfortable
		 * in the caller. Nothing more. We could take
		 * MAX_SCHEDULE_TIMEOUT from one of the negative value
		 * but I' d like to return a valid offset (>=0) to allow
		 * the caller to do everything it want with the retval.
		 */
		/* 无限哨兵单独保留为合法非负返回值，避免调用者把负数当剩余时间处理。 */
		schedule();
		goto out;
	default:
		/*
		 * Another bit of PARANOID. Note that the retval will be
		 * 0 since no piece of kernel is supposed to do a check
		 * for a negative retval of schedule_timeout() (since it
		 * should never happens anyway). You just have the printk()
		 * that will tell you if something is gone wrong and where.
		 */
		/* 负 timeout 违反接口：告警并恢复 RUNNING，最终统一钳成 0，而不向既有调用者返回负值。 */
		if (timeout < 0) {
			pr_err("%s: wrong timeout value %lx\n", __func__, timeout);
			dump_stack();
			__set_current_state(TASK_RUNNING);
			goto out;
		}
	}

	expire = timeout + jiffies;

	timer.task = current;
	timer_setup_on_stack(&timer.timer, process_timeout, 0);
	timer.timer.expires = expire;
	add_timer(&timer.timer);
	schedule();
	timer_delete_sync(&timer.timer);

	/* Remove the timer from the object tracker */
	/* 同步删除已保证 callback 完成，再撤销 on-stack 调试跟踪，栈容器才可安全离开作用域。 */
	timer_destroy_on_stack(&timer.timer);

	timeout = expire - jiffies;

 out:
	return timeout < 0 ? 0 : timeout;
}
EXPORT_SYMBOL(schedule_timeout);

/*
 * __set_current_state() can be used in schedule_timeout_*() functions, because
 * schedule_timeout() calls schedule() unconditionally.
 */
/* 这些包装可用较弱的 __set_current_state：紧随其后的 schedule_timeout 必定调用 schedule，已形成所需顺序。 */

/**
 * schedule_timeout_interruptible - sleep until timeout (interruptible)
 * @timeout: timeout value in jiffies
 *
 * See schedule_timeout() for details.
 *
 * Task state is set to TASK_INTERRUPTIBLE before starting the timeout.
 */
/*
 * schedule_timeout_interruptible() - 把 current 置 TASK_INTERRUPTIBLE 后执行 jiffy timeout。
 * @timeout 原样传给核心；信号、显式 wakeup 或到期均可返回，结果是非负剩余 jiffies。函数可睡眠，
 * 返回时为 TASK_RUNNING；调用者需结合 signal_pending/current 条件区分是否继续等待。
 */
signed long __sched schedule_timeout_interruptible(signed long timeout)
{
	__set_current_state(TASK_INTERRUPTIBLE);
	return schedule_timeout(timeout);
}
EXPORT_SYMBOL(schedule_timeout_interruptible);

/**
 * schedule_timeout_killable - sleep until timeout (killable)
 * @timeout: timeout value in jiffies
 *
 * See schedule_timeout() for details.
 *
 * Task state is set to TASK_KILLABLE before starting the timeout.
 */
/*
 * schedule_timeout_killable() - 以 TASK_KILLABLE 状态执行 jiffy timeout。
 * @timeout 原样传入；致命信号、显式 wakeup 或到期可结束睡眠，返回非负剩余 jiffies。相比 interruptible，
 * 非致命信号不应提前结束；可睡眠且返回时恢复 TASK_RUNNING。
 */
signed long __sched schedule_timeout_killable(signed long timeout)
{
	__set_current_state(TASK_KILLABLE);
	return schedule_timeout(timeout);
}
EXPORT_SYMBOL(schedule_timeout_killable);

/**
 * schedule_timeout_uninterruptible - sleep until timeout (uninterruptible)
 * @timeout: timeout value in jiffies
 *
 * See schedule_timeout() for details.
 *
 * Task state is set to TASK_UNINTERRUPTIBLE before starting the timeout.
 */
/*
 * schedule_timeout_uninterruptible() - 以 TASK_UNINTERRUPTIBLE 状态执行 jiffy timeout。
 * @timeout 原样传入；普通信号不提前唤醒，但 timer 到期或显式 wake_up_process 可以，返回非负剩余 jiffies。
 * 函数可睡眠，等待期间计入不可中断负载，返回时恢复 TASK_RUNNING。
 */
signed long __sched schedule_timeout_uninterruptible(signed long timeout)
{
	__set_current_state(TASK_UNINTERRUPTIBLE);
	return schedule_timeout(timeout);
}
EXPORT_SYMBOL(schedule_timeout_uninterruptible);

/**
 * schedule_timeout_idle - sleep until timeout (idle)
 * @timeout: timeout value in jiffies
 *
 * See schedule_timeout() for details.
 *
 * Task state is set to TASK_IDLE before starting the timeout. It is similar to
 * schedule_timeout_uninterruptible(), except this task will not contribute to
 * load average.
 */
/*
 * schedule_timeout_idle() - 以 TASK_IDLE 状态执行 jiffy timeout，避免等待任务计入 load average。
 * @timeout 原样传入；信号语义类似 uninterruptible，timer/显式 wakeup 可结束，返回非负剩余 jiffies。
 * 函数可睡眠，返回时恢复 TASK_RUNNING；适合可忽略负载贡献的后台等待。
 */
signed long __sched schedule_timeout_idle(signed long timeout)
{
	__set_current_state(TASK_IDLE);
	return schedule_timeout(timeout);
}
EXPORT_SYMBOL(schedule_timeout_idle);

/**
 * schedule_hrtimeout_range_clock - sleep until timeout
 * @expires:	timeout value (ktime_t)
 * @delta:	slack in expires timeout (ktime_t)
 * @mode:	timer mode
 * @clock_id:	timer clock to be used
 *
 * Details are explained in schedule_hrtimeout_range() function description as
 * this function is commonly used.
 */
/*
 * schedule_hrtimeout_range_clock() - 用指定 clock 的栈上 hrtimer_sleeper 执行一次可提前唤醒的高精度等待。
 * @expires 可空：非空指向相对/绝对 ktime（由 @mode 决定），NULL 表示无 timer 的无限单次 schedule；
 * @delta 是最晚硬到期相对 soft expiry 的纳秒 slack；@clock_id 选择时钟。调用者已设置 current TASK_*。
 *
 * `*expires==0` 不论 mode 都恢复 RUNNING 并返回 0；NULL 只 schedule 一次后返回 -EINTR。其他路径初始化
 * 栈 sleeper、设 soft/hard expiry 并启动；期限已过时 t.task 已清而不 schedule。正常入睡后，timer callback
 * 清 task 表示到期，信号/显式 wakeup 则通常保留 task。返回前总 cancel/destroy 栈 timer 并恢复 RUNNING；
 * task 为 NULL 返回 0，否则 -EINTR。函数可睡眠，返回值不提供剩余时间，竞态以最终 task 标记为准。
 */
int __sched schedule_hrtimeout_range_clock(ktime_t *expires, u64 delta,
					   const enum hrtimer_mode mode, clockid_t clock_id)
{
	struct hrtimer_sleeper t;

	/*
	 * Optimize when a zero timeout value is given. It does not
	 * matter whether this is an absolute or a relative time.
	 */
	/* 零期限立即完成，避免创建栈 timer；相对 0 与绝对 0 在这里都视作已到期。 */
	if (expires && *expires == 0) {
		__set_current_state(TASK_RUNNING);
		return 0;
	}

	/*
	 * A NULL parameter means "infinite"
	 */
	/* NULL 不武装 timer，只依赖外部 wakeup；函数被唤醒后以 -EINTR 表示并非自身期限到达。 */
	if (!expires) {
		schedule();
		return -EINTR;
	}

	hrtimer_setup_sleeper_on_stack(&t, clock_id, mode);
	hrtimer_set_expires_range_ns(&t.timer, *expires, delta);
	hrtimer_sleeper_start_expires(&t, mode);

	if (likely(t.task))
		schedule();

	hrtimer_cancel(&t.timer);
	destroy_hrtimer_on_stack(&t.timer);

	__set_current_state(TASK_RUNNING);

	return !t.task ? 0 : -EINTR;
}
EXPORT_SYMBOL_GPL(schedule_hrtimeout_range_clock);

/**
 * schedule_hrtimeout_range - sleep until timeout
 * @expires:	timeout value (ktime_t)
 * @delta:	slack in expires timeout (ktime_t)
 * @mode:	timer mode
 *
 * Make the current task sleep until the given expiry time has
 * elapsed. The routine will return immediately unless
 * the current task state has been set (see set_current_state()).
 *
 * The @delta argument gives the kernel the freedom to schedule the
 * actual wakeup to a time that is both power and performance friendly
 * for regular (non RT/DL) tasks.
 * The kernel give the normal best effort behavior for "@expires+@delta",
 * but may decide to fire the timer earlier, but no earlier than @expires.
 *
 * You can set the task state as follows -
 *
 * %TASK_UNINTERRUPTIBLE - at least @timeout time is guaranteed to
 * pass before the routine returns unless the current task is explicitly
 * woken up, (e.g. by wake_up_process()).
 *
 * %TASK_INTERRUPTIBLE - the routine may return early if a signal is
 * delivered to the current task or the current task is explicitly woken
 * up.
 *
 * The current task state is guaranteed to be TASK_RUNNING when this
 * routine returns.
 *
 * Returns: 0 when the timer has expired. If the task was woken before the
 * timer expired by a signal (only possible in state TASK_INTERRUPTIBLE) or
 * by an explicit wakeup, it returns -EINTR.
 */
/*
 * schedule_hrtimeout_range() - 在 CLOCK_MONOTONIC 上执行带 slack 的高精度睡眠。
 * @expires/@delta/@mode 原样交给通用 clock 版本；普通任务可在 [expires, expires+delta] 合并唤醒以省电，
 * 但不会早于 soft expiry。调用者须先设 TASK_*；返回 0 表示 timer 到期，-EINTR 表示信号/显式提前唤醒，
 * 返回时 TASK_RUNNING。NULL、零期限和相对/绝对语义完全继承核心函数。
 */
int __sched schedule_hrtimeout_range(ktime_t *expires, u64 delta,
				     const enum hrtimer_mode mode)
{
	return schedule_hrtimeout_range_clock(expires, delta, mode,
					      CLOCK_MONOTONIC);
}
EXPORT_SYMBOL_GPL(schedule_hrtimeout_range);

/**
 * schedule_hrtimeout - sleep until timeout
 * @expires:	timeout value (ktime_t)
 * @mode:	timer mode
 *
 * See schedule_hrtimeout_range() for details. @delta argument of
 * schedule_hrtimeout_range() is set to 0 and has therefore no impact.
 */
/*
 * schedule_hrtimeout() - 在 CLOCK_MONOTONIC 上执行无 slack 的高精度睡眠。
 * @expires 可空，@mode 指定相对/绝对；等价于 range 版本 delta=0。返回 0 表示到期，-EINTR 表示提前唤醒，
 * 可睡眠且返回时 current 为 TASK_RUNNING。
 */
int __sched schedule_hrtimeout(ktime_t *expires, const enum hrtimer_mode mode)
{
	return schedule_hrtimeout_range(expires, 0, mode);
}
EXPORT_SYMBOL_GPL(schedule_hrtimeout);

/**
 * msleep - sleep safely even with waitqueue interruptions
 * @msecs:	Requested sleep duration in milliseconds
 *
 * msleep() uses jiffy based timeouts for the sleep duration. Because of the
 * design of the timer wheel, the maximum additional percentage delay (slack) is
 * 12.5%. This is only valid for timers which will end up in level 1 or a higher
 * level of the timer wheel. For explanation of those 12.5% please check the
 * detailed description about the basics of the timer wheel.
 *
 * The slack of timers which will end up in level 0 depends on sleep duration
 * (msecs) and HZ configuration and can be calculated in the following way (with
 * the timer wheel design restriction that the slack is not less than 12.5%):
 *
 *   ``slack = MSECS_PER_TICK / msecs``
 *
 * When the allowed slack of the callsite is known, the calculation could be
 * turned around to find the minimal allowed sleep duration to meet the
 * constraints. For example:
 *
 * * ``HZ=1000`` with ``slack=25%``: ``MSECS_PER_TICK / slack = 1 / (1/4) = 4``:
 *   all sleep durations greater or equal 4ms will meet the constraints.
 * * ``HZ=1000`` with ``slack=12.5%``: ``MSECS_PER_TICK / slack = 1 / (1/8) = 8``:
 *   all sleep durations greater or equal 8ms will meet the constraints.
 * * ``HZ=250`` with ``slack=25%``: ``MSECS_PER_TICK / slack = 4 / (1/4) = 16``:
 *   all sleep durations greater or equal 16ms will meet the constraints.
 * * ``HZ=250`` with ``slack=12.5%``: ``MSECS_PER_TICK / slack = 4 / (1/8) = 32``:
 *   all sleep durations greater or equal 32ms will meet the constraints.
 *
 * See also the signal aware variant msleep_interruptible().
 */
/*
 * msleep() - 以 jiffy timer wheel 至少睡满请求毫秒数，并忽略无关显式 wakeup。
 * @msecs 转换为向上取整/饱和的 jiffies；0 转换为 0 时立即返回。循环调用 uninterruptible timeout，把提前
 * wake 返回的剩余 jiffies 再睡完，因此普通信号和伪唤醒不会缩短期限；无返回值，只能在可睡眠上下文调用。
 *
 * timer wheel 进入 level>=1 时额外 slack 最多约 12.5%；level0 受 tick 粒度影响，近似为
 * max(12.5%, MSECS_PER_TICK/msecs)。因此 HZ=1000 要满足 25%/12.5% 分别至少约 4/8ms，HZ=250 则约
 * 16/32ms。需要更紧上界应使用 usleep_range/hrtimer，而非假定 msleep 精确到毫秒。
 */
void msleep(unsigned int msecs)
{
	unsigned long timeout = msecs_to_jiffies(msecs);

	while (timeout)
		timeout = schedule_timeout_uninterruptible(timeout);
}
EXPORT_SYMBOL(msleep);

/**
 * msleep_interruptible - sleep waiting for signals
 * @msecs:	Requested sleep duration in milliseconds
 *
 * See msleep() for some basic information.
 *
 * The difference between msleep() and msleep_interruptible() is that the sleep
 * could be interrupted by a signal delivery and then returns early.
 *
 * Returns: The remaining time of the sleep duration transformed to msecs (see
 * schedule_timeout() for details).
 */
/*
 * msleep_interruptible() - 至少等待请求毫秒期限，除非 current 收到待处理信号。
 * @msecs 先转为 jiffies；循环仅在余量非零且无 signal_pending 时调用 interruptible timeout。显式 wakeup
 * 但无信号会继续睡剩余值，信号则提前退出。返回剩余 jiffies 再转毫秒的近似值，可能受换算舍入；0 表示
 * 期限已耗尽。函数可睡眠，返回时 TASK_RUNNING，调用者另行检查/处理具体信号。
 */
unsigned long msleep_interruptible(unsigned int msecs)
{
	unsigned long timeout = msecs_to_jiffies(msecs);

	while (timeout && !signal_pending(current))
		timeout = schedule_timeout_interruptible(timeout);
	return jiffies_to_msecs(timeout);
}
EXPORT_SYMBOL(msleep_interruptible);

/**
 * usleep_range_state - Sleep for an approximate time in a given state
 * @min:	Minimum time in usecs to sleep
 * @max:	Maximum time in usecs to sleep
 * @state:	State of the current task that will be while sleeping
 *
 * usleep_range_state() sleeps at least for the minimum specified time but not
 * longer than the maximum specified amount of time. The range might reduce
 * power usage by allowing hrtimers to coalesce an already scheduled interrupt
 * with this hrtimer. In the worst case, an interrupt is scheduled for the upper
 * bound.
 *
 * The sleeping task is set to the specified state before starting the sleep.
 *
 * In non-atomic context where the exact wakeup time is flexible, use
 * usleep_range() or its variants instead of udelay(). The sleep improves
 * responsiveness by avoiding the CPU-hogging busy-wait of udelay().
 */
/*
 * usleep_range_state() - 用绝对 monotonic hrtimer 至少等待 @min us，并允许在 @max us 前合并唤醒。
 * @min/@max 是 unsigned 微秒，@state 是每轮 schedule 前写入 current 的 TASK_*。exp 在入口固定为 now+min，
 * delta=(max-min)us；max<min 时 unsigned 差虽先回绕，但 WARN 后立即置 0，仍按 min 无 slack 等待。
 *
 * 循环忽略 schedule_hrtimeout_range 的 -EINTR/显式 wakeup，反复以同一绝对 exp 睡眠，只有 timer 到期返回
 * 0 才结束；因此即使用 INTERRUPTIBLE 状态，信号也不会缩短最小等待，持续 pending 信号可能令循环在
 * min 到达前反复快速重试。hrtimer 最晚目标为入口 now+max，但实际任务重新运行仍可能受调度延迟影响。
 * 函数可睡眠、无返回值，离开时 TASK_RUNNING；用于可容忍范围的短等待，比 udelay 忙等更友好。
 */
void __sched usleep_range_state(unsigned long min, unsigned long max, unsigned int state)
{
	ktime_t exp = ktime_add_us(ktime_get(), min);
	u64 delta = (u64)(max - min) * NSEC_PER_USEC;

	if (WARN_ON_ONCE(max < min))
		delta = 0;

	for (;;) {
		__set_current_state(state);
		/* Do not return before the requested sleep time has elapsed */
		/* 提前 wakeup/-EINTR 只重试同一绝对期限，保证不会在 @min 前返回。 */
		if (!schedule_hrtimeout_range(&exp, delta, HRTIMER_MODE_ABS))
			break;
	}
}
EXPORT_SYMBOL(usleep_range_state);
