// SPDX-License-Identifier: GPL-2.0-only
/*
 * kernel/locking/mutex.c
 *
 * Mutexes: blocking mutual exclusion locks
 *
 * Started by Ingo Molnar:
 *
 *  Copyright (C) 2004, 2005, 2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *
 * Many thanks to Arjan van de Ven, Thomas Gleixner, Steven Rostedt and
 * David Howells for suggestions and improvements.
 *
 *  - Adaptive spinning for mutexes by Peter Zijlstra. (Ported to mainline
 *    from the -rt tree, where it was originally implemented for rtmutexes
 *    by Steven Rostedt, based on work by Gregory Haskins, Peter Morreale
 *    and Sven Dietrich.
 *
 * Also see Documentation/locking/mutex-design.rst.
 */
/*
 * 本文件实现会阻塞睡眠的互斥锁；设计背景可继续参阅
 * Documentation/locking/mutex-design.rst。普通路径以 owner 原子字保存持有者和协议标志，
 * 竞争路径再用等待队列、调度器状态与可选乐观自旋完成交接。
 * 自适应自旋最初用于 rtmutex，后来从 -rt 树移植到主线 mutex；这里保留上述作者和贡献说明，
 * 便于把算法沿革与当前实现对应起来。
 */
#include <linux/mutex.h>
#include <linux/ww_mutex.h>
#include <linux/sched/signal.h>
#include <linux/sched/rt.h>
#include <linux/sched/wake_q.h>
#include <linux/sched/debug.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/debug_locks.h>
#include <linux/osq_lock.h>
#include <linux/hung_task.h>

#define CREATE_TRACE_POINTS
#include <trace/events/lock.h>

#ifndef CONFIG_PREEMPT_RT
#include "mutex.h"

#ifdef CONFIG_DEBUG_MUTEXES
/* 调试构建把内部不变量失败交给 debug-locks；普通构建移除这些诊断，且空宏不求值 cond。 */
# define MUTEX_WARN_ON(cond) DEBUG_LOCKS_WARN_ON(cond)
#else
# define MUTEX_WARN_ON(cond)
#endif

/*
 * __mutex_init_generic - 初始化所有配置共享的 mutex 运行时状态
 * @lock: 调用者提供且尚未发布的 mutex；函数借用该对象，不取得其存储所有权
 *
 * 调用者必须保证没有并发访问。函数清空 owner，在新建的 wait_lock 保护下发布空队首，按配置
 * 初始化 OSQ，最后调用调试 hook；无返回值和失败分支，返回后还需由公开初始化层完成 lockdep
 * 等配置相关状态，再由调用者把锁发布给其他任务。
 */
static void __mutex_init_generic(struct mutex *lock)
{
	atomic_long_set(&lock->owner, 0);
	scoped_guard (raw_spinlock_init, &lock->wait_lock) {
		lock->first_waiter = NULL;
	}
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
	osq_lock_init(&lock->osq);
#endif
	debug_mutex_init(lock);
}

/*
 * __owner_task - 从 owner 组合字中解出任务地址
 * @owner: 原子 owner 的一次值快照，低位可以携带 MUTEX_FLAGS
 *
 * 返回去掉协议位后的借用地址；它不增加 task 引用，离开外部并发保护后可能立即失效，调用者只能
 * 将它用于受约束的比较或推测，不能据此延长任务生命周期。
 */
static inline struct task_struct *__owner_task(unsigned long owner)
{
	return (struct task_struct *)(owner & ~MUTEX_FLAGS);
}

/*
 * mutex_is_locked - 查询 mutex 快照中是否存在持有任务
 * @lock: 已初始化且在查询期间仍存活的 mutex
 *
 * 可在无需取得该锁的观测路径调用，返回 true 只表示读取瞬间 owner 含非空 task；结果可能立即
 * 因并发获取或释放而过期，不能充当同步、所有权证明或后续解引用依据。
 */
bool mutex_is_locked(struct mutex *lock)
{
	return __mutex_owner(lock) != NULL;
}
EXPORT_SYMBOL(mutex_is_locked);

/*
 * __owner_flags - 取得 owner 快照中的 mutex 协议位
 * @owner: 同一次原子读取或 cmpxchg 失败回填得到的组合字
 *
 * 返回 WAITERS/HANDOFF/PICKUP 的子集，不检查 task 部分；调用者负责在自己的原子更新循环中解释
 * 各位并保留仍然有效的并发状态。
 */
static inline unsigned long __owner_flags(unsigned long owner)
{
	return owner & MUTEX_FLAGS;
}

/* Do not use the return value as a pointer directly. */
/* 返回值不得直接当指针使用。 */
/*
 * mutex_get_owner - 导出 owner 中任务地址的无引用数值快照
 * @lock: 已初始化且在本次原子读取期间保持存活的 mutex
 *
 * 返回去掉协议位后的地址数值，未持有时为 0。返回类型刻意是 unsigned long：任务可在读取后退出，
 * 因而调用者只能用于诊断、比较或哈希，不能转换后解引用；函数不提供锁语义，也不取得 task 引用。
 */
unsigned long mutex_get_owner(struct mutex *lock)
{
	unsigned long owner = atomic_long_read(&lock->owner);

	return (unsigned long)__owner_task(owner);
}

/*
 * Returns: __mutex_owner(lock) on failure or NULL on success.
 */
/*
 * 失败返回当次观察到的持有任务，成功返回 NULL。
 *
 * __mutex_trylock_common - 原子获取 mutex，或为队首请求定向交接
 * @lock: 已初始化且由调用者保证存活的 mutex
 * @handoff: true 表示锁已被占用时尝试置 HANDOFF；false 表示只尝试取得所有权
 *
 * 可从抢占已关闭的快/慢路径调用，不操作等待队列。循环同时处理三类状态：无 owner 时把
 * current 写入；PICKUP 时仅指定 current 能清位并接收锁；handoff 请求时在现 owner 上置位。
 * acquire cmpxchg 成功且 task 为 current 时返回 NULL；否则返回不带引用的 owner 快照。失败只表示
 * 本轮未取得锁，调用者必须按自己的队列、睡眠或自旋路径继续，不能解引用返回任务。
 */
static inline struct task_struct *__mutex_trylock_common(struct mutex *lock, bool handoff)
{
	unsigned long owner, curr = (unsigned long)current;

	owner = atomic_long_read(&lock->owner);
	for (;;) { /* must loop, can race against a flag */
		/* 必须循环：并发方即使不更换 task，也可能单独改变低位协议标志。 */
		unsigned long flags = __owner_flags(owner);
		unsigned long task = owner & ~MUTEX_FLAGS;

		if (task) {
			if (flags & MUTEX_FLAG_PICKUP) {
				if (task != curr)
					break;
				flags &= ~MUTEX_FLAG_PICKUP;
			} else if (handoff) {
				if (flags & MUTEX_FLAG_HANDOFF)
					break;
				flags |= MUTEX_FLAG_HANDOFF;
			} else {
				break;
			}
		} else {
			MUTEX_WARN_ON(flags & (MUTEX_FLAG_HANDOFF | MUTEX_FLAG_PICKUP));
			task = curr;
		}

		if (atomic_long_try_cmpxchg_acquire(&lock->owner, &owner, task | flags)) {
			if (task == curr)
				return NULL;
			break;
		}
	}

	return __owner_task(owner);
}

/*
 * Trylock or set HANDOFF
 */
/* 尝试获取；若 @handoff 为真且已有 owner，则改为请求持有者向队首定向交接。 */
/*
 * __mutex_trylock_or_handoff - 把公共 try/handoff 结果转换为布尔获取结果
 * @lock: 已初始化且保持存活的 mutex
 * @handoff: 是否允许在竞争时请求 HANDOFF
 *
 * 返回 true 表示 current 已取得或拾取锁，false 表示仍需等待；不改变等待队列，也不取得额外引用。
 */
static inline bool __mutex_trylock_or_handoff(struct mutex *lock, bool handoff)
{
	return !__mutex_trylock_common(lock, handoff);
}

/*
 * Actual trylock that will work on any unlocked state.
 */
/* 完整 trylock 能处理所有未持有状态，并能完成已定向给 current 的 PICKUP。 */
/*
 * __mutex_trylock - 尝试取得任意可获取的 mutex owner 状态
 * @lock: 已初始化且保持存活的 mutex
 *
 * 返回 true 表示 current 已拥有锁，false 表示当前仍有其他 owner 或交接目标；与仅接受严格零值的
 * 快速版本不同，本函数会保留 WAITERS 并处理 PICKUP，因而是快速失败后的权威重试。
 */
static inline bool __mutex_trylock(struct mutex *lock)
{
	return !__mutex_trylock_common(lock, false);
}

#ifndef CONFIG_DEBUG_LOCK_ALLOC
/*
 * Lockdep annotations are contained to the slow paths for simplicity.
 * There is nothing that would stop spreading the lockdep annotations outwards
 * except more code.
 */
/* 为简化快速路径，lockdep 注解集中在慢路径；把它外移并无语义障碍，只会增加代码。 */
/*
 * mutex_init_generic - 非 lockdep 构建的公开 mutex 初始化入口
 * @lock: 调用者拥有、尚未并发发布的 mutex 存储
 *
 * 仅委托共享初始化器，不分配资源且无失败返回；调用者必须在首次使用前调用，并保证锁仍被持有时
 * 不重新初始化或释放其内存。该配置没有额外的 lock class 初始化。
 */
void mutex_init_generic(struct mutex *lock)
{
	__mutex_init_generic(lock);
}
EXPORT_SYMBOL(mutex_init_generic);

/*
 * Optimistic trylock that only works in the uncontended case. Make sure to
 * follow with a __mutex_trylock() before failing.
 */
