// SPDX-License-Identifier: GPL-2.0
/*
 * PELT 更新与时间轴内部接口学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * PELT（Per-Entity Load Tracking）把 task、cfs_rq、RT、DL、IRQ 和硬件压力的近期
 * 活动表示为约 1ms 分段、约 32ms 半衰期的指数衰减信号。本头文件连接 pelt.c 的求和/
 * 平均值实现与 core/fair/cpufreq 等消费路径，并维护容量/频率不变的 rq PELT 时间轴。
 * 它不选择任务或 CPU；输出是后续负载均衡、能耗选核和调频使用的动态估计。
 *
 * 更新者通常持有目标 rq 锁且已更新 rq 时钟；`sched_avg.last_update_time/period_contrib`
 * 与各 sum/avg 必须作为同一状态推进。rq->clock_pelt 按 CPU/频率容量缩放运行时间，idle
 * 时追平 clock_task；lost_idle_time 排除“满载低容量导致的虚假空闲”。NO_HZ 迁移读者
 * 通过 idle 快照和写/读屏障，只允许低估遗漏时间，避免危险的过度衰减。
 *
 * CFS bandwidth 再从 rq PELT 时钟扣除组被 throttle 的时间，使被限流不被解释成自然
 * 空闲。配置关闭时相应接口退化为零值或直接 rq 时钟。所有输入指针均为借用对象内部
 * 状态，调用者负责保活和串行化；本文件不分配、发布或释放对象。
 */
#ifndef _KERNEL_SCHED_PELT_H
#define _KERNEL_SCHED_PELT_H
/* sched.h 提供 rq/cfs_rq/sched_entity/sched_avg 布局及锁时钟 helper。 */
#include "sched.h"

/* 自动生成头提供 LOAD_AVG_MAX 和指数衰减查表常量；该生成文件禁止直接手改。 */
#include "sched-pelt.h"

/*
 * pelt.c 的六个跨成员更新入口。
 *
 * @now 均为调用者提供的 PELT 时间戳，单位 ns，必须与目标 avg 的时间轴一致；@se、
 * @cfs_rq、@rq 均为不可空借用对象，调用者用 rq 锁/附着协议稳定它们。三个
 * `__update_load_avg_*` 分别衰减阻塞实体、更新已附着实体和更新整个 CFS rq；返回 1
 * 表示跨过 PELT 周期并重算 avg/发 tracepoint，0 表示没有完整周期变化。
 */
/*
 * __update_load_avg_blocked_se() - 让已阻塞/脱队实体的历史负载衰减到 @now
 * @now: 与实体原 cfs_rq 一致的 PELT ns 时间；@se: 输入输出的稳定借用实体。
 * 返回 1 表示跨周期并重算三个 avg/发 tracepoint，0 表示无需重算；以全零活动输入推进，
 * 不改变附着关系或 ownership，不睡眠。调用者须用 rq/removed-load 协议串行化。
 */
int __update_load_avg_blocked_se(u64 now, struct sched_entity *se);
/*
 * __update_load_avg_se() - 按实体 on_rq/runnable/running 状态更新其 PELT
 * @now: cfs_rq PELT ns 时间；@cfs_rq: 实体当前附着队列；@se: 输入输出实体，均不可空借用。
 * 返回 1/0 表示是否跨周期重算；成功还清 util_est unchanged 标志并发 tracepoint。调用者
 * 持所属 rq 锁，函数不迁移实体、不分配内存、不睡眠。
 */
int __update_load_avg_se(u64 now, struct cfs_rq *cfs_rq, struct sched_entity *se);
/*
 * __update_load_avg_cfs_rq() - 用队列权重、可运行数和 curr 状态更新聚合 CFS PELT
 * @now: 本队列 PELT ns 时间；@cfs_rq: 输入输出、不可空的稳定借用队列。
 * 返回 1 表示跨周期、重算 avg 并发 tracepoint，0 表示未跨周期；调用者持 rq 锁，
 * 函数不传播 task_group 聚合值，调用方在返回后完成传播。
 */
