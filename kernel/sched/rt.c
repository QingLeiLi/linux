// SPDX-License-Identifier: GPL-2.0
/*
 * Real-Time Scheduling Class (mapped to the SCHED_FIFO and SCHED_RR
 * policies)
 */
/*
 * 本文件实现 SCHED_FIFO/RR：每个 rt_rq 用 priority bitmap 找到数值最小的
 * 最高优先级队列，FIFO 保持队首直到阻塞/抢占，RR 在时间片耗尽后移到同优先级
 * 队尾。组调度把 task entity 与 group entity 递归串成层级，任何 enqueue/dequeue
 * 都必须从叶到根维持“子 rt_rq 非空才让父 entity 在队列中”的不变量。
 *
 * SMP 负载均衡把可迁移的非当前 RT task 放进 pushable plist；overload 位图和
 * cpupri 只提供无锁候选，最终迁移在双 rq 锁下复核亲和性、优先级和 on_rq。
 * 带宽控制以 period/runtime 限制每层 rt_rq，runtime_lock 保护借用/归还，耗尽时
 * throttle 并摘父 entity，period hrtimer 补充后重新入队。锁顺序、屏障和 push IPI
 * 共同保证不会因过期候选迁错任务或漏扫新 overload CPU。
 */

#include "sched.h"
#include "pelt.h"

/* RR 默认时间片以 tick 表示；sysctl 写入后会统一换算并更新该运行时值。 */
int sched_rr_timeslice = RR_TIMESLICE;
/* More than 4 hours if BW_SHIFT equals 20. */
/* BW_SHIFT 为 20 时该上界超过四小时，用于拒绝定点带宽换算会溢出的配置。 */
static const u64 max_rt_runtime = MAX_BW;

/*
 * period over which we measure -rt task CPU usage in us.
 * default: 1s
 */
/* RT 带宽记账窗口，用户 ABI 单位为微秒；默认一秒。 */
int sysctl_sched_rt_period = 1000000;

/*
 * part of the period that we allow rt tasks to run in us.
 * default: 1s
 */
/* 每窗口允许 RT 类使用的微秒数；-1 表示关闭运行额度限制。 */
int sysctl_sched_rt_runtime = 1000000;

#ifdef CONFIG_SYSCTL
/* RR sysctl 使用毫秒，而调度热路径使用 tick；handler 负责双向换算。 */
static int sysctl_sched_rr_timeslice = (MSEC_PER_SEC * RR_TIMESLICE) / HZ;
static int sched_rt_handler(const struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos);
static int sched_rr_handler(const struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos);
static const struct ctl_table sched_rt_sysctls[] = {
	/* 周期项限制在 1..INT_MAX 微秒，并由统一 handler 校验 period/runtime 组合。 */
	{
		.procname       = "sched_rt_period_us",
		.data           = &sysctl_sched_rt_period,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = sched_rt_handler,
		.extra1         = SYSCTL_ONE,
		.extra2         = SYSCTL_INT_MAX,
	},
	/* runtime 允许 -1；其非负上界跟随 period，保证额度不超过一个窗口。 */
	{
		.procname       = "sched_rt_runtime_us",
		.data           = &sysctl_sched_rt_runtime,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = sched_rt_handler,
		.extra1         = SYSCTL_NEG_ONE,
		.extra2         = (void *)&sysctl_sched_rt_period,
	},
	/* RR 时间片单独使用毫秒视图，写入零时 handler 会恢复默认时间片。 */
	{
		.procname       = "sched_rr_timeslice_ms",
		.data           = &sysctl_sched_rr_timeslice,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = sched_rr_handler,
	},
};

/* 启动期注册 RT period/runtime 与 RR timeslice sysctl；注册失败不向外传播。 */
/*
 * 业务背景：启动阶段把 RT 带宽周期、运行额度和 RR 时间片暴露为 kernel sysctl。
 * 入参：无。
 * 出参/返回：恒返回 0，无输出参；尝试注册 sched_rt_sysctls 表。
 * 注意事项：仅 CONFIG_SYSCTL 下存在并由 late_initcall 调用；注册失败不会传播，不能在运行期重复注册。
 */
static int __init sched_rt_sysctl_init(void)
{
	register_sysctl_init("kernel", sched_rt_sysctls);
	return 0;
}
late_initcall(sched_rt_sysctl_init);
#endif /* CONFIG_SYSCTL */

/* 初始化空 rt_rq 的优先级队列/哨兵位、pushable 集及可选带宽锁和计数。 */
/*
 * 业务背景：每 CPU 或每 task-group 的 RT runqueue 在接收实体前必须建立位图队列、迁移列表和带宽初态。
 * 入参：rt_rq 是不可空输出对象，须指向已分配但尚未投入并发使用的 struct rt_rq。
 * 出参/返回：无直接返回值；初始化 active、优先级缓存、pushable_tasks、排队标志及组调度字段。
 * 注意事项：调用期不得有并发读写；函数不分配内存、不睡眠，CONFIG_RT_GROUP_SCHED 决定带宽字段是否存在。
 */
void init_rt_rq(struct rt_rq *rt_rq)
{
	struct rt_prio_array *array;
	int i;

	/* 每个实时优先级拥有一个 FIFO 链；位图使选择最高优先级无需线性扫描链表。 */
	array = &rt_rq->active;
	for (i = 0; i < MAX_RT_PRIO; i++) {
		INIT_LIST_HEAD(array->queue + i);
		__clear_bit(i, array->bitmap);
	}
	/* delimiter for bitsearch: */
	/* 哨兵位保证 find_first_bit() 即使队列全空也得到合法的 MAX_RT_PRIO。 */
	__set_bit(MAX_RT_PRIO, array->bitmap);

	/* 空队列以最低实时优先级作缓存哨兵，且尚无可推送任务或顶层发布状态。 */
	rt_rq->highest_prio.curr = MAX_RT_PRIO-1;
	rt_rq->highest_prio.next = MAX_RT_PRIO-1;
	rt_rq->overloaded = 0;
	plist_head_init(&rt_rq->pushable_tasks);
	/* We start is dequeued state, because no RT tasks are queued */
	/* 初始没有 RT task，因此顶层 rt_rq 尚未计入 rq->nr_running。 */
	rt_rq->rt_queued = 0;

#ifdef CONFIG_RT_GROUP_SCHED
	/* 组队列从零消耗、未节流开始；runtime 稍后由所属 task group 注入。 */
	rt_rq->rt_time = 0;
	rt_rq->rt_throttled = 0;
	rt_rq->rt_runtime = 0;
	raw_spin_lock_init(&rt_rq->rt_runtime_lock);
	rt_rq->tg = &root_task_group;
#endif
}

#ifdef CONFIG_RT_GROUP_SCHED

static int do_sched_rt_period_timer(struct rt_bandwidth *rt_b, int overrun);

/* period hard hrtimer 追赶 overrun 并补充各 rt_rq；全 idle 时停止，否则重启。 */
/*
 * 业务背景：RT 带宽周期到期时需按遗漏周期数补充 runtime，并在仍有受控队列时继续计时。
 * 入参：timer 是不可空已触发 hard hrtimer，由 container_of 反查所属 rt_bandwidth。
 * 出参/返回：全部队列空闲返回 HRTIMER_NORESTART，否则返回 HRTIMER_RESTART；无输出参。
 * 注意事项：硬中断上下文运行，不能睡眠；rt_runtime_lock 保护 period_active，扫描各 rq 时临时解锁以避免锁嵌套。
 */
static enum hrtimer_restart sched_rt_period_timer(struct hrtimer *timer)
{
	struct rt_bandwidth *rt_b =
		container_of(timer, struct rt_bandwidth, rt_period_timer);
	int idle = 0;
	int overrun;

	/* runtime_lock 串行 period_active 与 timer 推进，避免停止判断和重新启动互相覆盖。 */
	raw_spin_lock(&rt_b->rt_runtime_lock);
	for (;;) {
		/* suspend/长中断可能跨过多个窗口；一次取出 overrun 数并批量补额度。 */
		overrun = hrtimer_forward_now(timer, rt_b->rt_period);
		if (!overrun)
			break;

		/* 扫描 rt_rq 会取得各 rq/runtime 锁，先释放 bandwidth 锁以保持锁序。 */
		raw_spin_unlock(&rt_b->rt_runtime_lock);
		idle = do_sched_rt_period_timer(rt_b, overrun);
		raw_spin_lock(&rt_b->rt_runtime_lock);
	}
	/* 所有受控队列均无需继续计时才撤销 active；后续入队会重新启动。 */
	if (idle)
		rt_b->rt_period_active = 0;
	raw_spin_unlock(&rt_b->rt_runtime_lock);

	return idle ? HRTIMER_NORESTART : HRTIMER_RESTART;
}

/* 初始化 bandwidth 的纳秒 period/runtime、锁和 pinned hard hrtimer。 */
/*
 * 业务背景：每个 RT 带宽域需要独立周期、额度和固定 CPU 的 hard hrtimer 执行补充。
 * 入参：rt_b 是不可空未发布输出对象；period/runtime 均以纳秒计，runtime 可为 RUNTIME_INF。
 * 出参/返回：无直接返回值；写入 period/runtime，初始化锁和 CLOCK_MONOTONIC hrtimer。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；初始化时无并发，函数不启动 timer，调用者负责对象生命周期长于回调。
 */
void init_rt_bandwidth(struct rt_bandwidth *rt_b, u64 period, u64 runtime)
{
	rt_b->rt_period = ns_to_ktime(period);
	rt_b->rt_runtime = runtime;

	raw_spin_lock_init(&rt_b->rt_runtime_lock);

	hrtimer_setup(&rt_b->rt_period_timer, sched_rt_period_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_HARD);
}

/* runtime_lock 下仅首次激活 period timer，并立即 forward 修复 DL 已消耗的旧周期。 */
/*
 * 业务背景：首个受限 RT task 入队时启动周期计时，并修正 DL 已经推进带宽但未重置 RT period 的情况。
 * 入参：rt_b 是不可空输入/输出带宽对象，timer 已由 init_rt_bandwidth() 初始化。
 * 出参/返回：无直接返回值；首次调用设置 period_active 并启动 timer，已激活时无变化。
 * 注意事项：内部持 rt_runtime_lock、不可睡眠；hrtimer 使用 ABS_PINNED_HARD，对象销毁前必须同步取消。
 */
static inline void do_start_rt_bandwidth(struct rt_bandwidth *rt_b)
{
	raw_spin_lock(&rt_b->rt_runtime_lock);
	if (!rt_b->rt_period_active) {
		/* active 在启动 timer 前发布，使并发入队不会重复挂载同一 hrtimer。 */
		rt_b->rt_period_active = 1;
		/*
		 * SCHED_DEADLINE updates the bandwidth, as a run away
		 * RT task with a DL task could hog a CPU. But DL does
		 * not reset the period. If a deadline task was running
		 * without an RT task running, it can cause RT tasks to
		 * throttle when they start up. Kick the timer right away
		 * to update the period.
		 */
		/* DL 也会消耗共享带宽；立即推进到当前周期，避免 RT 首次运行继承陈旧消耗。 */
		hrtimer_forward_now(&rt_b->rt_period_timer, ns_to_ktime(0));
		hrtimer_start_expires(&rt_b->rt_period_timer,
				      HRTIMER_MODE_ABS_PINNED_HARD);
	}
	raw_spin_unlock(&rt_b->rt_runtime_lock);
}

/* 仅在全局带宽启用且 runtime 有限时启动周期计时器。 */
/*
 * 业务背景：无限额度或全局禁用带宽控制时不需要周期补充，其他队列首次运行必须启动 timer。
 * 入参：rt_b 是不可空输入/输出带宽对象。
 * 出参/返回：无直接返回值；满足条件时可能激活 rt_b 的周期 timer。
 * 注意事项：可在调度路径调用且不睡眠；runtime 读取需由上层配置串行保证，实际激活由 runtime_lock 防重。
 */
static void start_rt_bandwidth(struct rt_bandwidth *rt_b)
{
	if (!rt_bandwidth_enabled() || rt_b->rt_runtime == RUNTIME_INF)
		return;

	do_start_rt_bandwidth(rt_b);
}

/* 同步取消 bandwidth hrtimer，返回后回调不再访问 @rt_b。 */
/*
 * 业务背景：task group 销毁前必须阻止周期回调继续访问即将释放的 rt_bandwidth。
 * 入参：rt_b 是不可空输入/输出带宽对象，timer 已初始化且可能活动。
 * 出参/返回：无直接返回值；同步取消 timer，无输出参。
 * 注意事项：hrtimer_cancel() 可能等待正在运行的回调，须在可睡眠上下文且不持回调所需锁时调用。
 */
static void destroy_rt_bandwidth(struct rt_bandwidth *rt_b)
{
	hrtimer_cancel(&rt_b->rt_period_timer);
}

#define rt_entity_is_task(rt_se) (!(rt_se)->my_q)

/* 由叶子 rt entity 反查 task；group entity 属于调用错误并 WARN。 */
/*
 * 业务背景：叶子 sched_rt_entity 内嵌于 task_struct，迁移和调度路径需恢复拥有它的 task。
 * 入参：rt_se 是不可空、必须满足 rt_entity_is_task() 的借用实体。
 * 出参/返回：返回包含该实体的 task_struct 借用指针，不增加引用、无输出参。
 * 注意事项：group entity 会 WARN 且 container_of 结果不可用；调用者须以 rq/RCU 或 task 引用保证生命周期。
 */
static inline struct task_struct *rt_task_of(struct sched_rt_entity *rt_se)
{
	WARN_ON_ONCE(!rt_entity_is_task(rt_se));

	return container_of(rt_se, struct task_struct, rt);
}

/* 从顶层或组 rt_rq 找到所属 CPU rq；调用者必须处于对应锁/RCU 保护。 */
/*
 * 业务背景：层级 RT 队列最终都归属某个物理 CPU rq，带宽和迁移逻辑需回到该顶层对象。
 * 入参：rt_rq 是不可空借用队列，已由 init_tg_rt_entry() 绑定 rq。
 * 出参/返回：返回所属 struct rq 借用指针，不增加引用、无输出参。
 * 注意事项：组调度关闭却传非 root group 会 WARN；调用者负责 rq 锁或热插拔稳定性，函数不睡眠。
 */
static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	/* Cannot fold with non-CONFIG_RT_GROUP_SCHED version, layout */
	WARN_ON(!rt_group_sched_enabled() && rt_rq->tg != &root_task_group);
	return rt_rq->rq;
}

/* 返回 entity 当前排队的 rt_rq，task/group 两类共用。 */
/*
 * 业务背景：实体的 enqueue/dequeue 操作必须定位其当前层级的父 rt_rq。
 * 入参：rt_se 是不可空借用实体，其 rt_rq 已完成初始化且生命周期稳定。
 * 出参/返回：返回 rt_se->rt_rq 借用指针，不增加引用、无输出参。
 * 注意事项：组调度关闭时非 root 布局会 WARN；不取锁，调用者须在 rq 锁下防止层级关系变化。
 */
static inline struct rt_rq *rt_rq_of_se(struct sched_rt_entity *rt_se)
{
	WARN_ON(!rt_group_sched_enabled() && rt_se->rt_rq->tg != &root_task_group);
	return rt_se->rt_rq;
}

/* 经 entity 的 rt_rq 返回底层 CPU rq。 */
/*
 * 业务背景：实体级操作需要把层级节点映射回负责串行调度状态的 CPU runqueue。
 * 入参：rt_se 是不可空借用实体，rt_se->rt_rq 必须已绑定底层 rq。
 * 出参/返回：返回所属 struct rq 借用指针，不增加引用、无输出参。
 * 注意事项：不取锁、不校验空指针；非预期组布局会 WARN，调用者须保证实体和 rq 生命周期。
 */
static inline struct rq *rq_of_rt_se(struct sched_rt_entity *rt_se)
{
	struct rt_rq *rt_rq = rt_se->rt_rq;

	WARN_ON(!rt_group_sched_enabled() && rt_rq->tg != &root_task_group);
	return rt_rq->rq;
}

/* 从每 CPU rt_rq 摘除 @tg 的父 entity，阻止新任务沿该组进入 RT 层级。 */
/*
 * 业务背景：RT task group 注销时先停止其周期回调，才能安全进入后续 per-CPU 对象释放阶段。
 * 入参：tg 是不可空输入/输出 task group，可能是组调度禁用时的占位对象。
 * 出参/返回：无直接返回值、无输出参；启用组调度且已分配 rt_se 时同步销毁带宽 timer。
 * 注意事项：调用者须先阻止新任务/引用进入；destroy 可能等待回调，不能持其 runtime/rq 锁。
 */
void unregister_rt_sched_group(struct task_group *tg)
{
	if (!rt_group_sched_enabled())
		return;

	if (tg->rt_se)
		destroy_rt_bandwidth(&tg->rt_bandwidth);
}

/* 释放 @tg 所有 per-CPU rt_rq/entity 数组；调用前必须已注销且无引用。 */
/*
 * 业务背景：task group 最终销毁阶段回收每个 possible CPU 的 RT 队列、父实体和索引数组。
 * 入参：tg 是不可空输入/输出 task group，其 RT timer 已注销且成员不再引用这些对象。
 * 出参/返回：无直接返回值；释放 tg->rt_rq/rt_se 所拥有内存，无输出参。
 * 注意事项：可睡眠；不清空指针且不处理并发，重复调用或尚有引用时会产生 UAF/双重释放。
 */
void free_rt_sched_group(struct task_group *tg)
{
	int i;

	if (!rt_group_sched_enabled())
		return;

	/* per-CPU 子对象先释放，最后释放保存这些拥有指针的索引数组。 */
	for_each_possible_cpu(i) {
		if (tg->rt_rq)
			kfree(tg->rt_rq[i]);
		if (tg->rt_se)
			kfree(tg->rt_se[i]);
	}

	/* 数组本身由 alloc_rt_sched_group() 分配，内容已不再被任何 CPU 引用。 */
	kfree(tg->rt_rq);
	kfree(tg->rt_se);
}

/* 初始化 @cpu 的组 rt_rq 和连接 parent 的 rt entity；root 没有父 entity。 */
/*
 * 业务背景：组调度为每 CPU 构造 rt_rq，并用 group entity 把子队列递归挂到父组队列。
 * 入参：tg/rt_rq 是不可空输入/输出对象；rt_se 可为空表示 root；cpu 是有效 possible CPU；parent 可为空表示挂到 rq 根 RT 队列。
 * 出参/返回：无直接返回值；写 tg 的 per-CPU 槽、rt_rq 归属及 rt_se 的父子链接。
 * 注意事项：仅初始化阶段调用、不得并发；不取得所有权外引用，所有对象必须至少活到 task group 注销。
 */
void init_tg_rt_entry(struct task_group *tg, struct rt_rq *rt_rq,
		struct sched_rt_entity *rt_se, int cpu,
		struct sched_rt_entity *parent)
{
	struct rq *rq = cpu_rq(cpu);

	/* 先建立子队列到物理 rq/task group 的归属，再把指针发布进 per-CPU 槽。 */
	rt_rq->highest_prio.curr = MAX_RT_PRIO-1;
	rt_rq->rt_nr_boosted = 0;
	rt_rq->rq = rq;
	rt_rq->tg = tg;

	tg->rt_rq[cpu] = rt_rq;
	tg->rt_se[cpu] = rt_se;

	/* root task group 直接使用 rq->rt，没有代表它的父层 entity。 */
	if (!rt_se)
		return;

	/* 一级组挂到 CPU 根 RT 队列；更深层组挂到 parent 所代表的子队列。 */
	if (!parent)
		rt_se->rt_rq = &rq->rt;
	else
		rt_se->rt_rq = parent->my_q;

	rt_se->my_q = rt_rq;
	rt_se->parent = parent;
	INIT_LIST_HEAD(&rt_se->run_list);
}

/* 分配并逐 CPU 初始化组 RT 状态；失败逆序释放，成功返回 1。 */
/*
 * 业务背景：新 task group 需一次性建立所有 possible CPU 的 RT 队列和层级实体，任一分配失败都不能发布半成品。
 * 入参：tg 是不可空输出 task group；parent 是不可空只读父组，其 rt_se 数组已初始化。
 * 出参/返回：成功返回 1 并由 tg 拥有所有数组/对象，失败返回 0；组调度禁用时视为无需分配而成功。
 * 注意事项：使用 GFP_KERNEL 可睡眠；失败路径依赖 task-group 上层清理已写数组槽，调用时不得并发访问 tg。
 */
int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent)
{
	struct rt_rq *rt_rq;
	struct sched_rt_entity *rt_se;
	int i;

	if (!rt_group_sched_enabled())
		return 1;

	/* 两个索引数组先建立；任一失败都保持 task group 不可发布。 */
	tg->rt_rq = kzalloc_objs(rt_rq, nr_cpu_ids);
	if (!tg->rt_rq)
		goto err;
	tg->rt_se = kzalloc_objs(rt_se, nr_cpu_ids);
	if (!tg->rt_se)
		goto err;

	/* 新组初始 runtime 为零，管理员配置额度前不能消耗普通 RT 带宽。 */
	init_rt_bandwidth(&tg->rt_bandwidth, ktime_to_ns(global_rt_period()), 0);

	for_each_possible_cpu(i) {
		/* 按 NUMA 节点分配该 CPU 的队列和父 entity，减少调度热路径远端访问。 */
		rt_rq = kzalloc_node(sizeof(struct rt_rq),
				     GFP_KERNEL, cpu_to_node(i));
		if (!rt_rq)
			goto err;

		rt_se = kzalloc_node(sizeof(struct sched_rt_entity),
				     GFP_KERNEL, cpu_to_node(i));
		if (!rt_se)
			goto err_free_rq;

		/* 完整初始化后才写入 tg 槽；此前失败对象仍由当前迭代负责释放。 */
		init_rt_rq(rt_rq);
		rt_rq->rt_runtime = tg->rt_bandwidth.rt_runtime;
		init_tg_rt_entry(tg, rt_rq, rt_se, i, parent->rt_se[i]);
	}

	return 1;

err_free_rq:
	/* entity 分配失败时 rt_rq 尚未发布，直接释放当前迭代的孤立对象。 */
	kfree(rt_rq);
err:
	/* 已发布到 tg 数组的早期 CPU 对象由上层统一 free_rt_sched_group() 回收。 */
	return 0;
}

#else /* !CONFIG_RT_GROUP_SCHED: */

#define rt_entity_is_task(rt_se) (1)

/*
 * 业务背景：未启用组调度时所有 RT entity 都是 task 叶子，可直接反查拥有者。
 * 入参：rt_se 是不可空、内嵌于 task_struct 的借用实体。
 * 出参/返回：返回所属 task_struct 借用指针，不增加引用、无输出参。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；错误传入非 task 内存会产生非法 container_of 结果。
 */
static inline struct task_struct *rt_task_of(struct sched_rt_entity *rt_se)
{
	return container_of(rt_se, struct task_struct, rt);
}

/*
 * 业务背景：无组层级时 rt_rq 直接内嵌于 CPU rq，可用固定布局反查。
 * 入参：rt_rq 是不可空、必须内嵌于 struct rq 的借用对象。
 * 出参/返回：返回所属 struct rq 借用指针，不增加引用、无输出参。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；调用者须以 rq 锁或 hotplug 约束保证生命周期。
 */
static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	return container_of(rt_rq, struct rq, rt);
}

/*
 * 业务背景：无组调度时 entity 所属 CPU rq 等同其 task 当前 rq。
 * 入参：rt_se 是不可空 task entity 借用指针。
 * 出参/返回：返回 task 当前 rq 借用指针，不增加引用、无输出参。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；迁移可改变结果，调用者必须持相应 rq/pi 锁。
 */
static inline struct rq *rq_of_rt_se(struct sched_rt_entity *rt_se)
{
	struct task_struct *p = rt_task_of(rt_se);

	return task_rq(p);
}

/*
 * 业务背景：无组调度时从 task entity 的 CPU rq 取得唯一顶层 RT 队列。
 * 入参：rt_se 是不可空 task entity 借用指针。
 * 出参/返回：返回所属 rq->rt 借用指针，不增加引用、无输出参。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；调用者须锁定 task rq，避免并发迁移改变归属。
 */
static inline struct rt_rq *rt_rq_of_se(struct sched_rt_entity *rt_se)
{
	struct rq *rq = rq_of_rt_se(rt_se);

	return &rq->rt;
}

/*
 * 业务背景：未启用 RT 组调度时没有 per-group timer 需要注销，保留统一生命周期接口。
 * 入参：tg 是未使用输入，可为 NULL，因为 stub 不解引用。
 * 出参/返回：无直接返回值、无输出参和副作用。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；必须保持不可睡眠的中性语义。
 */
void unregister_rt_sched_group(struct task_group *tg) { }

/*
 * 业务背景：未启用 RT 组调度时没有 per-group RT 内存需要释放。
 * 入参：tg 是未使用输入，可为 NULL，因为 stub 不解引用。
 * 出参/返回：无直接返回值、无输出参和副作用。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；不获取所有权、不睡眠。
 */
void free_rt_sched_group(struct task_group *tg) { }

/*
 * 业务背景：未启用 RT 组调度时 task group 创建无需分配 RT 层级对象，但上层仍使用统一成功约定。
 * 入参：tg/parent 均为未使用输入，可为 NULL，因为 stub 不解引用。
 * 出参/返回：恒返回 1 表示无需资源且初始化成功；无输出参。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；不分配内存、不睡眠。
 */
