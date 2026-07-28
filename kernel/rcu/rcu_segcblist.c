// SPDX-License-Identifier: GPL-2.0+
/*
 * 中文学习注释生成模型：OpenAI GPT-5 Codex（2026-07-28）。
 *
 * 【文件职责】
 * 本文件实现 RCU 回调的两级容器：
 *
 *   rcu_cblist       普通单向链表，用于临时搬运一批回调；
 *   rcu_segcblist    按宽限期进度把同一条链划分为四个逻辑段。
 *
 * 四段从旧到新依次是：
 *
 *   DONE        已越过所需宽限期，可以执行；
 *   WAIT        等待最早一个已知宽限期；
 *   NEXT_READY  等待稍后的已知宽限期；
 *   NEXT        新入队、尚未可靠绑定宽限期。
 *
 * 物理上只有一条 rcu_head 链。tails[] 保存各段“尾部 next 指针的地址”，
 * 相邻 tails 相等即表示中间段为空；gp_seq[] 记录对应等待段的目标宽限期，
 * seglen[] 保存各段数量，len 则是供 rcu_barrier() 等无锁观察者使用的总数。
 *
 * 【主调用链】
 * call_rcu()/call_srcu()/Tasks RCU 入队
 *   → rcu_segcblist_enqueue()
 *   → rcu_segcblist_accelerate() 把新回调绑定到宽限期
 *   → rcu_segcblist_advance() 把已完成回调推进到 DONE
 *   → extract_done_cbs() 临时摘出并执行
 *   → 必要时 insert_done_cbs() 放回未执行完的回调。
 *
 * 【并发边界】
 * 链表拓扑通常由调用方的 per-CPU/节点锁或禁抢占协议串行化，本文件不自行
 * 获取这些锁。len 是例外：rcu_barrier() 会无锁采样它，因此 0↔1 转换必须
 * 用全屏障与链表发布、回调执行完成建立顺序。READ_ONCE/WRITE_ONCE 只防止
 * 编译器合并和撕裂，不代替调用方的互斥。
 *
 * 【设计权衡】
 * 分段链表避免给每个回调单独保存、排序宽限期，推进时只移动段边界，因而
 * 具有 O(段数) 的固定开销；代价是 tails/gp_seq/seglen 必须始终同步，
 * CPU 热插拔合并时部分待处理回调还会保守地重新等待一个宽限期。
 */
/*
 * RCU segmented callback lists, function definitions
 *
 * Copyright IBM Corporation, 2017
 *
 * Authors: Paul E. McKenney <paulmck@linux.ibm.com>
 */

#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "rcu_segcblist.h"

/* Initialize simple callback list. */
/*
 * 初始化普通回调链表。
 *
 * 调用者传入非 NULL、独占可写且无需保留旧内容的 @rclp；本函数不睡眠、
 * 不取锁，也不分配内存。返回：无直接返回值。成功后 head 为空，tail 指向
 * head 自身，len 为 0，形成“向 *tail 写入即可追加”的空链表不变量。
 */
void rcu_cblist_init(struct rcu_cblist *rclp)
{
	rclp->head = NULL;
	rclp->tail = &rclp->head;
	rclp->len = 0;
}

/*
 * Enqueue an rcu_head structure onto the specified callback list.
 */
/*
 * 把一个回调节点追加到普通链表尾部。
 *
 * @rclp 是调用方已串行化、已初始化的输入输出链表；@rhp 是非 NULL、尚未
 * 位于其他链表中的节点，成功后链表取得其链接所有权，调用者不得再修改
 * rhp->next。函数不睡眠、不失败、无直接返回值。
 *
 * tail 不是尾节点，而是“当前最后一个 next 槽位的地址”。先把该槽位指向
 * rhp，再把 tail 移到 rhp->next，因而无需区分空链与非空链。len 使用
 * WRITE_ONCE 发布近似计数，但链表互斥仍由调用方负责。
 */
void rcu_cblist_enqueue(struct rcu_cblist *rclp, struct rcu_head *rhp)
{
	*rclp->tail = rhp;
	rclp->tail = &rhp->next;
	WRITE_ONCE(rclp->len, rclp->len + 1);
}

/*
 * Flush the second rcu_cblist structure onto the first one, obliterating
 * any contents of the first.  If rhp is non-NULL, enqueue it as the sole
 * element of the second rcu_cblist structure, but ensuring that the second
 * rcu_cblist structure, if initially non-empty, always appears non-empty
 * throughout the process.  If rdp is NULL, the second rcu_cblist structure
 * is instead initialized to empty.
 */
/*
 * 把源普通链表整体转移到目标，并把源重置为空或仅含 @rhp。
 *
 * @drclp 是输出链表，原内容会被覆盖且不由本函数释放；@srclp 是输入输出
 * 源链表，其原节点 ownership 全部转给 drclp；@rhp 可为 NULL，非 NULL 时
 * 必须是未链接节点，并成为 srclp 唯一元素。调用方必须保证两个链表不同且
 * 独占，函数不睡眠、无失败返回。
 *
 * 先复制 srclp 到 drclp，再重建 srclp，保证原本非空的源在整个切换窗口
 * 不会短暂表现为空；需要无锁观察这一性质的调用路径因此不会误判。原英文
 * 最后一句的 “rdp” 指的实际参数是 rhp。
 */
void rcu_cblist_flush_enqueue(struct rcu_cblist *drclp,
			      struct rcu_cblist *srclp,
			      struct rcu_head *rhp)
{
	drclp->head = srclp->head;
	if (drclp->head)
		drclp->tail = srclp->tail;
	else
		drclp->tail = &drclp->head;
	drclp->len = srclp->len;
	if (!rhp) {
		rcu_cblist_init(srclp);
	} else {
		rhp->next = NULL;
		srclp->head = rhp;
		srclp->tail = &rhp->next;
		WRITE_ONCE(srclp->len, 1);
	}
}

/*
 * Dequeue the oldest rcu_head structure from the specified callback
 * list.
 */
/*
 * 从普通链表头部摘下最老的回调。
 *
 * @rclp 是调用方独占的输入输出链表。返回 NULL 表示入口为空；否则返回被
 * 摘下节点的借出指针，节点 ownership 交回调用者。函数不清除 rhp->next，
 * 调用者若要重新入队须按目标接口重设链接。无锁、不可睡眠、不会失败。
 *
 * 摘走最后节点时必须让 tail 重新指向 head；否则下一次追加会经由已经脱链
 * 节点的 next 写入，破坏链表可达性。
 */
