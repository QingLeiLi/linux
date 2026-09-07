// SPDX-License-Identifier: GPL-2.0
/*
 * Workingset detection
 *
 * Copyright (C) 2013 Red Hat, Inc., Johannes Weiner
 */

/* memcg/LRU 提供归属与年龄，pagemap/swap/XArray 提供 shadow 槽，shrinker 回收元数据。 */
#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include <linux/writeback.h>
#include <linux/shmem_fs.h>
#include <linux/pagemap.h>
#include <linux/atomic.h>
#include <linux/module.h>
/* swap/DAX/fs/mm 头补齐匿名 shadow 编码、特殊映射与 page-cache 核心接口。 */
#include <linux/swap.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include "swap_table.h"
#include "internal.h"

/*
 *		Double CLOCK lists
 *
 * Per node, two clock lists are maintained for file pages: the
 * inactive and the active list.  Freshly faulted pages start out at
 * the head of the inactive list and page reclaim scans pages from the
 * tail.  Pages that are accessed multiple times on the inactive list
 * are promoted to the active list, to protect them from reclaim,
 * whereas active pages are demoted to the inactive list when the
 * active list grows too big.
 *
 *   fault ------------------------+
 *                                 |
 *              +--------------+   |            +-------------+
 *   reclaim <- |   inactive   | <-+-- demotion |    active   | <--+
 *              +--------------+                +-------------+    |
 *                     |                                           |
 *                     +-------------- promotion ------------------+
 *
 *
 *		Access frequency and refault distance
 *
 * A workload is thrashing when its pages are frequently used but they
 * are evicted from the inactive list every time before another access
 * would have promoted them to the active list.
 *
 * In cases where the average access distance between thrashing pages
 * is bigger than the size of memory there is nothing that can be
 * done - the thrashing set could never fit into memory under any
 * circumstance.
 *
 * However, the average access distance could be bigger than the
 * inactive list, yet smaller than the size of memory.  In this case,
 * the set could fit into memory if it weren't for the currently
 * active pages - which may be used more, hopefully less frequently:
 *
 *      +-memory available to cache-+
 *      |                           |
 *      +-inactive------+-active----+
 *  a b | c d e f g h i | J K L M N |
 *      +---------------+-----------+
 *
 * It is prohibitively expensive to accurately track access frequency
 * of pages.  But a reasonable approximation can be made to measure
 * thrashing on the inactive list, after which refaulting pages can be
 * activated optimistically to compete with the existing active pages.
 *
 * Approximating inactive page access frequency - Observations:
 *
 * 1. When a page is accessed for the first time, it is added to the
 *    head of the inactive list, slides every existing inactive page
 *    towards the tail by one slot, and pushes the current tail page
 *    out of memory.
 *
 * 2. When a page is accessed for the second time, it is promoted to
 *    the active list, shrinking the inactive list by one slot.  This
 *    also slides all inactive pages that were faulted into the cache
 *    more recently than the activated page towards the tail of the
 *    inactive list.
 *
 * Thus:
 *
 * 1. The sum of evictions and activations between any two points in
 *    time indicate the minimum number of inactive pages accessed in
 *    between.
 *
 * 2. Moving one inactive page N page slots towards the tail of the
 *    list requires at least N inactive page accesses.
 *
 * Combining these:
 *
 * 1. When a page is finally evicted from memory, the number of
 *    inactive pages accessed while the page was in cache is at least
 *    the number of page slots on the inactive list.
 *
 * 2. In addition, measuring the sum of evictions and activations (E)
 *    at the time of a page's eviction, and comparing it to another
 *    reading (R) at the time the page faults back into memory tells
 *    the minimum number of accesses while the page was not cached.
 *    This is called the refault distance.
 *
 * Because the first access of the page was the fault and the second
 * access the refault, we combine the in-cache distance with the
 * out-of-cache distance to get the complete minimum access distance
 * of this page:
 *
 *      NR_inactive + (R - E)
 *
 * And knowing the minimum access distance of a page, we can easily
 * tell if the page would be able to stay in cache assuming all page
 * slots in the cache were available:
 *
 *   NR_inactive + (R - E) <= NR_inactive + NR_active
 *
 * If we have swap we should consider about NR_inactive_anon and
 * NR_active_anon, so for page cache and anonymous respectively:
 *
 *   NR_inactive_file + (R - E) <= NR_inactive_file + NR_active_file
 *   + NR_inactive_anon + NR_active_anon
 *
 *   NR_inactive_anon + (R - E) <= NR_inactive_anon + NR_active_anon
 *   + NR_inactive_file + NR_active_file
 *
 * Which can be further simplified to:
 *
 *   (R - E) <= NR_active_file + NR_inactive_anon + NR_active_anon
 *
 *   (R - E) <= NR_active_anon + NR_inactive_file + NR_active_file
 *
 * Put into words, the refault distance (out-of-cache) can be seen as
 * a deficit in inactive list space (in-cache).  If the inactive list
 * had (R - E) more page slots, the page would not have been evicted
 * in between accesses, but activated instead.  And on a full system,
 * the only thing eating into inactive list space is active pages.
 *
 *
 *		Refaulting inactive pages
 *
 * All that is known about the active list is that the pages have been
 * accessed more than once in the past.  This means that at any given
 * time there is actually a good chance that pages on the active list
 * are no longer in active use.
 *
 * So when a refault distance of (R - E) is observed and there are at
 * least (R - E) pages in the userspace workingset, the refaulting page
 * is activated optimistically in the hope that (R - E) pages are actually
 * used less frequently than the refaulting page - or even not used at
 * all anymore.
 *
 * That means if inactive cache is refaulting with a suitable refault
 * distance, we assume the cache workingset is transitioning and put
 * pressure on the current workingset.
 *
 * If this is wrong and demotion kicks in, the pages which are truly
 * used more frequently will be reactivated while the less frequently
 * used once will be evicted from memory.
 *
 * But if this is right, the stale pages will be pushed out of memory
 * and the used pages get to stay in cache.
 *
 *		Refaulting active pages
 *
 * If on the other hand the refaulting pages have recently been
 * deactivated, it means that the active list is no longer protecting
 * actively used cache from reclaim. The cache is NOT transitioning to
 * a different workingset; the existing workingset is thrashing in the
 * space allocated to the page cache.
 *
 *
 *		Implementation
 *
 * For each node's LRU lists, a counter for inactive evictions and
 * activations is maintained (node->nonresident_age).
 *
 * On eviction, a snapshot of this counter (along with some bits to
 * identify the node) is stored in the now empty page cache
 * slot of the evicted page.  This is called a shadow entry.
 *
 * On cache misses for which there are shadow entries, an eligible
 * refault distance will immediately activate the refaulting page.
 */
