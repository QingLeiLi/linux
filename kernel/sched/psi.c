// SPDX-License-Identifier: GPL-2.0
/*
 * Pressure stall information for CPU, memory and IO
 *
 * Copyright (c) 2018 Facebook, Inc.
 * Author: Johannes Weiner <hannes@cmpxchg.org>
 *
 * Polling support by Suren Baghdasaryan <surenb@google.com>
 * Copyright (c) 2018 Google, Inc.
 *
 * When CPU, memory and IO are contended, tasks experience delays that
 * reduce throughput and introduce latencies into the workload. Memory
 * and IO contention, in addition, can cause a full loss of forward
 * progress in which the CPU goes idle.
 *
 * This code aggregates individual task delays into resource pressure
 * metrics that indicate problems with both workload health and
 * resource utilization.
 *
 *			Model
 *
 * The time in which a task can execute on a CPU is our baseline for
 * productivity. Pressure expresses the amount of time in which this
 * potential cannot be realized due to resource contention.
 *
 * This concept of productivity has two components: the workload and
 * the CPU. To measure the impact of pressure on both, we define two
 * contention states for a resource: SOME and FULL.
 *
 * In the SOME state of a given resource, one or more tasks are
 * delayed on that resource. This affects the workload's ability to
 * perform work, but the CPU may still be executing other tasks.
 *
 * In the FULL state of a given resource, all non-idle tasks are
 * delayed on that resource such that nobody is advancing and the CPU
 * goes idle. This leaves both workload and CPU unproductive.
 *
 *	SOME = nr_delayed_tasks != 0
 *	FULL = nr_delayed_tasks != 0 && nr_productive_tasks == 0
 *
 * What it means for a task to be productive is defined differently
 * for each resource. For IO, productive means a running task. For
 * memory, productive means a running task that isn't a reclaimer. For
 * CPU, productive means an on-CPU task.
 *
 * Naturally, the FULL state doesn't exist for the CPU resource at the
 * system level, but exist at the cgroup level. At the cgroup level,
 * FULL means all non-idle tasks in the cgroup are delayed on the CPU
 * resource which is being used by others outside of the cgroup or
 * throttled by the cgroup cpu.max configuration.
 *
 * The percentage of wall clock time spent in those compound stall
 * states gives pressure numbers between 0 and 100 for each resource,
 * where the SOME percentage indicates workload slowdowns and the FULL
 * percentage indicates reduced CPU utilization:
 *
 *	%SOME = time(SOME) / period
 *	%FULL = time(FULL) / period
 *
 *			Multiple CPUs
 *
 * The more tasks and available CPUs there are, the more work can be
 * performed concurrently. This means that the potential that can go
 * unrealized due to resource contention *also* scales with non-idle
 * tasks and CPUs.
 *
 * Consider a scenario where 257 number crunching tasks are trying to
 * run concurrently on 256 CPUs. If we simply aggregated the task
 * states, we would have to conclude a CPU SOME pressure number of
 * 100%, since *somebody* is waiting on a runqueue at all
 * times. However, that is clearly not the amount of contention the
 * workload is experiencing: only one out of 256 possible execution
 * threads will be contended at any given time, or about 0.4%.
 *
 * Conversely, consider a scenario of 4 tasks and 4 CPUs where at any
 * given time *one* of the tasks is delayed due to a lack of memory.
 * Again, looking purely at the task state would yield a memory FULL
 * pressure number of 0%, since *somebody* is always making forward
 * progress. But again this wouldn't capture the amount of execution
 * potential lost, which is 1 out of 4 CPUs, or 25%.
 *
 * To calculate wasted potential (pressure) with multiple processors,
 * we have to base our calculation on the number of non-idle tasks in
 * conjunction with the number of available CPUs, which is the number
 * of potential execution threads. SOME becomes then the proportion of
 * delayed tasks to possible threads, and FULL is the share of possible
 * threads that are unproductive due to delays:
 *
 *	threads = min(nr_nonidle_tasks, nr_cpus)
 *	   SOME = min(nr_delayed_tasks / threads, 1)
 *	   FULL = (threads - min(nr_productive_tasks, threads)) / threads
 *
 * For the 257 number crunchers on 256 CPUs, this yields:
 *
 *	threads = min(257, 256)
 *	   SOME = min(1 / 256, 1)             = 0.4%
 *	   FULL = (256 - min(256, 256)) / 256 = 0%
 *
 * For the 1 out of 4 memory-delayed tasks, this yields:
 *
 *	threads = min(4, 4)
 *	   SOME = min(1 / 4, 1)               = 25%
 *	   FULL = (4 - min(3, 4)) / 4         = 25%
 *
 * [ Substitute nr_cpus with 1, and you can see that it's a natural
 *   extension of the single-CPU model. ]
 *
 *			Implementation
 *
 * To assess the precise time spent in each such state, we would have
 * to freeze the system on task changes and start/stop the state
 * clocks accordingly. Obviously that doesn't scale in practice.
 *
 * Because the scheduler aims to distribute the compute load evenly
 * among the available CPUs, we can track task state locally to each
 * CPU and, at much lower frequency, extrapolate the global state for
 * the cumulative stall times and the running averages.
 *
 * For each runqueue, we track:
 *
 *	   tSOME[cpu] = time(nr_delayed_tasks[cpu] != 0)
 *	   tFULL[cpu] = time(nr_delayed_tasks[cpu] && !nr_productive_tasks[cpu])
 *	tNONIDLE[cpu] = time(nr_nonidle_tasks[cpu] != 0)
 *
 * and then periodically aggregate:
 *
 *	tNONIDLE = sum(tNONIDLE[i])
 *
 *	   tSOME = sum(tSOME[i] * tNONIDLE[i]) / tNONIDLE
 *	   tFULL = sum(tFULL[i] * tNONIDLE[i]) / tNONIDLE
 *
 *	   %SOME = tSOME / period
 *	   %FULL = tFULL / period
 *
 * This gives us an approximation of pressure that is practical
 * cost-wise, yet way more sensitive and accurate than periodic
 * sampling of the aggregate task states would be.
 */
/*
 * PSI 把每个 task 的 RUNNING/ONCPU/IOWAIT/MEMSTALL 状态先在本 CPU、各 cgroup
 * 祖先中记账，再低频归一化为 SOME/FULL 压力。写端随 rq 状态转换执行并用
 * per-CPU seqcount 发布；聚合读者可跨 CPU 得到近似但各 CPU 内一致的快照。
 *
 * 普通平均触发器复用 delayed_work；特权短窗口触发器使用 psimon kthread、
 * timer 与 rtpoll mutex。trigger 从链表摘除后先清 RCU 发布指针并等待 grace
 * period，才停止线程和释放内存；poll 以 release/acquire 发布 trigger，以
 * cmpxchg 独占一次 event。cgroup 移动持 task rq 锁，并按旧 flags 从旧树扣除、
 * 发布新 css_set、再向新树加入，避免压力状态同时留在两棵层级中。
 */
#include <linux/sched/clock.h>
#include <linux/workqueue.h>
#include <linux/psi.h>
#include "sched.h"

/* psi_bug 记录内部计数不变量是否曾被破坏；static key 则把全局与 cgroup 开关移出热路径分支。 */
static int psi_bug __read_mostly;

DEFINE_STATIC_KEY_FALSE(psi_disabled);
static DEFINE_STATIC_KEY_TRUE(psi_cgroups_enabled);

#ifdef CONFIG_PSI_DEFAULT_DISABLED
static bool psi_enable;
#else
static bool psi_enable = true;
#endif
/* 解析启动参数 psi=bool；成功返回 1 表示参数已消费，失败返回 0。 */
/*
 * 业务背景：早期启动参数需要在 static key 初始化前决定系统是否启用 PSI。
 * 入参：str 是不可空、NUL 结尾的可读启动参数值，借用且不转移 ownership。
 * 出参/返回：布尔解析成功返回 1并写 psi_enable，失败返回 0；无输出参数。
 * 注意事项：仅 __init 阶段调用且不报告具体 errno；失败时全局 enable 保持原值。
 */
static int __init setup_psi(char *str)
{
	return kstrtobool(str, &psi_enable) == 0;
}
__setup("psi=", setup_psi);

/* 三组衰减常量用定点数描述每个两秒采样周期后的保留比例。 */
/* Running averages - we need to be higher-res than loadavg */
#define PSI_FREQ	(2*HZ+1)	/* 2 sec intervals */
#define EXP_10s		1677		/* 1/exp(2s/10s) as fixed-point */
#define EXP_60s		1981		/* 1/exp(2s/60s) */
#define EXP_300s	2034		/* 1/exp(2s/300s) */

/* PSI trigger definitions */
#define WINDOW_MAX_US 10000000	/* Max window size is 10s */
#define UPDATES_PER_WINDOW 10	/* 10 updates per window */

/* 普通平均按 PSI_FREQ 聚合，触发器窗口再按窗口大小派生更短的检查周期。 */
/* Sampling frequency in nanoseconds */
static u64 psi_period __read_mostly;

/* System-level pressure and stall tracking */
static DEFINE_PER_CPU(struct psi_group_cpu, system_group_pcpu);
struct psi_group psi_system = {
	.pcpu = &system_group_pcpu,
};

static DEFINE_PER_CPU(seqcount_t, psi_seq) = SEQCNT_ZERO(psi_seq);

/* 开始 @cpu PSI 状态写序列；调用者已由 rq/本 CPU 上下文串行。 */
/*
 * 业务背景：PSI per-CPU 多字段更新需让无锁聚合读者检测并丢弃跨代组合。
 * 入参：cpu 是有效 CPU 编号，指定目标 per-CPU seqcount。
 * 出参/返回：无直接返回值、无输出参数；开始一次 seqcount 写段。
 * 注意事项：调用者持目标 rq 锁或等价本地串行；必须与 psi_write_end 同 CPU 严格配对且不可嵌套。
 */
static inline void psi_write_begin(int cpu)
{
	write_seqcount_begin(per_cpu_ptr(&psi_seq, cpu));
}

/* 结束 @cpu PSI 写序列，使读者能检测并发修改。 */
/*
 * 业务背景：per-CPU PSI 状态全部写完后要提交新代，允许聚合读者取得一致快照。
 * 入参：cpu 是与对应 begin 相同的有效 CPU 编号。
 * 出参/返回：无直接返回值、无输出参数；结束并发布 seqcount 写段。
 * 注意事项：必须和 psi_write_begin 严格配对；错 CPU 或漏调用会令读者永久重试或接受错误代。
 */
static inline void psi_write_end(int cpu)
{
	write_seqcount_end(per_cpu_ptr(&psi_seq, cpu));
}

/* 读取 @cpu seqcount 起始序号，供一致快照循环使用。 */
/*
 * 业务背景：聚合器读取一个 CPU 的 times/mask/start/tasks 前需取得可验证的版本号。
 * 入参：cpu 是有效 CPU 编号，纯输入。
 * 出参/返回：返回该 CPU seqcount 读序号；无输出参数或 ownership 变化。
 * 注意事项：返回序号必须传给同 CPU 的 psi_read_retry，单独调用不保证后续字段稳定。
 */
static inline u32 psi_read_begin(int cpu)
{
	return read_seqcount_begin(per_cpu_ptr(&psi_seq, cpu));
}

/* 若 @cpu 在快照期间发生写入则返回 true，要求调用者重读全部字段。 */
/*
 * 业务背景：聚合器读完多字段后需验证期间是否有 writer，以免拼接两代 PSI 状态。
 * 入参：cpu 是与 begin 相同的有效 CPU；seq 是 psi_read_begin 返回的输入序号。
 * 出参/返回：需要整体重试返回 true，否则 false；无输出参数或状态副作用。
 * 注意事项：只验证一致性，不冻结下一次更新；错配 CPU/seq 会破坏快照保证。
 */
static inline bool psi_read_retry(int cpu, u32 seq)
{
	return read_seqcount_retry(per_cpu_ptr(&psi_seq, cpu), seq);
}

static void psi_avgs_work(struct work_struct *work);

static void poll_timer_fn(struct timer_list *t);

/* 初始化 group 时钟、平均/rtpoll trigger 链表、锁、work、timer 与 RCU task 指针。 */
/*
 * 业务背景：system/cgroup PSI group 分配后要建立普通平均与实时 trigger 两套完整控制状态。
 * 入参：group 是不可空、由调用者独占且存储已分配的输入/输出对象。
 * 出参/返回：无直接返回值、无输出参数；初始化时钟、锁、链表、work/timer/waitqueue 和 RCU 指针。
 * 注意事项：可初始化 mutex/timer但不启动线程；同一对象只能初始化一次，pcpu/parent 由外层管理。
 */
