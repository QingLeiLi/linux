// SPDX-License-Identifier: GPL-2.0
/*
 * Per Entity Load Tracking (PELT)
 *
 *  Copyright (C) 2007 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *
 *  Interactivity improvements by Mike Galbraith
 *  (C) 2007 Mike Galbraith <efault@gmx.de>
 *
 *  Various enhancements by Dmitry Adamushko.
 *  (C) 2007 Dmitry Adamushko <dmitry.adamushko@gmail.com>
 *
 *  Group scheduling enhancements by Srivatsa Vaddagiri
 *  Copyright IBM Corporation, 2007
 *  Author: Srivatsa Vaddagiri <vatsa@linux.vnet.ibm.com>
 *
 *  Scaled math optimizations by Thomas Gleixner
 *  Copyright (C) 2007, Linutronix GmbH, Thomas Gleixner <tglx@kernel.org>
 *
 *  Adaptive scheduling granularity, math enhancements by Peter Zijlstra
 *  Copyright (C) 2007 Red Hat, Inc., Peter Zijlstra
 *
 *  Move PELT related code from fair.c into this pelt.c file
 *  Author: Vincent Guittot <vincent.guittot@linaro.org>
 */
#include "pelt.h"

/*
 * Approximate:
 *   val * y^n,    where y^32 ~= 0.5 (~1 scheduling period)
 */
/*
 * 中文释义：按 n 个 PELT 小周期衰减 val，其中每 32 段约减半。先用移位处理完整
 * 32 段，再查表乘余数，可在常数时间完成；超过 63 个半衰期直接归零，既避免
 * 无意义精度也限制移位范围。本函数只做定点运算，不修改跟踪对象。
 */
/*
 * 业务背景：PELT 更新需把旧贡献按离当前的周期数指数降权，本 helper 用移位加查表避免逐周期循环。
 * 入参：val 是纯输入的无符号定点贡献；n 是纯输入、以 1024us PELT 段计的非负衰减周期数。
 * 出参/返回：返回 val*y^n 的截断近似；n 超过 2016 段返回 0，无输出参数和 ownership 变化。
 * 注意事项：输入量纲由调用者保持一致；舍入与超长历史归零属于算法精度边界，函数不睡眠。
 */
static u64 decay_load(u64 val, u64 n)
{
	unsigned int local_n;

	/* 超过查表与移位仍有意义的历史长度时，直接把贡献视为完全衰减。 */
	if (unlikely(n > LOAD_AVG_PERIOD * 63))
		return 0;

	/* after bounds checking we can collapse to 32-bit */
	local_n = n;

	/*
	 * As y^PERIOD = 1/2, we can combine
	 *    y^n = 1/2^(n/PERIOD) * y^(n%PERIOD)
	 * With a look-up table which covers y^n (n<PERIOD)
	 *
	 * To achieve constant time decay_load.
	 */
	if (unlikely(local_n >= LOAD_AVG_PERIOD)) {
		val >>= local_n / LOAD_AVG_PERIOD;
		local_n %= LOAD_AVG_PERIOD;
	}

	val = mul_u64_u32_shr(val, runnable_avg_yN_inv[local_n], 32);
	return val;
}

/*
 * 计算跨越 period 边界时新增的三段贡献：旧周期尾 d1 要衰减 periods 次，中间
 * 完整周期用等比级数求和，当前周期头 d3 权重为 1。返回值尚未乘实体负载或容量。
 */
/*
 * 业务背景：一次更新时间可能跨越多个约 1ms 段，需要把旧段尾、完整中段和当前段头合成新时间贡献。
 * 入参：periods 是已跨完整段数且调用路径中大于 0；d1/d3 是各自残段贡献，范围 0..1024、纯输入。
 * 出参/返回：返回三段按年龄衰减后的 u32 时间贡献，不修改参数、对象或 ownership。
 * 注意事项：返回值尚未乘 load/容量刻度；参数必须来自同一次 period_contrib 分割，函数不睡眠。
 */
static u32 __accumulate_pelt_segments(u64 periods, u32 d1, u32 d3)
{
	u32 c1, c2, c3 = d3; /* y^0 == 1 */

	/*
	 * c1 = d1 y^p
	 */
	c1 = decay_load((u64)d1, periods);

	/*
	 *            p-1
	 * c2 = 1024 \Sum y^n
	 *            n=1
	 *
	 *              inf        inf
	 *    = 1024 ( \Sum y^n - \Sum y^n - y^0 )
	 *              n=0        n=p
	 */
	c2 = LOAD_AVG_MAX - decay_load(LOAD_AVG_MAX, periods) - 1024;

	return c1 + c2 + c3;
}

