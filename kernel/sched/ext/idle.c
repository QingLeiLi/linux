// SPDX-License-Identifier: GPL-2.0
/*
 * BPF extensible scheduler class: Documentation/scheduler/sched-ext.rst
 *
 * Built-in idle CPU tracking policy.
 *
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 * Copyright (c) 2024 Andrea Righi <arighi@nvidia.com>
 */
/*
 * 本文件维护 sched_ext 内建的“可领取 idle CPU”索引，并向 BPF scheduler
 * 暴露受校验的选 CPU kfunc。cpu mask 表示当前可领取的逻辑 CPU，smt mask
 * 只保留整个 SMT core 都空闲的 CPU；test-and-clear 是领取点，但它只是
 * 与并发 idle 转换竞速后的候选，不是任务必然在该 CPU 运行的承诺。
 *
 * 全局或 per-NUMA-node mask 在 scheduler enable/disable 与 CPU hotplug
 * 边界重建；per-CPU 临时 mask 依赖禁止抢占。拓扑 span 在 RCU 下借用，
 * BPF acquire/release 仅建立 verifier 的 trusted-pointer 生命周期，底层
 * 永久 cpumask 并没有引用计数或释放动作。
 */
#include "internal.h"
#include "cid.h"
#include "idle.h"

/* Enable/disable built-in idle CPU selection policy */
static DEFINE_STATIC_KEY_FALSE(scx_builtin_idle_enabled);

/* Enable/disable per-node idle cpumasks */
static DEFINE_STATIC_KEY_FALSE(scx_builtin_idle_per_node);

/* Enable/disable LLC aware optimizations */
static DEFINE_STATIC_KEY_FALSE(scx_selcpu_topo_llc);

/* Enable/disable NUMA aware optimizations */
static DEFINE_STATIC_KEY_FALSE(scx_selcpu_topo_numa);

/*
 * cpumasks to track idle CPUs within each NUMA node.
 *
 * If SCX_OPS_BUILTIN_IDLE_PER_NODE is not enabled, a single global cpumask
 * from is used to track all the idle CPUs in the system.
 */
/*
 * 每个容器同时保存可领取逻辑 CPU 位图 @cpu 与“整个 SMT core 均空闲”的
 * 派生位图 @smt；全局容器或各 node 容器在启动时创建、整个内核生命周期保留。
 */
struct scx_idle_cpus {
	cpumask_var_t cpu;
	cpumask_var_t smt;
};

/*
 * Global host-wide idle cpumasks (used when SCX_OPS_BUILTIN_IDLE_PER_NODE
 * is not enabled).
 */
static struct scx_idle_cpus scx_idle_global_masks;

/*
 * Per-node idle cpumasks.
 */
static struct scx_idle_cpus **scx_idle_node_masks;

/*
 * Local per-CPU cpumasks (used to generate temporary idle cpumasks).
 */
static DEFINE_PER_CPU(cpumask_var_t, local_idle_cpumask);
static DEFINE_PER_CPU(cpumask_var_t, local_llc_idle_cpumask);
static DEFINE_PER_CPU(cpumask_var_t, local_numa_idle_cpumask);

/*
 * Return the idle masks associated to a target @node.
 *
 * NUMA_NO_NODE identifies the global idle cpumask.
 */
/* 返回 @node 的永久 mask 容器；NUMA_NO_NODE 选择全局容器，调用者仅借用。 */
/*
 * 业务背景：所有 idle 位图操作需统一选择全局或 per-NUMA-node 存储容器。
 * 入参：node 是 NUMA_NO_NODE 或有效 node id，纯输入且无 ownership。
 * 出参/返回：返回永久存活的非空借用 scx_idle_cpus；无输出参数和引用变化。
 * 注意事项：不校验 node 范围；per-node 模式下调用者须确保对应容器已初始化。
 */
static struct scx_idle_cpus *idle_cpumask(int node)
{
	return node == NUMA_NO_NODE ? &scx_idle_global_masks : scx_idle_node_masks[node];
}

/*
 * Returns the NUMA node ID associated with a @cpu, or NUMA_NO_NODE if
 * per-node idle cpumasks are disabled.
 */
/* per-node 跟踪开启时返回 @cpu 的 node，否则返回 NUMA_NO_NODE。 */
/*
 * 业务背景：选核路径要在不重复分支的情况下决定使用全局还是 node idle mask。
 * 入参：cpu 是有效 CPU 编号，纯输入。
 * 出参/返回：per-node static key 开启时返回 CPU 所属 node，否则 NUMA_NO_NODE；无输出参数。
 * 注意事项：结果是瞬时拓扑映射；调用者负责 CPU hotplug/拓扑稳定条件。
 */
static int scx_cpu_node_if_enabled(int cpu)
{
	if (!static_branch_maybe(CONFIG_NUMA, &scx_builtin_idle_per_node))
		return NUMA_NO_NODE;

	return cpu_to_node(cpu);
}

/* 原子领取 @cpu，并清除其整个 SMT core 的“全闲”候选；成功返回原 idle 位。 */
/*
 * 业务背景：多个选核者可能并发领取同一 idle CPU，必须以 test-and-clear 决出唯一赢家并撤销 SMT 候选。
 * 入参：cpu 是有效且对应 idle mask 已初始化的 CPU 编号。
 * 出参/返回：成功把 cpu 位从 idle mask 清除返回 true，已被领取返回 false；无输出参数。
 * 注意事项：原子位操作只保证领取竞争，不保证 CPU 随后仍 idle；SMT mask 清除可能扩大缓存写入。
 */
static bool scx_idle_test_and_clear_cpu(int cpu)
{
	int node = scx_cpu_node_if_enabled(cpu);
	struct cpumask *idle_cpus = idle_cpumask(node)->cpu;

	/*
	 * SMT mask should be cleared whether we can claim @cpu or not. The SMT
	 * cluster is not wholly idle either way. This also prevents
	 * scx_pick_idle_cpu() from getting caught in an infinite loop.
	 */
	if (sched_smt_active()) {
		const struct cpumask *smt = cpu_smt_mask(cpu);
		struct cpumask *idle_smts = idle_cpumask(node)->smt;

		/* 无论本次逻辑 CPU 领取成败，所在 core 都不能继续作为“完整空闲”候选。 */
		/*
		 * If offline, @cpu is not its own sibling and
		 * scx_pick_idle_cpu() can get caught in an infinite loop as
		 * @cpu is never cleared from the idle SMT mask. Ensure that
		 * @cpu is eventually cleared.
		 *
		 * NOTE: Use cpumask_intersects() and cpumask_test_cpu() to
		 * reduce memory writes, which may help alleviate cache
		 * coherence pressure.
		 */
		if (cpumask_intersects(smt, idle_smts))
			cpumask_andnot(idle_smts, idle_smts, smt);
		else if (cpumask_test_cpu(cpu, idle_smts))
			__cpumask_clear_cpu(cpu, idle_smts);
	}

	return cpumask_test_and_clear_cpu(cpu, idle_cpus);
}

/*
 * Pick an idle CPU in a specific NUMA node.
 */
/* 在 @node 先选全闲 SMT core 再选任意 idle CPU；成功同时领取，失败 -EBUSY。 */
/*
 * 业务背景：node 内选核优先完整空闲 core，降低 SMT 干扰，并用分布式扫描避免总从低 CPU 开始。
 * 入参：cpus_allowed 是不可空只读借用 mask；node 是有效/global 节点；flags 是 SCX_PICK_IDLE_* 位图。
 * 出参/返回：成功返回已领取 CPU；无候选或仅 core 模式失败返回 -EBUSY；无输出参数。
 * 注意事项：竞争失败会重试；allowed/node 必须匹配在线 mask，返回后 idle 状态仍可能变化。
 */
static s32 pick_idle_cpu_in_node(const struct cpumask *cpus_allowed, int node, u64 flags)
{
	int cpu;

retry:
	if (sched_smt_active()) {
		/* 分布式扫描先从 full-idle core 集合取候选，竞争失败回 retry 换下一个。 */
		cpu = cpumask_any_and_distribute(idle_cpumask(node)->smt, cpus_allowed);
		if (cpu < nr_cpu_ids)
			goto found;

		if (flags & SCX_PICK_IDLE_CORE)
			return -EBUSY;
	}

	cpu = cpumask_any_and_distribute(idle_cpumask(node)->cpu, cpus_allowed);
	/* 非 SMT 或允许部分空闲 core 时，退化到普通逻辑 CPU idle 集合。 */
	if (cpu >= nr_cpu_ids)
		return -EBUSY;

found:
	if (scx_idle_test_and_clear_cpu(cpu))
		return cpu;
	else
		goto retry;
}

#ifdef CONFIG_NUMA
/*
 * Tracks nodes that have not yet been visited when searching for an idle
 * CPU across all available nodes.
 */
static DEFINE_PER_CPU(nodemask_t, per_cpu_unvisited);

/*
 * Search for an idle CPU across all nodes, excluding @node.
 */
/* 禁抢占使用本 CPU nodemask，按距离搜索除 @node 外的在线 node。 */
/*
 * 业务背景：起始 node 无候选时，per-node 策略按 NUMA 距离逐层扩展搜索范围。
 * 入参：cpus_allowed 是不可空借用 mask；node 是已搜索的有效起点；flags 是 idle 选择约束。
 * 出参/返回：成功返回已领取 CPU，否则 -EBUSY；无输出参数和 ownership 变化。
 * 注意事项：内部禁止抢占以独占 per-CPU scratch nodemask；大型 NUMA 系统最坏 O(N^2)。
 */
static s32 pick_idle_cpu_from_online_nodes(const struct cpumask *cpus_allowed, int node, u64 flags)
{
	nodemask_t *unvisited;
	s32 cpu = -EBUSY;

	preempt_disable();
	/* 禁抢占保证 this_cpu scratch 在整个清空、遍历和返回前不被另一 CPU 误用。 */
	unvisited = this_cpu_ptr(&per_cpu_unvisited);

	/*
	 * Restrict the search to the online nodes (excluding the current
	 * node that has been visited already).
	 */
	nodes_copy(*unvisited, node_states[N_ONLINE]);
	node_clear(node, *unvisited);

	/*
	 * Traverse all nodes in order of increasing distance, starting
	 * from @node.
	 *
	 * This loop is O(N^2), with N being the amount of NUMA nodes,
	 * which might be quite expensive in large NUMA systems. However,
	 * this complexity comes into play only when a scheduler enables
	 * SCX_OPS_BUILTIN_IDLE_PER_NODE and it's requesting an idle CPU
	 * without specifying a target NUMA node, so it shouldn't be a
	 * bottleneck is most cases.
	 *
	 * As a future optimization we may want to cache the list of nodes
	 * in a per-node array, instead of actually traversing them every
	 * time.
	 */
	for_each_node_numadist(node, *unvisited) {
		/* 每个 node 内部仍执行原子领取；成功即停止更远距离搜索。 */
		cpu = pick_idle_cpu_in_node(cpus_allowed, node, flags);
		if (cpu >= 0)
			break;
	}
	preempt_enable();

	return cpu;
}
#else
/* 无 NUMA 配置时不存在其他 node 可搜索，恒返回 -EBUSY。 */
/*
 * 业务背景：非 NUMA 构建保留统一跨 node helper 接口，避免调用点增加配置分支。
 * 入参：cpus_allowed/node/flags 均为实现未使用的只读输入，不转移 ownership。
 * 出参/返回：恒返回 -EBUSY；无输出参数。
 * 注意事项：仅非 NUMA 配置；失败表示没有其他 node，不表示传入 mask 非法。
 */