/*
 * 该乐观快速尝试只接受完全无竞争的零 owner；返回失败前必须再调用 __mutex_trylock()，以免漏掉
 * 带 WAITERS 的可获取状态或已经交给 current 的 PICKUP。
 *
 * __mutex_trylock_fast - 以单次 acquire cmpxchg 获取严格零状态
 * @lock: 已初始化且保持存活的 mutex
 *
 * 成功把 current 写为 owner 并返回 true；任何 task 或标志位存在都返回 false。它不碰等待队列、
 * 不做 lockdep acquire，调用者必须在失败后进入完整路径，并在成功后承担配对解锁责任。
 */
static __always_inline bool __mutex_trylock_fast(struct mutex *lock)
	__cond_acquires(true, lock)
{
	unsigned long curr = (unsigned long)current;
	unsigned long zero = 0UL;

	MUTEX_WARN_ON(lock->magic != lock);

	if (atomic_long_try_cmpxchg_acquire(&lock->owner, &zero, curr))
		return true;

	return false;
}

/*
 * __mutex_unlock_fast - 仅在 owner 严格等于 current 时快速释放
 * @lock: 必须由 current 持有且在函数返回前保持存活的 mutex
 *
 * release cmpxchg 成功把无标志 owner 清零并返回 true；若存在 WAITERS/HANDOFF/PICKUP、owner
 * 不匹配或并发变化则返回 false，调用者必须进入慢解锁。函数不做 lockdep release、不唤醒 waiter；
 * 成功后调用者已不再拥有锁，并须确保对象生命期满足公开 mutex_unlock() 的要求。
 */
static __always_inline bool __mutex_unlock_fast(struct mutex *lock)
	__cond_releases(true, lock)
{
	unsigned long curr = (unsigned long)current;

	return atomic_long_try_cmpxchg_release(&lock->owner, &curr, 0UL);
}

#else /* !CONFIG_DEBUG_LOCK_ALLOC */

/*
 * mutex_init_lockdep - 初始化 mutex 本体及可睡眠 lockdep map
 * @lock: 调用者拥有、尚未并发发布的 mutex 存储
 * @name: lock class 的借用名称，生命周期必须满足 lockdep 的登记要求
 * @key: 调用者提供的稳定 lock class key
 *
 * 先初始化运行时字段，再诊断“仍被持有的对象被重初始化”，最后以 LD_WAIT_SLEEP 登记依赖图。
 * 无失败返回和动态分配；调用者返回后才能发布锁，并必须保证 name、key 和 lock 的合法生命周期。
 */
void mutex_init_lockdep(struct mutex *lock, const char *name, struct lock_class_key *key)
{
	__mutex_init_generic(lock);

	/*
	 * Make sure we are not reinitializing a held lock:
	 */
	/* 确保没有正在把一把仍被持有的锁重新初始化。 */
	debug_check_no_locks_freed((void *)lock, sizeof(*lock));
	lockdep_init_map_wait(&lock->dep_map, name, key, 0, LD_WAIT_SLEEP);
}
EXPORT_SYMBOL(mutex_init_lockdep);
#endif /* !CONFIG_DEBUG_LOCK_ALLOC */

/*
 * __mutex_set_flag - 原子置位 owner 协议标志
 * @lock: 保持存活的 mutex
 * @flag: MUTEX_FLAGS 内需要置位的位掩码
 *
 * 保留 owner task 和其他并发标志，无返回值；调用者必须满足相应协议前置条件，例如首个 waiter
 * 发布 WAITERS。该原子 RMW 本身不负责 wait_lock 串行化或所有权转移。
 */
static inline void __mutex_set_flag(struct mutex *lock, unsigned long flag)
{
	atomic_long_or(flag, &lock->owner);
}

/*
 * __mutex_clear_flag - 原子清除 owner 协议标志
 * @lock: 保持存活的 mutex
 * @flag: MUTEX_FLAGS 内需要清除的位掩码
 *
 * 保留 owner task 和未指定标志，无返回值；调用者负责证明清位不会破坏并发 handoff/pickup，通常
 * 依靠 wait_lock 或当前协议状态。函数不修改等待链表。
 */
static inline void __mutex_clear_flag(struct mutex *lock, unsigned long flag)
{
	atomic_long_andnot(flag, &lock->owner);
}

/*
 * Add @waiter to the @lock wait_list and set the FLAG_WAITERS flag if it's
 * the first waiter.
 *
 * When @pos, @waiter is added before the waiter indicated by @pos. Otherwise
 * @waiter will be added to the tail of the list.
 */
/*
 * 把 @waiter 加入 @lock 的环形等待链；若它成为首个 waiter，则同时置 WAITERS。
 * @pos 非空时插到该 waiter 之前，否则追加到队尾。
 *
 * __mutex_add_waiter - 在 wait_lock 下发布一个栈上 mutex waiter
 * @lock: 已初始化、由调用者持有 wait_lock 的 mutex
 * @waiter: 当前慢路径栈帧拥有且已填好 task/ww_ctx 的节点，必须存活到退队
 * @pos: 可选的现有队列节点；非空时必须仍属于 @lock，函数在其前插入
 *
 * 函数登记 hung-task/debug blocker；有 @pos 时可能更新队首，无 @pos 时按 FIFO 追加，空队列则
 * 自环初始化节点、发布 first_waiter 并原子置 WAITERS。无返回值；返回后调用者仍持 wait_lock，
 * 且必须最终通过 __mutex_remove_waiter() 撤销该借用节点。
 */
static void
__mutex_add_waiter(struct mutex *lock, struct mutex_waiter *waiter,
		   struct mutex_waiter *pos)
	__must_hold(&lock->wait_lock)
{
	struct mutex_waiter *first = lock->first_waiter;

	hung_task_set_blocker(lock, BLOCKER_TYPE_MUTEX);
	debug_mutex_add_waiter(lock, waiter, current);

	if (pos) {
		/*
		 * Insert @waiter before @pos.
		 */
		/* 在 @pos 前插入 @waiter。 */
		list_add_tail(&waiter->list, &pos->list);
		/*
		 * If @pos == @first, then @waiter will be the new first.
		 */
		/* 若 @pos 原为队首，插入节点随之成为新队首。 */
		if (pos == first)
			lock->first_waiter = waiter;
		return;
	}

	if (first) {
		list_add_tail(&waiter->list, &first->list);
		return;
	}

	INIT_LIST_HEAD(&waiter->list);
	lock->first_waiter = waiter;
	__mutex_set_flag(lock, MUTEX_FLAG_WAITERS);
}

/*
 * __mutex_remove_waiter - 在 wait_lock 下撤销一个已发布的栈上 waiter
 * @lock: 调用者持有 wait_lock、且包含 @waiter 的 mutex
 * @waiter: 当前慢路径栈帧拥有的队列节点
 *
 * 若节点自环，它是最后一个 waiter，函数清除全部协议位并置空 first_waiter；否则必要时推进队首
 * 再摘链。最后撤销 debug 与 hung-task 记录。无返回值，返回时调用者仍持 wait_lock，节点已可在
 * 栈展开时失效；调用者仍负责恢复任务状态及处理实际锁所有权。
 */
static void
__mutex_remove_waiter(struct mutex *lock, struct mutex_waiter *waiter)
	__must_hold(&lock->wait_lock)
{
	if (list_empty(&waiter->list)) {
		__mutex_clear_flag(lock, MUTEX_FLAGS);
		lock->first_waiter = NULL;
	} else {
		if (lock->first_waiter == waiter)
			lock->first_waiter = list_next_entry(waiter, list);
		list_del(&waiter->list);
	}

	debug_mutex_remove_waiter(lock, waiter, current);
	hung_task_clear_blocker();
}

/*
 * Give up ownership to a specific task, when @task = NULL, this is equivalent
 * to a regular unlock. Sets PICKUP on a handoff, clears HANDOFF, preserves
 * WAITERS. Provides RELEASE semantics like a regular unlock, the
 * __mutex_trylock() provides a matching ACQUIRE semantics for the handoff.
 */
/*
 * 把所有权交给指定任务；@task 为 NULL 等价于普通释放。定向交接置 PICKUP、清 HANDOFF 并保留
 * WAITERS。这里的 release 与接收者 __mutex_trylock() 的 acquire 配对。
 *
 * __mutex_handoff - 由 current 原子释放或把 mutex 定向交给目标任务
 * @lock: 必须由 current 持有且保持存活的 mutex
 * @task: 已由等待路径保证存活的目标任务；NULL 表示只清除 owner
 *
 * cmpxchg 循环保留并发 WAITERS，拒绝非 current owner 或既有 PICKUP，并以 release 写入
 * task+PICKUP 或零 task。无返回值；目标非空时它尚未真正执行，随后仍需被唤醒并通过 trylock
 * 清 PICKUP，调用者不得把交接完成误作目标已运行。
 */
static void __mutex_handoff(struct mutex *lock, struct task_struct *task)
{
	unsigned long owner = atomic_long_read(&lock->owner);

	for (;;) {
		unsigned long new;

		MUTEX_WARN_ON(__owner_task(owner) != current);
		MUTEX_WARN_ON(owner & MUTEX_FLAG_PICKUP);

		new = (owner & MUTEX_FLAG_WAITERS);
		new |= (unsigned long)task;
		if (task)
			new |= MUTEX_FLAG_PICKUP;

		if (atomic_long_try_cmpxchg_release(&lock->owner, &owner, new))
			break;
	}
}

#ifndef CONFIG_DEBUG_LOCK_ALLOC
/*
 * We split the mutex lock/unlock logic into separate fastpath and
 * slowpath functions, to reduce the register pressure on the fastpath.
 * We also put the fastpath first in the kernel image, to make sure the
 * branch is predicted by the CPU as default-untaken.
 */
