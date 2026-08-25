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

	/* tunables_hook 把本 policy 挂入共享属性集合，集合只借用该链表节点。 */
	struct sugov_tunables	*tunables;
	struct list_head	tunables_hook;

	/* 以下缓存都由更新协议串行，时间单位分别为 ns，频率单位为 kHz。 */
	raw_spinlock_t		update_lock;
	u64			last_freq_update_time;
	s64			freq_update_delay_ns;
	unsigned int		next_freq;
	unsigned int		cached_raw_freq;

	/* slow-switch 资源从 INIT 创建到 EXIT 销毁，STOP 只排空未完成请求。 */
	/* The next fields are only needed if fast switch cannot be used: */
	struct			irq_work irq_work;
	struct			kthread_work work;
	struct			mutex work_lock;
	struct			kthread_worker worker;
	struct task_struct	*thread;
	bool			work_in_progress;

	/* 两个布尔量把控制路径的 limits 变化传递给下一次调度器采样。 */
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

	/* pending 限制每轮只增长一次，boost 使用 SCHED_CAPACITY_SCALE 尺度。 */
	bool			iowait_boost_pending;
	unsigned int		iowait_boost;
	u64			last_update;

	unsigned long		util;
	unsigned long		bw_min;

	/* saved_idle_calls 只服务单 CPU policy 的忙碌保持判断。 */
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
/*
 * 业务背景：scheduler 更新回调频繁触发，提交 DVFS 前要同时过滤无执行资格的 CPU、限速窗口和强制重算事件。
 * 入参：sg_policy 是不可空、借用且可写的活动 governor policy；time 是同一 rq 时钟域的 ns 时间戳。
 * 出参/返回：本次应继续计算/提交返回 true，否则 false；可能消费 limits_changed 并置 need_freq_update。
 * 注意事项：调用者持目标 rq 锁，共享 policy 还持 update_lock；完整屏障与 sugov_limits 的 wmb 配对且不可睡眠。
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
/*
 * 业务背景：频率解析后还要去重相同目标并原子推进限速时间基线，避免无意义驱动调用。
 * 入参：sg_policy 是不可空借用可写 policy；time 是本次 ns 更新时刻；next_freq 是驱动已解析的 kHz 目标。
 * 出参/返回：接受新提交返回 true 并更新 next/time；可安全跳过返回 false；不直接调用驱动或转移 ownership。
 * 注意事项：调用者持相应 rq/update_lock；need_freq_update 被本函数消费，驱动 NEED_UPDATE_LIMITS 可强制同频提交。
 */
