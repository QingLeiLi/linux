// SPDX-License-Identifier: GPL-2.0
/*
 * CPUFreq governor based on scheduler-provided CPU utilization data.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 */
#include <uapi/linux/sched/types.h>
#include "sched.h"

#define IOWAIT_BOOST_MIN	(SCHED_CAPACITY_SCALE / 8)

/* sysfs 可调参数及其 governor 属性集合；attr_set 引用决定共享 tunables 的释放时机。 */
struct sugov_tunables {
	struct gov_attr_set	attr_set;
	unsigned int		rate_limit_us;
};

/*
 * 一个 cpufreq policy 对应一个 schedutil 状态。update_lock 串行共享 policy 的多 CPU
 * 采样及慢路径排队；next/cached/last_time 实现去重和限速。驱动不能 fast switch 时，
 * irq_work 把 rq 锁内请求转交 SCHED_DEADLINE kthread，work_lock 再串行可睡眠驱动调用。
 * limits_changed/need_freq_update 跨 cpufreq 控制路径与更新回调传递强制重算请求。
 */
struct sugov_policy {
	struct cpufreq_policy	*policy;

	struct sugov_tunables	*tunables;
	struct list_head	tunables_hook;

	raw_spinlock_t		update_lock;
	u64			last_freq_update_time;
	s64			freq_update_delay_ns;
	unsigned int		next_freq;
	unsigned int		cached_raw_freq;

	/* The next fields are only needed if fast switch cannot be used: */
	struct			irq_work irq_work;
	struct			kthread_work work;
	struct			mutex work_lock;
	struct			kthread_worker worker;
	struct task_struct	*thread;
	bool			work_in_progress;

	bool			limits_changed;
	bool			need_freq_update;
};

/*
 * 每 CPU 回调状态：所属 policy、IO-wait boost 状态与最后时间、最近 util/带宽下限；
 * NO_HZ 的 idle 次数用于抑制过早降频。字段通常在目标 rq 锁下更新，共享 policy 还受
 * sg_policy->update_lock 保护。
 */
struct sugov_cpu {
	struct update_util_data	update_util;
	struct sugov_policy	*sg_policy;
	unsigned int		cpu;

	bool			iowait_boost_pending;
	unsigned int		iowait_boost;
	u64			last_update;

	unsigned long		util;
	unsigned long		bw_min;

	/* The field below is for single-CPU policies only: */
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long		saved_idle_calls;
#endif
};

static DEFINE_PER_CPU(struct sugov_cpu, sugov_cpu);

/************************ Governor internals ***********************/

/*
 * 判断本次回调能否且是否应更新频率。硬件不支持目标 CPU 发起请求时拒绝，避免远端
 * fast switch 或下线 CPU 遗留 irq_work；limits_changed 用 READ/WRITE_ONCE 加完整屏障
 * 与 sugov_limits 的写屏障配对并强制重算；否则 need 标志绕过限速，普通路径比较时间差。
 */
static bool sugov_should_update_freq(struct sugov_policy *sg_policy, u64 time)
{
	s64 delta_ns;

	/*
	 * Since cpufreq_update_util() is called with rq->lock held for
	 * the @target_cpu, our per-CPU data is fully serialized.
	 *
	 * However, drivers cannot in general deal with cross-CPU
	 * requests, so while get_next_freq() will work, our
	 * sugov_update_commit() call may not for the fast switching platforms.
	 *
	 * Hence stop here for remote requests if they aren't supported
	 * by the hardware, as calculating the frequency is pointless if
	 * we cannot in fact act on it.
	 *
	 * This is needed on the slow switching platforms too to prevent CPUs
	 * going offline from leaving stale IRQ work items behind.
	 */
	/* 原文结论：算得出频率不代表当前 CPU 能提交它，下线慢路径也必须在排队前挡住。 */
	if (!cpufreq_this_cpu_can_update(sg_policy->policy))
		return false;

	if (unlikely(READ_ONCE(sg_policy->limits_changed))) {
		WRITE_ONCE(sg_policy->limits_changed, false);
		sg_policy->need_freq_update = true;

		/*
		 * The above limits_changed update must occur before the reads
		 * of policy limits in cpufreq_driver_resolve_freq() or a policy
		 * limits update might be missed, so use a memory barrier to
		 * ensure it.
		 *
		 * This pairs with the write memory barrier in sugov_limits().
		 */
		/* 先消费 limits_changed 再读 policy limits，防止与控制路径更新交叉而永久漏重算。 */
		smp_mb();

		return true;
	} else if (sg_policy->need_freq_update) {
		/* ignore_dl_rate_limit() wants a new frequency to be found. */
		return true;
	}

	delta_ns = time - sg_policy->last_freq_update_time;

	return delta_ns >= sg_policy->freq_update_delay_ns;
}