/*
 * 译注：双 CLOCK 链表与 refault distance
 *
 * 每个 NUMA 节点为文件页维护 inactive/active 两条时钟链。新 fault 的页进入
 * inactive 头部，回收从尾部扫描；inactive 页再次访问会提升到 active，active
 * 过大时又降级。抖动意味着频繁使用的页尚未等到第二次访问就从 inactive 淘汰。
 * 若平均访问距离大于全部可用内存，工作集无论如何都装不下；若它只大于 inactive
 * 而小于总内存，则问题在于现有 active 页挤占空间，其中一些可能已不再常用。
 *
 * 精确记录每页访问频率成本过高，因此用 inactive 上的最小访问距离近似。第一次
 * 访问把新页插到 inactive 头部并把其余页推向尾部；第二次访问把页提升，同时缩小
 * inactive 并推动更新的页。故两个时点之间 eviction+activation 的增量，是期间
 * 至少发生的 inactive 访问数；页在链上向尾部移动 N 槽至少需要 N 次访问。
 *
 * 页被逐出时，它在内存中的最小距离至少为 NR_inactive；逐出时记 E，重新 fault
 * 时读 R，则离线期间距离为 R-E，总最小距离是 NR_inactive+(R-E)。若该值不超过
 * inactive+active，页在拥有整个缓存时本可驻留。存在 swap 时文件页和匿名页会
 * 竞争同一可回收容量，化简后文件 refault 比较 R-E 与 active_file 加 anon 总量，
 * 匿名 refault 则比较 R-E 与 active_anon 加 file 总量。
 *
 * 换言之，R-E 是 inactive 空间缺口；满内存时正是 active 页吃掉了这些槽。active
 * 只证明过去访问超过一次，未保证现在仍热。若 refault distance 不大于用户工作集，
 * 就乐观激活 refault 页，让它与旧 active 页竞争：判断错误时真正热页会再激活、冷页
 * 会被淘汰；判断正确时旧页退出而新工作集留下。若 refault 页逐出前已是 active，
 * 则说明 active 保护也失效，是现有工作集在所获缓存空间内抖动，而非工作集切换。
 *
 * 实现上，每个 lruvec 的 nonresident_age 统计 inactive eviction 与 activation。
 * eviction 把计数快照、节点和 memcg 身份编码进被清空 page-cache/swap 槽的 shadow
 * entry；cache miss 遇到 shadow 后计算距离，满足阈值便立即激活新 folio。
 */

/* 最低位保存“逐出前属于工作集”；其上依次编码节点、memcg 和截断时间戳。 */
#define WORKINGSET_SHIFT 1
#define EVICTION_SHIFT	((BITS_PER_LONG - BITS_PER_XA_VALUE) +	\
			 WORKINGSET_SHIFT + NODES_SHIFT + \
			 MEM_CGROUP_ID_SHIFT)
#define EVICTION_SHIFT_ANON	(EVICTION_SHIFT + SWAP_COUNT_SHIFT)
#define EVICTION_MASK	(~0UL >> EVICTION_SHIFT)
#define EVICTION_MASK_ANON	(~0UL >> EVICTION_SHIFT_ANON)

/*
 * Eviction timestamps need to be able to cover the full range of
 * actionable refaults. However, bits are tight in the xarray
 * entry, and after storing the identifier for the lruvec there might
 * not be enough left to represent every single actionable refault. In
 * that case, we have to sacrifice granularity for distance, and group
 * evictions into coarser buckets by shaving off lower timestamp bits.
 */
/*
 * 译注：逐出时间戳必须覆盖所有仍有决策价值的 refault 距离，但 XArray value 在
 * 保存 lruvec 身份后位数有限；位数不足时舍弃时间戳低位，把逐出事件分入更粗的桶，
 * 以牺牲精度换取可表示范围。FILE/ANON 因编码空间不同分别保存右移阶数。
 */
static unsigned int bucket_order[ANON_AND_FILE] __read_mostly;

/*
 * pack_shadow() - 把一次逐出快照编码为 XArray value entry。
 * 业务背景：folio 离开 page cache/swap 后不能保留对象，只在原槽保存足够的 refault
 * 决策信息；workingset_eviction()/MGLRU eviction 在删除 folio 时调用。
 * 入参：memcgid 是私有 cgroup id；pgdat 是借用节点描述；eviction 是已按实现准备的
 * 时间戳/token；workingset 表示逐出前是否活跃；file 区分文件与匿名编码宽度。
 * 出参/返回：返回 xa_mk_value() 标记的立即数指针，无分配和引用转移。
 * 注意事项：先按类型 mask 截断高位，再按固定顺序左移拼接；截断允许后续用模运算
 * 处理回绕，函数不睡眠。
 */
static void *pack_shadow(int memcgid, pg_data_t *pgdat, unsigned long eviction,
			 bool workingset, bool file)
{
	/* 从高层语义到低位标签逐项拼接，最终最低位留给 XArray value 标记编码。 */
	eviction &= file ? EVICTION_MASK : EVICTION_MASK_ANON;
	eviction = (eviction << MEM_CGROUP_ID_SHIFT) | memcgid;
	eviction = (eviction << NODES_SHIFT) | pgdat->node_id;
	eviction = (eviction << WORKINGSET_SHIFT) | workingset;

	return xa_mk_value(eviction);
}

/*
 * unpack_shadow() - 从 shadow value 恢复 lruvec 身份、时间戳和工作集位。
 * 业务背景：refault 路径必须在原逐出 memcg/节点上下文评价距离，与 pack_shadow()
 * 构成严格逆变换。
 * 入参：shadow 是有效 XArray value；四个输出指针均必非 NULL，分别接收 memcg id、
 * 借用 pgdat、截断 eviction/token 与 workingset 标志。
 * 出参/返回：无直接返回值；填充全部输出，不取得 memcg 或节点引用。
 * 注意事项：调用者必须在需要时用 RCU/tryget 稳定由 id 找到的 memcg；节点 id 假定
 * 来自仍有效的已编码内核节点，函数不会睡眠。
 */
static void unpack_shadow(void *shadow, int *memcgidp, pg_data_t **pgdat,
			  unsigned long *evictionp, bool *workingsetp)
{
	unsigned long entry = xa_to_value(shadow);
	int memcgid, nid;
	bool workingset;

	/* 按 pack 的反序从低位逐段取出，每次右移后再解释下一字段。 */
	workingset = entry & ((1UL << WORKINGSET_SHIFT) - 1);
	entry >>= WORKINGSET_SHIFT;
	nid = entry & ((1UL << NODES_SHIFT) - 1);
	entry >>= NODES_SHIFT;
	memcgid = entry & ((1UL << MEM_CGROUP_ID_SHIFT) - 1);
	entry >>= MEM_CGROUP_ID_SHIFT;

	/* 所有局部字段解码完毕后一次性发布给调用者的输出变量。 */
	*memcgidp = memcgid;
	*pgdat = NODE_DATA(nid);
	*evictionp = entry;
	*workingsetp = workingset;
}

#ifdef CONFIG_LRU_GEN

/*
 * lru_gen_eviction() - 为 MGLRU 逐出生成代际 token 并记录 tier 统计。
 * 业务背景：MGLRU 不使用传统 nonresident_age 距离，而把最老代序号和引用 tier
 * 编进 shadow，供 refault 判断是否仍在 MAX_NR_GENS 窗口。
 * 入参：folio 是已从 LRU 摘除并由逐出路径稳定的借用对象，不消费引用。
 * 出参/返回：返回包含 memcg/node/token/workingset 的 shadow value；同时按页数增加
 * lrugen->evicted[history][type][tier]。
 * 注意事项：RCU 稳定 folio memcg 及 lruvec 生命周期；READ_ONCE 只取序号快照，统计
 * 用原子量与并发老化配合；BUILD_BUG_ON 保证 token 可装入编码位数。
 */
