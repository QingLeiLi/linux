// SPDX-License-Identifier: GPL-2.0
/*
 * Implement CPU time clocks for the POSIX clock interface.
 */
/*
 * 学习总览：CPU timer 的时间轴不是墙钟，而是线程/线程组实际消耗的 PROF(utime+stime)、VIRT(utime) 与
 * SCHED(runtime) 纳秒。每个 scope 有三棵 timerqueue 和 nextevt 快速缓存；scheduler tick 只在样本越过缓存
 * 时进入慢路径。慢路径在 sighand siglock 下把到期节点搬到私有 firing 链，解锁后逐 timer 取 it_lock 发信号，
 * 从而维持 it_lock -> siglock 的锁序。可选 TASK_WORK 把实际到期延后到任务上下文，并以 handling+mutex 实现
 * cancel 等待。进程总 CPU 计数仅在有 timer/rlimit 时启用，tick dependency 与缓存共同控制成本。
 */

#include <linux/sched/signal.h>
#include <linux/sched/cputime.h>
#include <linux/posix-timers.h>
#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/uaccess.h>
#include <linux/kernel_stat.h>
#include <trace/events/timer.h>
#include <linux/tick.h>
#include <linux/workqueue.h>
#include <linux/compat.h>
#include <linux/sched/deadline.h>
#include <linux/task_work.h>

#include "posix-timers.h"

static bool posix_cpu_timer_rearm(struct k_itimer *timer);

/*
 * posix_cputimers_group_init() - 初始化线程组 CPU timer bases，并可预装 RLIMIT_CPU soft limit。
 * @pct 被完整清零且三个 nextevt 置 U64_MAX；@cpu_limit 非 RLIM_INFINITY 时换算为 PROF 纳秒期限并置
 * timers_active，使组 CPU 原子累计立即启用。调用者在 signal_struct 初始化阶段独占对象，无返回值。
 */
void posix_cputimers_group_init(struct posix_cputimers *pct, u64 cpu_limit)
{
	posix_cputimers_init(pct);
	if (cpu_limit != RLIM_INFINITY) {
		pct->bases[CPUCLOCK_PROF].nextevt = cpu_limit * NSEC_PER_SEC;
		pct->timers_active = true;
	}
}

/*
 * Called after updating RLIMIT_CPU to run cpu timer and update
 * tsk->signal->posix_cputimers.bases[clock].nextevt expiration cache if
 * necessary. Needs siglock protection since other code may update the
 * expiration cache as well.
 *
 * Returns 0 on success, -ESRCH on failure.  Can fail if the task is exiting and
 * we cannot lock_task_sighand.  Cannot fail if task is current.
 */
/*
 * update_rlimit_cpu() - RLIMIT_CPU 更新后同步 PROF expiry cache 与组 CPU accounting。
 * @rlim_new 秒换算 u64 纳秒；取得目标 @task sighand siglock，失败（退出中）返回 -ESRCH，current 不会失败。
 * 锁内调用 set_process_cpu_timer，不请求旧值，随后解锁返回 0。RLIM_INFINITY 的乘法值仅由下层比较语义处理。
 */
int update_rlimit_cpu(struct task_struct *task, unsigned long rlim_new)
{
	u64 nsecs = (u64)rlim_new * NSEC_PER_SEC;
	unsigned long irq_fl;

	if (!lock_task_sighand(task, &irq_fl))
		return -ESRCH;
	set_process_cpu_timer(task, CPUCLOCK_PROF, &nsecs, NULL);
	unlock_task_sighand(task, &irq_fl);
	return 0;
}

/*
 * Functions for validating access to tasks.
 */
/*
 * pid_for_clock() - 在 RCU 下校验 CPU clock 编码并解析目标 struct pid。
 * 拒绝 which>=CPUCLOCK_MAX。编码 pid=0 映射 current TID/TGID；非零 thread clock 要求目标存在且同线程组；
 * process clock 要求 pid 标识 TGID，但 @gettime=true 时允许 current 自身 TID 并规范化为 current TGID。
 * 成功返回 RCU 借用 pid，失败 NULL；这里校验结构/作用域，不取得 pid 引用或执行凭据权限检查。
 */
static struct pid *pid_for_clock(const clockid_t clock, bool gettime)
{
	const bool thread = !!CPUCLOCK_PERTHREAD(clock);
	const pid_t upid = CPUCLOCK_PID(clock);
	struct pid *pid;

	if (CPUCLOCK_WHICH(clock) >= CPUCLOCK_MAX)
		return NULL;

	/*
	 * If the encoded PID is 0, then the timer is targeted at current
	 * or the process to which current belongs.
	 */
	/* 零 pid 是 POSIX “调用者自身线程/进程”的快捷编码。 */
	if (upid == 0)
		return thread ? task_pid(current) : task_tgid(current);

	pid = find_vpid(upid);
	if (!pid)
		return NULL;

	if (thread) {
		struct task_struct *tsk = pid_task(pid, PIDTYPE_PID);
		return (tsk && same_thread_group(tsk, current)) ? pid : NULL;
	}

	/*
	 * For clock_gettime(PROCESS) allow finding the process by
	 * with the pid of the current task.  The code needs the tgid
	 * of the process so that pid_task(pid, PIDTYPE_TGID) can be
	 * used to find the process.
	 */
	/* gettime 特例接受 current 的线程 pid，但后续 process sample 必须以 TGID leader 查找。 */
	if (gettime && (pid == task_pid(current)))
		return task_tgid(current);

	/*
	 * For processes require that pid identifies a process.
	 */
	/* 非线程 clock 的非零 pid 必须确有 PIDTYPE_TGID 任务，不能指向任意线程 ID。 */
	return pid_has_task(pid, PIDTYPE_TGID) ? pid : NULL;
}

/* validate_clock_permissions() 在 RCU 下调用 pid_for_clock(gettime=false)，结构/目标无效 -EINVAL，否则 0。 */
static inline int validate_clock_permissions(const clockid_t clock)
{
	int ret;

	rcu_read_lock();
	ret = pid_for_clock(clock, false) ? 0 : -EINVAL;
	rcu_read_unlock();

	return ret;
}

/* clock_pid_type() 按 PERTHREAD 位选择 pid_task 的 PIDTYPE_PID 或 PIDTYPE_TGID。 */
static inline enum pid_type clock_pid_type(const clockid_t clock)
{
	return CPUCLOCK_PERTHREAD(clock) ? PIDTYPE_PID : PIDTYPE_TGID;
}

/* cpu_timer_task_rcu() 以 timer 持有的 pid 与 it_clock scope 查目标 task；调用者必须持 RCU，返回可空借用指针。 */
static inline struct task_struct *cpu_timer_task_rcu(struct k_itimer *timer)
{
	return pid_task(timer->it.cpu.pid, clock_pid_type(timer->it_clock));
}

/*
 * Update expiry time from increment, and increase overrun count,
 * given the current clock sample.
 */
/*
 * bump_cpu_timer() - 把周期 CPU timer 的绝对期限推进到严格晚于 @now，并累计跨过次数。
 * oneshot 或 now<expires 不改动。interval>0且已到期时，以倍增寻找最高 2 次幂批量，再从高到低分解
 * delta=now+interval-expires，避免直接 interval*count 溢出；每批更新 node.expires 与 it_overrun。
 * 返回新/原绝对期限。调用者持 it_lock，interval 与 expires 已规范；函数不入队。
 */
static u64 bump_cpu_timer(struct k_itimer *timer, u64 now)
{
	u64 delta, incr, expires = timer->it.cpu.node.expires;
	int i;

	if (!timer->it_interval)
		return expires;

	if (now < expires)
		return expires;

	incr = timer->it_interval;
	delta = now + incr - expires;

	/* Don't use (incr*2 < delta), incr*2 might overflow. */
	/* 用 incr < delta-incr 判断还能安全翻倍，避免先算 incr*2 的无符号回绕。 */
	for (i = 0; incr < delta - incr; i++)
		incr = incr << 1;

	for (; i >= 0; incr >>= 1, i--) {
		if (delta < incr)
			continue;

		timer->it.cpu.node.expires += incr;
		timer->it_overrun += 1LL << i;
		delta -= incr;
	}
	return timer->it.cpu.node.expires;
}

/* Check whether all cache entries contain U64_MAX, i.e. eternal expiry time */
/* expiry_cache_is_inactive() 仅当 PROF/VIRT/SCHED 三个 nextevt 全为 U64_MAX 时返回 true。 */
static inline bool expiry_cache_is_inactive(const struct posix_cputimers *pct)
{
	return !(~pct->bases[CPUCLOCK_PROF].nextevt |
		 ~pct->bases[CPUCLOCK_VIRT].nextevt |
		 ~pct->bases[CPUCLOCK_SCHED].nextevt);
}

/*
 * posix_cpu_clock_getres() - 校验 CPU clock 目标并报告 API 分辨率。
 * PROF/VIRT 返回 ceil(NSEC_PER_SEC/HZ)，SCHED 因 scheduler runtime 通常更精细而报告 1ns；失败 -EINVAL，
 * 成功写 @tp 并返回 0。该值不是硬件精度保证。
 */
static int
posix_cpu_clock_getres(const clockid_t which_clock, struct timespec64 *tp)
{
	int error = validate_clock_permissions(which_clock);

	if (!error) {
		tp->tv_sec = 0;
		tp->tv_nsec = ((NSEC_PER_SEC + HZ - 1) / HZ);
		if (CPUCLOCK_WHICH(which_clock) == CPUCLOCK_SCHED) {
			/*
			 * If sched_clock is using a cycle counter, we
			 * don't have any idea of its true resolution
			 * exported, but it is much more than 1s/HZ.
			 */
			/* sched_clock 未导出真实粒度，接口按纳秒 runtime 表示报告 1ns。 */
			tp->tv_nsec = 1;
		}
	}
	return error;
}

/*
 * posix_cpu_clock_set() - CPU clock_settime 固定不可修改。
 * 先校验 clock/目标，使无效输入优先返回 -EINVAL；合法时忽略 @tp 并返回 -EPERM。
 */
static int
posix_cpu_clock_set(const clockid_t clock, const struct timespec64 *tp)
{
	int error = validate_clock_permissions(clock);

	/*
	 * You can never reset a CPU clock, but we check for other errors
	 * in the call before failing with EPERM.
	 */
	/* 即使操作必然不允许，也先保留 POSIX 对无效 clock 的诊断优先级。 */
	return error ? : -EPERM;
}

/*
 * Sample a per-thread clock for the given task. clkid is validated.
 */
/*
 * cpu_clock_sample() - 采样单线程已校验的 CPU clock index。
 * SCHED 直接取 task_sched_runtime；PROF/VIRT 通过 task_cputime 取 utime/stime，分别返回和或 utime。
 * 非法 index WARN_ON_ONCE 并返回 0。@p 在 RCU/任务生命周期协议下稳定，结果单位纳秒。
 */
static u64 cpu_clock_sample(const clockid_t clkid, struct task_struct *p)
{
	u64 utime, stime;

	if (clkid == CPUCLOCK_SCHED)
		return task_sched_runtime(p);

	task_cputime(p, &utime, &stime);

	switch (clkid) {
	case CPUCLOCK_PROF:
		return utime + stime;
	case CPUCLOCK_VIRT:
		return utime;
	default:
		WARN_ON_ONCE(1);
	}
	return 0;
}