/*
 * 提交候选 next_freq 到 governor 状态。强制更新时即使频率相同，只有驱动未声明
 * CPUFREQ_NEED_UPDATE_LIMITS 才可跳过；普通相同值直接去重。真正接受时更新时间戳，
 * 返回 true 让调用者执行 fast switch 或排慢工作。
 */
static bool sugov_update_next_freq(struct sugov_policy *sg_policy, u64 time,
				   unsigned int next_freq)
{
	if (sg_policy->need_freq_update) {
		sg_policy->need_freq_update = false;
		/*
		 * The policy limits have changed, but if the return value of
		 * cpufreq_driver_resolve_freq() after applying the new limits
		 * is still equal to the previously selected frequency, the
		 * driver callback need not be invoked unless the driver
		 * specifically wants that to happen on every update of the
		 * policy limits.
		 */
		if (sg_policy->next_freq == next_freq &&
		    !cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS))
			return false;
	} else if (sg_policy->next_freq == next_freq) {
		return false;
	}

	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;

	return true;
}

/* 慢切换只允许一个待处理 work；update_lock 下设置标志并排 irq_work，后续请求只改 next。 */
static void sugov_deferred_update(struct sugov_policy *sg_policy)
{
	if (!sg_policy->work_in_progress) {
		sg_policy->work_in_progress = true;
		irq_work_queue(&sg_policy->irq_work);
	}
}

/**
 * get_capacity_ref_freq - get the reference frequency that has been used to
 * correlate frequency and compute capacity for a given cpufreq policy. We use
 * the CPU managing it for the arch_scale_freq_ref() call in the function.
 * @policy: the cpufreq policy of the CPU in question.
 *
 * Return: the reference CPU frequency to compute a capacity.
 */
/*
 * 优先使用架构报告的容量参考频率；频率不变性存在但无显式参考值时用硬件最大频率；
 * 否则用当前频率加 25% 余量，使非不变 util 在约 80% 忙时提前请求更高档位。
 */
static __always_inline
unsigned long get_capacity_ref_freq(struct cpufreq_policy *policy)
{
	unsigned int freq = arch_scale_freq_ref(policy->cpu);

	if (freq)
		return freq;

	if (arch_scale_freq_invariant())
		return policy->cpuinfo.max_freq;

	/*
	 * Apply a 25% margin so that we select a higher frequency than
	 * the current one before the CPU is fully busy:
	 */
	/* 25% headroom 把升频拐点放在 util/max=0.8，而不是等 CPU 完全饱和。 */
	return policy->cur + (policy->cur >> 2);
}

/**
 * get_next_freq - Compute a new frequency for a given cpufreq policy.
 * @sg_policy: schedutil policy object to compute the new frequency for.
 * @util: Current CPU utilization.
 * @max: CPU capacity.
 *
 * If the utilization is frequency-invariant, choose the new frequency to be
 * proportional to it, that is
 *
 * next_freq = C * max_freq * util / max
 *
 * Otherwise, approximate the would-be frequency-invariant utilization by
 * util_raw * (curr_freq / max_freq) which leads to
 *
 * next_freq = C * curr_freq * util_raw / max
 *
 * Take C = 1.25 for the frequency tipping point at (util / max) = 0.8.
 *
 * The lowest driver-supported frequency which is equal or greater than the raw
 * next_freq (as calculated above) is returned, subject to policy min/max and
 * cpufreq driver limitations.
 */
/*
 * 将容量尺度 util 按参考频率映射成 raw 目标，再由驱动表/limits 向上解析为可用频点。
 * raw 值与缓存相同且无需强制更新时复用已解析 next_freq，避免重复查频率表。
 */
