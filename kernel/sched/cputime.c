// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simple CPU accounting cgroup controller
 */
/*
 * 本文件把“CPU 上流逝的时间”归入 task、thread group、cgroup 与每 CPU
 * cpustat。入口分成三类：周期 tick 的抽样计费、IRQ/虚拟化提供的精确
 * 计费，以及 NO_HZ CPU 在读取时补算尚未落账的区间。
 *
 * 核心不变量是同一段时间只进入一种基础类别；guest 同时是 task 的
 * user time，但另以 GUEST/GUEST_NICE 记录其子集。当前 CPU 的写路径通常
 * 已关中断或持 rq 锁；跨 CPU 读取只承诺一致或可重试的快照，并不冻结
 * 正在运行的任务。seqcount 保护 vtime/idle 快照，prev_cputime 自旋锁则
 * 保证导出的 user/system 值单调且二者之和等于调度器运行时间。
 */
#include <linux/sched/clock.h>
#include <linux/sched/cputime.h>
#include <linux/tsacct_kern.h>
#include "sched.h"

#ifdef CONFIG_VIRT_CPU_ACCOUNTING_NATIVE
 #include <asm/cputime.h>
#endif

#ifdef CONFIG_IRQ_TIME_ACCOUNTING

DEFINE_STATIC_KEY_FALSE(sched_clock_irqtime);

/*
 * There are no locks covering percpu hardirq/softirq time.
 * They are only modified in vtime_account, on corresponding CPU
 * with interrupts disabled. So, writes are safe.
 * They are read and saved off onto struct rq in update_rq_clock().
 * This may result in other CPU reading this CPU's IRQ time and can
 * race with irq/vtime_account on this CPU. We would either get old
 * or new value with a side effect of accounting a slice of IRQ time to wrong
 * task when IRQ is in progress while we read rq->clock. That is a worthy
 * compromise in place of having locks on each IRQ in account_system_time.
 */
DEFINE_PER_CPU(struct irqtime, cpu_irqtime);

/* 开启 IRQ 精确计费静态分支；无入参和返回值，切换会影响所有 CPU 的后续入口。 */

void enable_sched_clock_irqtime(void)
{
	static_branch_enable(&sched_clock_irqtime);
}

/* 关闭已启用的 IRQ 精确计费；无入参和返回值，未启用时保持不变。 */
void disable_sched_clock_irqtime(void)
{
	if (irqtime_enabled())
		static_branch_disable(&sched_clock_irqtime);
}

/*
 * 把本 CPU 自上次 IRQ 边界以来的 @delta 纳秒提交到 @idx，并同步累计
 * irqtime 总量/tick 待扣量；调用者在本 CPU 关中断上下文中借用 @irqtime。
 */
static void irqtime_account_delta(struct irqtime *irqtime, u64 delta,
				  enum cpu_usage_stat idx)
{
	u64 *cpustat = kcpustat_this_cpu->cpustat;

	u64_stats_update_begin(&irqtime->sync);
	cpustat[idx] += delta;
	irqtime->total += delta;
	if (!kcpustat_idle_dyntick())
		irqtime->tick_delta += delta;
	u64_stats_update_end(&irqtime->sync);
}

/*
 * Called after incrementing preempt_count on {soft,}irq_enter
 * and before decrementing preempt_count on {soft,}irq_exit.
 */
/*
 * 必须在 irq_enter 已增加、irq_exit 尚未减少 preempt_count 的窗口调用，
 * @offset 用来扣掉调用点自身层级；据此区分 hardirq、softirq 与 ksoftirqd。
 */
void irqtime_account_irq(struct task_struct *curr, unsigned int offset)
{
	struct irqtime *irqtime = this_cpu_ptr(&cpu_irqtime);
	unsigned int pc;
	s64 delta;
	int cpu;

	if (!irqtime_enabled())
		return;

	cpu = smp_processor_id();
	delta = sched_clock_cpu(cpu) - irqtime->irq_start_time;
	irqtime->irq_start_time += delta;
	pc = irq_count() - offset;

	/*
	 * We do not account for softirq time from ksoftirqd here.
	 * We want to continue accounting softirq time to ksoftirqd thread
	 * in that case, so as not to confuse scheduler with a special task
	 * that do not consume any time, but still wants to run.
	 */
	if (pc & HARDIRQ_MASK)
		irqtime_account_delta(irqtime, delta, CPUTIME_IRQ);
	else if ((pc & SOFTIRQ_OFFSET) && curr != this_cpu_ksoftirqd())
		irqtime_account_delta(irqtime, delta, CPUTIME_SOFTIRQ);
}

/* 从 tick_delta 最多领取 @maxtime 纳秒，返回本 tick 不得再归给 task 的时间。 */
static u64 irqtime_tick_accounted(u64 maxtime)
{
	struct irqtime *irqtime = this_cpu_ptr(&cpu_irqtime);
	u64 delta;

	delta = min(irqtime->tick_delta, maxtime);
	irqtime->tick_delta -= delta;

	return delta;
}

#else /* !CONFIG_IRQ_TIME_ACCOUNTING: */

/* 未配置 IRQ_TIME 时没有可领取的独立 IRQ 时间，恒返回 0。 */
static u64 irqtime_tick_accounted(u64 dummy)
{
	return 0;
}

#endif /* !CONFIG_IRQ_TIME_ACCOUNTING */

/*
 * 把 @tmp 纳秒写入本 CPU 根 cpustat，再沿 @p 的 cgroup 层级计入 @index；
 * @p 只是借用，调用点负责保证本 CPU 计费上下文稳定。
 */
static inline void task_group_account_field(struct task_struct *p, int index,
					    u64 tmp)
{
	/*
	 * Since all updates are sure to touch the root cgroup, we
	 * get ourselves ahead and touch it first. If the root cgroup
	 * is the only cgroup, then nothing else should be necessary.
	 *
	 */
	__this_cpu_add(kernel_cpustat.cpustat[index], tmp);

	cgroup_account_cputime_field(p, index, tmp);
}

/*
 * Account user CPU time to a process.
 * @p: the process that the CPU time gets accounted to
 * @cputime: the CPU time spent in user space since the last update
 */