static void group_init(struct psi_group *group)
{
	/* 普通平均和实时轮询共享 group，但各自持锁、游标和调度状态，不能交叉串行化。 */
	group->enabled = true;
	group->avg_last_update = sched_clock();
	group->avg_next_update = group->avg_last_update + psi_period;
	mutex_init(&group->avgs_lock);

	/* Init avg trigger-related members */
	INIT_LIST_HEAD(&group->avg_triggers);
	memset(group->avg_nr_triggers, 0, sizeof(group->avg_nr_triggers));
	INIT_DELAYED_WORK(&group->avgs_work, psi_avgs_work);

	/* Init rtpoll trigger-related members */
	/* U32_MAX/ULLONG_MAX 表示尚无实时 trigger，因此不会无故唤醒轮询线程。 */
	atomic_set(&group->rtpoll_scheduled, 0);
	mutex_init(&group->rtpoll_trigger_lock);
	INIT_LIST_HEAD(&group->rtpoll_triggers);
	group->rtpoll_min_period = U32_MAX;
	group->rtpoll_next_update = ULLONG_MAX;
	init_waitqueue_head(&group->rtpoll_wait);
	timer_setup(&group->rtpoll_timer, poll_timer_fn, 0);
	rcu_assign_pointer(group->rtpoll_task, NULL);
}

/* 启动期配置 static key、采样周期并初始化 system group；禁用时不分配后台工作。 */
/*
 * 业务背景：启动时根据 psi= 和 cgroup 配置发布热路径 static key，并准备系统级聚合器。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；禁用时切换 static key，启用时设置周期并初始化 system group。
 * 注意事项：仅 __init 单次调用；static-key 修改影响全系统热路径，禁用分支不会初始化后台对象。
 */
void __init psi_init(void)
{
	/* 全局禁用时同时裁掉 system 与 cgroup 热路径，后续不应触碰未初始化 group。 */
	if (!psi_enable) {
		static_branch_enable(&psi_disabled);
		static_branch_disable(&psi_cgroups_enabled);
		return;
	}

	if (!cgroup_psi_enabled())
		static_branch_disable(&psi_cgroups_enabled);

	/* 所有平均窗口使用同一个纳秒周期，初始化后只读。 */
	psi_period = jiffies_to_nsecs(PSI_FREQ);
	group_init(&psi_system);
}

/* 从 task 计数与 ONCPU 位派生 IO/MEM/CPU SOME/FULL 和 NONIDLE 状态位图。 */
/*
 * 业务背景：原始 running/iowait/memstall 计数需转换为用户可见的 SOME/FULL 复合压力状态。
 * 入参：tasks 是不可空、含 NR_PSI_TASK_COUNTS 个计数的只读借用数组；state_mask 提供 PSI_ONCPU 输入位。
 * 出参/返回：返回加入 IO/MEM/CPU/NONIDLE 派生位的 mask；无输出参数或 ownership 变化。
 * 注意事项：计数须来自同一 CPU/同一 seq 代；函数不校验下溢，CPU FULL 在 cgroup 层才有实际意义。
 */
static u32 test_states(unsigned int *tasks, u32 state_mask)
{
	const bool oncpu = state_mask & PSI_ONCPU;

	/* 有等待者即产生 SOME；没有 runnable 任务时，IO 等待阻塞了全部非 idle 工作。 */
	if (tasks[NR_IOWAIT]) {
		state_mask |= BIT(PSI_IO_SOME);
		if (!tasks[NR_RUNNING])
			state_mask |= BIT(PSI_IO_FULL);
	}

	if (tasks[NR_MEMSTALL]) {
		/* 所有 runnable 都处于 reclaim/缺页停顿时，内存压力升级为 FULL。 */
		state_mask |= BIT(PSI_MEM_SOME);
		if (tasks[NR_RUNNING] == tasks[NR_MEMSTALL_RUNNING])
			state_mask |= BIT(PSI_MEM_FULL);
	}

	if (tasks[NR_RUNNING] > oncpu)
		state_mask |= BIT(PSI_CPU_SOME);

	/* 有 runnable 却无人占用 CPU，表示该组可运行工作全部在等处理器。 */
	if (tasks[NR_RUNNING] && !oncpu)
		state_mask |= BIT(PSI_CPU_FULL);

	if (tasks[NR_IOWAIT] || tasks[NR_MEMSTALL] || tasks[NR_RUNNING])
		state_mask |= BIT(PSI_NONIDLE);

	return state_mask;
}

/*
 * seqcount 下取得 @cpu 当前累计时间并换算为相对上次 @aggregator 的 u32 增量；
 * 输出 changed 位，平均 worker 自身睡眠时避免无穷自唤醒。
 */
/*
 * 业务背景：低频聚合器需从一个 CPU 的累计桶取出自上次该 aggregator 读取以来的 u32 增量。
 * 入参：group 是不可空借用组；cpu 是有效 CPU；aggregator 选 AVGS/POLL；times 和 pchanged_states 是不可空输出。
 * 出参/返回：无直接返回值；覆盖各状态增量和 changed 位，并推进该 aggregator 的 times_prev 游标。
 * 注意事项：seqcount 保证 per-CPU 快照；times_prev 由对应聚合锁串行，u32 小桶依赖频繁读取避免回绕。
 */
static void get_recent_times(struct psi_group *group, int cpu,
			     enum psi_aggregators aggregator, u32 *times,
			     u32 *pchanged_states)
{
	struct psi_group_cpu *groupc = per_cpu_ptr(group->pcpu, cpu);
	int current_cpu = raw_smp_processor_id();
	/* tasks 只在读取当前 CPU 时用于识别 worker 自身，远端 CPU 依赖 NONIDLE 增量。 */
	unsigned int tasks[NR_PSI_TASK_COUNTS];
	u64 now, state_start;
	enum psi_states s;
	unsigned int seq;
	u32 state_mask;

	/* 输出缓冲总是由本函数完整覆盖，changed 位从空集合开始累积。 */
	*pchanged_states = 0;

	/* Snapshot a coherent view of the CPU state */
	do {
		seq = psi_read_begin(cpu);
		/* cpu_clock 与累计桶必须处在同一 seq 代，才能补齐仍在进行的状态时长。 */
		now = cpu_clock(cpu);
		memcpy(times, groupc->times, sizeof(groupc->times));
		state_mask = groupc->state_mask;
		state_start = groupc->state_start;
		if (cpu == current_cpu)
			memcpy(tasks, groupc->tasks, sizeof(groupc->tasks));
	} while (psi_read_retry(cpu, seq));

	/* Calculate state time deltas against the previous snapshot */
	/* 每个状态都先补当前活跃尾段，再减去对应 aggregator 的独立历史游标。 */
	for (s = 0; s < NR_PSI_STATES; s++) {
		u32 delta;
		/*
		 * In addition to already concluded states, we also
		 * incorporate currently active states on the CPU,
		 * since states may last for many sampling periods.
		 *
		 * This way we keep our delta sampling buckets small
		 * (u32) and our reported pressure close to what's
		 * actually happening.
		 */
		if (state_mask & (1 << s))
			times[s] += now - state_start;

		/* 两套 aggregator 各自推进游标，短窗读取不会吃掉普通平均尚未消费的增量。 */
		delta = times[s] - groupc->times_prev[aggregator][s];
		groupc->times_prev[aggregator][s] = times[s];

		times[s] = delta;
		if (delta)
			*pchanged_states |= (1 << s);
	}

	/*
	 * When collect_percpu_times() from the avgs_work, we don't want to
	 * re-arm avgs_work when all CPUs are IDLE. But the current CPU running
	 * this avgs_work is never IDLE, cause avgs_work can't be shut off.
	 * So for the current CPU, we need to re-arm avgs_work only when
	 * (NR_RUNNING > 1 || NR_IOWAIT > 0 || NR_MEMSTALL > 0), for other CPUs
	 * we can just check PSI_NONIDLE delta.
	 */
	if (current_work() == &group->avgs_work.work) {
		bool reschedule;

		/* 当前 CPU 至少包含本 worker，故只有额外任务/停顿才代表真实非 idle 活动。 */
		if (cpu == current_cpu)
			reschedule = tasks[NR_RUNNING] +
				     tasks[NR_IOWAIT] +
				     tasks[NR_MEMSTALL] > 1;
		else
			reschedule = *pchanged_states & (1 << PSI_NONIDLE);

		if (reschedule)
			*pchanged_states |= PSI_STATE_RESCHEDULE;
	}
}

/* 将一个 active 样本及漏掉的零样本推进 10/60/300 秒定点衰减平均。 */
/*
 * 业务背景：PSI avg10/60/300 要按固定 2 秒周期衰减，并在 worker 停摆后补入缺失的零样本。
 * 入参：avg 是不可空、三个定点平均的输入/输出数组；missed_periods 是漏期数；time/period 是纳秒样本与周期。
 * 出参/返回：无直接返回值、无输出参数；原地更新三个平均值。
 * 注意事项：period 必须非零且 time 通常不超过 period；乘法范围由 PSI 周期约束，整数计算有舍入。
 */
static void calc_avgs(unsigned long avg[3], int missed_periods,
		      u64 time, u64 period)
{
	unsigned long pct;

	/* Fill in zeroes for periods of no activity */
	if (missed_periods) {
		/* 停机期间没有样本等价于连续零压力，需逐期衰减而不是冻结旧高值。 */
		avg[0] = calc_load_n(avg[0], EXP_10s, 0, missed_periods);
		avg[1] = calc_load_n(avg[1], EXP_60s, 0, missed_periods);
		avg[2] = calc_load_n(avg[2], EXP_300s, 0, missed_periods);
	}

	/* Sample the most recent active period */
	pct = div_u64(time * 100, period);
	/* calc_load 使用 FIXED_1 定点比例，避免调度统计热路径引入浮点运算。 */
	pct *= FIXED_1;
	avg[0] = calc_load(avg[0], EXP_10s, pct);
	avg[1] = calc_load(avg[1], EXP_60s, pct);
	avg[2] = calc_load(avg[2], EXP_300s, pct);
}

/* 汇总 possible CPU 增量，以 nonidle 时间加权并累计到 group total。 */
/*
 * 业务背景：per-CPU PSI 状态要按各 CPU 非 idle 时间加权，近似还原多处理器墙钟压力。
 * 入参：group 是不可空输入/输出组；aggregator 选 AVGS/POLL；pchanged_states 是可空 changed 位输出。
 * 出参/返回：无直接返回值；累加 group total、推进 per-CPU 游标，并可覆盖 changed states。
 * 注意事项：调用者持对应聚合 mutex；遍历 possible CPU且读取为近似快照，nonidle 为零时以 1 防除零。
 */
static void collect_percpu_times(struct psi_group *group,
				 enum psi_aggregators aggregator,
				 u32 *pchanged_states)
{
	u64 deltas[NR_PSI_STATES - 1] = { 0, };
	unsigned long nonidle_total = 0;
	u32 changed_states = 0;
	int cpu;
	int s;

	/* deltas 保持纳秒乘权重的中间量，循环结束前不能提前归一化。 */
	/*
	 * Collect the per-cpu time buckets and average them into a
	 * single time sample that is normalized to wall clock time.
	 *
	 * For averaging, each CPU is weighted by its non-idle time in
	 * the sampling period. This eliminates artifacts from uneven
	 * loading, or even entirely idle CPUs.
	 */
	for_each_possible_cpu(cpu) {
		u32 times[NR_PSI_STATES];
		u32 nonidle;
		u32 cpu_changed_states;

		get_recent_times(group, cpu, aggregator, times,
				&cpu_changed_states);
		/* changed 位取并集，用于决定后续 trigger 检查与 worker 是否续排。 */
		changed_states |= cpu_changed_states;

		nonidle = nsecs_to_jiffies(times[PSI_NONIDLE]);
		nonidle_total += nonidle;

		for (s = 0; s < PSI_NONIDLE; s++)
			/* 先乘各 CPU 的 nonidle 权重，循环后再统一除以总权重。 */
			deltas[s] += (u64)times[s] * nonidle;
	}

	/*
	 * Integrate the sample into the running statistics that are
	 * reported to userspace: the cumulative stall times and the
	 * decaying averages.
	 *
	 * Pressure percentages are sampled at PSI_FREQ. We might be
	 * called more often when the user polls more frequently than
	 * that; we might be called less often when there is no task
	 * activity, thus no data, and clock ticks are sporadic. The
	 * below handles both.
	 */

	/* total= */
	/* NONIDLE 只作归一化权重，不作为用户可见 pressure total 累计。 */
	for (s = 0; s < NR_PSI_STATES - 1; s++)
		group->total[aggregator][s] +=
				div_u64(deltas[s], max(nonidle_total, 1UL));

	if (pchanged_states)
		*pchanged_states = changed_states;
}