static bool sugov_update_next_freq(struct sugov_policy *sg_policy, u64 time,
				   unsigned int next_freq)
{
	if (sg_policy->need_freq_update) {
		sg_policy->need_freq_update = false;
		/* 强制请求仍可在驱动无需 limits 通知且解析频率未变时安全去重。 */
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

	/* 只有接受提交才推进时间基线，拒绝路径不能延后下一次合法更新。 */
	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;

	return true;
}

/* 慢切换只允许一个待处理 work；update_lock 下设置标志并排 irq_work，后续请求只改 next。 */
/*
 * 业务背景：不能在 rq 锁内睡眠的慢驱动需把最新频率请求合并后经 irq_work 转交 governor kthread。
 * 入参：sg_policy 是不可空、借用且可写的慢切换 policy，异步 worker 资源已初始化。
 * 出参/返回：无直接返回值；首次置 work_in_progress 并排 irq_work，已有 work 时仅保留上游最新 next_freq。
 * 注意事项：调用者持 update_lock 且不可睡眠；标志到真正 work 的生命周期由 stop/flush 协议回收。
 */
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
/*
 * 业务背景：把容量尺度 util 映射到频率前必须选择与架构 capacity 标定一致的参考频率。
 * 入参：policy 是不可空、稳定的只读借用 cpufreq policy；其 cpu/cur/cpuinfo 字段已有效。
 * 出参/返回：返回 kHz 参考频率：架构 ref、invariant 最大值或当前频率 1.25 倍；无输出参数或副作用。
 * 注意事项：最后一种只是升频余量启发式，可能超过 policy max，后续 resolve 负责钳制；函数不睡眠。
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
/*
 * 业务背景：schedutil 要把调度容量需求转换成驱动可实现且满足 policy limits 的实际频点。
 * 入参：sg_policy 是不可空借用可写 policy；util 是当前容量需求；max 是同尺度非零 CPU 最大容量。
 * 出参/返回：返回满足驱动表/limits 的 kHz 频率，并更新 raw 缓存；缓存命中可直接复用 next_freq。
 * 注意事项：调用者已串行 policy 状态；max 不得为 0，resolve 可能读取变化中的 limits，屏障由上游保证。
 */
static unsigned int get_next_freq(struct sugov_policy *sg_policy,
				  unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq;

	freq = get_capacity_ref_freq(policy);
	freq = map_util_freq(util, freq, max);

	/* raw 请求相同可复用上次驱动解析结果，但强制 limits 更新必须重新解析。 */
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
/*
 * 业务背景：调频目标既要给实际负载留 DVFS 余量，又要遵守调度器计算的最小/最大性能边界。
 * 入参：cpu 是接口保留的纯输入 CPU 编号（当前不读）；actual/min/max 是同一容量尺度的非负值。
 * 出参/返回：返回 max(min,min(map_util_perf(actual),max))；无输出参数、状态副作用或 ownership 变化。
 * 注意事项：min 高于 max 时最小保证优先；函数不验证尺度、不解析频点且不可睡眠。
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
/*
 * 业务背景：单 CPU 与共享 policy 更新都需把 SCX/CFS、uclamp、DL 带宽和 IO boost 合成为一个容量目标。
 * 入参：sg_cpu 是不可空、借用且可写的 per-CPU governor 状态；boost 是同 CPU 容量尺度的非负下限。
 * 出参/返回：无直接返回值；更新 sg_cpu->bw_min/util，不修改频率或转移 ownership。
 * 注意事项：调用者持该 CPU rq 锁，共享 policy 还持 update_lock；读取调度统计且不可睡眠。
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
/*
 * 业务背景：零散 I/O 唤醒不应永久维持高频，超过一 tick 未更新时需重新开始或关闭 boost 序列。
 * 入参：sg_cpu 是不可空可写借用状态；time 是 ns rq 时间；set_iowait_boost 表示本次是否有 IOWAIT 请求。
 * 出参/返回：发生超时重置返回 true 并更新 boost/pending，未超时返回 false 且不改它们。
 * 注意事项：调用者串行 per-CPU 状态；time 与 last_update 同域，负/不超 TICK_NSEC 差值均视为未超时。
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
/*
 * 业务背景：频繁 I/O 唤醒的任务需要快速升频，而同一更新周期的重复通知不能多次翻倍。
 * 入参：sg_cpu 是不可空可写借用状态；time 是 ns rq 时间；flags 是 SCHED_CPUFREQ_* 位图。
 * 出参/返回：无直接返回值；按 IOWAIT 位重置、置 pending 或倍增 boost 至容量满刻度。
 * 注意事项：调用者持 rq/update 锁；无 IOWAIT 或已有 pending 可无副作用返回，函数不可睡眠。
 */
static void sugov_iowait_boost(struct sugov_cpu *sg_cpu, u64 time,
			       unsigned int flags)
{
	bool set_iowait_boost = flags & SCHED_CPUFREQ_IOWAIT;

	/* Reset boost if the CPU appears to have been idle enough */
	if (sg_cpu->iowait_boost &&
	    sugov_iowait_reset(sg_cpu, time, set_iowait_boost))
		return;

	/* 普通采样只能衰减已有状态，不能凭空创建 I/O boost。 */
	/* Boost only tasks waking up after IO */
	if (!set_iowait_boost)
		return;

	/* Ensure boost doubles only one time at each request */
	if (sg_cpu->iowait_boost_pending)
		return;
	sg_cpu->iowait_boost_pending = true;

	/* 连续 I/O 周期按倍增提升，首次则从最小 OPP 对应容量起步。 */
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
/*
 * 业务背景：计算频率时要把累积 I/O boost 消费成当前 CPU 容量下限，并在无新请求时逐轮衰减。
 * 入参：sg_cpu 是不可空可写借用状态；time 是 ns rq 时间；max_cap 是该 CPU 非零容量满刻度。
 * 出参/返回：返回换算后的容量 boost，禁用/超时/衰减到底返回 0；更新 boost/pending 状态。
 * 注意事项：调用者串行 sg_cpu；结果仅是 util 下限而非频率，乘法范围由容量尺度约束且不可睡眠。
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

	/* 本周期未收到新 I/O 请求时衰减；低于最小有效值便彻底关闭。 */
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

	/* 消费 pending 后再把统一容量尺度换算到调用 CPU 的 max_cap。 */
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
/*
 * 业务背景：持续忙碌的单 CPU fair workload 可能在采样抖动中短暂降 util，NO_HZ idle 计数可阻止过早降频。
 * 入参：sg_cpu 是不可空、借用且可写的单 CPU policy 状态，cpu/saved_idle_calls 已初始化。
 * 出参/返回：建议维持当前频率返回 true，否则 false；每次更新 saved_idle_calls，无 ownership 变化。
 * 注意事项：仅 CONFIG_NO_HZ_COMMON；SCX 全接管或 uclamp_max 限制时禁用启发式，调用者持 rq 锁。
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
/*
 * 业务背景：未构建 NO_HZ 计数时保留统一调用面，但没有可靠信号判断 CPU 最近是否持续忙碌。
 * 入参：sg_cpu 是未读取的借用 per-CPU 状态，可按公共调用契约视为不可空。
 * 出参/返回：恒返回 false，无输出参数、状态副作用或 ownership 变化。
 * 注意事项：这是 !CONFIG_NO_HZ_COMMON 编译期 stub，不得把 false 解释为 CPU 已进入 idle。
 */
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
/*
 * 业务背景：deadline 带宽增加是硬性能下限变化，不能等待普通 governor rate limit 才响应。
 * 入参：sg_cpu 是不可空、借用且可写的 per-CPU 状态，所属 rq/policy 必须仍活动。
 * 出参/返回：无直接返回值；DL 当前带宽超过已保存 bw_min 时置 policy->need_freq_update。
 * 注意事项：调用者持 rq 锁，共享 policy 还持 update_lock；只强制后续重算，不直接切频且不可睡眠。
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
/*
 * 业务背景：single freq 与 adjust_perf 两条路径共享 IO/DL/限速/util 采样协议，必须保持同一状态推进顺序。
 * 入参：sg_cpu 是不可空可写借用状态；time 是 ns rq 时间；max_cap 是非零容量；flags 是更新原因位图。
 * 出参/返回：完成有效 util 重算返回 true；无资格/限速返回 false；更新 last_update、boost、bw_min/util。
 * 注意事项：调用者持目标 rq 锁且不可睡眠；false 仍可能记录 IO boost 和 last_update，不等于无副作用。
 */
static inline bool sugov_update_single_common(struct sugov_cpu *sg_cpu,
					      u64 time, unsigned long max_cap,
					      unsigned int flags)
{
	unsigned long boost;

	/* 先记录 I/O 事件和时间，即使后续限速拒绝，本轮状态也不能丢。 */
	sugov_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	ignore_dl_rate_limit(sg_cpu);

	if (!sugov_should_update_freq(sg_cpu->sg_policy, time))
		return false;

	/* 获准更新后才消费 boost，并把其作为本轮 util 的下限。 */
	boost = sugov_iowait_apply(sg_cpu, time, max_cap);
	sugov_get_util(sg_cpu, boost);

	return true;
}

/*
 * 单 CPU 频率路径：由 util 求驱动频点；NO_HZ 忙碌启发式只阻止非强制降频，并恢复 raw
 * cache 以保持 cache/next 一致。候选变化后，fast switch 在 rq 锁内直接提交；慢路径
 * 在 update_lock 下合并为 irq_work+kthread 请求。
 */
/*
 * 业务背景：不支持 adjust_perf 或频率不变性的单 CPU policy 要把调度 util 转成离散频点并提交驱动。
 * 入参：hook 是不可空、内嵌于 sugov_cpu 的借用回调对象；time 是 ns rq 时间；flags 是 SCHED_CPUFREQ_* 位图。
 * 出参/返回：无直接返回值；可能更新 per-CPU/policy 缓存并 fast switch，或排队一个合并慢请求。
 * 注意事项：在目标 rq 锁内不可睡眠；slow 分支用 update_lock，hold 取消降频时必须恢复 raw cache。
 */
static void sugov_update_single_freq(struct update_util_data *hook, u64 time,
				     unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned int cached_freq = sg_policy->cached_raw_freq;
	unsigned long max_cap;
	unsigned int next_f;

	/* 保存 raw cache，供忙碌保持撤销本轮降频计算时恢复一致性。 */
	max_cap = arch_scale_cpu_capacity(sg_cpu->cpu);

	if (!sugov_update_single_common(sg_cpu, time, max_cap, flags))
		return;

	next_f = get_next_freq(sg_policy, sg_cpu->util, max_cap);

	/* NO_HZ 忙碌保持只拦截非强制降频，上调和 limits 重算照常进行。 */
	if (sugov_hold_freq(sg_cpu) && next_f < sg_policy->next_freq &&
	    !sg_policy->need_freq_update) {
		next_f = sg_policy->next_freq;

		/* Restore cached freq as next_freq has changed */
		sg_policy->cached_raw_freq = cached_freq;
	}

	if (!sugov_update_next_freq(sg_policy, time, next_f))
		return;

	/* 到这里 policy 状态已接受目标，再按驱动能力选择同步或异步提交。 */
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
/*
 * 业务背景：支持 adjust_perf 的驱动可直接消费调度容量级，避免先解析离散频点的开销。
 * 入参：hook 是不可空、内嵌 sugov_cpu 的借用对象；time 是 ns rq 时间；flags 是更新原因位图。
 * 出参/返回：无直接返回值；可能调用 adjust_perf 并更新 util、need 标志和最后提交时间。
 * 注意事项：仅频率不变性成立才直映，否则转 single_freq；在 rq 锁内不可睡眠，hold 只阻止性能下降。
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

	/* 忙碌保持不能降低性能请求，但允许新的带宽下限或 util 推高它。 */
	if (sugov_hold_freq(sg_cpu) && sg_cpu->util < prev_util)
		sg_cpu->util = prev_util;

	cpufreq_driver_adjust_perf(sg_policy->policy, sg_cpu->bw_min,
				   sg_cpu->util, max_cap);

	/* adjust_perf 已直接提交，本路径自行消费强制标志并推进限速时间。 */
	sg_policy->need_freq_update = false;
	sg_policy->last_freq_update_time = time;
}

/*
 * 共享 policy 对每个成员 CPU 消费各自 IO boost、重算 util，并取最大需求驱动共同频点；
 * policy 内 CPU 容量尺度以触发 CPU 的 max_cap 为归一基准，调用者持 update_lock。
 */
/*
 * 业务背景：多个 CPU 共用一个硬件频点时，policy 必须满足成员中的最大性能需求而不能只看触发 CPU。
 * 入参：sg_cpu 是触发 CPU 的不可空借用状态；time 是 ns rq 时间，policy 成员状态由 update_lock 稳定。
 * 出参/返回：返回驱动已解析的共享 kHz 频点；更新每个成员的 boost/util/bw_min 及 policy raw 缓存。
 * 注意事项：调用者持 sg_policy->update_lock；max_cap 取触发 CPU 尺度，成员容量值由 util 计算归一化。
 */
static unsigned int sugov_next_freq_shared(struct sugov_cpu *sg_cpu, u64 time)
{
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util = 0, max_cap;
	unsigned int j;

	/* 共享频点统一使用触发 CPU 的容量满刻度比较各成员有效需求。 */
	max_cap = arch_scale_cpu_capacity(sg_cpu->cpu);

	for_each_cpu(j, policy->cpus) {
		struct sugov_cpu *j_sg_cpu = &per_cpu(sugov_cpu, j);
		unsigned long boost;

		boost = sugov_iowait_apply(j_sg_cpu, time, max_cap);
		sugov_get_util(j_sg_cpu, boost);

		/* 共同硬件频点必须覆盖成员最大值，不能做平均而压低繁忙 CPU。 */
		util = max(j_sg_cpu->util, util);
	}

	return get_next_freq(sg_policy, util, max_cap);
}

/*
 * 共享 policy 的每 CPU 回调全部在 update_lock 下串行：先更新触发 CPU 状态，再检查限速，
 * 聚合所有成员最大 util、去重频点，最后 fast switch 或合并慢工作。锁也保护其他 CPU
 * 的 sugov_cpu 字段在本次聚合中的一致访问。
 */
/*
 * 业务背景：共享 policy 可由任一成员 rq 回调触发，需要一把 raw lock 串行聚合与共同频率提交。
 * 入参：hook 是触发 CPU 内嵌 update_util 的不可空借用对象；time 是 ns rq 时间；flags 是原因位图。
 * 出参/返回：无直接返回值；可能更新成员状态并 fast switch 或排慢 work，限速/同频时只更新采样状态。
 * 注意事项：入口持触发 rq 锁，内部 update_lock 不可睡眠；unlock 标签覆盖去重提前退出。
 */
static void
sugov_update_shared(struct update_util_data *hook, u64 time, unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned int next_f;

	/* update_lock 同时串行不同成员 rq 的采样与 policy 提交缓存。 */
	raw_spin_lock(&sg_policy->update_lock);

	sugov_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	ignore_dl_rate_limit(sg_cpu);

	/* 未过限速仍保留刚记录的触发 CPU I/O/DL 状态，然后统一解锁。 */
	if (sugov_should_update_freq(sg_policy, time)) {
		next_f = sugov_next_freq_shared(sg_cpu, time);

		if (!sugov_update_next_freq(sg_policy, time, next_f))
			goto unlock;

		/* 已接受的共享目标按 fast/slow 能力二选一提交。 */
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
/*
 * 业务背景：慢切换驱动可能睡眠，governor kthread 需消费合并后的最新目标并允许并发请求再次排队。
 * 入参：work 是不可空、内嵌于 sugov_policy 的借用 kthread_work，policy/thread 生命周期由 STOP/EXIT 稳定。
 * 出参/返回：无直接返回值；清 pending、快照 kHz 目标并在 work_lock 下调用可睡眠 driver target。
 * 注意事项：先用 irq-safe update_lock 闭合排队竞态，再用 mutex 串行驱动；driver 错误不通过本回调返回。
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
/*
 * 业务背景：rq 锁内不能直接唤醒/执行慢驱动路径，irq_work 负责把请求安全桥接到 policy worker。
 * 入参：irq_work 是不可空、内嵌于活动 sugov_policy 的借用对象。
 * 出参/返回：无直接返回值；向 worker 排入其 kthread_work，不修改目标频率或转移 ownership。
 * 注意事项：运行于 irq_work 原子上下文且不可睡眠；STOP 必须 irq_work_sync 后才能释放 policy。
 */
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
/*
 * 业务背景：通用 governor sysfs 回调只拿到 attr_set，需要恢复 schedutil 私有 rate-limit 容器。
 * 入参：attr_set 是不可空、内嵌于存活 sugov_tunables 的借用指针。
 * 出参/返回：返回拥有该成员的非空借用 tunables 指针；无输出参数、引用获取或状态副作用。
 * 注意事项：传入非该容器成员会产生错误地址；调用者依赖 kobject 引用稳定生命周期，函数不睡眠。
 */
static inline struct sugov_tunables *to_sugov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct sugov_tunables, attr_set);
}

/* sysfs 读取当前微秒限速值，以文本和换行返回。 */
/*
 * 业务背景：用户态需要通过 governor sysfs 查看当前 policy/全局最小频率更新间隔。
 * 入参：attr_set 是不可空借用属性集合；buf 是 sysfs 提供、容量足够的可写输出缓冲区。
 * 出参/返回：返回写入字节数（不含终止 NUL），buf 得到十进制微秒值和换行；ownership 不变。
 * 注意事项：sysfs/kobject 层稳定 tunables 并串行属性访问；sprintf 本路径值为 unsigned int，不会越页。
 */
static ssize_t rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->rate_limit_us);
}

/*
 * 解析十进制微秒值，失败返回 -EINVAL；成功更新共享 tunables，并遍历挂接 policy 把
 * 纳秒延迟同步过去，返回原输入字节数。governor sysfs 层负责属性集合串行化。
 */
/*
 * 业务背景：修改 rate_limit_us 要同时更新共享 attr_set 下每个 policy 的 ns 热路径缓存。
 * 入参：attr_set 是不可空借用集合；buf 是 count 字节只读用户文本快照；count 是输入字节数。
 * 出参/返回：成功返回 count 并更新 tunables/所有挂接 policy；解析失败返回 -EINVAL 且无状态改变。
 * 注意事项：属性层串行 policy_list 与对象生命周期；微秒乘 NSEC_PER_USEC 的目标类型为 s64 ns。
 */
static ssize_t
rate_limit_us_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	/* 先更新共享可见值，再把微秒转换为每个 policy 热路径使用的纳秒缓存。 */
	tunables->rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook)
		sg_policy->freq_update_delay_ns = rate_limit_us * NSEC_PER_USEC;

	return count;
}