/*
 * 获取/释放被拆为快速与慢速函数以降低快速路径寄存器压力；快速路径还排在内核镜像前面，使进入
 * 慢路径的分支通常按“不跳转”预测。下面的声明是实现顺序需要的借用接口，不新增独立生命周期。
 */
static void __sched __mutex_lock_slowpath(struct mutex *lock)
	__acquires(lock);

/**
 * mutex_lock - acquire the mutex
 * @lock: the mutex to be acquired
 *
 * Lock the mutex exclusively for this task. If the mutex is not
 * available right now, it will sleep until it can get it.
 *
 * The mutex must later on be released by the same task that
 * acquired it. Recursive locking is not allowed. The task
 * may not exit without first unlocking the mutex. Also, kernel
 * memory where the mutex resides must not be freed with
 * the mutex still locked. The mutex must first be initialized
 * (or statically defined) before it can be locked. memset()-ing
 * the mutex to 0 is not allowed.
 *
 * (The CONFIG_DEBUG_MUTEXES .config option turns on debugging
 * checks that will enforce the restrictions and will also do
 * deadlock debugging)
 *
 * This function is similar to (but not equivalent to) down().
 */
/*
 * mutex_lock - 不可中断地独占获取 mutex
 * @lock: 已初始化或静态定义、并在获取与持有期间保持存活的 mutex
 *
 * 只能在可睡眠的任务上下文调用。严格零 owner 时走 acquire 快路径，否则进入慢路径直至成功；
 * 不返回错误。锁不可递归，必须由同一任务用 mutex_unlock() 释放，任务不得持锁退出，对象也不得
 * 持锁释放或用 memset 清零。DEBUG_MUTEXES 会加强这些限制及死锁诊断；语义类似但不等同信号量
 * down()。返回时 current 持锁并承担完整配对释放责任。
 */
void __sched mutex_lock(struct mutex *lock)
{
	might_sleep();

	if (!__mutex_trylock_fast(lock))
		__mutex_lock_slowpath(lock);
}
EXPORT_SYMBOL(mutex_lock);
#endif

#include "ww_mutex.h"

#ifdef CONFIG_MUTEX_SPIN_ON_OWNER

/*
 * Trylock variant that returns the owning task on failure.
 */
/* 这是失败时返回 owner 快照的 trylock 变体。 */
/*
 * __mutex_trylock_or_owner - 乐观自旋使用的完整 trylock
 * @lock: 已初始化且保持存活的 mutex
 *
 * 成功返回 NULL 且 current 获得锁；失败返回无引用、不可直接解引用的 owner 快照，供外层在抢占
 * 关闭的推测窗口比较。它不入队，调用者负责决定继续自旋还是转入睡眠路径。
 */
static inline struct task_struct *__mutex_trylock_or_owner(struct mutex *lock)
{
	return __mutex_trylock_common(lock, false);
}

/*
 * ww_mutex_spin_on_owner - 判断 ww 竞争者本轮是否仍可安全乐观自旋
 * @lock: 嵌入 ww_mutex 的 base mutex，调用期间保持存活
 * @ww_ctx: current 的有效 acquire context，非空且由调用者保持
 * @waiter: 已入队时为 current 的栈 waiter，尚未入队时为 NULL
 *
 * 不取得 wait_lock，因此只把 ww->ctx 和 first_waiter 当竞争快照。需要检查已持锁上下文时、未入队
 * 却已有 waiter 时、或入队后不再位于队首时返回 false，防止越过更早 stamp 并破坏死锁协议；
 * 其余返回 true。无所有权变化，外层每轮都须重查。
 */
static inline
bool ww_mutex_spin_on_owner(struct mutex *lock, struct ww_acquire_ctx *ww_ctx,
			    struct mutex_waiter *waiter)
{
	struct ww_mutex *ww;

	ww = container_of(lock, struct ww_mutex, base);

	/*
	 * If ww->ctx is set the contents are undefined, only
	 * by acquiring wait_lock there is a guarantee that
	 * they are not invalid when reading.
	 *
	 * As such, when deadlock detection needs to be
	 * performed the optimistic spinning cannot be done.
	 *
	 * Check this in every inner iteration because we may
	 * be racing against another thread's ww_mutex_lock.
	 */
	/*
	 * 未持 wait_lock 时 ww->ctx 内容不稳定；只有取锁后才保证读取有效。因此一旦需要死锁检测就不能
	 * 乐观自旋，并且必须在内层每轮重查，以覆盖与另一线程 ww_mutex_lock() 的竞态。
	 */
	if (ww_ctx->acquired > 0 && READ_ONCE(ww->ctx))
		return false;

	/*
	 * If we aren't on the wait list yet, cancel the spin
	 * if there are waiters. We want  to avoid stealing the
	 * lock from a waiter with an earlier stamp, since the
	 * other thread may already own a lock that we also
	 * need.
	 */
	/*
	 * 尚未入队而队中已有 waiter 时停止自旋，避免从 stamp 更早的等待者前偷锁；对方可能已持有
	 * current 后续也需要的另一把锁。
	 */
	if (!waiter && (atomic_long_read(&lock->owner) & MUTEX_FLAG_WAITERS))
		return false;

	/*
	 * Similarly, stop spinning if we are no longer the
	 * first waiter.
	 */
	/* 同理，已入队后若不再是首 waiter，也必须停止自旋。 */
	if (waiter && data_race(lock->first_waiter != waiter))
		return false;

	return true;
}

/*
 * Look out! "owner" is an entirely speculative pointer access and not
 * reliable.
 *
 * "noinline" so that this function shows up on perf profiles.
 */
/*
 * 注意：@owner 完全是推测性地址，不可靠；使用 noinline 是为了让该阶段单独出现在 perf 剖面。
 *
 * mutex_spin_on_owner - 在同一推测 owner 仍运行时自旋等待其状态改变
 * @lock: 保持存活的 mutex
 * @owner: 从 owner 原子字得到、未持 task 引用的推测快照
 * @ww_ctx: 可选的 current ww acquire context
 * @waiter: 可选的 current 已入队栈 waiter
 *
 * 调用者必须已关闭抢占，使该窗口同时成为 RCU 读侧临界区。每轮先确认 lock 仍指向同一 owner，
 * 再以编译器屏障约束 owner 解引用；owner 下 CPU、需要调度或 ww 规则禁止时返回 false，owner 改变
 * 则返回 true 让外层重试。函数不获取 mutex、不取得 task 引用，返回只决定下一阶段。
 */
static noinline
bool mutex_spin_on_owner(struct mutex *lock, struct task_struct *owner,
			 struct ww_acquire_ctx *ww_ctx, struct mutex_waiter *waiter)
{
	bool ret = true;

	lockdep_assert_preemption_disabled();

	while (__mutex_owner(lock) == owner) {
		/*
		 * Ensure we emit the owner->on_cpu, dereference _after_
		 * checking lock->owner still matches owner. And we already
		 * disabled preemption which is equal to the RCU read-side
		 * crital section in optimistic spinning code. Thus the
		 * task_strcut structure won't go away during the spinning
		 * period
		 */
		/*
		 * 必须先确认 lock->owner 仍匹配，再让编译器发出 owner->on_cpu 解引用。抢占关闭等价于此处
		 * 乐观自旋使用的 RCU 读侧临界区，所以该 task_struct 在自旋窗口内不会消失。
		 */
		barrier();

		/*
		 * Use vcpu_is_preempted to detect lock holder preemption issue.
		 */
		/* owner_on_cpu() 会结合 vcpu_is_preempted() 识别持锁 vCPU 已被抢占的情况。 */
		if (!owner_on_cpu(owner) || need_resched()) {
			ret = false;
			break;
		}

		if (ww_ctx && !ww_mutex_spin_on_owner(lock, ww_ctx, waiter)) {
			ret = false;
			break;
		}

		cpu_relax();
	}

	return ret;
}

/*
 * Initial check for entering the mutex spinning loop
 */
/* 这是进入 mutex 自旋循环前的廉价筛选。 */
/*
 * mutex_can_spin_on_owner - 判断首次竞争者是否值得进入 OSQ 自旋路径
 * @lock: 保持存活的 mutex
 *
 * 调用者必须关闭抢占。需要调度时返回 0；有 owner 时返回其是否仍在 CPU 上；owner 已清空则返回
 * 1，让外层优先 trylock 而不是直接睡眠。owner 地址只在本 RCU 等价窗口内推测使用，结果是瞬时
 * 启发式判断，不保证下一条指令时状态仍相同。
 */
static inline int mutex_can_spin_on_owner(struct mutex *lock)
{
	struct task_struct *owner;
	int retval = 1;

	lockdep_assert_preemption_disabled();

	if (need_resched())
		return 0;

	/*
	 * We already disabled preemption which is equal to the RCU read-side
	 * crital section in optimistic spinning code. Thus the task_strcut
	 * structure won't go away during the spinning period.
	 */
	/*
	 * 抢占已经关闭，这等价于乐观自旋所需的 RCU 读侧临界区，因此 task_struct 在窗口内不会消失。
	 */
	owner = __mutex_owner(lock);
	if (owner)
		retval = owner_on_cpu(owner);

	/*
	 * If lock->owner is not set, the mutex has been released. Return true
	 * such that we'll trylock in the spin path, which is a faster option
	 * than the blocking slow path.
	 */
	/* owner 未设置说明锁已释放；返回真以便自旋路径先 trylock，这通常比直接进入阻塞慢路径更快。 */
	return retval;
}

