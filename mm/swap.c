// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/swap.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 */

/*
 * This file contains the default values for the operation of the
 * Linux VM subsystem. Fine-tuning documentation can be found in
 * Documentation/admin-guide/sysctl/vm.rst.
 * Started 18.12.91
 * Swap aging added 23.2.95, Stephen Tweedie.
 * Buffermem limits added 12.3.98, Rik van Riel.
 */

#include <linux/mm.h>
/* 基础 MM 类型定义 folio、VMA、zone 与页表可见性；其余头补充各子系统专有操作。 */
#include <linux/sched.h>
/* 当前任务 flags 决定 fault/reclaim 特殊路径，不能把它们当作 folio 的持久属性。 */
#include <linux/kernel_stat.h>
/* VM 事件为统计观测，不能替代实际 LRU 链或引用计数的同步。 */
#include <linux/swap.h>
/* swap 公共 ABI 还被 reclaim、swapfile 和 fault 路径使用，本轮只解释此实现的消费点。 */
#include <linux/mman.h>
/* VMA flags 在 mlock 入 LRU 时影响可回收性，读取需服从调用者的 VMA 稳定性约束。 */
#include <linux/pagemap.h>
/* page-cache/folio 辅助函数决定 mapping 与 LRU 释放的先后，不能以裸 page 替代。 */
#include <linux/folio_batch.h>
/* folio_batch 是短期借用容器；入队额外引用和 drain 后归还构成其生命周期闭环。 */
#include <linux/init.h>
#include <linux/export.h>
#include <linux/mm_inline.h>
#include <linux/percpu_counter.h>
#include <linux/memremap.h>
#include <linux/percpu.h>
/* 本文件 per-CPU 批次仅可在本 CPU local lock 或 CPU 已死的 hotplug 路径访问。 */
#include <linux/cpu.h>
#include <linux/notifier.h>
/* notifier 相关依赖服务于外部 MM 协调，不改变此处 LRU 基本锁序。 */
#include <linux/backing-dev.h>
#include <linux/memcontrol.h>
/* memcg uncharge 必须发生在普通死 folio 脱离 LRU 后，设备/HugeTLB 有独立归属。 */
#include <linux/gfp.h>
#include <linux/uio.h>
#include <linux/hugetlb.h>
#include <linux/page_idle.h>
#include <linux/local_lock.h>
/* local_lock 在 PREEMPT/RT 配置下抽象本 CPU 排他性，不能自行替换为普通自旋锁。 */
#include <linux/buffer_head.h>

/* include 组提供 folio/LRU、memcg、CPU hotplug 和 sysctl 的交叉接口；本文件不直接实现 swap I/O。 */

#include "internal.h"

/* internal.h 提供 MM 内部的 lruvec、释放和初始化辅助契约，调用者必须遵守相应锁规则。 */

#define CREATE_TRACE_POINTS
#include <trace/events/pagemap.h>

/* 本文件集中实现 folio 引用归零、per-CPU 延迟 LRU 批次、访问冷热反馈和 swap 相关 sysctl。 */

/* How many pages do we try to swap or page in/out together? As a power of 2 */
/* 该 2 的幂决定一次换入/换出预读聚合规模，sysctl 写入受 page_cluster_max 约束。 */
int page_cluster;
static const int page_cluster_max = 31;

/* page_cluster 是全局策略输入，不承载单个 swap device 的状态；具体设备生命周期在 swapfile.c。 */

struct cpu_fbatches {
	/* 每 CPU 延迟 LRU 操作的私有队列；local lock 使同 CPU 的生产和 drain 串行。 */
	/*
	 * The following folio batches are grouped together because they are protected
	 * by disabling preemption (and interrupts remain enabled).
	 */
	local_lock_t lock;
	struct folio_batch lru_add;
	struct folio_batch lru_deactivate_file;
	struct folio_batch lru_deactivate;
	struct folio_batch lru_lazyfree;
	/* 各 batch 的操作函数不同，drain 顺序确保同一 folio 的状态转换仍由唯一 LRU 锁串行。 */
#ifdef CONFIG_SMP
	/* SMP 远端激活通过单独 batch 汇集，避免每次访问都抢 lruvec 自旋锁。 */
	struct folio_batch lru_activate;
#endif
	/* Protecting the following batches which require disabling interrupts */
	local_lock_t lock_irq;
	struct folio_batch lru_move_tail;
};

/* 普通 batch 只禁止抢占，可能由进程上下文 drain；move-tail 也可由中断生产，需禁 IRQ。 */

static DEFINE_PER_CPU(struct cpu_fbatches, cpu_fbatches) = {
	.lock = INIT_LOCAL_LOCK(lock),
	.lock_irq = INIT_LOCAL_LOCK(lock_irq),
};

/* per-CPU 批次的 folio 引用由入队者获得、由 folio_batch_move_lru() 或最终释放路径归还。 */

static void __page_cache_release(struct folio *folio, struct lruvec **lruvecp,
		unsigned long *flagsp)
{
	/*
	 * 业务背景：普通 folio 最后引用可在 batch 中连续释放，需复用 lruvec 锁降低 IRQ 锁开销。
	 * 入参：folio；lruvecp/flagsp 保存跨项复用的锁状态。出参/返回：无。
	 * 注意事项：只有带 LRU 位才取得锁并摘链；调用者负责最终归还可能仍持有的锁。
	 */
	/* 最后引用释放前，把仍在 LRU 的 folio 从其 lruvec 摘除；锁状态经输出参数跨批次复用。 */
	if (folio_test_lru(folio)) {
		/* relock helper 在 folio 跨 memcg/节点迁移时重新选择正确 lruvec，不能复用旧锁。 */
		folio_lruvec_relock_irqsave(folio, lruvecp, flagsp);
		lruvec_del_folio(*lruvecp, folio);
		__folio_clear_lru_flags(folio);
	}
}

/*
 * This path almost never happens for VM activity - pages are normally freed
 * in batches.  But it gets used by networking - and for compound pages.
 */
/*
 * page_cache_release() - 完成普通 folio 的 page-cache/LRU 最后引用清理。
 * 业务背景：普通 folio 引用归零后必须先从 LRU 体系撤销，才能交给后续 split、memcg 与 buddy 回收。
 * 入参：folio 为已通过引用归零路径确认要释放的 folio。出参/返回：无。
 * 注意事项：仅 __page_cache_release() 实际取得 lruvec 锁时才归还 IRQ 锁；不能处理设备或 HugeTLB folio。
 */
static void page_cache_release(struct folio *folio)
{
	struct lruvec *lruvec = NULL;
	unsigned long flags;

	__page_cache_release(folio, &lruvec, &flags);
	if (lruvec)
		/* __page_cache_release 只有实际摘链时持锁，调用者负责在这里一次性归还 IRQ 锁。 */
		lruvec_unlock_irqrestore(lruvec, flags);
}

/*
 * __folio_put() - 分派引用归零 folio 的最终释放路径。
 * 业务背景：folio 的 allocator、memcg 和 LRU 所有权随类型不同，不能用普通 buddy 路径统一释放。
 * 入参：folio 的最后一个普通引用已被调用者扣除。出参/返回：无。
 * 注意事项：ZONE_DEVICE 与 HugeTLB 先转交专属释放者；普通 folio 严格按摘 LRU、撤 split、uncharge、free 顺序执行。
 */
void __folio_put(struct folio *folio)
{
	if (unlikely(folio_is_zone_device(folio))) {
		/* ZONE_DEVICE 的释放由设备页生命周期管理，不能落入 buddy。 */
		free_zone_device_folio(folio);
		return;
	}

	if (folio_test_hugetlb(folio)) {
		/* HugeTLB 维护独立池和 memcg 规则，普通 LRU/page allocator 不接管。 */
		free_huge_folio(folio);
		return;
	}

	page_cache_release(folio);
	/* 普通路径顺序是摘 LRU、撤 deferred split、解除 memcg 计费、归还 frozen buddy 页。 */
	folio_unqueue_deferred_split(folio);
	mem_cgroup_uncharge(folio);
	free_frozen_pages(&folio->page, folio_order(folio));
}
EXPORT_SYMBOL(__folio_put);

typedef void (*move_fn_t)(struct lruvec *lruvec, struct folio *folio);

/*
 * lru_add() - 在已持有的 lruvec 锁内把 folio 发布到目标 LRU。
 * 业务背景：新 folio 先在 per-CPU batch 聚合，drain 时才依据 mlock/可回收性选择 active、inactive 或 unevictable 链。
 * 入参：lruvec 已加锁，folio 由 batch 持引用且尚未带 LRU 位。出参/返回：无。
 * 注意事项：mlock_count 只能保守处理；链表发布和 trace/统计必须在同一锁窗口完成。
 */
