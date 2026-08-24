/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KERNEL_STATS_H
#define _KERNEL_STATS_H

/* 本头文件把可选 schedstats、PSI 与 sched_info 热路径适配成配置无关调用面。 */
#ifdef CONFIG_SCHEDSTATS

/* 运行时静态键；默认 false，启用后热路径宏才真正更新诊断计数。 */
extern struct static_key_false sched_schedstats;

/*
 * Expects runqueue lock to be held for atomicity of update
 */
/* 调用者持有 rq 锁；把一次调度到达的等待时长加入 rq，并增加时间片计数。rq 可空。 */
static inline void
rq_sched_info_arrive(struct rq *rq, unsigned long long delta)
{
	if (rq) {
		rq->rq_sched_info.run_delay += delta;
		rq->rq_sched_info.pcount++;
	}
}

/*
 * Expects runqueue lock to be held for atomicity of update
 */
/* 调用者持有 rq 锁；把离开 CPU 前的执行时长累计到 rq_cpu_time，rq 可空。 */
static inline void
rq_sched_info_depart(struct rq *rq, unsigned long long delta)
{
	if (rq)
		rq->rq_cpu_time += delta;
}

/* 调用者持有 rq 锁；迁移/离队时只累计已等待片段，不增加时间片计数。 */
static inline void
rq_sched_info_dequeue(struct rq *rq, unsigned long long delta)
{
	if (rq)
		rq->rq_sched_info.run_delay += delta;
}
/* 静态键查询；以下无前缀宏自带门控，双下划线版本要求调用者已经门控。 */
#define   schedstat_enabled()		static_branch_unlikely(&sched_schedstats)
/* 无条件递增 @var；调用者保证 CONFIG/静态键条件和外层锁。 */
#define __schedstat_inc(var)		do { var++; } while (0)
/* 仅运行时启用 schedstats 时递增 @var。 */
#define   schedstat_inc(var)		do { if (schedstat_enabled()) { var++; } } while (0)
/* 无条件给 @var 加 @amt；参数只求值一次。 */
#define __schedstat_add(var, amt)	do { var += (amt); } while (0)
/* 静态键开启时给 @var 加 @amt。 */
#define   schedstat_add(var, amt)	do { if (schedstat_enabled()) { var += (amt); } } while (0)
/* 无条件把 @var 设为 @val。 */
#define __schedstat_set(var, val)	do { var = (val); } while (0)
/* 静态键开启时把 @var 设为 @val。 */
#define   schedstat_set(var, val)	do { if (schedstat_enabled()) { var = (val); } } while (0)
/* CONFIG_SCHEDSTATS 下直接读取字段；同步仍由调用者负责。 */
#define   schedstat_val(var)		(var)
/* 运行时关闭时返回 0，避免向用户态暴露未维护的旧值。 */
#define   schedstat_val_or_zero(var)	((schedstat_enabled()) ? (var) : 0)

/* 以下三个实现位于 stats.c；调用者持 rq 锁且已检查静态键，p 可空表示组实体。 */
void __update_stats_wait_start(struct rq *rq, struct task_struct *p,
			       struct sched_statistics *stats);

void __update_stats_wait_end(struct rq *rq, struct task_struct *p,
			     struct sched_statistics *stats);
void __update_stats_enqueue_sleeper(struct rq *rq, struct task_struct *p,
				    struct sched_statistics *stats);

/*
 * 若依赖 sched_stat_* tracepoint 已启用但 schedstats 静态键仍关闭，延迟且仅一次打印
 * 配置告警。无参数/返回值，不主动开启统计；热路径安全，printk_deferred 不直接睡眠。
 */
static inline void
check_schedstat_required(void)
{
	if (schedstat_enabled())
		return;

	/* Force schedstat enabled if a dependent tracepoint is active */
	/* 依赖 tracepoint 需要对应统计更新；这里只提示 kernel 参数/sysctl 开启方法。 */
	if (trace_sched_stat_wait_enabled()    ||
	    trace_sched_stat_sleep_enabled()   ||
	    trace_sched_stat_iowait_enabled()  ||
	    trace_sched_stat_blocked_enabled() ||
	    trace_sched_stat_runtime_enabled())
		printk_deferred_once("Scheduler tracepoints stat_sleep, stat_iowait, stat_blocked and stat_runtime require the kernel parameter schedstats=enable or kernel.sched_schedstats=1\n");
}