struct rcu_head *rcu_cblist_dequeue(struct rcu_cblist *rclp)
{
	struct rcu_head *rhp;

	rhp = rclp->head;
	if (!rhp)
		return NULL;
	rclp->len--;
	rclp->head = rhp->next;
	if (!rclp->head)
		rclp->tail = &rclp->head;
	return rhp;
}

/* Set the length of an rcu_segcblist structure. */
/*
 * 覆盖分段链表总计数。
 *
 * @rsclp 为调用方已稳定的输入输出对象，@v 是新的回调数，可在搬运期间暂时
 * 与实际链长不同。NOCB 配置下 len 还会被卸载线程并发访问，故用 atomic；
 * 否则 WRITE_ONCE 足以满足调用方已串行化的访问。无返回、不睡眠。
 *
 * 本 helper 不提供 rcu_barrier() 所需的前后全屏障；只允许初始化或已有
 * 更强外部排他条件的路径使用，普通增减必须走 rcu_segcblist_add_len()。
 */
static void rcu_segcblist_set_len(struct rcu_segcblist *rsclp, long v)
{
#ifdef CONFIG_RCU_NOCB_CPU
	atomic_long_set(&rsclp->len, v);
#else
	WRITE_ONCE(rsclp->len, v);
#endif
}

/* Get the length of a segment of the rcu_segcblist structure. */
/*
 * 读取指定逻辑段的近似回调数。
 *
 * @rsclp 是借用的稳定对象；@seg 必须处于 [RCU_DONE_TAIL,
 * RCU_CBLIST_NSEGS) 且由调用者保证合法。返回该段 seglen 快照，可能因并发
 * 变化立即过时；READ_ONCE 不提供链表拓扑一致性。无副作用、不睡眠。
 */
long rcu_segcblist_get_seglen(struct rcu_segcblist *rsclp, int seg)
{
	return READ_ONCE(rsclp->seglen[seg]);
}

/* Return number of callbacks in segmented callback list by summing seglen. */
/*
 * 汇总四个逻辑段的计数。
 *
 * @rsclp 为借用对象。返回逐段 READ_ONCE 后的和；在并发迁移期间各次读取
 * 不是同一原子快照，因此只适合断言和诊断，不应替代受协议保护的 len。
 * 函数无副作用、不睡眠。
 */
long rcu_segcblist_n_segment_cbs(struct rcu_segcblist *rsclp)
{
	/* len 是本次累加结果，i 依次覆盖 DONE 到 NEXT 的全部合法段号。 */
	long len = 0;
	int i;

	for (i = RCU_DONE_TAIL; i < RCU_CBLIST_NSEGS; i++)
		len += rcu_segcblist_get_seglen(rsclp, i);

	return len;
}

/* Set the length of a segment of the rcu_segcblist structure. */
/*
 * 覆盖一个逻辑段的计数。@seg 必须合法，@v 通常非负；调用方负责锁定
 * tails[]/seglen[] 的共同不变量。WRITE_ONCE 允许诊断并发取样但不提供
 * 互斥。无返回、不睡眠。
 */
static void rcu_segcblist_set_seglen(struct rcu_segcblist *rsclp, int seg, long v)
{
	WRITE_ONCE(rsclp->seglen[seg], v);
}

/* Increase the numeric length of a segment by a specified amount. */
/*
 * 给指定段计数加上 @v。@v 可正可负，但调用者必须保证结果与该段实际节点数
 * 相符且不为负；本函数只做一次可观察写入，不同步 tails[]。无返回、不睡眠。
 */
static void rcu_segcblist_add_seglen(struct rcu_segcblist *rsclp, int seg, long v)
{
	WRITE_ONCE(rsclp->seglen[seg], rsclp->seglen[seg] + v);
}

/* Move from's segment length to to's segment. */
/*
 * 只把 @from 的全部计数并入 @to，不移动任何链表指针。
 *
 * @from/@to 是合法段号，可以相同；@rsclp 由调用方独占。相同段或源计数为
 * 0 时直接返回，否则目标增加、源清零。调用者必须在同一阶段另行更新 tails
 * 或已知物理节点无需移动，才能维持“段边界与计数一致”。无直接返回值。
 */
static void rcu_segcblist_move_seglen(struct rcu_segcblist *rsclp, int from, int to)
{
	/* len 是源段在本次串行化窗口内的节点数快照。 */
	long len;

	if (from == to)
		return;

	len = rcu_segcblist_get_seglen(rsclp, from);
	if (!len)
		return;

	rcu_segcblist_add_seglen(rsclp, to, len);
	rcu_segcblist_set_seglen(rsclp, from, 0);
}

/* Increment segment's length. */
/*
 * 把 @seg 的计数增加 1，用于单节点入队或 entrain。参数和同步约束与
 * rcu_segcblist_add_seglen() 相同；无返回、不睡眠。
 */
static void rcu_segcblist_inc_seglen(struct rcu_segcblist *rsclp, int seg)
{
	rcu_segcblist_add_seglen(rsclp, seg, 1);
}

