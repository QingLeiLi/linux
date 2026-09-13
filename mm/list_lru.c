// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013 Red Hat, Inc. and Parallels Inc. All rights reserved.
 * Authors: David Chinner and Glauber Costa
 *
 * Generic LRU infrastructure
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/list_lru.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/memcontrol.h>
#include "slab.h"
#include "internal.h"

/* list_lru 将可回收对象按 NUMA 节点及可选 memcg 分桶；每个子链表的自旋锁保护链和 nr_items。 */

/* 业务背景：统一选择普通、irq 或 irqsave 的子 LRU 锁获取方式。
 * 入参：l 是目标子链；irq/irq_flags 决定中断状态保存。出参/返回：无，成功后持有 l->lock。
 * 注意事项：调用者必须以相同模式解锁；锁保护 list 和 nr_items，不能跨睡眠持有。
 */
static inline void lock_list_lru(struct list_lru_one *l, bool irq,
				 unsigned long *irq_flags)
{
	if (irq_flags)
		/* irqsave 形式保存调用者原状态，适合无法预知 IRQ 是否已关闭的外层路径。 */
		spin_lock_irqsave(&l->lock, *irq_flags);
	else if (irq)
		/* irq 形式仅关闭本 CPU 中断，配对 unlock_irq 将无条件重新打开它。 */
		spin_lock_irq(&l->lock);
	else
		spin_lock(&l->lock);
}

/* 业务背景：与 lock_list_lru 严格配对，恢复调用者进入临界区前的 IRQ 状态。
 * 入参：l 为已锁子链；irq_off/irq_flags 必须复现加锁方式。出参/返回：无。
 * 注意事项：错配会泄露中断关闭状态或错误 unlock；释放后不得再访问受该锁保护的链表。
 */
static inline void unlock_list_lru(struct list_lru_one *l, bool irq_off,
				   unsigned long *irq_flags)
{
	if (irq_flags)
		/* 仅 irqrestore 能恢复嵌套前 flags，不能退化成普通 spin_unlock。 */
		spin_unlock_irqrestore(&l->lock, *irq_flags);
	else if (irq_off)
		spin_unlock_irq(&l->lock);
	else
		spin_unlock(&l->lock);
}

#ifdef CONFIG_MEMCG
static LIST_HEAD(memcg_list_lrus);
/* 所有 memcg-aware 实例都在此表；mutex 把实例销毁与 cgroup reparent 串行化。 */
static DEFINE_MUTEX(list_lrus_mutex);

/* 业务背景：判断实例是否需要按 memcg 分桶，而非只使用每节点根链。
 * 入参：lru 为已初始化实例的借用指针。出参：其 memcg_aware 配置位。
 * 注意事项：配置在初始化后固定；读取本身不取得锁或 memcg 引用。
 */
static inline bool list_lru_memcg_aware(struct list_lru *lru)
{
	return lru->memcg_aware;
}

/* 业务背景：把 memcg-aware 实例加入全局注册表，供 cgroup offline 时批量 reparent。
 * 入参：lru 为已初始化且调用者持有的实例。出参/返回：无。
 * 注意事项：list_lrus_mutex 保护注册表；非 memcg 实例没有 reparent 协议而直接跳过。
 */
static void list_lru_register(struct list_lru *lru)
{
	if (!list_lru_memcg_aware(lru))
		/* 非 memcg 实例永不出现在全局表，避免 offline 路径访问没有 xa 的对象。 */
		return;

	mutex_lock(&list_lrus_mutex);
	list_add(&lru->list, &memcg_list_lrus);
	mutex_unlock(&list_lrus_mutex);
}

/* 业务背景：销毁实例前从全局 reparent 注册表摘除，阻止并发 cgroup 迭代发现悬空 lru。
 * 入参：lru 为仍有效、即将销毁的借用实例。出参/返回：无。
 * 注意事项：必须早于 node/xa 释放；同样由 list_lrus_mutex 串行化。
 */
static void list_lru_unregister(struct list_lru *lru)
{
	if (!list_lru_memcg_aware(lru))
		return;

	mutex_lock(&list_lrus_mutex);
	list_del(&lru->list);
	mutex_unlock(&list_lrus_mutex);
}

/* 业务背景：向 shrinker 位图查询当前 list_lru 的回收器编号。
 * 入参：lru 为借用实例。出参：已绑定 shrinker id。
 * 注意事项：仅 MEMCG 配置存在有效编号；关闭配置的桩返回 -1，调用点必须容忍。
 */
static int lru_shrinker_id(struct list_lru *lru)
{
	return lru->shrinker_id;
}

static inline struct list_lru_one *
/* 业务背景：把 memcg 私有 id 与 NUMA 节点解析为实际子 LRU。
 * 入参：lru/nid/idx 均借用；idx<0 或非 memcg 模式选择根节点链。出参：子链或 NULL。
 * 注意事项：xa_load 的对象生命周期由外层 RCU/重 parent 协议保护，返回指针本身不持久引用。
 */
list_lru_from_memcg_idx(struct list_lru *lru, int nid, int idx)
{
	if (list_lru_memcg_aware(lru) && idx >= 0) {
		/* 私有 id 对应的数组可能尚未按需分配，NULL 表示当前没有该 memcg 子链。 */
		struct list_lru_memcg *mlru = xa_load(&lru->xa, idx);

		return mlru ? &mlru->node[nid] : NULL;
	}
	return &lru->node[nid].lru;
}

static inline struct list_lru_one *
/* 业务背景：在 memcg reparent 竞态中取得一个活着的子链锁，必要时向祖先退避。
 * 入参：memcg 是可更新的输入输出借用指针；skip_empty 决定是否遇死链直接返回 NULL。
 * 出参：返回已锁子链或 NULL。注意：RCU 只保护查找，返回前锁已稳定目标链。
 */
