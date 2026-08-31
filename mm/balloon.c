// SPDX-License-Identifier: GPL-2.0-only
/*
 * Common interface for implementing a memory balloon, including support
 * for migration of pages inflated in a memory balloon.
 *
 * Copyright (C) 2012, Red Hat, Inc.  Rafael Aquini <aquini@redhat.com>
 */
/*
 * 内存气球驱动把 guest 页交给 host 时，页仍由 Linux page allocator 管理却暂时不能
 * 分配。本文件统一维护“已膨胀页”链表、vmstat/managed_pages 计账，并在启用迁移时
 * 把 PageOffline 页接入 movable_ops，使 compaction 能换出其物理位置而不缩小气球。
 */
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/balloon.h>

/*
 * Lock protecting the balloon_dev_info of all devices. We don't really
 * expect more than one device.
 */
/*
 * balloon_pages_lock 保护所有设备的 pages 链、isolated_pages 以及 page->private 的
 * 发布/清除。实现虽然通常只有一个气球设备，仍用单把全局锁让无锁 compaction 扫描
 * 与驱动 inflate/deflate 在同一串行点复核页状态；irqsave 允许这些入口来自中断可达路径。
 */
static DEFINE_SPINLOCK(balloon_pages_lock);

/**
 * balloon_page_insert - insert a page into the balloon's page list and make
 *			 the page->private assignment accordingly.
 * @balloon : pointer to balloon device
 * @page    : page to be assigned as a 'balloon page'
 *
 * Caller must ensure the balloon_pages_lock is held.
 */
/*
 * 把单页发布为某设备的 balloon 页，并建立迁移所需的反向关联。
 * 业务背景：驱动分配普通页后必须先完成 PageOffline/movable/private 初始化，才能把
 * 链表节点发布给 dequeue 或 compaction。调用者是 enqueue 与成功迁移路径。
 * 入参：@balloon 是借用且已初始化的设备描述符；@page 是驱动持有引用的输入输出页，
 * 其 lru 节点必须未挂在其他链表上，且尚未对 guest 最终移除。
 * 出参/返回：无直接返回；page 被标为 Offline，迁移配置下设置 MovableOps 并令 private
 * 借用指向 @balloon，最后加入 balloon->pages；页引用 ownership 不转移。
 * 注意事项：必须持 balloon_pages_lock、不可睡眠；链表插入是并发可见的发布边界，
 * 所有身份字段必须先写完。关闭迁移时不使用 private/MovableOps。
 */
static void balloon_page_insert(struct balloon_dev_info *balloon,
				       struct page *page)
{
	lockdep_assert_held(&balloon_pages_lock);
	/* PageOffline 让普通分配/热插拔路径识别该页暂由外部气球协议占用。 */
	__SetPageOffline(page);
	/* 迁移分派由 Offline 页类型与 MovableOps 位确定，private 定位负责设备。 */
	if (IS_ENABLED(CONFIG_BALLOON_MIGRATION)) {
		SetPageMovableOps(page);
		set_page_private(page, (unsigned long)balloon);
	}
	/* 身份元数据已完整，现在链入设备账本供 deflate/isolate 查找。 */
	list_add(&page->lru, &balloon->pages);
}

/**
 * balloon_page_finalize - prepare a balloon page that was removed from the
 *			   balloon list for release to the page allocator
 * @page: page to be released to the page allocator
 *
 * Caller must ensure the balloon_pages_lock is held.
 */
/*
 * 清理已从气球链摘除的页，使驱动随后可把它归还 page allocator。
 * 业务背景：deflate 和迁移源页都先摘链，再调用本函数撤销“属于某设备”的身份。
 * 入参：@page 是借用的输入输出页，调用者仍持有其引用；页必须已从 balloon 链摘除。
 * 出参/返回：无直接返回；迁移配置下清零 private，阻止新的 isolate 找到设备。
 * 注意事项：必须持 balloon_pages_lock、不可睡眠。PageOffline 故意保持到页真正进入
 * buddy 才清除，避免清身份与最终 free 之间被误认成普通在线页。
 */
