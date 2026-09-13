// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/mmu_notifier.c
 *
 *  Copyright (C) 2008  Qumranet, Inc.
 *  Copyright (C) 2008  SGI
 *             Christoph Lameter <cl@gentwo.org>
 */

#include <linux/rculist.h>
#include <linux/mmu_notifier.h>
/* RCU/SRCU 与公开 notifier ABI 分别保护回调遍历和驱动可见的注册、区间回调协议。 */
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/interval_tree.h>
#include <linux/srcu.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
/* mm 生命周期、mmap 锁与对象分配共同约束 subscriptions 的发布、销毁和等待路径。 */

#include "vma.h"

/* global SRCU for all MMs */
/* 单一 SRCU 域让所有 mm 的回调读取者与 unregister/异步 free 使用同一宽限期协议。 */
DEFINE_STATIC_SRCU(srcu);

#ifdef CONFIG_LOCKDEP
struct lockdep_map __mmu_notifier_invalidate_range_start_map = {
	.name = "mmu_notifier_invalidate_range_start"
};
#endif

/*
 * The mmu_notifier_subscriptions structure is allocated and installed in
 * mm->notifier_subscriptions inside the mm_take_all_locks() protected
 * critical section and it's released only when mm_count reaches zero
 * in mmdrop().
 */
struct mmu_notifier_subscriptions {
	/* 每个 mm 的 notifier 聚合体：list 服务传统广播，itree 服务 VA 精确订阅；仅 mmdrop 后销毁。 */
	/* all mmu notifiers registered in this mm are queued in this list */
	struct hlist_head list;
	/* has_itree 单调置位，避免没有 interval 用户的 mm 承担树失效序列维护成本。 */
	bool has_itree;
	/* to serialize the list modifications and hlist_unhashed */
	spinlock_t lock;
	/* lock 串行 list/tree/deferred 变更及 invalidate_seq；回调本身绝不在该自旋锁内执行。 */
	unsigned long invalidate_seq;
	/* 奇数表示全排他失效窗口，偶数表示空闲或仅部分失效；读侧以快照比较决定重试。 */
	unsigned long active_invalidate_ranges;
	/* 嵌套和并行 start/end 的引用计数；归零的最终 end 才能提交 deferred tree 更新。 */
	struct rb_root_cached itree;
	/* interval tree 只存区间 notifier；在奇数失效期间冻结结构，防止遍历游标失效。 */
	wait_queue_head_t wq;
	struct hlist_head deferred_list;
	/* 插入/删除在树冻结时排队，最终 inv_end 以确定顺序应用，避免 start 路径睡眠或活锁。 */
};

/*
 * This is a collision-retry read-side/write-side 'lock', a lot like a
 * seqcount, however this allows multiple write-sides to hold it at
 * once. Conceptually the write side is protecting the values of the PTEs in
 * this mm, such that PTES cannot be read into SPTEs (shadow PTEs) while any
 * writer exists.
 *
 * Note that the core mm creates nested invalidate_range_start()/end() regions
 * within the same thread, and runs invalidate_range_start()/end() in parallel
 * on multiple CPUs. This is designed to not reduce concurrency or block
 * progress on the mm side.
 *
 * As a secondary function, holding the full write side also serves to prevent
 * writers for the itree, this is an optimization to avoid extra locking
 * during invalidate_range_start/end notifiers.
 *
 * The write side has two states, fully excluded:
 *  - mm->active_invalidate_ranges != 0
 *  - subscriptions->invalidate_seq & 1 == True (odd)
 *  - some range on the mm_struct is being invalidated
 *  - the itree is not allowed to change
 *
 * And partially excluded:
 *  - mm->active_invalidate_ranges != 0
 *  - subscriptions->invalidate_seq & 1 == False (even)
 *  - some range on the mm_struct is being invalidated
 *  - the itree is allowed to change
 *
 * Operations on notifier_subscriptions->invalidate_seq (under spinlock):
 *    seq |= 1  # Begin writing
 *    seq++     # Release the writing state
 *    seq & 1   # True if a writer exists
 *
 * The later state avoids some expensive work on inv_end in the common case of
 * no mmu_interval_notifier monitoring the VA.
 */
static bool
mn_itree_is_invalidating(struct mmu_notifier_subscriptions *subscriptions)
{
	/* 调用者已持 subscriptions->lock；只读取奇偶状态，不能替代 active_invalidate_ranges 的生命周期判断。 */
	lockdep_assert_held(&subscriptions->lock);
	return subscriptions->invalidate_seq & 1;
}

static struct mmu_interval_notifier *
mn_itree_inv_start_range(struct mmu_notifier_subscriptions *subscriptions,
			 const struct mmu_notifier_range *range,
			 unsigned long *seq)
{
	/* 开始 interval 失效遍历：递增活跃数，命中首区间时置奇 seq，并把本轮序号交给回调。 */
	struct interval_tree_node *node;
	struct mmu_interval_notifier *res = NULL;

	spin_lock(&subscriptions->lock);
	/* 树定位与 seq 发布共用锁，插入/删除要么看见冻结而排队，要么看见稳定树。 */
	subscriptions->active_invalidate_ranges++;
	node = interval_tree_iter_first(&subscriptions->itree, range->start,
					range->end - 1);
	if (node) {
		/* 只有实际命中订阅时才进入全排他奇数状态，未监控 VA 的失效无需冻结 itree。 */
		subscriptions->invalidate_seq |= 1;
		/* 保存首节点后立刻释放锁；真正驱动回调在锁外，允许其取得 user_lock 或睡眠。 */
		res = container_of(node, struct mmu_interval_notifier,
				   interval_tree);
	}

	*seq = subscriptions->invalidate_seq;
	/* seq 在解锁前复制给调用者，后续 start 回调即使改变私有订阅状态也不会改写本轮全局世代。 */
	spin_unlock(&subscriptions->lock);
	return res;
}

static struct mmu_interval_notifier *
mn_itree_inv_next(struct mmu_interval_notifier *interval_sub,
		  const struct mmu_notifier_range *range)
{
	/* 基于当前节点找下一个相交区间；本轮树冻结，迭代游标不会被并发 insert/remove 破坏。 */
	struct interval_tree_node *node;

	node = interval_tree_iter_next(&interval_sub->interval_tree,
				       range->start, range->end - 1);
	if (!node)
		return NULL;
	return container_of(node, struct mmu_interval_notifier, interval_tree);
}

static void mn_itree_inv_end(struct mmu_notifier_subscriptions *subscriptions)
{
	/* 结束一层失效：仅最外层且树曾冻结时翻转 seq 为偶数、提交 deferred 更新并唤醒等待者。 */
	struct mmu_interval_notifier *interval_sub;
	struct hlist_node *next;

	spin_lock(&subscriptions->lock);
	if (--subscriptions->active_invalidate_ranges ||
	    !mn_itree_is_invalidating(subscriptions)) {
		spin_unlock(&subscriptions->lock);
		return;
	}

	/* Make invalidate_seq even */
	subscriptions->invalidate_seq++;
	/* 奇→偶是树重新可变和读者可完成碰撞判断的发布点，仍在锁内序列化队列处理。 */

	/*
	 * The inv_end incorporates a deferred mechanism like rtnl_unlock().
	 * Adds and removes are queued until the final inv_end happens then
	 * they are progressed. This arrangement for tree updates is used to
	 * avoid using a blocking lock during invalidate_range_start.
	 */
	hlist_for_each_entry_safe(interval_sub, next,
				  &subscriptions->deferred_list,
				  deferred_item) {
		if (RB_EMPTY_NODE(&interval_sub->interval_tree.rb))
			/* 空 RB 节点代表 deferred insert；非空代表 deferred remove，两种状态由 insert/remove 编码。 */
			interval_tree_insert(&interval_sub->interval_tree,
					     &subscriptions->itree);
		else
			interval_tree_remove(&interval_sub->interval_tree,
					     &subscriptions->itree);
		hlist_del(&interval_sub->deferred_item);
	}
	spin_unlock(&subscriptions->lock);

	wake_up_all(&subscriptions->wq);
	/* 锁外唤醒让等待者重读 seq/树状态，避免在自旋锁持有时调度。 */
}