/* Trigger tracking window manipulations */
/* 以当前累计值重置近似滑窗，并保存上一完整窗口增长供下一窗口插值。 */
/*
 * 业务背景：trigger 用常量内存近似滑窗，跨窗时要保存旧增长供下一窗口线性插值。
 * 入参：win 是不可空输入/输出窗口；now 是纳秒起点；value 是累计压力值；prev_growth 是上窗纳秒增长。
 * 出参/返回：无直接返回值、无输出参数；覆盖窗口起点、基值和历史增长。
 * 注意事项：调用者持 trigger 所属 mutex；now/value 必须单调同域，ownership 不转移。
 */
static void window_reset(struct psi_window *win, u64 now, u64 value,
			 u64 prev_growth)
{
	win->start_time = now;
	win->start_value = value;
	win->prev_growth = prev_growth;
}

/*
 * PSI growth tracking window update and growth calculation routine.
 *
 * This approximates a sliding tracking window by interpolating
 * partially elapsed windows using historical growth data from the
 * previous intervals. This minimizes memory requirements (by not storing
 * all the intermediate values in the previous window) and simplifies
 * the calculations. It works well because PSI signal changes only in
 * positive direction and over relatively small window sizes the growth
 * is close to linear.
 */
/* 返回当前窗口增长；未满窗口时线性叠加上一窗口的剩余比例。 */
/*
 * 业务背景：trigger 阈值需要近似最近一个滑动窗口增长，而非只看固定窗口当前半段。
 * 入参：win 是不可空输入/输出窗口；now 是当前纳秒；value 是单调累计压力纳秒。
 * 出参/返回：返回近似窗口增长纳秒；跨窗时同时重置 win，无输出参数。
 * 注意事项：win->size 必须非零；调用者持对应 mutex，时钟/累计倒退会造成无符号异常。
 */
static u64 window_update(struct psi_window *win, u64 now, u64 value)
{
	u64 elapsed;
	u64 growth;

	elapsed = now - win->start_time;
	growth = value - win->start_value;
	/* 完整跨过窗口时保留本窗增长；未跨窗时用上窗尾部补齐滑动窗口左侧。 */
	/*
	 * After each tracking window passes win->start_value and
	 * win->start_time get reset and win->prev_growth stores
	 * the average per-window growth of the previous window.
	 * win->prev_growth is then used to interpolate additional
	 * growth from the previous window assuming it was linear.
	 */
	if (elapsed > win->size)
		window_reset(win, now, value, growth);
	else {
		u32 remaining;

		remaining = win->size - elapsed;
		growth += div64_u64(win->prev_growth * remaining, win->size);
	}

	return growth;
}

/* 在对应 trigger mutex 下检查阈值并每窗口至多发布一个 event。 */
/*
 * 业务背景：聚合完成后要为每个 watcher 计算窗口增长、保留延迟事件并按窗口限速通知。
 * 入参：group 是不可空输入/输出组；now 是当前纳秒；aggregator 选择 AVGS 或 POLL 链和 total 游标。
 * 出参/返回：无直接返回值、无输出参数；可能设置/清 pending、发布 event并唤醒 kernfs/poll 等待者。
 * 注意事项：调用者持对应 trigger mutex；cmpxchg 保证事件只被发布一次，非法 aggregator 会选 POLL 分支。
 */
static void update_triggers(struct psi_group *group, u64 now,
						   enum psi_aggregators aggregator)
{
	struct psi_trigger *t;
	u64 *total = group->total[aggregator];
	struct list_head *triggers;
	u64 *aggregator_total;

	if (aggregator == PSI_AVGS) {
		/* 每种聚合器只访问自己的 trigger 链和上次已检查累计值。 */
		triggers = &group->avg_triggers;
		aggregator_total = group->avg_total;
	} else {
		triggers = &group->rtpoll_triggers;
		aggregator_total = group->rtpoll_total;
	}

	/*
	 * On subsequent updates, calculate growth deltas and let
	 * watchers know when their specified thresholds are exceeded.
	 */
	list_for_each_entry(t, triggers, node) {
		u64 growth;
		bool new_stall;

		new_stall = aggregator_total[t->state] != total[t->state];

		/* Check for stall activity or a previous threshold breach */
		/* pending_event 让限速期内的越阈事件延迟交付，而不是静默丢弃。 */
		if (!new_stall && !t->pending_event)
			continue;
		/*
		 * Check for new stall activity, as well as deferred
		 * events that occurred in the last window after the
		 * trigger had already fired (we want to ratelimit
		 * events without dropping any).
		 */
		if (new_stall) {
			/* Calculate growth since last update */
			growth = window_update(&t->win, now, total[t->state]);
			/* 首次越阈后保持 pending，直到窗口限速允许真正发布。 */
			if (!t->pending_event) {
				if (growth < t->threshold)
					continue;

				t->pending_event = true;
			}
		}
		/* Limit event signaling to once per window */
		if (now < t->last_event_time + t->win.size)
			continue;

		/* Generate an event */
		/* cmpxchg 与 poll 消费端竞争；只有 0→1 的发布者负责一次唤醒。 */
		if (cmpxchg(&t->event, 0, 1) == 0) {
			if (t->of)
				kernfs_notify(t->of->kn);
			else
				wake_up_interruptible(&t->event_wait);
		}
		t->last_event_time = now;
		/* Reset threshold breach flag once event got generated */
		t->pending_event = false;
	}
}

/* 推进 group 的 avg/total 游标，限制跨周期误差不超过 100%，返回下次固定期限。 */
/*
 * 业务背景：周期 tick 抖动会造成漏期和跨桶超额，需要固定期限推进且把超额压力延后报告。
 * 入参：group 是不可空输入/输出组；now 是当前 sched_clock 纳秒且不早于合理更新基线。
 * 出参/返回：返回下一次固定 avg 更新期限纳秒；更新 avg_last/avg_total/三个衰减平均。
 * 注意事项：调用者持 avgs_lock；样本钳到 period 避免超过 100%，余量留在 total 供未来周期消费。
 */
static u64 update_averages(struct psi_group *group, u64 now)
{
	unsigned long missed_periods = 0;
	u64 expires, period;
	u64 avg_next_update;
	int s;

	/* avgX= */
	expires = group->avg_next_update;
	/* 超过一个完整周期才计为 missed；当前到期周期仍由本次 active sample 消费。 */
	if (now - expires >= psi_period)
		missed_periods = div_u64(now - expires, psi_period);

	/*
	 * The periodic clock tick can get delayed for various
	 * reasons, especially on loaded systems. To avoid clock
	 * drift, we schedule the clock in fixed psi_period intervals.
	 * But the deltas we sample out of the per-cpu buckets above
	 * are based on the actual time elapsing between clock ticks.
	 */
	avg_next_update = expires + ((1 + missed_periods) * psi_period);
	/* 去掉完整漏期后，period 只表示本次需要归一化的活动采样跨度。 */
	period = now - (group->avg_last_update + (missed_periods * psi_period));
	group->avg_last_update = now;

	for (s = 0; s < NR_PSI_STATES - 1; s++) {
		u32 sample;

		sample = group->total[PSI_AVGS][s] - group->avg_total[s];
		/* avg_total 只推进实际消费量，超出本周期的部分自然留给下一次。 */
		/*
		 * Due to the lockless sampling of the time buckets,
		 * recorded time deltas can slip into the next period,
		 * which under full pressure can result in samples in
		 * excess of the period length.
		 *
		 * We don't want to report non-sensical pressures in
		 * excess of 100%, nor do we want to drop such events
		 * on the floor. Instead we punt any overage into the
		 * future until pressure subsides. By doing this we
		 * don't underreport the occurring pressure curve, we
		 * just report it delayed by one period length.
		 *
		 * The error isn't cumulative. As soon as another
		 * delta slips from a period P to P+1, by definition
		 * it frees up its time T in P.
		 */
		if (sample > period)
			sample = period;
		group->avg_total[s] += sample;
		calc_avgs(group->avg[s], missed_periods, sample, period);
	}

	return avg_next_update;
}

/* delayed_work 聚合平均与普通触发器；全组无活动时停止自重排。 */
/*
 * 业务背景：普通 PSI 平均和非特权 2 秒 trigger 共用 delayed_work，空闲时应自动停钟降开销。
 * 入参：work 是不可空、内嵌于 psi_group.avgs_work 的借用 work 对象。
 * 出参/返回：无直接返回值、无输出参数；聚合/更新 trigger和平均，活动时重新排 delayed work。
 * 注意事项：进程上下文可睡眠并持 avgs_lock；自身睡眠由 get_recent_times 特判避免永久 ping-pong。
 */
static void psi_avgs_work(struct work_struct *work)
{
	struct delayed_work *dwork;
	struct psi_group *group;
	u32 changed_states;
	u64 now;

	dwork = to_delayed_work(work);
	group = container_of(dwork, struct psi_group, avgs_work);

	/* mutex 同时保护平均游标、普通 trigger 链以及 delayed work 的续排决定。 */
	mutex_lock(&group->avgs_lock);

	now = sched_clock();

	collect_percpu_times(group, PSI_AVGS, &changed_states);
	/*
	 * If there is task activity, periodically fold the per-cpu
	 * times and feed samples into the running averages. If things
	 * are idle and there is no data to process, stop the clock.
	 * Once restarted, we'll catch up the running averages in one
	 * go - see calc_avgs() and missed_periods.
	 */
	if (now >= group->avg_next_update) {
		/* trigger 先看本轮新累计，随后平均游标再消费同一个快照。 */
		update_triggers(group, now, PSI_AVGS);
		group->avg_next_update = update_averages(group, now);
	}

	if (changed_states & PSI_STATE_RESCHEDULE) {
		/* 加一 jiffy 避免纳秒到 jiffies 向下取整导致提前反复唤醒。 */
		schedule_delayed_work(dwork, nsecs_to_jiffies(
				group->avg_next_update - now) + 1);
	}

	mutex_unlock(&group->avgs_lock);
}

/* 进入实时轮询窗口时把所有 trigger 窗口和 group 游标对齐到 @now。 */
/*
 * 业务背景：短窗口 rtpoll 从静止转活动时要统一重置 watcher 基线，防止把停钟期间累计误算进窗口。
 * 入参：group 是不可空输入/输出组；now 是当前 sched_clock 纳秒。
 * 出参/返回：无直接返回值、无输出参数；重置所有 POLL trigger、复制 total并设置下次期限。
 * 注意事项：调用者持 rtpoll_trigger_lock；rtpoll_min_period 必须由至少一个 trigger 建立。
 */
static void init_rtpoll_triggers(struct psi_group *group, u64 now)
{
	struct psi_trigger *t;

	list_for_each_entry(t, &group->rtpoll_triggers, node)
		window_reset(&t->win, now,
				group->total[PSI_POLL][t->state], 0);
	/* 同步 group 基线后，首轮只报告进入 rtpoll 以后产生的增长。 */
	memcpy(group->rtpoll_total, group->total[PSI_POLL],
		   sizeof(group->rtpoll_total));
	group->rtpoll_next_update = now + group->rtpoll_min_period;
}

/* Schedule rtpolling if it's not already scheduled or forced. */
/*
 * 以 atomic_xchg 合并调度并提供与状态写的全屏障；RCU 下若 worker 仍发布，
 * 修改 timer，否则清 scheduled 允许未来 trigger 重建后再次调度。
 */
/*
 * 业务背景：task 热路径只应合并一次 rtpoll 调度，用 timer 唤醒 psimon 而不能直接获取 mutex。
 * 入参：group 是不可空输入/输出组；delay 是 jiffies 延迟；force 决定已有 scheduled 时是否仍重设 timer。
 * 出参/返回：无直接返回值、无输出参数；可能置 scheduled并修改 timer，worker 已撤销时清标志。
 * 注意事项：可在 rq/原子热路径调用；atomic_xchg 提供与 worker 的全屏障，RCU 稳定 rtpoll_task 借用。
 */
static void psi_schedule_rtpoll_work(struct psi_group *group, unsigned long delay,
				   bool force)
{
	struct task_struct *task;

	/*
	 * atomic_xchg should be called even when !force to provide a
	 * full memory barrier (see the comment inside psi_rtpoll_work).
	 */
	if (atomic_xchg(&group->rtpoll_scheduled, 1) && !force)
		return;

	/* task 指针由 destroy 先置空再等待 RCU，热路径只能在读侧临界区借用。 */
	rcu_read_lock();

	task = rcu_dereference(group->rtpoll_task);
	/*
	 * kworker might be NULL in case psi_trigger_destroy races with
	 * psi_task_change (hotpath) which can't use locks
	 */
	if (likely(task))
		mod_timer(&group->rtpoll_timer, jiffies + delay);
	else
		/* worker 已撤销时归还 scheduled 令后续新 worker 能重新建立调度。 */
		atomic_set(&group->rtpoll_scheduled, 0);

	rcu_read_unlock();
}