/*
 * Accumulate the three separate parts of the sum; d1 the remainder
 * of the last (incomplete) period, d2 the span of full periods and d3
 * the remainder of the (incomplete) current period.
 *
 *           d1          d2           d3
 *           ^           ^            ^
 *           |           |            |
 *         |<->|<----------------->|<--->|
 * ... |---x---|------| ... |------|-----x (now)
 *
 *                           p-1
 * u' = (u + d1) y^p + 1024 \Sum y^n + d3 y^0
 *                           n=1
 *
 *    = u y^p +					(Step 1)
 *
 *                     p-1
 *      d1 y^p + 1024 \Sum y^n + d3 y^0		(Step 2)
 *                     n=1
 */
/*
 * 中文释义：把上次更新后的残段、跨过的完整 1024us 段和当前残段合并。先衰减
 * 三个历史 sum，再只对状态有效的维度增加新贡献；period_contrib 保存当前段位置。
 * 返回跨过的完整周期数，0 表示 sum 可更新但 avg 尚无需重新除法同步。
 */
/*
 * 业务背景：量化后的新时间要同时推进 load/runnable/util 三个指数和，并保持它们共享同一分段位置。
 * 入参：delta 是自上次更新起的非负 1024ns 单位数；sa 是不可空、借用的可写 PELT 状态；
 * load/runnable 是纯输入活动权重，running 是 0/1 执行状态；调用者保证 load 为 0 时后二者也为 0。
 * 出参/返回：返回跨过的完整 1024us 段数，并原地更新 sa 的三个 sum/period_contrib；ownership 不变。
 * 注意事项：调用者须串行化 sa 且确保乘法量纲/范围不溢出；返回 0 仍可能累计当前残段，不可当作无变化。
 */
static __always_inline u32
accumulate_sum(u64 delta, struct sched_avg *sa,
	       unsigned long load, unsigned long runnable, int running)
{
	u32 contrib = (u32)delta; /* p == 0 -> delta < 1024 */
	u64 periods;

	delta += sa->period_contrib;
	periods = delta / 1024; /* A period is 1024us (~1ms) */

	/*
	 * Step 1: decay old *_sum if we crossed period boundaries.
	 */
	/* 中文释义：只有越过段边界，旧历史才整体多乘相应次数的衰减因子。 */
	if (periods) {
		sa->load_sum = decay_load(sa->load_sum, periods);
		sa->runnable_sum =
			decay_load(sa->runnable_sum, periods);
		sa->util_sum = decay_load((u64)(sa->util_sum), periods);

		/*
		 * Step 2
		 */
		/* 中文释义：随后把三段新增时间按各自年龄权重加入本次贡献。 */
		delta %= 1024;
		if (load) {
			/*
			 * This relies on the:
			 *
			 * if (!load)
			 *	runnable = running = 0;
			 *
			 * clause from ___update_load_sum(); this results in
			 * the below usage of @contrib to disappear entirely,
			 * so no point in calculating it.
			 */
			/* 中文释义：load 为 0 时另外两种状态已被入口清零，故无需计算 contrib。 */
			contrib = __accumulate_pelt_segments(periods,
					1024 - sa->period_contrib, delta);
		}
	}
	/* 提交当前段游标后，三个活动维度使用同一份时间贡献保持窗口对齐。 */
	sa->period_contrib = delta;

	if (load)
		sa->load_sum += load * contrib;
	if (runnable)
		sa->runnable_sum += runnable * contrib << SCHED_CAPACITY_SHIFT;
	if (running)
		sa->util_sum += contrib << SCHED_CAPACITY_SHIFT;

	return periods;
}