static inline s32
pick_idle_cpu_from_online_nodes(const struct cpumask *cpus_allowed, int node, u64 flags)
{
	return -EBUSY;
}
#endif

/*
 * Find an idle CPU in the system, starting from @node.
 */
/* 先搜起始 @node，允许时再扩展到其他 node；返回已领取 CPU 或 -EBUSY。 */
/*
 * 业务背景：公共 idle 领取器需要组合 node 优先、仅 node 限制及跨 node 距离搜索。
 * 入参：cpus_allowed 是不可空借用 mask；node 是 NUMA_NO_NODE 或起点；flags 是 SCX_PICK_IDLE_* 位图。
 * 出参/返回：成功返回已原子领取 CPU，否则 -EBUSY；无输出参数。
 * 注意事项：函数不验证 flags 未知位；候选返回后仍须由调度核心校验可运行性。
 */
static s32 scx_pick_idle_cpu(const struct cpumask *cpus_allowed, int node, u64 flags)
{
	s32 cpu;

	/*
	 * Always search in the starting node first (this is an
	 * optimization that can save some cycles even when the search is
	 * not limited to a single node).
	 */
	cpu = pick_idle_cpu_in_node(cpus_allowed, node, flags);
	/* 起点通常是 prev CPU 所在 node；命中即可保留内存与缓存局部性。 */
	if (cpu >= 0)
		return cpu;

	/*
	 * Stop the search if we are using only a single global cpumask
	 * (NUMA_NO_NODE) or if the search is restricted to the first node
	 * only.
	 */
	if (node == NUMA_NO_NODE || flags & SCX_PICK_IDLE_IN_NODE)
		return -EBUSY;

	/* 只有 per-node 且未限制 IN_NODE 时，才按距离继续搜索其他在线 node。 */
	/*
	 * Extend the search to the other online nodes.
	 */
	return pick_idle_cpu_from_online_nodes(cpus_allowed, node, flags);
}

/*
 * Return the amount of CPUs in the same LLC domain of @cpu (or zero if the LLC
 * domain is not defined).
 */
/* RCU 读侧返回 @cpu LLC span 的 CPU 数；拓扑不存在返回 0。 */
/*
 * 业务背景：默认选核只在存在多个 LLC 域时启用缓存局部性优化，需要快速取得域大小。
 * 入参：cpu 是有效 CPU 编号，纯输入。
 * 出参/返回：返回 LLC span CPU 数，缺失 domain 返回 0；无输出参数。
 * 注意事项：调用者必须持 RCU 读锁并稳定拓扑，返回值只对应当前快照。
 */
static unsigned int llc_weight(s32 cpu)
{
	struct sched_domain *sd;

	sd = rcu_dereference(per_cpu(sd_llc, cpu));
	if (!sd)
		return 0;

	return sd->span_weight;
}

/*
 * Return the cpumask representing the LLC domain of @cpu (or NULL if the LLC
 * domain is not defined).
 */
/* RCU 读侧借用 @cpu 的 LLC span；拓扑不存在返回 NULL。 */
/*
 * 业务背景：选核器需把 idle mask 限制到 prev CPU 的 LLC 以优先复用共享缓存。
 * 入参：cpu 是有效 CPU 编号，纯输入。
 * 出参/返回：返回 RCU 借用的可空 LLC cpumask；无输出参数且不转移 ownership。
 * 注意事项：调用者持 RCU 读锁，不能修改或带出 span，NULL 表示拓扑未定义。
 */
static struct cpumask *llc_span(s32 cpu)
{
	struct sched_domain *sd;

	sd = rcu_dereference(per_cpu(sd_llc, cpu));
	if (!sd)
		return NULL;

	return sched_domain_span(sd);
}

/*
 * Return the amount of CPUs in the same NUMA domain of @cpu (or zero if the
 * NUMA domain is not defined).
 */
/* RCU 读侧返回 @cpu NUMA group 的 CPU 数；domain/group 缺失返回 0。 */
/*
 * 业务背景：NUMA 优化需比较本地 node 域与在线 CPU 总数及 LLC 域是否重合。
 * 入参：cpu 是有效 CPU 编号，纯输入。
 * 出参/返回：返回 NUMA group CPU 数，domain/group 缺失返回 0；无输出参数。
 * 注意事项：调用者必须持 RCU 读锁；不对非对称拓扑作持久保证。
 */
static unsigned int numa_weight(s32 cpu)
{
	struct sched_domain *sd;
	struct sched_group *sg;

	sd = rcu_dereference(per_cpu(sd_numa, cpu));
	/* NUMA domain 可能尚未建立，groups 也可能在拓扑退化时为空。 */
	if (!sd)
		return 0;
	sg = sd->groups;
	if (!sg)
		return 0;

	return sg->group_weight;
}

/*
 * Return the cpumask representing the NUMA domain of @cpu (or NULL if the NUMA
 * domain is not defined).
 */
/* RCU 读侧借用 @cpu NUMA group span；domain/group 缺失返回 NULL。 */
/*
 * 业务背景：默认选核可把候选限制在 prev CPU 的 NUMA 域以降低远端内存访问。
 * 入参：cpu 是有效 CPU 编号，纯输入。
 * 出参/返回：返回 RCU 借用的可空 NUMA cpumask；无输出参数和引用变化。
 * 注意事项：调用者持 RCU 读锁，返回 mask 只读且不能跨保护期保存。
 */
static struct cpumask *numa_span(s32 cpu)
{
	struct sched_domain *sd;
	struct sched_group *sg;

	sd = rcu_dereference(per_cpu(sd_numa, cpu));
	if (!sd)
		return NULL;
	/* group span 才是 NUMA 候选 mask；domain 存在但无 group 仍视为拓扑缺失。 */
	sg = sd->groups;
	if (!sg)
		return NULL;

	return sched_group_span(sg);
}

/*
 * Return true if the LLC domains do not perfectly overlap with the NUMA
 * domains, false otherwise.
 */
/* 扫描在线 CPU，判断是否至少一个 NUMA domain 含多个 LLC。 */
/*
 * 业务背景：LLC 与 NUMA 完全重合时重复两次局部性搜索没有收益，需检测是否真正错位。
 * 入参：无。
 * 出参/返回：任一在线 CPU 的 LLC/NUMA 权重不同返回 true，否则 false；无输出参数。
 * 注意事项：调用者持 RCU 且稳定 online mask；扫描是拓扑更新期控制路径而非唤醒热路径。
 */
static bool llc_numa_mismatch(void)
{
	int cpu;

	/*
	 * We need to scan all online CPUs to verify whether their scheduling
	 * domains overlap.
	 *
	 * While it is rare to encounter architectures with asymmetric NUMA
	 * topologies, CPU hotplugging or virtualized environments can result
	 * in asymmetric configurations.
	 *
	 * For example:
	 *
	 *  NUMA 0:
	 *    - LLC 0: cpu0..cpu7
	 *    - LLC 1: cpu8..cpu15 [offline]
	 *
	 *  NUMA 1:
	 *    - LLC 0: cpu16..cpu23
	 *    - LLC 1: cpu24..cpu31
	 *
	 * In this case, if we only check the first online CPU (cpu0), we might
	 * incorrectly assume that the LLC and NUMA domains are fully
	 * overlapping, which is incorrect (as NUMA 1 has two distinct LLC
	 * domains).
	 */
	for_each_online_cpu(cpu)
		/* 逐 CPU 比较可覆盖非对称 hotplug/虚拟拓扑，不能只抽样首个 CPU。 */
		if (llc_weight(cpu) != numa_weight(cpu))
			return true;

	return false;
}

/*
 * Initialize topology-aware scheduling.
 *
 * Detect if the system has multiple LLC or multiple NUMA domains and enable
 * cache-aware / NUMA-aware scheduling optimizations in the default CPU idle
 * selection policy.
 *
 * Assumption: the kernel's internal topology representation assumes that each
 * CPU belongs to a single LLC domain, and that each LLC domain is entirely
 * contained within a single NUMA node.
 */
/*
 * CPU hotplug 稳定期间检查在线拓扑并切换 LLC/NUMA static key；per-node
 * 模式天然限定 NUMA 搜索，故不重复启用全局 NUMA 优化。
 */
/*
 * 业务背景：CPU hotplug 或 scheduler 加载后要依据实际域关系开关默认选核的 LLC/NUMA 快路径。
 * 入参：ops 是不可空、加载期只读借用的 scheduler ops，flags 决定是否使用 per-node mask。
 * 出参/返回：无直接返回值、无输出参数；更新两个拓扑 static key 并打印调试快照。
 * 注意事项：调用者持 CPU hotplug 读/写保护；内部持 RCU，static key cpuslocked 版本依赖该锁。
 */