static unsigned int get_next_freq(struct sugov_policy *sg_policy,
				  unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq;

	freq = get_capacity_ref_freq(policy);
	freq = map_util_freq(util, freq, max);

	if (freq == sg_policy->cached_raw_freq && !sg_policy->need_freq_update)
		return sg_policy->next_freq;

	sg_policy->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

/*
 * 把实际利用率加 DVFS headroom，随后钳到调用者 max，并保证不低于 uclamp/DL 等给出的
 * min。返回容量尺度性能目标；cpu 参数保留在跨调度器调用接口中，当前实现不读取它，
 * 本函数也不直接修改频率。
 */
unsigned long sugov_effective_cpu_perf(int cpu, unsigned long actual,
				 unsigned long min,
				 unsigned long max)
{
	/* Add dvfs headroom to actual utilization */
	actual = map_util_perf(actual);
	/* Actually we don't need to target the max performance */
	if (actual < max)
		max = actual;

	/*
	 * Ensure at least minimum performance while providing more compute
	 * capacity when possible.
	 */
	return max(min, max);
}

/*
 * 合成当前 CPU 的性能需求：sched_ext 全接管时直接采用 BPF target，否则叠加 CFS util；
 * effective_cpu_util 计算 min/max 约束，IO boost 抬高 raw util，最终保存 bw_min 与有效目标。
 */
static void sugov_get_util(struct sugov_cpu *sg_cpu, unsigned long boost)
{
	unsigned long min, max, util = scx_cpuperf_target(sg_cpu->cpu);

	if (!scx_switched_all())
		util += cpu_util_cfs_boost(sg_cpu->cpu);
	util = effective_cpu_util(sg_cpu->cpu, util, &min, &max);
	util = max(util, boost);
	sg_cpu->bw_min = min;
	sg_cpu->util = sugov_effective_cpu_perf(sg_cpu->cpu, util, min, max);
}

/**
 * sugov_iowait_reset() - Reset the IO boost status of a CPU.
 * @sg_cpu: the sugov data for the CPU to boost
 * @time: the update time from the caller
 * @set_iowait_boost: true if an IO boost has been requested
 *
 * The IO wait boost of a task is disabled after a tick since the last update
 * of a CPU. If a new IO wait boost is requested after more then a tick, then
 * we enable the boost starting from IOWAIT_BOOST_MIN, which improves energy
 * efficiency by ignoring sporadic wakeups from IO.
 */
/*
 * 距上次更新超过一个 tick 才认为 boost 序列中断；按当前是否又收到 IOWAIT 请求重置为
 * 最小 boost 或 0，并同步 pending。未超时返回 false 且保持原状态。
 */
static bool sugov_iowait_reset(struct sugov_cpu *sg_cpu, u64 time,
			       bool set_iowait_boost)
{
	s64 delta_ns = time - sg_cpu->last_update;

	/* Reset boost only if a tick has elapsed since last request */
	if (delta_ns <= TICK_NSEC)
		return false;

	sg_cpu->iowait_boost = set_iowait_boost ? IOWAIT_BOOST_MIN : 0;
	sg_cpu->iowait_boost_pending = set_iowait_boost;

	return true;
}

/**
 * sugov_iowait_boost() - Updates the IO boost status of a CPU.
 * @sg_cpu: the sugov data for the CPU to boost
 * @time: the update time from the caller
 * @flags: SCHED_CPUFREQ_IOWAIT if the task is waking up after an IO wait
 *
 * Each time a task wakes up after an IO operation, the CPU utilization can be
 * boosted to a certain utilization which doubles at each "frequent and
 * successive" wakeup from IO, ranging from IOWAIT_BOOST_MIN to the utilization
 * of the maximum OPP.
 *
 * To keep doubling, an IO boost has to be requested at least once per tick,
 * otherwise we restart from the utilization of the minimum OPP.
 */
/*
 * 记录一次 I/O 唤醒：超时序列从最小值重启；非 IOWAIT 不新增 boost；同一次尚未消费
 * 的 pending 不重复翻倍。连续每 tick 至少一次请求使 boost 指数增长并钳到满容量。
 */
static void sugov_iowait_boost(struct sugov_cpu *sg_cpu, u64 time,
			       unsigned int flags)
{
	bool set_iowait_boost = flags & SCHED_CPUFREQ_IOWAIT;

	/* Reset boost if the CPU appears to have been idle enough */
	if (sg_cpu->iowait_boost &&
	    sugov_iowait_reset(sg_cpu, time, set_iowait_boost))
		return;

	/* Boost only tasks waking up after IO */
	if (!set_iowait_boost)
		return;

	/* Ensure boost doubles only one time at each request */
	if (sg_cpu->iowait_boost_pending)
		return;
	sg_cpu->iowait_boost_pending = true;

	/* Double the boost at each request */
	if (sg_cpu->iowait_boost) {
		sg_cpu->iowait_boost =
			min_t(unsigned int, sg_cpu->iowait_boost << 1, SCHED_CAPACITY_SCALE);
		return;
	}

	/* First wakeup after IO: start with minimum boost */
	sg_cpu->iowait_boost = IOWAIT_BOOST_MIN;
}

/**
 * sugov_iowait_apply() - Apply the IO boost to a CPU.
 * @sg_cpu: the sugov data for the cpu to boost
 * @time: the update time from the caller
 * @max_cap: the max CPU capacity
 *
 * A CPU running a task which woken up after an IO operation can have its
 * utilization boosted to speed up the completion of those IO operations.
 * The IO boost value is increased each time a task wakes up from IO, in
 * sugov_iowait_apply(), and it's instead decreased by this function,
 * each time an increase has not been requested (!iowait_boost_pending).
 *
 * A CPU which also appears to have been idle for at least one tick has also
 * its IO boost utilization reset.
 *
 * This mechanism is designed to boost high frequently IO waiting tasks, while
 * being more conservative on tasks which does sporadic IO operations.
 */
/*
 * 消费并衰减 IO boost：没有 boost 或闲置超一 tick 返回 0；本周期没有新请求则减半，
 * 低于最小值归零；有新请求仅清 pending。最后把 SCHED_CAPACITY_SCALE boost 换算到当前
 * CPU max_cap 尺度，供实际 util 取最大值。
 */
static unsigned long sugov_iowait_apply(struct sugov_cpu *sg_cpu, u64 time,
			       unsigned long max_cap)
{
	/* No boost currently required */
	if (!sg_cpu->iowait_boost)
		return 0;

	/* Reset boost if the CPU appears to have been idle enough */
	if (sugov_iowait_reset(sg_cpu, time, false))
		return 0;

	if (!sg_cpu->iowait_boost_pending) {
		/*
		 * No boost pending; reduce the boost value.
		 */
		sg_cpu->iowait_boost >>= 1;
		if (sg_cpu->iowait_boost < IOWAIT_BOOST_MIN) {
			sg_cpu->iowait_boost = 0;
			return 0;
		}
	}

	sg_cpu->iowait_boost_pending = false;

	/*
	 * sg_cpu->util is already in capacity scale; convert iowait_boost
	 * into the same scale so we can compare.
	 */
	return (sg_cpu->iowait_boost * max_cap) >> SCHED_CAPACITY_SHIFT;
}

#ifdef CONFIG_NO_HZ_COMMON
/*
 * 单 CPU fair policy 若自上次观察没有进入 idle，暂缓降频以避免忙任务被过早降档；SCX
 * 全接管时遵循 BPF 目标，uclamp_max 已限制时也必须更新。每次保存最新 idle_calls 快照。
 */
static bool sugov_hold_freq(struct sugov_cpu *sg_cpu)
{
	unsigned long idle_calls;
	bool ret;

	/*
	 * The heuristics in this function is for the fair class. For SCX, the
	 * performance target comes directly from the BPF scheduler. Let's just
	 * follow it.
	 */
	/* SCX 性能目标由 BPF 明确给出，本启发式不得覆盖它。 */
	if (scx_switched_all())
		return false;

	/* if capped by uclamp_max, always update to be in compliance */
	/* 被 uclamp_max 限制时即使未 idle 也必须允许降频，才能兑现容量上限。 */
	if (uclamp_rq_is_capped(cpu_rq(sg_cpu->cpu)))
		return false;

	/*
	 * Maintain the frequency if the CPU has not been idle recently, as
	 * reduction is likely to be premature.
	 */
	/* idle 调用计数未增长表示 CPU 最近持续忙，维持当前频率一轮。 */
	idle_calls = tick_nohz_get_idle_calls_cpu(sg_cpu->cpu);
	ret = idle_calls == sg_cpu->saved_idle_calls;

	sg_cpu->saved_idle_calls = idle_calls;
	return ret;
}
#else /* !CONFIG_NO_HZ_COMMON: */
/* 无 NO_HZ idle 计数时无法应用该启发式，恒不保持频率。 */
static inline bool sugov_hold_freq(struct sugov_cpu *sg_cpu) { return false; }
#endif /* !CONFIG_NO_HZ_COMMON */

/*
 * Make sugov_should_update_freq() ignore the rate limit when DL
 * has increased the utilization.
 */
/*
 * 若当前 DL 带宽超过上次 effective util 保存的 bw_min，强制下一次跳过 rate limit，
 * 防止新 deadline 需求等待整个限速窗口。
 */
static inline void ignore_dl_rate_limit(struct sugov_cpu *sg_cpu)
{
	if (cpu_bw_dl(cpu_rq(sg_cpu->cpu)) > sg_cpu->bw_min)
		sg_cpu->sg_policy->need_freq_update = true;
}

/*
 * 单 CPU 两条更新路径的公共前半段：记录/更新时间 IO boost，检查 DL 强制更新与 policy
 * 限速；允许更新后消费 boost 并重算有效 util。返回 false 表示本次无需触碰驱动。
 */
static inline bool sugov_update_single_common(struct sugov_cpu *sg_cpu,
					      u64 time, unsigned long max_cap,
					      unsigned int flags)
{
	unsigned long boost;

	sugov_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	ignore_dl_rate_limit(sg_cpu);

	if (!sugov_should_update_freq(sg_cpu->sg_policy, time))
		return false;

	boost = sugov_iowait_apply(sg_cpu, time, max_cap);
	sugov_get_util(sg_cpu, boost);

	return true;
}

/*
 * 单 CPU 频率路径：由 util 求驱动频点；NO_HZ 忙碌启发式只阻止非强制降频，并恢复 raw
 * cache 以保持 cache/next 一致。候选变化后，fast switch 在 rq 锁内直接提交；慢路径
 * 在 update_lock 下合并为 irq_work+kthread 请求。
 */
static void sugov_update_single_freq(struct update_util_data *hook, u64 time,
				     unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned int cached_freq = sg_policy->cached_raw_freq;
	unsigned long max_cap;
	unsigned int next_f;

	max_cap = arch_scale_cpu_capacity(sg_cpu->cpu);

	if (!sugov_update_single_common(sg_cpu, time, max_cap, flags))
		return;

	next_f = get_next_freq(sg_policy, sg_cpu->util, max_cap);

	if (sugov_hold_freq(sg_cpu) && next_f < sg_policy->next_freq &&
	    !sg_policy->need_freq_update) {
		next_f = sg_policy->next_freq;

		/* Restore cached freq as next_freq has changed */
		sg_policy->cached_raw_freq = cached_freq;
	}

	if (!sugov_update_next_freq(sg_policy, time, next_f))
		return;

	/*
	 * This code runs under rq->lock for the target CPU, so it won't run
	 * concurrently on two different CPUs for the same target and it is not
	 * necessary to acquire the lock in the fast switch case.
	 */
	/* 单 CPU policy 的目标 rq 锁已串行该回调，fast switch 无需 policy 额外锁。 */
	if (sg_policy->policy->fast_switch_enabled) {
		cpufreq_driver_fast_switch(sg_policy->policy, next_f);
	} else {
		raw_spin_lock(&sg_policy->update_lock);
		sugov_deferred_update(sg_policy);
		raw_spin_unlock(&sg_policy->update_lock);
	}
}

/*
 * 单 CPU adjust_perf 路径直接把 min/util/max 性能级交给驱动；只有频率不变性成立时
 * util 与性能级才可直接对应，否则退回频率解析路径。hold 启发式只阻止 util 下降，
 * 提交后清强制标志并更新时间戳。
 */
static void sugov_update_single_perf(struct update_util_data *hook, u64 time,
				     unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long prev_util = sg_cpu->util;
	unsigned long max_cap;

	/*
	 * Fall back to the "frequency" path if frequency invariance is not
	 * supported, because the direct mapping between the utilization and
	 * the performance levels depends on the frequency invariance.
	 */
	/* 原文结论：无频率不变性时同一 util 数值随当前频点而变，不能直接当性能级。 */
	if (!arch_scale_freq_invariant()) {
		sugov_update_single_freq(hook, time, flags);
		return;
	}

	max_cap = arch_scale_cpu_capacity(sg_cpu->cpu);

	if (!sugov_update_single_common(sg_cpu, time, max_cap, flags))
		return;

	if (sugov_hold_freq(sg_cpu) && sg_cpu->util < prev_util)
		sg_cpu->util = prev_util;

	cpufreq_driver_adjust_perf(sg_policy->policy, sg_cpu->bw_min,
				   sg_cpu->util, max_cap);

	sg_policy->need_freq_update = false;
	sg_policy->last_freq_update_time = time;
}

/*
 * 共享 policy 对每个成员 CPU 消费各自 IO boost、重算 util，并取最大需求驱动共同频点；
 * policy 内 CPU 容量尺度以触发 CPU 的 max_cap 为归一基准，调用者持 update_lock。
 */
static unsigned int sugov_next_freq_shared(struct sugov_cpu *sg_cpu, u64 time)
{
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util = 0, max_cap;
	unsigned int j;

	max_cap = arch_scale_cpu_capacity(sg_cpu->cpu);

	for_each_cpu(j, policy->cpus) {
		struct sugov_cpu *j_sg_cpu = &per_cpu(sugov_cpu, j);
		unsigned long boost;

		boost = sugov_iowait_apply(j_sg_cpu, time, max_cap);
		sugov_get_util(j_sg_cpu, boost);

		util = max(j_sg_cpu->util, util);
	}

	return get_next_freq(sg_policy, util, max_cap);
}

/*
 * 共享 policy 的每 CPU 回调全部在 update_lock 下串行：先更新触发 CPU 状态，再检查限速，
 * 聚合所有成员最大 util、去重频点，最后 fast switch 或合并慢工作。锁也保护其他 CPU
 * 的 sugov_cpu 字段在本次聚合中的一致访问。
 */
static void
sugov_update_shared(struct update_util_data *hook, u64 time, unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned int next_f;

	raw_spin_lock(&sg_policy->update_lock);

	sugov_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	ignore_dl_rate_limit(sg_cpu);

	if (sugov_should_update_freq(sg_policy, time)) {
		next_f = sugov_next_freq_shared(sg_cpu, time);

		if (!sugov_update_next_freq(sg_policy, time, next_f))
			goto unlock;

		if (sg_policy->policy->fast_switch_enabled)
			cpufreq_driver_fast_switch(sg_policy->policy, next_f);
		else
			sugov_deferred_update(sg_policy);
	}
unlock:
	raw_spin_unlock(&sg_policy->update_lock);
}

/*
 * 慢路径 kthread 在 update_lock 下快照最新 next_freq 并先清 work_in_progress；这样锁后
 * 到达的新请求能再次排 work，不会丢失。随后 work_lock 串行可睡眠 target 调用，并以
 * RELATION_L 选择不低于请求值的频点。
 */
static void sugov_work(struct kthread_work *work)
{
	struct sugov_policy *sg_policy = container_of(work, struct sugov_policy, work);
	unsigned int freq;
	unsigned long flags;

	/*
	 * Hold sg_policy->update_lock shortly to handle the case where:
	 * in case sg_policy->next_freq is read here, and then updated by
	 * sugov_deferred_update() just before work_in_progress is set to false
	 * here, we may miss queueing the new update.
	 *
	 * Note: If a work was queued after the update_lock is released,
	 * sugov_work() will just be called again by kthread_work code; and the
	 * request will be proceed before the sugov thread sleeps.
	 */
	/* 原文竞态：必须在同一锁区间读取 next 与清 pending，否则夹入的更新可能无人再排队。 */
	raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
	freq = sg_policy->next_freq;
	sg_policy->work_in_progress = false;
	raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);

	mutex_lock(&sg_policy->work_lock);
	__cpufreq_driver_target(sg_policy->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&sg_policy->work_lock);
}

