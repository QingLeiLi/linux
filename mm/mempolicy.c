// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simple NUMA memory policy for the Linux kernel.
 *
 * Copyright 2003,2004 Andi Kleen, SuSE Labs.
 * (C) Copyright 2005 Christoph Lameter, Silicon Graphics, Inc.
 *
 * NUMA policy allows the user to give hints in which node(s) memory should
 * be allocated.
 *
 * Support six policies per VMA and per process:
 *
 * The VMA policy has priority over the process policy for a page fault.
 *
 * interleave     Allocate memory interleaved over a set of nodes,
 *                with normal fallback if it fails.
 *                For VMA based allocations this interleaves based on the
 *                offset into the backing object or offset into the mapping
 *                for anonymous memory. For process policy an process counter
 *                is used.
 *
 * weighted interleave
 *                Allocate memory interleaved over a set of nodes based on
 *                a set of weights (per-node), with normal fallback if it
 *                fails.  Otherwise operates the same as interleave.
 *                Example: nodeset(0,1) & weights (2,1) - 2 pages allocated
 *                on node 0 for every 1 page allocated on node 1.
 *
 * bind           Only allocate memory on a specific set of nodes,
 *                no fallback.
 *                FIXME: memory is allocated starting with the first node
 *                to the last. It would be better if bind would truly restrict
 *                the allocation to memory nodes instead
 *
 * preferred      Try a specific node first before normal fallback.
 *                As a special case NUMA_NO_NODE here means do the allocation
 *                on the local CPU. This is normally identical to default,
 *                but useful to set in a VMA when you have a non default
 *                process policy.
 *
 * preferred many Try a set of nodes first before normal fallback. This is
 *                similar to preferred without the special case.
 *
 * default        Allocate on the local node first, or when on a VMA
 *                use the process policy. This is what Linux always did
 *		  in a NUMA aware kernel and still does by, ahem, default.
 *
 * The process policy is applied for most non interrupt memory allocations
 * in that process' context. Interrupts ignore the policies and always
 * try to allocate on the local CPU. The VMA policy is only applied for memory
 * allocations for a VMA in the VM.
 *
 * Currently there are a few corner cases in swapping where the policy
 * is not applied, but the majority should be handled. When process policy
 * is used it is not remembered over swap outs/swap ins.
 *
 * Only the highest zone in the zone hierarchy gets policied. Allocations
 * requesting a lower zone just use default policy. This implies that
 * on systems with highmem kernel lowmem allocation don't get policied.
 * Same with GFP_DMA allocations.
 *
 * For shmem/tmpfs shared memory the policy is shared between
 * all users and remembered even when nobody has memory mapped.
 */
/*
 * 本文件实现 Linux 的 NUMA 内存策略：用户可以分别为线程和 VMA 指定页应从哪些
 * NUMA 节点分配。缺页时 VMA 策略优先于线程策略；中断上下文不采用进程策略。
 *
 * 六类策略的核心语义如下：INTERLEAVE 按节点轮转；WEIGHTED_INTERLEAVE 按权重
 * 轮转；BIND 将分配限制在给定节点集合；PREFERRED 先尝试单个首选节点；
 * PREFERRED_MANY 先尝试一组节点；DEFAULT/LOCAL 回到本地节点或上层默认策略。
 * 失败回退能力由具体模式决定。共享 shmem/tmpfs 的策略保存在共享策略树中，即使
 * 当前没有映射者也不会随某个进程退出而消失。
 *
 * 阅读主线可分为五段：用户掩码解析 -> mempolicy 对象构造与绑定 -> VMA/任务策略
 * 安装 -> 缺页或显式迁移时选择目标节点 -> 引用计数、RCU 或共享策略树回收。
 * 页分配只对内存层级中最高的 zone 应用策略，低端内存和 DMA 请求沿用默认策略；
 * 交换路径仍有少数不保存进程策略的历史边界。
 */

/* Notebook:
   fix mmap readahead to honour policy and enable policy for any page cache
   object
   statistics for bigpages
   global policy for page cache? currently it uses process policy. Requires
   first item above.
   handle mremap for shared memory (currently ignored for the policy)
   grows down?
   make bind policy root only? It can trigger oom much faster and the
   kernel is not always grateful with that.
*/
/*
 * 上述 Notebook 是上游保留的待办清单：包括让 mmap 预读/任意 page-cache 对象遵循
 * 策略、统计大页、考虑全局 page-cache 策略、补全共享内存的 mremap 与向下增长
 * 语义，以及评估 BIND 是否需要特权。它们描述的是尚未完全解决的边界，不应被当成
 * 当前实现已经提供的保证。
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mempolicy.h>
#include <linux/pagewalk.h>
#include <linux/highmem.h>
#include <linux/hugetlb.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/numa_balancing.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/task.h>
#include <linux/nodemask.h>
#include <linux/cpuset.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/export.h>
#include <linux/nsproxy.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/compat.h>
#include <linux/ptrace.h>
#include <linux/swap.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/memory-tiers.h>
#include <linux/migrate.h>
#include <linux/ksm.h>
#include <linux/rmap.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/ctype.h>
#include <linux/mm_inline.h>
#include <linux/mmu_notifier.h>
#include <linux/printk.h>
#include <linux/leafops.h>
#include <linux/gcd.h>

#include <asm/tlbflush.h>
#include <asm/tlb.h>
#include <linux/uaccess.h>
#include <linux/memory.h>

#include "internal.h"

/* Internal flags */
/*
 * 这些位只在内核内部的 mbind/页表扫描协议中流转，不属于用户 ABI：DISCONTIG_OK
 * 允许跨不连续 VMA，INVERT 把节点测试反转为“选中掩码之外的页”，WRLOCK 记录
 * walker 已经按写方式锁定 VMA。把它们放在 MPOL_MF_INTERNAL 以上可避免与用户位冲突。
 */
#define MPOL_MF_DISCONTIG_OK (MPOL_MF_INTERNAL << 0)	/* Skip checks for continuous vmas */
/* 允许请求地址范围跨越 VMA 空洞，walker 不再要求映射连续。 */
#define MPOL_MF_INVERT       (MPOL_MF_INTERNAL << 1)	/* Invert check for nodemask */
/* 反转节点掩码测试，用于收集“不在目标集合”中的错位 folio。 */
#define MPOL_MF_WRLOCK       (MPOL_MF_INTERNAL << 2)	/* Write-lock walked vmas */
/* 告知页表 walker 以写方式锁定 VMA，供扫描后立即修改策略的 mbind 路径使用。 */

static struct kmem_cache *policy_cache;
static struct kmem_cache *sn_cache;
/*
 * policy_cache 分配独立 struct mempolicy；sn_cache 分配共享策略树的 sp_node。
 * 两者在 numa_policy_init() 创建，随后由本文件所有构造/销毁路径共同使用。
 */

/* Highest zone. An specific allocation for a zone below that is not
   policied. */
/*
 * policy_zone 是允许应用 NUMA 策略的最高 zone 阈值；请求更低 zone 的分配绕开策略，
 * 避免把 DMA/低端内存等受硬件约束的稀缺区域错误限制到用户节点集合中。
 */
enum zone_type policy_zone = 0;

/*
 * run-time system-wide default policy => local allocation
 */
/*
 * 运行期全局兜底对象代表本地分配。它是静态常驻对象，refcnt 的 1 永不释放；调用者
 * 只借用该指针，不能把它当成普通动态策略交给最终释放路径。
 */
static struct mempolicy default_policy = {
	.refcnt = ATOMIC_INIT(1), /* never free it */
	/* 静态对象永久保留这一基准引用，因此不会进入 __mpol_put() 的 RCU 回收。 */
	.mode = MPOL_LOCAL,
};

static struct mempolicy preferred_node_policy[MAX_NUMNODES];
/*
 * 每节点首选策略在启动后初始化为指向对应 nid，供没有显式线程策略的内核调用者快速
 * 取得“首选当前节点”对象；数组元素同样为静态对象，只能借用。
 */

/*
 * weightiness balances the tradeoff between small weights (cycles through nodes
 * faster, more fair/even distribution) and large weights (smaller errors
 * between actual bandwidth ratios and weight ratios). 32 is a number that has
 * been found to perform at a reasonable compromise between the two goals.
 */
/*
 * 权重越小，遍历节点一轮越快、短期分布越均匀；权重越大，实际带宽比例误差越小。
 * 经验值 32 在两项目标之间取得合理折中，后续 GCD 约分还会消除不必要的共同倍数。
 */
static const int weightiness = 32;
/*
 * 上游说明中的 32 是精度与轮转周期的经验折中：权重越小越快完成一轮、分布更平滑，
 * 权重越大越接近原始带宽比例。后续还会除以最大公约数，避免等价比例拉长周期。
 */

/*
 * A null weighted_interleave_state is interpreted as having .mode="auto",
 * and .iw_table is interpreted as an array of 1s with length nr_node_ids.
 */
/* NULL 状态等价于 mode_auto=true 且长度 nr_node_ids 的权重表全为 1。 */
struct weighted_interleave_state {
	bool mode_auto;
	u8 iw_table[];
};
/*
 * weighted_interleave_state 是一次不可变发布的权重快照：mode_auto 区分由带宽自动推导
 * 与 sysfs 手工设置；柔性数组按 nid 保存 1..255 的权重。写者构造完整新快照后通过
 * RCU 替换，读者在 RCU 临界区借用，旧快照等待宽限期后释放。
 */
static struct weighted_interleave_state __rcu *wi_state;
static unsigned int *node_bw_table;
/*
 * wi_state 是分配热路径读取的 RCU 指针；NULL 等价于自动模式且所有节点权重为 1。
 * node_bw_table 是写侧维护的原始带宽数据，不发布给无锁读者，用于自动模式重新计算。
 */

/*
 * wi_state_lock protects both wi_state and node_bw_table.
 * node_bw_table is only used by writers to update wi_state.
 */
/*
 * wi_state_lock 串行化两份状态的写入；读侧只通过 RCU 读取已经完整发布的 wi_state，
 * 不访问 node_bw_table。互斥锁允许分配新表和复制数据时睡眠，RCU 则避免页分配热路径
 * 被该互斥锁阻塞。
 */
static DEFINE_MUTEX(wi_state_lock);

/*
 * get_il_weight() - 读取一个 NUMA 节点当前的加权交错权重
 * 调用位置：weighted_interleave_*() -> get_il_weight()；页分配热路径据此推进轮转。
 * @node: 有效 nid，作为 iw_table 下标；调用者保证小于 nr_node_ids。
 * 上下文：不要求持 wi_state_lock，不睡眠；函数自行进入 RCU 读侧临界区。
 * 返回：已发布快照中的 u8 权重；wi_state 尚未建立时返回默认值 1。返回后不保留快照
 * 指针或引用，因此旧对象可在随后的 RCU 宽限期结束后被写者释放。
 */
static u8 get_il_weight(int node)
{
	/* state 仅在 RCU 窗口内借用；weight 把所需标量复制出来以越过该生命周期边界。 */
	struct weighted_interleave_state *state;
	u8 weight = 1;

	/* rcu_dereference 与写侧 rcu_assign_pointer 配对，保证先看到初始化完整的表内容。 */
	rcu_read_lock();
	state = rcu_dereference(wi_state);
	if (state)
		weight = state->iw_table[node];
	rcu_read_unlock();
	return weight;
}

/*
 * Convert bandwidth values into weighted interleave weights.
 * Call with wi_state_lock.
 */
/*
 * reduce_interleave_weights() - 把各节点带宽快照压缩为短周期整数权重
 * 调用者：mempolicy_set_node_perf()；调用前必须持有 wi_state_lock，输入/输出数组均为
 * 写者私有，函数不发布指针且不睡眠。
 * @bw: 每节点原始带宽，只读借用；0 表示尚无有效带宽贡献。
 * @new_iw: 每节点 u8 输出表，由调用者分配并最终随新 wi_state 发布。
 * 返回：无直接返回值。先按总带宽缩放到约 1..weightiness，再除以全部权重的最大公约
 * 数；这样保持比例的同时缩短轮转周期。调用者随后负责 RCU 发布。
 */
static void reduce_interleave_weights(unsigned int *bw, u8 *new_iw)
{
	/* sum_bw 是所有有内存节点的总带宽；其余变量分别承载安全降位、缩放值和累计 GCD。 */
	u64 sum_bw = 0;
	unsigned int cast_sum_bw, scaling_factor = 1, iw_gcd = 0;
	int nid;

	for_each_node_state(nid, N_MEMORY)
		sum_bw += bw[nid];

	/* Scale bandwidths to whole numbers in the range [1, weightiness] */
	/* 将带宽缩放为 [1, weightiness] 的整数；最小 1 保证节点仍能参与轮转。 */
	for_each_node_state(nid, N_MEMORY) {
		/*
		 * Try not to perform 64-bit division.
		 * If sum_bw < scaling_factor, then sum_bw < U32_MAX.
		 * If sum_bw > scaling_factor, then round the weight up to 1.
		 */
		/*
		 * 尽量避免 64 位除法：当总和可安全降为 u32 时才做整数除法；否则把极小份额
		 * 向上钳到 1。iw_gcd 在同一遍扫描中累计，供下一阶段约分。
		 */
		scaling_factor = weightiness * bw[nid];
		if (bw[nid] && sum_bw < scaling_factor) {
			cast_sum_bw = (unsigned int)sum_bw;
			new_iw[nid] = scaling_factor / cast_sum_bw;
		} else {
			new_iw[nid] = 1;
		}
		if (!iw_gcd)
			iw_gcd = new_iw[nid];
		iw_gcd = gcd(iw_gcd, new_iw[nid]);
	}

	/* 1:2 is strictly better than 16:32. Reduce by the weights' GCD. */
	/* 1:2 与 16:32 比例相同但周期更短，因此用最大公约数约分而不改变长期份额。 */
	for_each_node_state(nid, N_MEMORY)
		new_iw[nid] /= iw_gcd;
}

/*
 * mempolicy_set_node_perf() - 用节点性能坐标更新自动 weighted-interleave 权重
 * 调用位置：内存层级/性能数据更新 -> 本函数 -> reduce_interleave_weights() -> RCU
 * 发布；手工模式下只缓存带宽，暂不替换用户权重。
 * @node: 要更新的有效 nid；调用者保证可作为 nr_node_ids 长数组的下标。
 * @coords: 借用的访问性能坐标，不能为空；读取读/写带宽，调用后所有权不变。
 * 上下文：进程上下文，可睡眠；入口不持 wi_state_lock，本函数内部获取该互斥锁。
 * 返回：0 表示带宽缓存已更新（自动模式还完成权重发布），-ENOMEM 表示新带宽表或新
 * 权重快照分配失败且旧状态未变。成功替换后等待 RCU 宽限期再释放旧快照。
 */
int mempolicy_set_node_perf(unsigned int node, struct access_coordinate *coords)
{
	/* 新对象在锁外构造；old_* 在换代后由当前函数持有并负责释放。bw_val 取双向瓶颈。 */
	struct weighted_interleave_state *new_wi_state, *old_wi_state = NULL;
	unsigned int *old_bw, *new_bw;
	unsigned int bw_val;
	int i;

	bw_val = min(coords->read_bandwidth, coords->write_bandwidth);
	/* 阶段 1：预分配完整替代对象，避免持有全局互斥锁时进入内存回收慢路径。 */
	new_bw = kcalloc(nr_node_ids, sizeof(unsigned int), GFP_KERNEL);
	if (!new_bw)
		return -ENOMEM;

	new_wi_state = kmalloc_flex(*new_wi_state, iw_table, nr_node_ids);
	if (!new_wi_state) {
		kfree(new_bw);
		return -ENOMEM;
	}
	new_wi_state->mode_auto = true;
	for (i = 0; i < nr_node_ids; i++)
		new_wi_state->iw_table[i] = 1;

	/*
	 * Update bandwidth info, even in manual mode. That way, when switching
	 * to auto mode in the future, iw_table can be overwritten using
	 * accurate bw data.
	 */
	/*
	 * 即使当前是手工模式也更新原始带宽缓存；未来切回自动模式时便能立即根据最新
	 * 观测重建 iw_table，而不是继续使用切换前的陈旧比例。
	 */
	mutex_lock(&wi_state_lock);

	old_bw = node_bw_table;
	if (old_bw)
		memcpy(new_bw, old_bw, nr_node_ids * sizeof(*old_bw));
	new_bw[node] = bw_val;
	node_bw_table = new_bw;

	old_wi_state = rcu_dereference_protected(wi_state,
					lockdep_is_held(&wi_state_lock));
	if (old_wi_state && !old_wi_state->mode_auto) {
		/* Manual mode; skip reducing weights and updating wi_state */
		/* 手工模式保留现有 RCU 快照，只提交 new_bw；未发布的新权重对象仍归本函数。 */
		mutex_unlock(&wi_state_lock);
		kfree(new_wi_state);
		goto out;
	}

	/* NULL wi_state assumes auto=true; reduce weights and update wi_state*/
	/* NULL 依契约等价于自动全 1；计算新权重后一次性发布完整快照。 */
	reduce_interleave_weights(new_bw, new_wi_state->iw_table);
	rcu_assign_pointer(wi_state, new_wi_state);

	mutex_unlock(&wi_state_lock);
	/*
	 * 解除写锁后等待先前读者离开，才能释放 old_wi_state；引用计数没有参与此协议，
	 * 其生命周期完全由 RCU 保证。old_bw 从未向无锁读者发布，可以直接释放。
	 */
	if (old_wi_state) {
		synchronize_rcu();
		kfree(old_wi_state);
	}
out:
	kfree(old_bw);
	return 0;
}

/**
 * numa_nearest_node - Find nearest node by state
 * @node: Node id to start the search
 * @state: State to filter the search
 *
 * Lookup the closest node by distance if @nid is not in state.
 *
 * Return: this @node if it is in state, otherwise the closest node by distance
 */
/*
 * numa_nearest_node() - 在指定节点状态集合中选择离起点最近的节点
 * 调用者包括内存层级与 NUMA 回退路径；本函数只查询 node_states 和距离矩阵。
 * @node: 搜索起点；NUMA_NO_NODE 原样返回，合法 nid 若已属于集合也走快速返回。
 * @state: enum node_states 索引，必须小于 NR_NODE_STATES。
 * 上下文：无锁只读、不睡眠；节点状态由内核热插拔协议维护。
 * 返回：非法 state 返回 -EINVAL；否则返回起点或距离最小的候选 nid。若集合为空，
 * 当前实现保留初始 node，调用者必须结合所请求 state 的非空前置条件理解结果。
 */
int numa_nearest_node(int node, unsigned int state)
{
	/* min_dist/min_node 保存当前最优解，n/dist 是扫描候选及其 NUMA 距离。 */
	int min_dist = INT_MAX, dist, n, min_node;

	if (state >= NR_NODE_STATES)
		return -EINVAL;

	if (node == NUMA_NO_NODE || node_state(node, state))
		return node;

	min_node = node;
	/* 阶段 2：线性扫描状态集合；相同距离保留先遇到的节点，结果稳定但不做负载均衡。 */
	for_each_node_state(n, state) {
		dist = node_distance(node, n);
		if (dist < min_dist) {
			min_dist = dist;
			min_node = n;
		}
	}

	return min_node;
}
EXPORT_SYMBOL_GPL(numa_nearest_node);

/**
 * nearest_node_nodemask - Find the node in @mask at the nearest distance
 *			   from @node.
 *
 * @node: a valid node ID to start the search from.
 * @mask: a pointer to a nodemask representing the allowed nodes.
 *
 * This function iterates over all nodes in @mask and calculates the
 * distance from the starting @node, then it returns the node ID that is
 * the closest to @node, or MAX_NUMNODES if no node is found.
 *
 * Note that @node must be a valid node ID usable with node_distance(),
 * providing an invalid node ID (e.g., NUMA_NO_NODE) may result in crashes
 * or unexpected behavior.
 */
/*
 * nearest_node_nodemask() - 在调用者给定的节点掩码中寻找离起点最近的 nid
 * @node: 必须能安全传给 node_distance() 的有效起点，不能是 NUMA_NO_NODE。
 * @mask: 只读借用、不能为空；函数不修改掩码，也不取得长期引用。
 * 上下文：不睡眠、不加锁；调用者负责保证掩码与热插拔语义相容。
 * 返回：最近节点；空掩码时返回 MAX_NUMNODES 哨兵。与 numa_nearest_node() 不同，
 * 这里由任意 nodemask 决定候选集，并明确提供“未找到”的哨兵。
 */
int nearest_node_nodemask(int node, nodemask_t *mask)
{
	/* min_node 以越界哨兵开始；只有真正访问到候选节点才会被有效 nid 覆盖。 */
	int dist, n, min_dist = INT_MAX, min_node = MAX_NUMNODES;

	for_each_node_mask(n, *mask) {
		dist = node_distance(node, n);
		if (dist < min_dist) {
			min_dist = dist;
			min_node = n;
		}
	}

	return min_node;
}
EXPORT_SYMBOL_GPL(nearest_node_nodemask);

/*
 * get_task_policy() - 取得任务分配时应使用的有效策略
 * 调用位置：通用页分配/外部模块 -> 本函数；显式 task->mempolicy 优先，否则按当前
 * CPU 节点返回静态首选策略，启动早期未初始化时退回 default_policy。
 * @p: 借用的 task_struct，不能为空；调用者负责其生命周期和对 mempolicy 的并发约束。
 * 上下文：不睡眠、不增加策略引用；返回指针只能在调用者已有的任务/策略保护范围内使用。
 * 返回：动态任务策略或静态全局策略的借用指针，永不返回 NULL；本函数无可观察写副作用。
 */
struct mempolicy *get_task_policy(struct task_struct *p)
{
	/* pol 是借用策略，node 是当前执行 CPU 的 nid；迁移到其他 CPU 后需重新求值。 */
	struct mempolicy *pol = p->mempolicy;
	int node;

	if (pol)
		return pol;

	node = numa_node_id();
	if (node != NUMA_NO_NODE) {
		pol = &preferred_node_policy[node];
		/* preferred_node_policy is not initialised early in boot */
		/* 启动早期数组尚未建立 mode，必须回退，不能把零初始化对象误当有效策略。 */
		if (pol->mode)
			return pol;
	}

	return &default_policy;
}
EXPORT_SYMBOL_FOR_MODULES(get_task_policy, "kvm");

static const struct mempolicy_operations {
	int (*create)(struct mempolicy *pol, const nodemask_t *nodes);
	void (*rebind)(struct mempolicy *pol, const nodemask_t *nodes);
} mpol_ops[MPOL_MAX];
/*
 * mempolicy_operations 是按策略模式分派的构造/重绑定接口：create 把有效节点集写入新
 * 策略，rebind 在 cpuset 节点集合改变时修复现有策略。mpol_ops 是静态不可变表，函数
 * 指针不转移对象所有权；各回调的同步由上层 mpol_set_nodemask()/rebind 路径提供。
 */

/*
 * mpol_store_user_nodemask() - 判断策略是否必须保存用户原始掩码
 * 调用位置：mpol_set_nodemask()/mpol_rebind_policy()/do_get_mempolicy() 的表示选择阶段。
 * @pol: 只读借用的有效策略。返回非零表示 STATIC/RELATIVE 语义要求重绑定时从用户
 * 掩码重新计算；返回 0 表示可保存当时的 cpuset 有效集合。纯字段读取，不睡眠无副作用。
 */
static inline int mpol_store_user_nodemask(const struct mempolicy *pol)
{
	return pol->flags & MPOL_USER_NODEMASK_FLAGS;
}

/*
 * mpol_relative_nodemask() - 把相对序号掩码投影到当前允许节点集合
 * 调用位置：策略首次绑定或 cpuset 重绑定 -> 本函数 -> nodes_fold()/nodes_onto()。
 * @ret: 输出掩码，调用后被完整覆盖；@orig: 用户相对位图；@rel: 当前允许节点集合。
 * 三者均由调用者持有；函数不睡眠。先按 rel 的节点数折叠 orig，再把位序映射到 rel
 * 中实际 nid，从而在 cpuset 改变后保持“第几个允许节点”的相对含义。
 * 返回：无直接返回值；成功出口以 @ret 完整输出，输入掩码与其所有权均不改变。
 */
static void mpol_relative_nodemask(nodemask_t *ret, const nodemask_t *orig,
				   const nodemask_t *rel)
{
	nodemask_t tmp;
	nodes_fold(tmp, *orig, nodes_weight(*rel));
	nodes_onto(*ret, tmp, *rel);
}

/*
 * mpol_new_nodemask() - 为需要完整节点集合的模式安装已过滤掩码
 * 调用位置：mpol_ops[].create，由 mpol_set_nodemask() 在新策略发布前间接调用。
 * @pol: 新策略的输入输出对象；@nodes: 只读借用的有效候选集合。
 * 返回：空集合为 -EINVAL，否则复制到 pol->nodes 并返回 0；不分配内存、不睡眠。
 */
static int mpol_new_nodemask(struct mempolicy *pol, const nodemask_t *nodes)
{
	if (nodes_empty(*nodes))
		return -EINVAL;
	pol->nodes = *nodes;
	return 0;
}

/*
 * mpol_new_preferred() - 从非空候选集合选择第一个节点作为单节点首选策略
 * 调用位置：MPOL_PREFERRED 的 mpol_ops[].create；入口对象尚未发布，可原地初始化。
 * @pol: 新策略输入输出对象；@nodes: 已经过 cpuset/N_MEMORY 过滤的只读集合。
 * 返回：空集合为 -EINVAL；成功清空旧值并只设置 first_node，返回 0。选择顺序按 nid，
 * 并非距离或负载排序。
 * 上下文：调用者已按 alloc_lock 或 mmap 写锁稳定节点约束；本函数不睡眠、不持有引用。
 */
static int mpol_new_preferred(struct mempolicy *pol, const nodemask_t *nodes)
{
	if (nodes_empty(*nodes))
		return -EINVAL;

	nodes_clear(pol->nodes);
	node_set(first_node(*nodes), pol->nodes);
	return 0;
}

/*
 * mpol_set_nodemask is called after mpol_new() to set up the nodemask, if
 * any, for the new policy.  mpol_new() has already validated the nodes
 * parameter with respect to the policy mode and flags.
 *
 * Must be called holding task's alloc_lock to protect task's mems_allowed
 * and mempolicy.  May also be called holding the mmap_lock for write.
 */
/*
 * mpol_set_nodemask() - 将用户节点语义约束到当前 cpuset 与在线内存节点并完成策略构造
 * 调用位置：mpol_new() 后、策略发布前；任务策略路径持 alloc_lock，VMA 路径可持 mmap
 * 写锁，二者都保证 cpuset/mempolicy 组合不会在构造中途被并发观察。
 * @pol: 新策略输入输出；NULL/LOCAL 无节点状态，立即成功。
 * @nodes: 用户掩码的只读借用；非 LOCAL 策略必须非空指针。
 * @nsc: 调用者提供的临时双 nodemask，避免大栈对象；内容在返回后无契约。
 * 返回：0 或模式 create 回调的 -EINVAL；成功时 pol 保存有效节点及重绑定所需原始信息。
 */
static int mpol_set_nodemask(struct mempolicy *pol,
		     const nodemask_t *nodes, struct nodemask_scratch *nsc)
{
	/* ret 传递具体模式构造结果；nsc->mask1/2 分别是允许集合和最终投影集合。 */
	int ret;

	/*
	 * Default (pol==NULL) resp. local memory policies are not a
	 * subject of any remapping. They also do not need any special
	 * constructor.
	 */
	/* DEFAULT(NULL) 与 LOCAL 不携带需重映射的节点集合，也没有模式专用构造工作。 */
	if (!pol || pol->mode == MPOL_LOCAL)
		return 0;

	/* Check N_MEMORY */
	/* 阶段 2：同时取当前 cpuset 允许节点与真实有内存节点，排除无法承载分配的 nid。 */
	nodes_and(nsc->mask1,
		  cpuset_current_mems_allowed, node_states[N_MEMORY]);

	VM_BUG_ON(!nodes);

	if (pol->flags & MPOL_F_RELATIVE_NODES)
		mpol_relative_nodemask(&nsc->mask2, nodes, &nsc->mask1);
	else
		nodes_and(nsc->mask2, *nodes, nsc->mask1);

	if (mpol_store_user_nodemask(pol))
		/* STATIC/RELATIVE 必须保留用户表达，未来 cpuset 变化时才能重新解释。 */
		pol->w.user_nodemask = *nodes;
	else
		pol->w.cpuset_mems_allowed = cpuset_current_mems_allowed;

	ret = mpol_ops[pol->mode].create(pol, &nsc->mask2);
	/* create 只初始化尚未发布的对象；失败时释放责任仍在上层调用者。 */
	return ret;
}

/*
 * This function just creates a new policy, does some check and simple
 * initialization. You must invoke mpol_set_nodemask() to set nodes.
 */
/*
 * mpol_new() - 校验模式/标志组合并分配一个尚未绑定节点的策略对象
 * @mode: MPOL_DEFAULT..MPOL_MAX-1 中已由上层校验的模式；PREFERRED 空集会规范化为 LOCAL。
 * @flags: 策略持久标志；STATIC/RELATIVE 与 LOCAL/空 PREFERRED 的组合受严格限制。
 * @nodes: 用户节点集合；DEFAULT 可为 NULL，其他模式必须指向可读 nodemask。
 * 上下文：进程上下文，可因 slab 分配睡眠；不要求持锁，对象尚未发布。
 * 返回：DEFAULT 合法请求返回 NULL 表示继承；成功返回 refcnt=1 的持有指针；失败返回
 * ERR_PTR(-EINVAL/-ENOMEM)。调用者必须继续 mpol_set_nodemask()，失败则 mpol_put()。
 */
