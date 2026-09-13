// SPDX-License-Identifier: GPL-2.0
#include <linux/slab.h>
#include <linux/lockdep.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/memory.h>
#include <linux/memory-tiers.h>
#include <linux/notifier.h>
#include <linux/sched/sysctl.h>

#include "internal.h"

/*
 * 一个 memory_tier 表示相同抽象距离区间内的一组 memory_dev_type；list 按距离
 * 从快到慢排序，device 把该层发布到 sysfs。所有成员关系由 memory_tier_lock
 * 保护；pgdat->memtier 另以 RCU 指针发布给迁移和回收热路径。
 */
struct memory_tier {
	/* hierarchy of memory tiers */
	/* 按 adistance_start 排序的全局层级链节点。 */
	struct list_head list;
	/* list of all memory types part of this tier */
	/* 本 tier 内各设备类型的 tier_sibling 链。 */
	struct list_head memory_types;
	/*
	 * start value of abstract distance. memory tier maps
	 * an abstract distance  range,
	 * adistance_start .. adistance_start + MEMTIER_CHUNK_SIZE
	 */
	/* 原注释译注：本层抽象距离区间的起点，范围为 start 到 start+CHUNK_SIZE。 */
	int adistance_start;
	/* sysfs 生命周期锚点，最后 put 后进入 release。 */
	struct device dev;
	/* All the nodes that are part of all the lower memory tiers. */
	/* 原注释译注：所有更低 tier 节点的并集，作为 demotion 备用目标集合。 */
	nodemask_t lower_tier_mask;
};

struct demotion_nodes {
	/* 同一最佳距离的首选降级节点，RCU 读者随机选取。 */
	nodemask_t preferred;
};

struct node_memory_type_map {
	/* node 当前归属的设备类型，锁保护。 */
	struct memory_dev_type *memtype;
	/* 映射到该类型的设备数，首个映射持一份 kref。 */
	int map_count;
};

/* 串行化 tier/type/node 关系及 sysfs 注册；不能替代 pgdat->memtier 的 RCU 生命周期。 */
static DEFINE_MUTEX(memory_tier_lock);
static LIST_HEAD(memory_tiers);
/*
 * The list is used to store all memory types that are not created
 * by a device driver.
 */
/* 原注释译注：此链保存非驱动创建的默认 memory type，由节点初始化路径借用。 */
static LIST_HEAD(default_memory_types);
static struct node_memory_type_map node_memory_types[MAX_NUMNODES];
struct memory_dev_type *default_dram_type;
nodemask_t default_dram_nodes __initdata = NODE_MASK_NONE;

static const struct bus_type memory_tier_subsys = {
	.name = "memory_tiering",
	.dev_name = "memory_tier",
};

#ifdef CONFIG_NUMA_BALANCING
/**
 * folio_use_access_time - check if a folio reuses cpupid for page access time
 * @folio: folio to check
 *
 * folio's _last_cpupid field is repurposed by memory tiering. In memory
 * tiering mode, cpupid of slow memory folio (not toptier memory) is used to
 * record page access time.
 *
 * Return: the folio _last_cpupid is used to record page access time
 */
/*
 * 原注释译注：tiering 模式复用 slow-memory folio 的 _last_cpupid 记录访问时间。
 * 业务背景：NUMA balancing 仅在内存层级启用且 folio 不在 top tier 时把这个字段
 * 从 CPU/PID 最近访问编码切换为温度时间戳。入参：folio 是当前持有的借用对象。
 * 出参/返回：true 表示调用者可按访问时间解释字段。注意事项：配置关闭时无此实现；
 * node_is_toptier() 经 RCU 读 tier，结果是瞬时分类而非节点长期在线保证。
 */
bool folio_use_access_time(struct folio *folio)
{
	return (sysctl_numa_balancing_mode & NUMA_BALANCING_MEMORY_TIERING) &&
	       !node_is_toptier(folio_nid(folio));
}
#endif

#ifdef CONFIG_NUMA_MIGRATION
/*
 * 业务背景：把有 CPU 的最高可提升 tier 的抽象距离上界缓存下来，供 slow folio
 * 判断是否应按访问时间追踪。它由 establish_demotion_targets() 在 tier lock 下重算。
 */
static int top_tier_adistance;
/*
 * node_demotion[] examples:
 *
 * Example 1:
 *
 * Node 0 & 1 are CPU + DRAM nodes, node 2 & 3 are PMEM nodes.
 *
 * node distances:
 * node   0    1    2    3
 *    0  10   20   30   40
 *    1  20   10   40   30
 *    2  30   40   10   40
 *    3  40   30   40   10
 *
 * memory_tiers0 = 0-1
 * memory_tiers1 = 2-3
 *
 * node_demotion[0].preferred = 2
 * node_demotion[1].preferred = 3
 * node_demotion[2].preferred = <empty>
 * node_demotion[3].preferred = <empty>
 *
 * Example 2:
 *
 * Node 0 & 1 are CPU + DRAM nodes, node 2 is memory-only DRAM node.
 *
 * node distances:
 * node   0    1    2
 *    0  10   20   30
 *    1  20   10   30
 *    2  30   30   10
 *
 * memory_tiers0 = 0-2
 *
 * node_demotion[0].preferred = <empty>
 * node_demotion[1].preferred = <empty>
 * node_demotion[2].preferred = <empty>
 *
 * Example 3:
 *
 * Node 0 is CPU + DRAM nodes, Node 1 is HBM node, node 2 is PMEM node.
 *
 * node distances:
 * node   0    1    2
 *    0  10   20   30
 *    1  20   10   40
 *    2  30   40   10
 *
 * memory_tiers0 = 1
 * memory_tiers1 = 0
 * memory_tiers2 = 2
 *
 * node_demotion[0].preferred = 2
 * node_demotion[1].preferred = 0
 * node_demotion[2].preferred = <empty>
 *
 */