/*
 * Increase the numeric length of an rcu_segcblist structure by the
 * specified amount, which can be negative.  This can cause the ->len
 * field to disagree with the actual number of callbacks on the structure.
 * This increase is fully ordered with respect to the callers accesses
 * both before and after.
 *
 * So why on earth is a memory barrier required both before and after
 * the update to the ->len field???
 *
 * The reason is that rcu_barrier() locklessly samples each CPU's ->len
 * field, and if a given CPU's field is zero, avoids IPIing that CPU.
 * This can of course race with both queuing and invoking of callbacks.
 * Failing to correctly handle either of these races could result in
 * rcu_barrier() failing to IPI a CPU that actually had callbacks queued
 * which rcu_barrier() was obligated to wait on.  And if rcu_barrier()
 * failed to wait on such a callback, unloading certain kernel modules
 * would result in calls to functions whose code was no longer present in
 * the kernel, for but one example.
 *
 * Therefore, ->len transitions from 1->0 and 0->1 have to be carefully
 * ordered with respect with both list modifications and the rcu_barrier().
 *
 * The queuing case is CASE 1 and the invoking case is CASE 2.
 *
 * CASE 1: Suppose that CPU 0 has no callbacks queued, but invokes
 * call_rcu() just as CPU 1 invokes rcu_barrier().  CPU 0's ->len field
 * will transition from 0->1, which is one of the transitions that must
 * be handled carefully.  Without the full memory barriers after the ->len
 * update and at the beginning of rcu_barrier(), the following could happen:
 *
 * CPU 0				CPU 1
 *
 * call_rcu().
 *					rcu_barrier() sees ->len as 0.
 * set ->len = 1.
 *					rcu_barrier() does nothing.
 *					module is unloaded.
 * callback invokes unloaded function!
 *
 * With the full barriers, any case where rcu_barrier() sees ->len as 0 will
 * have unambiguously preceded the return from the racing call_rcu(), which
 * means that this call_rcu() invocation is OK to not wait on.  After all,
 * you are supposed to make sure that any problematic call_rcu() invocations
 * happen before the rcu_barrier().
 *
 *
 * CASE 2: Suppose that CPU 0 is invoking its last callback just as
 * CPU 1 invokes rcu_barrier().  CPU 0's ->len field will transition from
 * 1->0, which is one of the transitions that must be handled carefully.
 * Without the full memory barriers before the ->len update and at the
 * end of rcu_barrier(), the following could happen:
 *
 * CPU 0				CPU 1
 *
 * start invoking last callback
 * set ->len = 0 (reordered)
 *					rcu_barrier() sees ->len as 0
 *					rcu_barrier() does nothing.
 *					module is unloaded
 * callback executing after unloaded!
 *
 * With the full barriers, any case where rcu_barrier() sees ->len as 0
 * will be fully ordered after the completion of the callback function,
 * so that the module unloading operation is completely safe.
 *
 */
/*
 * 调整总回调数，并与调用者前后的链表操作建立完整顺序。
 *
 * @rsclp 是借用的分段链表；@v 是增量，入队通常为正、回调执行完成后的
 * 记账通常为负。返回：无直接返回值；副作用是更新 len。调用方负责保证
 * 结果不为负，并负责链表拓扑的锁；函数本身不可睡眠。
 *
 * 上述英文说明的核心是 rcu_barrier() 把 len==0 当成“不必向该 CPU 投递
 * barrier 回调”。因此两侧全屏障同时封住两种危险：
 *
 *   入队 0→1：节点发布和 len 墧长必须相对 barrier 的起始屏障有序；
 *   执行 1→0：回调函数结束必须先于 len 清零和 barrier 的结束屏障。
 *
 * 若缺少任一方向，barrier 可能在模块回调尚未登记或尚未执行完时返回，
 * 随后的模块卸载便会让回调跳入已释放代码。NOCB 使用 atomic 加专用屏障，
 * 普通配置使用 WRITE_ONCE 加完整 smp_mb()，两者提供同一跨 CPU 契约。
 */
void rcu_segcblist_add_len(struct rcu_segcblist *rsclp, long v)
{
#ifdef CONFIG_RCU_NOCB_CPU
	smp_mb__before_atomic(); // Read header comment above.
	/* 对应上文：先完成调用者在计数更新前的节点/回调访问。 */
	atomic_long_add(v, &rsclp->len);
	smp_mb__after_atomic();  // Read header comment above.
	/* 对应上文：后续发布或返回不得越过本次原子计数更新。 */
#else
	smp_mb(); // Read header comment above.
	/* 非原子配置同样需要在计数写之前完成调用者先前的访问。 */
	WRITE_ONCE(rsclp->len, rsclp->len + v);
	smp_mb(); // Read header comment above.
	/* 与 rcu_barrier() 的屏障配对，使计数写早于调用者后续访问。 */
#endif
}

/*
 * Increase the numeric length of an rcu_segcblist structure by one.
 * This can cause the ->len field to disagree with the actual number of
 * callbacks on the structure.  This increase is fully ordered with respect
 * to the callers accesses both before and after.
 */
/*
 * 以完整内存序把总计数增加 1。
 *
 * @rsclp 是借用的输入输出链表；无返回、不睡眠。它是 add_len(+1) 的语义
 * wrapper，通常用于发布新回调，调用者仍须同步更新具体段计数与链表链接。
 */
void rcu_segcblist_inc_len(struct rcu_segcblist *rsclp)
{
	rcu_segcblist_add_len(rsclp, 1);
}

/*
 * Initialize an rcu_segcblist structure.
 */
/*
 * 初始化一个可接收回调的四段链表。
 *
 * @rsclp 必须是非 NULL、未被并发访问且旧内容无需保留的对象。返回：无直接
 * 返回值；不分配内存、不睡眠。成功后 head=NULL，全部 tails 指向 head，
 * 全部段计数与总计数为 0，并发布 SEGCBLIST_ENABLED。
 *
 * BUILD_BUG_ON 在编译期验证 tails[] 与 gp_seq[] 的布局契约；若新增段却未
 * 同步数组大小，构建直接失败，避免运行时越界或段号错配。
 */
void rcu_segcblist_init(struct rcu_segcblist *rsclp)
{
	/* i 仅在初始化阶段遍历全部四个逻辑段。 */
	int i;

	/* 阶段 1：先证明数组布局，再建立空链表的共享尾指针表示。 */
	BUILD_BUG_ON(RCU_NEXT_TAIL + 1 != ARRAY_SIZE(rsclp->gp_seq));
	BUILD_BUG_ON(ARRAY_SIZE(rsclp->tails) != ARRAY_SIZE(rsclp->gp_seq));
	rsclp->head = NULL;
	for (i = 0; i < RCU_CBLIST_NSEGS; i++) {
		rsclp->tails[i] = &rsclp->head;
		rcu_segcblist_set_seglen(rsclp, i, 0);
	}
	/* 阶段 2：清计数后最后启用，避免观察者使用半初始化的段边界。 */
	rcu_segcblist_set_len(rsclp, 0);
	rcu_segcblist_set_flags(rsclp, SEGCBLIST_ENABLED);
}

/*
 * Disable the specified rcu_segcblist structure, so that callbacks can
 * no longer be posted to it.  This structure must be empty.
 */