int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent)
{
	return 1;
}
#endif /* !CONFIG_RT_GROUP_SCHED */

/* 当前 RT 优先级降低且 root_domain overload 时返回 true，请求稍后 pull。 */
/*
 * 业务背景：当前 task 离开或降优先级后，本 rq 可能需要从其他 overload CPU 拉取更高优先级 RT task。
 * 入参：rq 是不可空当前 CPU 队列；prev 是不可空刚切出的 task，二者均为只读借用。
 * 出参/返回：rq 在线且当前最高 RT prio 数值大于 prev->prio 时返回 true，否则 false；无输出参。
 * 注意事项：调用者须持 rq 锁；结果只是安排 pull callback 的条件，不能证明远端存在可迁移任务。
 */
static inline bool need_pull_rt_task(struct rq *rq, struct task_struct *prev)
{
	/* Try to pull RT tasks here if we lower this rq's prio */
	return rq->online && rq->rt.highest_prio.curr > prev->prio;
}

/* 无锁读取 root_domain 中 overload rt_rq 数；与发布屏障配对后再扫 mask。 */
/*
 * 业务背景：pull 路径先用 root_domain 计数快速判断是否值得扫描 overload CPU mask。
 * 入参：rq 是不可空借用 CPU runqueue，其 rd 生命周期由调度域/RCU 上下文保证。
 * 出参/返回：返回当前 overload rt_rq 原子计数，0 表示无需扫描；无输出参。
 * 注意事项：无锁快照可立即变化；非零后读取 rto_mask 必须执行与 rt_set_overload() 配对的屏障。
 */
static inline int rt_overloaded(struct rq *rq)
{
	return atomic_read(&rq->rd->rto_count);
}

/* rq 锁下先置 rto_mask 再发布 overload count，使 pull reader 不漏掉该 CPU。 */
/*
 * 业务背景：当一个 rq 出现多个可运行 RT task 时，将其发布到 root_domain 供其他 CPU 拉取。
 * 入参：rq 是不可空输入/输出 runqueue，调用者必须持有 rq 锁。
 * 出参/返回：无直接返回值；在线 rq 的 CPU 位加入 rto_mask 并递增 rto_count。
 * 注意事项：先写 mask、smp_wmb() 后增计数；必须只在未标记到已标记转换时调用，否则计数失衡。
 */
static inline void rt_set_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	cpumask_set_cpu(rq->cpu, rq->rd->rto_mask);
	/*
	 * Make sure the mask is visible before we set
	 * the overload count. That is checked to determine
	 * if we should look at the mask. It would be a shame
	 * if we looked at the mask, but the mask was not
	 * updated yet.
	 *
	 * Matched by the barrier in pull_rt_task().
	 */
	smp_wmb();
	atomic_inc(&rq->rd->rto_count);
}

/* rq 锁下撤销 overload CPU 位和计数，保持二者最终一致。 */
/*
 * 业务背景：rq 不再有多余可迁移 RT task 时，从 root_domain overload 集合撤销发布。
 * 入参：rq 是不可空输入/输出 runqueue，调用者必须持有 rq 锁。
 * 出参/返回：无直接返回值；在线 rq 的 rto_count 减一并清除 rto_mask CPU 位。
 * 注意事项：只能与先前 rt_set_overload() 一一配对；离线 rq 不修改共享域状态，误配会使计数失真。
 */
static inline void rt_clear_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	/* the order here really doesn't matter */
	atomic_dec(&rq->rd->rto_count);
	cpumask_clear_cpu(rq->cpu, rq->rd->rto_mask);
}

/* 返回 pushable plist 是否非空；无锁调用只能作为需要加 rq 锁复核的提示。 */
/*
 * 业务背景：RT balance callback 需快速判断本 rq 是否存在非当前、可迁移的 RT task。
 * 入参：rq 是不可空只读借用 runqueue。
 * 出参/返回：pushable_tasks 非空返回 1，否则 0；无输出参。
 * 注意事项：通常应在 rq 锁下读取；无锁结果仅作提示，真正迁移必须重新验证 task 状态和 affinity。
 */
static inline int has_pushable_tasks(struct rq *rq)
{
	return !plist_head_empty(&rq->rt.pushable_tasks);
}

/* 每 CPU push/pull callback 节点由 rq callback 链去重；节点生命周期覆盖整个 CPU 调度器生命周期。 */
static DEFINE_PER_CPU(struct balance_callback, rt_push_head);
static DEFINE_PER_CPU(struct balance_callback, rt_pull_head);

static void push_rt_tasks(struct rq *);
static void pull_rt_task(struct rq *);

/* 将 push balance callback 挂到 rq，出锁后的 callback 迁移多余 RT task。 */
/*
 * 业务背景：rq 出现可迁移 RT task 时延后到安全的 balance callback 阶段执行 push，缩短当前锁区。
 * 入参：rq 是不可空输入/输出 runqueue，调用者持 rq 锁。
 * 出参/返回：无直接返回值；存在 pushable task 时把 per-CPU rt_push_head 排入 callback 链。
 * 注意事项：queue helper 负责去重；任务状态仍可能变化，push_rt_tasks() 必须再次验证。
 */
static inline void rt_queue_push_tasks(struct rq *rq)
{
	if (!has_pushable_tasks(rq))
		return;

	queue_balance_callback(rq, &per_cpu(rt_push_head, rq->cpu), push_rt_tasks);
}

/* 将 pull callback 挂到 rq，低优先级切入时从其他 overload rq 拉取任务。 */
/*
 * 业务背景：本 rq 可接纳更高优先级 RT task 时，延迟扫描 root_domain 并尝试从 overload rq 拉取。
 * 入参：rq 是不可空输入/输出 runqueue，调用者持 rq 锁。
 * 出参/返回：无直接返回值；把 per-CPU rt_pull_head 排入 rq 的 balance callback 链。
 * 注意事项：只安排工作、不保证迁移成功；callback 执行时需用屏障、双 rq 锁和 affinity 重新验证。
 */
static inline void rt_queue_pull_task(struct rq *rq)
{
	queue_balance_callback(rq, &per_cpu(rt_pull_head, rq->cpu), pull_rt_task);
}

/* 把非当前且可迁移 @p 按 priority 加入 pushable plist，并更新 next highest。 */
/*
 * 业务背景：SMP RT 均衡按优先级维护可从本 rq 推走的 task，供 push 路径 O(1) 取得最高优先候选。
 * 入参：rq 是不可空输入/输出 runqueue；p 是不可空可迁移 RT task，调用者持 rq 锁且保证 p 不为 current。
 * 出参/返回：无直接返回值；重插 p 的 plist 节点，更新 next priority，并在首次 overload 时发布 rq。
 * 注意事项：不取得 task 引用；重复节点先 del 再 add，误把 current/单 CPU task 加入会破坏迁移候选不变量。
 */
static void enqueue_pushable_task(struct rq *rq, struct task_struct *p)
{
	/* 节点可能因优先级改变而已在表中；先删再按当前 p->prio 重新排序。 */
	plist_del(&p->pushable_tasks, &rq->rt.pushable_tasks);
	plist_node_init(&p->pushable_tasks, p->prio);
	plist_add(&p->pushable_tasks, &rq->rt.pushable_tasks);

	/* Update the highest prio pushable task */
	/* 数值越小优先级越高，缓存只在新 task 更高时向上收紧。 */
	if (p->prio < rq->rt.highest_prio.next)
		rq->rt.highest_prio.next = p->prio;

	/* 首个可推送 task 使该 CPU 对其他 pull 扫描者可见。 */
	if (!rq->rt.overloaded) {
		rt_set_overload(rq);
		rq->rt.overloaded = 1;
	}
}

/* 从 pushable plist 摘 @p，并用新表头刷新 highest_prio.next。 */
/*
 * 业务背景：task 运行、阻塞或失去迁移资格时必须从 pushable 集撤销，并维护 root-domain overload 状态。
 * 入参：rq 是不可空输入/输出 runqueue；p 是不可空且当前已在 rq->rt.pushable_tasks 的 task。
 * 出参/返回：无直接返回值；删除 p，刷新 next，列表变空时清 overload 发布。
 * 注意事项：调用者持 rq 锁；函数会复用局部 p 指向新表头，不改变调用者变量，未入表调用会破坏 plist。
 */
static void dequeue_pushable_task(struct rq *rq, struct task_struct *p)
{
	/* plist_del() 要求节点确实在本 rq 列表；调用者在状态转换前保证该前置条件。 */
	plist_del(&p->pushable_tasks, &rq->rt.pushable_tasks);

	/* Update the new highest prio pushable task */
	/* 表头始终是数值最小的最高优先候选，可直接重建 highest_prio.next。 */
	if (has_pushable_tasks(rq)) {
		p = plist_first_entry(&rq->rt.pushable_tasks,
				      struct task_struct, pushable_tasks);
		rq->rt.highest_prio.next = p->prio;
	} else {
		rq->rt.highest_prio.next = MAX_RT_PRIO-1;

		/* 最后一个候选消失时按“共享位图后本地标志”的顺序撤销发布。 */
		if (rq->rt.overloaded) {
			rt_clear_overload(rq);
			rq->rt.overloaded = 0;
		}
	}
}

static void enqueue_top_rt_rq(struct rt_rq *rt_rq);
static void dequeue_top_rt_rq(struct rt_rq *rt_rq, unsigned int count);

/* run_list 非空表示 entity 当前在某个 priority 队列中。 */
/*
 * 业务背景：层级 enqueue/dequeue 需要快速判断 entity 是否已经发布到父 rt_rq。
 * 入参：rt_se 是不可空只读借用实体。
 * 出参/返回：rt_se->on_rq 非零返回真值，否则 0；无输出参。
 * 注意事项：无锁读取，调用者通常须持所属 rq 锁；仅表示 RT 队列成员关系，不等同 task_on_rq 状态。
 */
static inline int on_rt_rq(struct sched_rt_entity *rt_se)
{
	return rt_se->on_rq;
}

#ifdef CONFIG_UCLAMP_TASK
/*
 * Verify the fitness of task @p to run on @cpu taking into account the uclamp
 * settings.
 *
 * This check is only important for heterogeneous systems where uclamp_min value
 * is higher than the capacity of a @cpu. For non-heterogeneous system this
 * function will always return true.
 *
 * The function will return true if the capacity of the @cpu is >= the
 * uclamp_min and false otherwise.
 *
 * Note that uclamp_min will be clamped to uclamp_max if uclamp_min
 * > uclamp_max.
 */
/* 在非对称容量系统判断 @cpu 是否满足 RT task uclamp/容量需求；否则恒 true。 */
/*
 * 业务背景：异构 CPU 选核时应避免把 RT task 放到低于其有效 uclamp_min 的 CPU。
 * 入参：p 是不可空只读 task；cpu 是有效 online CPU 编号，仅输入。
 * 出参/返回：对称系统或 cpu capacity 不低于 min(effective min,max) 时返回 true，否则 false；无输出参。
 * 注意事项：仅 CONFIG_UCLAMP_TASK 实现；无锁快照只用于候选排序，最终放置仍受 affinity/hotplug 复核。
 */
static inline bool rt_task_fits_capacity(struct task_struct *p, int cpu)
{
	unsigned int min_cap;
	unsigned int max_cap;
	unsigned int cpu_cap;

	/* Only heterogeneous systems can benefit from this check */
	/* 对称系统不存在容量更合适的 CPU，直接放行可避免无收益的 clamp 读取。 */
	if (!sched_asym_cpucap_active())
		return true;

	/* 有效值已经合并 task、cgroup 与系统约束；异常 min>max 时以 max 封顶。 */
	min_cap = uclamp_eff_value(p, UCLAMP_MIN);
	max_cap = uclamp_eff_value(p, UCLAMP_MAX);

	cpu_cap = arch_scale_cpu_capacity(cpu);

	return cpu_cap >= min(min_cap, max_cap);
}
#else /* !CONFIG_UCLAMP_TASK: */
/*
 * 业务背景：未启用 uclamp 时 RT 选核没有容量下限门禁，保留统一调用点。
 * 入参：p/cpu 均为未使用输入，p 可为 NULL，因为 stub 不解引用。
 * 出参/返回：恒返回 true；无输出参和副作用。
 * 注意事项：仅 !CONFIG_UCLAMP_TASK 下存在；不校验 cpu 范围、不取锁、不睡眠。
 */
static inline bool rt_task_fits_capacity(struct task_struct *p, int cpu)
{
	return true;
}
#endif /* !CONFIG_UCLAMP_TASK */

#ifdef CONFIG_RT_GROUP_SCHED

/* 返回 @rt_rq 所属 bandwidth 的每周期 runtime 纳秒。 */
/*
 * 业务背景：RT 计费路径需要读取当前组在此 CPU 上可用的本地 runtime 配额。
 * 入参：rt_rq 是不可空只读借用队列。
 * 出参/返回：返回 rt_rq->rt_runtime，单位纳秒，可为 RUNTIME_INF；无输出参。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；锁外读取可能是借用调整中的快照，精确修改须持 runtime_lock。
 */
static inline u64 sched_rt_runtime(struct rt_rq *rt_rq)
{
	return rt_rq->rt_runtime;
}

/* 返回 @rt_rq 所属 bandwidth 的 period 纳秒。 */
/*
 * 业务背景：带宽借用和超额判断需把 task group 的 hrtimer 周期转换成纳秒标量。
 * 入参：rt_rq 是不可空只读借用队列，其 tg/bandwidth 已初始化。
 * 出参/返回：返回所属 task group 的 RT period 纳秒值；无输出参。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；不取锁，配置更新期间只保证调用上下文允许的快照。
 */
static inline u64 sched_rt_period(struct rt_rq *rt_rq)
{
	return ktime_to_ns(rt_rq->tg->rt_bandwidth.rt_period);
}

typedef struct task_group *rt_rq_iter_t;

/* RCU 下按深度优先次序返回下一个 task_group，用于遍历各层 rt_rq。 */
/*
 * 业务背景：带宽 timer 与 CPU hotplug 要遍历所有非 autogroup 的 task group RT 队列。
 * 入参：tg 是不可空当前 task group 借用指针，必须位于全局 task_groups RCU 链表。
 * 出参/返回：返回下一个非 autogroup 借用指针，遍历结束返回 NULL；无输出参。
 * 注意事项：调用者须持 RCU 读锁；组调度运行时禁用会 WARN/返回 NULL，返回指针不得带出保护期。
 */
static inline struct task_group *next_task_group(struct task_group *tg)
{
	/* 运行时关闭层级调度时只有 root 合法，遍历在根后立即终止。 */
	if (!rt_group_sched_enabled()) {
		WARN_ON(tg != &root_task_group);
		return NULL;
	}

	/* autogroup 不参与 RT group bandwidth 层级，连续跳过直到普通组或链表头。 */
	do {
		tg = list_entry_rcu(tg->list.next,
			typeof(struct task_group), list);
	} while (&tg->list != &task_groups && task_group_is_autogroup(tg));

	/* 循环链表头不是 task_group，转换成 NULL 供 for 循环自然结束。 */
	if (&tg->list == &task_groups)
		tg = NULL;

	return tg;
}

#define for_each_rt_rq(rt_rq, iter, rq)					\
	for (iter = &root_task_group;					\
		iter && (rt_rq = iter->rt_rq[cpu_of(rq)]);		\
		iter = next_task_group(iter))

#define for_each_sched_rt_entity(rt_se) \
	for (; rt_se; rt_se = rt_se->parent)

/* group entity 返回其子 rt_rq；task entity 返回 NULL。 */
/*
 * 业务背景：层级代码用 my_q 区分代表 task 的叶子实体与代表子 task group 的父实体。
 * 入参：rt_se 是不可空只读借用实体。
 * 出参/返回：group entity 返回其子 rt_rq 借用指针，task entity 返回 NULL；无输出参。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；调用者以 rq 锁/RCU 保证实体与子队列生命周期。
 */
static inline struct rt_rq *group_rt_rq(struct sched_rt_entity *rt_se)
{
	return rt_se->my_q;
}

static void enqueue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags);
static void dequeue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags);

/* 子 rt_rq 有 runnable 且未 throttle 时把父 entity 逐层入队到 CPU rq。 */
/*
 * 业务背景：组内 RT 队列从不可选变为可运行时，必须把代表该组的 entity 向父层发布直到顶层 rq。
 * 入参：rt_rq 是不可空输入/输出子队列，调用者持所属 CPU rq 锁。
 * 出参/返回：无直接返回值；必要时入队父 entity/顶层计数，并可能请求 current 重调度。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；已 throttle 或无 runnable 时不发布，重复入队由 on_rt_rq 门禁避免。
 */
static void sched_rt_rq_enqueue(struct rt_rq *rt_rq)
{
	struct task_struct *donor = rq_of_rt_rq(rt_rq)->donor;
	struct rq *rq = rq_of_rt_rq(rt_rq);
	struct sched_rt_entity *rt_se;

	int cpu = cpu_of(rq);

	/* root 的 per-CPU 槽为 NULL；普通组槽保存向父队列发布的代表 entity。 */
	rt_se = rt_rq->tg->rt_se[cpu];

	/* 只有子队列真实非空才允许父层可选，避免调度器落入空组。 */
	if (rt_rq->rt_nr_running) {
		if (!rt_se)
			enqueue_top_rt_rq(rt_rq);
		else if (!on_rt_rq(rt_se))
			enqueue_rt_entity(rt_se, 0);

		/* 新发布的最高 RT 优先级压过当前 donor 时立即请求重新调度。 */
		if (rt_rq->highest_prio.curr < donor->prio)
			resched_curr(rq);
	}
}

/* 子 rt_rq 变空或 throttle 时逐层摘除父 entity，避免选择不可运行层级。 */
/*
 * 业务背景：组内 RT 队列失去可运行实体时，要撤销父层可选性；root 队列还需更新顶层计数和频率信号。
 * 入参：rt_rq 是不可空输入/输出队列，调用者持所属 CPU rq 锁。
 * 出参/返回：无直接返回值；摘除顶层 rt_rq 或已排队的父 entity，并可能通知 cpufreq。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；必须与 enqueue 层级不变量配对，不能在父 entity 未入队时强行删除。
 */
static void sched_rt_rq_dequeue(struct rt_rq *rt_rq)
{
	struct sched_rt_entity *rt_se;
	int cpu = cpu_of(rq_of_rt_rq(rt_rq));

	rt_se = rt_rq->tg->rt_se[cpu];

	/* root 没有父 entity，直接从 CPU 顶层 RT 计数撤销全部剩余 runnable。 */
	if (!rt_se) {
		dequeue_top_rt_rq(rt_rq, rt_rq->rt_nr_running);
		/* Kick cpufreq (see the comment in kernel/sched/sched.h). */
		cpufreq_update_util(rq_of_rt_rq(rt_rq), 0);
	}
	/* 子组只在代表 entity 已发布时摘除，避免重复 dequeue 破坏队列链。 */
	else if (on_rt_rq(rt_se))
		dequeue_rt_entity(rt_se, 0);
}

/* 只有 marked throttled 且没有 PI-boost entity 时才真正阻止选择。 */
/*
 * 业务背景：带宽耗尽通常 throttle RT 队列，但 PI boost task 必须继续运行以解除优先级反转。
 * 入参：rt_rq 是不可空只读借用队列。
 * 出参/返回：已标记 throttled 且 rt_nr_boosted 为 0 时返回真值，否则 0；无输出参。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；调用者须持 rq/runtime 允许的保护，结果可随 PI 状态变化。
 */
static inline int rt_rq_throttled(struct rt_rq *rt_rq)
{
	return rt_rq->rt_throttled && !rt_rq->rt_nr_boosted;
}

/* 沿 entity/parent 检查 task 是否因 PI 获得高于 normal_prio 的 RT 优先级。 */
/*
 * 业务背景：更新层级 boosted 计数时，需要判断 task 叶子或子组是否包含 PI 提升实体。
 * 入参：rt_se 是不可空只读借用 task/group 实体。
 * 出参/返回：group 返回子队列 rt_nr_boosted 是否非零，task 返回 prio!=normal_prio；无输出参。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；须在 rq/PI 锁协议内读取，普通策略差异不能误当永久 RT 配额。
 */
static int rt_se_boosted(struct sched_rt_entity *rt_se)
{
	struct rt_rq *rt_rq = group_rt_rq(rt_se);
	struct task_struct *p;

	/* group entity 的 boosted 状态由其子队列聚合，避免递归扫描所有 task。 */
	if (rt_rq)
		return !!rt_rq->rt_nr_boosted;

	p = rt_task_of(rt_se);
	return p->prio != p->normal_prio;
}

/* bandwidth timer 需要扫描的 CPU 集；通常是 online CPU。 */
/*
 * 业务背景：共享 RT bandwidth 的周期回调只需扫描当前 root_domain span 内的 CPU 队列。
 * 入参：无，隐式使用 this_rq() 的 root_domain。
 * 出参/返回：返回 rd->span 的只读借用 cpumask 指针；无输出参。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；调用者须处于能稳定 root_domain 生命周期的调度/RCU 上下文。
 */
static inline const struct cpumask *sched_rt_period_mask(void)
{
	return this_rq()->rd->span;
}

static inline
/* 由 bandwidth 与 @cpu 找到对应组/根 rt_rq；调用者只借用。 */
/*
 * 业务背景：周期和借用逻辑持有 bandwidth 时需定位同一 task group 在指定 CPU 上的 rt_rq。
 * 入参：rt_b 是不可空且内嵌于 task_group 的借用对象；cpu 是有效 possible CPU 编号。
 * 出参/返回：返回该 group 的 per-CPU rt_rq 借用指针，可能在未分配槽上为 NULL；无输出参。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；调用者须以 task-group/RCU 生命周期保证数组稳定。
 */
struct rt_rq *sched_rt_period_rt_rq(struct rt_bandwidth *rt_b, int cpu)
{
	return container_of(rt_b, struct task_group, rt_bandwidth)->rt_rq[cpu];
}

/* 返回 @rt_rq 所属 task_group 的共享 bandwidth 对象。 */
/*
 * 业务背景：per-CPU rt_rq 的计费和补充共享所属 task group 的 period/runtime 配置。
 * 入参：rt_rq 是不可空只读借用队列，其 tg 已初始化。
 * 出参/返回：返回 tg->rt_bandwidth 借用指针，不转移 ownership、无输出参。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；调用者负责 task group 生命周期和必要的 runtime_lock。
 */
static inline struct rt_bandwidth *sched_rt_bandwidth(struct rt_rq *rt_rq)
{
	return &rt_rq->tg->rt_bandwidth;
}

/* 判断当前 rt_rq 是否需要 runtime 计费；无限额度快速返回 false。 */
/*
 * 业务背景：update_curr_rt() 仅在周期 timer 活动或本地已消耗接近共享额度时才需走带宽记账路径。
 * 入参：rt_rq 是不可空只读借用队列。
 * 出参/返回：timer 活动或 rt_time 小于共享 runtime 时返回 true，否则 false；无输出参。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；无锁快照用于优化，真正超额判断在相应锁下再次完成。
 */
bool sched_rt_bandwidth_account(struct rt_rq *rt_rq)
{
	struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);

	return (hrtimer_active(&rt_b->rt_period_timer) ||
		rt_rq->rt_time < rt_b->rt_runtime);
}

/*
 * We ran out of runtime, see if we can borrow some from our neighbours.
 */
/* 从同 root_domain 其他 rt_rq 的闲置额度借 runtime，所有转移在双方 runtime_lock 下。 */
/*
 * 业务背景：启用 RT_RUNTIME_SHARE 后，耗尽额度的 CPU 可按域内权重借用邻居尚未消费的同组 runtime。
 * 入参：rt_rq 是不可空输入/输出目标队列，调用时其 runtime 不足且所属 rq/root_domain 稳定。
 * 出参/返回：无直接返回值；可能减少邻居 runtime 并增加目标 runtime，最多补到一个 period。
 * 注意事项：按 bandwidth 锁再逐个邻居 runtime_lock 的顺序，不睡眠；RUNTIME_INF 队列禁止参与借用。
 */
static void do_balance_runtime(struct rt_rq *rt_rq)
{
	struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);
	struct root_domain *rd = rq_of_rt_rq(rt_rq)->rd;
	int i, weight;
	u64 rt_period;

	/* 域内 CPU 数作为均分权重；每个 donor 最多贡献自身闲置额度的 1/n。 */
	weight = cpumask_weight(rd->span);

	/* bandwidth 锁稳定共享 period，并把所有 donor 的额度转移串成一个事务。 */
	raw_spin_lock(&rt_b->rt_runtime_lock);
	rt_period = ktime_to_ns(rt_b->rt_period);
	for_each_cpu(i, rd->span) {
		struct rt_rq *iter = sched_rt_period_rt_rq(rt_b, i);
		s64 diff;

		/* 目标队列不能向自己借用，否则只会制造无意义的锁递归。 */
		if (iter == rt_rq)
			continue;

		raw_spin_lock(&iter->rt_runtime_lock);
		/*
		 * Either all rqs have inf runtime and there's nothing to steal
		 * or __disable_runtime() below sets a specific rq to inf to
		 * indicate its been disabled and disallow stealing.
		 */
		/* 无限值既可能代表全局不限额，也可能是 offline 哨兵；两种场景都不可扣减。 */
		if (iter->rt_runtime == RUNTIME_INF)
			goto next;

		/*
		 * From runqueues with spare time, take 1/n part of their
		 * spare time, but no more than our period.
		 */
		/* 只有 donor 配额大于已用时间才有正闲置量；负值表示它自己也已超额。 */
		diff = iter->rt_runtime - iter->rt_time;
		if (diff > 0) {
			diff = div_u64((u64)diff, weight);
			/* 单 CPU 借入后最多拥有一个完整 period，防止共享放大组总带宽。 */
			if (rt_rq->rt_runtime + diff > rt_period)
				diff = rt_period - rt_rq->rt_runtime;
			iter->rt_runtime -= diff;
			rt_rq->rt_runtime += diff;
			/* 目标已满，无需继续扫描；先释放当前 donor 锁再退出循环。 */
			if (rt_rq->rt_runtime == rt_period) {
				raw_spin_unlock(&iter->rt_runtime_lock);
				break;
			}
		}
next:
		raw_spin_unlock(&iter->rt_runtime_lock);
	}
	raw_spin_unlock(&rt_b->rt_runtime_lock);
}