/* store_samples() 按固定索引把 stime/utime/runtime 组装为 PROF、VIRT、SCHED 三个纳秒样本。 */
static inline void store_samples(u64 *samples, u64 stime, u64 utime, u64 rtime)
{
	samples[CPUCLOCK_PROF] = stime + utime;
	samples[CPUCLOCK_VIRT] = utime;
	samples[CPUCLOCK_SCHED] = rtime;
}

/* task_sample_cputime() 采样单线程 utime/stime 与 se.sum_exec_runtime，并填三元素 @samples。 */
static void task_sample_cputime(struct task_struct *p, u64 *samples)
{
	u64 stime, utime;

	task_cputime(p, &utime, &stime);
	store_samples(samples, stime, utime, p->se.sum_exec_runtime);
}

/* proc_sample_cputime_atomic() 无锁读取已启用的线程组原子累计三字段，允许跨字段轻微不同代。 */
static void proc_sample_cputime_atomic(struct task_cputime_atomic *at,
				       u64 *samples)
{
	u64 stime, utime, rtime;

	utime = atomic64_read(&at->utime);
	stime = atomic64_read(&at->stime);
	rtime = atomic64_read(&at->sum_exec_runtime);
	store_samples(samples, stime, utime, rtime);
}

/*
 * Set cputime to sum_cputime if sum_cputime > cputime. Use cmpxchg
 * to avoid race conditions with concurrent updates to cputime.
 */
/*
 * __update_gt_cputime() - 把原子累计单调提升到至少 @sum_cputime。
 * cmpxchg 循环只在新全量样本更大时写；并发 accounting 增长若已超过目标则退出，绝不倒退。无返回值。
 */
static inline void __update_gt_cputime(atomic64_t *cputime, u64 sum_cputime)
{
	u64 curr_cputime = atomic64_read(cputime);

	do {
		if (sum_cputime <= curr_cputime)
			return;
	} while (!atomic64_try_cmpxchg(cputime, &curr_cputime, sum_cputime));
}

/* update_gt_cputime() 对线程组 utime/stime/runtime 三个原子字段分别执行单调 max 合并。 */
static void update_gt_cputime(struct task_cputime_atomic *cputime_atomic,
			      struct task_cputime *sum)
{
	__update_gt_cputime(&cputime_atomic->utime, sum->utime);
	__update_gt_cputime(&cputime_atomic->stime, sum->stime);
	__update_gt_cputime(&cputime_atomic->sum_exec_runtime, sum->sum_exec_runtime);
}

/**
 * thread_group_sample_cputime - Sample cputime for a given task
 * @tsk:	Task for which cputime needs to be started
 * @samples:	Storage for time samples
 *
 * Called from sys_getitimer() to calculate the expiry time of an active
 * timer. That means group cputime accounting is already active. Called
 * with task sighand lock held.
 *
 * Updates @times with an uptodate sample of the thread group cputimes.
 */
/*
 * thread_group_sample_cputime() - 在已持 @tsk sighand siglock且 accounting active 时读取线程组原子样本。
 * inactive 会 WARN_ON_ONCE；向三元素 @samples 写 PROF/VIRT/SCHED。供 getitimer 等已启用路径使用。
 */
void thread_group_sample_cputime(struct task_struct *tsk, u64 *samples)
{
	struct thread_group_cputimer *cputimer = &tsk->signal->cputimer;
	struct posix_cputimers *pct = &tsk->signal->posix_cputimers;

	WARN_ON_ONCE(!pct->timers_active);

	proc_sample_cputime_atomic(&cputimer->cputime_atomic, samples);
}

/**
 * thread_group_start_cputime - Start cputime and return a sample
 * @tsk:	Task for which cputime needs to be started
 * @samples:	Storage for time samples
 *
 * The thread group cputime accounting is avoided when there are no posix
 * CPU timers armed. Before starting a timer it's required to check whether
 * the time accounting is active. If not, a full update of the atomic
 * accounting store needs to be done and the accounting enabled.
 *
 * Updates @times with an uptodate sample of the thread group cputimes.
 */
/*
 * thread_group_start_cputime() - 必要时启用线程组 CPU 原子累计，并返回当前样本。
 * 要求持 @tsk sighand siglock。若 timers_active 尚 false，先全量聚合线程组，把三个原子值单调提升，再
 * WRITE_ONCE 发布 active；并发 accounting 可由 max 合并容忍，无需额外 barrier。最后读原子 store 填输出。
 */
static void thread_group_start_cputime(struct task_struct *tsk, u64 *samples)
{
	struct thread_group_cputimer *cputimer = &tsk->signal->cputimer;
	struct posix_cputimers *pct = &tsk->signal->posix_cputimers;

	lockdep_assert_task_sighand_held(tsk);

	/* Check if cputimer isn't running. This is accessed without locking. */
	/* active 是 accounting 热路径的无锁门，读写均用 READ/WRITE_ONCE 防编译器合并。 */
	if (!READ_ONCE(pct->timers_active)) {
		struct task_cputime sum;

		/*
		 * The POSIX timer interface allows for absolute time expiry
		 * values through the TIMER_ABSTIME flag, therefore we have
		 * to synchronize the timer to the clock every time we start it.
		 */
		/* 绝对 CPU 期限必须与当前累计坐标同步，不能从零或旧缓存开始。 */
		thread_group_cputime(tsk, &sum);
		update_gt_cputime(&cputimer->cputime_atomic, &sum);

		/*
		 * We're setting timers_active without a lock. Ensure this
		 * only gets written to in one operation. We set it after
		 * update_gt_cputime() as a small optimization, but
		 * barriers are not required because update_gt_cputime()
		 * can handle concurrent updates.
		 */
		/* 先补齐累计再发布 active；并发增量与补齐通过 atomic max 保持单调。 */
		WRITE_ONCE(pct->timers_active, true);
	}
	proc_sample_cputime_atomic(&cputimer->cputime_atomic, samples);
}

/* __thread_group_cputime() 在 accounting 未启用时执行一次完整线程组聚合，写三元素样本但不启用热路径。 */
static void __thread_group_cputime(struct task_struct *tsk, u64 *samples)
{
	struct task_cputime ct;

	thread_group_cputime(tsk, &ct);
	store_samples(samples, ct.stime, ct.utime, ct.sum_exec_runtime);
}

/*
 * Sample a process (thread group) clock for the given task clkid. If the
 * group's cputime accounting is already enabled, read the atomic
 * store. Otherwise a full update is required.  clkid is already validated.
 */
/*
 * cpu_clock_sample_group() - 采样进程范围已校验的 @clkid。
 * accounting inactive 时，@start=true（调用者持 siglock）走 start_cputime 启用并同步；false 只做一次全量
 * 聚合。active 时直接读原子累计。返回目标 index 纳秒，不保留 samples。
 */
static u64 cpu_clock_sample_group(const clockid_t clkid, struct task_struct *p,
				  bool start)
{
	struct thread_group_cputimer *cputimer = &p->signal->cputimer;
	struct posix_cputimers *pct = &p->signal->posix_cputimers;
	u64 samples[CPUCLOCK_MAX];

	if (!READ_ONCE(pct->timers_active)) {
		if (start)
			thread_group_start_cputime(p, samples);
		else
			__thread_group_cputime(p, samples);
	} else {
		proc_sample_cputime_atomic(&cputimer->cputime_atomic, samples);
	}

	return samples[clkid];
}

/*
 * posix_cpu_clock_get() - 解析任意 CPU clockid 并返回 timespec64 CPU 用时。
 * RCU 下 pid_for_clock(gettime=true)+pid_task 查目标，失败 -EINVAL；thread 调 cpu_clock_sample，process 调
 * group sample(start=false)，随后解 RCU并把纳秒转 @tp，返回 0。读取不启用长期 group accounting。
 */
static int posix_cpu_clock_get(const clockid_t clock, struct timespec64 *tp)
{
	const clockid_t clkid = CPUCLOCK_WHICH(clock);
	struct task_struct *tsk;
	u64 t;

	rcu_read_lock();
	tsk = pid_task(pid_for_clock(clock, true), clock_pid_type(clock));
	if (!tsk) {
		rcu_read_unlock();
		return -EINVAL;
	}

	if (CPUCLOCK_PERTHREAD(clock))
		t = cpu_clock_sample(clkid, tsk);
	else
		t = cpu_clock_sample_group(clkid, tsk, false);
	rcu_read_unlock();

	*tp = ns_to_timespec64(t);
	return 0;
}

/*
 * Validate the clockid_t for a new CPU-clock timer, and initialize the timer.
 * This is called from sys_timer_create() and do_cpu_nanosleep() with the
 * new timer already all-zeros initialized.
 */
/*
 * posix_cpu_timer_create() - 校验新 k_itimer 的 CPU clock 目标并初始化 cpu_timer union。
 * @new_timer 已清零、it_clock/it_lock 已由 core 设置。RCU 下 pid_for_clock(gettime=false)，无效返回 -EINVAL；
 * TASK_WORK 配置给 it_lock 单独 lockdep class，避免同一 class 同时记录 irq/task context 的误报。成功把 kclock
 * 切为 clock_posix_cpu、初始化 timerqueue node、get_pid 持有目标引用，返回 0；尚未入任何 base。
 */
static int posix_cpu_timer_create(struct k_itimer *new_timer)
{
	static struct lock_class_key posix_cpu_timers_key;
	struct pid *pid;

	rcu_read_lock();
	pid = pid_for_clock(new_timer->it_clock, false);
	if (!pid) {
		rcu_read_unlock();
		return -EINVAL;
	}

	/*
	 * If posix timer expiry is handled in task work context then
	 * timer::it_lock can be taken without disabling interrupts as all
	 * other locking happens in task context. This requires a separate
	 * lock class key otherwise regular posix timer expiry would record
	 * the lock class being taken in interrupt context and generate a
	 * false positive warning.
	 */
	/* task-work 到期只在任务上下文取 it_lock，独立 class 表达与 hardirq 模式不同的上下文约束。 */
	if (IS_ENABLED(CONFIG_POSIX_CPU_TIMERS_TASK_WORK))
		lockdep_set_class(&new_timer->it_lock, &posix_cpu_timers_key);

	new_timer->kclock = &clock_posix_cpu;
	timerqueue_init(&new_timer->it.cpu.node);
	new_timer->it.cpu.pid = get_pid(pid);
	rcu_read_unlock();
	return 0;
}

/* timer_base() 按 timer clock 的 PERTHREAD 位与 which index 选择 @tsk 私有或 signal 共享的 posix_cputimer_base。 */
static struct posix_cputimer_base *timer_base(struct k_itimer *timer,
					      struct task_struct *tsk)
{
	int clkidx = CPUCLOCK_WHICH(timer->it_clock);

	if (CPUCLOCK_PERTHREAD(timer->it_clock))
		return tsk->posix_cputimers.bases + clkidx;
	else
		return tsk->signal->posix_cputimers.bases + clkidx;
}

/*
 * Force recalculating the base earliest expiration on the next tick.
 * This will also re-evaluate the need to keep around the process wide
 * cputime counter and tick dependency and eventually shut these down
 * if necessary.
 */
/*
 * trigger_base_recalc_expires() - 把 timer 所属 base->nextevt 置 0，强制下一 tick 进入慢路径重算。
 * 调用者持目标 sighand siglock；这也让慢路径重新判断 group accounting/tick dependency 是否可关闭。
 */