static struct demotion_nodes *node_demotion __read_mostly;
#endif /* CONFIG_NUMA_MIGRATION */

static BLOCKING_NOTIFIER_HEAD(mt_adistance_algorithms);

/* The lock is used to protect `default_dram_perf*` info and nid. */
/* 原注释译注：此锁保护 default_dram_perf 族信息和参考 nid，防止算法/热插拔交错发布。 */
static DEFINE_MUTEX(default_dram_perf_lock);
static bool default_dram_perf_error;
static struct access_coordinate default_dram_perf;
static int default_dram_perf_ref_nid = NUMA_NO_NODE;
static const char *default_dram_perf_ref_source;

/* device 嵌入 memory_tier；container_of 只在 release/sysfs 传入本总线设备时有效。 */
static inline struct memory_tier *to_memory_tier(struct device *device)
{
	return container_of(device, struct memory_tier, dev);
}

/*
 * 业务背景：收集一个 tier 内所有 type 的节点，为 sysfs 展示和 demotion 构建提供快照。
 * 入参：memtier 在 memory_tier_lock 下借用；出参/返回：按值返回节点掩码。
 * 注意事项：遍历 tier_sibling 不取得引用；锁外调用会与 type 链接/摘除竞争。
 */
static __always_inline nodemask_t get_memtier_nodemask(struct memory_tier *memtier)
{
	nodemask_t nodes = NODE_MASK_NONE;
	struct memory_dev_type *memtype;

	list_for_each_entry(memtype, &memtier->memory_types, tier_sibling)
		nodes_or(nodes, nodes, memtype->nodes);

	return nodes;
}

/*
 * 业务背景：device core 最后一个引用释放时回收其嵌入的 tier。
 * 入参：dev 是注册过的 memory_tier device；出参/返回：无，释放 tier 存储。
 * 注意事项：clear_node_memory_tier() 已先 RCU 同步，保证读者不再持有 pgdat->memtier；
 * 不能提前 kfree 或改为 kfree_rcu，否则 device 生命周期与 list 摘除会失配。
 */
static void memory_tier_device_release(struct device *dev)
{
	struct memory_tier *tier = to_memory_tier(dev);
	/*
	 * synchronize_rcu in clear_node_memory_tier makes sure
	 * we don't have rcu access to this memory tier.
	 */
	/* 原注释译注：clear_node_memory_tier() 的 synchronize_rcu 保证没有 RCU 读者访问该 tier。 */
	kfree(tier);
}

/*
 * 业务背景：sysfs nodelist 属性把 tier 内当前节点集合导出给用户空间。
 * 入参：dev 是 tier 设备，attr 未使用，buf 是 sysfs 提供的输出缓冲。出参/返回：
 * 写入字节数或错误。注意事项：持 memory_tier_lock 获取一致成员快照，随后在锁内
 * sysfs_emit；不保存 type/tier 指针到锁外。
 */
static ssize_t nodelist_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	int ret;
	nodemask_t nmask;

	mutex_lock(&memory_tier_lock);
	nmask = get_memtier_nodemask(to_memory_tier(dev));
	ret = sysfs_emit(buf, "%*pbl\n", nodemask_pr_args(&nmask));
	mutex_unlock(&memory_tier_lock);
	return ret;
}
static DEVICE_ATTR_RO(nodelist);

static struct attribute *memtier_dev_attrs[] = {
	&dev_attr_nodelist.attr,
	NULL
};

static const struct attribute_group memtier_dev_group = {
	.attrs = memtier_dev_attrs,
};

static const struct attribute_group *memtier_dev_groups[] = {
	&memtier_dev_group,
	NULL
};

/*
 * 业务背景：把一个 device type 放入其抽象距离 chunk 对应的 tier；不存在则按距离
 * 创建、插入全局链并注册 sysfs device。
 * 入参：memtype 由调用者持有且尚可链接；出参/返回：所属 tier 或 ERR_PTR。
 * 注意事项：必须持 memory_tier_lock；已有 tier_sibling 却找不到同 chunk 是不变量
 * 破坏。device_register 成功后 device core 接管 release 时机；失败逆序摘链并 put。
 */
static struct memory_tier *find_create_memory_tier(struct memory_dev_type *memtype)
{
	int ret;
	bool found_slot = false;
	struct memory_tier *memtier, *new_memtier;
	int adistance = memtype->adistance;
	unsigned int memtier_adistance_chunk_size = MEMTIER_CHUNK_SIZE;

	lockdep_assert_held_once(&memory_tier_lock);

	adistance = round_down(adistance, memtier_adistance_chunk_size);
	/*
	 * If the memtype is already part of a memory tier,
	 * just return that.
	 */
	/* 原注释译注：memtype 已属于某 tier 时直接返回；找不到同距离层即报内部错误。 */
	if (!list_empty(&memtype->tier_sibling)) {
		list_for_each_entry(memtier, &memory_tiers, list) {
			if (adistance == memtier->adistance_start)
				return memtier;
		}
		WARN_ON(1);
		return ERR_PTR(-EINVAL);
	}

