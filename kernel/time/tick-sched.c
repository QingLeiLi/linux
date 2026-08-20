// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright(C) 2005-2006, Linutronix GmbH, Thomas Gleixner <tglx@kernel.org>
 *  Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 *  Copyright(C) 2006-2007  Timesys Corp., Thomas Gleixner
 *
 *  NOHZ implementation for low and high resolution timers
 *
 *  Started by: Thomas Gleixner and Ingo Molnar
 */
/*
 * 本文件用每 CPU tick_sched 把周期调度 tick 仿真、NO_HZ idle/full dynticks、jiffies/timekeeping 负责人和
 * highres/lowres 两种底层串成一个状态机。核心不变量是：全局 jiffies 只在 jiffies_lock/seq 下推进；每 CPU
 * flags 在 IRQ-off 本地语境修改；STOPPED 后任何 timer、依赖或 IRQ 变化都必须重算 deadline 或恢复周期 tick。
 * highres 用 pinned hard hrtimer，lowres 直接重编 clockevent；二者共享 sched_timer 中保存的时间线。
 */
#include <linux/compiler.h>
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/percpu.h>
#include <linux/nmi.h>
#include <linux/profile.h>
#include <linux/sched/signal.h>
#include <linux/sched/clock.h>
#include <linux/sched/stat.h>
#include <linux/sched/nohz.h>
#include <linux/sched/loadavg.h>
#include <linux/module.h>
#include <linux/irq_work.h>
#include <linux/posix-timers.h>
#include <linux/context_tracking.h>
#include <linux/mm.h>

#include <asm/irq_regs.h>

#include "tick-internal.h"

#include <trace/events/timer.h>

/*
 * Per-CPU nohz control structure
 */
static DEFINE_PER_CPU(struct tick_sched, tick_cpu_sched);
/* tick_cpu_sched 由 CPU 生命周期长期拥有；getter 只借出地址，跨 CPU 读写仍需 hotplug/锁或无锁字段契约。 */

/* tick_get_tick_sched() 返回 @cpu 的 per-CPU 控制块借用指针；不校验编号、不固定 CPU online 状态、不增引用。 */
struct tick_sched *tick_get_tick_sched(int cpu)
{
	return &per_cpu(tick_cpu_sched, cpu);
}

/*
 * The time when the last jiffy update happened. Write access must hold
 * jiffies_lock and jiffies_seq. tick_nohz_next_event() needs to get a
 * consistent view of jiffies and last_jiffies_update.
 */
static ktime_t last_jiffies_update;
/* last_jiffies_update 是已提交边界，tick_next_period 是下一周期边界；二者与 jiffies 构成同一 seqcount 快照。 */

/*
 * Must be called with interrupts disabled !
 */
/*
 * tick_do_update_jiffies64() - 把全局 tick 时间线追赶到 monotonic @now。
 * IRQ 必须关闭。64 位先 acquire 读 tick_next_period，32 位用 jiffies_seq 防撕裂；未到期无锁返回。慢路径持
 * jiffies_lock 后复查，按跨过的 TICK_NSEC 数批量推进 last_jiffies_update/jiffies_64，发布下一周期，再在
 * seq 外、锁内更新 load。解锁后推进 wall/aux timekeeper。多 CPU 竞争由锁复查消除；无返回值且不改 per-CPU 状态。
 */
static void tick_do_update_jiffies64(ktime_t now)
{
	unsigned long ticks = 1;
	ktime_t delta, nextp;

	/*
	 * 64-bit can do a quick check without holding the jiffies lock and
	 * without looking at the sequence count. The smp_load_acquire()
	 * pairs with the update done later in this function.
	 *
	 * 32-bit cannot do that because the store of 'tick_next_period'
	 * consists of two 32-bit stores, and the first store could be
	 * moved by the CPU to a random point in the future.
	 */
	if (IS_ENABLED(CONFIG_64BIT)) {
		if (ktime_before(now, smp_load_acquire(&tick_next_period)))
			return;
	} else {
		unsigned int seq;

		/*
		 * Avoid contention on 'jiffies_lock' and protect the quick
		 * check with the sequence count.
		 */
		/* 32 位先用 seqcount 读取完整 64 位 next_period，避免为未到期快路争用全局 raw lock。 */
		do {
			seq = read_seqcount_begin(&jiffies_seq);
			nextp = tick_next_period;
		} while (read_seqcount_retry(&jiffies_seq, seq));

		if (ktime_before(now, nextp))
			return;
	}

	/* Quick check failed, i.e. update is required. */
	/* 快检只表示可能到期，真正提交前仍必须在锁内复核。 */
	raw_spin_lock(&jiffies_lock);
	/*
	 * Re-evaluate with the lock held. Another CPU might have done the
	 * update already.
	 */
	/* 等锁期间其他 CPU 可能已推进；锁内早于新 next_period 就直接退出。 */
	if (ktime_before(now, tick_next_period)) {
		raw_spin_unlock(&jiffies_lock);
		return;
	}

	write_seqcount_begin(&jiffies_seq);

	delta = ktime_sub(now, tick_next_period);
	if (unlikely(delta >= TICK_NSEC)) {
		/* Slow path for long idle sleep times */
		/* 长 idle 一次计算跨过的完整周期数，避免逐 tick 循环。 */
		s64 incr = TICK_NSEC;

		ticks += ktime_divns(delta, incr);

		last_jiffies_update = ktime_add_ns(last_jiffies_update,
						   incr * ticks);
	} else {
		last_jiffies_update = ktime_add_ns(last_jiffies_update,
						   TICK_NSEC);
	}

	/* Advance jiffies to complete the 'jiffies_seq' protected job */
	/* jiffies 与两个时间锚点必须在同一 seq 写事务中对读者同时可见。 */
	jiffies_64 += ticks;

	/* Keep the tick_next_period variable up to date */
	/* 下一周期恒等于最新已提交边界加一个 TICK_NSEC。 */
	nextp = ktime_add_ns(last_jiffies_update, TICK_NSEC);

	if (IS_ENABLED(CONFIG_64BIT)) {
		/*
		 * Pairs with smp_load_acquire() in the lockless quick
		 * check above, and ensures that the update to 'jiffies_64' is
		 * not reordered vs. the store to 'tick_next_period', neither
		 * by the compiler nor by the CPU.
		 */
		/* release 与 64 位快路 acquire 配对，保证看到新期限的读者也看到此前 jiffies_64 更新。 */
		smp_store_release(&tick_next_period, nextp);
	} else {
		/*
		 * A plain store is good enough on 32-bit, as the quick check
		 * above is protected by the sequence count.
		 */
		/* 32 位读者依赖 seq 重试获得整体快照，因此写侧普通赋值已足够。 */
		tick_next_period = nextp;
	}

	/*
	 * Release the sequence count. calc_global_load() below is not
	 * protected by it, but 'jiffies_lock' needs to be held to prevent
	 * concurrent invocations.
	 */
	/* seq 只覆盖时间快照；global load 仍留在 jiffies_lock 内串行，但不要求读者把它纳入同一版本。 */
	write_seqcount_end(&jiffies_seq);

	calc_global_load();

	raw_spin_unlock(&jiffies_lock);
	update_wall_time();
}

/*
 * Initialize and return retrieve the jiffies update.
 */
/*
 * tick_init_jiffy_update() - 初始化或读取 jiffies 周期锚点。
 * 在 jiffies_lock+seq 写事务内，首次调用把全局 tick_next_period 向上对齐 TICK_NSEC 并写
 * last_jiffies_update；后续只返回已有锚点。返回 monotonic ktime，供每 CPU sched_timer 建立同源时间线。
 */
static ktime_t tick_init_jiffy_update(void)
{
	ktime_t period;

	raw_spin_lock(&jiffies_lock);
	write_seqcount_begin(&jiffies_seq);

	/* Have we started the jiffies update yet ? */
	/* 零值只在首次初始化成立；一旦设定，后续 CPU 复用同一全局锚点。 */
	if (last_jiffies_update == 0) {
		u32 rem;

		/*
		 * Ensure that the tick is aligned to a multiple of
		 * TICK_NSEC.
		 */
		/* 向上而非向下对齐，避免把首次周期期限设到当前锚点之前。 */
		div_u64_rem(tick_next_period, TICK_NSEC, &rem);
		if (rem)
			tick_next_period += TICK_NSEC - rem;

		last_jiffies_update = tick_next_period;
	}
	period = last_jiffies_update;

	write_seqcount_end(&jiffies_seq);
	raw_spin_unlock(&jiffies_lock);

	return period;
}

/* tick_sched_flag_test() 无锁读取 @ts->flags 指定位并规范成 0/1；调用者负责所需 CPU/IRQ 稳定性。 */
static inline int tick_sched_flag_test(struct tick_sched *ts,
				       unsigned long flag)
{
	return !!(ts->flags & flag);
}

/* tick_sched_flag_set() 要求 IRQ 已关闭后本地 OR @flag；不提供跨 CPU 原子性或内存屏障。 */
static inline void tick_sched_flag_set(struct tick_sched *ts,
				       unsigned long flag)
{
	lockdep_assert_irqs_disabled();
	ts->flags |= flag;
}

/* tick_sched_flag_clear() 要求 IRQ 已关闭后清 @flag；与 set 一样只适合本 CPU 串行修改。 */
static inline void tick_sched_flag_clear(struct tick_sched *ts,
					 unsigned long flag)
{
	lockdep_assert_irqs_disabled();
	ts->flags &= ~flag;
}

/*
 * Allow only one non-timekeeper CPU at a time update jiffies from
 * the timer tick.
 *
 * Returns true if update was run.
 */
/*
 * tick_limited_update_jiffies64() - 允许 stalled 非负责人 CPU 中至多一个尝试补推进 jiffies。
 * 静态 atomic 门闩用 cmpxchg 抢占；忙则 false。抢到者仅在本 CPU 上次样本仍等于全局 jiffies 时调用更新，
 * 随后释放门闩并返回 true；true 表示尝试窗口已执行，不保证 @now 必然跨过下一周期。要求上层 IRQ-off。
 */
static bool tick_limited_update_jiffies64(struct tick_sched *ts, ktime_t now)
{
	static atomic_t in_progress;
	int inp;

	inp = atomic_read(&in_progress);
	if (inp || !atomic_try_cmpxchg(&in_progress, &inp, 1))
		return false;

	if (ts->last_tick_jiffies == jiffies)
		tick_do_update_jiffies64(now);
	atomic_set(&in_progress, 0);
	return true;
}

#define MAX_STALLED_JIFFIES 5
/* 连续五次本地 tick 未观察到 jiffies 变化后，非负责人可进入上述受限补救，覆盖 stop_machine/长 VMEXIT。 */