int __update_load_avg_cfs_rq(u64 now, struct cfs_rq *cfs_rq);
/*
 * update_rt_rq_load_avg() - 更新 rq 的实时类二值运行利用率
 * @now: rq PELT ns 时间；@rq: 输入输出稳定借用队列；@running: RT 类当前是否执行。
 * 返回 1/0 表示是否跨周期重算 avg_rt 并发 tracepoint；不跟踪逐 RT 实体 load_avg。
 * 调用者持 rq 锁且时钟已更新，函数不睡眠。
 */
int update_rt_rq_load_avg(u64 now, struct rq *rq, int running);
/*
 * update_dl_rq_load_avg() - 更新 rq 的 deadline 类二值运行利用率
 * @now: rq PELT ns 时间；@rq: 输入输出稳定借用队列；@running: DL 类当前是否执行。
 * 返回 1/0 表示是否跨周期重算 avg_dl 并发 tracepoint；不跟踪逐 DL 实体 load_avg。
 * 调用者持 rq 锁且时钟已更新，函数不睡眠。
 */
int update_dl_rq_load_avg(u64 now, struct rq *rq, int running);
/*
 * update_other_load_avgs() - 一次更新当前 rq 的 RT、DL、硬件压力和 IRQ PELT 信号
 * @rq: 输入输出、不可为 NULL；调用者必须持 rq 锁并已更新 rq clock。
 * 返回 true 表示至少一个子信号跨周期更新，false 表示均未更新。函数使用按位 OR，保证
 * 四个 helper 全部执行；不更新 fair avg、不睡眠，也不转移 rq ownership。
 */
bool update_other_load_avgs(struct rq *rq);

#ifdef CONFIG_SCHED_HW_PRESSURE
/*
 * update_hw_load_avg() - 以硬件事件削减的容量更新 rq->avg_hw
 * @now: rq task-clock 时间戳，单位 ns；硬件压力不使用容量缩放后的 clock_pelt。
 * @rq: 输入输出的稳定借用 rq，调用者持锁且时钟已更新。
 * @capacity: 本采样区间损失的容量量，采用 SCHED_CAPACITY_SCALE 尺度，不是二值 running。
 * 返回 1 表示跨周期并重算 load_avg，0 表示没有；更新 tracepoint，无分配或睡眠。
 */
int update_hw_load_avg(u64 now, struct rq *rq, u64 capacity);

/*
 * hw_load_avg() - 无锁读取 rq 最近的平均硬件压力
 * @rq: 纯输入、不可为 NULL 的存活 rq，借用且不取得锁/引用。
 * 返回 SCHED_CAPACITY_SCALE 尺度的 load_avg 快照。READ_ONCE 只保证单次编译器可见读取，
 * 不与其他 rq 字段构成一致快照；无副作用、不会睡眠。
 */
static inline u64 hw_load_avg(struct rq *rq)
{
	return READ_ONCE(rq->avg_hw.load_avg);
}
#else /* !CONFIG_SCHED_HW_PRESSURE: */
/* 关闭硬件压力跟踪时保留调用面；三个参数均不消费，恒返回 0 且不修改 rq。 */
static inline int
update_hw_load_avg(u64 now, struct rq *rq, u64 capacity)
{
	return 0;
}

/* @rq 为未消费的借用输入；关闭配置时不存在 avg_hw 语义，恒返回 0。 */
static inline u64 hw_load_avg(struct rq *rq)
{
	return 0;
}
#endif /* !CONFIG_SCHED_HW_PRESSURE */

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
/*
 * update_irq_load_avg() - 把上次更新以来的 IRQ 执行时间加入 rq->avg_irq
 * @rq: 输入输出、不可为 NULL，调用者持 rq 锁且已更新 rq->clock。
 * @running: IRQ 运行时间，单位 ns；实现先按频率/CPU 容量缩放，再假定 IRQ 位于区间尾部。
 * 返回两个分段更新结果之和，0 表示未跨周期，正值表示 avg 已重算；不睡眠、不转移状态。
 */