lock_list_lru_of_memcg(struct list_lru *lru, int nid,
		       struct mem_cgroup **memcg, bool irq,
		       unsigned long *irq_flags, bool skip_empty)
{
	struct list_lru_one *l;

	rcu_read_lock();
	/* RCU 保证 xa 返回的 mlru 在找链/加锁窗口内不被 kvfree_rcu 释放。 */
again:
	l = list_lru_from_memcg_idx(lru, nid, memcg_kmem_id(*memcg));
	if (likely(l)) {
		/* 先取得候选链锁，再验证它没有被 reparent 标为 LONG_MIN。 */
		lock_list_lru(l, irq, irq_flags);
		if (likely(READ_ONCE(l->nr_items) != LONG_MIN)) {
			rcu_read_unlock();
			return l;
		}
		unlock_list_lru(l, irq, irq_flags);
	}
	/*
	 * Caller may simply bail out if raced with reparenting or
	 * may iterate through the list_lru and expect empty slots.
	 */
	if (skip_empty) {
		/* walker 可把 reparent 竞态视为本轮空链；add/del 则必须向父级继续寻找。 */
		rcu_read_unlock();
		return NULL;
	}
	VM_WARN_ON(!css_is_dying(&(*memcg)->css));
	*memcg = parent_mem_cgroup(*memcg);
	/* dying 子 cgroup 的新对象已应归父链，更新输出指针让 shrinker bit 与实际锁链一致。 */
	goto again;
}
#else
/* CONFIG_MEMCG=n：所有对象只在 node 根链，注册、xa 和 reparent 接口退化为空操作或固定返回。 */
/* 这些桩保留调用点的统一 ABI；参数有意不读取，编译器会消去对应 memcg 开销。 */
static void list_lru_register(struct list_lru *lru)
{
	/* CONFIG_MEMCG=n：没有全局实例表可维护，调用保留为无副作用。 */
}

static void list_lru_unregister(struct list_lru *lru)
{
	/* 对应注册桩；不访问 lru，因而可安全用于普通 node-only 实例销毁。 */
}

static int lru_shrinker_id(struct list_lru *lru)
{
	/* -1 使任何意外的 set_shrinker_bit 调用保持可识别的“无 memcg shrinker”状态。 */
	return -1;
}

static inline bool list_lru_memcg_aware(struct list_lru *lru)
{
	/* 编译期关闭配置时固定 false，所有对象经由根 node 链。 */
	return false;
}

static inline struct list_lru_one *
list_lru_from_memcg_idx(struct list_lru *lru, int nid, int idx)
{
	/* idx/memcg 在该配置下无语义，选择固定 node 根子链。 */
	return &lru->node[nid].lru;
}

static inline struct list_lru_one *
lock_list_lru_of_memcg(struct list_lru *lru, int nid,
		       struct mem_cgroup **memcg, bool irq,
		       unsigned long *irq_flags, bool skip_empty)
{
	struct list_lru_one *l = &lru->node[nid].lru;
	/* 无 reparent/XArray 竞态，直接按调用者指定 IRQ 模式获取根链锁。 */

	lock_list_lru(l, irq, irq_flags);

	return l;
}
#endif /* CONFIG_MEMCG */

/* 业务背景：普通进程上下文锁定指定 memcg/node 子链的公开包装。
 * 入参：lru/nid 借用，memcg 为可被 reparent 改写的输入输出指针。出参：已锁子链。
 * 注意事项：返回后调用者必须 list_lru_unlock，且不能假定原 memcg 仍是最终归属。
 */
struct list_lru_one *list_lru_lock(struct list_lru *lru, int nid,
				   struct mem_cgroup **memcg)
{
	return lock_list_lru_of_memcg(lru, nid, memcg, /*irq=*/false,
	/* wrapper 只固定锁模式，真正的 memcg 选择/退避仍集中在内部 helper，避免协议分叉。 */
				      /*irq_flags=*/NULL, /*skip_empty=*/false);
}

/* 业务背景：结束普通 list_lru_lock 临界区。
 * 入参：l 为该锁调用返回的已锁子链。出参/返回：无。
 * 注意事项：只匹配未关闭 IRQ 的锁模式；解锁后 item/计数可立即被并发 reclaim 改写。
 */
void list_lru_unlock(struct list_lru_one *l)
{
	unlock_list_lru(l, /*irq_off=*/false, /*irq_flags=*/NULL);
}

/* 业务背景：在可能与本 CPU 中断处理竞争的路径锁定子链。
 * 入参：lru/nid/memcg 与普通接口语义相同。出参：已锁且本 CPU IRQ 关闭的子链。
 * 注意事项：必须 list_lru_unlock_irq 配对，临界区不可睡眠。
 */
struct list_lru_one *list_lru_lock_irq(struct list_lru *lru, int nid,
				       struct mem_cgroup **memcg)
{
	return lock_list_lru_of_memcg(lru, nid, memcg, /*irq=*/true,
				      /*irq_flags=*/NULL, /*skip_empty=*/false);
}

/* 业务背景：恢复 list_lru_lock_irq 关闭的本 CPU IRQ 并释放子链锁。
 * 入参：l 为 irq 模式已锁子链。出参/返回：无。
 * 注意事项：不可与 irqsave/普通接口混配。
 */
void list_lru_unlock_irq(struct list_lru_one *l)
{
	unlock_list_lru(l, /*irq_off=*/true, /*irq_flags=*/NULL);
}