/*
 * tick_sched_do_timer() - 每次仿真 tick 处理全局负责人、jiffies 停滞和 idle 到达标志。
 * 若 NO_HZ 下负责人为 NONE，本 CPU 以 WRITE_ONCE 接管；若自己是负责人则推进 jiffies。之后比较
 * last_tick_jiffies，连续 MAX_STALLED_JIFFIES 次不变时通过全局 atomic 门闩尝试补救。INIDLE 时置
 * got_idle_tick 供 idle governor 消费。jiffies_lock 最终串行潜在双负责人；full-nohz 下 NONE 属异常。
 */
static void tick_sched_do_timer(struct tick_sched *ts, ktime_t now)
{
	int tick_cpu, cpu = smp_processor_id();

	/*
	 * Check if the do_timer duty was dropped. We don't care about
	 * concurrency: This happens only when the CPU in charge went
	 * into a long sleep. If two CPUs happen to assign themselves to
	 * this duty, then the jiffies update is still serialized by
	 * 'jiffies_lock'.
	 *
	 * If nohz_full is enabled, this should not happen because the
	 * 'tick_do_timer_cpu' CPU never relinquishes.
	 */
	/* 普通 NO_HZ timekeeper 长睡会发布 NONE，允许下一 tick CPU接管；并发接管由 jiffies_lock 最终串行。full 模式负责人不应交出。 */
	tick_cpu = READ_ONCE(tick_do_timer_cpu);

	if (IS_ENABLED(CONFIG_NO_HZ_COMMON) && unlikely(tick_cpu == TICK_DO_TIMER_NONE)) {
#ifdef CONFIG_NO_HZ_FULL
		WARN_ON_ONCE(tick_nohz_full_running);
#endif
		WRITE_ONCE(tick_do_timer_cpu, cpu);
		tick_cpu = cpu;
	}

	/* Check if jiffies need an update */
	/* 只有当前发布的负责人做常规全局推进，其他 CPU 仅执行下方 stall 监测。 */
	if (tick_cpu == cpu)
		tick_do_update_jiffies64(now);

	/*
	 * If the jiffies update stalled for too long (timekeeper in stop_machine()
	 * or VMEXIT'ed for several msecs), force an update.
	 */
	/* 负责人可能被 stop_machine/虚拟机退出阻塞；按本 CPU 连续样本计数触发限流补推进。 */
	if (ts->last_tick_jiffies != jiffies) {
		ts->stalled_jiffies = 0;
		ts->last_tick_jiffies = READ_ONCE(jiffies);
	} else {
		if (++ts->stalled_jiffies >= MAX_STALLED_JIFFIES) {
			if (tick_limited_update_jiffies64(ts, now)) {
				ts->stalled_jiffies = 0;
				ts->last_tick_jiffies = READ_ONCE(jiffies);
			}
		}
	}

	if (tick_sched_flag_test(ts, TS_FLAG_INIDLE))
		ts->got_idle_tick = 1;
}

/*
 * tick_sched_handle() - 执行一次当前任务调度 tick 与 profiling。
 * 若 CPU 处于 idle 且 tick 已停，先触碰 softlockup watchdog，并清 next_tick 防止相同 deadline 被错误跳过；
 * 随后用中断寄存器区分 user/system 调 update_process_times，再采 CPU_PROFILING。@regs 必须有效且只在调用期借用。
 */
static void tick_sched_handle(struct tick_sched *ts, struct pt_regs *regs)
{
	/*
	 * When we are idle and the tick is stopped, we have to touch
	 * the watchdog as we might not schedule for a really long
	 * time. This happens on completely idle SMP systems while
	 * waiting on the login prompt. We also increment the "start of
	 * idle" jiffy stamp so the idle accounting adjustment we do
	 * when we go busy again does not account too many ticks.
	 */
	/* stopped idle 可能长期不调度；喂 watchdog，并废弃缓存 deadline 以迫使下一轮真正重编硬件。 */
	if (IS_ENABLED(CONFIG_NO_HZ_COMMON) &&
	    tick_sched_flag_test(ts, TS_FLAG_STOPPED)) {
		touch_softlockup_watchdog_sched();
		/*
		 * In case the current tick fired too early past its expected
		 * expiration, make sure we don't bypass the next clock reprogramming
		 * to the same deadline.
		 */
		/* 过早中断后硬件可能已消费旧事件；即使目标数值相同也必须允许下一轮重新编程。 */
		ts->next_tick = 0;
	}

	update_process_times(user_mode(regs));
	profile_tick(CPU_PROFILING);
}

/*
 * We rearm the timer until we get disabled by the idle code.
 * Called with interrupts disabled.
 */
/*
 * tick_nohz_handler() - highres sched_timer 的周期 callback，也是 lowres handler 复用的公共 tick 核心。
 * 从嵌入 timer 取 @ts，采 now 后处理 do_timer；仅有有效 irq_regs 时做进程 tick，否则清 next_tick 促使重算。
 * STOPPED 表示 idle/full dynticks 已接管重编，返回 NORESTART；否则把 timer 严格 forward 一个或多个 TICK_NSEC
 * 到 now 之后并返回 RESTART。IRQ 必须关闭，栈上 regs 不跨调用保存。
 */
static enum hrtimer_restart tick_nohz_handler(struct hrtimer *timer)
{
	struct tick_sched *ts =	container_of(timer, struct tick_sched, sched_timer);
	struct pt_regs *regs = get_irq_regs();
	ktime_t now = ktime_get();

	tick_sched_do_timer(ts, now);

	/*
	 * Do not call when we are not in IRQ context and have
	 * no valid 'regs' pointer
	 */
	/* 非真实 IRQ 调用没有可解释的 user_mode 寄存器，只推进全局时间并强制下次 deadline 重算。 */
	if (regs)
		tick_sched_handle(ts, regs);
	else
		ts->next_tick = 0;

	/*
	 * In dynticks mode, tick reprogram is deferred:
	 * - to the idle task if in dynticks-idle
	 * - to IRQ exit if in full-dynticks.
	 */
	/* STOPPED 后的下一期限由 idle loop 或 full-nohz IRQ exit 决定，callback 不能自行周期重装。 */
	if (unlikely(tick_sched_flag_test(ts, TS_FLAG_STOPPED)))
		return HRTIMER_NORESTART;

	hrtimer_forward(timer, now, TICK_NSEC);

	return HRTIMER_RESTART;
}

#ifdef CONFIG_NO_HZ_FULL
cpumask_var_t tick_nohz_full_mask;
EXPORT_SYMBOL_GPL(tick_nohz_full_mask);
bool tick_nohz_full_running;
EXPORT_SYMBOL_GPL(tick_nohz_full_running);
static atomic_t tick_dep_mask;
/* full_mask 是启动期隔离 CPU 集合，running 是最终启用门；tick_dep_mask 聚合系统级必须保留 tick 的原因位。 */

/*
 * check_tick_dependency() - 判断一个原子依赖掩码是否阻止 full tick 停止。
 * tracepoint 未启用时只返回是否非零；启用时按 POSIX timer、perf、scheduler、clock unstable、RCU/expedited
 * 顺序找首个已知位并发 trace，任一命中 true，全空 false。仅原子快照，不清位；未知非零位在 trace 开启时不匹配。
 */
static bool check_tick_dependency(atomic_t *dep)
{
	int val = atomic_read(dep);

	if (likely(!tracepoint_enabled(tick_stop)))
		return !!val;

	if (val & TICK_DEP_MASK_POSIX_TIMER) {
		trace_tick_stop(0, TICK_DEP_MASK_POSIX_TIMER);
		return true;
	}

	if (val & TICK_DEP_MASK_PERF_EVENTS) {
		trace_tick_stop(0, TICK_DEP_MASK_PERF_EVENTS);
		return true;
	}

	if (val & TICK_DEP_MASK_SCHED) {
		trace_tick_stop(0, TICK_DEP_MASK_SCHED);
		return true;
	}

	if (val & TICK_DEP_MASK_CLOCK_UNSTABLE) {
		trace_tick_stop(0, TICK_DEP_MASK_CLOCK_UNSTABLE);
		return true;
	}

	if (val & TICK_DEP_MASK_RCU) {
		trace_tick_stop(0, TICK_DEP_MASK_RCU);
		return true;
	}

	if (val & TICK_DEP_MASK_RCU_EXP) {
		trace_tick_stop(0, TICK_DEP_MASK_RCU_EXP);
		return true;
	}

	return false;
}

/*
 * can_stop_full_tick() - 汇总当前 CPU 的四层 full-dynticks 依赖。
 * 要求 IRQ-off；CPU offline、global、per-CPU、current task 或 current signal 任一 mask 非零均返回 false，
 * 全部允许才 true。@ts 必须是本 CPU 控制块，current/signal 生命周期由调度上下文稳定。
 */
static bool can_stop_full_tick(int cpu, struct tick_sched *ts)
{
	lockdep_assert_irqs_disabled();

	if (unlikely(!cpu_online(cpu)))
		return false;

	if (check_tick_dependency(&tick_dep_mask))
		return false;

	if (check_tick_dependency(&ts->tick_dep_mask))
		return false;

	if (check_tick_dependency(&current->tick_dep_mask))
		return false;

	if (check_tick_dependency(&current->signal->tick_dep_mask))
		return false;

	return true;
}

/* nohz_full_kick_func() 的 hard irq_work body 故意为空；仅进入/退出 IRQ 就会在 tick_nohz_irq_exit 重评估。 */
static void nohz_full_kick_func(struct irq_work *work)
{
	/* Empty, the tick restart happens on tick_nohz_irq_exit() */
	/* work 只充当一次自 IPI/远端 IPI 触发器，实际 restart 不在 callback 内执行。 */
}

static DEFINE_PER_CPU(struct irq_work, nohz_full_kick_work) =
	IRQ_WORK_INIT_HARD(nohz_full_kick_func);
/* 每 CPU 静态 hard irq_work 可合并重复 kick，无动态分配；队列状态由 irq_work core 管理。 */

/*
 * Kick this CPU if it's full dynticks in order to force it to
 * re-evaluate its dependency on the tick and restart it if necessary.
 * This kick, unlike tick_nohz_full_kick_cpu() and tick_nohz_full_kick_all(),
 * is NMI safe.
 */
/*
 * tick_nohz_full_kick() - NMI-safe 地请求当前 full-nohz CPU 重评估 tick。
 * 非 full-nohz CPU 无动作；否则把本 CPU静态 hard irq_work 入队，重复请求可合并。只负责触发 IRQ exit，
 * 不直接改 STOPPED/期限，适合 perf 等 NMI 调用者。
 */
static void tick_nohz_full_kick(void)
{
	if (!tick_nohz_full_cpu(smp_processor_id()))
		return;

	irq_work_queue(this_cpu_ptr(&nohz_full_kick_work));
}