int update_irq_load_avg(struct rq *rq, u64 running);
#else
/* 配置/架构不提供 IRQ PELT 时，@rq/@running 均不消费，恒返回 0 且无副作用。 */
static inline int
update_irq_load_avg(struct rq *rq, u64 running)
{
	return 0;
}
#endif

/*
 * 一个完整指数衰减历史在周期边界的最小归一化分母。当前未完成周期的
 * period_contrib（0..1023）随后加入它，避免把尚未流逝的窗口尾部误算成 idle。
 */
#define PELT_MIN_DIVIDER	(LOAD_AVG_MAX - 1024)

/*
 * get_pelt_divider() - 取得与 avg 当前窗口位置匹配的 sum→avg 分母
 * @avg: 纯输入、不可为 NULL 的 sched_avg 借用指针；调用者须稳定更新状态。
 * 返回 PELT_MIN_DIVIDER + period_contrib，量纲是衰减贡献单位；不修改对象、不睡眠。
 */
static inline u32 get_pelt_divider(struct sched_avg *avg)
{
	return PELT_MIN_DIVIDER + avg->period_contrib;
}

/*
 * cfs_se_util_change() - 在 util_avg 变化后清除 util_est 的“平均值未变化”标志
 * @avg: 输入输出、不可为 NULL 的实体 sched_avg；调用者已在 rq/PELT 更新协议内持有它。
 * 返回：无直接返回值。UTIL_EST 关闭或标志已清时快速返回；否则只清
 * UTIL_AVG_UNCHANGED，并以 WRITE_ONCE 发布，使后续 util_est 更新知道需要重新采样。
 * 函数不重算 util_est、不睡眠；局部 enqueued 是含估值和标志位的瞬时副本。
 */
static inline void cfs_se_util_change(struct sched_avg *avg)
{
	unsigned int enqueued;

	/* 特性关闭时不维护这项协议信号，保留现有估值内容。 */
	if (!sched_feat(UTIL_EST))
		return;

	/* Avoid store if the flag has been already reset */
	/* 原文说明：若标志已被其他更新清除，避免一次无意义且会扰动 cache line 的写入。 */
	enqueued = avg->util_est;
	if (!(enqueued & UTIL_AVG_UNCHANGED))
		return;

	/* Reset flag to report util_avg has been updated */
	/* 原文说明：清位向 util_est 消费路径报告 util_avg 已变化；其余估值位逐位保留。 */
	enqueued &= ~UTIL_AVG_UNCHANGED;
	WRITE_ONCE(avg->util_est, enqueued);
}

/*
 * rq_clock_pelt() - 返回排除不可恢复 idle 缺口后的 rq PELT 时间
 * @rq: 纯输入、不可为 NULL；调用者必须持有 rq 锁且已经 update_rq_clock()。
 * 返回 `clock_pelt - lost_idle_time`，单位 ns，是 fair/RT/DL PELT 更新的共同时间轴。
 * lockdep/assert 只验证调用契约；函数借用字段、不修改 rq、不会睡眠。
 */
static inline u64 rq_clock_pelt(struct rq *rq)
{
	lockdep_assert_rq_held(rq);
	assert_clock_updated(rq);

	return rq->clock_pelt - rq->lost_idle_time;
}