/*
 * 禁用一个已经排空的分段链表，使入队协议不再把它当作可用队列。
 *
 * @rsclp 是调用方独占的输入输出对象。入口要求物理链和总计数均为 0；
 * 两个 WARN 分别检查拓扑与记账，便于发现“已摘出但尚未完成记账”的状态。
 * 即使告警仍会清 ENABLED，因此调用方必须保证前置条件，不能把告警当恢复
 * 机制。返回：无直接返回值；不睡眠、不释放回调。
 */
void rcu_segcblist_disable(struct rcu_segcblist *rsclp)
{
	WARN_ON_ONCE(!rcu_segcblist_empty(rsclp));
	WARN_ON_ONCE(rcu_segcblist_n_cbs(rsclp));
	rcu_segcblist_clear_flags(rsclp, SEGCBLIST_ENABLED);
}

/*
 * Does the specified rcu_segcblist structure contain callbacks that
 * are ready to be invoked?
 */
/*
 * 判断 DONE 段是否非空。
 *
 * @rsclp 是借用对象；返回 true 仅当队列已启用且 head 到 DONE 尾边界之间
 * 至少有一个节点，否则返回 false。读取是无锁快照，适合作为持有调用方锁
 * 后的快速判定；它不取得节点 ownership，也不保证返回后状态不变。
 */
bool rcu_segcblist_ready_cbs(struct rcu_segcblist *rsclp)
{
	return rcu_segcblist_is_enabled(rsclp) &&
	       &rsclp->head != READ_ONCE(rsclp->tails[RCU_DONE_TAIL]);
}

/*
 * Does the specified rcu_segcblist structure contain callbacks that
 * are still pending, that is, not yet ready to be invoked?
 */
/*
 * 判断 DONE 之后是否还有等待宽限期的回调。
 *
 * @rsclp 为借用对象；返回 true 表示 WAIT/NEXT_READY/NEXT 至少一段非空，
 * false 表示禁用或所有非 DONE 段为空。无副作用、不睡眠；无锁结果可能立即
 * 过时，修改者仍须遵循调用方的队列锁协议。
 */
bool rcu_segcblist_pend_cbs(struct rcu_segcblist *rsclp)
{
	return rcu_segcblist_is_enabled(rsclp) &&
	       !rcu_segcblist_restempty(rsclp, RCU_DONE_TAIL);
}

/*
 * Return a pointer to the first callback in the specified rcu_segcblist
 * structure.  This is useful for diagnostics.
 */
/*
 * 返回整条物理链的首节点供诊断使用。
 *
 * @rsclp 是借用对象。启用时返回 head（可为 NULL），禁用时固定返回 NULL；
 * 返回的是无引用裸指针，不转移节点 ownership，只能在调用方已有生命周期
 * 保护的窗口内观察，不能据此执行或摘链。无副作用、不睡眠。
 */
struct rcu_head *rcu_segcblist_first_cb(struct rcu_segcblist *rsclp)
{
	if (rcu_segcblist_is_enabled(rsclp))
		return rsclp->head;
	return NULL;
}

/*
 * Return a pointer to the first pending callback in the specified
 * rcu_segcblist structure.  This is useful just after posting a given
 * callback -- if that callback is the first pending callback, then
 * you cannot rely on someone else having already started up the required
 * grace period.
 */
/*
 * 返回第一个尚未 ready 的回调，用于判断是否需要启动宽限期。
 *
 * @rsclp 是借用对象。启用时解引用 DONE 的尾槽：该槽正是 WAIT 或其后第一个
 * 节点的链接位置，因此结果可为 NULL；禁用时返回 NULL。返回值不带引用、
 * 不转移 ownership，调用者必须在队列同步范围内使用。
 *
 * 新回调若等于此返回值，说明它是当前最老的 pending 回调，不能假设此前
 * 已有回调替它触发 GP；调用者通常需要显式唤醒或请求宽限期。
 */
struct rcu_head *rcu_segcblist_first_pend_cb(struct rcu_segcblist *rsclp)
{
	if (rcu_segcblist_is_enabled(rsclp))
		return *rsclp->tails[RCU_DONE_TAIL];
	return NULL;
}

/*
 * Return false if there are no CBs awaiting grace periods, otherwise,
 * return true and store the nearest waited-upon grace period into *lp.
 */
/*
 * 查询当前最早等待的宽限期序号。
 *
 * @rsclp 是借用输入；@lp 是非 NULL 输出指针，仅在返回 true 时写入，调用前
 * 内容无要求、ownership 不变。若没有 pending 回调返回 false 且不修改
 * *lp；否则返回 true，并输出 WAIT 段的 gp_seq。无锁快照可能过时，但序号
 * 可供 GP 驱动逻辑决定下一次等待目标；函数不睡眠。
 */
bool rcu_segcblist_nextgp(struct rcu_segcblist *rsclp, unsigned long *lp)
{
	if (!rcu_segcblist_pend_cbs(rsclp))
		return false;
	*lp = rsclp->gp_seq[RCU_WAIT_TAIL];
	return true;
}

/*
 * Enqueue the specified callback onto the specified rcu_segcblist
 * structure, updating accounting as needed.  Note that the ->len
 * field may be accessed locklessly, hence the WRITE_ONCE().
 * The ->len field is used by rcu_barrier() and friends to determine
 * if it must post a callback on this structure, and it is OK
 * for rcu_barrier() to sometimes post callbacks needlessly, but
 * absolutely not OK for it to ever miss posting a callback.
 */
/*
 * 把新回调追加到 NEXT 段并发布总计数。
 *
 * @rsclp 必须已启用且由调用方持有其队列同步；@rhp 是非 NULL、尚未链接的
 * 输入节点，成功后链接 ownership 转给 rsclp。返回：无直接返回值；不睡眠、
 * 不失败，回调尚未绑定精确 GP，后续由 accelerate() 完成。
 *
 * 阶段顺序刻意先增加 len，再链接节点。rcu_barrier() 可以因并发而多投递
 * 哨兵，却绝不能看到 0 而漏投；inc_len() 的全屏障和这里的 WRITE_ONCE
 * 共同发布 0→1 转换。seglen、next 和 NEXT 尾槽随后同步推进，保持物理链
 * 与逻辑段计数一致。
 */