/*
 * Kick the CPU if it's full dynticks in order to force it to
 * re-evaluate its dependency on the tick and restart it if necessary.
 */
/*
 * tick_nohz_full_kick_cpu() - 向指定 full-nohz @cpu 排队远端 hard irq_work。
 * 非 full-nohz 目标直接返回；调用者须保证远端队列 API 的 CPU online/NMI 约束，本函数不等待执行、不返回状态。
 */
void tick_nohz_full_kick_cpu(int cpu)
{
	if (!tick_nohz_full_cpu(cpu))
		return;

	irq_work_queue_on(&per_cpu(nohz_full_kick_work, cpu), cpu);
}

/*
 * tick_nohz_kick_task() - 为新增 per-task/group 依赖唤醒任务可能运行的 CPU。
 * 不在 runqueue 的任务无需 IPI，未来 schedule 会读新 mask；在队列者先读 task_cpu，再禁抢占确认目标 online 并
 * 远端 kick。与 activate/schedule/migration 的屏障注释保证依赖或在当前 CPU 被 kick 看见，或在迁移后调度时看见。
 * @tsk 由调用者稳定持有；无返回值，迁移竞态不会漏依赖，最坏多一次 IPI。
 */
static void tick_nohz_kick_task(struct task_struct *tsk)
{
	int cpu;

	/*
	 * If the task is not running, run_posix_cpu_timers()
	 * has nothing to elapse, and an IPI can then be optimized out.
	 *
	 * activate_task()                      STORE p->tick_dep_mask
	 *   STORE p->on_rq
	 * __schedule() (switch to task 'p')    smp_mb() (atomic_fetch_or())
	 *   LOCK rq->lock                      LOAD p->on_rq
	 *   smp_mb__after_spin_lock()
	 *   tick_nohz_task_switch()
	 *     LOAD p->tick_dep_mask
	 *
	 * XXX given a task picks up the dependency on schedule(), should we
	 * only care about tasks that are currently on the CPU instead of all
	 * that are on the runqueue?
	 *
	 * That is, does this want to be: task_on_cpu() / task_curr()?
	 */
	/* 当前选择保守覆盖所有 on-rq 任务；即使尚未 current，也允许提前 kick，代价是可能多一次 IPI。 */
	if (!sched_task_on_rq(tsk))
		return;

	/*
	 * If the task concurrently migrates to another CPU,
	 * we guarantee it sees the new tick dependency upon
	 * schedule.
	 *
	 * set_task_cpu(p, cpu);
	 *   STORE p->cpu = @cpu
	 * __schedule() (switch to task 'p')
	 *   LOCK rq->lock
	 *   smp_mb__after_spin_lock()          STORE p->tick_dep_mask
	 *   tick_nohz_task_switch()            smp_mb() (atomic_fetch_or())
	 *      LOAD p->tick_dep_mask           LOAD p->cpu
	 */
	/* 迁移与 atomic_fetch_or 的全屏障配对：新 CPU 要么接到 kick，要么在 schedule 的 rq 锁后读到 dependency。 */
	cpu = task_cpu(tsk);

	preempt_disable();
	if (cpu_online(cpu))
		tick_nohz_full_kick_cpu(cpu);
	preempt_enable();
}

/*
 * Kick all full dynticks CPUs in order to force these to re-evaluate
 * their dependency on the tick and restart it if necessary.
 */
/*
 * tick_nohz_full_kick_all() - 向所有在线 full-nohz CPU 广播一次依赖重评估。
 * running=false 无动作；禁抢占稳定本 CPU 迭代语境，遍历 full_mask 与 online mask 交集逐个异步 kick，不等待完成。
 */
static void tick_nohz_full_kick_all(void)
{
	int cpu;

	if (!tick_nohz_full_running)
		return;

	preempt_disable();
	for_each_cpu_and(cpu, tick_nohz_full_mask, cpu_online_mask)
		tick_nohz_full_kick_cpu(cpu);
	preempt_enable();
}

/*
 * tick_nohz_dep_set_all() - 在 @dep 原子 mask 设置 @bit，并在掩码由全零变为非零时 kick 全部 full CPU。
 * 已有任意依赖时不重复广播，因为 CPU 已应保持/恢复 tick；不同位并发由 atomic_fetch_or 合并。
 */
static void tick_nohz_dep_set_all(atomic_t *dep,
				  enum tick_dep_bits bit)
{
	int prev;

	prev = atomic_fetch_or(BIT(bit), dep);
	if (!prev)
		tick_nohz_full_kick_all();
}

/*
 * Set a global tick dependency. Used by perf events that rely on freq and
 * unstable clocks.
 */
/* tick_nohz_dep_set() 设置系统级依赖位，供 perf 频率采样、不稳定 clock 等要求所有 full CPU 保留 tick。 */
void tick_nohz_dep_set(enum tick_dep_bits bit)
{
	tick_nohz_dep_set_all(&tick_dep_mask, bit);
}

/* tick_nohz_dep_clear() 原子清系统级 @bit；不主动 kick，CPU 在后续 IRQ/tick 机会重新判断是否可停。 */
void tick_nohz_dep_clear(enum tick_dep_bits bit)
{
	atomic_andnot(BIT(bit), &tick_dep_mask);
}

/*
 * Set per-CPU tick dependency. Used by scheduler and perf events in order to
 * manage event-throttling.
 */
/*
 * tick_nohz_dep_set_cpu() - 设置指定 CPU 的 scheduler/perf 依赖并在首次依赖时触发重评估。
 * atomic OR 返回旧掩码；仅旧值全零才 kick。禁抢占后，本 CPU 走 NMI-safe local irq_work，远端在 NMI 中会
 * WARN 且不排队，否则用远端 kick。无返回值；@cpu 的 per-CPU 存储需由调用者/hotplug 协议稳定。
 */
void tick_nohz_dep_set_cpu(int cpu, enum tick_dep_bits bit)
{
	int prev;
	struct tick_sched *ts;

	ts = per_cpu_ptr(&tick_cpu_sched, cpu);

	prev = atomic_fetch_or(BIT(bit), &ts->tick_dep_mask);
	if (!prev) {
		preempt_disable();
		/* Perf needs local kick that is NMI safe */
		/* perf 可从 NMI 设置本地依赖；只有 local hard irq_work 路径承诺 NMI-safe。 */
		if (cpu == smp_processor_id()) {
			tick_nohz_full_kick();
		} else {
			/* Remote IRQ work not NMI-safe */
			if (!WARN_ON_ONCE(in_nmi()))
				tick_nohz_full_kick_cpu(cpu);
		}
		preempt_enable();
	}
}
EXPORT_SYMBOL_GPL(tick_nohz_dep_set_cpu);

/* tick_nohz_dep_clear_cpu() 原子清 @cpu 的依赖位，不触发立即重算；跨 CPU 存储生命周期由调用者保证。 */
void tick_nohz_dep_clear_cpu(int cpu, enum tick_dep_bits bit)
{
	struct tick_sched *ts = per_cpu_ptr(&tick_cpu_sched, cpu);

	atomic_andnot(BIT(bit), &ts->tick_dep_mask);
}
EXPORT_SYMBOL_GPL(tick_nohz_dep_clear_cpu);

/*
 * Set a per-task tick dependency. RCU needs this. Also posix CPU timers
 * in order to elapse per task timers.
 */
/*
 * tick_nohz_dep_set_task() - 设置任务级 RCU/perf/POSIX CPU timer 依赖。
 * 仅 mask 从零变非零时按任务 runqueue/迁移协议 kick；@tsk 必须有稳定引用，原子位允许并发来源合并。
 */
void tick_nohz_dep_set_task(struct task_struct *tsk, enum tick_dep_bits bit)
{
	if (!atomic_fetch_or(BIT(bit), &tsk->tick_dep_mask))
		tick_nohz_kick_task(tsk);
}
EXPORT_SYMBOL_GPL(tick_nohz_dep_set_task);

/* tick_nohz_dep_clear_task() 原子清任务依赖，不发送 IPI；任务下次状态检查可据剩余 mask 决定停 tick。 */
void tick_nohz_dep_clear_task(struct task_struct *tsk, enum tick_dep_bits bit)
{
	atomic_andnot(BIT(bit), &tsk->tick_dep_mask);
}
EXPORT_SYMBOL_GPL(tick_nohz_dep_clear_task);

/*
 * Set a per-taskgroup tick dependency. Posix CPU timers need this in order to elapse
 * per process timers.
 */
/*
 * tick_nohz_dep_set_signal() - 设置线程组级依赖并在首次置位时 kick 组内每个 on-rq 线程。
 * @tsk 提供 signal/sighand；首次转换要求调用者持 siglock，保证 __for_each_thread 链稳定。每个线程再由
 * tick_nohz_kick_task 处理迁移；已有任意组依赖时无需重复遍历。无返回值。
 */
void tick_nohz_dep_set_signal(struct task_struct *tsk,
			      enum tick_dep_bits bit)
{
	int prev;
	struct signal_struct *sig = tsk->signal;

	prev = atomic_fetch_or(BIT(bit), &sig->tick_dep_mask);
	if (!prev) {
		struct task_struct *t;

		lockdep_assert_held(&tsk->sighand->siglock);
		__for_each_thread(sig, t)
			tick_nohz_kick_task(t);
	}
}

/* tick_nohz_dep_clear_signal() 原子清线程组 @bit；@sig 生命周期由调用者稳定，不遍历线程也不立即重算。 */
void tick_nohz_dep_clear_signal(struct signal_struct *sig, enum tick_dep_bits bit)
{
	atomic_andnot(BIT(bit), &sig->tick_dep_mask);
}

/*
 * Re-evaluate the need for the tick as we switch the current task.
 * It might need the tick due to per task/process properties:
 * perf events, posix CPU timers, ...
 */
/*
 * __tick_nohz_task_switch() - context switch 后为新 current 重评估 task/signal tick 依赖。
 * 非 full-nohz CPU 或 tick 尚运行直接返回；STOPPED 且任一 current mask 非零时排本地 NMI-safe kick，使 IRQ exit
 * 恢复 tick。调度器 rq 锁/屏障保证看见并发置位；函数不直接重编设备。
 */
void __tick_nohz_task_switch(void)
{
	struct tick_sched *ts;

	if (!tick_nohz_full_cpu(smp_processor_id()))
		return;

	ts = this_cpu_ptr(&tick_cpu_sched);

	if (tick_sched_flag_test(ts, TS_FLAG_STOPPED)) {
		if (atomic_read(&current->tick_dep_mask) ||
		    atomic_read(&current->signal->tick_dep_mask))
			tick_nohz_full_kick();
	}
}

