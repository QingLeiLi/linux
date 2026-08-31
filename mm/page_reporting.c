// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/page_reporting.h>
#include <linux/gfp.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/scatterlist.h>

#include "page_reporting.h"
#include "internal.h"

/* Initialize to an unsupported value */
/* 以无符号 -1 初始化为“未指定”；注册时再由模块参数、驱动或 pageblock_order 决定。 */
unsigned int page_reporting_order = PAGE_REPORTING_ORDER_UNSPECIFIED;

/*
 * page_order_update_notify() - 校验并提交 page_reporting_order 模块参数。
 * @val 是借用文本，@kp 描述目标 unsigned int；成功返回 0，非法或超出
 * [0, MAX_PAGE_ORDER] 返回负 errno。不会自行改成默认值；默认回退发生在 register。
 * 参数可由 sysfs 0644 节点运行期写入，调用方负责参数框架串行化。
 */
static int page_order_update_notify(const char *val, const struct kernel_param *kp)
{
	/*
	 * If param is set beyond this limit, order is set to default
	 * pageblock_order value
	 */
	/* 原说明中的“超界设默认”按当前代码应理解为超界拒绝；仅未指定值在注册时回退。 */
	return  param_set_uint_minmax(val, kp, 0, MAX_PAGE_ORDER);
}

/* 参数写入走有界 uint 解析，读取故意按 int 显示初始 UINT_MAX 为 -1。 */
static const struct kernel_param_ops page_reporting_param_ops = {
	.set = &page_order_update_notify,
	/*
	 * For the get op, use param_get_int instead of param_get_uint.
	 * This is to make sure that when unset the initialized value of
	 * -1 is shown correctly
	 */
	/* get 使用 int 而非 uint，确保未设置时把初始化的 -1 正确显示出来。 */
	.get = &param_get_int,
};

module_param_cb(page_reporting_order, &page_reporting_param_ops,
			&page_reporting_order, 0644);
MODULE_PARM_DESC(page_reporting_order, "Set page reporting order");

/*
 * This symbol is also a kernel parameter. Export the page_reporting_order
 * symbol so that other drivers can access it to control order values without
 * having to introduce another configurable parameter. Only one driver can
 * register with the page_reporting driver for the service, so we have just
 * one control parameter for the use case(which can be accessed in both
 * drivers)
 */
/*
 * 该符号同时是内核参数。导出它让唯一注册的服务驱动及协作驱动共用同一 order，
 * 无需再创建参数；系统一次只允许一个 page-reporting 设备注册。
 */
EXPORT_SYMBOL_GPL(page_reporting_order);

/* 合并释放事件两秒后再处理，让 scatterlist 更可能攒满并限制设备调用频率。 */
#define PAGE_REPORTING_DELAY	(2 * HZ)
/* 当前唯一设备由注册 mutex 写、通知热路径 RCU 读；注销先置 NULL 再等待 grace period。 */
static struct page_reporting_dev_info __rcu *pr_dev_info __read_mostly;

/* 原子状态机：IDLE 无工作，REQUESTED 表示需再跑一轮，ACTIVE 表示 worker 正扫描。 */
enum {
	PAGE_REPORTING_IDLE = 0,
	PAGE_REPORTING_REQUESTED,
	PAGE_REPORTING_ACTIVE
};

/* request page reporting */
/* 请求一次 page reporting；重复请求通过原子状态合并。 */
/*
 * __page_reporting_request() - 把释放事件并入设备的延迟工作状态机。
 * @prdev 是 RCU 或注册锁保护下的借用对象；无返回值。REQUESTED 直接合并；IDLE
 * 原子切到 REQUESTED 并排两秒工作；ACTIVE 被改为 REQUESTED，由当前 worker 收尾
 * 时续排。可从释放热路径调用，不睡眠，不取得设备 ownership。
 */