void rcu_segcblist_enqueue(struct rcu_segcblist *rsclp,
			   struct rcu_head *rhp)
{
	/* 阶段 1：先让无锁 barrier 观察者保守地看到“可能有回调”。 */
	rcu_segcblist_inc_len(rsclp);
	rcu_segcblist_inc_seglen(rsclp, RCU_NEXT_TAIL);
	/* 阶段 2：把节点发布到 NEXT 尾部，并把尾槽移到新节点的 next。 */
	rhp->next = NULL;
	WRITE_ONCE(*rsclp->tails[RCU_NEXT_TAIL], rhp);
	WRITE_ONCE(rsclp->tails[RCU_NEXT_TAIL], &rhp->next);
}

/*
 * Entrain the specified callback onto the specified rcu_segcblist at
 * the end of the last non-empty segment.  If the entire rcu_segcblist
 * is empty, make no change, but return false.
 *
 * This is intended for use by rcu_barrier()-like primitives, -not-
 * for normal grace-period use.  IMPORTANT:  The callback you enqueue
 * will wait for all prior callbacks, NOT necessarily for a grace
 * period.  You have been warned.
 */
/*
 * 把 barrier 类哨兵附着到最后一个非空段的末尾。
 *
 * @rsclp 是调用方锁定的已启用链表；@rhp 是非 NULL、未链接的专用节点，
 * 成功后 ownership 转给队列。若总计数为 0，返回 false 且两者不变；否则
 * 返回 true，哨兵会排在入口时所有回调之后。
 *
 * 这不是普通 GP 入队：哨兵继承“最后非空段”的完成位置，只保证先前回调都
 * 已执行，不承诺自己额外等待一个完整宽限期。典型调用者是 rcu_barrier()。
 * 函数不睡眠；调用方必须阻止并发段迁移。
 */
bool rcu_segcblist_entrain(struct rcu_segcblist *rsclp,
			   struct rcu_head *rhp)
{
	/* i 从最新段向前寻找最后一个真实含有节点的逻辑段。 */
	int i;

	if (rcu_segcblist_n_cbs(rsclp) == 0)
		return false;
	/*
	 * 先发布 len 增量，再用额外全屏障保证计数对 barrier 可见，之后才把
	 * 哨兵链接进去；这样无锁观察者不会漏掉已经可达的 barrier 节点。
	 */
	rcu_segcblist_inc_len(rsclp);
	smp_mb(); /* Ensure counts are updated before callback is entrained. */
	/* 计数必须先于哨兵入链可见，避免 barrier 把非空队列误判为空。 */
	rhp->next = NULL;
	for (i = RCU_NEXT_TAIL; i > RCU_DONE_TAIL; i--)
		if (!rcu_segcblist_segempty(rsclp, i))
			break;
	/*
	 * 把节点接到选中段后，并将该段以及所有更晚的空段尾指针一起推进；
	 * 否则相等 tails 表示的空段关系会被破坏。
	 */
	rcu_segcblist_inc_seglen(rsclp, i);
	WRITE_ONCE(*rsclp->tails[i], rhp);
	for (; i <= RCU_NEXT_TAIL; i++)
		WRITE_ONCE(rsclp->tails[i], &rhp->next);
	return true;
}

/*
 * Extract only those callbacks ready to be invoked from the specified
 * rcu_segcblist structure and place them in the specified rcu_cblist
 * structure.
 */
/*
 * 把 DONE 段整体摘到普通链表，供调用者执行。
 *
 * @rsclp 是已锁定的输入输出分段链表；@rclp 是已初始化的输入输出普通
 * 链表，通常入口为空，其 tail 用作拼接槽。无 ready 回调时直接返回且
 * 两者不变；否则 DONE 节点的链接 ownership 转给 rclp。无直接返回值、
 * 不睡眠，且故意不减少总 len：只有回调真正执行后才能完成 1→0 记账。
 */
void rcu_segcblist_extract_done_cbs(struct rcu_segcblist *rsclp,
				    struct rcu_cblist *rclp)
{
	/* i 反向修复所有曾与旧 DONE 尾重合的空段边界。 */
	int i;

	if (!rcu_segcblist_ready_cbs(rsclp))
		return; /* Nothing to do. */
	/* 当前没有可执行回调，避免无意义地改写链表边界。 */
	/* 阶段 1：把 [head, DONE-tail) 拼到 rclp，并在边界处截断。 */
	rclp->len = rcu_segcblist_get_seglen(rsclp, RCU_DONE_TAIL);
	*rclp->tail = rsclp->head;
	WRITE_ONCE(rsclp->head, *rsclp->tails[RCU_DONE_TAIL]);
	WRITE_ONCE(*rsclp->tails[RCU_DONE_TAIL], NULL);
	rclp->tail = rsclp->tails[RCU_DONE_TAIL];
	/*
	 * 阶段 2：rsclp 的新 head 是原第一个 pending 节点。凡尾指针与旧 DONE
	 * 尾重合的空段现在都必须改指新 head，继续表达“这些段为空”。
	 */
	for (i = RCU_CBLIST_NSEGS - 1; i >= RCU_DONE_TAIL; i--)
		if (rsclp->tails[i] == rsclp->tails[RCU_DONE_TAIL])
			WRITE_ONCE(rsclp->tails[i], &rsclp->head);
	rcu_segcblist_set_seglen(rsclp, RCU_DONE_TAIL, 0);
}

/*
 * Extract only those callbacks still pending (not yet ready to be
 * invoked) from the specified rcu_segcblist structure and place them in
 * the specified rcu_cblist structure.  Note that this loses information
 * about any callbacks that might have been partway done waiting for
 * their grace period.  Too bad!  They will have to start over.
 */
/*
 * 把全部 pending 段摘到普通链表，并丢弃其 GP 分段信息。
 *
 * @rsclp 是调用方锁定的输入输出对象；@rclp 是已初始化、通常为空的输出
 * 链表。没有 pending 回调时直接返回；否则 WAIT 到 NEXT 的链接 ownership
 * 转给 rclp，DONE 保留在 rsclp。无直接返回值、不睡眠。
 *
 * gp_seq 不随普通链表保存，因此这些回调日后回插 NEXT 后必须重新
 * accelerate，可能保守地多等一个 GP，但绝不能提早执行。
 */