/*
 * Optimistic spinning.
 *
 * We try to spin for acquisition when we find that the lock owner
 * is currently running on a (different) CPU and while we don't
 * need to reschedule. The rationale is that if the lock owner is
 * running, it is likely to release the lock soon.
 *
 * The mutex spinners are queued up using MCS lock so that only one
 * spinner can compete for the mutex. However, if mutex spinning isn't
 * going to happen, there is no point in going through the lock/unlock
 * overhead.
 *
 * Returns true when the lock was taken, otherwise false, indicating
 * that we need to jump to the slowpath and sleep.
 *
 * The waiter flag is set to true if the spinner is a waiter in the wait
 * queue. The waiter-spinner will spin on the lock directly and concurrently
 * with the spinner at the head of the OSQ, if present, until the owner is
 * changed to itself.
 */
/*
 * 乐观自旋只在 owner 正运行且 current 无需调度时尝试，因为持锁者可能很快释放。尚未入队的
 * spinner 先用 MCS 风格 OSQ 串行化，避免所有 CPU 同时争 owner；若一开始就不适合自旋则跳过
 * OSQ 成本。返回 true 表示已取得锁，false 表示应进入睡眠慢路径。已入队的 waiter-spinner 不取
 * OSQ，可与 OSQ 队首同时直接观察锁，直到 owner 指向自己。
 *
 * mutex_optimistic_spin - 在关闭抢占期间尝试以自旋代替睡眠
 * @lock: 已初始化且保持存活的 mutex
 * @ww_ctx: 可选的 current ww acquire context
 * @waiter: current 已入队的栈 waiter；NULL 表示首次到达的非 waiter spinner
 *
 * 非 waiter 先筛选并取得 OSQ，随后循环 trylock、观察 owner 和 ww 约束；所有非 waiter 出口都
 * 释放 OSQ。成功返回 true 且 current 持锁；失败若因 need_resched 会先强制 TASK_RUNNING 并在抢占
 * 关闭形式下调度，避免刚获得 mutex 就被切走，最终返回 false。函数不拥有参数存储。
 */
static __always_inline bool
mutex_optimistic_spin(struct mutex *lock, struct ww_acquire_ctx *ww_ctx,
		      struct mutex_waiter *waiter)
{
	if (!waiter) {
		/*
		 * The purpose of the mutex_can_spin_on_owner() function is
		 * to eliminate the overhead of osq_lock() and osq_unlock()
		 * in case spinning isn't possible. As a waiter-spinner
		 * is not going to take OSQ lock anyway, there is no need
		 * to call mutex_can_spin_on_owner().
		 */
		/*
		 * 首次筛选用于避免不可能自旋时仍付出 OSQ 加解锁成本；waiter-spinner 本就不取 OSQ，所以
		 * 无需调用该筛选。
		 */
		if (!mutex_can_spin_on_owner(lock))
			goto fail;

		/*
		 * In order to avoid a stampede of mutex spinners trying to
		 * acquire the mutex all at once, the spinners need to take a
		 * MCS (queued) lock first before spinning on the owner field.
		 */
		/* 用 MCS 排队锁把首次 spinner 串行化，避免它们同时冲击 mutex owner。 */
		if (!osq_lock(&lock->osq))
			goto fail;
	}

	for (;;) {
		struct task_struct *owner;

		/* Try to acquire the mutex... */
		/* 先尝试取得 mutex。 */
		owner = __mutex_trylock_or_owner(lock);
		if (!owner)
			break;

		/*
		 * There's an owner, wait for it to either
		 * release the lock or go to sleep.
		 */
		/* owner 存在时，等它释放 mutex 或离开 CPU。 */
		if (!mutex_spin_on_owner(lock, owner, ww_ctx, waiter))
			goto fail_unlock;

		/*
		 * The cpu_relax() call is a compiler barrier which forces
		 * everything in this loop to be re-loaded. We don't need
		 * memory barriers as we'll eventually observe the right
		 * values at the cost of a few extra spins.
		 */
		/*
		 * cpu_relax() 同时充当编译器屏障，迫使循环重新加载；无需额外内存屏障，因为允许多转几圈后
		 * 再观察到正确值，真正获取仍由 acquire 原子操作提供排序。
		 */
		cpu_relax();
	}

	if (!waiter)
		osq_unlock(&lock->osq);

	return true;


fail_unlock:
	if (!waiter)
		osq_unlock(&lock->osq);

fail:
	/*
	 * If we fell out of the spin path because of need_resched(),
	 * reschedule now, before we try-lock the mutex. This avoids getting
	 * scheduled out right after we obtained the mutex.
	 */
	/*
	 * 若因 need_resched 离开自旋，先调度再重试锁，避免刚取得 mutex 就立即被切出。
	 */
	if (need_resched()) {
		/*
		 * We _should_ have TASK_RUNNING here, but just in case
		 * we do not, make it so, otherwise we might get stuck.
		 */
		/* 理论上此处已是 TASK_RUNNING；仍显式修正，以免异常状态让任务无法再次被选择运行。 */
		__set_current_state(TASK_RUNNING);
		schedule_preempt_disabled();
	}

	return false;
}
#else
/*
 * mutex_optimistic_spin - 未启用 owner 自旋时的统一空实现
 * @lock: 未使用；调用者仍须保证调用表达式本身合法
 * @ww_ctx: 未使用的可选 ww 上下文
 * @waiter: 未使用的可选 waiter
 *
 * 始终返回 false，使调用者直接进入阻塞慢路径；作为真实函数，三个实参仍会按 C 规则求值。
 */
static __always_inline bool
mutex_optimistic_spin(struct mutex *lock, struct ww_acquire_ctx *ww_ctx,
		      struct mutex_waiter *waiter)
{
	return false;
}
#endif

/*
 * __mutex_unlock_slowpath - 慢解锁实现的前置声明
 * @lock: current 持有且至少存活到实现返回的 mutex
 * @ip: 归因给 lockdep 的调用地址
 *
 * 实现会执行 release、处理 waiter 或定向交接；声明上的 __releases 告知静态分析返回时不再持锁。
 */
static noinline void __sched __mutex_unlock_slowpath(struct mutex *lock, unsigned long ip)
	__releases(lock);

/**
 * mutex_unlock - release the mutex
 * @lock: the mutex to be released
 *
 * Unlock a mutex that has been locked by this task previously.
 *
 * This function must not be used in interrupt context. Unlocking
 * of a not locked mutex is not allowed.
 *
 * The caller must ensure that the mutex stays alive until this function has
 * returned - mutex_unlock() can NOT directly be used to release an object such
 * that another concurrent task can free it.
 * Mutexes are different from spinlocks & refcounts in this aspect.
 *
 * This function is similar to (but not equivalent to) up().
 */
/*
 * mutex_unlock - 释放 current 先前取得的 mutex
 * @lock: current 持有、且必须一直存活到本函数返回的 mutex
 *
 * 不得在中断上下文调用，也不得释放未持有的锁。非 lockdep 构建先尝试严格 owner 的 release 快路；
 * 其余进入慢路径处理标志、waiter 和唤醒。返回时 current 不再持锁。与某些 spinlock/refcount 释放
 * 模式不同，不能借本调用让并发任务在函数返回前释放包含该 mutex 的对象；语义类似但不等同 up()。
 */
void __sched mutex_unlock(struct mutex *lock)
{
#ifndef CONFIG_DEBUG_LOCK_ALLOC
	if (__mutex_unlock_fast(lock))
		return;
#endif
	__mutex_unlock_slowpath(lock, _RET_IP_);
}
EXPORT_SYMBOL(mutex_unlock);

/**
 * ww_mutex_unlock - release the w/w mutex
 * @lock: the mutex to be released
 *
 * Unlock a mutex that has been locked by this task previously with any of the
 * ww_mutex_lock* functions (with or without an acquire context). It is
 * forbidden to release the locks after releasing the acquire context.
 *
 * This function must not be used in interrupt context. Unlocking
 * of a unlocked mutex is not allowed.
 */
/*
 * ww_mutex_unlock - 更新 ww 上下文后释放其 base mutex
 * @lock: current 通过任一 ww_mutex_lock* API 取得的 ww_mutex
 *
 * 可在获取时带或不带 acquire context，但必须在销毁相应 context 前解锁；不得在中断上下文或未
 * 持有时调用。函数先让 ww 层减少 acquired/清 ctx，再委托 mutex_unlock()，返回后对象仍由调用者
 * 拥有存储，而 current 不再持有该锁。
 */
void __sched ww_mutex_unlock(struct ww_mutex *lock)
	__no_context_analysis
{
	__ww_mutex_unlock(lock);
	mutex_unlock(&lock->base);
}
EXPORT_SYMBOL(ww_mutex_unlock);

/*
 * Lock a mutex (possibly interruptible), slowpath:
 */
/* 这是可按 @state 响应信号的共享 mutex 获取慢路径。 */
/*
 * __mutex_lock_common - 统一执行普通 mutex 与 ww_mutex 的竞争获取
 * @lock: 已初始化且在调用和成功持有期间保持存活的 base mutex
 * @state: 等待时写入 current 的 TASK_UNINTERRUPTIBLE/KILLABLE/INTERRUPTIBLE 状态
 * @subclass: lockdep 子类
 * @nest_lock: 可选的 lockdep 嵌套 map；ww 调试路径会改用 context dep_map
 * @ip: lockdep 与 contention trace 的调用地址
 * @ww_ctx: 可选的 ww acquire context
 * @use_ww_ctx: true 表示按 ww 语义初始化 waiter，即使 @ww_ctx 为 NULL
 *
 * 可睡眠任务上下文调用。阶段依次为：ww 前检；关闭抢占并登记 lockdep；完整 trylock/首次乐观
 * 自旋；取得 wait_lock 后重试；构造并发布栈 waiter；在 blocked_lock+wait_lock 协议下处理交接、
 * 信号、ww kill、睡眠和队首自旋；最后统一成功或错误清理。返回 0 时 current 持锁，返回
 * -EALREADY/-EINTR/-EDEADLK 等错误时不持锁。waiter 只在本栈帧内存活，wake_q 把受保护区内选出的
 * 唤醒延后到解锁时执行；所有出口恢复任务状态、释放自旋锁并重新启用抢占。
 */