static void lru_add(struct lruvec *lruvec, struct folio *folio)
{
	int was_unevictable = folio_test_clear_unevictable(folio);
	long nr_pages = folio_nr_pages(folio);

	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);

	/*
	 * Is an smp_mb__after_atomic() still required here, before
	 * folio_evictable() tests the mlocked flag, to rule out the possibility
	 * of stranding an evictable folio on an unevictable LRU?  I think
	 * not, because __munlock_folio() only clears the mlocked flag
	 * while the LRU lock is held.
	 *
	 * (That is not true of __page_cache_release(), and not necessarily
	 * true of folios_put(): but those only clear the mlocked flag after
	 * folio_put_testzero() has excluded any other users of the folio.)
	 */
	if (folio_evictable(folio)) {
		/* 清除了 unevictable 的 folio 重获回收资格，统计 rescued 页数而非 folio 个数。 */
		if (was_unevictable)
			__count_vm_events(UNEVICTABLE_PGRESCUED, nr_pages);
	} else {
		/* 不可回收 folio 不能带 active；mlock_count 保守置零让 reclaim 后续校正。 */
		folio_clear_active(folio);
		folio_set_unevictable(folio);
		/*
		 * folio->mlock_count = !!folio_test_mlocked(folio)?
		 * But that leaves __mlock_folio() in doubt whether another
		 * actor has already counted the mlock or not.  Err on the
		 * safe side, underestimate, let page reclaim fix it, rather
		 * than leaving a page on the unevictable LRU indefinitely.
		 */
		folio->mlock_count = 0;
		if (!was_unevictable)
			__count_vm_events(UNEVICTABLE_PGCULLED, nr_pages);
	}

	lruvec_add_folio(lruvec, folio);
	/* 链表发布后才 trace，观察者看到的 LRU 状态与统计应一致。 */
	trace_mm_lru_insertion(folio);
}

/*
 * folio_batch_move_lru() - drain 一个 per-CPU 延迟 LRU batch。
 * 业务背景：批量提交摊薄 lru 锁竞争，并允许访问反馈在 add 到真实链表前改变 active 状态。
 * 入参：fbatch 是当前 CPU 或已死 CPU 的队列；move_fn 决定 add、activate、deactivate 等具体迁移。
 * 出参/返回：无；batch 中每项额外引用在提交或死页释放后归还。
 * 注意事项：普通移动先 clear LRU 位取得唯一权；dead add folio 必须撤 deferred split 后统一 uncharge/free。
 */
static void folio_batch_move_lru(struct folio_batch *fbatch, move_fn_t move_fn)
{
	int i;
	struct lruvec *lruvec = NULL;
	unsigned long flags = 0;
	struct folio_batch free_fbatch;
	bool is_lru_add = (move_fn == lru_add);

	/*
	 * If we're adding to the LRU, preemptively filter dead folios. Use
	 * this dedicated folio batch for temp storage and deferred cleanup.
	 */
	if (is_lru_add)
		/* add 批次可含已死 folio，临时 batch 集中其后续 uncharge/free，避免循环内锁抖动。 */
		folio_batch_init(&free_fbatch);

	for (i = 0; i < folio_batch_count(fbatch); i++) {
		/* 每项的 LRU 归属可能在批次等待期间变化，必须逐项重新测试/加锁。 */
		struct folio *folio = fbatch->folios[i];

		/* block memcg migration while the folio moves between lru */
		if (!is_lru_add && !folio_test_clear_lru(folio))
			/* 非 add 操作以清 LRU 位取得“唯一移动者”资格，竞争者已处理则跳过。 */
			continue;

		/*
		 * Filter dead folios by moving them from the add batch to the temp
		 * batch for freeing after this loop.
		 *
		 * We're bypassing normal cleanup. Clear flags that are not
		 * applicable to dead folios.
		 *
		 * Since the folio may be part of a huge page, unqueue from
		 * deferred split list to avoid a dangling list entry.
		 */
		if (is_lru_add && folio_ref_freeze(folio, 1)) {
			/* 冻结成功说明只剩批次持有的引用；不再入 LRU，转交统一释放。 */
			__folio_clear_active(folio);
			__folio_clear_unevictable(folio);
			folio_unqueue_deferred_split(folio);
			fbatch->folios[i] = NULL;
			folio_batch_add(&free_fbatch, folio);
			continue;
		}

		folio_lruvec_relock_irqsave(folio, &lruvec, &flags);
		/* lruvec 锁覆盖 move_fn 对链、active/reclaim 旗标和统计的复合更新。 */
		move_fn(lruvec, folio);

		folio_set_lru(folio);
	}

	if (lruvec)
		/* 同一 lruvec 跨多项复用锁，循环结束后才恢复 IRQ。 */
		lruvec_unlock_irqrestore(lruvec, flags);

	/* Cleanup filtered dead folios. */
	if (is_lru_add) {
		/* 死 folio 不经普通 folios_put，因引用已冻结；先 uncharge 后归还 buddy。 */
		mem_cgroup_uncharge_folios(&free_fbatch);
		free_unref_folios(&free_fbatch);
	}

	folios_put(fbatch);
}

static void __folio_batch_add_and_move(struct folio_batch __percpu *fbatch,
		struct folio *folio, move_fn_t move_fn, bool disable_irq)
{
	/*
	 * 业务背景：所有 LRU 请求共用该 producer，先取额外引用再按 batch 容量决定延迟或立即提交。
	 * 入参：percpu batch、folio、迁移函数和是否需 IRQ local lock。出参/返回：无。
	 * 注意事项：batch 成员排序决定 disable_irq；锁内不能遗漏 folio_may_be_lru_cached 与全局禁用检查。
	 */
	/* 入队前持引用；local lock 保护本 CPU 批次，满批、不可缓存或全局禁用时同步 drain。 */
	unsigned long flags;

	folio_get(folio);
	/* 批次异步提交，额外引用保证调用者离开后 folio 仍可被 drain 安全访问。 */

	if (disable_irq)
		local_lock_irqsave(&cpu_fbatches.lock_irq, flags);
	else
		local_lock(&cpu_fbatches.lock);

	if (!folio_batch_add(this_cpu_ptr(fbatch), folio) ||
			!folio_may_be_lru_cached(folio) || lru_cache_disabled())
		folio_batch_move_lru(this_cpu_ptr(fbatch), move_fn);
	/* 满批与全局禁用均强制同步提交，避免隔离/回收等待隐藏的 per-CPU LRU 状态。 */

	if (disable_irq)
		local_unlock_irqrestore(&cpu_fbatches.lock_irq, flags);
	else
		local_unlock(&cpu_fbatches.lock);
}

/* 宏由成员相对 lock_irq 的布局判定保护级别；调整 cpu_fbatches 字段顺序会改变并发语义。 */
#define folio_batch_add_and_move(folio, op)		\
	__folio_batch_add_and_move(			\
		&cpu_fbatches.op,			\
		folio,					\
		op,					\
		offsetof(struct cpu_fbatches, op) >=	\
		offsetof(struct cpu_fbatches, lock_irq)	\
	)

/*
 * lru_move_tail() - 在 lru 锁下把可回收 folio 旋转到 inactive 尾。
 * 业务背景：writeback 完成后的 reclaim 候选应延后再次扫描。入参：已加锁 lruvec 和其 folio。
 * 出参/返回：无。注意事项：unevictable 不移动；摘链、清 active 和尾插是同一锁内状态转换。
 */
static void lru_move_tail(struct lruvec *lruvec, struct folio *folio)
{
	if (folio_test_unevictable(folio))
		/* 不可回收链没有旋转语义，保留原位置以避免错误改变 mlock 状态。 */
		return;

	lruvec_del_folio(lruvec, folio);
	/* 删除、清 active、尾插必须同锁完成，观察者不会看到 folio 同时位于两处。 */
	folio_clear_active(folio);
	lruvec_add_folio_tail(lruvec, folio);
	__count_vm_events(PGROTATED, folio_nr_pages(folio));
}

/*
 * Writeback is about to end against a folio which has been marked for
 * immediate reclaim.  If it still appears to be reclaimable, move it
 * to the tail of the inactive list.
 *
 * folio_rotate_reclaimable() must disable IRQs, to prevent nasty races.
 */
/*
 * folio_rotate_reclaimable() - 提示 writeback 即将完成的 folio 旋转到 inactive 尾部。
 * 业务背景：reclaim 标记的干净 folio 可在 writeback 后让位给其他候选，以改善后续扫描顺序。
 * 入参：folio 为可能仍在 LRU 的候选。出参/返回：无。
 * 注意事项：调用上下文必须允许 IRQ-safe batch 操作；锁定、脏、unevictable 或已脱链的 folio 一律不移动。
 */
void folio_rotate_reclaimable(struct folio *folio)
{
	if (folio_test_locked(folio) || folio_test_dirty(folio) ||
	    folio_test_unevictable(folio) || !folio_test_lru(folio))
		return;
	/* writeback 即将结束前的最佳努力提示；随后中断安全批次再做真实旋转。 */

	folio_batch_add_and_move(folio, lru_move_tail);
}