static void
__page_reporting_request(struct page_reporting_dev_info *prdev)
{
	unsigned int state;

	/* Check to see if we are in desired state */
	/* 已有待处理请求时无需再次写原子量或触碰 workqueue。 */
	state = atomic_read(&prdev->state);
	if (state == PAGE_REPORTING_REQUESTED)
		return;

	/*
	 * If reporting is already active there is nothing we need to do.
	 * Test against 0 as that represents PAGE_REPORTING_IDLE.
	 */
	/* ACTIVE 无需立即排第二份工作，只标 REQUESTED；0 即 IDLE 才负责首次排队。 */
	state = atomic_xchg(&prdev->state, PAGE_REPORTING_REQUESTED);
	if (state != PAGE_REPORTING_IDLE)
		return;

	/*
	 * Delay the start of work to allow a sizable queue to build. For
	 * now we are limiting this to running no more than once every
	 * couple of seconds.
	 */
	/* 延迟启动以积累可观队列，当前最多约每两秒开始一轮。 */
	schedule_delayed_work(&prdev->work, PAGE_REPORTING_DELAY);
}

/* notify prdev of free page reporting request */
/* 通知当前设备：伙伴系统出现了符合阈值的新空闲块。 */
/*
 * __page_reporting_notify() - 从页释放热路径安全取得设备并请求异步扫描。
 * 无参数/返回值；RCU 下借用全局 prdev，注销窗口看到 NULL 就静默退出。
 * 仅做原子状态转换和 delayed_work 调度，不传递具体页、不等待 report 完成。
 */
void __page_reporting_notify(void)
{
	struct page_reporting_dev_info *prdev;

	/*
	 * We use RCU to protect the pr_dev_info pointer. In almost all
	 * cases this should be present, however in the unlikely case of
	 * a shutdown this will be NULL and we should exit.
	 */
	/* RCU 保护全局设备指针；正常存在，注销期置 NULL 后本次通知无副作用。 */
	rcu_read_lock();
	prdev = rcu_dereference(pr_dev_info);
	if (likely(prdev))
		__page_reporting_request(prdev);

	rcu_read_unlock();
}

/*
 * page_reporting_drain() - 把 scatterlist 中隔离页放回各自 buddy 链并更新标志。
 * @prdev 当前未使用；@sgl/@nents 是已填充表及容量，@reported 表示设备调用成功。
 * 无返回值。调用者必须持对应 zone->lock；每项 page ownership 从 reporting 临时
 * 隔离态交还 buddy。只有未与 buddy 合并且 order 未变的块才设置 PageReported，
 * 失败时全部放回但不置位；最后重置表供下一批复用。
 */
static void
page_reporting_drain(struct page_reporting_dev_info *prdev,
		     struct scatterlist *sgl, unsigned int nents, bool reported)
{
	struct scatterlist *sg = sgl;

	/*
	 * Drain the now reported pages back into their respective
	 * free lists/areas. We assume at least one page is populated.
	 */
	/* 将已处理页放回原 zone/free area；调用约定保证表中至少有一项。 */
	do {
		/* sg length 精确编码隔离时 order，migratetype 在放回前按当前 pageblock 重读。 */
		struct page *page = sg_page(sg);
		int mt = get_pageblock_migratetype(page);
		unsigned int order = get_order(sg->length);

		__putback_isolated_page(page, order, mt);

		/* If the pages were not reported due to error skip flagging */
		/* 设备失败只回收临时 ownership，不把未成功处理的块标成 reported。 */
		if (!reported)
			continue;

		/*
		 * If page was not commingled with another page we can
		 * consider the result to be "reported" since the page
		 * hasn't been modified, otherwise we will need to
		 * report on the new larger page when we make our way
		 * up to that higher order.
		 */
		/*
		 * 若放回时未与其他块混合且 order 保持不变，可确认该块内容仍是设备处理结果；
		 * 若合并成更大块，则留待之后以新 order 整体重新报告。
		 */
		if (PageBuddy(page) && buddy_order(page) == order)
			__SetPageReported(page);
	} while ((sg = sg_next(sg)));

	/* reinitialize scatterlist now that it is empty */
	/* 所有隔离页均已交回 buddy，重置链/终止标记以复用同一数组。 */
	sg_init_table(sgl, nents);
}