static struct mempolicy *mpol_new(unsigned short mode, unsigned short flags,
				  nodemask_t *nodes)
{
	/* policy 在成功分配后归调用者独占；在此之前所有失败都不产生需回滚对象。 */
	struct mempolicy *policy;

	if (mode == MPOL_DEFAULT) {
		/* 阶段 1：DEFAULT 以 NULL 表示继承，只允许空节点表达；此快速路径不分配对象。 */
		if (nodes && !nodes_empty(*nodes))
			return ERR_PTR(-EINVAL);
		return NULL;
	}
	VM_BUG_ON(!nodes);

	/*
	 * MPOL_PREFERRED cannot be used with MPOL_F_STATIC_NODES or
	 * MPOL_F_RELATIVE_NODES if the nodemask is empty (local allocation).
	 * All other modes require a valid pointer to a non-empty nodemask.
	 */
	/*
	 * PREFERRED 的空集合表示“当前本地节点”，但 STATIC/RELATIVE 都需要可重解释的
	 * 用户位图，故不能与该特殊语义组合。LOCAL 则必须完全不携带节点约束。
	 */
	if (mode == MPOL_PREFERRED) {
		if (nodes_empty(*nodes)) {
			if (((flags & MPOL_F_STATIC_NODES) ||
			     (flags & MPOL_F_RELATIVE_NODES)))
				return ERR_PTR(-EINVAL);

			mode = MPOL_LOCAL;
		}
		/* 非空 PREFERRED 保留候选集合；空集已在上面规范化为 LOCAL。 */
	} else if (mode == MPOL_LOCAL) {
		if (!nodes_empty(*nodes) ||
		    (flags & MPOL_F_STATIC_NODES) ||
		    (flags & MPOL_F_RELATIVE_NODES))
			return ERR_PTR(-EINVAL);
	} else if (nodes_empty(*nodes))
		return ERR_PTR(-EINVAL);
	/* 至此模式、空掩码与持久标志组合均合法，才进入可能睡眠的 slab 分配阶段。 */

	policy = kmem_cache_alloc(policy_cache, GFP_KERNEL);
	/* 阶段 2：只初始化对象公共头；具体 nodes 联合字段由 mpol_set_nodemask() 填充。 */
	if (!policy)
		return ERR_PTR(-ENOMEM);
	atomic_set(&policy->refcnt, 1);
	policy->mode = mode;
	policy->flags = flags;
	policy->home_node = NUMA_NO_NODE;

	return policy;
}

/* Slow path of a mpol destructor. */
/*
 * __mpol_put() - mempolicy 引用计数归零时执行 RCU 延迟销毁
 * 这是内联 mpol_put() 的慢路径；@pol 是调用者持有的一份动态策略引用，不能为空，也
 * 不能是静态 default/preferred_node 对象的永久基准引用。
 * 上下文：原子递减本身不睡眠；最终释放排入 RCU，允许 speculative mmap-lock 读者越过
 * 常规锁边界仍安全访问对象内存。返回无直接值；未归零仅交还当前引用，归零则禁止再用。
 */
void __mpol_put(struct mempolicy *pol)
{
	if (!atomic_dec_and_test(&pol->refcnt))
		return;
	/*
	 * Required to allow mmap_lock_speculative*() access, see for example
	 * futex_key_to_node_opt(). All accesses are serialized by mmap_lock,
	 * however the speculative lock section unbound by the normal lock
	 * boundaries, requiring RCU freeing.
	 */
	/*
	 * mmap_lock 序列化字段访问，但 speculative 读段可能跨越普通锁边界；因此引用归零
	 * 只能摘除逻辑所有权，实际内存必须等既有 RCU 读者退出后再释放。
	 */
	kfree_rcu(pol, rcu);
}
EXPORT_SYMBOL_FOR_MODULES(__mpol_put, "kvm");

/*
 * mpol_rebind_default() - DEFAULT/LOCAL 的空重绑定回调
 * @pol/@nodes 均为借用且不使用；这两种模式不保存节点集，所以 cpuset 改变不产生状态
 * 修改。返回无直接值、不睡眠、无副作用。
 */
static void mpol_rebind_default(struct mempolicy *pol, const nodemask_t *nodes)
{
}

/*
 * mpol_rebind_nodemask() - 在允许节点集合变化后重算多节点策略的有效掩码
 * 调用位置：cpuset 更新 -> mpol_rebind_policy() -> 本模式回调；不更换策略对象。
 * @pol: 输入输出策略，调用者以 alloc_lock 或 mmap 写锁保护；@nodes: 新允许集合。
 * STATIC 取用户绝对掩码交集，RELATIVE 重新投影相对位，普通模式按旧/新 cpuset 映射；
 * 若结果为空则退回全部新允许节点，避免策略永久不可分配。返回无直接值、不分配内存。
 */
static void mpol_rebind_nodemask(struct mempolicy *pol, const nodemask_t *nodes)
{
	nodemask_t tmp;

	/* 阶段 1：按策略最初表达方式求新有效集合，三种分支不能混用各自的基准字段。 */
	if (pol->flags & MPOL_F_STATIC_NODES)
		nodes_and(tmp, pol->w.user_nodemask, *nodes);
	else if (pol->flags & MPOL_F_RELATIVE_NODES)
		mpol_relative_nodemask(&tmp, &pol->w.user_nodemask, nodes);
	else {
		nodes_remap(tmp, pol->nodes, pol->w.cpuset_mems_allowed,
								*nodes);
		pol->w.cpuset_mems_allowed = *nodes;
	}

	if (nodes_empty(tmp))
		/* 交集为空时退化为全部新允许节点，保证任务仍有可分配目标而不会伪造 OOM。 */
		tmp = *nodes;

	pol->nodes = tmp;
}

/*
 * mpol_rebind_preferred() - 更新首选类策略记录的 cpuset 基线
 * 调用位置：cpuset 更新 -> mpol_rebind_policy() -> PREFERRED/PREFERRED_MANY 回调。
 * @pol: 受上层锁保护的输入输出策略；@nodes: 新允许集合。首选 nid 本身由后续分配回退
 * 逻辑解释，这里只保存重绑定基线；返回无直接值、不睡眠。
 */
static void mpol_rebind_preferred(struct mempolicy *pol,
						const nodemask_t *nodes)
{
	pol->w.cpuset_mems_allowed = *nodes;
}

/*
 * mpol_rebind_policy - Migrate a policy to a different set of nodes
 *
 * Per-vma policies are protected by mmap_lock. Allocations using per-task
 * policies are protected by task->mems_allowed_seq to prevent a premature
 * OOM/allocation failure due to parallel nodemask modification.
 */
/*
 * mpol_rebind_policy() - 把已有策略迁移到新的允许节点集合
 * 调用位置：任务/VMA cpuset 重绑定和 __mpol_dup() 的上下文化阶段。
 * @pol: 可为 NULL 的输入输出策略；@newmask: 新集合只读借用。
 * VMA 策略由 mmap_lock 保护；任务策略的写侧与 mems_allowed_seq 配合分配读侧重试，避免
 * 读者看到半更新掩码后误判 OOM。LOCAL/NULL 和未变化的普通策略走快速返回。
 * 返回无直接值；具体字段更新由模式 rebind 回调完成，不改变策略引用所有权。
 * 上下文：调用者持 task alloc_lock、mmap 写锁，或独占尚未发布的副本；本函数不睡眠。
 */
static void mpol_rebind_policy(struct mempolicy *pol, const nodemask_t *newmask)
{
	if (!pol || pol->mode == MPOL_LOCAL)
		return;
	if (!mpol_store_user_nodemask(pol) &&
	    nodes_equal(pol->w.cpuset_mems_allowed, *newmask))
		return;

	mpol_ops[pol->mode].rebind(pol, newmask);
}

/*
 * Wrapper for mpol_rebind_policy() that just requires task
 * pointer, and updates task mempolicy.
 *
 * Called with task's alloc_lock held.
 */
/*
 * mpol_rebind_task() - 在 task alloc_lock 下重绑定该任务的显式策略
 * @tsk: 借用任务，调用者保证存活并持 alloc_lock；@new: 新 mems_allowed 只读集合。
 * 返回无直接值；只修改 tsk->mempolicy 指向对象的节点字段，不替换指针或引用。
 * 调用位置：cpuset 写侧更新任务 mems_allowed 后；上下文不可在此睡眠，锁由调用者释放。
 */
void mpol_rebind_task(struct task_struct *tsk, const nodemask_t *new)
{
	mpol_rebind_policy(tsk->mempolicy, new);
}

/*
 * Rebind each vma in mm to new nodemask.
 *
 * Call holding a reference to mm.  Takes mm->mmap_lock during call.
 */
/*
 * mpol_rebind_mm() - 在 cpuset 变化后重绑定 mm 中每个 VMA 的策略
 * @mm: 调用者持有引用的地址空间；@new: 新允许节点集合，只读借用。
 * 本函数取得 mmap 写锁并逐 VMA 调用 vma_start_write()，因此可睡眠；成功返回前所有当前
 * VMA 策略均已重算。返回无直接值，不更改 mm/new 所有权，也不迁移已经存在的页。
 */
void mpol_rebind_mm(struct mm_struct *mm, nodemask_t *new)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, 0);

	mmap_write_lock(mm);
	/* 写锁稳定 VMA 集合；vma_start_write 为 maple-tree/VMA 写协议标记当前修改者。 */
	for_each_vma(vmi, vma) {
		vma_start_write(vma);
		mpol_rebind_policy(vma->vm_policy, new);
	}
	mmap_write_unlock(mm);
}

static const struct mempolicy_operations mpol_ops[MPOL_MAX] = {
	/*
	 * 模式分派表：DEFAULT/LOCAL 无构造数据；INTERLEAVE/BIND/WEIGHTED 保存完整集合；
	 * PREFERRED 只选择首节点；PREFERRED_MANY 保存集合但采用首选类重绑定语义。
	 */
	[MPOL_DEFAULT] = {
		.rebind = mpol_rebind_default,
	},
	[MPOL_INTERLEAVE] = {
		.create = mpol_new_nodemask,
		.rebind = mpol_rebind_nodemask,
	},
	[MPOL_PREFERRED] = {
		.create = mpol_new_preferred,
		.rebind = mpol_rebind_preferred,
	},
	[MPOL_BIND] = {
		.create = mpol_new_nodemask,
		.rebind = mpol_rebind_nodemask,
	},
	[MPOL_LOCAL] = {
		.rebind = mpol_rebind_default,
	},
	[MPOL_PREFERRED_MANY] = {
		.create = mpol_new_nodemask,
		.rebind = mpol_rebind_preferred,
	},
	[MPOL_WEIGHTED_INTERLEAVE] = {
		.create = mpol_new_nodemask,
		.rebind = mpol_rebind_nodemask,
	},
};

static bool migrate_folio_add(struct folio *folio, struct list_head *foliolist,
				unsigned long flags);
static nodemask_t *policy_nodemask(gfp_t gfp, struct mempolicy *pol,
				pgoff_t ilx, int *nid);
/* 上述前置声明连接页表扫描与后文分配选择实现；这里只声明，不取得任何对象所有权。 */

/*
 * strictly_unmovable() - 判断 mbind 标志是否要求发现错位页即失败而不迁移
 * @flags: MPOL_MF_* 位图。仅 STRICT 且没有 MOVE/MOVE_ALL 时返回 true；调用者据此把
 * 扫描到的错位页计为 -EIO，而不是隔离到迁移列表。纯位运算、不睡眠、无副作用。
 */
static bool strictly_unmovable(unsigned long flags)
{
	/*
	 * STRICT without MOVE flags lets do_mbind() fail immediately with -EIO
	 * if any misplaced page is found.
	 */
	/* 仅 STRICT 而无 MOVE 位时，发现任一错位页就让 do_mbind 以 -EIO 失败。 */
	return (flags & (MPOL_MF_STRICT | MPOL_MF_MOVE | MPOL_MF_MOVE_ALL)) ==
			 MPOL_MF_STRICT;
}

struct migration_mpol {		/* for alloc_migration_target_by_mpol() */
	/* 本结构专供 alloc_migration_target_by_mpol() 的 private 回调上下文。 */
	struct mempolicy *pol;
	pgoff_t ilx;
};
/*
 * migration_mpol 是迁移目标分配回调的短生命周期上下文：pol 为借用策略，ilx 是文件页
 * 或匿名映射中的交错索引。调用者在同步迁移完成前保持二者有效，结构本身不持引用。
 */

struct queue_pages {
	struct list_head *pagelist;
	unsigned long flags;
	nodemask_t *nmask;
	unsigned long start;
	unsigned long end;
	struct vm_area_struct *first;
	struct folio *large;		/* note last large folio encountered */
	/* 记录最近遇到的大 folio，避免同一对象经多个 PTE 被重复隔离或重复计错。 */
	long nr_failed;			/* could not be isolated at this time */
	/* 累计本轮暂时无法隔离的 folio 数，最终转换为正返回值或 STRICT 的 -EIO。 */
};
/*
 * queue_pages 汇总一次页表遍历的输入与结果：pagelist 接收已隔离 folio；flags/nmask
 * 决定选择方向；start/end 是字节地址范围；first 记录首个 VMA；large 缓存最近遇到的
 * 大 folio 以避免重复处理；nr_failed 统计暂时无法隔离的页。walk->private 借用该对象，
 * 生命周期覆盖整个 walk；页表锁/mmap 锁分别保护页表与 VMA，列表所有权由调用者回收。
 */

/*
 * Check if the folio's nid is in qp->nmask.
 *
 * If MPOL_MF_INVERT is set in qp->flags, check if the nid is
 * in the invert of qp->nmask.
 */
/*
 * queue_folio_required() - 判断 folio 所在节点是否属于本次扫描的目标集合
 * 调用位置：PTE、PMD 与 hugetlb walker 在隔离候选之前共同使用的纯判断 helper。
 * @folio: 页表锁或其他上层协议稳定的借用 folio；@qp: 本次遍历上下文。
 * 返回：普通模式下 nid 在 nmask 中为 true；INVERT 时结果反转。只读取节点与标志，
 * 不隔离 folio、不修改列表，也不睡眠。
 */
static inline bool queue_folio_required(struct folio *folio,
					struct queue_pages *qp)
{
	int nid = folio_nid(folio);
	unsigned long flags = qp->flags;

	return node_isset(nid, *qp->nmask) == !(flags & MPOL_MF_INVERT);
}

/*
 * queue_folios_pmd() - 处理页表遍历遇到的叶子 PMD 大 folio
 * @pmd: 当前 PMD 指针，由 walk 框架与页表锁协议保护；@walk: 携带 queue_pages 私有状态。
 * 返回无直接值。迁移条目或无法取得/隔离 folio 时累计 nr_failed；无需处理的节点直接
 * 返回；成功则把 folio 所有隔离引用交给 qp->pagelist。函数处于页表遍历回调上下文，
 * 不能任意睡眠。
 */
static void queue_folios_pmd(pmd_t *pmd, struct mm_walk *walk)
{
	/* folio 是从 PMD 借用的大页对象；qp 累积失败计数和迁移列表。 */
	struct folio *folio;
	struct queue_pages *qp = walk->private;

	if (unlikely(pmd_is_migration_entry(*pmd))) {
		/* 已在迁移中的 PMD 无法再次隔离，记一次暂时失败而不等待它完成。 */
		qp->nr_failed++;
		return;
	}
	folio = pmd_folio(*pmd);
	if (is_huge_zero_folio(folio)) {
		/* 共享 huge-zero folio 不属于该进程且不可迁移，要求 walker 继续深入/前进。 */
		walk->action = ACTION_CONTINUE;
		return;
	}
	if (!queue_folio_required(folio, qp))
		return;
	if (!(qp->flags & (MPOL_MF_MOVE | MPOL_MF_MOVE_ALL)) ||
	    !vma_migratable(walk->vma) ||
	    !migrate_folio_add(folio, qp->pagelist, qp->flags))
		qp->nr_failed++;
}

/*
 * Scan through folios, checking if they satisfy the required conditions,
 * moving them from LRU to local pagelist for migration if they do (or not).
 *
 * queue_folios_pte_range() has two possible return values:
 * 0 - continue walking to scan for more, even if an existing folio on the
 *     wrong node could not be isolated and queued for migration.
 * -EIO - only MPOL_MF_STRICT was specified, without MPOL_MF_MOVE or ..._ALL,
 *        and an existing folio was on a node that does not follow the policy.
 */
/*
 * queue_folios_pte_range() - 扫描普通 PTE，把符合节点条件且允许迁移的 folio 隔离入队
 * @pmd: 覆盖 [addr,end) 的上级页表项；@addr/@end: 页对齐虚拟地址范围；@walk: 提供
 * 当前 VMA/mm 和 queue_pages。所有参数均为遍历期间借用。
 * 上下文：先尝试 THP PMD 锁，否则逐 PTE 持页表锁；释放锁后 cond_resched()，因此调用
 * 路径必须允许调度。返回 0 继续扫描；仅 STRICT 且不迁移、发现错位页时返回 -EIO。
 * 成功隔离的 folio 转入 qp->pagelist，后续 queue_pages_range()/migrate_pages() 接管。
 */
static int queue_folios_pte_range(pmd_t *pmd, unsigned long addr,
			unsigned long end, struct mm_walk *walk)
{
	/* vma/qp 是借用上下文；pte/mapped_pte 与 ptl 描述当前映射窗口和其锁。 */
	struct vm_area_struct *vma = walk->vma;
	struct folio *folio;
	struct queue_pages *qp = walk->private;
	unsigned long flags = qp->flags;
	pte_t *pte, *mapped_pte;
	pte_t ptent;
	spinlock_t *ptl;
	int max_nr, nr;

	ptl = pmd_trans_huge_lock(pmd, vma);
	/* 阶段 1：若 PMD 直接映射 THP，则在对应锁下按一个大 folio 处理，避免逐 PTE 重复。 */
	if (ptl) {
		queue_folios_pmd(pmd, walk);
		spin_unlock(ptl);
		goto out;
	}

	mapped_pte = pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	/* 阶段 2：建立并锁定 PTE 映射；并发页表拆分要求 ACTION_AGAIN 由 walker 重试。 */
	if (!pte) {
		walk->action = ACTION_AGAIN;
		return 0;
	}
	for (; addr != end; pte += nr, addr += nr * PAGE_SIZE) {
		/* nr 是本次可批量跳过的连续 PTE 数，通常为 1，大 folio 可一次跨过同批映射。 */
		max_nr = (end - addr) >> PAGE_SHIFT;
		nr = 1;
		ptent = ptep_get(pte);
		if (pte_none(ptent))
			continue;
		if (!pte_present(ptent)) {
			const softleaf_t entry = softleaf_from_pte(ptent);

			if (softleaf_is_migration(entry))
				/* 迁移条目代表页暂不可稳定取得，计失败但普通模式继续完成其余扫描。 */
				qp->nr_failed++;
			continue;
		}
		folio = vm_normal_folio(vma, addr, ptent);
		if (!folio || folio_is_zone_device(folio))
			continue;
		if (folio_test_large(folio) && max_nr != 1)
			nr = folio_pte_batch(folio, pte, ptent, max_nr);
		/*
		 * vm_normal_folio() filters out zero pages, but there might
		 * still be reserved folios to skip, perhaps in a VDSO.
		 */
		/* vm_normal_folio 已排除零页，但 VDSO 等保留 folio 仍不能参与普通 LRU 迁移。 */
		if (folio_test_reserved(folio))
			continue;
		if (!queue_folio_required(folio, qp))
			continue;
		if (folio_test_large(folio)) {
			/*
			 * A large folio can only be isolated from LRU once,
			 * but may be mapped by many PTEs (and Copy-On-Write may
			 * intersperse PTEs of other, order 0, folios).  This is
			 * a common case, so don't mistake it for failure (but
			 * there can be other cases of multi-mapped pages which
			 * this quick check does not help to filter out - and a
			 * search of the pagelist might grow to be prohibitive).
			 *
			 * migrate_pages(&pagelist) returns nr_failed folios, so
			 * check "large" now so that queue_pages_range() returns
			 * a comparable nr_failed folios.  This does imply that
			 * if folio could not be isolated for some racy reason
			 * at its first PTE, later PTEs will not give it another
			 * chance of isolation; but keeps the accounting simple.
			 */
			/*
			 * 同一大 folio 可由多个 PTE 映射，却只能从 LRU 隔离一次。large 缓存最近对象，
			 * 避免把重复 PTE 误计为迁移失败，并让这里与 migrate_pages 的 folio 计数一致。
			 * 代价是第一次因竞态隔离失败后，本次扫描不会借后续 PTE 再试。
			 */
			if (folio == qp->large)
				continue;
			qp->large = folio;
		}
		if (!(flags & (MPOL_MF_MOVE | MPOL_MF_MOVE_ALL)) ||
		    !vma_migratable(vma) ||
		    !migrate_folio_add(folio, qp->pagelist, flags)) {
			qp->nr_failed += nr;
			if (strictly_unmovable(flags))
				/* STRICT-only 已满足 -EIO 条件，无需继续扫描本 PTE 范围。 */
				break;
		}
	}
	pte_unmap_unlock(mapped_pte, ptl);
	/* 阶段 3：先解除页表锁和临时映射，再允许调度，避免在自旋锁内切换任务。 */
	cond_resched();
out:
	if (qp->nr_failed && strictly_unmovable(flags))
		return -EIO;
	return 0;
}

/*
 * queue_folios_hugetlb() - 扫描 hugetlb 页表项并按 mbind 规则选择迁移对象
 * @pte: huge PTE；@hmask: hugepage 范围掩码（本回调无需直接使用）；@addr/@end: 当前
 * 地址范围；@walk: 当前 VMA/mm 与 queue_pages。参数均为借用。
 * CONFIG_HUGETLB_PAGE 下持 huge_pte_lock 检查条目；MOVE_ALL 可迁移共享 huge folio，
 * 普通 MOVE 则保守跳过可能共享对象。返回 -EIO 仅表示 STRICT-only 发现错位页，否则 0。
 * 成功隔离的 folio 所有权交给 qp->pagelist；关闭配置时是无副作用 stub。
 * 上下文：mm_walk 回调；CONFIG_HUGETLB_PAGE 下取得 huge PTE 自旋锁，锁内不得睡眠。
 */
static int queue_folios_hugetlb(pte_t *pte, unsigned long hmask,
			       unsigned long addr, unsigned long end,
			       struct mm_walk *walk)
{
#ifdef CONFIG_HUGETLB_PAGE
	/* qp/flags 是遍历策略；folio 为锁内借用；ptl/ptep 稳定 huge 页表项。 */
	struct queue_pages *qp = walk->private;
	unsigned long flags = qp->flags;
	struct folio *folio;
	spinlock_t *ptl;
	pte_t ptep;

	ptl = huge_pte_lock(hstate_vma(walk->vma), walk->mm, pte);
	/* 阶段 1：在 huge PTE 锁下读取条目，迁移条目只计失败，空条目直接离开。 */
	ptep = huge_ptep_get(walk->mm, addr, pte);
	if (!pte_present(ptep)) {
		if (!huge_pte_none(ptep)) {
			const softleaf_t entry = softleaf_from_pte(ptep);

			if (unlikely(softleaf_is_migration(entry)))
				qp->nr_failed++;
		}

		goto unlock;
	}
	folio = pfn_folio(pte_pfn(ptep));
	/* 阶段 2：条目已确认 present；锁内取得 folio 后依次验证节点、VMA 和 MOVE 权限。 */
	if (!queue_folio_required(folio, qp))
		goto unlock;
	if (!(flags & (MPOL_MF_MOVE | MPOL_MF_MOVE_ALL)) ||
	    !vma_migratable(walk->vma)) {
		qp->nr_failed++;
		goto unlock;
	}
	/*
	 * Unless MPOL_MF_MOVE_ALL, we try to avoid migrating a shared folio.
	 * Choosing not to migrate a shared folio is not counted as a failure.
	 *
	 * See folio_maybe_mapped_shared() on possible imprecision when we
	 * cannot easily detect if a folio is shared.
	 */
	/*
	 * 除非用户有 MOVE_ALL 权限，否则尽量不迁移共享 folio；共享探测可能保守不精确，
	 * 但“选择跳过”不计失败，因为普通 MOVE 本就没有夺取其他映射页的承诺。
	 */
	if ((flags & MPOL_MF_MOVE_ALL) ||
	    (!folio_maybe_mapped_shared(folio) && !hugetlb_pmd_shared(pte)))
		if (!folio_isolate_hugetlb(folio, qp->pagelist))
			qp->nr_failed++;
unlock:
	/* 所有出口统一释放 huge PTE 锁；锁只保证检查窗口，不延长 folio 的长期生命周期。 */
	spin_unlock(ptl);
	if (qp->nr_failed && strictly_unmovable(flags))
		return -EIO;
#endif
	return 0;
}

#ifdef CONFIG_NUMA_BALANCING
/**
 * folio_can_map_prot_numa() - check whether the folio can map prot numa
 * @folio: The folio whose mapping considered for being made NUMA hintable
 * @vma: The VMA that the folio belongs to.
 * @is_private_single_threaded: Is this a single-threaded private VMA or not
 *
 * This function checks to see if the folio actually indicates that
 * we need to make the mapping one which causes a NUMA hinting fault,
 * as there are cases where it's simply unnecessary, and the folio's
 * access time is adjusted for memory tiering if prot numa needed.
 *
 * Return: True if the mapping of the folio needs to be changed, false otherwise.
 */
/*
 * folio_can_map_prot_numa() - 判断映射是否值得改成 NUMA hinting fault 形式
 * @folio: 可为 NULL 的借用 folio；@vma: 所属 VMA 借用指针；
 * @is_private_single_threaded: 是否是单线程私有地址空间，可启用“已在本地”快速跳过。
 * 上下文：NUMA 扫描路径，只做属性查询与可选访问时间原子交换，不睡眠、不持有引用。
 * 返回 true 表示后续可以把 PTE 改为 NUMA 提示保护；设备页、KSM、共享 COW、DMA pin、
 * 异步不可迁移的脏文件页及无需平衡的顶层节点返回 false。true 不保证将来迁移成功。
 */
bool folio_can_map_prot_numa(struct folio *folio, struct vm_area_struct *vma,
		bool is_private_single_threaded)
{
	/* nid 只在基础可迁移性检查通过后读取，避免对 NULL/特殊 folio 求节点。 */
	int nid;

	if (!folio || folio_is_zone_device(folio) || folio_test_ksm(folio))
		return false;

	/* Also skip shared copy-on-write folios */
	/* 共享 COW folio 的放置关系到多个映射，单个 VMA 的采样不能代表所有者意愿。 */
	if (is_cow_mapping(vma->vm_flags) && folio_maybe_mapped_shared(folio))
		return false;

	/* Folios are pinned and can't be migrated */
	/* DMA 长期 pin 阻止物理页搬迁，制造提示 fault 只会增加开销而不会产生收益。 */
	if (folio_maybe_dma_pinned(folio))
		return false;

	/*
	 * While migration can move some dirty folios,
	 * it cannot move them all from MIGRATE_ASYNC
	 * context.
	 */
	/* 异步 NUMA 迁移不能覆盖所有脏文件页，故先跳过，避免反复 fault 后仍无法移动。 */
	if (folio_is_file_lru(folio) && folio_test_dirty(folio))
		return false;

	/*
	 * Don't mess with PTEs if folio is already on the node
	 * a single-threaded process is running on.
	 */
	/* 单线程私有映射已位于当前 CPU 节点时无需采样；多线程情形不能用一个 CPU 代表。 */
	nid = folio_nid(folio);
	if (is_private_single_threaded && (nid == numa_node_id()))
		return false;

	/*
	 * Skip scanning top tier node if normal numa
	 * balancing is disabled
	 */
	/* 若只启用内存分层而关闭普通 NUMA balancing，顶层节点无需再做同层位置优化。 */
	if (!(sysctl_numa_balancing_mode & NUMA_BALANCING_NORMAL) &&
	    node_is_toptier(nid))
		return false;

	if (folio_use_access_time(folio))
		/* 分层使用 folio access-time 时，在制造提示 fault 的边界记录本轮扫描时间。 */
		folio_xchg_access_time(folio, jiffies_to_msecs(jiffies));

	return true;
}

/*
 * This is used to mark a range of virtual addresses to be inaccessible.
 * These are later cleared by a NUMA hinting fault. Depending on these
 * faults, pages may be migrated for better NUMA placement.
 *
 * This is assuming that NUMA faults are handled using PROT_NONE. If
 * an architecture makes a different choice, it will need further
 * changes to the core.
 */
/*
 * change_prot_numa() - 把一段映射改为会触发 NUMA hinting fault 的保护形式
 * @vma: 借用 VMA，调用者持有适当 mmap/VMA 写保护；@addr/@end: 页对齐半开区间。
 * 上下文：可执行页表修改和 TLB shootdown；调用者须处于允许该同步的进程上下文。
 * 返回：实际更新的 PTE 数（0 表示无需修改）。副作用包括修改页表、刷新 TLB，并把正数
 * 更新量计入全局与 memcg NUMA_PTE_UPDATES。后续 fault 才负责采样并可能迁移页面。
 */