void lru_note_cost_unlock_irq(struct lruvec *lruvec, bool file,
		unsigned int nr_io, unsigned int nr_rotated)
		__releases(lruvec->lru_lock)
		__releases(rcu)
{
	/*
	 * 业务背景：reclaim 以 file/anon 代价反馈调整扫描平衡，统计需传播到所有祖先 memcg。
	 * 入参：已加 IRQ lru 锁的 lruvec、类型和 IO/rotation 页数。出参/返回：无。
	 * 注意事项：本函数消费 lru 锁和 RCU 读锁；每次上溯前先解子锁，避免父子锁嵌套。
	 */
	/* 此函数消费 lru 锁和 RCU 读锁：向根 memcg 传播衰减后的 IO/CPU 代价后自行释放二者。 */
	unsigned long cost;

	/*
	 * Reflect the relative cost of incurring IO and spending CPU
	 * time on rotations. This doesn't attempt to make a precise
	 * comparison, it just says: if reloads are about comparable
	 * between the LRU lists, or rotations are overwhelmingly
	 * different between them, adjust scan balance for CPU work.
	 */
	cost = nr_io * SWAP_CLUSTER_MAX + nr_rotated;
	/* IO 以 cluster 权重折算，rotation 近似 CPU 工作；这是扫描平衡信号而非精确时延。 */
	if (!cost) {
		/* 空事件仍必须交还调用者移交的 lru 锁与 RCU 读侧临界区。 */
		spin_unlock_irq(&lruvec->lru_lock);
		rcu_read_unlock();
		return;
	}

	for (;;) {
		/* 从叶 memcg 向父级传播，父 lruvec 的生存期由外层 RCU 读锁稳定。 */
		unsigned long lrusize;

		/* Record cost event */
		if (file)
			lruvec->file_cost += cost;
		else
			lruvec->anon_cost += cost;

		/*
		 * Decay previous events
		 *
		 * Because workloads change over time (and to avoid
		 * overflow) we keep these statistics as a floating
		 * average, which ends up weighing recent refaults
		 * more than old ones.
		 */
		lrusize = lruvec_page_state(lruvec, NR_INACTIVE_ANON) +
			  lruvec_page_state(lruvec, NR_ACTIVE_ANON) +
			  lruvec_page_state(lruvec, NR_INACTIVE_FILE) +
			  lruvec_page_state(lruvec, NR_ACTIVE_FILE);

		if (lruvec->file_cost + lruvec->anon_cost > lrusize / 4) {
			/* 半衰减防止溢出并使近期 refault/rotation 比历史事件权重更高。 */
			lruvec->file_cost /= 2;
			lruvec->anon_cost /= 2;
		}

		spin_unlock_irq(&lruvec->lru_lock);
		lruvec = parent_lruvec(lruvec);
		/* 已解锁子 lruvec 后才上溯，避免同时持父子 lru 锁造成锁序问题。 */
		if (!lruvec) {
			rcu_read_unlock();
			break;
		}
		spin_lock_irq(&lruvec->lru_lock);
	}
}

/*
 * lru_note_cost_refault() - 为一次 refault 向 folio 所属 memcg LRU 祖先链记代价。
 * 业务背景：file/anon 扫描平衡需要近似比较 IO reload 与 CPU rotation 的成本。
 * 入参：folio 可定位当前 lruvec。出参/返回：无。
 * 注意事项：下层 lru_note_cost_unlock_irq() 消费 lru 锁和 RCU 读锁；本函数不得在返回后继续使用 lruvec。
 */
void lru_note_cost_refault(struct folio *folio)
{
	struct lruvec *lruvec;

	lruvec = folio_lruvec_lock_irq(folio);
	lru_note_cost_unlock_irq(lruvec, folio_is_file_lru(folio),
				folio_nr_pages(folio), 0);
}

/*
 * lru_activate() - 在 lru 锁内提升 inactive folio。
 * 业务背景：重复访问需要提高回收保护。入参：已加锁 lruvec 与 batch folio。出参/返回：无。
 * 注意事项：active/unevictable 已设置时不重复记账；链和事件统计在同一临界区提交。
 */
static void lru_activate(struct lruvec *lruvec, struct folio *folio)
{
	long nr_pages = folio_nr_pages(folio);

	if (folio_test_active(folio) || folio_test_unevictable(folio))
		/* 已激活或不可回收无需重复记账，保持 active/unevictable 互斥不变量。 */
		return;


	lruvec_del_folio(lruvec, folio);
	folio_set_active(folio);
	lruvec_add_folio(lruvec, folio);
	trace_mm_lru_activate(folio);

	__count_vm_events(PGACTIVATE, nr_pages);
	count_memcg_events(lruvec_memcg(lruvec), PGACTIVATE, nr_pages);
}

#ifdef CONFIG_SMP
/*
 * folio_activate_drain() - 提交指定 CPU 的 activate batch。
 * 业务背景：访问热度延迟聚合以减少 lru 锁竞争。入参：cpu。出参/返回：无。
 * 注意事项：仅 SMP 有实际队列；调用者须满足该 CPU percpu 数据可访问的本地或离线前提。
 */
static void folio_activate_drain(int cpu)
{
	struct folio_batch *fbatch = &per_cpu(cpu_fbatches.lru_activate, cpu);

	if (folio_batch_count(fbatch))
		/* CPU 下线/显式 drain 都必须清空激活批次，批次引用随提交或释放归还。 */
		folio_batch_move_lru(fbatch, lru_activate);
}

/*
 * folio_activate() - 请求提升仍在 LRU 的 folio。
 * 业务背景：传统 LRU 将访问反馈批量转化为 active 链迁移。入参：folio。出参/返回：无。
 * 注意事项：active、unevictable 或已脱链项直接跳过；队列额外引用由后续 drain 归还。
 */
void folio_activate(struct folio *folio)
{
	if (folio_test_active(folio) || folio_test_unevictable(folio) ||
	    !folio_test_lru(folio))
		return;

	folio_batch_add_and_move(folio, lru_activate);
}

#else
/* UP 没有远端 batch；清 LRU 位取得移动权后在当前上下文直接取 lru 锁完成激活。 */
static inline void folio_activate_drain(int cpu)
{
}

void folio_activate(struct folio *folio)
{
	struct lruvec *lruvec;

	if (!folio_test_clear_lru(folio))
		/* 已被其他移动者摘链则不重复操作，避免同一 folio 双重插链。 */
		return;

	lruvec = folio_lruvec_lock_irq(folio);
	/* lock helper 将 folio 的 memcg/node 归属转换为受 IRQ 锁保护的当前 lruvec。 */
	lru_activate(lruvec, folio);
	lruvec_unlock_irq(lruvec);
	folio_set_lru(folio);
	/* 移动完成后恢复 LRU 位，发布顺序使其他路径只在链已一致时观察到该位。 */
}
#endif

/*
 * __lru_cache_activate_folio() - 在本 CPU 尚未提交的 add batch 中标记 active。
 * 业务背景：folio 尚未上 LRU 时仍可吸收访问反馈。入参：folio。出参/返回：无。
 * 注意事项：只允许检查 local batch，远端 batch 可并发 drain/迁移/释放，不能安全改写。
 */
static void __lru_cache_activate_folio(struct folio *folio)
{
	struct folio_batch *fbatch;
	int i;

	local_lock(&cpu_fbatches.lock);
	/* 只能扫描本 CPU add batch：远端条目可能同时 drain、迁移或释放。 */
	fbatch = this_cpu_ptr(&cpu_fbatches.lru_add);

	/*
	 * Search backwards on the optimistic assumption that the folio being
	 * activated has just been added to this batch. Note that only
	 * the local batch is examined as a !LRU folio could be in the
	 * process of being released, reclaimed, migrated or on a remote
	 * batch that is currently being drained. Furthermore, marking
	 * a remote batch's folio active potentially hits a race where
	 * a folio is marked active just after it is added to the inactive
	 * list causing accounting errors and BUG_ON checks to trigger.
	 */
	for (i = folio_batch_count(fbatch) - 1; i >= 0; i--) {
		/* 逆向优先命中刚入队项，减少热点访问为查找旧 batch 付出的线性成本。 */
		struct folio *batch_folio = fbatch->folios[i];

		if (batch_folio == folio) {
			/* 这里只置 active，不改 LRU 链；实际链选择仍在同一 batch drain 时发生。 */
			folio_set_active(folio);
			break;
		}
	}

	local_unlock(&cpu_fbatches.lock);
	/* 释放 local lock 后不得再解引用 fbatch 中的借用项，它们可立即被 drain。 */
}

#ifdef CONFIG_LRU_GEN

/*
 * lru_gen_inc_refs() - 累加多代 LRU 的访问 refs。
 * 业务背景：多代回收以 flags 中的有限 refs 字段记录热度。入参：folio。出参/返回：无。
 * 注意事项：cmpxchg 必须保留并发更新；饱和后只置 workingset，不得溢出字段。
 */
static void lru_gen_inc_refs(struct folio *folio)
{
	/* flags 的 LRU refs 字段可能被并发访问更新，循环以 cmpxchg 保留所有非 refs 位。 */
	unsigned long new_flags, old_flags = READ_ONCE(folio->flags.f);

	if (folio_test_unevictable(folio))
		/* 不可回收 folio 不参加代际老化，避免访问噪声改变不可回收集合。 */
		return;

	/* see the comment on LRU_REFS_FLAGS */
	if (!folio_test_referenced(folio)) {
		/* 第一次访问仅设 referenced；下一次访问才触发 active 提升，避免一次性过热。 */
		set_mask_bits(&folio->flags.f, LRU_REFS_MASK, BIT(PG_referenced));
		return;
	}

	do {
		/* refs 饱和时不再递增；workingset 标记表示重复访问足以影响后续回收决策。 */
		if ((old_flags & LRU_REFS_MASK) == LRU_REFS_MASK) {
			if (!folio_test_workingset(folio))
				folio_set_workingset(folio);
			return;
		}

		new_flags = old_flags + BIT(LRU_REFS_PGOFF);
		/* 失败时 old_flags 被 cmpxchg 刷新，重新计算后重试，不能用普通写覆盖竞争更新。 */
	} while (!try_cmpxchg(&folio->flags.f, &old_flags, new_flags));
}