	list_for_each_entry(memtier, &memory_tiers, list) {
		if (adistance == memtier->adistance_start) {
			goto link_memtype;
		} else if (adistance < memtier->adistance_start) {
			found_slot = true;
			break;
		}
	}

	/* 阶段 1：按距离找到插入点，分配零化 tier 并先把它链接到有序链。 */
	new_memtier = kzalloc_obj(struct memory_tier);
	if (!new_memtier)
		return ERR_PTR(-ENOMEM);

	new_memtier->adistance_start = adistance;
	INIT_LIST_HEAD(&new_memtier->list);
	INIT_LIST_HEAD(&new_memtier->memory_types);
	if (found_slot)
		list_add_tail(&new_memtier->list, &memtier->list);
	else
		list_add_tail(&new_memtier->list, &memory_tiers);

	new_memtier->dev.id = adistance >> MEMTIER_CHUNK_BITS;
	new_memtier->dev.bus = &memory_tier_subsys;
	new_memtier->dev.release = memory_tier_device_release;
	new_memtier->dev.groups = memtier_dev_groups;

	/* 阶段 2：注册 sysfs device；失败时删除链节点并 put，让 release 回收分配。 */
	ret = device_register(&new_memtier->dev);
	if (ret) {
		list_del(&new_memtier->list);
		put_device(&new_memtier->dev);
		return ERR_PTR(ret);
	}
	memtier = new_memtier;

	/* 阶段 3：最终把 type 链到 tier；此后所有 node 查询可经 type 找到该层。 */
link_memtype:
	list_add(&memtype->tier_sibling, &memtier->memory_types);
	return memtier;
}

/*
 * 业务背景：写侧重建 tier 时在 mutex 下读取节点当前 tier，避免额外 RCU 临界区。
 * 入参：node 为 NUMA 节点编号；出参/返回：tier 借用指针或 NULL。
 * 注意事项：仅 memory_tier_lock 持有者可调用；返回值不能逃逸出该锁，否则节点清理
 * 可在 RCU grace period 后释放它。
 */
static struct memory_tier *__node_get_memory_tier(int node)
{
	pg_data_t *pgdat;

	pgdat = NODE_DATA(node);
	if (!pgdat)
		return NULL;
	/*
	 * Since we hold memory_tier_lock, we can avoid
	 * RCU read locks when accessing the details. No
	 * parallel updates are possible here.
	 */
	/* 原注释译注：持 memory_tier_lock 时可免 RCU 读锁，不存在并行更新。 */
	return rcu_dereference_check(pgdat->memtier,
				     lockdep_is_held(&memory_tier_lock));
}

#ifdef CONFIG_NUMA_MIGRATION
/*
 * 业务背景：NUMA balancing 要区分快 tier 与可提升的慢 tier；未分配 tier 的节点按
 * top tier 处理以保守地避免错误温度记录。
 * 入参：node 为节点编号。出参/返回：true 表示 tier 距离不大于 top_tier_adistance。
 * 注意事项：RCU 只保证 memtier 内存在读侧临界区有效，不保证节点持续 online；调用者
 * 不可持裸 tier 指针离开 rcu_read_lock。
 */
bool node_is_toptier(int node)
{
	bool toptier;
	pg_data_t *pgdat;
	struct memory_tier *memtier;

	pgdat = NODE_DATA(node);
	if (!pgdat)
		return false;

	/* 在 RCU 临界区快照 pgdat->memtier；NULL 是启动/未归类节点的 top-tier 退化。 */
	rcu_read_lock();
	memtier = rcu_dereference(pgdat->memtier);
	if (!memtier) {
		toptier = true;
		goto out;
	}
	if (memtier->adistance_start <= top_tier_adistance)
		toptier = true;
	else
		toptier = false;
out:
	rcu_read_unlock();
	return toptier;
}

/*
 * 业务背景：迁移/回收在降级前取得当前节点所有低 tier 的允许目标集合。
 * 入参：pgdat 是节点描述符借用指针；targets 是调用者提供的输出掩码。
 * 出参/返回：无；写入 lower_tier_mask 或空掩码。注意事项：RCU 覆盖指针解引用和
 * mask 复制；输出为按值快照，节点后续热插拔/重建不会自动更新调用者副本。
 */
void node_get_allowed_targets(pg_data_t *pgdat, nodemask_t *targets)
{
	struct memory_tier *memtier;

	/*
	 * pg_data_t.memtier updates includes a synchronize_rcu()
	 * which ensures that we either find NULL or a valid memtier
	 * in NODE_DATA. protect the access via rcu_read_lock();
	 */
	/* 原注释译注：更新包含 synchronize_rcu，读者在 RCU 锁下只会看到 NULL 或有效 tier。 */
	rcu_read_lock();
	memtier = rcu_dereference(pgdat->memtier);
	if (memtier)
		*targets = memtier->lower_tier_mask;
	else
		*targets = NODE_MASK_NONE;
	rcu_read_unlock();
}

/**
 * next_demotion_node() - Get the next node in the demotion path
 * @node: The starting node to lookup the next node
 * @allowed_mask: The pointer to allowed node mask
 *
 * Return: node id for next memory node in the demotion path hierarchy
 * from @node; NUMA_NO_NODE if @node is terminal.  This does not keep
 * @node online or guarantee that it *continues* to be the next demotion
 * target.
 */