/* @p 为当前运行 task，@cputime 为本次新增纳秒；同时更新 task、线程组、cgroup 和进程记账。 */
void account_user_time(struct task_struct *p, u64 cputime)
{
	int index;

	/* Add user time to process. */
	p->utime += cputime;
	account_group_user_time(p, cputime);

	index = (task_nice(p) > 0) ? CPUTIME_NICE : CPUTIME_USER;

	/* Add user time to cpustat. */
	task_group_account_field(p, index, cputime);

	/* Account for user time used */
	acct_account_cputime(p);
}

/*
 * Account guest CPU time to a process.
 * @p: the process that the CPU time gets accounted to
 * @cputime: the CPU time spent in virtual machine since the last update
 */
/* guest 时间既累计进 user 总量，也按 nice 状态写入独立 guest 子类别；无直接返回值。 */
void account_guest_time(struct task_struct *p, u64 cputime)
{
	u64 *cpustat = kcpustat_this_cpu->cpustat;

	/* Add guest time to process. */
	p->utime += cputime;
	account_group_user_time(p, cputime);
	p->gtime += cputime;

	/* Add guest time to cpustat. */
	if (task_nice(p) > 0) {
		task_group_account_field(p, CPUTIME_NICE, cputime);
		cpustat[CPUTIME_GUEST_NICE] += cputime;
	} else {
		task_group_account_field(p, CPUTIME_USER, cputime);
		cpustat[CPUTIME_GUEST] += cputime;
	}
}

/*
 * Account system CPU time to a process and desired cpustat field
 * @p: the process that the CPU time gets accounted to
 * @cputime: the CPU time spent in kernel space since the last update
 * @index: pointer to cpustat field that has to be updated
 */
/* 把 @cputime 纳秒归给 @p 的 system 总量及调用者选择的 cpustat @index。 */
void account_system_index_time(struct task_struct *p,
			       u64 cputime, enum cpu_usage_stat index)
{
	/* Add system time to process. */
	p->stime += cputime;
	account_group_system_time(p, cputime);

	/* Add system time to cpustat. */
	task_group_account_field(p, index, cputime);

	/* Account for system time used */
	acct_account_cputime(p);
}

/*
 * Account system CPU time to a process.
 * @p: the process that the CPU time gets accounted to
 * @hardirq_offset: the offset to subtract from hardirq_count()
 * @cputime: the CPU time spent in kernel space since the last update
 */
/*
 * 根据 PF_VCPU 与当前 IRQ 嵌套状态把内核态区间分流到 guest、hardirq、
 * softirq 或普通 system；@hardirq_offset 排除调用点人为增加的层级。
 */
void account_system_time(struct task_struct *p, int hardirq_offset, u64 cputime)
{
	int index;

	if ((p->flags & PF_VCPU) && (irq_count() - hardirq_offset == 0)) {
		account_guest_time(p, cputime);
		return;
	}

	if (hardirq_count() - hardirq_offset)
		index = CPUTIME_IRQ;
	else if (in_serving_softirq())
		index = CPUTIME_SOFTIRQ;
	else
		index = CPUTIME_SYSTEM;

	account_system_index_time(p, cputime, index);
}

/*
 * Account for involuntary wait time.
 * @cputime: the CPU time spent in involuntary wait
 */
/* 把 hypervisor 抢占 guest 的 @cputime 纳秒写入本 CPU STEAL 类别。 */
void account_steal_time(u64 cputime)
{
	u64 *cpustat = kcpustat_this_cpu->cpustat;

	cpustat[CPUTIME_STEAL] += cputime;
}

/*
 * Account for idle time.
 * @cputime: the CPU time spent in idle wait
 */
/* idle 区间按采样时 nr_iowait 是否非零归入 IOWAIT 或 IDLE；该分类是近似快照。 */
void account_idle_time(u64 cputime)
{
	u64 *cpustat = kcpustat_this_cpu->cpustat;
	struct rq *rq = this_rq();

	if (atomic_read(&rq->nr_iowait) > 0)
		cpustat[CPUTIME_IOWAIT] += cputime;
	else
		cpustat[CPUTIME_IDLE] += cputime;
}


#ifdef CONFIG_SCHED_CORE
/*
 * Account for forceidle time due to core scheduling.
 *
 * REQUIRES: schedstat is enabled.
 */
/* core scheduling 强制空转时，为受影响 task 的统计和 cgroup 增加 @delta 纳秒。 */
void __account_forceidle_time(struct task_struct *p, u64 delta)
{
	__schedstat_add(p->stats.core_forceidle_sum, delta);

	task_group_account_field(p, CPUTIME_FORCEIDLE, delta);
}
#endif /* CONFIG_SCHED_CORE */

/*
 * When a guest is interrupted for a longer amount of time, missed clock
 * ticks are not redelivered later. Due to that, this function may on
 * occasion account more time than the calling functions think elapsed.
 */
#ifdef CONFIG_PARAVIRT
struct static_key paravirt_steal_enabled;

#ifdef CONFIG_HAVE_PV_STEAL_CLOCK_GEN
/* 原生后备实现没有 paravirt steal clock，@cpu 任意时均返回 0。 */
static u64 native_steal_clock(int cpu)
{
	return 0;
}

DEFINE_STATIC_CALL(pv_steal_clock, native_steal_clock);
#endif
#endif

/*
 * 从平台累计 steal clock 领取不超过 @maxtime 的增量并推进 rq 游标；
 * 静态分支关闭或未配置 PARAVIRT 时返回 0。
 */
static __always_inline u64 steal_account_process_time(u64 maxtime)
{
#ifdef CONFIG_PARAVIRT
	if (static_key_false(&paravirt_steal_enabled)) {
		u64 steal;

		steal = paravirt_steal_clock(smp_processor_id());
		steal -= this_rq()->prev_steal_time;
		steal = min(steal, maxtime);
		account_steal_time(steal);
		this_rq()->prev_steal_time += steal;

		return steal;
	}
#endif /* CONFIG_PARAVIRT */
	return 0;
}

/*
 * Account how much elapsed time was spent in steal, IRQ, or softirq time.
 */
/* 关中断调用；先扣 steal 再扣 IRQ，返回总领取量且绝不超过 @max。 */
static inline u64 account_other_time(u64 max)
{
	u64 accounted;

	lockdep_assert_irqs_disabled();

	accounted = steal_account_process_time(max);

	if (accounted < max)
		accounted += irqtime_tick_accounted(max - accounted);

	return accounted;
}