/*
 * lru_gen_clear_refs() - 清热度并判断 generation 是否已是最老。
 * 业务背景：deactivate 需避免对已无保护价值的 generation 做无效 shuffle。入参：folio。出参/返回：是否可跳过移动。
 * 注意事项：读取 lruvec 最小序列受 RCU 保护；gen 小于零按保守 true 处理。
 */
static bool lru_gen_clear_refs(struct folio *folio)
{
	/* 读取最小序列需 RCU 保护 lruvec；结果只是锁内是否可省略 shuffle 的判断。 */
	int gen = folio_lru_gen(folio);
	int type = folio_is_file_lru(folio);
	unsigned long seq;

	if (gen < 0)
		/* 不在 generation 的 folio 没有可清 refs，调用者应保守跳过传统移动。 */
		return true;

	set_mask_bits(&folio->flags.f, LRU_REFS_FLAGS | BIT(PG_workingset), 0);
	/* 清除引用和 workingset 是对本轮冷却的提交，后续访问可重新建立热度。 */

	rcu_read_lock();
	seq = READ_ONCE(folio_lruvec(folio)->lrugen.min_seq[type]);
	rcu_read_unlock();
	/* whether can do without shuffling under the LRU lock */
	return gen == lru_gen_from_seq(seq);
}

#else /* !CONFIG_LRU_GEN */
	/* 未启用多代 LRU 时两 helper 退化为空/false，调用者继续传统 active/inactive 协议。 */

static void lru_gen_inc_refs(struct folio *folio)
{
}

static bool lru_gen_clear_refs(struct folio *folio)
{
	return false;
}

#endif /* CONFIG_LRU_GEN */

/**
 * folio_mark_accessed - Mark a folio as having seen activity.
 * @folio: The folio to mark.
 *
 * This function will perform one of the following transitions:
 *
 * * inactive,unreferenced	->	inactive,referenced
 * * inactive,referenced	->	active,unreferenced
 * * active,unreferenced	->	active,referenced
 *
 * When a newly allocated folio is not yet visible, so safe for non-atomic ops,
 * __folio_set_referenced() may be substituted for folio_mark_accessed().
 */
/*
 * folio_mark_accessed() - 将一次 folio 访问反馈写入传统或多代 LRU 状态机。
 * 业务背景：reclaim 需要区分一次性访问、重复访问和热点 workingset，避免冷页长期占据 active LRU。
 * 入参：folio 为调用者正在访问的有效对象。出参/返回：无。
 * 注意事项：启用 LRU_GEN 时只更新 refs 位；传统模式可能排队激活。dropbehind 与 unevictable 不得被误升温。
 */
void folio_mark_accessed(struct folio *folio)
{
	if (folio_test_dropbehind(folio))
		/* dropbehind 是顺序访问回收提示，访问不应重新升温以免破坏预期丢弃。 */
		return;
	if (lru_gen_enabled()) {
		/* 多代启用时不混用传统 referenced/active 双状态机，统一由 refs 字段计数。 */
		lru_gen_inc_refs(folio);
		return;
	}

	if (!folio_test_referenced(folio)) {
		/* 首次引用仅记录轻量访问，避免瞬时访问立即污染 active 列表。 */
		folio_set_referenced(folio);
	} else if (folio_test_unevictable(folio)) {
		/* unevictable LRU 不被轮转，已有 referenced 也无需再做激活转换。 */
		/*
		 * Unevictable pages are on the "LRU_UNEVICTABLE" list. But,
		 * this list is never rotated or maintained, so marking an
		 * unevictable page accessed has no effect.
		 */
	} else if (!folio_test_active(folio)) {
		/* 普通 LRU 走独立 activate 批次；仍在 add 批次则原地置 active 等 drain 选链。 */
		/*
		 * If the folio is on the LRU, queue it for activation via
		 * cpu_fbatches.lru_activate. Otherwise, assume the folio is in a
		 * folio_batch, mark it active and it'll be moved to the active
		 * LRU on the next drain.
		 */
		if (folio_test_lru(folio))
			/* 已在 LRU 的 folio 只能通过激活 batch 改链；不得在无 lru 锁时直接操作链表。 */
			folio_activate(folio);
		else
			/* 尚未提交的 add batch 可安全置位，drain 会依据 active 位选择最终链。 */
			__lru_cache_activate_folio(folio);
		folio_clear_referenced(folio);
		/* active 转换消耗 referenced 位，下一轮访问才再次累积热度。 */
		workingset_activation(folio);
		/* workingset 统计在真正提升时记账，为 refault/scan 策略提供信号。 */
	}
	if (folio_test_idle(folio))
		/* 访问反馈同时撤销 idle-page tracking 标记，避免用户态继续把它视作闲置。 */
		folio_clear_idle(folio);
}
EXPORT_SYMBOL(folio_mark_accessed);

/**
 * folio_add_lru - Add a folio to an LRU list.
 * @folio: The folio to be added to the LRU.
 *
 * Queue the folio for addition to the LRU. The decision on whether
 * to add the page to the [in]active [file|anon] list is deferred until the
 * folio_batch is drained. This gives a chance for the caller of folio_add_lru()
 * have the folio added to the active list using folio_mark_accessed().
 */
/*
 * folio_add_lru() - 把新 folio 延迟加入合适的 LRU。
 * 业务背景：fault、readahead 等生产者不应每页抢 lru 锁，先在本 CPU batch 中收集再统一发布。
 * 入参：folio 尚未在 LRU，调用者仍持有自己的引用。出参/返回：无。
 * 注意事项：batch 会额外取引用；LRU_GEN fault 的预热只对非 PF_MEMALLOC、可回收 folio 生效。
 */
void folio_add_lru(struct folio *folio)
{
	VM_BUG_ON_FOLIO(folio_test_active(folio) &&
			folio_test_unevictable(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);

	/*
	 * For refaulted workingset folios, set PG_active so they
	 * can be added to active generations.
	 * For prefaulted file folios, folio_mark_accessed() sets
	 * PG_referenced so lru_gen_folio_seq() places them into
	 * the second oldest generation.
	 */
	if (lru_gen_enabled() && !folio_test_unevictable(folio) &&
		/* 仅 fault 上的非回收任务可预热；workingset 与 referenced 分别映射到 active/次老 generation。 */
	    lru_gen_in_fault() && !(current->flags & PF_MEMALLOC)) {
		if (folio_test_workingset(folio))
			folio_set_active(folio);
		else if (!folio_test_referenced(folio))
			folio_mark_accessed(folio);
	/* fault 上的预设使 drain 时直接选择较热 generation，PF_MEMALLOC 任务避免扰动回收。 */
	}

	folio_batch_add_and_move(folio, lru_add);
	/* 入队后调用者仍保有自身引用；batch 的额外引用直到实际插链或死页释放才归还。 */
}
EXPORT_SYMBOL(folio_add_lru);

/**
 * folio_add_lru_vma() - Add a folio to the appropriate LRU list for this VMA.
 * @folio: The folio to be added to the LRU.
 * @vma: VMA in which the folio is mapped.
 *
 * If the VMA is mlocked, @folio is added to the unevictable list.
 * Otherwise, it is treated the same way as folio_add_lru().
 */
/*
 * folio_add_lru_vma() - 按映射 VMA 的锁页属性选择 folio 的初始 LRU 归属。
 * 业务背景：新映射页若受 VM_LOCKED 保护必须进入 unevictable 路径，普通页则复用批量 LRU 加入机制。
 * 入参：folio 尚未在 LRU；vma 是其当前映射上下文。出参/返回：无。
 * 注意事项：VM_SPECIAL|VM_LOCKED 不是普通 mlock；只在标志恰为 VM_LOCKED 时调用 mlock_new_folio。
 */
void folio_add_lru_vma(struct folio *folio, struct vm_area_struct *vma)
{
	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);

	if (unlikely((vma->vm_flags & (VM_LOCKED | VM_SPECIAL)) == VM_LOCKED))
		/* VM_SPECIAL 与 VM_LOCKED 同时出现不等同普通 mlock，避免对特殊映射错误 pin。 */
		mlock_new_folio(folio);
	else
		folio_add_lru(folio);
}

/*
 * If the folio cannot be invalidated, it is moved to the
 * inactive list to speed up its reclaim.  It is moved to the
 * head of the list, rather than the tail, to give the flusher
 * threads some time to write it out, as this is much more
 * effective than the single-page writeout from reclaim.
 *
 * If the folio isn't mapped and dirty/writeback, the folio
 * could be reclaimed asap using the reclaim flag.
 *
 * 1. active, mapped folio -> none
 * 2. active, dirty/writeback folio -> inactive, head, reclaim
 * 3. inactive, mapped folio -> none
 * 4. inactive, dirty/writeback folio -> inactive, head, reclaim
 * 5. inactive, clean -> inactive, tail
 * 6. Others -> none
 *
 * In 4, it moves to the head of the inactive list so the folio is
 * written out by flusher threads as this is much more efficient
 * than the single-page writeout from reclaim.
 */