/*
 * 原注释译注：返回降级路径的下一节点，终点返回 NUMA_NO_NODE；不保证 node 在线
 * 或继续保持该 target。业务背景：优先 mask 命中时随机选择等距节点，否则在允许
 * 集合中选择最近备用节点。
 * 入参：node 为源节点，allowed_mask 限制本次可用目标。出参/返回：节点号或终点。
 * 注意事项：node_demotion 可并发更新，RCU 只包住一致读取；选择后的节点仍须由
 * 下游分配路径重验可用性，不能把本结果当引用。
 */
int next_demotion_node(int node, const nodemask_t *allowed_mask)
{
	struct demotion_nodes *nd;
	nodemask_t mask;

	if (!node_demotion)
		return NUMA_NO_NODE;

	nd = &node_demotion[node];

	/*
	 * node_demotion[] is updated without excluding this
	 * function from running.
	 *
	 * Make sure to use RCU over entire code blocks if
	 * node_demotion[] reads need to be consistent.
	 */
	/* 原注释译注：node_demotion 可在本函数运行时更新，所有相关读取须在同一 RCU 块内。 */
	rcu_read_lock();
	/* Filter out nodes that are not in allowed_mask. */
	/* 原注释译注：过滤不在 allowed_mask 的首选节点，保证本次策略约束优先。 */
	nodes_and(mask, nd->preferred, *allowed_mask);
	rcu_read_unlock();

	/*
	 * If there are multiple target nodes, just select one
	 * target node randomly.
	 *
	 * In addition, we can also use round-robin to select
	 * target node, but we should introduce another variable
	 * for node_demotion[] to record last selected target node,
	 * that may cause cache ping-pong due to the changing of
	 * last target node. Or introducing per-cpu data to avoid
	 * caching issue, which seems more complicated. So selecting
	 * target node randomly seems better until now.
	 */
	/* 原注释译注：多个首选目标随机选择，避免共享轮转游标的 cache ping-pong。 */
	if (!nodes_empty(mask))
		return node_random(&mask);

	/*
	 * Preferred nodes are not in allowed_mask. Flip bits in
	 * allowed_mask as used node mask. Then, use it to get the
	 * closest demotion target.
	 */
	nodes_complement(mask, *allowed_mask);
	return find_next_best_node(node, &mask);
}

static void disable_all_demotion_targets(void)
{
	struct memory_tier *memtier;
	int node;

	for_each_node_state(node, N_MEMORY) {
		node_demotion[node].preferred = NODE_MASK_NONE;
		/*
		 * We are holding memory_tier_lock, it is safe
		 * to access pgda->memtier.
		 */
		memtier = __node_get_memory_tier(node);
		if (memtier)
			memtier->lower_tier_mask = NODE_MASK_NONE;
	}
	/*
	 * Ensure that the "disable" is visible across the system.
	 * Readers will see either a combination of before+disable
	 * state or disable+after.  They will never see before and
	 * after state together.
	 */
	synchronize_rcu();
}

static void dump_demotion_targets(void)
{
	int node;

	for_each_node_state(node, N_MEMORY) {
		struct memory_tier *memtier = __node_get_memory_tier(node);
		nodemask_t preferred = node_demotion[node].preferred;

		if (!memtier)
			continue;

		if (nodes_empty(preferred))
			pr_info("Demotion targets for Node %d: null\n", node);
		else
			pr_info("Demotion targets for Node %d: preferred: %*pbl, fallback: %*pbl\n",
				node, nodemask_pr_args(&preferred),
				nodemask_pr_args(&memtier->lower_tier_mask));
	}
}

/*
 * Find an automatic demotion target for all memory
 * nodes. Failing here is OK.  It might just indicate
 * being at the end of a chain.
 */
/*
 * 原注释译注：为所有 memory node 建立自动降级目标；失败可以只是链尾。
 * 业务背景：在 mutex 下先清旧图，经 RCU 屏障后为每节点选择下一低 tier 的同距
 * preferred 节点，并计算 lower_tier fallback 与 top tier 距离边界。
 * 入参/返回：无；直接重建全局 demotion 状态。注意事项：节点热插拔后必须重建，
 * 不把选中的 node 当长期在线引用。
 */