#ifdef CONFIG_64BIT
/* 64 位平台可原子读取 task 的累计运行纳秒，返回借用快照。 */
static inline u64 read_sum_exec_runtime(struct task_struct *t)
{
	return t->se.sum_exec_runtime;
}
#else /* !CONFIG_64BIT: */
/* 32 位平台用 task rq 锁防止 64 位字段撕裂，返回稳定的累计运行纳秒。 */
static u64 read_sum_exec_runtime(struct task_struct *t)
{
	u64 ns;
	struct rq_flags rf;
	struct rq *rq;

	rq = task_rq_lock(t, &rf);
	ns = t->se.sum_exec_runtime;
	task_rq_unlock(rq, t, &rf);

	return ns;
}
#endif /* !CONFIG_64BIT */

/*
 * Accumulate raw cputime values of dead tasks (sig->[us]time) and live
 * tasks (sum on group iteration) belonging to @tsk's group.
 */
/*
 * 把 @tsk 线程组中已死亡线程的 signal 累计值与存活线程快照合并写入
 * 输出参数 @times；RCU 稳定线程链，stats_lock seqcount 使死亡累计与遍历一致。
 */
void thread_group_cputime(struct task_struct *tsk, struct task_cputime *times)
{
	struct signal_struct *sig = tsk->signal;
	struct task_struct *t;
	u64 utime, stime;

	/*
	 * Update current task runtime to account pending time since last
	 * scheduler action or thread_group_cputime() call. This thread group
	 * might have other running tasks on different CPUs, but updating
	 * their runtime can affect syscall performance, so we skip account
	 * those pending times and rely only on values updated on tick or
	 * other scheduler action.
	 */
	if (same_thread_group(current, tsk))
		(void) task_sched_runtime(current);

	guard(rcu)();
	scoped_seqlock_read (&sig->stats_lock, ss_lock_irqsave) {
		times->utime = sig->utime;
		times->stime = sig->stime;
		times->sum_exec_runtime = sig->sum_sched_runtime;

		__for_each_thread(sig, t) {
			task_cputime(t, &utime, &stime);
			times->utime += utime;
			times->stime += stime;
			times->sum_exec_runtime += read_sum_exec_runtime(t);
		}
	}
}

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
/*
 * Account a tick to a process and cpustat
 * @p: the process that the CPU time gets accounted to
 * @user_tick: is the tick from userspace
 * @rq: the pointer to rq
 *
 * Tick demultiplexing follows the order
 * - pending hardirq update
 * - pending softirq update
 * - user_time
 * - idle_time
 * - system time
 *   - check for guest_time
 *   - else account as system_time
 *
 * Check for hardirq is done both for system and user time as there is
 * no timer going off while we are on hardirq and hence we may never get an
 * opportunity to update it solely in system time.
 * p->stime and friends are only updated on system time and not on IRQ
 * softirq as those do not count in task exec_runtime any more.
 */
/*
 * 将 @ticks 个周期先扣除尚未落账的 steal/IRQ，再按 ksoftirqd、user、
 * idle、guest、system 的互斥顺序归类；@p 借用且必须是本 CPU 当前任务。
 */
static void irqtime_account_process_tick(struct task_struct *p, int user_tick,
					 int ticks)
{
	u64 other, cputime = TICK_NSEC * ticks;

	/*
	 * When returning from idle, many ticks can get accounted at
	 * once, including some ticks of steal, IRQ, and softirq time.
	 * Subtract those ticks from the amount of time accounted to
	 * idle, or potentially user or system time. Due to rounding,
	 * other time can exceed ticks occasionally.
	 */
	other = account_other_time(ULONG_MAX);
	if (other >= cputime)
		return;

	cputime -= other;

	if (this_cpu_ksoftirqd() == p) {
		/*
		 * ksoftirqd time do not get accounted in cpu_softirq_time.
		 * So, we have to handle it separately here.
		 * Also, p->stime needs to be updated for ksoftirqd.
		 */
		account_system_index_time(p, cputime, CPUTIME_SOFTIRQ);
	} else if (user_tick) {
		account_user_time(p, cputime);
	} else if (p == this_rq()->idle) {
		account_idle_time(cputime);
	} else if (p->flags & PF_VCPU) { /* System time or guest time */
		account_guest_time(p, cputime);
	} else {
		account_system_index_time(p, cputime, CPUTIME_SYSTEM);
	}
}

#else /* !CONFIG_IRQ_TIME_ACCOUNTING: */
/* 配置关闭时 tick 不需要做 IRQ 独立分流，调用者继续普通 tick 计费。 */
static inline void irqtime_account_process_tick(struct task_struct *p, int user_tick,
						int nr_ticks) { }
#endif /* !CONFIG_IRQ_TIME_ACCOUNTING */

#if defined(CONFIG_NO_HZ_COMMON) && !defined(CONFIG_HAVE_VIRT_CPU_ACCOUNTING_IDLE)
/*
 * 结束 @kc 从 idle_entrytime 到 @now 的动态 tick 空闲段；先消费上轮延迟的
 * steal，避免公开 idle 值倒退，再将本轮 steal 留给下一次扣除。
 */
static void kcpustat_idle_stop(struct kernel_cpustat *kc, u64 now)
{
	u64 *cpustat = kc->cpustat;
	u64 delta, steal, steal_delta;
	int iowait;

	if (!kc->idle_elapse)
		return;

	iowait = nr_iowait_cpu(smp_processor_id()) > 0;
	delta = now - kc->idle_entrytime;
	steal = steal_account_process_time(delta);

	/*
	 * Record the idle time after substracting the steal time from
	 * previous update sequence. Don't substract the steal time from
	 * the current update sequence to avoid readers moving backward.
	 */
	write_seqcount_begin(&kc->idle_sleeptime_seq);
	steal_delta = min_t(u64, kc->idle_stealtime[iowait], delta);
	delta -= steal_delta;
	kc->idle_stealtime[iowait] -= steal_delta;

	if (iowait)
		cpustat[CPUTIME_IOWAIT] += delta;
	else
		cpustat[CPUTIME_IDLE] += delta;

	kc->idle_stealtime[iowait] += steal;
	kc->idle_entrytime = now;
	kc->idle_elapse = false;
	write_seqcount_end(&kc->idle_sleeptime_seq);
}

/* 在 seqcount 下开启 @kc 的动态 tick 空闲区间；重复 start 是安全空操作。 */
static void kcpustat_idle_start(struct kernel_cpustat *kc, u64 now)
{
	/* Irqtime accounting might have been enabled in the middle of the IRQ */
	if (kc->idle_elapse)
		return;

	write_seqcount_begin(&kc->idle_sleeptime_seq);
	kc->idle_entrytime = now;
	kc->idle_elapse = true;
	write_seqcount_end(&kc->idle_sleeptime_seq);
}

