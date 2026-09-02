// SPDX-License-Identifier: GPL-2.0
/*
 * linux/mm/page_isolation.c
 */

#include <linux/mm.h>
#include <linux/page-isolation.h>
#include <linux/pageblock-flags.h>
#include <linux/memory.h>
#include <linux/hugetlb.h>
#include <linux/page_owner.h>
#include <linux/migrate.h>
#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/page_isolation.h>

/*
 * pageblock isolation 是连续分配/CMA/内存下线的第一阶段：先把 migratetype 改成
 * MIGRATE_ISOLATE，阻止新的 buddy 分配，再由调用者迁移/回收在用页，最后测试
 * 是否全空闲。zone->lock 保护 pageblock 标志、free list 和 isolate 计数；范围级
 * API 自身没有全局互斥，重叠请求靠发现已 isolate 后回滚来仲裁。
 */
/*
 * page_is_unmovable() - 无引用地判断当前页是否阻碍 pageblock 隔离。
 * @zone 必须是 @page 所属 zone，@page 只借用，@mode 区分 CMA/下线/普通扫描，
 * @step 输入通常为 1、输出可跳过整个 buddy/compound 范围。不可移动返回 true，
 * 可移动/可忽略返回 false。函数不锁页，结论允许假阴性，调用者稍后迁移阶段
 * 仍须作权威检查；不应依赖它保证页生命周期。
 */
bool page_is_unmovable(struct zone *zone, struct page *page,
		enum pb_isolate_mode mode, unsigned long *step)
{
	/*
	 * Both, bootmem allocations and memory holes are marked
	 * PG_reserved and are unmovable. We can even have unmovable
	 * allocations inside ZONE_MOVABLE, for example when
	 * specifying "movablecore".
	 */
	/* 中文翻译：bootmem 和内存洞都标 PG_reserved；ZONE_MOVABLE 也可能含此类页。 */
	if (PageReserved(page))
		return true;

	/*
	 * If the zone is movable and we have ruled out all reserved
	 * pages then it should be reasonably safe to assume the rest
	 * is movable.
	 */
	/* 中文翻译：排除 reserved 后，ZONE_MOVABLE 中其余页可合理视为可移动。 */
	if (zone_idx(zone) == ZONE_MOVABLE)
		return false;

	/*
	 * Hugepages are not in LRU lists, but they're movable.
	 * THPs are on the LRU, but need to be counted as #small pages.
	 * We need not scan over tail pages because we don't
	 * handle each tail page individually in migration.
	 */
	/*
	 * 中文翻译：HugeTLB 不在 LRU 但可迁移；THP 在 LRU 且按 base page 计数，
	 * 迁移以 compound 整体处理，扫描无需逐个 tail。
	 */
	if (PageCompound(page)) {
		/* folio 未加引用，仅在当前无锁分类窗口读取 order/type。 */
		struct folio *folio = page_folio(page);
		unsigned long nr_pages, pfn;
		unsigned int order;

		/* 超过通用 folio 可表示阶数时不能安全计算/迁移，保守判为不可移动。 */
		order = compound_order(&folio->page);
		if (order > MAX_FOLIO_ORDER)
			return true;

		/* HugeTLB 还要求架构开关和对应 hstate 都声明支持迁移。 */
		if (folio_test_hugetlb(folio)) {
			struct hstate *h;

			if (!IS_ENABLED(CONFIG_ARCH_ENABLE_HUGEPAGE_MIGRATION))
				return true;

			/*
			 * The huge page may be freed so can not
			 * use folio_hstate() directly.
			 */
			/* 中文翻译：huge page 可能并发释放，不能直接用会解引用 folio 的 hstate。 */
			h = size_to_hstate(PAGE_SIZE << order);
			if (!h || !hugepage_migration_supported(h))
				return true;
		} else if (!PageLRU(page)) {
			/* 非 HugeTLB compound 只有在 LRU 上才具备这里认可的迁移协议。 */
			return true;
		}

		/* step 跳到 compound 尾后；即使从 tail 中间开始也不会重复检查余下 tail。 */
		nr_pages = 1UL << order;
		pfn = page_to_pfn(page);
		*step = (pfn | (nr_pages - 1)) + 1 - pfn;
		return false;
	}

	/*
	 * We can't use page_count without pin a page
	 * because another CPU can free compound page.
	 * This check already skips compound tails of THP
	 * because their page->_refcount is zero at all time.
	 */
	/* 中文翻译：无引用不能安全 page_count；零 ref 也自然跳过 refcount 恒零的 THP tail。 */
	if (!page_ref_count(page)) {
		/* buddy head 可按 order 一次跨过整个空闲块，普通零引用页只前进一页。 */
		if (PageBuddy(page))
			*step = (1 << buddy_order(page));
		return false;
	}