/**
 * mmu_interval_read_begin - Begin a read side critical section against a VA
 *                           range
 * @interval_sub: The interval subscription
 *
 * mmu_iterval_read_begin()/mmu_iterval_read_retry() implement a
 * collision-retry scheme similar to seqcount for the VA range under
 * subscription. If the mm invokes invalidation during the critical section
 * then mmu_interval_read_retry() will return true.
 *
 * This is useful to obtain shadow PTEs where teardown or setup of the SPTEs
 * require a blocking context.  The critical region formed by this can sleep,
 * and the required 'user_lock' can also be a sleeping lock.
 *
 * The caller is required to provide a 'user_lock' to serialize both teardown
 * and setup.
 *
 * The return value should be passed to mmu_interval_read_retry().
 */
unsigned long
mmu_interval_read_begin(struct mmu_interval_notifier *interval_sub)
{
	/* 读侧起点返回 read_retry 的 seq 快照；可睡眠等待当前失效窗口结束，user_lock 由调用者提供。 */
	struct mmu_notifier_subscriptions *subscriptions =
		interval_sub->mm->notifier_subscriptions;
	unsigned long seq;
	bool is_invalidating;

	/*
	 * If the subscription has a different seq value under the user_lock
	 * than we started with then it has collided.
	 *
	 * If the subscription currently has the same seq value as the
	 * subscriptions seq, then it is currently between
	 * invalidate_start/end and is colliding.
	 *
	 * The locking looks broadly like this:
	 *   mn_itree_inv_start():                 mmu_interval_read_begin():
	 *                                         spin_lock
	 *                                          seq = READ_ONCE(interval_sub->invalidate_seq);
	 *                                          seq == subs->invalidate_seq
	 *                                         spin_unlock
	 *    spin_lock
	 *     seq = ++subscriptions->invalidate_seq
	 *    spin_unlock
	 *     op->invalidate():
	 *       user_lock
	 *        mmu_interval_set_seq()
	 *         interval_sub->invalidate_seq = seq
	 *       user_unlock
	 *
	 *                          [Required: mmu_interval_read_retry() == true]
	 *
	 *   mn_itree_inv_end():
	 *    spin_lock
	 *     seq = ++subscriptions->invalidate_seq
	 *    spin_unlock
	 *
	 *                                        user_lock
	 *                                         mmu_interval_read_retry():
	 *                                          interval_sub->invalidate_seq != seq
	 *                                        user_unlock
	 *
	 * Barriers are not needed here as any races here are closed by an
	 * eventual mmu_interval_read_retry(), which provides a barrier via the
	 * user_lock.
	 */
	spin_lock(&subscriptions->lock);
	/* 私有 seq 与全局 seq 必须在同一锁域读取，才能区分已碰撞和正在 start/end 窗口。 */
	/* Pairs with the WRITE_ONCE in mmu_interval_set_seq() */
	seq = READ_ONCE(interval_sub->invalidate_seq);
	is_invalidating = seq == subscriptions->invalidate_seq;
	spin_unlock(&subscriptions->lock);

	/*
	 * interval_sub->invalidate_seq must always be set to an odd value via
	 * mmu_interval_set_seq() using the provided cur_seq from
	 * mn_itree_inv_start_range(). This ensures that if seq does wrap we
	 * will always clear the below sleep in some reasonable time as
	 * subscriptions->invalidate_seq is even in the idle state.
	 */
	lock_map_acquire(&__mmu_notifier_invalidate_range_start_map);
	lock_map_release(&__mmu_notifier_invalidate_range_start_map);
	if (is_invalidating)
		/* 等待 seq 改变而非仅等偶数，确保嵌套/回绕时不会把本轮正在失效的订阅误判为稳定。 */
		wait_event(subscriptions->wq,
			   READ_ONCE(subscriptions->invalidate_seq) != seq);

	/*
	 * Notice that mmu_interval_read_retry() can already be true at this
	 * point, avoiding loops here allows the caller to provide a global
	 * time bound.
	 */

	return seq;
}
EXPORT_SYMBOL_GPL(mmu_interval_read_begin);

static void mn_itree_finish_pass(struct llist_head *finish_passes)
{
	/* start 回调可请求延后 finish；无锁链表反转后按收集顺序执行，回调不持 subscriptions 锁。 */
	struct llist_node *first = llist_reverse_order(__llist_del_all(finish_passes));
	struct mmu_interval_notifier_finish *f, *next;

	llist_for_each_entry_safe(f, next, first, link)
		f->notifier->ops->invalidate_finish(f);
}

static void mn_itree_release(struct mmu_notifier_subscriptions *subscriptions,
			     struct mm_struct *mm)
{
	/* mm 销毁时对全 VA 范围广播 interval release；所有订阅清 SPTE 后以统一 inv_end 解除冻结。 */
	struct mmu_notifier_range range = {
		/* release 范围覆盖 [0, ULONG_MAX)，事件类型让驱动与普通 PTE 失效区分终局拆除。 */
		.flags = MMU_NOTIFIER_RANGE_BLOCKABLE,
		.event = MMU_NOTIFY_RELEASE,
		.mm = mm,
		.start = 0,
		.end = ULONG_MAX,
	};
	/* 这个栈上 range 仅在本函数同步回调期间借用；finish token 必须在它离开作用域前被消费。 */
	struct mmu_interval_notifier *interval_sub;
	LLIST_HEAD(finish_passes);
	unsigned long cur_seq;
	bool ret;

	for (interval_sub =
		     mn_itree_inv_start_range(subscriptions, &range, &cur_seq);
	     interval_sub;
	     interval_sub = mn_itree_inv_next(interval_sub, &range)) {
		if (interval_sub->ops->invalidate_start) {
			/* 每次迭代只处理与全空间相交的单个订阅，mn_itree_inv_next 在回调后继续使用冻结树。 */
			/* cur_seq 对整轮固定，驱动据此标记自己已观察到哪个全局失效世代。 */
			struct mmu_interval_notifier_finish *finish = NULL;

			ret = interval_sub->ops->invalidate_start(interval_sub,
								  &range,
								  cur_seq,
								  &finish);
			if (ret && finish) {
				/* 只有成功 start 的 finish 才合法；失败回调不得遗留异步完成工作。 */
				finish->notifier = interval_sub;
				__llist_add(&finish->link, &finish_passes);
			}

		} else {
			/* 旧 invalidate ABI 同步完成工作，ret 仍须为真以满足 release 不可拒绝的约定。 */
			ret = interval_sub->ops->invalidate(interval_sub,
							    &range,
							    cur_seq);
		}
		/* release 返回值为 false 仅触发 WARN；不能中断后续订阅，否则会留下仍指向即将销毁 mm 的 SPTE。 */
		WARN_ON(!ret);
	}