void scx_idle_update_selcpu_topology(struct sched_ext_ops *ops)
{
	bool enable_llc = false, enable_numa = false;
	unsigned int nr_cpus;
	s32 cpu = cpumask_first(cpu_online_mask);

	/*
	 * Enable LLC domain optimization only when there are multiple LLC
	 * domains among the online CPUs. If all online CPUs are part of a
	 * single LLC domain, the idle CPU selection logic can choose any
	 * online CPU without bias.
	 *
	 * Note that it is sufficient to check the LLC domain of the first
	 * online CPU to determine whether a single LLC domain includes all
	 * CPUs.
	 */
	rcu_read_lock();
	/* 首个在线 CPU 的 LLC 覆盖全部在线 CPU 时，全局再按 LLC 搜索没有收益。 */
	nr_cpus = llc_weight(cpu);
	if (nr_cpus > 0) {
		if (nr_cpus < num_online_cpus())
			enable_llc = true;
		pr_debug("sched_ext: LLC=%*pb weight=%u\n",
			 cpumask_pr_args(llc_span(cpu)), llc_weight(cpu));
	}

	/*
	 * Enable NUMA optimization only when there are multiple NUMA domains
	 * among the online CPUs and the NUMA domains don't perfectly overlap
	 * with the LLC domains.
	 *
	 * If all CPUs belong to the same NUMA node and the same LLC domain,
	 * enabling both NUMA and LLC optimizations is unnecessary, as checking
	 * for an idle CPU in the same domain twice is redundant.
	 *
	 * If SCX_OPS_BUILTIN_IDLE_PER_NODE is enabled ignore the NUMA
	 * optimization, as we would naturally select idle CPUs within
	 * specific NUMA nodes querying the corresponding per-node cpumask.
	 */
	if (!(ops->flags & SCX_OPS_BUILTIN_IDLE_PER_NODE)) {
		/* 全局 mask 模式下，仅当 NUMA 跨 LLC 时再增加独立 NUMA 局部性阶段。 */
		nr_cpus = numa_weight(cpu);
		if (nr_cpus > 0) {
			if (nr_cpus < num_online_cpus() && llc_numa_mismatch())
				enable_numa = true;
			pr_debug("sched_ext: NUMA=%*pb weight=%u\n",
				 cpumask_pr_args(numa_span(cpu)), nr_cpus);
		}
	}
	rcu_read_unlock();

	/* 调试输出记录本轮拓扑推导结果，随后才切换静态分支。 */
	pr_debug("sched_ext: LLC idle selection %s\n",
		 str_enabled_disabled(enable_llc));
	pr_debug("sched_ext: NUMA idle selection %s\n",
		 str_enabled_disabled(enable_numa));

	if (enable_llc)
		/* CPU 集已锁定，static key 可安全在拓扑结论变化时切换。 */
		static_branch_enable_cpuslocked(&scx_selcpu_topo_llc);
	else
		static_branch_disable_cpuslocked(&scx_selcpu_topo_llc);
	if (enable_numa)
		static_branch_enable_cpuslocked(&scx_selcpu_topo_numa);
	else
		static_branch_disable_cpuslocked(&scx_selcpu_topo_numa);
}

/*
 * Return true if @p can run on all possible CPUs, false otherwise.
 */
/* 返回 @p 的亲和性是否覆盖所有 possible CPU；仅读 nr_cpus_allowed 快照。 */
/*
 * 业务背景：额外 allowed mask 与 task mask 求交前，可用全亲和性条件跳过 per-CPU scratch 计算。
 * 入参：p 是不可空、借用且亲和性字段已由调用上下文稳定的 task。
 * 出参/返回：允许 CPU 数覆盖 possible CPU 总数返回 true，否则 false；无输出参数。
 * 注意事项：只比较计数而不返回 mask；CPU hotplug 不改变 possible 集合，task 锁契约由调用者承担。
 */
static inline bool task_affinity_all(const struct task_struct *p)
{
	return p->nr_cpus_allowed >= num_possible_cpus();
}

/*
 * Built-in CPU idle selection policy:
 *
 * 1. Prioritize full-idle cores:
 *   - always prioritize CPUs from fully idle cores (both logical CPUs are
 *     idle) to avoid interference caused by SMT.
 *
 * 2. Reuse the same CPU:
 *   - prefer the last used CPU to take advantage of cached data (L1, L2) and
 *     branch prediction optimizations.
 *
 * 3. Prefer @prev_cpu's SMT sibling:
 *   - if @prev_cpu is busy and no fully idle core is available, try to
 *     place the task on an idle SMT sibling of @prev_cpu; keeping the
 *     task on the same core makes migration cheaper, preserves L1 cache
 *     locality and reduces wakeup latency.
 *
 * 4. Pick a CPU within the same LLC (Last-Level Cache):
 *   - if the above conditions aren't met, pick a CPU that shares the same
 *     LLC, if the LLC domain is a subset of @cpus_allowed, to maintain
 *     cache locality.
 *
 * 5. Pick a CPU within the same NUMA node, if enabled:
 *   - choose a CPU from the same NUMA node, if the node cpumask is a
 *     subset of @cpus_allowed, to reduce memory access latency.
 *
 * 6. Pick any idle CPU within the @cpus_allowed domain.
 *
 * Step 4 and 5 are performed only if the system has, respectively,
 * multiple LLCs / multiple NUMA nodes (see scx_selcpu_topo_llc and
 * scx_selcpu_topo_numa) and they don't contain the same subset of CPUs.
 *
 * If %SCX_OPS_BUILTIN_IDLE_PER_NODE is enabled, the search will always
 * begin in @prev_cpu's node and proceed to other nodes in order of
 * increasing distance.
 *
 * Return the picked CPU if idle, or a negative value otherwise.
 *
 * NOTE: tasks that can only run on 1 CPU are excluded by this logic, because
 * we never call ops.select_cpu() for them, see select_task_rq().
 */
/*
 * 在稳定 @p 亲和性、禁止抢占且 RCU 保护拓扑下，按同步唤醒、全闲 core、
 * prev CPU/sibling、LLC、NUMA、全局顺序领取 idle CPU；返回 CPU 或负 errno。
 */
/*
 * 业务背景：未实现自定义 select_cpu 的 scheduler 需要兼顾同步唤醒、缓存/NUMA 局部性和空闲 core 的默认策略。
 * 入参：p 是不可空锁稳定 task；prev_cpu 是有效旧 CPU；wake_flags/flags 为选择位；cpus_allowed 可空借用 mask。
 * 出参/返回：成功返回已领取 idle CPU；无合适候选返回负 errno；无输出参数和 ownership 变化。
 * 注意事项：内部禁抢占并持 RCU，使用 per-CPU scratch；领取只是提示，最终放置仍由调度核心复核。
 */
s32 scx_select_cpu_dfl(struct task_struct *p, s32 prev_cpu, u64 wake_flags,
		       const struct cpumask *cpus_allowed, u64 flags)
{
	const struct cpumask *llc_cpus = NULL, *numa_cpus = NULL;
	const struct cpumask *allowed = cpus_allowed ?: p->cpus_ptr;
	int node = scx_cpu_node_if_enabled(prev_cpu);
	bool is_prev_allowed;
	s32 cpu;

	/* 禁抢占覆盖所有 this_cpu scratch 与 smp_processor_id 使用。 */
	preempt_disable();

	/*
	 * Determine the subset of CPUs usable by @p within @cpus_allowed.
	 */
	/* 显式 allowed 还要与 task 亲和性求交，交集存入仅本次禁抢占窗口可用的 scratch。 */
	if (allowed != p->cpus_ptr) {
		struct cpumask *local_cpus = this_cpu_cpumask_var_ptr(local_idle_cpumask);

		/* 全亲和 task 可直接采用外部 mask；否则只有非空交集才形成有效候选域。 */
		if (task_affinity_all(p)) {
			allowed = cpus_allowed;
		} else if (cpumask_and(local_cpus, cpus_allowed, p->cpus_ptr)) {
			allowed = local_cpus;
		} else {
			cpu = -EBUSY;
			goto out_enable;
		}
	}

	/*
	 * Check whether @prev_cpu is still within the allowed set. If not,
	 * we can still try selecting a nearby CPU.
	 */
	is_prev_allowed = cpumask_test_cpu(prev_cpu, allowed);

	/* RCU 稳定后续借用的 LLC/NUMA sched_domain spans。 */
	/*
	 * This is necessary to protect llc_cpus.
	 */
	rcu_read_lock();

	/*
	 * Determine the subset of CPUs that the task can use in its
	 * current LLC and node.
	 *
	 * If the task can run on all CPUs, use the node and LLC cpumasks
	 * directly.
	 */
	if (static_branch_maybe(CONFIG_NUMA, &scx_selcpu_topo_numa)) {
		/* 全亲和 task 可直接借用 domain mask，否则在 per-CPU scratch 中与 allowed 求交。 */
		struct cpumask *local_cpus = this_cpu_cpumask_var_ptr(local_numa_idle_cpumask);
		const struct cpumask *cpus = numa_span(prev_cpu);

		if (allowed == p->cpus_ptr && task_affinity_all(p))
			numa_cpus = cpus;
		else if (cpus && cpumask_and(local_cpus, allowed, cpus))
			numa_cpus = local_cpus;
	}

	if (static_branch_maybe(CONFIG_SCHED_MC, &scx_selcpu_topo_llc)) {
		/* LLC 候选用独立 scratch，避免覆盖仍待使用的 NUMA 交集。 */
		struct cpumask *local_cpus = this_cpu_cpumask_var_ptr(local_llc_idle_cpumask);
		const struct cpumask *cpus = llc_span(prev_cpu);

		if (allowed == p->cpus_ptr && task_affinity_all(p))
			llc_cpus = cpus;
		else if (cpus && cpumask_and(local_cpus, allowed, cpus))
			llc_cpus = local_cpus;
	}

	/*
	 * If WAKE_SYNC, try to migrate the wakee to the waker's CPU.
	 */
	/* 同步唤醒先尝试 cache-affine prev，再在欠载且 waker 本地 DSQ 为空时选择 waker CPU。 */
	if (wake_flags & SCX_WAKE_SYNC) {
		int waker_node;

		/*
		 * If the waker's CPU is cache affine and prev_cpu is idle,
		 * then avoid a migration.
		 */
		cpu = smp_processor_id();
		if (is_prev_allowed && cpus_share_cache(cpu, prev_cpu) &&
		    scx_idle_test_and_clear_cpu(prev_cpu)) {
			cpu = prev_cpu;
			goto out_unlock;
		}

		/*
		 * If the waker's local DSQ is empty, and the system is under
		 * utilized, try to wake up @p to the local DSQ of the waker.
		 *
		 * Checking only for an empty local DSQ is insufficient as it
		 * could give the wakee an unfair advantage when the system is
		 * oversaturated.
		 *
		 * Checking only for the presence of idle CPUs is also
		 * insufficient as the local DSQ of the waker could have tasks
		 * piled up on it even if there is an idle core elsewhere on
		 * the system.
		 */
		waker_node = scx_cpu_node_if_enabled(cpu);
		/* 同时要求 waker 未退出、本地 DSQ 空、node 合规且系统仍存在 idle 容量。 */
		if (!(current->flags & PF_EXITING) &&
		    cpu_rq(cpu)->scx.local_dsq.nr == 0 &&
		    (!(flags & SCX_PICK_IDLE_IN_NODE) || (waker_node == node)) &&
		    !cpumask_empty(idle_cpumask(waker_node)->cpu)) {
			if (cpumask_test_cpu(cpu, allowed))
				goto out_unlock;
		}
	}

	/*
	 * If CPU has SMT, any wholly idle CPU is likely a better pick than
	 * partially idle @prev_cpu.
	 */
	/* SMT 阶段严格先完整空闲 core，再考虑 prev CPU 和部分空闲 sibling。 */
	if (sched_smt_active()) {
		/*
		 * Keep using @prev_cpu if it's part of a fully idle core.
		 */
		if (is_prev_allowed &&
		    cpumask_test_cpu(prev_cpu, idle_cpumask(node)->smt) &&
		    scx_idle_test_and_clear_cpu(prev_cpu)) {
			cpu = prev_cpu;
			goto out_unlock;
		}

		/*
		 * Search for any fully idle core in the same LLC domain.
		 */
		if (llc_cpus) {
			/* 同 LLC full-idle core 同时保留缓存局部性与硬件线程隔离。 */
			cpu = pick_idle_cpu_in_node(llc_cpus, node, SCX_PICK_IDLE_CORE);
			if (cpu >= 0)
				goto out_unlock;
		}

		/*
		 * Search for any fully idle core in the same NUMA node.
		 */
		if (numa_cpus) {
			/* LLC 耗尽后扩大到本 NUMA 域，但仍坚持整个 core 空闲。 */
			cpu = pick_idle_cpu_in_node(numa_cpus, node, SCX_PICK_IDLE_CORE);
			if (cpu >= 0)
				goto out_unlock;
		}

		/*
		 * Search for any full-idle core usable by the task.
		 *
		 * If the node-aware idle CPU selection policy is enabled
		 * (%SCX_OPS_BUILTIN_IDLE_PER_NODE), the search will always
		 * begin in prev_cpu's node and proceed to other nodes in
		 * order of increasing distance.
		 */
		cpu = scx_pick_idle_cpu(allowed, node, flags | SCX_PICK_IDLE_CORE);
		/* 最后跨域搜索完整空闲 core，仍遵守 allowed 与 node 距离限制。 */
		if (cpu >= 0)
			goto out_unlock;

		/*
		 * Give up if we're strictly looking for a full-idle SMT
		 * core.
		 */
		if (flags & SCX_PICK_IDLE_CORE) {
			cpu = -EBUSY;
			goto out_unlock;
		}
	}

	/*
	 * Use @prev_cpu if it's idle.
	 */
	if (is_prev_allowed && scx_idle_test_and_clear_cpu(prev_cpu)) {
		/* full-core 策略未命中后复用 prev CPU，优先保留私有 cache 热度。 */
		cpu = prev_cpu;
		goto out_unlock;
	}

	/*
	 * Use @prev_cpu's sibling if it's idle.
	 */
	if (sched_smt_active()) {
		/* prev 忙时尝试其允许的 idle sibling，代价通常低于跨物理 core。 */
		for_each_cpu_and(cpu, cpu_smt_mask(prev_cpu), allowed) {
			if (cpu == prev_cpu)
				continue;
			if (scx_idle_test_and_clear_cpu(cpu))
				goto out_unlock;
		}
	}

	/*
	 * Search for any idle CPU in the same LLC domain.
	 */
	if (llc_cpus) {
		/* 普通 idle 阶段保留同 LLC，但不再要求 sibling 全部 idle。 */
		cpu = pick_idle_cpu_in_node(llc_cpus, node, 0);
		if (cpu >= 0)
			goto out_unlock;
	}

	/*
	 * Search for any idle CPU in the same NUMA node.
	 */
	if (numa_cpus) {
		/* LLC 候选耗尽后仍尽量保持 NUMA 内存局部性。 */
		cpu = pick_idle_cpu_in_node(numa_cpus, node, 0);
		if (cpu >= 0)
			goto out_unlock;
	}

	/*
	 * Search for any idle CPU usable by the task.
	 *
	 * If the node-aware idle CPU selection policy is enabled
	 * (%SCX_OPS_BUILTIN_IDLE_PER_NODE), the search will always begin
	 * in prev_cpu's node and proceed to other nodes in order of
	 * increasing distance.
	 */
	cpu = scx_pick_idle_cpu(allowed, node, flags);

out_unlock:
	/* 统一逆序释放 RCU 和抢占保护，所有借用 domain/scratch 到此失效。 */
	rcu_read_unlock();
out_enable:
	preempt_enable();

	return cpu;
}

