// SPDX-License-Identifier: GPL-2.0-only
/*
 *  kernel/sched/cpupri.c
 *
 *  CPU priority management
 *
 *  Copyright (C) 2007-2008 Novell
 *
 *  Author: Gregory Haskins <ghaskins@novell.com>
 *
 *  This code tracks the priority of each CPU so that global migration
 *  decisions are easy to calculate.  Each CPU can be in a state as follows:
 *
 *                 (INVALID), NORMAL, RT1, ... RT99, HIGHER
 *
 *  going from the lowest priority to the highest.  CPUs in the INVALID state
 *  are not eligible for routing.  The system maintains this state with
 *  a 2 dimensional bitmap (the first for priority class, the second for CPUs
 *  in that class).  Therefore a typical application without affinity
 *  restrictions can find a suitable CPU with O(1) complexity (e.g. two bit
 *  searches).  For tasks with affinity restrictions, the algorithm has a
 *  worst case complexity of O(min(101, nr_domcpus)), though the scenario that
 *  yields the worst case search is fairly contrived.
 */
/*
 * 本文件维护 root_domain 内按“CPU 当前最高实时优先级”分桶的无锁候选索引。
 * INVALID CPU 不参与路由，NORMAL、RT1..RT99、HIGHER 从低到高排列。每桶用原子
 * count 作快速提示、cpumask 保存成员；无亲和性限制通常两次位搜索即可找到低优先级
 * CPU，受限任务最坏扫描 101 个桶。查询只是可能过期的提示，最终迁移仍由 rq 锁复核。
 */
#include "sched.h"

/*
 * p->rt_priority   p->prio   newpri   cpupri
 *
 *				  -1       -1 (CPUPRI_INVALID)
 *
 *				  99        0 (CPUPRI_NORMAL)
 *
 *		1        98       98        1
 *	      ...
 *	       49        50       50       49
 *	       50        49       49       50
 *	      ...
 *	       99         0        0       99
 *
 *				 100	  100 (CPUPRI_HIGHER)
 */
/*
 * 上表给出 task/运行队列内部 prio 到 cpupri 桶的反向编码：普通调度 prio 99 映射
 * NORMAL 0，RT 内部 prio 越小越紧急，映射后桶号越大；MAX_RT_PRIO 映射 HIGHER，
 * -1 保持 INVALID。这样从桶 0 向上扫描即可先找到比任务优先级低的 CPU。
 */
/*
 * 把 @prio（CPUPRI_INVALID 或调度器内部 0..MAX_RT_PRIO）转换为桶号。所有合法输入
 * 返回 [-1,100]；switch 无 default，调用者必须遵守范围，后续 BUG_ON 只检查过大值。
 * 函数纯计算、无锁且不能睡眠。
 */
static int convert_prio(int prio)
{
	/* cpupri 接收每个合法 case 的转换结果。 */
	int cpupri;

	switch (prio) {
	case CPUPRI_INVALID:
		cpupri = CPUPRI_INVALID;	/* -1 */
		break;

	case 0 ... 98:
		cpupri = MAX_RT_PRIO-1 - prio;	/* 1 ... 99 */
		break;

	case MAX_RT_PRIO-1:
		cpupri = CPUPRI_NORMAL;		/*  0 */
		break;

	case MAX_RT_PRIO:
		cpupri = CPUPRI_HIGHER;		/* 100 */
		break;
	}

	return cpupri;
}

/*
 * 检查单个优先级桶 @idx 是否含 @p 可用 CPU。@cp/@p 只借用；@lowest_mask 可为 NULL，
 * 非 NULL 时写入任务亲和性、桶 mask 和 cpu_active_mask 的交集。先读 count、rmb、
 * 再读 mask，与 set 的发布次序配合；视图仍可能竞态，但空计数只会漏掉离队触发 pull
 * 的瞬时 CPU，陈旧非空只多做工作。返回 1 表示找到非空候选，0 表示跳过/交集为空。
 * 函数不取锁、不分配且不能睡眠。
 */
static inline int __cpupri_find(struct cpupri *cp, struct task_struct *p,
				struct cpumask *lowest_mask, int idx)
{
	/* vec 是当前桶；skip 快照 count==0，屏障后才决定是否读取 mask。 */
	struct cpupri_vec *vec  = &cp->pri_to_cpu[idx];
	int skip = 0;