/* Get the boot-time nohz CPU list from the kernel parameters. */
/*
 * 从内核参数取得启动期 nohz CPU 列表。housekeeping_setup() 传入的是
 * non-housekeeping/full-dynticks CPU 集合，而不是它的补集。
 */
/*
 * tick_nohz_full_setup() - 保存启动参数指定的 full-nohz CPU 集合。
 *
 * @cpumask 是命令行解析器持有的临时借用掩码；本函数复制内容，不保存该指针，
 * 所以调用者返回后可释放 bootmem 临时对象。函数在 early boot、无并发读者时
 * 调用并标记 __init；分配接口按启动期规则处理失败。
 *
 * 返回：无直接返回值。成功后 tick_nohz_full_mask 拥有独立 bootmem 存储，
 * tick_nohz_full_running 发布“full dynticks 已配置”的状态，后续 tick 初始化、
 * CPU hotplug 和任务切换路径据此启用相应协议。
 */
void __init tick_nohz_full_setup(cpumask_var_t cpumask)
{
	/* 先分配并完整复制掩码，最后置 running，避免消费者观察到未初始化列表。 */
	alloc_bootmem_cpumask_var(&tick_nohz_full_mask);
	cpumask_copy(tick_nohz_full_mask, cpumask);
	tick_nohz_full_running = true;
}

/*
 * tick_nohz_cpu_hotpluggable() - 判断 CPU 是否允许因 hotplug 下线。
 *
 * @cpu 为纯输入 CPU 编号。full-nohz 未启用或 CPU 不是当前 tick_do_timer_cpu 时
 * 返回 true；否则返回 false。tick_do_timer_cpu 代表 full-dynticks CPU 承担
 * timekeeping、unbound timer/workqueue 等职责，必须保持在线。
 * READ_ONCE 取得单次标量快照，但不固定后续状态；调用者仍处于 hotplug 协议中。
 */
bool tick_nohz_cpu_hotpluggable(unsigned int cpu)
{
	/*
	 * The 'tick_do_timer_cpu' CPU handles housekeeping duty (unbound
	 * timers, workqueues, timekeeping, ...) on behalf of full dynticks
	 * CPUs. It must remain online when nohz full is enabled.
	 */
	/*
	 * tick_do_timer_cpu 代表 full-dynticks CPU 承担 housekeeping 职责，包括
	 * unbound timer、workqueue 和 timekeeping；nohz full 启用时它必须保持在线。
	 */
	if (tick_nohz_full_running && READ_ONCE(tick_do_timer_cpu) == cpu)
		return false;
	return true;
}

/* tick_nohz_cpu_down() 是 cpuhp predown 门禁：可下线返回 0，timekeeping/housekeeping 负责人返回 -EBUSY。 */
static int tick_nohz_cpu_down(unsigned int cpu)
{
	return tick_nohz_cpu_hotpluggable(cpu) ? 0 : -EBUSY;
}

/*
 * tick_nohz_init() - 在启动后期验证并发布 full-dynticks 运行环境。
 * 未配置直接返回；架构缺 irq_work 自 IPI 时清 mask/running 并禁用。特定 SMP suspend 配置把启动 CPU 从
 * full mask 移除，随后为每个隔离 CPU 启用 context tracking，注册 cpuhp predown 门禁并打印最终集合。
 * cpuhp 注册失败仅 WARN，不回滚 full 配置；__init 期间无并发修改 mask。
 */
void __init tick_nohz_init(void)
{
	int cpu, ret;

	if (!tick_nohz_full_running)
		return;

	/*
	 * Full dynticks uses IRQ work to drive the tick rescheduling on safe
	 * locking contexts. But then we need IRQ work to raise its own
	 * interrupts to avoid circular dependency on the tick.
	 */
	/* full CPU停 tick 后只能靠 irq_work 自 IPI重新进入内核；若架构做不到，就不能安全启用该模式。 */
	if (!arch_irq_work_has_interrupt()) {
		pr_warn("NO_HZ: Can't run full dynticks because arch doesn't support IRQ work self-IPIs\n");
		cpumask_clear(tick_nohz_full_mask);
		tick_nohz_full_running = false;
		return;
	}

	if (IS_ENABLED(CONFIG_PM_SLEEP_SMP) &&
			!IS_ENABLED(CONFIG_PM_SLEEP_SMP_NONZERO_CPU)) {
		cpu = smp_processor_id();

		if (cpumask_test_cpu(cpu, tick_nohz_full_mask)) {
			pr_warn("NO_HZ: Clearing %d from nohz_full range "
				"for timekeeping\n", cpu);
			cpumask_clear_cpu(cpu, tick_nohz_full_mask);
		}
	}

	for_each_cpu(cpu, tick_nohz_full_mask)
		ct_cpu_track_user(cpu);

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					"kernel/nohz:predown", NULL,
					tick_nohz_cpu_down);
	WARN_ON(ret < 0);
	pr_info("NO_HZ: Full dynticks CPUs: %*pbl.\n",
		cpumask_pr_args(tick_nohz_full_mask));
}
#endif /* #ifdef CONFIG_NO_HZ_FULL */

/*
 * NOHZ - aka dynamic tick functionality
 */
/* NOHZ 即 dynamic tick：只有在下次必要事件较远时才停止固定周期中断。 */
#ifdef CONFIG_NO_HZ_COMMON
/*
 * NO HZ enabled ?
 */
/* enabled 可由启动参数关闭，active 则由首个成功配置的 CPU 发布。 */
bool tick_nohz_enabled __read_mostly  = true;
static unsigned long tick_nohz_active  __read_mostly;
/* enabled 是 `nohz=` 启动开关；active bit0 表示至少一 CPU 已真正激活 NO_HZ，并触发 timer 侧刷新。 */
/*
 * Enable / Disable tickless mode
 */
/* setup_tick_nohz() 解析 `nohz=` 布尔值；解析成功返回 1 让 __setup 消费参数，失败返回 0 并保留旧值。 */
static int __init setup_tick_nohz(char *str)
{
	return (kstrtobool(str, &tick_nohz_enabled) == 0);
}

__setup("nohz=", setup_tick_nohz);

/* tick_nohz_is_active() 无锁返回全局 active 位；非零仅代表已有 CPU 激活，不保证当前 CPU 的 NOHZ flag。 */
bool tick_nohz_is_active(void)
{
	return tick_nohz_active;
}
EXPORT_SYMBOL_GPL(tick_nohz_is_active);

/* tick_nohz_tick_stopped() 查询当前 CPU STOPPED flag；调用者负责本地状态同步，返回 bool。 */
bool tick_nohz_tick_stopped(void)
{
	struct tick_sched *ts = this_cpu_ptr(&tick_cpu_sched);

	return tick_sched_flag_test(ts, TS_FLAG_STOPPED);
}

/* tick_nohz_tick_stopped_cpu() 无锁查询远端 @cpu STOPPED 快照；不校验 online，也不保证返回后状态不变。 */
bool tick_nohz_tick_stopped_cpu(int cpu)
{
	struct tick_sched *ts = per_cpu_ptr(&tick_cpu_sched, cpu);

	return tick_sched_flag_test(ts, TS_FLAG_STOPPED);
}

/**
 * tick_nohz_update_jiffies - update jiffies when idle was interrupted
 * @now: current ktime_t
 *
 * Called from interrupt entry when the CPU was idle
 *
 * In case the sched_tick was stopped on this CPU, we have to check if jiffies
 * must be updated. Otherwise an interrupt handler could use a stale jiffy
 * value. We do this unconditionally on any CPU, as we don't know whether the
 * CPU, which has the update task assigned, is in a long sleep.
 */
/*
 * tick_nohz_update_jiffies() - idle 被 IRQ 打断时刷新可被 handler 读取的全局 jiffies。
 * 先记录本 CPU idle_waketime=@now，再 save IRQ（允许调用者状态不同）调用全局追赶，恢复原状态并喂 watchdog。
 * 即使本 CPU 非负责人也执行，因为负责人可能长睡；无返回值，不恢复周期 tick。
 */
static void tick_nohz_update_jiffies(ktime_t now)
{
	unsigned long flags;

	__this_cpu_write(tick_cpu_sched.idle_waketime, now);

	local_irq_save(flags);
	tick_do_update_jiffies64(now);
	local_irq_restore(flags);

	touch_softlockup_watchdog_sched();
}

/* Simplified variant of hrtimer_forward_now() */
/*
 * tick_forward_now() - 把周期 @expires 推进到严格晚于 @now 的首个 TICK_NSEC 边界。
 * 未落后一个周期时直接加一 tick；否则整数除法跳过整周期并在等于/早于 now 时再加一次。返回新绝对期限，
 * 不修改 timer；假定时间差与乘法在 ktime 范围内。
 */
static ktime_t tick_forward_now(ktime_t expires, ktime_t now)
{
	ktime_t delta = now - expires;

	if (likely(delta < TICK_NSEC))
		return expires + TICK_NSEC;

	expires += TICK_NSEC * ktime_divns(delta, TICK_NSEC);
	if (expires > now)
		return expires;
	return expires + TICK_NSEC;
}

/*
 * tick_nohz_restart() - 沿停止前保存的周期时间线恢复 sched tick。
 * 从 ts->last_tick 起，若已过期则 forward 到 now 后；HIGHRES 启动 pinned hard hrtimer，否则只更新其 expires
 * 镜像并 force 编程本地 clockevent。最后清 next_tick 缓存，保证下一次 stop 重新比较真实 deadline。
 */
static void tick_nohz_restart(struct tick_sched *ts, ktime_t now)
{
	ktime_t expires = ts->last_tick;

	if (now >= expires)
		expires = tick_forward_now(expires, now);

	if (tick_sched_flag_test(ts, TS_FLAG_HIGHRES)) {
		hrtimer_start(&ts->sched_timer,	expires, HRTIMER_MODE_ABS_PINNED_HARD);
	} else {
		hrtimer_set_expires(&ts->sched_timer, expires);
		tick_program_event(expires, 1);
	}

	/*
	 * Reset to make sure the next tick stop doesn't get fooled by past
	 * cached clock deadline.
	 */
	/* 旧缓存可能等于新期限但硬件已改变；置零禁止 stop 路径误走“无需重编”快路。 */
	ts->next_tick = 0;
}

/* local_timer_softirq_pending() 只测试本 CPU pending mask 的 TIMER_SOFTIRQ 位；不消费或执行 softirq。 */
static inline bool local_timer_softirq_pending(void)
{
	return local_timers_pending() & BIT(TIMER_SOFTIRQ);
}

/*
 * Read jiffies and the time when jiffies were updated last
 */