static void trigger_base_recalc_expires(struct k_itimer *timer,
					struct task_struct *tsk)
{
	struct posix_cputimer_base *base = timer_base(timer, tsk);

	base->nextevt = 0;
}

/*
 * Dequeue the timer and reset the base if it was its earliest expiration.
 * It makes sure the next tick recalculates the base next expiration so we
 * don't keep the costly process wide cputime counter around for a random
 * amount of time, along with the tick dependency.
 *
 * If another timer gets queued between this and the next tick, its
 * expiration will update the base next event if necessary on the next
 * tick.
 */
/*
 * disarm_timer() - 在已持目标 sighand siglock 时从 timerqueue 摘除 @timer。
 * 未入队直接返回；若被删期限等于 base 当前最早缓存，置 nextevt=0 让下一 tick 重算，避免无期限保留昂贵
 * group accounting/tick。只维护 queue/cache，不改 k_itimer status 或 expires。
 */
static void disarm_timer(struct k_itimer *timer, struct task_struct *p)
{
	struct cpu_timer *ctmr = &timer->it.cpu;
	struct posix_cputimer_base *base;

	if (!cpu_timer_dequeue(ctmr))
		return;

	base = timer_base(timer, p);
	if (cpu_timer_getexpires(ctmr) == base->nextevt)
		trigger_base_recalc_expires(timer, p);
}

/*
 * Lookup the task via timer->it.cpu.pid and attempt to lock the task's sighand.
 *
 * This can race with the reaping of the task:
 *
 * CPU0					CPU1
 *
 * // Finds task
 * p = pid_task(pid, pid_type);		__exit_signal(p)
 *					  lock(p, sighand);
 *					  posix_cpu_timers*_exit();
 * sighand = lock_task_sighand(p);	  unhash_task(p);
 *					  p->sighand = NULL;
 *					  unlock(sighand);
 *
 * In this case sighand is NULL, which means the task and the associated timer
 * queue cannot be longer accessed safely.
 *
 * __exit_signal() invokes posix_cpu_timers_exit() and if the thread group is
 * dead it also invokes posix_cpu_timers_group_exit(). These functions delete
 * all pending timers from the related timer queues. The POSIX timers (k_itimer)
 * themself are still accessible, but not longer connected to the task.
 *
 * exec() works slightly differently. The task which exec()'s terminates all
 * other threads in the thread group and runs __exit_signal() on them. As the
 * thread group is not dead they only clean up the per task timers via
 * posix_cpu_timers_exit().
 *
 * As the TGID on exec() stays the same per process timers stay queued, if they
 * are armed. This works without a problem when exec() is done by the thread
 * group leader. If a non-leader thread exec()'s this can end up in the
 * following scenario:
 *
 * CPU0					CPU1
 * // Returns old leader
 * p = pid_task(pid, pid_type);		de_thread()
 *					switch_leader()
 *					release_task(old leader)
 *					  __exit_signal()
 *					  old_leader->sighand = NULL;
 * // Returns NULL
 * sighand = lock_task_sighand(p)
 *
 * That's problematic for several functions:
 *
 *  - posix_cpu_timer_del(): If the timer is still enqueued on the task the
 *    underlying k_itimer will be freed which results in a UAF in
 *    run_posix_cpu_timers() or on timerqueue related add/delete operations.
 *    If the timer is not enqueued, the failure is harmless
 *
 *  - posix_cpu_timer_set(): Independent of the enqueued state that results in a
 *    transient failure which is user space visible (-ESRCH) for regular posix
 *    timers. But for the use case in do_cpu_nanosleep() it's the same UAF
 *    problem just that the timer is allocated on the stack.
 *
 *  - posix_cpu_timer_rearm(): Timer is not enqueued at that point, but this
 *    silently ignores the rearm request, which is a functional problem as the
 *    timer wont expire anymore.
 */
/*
 * timer_lock_sighand() - 通过 timer 持有 pid 反复解析当前 task/leader并取得其 sighand siglock。
 * @flags 输出 irq 状态；RCU 覆盖 pid_task 与 leader 切换。目标不存在则失败；lock_task_sighand 因 exit/exec
 * 竞态失败时重新 lookup，使非 leader exec 后可转到新 leader。成功返回锁定的 task 借用指针，调用者必须
 * unlock_task_sighand。
 *
 * 最终无 task 时，exit 清理应已在 siglock 下从所有 timerqueue 摘除节点；smp_rmb 与 exit 侧写屏障配对后
 * WARN 检查 head/node 双状态。失败返回 NULL且无锁，不能再访问 task queue；k_itimer 本身仍由 core 引用稳定。
 * 上方保留的 CPU0/CPU1 时序解释了直接使用一次 pid lookup 会造成 delete/set/rearm UAF 或漏重装的原因。
 */
static struct task_struct *timer_lock_sighand(struct k_itimer *timer, unsigned long *flags)
{
	enum pid_type type = clock_pid_type(timer->it_clock);
	struct cpu_timer *ctmr = &timer->it.cpu;

	guard(rcu)();

	for (;;) {
		struct task_struct *t = pid_task(timer->it.cpu.pid, type);

		/* Fail if the task cannot be found. */
		/* pid 已无目标表示 exit 清理完成或正在完成，转到屏障后的脱队断言。 */
		if (!t)
			break;

		/* Try to lock the task's sighand */
		/* 成功锁定同时稳定 task->sighand、signal 与对应 timerqueue。 */
		if (lock_task_sighand(t, flags))
			return t;

		/*
		 * The next PID lookup might either fail or return the new
		 * leader. This is correct for both exit() and exec().
		 */
		/* 失败后重新解析可观察 exec 切换的新 leader；exit 则最终观察 NULL。 */
	}

	/*
	 * If the timer is still enqueued, warn. There is nothing safe to do
	 * here as there might be two timers in there which are removed in
	 * parallel and that will cause more damage than good. This should never
	 * happen!
	 *
	 * Ensure that the stores to the timer and timerqueue are visible:
	 *
	 * __exit_signal()
	 *   posix_cpu_timers*_exit()
	 *   write_seqlock(seqlock)
	 *	smp_wmb(); <-------
	 *   __unhash_process()	  |	!pid_task()
	 *			  ---->	smp_rmb();
	 *				WARN_ON_ONCE(...)
	 */
	/* 读屏障确保看到 exit 在 unhash task 之前完成的 timerqueue 脱链写入。 */
	smp_rmb();
	WARN_ON_ONCE(ctmr->head || timerqueue_node_queued(&ctmr->node));
	return NULL;
}

/*
 * Clean up a CPU-clock timer that is about to be destroyed.
 * This is called from timer deletion with the timer already locked.
 * If we return TIMER_RETRY, it's necessary to release the timer's lock
 * and try again.  (This happens when the timer is in the middle of firing.)
 */
/*
 * posix_cpu_timer_del() - clock-specific 删除/销毁 CPU timer。
 * 调用者持 it_lock。先取得目标 sighand；若 firing，清 flag 阻止 firing-list 路径发事件，但节点不受 siglock
 * 保护不能就地删除，返回 TIMER_RETRY 让 core 解锁等待后重试；否则 disarm。目标已退出时 cleanup 已脱队。
 * 非 RETRY 才 put_pid、置 DISARMED并返回 0；RETRY 保留 pid ownership 供下一轮。
 */
static int posix_cpu_timer_del(struct k_itimer *timer)
{
	struct task_struct *p;
	unsigned long flags;
	int ret = 0;

	p = timer_lock_sighand(timer, &flags);

	if (likely(p)) {
		if (timer->it.cpu.firing) {
			/*
			 * Prevent signal delivery. The timer cannot be dequeued
			 * because it is on the firing list which is not protected
			 * by sighand->lock. The delivery path is waiting for
			 * the timer lock. So go back, unlock and retry.
			 */
			/* firing list 私有于 expiry handler；这里只能撤销其 firing 意图并走等待协议。 */
			timer->it.cpu.firing = false;
			ret = TIMER_RETRY;
		} else {
			disarm_timer(timer, p);
		}
		unlock_task_sighand(p, &flags);
	}

	if (!ret) {
		put_pid(timer->it.cpu.pid);
		timer->it_status = POSIX_TIMER_DISARMED;
	}
	return ret;
}

/*
 * cleanup_timerqueue() - exit 时清空一个 base queue。
 * 在调用者持 siglock且无后续 queue 使用的前提下，反复删除最早 node，并把每个 cpu_timer->head 清 NULL；
 * 不改 k_itimer status/expires/ref，timer 对象之后仍可由 ID 查到但不能再重装到已消失 task。
 */
static void cleanup_timerqueue(struct timerqueue_head *head)
{
	struct timerqueue_node *node;
	struct cpu_timer *ctmr;

	while ((node = timerqueue_getnext(head))) {
		timerqueue_del(head, node);
		ctmr = container_of(node, struct cpu_timer, node);
		ctmr->head = NULL;
	}
}

/*
 * Clean out CPU timers which are still armed when a thread exits. The
 * timers are only removed from the list. No other updates are done. The
 * corresponding posix timers are still accessible, but cannot be rearmed.
 *
 * This must be called with the siglock held.
 */
/* cleanup_timers() 在 siglock 下依次清 PROF/VIRT/SCHED 三个队列；不重置 cache，因为 scope 正在销毁。 */
static void cleanup_timers(struct posix_cputimers *pct)
{
	cleanup_timerqueue(&pct->bases[CPUCLOCK_PROF].tqhead);
	cleanup_timerqueue(&pct->bases[CPUCLOCK_VIRT].tqhead);
	cleanup_timerqueue(&pct->bases[CPUCLOCK_SCHED].tqhead);
}

/*
 * These are both called with the siglock held, when the current thread
 * is being reaped.  When the final (leader) thread in the group is reaped,
 * posix_cpu_timers_exit_group will be called after posix_cpu_timers_exit.
 */
/* posix_cpu_timers_exit() 清 current/目标线程私有三队列；由 exit signal 路径持 siglock调用。 */
void posix_cpu_timers_exit(struct task_struct *tsk)
{
	cleanup_timers(&tsk->posix_cputimers);
}
/* posix_cpu_timers_exit_group() 在最后线程退出时继续清 signal_struct 共享三队列；同样要求 siglock。 */
void posix_cpu_timers_exit_group(struct task_struct *tsk)
{
	cleanup_timers(&tsk->signal->posix_cputimers);
}

/*
 * Insert the timer on the appropriate list before any timers that
 * expire later.  This must be called with the sighand lock held.
 */
/*
 * arm_timer() - 把 CPU timer 按绝对 expires 插入所属线程/进程 base。
 * 要求持目标 sighand siglock与 timer it_lock。先置 ARMED；timerqueue_add 返回 false 表示不是新最早项，缓存
 * 无需改。若成为最早，按 min 更新与 itimer/RLIMIT 共享的 nextevt，并为 task/signal 设置 POSIX_TIMER tick
 * dependency，保证 NO_HZ 仍周期检查 CPU 用时。无失败返回，对象必须未入队。
 */
