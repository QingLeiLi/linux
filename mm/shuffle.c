// SPDX-License-Identifier: GPL-2.0
// Copyright(c) 2018 Intel Corporation. All rights reserved.

#include <linux/mm.h>
#include <linux/init.h>
#include <linux/mmzone.h>
#include <linux/random.h>
#include <linux/moduleparam.h>
#include "internal.h"
#include "shuffle.h"

/*
 * page_alloc_shuffle_key 默认 false；启动参数置 true 后 patch static-branch 调用点，
 * 同时门控启动/热插拔全区洗牌与 buddy 高阶块随机头尾插入。键静态存活，不归调用者所有。
 */
DEFINE_STATIC_KEY_FALSE(page_alloc_shuffle_key);

/* page_alloc.shuffle 的只读参数 backing；启动解析前为 false，解析后供 sysfs 读回。 */
static bool shuffle_param;

/*
 * shuffle_param_set() - 解析启动期 page_alloc.shuffle 并提交 static key。
 * 业务背景：编译 CONFIG 只提供能力，平台/用户必须显式启用才承担洗牌成本。
 * @val 是 NUL 结尾的借用参数文本；@kp 是借用参数描述，arg 指向 shuffle_param。
 * 出参/返回：合法布尔返回 0，解析失败返回 -EINVAL；true 时永久启用本启动期 static key。
 * 注意事项：__meminit、参数权限 0400 禁止运行期写；false 只保持默认关闭，不执行 disable。
 */
static __meminit int shuffle_param_set(const char *val,
		const struct kernel_param *kp)
{
	if (param_set_bool(val, kp))
		return -EINVAL;
	if (*(bool *)kp->arg)
		static_branch_enable(&page_alloc_shuffle_key);
	return 0;
}

/*
 * 参数操作表静态存活：set 使用上述提交逻辑，get 以标准 bool 格式导出当前 backing；
 * 两个函数借用 kernel_param，不取得其 arg ownership。
 */
static const struct kernel_param_ops shuffle_param_ops = {
	.set = shuffle_param_set,
	.get = param_get_bool,
};

/* 注册只读 `page_alloc.shuffle`：启动命令行可设置，sysfs 模式 0400 仅允许读取。 */
module_param_cb(shuffle, &shuffle_param_ops, &shuffle_param, 0400);

/*
 * For two pages to be swapped in the shuffle, they must be free (on a
 * 'free_area' lru), have the same order, and have the same migratetype.
 */
/*
 * 两页只有同时位于 free_area、buddy 阶数相同且 migratetype 相同才允许交换。
 * 本 helper 先验证在线/zone/PageBuddy/order，migratetype 因需要两页对比而由调用者检查。
 */
/*
 * shuffle_valid_page() - 验证随机 PFN 能否作为指定 zone/order 的空闲块候选。
 * @zone 是已持 zone->lock 的借用对象，@pfn 是候选页帧号，@order 是 buddy 阶数。
 * 返回借用 page 指针；PFN 离线、跨 zone、非 PageBuddy 或阶数不符均返回 NULL。
 * 注意事项：不取页引用、不睡眠；PageBuddy/order 快照只在调用者持锁期间有效。
 */
static struct page * __meminit shuffle_valid_page(struct zone *zone,
						  unsigned long pfn, int order)
{
	struct page *page = pfn_to_online_page(pfn);

	/*
	 * Given we're dealing with randomly selected pfns in a zone we
	 * need to ask questions like...
	 */
	/* 随机 PFN 可能落入洞、离线页或其他 zone，所以下面按不变量逐层过滤。 */

	/* ... is the page managed by the buddy? */
	/* 首先要求 PFN 对应在线 struct page；洞和离线内存没有可交换链表节点。 */
	if (!page)
		return NULL;

	/* ... is the page assigned to the same zone? */
	/* zone span 可含边界/洞，必须拒绝不属于目标 zone 的在线页。 */
	if (page_zone(page) != zone)
		return NULL;

	/* ...is the page free and currently on a free_area list? */
	/* PageBuddy 表示该页当前是 free_area 中一个 buddy 块的头页。 */
	if (!PageBuddy(page))
		return NULL;

	/*
	 * ...is the page on the same list as the page we will
	 * shuffle it with?
	 */
	/* 阶数必须与 SHUFFLE_ORDER 候选一致，否则两个 lru 节点属于不同 free_area。 */
	if (buddy_order(page) != order)
		return NULL;

	return page;
}