unsigned long change_prot_numa(struct vm_area_struct *vma,
			unsigned long addr, unsigned long end)
{
	/* tlb 聚合失效范围；nr_updated 是 change_protection 真正改写的页表项数量。 */
	struct mmu_gather tlb;
	long nr_updated;

	tlb_gather_mmu(&tlb, vma->vm_mm);

	nr_updated = change_protection(&tlb, vma, addr, end, MM_CP_PROT_NUMA);
	/* 只有发生外界可见页表变化才记账，避免把扫描过但未改变的页算成提示样本。 */
	if (nr_updated > 0) {
		count_vm_numa_events(NUMA_PTE_UPDATES, nr_updated);
		count_memcg_events_mm(vma->vm_mm, NUMA_PTE_UPDATES, nr_updated);
	}

	tlb_finish_mmu(&tlb);

	return nr_updated;
}
#endif /* CONFIG_NUMA_BALANCING */
/* 关闭 NUMA_BALANCING 时，上述提示 fault 选择与保护修改入口均不编译。 */

/*
 * queue_pages_test_walk() - 在进入页表前校验 VMA 连续性并决定该 VMA 是否需要扫描
 * @start/@end: walker 当前覆盖的半开区间；@walk: 当前 VMA 和 queue_pages 上下文。
 * 上下文：调用者持 mmap 锁，函数可查询相邻 VMA，但不取得新引用。
 * 返回：0 表示执行页表回调；1 表示该 VMA 可跳过；-EFAULT 表示要求连续却发现空洞；
 * STRICT 路径即使 VMA 不可迁移也继续检查，以便发现违例后返回 -EIO。
 */
static int queue_pages_test_walk(unsigned long start, unsigned long end,
				struct mm_walk *walk)
{
	/* first 用于识别范围头，next 用于检测中部/尾部空洞，flags 决定是否容许不连续。 */
	struct vm_area_struct *next, *vma = walk->vma;
	struct queue_pages *qp = walk->private;
	unsigned long flags = qp->flags;

	/* range check first */
	/* walker 契约要求当前子区间完全落入 vma；违反说明调用框架或范围拆分有 bug。 */
	VM_BUG_ON_VMA(!range_in_vma(vma, start, end), vma);

	if (!qp->first) {
		qp->first = vma;
		if (!(flags & MPOL_MF_DISCONTIG_OK) &&
			(qp->start < vma->vm_start))
			/* hole at head side of range */
			/* 请求起点早于首个 VMA，范围头存在空洞。 */
			return -EFAULT;
	}
	next = find_vma(vma->vm_mm, vma->vm_end);
	if (!(flags & MPOL_MF_DISCONTIG_OK) &&
		((vma->vm_end < qp->end) &&
		(!next || vma->vm_end < next->vm_start)))
		/* hole at middle or tail of range */
		/* 当前 VMA 结束后到请求 end 之间没有连续下一 VMA，属于中间或尾部空洞。 */
		return -EFAULT;

	/*
	 * Need check MPOL_MF_STRICT to return -EIO if possible
	 * regardless of vma_migratable
	 */
	/* STRICT 必须检查不可迁移 VMA 中是否已有错位页，不能因“反正搬不动”而跳过。 */
	if (!vma_migratable(vma) &&
	    !(flags & MPOL_MF_STRICT))
		return 1;

	/*
	 * Check page nodes, and queue pages to move, in the current vma.
	 * But if no moving, and no strict checking, the scan can be skipped.
	 */
	/* 只有严格验证或实际 MOVE 请求需要进入昂贵页表扫描；单纯改策略可直接处理 VMA。 */
	if (flags & (MPOL_MF_STRICT | MPOL_MF_MOVE | MPOL_MF_MOVE_ALL))
		return 0;
	return 1;
}

static const struct mm_walk_ops queue_pages_walk_ops = {
	/* 读锁版本用于只需稳定 VMA 的扫描；具体 PTE/hugetlb 回调自行取得页表锁。 */
	.hugetlb_entry		= queue_folios_hugetlb,
	.pmd_entry		= queue_folios_pte_range,
	.test_walk		= queue_pages_test_walk,
	.walk_lock		= PGWALK_RDLOCK,
};

static const struct mm_walk_ops queue_pages_lock_vma_walk_ops = {
	/* 写锁版本供同时修改 VMA 策略的 mbind 路径，避免扫描与策略替换之间出现窗口。 */
	.hugetlb_entry		= queue_folios_hugetlb,
	.pmd_entry		= queue_folios_pte_range,
	.test_walk		= queue_pages_test_walk,
	.walk_lock		= PGWALK_WRLOCK,
};

/*
 * Walk through page tables and collect pages to be migrated.
 *
 * If pages found in a given range are not on the required set of @nodes,
 * and migration is allowed, they are isolated and queued to @pagelist.
 *
 * queue_pages_range() may return:
 * 0 - all pages already on the right node, or successfully queued for moving
 *     (or neither strict checking nor moving requested: only range checking).
 * >0 - this number of misplaced folios could not be queued for moving
 *      (a hugetlbfs page or a transparent huge page being counted as 1).
 * -EIO - a misplaced page found, when MPOL_MF_STRICT specified without MOVEs.
 * -EFAULT - a hole in the memory range, when MPOL_MF_DISCONTIG_OK unspecified.
 */
/*
 * queue_pages_range() - 遍历地址范围并收集需要迁移的 folio
 * 调用位置：do_mbind()/migrate_to_node() -> walk_page_range() -> 各级页表回调。
 * @mm: 调用者持有并已按 flags 选择合适 mmap 锁语义的地址空间；@start/@end: 半开地址
 * 区间；@nodes: 节点测试掩码；@flags: STRICT/MOVE/INVERT/WRLOCK 等；@pagelist: 输出列表。
 * 函数通过 mm_walk 回调隔离 folio；成功隔离的列表节点及引用转交调用者，调用者必须迁移
 * 或 putback。返回 0 表示无违例/均已入队，正数为未能入队的 folio 数，-EIO 为严格违例，
 * -EFAULT 为不允许的 VMA 空洞。可调度，要求进程上下文。
 */
static long
queue_pages_range(struct mm_struct *mm, unsigned long start, unsigned long end,
		nodemask_t *nodes, unsigned long flags,
		struct list_head *pagelist)
{
	/* qp 是整个 walk 的栈上状态；ops 根据调用者是否已有 VMA 写锁选择锁级别。 */
	int err;
	struct queue_pages qp = {
		.pagelist = pagelist,
		.flags = flags,
		.nmask = nodes,
		.start = start,
		.end = end,
		.first = NULL,
	};
	/* WRLOCK 只改变 walker 的锁回调，不改变 qp 中的范围与筛选语义。 */
	const struct mm_walk_ops *ops = (flags & MPOL_MF_WRLOCK) ?
			&queue_pages_lock_vma_walk_ops : &queue_pages_walk_ops;

	/* 阶段 1：按 WRLOCK 选择 VMA 锁级别，再把所有选择与计数状态交给 walker。 */
	err = walk_page_range(mm, start, end, ops, &qp);
	/* walk 错误优先于累计失败数；GNU ?: 在 err 为 0 时返回 qp.nr_failed。 */

	if (!qp.first)
		/* whole range in hole */
		/* walker 从未遇到 VMA，整个请求都是空洞，即使允许中间不连续也无对象可绑定。 */
		err = -EFAULT;

	return err ? : qp.nr_failed;
}

/*
 * Apply policy to a single VMA
 * This must be called with the mmap_lock held for writing.
 */
/*
 * vma_replace_policy() - 原子地用策略副本替换单个 VMA 的策略
 * @vma: 已由 mmap 写锁稳定且经 vma_start_write 的输入输出 VMA；@pol: 借用模板策略，
 * 可为 NULL 表示默认。函数复制一份 VMA 自有引用，必要时先让文件系统 set_policy 接受。
 * 返回 0 表示 WRITE_ONCE 已发布新指针并释放旧引用；-ENOMEM 或回调 errno 表示 VMA 保持
 * 原策略，新副本已回收。可睡眠；调用者继续持有 @pol，自身所有权不变。
 */
static int vma_replace_policy(struct vm_area_struct *vma,
				struct mempolicy *pol)
{
	/* new 是待发布的独占引用，old 是发布点后由本函数负责 put 的旧 VMA 引用。 */
	int err;
	struct mempolicy *old;
	struct mempolicy *new;

	vma_assert_write_locked(vma);

	new = mpol_dup(pol);
	if (IS_ERR(new))
		return PTR_ERR(new);

	if (vma->vm_ops && vma->vm_ops->set_policy) {
		/* 文件系统/特殊映射先验证或安装其私有策略状态；失败前不能改 vm_policy。 */
		err = vma->vm_ops->set_policy(vma, new);
		if (err)
			goto err_out;
	}

	old = vma->vm_policy;
	WRITE_ONCE(vma->vm_policy, new); /* protected by mmap_lock */
	/* mmap 写锁提供结构同步，WRITE_ONCE 避免无锁诊断/推测读者看到撕裂指针。 */
	mpol_put(old);

	return 0;
 err_out:
	/* 回调拒绝时 new 尚未发布，当前函数仍拥有并可直接交还其引用。 */
	mpol_put(new);
	return err;
}

/* Split or merge the VMA (if required) and apply the new policy */
/*
 * mbind_range() - 把新策略应用到一个请求子区间，必要时拆分或合并 VMA
 * @vmi: 已定位的 VMA 迭代器；@vma: 当前借用 VMA；@prev: 输入输出前驱，供 maple-tree
 * 修改/合并；@start/@end: 请求半开区间；@new_pol: 借用策略模板。
 * 入口持 mmap 写锁。相同策略快速返回；否则 vma_modify_policy 先塑造精确边界，再由
 * vma_replace_policy 发布副本。返回 0 或修改/复制 errno；失败后的 VMA 结构由 helper
 * 契约保持有效，new_pol 所有权始终属于上层。
 */
static int mbind_range(struct vma_iterator *vmi, struct vm_area_struct *vma,
		struct vm_area_struct **prev, unsigned long start,
		unsigned long end, struct mempolicy *new_pol)
{
	/* vmstart/vmend 是当前 VMA 与请求范围的交集边界。 */
	unsigned long vmstart, vmend;

	vmend = min(end, vma->vm_end);
	if (start > vma->vm_start) {
		*prev = vma;
		vmstart = start;
	} else {
		vmstart = vma->vm_start;
	}

	if (mpol_equal(vma->vm_policy, new_pol)) {
		/* 策略语义完全相同，不必拆分 VMA；仍推进 prev 以维持迭代器合并上下文。 */
		*prev = vma;
		return 0;
	}

	vma =  vma_modify_policy(vmi, *prev, vma, vmstart, vmend, new_pol);
	/* vma_modify_policy 是结构变更点；返回的新 VMA 才精确覆盖待绑定区间。 */
	if (IS_ERR(vma))
		return PTR_ERR(vma);

	*prev = vma;
	return vma_replace_policy(vma, new_pol);
}

/* Set the process memory policy */
/*
 * do_set_mempolicy() - 构造并发布 current 的线程级 NUMA 策略
 * @mode/@flags/@nodes: 已从用户 ABI 解析的模式、持久标志和节点掩码；nodes 由调用者
 * 栈上持有。进程上下文，可分配内存并睡眠。
 * 返回 0 或 -ENOMEM/-EINVAL。成功时 current 接管 new 的初始引用，旧策略在解锁后 put；
 * 交错策略同时重置 il_prev/il_weight，使下一次分配从新节点集合重新开始。失败不改变
 * current->mempolicy，所有临时对象与 NODEMASK_SCRATCH 均在本函数回收。
 */
static long do_set_mempolicy(unsigned short mode, unsigned short flags,
			     nodemask_t *nodes)
{
	/* new/old 分别是候选与被替换引用；scratch 避免在内核栈放置多个大 nodemask。 */
	struct mempolicy *new, *old;
	NODEMASK_SCRATCH(scratch);
	int ret;

	if (!scratch)
		return -ENOMEM;

	new = mpol_new(mode, flags, nodes);
	/* 阶段 1：对象尚未发布，错误指针可直接转换为 errno。 */
	if (IS_ERR(new)) {
		ret = PTR_ERR(new);
		goto out;
	}

	task_lock(current);
	/* task_lock 即 alloc_lock，串行化 mems_allowed 与策略构造/指针替换。 */
	ret = mpol_set_nodemask(new, nodes, scratch);
	if (ret) {
		task_unlock(current);
		mpol_put(new);
		goto out;
	}

	old = current->mempolicy;
	current->mempolicy = new;
	/* 这里是线程策略发布点；锁内读者不会观察到未完成 nodemask 的 new。 */
	if (new && (new->mode == MPOL_INTERLEAVE ||
		    new->mode == MPOL_WEIGHTED_INTERLEAVE)) {
		current->il_prev = MAX_NUMNODES-1;
		current->il_weight = 0;
	}
	task_unlock(current);
	/* 旧对象已从任务摘除，解锁后交还其引用；NULL/default 无需释放。 */
	mpol_put(old);
	ret = 0;
out:
	NODEMASK_SCRATCH_FREE(scratch);
	return ret;
}

/*
 * Return nodemask for policy for get_mempolicy() query
 *
 * Called with task's alloc_lock held
 */
/*
 * get_policy_nodemask() - 为 get_mempolicy 查询提取策略的有效节点集合
 * 调用位置：do_get_mempolicy() 在 task alloc_lock 内构造用户查询结果。
 * @pol: 借用的有效策略或 default_policy；@nodes: 输出掩码，进入时内容无意义。
 * 调用者持 task alloc_lock，函数不睡眠、不获取引用。DEFAULT/LOCAL 输出空集，多节点与
 * PREFERRED 输出 pol->nodes；未知模式触发 BUG，表示内核内部不变量损坏。
 * 返回：无直接返回值；唯一输出是 @nodes，@pol 只读借用且调用后所有权不变。
 */
static void get_policy_nodemask(struct mempolicy *pol, nodemask_t *nodes)
{
	/* 阶段 1：先建立空输出，使 DEFAULT/LOCAL 的所有快速出口都返回确定内容。 */
	nodes_clear(*nodes);
	if (pol == &default_policy)
		return;

	/* 阶段 2：只有携带节点集合的模式复制 policy->nodes，LOCAL 继续保持空集。 */
	switch (pol->mode) {
	case MPOL_BIND:
	case MPOL_INTERLEAVE:
	case MPOL_PREFERRED:
	case MPOL_PREFERRED_MANY:
	case MPOL_WEIGHTED_INTERLEAVE:
		*nodes = pol->nodes;
		break;
	case MPOL_LOCAL:
		/* return empty node mask for local allocation */
		/* LOCAL 的 ABI 表示就是空节点掩码，由执行时当前 CPU 决定实际 nid。 */
		break;
	default:
		BUG();
	}
}

/*
 * lookup_node() - 查询一个用户虚拟地址当前驻留页的 NUMA nid
 * @mm: 查询地址空间的借用指针；当前实现通过 current 的 fast-GUP，调用者传 current->mm。
 * @addr: 任意用户地址，函数向下按 PAGE_MASK 对齐。可触发 GUP 慢动作但不写页面。
 * 返回有效 nid，或 get_user_pages_fast 的 0/负 errno；成功 pin 的 page 在读取 nid 后立即
 * put，不把引用传给调用者。
 * 调用位置：do_get_mempolicy(MPOL_F_NODE|MPOL_F_ADDR) 解 mmap 锁后的驻留节点查询。
 * 上下文：进程上下文；fast-GUP 可能访问页表，函数本身不要求调用者持 mmap 锁。
 */
static int lookup_node(struct mm_struct *mm, unsigned long addr)
{
	struct page *p = NULL;
	int ret;

	ret = get_user_pages_fast(addr & PAGE_MASK, 1, 0, &p);
	if (ret > 0) {
		ret = page_to_nid(p);
		put_page(p);
	}
	return ret;
}

/* Retrieve NUMA policy */
/*
 * do_get_mempolicy() - 实现 get_mempolicy 的内核查询语义
 * 调用位置：kernel_get_mempolicy() 完成用户参数准备后进入的核心状态查询层。
 * @policy: 必须可写的内核输出整数；@nmask: 可为 NULL 的输出节点掩码；@addr: 仅与
 * MPOL_F_ADDR 配合；@flags: NODE/ADDR/MEMS_ALLOWED 查询组合。
 * 可睡眠并取得 mmap 读锁、task_lock 与临时策略引用。返回 0 或 -EINVAL/-EFAULT/GUP
 * errno。ADDR 查询故意在 VMA/共享策略为 NULL 时报告 DEFAULT，而不回退线程策略；NODE
 * 查询返回驻留页 nid，或线程交错策略下一目标。所有锁与临时引用在统一出口释放。
 */
static long do_get_mempolicy(int *policy, nodemask_t *nmask,
			     unsigned long addr, unsigned long flags)
{
	/* pol 默认借用当前线程策略；pol_refcount 只在解 mmap 锁后仍需使用时持有额外引用。 */
	int err;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma = NULL;
	struct mempolicy *pol = current->mempolicy, *pol_refcount = NULL;

	if (flags &
		~(unsigned long)(MPOL_F_NODE|MPOL_F_ADDR|MPOL_F_MEMS_ALLOWED))
		return -EINVAL;

	if (flags & MPOL_F_MEMS_ALLOWED) {
		/* 该模式只返回 cpuset 有效集合，不能与地址/节点查询组合。 */
		if (flags & (MPOL_F_NODE|MPOL_F_ADDR))
			return -EINVAL;
		*policy = 0;	/* just so it's initialized */
		/* ABI 仍要求 policy 输出已初始化，但此查询的有效结果仅在 nmask。 */
		task_lock(current);
		*nmask  = cpuset_current_mems_allowed;
		task_unlock(current);
		return 0;
	}
	/* 阶段 1 结束：特殊 cpuset 查询已返回；其余路径开始解析地址或任务有效策略。 */

	if (flags & MPOL_F_ADDR) {
		pgoff_t ilx;		/* ignored here */
		/* ilx 只是 __get_vma_policy 的必需输出，本查询不需要交错页索引。 */
		/*
		 * Do NOT fall back to task policy if the
		 * vma/shared policy at addr is NULL.  We
		 * want to return MPOL_DEFAULT in this case.
		 */
		/*
		 * 地址查询关注该位置显式 VMA/共享策略；NULL 必须呈现为 MPOL_DEFAULT，不能偷偷
		 * 继承 current 策略，否则用户无法区分“VMA 未设置”和“有效结果相同”。
		 */
		mmap_read_lock(mm);
		vma = vma_lookup(mm, addr);
		if (!vma) {
			mmap_read_unlock(mm);
			return -EFAULT;
		}
		pol = __get_vma_policy(vma, addr, &ilx);
	} else if (addr)
		return -EINVAL;

	if (!pol)
		pol = &default_policy;	/* indicates default behavior */
		/* 用静态哨兵统一后续 mode/nodemask 分支；它只借用且不参与条件 put。 */

	if (flags & MPOL_F_NODE) {
		/* 阶段 2：NODE+ADDR 查物理驻留节点；单独 NODE 只对当前线程交错策略有定义。 */
		if (flags & MPOL_F_ADDR) {
			/*
			 * Take a refcount on the mpol, because we are about to
			 * drop the mmap_lock, after which only "pol" remains
			 * valid, "vma" is stale.
			 */
			/*
			 * 解 mmap_lock 后 VMA 可立即销毁，故清空 vma 并给 pol 加引用；引用只延长策略
			 * 内存生命，不冻结字段，查询所需字段已受先前锁窗口约束。
			 */
			pol_refcount = pol;
			vma = NULL;
			mpol_get(pol);
			mmap_read_unlock(mm);
			err = lookup_node(mm, addr);
			if (err < 0)
				goto out;
			*policy = err;
		} else if (pol == current->mempolicy &&
				pol->mode == MPOL_INTERLEAVE) {
			*policy = next_node_in(current->il_prev, pol->nodes);
			/* 普通交错返回动态游标的下一节点，但查询本身不推进 current->il_prev。 */
		} else if (pol == current->mempolicy &&
				pol->mode == MPOL_WEIGHTED_INTERLEAVE) {
			if (current->il_weight)
				*policy = current->il_prev;
			else
				*policy = next_node_in(current->il_prev,
						       pol->nodes);
		} else {
			err = -EINVAL;
			goto out;
		}
	} else {
		/* 阶段 3：非 NODE 查询输出模式与可公开标志，不观察页面驻留状态。 */
		*policy = pol == &default_policy ? MPOL_DEFAULT :
						pol->mode;
		/*
		 * Internal mempolicy flags must be masked off before exposing
		 * the policy to userspace.
		 */
		/* 只向用户暴露 ABI 模式位；MPOL_MF_INTERNAL 等实现标志必须屏蔽。 */
		*policy |= (pol->flags & MPOL_MODE_FLAGS);
	}

	err = 0;
	/* 阶段 4：在相应保护下复制用户原始或当前有效 nodemask，再统一释放锁与引用。 */
	if (nmask) {
		/* STATIC/RELATIVE 返回用户原始表达，普通策略返回当前有效集合。 */
		if (mpol_store_user_nodemask(pol)) {
			*nmask = pol->w.user_nodemask;
		} else {
			task_lock(current);
			get_policy_nodemask(pol, nmask);
			task_unlock(current);
		}
	}

 out:
	/*
	 * mpol_cond_put 只释放 __get_vma_policy 可能取得的共享策略引用；pol_refcount 是跨越
	 * mmap 解锁额外取得的独立引用，必须另行 put。vma 非 NULL 表示锁尚未提前释放。
	 */
	mpol_cond_put(pol);
	if (vma)
		mmap_read_unlock(mm);
	if (pol_refcount)
		mpol_put(pol_refcount);
	return err;
}

#ifdef CONFIG_NUMA_MIGRATION
/* 启用 NUMA_MIGRATION 时，以下路径真正隔离并同步迁移 folio。 */
/*
 * migrate_folio_add() - 尝试把普通 folio 从 LRU 隔离并加入迁移列表
 * 调用位置：PTE/PMD walker 发现错位 folio 后、真正 migrate_pages() 之前。
 * @folio: 借用候选；@foliolist: 调用者持有的输出列表；@flags: MOVE/MOVE_ALL 权限语义。
 * 返回 true 表示无需迁移或已成功入队；false 表示当前无法隔离。普通 MOVE 保守跳过共享
 * folio且不算失败；MOVE_ALL 可尝试。成功隔离后列表接管 folio 的隔离状态，统计同步增加，
 * 调用者必须 migrate_pages 或 putback_movable_pages 完成闭环。
 * 上下文：页表遍历的锁定窗口；folio_isolate_lru 不睡眠，列表由外层迁移路径独占。
 */
static bool migrate_folio_add(struct folio *folio, struct list_head *foliolist,
				unsigned long flags)
{
	/*
	 * Unless MPOL_MF_MOVE_ALL, we try to avoid migrating a shared folio.
	 * Choosing not to migrate a shared folio is not counted as a failure.
	 *
	 * See folio_maybe_mapped_shared() on possible imprecision when we
	 * cannot easily detect if a folio is shared.
	 */
	/* 普通 MOVE 避免改变其他映射的物理位置；共享性探测允许保守误判。 */
	if ((flags & MPOL_MF_MOVE_ALL) || !folio_maybe_mapped_shared(folio)) {
		/* 阶段 2：只有需要实际隔离的对象才修改 LRU/统计；保守跳过共享页视为成功。 */
		if (folio_isolate_lru(folio)) {
			list_add_tail(&folio->lru, foliolist);
			node_stat_mod_folio(folio,
				NR_ISOLATED_ANON + folio_is_file_lru(folio),
				folio_nr_pages(folio));
		} else {
			/*
			 * Non-movable folio may reach here.  And, there may be
			 * temporary off LRU folios or non-LRU movable folios.
			 * Treat them as unmovable folios since they can't be
			 * isolated, so they can't be moved at the moment.
			 */
			/* 非 LRU 或暂时已离开 LRU 的对象当前都不能隔离，统一作为本轮不可移动。 */
			return false;
		}
	}
	return true;
}

/*
 * Migrate pages from one node to a target node.
 * Returns error or the number of pages not migrated.
 */
/*
 * migrate_to_node() - 把 mm 中位于单一源节点的可迁移页同步搬到目标节点
 * 调用位置：do_migrate_pages() 为每个 source->dest 配对调用的单节点迁移阶段。
 * @mm: 持有引用的地址空间；@source/@dest: 有效源/目标 nid；@flags: 必含 MOVE 位。
 * 可睡眠；先持 mmap 读锁扫描并隔离，再解锁调用 migrate_pages。返回负 errno 或未迁移
 * folio 数；所有残留列表在迁移失败时 putback。页内容和映射由迁移核心原子切换，本函数
 * 只负责选择、隔离及目标分配控制。
 */
static long migrate_to_node(struct mm_struct *mm, int source, int dest,
			    int flags)
{
	/* nmask 只含源节点；pagelist 持隔离对象；mtc 强制目标节点且标记系统调用原因。 */
	nodemask_t nmask;
	struct vm_area_struct *vma;
	LIST_HEAD(pagelist);
	long nr_failed;
	long err = 0;
	struct migration_target_control mtc = {
		.nid = dest,
		.gfp_mask = GFP_HIGHUSER_MOVABLE | __GFP_THISNODE,
		.reason = MR_SYSCALL,
	};

	/* 阶段 1：所有栈状态初始化完毕后，再生成供反向扫描使用的单节点源掩码。 */
	nodes_clear(nmask);
	/* 阶段 1：构造只含 source 的反向选择掩码，并验证上层确实请求了迁移。 */
	node_set(source, nmask);

	VM_BUG_ON(!(flags & (MPOL_MF_MOVE | MPOL_MF_MOVE_ALL)));

	mmap_read_lock(mm);
	vma = find_vma(mm, 0);
	if (unlikely(!vma)) {
		mmap_read_unlock(mm);
		return 0;
	}

	/*
	 * This does not migrate the range, but isolates all pages that
	 * need migration.  Between passing in the full user address
	 * space range and MPOL_MF_DISCONTIG_OK, this call cannot fail,
	 * but passes back the count of pages which could not be isolated.
	 */
	/*
	 * 扫描覆盖整个用户地址空间并允许 VMA 空洞，所以结构性 -EFAULT 不应出现；返回值
	 * 主要是竞态/不可隔离 folio 数。解 mmap 锁后 VMA 指针不再使用，隔离引用保持页存活。
	 */
	nr_failed = queue_pages_range(mm, vma->vm_start, mm->task_size, &nmask,
				      flags | MPOL_MF_DISCONTIG_OK, &pagelist);
	mmap_read_unlock(mm);

	if (!list_empty(&pagelist)) {
		/* 阶段 2：同步迁移；失败留下的隔离 folio 必须放回 LRU，不能遗失在私有列表。 */
		err = migrate_pages(&pagelist, alloc_migration_target, NULL,
			(unsigned long)&mtc, MIGRATE_SYNC, MR_SYSCALL, NULL);
		if (err)
			putback_movable_pages(&pagelist);
	}

	if (err >= 0)
		/* 合并“隔离失败”与“迁移失败”两类正计数，向上层提供总未完成量。 */
		err += nr_failed;
	return err;
}

/*
 * Move pages between the two nodesets so as to preserve the physical
 * layout as much as possible.
 *
 * Returns the number of page that could not be moved.
 */
/*
 * do_migrate_pages() - 在两组节点之间建立映射并迁移 mm 的现有页
 * @mm: 持有引用的地址空间；@from/@to: 只读源/目标节点集；@flags: MOVE 或 MOVE_ALL。
 * 可睡眠。函数临时禁用 LRU cache 批处理以稳定隔离/迁移交互，逐个 source->dest 调用
 * migrate_to_node。返回首个负 errno，或未迁移数（钳到 INT_MAX）；无论出口都重新启用
 * LRU cache。它尽量保持节点相对位置，并优先腾空尚未接收入站页的目标。
 */