static void *lru_gen_eviction(struct folio *folio)
{
	/*
	 * 变量地图：type 区分 anon/file；delta 是基础页数；refs/workingset 决定 tier；
	 * min_seq/token 保存逐出代与引用强度；hist 选择循环统计槽；memcg_id/pgdat 编码归属。
	 */
	int hist;
	unsigned long token;
	unsigned long min_seq;
	struct lruvec *lruvec;
	struct lru_gen_folio *lrugen;
	/* 下列派生量在 folio 独占窗口读取，用于选择编码 mask、页数和 tier。 */
	int type = folio_is_file_lru(folio);
	int delta = folio_nr_pages(folio);
	int refs = folio_lru_refs(folio);
	bool workingset = folio_test_workingset(folio);
	int tier = lru_tier_from_refs(refs, workingset);
	struct mem_cgroup *memcg;
	struct pglist_data *pgdat = folio_pgdat(folio);
	unsigned short memcg_id;

	/* 编译期验证“代际序号+引用位”不会侵占 shadow 的身份字段。 */
	BUILD_BUG_ON(LRU_GEN_WIDTH + LRU_REFS_WIDTH >
		     BITS_PER_LONG - max(EVICTION_SHIFT, EVICTION_SHIFT_ANON));

	rcu_read_lock();
	/* 在 folio 所属 memcg/node 的 lrugen 中截取当前最老代和引用 tier。 */
	memcg = folio_memcg(folio);
	lruvec = mem_cgroup_lruvec(memcg, pgdat);
	lrugen = &lruvec->lrugen;
	min_seq = READ_ONCE(lrugen->min_seq[type]);
	token = (min_seq << LRU_REFS_WIDTH) | max(refs - 1, 0);

	/* 逐出和稍后的 refault 以相同 history/type/tier 槽配对进行反馈。 */
	hist = lru_hist_from_seq(min_seq);
	atomic_long_add(delta, &lrugen->evicted[hist][type][tier]);
	memcg_id = mem_cgroup_private_id(memcg);
	rcu_read_unlock();

	return pack_shadow(memcg_id, pgdat, token, workingset, type);
}

/*
 * Tests if the shadow entry is for a folio that was recently evicted.
 * Fills in @lruvec, @token, @workingset with the values unpacked from shadow.
 */
/*
 * 译注：判断 shadow 所代表的 folio 是否刚被逐出，并解码 lruvec、token 与 workingset。
 * 业务背景：MGLRU refault 只有仍落在当前代际窗口内才参与反馈，避免陈旧 shadow
 * 污染新一轮代际统计。
 * 入参：shadow 为有效 value；lruvec/token/workingset 为必非 NULL 输出；file 指定
 * 文件或匿名 mask。输出 lruvec 为 RCU 借用指针。
 * 出参/返回：代差小于 MAX_NR_GENS 返回 true，否则 false；始终填充三个输出。
 * 注意事项：调用者必须持 RCU 读锁；memcg id 可能复用，后续还要核对新 folio 的
 * folio_lruvec() 身份，READ_ONCE 防止撕裂但不冻结 max_seq。
 */
static bool lru_gen_test_recent(void *shadow, struct lruvec **lruvec,
				unsigned long *token, bool *workingset, bool file)
{
	int memcg_id;
	unsigned long max_seq;
	struct mem_cgroup *memcg;
	struct pglist_data *pgdat;

	/* 解码身份后由私有 id 找 memcg，生命周期仅由外层 RCU 窗口支撑。 */
	unpack_shadow(shadow, &memcg_id, &pgdat, token, workingset);

	memcg = mem_cgroup_from_private_id(memcg_id);
	*lruvec = mem_cgroup_lruvec(memcg, pgdat);

	max_seq = READ_ONCE((*lruvec)->lrugen.max_seq);
	/* mask 去掉 token 的引用低位，模空间中的绝对差小于代数上限才算近期。 */
	max_seq &= (file ? EVICTION_MASK : EVICTION_MASK_ANON) >> LRU_REFS_WIDTH;

	return abs_diff(max_seq, *token >> LRU_REFS_WIDTH) < MAX_NR_GENS;
}

/*
 * lru_gen_refault() - 把 MGLRU shadow 反馈到新 folio 和代际统计。
 * 业务背景：swap/page-cache miss 建好替代 folio 后调用；近期 refault 用逐出 token
 * 恢复引用 tier 或 workingset 状态，帮助 MGLRU 调整保护强度。
 * 入参：folio 为已锁定的新替代 folio借用对象；shadow 为旧 folio 的 value entry。
 * 出参/返回：无直接返回值；可能更新 lruvec refault/activate/restore 统计以及新 folio
 * 的 workingset 或 LRU_REFS 位，不保留引用。
 * 注意事项：全程 RCU；解码 lruvec 必须与新 folio 当前 lruvec 相同才可信，陈旧或跨组
 * shadow 只被忽略；原子统计与并发代际推进允许近似快照。
 */
static void lru_gen_refault(struct folio *folio, void *shadow)
{
	/*
	 * 变量地图：token/workingset 来自 shadow；recent 是代际窗口判断；lruvec/lrugen
	 * 是 RCU 借用目标；refs/tier/hist 重建逐出时统计槽；delta 是新 folio 基础页数。
	 */
	bool recent;
	int hist, tier, refs;
	bool workingset;
	unsigned long token;
	struct lruvec *lruvec;
	struct lru_gen_folio *lrugen;
	int type = folio_is_file_lru(folio);
	int delta = folio_nr_pages(folio);

	rcu_read_lock();

	/* 先解码并验证归属；memcg id 复用或 folio 迁组会使身份不一致。 */
	recent = lru_gen_test_recent(shadow, &lruvec, &token, &workingset, type);
	if (lruvec != folio_lruvec(folio))
		goto unlock;

	mod_lruvec_state(lruvec, WORKINGSET_REFAULT_BASE + type, delta);

	/* 所有同归属 refault 都记总数，只有近期样本才进入 MGLRU tier 反馈。 */
	if (!recent)
		goto unlock;

	lrugen = &lruvec->lrugen;

	hist = lru_hist_from_seq(READ_ONCE(lrugen->min_seq[type]));
	refs = (token & (BIT(LRU_REFS_WIDTH) - 1)) + 1;
	tier = lru_tier_from_refs(refs, workingset);

	atomic_long_add(delta, &lrugen->refaulted[hist][type][tier]);

	if (workingset) {
		/*
		 * see folio_add_lru(), where folio_set_active() is
		 * called for workingset folios
		 */
		/*
		 * 译注：workingset folio 会在 folio_add_lru() 被置 active；fault 上下文
		 * 同时记 activation，再恢复 workingset 位和 restore 统计。
		 */
		if (lru_gen_in_fault())
			mod_lruvec_state(lruvec, WORKINGSET_ACTIVATE_BASE + type, delta);
		folio_set_workingset(folio);
		mod_lruvec_state(lruvec, WORKINGSET_RESTORE_BASE + type, delta);
	} else
		/* 非 workingset 样本只恢复引用 tier，留给入 LRU 时选择代际。 */
		set_mask_bits(&folio->flags.f, LRU_REFS_MASK, (refs - 1UL) << LRU_REFS_PGOFF);
unlock:
	/* 所有归属不匹配、陈旧和成功路径在此结束 RCU 借用窗口。 */
	rcu_read_unlock();
}

#else /* !CONFIG_LRU_GEN */