/*
 * We can represent the historical contribution to runnable average as the
 * coefficients of a geometric series.  To do this we sub-divide our runnable
 * history into segments of approximately 1ms (1024us); label the segment that
 * occurred N-ms ago p_N, with p_0 corresponding to the current period, e.g.
 *
 * [<- 1024us ->|<- 1024us ->|<- 1024us ->| ...
 *      p0            p1           p2
 *     (now)       (~1ms ago)  (~2ms ago)
 *
 * Let u_i denote the fraction of p_i that the entity was runnable.
 *
 * We then designate the fractions u_i as our co-efficients, yielding the
 * following representation of historical load:
 *   u_0 + u_1*y + u_2*y^2 + u_3*y^3 + ...
 *
 * We choose y based on the with of a reasonably scheduling period, fixing:
 *   y^32 = 0.5
 *
 * This means that the contribution to load ~32ms ago (u_32) will be weighted
 * approximately half as much as the contribution to load within the last ms
 * (u_0).
 *
 * When a period "rolls over" and we have new u_0`, multiplying the previous
 * sum again by y is sufficient to update:
 *   load_avg = u_0` + y*(u_0 + u_1*y + u_2*y^2 + ... )
 *            = u_0 + u_1*y + u_2*y^2 + ... [re-labeling u_i --> u_{i+1}]
 */
/*
 * 中文释义：PELT 将历史按约 1ms 分段并以 y^32=0.5 加权，越旧的 runnable 时间
 * 影响越小。入口先处理 sched_clock 初始化期间的倒退，再把纳秒换成 1024ns 单位；
 * 只有跨过完整 PELT 段才返回 1，提示调用者把 sum 同步成 avg。
 */
/*
 * 业务背景：各调度实体/队列在事件点先把墙上时间和活动状态折进 PELT sum，再按需重算可消费 avg。
 * 入参：now 是与 sa 同时钟域的 ns 时间戳；sa 是不可空、借用的可写状态；load/runnable 是活动权重，
 * running 是 0/1 执行状态，均为纯输入；load 为 0 时实现强制清除另外两种活动输入。
 * 出参/返回：跨完整 PELT 段返回 1，否则（含时钟倒退或不足 1024ns）返回 0；原地推进 sa sum/时间基准。
 * 注意事项：调用者须持相应 rq 锁并保持 now 单调；返回 0 不必然无副作用，也不转移 sa ownership。
 */
static __always_inline int
___update_load_sum(u64 now, struct sched_avg *sa,
		  unsigned long load, unsigned long runnable, int running)
{
	u64 delta;

	delta = now - sa->last_update_time;
	/*
	 * This should only happen when time goes backwards, which it
	 * unfortunately does during sched clock init when we swap over to TSC.
	 */
	/* 中文释义：时钟倒退时只重置基准并拒绝用负时间累计，避免制造巨大的无符号 delta。 */
	if ((s64)delta < 0) {
		sa->last_update_time = now;
		return 0;
	}

	/*
	 * Use 1024ns as the unit of measurement since it's a reasonable
	 * approximation of 1us and fast to compute.
	 */
	/* 中文释义：右移 10 以低成本量化；不足一个单位的尾数留在 last_update_time 中。 */
	delta >>= 10;
	if (!delta)
		return 0;

	sa->last_update_time += delta << 10;

	/*
	 * running is a subset of runnable (weight) so running can't be set if
	 * runnable is clear. But there are some corner cases where the current
	 * se has been already dequeued but cfs_rq->curr still points to it.
	 * This means that weight will be 0 but not running for a sched_entity
	 * but also for a cfs_rq if the latter becomes idle. As an example,
	 * this happens during sched_balance_newidle() which calls
	 * sched_balance_update_blocked_averages().
	 *
	 * Also see the comment in accumulate_sum().
	 */
	/*
	 * 中文释义：running 必须是 runnable/load 的子集；实体已脱队但 curr 指针尚未清除
	 * 等角落状态以 load==0 为准，同时清空 runnable/running，防止凭旧指针继续计费。
	 */
	if (!load)
		runnable = running = 0;

	/*
	 * Now we know we crossed measurement unit boundaries. The *_avg
	 * accrues by two steps:
	 *
	 * Step 1: accumulate *_sum since last_update_time. If we haven't
	 * crossed period boundaries, finish.
	 */
	if (!accumulate_sum(delta, sa, load, runnable, running))
		return 0;

	return 1;
}