static void arm_timer(struct k_itimer *timer, struct task_struct *p)
{
	struct posix_cputimer_base *base = timer_base(timer, p);
	struct cpu_timer *ctmr = &timer->it.cpu;
	u64 newexp = cpu_timer_getexpires(ctmr);

	timer->it_status = POSIX_TIMER_ARMED;
	if (!cpu_timer_enqueue(&base->tqhead, ctmr))
		return;

	/*
	 * We are the new earliest-expiring POSIX 1.b timer, hence
	 * need to update expiration cache. Take into account that
	 * for process timers we share expiration cache with itimers
	 * and RLIMIT_CPU and for thread timers with RLIMIT_RTTIME.
	 */
	/* 同一 cache 还承载传统 CPU itimer/rlimit，故只能做 min 收紧，完整放宽留给慢路径重算。 */
	if (newexp < base->nextevt)
		base->nextevt = newexp;

	if (CPUCLOCK_PERTHREAD(timer->it_clock))
		tick_dep_set_task(p, TICK_DEP_BIT_POSIX_TIMER);
	else
		tick_dep_set_signal(p, TICK_DEP_BIT_POSIX_TIMER);
}

/*
 * The timer is locked, fire it and arrange for its reload.
 */
/*
 * cpu_timer_fire() - 在已持 @timer->it_lock、节点已移到 firing list 后完成一次到期。
 * 先置 DISARMED。栈上 nanosleep timer 唤醒 it_process 并清 expires=0 作为完成标志；普通 timer 调
 * posix_timer_queue_signal，周期 timer 保留旧 expires 等信号递送 rearm，oneshot 清 expires。无返回值。
 */
static void cpu_timer_fire(struct k_itimer *timer)
{
	struct cpu_timer *ctmr = &timer->it.cpu;

	timer->it_status = POSIX_TIMER_DISARMED;

	if (unlikely(ctmr->nanosleep)) {
		/*
		 * This a special case for clock_nanosleep,
		 * not a normal timer from sys_timer_create.
		 */
		/* nanosleep 不使用 sigqueue；wake 与 expires=0 共同向睡眠循环发布完成。 */
		wake_up_process(timer->it_process);
		cpu_timer_setexpires(ctmr, 0);
	} else {
		posix_timer_queue_signal(timer);
		/* Disable oneshot timers */
		/* 周期期限需留给 bump_cpu_timer，只有 interval=0 才真正清零。 */
		if (!timer->it_interval)
			cpu_timer_setexpires(ctmr, 0);
	}
}

static void __posix_cpu_timer_get(struct k_itimer *timer, struct itimerspec64 *itp, u64 now);

/*
 * Guts of sys_timer_settime for CPU timers.
 * This is called with the timer locked and interrupts disabled.
 * If we return TIMER_RETRY, it's necessary to release the timer's lock
 * and try again.  (This happens when the timer is in the middle of firing.)
 */
/*
 * posix_cpu_timer_set() - 在 it_lock 下替换 CPU timer 设置。
 * 取得目标 sighand，失败 -ESRCH。新 value 经 timespec->ktime 钳到 KTIME_MAX 后取纳秒；保存旧绝对 expires。
 * firing 时清 flag并标 TIMER_RETRY，否则从 queue 摘除、置 DISARMED。随后采样目标 clock：process 的真实
 * signal timer 会启用 group accounting，SIGEV_NONE 不启用。可选 old 用旧 expires 与同一 now 求剩余。
 *
 * RETRY 解 sighand 原样返回，不提交新值。成功时相对值加 now 转绝对，写 expires；真实 signal 且期限在
 * 未来则 arm，禁用/已过期则触发 cache 重算，SIGEV_NONE 永不入队。解 sighand 后提交 interval/overrun；
 * 已过期真实 timer 在当前 it_lock 路径立即 fire，即使目标以后不再运行。返回 0/TIMER_RETRY/-ESRCH。
 */
static int posix_cpu_timer_set(struct k_itimer *timer, int timer_flags,
			       struct itimerspec64 *new, struct itimerspec64 *old)
{
	bool sigev_none = timer->it_sigev_notify == SIGEV_NONE;
	clockid_t clkid = CPUCLOCK_WHICH(timer->it_clock);
	struct cpu_timer *ctmr = &timer->it.cpu;
	u64 old_expires, new_expires, now;
	struct task_struct *p;
	unsigned long flags;
	int ret = 0;

	p = timer_lock_sighand(timer, &flags);
	/*
	 * If p has just been reaped, we can no longer get any information about
	 * it at all.
	 */
	/* 目标已被回收时 queue 已由 exit 清理，但也无法再采样旧值或提交新期限。 */
	if (!p)
		return -ESRCH;

	/*
	 * Use the to_ktime conversion because that clamps the maximum
	 * value to KTIME_MAX and avoid multiplication overflows.
	 */
	/* ktime 转换统一执行饱和，避免 sec 到 ns 直接乘法溢出。 */
	new_expires = ktime_to_ns(timespec64_to_ktime(new->it_value));

	/* Retrieve the current expiry time before disarming the timer */
	/* 旧绝对值必须在 dequeue/新值覆盖前保存，用于 old_setting。 */
	old_expires = cpu_timer_getexpires(ctmr);

	if (unlikely(timer->it.cpu.firing)) {
		/*
		 * Prevent signal delivery. The timer cannot be dequeued
		 * because it is on the firing list which is not protected
		 * by sighand->lock. The delivery path is waiting for
		 * the timer lock. So go back, unlock and retry.
		 */
		/* 与 delete 相同，firing list 不受 siglock保护，只能清意图并请求外层等待重试。 */
		timer->it.cpu.firing = false;
		ret = TIMER_RETRY;
	} else {
		cpu_timer_dequeue(ctmr);
		timer->it_status = POSIX_TIMER_DISARMED;
	}

	/*
	 * Sample the current clock for saving the previous setting
	 * and for rearming the timer.
	 */
	/* process signal timer 的 start=true 会按需启用原子组累计；纯查询型 SIGEV_NONE 避免这项成本。 */
	if (CPUCLOCK_PERTHREAD(timer->it_clock))
		now = cpu_clock_sample(clkid, p);
	else
		now = cpu_clock_sample_group(clkid, p, !sigev_none);

	/* Retrieve the previous expiry value if requested. */
	if (old) {
		old->it_value = (struct timespec64){ };
		if (old_expires)
			__posix_cpu_timer_get(timer, old, now);
	}

	/* Retry if the timer expiry is running concurrently */
	/* RETRY 不覆盖 expires/interval；old 若请求仍代表首次尝试观察的旧状态。 */
	if (unlikely(ret)) {
		unlock_task_sighand(p, &flags);
		return ret;
	}

	/* Convert relative expiry time to absolute */
	/* CPU 时间只在目标执行时增长，相对值以本次样本为基点固定成绝对 CPU 用时。 */
	if (new_expires && !(timer_flags & TIMER_ABSTIME))
		new_expires += now;

	/* Set the new expiry time (might be 0) */
	cpu_timer_setexpires(ctmr, new_expires);

	/*
	 * Arm the timer if it is not disabled, the new expiry value has
	 * not yet expired and the timer requires signal delivery.
	 * SIGEV_NONE timers are never armed. In case the timer is not
	 * armed, enforce the reevaluation of the timer base so that the
	 * process wide cputime counter can be disabled eventually.
	 */
	/* SIGEV_NONE 只保存绝对期限供 gettime 模拟；真实 timer 的过期/禁用都迫使 base 后续重算。 */
	if (likely(!sigev_none)) {
		if (new_expires && now < new_expires)
			arm_timer(timer, p);
		else
			trigger_base_recalc_expires(timer, p);
	}

	unlock_task_sighand(p, &flags);

	posix_timer_set_common(timer, new);

	/*
	 * If the new expiry time was already in the past the timer was not
	 * queued. Fire it immediately even if the thread never runs to
	 * accumulate more time on this clock.
	 */
	/* 过去期限不会靠未来 CPU tick 触发，必须在 set syscall 当前上下文同步产生事件。 */
	if (!sigev_none && new_expires && now >= new_expires)
		cpu_timer_fire(timer);
	return ret;
}

/*
 * __posix_cpu_timer_get() - 用已采样 @now 填 CPU timer 的剩余 it_value。
 * 周期 timer 若未 ARMED（SIGEV_NONE 或信号待递送）先 bump 到 now 之后；否则读当前 expires。未来期限返回差值；
 * 已过期 SIGEV_NONE oneshot 保持预清零的 0，真实 signal timer 返回 1ns 待递送哨兵。调用者持 it_lock。
 */
static void __posix_cpu_timer_get(struct k_itimer *timer, struct itimerspec64 *itp, u64 now)
{
	bool sigev_none = timer->it_sigev_notify == SIGEV_NONE;
	u64 expires, iv = timer->it_interval;

	/*
	 * Make sure that interval timers are moved forward for the
	 * following cases:
	 *  - SIGEV_NONE timers which are never armed
	 *  - Timers which expired, but the signal has not yet been
	 *    delivered
	 */
	/* 未入队周期 timer 在查询时模拟应有的周期推进并累计 overrun。 */
	if (iv && timer->it_status != POSIX_TIMER_ARMED)
		expires = bump_cpu_timer(timer, now);
	else
		expires = cpu_timer_getexpires(&timer->it.cpu);

	/*
	 * Expired interval timers cannot have a remaining time <= 0.
	 * The kernel has to move them forward so that the next
	 * timer expiry is > @now.
	 */
	/* bump 保证周期 timer 严格晚于 now，因此非正分支只剩 oneshot/待信号语义。 */
	if (now < expires) {
		itp->it_value = ns_to_timespec64(expires - now);
	} else {
		/*
		 * A single shot SIGEV_NONE timer must return 0, when it is
		 * expired! Timers which have a real signal delivery mode
		 * must return a remaining time greater than 0 because the
		 * signal has not yet been delivered.
		 */
		/* 与 wall-clock common_timer_get 对齐：真实信号待消费用 1ns，SIGEV_NONE 真实过期用 0。 */
		if (!sigev_none)
			itp->it_value.tv_nsec = 1;
	}
}

/*
 * posix_cpu_timer_get() - clock-specific timer_get 包装。
 * 调用者已预清零输出并持 it_lock；RCU 下找目标。仅目标存在且 expires 非零时写 interval、采样 thread/group
 * clock(start=false) 并调用内部 remaining；否则保持全零。无返回值，不入队或启用 group accounting。
 */
static void posix_cpu_timer_get(struct k_itimer *timer, struct itimerspec64 *itp)
{
	clockid_t clkid = CPUCLOCK_WHICH(timer->it_clock);
	struct task_struct *p;
	u64 now;

	rcu_read_lock();
	p = cpu_timer_task_rcu(timer);
	if (p && cpu_timer_getexpires(&timer->it.cpu)) {
		itp->it_interval = ktime_to_timespec64(timer->it_interval);

		if (CPUCLOCK_PERTHREAD(timer->it_clock))
			now = cpu_clock_sample(clkid, p);
		else
			now = cpu_clock_sample_group(clkid, p, false);

		__posix_cpu_timer_get(timer, itp, now);
	}
	rcu_read_unlock();
}

#define MAX_COLLECTED	20
/* MAX_COLLECTED 是单 base 扫描停止阈值；当前 ++i==20 时先返回，实际一次最多搬出 19 个到期节点。 */

/*
 * collect_timerqueue() - 在 sighand siglock 下从一个有序 CPU timerqueue 收集到期项到私有 @firing。
 * 从队首开始；遇未来期限或第 20 个候选立即返回该 expires 作为新 nextevt。对实际收集项置 firing=true，
 * RCU 发布 handling=current 供 cancel waiter，dequeue 后接 firing 尾。队列耗尽返回 U64_MAX。
 */