/* The rq is idle, we can sync to clock_task */
/*
 * 原文说明：rq 已进入 idle，因此容量缩放的工作时间轴可以追平 clock_task。
 *
 * _update_idle_rq_clock_pelt() - 发布 rq 最近一次 idle 的两份配对时钟快照
 * @rq: 输入输出、不可为 NULL；调用者持 rq 锁且时钟已更新，并确认 curr 是 idle task。
 * 返回：无直接返回值。先令 clock_pelt 追平 task clock，保存真实 rq clock 快照，再以
 * 写屏障发布扣除 lost_idle_time 的 PELT 快照。u64_u32_store 允许 32 位架构安全读写 u64。
 * 与 migrate_se_pelt_lag() 的“先读 PELT、rmb、再读 rq clock”配对：reader 若混合代际，
 * 最坏看到旧 PELT + 新 rq clock 而低估遗漏时间，不会看到会导致过度衰减的反向组合。
 */
static inline void _update_idle_rq_clock_pelt(struct rq *rq)
{
	/* idle 是同步边界：之前因容量缩放放慢的 PELT 时钟在这里补齐到 task clock。 */
	rq->clock_pelt  = rq_clock_task(rq);

	/* 先发布未缩放 rq clock；它是迁移方估计 idle 后经过真实时间的基准。 */
	u64_u32_store(rq->clock_idle, rq_clock(rq));
	/* Paired with smp_rmb in migrate_se_pelt_lag() */
	/* 原文说明：与 migrate_se_pelt_lag() 的 smp_rmb 配对，约束两份快照的可见顺序。 */
	smp_wmb();
	/* 后发布有效 PELT 时钟，使观察到新值的 reader 必然也能观察到前一份真实时钟。 */
	u64_u32_store(rq->clock_pelt_idle, rq_clock_pelt(rq));
}

/*
 * The clock_pelt scales the time to reflect the effective amount of
 * computation done during the running delta time but then sync back to
 * clock_task when rq is idle.
 *
 *
 * absolute time   | 1| 2| 3| 4| 5| 6| 7| 8| 9|10|11|12|13|14|15|16
 * @ max capacity  ------******---------------******---------------
 * @ half capacity ------************---------************---------
 * clock pelt      | 1| 2|    3|    4| 7| 8| 9|   10|   11|14|15|16
 *
 */
/*
 * 原文说明：clock_pelt 把实际经过时间缩放成“以最大算力完成了多少计算”，但 rq idle 时
 * 又同步回 clock_task。示意图中半容量执行同样工作需要两倍墙上时间，PELT 时钟只推进
 * 一半；进入 idle 后跳到 task clock，差值作为从低容量忙碌阶段借来的 idle 时间处理。
 *
 * update_rq_clock_pelt() - 推进一次 rq 容量/频率不变的 PELT 时钟
 * @rq: 输入输出、不可为 NULL；调用者持 rq 锁且已把通用 rq 时钟推进到当前时刻。
 * @delta: 自上次 rq clock 更新经过的非负时间，单位 ns；idle 分支不使用其数值。
 * 返回：无直接返回值。idle task 走同步/快照路径；非 idle 依次乘 CPU 原始容量和当前
 * 频率容量比例后累加 clock_pelt。函数不更新任何 avg、不睡眠，也不转移 rq ownership。
 */
static inline void update_rq_clock_pelt(struct rq *rq, s64 delta)
{
	/* idle 时不能继续按执行容量推进；同步并发布迁移估时所需快照后结束。 */
	if (unlikely(is_idle_task(rq->curr))) {
		_update_idle_rq_clock_pelt(rq);
		return;
	}

	/*
	 * When a rq runs at a lower compute capacity, it will need
	 * more time to do the same amount of work than at max
	 * capacity. In order to be invariant, we scale the delta to
	 * reflect how much work has been really done.
	 * Running longer results in stealing idle time that will
	 * disturb the load signal compared to max capacity. This
	 * stolen idle time will be automatically reflected when the
	 * rq will be idle and the clock will be synced with
	 * rq_clock_task.
	 */
	/*
	 * 原文说明：低算力 CPU 完成同样工作需要更久；若直接用墙上时间，PELT 会把“运行更久”
	 * 错当成更多工作并挤占未来 idle。这里按原始容量和频率容量缩小 delta，使信号对算力
	 * 不变；其墙上时间差在 rq 后续 idle、clock_pelt 追平 clock_task 时自然显现。
	 */

	/*
	 * Scale the elapsed time to reflect the real amount of
	 * computation
	 */
	/* 原文说明：两次 cap_scale 分别校正 CPU 最大容量差异和当前频率造成的算力差异。 */
	delta = cap_scale(delta, arch_scale_cpu_capacity(cpu_of(rq)));
	delta = cap_scale(delta, arch_scale_freq_capacity(cpu_of(rq)));

	/* 只有 rq 锁持有者写该字段；所有 PELT 类信号随后从这个统一时间轴取 now。 */
	rq->clock_pelt += delta;
}