/*
 * Ensure this RQ takes back all the runtime it lend to its neighbours.
 */
/* CPU offline 前归还本 rq 借入/借出的 runtime，恢复每个组的基准额度。 */
/*
 * 业务背景：CPU 停用前须从域内邻居回收曾借出的额度，并把本 CPU 标成无限以退出后续共享计算。
 * 入参：rq 是不可空输入/输出离线 runqueue，调用者已串行 CPU hotplug 与调度状态。
 * 出参/返回：无直接返回值；逐组平衡 runtime、清 throttle、设 RUNTIME_INF 并重新发布可运行队列。
 * 注意事项：锁序为 bandwidth 后各 runtime_lock；回收不守恒会 WARN，scheduler 尚未运行时直接返回。
 */
static void __disable_runtime(struct rq *rq)
{
	struct root_domain *rd = rq->rd;
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	if (unlikely(!scheduler_running))
		return;

	/* 每个 task group 独立共享 bandwidth，必须逐层分别回收该 CPU 的借贷余额。 */
	for_each_rt_rq(rt_rq, iter, rq) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);
		s64 want;
		int i;

		/* bandwidth 锁阻止新的跨 CPU 借贷，本地锁稳定离线 rq 的当前余额。 */
		raw_spin_lock(&rt_b->rt_runtime_lock);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/*
		 * Either we're all inf and nobody needs to borrow, or we're
		 * already disabled and thus have nothing to do, or we have
		 * exactly the right amount of runtime to take out.
		 */
		/* 已禁用或余额恰等于基准时无需访问邻居，可直接进入禁用发布阶段。 */
		if (rt_rq->rt_runtime == RUNTIME_INF ||
				rt_rq->rt_runtime == rt_b->rt_runtime)
			goto balanced;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);

		/*
		 * Calculate the difference between what we started out with
		 * and what we current have, that's the amount of runtime
		 * we lend and now have to reclaim.
		 */
		/* want>0 表示本 rq 曾借出需收回；want<0 表示曾借入，需归还给邻居。 */
		want = rt_b->rt_runtime - rt_rq->rt_runtime;

		/*
		 * Greedy reclaim, take back as much as we can.
		 */
		/* 贪心扫描同域队列，优先从当前可用邻居收回，直到差额归零。 */
		for_each_cpu(i, rd->span) {
			struct rt_rq *iter = sched_rt_period_rt_rq(rt_b, i);
			s64 diff;

			/*
			 * Can't reclaim from ourselves or disabled runqueues.
			 */
			if (iter == rt_rq || iter->rt_runtime == RUNTIME_INF)
				continue;

			/* bandwidth 锁已冻结借贷；逐 donor 加锁原子更新其本地额度。 */
			raw_spin_lock(&iter->rt_runtime_lock);
			if (want > 0) {
				/* 收回量不能超过 donor 当前额度，也不能超过剩余欠额。 */
				diff = min_t(s64, iter->rt_runtime, want);
				iter->rt_runtime -= diff;
				want -= diff;
			} else {
				/* 本 rq 多出的额度一次性归给首个可用邻居，want 因而直接归零。 */
				iter->rt_runtime -= want;
				want -= want;
			}
			raw_spin_unlock(&iter->rt_runtime_lock);

			if (!want)
				break;
		}

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/*
		 * We cannot be left wanting - that would mean some runtime
		 * leaked out of the system.
		 */
		/* 非零说明额度在借贷路径泄漏；继续禁用可避免离线过程卡死，但发出告警。 */
		WARN_ON_ONCE(want);
balanced:
		/*
		 * Disable all the borrow logic by pretending we have inf
		 * runtime - in which case borrowing doesn't make sense.
		 */
		/* RUNTIME_INF 同时阻止别人再向离线 rq 借/还；清节流使残留 PI 路径可退出。 */
		rt_rq->rt_runtime = RUNTIME_INF;
		rt_rq->rt_throttled = 0;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		raw_spin_unlock(&rt_b->rt_runtime_lock);

		/* Make rt_rq available for pick_next_task() */
		/* 离线迁移仍需选择队列中的 task，故撤销 throttle 后重新发布该层。 */
		sched_rt_rq_enqueue(rt_rq);
	}
}

/* CPU online 时重新启用每个组 rt_rq 的 runtime 计费并清过期 throttle 状态。 */
/*
 * 业务背景：CPU 重新上线后要撤销离线阶段的 RUNTIME_INF 哨兵，恢复每组配置额度并从零开始计费。
 * 入参：rq 是不可空输入/输出上线 runqueue，hotplug 上下文保证其层级队列稳定。
 * 出参/返回：无直接返回值；重置各 rt_rq 的 runtime、rt_time 和 throttled。
 * 注意事项：按 bandwidth→本地 runtime_lock 修改；scheduler 未启动时无操作，调用者负责之后的队列发布。
 */
static void __enable_runtime(struct rq *rq)
{
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	if (unlikely(!scheduler_running))
		return;

	/*
	 * Reset each runqueue's bandwidth settings
	 */
	/* 上线 CPU 不继承离线前的消费或借贷；每个组都从配置基准重新开始。 */
	for_each_rt_rq(rt_rq, iter, rq) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);

		raw_spin_lock(&rt_b->rt_runtime_lock);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/* 两把锁让配置基准与本地三元状态在同一快照内恢复。 */
		rt_rq->rt_runtime = rt_b->rt_runtime;
		rt_rq->rt_time = 0;
		rt_rq->rt_throttled = 0;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		raw_spin_unlock(&rt_b->rt_runtime_lock);
	}
}

/* runtime 不足且特性开启时尝试借额；无限/足额快速返回。 */
/*
 * 业务背景：本地 rt_time 超过 runtime 时，在真正 throttle 前可利用域内其他 CPU 的闲置额度。
 * 入参：rt_rq 是不可空输入/输出队列，调用者进入时持有其 rt_runtime_lock。
 * 出参/返回：无直接返回值；特性启用且超额时可能调整本地及邻居 runtime。
 * 注意事项：函数临时释放本地 runtime_lock 以遵守全局锁序，返回前重新获取；调用者不能假定中间状态不变。
 */
static void balance_runtime(struct rt_rq *rt_rq)
{
	/* 特性关闭时保持每 CPU 固定额度，超额直接交给 throttle 判定。 */
	if (!sched_feat(RT_RUNTIME_SHARE))
		return;

	/* 仅真实超额才付出跨 CPU 扫描成本；相等仍可运行到下一次计费。 */
	if (rt_rq->rt_time > rt_rq->rt_runtime) {
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		do_balance_runtime(rt_rq);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
	}
}

/* 按 @overrun 个周期补充所有 CPU rt_rq，解除可运行队列 throttle；全 idle 返回 1。 */
/*
 * 业务背景：period timer 每次触发要扣除跨过周期对应的已用时间，解除有新额度队列的 throttle 并判断能否停表。
 * 入参：rt_b 是不可空输入/输出共享带宽；overrun 是大于 0 的逾期周期数。
 * 出参/返回：无需继续 timer 返回 1，否则返回 0；无输出参，可能重置 runtime、rt_time、throttle 和入队状态。
 * 注意事项：hard timer 上下文不可睡眠；逐 CPU 获取 runtime/rq 锁，root group 用 online mask 避免隔离 CPU 永久 throttle。
 */
static int do_sched_rt_period_timer(struct rt_bandwidth *rt_b, int overrun)
{
	int i, idle = 1, throttled = 0;
	const struct cpumask *span;

	/* 普通组限定在当前 root_domain；根组会在下方扩为 online mask。 */
	span = sched_rt_period_mask();

	/*
	 * FIXME: isolated CPUs should really leave the root task group,
	 * whether they are isolcpus or were isolated via cpusets, lest
	 * the timer run on a CPU which does not service all runqueues,
	 * potentially leaving other CPUs indefinitely throttled.  If
	 * isolation is really required, the user will turn the throttle
	 * off to kill the perturbations it causes anyway.  Meanwhile,
	 * this maintains functionality for boot and/or troubleshooting.
	 */
	/* 根组必须补充所有在线 CPU，即使隔离配置使当前 root_domain 看不到其中一部分。 */
	if (rt_b == &root_task_group.rt_bandwidth)
		span = cpu_online_mask;

	/* 每个 CPU 独立扣减 overrun 周期并决定是否把此前节流的层级重新发布。 */
	for_each_cpu(i, span) {
		int enqueue = 0;
		struct rt_rq *rt_rq = sched_rt_period_rt_rq(rt_b, i);
		struct rq *rq = rq_of_rt_rq(rt_rq);
		struct rq_flags rf;
		int skip;

		/*
		 * When span == cpu_online_mask, taking each rq->lock
		 * can be time-consuming. Try to avoid it when possible.
		 */
		/* 先只取轻量 runtime 锁；既无消费又无 runnable 时跳过昂贵 rq 锁。 */
		raw_spin_lock(&rt_rq->rt_runtime_lock);
		if (!sched_feat(RT_RUNTIME_SHARE) && rt_rq->rt_runtime != RUNTIME_INF)
			rt_rq->rt_runtime = rt_b->rt_runtime;
		/* 零消费且空队列无需补充、解节流或更新 rq 时钟。 */
		skip = !rt_rq->rt_time && !rt_rq->rt_nr_running;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		if (skip)
			continue;

		/* 后续可能修改可运行层级和重调度状态，必须进入 rq 串行域。 */
		rq_lock(rq, &rf);
		update_rq_clock(rq);

		/* 有历史消费时按跨过的周期数扣减，但不会把 rt_time 减成负数。 */
		if (rt_rq->rt_time) {
			u64 runtime;

			raw_spin_lock(&rt_rq->rt_runtime_lock);
			/* throttle 队列先尝试共享借额，以免本周期仍有域内闲置却继续阻塞。 */
			if (rt_rq->rt_throttled)
				balance_runtime(rt_rq);
			runtime = rt_rq->rt_runtime;
			rt_rq->rt_time -= min(rt_rq->rt_time, overrun*runtime);
			/* 消费降回额度内即解除标志，并在出锁后重新发布父层 entity。 */
			if (rt_rq->rt_throttled && rt_rq->rt_time < runtime) {
				rt_rq->rt_throttled = 0;
				enqueue = 1;

				/*
				 * When we're idle and a woken (rt) task is
				 * throttled wakeup_preempt() will set
				 * skip_update and the time between the wakeup
				 * and this unthrottle will get accounted as
				 * 'runtime'.
				 */
				/* idle 唤醒曾跳过时钟更新；取消 skip 防止把等待节流的时间误算成执行时间。 */
				if (rt_rq->rt_nr_running && rq->curr == rq->idle)
					rq_clock_cancel_skipupdate(rq);
			}
			/* 尚有消费或 runnable 意味着 timer 仍有后续工作，不能进入 idle 停表。 */
			if (rt_rq->rt_time || rt_rq->rt_nr_running)
				idle = 0;
			raw_spin_unlock(&rt_rq->rt_runtime_lock);
		} else if (rt_rq->rt_nr_running) {
			/* 无历史消费但队列非空，同样必须保留 timer，并确保未节流层级已发布。 */
			idle = 0;
			if (!rt_rq_throttled(rt_rq))
				enqueue = 1;
		}
		/* 任一队列仍标记 throttle 时，即便暂时 idle 也不能在无限额度切换前贸然停表。 */
		if (rt_rq->rt_throttled)
			throttled = 1;

		/* enqueue 在 runtime 锁外、rq 锁内执行，恢复父层可见性并可能触发抢占。 */
		if (enqueue)
			sched_rt_rq_enqueue(rt_rq);
		rq_unlock(rq, &rf);
	}

	/* 全局禁用或无限额度且不存在遗留 throttle 时，周期回调可永久停止。 */
	if (!throttled && (!rt_bandwidth_enabled() || rt_b->rt_runtime == RUNTIME_INF))
		return 1;

	return idle;
}

/* 更新已用 runtime，超配额时 throttle 并启动 period timer；PI boost 可临时绕过。 */
/*
 * 业务背景：RT 实体运行后检查本层队列是否耗尽周期额度，必要时从父层摘除以保护非 RT 任务。
 * 入参：rt_rq 是不可空输入/输出队列，调用者持所属 rq 锁和 rt_runtime_lock。
 * 出参/返回：队列最终被有效 throttle 返回 1，否则 0；无输出参，可能借额、置 throttle 或清零零额度 PI 时间。
 * 注意事项：PI boosted 实体可绕过 throttle 以解除锁依赖；runtime>=period 或 RUNTIME_INF 表示不限制。
 */
static int sched_rt_runtime_exceeded(struct rt_rq *rt_rq)
{
	u64 runtime = sched_rt_runtime(rt_rq);

	/* 已置标志时只需考虑 PI boost 例外，不重复打印或摘除队列。 */
	if (rt_rq->rt_throttled)
		return rt_rq_throttled(rt_rq);

	/* 额度覆盖整个周期等价于不限比例，无需 throttle 或启动借贷。 */
	if (runtime >= sched_rt_period(rt_rq))
		return 0;

	/* 本地超额前先尝试从邻居借用；返回后必须重新读取可能改变的 runtime。 */
	balance_runtime(rt_rq);
	runtime = sched_rt_runtime(rt_rq);
	if (runtime == RUNTIME_INF)
		return 0;

	/* 只有累计执行严格超过最终额度才进入节流状态转换。 */
	if (rt_rq->rt_time > runtime) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);

		/*
		 * Don't actually throttle groups that have no runtime assigned
		 * but accrue some time due to boosting.
		 */
		/* 配置为零的组只可能因 PI boost 记到时间；节流它会阻碍锁持有者释放资源。 */
		if (likely(rt_b->rt_runtime)) {
			rt_rq->rt_throttled = 1;
			printk_deferred_once("sched: RT throttling activated\n");
		} else {
			/*
			 * In case we did anyway, make it go away,
			 * replenishment is a joke, since it will replenish us
			 * with exactly 0 ns.
			 */
			/* 零额度无法靠周期补充恢复，直接丢弃 boost 产生的计费以避免永久 throttle。 */
			rt_rq->rt_time = 0;
		}

		/* 没有 boosted entity 时节流才真正生效：撤销父层发布并通知调用者。 */
		if (rt_rq_throttled(rt_rq)) {
			sched_rt_rq_dequeue(rt_rq);
			return 1;
		}
	}

	/* 未超额、借额成功或仍有 PI boost 时保持队列可运行，向调用者报告未有效节流。 */
	return 0;
}

#else /* !CONFIG_RT_GROUP_SCHED: */

/* 无组调度时遍历类型就是单个 CPU 根 rt_rq，宏只执行一次且没有祖先层级。 */
typedef struct rt_rq *rt_rq_iter_t;

#define for_each_rt_rq(rt_rq, iter, rq) \
	for ((void) iter, rt_rq = &rq->rt; rt_rq; rt_rq = NULL)

#define for_each_sched_rt_entity(rt_se) \
	for (; rt_se; rt_se = NULL)

/*
 * 业务背景：无 RT 组调度时不存在代表子队列的 entity，统一 helper 必须报告叶子语义。
 * 入参：rt_se 是未使用输入，可为 NULL，因为 stub 不解引用。
 * 出参/返回：恒返回 NULL，表示没有子 rt_rq；无输出参。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；不取锁、不睡眠。
 */
static inline struct rt_rq *group_rt_rq(struct sched_rt_entity *rt_se)
{
	return NULL;
}

/*
 * 业务背景：无组层级时可运行 RT 数量直接映射到 CPU 顶层 rq，并立即请求重调度。
 * 入参：rt_rq 是不可空输入/输出顶层队列，调用者持 rq 锁。
 * 出参/返回：无直接返回值；非空时发布顶层计数并 resched current。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；空队列无副作用，重复发布由 enqueue_top_rt_rq 防护。
 */
static inline void sched_rt_rq_enqueue(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	if (!rt_rq->rt_nr_running)
		return;

	enqueue_top_rt_rq(rt_rq);
	resched_curr(rq);
}

/*
 * 业务背景：无组层级时撤销 RT 可运行状态只需更新 CPU 顶层 rq 计数。
 * 入参：rt_rq 是不可空输入/输出顶层队列，调用者持 rq 锁。
 * 出参/返回：无直接返回值；按当前 rt_nr_running 撤销顶层发布。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；计数必须与先前 enqueue 配对。
 */
static inline void sched_rt_rq_dequeue(struct rt_rq *rt_rq)
{
	dequeue_top_rt_rq(rt_rq, rt_rq->rt_nr_running);
}

/*
 * 业务背景：未启用组调度时本文件没有分层 RT runtime throttle，保留统一查询接口。
 * 入参：rt_rq 是未使用输入，可为 NULL，因为 stub 不解引用。
 * 出参/返回：恒返回 false；无输出参。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；全局 RT throttle 由其他配置路径体现，不能据此推断系统无限制。
 */
static inline int rt_rq_throttled(struct rt_rq *rt_rq)
{
	return false;
}

/*
 * 业务背景：无组调度时若需要扫描 RT 周期对象，范围就是全部 online CPU。
 * 入参：无。
 * 出参/返回：返回 cpu_online_mask 只读借用指针；无输出参。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；mask 可随 hotplug 变化，调用者须在合适保护下遍历。
 */
static inline const struct cpumask *sched_rt_period_mask(void)
{
	return cpu_online_mask;
}

static inline
/*
 * 业务背景：无组层级时任意 bandwidth/CPU 组合都对应 CPU 顶层 rt_rq。
 * 入参：rt_b 是未使用输入、可为 NULL；cpu 是有效 possible CPU 编号。
 * 出参/返回：返回 cpu_rq(cpu)->rt 借用指针；无输出参。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；调用者保证 cpu 与 hotplug 生命周期稳定。
 */
struct rt_rq *sched_rt_period_rt_rq(struct rt_bandwidth *rt_b, int cpu)
{
	return &cpu_rq(cpu)->rt;
}

/*
 * 业务背景：未启用组调度时 CPU online 不需要恢复 per-group runtime 状态。
 * 入参：rq 是未使用输入，可为 NULL，因为 stub 不解引用。
 * 出参/返回：无直接返回值、无输出参和副作用。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；不可睡眠。
 */
static void __enable_runtime(struct rq *rq) { }
/*
 * 业务背景：未启用组调度时 CPU offline 不需要回收 per-group runtime 借用。
 * 入参：rq 是未使用输入，可为 NULL，因为 stub 不解引用。
 * 出参/返回：无直接返回值、无输出参和副作用。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；不可睡眠。
 */
static void __disable_runtime(struct rq *rq) { }

#endif /* !CONFIG_RT_GROUP_SCHED */

/* 返回 task entity 的有效 prio，group entity 返回其子 rt_rq 当前最高 prio。 */
/*
 * 业务背景：priority 队列插入层级 entity 时，需要把 task prio 或子组最高 prio 归一为同一键。
 * 入参：rt_se 是不可空只读 task/group entity 借用指针。
 * 出参/返回：group 返回 my_q->highest_prio.curr，task 返回拥有者 p->prio；无输出参。
 * 注意事项：调用者须持所属 rq 锁；组调度配置决定是否检查 my_q，空子组不应被调用。
 */
static inline int rt_se_prio(struct sched_rt_entity *rt_se)
{
#ifdef CONFIG_RT_GROUP_SCHED
	struct rt_rq *rt_rq = group_rt_rq(rt_se);

	if (rt_rq)
		return rt_rq->highest_prio.curr;
#endif

	return rt_task_of(rt_se)->prio;
}

/*
 * Update the current task's runtime statistics. Skip current tasks that
 * are not in our scheduling class.
 */
/* 用 rq clock_task 给当前 RT donor 累计运行时间/组 runtime，并在超额时请求重调度。 */
/*
 * 业务背景：每次调度时钟更新要把实际执行时间记入当前 RT task，并沿 task-group 层级扣减 runtime 配额。
 * 入参：rq 是不可空输入/输出当前 runqueue，调用者持 rq 锁且已更新 rq clock。
 * 出参/返回：无直接返回值；更新 donor 运行统计和各层 rt_time，超额时 throttle/resched 并启动 timer。
 * 注意事项：非 RT donor 或非正 delta 快速返回；runtime_lock 嵌于 rq 锁内，函数不可睡眠。
 */
static void update_curr_rt(struct rq *rq)
{
	struct task_struct *donor = rq->donor;
	s64 delta_exec;

	/* donor 可能因核心调度与 curr 不同；只有实际向 RT 类借出 CPU 的 task 才在此计费。 */
	if (donor->sched_class != &rt_sched_class)
		return;

	/* common helper 结算 exec_start 与通用统计，并返回本次新增运行纳秒。 */
	delta_exec = update_curr_common(rq);
	if (unlikely(delta_exec <= 0))
		return;

#ifdef CONFIG_RT_GROUP_SCHED
	struct sched_rt_entity *rt_se = &donor->rt;

	/* 全局禁用节流时仍保留 task 运行统计，但跳过所有组额度记账。 */
	if (!rt_bandwidth_enabled())
		return;

	/* 叶 task 的执行同时消耗每一级祖先组配额，必须逐层独立累计。 */
	for_each_sched_rt_entity(rt_se) {
		struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
		int exceeded;

		/* 无限额度层无需锁和 timer；有限层在 runtime_lock 下原子累计并判断超额。 */
		if (sched_rt_runtime(rt_rq) != RUNTIME_INF) {
			raw_spin_lock(&rt_rq->rt_runtime_lock);
			rt_rq->rt_time += delta_exec;
			exceeded = sched_rt_runtime_exceeded(rt_rq);
			/* 有效 throttle 已从层级摘除队列，当前 donor 必须尽快重新选择。 */
			if (exceeded)
				resched_curr(rq);
			raw_spin_unlock(&rt_rq->rt_runtime_lock);
			/* 出 runtime 锁后启动补充 timer，避免 timer 锁与本地 runtime 锁倒置。 */
			if (exceeded)
				do_start_rt_bandwidth(sched_rt_bandwidth(rt_rq));
		}
	}
#endif /* CONFIG_RT_GROUP_SCHED */
}

/* 顶层 rt_rq runnable 数减少 @count，首次变空时从 rq 调度类负载中注销。 */
/*
 * 业务背景：顶层 RT 队列被 throttle 或变空时，CPU rq 的总 runnable 计数必须同步扣除已发布实体数。
 * 入参：rt_rq 是不可空输入/输出且必须为 rq->rt；count 是要扣除的已发布 runnable 数。
 * 出参/返回：无直接返回值；已发布时 sub_nr_running(count) 并清 rt_queued。
 * 注意事项：调用者持 rq 锁；非顶层或 rq 总数为零会 BUG，重复撤销由 rt_queued 快路径阻止。
 */
static void
dequeue_top_rt_rq(struct rt_rq *rt_rq, unsigned int count)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	/* 顶层总计数只接受 CPU 自身 rq->rt，子组必须先经父 entity 折叠。 */
	BUG_ON(&rq->rt != rt_rq);

	/* 未发布表示 count 从未加入 rq->nr_running，重复撤销应为无操作。 */
	if (!rt_rq->rt_queued)
		return;

	BUG_ON(!rq->nr_running);

	/* count 是叶子 runnable 数而非一个 group entity，保持 rq 总负载统计口径一致。 */
	sub_nr_running(rq, count);
	rt_rq->rt_queued = 0;

}

/* 非 throttle 且首次变为 runnable 时把顶层 rt_rq 发布给 CPU rq。 */
/*
 * 业务背景：顶层 RT 队列首次拥有可运行实体时，要计入 CPU rq 总 runnable 数并通知 cpufreq。
 * 入参：rt_rq 是不可空输入/输出且必须为 rq->rt，调用者持 rq 锁。
 * 出参/返回：无直接返回值；未 throttle 且非空时 add_nr_running 并置 rt_queued，随后更新频率利用率。
 * 注意事项：非顶层会 BUG；重复发布或 throttle 时不改计数，rt_nr_running 必须与层级维护一致。
 */
static void
enqueue_top_rt_rq(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	BUG_ON(&rq->rt != rt_rq);

	/* rt_queued 是顶层发布位，避免层级反复上浮造成总数重复相加。 */
	if (rt_rq->rt_queued)
		return;

	/* 有效 throttle 的队列即使含 task 也不可交给 pick_next_task()。 */
	if (rt_rq_throttled(rt_rq))
		return;

	/* 顶层用叶 task 数计入 rq，而不是只计一个调度类或 group 节点。 */
	if (rt_rq->rt_nr_running) {
		add_nr_running(rq, rt_rq->rt_nr_running);
		rt_rq->rt_queued = 1;
	}

	/* Kick cpufreq (see the comment in kernel/sched/sched.h). */
	/* 即使本次未新增计数，也让 governor 观察最新 RT 可运行状态。 */
	cpufreq_update_util(rq, 0);
}