/*
 * When syncing *_avg with *_sum, we must take into account the current
 * position in the PELT segment otherwise the remaining part of the segment
 * will be considered as idle time whereas it's not yet elapsed and this will
 * generate unwanted oscillation in the range [1002..1024[.
 *
 * The max value of *_sum varies with the position in the time segment and is
 * equals to :
 *
 *   LOAD_AVG_MAX*y + sa->period_contrib
 *
 * which can be simplified into:
 *
 *   LOAD_AVG_MAX - 1024 + sa->period_contrib
 *
 * because LOAD_AVG_MAX*y == LOAD_AVG_MAX-1024
 *
 * The same care must be taken when a sched entity is added, updated or
 * removed from a cfs_rq and we need to update sched_avg. Scheduler entities
 * and the cfs rq, to which they are attached, have the same position in the
 * time segment because they use the same clock. This means that we can use
 * the period_contrib of cfs_rq when updating the sched_avg of a sched_entity
 * if it's more convenient.
 */
/*
 * 中文释义：除数必须包含当前分段已走过的位置，否则尚未发生的段尾会被误当成空闲，
 * 使满载结果在 1002..1024 间振荡。实体和所属 cfs_rq 共用时钟，因而段位置可对齐。
 * load_avg 额外乘权重，runnable/util 已在 sum 中按容量刻度表示；util 用 WRITE_ONCE
 * 发布，避免并发观察者读到编译器拆分或重复访问的值。
 */
/*
 * 业务背景：sum 只适合内部累加，负载均衡和调频消费者需要按当前窗口长度归一化后的 avg 快照。
 * 入参：sa 是不可空、借用的可写 PELT 状态；load 是纯输入的 load_sum 权重乘数，取值由实体/队列决定。
 * 出参/返回：无直接返回值；原地重算 sa 的 load/runnable/util_avg，无输出指针和 ownership 变化。
 * 注意事项：调用者先成功跨段更新 sum 并串行化 sa；WRITE_ONCE 仅发布单字段，不形成三个 avg 的原子快照。
 */
static __always_inline void
___update_load_avg(struct sched_avg *sa, unsigned long load)
{
	u32 divider = get_pelt_divider(sa);

	/*
	 * Step 2: update *_avg.
	 */
	sa->load_avg = div_u64(load * sa->load_sum, divider);
	sa->runnable_avg = div_u64(sa->runnable_sum, divider);
	WRITE_ONCE(sa->util_avg, sa->util_sum / divider);
}

/*
 * sched_entity:
 *
 *   task:
 *     se_weight()   = se->load.weight
 *     se_runnable() = !!on_rq
 *
 *   group: [ see update_cfs_group() ]
 *     se_weight()   = tg->weight * grq->load_avg / tg->load_avg
 *     se_runnable() = grq->h_nr_runnable
 *
 *   runnable_sum = se_runnable() * runnable = grq->runnable_sum
 *   runnable_avg = runnable_sum
 *
 *   load_sum := runnable
 *   load_avg = se_weight(se) * load_sum
 *
 * cfq_rq:
 *
 *   runnable_sum = \Sum se->avg.runnable_sum
 *   runnable_avg = \Sum se->avg.runnable_avg
 *
 *   load_sum = \Sum se_weight(se) * se->avg.load_sum
 *   load_avg = \Sum se->avg.load_avg
 */

/*
 * 脱队/阻塞实体没有新的 load、runnable 或 running 时间，只衰减已有历史；跨段后
 * 才重算平均值并发 trace。返回 1 表示 avg 数值已同步，0 表示未跨完整段。
 */
/*
 * 业务背景：阻塞实体虽不再产生运行时间，其旧负载仍须随时间衰减，避免唤醒选核使用陈旧峰值。
 * 入参：now 是实体原 rq 时间轴上的 ns 时间戳；se 是不可空、借用且可写的稳定调度实体。
 * 出参/返回：跨段并重算 se->avg/发 trace 返回 1，否则返回 0；不改变 se 的队列关系或 ownership。
 * 注意事项：调用者须以 rq 或 removed-load 协议串行化 se，保持 now 同时钟域；函数不睡眠。
 */

int __update_load_avg_blocked_se(u64 now, struct sched_entity *se)
{
	if (___update_load_sum(now, &se->avg, 0, 0, 0)) {
		___update_load_avg(&se->avg, se_weight(se));
		trace_pelt_se_tp(se);
		return 1;
	}

	return 0;
}

/*
 * 按实体当前 on_rq、层级 runnable 和是否为 cfs_rq->curr 三个状态累计 PELT；跨段
 * 后用实体权重生成 load_avg，并通知利用率变化钩子。调用者须提供同一 rq 时钟域。
 */