static __always_inline int __sched
__mutex_lock_common(struct mutex *lock, unsigned int state, unsigned int subclass,
		    struct lockdep_map *nest_lock, unsigned long ip,
		    struct ww_acquire_ctx *ww_ctx, const bool use_ww_ctx)
	__cond_acquires(0, lock)
{
	DEFINE_WAKE_Q(wake_q);
	struct mutex_waiter waiter;
	struct ww_mutex *ww;
	unsigned long flags;
	int ret;
	/*
	 * wake_q 暂存退出 wait_lock 后才执行的唤醒；waiter 是本栈帧借给等待链的节点；ww 是由 base
	 * 反推的容器，仅在启用 ww 语义时访问；flags 保存 wait_lock 的本地 IRQ 状态；ret 贯穿信号与
	 * ww 取消错误出口，成功路径固定返回 0。
	 */

	if (!use_ww_ctx)
		ww_ctx = NULL;

	might_sleep();

	MUTEX_WARN_ON(lock->magic != lock);

	ww = container_of(lock, struct ww_mutex, base);
	if (ww_ctx) {
		if (unlikely(ww_ctx == READ_ONCE(ww->ctx)))
			return -EALREADY;

		/*
		 * Reset the wounded flag after a kill. No other process can
		 * race and wound us here since they can't have a valid owner
		 * pointer if we don't have any locks held.
		 */
		/*
		 * 被 kill 后且 acquired 为零时重置 wounded；此时其他任务不可能持有指向本 context 的有效
		 * owner，因此不会并发再次 wound 它。
		 */
		if (ww_ctx->acquired == 0)
			ww_ctx->wounded = 0;

#ifdef CONFIG_DEBUG_LOCK_ALLOC
		nest_lock = &ww_ctx->dep_map;
#endif
	}

	preempt_disable();
	mutex_acquire_nest(&lock->dep_map, subclass, 0, nest_lock, ip);

	trace_contention_begin(lock, LCB_F_MUTEX | LCB_F_SPIN);
	if (__mutex_trylock(lock) ||
	    mutex_optimistic_spin(lock, ww_ctx, NULL)) {
		/* got the lock, yay! */
		/* 已取得锁：完成 lockdep、ww context 和 contention trace 发布。 */
		lock_acquired(&lock->dep_map, ip);
		if (ww_ctx)
			ww_mutex_set_context_fastpath(ww, ww_ctx);
		trace_contention_end(lock, 0);
		preempt_enable();
		return 0;
	}

	raw_spin_lock_irqsave(&lock->wait_lock, flags);
	/*
	 * After waiting to acquire the wait_lock, try again.
	 */
	/* 等待取得 wait_lock 期间 owner 可能已释放，因此在入队前重新尝试。 */
	if (__mutex_trylock(lock)) {
		if (ww_ctx)
			__ww_mutex_check_waiters(lock, ww_ctx, &wake_q);

		goto skip_wait;
	}

	debug_mutex_lock_common(lock, &waiter);
	waiter.task = current;
	if (use_ww_ctx)
		waiter.ww_ctx = ww_ctx;

	lock_contended(&lock->dep_map, ip);

	if (!use_ww_ctx) {
		/* add waiting tasks to the end of the waitqueue (FIFO): */
		/* 普通等待任务追加到队尾，形成 FIFO。 */
		__mutex_add_waiter(lock, &waiter, NULL);
	} else {
		/*
		 * Add in stamp order, waking up waiters that must kill
		 * themselves.
		 */
		/* ww waiter 按 stamp 顺序插入，同时唤醒那些必须自行终止的等待者。 */
		ret = __ww_mutex_add_waiter(&waiter, lock, ww_ctx, &wake_q);
		if (ret)
			goto err_early_kill;
	}

	raw_spin_lock(&current->blocked_lock);
	__set_task_blocked_on(current, lock);
	set_current_state(state);
	trace_contention_begin(lock, LCB_F_MUTEX);
	for (;;) {
		bool first;
		/* first 是睡眠后对队首的竞争快照，决定是否请求 HANDOFF 以及能否进入 waiter 自旋。 */

		/*
		 * Once we hold wait_lock, we're serialized against
		 * mutex_unlock() handing the lock off to us, do a trylock
		 * before testing the error conditions to make sure we pick up
		 * the handoff.
		 */
		/*
		 * 持有 wait_lock 后与 mutex_unlock() 的交接串行化；必须先 trylock 再检查错误，确保已经定向
		 * 给 current 的 PICKUP 不会被信号取消路径漏掉。
		 */
		if (__mutex_trylock(lock))
			break;

		raw_spin_unlock(&current->blocked_lock);
		/*
		 * Check for signals and kill conditions while holding
		 * wait_lock. This ensures the lock cancellation is ordered
		 * against mutex_unlock() and wake-ups do not go missing.
		 */
		/* 在 wait_lock 下检查信号与 ww kill，使取消与 unlock 有序，避免丢失唤醒。 */
		if (signal_pending_state(state, current)) {
			ret = -EINTR;
			goto err;
		}

		if (ww_ctx) {
			ret = __ww_mutex_check_kill(lock, &waiter, ww_ctx);
			if (ret)
				goto err;
		}

		raw_spin_unlock_irqrestore_wake(&lock->wait_lock, flags, &wake_q);

		schedule_preempt_disabled();

		first = lock->first_waiter == &waiter;

		raw_spin_lock_irqsave(&lock->wait_lock, flags);
		raw_spin_lock(&current->blocked_lock);
		/*
		 * As we likely have been woken up by task
		 * that has cleared our blocked_on state, re-set
		 * it to the lock we are trying to acquire.
		 */
		/* 唤醒方很可能已清 blocked_on；重新把它登记为当前仍在获取的 mutex。 */
		__set_task_blocked_on(current, lock);
		set_current_state(state);
		/*
		 * Here we order against unlock; we must either see it change
		 * state back to RUNNING and fall through the next schedule(),
		 * or we must see its unlock and acquire.
		 */
		/*
		 * 这里与 unlock 排序：要么观察到它把任务改回 RUNNING，使下一次 schedule 直接穿过；要么
		 * 观察到释放并取得锁。两者至少发生其一，因而不会睡过唤醒。
		 */
		if (__mutex_trylock_or_handoff(lock, first))
			break;

		if (first) {
			bool opt_acquired;
			/* opt_acquired 只记录释放内部锁期间的自旋结果；返回后必须重新取得两把内部锁再解释。 */

			/*
			 * mutex_optimistic_spin() can call schedule(), so
			 * we need to release these locks before calling it,
			 * and clear blocked on so we don't become unselectable
			 * to run.
			 */
			/*
			 * 乐观自旋内部可能调度，调用前必须释放 blocked_lock 与 wait_lock，并清 blocked_on，避免
			 * 调度器把 current 当成不可选择运行的阻塞任务。
			 */
			__clear_task_blocked_on(current, lock);
			raw_spin_unlock(&current->blocked_lock);
			raw_spin_unlock_irqrestore(&lock->wait_lock, flags);

			trace_contention_begin(lock, LCB_F_MUTEX | LCB_F_SPIN);
			opt_acquired = mutex_optimistic_spin(lock, ww_ctx, &waiter);

			raw_spin_lock_irqsave(&lock->wait_lock, flags);
			raw_spin_lock(&current->blocked_lock);
			__set_task_blocked_on(current, lock);
			set_current_state(state);

			if (opt_acquired)
				break;
			trace_contention_begin(lock, LCB_F_MUTEX);
		}
	}
	__clear_task_blocked_on(current, lock);
	__set_current_state(TASK_RUNNING);
	raw_spin_unlock(&current->blocked_lock);

	if (ww_ctx) {
		/*
		 * Wound-Wait; we stole the lock (!first_waiter), check the
		 * waiters as anyone might want to wound us.
		 */
		/* Wound-Wait 下若不是从队首取得锁，必须检查其他 waiter，因为其中任何一个都可能要 wound 当前。 */
		if (!ww_ctx->is_wait_die && lock->first_waiter != &waiter)
			__ww_mutex_check_waiters(lock, ww_ctx, &wake_q);
	}

	__mutex_remove_waiter(lock, &waiter);

	debug_mutex_free_waiter(&waiter);

skip_wait:
	/* got the lock - cleanup and rejoice! */
	/* 已取得锁：完成通用清理并发布成功观测。 */
	lock_acquired(&lock->dep_map, ip);
	trace_contention_end(lock, 0);

	if (ww_ctx)
		ww_mutex_lock_acquired(ww, ww_ctx);

	raw_spin_unlock_irqrestore_wake(&lock->wait_lock, flags, &wake_q);
	preempt_enable();
	return 0;

err:
	clear_task_blocked_on(current, lock);
	__set_current_state(TASK_RUNNING);
	__mutex_remove_waiter(lock, &waiter);
err_early_kill:
	WARN_ON(get_task_blocked_on(current));
	trace_contention_end(lock, ret);
	raw_spin_unlock_irqrestore_wake(&lock->wait_lock, flags, &wake_q);
	debug_mutex_free_waiter(&waiter);
	mutex_release(&lock->dep_map, ip);
	preempt_enable();
	return ret;
}