void rcu_segcblist_extract_pend_cbs(struct rcu_segcblist *rsclp,
				    struct rcu_cblist *rclp)
{
	/* i 遍历所有非 DONE 段，汇总计数并把其边界折叠到 DONE 尾。 */
	int i;

	if (!rcu_segcblist_pend_cbs(rsclp))
		return; /* Nothing to do. */
	/* 没有待 GP 回调，保持输出链表及所有分段元数据不变。 */
	/* 阶段 1：从 DONE 尾槽接管 pending 子链，尾部沿用 NEXT 尾槽。 */
	rclp->len = 0;
	*rclp->tail = *rsclp->tails[RCU_DONE_TAIL];
	rclp->tail = rsclp->tails[RCU_NEXT_TAIL];
	WRITE_ONCE(*rsclp->tails[RCU_DONE_TAIL], NULL);
	/* 阶段 2：清空各 pending 段计数并让所有尾边界折叠到 DONE 末端。 */
	for (i = RCU_DONE_TAIL + 1; i < RCU_CBLIST_NSEGS; i++) {
		rclp->len += rcu_segcblist_get_seglen(rsclp, i);
		WRITE_ONCE(rsclp->tails[i], rsclp->tails[RCU_DONE_TAIL]);
		rcu_segcblist_set_seglen(rsclp, i, 0);
	}
}

/*
 * Insert counts from the specified rcu_cblist structure in the
 * specified rcu_segcblist structure.
 */
/*
 * 只把普通链表的数量计入分段链表总数。
 *
 * @rsclp 是输入输出目标；@rclp 是借用的数量来源，内容和 ownership 不变。
 * 返回：无直接返回值。该函数不链接节点也不更新 seglen，必须与后续
 * insert_done_cbs()/insert_pend_cbs() 成对使用；add_len() 提供 barrier
 * 所需全序。调用方负责串行化，函数不睡眠。
 */
void rcu_segcblist_insert_count(struct rcu_segcblist *rsclp,
				struct rcu_cblist *rclp)
{
	rcu_segcblist_add_len(rsclp, rclp->len);
}

/*
 * Move callbacks from the specified rcu_cblist to the beginning of the
 * done-callbacks segment of the specified rcu_segcblist.
 */
/*
 * 把普通链表前插到分段链表 DONE 段。
 *
 * @rsclp 是调用方锁定的目标；@rclp 是输入输出源，非空时其全部节点
 * ownership 转给 rsclp，返回后 rclp 重置为空。空源直接返回。无直接
 * 返回值、不睡眠；总 len 必须已由 insert_count() 单独计入。
 *
 * 前插而非后插可保持“先前摘出、尚未执行完”的回调仍早于后来 ready 的
 * 回调。只推进从 head 开始连续为空的段边界，遇到首个非空段即停止。
 */
void rcu_segcblist_insert_done_cbs(struct rcu_segcblist *rsclp,
				   struct rcu_cblist *rclp)
{
	/* i 从 DONE 向后修复与旧 head 重合的连续空段。 */
	int i;

	if (!rclp->head)
		return; /* No callbacks to move. */
	/* 空源没有节点 ownership 可转移。 */
	/* 阶段 1：源尾接旧 head，随后把 rsclp->head 发布为源首节点。 */
	rcu_segcblist_add_seglen(rsclp, RCU_DONE_TAIL, rclp->len);
	*rclp->tail = rsclp->head;
	WRITE_ONCE(rsclp->head, rclp->head);
	/* 阶段 2：让原本为空的前导段尾边界随新 DONE 尾向后移动。 */
	for (i = RCU_DONE_TAIL; i < RCU_CBLIST_NSEGS; i++)
		if (&rsclp->head == rsclp->tails[i])
			WRITE_ONCE(rsclp->tails[i], rclp->tail);
		else
			break;
	rclp->head = NULL;
	rclp->tail = &rclp->head;
}

/*
 * Move callbacks from the specified rcu_cblist to the end of the
 * new-callbacks segment of the specified rcu_segcblist.
 */
/*
 * 把普通链表追加到分段链表 NEXT 段。
 *
 * @rsclp 是调用方锁定的目标；@rclp 是借用源，其节点在非空时转移给
 * rsclp。空源直接返回。无直接返回值、不睡眠；本函数不会清空 rclp 的
 * 元数据，也不增加总 len，调用方通常随后丢弃/重置临时链表，并必须已用
 * insert_count() 完成总数记账。
 */
void rcu_segcblist_insert_pend_cbs(struct rcu_segcblist *rsclp,
				   struct rcu_cblist *rclp)
{
	if (!rclp->head)
		return; /* Nothing to do. */
	/* 空源无需更新 NEXT 段边界。 */

	/* 源首节点接入旧 NEXT 尾槽，再接管源尾槽，完成整批 ownership 转移。 */
	rcu_segcblist_add_seglen(rsclp, RCU_NEXT_TAIL, rclp->len);
	WRITE_ONCE(*rsclp->tails[RCU_NEXT_TAIL], rclp->head);
	WRITE_ONCE(rsclp->tails[RCU_NEXT_TAIL], rclp->tail);
}

/*
 * Advance the callbacks in the specified rcu_segcblist structure based
 * on the current value passed in for the grace-period counter.
 */
/*
 * 按当前已完成的宽限期序号推进分段，把可执行回调移入 DONE。
 *
 * 典型调用位置：
 *   GP 状态更新 → rcu_segcblist_advance() → extract_done_cbs() 执行回调。
 *
 * @rsclp 是已启用、由调用方队列锁或等价 per-CPU 协议独占的输入输出对象；
 * @seq 是当前已完成/可确认通过的 GP 序号，使用回绕安全比较，不能用普通
 * 无符号大小比较替代。返回：无直接返回值；不睡眠、不分配、不改变总 len。
 *
 * 函数先把 gp_seq<=seq 的连续旧段并入 DONE，再压缩剩余段以填补空洞。
 * 节点物理顺序始终不动，只修改 tails[]、seglen[] 和必要的 gp_seq[]；
 * 因此开销与固定段数有关，而与回调数量无关。若队列禁用只告警，调用者
 * 仍须满足前置条件。
 */