/* SMP 下更新 highest curr/next、cpupri 与 overload 派生索引。 */
/*
 * 业务背景：顶层 RT 最高优先级提高时，要更新 root-domain cpupri，供其他 CPU 快速查找可迁移目标。
 * 入参：rt_rq 是不可空输入/输出队列；prio 是新实体内部优先级；prev_prio 是更新前最高优先级。
 * 出参/返回：无直接返回值；仅顶层、online rq 且优先级提高时更新 cpupri。
 * 注意事项：调用者持 rq 锁；子组队列不能直接发布到 cpupri，prio 数值越小越高。
 */
static void
inc_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	/*
	 * Change rq's cpupri only if rt_rq is the top queue.
	 */
	/* 子组最高优先级会逐层折叠到根，直接发布会把同一 CPU 重复写入 cpupri。 */
	if (IS_ENABLED(CONFIG_RT_GROUP_SCHED) && &rq->rt != rt_rq)
		return;

	/* 仅提高最高优先级才改变索引；离线 rq 不应成为其他 CPU 的迁移目标。 */
	if (rq->online && prio < prev_prio)
		cpupri_set(&rq->rd->cpupri, rq->cpu, prio);
}

/* 最高优先级离开后刷新 curr/next 与 root-domain cpupri 候选。 */
/*
 * 业务背景：顶层 RT 最高优先级降低后必须刷新 cpupri，否则 push 可能选择已不合适的 CPU。
 * 入参：rt_rq 是不可空输入/输出队列；prio 是离开实体优先级；prev_prio 是离开前最高值。
 * 出参/返回：无直接返回值；顶层 online rq 的最高值发生变化时更新 cpupri。
 * 注意事项：prio 只用于接口对称且本实现不读取；调用者持 rq 锁，cpupri 仍只是迁移候选索引。
 */
static void
dec_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	/*
	 * Change rq's cpupri only if rt_rq is the top queue.
	 */
	/* 与入队相同，只有根 RT 队列有资格代表整个 CPU 发布 cpupri。 */
	if (IS_ENABLED(CONFIG_RT_GROUP_SCHED) && &rq->rt != rt_rq)
		return;

	/* 最高值未变化说明同优先级仍有实体，无需触发 root-domain 索引更新。 */
	if (rq->online && rt_rq->highest_prio.curr != prev_prio)
		cpupri_set(&rq->rd->cpupri, rq->cpu, rt_rq->highest_prio.curr);
}

/* entity 加入后将 @prio 合入 rt_rq 最高优先级。 */
/*
 * 业务背景：RT entity 入队时缓存数值最小的最高优先级，避免选取路径重复扫描位图。
 * 入参：rt_rq 是不可空输入/输出队列；prio 是 [0,MAX_RT_PRIO) 的实体内部优先级。
 * 出参/返回：无直接返回值；可能降低 highest_prio.curr 并同步顶层 cpupri。
 * 注意事项：调用者持 rq 锁且 bitmap/计数已处于匹配阶段；非法 prio 会破坏候选索引。
 */
static void
inc_rt_prio(struct rt_rq *rt_rq, int prio)
{
	int prev_prio = rt_rq->highest_prio.curr;

	/* 数值越小优先级越高；只有更高实体才能改变 curr 缓存。 */
	if (prio < prev_prio)
		rt_rq->highest_prio.curr = prio;

	inc_rt_prio_smp(rt_rq, prio, prev_prio);
}

/* 最后一个 @prio entity 离开时扫描 bitmap 并刷新最高优先级索引。 */
/*
 * 业务背景：RT entity 出队后若移走当前最高优先级，需要从 active bitmap 找到下一非空队列。
 * 入参：rt_rq 是不可空输入/输出队列；prio 是刚移除实体的有效内部优先级。
 * 出参/返回：无直接返回值；刷新 highest_prio.curr，空队列恢复哨兵，并同步 cpupri。
 * 注意事项：调用者持 rq 锁且已更新 rt_nr_running/bitmap；prio 小于旧最高值会 WARN，状态不一致会误选任务。
 */
static void
dec_rt_prio(struct rt_rq *rt_rq, int prio)
{
	int prev_prio = rt_rq->highest_prio.curr;

	/* 非空时只有移除旧最高队列才需扫描；空队列直接恢复哨兵。 */
	if (rt_rq->rt_nr_running) {

		/* 被移除优先级不可能高于缓存最高值，否则位图/缓存已失配。 */
		WARN_ON(prio < prev_prio);

		/*
		 * This may have been our highest task, and therefore
		 * we may have some re-computation to do
		 */
		/* 同优先级最后节点消失后，位图第一置位重新给出下一最高队列。 */
		if (prio == prev_prio) {
			struct rt_prio_array *array = &rt_rq->active;

			rt_rq->highest_prio.curr =
				sched_find_first_bit(array->bitmap);
		}

	} else {
		/* MAX_RT_PRIO-1 是本实现空队列缓存值；真正位图另有 MAX_RT_PRIO 哨兵。 */
		rt_rq->highest_prio.curr = MAX_RT_PRIO-1;
	}

	dec_rt_prio_smp(rt_rq, prio, prev_prio);
}

#ifdef CONFIG_RT_GROUP_SCHED

/*
 * 业务背景：entity 入队时把 PI boost 贡献累加到父 rt_rq，并确保所属 task group 的周期补充已启动。
 * 入参：rt_se 是不可空只读 entity；rt_rq 是不可空输入/输出父队列。
 * 出参/返回：无直接返回值；可能递增 rt_nr_boosted 并启动 bandwidth timer。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；调用者持 rq 锁，boost 判断须与 PI 状态一致。
 */
static void
inc_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	/* boosted 计数让零额度组仍可运行 PI 锁持有者，解除优先级反转链。 */
	if (rt_se_boosted(rt_se))
		rt_rq->rt_nr_boosted++;

	/* 首个实体入队即确保有限额度组有周期补充来源。 */
	start_rt_bandwidth(&rt_rq->tg->rt_bandwidth);
}

/*
 * 业务背景：entity 出队时撤销其对父队列 PI boost 计数的贡献，维持 throttle 旁路判断。
 * 入参：rt_se 是不可空只读 entity；rt_rq 是不可空输入/输出父队列。
 * 出参/返回：无直接返回值；boosted entity 使 rt_nr_boosted 减一。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；调用者持 rq 锁，空队列仍有 boosted 计数会 WARN。
 */
static void
dec_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	/* 与入队贡献一一配对；是否 boosted 必须在相同 rq/PI 串行状态下判断。 */
	if (rt_se_boosted(rt_se))
		rt_rq->rt_nr_boosted--;

	/* 空队列不应残留 boost 贡献，否则后续 throttle 会被永久绕过。 */
	WARN_ON(!rt_rq->rt_nr_running && rt_rq->rt_nr_boosted);
}

#else /* !CONFIG_RT_GROUP_SCHED: */

/*
 * 业务背景：无组调度时无需维护父组 boosted 计数或启动 per-group timer。
 * 入参：rt_se/rt_rq 均为未使用输入，可为 NULL，因为 stub 不解引用。
 * 出参/返回：无直接返回值、无输出参和副作用。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；不可睡眠。
 */
static void
inc_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
}

static inline
/*
 * 业务背景：无组调度时 entity 出队没有父组 boosted 计数需要撤销。
 * 入参：rt_se/rt_rq 均为未使用输入，可为 NULL，因为 stub 不解引用。
 * 出参/返回：无直接返回值、无输出参和副作用。
 * 注意事项：仅 !CONFIG_RT_GROUP_SCHED 下存在；不可睡眠。
 */
void dec_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq) {}

#endif /* !CONFIG_RT_GROUP_SCHED */

/* task entity 贡献 1，group entity 贡献其子 rt_rq 的 runnable task 数。 */
/*
 * 业务背景：父 rt_rq 的 runnable 总数按叶子 task 数统计，group entity 需展开其子队列贡献。
 * 入参：rt_se 是不可空只读 task/group entity。
 * 出参/返回：task 返回 1，group 返回子 rt_rq->rt_nr_running；无输出参。
 * 注意事项：调用者持 rq 锁保证层级计数稳定；返回 0 的空 group 不应处于父优先级列表。
 */
static inline
unsigned int rt_se_nr_running(struct sched_rt_entity *rt_se)
{
	struct rt_rq *group_rq = group_rt_rq(rt_se);

	/* 父层统计叶 task 数，因此 group entity 贡献子树总数而不是固定 1。 */
	if (group_rq)
		return group_rq->rt_nr_running;
	else
		return 1;
}

/* 返回 entity 覆盖的 SCHED_RR task 数。 */
/*
 * 业务背景：rq 需要单独统计 RR task 数，以决定 tick 是否执行时间片轮转逻辑。
 * 入参：rt_se 是不可空只读 task/group entity。
 * 出参/返回：group 返回子队列 rr_nr_running；task 为 SCHED_RR 返回 1，否则 0；无输出参。
 * 注意事项：调用者持 rq 锁；task policy 并发变化必须由调度类切换协议串行。
 */
static inline
unsigned int rt_se_rr_nr_running(struct sched_rt_entity *rt_se)
{
	struct rt_rq *group_rq = group_rt_rq(rt_se);
	struct task_struct *tsk;

	/* group 使用预聚合 RR 数；叶子则由当前 policy 判定是否贡献一个。 */
	if (group_rq)
		return group_rq->rr_nr_running;

	tsk = rt_task_of(rt_se);

	return (tsk->policy == SCHED_RR) ? 1 : 0;
}

/* entity 入队时增加 runnable/RR/迁移计数并更新顶层/cpupri/overload。 */
/*
 * 业务背景：entity 成为父 rt_rq 成员时，所有派生计数、最高优先级和组带宽状态必须同步增加。
 * 入参：rt_se 是不可空已入队 entity；rt_rq 是不可空输入/输出父队列。
 * 出参/返回：无直接返回值；增加 runnable/RR/boosted 计数并更新 highest/cpupri、启动带宽。
 * 注意事项：调用者持 rq 锁且只能对一次入队调用；非 RT prio 会 WARN，重复增加会破坏全局 nr_running。
 */
static inline
void inc_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	int prio = rt_se_prio(rt_se);

	/* 进入 RT 队列的 entity 必须解析为合法实时优先级。 */
	WARN_ON(!rt_prio(prio));
	rt_rq->rt_nr_running += rt_se_nr_running(rt_se);
	rt_rq->rr_nr_running += rt_se_rr_nr_running(rt_se);

	/* 基础计数完成后再刷新派生优先级与组 boost/带宽状态。 */
	inc_rt_prio(rt_rq, prio);
	inc_rt_group(rt_se, rt_rq);
}

/* entity 出队时对称减少计数并撤销不再成立的 overload/cpupri 状态。 */
/*
 * 业务背景：entity 离开父 rt_rq 时要对称撤销其叶子、RR 和 boosted 贡献，并重新计算最高优先级。
 * 入参：rt_se 是不可空正在出队 entity；rt_rq 是不可空输入/输出父队列。
 * 出参/返回：无直接返回值；减少计数并更新 highest/cpupri 与组状态。
 * 注意事项：调用者持 rq 锁且 entity 必须已计数；空计数或非 RT prio 会 WARN，下溢将破坏调度选择。
 */
static inline
void dec_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	WARN_ON(!rt_prio(rt_se_prio(rt_se)));
	WARN_ON(!rt_rq->rt_nr_running);
	/* 先撤销叶子和 RR 贡献，dec_rt_prio() 才能依据新的非空状态选择哨兵或扫描。 */
	rt_rq->rt_nr_running -= rt_se_nr_running(rt_se);
	rt_rq->rr_nr_running -= rt_se_rr_nr_running(rt_se);

	/* 最后同步最高优先级索引并撤销 boost 贡献，完成入队操作的逆序对称。 */
	dec_rt_prio(rt_rq, rt_se_prio(rt_se));
	dec_rt_group(rt_se, rt_rq);
}

/*
 * Change rt_se->run_list location unless SAVE && !MOVE
 *
 * assumes ENQUEUE/DEQUEUE flags match
 */
/* SAVE/RESTORE 属性更新时可保持队列位置，普通操作才真正移动 entity。 */
/*
 * 业务背景：调度属性更新可能只暂存 entity 状态而不改变同优先级 FIFO 顺序，需从通用 flags 判断是否移动链表节点。
 * 入参：flags 是 ENQUEUE/DEQUEUE 标志位集合，仅输入。
 * 出参/返回：仅 DEQUEUE_SAVE 且无 DEQUEUE_MOVE 时返回 false，其他组合返回 true；无输出参。
 * 注意事项：调用方的 enqueue/dequeue flags 必须语义配对；误判会改变 FIFO/RR 公平顺序。
 */
static inline bool move_entity(unsigned int flags)
{
	if ((flags & (DEQUEUE_SAVE | DEQUEUE_MOVE)) == DEQUEUE_SAVE)
		return false;

	return true;
}

/* 从 priority list 摘 entity；该优先级变空时清 bitmap 位。 */
/*
 * 业务背景：RT priority array 以链表保存同优先级 FIFO 顺序、以 bitmap 标记非空队列，两者必须原子维护。
 * 入参：rt_se 是不可空且 on_list 的输入/输出 entity；array 是不可空所属优先级数组。
 * 出参/返回：无直接返回值；摘除 run_list，必要时清 bitmap，并清 on_list。
 * 注意事项：调用者持 rq 锁；entity 未在该 array 时调用会破坏链表，prio 必须在合法范围。
 */
static void __delist_rt_entity(struct sched_rt_entity *rt_se, struct rt_prio_array *array)
{
	list_del_init(&rt_se->run_list);

	if (list_empty(array->queue + rt_se_prio(rt_se)))
		__clear_bit(rt_se_prio(rt_se), array->bitmap);

	rt_se->on_list = 0;
}

/*
 * 业务背景：把 RT entity 映射到可用的 task schedstats，供后续统计 helper 统一判空。
 * 入参：rt_se 是不可空只读借用 entity，可代表 task 或 group。
 * 出参/返回：task entity 返回 stats 借用指针，group entity 返回 NULL；无输出参数和 ownership 转移。
 * 注意事项：调用者须保护 entity/task 生命周期；本函数不取锁，也不增加引用。
 */
static inline struct sched_statistics *
/*
 * 业务背景：schedstats 只存于 task_struct，RT group entity 没有可记录的独立统计对象。
 * 入参：rt_se 是不可空只读 task/group entity。
 * 出参/返回：task entity 返回其 stats 借用指针，group entity 返回 NULL；无输出参。
 * 注意事项：不取锁、不增加 task 引用；调用者须在 rq/task 生命周期保护内使用返回指针。
 */
__schedstats_from_rt_se(struct sched_rt_entity *rt_se)
{
	/* schedstats is not supported for rt group. */
	if (!rt_entity_is_task(rt_se))
		return NULL;

	return &rt_task_of(rt_se)->stats;
}

/*
 * 业务背景：在 RT task 进入等待阶段时建立 schedstats 计时起点。
 * 入参：rt_rq 与 rt_se 是不可空输入/输出借用对象，分别表示所属队列和 entity。
 * 出参/返回：无直接返回值和输出参数；符合条件时更新 task 的 wait_start。
 * 注意事项：调用者持 rq 锁；统计关闭或 entity 为 group 时无操作，不转移 ownership。
 */
static inline void
/*
 * 业务背景：RT task 开始在队列等待时记录等待起点，供 schedstats 计算调度延迟。
 * 入参：rt_rq 是不可空所属队列；rt_se 是不可空 task/group entity，均为输入/输出借用。
 * 出参/返回：无直接返回值；schedstats 启用且为 task 时更新其 wait_start。
 * 注意事项：调用者持 rq 锁；group entity 和禁用 schedstats 时无操作，不得把 NULL stats 传下层。
 */
update_stats_wait_start_rt(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se)
{
	struct sched_statistics *stats;
	struct task_struct *p = NULL;

	/* 静态键关闭时不解析 entity，也不读取 rq 时钟，保持热路径接近零成本。 */
	if (!schedstat_enabled())
		return;

	/* 只有叶 task 能作为 schedstats 的主体；group 保持 p=NULL 并在下方退出。 */
	if (rt_entity_is_task(rt_se))
		p = rt_task_of(rt_se);

	/* helper 把 group entity 显式映射为 NULL，统一处理两种编译配置。 */
	stats = __schedstats_from_rt_se(rt_se);
	if (!stats)
		return;

	__update_stats_wait_start(rq_of_rt_rq(rt_rq), p, stats);
}

/*
 * 业务背景：在睡眠 RT task 被唤醒入队时补记 sleeper 统计。
 * 入参：rt_rq 与 rt_se 是不可空借用对象，分别表示所属队列和 task/group entity。
 * 出参/返回：无直接返回值和输出参数；符合条件时更新 task 的 enqueue-sleeper 统计。
 * 注意事项：调用者持 rq 锁；统计关闭或 group entity 时无操作，不转移 ownership。
 */
static inline void
/*
 * 业务背景：睡眠唤醒的 RT task 再入队时需记录睡眠结束及等待开始相关统计。
 * 入参：rt_rq 是不可空所属队列；rt_se 是不可空 task/group entity。
 * 出参/返回：无直接返回值；schedstats 启用且为 task 时更新 enqueue-sleeper 统计。
 * 注意事项：调用者持 rq 锁；group entity 无独立 schedstats，禁用统计时快速返回。
 */
update_stats_enqueue_sleeper_rt(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se)
{
	struct sched_statistics *stats;
	struct task_struct *p = NULL;

	/* 禁用统计时不触碰 task 字段，避免为观测功能增加正常唤醒成本。 */
	if (!schedstat_enabled())
		return;

	/* group entity 没有独立睡眠生命周期，只有叶 task 参与 sleeper 统计。 */
	if (rt_entity_is_task(rt_se))
		p = rt_task_of(rt_se);

	/* 非 task 返回 NULL，防止把组层级的入队误计为一次进程唤醒。 */
	stats = __schedstats_from_rt_se(rt_se);
	if (!stats)
		return;

	__update_stats_enqueue_sleeper(rq_of_rt_rq(rt_rq), p, stats);
}

/*
 * 业务背景：只把真实唤醒入队转换为 sleeper 统计，过滤内部重排造成的重复样本。
 * 入参：rt_rq/rt_se 是不可空借用队列和 entity；flags 是只读入队原因位集合。
 * 出参/返回：无直接返回值和输出参数；ENQUEUE_WAKEUP 时可能更新 task schedstats。
 * 注意事项：调用者持 rq 锁；统计关闭、非唤醒或 group entity 时无副作用。
 */
static inline void
/*
 * 业务背景：RT entity 入队统计只在真正的 wakeup 场景记录睡眠者延迟，避免内部重排重复计数。
 * 入参：rt_rq/rt_se 是不可空所属队列和实体；flags 是入队原因标志。
 * 出参/返回：无直接返回值；ENQUEUE_WAKEUP 时可能更新 task schedstats。
 * 注意事项：调用者持 rq 锁；统计禁用或非 wakeup 时无副作用。
 */
update_stats_enqueue_rt(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se,
			int flags)
{
	if (!schedstat_enabled())
		return;

	if (flags & ENQUEUE_WAKEUP)
		update_stats_enqueue_sleeper_rt(rt_rq, rt_se);
}

/*
 * 业务背景：RT task 结束排队等待时闭合 schedstats 等待区间。
 * 入参：rt_rq 与 rt_se 是不可空借用队列和 task/group entity。
 * 出参/返回：无直接返回值和输出参数；符合条件时更新 task 的 wait_end。
 * 注意事项：调用者持 rq 锁；统计关闭或 group entity 时无操作，不转移 ownership。
 */
static inline void
/*
 * 业务背景：RT task 被选中或离队时结束等待区间，累加其 runqueue 等待时长。
 * 入参：rt_rq 是不可空所属队列；rt_se 是不可空 task/group entity。
 * 出参/返回：无直接返回值；schedstats 启用且为 task 时更新 wait_end。
 * 注意事项：调用者持 rq 锁；group entity 和关闭统计时无操作，时钟取自底层 CPU rq。
 */
update_stats_wait_end_rt(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se)
{
	struct sched_statistics *stats;
	struct task_struct *p = NULL;

	/* 与 wait_start 对称受同一静态键控制，避免形成只有一端的等待区间。 */
	if (!schedstat_enabled())
		return;

	/* 叶 task 提供统计存储和身份；父 group 只参与调度层级，不产生等待样本。 */
	if (rt_entity_is_task(rt_se))
		p = rt_task_of(rt_se);

	/* 返回 NULL 即属于 group，直接结束而不调用只接受 task stats 的通用 helper。 */
	stats = __schedstats_from_rt_se(rt_se);
	if (!stats)
		return;

	__update_stats_wait_end(rq_of_rt_rq(rt_rq), p, stats);
}

/*
 * 业务背景：RT entity 离队时结束等待统计，并为真实睡眠建立 sleep/block 起点。
 * 入参：rt_rq/rt_se 是不可空借用队列和 entity；flags 是只读 DEQUEUE_* 原因位集合。
 * 出参/返回：无直接返回值和输出参数；可能更新 wait_end、sleep_start 或 block_start。
 * 注意事项：调用者持 rq 锁；current、group 与非睡眠离队走不同统计路径，不转移 ownership。
 */
static inline void
/*
 * 业务背景：RT task 离队时要结束排队等待；因睡眠离队还需区分可中断睡眠与不可中断阻塞起点。
 * 入参：rt_rq/rt_se 是不可空所属队列和实体；flags 是 DEQUEUE_* 原因标志。
 * 出参/返回：无直接返回值；可能更新 wait_end、sleep_start 或 block_start。
 * 注意事项：调用者持 rq 锁；current 不重复结束等待，task state 用 READ_ONCE 取瞬时值，group 无睡眠统计。
 */
update_stats_dequeue_rt(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se,
			int flags)
{
	struct task_struct *p = NULL;
	struct rq *rq = rq_of_rt_rq(rt_rq);

	/* 统计关闭时连 task state 都不读取，避免在 dequeue 热路径引入额外共享访问。 */
	if (!schedstat_enabled())
		return;

	/* 非 current task 离队意味着排队等待结束；current 已在被选中时结束过等待。 */
	if (rt_entity_is_task(rt_se)) {
		p = rt_task_of(rt_se);

		if (p != rq->curr)
			update_stats_wait_end_rt(rt_rq, rt_se);
	}

	/* 只有真实 task 因睡眠离队才开启 sleep/block 区间，迁移和重排不计入。 */
	if ((flags & DEQUEUE_SLEEP) && p) {
		unsigned int state;

		/* 状态可能由唤醒路径并发改变；单次快照避免编译器重复读取混合分支。 */
		state = READ_ONCE(p->__state);
		if (state & TASK_INTERRUPTIBLE)
			__schedstat_set(p->stats.sleep_start,
					rq_clock(rq_of_rt_rq(rt_rq)));

		/* 不可中断等待单独累计为 block；复合状态可按通用 schedstats 语义分别命中。 */
		if (state & TASK_UNINTERRUPTIBLE)
			__schedstat_set(p->stats.block_start,
					rq_clock(rq_of_rt_rq(rt_rq)));
	}
}

/* 将单层 entity 按 HEAD/尾部插入 priority list，并更新 bitmap 与计数。 */
/*
 * 业务背景：层级重建时把一个 task/group entity 发布到其直接父 rt_rq，并维护 FIFO 队列和所有派生计数。
 * 入参：rt_se 是不可空输入/输出 entity；flags 控制是否移动及头/尾插入。
 * 出参/返回：无直接返回值；可插入 run_list、置 bitmap/on_rq/on_list 并增加父队列计数。
 * 注意事项：调用者持底层 rq 锁；throttled/空 group 不得发布，重复 on_list 会 WARN，SAVE 可只恢复计数不移动。
 */
static void __enqueue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;
	struct rt_rq *group_rq = group_rt_rq(rt_se);
	struct list_head *queue = array->queue + rt_se_prio(rt_se);

	/*
	 * Don't enqueue the group if its throttled, or when empty.
	 * The latter is a consequence of the former when a child group
	 * get throttled and the current group doesn't have any other
	 * active members.
	 */
	/* throttle 或空子组都不能成为父层候选；若旧节点尚在表中则顺便清理陈旧发布。 */
	if (group_rq && (rt_rq_throttled(group_rq) || !group_rq->rt_nr_running)) {
		if (rt_se->on_list)
			__delist_rt_entity(rt_se, array);
		return;
	}

	/* SAVE&&!MOVE 只恢复 on_rq/计数，保留原链表位置；普通入队才操作 FIFO 链。 */
	if (move_entity(flags)) {
		WARN_ON_ONCE(rt_se->on_list);
		/* HEAD 用于保持或提升同优先级位置；默认尾插维持 FIFO 到达顺序。 */
		if (flags & ENQUEUE_HEAD)
			list_add(&rt_se->run_list, queue);
		else
			list_add_tail(&rt_se->run_list, queue);

		/* 链表节点完成后再置 bitmap，随后选择路径才能看到完整非空队列。 */
		__set_bit(rt_se_prio(rt_se), array->bitmap);
		rt_se->on_list = 1;
	}
	/* on_rq 表示逻辑计数贡献，即使 SAVE 路径没有移动 on_list 也必须置位。 */
	rt_se->on_rq = 1;

	inc_rt_tasks(rt_se, rt_rq);
}