/*
 * Fisher-Yates shuffle the freelist which prescribes iterating through an
 * array, pfns in this case, and randomly swapping each entry with another in
 * the span, end_pfn - start_pfn.
 *
 * To keep the implementation simple it does not attempt to correct for sources
 * of bias in the distribution, like modulo bias or pseudo-random number
 * generator bias. I.e. the expectation is that this shuffling raises the bar
 * for attacks that exploit the predictability of page allocations, but need not
 * be a perfect shuffle.
 */
/*
 * 实现借鉴 Fisher-Yates：按 PFN 顺序遍历 zone，并为每个合格 SHUFFLE_ORDER 块从
 * 整个 span 随机挑另一个块交换 freelist 节点。它不修正取模偏差或伪随机源偏差，
 * 目标只是降低物理页分配的可预测性、提高利用 memory-side cache 的平均效果，
 * 不承诺均匀置换，也不能当作密码学安全随机化边界。
 */
/* 每个顺序候选最多尝试 10 个随机 PFN，限制稀疏 zone 中持锁扫描成本。 */
#define SHUFFLE_RETRY 10
/*
 * __shuffle_zone() - 重排一个 zone 的 SHUFFLE_ORDER 空闲块链表顺序。
 * 业务背景：启动期及内存上线后建立初始随机分布，后续 buddy 释放再持续随机插入。
 * @z 是非 NULL 输入输出借用 zone；页框/free_area 已初始化，调用者未持 z->lock。
 * 出参/返回：无直接返回；只交换同 order、同 migratetype 的 lru 节点，不改页数或类型。
 * 注意事项：__meminit，可睡眠；内部 irqsave 持 zone 锁并周期性解锁 cond_resched。
 */
void __meminit __shuffle_zone(struct zone *z)
{
	/* 变量地图：i 顺序遍历块头 PFN，flags 保存 IRQ 状态，边界单位均为 PFN。 */
	unsigned long i, flags;
	unsigned long start_pfn = z->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(z);
	const int order = SHUFFLE_ORDER;
	const int order_pages = 1 << order;

	/* 阶段 1：锁住 buddy free_area，并把首 PFN 上调到洗牌阶数的块边界。 */
	spin_lock_irqsave(&z->lock, flags);
	start_pfn = ALIGN(start_pfn, order_pages);
	for (i = start_pfn; i < end_pfn; i += order_pages) {
		/* j 是随机 PFN；migratetype 固定目标链；retry 限制洞重抽；page_* 均为锁内借用。 */
		unsigned long j;
		int migratetype, retry;
		struct page *page_i, *page_j;

		/*
		 * We expect page_i, in the sub-range of a zone being added
		 * (@start_pfn to @end_pfn), to more likely be valid compared to
		 * page_j randomly selected in the span @zone_start_pfn to
		 * @spanned_pages.
		 */
		/*
		 * i 位于本轮 zone 顺序子范围，通常比从整个 zone span 随机抽取的 j 更可能
		 * 有效；两者仍都必须在锁内重新验证，不能把 span 当作连续在线内存。
		 */
		/*
		 * 修正说明：原注释沿用了曾传入新增子区间的表述；当前函数没有 start/end
		 * 参数，而是用 zone_start_pfn..zone_end_pfn 遍历整个 zone。热插拔调用也因此
		 * 在新页解除隔离后重洗全 zone，而不是只处理刚上线的 PFN 范围。
		 */
		page_i = shuffle_valid_page(z, i, order);
		if (!page_i)
			continue;

		for (retry = 0; retry < SHUFFLE_RETRY; retry++) {
			/*
			 * Pick a random order aligned page in the zone span as
			 * a swap target. If the selected pfn is a hole, retry
			 * up to SHUFFLE_RETRY attempts find a random valid pfn
			 * in the zone.
			 */
			/*
			 * 从 `[zone_start_pfn, zone_start_pfn + spanned_pages)` 取模并按块粒度
			 * 向下对齐；洞、离线页、已分配页和错阶页最多重试 SHUFFLE_RETRY 次。
			 */
			j = z->zone_start_pfn +
				ALIGN_DOWN(get_random_long() % z->spanned_pages,
						order_pages);
			page_j = shuffle_valid_page(z, j, order);
			if (page_j && page_j != page_i)
				break;
		}
		if (retry >= SHUFFLE_RETRY) {
			/* 稀疏或繁忙 zone 找不到目标只降低随机度，不影响 allocator 正确性。 */
			pr_debug("%s: failed to swap %#lx\n", __func__, i);
			continue;
		}

		/*
		 * Each migratetype corresponds to its own list, make sure the
		 * types match otherwise we're moving pages to lists where they
		 * do not belong.
		 */
		/*
		 * 每种 migratetype 有独立 free list；交换不同类型节点会让后续分配从错误策略
		 * 链表取页，因此在真正改链前比较 pageblock 类型，失败则保持两链不变。
		 */
		migratetype = get_pageblock_migratetype(page_i);
		if (get_pageblock_migratetype(page_j) != migratetype) {
			pr_debug("%s: migratetype mismatch %#lx\n", __func__, i);
			continue;
		}

		/* 阶段 2（提交点）：在同一 z->lock 临界区交换两节点位置，计数和属性均不变。 */
		list_swap(&page_i->lru, &page_j->lru);

		pr_debug("%s: swap: %#lx -> %#lx\n", __func__, i, j);

		/* take it easy on the zone lock */
		/* 每约 100 个洗牌阶块释放 IRQ/zone 锁并让出 CPU，避免长时间阻塞分配热路径。 */
		if ((i % (100 * order_pages)) == 0) {
			spin_unlock_irqrestore(&z->lock, flags);
			cond_resched();
			spin_lock_irqsave(&z->lock, flags);
		}
	}
	/* 阶段 3：结束遍历，恢复调用者进入函数前的本地 IRQ 状态。 */
	spin_unlock_irqrestore(&z->lock, flags);
}