	mn_itree_finish_pass(&finish_passes);
	/* 全部 start/invalidate 返回后才执行 finish，保持每个驱动的失效准备与完成阶段顺序。 */
	mn_itree_inv_end(subscriptions);
}

/*
 * This function can't run concurrently against mmu_notifier_register
 * because mm->mm_users > 0 during mmu_notifier_register and exit_mmap
 * runs with mm_users == 0. Other tasks may still invoke mmu notifiers
 * in parallel despite there being no task using this mm any more,
 * through the vmas outside of the exit_mmap context, such as with
 * vmtruncate. This serializes against mmu_notifier_unregister with
 * the notifier_subscriptions->lock in addition to SRCU and it serializes
 * against the other mmu notifiers with SRCU. struct mmu_notifier_subscriptions
 * can't go away from under us as exit_mmap holds an mm_count pin
 * itself.
 */
static void mn_hlist_release(struct mmu_notifier_subscriptions *subscriptions,
			     struct mm_struct *mm)
{
	/* exit_mmap 释放传统 notifier：SRCU 包住 release 广播，随后锁内摘链并同步宽限期，阻止页释放早于驱动清 SPTE。 */
	struct mmu_notifier *subscription;
	int id;

	/*
	 * SRCU here will block mmu_notifier_unregister until
	 * ->release returns.
	 */
	id = srcu_read_lock(&srcu);
	/* SRCU 既让 unregister 等待当前 release，也保护 hlist 遍历期间 subscription 不被异步 free。 */
	hlist_for_each_entry_srcu(subscription, &subscriptions->list, hlist,
				 srcu_read_lock_held(&srcu))
		/*
		 * If ->release runs before mmu_notifier_unregister it must be
		 * handled, as it's the only way for the driver to flush all
		 * existing sptes and stop the driver from establishing any more
		 * sptes before all the pages in the mm are freed.
		 */
		if (subscription->ops->release)
			/* release 是驱动停止建立新 SPTE、清空旧 SPTE 的唯一终局通知，必须先于 mm 页释放。 */
			subscription->ops->release(subscription, mm);
	/* 即便某订阅无 release，后续锁内摘链也禁止它在 mm 已销毁阶段再收到其他广播。 */

	spin_lock(&subscriptions->lock);
	/* 回调返回后才摘链；并发 unregister 若先到达会发现已摘链而只等待 SRCU。 */
	while (unlikely(!hlist_empty(&subscriptions->list))) {
		/* 从表头逐个摘除而不 free 对象；对象所有者的 unregister/put 会在 SRCU 后执行释放。 */
		subscription = hlist_entry(subscriptions->list.first,
					   struct mmu_notifier, hlist);
		/*
		 * We arrived before mmu_notifier_unregister so
		 * mmu_notifier_unregister will do nothing other than to wait
		 * for ->release to finish and for mmu_notifier_unregister to
		 * return.
		 */
		hlist_del_init_rcu(&subscription->hlist);
	}
	spin_unlock(&subscriptions->lock);
	srcu_read_unlock(&srcu, id);

	/*
	 * synchronize_srcu here prevents mmu_notifier_release from returning to
	 * exit_mmap (which would proceed with freeing all pages in the mm)
	 * until the ->release method returns, if it was invoked by
	 * mmu_notifier_unregister.
	 *
	 * The notifier_subscriptions can't go away from under us because
	 * one mm_count is held by exit_mmap.
	 */
	synchronize_srcu(&srcu);
	/* 宽限期保证任何由 unregister 触发的 release 已返回，exit_mmap 才可继续释放页表/页。 */
}

void __mmu_notifier_release(struct mm_struct *mm)
{
	/* mm teardown 总入口：interval 先覆盖全地址空间，再处理传统 hlist，二者共享同一个 subscriptions 生命周期。 */
	struct mmu_notifier_subscriptions *subscriptions =
		mm->notifier_subscriptions;

	if (subscriptions->has_itree)
		mn_itree_release(subscriptions, mm);

	if (!hlist_empty(&subscriptions->list))
		mn_hlist_release(subscriptions, mm);
}

/*
 * If no young bitflag is supported by the hardware, ->clear_flush_young can
 * unmap the address and return 1 or 0 depending if the mapping previously
 * existed or not.
 */
bool __mmu_notifier_clear_flush_young(struct mm_struct *mm,
		unsigned long start, unsigned long end)
{
	/* 广播清年轻位并可能执行驱动 TLB flush；任一订阅报告曾 young 即累积 true。 */
	struct mmu_notifier *subscription;
	bool young = false;
	int id;

	id = srcu_read_lock(&srcu);
	hlist_for_each_entry_srcu(subscription,
				 &mm->notifier_subscriptions->list, hlist,
				 srcu_read_lock_held(&srcu)) {
		if (subscription->ops->clear_flush_young)
			/* 回调可按硬件能力退化为 unmap；返回值表示失效前是否存在映射，不是操作是否成功。 */
			young |= subscription->ops->clear_flush_young(
				subscription, mm, start, end);
	}
	srcu_read_unlock(&srcu, id);

	return young;
}

bool __mmu_notifier_clear_young(struct mm_struct *mm,
		unsigned long start, unsigned long end)
{
	/* 不要求 flush 的年轻位清除广播；SRCU 保护 ops 指针和 hlist 节点，聚合任意 true。 */
	struct mmu_notifier *subscription;
	bool young = false;
	int id;

	id = srcu_read_lock(&srcu);
	/* clear_young 不触发额外内存管理动作，返回值只聚合驱动观察到的访问历史。 */
	hlist_for_each_entry_srcu(subscription,
				 &mm->notifier_subscriptions->list, hlist,
				 srcu_read_lock_held(&srcu)) {
		if (subscription->ops->clear_young)
			/* 缺少此可选回调的订阅不影响其它驱动；OR 允许多个 secondary mapping 独立报告。 */
			young |= subscription->ops->clear_young(subscription,
								mm, start, end);
	}
	srcu_read_unlock(&srcu, id);

	return young;
}

bool __mmu_notifier_test_young(struct mm_struct *mm,
		unsigned long address)
{
	/* 查询任意 secondary mapping 的访问位；首个 true 可提前结束遍历，false 仍需问完所有订阅。 */
	struct mmu_notifier *subscription;
	bool young = false;
	int id;

	id = srcu_read_lock(&srcu);
	/* 读查询同样须在 SRCU 域内，避免 unregister 在 ops 调用期间释放驱动对象。 */
	hlist_for_each_entry_srcu(subscription,
				 &mm->notifier_subscriptions->list, hlist,
				 srcu_read_lock_held(&srcu)) {
		if (subscription->ops->test_young) {
			/* true 已满足“任一 mapping young”的语义，提前退出降低多设备共享 mm 的查询成本。 */
			young = subscription->ops->test_young(subscription, mm,
							      address);
			if (young)
				break;
		}
	}
	/* 遍历结束后再离开 SRCU，young 的最终值只反映本次查询期间仍有效订阅的并集。 */
	srcu_read_unlock(&srcu, id);

	return young;
}