/*
 * The page reporting cycle consists of 4 stages, fill, report, drain, and
 * idle. We will cycle through the first 3 stages until we cannot obtain a
 * full scatterlist of pages, in that case we will switch to idle.
 */
/*
 * 一轮 page reporting 由 fill、report、drain、idle 四阶段组成；前三阶段循环，
 * 无法再得到满 scatterlist 时由上层报告余项并转入 idle。
 */
/*
 * page_reporting_cycle() - 扫描一个 zone/order/migratetype 空闲链并提交满批次。
 * @prdev 为活动设备，@zone 为借用 zone，@order/@mt 选择 free_area 链，@sgl 为
 * PAGE_REPORTING_CAPACITY 项数组，@offset 是反向填充游标且跨链保留。
 * 返回 0 或 report 回调错误。zone 锁下隔离页，锁外调用可睡眠设备回调，再加锁
 * drain；预算耗尽置 REQUESTED 留待下一轮。所有已提交页无论成功失败均归还 buddy。
 */
static int
page_reporting_cycle(struct page_reporting_dev_info *prdev, struct zone *zone,
		     unsigned int order, unsigned int mt,
		     struct scatterlist *sgl, unsigned int *offset)
{
	/* area/list 只在 zone 锁下解引用内容；page_len 是每个 sg 项的连续字节数。 */
	struct free_area *area = &zone->free_area[order];
	struct list_head *list = &area->free_list[mt];
	unsigned int page_len = PAGE_SIZE << order;
	struct page *page, *next;
	long budget;
	int err = 0;

	/*
	 * Perform early check, if free area is empty there is
	 * nothing to process so we can skip this free_list.
	 */
	/* 锁外空检查只是快速筛选；并发变化会在随后持锁遍历时自然体现。 */
	if (list_empty(list))
		return err;

	spin_lock_irq(&zone->lock);

	/*
	 * Limit how many calls we will be making to the page reporting
	 * device for this list. By doing this we avoid processing any
	 * given list for too long.
	 *
	 * The current value used allows us enough calls to process over a
	 * sixteenth of the current list plus one additional call to handle
	 * any pages that may have already been present from the previous
	 * list processed. This should result in us reporting all pages on
	 * an idle system in about 30 seconds.
	 *
	 * The division here should be cheap since PAGE_REPORTING_CAPACITY
	 * should always be a power of 2.
	 */
	/*
	 * 每条链限制设备调用次数，避免长期占用一个 zone/order；预算约为当前空闲块数
	 * 的 1/16 个批次，再容纳从上一链遗留的一批，因此空闲系统约 30 秒扫完。
	 * PAGE_REPORTING_CAPACITY 为 2 的幂，使除法可低成本实现。
	 */
	budget = DIV_ROUND_UP(area->nr_free, PAGE_REPORTING_CAPACITY * 16);