/* 从单层 priority queue 摘 entity 并更新等待统计和 runnable 计数。 */
/*
 * 业务背景：层级拆除时从直接父 rt_rq 撤销一个 entity，保持链表、bitmap 与 runnable 计数对称。
 * 入参：rt_se 是不可空输入/输出且 on_rq 的 entity；flags 控制是否实际移动链表节点。
 * 出参/返回：无直接返回值；可能摘 run_list，清 on_rq 并减少父队列计数。
 * 注意事项：调用者持 rq 锁；要求与先前 enqueue 配对，非 SAVE 路径发现 !on_list 会 WARN。
 */
static void __dequeue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;

	/* 普通离队删除物理节点；SAVE&&!MOVE 只撤销逻辑贡献以便稍后原位恢复。 */
	if (move_entity(flags)) {
		WARN_ON_ONCE(!rt_se->on_list);
		__delist_rt_entity(rt_se, array);
	}
	/* 先清逻辑发布位，再对称扣除叶子/RR/boost 与最高优先级派生状态。 */
	rt_se->on_rq = 0;

	dec_rt_tasks(rt_se, rt_rq);
}

/*
 * Because the prio of an upper entry depends on the lower
 * entries, we must remove entries top - down.
 */
/* 先从叶到根摘整条 entity 栈，避免父节点仍代表已变化的子队列。 */
/*
 * 业务背景：子队列最高优先级依赖叶子状态，修改前必须先按父到叶顺序撤销整条已发布层级，避免父键过期。
 * 入参：rt_se 是不可空起始 entity；flags 是与后续重建配对的移动/保存标志。
 * 出参/返回：无直接返回值；临时写各 entity->back，摘除所有 on_rq 层并撤销顶层计数。
 * 注意事项：调用者持同一 CPU rq 锁；back 是临时链，不得并发使用，起始实体所属层级必须完整有效。
 */
static void dequeue_rt_stack(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct sched_rt_entity *back = NULL;
	unsigned int rt_nr_running;

	/* 正向遍历叶到根，用 back 反接成根到叶链，避免额外栈或动态内存。 */
	for_each_sched_rt_entity(rt_se) {
		rt_se->back = back;
		back = rt_se;
	}

	/* 在拆层级前保存顶层已发布叶子数，供最后一次性扣 rq 总计数。 */
	rt_nr_running = rt_rq_of_se(back)->rt_nr_running;

	/* 从根向叶摘除，确保父层不会短暂携带已经变化的子组优先级。 */
	for (rt_se = back; rt_se; rt_se = rt_se->back) {
		if (on_rt_rq(rt_se))
			__dequeue_rt_entity(rt_se, flags);
	}

	/* 所有层级贡献撤销后，再把原顶层叶子数从 CPU rq 总数中扣除。 */
	dequeue_top_rt_rq(rt_rq_of_se(back), rt_nr_running);
}

/* 层级安全入队：先清旧祖先位置，再按当前子队列状态自根向叶发布。 */
/*
 * 业务背景：task/group entity 入队会改变每级父 entity 的有效优先级，必须整栈摘除后按新状态重建。
 * 入参：rt_se 是不可空输入/输出起始 entity；flags 是入队原因及顺序标志。
 * 出参/返回：无直接返回值；更新统计、层级队列、bitmap/计数并发布顶层 rq。
 * 注意事项：调用者持 rq 锁；会暂时让整条层级不可见，scope 内不得中途解锁，重复入队会破坏计数。
 */
static void enqueue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rq *rq = rq_of_rt_se(rt_se);

	/* 统计只记录本次叶子入队原因，不随祖先重建重复累计。 */
	update_stats_enqueue_rt(rt_rq_of_se(rt_se), rt_se, flags);

	/* 先清除整条旧层级，防止子优先级变化后父节点仍留在旧 priority queue。 */
	dequeue_rt_stack(rt_se, flags);
	/* 叶到根逐层重建；每层入队后为上一层提供新的 runnable/priority 聚合。 */
	for_each_sched_rt_entity(rt_se)
		__enqueue_rt_entity(rt_se, flags);
	enqueue_top_rt_rq(&rq->rt);
}

/* 层级安全出队：整栈摘除后按仍非空的子队列从根到叶重建。 */
/*
 * 业务背景：叶子离队后祖先 group 可能仍有其他 runnable task，需撤销旧层级再只重建非空节点。
 * 入参：rt_se 是不可空输入/输出起始 entity；flags 描述睡眠、保存或移动原因。
 * 出参/返回：无直接返回值；更新离队统计，撤销目标贡献并重新发布仍非空祖先和顶层 rq。
 * 注意事项：调用者持 rq 锁；层级计数必须先由单层 dequeue 更新，空 group 不得重新入父队列。
 */
static void dequeue_rt_entity(struct sched_rt_entity *rt_se, unsigned int flags)
{
	struct rq *rq = rq_of_rt_se(rt_se);

	/* 在改变 on_rq 与链表前截取等待/睡眠统计所需的 task 状态。 */
	update_stats_dequeue_rt(rt_rq_of_se(rt_se), rt_se, flags);

	/* 根到叶撤销旧层级和顶层计数，之后再依据剩余子树重建。 */
	dequeue_rt_stack(rt_se, flags);

	/* 叶目标已经扣除；仍有 runnable 的子组代表其他成员，必须重新挂回父层。 */
	for_each_sched_rt_entity(rt_se) {
		struct rt_rq *rt_rq = group_rt_rq(rt_se);

		/* task entity 没有 group_rq；空 group 也不得重新成为可选择节点。 */
		if (rt_rq && rt_rq->rt_nr_running)
			__enqueue_rt_entity(rt_se, flags);
	}
	enqueue_top_rt_rq(&rq->rt);
}

/*
 * Adding/removing a task to/from a priority array:
 */
/* rq 锁下更新 curr、加入 task entity/可 push 集并启动 bandwidth。 */
/*
 * 业务背景：RT task 变为 runnable 时需发布层级 entity、开始等待统计，并在 SMP 上登记可迁移候选。
 * 入参：rq 是不可空输入/输出目标队列；p 是不可空输入/输出 RT task；flags 是入队原因和位置标志。
 * 出参/返回：无直接返回值；更新 timeout/统计/层级队列，合格时加入 pushable 集。
 * 注意事项：调用者持 rq 锁；blocked task 不进入 pushable，current 和单 CPU task 也不得发布为迁移候选。
 */
static void
enqueue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;

	/* 新一轮唤醒清 watchdog 连续运行计数，阻塞间隔打断 run-away 判定。 */
	if (flags & ENQUEUE_WAKEUP)
		rt_se->timeout = 0;

	/* 静态键可能因调试配置开启；随后记录 task 从此刻开始的 rq 等待。 */
	check_schedstat_required();
	update_stats_wait_start_rt(rt_rq_of_se(rt_se), rt_se);

	/* 层级发布完成后 task 才成为 pick_next_task_rt() 的候选。 */
	enqueue_rt_entity(rt_se, flags);

	/* 锁阻塞 task 由 PI/唤醒协议管理，不能作为普通 pushable 候选迁移。 */
	if (task_is_blocked(p))
		return;

	/* current 无法立即迁走，单 CPU affinity 也无目标；其余 task 登记供 push 使用。 */
	if (!task_current(rq, p) && p->nr_cpus_allowed > 1)
		enqueue_pushable_task(rq, p);
}

/* rq 锁下结算 curr、摘 entity/pushable，并返回 task 是否仍在 class 队列。 */
/*
 * 业务背景：RT task 阻塞、迁移或换类时要结算执行时间并从层级及 pushable 两套索引对称撤销。
 * 入参：rq 是不可空输入/输出所属队列；p 是不可空输入/输出 RT task；flags 是离队原因标志。
 * 出参/返回：恒返回 true；无输出参，更新 runtime/统计并摘除实体和迁移节点。
 * 注意事项：调用者持 rq 锁且 p 已入队；dequeue_pushable_task 要求其 plist 状态与调度核心约定一致。
 */
static bool dequeue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;

	/* 若 p/current 正在执行，先把离队前最后一段 CPU 时间计入 task 与组配额。 */
	update_curr_rt(rq);
	/* 撤销层级可运行性后，pick 路径不再能通过 priority bitmap 找到 p。 */
	dequeue_rt_entity(rt_se, flags);

	/* 调度核心保证入队 RT task 有对应 plist 状态；离队必须对称删除。 */
	dequeue_pushable_task(rq, p);

	return true;
}

/*
 * Put task to the head or the end of the run list without the overhead of
 * dequeue followed by enqueue.
 */
/* 保持计数不变，把单层 entity 移到同优先级队头或队尾。 */
/*
 * 业务背景：FIFO/RR 轮转只需改变同优先级链表顺序，无需完整出入队和重新计算计数。
 * 入参：rt_rq 是不可空所属队列；rt_se 是不可空输入/输出 entity；head 非零表示移到队头，否则队尾。
 * 出参/返回：无直接返回值；entity on_rq 时调整 run_list 位置。
 * 注意事项：调用者持 rq 锁；不改变 bitmap/计数，rt_se 必须属于 rt_rq 对应 priority queue。
 */
static void
requeue_rt_entity(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se, int head)
{
	/* throttle/空组可能使某层不在 rq；此时不应凭 requeue 重新发布它。 */
	if (on_rt_rq(rt_se)) {
		struct rt_prio_array *array = &rt_rq->active;
		struct list_head *queue = array->queue + rt_se_prio(rt_se);

		/* list_move 保持 bitmap、on_list 和所有计数不变，仅调整同优先级相对顺序。 */
		if (head)
			list_move(&rt_se->run_list, queue);
		else
			list_move_tail(&rt_se->run_list, queue);
	}
}

/* 把 @p 及必要祖先移到同优先级头或尾，保持层级可选择性。 */
/*
 * 业务背景：task 的 FIFO/RR 顺序改变需沿层级移动代表它的 entity，确保父队列观察一致顺序。
 * 入参：rq 是不可空所属 runqueue；p 是不可空输入/输出 RT task；head 非零队头、零为队尾。
 * 出参/返回：无直接返回值；重排 p 及所有祖先 entity 的同优先级位置。
 * 注意事项：调用者持 rq 锁；rq 参数用于契约/锁归属而函数体不解引用，错误层级会造成列表错位。
 */
static void requeue_task_rt(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq;

	/* 从 task 向根移动每一级代表节点，使层级 FIFO 次序与叶子轮转同步。 */
	for_each_sched_rt_entity(rt_se) {
		rt_rq = rt_rq_of_se(rt_se);
		requeue_rt_entity(rt_rq, rt_se, head);
	}
}

/* 当前 RT task 主动 yield 时移到同优先级队尾；独占时不保证换人运行。 */
/*
 * 业务背景：SCHED_RR/FIFO 的 yield 语义通过把 current 的层级 entity 移到同优先级队尾实现。
 * 入参：rq 是不可空输入/输出当前 runqueue，隐式目标为 rq->donor。
 * 出参/返回：无直接返回值、无输出参；重排 current 的 RT 队列位置。
 * 注意事项：调用者持 rq 锁；无同优先级竞争者时仍可能继续运行，不能提供同步或进度保证。
 */
static void yield_task_rt(struct rq *rq)
{
	requeue_task_rt(rq, rq->donor, 0);
}

static int find_lowest_rq(struct task_struct *task);

/* 唤醒/迁移时用 cpupri、亲和性与容量选候选 CPU，锁后路径仍会复核。 */
/*
 * 业务背景：RT task 唤醒时尽量避免压在已有高优先级 RT rq 上，并在异构系统满足 uclamp 容量需求。
 * 入参：p 是不可空只读唤醒 task；cpu 是初始候选 CPU；flags 是 wake/fork 等选择原因。
 * 出参/返回：返回建议 CPU 编号，找不到更优目标时返回原 cpu；无输出参。
 * 注意事项：RCU 下无锁读取 curr/donor，结果是乐观候选；后续迁移锁路径必须复核 affinity、online 和优先级。
 */
static int
select_task_rq_rt(struct task_struct *p, int cpu, int flags)
{
	struct task_struct *curr, *donor;
	struct rq *rq;
	bool test;

	/* For anything but wake ups, just return the task_cpu */
	/* 只有新建/唤醒允许在此重新选核；其他迁移原因由调度核心的专门路径处理。 */
	if (!(flags & (WF_TTWU | WF_FORK)))
		goto out;

	/* 初始 CPU 已满足 affinity；RCU 窗口只稳定无锁读取所需的调度对象生命周期。 */
	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = READ_ONCE(rq->curr); /* unlocked access */
	donor = READ_ONCE(rq->donor);

	/*
	 * If the current task on @p's runqueue is an RT task, then
	 * try to see if we can wake this RT task up on another
	 * runqueue. Otherwise simply start this RT task
	 * on its current runqueue.
	 *
	 * We want to avoid overloading runqueues. If the woken
	 * task is a higher priority, then it will stay on this CPU
	 * and the lower prio task should be moved to another CPU.
	 * Even though this will probably make the lower prio task
	 * lose its cache, we do not want to bounce a higher task
	 * around just because it gave up its CPU, perhaps for a
	 * lock?
	 *
	 * For equal prio tasks, we just let the scheduler sort it out.
	 *
	 * Otherwise, just let it ride on the affine RQ and the
	 * post-schedule router will push the preempted task away
	 *
	 * This test is optimistic, if we get it wrong the load-balancer
	 * will have to sort it out.
	 *
	 * We take into account the capacity of the CPU to ensure it fits the
	 * requirement of the task - which is only important on heterogeneous
	 * systems like big.LITTLE.
	 */
	/* 当前 CPU 已承载 RT 且不宜移动旧 task，或容量不足时，才支付全域 cpupri 搜索成本。 */
	test = curr &&
	       unlikely(rt_task(donor)) &&
	       (curr->nr_cpus_allowed < 2 || donor->prio <= p->prio);

	if (test || !rt_task_fits_capacity(p, cpu)) {
		int target = find_lowest_rq(p);

		/*
		 * Bail out if we were forcing a migration to find a better
		 * fitting CPU but our search failed.
		 */
		/* 容量触发的迁移不能退化到另一个仍不合适的 CPU；保留原亲和 CPU 交给后续均衡。 */
		if (!test && target != -1 && !rt_task_fits_capacity(p, target))
			goto out_unlock;

		/*
		 * Don't bother moving it if the destination CPU is
		 * not running a lower priority task.
		 */
		/* 只有 p 能严格抢占目标 rq 的最高 RT 优先级时，移动才不会把竞争转移后反而更差。 */
		if (target != -1 &&
		    p->prio < cpu_rq(target)->rt.highest_prio.curr)
			cpu = target;
	}

out_unlock:
	rcu_read_unlock();

out:
	return cpu;
}

/* 同优先级唤醒时若 current 被固定而 @p 可迁移，resched 以创造 push 机会。 */
/*
 * 业务背景：同优先级新 task 无法迁移而 current 可迁移时，应先让调度器运行 push，为固定 task 腾出 CPU。
 * 入参：rq 是不可空输入/输出当前队列；p 是不可空刚唤醒 RT task。
 * 出参/返回：无直接返回值；满足不对称迁移条件时将 p 头插并请求 current 重调度。
 * 注意事项：调用者持 rq 锁；cpupri 查询仅是候选，重排不保证 push 最终成功。
 */
static void check_preempt_equal_prio(struct rq *rq, struct task_struct *p)
{
	/* current 被固定，或 cpupri 找不到它可去的 CPU 时，无法通过 push 为 p 腾位。 */
	if (rq->curr->nr_cpus_allowed == 1 ||
	    !cpupri_find(&rq->rd->cpupri, rq->donor, NULL))
		return;

	/*
	 * p is migratable, so let's not schedule it and
	 * see if it is pushed or pulled somewhere else.
	 */
	/* p 自身存在可接纳目标时让后续常规 push 处理，无需扰动 current。 */
	if (p->nr_cpus_allowed != 1 &&
	    cpupri_find(&rq->rd->cpupri, p, NULL))
		return;

	/*
	 * There appear to be other CPUs that can accept
	 * the current task but none can run 'p', so lets reschedule
	 * to try and push the current task away:
	 */
	/* 仅 current 可迁而 p 无处可去：把 p 放队首并触发一次选择，促使 current 进入 pushable。 */
	requeue_task_rt(rq, p, 1);
	resched_curr(rq);
}

/* pick 前在需要时临时出锁，从 overload CPU 拉取更高优先级 RT task。 */
/*
 * 业务背景：本 rq 优先级下降时，在正式 pick 前尝试拉取远端更高优先级 RT task，减少空闲或优先级反转。
 * 入参：rq 是不可空输入/输出队列；rf 是不可空 rq 锁状态输出对象。
 * 出参/返回：stop、DL 或 RT 任一仍 runnable 时返回非零，否则 0；可能在中途 pull task。
 * 注意事项：函数会 unpin/repin rq 锁但保持中断/抢占约束；donor 可在锁丢失时变化，不能缓存跨越锁降级。
 */
static int balance_rt(struct rq *rq, struct rq_flags *rf)
{
	/*
	 * Note, rq->donor may change during rq lock drops,
	 * so don't re-use p across lock drops
	 */
	struct task_struct *p = rq->donor;

	/* donor 已离开 RT 队列且本 rq 优先级下降时，才可能从远端拉来更合适的 RT task。 */
	if (!on_rt_rq(&p->rt) && need_pull_rt_task(rq, p)) {
		/*
		 * This is OK, because current is on_cpu, which avoids it being
		 * picked for load-balance and preemption/IRQs are still
		 * disabled avoiding further scheduler activity on it and we've
		 * not yet started the picking loop.
		 */
		/* on_cpu 阻止 current 被迁移；关中断/抢占保证本 rq 在临时解锁期间不会本地重入调度。 */
		rq_unpin_lock(rq, rf);
		pull_rt_task(rq);
		rq_repin_lock(rq, rf);
	}

	/* 锁恢复后重新查看各高调度类，返回值只说明 pick 循环是否仍有候选。 */
	return sched_stop_runnable(rq) || sched_dl_runnable(rq) || sched_rt_runnable(rq);
}

/*
 * Preempt the current task with a newly woken task if needed:
 */
/* 新 RT task 更高优先级时立即 resched；相等时检查迁移/push 条件。 */
/*
 * 业务背景：RT 唤醒抢占按严格优先级执行，同优先级只在固定/可迁移不对称时创造 push 机会。
 * 入参：rq 是不可空输入/输出目标队列；p 是不可空新唤醒 task；flags 是未使用唤醒标志输入。
 * 出参/返回：无直接返回值；可能标记 current 需重调度或重排 p。
 * 注意事项：调用者持 rq 锁；非 RT class 直接返回，flags 当前不解引用，等优先级不默认抢占 FIFO task。
 */
static void wakeup_preempt_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct task_struct *donor = rq->donor;

	/*
	 * XXX If we're preempted by DL, queue a push?
	 */
	/* 调度核心可能复用回调处理 class 已变化的 task；非 RT 不能套用 RT 优先级语义。 */
	if (p->sched_class != &rt_sched_class)
		return;

	/* 数值更小即严格更高优先级，立即设置 resched 而不改变 FIFO 队列位置。 */
	if (p->prio < donor->prio) {
		resched_curr(rq);
		return;
	}

	/*
	 * If:
	 *
	 * - the newly woken task is of equal priority to the current task
	 * - the newly woken task is non-migratable while current is migratable
	 * - current will be preempted on the next reschedule
	 *
	 * we should check to see if current can readily move to a different
	 * cpu.  If so, we will reschedule to allow the push logic to try
	 * to move current somewhere else, making room for our non-migratable
	 * task.
	 */
	/* 已经请求重调度时无需重复等优先级迁移探测，下一次调度自然会运行 push 逻辑。 */
	if (p->prio == donor->prio && !test_tsk_need_resched(rq->curr))
		check_preempt_equal_prio(rq, p);
}

/* 选中 @p 时从 pushable 摘除、结束等待统计并建立 exec_start/PELT 基线。 */
/*
 * 业务背景：调度核心选定 RT task 后要把它从“等待/可迁”状态转换为“正在执行”，并建立后续计费基线。
 * 入参：rq 是不可空输入/输出当前 runqueue；p 是不可空被选 task；first 表示本次是否首次完成 class 切入。
 * 出参/返回：无直接返回值、无输出参数；更新 exec_start/等待统计，摘 pushable，首次切入时更新 PELT 并登记 push。
 * 注意事项：调用者持 rq 锁；p 必须已属于该 rq，dequeue_pushable_task() 依赖调度核心维持节点有效性。
 */
static inline void set_next_task_rt(struct rq *rq, struct task_struct *p, bool first)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq = &rq->rt;

	/* exec_start 是下次 update_curr_common() 的差值基线，必须在 task 获得 CPU 时刷新。 */
	p->se.exec_start = rq_clock_task(rq);
	if (on_rt_rq(&p->rt))
		update_stats_wait_end_rt(rt_rq, rt_se);

	/* The running task is never eligible for pushing */
	/* 运行中的 task 只能经抢占/stopper 协议迁移，不能留在普通 pushable plist。 */
	dequeue_pushable_task(rq, p);

	/* 同一次 pick 的重复 set_next 不应重复切换 PELT 状态或排 balance callback。 */
	if (!first)
		return;

	/*
	 * If prev task was rt, put_prev_task() has already updated the
	 * utilization. We only care of the case where we start to schedule a
	 * rt task
	 */
	/* 从非 RT class 首次切入时才需把 rq 的 RT PELT 信号推进到当前时刻。 */
	if (rq->donor->sched_class != &rt_sched_class)
		update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 0);

	rt_queue_push_tasks(rq);
}

/* 由 bitmap 找最高非空 priority，并返回对应 FIFO list 队首 entity。 */
/*
 * 业务背景：RT 选择必须以 O(bitmap words) 找到严格最高优先级，再遵循该级 FIFO/RR 链表顺序。
 * 入参：rt_rq 是不可空只读/内部缓存输入队列，调用者持所属 rq 锁且队列已发布为非空。
 * 出参/返回：成功返回最高优先级队首 entity 借用指针；位图/链表失配时告警并返回 NULL；无输出参数。
 * 注意事项：不增加 entity 引用；返回值只在 rq 锁与层级生命周期内有效，idx 越界表示严重不变量破坏并 BUG。
 */
static struct sched_rt_entity *pick_next_rt_entity(struct rt_rq *rt_rq)
{
	struct rt_prio_array *array = &rt_rq->active;
	struct sched_rt_entity *next = NULL;
	struct list_head *queue;
	int idx;

	/* 位图最低置位对应数值最小、优先级最高的非空 FIFO 队列。 */
	idx = sched_find_first_bit(array->bitmap);
	BUG_ON(idx >= MAX_RT_PRIO);

	/* bitmap/list 不一致属于 rq 锁协议破坏：告警并避免解引用空表头。 */
	queue = array->queue + idx;
	if (WARN_ON_ONCE(list_empty(queue)))
		return NULL;
	/* 队首保持 SCHED_FIFO 到达顺序，RR 只在时间片到期时显式移到尾部。 */
	next = list_entry(queue->next, struct sched_rt_entity, run_list);

	return next;
}

/* 从顶层 entity 沿 group rt_rq 下钻到叶子 task；throttle/空队列返回 NULL。 */
/*
 * 业务背景：组调度把 task 隐藏在多级 group entity 后，最终 pick 必须逐层选择最高节点直到恢复实际 task。
 * 入参：rq 是不可空只读当前 runqueue，调用者持 rq 锁且顶层 RT 队列已判定 runnable。
 * 出参/返回：成功返回最高优先级 task 借用指针；任一层位图/链表异常返回 NULL；无输出参数。
 * 注意事项：不增加 task 引用；每个已入队 group 理应拥有非空未节流子队列，否则说明层级发布协议失配。
 */
static struct task_struct *_pick_next_task_rt(struct rq *rq)
{
	struct sched_rt_entity *rt_se;
	struct rt_rq *rt_rq  = &rq->rt;

	/* 每次选本层最高 entity；group entity 转入子队列，直到遇到 task 叶子。 */
	do {
		rt_se = pick_next_rt_entity(rt_rq);
		if (unlikely(!rt_se))
			return NULL;
		/* task 返回 NULL 结束下钻；group 返回 my_q，且其非空性由入队门禁保证。 */
		rt_rq = group_rt_rq(rt_se);
	} while (rt_rq);

	return rt_task_of(rt_se);
}

/* 先执行 balance/pull，再选择最高优先级 task；无 RT 可运行返回 NULL。 */
/*
 * 业务背景：调度核心需要 RT class 提供当前 rq 的最高候选；无可运行 RT 时应让更低调度类继续选择。
 * 入参：rq 是不可空当前 runqueue；rf 是未使用的 rq 锁状态借用输入，平衡由独立 balance 回调承担。
 * 出参/返回：有 RT runnable 时返回最高 task 借用指针，否则返回 NULL；无输出参数。
 * 注意事项：调用者持 rq 锁；返回 task 生命周期由 rq/on_rq 协议稳定，函数本身不执行 pull 或迁移。
 */