#else /* !CONFIG_SCHEDSTATS: */

/* 未编译 schedstats 时 rq/task 更新全部退化为空操作，且不会求值统计字段。 */
static inline void rq_sched_info_arrive  (struct rq *rq, unsigned long long delta) { }
static inline void rq_sched_info_dequeue(struct rq *rq, unsigned long long delta) { }
static inline void rq_sched_info_depart  (struct rq *rq, unsigned long long delta) { }
# define   schedstat_enabled()		0
# define __schedstat_inc(var)		do { } while (0)
# define   schedstat_inc(var)		do { } while (0)
# define __schedstat_add(var, amt)	do { } while (0)
# define   schedstat_add(var, amt)	do { } while (0)
# define __schedstat_set(var, val)	do { } while (0)
# define   schedstat_set(var, val)	do { } while (0)
# define   schedstat_val(var)		0
# define   schedstat_val_or_zero(var)	0

# define __update_stats_wait_start(rq, p, stats)       do { } while (0)
# define __update_stats_wait_end(rq, p, stats)         do { } while (0)
# define __update_stats_enqueue_sleeper(rq, p, stats)  do { } while (0)
# define check_schedstat_required()                    do { } while (0)

#endif /* CONFIG_SCHEDSTATS */

/*
 * 返回 @se 对应的 sched_statistics：FAIR 组实体取 cfs_tg_state.stats，task 实体取
 * task_struct.stats。返回借用指针，不加锁；调用者以 rq 锁稳定实体及字段更新。
 */
static inline struct sched_statistics *
__schedstats_from_se(struct sched_entity *se)
{
#ifdef CONFIG_FAIR_GROUP_SCHED
	if (!entity_is_task(se))
		return &container_of(se, struct cfs_tg_state, se)->stats;
#endif
	return &task_of(se)->stats;
}

#ifdef CONFIG_PSI
/* PSI 核心实现位于 psi.c；这些声明消费 task 状态位并更新 rq/cgroup 聚合。 */
void psi_task_change(struct task_struct *task, int clear, int set);
void psi_task_switch(struct task_struct *prev, struct task_struct *next,
		     bool sleep);
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
/* IRQ 时间切换时把 curr/prev 状态计入 @rq；参数借用，调用者处于调度原子上下文。 */
void psi_account_irqtime(struct rq *rq, struct task_struct *curr, struct task_struct *prev);
#else /* !CONFIG_IRQ_TIME_ACCOUNTING: */
/* 无 IRQ time accounting 时保持调用点为空操作。 */
static inline void psi_account_irqtime(struct rq *rq, struct task_struct *curr,
				       struct task_struct *prev) {}
#endif /* !CONFIG_IRQ_TIME_ACCOUNTING */
/*
 * PSI tracks state that persists across sleeps, such as iowaits and
 * memory stalls. As a result, it has to distinguish between sleeps,
 * where a task's runnable state changes, and migrations, where a task
 * and its runnable state are being moved between CPUs and runqueues.
 *
 * A notable case is a task whose dequeue is delayed. PSI considers
 * those sleeping, but because they are still on the runqueue they can
 * go through migration requeues. In this case, *sleeping* states need
 * to be transferred.
 */
/*
 * PSI 状态会跨睡眠持续（如 iowait、memstall），所以必须区分真正睡眠与仅在 CPU/rq
 * 之间迁移。延迟 dequeue 的任务虽仍在 rq 上，PSI 已把它视为睡眠；迁移重入队时
 * 需要把这些 sleep-persistent 状态一并转移。
 */