/* CPU 离开 dyntick idle 时结算空闲段并恢复普通 vtime；@now 为纳秒时间点。 */
void kcpustat_dyntick_stop(u64 now)
{
	struct kernel_cpustat *kc = kcpustat_this_cpu;

	if (!vtime_generic_enabled_this_cpu()) {
		WARN_ON_ONCE(!kc->idle_dyntick);
		kcpustat_idle_stop(kc, now);
		kc->idle_dyntick = false;
		vtime_dyntick_stop();
	}
}

/* CPU 进入 dyntick idle 时启动 vtime 与 cpustat 的空闲区间。 */
void kcpustat_dyntick_start(u64 now)
{
	struct kernel_cpustat *kc = kcpustat_this_cpu;

	if (!vtime_generic_enabled_this_cpu()) {
		vtime_dyntick_start();
		kc->idle_dyntick = true;
		kcpustat_idle_start(kc, now);
	}
}

/* IRQ 打断 idle 前暂停 NO_HZ 空闲累计，避免同一段时间同时记为 idle 和 IRQ。 */
void kcpustat_irq_enter(u64 now)
{
	struct kernel_cpustat *kc = kcpustat_this_cpu;

	if (!vtime_generic_enabled_this_cpu() &&
	    (irqtime_enabled() || vtime_accounting_enabled_this_cpu()))
		kcpustat_idle_stop(kc, now);
}

/* IRQ 返回 idle 后重新开始 NO_HZ 空闲累计；运行时禁用 irqtime 也必须配平 enter。 */
void kcpustat_irq_exit(u64 now)
{
	struct kernel_cpustat *kc = kcpustat_this_cpu;

	/*
	 * Generic vtime already does its own idle accounting.
	 * But irqtime accounting or arch vtime which also accounts IRQs
	 * need to pause nohz accounting. Resume nohz accounting as long
	 * as the irqtime config is enabled to handle case where irqtime
	 * accounting got runtime disabled in the middle of an IRQ.
	 */
	if (!vtime_generic_enabled_this_cpu() &&
	    (IS_ENABLED(CONFIG_IRQ_TIME_ACCOUNTING) || vtime_accounting_enabled_this_cpu()))
		kcpustat_idle_start(kc, now);
}

/*
 * 读取 @cpu 的 @idx 累计值，并在请求时补算尚未 stop 的当前空闲段；
 * seqcount 失败则重试，因此不会组合 start/stop 两代状态。
 */
static u64 kcpustat_field_dyntick(int cpu, enum cpu_usage_stat idx,
				  bool compute_delta, u64 now)
{
	struct kernel_cpustat *kc = &kcpustat_cpu(cpu);
	int iowait = idx == CPUTIME_IOWAIT;
	u64 *cpustat = kc->cpustat;
	unsigned int seq;
	u64 idle;

	do {
		seq = read_seqcount_begin(&kc->idle_sleeptime_seq);

		idle = cpustat[idx];

		if (kc->idle_elapse && compute_delta && now > kc->idle_entrytime) {
			u64 delta = now - kc->idle_entrytime;

			delta -= min_t(u64, kc->idle_stealtime[iowait], delta);
			idle += delta;
		}
	} while (read_seqcount_retry(&kc->idle_sleeptime_seq, seq));

	return idle;
}

/* 返回 @cpu 的累计 idle 纳秒；有 iowait 时不把当前未结算区间算入 idle。 */
u64 kcpustat_field_idle(int cpu)
{
	return kcpustat_field_dyntick(cpu, CPUTIME_IDLE,
				      !nr_iowait_cpu(cpu), ktime_get());
}
EXPORT_SYMBOL_GPL(kcpustat_field_idle);

/* 返回 @cpu 的累计 iowait 纳秒；仅在当前存在 iowait waiter 时补算开放区间。 */
u64 kcpustat_field_iowait(int cpu)
{
	return kcpustat_field_dyntick(cpu, CPUTIME_IOWAIT,
				      nr_iowait_cpu(cpu), ktime_get());
}
EXPORT_SYMBOL_GPL(kcpustat_field_iowait);
#else
/* 无 NO_HZ 补算能力时直接返回已提交 cpustat 字段，其他参数不改变结果。 */
static u64 kcpustat_field_dyntick(int cpu, enum cpu_usage_stat idx,
				  bool compute_delta, ktime_t now)
{
	return kcpustat_cpu(cpu).cpustat[idx];
}
#endif /* CONFIG_NO_HZ_COMMON && !CONFIG_HAVE_VIRT_CPU_ACCOUNTING_IDLE */

/* 将指定 sleep 字段转换为微秒；可选输出 @last_update_time，不转移任何所有权。 */
static u64 get_cpu_sleep_time_us(int cpu, enum cpu_usage_stat idx,
				 bool compute_delta, u64 *last_update_time)
{
	ktime_t now = ktime_get();
	u64 res;

	if (vtime_generic_enabled_cpu(cpu))
		res = kcpustat_field(idx, cpu);
	else
		res = kcpustat_field_dyntick(cpu, idx, compute_delta, now);

	do_div(res, NSEC_PER_USEC);

	if (last_update_time)
		*last_update_time = ktime_to_us(now);

	return res;
}

/**
 * get_cpu_idle_time_us - get the total idle time of a CPU
 * @cpu: CPU number to query
 * @last_update_time: variable to store update time in. Do not update
 * counters if NULL.
 *
 * Return the cumulative idle time (since boot) for a given
 * CPU, in microseconds. Note that this is partially broken due to
 * the counter of iowait tasks that can be remotely updated without
 * any synchronization. Therefore it is possible to observe backward
 * values within two consecutive reads.
 *
 * This time is measured via accounting rather than sampling,
 * and is as accurate as ktime_get() is.
 *
 * Return: total idle time of the @cpu
 */
/* 返回 @cpu 自启动以来累计 idle 微秒；远端 iowait 竞态允许相邻读数暂时倒退。 */
u64 get_cpu_idle_time_us(int cpu, u64 *last_update_time)
{
	return get_cpu_sleep_time_us(cpu, CPUTIME_IDLE,
				     !nr_iowait_cpu(cpu), last_update_time);
}
EXPORT_SYMBOL_GPL(get_cpu_idle_time_us);