/*
 * lru_deactivate_file() - 将适合回收的未映射文件 folio 降级。
 * 业务背景：无效化失败的脏/回写 file folio 需尽早被 flusher 看见，干净项则可放到 inactive 尾等待回收。
 * 入参：lruvec 已加锁；folio 由 deactivation batch 持引用。出参/返回：无。
 * 注意事项：已映射或 unevictable 项不动；设置 reclaim 与 writeback 结束存在小竞态，属于刻意接受的提示性语义。
 */
static void lru_deactivate_file(struct lruvec *lruvec, struct folio *folio)
{
	bool active = folio_test_active(folio) || lru_gen_enabled();
	long nr_pages = folio_nr_pages(folio);

	if (folio_test_unevictable(folio))
		/* unevictable 不参与文件 LRU 回收加速，即使调用者请求也维持原链。 */
		return;

	/* Some processes are using the folio */
	if (folio_mapped(folio))
		/* 映射中的 folio 仍可能被访问，不能把它作为无效化失败后的纯 writeback 候选。 */
		return;

	lruvec_del_folio(lruvec, folio);
	folio_clear_active(folio);
	folio_clear_referenced(folio);

	if (folio_test_writeback(folio) || folio_test_dirty(folio)) {
		/* 脏/回写项先回 inactive 头并标 reclaim，让后台 flusher 批量写回而非 reclaim 单页写出。 */
		/*
		 * Setting the reclaim flag could race with
		 * folio_end_writeback() and confuse readahead.  But the
		 * race window is _really_ small and  it's not a critical
		 * problem.
		 */
		lruvec_add_folio(lruvec, folio);
		folio_set_reclaim(folio);
	} else {
		/* 批次等待期间回写已完成则放 inactive 尾，给其他候选优先回收机会。 */
		/*
		 * The folio's writeback ended while it was in the batch.
		 * We move that folio to the tail of the inactive list.
		 */
		lruvec_add_folio_tail(lruvec, folio);
		__count_vm_events(PGROTATED, nr_pages);
	}

	if (active) {
		/* 只有从活跃语义降级才计 PGDEACTIVATE，避免对原 inactive 项重复记账。 */
		__count_vm_events(PGDEACTIVATE, nr_pages);
		count_memcg_events(lruvec_memcg(lruvec), PGDEACTIVATE,
				     nr_pages);
	}
}

/*
 * lru_deactivate() - 在 lru 锁内将普通 active folio 转为 inactive。
 * 业务背景：reclaim/访问反馈需要把未持续活跃的页降温，给更热点的页腾出 active 空间。
 * 入参：lruvec 已加锁；folio 是 batch 中的候选。出参/返回：无。
 * 注意事项：传统模式要求 active 位，多代模式使用 generation 语义；摘链、清标志和再插必须原子于 lru 锁。
 */
static void lru_deactivate(struct lruvec *lruvec, struct folio *folio)
{
	long nr_pages = folio_nr_pages(folio);

	if (folio_test_unevictable(folio) || !(folio_test_active(folio) || lru_gen_enabled()))
		/* 传统模式需 active 位；多代模式允许由 generation 状态替代该条件。 */
		return;

	lruvec_del_folio(lruvec, folio);
	/* 摘链、清热度、再插 inactive 均在同一个 lru 锁区间内完成。 */
	folio_clear_active(folio);
	folio_clear_referenced(folio);
	lruvec_add_folio(lruvec, folio);

	__count_vm_events(PGDEACTIVATE, nr_pages);
	count_memcg_events(lruvec_memcg(lruvec), PGDEACTIVATE, nr_pages);
}

/*
 * lru_lazyfree() - 将可丢弃的清洁匿名 folio 转成 lazyfree 状态。
 * 业务背景：MADV_FREE 类语义允许匿名内容在未再写入前按 file LRU 回收，避免不必要 swap 写出。
 * 入参：lruvec 已加锁；folio 来自 lazyfree batch。出参/返回：无。
 * 注意事项：只能处理 anon、swapbacked、非 swapcache、可回收 folio；清 swapbacked 后改变的是回收语义而非引用所有权。
 */
static void lru_lazyfree(struct lruvec *lruvec, struct folio *folio)
{
	long nr_pages = folio_nr_pages(folio);

	if (!folio_test_anon(folio) || !folio_test_swapbacked(folio) ||
		/* lazyfree 拒绝 file、非 swapbacked、swapcache 和不可回收项，防止错误改变交换语义。 */
	    folio_test_swapcache(folio) || folio_test_unevictable(folio))
		return;

	lruvec_del_folio(lruvec, folio);
	folio_clear_active(folio);
	if (lru_gen_enabled())
		/* 多代模式通过 refs helper 清状态；传统模式直接清 referenced 位。 */
		lru_gen_clear_refs(folio);
	else
		folio_clear_referenced(folio);
	/*
	 * Lazyfree folios are clean anonymous folios.  They have
	 * the swapbacked flag cleared, to distinguish them from normal
	 * anonymous folios
	 */
	folio_clear_swapbacked(folio);
	/* 清 swapbacked 后 reclaim 将其视作可丢弃的干净匿名内容，而非需要 swap backing 的 anon。 */
	lruvec_add_folio(lruvec, folio);

	__count_vm_events(PGLAZYFREE, nr_pages);
	count_memcg_events(lruvec_memcg(lruvec), PGLAZYFREE, nr_pages);
}

/*
 * Drain pages out of the cpu's folio_batch.
 * Either "cpu" is the current CPU, and preemption has already been
 * disabled; or "cpu" is being hot-unplugged, and is already dead.
 */
/*
 * lru_add_drain_cpu() - drain 指定 CPU 的全部延迟 LRU batch。
 * 业务背景：per-CPU batch 隐藏了尚未发布到真实 LRU 的 folio，CPU 下线、回收和显式同步必须清空它们。
 * 入参：cpu 是当前 CPU（已禁止抢占）或已经 offline 的 CPU。出参/返回：无。
 * 注意事项：move-tail batch 可由中断生产，必须用 IRQ local lock；其余 batch 遵循普通 local lock 协议。
 */
void lru_add_drain_cpu(int cpu)
{
	struct cpu_fbatches *fbatches = &per_cpu(cpu_fbatches, cpu);
	/* 仅读取计数，不取 local lock；调用者据此决定是否排队，实际 drain 自行获得正确保护。 */
	struct folio_batch *fbatch = &fbatches->lru_add;

	if (folio_batch_count(fbatch))
		/* add batch 最常见，先 drain；每个 drain 均可能归还 batch 持有的 folio 引用。 */
		folio_batch_move_lru(fbatch, lru_add);

	fbatch = &fbatches->lru_move_tail;
	/* move-tail 可能由中断加入，读取计数用 data_race，仅作为是否尝试抢 IRQ-local-lock 的提示。 */
	/* Disabling interrupts below acts as a compiler barrier. */
	if (data_race(folio_batch_count(fbatch))) {
		unsigned long flags;

		/* No harm done if a racing interrupt already did this */
		local_lock_irqsave(&cpu_fbatches.lock_irq, flags);
		folio_batch_move_lru(fbatch, lru_move_tail);
		local_unlock_irqrestore(&cpu_fbatches.lock_irq, flags);
	}

	fbatch = &fbatches->lru_deactivate_file;
	/* 文件降级、普通降级和 lazyfree 分列以保留各自 move_fn 的状态过滤。 */
	if (folio_batch_count(fbatch))
		folio_batch_move_lru(fbatch, lru_deactivate_file);

	fbatch = &fbatches->lru_deactivate;
	if (folio_batch_count(fbatch))
		folio_batch_move_lru(fbatch, lru_deactivate);

	fbatch = &fbatches->lru_lazyfree;
	if (folio_batch_count(fbatch))
		folio_batch_move_lru(fbatch, lru_lazyfree);

	folio_activate_drain(cpu);
}

/**
 * deactivate_file_folio() - Deactivate a file folio.
 * @folio: Folio to deactivate.
 *
 * This function hints to the VM that @folio is a good reclaim candidate,
 * for example if its invalidation fails due to the folio being dirty
 * or under writeback.
 *
 * Context: Caller holds a reference on the folio.
 */
/*
 * deactivate_file_folio() - 请求把文件 folio 降温以加速后续回收。
 * 业务背景：无效化失败或预期不再使用的文件内容应尽快成为 reclaim 候选，但移动由 batch 合并。
 * 入参：folio，调用者持有一个引用。出参/返回：无。
 * 注意事项：unevictable、已不在 LRU 或多代模式仍有热度的 folio 不排队；本函数不保证返回时已移动。
 */
void deactivate_file_folio(struct folio *folio)
{
	/* Deactivating an unevictable folio will not accelerate reclaim */
	if (folio_test_unevictable(folio) || !folio_test_lru(folio))
		return;

	if (lru_gen_enabled() && lru_gen_clear_refs(folio))
		return;

	folio_batch_add_and_move(folio, lru_deactivate_file);
}

/*
 * folio_deactivate - deactivate a folio
 * @folio: folio to deactivate
 *
 * folio_deactivate() moves @folio to the inactive list if @folio was on the
 * active list and was not unevictable. This is done to accelerate the
 * reclaim of @folio.
 */