/*
 * 入队时根据 @flags 把 @p 的 PSI 状态登记到新 rq。RESTORE 和仍 on_cpu 快速返回；
 * delayed migration 恢复 memstall/iowait，普通迁移恢复 running，唤醒则清 iowait 并
 * 设置 running。psi_disabled 静态键关闭时零成本返回。函数持调用者 rq 锁、不可睡眠。
 */
static inline void psi_enqueue(struct task_struct *p, int flags)
{
	/* clear/set 是传给 psi_task_change 的位集合，初始无变化。 */
	int clear = 0, set = 0;

	if (static_branch_likely(&psi_disabled))
		return;

	/* Same runqueue, nothing changed for psi */
	/* 同一 rq 的保存/恢复不改变 PSI 归属。 */
	if (flags & ENQUEUE_RESTORE)
		return;

	/* psi_sched_switch() will handle the flags */
	/* 当前仍在 CPU 上时由随后的 switch 原子处理 ONCPU/RUNNING 状态。 */
	if (task_on_cpu(task_rq(p), p))
		return;

	if (p->se.sched_delayed) {
		/* CPU migration of "sleeping" task */
		/* 延迟出队的“睡眠”任务迁移，复制跨睡眠保持的 stall 状态。 */
		WARN_ON_ONCE(!(flags & ENQUEUE_MIGRATED));
		if (p->in_memstall)
			set |= TSK_MEMSTALL;
		if (p->in_iowait)
			set |= TSK_IOWAIT;
	} else if (flags & ENQUEUE_MIGRATED) {
		/* CPU migration of runnable task */
		/* 普通可运行任务迁移，在目标 rq 重新登记 RUNNING。 */
		set = TSK_RUNNING;
		if (p->in_memstall)
			set |= TSK_MEMSTALL | TSK_MEMSTALL_RUNNING;
	} else {
		/* Wakeup of new or sleeping task */
		/* 新任务/睡眠唤醒：结束 I/O wait，并开始 RUNNING/memstall-running。 */
		if (p->in_iowait)
			clear |= TSK_IOWAIT;
		set = TSK_RUNNING;
		if (p->in_memstall)
			set |= TSK_MEMSTALL_RUNNING;
	}

	psi_task_change(p, clear, set);
}

/*
 * 出队时从旧 rq 注销 @p PSI 状态。DEQUEUE_SAVE 不改变 rq；即将发生的自愿睡眠 switch
 * 由 psi_task_switch 一次处理，避免两次遍历祖先。其他迁移/代理执行路径清掉当前所有
 * psi_flags，目标 enqueue 重建。静态键关闭快速返回；调用者持 rq 锁且函数不睡眠。
 */
static inline void psi_dequeue(struct task_struct *p, int flags)
{
	if (static_branch_likely(&psi_disabled))
		return;

	/* Same runqueue, nothing changed for psi */
	/* DEQUEUE_SAVE 之后会在同 rq 恢复，不转移 PSI。 */
	if (flags & DEQUEUE_SAVE)
		return;

	/*
	 * A voluntary sleep is a dequeue followed by a task switch. To
	 * avoid walking all ancestors twice, psi_task_switch() handles
	 * TSK_RUNNING and TSK_IOWAIT for us when it moves TSK_ONCPU.
	 * Do nothing here.
	 *
	 * In the SCHED_PROXY_EXECUTION case we may do sleeping
	 * dequeues that are not followed by a task switch, so check
	 * TSK_ONCPU is set to ensure the task switch is imminent.
	 * Otherwise clear the flags as usual.
	 */
	/*
	 * 自愿睡眠通常紧随 task switch，由 switch 同时搬走 ONCPU/RUNNING/IOWAIT，避免
	 * 重复遍历 cgroup 祖先；proxy execution 可能睡眠出队却不 switch，故仅 ONCPU 时跳过。
	 */
	if ((flags & DEQUEUE_SLEEP) && (p->psi_flags & TSK_ONCPU))
		return;

	/*
	 * When migrating a task to another CPU, clear all psi
	 * state. The enqueue callback above will work it out.
	 */
	/* 迁移先清空旧 rq 状态，目标 psi_enqueue 再按任务字段推导新状态。 */
	psi_task_change(p, p->psi_flags, 0);
}