static void balloon_page_finalize(struct page *page)
{
	lockdep_assert_held(&balloon_pages_lock);
	if (IS_ENABLED(CONFIG_BALLOON_MIGRATION))
		set_page_private(page, 0);
	/* PageOffline is sticky until the page is freed to the buddy. */
	/* PageOffline 会一直保留到页被释放进 buddy；这里只撤销可隔离的设备身份。 */
}

/*
 * 在持锁批处理中登记一页并同步全局内存计账。
 * 业务背景：单页与链表 enqueue 共用此核心，保证身份发布、managed_pages 调整和 vmstat
 * 事件顺序一致。
 * 入参：@b_dev_info 是借用的目标设备；@page 是借用的输入输出页，引用仍归驱动。
 * 出参/返回：无直接返回；页加入设备链，按策略从 zone managed_pages 减一，并增加
 * BALLOON_INFLATE 与 NR_BALLOON_PAGES。
 * 注意事项：调用者必须持 balloon_pages_lock、不可睡眠，并确保该页只登记一次。
 */
static void balloon_page_enqueue_one(struct balloon_dev_info *b_dev_info,
				     struct page *page)
{
	/* 先发布页身份，再把它从 guest 可管理容量和统计中扣除。 */
	balloon_page_insert(b_dev_info, page);
	if (b_dev_info->adjust_managed_page_count)
		adjust_managed_page_count(page, -1);
	__count_vm_event(BALLOON_INFLATE);
	inc_node_page_state(page, NR_BALLOON_PAGES);
}

/**
 * balloon_page_list_enqueue() - inserts a list of pages into the balloon page
 *				 list.
 * @b_dev_info: balloon device descriptor where we will insert a new page to
 * @pages: pages to enqueue - allocated using balloon_page_alloc.
 *
 * Driver must call this function to properly enqueue balloon pages before
 * definitively removing them from the guest system.
 *
 * Return: number of pages that were enqueued.
 */
/*
 * 把调用者链表中的一批已分配页转入设备 balloon 链。
 * 业务背景：批量 balloon 驱动在真正通知 host/从 guest 撤走页前，用本函数统一完成
 * page 标志、设备归属和内存统计；主要调用者包括 VMware balloon 批处理路径。
 * 入参：@b_dev_info 是借用且已初始化的目标设备；@pages 是借用的输入输出链表头，
 * 其中每页由 balloon_page_alloc() 获得、lru 仅用于此链，页引用 ownership 仍归驱动。
 * 出参/返回：返回实际转移页数；成功后输入链表为空，所有页位于 b_dev_info->pages，
 * 每页已完成 inflate 计账，不存在部分失败返回。
 * 注意事项：函数用 irqsave 全局锁串行所有设备与 compaction，持锁期间不睡眠；调用者
 * 不得在 enqueue 后继续把 page->lru 用作别的链表节点，直至 dequeue。
 */
size_t balloon_page_list_enqueue(struct balloon_dev_info *b_dev_info,
				 struct list_head *pages)
{
	/* page 是当前转移项，tmp 预存下一项，允许循环体安全删除 page->lru。 */
	struct page *page, *tmp;
	/* flags 保存本 CPU 中断状态；n_pages 是已提交的页数。 */
	unsigned long flags;
	size_t n_pages = 0;

	/* 阶段 1：锁内逐页从调用者链摘除并发布到设备链，避免任一页同时属于两条链。 */
	spin_lock_irqsave(&balloon_pages_lock, flags);
	list_for_each_entry_safe(page, tmp, pages, lru) {
		list_del(&page->lru);
		balloon_page_enqueue_one(b_dev_info, page);
		n_pages++;
	}
	spin_unlock_irqrestore(&balloon_pages_lock, flags);
	/* 锁外返回完整转移数；没有分配或可失败 helper，故等于入口链表长度。 */
	return n_pages;
}
EXPORT_SYMBOL_GPL(balloon_page_list_enqueue);