	/*
	 * The HWPoisoned page may be not in buddy system, and
	 * page_count() is not 0.
	 */
	/* 中文翻译：HWPoison 页可能不在 buddy 且 refcount 非零，下线模式仍允许跳过。 */
	if ((mode == PB_ISOLATE_MODE_MEM_OFFLINE) && PageHWPoison(page))
		return false;

	/*
	 * We treat all PageOffline() pages as movable when offlining
	 * to give drivers a chance to decrement their reference count
	 * in MEM_GOING_OFFLINE in order to indicate that these pages
	 * can be offlined as there are no direct references anymore.
	 * For actually unmovable PageOffline() where the driver does
	 * not support this, we will fail later when trying to actually
	 * move these pages that still have a reference count > 0.
	 * (false negatives in this function only)
	 */
	/*
	 * 中文翻译：下线时先把 PageOffline 当可移动，让驱动在 MEM_GOING_OFFLINE
	 * 丢引用；若驱动不支持，稍后的实际迁移会因剩余引用失败，所以这里只会假阴性。
	 */
	if ((mode == PB_ISOLATE_MODE_MEM_OFFLINE) && PageOffline(page))
		return false;

	/* LRU 页和注册 movable_ops 的驱动页都有后续迁移通道。 */
	if (PageLRU(page) || page_has_movable_ops(page))
		return false;

	/*
	 * If there are RECLAIMABLE pages, we need to check
	 * it.  But now, memory offline itself doesn't call
	 * shrink_node_slabs() and it still to be fixed.
	 */
	/* 中文翻译：reclaimable slab 仍需额外检查，但内存下线尚未主动 shrink_node_slabs。 */
	return true;
}

/*
 * This function checks whether the range [start_pfn, end_pfn) includes
 * unmovable pages or not. The range must fall into a single pageblock and
 * consequently belong to a single zone.
 *
 * PageLRU check without isolation or lru_lock could race so that
 * MIGRATE_MOVABLE block might include unmovable pages. Similarly, pages
 * with movable_ops can only be identified some time after they were
 * allocated. So you can't expect this function should be exact.
 *
 * Returns a page without holding a reference. If the caller wants to
 * dereference that page (e.g., dumping), it has to make sure that it
 * cannot get removed (e.g., via memory unplug) concurrently.
 *
 */
/*
 * 中文学习补充：has_unmovable_pages() 只做 pageblock 内的尽力预检；LRU 与
 * movable_ops 识别存在竞态。返回 page 不带引用，调用者若要 dump 必须另由
 * hotplug 锁等保证 struct page 不消失。范围必须在同一 pageblock/zone。
 */
static struct page *has_unmovable_pages(unsigned long start_pfn, unsigned long end_pfn,
				enum pb_isolate_mode mode)
{
	struct page *page = pfn_to_page(start_pfn);
	struct zone *zone = page_zone(page);

	/* 单 pageblock 前置条件让首个 page 的 zone 可代表完整范围。 */
	VM_BUG_ON(pageblock_start_pfn(start_pfn) !=
		  pageblock_start_pfn(end_pfn - 1));

	if (is_migrate_cma_page(page)) {
		/*
		 * CMA allocations (alloc_contig_range) really need to mark
		 * isolate CMA pageblocks even when they are not movable in fact
		 * so consider them movable here.
		 */
		/* 中文翻译：CMA 自己必须隔离 CMA block，即使其中页当前看似不可移动。 */
		if (mode == PB_ISOLATE_MODE_CMA_ALLOC)
			return NULL;

		return page;
	}

	/* step 由分类器扩展，可按 buddy/compound 粒度跳过，不逐 base page 重扫。 */
	while (start_pfn < end_pfn) {
		unsigned long step = 1;

		page = pfn_to_page(start_pfn);
		if (page_is_unmovable(zone, page, mode, &step))
			return page;

		start_pfn += step;
	}
	/* NULL 表示预检未发现阻塞者，不等于实际迁移必然成功。 */
	return NULL;
}

/*
 * This function set pageblock migratetype to isolate if no unmovable page is
 * present in [start_pfn, end_pfn). The pageblock must intersect with
 * [start_pfn, end_pfn).
 */