/*
 * folio_deactivate() - 请求将普通 active folio 降到 inactive。
 * 业务背景：回收路径和上层提示通过该接口减弱冷页保护，实际改链延迟到 lru batch drain。
 * 入参：folio 为可能仍在 LRU 的候选。出参/返回：无。
 * 注意事项：多代模式先判断 generation 是否可清；传统模式要求 active 位，unevictable 不得移动。
 */
void folio_deactivate(struct folio *folio)
{
	if (folio_test_unevictable(folio) || !folio_test_lru(folio))
		return;

	if (lru_gen_enabled() ? lru_gen_clear_refs(folio) : !folio_test_active(folio))
		return;

	folio_batch_add_and_move(folio, lru_deactivate);
}

/**
 * folio_mark_lazyfree - make an anon folio lazyfree
 * @folio: folio to deactivate
 *
 * folio_mark_lazyfree() moves @folio to the inactive file list.
 * This is done to accelerate the reclaim of @folio.
 */
/*
 * folio_mark_lazyfree() - 请求把可丢弃匿名 folio 转为 lazyfree。
 * 业务背景：用户声明内容可在未再次写入前丢弃时，VM 应避免为它保持 swap backing。
 * 入参：folio 是可能在 LRU 的匿名页。出参/返回：无。
 * 注意事项：swapcache、非 swapbacked、unevictable 或脱链项全部拒绝；实际清标志在 lru 锁下延迟发生。
 */
void folio_mark_lazyfree(struct folio *folio)
{
	if (!folio_test_anon(folio) || !folio_test_swapbacked(folio) ||
	    !folio_test_lru(folio) ||
	    folio_test_swapcache(folio) || folio_test_unevictable(folio))
		return;

	folio_batch_add_and_move(folio, lru_lazyfree);
}

/*
 * lru_add_drain() - 同步提交当前 CPU 的 LRU 与 mlock 延迟状态。
 * 业务背景：需要确定 LRU 可见性的调用者不能只依赖 batch 入队，必须在本 CPU 锁窗口清空队列。
 * 入参：无。出参/返回：无。
 * 注意事项：local lock 防止迁移到其他 CPU；随后 mlock_drain_local() 与 LRU drain 保持同一可见性边界。
 */
void lru_add_drain(void)
{
	local_lock(&cpu_fbatches.lock);
	lru_add_drain_cpu(smp_processor_id());
	local_unlock(&cpu_fbatches.lock);
	mlock_drain_local();
}

/*
 * It's called from per-cpu workqueue context in SMP case so
 * lru_add_drain_cpu and invalidate_bh_lrus_cpu should run on
 * the same cpu. It shouldn't be a problem in !SMP case since
 * the core is only one and the locks will disable preemption.
 */
/*
 * lru_add_and_bh_lrus_drain() - worker 上同步 drain folio 与 buffer-head LRU。
 * 业务背景：远端 CPU 的两类延迟缓存都必须在本 CPU 上处理。入参/出参：无。
 * 注意事项：local lock 保证 percpu folio batch 的访问；调用上下文已绑定目标 CPU。
 */
static void lru_add_and_bh_lrus_drain(void)
{
	local_lock(&cpu_fbatches.lock);
	lru_add_drain_cpu(smp_processor_id());
	local_unlock(&cpu_fbatches.lock);
	invalidate_bh_lrus_cpu();
	mlock_drain_local();
}

/*
 * lru_add_drain_cpu_zone() - drain 当前 CPU 的 LRU batch 和指定 zone 本地页。
 * 业务背景：zone 回收/隔离需要清除同 CPU 隐藏缓存。入参：zone。出参/返回：无。
 * 注意事项：本函数不能跨 CPU 使用；mlock 延迟状态在同一边界 drain。
 */
void lru_add_drain_cpu_zone(struct zone *zone)
{
	local_lock(&cpu_fbatches.lock);
	lru_add_drain_cpu(smp_processor_id());
	drain_local_pages(zone);
	local_unlock(&cpu_fbatches.lock);
	mlock_drain_local();
}

#ifdef CONFIG_SMP

static DEFINE_PER_CPU(struct work_struct, lru_add_drain_work);

/*
 * lru_add_drain_per_cpu() - 远端 drain work 的回调。
 * 业务背景：percpu batch 只能由所属 CPU 处理。入参：dummy work 参数未使用。出参/返回：无。
 * 注意事项：work 必须经 queue_work_on() 投递；不得从任意 CPU 直接访问目标 batch。
 */
static void lru_add_drain_per_cpu(struct work_struct *dummy)
{
	lru_add_and_bh_lrus_drain();
}

/*
 * cpu_needs_drain() - 判断一个 CPU 是否值得投递 drain work。
 * 业务背景：全局 drain 只为有待处理 LRU/mlock/BH 状态的 CPU 建 work。入参：cpu。出参/返回：是否可能有工作。
 * 注意事项：读计数是调度提示而非锁定快照；假阳性只多一次 work，状态竞争由实际 drain 处理。
 */
static bool cpu_needs_drain(unsigned int cpu)
{
	struct cpu_fbatches *fbatches = &per_cpu(cpu_fbatches, cpu);

	/* Check these in order of likelihood that they're not zero */
	return folio_batch_count(&fbatches->lru_add) ||
		/* 按常见度短路，减少常态无工作 CPU 的 percpu 读取；mlock/BH 也必须同批同步。 */
		folio_batch_count(&fbatches->lru_move_tail) ||
		folio_batch_count(&fbatches->lru_deactivate_file) ||
		folio_batch_count(&fbatches->lru_deactivate) ||
		folio_batch_count(&fbatches->lru_lazyfree) ||
		folio_batch_count(&fbatches->lru_activate) ||
		need_mlock_drain(cpu) ||
		has_bh_in_lru(cpu, NULL);
}

/*
 * Doesn't need any cpu hotplug locking because we do rely on per-cpu
 * kworkers being shut down before our page_alloc_cpu_dead callback is
 * executed on the offlined cpu.
 * Calling this function with cpu hotplug locks held can actually lead
 * to obscure indirect dependencies via WQ context.
 */
static inline void __lru_add_drain_all(bool force_all_cpus)
{
	/* 全局代数与 mutex 合并并发 drain 请求；屏障确保“已排队”不会遗漏本次之前的入队。 */
	/*
	 * lru_drain_gen - Global pages generation number
	 *
	 * (A) Definition: global lru_drain_gen = x implies that all generations
	 *     0 < n <= x are already *scheduled* for draining.
	 *
	 * This is an optimization for the highly-contended use case where a
	 * user space workload keeps constantly generating a flow of pages for
	 * each CPU.
	 */
	static unsigned int lru_drain_gen;
	static struct cpumask has_work;
	static DEFINE_MUTEX(lock);
	unsigned cpu, this_gen;

	/*
	 * Make sure nobody triggers this path before mm_percpu_wq is fully
	 * initialized.
	 */
	if (WARN_ON(!mm_percpu_wq))
		/* 初始化前无法安全提交远端 work；WARN 后保守返回，调用者不得假定已 drain。 */
		return;

	/*
	 * Guarantee folio_batch counter stores visible by this CPU
	 * are visible to other CPUs before loading the current drain
	 * generation.
	 */
	smp_mb();
	/* 屏障使本 CPU 先前的 batch 写入先于读取 generation 对其他 CPU 可见。 */

	/*
	 * (B) Locally cache global LRU draining generation number
	 *
	 * The read barrier ensures that the counter is loaded before the mutex
	 * is taken. It pairs with smp_mb() inside the mutex critical section
	 * at (D).
	 */
	this_gen = smp_load_acquire(&lru_drain_gen);
	/* acquire 与稍后的 generation 发布配对，阻止本次合并判断跨越 batch 可见性边界。 */

	/* It helps everyone if we do our own local drain immediately. */
	lru_add_drain();
	/* 先 drain 自己避免排队 work 的延迟，也降低全局 mutex 下的待处理量。 */

	mutex_lock(&lock);
	/* mutex 串行 generation 提升和 has_work 构造，work 完成前不允许下一批覆盖其 work_struct。 */

	/*
	 * (C) Exit the draining operation if a newer generation, from another
	 * lru_add_drain_all(), was already scheduled for draining. Check (A).
	 */
	if (unlikely(this_gen != lru_drain_gen && !force_all_cpus))
		/* 更晚请求已经承诺覆盖本代时合并退出；强制模式为 disable 路径，必须重新扫描。 */
		goto done;

	/*
	 * (D) Increment global generation number
	 *
	 * Pairs with smp_load_acquire() at (B), outside of the critical
	 * section. Use a full memory barrier to guarantee that the
	 * new global drain generation number is stored before loading
	 * folio_batch counters.
	 *
	 * This pairing must be done here, before the for_each_online_cpu loop
	 * below which drains the page vectors.
	 *
	 * Let x, y, and z represent some system CPU numbers, where x < y < z.
	 * Assume CPU #z is in the middle of the for_each_online_cpu loop
	 * below and has already reached CPU #y's per-cpu data. CPU #x comes
	 * along, adds some pages to its per-cpu vectors, then calls
	 * lru_add_drain_all().
	 *
	 * If the paired barrier is done at any later step, e.g. after the
	 * loop, CPU #x will just exit at (C) and miss flushing out all of its
	 * added pages.
	 */
	WRITE_ONCE(lru_drain_gen, lru_drain_gen + 1);
	/* 发布新代后 full barrier 保证再读取各 CPU 计数前，其他请求可据此安全合并。 */
	smp_mb();

	cpumask_clear(&has_work);
	/* has_work 只记录本轮成功排队的 CPU，随后逐一 flush 保证调用返回时这些 batch 已提交。 */
	for_each_online_cpu(cpu) {
		struct work_struct *work = &per_cpu(lru_add_drain_work, cpu);

		if (cpu_needs_drain(cpu)) {
			/* work 绑定目标 CPU 才能访问其 percpu queue；hotplug 依赖 kworker 先停止的约定。 */
			INIT_WORK(work, lru_add_drain_per_cpu);
			queue_work_on(cpu, mm_percpu_wq, work);
			__cpumask_set_cpu(cpu, &has_work);
		}
	}

	for_each_cpu(cpu, &has_work)
		/* flush 是同步点：迁移隔离等调用者返回后不会再看到本轮隐藏的 LRU batch。 */
		flush_work(&per_cpu(lru_add_drain_work, cpu));

done:
	/* 所有退出路径均释放合并 mutex；已排队 work 在正常路径已由上方 flush 完成。 */
	mutex_unlock(&lock);
}