	/* loop through free list adding unreported pages to sg list */
	/* 遍历 free list，把尚未报告的块隔离后反向填入 sg 数组。 */
	list_for_each_entry_safe(page, next, list, lru) {
		/* We are going to skip over the reported pages. */
		/* 已报告且未离开 buddy 的块内容未变，直接跳过。 */
		if (PageReported(page))
			continue;

		/*
		 * If we fully consumed our budget then update our
		 * state to indicate that we are requesting additional
		 * processing and exit this list.
		 */
		/* 本链预算耗尽时请求后续轮次，并把当前页作为恢复旋转锚点。 */
		if (budget < 0) {
			atomic_set(&prdev->state, PAGE_REPORTING_REQUESTED);
			next = page;
			break;
		}

		/* Attempt to pull page from list and place in scatterlist */
		/* offset 非零表示批次尚未填满；隔离失败则停在当前链，避免破坏 buddy。 */
		if (*offset) {
			if (!__isolate_free_page(page, order)) {
				next = page;
				break;
			}

			/* Add page to scatter list */
			/* 隔离成功后 ownership 临时属于 sg，游标从数组末端向前推进。 */
			--(*offset);
			sg_set_page(&sgl[*offset], page, page_len, 0);

			continue;
		}

		/*
		 * Make the first non-reported page in the free list
		 * the new head of the free list before we release the
		 * zone lock.
		 */
		/* 批次已满；先把当前未处理页旋到链首，锁外 report 后可从新快照恢复。 */
		if (!list_is_first(&page->lru, list))
			list_rotate_to_front(&page->lru, list);

		/* release lock before waiting on report processing */
		/* 设备回调可能睡眠，绝不能持 zone->lock 调用。 */
		spin_unlock_irq(&zone->lock);

		/* begin processing pages in local list */
		/* 满批次页已从 buddy 隔离，驱动在此获得临时借用 scatterlist。 */
		err = prdev->report(prdev, sgl, PAGE_REPORTING_CAPACITY);

		/* reset offset since the full list was reported */
		/* report 已消费满批次，drain 后数组从容量位置重新开始反向填充。 */
		*offset = PAGE_REPORTING_CAPACITY;

		/* update budget to reflect call to report function */
		/* 预算按设备调用次数而非页数扣减。 */
		budget--;

		/* reacquire zone lock and resume processing */
		/* 放回 buddy、置 PageReported 和重建遍历位置都要求 zone 锁。 */
		spin_lock_irq(&zone->lock);

		/* flush reported pages from the sg list */
		/* err==0 才允许置 reported；错误批次同样必须完整归还。 */
		page_reporting_drain(prdev, sgl, PAGE_REPORTING_CAPACITY, !err);

		/*
		 * Reset next to first entry, the old next isn't valid
		 * since we dropped the lock to report the pages
		 */
		/* 锁曾释放，旧 next 可能失效；从当前链首重新建立安全迭代位置。 */
		next = list_first_entry(list, struct page, lru);

		/* exit on error */
		/* 首个设备错误终止本链并向上层传播，不继续提交新页。 */
		if (err)
			break;
	}

	/* Rotate any leftover pages to the head of the freelist */
	/* 把尚未处理的恢复锚点旋到链首，让下轮优先继续而非反复扫描旧前缀。 */
	if (!list_entry_is_head(next, list, lru) && !list_is_first(&next->lru, list))
		list_rotate_to_front(&next->lru, list);

	spin_unlock_irq(&zone->lock);

	return err;
}

/*
 * page_reporting_process_zone() - 按 order/migratetype 扫描一个 zone 并处理尾批次。
 * @prdev 为活动设备，@sgl 是 worker 独占数组，@zone 为借用 zone；返回 0 或首个
 * report 错误。先要求 low watermark 之外仍容纳一个最小 order 的满批次，避免
 * reporting 隔离妨碍分配进度；跳过 MIGRATE_ISOLATE。跨 free list 复用 offset，
 * 最后不足一批也调用设备并在 zone 锁下全部 drain。
 */
static int
page_reporting_process_zone(struct page_reporting_dev_info *prdev,
			    struct scatterlist *sgl, struct zone *zone)
{
	unsigned int order, mt, leftover, offset = PAGE_REPORTING_CAPACITY;
	unsigned long watermark;
	int err = 0;

	/* Generate minimum watermark to be able to guarantee progress */
	/* low watermark 加一整批目标 order 页，保证隔离期间分配器仍有基本余量。 */
	watermark = low_wmark_pages(zone) +
		    (PAGE_REPORTING_CAPACITY << page_reporting_order);

	/*
	 * Cancel request if insufficient free memory or if we failed
	 * to allocate page reporting statistics for the zone.
	 */
	/* 当前实现无 per-zone 统计分配；实际快速取消条件是空闲水位不足。 */
	if (!zone_watermark_ok(zone, 0, watermark, 0, ALLOC_CMA))
		return err;

	/* Process each free list starting from lowest order/mt */
	/* 从配置的最小 order 向高阶遍历，每阶再扫描所有普通 migratetype。 */
	for (order = page_reporting_order; order < NR_PAGE_ORDERS; order++) {
		for (mt = 0; mt < MIGRATE_TYPES; mt++) {
			/* We do not pull pages from the isolate free list */
			/* 隔离迁移类型由内存下线等流程拥有，reporting 不得抽取。 */
			if (is_migrate_isolate(mt))
				continue;

			err = page_reporting_cycle(prdev, zone, order, mt,
						   sgl, &offset);
			if (err)
				return err;
		}
	}