/*
 * try_to_wake_up 迁移前，若 @p 仍有跨睡眠 PSI 状态，则锁定旧 task rq、注销全部状态，
 * 再让后续 enqueue 重建。无状态快速返回；__task_rq_lock 稳定 CPU/rq，函数不可睡眠。
 */
static inline void psi_ttwu_dequeue(struct task_struct *p)
{
	if (static_branch_likely(&psi_disabled))
		return;
	/*
	 * Is the task being migrated during a wakeup? Make sure to
	 * deregister its sleep-persistent psi states from the old
	 * queue, and let psi_enqueue() know it has to requeue.
	 */
	/* 唤醒迁移需从旧 rq 注销 sleep-persistent 状态，目标 enqueue 才能正确重新登记。 */
	if (unlikely(p->psi_flags)) {
		struct rq_flags rf;
		struct rq *rq;

		rq = __task_rq_lock(p, &rf);
		psi_task_change(p, p->psi_flags, 0);
		__task_rq_unlock(rq, p, &rf);
	}
}

/* 调度切换薄包装；PSI 关闭直接返回，否则把 prev/next/sleep 交给 psi.c，调用者持 rq 锁。 */
static inline void psi_sched_switch(struct task_struct *prev,
				    struct task_struct *next,
				    bool sleep)
{
	if (static_branch_likely(&psi_disabled))
		return;

	psi_task_switch(prev, next, sleep);
}

#else /* !CONFIG_PSI: */
/* 未编译 PSI 时所有 hook 都为空操作，保持调度核心无需条件编译。 */
static inline void psi_enqueue(struct task_struct *p, bool migrate) {}
static inline void psi_dequeue(struct task_struct *p, bool migrate) {}
static inline void psi_ttwu_dequeue(struct task_struct *p) {}
static inline void psi_sched_switch(struct task_struct *prev,
				    struct task_struct *next,
				    bool sleep) {}
static inline void psi_account_irqtime(struct rq *rq, struct task_struct *curr,
				       struct task_struct *prev) {}
#endif /* !CONFIG_PSI */

#ifdef CONFIG_SCHED_INFO
/*
 * We are interested in knowing how long it was from the *first* time a
 * task was queued to the time that it finally hit a CPU, we call this routine
 * from dequeue_task() to account for possible rq->clock skew across CPUs. The
 * delta taken on each CPU would annul the skew.
 */
/*
 * 统计从第一次入队到真正运行的总等待；迁移离队时先用旧 rq_clock 结算片段，可消除
 * 不同 CPU rq 时钟偏差。@rq/@t 由 rq 锁稳定；last_queued 为 0 快速返回。更新 task
 * 总量/最大最小值及 rq run_delay，无返回值、不可睡眠。
 */
static inline void sched_info_dequeue(struct rq *rq, struct task_struct *t)
{
	/* delta 是当前 CPU 上本段等待纳秒数。 */
	unsigned long long delta = 0;

	if (!t->sched_info.last_queued)
		return;

	delta = rq_clock(rq) - t->sched_info.last_queued;
	t->sched_info.last_queued = 0;
	t->sched_info.run_delay += delta;
	if (delta > t->sched_info.max_run_delay) {
		/* 新最大值同时记录真实墙钟时间，供诊断定位发生时刻。 */
		t->sched_info.max_run_delay = delta;
		ktime_get_real_ts64(&t->sched_info.max_run_delay_ts);
	}
	if (delta && (!t->sched_info.min_run_delay || delta < t->sched_info.min_run_delay))
		t->sched_info.min_run_delay = delta;
	rq_sched_info_dequeue(rq, delta);
}

/*
 * Called when a task finally hits the CPU.  We can now calculate how
 * long it was waiting to run.  We also note when it began so that we
 * can keep stats on how long its time-slice is.
 */