/*
 * Initialize global and per-node idle cpumasks.
 */
/* 启动期分配全局、各 node 与各 CPU 临时 mask；分配失败 BUG，成功后永久存在。 */
/*
 * 业务背景：内建 idle 跟踪和选核 scratch 在 scheduler 启用前必须为所有可能 CPU/node 预分配。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；创建永久全局/per-node/per-CPU cpumask 存储。
 * 注意事项：启动期 GFP_KERNEL 可睡眠；任一分配失败触发 BUG，成功对象本文件不释放。
 */
void scx_idle_init_masks(void)
{
	int i;

	/* Allocate global idle cpumasks */
	/* 全局模式的 cpu/smt 两张位图成对存在，任一失败都不能继续启动。 */
	BUG_ON(!alloc_cpumask_var(&scx_idle_global_masks.cpu, GFP_KERNEL));
	BUG_ON(!alloc_cpumask_var(&scx_idle_global_masks.smt, GFP_KERNEL));

	/* Allocate per-node idle cpumasks (use nr_node_ids for non-contiguous NUMA nodes) */
	scx_idle_node_masks = kzalloc_objs(*scx_idle_node_masks, nr_node_ids);
	BUG_ON(!scx_idle_node_masks);

	for_each_node(i) {
		/* 容器与两张位图都优先从目标 node 分配，减少跨 node 更新成本。 */
		scx_idle_node_masks[i] = kzalloc_node(sizeof(**scx_idle_node_masks),
							 GFP_KERNEL, i);
		BUG_ON(!scx_idle_node_masks[i]);

		BUG_ON(!alloc_cpumask_var_node(&scx_idle_node_masks[i]->cpu, GFP_KERNEL, i));
		BUG_ON(!alloc_cpumask_var_node(&scx_idle_node_masks[i]->smt, GFP_KERNEL, i));
	}

	/* Allocate local per-cpu idle cpumasks */
	/* 三套 scratch 分离 allowed、LLC 与 NUMA 交集，避免同次选核互相覆盖。 */
	for_each_possible_cpu(i) {
		BUG_ON(!alloc_cpumask_var_node(&per_cpu(local_idle_cpumask, i),
					       GFP_KERNEL, cpu_to_node(i)));
		BUG_ON(!alloc_cpumask_var_node(&per_cpu(local_llc_idle_cpumask, i),
					       GFP_KERNEL, cpu_to_node(i)));
		BUG_ON(!alloc_cpumask_var_node(&per_cpu(local_numa_idle_cpumask, i),
					       GFP_KERNEL, cpu_to_node(i)));
	}
}

/* 更新 @cpu idle 位，并以“所有 sibling 均 idle”派生 SMT mask；竞态可自修复。 */
/*
 * 业务背景：CPU idle/busy 转换要维护可领取逻辑 CPU 与完整空闲 SMT core 两级索引。
 * 入参：cpu 是有效 CPU 编号；idle 是目标状态，均为纯输入。
 * 出参/返回：无直接返回值、无输出参数；更新所属容器的 cpu/smt 位图。
 * 注意事项：并发 SMT 派生允许短暂不精确且会自修复；调用者提供 rq 锁/状态转换排序。
 */
static void update_builtin_idle(int cpu, bool idle)
{
	int node = scx_cpu_node_if_enabled(cpu);
	struct cpumask *idle_cpus = idle_cpumask(node)->cpu;

	assign_cpu(cpu, idle_cpus, idle);

	/* SMT 派生仅在硬件线程拓扑激活时维护；非 SMT 机器逻辑 mask 已足够。 */
	if (sched_smt_active()) {
		const struct cpumask *smt = cpu_smt_mask(cpu);
		struct cpumask *idle_smts = idle_cpumask(node)->smt;

		if (idle) {
			/*
			 * idle_smt handling is racy but that's fine as it's
			 * only for optimization and self-correcting.
			 */
			if (!cpumask_subset(smt, idle_cpus))
				return;
			/* 最后一个 sibling 进入 idle 后，整组 sibling 一次性加入 full-core 候选。 */
			cpumask_or(idle_smts, idle_smts, smt);
		} else {
			/* 任一 sibling 变 busy 都撤销整个 core，防止继续按 full-idle 领取。 */
			cpumask_andnot(idle_smts, idle_smts, smt);
		}
	}
}

/*
 * Update the idle state of a CPU to @idle.
 *
 * If @do_notify is true, ops.update_idle() is invoked to notify the scx
 * scheduler of an actual idle state transition (idle to busy or vice
 * versa). If @do_notify is false, only the idle state in the idle masks is
 * refreshed without invoking ops.update_idle().
 *
 * This distinction is necessary, because an idle CPU can be "reserved" and
 * awakened via scx_bpf_pick_idle_cpu() + scx_bpf_kick_cpu(), marking it as
 * busy even if no tasks are dispatched. In this case, the CPU may return
 * to idle without a true state transition. Refreshing the idle masks
 * without invoking ops.update_idle() ensures accurate idle state tracking
 * while avoiding unnecessary updates and maintaining balanced state
 * transitions.
 */
/*
 * 在已持 @rq 锁下先更新内建 mask，再可选调用 BPF update_idle；顺序保证
 * enqueue 要么看到 idle 位，要么 update_idle 回调看到已排队任务。
 */
/*
 * 业务背景：idle class 切换要同步内建索引，并可将真实状态转换通知 BPF scheduler。
 * 入参：rq 是不可空且当前已锁的输入/输出队列；idle 是目标状态；do_notify 区分真实转换与 mask 刷新。
 * 出参/返回：无直接返回值、无输出参数；可能更新 mask并调用 ops.update_idle。
 * 注意事项：rq 锁内不可睡眠；内建更新必须先于 BPF 回调，bypass 时抑制通知。
 */
void __scx_update_idle(struct rq *rq, bool idle, bool do_notify)
{
	struct scx_sched *sch = scx_root;
	int cpu = cpu_of(rq);

	lockdep_assert_rq_held(rq);

	/* idle-to-idle 刷新用于归还此前预留却未真正运行任务的 CPU。 */
	/*
	 * Update the idle masks:
	 * - for real idle transitions (do_notify == true)
	 * - for idle-to-idle transitions (indicated by the previous task
	 *   being the idle thread, managed by pick_task_idle())
	 *
	 * Skip updating idle masks if the previous task is not the idle
	 * thread, since set_next_task_idle() has already handled it when
	 * transitioning from a task to the idle thread (calling this
	 * function with do_notify == true).
	 *
	 * In this way we can avoid updating the idle masks twice,
	 * unnecessarily.
	 */
	if (static_branch_likely(&scx_builtin_idle_enabled))
		if (do_notify || is_idle_task(rq->curr))
			update_builtin_idle(cpu, idle);

	/*
	 * Trigger ops.update_idle() only when transitioning from a task to
	 * the idle thread and vice versa.
	 *
	 * Idle transitions are indicated by do_notify being set to true,
	 * managed by put_prev_task_idle()/set_next_task_idle().
	 *
	 * This must come after builtin idle update so that BPF schedulers can
	 * create interlocking between ops.update_idle() and ops.enqueue() -
	 * either enqueue() sees the idle bit or update_idle() sees the task
	 * that enqueue() queued.
	 */
	if (SCX_HAS_OP(sch, update_idle) && do_notify &&
	    !scx_bypassing(sch, cpu_of(rq)))
		/* 回调带 rq 上下文发布，内部 kfunc 可验证已锁队列。 */
		SCX_CALL_OP(sch, update_idle, rq, scx_cpu_arg(cpu_of(rq)), idle);
}