/*
 * When rq becomes idle, we have to check if it has lost idle time
 * because it was fully busy. A rq is fully used when the /Sum util_sum
 * is greater or equal to:
 * (LOAD_AVG_MAX - 1024 + rq->cfs.avg.period_contrib) << SCHED_CAPACITY_SHIFT;
 * For optimization and computing rounding purpose, we don't take into account
 * the position in the current window (period_contrib) and we use the higher
 * bound of util_sum to decide.
 */
/*
 * 原文说明：rq 转 idle 时要判断低容量满载阶段是否“借走”了理论 idle。完整利用时，
 * cfs/RT/DL util_sum 之和达到与当前 PELT 分母对应的满量程阈值；为减少计算和统一舍入，
 * 这里忽略当前窗口位置 period_contrib，采用 util_sum 上界作保守判定。
 *
 * update_idle_rq_clock_pelt() - 在 rq 转 idle 边界结算 lost_idle_time 并同步 PELT 时钟
 * @rq: 输入输出、不可为 NULL；调用者持 rq 锁、已更新时钟，且正在把 rq 置为 idle。
 * 返回：无直接返回值。汇总 CFS/RT/DL util_sum；达到满载阈值时把 task clock 超前于
 * clock_pelt 的差加入 lost_idle_time，随后无条件调用内部同步 helper 发布 idle 快照。
 * 函数不包含 IRQ/HW 压力于满载判定，不重算 avg、不睡眠。
 */
static inline void update_idle_rq_clock_pelt(struct rq *rq)
{
	/* divider 是忽略 period_contrib 后、按 SCHED_CAPACITY_SHIFT 放大的满载比较阈值。 */
	u32 divider = ((LOAD_AVG_MAX - 1024) << SCHED_CAPACITY_SHIFT) - LOAD_AVG_MAX;
	/* util_sum 汇总三个会占用 task 执行时段的调度类；局部副本仅在本次结算有效。 */
	u32 util_sum = rq->cfs.avg.util_sum;
	util_sum += rq->avg_rt.util_sum;
	util_sum += rq->avg_dl.util_sum;

	/*
	 * Reflecting stolen time makes sense only if the idle
	 * phase would be present at max capacity. As soon as the
	 * utilization of a rq has reached the maximum value, it is
	 * considered as an always running rq without idle time to
	 * steal. This potential idle time is considered as lost in
	 * this case. We keep track of this lost idle time compare to
	 * rq's clock_task.
	 */
	/*
	 * 原文说明：只有最大容量下本应出现 idle、而低容量导致任务延长运行时，“被偷走”的
	 * idle 才应反映进时间轴。信号已经满量程表示该 rq 可视为一直运行，没有可偷 idle；
	 * 此时把 clock_task 与较慢 clock_pelt 的差记为永久丢失，后续返回的 rq_clock_pelt()
	 * 会持续扣除它，避免 idle 同步凭空增加可衰减时间。
	 */
	if (util_sum >= divider)
		rq->lost_idle_time += rq_clock_task(rq) - rq->clock_pelt;

	/* 无论是否满载，idle 边界都要追平 clock_pelt 并发布迁移方使用的配对快照。 */
	_update_idle_rq_clock_pelt(rq);
}