/*
 * lru_add_drain_all() - 请求并同步等待所有在线 CPU 的 LRU drain。
 * 业务背景：调用者需要全局 LRU 可见性时，必须合并并等待各 percpu batch。入参/出参：无。
 * 注意事项：SMP 路径以 generation 合并并发请求；UP 路径只 drain 当前 CPU。
 */
void lru_add_drain_all(void)
{
	__lru_add_drain_all(false);
}
#else
/* 非 SMP 的公开入口直接 drain 当前 CPU；没有远端队列或 work 完成等待。 */
void lru_add_drain_all(void)
{
	lru_add_drain();
}
#endif /* CONFIG_SMP */

atomic_t lru_disable_count = ATOMIC_INIT(0);

/* 禁用计数允许嵌套 isolate 调用；最后 enable 前各 CPU 的延迟 add 队列不得重新暴露候选。 */

/*
 * lru_cache_disable() needs to be called before we start compiling
 * a list of folios to be migrated using folio_isolate_lru().
 * It drains folios on LRU cache and then disable on all cpus until
 * lru_cache_enable is called.
 *
 * Must be paired with a call to lru_cache_enable().
 */
/*
 * lru_cache_disable() - 在 LRU 隔离操作前禁止新的延迟 LRU 缓存。
 * 业务背景：迁移候选收集需要稳定的 LRU 集合，不能让 CPU batch 在隔离扫描期间偷偷发布 folio。
 * 入参：无。出参/返回：无；与 lru_cache_enable() 成对。
 * 注意事项：支持嵌套调用；RCU 同步覆盖仍观察旧零计数的生产者，之后必须强制 drain 所有 CPU。
 */
void lru_cache_disable(void)
{
	/* 先发布非零计数阻止新缓存，再等待所有可能仍看到旧零值的 preempt/RCU 临界区结束。 */
	atomic_inc(&lru_disable_count);
	/* 引用式计数而非 bool，确保嵌套调用者不会因另一个 enable 而过早恢复 LRU 缓存。 */
	/*
	 * Readers of lru_disable_count are protected by either disabling
	 * preemption or rcu_read_lock:
	 *
	 * preempt_disable, local_irq_disable  [bh_lru_lock()]
	 * rcu_read_lock		       [rt_spin_lock CONFIG_PREEMPT_RT]
	 * preempt_disable		       [local_lock !CONFIG_PREEMPT_RT]
	 *
	 * Since v5.1 kernel, synchronize_rcu() is guaranteed to wait on
	 * preempt_disable() regions of code. So any CPU which sees
	 * lru_disable_count = 0 will have exited the critical
	 * section when synchronize_rcu() returns.
	 */
	synchronize_rcu_expedited();
	/* RCU 同步后，旧生产者已离开其 local/preempt 临界区，强制 drain 可覆盖全部先前入队。 */
#ifdef CONFIG_SMP
	__lru_add_drain_all(true);
#else
	lru_add_and_bh_lrus_drain();
#endif
}

/**
 * folios_put_refs - Reduce the reference count on a batch of folios.
 * @folios: The folios.
 * @refs: The number of refs to subtract from each folio.
 *
 * Like folio_put(), but for a batch of folios.  This is more efficient
 * than writing the loop yourself as it will optimise the locks which need
 * to be taken if the folios are freed.  The folios batch is returned
 * empty and ready to be reused for another batch; there is no need
 * to reinitialise it.  If @refs is NULL, we subtract one from each
 * folio refcount.
 *
 * Context: May be called in process or interrupt context, but not in NMI
 * context.  May be called while holding a spinlock.
 */
/*
 * folios_put_refs() - 批量扣减 folio 引用并处置已归零项。
 * 业务背景：批量释放可复用 lruvec 锁和 memcg/buddy 操作，避免调用者逐页承担高竞争释放成本。
 * 入参：folios 为可复用 batch；refs 可为每项扣减数或 NULL（每项减一）。出参/返回：无，batch 被清空或压缩。
 * 注意事项：设备、HugeTLB 和 huge zero folio 使用专属生命周期；普通死 folio 才可合并 uncharge/free。
 */
void folios_put_refs(struct folio_batch *folios, unsigned int *refs)
{
	int i, j;
	struct lruvec *lruvec = NULL;
	unsigned long flags = 0;
	/* lruvec 锁跨相邻普通 folio 复用，遇设备/HugeTLB 必先释放以避免混合生命周期。 */

	for (i = 0, j = 0; i < folios->nr; i++) {
		/* j 压缩真正引用归零的普通 folio，最终 batch 才可安全交给 memcg/buddy 批量释放。 */
		struct folio *folio = folios->folios[i];
		unsigned int nr_refs = refs ? refs[i] : 1;

		/* Folio batch entry may have been preemptively removed during drain. */
		if (!folio)
			/* drain 可预先清空槽位；NULL 不是错误，避免对已处理 folio 二次减引用。 */
			continue;

		if (is_huge_zero_folio(folio))
			/* 全局 Huge Zero folio 不走普通引用归零释放，保持其共享常驻生命周期。 */
			continue;

		if (folio_is_zone_device(folio)) {
			/* 设备 folio 最终释放转交驱动/ZONE_DEVICE，普通 memcg 与 buddy 路径不可触及。 */
			if (lruvec) {
				lruvec_unlock_irqrestore(lruvec, flags);
				lruvec = NULL;
			}
			if (folio_ref_sub_and_test(folio, nr_refs))
				free_zone_device_folio(folio);
			continue;
		}

		if (!folio_ref_sub_and_test(folio, nr_refs))
			/* 仍有其他持有者则无需 LRU/memcg 操作，保留原 batch 槽等待调用者复用规则。 */
			continue;

		/* hugetlb has its own memcg */
		if (folio_test_hugetlb(folio)) {
			/* HugeTLB 的池与计费独立，不能把其死页混入普通 uncharge_folios 批次。 */
			if (lruvec) {
				lruvec_unlock_irqrestore(lruvec, flags);
				lruvec = NULL;
			}
			free_huge_folio(folio);
			continue;
		}
		folio_unqueue_deferred_split(folio);
		/* 普通死 folio 先撤 split 队列，再摘 LRU，避免释放后留下延迟拆分链表悬挂项。 */
		__page_cache_release(folio, &lruvec, &flags);

		if (j != i)
			folios->folios[j] = folio;
		j++;
	}
	if (lruvec)
		/* 循环结束统一交还最后复用的 lru 锁；前面的特殊分支已先行解锁。 */
		lruvec_unlock_irqrestore(lruvec, flags);
	if (!j) {
		/* 没有普通死 folio 时重置 batch，避免旧空洞和计数影响下一次批处理。 */
		folio_batch_reinit(folios);
		return;
	}

	folios->nr = j;
	/* 压缩后的连续集合先解除 memcg 计费，再归还 buddy；顺序保证释放后不再被计费引用。 */
	mem_cgroup_uncharge_folios(folios);
	free_unref_folios(folios);
}
EXPORT_SYMBOL(folios_put_refs);

/**
 * release_pages - batched put_page()
 * @arg: array of pages to release
 * @nr: number of pages
 *
 * Decrement the reference count on all the pages in @arg.  If it
 * fell to zero, remove the page from the LRU and free it.
 *
 * Note that the argument can be an array of pages, encoded pages,
 * or folio pointers. We ignore any encoded bits, and turn any of
 * them into just a folio that gets free'd.
 */
/*
 * release_pages() - 兼容旧 page 和 encoded-page ABI 的批量引用归还入口。
 * 业务背景：多个 MM 调用者传入的元素可包含 page、folio 或“下一项为扣减页数”的编码，统一后复用 folio 批量释放。
 * 入参：arg 是 encoded_page 数组视图；nr 是数组元素数。出参/返回：无。
 * 注意事项：NR_PAGES_NEXT 会消耗紧随元素；局部 refs 只与当前 batch 同步使用，尾批必须显式提交。
 */