/**
 * get_cpu_iowait_time_us - get the total iowait time of a CPU
 * @cpu: CPU number to query
 * @last_update_time: variable to store update time in. Do not update
 * counters if NULL.
 *
 * Return the cumulative iowait time (since boot) for a given
 * CPU, in microseconds. Note this is partially broken due to
 * the counter of iowait tasks that can be remotely updated without
 * any synchronization. Therefore it is possible to observe backward
 * values within two consecutive reads.
 *
 * This time is measured via accounting rather than sampling,
 * and is as accurate as ktime_get() is.
 *
 * Return: total iowait time of @cpu
 */
/* 返回 @cpu 自启动以来累计 iowait 微秒；@last_update_time 为可空输出参数。 */
u64 get_cpu_iowait_time_us(int cpu, u64 *last_update_time)
{
	return get_cpu_sleep_time_us(cpu, CPUTIME_IOWAIT,
				     nr_iowait_cpu(cpu), last_update_time);
}
EXPORT_SYMBOL_GPL(get_cpu_iowait_time_us);

/*
 * Use precise platform statistics if available:
 */
#ifdef CONFIG_VIRT_CPU_ACCOUNTING_NATIVE

/* 原生 vtime 在 IRQ 边界按嵌套状态结算 hardirq、softirq、idle 或 kernel 时间。 */
void vtime_account_irq(struct task_struct *tsk, unsigned int offset)
{
	unsigned int pc = irq_count() - offset;

	if (pc & HARDIRQ_OFFSET) {
		vtime_account_hardirq(tsk);
	} else if (pc & SOFTIRQ_OFFSET) {
		vtime_account_softirq(tsk);
	} else if (!kcpustat_idle_dyntick()) {
		if (!IS_ENABLED(CONFIG_HAVE_VIRT_CPU_ACCOUNTING_IDLE) &&
		    is_idle_task(tsk)) {
			vtime_account_idle(tsk);
		} else {
			vtime_account_kernel(tsk);
		}
	} else {
		vtime_reset();
	}
}

/* 原生精确计费无需缩放；把 @curr 的 user/system 纳秒原样写到输出 @ut/@st。 */
void cputime_adjust(struct task_cputime *curr, struct prev_cputime *prev,
		    u64 *ut, u64 *st)
{
	*ut = curr->utime;
	*st = curr->stime;
}

/* 返回单 task 已精确累计的 user/system 纳秒；输出指针必须非空。 */
void task_cputime_adjusted(struct task_struct *p, u64 *ut, u64 *st)
{
	*ut = p->utime;
	*st = p->stime;
}
EXPORT_SYMBOL_GPL(task_cputime_adjusted);

/* 汇总 @p 整个线程组并输出精确 user/system 纳秒；不取得 task 引用。 */
void thread_group_cputime_adjusted(struct task_struct *p, u64 *ut, u64 *st)
{
	struct task_cputime cputime;

	thread_group_cputime(p, &cputime);

	*ut = cputime.utime;
	*st = cputime.stime;
}

#else /* !CONFIG_VIRT_CPU_ACCOUNTING_NATIVE: */

/*
 * Account a single tick of CPU time.
 * @p: the process that the CPU time gets accounted to
 * @user_tick: indicates if the tick is a user or a system tick
 */
/*
 * 普通 tick 计费入口；vtime/NO_HZ idle 已接管时立即返回，否则依次扣 steal、
 * 选择 IRQ 精确路径或把一个 TICK_NSEC 归入 user/system/idle。
 */
void account_process_tick(struct task_struct *p, int user_tick)
{
	u64 cputime, steal;

	if (vtime_accounting_enabled_this_cpu())
		return;

	if (kcpustat_idle_dyntick())
		return;

	if (irqtime_enabled()) {
		irqtime_account_process_tick(p, user_tick, 1);
		return;
	}

	cputime = TICK_NSEC;
	steal = steal_account_process_time(ULONG_MAX);

	if (steal >= cputime)
		return;

	cputime -= steal;

	if (user_tick)
		account_user_time(p, cputime);
	else if ((p != this_rq()->idle) || (irq_count() != HARDIRQ_OFFSET))
		account_system_time(p, HARDIRQ_OFFSET, cputime);
	else
		account_idle_time(cputime);
}

/*
 * Adjust tick based cputime random precision against scheduler runtime
 * accounting.
 *
 * Tick based cputime accounting depend on random scheduling timeslices of a
 * task to be interrupted or not by the timer.  Depending on these
 * circumstances, the number of these interrupts may be over or
 * under-optimistic, matching the real user and system cputime with a variable
 * precision.
 *
 * Fix this by scaling these tick based values against the total runtime
 * accounted by the CFS scheduler.
 *
 * This code provides the following guarantees:
 *
 *   stime + utime == rtime
 *   stime_i+1 >= stime_i, utime_i+1 >= utime_i
 *
 * Assuming that rtime_i+1 >= rtime_i.
 */
/*
 * 用调度器精确的 @curr->sum_exec_runtime 缩放 tick 抽样的 user/system 比例；
 * @prev 的锁串行并发读者，输出保证 ut+st=rtime 且两者各自不倒退。
 */
void cputime_adjust(struct task_cputime *curr, struct prev_cputime *prev,
		    u64 *ut, u64 *st)
{
	u64 rtime, stime, utime;
	unsigned long flags;

	/* Serialize concurrent callers such that we can honour our guarantees */
	raw_spin_lock_irqsave(&prev->lock, flags);
	rtime = curr->sum_exec_runtime;

	/*
	 * This is possible under two circumstances:
	 *  - rtime isn't monotonic after all (a bug);
	 *  - we got reordered by the lock.
	 *
	 * In both cases this acts as a filter such that the rest of the code
	 * can assume it is monotonic regardless of anything else.
	 */
	if (prev->stime + prev->utime >= rtime)
		goto out;

	stime = curr->stime;
	utime = curr->utime;

	/*
	 * If either stime or utime are 0, assume all runtime is userspace.
	 * Once a task gets some ticks, the monotonicity code at 'update:'
	 * will ensure things converge to the observed ratio.
	 */
	if (stime == 0) {
		utime = rtime;
		goto update;
	}

	if (utime == 0) {
		stime = rtime;
		goto update;
	}

	stime = mul_u64_u64_div_u64(stime, rtime, stime + utime);

update:
	/*
	 * Make sure stime doesn't go backwards; this preserves monotonicity
	 * for utime because rtime is monotonic.
	 *
	 *  utime_i+1 = rtime_i+1 - stime_i
	 *            = rtime_i+1 - (rtime_i - utime_i)
	 *            = (rtime_i+1 - rtime_i) + utime_i
	 *            >= utime_i
	 */
	if (stime < prev->stime)
		stime = prev->stime;
	utime = rtime - stime;

	/*
	 * Make sure utime doesn't go backwards; this still preserves
	 * monotonicity for stime, analogous argument to above.
	 */
	if (utime < prev->utime) {
		utime = prev->utime;
		stime = rtime - utime;
	}

	prev->stime = stime;
	prev->utime = utime;
out:
	*ut = prev->utime;
	*st = prev->stime;
	raw_spin_unlock_irqrestore(&prev->lock, flags);
}