static int mn_itree_invalidate(struct mmu_notifier_subscriptions *subscriptions,
			       const struct mmu_notifier_range *range)
{
	/* 对相交 interval 执行 start/invalidate；非阻塞回调拒绝时以 -EAGAIN 结束并自行配对 inv_end。 */
	struct mmu_interval_notifier *interval_sub;
	LLIST_HEAD(finish_passes);
	unsigned long cur_seq;
	int err = 0;

	for (interval_sub =
		     mn_itree_inv_start_range(subscriptions, range, &cur_seq);
	     interval_sub;
	     interval_sub = mn_itree_inv_next(interval_sub, range)) {
		bool ret;

		/* cur_seq 同轮共享，允许多个驱动把各自 invalidate_seq 设置为同一碰撞点。 */

		if (interval_sub->ops->invalidate_start) {
			/* 新型回调可返回 finish token；token 只在回调接受工作时排进本轮 finish 链。 */
			struct mmu_interval_notifier_finish *finish = NULL;

			ret = interval_sub->ops->invalidate_start(interval_sub,
			/* 回调在 tree 锁外，可阻塞与否由 range flags 决定；返回 false 仅允许 non-blocking 重试。 */
								  range,
								  cur_seq,
								  &finish);
			if (ret && finish) {
				finish->notifier = interval_sub;
				__llist_add(&finish->link, &finish_passes);
			}

		} else {
			/* 无 start 的旧 interval 回调一次完成失效，不能产出 finish token。 */
			ret = interval_sub->ops->invalidate(interval_sub,
							    range,
							    cur_seq);
		}
		if (!ret) {
			/* blockable range 不允许被拒绝；仅 non-blocking 的 false 转换成调用者可重试的 -EAGAIN。 */
			if (WARN_ON(mmu_notifier_range_blockable(range)))
				continue;
			err = -EAGAIN;
			/* 停在首个拒绝者，已接受的 start 仍由下面 finish 和 inv_end 收尾。 */
			break;
		}
	}

	mn_itree_finish_pass(&finish_passes);
	/* 即使后续订阅拒绝，也要完成先前成功订阅的 finish，不能把驱动留在半失效阶段。 */

	/*
	 * On -EAGAIN the non-blocking caller is not allowed to call
	 * invalidate_range_end()
	 */
	if (err)
		/* 非阻塞失败者不得再调用 range_end，因此本层必须立即结束计数、解冻可能的 tree。 */
		mn_itree_inv_end(subscriptions);

	return err;
}

static int mn_hlist_invalidate_range_start(
	struct mmu_notifier_subscriptions *subscriptions,
	struct mmu_notifier_range *range)
{
	/* 传统 notifier 的 start 广播：在 SRCU 下逐个调用，non-blocking 回调失败后仍要补 end 给已成功者。 */
	struct mmu_notifier *subscription;
	int ret = 0;
	int id;

	id = srcu_read_lock(&srcu);
	hlist_for_each_entry_srcu(subscription, &subscriptions->list, hlist,
				 srcu_read_lock_held(&srcu)) {
		const struct mmu_notifier_ops *ops = subscription->ops;

		if (ops->invalidate_range_start) {
			/* non_block 标记让 lockdep/调试检测回调是否违反不可睡眠约束。 */
			int _ret;

			if (!mmu_notifier_range_blockable(range))
				non_block_start();
			_ret = ops->invalidate_range_start(subscription, range);
			if (!mmu_notifier_range_blockable(range))
				non_block_end();
			if (_ret) {
				/* EAGAIN 无法识别哪些订阅已建立 start 状态，约定失败 start 不能拥有 end，最终统一补偿。 */
				pr_info("%pS callback failed with %d in %sblockable context.\n",
					ops->invalidate_range_start, _ret,
					!mmu_notifier_range_blockable(range) ?
						"non-" :
						"");
				/* 日志保留具体回调符号和阻塞属性，便于驱动区分 API 误用与暂时资源冲突。 */
				WARN_ON(mmu_notifier_range_blockable(range) ||
					_ret != -EAGAIN);
				/*
				 * We call all the notifiers on any EAGAIN,
				 * there is no way for a notifier to know if
				 * its start method failed, thus a start that
				 * does EAGAIN can't also do end.
				 */
				WARN_ON(ops->invalidate_range_end);
				ret = _ret;
			}
		}
		/* 每个 notifier 即使没有 start 回调也被遍历，保证 ret 的处理不改变其它订阅的可见性。 */
	}

	if (ret) {
		/* 仅 non-blocking 会到此；所有带 end 的订阅都获得配对回调，避免驱动遗留失效窗口。 */
		/* ret 保留最后一个 EAGAIN，调用者只需知道整轮不可阻塞失效没有完全建立。 */
		/*
		 * Must be non-blocking to get here.  If there are multiple
		 * notifiers and one or more failed start, any that succeeded
		 * start are expecting their end to be called.  Do so now.
		 */
		hlist_for_each_entry_srcu(subscription, &subscriptions->list,
			/* 补偿遍历不判断哪个 start 成功：API 约定失败者没有 end，因此全调用 end 是安全的。 */
					 hlist, srcu_read_lock_held(&srcu)) {
			if (!subscription->ops->invalidate_range_end)
				continue;

			/* 补偿 end 运行在同一 SRCU 域；驱动可据 range 识别本轮失败并撤销已建立的临时失效状态。 */
			subscription->ops->invalidate_range_end(subscription,
								range);
		}
	}
	srcu_read_unlock(&srcu, id);

	return ret;
}

int __mmu_notifier_invalidate_range_start(struct mmu_notifier_range *range)
{
	/* MM 失效入口先通知精确 interval（其失败可阻断），再广播传统订阅；无订阅时为零成本成功。 */
	struct mmu_notifier_subscriptions *subscriptions =
		range->mm->notifier_subscriptions;
	int ret;

	/* subscriptions 已由外层 MM 路径保证存在；本函数不负责首次注册或对空指针做延迟初始化。 */

	if (subscriptions->has_itree) {
		/* has_itree 一经开启保持真；没有实际相交节点时 helper 仍正确平衡 active_invalidate_ranges。 */
		/* interval 的 -EAGAIN 优先上返，hlist start 尚未执行，调用者可原子地整体重试范围操作。 */
		ret = mn_itree_invalidate(subscriptions, range);
		if (ret)
			return ret;
	}
	if (!hlist_empty(&subscriptions->list))
		/* hlist 回调完成后成功返回，range_end 将按调用者协议随后广播。 */
		return mn_hlist_invalidate_range_start(subscriptions, range);
	return 0;
	/* 空表/空树无需 SRCU 或自旋锁，调用方仍必须按上层 MM 协议决定是否调用 range_end。 */
}

static void
mn_hlist_invalidate_end(struct mmu_notifier_subscriptions *subscriptions,
			struct mmu_notifier_range *range)
{
	/* 与 hlist start 配对的 end 广播；non-blocking 标记在每个回调周围建立，SRCU 覆盖全遍历。 */
	struct mmu_notifier *subscription;
	int id;

	id = srcu_read_lock(&srcu);
	/* end 不改变 hlist，仅在 SRCU 保护下通知仍注册的订阅完成本轮失效。 */
	hlist_for_each_entry_srcu(subscription, &subscriptions->list, hlist,
				 srcu_read_lock_held(&srcu)) {
		if (subscription->ops->invalidate_range_end) {
			/* optional end 只属于实现了 start/end 协议的 notifier，secondary-TLB-only 订阅不会进入。 */
			if (!mmu_notifier_range_blockable(range))
				non_block_start();
			subscription->ops->invalidate_range_end(subscription,
								range);
			if (!mmu_notifier_range_blockable(range))
				non_block_end();
		}
	}
	/* 所有 end 回调完成后才退出 SRCU，使 unregister 返回边界覆盖这一整轮结束广播。 */
	srcu_read_unlock(&srcu, id);
}