int do_migrate_pages(struct mm_struct *mm, const nodemask_t *from,
		     const nodemask_t *to, int flags)
{
	/* tmp 是尚未处理源集合；nr_failed 累积正值失败计数，err 保存最近一次迁移结果。 */
	long nr_failed = 0;
	long err = 0;
	nodemask_t tmp;

	lru_cache_disable();

	/*
	 * Find a 'source' bit set in 'tmp' whose corresponding 'dest'
	 * bit in 'to' is not also set in 'tmp'.  Clear the found 'source'
	 * bit in 'tmp', and return that <source, dest> pair for migration.
	 * The pair of nodemasks 'to' and 'from' define the map.
	 *
	 * If no pair of bits is found that way, fallback to picking some
	 * pair of 'source' and 'dest' bits that are not the same.  If the
	 * 'source' and 'dest' bits are the same, this represents a node
	 * that will be migrating to itself, so no pages need move.
	 *
	 * If no bits are left in 'tmp', or if all remaining bits left
	 * in 'tmp' correspond to the same bit in 'to', return false
	 * (nothing left to migrate).
	 *
	 * This lets us pick a pair of nodes to migrate between, such that
	 * if possible the dest node is not already occupied by some other
	 * source node, minimizing the risk of overloading the memory on a
	 * node that would happen if we migrated incoming memory to a node
	 * before migrating outgoing memory source that same node.
	 *
	 * A single scan of tmp is sufficient.  As we go, we remember the
	 * most recent <s, d> pair that moved (s != d).  If we find a pair
	 * that not only moved, but what's better, moved to an empty slot
	 * (d is not set in tmp), then we break out then, with that pair.
	 * Otherwise when we finish scanning from_tmp, we at least have the
	 * most recent <s, d> pair that moved.  If we get all the way through
	 * the scan of tmp without finding any node that moved, much less
	 * moved to an empty node, then there is nothing left worth migrating.
	 */
	/*
	 * 每轮优先选择目标不再位于剩余源集合的映射，先搬“出站”再接收入站，可降低目标
	 * 瞬时过载。若找不到空目标，退而使用最近一个 s!=d 的映射；s==d 无需搬迁。
	 */

	tmp = *from;
	while (!nodes_empty(tmp)) {
		/* source 的 NUMA_NO_NODE 是“本轮未找到需移动映射”的哨兵；dest 仅在找到后有效。 */
		int s, d;
		int source = NUMA_NO_NODE;
		int dest = 0;

		for_each_node_mask(s, tmp) {

			/*
			 * do_migrate_pages() tries to maintain the relative
			 * node relationship of the pages established between
			 * threads and memory areas.
                         *
			 * However if the number of source nodes is not equal to
			 * the number of destination nodes we can not preserve
			 * this node relative relationship.  In that case, skip
			 * copying memory from a node that is in the destination
			 * mask.
			 *
			 * Example: [2,3,4] -> [3,4,5] moves everything.
			 *          [0-7] - > [3,4,5] moves only 0,1,2,6,7.
			 */
			/*
			 * 源/目标节点数不等时无法一一保持相对位置；已经同时属于目标集的源节点保留
			 * 原地，只迁移集合差异部分。示例 [2,3,4]->[3,4,5] 会保留 3、4。
			 */

			if ((nodes_weight(*from) != nodes_weight(*to)) &&
						(node_isset(s, *to)))
				continue;

			d = node_remap(s, *from, *to);
			if (s == d)
				continue;

			source = s;	/* Node moved. Memorize */
			/* 记住可移动配对；即使找不到空目标，扫描结束也有一个可用退化选择。 */
			dest = d;

			/* dest not in remaining from nodes? */
			/* 若目标已不在剩余源集合中，它是可优先接收入站页的“空槽”。 */
			if (!node_isset(dest, tmp))
				break;
		}
		if (source == NUMA_NO_NODE)
			break;

		node_clear(source, tmp);
		err = migrate_to_node(mm, source, dest, flags);
		/* 每处理一个源位就从 tmp 摘除，错误时停止，已完成的迁移不回滚。 */
		if (err > 0)
			nr_failed += err;
		if (err < 0)
			break;
	}

	lru_cache_enable();
	if (err < 0)
		return err;
	return (nr_failed < INT_MAX) ? nr_failed : INT_MAX;
}

/*
 * Allocate a new folio for page migration, according to NUMA mempolicy.
 */
/*
 * alloc_migration_target_by_mpol() - 按 mbind 策略为迁移源 folio 分配目标
 * 调用位置：do_mbind() -> migrate_pages() 的目标分配回调；返回后由迁移核心继续复制。
 * @src: 迁移核心持有并传入的源 folio；@private: 编码为 migration_mpol 指针，包含借用
 * 策略与区间基准 ilx。回调可睡眠分配。
 * 返回新 folio 的持有指针或 NULL。hugetlb 走 hstate 专用分配并尊重 fallback；普通/THP
 * 保留源 order，用源 index 修正交错索引后调用 folio_alloc_mpol。目标对象所有权交给
 * migrate_pages，源对象不在本函数修改。
 */
static struct folio *alloc_migration_target_by_mpol(struct folio *src,
						    unsigned long private)
{
	/* mmpol/pol 是同步迁移期间借用；ilx 加源页偏移后才代表该 folio 的策略位置。 */
	struct migration_mpol *mmpol = (struct migration_mpol *)private;
	struct mempolicy *pol = mmpol->pol;
	pgoff_t ilx = mmpol->ilx;
	unsigned int order;
	int nid = numa_node_id();
	gfp_t gfp;

	order = folio_order(src);
	/* 阶段 1：保持源阶数，并把范围基准 ilx 推进到当前源 folio 的对象偏移。 */
	ilx += src->index >> order;

	if (folio_test_hugetlb(src)) {
		/* hugetlb 的 GFP、fallback 与普通页不同，必须使用 hstate 约束和专用分配器。 */
		nodemask_t *nodemask;
		struct hstate *h;

		h = folio_hstate(src);
		gfp = htlb_alloc_mask(h);
		nodemask = policy_nodemask(gfp, pol, ilx, &nid);
		return alloc_hugetlb_folio_nodemask(h, nid, nodemask, gfp,
				htlb_allow_alloc_fallback(MR_MEMPOLICY_MBIND));
	}

	if (folio_test_large(src))
		/* 阶段 3：普通大 folio 使用 THP 约束，order-0/其他复合页使用高端可移动约束。 */
		gfp = GFP_TRANSHUGE;
	else
		gfp = GFP_HIGHUSER_MOVABLE | __GFP_RETRY_MAYFAIL | __GFP_COMP;

	return folio_alloc_mpol(gfp, order, pol, ilx, nid);
}
#else
/* 关闭 NUMA_MIGRATION 时保留调用接口，但不能隔离或搬迁任何页面。 */

/* migrate_folio_add() stub：参数均不使用，返回 false 告知扫描者无法隔离候选。 */
static bool migrate_folio_add(struct folio *folio, struct list_head *foliolist,
				unsigned long flags)
{
	return false;
}

/* do_migrate_pages() stub：无迁移能力时稳定返回 -ENOSYS，输入对象与掩码不变。 */
int do_migrate_pages(struct mm_struct *mm, const nodemask_t *from,
		     const nodemask_t *to, int flags)
{
	return -ENOSYS;
}

/* alloc_migration_target_by_mpol() stub：不分配目标，返回 NULL 让迁移核心失败退出。 */
static struct folio *alloc_migration_target_by_mpol(struct folio *src,
						    unsigned long private)
{
	return NULL;
}
#endif

/*
 * do_mbind() - 为 current 的地址范围安装 VMA 策略，并按请求迁移已有 folio
 * 调用位置：kernel_mbind() 完成 ABI 解析后进入的核心实现；成功后 fault/分配路径消费策略。
 * @start/@len: 用户虚拟地址起点和字节长度；起点须页对齐，长度向上对齐。
 * @mode/@mode_flags/@nmask: 已净化的模式、持久标志与内核节点掩码；@flags: STRICT、
 * MOVE、MOVE_ALL 等操作标志。
 * 进程上下文，可分配、持 mmap 写锁并同步迁移。返回 0 或 -EINVAL/-EPERM/-ENOMEM/
 * -EFAULT/-EIO 等。VMA 策略修改不会因后续 VMA 失败而整体回滚；隔离但未迁移的 folio
 * 在出口放回。new 的引用、LRU cache 禁用状态和 mmap 锁均由本函数闭环。
 */
static long do_mbind(unsigned long start, unsigned long len,
		     unsigned short mode, unsigned short mode_flags,
		     nodemask_t *nmask, unsigned long flags)
{
	/* mm/vma/prev/vmi 驱动 VMA 修改；mmpol 为迁移分配回调；pagelist 接收隔离 folio。 */
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma, *prev;
	struct vma_iterator vmi;
	struct migration_mpol mmpol;
	struct mempolicy *new;
	unsigned long end;
	long err;
	long nr_failed;
	LIST_HEAD(pagelist);

	if (flags & ~(unsigned long)MPOL_MF_VALID)
		/* ABI 只接受已定义操作位，防止内部标志被用户伪造。 */
		return -EINVAL;
	if ((flags & MPOL_MF_MOVE_ALL) && !capable(CAP_SYS_NICE))
		/* MOVE_ALL 可搬动其他映射共享页，影响范围更大，必须有 CAP_SYS_NICE。 */
		return -EPERM;

	if (start & ~PAGE_MASK)
		return -EINVAL;

	if (mode == MPOL_DEFAULT)
		/* DEFAULT 没有可严格验证的专属节点集合，STRICT 对其无意义。 */
		flags &= ~MPOL_MF_STRICT;

	len = PAGE_ALIGN(len);
	/* 对齐后检查加法回绕；空区间是无副作用成功。 */
	end = start + len;

	if (end < start)
		return -EINVAL;
	if (end == start)
		return 0;

	new = mpol_new(mode, mode_flags, nmask);
	/* new 为尚未发布、refcnt=1 的模板；NULL 是合法 DEFAULT 表示。 */
	if (IS_ERR(new))
		return PTR_ERR(new);

	/*
	 * If we are using the default policy then operation
	 * on discontinuous address spaces is okay after all
	 */
	/* DEFAULT 只是清除显式策略，地址空间中的空洞不应阻止这一操作。 */
	if (!new)
		flags |= MPOL_MF_DISCONTIG_OK;

	if (flags & (MPOL_MF_MOVE | MPOL_MF_MOVE_ALL))
		/* 迁移期间关闭 per-CPU LRU 批处理，使隔离状态与全局 LRU 观察保持一致。 */
		lru_cache_disable();
	{
		NODEMASK_SCRATCH(scratch);
		if (scratch) {
			mmap_write_lock(mm);
			/* 阶段 2：写锁同时保护节点绑定、页表扫描及随后 VMA 结构修改。 */
			err = mpol_set_nodemask(new, nmask, scratch);
			if (err)
				mmap_write_unlock(mm);
		} else
			err = -ENOMEM;
		NODEMASK_SCRATCH_FREE(scratch);
	}
	if (err)
		goto mpol_out;

	/*
	 * Lock the VMAs before scanning for pages to migrate,
	 * to ensure we don't miss a concurrently inserted page.
	 */
	/* 写锁覆盖扫描与策略安装，阻止并发 fault 在二者之间插入一页而逃过迁移选择。 */
	nr_failed = queue_pages_range(mm, start, end, nmask,
			flags | MPOL_MF_INVERT | MPOL_MF_WRLOCK, &pagelist);

	if (nr_failed < 0) {
		/* 结构/严格错误阻止策略安装；已隔离列表仍在统一出口放回。 */
		err = nr_failed;
		nr_failed = 0;
	} else {
		/* 阶段 3：按源码地址顺序修改相交 VMA；此前的成功修改不会因后续错误撤销。 */
		vma_iter_init(&vmi, mm, start);
		prev = vma_prev(&vmi);
		for_each_vma_range(vmi, vma, end) {
			err = mbind_range(&vmi, vma, &prev, start, end, new);
			if (err)
				break;
		}
	}

	if (!err && !list_empty(&pagelist)) {
		/* Convert MPOL_DEFAULT's NULL to task or default policy */
		/* 迁移分配需要一个实际策略对象，DEFAULT(NULL) 在此解析为任务或系统有效策略。 */
		if (!new) {
			new = get_task_policy(current);
			mpol_get(new);
		}
		mmpol.pol = new;
		mmpol.ilx = 0;

		/*
		 * In the interleaved case, attempt to allocate on exactly the
		 * targeted nodes, for the first VMA to be migrated; for later
		 * VMAs, the nodes will still be interleaved from the targeted
		 * nodemask, but one by one may be selected differently.
		 */
		/*
		 * 交错迁移要让首个 VMA 的第一个可定位 folio 与其原地址索引对齐。KSM folio
		 * 无法唯一反查 VMA，故跳过；后续 VMA 仍在同一节点集合内交错，但起点可不同。
		 */
		if (new->mode == MPOL_INTERLEAVE ||
		    new->mode == MPOL_WEIGHTED_INTERLEAVE) {
			struct folio *folio;
			unsigned int order;
			unsigned long addr = -EFAULT;

			list_for_each_entry(folio, &pagelist, lru) {
				if (!folio_test_ksm(folio))
					break;
			}
			/* 找到首个非 KSM folio 后，反查包含它的 VMA 和虚拟地址以恢复交错索引。 */
			if (!list_entry_is_head(folio, &pagelist, lru)) {
				vma_iter_init(&vmi, mm, start);
				for_each_vma_range(vmi, vma, end) {
					addr = page_address_in_vma(folio,
						folio_page(folio, 0), vma);
					if (addr != -EFAULT)
						break;
				}
			}
			if (addr != -EFAULT) {
				order = folio_order(folio);
				/* We already know the pol, but not the ilx */
				/* 策略已知，只借 helper 计算 ilx；共享策略可能带临时引用，立即 cond_put。 */
				mpol_cond_put(get_vma_policy(vma, addr, order,
							     &mmpol.ilx));
				/* Set base from which to increment by index */
				/* 把当前 folio 的绝对 ilx 换算为基准，回调再按每个 src->index 增量恢复。 */
				mmpol.ilx -= folio->index >> order;
			}
		}
	}

	mmap_write_unlock(mm);
	/* 阶段 4：VMA/策略已经稳定发布；解锁后隔离引用足以支撑同步迁移。 */

	if (!err && !list_empty(&pagelist)) {
		nr_failed |= migrate_pages(&pagelist,
				alloc_migration_target_by_mpol, NULL,
				(unsigned long)&mmpol, MIGRATE_SYNC,
				MR_MEMPOLICY_MBIND, NULL);
	}

	if (nr_failed && (flags & MPOL_MF_STRICT))
		/* STRICT 把任一扫描或迁移失败折叠为 -EIO，但已完成迁移/策略修改不回滚。 */
		err = -EIO;
	if (!list_empty(&pagelist))
		putback_movable_pages(&pagelist);
mpol_out:
	/* 统一交还策略模板引用并恢复 LRU cache 状态；NULL/static 借用由 mpol_put 契约处理。 */
	mpol_put(new);
	if (flags & (MPOL_MF_MOVE | MPOL_MF_MOVE_ALL))
		lru_cache_enable();
	return err;
}

/*
 * User space interface with variable sized bitmaps for nodelists.
 */
/*
 * get_bitmap() - 从用户态读取任意位数的节点位图片段
 * 调用位置：get_nodes() 的底层 uaccess helper；失败后上层不使用部分输出。
 * @mask: 内核输出缓冲区；@nmask: 用户只读指针；@maxnode: 有效位数而非最大 nid。
 * 可发生用户访问 fault；兼容系统调用按 compat_ulong_t 布局转换。返回 0 或 -EFAULT。
 * 最后一个机器字超出 maxnode 的高位被清零，避免未初始化位进入节点校验。
 * 上下文：系统调用进程上下文；用户复制可能 fault，但入口不持自旋锁。
 */
static int get_bitmap(unsigned long *mask, const unsigned long __user *nmask,
		      unsigned long maxnode)
{
	/* nlongs 是本机布局所需字数；ret 为未复制字节数/compat 状态，非零统一映射 EFAULT。 */
	unsigned long nlongs = BITS_TO_LONGS(maxnode);
	int ret;

	/* 阶段 1：按调用 ABI 选择 32 位兼容转换或本机 unsigned long 直接复制。 */
	if (in_compat_syscall())
		ret = compat_get_bitmap(mask,
					(const compat_ulong_t __user *)nmask,
					maxnode);
	else
		ret = copy_from_user(mask, nmask,
				     nlongs * sizeof(unsigned long));

	if (ret)
		return -EFAULT;

	if (maxnode % BITS_PER_LONG)
		/* 只保留尾字中用户声明有效的低位。 */
		mask[nlongs - 1] &= (1UL << (maxnode % BITS_PER_LONG)) - 1;

	return 0;
}

/* Copy a node mask from user space. */
/*
 * get_nodes() - 将用户变长节点位图规范化为内核 nodemask_t
 * 调用位置：mbind/set_mempolicy/migrate_pages 的 wrapper 在进入核心逻辑前调用。
 * @nodes: 输出，进入即清空；@nmask: 可为 NULL 的用户位图；@maxnode: ABI 的“最大节点号
 * 加一”，函数先减一以沿用历史边界语义。
 * 返回 0、-EFAULT 或 -EINVAL。支持用户传入超过 MAX_NUMNODES 的零扩展位，但任何不受
 * 支持的置位都拒绝；扫描上限为一页，限制用户指针读取成本。可因 uaccess fault 失败。
 * 上下文：系统调用进程上下文，可触发用户缺页；不持 task、mmap 或 cpuset 锁。
 */
static int get_nodes(nodemask_t *nodes, const unsigned long __user *nmask,
		     unsigned long maxnode)
{
	/* 阶段 1：把 ABI 上界换为有效位数并预清输出，空输入因此得到确定空集。 */
	--maxnode;
	nodes_clear(*nodes);
	if (maxnode == 0 || !nmask)
		return 0;
	if (maxnode > PAGE_SIZE*BITS_PER_BYTE)
		return -EINVAL;

	/*
	 * When the user specified more nodes than supported just check
	 * if the non supported part is all zero, one word at a time,
	 * starting at the end.
	 */
	/*
	 * 用户声明的位数超过内核能力时，从尾部逐机器字验证扩展区域全零；这样 ABI 可用
	 * 更大固定位图与较小内核通信，同时不会静默丢弃用户实际请求的未知节点。
	 */
	while (maxnode > MAX_NUMNODES) {
		/* 每轮从用户位图尾部验证一个字，逐步缩小到内核可表示范围。 */
		unsigned long bits = min_t(unsigned long, maxnode, BITS_PER_LONG);
		unsigned long t;

		if (get_bitmap(&t, &nmask[(maxnode - 1) / BITS_PER_LONG], bits))
			return -EFAULT;

		/* 完整的超范围字直接丢弃；跨越边界的最后一字只屏蔽内核可表示低位。 */
		if (maxnode - bits >= MAX_NUMNODES) {
			maxnode -= bits;
		} else {
			maxnode = MAX_NUMNODES;
			t &= ~((1UL << (MAX_NUMNODES % BITS_PER_LONG)) - 1);
		}
		if (t)
			return -EINVAL;
	}

	return get_bitmap(nodes_addr(*nodes), nmask, maxnode);
}

/* Copy a kernel node mask to user space */
/*
 * copy_nodes_to_user() - 按 native/compat ABI 把内核 nodemask 复制给用户
 * 调用位置：kernel_get_mempolicy() 在内部查询成功后的最终用户输出阶段。
 * @mask: 用户输出缓冲区；@maxnode: 用户声明容量；@nodes: 内核只读掩码。
 * 返回 0、-EINVAL（请求清零范围超过一页）或 -EFAULT。若用户缓冲区比内核位图大，先
 * 清零尾部再复制有效字节，防止泄漏与旧数据伪装成节点；不改变 nodes 所有权。
 * 上下文：系统调用进程上下文，可触发用户写 fault；不持 task_lock/mmap_lock。
 */
static int copy_nodes_to_user(unsigned long __user *mask, unsigned long maxnode,
			      nodemask_t *nodes)
{
	/* copy 是用户 ABI 字节数，nbytes 是当前内核有效节点表示的 native/compat 字节数。 */
	unsigned long copy = ALIGN(maxnode-1, 64) / 8;
	unsigned int nbytes = BITS_TO_LONGS(nr_node_ids) * sizeof(long);
	bool compat = in_compat_syscall();

	/* 阶段 1：根据 ABI 计算内核有效表示长度，之后统一处理用户扩展尾部。 */
	if (compat)
		nbytes = BITS_TO_COMPAT_LONGS(nr_node_ids) * sizeof(compat_long_t);

	if (copy > nbytes) {
		/* 扩展尾部最多允许一页并必须显式清零，然后把实际复制限制到内核位图长度。 */
		if (copy > PAGE_SIZE)
			return -EINVAL;
		if (clear_user((char __user *)mask + nbytes, copy - nbytes))
			return -EFAULT;
		copy = nbytes;
		maxnode = nr_node_ids;
	}

	if (compat)
		return compat_put_bitmap((compat_ulong_t __user *)mask,
					 nodes_addr(*nodes), maxnode);

	return copy_to_user(mask, nodes_addr(*nodes), copy) ? -EFAULT : 0;
}

/* Basic parameter sanity check used by both mbind() and set_mempolicy() */
/*
 * sanitize_mpol_flags() - 从复合 mode 整数拆出并验证 NUMA 策略模式标志
 * 调用位置：kernel_mbind()/kernel_set_mempolicy() 的首个纯参数校验阶段。
 * @mode: 输入输出，进入含模式与 MPOL_MODE_FLAGS，返回仅基础模式；@flags: 输出持久位。
 * 返回 0 或 -EINVAL。STATIC 与 RELATIVE 互斥；NUMA_BALANCING 只允许 BIND 或
 * PREFERRED_MANY，并派生内核使用的 MOF/MORON 位。纯参数变换、不睡眠。
 */
static inline int sanitize_mpol_flags(int *mode, unsigned short *flags)
{
	*flags = *mode & MPOL_MODE_FLAGS;
	*mode &= ~MPOL_MODE_FLAGS;

	if ((unsigned int)(*mode) >=  MPOL_MAX)
		return -EINVAL;
	if ((*flags & MPOL_F_STATIC_NODES) && (*flags & MPOL_F_RELATIVE_NODES))
		return -EINVAL;
	if (*flags & MPOL_F_NUMA_BALANCING) {
		if (*mode == MPOL_BIND || *mode == MPOL_PREFERRED_MANY)
			*flags |= (MPOL_F_MOF | MPOL_F_MORON);
		else
			return -EINVAL;
	}
	return 0;
}

/*
 * kernel_mbind() - mbind 系统调用的参数复制与内部语义桥接层
 * @start/@len: 用户地址范围；@mode: 复合模式；@nmask/@maxnode: 用户节点位图；@flags:
 * 操作标志。先剥离地址 tag，再净化模式并复制位图，最后调用 do_mbind。
 * 返回各阶段 errno 或 do_mbind 结果；失败于解析时尚未修改任何 VMA。
 * 上下文：系统调用进程上下文，可因用户复制及后续 do_mbind 睡眠；入口不持锁。
 */
static long kernel_mbind(unsigned long start, unsigned long len,
			 unsigned long mode, const unsigned long __user *nmask,
			 unsigned long maxnode, unsigned int flags)
{
	/* mode_flags/lmode 分离 ABI 复合整数，nodes 是用户位图的内核私有副本。 */
	unsigned short mode_flags;
	nodemask_t nodes;
	int lmode = mode;
	int err;

	/* 阶段 1：先规范地址与模式；任一失败都早于用户位图复制和 VMA 修改。 */
	start = untagged_addr(start);
	err = sanitize_mpol_flags(&lmode, &mode_flags);
	if (err)
		return err;

	err = get_nodes(&nodes, nmask, maxnode);
	if (err)
		return err;

	/* 阶段 2：从这里进入可修改 VMA/迁移 folio 的核心层，nodes 覆盖整个调用。 */
	return do_mbind(start, len, lmode, mode_flags, &nodes, flags);
}

/*
 * set_mempolicy_home_node() - 为 BIND/PREFERRED_MANY VMA 设置分配 home node
 * 调用位置：用户系统调用入口直接执行；成功后 policy_nodemask() 在分配中读取。
 * @start/@len: 页对齐范围；@home_node: 必须在线的有效 nid；@flags: 当前必须为 0。
 * 持 mmap 写锁逐 VMA 复制旧策略、修改副本并通过 mbind_range 发布。返回 0/最后结果或
 * -ENOENT/-EINVAL/-EOPNOTSUPP/-ENOMEM。该操作不是事务：遇错前已更新的 VMA 不回滚；
 * 无显式策略的 VMA 被跳过。home_node 只影响支持模式的分配偏好，不迁移现有页。
 * 上下文：系统调用进程上下文，可睡眠；本函数独占 current->mm 的 mmap 写锁。
 */
SYSCALL_DEFINE4(set_mempolicy_home_node, unsigned long, start, unsigned long, len,
		unsigned long, home_node, unsigned long, flags)
{
	/* new 是每个 VMA 的临时策略副本，old 为锁内借用；err 初值表示范围未命中可更新项。 */
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma, *prev;
	struct mempolicy *new, *old;
	unsigned long end;
	int err = -ENOENT;
	VMA_ITERATOR(vmi, mm, start);

	start = untagged_addr(start);
	if (start & ~PAGE_MASK)
		return -EINVAL;
	/*
	 * flags is used for future extension if any.
	 */
	/* flags 预留给未来扩展；当前拒绝任何非零值，保持 ABI 可演进空间。 */
	if (flags != 0)
		return -EINVAL;

	/*
	 * Check home_node is online to avoid accessing uninitialized
	 * NODE_DATA.
	 */
	/* 在线检查保证 NODE_DATA 已初始化，不能只验证 nid 数值范围。 */
	if (home_node >= MAX_NUMNODES || !node_online(home_node))
		return -EINVAL;

	len = PAGE_ALIGN(len);
	end = start + len;

	if (end < start)
		return -EINVAL;
	if (end == start)
		return 0;
	mmap_write_lock(mm);
	/* 写锁稳定 VMA 与策略指针，prev/vmi 维护拆分合并后的迭代位置。 */
	prev = vma_prev(&vmi);
	for_each_vma_range(vmi, vma, end) {
		/*
		 * If any vma in the range got policy other than MPOL_BIND
		 * or MPOL_PREFERRED_MANY we return error. We don't reset
		 * the home node for vmas we already updated before.
		 */
		/*
		 * 只有 BIND/PREFERRED_MANY 定义 home_node。遇到其他显式策略立即报错，但此前
		 * VMA 的发布已经对 fault 可见，按 ABI 明确不执行事务回滚。
		 */
		old = vma_policy(vma);
		if (!old) {
			prev = vma;
			continue;
		}
		if (old->mode != MPOL_BIND && old->mode != MPOL_PREFERRED_MANY) {
			err = -EOPNOTSUPP;
			break;
		}
		new = mpol_dup(old);
		/* 修改副本而非共享 old，避免同一策略引用被其他 VMA 同时观察到半更新。 */
		if (IS_ERR(new)) {
			err = PTR_ERR(new);
			break;
		}

		/* 阶段 3：持 VMA 写锁发布副本；mbind_range 负责相邻区间拆分及合并。 */
		vma_start_write(vma);
		new->home_node = home_node;
		err = mbind_range(&vmi, vma, &prev, start, end, new);
		mpol_put(new);
		if (err)
			break;
	}
	mmap_write_unlock(mm);
	return err;
}

/*
 * mbind() - 用户 ABI 入口；系统调用宏生成体系结构 wrapper，参数原样交给 kernel_mbind。
 * 返回值、睡眠性和所有副作用均由 kernel_mbind/do_mbind 契约定义。
 */
SYSCALL_DEFINE6(mbind, unsigned long, start, unsigned long, len,
		unsigned long, mode, const unsigned long __user *, nmask,
		unsigned long, maxnode, unsigned int, flags)
{
	return kernel_mbind(start, len, mode, nmask, maxnode, flags);
}

/* Set the process memory policy */
/*
 * kernel_set_mempolicy() - set_mempolicy 的用户位图解析层
 * @mode: 复合模式；@nmask/@maxnode: 用户位图及位数。净化模式、复制节点后调用
 * do_set_mempolicy。返回 0 或解析/构造 errno；解析失败不改变 current 策略。
 * 调用位置：set_mempolicy() ABI wrapper；可因 uaccess/策略分配睡眠，入口不持锁。
 */
static long kernel_set_mempolicy(int mode, const unsigned long __user *nmask,
				 unsigned long maxnode)
{
	/* 阶段 1：拆分验证模式；阶段 2：复制位图；成功后才允许发布 current 策略。 */
	unsigned short mode_flags;
	nodemask_t nodes;
	int lmode = mode;
	int err;

	err = sanitize_mpol_flags(&lmode, &mode_flags);
	if (err)
		return err;

	err = get_nodes(&nodes, nmask, maxnode);
	if (err)
		return err;

	/* 两阶段验证均成功，才把规范化后的模式与内核节点副本交给发布核心。 */
	return do_set_mempolicy(lmode, mode_flags, &nodes);
}

/*
 * set_mempolicy() - 设置调用线程默认 NUMA 策略的 ABI 入口；宏 wrapper 把用户参数交给
 * kernel_set_mempolicy，成功只影响 current，既有 VMA 显式策略仍优先且现有页不迁移。
 * 返回：0 或 kernel_set_mempolicy 的负 errno；系统调用进程上下文，可睡眠，入口不持锁。
 */
SYSCALL_DEFINE3(set_mempolicy, int, mode, const unsigned long __user *, nmask,
		unsigned long, maxnode)
{
	return kernel_set_mempolicy(mode, nmask, maxnode);
}

/*
 * kernel_migrate_pages() - 校验权限与 cpuset 后迁移指定进程的现有页面
 * 调用位置：migrate_pages() ABI wrapper；授权完成后调用 do_migrate_pages()。
 * @pid: 0 表示 current，否则目标虚拟 pid；@maxnode/@old_nodes/@new_nodes: 用户位图。
 * 可睡眠；RCU 只用于稳定 task 查找，随后 get_task_struct/get_task_mm 转为长期引用。
 * 返回 -ENOMEM/-EFAULT/-ESRCH/-EPERM/-EINVAL/LSM errno 或未迁移页数。无 CAP_SYS_NICE
 * 时只能 MOVE 非共享页；目标集合被限制为目标与调用者 cpuset 可访问交集。所有引用和
 * scratch 在标签路径逆序释放。
 */
