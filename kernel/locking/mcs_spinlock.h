/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MCS lock defines
 *
 * This file contains the main data structure and API definitions of MCS lock.
 *
 * The MCS lock (proposed by Mellor-Crummey and Scott) is a simple spin-lock
 * with the desirable properties of being fair, and with each cpu trying
 * to acquire the lock spinning on a local variable.
 * It avoids expensive cache bounces that common test-and-set spin-lock
 * implementations incur.
 */
/*
 * MCS 锁定义
 *
 * 本文件给出 MCS 锁的主要数据结构使用约定和 API 定义。
 *
 * MCS 锁由 Mellor-Crummey 和 Scott 提出，是一种简单的排队自旋锁。等待者按
 * 入队顺序获得锁，因而具有公平性；每个 CPU 只轮询自己节点中的局部变量，
 * 避免普通 test-and-set 自旋锁让所有等待者反复争用同一缓存行所造成的昂贵
 * cacheline 抖动。队列尾指针仍是共享入口，但稳定等待阶段转移到了各自节点。
 */
#ifndef __LINUX_MCS_SPINLOCK_H
#define __LINUX_MCS_SPINLOCK_H

/*
 * 体系结构头提供 struct mcs_spinlock，并可在包含本文件前定义下方两个
 * arch_mcs_* 宏。include guard 保证同一编译单元只建立一份接口定义；这里
 * 不拥有节点存储，节点的分配、唯一使用和存活期都由调用者负责。
 */
#include <asm/mcs_spinlock.h>

#ifndef arch_mcs_spin_lock_contended
/*
 * Using smp_cond_load_acquire() provides the acquire semantics
 * required so that subsequent operations happen after the
 * lock is acquired. Additionally, some architectures such as
 * ARM64 would like to do spin-waiting instead of purely
 * spinning, and smp_cond_load_acquire() provides that behavior.
 */
/*
 * 使用 smp_cond_load_acquire() 提供 acquire 语义，保证成功取得锁之后的操作
 * 不会越过取得锁的时刻。除此之外，ARM64 等体系结构希望在等待时使用专门的
 * 自旋等待提示，而不是只做普通忙循环；smp_cond_load_acquire() 允许体系结构
 * 为这种等待选择合适实现。
 *
 * @l 指向当前等待节点的 locked 字段，调用期间必须有效。默认实现持续读取，
 * 直到值非零才返回；它不睡眠、不取得节点所有权，成功观察与解锁侧的 release
 * 写配对，使前任临界区的访问先于当前持有者随后的访问。
 */
#define arch_mcs_spin_lock_contended(l)					\
	smp_cond_load_acquire(l, VAL)
#endif

#ifndef arch_mcs_spin_unlock_contended
/*
 * smp_store_release() provides a memory barrier to ensure all
 * operations in the critical section has been completed before
 * unlocking.
 */
/*
 * smp_store_release() 提供内存屏障，保证临界区内的全部操作先于解锁发布完成。
 *
 * @l 指向后继节点的 locked 字段，调用期间必须有效。把它写为 1 会把锁直接
 * 交给该后继，并与其 acquire 等待配对；宏不回收节点，也不唤醒可睡眠任务，
 * 体系结构若需要事件指令等自旋唤醒机制，可以覆盖这个默认实现。
 */
#define arch_mcs_spin_unlock_contended(l)				\
	smp_store_release((l), 1)
#endif

/*
 * Note: the smp_load_acquire/smp_store_release pair is not
 * sufficient to form a full memory barrier across
 * cpus for many architectures (except x86) for mcs_unlock and mcs_lock.
 * For applications that need a full barrier across multiple cpus
 * with mcs_unlock and mcs_lock pair, smp_mb__after_unlock_lock() should be
 * used after mcs_lock.
 */
/*
 * 注意：在许多体系结构上（x86 除外），smp_load_acquire/
 * smp_store_release 配对不足以让跨 CPU 的 mcs_unlock 与 mcs_lock 组合形成
 * 完整内存屏障。需要该 unlock+lock 对在多个 CPU 之间提供全屏障的调用者，
 * 应当在 mcs_lock 之后调用 smp_mb__after_unlock_lock()。
 *
 * release/acquire 已足够保护本锁临界区的发布与消费；这里强调的是更强的全局
 * 排序需求。不能因为队列交接正确，就推断未参与该交接的 CPU 也会以同一顺序
 * 观察所有访问。
 */

/*
 * In order to acquire the lock, the caller should declare a local node and
 * pass a reference of the node to this function in addition to the lock.
 * If the lock has already been acquired, then this will proceed to spin
 * on this node->locked until the previous lock holder sets the node->locked
 * in mcs_spin_unlock().
 */
/*
 * 获取锁时，调用者除共享队尾 @lock 外，还应声明一个本地节点，并把节点引用
 * 传给本函数。如果锁已被持有，本函数就在 node->locked 上自旋，直到前任持有者
 * 在 mcs_spin_unlock() 中设置该字段。
 *
 * @lock: 指向共享 MCS 队尾指针的借用地址，非 NULL；所有竞争者必须使用同一地址。
 * @node: 本次获取专用且尚未入队的节点，非 NULL；从调用前一直存活到与之配对的
 *        mcs_spin_unlock() 完成，期间不得被另一获取尝试复用。
 *
 * 调用上下文必须允许非睡眠自旋。函数无返回值；返回即表示调用者持有锁。无前驱
 * 时直接进入临界区；有前驱时先把自身链接到前驱，再等待前驱以 release 方式交接。
 * 本函数不分配资源、不处理超时，也没有失败返回；节点及临界区退出责任仍归调用者。
 */