void rcu_segcblist_advance(struct rcu_segcblist *rsclp, unsigned long seq)
{
	/*
	 * i 扫描可完成的旧段并最终指向首个未完成段；j 用于修复空段边界和
	 * 把后续段向前压缩。二者仅在调用方串行化窗口内有效。
	 */
	int i, j;

	WARN_ON_ONCE(!rcu_segcblist_is_enabled(rsclp));
	/* 快速路径：DONE 之后没有任何节点，既无 GP 状态可推进也无需改边界。 */
	if (rcu_segcblist_restempty(rsclp, RCU_DONE_TAIL))
		return;

	/*
	 * Find all callbacks whose ->gp_seq numbers indicate that they
	 * are ready to invoke, and put them into the RCU_DONE_TAIL segment.
	 */
	/*
	 * 阶段 1：从最老 WAIT 段开始，连续吸收目标序号不晚于 seq 的段。
	 * ULONG_CMP_LT 处理序号回绕；遇到第一个仍在未来的段就停止，因为段按
	 * GP 先后有序。移动尾边界即可让节点逻辑上属于 DONE，随后同步计数。
	 */
	for (i = RCU_WAIT_TAIL; i < RCU_NEXT_TAIL; i++) {
		if (ULONG_CMP_LT(seq, rsclp->gp_seq[i]))
			break;
		WRITE_ONCE(rsclp->tails[RCU_DONE_TAIL], rsclp->tails[i]);
		rcu_segcblist_move_seglen(rsclp, i, RCU_DONE_TAIL);
	}

	/* If no callbacks moved, nothing more need be done. */
	/* 若最老 WAIT 尚未完成，后续段更不可能完成，保持所有元数据不变。 */
	if (i == RCU_WAIT_TAIL)
		return;

	/* Clean up tail pointers that might have been misordered above. */
	/*
	 * 阶段 2：被吸收段现在都为空，它们的 tails 必须与新 DONE 尾相等；
	 * 不修复会让 segempty() 误以为这些段仍含节点。
	 */
	for (j = RCU_WAIT_TAIL; j < i; j++)
		WRITE_ONCE(rsclp->tails[j], rsclp->tails[RCU_DONE_TAIL]);

	/*
	 * Callbacks moved, so there might be an empty RCU_WAIT_TAIL
	 * and a non-empty RCU_NEXT_READY_TAIL.  If so, copy the
	 * RCU_NEXT_READY_TAIL segment to fill the RCU_WAIT_TAIL gap
	 * created by the now-ready-to-invoke segments.
	 */
	/*
	 * 阶段 3：把尚未完成的后继段依次左移填空。这里复制的是尾边界、段计数
	 * 和目标 gp_seq，物理节点不复制；当目标尾已等于 NEXT 尾时说明没有
	 * 更多已绑定 GP 的回调，剩余逻辑段自然为空。
	 */
	for (j = RCU_WAIT_TAIL; i < RCU_NEXT_TAIL; i++, j++) {
		if (rsclp->tails[j] == rsclp->tails[RCU_NEXT_TAIL])
			break;  /* No more callbacks. */
		/* 已到物理链尾，不再有可压缩的等待段。 */
		WRITE_ONCE(rsclp->tails[j], rsclp->tails[i]);
		rcu_segcblist_move_seglen(rsclp, i, j);
		rsclp->gp_seq[j] = rsclp->gp_seq[i];
	}
}

/*
 * "Accelerate" callbacks based on more-accurate grace-period information.
 * The reason for this is that RCU does not synchronize the beginnings and
 * ends of grace periods, and that callbacks are posted locally.  This in
 * turn means that the callbacks must be labelled conservatively early
 * on, as getting exact information would degrade both performance and
 * scalability.  When more accurate grace-period information becomes
 * available, previously posted callbacks can be "accelerated", marking
 * them to complete at the end of the earlier grace period.
 *
 * This function operates on an rcu_segcblist structure, and also the
 * grace-period sequence number seq at which new callbacks would become
 * ready to invoke.  Returns true if there are callbacks that won't be
 * ready to invoke until seq, false otherwise.
 */
/*
 * 用更准确的 GP 信息给新回调绑定最早安全序号，并合并保守分段。
 *
 * 典型调用位置：
 *   call_rcu() 把节点放入 NEXT
 *     → GP 驱动取得精确快照 seq
 *     → rcu_segcblist_accelerate()
 *     → 后续 advance() 在 seq 完成时转入 DONE。
 *
 * @rsclp 是已启用且由调用方串行化的输入输出链表；@seq 是“现在新回调
 * 最早可在哪个 GP 结束时执行”的回绕序号。返回 true 表示处理后仍有回调
 * 需要等到 @seq（通常也意味着 GP 驱动需要确保该 GP 运行）；返回 false
 * 表示没有形成这样的等待段，可能因为无 pending 回调，或已有分段都早于
 * @seq、应先由 advance() 处理。无睡眠、无分配、总 len 不变。
 *
 * “加速”不是绕过 GP，而是把先前因本地信息不足而标得过晚的回调合并到
 * 已证明安全的更早序号。节点物理顺序不变，故回调 FIFO 顺序仍保持。
 */