	if (!atomic_read(&(vec)->count))
		skip = 1;
	/*
	 * When looking at the vector, we need to read the counter,
	 * do a memory barrier, then read the mask.
	 *
	 * Note: This is still all racy, but we can deal with it.
	 *  Ideally, we only want to look at masks that are set.
	 *
	 *  If a mask is not set, then the only thing wrong is that we
	 *  did a little more work than necessary.
	 *
	 *  If we read a zero count but the mask is set, because of the
	 *  memory barriers, that can only happen when the highest prio
	 *  task for a run queue has left the run queue, in which case,
	 *  it will be followed by a pull. If the task we are processing
	 *  fails to find a proper place to go, that pull request will
	 *  pull this task if the run queue is running at a lower
	 *  priority.
	 */
	/*
	 * 查询必须先读 count、执行读屏障、再读 mask。视图仍有竞态：mask 已空只会
	 * 多做一次求交；count 为零而 mask 尚置位只能出现在最高优先级实体离队，随后
	 * 的 pull 会弥补本次漏选，必要时把当前任务拉到较低优先级 rq。
	 */
	smp_rmb();

	/* Need to do the rmb for every iteration */
	/* 每个桶都必须独立执行屏障；空计数在屏障后才可快速跳过。 */
	if (skip)
		return 0;

	if (cpumask_any_and(&p->cpus_mask, vec->mask) >= nr_cpu_ids)
		/* 不需要输出 mask 时也先确认亲和性交集存在。 */
		return 0;

	if (lowest_mask) {
		cpumask_and(lowest_mask, &p->cpus_mask, vec->mask);
		cpumask_and(lowest_mask, lowest_mask, cpu_active_mask);

		/*
		 * We have to ensure that we have at least one bit
		 * still set in the array, since the map could have
		 * been concurrently emptied between the first and
		 * second reads of vec->mask.  If we hit this
		 * condition, simply act as though we never hit this
		 * priority level and continue on.
		 */
		/*
		 * 两次读取 vec->mask 之间桶可能被并发清空；若叠加 active mask 后为空，
		 * 就把本桶当作从未命中并继续扫描，不能返回一个空“成功”集合。
		 */
		if (cpumask_empty(lowest_mask))
			return 0;
	}

	return 1;
}

/*
 * 不带容量 fitness 的候选查询 wrapper。参数/返回/竞态语义完全委托
 * cpupri_find_fitness(..., NULL)：返回 1 表示找到瞬时候选，0 表示没有；不睡眠。
 */
int cpupri_find(struct cpupri *cp, struct task_struct *p,
		struct cpumask *lowest_mask)
{
	return cpupri_find_fitness(cp, p, lowest_mask, NULL);
}

/**
 * cpupri_find_fitness - find the best (lowest-pri) CPU in the system
 * @cp: The cpupri context
 * @p: The task
 * @lowest_mask: A mask to fill in with selected CPUs (or NULL)
 * @fitness_fn: A pointer to a function to do custom checks whether the CPU
 *              fits a specific criteria so that we only return those CPUs.
 *
 * Note: This function returns the recommended CPUs as calculated during the
 * current invocation.  By the time the call returns, the CPUs may have in
 * fact changed priorities any number of times.  While not ideal, it is not
 * an issue of correctness since the normal rebalancer logic will correct
 * any discrepancies created by racing against the uncertainty of the current
 * priority configuration.
 *
 * Return: (int)bool - CPUs were found
 */
/*
 * cpupri_find_fitness() - 寻找最低优先级且满足额外条件的候选 CPU
 *
 * @cp: 借用的 root_domain 索引，生命周期由调用者 rq/RCU 协议保证。
 * @p: 不可为 NULL 的 RT task，仅读 prio、亲和性和 fitness 所需字段。
 * @lowest_mask: 可为 NULL 的输出；非 NULL 时承接当前最低桶的 active 候选。
 * @fitness_fn: 可为 NULL 的同步只读回调，检查 task 是否适配某 CPU，不得睡眠或取走引用。
 *
 * 从最低桶扫描到任务桶之前，只选择能被该任务抢占的 CPU；命中后可按容量逐个过滤。
 * 若所有低优先级候选都不适配，则递归关闭 fitness 重扫，优先保证高优先级 RT task
 * 能运行。返回 1/0 而非 CPU 编号。索引无锁且可过期，rebalancer 与目标 rq 复核纠偏。
 */