/* 属性描述符把 show/store 暴露为 rate_limit_us，数组以 NULL 结束供宏生成组。 */
static struct governor_attr rate_limit_us = __ATTR_RW(rate_limit_us);

static struct attribute *sugov_attrs[] = {
	&rate_limit_us.attr,
	NULL
};
ATTRIBUTE_GROUPS(sugov);

/* kobject 最后一份引用释放时回收包含它的 tunables；attr_set 成员已由 kobject 核心清理。 */
/*
 * 业务背景：tunables 可能被多个 policy 共享，只有 kobject 最后引用释放回调能安全回收容器。
 * 入参：kobj 是不可空、内嵌于 gov_attr_set/tunables 的最后引用对象，ownership 正由 kobject 核心交还。
 * 出参/返回：无直接返回值；释放整个 sugov_tunables 存储，无输出参数。
 * 注意事项：只能作为 ktype.release 调用一次；policy hook 必须已摘空，释放后不得再访问 attr_set。
 */
static void sugov_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = to_gov_attr_set(kobj);

	kfree(to_sugov_tunables(attr_set));
}

/* ktype 将 sysfs 操作与最后引用释放统一绑定到 tunables 容器。 */
static const struct kobj_type sugov_tunables_ktype = {
	.default_groups = sugov_groups,
	.sysfs_ops = &governor_sysfs_ops,
	.release = &sugov_tunables_free,
};