/*
 * __mutex_lock - 把普通 mutex 参数适配到共享慢路径
 * @lock: 已初始化且保持存活的 mutex
 * @state: 等待任务状态，决定可响应的信号类别
 * @subclass: lockdep 子类
 * @nest_lock: 可选 lockdep 嵌套 map
 * @ip: 调用地址
 *
 * 禁用 ww 语义并原样返回共享慢路径结果；0 表示已持锁，负值表示未持锁。上下文、睡眠和清理契约
 * 由 __mutex_lock_common() 承担。
 */
static int __sched
__mutex_lock(struct mutex *lock, unsigned int state, unsigned int subclass,
	     struct lockdep_map *nest_lock, unsigned long ip)
	__cond_acquires(0, lock)
{
	return __mutex_lock_common(lock, state, subclass, nest_lock, ip, NULL, false);
}

/*
 * __ww_mutex_lock - 把 ww_mutex 参数适配到共享慢路径
 * @lock: ww_mutex 中保持存活的 base mutex
 * @state: 不可中断或可中断等待状态
 * @subclass: lockdep 子类
 * @ip: 调用地址
 * @ww_ctx: 可选 acquire context；即使为空仍启用 ww waiter 布局
 *
 * 返回共享路径的 0 或负错误；成功时 current 持有 base 且 ww 上下文已登记，失败时不持锁。
 */
static int __sched
__ww_mutex_lock(struct mutex *lock, unsigned int state, unsigned int subclass,
		unsigned long ip, struct ww_acquire_ctx *ww_ctx)
	__cond_acquires(0, lock)
{
	return __mutex_lock_common(lock, state, subclass, NULL, ip, ww_ctx, true);
}

/**
 * ww_mutex_trylock - tries to acquire the w/w mutex with optional acquire context
 * @ww: mutex to lock
 * @ww_ctx: optional w/w acquire context
 *
 * Trylocks a mutex with the optional acquire context; no deadlock detection is
 * possible. Returns 1 if the mutex has been acquired successfully, 0 otherwise.
 *
 * Unlike ww_mutex_lock, no deadlock handling is performed. However, if a @ctx is
 * specified, -EALREADY handling may happen in calls to ww_mutex_trylock.
 *
 * A mutex acquired with this function must be released with ww_mutex_unlock.
 */
/*
 * ww_mutex_trylock - 带可选 acquire context 的非阻塞 ww_mutex 尝试
 * @ww: 已初始化且保持存活的 ww_mutex
 * @ww_ctx: 可选的有效 acquire context
 *
 * 本 API 不等待，因此不能执行完整死锁处理；成功返回 1，竞争返回 0。无 context 时直接委托普通
 * mutex_trylock()；有 context 时可在重复获取诊断中涉及 -EALREADY 语义，但本函数的公开返回仍是
 * 布尔值。成功后登记 ww context 和 lockdep nest，必须用 ww_mutex_unlock() 释放；失败不改所有权。
 */
int ww_mutex_trylock(struct ww_mutex *ww, struct ww_acquire_ctx *ww_ctx)
{
	if (!ww_ctx)
		return mutex_trylock(&ww->base);

	MUTEX_WARN_ON(ww->base.magic != &ww->base);

	/*
	 * Reset the wounded flag after a kill. No other process can
	 * race and wound us here, since they can't have a valid owner
	 * pointer if we don't have any locks held.
	 */
	/* acquired 为零时可安全重置被 wound 标记，因为没有有效 owner 能并发引用并 wound 此 context。 */
	if (ww_ctx->acquired == 0)
		ww_ctx->wounded = 0;

	if (__mutex_trylock(&ww->base)) {
		ww_mutex_set_context_fastpath(ww, ww_ctx);
		mutex_acquire_nest(&ww->base.dep_map, 0, 1, &ww_ctx->dep_map, _RET_IP_);
		return 1;
	}

	return 0;
}
EXPORT_SYMBOL(ww_mutex_trylock);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/*
 * mutex_lock_nested - 以显式 lockdep 子类不可中断地获取 mutex
 * @lock: 已初始化且保持存活的 mutex
 * @subclass: 调用者选择的 lockdep 子类编号
 *
 * 可睡眠任务上下文调用；内部慢路径不返回错误，返回时 current 持锁。末尾 __acquire() 只补充静态
 * 上下文分析，实际运行时所有权已由 __mutex_lock() 建立，调用者必须配对 mutex_unlock()。
 */
void __sched
mutex_lock_nested(struct mutex *lock, unsigned int subclass)
{
	__mutex_lock(lock, TASK_UNINTERRUPTIBLE, subclass, NULL, _RET_IP_);
	__acquire(lock);
}

EXPORT_SYMBOL_GPL(mutex_lock_nested);

/*
 * _mutex_lock_nest_lock - 按另一 lockdep map 的嵌套关系获取 mutex
 * @lock: 已初始化且保持存活的 mutex
 * @nest: 作为嵌套父关系的借用 lockdep map
 *
 * 以不可中断状态睡眠直至成功；返回时 current 持锁且依赖图以 @nest 归类。函数不取得 @nest 的
 * 生命周期所有权，调用者须保证登记期间有效并最终释放 @lock。
 */
void __sched
_mutex_lock_nest_lock(struct mutex *lock, struct lockdep_map *nest)
{
	__mutex_lock(lock, TASK_UNINTERRUPTIBLE, 0, nest, _RET_IP_);
	__acquire(lock);
}
EXPORT_SYMBOL_GPL(_mutex_lock_nest_lock);

/*
 * _mutex_lock_killable - 可被致命信号打断的 lockdep 嵌套获取
 * @lock: 已初始化且保持存活的 mutex
 * @subclass: lockdep 子类
 * @nest: 可选嵌套 map
 *
 * 可睡眠任务上下文调用；返回 0 时 current 持锁，返回 -EINTR 时未持锁。调用者必须只在成功后
 * 配对解锁，并保证 @nest 在本次依赖登记期间有效。
 */
int __sched
_mutex_lock_killable(struct mutex *lock, unsigned int subclass,
				      struct lockdep_map *nest)
{
	return __mutex_lock(lock, TASK_KILLABLE, subclass, nest, _RET_IP_);
}
EXPORT_SYMBOL_GPL(_mutex_lock_killable);

/*
 * mutex_lock_interruptible_nested - 可被任意待处理信号打断的子类获取
 * @lock: 已初始化且保持存活的 mutex
 * @subclass: lockdep 子类
 *
 * 返回 0 时 current 持锁并须解锁，返回 -EINTR 时取消 waiter 且不持锁；调用期间可能睡眠。
 */
int __sched
mutex_lock_interruptible_nested(struct mutex *lock, unsigned int subclass)
{
	return __mutex_lock(lock, TASK_INTERRUPTIBLE, subclass, NULL, _RET_IP_);
}
EXPORT_SYMBOL_GPL(mutex_lock_interruptible_nested);

/*
 * mutex_lock_io_nested - 以 I/O wait 记账不可中断地获取指定子类 mutex
 * @lock: 已初始化且保持存活的 mutex
 * @subclass: lockdep 子类
 *
 * 可睡眠任务上下文调用。io_schedule_prepare() 返回的 token 只在本栈帧借用，用于在获取完成后恢复
 * 原记账状态；共享路径不返回错误。返回时 current 持锁并已结束 I/O wait 记账，调用者须解锁。
 */
void __sched
mutex_lock_io_nested(struct mutex *lock, unsigned int subclass)
{
	int token;

	might_sleep();

	token = io_schedule_prepare();
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE,
			    subclass, NULL, _RET_IP_, NULL, 0);
	__acquire(lock);
	io_schedule_finish(token);
}
EXPORT_SYMBOL_GPL(mutex_lock_io_nested);

/*
 * ww_mutex_deadlock_injection - 调试构建中按递增间隔伪造 ww 死锁回退
 * @lock: current 刚取得且保持存活的 ww_mutex
 * @ctx: current 的有效 acquire context
 *
 * 启用 DEBUG_WW_MUTEX_SLOWPATH 时递减 countdown；命中零后把下次间隔约扩大 3.5 倍并饱和到
 * UINT_MAX，记录 contending_lock，主动释放 @lock，再返回 -EDEADLK。未命中或关闭配置返回 0 且
 * 仍持锁。调用者必须依据返回值判断所有权，非零出口已替它完成释放。
 */
static inline int
ww_mutex_deadlock_injection(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
	__cond_releases(nonzero, lock)
{
#ifdef CONFIG_DEBUG_WW_MUTEX_SLOWPATH
	unsigned tmp;
	/* tmp 只在本栈帧计算下一次注入间隔，写回 context 后即失效。 */

	if (ctx->deadlock_inject_countdown-- == 0) {
		tmp = ctx->deadlock_inject_interval;
		if (tmp > UINT_MAX/4)
			tmp = UINT_MAX;
		else
			tmp = tmp*2 + tmp + tmp/2;

		ctx->deadlock_inject_interval = tmp;
		ctx->deadlock_inject_countdown = tmp;
		ctx->contending_lock = lock;

		ww_mutex_unlock(lock);

		return -EDEADLK;
	}
#endif

	return 0;
}

/*
 * ww_mutex_lock - 不可中断地获取 ww_mutex 的 lockdep 调试实现
 * @lock: 已初始化且保持存活的 ww_mutex
 * @ctx: 可选 acquire context
 *
 * 调用可能睡眠。共享 ww 路径成功后，若 context 已持有多把锁则运行死锁注入；返回 0 时 current
 * 持锁，返回负错误时不持锁（注入的 -EDEADLK 已在 helper 中解锁）。成功必须配对 ww_mutex_unlock()。
 */
int __sched
ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	int ret;

	might_sleep();
	ret =  __ww_mutex_lock(&lock->base, TASK_UNINTERRUPTIBLE,
			       0, _RET_IP_, ctx);
	if (!ret && ctx && ctx->acquired > 1)
		return ww_mutex_deadlock_injection(lock, ctx);

	return ret;
}
EXPORT_SYMBOL_GPL(ww_mutex_lock);