/*
 * get_jiffies_update() - 取得匹配的 jiffies 与 last_jiffies_update 快照。
 * 以 jiffies_seq 重试同时读取，向 *@basej 写 jiffies 并返回 monotonic ns 锚点；调用者提供有效内核指针，
 * 函数无锁等待 writer 完成，不改变状态。
 */
u64 get_jiffies_update(unsigned long *basej)
{
	unsigned long basejiff;
	unsigned int seq;
	u64 basemono;

	do {
		seq = read_seqcount_begin(&jiffies_seq);
		basemono = last_jiffies_update;
		basejiff = jiffies;
	} while (read_seqcount_retry(&jiffies_seq, seq));
	*basej = basejiff;
	return basemono;
}

/**
 * tick_nohz_next_event() - return the clock monotonic based next event
 * @ts:		pointer to tick_sched struct
 * @cpu:	CPU number
 *
 * Return:
 * *%0		- When the next event is a maximum of TICK_NSEC in the future
 *		  and the tick is not stopped yet
 * *%next_event	- Next event based on clock monotonic
 */
/*
 * tick_nohz_next_event() - 计算本 CPU 可停止周期 tick 时的 monotonic 最早唤醒期限。
 * 先保存同一 jiffies/mono 基准。RCU/arch/irq_work 或已 pending timer softirq 要求下一 tick；否则查询 timer wheel
 * （低分辨率还含 hrtimer）并缓存 next_timer。期限在一个 tick 内且尚未 STOPPED 时返回 0 表示保留周期 tick。
 * 最后把 timer 期限与 timekeeping_max_deferment 取最小；非负责人通常可无限 defer，最后负责人需受上限约束。
 * 返回 0、有限绝对期限或 KTIME_MAX，并写 timer_expires/base；要求本 CPU IRQ-off 且 @ts 匹配 @cpu。
 */
static ktime_t tick_nohz_next_event(struct tick_sched *ts, int cpu)
{
	u64 basemono, next_tick, delta, expires;
	unsigned long basejiff;
	int tick_cpu;

	basemono = get_jiffies_update(&basejiff);
	ts->last_jiffies = basejiff;
	ts->timer_expires_base = basemono;

	/*
	 * Keep the periodic tick, when RCU, architecture or irq_work
	 * requests it.
	 * Aside of that, check whether the local timer softirq is
	 * pending. If so, its a bad idea to call get_next_timer_interrupt(),
	 * because there is an already expired timer, so it will request
	 * immediate expiry, which rearms the hardware timer with a
	 * minimal delta, which brings us back to this place
	 * immediately. Lather, rinse and repeat...
	 */
	/* 这些消费者需要近期 CPU机会；尤其已过期 timer softirq 若再查询会反复得到立即期限并形成最小 delta 中断循环。 */
	if (rcu_needs_cpu() || arch_needs_cpu() ||
	    irq_work_needs_cpu() || local_timer_softirq_pending()) {
		next_tick = basemono + TICK_NSEC;
	} else {
		/*
		 * Get the next pending timer. If high resolution
		 * timers are enabled this only takes the timer wheel
		 * timers into account. If high resolution timers are
		 * disabled this also looks at the next expiring
		 * hrtimer.
		 */
		/* highres 的 hrtimer 自有硬件队列，此处只问 timer wheel；lowres 则必须把 hrtimer 也合入 tick 后端期限。 */
		next_tick = get_next_timer_interrupt(basejiff, basemono);
		ts->next_timer = next_tick;
	}

	/* Make sure next_tick is never before basemono! */
	/* 过期结果是内部不变量异常；钳到基准防 unsigned delta 下溢成超长睡眠。 */
	if (WARN_ON_ONCE(basemono > next_tick))
		next_tick = basemono;

	/*
	 * If the tick is due in the next period, keep it ticking or
	 * force prod the timer.
	 */
	/* 一周期内没有停 tick 的收益；若已经 stopped，仍保留该期限以用一次性事件及时唤醒。 */
	delta = next_tick - basemono;
	if (delta <= (u64)TICK_NSEC) {
		/*
		 * We've not stopped the tick yet, and there's a timer in the
		 * next period, so no point in stopping it either, bail.
		 */
		/* 尚未 stopped 时返回 0 作为“保留周期模式”协议，而不是绝对时间零。 */
		if (!tick_sched_flag_test(ts, TS_FLAG_STOPPED)) {
			ts->timer_expires = 0;
			goto out;
		}
	}

	/*
	 * If this CPU is the one which had the do_timer() duty last, we limit
	 * the sleep time to the timekeeping 'max_deferment' value.
	 * Otherwise we can sleep as long as we want.
	 */
	/* 负责全局时间推进或最后曾负责且尚无人接管者，睡眠不得超过 timekeeping 可延迟上限。 */
	delta = timekeeping_max_deferment();
	tick_cpu = READ_ONCE(tick_do_timer_cpu);
	if (tick_cpu != cpu &&
	    (tick_cpu != TICK_DO_TIMER_NONE || !tick_sched_flag_test(ts, TS_FLAG_DO_TIMER_LAST)))
		delta = KTIME_MAX;

	/* Calculate the next expiry time */
	/* 对 KTIME_MAX 做饱和加法，再与 timer 期限取最小，避免绝对时间溢出。 */
	if (delta < (KTIME_MAX - basemono))
		expires = basemono + delta;
	else
		expires = KTIME_MAX;

	ts->timer_expires = min_t(u64, expires, next_tick);

out:
	return ts->timer_expires;
}

/*
 * tick_nohz_stop_tick() - 提交 tick_nohz_next_event 计算并停止/改编本 CPU sched tick。
 * 先消费 timer_expires_base，再令 timer base 进入 idle 以封闭远端入队漏唤醒；若重查期限变晚，仍保留原较早值
 * 配合 cpuidle 浅状态。base 未 idle 则保留 tick。当前 do_timer 负责人交出职责并记录 LAST；首次停止保存
 * last_tick、启动 load/vmstat/nohz balance 统计并置 STOPPED。相同期限仅在 timer 确实已编程时跳过；否则告警。
 * KTIME_MAX 取消 highres timer/停止 lowres device，有限值按模式重装。要求 IRQ-off，本函数不报告编程错误。
 */
static void tick_nohz_stop_tick(struct tick_sched *ts, int cpu)
{
	struct clock_event_device *dev = __this_cpu_read(tick_cpu_device.evtdev);
	unsigned long basejiff = ts->last_jiffies;
	u64 basemono = ts->timer_expires_base;
	bool timer_idle = tick_sched_flag_test(ts, TS_FLAG_STOPPED);
	int tick_cpu;
	u64 expires;

	/* Make sure we won't be trying to stop it twice in a row. */
	/* timer_expires_base 是一次性“已预计算”凭证，入口即清，防止后续 idle 轮次复用过期快照。 */
	ts->timer_expires_base = 0;

	/*
	 * Now the tick should be stopped definitely - so the timer base needs
	 * to be marked idle as well to not miss a newly queued timer.
	 */
	/* 先发布 timer base idle，远端更早 timer 入队才会发送 IPI；返回期限同时复核计算后发生的队列变化。 */
	expires = timer_base_try_to_set_idle(basejiff, basemono, &timer_idle);
	if (expires > ts->timer_expires) {
		/*
		 * This path could only happen when the first timer was removed
		 * between calculating the possible sleep length and now (when
		 * high resolution mode is not active, timer could also be a
		 * hrtimer).
		 *
		 * We have to stick to the original calculated expiry value to
		 * not stop the tick for too long with a shallow C-state (which
		 * was programmed by cpuidle because of an early next expiration
		 * value).
		 */
		/* 重查只变晚通常因最早 timer 被删除；仍用旧早期限，尊重 governor 已据此选择的浅 C-state。 */
		expires = ts->timer_expires;
	}

	/* If the timer base is not idle, retain the not yet stopped tick. */
	/* base 已被并发变化阻止进入 idle 时不能停 tick，否则可能漏掉不触发 IPI 的新 timer。 */
	if (!timer_idle)
		return;

	/*
	 * If this CPU is the one which updates jiffies, then give up
	 * the assignment and let it be taken by the CPU which runs
	 * the tick timer next, which might be this CPU as well. If we
	 * don't drop this here, the jiffies might be stale and
	 * do_timer() never gets invoked. Keep track of the fact that it
	 * was the one which had the do_timer() duty last.
	 */
	/* idle timekeeper 交出全局职责；LAST 让无人接管时仍用 max_deferment 限制自己的睡眠。 */
	tick_cpu = READ_ONCE(tick_do_timer_cpu);
	if (tick_cpu == cpu) {
		WRITE_ONCE(tick_do_timer_cpu, TICK_DO_TIMER_NONE);
		tick_sched_flag_set(ts, TS_FLAG_DO_TIMER_LAST);
	} else if (tick_cpu != TICK_DO_TIMER_NONE) {
		tick_sched_flag_clear(ts, TS_FLAG_DO_TIMER_LAST);
	}

	/* Skip reprogram of event if it's not changed */
	/* deadline 未变可省硬件写，但必须核对 highres timer 镜像仍等于该值，否则留下状态失配告警。 */
	if (tick_sched_flag_test(ts, TS_FLAG_STOPPED) && (expires == ts->next_tick)) {
		/* Sanity check: make sure clockevent is actually programmed */
		/* 数值相同只有在无期限或 sched_timer 的真实 expires 同步时才能安全省略重编。 */
		if (expires == KTIME_MAX || ts->next_tick == hrtimer_get_expires(&ts->sched_timer))
			return;

		WARN_ONCE(1, "basemono: %llu ts->next_tick: %llu dev->next_event: %llu "
			  "timer->active: %d timer->expires: %llu\n", basemono, ts->next_tick,
			  dev->next_event, hrtimer_active(&ts->sched_timer),
			  hrtimer_get_expires(&ts->sched_timer));
	}

	/*
	 * tick_nohz_stop_tick() can be called several times before
	 * tick_nohz_restart_sched_tick() is called. This happens when
	 * interrupts arrive which do not cause a reschedule. In the first
	 * call we save the current tick time, so we can restart the
	 * scheduler tick in tick_nohz_restart_sched_tick().
	 */
	/* 多个不触发调度的 IRQ 可重复到此；仅首次停止保存原周期相位和开始统计，避免覆盖 last_tick。 */
	if (!tick_sched_flag_test(ts, TS_FLAG_STOPPED)) {
		calc_load_nohz_start();
		quiet_vmstat();

		ts->last_tick = hrtimer_get_expires(&ts->sched_timer);
		tick_sched_flag_set(ts, TS_FLAG_STOPPED);
		trace_tick_stop(1, TICK_DEP_MASK_NONE);
	}

	ts->next_tick = expires;

	/*
	 * If the expiration time == KTIME_MAX, then we simply stop
	 * the tick timer.
	 */
	/* 无任何期限时真正停设备；有限期限只是把周期 tick 改成一次性唤醒。 */
	if (unlikely(expires == KTIME_MAX)) {
		if (tick_sched_flag_test(ts, TS_FLAG_HIGHRES))
			hrtimer_cancel(&ts->sched_timer);
		else
			tick_program_event(KTIME_MAX, 1);
		return;
	}

	if (tick_sched_flag_test(ts, TS_FLAG_HIGHRES)) {
		hrtimer_start(&ts->sched_timer, expires,
			      HRTIMER_MODE_ABS_PINNED_HARD);
	} else {
		hrtimer_set_expires(&ts->sched_timer, expires);
		tick_program_event(expires, 1);
	}
}