/* 启用策略时把在线 CPU 乐观标为 idle，并按 ops flags 选择全局或 node mask。 */
/*
 * 业务背景：每次 scheduler 启用需从干净基线重建 idle 位图，随后由真实切换快速收敛。
 * 入参：ops 是不可空、加载期只读借用 ops，flags 选择全局或 per-node 布局。
 * 出参/返回：无直接返回值、无输出参数；覆盖选中布局的 cpu/smt masks。
 * 注意事项：调用者稳定 online/node masks；初始乐观状态可能短暂把 busy CPU 标 idle。
 */
static void reset_idle_masks(struct sched_ext_ops *ops)
{
	int node;

	/*
	 * Consider all online cpus idle. Should converge to the actual state
	 * quickly.
	 */
	if (!(ops->flags & SCX_OPS_BUILTIN_IDLE_PER_NODE)) {
		/* 全局模式直接复制 online mask，后续真实切换会清除乐观 busy 位。 */
		cpumask_copy(idle_cpumask(NUMA_NO_NODE)->cpu, cpu_online_mask);
		cpumask_copy(idle_cpumask(NUMA_NO_NODE)->smt, cpu_online_mask);
		return;
	}

	for_each_node(node) {
		const struct cpumask *node_mask = cpumask_of_node(node);

		/* node 容器只标记同时 online 且属于该 node 的 CPU。 */
		cpumask_and(idle_cpumask(node)->cpu, cpu_online_mask, node_mask);
		cpumask_and(idle_cpumask(node)->smt, cpu_online_mask, node_mask);
	}
}

/* CPU 集稳定时配置内建/per-node static key 并重置 mask；无直接返回值。 */
/*
 * 业务背景：scheduler ops 决定保留内建 idle 跟踪还是完全接管 update_idle，并可选择 per-node 索引。
 * 入参：ops 是不可空、加载期稳定且由 core 拥有的只读 ops。
 * 出参/返回：无直接返回值、无输出参数；切换 static keys并重置永久 masks。
 * 注意事项：调用者持 CPU hotplug 锁，必须使用 cpuslocked static-key API；不取得 ops ownership。
 */
void scx_idle_enable(struct sched_ext_ops *ops)
{
	/* 没有自定义 update_idle 或显式 KEEP 时，core 继续维护可供 kfunc 使用的位图。 */
	if (!ops->update_idle || (ops->flags & SCX_OPS_KEEP_BUILTIN_IDLE))
		static_branch_enable_cpuslocked(&scx_builtin_idle_enabled);
	else
		static_branch_disable_cpuslocked(&scx_builtin_idle_enabled);

	if (ops->flags & SCX_OPS_BUILTIN_IDLE_PER_NODE)
		/* per-node key 决定所有 mask 查找的存储布局，必须在 reset 前设置。 */
		static_branch_enable_cpuslocked(&scx_builtin_idle_per_node);
	else
		static_branch_disable_cpuslocked(&scx_builtin_idle_per_node);

	reset_idle_masks(ops);
}

/* 关闭两个 idle 跟踪 static key；永久 mask 保留供下次启用重建。 */
/*
 * 业务背景：scheduler 停用后热路径不应继续读取其 idle 策略，但预分配存储可供下次复用。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；关闭内建和 per-node static keys。
 * 注意事项：调用者负责与使用者同步；不会清空或释放 masks，旧位不得在禁用期解释。
 */
void scx_idle_disable(void)
{
	static_branch_disable(&scx_builtin_idle_enabled);
	static_branch_disable(&scx_builtin_idle_per_node);
}

/********************************************************************************
 * Helpers that can be called from the BPF scheduler.
 */

/* 校验 per-node 模式与 @node；成功原样返回 node，失败记录 scx_error 并返回 errno。 */
/*
 * 业务背景：node 型 BPF kfunc 必须统一拒绝未启用、越界或不可能存在的 NUMA node。
 * 入参：sch 是不可空借用 scheduler；node 是待校验 s32 node id。
 * 出参/返回：合法返回 node；NO_NODE 返回 -ENOENT；模式关闭 -EOPNOTSUPP；非法返回 -EINVAL。
 * 注意事项：除 NO_NODE 外失败会记录 scx_error并可能触发停用；无输出参数和 ownership 变化。
 */
static int validate_node(struct scx_sched *sch, int node)
{
	/* 模式关闭是接口误用，向 scheduler 报错后返回不支持。 */
	if (!static_branch_likely(&scx_builtin_idle_per_node)) {
		scx_error(sch, "per-node idle tracking is disabled");
		return -EOPNOTSUPP;
	}

	/* Return no entry for NUMA_NO_NODE (not a critical scx error) */
	if (node == NUMA_NO_NODE)
		return -ENOENT;

	/* Make sure node is in a valid range */
	if (node < 0 || node >= nr_node_ids) {
		/* 范围错误与 possible 集合缺失分开诊断，便于 BPF 修正输入。 */
		scx_error(sch, "invalid node %d", node);
		return -EINVAL;
	}

	/* Make sure the node is part of the set of possible nodes */
	if (!node_possible(node)) {
		scx_error(sch, "unavailable node %d", node);
		return -EINVAL;
	}

	return node;
}

__bpf_kfunc_start_defs();

/* 检查内建 idle static key；关闭时向 @sch 报错并返回 false。 */
/*
 * 业务背景：依赖内建位图的 BPF kfunc 必须在访问永久 mask 前确认 scheduler 未完全接管 idle 跟踪。
 * 入参：sch 是不可空、借用的错误归属 scheduler。
 * 出参/返回：内建跟踪开启返回 true，否则记录错误并返回 false；无输出参数。
 * 注意事项：static key 是瞬时状态；失败通过 scx_error 影响 scheduler 生命周期。
 */
static bool check_builtin_idle_enabled(struct scx_sched *sch)
{
	if (static_branch_likely(&scx_builtin_idle_enabled))
		return true;

	scx_error(sch, "built-in idle tracking is disabled");
	return false;
}

/*
 * Determine whether @p is a migration-disabled task in the context of BPF
 * code.
 *
 * We can't simply check whether @p->migration_disabled is set in a
 * sched_ext callback, because the BPF prolog (__bpf_prog_enter) may disable
 * migration for the current task while running BPF code.
 *
 * Since the BPF prolog calls migrate_disable() only when CONFIG_PREEMPT_RCU
 * is enabled (via rcu_read_lock_dont_migrate()), migration_disabled == 1 for
 * the current task is ambiguous only in that case: it could be from the BPF
 * prolog rather than a real migrate_disable() call.
 *
 * Without CONFIG_PREEMPT_RCU, the BPF prolog never calls migrate_disable(),
 * so migration_disabled == 1 always means the task is truly
 * migration-disabled.
 *
 * Therefore, when migration_disabled == 1 and CONFIG_PREEMPT_RCU is enabled,
 * check whether @p is the current task or not: if it is, then migration was
 * not disabled before entering the callback, otherwise migration was disabled.
 *
 * Returns true if @p is migration-disabled, false otherwise.
 */
/* 排除 PREEMPT_RCU BPF prolog 的一次临时禁止迁移，判断 @p 原本是否被固定。 */
/*
 * 业务背景：BPF prolog 自身可能把 current 的 migration_disabled 加一，选核不能误判为用户固定。
 * 入参：p 是不可空、借用且 migration_disabled 可稳定读取的 task。
 * 出参/返回：task 在进入 BPF 前已禁止迁移返回 true，否则 false；无输出参数。
 * 注意事项：CONFIG_PREEMPT_RCU 下仅值 1 且 p==current 被视为 prolog 临时状态。
 */
static bool is_bpf_migration_disabled(const struct task_struct *p)
{
	if (p->migration_disabled == 1) {
		if (IS_ENABLED(CONFIG_PREEMPT_RCU))
			return p != current;
		return true;
	}
	return p->migration_disabled;
}

/*
 * 统一 BPF 选 CPU 入口：验证 scheduler/prev_cpu/内建跟踪，并确认现有 rq/pi
 * 锁确实覆盖 @p；无锁上下文自行取 pi_lock，成功返回已领取 CPU，否则 errno。
 */
/*
 * 业务背景：多个 BPF 调用上下文共享默认选核器，必须统一验证 scheduler、CPU、idle 能力和 task 锁覆盖。
 * 入参：sch/p 不可空借用；prev_cpu 有效；wake_flags/flags 为位图；allowed 是可空只读 mask。
 * 出参/返回：成功返回已领取 CPU；校验/锁归属失败 -EINVAL，禁用或无候选 -EBUSY；无输出参数。
 * 注意事项：可自行 irqsave 获取 pi_lock但不睡眠；跨 task rq 回调会记录 scx_error。
 */