void __mmu_notifier_invalidate_range_end(struct mmu_notifier_range *range)
{
	/* MM 失效结束入口：lockdep map 验证 start/end 配对，interval 先解冻再通知传统 end。 */
	struct mmu_notifier_subscriptions *subscriptions =
		range->mm->notifier_subscriptions;

	lock_map_acquire(&__mmu_notifier_invalidate_range_start_map);
	/* lockdep 伪锁跨越调用者的 start/end，诊断漏配对或错误嵌套；不提供实际互斥。 */
	if (subscriptions->has_itree)
		/* 先结束 interval 使等待 read_begin/remove 可推进，再让传统 end 回调取得可能相同的驱动锁。 */
		mn_itree_inv_end(subscriptions);

	if (!hlist_empty(&subscriptions->list))
		mn_hlist_invalidate_end(subscriptions, range);
	lock_map_release(&__mmu_notifier_invalidate_range_start_map);
}

void __mmu_notifier_arch_invalidate_secondary_tlbs(struct mm_struct *mm,
					unsigned long start, unsigned long end)
{
	/* 架构专用 secondary TLB 失效广播与普通 range 回调互斥注册，避免同一驱动接收重复语义。 */
	struct mmu_notifier *subscription;
	int id;

	id = srcu_read_lock(&srcu);
	/* 地址区间按架构单位解释，核心只负责将 [start,end) 原样交给各驱动 secondary TLB 实现。 */
	/* 架构 secondary TLB 广播不走 interval tree；其 list 遍历仍使用 SRCU 防止并发摘链。 */
	hlist_for_each_entry_srcu(subscription,
				 &mm->notifier_subscriptions->list, hlist,
				 srcu_read_lock_held(&srcu)) {
		if (subscription->ops->arch_invalidate_secondary_tlbs)
			/* start/end 与 arch 回调在注册时被排他约束，此处无需额外去重或配对。 */
			subscription->ops->arch_invalidate_secondary_tlbs(
				subscription, mm,
				start, end);
	}
	srcu_read_unlock(&srcu, id);
}

/*
 * Same as mmu_notifier_register but here the caller must hold the mmap_lock in
 * write mode. A NULL mn signals the notifier is being registered for itree
 * mode.
 */
int __mmu_notifier_register(struct mmu_notifier *subscription,
			    struct mm_struct *mm)
{
	/* mmap 写锁版本的注册核心：必要时分配 subscriptions，借 mm_count 给普通 notifier，并在全锁保护下发布 hlist/itree 模式。 */
	struct mmu_notifier_subscriptions *subscriptions = NULL;
	int ret;

	mmap_assert_write_locked(mm);
	/* 写 mmap_lock 稳定 notifier_subscriptions 指针；mm_users 正值阻止 exit_mmap/release 与本次注册并发。 */
	BUG_ON(atomic_read(&mm->mm_users) <= 0);

	/*
	 * Subsystems should only register for invalidate_secondary_tlbs() or
	 * invalidate_range_start()/end() callbacks, not both.
	 */
	if (WARN_ON_ONCE(subscription &&
			 (subscription->ops->arch_invalidate_secondary_tlbs &&
			 (subscription->ops->invalidate_range_start ||
			  subscription->ops->invalidate_range_end))))
		return -EINVAL;
	/* secondary-TLB 与普通 range 回调的失效责任不可混用，同一 notifier 注册两者会导致重复/错序 flush。 */

	if (!mm->notifier_subscriptions) {
		/* 分配在 mm_take_all_locks 前完成，因为该锁域不能睡眠；失败没有对 mm 发布任何半初始化对象。 */
		/*
		 * kmalloc cannot be called under mm_take_all_locks(), but we
		 * know that mm->notifier_subscriptions can't change while we
		 * hold the write side of the mmap_lock.
		 */
		subscriptions = kzalloc_obj(struct mmu_notifier_subscriptions);
		if (!subscriptions)
			return -ENOMEM;

		INIT_HLIST_HEAD(&subscriptions->list);
		spin_lock_init(&subscriptions->lock);
		subscriptions->invalidate_seq = 2;
		subscriptions->itree = RB_ROOT_CACHED;
		init_waitqueue_head(&subscriptions->wq);
		INIT_HLIST_HEAD(&subscriptions->deferred_list);
		/* 初始 seq 取非零偶数，保证首个空闲订阅可得到相邻奇数快照且远离立即回绕。 */
	}

	ret = mm_take_all_locks(mm);
	/* 全锁排除其他 notifier 方法和 page-table 修改，建立 subscriptions 后才允许无锁读取者 acquire 访问。 */
	if (unlikely(ret))
		goto out_clean;

	/*
	 * Serialize the update against mmu_notifier_unregister. A
	 * side note: mmu_notifier_release can't run concurrently with
	 * us because we hold the mm_users pin (either implicitly as
	 * current->mm or explicitly with get_task_mm() or similar).
	 * We can't race against any other mmu notifier method either
	 * thanks to mm_take_all_locks().
	 *
	 * release semantics on the initialization of the
	 * mmu_notifier_subscriptions's contents are provided for unlocked
	 * readers.  acquire can only be used while holding the mmgrab or
	 * mmget, and is safe because once created the
	 * mmu_notifier_subscriptions is not freed until the mm is destroyed.
	 * As above, users holding the mmap_lock or one of the
	 * mm_take_all_locks() do not need to use acquire semantics.
	 */
	if (subscriptions)
		/* release store 是唯一公开新聚合体的点；初始化字段对 acquire 读取 interval insert 可见。 */
		smp_store_release(&mm->notifier_subscriptions, subscriptions);

	if (subscription) {
		/* 普通 notifier 各自持 mm_count pin；最后 unregister 或异步 put 的 mmdrop 与此严格配对。 */
		/* Pairs with the mmdrop in mmu_notifier_unregister_* */
		mmgrab(mm);
		subscription->mm = mm;
		subscription->users = 1;

		spin_lock(&mm->notifier_subscriptions->lock);
		hlist_add_head_rcu(&subscription->hlist,
				   &mm->notifier_subscriptions->list);
		spin_unlock(&mm->notifier_subscriptions->lock);
	} else
		/* NULL 注册仅开启 itree 能力，不加入 hlist 或额外持 mm_count；interval notifier 自己持 pin。 */
		mm->notifier_subscriptions->has_itree = true;

	mm_drop_all_locks(mm);
	BUG_ON(atomic_read(&mm->mm_users) <= 0);
	return 0;

out_clean:
	/* 未发布的临时聚合体可直接释放；已发布路径由 mm 销毁统一回收。 */
	kfree(subscriptions);
	return ret;
}
EXPORT_SYMBOL_GPL(__mmu_notifier_register);

/**
 * mmu_notifier_register - Register a notifier on a mm
 * @subscription: The notifier to attach
 * @mm: The mm to attach the notifier to
 *
 * Must not hold mmap_lock nor any other VM related lock when calling
 * this registration function. Must also ensure mm_users can't go down
 * to zero while this runs to avoid races with mmu_notifier_release,
 * so mm has to be current->mm or the mm should be pinned safely such
 * as with get_task_mm(). If the mm is not current->mm, the mm_users
 * pin should be released by calling mmput after mmu_notifier_register
 * returns.
 *
 * mmu_notifier_unregister() or mmu_notifier_put() must be always called to
 * unregister the notifier.
 *
 * While the caller has a mmu_notifier get the subscription->mm pointer will remain
 * valid, and can be converted to an active mm pointer via mmget_not_zero().
 */