/* 在 trigger mutex 下聚合短窗口、发事件并决定停止或按最小周期重排。 */
/*
 * 业务背景：psimon 被 timer 唤醒后需采集短窗状态、触发事件，并在活动窗口结束时自动停轮询。
 * 入参：group 是不可空输入/输出 PSI 组。
 * 出参/返回：无直接返回值、无输出参数；更新 POLL total/window/期限并可能重新调度 timer。
 * 注意事项：进程上下文可睡眠并持 rtpoll_trigger_lock；scheduled 清零后的 smp_mb 与热路径 xchg 配对。
 */
static void psi_rtpoll_work(struct psi_group *group)
{
	bool force_reschedule = false;
	u32 changed_states;
	u64 now;

	mutex_lock(&group->rtpoll_trigger_lock);

	now = sched_clock();

	/* 超出活动窗口先开放热路径重新调度；仍在窗内则强制维持 timer 节拍。 */
	if (now > group->rtpoll_until) {
		/*
		 * We are either about to start or might stop rtpolling if no
		 * state change was recorded. Resetting rtpoll_scheduled leaves
		 * a small window for psi_group_change to sneak in and schedule
		 * an immediate rtpoll_work before we get to rescheduling. One
		 * potential extra wakeup at the end of the rtpolling window
		 * should be negligible and rtpoll_next_update still keeps
		 * updates correctly on schedule.
		 */
		atomic_set(&group->rtpoll_scheduled, 0);
		/*
		 * A task change can race with the rtpoll worker that is supposed to
		 * report on it. To avoid missing events, ensure ordering between
		 * rtpoll_scheduled and the task state accesses, such that if the
		 * rtpoll worker misses the state update, the task change is
		 * guaranteed to reschedule the rtpoll worker:
		 *
		 * rtpoll worker:
		 *   atomic_set(rtpoll_scheduled, 0)
		 *   smp_mb()
		 *   LOAD states
		 *
		 * task change:
		 *   STORE states
		 *   if atomic_xchg(rtpoll_scheduled, 1) == 0:
		 *     schedule rtpoll worker
		 *
		 * The atomic_xchg() implies a full barrier.
		 */
		smp_mb();
	} else {
		/* The rtpolling window is not over, keep rescheduling */
		force_reschedule = true;
	}


	collect_percpu_times(group, PSI_POLL, &changed_states);

	/* 只有 watcher 关心的状态变化才延长 rtpoll 活跃期。 */
	if (changed_states & group->rtpoll_states) {
		/* Initialize trigger windows when entering rtpolling mode */
		if (now > group->rtpoll_until)
			init_rtpoll_triggers(group, now);

		/*
		 * Keep the monitor active for at least the duration of the
		 * minimum tracking window as long as monitor states are
		 * changing.
		 */
		group->rtpoll_until = now +
			group->rtpoll_min_period * UPDATES_PER_WINDOW;
	}

	if (now > group->rtpoll_until) {
		/* 无持续活动即停钟，用哨兵表明下次须从 trigger 基线重新开始。 */
		group->rtpoll_next_update = ULLONG_MAX;
		goto out;
	}

	if (now >= group->rtpoll_next_update) {
		/* 到期但无相关状态变化时只推进期限，不做无意义的 trigger 遍历。 */
		if (changed_states & group->rtpoll_states) {
			update_triggers(group, now, PSI_POLL);
			memcpy(group->rtpoll_total, group->total[PSI_POLL],
				   sizeof(group->rtpoll_total));
		}
		group->rtpoll_next_update = now + group->rtpoll_min_period;
	}

	psi_schedule_rtpoll_work(group,
		/* 纳秒差换算后补一 jiffy，确保下一次检查不早于固定期限。 */
		nsecs_to_jiffies(group->rtpoll_next_update - now) + 1,
		force_reschedule);

out:
	mutex_unlock(&group->rtpoll_trigger_lock);
}

/* 低优先级 FIFO psimon 等待 timer 唤醒，直至 kthread_stop；返回 0。 */
/*
 * 业务背景：短 PSI 窗口需要独立低优先级实时线程，在 timer 只发信号后执行可睡眠聚合。
 * 入参：data 是不可空、生命周期覆盖线程的借用 psi_group 指针。
 * 出参/返回：收到 kthread_stop 后返回 0；循环消费 wakeup 位并调用 rtpoll work。
 * 注意事项：线程 ownership 由 trigger create/destroy 管理；interruptible 虚假唤醒会重新检查条件。
 */
static int psi_rtpoll_worker(void *data)
{
	struct psi_group *group = (struct psi_group *)data;

	sched_set_fifo_low(current);

	while (true) {
		/* cmpxchg 同时消费单比特唤醒；多个 timer/状态变化可安全合并。 */
		wait_event_interruptible(group->rtpoll_wait,
				atomic_cmpxchg(&group->rtpoll_wakeup, 1, 0) ||
				kthread_should_stop());
		if (kthread_should_stop())
			break;

		psi_rtpoll_work(group);
	}
	return 0;
}

/* timer 回调只置 wakeup 位并唤醒 psimon，不在 timer 上下文执行聚合。 */
/*
 * 业务背景：hrtimer/softirq 类上下文不能获取 trigger mutex，故只桥接到可睡眠 psimon。
 * 入参：t 是不可空、内嵌于存活 psi_group 的借用 timer。
 * 出参/返回：无直接返回值、无输出参数；置 rtpoll_wakeup=1并唤醒等待队列。
 * 注意事项：timer 原子上下文不可睡眠；destroy 必须 delete timer并经 RCU/线程停止稳定 group 生命周期。
 */
static void poll_timer_fn(struct timer_list *t)
{
	struct psi_group *group = timer_container_of(group, t, rtpoll_timer);

	atomic_set(&group->rtpoll_wakeup, 1);
	wake_up_interruptible(&group->rtpoll_wait);
}

/* 把 state_start 至 @now 的区间按旧 state_mask 累入互斥 SOME/FULL/NONIDLE 桶。 */
/*
 * 业务背景：每次状态改变前必须先按旧 mask 封口时间区间，确保各压力桶累计连续且不重叠误切。
 * 入参：groupc 是不可空、本 CPU 写序列内输入/输出状态；now 是同 CPU clock 纳秒时间点。
 * 出参/返回：无直接返回值、无输出参数；推进 state_start并累加旧状态对应 times。
 * 注意事项：调用者持 rq 锁和 psi seq 写段；now 必须单调，SOME 与对应 FULL 可作为父子指标同时增长。
 */
static void record_times(struct psi_group_cpu *groupc, u64 now)
{
	u32 delta;

	delta = now - groupc->state_start;
	/* 先推进起点，后续各 active 桶共享完全相同的旧状态区间。 */
	groupc->state_start = now;

	if (groupc->state_mask & (1 << PSI_IO_SOME)) {
		groupc->times[PSI_IO_SOME] += delta;
		if (groupc->state_mask & (1 << PSI_IO_FULL))
			groupc->times[PSI_IO_FULL] += delta;
	}

	if (groupc->state_mask & (1 << PSI_MEM_SOME)) {
		/* FULL 是 SOME 的子集，因此同一 delta 同时进入两条累计曲线。 */
		groupc->times[PSI_MEM_SOME] += delta;
		if (groupc->state_mask & (1 << PSI_MEM_FULL))
			groupc->times[PSI_MEM_FULL] += delta;
	}

	if (groupc->state_mask & (1 << PSI_CPU_SOME)) {
		/* CPU SOME/FULL 沿用同样的父子累计关系，供用户分别观察竞争与完全停顿。 */
		groupc->times[PSI_CPU_SOME] += delta;
		if (groupc->state_mask & (1 << PSI_CPU_FULL))
			groupc->times[PSI_CPU_FULL] += delta;
	}

	if (groupc->state_mask & (1 << PSI_NONIDLE))
		groupc->times[PSI_NONIDLE] += delta;
}

#define for_each_group(iter, group) \
	for (typeof(group) iter = group; iter; iter = iter->parent)

/*
 * 已持 @cpu rq 锁和 PSI 写序列时，先结算旧状态，再按 clear/set 更新 task
 * 计数与派生 mask；活动重启平均 work，命中短窗状态则调度 rtpoll。
 */
/*
 * 业务背景：task 状态变化要沿一个 PSI group 更新 per-CPU 计数、封口旧状态并驱动两类聚合时钟。
 * 入参：group/cpu 定位状态；clear/set 是互斥 TSK_* 位；now 是 CPU 纳秒；wake_clock 控制平均 work 唤醒。
 * 出参/返回：无直接返回值、无输出参数；更新 tasks/state/times并可能排 avg/rtpoll 工作。
 * 注意事项：调用者持目标 rq 锁和 psi_write 段；下溢只告警一次，disabled group 仅保留 task 计数。
 */
static void psi_group_change(struct psi_group *group, int cpu,
			     unsigned int clear, unsigned int set,
			     u64 now, bool wake_clock)
{
	struct psi_group_cpu *groupc;
	unsigned int t, m;
	u32 state_mask;

	lockdep_assert_rq_held(cpu_rq(cpu));
	/* rq 锁串行 task 计数，外围 seqcount 则向无锁聚合读者标记写入代。 */
	groupc = per_cpu_ptr(group->pcpu, cpu);

	/*
	 * Start with TSK_ONCPU, which doesn't have a corresponding
	 * task count - it's just a boolean flag directly encoded in
	 * the state mask. Clear, set, or carry the current state if
	 * no changes are requested.
	 */
	if (unlikely(clear & TSK_ONCPU)) {
		/* ONCPU 没有计数，直接从旧 mask 清除或设置；其余位稍后由计数派生。 */
		state_mask = 0;
		clear &= ~TSK_ONCPU;
	} else if (unlikely(set & TSK_ONCPU)) {
		state_mask = PSI_ONCPU;
		set &= ~TSK_ONCPU;
	} else {
		state_mask = groupc->state_mask & PSI_ONCPU;
	}

	/*
	 * The rest of the state mask is calculated based on the task
	 * counts. Update those first, then construct the mask.
	 */
	for (t = 0, m = clear; m; m &= ~(1 << t), t++) {
		/* m 每轮消费一个位；空位继续前移，存在位才修改对应 task 计数。 */
		if (!(m & (1 << t)))
			continue;
		if (groupc->tasks[t]) {
			groupc->tasks[t]--;
		} else if (!psi_bug) {
				/* 下溢说明调用配对已破坏；只打印一次以免在调度热路径刷屏。 */
			printk_deferred(KERN_ERR "psi: task underflow! cpu=%d t=%d tasks=[%u %u %u %u] clear=%x set=%x\n",
					cpu, t, groupc->tasks[0],
					groupc->tasks[1], groupc->tasks[2],
					groupc->tasks[3], clear, set);
			psi_bug = 1;
		}
	}

	for (t = 0; set; set &= ~(1 << t), t++)
		if (set & (1 << t))
			groupc->tasks[t]++;

	/* 禁用组仍维护计数，保证重启 PSI 时首个派生 mask 以真实 task 状态为基线。 */
	if (!group->enabled) {
		/*
		 * On the first group change after disabling PSI, conclude
		 * the current state and flush its time. This is unlikely
		 * to matter to the user, but aggregation (get_recent_times)
		 * may have already incorporated the live state into times_prev;
		 * avoid a delta sample underflow when PSI is later re-enabled.
		 */
		if (unlikely(groupc->state_mask & (1 << PSI_NONIDLE)))
			record_times(groupc, now);

		groupc->state_mask = state_mask;

		return;
	}

	state_mask = test_states(groupc->tasks, state_mask);

	/* 当前 task 主动 reclaim 时，即使组内存在 runnable，也没有生产性工作能占用该 CPU。 */
	/*
	 * Since we care about lost potential, a memstall is FULL
	 * when there are no other working tasks, but also when
	 * the CPU is actively reclaiming and nothing productive
	 * could run even if it were runnable. So when the current
	 * task in a cgroup is in_memstall, the corresponding groupc
	 * on that cpu is in PSI_MEM_FULL state.
	 */
	if (unlikely((state_mask & PSI_ONCPU) && cpu_curr(cpu)->in_memstall))
		state_mask |= (1 << PSI_MEM_FULL);

	record_times(groupc, now);

	/* 旧状态封口完成后才发布新 mask，下一次变化从同一 now 起算。 */
	groupc->state_mask = state_mask;

	if (state_mask & group->rtpoll_states)
		psi_schedule_rtpoll_work(group, 1, false);

	if (wake_clock && !delayed_work_pending(&group->avgs_work))
		/* 普通平均仅在从 idle 转活动且尚未排队时启动，避免重复 work。 */
		schedule_delayed_work(&group->avgs_work, PSI_FREQ);
}