/*
 * set_migratetype_isolate() - 锁内预检并发布单个 pageblock 的 isolate 状态。
 * @page 位于目标 block，@mode 控制特例，[@start_pfn,@end_pfn) 是用户原范围；
 * 成功返回 0，竞态/不可移动/zone 边界失败返回 -EBUSY。可先 accept page；随后
 * zone->lock 下移动空闲页、设置标志并增加计数。失败不留修改；下线诊断在解锁
 * 后 dump，避免 printk 与 zone lock 的锁序问题。
 */
static int set_migratetype_isolate(struct page *page, enum pb_isolate_mode mode,
			unsigned long start_pfn, unsigned long end_pfn)
{
	struct zone *zone = page_zone(page);
	struct page *unmovable;
	unsigned long check_unmovable_start, check_unmovable_end;

	/* accept 可能触及固件/页内容，必须在获取 zone 自旋锁之前完成。 */
	if (PageUnaccepted(page))
		accept_page(page);

	scoped_guard(spinlock_irqsave, &zone->lock) {
		/*
		 * We assume the caller intended to SET migrate type to
		 * isolate. If it is already set, then someone else must have
		 * raced and set it before us.
		 */
		/* 中文翻译：已 isolate 说明重叠请求先到，当前请求以 -EBUSY 仲裁退出。 */
		if (is_migrate_isolate_page(page))
			return -EBUSY;

		/*
		 * FIXME: Now, memory hotplug doesn't call shrink_slab() by
		 * itself. We just check MOVABLE pages.
		 *
		 * Pass the intersection of [start_pfn, end_pfn) and the page's
		 * pageblock to avoid redundant checks.
		 */
		/* 中文翻译：hotplug 尚不 shrink slab；只检查用户范围与本 pageblock 的交集。 */
		check_unmovable_start = max(page_to_pfn(page), start_pfn);
		check_unmovable_end = min(pageblock_end_pfn(page_to_pfn(page)),
					  end_pfn);

		/* zone 锁稳定 pageblock/free list，但页的 LRU/驱动状态仍是尽力分类。 */
		unmovable = has_unmovable_pages(check_unmovable_start,
				check_unmovable_end, mode);
		if (!unmovable) {
			/* helper 设置 isolate 并把 block 内空闲页搬到对应 freelist。 */
			if (!pageblock_isolate_and_move_free_pages(zone, page))
				return -EBUSY;
			/* 计数与标志在同一 zone 锁临界区发布，二者对观察者保持一致。 */
			zone->nr_isolate_pageblock++;
			return 0;
		}
	}
	if (mode == PB_ISOLATE_MODE_MEM_OFFLINE) {
		/*
		 * printk() with zone->lock held will likely trigger a
		 * lockdep splat, so defer it here.
		 */
		/* 中文翻译：持 zone->lock printk 易触发 lockdep，故解锁后再 dump 阻塞页。 */
		dump_page(unmovable, "unmovable page");
	}

	return -EBUSY;
}

/*
 * unset_migratetype_isolate() - 撤销一个 pageblock 并恢复其空闲页可分配性。
 * @page 位于目标 block，只借用；无返回值。zone->lock 内幂等检查 isolate 标志。
 * 若 block 边界落在跨 block 高阶 buddy 内，先把该页临时从 freelist 隔离，清
 * 标志后按原 order/type 放回以触发正确合并；否则扫描搬回空闲页。最后递减
 * nr_isolate_pageblock，调用后 allocator 可再次选择该 block。
 */