void release_pages(release_pages_arg arg, int nr)
{
	/* 兼容入口把各编码统一为 folio；局部 refs 数组只在本次同步 drain 调用期间借用。 */
	struct folio_batch fbatch;
	int refs[FOLIO_BATCH_SIZE];
	struct encoded_page **encoded = arg.encoded_pages;
	int i;

	folio_batch_init(&fbatch);
	/* batch 满时立即提交并复用容器，因而 refs 下标始终与当前 fbatch 槽位对齐。 */
	for (i = 0; i < nr; i++) {
		/* 编码数组可紧随一个“页数即引用数”控制项，消费它时手动推进 i。 */
		/* Turn any of the argument types into a folio */
		struct folio *folio = page_folio(encoded_page_ptr(encoded[i]));
		/* encoded 标志在取指针时被剥离，任何 page/folio 表示均折叠到 folio 头。 */

		/* Is our next entry actually "nr_pages" -> "nr_refs" ? */
		refs[fbatch.nr] = 1;
		/* 默认每个输入减一个引用；NR_PAGES_NEXT 仅改变紧随 folio 的扣减数。 */
		if (unlikely(encoded_page_flags(encoded[i]) &
			     ENCODED_PAGE_BIT_NR_PAGES_NEXT))
			refs[fbatch.nr] = encoded_nr_pages(encoded[++i]);

		if (folio_batch_add(&fbatch, folio) > 0)
			/* 未满继续聚合，批量释放可复用 LRU 锁并减少 memcg/buddy 开销。 */
			continue;
		folios_put_refs(&fbatch, refs);
		/* 满批提交后 fbatch 被重置，下一项从 refs[0] 重新开始填充。 */
	}

	if (fbatch.nr)
		/* 尾批不能遗漏；即使未满也要在返回前归还本入口消费的引用。 */
		folios_put_refs(&fbatch, refs);
}
EXPORT_SYMBOL(release_pages);

/*
 * The folios which we're about to release may be in the deferred lru-addition
 * queues.  That would prevent them from really being freed right now.  That's
 * OK from a correctness point of view but is inefficient - those folios may be
 * cache-warm and we want to give them back to the page allocator ASAP.
 *
 * So __folio_batch_release() will drain those queues here.
 * folio_batch_move_lru() calls folios_put() directly to avoid
 * mutual recursion.
 */
/*
 * __folio_batch_release() - 在释放 batch 前消除当前 CPU 的延迟 LRU 可见性。
 * 业务背景：即将释放的 folio 可能仍被同 CPU add batch 持有引用，先 drain 才能让它及时归还 allocator。
 * 入参：fbatch 为待释放的 folio batch。出参/返回：无。
 * 注意事项：percpu_pvec_drained 防止重复 drain；真正引用、memcg、LRU 和 buddy 收尾仍委托 folios_put()。
 */
void __folio_batch_release(struct folio_batch *fbatch)
{
	/* 该 helper 的额外 drain 只需一次，同一 batch 后续释放不重复遍历当前 CPU 队列。 */
	if (!fbatch->percpu_pvec_drained) {
		/* 延迟 add 队列若仍持引用会阻止立刻 free；先提交能提升释放及时性但不改变正确性。 */
		lru_add_drain();
		fbatch->percpu_pvec_drained = true;
	}
	folios_put(fbatch);
	/* drain 后沿普通批量释放路径处理，那里负责 memcg、LRU 和 buddy 的最终顺序。 */
}
EXPORT_SYMBOL(__folio_batch_release);

/**
 * folio_batch_remove_exceptionals() - Prune non-folios from a batch.
 * @fbatch: The batch to prune
 *
 * find_get_entries() fills a batch with both folios and shadow/swap/DAX
 * entries.  This function prunes all the non-folio entries from @fbatch
 * without leaving holes, so that it can be passed on to folio-only batch
 * operations.
 */
/*
 * folio_batch_remove_exceptionals() - 从混合 XArray 查询结果中过滤非 folio value。
 * 业务背景：page-cache 查询会返回 shadow、swap、DAX 等编码值，folio-only 批处理不能解引用它们。
 * 入参：fbatch 为可原地修改的混合批次。出参/返回：无，fbatch->nr 缩短到真实 folio 数。
 * 注意事项：保持真实 folio 相对顺序；nr 之外的旧槽不再属于 API 可见集合，无需清零。
 */
void folio_batch_remove_exceptionals(struct folio_batch *fbatch)
{
	/* XArray value 并非可解引用 folio；原地压缩保持输入相对顺序且不额外分配。 */
	unsigned int i, j;

	for (i = 0, j = 0; i < folio_batch_count(fbatch); i++) {
		/* 仅复制真实指针项，shadow/swap/DAX value 被丢弃，尾部旧槽对 nr 之外消费者不可见。 */
		struct folio *folio = fbatch->folios[i];
		if (!xa_is_value(folio))
			fbatch->folios[j++] = folio;
	}
	fbatch->nr = j;
	/* 新长度是后续 folio-only API 的边界，旧槽无需清零。 */
}

#ifdef CONFIG_MEMCG
static void lruvec_reparent_lru(struct lruvec *child_lruvec,
				struct lruvec *parent_lruvec,
				enum lru_list lru, int nid)
{
	/*
	 * 业务背景：memcg 离线要把子组某条 LRU 链和每 zone 页数归入父组。
	 * 入参：子/父 lruvec、LRU 类型与节点。出参/返回：无。
	 * 注意事项：unevictable 链不拼接；调用者已提供 reparent 同步，统计与链归并不得分离。
	 */
	/* memcg 离线时把子 lru 链和分 zone 计数并入父级；UNEVICTABLE 链不在这里拼接。 */
	int zid;
	struct zone *zone;
	/* 调用者已处于 memcg reparent 协议；链拼接与每 zone 统计更新必须对同一 lru 一致。 */

	if (lru != LRU_UNEVICTABLE)
		/* unevictable 链不随普通 LRU 拼接，因其 mlock/隔离语义由专门路径维持。 */
		list_splice_tail_init(&child_lruvec->lists[lru], &parent_lruvec->lists[lru]);

	for_each_managed_zone_pgdat(zone, NODE_DATA(nid), zid, MAX_NR_ZONES - 1) {
		/* 逐 managed zone 转移页数，避免父 memcg 的 node 统计与已拼接链表失配。 */
		unsigned long size = mem_cgroup_get_zone_lru_size(child_lruvec, lru, zid);

		mem_cgroup_update_lru_size(parent_lruvec, lru, zid, size);
	}
}

/*
 * lru_reparent_memcg() - 在 memcg 离线时将一个节点的 LRU 状态归并给父组。
 * 业务背景：子 cgroup 消失不能遗留 LRU 链、扫描代价或每 zone 统计，否则父级 reclaim 账本会失真。
 * 入参：memcg、parent 和 nid 标识同一节点上的源/目标 lruvec。出参/返回：无。
 * 注意事项：UNEVICTABLE 链不走普通 splice；调用者必须已满足 memcg reparent 所要求的外层同步。
 */
void lru_reparent_memcg(struct mem_cgroup *memcg, struct mem_cgroup *parent, int nid)
{
	enum lru_list lru;
	struct lruvec *child_lruvec, *parent_lruvec;
	/* 两个 lruvec 固定在同一 nid；代价先累加、随后每种 LRU 链和 zone 计数逐项并入。 */

	child_lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(nid));
	parent_lruvec = mem_cgroup_lruvec(parent, NODE_DATA(nid));
	parent_lruvec->anon_cost += child_lruvec->anon_cost;
	parent_lruvec->file_cost += child_lruvec->file_cost;

	for_each_lru(lru)
		lruvec_reparent_lru(child_lruvec, parent_lruvec, lru, nid);
}
#endif

static const struct ctl_table swap_sysctl_table[] = {
	/* page-cluster 是唯一导出的本文件策略项，proc handler 在 [0, page_cluster_max] 内解析整数。 */
	{
		.procname	= "page-cluster",
		.data		= &page_cluster,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= (void *)&page_cluster_max,
	}
};

/*
 * Perform any setup for the swap system
 */
/*
 * swap_setup() - 初始化 swap readahead 聚合策略和 page-cluster sysctl。
 * 业务背景：小内存机器不能承受过大的换页预读，启动默认值按总 RAM 选择而运行期允许管理员调节。
 * 入参：无。出参/返回：无。
 * 注意事项：仅 __init 阶段计算 totalram；sysctl 表是静态对象，注册后由 sysctl core 借用且不可释放。
 */
void __init swap_setup(void)
{
	unsigned long megs = PAGES_TO_MB(totalram_pages());
	/* 只在启动期计算总 RAM；随后 page_cluster 通过 sysctl 可独立于内存热插拔调整。 */

	/* Use a smaller cluster for small-memory machines */
	if (megs < 16)
		/* 小内存机器降低连续换页规模，避免预读聚合吞掉过多可用页。 */
		page_cluster = 2;
	else
		/* 默认三位指数即八页，作为吞吐与无用预读之间的通用折中。 */
		page_cluster = 3;
	/*
	 * Right now other parts of the system means that we
	 * _really_ don't want to cluster much more
	 */

	register_sysctl_init("vm", swap_sysctl_table);
	/* 注册后 sysctl core 持有表的静态生命周期；启动函数不需要也不能释放该描述。 */
}