/* RCU/rq 保护下返回 @task 当前 cgroup PSI group；禁用 cgroup PSI 时返回 system。 */
/*
 * 业务背景：所有 task 热路径需统一选择 cgroup 专属 PSI group 或系统根组。
 * 入参：task 是不可空且由 rq 锁/RCU 稳定的只读借用 task。
 * 出参/返回：返回非空借用 psi_group；cgroup static key 关闭时返回 psi_system，无输出参数。
 * 注意事项：不增加 group/cgroup 引用；调用者必须在保护期内使用，配置关闭分支避免 cgroup 查找成本。
 */
static inline struct psi_group *task_psi_group(struct task_struct *task)
{
#ifdef CONFIG_CGROUPS
	if (static_branch_likely(&psi_cgroups_enabled))
		return cgroup_psi(task_dfl_cgroup(task));
#endif
	return &psi_system;
}

/* 仅更新 task 自身 psi_flags，并检查 clear/set 不重叠及状态转换合法性。 */
/*
 * 业务背景：task 私有 PSI 状态是 cgroup 移动和切换合并的权威输入，非法转换需诊断后仍继续修正。
 * 入参：task 是不可空输入/输出借用对象；clear/set 是应分别清除/设置且不重叠的 TSK_* 位图。
 * 出参/返回：无直接返回值、无输出参数；更新 task->psi_flags，首次不一致时设置 psi_bug并延迟打印。
 * 注意事项：调用者持 task rq 锁；psi_bug 抑制后续洪泛但不回滚，错误位图可能造成组计数偏差。
 */
static void psi_flags_change(struct task_struct *task, int clear, int set)
{
	/* set 中已有位或 clear 中缺失位都说明上层 enter/leave 配对不一致。 */
	if (((task->psi_flags & set) ||
	     (task->psi_flags & clear) != clear) &&
	    !psi_bug) {
		printk_deferred(KERN_ERR "psi: inconsistent task state! task=%d:%s cpu=%d psi_flags=%x clear=%x set=%x\n",
				task->pid, task->comm, task_cpu(task),
				task->psi_flags, clear, set);
		psi_bug = 1;
	}

	/* 即使诊断失败也执行请求的位变换，让后续状态尽量回到调用者声明的目标。 */
	task->psi_flags &= ~clear;
	task->psi_flags |= set;
}

/*
 * 对非 idle task 更新自身 flags，并在本 CPU seqcount 写段内沿 cgroup→root
 * 传播计数和时钟；调用者持 task 所在 rq 锁，故 task_cpu 稳定。
 */
/*
 * 业务背景：入队/出队/memstall 等单 task 状态变化要同步更新 task flags 和从 cgroup 到 root 的 PSI 计数。
 * 入参：task 是不可空且 rq 锁稳定的输入/输出 task；clear/set 是不重叠的 TSK_* 位图。
 * 出参/返回：无直接返回值、无输出参数；非 idle task 更新 flags及所有祖先 group。
 * 注意事项：调用者必须持 task rq 锁且不可睡眠；pid 0 idle task直接跳过，seqcount 覆盖整条祖先传播。
 */
void psi_task_change(struct task_struct *task, int clear, int set)
{
	int cpu = task_cpu(task);
	u64 now;

	if (!task->pid)
		return;

	/* 先更新 task 权威 flags，cgroup 迁移随后可据此完整搬运当前状态。 */
	psi_flags_change(task, clear, set);

	/* 单个 seq 写段覆盖所有祖先，读者不会看到层级传播到一半的本 CPU 字段组合。 */
	psi_write_begin(cpu);
	now = cpu_clock(cpu);
	for_each_group(group, task_psi_group(task))
		psi_group_change(group, cpu, clear, set, now, true);
	psi_write_end(cpu);
}

/*
 * context switch 合并 next ONCPU 与 prev 下 CPU/睡眠状态传播；遇共同祖先后
 * 只继续传播非 ONCPU 差异，避免同一层级重复走树。
 */
/*
 * 业务背景：context switch 同时改变 prev/next 的 ONCPU及可选睡眠状态，应合并共同祖先更新降低热路径成本。
 * 入参：prev/next 是不可空且 rq 锁稳定的借用 task；sleep 表示 prev 此次真正睡眠而非抢占。
 * 出参/返回：无直接返回值、无输出参数；更新两 task flags及相关新旧 cgroup 祖先的 per-CPU PSI。
 * 注意事项：调度切换 rq 锁内不可睡眠；共同祖先仅合并 ONCPU，其他差异必须继续传播到 root。
 */
void psi_task_switch(struct task_struct *prev, struct task_struct *next,
		     bool sleep)
{
	struct psi_group *common = NULL;
	int cpu = task_cpu(prev);
	u64 now;

	psi_write_begin(cpu);
	now = cpu_clock(cpu);

	/* 先把 next 标为 ONCPU；遇到已由 prev 占用的祖先即记录两条链的交点。 */
	if (next->pid) {
		psi_flags_change(next, 0, TSK_ONCPU);
		/*
		 * Set TSK_ONCPU on @next's cgroups. If @next shares any
		 * ancestors with @prev, those will already have @prev's
		 * TSK_ONCPU bit set, and we can stop the iteration there.
		 */
		for_each_group(group, task_psi_group(next)) {
			struct psi_group_cpu *groupc = per_cpu_ptr(group->pcpu, cpu);

			/* 首个已有 ONCPU 的祖先就是 prev/next 共享段，无需重复改变该段布尔位。 */
			if (groupc->state_mask & PSI_ONCPU) {
				common = group;
				break;
			}
			psi_group_change(group, cpu, 0, TSK_ONCPU, now, true);
		}
	}

	if (prev->pid) {
		int clear = TSK_ONCPU, set = 0;
		bool wake_clock = true;

		/* 真睡眠把 dequeue 延迟的 RUNNING/IOWAIT/MEMSTALL_RUNNING 一并合入本次树遍历。 */
		/*
		 * When we're going to sleep, psi_dequeue() lets us
		 * handle TSK_RUNNING, TSK_MEMSTALL_RUNNING and
		 * TSK_IOWAIT here, where we can combine it with
		 * TSK_ONCPU and save walking common ancestors twice.
		 */
		if (sleep) {
			clear |= TSK_RUNNING;
			/* reclaim 中的 runnable 子状态随 RUNNING 一起离开；iowait 则在睡眠时进入。 */
			if (prev->in_memstall)
				clear |= TSK_MEMSTALL_RUNNING;
			if (prev->in_iowait)
				set |= TSK_IOWAIT;

			/*
			 * Periodic aggregation shuts off if there is a period of no
			 * task changes, so we wake it back up if necessary. However,
			 * don't do this if the task change is the aggregation worker
			 * itself going to sleep, or we'll ping-pong forever.
			 */
			if (unlikely((prev->flags & PF_WQ_WORKER) &&
				     wq_worker_last_func(prev) == psi_avgs_work))
				/* 聚合 worker 自己睡眠不能再次唤醒自己，否则系统空闲后仍会永久循环。 */
				wake_clock = false;
		}

		psi_flags_change(prev, clear, set);

		/* 共同祖先上的 ONCPU 从 prev 无缝转给 next，无需先清后设制造瞬时 FULL。 */
		for_each_group(group, task_psi_group(prev)) {
			if (group == common)
				break;
			psi_group_change(group, cpu, clear, set, now, wake_clock);
		}

		/*
		 * TSK_ONCPU is handled up to the common ancestor. If there are
		 * any other differences between the two tasks (e.g. prev goes
		 * to sleep, or only one task is memstall), finish propagating
		 * those differences all the way up to the root.
		 */
		if ((prev->psi_flags ^ next->psi_flags) & ~TSK_ONCPU) {
			/* 睡眠等非 ONCPU 差异仍影响共同祖先，故从交点继续传播到 root。 */
			clear &= ~TSK_ONCPU;
			for_each_group(group, common)
				psi_group_change(group, cpu, clear, set, now, wake_clock);
		}
	}
	psi_write_end(cpu);
}

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
/* 在 rq 锁下把切换边界新增 IRQ 时间沿 curr cgroup 祖先计入 IRQ_FULL。 */
/*
 * 业务背景：精确 IRQ 时间会让当前 cgroup 完全失去 CPU，需要在 cgroup 边界变化时补入 PSI_IRQ_FULL。
 * 入参：rq 是不可空且已锁定的目标队列；curr 是不可空当前 task；prev 是可空前 task，均为借用输入。
 * 出参/返回：无直接返回值、无输出参数；推进 rq IRQ 游标并沿 curr 祖先累加 IRQ_FULL。
 * 注意事项：仅 IRQ_TIME_ACCOUNTING；禁用/idle/同组/时钟倒退均跳过，必须在 rq 锁和 psi seq 内更新。
 */
void psi_account_irqtime(struct rq *rq, struct task_struct *curr, struct task_struct *prev)
{
	int cpu = task_cpu(curr);
	struct psi_group_cpu *groupc;
	s64 delta;
	u64 irq;
	u64 now;

	if (static_branch_likely(&psi_disabled) || !irqtime_enabled())
		return;

	/* idle 线程不归属于用户工作负载，且同组切换无需切分 IRQ 累计区间。 */
	if (!curr->pid)
		return;

	lockdep_assert_rq_held(rq);
	if (prev && task_psi_group(prev) == task_psi_group(curr))
		return;

	irq = irq_time_read(cpu);
	/* 时钟倒退可能来自计费源重置；不更新游标，等待后续读数重新追上。 */
	delta = (s64)(irq - rq->psi_irq_time);
	if (delta < 0)
		return;
	rq->psi_irq_time = irq;

	psi_write_begin(cpu);
	now = cpu_clock(cpu);

	for_each_group(group, task_psi_group(curr)) {
		/* disabled 组保留 task 计数但不再累计任何压力时间。 */
		if (!group->enabled)
			continue;

		groupc = per_cpu_ptr(group->pcpu, cpu);

		record_times(groupc, now);
		groupc->times[PSI_IRQ_FULL] += delta;

		if (group->rtpoll_states & (1 << PSI_IRQ_FULL))
			psi_schedule_rtpoll_work(group, 1, false);
	}
	/* 所有祖先的 IRQ 桶完成后一次性结束本 CPU seq 写代。 */
	psi_write_end(cpu);
}
#endif /* CONFIG_IRQ_TIME_ACCOUNTING */

/**
 * psi_memstall_enter - mark the beginning of a memory stall section
 * @flags: flags to handle nested sections
 *
 * Marks the calling task as being stalled due to a lack of memory,
 * such as waiting for a refault or performing reclaim.
 */
/* 支持嵌套地标记 current 进入内存停顿；rq 锁使标志更新与迁移/调度原子。 */
/*
 * 业务背景：reclaim/refault 等内存阻塞区间需标记 current，并允许调用者安全嵌套 enter/leave。
 * 入参：flags 是不可空、调用者拥有的输入/输出保存槽，接收进入前 in_memstall 状态。
 * 出参/返回：无直接返回值；最外层设置 current 状态并加入 MEMSTALL/MEMSTALL_RUNNING，嵌套层不重复计数。
 * 注意事项：可能 irqsave 获取本 rq 锁且不可在冲突锁下调用；PSI 禁用时不保证写 flags。
 */
void psi_memstall_enter(unsigned long *flags)
{
	struct rq_flags rf;
	struct rq *rq;

	if (static_branch_likely(&psi_disabled))
		return;

	*flags = current->in_memstall;
	/* 保存旧值令嵌套调用只由最外层修改 PSI 计数。 */
	if (*flags)
		return;
	/*
	 * in_memstall setting & accounting needs to be atomic wrt
	 * changes to the task's scheduling state, otherwise we can
	 * race with CPU migration.
	 */
	rq = this_rq_lock_irq(&rf);

	/* rq 锁关闭迁移窗口，使 current 标志与 per-CPU/group 记账原子一致。 */
	current->in_memstall = 1;
	psi_task_change(current, 0, TSK_MEMSTALL | TSK_MEMSTALL_RUNNING);

	rq_unlock_irq(rq, &rf);
}
EXPORT_SYMBOL_GPL(psi_memstall_enter);