/*
 * ww_mutex_lock_interruptible - 可被信号打断的 ww_mutex lockdep 调试实现
 * @lock: 已初始化且保持存活的 ww_mutex
 * @ctx: 可选 acquire context
 *
 * 共享路径以 TASK_INTERRUPTIBLE 等待；成功后可能执行死锁注入。返回 0 时 current 持锁，返回
 * -EINTR/-EDEADLK 等负值时不持锁，其中注入分支已主动解锁。成功必须配对 ww_mutex_unlock()。
 */
int __sched
ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	int ret;

	might_sleep();
	ret = __ww_mutex_lock(&lock->base, TASK_INTERRUPTIBLE,
			      0, _RET_IP_, ctx);

	if (!ret && ctx && ctx->acquired > 1)
		return ww_mutex_deadlock_injection(lock, ctx);

	return ret;
}
EXPORT_SYMBOL_GPL(ww_mutex_lock_interruptible);

#endif

/*
 * Release the lock, slowpath:
 */
/* 这是处理 waiter、定向交接和 proxy donor 的释放慢路径。 */
/*
 * __mutex_unlock_slowpath - 释放带协议标志或 lockdep 状态的 mutex 并选择唤醒目标
 * @lock: current 持有、且必须至少存活到函数返回的 mutex
 * @ip: lockdep release 的调用地址
 *
 * 先登记 lockdep release，再关闭抢占稳定 proxy donor。无 HANDOFF 时优先 release cmpxchg 清 task，
 * 若既无 waiter 可直接返回；否则取得 wait_lock 与 current->blocked_lock，从调度器 donor 或首 waiter
 * 中选择 next，必要时以 PICKUP 定向交接，释放内部锁后再唤醒并归还临时 task 引用。函数不返回
 * 错误；所有出口 current 都不再持 mutex。调用者必须确保对象在整个慢路径内不能被并发释放。
 */
static noinline void __sched __mutex_unlock_slowpath(struct mutex *lock, unsigned long ip)
	__releases(lock)
{
	struct task_struct *donor, *next = NULL;
	struct mutex_waiter *waiter;
	unsigned long owner;
	unsigned long flags;
	/*
	 * donor 是无引用调度器候选，next 是选中后持引用的唤醒/交接目标；waiter 是 wait_lock 下的队首
	 * 快照；owner 保存 cmpxchg 失败回填的协议字，flags 保存 wait_lock 的本地 IRQ 状态。
	 */

	mutex_release(&lock->dep_map, ip);
	__release(lock);

	/*
	 * Ensures the proxy donor stack is stable across unlock and handoff.
	 * Specifically, it avoids the case where current->blocked_donor is
	 * NULL when it is inspected while doing the unlock, but a preemption
	 * before taking the wake_lock would make it set and a hand-off is
	 * missed.
	 */
	/*
	 * 抢占 guard 让 proxy donor 栈跨越解锁和交接保持稳定，避免检查 blocked_donor 时为 NULL，却在
	 * 取得 wake_lock 前被抢占并新增 donor，从而漏掉应做的 handoff。
	 */
	guard(preempt)();
	/*
	 * Release the lock before (potentially) taking the spinlock such that
	 * other contenders can get on with things ASAP.
	 *
	 * Except when HANDOFF, in that case we must not clear the owner field,
	 * but instead set it to the top waiter.
	 */
	/*
	 * 通常先释放 owner 再取自旋锁，让其他竞争者尽早前进；HANDOFF 例外，不能先清 owner，而应把
	 * 它直接设为最高优先等待者。
	 */
	owner = atomic_long_read(&lock->owner);
	for (;;) {
		MUTEX_WARN_ON(__owner_task(owner) != current);
		MUTEX_WARN_ON(owner & MUTEX_FLAG_PICKUP);

		if (sched_proxy_exec() && current->blocked_donor) {
			/* force handoff if we have a blocked_donor */
			/* 存在 blocked_donor 时强制定向交接。 */
			owner = MUTEX_FLAG_HANDOFF;
			break;
		}

		if (owner & MUTEX_FLAG_HANDOFF)
			break;

		if (atomic_long_try_cmpxchg_release(&lock->owner, &owner, __owner_flags(owner))) {
			if (owner & MUTEX_FLAG_WAITERS)
				break;

			return;
		}
	}

	raw_spin_lock_irqsave(&lock->wait_lock, flags);
	raw_spin_lock(&current->blocked_lock);
	debug_mutex_unlock(lock);

	if (sched_proxy_exec()) {
		/*
		 * If we have a task boosting current, and that task was boosting
		 * current through this lock, hand the lock to that task, as that
		 * is the highest waiter, as selected by the scheduling function.
		 */
		/*
		 * 若某任务正通过本锁提升 current，它就是调度函数选出的最高 waiter；确认其 blocked_on 仍是
		 * 本锁后取得 task 引用、清阻塞关系并优先把锁交给它。
		 */
		donor = current->blocked_donor;
		if (donor) {
			struct mutex *next_lock;

			raw_spin_lock_nested(&donor->blocked_lock, SINGLE_DEPTH_NESTING);
			next_lock = __get_task_blocked_on(donor);
			if (next_lock == lock) {
				next = get_task_struct(donor);
				__clear_task_blocked_on(next, lock);
				current->blocked_donor = NULL;
			}
			raw_spin_unlock(&donor->blocked_lock);
		}
	}

	/*
	 * Failing that, pick first on the wait list.
	 */
	/* 没有合格 donor 时退回选择普通等待队列首项。 */
	waiter = lock->first_waiter;
	if (!next && waiter) {
		next = get_task_struct(waiter->task);

		raw_spin_lock_nested(&next->blocked_lock, SINGLE_DEPTH_NESTING);
		debug_mutex_wake_waiter(lock, waiter);
		__clear_task_blocked_on(next, lock);
		raw_spin_unlock(&next->blocked_lock);

	}

	if (trace_contended_release_enabled() && waiter)
		trace_call__contended_release(lock);

	if (owner & MUTEX_FLAG_HANDOFF)
		__mutex_handoff(lock, next);

	raw_spin_unlock(&current->blocked_lock);
	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);
	if (next) {
		wake_up_process(next);
		put_task_struct(next);
	}
}

#ifndef CONFIG_DEBUG_LOCK_ALLOC
/*
 * Here come the less common (and hence less performance-critical) APIs:
 * mutex_lock_interruptible() and mutex_trylock().
 */
/* 以下是使用较少、因而性能敏感度较低的 interruptible/killable/trylock API。 */
/*
 * __mutex_lock_killable_slowpath - killable 公共入口所用慢路径声明
 * @lock: 保持存活的 mutex；返回 0 时由 current 持有，-EINTR 时不持有
 */
static noinline int __sched
__mutex_lock_killable_slowpath(struct mutex *lock);

/*
 * __mutex_lock_interruptible_slowpath - interruptible 公共入口所用慢路径声明
 * @lock: 保持存活的 mutex；返回 0 时由 current 持有，-EINTR 时不持有
 */
static noinline int __sched
__mutex_lock_interruptible_slowpath(struct mutex *lock);

/**
 * mutex_lock_interruptible() - Acquire the mutex, interruptible by signals.
 * @lock: The mutex to be acquired.
 *
 * Lock the mutex like mutex_lock().  If a signal is delivered while the
 * process is sleeping, this function will return without acquiring the
 * mutex.
 *
 * Context: Process context.
 * Return: 0 if the lock was successfully acquired or %-EINTR if a
 * signal arrived.
 */
/*
 * mutex_lock_interruptible - 可被信号打断地获取 mutex
 * @lock: 已初始化且保持存活的 mutex
 *
 * 仅限可睡眠任务上下文。严格零 owner 走快速获取；竞争时进入 TASK_INTERRUPTIBLE 慢路径。返回 0
 * 表示 current 持锁并须解锁，返回 -EINTR 表示信号到达且 waiter 已撤销、没有取得锁。
 */
int __sched mutex_lock_interruptible(struct mutex *lock)
{
	might_sleep();

	if (__mutex_trylock_fast(lock))
		return 0;

	return __mutex_lock_interruptible_slowpath(lock);
}

EXPORT_SYMBOL(mutex_lock_interruptible);

/**
 * mutex_lock_killable() - Acquire the mutex, interruptible by fatal signals.
 * @lock: The mutex to be acquired.
 *
 * Lock the mutex like mutex_lock().  If a signal which will be fatal to
 * the current process is delivered while the process is sleeping, this
 * function will return without acquiring the mutex.
 *
 * Context: Process context.
 * Return: 0 if the lock was successfully acquired or %-EINTR if a
 * fatal signal arrived.
 */
/*
 * mutex_lock_killable - 只可被致命信号打断地获取 mutex
 * @lock: 已初始化且保持存活的 mutex
 *
 * 仅限可睡眠任务上下文。快速成功返回 0；慢路径以 TASK_KILLABLE 等待，返回 -EINTR 时未持锁，
 * 其他普通信号不会取消等待。成功后必须由同一任务 mutex_unlock()。
 */
int __sched mutex_lock_killable(struct mutex *lock)
{
	might_sleep();

	if (__mutex_trylock_fast(lock))
		return 0;

	return __mutex_lock_killable_slowpath(lock);
}
EXPORT_SYMBOL(mutex_lock_killable);

/**
 * mutex_lock_io() - Acquire the mutex and mark the process as waiting for I/O
 * @lock: The mutex to be acquired.
 *
 * Lock the mutex like mutex_lock().  While the task is waiting for this
 * mutex, it will be accounted as being in the IO wait state by the
 * scheduler.
 *
 * Context: Process context.
 */