static int kernel_migrate_pages(pid_t pid, unsigned long maxnode,
				const unsigned long __user *old_nodes,
				const unsigned long __user *new_nodes)
{
	/* old/new 借用 NODEMASK_SCRATCH 两个掩码；task/mm 分别用显式引用跨越 RCU/退出竞态。 */
	struct mm_struct *mm = NULL;
	struct task_struct *task;
	nodemask_t task_nodes;
	int err;
	nodemask_t *old;
	nodemask_t *new;
	NODEMASK_SCRATCH(scratch);

	/* scratch 同时容纳两个完整掩码，分配失败时尚未取得任何外部引用。 */
	if (!scratch)
		return -ENOMEM;

	old = &scratch->mask1;
	new = &scratch->mask2;

	err = get_nodes(old, old_nodes, maxnode);
	if (err)
		goto out;

	err = get_nodes(new, new_nodes, maxnode);
	if (err)
		goto out;
	/* 阶段 1：两个用户掩码均已完整复制，之后不再解引用用户指针。 */

	/* Find the mm_struct */
	/* 阶段 2：RCU 保证 pid 查找得到的 task 内存暂存，get_task_struct 把它升级为长期引用。 */
	rcu_read_lock();
	task = pid ? find_task_by_vpid(pid) : current;
	if (!task) {
		rcu_read_unlock();
		err = -ESRCH;
		goto out;
	}
	get_task_struct(task);

	err = -EINVAL;

	/*
	 * Check if this process has the right to modify the specified process.
	 * Use the regular "ptrace_may_access()" checks.
	 */
	/* 使用 ptrace REALCREDS 规则统一跨进程可见性/凭据检查，而不是只比较 uid。 */
	if (!ptrace_may_access(task, PTRACE_MODE_READ_REALCREDS)) {
		rcu_read_unlock();
		err = -EPERM;
		goto out_put;
	}
	rcu_read_unlock();

	task_nodes = cpuset_mems_allowed(task);
	/* Is the user allowed to access the target nodes? */
	/* 非特权调用者不能把目标任务页面移到其 cpuset 之外。 */
	if (!nodes_subset(*new, task_nodes) && !capable(CAP_SYS_NICE)) {
		err = -EPERM;
		goto out_put;
	}

	task_nodes = cpuset_mems_allowed(current);
	/* 再与调用者可访问集合求交；空交集保留初始 -EINVAL，避免越权触碰目标节点。 */
	if (!nodes_and(*new, *new, task_nodes))
		goto out_put;

	err = security_task_movememory(task);
	/* LSM 在取得 mm 前拥有最终授权机会。 */
	if (err)
		goto out_put;

	mm = get_task_mm(task);
	put_task_struct(task);
	/* mm 引用独立于 task 生命周期；一旦取得即可释放 task，目标随后退出也不 UAF。 */

	if (!mm) {
		err = -EINVAL;
		goto out;
	}
	/* 阶段 4：持 mm 引用执行迁移；目标 task 此后退出也不影响地址空间生命周期。 */

	err = do_migrate_pages(mm, old, new,
		capable(CAP_SYS_NICE) ? MPOL_MF_MOVE_ALL : MPOL_MF_MOVE);

	mmput(mm);
out:
	NODEMASK_SCRATCH_FREE(scratch);

	return err;

out_put:
	/* 仅到达这里的路径仍持 task 引用；释放后复用 scratch 清理出口。 */
	put_task_struct(task);
	goto out;
}

/*
 * migrate_pages() - 跨进程页迁移 ABI 入口；wrapper 只转交参数，具体权限、引用与返回
 * 语义由 kernel_migrate_pages/do_migrate_pages 定义。
 */
SYSCALL_DEFINE4(migrate_pages, pid_t, pid, unsigned long, maxnode,
		const unsigned long __user *, old_nodes,
		const unsigned long __user *, new_nodes)
{
	return kernel_migrate_pages(pid, maxnode, old_nodes, new_nodes);
}

/* Retrieve NUMA policy */
/*
 * kernel_get_mempolicy() - 将内部策略查询结果安全复制回用户态
 * @policy: 可为 NULL 的用户整数输出；@nmask: 可为 NULL 的用户位图输出；@maxnode:
 * 位图容量；@addr/@flags: 查询位置与模式。
 * 先要求 nmask 容量覆盖 nr_node_ids，再去除地址 tag，调用 do_get_mempolicy。返回 0 或
 * -EINVAL/-EFAULT/内部查询 errno。内部结果先落在栈变量，只有查询成功才逐项 copy_to_user；
 * policy 已写而 nmask 复制失败时不会回滚用户内存。
 * 调用位置：get_mempolicy() ABI wrapper；系统调用进程上下文，可睡眠，入口不持锁。
 */
static int kernel_get_mempolicy(int __user *policy,
				unsigned long __user *nmask,
				unsigned long maxnode,
				unsigned long addr,
				unsigned long flags)
{
	/* pval/nodes 先承接完整内核结果，避免 do_get_mempolicy 持锁时写用户内存。 */
	int err;
	int pval;
	nodemask_t nodes;

	if (nmask != NULL && maxnode < nr_node_ids)
		return -EINVAL;

	addr = untagged_addr(addr);

	err = do_get_mempolicy(&pval, &nodes, addr, flags);

	if (err)
		return err;
	/* 阶段 2：内部查询已释放锁与引用，现按 ABI 顺序复制两个可选输出。 */

	if (policy && put_user(pval, policy))
		return -EFAULT;

	if (nmask)
		err = copy_nodes_to_user(nmask, maxnode, &nodes);

	return err;
}

/*
 * get_mempolicy() - NUMA 策略查询 ABI wrapper
 * 调用位置：体系结构系统调用分派 -> 本 wrapper -> kernel_get_mempolicy()。
 * 全部用户指针只转交下层；返回 0 或查询/uaccess errno。进程上下文，可睡眠，入口不持锁。
 */
SYSCALL_DEFINE5(get_mempolicy, int __user *, policy,
		unsigned long __user *, nmask, unsigned long, maxnode,
		unsigned long, addr, unsigned long, flags)
{
	return kernel_get_mempolicy(policy, nmask, maxnode, addr, flags);
}

/*
 * vma_migratable() - 判断 VMA 的页是否具备 NUMA 节点间迁移能力
 * @vma: 借用且由调用者锁定/稳定的 VMA。纯属性查询，不取得引用、不睡眠。
 * 返回 false：IO/PFNMAP、DAX、架构不支持迁移的 hugetlb，或文件映射 GFP zone 低于
 * policy_zone；其他返回 true。true 只是类型能力判断，具体 folio 仍可能被 pin/共享而失败。
 */
bool vma_migratable(struct vm_area_struct *vma)
{
	if (vma->vm_flags & (VM_IO | VM_PFNMAP))
		return false;

	/*
	 * DAX device mappings require predictable access latency, so avoid
	 * incurring periodic faults.
	 */
	/* DAX 依赖设备持久内存的可预测访问时延，周期性提示 fault/迁移会破坏该模型。 */
	if (vma_is_dax(vma))
		return false;

	if (is_vm_hugetlb_page(vma) &&
		!hugepage_migration_supported(hstate_vma(vma)))
		return false;

	/*
	 * Migration allocates pages in the highest zone. If we cannot
	 * do so then migration (at least from node to node) is not
	 * possible.
	 */
	/* 节点迁移按最高 zone 分配目标；文件 mapping 若限制在更低 zone，就没有合法目标。 */
	if (vma->vm_file &&
		gfp_zone(mapping_gfp_mask(vma->vm_file->f_mapping))
			< policy_zone)
		return false;
	return true;
}

/*
 * __get_vma_policy() - 只查询地址处显式 VMA/文件系统共享策略
 * @vma: 锁内借用 VMA；@addr: VMA 内地址；@ilx: 输出交错索引，进入先清零。
 * 若 vm_ops->get_policy 存在则间接分派（shmem 等可返回共享策略并增加条件引用），否则
 * 返回 vm_policy 借用指针；可返回 NULL，且绝不回退任务策略。调用者负责 mpol_cond_put。
 */
struct mempolicy *__get_vma_policy(struct vm_area_struct *vma,
				   unsigned long addr, pgoff_t *ilx)
{
	*ilx = 0;
	return (vma->vm_ops && vma->vm_ops->get_policy) ?
		vma->vm_ops->get_policy(vma, addr, ilx) : vma->vm_policy;
}

/*
 * get_vma_policy(@vma, @addr, @order, @ilx)
 * @vma: virtual memory area whose policy is sought
 * @addr: address in @vma for shared policy lookup
 * @order: 0, or appropriate huge_page_order for interleaving
 * @ilx: interleave index (output), for use only when MPOL_INTERLEAVE or
 *       MPOL_WEIGHTED_INTERLEAVE
 *
 * Returns effective policy for a VMA at specified address.
 * Falls back to current->mempolicy or system default policy, as necessary.
 * Shared policies [those marked as MPOL_F_SHARED] require an extra reference
 * count--added by the get_policy() vm_op, as appropriate--to protect against
 * freeing by another task.  It is the caller's responsibility to free the
 * extra reference for shared policies.
 */
/*
 * get_vma_policy() - 求某 VMA 地址最终生效的策略及静态交错索引
 * @vma: mmap 锁保护的借用 VMA；@addr: VMA 内地址；@order: 0 或大页阶数；@ilx: 输出。
 * 先查 VMA/共享策略，NULL 时回退 current 策略/系统默认；INTERLEAVE 类把 vm_pgoff 与
 * VMA 内页偏移折算到对应 order 的索引。返回借用策略，或带 MPOL_F_SHARED 的条件引用；
 * 调用者必须 mpol_cond_put，不能无条件 put 静态/任务策略。
 */
struct mempolicy *get_vma_policy(struct vm_area_struct *vma,
				 unsigned long addr, int order, pgoff_t *ilx)
{
	/* pol 的引用性质取决于 vm_ops；ilx 只有交错模式才供分配选择使用。 */
	struct mempolicy *pol;

	pol = __get_vma_policy(vma, addr, ilx);
	if (!pol)
		pol = get_task_policy(current);
	if (pol->mode == MPOL_INTERLEAVE ||
	    pol->mode == MPOL_WEIGHTED_INTERLEAVE) {
		*ilx += vma->vm_pgoff >> order;
		*ilx += (addr - vma->vm_start) >> (PAGE_SHIFT + order);
		/* 文件偏移与 VMA 内偏移相加，使同一 backing object 在不同映射中保持交错一致。 */
	}
	return pol;
}

/*
 * vma_policy_mof() - 查询 VMA 有效策略是否设置“迁移时跟随”MPOL_F_MOF
 * @vma: 锁内借用。文件系统 get_policy 可能返回共享策略引用，函数当场 cond_put；普通
 * VMA 则回退 current 有效策略。返回布尔值，不转移任何引用；间接回调可能受其实现约束。
 */
bool vma_policy_mof(struct vm_area_struct *vma)
{
	struct mempolicy *pol;

	if (vma->vm_ops && vma->vm_ops->get_policy) {
		bool ret = false;
		pgoff_t ilx;		/* ignored here */
		/* ilx 仅满足回调输出契约，本查询只关心 flags。 */

		pol = vma->vm_ops->get_policy(vma, vma->vm_start, &ilx);
		if (pol && (pol->flags & MPOL_F_MOF))
			ret = true;
		mpol_cond_put(pol);

		return ret;
	}

	pol = vma->vm_policy;
	if (!pol)
		pol = get_task_policy(current);

	return pol->flags & MPOL_F_MOF;
}

/*
 * apply_policy_zone() - 判断一次 GFP zone 请求是否应应用 BIND 节点限制
 * 调用位置：policy_nodemask() 处理 MPOL_BIND 时决定是否返回硬过滤掩码。
 * @policy: 借用有效策略；@zone: gfp_zone 的请求级别。返回布尔值，不睡眠。
 * 通常以全局 policy_zone 为阈值；若策略节点只有 ZONE_MOVABLE 内存，则动态阈值提升为
 * ZONE_MOVABLE，避免在节点根本没有普通高端内存时把较低 zone 请求错误限制为空。
 */
bool apply_policy_zone(struct mempolicy *policy, enum zone_type zone)
{
	enum zone_type dynamic_policy_zone = policy_zone;

	BUG_ON(dynamic_policy_zone == ZONE_MOVABLE);

	/*
	 * if policy->nodes has movable memory only,
	 * we apply policy when gfp_zone(gfp) = ZONE_MOVABLE only.
	 *
	 * policy->nodes is intersect with node_states[N_MEMORY].
	 * so if the following test fails, it implies
	 * policy->nodes has movable memory only.
	 */
	/* policy->nodes 已与 N_MEMORY 相交；不与 N_HIGH_MEMORY 相交即可推导其只含 movable。 */
	if (!nodes_intersects(policy->nodes, node_states[N_HIGH_MEMORY]))
		dynamic_policy_zone = ZONE_MOVABLE;

	return zone >= dynamic_policy_zone;
}

/*
 * weighted_interleave_nodes() - 为线程动态加权交错选择下一分配节点
 * 调用位置：slab、单页与 bulk 分配的动态交错路径；返回后分配器尝试该 nid。
 * @policy: current 显式策略的借用指针。返回 nid 或 MAX_NUMNODES；更新 current->il_prev
 * 和 il_weight。通过 mems_allowed_seq 检测并发 cpuset 重绑定并重试；不睡眠。
 * il_weight 表示当前节点尚可消费的分配次数，耗尽或节点被移除后才推进到下一节点。
 */
static unsigned int weighted_interleave_nodes(struct mempolicy *policy)
{
	unsigned int node;
	unsigned int cpuset_mems_cookie;

retry:
	/* to prevent miscount use tsk->mems_allowed_seq to detect rebind */
	/* seq 不是对象引用，而是检测无锁读取期间 nodemask 是否被重绑定；变化则整段重算。 */
	cpuset_mems_cookie = read_mems_allowed_begin();
	node = current->il_prev;
	if (!current->il_weight || !node_isset(node, policy->nodes)) {
		node = next_node_in(node, policy->nodes);
		if (read_mems_allowed_retry(cpuset_mems_cookie))
			goto retry;
		if (node == MAX_NUMNODES)
			return node;
		current->il_prev = node;
		current->il_weight = get_il_weight(node);
		/* 复制当前 RCU 权重到任务计数器，随后每次成功选择消费一次。 */
	}
	current->il_weight--;
	return node;
}

/* Do dynamic interleaving for a process */
/*
 * interleave_nodes() - 为线程普通交错策略选择并记录下一节点
 * 调用位置：slab/页/bulk 分配使用 NO_INTERLEAVE_INDEX 时的动态节点选择。
 * @policy: 借用策略。返回 next_node_in 结果；有效时写 current->il_prev。使用
 * mems_allowed_seq 重试并发 rebind，保证不会按混合的新旧掩码漏算/重复节点。不睡眠。
 */
static unsigned int interleave_nodes(struct mempolicy *policy)
{
	unsigned int nid;
	unsigned int cpuset_mems_cookie;

	/* to prevent miscount, use tsk->mems_allowed_seq to detect rebind */
	/* 在同一个 seq 快照内读取 il_prev 与 policy->nodes，重绑定发生则重新选择。 */
	do {
		cpuset_mems_cookie = read_mems_allowed_begin();
		nid = next_node_in(current->il_prev, policy->nodes);
	} while (read_mems_allowed_retry(cpuset_mems_cookie));

	if (nid < MAX_NUMNODES)
		current->il_prev = nid;
	return nid;
}

/*
 * Depending on the memory policy provide a node from which to allocate the
 * next slab entry.
 */
/*
 * mempolicy_slab_node() - 为当前上下文下一次 slab 分配给出首选 nid
 * 入参：无。任意上下文可调用；非任务上下文或无显式策略时返回 numa_mem_id()，不会
 * 读取可变任务策略。PREFERRED/交错按各自状态选择，BIND/PREFERRED_MANY 从 fallback
 * zonelist 中找集合内首个可用 zone，LOCAL 返回本地。返回只是首选节点，slab 分配仍可
 * 按自身 GFP 规则回退；动态交错模式会推进 current 计数状态。
 */
unsigned int mempolicy_slab_node(void)
{
	/* policy 是仅在 current 上下文安全借用的显式策略；node 是本地内存 nid 兜底。 */
	struct mempolicy *policy;
	int node = numa_mem_id();

	if (!in_task())
		return node;

	policy = current->mempolicy;
	if (!policy)
		return node;

	/* 显式策略只读借用；各分支返回首选 nid，不在此执行实际 slab 分配。 */
	switch (policy->mode) {
	case MPOL_PREFERRED:
		return first_node(policy->nodes);

	case MPOL_INTERLEAVE:
		return interleave_nodes(policy);

	case MPOL_WEIGHTED_INTERLEAVE:
		return weighted_interleave_nodes(policy);

	case MPOL_BIND:
	case MPOL_PREFERRED_MANY:
	{
		struct zoneref *z;

		/*
		 * Follow bind policy behavior and start allocation at the
		 * first node.
		 */
		/* BIND 类按 zonelist 顺序从策略集合首个可用 zone 开始；找不到则保留本地兜底。 */
		struct zonelist *zonelist;
		enum zone_type highest_zoneidx = gfp_zone(GFP_KERNEL);
		zonelist = &NODE_DATA(node)->node_zonelists[ZONELIST_FALLBACK];
		z = first_zones_zonelist(zonelist, highest_zoneidx,
							&policy->nodes);
		return zonelist_zone(z) ? zonelist_node_idx(z) : node;
	}
	case MPOL_LOCAL:
		return node;

	default:
		BUG();
	}
}

/*
 * read_once_policy_nodemask() - 为无锁分配选择复制一个局部 nodemask 快照
 * 调用位置：interleave_nid()/weighted_interleave_nid() 遍历策略节点之前。
 * @pol: 借用策略，可能被 cpuset rebind 并发改写；@mask: 栈上输出副本。
 * 返回副本节点数。编译器 barrier 防止 memcpy 与遍历被重排/合并，但不提供硬件原子
 * 快照；允许短暂不一致，因为最终分配器还会按 mems_allowed 验证节点合法性。
 */
static unsigned int read_once_policy_nodemask(struct mempolicy *pol,
					      nodemask_t *mask)
{
	/*
	 * barrier stabilizes the nodemask locally so that it can be iterated
	 * over safely without concern for changes. Allocators validate node
	 * selection does not violate mems_allowed, so this is safe.
	 */
	/* 这里追求“可安全遍历的本地副本”而非强一致；后级 cpuset 检查是正确性兜底。 */
	barrier();
	memcpy(mask, &pol->nodes, sizeof(nodemask_t));
	barrier();
	return nodes_weight(*mask);
}

/*
 * weighted_interleave_nid() - 按稳定地址索引计算加权交错目标节点
 * 调用位置：VMA 缺页、hugetlb 与迁移目标按 backing-object ilx 选择节点。
 * @pol: 借用策略；@ilx: backing object/映射中的页索引。返回确定性 nid；空掩码回退
 * 当前节点。先复制策略 nodemask，再在 RCU 下读取权重快照，计算 ilx 对总权重取模后
 * 落入某节点权重区间。不修改线程动态计数，也不把 RCU 指针带出临界区。
 */
static unsigned int weighted_interleave_nid(struct mempolicy *pol, pgoff_t ilx)
{
	/* nodemask 是本地快照；table 在 RCU 内借用；target 是一轮总权重中的偏移。 */
	struct weighted_interleave_state *state;
	nodemask_t nodemask;
	unsigned int target, nr_nodes;
	u8 *table = NULL;
	unsigned int weight_total = 0;
	u8 weight;
	int nid = 0;

	nr_nodes = read_once_policy_nodemask(pol, &nodemask);
	if (!nr_nodes)
		return numa_node_id();

	/* 节点集合已脱离策略生命周期；RCU 临界区只保护可热更新的权重表。 */
	rcu_read_lock();

	state = rcu_dereference(wi_state);
	/* Uninitialized wi_state means we should assume all weights are 1 */
	/* NULL 快照按系统默认全 1 解释，与 get_il_weight 的默认契约一致。 */
	if (state)
		table = state->iw_table;

	/* calculate the total weight */
	/* 只累计策略包含节点，在线表中的其他 nid 不影响本策略周期。 */
	for_each_node_mask(nid, nodemask)
		weight_total += table ? table[nid] : 1;

	/* Calculate the node offset based on totals */
	/* 把索引映射到权重区间；每减去一个节点权重就推进到下一个策略节点。 */
	target = ilx % weight_total;
	nid = first_node(nodemask);
	while (target) {
		/* detect system default usage */
		/* table 为 NULL 表示系统默认权重 1，而不是缺少该节点。 */
		weight = table ? table[nid] : 1;
		if (target < weight)
			break;
		target -= weight;
		nid = next_node_in(nid, nodemask);
	}
	rcu_read_unlock();
	return nid;
}

/*
 * Do static interleaving for interleave index @ilx.  Returns the ilx'th
 * node in pol->nodes (starting from ilx=0), wrapping around if ilx
 * exceeds the number of present nodes.
 */
/*
 * interleave_nid() - 按静态索引在策略节点集合中循环选择目标
 * 调用位置：VMA 缺页与 mpol_misplaced() 的普通交错节点计算。
 * @pol: 借用策略；@ilx: 从 0 开始的交错索引。返回第 ilx%节点数 个 nid；并发重绑定
 * 导致空快照时回退当前节点。只使用局部掩码，不修改 current->il_prev，不睡眠。
 */
static unsigned int interleave_nid(struct mempolicy *pol, pgoff_t ilx)
{
	nodemask_t nodemask;
	unsigned int target, nnodes;
	int i;
	int nid;

	/* 阶段 1：复制可遍历快照并处理并发重绑定导致的空集退化。 */
	nnodes = read_once_policy_nodemask(pol, &nodemask);
	if (!nnodes)
		return numa_node_id();
	/* 阶段 2：将无限 ilx 压入一轮节点数，再从最小 nid 顺序推进到目标。 */
	target = ilx % nnodes;
	nid = first_node(nodemask);
	for (i = 0; i < target; i++)
		nid = next_node(nid, nodemask);
	return nid;
}

/*
 * Return a nodemask representing a mempolicy for filtering nodes for
 * page allocation, together with preferred node id (or the input node id).
 */
/*
 * policy_nodemask() - 把 mempolicy 转换为页分配器的首选 nid 与过滤掩码
 * 调用位置：alloc_pages_mpol()/huge_node()/迁移回调进入实际分配器之前。
 * @gfp: 分配约束；@pol: 借用有效策略；@ilx: 静态交错索引或 NO_INTERLEAVE_INDEX；
 * @nid: 输入输出首选节点。返回 NULL 表示不限制 zonelist，或返回 pol->nodes 借用指针。
 * PREFERRED/交错覆写 nid；PREFERRED_MANY/BIND 可返回过滤集合并采用 home_node；BIND
 * 仅在 zone/cpuset 合法时限制。函数不取得引用，返回掩码只在策略保护窗口内有效。
 * 上下文：页分配热路径，不睡眠；动态交错更新 current 游标，静态路径只读快照。
 */
static nodemask_t *policy_nodemask(gfp_t gfp, struct mempolicy *pol,
				   pgoff_t ilx, int *nid)
{
	nodemask_t *nodemask = NULL;

	switch (pol->mode) {
	case MPOL_PREFERRED:
		/* Override input node id */
		/* 单首选只改变 zonelist 起点，允许普通 fallback，不返回硬过滤掩码。 */
		*nid = first_node(pol->nodes);
		break;
	case MPOL_PREFERRED_MANY:
		nodemask = &pol->nodes;
		if (pol->home_node != NUMA_NO_NODE)
			*nid = pol->home_node;
		break;
	case MPOL_BIND:
		/* Restrict to nodemask (but not on lower zones) */
		/* 只有请求 zone 可实施且仍与当前 cpuset 相容时才把 BIND 作为硬过滤器。 */
		if (apply_policy_zone(pol, gfp_zone(gfp)) &&
		    cpuset_nodemask_valid_mems_allowed(&pol->nodes))
			nodemask = &pol->nodes;
		if (pol->home_node != NUMA_NO_NODE)
			*nid = pol->home_node;
		/*
		 * __GFP_THISNODE shouldn't even be used with the bind policy
		 * because we might easily break the expectation to stay on the
		 * requested node and not break the policy.
		 */
		/* __GFP_THISNODE 与 BIND 的集合语义冲突，调用者不应组合；一次性告警但仍继续。 */
		WARN_ON_ONCE(gfp & __GFP_THISNODE);
		break;
	case MPOL_INTERLEAVE:
		/* Override input node id */
		/* 普通交错按动态任务游标或静态 ilx 覆写输入首选节点。 */
		*nid = (ilx == NO_INTERLEAVE_INDEX) ?
			interleave_nodes(pol) : interleave_nid(pol, ilx);
		break;
	case MPOL_WEIGHTED_INTERLEAVE:
		*nid = (ilx == NO_INTERLEAVE_INDEX) ?
			weighted_interleave_nodes(pol) :
			weighted_interleave_nid(pol, ilx);
		break;
	}

	return nodemask;
}

#ifdef CONFIG_HUGETLBFS
/*
 * huge_node(@vma, @addr, @gfp_flags, @mpol)
 * @vma: virtual memory area whose policy is sought
 * @addr: address in @vma for shared policy lookup and interleave policy
 * @gfp_flags: for requested zone
 * @mpol: pointer to mempolicy pointer for reference counted mempolicy
 * @nodemask: pointer to nodemask pointer for 'bind' and 'prefer-many' policy
 *
 * Returns a nid suitable for a huge page allocation and a pointer
 * to the struct mempolicy for conditional unref after allocation.
 * If the effective policy is 'bind' or 'prefer-many', returns a pointer
 * to the mempolicy's @nodemask for filtering the zonelist.
 */
/*
 * huge_node() - 为 hugetlb VMA 地址求策略、目标 nid 与可选过滤掩码
 * @vma/@addr/@gfp_flags: 锁内 VMA、其中地址和请求 zone；@mpol: 输出有效策略；
 * @nodemask: 输出 BIND/PREFERRED_MANY 过滤集合或 NULL。
 * 返回目标 nid。get_vma_policy 可能让 *mpol 带共享引用，调用者分配结束后必须
 * mpol_cond_put；*nodemask 借用该策略内部存储，不能在释放策略后使用。
 */
int huge_node(struct vm_area_struct *vma, unsigned long addr, gfp_t gfp_flags,
		struct mempolicy **mpol, nodemask_t **nodemask)
{
	pgoff_t ilx;
	int nid;

	nid = numa_node_id();
	*mpol = get_vma_policy(vma, addr, hstate_vma(vma)->order, &ilx);
	*nodemask = policy_nodemask(gfp_flags, *mpol, ilx, &nid);
	return nid;
}

/*
 * init_nodemask_of_mempolicy
 *
 * If the current task's mempolicy is "default" [NULL], return 'false'
 * to indicate default policy.  Otherwise, extract the policy nodemask
 * for 'bind' or 'interleave' policy into the argument nodemask, or
 * initialize the argument nodemask to contain the single node for
 * 'preferred' or 'local' policy and return 'true' to indicate presence
 * of non-default mempolicy.
 *
 * We don't bother with reference counting the mempolicy [mpol_get/put]
 * because the current task is examining it's own mempolicy and a task's
 * mempolicy is only ever changed by the task itself.
 *
 * N.B., it is the caller's responsibility to free a returned nodemask.
 */
/*
 * init_nodemask_of_mempolicy() - 把 current 的非默认策略转换为分配过滤掩码
 * @mask: 调用者分配的输出 nodemask，不能为空。无 current->mempolicy 返回 false 且不写；
 * 其余模式返回 true，多节点复制 policy->nodes，LOCAL 写当前 nid。
 * task_lock 防止策略被替换；因为只有任务自身修改自身策略，锁内借用无需额外 refcount。
 * 函数不接管 mask，所谓“caller free”指调用者负责其容器/临时分配生命周期。
 */
bool init_nodemask_of_mempolicy(nodemask_t *mask)
{
	struct mempolicy *mempolicy;

	if (!(mask && current->mempolicy))
		return false;

	/* task_lock 把策略指针检查与节点集合复制放在同一替换互斥窗口内。 */
	task_lock(current);
	mempolicy = current->mempolicy;
	switch (mempolicy->mode) {
	case MPOL_PREFERRED:
	case MPOL_PREFERRED_MANY:
	case MPOL_BIND:
	case MPOL_INTERLEAVE:
	case MPOL_WEIGHTED_INTERLEAVE:
		*mask = mempolicy->nodes;
		break;

	case MPOL_LOCAL:
		init_nodemask_of_node(mask, numa_node_id());
		break;

	default:
		BUG();
	}
	task_unlock(current);

	return true;
}
#endif

/*
 * mempolicy_in_oom_domain
 *
 * If tsk's mempolicy is "bind", check for intersection between mask and
 * the policy nodemask. Otherwise, return true for all other policies
 * including "interleave", as a tsk with "interleave" policy may have
 * memory allocated from all nodes in system.
 *
 * Takes task_lock(tsk) to prevent freeing of its mempolicy.
 */
/*
 * mempolicy_in_oom_domain() - 判断任务策略是否与一次 OOM 节点域相交
 * 调用位置：OOM 受害者域过滤；返回后 OOM 核心决定任务是否属于候选集合。
 * @tsk: 借用任务；@mask: 可为 NULL 的 OOM 域。NULL 或非 BIND 策略返回 true；BIND
 * 只有 nodes 相交才返回 true。task_lock 同时稳定 tsk->mempolicy 指针及其生命周期；
 * 不增加引用、不睡眠。INTERLEAVE 保守视为全系统域，因为历史分配可能散布各节点。
 */