int cpupri_find_fitness(struct cpupri *cp, struct task_struct *p,
		struct cpumask *lowest_mask,
		bool (*fitness_fn)(struct task_struct *p, int cpu))
{
	/* task_pri 是任务对应桶上界；idx/cpu 分别扫描桶和候选 CPU。 */
	int task_pri = convert_prio(p->prio);
	int idx, cpu;

	WARN_ON_ONCE(task_pri >= CPUPRI_NR_PRIORITIES);

	for (idx = 0; idx < task_pri; idx++) {

		/* 空桶或与亲和性/active mask 无交集时继续更高一级。 */
		if (!__cpupri_find(cp, p, lowest_mask, idx))
			continue;

		if (!lowest_mask || !fitness_fn)
			/* 不需要输出集合或没有额外条件时，存在性已经足够。 */
			return 1;

		/* Ensure the capacity of the CPUs fit the task */
		/* 原文：确保候选 CPU 的容量适合该任务；不适配者原地清除。 */
		for_each_cpu(cpu, lowest_mask) {
			if (!fitness_fn(p, cpu))
				cpumask_clear_cpu(cpu, lowest_mask);
		}

		/*
		 * If no CPU at the current priority can fit the task
		 * continue looking
		 */
		/* 当前桶全被过滤时继续扫描更高但仍低于 task 的优先级桶。 */
		if (cpumask_empty(lowest_mask))
			continue;

		return 1;
	}

	/*
	 * If we failed to find a fitting lowest_mask, kick off a new search
	 * but without taking into account any fitness criteria this time.
	 *
	 * This rule favours honouring priority over fitting the task in the
	 * correct CPU (Capacity Awareness being the only user now).
	 * The idea is that if a higher priority task can run, then it should
	 * run even if this ends up being on unfitting CPU.
	 *
	 * The cost of this trade-off is not entirely clear and will probably
	 * be good for some workloads and bad for others.
	 *
	 * The main idea here is that if some CPUs were over-committed, we try
	 * to spread which is what the scheduler traditionally did. Sys admins
	 * must do proper RT planning to avoid overloading the system if they
	 * really care.
	 */
	/*
	 * 若 fitness 扫描完全失败，再忽略容量条件重搜。该策略优先兑现 RT 优先级，
	 * 即使最终 CPU 容量不足；它也把过载扩散到多个 CPU。代价依工作负载而异，严格
	 * RT 系统仍需管理员做容量规划，不能依赖此退化路径避免过载。
	 */
	if (fitness_fn)
		/* NULL fitness 保证递归至多一层，不会无限循环。 */
		return cpupri_find(cp, p, lowest_mask);

	return 0;
}

/**
 * cpupri_set - update the CPU priority setting
 * @cp: The cpupri context
 * @cpu: The target CPU
 * @newpri: The priority (INVALID,NORMAL,RT1-RT99,HIGHER) to assign to this CPU
 *
 * Note: Assumes cpu_rq(cpu)->lock is locked
 *
 * Returns: (void)
 */
/*
 * cpupri_set() - 发布 CPU 当前顶层 RT rq 的最高优先级
 *
 * @cp: 借用且可写的 root_domain 索引。
 * @cpu: possible CPU 编号；调用者持有 cpu_rq(cpu)->lock，串行同 CPU 更新。
 * @newpri: CPUPRI_INVALID 或内部调度 prio 0..MAX_RT_PRIO，函数先转桶号。
 *
 * 相同桶快速返回；否则先把 CPU 加入新桶并发布 count，再从旧桶撤销 count/mask，
 * 最后更新反向表。顺序保证提升 rq 优先级时无锁扫描至少能在新旧桶之一看到 CPU；
 * 降低优先级的短暂偏差由 RT pull 修正。无返回/分配且不能睡眠；非法上界 BUG。
 */