static inline
void mcs_spin_lock(struct mcs_spinlock **lock, struct mcs_spinlock *node)
{
	/* prev 保存 xchg() 返回的旧队尾；NULL 表示入队前队列为空。 */
	struct mcs_spinlock *prev;

	/* Init node */
	/* 初始化本次排队节点：尚未获锁，也尚无后继。 */
	node->locked = 0;
	node->next   = NULL;

	/*
	 * We rely on the full barrier with global transitivity implied by the
	 * below xchg() to order the initialization stores above against any
	 * observation of @node. And to provide the ACQUIRE ordering associated
	 * with a LOCK primitive.
	 */
	/*
	 * 这里依赖下方 xchg() 隐含的、具有全局传递性的完整屏障，把上面的节点
	 * 初始化写排在任何其他 CPU 对 @node 的观察之前；同一个 xchg() 还提供
	 * LOCK 原语所需的 ACQUIRE 排序。
	 *
	 * 原子替换把本节点发布为新队尾，并返回发布前的队尾。这样每个竞争者只会
	 * 得到自己的唯一前驱，形成 FIFO 链；它不等于已经拿到锁，是否需要等待由
	 * prev 是否为 NULL 决定。
	 */
	prev = xchg(lock, node);
	if (likely(prev == NULL)) {
		/*
		 * Lock acquired, don't need to set node->locked to 1. Threads
		 * only spin on its own node->locked value for lock acquisition.
		 * However, since this thread can immediately acquire the lock
		 * and does not proceed to spin on its own node->locked, this
		 * value won't be used. If a debug mode is needed to
		 * audit lock status, then set node->locked value here.
		 */
		/*
		 * 锁已取得，不必把 node->locked 设为 1。线程获取锁时只会在自己的
		 * node->locked 上自旋；本线程立即取得锁，没有进入这个等待过程，所以
		 * 该值不会被使用。若调试模式需要审计持锁状态，可以在这里设置它。
		 *
		 * 此时 @node 同时是队尾和当前持有者的节点；调用者必须把同一节点传给
		 * 解锁函数，不能因为没有等待就提前复用或释放。
		 */
		return;
	}
	/*
	 * 有前驱时发布前驱的 next 链接。解锁者可能已经看到队尾发生变化，却尚未
	 * 看到本次 WRITE_ONCE；这种短暂窗口由解锁慢分支轮询 node->next 来闭合。
	 */
	WRITE_ONCE(prev->next, node);

	/* Wait until the lock holder passes the lock down. */
	/* 等待当前持有者把锁向后交接；只轮询本地节点，返回后即可进入临界区。 */
	arch_mcs_spin_lock_contended(&node->locked);
}

/*
 * Releases the lock. The caller should pass in the corresponding node that
 * was used to acquire the lock.
 */
/*
 * 释放锁。调用者应传入获取该锁时使用的对应节点。
 *
 * @lock: 获取时使用的共享队尾地址，非 NULL，函数只借用。
 * @node: 当前持有者的排队节点，必须正是配对 mcs_spin_lock() 所用节点，并保持
 *        有效直到本函数完成；调用期间不得并发复用。
 *
 * 调用者进入时必须持锁，且上下文必须允许短暂非睡眠自旋。无可见后继时先尝试
 * 以 release cmpxchg 把自身从队尾摘除；若后继已替换队尾但尚未写好链接，则等待
 * next 发布；一旦有后继，就以 release 写其 locked 字段完成直接交接。函数无返回
 * 值和失败路径，不释放节点内存；返回后调用者才可按其外部存储策略复用节点。
 */
static inline
void mcs_spin_unlock(struct mcs_spinlock **lock, struct mcs_spinlock *node)
{
	/* next 是当前节点后继的并发快照；NULL 也可能只表示链接尚未发布。 */
	struct mcs_spinlock *next = READ_ONCE(node->next);

	if (likely(!next)) {
		/*
		 * Release the lock by setting it to NULL
		 */
		/*
		 * 通过把共享队尾设为 NULL 来释放锁。只有队尾仍等于 @node 时比较交换
		 * 才成功；release 语义保证临界区访问先于其他 CPU 随后的成功获取。
		 */
		if (likely(cmpxchg_release(lock, node, NULL) == node))
			return;
		/* Wait until the next pointer is set */
		/*
		 * 等待 next 指针被设置。cmpxchg 失败说明已有后继把共享队尾从 @node
		 * 换走，但该后继可能尚未来得及写 node->next；这里只等待这段入队窗口，
		 * READ_ONCE 防止编译器合并读取，cpu_relax() 为忙等循环提供体系结构提示。
		 */
		while (!(next = READ_ONCE(node->next)))
			cpu_relax();
	}

	/* Pass lock to next waiter. */
	/* 向下一等待者交接锁；release 写与其 acquire 等待配对后，本节点即可退出使用。 */
	arch_mcs_spin_unlock_contended(&next->locked);
}

#endif /* __LINUX_MCS_SPINLOCK_H */