/********************** cpufreq governor interface *********************/

static struct cpufreq_governor schedutil_gov;

/* 分配零初始化 policy 状态并初始化 update_lock；失败返回 NULL，不修改 cpufreq policy。 */
/*
 * 业务背景：governor INIT 为每个 cpufreq policy 建立独立热路径、缓存与可选异步 worker 容器。
 * 入参：policy 是不可空、由 cpufreq 核心拥有的借用对象，生命周期覆盖 governor 实例。
 * 出参/返回：成功返回新分配且由调用者拥有的 sugov_policy；分配失败返回 NULL；同时绑定 policy/初始化锁。
 * 注意事项：GFP_KERNEL 可睡眠；返回对象尚未发布、无 tunables/thread，失败清理由调用者按阶段负责。
 */
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
/*
 * 业务背景：governor EXIT 或 INIT 回滚最终需要释放尚未发布或已完全停止的 policy 私有容器。
 * 入参：sg_policy 是不可空、由调用者独占且来自 sugov_policy_alloc() 的输入指针，函数接管其释放责任。
 * 出参/返回：无直接返回值、无输出参数；sg_policy 指向的存储被释放，调用者不再拥有可用对象。
 * 注意事项：调用前必须摘除 tunables/hook 并停止 worker；函数不校验并发引用，释放后访问会 UAF。
 */
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
/*
 * 业务背景：不支持 rq 上下文 fast switch 的 policy 需要专用线程承接可能睡眠的驱动调频操作。
 * 入参：sg_policy 是不可空、尚未发布且由 INIT 独占的输入/输出对象，内部 policy 为借用引用。
 * 出参/返回：fast 路径或创建成功返回 0；线程创建或设置属性失败返回对应负 errno；成功时写入 thread/work/irq/mutex。
 * 注意事项：可睡眠；失败会停止已创建线程且不转移 sg_policy ownership，调用者仍须回滚 policy 对象和 fast-switch 状态。
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

	/* 慢路径先初始化 worker 容器，再创建尚未唤醒的执行线程。 */
	kthread_init_work(&sg_policy->work, sugov_work);
	kthread_init_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"sugov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create sugov thread: %pe\n", thread);
		return PTR_ERR(thread);
	}

	/* 特殊 DL 属性只解决 PI 调度优先级，设置失败必须停止未发布线程。 */
	ret = sched_setattr_nocheck(thread, &attr);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_DEADLINE\n", __func__);
		return ret;
	}

	/* 发布 thread 后按驱动能力允许跨 CPU 执行，或严格绑定 related_cpus。 */
	sg_policy->thread = thread;
	if (policy->dvfs_possible_from_any_cpu)
		set_cpus_allowed_ptr(thread, policy->related_cpus);
	else
		kthread_bind_mask(thread, policy->related_cpus);

	init_irq_work(&sg_policy->irq_work, sugov_irq_work);
	mutex_init(&sg_policy->work_lock);

	/* 所有可被线程访问的状态初始化完成后才允许其开始运行。 */
	wake_up_process(thread);

	return 0;
}