/* 业务背景：保存调用者 IRQ flags 后锁定子链，供需要嵌套/恢复原状态的回收路径使用。
 * 入参：flags 是非空输出槽，其他参数同普通锁。出参：返回已锁子链并写 flags。
 * 注意事项：flags 必须原样交给 irqrestore，锁区不能睡眠。
 */
struct list_lru_one *list_lru_lock_irqsave(struct list_lru *lru, int nid,
					   struct mem_cgroup **memcg,
					   unsigned long *flags)
{
	return lock_list_lru_of_memcg(lru, nid, memcg, /*irq=*/true,
				      /*irq_flags=*/flags, /*skip_empty=*/false);
}

/* 业务背景：完成 irqsave 版本临界区并恢复进入前 IRQ 状态。
 * 入参：l 和 flags 必须来自同次 list_lru_lock_irqsave。出参/返回：无。
 * 注意事项：flags 是调用者栈状态，不能跨异步边界保存。
 */
void list_lru_unlock_irqrestore(struct list_lru_one *l, unsigned long *flags)
{
	unlock_list_lru(l, /*irq_off=*/true, /*irq_flags=*/flags);
}

/* 业务背景：在调用者已锁定正确子链后把对象首次挂入可 shrink 的 LRU。
 * 入参：lru/l/item 借用；nid/memcg 描述最终锁定链的归属。出参：首次插入 true，否则 false。
 * 注意事项：l->lock 必须持有；首次非空转换才置 shrinker bit，节点总计数与子链计数必须同步。
 */