/**
 * psi_memstall_leave - mark the end of an memory stall section
 * @flags: flags to handle nested memdelay sections
 *
 * Marks the calling task as no longer stalled due to lack of memory.
 */
/* 仅最外层离开时在 rq 锁下清 MEMSTALL/MEMSTALL_RUNNING；嵌套层为空操作。 */
/*
 * 业务背景：内存停顿区间结束时仅最外层 leave 能撤销 PSI 状态，保持嵌套调用计数平衡。
 * 入参：flags 是不可空、由匹配 enter 写入的只读保存槽，ownership 归调用者。
 * 出参/返回：无直接返回值、无输出参数；flags 为 0 时清 current in_memstall及两个 task PSI 位。
 * 注意事项：必须与同一作用域 enter 配对；irqsave 获取本 rq 锁，PSI 禁用或嵌套 flags 非零时直接返回。
 */
void psi_memstall_leave(unsigned long *flags)
{
	struct rq_flags rf;
	struct rq *rq;

	if (static_branch_likely(&psi_disabled))
		return;

	if (*flags)
		return;
	/*
	 * in_memstall clearing & accounting needs to be atomic wrt
	 * changes to the task's scheduling state, otherwise we could
	 * race with CPU migration.
	 */
	rq = this_rq_lock_irq(&rf);

	/* 先清 task 标志再撤销两个 PSI 位，后续切换不会再把它视为 reclaim。 */
	current->in_memstall = 0;
	psi_task_change(current, TSK_MEMSTALL | TSK_MEMSTALL_RUNNING, 0);

	rq_unlock_irq(rq, &rf);
}
EXPORT_SYMBOL_GPL(psi_memstall_leave);

#ifdef CONFIG_CGROUPS
/* 为 cgroup 分配 group 与 percpu 状态并链接 parent；失败逆序释放并返回 -ENOMEM。 */
/*
 * 业务背景：每个 cgroup 需要独立 PSI group/per-CPU 桶，以计算层级内压力并链接父组。
 * 入参：cgroup 是不可空、创建阶段由 cgroup 核心拥有的输入/输出对象。
 * 出参/返回：成功或功能关闭返回 0；group/percpu 分配失败返回 -ENOMEM并回滚；成功发布 cgroup->psi。
 * 注意事项：仅 CGROUPS且 GFP 分配可睡眠；parent 是借用指针，最终 ownership 由 psi_cgroup_free 回收。
 */
int psi_cgroup_alloc(struct cgroup *cgroup)
{
	if (!static_branch_likely(&psi_cgroups_enabled))
		return 0;

	cgroup->psi = kzalloc_obj(struct psi_group);
	/* group 与 pcpu 分两级分配；第二级失败必须释放第一级并保持未发布状态。 */
	if (!cgroup->psi)
		return -ENOMEM;

	cgroup->psi->pcpu = alloc_percpu(struct psi_group_cpu);
	if (!cgroup->psi->pcpu) {
		kfree(cgroup->psi);
		return -ENOMEM;
	}
	group_init(cgroup->psi);
	/* parent 形成向根遍历链，子组不持有额外引用且依赖 cgroup 销毁顺序。 */
	cgroup->psi->parent = cgroup_psi(cgroup_parent(cgroup));
	return 0;
}

/* 取消平均 work、释放 percpu/group；调用前所有 trigger 必须已移除。 */
/*
 * 业务背景：cgroup 销毁时要停止其平均聚合并释放所有 PSI 私有存储。
 * 入参：cgroup 是不可空、销毁阶段且拥有 psi group 的输入/输出对象。
 * 出参/返回：无直接返回值、无输出参数；同步取消 work并释放 pcpu/group。
 * 注意事项：可睡眠；所有 trigger 必须先销毁，泄漏只 WARN 后仍释放，调用后 cgroup->psi 不可访问。
 */
void psi_cgroup_free(struct cgroup *cgroup)
{
	if (!static_branch_likely(&psi_cgroups_enabled))
		return;

	cancel_delayed_work_sync(&cgroup->psi->avgs_work);
	/* 同步取消确保 worker 不再借用 pcpu，随后才能释放 per-CPU 存储。 */
	free_percpu(cgroup->psi->pcpu);
	/* All triggers must be removed by now */
	WARN_ONCE(cgroup->psi->rtpoll_states, "psi: trigger leak\n");
	kfree(cgroup->psi);
}

/**
 * cgroup_move_task - move task to a different cgroup
 * @task: the task
 * @to: the target css_set
 *
 * Move task to a new cgroup and safely migrate its associated stall
 * state between the different groups.
 *
 * This function acquires the task's rq lock to lock out concurrent
 * changes to the task's scheduling state and - in case the task is
 * running - concurrent changes to its stall state.
 */
/*
 * 持 task rq 锁，以 task->psi_flags 从旧祖先链扣除，RCU 发布 @to，再向新链
 * 加回；直接依赖 PSI flags 而非可能处于 schedule 中间窗口的 on_rq 状态。
 */
/*
 * 业务背景：task 迁移 cgroup 时，已有运行/阻塞 PSI 状态必须从旧层级原子搬到新层级。
 * 入参：task 是不可空且生命周期稳定的输入/输出 task；to 是不可空目标 css_set 借用输入。
 * 出参/返回：无直接返回值、无输出参数；RCU 发布 task->cgroups并按 psi_flags 在两棵祖先树间迁移计数。
 * 注意事项：获取 task rq 锁并可能重试迁移；cgroup PSI 关闭时只发布指针，不能依赖 on_rq 中间状态。
 */
void cgroup_move_task(struct task_struct *task, struct css_set *to)
{
	unsigned int task_flags;
	struct rq_flags rf;
	struct rq *rq;

	if (!static_branch_likely(&psi_cgroups_enabled)) {
		/* 即使不计 cgroup PSI，调度器仍负责在内部安全发布 css_set。 */
		/*
		 * Lame to do this here, but the scheduler cannot be locked
		 * from the outside, so we move cgroups from inside sched/.
		 */
		rcu_assign_pointer(task->cgroups, to);
		return;
	}

	rq = task_rq_lock(task, &rf);

	/* rq 锁稳定 task_cpu、psi_flags 及 switch 延迟更新窗口。 */
	/*
	 * We may race with schedule() dropping the rq lock between
	 * deactivating prev and switching to next. Because the psi
	 * updates from the deactivation are deferred to the switch
	 * callback to save cgroup tree updates, the task's scheduling
	 * state here is not coherent with its psi state:
	 *
	 * schedule()                   cgroup_move_task()
	 *   rq_lock()
	 *   deactivate_task()
	 *     p->on_rq = 0
	 *     psi_dequeue() // defers TSK_RUNNING & TSK_IOWAIT updates
	 *   pick_next_task()
	 *     rq_unlock()
	 *                                rq_lock()
	 *                                psi_task_change() // old cgroup
	 *                                task->cgroups = to
	 *                                psi_task_change() // new cgroup
	 *                                rq_unlock()
	 *     rq_lock()
	 *   psi_sched_switch() // does deferred updates in new cgroup
	 *
	 * Don't rely on the scheduling state. Use psi_flags instead.
	 */
	task_flags = task->psi_flags;

	/* 以完全相同的 flags 先从旧树扣除、再在 RCU 发布后加入新树，守恒组计数。 */
	if (task_flags)
		psi_task_change(task, task_flags, 0);

	/* See comment above */
	rcu_assign_pointer(task->cgroups, to);

	if (task_flags)
		psi_task_change(task, 0, task_flags);

	task_rq_unlock(rq, task, &rf);
}

/* 重新启用 group 后逐 CPU 持 rq 锁重建派生 mask/起始时钟；task 计数一直保留。 */
/*
 * 业务背景：cgroup PSI disabled 期间保留 task 计数但停派生状态，重新启用需逐 CPU 重建时钟/mask。
 * 入参：group 是不可空、已由控制面决定 enabled 状态的输入/输出组。
 * 出参/返回：无直接返回值、无输出参数；enabled 时逐 possible CPU 调用零变化 group_change 重启聚合。
 * 注意事项：仅 CGROUPS且可循环获取各 rq irq 锁；disabled 时直接返回，调用者稳定 group 生命周期。
 */
void psi_cgroup_restart(struct psi_group *group)
{
	int cpu;

	/*
	 * After we disable psi_group->enabled, we don't actually
	 * stop percpu tasks accounting in each psi_group_cpu,
	 * instead only stop test_states() loop, record_times()
	 * and averaging worker, see psi_group_change() for details.
	 *
	 * When disable cgroup PSI, this function has nothing to sync
	 * since cgroup pressure files are hidden and percpu psi_group_cpu
	 * would see !psi_group->enabled and only do task accounting.
	 *
	 * When re-enable cgroup PSI, this function use psi_group_change()
	 * to get correct state mask from test_states() loop on tasks[],
	 * and restart groupc->state_start from now, use .clear = .set = 0
	 * here since no task status really changed.
	 */
	if (!group->enabled)
		return;

	/* 逐 CPU 独立持 rq 锁，避免一次锁住所有运行队列；每个快照用本 CPU seq 发布。 */
	for_each_possible_cpu(cpu) {
		u64 now;

		guard(rq_lock_irq)(cpu_rq(cpu));

		psi_write_begin(cpu);
		now = cpu_clock(cpu);
		psi_group_change(group, cpu, 0, 0, now, true);
		psi_write_end(cpu);
	}
}
#endif /* CONFIG_CGROUPS */

/*
 * 在 avgs mutex 下先聚合最新时间，再输出 @res 的 some/full 10/60/300 平均
 * 与累计微秒；system CPU FULL 未定义按零展示，禁用资源返回 -EOPNOTSUPP。
 */
/*
 * 业务背景：/proc 与 cgroup pressure 文件需在读取前刷新 group 平均，并输出资源 SOME/FULL 标准格式。
 * 入参：m 是不可空输出 seq_file；group 是不可空输入/输出组；res 是 IO/MEM/CPU/IRQ 资源枚举。
 * 出参/返回：成功返回 0并输出 avg10/60/300及 total；PSI或 IRQ 资源不可用返回 -EOPNOTSUPP。
 * 注意事项：可睡眠并持 avgs_lock；system CPU FULL 固定为零，输出是刷新后的瞬时快照。
 */
int psi_show(struct seq_file *m, struct psi_group *group, enum psi_res res)
{
	bool only_full = false;
	int full;
	u64 now;

	if (static_branch_likely(&psi_disabled))
		return -EOPNOTSUPP;

	/* IRQ 节点的编译期与运行期开关都必须成立，避免展示没有计费来源的零值。 */
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	if (!irqtime_enabled() && res == PSI_IRQ)
		return -EOPNOTSUPP;
#endif

	/* Update averages before reporting them */
	/* 读取路径主动折叠最新 per-CPU 桶，避免只因后台 work 尚未到期而返回陈旧 total。 */
	mutex_lock(&group->avgs_lock);
	now = sched_clock();
	collect_percpu_times(group, PSI_AVGS, NULL);
	if (now >= group->avg_next_update)
		group->avg_next_update = update_averages(group, now);
	mutex_unlock(&group->avgs_lock);

	/* IRQ ABI 只定义 full，其他资源则输出 some/full 两行。 */
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	only_full = res == PSI_IRQ;
#endif

	for (full = 0; full < 2 - only_full; full++) {
		unsigned long avg[3] = { 0, };
		u64 total = 0;
		int w;

		/* CPU FULL is undefined at the system level */
		/* IRQ 只有 FULL 一行；普通资源按 SOME 后 FULL 的稳定 ABI 顺序输出。 */
		if (!(group == &psi_system && res == PSI_CPU && full)) {
			for (w = 0; w < 3; w++)
				avg[w] = group->avg[res * 2 + full][w];
			total = div_u64(group->total[PSI_AVGS][res * 2 + full],
					NSEC_PER_USEC);
		}

		seq_printf(m, "%s avg10=%lu.%02lu avg60=%lu.%02lu avg300=%lu.%02lu total=%llu\n",
			   /* 平均值由定点宏拆整数/两位小数，累计纳秒则转换成 ABI 规定的微秒。 */
			   full || only_full ? "full" : "some",
			   LOAD_INT(avg[0]), LOAD_FRAC(avg[0]),
			   LOAD_INT(avg[1]), LOAD_FRAC(avg[1]),
			   LOAD_INT(avg[2]), LOAD_FRAC(avg[2]),
			   total);
	}

	return 0;
}

/*
 * 解析 "some|full threshold window"，校验权限/窗口后分配 trigger；普通用户
 * 挂 avgs 链，特权短窗按需创建并 RCU 发布 psimon，返回指针或 ERR_PTR。
 */