/*
 * 业务背景：在队列事件和周期更新点，需要把单个实体的可运行/执行状态转成选核、均衡使用的近期负载。
 * 入参：now 是所属 cfs_rq 的 ns PELT 时间；cfs_rq 是不可空借用队列；se 是附着其上的可写借用实体。
 * 出参/返回：跨段并重算 avg、清 util_est unchanged 标志和发 trace 返回 1，否则返回 0；ownership 不变。
 * 注意事项：调用者持所属 rq 锁并保证二者附着关系稳定；本函数不入队、迁移、分配或睡眠。
 */
int __update_load_avg_se(u64 now, struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	/* on_rq、层级 runnable 与 curr 身份分别形成 load、runnable、running 输入。 */
	if (___update_load_sum(now, &se->avg, !!se->on_rq, se_runnable(se),
				cfs_rq->curr == se)) {

		___update_load_avg(&se->avg, se_weight(se));
		cfs_se_util_change(&se->avg);
		trace_pelt_se_tp(se);
		return 1;
	}

	return 0;
}

/*
 * 更新整个 CFS 运行队列的聚合 PELT：load 是缩放后的总权重，runnable 是层级可运行
 * 实体数，running 表示当前有 CFS 实体执行；聚合 sum 已含权重，故平均阶段乘数为 1。
 */
/*
 * 业务背景：task_group 传播和负载均衡需要 cfs_rq 的聚合近期负载，而不能逐次扫描所有实体。
 * 入参：now 是该队列的 ns PELT 时间；cfs_rq 是不可空、借用且可写的稳定 CFS 运行队列。
 * 出参/返回：跨段并重算 cfs_rq->avg/发 trace 返回 1，否则返回 0；不直接传播组值且 ownership 不变。
 * 注意事项：调用者持所属 rq 锁并已更新时钟；返回 0 仍可能推进 sum，函数不睡眠。
 */
int __update_load_avg_cfs_rq(u64 now, struct cfs_rq *cfs_rq)
{
	/* 聚合队列直接取总权重、层级可运行数以及是否存在当前实体作为三个状态维度。 */
	if (___update_load_sum(now, &cfs_rq->avg,
				scale_load_down(cfs_rq->load.weight),
				cfs_rq->h_nr_runnable,
				cfs_rq->curr != NULL)) {

		___update_load_avg(&cfs_rq->avg, 1);
		trace_pelt_cfs_tp(cfs_rq);
		return 1;
	}

	return 0;
}

/*
 * rt_rq:
 *
 *   util_sum = \Sum se->avg.util_sum but se->avg.util_sum is not tracked
 *   util_sum = cpu_scale * load_sum
 *   runnable_sum = util_sum
 *
 *   load_avg and runnable_avg are not supported and meaningless.
 *
 */

/*
 * RT 队列只跟踪 CPU 是否正在执行 RT 类的二值时间信号；load/runnable/running 同值，
 * 因而 util_sum、runnable_sum 与容量刻度下的 load_sum 等价。返回是否跨段并更新 avg。
 */
/*
 * 业务背景：CPU 容量决策需要知道近期有多少时间被 RT 类占用，而 RT 不维护逐实体 PELT 负载。
 * 入参：now 是 rq 的 ns PELT 时间；rq 是不可空、借用且可写的运行队列；running 仅取 0/1 表示 RT 在执行。
 * 出参/返回：跨段并重算 rq->avg_rt/发 trace 返回 1，否则返回 0；不改变任务或 rq ownership。
 * 注意事项：调用者持 rq 锁且已更新时间；running 必须反映采样区间状态，函数不睡眠。
 */

int update_rt_rq_load_avg(u64 now, struct rq *rq, int running)
{
	/* 二值 running 同时驱动三个 sum，使 avg_rt 只表示 RT 实际占用时间。 */
	if (___update_load_sum(now, &rq->avg_rt,
				running,
				running,
				running)) {

		___update_load_avg(&rq->avg_rt, 1);
		trace_pelt_rt_tp(rq);
		return 1;
	}

	return 0;
}

/*
 * dl_rq:
 *
 *   util_sum = \Sum se->avg.util_sum but se->avg.util_sum is not tracked
 *   util_sum = cpu_scale * load_sum
 *   runnable_sum = util_sum
 *
 *   load_avg and runnable_avg are not supported and meaningless.
 *
 */