	/* report the leftover pages before going idle */
	/* 所有链处理完后，数组尾部可能有不足容量的一批，也必须在 idle 前提交。 */
	leftover = PAGE_REPORTING_CAPACITY - offset;
	if (leftover) {
		sgl = &sgl[offset];
		err = prdev->report(prdev, sgl, leftover);

		/* flush any remaining pages out from the last report */
		/* 尾批次无论设备成功与否都在 zone 锁下归还，避免泄漏隔离页。 */
		spin_lock_irq(&zone->lock);
		page_reporting_drain(prdev, sgl, leftover, !err);
		spin_unlock_irq(&zone->lock);
	}

	return err;
}

/*
 * page_reporting_process() - delayed_work 主循环，依次处理系统中所有 zone。
 * @work 内嵌于唯一 prdev；无返回值。开始发布 ACTIVE，分配 worker 独占 sg 数组，
 * 任一 zone 报错即停止本轮。收尾用 cmpxchg(ACTIVE->IDLE)：若并发释放已把状态
 * 改回 REQUESTED，则两秒后续排。分配失败也走同一状态收尾，不遗留活动状态。
 */
static void page_reporting_process(struct work_struct *work)
{
	struct delayed_work *d_work = to_delayed_work(work);
	struct page_reporting_dev_info *prdev =
		container_of(d_work, struct page_reporting_dev_info, work);
	int err = 0, state = PAGE_REPORTING_ACTIVE;
	struct scatterlist *sgl;
	struct zone *zone;

	/*
	 * Change the state to "Active" so that we can track if there is
	 * anyone requests page reporting after we complete our pass. If
	 * the state is not altered by the end of the pass we will switch
	 * to idle and quit scheduling reporting runs.
	 */
	/* 标为 ACTIVE 后，新通知会把状态改成 REQUESTED，作为本轮结束后的续跑信号。 */
	atomic_set(&prdev->state, state);

	/* allocate scatterlist to store pages being reported on */
	/* sg 数组只属于本次 worker，分配失败不调用驱动且仍执行状态机收尾。 */
	sgl = kmalloc_objs(*sgl, PAGE_REPORTING_CAPACITY);
	if (!sgl)
		goto err_out;

	sg_init_table(sgl, PAGE_REPORTING_CAPACITY);

	for_each_zone(zone) {
		/* zone 间不共享隔离页；process_zone 返回前已 drain 自己的尾批次。 */
		err = page_reporting_process_zone(prdev, sgl, zone);
		if (err)
			break;
	}

	kfree(sgl);
err_out:
	/*
	 * If the state has reverted back to requested then there may be
	 * additional pages to be processed. We will defer for 2s to allow
	 * more pages to accumulate.
	 */
	/*
	 * 仅状态仍等于本地 ACTIVE 时原子转 IDLE；若期间变成 REQUESTED，cmpxchg 保留
	 * 该值并返回 REQUESTED，于是延迟两秒续跑，让更多空闲块聚合。
	 */
	state = atomic_cmpxchg(&prdev->state, state, PAGE_REPORTING_IDLE);
	if (state == PAGE_REPORTING_REQUESTED)
		schedule_delayed_work(&prdev->work, PAGE_REPORTING_DELAY);
}

/* 串行唯一设备的注册/注销和 RCU 指针写入；不得覆盖仍在线设备。 */
static DEFINE_MUTEX(page_reporting_mutex);
/* 关闭时伙伴热路径为 false；首次成功注册后启用，注销不再关闭但 RCU NULL 保安全。 */
DEFINE_STATIC_KEY_FALSE(page_reporting_enabled);