/**
 * balloon_page_list_dequeue() - removes pages from balloon's page list and
 *				 returns a list of the pages.
 * @b_dev_info: balloon device descriptor where we will grab a page from.
 * @pages: pointer to the list of pages that would be returned to the caller.
 * @n_req_pages: number of requested pages.
 *
 * Driver must call this function to properly de-allocate a previous enlisted
 * balloon pages before definitively releasing it back to the guest system.
 * This function tries to remove @n_req_pages from the ballooned pages and
 * return them to the caller in the @pages list.
 *
 * Note that this function may fail to dequeue some pages even if the balloon
 * isn't empty - since the page list can be temporarily empty due to compaction
 * of isolated pages.
 *
 * Return: number of pages that were added to the @pages list.
 */
/*
 * 尝试从设备气球中摘出至多 @n_req_pages 页，并转交给调用者释放。
 * 业务背景：deflate 必须先撤销 page->private/设备链归属和统计，再让驱动把页交回
 * guest allocator；隔离中的迁移页暂不在链上，因此本次允许短取。
 * 入参：@b_dev_info 是借用的源设备；@pages 是借用的输出链表头，调用前必须已初始化；
 * @n_req_pages 是最大请求页数，可为 0。页引用从驱动的 balloon 账本逻辑转回调用者。
 * 出参/返回：返回实际加入 @pages 的页数，范围 0..@n_req_pages；每个返回页已清 private、
 * 完成 deflate/vmstat/managed_pages 计账，但仍保持 PageOffline 直到调用者 free。
 * 注意事项：全局 irqsave 锁保护摘链与身份清除；少于请求量不是错误，可能只是页正在
 * compaction 隔离。函数不等待隔离页归队，也不释放返回页。
 */
size_t balloon_page_list_dequeue(struct balloon_dev_info *b_dev_info,
				 struct list_head *pages, size_t n_req_pages)
{
	/* safe 迭代允许把 page 从设备链移动到输出链；tmp 保存下一项。 */
	struct page *page, *tmp;
	/* flags 保存中断状态，n_pages 跟踪已成功转交的页数。 */
	unsigned long flags;
	size_t n_pages = 0;

	/* 阶段 1：在同一锁域内摘链、恢复容量、撤销设备身份并完成统计提交。 */
	spin_lock_irqsave(&balloon_pages_lock, flags);
	list_for_each_entry_safe(page, tmp, &b_dev_info->pages, lru) {
		if (n_pages == n_req_pages)
			break;
		/* 从设备链摘除后，新 isolate 已不能通过链表选中该页。 */
		list_del(&page->lru);
		if (b_dev_info->adjust_managed_page_count)
			adjust_managed_page_count(page, 1);
		balloon_page_finalize(page);
		__count_vm_event(BALLOON_DEFLATE);
		list_add(&page->lru, pages);
		/* 输出链发布完成后再减少 balloon 页统计，使账本与 ownership 同步收敛。 */
		dec_node_page_state(page, NR_BALLOON_PAGES);
		n_pages++;
	}
	spin_unlock_irqrestore(&balloon_pages_lock, flags);

	/* 0 或短取均由调用者结合 isolated_pages 决定重试时机。 */
	return n_pages;
}
EXPORT_SYMBOL_GPL(balloon_page_list_dequeue);

/**
 * balloon_page_alloc - allocates a new page for insertion into the balloon
 *			page list.
 *
 * Driver must call this function to properly allocate a new balloon page.
 * Driver must call balloon_page_enqueue before definitively removing the page
 * from the guest system.
 *
 * Return: struct page for the allocated page or NULL on allocation failure.
 */
/*
 * 为下一次 inflate 分配一张符合气球迁移策略的页。
 * 业务背景：驱动不能任取救急保留页，否则 host 回收压力可能耗尽内核前进保障；本函数
 * 位于驱动挑页与 balloon_page_enqueue() 之间，只分配，尚不发布为 balloon 页。
 * 入参：无。出参/返回：成功返回一张引用归调用者的新页，失败返回 NULL；页未置
 * PageOffline、未入任何 balloon 链，失败无副作用。
 * 注意事项：分配可能睡眠但不进行激进重试、不发告警且不使用内存保留；迁移配置选择
 * HIGHUSER_MOVABLE，关闭时选择普通 HIGHUSER。调用者必须最终 enqueue 或 put_page/free。
 */