static void unset_migratetype_isolate(struct page *page)
{
	struct zone *zone;
	bool isolated_page = false;
	unsigned int order;
	struct page *buddy;

	/* guard 在所有 return 上自动 irqrestore，保护 flag/free list/isolate 计数。 */
	zone = page_zone(page);
	guard(spinlock_irqsave)(&zone->lock);
	if (!is_migrate_isolate_page(page))
		return;

	/*
	 * Because freepage with more than pageblock_order on isolated
	 * pageblock is restricted to merge due to freepage counting problem,
	 * it is possible that there is free buddy page.
	 * move_freepages_block() doesn't care of merge so we need other
	 * approach in order to merge them. Isolation and free will make
	 * these pages to be merged.
	 */
	/*
	 * 中文翻译：隔离 block 会限制跨 pageblock 高阶空闲页合并，普通 move helper
	 * 不处理该情形；需通过 isolate 后重新 free 来重建正确 buddy 合并。
	 */
	/* 只处理合法、尚未达到 MAX_PAGE_ORDER 的跨 pageblock buddy head。 */
	if (PageBuddy(page)) {
		order = buddy_order(page);
		if (order >= pageblock_order && order < MAX_PAGE_ORDER) {
			buddy = find_buddy_page_pfn(page, page_to_pfn(page),
						    order, NULL);
			/* 相邻 buddy 已非 isolate 才需临时摘下当前块来跨边界重新合并。 */
			if (buddy && !is_migrate_isolate_page(buddy)) {
				isolated_page = !!__isolate_free_page(page, order);
				/*
				 * Isolating a free page in an isolated pageblock
				 * is expected to always work as watermarks don't
				 * apply here.
				 */
				/* 中文翻译：isolated block 内摘空闲页不受 watermark 限制，预期必成功。 */
				VM_WARN_ON(!isolated_page);
			}
		}
	}

	/*
	 * If we isolate freepage with more than pageblock_order, there
	 * should be no freepage in the range, so we could avoid costly
	 * pageblock scanning for freepage moving.
	 *
	 * We didn't actually touch any of the isolated pages, so place them
	 * to the tail of the freelist. This is an optimization for memory
	 * onlining - just onlined memory won't immediately be considered for
	 * allocation.
	 */
	/*
	 * 中文翻译：若已摘下高阶页，范围内无需昂贵扫描；未触碰的隔离页放回
	 * freelist 尾部，避免刚上线内存立刻成为优先分配目标。
	 */
	if (!isolated_page) {
		/*
		 * Isolating this block already succeeded, so this
		 * should not fail on zone boundaries.
		 */
		/* 中文翻译：此前 isolate 已验证 zone 边界，逆操作不应再因边界失败。 */
		WARN_ON_ONCE(!pageblock_unisolate_and_move_free_pages(zone, page));
	} else {
		/* 清标志先于 putback，使 buddy free 路径按恢复后的 migratetype 合并。 */
		clear_pageblock_isolate(page);
		__putback_isolated_page(page, order, get_pageblock_migratetype(page));
	}
	/* 计数是撤销的最后发布点，此后锁外观察者看到 block 已完全恢复。 */
	zone->nr_isolate_pageblock--;
}

/*
 * __first_valid_page() - 在 PFN 窗口中找到首个 online struct page。
 * @pfn 为起点，@nr_pages 为页数；返回未加引用的 page，全部离线/洞返回 NULL。
 * 调用者须用 memory hotplug 稳定性保护结果；本函数只线性探测，无副作用。
 */
static inline struct page *
__first_valid_page(unsigned long pfn, unsigned long nr_pages)
{
	int i;

	/* pfn_to_online_page 同时过滤无 memmap 和 offline section。 */
	for (i = 0; i < nr_pages; i++) {
		struct page *page;

		page = pfn_to_online_page(pfn + i);
		if (!page)
			continue;
		return page;
	}
	return NULL;
}

/**
 * isolate_single_pageblock() -- tries to isolate a pageblock that might be
 * within a free or in-use page.
 * @boundary_pfn:		pageblock-aligned pfn that a page might cross
 * @mode:			isolation mode
 * @isolate_before:	isolate the pageblock before the boundary_pfn
 * @skip_isolation:	the flag to skip the pageblock isolation in second
 *			isolate_single_pageblock()
 *
 * Free and in-use pages can be as big as MAX_PAGE_ORDER and contain more than one
 * pageblock. When not all pageblocks within a page are isolated at the same
 * time, free page accounting can go wrong. For example, in the case of
 * MAX_PAGE_ORDER = pageblock_order + 1, a MAX_PAGE_ORDER page has two
 * pageblocks.
 * [      MAX_PAGE_ORDER         ]
 * [  pageblock0  |  pageblock1  ]
 * When either pageblock is isolated, if it is a free page, the page is not
 * split into separate migratetype lists, which is supposed to; if it is an
 * in-use page and freed later, __free_one_page() does not split the free page
 * either. The function handles this by splitting the free page or migrating
 * the in-use page then splitting the free page.
 */
/*
 * 中文学习补充：@boundary_pfn pageblock 对齐，@isolate_before 选择边界左/右块，
 * @skip_isolation 用于首尾其实是同一块的第二次调用。成功 0，无法处理跨边界
 * compound 返回 -EBUSY。函数先发布目标 block isolate，再从所属 MAX_ORDER
 * 对齐窗口扫描可能跨界的高阶页；free page 已由 helper 拆/搬，无法安全处理的
 * 在 failed 撤销本次发布。调用于可睡眠 hotplug/contig 分配路径。
 */