static void establish_demotion_targets(void)
{
	struct memory_tier *memtier;
	struct demotion_nodes *nd;
	int target = NUMA_NO_NODE, node;
	int distance, best_distance;
	nodemask_t tier_nodes, lower_tier;

	lockdep_assert_held_once(&memory_tier_lock);

	if (!node_demotion)
		return;

	disable_all_demotion_targets();

	/* 阶段 1：每个非末层节点从紧邻低 tier 选择全部同最短距离的 preferred target。 */
	for_each_node_state(node, N_MEMORY) {
		best_distance = -1;
		nd = &node_demotion[node];

		memtier = __node_get_memory_tier(node);
		if (!memtier || list_is_last(&memtier->list, &memory_tiers))
			continue;
		/*
		 * Get the lower memtier to find the  demotion node list.
		 */
		memtier = list_next_entry(memtier, list);
		tier_nodes = get_memtier_nodemask(memtier);
		/*
		 * find_next_best_node, use 'used' nodemask as a skip list.
		 * Add all memory nodes except the selected memory tier
		 * nodelist to skip list so that we find the best node from the
		 * memtier nodelist.
		 */
		/* 原注释译注：用 used 掩码跳过 tier 外节点，使最近搜索只落到选定低层。 */
		nodes_andnot(tier_nodes, node_states[N_MEMORY], tier_nodes);

		/*
		 * Find all the nodes in the memory tier node list of same best distance.
		 * add them to the preferred mask. We randomly select between nodes
		 * in the preferred mask when allocating pages during demotion.
		 */
		/* 重复搜索直到距离变差，避免只随机保留第一个同距候选。 */
		do {
			target = find_next_best_node(node, &tier_nodes);
			if (target == NUMA_NO_NODE)
				break;

			distance = node_distance(node, target);
			if (distance == best_distance || best_distance == -1) {
				best_distance = distance;
				node_set(target, nd->preferred);
			} else {
				break;
			}
		} while (1);
	}
	/*
	 * Promotion is allowed from a memory tier to higher
	 * memory tier only if the memory tier doesn't include
	 * compute. We want to skip promotion from a memory tier,
	 * if any node that is part of the memory tier have CPUs.
	 * Once we detect such a memory tier, we consider that tier
	 * as top tiper from which promotion is not allowed.
	 */
	/* 阶段 2：反向找到第一个含 CPU 的 tier，其抽象距离上界定义 top tier。 */
	list_for_each_entry_reverse(memtier, &memory_tiers, list) {
		tier_nodes = get_memtier_nodemask(memtier);
		if (nodes_and(tier_nodes, node_states[N_CPU], tier_nodes)) {
			/*
			 * abstract distance below the max value of this memtier
			 * is considered toptier.
			 */
			top_tier_adistance = memtier->adistance_start +
						MEMTIER_CHUNK_SIZE - 1;
			break;
		}
	}
	/*
	 * Now build the lower_tier mask for each node collecting node mask from
	 * all memory tier below it. This allows us to fallback demotion page
	 * allocation to a set of nodes that is closer the above selected
	 * preferred node.
	 */
	/* 阶段 3：逐层剔除当前及更快节点，生成该层可回退的所有更慢 memory nodes。 */
	lower_tier = node_states[N_MEMORY];
	list_for_each_entry(memtier, &memory_tiers, list) {
		/*
		 * Keep removing current tier from lower_tier nodes,
		 * This will remove all nodes in current and above
		 * memory tier from the lower_tier mask.
		 */
		tier_nodes = get_memtier_nodemask(memtier);
		nodes_andnot(lower_tier, lower_tier, tier_nodes);
		memtier->lower_tier_mask = lower_tier;
	}

	dump_demotion_targets();
}

#else
static inline void establish_demotion_targets(void) {}
#endif /* CONFIG_NUMA_MIGRATION */

/* 将 node 映射到 type；同 type 首次映射取得一份 kref，后续仅增加 map_count。 */
static inline void __init_node_memory_type(int node, struct memory_dev_type *memtype)
{
	if (!node_memory_types[node].memtype)
		node_memory_types[node].memtype = memtype;
	/*
	 * for each device getting added in the same NUMA node
	 * with this specific memtype, bump the map count. We
	 * Only take memtype device reference once, so that
	 * changing a node memtype can be done by dropping the
	 * only reference count taken here.
	 */

	if (node_memory_types[node].memtype == memtype) {
		if (!node_memory_types[node].map_count++)
			kref_get(&memtype->kref);
	}
}

/* 在 memory_tier_lock 下为 online memory node 获取 type、加入 tier，再 RCU 发布 pgdat->memtier。 */
static struct memory_tier *set_node_memory_tier(int node)
{
	struct memory_tier *memtier;
	struct memory_dev_type *memtype = default_dram_type;
	int adist = MEMTIER_ADISTANCE_DRAM;
	pg_data_t *pgdat = NODE_DATA(node);


	lockdep_assert_held_once(&memory_tier_lock);

	if (!node_state(node, N_MEMORY))
		return ERR_PTR(-EINVAL);

	/* 距离算法失败时使用默认 DRAM type；成功建 tier 后才能发布给 RCU 读者。 */
	mt_calc_adistance(node, &adist);
	if (!node_memory_types[node].memtype) {
		memtype = mt_find_alloc_memory_type(adist, &default_memory_types);
		if (IS_ERR(memtype)) {
			memtype = default_dram_type;
			pr_info("Failed to allocate a memory type. Fall back.\n");
		}
	}

	__init_node_memory_type(node, memtype);

	memtype = node_memory_types[node].memtype;
	node_set(node, memtype->nodes);
	memtier = find_create_memory_tier(memtype);
	if (!IS_ERR(memtier))
		rcu_assign_pointer(pgdat->memtier, memtier);
	return memtier;
}

/* 空 tier 先摘全局链再注销 device；device release 负责延迟到最后引用的内存回收。 */
static void destroy_memory_tier(struct memory_tier *memtier)
{
	list_del(&memtier->list);
	device_unregister(&memtier->dev);
}

/* 节点下线时先 RCU 发布 NULL、等待旧 reader，再拆 type/tier 链并按需销毁空 tier。 */
static bool clear_node_memory_tier(int node)
{
	bool cleared = false;
	pg_data_t *pgdat;
	struct memory_tier *memtier;

	pgdat = NODE_DATA(node);
	if (!pgdat)
		return false;

	/*
	 * Make sure that anybody looking at NODE_DATA who finds
	 * a valid memtier finds memory_dev_types with nodes still
	 * linked to the memtier. We achieve this by waiting for
	 * rcu read section to finish using synchronize_rcu.
	 * This also enables us to free the destroyed memory tier
	 * with kfree instead of kfree_rcu
	 */
	/* 原注释译注：等 RCU 读段完成后再拆 nodes/type 链，因此普通 device release 可 kfree。 */
	memtier = __node_get_memory_tier(node);
	if (memtier) {
		struct memory_dev_type *memtype;

		rcu_assign_pointer(pgdat->memtier, NULL);
		synchronize_rcu();
		memtype = node_memory_types[node].memtype;
		node_clear(node, memtype->nodes);
		if (nodes_empty(memtype->nodes)) {
			list_del_init(&memtype->tier_sibling);
			if (list_empty(&memtier->memory_types))
				destroy_memory_tier(memtier);
		}
		cleared = true;
	}
	return cleared;
}