int mmu_notifier_register(struct mmu_notifier *subscription,
			  struct mm_struct *mm)
{
	/* 公共包装获取/释放 mmap 写锁，调用者不得持 VM 锁以免与内部全锁顺序相反。 */
	int ret;

	mmap_write_lock(mm);
	ret = __mmu_notifier_register(subscription, mm);
	mmap_write_unlock(mm);
	return ret;
}
EXPORT_SYMBOL_GPL(mmu_notifier_register);

static struct mmu_notifier *
find_get_mmu_notifier(struct mm_struct *mm, const struct mmu_notifier_ops *ops)
{
	/* 在锁内按 ops 去重复用 notifier，并递增 users；UINT_MAX 溢出显式返回错误而不回绕为零。 */
	struct mmu_notifier *subscription;

	spin_lock(&mm->notifier_subscriptions->lock);
	hlist_for_each_entry_srcu(subscription,
				 &mm->notifier_subscriptions->list, hlist,
				 lockdep_is_held(&mm->notifier_subscriptions->lock)) {
		if (subscription->ops != ops)
			/* ops 指针是单例键，不比较驱动私有字段；同一驱动跨调用者共享一份 notifier。 */
			continue;

		if (likely(subscription->users != UINT_MAX))
			/* get/put 引用只保护 notifier 对象，不额外取得 mm_count；该 pin 属于首次注册。 */
			subscription->users++;
		else
			/* 溢出对象不再递增，返回 ERR_PTR 让调用者避免把 users 回绕成最后引用。 */
			subscription = ERR_PTR(-EOVERFLOW);
		spin_unlock(&mm->notifier_subscriptions->lock);
		return subscription;
	}
	spin_unlock(&mm->notifier_subscriptions->lock);
	return NULL;
}

/**
 * mmu_notifier_get_locked - Return the single struct mmu_notifier for
 *                           the mm & ops
 * @ops: The operations struct being subscribe with
 * @mm : The mm to attach notifiers too
 *
 * This function either allocates a new mmu_notifier via
 * ops->alloc_notifier(), or returns an already existing notifier on the
 * list. The value of the ops pointer is used to determine when two notifiers
 * are the same.
 *
 * Each call to mmu_notifier_get() must be paired with a call to
 * mmu_notifier_put(). The caller must hold the write side of mm->mmap_lock.
 *
 * While the caller has a mmu_notifier get the mm pointer will remain valid,
 * and can be converted to an active mm pointer via mmget_not_zero().
 */
struct mmu_notifier *mmu_notifier_get_locked(const struct mmu_notifier_ops *ops,
					     struct mm_struct *mm)
{
	/* 以 ops 为键获取或创建单例 notifier；失败时调用 free_notifier 回收尚未发布的驱动私有对象。 */
	struct mmu_notifier *subscription;
	int ret;

	mmap_assert_write_locked(mm);
	/* 写锁同时防止新注册和 mm teardown 改变 subscriptions，alloc_notifier 可据此安全绑定 mm 私有状态。 */

	if (mm->notifier_subscriptions) {
		/* 已有聚合体先尝试复用，避免同一 ops 被注册两次而获得重复失效回调。 */
		subscription = find_get_mmu_notifier(mm, ops);
		if (subscription)
			return subscription;
	}
	/* 未找到现有 ops 时才调用驱动 allocator；其返回对象必须由本函数设置 ops 并纳入标准注册路径。 */

	subscription = ops->alloc_notifier(mm);
	/* alloc_notifier 的对象尚不在 hlist；后续注册失败必须由 out_free 交还其全部私有资源。 */
	if (IS_ERR(subscription))
		return subscription;
	subscription->ops = ops;
	/* 仅在 allocator 成功后写 ops；__register 把它作为 hlist 广播和去重键，之后不可更改。 */
	ret = __mmu_notifier_register(subscription, mm);
	if (ret)
		goto out_free;
	/* 注册成功后 ownership 交给 hlist/users；此函数不再直接释放 subscription，调用者必须配对 put。 */
	return subscription;
out_free:
	subscription->ops->free_notifier(subscription);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(mmu_notifier_get_locked);

/* this is called after the last mmu_notifier_unregister() returned */
void __mmu_notifier_subscriptions_destroy(struct mm_struct *mm)
{
	/* 最后 notifier 已注销后销毁聚合体；BUG_ON 防止仍有 SRCU 可见 hlist 节点时提前 free。 */
	BUG_ON(!hlist_empty(&mm->notifier_subscriptions->list));
	kfree(mm->notifier_subscriptions);
	mm->notifier_subscriptions = LIST_POISON1; /* debug */
}

/*
 * This releases the mm_count pin automatically and frees the mm
 * structure if it was the last user of it. It serializes against
 * running mmu notifiers with SRCU and against mmu_notifier_unregister
 * with the unregister lock + SRCU. All sptes must be dropped before
 * calling mmu_notifier_unregister. ->release or any other notifier
 * method may be invoked concurrently with mmu_notifier_unregister,
 * and only after mmu_notifier_unregister returned we're guaranteed
 * that ->release or any other method can't run anymore.
 */
void mmu_notifier_unregister(struct mmu_notifier *subscription,
			     struct mm_struct *mm)
{
	/* 显式注销同步调用 release、摘 hlist、等待 SRCU，并归还注册时的 mm_count pin；调用者须先清 SPTE。 */
	BUG_ON(atomic_read(&mm->mm_count) <= 0);

	if (!hlist_unhashed(&subscription->hlist)) {
		/* 若 release 路径已先摘链，本路径只需等待其 SRCU 完成，不能重复调用驱动 release。 */
		/*
		 * SRCU here will force exit_mmap to wait for ->release to
		 * finish before freeing the pages.
		 */
		int id;

		id = srcu_read_lock(&srcu);
		/*
		 * exit_mmap will block in mmu_notifier_release to guarantee
		 * that ->release is called before freeing the pages.
		 */
		if (subscription->ops->release)
			/* 在 SRCU 读侧内调用，保证 exit_mmap 和并发 unregister 对 release 的先后/完成边界一致。 */
			subscription->ops->release(subscription, mm);
		srcu_read_unlock(&srcu, id);

		spin_lock(&mm->notifier_subscriptions->lock);
		/*
		 * Can not use list_del_rcu() since __mmu_notifier_release
		 * can delete it before we hold the lock.
		 */
		hlist_del_init_rcu(&subscription->hlist);
		/* 使用 init 版本容忍 release 先删节点；RCU 删除让现有广播遍历安全越过该订阅。 */
		spin_unlock(&mm->notifier_subscriptions->lock);
	}

	/*
	 * Wait for any running method to finish, of course including
	 * ->release if it was run by mmu_notifier_release instead of us.
	 */
	synchronize_srcu(&srcu);
	/* 返回前等待所有回调离开，调用者由此可释放其 notifier 私有资源和模块引用。 */

	BUG_ON(atomic_read(&mm->mm_count) <= 0);

