// SPDX-License-Identifier: GPL-2.0
/*
 * linux/mm/mmzone.c
 *
 * management codes for pgdats, zones and page flags
 */
/*
 * 本文件集中实现 pgdat、zone 与页标志相关的公共管理代码：前半部分为
 * 全局 node/zone 遍历器和 zonelist 过滤器，后半部分初始化 LRU 容器并在
 * 特定 NUMA 配置下原子更新 folio 的 last_cpupid。它们是分配、回收、
 * vmstat 与 NUMA balancing 共用的底层机制，本身不分配或释放物理页。
 */


#include <linux/stddef.h>
#include <linux/mm.h>
#include <linux/mmzone.h>

/*
 * first_online_pgdat() - 返回在线节点序列中的第一个 pgdat。
 *
 * 业务背景：for_each_online_pgdat() 和 for_each_zone() 需要一个统一起点，
 * 本函数把 online node 位图中的首个 nid 映射为节点内存管理对象。
 * 入参：无。
 * 出参/返回：借用的 pgdat 指针；不增加引用、不转移 ownership。在线节点
 * 集合至少包含启动节点，调用者随后通常交给 next_online_pgdat() 继续扫描。
 * 注意事项：调用者须处在节点 online 状态稳定的上下文；函数不睡眠，也不
 * 锁住热插拔状态，返回的裸指针只能在外层热插拔同步契约内使用。
 */
struct pglist_data *first_online_pgdat(void)
{
	/* first_online_node 给出位图中的首个 nid，NODE_DATA 完成 nid 到 pgdat 的映射。 */
	return NODE_DATA(first_online_node);
}

/*
 * next_online_pgdat() - 沿在线节点位图取得当前 pgdat 的后继。
 *
 * 业务背景：它是 node/zone 全局迭代的推进步骤；先从 @pgdat 的 node_id
 * 查下一个在线 nid，再把有效 nid 转为 pgdat。
 * 入参：@pgdat 是当前在线节点的借用指针，非 NULL；函数只读 node_id，
 * 不取得引用、不修改对象。
 * 出参/返回：存在后继时返回借用的 pgdat；到达位图哨兵 MAX_NUMNODES 时
 * 返回 NULL，通知 for_each_online_pgdat() 终止。
 * 注意事项：不睡眠、不自行持热插拔锁；online 位图与 pgdat 生命周期必须
 * 由调用者所在的启动期或内存热插拔同步范围稳定。
 */
struct pglist_data *next_online_pgdat(struct pglist_data *pgdat)
{
	/* nid 仅在本次查找阶段有效；MAX_NUMNODES 不是可解引用节点。 */
	int nid = next_online_node(pgdat->node_id);

	/* 先拦截终止哨兵，避免把 MAX_NUMNODES 传给 NODE_DATA 越界索引。 */
	if (nid == MAX_NUMNODES)
		return NULL;
	return NODE_DATA(nid);
}

/*
 * next_zone - helper magic for for_each_zone()
 */
/*
 * 上述英文说明意为：next_zone() 是 for_each_zone() 的迭代辅助器。这里的
 * “magic”是先利用 node_zones 数组的连续布局，再在数组末尾跨到下一在线
 * pgdat；因此宏调用者无需显式维护 node 与 zone 两层循环。
 */
/*
 * next_zone() - 返回全局 zone 遍历序列中当前 zone 的后继。
 *
 * 业务背景：for_each_zone() 要依次覆盖每个在线 pgdat 内的全部 zone 类型，
 * 本函数完成“节点内推进，节点末尾跨节点”的状态转换。
 * 入参：@zone 是当前 pgdat->node_zones 数组内元素的借用指针，非 NULL；
 * 不取得 zone 或 pgdat 引用。
 * 出参/返回：同节点尚有槽位时返回下一元素；否则返回下一在线节点的首个
 * zone；所有节点耗尽时返回 NULL。空 zone 槽位也会出现，由上层决定是否跳过。
 * 注意事项：不睡眠、不锁定内存热插拔；指针比较只对同一 node_zones 数组
 * 有效，调用者不得传入数组外对象，并须稳定 online node/pgdat 生命周期。
 */
struct zone *next_zone(struct zone *zone)
{
	/* zone_pgdat 是 zone 指回所属节点的稳定反向链接，不增加 pgdat 引用。 */
	pg_data_t *pgdat = zone->zone_pgdat;