/* memory_dev_type 的最后 kref 回调；任何仍挂在 node/tier/list 的 type 都不得到这里。 */
static void release_memtype(struct kref *kref)
{
	struct memory_dev_type *memtype;

	memtype = container_of(kref, struct memory_dev_type, kref);
	kfree(memtype);
}

/* 分配未关联 node 的 type；调用者取得初始 kref，后续由 put_memory_type() 配对。 */
struct memory_dev_type *alloc_memory_type(int adistance)
{
	struct memory_dev_type *memtype;

	memtype = kmalloc_obj(*memtype);
	if (!memtype)
		return ERR_PTR(-ENOMEM);

	memtype->adistance = adistance;
	INIT_LIST_HEAD(&memtype->tier_sibling);
	memtype->nodes  = NODE_MASK_NONE;
	kref_init(&memtype->kref);
	return memtype;
}
EXPORT_SYMBOL_GPL(alloc_memory_type);

/* 归还一份 type kref，最后引用触发 release；不得在锁外继续使用可能被释放的 type。 */
void put_memory_type(struct memory_dev_type *memtype)
{
	kref_put(&memtype->kref, release_memtype);
}
EXPORT_SYMBOL_GPL(put_memory_type);

/* 公共包装：在 memory_tier_lock 下把一个设备映射计入 node/type 关系。 */
void init_node_memory_type(int node, struct memory_dev_type *memtype)
{

	mutex_lock(&memory_tier_lock);
	__init_node_memory_type(node, memtype);
	mutex_unlock(&memory_tier_lock);
}
EXPORT_SYMBOL_GPL(init_node_memory_type);

/* 公共反向操作：减少 map_count，归零时断开 node 的 type 并归还首个映射持有的 kref。 */
void clear_node_memory_type(int node, struct memory_dev_type *memtype)
{
	mutex_lock(&memory_tier_lock);
	if (node_memory_types[node].memtype == memtype || !memtype)
		node_memory_types[node].map_count--;
	/*
	 * If we unmapped all the attached devices to this node,
	 * clear the node memory type.
	 */
	/* 原注释译注：若该节点所有附属设备均解除映射，则清空 node memory type。 */
	if (!node_memory_types[node].map_count) {
		memtype = node_memory_types[node].memtype;
		node_memory_types[node].memtype = NULL;
		put_memory_type(memtype);
	}
	mutex_unlock(&memory_tier_lock);
}
EXPORT_SYMBOL_GPL(clear_node_memory_type);

/* 在调用者给定 type 链中复用同距离对象或分配并插入新对象；返回者借用/持有原有 kref 规则不变。 */
struct memory_dev_type *mt_find_alloc_memory_type(int adist, struct list_head *memory_types)
{
	struct memory_dev_type *mtype;

	list_for_each_entry(mtype, memory_types, list)
		if (mtype->adistance == adist)
			return mtype;

	mtype = alloc_memory_type(adist);
	if (IS_ERR(mtype))
		return mtype;

	list_add(&mtype->list, memory_types);

	return mtype;
}
EXPORT_SYMBOL_GPL(mt_find_alloc_memory_type);

/* 清空临时 type 链并逐项 put；safe 遍历允许当前节点删除。 */
void mt_put_memory_types(struct list_head *memory_types)
{
	struct memory_dev_type *mtype, *mtn;

	list_for_each_entry_safe(mtype, mtn, memory_types, list) {
		list_del(&mtype->list);
		put_memory_type(mtype);
	}
}
EXPORT_SYMBOL_GPL(mt_put_memory_types);

/*
 * This is invoked via `late_initcall()` to initialize memory tiers for
 * memory nodes, both with and without CPUs. After the initialization of
 * firmware and devices, adistance algorithms are expected to be provided.
 */
/* 原注释译注：late_initcall 在固件/设备及距离算法就绪后，为带/不带 CPU 的 memory node 初始化 tier。 */
static int __init memory_tier_late_init(void)
{
	int nid;
	struct memory_tier *memtier;

	get_online_mems();
	guard(mutex)(&memory_tier_lock);

	/* Assign each uninitialized N_MEMORY node to a memory tier. */
	for_each_node_state(nid, N_MEMORY) {
		/*
		 * Some device drivers may have initialized
		 * memory tiers, potentially bringing memory nodes
		 * online and configuring memory tiers.
		 * Exclude them here.
		 */
		if (node_memory_types[nid].memtype)
			continue;

		memtier = set_node_memory_tier(nid);
		if (IS_ERR(memtier))
			continue;
	}

	establish_demotion_targets();
	put_online_mems();

	return 0;
}
late_initcall(memory_tier_late_init);

/* 把访问坐标作为诊断输出；coord/prefix 都是只读借用，不改变算法状态。 */
static void dump_hmem_attrs(struct access_coordinate *coord, const char *prefix)
{
	pr_info(
"%sread_latency: %u, write_latency: %u, read_bandwidth: %u, write_bandwidth: %u\n",
		prefix, coord->read_latency, coord->write_latency,
		coord->read_bandwidth, coord->write_bandwidth);
}