/* 获取 @p 的原始快照，必要时刷新运行中 task，再经 prev_cputime 输出单调结果。 */
void task_cputime_adjusted(struct task_struct *p, u64 *ut, u64 *st)
{
	struct task_cputime cputime = {
		.sum_exec_runtime = p->se.sum_exec_runtime,
	};

	if (task_cputime(p, &cputime.utime, &cputime.stime))
		cputime.sum_exec_runtime = task_sched_runtime(p);
	cputime_adjust(&cputime, &p->prev_cputime, ut, st);
}
EXPORT_SYMBOL_GPL(task_cputime_adjusted);

/* 汇总线程组后用 signal 共享的 prev_cputime 输出线程组级单调 user/system 时间。 */
void thread_group_cputime_adjusted(struct task_struct *p, u64 *ut, u64 *st)
{
	struct task_cputime cputime;

	thread_group_cputime(p, &cputime);
	cputime_adjust(&cputime, &p->signal->prev_cputime, ut, st);
}
#endif /* !CONFIG_VIRT_CPU_ACCOUNTING_NATIVE */

#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
/* 返回从 @vtime->starttime 到当前 sched_clock 的非负纳秒差；时钟倒退时返回 0。 */
static u64 vtime_delta(struct vtime *vtime)
{
	unsigned long long clock;

	clock = sched_clock();
	if (clock < vtime->starttime)
		return 0;

	return clock - vtime->starttime;
}

/* 领取当前 vtime 区间并扣除 steal/IRQ；推进 starttime，返回可归 task 的纳秒。 */
static u64 get_vtime_delta(struct vtime *vtime)
{
	u64 delta = vtime_delta(vtime);
	u64 other;

	/*
	 * Unlike tick based timing, vtime based timing never has lost
	 * ticks, and no need for steal time accounting to make up for
	 * lost ticks. Vtime accounts a rounded version of actual
	 * elapsed time. Limit account_other_time to prevent rounding
	 * errors from causing elapsed vtime to go negative.
	 */
	other = account_other_time(delta);
	WARN_ON_ONCE(vtime->state == VTIME_INACTIVE);
	vtime->starttime += delta;

	return delta - other;
}

/* 累积 system 残量，达到一个 tick 后批量提交给 task/cpustat 并清空残量。 */
static void vtime_account_system(struct task_struct *tsk,
				 struct vtime *vtime)
{
	vtime->stime += get_vtime_delta(vtime);
	if (vtime->stime >= TICK_NSEC) {
		account_system_time(tsk, irq_count(), vtime->stime);
		vtime->stime = 0;
	}
}

/* 累积 guest 残量，达到一个 tick 后批量提交并清空；@tsk/@vtime 均为借用。 */
static void vtime_account_guest(struct task_struct *tsk,
				struct vtime *vtime)
{
	vtime->gtime += get_vtime_delta(vtime);
	if (vtime->gtime >= TICK_NSEC) {
		account_guest_time(tsk, vtime->gtime);
		vtime->gtime = 0;
	}
}

/* 根据旧状态结算 guest 或普通 kernel 区间，用于切换/内核边界的共同内部入口。 */
static void __vtime_account_kernel(struct task_struct *tsk,
				   struct vtime *vtime)
{
	/* We might have scheduled out from guest path */
	if (vtime->state == VTIME_GUEST)
		vtime_account_guest(tsk, vtime);
	else
		vtime_account_system(tsk, vtime);
}

/* 在 seqcount 写段内结算运行 task 的 kernel/guest 时间；零增量走快速返回。 */
void vtime_account_kernel(struct task_struct *tsk)
{
	struct vtime *vtime = &tsk->vtime;

	if (!vtime_delta(vtime))
		return;

	write_seqcount_begin(&vtime->seqcount);
	__vtime_account_kernel(tsk, vtime);
	write_seqcount_end(&vtime->seqcount);
}

/* 进入用户态前结算 system 区间并发布 VTIME_USER，新区间从更新后的 starttime 起算。 */
void vtime_user_enter(struct task_struct *tsk)
{
	struct vtime *vtime = &tsk->vtime;

	write_seqcount_begin(&vtime->seqcount);
	vtime_account_system(tsk, vtime);
	vtime->state = VTIME_USER;
	write_seqcount_end(&vtime->seqcount);
}

/* 离开用户态时结算 user 残量并切回 VTIME_SYS；seqcount 让远端读取整体重试。 */
void vtime_user_exit(struct task_struct *tsk)
{
	struct vtime *vtime = &tsk->vtime;

	write_seqcount_begin(&vtime->seqcount);
	vtime->utime += get_vtime_delta(vtime);
	if (vtime->utime >= TICK_NSEC) {
		account_user_time(tsk, vtime->utime);
		vtime->utime = 0;
	}
	vtime->state = VTIME_SYS;
	write_seqcount_end(&vtime->seqcount);
}

/*
 * 进入 guest 前在同一 seqcount 写段中结算 system、设置 PF_VCPU 与
 * VTIME_GUEST；该顺序使 task_gtime() 不会漏掉无 tick 的运行区间。
 */
void vtime_guest_enter(struct task_struct *tsk)
{
	struct vtime *vtime = &tsk->vtime;
	/*
	 * The flags must be updated under the lock with
	 * the vtime_starttime flush and update.
	 * That enforces a right ordering and update sequence
	 * synchronization against the reader (task_gtime())
	 * that can thus safely catch up with a tickless delta.
	 */
	write_seqcount_begin(&vtime->seqcount);
	vtime_account_system(tsk, vtime);
	tsk->flags |= PF_VCPU;
	vtime->state = VTIME_GUEST;
	write_seqcount_end(&vtime->seqcount);
}
EXPORT_SYMBOL_GPL(vtime_guest_enter);