/* tick_nohz_retain_tick() 清预计算凭证，表示本轮不停止；不改 STOPPED、设备或 timer base idle 状态。 */
static void tick_nohz_retain_tick(struct tick_sched *ts)
{
	ts->timer_expires_base = 0;
}

#ifdef CONFIG_NO_HZ_FULL
/* full_stop_tick() 计算后以非零期限停止/改编，返回 0 则仅丢弃预计算并保留周期 tick。 */
static void tick_nohz_full_stop_tick(struct tick_sched *ts, int cpu)
{
	if (tick_nohz_next_event(ts, cpu))
		tick_nohz_stop_tick(ts, cpu);
	else
		tick_nohz_retain_tick(ts);
}
#endif /* CONFIG_NO_HZ_FULL */

/*
 * tick_nohz_restart_sched_tick() - 从 STOPPED 状态恢复周期调度 tick。
 * 先追赶 jiffies，再清 timer base idle 防止远端无谓 IPI，结束 load NO_HZ、喂 watchdog，清 STOPPED 并按
 * last_tick 相位调用 restart。要求 IRQ-off；恢复前 @ts 必须处于 stopped，函数不返回底层编程错误。
 */
static void tick_nohz_restart_sched_tick(struct tick_sched *ts, ktime_t now)
{
	/* Update jiffies first */
	/* 恢复前先补全长睡期间的全局时间，使随后的 timer/调度读取不见陈旧 jiffies。 */
	tick_do_update_jiffies64(now);

	/*
	 * Clear the timer idle flag, so we avoid IPIs on remote queueing and
	 * the clock forward checks in the enqueue path:
	 */
	/* 清 idle 后远端入队无需再发“唤醒 idle base”IPI，enqueue 也恢复常规 forward 检查。 */
	timer_clear_idle();

	calc_load_nohz_stop();
	touch_softlockup_watchdog_sched();

	/* Cancel the scheduled timer and restore the tick: */
	/* clear STOPPED 后 callback 可重新周期 RESTART；restart 保持停止前相位。 */
	tick_sched_flag_clear(ts, TS_FLAG_STOPPED);
	tick_nohz_restart(ts, now);
}

/*
 * __tick_nohz_full_update_tick() - IRQ-off 下按四层依赖决定 full CPU tick 状态。
 * 可停则计算/停止；不可停且当前 STOPPED 才恢复。CONFIG_NO_HZ_FULL=n 时 body 为空，参数无副作用。
 */
static void __tick_nohz_full_update_tick(struct tick_sched *ts,
					 ktime_t now)
{
#ifdef CONFIG_NO_HZ_FULL
	int cpu = smp_processor_id();

	if (can_stop_full_tick(cpu, ts))
		tick_nohz_full_stop_tick(ts, cpu);
	else if (tick_sched_flag_test(ts, TS_FLAG_STOPPED))
		tick_nohz_restart_sched_tick(ts, now);
#endif
}

/* tick_nohz_full_update_tick() 仅对本 CPU full-nohz 且 NOHZ 已激活时采 now 并调用核心决策；其他情况无动作。 */
static void tick_nohz_full_update_tick(struct tick_sched *ts)
{
	if (!tick_nohz_full_cpu(smp_processor_id()))
		return;

	if (!tick_sched_flag_test(ts, TS_FLAG_NOHZ))
		return;

	__tick_nohz_full_update_tick(ts, ktime_get());
}

/*
 * A pending softirq outside an IRQ (or softirq disabled section) context
 * should be waiting for ksoftirqd to handle it. Therefore we shouldn't
 * reach this code due to the need_resched() early check in can_stop_idle_tick().
 *
 * However if we are between CPUHP_AP_SMPBOOT_THREADS and CPU_TEARDOWN_CPU on the
 * cpu_down() process, softirqs can still be raised while ksoftirqd is parked,
 * triggering the code below, since wakep_softirqd() is ignored.
 *
 */
/*
 * report_idle_softirq() - 判断 pending softirq 是否禁止 idle tick stop，并限流报告异常。
 * 无 pending 返回 false；CPU inactive 时剔除 hotplug-safe 位；PREEMPT_RT 上 BH 因锁阻塞也允许停 tick。
 * 其余 pending 返回 true，前 10 次打印 mask。静态 ratelimit 仅近似跨 CPU 共享，诊断不清 pending 位。
 */
static bool report_idle_softirq(void)
{
	static int ratelimit;
	unsigned int pending = local_softirq_pending();

	if (likely(!pending))
		return false;

	/* Some softirqs claim to be safe against hotplug and ksoftirqd parking */
	/* CPU 下线窗口 ksoftirqd 已 parked，只有声明 hotplug-safe 的 softirq 可不阻止停 tick。 */
	if (!cpu_active(smp_processor_id())) {
		pending &= ~SOFTIRQ_HOTPLUG_SAFE_MASK;
		if (!pending)
			return false;
	}

	/* On RT, softirq handling may be waiting on some lock */
	/* RT 的 BH blocked 可能等待锁而非等待周期 tick，保留 tick 无法推进它。 */
	if (local_bh_blocked())
		return false;

	if (ratelimit < 10) {
		pr_warn("NOHZ tick-stop error: local softirq work is pending, handler #%02x!!!\n",
			pending);
		ratelimit++;
	}

	return true;
}

/*
 * can_stop_idle_tick() - idle task 停 tick 的前置门禁。
 * offline 仅 WARN；NOHZ 未激活、need_resched 或不可安全延迟的 softirq 均 false。full-nohz 系统还必须让
 * tick_do_timer_cpu 保持 tick，且 NONE 是异常 false。其余返回 true；要求本 CPU IRQ-off、@ts 匹配 @cpu。
 */
static bool can_stop_idle_tick(int cpu, struct tick_sched *ts)
{
	WARN_ON_ONCE(cpu_is_offline(cpu));

	if (unlikely(!tick_sched_flag_test(ts, TS_FLAG_NOHZ)))
		return false;

	if (need_resched())
		return false;

	if (unlikely(report_idle_softirq()))
		return false;

	if (tick_nohz_full_enabled()) {
		int tick_cpu = READ_ONCE(tick_do_timer_cpu);

		/*
		 * Keep the tick alive to guarantee timekeeping progression
		 * if there are full dynticks CPUs around
		 */
		/* full CPU可能全部停 tick，因此唯一 timekeeper CPU必须继续周期运行以保证全局时间前进。 */
		if (tick_cpu == cpu)
			return false;

		/* Should not happen for nohz-full */
		/* full-nohz 初始化保证固定负责人；观察到 NONE 表示职责协议被破坏。 */
		if (WARN_ON_ONCE(tick_cpu == TICK_DO_TIMER_NONE))
			return false;
	}

	return true;
}

/**
 * tick_nohz_idle_stop_tick - stop the idle tick from the idle task
 *
 * When the next event is more than a tick into the future, stop the idle tick
 */
/*
 * tick_nohz_idle_stop_tick() - idle loop 中计算并尝试停止本 CPU tick。
 * 若 governor 已填 timer_expires_base 则复用，否则先过门禁并计算。每次尝试增 idle_calls；期限>0 时提交 stop，
 * 记录 idle_sleeps/expires，并仅在本轮从 running→STOPPED 时启动 kcpustat/nohz balance idle；0 则保留 tick。
 * 要求 IRQ-off，所有 per-CPU 字段由当前 CPU 独占。
 */
void tick_nohz_idle_stop_tick(void)
{
	struct tick_sched *ts = this_cpu_ptr(&tick_cpu_sched);
	int cpu = smp_processor_id();
	ktime_t expires;

	/*
	 * If tick_nohz_get_sleep_length() ran tick_nohz_next_event(), the
	 * tick timer expiration time is known already.
	 */
	/* governor 的 sleep_length 可提前计算并缓存同基准期限，本轮直接复用，避免重复扫描 timer。 */
	if (ts->timer_expires_base)
		expires = ts->timer_expires;
	else if (can_stop_idle_tick(cpu, ts))
		expires = tick_nohz_next_event(ts, cpu);
	else
		return;

	ts->idle_calls++;

	if (expires > 0LL) {
		int was_stopped = tick_sched_flag_test(ts, TS_FLAG_STOPPED);

		tick_nohz_stop_tick(ts, cpu);

		ts->idle_sleeps++;
		ts->idle_expires = expires;

		if (!was_stopped && tick_sched_flag_test(ts, TS_FLAG_STOPPED)) {
			kcpustat_dyntick_start(ts->idle_entrytime);
			nohz_balance_enter_idle(cpu);
		}
	} else {
		tick_nohz_retain_tick(ts);
	}
}

/* tick_nohz_idle_retain_tick() 只清当前 CPU 的预计算凭证，供 idle governor 明确选择保留周期 tick。 */
void tick_nohz_idle_retain_tick(void)
{
	tick_nohz_retain_tick(this_cpu_ptr(&tick_cpu_sched));
}

/* tick_nohz_clock_sleep() 置 IDLE_ACTIVE 并通知 sched_clock 进入 idle；要求 IRQ-off，重复调用需由状态机避免。 */
static void tick_nohz_clock_sleep(struct tick_sched *ts)
{
	tick_sched_flag_set(ts, TS_FLAG_IDLE_ACTIVE);
	sched_clock_idle_sleep_event();
}

/* tick_nohz_clock_wakeup() 仅在 IDLE_ACTIVE 时清位并通知 sched_clock，因而对重复 IRQ/退出幂等。 */
static void tick_nohz_clock_wakeup(struct tick_sched *ts)
{
	if (tick_sched_flag_test(ts, TS_FLAG_IDLE_ACTIVE)) {
		tick_sched_flag_clear(ts, TS_FLAG_IDLE_ACTIVE);
		sched_clock_idle_wakeup_event();
	}
}

/**
 * tick_nohz_idle_enter - prepare for entering idle on the current CPU
 *
 * Called when we start the idle loop.
 */