/* irq_work 仅把硬中断/rq 锁上下文桥接到可睡眠 kthread worker；实际频率切换不在此执行。 */
static void sugov_irq_work(struct irq_work *irq_work)
{
	struct sugov_policy *sg_policy;

	sg_policy = container_of(irq_work, struct sugov_policy, irq_work);

	kthread_queue_work(&sg_policy->worker, &sg_policy->work);
}

/************************** sysfs interface ************************/

static struct sugov_tunables *global_tunables;
/* 串行 global/per-policy tunables 的创建、引用挂接、清除及 policy governor_data 发布。 */
static DEFINE_MUTEX(global_tunables_lock);

/* 从内嵌 gov_attr_set 还原拥有它的 schedutil tunables。 */
static inline struct sugov_tunables *to_sugov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct sugov_tunables, attr_set);
}

/* sysfs 读取当前微秒限速值，以文本和换行返回。 */
static ssize_t rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->rate_limit_us);
}

/*
 * 解析十进制微秒值，失败返回 -EINVAL；成功更新共享 tunables，并遍历挂接 policy 把
 * 纳秒延迟同步过去，返回原输入字节数。governor sysfs 层负责属性集合串行化。
 */
static ssize_t
rate_limit_us_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook)
		sg_policy->freq_update_delay_ns = rate_limit_us * NSEC_PER_USEC;

	return count;
}