static s32 select_cpu_from_kfunc(struct scx_sched *sch, struct task_struct *p,
				 s32 prev_cpu, u64 wake_flags,
				 const struct cpumask *allowed, u64 flags)
{
	unsigned long irq_flags;
	bool we_locked = false;
	s32 cpu;

	/* 先拒绝非法 CPU 和关闭的内建策略，避免之后进入任何 task 锁路径。 */
	if (!scx_cpu_valid(sch, prev_cpu, NULL))
		return -EINVAL;

	if (!check_builtin_idle_enabled(sch))
		return -EBUSY;

	/* 亲和性读取必须由 select_cpu 的 pi_lock、当前 SCX rq 锁或本函数自取 pi_lock 三选一保护。 */
	/*
	 * Accessing p->cpus_ptr / p->nr_cpus_allowed needs either @p's rq
	 * lock or @p's pi_lock. Three cases:
	 *
	 *  - inside ops.select_cpu(): try_to_wake_up() holds the wake-up
	 *    task's pi_lock; the wake-up task is recorded in kf_tasks[0]
	 *    by SCX_CALL_OP_TASK_RET().
	 *  - other rq-locked SCX op: scx_locked_rq() points at the held rq.
	 *  - truly unlocked (UNLOCKED ops, SYSCALL, non-SCX struct_ops):
	 *    nothing held, take pi_lock ourselves.
	 *
	 * In the first two cases, BPF schedulers may pass an arbitrary task
	 * that the held lock doesn't cover. Refuse those.
	 */
	if (this_rq()->scx.in_select_cpu) {
		if (!scx_kf_arg_task_ok(sch, p))
			return -EINVAL;
		lockdep_assert_held(&p->pi_lock);
	} else if (scx_locked_rq()) {
		if (task_rq(p) != scx_locked_rq())
			goto cross_task;
	} else {
		/* 真正无锁的 syscall/test_run 上下文自行 irqsave，返回前严格配对释放。 */
		raw_spin_lock_irqsave(&p->pi_lock, irq_flags);
		we_locked = true;
	}

	/*
	 * This may also be called from ops.enqueue(), so we need to handle
	 * per-CPU tasks as well. For these tasks, we can skip all idle CPU
	 * selection optimizations and simply check whether the previously
	 * used CPU is idle and within the allowed cpumask.
	 */
	if (p->nr_cpus_allowed == 1 || is_bpf_migration_disabled(p)) {
		/* 固定 task 不能走拓扑扩展，只能确认 prev_cpu 同时允许且仍可领取。 */
		if (cpumask_test_cpu(prev_cpu, allowed ?: p->cpus_ptr) &&
		    scx_idle_test_and_clear_cpu(prev_cpu))
			cpu = prev_cpu;
		else
			cpu = -EBUSY;
	} else {
		cpu = scx_select_cpu_dfl(p, prev_cpu, wake_flags,
					 allowed ?: p->cpus_ptr, flags);
	}

	if (we_locked)
		/* 仅释放本函数取得的锁，调用者传入的 rq/pi 锁 ownership 保持不变。 */
		raw_spin_unlock_irqrestore(&p->pi_lock, irq_flags);

	return cpu;

cross_task:
	scx_error(sch, "select_cpu kfunc called cross-task on %s[%d]",
		  p->comm, p->pid);
	return -EINVAL;
}

/**
 * scx_bpf_cpu_node - Return the NUMA node the given @cpu belongs to, or
 *		      trigger an error if @cpu is invalid
 * @cpu: target CPU
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 */
/* 校验 @aux 所属 scheduler 与 @cpu，成功返回 NUMA node，失败返回 NUMA_NO_NODE。 */
/*
 * 业务背景：BPF scheduler 需要把合法 CPU 映射到 NUMA node 以选择 node 型 idle 接口。
 * 入参：cpu 是待校验 CPU；aux 是不可空、调用期借用且对 BPF 隐藏的 program 元数据。
 * 出参/返回：成功返回 node id；无 scheduler或 CPU 非法返回 NUMA_NO_NODE；无输出参数。
 * 注意事项：函数在 RCU guard 内借用 scheduler；非法 CPU 校验可能记录 scheduler 错误。
 */
__bpf_kfunc s32 scx_bpf_cpu_node(s32 cpu, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	/* aux 先解析为调用 scheduler，避免把 CPU 校验错误归到错误层级。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch) || !scx_cpu_valid(sch, cpu, NULL))
		return NUMA_NO_NODE;
	return cpu_to_node(cpu);
}

/**
 * scx_bpf_select_cpu_dfl - The default implementation of ops.select_cpu()
 * @p: task_struct to select a CPU for
 * @prev_cpu: CPU @p was on previously
 * @wake_flags: %SCX_WAKE_* flags
 * @is_idle: out parameter indicating whether the returned CPU is idle
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Can be called from ops.select_cpu(), ops.enqueue(), or from an unlocked
 * context such as a BPF test_run() call, as long as built-in CPU selection
 * is enabled: ops.update_idle() is missing or %SCX_OPS_KEEP_BUILTIN_IDLE
 * is set.
 *
 * Returns the picked CPU with *@is_idle indicating whether the picked CPU is
 * currently idle and thus a good candidate for direct dispatching.
 */
/* 返回默认选择 CPU，并通过 @is_idle 输出是否成功领取；无 idle 候选时退回 prev_cpu。 */
/*
 * 业务背景：BPF select_cpu 可复用内建策略，并需知道返回 CPU 是否适合直接 dispatch。
 * 入参：p/aux 不可空借用；prev_cpu 有效；wake_flags 为位图；is_idle 是不可空布尔输出槽。
 * 出参/返回：成功/无候选均返回 CPU，分别写 true/false；无 scheduler 返回 -ENODEV且不保证写输出。
 * 注意事项：RCU 内调用统一选核；返回 idle=true 只表示已领取瞬时 idle 位。
 */
__bpf_kfunc s32 scx_bpf_select_cpu_dfl(struct task_struct *p, s32 prev_cpu,
				       u64 wake_flags, bool *is_idle,
				       const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	s32 cpu;

	guard(rcu)();

	/* 解析失败在任何能力/CPU 检查前返回，避免空 scheduler 参与错误上报。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return -ENODEV;

	/* 负返回只表示未领取 idle 候选，ABI 仍以 prev_cpu 作为可用 fallback。 */
	cpu = select_cpu_from_kfunc(sch, p, prev_cpu, wake_flags, NULL, 0);
	if (cpu >= 0) {
		*is_idle = true;
		return cpu;
	}
	*is_idle = false;
	return prev_cpu;
}

/*
 * 把 BPF 五参数上限之外的输入封装成借用结构：prev_cpu 是上次 CPU，
 * wake_flags 描述唤醒关系，flags 控制 idle/core/node 搜索边界。
 */
struct scx_bpf_select_cpu_and_args {
	/* @p and @cpus_allowed can't be packed together as KF_RCU is not transitive */
	s32			prev_cpu;
	u64			wake_flags;
	u64			flags;
};

/**
 * __scx_bpf_select_cpu_and - Arg-wrapped CPU selection with cpumask
 * @p: task_struct to select a CPU for
 * @cpus_allowed: cpumask of allowed CPUs
 * @args: struct containing the rest of the arguments
 *       @args->prev_cpu: CPU @p was on previously
 *       @args->wake_flags: %SCX_WAKE_* flags
 *       @args->flags: %SCX_PICK_IDLE* flags
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Wrapper kfunc that takes arguments via struct to work around BPF's 5 argument
 * limit. BPF programs should use scx_bpf_select_cpu_and() which is provided
 * as an inline wrapper in common.bpf.h.
 *
 * Can be called from ops.select_cpu(), ops.enqueue(), or from an unlocked
 * context such as a BPF test_run() call, as long as built-in CPU selection
 * is enabled: ops.update_idle() is missing or %SCX_OPS_KEEP_BUILTIN_IDLE
 * is set.
 *
 * @p, @args->prev_cpu and @args->wake_flags match ops.select_cpu().
 *
 * Returns the selected idle CPU, which will be automatically awakened upon
 * returning from ops.select_cpu() and can be used for direct dispatch, or
 * a negative value if no idle CPU is available.
 */
/* 以参数结构绕过 BPF 五参数上限，返回已领取 idle CPU 或负 errno。 */
/*
 * 业务背景：BPF kfunc 最多五参数，显式 allowed mask 的选核接口需把其余参数封装传入。
 * 入参：p/cpus_allowed/args/aux 均不可空借用；args 含 prev_cpu、wake flags 和 idle flags。
 * 出参/返回：返回已领取 CPU，或 -ENODEV/-EINVAL/-EBUSY；无输出参数和 ownership 变化。
 * 注意事项：KF_RCU 不传递到打包字段，p 与 mask 必须分开；RCU guard 稳定 scheduler。
 */
__bpf_kfunc s32
__scx_bpf_select_cpu_and(struct task_struct *p, const struct cpumask *cpus_allowed,
			 struct scx_bpf_select_cpu_and_args *args,
			 const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	/* scheduler 解析和 node 校验均在同一 RCU 窗口，返回 mask 由 verifier acquire 接管。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return -ENODEV;

	/* args 字段只读拆包，实际锁与亲和性验证统一由 select_cpu_from_kfunc 完成。 */
	return select_cpu_from_kfunc(sch, p, args->prev_cpu, args->wake_flags,
				     cpus_allowed, args->flags);
}

/*
 * COMPAT: Will be removed in v6.22.
 */
/* 兼容旧五参数 ABI；存在 sub-scheduler 时无法判定调用者，拒绝并返回 -EINVAL。 */
/*
 * 业务背景：v6.22 前 BPF 程序仍可调用旧五参数接口，单 root 情况下转接统一选核器。
 * 入参：p/cpus_allowed 不可空借用；prev_cpu、wake_flags、flags 为输入；无隐式 aux。
 * 出参/返回：成功返回已领取 CPU；无 root -ENODEV；分层歧义 -EINVAL；无候选 -EBUSY。
 * 注意事项：兼容接口将移除；有任一 sub-scheduler 时拒绝并向 p 所属 scheduler 报错。
 */
__bpf_kfunc s32 scx_bpf_select_cpu_and(struct task_struct *p, s32 prev_cpu, u64 wake_flags,
				       const struct cpumask *cpus_allowed, u64 flags)
{
	struct scx_sched *sch;

	guard(rcu)();

	sch = rcu_dereference(scx_root);
	if (unlikely(!sch))
		return -ENODEV;

	/* 旧 ABI 没有 aux，只有不存在 children 时 root 才是唯一且安全的调用归属。 */
#ifdef CONFIG_EXT_SUB_SCHED
	/*
	 * Disallow if any sub-scheds are attached. There is no way to tell
	 * which scheduler called us, just error out @p's scheduler.
	 */
	if (unlikely(!list_empty(&sch->children))) {
		scx_error(scx_task_sched(p), "__scx_bpf_select_cpu_and() must be used");
		return -EINVAL;
	}
#endif

	return select_cpu_from_kfunc(sch, p, prev_cpu, wake_flags,
				     cpus_allowed, flags);
}

/**
 * scx_bpf_get_idle_cpumask_node - Get a referenced kptr to the
 * idle-tracking per-CPU cpumask of a target NUMA node.
 * @node: target NUMA node
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Returns an empty cpumask if idle tracking is not enabled, if @node is
 * not valid, or running on a UP kernel. In this case the actual error will
 * be reported to the BPF scheduler via scx_error().
 */
/* 获取指定 node 的 idle mask verifier 引用；校验失败返回永久空 mask。 */
/*
 * 业务背景：per-node BPF 策略需读取当前可领取 CPU 集合，并让 verifier 追踪 trusted pointer 生命周期。
 * 入参：node 是目标 id；aux 是不可空、调用期借用的隐式 program 元数据。
 * 出参/返回：成功返回 acquire 的只读 node idle mask；失败返回永久 cpu_none_mask；无输出参数。
 * 注意事项：返回指针必须用 put kfunc 结束 verifier 生命周期；底层永久对象不实际增加引用。
 */
__bpf_kfunc const struct cpumask *
scx_bpf_get_idle_cpumask_node(s32 node, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	/* 先解析归属，再检查全局/per-node 模式，确保错误记到实际调用 scheduler。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return cpu_none_mask;

	/* validate_node 同时确认 per-node 模式，失败统一返回 verifier 可接受的永久空 mask。 */
	node = validate_node(sch, node);
	if (node < 0)
		return cpu_none_mask;

	return idle_cpumask(node)->cpu;
}

/**
 * scx_bpf_get_idle_cpumask - Get a referenced kptr to the idle-tracking
 * per-CPU cpumask.
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Returns an empty mask if idle tracking is not enabled, or running on a
 * UP kernel.
 */
/* 获取全局 idle mask 的 verifier 引用；per-node/禁用/无 scheduler 时返回空 mask。 */
/*
 * 业务背景：全局模式 BPF 策略需读取逻辑 idle CPU 位图作为候选集合。
 * 入参：aux 是不可空、调用期借用的隐式 BPF program 元数据。
 * 出参/返回：成功返回 acquire 的全局只读 idle mask；错误返回 cpu_none_mask；无输出参数。
 * 注意事项：per-node 模式禁止调用；返回值需配对 put，位图内容可与并发状态转换竞速。
 */
__bpf_kfunc const struct cpumask *scx_bpf_get_idle_cpumask(const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	/* 检查顺序为 scheduler、内建开关、CPU 合法性，最后才参与原子领取竞争。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return cpu_none_mask;

	if (static_branch_unlikely(&scx_builtin_idle_per_node)) {
		/* 全局 getter 与 per-node 布局互斥，防止读取未维护的 global 位图。 */
		scx_error(sch, "SCX_OPS_BUILTIN_IDLE_PER_NODE enabled");
		return cpu_none_mask;
	}

	if (!check_builtin_idle_enabled(sch))
		return cpu_none_mask;

	return idle_cpumask(NUMA_NO_NODE)->cpu;
}