	mmdrop(mm);
}
EXPORT_SYMBOL_GPL(mmu_notifier_unregister);

static void mmu_notifier_free_rcu(struct rcu_head *rcu)
{
	/* container_of 恢复延后对象；此时所有 SRCU 广播观察者已离开，free_notifier 可释放驱动内存。 */
	/* get/put 最后引用的异步回收回调：宽限期后驱动 free_notifier，再归还首次注册取得的 mm_count。 */
	struct mmu_notifier *subscription =
		container_of(rcu, struct mmu_notifier, rcu);
	struct mm_struct *mm = subscription->mm;

	subscription->ops->free_notifier(subscription);
	/* Pairs with the get in __mmu_notifier_register() */
	mmdrop(mm);
}

/**
 * mmu_notifier_put - Release the reference on the notifier
 * @subscription: The notifier to act on
 *
 * This function must be paired with each mmu_notifier_get(), it releases the
 * reference obtained by the get. If this is the last reference then process
 * to free the notifier will be run asynchronously.
 *
 * Unlike mmu_notifier_unregister() the get/put flow only calls ops->release
 * when the mm_struct is destroyed. Instead free_notifier is always called to
 * release any resources held by the user.
 *
 * As ops->release is not guaranteed to be called, the user must ensure that
 * all sptes are dropped, and no new sptes can be established before
 * mmu_notifier_put() is called.
 *
 * This function can be called from the ops->release callback, however the
 * caller must still ensure it is called pairwise with mmu_notifier_get().
 *
 * Modules calling this function must call mmu_notifier_synchronize() in
 * their __exit functions to ensure the async work is completed.
 */
void mmu_notifier_put(struct mmu_notifier *subscription)
{
	/* put 不调用 release：驱动必须在此前自行停止 SPTE 建立，release 仅保证 mm 被销毁时执行。 */
	/* 与每次 get 配对递减 users；最后一个引用先摘链，随后 call_srcu 延后 free，不能假定 release 已被调用。 */
	struct mm_struct *mm = subscription->mm;

	spin_lock(&mm->notifier_subscriptions->lock);
	if (WARN_ON(!subscription->users) || --subscription->users)
		/* users 非零时保持 hlist 可见；警告分支也不允许继续删除/重复 mmdrop。 */
		/* 引用下溢只警告并保持锁内状态；非最后 put 不改变 hlist 和 mm pin。 */
		goto out_unlock;
	hlist_del_init_rcu(&subscription->hlist);
	spin_unlock(&mm->notifier_subscriptions->lock);

	call_srcu(&srcu, &subscription->rcu, mmu_notifier_free_rcu);
	/* SRCU 回调与正在遍历的广播自动排序；模块退出需另调 mmu_notifier_synchronize。 */
	return;

out_unlock:
	spin_unlock(&mm->notifier_subscriptions->lock);
}
EXPORT_SYMBOL_GPL(mmu_notifier_put);

static int __mmu_interval_notifier_insert(
	struct mmu_interval_notifier *interval_sub, struct mm_struct *mm,
	struct mmu_notifier_subscriptions *subscriptions, unsigned long start,
	unsigned long length, const struct mmu_interval_notifier_ops *ops)
{
	/* 初始化一个 VA 区间订阅并持有 mm_count；若失效正进行则按奇偶状态直接插树或 deferred，保证 read_begin 必能检测碰撞。 */
	interval_sub->mm = mm;
	interval_sub->ops = ops;
	RB_CLEAR_NODE(&interval_sub->interval_tree.rb);
	interval_sub->interval_tree.start = start;
	/* start/last 初始化先于任何 tree/deferred 发布，回调若随后发现碰撞也能读取完整区间边界。 */
	/* interval tree 使用闭区间 last，length-1 的溢出检查防止 start+length 包绕成看似很小的范围。 */
	/*
	 * Note that the representation of the intervals in the interval tree
	 * considers the ending point as contained in the interval.
	 */
	if (length == 0 ||
	    check_add_overflow(start, length - 1,
			       &interval_sub->interval_tree.last))
		return -EOVERFLOW;
	/* last 是包含端点，故 length=1 恰监控 start 一个地址；check_add_overflow 覆盖 ULONG_MAX 边界。 */
	/* 无效长度在取得 mm pin 前退出，调用者无需执行 remove。 */

	/* Must call with a mmget() held */
	if (WARN_ON(atomic_read(&mm->mm_users) <= 0))
		return -EINVAL;

	/* pairs with mmdrop in mmu_interval_notifier_remove() */
	mmgrab(mm);
	/* 此 pin 覆盖订阅存在期，即使最后用户线程退出，remove 仍可安全访问 subscriptions 并配对 mmdrop。 */

	/*
	 * If some invalidate_range_start/end region is going on in parallel
	 * we don't know what VA ranges are affected, so we must assume this
	 * new range is included.
	 *
	 * If the itree is invalidating then we are not allowed to change
	 * it. Retrying until invalidation is done is tricky due to the
	 * possibility for live lock, instead defer the add to
	 * mn_itree_inv_end() so this algorithm is deterministic.
	 *
	 * In all cases the value for the interval_sub->invalidate_seq should be
	 * odd, see mmu_interval_read_begin()
	 */
	spin_lock(&subscriptions->lock);
	/* 树结构、deferred 链和订阅初始 invalidate_seq 必须作为一个锁内事务发布给并发失效读侧。 */
	if (subscriptions->active_invalidate_ranges) {
		/* 任意进行中的 range 都可能覆盖新订阅；新订阅从当前 seq 起被标记为已碰撞，读侧必须验证。 */
		if (mn_itree_is_invalidating(subscriptions))
			/* 奇数阶段迭代器正在走树，不能修改 RB 结构；只排 deferred，最终 inv_end 再真正插入。 */
			hlist_add_head(&interval_sub->deferred_item,
				       &subscriptions->deferred_list);
		else {
			/* 偶数但 active 非零表示其它无匹配 range；允许插树但先置奇 seq 使新订阅保守失效。 */
			subscriptions->invalidate_seq |= 1;
			interval_tree_insert(&interval_sub->interval_tree,
					     &subscriptions->itree);
		}
		interval_sub->invalidate_seq = subscriptions->invalidate_seq;
		/* 私有 seq 记录插入时观察到的世代；随后的 read_begin 会把它与全局 seq 比较并拒绝旧 SPTE。 */
	} else {
		/* 空闲树直接插入，私有 seq 设成全局偶数前一值（奇数）以避免与空闲全局 seq 相等。 */
		WARN_ON(mn_itree_is_invalidating(subscriptions));
		/*
		 * The starting seq for a subscription not under invalidation
		 * should be odd, not equal to the current invalidate_seq and
		 * invalidate_seq should not 'wrap' to the new seq any time
		 * soon.
		 */
		interval_sub->invalidate_seq =
			subscriptions->invalidate_seq - 1;
		/* 选取相邻奇数而非当前偶数，使第一次 read_begin 不会因“相等且正失效”的判定遗漏未来窗口。 */
		interval_tree_insert(&interval_sub->interval_tree,
				     &subscriptions->itree);
	}
	/* 插入或排队动作完成后才解锁；并发 start 要么看见完整 RB 节点，要么看见 deferred 链与对应 seq。 */
	spin_unlock(&subscriptions->lock);
	return 0;
}

/**
 * mmu_interval_notifier_insert - Insert an interval notifier
 * @interval_sub: Interval subscription to register
 * @start: Starting virtual address to monitor
 * @length: Length of the range to monitor
 * @mm: mm_struct to attach to
 * @ops: Interval notifier operations to be called on matching events
 *
 * This function subscribes the interval notifier for notifications from the
 * mm.  Upon return the ops related to mmu_interval_notifier will be called
 * whenever an event that intersects with the given range occurs.
 *
 * Upon return the range_notifier may not be present in the interval tree yet.
 * The caller must use the normal interval notifier read flow via
 * mmu_interval_read_begin() to establish SPTEs for this range.
 */
int mmu_interval_notifier_insert(struct mmu_interval_notifier *interval_sub,
				 struct mm_struct *mm, unsigned long start,
				 unsigned long length,
				 const struct mmu_interval_notifier_ops *ops)
{
	/* 非锁版本先 acquire 读取 subscriptions；尚未开启 itree 时走普通注册建立聚合体，再进入统一插入核心。 */
	struct mmu_notifier_subscriptions *subscriptions;
	int ret;

	WARN_ON_ONCE(ops->invalidate_start && !ops->invalidate_finish);
	/* start/finish 必须成对，驱动若提供 start 却无 finish 会让延后清理 token 无处消费。 */
	might_lock(&mm->mmap_lock);

	subscriptions = smp_load_acquire(&mm->notifier_subscriptions);
	/* acquire 与注册时 release store 配对，确保观察到非 NULL 时其锁、树根和 waitqueue 已初始化。 */
	if (!subscriptions || !subscriptions->has_itree) {
		/* 首个 interval 用户以 NULL notifier 注册开启 has_itree；mmap 锁由公共注册包装获取。 */
		ret = mmu_notifier_register(NULL, mm);
		if (ret)
			return ret;
		subscriptions = mm->notifier_subscriptions;
	}
	/* 注册成功后使用发布后的聚合体进入核心；失败时 interval_sub 尚未得到 mm pin 或树节点。 */
	return __mmu_interval_notifier_insert(interval_sub, mm, subscriptions,
					      start, length, ops);
}
EXPORT_SYMBOL_GPL(mmu_interval_notifier_insert);

int mmu_interval_notifier_insert_locked(
	struct mmu_interval_notifier *interval_sub, struct mm_struct *mm,
	unsigned long start, unsigned long length,
	const struct mmu_interval_notifier_ops *ops)
{
	/* 调用者已持 mmap 写锁的变体，避免嵌套拿锁；其余 itree 初始化和区间发布语义与非锁版本一致。 */
	struct mmu_notifier_subscriptions *subscriptions =
		mm->notifier_subscriptions;
	int ret;

	mmap_assert_write_locked(mm);
	/* 直接读 subscriptions 安全因为写 mmap_lock 排除发布/销毁；无需 acquire 原子读取。 */

	if (!subscriptions || !subscriptions->has_itree) {
		ret = __mmu_notifier_register(NULL, mm);
		if (ret)
			return ret;
		subscriptions = mm->notifier_subscriptions;
	}
	/* 锁版本同样只在 itree 已启用后插入；__register 保持 mmap 锁所有权在调用者一侧。 */
	return __mmu_interval_notifier_insert(interval_sub, mm, subscriptions,
					      start, length, ops);
}
EXPORT_SYMBOL_GPL(mmu_interval_notifier_insert_locked);

static bool
mmu_interval_seq_released(struct mmu_notifier_subscriptions *subscriptions,
			  unsigned long seq)
{
	/* 等待谓词在锁内比较 seq，避免移除者观察到奇数阶段尚未执行 deferred 更新就提前返回。 */
	bool ret;

	spin_lock(&subscriptions->lock);
	ret = subscriptions->invalidate_seq != seq;
	spin_unlock(&subscriptions->lock);
	return ret;
}

/**
 * mmu_interval_notifier_remove - Remove a interval notifier
 * @interval_sub: Interval subscription to unregister
 *
 * This function must be paired with mmu_interval_notifier_insert(). It cannot
 * be called from any ops callback.
 *
 * Once this returns ops callbacks are no longer running on other CPUs and
 * will not be called in future.
 */
void mmu_interval_notifier_remove(struct mmu_interval_notifier *interval_sub)
{
	/* 删除订阅：稳定树中直接移除；冻结时取消未插入项或改为 deferred remove，并等待该失效 seq 释放后才 mmdrop。 */
	struct mm_struct *mm = interval_sub->mm;
	struct mmu_notifier_subscriptions *subscriptions =
		mm->notifier_subscriptions;
	unsigned long seq = 0;

	might_sleep();
	/* remove 可能等待最终 inv_end，调用者若持驱动 invalidate 回调取得的锁会形成死锁。 */

	spin_lock(&subscriptions->lock);
	/* 与 start/end、并发 insert 共用锁，RB 节点是否为空编码“仅 deferred”与“已在 tree”两种状态。 */
	if (mn_itree_is_invalidating(subscriptions)) {
		/* 奇数阶段不能改树；已入树的删除先排队，尚未入树的插入则直接从 deferred 链撤销。 */
		/*
		 * remove is being called after insert put this on the
		 * deferred list, but before the deferred list was processed.
		 */
		if (RB_EMPTY_NODE(&interval_sub->interval_tree.rb)) {
			hlist_del(&interval_sub->deferred_item);
		} else {
			hlist_add_head(&interval_sub->deferred_item,
				       &subscriptions->deferred_list);
			seq = subscriptions->invalidate_seq;
		}
	} else {
		/* 稳定偶数阶段 RB 节点必须已入树；WARN 后仍尝试 remove 以暴露违反 insert/remove 配对的错误。 */
		WARN_ON(RB_EMPTY_NODE(&interval_sub->interval_tree.rb));
		interval_tree_remove(&interval_sub->interval_tree,
				     &subscriptions->itree);
	}
	/* seq 保持零表示无需等待：节点已经从稳定树删除，随后不可能再被新一轮 interval 遍历选中。 */
	spin_unlock(&subscriptions->lock);
	/* 解锁后才能执行 wait_event；最终 inv_end 同样需要该锁来提交 deferred 链。 */

	/*
	 * The possible sleep on progress in the invalidation requires the
	 * caller not hold any locks held by invalidation callbacks.
	 */
	lock_map_acquire(&__mmu_notifier_invalidate_range_start_map);
	lock_map_release(&__mmu_notifier_invalidate_range_start_map);
	if (seq)
		/* 等待最终 inv_end 改变 seq 并应用 deferred remove；调用者不得持 invalidate 回调会取得的锁。 */
		wait_event(subscriptions->wq,
			   mmu_interval_seq_released(subscriptions, seq));

	/* pairs with mmgrab in mmu_interval_notifier_insert() */
	mmdrop(mm);
	/* 现在无回调可再定位该 interval_sub，归还 insert 的 mm_count pin 并结束对象与 mm 的绑定。 */
}
EXPORT_SYMBOL_GPL(mmu_interval_notifier_remove);

/**
 * mmu_notifier_synchronize - Ensure all mmu_notifiers are freed
 *
 * This function ensures that all outstanding async SRU work from
 * mmu_notifier_put() is completed. After it returns any mmu_notifier_ops
 * associated with an unused mmu_notifier will no longer be called.
 *
 * Before using the caller must ensure that all of its mmu_notifiers have been
 * fully released via mmu_notifier_put().
 *
 * Modules using the mmu_notifier_put() API should call this in their __exit
 * function to avoid module unloading races.
 */
void mmu_notifier_synchronize(void)
{
	/* 模块卸载屏障：等待所有 mmu_notifier_put 排队的 free_notifier SRCU 回调完成。 */
	synchronize_srcu(&srcu);
}
EXPORT_SYMBOL_GPL(mmu_notifier_synchronize);