/*
 * mutex_lock_io - 以 I/O wait 记账不可中断地获取 mutex
 * @lock: 已初始化且保持存活的 mutex
 *
 * 仅限可睡眠任务上下文。先保存并切换 I/O 等待记账，再调用无错误返回的 mutex_lock()，最后用栈上
 * token 恢复记账。返回时 current 持锁且调度记账已恢复，调用者须配对解锁。
 */
void __sched mutex_lock_io(struct mutex *lock)
{
	int token;

	token = io_schedule_prepare();
	mutex_lock(lock);
	io_schedule_finish(token);
}
EXPORT_SYMBOL_GPL(mutex_lock_io);

/*
 * __mutex_lock_slowpath - 非 lockdep 公共 mutex_lock() 的不可中断慢包装
 * @lock: 已初始化且保持存活的 mutex
 *
 * 以默认子类进入共享路径且不返回错误；返回时 current 持锁。__acquire() 是静态分析标记，调用者
 * mutex_lock() 负责最终配对解锁。
 */
static noinline void __sched
__mutex_lock_slowpath(struct mutex *lock)
	__acquires(lock)
{
	__mutex_lock(lock, TASK_UNINTERRUPTIBLE, 0, NULL, _RET_IP_);
	__acquire(lock);
}

/*
 * __mutex_lock_killable_slowpath - 默认子类的 killable 慢包装
 * @lock: 已初始化且保持存活的 mutex
 *
 * 返回 0 时 current 持锁，返回 -EINTR 时不持锁；共享路径负责 waiter 和任务状态清理。
 */
static noinline int __sched
__mutex_lock_killable_slowpath(struct mutex *lock)
	__cond_acquires(0, lock)
{
	return __mutex_lock(lock, TASK_KILLABLE, 0, NULL, _RET_IP_);
}

/*
 * __mutex_lock_interruptible_slowpath - 默认子类的 interruptible 慢包装
 * @lock: 已初始化且保持存活的 mutex
 *
 * 返回 0 时 current 持锁，返回 -EINTR 时不持锁；共享路径负责撤销阻塞状态和 waiter。
 */
static noinline int __sched
__mutex_lock_interruptible_slowpath(struct mutex *lock)
	__cond_acquires(0, lock)
{
	return __mutex_lock(lock, TASK_INTERRUPTIBLE, 0, NULL, _RET_IP_);
}

/*
 * __ww_mutex_lock_slowpath - 非 lockdep ww 公共入口的不可中断慢包装
 * @lock: 已初始化且保持存活的 ww_mutex
 * @ctx: 可选 acquire context
 *
 * 返回共享 ww 路径的 0 或负错误；成功时 current 持锁且 context 已登记，失败时不持锁。
 */
static noinline int __sched
__ww_mutex_lock_slowpath(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
	__cond_acquires(0, lock)
{
	return __ww_mutex_lock(&lock->base, TASK_UNINTERRUPTIBLE, 0,
			       _RET_IP_, ctx);
}

/*
 * __ww_mutex_lock_interruptible_slowpath - 非 lockdep ww 公共入口的可中断慢包装
 * @lock: 已初始化且保持存活的 ww_mutex
 * @ctx: 可选 acquire context
 *
 * 以 TASK_INTERRUPTIBLE 进入共享路径；返回 0 时持锁，-EINTR 或 ww 死锁错误时不持锁。
 */
static noinline int __sched
__ww_mutex_lock_interruptible_slowpath(struct ww_mutex *lock,
					    struct ww_acquire_ctx *ctx)
	__cond_acquires(0, lock)
{
	return __ww_mutex_lock(&lock->base, TASK_INTERRUPTIBLE, 0,
			       _RET_IP_, ctx);
}

#endif

#ifndef CONFIG_DEBUG_LOCK_ALLOC
/**
 * mutex_trylock - try to acquire the mutex, without waiting
 * @lock: the mutex to be acquired
 *
 * Try to acquire the mutex atomically. Returns 1 if the mutex
 * has been acquired successfully, and 0 on contention.
 *
 * NOTE: this function follows the spin_trylock() convention, so
 * it is negated from the down_trylock() return values! Be careful
 * about this when converting semaphore users to mutexes.
 *
 * This function must not be used in interrupt context. The
 * mutex must be released by the same task that acquired it.
 */
/*
 * mutex_trylock - 不等待地尝试取得 mutex
 * @lock: 已初始化且保持存活的 mutex
 *
 * 不得在中断上下文调用。原子成功返回 1 且 current 持锁，竞争返回 0 且无所有权变化；返回约定与
 * spin_trylock() 相同、与 down_trylock() 相反。成功后必须由同一任务 mutex_unlock()。
 */
int __sched mutex_trylock(struct mutex *lock)
{
	MUTEX_WARN_ON(lock->magic != lock);
	return __mutex_trylock(lock);
}
EXPORT_SYMBOL(mutex_trylock);
#else
/*
 * _mutex_trylock_nest_lock - 带 lockdep 嵌套 map 的非阻塞尝试
 * @lock: 已初始化且保持存活的 mutex
 * @nest_lock: 成功时用于依赖登记的借用嵌套 map
 *
 * 成功返回 1、登记 acquire 并由 current 持锁；竞争返回 0 且不登记。函数不等待、不取得 map
 * 生命周期所有权，成功后必须配对 mutex_unlock()。
 */
int __sched _mutex_trylock_nest_lock(struct mutex *lock, struct lockdep_map *nest_lock)
{
	bool locked;

	MUTEX_WARN_ON(lock->magic != lock);
	locked = __mutex_trylock(lock);
	if (locked)
		mutex_acquire_nest(&lock->dep_map, 0, 1, nest_lock, _RET_IP_);

	return locked;
}
EXPORT_SYMBOL(_mutex_trylock_nest_lock);
#endif

#ifndef CONFIG_DEBUG_LOCK_ALLOC
/*
 * ww_mutex_lock - 非 lockdep 构建的不可中断 ww_mutex 快/慢入口
 * @lock: 已初始化且保持存活的 ww_mutex
 * @ctx: 可选 acquire context
 *
 * 可睡眠任务上下文调用。严格零 owner 时快速取得并按需发布 context；否则进入 ww 慢路径。返回 0
 * 时 current 持锁，负错误时不持锁；成功必须配对 ww_mutex_unlock()。
 */
int __sched
ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	might_sleep();

	if (__mutex_trylock_fast(&lock->base)) {
		if (ctx)
			ww_mutex_set_context_fastpath(lock, ctx);
		return 0;
	}

	return __ww_mutex_lock_slowpath(lock, ctx);
}
EXPORT_SYMBOL(ww_mutex_lock);

/*
 * ww_mutex_lock_interruptible - 非 lockdep 构建的可中断 ww_mutex 快/慢入口
 * @lock: 已初始化且保持存活的 ww_mutex
 * @ctx: 可选 acquire context
 *
 * 快速成功时登记 context 并返回 0；竞争时以 TASK_INTERRUPTIBLE 进入共享慢路径。返回 0 时持锁，
 * -EINTR 或 ww 错误时不持锁，成功后必须配对 ww_mutex_unlock()。
 */
int __sched
ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	might_sleep();

	if (__mutex_trylock_fast(&lock->base)) {
		if (ctx)
			ww_mutex_set_context_fastpath(lock, ctx);
		return 0;
	}

	return __ww_mutex_lock_interruptible_slowpath(lock, ctx);
}
EXPORT_SYMBOL(ww_mutex_lock_interruptible);

#endif /* !CONFIG_DEBUG_LOCK_ALLOC */
#endif /* !CONFIG_PREEMPT_RT */

EXPORT_TRACEPOINT_SYMBOL_GPL(contention_begin);
EXPORT_TRACEPOINT_SYMBOL_GPL(contention_end);
EXPORT_TRACEPOINT_SYMBOL_GPL(contended_release);

/**
 * atomic_dec_and_mutex_lock - return holding mutex if we dec to 0
 * @cnt: the atomic which we are to dec
 * @lock: the mutex to return holding if we dec to 0
 *
 * return true and hold lock if we dec to 0, return false otherwise
 */
/*
 * atomic_dec_and_mutex_lock - 递减计数，并只在归零时带锁返回
 * @cnt: 调用者共享的原子计数；本函数消费恰好一次递减
 * @lock: 归零串行化所用、已初始化且保持存活的 mutex
 *
 * 若计数不可能从 1 变 0，atomic_add_unless() 直接递减并返回 0。可能归零时先获取 mutex，再执行
 * 最终递减：未归零则解锁并返回 0；恰好归零则返回 1 且 current 仍持有 @lock，由调用者在完成
 * 零引用处理后解锁。函数可睡眠，不能在中断上下文调用；布尔返回同时编码锁所有权。
 */
int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
	/* dec if we can't possibly hit 0 */
	/* 若本次不可能归零，直接递减而无需串行化。 */
	if (atomic_add_unless(cnt, -1, 1))
		return 0;
	/* we might hit 0, so take the lock */
	/* 可能归零，先取 mutex 与其他归零处理串行化。 */
	mutex_lock(lock);
	if (!atomic_dec_and_test(cnt)) {
		/* when we actually did the dec, we didn't hit 0 */
		/* 真正递减后并未归零，释放 mutex 并报告无锁返回。 */
		mutex_unlock(lock);
		return 0;
	}
	/* we hit 0, and we hold the lock */
	/* 已归零且仍持有 mutex，把零引用处理责任交给调用者。 */
	return 1;
}
EXPORT_SYMBOL(atomic_dec_and_mutex_lock);