/*
 * 业务背景：用户写 pressure 文件要创建一个窗口阈值 watcher，并按权限选择普通平均或实时轮询后端。
 * 入参：group 是目标借用组；buf 是可写配置文本；res 是资源；file 提供凭据；of 是可空 kernfs 通知对象。
 * 出参/返回：成功返回调用者拥有的 trigger；失败返回 ERR_PTR(-EOPNOTSUPP/-EINVAL/-ENOMEM或线程错误)。
 * 注意事项：可分配/睡眠并持 trigger mutex；file/of/group 不转移 ownership，成功对象必须 destroy。
 */
struct psi_trigger *psi_trigger_create(struct psi_group *group, char *buf,
				       enum psi_res res, struct file *file,
				       struct kernfs_open_file *of)
{
	struct psi_trigger *t;
	enum psi_states state;
	u32 threshold_us;
	bool privileged;
	u32 window_us;

	/* 全局禁用在解析和分配前失败，调用者不会得到半初始化对象。 */
	if (static_branch_likely(&psi_disabled))
		return ERR_PTR(-EOPNOTSUPP);

	/* 权限取自打开文件时凭据，允许显式把已打开 fd 委托给低权限写者。 */
	/*
	 * Checking the privilege here on file->f_cred implies that a privileged user
	 * could open the file and delegate the write to an unprivileged one.
	 */
	privileged = cap_raised(file->f_cred->cap_effective, CAP_SYS_RESOURCE);

	if (sscanf(buf, "some %u %u", &threshold_us, &window_us) == 2)
		/* 资源枚举每项占 SOME/FULL 两个连续状态槽。 */
		state = PSI_IO_SOME + res * 2;
	else if (sscanf(buf, "full %u %u", &threshold_us, &window_us) == 2)
		state = PSI_IO_FULL + res * 2;
	else
		return ERR_PTR(-EINVAL);

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	/* IRQ 只有 FULL；--state 把通用资源公式产生的槽映射到唯一 IRQ 状态。 */
	if (res == PSI_IRQ && --state != PSI_IRQ_FULL)
		return ERR_PTR(-EINVAL);
#endif

	if (state >= PSI_NONIDLE)
		return ERR_PTR(-EINVAL);

	if (window_us == 0 || window_us > WINDOW_MAX_US)
		return ERR_PTR(-EINVAL);

	/* 低权限 watcher 限制为两秒整数倍，确保只复用普通 work 而不创建实时线程。 */
	/*
	 * Unprivileged users can only use 2s windows so that averages aggregation
	 * work is used, and no RT threads need to be spawned.
	 */
	if (!privileged && window_us % 2000000)
		return ERR_PTR(-EINVAL);

	/* Check threshold */
	/* 阈值必须落在窗口内部，否则永远不可能触发或退化为持续触发。 */
	if (threshold_us == 0 || threshold_us > window_us)
		return ERR_PTR(-EINVAL);

	t = kmalloc_obj(*t);
	if (!t)
		return ERR_PTR(-ENOMEM);

	t->group = group;
	/* 用户 ABI 使用微秒，内部窗口与累计统一转换为纳秒防止混合单位。 */
	t->state = state;
	t->threshold = threshold_us * NSEC_PER_USEC;
	t->win.size = window_us * NSEC_PER_USEC;
	window_reset(&t->win, sched_clock(),
			group->total[PSI_POLL][t->state], 0);

	t->event = 0;
	/* kernfs watcher 由 kernfs 通知；proc watcher 没有 of，需自带等待队列。 */
	t->last_event_time = 0;
	t->of = of;
	if (!of)
		init_waitqueue_head(&t->event_wait);
	t->pending_event = false;
	t->aggregator = privileged ? PSI_POLL : PSI_AVGS;

	if (privileged) {
		/* 短窗链及唯一 psimon 均由 rtpoll mutex 串行创建。 */
		mutex_lock(&group->rtpoll_trigger_lock);

		if (!rcu_access_pointer(group->rtpoll_task)) {
			struct task_struct *task;

			task = kthread_create(psi_rtpoll_worker, group, "psimon");
			/* 线程未发布前失败只需释放新 trigger，不影响现有 group 状态。 */
			if (IS_ERR(task)) {
				kfree(t);
				mutex_unlock(&group->rtpoll_trigger_lock);
				return ERR_CAST(task);
			}
			atomic_set(&group->rtpoll_wakeup, 0);
			wake_up_process(task);
			rcu_assign_pointer(group->rtpoll_task, task);
		}

		list_add(&t->node, &group->rtpoll_triggers);
		/* 最小窗口的十分之一决定共享 timer 频率，状态位图过滤无关 task 变化。 */
		group->rtpoll_min_period = min(group->rtpoll_min_period,
			div_u64(t->win.size, UPDATES_PER_WINDOW));
		group->rtpoll_nr_triggers[t->state]++;
		group->rtpoll_states |= (1 << t->state);

		mutex_unlock(&group->rtpoll_trigger_lock);
	} else {
		/* 普通 watcher 仅加入平均链，不创建线程或调整 rtpoll 周期。 */
		mutex_lock(&group->avgs_lock);

		list_add(&t->node, &group->avg_triggers);
		group->avg_nr_triggers[t->state]++;

		mutex_unlock(&group->avgs_lock);
	}
	return t;
}

/*
 * 唤醒 poller、从相应链表摘除并更新状态位；最后短窗 trigger 清 RCU task
 * 与 timer。等待 grace period 后在 mutex 外 stop kthread，再释放 trigger。
 */
/*
 * 业务背景：关闭 fd/cgroup watcher 时必须先撤销事件入口与链表，再安全停止最后一个 psimon并释放对象。
 * 入参：t 是可空、由 create 返回且调用者拥有的 trigger；NULL 为安全空操作。
 * 出参/返回：无直接返回值、无输出参数；摘链、更新 group状态、必要时停 timer/thread并释放 t。
 * 注意事项：可睡眠；RCU grace period 阻止热路径继续发现 worker，kthread_stop 必须在 mutex 外避免死锁。
 */
void psi_trigger_destroy(struct psi_trigger *t)
{
	struct psi_group *group;
	struct task_struct *task_to_destroy = NULL;

	/*
	 * We do not check psi_disabled since it might have been disabled after
	 * the trigger got created.
	 */
	if (!t)
		return;

	/* t 在整个销毁过程保持存活；先从可发现位置撤销，最后才 kfree。 */
	group = t->group;
	/*
	 * Wakeup waiters to stop polling and clear the queue to prevent it from
	 * being accessed later. Can happen if cgroup is deleted from under a
	 * polling process.
	 */
	if (t->of)
		kernfs_notify(t->of->kn);
	else
		wake_up_interruptible(&t->event_wait);

	/* aggregator 在创建后不变，决定链表、计数和互斥锁的唯一归属。 */
	if (t->aggregator == PSI_AVGS) {
		mutex_lock(&group->avgs_lock);
		if (!list_empty(&t->node)) {
			/* list_empty 允许控制面重复走到已摘链对象而不再次减少计数。 */
			list_del(&t->node);
			group->avg_nr_triggers[t->state]--;
		}
		mutex_unlock(&group->avgs_lock);
	} else {
		mutex_lock(&group->rtpoll_trigger_lock);
		if (!list_empty(&t->node)) {
			struct psi_trigger *tmp;
			u64 period = ULLONG_MAX;

			/* 先摘链，使后续重算周期与状态计数只观察仍然存活的 watcher。 */
			list_del(&t->node);
			group->rtpoll_nr_triggers[t->state]--;
			/* 某状态最后一个 watcher 消失后，热路径无需再因该状态调度轮询。 */
			if (!group->rtpoll_nr_triggers[t->state])
				group->rtpoll_states &= ~(1 << t->state);
			/*
			 * Reset min update period for the remaining triggers
			 * iff the destroying trigger had the min window size.
			 */
			if (group->rtpoll_min_period == div_u64(t->win.size, UPDATES_PER_WINDOW)) {
				/* 仅删除当前最短窗口时才需要扫描余链重算共享最小周期。 */
				list_for_each_entry(tmp, &group->rtpoll_triggers, node)
					period = min(period, div_u64(tmp->win.size,
							UPDATES_PER_WINDOW));
				group->rtpoll_min_period = period;
			}
			/* Destroy rtpoll_task when the last trigger is destroyed */
			if (group->rtpoll_states == 0) {
				/* 先撤销 RCU 发布并删 timer，阻止新的热路径/定时器再唤醒该线程。 */
				group->rtpoll_until = 0;
				task_to_destroy = rcu_dereference_protected(
						group->rtpoll_task,
						lockdep_is_held(&group->rtpoll_trigger_lock));
				rcu_assign_pointer(group->rtpoll_task, NULL);
				timer_delete(&group->rtpoll_timer);
			}
			/* mutex 解锁前链表、状态位、最小周期和 RCU task 已形成一致的新控制状态。 */
		}
		mutex_unlock(&group->rtpoll_trigger_lock);
	}

	/*
	 * Wait for psi_schedule_rtpoll_work RCU to complete its read-side
	 * critical section before destroying the trigger and optionally the
	 * rtpoll_task.
	 */
	synchronize_rcu();
	/* grace period 结束后，所有已进入 schedule_rtpoll 的读者都已放弃 task 借用。 */
	/*
	 * Stop kthread 'psimon' after releasing rtpoll_trigger_lock to prevent
	 * a deadlock while waiting for psi_rtpoll_work to acquire
	 * rtpoll_trigger_lock
	 */
	if (task_to_destroy) {
		/*
		 * After the RCU grace period has expired, the worker
		 * can no longer be found through group->rtpoll_task.
		 */
		kthread_stop(task_to_destroy);
		/* 线程退出后清 scheduled，为同 group 将来创建新 trigger 留下干净状态。 */
		atomic_set(&group->rtpoll_scheduled, 0);
	}
	kfree(t);
}

/* acquire 读取已发布 trigger、登记等待队列，并用 cmpxchg 领取一次 EPOLLPRI。 */
/*
 * 业务背景：poll/epoll 需要观察 trigger 生命周期和一次性阈值事件，同时在销毁/禁用时报告错误。
 * 入参：trigger_ptr 是不可空、release 发布的可空 trigger 指针槽；file 是借用文件；wait 是可空 poll 表。
 * 出参/返回：返回默认 mask，事件时加 EPOLLPRI，禁用/无 trigger 时加 EPOLLERR|EPOLLPRI。
 * 注意事项：acquire 与发布配对；cmpxchg 独占消费 event，trigger 生命周期由 release/destroy 同步保证。
 */
__poll_t psi_trigger_poll(void **trigger_ptr,
				struct file *file, poll_table *wait)
{
	__poll_t ret = DEFAULT_POLLMASK;
	struct psi_trigger *t;

	if (static_branch_likely(&psi_disabled))
		return DEFAULT_POLLMASK | EPOLLERR | EPOLLPRI;

	t = smp_load_acquire(trigger_ptr);
	/* NULL 既可表示尚未写入，也可表示释放中；两者都以错误加优先事件唤醒用户。 */
	if (!t)
		return DEFAULT_POLLMASK | EPOLLERR | EPOLLPRI;

	if (t->of)
		kernfs_generic_poll(t->of, wait);
	else
		poll_wait(file, &t->event_wait, wait);

	/* 事件位一次只交给一个并发 poller，后续事件须由聚合器重新执行 0→1。 */
	if (cmpxchg(&t->event, 1, 0) == 1)
		ret |= EPOLLPRI;

	return ret;
}

#ifdef CONFIG_PROC_FS
/* 输出 system IO PSI；@v 未使用。 */
/*
 * 业务背景：/proc/pressure/io 的 seq show 复用系统 group 通用格式器。
 * 入参：m 是不可空输出 seq_file；v 是未使用借用参数，可为 NULL。
 * 出参/返回：返回 psi_show 的 0 或 -EOPNOTSUPP；无输出参数或 ownership 变化。
 * 注意事项：仅 PROC_FS；读取可睡眠并刷新平均。
 */
static int psi_io_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_IO);
}

/* 输出 system memory PSI。 */
/*
 * 业务背景：/proc/pressure/memory 需输出系统级 memory SOME/FULL 压力。
 * 入参：m 是不可空输出 seq_file；v 是未使用借用参数，可为 NULL。
 * 出参/返回：返回 psi_show 的 0 或 -EOPNOTSUPP；无输出参数或 ownership 变化。
 * 注意事项：仅 PROC_FS；结果是读取时刷新后的快照。
 */
static int psi_memory_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_MEM);
}