bool mempolicy_in_oom_domain(struct task_struct *tsk,
					const nodemask_t *mask)
{
	struct mempolicy *mempolicy;
	bool ret = true;

	/* 阶段 1：无节点域表示全局 OOM，无需读取任务策略或获取 task_lock。 */
	if (!mask)
		return ret;

	/* 阶段 2：锁内借用策略并仅为 BIND 收紧域；裸策略指针不会带出锁外。 */
	task_lock(tsk);
	mempolicy = tsk->mempolicy;
	if (mempolicy && mempolicy->mode == MPOL_BIND)
		ret = nodes_intersects(mempolicy->nodes, *mask);
	task_unlock(tsk);

	return ret;
}

/*
 * alloc_pages_preferred_many() - 两阶段实现 PREFERRED_MANY 页分配
 * 调用位置：alloc_pages_mpol() 识别 PREFERRED_MANY 后的专用回退实现。
 * @gfp/@order: 原始约束和阶数；@nid: 首选起点；@nodemask: 首轮节点集合借用。
 * 首轮只在首选集合尝试、禁止 direct reclaim/NOFAIL 且静默失败；失败后用原始 GFP 和
 * 全系统 fallback 重试。返回 page 持有指针或 NULL，可按 GFP 睡眠；不修改策略。
 */
static struct page *alloc_pages_preferred_many(gfp_t gfp, unsigned int order,
						int nid, nodemask_t *nodemask)
{
	struct page *page;
	gfp_t preferred_gfp;

	/*
	 * This is a two pass approach. The first pass will only try the
	 * preferred nodes but skip the direct reclaim and allow the
	 * allocation to fail, while the second pass will try all the
	 * nodes in system.
	 */
	/* 快速首选轮避免为了偏好节点触发昂贵回收；第二轮恢复调用者的可靠性承诺。 */
	preferred_gfp = gfp | __GFP_NOWARN;
	preferred_gfp &= ~(__GFP_DIRECT_RECLAIM | __GFP_NOFAIL);
	page = __alloc_frozen_pages_noprof(preferred_gfp, order, nid, nodemask);
	if (!page)
		page = __alloc_frozen_pages_noprof(gfp, order, nid, NULL);

	return page;
}

/**
 * alloc_pages_mpol - Allocate pages according to NUMA mempolicy.
 * @gfp: GFP flags.
 * @order: Order of the page allocation.
 * @pol: Pointer to the NUMA mempolicy.
 * @ilx: Index for interleave mempolicy (also distinguishes alloc_pages()).
 * @nid: Preferred node (usually numa_node_id() but @mpol may override it).
 *
 * Return: The page on success or NULL if allocation fails.
 */
/*
 * alloc_pages_mpol() - 根据有效 NUMA 策略执行单次连续页分配
 * @gfp/@order: 分配约束与 2^order 页；@pol: 借用策略；@ilx: 静态交错索引或动态哨兵；
 * @nid: 默认首选节点。可按 GFP 睡眠，返回冻结引用状态的 page 或 NULL。
 * policy_nodemask 计算实际 nid/过滤器；PREFERRED_MANY 走两轮。THP 对本地性采用先本地
 * 紧缩、必要时远端回退的特殊策略；交错命中按实际页 nid 记账。调用者负责解冻/设引用。
 */
static struct page *alloc_pages_mpol(gfp_t gfp, unsigned int order,
		struct mempolicy *pol, pgoff_t ilx, int nid)
{
	nodemask_t *nodemask;
	struct page *page;

	nodemask = policy_nodemask(gfp, pol, ilx, &nid);

	if (pol->mode == MPOL_PREFERRED_MANY)
		return alloc_pages_preferred_many(gfp, order, nid, nodemask);

	if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) &&
	    /* filter "hugepage" allocation, unless from alloc_pages() */
	    /* 只对 VMA 大页分配应用本地性优化，通用 alloc_pages 的动态哨兵保持原语义。 */
	    is_pmd_order(order) && ilx != NO_INTERLEAVE_INDEX) {
		/*
		 * For hugepage allocation and non-interleave policy which
		 * allows the current node (or other explicitly preferred
		 * node) we only try to allocate from the current/preferred
		 * node and don't fall back to other nodes, as the cost of
		 * remote accesses would likely offset THP benefits.
		 *
		 * If the policy is interleave or does not allow the current
		 * node in its nodemask, we allocate the standard way.
		 */
		/*
		 * 非交错策略且当前/首选 nid 被允许时，远端 THP 的访问代价可能抵消大页收益，
		 * 因此先严格本地尝试；交错或本地不在掩码时直接遵循标准策略选择。
		 */
		if (pol->mode != MPOL_INTERLEAVE &&
		    pol->mode != MPOL_WEIGHTED_INTERLEAVE &&
		    (!nodemask || node_isset(nid, *nodemask))) {
			/*
			 * First, try to allocate THP only on local node, but
			 * don't reclaim unnecessarily, just compact.
			 */
			/* 第一轮用 THISNODE+NORETRY，仅本地紧缩，不为大页偏好触发过度回收。 */
			page = __alloc_frozen_pages_noprof(
				gfp | __GFP_THISNODE | __GFP_NORETRY, order,
				nid, NULL);
			if (page || !(gfp & __GFP_DIRECT_RECLAIM))
				return page;
			/*
			 * If hugepage allocations are configured to always
			 * synchronous compact or the vma has been madvised
			 * to prefer hugepage backing, retry allowing remote
			 * memory with both reclaim and compact as well.
			 */
			/* 若调用者允许 direct reclaim，则落入通用分配，按配置/madvise 可远端回退。 */
		}
	}

	page = __alloc_frozen_pages_noprof(gfp, order, nid, nodemask);

	if (unlikely(pol->mode == MPOL_INTERLEAVE ||
		     pol->mode == MPOL_WEIGHTED_INTERLEAVE) && page) {
		/* skip NUMA_INTERLEAVE_HIT update if numa stats is disabled */
		/* 仅实际落在所选 nid 算命中；统计静态键关闭时完全跳过热路径开销。 */
		if (static_branch_likely(&vm_numa_stat_key) &&
		    page_to_nid(page) == nid) {
			preempt_disable();
			__count_numa_event(page_zone(page), NUMA_INTERLEAVE_HIT);
			preempt_enable();
		}
	}

	return page;
}

/*
 * folio_alloc_mpol_noprof() - 按显式策略分配可映射复合 folio
 * 调用位置：VMA 分配与迁移回调在取得有效策略/ilx 后调用。
 * @gfp/@order/@pol/@ilx/@nid: 与 alloc_pages_mpol 相同；强制 __GFP_COMP。
 * 返回已建立正常引用计数、可映射 folio 的持有指针或 NULL。先由底层返回冻结 page，
 * 本函数负责 set_page_refcounted 与 page_rmappable_folio 状态转换。
 * 上下文：睡眠性由 @gfp 决定；不取得额外策略引用，不改变 @pol 所有权。
 */
struct folio *folio_alloc_mpol_noprof(gfp_t gfp, unsigned int order,
		struct mempolicy *pol, pgoff_t ilx, int nid)
{
	struct page *page = alloc_pages_mpol(gfp | __GFP_COMP, order, pol,
			ilx, nid);
	if (!page)
		return NULL;

	set_page_refcounted(page);
	return page_rmappable_folio(page);
}

/**
 * vma_alloc_folio - Allocate a folio for a VMA.
 * @gfp: GFP flags.
 * @order: Order of the folio.
 * @vma: Pointer to VMA.
 * @addr: Virtual address of the allocation.  Must be inside @vma.
 *
 * Allocate a folio for a specific address in @vma, using the appropriate
 * NUMA policy.  The caller must hold the mmap_lock of the mm_struct of the
 * VMA to prevent it from going away.  Should be used for all allocations
 * for folios that will be mapped into user space, excepting hugetlbfs, and
 * excepting where direct use of folio_alloc_mpol() is more appropriate.
 *
 * Return: The folio on success or NULL if allocation fails.
 */
/*
 * vma_alloc_folio_noprof() - 为 VMA 中具体地址按有效策略分配用户可映射 folio
 * 调用位置：匿名/文件 VMA fault 分配入口；返回后 fault 路径继续安装页表映射。
 * @gfp/@order: 分配约束与阶数；@vma: mmap_lock 保证存活的借用 VMA；@addr: VMA 内地址。
 * VM_DROPPABLE 抑制分配警告；函数取得有效策略与 ilx，分配后无论成败 cond_put 共享
 * 策略引用。返回 folio 持有指针或 NULL；除 hugetlb/显式 folio_alloc_mpol 场景外应使用它。
 * 上下文：调用者持对应 mm 的 mmap_lock；可按 @gfp 睡眠，锁外不得继续使用 @vma。
 */
struct folio *vma_alloc_folio_noprof(gfp_t gfp, int order, struct vm_area_struct *vma,
		unsigned long addr)
{
	struct mempolicy *pol;
	pgoff_t ilx;
	struct folio *folio;

	if (vma->vm_flags & VM_DROPPABLE)
		gfp |= __GFP_NOWARN;

	pol = get_vma_policy(vma, addr, order, &ilx);
	folio = folio_alloc_mpol_noprof(gfp, order, pol, ilx, numa_node_id());
	mpol_cond_put(pol);
	return folio;
}
EXPORT_SYMBOL(vma_alloc_folio_noprof);

/*
 * alloc_frozen_pages_noprof() - 按 current 策略分配仍处于 frozen-ref 状态的 page
 * @gfp/@order: 任意上下文适配的 GFP 与阶数。中断或 THISNODE 请求绕开任务策略，使用
 * 静态 default；进程上下文借用 current 策略，无需 refcount。返回 frozen page 或 NULL，
 * 调用者必须建立正常引用计数；策略可决定 nid/掩码并推进动态交错状态。
 */
struct page *alloc_frozen_pages_noprof(gfp_t gfp, unsigned order)
{
	struct mempolicy *pol = &default_policy;

	/*
	 * No reference counting needed for current->mempolicy
	 * nor system default_policy
	 */
	/* current 只能由自身替换策略，当前调用栈内借用安全；静态 default 永不释放。 */
	if (!in_interrupt() && !(gfp & __GFP_THISNODE))
		pol = get_task_policy(current);

	return alloc_pages_mpol(gfp, order, pol, NO_INTERLEAVE_INDEX,
				       numa_node_id());
}

/**
 * alloc_pages - Allocate pages.
 * @gfp: GFP flags.
 * @order: Power of two of number of pages to allocate.
 *
 * Allocate 1 << @order contiguous pages.  The physical address of the
 * first page is naturally aligned (eg an order-3 allocation will be aligned
 * to a multiple of 8 * PAGE_SIZE bytes).  The NUMA policy of the current
 * process is honoured when in process context.
 *
 * Context: Can be called from any context, providing the appropriate GFP
 * flags are used.
 * Return: The page on success or NULL if allocation fails.
 */
/*
 * alloc_pages_noprof() - 通用 NUMA 策略感知页分配实现（不含 profiling wrapper）
 * @gfp/@order: 见上游 kernel-doc。任意上下文可用，睡眠性由 GFP 决定。返回正常引用计数
 * 的 page 或 NULL；内部先 frozen 分配，成功后由本函数发布为 refcounted 状态。
 */
struct page *alloc_pages_noprof(gfp_t gfp, unsigned int order)
{
	struct page *page = alloc_frozen_pages_noprof(gfp, order);

	if (page)
		set_page_refcounted(page);
	return page;
}
EXPORT_SYMBOL(alloc_pages_noprof);

/*
 * folio_alloc_noprof() - 将通用复合页分配包装为可映射 folio
 * 调用位置：通用 folio_alloc profiling wrapper 的底层实现。
 * @gfp/@order: 分配参数；强制 __GFP_COMP。返回 folio 持有指针或 NULL，所有睡眠和 NUMA
 * 选择语义继承 alloc_pages_noprof；本函数无额外策略引用。
 */
struct folio *folio_alloc_noprof(gfp_t gfp, unsigned int order)
{
	return page_rmappable_folio(alloc_pages_noprof(gfp | __GFP_COMP, order));
}
EXPORT_SYMBOL(folio_alloc_noprof);

/*
 * alloc_pages_bulk_interleave() - 把批量页数尽量平均分配到普通交错节点
 * @gfp/@pol/@nr_pages/@page_array: 分配约束、借用策略、请求数和输出指针数组。
 * 先均分并把余数逐节点加一，每批调用 interleave_nodes 推进线程状态。返回实际分配数；
 * 可部分成功，page_array 前返回数量归调用者。策略节点集非空是调用前置条件。
 * 上下文：进程分配上下文，可按 @gfp 睡眠；current 游标仅由当前任务修改。
 */
static unsigned long alloc_pages_bulk_interleave(gfp_t gfp,
		struct mempolicy *pol, unsigned long nr_pages,
		struct page **page_array)
{
	/* nr_pages_per_node 是整轮份额，delta 是需额外分配一页的前若干节点数。 */
	int nodes;
	unsigned long nr_pages_per_node;
	int delta;
	int i;
	unsigned long nr_allocated;
	unsigned long total_allocated = 0;

	/* 阶段 1：把请求拆为每节点基础份额与余数，保证各节点份额差最多一页。 */
	nodes = nodes_weight(pol->nodes);
	nr_pages_per_node = nr_pages / nodes;
	delta = nr_pages - nodes * nr_pages_per_node;

	for (i = 0; i < nodes; i++) {
		/* 阶段 2：每轮推进到下一个交错节点，再发起一次该节点的 bulk 请求。 */
		if (delta) {
			nr_allocated = alloc_pages_bulk_noprof(gfp,
					interleave_nodes(pol), NULL,
					nr_pages_per_node + 1,
					page_array);
			delta--;
		} else {
			nr_allocated = alloc_pages_bulk_noprof(gfp,
					interleave_nodes(pol), NULL,
					nr_pages_per_node, page_array);
		}

		/* 只按实际成功数推进输出指针；部分失败不会产生被计入的数组空洞。 */
		page_array += nr_allocated;
		total_allocated += nr_allocated;
	}

	return total_allocated;
}

/*
 * alloc_pages_bulk_weighted_interleave() - 按权重批量分配并维护可续接的线程游标
 * @gfp/@pol/@nr_pages/@page_array: 同批量接口。返回实际分配页数，可部分成功。
 * 先消费 current 尚余权重，再复制 RCU 权重表到可睡眠的本地缓冲，按完整轮次+余数合并
 * 每节点 bulk 调用，最后保存 resume_node/resume_weight。cpuset rebind 用 seq 重试；内存
 * 分配失败时已成功页仍归调用者，游标清理避免错误续用旧配额。
 */
static unsigned long alloc_pages_bulk_weighted_interleave(gfp_t gfp,
		struct mempolicy *pol, unsigned long nr_pages,
		struct page **page_array)
{
	/* weights 是 RCU 外可用副本；rem_pages/rounds/delta 描述剩余请求的完整与部分权重轮。 */
	struct weighted_interleave_state *state;
	struct task_struct *me = current;
	unsigned int cpuset_mems_cookie;
	unsigned long total_allocated = 0;
	unsigned long nr_allocated = 0;
	unsigned long rounds;
	unsigned long node_pages, delta;
	u8 *weights, weight;
	unsigned int weight_total = 0;
	unsigned long rem_pages = nr_pages;
	nodemask_t nodes;
	int nnodes, node;
	int resume_node = MAX_NUMNODES - 1;
	u8 resume_weight = 0;
	int prev_node;
	int i;

	/* 空请求不读取策略和权重，也不改变调用线程的续分配游标。 */
	if (!nr_pages)
		return 0;

	/* read the nodes onto the stack, retry if done during rebind */
	/* 同一 seq 快照复制 nodemask；若 cpuset 重绑定跨越复制窗口则重新读取。 */
	do {
		cpuset_mems_cookie = read_mems_allowed_begin();
		nnodes = read_once_policy_nodemask(pol, &nodes);
	} while (read_mems_allowed_retry(cpuset_mems_cookie));

	/* if the nodemask has become invalid, we cannot do anything */
	/* 空集合没有除数和目标，按 bulk API 返回部分成功数 0 而非 errno。 */
	if (!nnodes)
		return 0;

	/* Continue allocating from most recent node and adjust the nr_pages */
	/* 阶段 2：优先消费上次调用未用完的节点权重，保证跨 bulk 调用仍维持长期比例。 */
	node = me->il_prev;
	weight = me->il_weight;
	if (weight && node_isset(node, nodes)) {
		node_pages = min(rem_pages, weight);
		nr_allocated = __alloc_pages_bulk(gfp, node, NULL, node_pages,
						  page_array);
		page_array += nr_allocated;
		total_allocated += nr_allocated;
		/* if that's all the pages, no need to interleave */
		/* 请求完全落在当前剩余权重内时无需推进到下一节点，只扣除本批逻辑份额。 */
		if (rem_pages <= weight) {
			me->il_weight -= rem_pages;
			return total_allocated;
		}
		/* Otherwise we adjust remaining pages, continue from there */
		/* 否则当前配额视为耗尽，从 rem_pages 中扣除并在后续节点继续。 */
		rem_pages -= weight;
	}
	/* clear active weight in case of an allocation failure */
	/* 即使本节点只部分成功也不能保留旧配额，否则下次会把失败请求误当已消费。 */
	me->il_weight = 0;
	prev_node = node;

	/* create a local copy of node weights to operate on outside rcu */
	/* bulk 分配可睡眠，绝不能跨分配持 RCU 读锁；先复制 u8 表再离开 RCU。 */
	weights = kzalloc(nr_node_ids, GFP_KERNEL);
	if (!weights)
		return total_allocated;

	rcu_read_lock();
	state = rcu_dereference(wi_state);
	if (state) {
		memcpy(weights, state->iw_table, nr_node_ids * sizeof(u8));
		rcu_read_unlock();
	} else {
		rcu_read_unlock();
		for (i = 0; i < nr_node_ids; i++)
			weights[i] = 1;
	}

	/* calculate total, detect system default usage */
	/* 只累计本策略节点；NULL wi_state 已在副本中展开为全 1。 */
	for_each_node_mask(node, nodes)
		weight_total += weights[node];

	/*
	 * Calculate rounds/partial rounds to minimize __alloc_pages_bulk calls.
	 * Track which node weighted interleave should resume from.
	 *
	 * if (rounds > 0) and (delta == 0), resume_node will always be
	 * the node following prev_node and its weight.
	 */
	/*
	 * 把请求拆为若干完整权重轮与一个 delta，可把逐页选择压缩成每节点一次 bulk 调用。
	 * resume_* 指向逻辑序列下一位置，保证下一批接续；delta 恰为 0 时从 prev 后继开始。
	 */
	rounds = rem_pages / weight_total;
	delta = rem_pages % weight_total;
	resume_node = next_node_in(prev_node, nodes);
	resume_weight = weights[resume_node];
	for (i = 0; i < nnodes; i++) {
		node = next_node_in(prev_node, nodes);
		weight = weights[node];
		node_pages = weight * rounds;
		/* If a delta exists, add this node's portion of the delta */
		/* delta 跨完整节点权重时全给；落在节点内部时记录该节点剩余权重作为续点。 */
		if (delta > weight) {
			node_pages += weight;
			delta -= weight;
		} else if (delta) {
			/* when delta is depleted, resume from that node */
			/* delta 在本节点内耗尽，下一批仍从此节点未消费的权重继续。 */
			node_pages += delta;
			resume_node = node;
			resume_weight = weight - delta;
			delta = 0;
		}
		/* node_pages can be 0 if an allocation fails and rounds == 0 */
		/* 若前序分配失败且不足一整轮，本节点份额可为 0，此时结束而不发空 bulk 请求。 */
		if (!node_pages)
			break;
		nr_allocated = __alloc_pages_bulk(gfp, node, NULL, node_pages,
						  page_array);
		page_array += nr_allocated;
		total_allocated += nr_allocated;
		if (total_allocated == nr_pages)
			break;
		prev_node = node;
	}
	me->il_prev = resume_node;
	me->il_weight = resume_weight;
	kfree(weights);
	return total_allocated;
}

/*
 * alloc_pages_bulk_preferred_many() - PREFERRED_MANY 的批量两阶段分配
 * @gfp/@nid/@pol/@nr_pages/@page_array: 原始约束、本地起点、借用策略、请求数和输出数组。
 * 首轮仅策略集合且无 direct reclaim/NOFAIL；不足部分用原 GFP 从本地全局回退补齐。
 * 返回实际总数，可部分成功；数组前返回数个 page 所有权交给调用者。
 * 上下文：进程分配上下文，可按 @gfp 睡眠；策略只读借用且不持额外引用。
 */
static unsigned long alloc_pages_bulk_preferred_many(gfp_t gfp, int nid,
		struct mempolicy *pol, unsigned long nr_pages,
		struct page **page_array)
{
	gfp_t preferred_gfp;
	unsigned long nr_allocated = 0;

	/* 阶段 1：首选集合内安静尝试，禁止可靠性标志迫使回收或无限等待。 */
	preferred_gfp = gfp | __GFP_NOWARN;
	preferred_gfp &= ~(__GFP_DIRECT_RECLAIM | __GFP_NOFAIL);

	nr_allocated  = alloc_pages_bulk_noprof(preferred_gfp, nid, &pol->nodes,
					   nr_pages, page_array);

	/* 阶段 2：用原始 GFP 和全局节点补足剩余数组尾部，保留已成功首选页。 */
	if (nr_allocated < nr_pages)
		nr_allocated += alloc_pages_bulk_noprof(gfp, numa_node_id(), NULL,
				nr_pages - nr_allocated,
				page_array + nr_allocated);
	return nr_allocated;
}

/* alloc pages bulk and mempolicy should be considered at the
 * same time in some situation such as vmalloc.
 *
 * It can accelerate memory allocation especially interleaving
 * allocate memory.
 */
/*
 * alloc_pages_bulk_mempolicy_noprof() - 按当前任务策略批量分配页
 * @gfp: 分配约束；@nr_pages: 请求数；@page_array: 至少容纳该数量的输出数组。
 * 非任务/中断/THISNODE 使用 default；交错与 PREFERRED_MANY 走专用批处理，其余通过
 * policy_nodemask 后调用通用 bulk。返回实际分配数，可部分成功；前返回数个 page 归调用者。
 * 将策略与批量同时处理可显著减少 vmalloc 等交错场景的逐页分配调用。
 * 调用位置：vmalloc 等批量分配者；上下文匹配 @gfp，非任务上下文不读 current 策略。
 */
unsigned long alloc_pages_bulk_mempolicy_noprof(gfp_t gfp,
		unsigned long nr_pages, struct page **page_array)
{
	struct mempolicy *pol = &default_policy;
	nodemask_t *nodemask;
	int nid;

	/* 阶段 1：仅能安全借用 current 策略的进程上下文才覆盖静态 default。 */
	if (!in_interrupt() && !(gfp & __GFP_THISNODE))
		pol = get_task_policy(current);

	/* 阶段 2：需保持批次级状态的交错与 PREFERRED_MANY 先进入专用实现。 */
	if (pol->mode == MPOL_INTERLEAVE)
		return alloc_pages_bulk_interleave(gfp, pol,
							 nr_pages, page_array);

	if (pol->mode == MPOL_WEIGHTED_INTERLEAVE)
		return alloc_pages_bulk_weighted_interleave(
				  gfp, pol, nr_pages, page_array);

	if (pol->mode == MPOL_PREFERRED_MANY)
		return alloc_pages_bulk_preferred_many(gfp,
				numa_node_id(), pol, nr_pages, page_array);

	/* 阶段 3：其余模式统一求首选 nid/过滤掩码并调用通用 bulk 分配器。 */
	nid = numa_node_id();
	nodemask = policy_nodemask(gfp, pol, NO_INTERLEAVE_INDEX, &nid);
	return alloc_pages_bulk_noprof(gfp, nid, nodemask,
				       nr_pages, page_array);
}

/*
 * vma_dup_policy() - 为复制后的 VMA 建立独立策略引用/副本
 * @src: 源 VMA 借用；@dst: 尚未发布的目标 VMA 输入输出。返回 0 或 -ENOMEM；成功把
 * mpol_dup 结果（可为 NULL）安装到 dst->vm_policy，目标 VMA 后续负责 put。失败不修改
 * dst 策略。调用者的 VMA/mm 锁协议保证 src 指针稳定。
 */
int vma_dup_policy(struct vm_area_struct *src, struct vm_area_struct *dst)
{
	struct mempolicy *pol = mpol_dup(src->vm_policy);

	if (IS_ERR(pol))
		return PTR_ERR(pol);
	dst->vm_policy = pol;
	return 0;
}

/*
 * If mpol_dup() sees current->cpuset == cpuset_being_rebound, then it
 * rebinds the mempolicy its copying by calling mpol_rebind_policy()
 * with the mems_allowed returned by cpuset_mems_allowed().  This
 * keeps mempolicies cpuset relative after its cpuset moves.  See
 * further kernel/cpuset.c update_nodemask().
 *
 * current's mempolicy may be rebinded by the other task(the task that changes
 * cpuset's mems), so we needn't do rebind work for current task.
 */
/*
 * 复制策略时若 current 的 cpuset 正在重绑定，副本必须按 cpuset_mems_allowed() 重新映射，
 * 使相对节点语义随 cpuset 移动。current 自己的任务策略会由修改 cpuset 的任务并发重绑，
 * 因而复制 current->mempolicy 时这里只需在 alloc_lock 下取得一致字段快照，不重复改原件。
 */

/* Slow path of a mempolicy duplicate */
/*
 * __mpol_dup() - 为 mempolicy 创建 refcnt=1 的独立副本
 * @old: 借用非 NULL 动态/静态策略；current 策略在 task_lock 下复制，其他对象由调用者
 * 已有锁/引用保证稳定。可睡眠分配。返回新对象持有指针或 ERR_PTR(-ENOMEM)。
 * 若当前 cpuset 正在 rebind，副本在发布前按 current 有效节点重算；复制的 rcu/refcnt
 * 原值不沿用，最终重置引用计数。调用者负责 mpol_put。
 */
struct mempolicy *__mpol_dup(struct mempolicy *old)
{
	struct mempolicy *new = kmem_cache_alloc(policy_cache, GFP_KERNEL);

	if (!new)
		return ERR_PTR(-ENOMEM);

	/* task's mempolicy is protected by alloc_lock */
	/* current->mempolicy 字段及 union 可能被 cpuset 写侧更新，必须在 alloc_lock 内整体复制。 */
	if (old == current->mempolicy) {
		task_lock(current);
		*new = *old;
		task_unlock(current);
	} else
		*new = *old;

	if (current_cpuset_is_being_rebound()) {
		/* 副本尚未发布，可直接重绑定而无需额外读侧 seq 协议。 */
		nodemask_t mems = cpuset_mems_allowed(current);
		mpol_rebind_policy(new, &mems);
	}
	atomic_set(&new->refcnt, 1);
	return new;
}

/* Slow path of a mempolicy comparison */
/*
 * __mpol_equal() - 比较两个非平凡策略是否语义等价
 * 调用位置：mpol_equal() 快速判断未决后进入的完整字段比较慢路径。
 * @a/@b: 可为 NULL 的借用指针，调用者保证生命周期与字段稳定。返回布尔值，不睡眠。
 * 依次比较 mode/flags/home_node、需持久化的用户掩码和模式有效节点；LOCAL 无节点字段，
 * 公共元数据相同即相等。NULL 与任何进入慢路径的对象不等，非法模式触发 BUG。
 */
bool __mpol_equal(struct mempolicy *a, struct mempolicy *b)
{
	/* 阶段 1：公共身份字段逐项短路；任一差异都无需读取模式专有 union。 */
	if (!a || !b)
		return false;
	if (a->mode != b->mode)
		return false;
	if (a->flags != b->flags)
		return false;
	if (a->home_node != b->home_node)
		return false;
	/* 用户原始掩码会影响未来 cpuset 重绑定，因而也是策略语义的一部分。 */
	if (mpol_store_user_nodemask(a))
		if (!nodes_equal(a->w.user_nodemask, b->w.user_nodemask))
			return false;

	/* 阶段 2：公共字段相同后，按模式比较真正参与分配语义的节点表示。 */
	switch (a->mode) {
	case MPOL_BIND:
	case MPOL_INTERLEAVE:
	case MPOL_PREFERRED:
	case MPOL_PREFERRED_MANY:
	case MPOL_WEIGHTED_INTERLEAVE:
		return nodes_equal(a->nodes, b->nodes);
	case MPOL_LOCAL:
		return true;
	default:
		BUG();
		return false;
	}
}

/*
 * Shared memory backing store policy support.
 *
 * Remember policies even when nobody has shared memory mapped.
 * The policies are kept in Red-Black tree linked from the inode.
 * They are protected by the sp->lock rwlock, which should be held
 * for any accesses to the tree.
 */
/*
 * 共享内存 backing store 的策略必须在无人映射时仍存在，因此按文件页索引区间保存在 inode
 * 的红黑树中，而不是某个 VMA。sp->lock 读侧保护查询，写侧保护区间拆分、插入和删除；
 * 每个 sp_node 独占一份带 MPOL_F_SHARED 的策略引用，删除节点时同步 put。
 */

/*
 * lookup first element intersecting start-end.  Caller holds sp->lock for
 * reading or for writing
 */