static int isolate_single_pageblock(unsigned long boundary_pfn,
			enum pb_isolate_mode mode, bool isolate_before,
			bool skip_isolation)
{
	unsigned long start_pfn;
	unsigned long isolate_pageblock;
	unsigned long pfn;
	struct zone *zone;
	int ret;

	/* 边界不对齐会错误选择左右 block，是调用者编程错误。 */
	VM_BUG_ON(!pageblock_aligned(boundary_pfn));

	/* 第一阶段确定要隔离的 block：boundary 左侧或以 boundary 起始的右侧。 */
	if (isolate_before)
		isolate_pageblock = boundary_pfn - pageblock_nr_pages;
	else
		isolate_pageblock = boundary_pfn;

	/*
	 * scan at the beginning of MAX_ORDER_NR_PAGES aligned range to avoid
	 * only isolating a subset of pageblocks from a bigger than pageblock
	 * free or in-use page. Also make sure all to-be-isolated pageblocks
	 * are within the same zone.
	 */
	/* 中文翻译：从 MAX_ORDER 对齐起点扫描，避免只隔离跨多 block 高阶页的一部分，并裁到 zone 起点。 */
	zone  = page_zone(pfn_to_page(isolate_pageblock));
	start_pfn  = max(ALIGN_DOWN(isolate_pageblock, MAX_ORDER_NR_PAGES),
				      zone->zone_start_pfn);

	/* 首尾同块时第二次只验证状态；否则调用 set 发布 isolate。 */
	if (skip_isolation) {
		VM_BUG_ON(!get_pageblock_isolate(pfn_to_page(isolate_pageblock)));
	} else {
		ret = set_migratetype_isolate(pfn_to_page(isolate_pageblock),
				mode, isolate_pageblock,
				isolate_pageblock + pageblock_nr_pages);

		if (ret)
			return ret;
	}

	/*
	 * Bail out early when the to-be-isolated pageblock does not form
	 * a free or in-use page across boundary_pfn:
	 *
	 * 1. isolate before boundary_pfn: the page after is not online
	 * 2. isolate after boundary_pfn: the page before is not online
	 *
	 * This also ensures correctness. Without it, when isolate after
	 * boundary_pfn and [start_pfn, boundary_pfn) are not online,
	 * __first_valid_page() will return unexpected NULL in the for loop
	 * below.
	 */
	/*
	 * 中文翻译：相邻侧不 online 说明没有页能跨 boundary，可提前成功；右侧隔离时
	 * 该检查还保证下方 __first_valid_page 不会意外得到 NULL。
	 */
	if (isolate_before) {
		if (!pfn_to_online_page(boundary_pfn))
			return 0;
	} else {
		if (!pfn_to_online_page(boundary_pfn - 1))
			return 0;
	}

	/* 第二阶段扫描 boundary 之前的 MAX_ORDER 窗口，寻找跨进目标 block 的页。 */
	for (pfn = start_pfn; pfn < boundary_pfn;) {
		struct page *page = __first_valid_page(pfn, boundary_pfn - pfn);

		VM_BUG_ON(!page);
		pfn = page_to_pfn(page);

		/* unaccepted 范围尚未进入 buddy，按最大阶窗口跳过且不触碰内容。 */
		if (PageUnaccepted(page)) {
			pfn += MAX_ORDER_NR_PAGES;
			continue;
		}

		/* free 跨界页已由 pageblock isolate helper 处理，此处验证并按 order 跳过。 */
		if (PageBuddy(page)) {
			int order = buddy_order(page);

			/* pageblock_isolate_and_move_free_pages() handled this */
			/* 中文翻译：跨过 boundary 的 buddy 本应已被前面的 helper 拆开。 */
			VM_WARN_ON_ONCE(pfn + (1 << order) > boundary_pfn);

			pfn += 1UL << order;
			continue;
		}

		/*
		 * If a compound page is straddling our block, attempt
		 * to migrate it out of the way.
		 *
		 * We don't have to worry about this creating a large
		 * free page that straddles into our block: gigantic
		 * pages are freed as order-0 chunks, and LRU pages
		 * (currently) do not exceed pageblock_order.
		 *
		 * The block of interest has already been marked
		 * MIGRATE_ISOLATE above, so when migration is done it
		 * will free its pages onto the correct freelists.
		 */
		/*
		 * 中文翻译：跨目标 block 的在用 compound 需先迁走再拆；gigantic hugepage
		 * 按 order-0 释放，LRU 页目前不超过 pageblock_order，故不会生成新的跨界大块。
		 * 目标已标 ISOLATE，迁移释放会进入正确 freelist。
		 */
		if (PageCompound(page)) {
			/* head/order 都是无引用快照，外层 hotplug/隔离流程负责页生命周期。 */
			struct page *head = compound_head(page);
			unsigned long head_pfn = page_to_pfn(head);
			unsigned long nr_pages = compound_nr(head);

			/* 未跨界或 HugeTLB 有独立处理方式时按 compound 尾推进。 */
			if (head_pfn + nr_pages <= boundary_pfn ||
			    PageHuge(page)) {
				pfn = head_pfn + nr_pages;
				continue;
			}

			/*
			 * These pages are movable too, but they're
			 * not expected to exceed pageblock_order.
			 *
			 * Let us know when they do, so we can add
			 * proper free and split handling for them.
			 */
			/* 中文翻译：LRU/movable_ops 理论上不超 pageblock；若发生则告警并要求补专门拆分逻辑。 */
			VM_WARN_ON_ONCE_PAGE(PageLRU(page), page);
			VM_WARN_ON_ONCE_PAGE(page_has_movable_ops(page), page);

			goto failed;
		}

		pfn++;
	}
	return 0;
failed:
	/* restore the original migratetype */
	/* 中文翻译：失败时只撤销本次实际执行的隔离；复用旧隔离状态时不得误撤销。 */
	if (!skip_isolation)
		unset_migratetype_isolate(pfn_to_page(isolate_pageblock));
	return -EBUSY;
}