/* 在 mutex 下建立或校验默认 DRAM 性能基线；差异超过 10% 永久禁用该距离算法。 */
int mt_set_default_dram_perf(int nid, struct access_coordinate *perf,
			     const char *source)
{
	guard(mutex)(&default_dram_perf_lock);
	if (default_dram_perf_error)
		return -EIO;

	if (perf->read_latency + perf->write_latency == 0 ||
	    perf->read_bandwidth + perf->write_bandwidth == 0)
		return -EINVAL;

	if (default_dram_perf_ref_nid == NUMA_NO_NODE) {
		default_dram_perf = *perf;
		default_dram_perf_ref_nid = nid;
		default_dram_perf_ref_source = kstrdup(source, GFP_KERNEL);
		return 0;
	}

	/*
	 * The performance of all default DRAM nodes is expected to be
	 * same (that is, the variation is less than 10%).  And it
	 * will be used as base to calculate the abstract distance of
	 * other memory nodes.
	 */
	/* 原注释译注：默认 DRAM 性能应在 10% 内一致，并作为其它 memory node 距离基准。 */
	if (abs(perf->read_latency - default_dram_perf.read_latency) * 10 >
	    default_dram_perf.read_latency ||
	    abs(perf->write_latency - default_dram_perf.write_latency) * 10 >
	    default_dram_perf.write_latency ||
	    abs(perf->read_bandwidth - default_dram_perf.read_bandwidth) * 10 >
	    default_dram_perf.read_bandwidth ||
	    abs(perf->write_bandwidth - default_dram_perf.write_bandwidth) * 10 >
	    default_dram_perf.write_bandwidth) {
		pr_info(
"memory-tiers: the performance of DRAM node %d mismatches that of the reference\n"
"DRAM node %d.\n", nid, default_dram_perf_ref_nid);
		pr_info("  performance of reference DRAM node %d from %s:\n",
			default_dram_perf_ref_nid, default_dram_perf_ref_source);
		dump_hmem_attrs(&default_dram_perf, "    ");
		pr_info("  performance of DRAM node %d from %s:\n", nid, source);
		dump_hmem_attrs(perf, "    ");
		pr_info(
"  disable default DRAM node performance based abstract distance algorithm.\n");
		default_dram_perf_error = true;
		return -EINVAL;
	}

	return 0;
}

/* 把延迟正比、带宽反比的性能坐标换算为抽象距离；要求已建立有效默认 DRAM 基线。 */
int mt_perf_to_adistance(struct access_coordinate *perf, int *adist)
{
	guard(mutex)(&default_dram_perf_lock);
	if (default_dram_perf_error)
		return -EIO;

	if (perf->read_latency + perf->write_latency == 0 ||
	    perf->read_bandwidth + perf->write_bandwidth == 0)
		return -EINVAL;

	if (default_dram_perf_ref_nid == NUMA_NO_NODE)
		return -ENOENT;

	/*
	 * The abstract distance of a memory node is in direct proportion to
	 * its memory latency (read + write) and inversely proportional to its
	 * memory bandwidth (read + write).  The abstract distance, memory
	 * latency, and memory bandwidth of the default DRAM nodes are used as
	 * the base.
	 */
	*adist = MEMTIER_ADISTANCE_DRAM *
		(perf->read_latency + perf->write_latency) /
		(default_dram_perf.read_latency + default_dram_perf.write_latency) *
		(default_dram_perf.read_bandwidth + default_dram_perf.write_bandwidth) /
		(perf->read_bandwidth + perf->write_bandwidth);

	return 0;
}
EXPORT_SYMBOL_GPL(mt_perf_to_adistance);

/**
 * register_mt_adistance_algorithm() - Register memory tiering abstract distance algorithm
 * @nb: The notifier block which describe the algorithm
 *
 * Return: 0 on success, errno on error.
 *
 * Every memory tiering abstract distance algorithm provider needs to
 * register the algorithm with register_mt_adistance_algorithm().  To
 * calculate the abstract distance for a specified memory node, the
 * notifier function will be called unless some high priority
 * algorithm has provided result.  The prototype of the notifier
 * function is as follows,
 *
 *   int (*algorithm_notifier)(struct notifier_block *nb,
 *                             unsigned long nid, void *data);
 *
 * Where "nid" specifies the memory node, "data" is the pointer to the
 * returned abstract distance (that is, "int *adist").  If the
 * algorithm provides the result, NOTIFY_STOP should be returned.
 * Otherwise, return_value & %NOTIFY_STOP_MASK == 0 to allow the next
 * algorithm in the chain to provide the result.
 */
/* 原注释译注：注册距离算法 notifier；高优先级算法给出结果时返回 NOTIFY_STOP，否则继续链。 */
int register_mt_adistance_algorithm(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&mt_adistance_algorithms, nb);
}
EXPORT_SYMBOL_GPL(register_mt_adistance_algorithm);

/**
 * unregister_mt_adistance_algorithm() - Unregister memory tiering abstract distance algorithm
 * @nb: the notifier block which describe the algorithm
 *
 * Return: 0 on success, errno on error.
 */
/* 原注释译注：从 blocking notifier 链注销算法；调用者须保证不再依赖其回调。 */
int unregister_mt_adistance_algorithm(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&mt_adistance_algorithms, nb);
}
EXPORT_SYMBOL_GPL(unregister_mt_adistance_algorithm);