	/* 快路径：尚未到固定长度 node_zones 数组末端，只推进一个 zone 类型。 */
	if (zone < pgdat->node_zones + MAX_NR_ZONES - 1)
		zone++;
	else {
		/* 慢路径：跨节点；下一在线节点不存在时把 NULL 作为整个迭代终点。 */
		pgdat = next_online_pgdat(pgdat);
		if (pgdat)
			zone = pgdat->node_zones;
		else
			zone = NULL;
	}
	return zone;
}

/*
 * zref_in_nodemask() - 判断 zoneref 所属节点是否允许参与本次分配。
 *
 * 业务背景：__next_zones_zonelist() 用它把 NUMA 策略 nodemask 应用到预先
 * 排序的 zonelist；非 NUMA 内核只有本地节点，因此保留恒真的零开销桩。
 * 入参：@zref 是 zonelist 中当前借用条目，非 NULL；@nodes 是调用者借用的
 * 输入位图，非 NULL，函数不保存也不修改二者。
 * 出参/返回：CONFIG_NUMA 下返回该条目的 nid 是否置位；否则恒为 1。
 * 注意事项：不睡眠、无 ownership 变化；调用者负责保证 zonelist 与位图在
 * 读取期间稳定，并负责在调用本函数前处理 NULL nodemask。
 */
static inline int zref_in_nodemask(struct zoneref *zref, nodemask_t *nodes)
{
#ifdef CONFIG_NUMA
	/* zonelist_node_idx() 提取条目对应 nid，再测试策略位图。 */
	return node_isset(zonelist_node_idx(zref), *nodes);
#else
	/* UMA 配置没有跨节点过滤语义，恒真使同一扫描器无需条件编译。 */
	return 1;
#endif /* CONFIG_NUMA */
}

/* Returns the next zone at or below highest_zoneidx in a zonelist */
/*
 * 返回 zonelist 中 zone 类型不高于 highest_zoneidx 的下一个条目；若提供
 * nodemask，还必须属于允许节点。返回的是 zoneref 游标而不是 zone 引用，
 * 末尾哨兵条目的 zone 为 NULL，供分配器终止扫描。
 */
/*
 * __next_zones_zonelist() - 将游标推进到满足 zone 类型与 NUMA 策略的条目。
 *
 * 业务背景：伙伴分配器已按回退优先级建立 zonelist，本函数是
 * next_zones_zonelist()/for_each_zone_zonelist_nodemask() 的核心过滤循环。
 * 入参：@z 为输入输出式借用游标，必须指向带终止哨兵的 zonelist；
 * @highest_zoneidx 是允许的最高 zone_type；@nodes 为可空的只读 nodemask，
 * NULL 表示不按节点过滤。三者均不发生 ownership 转移。
 * 出参/返回：返回首个 zone 类型合规且节点合规的 zoneref，或返回 zone==NULL
 * 的尾哨兵；不取得 zone 引用，也不报告 errno。
 * 注意事项：循环不睡眠且不加锁；调用者须保证 zonelist/位图稳定。哨兵的
 * zone 索引必须能终止类型检查，带 nodemask 路径还显式允许 NULL zone 汇合。
 */
struct zoneref *__next_zones_zonelist(struct zoneref *z,
					enum zone_type highest_zoneidx,
					nodemask_t *nodes)
{
	/*
	 * Find the next suitable zone to use for the allocation.
	 * Only filter based on nodemask if it's set
	 */
	/*
	 * 为本次分配寻找下一个合适 zone；只有 @nodes 已设置时才应用节点过滤。
	 * 类型上限始终生效，避免例如 GFP 限制在 ZONE_NORMAL 时误落入更高 zone。
	 */
	/* NULL nodemask 是常见快路径，只需跳过 zone 类型过高的条目。 */
	if (unlikely(nodes == NULL))
		while (zonelist_zone_idx(z) > highest_zoneidx)
			z++;
	else
		/* 同时过滤类型和 nid；尾哨兵的 NULL zone 不再调用 nodemask helper。 */
		while (zonelist_zone_idx(z) > highest_zoneidx ||
				(zonelist_zone(z) && !zref_in_nodemask(z, nodes)))
			z++;

	return z;
}