struct page *balloon_page_alloc(void)
{
	/* 基础 flags 禁止动用 emergency reserve，并让失败快速、安静地返回驱动。 */
	gfp_t gfp_flags = __GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN;

	/* movable 页便于 compaction 搬迁；无迁移支持时无需限制到 movable zone。 */
	if (IS_ENABLED(CONFIG_BALLOON_MIGRATION))
		gfp_flags |= GFP_HIGHUSER_MOVABLE;
	else
		gfp_flags |= GFP_HIGHUSER;

	/* 返回的单页仍是普通 allocator 页，ownership 完全属于调用者。 */
	return alloc_page(gfp_flags);
}
EXPORT_SYMBOL_GPL(balloon_page_alloc);

/**
 * balloon_page_enqueue - inserts a new page into the balloon page list.
 *
 * @b_dev_info: balloon device descriptor where we will insert a new page
 * @page: new page to enqueue - allocated using balloon_page_alloc.
 *
 * Drivers must call this function to properly enqueue a new allocated balloon
 * page before definitively removing the page from the guest system.
 *
 * Drivers must not enqueue pages while page->lru is still in
 * use, and must not use page->lru until a page was unqueued again.
 */
/*
 * 把一张新分配页登记为设备的 balloon 页。
 * 业务背景：这是单页驱动（如 virtio balloon）的 inflate 提交入口；只有本函数完成后，
 * 驱动才能最终通知 host 该 guest 页已让出。
 * 入参：@b_dev_info 是借用的目标设备；@page 是 balloon_page_alloc() 返回且由调用者持有
 * 引用的输入输出页，page->lru 必须空闲。函数不接管页引用 ownership。
 * 出参/返回：无直接返回；页被加入设备链、发布设备身份并完成 inflate 统计。
 * 注意事项：内部 irqsave 锁与 dequeue/isolate 串行，不睡眠；重复入队或复用 lru 会破坏
 * 链表。调用者之后只能通过 dequeue 取回该页再释放。
 */
void balloon_page_enqueue(struct balloon_dev_info *b_dev_info,
			  struct page *page)
{
	/* flags 仅在本次临界区保存/恢复调用 CPU 的中断状态。 */
	unsigned long flags;

	/* 单页包装把全部状态转换委托给锁内核心，保证与批量入口完全同义。 */
	spin_lock_irqsave(&balloon_pages_lock, flags);
	balloon_page_enqueue_one(b_dev_info, page);
	spin_unlock_irqrestore(&balloon_pages_lock, flags);
}
EXPORT_SYMBOL_GPL(balloon_page_enqueue);

/**
 * balloon_page_dequeue - removes a page from balloon's page list and returns
 *			  its address to allow the driver to release the page.
 * @b_dev_info: balloon device descriptor where we will grab a page from.
 *
 * Driver must call this function to properly dequeue a previously enqueued page
 * before definitively releasing it back to the guest system.
 *
 * Caller must perform its own accounting to ensure that this
 * function is called only if some pages are actually enqueued.
 *
 * Note that this function may fail to dequeue some pages even if there are
 * some enqueued pages - since the page list can be temporarily empty due to
 * the compaction of isolated pages.
 *
 * TODO: remove the caller accounting requirements, and allow caller to wait
 * until all pages can be dequeued.
 *
 * Return: struct page for the dequeued page, or NULL if no page was dequeued.
 */