void cpupri_set(struct cpupri *cp, int cpu, int newpri)
{
	/* currpri 指向按 CPU 反向桶号；oldpri 是 rq 锁下稳定旧值。 */
	int *currpri = &cp->cpu_to_pri[cpu];
	int oldpri = *currpri;
	/* do_mb 记录本次是否已发布新桶，以决定跨新旧原子计数的排序。 */
	int do_mb = 0;

	newpri = convert_prio(newpri);

	BUG_ON(newpri >= CPUPRI_NR_PRIORITIES);

	if (newpri == oldpri)
		/* 不触及 count/mask，避免制造不必要的竞态窗口。 */
		return;

	/*
	 * If the CPU was currently mapped to a different value, we
	 * need to map it to the new value then remove the old value.
	 * Note, we must add the new value first, otherwise we risk the
	 * cpu being missed by the priority loop in cpupri_find.
	 */
	/*
	 * CPU 从旧桶迁移时必须先加入新桶再移出旧桶；反序会使 cpupri_find 的桶循环
	 * 暂时完全看不到它，破坏优先级提升时的候选保证。
	 */
	if (likely(newpri != CPUPRI_INVALID)) {
		/* vec 是目标桶，mask 先置位再通过 count 对 reader 宣告可读。 */
		struct cpupri_vec *vec = &cp->pri_to_cpu[newpri];

		cpumask_set_cpu(cpu, vec->mask);
		/*
		 * When adding a new vector, we update the mask first,
		 * do a write memory barrier, and then update the count, to
		 * make sure the vector is visible when count is set.
		 */
		/* 写屏障保证 reader 看到非零 count 后也能看到此前 mask 置位。 */
		smp_mb__before_atomic();
		atomic_inc(&(vec)->count);
		do_mb = 1;
	}
	if (likely(oldpri != CPUPRI_INVALID)) {
		/* vec 是来源桶；INVALID 表示 CPU 此前没有可撤销成员关系。 */
		struct cpupri_vec *vec  = &cp->pri_to_cpu[oldpri];

		/*
		 * Because the order of modification of the vec->count
		 * is important, we must make sure that the update
		 * of the new prio is seen before we decrement the
		 * old prio. This makes sure that the loop sees
		 * one or the other when we raise the priority of
		 * the run queue. We don't care about when we lower the
		 * priority, as that will trigger an rt pull anyway.
		 *
		 * We only need to do a memory barrier if we updated
		 * the new priority vec.
		 */
		/*
		 * 新桶 count 增加必须先于旧桶 count 减少对 reader 可见，使提升优先级时扫描
		 * 至少命中一处；只有实际加入新桶时才需要这道跨原子操作屏障。
		 */
		if (do_mb)
			smp_mb__after_atomic();

		/*
		 * When removing from the vector, we decrement the counter first
		 * do a memory barrier and then clear the mask.
		 */
		/* 移除按 count--、屏障、清 mask 排序，使零 count reader 可以安全跳过。 */
		atomic_dec(&(vec)->count);
		smp_mb__after_atomic();
		cpumask_clear_cpu(cpu, vec->mask);
	}

	/* 所有公开桶状态更新完成后提交同 CPU 的反向位置。 */
	*currpri = newpri;
}

/**
 * cpupri_init - initialize the cpupri structure
 * @cp: The cpupri context
 *
 * Return: -ENOMEM on memory allocation failure.
 */
/*
 * cpupri_init() - 初始化 root_domain 的 RT 优先级索引
 *
 * @cp 是调用者提供的纯输出对象；成功后拥有 101 个动态 cpumask 和 nr_cpu_ids 长度
 * 的反向表，必须由 cleanup 成对释放。逐桶清零 count 并以 GFP_KERNEL 分配 mask，
 * 再分配反向表并把 possible CPU 设 INVALID。返回 0 或 -ENOMEM；任一失败逆序释放
 * 已分配桶。函数可睡眠，失败对象不得发布或 cleanup。
 */
int cpupri_init(struct cpupri *cp)
{
	/* i 既是桶初始化下标，也在成功后遍历 possible CPU 反向表。 */
	int i;

	for (i = 0; i < CPUPRI_NR_PRIORITIES; i++) {
		/* vec 的 count/mask 必须在 root_domain 发布前同时初始化。 */
		struct cpupri_vec *vec = &cp->pri_to_cpu[i];

		atomic_set(&vec->count, 0);
		if (!zalloc_cpumask_var(&vec->mask, GFP_KERNEL))
			goto cleanup;
	}

	cp->cpu_to_pri = kzalloc_objs(int, nr_cpu_ids);
	if (!cp->cpu_to_pri)
		goto cleanup;

	for_each_possible_cpu(i)
		cp->cpu_to_pri[i] = CPUPRI_INVALID;

	return 0;

cleanup:
	/* i 指向失败桶或 NR_PRIORITIES；先减一，只释放此前成功分配的 mask。 */
	for (i--; i >= 0; i--)
		free_cpumask_var(cp->pri_to_cpu[i].mask);
	return -ENOMEM;
}

/**
 * cpupri_cleanup - clean up the cpupri structure
 * @cp: The cpupri context
 */
/*
 * cpupri_cleanup() - 释放成功初始化的索引
 *
 * @cp 必须已停止所有 find/set，正常由 root_domain 最后一引用消失并经过 RCU 宽限期
 * 后调用。先释放反向表，再释放全部桶 mask；无返回值、不取锁、不清空字段，调用后
 * 不得继续访问或重复 cleanup。函数不负责释放 cp/root_domain 本体。
 */
void cpupri_cleanup(struct cpupri *cp)
{
	/* i 遍历每个成功初始化的固定桶。 */
	int i;

	kfree(cp->cpu_to_pri);
	for (i = 0; i < CPUPRI_NR_PRIORITIES; i++)
		free_cpumask_var(cp->pri_to_cpu[i].mask);
}