/**
 * start_isolate_page_range() - mark page range MIGRATE_ISOLATE
 * @start_pfn:		The first PFN of the range to be isolated.
 * @end_pfn:		The last PFN of the range to be isolated.
 * @mode:		isolation mode
 *
 * Making page-allocation-type to be MIGRATE_ISOLATE means free pages in
 * the range will never be allocated. Any free pages and pages freed in the
 * future will not be allocated again. If specified range includes migrate types
 * other than MOVABLE or CMA, this will fail with -EBUSY. For isolating all
 * pages in the range finally, the caller have to free all pages in the range.
 * test_page_isolated() can be used for test it.
 *
 * The function first tries to isolate the pageblocks at the beginning and end
 * of the range, since there might be pages across the range boundaries.
 * Afterwards, it isolates the rest of the range.
 *
 * There is no high level synchronization mechanism that prevents two threads
 * from trying to isolate overlapping ranges. If this happens, one thread
 * will notice pageblocks in the overlapping range already set to isolate.
 * This happens in set_migratetype_isolate, and set_migratetype_isolate
 * returns an error. We then clean up by restoring the migration type on
 * pageblocks we may have modified and return -EBUSY to caller. This
 * prevents two threads from simultaneously working on overlapping ranges.
 *
 * Please note that there is no strong synchronization with the page allocator
 * either. Pages might be freed while their page blocks are marked ISOLATED.
 * A call to drain_all_pages() after isolation can flush most of them. However
 * in some cases pages might still end up on pcp lists and that would allow
 * for their allocation even when they are in fact isolated already. Depending
 * on how strong of a guarantee the caller needs, zone_pcp_disable/enable()
 * might be used to flush and disable pcplist before isolation and enable after
 * unisolation.
 *
 * Return: 0 on success and -EBUSY if any part of range cannot be isolated.
 */
/*
 * 中文学习补充：本函数把用户范围向外扩到 pageblock；先隔离首尾以处理跨界
 * 高阶页，再处理中间块。成功只保证 allocator 不再从这些 block 取 buddy 页，
 * 不保证在用页已经迁走，调用者还需 drain/迁移并用 test_pages_isolated 验证。
 * 重叠请求在 set 时返回 -EBUSY，函数逆序恢复自己已改的块。与 PCP 没有强同步；
 * 需要严格保证的调用者应在外层 zone_pcp_disable/enable。
 */
int start_isolate_page_range(unsigned long start_pfn, unsigned long end_pfn,
			     enum pb_isolate_mode mode)
{
	unsigned long pfn;
	struct page *page;
	/* isolation is done at page block granularity */
	/* 中文翻译：隔离粒度是 pageblock，因此实际范围向下/向上对齐。 */
	unsigned long isolate_start = pageblock_start_pfn(start_pfn);
	unsigned long isolate_end = pageblock_align(end_pfn);
	int ret;
	bool skip_isolation = false;

	/* isolate [isolate_start, isolate_start + pageblock_nr_pages) pageblock */
	/* 中文翻译：阶段一先隔离起始 block，失败时尚无本函数资源需要回滚。 */
	ret = isolate_single_pageblock(isolate_start, mode, false,
			skip_isolation);
	if (ret)
		return ret;