/*
 * 尝试取回一张 balloon 页供驱动归还 guest。
 * 业务背景：单页 deflate 驱动反复调用本入口；它复用批量 dequeue，并额外检查“账面有页
 * 却永久无页可取”的不变量，防止驱动释放循环无穷等待。
 * 入参：@b_dev_info 是借用设备，调用者须通过自身目标计数保证理论上至少有一页可取。
 * 出参/返回：成功返回一张由调用者负责释放的页；暂时所有页都被迁移隔离时返回 NULL；
 * 返回页已完成 deflate 计账和 private 清除，PageOffline 到最终 free 才消失。
 * 注意事项：可能因 compaction 短暂失败且不等待；若设备链为空且 isolated_pages=0，
 * 说明驱动计数或链表丢页，BUG() 终止而不是让调用者死循环。原 TODO 希望未来移除
 * 调用者预先计数的要求，并允许调用者等待到全部目标页均可 dequeue。
 */
struct page *balloon_page_dequeue(struct balloon_dev_info *b_dev_info)
{
	/* flags 只用于失败复核锁；pages 接收至多一页，n_pages 保存实际数量。 */
	unsigned long flags;
	LIST_HEAD(pages);
	int n_pages;

	/* 阶段 1：批量核心原子完成摘链、身份撤销和计账。 */
	n_pages = balloon_page_list_dequeue(b_dev_info, &pages, 1);

	/* 阶段 2：短取时区分合法的“正在隔离”与不可恢复的账本丢失。 */
	if (n_pages != 1) {
		/*
		 * If we are unable to dequeue a balloon page because the page
		 * list is empty and there are no isolated pages, then something
		 * went out of track and some balloon pages are lost.
		 * BUG() here, otherwise the balloon driver may get stuck in
		 * an infinite loop while attempting to release all its pages.
		 */
		/*
		 * 若链表为空但仍有 isolated_pages，迁移稍后会 putback 或完成，返回 NULL
		 * 让驱动重试；两者都为零则声称存在的 balloon 页已经无处可寻。
		 */
		spin_lock_irqsave(&balloon_pages_lock, flags);
		if (unlikely(list_empty(&b_dev_info->pages) &&
			     !b_dev_info->isolated_pages))
			BUG();
		spin_unlock_irqrestore(&balloon_pages_lock, flags);
		return NULL;
	}
	/* 输出链恰有一项；取首项不会转移或增加额外引用，ownership 已由核心交给调用者。 */
	return list_first_entry(&pages, struct page, lru);
}
EXPORT_SYMBOL_GPL(balloon_page_dequeue);

#ifdef CONFIG_BALLOON_MIGRATION
/*
 * 从 PageOffline 页的 private 字段恢复负责它的 balloon 设备。
 * 业务背景：movable_ops 回调只有 page 参数，inflate 时保存的反向指针补回设备上下文。
 * 入参：@page 是借用页；出参/返回：返回借用设备指针，未发布或已 deflate 时为 NULL。
 * 注意事项：private 的并发稳定性由 balloon_pages_lock 或已隔离状态保证；不增加引用。
 */
static struct balloon_dev_info *balloon_page_device(struct page *page)
{
	return (struct balloon_dev_info *)page_private(page);
}

/*
 * 为 compaction 从设备 balloon 链隔离一页。
 * 业务背景：isolate_movable_ops_page() 已取得页引用/页锁后调用此回调；本函数再与并发
 * deflate 在 balloon_pages_lock 下竞争，成功后页只属于迁移流程，不再可被驱动 dequeue。
 * 入参：@page 是借用的候选 balloon 页；@mode 是迁移核心给出的隔离模式，本实现无需
 * 区分其取值。出参/返回：成功摘链、isolated_pages++ 并返回 true；页已 deflate 则
 * 返回 false，链表与计数不变。
 * 注意事项：private 非 NULL 与“在设备链或已隔离”构成不变量；函数使用 irqsave 锁、
 * 不睡眠，不清 private，因而成功隔离后设备在 migrate/putback 前仍可定位。
 */
static bool balloon_page_isolate(struct page *page, isolate_mode_t mode)