bool __list_lru_add(struct list_lru *lru, struct list_lru_one *l,
		    struct list_head *item, int nid,
		    struct mem_cgroup *memcg)
{
	if (list_empty(item)) {
		/* item 的空链状态是“尚未被本或其他 LRU 挂入”的唯一插入许可，避免双重链入。 */
		list_add_tail(item, &l->list);
		/*
		 * Set shrinker bit on the memcg that owns the locked
		 * sublist - lock_list_lru_of_memcg() may have walked up
		 * past a dying memcg, and the bit must be set there.
		 */
		if (!l->nr_items++)
			/* 0→1 转换才通知 shrinker；位属于实际锁住的 memcg，可能已从 dying 子级退避。 */
			set_shrinker_bit(memcg, nid, lru_shrinker_id(lru));
		atomic_long_inc(&lru->node[nid].nr_items);
		/* 节点汇总在各子链锁下更新，读者用原子快照做预算而不需锁所有 memcg 链。 */
		return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(list_lru_add);

/* 业务背景：在持锁前提下从子 LRU 摘除对象，和 add 形成精确计数配对。
 * 入参：lru/l/item/nid 均借用。出参：确实摘除 true，原本未入链 false。
 * 注意事项：l->lock 必须持有；只减少节点总数，不在此路径清 shrinker bit。
 */
bool __list_lru_del(struct list_lru *lru, struct list_lru_one *l,
		    struct list_head *item, int nid)
{
	if (!list_empty(item)) {
		/* list_del_init 同时摘除并恢复空链哨兵，使同一对象之后可安全重新加入。 */
		list_del_init(item);
		l->nr_items--;
		atomic_long_dec(&lru->node[nid].nr_items);
		return true;
	}
	return false;
}

/* The caller must ensure the memcg lifetime. */
/* 业务背景：普通上下文把对象加入指定 node/memcg 的可回收链。
 * 入参：item 是嵌入对象且 memcg 生命周期由调用者保证；其余为借用定位信息。出参：是否首次入链。
 * 注意事项：内部锁可能因 dying memcg 改写 memcg 到父级，调用者不得据原值更新额外账本。
 */
bool list_lru_add(struct list_lru *lru, struct list_head *item, int nid,
		  struct mem_cgroup *memcg)
{
	struct list_lru_one *l;
	bool ret;

	l = list_lru_lock(lru, nid, &memcg);
	/* 获取后使用可能被修正的 memcg；这保证 __add 的 shrinker bit 与实际子链一致。 */
	ret = __list_lru_add(lru, l, item, nid, memcg);
	list_lru_unlock(l);
	return ret;
}

/* 业务背景：IRQ 竞争环境下的 list_lru_add 版本。
 * 入参/返回与普通 add 相同。出参：首次入链 true。
 * 注意事项：使用 irq 锁模式，不能在调用链上引入可睡眠操作。
 */
bool list_lru_add_irq(struct list_lru *lru, struct list_head *item,
		      int nid, struct mem_cgroup *memcg)
{
	struct list_lru_one *l;
	bool ret;

	l = list_lru_lock_irq(lru, nid, &memcg);
	ret = __list_lru_add(lru, l, item, nid, memcg);
	list_lru_unlock_irq(l);
	return ret;
}

/* 业务背景：由对象虚拟地址自动推导 node/memcg 的便捷入链接口。
 * 入参：item 是对象内嵌链节点，必须可由 virt_to_page 反查。出参：是否首次入链。
 * 注意事项：memcg-aware 分支在 RCU 下借用归属；对象释放者仍须保证 item 本身存活。
 */
bool list_lru_add_obj(struct list_lru *lru, struct list_head *item)
{
	bool ret;
	/* 随后的 RCU 分支只决定归属；__list_lru_del 在获取子链锁后才原子地判断 item 是否仍在链上。 */
	int nid = page_to_nid(virt_to_page(item));

	if (list_lru_memcg_aware(lru)) {
		/* 删除的 RCU 区间仅包住 mem_cgroup_from_virt 到内部同步删除，不能把 item 生命周期交给 RCU。 */
		/* 对象地址到 memcg 的映射受 RCU 保护；无需取得长引用，因为 add 不会睡眠。 */
		rcu_read_lock();
		ret = list_lru_add(lru, item, nid, mem_cgroup_from_virt(item));
		rcu_read_unlock();
	} else {
		ret = list_lru_add(lru, item, nid, NULL);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(list_lru_add_obj);

/* The caller must ensure the memcg lifetime. */
/* 业务背景：按调用者给定 node/memcg 删除对象，和 add 的锁/退避协议对称。
 * 入参：item 与 memcg 生命周期由调用者保持。出参：实际删除 true。
 * 注意事项：死 memcg 仍可能向父链退避，不能绕开 lock_list_lru_of_memcg 直接操作 xa。
 */
bool list_lru_del(struct list_lru *lru, struct list_head *item, int nid,
		  struct mem_cgroup *memcg)
{
	struct list_lru_one *l;
	bool ret;

	l = list_lru_lock(lru, nid, &memcg);
	/* 删除同样走 reparent 安全锁入口，避免 offline 窗口直接操作已迁移源链。 */
	ret = __list_lru_del(lru, l, item, nid);
	list_lru_unlock(l);
	return ret;
}

/* 业务背景：按对象地址自动定位 node/memcg 后删除 LRU 节点。
 * 入参：item 是仍有效的嵌入节点。出参：摘除成功 true。
 * 注意事项：RCU 只保护 memcg 查找窗口；调用者保证对象不在查找后被释放。
 */
bool list_lru_del_obj(struct list_lru *lru, struct list_head *item)
{
	/* nid 从节点所在页获得，故对象必须是可直接映射的内核内存，不能传独立的临时链节点。 */
	bool ret;
	int nid = page_to_nid(virt_to_page(item));

	if (list_lru_memcg_aware(lru)) {
		rcu_read_lock();
		ret = list_lru_del(lru, item, nid, mem_cgroup_from_virt(item));
		rcu_read_unlock();
	} else {
		ret = list_lru_del(lru, item, nid, NULL);
	}

	/* 两个配置分支都已完成同步删除；返回值只反映 item 是否真的曾被挂入目标 LRU。 */
	return ret;
}
EXPORT_SYMBOL_GPL(list_lru_del_obj);

/* 删除对象接口之后，下面两个 isolate helper 假定 walker 已持子链锁，不能独立用于无锁摘链。 */

/* 业务背景：walker 回调已决定回收时，从当前已锁子链摘取对象交给调用者后续处理。
 * 入参：list 已锁，item 是该链成员。出参/返回：无；子链数量减一。
 * 注意事项：不更新 node 总计数，walker 根据 LRU_REMOVED 统一完成该配对。
 */
void list_lru_isolate(struct list_lru_one *list, struct list_head *item)
{
	list_del_init(item);
	/* isolate 不释放对象；它仅把“链表 ownership”交给 walker 回调后的私有处理路径。 */
	list->nr_items--;
}
EXPORT_SYMBOL_GPL(list_lru_isolate);

/* 业务背景：从已锁 LRU 摘除对象并转移到调用者私有工作链。
 * 入参：list 已锁，item 为成员，head 为目标链。出参/返回：无。
 * 注意事项：和 isolate 一样，node 总计数由 walk 状态机而非本 helper 维护。
 */
void list_lru_isolate_move(struct list_lru_one *list, struct list_head *item,
			   struct list_head *head)
{
	list_move(item, head);
	/* move 保留 item 的 list 节点但转移所属链，典型用途是把待释放对象汇集到临时链。 */
	list->nr_items--;
}
EXPORT_SYMBOL_GPL(list_lru_isolate_move);

/* 业务背景：读取一个 memcg/node 子链的近似对象数，供 shrink 决策估算。
 * 入参：lru/nid/memcg 为借用定位。出参：非负数量，缺失/死链视为 0。
 * 注意事项：RCU 保护 xa 查找，READ_ONCE 只给快照；LONG_MIN 是 reparent 死链哨兵。
 */
unsigned long list_lru_count_one(struct list_lru *lru,
				 int nid, struct mem_cgroup *memcg)
{
	struct list_lru_one *l;
	long count;

	rcu_read_lock();
	/* count 允许无锁近似，RCU 只保证 mlru 指针有效，不保证 nr_items 随后不变。 */
	l = list_lru_from_memcg_idx(lru, nid, memcg_kmem_id(memcg));
	count = l ? READ_ONCE(l->nr_items) : 0;
	rcu_read_unlock();

	if (unlikely(count < 0))
		count = 0;

	return count;
}
EXPORT_SYMBOL_GPL(list_lru_count_one);

/* 业务背景：读取 node 上所有根/memcg 子链汇总的原子对象数。
 * 入参：lru/nid 为借用定位。出参：原子快照。
 * 注意事项：该数不等于任一链可锁定快照，只适合统计和 shrink 预算。
 */
unsigned long list_lru_count_node(struct list_lru *lru, int nid)
{
	struct list_lru_node *nlru;

	nlru = &lru->node[nid];
	return atomic_long_read(&nlru->nr_items);
	/* 该总数包括根链和所有 memcg 子链的已提交 add/del，walker 的 isolated 分支同步递减。 */
}
EXPORT_SYMBOL_GPL(list_lru_count_node);

/* 业务背景：在一个 node/memcg 子链上执行 shrinker 指定的隔离回调并维护预算/计数。
 * 入参：isolate/cb_arg 定义对象处置；nr_to_walk 是输入输出预算；irq_off 选择锁模式。
 * 出参：返回隔离数。注意：回调可释放锁，LRU_RETRY 类状态要求从头重新锁定遍历。
 */
static unsigned long
__list_lru_walk_one(struct list_lru *lru, int nid, struct mem_cgroup *memcg,
		    list_lru_walk_cb isolate, void *cb_arg,
		    unsigned long *nr_to_walk, bool irq_off)
{
	struct list_lru_node *nlru = &lru->node[nid];
	struct list_lru_one *l = NULL;
	struct list_head *item, *n;
	unsigned long isolated = 0;

restart:
	/* isolate 可主动丢锁，任何此类返回都必须重新查 memcg 子链并从头走，不能沿用迭代指针。 */
	l = lock_list_lru_of_memcg(lru, nid, &memcg, /*irq=*/irq_off,
				   /*irq_flags=*/NULL, /*skip_empty=*/true);
	if (!l)
		/* reparent 后 skip_empty 返回 NULL 是正常竞争结果，本轮没有可安全扫描的子链。 */
		return isolated;
	list_for_each_safe(item, n, &l->list) {
		/* safe 迭代允许回调删除/移动当前 item；但回调丢锁后仍只能 restart。 */
		enum lru_status ret;

		/*
		 * decrement nr_to_walk first so that we don't livelock if we
		 * get stuck on large numbers of LRU_RETRY items
		 */
		if (!*nr_to_walk)
			/* 预算先检查再调用回调，避免大批 LRU_RETRY 绕过 shrinker 给定的工作上限。 */
			break;
		--*nr_to_walk;

		ret = isolate(item, l, cb_arg);
		/* 回调返回值同时编码 item 去向与锁是否仍持有，下面每个 case 维持对应不变量。 */
		switch (ret) {
		/*
		 * LRU_RETRY, LRU_REMOVED_RETRY and LRU_STOP will drop the lru
		 * lock. List traversal will have to restart from scratch.
		 */
		case LRU_RETRY:
			goto restart;
		case LRU_REMOVED_RETRY:
			fallthrough;
		case LRU_REMOVED:
			/* item 已由回调/isolate 摘除，汇总 node 计数在此与子链减计数配对。 */
			isolated++;
			atomic_long_dec(&nlru->nr_items);
			if (ret == LRU_REMOVED_RETRY)
				goto restart;
			break;
		case LRU_ROTATE:
			/* 未回收但重新排到队尾，避免同一难处理对象在本次扫描中饥饿其它成员。 */
			list_move_tail(item, &l->list);
			break;
		case LRU_SKIP:
			/* 保持当前位置和锁，继续扫描下一个成员；适用于本轮不宜处理但无需重排的对象。 */
			break;
		case LRU_STOP:
			goto out;
		default:
			BUG();
		}
	}
	unlock_list_lru(l, irq_off, NULL);
out:
	/* 正常耗尽、STOP 或空链都从唯一出口返回，只有仍持锁的正常循环末尾显式 unlock。 */
	return isolated;
}

unsigned long
/* 业务背景：普通锁模式下回收一个指定 memcg/node 子链。
 * 入参：lru/nid/memcg 与 isolate 回调借用，nr_to_walk 输入输出。出参：隔离数。
 * 注意事项：回调遵守 lru_status 的锁协议；返回后预算可能已耗尽。
 */
list_lru_walk_one(struct list_lru *lru, int nid, struct mem_cgroup *memcg,
		  list_lru_walk_cb isolate, void *cb_arg,
		  unsigned long *nr_to_walk)
{
	return __list_lru_walk_one(lru, nid, memcg, isolate,
	/* 公开 wrapper 不增加状态，所有 restart/锁释放语义集中在核心 walker。 */
				   cb_arg, nr_to_walk, false);
}
EXPORT_SYMBOL_GPL(list_lru_walk_one);

unsigned long
/* 业务背景：IRQ 锁模式的单子链回收入口。
 * 入参/返回与普通 walk 相同。出参：隔离数。
 * 注意事项：isolate 若返回会释放锁的状态，必须遵守 IRQ 上下文恢复约束。
 */
list_lru_walk_one_irq(struct list_lru *lru, int nid, struct mem_cgroup *memcg,
		      list_lru_walk_cb isolate, void *cb_arg,
		      unsigned long *nr_to_walk)
{
	return __list_lru_walk_one(lru, nid, memcg, isolate,
				   cb_arg, nr_to_walk, true);
}

/* 业务背景：回收一个 node 的根链及全部存活 memcg 子链，直到预算耗尽。
 * 入参：lru/nid/isolate/cb_arg 借用，nr_to_walk 为共享输入输出预算。出参：总隔离数。
 * 注意事项：遍历 xa 时 tryget memcg 防 offline 释放；每个引用必须在子链 walk 后 put。
 */
unsigned long list_lru_walk_node(struct list_lru *lru, int nid,
				 list_lru_walk_cb isolate, void *cb_arg,
				 unsigned long *nr_to_walk)
{
	long isolated = 0;

	isolated += list_lru_walk_one(lru, nid, NULL, isolate, cb_arg,
	/* 根链总是先扫描；剩余预算才分配给各 memcg 子链，维持全 node 的统一上限。 */
				      nr_to_walk);

#ifdef CONFIG_MEMCG
	if (*nr_to_walk > 0 && list_lru_memcg_aware(lru)) {
		/* 根链未耗尽预算才读取 xa，避免无需 memcg 引用的额外遍历。 */
		struct list_lru_memcg *mlru;
		struct mem_cgroup *memcg;
		unsigned long index;

		xa_for_each(&lru->xa, index, mlru) {
			/* XArray 可并发发生 reparent erase，必须先把 private id 转回 memcg 并 tryget。 */
			rcu_read_lock();
			memcg = mem_cgroup_from_private_id(index);
			/* private id 反查可能得到正在 offline 的 css，随后的 tryget 是把快照变为可用引用的唯一关口。 */
			if (!mem_cgroup_tryget(memcg)) {
				/* tryget 失败时不可解引用该 css，也不可根据它的 id 再查 list_lru 子链。 */
				/* offline 竞争者已使 css 不可引用，跳过而不是使用可能释放的子链数组。 */
				rcu_read_unlock();
				continue;
			}
			/* 成功 tryget 后即使 RCU 结束，memcg 仍保持到本次子链 walk 与 put 完成。 */
			rcu_read_unlock();
			isolated += __list_lru_walk_one(lru, nid, memcg,
			/* 子链 walk 结束后立即 put，回调不可把该借用 memcg 保存到异步路径。 */
							isolate, cb_arg,
							nr_to_walk, false);
			mem_cgroup_put(memcg);
			/* put 后不得使用 memcg；只根据共享预算决定是否继续枚举下一条 xa 项。 */

			if (*nr_to_walk <= 0)
				break;
		}
	}
#endif

	return isolated;
}
EXPORT_SYMBOL_GPL(list_lru_walk_node);

/* 业务背景：初始化一个 node 或 memcg-node 子链的空链、锁和计数。
 * 入参：lru 提供可选 lockdep class，l 是待初始化存储。出参/返回：无。
 * 注意事项：必须早于任何发布/入链；CONFIG_LOCKDEP 下同实例锁使用共同 class。
 */
static void init_one_lru(struct list_lru *lru, struct list_lru_one *l)
{
	INIT_LIST_HEAD(&l->list);
	/* 空链、锁与零计数共同建立“可首次 add”的初始不变量。 */
	spin_lock_init(&l->lock);
	l->nr_items = 0;
#ifdef CONFIG_LOCKDEP
	if (lru->key)
		/* 调试锁类按 list_lru 实例分组，帮助 lockdep 区分不同缓存/子系统的锁顺序。 */
		lockdep_set_class(&l->lock, lru->key);
#endif
}

#ifdef CONFIG_MEMCG
/* 业务背景：为一个 memcg 分配覆盖所有 NUMA 节点的子链数组。
 * 入参：lru 提供锁类，gfp 控制分配；出参：新数组或 NULL，所有权交给 xa 发布者。
 * 注意事项：每个 node 必须独立 init，未发布失败对象由当前调用者释放。
 */
static struct list_lru_memcg *memcg_init_list_lru_one(struct list_lru *lru, gfp_t gfp)
{
	int nid;
	struct list_lru_memcg *mlru;

	mlru = kmalloc_flex(*mlru, node, nr_node_ids, gfp);
	/* 柔性 node 数组一次分配，保证一个 memcg 的所有 node 子链可作为整体 RCU 释放。 */
	if (!mlru)
		return NULL;

	for_each_node(nid)
		/* 每项独立锁，跨 NUMA node shrink 不需要持一把全局 LRU 锁。 */
		init_one_lru(lru, &mlru->node[nid]);

	return mlru;
}

/* 业务背景：初始化实例的 memcg XArray 和固定模式位。
 * 入参：lru 为尚未发布实例，memcg_aware 是初始化策略。出参/返回：无。
 * 注意事项：XA_FLAGS_LOCK_IRQ 与 reparent/分配路径的 irq 锁配对，初始化后不可随意切换模式。
 */
static inline void memcg_init_list_lru(struct list_lru *lru, bool memcg_aware)
{
	if (memcg_aware)
		/* XA_FLAGS_LOCK_IRQ 使 xas 更新与 reparent 的 IRQ 锁路径有相同中断约束。 */
		xa_init_flags(&lru->xa, XA_FLAGS_LOCK_IRQ);
	lru->memcg_aware = memcg_aware;
}

/* 业务背景：销毁 list_lru 时清空所有 memcg 子链存储。
 * 入参：lru 为已从全局注册表摘除的实例。出参/返回：无。
 * 注意事项：xas 锁保护 erase 与 kfree；调用者须保证没有并发 walker/分配者再访问实例。
 */
static void memcg_destroy_list_lru(struct list_lru *lru)
{
	XA_STATE(xas, &lru->xa, 0);
	struct list_lru_memcg *mlru;

	if (!list_lru_memcg_aware(lru))
		/* 无 xa 的实例不能执行 xas 操作，销毁只由外层释放 node 根链。 */
		return;

	xas_lock_irq(&xas);
	/* 锁住 xa 后逐条先取存储再清指针，确保后续不再有新读者从实例拿到该 mlru。 */
	xas_for_each(&xas, mlru, ULONG_MAX) {
		kfree(mlru);
		xas_store(&xas, NULL);
	}
	xas_unlock_irq(&xas);
}

/* 业务背景：cgroup offline 时把一个子 memcg/node 链迁移到父 memcg，并永久标记源链死亡。
 * 入参：src 已属 dying memcg，dst_memcg 是仍存活父级。出参/返回：无。
 * 注意事项：先锁 src 再嵌套锁 dst，先搬链并置 LONG_MIN 后才能删 xa，防 del 落在错误链。
 */
static void memcg_reparent_list_lru_one(struct list_lru *lru, int nid,
					struct list_lru_one *src,
					struct mem_cgroup *dst_memcg)
{
	int dst_idx = dst_memcg->kmemcg_id;
	struct list_lru_one *dst;

	spin_lock_irq(&src->lock);
	/* 先锁源链冻结其成员，随后嵌套锁父链；固定顺序防相邻 reparent 相互死锁。 */
	dst = list_lru_from_memcg_idx(lru, nid, dst_idx);
	spin_lock_nested(&dst->lock, SINGLE_DEPTH_NESTING);

	list_splice_init(&src->list, &dst->list);
	/* splice 后所有对象可由 parent shrinker 看到，src 只保留死亡哨兵而不再拥有成员。 */
	if (src->nr_items) {
		WARN_ON(src->nr_items < 0);
		dst->nr_items += src->nr_items;
		set_shrinker_bit(dst_memcg, nid, lru_shrinker_id(lru));
	}
	/* Mark the list_lru_one dead */
	src->nr_items = LONG_MIN;
	/* LONG_MIN 让并发 lookup 在拿锁后识别死链并向 parent 退避，不能用普通零值。 */

	spin_unlock(&dst->lock);
	spin_unlock_irq(&src->lock);
}

/* 业务背景：memcg offline 的全局回调，遍历已注册 list_lru 并把该 cgroup 对象转交 parent。
 * 入参：memcg 为 dying 源，parent 为接收者，均由 cgroup 生命周期借用。出参/返回：无。
 * 注意事项：list_lrus_mutex 固定实例表；xa erase 必须晚于全部 node reparent，释放用 RCU 延迟。
 */
void memcg_reparent_list_lrus(struct mem_cgroup *memcg, struct mem_cgroup *parent)
{
	struct list_lru *lru;
	int i;

	mutex_lock(&list_lrus_mutex);
	/* 销毁/注册与 offline 回调共享 mutex，遍历期间实例表和每个 lru 的 xa 指针稳定。 */
	list_for_each_entry(lru, &memcg_list_lrus, list) {
		struct list_lru_memcg *mlru;

		/*
		 * css_is_dying() check in memcg_list_lru_alloc() avoids
		 * allocating a new mlru since CSS_DYING is already set for this
		 * memcg a rcu grace period ago.
		 */
		mlru = xa_load(&lru->xa, memcg->kmemcg_id);
		/* CSS_DYING 已提前发布，因此不会为离线 cgroup 新建 mlru；不存在则该实例无需迁移。 */
		if (!mlru)
			continue;

		/*
		 * Reparent each per-node list and mark the child dead
		 * (LONG_MIN) before clearing xarray entry otherwise a
		 * concurrent list_lru_del() may corrupt the list if it arrives
		 * after xarray clear but before reparenting as
		 * lock_list_lru_of_memcg will acquire parent's lock while the
		 * item is still on child's list.
		 */
		for_each_node(i)
			/* 逐 node 完成搬迁和 LONG_MIN 标记后才允许删除 xa 入口。 */
			memcg_reparent_list_lru_one(lru, i, &mlru->node[i], parent);

		xa_erase_irq(&lru->xa, memcg->kmemcg_id);
		/* 此后新 lookup 只能向 parent 退避，旧 RCU 读者仍由 kvfree_rcu 延长内存寿命。 */

		/*
		 * Here all list_lrus corresponding to the cgroup are guaranteed
		 * to remain empty, we can safely free this lru, any further
		 * memcg_list_lru_alloc() call will simply bail out.
		 */
		kvfree_rcu(mlru, rcu);
	}
	mutex_unlock(&list_lrus_mutex);
}

/* 业务背景：快速判断一个 memcg 是否已有 list_lru 子链数组。
 * 入参：memcg/lru 为借用。出参：根/无私有 id 或 xa 有条目时 true。
 * 注意事项：只是查找快照，发布与 dying 竞态由调用者的 xa/RCU 协议处理。
 */
static inline bool memcg_list_lru_allocated(struct mem_cgroup *memcg,
					    struct list_lru *lru)
{
	int idx = memcg->kmemcg_id;

	return idx < 0 || xa_load(&lru->xa, idx);
}

/* 业务背景：为 memcg 及尚缺失的祖先逐级构造并发布 list_lru 子链，保证将来 reparent 有落点。
 * 入参：memcg/lru 借用，gfp 为分配约束。出参：0 或 -ENOMEM/XArray 错误。
 * 注意事项：xas 锁内拒绝 dying css；未赢得发布的 mlru 仍归当前函数释放，分配可睡眠。
 */
static int __memcg_list_lru_alloc(struct mem_cgroup *memcg,
				  struct list_lru *lru, gfp_t gfp)
{
	unsigned long flags;
	struct list_lru_memcg *mlru = NULL;
	struct mem_cgroup *pos, *parent;
	XA_STATE(xas, &lru->xa, 0);

	gfp &= GFP_RECLAIM_MASK;
	/* 移除非 reclaim 位，避免在 cgroup 元数据分配中携带调用者不适用的高层分配语义。 */
	/*
	 * Because the list_lru can be reparented to the parent cgroup's
	 * list_lru, we should make sure that this cgroup and all its
	 * ancestors have allocated list_lru_memcg.
	 */
	do {
		/* 每轮找到从最近缺失祖先到目标的一个位置；成功发布后再继续向下补齐链路。 */
		/*
		 * Keep finding the farest parent that wasn't populated
		 * until found memcg itself.
		 */
		pos = memcg;
		parent = parent_mem_cgroup(pos);
		while (!memcg_list_lru_allocated(parent, lru)) {
			/* 向上寻找最远未分配祖先，先建父级确保 child reparent 总有已存在接收链。 */
			pos = parent;
			parent = parent_mem_cgroup(pos);
		}

		if (!mlru) {
			mlru = memcg_init_list_lru_one(lru, gfp);
			if (!mlru)
				return -ENOMEM;
		}
		xas_set(&xas, pos->kmemcg_id);
		/* xas 指向本轮待发布 id；锁内再次查空和 dying，避免重复分配者/离线者覆盖现有数组。 */
		do {
			xas_lock_irqsave(&xas, flags);
			if (!xas_load(&xas) && !css_is_dying(&pos->css)) {
				xas_store(&xas, mlru);
				if (!xas_error(&xas))
					mlru = NULL;
			}
			xas_unlock_irqrestore(&xas, flags);
		} while (xas_nomem(&xas, gfp));
		/* xas_nomem 可能在锁外分配节点并重试，mlru 直到 store 成功前始终只归当前函数。 */
	} while (pos != memcg && !css_is_dying(&pos->css));

	if (unlikely(mlru))
		/* 未被 xas 接管的候选可能因竞争或 dying 被拒绝，仍由本函数释放。 */
		kfree(mlru);

	return xas_error(&xas);
}

/* 业务背景：公开按 memcg 预建 list_lru 头，常用于对象开始进入该 cgroup 前。
 * 入参：memcg/lru 借用，gfp 控制可能睡眠的分配。出参：已有/非 memcg 模式为 0，否则分配结果。
 * 注意事项：不持有 memcg 长引用；调用者必须保证该对象在调用期间有效。
 */
int memcg_list_lru_alloc(struct mem_cgroup *memcg, struct list_lru *lru,
			 gfp_t gfp)
{
	if (!list_lru_memcg_aware(lru) || memcg_list_lru_allocated(memcg, lru))
		/* 已有头或非 memcg 实例无需分配，快速成功不持锁也不睡眠。 */
		return 0;
	return __memcg_list_lru_alloc(memcg, lru, gfp);
}

/* 业务背景：从 folio 的当前 charge 获取 memcg，并在缺失时持引用跨可睡眠子链分配。
 * 入参：folio/lru 借用，gfp 为分配掩码。出参：0 或分配错误。
 * 注意事项：RCU 快路径不延长 memcg；慢路径 get_mem_cgroup_from_folio 与 put 必须严格配对。
 */
int folio_memcg_list_lru_alloc(struct folio *folio, struct list_lru *lru,
			       gfp_t gfp)
{
	struct mem_cgroup *memcg;
	int res;

	if (!list_lru_memcg_aware(lru))
		return 0;

	/* Fast path when list_lru heads already exist */
	rcu_read_lock();
	/* 快路径只读 folio 当前 charge；若缺头才转为持引用慢路径，避免在 RCU 内分配。 */
	memcg = folio_memcg(folio);
	res = memcg_list_lru_allocated(memcg, lru);
	rcu_read_unlock();
	if (likely(res))
		return 0;

	/* Allocation may block, pin the memcg */
	memcg = get_mem_cgroup_from_folio(folio);
	/* 慢路径引用跨 __memcg_list_lru_alloc 的可能睡眠，folio 的 charge 不能仅靠短 RCU 借用。 */
	res = __memcg_list_lru_alloc(memcg, lru, gfp);
	mem_cgroup_put(memcg);
	return res;
}
#else
/* MEMCG 关闭时 init/destroy 子步骤为空，外层仍初始化/释放 node 根链。 */
static inline void memcg_init_list_lru(struct list_lru *lru, bool memcg_aware)
{
}

static void memcg_destroy_list_lru(struct list_lru *lru)
{
}
#endif /* CONFIG_MEMCG */

/* 业务背景：创建 list_lru 的 node 根链、可选 memcg XArray 及 shrinker 关联，然后发布到 reparent 表。
 * 入参：lru 为调用者存储，memcg_aware 为策略，shrinker 可空借用。出参：0 或 -ENOMEM。
 * 注意事项：node 分配失败前实例未发布；MEMCG 被禁用时强制降级，destroy 必须与成功 init 配对。
 */
int __list_lru_init(struct list_lru *lru, bool memcg_aware, struct shrinker *shrinker)
{
	int i;

#ifdef CONFIG_MEMCG
	/* shrinker id 仅供 memcg bit 标记；没有 shrinker 的实例用 -1 明确表示不可设置 bit。 */
	if (shrinker)
		lru->shrinker_id = shrinker->id;
	else
		lru->shrinker_id = -1;

	if (mem_cgroup_kmem_disabled())
		memcg_aware = false;
#endif

	lru->node = kzalloc_objs(*lru->node, nr_node_ids);
	/* 根链数组成功分配前没有任何全局可见状态；失败可直接返回而无需回滚注册表。 */
	if (!lru->node)
		return -ENOMEM;

	for_each_node(i)
		init_one_lru(lru, &lru->node[i].lru);

	memcg_init_list_lru(lru, memcg_aware);
	/* 完成所有 node/XArray 初始化后才注册，offline 回调由此开始可以安全遍历该实例。 */
	list_lru_register(lru);

	return 0;
}
EXPORT_SYMBOL_GPL(__list_lru_init);

/* 业务背景：撤销一个 list_lru 实例，先阻断 reparent，再释放 memcg 子链和 node 根链。
 * 入参：lru 为调用者拥有实例。出参/返回：无；重复销毁/未初始化为无副作用。
 * 注意事项：所有对象必须已摘除，销毁不能与 walker、add/del 或 memcg offline 并发。
 */
void list_lru_destroy(struct list_lru *lru)
{
	/* Already destroyed or not yet initialized? */
	if (!lru->node)
		/* NULL 表示未初始化或已销毁，避免重复 free/重复 unregister。 */
		return;

	list_lru_unregister(lru);
	/* 先摘全局表，再释放 xa/node，杜绝 cgroup offline 获得即将失效的实例。 */

	memcg_destroy_list_lru(lru);
	kfree(lru->node);
	lru->node = NULL;

#ifdef CONFIG_MEMCG
	lru->shrinker_id = -1;
#endif
}
EXPORT_SYMBOL_GPL(list_lru_destroy);