	/* 单 pageblock 范围的“尾块”就是首块，第二次调用只复核而不重复隔离。 */
	if (isolate_start == isolate_end - pageblock_nr_pages)
		skip_isolation = true;

	/* isolate [isolate_end - pageblock_nr_pages, isolate_end) pageblock */
	/* 中文翻译：阶段二隔离末尾 block；失败需撤销已经成功的首块。 */
	ret = isolate_single_pageblock(isolate_end, mode, true, skip_isolation);
	if (ret) {
		/* 即使单块 skip 情形也只有首块由本函数拥有，统一恢复它。 */
		unset_migratetype_isolate(pfn_to_page(isolate_start));
		return ret;
	}

	/* skip isolated pageblocks at the beginning and end */
	/* 中文翻译：阶段三跳过已完成的首尾，只逐块处理内部范围。 */
	for (pfn = isolate_start + pageblock_nr_pages;
	     pfn < isolate_end - pageblock_nr_pages;
	     pfn += pageblock_nr_pages) {
		/* 全洞 block 无 struct page 可标记，视为空洞跳过。 */
		page = __first_valid_page(pfn, pageblock_nr_pages);
		if (page && set_migratetype_isolate(page, mode, start_pfn,
					end_pfn)) {
			/* 中间失败：先撤销 [start,pfn)，再单独撤销预先隔离的尾块。 */
			undo_isolate_page_range(isolate_start, pfn);
			unset_migratetype_isolate(
				pfn_to_page(isolate_end - pageblock_nr_pages));
			return -EBUSY;
		}
	}
	/* 所有 block 的 migratetype/计数均已发布；调用者现在可 drain 并迁移页。 */
	return 0;
}

/**
 * undo_isolate_page_range - undo effects of start_isolate_page_range()
 * @start_pfn:		The first PFN of the isolated range
 * @end_pfn:		The last PFN of the isolated range
 *
 * This finds and unsets every MIGRATE_ISOLATE page block in the given range
 */
/*
 * 中文翻译：查找给定范围中每个 MIGRATE_ISOLATE pageblock 并撤销。
 * @start_pfn/@end_pfn 是半开范围，函数按 pageblock 向外对齐；无返回值。调用于
 * 隔离失败回滚或 contig/hotplug 收尾，可睡眠外层但内部逐块拿 zone 自旋锁。
 * 它幂等跳过洞和非 isolate 块，不区分最初由谁隔离，调用者须保证范围 ownership。
 */
void undo_isolate_page_range(unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long pfn;
	struct page *page;
	unsigned long isolate_start = pageblock_start_pfn(start_pfn);
	unsigned long isolate_end = pageblock_align(end_pfn);

	/* 每块只取首个 online page 作为 pageblock/zone 代表。 */
	for (pfn = isolate_start;
	     pfn < isolate_end;
	     pfn += pageblock_nr_pages) {
		page = __first_valid_page(pfn, pageblock_nr_pages);
		/* 空洞和已由其他路径恢复的块都是安全的幂等跳过。 */
		if (!page || !is_migrate_isolate_page(page))
			continue;
		unset_migratetype_isolate(page);
	}
}
/*
 * Test all pages in the range is free(means isolated) or not.
 * all pages in [start_pfn...end_pfn) must be in the same zone.
 * zone->lock must be held before call this.
 *
 * Returns the last tested pfn.
 */
/*
 * 中文翻译：在已持 zone->lock 的单 zone 范围内验证每页为空闲/可忽略，返回首个
 * 阻塞 PFN；等于 @end_pfn 表示全部通过。@mode 为下线时额外接受 HWPoison，
 * 以及驱动已在 MEM_GOING_OFFLINE 把引用降为零的 PageOffline 页。
 */
static unsigned long
__test_page_isolated_in_pageblock(unsigned long pfn, unsigned long end_pfn,
				  enum pb_isolate_mode mode)
{
	struct page *page;