/*
 * __shuffle_free_memory - reduce the predictability of the page allocator
 * @pgdat: node page data
 */
/*
 * __shuffle_free_memory() 通过重排一个 NUMA node 的空闲链表降低 page allocator
 * 可预测性。@pgdat 是不可为 NULL 的借用 node 数据；函数逐个遍历固定 MAX_NR_ZONES，
 * 并经 static-key 包装 shuffle_zone() 进入核心。无直接返回，不改 node/zone ownership。
 * __meminit 且各 zone 核心可解锁调度；调用者必须处于内存初始化允许睡眠的上下文。
 */
void __meminit __shuffle_free_memory(pg_data_t *pgdat)
{
	/* z 是 node_zones 数组内的借用游标；空 zone 的核心循环自然不做任何交换。 */
	struct zone *z;

	/* 覆盖 DMA/Normal/Movable 等全部槽位，使存在空闲块的每种 zone 都获得初始扰动。 */
	for (z = pgdat->node_zones; z < pgdat->node_zones + MAX_NR_ZONES; z++)
		shuffle_zone(z);
}

/*
 * shuffle_pick_tail() - 从共享随机位缓存返回一次 buddy freelist 头/尾选择。
 * 业务背景：__free_one_page() 在最终阶数达到 SHUFFLE_ORDER 时用它替代可预测启发式。
 * 入参：无。出参/返回：每次消费 rand 的最低位；true 选尾、false 选头，无 ownership 变化。
 * 注意事项：故意无锁，竞态允许丢失/重复状态且只作为额外扰动；不可用于安全随机或公平性。
 */
bool shuffle_pick_tail(void)
{
	/* rand 缓存 64 个随机选择位，rand_bits 记录尚未消费的低位数量。 */
	static u64 rand;
	static u8 rand_bits;
	bool ret;

	/*
	 * The lack of locking is deliberate. If 2 threads race to
	 * update the rand state it just adds to the entropy.
	 */
	/*
	 * 缺锁是有意设计：两个线程竞争 refill/shift 最多改变位消费顺序或覆盖缓存，
	 * 对本用途只会增加不可预测扰动，不承担必须精确序列化的 allocator 不变量。
	 */
	if (rand_bits == 0) {
		/* 阶段 1：缓存耗尽时一次获取 64 位，摊薄 get_random_u64() 调用成本。 */
		rand_bits = 64;
		rand = get_random_u64();
	}

	/* 阶段 2：读取最低位作为本次选择，再右移并递减剩余计数。 */
	ret = rand & 1;

	rand_bits--;
	rand >>= 1;

	/* 返回后调用者只消费布尔决策，不取得共享 rand 状态的任何所有权。 */
	return ret;
}