/*
 * tick_nohz_idle_enter() - 当前 CPU 开始 idle loop 时建立记账边界。
 * 要求入口 IRQ enabled；函数短暂关 IRQ，确认无残留 timer_expires_base，置 INIDLE，记录 monotonic
 * idle_entrytime 并通知 sched_clock sleep，再重新开 IRQ。此处尚不停止 tick，实际决定在 idle_stop_tick。
 */
void tick_nohz_idle_enter(void)
{
	struct tick_sched *ts;

	lockdep_assert_irqs_enabled();

	local_irq_disable();

	ts = this_cpu_ptr(&tick_cpu_sched);
	WARN_ON_ONCE(ts->timer_expires_base);
	tick_sched_flag_set(ts, TS_FLAG_INIDLE);
	ts->idle_entrytime = ktime_get();
	tick_nohz_clock_sleep(ts);

	local_irq_enable();
}

/**
 * tick_nohz_irq_exit - Notify the tick about IRQ exit
 *
 * A timer may have been added/modified/deleted either by the current IRQ,
 * or by another place using this IRQ as a notification. This IRQ may have
 * also updated the RCU callback list. These events may require a
 * re-evaluation of the next tick. Depending on the context:
 *
 * 1) If the CPU is idle and no resched is pending, just proceed with idle
 *    time accounting. The next tick will be re-evaluated on the next idle
 *    loop iteration.
 *
 * 2) If the CPU is nohz_full:
 *
 *    2.1) If there is any tick dependency, restart the tick if stopped.
 *
 *    2.2) If there is no tick dependency, (re-)evaluate the next tick and
 *         stop/update it accordingly.
 */
/*
 * tick_nohz_irq_exit() - IRQ 退出时按 idle/full-nohz 上下文收敛 tick 状态。
 * INIDLE 时重新进入 sched_clock sleep、刷新 idle_entrytime；若 STOPPED，先用 kcpustat_irq_exit 结算 IRQ 段，
 * 下一 idle 迭代再重算期限。非 idle 则立即按 full 依赖停止/恢复 tick。调用者处于 IRQ-off exit 路径。
 */
void tick_nohz_irq_exit(void)
{
	struct tick_sched *ts = this_cpu_ptr(&tick_cpu_sched);

	if (tick_sched_flag_test(ts, TS_FLAG_INIDLE)) {
		tick_nohz_clock_sleep(ts);
		ts->idle_entrytime = ktime_get();
		if (tick_sched_flag_test(ts, TS_FLAG_STOPPED))
			kcpustat_irq_exit(ts->idle_entrytime);
	} else {
		tick_nohz_full_update_tick(ts);
	}
}

/**
 * tick_nohz_idle_got_tick - Check whether or not the tick handler has run
 *
 * Return: %true if the tick handler has run, otherwise %false
 */
/* tick_nohz_idle_got_tick() 读取并消费本 CPU got_idle_tick latch；首次返回 true 并清零，之后 false。 */
bool tick_nohz_idle_got_tick(void)
{
	struct tick_sched *ts = this_cpu_ptr(&tick_cpu_sched);

	if (ts->got_idle_tick) {
		ts->got_idle_tick = 0;
		return true;
	}
	return false;
}

/**
 * tick_nohz_get_next_hrtimer - return the next expiration time for the hrtimer
 * or the tick, whichever expires first. Note that, if the tick has been
 * stopped, it returns the next hrtimer.
 *
 * Called from power state control code with interrupts disabled
 *
 * Return: the next expiration time
 */
/*
 * tick_nohz_get_next_hrtimer() - 返回本 CPU 当前 clockevent 的 next_event 镜像。
 * IRQ 必须关闭；值代表 hrtimer/tick 中较早者，tick 已停时即下一 hrtimer。只借用 evtdev，不验证 KTIME_MAX/过期。
 */
ktime_t tick_nohz_get_next_hrtimer(void)
{
	return __this_cpu_read(tick_cpu_device.evtdev)->next_event;
}

/**
 * tick_nohz_get_sleep_length - return the expected length of the current sleep
 * @delta_next: duration until the next event if the tick cannot be stopped
 *
 * Called from power state control code with interrupts disabled.
 *
 * The return value of this function and/or the value returned by it through the
 * @delta_next pointer can be negative which must be taken into account by its
 * callers.
 *
 * Return: the expected length of the current sleep
 */
/*
 * tick_nohz_get_sleep_length() - 为 cpuidle governor 估算当前 idle 可睡时长。
 * IRQ-off 且 INIDLE。以 idle_entrytime 近似 now，先把现有设备期限差写 *@delta_next；不能停 tick 时直接返回它。
 * 可停时调用 next_event 缓存 stop 计算；返回 0 表示保留 tick。否则再与排除 sched_timer 的最早 hrtimer 取小，
 * 返回其相对差。两个差值都允许为负；调用者提供有效输出指针，函数可能写 ts 的预计算字段。
 */
ktime_t tick_nohz_get_sleep_length(ktime_t *delta_next)
{
	struct clock_event_device *dev = __this_cpu_read(tick_cpu_device.evtdev);
	struct tick_sched *ts = this_cpu_ptr(&tick_cpu_sched);
	int cpu = smp_processor_id();
	/*
	 * The idle entry time is expected to be a sufficient approximation of
	 * the current time at this point.
	 */
	/* idle entry 与 governor 查询相邻，省一次 ktime_get；延迟会体现为更短甚至负的结果。 */
	ktime_t now = ts->idle_entrytime;
	ktime_t next_event;

	WARN_ON_ONCE(!tick_sched_flag_test(ts, TS_FLAG_INIDLE));

	*delta_next = ktime_sub(dev->next_event, now);

	if (!can_stop_idle_tick(cpu, ts))
		return *delta_next;

	next_event = tick_nohz_next_event(ts, cpu);
	if (!next_event)
		return *delta_next;

	/*
	 * If the next highres timer to expire is earlier than 'next_event', the
	 * idle governor needs to know that.
	 */
	/* sched_timer 是周期仿真本身，排除它后再纳入真正业务 hrtimer，避免把计划停止的 tick 当唤醒原因。 */
	next_event = min(next_event, hrtimer_next_event_without(&ts->sched_timer));

	return ktime_sub(next_event, now);
}

/**
 * tick_nohz_get_idle_calls_cpu - return the current idle calls counter value
 * for a particular CPU.
 * @cpu: target CPU number
 *
 * Called from the schedutil frequency scaling governor in scheduler context.
 *
 * Return: the current idle calls counter value for @cpu
 */
/* tick_nohz_get_idle_calls_cpu() 无锁读取 @cpu idle_calls 快照，供 schedutil 判断活动；不固定 CPU online 状态。 */
unsigned long tick_nohz_get_idle_calls_cpu(int cpu)
{
	struct tick_sched *ts = tick_get_tick_sched(cpu);

	return ts->idle_calls;
}

/*
 * tick_nohz_idle_restart_tick() - idle trip 内因强制广播/polling 临时恢复已停 tick。
 * 仅 STOPPED 时采新 entrytime，先结束 kcpustat dyntick idle，再完整 restart；更新 entrytime 防同一 idle trip
 * 稍后再次停 tick 时重复计算微小区间。要求 IRQ-off，本函数不清 INIDLE。
 */
void tick_nohz_idle_restart_tick(void)
{
	struct tick_sched *ts = this_cpu_ptr(&tick_cpu_sched);

	if (tick_sched_flag_test(ts, TS_FLAG_STOPPED)) {
		/*
		 * Update entrytime here in case the tick restart is due to temporary
		 * polling on forced broadcast. The tick may be stopped again later within
		 * the same idle trip. The idle_entrytime was updated recently but make sure
		 * no tiny amount of idle time is accounted twice.
		 */
		/* 同一次 idle 内短暂恢复后可能再次停止；重置边界避免前一小段被两个 dyntick 区间重复计入。 */
		ts->idle_entrytime = ktime_get();
		kcpustat_dyntick_stop(ts->idle_entrytime);
		tick_nohz_restart_sched_tick(ts, ts->idle_entrytime);
	}
}

/* 非 full CPU 无条件恢复；full CPU复用依赖决策，可能继续保持 stopped 并只更新下一期限。 */
static void tick_nohz_idle_update_tick(struct tick_sched *ts, ktime_t now)
{
	if (tick_nohz_full_cpu(smp_processor_id()))
		__tick_nohz_full_update_tick(ts, now);
	else
		tick_nohz_restart_sched_tick(ts, now);
}

/**
 * tick_nohz_idle_exit - Update the tick upon idle task exit
 *
 * When the idle task exits, update the tick depending on the
 * following situations:
 *
 * 1) If the CPU is not in nohz_full mode (most cases), then
 *    restart the tick.
 *
 * 2) If the CPU is in nohz_full mode (corner case):
 *   2.1) If the tick can be kept stopped (no tick dependencies)
 *        then re-evaluate the next tick and try to keep it stopped
 *        as long as possible.
 *   2.2) If the tick has dependencies, restart the tick.
 *
 */
/*
 * tick_nohz_idle_exit() - idle task 离开时关闭 idle 记账并收敛 tick。
 * 函数自行关/开 IRQ，验证 INIDLE 与无残留预计算，清 INIDLE、唤醒 sched_clock；若 STOPPED，采 now、结束
 * kcpustat idle，并按普通/full CPU 规则恢复或继续停 tick。无返回值，返回后 IDLE_ACTIVE 已清。
 */
void tick_nohz_idle_exit(void)
{
	struct tick_sched *ts = this_cpu_ptr(&tick_cpu_sched);
	ktime_t now;

	local_irq_disable();

	WARN_ON_ONCE(!tick_sched_flag_test(ts, TS_FLAG_INIDLE));
	WARN_ON_ONCE(ts->timer_expires_base);

	tick_sched_flag_clear(ts, TS_FLAG_INIDLE);
	tick_nohz_clock_wakeup(ts);

	if (tick_sched_flag_test(ts, TS_FLAG_STOPPED)) {
		now = ktime_get();
		kcpustat_dyntick_stop(now);
		tick_nohz_idle_update_tick(ts, now);
	}

	local_irq_enable();
}

/*
 * In low-resolution mode, the tick handler must be implemented directly
 * at the clockevent level. hrtimer can't be used instead, because its
 * infrastructure actually relies on the tick itself as a backend in
 * low-resolution mode (see hrtimer_run_queues()).
 */
/*
 * tick_nohz_lowres_handler() - 低分辨率 NO_HZ 的 clockevent IRQ handler。
 * 先清设备 next_event/forced 镜像，再用嵌入 sched_timer 调公共 nohz handler；若其要求 RESTART，读取 forward 后
 * expires 并 force 重编设备，NORESTART 则交 idle/IRQ-exit。hrtimer 低分辨率依赖 tick，不能反过来承载此入口。
 */