/*
 * page_reporting_register() - 初始化并发布系统唯一 page-reporting 设备。
 * @prdev 由驱动分配并长期拥有，必须提供有效 report 回调；成功返回 0，已有设备
 * 返回 -EBUSY。函数在 mutex 下确定最小 order、初始化状态/work、请求首次全区扫描，
 * 再以 RCU 发布指针并启用 static key。发布后驱动须保持对象有效直到 unregister
 * 返回；首次工作延迟执行，因而会在指针发布之后看到完整初始化。
 */
int page_reporting_register(struct page_reporting_dev_info *prdev)
{
	int err = 0;

	mutex_lock(&page_reporting_mutex);

	/* nothing to do if already in use */
	/* 单实例协议：锁保护的非 NULL RCU 指针阻止第二个设备覆盖。 */
	if (rcu_dereference_protected(pr_dev_info,
				lockdep_is_held(&page_reporting_mutex))) {
		err = -EBUSY;
		goto err_out;
	}

	/*
	 * If the page_reporting_order value is not set, we check if
	 * an order is provided from the driver that is performing the
	 * registration. If that is not provided either, we default to
	 * pageblock_order.
	 */
	/*
	 * 全局参数未指定时优先采用驱动合法 order；驱动也未指定或越界则回退
	 * pageblock_order。已由参数设置的值不会被新设备覆盖。
	 */

	if (page_reporting_order == PAGE_REPORTING_ORDER_UNSPECIFIED) {
		if (prdev->order != PAGE_REPORTING_ORDER_UNSPECIFIED &&
		    prdev->order <= MAX_PAGE_ORDER)
			page_reporting_order = prdev->order;
		else
			page_reporting_order = pageblock_order;
	}

	/* initialize state and work structures */
	/* 所有状态先于 RCU 发布初始化；work 回调通过 container_of 找回此 prdev。 */
	atomic_set(&prdev->state, PAGE_REPORTING_IDLE);
	INIT_DELAYED_WORK(&prdev->work, &page_reporting_process);

	/* Begin initial flush of zones */
	/* 首次注册主动请求全区扫描，无需等待下一次大块释放触发。 */
	__page_reporting_request(prdev);

	/* Assign device to allow notifications */
	/* RCU 发布点：此后伙伴释放路径可借用已完全初始化的设备。 */
	rcu_assign_pointer(pr_dev_info, prdev);

	/* enable page reporting notification */
	/* static key 只在首次注册 patch；后续注销/重注册无需反复改热路径指令。 */
	if (!static_key_enabled(&page_reporting_enabled)) {
		static_branch_enable(&page_reporting_enabled);
		pr_info("Free page reporting enabled\n");
	}
err_out:
	mutex_unlock(&page_reporting_mutex);

	return err;
}
EXPORT_SYMBOL_GPL(page_reporting_register);

/*
 * page_reporting_unregister() - 撤销指定设备并同步排空所有读者和工作。
 * @prdev 仍由驱动拥有；无返回值。仅当它正是当前注册对象时，在 mutex 下先把 RCU
 * 指针置 NULL，synchronize_rcu() 等待释放热路径读者，再同步取消 delayed_work。
 * 返回后 report 不会再被调用，驱动可释放对象；传入非当前设备是无副作用 no-op。
 * static key 故意保持启用，之后通知仅付出 RCU 查 NULL 成本并允许安全重注册。
 */
void page_reporting_unregister(struct page_reporting_dev_info *prdev)
{
	mutex_lock(&page_reporting_mutex);

	if (prdev == rcu_dereference_protected(pr_dev_info,
				lockdep_is_held(&page_reporting_mutex))) {
		/* Disable page reporting notification */
		/* 先撤发布再等 grace period，阻止新的 notify 取得该对象。 */
		RCU_INIT_POINTER(pr_dev_info, NULL);
		synchronize_rcu();

		/* Flush any existing work, and lock it out */
		/* 同步取消覆盖尚未运行和正在运行的 work，返回时设备回调已静止。 */
		cancel_delayed_work_sync(&prdev->work);
	}

	mutex_unlock(&page_reporting_mutex);
}
EXPORT_SYMBOL_GPL(page_reporting_unregister);