/*
 * sp_lookup() - 查找与 [start,end) 相交且起点最早的共享策略节点
 * 调用位置：共享策略读查询与写替换路径在持有 sp->lock 后共同调用。
 * @sp: 锁已由调用者以读或写方式持有；@start/@end: 文件页索引半开区间。
 * 返回树内借用 sp_node 或 NULL，不增加策略引用。先按区间关系找到任一交点，再沿 rb_prev
 * 回溯到仍相交的最早节点；树节点在持锁期间不会释放。
 */
static struct sp_node *sp_lookup(struct shared_policy *sp,
					pgoff_t start, pgoff_t end)
{
	struct rb_node *n = sp->root.rb_node;

	while (n) {
		/* 阶段 1：按区间相对位置在红黑树中定位任意一个相交节点。 */
		struct sp_node *p = rb_entry(n, struct sp_node, nd);

		if (start >= p->end)
			n = n->rb_right;
		else if (end <= p->start)
			n = n->rb_left;
		else
			break;
	}
	if (!n)
		return NULL;
	for (;;) {
		/* 回溯前驱确保调用者按顺序处理所有相交区间，而不是从任意命中点遗漏左侧节点。 */
		struct sp_node *w = NULL;
		struct rb_node *prev = rb_prev(n);
		if (!prev)
			break;
		w = rb_entry(prev, struct sp_node, nd);
		if (w->end <= start)
			break;
		n = prev;
	}
	return rb_entry(n, struct sp_node, nd);
}

/*
 * Insert a new shared policy into the list.  Caller holds sp->lock for
 * writing.
 */
/*
 * sp_insert() - 将一个不重叠共享策略区间插入红黑树
 * 调用位置：初始化、区间替换和旧节点拆分在写锁内发布新节点时调用。
 * @sp: 写锁已持有；@new: 调用者把节点及其 policy 引用所有权转移给树。
 * 返回无直接值。按 start/end 定位唯一空槽，重叠/重复区间触发 BUG，随后链接并着色保持
 * 红黑树平衡；发布后只能由 sp_delete/shared_policy_replace 摘除。
 */
static void sp_insert(struct shared_policy *sp, struct sp_node *new)
{
	struct rb_node **p = &sp->root.rb_node;
	struct rb_node *parent = NULL;
	struct sp_node *nd;

	while (*p) {
		/* 阶段 1：按 start/end 找唯一空链接；树不允许等价或重叠区间。 */
		parent = *p;
		nd = rb_entry(parent, struct sp_node, nd);
		if (new->start < nd->start)
			p = &(*p)->rb_left;
		else if (new->end > nd->end)
			p = &(*p)->rb_right;
		else
			BUG();
	}
	/* 阶段 2：先链接 BST 位置，再旋转/着色恢复红黑树平衡，此后读者可查到 new。 */
	rb_link_node(&new->nd, parent, p);
	rb_insert_color(&new->nd, &sp->root);
}

/* Find shared policy intersecting idx */
/*
 * mpol_shared_policy_lookup() - 查询文件页索引处的共享策略并取得引用
 * 调用位置：shmem/tmpfs vm_ops->get_policy() 为 VMA 地址解析共享 inode 策略。
 * @sp: inode 共享策略容器；@idx: 单个文件页索引。空树无锁快速返回 NULL；否则在读锁
 * 下查 [idx,idx+1)，命中后 mpol_get 再解锁。返回持有引用或 NULL，调用者必须 mpol_put。
 */
struct mempolicy *mpol_shared_policy_lookup(struct shared_policy *sp,
						pgoff_t idx)
{
	struct mempolicy *pol = NULL;
	struct sp_node *sn;

	if (!sp->root.rb_node)
		return NULL;
	/* 阶段 1：读锁稳定树节点；命中后在解锁前把 policy 借用升级为持有引用。 */
	read_lock(&sp->lock);
	sn = sp_lookup(sp, idx, idx+1);
	if (sn) {
		mpol_get(sn->policy);
		pol = sn->policy;
	}
	read_unlock(&sp->lock);
	return pol;
}
EXPORT_SYMBOL_FOR_MODULES(mpol_shared_policy_lookup, "kvm");

/*
 * sp_free() - 销毁已从共享策略树摘除的节点
 * @n: 调用者独占的节点；先交还其 policy 引用，再归还 sn_cache。返回无直接值；调用后
 * n 与 n->policy 均不可访问。调用路径持写锁，释放动作本身不要求额外树同步。
 */
static void sp_free(struct sp_node *n)
{
	mpol_put(n->policy);
	kmem_cache_free(sn_cache, n);
}

/**
 * mpol_misplaced - check whether current folio node is valid in policy
 *
 * @folio: folio to be checked
 * @vmf: structure describing the fault
 * @addr: virtual address in @vma for shared policy lookup and interleave policy
 *
 * Lookup current policy node id for vma,addr and "compare to" folio's
 * node id.  Policy determination "mimics" alloc_page_vma().
 * Called from fault path where we know the vma and faulting address.
 *
 * Return: NUMA_NO_NODE if the page is in a node that is valid for this
 * policy, or a suitable node ID to allocate a replacement folio from.
 */
/*
 * mpol_misplaced() - 在 NUMA hinting fault 中判断 folio 是否应迁到策略目标节点
 * @folio: 页表锁稳定的借用 folio；@vmf: fault 上下文，vmf->ptl 必须持有；@addr: VMA
 * 内地址。返回 NUMA_NO_NODE 表示当前位置可接受/本轮不迁移，或目标 nid。
 * 函数按分配路径相同规则计算 interleave/preferred/bind 目标；MOF 未设置直接退出，MORON
 * 则结合 should_numa_migrate_memory 决定是否朝当前访问 CPU 迁移。共享策略引用统一
 * cond_put。持 PTE 自旋锁保证不被抢占和 CPU id 稳定，函数不得睡眠。
 */
int mpol_misplaced(struct folio *folio, struct vm_fault *vmf,
		   unsigned long addr)
{
	/* curnid 是页位置，thiscpu/thisnid 是稳定访问者位置，polnid/ret 以“不迁移”哨兵开始。 */
	struct mempolicy *pol;
	pgoff_t ilx;
	struct zoneref *z;
	int curnid = folio_nid(folio);
	struct vm_area_struct *vma = vmf->vma;
	int thiscpu = raw_smp_processor_id();
	int thisnid = numa_node_id();
	int polnid = NUMA_NO_NODE;
	int ret = NUMA_NO_NODE;

	/*
	 * Make sure ptl is held so that we don't preempt and we
	 * have a stable smp processor id
	 */
	/* PTE 锁既保护映射，也隐含禁止抢占，使 raw CPU id 与 numa_node_id 属于同一 CPU。 */
	lockdep_assert_held(vmf->ptl);
	pol = get_vma_policy(vma, addr, folio_order(folio), &ilx);
	if (!(pol->flags & MPOL_F_MOF))
		/* 没有“迁移时跟随”位时策略只约束新分配，不改变现有 folio。 */
		goto out;

	/* 阶段 2：把当前地址/索引映射为策略期望 nid；无确定目标的分支直接退出。 */
	switch (pol->mode) {
	case MPOL_INTERLEAVE:
		polnid = interleave_nid(pol, ilx);
		break;

	case MPOL_WEIGHTED_INTERLEAVE:
		polnid = weighted_interleave_nid(pol, ilx);
		break;

	case MPOL_PREFERRED:
		if (node_isset(curnid, pol->nodes))
			goto out;
		polnid = first_node(pol->nodes);
		break;

	case MPOL_LOCAL:
		polnid = numa_node_id();
		break;

	case MPOL_BIND:
	case MPOL_PREFERRED_MANY:
		/*
		 * Even though MPOL_PREFERRED_MANY can allocate pages outside
		 * policy nodemask we don't allow numa migration to nodes
		 * outside policy nodemask for now. This is done so that if we
		 * want demotion to slow memory to happen, before allocating
		 * from some DRAM node say 'x', we will end up using a
		 * MPOL_PREFERRED_MANY mask excluding node 'x'. In such scenario
		 * we should not promote to node 'x' from slow memory node.
		 */
		/*
		 * PREFERRED_MANY 虽允许分配 fallback，自动 NUMA 迁移暂不越出其掩码，避免把慢内存
		 * 页提升到用户刻意排除的 DRAM 节点，从而破坏内存分层/降级策略。
		 */
		if (pol->flags & MPOL_F_MORON) {
			/*
			 * Optimize placement among multiple nodes
			 * via NUMA balancing
			 */
			/* MORON 允许 NUMA balancing 在掩码内按访问位置优化；当前 CPU 不在集合则不迁。 */
			if (node_isset(thisnid, pol->nodes))
				break;
			goto out;
		}

		/*
		 * use current page if in policy nodemask,
		 * else select nearest allowed node, if any.
		 * If no allowed nodes, use current [!misplaced].
		 */
		/* 当前页已在允许集合即有效，否则从访问 CPU 的 zonelist 选最近允许节点。 */
		if (node_isset(curnid, pol->nodes))
			goto out;
		z = first_zones_zonelist(
				node_zonelist(thisnid, GFP_HIGHUSER),
				gfp_zone(GFP_HIGHUSER),
				&pol->nodes);
		polnid = zonelist_node_idx(z);
		break;

	default:
		BUG();
	}

	/* Migrate the folio towards the node whose CPU is referencing it */
	/* MORON 把目标改为当前访问 CPU 节点，但仍需调度 NUMA 统计/节流判断批准。 */
	if (pol->flags & MPOL_F_MORON) {
		polnid = thisnid;

		if (!should_numa_migrate_memory(current, folio, curnid,
						thiscpu))
			goto out;
	}

	if (curnid != polnid)
		ret = polnid;
out:
	mpol_cond_put(pol);

	return ret;
}

/*
 * Drop the (possibly final) reference to task->mempolicy.  It needs to be
 * dropped after task->mempolicy is set to NULL so that any allocation done as
 * part of its kmem_cache_free(), such as by KASAN, doesn't reference a freed
 * policy.
 */
/*
 * mpol_put_task_policy() - 从任务摘除并释放其可能最后一份策略引用
 * @task: 调用者保证存活的任务。task_lock 下先把 task->mempolicy 置 NULL，再解锁 put；
 * 顺序不可颠倒，因为最终 kmem_cache_free/KASAN 内部若触发分配，会重新查询任务策略，
 * 必须看到 NULL 而不是已释放指针。返回无直接值，调用后任务回到默认策略。
 */
void mpol_put_task_policy(struct task_struct *task)
{
	struct mempolicy *pol;

	task_lock(task);
	pol = task->mempolicy;
	task->mempolicy = NULL;
	task_unlock(task);
	mpol_put(pol);
}

/*
 * sp_delete() - 在写锁下从共享策略树摘除并立即销毁节点
 * 调用位置：shared_policy_replace() 覆盖旧区间，或 inode 销毁时清空整树。
 * @sp: 写锁已持有；@n: 树内节点。rb_erase 先终止新读者可达性，再 sp_free 释放策略引用
 * 和节点内存；rwlock 保证没有并发读者仍持裸 n 指针。返回无直接值。
 */
static void sp_delete(struct shared_policy *sp, struct sp_node *n)
{
	rb_erase(&n->nd, &sp->root);
	sp_free(n);
}

/*
 * sp_node_init() - 初始化尚未发布的共享策略区间节点
 * 调用位置：sp_alloc() 和 shared_policy_replace() 的右半区间拆分阶段。
 * @node: 输出节点；@start/@end: 文件页索引半开区间；@pol: 已由调用者转移给节点的策略
 * 引用。只写字段，不链接红黑树、不睡眠；后续 sp_insert 发布。
 * 返回：无直接返回值；输出是 @node 三个字段，@pol 的释放责任转移给该节点。
 */
static void sp_node_init(struct sp_node *node, unsigned long start,
			unsigned long end, struct mempolicy *pol)
{
	node->start = start;
	node->end = end;
	node->policy = pol;
}

/*
 * sp_alloc() - 分配共享策略节点并复制其独立策略
 * 调用位置：共享策略初始化与 VMA 区间安装在取得写锁之前预构造节点。
 * @start/@end: 区间；@pol: 可由 mpol_dup 处理的借用策略模板。
 * 可睡眠。返回拥有 policy 引用且尚未入树的节点，或 NULL；成功策略标记 MPOL_F_SHARED，
 * 调用者必须 sp_insert 转移给树，或 sp_free 回滚。
 */
static struct sp_node *sp_alloc(unsigned long start, unsigned long end,
				struct mempolicy *pol)
{
	struct sp_node *n;
	struct mempolicy *newpol;

	/* 阶段 1：先分配容器；失败时没有策略引用需要回滚。 */
	n = kmem_cache_alloc(sn_cache, GFP_KERNEL);
	if (!n)
		return NULL;

	/* 阶段 2：为树复制独立策略，失败只释放尚未发布的节点容器。 */
	newpol = mpol_dup(pol);
	if (IS_ERR(newpol)) {
		kmem_cache_free(sn_cache, n);
		return NULL;
	}
	newpol->flags |= MPOL_F_SHARED;
	sp_node_init(n, start, end, newpol);

	return n;
}

/* Replace a policy range. */
/*
 * shared_policy_replace() - 用可选新节点替换共享策略树的 [start,end)
 * 调用位置：mpol_set_shared_policy() 安装或清除一个 VMA 对应的文件页策略区间。
 * @sp: inode 容器；@start/@end: 文件页区间；@new: 可为 NULL，成功时所有权转移给树。
 * 写锁下删除完全覆盖节点、截短部分重叠节点；若旧节点横跨整个新区间，需要把右半复制
 * 成新节点。因为 GFP_KERNEL 不能在 rwlock 内分配，函数解锁预分配后 restart 并重新查树。
 * 返回 0 或 -ENOMEM；失败时 @new 仍归调用者。n_new/mpol_new 在统一出口逆序回收。
 */
static int shared_policy_replace(struct shared_policy *sp, pgoff_t start,
				 pgoff_t end, struct sp_node *new)
{
	struct sp_node *n;
	struct sp_node *n_new = NULL;
	struct mempolicy *mpol_new = NULL;
	int ret = 0;

restart:
	/* 每次重新加锁都必须重查，解锁分配期间树可能被其他写者修改。 */
	write_lock(&sp->lock);
	n = sp_lookup(sp, start, end);
	/* Take care of old policies in the same range. */
	/* 逐个删除、截短或拆分所有与新区间重叠的旧策略，先消除重叠再插入 new。 */
	while (n && n->start < end) {
		/* 阶段 2：按起点顺序消费所有相交旧节点，并预取 next 避免删除后再解引用 n。 */
		struct rb_node *next = rb_next(&n->nd);
		if (n->start >= start) {
			if (n->end <= end)
				sp_delete(sp, n);
			else
				n->start = end;
		} else {
			/* Old policy spanning whole new range. */
			/* 旧区间横跨新范围时需保留左右两段：原节点缩为左段，新副本承载右段。 */
			if (n->end > end) {
				if (!n_new)
					goto alloc_new;

				*mpol_new = *n->policy;
				atomic_set(&mpol_new->refcnt, 1);
				sp_node_init(n_new, end, n->end, mpol_new);
				n->end = start;
				sp_insert(sp, n_new);
				n_new = NULL;
				mpol_new = NULL;
				break;
			} else
				n->end = start;
		}
		/* 当前重叠节点处理完毕，再沿预先保存的 next 前进，避免删除后访问失效链接。 */
		if (!next)
			break;
		n = rb_entry(next, struct sp_node, nd);
	}
	if (new)
		/* 所有重叠已消除，此处是 new 所有权正式转移给树的发布点。 */
		sp_insert(sp, new);
	write_unlock(&sp->lock);
	ret = 0;

err_out:
	/* 失败/成功统一回收尚未入树的右半预分配；已转移给树的指针已被清 NULL。 */
	if (mpol_new)
		mpol_put(mpol_new);
	if (n_new)
		kmem_cache_free(sn_cache, n_new);

	return ret;

alloc_new:
	/* 不能持自旋型 rwlock 做 GFP_KERNEL；解锁后准备节点和策略，再从头验证。 */
	write_unlock(&sp->lock);
	ret = -ENOMEM;
	n_new = kmem_cache_alloc(sn_cache, GFP_KERNEL);
	if (!n_new)
		goto err_out;
	mpol_new = kmem_cache_alloc(policy_cache, GFP_KERNEL);
	if (!mpol_new)
		goto err_out;
	atomic_set(&mpol_new->refcnt, 1);
	goto restart;
}

/**
 * mpol_shared_policy_init - initialize shared policy for inode
 * @sp: pointer to inode shared policy
 * @mpol:  struct mempolicy to install
 *
 * Install non-NULL @mpol in inode's shared policy rb-tree.
 * On entry, the current task has a reference on a non-NULL @mpol.
 * This must be released on exit.
 * This is called at get_inode() calls and we can use GFP_KERNEL.
 */
/*
 * mpol_shared_policy_init() - 初始化 inode 共享策略树并可安装覆盖整个文件的挂载策略
 * @sp: 输出容器；@mpol: 可为 NULL，但非 NULL 时进入即携带调用者转交的一份引用，本
 * 函数所有出口都释放它。可使用 GFP_KERNEL。
 * 先建立空树/rwlock；再把 tmpfs 挂载策略按 current cpuset 具体化为 npol，sp_alloc 为
 * 整个文件创建共享副本并入树。任一分配/节点交集失败都保留空树；scratch、npol、输入
 * mpol 按标签逆序释放，函数无直接返回值。
 * 上下文：inode 创建的进程上下文，可因 GFP_KERNEL 睡眠；树尚未发布，无并发读写者。
 */
void mpol_shared_policy_init(struct shared_policy *sp, struct mempolicy *mpol)
{
	int ret;

	sp->root = RB_ROOT;		/* empty tree == default mempolicy */
	/* 空红黑树代表继承默认策略，不需要额外哨兵节点。 */
	rwlock_init(&sp->lock);

	if (mpol) {
		struct sp_node *sn;
		struct mempolicy *npol;
		NODEMASK_SCRATCH(scratch);

		if (!scratch)
			goto put_mpol;

		/* contextualize the tmpfs mount point mempolicy to this file */
		/* 将 superblock 用户掩码重新约束到当前文件创建者 cpuset，而非盲拷贝旧有效集合。 */
		npol = mpol_new(mpol->mode, mpol->flags, &mpol->w.user_nodemask);
		if (IS_ERR(npol))
			goto free_scratch; /* no valid nodemask intersection */
			/* 用户掩码与当前有效节点没有合法交集，保留空共享策略树并清理 scratch。 */

		task_lock(current);
		ret = mpol_set_nodemask(npol, &mpol->w.user_nodemask, scratch);
		task_unlock(current);
		if (ret)
			goto put_npol;

		/* alloc node covering entire file; adds ref to file's npol */
		/* sp_alloc 再复制一份带 SHARED 的树引用，局部 npol 初始引用随后可安全释放。 */
		sn = sp_alloc(0, MAX_LFS_FILESIZE >> PAGE_SHIFT, npol);
		if (sn)
			sp_insert(sp, sn);
put_npol:
		mpol_put(npol);	/* drop initial ref on file's npol */
		/* 交还文件具体化策略的初始引用；若已入树，树持有的是 sp_alloc 的独立副本。 */
free_scratch:
		NODEMASK_SCRATCH_FREE(scratch);
put_mpol:
		mpol_put(mpol);	/* drop our incoming ref on sb mpol */
		/* 无论安装是否成功，都消费调用者转交的 superblock 策略引用。 */
	}
}
EXPORT_SYMBOL_FOR_MODULES(mpol_shared_policy_init, "kvm");

/*
 * mpol_set_shared_policy() - 将 VMA 文件偏移区间的策略写入 inode 共享树
 * 调用位置：shmem/tmpfs set_policy vm_op；返回后 VMA 策略安装流程继续。
 * @sp: 共享容器；@vma: 借用 VMA，其 vm_pgoff/vma_pages 定义区间；@pol: 可为 NULL 的
 * 策略模板。可睡眠分配。返回 0 或 -ENOMEM；非 NULL 时先创建待发布节点，replace 成功
 * 转移给树，失败则本函数 sp_free。NULL 表示删除区间内显式共享策略。
 */
int mpol_set_shared_policy(struct shared_policy *sp,
			struct vm_area_struct *vma, struct mempolicy *pol)
{
	int err;
	struct sp_node *new = NULL;
	unsigned long sz = vma_pages(vma);

	if (pol) {
		/* 阶段 1：锁外构造完整待发布节点，避免在 sp->lock 内执行 GFP_KERNEL。 */
		new = sp_alloc(vma->vm_pgoff, vma->vm_pgoff + sz, pol);
		if (!new)
			return -ENOMEM;
	}
	/* 阶段 2：replace 成功接管 new；失败时所有权仍在本函数并由 sp_free 回滚。 */
	err = shared_policy_replace(sp, vma->vm_pgoff, vma->vm_pgoff + sz, new);
	if (err && new)
		sp_free(new);
	return err;
}
EXPORT_SYMBOL_FOR_MODULES(mpol_set_shared_policy, "kvm");

/* Free a backing policy store on inode delete. */
/*
 * mpol_free_shared_policy() - inode 删除时销毁整棵共享策略树
 * 调用位置：共享 backing inode 最终释放路径；调用后不再允许任何策略查询者进入。
 * @sp: 不再会被新外部用户获得的容器。空树快速返回；否则持写锁按 rb_next 安全预取后
 * 逐节点 sp_delete，释放全部策略引用和 sn_cache 对象。返回无直接值，结束后树为空。
 */
void mpol_free_shared_policy(struct shared_policy *sp)
{
	struct sp_node *n;
	struct rb_node *next;

	if (!sp->root.rb_node)
		return;
	/* 阶段 1：写锁排除现有查询/修改者；循环中先取 next，再删除当前节点。 */
	write_lock(&sp->lock);
	next = rb_first(&sp->root);
	while (next) {
		n = rb_entry(next, struct sp_node, nd);
		next = rb_next(&n->nd);
		sp_delete(sp, n);
	}
	write_unlock(&sp->lock);
}
EXPORT_SYMBOL_FOR_MODULES(mpol_free_shared_policy, "kvm");

#ifdef CONFIG_NUMA_BALANCING
static int __initdata numabalancing_override;
/* 启动期覆盖值：0 未指定，1 强制启用，-1 强制禁用；__initdata 在初始化后释放。 */

/*
 * check_numabalancing_enable() - 根据编译默认、节点数和启动参数确定自动 NUMA balancing
 * 入参：无；仅启动期调用，不睡眠。显式 override 优先；无 override 且多于一个在线节点
 * 时应用 CONFIG_NUMA_BALANCING_DEFAULT_ENABLED 并打印提示。返回无直接值，副作用是更新
 * 全局 NUMA balancing 状态；单节点且无覆盖时保持原状态。
 */
static void __init check_numabalancing_enable(void)
{
	bool numabalancing_default = false;

	if (IS_ENABLED(CONFIG_NUMA_BALANCING_DEFAULT_ENABLED))
		numabalancing_default = true;

	/* Parsed by setup_numabalancing. override == 1 enables, -1 disables */
	/* setup_numabalancing 已把文本折叠为三态，显式用户选择不再受节点数/编译默认覆盖。 */
	if (numabalancing_override)
		set_numabalancing_state(numabalancing_override == 1);

	if (num_online_nodes() > 1 && !numabalancing_override) {
		pr_info("%s automatic NUMA balancing. Configure with numa_balancing= or the kernel.numa_balancing sysctl\n",
			numabalancing_default ? "Enabling" : "Disabling");
		set_numabalancing_state(numabalancing_default);
	}
}

/*
 * setup_numabalancing() - 解析 numa_balancing=enable|disable 启动参数
 * 调用位置：__setup 在早期命令行扫描时调用，结果稍后由 numa_policy_init() 提交。
 * @str: 启动命令行值，可为 NULL。返回 1 表示已消费合法参数，0 表示无法解析并打印警告。
 * 只写 __initdata override，不立即改变运行状态；check_numabalancing_enable 稍后统一提交。
 * 上下文：单线程启动期，不需要锁、不睡眠；str 只读借用且不会越过初始化阶段保存。
 */
static int __init setup_numabalancing(char *str)
{
	int ret = 0;
	/* 阶段 1：只接受两个稳定关键字；NULL 或未知值都汇入统一警告出口。 */
	if (!str)
		goto out;

	if (!strcmp(str, "enable")) {
		numabalancing_override = 1;
		ret = 1;
	} else if (!strcmp(str, "disable")) {
		numabalancing_override = -1;
		ret = 1;
	}
out:
	/* ret 同时遵循 __setup 的“已消费参数”约定；未知值保留 0 并发出诊断。 */
	if (!ret)
		pr_warn("Unable to parse numa_balancing=\n");

	return ret;
}
__setup("numa_balancing=", setup_numabalancing);
#else
/*
 * check_numabalancing_enable() stub - 未编译 NUMA_BALANCING 时的空初始化钩子
 * 入参：无；返回无直接值，无副作用，确保 numa_policy_init 无需条件编译调用点。
 */
static inline void __init check_numabalancing_enable(void)
{
}
#endif /* CONFIG_NUMA_BALANCING */
/* 上述配置分支只影响自动 NUMA 扫描/迁移开关，不移除基本 NUMA 分配策略。 */

/*
 * numa_policy_init() - 启动期建立策略缓存、静态每节点策略并设置 init 线程交错策略
 * 入参：无；__init 仅调用一次。SLAB_PANIC 缓存创建失败会终止启动，不返回 errno。
 * 初始化 policy_cache/sn_cache 和永久 preferred_node_policy；随后在所有有内存且至少
 * 16MiB 的节点间为当前 init 任务设 INTERLEAVE，若都太小则只用最大节点，最后应用自动
 * balancing 默认。返回无直接值；do_set_mempolicy 失败只记录错误，缓存仍有效。
 */
void __init numa_policy_init(void)
{
	/* interleave_nodes 是 init 策略集合；largest/prefer 记录“小节点全部不达阈值”兜底。 */
	nodemask_t interleave_nodes;
	unsigned long largest = 0;
	int nid, prefer = 0;

	policy_cache = kmem_cache_create("numa_policy",
					 sizeof(struct mempolicy),
					 0, SLAB_PANIC, NULL);

	sn_cache = kmem_cache_create("shared_policy_node",
				     sizeof(struct sp_node),
				     0, SLAB_PANIC, NULL);

	for_each_node(nid) {
		/* 静态策略每个固定指向自身 nid，refcnt=1 永久存在，并允许 fault 跟随访问位置。 */
		preferred_node_policy[nid] = (struct mempolicy) {
			.refcnt = ATOMIC_INIT(1),
			.mode = MPOL_PREFERRED,
			.flags = MPOL_F_MOF | MPOL_F_MORON,
			.nodes = nodemask_of_node(nid),
		};
	}

	/*
	 * Set interleaving policy for system init. Interleaving is only
	 * enabled across suitably sized nodes (default is >= 16MB), or
	 * fall back to the largest node if they're all smaller.
	 */
	/* init 的早期分配跨足够大的内存节点交错；小于 16MiB 的节点避免被启动流量耗尽。 */
	nodes_clear(interleave_nodes);
	for_each_node_state(nid, N_MEMORY) {
		unsigned long total_pages = node_present_pages(nid);

		/* Preserve the largest node */
		/* 始终保存最大 present-pages 节点，保证阈值筛选为空时仍有合法策略。 */
		if (largest < total_pages) {
			largest = total_pages;
			prefer = nid;
		}

		/* Interleave this node? */
		/* 以字节比较 16MiB；只遍历 N_MEMORY，排除无可分配内存节点。 */
		if ((total_pages << PAGE_SHIFT) >= (16 << 20))
			node_set(nid, interleave_nodes);
	}

	/* All too small, use the largest */
	/* 所有节点都太小时退为单节点交错，避免向 mpol_new 提交空掩码。 */
	if (unlikely(nodes_empty(interleave_nodes)))
		node_set(prefer, interleave_nodes);

	if (do_set_mempolicy(MPOL_INTERLEAVE, 0, &interleave_nodes))
		pr_err("%s: interleaving failed\n", __func__);

	check_numabalancing_enable();
}

/* Reset policy of current process to default */
/*
 * numa_default_policy() - 清除 current 显式策略并恢复默认本地分配
 * 调用位置：内核调用者要求撤销当前线程策略时的便捷 wrapper。
 * 入参：无；进程上下文，可因 do_set_mempolicy 的 scratch/锁协议睡眠。返回无直接值，
 * 当前接口忽略内部错误；成功发布 NULL 并释放旧任务策略，不改变 VMA 显式策略或现有页。
 */
void numa_default_policy(void)
{
	do_set_mempolicy(MPOL_DEFAULT, 0, NULL);
}

/*
 * Parse and format mempolicy from/to strings
 */
/* 字符串接口服务 tmpfs mount 选项与 procfs 展示；mode 名表是模式枚举到稳定文本的映射。 */
static const char * const policy_modes[] =
{
	[MPOL_DEFAULT]    = "default",
	[MPOL_PREFERRED]  = "prefer",
	[MPOL_BIND]       = "bind",
	[MPOL_INTERLEAVE] = "interleave",
	[MPOL_WEIGHTED_INTERLEAVE] = "weighted interleave",
	[MPOL_LOCAL]      = "local",
	[MPOL_PREFERRED_MANY]  = "prefer (many)",
};