/*
 * lru_gen_eviction() - 关闭 CONFIG_LRU_GEN 时的不可达兼容桩。
 * 业务背景：统一调用点可保留编译形状，运行期 lru_gen_enabled() 为 false。
 * 入参：folio 为未使用借用指针。出参/返回：NULL，无副作用。
 * 注意事项：若误调用不会生成有效 shadow；不睡眠。
 */
static void *lru_gen_eviction(struct folio *folio)
{
	return NULL;
}

/*
 * lru_gen_test_recent() - 关闭 MGLRU 时的近期判断桩。
 * 业务背景：传统 LRU 走 nonresident_age，MGLRU helper 保留静态接口。
 * 入参：全部参数未使用，输出指针不写。出参/返回：恒 false，无副作用。
 * 注意事项：调用者不应在关闭配置下依赖输出内容；不睡眠。
 */
static bool lru_gen_test_recent(void *shadow, struct lruvec **lruvec,
				unsigned long *token, bool *workingset, bool file)
{
	return false;
}

/*
 * lru_gen_refault() - 关闭 MGLRU 时的 refault 空桩。
 * 业务背景：传统路径由 workingset_refault() 自行完成距离判断。
 * 入参：folio/shadow 均未使用借用值。出参/返回：无，且无任何副作用。
 * 注意事项：运行期门禁保证正常流程不会把反馈交给此桩；不睡眠。
 */
static void lru_gen_refault(struct folio *folio, void *shadow)
{
}

#endif /* CONFIG_LRU_GEN */

/**
 * workingset_age_nonresident - age non-resident entries as LRU ages
 * @lruvec: the lruvec that was aged
 * @nr_pages: the number of pages to count
 *
 * As in-memory pages are aged, non-resident pages need to be aged as
 * well, in order for the refault distances later on to be comparable
 * to the in-memory dimensions. This function allows reclaim and LRU
 * operations to drive the non-resident aging along in parallel.
 */
/*
 * 译注：内存中 LRU 前进时，非驻留条目也必须同步老化，后续 refault distance 才能
 * 与当前内存容量比较；回收和 LRU 操作通过本函数共同推进这只虚拟时钟。
 * 业务背景：传统 LRU 的 eviction/refault 以 lruvec->nonresident_age 差值近似访问距离。
 * 入参：lruvec 为叶 memcg/node 的借用向量；nr_pages 是本次老化的基础页数。
 * 出参/返回：无直接返回值；原子增加本 lruvec 及全部祖先的 nonresident_age。
 * 注意事项：parent_lruvec() 返回借用父级并终止于根；原子计数允许并发和无符号回绕，
 * 不要求持 LRU 锁且不会睡眠。
 */
void workingset_age_nonresident(struct lruvec *lruvec, unsigned long nr_pages)
{
	/*
	 * Reclaiming a cgroup means reclaiming all its children in a
	 * round-robin fashion. That means that each cgroup has an LRU
	 * order that is composed of the LRU orders of its child
	 * cgroups; and every page has an LRU position not just in the
	 * cgroup that owns it, but in all of that group's ancestors.
	 *
	 * So when the physical inactive list of a leaf cgroup ages,
	 * the virtual inactive lists of all its parents, including
	 * the root cgroup's, age as well.
	 */
	/*
	 * 译注：回收某 cgroup 会轮转处理全部子组，所以父组的虚拟 LRU 顺序由子组顺序
	 * 合成，每页同时占据所属组及祖先的逻辑位置；叶组 inactive 前进时必须把根在内
	 * 的所有祖先时钟一并推进，跨层级 refault 比较才使用同一时间尺度。
	 */
	do {
		atomic_long_add(nr_pages, &lruvec->nonresident_age);
	} while ((lruvec = parent_lruvec(lruvec)));
}

/**
 * workingset_eviction - note the eviction of a folio from memory
 * @target_memcg: the cgroup that is causing the reclaim
 * @folio: the folio being evicted
 *
 * Return: a shadow entry to be stored in @folio->mapping->i_pages in place
 * of the evicted @folio so that a later refault can be detected.
 */
/*
 * 译注：记录 folio 离开内存，并返回要替代它存入 mapping->i_pages 的 shadow entry，
 * 让以后相同索引重新 fault 时可以识别。
 * 业务背景：回收删除 page-cache/swap folio 的不可回滚边界上截取逐出时钟与归属；
 * MGLRU 和传统 LRU 分别生成代际 token 或 nonresident_age 快照。
 * 入参：folio 为已锁定、已摘 LRU、引用计数归零但内存尚由回收路径独占的借用对象；
 * target_memcg 为触发回收的借用 cgroup，可为 NULL。
 * 出参/返回：返回无需释放的 XArray value shadow；传统路径推进非驻留时钟，MGLRU
 * 路径更新逐出 tier 统计；不取得 folio/memcg 长期引用。
 * 注意事项：三个 VM_BUG_ON 验证独占前置条件；必须在 folio 内存仍可读时编码属性，
 * 函数不睡眠，shadow 中的 id/时间戳以后可能回绕或复用。
 */