/* 离开 guest 时先提交 guest 区间，再清 PF_VCPU 并发布 VTIME_SYS。 */
void vtime_guest_exit(struct task_struct *tsk)
{
	struct vtime *vtime = &tsk->vtime;

	write_seqcount_begin(&vtime->seqcount);
	vtime_account_guest(tsk, vtime);
	tsk->flags &= ~PF_VCPU;
	vtime->state = VTIME_SYS;
	write_seqcount_end(&vtime->seqcount);
}
EXPORT_SYMBOL_GPL(vtime_guest_exit);

/* 将当前 idle vtime 区间一次性归入 idle/iowait，调用者持有 vtime 写序列。 */
static void __vtime_account_idle(struct vtime *vtime)
{
	account_idle_time(get_vtime_delta(vtime));
}

/*
 * context switch 提交 @prev 的最后区间并标成 INACTIVE，再按 next 身份建立
 * 新状态/starttime/cpu；两次独立 seqcount 写段供远端 cpustat 读者检测竞态。
 */
void vtime_task_switch_generic(struct task_struct *prev)
{
	struct vtime *vtime = &prev->vtime;

	write_seqcount_begin(&vtime->seqcount);
	if (vtime->state == VTIME_IDLE)
		__vtime_account_idle(vtime);
	else
		__vtime_account_kernel(prev, vtime);
	vtime->state = VTIME_INACTIVE;
	vtime->cpu = -1;
	write_seqcount_end(&vtime->seqcount);

	vtime = &current->vtime;

	write_seqcount_begin(&vtime->seqcount);
	if (is_idle_task(current))
		vtime->state = VTIME_IDLE;
	else if (current->flags & PF_VCPU)
		vtime->state = VTIME_GUEST;
	else
		vtime->state = VTIME_SYS;
	vtime->starttime = sched_clock();
	vtime->cpu = smp_processor_id();
	write_seqcount_end(&vtime->seqcount);
}

/* 初始化 @cpu 的 idle task vtime 基线；本地关中断防止初始化被 IRQ 计费打断。 */
void vtime_init_idle(struct task_struct *t, int cpu)
{
	struct vtime *vtime = &t->vtime;
	unsigned long flags;

	local_irq_save(flags);
	write_seqcount_begin(&vtime->seqcount);
	vtime->state = VTIME_IDLE;
	vtime->starttime = sched_clock();
	vtime->cpu = cpu;
	write_seqcount_end(&vtime->seqcount);
	local_irq_restore(flags);
}

/* 返回 @t 已提交与当前未提交 guest 时间之和；seqcount 失败则重新获取完整快照。 */
u64 task_gtime(struct task_struct *t)
{
	struct vtime *vtime = &t->vtime;
	unsigned int seq;
	u64 gtime;

	if (!vtime_accounting_enabled())
		return t->gtime;

	do {
		seq = read_seqcount_begin(&vtime->seqcount);

		gtime = t->gtime;
		if (vtime->state == VTIME_GUEST)
			gtime += vtime->gtime + vtime_delta(vtime);

	} while (read_seqcount_retry(&vtime->seqcount, seq));

	return gtime;
}

/*
 * Fetch cputime raw values from fields of task_struct and
 * add up the pending nohz execution time since the last
 * cputime snapshot.
 */
/*
 * 输出 @t 的原始 user/system 纳秒；vtime 活跃时把开放区间加入对应输出并
 * 返回 true，睡眠/未启用时返回 false。输出仅是瞬时快照，不取得引用。
 */
bool task_cputime(struct task_struct *t, u64 *utime, u64 *stime)
{
	struct vtime *vtime = &t->vtime;
	unsigned int seq;
	u64 delta;
	int ret;

	if (!vtime_accounting_enabled()) {
		*utime = t->utime;
		*stime = t->stime;
		return false;
	}

	do {
		ret = false;
		seq = read_seqcount_begin(&vtime->seqcount);

		*utime = t->utime;
		*stime = t->stime;

		/* Task is sleeping or idle, nothing to add */
		if (vtime->state < VTIME_SYS)
			continue;

		ret = true;
		delta = vtime_delta(vtime);

		/*
		 * Task runs either in user (including guest) or kernel space,
		 * add pending nohz time to the right place.
		 */
		if (vtime->state == VTIME_SYS)
			*stime += vtime->stime + delta;
		else
			*utime += vtime->utime + delta;
	} while (read_seqcount_retry(&vtime->seqcount, seq));

	return ret;
}

/*
 * 验证 @vtime 是否仍属于 @cpu 且已越过切入的 INACTIVE 窗口；成功返回
 * VTIME_* 状态，竞态返回 -EAGAIN 让上层重新读取 rq->curr。
 */
static int vtime_state_fetch(struct vtime *vtime, int cpu)
{
	int state = READ_ONCE(vtime->state);

	/*
	 * We raced against a context switch, fetch the
	 * kcpustat task again.
	 */
	if (vtime->cpu != cpu && vtime->cpu != -1)
		return -EAGAIN;

	/*
	 * Two possible things here:
	 * 1) We are seeing the scheduling out task (prev) or any past one.
	 * 2) We are seeing the scheduling in task (next) but it hasn't
	 *    passed though vtime_task_switch() yet so the pending
	 *    cputime of the prev task may not be flushed yet.
	 *
	 * Case 1) is ok but 2) is not. So wait for a safe VTIME state.
	 */
	if (state == VTIME_INACTIVE)
		return -EAGAIN;

	return state;
}

/* 按 USER/GUEST 状态返回尚未提交的用户侧纳秒，其他状态返回 0。 */
static u64 kcpustat_user_vtime(struct vtime *vtime)
{
	if (vtime->state == VTIME_USER)
		return vtime->utime + vtime_delta(vtime);
	else if (vtime->state == VTIME_GUEST)
		return vtime->gtime + vtime_delta(vtime);
	return 0;
}

/*
 * 基于稳定的 @tsk vtime 快照补算单个 @usage 字段到输出 @val；若 task
 * 已迁移或尚未完成切换则返回 -EAGAIN，成功返回 0。
 */