static struct task_struct *pick_task_rt(struct rq *rq, struct rq_flags *rf)
{
	struct task_struct *p;

	/* 顶层未发布时不进入层级位图，避免在 throttle/空队列上 BUG。 */
	if (!sched_rt_runnable(rq))
		return NULL;

	/* balance 已由调度核心在 pick 协议的相应阶段调用；这里只做纯最高优先级选择。 */
	p = _pick_next_task_rt(rq);

	return p;
}

/* 切出 @p 前结算运行时间，若仍 runnable 则重新加入 pushable 候选。 */
/*
 * 业务背景：RT task 失去 CPU 时要关闭执行计费区间，并把仍 runnable 且可迁的 task 重新暴露给负载均衡。
 * 入参：rq 是不可空输入/输出当前 runqueue；p 是不可空切出 task；next 是未使用的下一个 task 借用输入。
 * 出参/返回：无直接返回值、无输出参数；更新等待统计/runtime/PELT，并可能把 p 加入 pushable plist。
 * 注意事项：调用者持 rq 锁；blocked task 不可 push，只有仍 on_rq 且 affinity 多 CPU 的 task 才重新发布。
 */
static void put_prev_task_rt(struct rq *rq, struct task_struct *p, struct task_struct *next)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq = &rq->rt;

	/* 仍 runnable 的 prev 从切出时刻重新开始累计 runqueue 等待；阻塞者已由 dequeue 统计。 */
	if (on_rt_rq(&p->rt))
		update_stats_wait_start_rt(rt_rq, rt_se);

	/* 在覆盖 exec_start 或改变 class PELT 前结算最后一段实际运行时间和带宽。 */
	update_curr_rt(rq);

	/* running=1 的更新关闭本次 RT 执行区间，并把利用率传播给频率/负载消费者。 */
	update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 1);

	if (task_is_blocked(p))
		return;
	/*
	 * The previous task needs to be made eligible for pushing
	 * if it is still active
	 */
	/* 仍在 rq 且有多个 affinity CPU 的 prev 在切出后重新成为可 push 候选。 */
	if (on_rt_rq(&p->rt) && p->nr_cpus_allowed > 1)
		enqueue_pushable_task(rq, p);
}

/* Only try algorithms three times */
#define RT_MAX_TRIES 3

/*
 * Return the highest pushable rq's task, which is suitable to be executed
 * on the CPU, NULL otherwise
 */
/* 从 @rq pushable plist 找亲和性允许 @cpu 的最高优先级 task；仅返回候选。 */
/*
 * 业务背景：pull 路径在锁住源 rq 后需要找到既最高优先又能在目标 CPU 运行的 queued RT task。
 * 入参：rq 是不可空只读源 runqueue；cpu 是有效目标 CPU 编号，调用者持 rq 锁。
 * 出参/返回：返回首个满足 task_is_pushable() 的 task 借用指针，找不到返回 NULL；无输出参数。
 * 注意事项：不增加 task 引用；plist 顺序保证首个合格者优先级最高，真正迁移仍需双 rq 锁条件。
 */
static struct task_struct *pick_highest_pushable_task(struct rq *rq, int cpu)
{
	struct plist_head *head = &rq->rt.pushable_tasks;
	struct task_struct *p;

	/* 空 plist 快速返回；非空表按 prio 排序，首个满足迁移协议者即为全局最优候选。 */
	if (!has_pushable_tasks(rq))
		return NULL;

	/* task_is_pushable() 复核 on_rq、非 current、affinity 与目标 CPU 等动态条件。 */
	plist_for_each_entry(p, head, pushable_tasks) {
		if (task_is_pushable(rq, p, cpu))
			return p;
	}

	return NULL;
}

/* find_lowest_rq() 的每 CPU 临时候选集，避免 RT 选核热路径动态分配并隔离并发调用者的写入。 */
static DEFINE_PER_CPU(cpumask_var_t, local_cpu_mask);

/* 用 cpupri/亲和性/容量为 @task 找最低优先级 CPU；无合适目标返回 -1。 */
/*
 * 业务背景：RT push/唤醒要把 task 放到当前 RT 负载最低且拓扑/容量合适的 CPU，减少高优先级竞争与缓存损失。
 * 入参：task 是不可空只读 RT task，调用者通过 rq/pi/RCU 协议稳定 affinity、root_domain 与调度域。
 * 出参/返回：返回一个候选 CPU 编号；无临时 mask、单 CPU affinity 或无合适目标时返回 -1；无输出参数。
 * 注意事项：结果是无锁候选且不持目标 rq 锁；调用者必须复核 online、affinity、priority 与 migration-disabled。
 */
static int find_lowest_rq(struct task_struct *task)
{
	struct sched_domain *sd;
	struct cpumask *lowest_mask = this_cpu_cpumask_var_ptr(local_cpu_mask);
	int this_cpu = smp_processor_id();
	int cpu      = task_cpu(task);
	int ret;

	/* Make sure the mask is initialized first */
	/* per-CPU 临时 mask 在 CPU 初始化阶段分配；缺失时不能安全调用 cpupri 填充。 */
	if (unlikely(!lowest_mask))
		return -1;

	/* 单 CPU affinity 没有替代目的地，跳过 root-domain 与 sched-domain 扫描。 */
	if (task->nr_cpus_allowed == 1)
		return -1; /* No other targets possible */

	/*
	 * If we're on asym system ensure we consider the different capacities
	 * of the CPUs when searching for the lowest_mask.
	 */
	/* 异构系统让 cpupri 同时过滤有效容量；同构系统只按优先级和 affinity 建 mask。 */
	if (sched_asym_cpucap_active()) {

		ret = cpupri_find_fitness(&task_rq(task)->rd->cpupri,
					  task, lowest_mask,
					  rt_task_fits_capacity);
	} else {

		ret = cpupri_find(&task_rq(task)->rd->cpupri,
				  task, lowest_mask);
	}

	/* cpupri 未找到比 task 更低优先级的兼容 rq，调用者保留原位置。 */
	if (!ret)
		return -1; /* No targets found */

	/*
	 * At this point we have built a mask of CPUs representing the
	 * lowest priority tasks in the system.  Now we want to elect
	 * the best one based on our affinity and topology.
	 *
	 * We prioritize the last CPU that the task executed on since
	 * it is most likely cache-hot in that location.
	 */
	/* 上次运行 CPU 若已在最低优先级集合中，缓存局部性优先于进一步拓扑选择。 */
	if (cpumask_test_cpu(cpu, lowest_mask))
		return cpu;

	/*
	 * Otherwise, we consult the sched_domains span maps to figure
	 * out which CPU is logically closest to our hot cache data.
	 */
	/* 当前 CPU 只有同时属于候选集才可作为本地抢占优化，否则设 -1 禁用该分支。 */
	if (!cpumask_test_cpu(this_cpu, lowest_mask))
		this_cpu = -1; /* Skip this_cpu opt if not among lowest */

	/* sched_domain 可由拓扑重建替换，RCU 保证遍历期间节点内存有效。 */
	rcu_read_lock();
	for_each_domain(cpu, sd) {
		if (sd->flags & SD_WAKE_AFFINE) {
			int best_cpu;

			/*
			 * "this_cpu" is cheaper to preempt than a
			 * remote processor.
			 */
			/* 当前 CPU 位于本层 affine span 时优先本地，减少 IPI 与缓存迁移。 */
			if (this_cpu != -1 &&
			    cpumask_test_cpu(this_cpu, sched_domain_span(sd))) {
				rcu_read_unlock();
				return this_cpu;
			}

			/* 否则在本层 span 与候选交集中分散选择，避免所有唤醒集中到首个 CPU。 */
			best_cpu = cpumask_any_and_distribute(lowest_mask,
							      sched_domain_span(sd));
			if (best_cpu < nr_cpu_ids) {
				rcu_read_unlock();
				return best_cpu;
			}
		}
	}
	rcu_read_unlock();

	/*
	 * And finally, if there were no matches within the domains
	 * just give the caller *something* to work with from the compatible
	 * locations.
	 */
	/* 没有 SD_WAKE_AFFINE 匹配时仍返回任一 cpupri 候选，正确性由候选 mask 保证。 */
	if (this_cpu != -1)
		return this_cpu;

	cpu = cpumask_any_distribute(lowest_mask);
	if (cpu < nr_cpu_ids)
		return cpu;

	return -1;
}

/* 返回并校验 pushable plist 表头不是 current、已排队且可迁移。 */
/*
 * 业务背景：push 源 rq 需要选一个未在 CPU 上执行的最高优先级 queued task，并在进入迁移前验证 plist 不变量。
 * 入参：rq 是不可空输入/输出源 runqueue，调用者持 rq 锁。
 * 出参/返回：返回最高可处理 task 的借用指针；列表空或所有候选 on_cpu 时返回 NULL；无输出参数。
 * 注意事项：发现归属/current/affinity/on_rq/class 不变量破坏会 BUG；proxy-exec 场景允许跳过 on_cpu 表头。
 */
static struct task_struct *pick_next_pushable_task(struct rq *rq)
{
	struct plist_head *head = &rq->rt.pushable_tasks;
	struct task_struct *i, *p = NULL;

	/* plist 为空无需扫描；非空时 proxy-exec 可能让表头 task 暂时 on_cpu。 */
	if (!has_pushable_tasks(rq))
		return NULL;

	/* 跳过任何正在 CPU 上执行的候选，选择最高优先级的静止 queued task。 */
	plist_for_each_entry(i, head, pushable_tasks) {
		/* make sure task isn't on_cpu (possible with proxy-exec) */
		if (!task_on_cpu(rq, i)) {
			p = i;
			break;
		}
	}

	/* 所有节点均 on_cpu 时本轮不能安全迁移，等待后续状态变化重新尝试。 */
	if (!p)
		return NULL;

	/* 以下断言验证 pushable plist 的核心不变量，避免带错误归属进入双 rq 迁移。 */
	BUG_ON(rq->cpu != task_cpu(p));
	BUG_ON(task_current(rq, p));
	BUG_ON(task_current_donor(rq, p));
	BUG_ON(p->nr_cpus_allowed <= 1);

	/* 候选必须仍在普通 rq 队列且属于 RT class，否则 move_queued_task_locked() 不成立。 */
	BUG_ON(!task_on_rq_queued(p));
	BUG_ON(!rt_task(p));

	return p;
}

/* Will lock the rq it finds */
/* 尝试锁目标 rq 并复核 task/优先级/亲和性；竞态时有限重试，失败返回 NULL。 */
/*
 * 业务背景：cpupri 只给无锁目标候选，实际 push 必须按 rq 锁序取得双锁并确认 task 和目标仍适合迁移。
 * 入参：task 是不可空持引用候选；rq 是不可空源 runqueue，调用者进入时持其 rq 锁。
 * 出参/返回：成功返回已锁住的目标 rq，并保持源 rq 锁；失败返回 NULL且只保持源锁；无输出参数。
 * 注意事项：最多 RT_MAX_TRIES 次；double_lock_balance() 可能释放源锁，必须复核 affinity、表头和 migration-disabled。
 */
static struct rq *find_lock_lowest_rq(struct task_struct *task, struct rq *rq)
{
	struct rq *lowest_rq = NULL;
	int tries;
	int cpu;

	/* 无锁候选可能在加双锁前失效，最多三次重新搜索以限制 rq 锁热路径延迟。 */
	for (tries = 0; tries < RT_MAX_TRIES; tries++) {
		cpu = find_lowest_rq(task);

		/* 无候选或仍是源 CPU 都不能形成迁移，直接结束本轮。 */
		if ((cpu == -1) || (cpu == rq->cpu))
			break;

		lowest_rq = cpu_rq(cpu);

		/* 目标已有等高或更高 RT task 时迁移无收益，且不释放源锁即可确定失败。 */
		if (lowest_rq->rt.highest_prio.curr <= task->prio) {
			/*
			 * Target rq has tasks of equal or higher priority,
			 * retrying does not release any lock and is unlikely
			 * to yield a different result.
			 */
			lowest_rq = NULL;
			break;
		}

		/* if the prio of this runqueue changed, try again */
		/* helper 可能为锁序释放并重取源 rq；返回真表示必须完整复核 task 身份和 affinity。 */
		if (double_lock_balance(rq, lowest_rq)) {
			/*
			 * We had to unlock the run queue. In
			 * the mean time, task could have
			 * migrated already or had its affinity changed,
			 * therefore check if the task is still at the
			 * head of the pushable tasks list.
			 * It is possible the task was scheduled, set
			 * "migrate_disabled" and then got preempted, so we must
			 * check the task migration disable flag here too.
			 */
			/* 任一条件变化都使旧 cpupri 决策失效；双解锁后停止使用该候选。 */
			if (unlikely(is_migration_disabled(task) ||
				     !cpumask_test_cpu(lowest_rq->cpu, &task->cpus_mask) ||
				     task != pick_next_pushable_task(rq))) {

				double_unlock_balance(rq, lowest_rq);
				lowest_rq = NULL;
				break;
			}
		}

		/* If this rq is still suitable use it. */
		/* 双锁快照中 task 能严格抢占目标最高 RT task，返回时两把 rq 锁均保持。 */
		if (lowest_rq->rt.highest_prio.curr > task->prio)
			break;

		/* try again */
		/* 目标在加锁期间变得不合适，释放目标并重新运行 cpupri 搜索。 */
		double_unlock_balance(rq, lowest_rq);
		lowest_rq = NULL;
	}

	return lowest_rq;
}

/*
 * If the current CPU has more than one RT task, see if the non
 * running task can migrate over to a CPU that is running a task
 * of lesser priority.
 */
/* 从 overload @rq 迁出一个 pushable task；双 rq 锁复核后移动，成功返回 1。 */
/*
 * 业务背景：一个 CPU 同时排队多个 RT task 时，应把非当前可迁 task 推到能被它抢占的较低优先级 CPU。
 * 入参：rq 是不可空输入/输出源 runqueue且调用者持 rq 锁；pull 表示本次由远端拉取请求触发，可启用 stopper 退化路径。
 * 出参/返回：成功迁移一个 queued task 返回 1，否则返回 0；无输出参数，可能 resched 源/目标或提交 stop work。
 * 注意事项：不可睡眠；会临时释放 rq 锁并持 task 引用，所有候选条件必须锁后复核，migration-disabled 不能直接移动。
 */
static int push_rt_task(struct rq *rq, bool pull)
{
	struct task_struct *next_task;
	struct rq *lowest_rq;
	int ret = 0;

	/* overload 表示存在多余可迁 RT task；未发布时无需触碰 plist 或 cpupri。 */
	if (!rq->rt.overloaded)
		return 0;

	/* 从源 rq 锁保护的 plist 取得当前最高优先级且不在 CPU 上的候选。 */
	next_task = pick_next_pushable_task(rq);
	if (!next_task)
		return 0;

retry:
	/*
	 * It's possible that the next_task slipped in of
	 * higher priority than current. If that's the case
	 * just reschedule current.
	 */
	/* 候选比 donor 更高说明唤醒尚未完成抢占；先让本 CPU 调度，不能把最高任务推走。 */
	if (unlikely(next_task->prio < rq->donor->prio)) {
		resched_curr(rq);
		return 0;
	}

	/* migration-disabled 候选不能直接 move；pull 发起时可改为用 stopper 推走当前 task。 */
	if (is_migration_disabled(next_task)) {
		struct task_struct *push_task = NULL;
		int cpu;

		/* 普通本地 push 不启动 stopper；push_busy 防止同一 rq 并发提交多个 stop work。 */
		if (!pull || rq->push_busy)
			return 0;

		/*
		 * Invoking find_lowest_rq() on anything but an RT task doesn't
		 * make sense. Per the above priority check, curr has to
		 * be of higher priority than next_task, so no need to
		 * reschedule when bailing out.
		 *
		 * Note that the stoppers are masqueraded as SCHED_FIFO
		 * (cf. sched_set_stop_task()), so we can't rely on rt_task().
		 */
		/* donor 必须是真 RT class；伪装 FIFO 的 stopper 不能作为 get_push_task() 的迁移对象。 */
		if (rq->donor->sched_class != &rt_sched_class)
			return 0;

		/* 为当前 task 找替代 CPU；无目标时无法给固定候选腾出本 CPU。 */
		cpu = find_lowest_rq(rq->curr);
		if (cpu == -1 || cpu == rq->cpu)
			return 0;

		/*
		 * Given we found a CPU with lower priority than @next_task,
		 * therefore it should be running. However we cannot migrate it
		 * to this other CPU, instead attempt to push the current
		 * running task on this CPU away.
		 */
		/* get_push_task() 固定 task/rq 状态，stopper 在目标 CPU 上越过普通 migration-disabled 限制。 */
		push_task = get_push_task(rq);
		if (push_task) {
			/* stop_one_cpu_nowait() 不能在持 rq 锁时提交；task 引用/忙标志由 push 协议管理。 */
			preempt_disable();
			raw_spin_rq_unlock(rq);
			stop_one_cpu_nowait(rq->cpu, push_cpu_stop,
					    push_task, &rq->push_work);
			preempt_enable();
			raw_spin_rq_lock(rq);
		}

		return 0;
	}

	/* plist 不应包含 current；若不变量破坏，拒绝迁移以免移动正在执行的 task。 */
	if (WARN_ON(next_task == rq->curr))
		return 0;

	/* We might release rq lock */
	/* find_lock_lowest_rq() 可能释放源锁，显式引用保证候选跨该窗口不被释放。 */
	get_task_struct(next_task);

	/* find_lock_lowest_rq locks the rq if found */
	/* 成功返回时源/目标 rq 均已锁住且迁移条件经过复核。 */
	lowest_rq = find_lock_lowest_rq(next_task, rq);
	if (!lowest_rq) {
		struct task_struct *task;
		/*
		 * find_lock_lowest_rq releases rq->lock
		 * so it is possible that next_task has migrated.
		 *
		 * We need to make sure that the task is still on the same
		 * run-queue and is also still the next task eligible for
		 * pushing.
		 */
		/* 重新读取源 plist，区分“同一 task 暂无目标”和“候选已变化可重试”。 */
		task = pick_next_pushable_task(rq);
		if (task == next_task) {
			/*
			 * The task hasn't migrated, and is still the next
			 * eligible task, but we failed to find a run-queue
			 * to push it to.  Do not retry in this case, since
			 * other CPUs will pull from us when ready.
			 */
			goto out;
		}

		if (!task)
			/* No more tasks, just exit */
			goto out;

		/*
		 * Something has shifted, try again.
		 */
		/* 旧引用先释放，再以新表头继续，避免把过期 task 带入下一轮。 */
		put_task_struct(next_task);
		next_task = task;
		goto retry;
	}

	/* 双锁下原子撤销源队列索引并加入目标队列，task_cpu/亲和可见性随 helper 更新。 */
	move_queued_task_locked(rq, lowest_rq, next_task);
	/* 新 task 能抢占目标低优先级工作，立即请求目标重新调度。 */
	resched_curr(lowest_rq);
	ret = 1;

	/* helper 约定返回前释放目标并恢复仅持源 rq 锁的调用状态。 */
	double_unlock_balance(rq, lowest_rq);
out:
	/* 对应跨解锁窗口前的 get_task_struct()，所有出口统一释放。 */
	put_task_struct(next_task);

	return ret;
}

/* 循环 push 直到当前 rq 不再有可迁出的多余 RT task。 */
/*
 * 业务背景：一次迁移后源 rq 可能仍 overload，balance callback 应持续清理直到无合适目标或无多余 task。
 * 入参：rq 是不可空输入/输出源 runqueue，调用者持 rq 锁。
 * 出参/返回：无直接返回值、无输出参数；可能迁移多个 RT task并触发多个目标 CPU resched。
 * 注意事项：不可睡眠；push_rt_task() 返回 0也可能只是暂时无目标，后续由 pull/新事件再次触发。
 */
static void push_rt_tasks(struct rq *rq)
{
	/* push_rt_task will return true if it moved an RT */
	while (push_rt_task(rq, false))
		;
}

#ifdef HAVE_RT_PUSH_IPI

/*
 * When a high priority task schedules out from a CPU and a lower priority
 * task is scheduled in, a check is made to see if there's any RT tasks
 * on other CPUs that are waiting to run because a higher priority RT task
 * is currently running on its CPU. In this case, the CPU with multiple RT
 * tasks queued on it (overloaded) needs to be notified that a CPU has opened
 * up that may be able to run one of its non-running queued RT tasks.
 *
 * All CPUs with overloaded RT tasks need to be notified as there is currently
 * no way to know which of these CPUs have the highest priority task waiting
 * to run. Instead of trying to take a spinlock on each of these CPUs,
 * which has shown to cause large latency when done on machines with many
 * CPUs, sending an IPI to the CPUs to have them push off the overloaded
 * RT tasks waiting to run.
 *
 * Just sending an IPI to each of the CPUs is also an issue, as on large
 * count CPU machines, this can cause an IPI storm on a CPU, especially
 * if its the only CPU with multiple RT tasks queued, and a large number
 * of CPUs scheduling a lower priority task at the same time.
 *
 * Each root domain has its own IRQ work function that can iterate over
 * all CPUs with RT overloaded tasks. Since all CPUs with overloaded RT
 * task must be checked if there's one or many CPUs that are lowering
 * their priority, there's a single IRQ work iterator that will try to
 * push off RT tasks that are waiting to run.
 *
 * When a CPU schedules a lower priority task, it will kick off the
 * IRQ work iterator that will jump to each CPU with overloaded RT tasks.
 * As it only takes the first CPU that schedules a lower priority task
 * to start the process, the rto_start variable is incremented and if
 * the atomic result is one, then that CPU will try to take the rto_lock.
 * This prevents high contention on the lock as the process handles all
 * CPUs scheduling lower priority tasks.
 *
 * All CPUs that are scheduling a lower priority task will increment the
 * rt_loop_next variable. This will make sure that the IRQ work iterator
 * checks all RT overloaded CPUs whenever a CPU schedules a new lower
 * priority task, even if the iterator is in the middle of a scan. Incrementing
 * the rt_loop_next will cause the iterator to perform another scan.
 *
 */
/*
 * 当某 CPU 降低当前任务优先级时，所有 RT overload CPU 都可能持有可迁入的最高优先级任务，
 * 但发起方既不能预知来源，也不宜逐个争抢远端 rq 锁。每个 root-domain 因此只启动一条
 * irq_work 接力链，让各来源 CPU 自己执行 push；rto_loop_start 保证只有首个请求者启动链，
 * rto_lock 串行维护迭代位置。扫描期间每个新请求都会增加 rto_loop_next，使接力链至少再完整
 * 扫描一轮，既不遗漏新机会，也避免向大量 CPU 同时广播 IPI 形成风暴。
 */
/* 在 rto_lock 下轮转 overload mask；loop_next 变化时再扫一轮，结束返回 -1。 */
/*
 * 业务背景：RT push IPI 用单一 irq_work 在所有 overload CPU 间接力，并须覆盖扫描期间新到达的低优先级事件。
 * 入参：rd 是不可空输入/输出 root_domain，调用者持 rd->rto_lock且其引用在异步链期间有效。
 * 出参/返回：返回下一个需接力的 CPU 编号；mask 扫完且 loop 代数稳定时返回 -1；更新 rto_cpu/rto_loop。
 * 注意事项：不可睡眠；跳过当前 CPU，acquire 读取与 overload 发布 WMB 配对，mask 可在扫描中并发变化。
 */
static int rto_next_cpu(struct root_domain *rd)
{
	int this_cpu = smp_processor_id();
	int next;
	int cpu;

	/*
	 * When starting the IPI RT pushing, the rto_cpu is set to -1,
	 * rt_next_cpu() will simply return the first CPU found in
	 * the rto_mask.
	 *
	 * If rto_next_cpu() is called with rto_cpu is a valid CPU, it
	 * will return the next CPU found in the rto_mask.
	 *
	 * If there are no more CPUs left in the rto_mask, then a check is made
	 * against rto_loop and rto_loop_next. rto_loop is only updated with
	 * the rto_lock held, but any CPU may increment the rto_loop_next
	 * without any locking.
	 */
	/* 一轮扫描可能因并发 loop_next 增长而接续下一轮，直到观察到稳定代数。 */
	for (;;) {

		/* When rto_cpu is -1 this acts like cpumask_first() */
		/* rto_cpu=-1 时等价 first；否则从上次位置继续，避免重复通知已处理 CPU。 */
		cpu = cpumask_next(rd->rto_cpu, rd->rto_mask);

		rd->rto_cpu = cpu;

		/* Do not send IPI to self */
		/* 当前 CPU 已在 hardirq 中执行回调，无需给自己再排同一 irq_work。 */
		if (cpu == this_cpu)
			continue;

		/* 找到有效 overload CPU 就返回，由调用者把同一 work 接力过去。 */
		if (cpu < nr_cpu_ids)
			return cpu;

		/* mask 已走到末尾，复位游标并比较是否有扫描期间到达的新请求。 */
		rd->rto_cpu = -1;

		/*
		 * ACQUIRE ensures we see the @rto_mask changes
		 * made prior to the @next value observed.
		 *
		 * Matches WMB in rt_set_overload().
		 */
		/* acquire 看到新 loop 代数时，也必须看到发起者在递增前发布的 overload mask。 */
		next = atomic_read_acquire(&rd->rto_loop_next);

		/* 代数不变表示没有漏掉并发请求，本次接力链可以结束并释放 rd 引用。 */
		if (rd->rto_loop == next)
			break;

		rd->rto_loop = next;
	}

	return -1;
}