void *workingset_eviction(struct folio *folio, struct mem_cgroup *target_memcg)
{
	struct pglist_data *pgdat = folio_pgdat(folio);
	int file = folio_is_file_lru(folio);
	unsigned long eviction;
	struct lruvec *lruvec;
	int memcgid;

	/* Folio is fully exclusive and pins folio's memory cgroup pointer */
	/* 译注：folio 已完全独占，因此它自身就稳定住 memcg 指针，无需额外引用或 RCU。 */
	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);
	VM_BUG_ON_FOLIO(folio_ref_count(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	if (lru_gen_enabled())
		/* MGLRU 用代际序号和引用 tier，跳过传统 nonresident_age。 */
		return lru_gen_eviction(folio);

	/* 传统路径在造成回收的 cgroup 层级记录时钟，而非只看 folio 原所属层级。 */
	lruvec = mem_cgroup_lruvec(target_memcg, pgdat);
	/* XXX: target_memcg can be NULL, go through lruvec */
	/* 译注：target_memcg 可为 NULL，必须先经 lruvec 归一到根 memcg 再提取私有 id。 */
	memcgid = mem_cgroup_private_id(lruvec_memcg(lruvec));
	eviction = atomic_long_read(&lruvec->nonresident_age);
	eviction >>= bucket_order[file];
	/* shadow 保存旧快照后再按 folio 页数推进当前及祖先时钟。 */
	workingset_age_nonresident(lruvec, folio_nr_pages(folio));
	return pack_shadow(memcgid, pgdat, eviction,
			   folio_test_workingset(folio), file);
}

/**
 * workingset_test_recent - tests if the shadow entry is for a folio that was
 * recently evicted. Also fills in @workingset with the value unpacked from
 * shadow.
 * @shadow: the shadow entry to be tested.
 * @file: whether the corresponding folio is from the file lru.
 * @workingset: where the workingset value unpacked from shadow should
 * be stored.
 * @flush: whether to flush cgroup rstat.
 *
 * Return: true if the shadow is for a recently evicted folio; false otherwise.
 */
/*
 * 译注：判断 shadow 对应 folio 是否近期逐出，并把其中 workingset 位写到输出参数。
 * 业务背景：refault 激活与 cachestat 查询共享距离判断；评价必须回到造成逐出的
 * memcg/node，而不是新 folio 当前归属。
 * 入参：shadow 为有效 value；file 指定文件/匿名编码；workingset 为必非 NULL 输出；
 * flush=true 允许在 RCU 外刷新 memcg rstat，false 供已处于 RCU 的只读调用者。
 * 出参/返回：refault distance 不大于可竞争工作集返回 true，否则 false；始终解码
 * workingset，临时取得的 eviction_memcg 引用在返回前释放。
 * 注意事项：MGLRU 走代际窗口；传统路径容忍 id/计数回绕的极少误判。flush 可能睡眠，
 * 调用者在 RCU 临界区时必须传 false。
 */
bool workingset_test_recent(void *shadow, bool file, bool *workingset,
				bool flush)
{
	/*
	 * 变量地图：eviction 是解码并恢复精度的旧时钟；refault 是当前时钟；二者差为
	 * refault_distance；workingset_size 是逐出 lruvec 当前可竞争容量；eviction_memcg
	 * 从 RCU 查找升级为持有引用，eviction_lruvec/pgdat 均借用其生命周期。
	 */
	struct mem_cgroup *eviction_memcg;
	struct lruvec *eviction_lruvec;
	unsigned long refault_distance;
	unsigned long workingset_size;
	unsigned long refault;
	int memcgid;
	struct pglist_data *pgdat;
	unsigned long eviction;

	if (lru_gen_enabled()) {
		bool recent;

		/* MGLRU 的 lruvec 是 RCU 借用对象，判断和输出填充都限制在临界区内。 */
		rcu_read_lock();
		recent = lru_gen_test_recent(shadow, &eviction_lruvec, &eviction,
					     workingset, file);
		rcu_read_unlock();
		return recent;
	}

	rcu_read_lock();
	/* 传统 shadow 的低精度时间戳先按 file/anon 桶阶数恢复到比较尺度。 */
	unpack_shadow(shadow, &memcgid, &pgdat, &eviction, workingset);
	eviction <<= bucket_order[file];

	/*
	 * Look up the memcg associated with the stored ID. It might
	 * have been deleted since the folio's eviction.
	 *
	 * Note that in rare events the ID could have been recycled
	 * for a new cgroup that refaults a shared folio. This is
	 * impossible to tell from the available data. However, this
	 * should be a rare and limited disturbance, and activations
	 * are always speculative anyway. Ultimately, it's the aging
	 * algorithm's job to shake out the minimum access frequency
	 * for the active cache.
	 *
	 * XXX: On !CONFIG_MEMCG, this will always return NULL; it
	 * would be better if the root_mem_cgroup existed in all
	 * configurations instead.
	 */
	/*
	 * 译注：按 shadow 私有 id 查逐出 memcg；它可能已删除，id 也可能极少量地被复用给
	 * 一个重新 fault 共享 folio 的新组，现有位数无法区分。激活本就是推测，错误扰动
	 * 最终会被老化算法淘汰。关闭 MEMCG 时查找总为 NULL，理想实现应始终有 root 对象。
	 * tryget 在 RCU 内把成功对象转成可跨临界区使用的持有引用。
	 */
	eviction_memcg = mem_cgroup_from_private_id(memcgid);
	if (!mem_cgroup_tryget(eviction_memcg))
		eviction_memcg = NULL;
	rcu_read_unlock();

	if (!mem_cgroup_disabled() && !eviction_memcg)
		/* 启用 memcg 却无法稳定原归属时，shadow 已不可信，拒绝近期判断。 */
		return false;
	/*
	 * Flush stats (and potentially sleep) outside the RCU read section.
	 *
	 * Note that workingset_test_recent() itself might be called in RCU read
	 * section (for e.g, in cachestat) - these callers need to skip flushing
	 * stats (via the flush argument).
	 *
	 * XXX: With per-memcg flushing and thresholding, is ratelimiting
	 * still needed here?
	 */
	/*
	 * 译注：统计刷新可能睡眠，必须在 RCU 外执行；cachestat 等外层已持 RCU 的调用者
	 * 通过 flush=false 跳过。若未来按 memcg 分别阈值刷新，是否仍需限速尚待评估。
	 */
	if (flush)
		mem_cgroup_flush_stats_ratelimited(eviction_memcg);

	eviction_lruvec = mem_cgroup_lruvec(eviction_memcg, pgdat);
	refault = atomic_long_read(&eviction_lruvec->nonresident_age);

	/*
	 * Calculate the refault distance
	 *
	 * The unsigned subtraction here gives an accurate distance
	 * across nonresident_age overflows in most cases. There is a
	 * special case: usually, shadow entries have a short lifetime
	 * and are either refaulted or reclaimed along with the inode
	 * before they get too old.  But it is not impossible for the
	 * nonresident_age to lap a shadow entry in the field, which
	 * can then result in a false small refault distance, leading
	 * to a false activation should this old entry actually
	 * refault again.  However, earlier kernels used to deactivate
	 * unconditionally with *every* reclaim invocation for the
	 * longest time, so the occasional inappropriate activation
	 * leading to pressure on the active list is not a problem.
	 */
	/*
	 * 译注：无符号减法配 mask 通常能跨 nonresident_age 回绕得到准确距离。极老 shadow
	 * 若被时钟套圈可能伪装成小距离并错误激活，但 shadow 通常很快 refault 或随 inode
	 * 回收；偶发误激活只给 active 链施压，后续老化会纠正，风险低于增加更多编码状态。
	 */
	refault_distance = ((refault - eviction) &
			    (file ? EVICTION_MASK : EVICTION_MASK_ANON));

	/*
	 * Compare the distance to the existing workingset size. We
	 * don't activate pages that couldn't stay resident even if
	 * all the memory was available to the workingset. Whether
	 * workingset competition needs to consider anon or not depends
	 * on having free swap space.
	 */
	/*
	 * 译注：只有即使获得全部可竞争内存也能驻留的页才值得激活。文件 refault 基础
	 * 比较 active_file；匿名 refault 还加入 file inactive；有可用 swap 时 anon 也参与
	 * 竞争，文件页再计 anon inactive。这里按逐出 lruvec 的当前状态构造容量快照。
	 */
	workingset_size = lruvec_page_state(eviction_lruvec, NR_ACTIVE_FILE);
	if (!file) {
		workingset_size += lruvec_page_state(eviction_lruvec,
						     NR_INACTIVE_FILE);
	}
	/* 只有存在 swap 时匿名页才可被逐出并与文件缓存交换容量。 */
	if (mem_cgroup_get_nr_swap_pages(eviction_memcg) > 0) {
		workingset_size += lruvec_page_state(eviction_lruvec,
						     NR_ACTIVE_ANON);
		if (file) {
			workingset_size += lruvec_page_state(eviction_lruvec,
						     NR_INACTIVE_ANON);
		}
	}

	/* 距离和容量快照完成，归还跨 RCU 持有的 memcg 引用后发布布尔结论。 */
	mem_cgroup_put(eviction_memcg);
	return refault_distance <= workingset_size;
}

/**
 * workingset_refault - Evaluate the refault of a previously evicted folio.
 * @folio: The freshly allocated replacement folio.
 * @shadow: Shadow entry of the evicted folio.
 *
 * Calculates and evaluates the refault distance of the previously
 * evicted folio in the context of the node and the memcg whose memory
 * pressure caused the eviction.
 */
/*
 * 译注：在造成旧 folio 逐出的节点和 memcg 上下文中，计算并评价新 folio 的 refault。
 * 业务背景：swap/page-cache miss 用 shadow 替换出新 folio后调用；近期 refault 会被
 * 乐观激活，逐出前已 active 的页还恢复 workingset 身份。
 * 入参：folio 是新分配、已锁定且由 fault 路径持有的借用对象；shadow 是已从 XArray
 * 取出的旧 value，不需释放。
 * 出参/返回：无直接返回值；总是记当前拥有者的 refault，近期时设置 active、推进时钟
 * 并记 activate；旧 workingset 还设置标志、反馈回收成本并记 restore。
 * 注意事项：评价层级取自 shadow 而计费层级取新 folio；持有 folio 锁稳定其 memcg，
 * get_mem_cgroup_from_folio() 引用在 out 统一释放，统计刷新路径可能睡眠。
 */
void workingset_refault(struct folio *folio, void *shadow)
{
	bool file = folio_is_file_lru(folio);
	struct mem_cgroup *memcg;
	struct lruvec *lruvec;
	bool workingset;
	long nr;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	if (lru_gen_enabled()) {
		/* MGLRU 自行恢复 tier/workingset 并记代际反馈，不进入传统 active 路径。 */
		lru_gen_refault(folio, shadow);
		return;
	}

	/*
	 * The activation decision for this folio is made at the level
	 * where the eviction occurred, as that is where the LRU order
	 * during folio reclaim is being determined.
	 *
	 * However, the cgroup that will own the folio is the one that
	 * is actually experiencing the refault event. Make sure the folio is
	 * locked to guarantee folio_memcg() stability throughout.
	 */
	/*
	 * 译注：是否激活在“当初发生回收”的层级决定，因为该层级定义逐出时的 LRU 顺序；
	 * 但 refault 事件计入新 folio 的实际拥有组。folio 锁保证全过程 folio_memcg 稳定。
	 */
	nr = folio_nr_pages(folio);
	memcg = get_mem_cgroup_from_folio(folio);
	lruvec = mem_cgroup_lruvec(memcg, folio_pgdat(folio));
	mod_lruvec_state(lruvec, WORKINGSET_REFAULT_BASE + file, nr);

	/* 陈旧或失去 memcg 身份的 shadow 只保留 refault 总数，不激活新 folio。 */
	if (!workingset_test_recent(shadow, file, &workingset, true))
		goto out;

	folio_set_active(folio);
	/* 乐观激活相当于一次 inactive 访问，推进拥有组及祖先的非驻留时钟。 */
	workingset_age_nonresident(lruvec, nr);
	mod_lruvec_state(lruvec, WORKINGSET_ACTIVATE_BASE + file, nr);

	/* Folio was active prior to eviction */
	/* 译注：shadow 的 workingset 位说明旧 folio 在逐出前已属于受保护工作集。 */
	if (workingset) {
		folio_set_workingset(folio);
		/*
		 * XXX: Move to folio_add_lru() when it supports new vs
		 * putback
		 */
		/* 译注：待 folio_add_lru() 能区分新入链与 putback 后，可把该成本反馈移到那里。 */
		lru_note_cost_refault(folio);
		mod_lruvec_state(lruvec, WORKINGSET_RESTORE_BASE + file, nr);
	}
out:
	/* 正常、陈旧 shadow 与失败身份路径都在此归还新 folio memcg 的持有引用。 */
	mem_cgroup_put(memcg);
}

/**
 * workingset_activation - note a page activation
 * @folio: Folio that is being activated.
 */
/*
 * 译注：记录一个 folio 被激活。
 * 业务背景：传统 LRU 的 activation 与 eviction 共同推进 nonresident_age，使 shadow
 * refault distance 反映真实 inactive 访问；由 mark_page_accessed 等激活路径调用。
 * 入参：folio 为调用者稳定的借用对象，不转移引用。
 * 出参/返回：无直接返回值；对可归属页面按 folio 页数推进其 lruvec 及祖先时钟。
 * 注意事项：未计费的非 memcg 特殊页在启用 memcg 时跳过；RCU 仅稳定 lruvec 生命周期，
 * 原子年龄更新不冻结 LRU 内容且不睡眠。
 */
void workingset_activation(struct folio *folio)
{
	/*
	 * Filter non-memcg pages here, e.g. unmap can call
	 * mark_page_accessed() on VDSO pages.
	 */
	/* 译注：unmap 可能对 VDSO 等无 memcg 计费页调用 mark_page_accessed()，这里排除它们。 */
	if (mem_cgroup_disabled() || folio_memcg_charged(folio)) {
		rcu_read_lock();
		workingset_age_nonresident(folio_lruvec(folio), folio_nr_pages(folio));
		rcu_read_unlock();
	}
}

/*
 * Shadow entries reflect the share of the working set that does not
 * fit into memory, so their number depends on the access pattern of
 * the workload.  In most cases, they will refault or get reclaimed
 * along with the inode, but a (malicious) workload that streams
 * through files with a total size several times that of available
 * memory, while preventing the inodes from being reclaimed, can
 * create excessive amounts of shadow nodes.  To keep a lid on this,
 * track shadow nodes and reclaim them when they grow way past the
 * point where they would still be useful.
 */
/*
 * 译注：shadow 数量反映工作集中装不进内存的部分，通常会重新 fault 或随 inode 一起
 * 回收；但恶意流式访问可遍历数倍于内存的文件并阻止 inode 回收，制造过量纯 shadow
 * XArray 节点。为限制这类元数据，单独跟踪纯 shadow 节点，超过仍有决策价值的规模后
 * 由 shrinker 回收。
 */

/* 全局 memcg-aware list_lru 只链接“非空且全部槽均为 shadow value”的 xa_node。 */
struct list_lru shadow_nodes;

/*
 * workingset_update_node() - 按 xa_node 内容变化维护 shadow_nodes 成员资格。
 * 业务背景：page cache 插入/删除和 xa_delete_node 回调在持 i_pages xa_lock 时调用；
 * 纯 shadow 节点进入可回收 LRU，出现真实 folio或清空/释放时退出。
 * 入参：node 为借用且由 node->array->xa_lock 稳定的 XArray 节点。
 * 出参/返回：无直接返回值；可能增删 node->private_list 并同步 WORKINGSET_NODES 计数。
 * 注意事项：xa_lock 与 list_lru 内锁共同保护链表；list_empty 快速判断在 xa_lock 下可靠，
 * list_lru 按 node 所在 slab 对象推导 NUMA/memcg 归属，不转移 node ownership。
 */
void workingset_update_node(struct xa_node *node)
{
	struct page *page = virt_to_page(node);

	/*
	 * Track non-empty nodes that contain only shadow entries;
	 * unlink those that contain pages or are being freed.
	 *
	 * Avoid acquiring the list_lru lock when the nodes are
	 * already where they should be. The list_empty() test is safe
	 * as node->private_list is protected by the i_pages lock.
	 */
	/*
	 * 译注：只跟踪 count>0 且 count==nr_values 的纯 shadow 节点；含页面或正释放的节点
	 * 必须摘链。节点已处于正确状态时跳过 list_lru 锁，而 private_list 判空由 i_pages
	 * 锁保护，能与插入、删除和 shrinker 反向锁序列化。
	 */
	lockdep_assert_held(&node->array->xa_lock);

	if (node->count && node->count == node->nr_values) {
		/* 从非纯 shadow 转为纯 shadow 的发布点：先入 LRU，再增加节点统计。 */
		if (list_empty(&node->private_list)) {
			list_lru_add_obj(&shadow_nodes, &node->private_list);
			__inc_node_page_state(page, WORKINGSET_NODES);
		}
	} else {
		/* 出现真实页或节点清空时先摘 LRU，阻止 shrinker 再选择它。 */
		if (!list_empty(&node->private_list)) {
			list_lru_del_obj(&shadow_nodes, &node->private_list);
			__dec_node_page_state(page, WORKINGSET_NODES);
		}
	}
}

/*
 * count_shadow_nodes() - 计算本轮 shrinker 应回收的过量 shadow xa_node 数。
 * 业务背景：shadow 元数据只有在 refault 距离仍可能命中时有价值；shrinker count
 * 回调按目标 NUMA/memcg 的缓存容量保留合理上限，只报告超额部分。
 * 入参：shrinker 为已注册 shadow shrinker 的借用指针；sc 为借用扫描上下文，提供
 * nid、memcg 和回收约束。
 * 出参/返回：无节点返回 SHRINK_EMPTY；未超限返回 0；否则返回 nodes-max_nodes。
 * 注意事项：memcg 统计刷新可能睡眠；容量是近似值，按最坏 1/8 节点密度折中元数据
 * 成本和 refault 可观测性，不锁定随后实际可隔离数量。
 */
static unsigned long count_shadow_nodes(struct shrinker *shrinker,
					struct shrink_control *sc)
{
	unsigned long max_nodes;
	unsigned long nodes;
	unsigned long pages;

	nodes = list_lru_shrink_count(&shadow_nodes, sc);
	/* 空 LRU 用 SHRINK_EMPTY 告诉回收核心无需调用 scan。 */
	if (!nodes)
		return SHRINK_EMPTY;

	/*
	 * Approximate a reasonable limit for the nodes
	 * containing shadow entries. We don't need to keep more
	 * shadow entries than possible pages on the active list,
	 * since refault distances bigger than that are dismissed.
	 *
	 * The size of the active list converges toward 100% of
	 * overall page cache as memory grows, with only a tiny
	 * inactive list. Assume the total cache size for that.
	 *
	 * Nodes might be sparsely populated, with only one shadow
	 * entry in the extreme case. Obviously, we cannot keep one
	 * node for every eligible shadow entry, so compromise on a
	 * worst-case density of 1/8th. Below that, not all eligible
	 * refaults can be detected anymore.
	 *
	 * On 64-bit with 7 xa_nodes per page and 64 slots
	 * each, this will reclaim shadow entries when they consume
	 * ~1.8% of available memory:
	 *
	 * PAGE_SIZE / xa_nodes / node_entries * 8 / PAGE_SIZE
	 */
	/*
	 * 译注：无需保留多于潜在 active 页数的 shadow，因为更长 refault distance 本就
	 * 被拒绝。active 可随内存趋近全部缓存，故用总缓存近似。xa_node 最坏仅一个 shadow，
	 * 不能按每候选一节点保留，折中要求至少 1/8 密度；64 位每页约 7 个、每节点 64 槽，
	 * 因而 shadow 节点到约可用内存 1.8% 时开始回收。
	 */
#ifdef CONFIG_MEMCG
	if (sc->memcg) {
		struct lruvec *lruvec;
		int i;

		/* memcg 回收只计算该组在目标节点的 LRU 与 slab 页面近似容量。 */
		mem_cgroup_flush_stats_ratelimited(sc->memcg);
		lruvec = mem_cgroup_lruvec(sc->memcg, NODE_DATA(sc->nid));

		for (pages = 0, i = 0; i < NR_LRU_LISTS; i++)
			pages += lruvec_lru_size(lruvec, i, MAX_NR_ZONES - 1);

		pages += lruvec_page_state_local(
			lruvec, NR_SLAB_RECLAIMABLE_B) >> PAGE_SHIFT;
		pages += lruvec_page_state_local(
			lruvec, NR_SLAB_UNRECLAIMABLE_B) >> PAGE_SHIFT;
	} else
#endif
		/* 全局回收用节点 present pages 作为所有 cache/slab 可竞争容量的上界。 */
		pages = node_present_pages(sc->nid);

	/* XA_CHUNK_SHIFT 槽位再减 3 等价于除以“每节点槽数/8”。 */
	max_nodes = pages >> (XA_CHUNK_SHIFT - 3);

	if (nodes <= max_nodes)
		return 0;
	return nodes - max_nodes;
}

/*
 * shadow_lru_isolate() - 从 shadow LRU 隔离并删除一个纯 value xa_node。
 * 业务背景：list_lru shrink walk 初始持 lru->lock，但页缓存正常锁序是 i_pages xa_lock
 * 外层、LRU 锁内层；本回调用 trylock 反转过渡，避免 ABBA 死锁。
 * 入参：item 是嵌入 xa_node 的 private_list；lru 为当前已加锁子链；arg 未使用。
 * 出参/返回：锁竞争返回 LRU_RETRY；成功或发现不变量异常均返回 LRU_REMOVED_RETRY；
 * 成功时节点从 LRU/统计摘除，纯 shadow 槽和空 node 由 xa_delete_node() 删除释放。
 * 注意事项：入口持 lru->lock 且 IRQ 已禁用；所有出口按 list_lru 回调协议释放该锁，
 * i_pages 锁稳定 mapping，inode i_lock 保护 inode LRU 状态；函数可 cond_resched()。
 */
static enum lru_status shadow_lru_isolate(struct list_head *item,
					  struct list_lru_one *lru,
					  void *arg) __must_hold(lru->lock)
{
	struct xa_node *node = container_of(item, struct xa_node, private_list);
	struct address_space *mapping;
	int ret;

	/*
	 * Page cache insertions and deletions synchronously maintain
	 * the shadow node LRU under the i_pages lock and the
	 * &lru->lock. Because the page cache tree is emptied before
	 * the inode can be destroyed, holding the &lru->lock pins any
	 * address_space that has nodes on the LRU.
	 *
	 * We can then safely transition to the i_pages lock to
	 * pin only the address_space of the particular node we want
	 * to reclaim, take the node off-LRU, and drop the &lru->lock.
	 */
	/*
	 * 译注：页缓存插入/删除在 i_pages 锁和 lru 锁下同步维护 shadow LRU；inode 销毁前
	 * 必先清空页缓存，所以持 lru 锁可暂时钉住所有仍有 LRU 节点的 address_space。
	 * 随后 trylock 目标 i_pages，摘除节点并释放 lru 锁，把广域生命周期保证收缩到
	 * 当前 mapping；这条过渡闭合 inode 回收与 shadow shrink 的竞态。
	 */

	mapping = container_of(node->array, struct address_space, i_pages);

	/* Coming from the list, invert the lock order */
	/* 译注：当前从 LRU 锁向外层 i_pages 锁逆序获取，只能 trylock；失败必须退锁重试。 */
	if (!xa_trylock(&mapping->i_pages)) {
		spin_unlock_irq(&lru->lock);
		ret = LRU_RETRY;
		goto out;
	}

	/* For page cache we need to hold i_lock */
	/* 译注：有 host 的页缓存 mapping 还要 trylock inode，稳定后续 shrinkable/LRU 更新。 */
	if (mapping->host != NULL) {
		if (!spin_trylock(&mapping->host->i_lock)) {
			xa_unlock(&mapping->i_pages);
			spin_unlock_irq(&lru->lock);
			ret = LRU_RETRY;
			goto out;
		}
	}

	/* 三把保护中先摘 LRU并减统计，使其他 shrinker 不再选择该 node。 */
	list_lru_isolate(lru, item);
	__dec_node_page_state(virt_to_page(node), WORKINGSET_NODES);

	spin_unlock(&lru->lock);

	/*
	 * The nodes should only contain one or more shadow entries,
	 * no pages, so we expect to be able to remove them all and
	 * delete and free the empty node afterwards.
	 */
	/*
	 * 译注：被跟踪节点应含至少一个 value 且 count==nr_values，不含真实 folio；满足时
	 * 删除全部 shadow，XArray 随后释放空节点。WARN 分支仍完成解锁和 inode LRU 修复。
	 */
	if (WARN_ON_ONCE(!node->nr_values))
		goto out_invalid;
	if (WARN_ON_ONCE(node->count != node->nr_values))
		goto out_invalid;
	xa_delete_node(node, workingset_update_node);
	mod_lruvec_kmem_state(node, WORKINGSET_NODERECLAIM, 1);

out_invalid:
	/* 先用 irq 版本释放最外层 xa_lock，恢复进入回调前的中断状态。 */
	xa_unlock_irq(&mapping->i_pages);
	if (mapping->host != NULL) {
		if (mapping_shrinkable(mapping))
			inode_lru_list_add(mapping->host);
		spin_unlock(&mapping->host->i_lock);
	}
	ret = LRU_REMOVED_RETRY;
out:
	/* 返回前给长时间 shadow 回收让出 CPU；此时不再持上述自旋锁。 */
	cond_resched();
	return ret;
}

/*
 * scan_shadow_nodes() - 让 list_lru shrinker 扫描并删除过量 shadow 节点。
 * 业务背景：VM shrinker 在 count 报告超额后调用本 scan 回调，逐项进入
 * shadow_lru_isolate() 完成锁序过渡和 XArray 删除。
 * 入参：shrinker/sc 均为回收框架借用对象，sc 给出扫描预算与 NUMA/memcg 范围。
 * 出参/返回：返回 list_lru 实际处理数量；回调可能因锁竞争要求重试。
 * 注意事项：必须使用 IRQ-safe walk，因为 list_lru 锁嵌套在同样 IRQ-safe 的 i_pages
 * 锁内；可能调度，不能在调用后假定某个 shadow 仍存在。
 */
static unsigned long scan_shadow_nodes(struct shrinker *shrinker,
				       struct shrink_control *sc)
{
	/* list_lru lock nests inside the IRQ-safe i_pages lock */
	/* 译注：锁类规定 list_lru 位于 IRQ-safe i_pages 锁内，因此 walk 也必须关闭 IRQ。 */
	return list_lru_shrink_walk_irq(&shadow_nodes, sc, shadow_lru_isolate,
					NULL);
}

/*
 * Our list_lru->lock is IRQ-safe as it nests inside the IRQ-safe
 * i_pages lock.
 */
/* 译注：shadow list_lru 锁作为 i_pages IRQ-safe 锁的内层，使用独立 lockdep 类验证顺序。 */
static struct lock_class_key shadow_nodes_key;

/*
 * workingset_init() - 初始化 shadow 编码精度、LRU 与内存回收 shrinker。
 * 业务背景：模块/内建初始化时根据启动内存计算时间戳桶，随后发布 memcg/NUMA-aware
 * shadow LRU 回收器；page cache 开始生成 shadow 前必须完成。
 * 入参：无。
 * 出参/返回：成功返回 0；shrinker 分配或 list_lru 初始化失败返回负 errno，并按已取得
 * 资源逆序释放；成功后 shrinker 框架持有注册对象，shadow_nodes 保持到系统结束。
 * 注意事项：初始化进程上下文可睡眠；BUILD_BUG_ON 保证基本字段可编码，热插拔预留
 * 最多初始内存两倍的距离范围；注册后无本文件卸载路径。
 */
static int __init workingset_init(void)
{
	unsigned int timestamp_bits, timestamp_bits_anon;
	struct shrinker *workingset_shadow_shrinker;
	unsigned int max_order;
	int ret = -ENOMEM;

	/* 固定身份字段若已占满 unsigned long，任何时间戳方案都不可表示，编译即失败。 */
	BUILD_BUG_ON(BITS_PER_LONG < EVICTION_SHIFT);
	/*
	 * Calculate the eviction bucket size to cover the longest
	 * actionable refault distance, which is currently half of
	 * memory (totalram_pages/2). However, memory hotplug may add
	 * some more pages at runtime, so keep working with up to
	 * double the initial memory by using totalram_pages as-is.
	 */
	/*
	 * 译注：最长有意义 refault 当前约为总内存一半；为容纳热插拔后额外页面，直接以
	 * 初始 totalram_pages 的阶数编码，相当于为约两倍初始内存保留距离。FILE/ANON
	 * 可用位数不同，超出时分别设置丢弃低位的 bucket_order。
	 */
	timestamp_bits = BITS_PER_LONG - EVICTION_SHIFT;
	timestamp_bits_anon = BITS_PER_LONG - EVICTION_SHIFT_ANON;
	max_order = fls_long(totalram_pages() - 1);
	if (max_order > (BITS_PER_LONG - EVICTION_SHIFT))
		bucket_order[WORKINGSET_FILE] = max_order - timestamp_bits;
	if (max_order > timestamp_bits_anon)
		bucket_order[WORKINGSET_ANON] = max_order - timestamp_bits_anon;
	/* 启动日志暴露实际编码位数与桶精度，便于解释架构/内存规模差异。 */
	pr_info("workingset: timestamp_bits=%d (anon: %d) max_order=%d bucket_order=%u (anon: %d)\n",
		timestamp_bits, timestamp_bits_anon, max_order,
		bucket_order[WORKINGSET_FILE], bucket_order[WORKINGSET_ANON]);

	/* shrinker 同时按 NUMA 节点和 memcg 隔离预算，与 shadow_nodes 的归属方式一致。 */
	workingset_shadow_shrinker = shrinker_alloc(SHRINKER_NUMA_AWARE |
						    SHRINKER_MEMCG_AWARE,
						    "mm-shadow");
	if (!workingset_shadow_shrinker)
		goto err;

	/* 先把 LRU 与 shrinker/memcg 基础设施绑定，失败时只需释放未注册 shrinker。 */
	ret = list_lru_init_memcg_key(&shadow_nodes, workingset_shadow_shrinker,
				      &shadow_nodes_key);
	if (ret)
		goto err_list_lru;

	workingset_shadow_shrinker->count_objects = count_shadow_nodes;
	workingset_shadow_shrinker->scan_objects = scan_shadow_nodes;
	/* ->count reports only fully expendable nodes */
	/* 译注：count 已只报告完全可牺牲的超额节点，所以 seeks=0 表示无需额外扫描成本折扣。 */
	workingset_shadow_shrinker->seeks = 0;

	shrinker_register(workingset_shadow_shrinker);
	/* 注册是发布点；此后 VM 回收可并发调用 count/scan，初始化对象不再回滚。 */
	return 0;
err_list_lru:
	/* list_lru 初始化失败时 shrinker 尚未注册，仅释放分配对象。 */
	shrinker_free(workingset_shadow_shrinker);
err:
	return ret;
}

/* 内建或模块初始化入口；本文件没有卸载清理，符合核心 MM 生命周期。 */
module_init(workingset_init);