static int kcpustat_field_vtime(u64 *cpustat,
				struct task_struct *tsk,
				enum cpu_usage_stat usage,
				int cpu, u64 *val)
{
	struct vtime *vtime = &tsk->vtime;
	struct rq *rq = cpu_rq(cpu);
	unsigned int seq;

	do {
		int state;

		seq = read_seqcount_begin(&vtime->seqcount);

		state = vtime_state_fetch(vtime, cpu);
		if (state < 0)
			return state;

		*val = cpustat[usage];

		/*
		 * Nice VS unnice cputime accounting may be inaccurate if
		 * the nice value has changed since the last vtime update.
		 * But proper fix would involve interrupting target on nice
		 * updates which is a no go on nohz_full (although the scheduler
		 * may still interrupt the target if rescheduling is needed...)
		 */
		switch (usage) {
		case CPUTIME_SYSTEM:
			if (state == VTIME_SYS)
				*val += vtime->stime + vtime_delta(vtime);
			break;
		case CPUTIME_USER:
			if (task_nice(tsk) <= 0)
				*val += kcpustat_user_vtime(vtime);
			break;
		case CPUTIME_NICE:
			if (task_nice(tsk) > 0)
				*val += kcpustat_user_vtime(vtime);
			break;
		case CPUTIME_GUEST:
			if (state == VTIME_GUEST && task_nice(tsk) <= 0)
				*val += vtime->gtime + vtime_delta(vtime);
			break;
		case CPUTIME_GUEST_NICE:
			if (state == VTIME_GUEST && task_nice(tsk) > 0)
				*val += vtime->gtime + vtime_delta(vtime);
			break;
		case CPUTIME_IDLE:
			if (state == VTIME_IDLE && !atomic_read(&rq->nr_iowait))
				*val += vtime_delta(vtime);
			break;
		case CPUTIME_IOWAIT:
			if (state == VTIME_IDLE && atomic_read(&rq->nr_iowait) > 0)
				*val += vtime_delta(vtime);
			break;
		default:
			break;
		}
	} while (read_seqcount_retry(&vtime->seqcount, seq));

	return 0;
}

/*
 * 返回 @cpu 的指定 cpustat 字段；generic vtime 下以 RCU 稳定 rq->curr，
 * 遇到切换窗口循环重试，把当前未落账区间加入已提交基数。
 */
u64 kcpustat_field(enum cpu_usage_stat usage, int cpu)
{
	u64 *cpustat = kcpustat_cpu(cpu).cpustat;
	u64 val = cpustat[usage];
	struct rq *rq;
	int err;

	if (!vtime_generic_enabled_cpu(cpu))
		return kcpustat_field_default(usage, cpu);

	rq = cpu_rq(cpu);

	for (;;) {
		struct task_struct *curr;

		rcu_read_lock();
		curr = rcu_dereference(rq->curr);
		if (WARN_ON_ONCE(!curr)) {
			rcu_read_unlock();
			return cpustat[usage];
		}

		err = kcpustat_field_vtime(cpustat, curr, usage, cpu, &val);
		rcu_read_unlock();

		if (!err)
			return val;

		cpu_relax();
	}
}
EXPORT_SYMBOL_GPL(kcpustat_field);

/* 将整个 @src 快照复制到 @dst 并补算 @tsk 当前区间；竞态返回 -EAGAIN。 */
static int kcpustat_cpu_fetch_vtime(struct kernel_cpustat *dst,
				    const struct kernel_cpustat *src,
				    struct task_struct *tsk, int cpu)
{
	struct vtime *vtime = &tsk->vtime;
	unsigned int seq;

	do {
		u64 *cpustat;
		u64 delta;
		int state;

		seq = read_seqcount_begin(&vtime->seqcount);

		state = vtime_state_fetch(vtime, cpu);
		if (state < 0)
			return state;

		*dst = *src;
		cpustat = dst->cpustat;

		/* Task is sleeping or dead, nothing to add */
		if (state < VTIME_IDLE)
			continue;

		delta = vtime_delta(vtime);

		/*
		 * Task runs either in user (including guest) or kernel space,
		 * add pending nohz time to the right place.
		 */
		switch (state) {
		case VTIME_SYS:
			cpustat[CPUTIME_SYSTEM] += vtime->stime + delta;
			break;
		case VTIME_USER:
			if (task_nice(tsk) > 0)
				cpustat[CPUTIME_NICE] += vtime->utime + delta;
			else
				cpustat[CPUTIME_USER] += vtime->utime + delta;
			break;
		case VTIME_GUEST:
			if (task_nice(tsk) > 0) {
				cpustat[CPUTIME_GUEST_NICE] += vtime->gtime + delta;
				cpustat[CPUTIME_NICE] += vtime->gtime + delta;
			} else {
				cpustat[CPUTIME_GUEST] += vtime->gtime + delta;
				cpustat[CPUTIME_USER] += vtime->gtime + delta;
			}
			break;
		case VTIME_IDLE:
			if (atomic_read(&cpu_rq(cpu)->nr_iowait) > 0)
				cpustat[CPUTIME_IOWAIT] += delta;
			else
				cpustat[CPUTIME_IDLE] += delta;
			break;
		default:
			WARN_ON_ONCE(1);
		}
	} while (read_seqcount_retry(&vtime->seqcount, seq));

	return 0;
}

/*
 * 获取 @cpu 的完整 cpustat 快照到调用者拥有的 @dst；generic vtime 下
 * RCU 稳定当前 task 并在迁移窗口重试，异常时退回已提交数据。
 */
void kcpustat_cpu_fetch(struct kernel_cpustat *dst, int cpu)
{
	const struct kernel_cpustat *src = &kcpustat_cpu(cpu);
	struct rq *rq;
	int err;

	if (!vtime_generic_enabled_cpu(cpu)) {
		kcpustat_cpu_fetch_default(dst, cpu);
		return;
	}

	rq = cpu_rq(cpu);

	for (;;) {
		struct task_struct *curr;

		rcu_read_lock();
		curr = rcu_dereference(rq->curr);
		if (WARN_ON_ONCE(!curr)) {
			rcu_read_unlock();
			kcpustat_cpu_fetch_default(dst, cpu);
			return;
		}

		err = kcpustat_cpu_fetch_vtime(dst, src, curr, cpu);
		rcu_read_unlock();

		if (!err)
			return;

		cpu_relax();
	}
}
EXPORT_SYMBOL_GPL(kcpustat_cpu_fetch);

#endif /* CONFIG_VIRT_CPU_ACCOUNTING_GEN */