/* acquire cmpxchg 竞争 RT push IPI 扫描启动权。 */
/*
 * 业务背景：多个 CPU 同时降优先级时只能有一个负责启动或续接 root-domain push IPI 扫描。
 * 入参：v 是不可空输入/输出原子启动门，0 表示空闲、1 表示已有发起者。
 * 出参/返回：成功把 0改成1返回 true，竞争失败返回 false；无输出参数。
 * 注意事项：acquire 保证成功者观察前一 release 解锁前的游标状态；调用者最终必须 rto_start_unlock()。
 */
static inline bool rto_start_trylock(atomic_t *v)
{
	return !atomic_cmpxchg_acquire(v, 0, 1);
}

/* release 清启动权，使下一发起者看到本轮 rto 状态更新。 */
/*
 * 业务背景：push IPI 发起者完成游标检查后释放启动门，让后续低优先级事件决定是否需要重新启动扫描。
 * 入参：v 是不可空输入/输出且当前值由调用者拥有的原子启动门。
 * 出参/返回：无直接返回值、无输出参数；以 release 语义把门写回0。
 * 注意事项：必须与成功的 rto_start_trylock() 配对；误解锁会允许并行发起者破坏单接力链不变量。
 */
static inline void rto_start_unlock(atomic_t *v)
{
	atomic_set_release(v, 0);
}

/* 合并并发请求，取得 root-domain 引用后把 irq_work 发往首个 overload CPU。 */
/*
 * 业务背景：本 CPU 出现承载空间时通知所有 overload CPU 自主 push；并发通知通过 loop 代数合并，避免 IPI 风暴。
 * 入参：rq 是不可空输入/输出当前 runqueue，调用者持 rq 锁且其 root_domain 生命周期稳定。
 * 出参/返回：无直接返回值、无输出参数；可能增加 loop 代数、取得 rd 引用并向首个 overload CPU 排 irq_work。
 * 注意事项：不可睡眠；只有启动门赢家操作 rto 游标，已有接力活动时仅 loop_next 增量产生效果。
 */
static void tell_cpu_to_push(struct rq *rq)
{
	int cpu = -1;

	/* Keep the loop going if the IPI is currently active */
	/* 每个低优先级切入事件都增加代数，迫使正在运行的扫描至少再覆盖一轮 mask。 */
	atomic_inc(&rq->rd->rto_loop_next);

	/* Only one CPU can initiate a loop at a time */
	/* start 原子门只选一个启动者；失败者的 loop_next 增量仍会被现有扫描消费。 */
	if (!rto_start_trylock(&rq->rd->rto_loop_start))
		return;

	/* rto_lock 串行游标与接力 work，防止两个发起者从同一 mask 位置并行启动。 */
	raw_spin_lock(&rq->rd->rto_lock);

	/*
	 * The rto_cpu is updated under the lock, if it has a valid CPU
	 * then the IPI is still running and will continue due to the
	 * update to loop_next, and nothing needs to be done here.
	 * Otherwise it is finishing up and an IPI needs to be sent.
	 */
	/* 有效游标说明接力仍活动，只需 loop_next 请求下一轮；-1 才选择首个 CPU 重启。 */
	if (rq->rd->rto_cpu < 0)
		cpu = rto_next_cpu(rq->rd);

	raw_spin_unlock(&rq->rd->rto_lock);

	/* 游标决策完成后 release 启动门，使后续发起者看到最新 rto_cpu。 */
	rto_start_unlock(&rq->rd->rto_loop_start);

	if (cpu >= 0) {
		/* Make sure the rd does not get freed while pushing */
		/* irq_work 可跨调度域重建异步执行，引用把 root_domain 保活到接力链结束。 */
		sched_get_rd(rq->rd);
		irq_work_queue_on(&rq->rd->rto_push_work, cpu);
	}
}

/* Called from hardirq context */
/* hardirq 中推空本 CPU 可迁 RT task，再把持引用的 irq_work 传给下一 CPU。 */
/*
 * 业务背景：RT push IPI 接力到达 overload CPU 后，由源 CPU 在本地 rq 锁下主动迁出等待 task，再继续全域扫描。
 * 入参：work 是不可空、内嵌于持引用 root_domain 的 rto_push_work。
 * 出参/返回：无直接返回值、无输出参数；可能迁移多个 task、把 work 排到下一 CPU，末端归还 rd 引用。
 * 注意事项：hardirq 上下文不可睡眠；同一 irq_work 串行接力，锁外 has_pushable 仅作提示，真实迁移在 rq 锁下复核。
 */
void rto_push_irq_work_func(struct irq_work *work)
{
	struct root_domain *rd =
		container_of(work, struct root_domain, rto_push_work);
	struct rq *rq;
	int cpu;

	/* work 被明确排到 overload CPU，本地 rq 即当前需要尝试清空 pushable 的源队列。 */
	rq = this_rq();

	/*
	 * We do not need to grab the lock to check for has_pushable_tasks.
	 * When it gets updated, a check is made if a push is possible.
	 */
	/* 锁外非空只是优化提示；真时进 rq 锁并循环，假阴性由后续发布再触发扫描。 */
	if (has_pushable_tasks(rq)) {
		raw_spin_rq_lock(rq);
		while (push_rt_task(rq, true))
			;
		raw_spin_rq_unlock(rq);
	}

	/* 本 CPU 推送完成后，在游标锁下选择接力目标或判断整轮稳定结束。 */
	raw_spin_lock(&rd->rto_lock);

	/* Pass the IPI to the next rt overloaded queue */
	cpu = rto_next_cpu(rd);

	raw_spin_unlock(&rd->rto_lock);

	/* 无下一个 CPU 且无新 loop 请求：本轮最后一个回调归还 root-domain 引用。 */
	if (cpu < 0) {
		sched_put_rd(rd);
		return;
	}

	/* Try the next RT overloaded CPU */
	/* 同一个 irq_work 对象串行接力，不会在多个 CPU 上并发执行。 */
	irq_work_queue_on(&rd->rto_push_work, cpu);
}
#endif /* HAVE_RT_PUSH_IPI */

/* 从其他 overload rq 拉取能抢占本 rq 的最高 RT task；双锁后复核所有候选。 */
/*
 * 业务背景：本 CPU 最高 RT 优先级下降后，应从 root-domain overload 队列拉取能立即抢占的远端 task，减少等待延迟。
 * 入参：this_rq 是不可空输入/输出目标 runqueue，调用者持其 rq 锁。
 * 出参/返回：无直接返回值、无输出参数；可能迁入一个或多个更高优先级 task、提交 stopper 或请求本 CPU resched。
 * 注意事项：不可睡眠；可能在双锁/stopper 提交时释放并重取 this_rq 锁，mask/cpupri 都是候选且需锁后复核。
 */
static void pull_rt_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu, cpu;
	bool resched = false;
	struct task_struct *p, *push_task;
	struct rq *src_rq;
	int rt_overload_count = rt_overloaded(this_rq);

	/* 原子计数为零时没有已发布源队列，避免读取 mask 和跨 rq 锁。 */
	if (likely(!rt_overload_count))
		return;

	/*
	 * Match the barrier from rt_set_overloaded; this guarantees that if we
	 * see overloaded we must also see the rto_mask bit.
	 */
	/* 与发布端 mask→wmb→count 配对，保证非零 count 后不会漏读对应 CPU 位。 */
	smp_rmb();

	/* If we are the only overloaded CPU do nothing */
	/* 唯一 overload CPU 正是自己时没有远端来源，pull 不可能成功。 */
	if (rt_overload_count == 1 &&
	    cpumask_test_cpu(this_rq->cpu, this_rq->rd->rto_mask))
		return;

#ifdef HAVE_RT_PUSH_IPI
	/* 大系统用 IPI 接力让源 CPU 自己 push，避免本 CPU 逐个争抢所有远端 rq 锁。 */
	if (sched_feat(RT_PUSH_IPI)) {
		tell_cpu_to_push(this_rq);
		return;
	}
#endif

	/* 非 IPI 模式逐个扫描已发布 overload CPU，继续寻找可能更高优先级的候选。 */
	for_each_cpu(cpu, this_rq->rd->rto_mask) {
		/* 本 rq 不是远端来源；自己的 pushable task 由 push 路径处理。 */
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);

		/*
		 * Don't bother taking the src_rq->lock if the next highest
		 * task is known to be lower-priority than our current task.
		 * This may look racy, but if this value is about to go
		 * logically higher, the src_rq will push this task away.
		 * And if its going logically lower, we do not care
		 */
		/* next 缓存不优于本 rq curr 时跳锁；缓存变高会由源 rq 主动 push 修正。 */
		if (src_rq->rt.highest_prio.next >=
		    this_rq->rt.highest_prio.curr)
			continue;

		/*
		 * We can potentially drop this_rq's lock in
		 * double_lock_balance, and another CPU could
		 * alter this_rq
		 */
		/* double_lock_balance() 可能短暂释放本 rq，后续所有条件都必须在双锁下重读。 */
		push_task = NULL;
		double_lock_balance(this_rq, src_rq);

		/*
		 * We can pull only a task, which is pushable
		 * on its rq, and no others.
		 */
		/* 在源锁下按目标 CPU affinity 选择最高 pushable task，过滤 current/on_cpu。 */
		p = pick_highest_pushable_task(src_rq, this_cpu);

		/*
		 * Do we have an RT task that preempts
		 * the to-be-scheduled task?
		 */
		/* 只有严格高于本 rq 当前最高优先级，拉取才会改善实时调度顺序。 */
		if (p && (p->prio < this_rq->rt.highest_prio.curr)) {
			WARN_ON(p == src_rq->curr);
			WARN_ON(!task_on_rq_queued(p));

			/*
			 * There's a chance that p is higher in priority
			 * than what's currently running on its CPU.
			 * This is just that p is waking up and hasn't
			 * had a chance to schedule. We only pull
			 * p if it is lower in priority than the
			 * current task on the run queue
			 */
			/* p 若高于源 donor，源 CPU 应先运行它；此时拉走会破坏本地严格优先级。 */
			if (p->prio < src_rq->donor->prio)
				goto skip;

			/* migration-disabled p 不能直接拉；改为请求 stopper 推走源 current，为 p 腾位。 */
			if (is_migration_disabled(p)) {
				push_task = get_push_task(src_rq);
			} else {
				/* 双锁下直接把 queued p 从源迁入本 rq，并在循环结束统一请求 resched。 */
				move_queued_task_locked(src_rq, this_rq, p);
				resched = true;
			}
			/*
			 * We continue with the search, just in
			 * case there's an even higher prio task
			 * in another runqueue. (low likelihood
			 * but possible)
			 */
			/* 不提前停止：另一个 overload rq 可能还有数值更小的更高优先级 task。 */
		}
skip:
		double_unlock_balance(this_rq, src_rq);

		if (push_task) {
			/* 提交 stopper 前释放本 rq 锁；preempt_disable 保持当前 CPU 与 rq 关联稳定。 */
			preempt_disable();
			raw_spin_rq_unlock(this_rq);
			stop_one_cpu_nowait(src_rq->cpu, push_cpu_stop,
					    push_task, &src_rq->push_work);
			preempt_enable();
			raw_spin_rq_lock(this_rq);
		}
	}

	/* 至少成功拉入一个能抢占的 task 时，通知当前 CPU 尽快进入新的最高优先级。 */
	if (resched)
		resched_curr(this_rq);
}

/*
 * If we are not running and we are not going to reschedule soon, we should
 * try to push tasks away now
 */
/* 唤醒后若本 rq 不会很快调度且可迁移，主动 push 防止高优先级任务滞留。 */
/*
 * 业务背景：RT task 唤醒后若暂不触发本地抢占，需主动把可迁移任务推向更空闲 CPU，避免 overload 长时间维持。
 * 入参：rq 是不可空输入/输出目标 runqueue；p 是不可空刚唤醒且已完成入队的 RT task。
 * 出参/返回：无直接返回值、无输出参数；满足条件时同步执行 push_rt_tasks()，可能迁走一个或多个 task。
 * 注意事项：调用者持 rq 锁且不可睡眠；条件只基于锁内快照，push 仍须在双 rq 锁下复核 affinity 与优先级。
 */
static void task_woken_rt(struct rq *rq, struct task_struct *p)
{
	bool need_to_push = !task_on_cpu(rq, p) &&
			    !test_tsk_need_resched(rq->curr) &&
			    p->nr_cpus_allowed > 1 &&
			    (dl_task(rq->donor) || rt_task(rq->donor)) &&
			    (rq->curr->nr_cpus_allowed < 2 ||
			     rq->donor->prio <= p->prio);

	/* task 未在 CPU、当前无待调度、可迁且 donor 为高调度类时，才值得立即执行 push。 */
	if (need_to_push)
		push_rt_tasks(rq);
}

/* Assumes rq->lock is held */
/* rq 锁下发布 overload/runtime/cpupri，使新在线 CPU 参与 RT 选择。 */
/*
 * 业务背景：CPU 上线后必须把本地 RT 负载重新发布进 root-domain 索引，其他 CPU 才能选择或拉取它。
 * 入参：rq 是不可空输入/输出上线 runqueue，CPU hotplug 已持有 rq 锁并稳定其 root_domain。
 * 出参/返回：无直接返回值、无输出参数；恢复 overload 位、组 runtime 状态和 cpupri 优先级。
 * 注意事项：不可睡眠；发布顺序须与 rq_offline_rt() 对称，错误 cpupri 会让迁移选择遗漏或选中离线 CPU。
 */
static void rq_online_rt(struct rq *rq)
{
	/* 本地 overloaded 标志跨 offline 保留，重新上线时才再次发布共享 mask/count。 */
	if (rq->rt.overloaded)
		rt_set_overload(rq);

	/* 撤销 offline 使用的 RUNTIME_INF 哨兵并清旧消费/节流。 */
	__enable_runtime(rq);

	cpupri_set(&rq->rd->cpupri, rq->cpu, rq->rt.highest_prio.curr);
}

/* Assumes rq->lock is held */
/* rq 锁下撤销 overload/cpupri 并归还 runtime，阻止选择下线 CPU。 */
/*
 * 业务背景：CPU 下线前需从所有 RT 迁移候选和共享带宽计算中摘除，避免新任务继续流入。
 * 入参：rq 是不可空输入/输出下线 runqueue，hotplug 上下文持有 rq 锁并阻止新的在线发布。
 * 出参/返回：无直接返回值、无输出参数；撤销 overload、回收借贷 runtime，并将 cpupri 置无效。
 * 注意事项：__disable_runtime() 会操作组带宽锁但不睡眠；任务迁出仍可能临时使用已解除 throttle 的本地队列。
 */
static void rq_offline_rt(struct rq *rq)
{
	/* 先让 pull 扫描者看不到该 CPU，再回收它与邻居之间的 runtime 借贷。 */
	if (rq->rt.overloaded)
		rt_clear_overload(rq);

	__disable_runtime(rq);

	/* 最后使选核索引拒绝该 CPU；已有无锁候选仍会在 rq 锁/hotplug 检查中失败。 */
	cpupri_set(&rq->rd->cpupri, rq->cpu, CPUPRI_INVALID);
}

/*
 * When switch from the rt queue, we bring ourselves to a position
 * that we might want to pull RT tasks from other runqueues.
 */
/* 最后一个 RT task 离开 class 后请求 pull，利用本 rq 新出现的承载空间。 */
/*
 * 业务背景：task 从 RT 类切出可能让本 CPU 突然具备承载远端高优先级任务的空间，需要安排一次 pull。
 * 入参：rq 是不可空输入/输出所属 runqueue；p 是不可空刚离开 RT class 的 task。
 * 出参/返回：无直接返回值、无输出参数；当 p 已离队且本 rq 无其他 RT task 时登记 pull callback。
 * 注意事项：调用者持 rq 锁；只登记异步平衡，不保证远端存在可迁 task，其他 RT task 存在时由正常调度处理。
 */
static void switched_from_rt(struct rq *rq, struct task_struct *p)
{
	/*
	 * If there are other RT tasks then we will reschedule
	 * and the scheduling of the other RT tasks will handle
	 * the balancing. But if we are the last RT task
	 * we may need to handle the pulling of RT tasks
	 * now.
	 */
	/* p 仍 queued 或本 rq 仍有 RT task 时，后续正常 pick/put_prev 会完成平衡。 */
	if (!task_on_rq_queued(p) || rq->rt.rt_nr_running)
		return;

	rt_queue_pull_task(rq);
}

/* 启动期为每个 possible CPU 分配选核临时 mask。 */
/*
 * 业务背景：find_lowest_rq() 需要无动态分配的 per-CPU cpumask 暂存 cpupri 候选，须在调度器初始化期建立。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；为每个 possible CPU 尝试分配 NUMA 本地 local_cpu_mask。
 * 注意事项：仅启动期调用、可使用 GFP_KERNEL；分配失败保留 NULL，由选核路径返回 -1 而非阻止启动。
 */
void __init init_sched_rt_class(void)
{
	unsigned int i;

	/* possible 而非 online，确保之后热插拔上线的 CPU 已有本地临时 mask。 */
	for_each_possible_cpu(i) {
		zalloc_cpumask_var_node(&per_cpu(local_cpu_mask, i),
					GFP_KERNEL, cpu_to_node(i));
	}
}

/*
 * When switching a task to RT, we may overload the runqueue
 * with RT tasks. In this case we try to push them off to
 * other runqueues.
 */
/* task 切入 RT 后更新 PELT；若已排队则按优先级触发 push 或 resched。 */
/*
 * 业务背景：策略/PI 变化把 task 切入 RT 类时，必须同步利用率记账并立即修正可能形成的 rq overload/抢占。
 * 入参：rq 是不可空输入/输出所属 runqueue；p 是不可空刚切入 rt_sched_class 的 task。
 * 出参/返回：无直接返回值、无输出参数；可能更新 RT PELT、登记 push callback 或请求本 CPU 重调度。
 * 注意事项：调用者持 rq 锁；current 与 queued 非 current 分支互斥，离线 rq 不触发 resched。
 */
static void switched_to_rt(struct rq *rq, struct task_struct *p)
{
	/*
	 * If we are running, update the avg_rt tracking, as the running time
	 * will now on be accounted into the latter.
	 */
	/* 正在运行的 task 从此刻起归入 RT PELT，更新基线后无需入队抢占处理。 */
	if (task_current(rq, p)) {
		update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 0);
		return;
	}

	/*
	 * If we are not running we may need to preempt the current
	 * running task. If that current running task is also an RT task
	 * then see if we can move to another run queue.
	 */
	/* 只有已排队 task 才会改变本 rq 候选；睡眠 task 等未来 wakeup 再处理。 */
	if (task_on_rq_queued(p)) {
		if (p->nr_cpus_allowed > 1 && rq->rt.overloaded)
			rt_queue_push_tasks(rq);
		if (p->prio < rq->donor->prio && cpu_online(cpu_of(rq)))
			resched_curr(rq);
	}
}

/*
 * Priority of the task has changed. This may cause
 * us to initiate a push or pull.
 */
/* RT priority 改变后，运行者降级触发 pull/重调度，等待者升级触发抢占。 */
/*
 * 业务背景：RT task 有效优先级因 sched_setscheduler 或 PI 改变后，原先的抢占与迁移决策可能不再成立。
 * 入参：rq 是不可空输入/输出所属 runqueue；p 是不可空已完成优先级字段更新的 RT task；oldprio 是旧内部优先级。
 * 出参/返回：无直接返回值、无输出参数；可能登记 pull callback或请求当前 CPU 重新调度。
 * 注意事项：调用者持 rq/pi 锁协议；数值越小优先级越高，未 queued 或优先级未变时无副作用。
 */
static void
prio_changed_rt(struct rq *rq, struct task_struct *p, u64 oldprio)
{
	/* 睡眠 task 不参与当前 rq 选择，优先级将在下次入队时自然生效。 */
	if (!task_on_rq_queued(p))
		return;

	if (p->prio == oldprio)
		return;

	/* 正在向 CPU 提供执行上下文的 donor 降级时，可能应让远端或本地更高任务接管。 */
	if (task_current_donor(rq, p)) {
		/*
		 * If our priority decreases while running, we
		 * may need to pull tasks to this runqueue.
		 */
		if (oldprio < p->prio)
			rt_queue_pull_task(rq);

		/*
		 * If there's a higher priority task waiting to run
		 * then reschedule.
		 */
		if (p->prio > rq->rt.highest_prio.curr)
			resched_curr(rq);
	} else {
		/* 等待 task 升到 donor 之上时立即 resched；降级不影响当前执行者。 */
		/*
		 * This task is not running, but if it is
		 * greater than the current running task
		 * then reschedule.
		 */
		if (p->prio < rq->donor->prio)
			resched_curr(rq);
	}
}

#ifdef CONFIG_POSIX_TIMERS
/* tick 检查 RLIMIT_RTTIME，超过软/硬门槛时交 POSIX CPU timer 发信号。 */
/*
 * 业务背景：防止用户 RT task 长期垄断 CPU，调度 tick 依据 RLIMIT_RTTIME 累计连续实时运行并触发信号处理。
 * 入参：rq 是不可空当前 runqueue；p 是不可空正在运行的 RT task，二者均为借用且由 rq 锁稳定。
 * 出参/返回：无直接返回值、无输出参数；可能增加 timeout 并调用 POSIX CPU timer watchdog 发出限制信号。
 * 注意事项：tick/远程 tick 上下文不可睡眠；rlimit 并发变化允许延迟到下一 tick 修正，INFINITY 时禁用检查。
 */
static void watchdog(struct rq *rq, struct task_struct *p)
{
	unsigned long soft, hard;

	/* max may change after cur was read, this will be fixed next tick */
	/* soft/max 分次读取可能混合新旧配置，但只影响本 tick，下一次会重新取值。 */
	soft = task_rlimit(p, RLIMIT_RTTIME);
	hard = task_rlimit_max(p, RLIMIT_RTTIME);

	/* 软限制无限表示用户明确关闭连续 RT 运行 watchdog。 */
	if (soft != RLIM_INFINITY) {
		unsigned long next;

		/* 同一 jiffy 可能收到本地与远程 tick，只允许 timeout 增加一次。 */
		if (p->rt.watchdog_stamp != jiffies) {
			p->rt.timeout++;
			p->rt.watchdog_stamp = jiffies;
		}

		/* 取软硬限制较小者并向上换算 tick，避免微秒边界被向下截断而过早触发。 */
		next = DIV_ROUND_UP(min(soft, hard), USEC_PER_SEC/HZ);
		if (p->rt.timeout > next) {
			posix_cputimers_rt_watchdog(&p->posix_cputimers,
						    p->se.sum_exec_runtime);
		}
	}
}
#else /* !CONFIG_POSIX_TIMERS: */
/* 无 POSIX_TIMERS 配置时 RT watchdog 为空操作。 */
/*
 * 业务背景：未编译 POSIX CPU timer 时保留统一 tick 调用点，而不提供 RLIMIT_RTTIME 信号机制。
 * 入参：rq/p 均为未使用借用输入，可为任意调用者已保证有效的值，函数不解引用。
 * 出参/返回：无直接返回值、无输出参数和副作用。
 * 注意事项：仅 !CONFIG_POSIX_TIMERS 下存在；不可据此推断 RT 带宽节流也被关闭。
 */
static inline void watchdog(struct rq *rq, struct task_struct *p) { }
#endif /* !CONFIG_POSIX_TIMERS */

/*
 * scheduler tick hitting a task of our scheduling class.
 *
 * NOTE: This function can be called remotely by the tick offload that
 * goes along full dynticks. Therefore no local assumption can be made
 * and everything must be accessed through the @rq and @curr passed in
 * parameters.
 */
/* tick 结算 runtime/PELT/watchdog；RR 时间片耗尽时移到同优先级队尾并 resched。 */
/*
 * 业务背景：RT class 的周期 tick 负责执行计费与限制；SCHED_RR 还需用时间片在同优先级任务间轮转。
 * 入参：rq 是不可空输入/输出当前 runqueue；p 是不可空正在运行的 RT task；queued 表示 tick 来源状态但本实现不读取。
 * 出参/返回：无直接返回值、无输出参数；更新 runtime/PELT/watchdog，RR 到期时重置时间片并可能重排、resched。
 * 注意事项：可由 full-dynticks 远程调用，不能依赖本地 CPU 隐式状态；调用者持 rq 锁，FIFO 不做时间片轮转。
 */
static void task_tick_rt(struct rq *rq, struct task_struct *p, int queued)
{
	struct sched_rt_entity *rt_se = &p->rt;

	/* 先结算本 tick 前的真实运行时间，保证带宽 throttle 与 RR 轮转使用同一时钟边界。 */
	update_curr_rt(rq);
	update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 1);

	/* RLIMIT_RTTIME 独立于组带宽：即使 bandwidth 无限，用户限制仍可发信号。 */
	watchdog(rq, p);

	/*
	 * RR tasks need a special form of time-slice management.
	 * FIFO tasks have no timeslices.
	 */
	/* FIFO 只因阻塞、yield 或更高优先级抢占而切出，不消耗 RR timeslice。 */
	if (p->policy != SCHED_RR)
		return;

	/* 非零表示仍有 tick 配额；减到零才进入轮转与重置阶段。 */
	if (--p->rt.time_slice)
		return;

	/* 无论是否存在同优先级竞争者都重置，下一轮运行从完整全局时间片开始。 */
	p->rt.time_slice = sched_rr_timeslice;

	/*
	 * Requeue to the end of queue if we (and all of our ancestors) are not
	 * the only element on the queue
	 */
	/* 任一级有同优先级兄弟就重排整条 task 层级；全部独占时继续运行避免无效切换。 */
	for_each_sched_rt_entity(rt_se) {
		if (rt_se->run_list.prev != rt_se->run_list.next) {
			requeue_task_rt(rq, p, 0);
			resched_curr(rq);
			return;
		}
	}
}