static struct governor_attr rate_limit_us = __ATTR_RW(rate_limit_us);

static struct attribute *sugov_attrs[] = {
	&rate_limit_us.attr,
	NULL
};
ATTRIBUTE_GROUPS(sugov);

/* kobject 最后一份引用释放时回收包含它的 tunables；attr_set 成员已由 kobject 核心清理。 */
static void sugov_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = to_gov_attr_set(kobj);

	kfree(to_sugov_tunables(attr_set));
}

static const struct kobj_type sugov_tunables_ktype = {
	.default_groups = sugov_groups,
	.sysfs_ops = &governor_sysfs_ops,
	.release = &sugov_tunables_free,
};

/********************** cpufreq governor interface *********************/

static struct cpufreq_governor schedutil_gov;

/* 分配零初始化 policy 状态并初始化 update_lock；失败返回 NULL，不修改 cpufreq policy。 */
static struct sugov_policy *sugov_policy_alloc(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;

	sg_policy = kzalloc_obj(*sg_policy);
	if (!sg_policy)
		return NULL;

	sg_policy->policy = policy;
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

/* 回收已停止、已摘除 tunables 引用的 policy 私有状态。 */
static void sugov_policy_free(struct sugov_policy *sg_policy)
{
	kfree(sg_policy);
}

/*
 * 慢切换 policy 创建专用 kthread worker；fast switch 快速返回。线程用带 SUGOV 标志的
 * SCHED_DEADLINE 属性解决 PI 优先级问题，带宽参数为未实际消费的占位值。创建/设属性
 * 失败返回错误并停止已建线程；成功按驱动跨 CPU 能力设置 affinity，初始化 work/irq/
 * mutex 并唤醒线程。
 */
static int sugov_kthread_create(struct sugov_policy *sg_policy)
{
	struct task_struct *thread;
	struct sched_attr attr = {
		.size		= sizeof(struct sched_attr),
		.sched_policy	= SCHED_DEADLINE,
		.sched_flags	= SCHED_FLAG_SUGOV,
		.sched_nice	= 0,
		.sched_priority	= 0,
		/*
		 * Fake (unused) bandwidth; workaround to "fix"
		 * priority inheritance.
		 */
		/* 虚构且不使用的 DL 带宽只为让 PI 路径正确处理该特殊 worker。 */
		.sched_runtime	= NSEC_PER_MSEC,
		.sched_deadline = 10 * NSEC_PER_MSEC,
		.sched_period	= 10 * NSEC_PER_MSEC,
	};
	struct cpufreq_policy *policy = sg_policy->policy;
	int ret;

	/* kthread only required for slow path */
	/* fast switch 在 rq 回调中完成，不创建任何异步执行资源。 */
	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&sg_policy->work, sugov_work);
	kthread_init_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"sugov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create sugov thread: %pe\n", thread);
		return PTR_ERR(thread);
	}

	ret = sched_setattr_nocheck(thread, &attr);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_DEADLINE\n", __func__);
		return ret;
	}

	sg_policy->thread = thread;
	if (policy->dvfs_possible_from_any_cpu)
		set_cpus_allowed_ptr(thread, policy->related_cpus);
	else
		kthread_bind_mask(thread, policy->related_cpus);

	init_irq_work(&sg_policy->irq_work, sugov_irq_work);
	mutex_init(&sg_policy->work_lock);

	wake_up_process(thread);

	return 0;
}