	/* buddy head 按 order 跨过整个空闲块，锁保证 order/free list 稳定。 */
	while (pfn < end_pfn) {
		page = pfn_to_page(pfn);
		if (PageBuddy(page))
			/*
			 * If the page is on a free list, it has to be on
			 * the correct MIGRATE_ISOLATE freelist. There is no
			 * simple way to verify that as VM_BUG_ON(), though.
			 */
			/* 中文翻译：在 free list 上理论上应属于 ISOLATE 链，但没有简单 VM_BUG 验证方法。 */
			pfn += 1 << buddy_order(page);
		else if ((mode == PB_ISOLATE_MODE_MEM_OFFLINE) &&
			 PageHWPoison(page))
			/* A HWPoisoned page cannot be also PageBuddy */
			/* 中文翻译：HWPoison 页不会同时是 buddy，下线模式按一页跳过。 */
			pfn++;
		else if ((mode == PB_ISOLATE_MODE_MEM_OFFLINE) &&
			 PageOffline(page) && !page_count(page))
			/*
			 * The responsible driver agreed to skip PageOffline()
			 * pages when offlining memory by dropping its
			 * reference in MEM_GOING_OFFLINE.
			 */
			/* 中文翻译：驱动通过在 MEM_GOING_OFFLINE 丢引用，同意下线时跳过 Offline 页。 */
			pfn++;
		else
			/* 第一个仍在用的普通页终止扫描，返回其 PFN 给 trace/调用者。 */
			break;
	}

	return pfn;
}

/**
 * test_pages_isolated - check if pageblocks in range are isolated
 * @start_pfn:		The first PFN of the isolated range
 * @end_pfn:		The first PFN *after* the isolated range
 * @mode:		Testing mode
 *
 * This tests if all in the specified range are free.
 *
 * If %PB_ISOLATE_MODE_MEM_OFFLINE specified in @mode, it will consider
 * poisoned and offlined pages free as well.
 *
 * Caller must ensure the requested range doesn't span zones.
 *
 * Returns 0 if true, -EBUSY if one or more pages are in use.
 */
/*
 * test_pages_isolated() - 权威验证隔离范围是否已无不可接受的在用页。
 * @start_pfn/@end_pfn 为同一 zone 的半开范围，@mode 控制下线特例；全通过返回 0，
 * block 未 isolate、范围无有效页或发现占用返回 -EBUSY。函数先等待延迟 HugeTLB
 * free，再无锁核对每个 block 的 migratetype，最后 zone->lock 下扫描 buddy/
 * 特例。无论成败都发 trace，pfn 标出通过终点或首个失败位置。
 */
int test_pages_isolated(unsigned long start_pfn, unsigned long end_pfn,
			enum pb_isolate_mode mode)
{
	unsigned long pfn, flags;
	struct page *page;
	struct zone *zone;
	int ret;

	/*
	 * Due to the deferred freeing of hugetlb folios, the hugepage folios may
	 * not immediately release to the buddy system. This can cause PageBuddy()
	 * to fail in __test_page_isolated_in_pageblock(). To ensure that the
	 * hugetlb folios are properly released back to the buddy system, we
	 * invoke the wait_for_freed_hugetlb_folios() function to wait for the
	 * release to complete.
	 */
	/* 中文翻译：HugeTLB folio 延迟释放会暂时看不到 PageBuddy，先等待其回归 buddy。 */
	wait_for_freed_hugetlb_folios();

	/*
	 * Note: pageblock_nr_pages != MAX_PAGE_ORDER. Then, chunks of free
	 * pages are not aligned to pageblock_nr_pages.
	 * Then we just check migratetype first.
	 */
	/* 中文翻译：pageblock 与 MAX_ORDER 不一定对齐，先逐 block 验证 migratetype。 */
	/* 阶段一拒绝任何尚未标 ISOLATE 的有效 block；空洞 block 本身可跳过。 */
	for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages) {
		page = __first_valid_page(pfn, pageblock_nr_pages);
		if (page && !is_migrate_isolate_page(page))
			break;
	}
	/* 取得 zone 代表页；全范围无 online page 无法满足测试契约，返回忙。 */
	page = __first_valid_page(start_pfn, end_pfn - start_pfn);
	if ((pfn < end_pfn) || !page) {
		ret = -EBUSY;
		goto out;
	}

	/* Check all pages are free or marked as ISOLATED */
	/* 中文翻译：阶段二在 zone 锁下确认所有页已空闲或属于下线允许的隔离特例。 */
	zone = page_zone(page);
	spin_lock_irqsave(&zone->lock, flags);
	pfn = __test_page_isolated_in_pageblock(start_pfn, end_pfn, mode);
	spin_unlock_irqrestore(&zone->lock, flags);

	/* helper 停在范围内表示首个占用页；走到 end 才是成功。 */
	ret = pfn < end_pfn ? -EBUSY : 0;

out:
	/* trace 是所有出口共有的观察点，不改变返回语义；失败 pfn 帮助定位阻塞处。 */
	trace_test_pages_isolated(start_pfn, end_pfn, pfn);

	return ret;
}