static u64 collect_timerqueue(struct timerqueue_head *head,
			      struct list_head *firing, u64 now)
{
	struct timerqueue_node *next;
	int i = 0;

	while ((next = timerqueue_getnext(head))) {
		struct cpu_timer *ctmr;
		u64 expires;

		ctmr = container_of(next, struct cpu_timer, node);
		expires = cpu_timer_getexpires(ctmr);
		/* Limit the number of timers to expire at once */
		/* 阈值保留后续项在 queue 中，限制一次持 siglock/随后 firing 处理的工作量。 */
		if (++i == MAX_COLLECTED || now < expires)
			return expires;

		ctmr->firing = true;
		/* See posix_cpu_timer_wait_running() */
		/* handling 必须在搬出前发布，firing 与等待协议共同保护私有链窗口。 */
		rcu_assign_pointer(ctmr->handling, current);
		cpu_timer_dequeue(ctmr);
		list_add_tail(&ctmr->elist, firing);
	}

	return U64_MAX;
}

/* collect_posix_cputimers() 对 PROF/VIRT/SCHED 三个 base 以对应样本收集，并用返回值重建各 nextevt。 */
static void collect_posix_cputimers(struct posix_cputimers *pct, u64 *samples,
				    struct list_head *firing)
{
	struct posix_cputimer_base *base = pct->bases;
	int i;

	for (i = 0; i < CPUCLOCK_MAX; i++, base++) {
		base->nextevt = collect_timerqueue(&base->tqhead, firing,
						    samples[i]);
	}
}

/* check_dl_overrun() 在 siglock 下消费 deadline task 的 dl_overrun 标志，并向线程组发送一次 SIGXCPU。 */
static inline void check_dl_overrun(struct task_struct *tsk)
{
	if (tsk->dl.dl_overrun) {
		tsk->dl.dl_overrun = 0;
		send_signal_locked(SIGXCPU, SEND_SIG_PRIV, tsk, PIDTYPE_TGID);
	}
}

/*
 * check_rlimit() - 比较当前 CPU/RT @time 与 @limit，并在越界时发送 @signo。
 * 未到返回 false；到达时可按 print_fatal_signals 打印 current 的 RT/CPU、hard/soft 诊断，随后在已持 siglock
 * 下向 current TGID 发送内核特权信号并返回 true。@rt/@hard 只影响日志文本。
 */
static bool check_rlimit(u64 time, u64 limit, int signo, bool rt, bool hard)
{
	if (time < limit)
		return false;

	if (print_fatal_signals) {
		pr_info("%s Watchdog Timeout (%s): %s[%d]\n",
			rt ? "RT" : "CPU", hard ? "hard" : "soft",
			current->comm, task_pid_nr(current));
	}
	send_signal_locked(signo, SEND_SIG_PRIV, current, PIDTYPE_TGID);
	return true;
}

/*
 * Check for any per-thread CPU timers that have fired and move them off
 * the tsk->cpu_timers[N] list onto the firing list.  Here we update the
 * tsk->it_*_expires values to reflect the remaining thread CPU timers.
 */
/*
 * check_thread_timers() - 在 @tsk sighand siglock 下处理线程范围 CPU 到期源。
 * deadline task 先消费 overrun；三 cache 全 inactive 则返回。采样线程三时钟并把到期 POSIX timer 搬到
 * @firing，同时重建 nextevt。再检查 RLIMIT_RTTIME：timeout jiffies 转微秒，hard 到达发 SIGKILL并返回；
 * soft 到达发 SIGXCPU，并把 soft limit 推迟 1 秒以周期提醒。最终若三 cache inactive，清 task tick dependency。
 */
static void check_thread_timers(struct task_struct *tsk,
				struct list_head *firing)
{
	struct posix_cputimers *pct = &tsk->posix_cputimers;
	u64 samples[CPUCLOCK_MAX];
	unsigned long soft;

	if (dl_task(tsk))
		check_dl_overrun(tsk);

	if (expiry_cache_is_inactive(pct))
		return;

	task_sample_cputime(tsk, samples);
	collect_posix_cputimers(pct, samples, firing);

	/*
	 * Check for the special case thread timers.
	 */
	/* RLIMIT_RTTIME 与 thread POSIX bases 共享 scheduler tick 检查入口，但不存于 timerqueue。 */
	soft = task_rlimit(tsk, RLIMIT_RTTIME);
	if (soft != RLIM_INFINITY) {
		/* Task RT timeout is accounted in jiffies. RTTIME is usec */
		/* scheduler 保存 jiffy timeout，用户 rlimit 单位微秒，按 HZ 比例转换。 */
		unsigned long rttime = tsk->rt.timeout * (USEC_PER_SEC / HZ);
		unsigned long hard = task_rlimit_max(tsk, RLIMIT_RTTIME);

		/* At the hard limit, send SIGKILL. No further action. */
		/* hard limit 是终止路径，不再更新 cache/soft 阈值。 */
		if (hard != RLIM_INFINITY &&
		    check_rlimit(rttime, hard, SIGKILL, true, true))
			return;

		/* At the soft limit, send a SIGXCPU every second */
		/* 写回 rlim_cur 形成每额外 1 秒一次的后续 SIGXCPU。 */
		if (check_rlimit(rttime, soft, SIGXCPU, true, false)) {
			soft += USEC_PER_SEC;
			tsk->signal->rlim[RLIMIT_RTTIME].rlim_cur = soft;
		}
	}

	if (expiry_cache_is_inactive(pct))
		tick_dep_clear_task(tsk, TICK_DEP_BIT_POSIX_TIMER);
}

/*
 * stop_process_timers() - 关闭线程组 CPU accounting/tick 快路径。
 * WRITE_ONCE 清 timers_active，并清 signal POSIX_TIMER tick dependency；要求 siglock 且三个 nextevt 已 inactive。
 */
static inline void stop_process_timers(struct signal_struct *sig)
{
	struct posix_cputimers *pct = &sig->posix_cputimers;

	/* Turn off the active flag. This is done without locking. */
	/* accounting 热路径无锁读取该门，故以单次 WRITE_ONCE 发布关闭。 */
	WRITE_ONCE(pct->timers_active, false);
	tick_dep_clear_signal(sig, TICK_DEP_BIT_POSIX_TIMER);
}

/*
 * check_cpu_itimer() - 检查一个传统进程 CPU itimer（PROF/VIRTUAL）并收紧共享 @expires cache。
 * 未启用直接返回；@cur_time 到期时，周期只推进一个 incr，oneshot 清零，trace 后向 @tsk TGID 发 @signo。
 * 若新期限仍非零且更早，更新 base nextevt。要求 siglock；远落后周期会在后续 tick 继续补发而非一次跨越。
 */
static void check_cpu_itimer(struct task_struct *tsk, struct cpu_itimer *it,
			     u64 *expires, u64 cur_time, int signo)
{
	if (!it->expires)
		return;

	if (cur_time >= it->expires) {
		if (it->incr)
			it->expires += it->incr;
		else
			it->expires = 0;

		trace_itimer_expire(signo == SIGPROF ?
				    ITIMER_PROF : ITIMER_VIRTUAL,
				    task_tgid(tsk), cur_time);
		send_signal_locked(signo, SEND_SIG_PRIV, tsk, PIDTYPE_TGID);
	}

	if (it->expires && it->expires < *expires)
		*expires = it->expires;
}

/*
 * Check for any per-thread CPU timers that have fired and move them
 * off the tsk->*_timers list onto the firing list.  Per-thread timers
 * have already been taken off.
 */
/*
 * check_process_timers() - 在 siglock 下由一个线程处理整个线程组的 CPU 到期源。
 * timers_active=false 或另一线程 expiry_active 时跳过；否则置 expiry_active，读取已启用原子组样本，收集
 * process POSIX queues，并检查 ITIMER_PROF/VIRTUAL 与共享 cache。
 *
 * RLIMIT_CPU 秒值转 PROF 纳秒：hard 到达发 SIGKILL并直接返回；soft 到达发 SIGXCPU、把 rlim_cur 与下次
 * cache 推迟 1 秒。若最终三 cache inactive，关闭 group accounting/tick；正常路径清 expiry_active。
 * hard-limit 终止路径保留 expiry_active=true，因为线程组将被 SIGKILL 退出。
 */
static void check_process_timers(struct task_struct *tsk,
				 struct list_head *firing)
{
	struct signal_struct *const sig = tsk->signal;
	struct posix_cputimers *pct = &sig->posix_cputimers;
	u64 samples[CPUCLOCK_MAX];
	unsigned long soft;

	/*
	 * If there are no active process wide timers (POSIX 1.b, itimers,
	 * RLIMIT_CPU) nothing to check. Also skip the process wide timer
	 * processing when there is already another task handling them.
	 */
	/* 两个无锁门只是避免重复重活；实际写 expiry_active 在 siglock 下串行。 */
	if (!READ_ONCE(pct->timers_active) || pct->expiry_active)
		return;

	/*
	 * Signify that a thread is checking for process timers.
	 * Write access to this field is protected by the sighand lock.
	 */
	/* expiry_active 阻止同组其他 tick 同时扫描共享 queues。 */
	pct->expiry_active = true;

	/*
	 * Collect the current process totals. Group accounting is active
	 * so the sample can be taken directly.
	 */
	/* timers_active 保证 accounting hot path 已持续更新原子累计，可直接读而无需全组遍历。 */
	proc_sample_cputime_atomic(&sig->cputimer.cputime_atomic, samples);
	collect_posix_cputimers(pct, samples, firing);

	/*
	 * Check for the special case process timers.
	 */
	/* 传统 itimer 与 POSIX timer 共用 PROF/VIRT nextevt，检查后都以 min 贡献下次期限。 */
	check_cpu_itimer(tsk, &sig->it[CPUCLOCK_PROF],
			 &pct->bases[CPUCLOCK_PROF].nextevt,
			 samples[CPUCLOCK_PROF], SIGPROF);
	check_cpu_itimer(tsk, &sig->it[CPUCLOCK_VIRT],
			 &pct->bases[CPUCLOCK_VIRT].nextevt,
			 samples[CPUCLOCK_VIRT], SIGVTALRM);

	soft = task_rlimit(tsk, RLIMIT_CPU);
	if (soft != RLIM_INFINITY) {
		/* RLIMIT_CPU is in seconds. Samples are nanoseconds */
		/* 限额乘 NSEC_PER_SEC 后与 PROF 纳秒样本同单位比较。 */
		unsigned long hard = task_rlimit_max(tsk, RLIMIT_CPU);
		u64 ptime = samples[CPUCLOCK_PROF];
		u64 softns = (u64)soft * NSEC_PER_SEC;
		u64 hardns = (u64)hard * NSEC_PER_SEC;

		/* At the hard limit, send SIGKILL. No further action. */
		/* 终止信号后不再维护 expiry_active/cache，退出清理将销毁整个 scope。 */
		if (hard != RLIM_INFINITY &&
		    check_rlimit(ptime, hardns, SIGKILL, false, true))
			return;

		/* At the soft limit, send a SIGXCPU every second */
		/* 每触及一次 soft 阈值便原地增加 1 秒，实现持续超限的周期告警。 */
		if (check_rlimit(ptime, softns, SIGXCPU, false, false)) {
			sig->rlim[RLIMIT_CPU].rlim_cur = soft + 1;
			softns += NSEC_PER_SEC;
		}

		/* Update the expiry cache */
		/* RLIMIT soft 下一告警也是 PROF base 的一个隐式期限。 */
		if (softns < pct->bases[CPUCLOCK_PROF].nextevt)
			pct->bases[CPUCLOCK_PROF].nextevt = softns;
	}