/**
 * scx_bpf_get_idle_smtmask_node - Get a referenced kptr to the
 * idle-tracking, per-physical-core cpumask of a target NUMA node. Can be
 * used to determine if an entire physical core is free.
 * @node: target NUMA node
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Returns an empty cpumask if idle tracking is not enabled, if @node is
 * not valid, or running on a UP kernel. In this case the actual error will
 * be reported to the BPF scheduler via scx_error().
 */
/* 获取指定 node 的全闲 SMT mask；无 SMT 时返回该 node 的逻辑 idle mask。 */
/*
 * 业务背景：per-node 策略要优先整个物理 core 空闲的候选，并在非 SMT 机器复用逻辑 idle 集合。
 * 入参：node 是目标 id；aux 是不可空、调用期借用的隐式 program 元数据。
 * 出参/返回：成功返回 acquire 的 smt/cpu mask；失败返回 cpu_none_mask；无输出参数。
 * 注意事项：需配对 put；mask 是竞态允许的优化快照，不承诺返回时 core 仍完全 idle。
 */
__bpf_kfunc const struct cpumask *
scx_bpf_get_idle_smtmask_node(s32 node, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	/* scheduler 与 node 均合法后才访问对应永久 mask，避免越界容器解引用。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return cpu_none_mask;

	node = validate_node(sch, node);
	if (node < 0)
		return cpu_none_mask;

	/* 非 SMT 系统 cpu mask 与“完整 core 空闲”语义等价。 */
	if (sched_smt_active())
		return idle_cpumask(node)->smt;
	else
		return idle_cpumask(node)->cpu;
}

/**
 * scx_bpf_get_idle_smtmask - Get a referenced kptr to the idle-tracking,
 * per-physical-core cpumask. Can be used to determine if an entire physical
 * core is free.
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Returns an empty mask if idle tracking is not enabled, or running on a
 * UP kernel.
 */
/* 获取全局全闲 SMT mask；无 SMT 时退回逻辑 idle mask，错误返回空 mask。 */
/*
 * 业务背景：全局策略需要读取完整空闲物理 core 集合，以减少同 core 线程干扰。
 * 入参：aux 是不可空、调用期借用的隐式 program 元数据。
 * 出参/返回：成功返回 acquire 的全局 smt/cpu mask；失败返回 cpu_none_mask；无输出参数。
 * 注意事项：per-node 模式或内建跟踪关闭时记录错误；结果必须配对 put且只读。
 */
__bpf_kfunc const struct cpumask *scx_bpf_get_idle_smtmask(const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	/* 全局 API 显式拒绝 per-node 布局，避免对未维护 global mask 作出成功承诺。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return cpu_none_mask;

	if (static_branch_unlikely(&scx_builtin_idle_per_node)) {
		scx_error(sch, "SCX_OPS_BUILTIN_IDLE_PER_NODE enabled");
		return cpu_none_mask;
	}

	if (!check_builtin_idle_enabled(sch))
		return cpu_none_mask;

	/* 返回永久容器中的只读地址，KF_ACQUIRE 只约束 BPF verifier 生命周期。 */
	if (sched_smt_active())
		return idle_cpumask(NUMA_NO_NODE)->smt;
	else
		return idle_cpumask(NUMA_NO_NODE)->cpu;
}

/**
 * scx_bpf_put_idle_cpumask - Release a previously acquired referenced kptr to
 * either the percpu, or SMT idle-tracking cpumask.
 * @idle_mask: &cpumask to use
 */
/* 结束 verifier 的 acquire 生命周期；全局 mask 永久存在，运行时无需真正 put。 */
/*
 * 业务背景：get idle mask kfunc 的 trusted pointer 必须有 release 端供 verifier 证明生命周期闭合。
 * 入参：idle_mask 是由匹配 get kfunc 返回的不可空只读借用 mask。
 * 出参/返回：无直接返回值、无输出参数；仅结束 verifier ownership，不释放底层对象。
 * 注意事项：运行时为空函数；传入非 acquire 指针会由 verifier 拒绝，而非在这里检查。
 */
__bpf_kfunc void scx_bpf_put_idle_cpumask(const struct cpumask *idle_mask)
{
	/*
	 * Empty function body because we aren't actually acquiring or releasing
	 * a reference to a global idle cpumask, which is read-only in the
	 * caller and is never released. The acquire / release semantics here
	 * are just used to make the cpumask a trusted pointer in the caller.
	 */
}

/**
 * scx_bpf_test_and_clear_cpu_idle - Test and clear @cpu's idle state
 * @cpu: cpu to test and clear idle for
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Returns %true if @cpu was idle and its idle state was successfully cleared.
 * %false otherwise.
 *
 * Unavailable if ops.update_idle() is implemented and
 * %SCX_OPS_KEEP_BUILTIN_IDLE is not set.
 */
/* 校验调用 scheduler 与 CPU 后尝试领取 idle 位；成功 true，任一条件失败 false。 */
/*
 * 业务背景：BPF 已选定具体 CPU 时需原子确认并领取其内建 idle 状态，避免重复 direct dispatch。
 * 入参：cpu 是待领取 CPU；aux 是不可空、调用期借用的隐式 program 元数据。
 * 出参/返回：成功清 idle 位返回 true；无 scheduler、禁用、非法或竞争失败返回 false；无输出参数。
 * 注意事项：RCU guard 稳定 scheduler；true 仍不保证 CPU 在实际 dispatch 前保持 idle。
 */
__bpf_kfunc bool scx_bpf_test_and_clear_cpu_idle(s32 cpu, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	/* node 校验既检查布局开关也检查范围，成功后才能索引 node mask。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return false;

	if (!check_builtin_idle_enabled(sch))
		return false;

	if (!scx_cpu_valid(sch, cpu, NULL))
		return false;

	/* 最终原子 test-and-clear 决定本调用是否赢得并发领取竞争。 */
	return scx_idle_test_and_clear_cpu(cpu);
}

/**
 * scx_bpf_pick_idle_cpu_node - Pick and claim an idle cpu from @node
 * @cpus_allowed: Allowed cpumask
 * @node: target NUMA node
 * @flags: %SCX_PICK_IDLE_* flags
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Pick and claim an idle cpu in @cpus_allowed from the NUMA node @node.
 *
 * Returns the picked idle cpu number on success, or -%EBUSY if no matching
 * cpu was found.
 *
 * The search starts from @node and proceeds to other online NUMA nodes in
 * order of increasing distance (unless SCX_PICK_IDLE_IN_NODE is specified,
 * in which case the search is limited to the target @node).
 *
 * Always returns an error if ops.update_idle() is implemented and
 * %SCX_OPS_KEEP_BUILTIN_IDLE is not set, or if
 * %SCX_OPS_BUILTIN_IDLE_PER_NODE is not set.
 */
/* 校验 node 后在允许集合中领取 idle CPU；成功返回 CPU，失败返回具体 errno。 */
/*
 * 业务背景：per-node BPF 策略需要从指定起点按距离搜索并原子领取允许的 idle CPU。
 * 入参：cpus_allowed/aux 不可空借用；node 是起点；flags 是 SCX_PICK_IDLE_* 输入位图。
 * 出参/返回：成功返回已领取 CPU；无 scheduler -ENODEV，node 校验错误或无候选返回负 errno。
 * 注意事项：RCU 内借用 scheduler/mask；返回 CPU 的最终可运行性和随后状态仍由 core 复核。
 */
__bpf_kfunc s32 scx_bpf_pick_idle_cpu_node(const struct cpumask *cpus_allowed,
					   s32 node, u64 flags,
					   const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	/* 全局 helper 先解析 scheduler，再拒绝与 per-node mask 布局混用。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return -ENODEV;

	node = validate_node(sch, node);
	if (node < 0)
		return node;

	/* node helper 可按 flags 扩展到其他 node，除非显式指定 IN_NODE。 */
	return scx_pick_idle_cpu(cpus_allowed, node, flags);
}

/**
 * scx_bpf_pick_idle_cpu - Pick and claim an idle cpu
 * @cpus_allowed: Allowed cpumask
 * @flags: %SCX_PICK_IDLE_CPU_* flags
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Pick and claim an idle cpu in @cpus_allowed. Returns the picked idle cpu
 * number on success. -%EBUSY if no matching cpu was found.
 *
 * Idle CPU tracking may race against CPU scheduling state transitions. For
 * example, this function may return -%EBUSY as CPUs are transitioning into the
 * idle state. If the caller then assumes that there will be dispatch events on
 * the CPUs as they were all busy, the scheduler may end up stalling with CPUs
 * idling while there are pending tasks. Use scx_bpf_pick_any_cpu() and
 * scx_bpf_kick_cpu() to guarantee that there will be at least one dispatch
 * event in the near future.
 *
 * Unavailable if ops.update_idle() is implemented and
 * %SCX_OPS_KEEP_BUILTIN_IDLE is not set.
 *
 * Always returns an error if %SCX_OPS_BUILTIN_IDLE_PER_NODE is set, use
 * scx_bpf_pick_idle_cpu_node() instead.
 */