/*
 * lruvec_init() - 初始化一个 node 或 memcg-node 维度的 LRU 容器。
 *
 * 业务背景：pgdat_init_internals() 初始化根 lruvec，memcg 分配路径初始化
 * 子 lruvec；回收器随后依赖这里建立的锁、各 LRU 链表和多代 LRU 状态。
 * 入参：@lruvec 是调用者拥有、尚未发布的可写对象，非 NULL；函数借用它并
 * 原地初始化，不接管存储 ownership。
 * 出参/返回：无直接返回值；对象内容先清零，lru_lock、zswap 状态、传统 LRU
 * 表头和 lru_gen 状态变为可用，不分配资源、没有失败返回。
 * 注意事项：初始化期间不得有并发读写，函数不要求外部锁且不睡眠；调用者
 * 必须在发布 lruvec 前补齐 pgdat/memcg 反向链接等外围字段。
 */
void lruvec_init(struct lruvec *lruvec)
{
	/* lru 是 for_each_lru() 的枚举游标，逐项建立 list_head 哨兵。 */
	enum lru_list lru;

	/* 阶段 1：清除旧状态并建立保护传统 LRU/统计更新的自旋锁与 zswap 状态。 */
	memset(lruvec, 0, sizeof(struct lruvec));
	spin_lock_init(&lruvec->lru_lock);
	zswap_lruvec_state_init(lruvec);

	/* 阶段 2：每种传统 LRU 都先初始化为空的循环双链表。 */
	for_each_lru(lru)
		INIT_LIST_HEAD(&lruvec->lists[lru]);
	/*
	 * The "Unevictable LRU" is imaginary: though its size is maintained,
	 * it is never scanned, and unevictable pages are not threaded on it
	 * (so that their lru fields can be reused to hold mlock_count).
	 * Poison its list head, so that any operations on it would crash.
	 */
	/*
	 * “Unevictable LRU”是虚构队列：内核维护它的规模，却从不扫描，也不把
	 * unevictable 页串入该链表，因为页内 lru 字段要复用为 mlock_count。
	 * 因而主动毒化其表头，让任何误操作立即崩溃，而不是静默破坏页状态。
	 */
	list_del(&lruvec->lists[LRU_UNEVICTABLE]);

	/* 阶段 3：在传统列表骨架之上初始化 multigenerational LRU 的代际状态。 */
	lru_gen_init_lruvec(lruvec);
}

#if defined(CONFIG_NUMA_BALANCING) && !defined(LAST_CPUPID_NOT_IN_PAGE_FLAGS)
/*
 * folio_xchg_last_cpupid() - 原子替换 folio flags 中编码的最后访问 CPUPID。
 *
 * 业务背景：NUMA balancing 用“上次访问 CPU+PID”判断页是否应迁移；当位宽
 * 足够把字段嵌入 flags 时，本函数须在不破坏同一字中其他 page flags 的前提
 * 下交换该子字段。调度 fault 路径更新它，迁移路径则搬运或清空它。
 * 入参：@folio 是仍有有效生命周期的借用 folio，非 NULL；@cpupid 是待写入
 * 的编码值，超出 LAST_CPUPID_MASK 的高位被截断，-1 形成全 1 哨兵。
 * 出参/返回：返回替换前的掩码内 CPUPID；folio flags 的目标位被原子更新，
 * 不改变 folio 引用、锁状态或其他 flag 位。
 * 注意事项：可在并发 flag 更新中调用，不睡眠；cmpxchg 循环是必要的，因为
 * 普通读改写会丢失其他 CPU 对同一 flags 字的更新。仅在指定配置组合下编译。
 */
int folio_xchg_last_cpupid(struct folio *folio, int cpupid)
{
	/* old_flags 是 cmpxchg 的期望值，flags 是保留其他位后的候选新值。 */
	unsigned long old_flags, flags;
	/* last_cpupid 保存本轮快照中的旧字段，并最终返回给调用者。 */
	int last_cpupid;

	/* READ_ONCE 建立首次一致宽度快照；失败的 try_cmpxchg 会回填最新旧值。 */
	old_flags = READ_ONCE(folio->flags.f);
	do {
		/* 每轮从最新快照重新提取旧值并仅替换 LAST_CPUPID 位域。 */
		flags = old_flags;
		last_cpupid = (flags >> LAST_CPUPID_PGSHIFT) & LAST_CPUPID_MASK;

		flags &= ~(LAST_CPUPID_MASK << LAST_CPUPID_PGSHIFT);
		flags |= (cpupid & LAST_CPUPID_MASK) << LAST_CPUPID_PGSHIFT;
	} while (unlikely(!try_cmpxchg(&folio->flags.f, &old_flags, flags)));

	/* 成功轮次的 last_cpupid 与实际被替换的旧 flags 快照严格对应。 */
	return last_cpupid;
}
#endif