	if (expiry_cache_is_inactive(pct))
		stop_process_timers(sig);

	pct->expiry_active = false;
}

/*
 * This is called from the signal code (via posixtimer_rearm)
 * when the last timer signal was delivered and we have to reload the timer.
 *
 * Return true unconditionally so the core code assumes the timer to be
 * armed. Otherwise it would requeue the signal.
 */
/*
 * posix_cpu_timer_rearm() - timer 信号实际递送后推进并重新插入周期 CPU timer。
 * 调用者持 it_lock。反复解析并锁目标 sighand；目标已退出时返回 true 假装 armed，避免 core 重排信号，timer
 * 实际不再到期。成功则采样 thread/group（group start=true），bump 到 now 之后，arm 并解锁。固定返回 true。
 */
static bool posix_cpu_timer_rearm(struct k_itimer *timer)
{
	clockid_t clkid = CPUCLOCK_WHICH(timer->it_clock);
	struct task_struct *p;
	unsigned long flags;
	u64 now;

	p = timer_lock_sighand(timer, &flags);
	if (unlikely(!p))
		return true;

	/*
	 * Fetch the current sample and update the timer's expiry time.
	 */
	/* 使用递送时最新 CPU 样本计算跨期数与新绝对期限。 */
	if (CPUCLOCK_PERTHREAD(timer->it_clock))
		now = cpu_clock_sample(clkid, p);
	else
		now = cpu_clock_sample_group(clkid, p, true);

	bump_cpu_timer(timer, now);

	/*
	 * Now re-arm for the new expiry time.
	 */
	/* arm 更新 queue/cache/tick dependency；core 随后把状态视为 ARMED。 */
	arm_timer(timer, p);
	unlock_task_sighand(p, &flags);
	return true;
}

/**
 * task_cputimers_expired - Check whether posix CPU timers are expired
 *
 * @samples:	Array of current samples for the CPUCLOCK clocks
 * @pct:	Pointer to a posix_cputimers container
 *
 * Returns true if any member of @samples is greater than the corresponding
 * member of @pct->bases[CLK].nextevt. False otherwise
 */
/* task_cputimers_expired() 顺序比较三样本与 cache，任一 sample>=nextevt 返回 true；U64_MAX 表示永不到期。 */
static inline bool
task_cputimers_expired(const u64 *samples, struct posix_cputimers *pct)
{
	int i;

	for (i = 0; i < CPUCLOCK_MAX; i++) {
		if (samples[i] >= pct->bases[i].nextevt)
			return true;
	}
	return false;
}

/**
 * fastpath_timer_check - POSIX CPU timers fast path.
 *
 * @tsk:	The task (thread) being checked.
 *
 * Check the task and thread group timers.  If both are zero (there are no
 * timers set) return false.  Otherwise snapshot the task and thread group
 * timers and compare them with the corresponding expiration times.  Return
 * true if a timer has expired, else return false.
 */
/*
 * fastpath_timer_check() - scheduler tick 的无锁启发式到期判定。
 * thread cache active 时采样并比较；process timers_active 且无人 expiry_active 时读原子组样本比较；deadline
 * overrun 也返回 true。无命中 false。并发门状态陈旧最多把信号延迟到同组后续 tick，不承担精确状态提交。
 */
static inline bool fastpath_timer_check(struct task_struct *tsk)
{
	struct posix_cputimers *pct = &tsk->posix_cputimers;
	struct signal_struct *sig;

	if (!expiry_cache_is_inactive(pct)) {
		u64 samples[CPUCLOCK_MAX];

		task_sample_cputime(tsk, samples);
		if (task_cputimers_expired(samples, pct))
			return true;
	}

	sig = tsk->signal;
	pct = &sig->posix_cputimers;
	/*
	 * Check if thread group timers expired when timers are active and
	 * no other thread in the group is already handling expiry for
	 * thread group cputimers. These fields are read without the
	 * sighand lock. However, this is fine because this is meant to be
	 * a fastpath heuristic to determine whether we should try to
	 * acquire the sighand lock to handle timer expiry.
	 *
	 * In the worst case scenario, if concurrently timers_active is set
	 * or expiry_active is cleared, but the current thread doesn't see
	 * the change yet, the timer checks are delayed until the next
	 * thread in the group gets a scheduler interrupt to handle the
	 * timer. This isn't an issue in practice because these types of
	 * delays with signals actually getting sent are expected.
	 */
	/* 无锁读取只决定是否值得尝试 siglock，真正扫描会在锁下重验并重建 cache。 */
	if (READ_ONCE(pct->timers_active) && !READ_ONCE(pct->expiry_active)) {
		u64 samples[CPUCLOCK_MAX];

		proc_sample_cputime_atomic(&sig->cputimer.cputime_atomic,
					   samples);

		if (task_cputimers_expired(samples, pct))
			return true;
	}

	if (dl_task(tsk) && tsk->dl.dl_overrun)
		return true;

	return false;
}

static void handle_posix_cpu_timers(struct task_struct *tsk);

#ifdef CONFIG_POSIX_CPU_TIMERS_TASK_WORK
/*
 * posix_cpu_timers_work() - TWA_RESUME task_work callback。
 * 从 @work 反推 current 的 work 容器，持 per-task mutex 覆盖完整 handle；该 mutex 也是 cancel waiter 的完成
 * 屏障。回调可睡眠，返回前释放 mutex。
 */
static void posix_cpu_timers_work(struct callback_head *work)
{
	struct posix_cputimers_work *cw = container_of(work, typeof(*cw), work);

	mutex_lock(&cw->mutex);
	handle_posix_cpu_timers(current);
	mutex_unlock(&cw->mutex);
}

/*
 * Invoked from the posix-timer core when a cancel operation failed because
 * the timer is marked firing. The caller holds rcu_read_lock(), which
 * protects the timer and the task which is expiring it from being freed.
 */
/*
 * posix_cpu_timer_wait_running() - TASK_WORK 模式等待 firing handler 越过目标 timer。
 * 调用者持 RCU且不持 it_lock；RCU 读取 handling。NULL 表示已完成。非空则 get_task_struct，主动退 RCU后
 * 锁/立即解 handling task 的 work mutex，等待整次 handle 完成，再 put task并重进 RCU以平衡调用点。
 * 返回后原 timr 可能失效，不可访问。
 */
static void posix_cpu_timer_wait_running(struct k_itimer *timr)
{
	struct task_struct *tsk = rcu_dereference(timr->it.cpu.handling);

	/* Has the handling task completed expiry already? */
	/* handler 清 handling 在解 mutex 前完成；NULL 可直接判定无需等待。 */
	if (!tsk)
		return;

	/* Ensure that the task cannot go away */
	/* task 引用替代即将释放的 RCU 保护，使阻塞 mutex 期间容器稳定。 */
	get_task_struct(tsk);
	/* Now drop the RCU protection so the mutex can be locked */
	rcu_read_unlock();
	/* Wait on the expiry mutex */
	mutex_lock(&tsk->posix_cputimers_work.mutex);
	/* Release it immediately again. */
	mutex_unlock(&tsk->posix_cputimers_work.mutex);
	/* Drop the task reference. */
	put_task_struct(tsk);
	/* Relock RCU so the callsite is balanced */
	rcu_read_lock();
}

/*
 * posix_cpu_timer_wait_running_nsleep() - 栈上 nanosleep timer 的专用等待包装。
 * 自行进入 RCU、释放 irq-disabled it_lock，调用通用 wait（内部可退/重进 RCU），结束 RCU后重取 it_lock；
 * timer 在当前栈上固定存活，返回后可继续 delete。
 */
static void posix_cpu_timer_wait_running_nsleep(struct k_itimer *timr)
{
	/* Ensure that timr->it.cpu.handling task cannot go away */
	rcu_read_lock();
	spin_unlock_irq(&timr->it_lock);
	posix_cpu_timer_wait_running(timr);
	rcu_read_unlock();
	/* @timr is on stack and is valid */
	spin_lock_irq(&timr->it_lock);
}

/*
 * Clear existing posix CPU timers task work.
 */
/*
 * clear_posix_cputimers_work() - fork/init 时重建 @p 的 CPU timer task-work 容器。
 * 先清掉从旧 task 复制而无意义的 callback_head（init_task_work 本身不清），再绑定静态 callback、初始化 mutex
 * 并清 scheduled。调用者独占尚未运行的新 task；无返回值。
 */
void clear_posix_cputimers_work(struct task_struct *p)
{
	/*
	 * A copied work entry from the old task is not meaningful, clear it.
	 * N.B. init_task_work will not do this.
	 */
	memset(&p->posix_cputimers_work.work, 0,
	       sizeof(p->posix_cputimers_work.work));
	init_task_work(&p->posix_cputimers_work.work,
		       posix_cpu_timers_work);
	mutex_init(&p->posix_cputimers_work.mutex);
	p->posix_cputimers_work.scheduled = false;
}

/*
 * Initialize posix CPU timers task work in init task. Out of line to
 * keep the callback static and to avoid header recursion hell.
 */
/* posix_cputimers_init_work() 在 timer 初始化阶段对 init/current task 调用公共 work 初始化，避免头文件引用 callback。 */
void __init posix_cputimers_init_work(void)
{
	clear_posix_cputimers_work(current);
}

/*
 * Note: All operations on tsk->posix_cputimer_work.scheduled happen either
 * in hard interrupt context or in task context with interrupts
 * disabled. Aside of that the writer/reader interaction is always in the
 * context of the current task, which means they are strict per CPU.
 */
/* posix_cpu_timers_work_scheduled() 在 irq-off/current-task 约束下读取 scheduled，无额外原子操作。 */
static inline bool posix_cpu_timers_work_scheduled(struct task_struct *tsk)
{
	return tsk->posix_cputimers_work.scheduled;
}

/*
 * __run_posix_cpu_timers() - TASK_WORK 模式把实际 expiry 安排到 @tsk 返回用户前。
 * 重复 scheduled WARN并返回；否则先置 true，再 task_work_add(TWA_RESUME)。调用者是该任务 tick 的 irq-off路径，
 * task 生命周期稳定；本实现不处理 add 返回值，依约束应成功。
 */
static inline void __run_posix_cpu_timers(struct task_struct *tsk)
{
	if (WARN_ON_ONCE(tsk->posix_cputimers_work.scheduled))
		return;

	/* Schedule task work to actually expire the timers */
	/* scheduled 先于发布 work，后续 tick 将跳过重复排队。 */
	tsk->posix_cputimers_work.scheduled = true;
	task_work_add(tsk, &tsk->posix_cputimers_work.work, TWA_RESUME);
}

/*
 * posix_cpu_timers_enable_work() - collection 后安全重开 tick 快路径，并判断是否需立即再扫。
 * !PREEMPT_RT 时 siglock 已关 IRQ，无 tick 穿越，直接清 scheduled并 true。RT 时先 local_irq_disable；若
 * jiffies 自 @start 推进且 fastpath 又发现到期，保持 scheduled并 false 让 handle 循环，否则清位并 true。
 * 返回前恢复 IRQ；调用者仍持 sighand lock。
 */