#ifdef CONFIG_TMPFS
/**
 * mpol_parse_str - parse string to mempolicy, for tmpfs mpol mount option.
 * @str:  string containing mempolicy to parse
 * @mpol:  pointer to struct mempolicy pointer, returned on success.
 *
 * Format of input:
 *	<mode>[=<flags>][:<nodelist>]
 *
 * Return: %0 on success, else %1
 */
/*
 * mpol_parse_str() - 解析 tmpfs mpol 挂载字符串并构造尚未具体化的策略
 * 调用位置：tmpfs 挂载选项解析；成功输出稍后交给 mpol_shared_policy_init()。
 * @str: 可写字符串，格式 mode[=static|relative][:nodelist]，解析时临时插入 NUL，出口恢复；
 * @mpol: 成功输出 refcnt=1 策略，失败保持不变。
 * 返回 0 成功、1 失败（该接口不用负 errno）。验证节点属于 N_MEMORY；各模式强制节点
 * 列表有无规则。结果保存用户 nodes 供 proc 展示和未来按具体 cpuset contextualize，调用者
 * 必须最终 mpol_put。DEFAULT 成功返回时不创建对象，由上层既有语义处理。
 * 上下文：挂载进程上下文，可因策略 slab 分配睡眠；不持 cpuset 或共享策略树锁。
 */
int mpol_parse_str(char *str, struct mempolicy **mpol)
{
	/* nodelist/flags 指向 str 内分隔符后的子串；new 在成功前归本函数，err 默认失败。 */
	struct mempolicy *new = NULL;
	unsigned short mode_flags;
	nodemask_t nodes;
	char *nodelist = strchr(str, ':');
	char *flags = strchr(str, '=');
	int err = 1, mode;

	if (flags)
		*flags++ = '\0';	/* terminate mode string */
		/* 临时终止 mode 子串，出口把 '=' 恢复，便于上层打印原始错误参数。 */

	if (nodelist) {
		/* NUL-terminate mode or flags string */
		/* ':' 前可能是 mode 或 flag；解析后拒绝任何不具备内存的节点。 */
		*nodelist++ = '\0';
		if (nodelist_parse(nodelist, nodes))
			goto out;
		if (!nodes_subset(nodes, node_states[N_MEMORY]))
			goto out;
	} else
		nodes_clear(nodes);

	mode = match_string(policy_modes, MPOL_MAX, str);
	if (mode < 0)
		goto out;

	/* 阶段 2：按模式核对节点列表基数及允许标志，形成可构造策略的规范输入。 */
	switch (mode) {
	case MPOL_PREFERRED:
		/*
		 * Insist on a nodelist of one node only, although later
		 * we use first_node(nodes) to grab a single node, so here
		 * nodelist (or nodes) cannot be empty.
		 */
		/* 若显式给出列表，只接受单个十进制 nid；空列表也非法，避免 first_node 空集。 */
		if (nodelist) {
			char *rest = nodelist;
			while (isdigit(*rest))
				rest++;
			if (*rest)
				goto out;
			if (nodes_empty(nodes))
				goto out;
		}
		break;
	case MPOL_INTERLEAVE:
	case MPOL_WEIGHTED_INTERLEAVE:
		/*
		 * Default to online nodes with memory if no nodelist
		 */
		/* 交错模式未给列表时默认覆盖所有当前 N_MEMORY 节点。 */
		if (!nodelist)
			nodes = node_states[N_MEMORY];
		break;
	case MPOL_LOCAL:
		/*
		 * Don't allow a nodelist;  mpol_new() checks flags
		 */
		/* LOCAL 的节点由分配时当前 CPU 决定，任何列表都与语义冲突。 */
		if (nodelist)
			goto out;
		break;
	case MPOL_DEFAULT:
		/*
		 * Insist on a empty nodelist
		 */
		/* DEFAULT 只允许完全省略 nodelist；它表示没有要安装的显式策略。 */
		if (!nodelist)
			err = 0;
		goto out;
	case MPOL_PREFERRED_MANY:
	case MPOL_BIND:
		/*
		 * Insist on a nodelist
		 */
		/* BIND/PREFERRED_MANY 没有集合就无法表达约束，必须显式提供。 */
		if (!nodelist)
			goto out;
	}

	mode_flags = 0;
	if (flags) {
		/*
		 * Currently, we only support two mutually exclusive
		 * mode flags.
		 */
		/* 当前字符串 ABI 只支持互斥的 static/relative 单值，不接受组合或未知标志。 */
		if (!strcmp(flags, "static"))
			mode_flags |= MPOL_F_STATIC_NODES;
		else if (!strcmp(flags, "relative"))
			mode_flags |= MPOL_F_RELATIVE_NODES;
		else
			goto out;
	}

	new = mpol_new(mode, mode_flags, &nodes);
	if (IS_ERR(new))
		goto out;

	/*
	 * Save nodes for mpol_to_str() to show the tmpfs mount options
	 * for /proc/mounts, /proc/pid/mounts and /proc/pid/mountinfo.
	 */
	/*
	 * 此时不调用 mpol_set_nodemask：先保存挂载选项原始 nodes 以供 procfs 重现；inode
	 * 创建时 mpol_shared_policy_init 才在具体 cpuset 上下文化。PREFERRED 无列表转 LOCAL。
	 */
	if (mode != MPOL_PREFERRED) {
		new->nodes = nodes;
	} else if (nodelist) {
		nodes_clear(new->nodes);
		node_set(first_node(nodes), new->nodes);
	} else {
		new->mode = MPOL_LOCAL;
	}

	/*
	 * Save nodes for contextualization: this will be used to "clone"
	 * the mempolicy in a specific context [cpuset] at a later time.
	 */
	/* user_nodemask 是未来 clone/contextualize 的源表达，不一定等于当时有效节点集合。 */
	new->w.user_nodemask = nodes;

	err = 0;

out:
	/* Restore string for error message */
	/* 无论成败恢复 ':'/'='，保证调用者仍拥有原始可打印字符串。 */
	if (nodelist)
		*--nodelist = ':';
	if (flags)
		*--flags = '=';
	if (!err)
		*mpol = new;
	return err;
}
#endif /* CONFIG_TMPFS */
/* 关闭 TMPFS 时无需挂载字符串解析入口，但通用格式化函数仍保留。 */

/**
 * mpol_to_str - format a mempolicy structure for printing
 * @buffer:  to contain formatted mempolicy string
 * @maxlen:  length of @buffer
 * @pol:  pointer to mempolicy to be formatted
 *
 * Convert @pol into a string.  If @buffer is too short, truncate the string.
 * Recommend a @maxlen of at least 51 for the longest mode, "weighted
 * interleave", plus the longest flag flags, "relative|balancing", and to
 * display at least a few node ids.
 */
/*
 * mpol_to_str() - 将策略格式化为稳定的人类可读字符串
 * 调用位置：tmpfs/procfs 挂载信息与诊断输出；返回后调用者消费 @buffer 文本。
 * @buffer: 输出缓冲区；@maxlen: 总容量；@pol: 可为 NULL/静态默认/静态每节点/动态策略。
 * 返回无直接值；输出始终截断到容量。静态默认与 preferred_node_policy 被显示为 default，
 * 避免暴露内部兜底对象；动态模式附加 static/relative/balancing 与节点列表。调用者保证
 * pol 生命周期；函数只读且不取得引用。建议 maxlen>=51 以容纳最长模式、标志和若干 nid。
 * 上下文：调用者保证策略稳定；snprintf/scnprintf 不睡眠，本函数不取得任何锁。
 */
void mpol_to_str(char *buffer, int maxlen, struct mempolicy *pol)
{
	/* p 是当前写指针；nodes/mode/flags 从安全默认值开始，仅动态策略覆盖。 */
	char *p = buffer;
	nodemask_t nodes = NODE_MASK_NONE;
	unsigned short mode = MPOL_DEFAULT;
	unsigned short flags = 0;

	if (pol &&
	    pol != &default_policy &&
	    !(pol >= &preferred_node_policy[0] &&
	      pol <= &preferred_node_policy[ARRAY_SIZE(preferred_node_policy) - 1])) {
		mode = pol->mode;
		flags = pol->flags;
	}

	/* 静态首选节点对象和 default_policy 没有普通动态对象布局，已在上面单独归一化。 */
	/* 阶段 1：把内部对象规范化为 ABI 模式、标志和可选节点集合。 */
	switch (mode) {
	case MPOL_DEFAULT:
	case MPOL_LOCAL:
		break;
	case MPOL_PREFERRED:
	case MPOL_PREFERRED_MANY:
	case MPOL_BIND:
	case MPOL_INTERLEAVE:
	case MPOL_WEIGHTED_INTERLEAVE:
		nodes = pol->nodes;
		break;
	default:
		/* 未知内部模式以一次性告警和 "unknown" 降级，避免越界索引 policy_modes。 */
		WARN_ON_ONCE(1);
		snprintf(p, maxlen, "unknown");
		return;
	}

	/* 阶段 2：先输出模式，再在剩余容量内追加标志和节点列表；每步更新写指针。 */
	p += snprintf(p, maxlen, "%s", policy_modes[mode]);

	if (flags & MPOL_MODE_FLAGS) {
		p += snprintf(p, buffer + maxlen - p, "=");

		/*
		 * Static and relative are mutually exclusive.
		 */
		/* STATIC 与 RELATIVE 构造时已保证互斥，故文本最多输出其中一个。 */
		if (flags & MPOL_F_STATIC_NODES)
			p += snprintf(p, buffer + maxlen - p, "static");
		else if (flags & MPOL_F_RELATIVE_NODES)
			p += snprintf(p, buffer + maxlen - p, "relative");

		if (flags & MPOL_F_NUMA_BALANCING) {
			if (!is_power_of_2(flags & MPOL_MODE_FLAGS))
				p += snprintf(p, buffer + maxlen - p, "|");
			p += snprintf(p, buffer + maxlen - p, "balancing");
		}
	}

	if (!nodes_empty(nodes))
		p += scnprintf(p, buffer + maxlen - p, ":%*pbl",
			       nodemask_pr_args(&nodes));
}

#ifdef CONFIG_SYSFS
/* 以下 sysfs 目录暴露 weighted_interleave/auto 与每个有内存节点的可写权重。 */
struct iw_node_attr {
	struct kobj_attribute kobj_attr;
	int nid;
};
/* iw_node_attr 把通用 kobj_attribute 嵌入节点属性，并保存 show/store 使用的 nid。 */

struct sysfs_wi_group {
	struct kobject wi_kobj;
	struct mutex kobj_lock;
	struct iw_node_attr *nattrs[];
};
/*
 * sysfs_wi_group 拥有 weighted_interleave kobject、串行化 nattrs 发布/删除的 kobj_lock 和
 * 按 nid 索引的柔性属性指针表。kobject release 最终释放整个 group；每个 attr 单独分配。
 */

static struct sysfs_wi_group *wi_group;
/* 启动成功后全局指向唯一 sysfs 组；其生命由嵌入 kobject 引用计数管理。 */

/*
 * node_show() - 读取单节点当前 weighted-interleave 权重
 * 调用位置：sysfs 读取 weighted_interleave/nodeN 时由 kobj_sysfs_ops 分派。
 * @kobj: 组 kobject 借用；@attr: 嵌入 iw_node_attr；@buf: sysfs 页输出。
 * container_of 恢复节点属性，RCU 安全读取权重，返回写入字节数；无显式状态时显示 1。
 */
static ssize_t node_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	struct iw_node_attr *node_attr;
	u8 weight;

	node_attr = container_of(attr, struct iw_node_attr, kobj_attr);
	weight = get_il_weight(node_attr->nid);
	return sysfs_emit(buf, "%d\n", weight);
}

/*
 * node_store() - 以用户给定非零 u8 权重切换/更新手工 weighted-interleave 模式
 * 调用位置：sysfs 写 weighted_interleave/nodeN 时由 kobj_sysfs_ops 分派。
 * @kobj/@attr: sysfs 对象与节点属性；@buf/@count: 用户文本及长度。
 * 返回 count、-EINVAL 或 -ENOMEM。锁外分配完整新快照，锁内复制旧表（NULL 则全 1）、
 * 更新单 nid 并标记 manual，通过 RCU 发布；解锁后等待宽限期释放旧表。输入 0/空值拒绝。
 */
static ssize_t node_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	/* new 是待发布所有权，old 替换后由本函数释放；weight 是解析后的单节点配额。 */
	struct weighted_interleave_state *new_wi_state, *old_wi_state = NULL;
	struct iw_node_attr *node_attr;
	u8 weight = 0;
	int i;

	/* 阶段 1：从嵌入属性恢复 nid，并在任何分配/加锁前完整验证用户文本。 */
	node_attr = container_of(attr, struct iw_node_attr, kobj_attr);
	if (count == 0 || sysfs_streq(buf, "") ||
	    kstrtou8(buf, 0, &weight) || weight == 0)
		return -EINVAL;

	/* 阶段 2：锁外构造待发布容器，避免在全局写锁内进入内存回收。 */
	new_wi_state = kzalloc_flex(*new_wi_state, iw_table, nr_node_ids);
	if (!new_wi_state)
		return -ENOMEM;

	mutex_lock(&wi_state_lock);
	/* 写锁使复制与发布基于同一旧快照，并与自动模式/性能更新串行。 */
	old_wi_state = rcu_dereference_protected(wi_state,
					lockdep_is_held(&wi_state_lock));
	if (old_wi_state) {
		memcpy(new_wi_state->iw_table, old_wi_state->iw_table,
					nr_node_ids * sizeof(u8));
	} else {
		/* NULL 旧状态按契约展开为每节点权重 1，再只覆盖用户指定的 nid。 */
		for (i = 0; i < nr_node_ids; i++)
			new_wi_state->iw_table[i] = 1;
	}
	new_wi_state->iw_table[node_attr->nid] = weight;
	new_wi_state->mode_auto = false;

	rcu_assign_pointer(wi_state, new_wi_state);
	/* 新快照初始化完整后才 release 发布；读侧只会看到旧表或完整新表。 */
	mutex_unlock(&wi_state_lock);
	/* 阶段 4：发布完成后才等待旧 RCU 读者；等待期间不占用写锁。 */
	if (old_wi_state) {
		synchronize_rcu();
		kfree(old_wi_state);
	}
	return count;
}

/*
 * weighted_interleave_auto_show() - 显示权重模式是否由带宽自动计算
 * 调用位置：sysfs 读取 weighted_interleave/auto 时由 kobj_sysfs_ops 分派。
 * @kobj/@attr: 未使用的 sysfs 借用参数；@buf: 输出缓冲区。返回 "true/false\n" 字节数。
 * RCU 下借用状态；NULL 按 auto=true 契约显示，不改变任何状态。
 */
static ssize_t weighted_interleave_auto_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct weighted_interleave_state *state;
	bool wi_auto = true;

	rcu_read_lock();
	state = rcu_dereference(wi_state);
	if (state)
		wi_auto = state->mode_auto;
	rcu_read_unlock();

	return sysfs_emit(buf, "%s\n", str_true_false(wi_auto));
}

/*
 * weighted_interleave_auto_store() - 在自动带宽权重与手工权重模式之间切换
 * 调用位置：sysfs 写 weighted_interleave/auto 时由 kobj_sysfs_ops 分派。
 * @kobj/@attr: sysfs 借用；@buf/@count: 布尔文本及长度。返回 count、-EINVAL、-ENOMEM
 * 或 -ENODEV（请求 auto 但尚无带宽表）。锁外预分配全 1 快照，锁内检测幂等；切 manual
 * 时保留旧权重，切 auto 时按 node_bw_table 重算，RCU 发布后等待旧读者退出再释放。
 */
static ssize_t weighted_interleave_auto_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct weighted_interleave_state *new_wi_state, *old_wi_state = NULL;
	unsigned int *bw;
	bool input;
	int i;

	/* 阶段 1：先解析布尔值并锁外准备默认全 1 快照。 */
	if (kstrtobool(buf, &input))
		return -EINVAL;

	new_wi_state = kzalloc_flex(*new_wi_state, iw_table, nr_node_ids);
	if (!new_wi_state)
		return -ENOMEM;
	for (i = 0; i < nr_node_ids; i++)
		new_wi_state->iw_table[i] = 1;

	/* 阶段 2：锁内基于同一个旧快照决定幂等、manual 继承或 auto 重算。 */
	mutex_lock(&wi_state_lock);
	old_wi_state = rcu_dereference_protected(wi_state,
				lockdep_is_held(&wi_state_lock));

	if (old_wi_state && input == old_wi_state->mode_auto) {
		/* 已处于请求模式，无需发布等价快照；释放预分配对象后按成功返回。 */
		mutex_unlock(&wi_state_lock);
		kfree(new_wi_state);
		return count;
	}

	if (!input) {
		/* 关闭自动只改变 mode_auto；尽量继承当前权重，NULL 时保留预置全 1。 */
		if (old_wi_state)
			memcpy(new_wi_state->iw_table, old_wi_state->iw_table,
						       nr_node_ids * sizeof(u8));
		goto update_wi_state;
	}

	bw = node_bw_table;
	/* 自动模式必须有性能提供者提交过带宽，否则无法从数据推导比例。 */
	if (!bw) {
		mutex_unlock(&wi_state_lock);
		kfree(new_wi_state);
		return -ENODEV;
	}

	new_wi_state->mode_auto = true;
	/* 阶段 3：带宽表只由同一写锁保护，故可直接计算一个自洽的新权重快照。 */
	reduce_interleave_weights(bw, new_wi_state->iw_table);

update_wi_state:
	/* 两种切换在此汇合：锁内发布、锁外 RCU 宽限期回收。 */
	rcu_assign_pointer(wi_state, new_wi_state);
	mutex_unlock(&wi_state_lock);
	/* 阶段 4：解除写锁后等待旧 RCU 读者并释放被替换快照。 */
	if (old_wi_state) {
		synchronize_rcu();
		kfree(old_wi_state);
	}
	return count;
}

/*
 * sysfs_wi_node_delete() - 删除一个节点的权重属性并释放其对象
 * @nid: 节点 id，越界或尚未创建时无副作用。kobj_lock 下先把 nattrs 槽清 NULL，阻止
 * notifier/并发管理路径再次取得；解锁后移除 sysfs 文件并释放名称和属性。返回无直接值。
 */
static void sysfs_wi_node_delete(int nid)
{
	struct iw_node_attr *attr;

	if (nid < 0 || nid >= nr_node_ids)
		return;

	mutex_lock(&wi_group->kobj_lock);
	attr = wi_group->nattrs[nid];
	if (!attr) {
		mutex_unlock(&wi_group->kobj_lock);
		return;
	}

	wi_group->nattrs[nid] = NULL;
	/* 清槽是管理层摘除点；sysfs_remove_file 随后阻止新的用户回调进入。 */
	mutex_unlock(&wi_group->kobj_lock);

	sysfs_remove_file(&wi_group->wi_kobj, &attr->kobj_attr.attr);
	kfree(attr->kobj_attr.attr.name);
	kfree(attr);
}

/*
 * sysfs_wi_node_delete_all() - 删除所有可能存在的节点权重属性
 * 入参：无；逐 nid 调用幂等 delete，返回无直接值。用于初始化失败和组清理。
 * 上下文：进程/初始化清理上下文，可睡眠；每次 delete 自行取得并释放 kobj_lock。
 */
static void sysfs_wi_node_delete_all(void)
{
	int nid;

	for (nid = 0; nid < nr_node_ids; nid++)
		sysfs_wi_node_delete(nid);
}

/*
 * wi_state_free() - 把全局权重快照恢复为 NULL/自动全 1 并安全释放旧对象
 * 调用位置：wi_cleanup() 在关闭所有 sysfs 用户入口后释放权重快照。
 * 入参：无。wi_state_lock 下摘除 RCU 指针，解锁后 synchronize_rcu，再释放旧表；返回
 * 无直接值。node_bw_table 不在此释放，因为它由性能更新生命周期另行维护。
 */
static void wi_state_free(void)
{
	struct weighted_interleave_state *old_wi_state;

	mutex_lock(&wi_state_lock);
	old_wi_state = rcu_dereference_protected(wi_state,
			lockdep_is_held(&wi_state_lock));
	rcu_assign_pointer(wi_state, NULL);
	mutex_unlock(&wi_state_lock);

	if (old_wi_state) {
		synchronize_rcu();
		kfree(old_wi_state);
	}
}

static struct kobj_attribute wi_auto_attr = {
	.attr = { .name = "auto", .mode = 0664 },
	.show = weighted_interleave_auto_show,
	.store = weighted_interleave_auto_store,
};
/* wi_auto_attr 是静态常驻的 auto 文件描述符，不由 sysfs 删除路径释放内存。 */

/*
 * wi_cleanup() - 撤销 weighted_interleave 组内已发布文件和 RCU 状态
 * 入参：无；先删除 auto，再删除节点文件，最后摘除权重快照。返回无直接值；外层仍负责
 * kobject_del/put。顺序确保用户入口先关闭，再释放其访问的数据。
 */
static void wi_cleanup(void) {
	sysfs_remove_file(&wi_group->wi_kobj, &wi_auto_attr.attr);
	sysfs_wi_node_delete_all();
	wi_state_free();
}

/*
 * wi_kobj_release() - weighted_interleave kobject 最后一份引用释放回调
 * @wi_kobj: 嵌入 wi_group 的对象（参数无需反向取址，因为全局唯一）；释放 wi_group。
 * 返回无直接值。调用前所有 sysfs 文件/子属性必须已清理。
 * 上下文：kobject 引用归零的 release 回调，可释放内存；不持 kobj_lock/wi_state_lock。
 */
static void wi_kobj_release(struct kobject *wi_kobj)
{
	kfree(wi_group);
}

static const struct kobj_type wi_ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
	.release = wi_kobj_release,
};
/* wi_ktype 绑定标准 sysfs 操作和最终 release，保证 kobject_put 能回收 group 容器。 */

/*
 * sysfs_wi_node_add() - 为一个有内存节点创建 nodeN 权重文件
 * @nid: 必须在 [0,nr_node_ids)。可睡眠分配名称/属性。kobj_lock 串行槽位检查、
 * sysfs_create_file 与发布；返回 0、-EINVAL、-ENOMEM、-EEXIST 或 sysfs errno。
 * 成功后 wi_group 拥有 new_attr；失败路径释放 name/attr，不留下半发布槽位。
 * 调用位置：sysfs 组初始化和节点首次获得内存的热插拔通知。
 */
static int sysfs_wi_node_add(int nid)
{
	int ret;
	char *name;
	struct iw_node_attr *new_attr;

	if (nid < 0 || nid >= nr_node_ids) {
		pr_err("invalid node id: %d\n", nid);
		return -EINVAL;
	}

	/* 阶段 1：锁外分配属性及其稳定名称；失败路径尚无全局可见状态。 */
	new_attr = kzalloc_obj(*new_attr);
	if (!new_attr)
		return -ENOMEM;

	name = kasprintf(GFP_KERNEL, "node%d", nid);
	if (!name) {
		kfree(new_attr);
		return -ENOMEM;
	}

	/* 阶段 2：完整初始化回调、权限和 nid，发布前不允许 sysfs 读者进入。 */
	sysfs_attr_init(&new_attr->kobj_attr.attr);
	new_attr->kobj_attr.attr.name = name;
	new_attr->kobj_attr.attr.mode = 0644;
	new_attr->kobj_attr.show = node_show;
	new_attr->kobj_attr.store = node_store;
	new_attr->nid = nid;

	/* 阶段 3：锁内原子完成去重、sysfs 文件创建与 nattrs 槽位发布。 */
	mutex_lock(&wi_group->kobj_lock);
	/* 同一 nid 热插拔/初始化并发时只允许一个属性发布。 */
	if (wi_group->nattrs[nid]) {
		mutex_unlock(&wi_group->kobj_lock);
		ret = -EEXIST;
		goto out;
	}

	ret = sysfs_create_file(&wi_group->wi_kobj, &new_attr->kobj_attr.attr);
	/* 文件创建成功后再写 nattrs，使管理查找不会取得尚未注册的属性。 */
	if (ret) {
		mutex_unlock(&wi_group->kobj_lock);
		goto out;
	}
	wi_group->nattrs[nid] = new_attr;
	mutex_unlock(&wi_group->kobj_lock);
	return 0;

out:
	/* 只有尚未转移给 wi_group 的 new_attr 会到达此回滚出口。 */
	kfree(new_attr->kobj_attr.attr.name);
	kfree(new_attr);
	return ret;
}

/*
 * wi_node_notifier() - 随节点首次获得/最后失去内存动态维护权重 sysfs 文件
 * 调用位置：memory hotplug notifier chain；回调结束后热插拔核心继续其他观察者。
 * @nb: 未使用 notifier；@action: NODE_ADDED_FIRST_MEMORY/REMOVED_LAST_MEMORY 等；@data:
 * node_notify 借用对象。增加失败只记录日志，不阻断热插拔；删除幂等。始终返回 NOTIFY_OK，
 * 表示本观察者不否决节点状态转换。
 * 上下文：阻塞式节点通知进程上下文，可创建/删除 sysfs 文件；不持本组互斥锁进入。
 */
static int wi_node_notifier(struct notifier_block *nb,
			       unsigned long action, void *data)
{
	int err;
	struct node_notify *nn = data;
	int nid = nn->nid;

	/* 按“首次获得内存/最后失去内存”边界增删文件；其他节点事件无需动作。 */
	switch (action) {
	case NODE_ADDED_FIRST_MEMORY:
		err = sysfs_wi_node_add(nid);
		if (err)
			pr_err("failed to add sysfs for node%d during hotplug: %d\n",
			       nid, err);
		break;
	case NODE_REMOVED_LAST_MEMORY:
		sysfs_wi_node_delete(nid);
		break;
	}

	/* notifier 仅记录创建失败，不阻断节点热插拔状态机。 */
	return NOTIFY_OK;
}

/*
 * add_weighted_interleave_group() - 创建 weighted_interleave kobject、auto 与各节点文件
 * 调用位置：mempolicy_sysfs_init() 创建父目录后，构造唯一子组。
 * @mempolicy_kobj: 父 kobject 借用。启动期可睡眠。返回 0 或分配/sysfs errno。
 * 成功注册热插拔 notifier并由 kobject 持有 group；失败按“组内文件 -> kobject_del ->
 * kobject_put/release”逆序回滚，避免泄漏属性或 RCU 状态。
 */
static int __init add_weighted_interleave_group(struct kobject *mempolicy_kobj)
{
	int nid, err;

	/* 阶段 1：分配唯一组容器和按 nid 索引的属性槽，尚未对 sysfs 可见。 */
	wi_group = kzalloc_flex(*wi_group, nattrs, nr_node_ids);
	if (!wi_group)
		return -ENOMEM;
	mutex_init(&wi_group->kobj_lock);

	/* 阶段 2：发布子 kobject 与静态 auto 文件；失败由 kobject release 回收容器。 */
	err = kobject_init_and_add(&wi_group->wi_kobj, &wi_ktype, mempolicy_kobj,
				   "weighted_interleave");
	if (err)
		goto err_put_kobj;

	err = sysfs_create_file(&wi_group->wi_kobj, &wi_auto_attr.attr);
	if (err)
		goto err_put_kobj;

	/* 阶段 3：为初始有内存节点逐个创建文件；任一失败撤销此前全部文件。 */
	for_each_online_node(nid) {
		/* 只为真正有内存的在线节点暴露权重；CPU-only 节点没有分配份额。 */
		if (!node_state(nid, N_MEMORY))
			continue;

		err = sysfs_wi_node_add(nid);
		if (err) {
			pr_err("failed to add sysfs for node%d during init: %d\n",
			       nid, err);
			goto err_cleanup_kobj;
		}
	}

	hotplug_node_notifier(wi_node_notifier, DEFAULT_CALLBACK_PRI);
	/* 所有初始文件成功后才订阅热插拔，避免 notifier 访问半初始化 group。 */
	return 0;

err_cleanup_kobj:
	/* 已创建 auto/部分 node 文件时先清组内容，再删除并 put kobject。 */
	wi_cleanup();
	kobject_del(&wi_group->wi_kobj);
err_put_kobj:
	kobject_put(&wi_group->wi_kobj);
	return err;
}

/*
 * mempolicy_sysfs_init() - late init 创建 /sys/kernel/mm/mempolicy 层级
 * 入参：无。返回 0、-ENOMEM 或子组创建 errno。父 kobject 成功后交由 sysfs 引用管理；
 * 子组失败则 del+put 父对象。late_initcall 保证 mm_kobj 与 NUMA 策略核心已初始化。
 * 上下文：late init 进程上下文，可睡眠；串行初始化，无并发调用者需要额外锁。
 */
static int __init mempolicy_sysfs_init(void)
{
	int err;
	static struct kobject *mempolicy_kobj;

	/* 阶段 1：创建父目录；成功后其引用由本初始化路径持有并在失败时显式 put。 */
	mempolicy_kobj = kobject_create_and_add("mempolicy", mm_kobj);
	if (!mempolicy_kobj)
		return -ENOMEM;

	/* 阶段 2：创建唯一子组；成功后整个层级交给 kobject/sysfs 生命周期。 */
	err = add_weighted_interleave_group(mempolicy_kobj);
	if (err)
		goto err_kobj;

	return 0;

err_kobj:
	kobject_del(mempolicy_kobj);
	kobject_put(mempolicy_kobj);
	return err;
}

late_initcall(mempolicy_sysfs_init);
#endif /* CONFIG_SYSFS */
/* 关闭 SYSFS 时权重仍可由性能坐标自动更新，只是不提供用户可见的手工调节文件。 */