bool rcu_segcblist_accelerate(struct rcu_segcblist *rsclp, unsigned long seq)
{
	/*
	 * i 从新向旧定位不可合并的最后一个早期段，随后成为合并目标；
	 * j 只汇总将被吸收的较新段计数。
	 */
	int i, j;

	WARN_ON_ONCE(!rcu_segcblist_is_enabled(rsclp));
	/* 快速路径：DONE 之外为空，没有新回调需要绑定 GP。 */
	if (rcu_segcblist_restempty(rsclp, RCU_DONE_TAIL))
		return false;

	/*
	 * Find the segment preceding the oldest segment of callbacks
	 * whose ->gp_seq[] completion is at or after that passed in via
	 * "seq", skipping any empty segments.  This oldest segment, along
	 * with any later segments, can be merged in with any newly arrived
	 * callbacks in the RCU_NEXT_TAIL segment, and assigned "seq"
	 * as their ->gp_seq[] grace-period completion sequence number.
	 */
	/*
	 * 阶段 1：从 NEXT_READY 向旧方向跳过空段，寻找最后一个目标严格早于
	 * seq 的非空段。它和所有更老段必须保留原 gp_seq；它之后的回调目标
	 * 不早于 seq，可以与 NEXT 新回调一起保守地归到 seq。
	 */
	for (i = RCU_NEXT_READY_TAIL; i > RCU_DONE_TAIL; i--)
		if (!rcu_segcblist_segempty(rsclp, i) &&
		    ULONG_CMP_LT(rsclp->gp_seq[i], seq))
			break;

	/*
	 * If all the segments contain callbacks that correspond to
	 * earlier grace-period sequence numbers than "seq", leave.
	 * Assuming that the rcu_segcblist structure has enough
	 * segments in its arrays, this can only happen if some of
	 * the non-done segments contain callbacks that really are
	 * ready to invoke.  This situation will get straightened
	 * out by the next call to rcu_segcblist_advance().
	 *
	 * Also advance to the oldest segment of callbacks whose
	 * ->gp_seq[] completion is at or after that passed in via "seq",
	 * skipping any empty segments.
	 *
	 * Note that segment "i" (and any lower-numbered segments
	 * containing older callbacks) will be unaffected, and their
	 * grace-period numbers remain unchanged.  For example, if i ==
	 * WAIT_TAIL, then neither WAIT_TAIL nor DONE_TAIL will be touched.
	 * Instead, the CBs in NEXT_TAIL will be merged with those in
	 * NEXT_READY_TAIL and the grace-period number of NEXT_READY_TAIL
	 * would be updated.  NEXT_TAIL would then be empty.
	 */
	/*
	 * 阶段 2：确认确有可合并区间。
	 *
	 * restempty(i) 表示候选边界之后根本没有节点；++i 到达 NEXT 表示只有
	 * NEXT 或更早分段不满足本次重标条件。这两种情况都返回 false。否则 i
	 * 指向最老的合并目标，低编号段保持原序号和节点归属。
	 */
	if (rcu_segcblist_restempty(rsclp, i) || ++i >= RCU_NEXT_TAIL)
		return false;

	/* Accounting: everything below i is about to get merged into i. */
	/* 阶段 3：先把所有较新段（含 NEXT）的计数并入目标段。 */
	for (j = i + 1; j <= RCU_NEXT_TAIL; j++)
		rcu_segcblist_move_seglen(rsclp, j, i);

	/*
	 * Merge all later callbacks, including newly arrived callbacks,
	 * into the segment located by the for-loop above.  Assign "seq"
	 * as the ->gp_seq[] value in order to correctly handle the case
	 * where there were no pending callbacks in the rcu_segcblist
	 * structure other than in the RCU_NEXT_TAIL segment.
	 */
	/*
	 * 阶段 4：把目标至 NEXT_READY 的尾边界全部推进到 NEXT 尾，并统一写入
	 * seq。相等尾指针会把中间逻辑段折叠为空；最终 NEXT 也为空，所有新节点
	 * 已获得可供 advance() 判断的 GP 目标。写 gp_seq 即本次状态提交点。
	 */
	for (; i < RCU_NEXT_TAIL; i++) {
		WRITE_ONCE(rsclp->tails[i], rsclp->tails[RCU_NEXT_TAIL]);
		rsclp->gp_seq[i] = seq;
	}
	return true;
}

/*
 * Merge the source rcu_segcblist structure into the destination
 * rcu_segcblist structure, then initialize the source.  Any pending
 * callbacks from the source get to start over.  It is best to
 * advance and accelerate both the destination and the source
 * before merging.
 */
/*
 * 在 CPU 热插拔期间把源分段链表合并进目标，并重新初始化源。
 *
 * 典型调用者是 RCU CPU-offline 迁移路径。@dst_rsclp/@src_rsclp 均为非
 * NULL、不同对象，调用者持有 CPU hotplug 锁并排除了常规并发修改；源中
 * DONE 回调前插到目标 DONE，pending 回调追加到目标 NEXT。成功后目标取得
 * 源全部节点 ownership，源成为已启用空链表。返回：无直接返回值；不分配、
 * 不睡眠、无失败路径。
 *
 * 源 pending 的 gp_seq 分段会丢失，回插 NEXT 后重新 accelerate，可能多等
 * 一个 GP。调用前先 advance/accelerate 两端可减少这种保守退化。热插拔锁
 * 同时排除 rcu_barrier()，所以源 len 清零无需 add_len() 的全屏障。
 */
void rcu_segcblist_merge(struct rcu_segcblist *dst_rsclp,
			 struct rcu_segcblist *src_rsclp)
{
	/*
	 * donecbs/pendcbs 是栈上临时搬运容器：前者保存已 ready 节点，后者保存
	 * 仍待 GP 节点；不拥有节点内存，只在本函数内承接链接 ownership。
	 */
	struct rcu_cblist donecbs;
	struct rcu_cblist pendcbs;

	/* 证明外层热插拔排他条件存在；它既稳定 per-CPU 队列也排除 barrier。 */
	lockdep_assert_cpus_held();

	/* 阶段 1：建立两个空临时链，再从源按 DONE/pending 边界完整摘出。 */
	rcu_cblist_init(&donecbs);
	rcu_cblist_init(&pendcbs);

	rcu_segcblist_extract_done_cbs(src_rsclp, &donecbs);
	rcu_segcblist_extract_pend_cbs(src_rsclp, &pendcbs);

	/*
	 * No need smp_mb() before setting length to 0, because CPU hotplug
	 * lock excludes rcu_barrier.
	 */
	/*
	 * 阶段 2：源已无可达节点，清总数。CPU hotplug 锁保证 barrier 不能同时
	 * 无锁据此作投递决定，因此这里使用 set_len() 而不是带双屏障的 add_len()。
	 */
	rcu_segcblist_set_len(src_rsclp, 0);

	/*
	 * 阶段 3：先把两批数量发布到目标总数，再分别恢复 DONE 与 NEXT 链接。
	 * DONE 前插保持其优先执行；pending 重新从 NEXT 开始等待精确 GP。
	 */
	rcu_segcblist_insert_count(dst_rsclp, &donecbs);
	rcu_segcblist_insert_count(dst_rsclp, &pendcbs);
	rcu_segcblist_insert_done_cbs(dst_rsclp, &donecbs);
	rcu_segcblist_insert_pend_cbs(dst_rsclp, &pendcbs);

	/* 阶段 4：源不再持有任何节点，重建完整空队列不变量供日后复用。 */
	rcu_segcblist_init(src_rsclp);
}