/* 慢路径退出先冲刷全部 worker 请求，再停止线程并销毁 mutex；fast policy 无资源可清。 */
static void sugov_kthread_stop(struct sugov_policy *sg_policy)
{
	/* kthread only required for slow path */
	if (sg_policy->policy->fast_switch_enabled)
		return;

	kthread_flush_worker(&sg_policy->worker);
	kthread_stop(sg_policy->thread);
	mutex_destroy(&sg_policy->work_lock);
}

/*
 * 分配并初始化 tunables 属性集合，首个非 per-policy governor 同时发布为全局共享对象；
 * 分配失败返回 NULL，实际释放由 kobject release 回调完成。
 */
static struct sugov_tunables *sugov_tunables_alloc(struct sugov_policy *sg_policy)
{
	struct sugov_tunables *tunables;

	tunables = kzalloc_obj(*tunables);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

/* 仅在使用全局 tunables 模式时清发布指针；对象本身由 attr_set/kobject 引用计数释放。 */
static void sugov_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;
}

/*
 * governor INIT 状态为 policy 建立完整资源：拒绝已有 governor_data，先尝试启用 fast
 * switch，再分配 policy/可选 kthread；在全局锁下复用共享 tunables 或创建 kobject。
 * 任一失败按 kobject→全局发布→线程→policy→fast-switch 的逆序回滚并返回 errno；成功
 * 发布 governor_data、初始化默认 rate limit，并因 EAS 偏好变化重建 sched domains。
 */