#ifdef CONFIG_CFS_BANDWIDTH
/*
 * update_idle_cfs_rq_clock_pelt() - 在 cfs_rq 变空时保存其 throttle 时间快照
 * @cfs_rq: 输入输出、不可为 NULL；调用者持所属 rq 锁，且 nr_queued 已变为 0。
 * 返回：无直接返回值。若 PELT 时钟当前因层级 throttle 停止，保存 U64_MAX 哨兵；否则
 * 保存累计 throttled_clock_pelt_time。u64_u32_store 支持 32 位无撕裂快照，供 NO_HZ
 * 迁移估算扣除限流时间；不更新 avg、不睡眠。
 */
static inline void update_idle_cfs_rq_clock_pelt(struct cfs_rq *cfs_rq)
{
	/* throttled 是本次 idle 边界发布的累计停钟时间，U64_MAX 表示停钟区间尚未闭合。 */
	u64 throttled;

	/* 正在 throttle 时无法知道最终持续量，迁移方看到哨兵会放弃估算以免过度衰减。 */
	if (unlikely(cfs_rq->pelt_clock_throttled))
		throttled = U64_MAX;
	else
		throttled = cfs_rq->throttled_clock_pelt_time;

	/* 发布累计值或停钟哨兵；对象由 rq 锁保护，迁移方用配套 u64_u32_load() 读取。 */
	u64_u32_store(cfs_rq->throttled_pelt_idle, throttled);
}

/* rq->task_clock normalized against any time this cfs_rq has spent throttled */
/*
 * 原文说明：返回的 cfs_rq PELT 时钟以 rq task clock 为基础，但扣除本组被 throttle 的
 * 全部时间；限流不是实体自愿 idle，不能让负载信号在无法运行期间自然衰减。
 *
 * cfs_rq_clock_pelt() - 取得排除 CFS bandwidth 限流区间的组 PELT 时间
 * @cfs_rq: 纯输入、不可为 NULL；调用者持所属 rq 锁且已更新时钟。
 * 返回 ns 时间戳。停钟中使用冻结的 throttled_clock_pelt，正常时使用 rq_clock_pelt；
 * 两条路径都扣除已累计 throttle 时间。无副作用、不睡眠，结果保持本组 avg 时间轴连续。
 */
static inline u64 cfs_rq_clock_pelt(struct cfs_rq *cfs_rq)
{
	/* 当前限流时钟冻结，不能用仍在推进的 rq clock，否则会错误衰减被禁止运行的组。 */
	if (unlikely(cfs_rq->pelt_clock_throttled))
		return cfs_rq->throttled_clock_pelt - cfs_rq->throttled_clock_pelt_time;

	/* 未限流时从 rq 有效 PELT 时钟扣除历史累计停钟时间。 */
	return rq_clock_pelt(rq_of(cfs_rq)) - cfs_rq->throttled_clock_pelt_time;
}
#else /* !CONFIG_CFS_BANDWIDTH: */
/* @cfs_rq 为未消费借用输入；无 bandwidth 时没有 throttle 快照，stub 无副作用。 */
static inline void update_idle_cfs_rq_clock_pelt(struct cfs_rq *cfs_rq) { }
/*
 * cfs_rq_clock_pelt() - 无 bandwidth 配置下直接复用所属 rq 的 PELT 时钟
 * @cfs_rq: 纯输入、不可为 NULL；调用者仍须持 rq 锁并已更新时间。
 * 返回 ns 借用快照；没有需扣除的 throttle 时间，无副作用、不会睡眠。
 */
static inline u64 cfs_rq_clock_pelt(struct cfs_rq *cfs_rq)
{
	return rq_clock_pelt(rq_of(cfs_rq));
}
#endif /* !CONFIG_CFS_BANDWIDTH */
/* 两个配置分支维持相同调用面，但只有 bandwidth 版本拥有停钟状态和迁移快照。 */

#endif /* _KERNEL_SCHED_PELT_H */
/* 结束本内部头的重复包含保护。 */