/* 输出 system CPU PSI。 */
/*
 * 业务背景：/proc/pressure/cpu 需输出系统级 CPU SOME及定义为空的 FULL 行。
 * 入参：m 是不可空输出 seq_file；v 是未使用借用参数，可为 NULL。
 * 出参/返回：返回 psi_show 的 0 或 -EOPNOTSUPP；无输出参数或 ownership 变化。
 * 注意事项：仅 PROC_FS；系统 CPU FULL 按通用 show 规则输出零。
 */
static int psi_cpu_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_CPU);
}

/* 为 /proc/pressure/io 建立 single_open 上下文。 */
/*
 * 业务背景：打开 IO pressure 文件时需创建绑定 system IO show 的 single seq 上下文。
 * 入参：inode 是未使用借用 inode；file 是不可空 VFS 输入/输出文件。
 * 出参/返回：成功返回 0并初始化 private_data；失败返回 single_open 负 errno。
 * 注意事项：仅 PROC_FS且可睡眠；资源由 psi_fop_release/single_release 配对回收。
 */
static int psi_io_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_io_show, NULL);
}

/* 为 /proc/pressure/memory 建立 single_open 上下文。 */
/*
 * 业务背景：打开 memory pressure 文件时需创建绑定对应 show 的 single seq 上下文。
 * 入参：inode 是未使用借用 inode；file 是不可空 VFS 输入/输出文件。
 * 出参/返回：成功返回 0并初始化 private_data；失败返回负 errno。
 * 注意事项：仅 PROC_FS且可睡眠；成功资源由 release 回收。
 */
static int psi_memory_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_memory_show, NULL);
}

/* 为 /proc/pressure/cpu 建立 single_open 上下文。 */
/*
 * 业务背景：打开 CPU pressure 文件时需创建绑定 system CPU show 的 single seq 上下文。
 * 入参：inode 是未使用借用 inode；file 是不可空 VFS 输入/输出文件。
 * 出参/返回：成功返回 0并初始化 private_data；失败返回负 errno。
 * 注意事项：仅 PROC_FS且可睡眠；文件/seq ownership 由 VFS 与 release 管理。
 */
static int psi_cpu_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_cpu_show, NULL);
}

/*
 * 为一个 fd 创建唯一 system trigger；seq lock 串行写者，成功以 release
 * 发布到 seq->private 并返回原 nbytes，重复写返回 -EBUSY。
 */
/*
 * 业务背景：system pressure fd 写入配置要创建且只创建一个 trigger，并发布给并发 poll。
 * 入参：file 是不可空且 private 为 seq_file 的借用文件；user_buf 是用户只读文本；nbytes 是字节数；res 是资源。
 * 出参/返回：成功返回原 nbytes；禁用 -EOPNOTSUPP、空输入 -EINVAL、复制 -EFAULT、重复 -EBUSY或 create 错误。
 * 注意事项：可睡眠并持 seq->lock；最多复制 32 字节且强制末字节 NUL，release-store 与 poll acquire 配对。
 */
static ssize_t psi_write(struct file *file, const char __user *user_buf,
			 size_t nbytes, enum psi_res res)
{
	char buf[32];
	size_t buf_size;
	struct seq_file *seq;
	struct psi_trigger *new;

	/* 禁用检查早于用户内存访问，保持功能关闭时稳定返回不支持。 */
	if (static_branch_likely(&psi_disabled))
		return -EOPNOTSUPP;

	if (!nbytes)
		return -EINVAL;

	/* 固定小缓冲限制解析成本；末字节改 NUL 意味着满 32 字节输入最后一字节不参与解析。 */
	buf_size = min(nbytes, sizeof(buf));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;

	buf[buf_size - 1] = '\0';

	seq = file->private_data;

	/* Take seq->lock to protect seq->private from concurrent writes */
	/* seq mutex 覆盖“检查为空—创建—发布”，从而维持每 fd 单 trigger 不变量。 */
	mutex_lock(&seq->lock);

	/* Allow only one trigger per file descriptor */
	if (seq->private) {
		mutex_unlock(&seq->lock);
		return -EBUSY;
	}

	new = psi_trigger_create(&psi_system, buf, res, file, NULL);
	/* 创建失败不发布 private，调用者可修正配置后在同一 fd 上重试。 */
	if (IS_ERR(new)) {
		mutex_unlock(&seq->lock);
		return PTR_ERR(new);
	}

	smp_store_release(&seq->private, new);
	/* release-store 保证 poll acquire 见到指针时也看见 trigger 的全部初始化字段。 */
	mutex_unlock(&seq->lock);

	return nbytes;
}

/* 将 proc IO trigger 配置转交 psi_write。 */
/*
 * 业务背景：IO proc write 只需固定资源类型并复用 fd 唯一 trigger 创建协议。
 * 入参：file/user_buf/nbytes 是借用文件、用户只读文本和字节数；ppos 是未使用可空偏移参数。
 * 出参/返回：返回 psi_write 的成功字节数或负 errno；无额外输出或 ownership 变化。
 * 注意事项：仅 PROC_FS；不推进 *ppos，trigger 生命周期绑定 file release。
 */
static ssize_t psi_io_write(struct file *file, const char __user *user_buf,
			    size_t nbytes, loff_t *ppos)
{
	return psi_write(file, user_buf, nbytes, PSI_IO);
}

/* 将 proc memory trigger 配置转交 psi_write。 */
/*
 * 业务背景：memory proc write 固定 PSI_MEM 后复用通用 trigger 解析和发布。
 * 入参：file/user_buf/nbytes 是借用文件、用户文本和字节数；ppos 未使用且可空。
 * 出参/返回：返回 psi_write 的成功字节数或负 errno；无额外输出参数。
 * 注意事项：仅 PROC_FS；一个 fd 仍只允许一个 trigger。
 */
static ssize_t psi_memory_write(struct file *file, const char __user *user_buf,
				size_t nbytes, loff_t *ppos)
{
	return psi_write(file, user_buf, nbytes, PSI_MEM);
}

/* 将 proc CPU trigger 配置转交 psi_write。 */
/*
 * 业务背景：CPU proc write 固定 PSI_CPU 后复用通用 trigger 创建路径。
 * 入参：file/user_buf/nbytes 是借用文件、用户文本和字节数；ppos 未使用且可空。
 * 出参/返回：返回 psi_write 的成功字节数或负 errno；无额外输出参数。
 * 注意事项：仅 PROC_FS；system CPU trigger 的 state 合法性由 create 校验。
 */
static ssize_t psi_cpu_write(struct file *file, const char __user *user_buf,
			     size_t nbytes, loff_t *ppos)
{
	return psi_write(file, user_buf, nbytes, PSI_CPU);
}

/* 对 seq->private trigger 执行 acquire/poll/event 领取。 */
/*
 * 业务背景：三个/四个 proc pressure fd 的 poll 回调共享 trigger acquire 和一次事件消费逻辑。
 * 入参：file 是不可空且 private 为 seq_file 的借用文件；wait 是可空 poll_table。
 * 出参/返回：返回 psi_trigger_poll 的 poll mask；无输出参数或 ownership 变化。
 * 注意事项：仅 PROC_FS；seq->private 由 write release-store 发布并由 release 销毁。
 */
static __poll_t psi_fop_poll(struct file *file, poll_table *wait)
{
	struct seq_file *seq = file->private_data;

	return psi_trigger_poll(&seq->private, file, wait);
}

/* 销毁 fd 私有 trigger 后释放 single_open；确保 poller 不再访问已释放对象。 */
/*
 * 业务背景：关闭 pressure fd 时要先撤销可能存在的 watcher，再回收 seq_file 上下文。
 * 入参：inode/file 是不可空、VFS 借用的关闭对象，file->private_data 为有效 seq_file。
 * 出参/返回：返回 single_release 的 0/错误；销毁 seq->private trigger并释放文件私有状态。
 * 注意事项：可睡眠等待 RCU/kthread；destroy 接受 NULL，调用后不得再 poll/write 此 file。
 */
static int psi_fop_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;

	psi_trigger_destroy(seq->private);
	return single_release(inode, file);
}

static const struct proc_ops psi_io_proc_ops = {
	/* IO 节点读平均、写 trigger、poll 事件，并由统一 release 销毁 fd 私有 watcher。 */
	.proc_open	= psi_io_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_write	= psi_io_write,
	.proc_poll	= psi_fop_poll,
	.proc_release	= psi_fop_release,
};

static const struct proc_ops psi_memory_proc_ops = {
	/* memory 与 IO 使用相同生命周期协议，仅 open/write 固定的资源枚举不同。 */
	.proc_open	= psi_memory_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_write	= psi_memory_write,
	.proc_poll	= psi_fop_poll,
	.proc_release	= psi_fop_release,
};

static const struct proc_ops psi_cpu_proc_ops = {
	/* CPU 节点同样复用 seq/poll/release，系统级 FULL 的零值由 psi_show 处理。 */
	.proc_open	= psi_cpu_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_write	= psi_cpu_write,
	.proc_poll	= psi_fop_poll,
	.proc_release	= psi_fop_release,
};

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
/* IRQ time accounting 可用时输出 system IRQ FULL PSI。 */
/*
 * 业务背景：/proc/pressure/irq 在精确 IRQ 计费构建中展示系统 IRQ FULL 压力。
 * 入参：m 是不可空输出 seq_file；v 是未使用借用参数，可为 NULL。
 * 出参/返回：返回 psi_show 的 0 或 -EOPNOTSUPP；无输出参数或 ownership 变化。
 * 注意事项：仅 PROC_FS+IRQ_TIME_ACCOUNTING；运行时 irqtime 关闭会返回不支持。
 */
static int psi_irq_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_IRQ);
}

/* 为 /proc/pressure/irq 建立 single_open 上下文。 */
/*
 * 业务背景：打开 IRQ pressure 节点时需创建绑定 psi_irq_show 的 single seq 上下文。
 * 入参：inode 是未使用借用 inode；file 是不可空 VFS 输入/输出文件。
 * 出参/返回：成功返回 0并初始化 private_data；失败返回负 errno。
 * 注意事项：仅 PROC_FS+IRQ_TIME_ACCOUNTING且可睡眠；资源由统一 release 回收。
 */
static int psi_irq_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_irq_show, NULL);
}

/* 将 proc IRQ trigger 配置转交 psi_write；只接受 full 状态。 */
/*
 * 业务背景：IRQ proc write 固定 PSI_IRQ 后复用通用 trigger 路径，create 会把合法状态限制为 FULL。
 * 入参：file/user_buf/nbytes 是借用文件、用户文本和字节数；ppos 未使用且可空。
 * 出参/返回：返回 psi_write 的成功字节数或负 errno；无额外输出参数。
 * 注意事项：仅 PROC_FS+IRQ_TIME_ACCOUNTING；some 配置返回 -EINVAL。
 */
static ssize_t psi_irq_write(struct file *file, const char __user *user_buf,
			     size_t nbytes, loff_t *ppos)
{
	return psi_write(file, user_buf, nbytes, PSI_IRQ);
}

static const struct proc_ops psi_irq_proc_ops = {
	/* IRQ 节点只在精确 IRQ 计费构建中注册，写配置也仅接受 FULL。 */
	.proc_open	= psi_irq_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_write	= psi_irq_write,
	.proc_poll	= psi_fop_poll,
	.proc_release	= psi_fop_release,
};
#endif /* CONFIG_IRQ_TIME_ACCOUNTING */

/* PSI 启用时创建四类 /proc/pressure 节点；节点创建失败不阻止启动。 */
/*
 * 业务背景：procfs 初始化要发布 system IO/memory/CPU及可选 IRQ pressure 读写/poll 接口。
 * 入参：无。
 * 出参/返回：恒返回 0；psi_enable 时尝试创建目录和各节点，失败允许部分完成。
 * 注意事项：__init 可睡眠上下文；procfs 接管节点生命周期，禁用时不创建任何入口。
 */
static int __init psi_proc_init(void)
{
	if (psi_enable) {
		/* proc_create 的 NULL 返回不回滚已有节点，启动继续保留可用的部分接口。 */
		proc_mkdir("pressure", NULL);
		proc_create("pressure/io", 0666, NULL, &psi_io_proc_ops);
		proc_create("pressure/memory", 0666, NULL, &psi_memory_proc_ops);
		proc_create("pressure/cpu", 0666, NULL, &psi_cpu_proc_ops);
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
		proc_create("pressure/irq", 0666, NULL, &psi_irq_proc_ops);
#endif
	}
	/* proc 节点创建采用尽力而为策略，initcall 不把局部接口失败升级为启动失败。 */
	return 0;
}
module_init(psi_proc_init);

#endif /* CONFIG_PROC_FS */