{
	/* b_dev_info 在锁内从 private 读取；flags 保存中断状态。@mode 有意未使用。 */
	struct balloon_dev_info *b_dev_info;
	unsigned long flags;

	/* 阶段 1：锁内复核 deflate 是否已抢先清除 private。 */
	spin_lock_irqsave(&balloon_pages_lock, flags);
	b_dev_info = balloon_page_device(page);
	if (!b_dev_info) {
		/*
		 * The page already got deflated and removed from the
		 * balloon list.
		 */
		/* 页已被 deflate 并从气球链摘除，隔离失败且不能再触碰其 lru。 */
		spin_unlock_irqrestore(&balloon_pages_lock, flags);
		return false;
	}
	/* 阶段 2：摘链并记入隔离计数；页锁/引用由迁移核心维持至后续回调。 */
	list_del(&page->lru);
	b_dev_info->isolated_pages++;
	spin_unlock_irqrestore(&balloon_pages_lock, flags);

	return true;
}

/*
 * 把迁移未完成的隔离页重新挂回原设备 balloon 链。
 * 业务背景：迁移核心放弃或回滚时通过 putback_movable_ops_page() 调用；页仍保持
 * PageOffline/private，因此不会被驱动当作已 deflate 页释放。
 * 入参：@page 是借用、已隔离且页锁保护的 balloon 页。出参/返回：无直接返回；页重新
 * 加入设备链并 isolated_pages--，引用由迁移核心随后释放。
 * 注意事项：设备指针缺失违反隔离不变量，只 WARN 后返回以避免 NULL 解引用；锁内不睡眠。
 */
static void balloon_page_putback(struct page *page)
{
	/* private 在隔离期必须稳定；flags 用于链表/计数临界区。 */
	struct balloon_dev_info *b_dev_info = balloon_page_device(page);
	unsigned long flags;

	/*
	 * When we isolated the page, the page was still inflated in a balloon
	 * device. As isolated balloon pages cannot get deflated, we still have
	 * a balloon device here.
	 */
	/*
	 * 隔离时该页仍处于 inflated 状态；隔离页不能被 dequeue，所以 private 所指设备
	 * 理应一直存在。若为空，记录一次不变量破坏，无法安全选择回挂链表。
	 */
	if (WARN_ON_ONCE(!b_dev_info))
		return;

	/* 回挂与隔离计数递减必须原子可见，避免 dequeue 错判“无链表页也无隔离页”。 */
	spin_lock_irqsave(&balloon_pages_lock, flags);
	list_add(&page->lru, &b_dev_info->pages);
	b_dev_info->isolated_pages--;
	spin_unlock_irqrestore(&balloon_pages_lock, flags);
}

/*
 * 完成一张已隔离 balloon 页向新物理页的设备级迁移或退化 deflate。
 * 业务背景：迁移核心持有源/目标页并调用设备 migratepage 回调通知 host；通用层随后
 * 提交 Linux 侧链表、计账与引用变化，使气球大小与 hypervisor 已接受的结果一致。
 * 入参：@newpage 是迁移核心持有的目标页；@page 是已隔离源页；两者均为借用输入输出；
 * @mode 透传给设备，表示同步/异步迁移策略。@page 的 private 必须指向有效设备。
 * 出参/返回：设备返回 0 时新页接替源页并返回 0；返回 -ENOENT 表示源页已 deflate、
 * 新页未 inflate，也作为已完成返回 0；其他负 errno 原样返回，ownership/隔离状态不变。
 * 注意事项：设备回调可睡眠且在本文件锁外执行；成功提交段持 irqsave 锁。新页接替时
 * 本层额外 get_page() 交给 balloon 账本，源页最后 put_page() 释放隔离引用。
 */
static int balloon_page_migrate(struct page *newpage, struct page *page,
		enum migrate_mode mode)
{
	/* b_dev_info 从隔离期稳定的 private 借用；rc 是设备提交结果。 */
	struct balloon_dev_info *b_dev_info = balloon_page_device(page);
	unsigned long flags;
	int rc;

	/*
	 * When we isolated the page, the page was still inflated in a balloon
	 * device. As isolated balloon pages cannot get deflated, we still have
	 * a balloon device here.
	 */
	/* 隔离页不可被 deflate，故 private 丢失只可能是不变量破坏；让迁移稍后重试。 */
	if (WARN_ON_ONCE(!b_dev_info))
		return -EAGAIN;