/* 慢路径退出先冲刷全部 worker 请求，再停止线程并销毁 mutex；fast policy 无资源可清。 */
/*
 * 业务背景：governor 退出或初始化回滚必须关闭慢切换执行器，确保其不再触碰 policy 状态。
 * 入参：sg_policy 是不可空、由调用者拥有的输入/输出对象；其 policy 借用引用决定是否存在慢路径资源。
 * 出参/返回：无直接返回值、无输出参数；慢路径返回时 worker 已冲刷、线程已停止、work_lock 已销毁。
 * 注意事项：可睡眠且要求外部已阻止新请求；fast-switch policy 未创建这些资源，函数直接返回。
 */
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
/*
 * 业务背景：每个 policy 或首个全局实例需要 sysfs tunables 容器保存 rate limit 并挂接 policy 列表。
 * 入参：sg_policy 是不可空、尚在 INIT 且由调用者拥有的输入/输出对象，其 tunables_hook 被 attr_set 借用。
 * 出参/返回：成功返回新分配的非空 tunables 并初始化 attr_set；失败返回 NULL；全局模式还发布 global_tunables。
 * 注意事项：GFP_KERNEL 可睡眠；返回对象最终由 kobject release 释放，发布全局指针时调用者必须持 global_tunables_lock。
 */