/**
 * mt_calc_adistance() - Calculate abstract distance with registered algorithms
 * @node: the node to calculate abstract distance for
 * @adist: the returned abstract distance
 *
 * Return: if return_value & %NOTIFY_STOP_MASK != 0, then some
 * abstract distance algorithm provides the result, and return it via
 * @adist.  Otherwise, no algorithm can provide the result and @adist
 * will be kept as it is.
 */
/* 原注释译注：调用算法链计算 node 距离；未停止时保留调用者提供的 adist 默认值。 */
int mt_calc_adistance(int node, int *adist)
{
	return blocking_notifier_call_chain(&mt_adistance_algorithms, node, adist);
}
EXPORT_SYMBOL_GPL(mt_calc_adistance);

/* 热插拔回调在首个 memory 加入/最后 memory 移除时更新 node tier 并重建 demotion 图。 */
static int __meminit memtier_hotplug_callback(struct notifier_block *self,
					      unsigned long action, void *_arg)
{
	struct memory_tier *memtier;
	struct node_notify *nn = _arg;

	/* 两个 case 都在同一 mutex 内把 node/type/tier 关系和目标图作为一个写侧事务更新。 */
	switch (action) {
	case NODE_REMOVED_LAST_MEMORY:
		mutex_lock(&memory_tier_lock);
		if (clear_node_memory_tier(nn->nid))
			establish_demotion_targets();
		mutex_unlock(&memory_tier_lock);
		break;
	case NODE_ADDED_FIRST_MEMORY:
		mutex_lock(&memory_tier_lock);
		memtier = set_node_memory_tier(nn->nid);
		if (!IS_ERR(memtier))
			establish_demotion_targets();
		mutex_unlock(&memory_tier_lock);
		break;
	}

	return notifier_from_errno(0);
}

/* 子系统初始化：注册虚拟 bus、分配默认 DRAM type/可选 demotion 数组，并挂接热插拔 notifier。 */
static int __init memory_tier_init(void)
{
	int ret;

	/* bus 注册失败和默认 type 分配失败均是启动期不可恢复错误。 */
	ret = subsys_virtual_register(&memory_tier_subsys, NULL);
	if (ret)
		panic("%s() failed to register memory tier subsystem\n", __func__);

#ifdef CONFIG_NUMA_MIGRATION
	node_demotion = kzalloc_objs(struct demotion_nodes, nr_node_ids);
	WARN_ON(!node_demotion);
#endif

	mutex_lock(&memory_tier_lock);
	/*
	 * For now we can have 4 faster memory tiers with smaller adistance
	 * than default DRAM tier.
	 */
	/* 原注释译注：目前允许四个比默认 DRAM 更快且 adistance 更小的 memory tier。 */
	default_dram_type = mt_find_alloc_memory_type(MEMTIER_ADISTANCE_DRAM,
						      &default_memory_types);
	mutex_unlock(&memory_tier_lock);
	if (IS_ERR(default_dram_type))
		panic("%s() failed to allocate default DRAM tier\n", __func__);

	/* Record nodes with memory and CPU to set default DRAM performance. */
	nodes_and(default_dram_nodes, node_states[N_MEMORY],
		  node_states[N_CPU]);

	hotplug_node_notifier(memtier_hotplug_callback, MEMTIER_HOTPLUG_PRI);
	return 0;
}
subsys_initcall(memory_tier_init);

/* sysfs 可切换的全局 demotion 策略位；kswapd 读取它决定是否尝试跨 tier 回收。 */
bool numa_demotion_enabled = false;

#ifdef CONFIG_NUMA_MIGRATION
#ifdef CONFIG_SYSFS
/* 读取 sysfs 开关为 true/false 文本；无所有权或状态变更。 */
static ssize_t demotion_enabled_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", str_true_false(numa_demotion_enabled));
}

/* 解析 sysfs bool；从关闭变开启时清 kswapd hopeless 统计以适配新的回收策略。 */
static ssize_t demotion_enabled_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	ssize_t ret;
	bool before = numa_demotion_enabled;

	ret = kstrtobool(buf, &numa_demotion_enabled);
	if (ret)
		return ret;

	/*
	 * Reset kswapd_failures statistics. They may no longer be
	 * valid since the policy for kswapd has changed.
	 */
	/* 原注释译注：策略改变后旧 kswapd_failures 统计可能失效，开启时需重置。 */
	if (before == false && numa_demotion_enabled == true) {
		struct pglist_data *pgdat;

		for_each_online_pgdat(pgdat)
			kswapd_clear_hopeless(pgdat, KSWAPD_CLEAR_HOPELESS_OTHER);
	}

	return count;
}

static struct kobj_attribute numa_demotion_enabled_attr =
	__ATTR_RW(demotion_enabled);

static struct attribute *numa_attrs[] = {
	&numa_demotion_enabled_attr.attr,
	NULL,
};

static const struct attribute_group numa_attr_group = {
	.attrs = numa_attrs,
};

static int __init numa_init_sysfs(void)
{
	int err;
	struct kobject *numa_kobj;

	numa_kobj = kobject_create_and_add("numa", mm_kobj);
	if (!numa_kobj) {
		pr_err("failed to create numa kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(numa_kobj, &numa_attr_group);
	if (err) {
		pr_err("failed to register numa group\n");
		goto delete_obj;
	}
	return 0;

delete_obj:
	kobject_put(numa_kobj);
	return err;
}
subsys_initcall(numa_init_sysfs);
#endif /* CONFIG_SYSFS */
#endif