	/* 阶段 1：锁外与 hypervisor 交换页；驱动自行串行其设备协议并可按 mode 退避。 */
	rc = b_dev_info->migratepage(b_dev_info, newpage, page, mode);
	/* 普通失败保留源页隔离状态，交给迁移核心重试或 putback；-ENOENT 是已提交退化。 */
	if (rc < 0 && rc != -ENOENT)
		return rc;

	/* 阶段 2：设备结果已不可回滚，在通用锁下提交 Linux 侧账本。 */
	spin_lock_irqsave(&balloon_pages_lock, flags);
	if (!rc) {
		/* Insert the new page into the balloon list. */
		/* 新页已被 host 接纳：取得 balloon 持有引用，再完整发布到设备链。 */
		get_page(newpage);
		balloon_page_insert(b_dev_info, newpage);
		__count_vm_event(BALLOON_MIGRATE);

		if (b_dev_info->adjust_managed_page_count &&
		    page_zone(page) != page_zone(newpage)) {
			/*
			 * When we migrate a page to a different zone we
			 * have to fixup the count of both involved zones.
			 */
			/*
			 * 跨 zone 迁移时旧 zone 重新获得一个 managed page，新 zone 失去一个；
			 * 同 zone 则净变化为零，无需触碰计数。
			 */
			adjust_managed_page_count(page, 1);
			adjust_managed_page_count(newpage, -1);
		}
	} else {
		/* Old page was deflated but new page not inflated. */
		/* -ENOENT 已让气球缩小：目标页仍归迁移核心，源页恢复为 guest 可管理容量。 */
		__count_vm_event(BALLOON_DEFLATE);

		if (b_dev_info->adjust_managed_page_count)
			adjust_managed_page_count(page, 1);
	}

	/* 两种成功结果都结束源页隔离，故统一减少设备隔离计数。 */
	b_dev_info->isolated_pages--;

	/* Free the now-deflated page we isolated in balloon_page_isolate(). */
	/* 源页已从 host 气球释放；清 private，PageOffline 留待最后 put_page() 进入 buddy。 */
	balloon_page_finalize(page);
	spin_unlock_irqrestore(&balloon_pages_lock, flags);

	/* 锁外释放隔离时取得的源页引用，可能在这里真正归还 allocator。 */
	put_page(page);

	return 0;
}

/*
 * balloon_mops 是所有 PageOffline balloon 页共享且终生只读的迁移操作表；页类型分派
 * 找到它后，按 isolate → migrate 或 putback 的协议调用。表不持有设备/page 引用，
 * 具体设备由每页 private 恢复，三个回调的状态由页锁和 balloon_pages_lock 衔接。
 */
static const struct movable_operations balloon_mops = {
	/* 迁移核心按“迁移、隔离、回放”三阶段调用，所有成员仅在本配置下注册。 */
	.migrate_page = balloon_page_migrate,
	.isolate_page = balloon_page_isolate,
	.putback_page = balloon_page_putback,
};

/*
 * 在 core initcall 阶段注册 PageOffline 类型的 movable_ops 分派。
 * 业务背景：compaction 只能由页类型找到全局回调表；注册必须早于 balloon 驱动产生
 * MovableOps 页。入参：无。出参/返回：0 成功，重复注册返回 -EBUSY，非法类型不可能；
 * 无对象 ownership 转移，表为静态常量且终生有效。初始化串行，不需要锁，可睡眠性无要求。
 */
static int __init balloon_init(void)
{
	/* PGTY_offline 将 PageOffline+MovableOps 页唯一分派到 balloon_mops。 */
	return set_movable_ops(&balloon_mops, PGTY_offline);
}
core_initcall(balloon_init);

#endif /* CONFIG_BALLOON_MIGRATION */
/* 关闭迁移配置时不编译回调和注册，balloon 页仍可 inflate/deflate，但不会被 compaction 搬迁。 */