/*
 * DL 队列与 RT 使用相同的二值 CPU 时间模型，但状态保存在独立 avg_dl 中，避免不同
 * 调度类的利用率历史混合；调用者以当前 donor 调度类决定 running。
 */
/*
 * 业务背景：deadline 带宽与容量决策要单独观察近期 DL 执行占比，避免与 RT/CFS 历史混合。
 * 入参：now 是 rq 的 ns PELT 时间；rq 是不可空、借用且可写的运行队列；running 仅取 0/1 表示 DL 在执行。
 * 出参/返回：跨段并重算 rq->avg_dl/发 trace 返回 1，否则返回 0；不改变调度类或 ownership。
 * 注意事项：调用者持 rq 锁且已更新时间，running 通常由 donor 调度类推导；函数不睡眠。
 */

int update_dl_rq_load_avg(u64 now, struct rq *rq, int running)
{
	/* 独立推进 avg_dl，防止同一 rq 上不同调度类的近期执行历史互相污染。 */
	if (___update_load_sum(now, &rq->avg_dl,
				running,
				running,
				running)) {

		___update_load_avg(&rq->avg_dl, 1);
		trace_pelt_dl_tp(rq);
		return 1;
	}

	return 0;
}

/* 硬件压力字段和实现只在架构提供该调度信号时编译，关闭配置由 pelt.h 的 stub 保持调用面。 */
#ifdef CONFIG_SCHED_HW_PRESSURE
/*
 * hardware:
 *
 *   load_sum = \Sum se->avg.load_sum but se->avg.load_sum is not tracked
 *
 *   util_avg and runnable_load_avg are not supported and meaningless.
 *
 * Unlike rt/dl utilization tracking that track time spent by a cpu
 * running a rt/dl task through util_avg, the average HW pressure is
 * tracked through load_avg. This is because HW pressure signal is
 * time weighted "delta" capacity unlike util_avg which is binary.
 * "delta capacity" =  actual capacity  -
 *			capped capacity a cpu due to a HW event.
 */

/*
 * 中文释义：硬件压力不是“是否运行”的二值时间，而是硬件事件造成的容量差，因此
 * 用 capacity 作为连续权重累计到 load_avg；配置关闭时本入口不存在，由头文件 stub
 * 令调用者得到未更新结果。
 */
/*
 * 业务背景：热降频等硬件事件会减少可用 CPU 容量，需要把容量损失的时间加权平均提供给调度决策。
 * 入参：now 是未做容量不变缩放的 rq task-clock ns；rq 是不可空可写借用队列；capacity 是
 * SCHED_CAPACITY_SCALE 尺度的非负损失容量，三者均为输入且 ownership 不变。
 * 出参/返回：跨段并重算 rq->avg_hw.load_avg/发 trace 返回 1，否则返回 0；无输出参数。
 * 注意事项：仅 CONFIG_SCHED_HW_PRESSURE 构建；调用者持 rq 锁并已更新时钟，函数不睡眠。
 */

int update_hw_load_avg(u64 now, struct rq *rq, u64 capacity)
{
	/* 容量损失作为连续权重同时送入三个 sum，最终只消费 load_avg。 */
	if (___update_load_sum(now, &rq->avg_hw,
			       capacity,
			       capacity,
			       capacity)) {
		___update_load_avg(&rq->avg_hw, 1);
		trace_pelt_hw_tp(rq);
		return 1;
	}

	return 0;
}
/* 配置开启分支到此结束；关闭分支不会分配 avg_hw 状态或产生 trace。 */
#endif /* CONFIG_SCHED_HW_PRESSURE */

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
/*
 * IRQ:
 *
 *   util_sum = \Sum se->avg.util_sum but se->avg.util_sum is not tracked
 *   util_sum = cpu_scale * load_sum
 *   runnable_sum = util_sum
 *
 *   load_avg and runnable_avg are not supported and meaningless.
 *
 */

/*
 * IRQ 时间不包含在 clock_task/clock_pelt 中，先按频率与 CPU 容量换算成实际计算量，
 * 再把 [上次更新, clock-running) 当普通上下文衰减、把末尾 running 当中断忙碌时间。
 * 这是偏保守的时间放置近似；任一阶段跨段就重算平均并发 trace。
 */