static void tick_nohz_lowres_handler(struct clock_event_device *dev)
{
	struct tick_sched *ts = this_cpu_ptr(&tick_cpu_sched);

	dev->next_event = KTIME_MAX;
	dev->next_event_forced = 0;

	if (likely(tick_nohz_handler(&ts->sched_timer) == HRTIMER_RESTART))
		tick_program_event(hrtimer_get_expires(&ts->sched_timer), 1);
}

/*
 * tick_nohz_activate() - 在允许时给本 CPU置 NOHZ，并首次全局激活时通知 timer 层刷新静态分支。
 * `nohz=off` 无动作；test_and_set_bit 保证 timers_update_nohz 全系统仅首个 CPU 调一次。
 */
static inline void tick_nohz_activate(struct tick_sched *ts)
{
	if (!tick_nohz_enabled)
		return;
	tick_sched_flag_set(ts, TS_FLAG_NOHZ);
	/* One update is enough */
	/* active 是全局发布位，后续 CPU只需置自己的 NOHZ flag，无需重复刷新 timer 迁移状态。 */
	if (!test_and_set_bit(0, &tick_nohz_active))
		timers_update_nohz();
}

/**
 * tick_nohz_switch_to_nohz - switch to NOHZ mode
 */
/*
 * tick_nohz_switch_to_nohz() - 在 highres 不可用时把本 CPU 切到低分辨率 NO_HZ。
 * 禁用 nohz 或 oneshot switch 失败直接返回；成功安装 lowres clockevent handler，再以 hrtimer=false 初始化
 * sched_timer 仅作期限/公共状态容器。要求 IRQ-off，setup 最终激活 NOHZ。
 */
static void tick_nohz_switch_to_nohz(void)
{
	if (!tick_nohz_enabled)
		return;

	if (tick_switch_to_oneshot(tick_nohz_lowres_handler))
		return;

	/*
	 * Recycle the hrtimer in 'ts', so we can share the
	 * highres code.
	 */
	/* lowres 不启动 hrtimer 硬件，只复用其 expires 与 callback 逻辑，实际中断由 clockevent 提供。 */
	tick_setup_sched_timer(false);
}

/*
 * tick_nohz_irq_enter() - idle CPU进入 IRQ 时切换 sched_clock/kcpustat 并刷新 jiffies。
 * 先幂等 clock wake；tick 未停则返回。STOPPED 时采 now、开始 IRQ 记账，并无条件追赶 jiffies，作为负责人长时间
 * IRQ-off 的最后补救。调用者是 IRQ entry 且中断关闭，不直接恢复周期 tick。
 */
static inline void tick_nohz_irq_enter(void)
{
	struct tick_sched *ts = this_cpu_ptr(&tick_cpu_sched);
	ktime_t now;

	tick_nohz_clock_wakeup(ts);

	if (!tick_sched_flag_test(ts, TS_FLAG_STOPPED))
		return;

	now = ktime_get();
	kcpustat_irq_enter(now);

	/*
	 * If all CPUs are idle we may need to update a stale jiffies value.
	 * Note nohz_full is a special case: a timekeeper is guaranteed to stay
	 * alive but it might be busy looping with interrupts disabled in some
	 * rare case (typically stop machine). So we must make sure we have a
	 * last resort.
	 */
	/* 即使 full 模式固定负责人，它也可能在 stop_machine 等场景长时间关 IRQ；任意 idle IRQ 都作为补推进后备。 */
	tick_nohz_update_jiffies(now);
}

#else

/* CONFIG_NO_HZ_COMMON=n 时三个入口均无副作用，保留通用 IRQ/setup 调用点而不创建 tickless 状态。 */
static inline void tick_nohz_switch_to_nohz(void) { }
static inline void tick_nohz_irq_enter(void) { }
static inline void tick_nohz_activate(struct tick_sched *ts) { }

#endif /* CONFIG_NO_HZ_COMMON */

/*
 * Called from irq_enter() to notify about the possible interruption of idle()
 */
/*
 * tick_irq_enter() - 通用 IRQ entry 对 tick 子系统的通知入口。
 * 先让 broadcast oneshot 检查本 CPU是否需退出广播，再执行 NO_HZ idle wake/jiffies 刷新；禁配实现均为空。
 * 调用者已关 IRQ，本函数无返回值，实际下一 tick 决策延后至 IRQ exit/idle loop。
 */
void tick_irq_enter(void)
{
	tick_check_oneshot_broadcast_this_cpu();
	tick_nohz_irq_enter();
}

static int sched_skew_tick;
/* sched_skew_tick 由 early param 控制是否按 CPU 编号错开周期 tick，以降低 jiffies_lock 同时竞争。 */

/* skew_tick() 用 get_option 解析整数到全局开关并始终返回 0，early_param 框架据此把处理视为成功。 */
static int __init skew_tick(char *str)
{
	get_option(&str, &sched_skew_tick);

	return 0;
}
early_param("skew_tick", skew_tick);

/**
 * tick_setup_sched_timer - setup the tick emulation timer
 * @hrtimer: whether to use the hrtimer or not
 */
/*
 * tick_setup_sched_timer() - 初始化本 CPU 周期 tick 仿真及首次 deadline。
 * 总以 MONOTONIC ABS_HARD 初始化嵌入 sched_timer；highres 配置且 @hrtimer 时置 HIGHRES。以全局 jiffy 锚点
 * 设置 expires，可选按 CPU 编号错开半 tick/possible_cpu 数以减锁竞争，再 forward 到 now 后。highres 启动
 * pinned hard hrtimer，lowres force 编 clockevent，最后激活 NOHZ。要求 IRQ-off/CPU stable，不返回编程错误。
 */
void tick_setup_sched_timer(bool hrtimer)
{
	struct tick_sched *ts = this_cpu_ptr(&tick_cpu_sched);

	/* Emulate tick processing via per-CPU hrtimers: */
	/* 同一 timer 在 highres 真正入队，在 lowres 只保存公共 callback/expires 状态。 */
	hrtimer_setup(&ts->sched_timer, tick_nohz_handler, CLOCK_MONOTONIC, HRTIMER_MODE_ABS_HARD);

	if (IS_ENABLED(CONFIG_HIGH_RES_TIMERS) && hrtimer)
		tick_sched_flag_set(ts, TS_FLAG_HIGHRES);

	/* Get the next period (per-CPU) */
	/* 所有 CPU 从同一 jiffy 周期锚点起步，随后可选 skew。 */
	hrtimer_set_expires(&ts->sched_timer, tick_init_jiffy_update());

	/* Offset the tick to avert 'jiffies_lock' contention. */
	/* skew 总跨度不足半个 tick，按 CPU id 线性错开，降低同一时刻争用而不改变频率。 */
	if (sched_skew_tick) {
		u64 offset = TICK_NSEC >> 1;
		do_div(offset, num_possible_cpus());
		offset *= smp_processor_id();
		hrtimer_add_expires_ns(&ts->sched_timer, offset);
	}

	hrtimer_forward_now(&ts->sched_timer, TICK_NSEC);
	if (IS_ENABLED(CONFIG_HIGH_RES_TIMERS) && hrtimer)
		hrtimer_start_expires(&ts->sched_timer, HRTIMER_MODE_ABS_PINNED_HARD);
	else
		tick_program_event(hrtimer_get_expires(&ts->sched_timer), 1);
	tick_nohz_activate(ts);
}

/*
 * Shut down the tick and make sure the CPU won't try to retake the timekeeping
 * duty before disabling IRQs in idle for the last time.
 */
/*
 * tick_sched_timer_dying() - CPU teardown 时终止 sched timer 并重置 per-CPU NO_HZ 状态。
 * 必须在 hrtimer 迁移前调用；HIGHRES 时同步 cancel。保存累计 idle_calls/sleeps，memset 其余字段（含 STOPPED、
 * LAST、依赖与期限）后恢复两计数。@cpu 由 hotplug 串行保证离线且无人并发使用；不转交 do_timer 职责本身。
 */
void tick_sched_timer_dying(int cpu)
{
	struct tick_sched *ts = &per_cpu(tick_cpu_sched, cpu);
	unsigned long idle_calls, idle_sleeps;

	/* This must happen before hrtimers are migrated! */
	/* 否则 sched_timer 可能已迁到别 CPU，按旧 per-CPU 容器 cancel/清零会破坏新归属。 */
	if (tick_sched_flag_test(ts, TS_FLAG_HIGHRES))
		hrtimer_cancel(&ts->sched_timer);

	idle_calls = ts->idle_calls;
	idle_sleeps = ts->idle_sleeps;
	memset(ts, 0, sizeof(*ts));
	ts->idle_calls = idle_calls;
	ts->idle_sleeps = idle_sleeps;
}

/*
 * Async notification about clocksource changes
 */
/* tick_clock_notify() 给所有 possible CPU 的 check_clocks 置 bit0；异步、可合并，不发送 IPI或立即切模式。 */
void tick_clock_notify(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		set_bit(0, &per_cpu(tick_cpu_sched, cpu).check_clocks);
}

/*
 * Async notification about clock event changes
 */
/* tick_oneshot_notify() 只给当前 CPU置 check_clocks bit0，等待 hrtimer/timer softirq 周期检查。 */
void tick_oneshot_notify(void)
{
	struct tick_sched *ts = this_cpu_ptr(&tick_cpu_sched);

	set_bit(0, &ts->check_clocks);
}

/*
 * Check if a change happened, which makes oneshot possible.
 *
 * Called cyclically from the hrtimer softirq (driven by the timer
 * softirq). 'allow_nohz' signals that we can switch into low-res NOHZ
 * mode, because high resolution timers are disabled (either compile
 * or runtime). Called with interrupts disabled.
 */
/*
 * tick_check_oneshot_change() - 消费当前 CPU clock change 通知并判断是否可切 oneshot。
 * 无 pending 或已 NOHZ 返回 0；timekeeping 不适合 highres/设备不支持也返回 0。条件满足且 @allow_nohz=false
 * 返回 1，通知 highres 层执行切换；allow=true 则本地切低分辨率 NO_HZ 后返回 0。bit 在判定前已清，即使条件
 * 暂不满足也需未来新 notify 才重试。要求 IRQ-off，返回值不是 errno。
 */
int tick_check_oneshot_change(int allow_nohz)
{
	struct tick_sched *ts = this_cpu_ptr(&tick_cpu_sched);

	if (!test_and_clear_bit(0, &ts->check_clocks))
		return 0;

	if (tick_sched_flag_test(ts, TS_FLAG_NOHZ))
		return 0;

	if (!timekeeping_valid_for_hres() || !tick_is_oneshot_available())
		return 0;

	if (!allow_nohz)
		return 1;

	tick_nohz_switch_to_nohz();
	return 0;
}