static inline bool posix_cpu_timers_enable_work(struct task_struct *tsk,
						unsigned long start)
{
	bool ret = true;

	/*
	 * On !RT kernels interrupts are disabled while collecting expired
	 * timers, so no tick can happen and the fast path check can be
	 * reenabled without further checks.
	 */
	/* 非 RT 的 irq-off 临界区保证 collection 与清 scheduled 间没有漏掉 tick。 */
	if (!IS_ENABLED(CONFIG_PREEMPT_RT)) {
		tsk->posix_cputimers_work.scheduled = false;
		return true;
	}

	/*
	 * On RT enabled kernels ticks can happen while the expired timers
	 * are collected under sighand lock. But any tick which observes
	 * the CPUTIMERS_WORK_SCHEDULED bit set, does not run the fastpath
	 * checks. So reenabling the tick work has do be done carefully:
	 *
	 * Disable interrupts and run the fast path check if jiffies have
	 * advanced since the collecting of expired timers started. If
	 * jiffies have not advanced or the fast path check did not find
	 * newly expired timers, reenable the fast path check in the timer
	 * interrupt. If there are newly expired timers, return false and
	 * let the collection loop repeat.
	 */
	/* RT siglock 不关硬中断，使用 jiffies 变化+再采样封闭 scheduled 清位窗口。 */
	local_irq_disable();
	if (start != jiffies && fastpath_timer_check(tsk))
		ret = false;
	else
		tsk->posix_cputimers_work.scheduled = false;
	local_irq_enable();

	return ret;
}
#else /* CONFIG_POSIX_CPU_TIMERS_TASK_WORK */
/* 非 TASK_WORK 模式在当前 timer interrupt 内直接处理，并用 lockdep 标记 POSIX timer irq 上下文。 */
static inline void __run_posix_cpu_timers(struct task_struct *tsk)
{
	lockdep_posixtimer_enter();
	handle_posix_cpu_timers(tsk);
	lockdep_posixtimer_exit();
}

/* 非 TASK_WORK callback 在 hardirq 同步推进，cancel 重试只需 cpu_relax 等持锁 handler 获得进度。 */
static void posix_cpu_timer_wait_running(struct k_itimer *timr)
{
	cpu_relax();
}

/* 栈上 nanosleep 在非 TASK_WORK 模式临时解 it_lock、relax 后重取，允许 firing 路径先完成。 */
static void posix_cpu_timer_wait_running_nsleep(struct k_itimer *timr)
{
	spin_unlock_irq(&timr->it_lock);
	cpu_relax();
	spin_lock_irq(&timr->it_lock);
}

/* 非 TASK_WORK 没有 deferred scheduled 状态，恒 false。 */
static inline bool posix_cpu_timers_work_scheduled(struct task_struct *tsk)
{
	return false;
}

/* 非 TASK_WORK collection 与 firing 同步位于 irq-off路径，无需重开 work 门，恒 true。 */
static inline bool posix_cpu_timers_enable_work(struct task_struct *tsk,
						unsigned long start)
{
	return true;
}
#endif /* CONFIG_POSIX_CPU_TIMERS_TASK_WORK */

/*
 * handle_posix_cpu_timers() - CPU timer expiry 慢路径与 firing 两阶段提交。
 * 先 lock_task_sighand；每轮记录 jiffies/barrier，在 siglock 下检查 thread/process，将到期节点置 firing、发布
 * handling并搬到局部链，同时重建 caches。TASK_WORK+RT 若 collection 窗口穿越 tick 且又有到期项则循环。
 *
 * 解 siglock 后局部 firing 链只由本函数拥有。逐项取 it_lock、脱链并读取/清 firing；若 set/delete 已清 flag
 * 则跳过事件，否则 cpu_timer_fire。最后 RCU 清 handling 并解锁。锁序避免持 siglock 等 it_lock；TASK_WORK
 * mutex 或 hardirq 同步保证 cancel waiter 最终观察完成。目标 sighand 已消失则直接返回。
 */
static void handle_posix_cpu_timers(struct task_struct *tsk)
{
	struct k_itimer *timer, *next;
	unsigned long flags, start;
	LIST_HEAD(firing);

	if (!lock_task_sighand(tsk, &flags))
		return;

	do {
		/*
		 * On RT locking sighand lock does not disable interrupts,
		 * so this needs to be careful vs. ticks. Store the current
		 * jiffies value.
		 */
		/* barrier 保证 RT enable_work 用 start 判断期间不被编译器跨 collection 重排。 */
		start = READ_ONCE(jiffies);
		barrier();

		/*
		 * Here we take off tsk->signal->cpu_timers[N] and
		 * tsk->cpu_timers[N] all the timers that are firing, and
		 * put them on the firing list.
		 */
		/* siglock 下同时处理 task 私有和 signal 共享 queue，建立一致 cache。 */
		check_thread_timers(tsk, &firing);

		check_process_timers(tsk, &firing);

		/*
		 * The above timer checks have updated the expiry cache and
		 * because nothing can have queued or modified timers after
		 * sighand lock was taken above it is guaranteed to be
		 * consistent. So the next timer interrupt fastpath check
		 * will find valid data.
		 *
		 * If timer expiry runs in the timer interrupt context then
		 * the loop is not relevant as timers will be directly
		 * expired in interrupt context. The stub function below
		 * returns always true which allows the compiler to
		 * optimize the loop out.
		 *
		 * If timer expiry is deferred to task work context then
		 * the following rules apply:
		 *
		 * - On !RT kernels no tick can have happened on this CPU
		 *   after sighand lock was acquired because interrupts are
		 *   disabled. So reenabling task work before dropping
		 *   sighand lock and reenabling interrupts is race free.
		 *
		 * - On RT kernels ticks might have happened but the tick
		 *   work ignored posix CPU timer handling because the
		 *   CPUTIMERS_WORK_SCHEDULED bit is set. Reenabling work
		 *   must be done very carefully including a check whether
		 *   ticks have happened since the start of the timer
		 *   expiry checks. posix_cpu_timers_enable_work() takes
		 *   care of that and eventually lets the expiry checks
		 *   run again.
		 */
	} while (!posix_cpu_timers_enable_work(tsk, start));

	/*
	 * We must release sighand lock before taking any timer's lock.
	 * There is a potential race with timer deletion here, as the
	 * siglock now protects our private firing list.  We have set
	 * the firing flag in each timer, so that a deletion attempt
	 * that gets the timer lock before we do will give it up and
	 * spin until we've taken care of that timer below.
	 */
	/* 搬链后必须先退 siglock再取 it_lock；firing flag 充当跨锁域的取消握手。 */
	unlock_task_sighand(tsk, &flags);

	/*
	 * Now that all the timers on our list have the firing flag,
	 * no one will touch their list entries but us.  We'll take
	 * each timer's lock before clearing its firing flag, so no
	 * timer call will interfere.
	 */
	/* elist 已私有，it_lock 只保护事件状态与 cancel/set 竞争，不再保护链所有权。 */
	list_for_each_entry_safe(timer, next, &firing, it.cpu.elist) {
		bool cpu_firing;

		/*
		 * spin_lock() is sufficient here even independent of the
		 * expiry context. If expiry happens in hard interrupt
		 * context it's obvious. For task work context it's safe
		 * because all other operations on timer::it_lock happen in
		 * task context (syscall or exit).
		 */
		/* hardirq 已关 IRQ；task-work class 保证其他 it_lock 用户也仅在任务上下文，普通 spin_lock 足够。 */
		spin_lock(&timer->it_lock);
		list_del_init(&timer->it.cpu.elist);
		cpu_firing = timer->it.cpu.firing;
		timer->it.cpu.firing = false;
		/*
		 * If the firing flag is cleared then this raced with a
		 * timer rearm/delete operation. So don't generate an
		 * event.
		 */
		/* set/delete 抢先清 firing 表示本次到期已撤销，只做清理不发信号/唤醒。 */
		if (likely(cpu_firing))
			cpu_timer_fire(timer);
		/* See posix_cpu_timer_wait_running() */
		/* handling=NULL 发布单 timer 完成；mutex 仍覆盖整个 handler，供阻塞 waiter 作为更强屏障。 */
		rcu_assign_pointer(timer->it.cpu.handling, NULL);
		spin_unlock(&timer->it_lock);
	}
}

/*
 * This is called from the timer interrupt handler.  The irq handler has
 * already updated our counts.  We need to check if any timers fire now.
 * Interrupts are disabled.
 */
/*
 * run_posix_cpu_timers() - scheduler tick 更新 CPU 计数后的 irq-off入口。
 * current 已 exit 时跳过，防止 release_task 与 firing 检测竞态；TASK_WORK 已 scheduled 时跳过；fastpath 无到期
 * 跳过，否则按配置直接处理或排 task_work。函数不返回状态，lockdep 断言 IRQ 已关闭。
 */
void run_posix_cpu_timers(void)
{
	struct task_struct *tsk = current;

	lockdep_assert_irqs_disabled();

	/*
	 * Ensure that release_task(tsk) can't happen while
	 * handle_posix_cpu_timers() is running. Otherwise, a concurrent
	 * posix_cpu_timer_del() may fail to lock_task_sighand(tsk) and
	 * miss timer->it.cpu.firing != 0.
	 */
	/* exit_state 门让 task/sighand 生命周期覆盖后续 handler 与 delete 竞争。 */
	if (tsk->exit_state)
		return;

	/*
	 * If the actual expiry is deferred to task work context and the
	 * work is already scheduled there is no point to do anything here.
	 */
	/* deferred work 已承担检查时，后续 tick 不重复扫描或排同一 callback_head。 */
	if (posix_cpu_timers_work_scheduled(tsk))
		return;

	/*
	 * The fast path checks that there are no expired thread or thread
	 * group timers.  If that's so, just return.
	 */
	/* cache heuristic 是最热路径；只有可能到期才进入取 siglock 的慢路径。 */
	if (!fastpath_timer_check(tsk))
		return;

	__run_posix_cpu_timers(tsk);
}

/*
 * Set one of the process-wide special case CPU timers or RLIMIT_CPU.
 * The tsk->sighand->siglock must be held by the caller.
 */
/*
 * set_process_cpu_timer() - 更新传统 process CPU itimer 或 RLIMIT_CPU 的共享 nextevt/tick 门。
 * @clkid 只接受 PROF/VIRT；调用者持 @tsk siglock。采样并按需启用 group accounting。@oldval 非空表示 itimer：
 * 把旧绝对值转剩余（已到期用 TICK_NSEC），把非零新相对值转绝对；NULL 表示 @newval 已是 RLIMIT 绝对 CPU
 * 时间。若新值早于 cache 则收紧，始终设置 signal tick dependency；放宽/关闭的完整重算留给慢路径。
 */