/*
 * 业务背景：IRQ/steal 占用会挤压任务容量，却不在 task clock 中，必须单独投影到近期利用率信号。
 * 入参：rq 是不可空、借用且可写的运行队列；running 是从上次更新以来的非负 IRQ 时间，单位 ns。
 * 出参/返回：返回两个 sum 更新的 0..2 之和；正值时重算 rq->avg_irq 并发 trace，ownership 不变。
 * 注意事项：仅 CONFIG_HAVE_SCHED_AVG_IRQ 构建；调用者持 rq 锁、更新 rq->clock 且保证 running 不超增量。
 */

int update_irq_load_avg(struct rq *rq, u64 running)
{
	int ret = 0;

	/*
	 * We can't use clock_pelt because IRQ time is not accounted in
	 * clock_task. Instead we directly scale the running time to
	 * reflect the real amount of computation
	 */
	/* 中文释义：两次 cap_scale 消除当前频率和异构 CPU 原始算力差异。 */
	running = cap_scale(running, arch_scale_freq_capacity(cpu_of(rq)));
	running = cap_scale(running, arch_scale_cpu_capacity(cpu_of(rq)));

	/*
	 * We know the time that has been used by interrupt since last update
	 * but we don't when. Let be pessimistic and assume that interrupt has
	 * happened just before the update. This is not so far from reality
	 * because interrupt will most probably wake up task and trig an update
	 * of rq clock during which the metric is updated.
	 * We start to decay with normal context time and then we add the
	 * interrupt context time.
	 * We can safely remove running from rq->clock because
	 * rq->clock += delta with delta >= running
	 */
	/*
	 * 中文释义：无法还原中断在区间内的精确位置，按“紧邻本次更新”放置会让近期
	 * 压力不被过早衰减；rq 时钟增量覆盖 running，故减法不会倒退。
	 */
	ret = ___update_load_sum(rq->clock - running, &rq->avg_irq,
				0,
				0,
				0);
	ret += ___update_load_sum(rq->clock, &rq->avg_irq,
				1,
				1,
				1);

	/* 两段任一跨界都要用合成后的 sum 统一重算一次 avg，避免中间态对外可见。 */
	if (ret) {
		___update_load_avg(&rq->avg_irq, 1);
		trace_pelt_irq_tp(rq);
	}

	return ret;
}
#endif /* CONFIG_HAVE_SCHED_AVG_IRQ */

/*
 * Load avg and utiliztion metrics need to be updated periodically and before
 * consumption. This function updates the metrics for all subsystems except for
 * the fair class. @rq must be locked and have its clock updated.
 */
/*
 * 中文释义：在持 rq 锁且时钟已更新的前提下，一次推进 RT、DL、硬件压力和 IRQ
 * 四类 PELT。位或而非逻辑或保证所有更新都执行，返回任一子系统是否跨段；硬件
 * 压力用 task 时钟以保持真实时间权重，不参与 PELT 频率不变性缩放。
 */
/*
 * 业务背景：调度核心在一个 rq 时钟更新点需要同步推进所有非 CFS 压力信号，供容量和调频统一消费。
 * 入参：rq 是不可空、借用且可写的当前运行队列；调用者已持 rq 锁并更新 rq/task/PELT 时钟。
 * 出参/返回：任一 RT/DL/HW/IRQ 信号跨段返回 true，否则 false；四个 helper 均执行且 ownership 不变。
 * 注意事项：不更新 fair PELT；配置关闭的 HW/IRQ helper 为无副作用 stub，按位 OR 不得改成短路 OR。
 */
bool update_other_load_avgs(struct rq *rq)
{
	/* 先在同一锁内快照公共时间、当前 donor 类和硬件压力，供四路更新共享。 */
	u64 now = rq_clock_pelt(rq);
	const struct sched_class *curr_class = rq->donor->sched_class;
	unsigned long hw_pressure = arch_scale_hw_pressure(cpu_of(rq));

	lockdep_assert_rq_held(rq);

	/* hw_pressure doesn't care about invariance */
	return update_rt_rq_load_avg(now, rq, curr_class == &rt_sched_class) |
		update_dl_rq_load_avg(now, rq, curr_class == &dl_sched_class) |
		update_hw_load_avg(rq_clock_task(rq), rq, hw_pressure) |
		update_irq_load_avg(rq, 0);
}