static int sugov_init(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;
	struct sugov_tunables *tunables;
	int ret = 0;

	/* State should be equivalent to EXIT */
	/* 非 NULL 表示生命周期状态不在 EXIT，重复 INIT 返回 -EBUSY，避免覆盖所有权。 */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	sg_policy = sugov_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = sugov_kthread_create(sg_policy);
	if (ret)
		goto free_sg_policy;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		/* 全局模式为新 policy 增加 attr_set 引用；per-policy 模式出现全局对象是内部错误。 */
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}

	tunables = sugov_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	tunables->rate_limit_us = cpufreq_policy_transition_delay_us(policy);

	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &sugov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   schedutil_gov.name);
	if (ret)
		goto fail;

out:
	/*
	 * Schedutil is the preferred governor for EAS, so rebuild sched domains
	 * on governor changes to make sure the scheduler knows about them.
	 */
	/* schedutil 是 EAS 的优选 governor，切换后需让调度域重新评估能耗拓扑。 */
	em_rebuild_sched_domains();
	mutex_unlock(&global_tunables_lock);
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	sugov_clear_global_tunables();

stop_kthread:
	sugov_kthread_stop(sg_policy);
	mutex_unlock(&global_tunables_lock);

free_sg_policy:
	sugov_policy_free(sg_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

/*
 * governor EXIT 在全局锁下摘除 policy 的 tunables 引用并清 governor_data；最后引用时
 * 清全局发布。锁外停止异步线程、释放 policy、关闭 fast switch，最后重建调度域。
 */
static void sugov_exit(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count)
		sugov_clear_global_tunables();

	mutex_unlock(&global_tunables_lock);

	sugov_kthread_stop(sg_policy);
	sugov_policy_free(sg_policy);
	cpufreq_disable_fast_switch(policy);

	em_rebuild_sched_domains();
}