/* RR 返回全局 timeslice jiffies，FIFO 返回 0 表示无限。 */
/*
 * 业务背景：sched_rr_get_interval 等接口需把 RT 策略的调度量子暴露给调用者，FIFO 用零表达无固定量子。
 * 入参：rq 是未使用的所属 runqueue 借用输入；task 是不可空只读调度实体。
 * 出参/返回：SCHED_RR 返回全局 sched_rr_timeslice（jiffies），其他 RT 策略返回 0；无输出参数。
 * 注意事项：调用者负责把 jiffies 转 ABI 时间单位；返回是当前全局配置快照，之后 sysctl 可改变。
 */
static unsigned int get_rr_interval_rt(struct rq *rq, struct task_struct *task)
{
	/*
	 * Time slice is 0 for SCHED_FIFO tasks
	 */
	if (task->policy == SCHED_RR)
		return sched_rr_timeslice;
	else
		return 0;
}

#ifdef CONFIG_SCHED_CORE
/* core scheduling 查询 @p 在 @cpu 所属 rt_rq 是否真正 throttle。 */
/*
 * 业务背景：core scheduling 在兄弟线程共同选取 cookie 候选时必须排除因 RT bandwidth 已不可运行的 task。
 * 入参：p 是不可空只读 RT task；cpu 是有效候选 CPU 编号，二者均为借用输入。
 * 出参/返回：所属 rt_rq 被有效 throttle 返回非零，否则返回 0；无输出参数。
 * 注意事项：仅 CONFIG_SCHED_CORE 下存在；组调度配置决定队列定位，返回快照仍受 rq/runtime 锁协议约束。
 */
static int task_is_throttled_rt(struct task_struct *p, int cpu)
{
	struct rt_rq *rt_rq;

#ifdef CONFIG_RT_GROUP_SCHED // XXX maybe add task_rt_rq(), see also sched_rt_period_rt_rq
	rt_rq = task_group(p)->rt_rq[cpu];
	WARN_ON(!rt_group_sched_enabled() && rt_rq->tg != &root_task_group);
#else
	rt_rq = &cpu_rq(cpu)->rt;
#endif

	/* helper 同时考虑 PI boosted 例外，不能只读取裸 rt_throttled 标志。 */
	return rt_rq_throttled(rt_rq);
}
#endif /* CONFIG_SCHED_CORE */

DEFINE_SCHED_CLASS(rt) = {
	/* runnable 生命周期：入队/出队维护层级 priority array，yield 只重排同优先级顺序。 */
	.enqueue_task		= enqueue_task_rt,
	.dequeue_task		= dequeue_task_rt,
	.yield_task		= yield_task_rt,

	/* 唤醒阶段依据严格 RT 优先级决定抢占，并处理等优先级迁移不对称。 */
	.wakeup_preempt		= wakeup_preempt_rt,

	/* 核心选择从位图最高层下钻到 task，切入/切出回调结算统计与 pushable 状态。 */
	.pick_task		= pick_task_rt,
	.put_prev_task		= put_prev_task_rt,
	.set_next_task          = set_next_task_rt,

	/* SMP 平衡通过 cpupri 选核，并在 online/offline 与 wake/class 切换时维护派生索引。 */
	.balance		= balance_rt,
	.select_task_rq		= select_task_rq_rt,
	.set_cpus_allowed       = set_cpus_allowed_common,
	.rq_online              = rq_online_rt,
	.rq_offline             = rq_offline_rt,
	.task_woken		= task_woken_rt,
	.switched_from		= switched_from_rt,
	.find_lock_rq		= find_lock_lowest_rq,

	/* tick 同时承担带宽、watchdog 与 RR 时间片，查询接口向外暴露 RR 量子。 */
	.task_tick		= task_tick_rt,

	.get_rr_interval	= get_rr_interval_rt,

	/* 策略/优先级变化重新评估抢占与 push/pull，update_curr 供调度核心主动结算。 */
	.switched_to		= switched_to_rt,
	.prio_changed		= prio_changed_rt,

	.update_curr		= update_curr_rt,

#ifdef CONFIG_SCHED_CORE
	/* core scheduler 选择 cookie 同伴前排除被带宽真正 throttle 的 RT task。 */
	.task_is_throttled	= task_is_throttled_rt,
#endif

#ifdef CONFIG_UCLAMP_TASK
	/* 标记本调度类参与 uclamp，选核路径会检查异构 CPU 的有效容量。 */
	.uclamp_enabled		= 1,
#endif
};

#ifdef CONFIG_RT_GROUP_SCHED
/*
 * Ensure that the real time constraints are schedulable.
 */
/* 串行全局及各 task-group RT 带宽 admission/提交，保护跨整棵层级的比例不变量。 */
static DEFINE_MUTEX(rt_constraints_mutex);

/* 遍历 @tg tasks 判断是否已有 RT task；autogroup 按设计恒无 RT task。 */
/*
 * 业务背景：把 task group runtime 从非零改为零会饿死现有 RT task，admission 前需检查组成员策略。
 * 入参：tg 是不可空只读 task group，调用者持 constraints mutex/RCU 所需的层级稳定条件。
 * 出参/返回：发现至少一个 RT task 返回非零，否则返回 0；无输出参数。
 * 注意事项：仅 CONFIG_RT_GROUP_SCHED 下存在；CSS 迭代可能较慢，autogroup 按创建约束直接返回 0。
 */
static inline int tg_has_rt_tasks(struct task_group *tg)
{
	struct task_struct *task;
	struct css_task_iter it;
	int ret = 0;

	/*
	 * Autogroups do not have RT tasks; see autogroup_create().
	 */
	/* autogroup_create() 不允许 RT 成员，跳过 CSS 遍历并保持该设计契约。 */
	if (task_group_is_autogroup(tg))
		return 0;

	/* 迭代器稳定成员生命周期；找到首个 RT task 后用 ret 条件提前终止。 */
	css_task_iter_start(&tg->css, 0, &it);
	while (!ret && (task = css_task_iter_next(&it)))
		ret |= rt_task(task);
	css_task_iter_end(&it);

	return ret;
}

/* 带宽可调候选值，walk_tg_tree 用它临时替代目标组当前 period/runtime。 */
/*
 * 校验快照只覆盖正在修改的 tg：遍历其他节点时仍读取已发布 bandwidth。
 * period/runtime 均为纳秒；结构体只在 constraints mutex 与 RCU 遍历期间借用，不拥有 tg。
 */
struct rt_schedulable_data {
	struct task_group *tg;
	u64 rt_period;
	u64 rt_runtime;
};

/* 校验单组 runtime<=period、全局上限及子组 ratio 总和不超过父组。 */
/*
 * 业务背景：分层 RT bandwidth 必须保证每个节点及其直接子组总额度都可由父额度承载，避免不可调度配置。
 * 入参：tg 是不可空当前遍历节点；data 是不可空 rt_schedulable_data 候选快照，均为只读借用。
 * 出参/返回：通过返回 0；额度越界返回 -EINVAL；把有 RT task 的现有非零组改成零返回 -EBUSY；无输出参数。
 * 注意事项：在 RCU task-group 遍历和 constraints mutex 下调用；RUNTIME_INF 经 to_ratio() 按全额语义处理。
 */
static int tg_rt_schedulable(struct task_group *tg, void *data)
{
	struct rt_schedulable_data *d = data;
	struct task_group *child;
	u64 total, sum = 0;
	u64 period, runtime;

	/* 默认读取当前已发布配置；命中目标组时用尚未提交的候选替换。 */
	period = ktime_to_ns(tg->rt_bandwidth.rt_period);
	runtime = tg->rt_bandwidth.rt_runtime;

	if (tg == d->tg) {
		period = d->rt_period;
		runtime = d->rt_runtime;
	}

	/*
	 * Cannot have more runtime than the period.
	 */
	/* 有限 runtime 超过 period 表示单组利用率大于 100%，直接拒绝。 */
	if (runtime > period && runtime != RUNTIME_INF)
		return -EINVAL;

	/*
	 * Ensure we don't starve existing RT tasks if runtime turns zero.
	 */
	/* 仅阻止从非零变零且已有 RT task；原本零额度组保持零不新增兼容性限制。 */
	if (rt_bandwidth_enabled() && !runtime &&
	    tg->rt_bandwidth.rt_runtime && tg_has_rt_tasks(tg))
		return -EBUSY;

	/* 固定点 ratio 统一比较不同 period 的带宽比例，而不是直接比较纳秒数。 */
	total = to_ratio(period, runtime);

	/*
	 * Nobody can have more than the global setting allows.
	 */
	/* 任一组比例都不能超过系统为 RT/DL 保留的全局上限。 */
	if (total > to_ratio(global_rt_period(), global_rt_runtime()))
		return -EINVAL;

	/*
	 * The sum of our children's runtime should not exceed our own.
	 */
	/* 只累加直接子组；walk_tg_tree 会在每个子组节点再次校验其下一层。 */
	list_for_each_entry_rcu(child, &tg->children, siblings) {
		period = ktime_to_ns(child->rt_bandwidth.rt_period);
		runtime = child->rt_bandwidth.rt_runtime;

		if (child == d->tg) {
			period = d->rt_period;
			runtime = d->rt_runtime;
		}

		/* 若当前修改目标是这个 child，父节点校验也必须使用同一候选快照。 */
		sum += to_ratio(period, runtime);
	}

	if (sum > total)
		return -EINVAL;

	return 0;
}

/* RCU 遍历整棵 task_group 树模拟候选配置；任一层违规返回负 errno。 */
/*
 * 业务背景：提交单组新配额前必须用同一候选检查整棵层级，因为它同时改变祖先的子组总和约束。
 * 入参：tg 是待修改组借用指针；period/runtime 是候选纳秒值，runtime 可为 RUNTIME_INF。
 * 出参/返回：全树可调度返回 0，否则返回首个校验回调的负 errno；无输出参数且不提交配置。
 * 注意事项：调用者持 rt_constraints_mutex；函数内部 RCU 只保证遍历生命周期，候选 data 位于当前栈上。
 */
static int __rt_schedulable(struct task_group *tg, u64 period, u64 runtime)
{
	int ret;

	struct rt_schedulable_data data = {
		.tg = tg,
		.rt_period = period,
		.rt_runtime = runtime,
	};

	/* walk_tg_tree 对每个节点执行 tg_rt_schedulable，任一失败即停止并传播 errno。 */
	rcu_read_lock();
	ret = walk_tg_tree(tg_rt_schedulable, tg_nop, &data);
	rcu_read_unlock();

	return ret;
}

/* 在 constraints mutex 下先全树 admission，再原子发布组及逐 CPU runtime。 */
/*
 * 业务背景：cgroup RT period/runtime 更新必须先证明层级可调度，再同步发布共享配置和所有 per-CPU 队列额度。
 * 入参：tg 是不可空输入/输出 task group；rt_period/rt_runtime 是纳秒候选值，runtime 可为 RUNTIME_INF。
 * 出参/返回：成功返回 0并发布配置；非法根零额度、零周期、溢出或层级违规返回负 errno；无输出参数。
 * 注意事项：可睡眠并持 constraints mutex；发布阶段用 bandwidth irq-safe 锁包住所有本地 runtime 锁，调用者不得持 rq 锁。
 */
static int tg_set_rt_bandwidth(struct task_group *tg,
		u64 rt_period, u64 rt_runtime)
{
	int i, err = 0;

	/*
	 * Disallowing the root group RT runtime is BAD, it would disallow the
	 * kernel creating (and or operating) RT threads.
	 */
	/* root 零额度会阻止迁移线程、watchdog 等内核 RT 线程运行，属于系统级死锁风险。 */
	if (tg == &root_task_group && rt_runtime == 0)
		return -EINVAL;

	/* No period doesn't make any sense. */
	/* 零周期无法定义补充时点，也会使 ratio 计算除零。 */
	if (rt_period == 0)
		return -EINVAL;

	/*
	 * Bound quota to defend quota against overflow during bandwidth shift.
	 */
	/* 有限额度先限制在定点带宽表示范围内；无限哨兵不参与该数值比较。 */
	if (rt_runtime != RUNTIME_INF && rt_runtime > max_rt_runtime)
		return -EINVAL;

	/* mutex 串行所有组/全局配置，保证 admission 看到稳定树并与提交不可交错。 */
	mutex_lock(&rt_constraints_mutex);
	err = __rt_schedulable(tg, rt_period, rt_runtime);
	if (err)
		goto unlock;

	/* admission 成功后进入不可失败提交区：先发布共享 period/runtime，再更新每 CPU 镜像。 */
	raw_spin_lock_irq(&tg->rt_bandwidth.rt_runtime_lock);
	tg->rt_bandwidth.rt_period = ns_to_ktime(rt_period);
	tg->rt_bandwidth.rt_runtime = rt_runtime;

	/* possible CPU 全覆盖，使未来 hotplug 上线也继承新额度，而非离线时的旧配置。 */
	for_each_possible_cpu(i) {
		struct rt_rq *rt_rq = tg->rt_rq[i];

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		rt_rq->rt_runtime = rt_runtime;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
	}
	raw_spin_unlock_irq(&tg->rt_bandwidth.rt_runtime_lock);
unlock:
	/* admission 失败没有写入任何配置；成功与失败都从同一出口释放全局串行锁。 */
	mutex_unlock(&rt_constraints_mutex);

	return err;
}

/* 将用户微秒 runtime 转纳秒，-1 表示无限，溢出返回 -EINVAL。 */
/*
 * 业务背景：cgroup 接口以微秒接收 RT runtime，需要安全转换后复用统一 admission/提交事务。
 * 入参：tg 是不可空输入/输出 task group；rt_runtime_us 是微秒额度，负值表示无限。
 * 出参/返回：返回 tg_set_rt_bandwidth() 的 0或负 errno；转换溢出返回 -EINVAL；无输出参数。
 * 注意事项：可睡眠；除负值哨兵外不截断超范围输入，period 保持当前已发布值。
 */
int sched_group_set_rt_runtime(struct task_group *tg, long rt_runtime_us)
{
	u64 rt_runtime, rt_period;

	/* 先保留当前 period，只有 runtime 作为本次候选发生变化。 */
	rt_period = ktime_to_ns(tg->rt_bandwidth.rt_period);
	rt_runtime = (u64)rt_runtime_us * NSEC_PER_USEC;
	if (rt_runtime_us < 0)
		rt_runtime = RUNTIME_INF;
	else if ((u64)rt_runtime_us > U64_MAX / NSEC_PER_USEC)
		return -EINVAL;

	return tg_set_rt_bandwidth(tg, rt_period, rt_runtime);
}

/* 返回组 runtime 微秒；无限额度映射为 -1。 */
/*
 * 业务背景：cgroup 读取接口需把内部纳秒 runtime 还原为用户可见微秒，并保留无限额度哨兵。
 * 入参：tg 是不可空只读 task group 借用指针。
 * 出参/返回：有限额度返回截断到微秒的非负 long，RUNTIME_INF 返回 -1；无输出参数。
 * 注意事项：无锁读取是配置快照；极端大值向 long 的转换受平台宽度约束，配置上限已在写路径限制。
 */
long sched_group_rt_runtime(struct task_group *tg)
{
	u64 rt_runtime_us;

	if (tg->rt_bandwidth.rt_runtime == RUNTIME_INF)
		return -1;

	/* do_div 原地除以每微秒纳秒数，舍弃不足一微秒的内部余数。 */
	rt_runtime_us = tg->rt_bandwidth.rt_runtime;
	do_div(rt_runtime_us, NSEC_PER_USEC);
	return rt_runtime_us;
}

/* 将微秒 period 安全转纳秒并复用带宽 admission/发布路径。 */
/*
 * 业务背景：cgroup period 写接口需改变补充窗口而保持当前 runtime，并重新验证所有层级比例。
 * 入参：tg 是不可空输入/输出 task group；rt_period_us 是候选周期微秒数。
 * 出参/返回：成功返回 0；乘法溢出或 admission 失败返回负 errno；无输出参数。
 * 注意事项：可睡眠；零值虽不溢出但会由 tg_set_rt_bandwidth() 明确拒绝。
 */
int sched_group_set_rt_period(struct task_group *tg, u64 rt_period_us)
{
	u64 rt_runtime, rt_period;

	if (rt_period_us > U64_MAX / NSEC_PER_USEC)
		return -EINVAL;

	rt_period = rt_period_us * NSEC_PER_USEC;
	rt_runtime = tg->rt_bandwidth.rt_runtime;

	return tg_set_rt_bandwidth(tg, rt_period, rt_runtime);
}

/* 返回组 period 的微秒表示。 */
/*
 * 业务背景：cgroup 读取接口把内部 ktime 周期转换为用户 ABI 使用的微秒标量。
 * 入参：tg 是不可空只读 task group 借用指针。
 * 出参/返回：返回当前 period 的微秒值；无输出参数。
 * 注意事项：无锁快照可能与并发配置交错；纳秒不能整除微秒时向下截断。
 */
long sched_group_rt_period(struct task_group *tg)
{
	u64 rt_period_us;

	rt_period_us = ktime_to_ns(tg->rt_bandwidth.rt_period);
	do_div(rt_period_us, NSEC_PER_USEC);
	return rt_period_us;
}

/* 组调度开启时拒绝把 RT task 挂到 runtime 为零的组；允许返回 1。 */
/*
 * 业务背景：cgroup attach 前必须保证目标组能为 RT task 提供运行额度，避免迁入后永久不可调度。
 * 入参：tg 是不可空目标 task group；tsk 是不可空待迁移 task，均为只读借用。
 * 出参/返回：允许 attach 返回 1；组调度启用、task 为 RT 且目标 runtime 为零时返回 0；无输出参数。
 * 注意事项：这是 admission 快照，真正 attach 由 cgroup 锁协议串行；非 RT task 和运行时关闭组调度均放行。
 */
int sched_rt_can_attach(struct task_group *tg, struct task_struct *tsk)
{
	/* Don't accept real-time tasks when there is no way for them to run */
	/* 零 runtime 组中的 RT task 即便 runnable 也无法获得周期补充，故在所有权迁移前拒绝。 */
	if (rt_group_sched_enabled() && rt_task(tsk) && tg->rt_bandwidth.rt_runtime == 0)
		return 0;

	return 1;
}

#endif /* !CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_SYSCTL
/* 校验全局 runtime/period、最大可表示额度及整棵组层级 admission。 */
/*
 * 业务背景：全局 RT sysctl 改变所有 task group 的上界，提交前必须验证数值范围及现有层级仍可调度。
 * 入参：无，隐式读取 sysctl_sched_rt_period/runtime 候选全局值。
 * 出参/返回：有效返回 0，runtime>period、定点上限溢出或层级违规返回负 errno；无输出参数。
 * 注意事项：由 sysctl handler 在配置串行锁下调用；组调度运行时关闭则跳过层级树校验。
 */
static int sched_rt_global_validate(void)
{
	/* -1 无限哨兵跳过有限值比较；其他值必须不超过 period 与定点表示上限。 */
	if ((sysctl_sched_rt_runtime != RUNTIME_INF) &&
		((sysctl_sched_rt_runtime > sysctl_sched_rt_period) ||
		 ((u64)sysctl_sched_rt_runtime *
			NSEC_PER_USEC > max_rt_runtime)))
		return -EINVAL;

#ifdef CONFIG_RT_GROUP_SCHED
	/* 运行时未启用组层级时不存在子组比例约束，只保留全局数值检查。 */
	if (!rt_group_sched_enabled())
		return 0;

	/* scoped_guard 保证正常/错误返回都释放 mutex，并在锁内遍历当前 task-group 树。 */
	scoped_guard(mutex, &rt_constraints_mutex)
		return __rt_schedulable(NULL, 0, 0);
#endif
	return 0;
}

/* 串行 sysctl 写，联合验证 RT/DL 后提交；失败恢复旧值并重建 domains。 */
/*
 * 业务背景：全局 RT period/runtime 与 DEADLINE 带宽共享约束，sysctl 写必须作为联合事务校验并更新调度域。
 * 入参：table 描述当前 sysctl 项；write 表示读或写；buffer/lenp/ppos 是 procfs 用户缓冲区、长度和位置输入输出参数。
 * 出参/返回：成功返回 0，解析或 admission 失败返回负 errno；失败恢复两个全局 RT 值，lenp/ppos 按 proc handler 语义更新。
 * 注意事项：可睡眠并依次持本地 mutex、sched_domains_mutex；rebuild_sched_domains() 在解锁后执行，即使读或失败也会调用。
 */
static int sched_rt_handler(const struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos)
{
	int old_period, old_runtime;
	static DEFINE_MUTEX(mutex);
	int ret;

	/* 本地 mutex 串行两个共享 handler 项，domains mutex 再串行 RT/DL 与拓扑派生数据。 */
	mutex_lock(&mutex);
	sched_domains_mutex_lock();
	old_period = sysctl_sched_rt_period;
	old_runtime = sysctl_sched_rt_runtime;

	/* 通用 proc helper 先读写候选整数并执行表中 min/max；旧值已保存供联合校验回滚。 */
	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	/* 只有成功写入才做 RT 层级和 DL 联合 admission；纯读不改变配置。 */
	if (!ret && write) {
		ret = sched_rt_global_validate();
		if (ret)
			goto undo;

		ret = sched_dl_global_validate();
		if (ret)
			goto undo;

		/* 两项校验均通过后提交 DEADLINE 派生全局值；RT 直接使用已写 sysctl 标量。 */
		sched_dl_do_global();
	}
	if (0) {
undo:
	/* 任一 admission 失败都恢复 period/runtime 成对旧值，避免留下半事务配置。 */
		sysctl_sched_rt_period = old_period;
		sysctl_sched_rt_runtime = old_runtime;
	}
	sched_domains_mutex_unlock();
	mutex_unlock(&mutex);

	/*
	 * After changing maximum available bandwidth for DEADLINE, we need to
	 * recompute per root domain and per cpus variables accordingly.
	 */
	/* 在所有配置锁外重建，避免重建路径获取同一 mutex 造成锁递归。 */
	rebuild_sched_domains();

	return ret;
}

/* 读写 RR 毫秒 timeslice；零/负写恢复默认，并将内核值保持为 jiffies。 */
/*
 * 业务背景：RR sysctl 使用毫秒便于用户配置，而调度 tick 必须使用 jiffies，需要在写入时同步两个表示。
 * 入参：table/write/buffer/lenp/ppos 是标准 proc handler 输入输出参数，buffer 可由用户写入或读取。
 * 出参/返回：返回 proc_dointvec() 的 0或负 errno；成功写更新 sched_rr_timeslice，并可能把非正用户值规范为默认毫秒。
 * 注意事项：可睡眠并由静态 mutex 串行；毫秒转 jiffies 受 HZ 舍入，零不是零时间片而是恢复默认。
 */
static int sched_rr_handler(const struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos)
{
	int ret;
	static DEFINE_MUTEX(mutex);

	/* 串行用户毫秒视图与内部 jiffies 值，读者不会观察到同一次写的半转换状态。 */
	mutex_lock(&mutex);
	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	/*
	 * Make sure that internally we keep jiffies.
	 * Also, writing zero resets the time-slice to default:
	 */
	/* 解析成功的写才转换；非正值选择编译期默认，正值按当前 HZ 向 jiffies 舍入。 */
	if (!ret && write) {
		sched_rr_timeslice =
			sysctl_sched_rr_timeslice <= 0 ? RR_TIMESLICE :
			msecs_to_jiffies(sysctl_sched_rr_timeslice);

		/* 同时把用户可见变量规范为默认毫秒，后续读取不会继续显示零/负哨兵。 */
		if (sysctl_sched_rr_timeslice <= 0)
			sysctl_sched_rr_timeslice = jiffies_to_msecs(RR_TIMESLICE);
	}
	mutex_unlock(&mutex);

	return ret;
}
#endif /* CONFIG_SYSCTL */

/* RCU 下遍历 @cpu 的根及组 rt_rq 并输出诊断快照。 */
/*
 * 业务背景：sched_debug 等诊断接口需要按 CPU 展示根及所有 task group 的 RT 队列计数、优先级和带宽状态。
 * 入参：m 是不可空 seq_file 输出对象；cpu 是有效 possible CPU 编号。
 * 出参/返回：无直接返回值；向 m 追加每个 rt_rq 的文本快照，不转移任何对象 ownership。
 * 注意事项：可睡眠性由 seq_file 调用上下文决定；RCU 只保 task group 生命周期，打印值是非原子诊断快照。
 */
void print_rt_stats(struct seq_file *m, int cpu)
{
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	/* 遍历期间 group 不能释放；各队列字段未统一加 rq 锁，允许展示近似时刻。 */
	rcu_read_lock();
	for_each_rt_rq(rt_rq, iter, cpu_rq(cpu))
		print_rt_rq(m, cpu, rt_rq);
	rcu_read_unlock();
}