static struct sugov_tunables *sugov_tunables_alloc(struct sugov_policy *sg_policy)
{
	struct sugov_tunables *tunables;

	/* 分配成功后 attr_set 立即借用 policy hook；失败保持全局发布不变。 */
	tunables = kzalloc_obj(*tunables);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

/* 仅在使用全局 tunables 模式时清发布指针；对象本身由 attr_set/kobject 引用计数释放。 */
/*
 * 业务背景：全局 tunables 的最后一个 policy 退出或初始化失败时要撤销共享对象的发现入口。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；非 per-policy 模式下 global_tunables 被置空，对象 ownership 不变。
 * 注意事项：调用者必须持 global_tunables_lock；这里只撤销指针，不减少 kobject 引用也不释放内存。
 */
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
/*
 * 业务背景：cpufreq governor INIT 要把裸 policy 转换为可供 schedutil 热路径使用的完整实例。
 * 入参：policy 是不可空、由 cpufreq 核心拥有的输入/输出对象；要求 governor_data 为 NULL，policy 生命周期覆盖实例。
 * 出参/返回：成功返回 0 并发布 governor_data/tunables/执行资源；重复初始化返回 -EBUSY，分配或配置失败返回相应负 errno 并回滚。
 * 注意事项：可睡眠并持 global_tunables_lock；成功和失败均可能切换 fast-switch，失败路径必须按逆序撤销全部阶段性 ownership。
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

	/* fast-switch 是第一阶段外部状态，后续任一失败都由末端标签撤销。 */
	cpufreq_enable_fast_switch(policy);