void set_process_cpu_timer(struct task_struct *tsk, unsigned int clkid,
			   u64 *newval, u64 *oldval)
{
	u64 now, *nextevt;

	if (WARN_ON_ONCE(clkid >= CPUCLOCK_SCHED))
		return;

	nextevt = &tsk->signal->posix_cputimers.bases[clkid].nextevt;
	now = cpu_clock_sample_group(clkid, tsk, true);

	if (oldval) {
		/*
		 * We are setting itimer. The *oldval is absolute and we update
		 * it to be relative, *newval argument is relative and we update
		 * it to be absolute.
		 */
		/* oldval 的存在区分 setitimer 双向换算与 update_rlimit_cpu 的绝对 limit 输入。 */
		if (*oldval) {
			if (*oldval <= now) {
				/* Just about to fire. */
				/* 已过期但尚未处理，用一个 tick 表示即将触发而非已关闭。 */
				*oldval = TICK_NSEC;
			} else {
				*oldval -= now;
			}
		}

		if (*newval)
			*newval += now;
	}

	/*
	 * Update expiration cache if this is the earliest timer. CPUCLOCK_PROF
	 * expiry cache is also used by RLIMIT_CPU!.
	 */
	/* cache 只能安全收紧；若新期限更晚，旧最早项可能来自其他 timer/rlimit。 */
	if (*newval < *nextevt)
		*nextevt = *newval;

	tick_dep_set_signal(tsk, TICK_DEP_BIT_POSIX_TIMER);
}

/*
 * do_cpu_nanosleep() - 用栈上特殊 CPU k_itimer 等待目标 CPU clock 到期。
 * 清零 timer、初始化 it_lock/clock/overrun，经 posix_cpu_timer_create 持目标 pid，标记 nanosleep并保存 current
 * 供到期唤醒。创建成功后构造 it_value，锁内 set；失败则 del/解锁并返回。
 *
 * 等待循环持锁检查 expires=0（fire 已完成），否则设 TASK_INTERRUPTIBLE、解锁 schedule、醒后重取；信号
 * pending 时保存绝对 expires，以零设置尝试 disarm并取得 remaining。若与 firing 竞争，使用栈对象专用
 * wait后反复 del；最终解锁。remaining=0 表示实际已到期返回 0，否则返回 -ERESTART_RESTARTBLOCK，并把绝对
 * expires 存 restart_block、按 TT_NATIVE/COMPAT 可选复制剩余时间。所有成功创建路径都 put pid且不遗留队列。
 */
static int do_cpu_nanosleep(const clockid_t which_clock, int flags,
			    const struct timespec64 *rqtp)
{
	struct itimerspec64 it;
	struct k_itimer timer;
	u64 expires;
	int error;

	/*
	 * Set up a temporary timer and then wait for it to go off.
	 */
	/* 栈对象必须在所有 firing/cancel 完成后才离开函数，nanosleep 标志选择 wake 而非 sigqueue。 */
	memset(&timer, 0, sizeof timer);
	spin_lock_init(&timer.it_lock);
	timer.it_clock = which_clock;
	timer.it_overrun = -1;
	error = posix_cpu_timer_create(&timer);
	timer.it_process = current;
	timer.it.cpu.nanosleep = true;

	if (!error) {
		static struct itimerspec64 zero_it;
		struct restart_block *restart;

		memset(&it, 0, sizeof(it));
		it.it_value = *rqtp;

		spin_lock_irq(&timer.it_lock);
		error = posix_cpu_timer_set(&timer, flags, &it, NULL);
		if (error) {
			posix_cpu_timer_del(&timer);
			spin_unlock_irq(&timer.it_lock);
			return error;
		}

		while (!signal_pending(current)) {
			if (!cpu_timer_getexpires(&timer.it.cpu)) {
				/*
				 * Our timer fired and was reset, below
				 * deletion can not fail.
				 */
				/* cpu_timer_fire 已清 expires，局部 timer 不在 firing/queue，delete 可直接收尾 pid。 */
				posix_cpu_timer_del(&timer);
				spin_unlock_irq(&timer.it_lock);
				return 0;
			}

			/*
			 * Block until cpu_timer_fire (or a signal) wakes us.
			 */
			/* 先设置睡眠态再解锁，firing 期间 wake_up_process 可封闭检查到 schedule 的漏唤醒窗口。 */
			__set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock_irq(&timer.it_lock);
			schedule();
			spin_lock_irq(&timer.it_lock);
		}

		/*
		 * We were interrupted by a signal.
		 */
		/* signal pending 退出睡眠循环，随后必须与可能并发的 firing 完成握手再销毁栈对象。 */
		expires = cpu_timer_getexpires(&timer.it.cpu);
		error = posix_cpu_timer_set(&timer, 0, &zero_it, &it);
		if (!error) {
			/* Timer is now unarmed, deletion can not fail. */
			/* zero set 已摘队并清 firing，delete 只归还 pid。 */
			posix_cpu_timer_del(&timer);
		} else {
			while (error == TIMER_RETRY) {
				posix_cpu_timer_wait_running_nsleep(&timer);
				error = posix_cpu_timer_del(&timer);
			}
		}

		spin_unlock_irq(&timer.it_lock);

		if ((it.it_value.tv_sec | it.it_value.tv_nsec) == 0) {
			/*
			 * It actually did fire already.
			 */
			/* 信号与到期竞态中到期获胜，POSIX 语义按睡眠完成返回 0。 */
			return 0;
		}

		error = -ERESTART_RESTARTBLOCK;
		/*
		 * Report back to the user the time still remaining.
		 */
		/* 保存绝对 CPU 期限保证 syscall restart 不会重复计算已经消耗的相对时长。 */
		restart = &current->restart_block;
		restart->nanosleep.expires = ns_to_ktime(expires);
		if (restart->nanosleep.type != TT_NONE)
			error = nanosleep_copyout(restart, &it.it_value);
	}

	return error;
}

static long posix_cpu_nsleep_restart(struct restart_block *restart_block);

/*
 * posix_cpu_nsleep() - 任意编码 CPU clock 的 nanosleep 前置校验与 restart 安装。
 * 禁止睡 current 自身 thread CPU clock（pid=0 或自身 vnr），因为线程睡眠时该 clock 不增长，返回 -EINVAL。
 * 其余调用 do_cpu_nanosleep；若返回 restart，ABSTIME 改为 -ERESTARTNOHAND，不自动重启；相对请求保存原
 * clockid并安装 posix_cpu_nsleep_restart。其他成功/错误原样返回；未知 flags 仅由 TIMER_ABSTIME 位影响 set。
 */
static int posix_cpu_nsleep(const clockid_t which_clock, int flags,
			    const struct timespec64 *rqtp)
{
	struct restart_block *restart_block = &current->restart_block;
	int error;

	/*
	 * Diagnose required errors first.
	 */
	/* 自身 thread CPU sleep 无可推进者，必须在分配临时 timer 前诊断。 */
	if (CPUCLOCK_PERTHREAD(which_clock) &&
	    (CPUCLOCK_PID(which_clock) == 0 ||
	     CPUCLOCK_PID(which_clock) == task_pid_vnr(current)))
		return -EINVAL;

	error = do_cpu_nanosleep(which_clock, flags, rqtp);

	if (error == -ERESTART_RESTARTBLOCK) {

		if (flags & TIMER_ABSTIME)
			return -ERESTARTNOHAND;

		restart_block->nanosleep.clockid = which_clock;
		set_restart_fn(restart_block, posix_cpu_nsleep_restart);
	}
	return error;
}

/*
 * posix_cpu_nsleep_restart() - 以首次睡眠保存的绝对 CPU expires 继续相对 sleep。
 * 从 @restart_block 取 clockid/ktime，转 timespec 后强制 TIMER_ABSTIME 调 do_cpu_nanosleep，避免重复加当前样本。
 */
static long posix_cpu_nsleep_restart(struct restart_block *restart_block)
{
	clockid_t which_clock = restart_block->nanosleep.clockid;
	struct timespec64 t;

	t = ktime_to_timespec64(restart_block->nanosleep.expires);

	return do_cpu_nanosleep(which_clock, TIMER_ABSTIME, &t);
}

#define PROCESS_CLOCK	make_process_cpuclock(0, CPUCLOCK_SCHED)
#define THREAD_CLOCK	make_thread_cpuclock(0, CPUCLOCK_SCHED)
/* 固定标准 PROCESS/THREAD clock 表统一映射到 pid=0 的 SCHED runtime 编码。 */

/* process_cpu_clock_getres() 忽略标准 id，按 current process SCHED clock 返回分辨率。 */
static int process_cpu_clock_getres(const clockid_t which_clock,
				    struct timespec64 *tp)
{
	return posix_cpu_clock_getres(PROCESS_CLOCK, tp);
}
/* process_cpu_clock_get() 读取 current 线程组的 SCHED runtime。 */
static int process_cpu_clock_get(const clockid_t which_clock,
				 struct timespec64 *tp)
{
	return posix_cpu_clock_get(PROCESS_CLOCK, tp);
}
/* process_cpu_timer_create() 把通用 core 传入 clock 改写为 PROCESS_CLOCK，再初始化 CPU timer。 */
static int process_cpu_timer_create(struct k_itimer *timer)
{
	timer->it_clock = PROCESS_CLOCK;
	return posix_cpu_timer_create(timer);
}
/* process_cpu_nsleep() 在 current process SCHED clock 上复用 CPU nanosleep；其他线程运行可推进期限。 */
static int process_cpu_nsleep(const clockid_t which_clock, int flags,
			      const struct timespec64 *rqtp)
{
	return posix_cpu_nsleep(PROCESS_CLOCK, flags, rqtp);
}
/* thread_cpu_clock_getres() 忽略标准 id，按 current thread SCHED clock 返回分辨率。 */
static int thread_cpu_clock_getres(const clockid_t which_clock,
				   struct timespec64 *tp)
{
	return posix_cpu_clock_getres(THREAD_CLOCK, tp);
}
/* thread_cpu_clock_get() 读取 current thread 的 SCHED runtime。 */
static int thread_cpu_clock_get(const clockid_t which_clock,
				struct timespec64 *tp)
{
	return posix_cpu_clock_get(THREAD_CLOCK, tp);
}
/* thread_cpu_timer_create() 改写为 THREAD_CLOCK 后初始化；timer 可在该线程实际运行时到期。 */
static int thread_cpu_timer_create(struct k_itimer *timer)
{
	timer->it_clock = THREAD_CLOCK;
	return posix_cpu_timer_create(timer);
}

/*
 * clock_posix_cpu 处理所有负 pid/type 编码 CPU clock：支持 getres/set(固定 EPERM)/get、timer create/set/get/del/
 * rearm/wait 与 nanosleep。clock_process/clock_thread 是两个非负标准 ID 的窄表，固定 current SCHED runtime；
 * process 允许 nanosleep，thread 不提供 nsleep。三表静态常驻，timer 对象仍由 POSIX core 管理。
 */
const struct k_clock clock_posix_cpu = {
	.clock_getres		= posix_cpu_clock_getres,
	.clock_set		= posix_cpu_clock_set,
	.clock_get_timespec	= posix_cpu_clock_get,
	.timer_create		= posix_cpu_timer_create,
	.nsleep			= posix_cpu_nsleep,
	.timer_set		= posix_cpu_timer_set,
	.timer_del		= posix_cpu_timer_del,
	.timer_get		= posix_cpu_timer_get,
	.timer_rearm		= posix_cpu_timer_rearm,
	.timer_wait_running	= posix_cpu_timer_wait_running,
};

const struct k_clock clock_process = {
	.clock_getres		= process_cpu_clock_getres,
	.clock_get_timespec	= process_cpu_clock_get,
	.timer_create		= process_cpu_timer_create,
	.nsleep			= process_cpu_nsleep,
};

const struct k_clock clock_thread = {
	.clock_getres		= thread_cpu_clock_getres,
	.clock_get_timespec	= thread_cpu_clock_get,
	.timer_create		= thread_cpu_timer_create,
};