/*
 * task 真正获得 CPU 时结算最后一段等待并记录执行起点。入口持 rq 锁；@t 借用，
 * last_queued=0 表示没有可结算等待。更新 task run_delay/max/min、arrival、pcount 以及
 * rq 统计，无返回值；ktime 读取不睡眠。
 */
static void sched_info_arrive(struct rq *rq, struct task_struct *t)
{
	/* now/delta 均使用 rq_clock 纳秒域，避免跨 CPU 直接相减。 */
	unsigned long long now, delta = 0;

	if (!t->sched_info.last_queued)
		return;

	now = rq_clock(rq);
	delta = now - t->sched_info.last_queued;
	t->sched_info.last_queued = 0;
	t->sched_info.run_delay += delta;
	t->sched_info.last_arrival = now;
	t->sched_info.pcount++;
	if (delta > t->sched_info.max_run_delay) {
		t->sched_info.max_run_delay = delta;
		ktime_get_real_ts64(&t->sched_info.max_run_delay_ts);
	}
	if (delta && (!t->sched_info.min_run_delay || delta < t->sched_info.min_run_delay))
		t->sched_info.min_run_delay = delta;

	rq_sched_info_arrive(rq, delta);
}

/*
 * This function is only called from enqueue_task(), but also only updates
 * the timestamp if it is already not set.  It's assumed that
 * sched_info_dequeue() will clear that stamp when appropriate.
 */
/*
 * enqueue_task 路径仅在 last_queued 未设置时记录第一次等待起点，重复 requeue 不重置
 * 时间，从而统计完整等待。@rq/@t 由 rq 锁稳定，无返回值、不可睡眠。
 */
static inline void sched_info_enqueue(struct rq *rq, struct task_struct *t)
{
	if (!t->sched_info.last_queued)
		t->sched_info.last_queued = rq_clock(rq);
}

/*
 * Called when a process ceases being the active-running process involuntarily
 * due, typically, to expiring its time slice (this may also be called when
 * switching to the idle task).  Now we can calculate how long we ran.
 * Also, if the process is still in the TASK_RUNNING state, call
 * sched_info_enqueue() to mark that it has now again started waiting on
 * the runqueue.
 */
/*
 * @t 非自愿离开 CPU 时，以 last_arrival 结算本时间片到 rq_cpu_time；若仍 TASK_RUNNING，
 * 立即调用 enqueue 标记重新等待。调用者持 rq 锁，参数借用，无返回值、不可睡眠。
 */
static inline void sched_info_depart(struct rq *rq, struct task_struct *t)
{
	unsigned long long delta = rq_clock(rq) - t->sched_info.last_arrival;

	rq_sched_info_depart(rq, delta);

	if (task_is_running(t))
		sched_info_enqueue(rq, t);
}

/*
 * Called when tasks are switched involuntarily due, typically, to expiring
 * their time slice.  (This may also be called when switching to or from
 * the idle task.)  We are only called when prev != next.
 */
/*
 * prev!=next 的 context switch 统计总入口：非 idle prev 结算执行，非 idle next 结算等待
 * 并开始时间片。idle 不参与效率统计。调用者持 rq 锁；无返回值、无 ownership 变化。
 */
static inline void
sched_info_switch(struct rq *rq, struct task_struct *prev, struct task_struct *next)
{
	/*
	 * prev now departs the CPU.  It's not interesting to record
	 * stats about how efficient we were at scheduling the idle
	 * process, however.
	 */
	/* idle 的运行/等待没有用户调度效率含义，因此两侧分别跳过。 */
	if (prev != rq->idle)
		sched_info_depart(rq, prev);

	if (next != rq->idle)
		sched_info_arrive(rq, next);
}

#else /* !CONFIG_SCHED_INFO: */
/* 未编译 SCHED_INFO 时三类 hook 为空操作，参数不会产生统计副作用。 */
# define sched_info_enqueue(rq, t)	do { } while (0)
# define sched_info_dequeue(rq, t)	do { } while (0)
# define sched_info_switch(rq, t, next)	do { } while (0)
#endif /* !CONFIG_SCHED_INFO */

#endif /* _KERNEL_STATS_H */