	sg_policy = sugov_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = sugov_kthread_create(sg_policy);
	if (ret)
		goto free_sg_policy;

	/* 从此处串行全局 tunables 的发现、创建、引用和 governor_data 发布。 */
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

	/* 没有共享对象时创建新容器，分配失败尚未发布 governor_data。 */
	tunables = sugov_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	tunables->rate_limit_us = cpufreq_policy_transition_delay_us(policy);

	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;

	/* kobject 发布失败后仍须 put 初始引用，并撤销此前的两个发布指针。 */
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
	/* 只回滚已经到达的阶段，各标签向下贯穿形成严格逆序释放。 */
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	sugov_clear_global_tunables();

stop_kthread:
	sugov_kthread_stop(sg_policy);
	mutex_unlock(&global_tunables_lock);

	/* 此后对象已不再被全局状态发现，可释放 policy 并撤销最早启用的 fast switch。 */
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
/*
 * 业务背景：cpufreq governor EXIT 要撤销 INIT 建立的 sysfs 共享关系、异步执行器和 policy 私有状态。
 * 入参：policy 是不可空、由 cpufreq 核心拥有的输入/输出对象；要求 governor_data 指向已停止热路径的活动实例。
 * 出参/返回：无直接返回值、无输出参数；清空 governor_data，放弃 tunables 引用并释放 sg_policy，关闭 fast switch。
 * 注意事项：可睡眠且会持 global_tunables_lock；调用前应先 STOP 摘 hook，返回后旧 governor_data 不得再访问。
 */
static void sugov_exit(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	/* 锁内断开共享集合和 governor_data，最后引用同时撤销全局发现入口。 */
	mutex_lock(&global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count)
		sugov_clear_global_tunables();

	mutex_unlock(&global_tunables_lock);

	/* 发布关系撤销后，锁外执行可睡眠的线程停止和私有对象释放。 */
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
/*
 * 业务背景：governor START 要为一次运行期选择更新算法并向每个受管 CPU 发布 update-util hook。
 * 入参：policy 是不可空、由 cpufreq 核心拥有的输入/输出对象；governor_data 必须是 INIT 完成的活动实例。
 * 出参/返回：返回 0；重置 sg_policy 动态字段并初始化每 CPU sugov_cpu，向 cpufreq 槽发布对应回调。
 * 注意事项：调用环境必须与 cpufreq START 串行，发布后回调可在 rq/原子上下文执行；失败类别为无，ownership 不转移。
 */
static int sugov_start(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	void (*uu)(struct update_util_data *data, u64 time, unsigned int flags);
	unsigned int cpu;

	/* 每次 START 都清除上一运行期的限速、目标、pending 与 raw cache。 */
	sg_policy->freq_update_delay_ns	= sg_policy->tunables->rate_limit_us * NSEC_PER_USEC;
	sg_policy->last_freq_update_time	= 0;
	sg_policy->next_freq			= 0;
	sg_policy->work_in_progress		= false;
	sg_policy->limits_changed		= false;
	sg_policy->cached_raw_freq		= 0;

	sg_policy->need_freq_update = cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS);

	/* 回调选择优先共享 policy，其次 fast adjust_perf，最后通用频率路径。 */
	if (policy_is_shared(policy))
		uu = sugov_update_shared;
	else if (policy->fast_switch_enabled && cpufreq_driver_has_adjust_perf())
		uu = sugov_update_single_perf;
	else
		uu = sugov_update_single_freq;

	/* 清零每 CPU 旧状态并完成反向关联后，最后一步才发布 update-util hook。 */
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
/*
 * 业务背景：governor STOP 要先阻止新调度器采样，再排空已进入的 RCU、irq_work 与 worker 执行。
 * 入参：policy 是不可空、由 cpufreq 核心拥有的输入/输出对象；governor_data 指向 START 后的活动实例。
 * 出参/返回：无直接返回值、无输出参数；移除全部 CPU hook，返回时不存在仍访问该运行期状态的回调或慢路径 work。
 * 注意事项：synchronize_rcu/同步取消可等待，不能在不允许阻塞的上下文调用；fast-switch 配置没有异步资源可取消。
 */
static void sugov_stop(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	/* 先逐 CPU 摘除发现入口，再用 RCU grace period 等待已开始的读取者。 */
	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_rcu();

	/* slow-switch 还可能停留在 irq_work 或 worker 两级队列，必须分别同步。 */
	if (!policy->fast_switch_enabled) {
		irq_work_sync(&sg_policy->irq_work);
		kthread_cancel_work_sync(&sg_policy->work);
	}
}

/*
 * policy min/max 改变时，慢驱动先在 work_lock 下立即应用硬限制；随后写屏障保证 limits
 * 字段更新与 limits_changed 发布次序，与更新回调完整屏障配对，令下一次采样强制重算。
 */
/*
 * 业务背景：cpufreq 修改 policy 的 kHz min/max 后，schedutil 必须立即约束慢路径并令下一次采样绕过缓存重算。
 * 入参：policy 是不可空、由 cpufreq 核心拥有的输入/输出对象；governor_data 为活动 sg_policy，limits 已由核心更新。
 * 出参/返回：无直接返回值、无输出参数；慢路径应用硬限制，并发布 limits_changed=true 供更新回调消费。
 * 注意事项：慢路径会获取 work_lock 并可睡眠；写屏障与 sugov_should_update_freq() 配对，不能改成无序普通提示。
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
	/* 生命周期回调由 cpufreq 核心按 INIT→START→STOP→EXIT 顺序调用。 */
	.init			= sugov_init,
	.exit			= sugov_exit,
	.start			= sugov_start,
	.stop			= sugov_stop,
	.limits			= sugov_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
/* 配置选择 schedutil 为默认 governor 时返回其静态描述符，所有权仍归本模块。 */
/*
 * 业务背景：编译期选择 schedutil 为默认 governor 时，cpufreq 核心需要取得其注册描述符。
 * 入参：无。
 * 出参/返回：返回非空、指向静态 schedutil_gov 的借用指针；无输出参数，ownership 不转移。
 * 注意事项：仅在 CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL 下存在；调用者不得释放或修改描述符生命周期。
 */
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &schedutil_gov;
}
#endif

/* 以描述符地址判断 policy 当前 governor 是否正是 schedutil；只读瞬时状态。 */
/*
 * 业务背景：调度器和能耗路径需要快速判断一个 cpufreq policy 当前是否由 schedutil 管理。
 * 入参：policy 是不可空、由 cpufreq 核心拥有的只读借用输入；函数不获取额外引用。
 * 出参/返回：描述符地址等于 schedutil_gov 返回 true，否则返回 false；无输出参数或 ownership 变化。
 * 注意事项：只做无锁瞬时读取，不保证返回后 governor 不切换；传入 NULL 会直接解引用并崩溃。
 */
bool sugov_is_governor(struct cpufreq_policy *policy)
{
	return policy->governor == &schedutil_gov;
}

cpufreq_governor_init(schedutil_gov);