/* 全局模式领取 idle CPU；per-node 模式或跟踪关闭时返回 -EBUSY。 */
/*
 * 业务背景：非 per-node BPF 策略需要从全局允许集合原子领取一个 idle CPU。
 * 入参：cpus_allowed/aux 不可空借用；flags 是 SCX_PICK_IDLE_* 输入位图。
 * 出参/返回：成功返回 CPU；无 scheduler -ENODEV，模式冲突/禁用/无候选返回 -EBUSY。
 * 注意事项：模式冲突会记录 scx_error；领取结果是瞬时提示，不能保证后续一定 dispatch。
 */
__bpf_kfunc s32 scx_bpf_pick_idle_cpu(const struct cpumask *cpus_allowed,
				      u64 flags, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	/* scheduler 归属成功后才检查全局/per-node 模式，错误需记到正确实例。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return -ENODEV;

	if (static_branch_maybe(CONFIG_NUMA, &scx_builtin_idle_per_node)) {
		scx_error(sch, "per-node idle tracking is enabled");
		return -EBUSY;
	}

	if (!check_builtin_idle_enabled(sch))
		return -EBUSY;

	/* NUMA_NO_NODE 强制使用全局永久 mask，不进入按距离 node 遍历。 */
	return scx_pick_idle_cpu(cpus_allowed, NUMA_NO_NODE, flags);
}

/**
 * scx_bpf_pick_any_cpu_node - Pick and claim an idle cpu if available
 *			       or pick any CPU from @node
 * @cpus_allowed: Allowed cpumask
 * @node: target NUMA node
 * @flags: %SCX_PICK_IDLE_CPU_* flags
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Pick and claim an idle cpu in @cpus_allowed. If none is available, pick any
 * CPU in @cpus_allowed. Guaranteed to succeed and returns the picked idle cpu
 * number if @cpus_allowed is not empty. -%EBUSY is returned if @cpus_allowed is
 * empty.
 *
 * The search starts from @node and proceeds to other online NUMA nodes in
 * order of increasing distance (unless %SCX_PICK_IDLE_IN_NODE is specified,
 * in which case the search is limited to the target @node, regardless of
 * the CPU idle state).
 *
 * If ops.update_idle() is implemented and %SCX_OPS_KEEP_BUILTIN_IDLE is not
 * set, this function can't tell which CPUs are idle and will always pick any
 * CPU.
 */
/* 优先领取 node 内 idle CPU，无候选时分散选择任意允许 CPU；空集合 -EBUSY。 */
/*
 * 业务背景：为保证将来至少有可 kick 的 dispatch 目标，node 策略可在无 idle 候选时退化为任意 CPU。
 * 入参：cpus_allowed/aux 不可空借用；node 是起点；flags 控制仅 node/完整 core 等约束。
 * 出参/返回：返回 idle 或任意允许 CPU；无 scheduler/node 错误返回相应 errno，空集合 -EBUSY。
 * 注意事项：fallback CPU 可能忙且未被领取；调用者通常还需 kick 以保证调度事件。
 */
__bpf_kfunc s32 scx_bpf_pick_any_cpu_node(const struct cpumask *cpus_allowed,
					  s32 node, u64 flags,
					  const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	s32 cpu;

	guard(rcu)();

	/* 先验证调用 scheduler 和 node，随后才执行 idle-first/fallback 两阶段选择。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return -ENODEV;

	node = validate_node(sch, node);
	if (node < 0)
		return node;

	cpu = scx_pick_idle_cpu(cpus_allowed, node, flags);
	if (cpu >= 0)
		return cpu;

	/* fallback 不修改 idle 位；IN_NODE 仍限制集合，否则从完整 allowed 分散选取。 */
	if (flags & SCX_PICK_IDLE_IN_NODE)
		cpu = cpumask_any_and_distribute(cpumask_of_node(node), cpus_allowed);
	else
		cpu = cpumask_any_distribute(cpus_allowed);
	if (cpu < nr_cpu_ids)
		return cpu;
	else
		return -EBUSY;
}

/**
 * scx_bpf_pick_any_cpu - Pick and claim an idle cpu if available or pick any CPU
 * @cpus_allowed: Allowed cpumask
 * @flags: %SCX_PICK_IDLE_CPU_* flags
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Pick and claim an idle cpu in @cpus_allowed. If none is available, pick any
 * CPU in @cpus_allowed. Guaranteed to succeed and returns the picked idle cpu
 * number if @cpus_allowed is not empty. -%EBUSY is returned if @cpus_allowed is
 * empty.
 *
 * If ops.update_idle() is implemented and %SCX_OPS_KEEP_BUILTIN_IDLE is not
 * set, this function can't tell which CPUs are idle and will always pick any
 * CPU.
 *
 * Always returns an error if %SCX_OPS_BUILTIN_IDLE_PER_NODE is set, use
 * scx_bpf_pick_any_cpu_node() instead.
 */
/* 全局模式优先 idle、再任选允许 CPU；per-node 模式拒绝，空集合 -EBUSY。 */
/*
 * 业务背景：全局策略需在 idle 跟踪竞态下仍选出可 kick CPU，避免任务等待而所有 CPU 随后进入 idle。
 * 入参：cpus_allowed/aux 不可空借用；flags 是 SCX_PICK_IDLE_* 输入位图。
 * 出参/返回：返回 idle 或任意允许 CPU；无 scheduler -ENODEV，模式冲突/空集合 -EBUSY。
 * 注意事项：任意 fallback 不代表 idle或已领取；per-node scheduler 必须改用 node 版本。
 */
__bpf_kfunc s32 scx_bpf_pick_any_cpu(const struct cpumask *cpus_allowed,
				     u64 flags, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	s32 cpu;

	guard(rcu)();

	/* per-node 布局必须改走 node API；全局接口只在唯一 global mask 语义下兜底。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return -ENODEV;

	if (static_branch_maybe(CONFIG_NUMA, &scx_builtin_idle_per_node)) {
		scx_error(sch, "per-node idle tracking is enabled");
		return -EBUSY;
	}

	if (static_branch_likely(&scx_builtin_idle_enabled)) {
		/* 有内建索引时先尝试原子领取；失败再保证返回某个允许的忙 CPU。 */
		cpu = scx_pick_idle_cpu(cpus_allowed, NUMA_NO_NODE, flags);
		if (cpu >= 0)
			return cpu;
	}

	/* 无 idle 候选或内建跟踪关闭时，从 allowed 中分散选一个可能忙的 CPU。 */
	cpu = cpumask_any_distribute(cpus_allowed);
	if (cpu < nr_cpu_ids)
		return cpu;
	else
		return -EBUSY;
}

/* 至此结束 kfunc 定义区，下面只描述 verifier 注册集合而不再实现运行逻辑。 */
__bpf_kfunc_end_defs();

BTF_KFUNCS_START(scx_kfunc_ids_idle)
/* acquire/release 标志让 verifier 配对 mask 指针，RCU 标志约束 scheduler/task 借用。 */
BTF_ID_FLAGS(func, scx_bpf_cpu_node, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_get_idle_cpumask_node, KF_IMPLICIT_ARGS | KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_get_idle_cpumask, KF_IMPLICIT_ARGS | KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_get_idle_smtmask_node, KF_IMPLICIT_ARGS | KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_get_idle_smtmask, KF_IMPLICIT_ARGS | KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_put_idle_cpumask, KF_RELEASE)
/* 领取与选择类不返回 trusted mask，其中 pick 系列要求 verifier 保持 RCU 参数有效。 */
BTF_ID_FLAGS(func, scx_bpf_test_and_clear_cpu_idle, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_pick_idle_cpu_node, KF_IMPLICIT_ARGS | KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_pick_idle_cpu, KF_IMPLICIT_ARGS | KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_pick_any_cpu_node, KF_IMPLICIT_ARGS | KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_pick_any_cpu, KF_IMPLICIT_ARGS | KF_RCU)
BTF_KFUNCS_END(scx_kfunc_ids_idle)

static const struct btf_kfunc_id_set scx_kfunc_set_idle = {
	/* 通用 idle 集可注册到 tracing；运行时 filter 继续限制具体 scheduler form。 */
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_idle,
	.filter			= scx_kfunc_context_filter,
};

/*
 * The select_cpu kfuncs internally call task_rq_lock() when invoked from an
 * rq-unlocked context, and thus cannot be safely called from arbitrary tracing
 * contexts where @p's pi_lock state is unknown. Keep them out of
 * BPF_PROG_TYPE_TRACING by registering them in their own set which is exposed
 * only to STRUCT_OPS and SYSCALL programs.
 *
 * These kfuncs are also members of scx_kfunc_ids_unlocked (see ext.c) because
 * they're callable from unlocked contexts in addition to ops.select_cpu() and
 * ops.enqueue().
 */
BTF_KFUNCS_START(scx_kfunc_ids_select_cpu)
/* select_cpu 可能取 pi_lock，故只对 STRUCT_OPS 与 SYSCALL 注册，不进入 tracing。 */
BTF_ID_FLAGS(func, __scx_bpf_select_cpu_and, KF_IMPLICIT_ARGS | KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_select_cpu_and, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_select_cpu_dfl, KF_IMPLICIT_ARGS | KF_RCU)
BTF_KFUNCS_END(scx_kfunc_ids_select_cpu)

static const struct btf_kfunc_id_set scx_kfunc_set_select_cpu = {
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_select_cpu,
	.filter			= scx_kfunc_context_filter,
};

/* 为允许的 BPF program type 注册 idle/select_cpu kfunc 集；首个失败 errno 终止链。 */
/*
 * 业务背景：启动时要把通用 idle kfunc 与可能获取 pi_lock 的 select_cpu kfunc 注册到不同安全 program 类型。
 * 入参：无。
 * 出参/返回：全部注册成功返回 0；任一步失败返回首个负 errno，后续集合不再注册；无输出参数。
 * 注意事项：初始化期可调用；失败不回滚之前已注册集合，调用方按 init 错误处理模块启用。
 */
int scx_idle_init(void)
{
	return register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS, &scx_kfunc_set_idle) ?:
	       register_btf_kfunc_id_set(BPF_PROG_TYPE_TRACING, &scx_kfunc_set_idle) ?:
	       register_btf_kfunc_id_set(BPF_PROG_TYPE_SYSCALL, &scx_kfunc_set_idle) ?:
	       register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS, &scx_kfunc_set_select_cpu) ?:
	       register_btf_kfunc_id_set(BPF_PROG_TYPE_SYSCALL, &scx_kfunc_set_select_cpu);
}