/*
 * governor START 重置动态状态，选择共享、adjust_perf 或频率回调，并为 policy 中每个
 * CPU 清零/绑定 sugov_cpu 后以 RCU update-util hook 发布。驱动 NEED_UPDATE_LIMITS 决定
 * 首次是否必须提交相同频率；成功恒返回 0，资源已由 INIT 准备。
 */
static int sugov_start(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	void (*uu)(struct update_util_data *data, u64 time, unsigned int flags);
	unsigned int cpu;

	sg_policy->freq_update_delay_ns	= sg_policy->tunables->rate_limit_us * NSEC_PER_USEC;
	sg_policy->last_freq_update_time	= 0;
	sg_policy->next_freq			= 0;
	sg_policy->work_in_progress		= false;
	sg_policy->limits_changed		= false;
	sg_policy->cached_raw_freq		= 0;

	sg_policy->need_freq_update = cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS);

	if (policy_is_shared(policy))
		uu = sugov_update_shared;
	else if (policy->fast_switch_enabled && cpufreq_driver_has_adjust_perf())
		uu = sugov_update_single_perf;
	else
		uu = sugov_update_single_freq;

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);

		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->cpu = cpu;
		sg_cpu->sg_policy = sg_policy;
		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util, uu);
	}
	return 0;
}

/*
 * governor STOP 先从每 CPU 槽摘除 hook，再 synchronize_rcu 等所有正在执行的回调退出；
 * 慢路径还同步 irq_work 并取消 kthread work，保证 STOP 返回后不再访问 policy 动态状态。
 */
static void sugov_stop(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_rcu();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&sg_policy->irq_work);
		kthread_cancel_work_sync(&sg_policy->work);
	}
}

/*
 * policy min/max 改变时，慢驱动先在 work_lock 下立即应用硬限制；随后写屏障保证 limits
 * 字段更新与 limits_changed 发布次序，与更新回调完整屏障配对，令下一次采样强制重算。
 */
static void sugov_limits(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg_policy->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg_policy->work_lock);
	}

	/*
	 * The limits_changed update below must take place before the updates
	 * of policy limits in cpufreq_set_policy() or a policy limits update
	 * might be missed, so use a memory barrier to ensure it.
	 *
	 * This pairs with the memory barrier in sugov_should_update_freq().
	 */
	/* 原文结论：标志发布必须与 policy limits 写入有序，否则回调可能清标志却读到旧限制。 */
	smp_wmb();

	WRITE_ONCE(sg_policy->limits_changed, true);
}

static struct cpufreq_governor schedutil_gov = {
	.name			= "schedutil",
	.owner			= THIS_MODULE,
	.flags			= CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init			= sugov_init,
	.exit			= sugov_exit,
	.start			= sugov_start,
	.stop			= sugov_stop,
	.limits			= sugov_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
/* 配置选择 schedutil 为默认 governor 时返回其静态描述符，所有权仍归本模块。 */
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &schedutil_gov;
}
#endif

/* 以描述符地址判断 policy 当前 governor 是否正是 schedutil；只读瞬时状态。 */
bool sugov_is_governor(struct cpufreq_policy *policy)
{
	return policy->governor == &schedutil_gov;
}

cpufreq_governor_init(schedutil_gov);
