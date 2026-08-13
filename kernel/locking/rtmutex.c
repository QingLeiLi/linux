// SPDX-License-Identifier: GPL-2.0-only
/*
 * RT-Mutexes: simple blocking mutual exclusion locks with PI support
 *
 * started by Ingo Molnar and Thomas Gleixner.
 *
 *  Copyright (C) 2004-2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *  Copyright (C) 2005-2006 Timesys Corp., Thomas Gleixner <tglx@timesys.com>
 *  Copyright (C) 2005 Kihon Technologies Inc., Steven Rostedt
 *  Copyright (C) 2006 Esben Nielsen
 * Adaptive Spinlocks:
 *  Copyright (C) 2008 Novell, Inc., Gregory Haskins, Sven Dietrich,
 *				     and Peter Morreale,
 * Adaptive Spinlocks simplification:
 *  Copyright (C) 2008 Red Hat, Inc., Steven Rostedt <srostedt@redhat.com>
 *
 *  See Documentation/locking/rt-mutex-design.rst for details.
 */
/*
 * RT-Mutex：带优先级继承（PI）的简单阻塞式互斥锁。
 * 最初由 Ingo Molnar 与 Thomas Gleixner 开始实现。
 * 自适应自旋锁相关贡献：
 * 自适应自旋锁简化相关贡献：
 *  详细设计见 Documentation/locking/rt-mutex-design.rst；本文件把 PI 链、死锁检测和慢路径落实为代码。
 */
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/deadline.h>
#include <linux/sched/signal.h>
#include <linux/sched/rt.h>
#include <linux/sched/wake_q.h>
#include <linux/ww_mutex.h>

#include <trace/events/lock.h>

#include "rtmutex_common.h"
#include "lock_events.h"

#ifndef WW_RT
/* 当前文本实例不是 WW-RT：编译期常量让编译器删除所有 wound/wait 专属分支。 */
# define build_ww_mutex()	(false)
/* 非 WW 实例没有外层 ww_mutex 容器，相关只读别名恒为空。 */
# define ww_container_of(rtm)	NULL

/*
 * 非 WW-RT 构建中“新 waiter 加入后的 wound/wait 判定”占位实现。
 * waiter、lock、ww_ctx 与 wake_q 均为借用输入且在此实例不读取；调用者已持有底层 wait_lock。
 * 返回：恒为 0，表示普通 rtmutex 不要求事务立即退避；无字段、引用、锁或唤醒队列副作用，不能睡眠。
 * WW_RT 实例由随后包含的 ww_mutex.h 提供真实实现，可能返回 -EDEADLK 并排入延迟唤醒。
 */
static inline int __ww_mutex_add_waiter(struct rt_mutex_waiter *waiter,
					struct rt_mutex *lock,
					struct ww_acquire_ctx *ww_ctx,
					struct wake_q_head *wake_q)
{
	return 0;
}

/*
 * 非 WW-RT 构建中“锁状态变化后检查 waiter”占位实现。
 * lock、ww_ctx 和 wake_q 都是借用输入，在普通 rtmutex 实例中无事务可 wound，故全部忽略。
 * 返回：无直接返回值；不修改锁、上下文或 wake_q，不取得引用且不能睡眠。
 * WW_RT 实例的对应 helper 会在 wait_lock 下扫描需唤醒/击杀的事务。
 */
static inline void __ww_mutex_check_waiters(struct rt_mutex *lock,
					    struct ww_acquire_ctx *ww_ctx,
					    struct wake_q_head *wake_q)
{
}

/*
 * 非 WW-RT 构建中“成功获取后登记 acquire_ctx”占位实现。
 * lock 与 ww_ctx 均为借用输入；普通 rtmutex 没有 acquired 计数或 ctx ownership，故不访问参数。
 * 返回：无直接返回值、无副作用且不能睡眠；真实 WW_RT 实例在成功边界发布 lock->ctx 并增加 acquired。
 */
static inline void ww_mutex_lock_acquired(struct ww_mutex *lock,
					  struct ww_acquire_ctx *ww_ctx)
{
}

/*
 * 非 WW-RT 构建中等待循环的 wound/kill 检查占位实现。
 * lock、waiter 与 ww_ctx 都是借用输入，调用位置持有 wait_lock；普通 rtmutex 不执行 WW 退避协议。
 * 返回：恒为 0，允许继续等待；不出队、不改变任务状态且不能睡眠。WW_RT 实例可能返回 -EDEADLK。
 */
static inline int __ww_mutex_check_kill(struct rt_mutex *lock,
					struct rt_mutex_waiter *waiter,
					struct ww_acquire_ctx *ww_ctx)
{
	return 0;
}

#else
/* WW_RT 文本实例启用事务分支，并从内嵌 rt_mutex 反推出外层 ww_mutex。 */
# define build_ww_mutex()	(true)
# define ww_container_of(rtm)	container_of(rtm, struct ww_mutex, base)
/* 引入已按 RT 后端参数化的 WW helper；这些定义只属于 WW_RT 生成实例。 */
# include "ww_mutex.h"
#endif

/*
 * lock->owner state tracking:
 *
 * lock->owner holds the task_struct pointer of the owner. Bit 0
 * is used to keep track of the "lock has waiters" state.
 *
 * owner	bit0
 * NULL		0	lock is free (fast acquire possible)
 * NULL		1	lock is free and has waiters and the top waiter
 *				is going to take the lock*
 * taskpointer	0	lock is held (fast release possible)
 * taskpointer	1	lock is held and has waiters**
 *
 * The fast atomic compare exchange based acquire and release is only
 * possible when bit 0 of lock->owner is 0.
 *
 * (*) It also can be a transitional state when grabbing the lock
 * with ->wait_lock is held. To prevent any fast path cmpxchg to the lock,
 * we need to set the bit0 before looking at the lock, and the owner may be
 * NULL in this small time, hence this can be a transitional state.
 *
 * (**) There is a small time when bit 0 is set but there are no
 * waiters. This can happen when grabbing the lock in the slow path.
 * To prevent a cmpxchg of the owner releasing the lock, we need to
 * set this bit before looking at the lock.
 */
/*
 *
 * lock->owner 保存 task_struct owner 指针，并借用按对齐恒为 0 的最低位表示“存在等待者”。四种组合：
 * NULL/0 为空闲且可 fast acquire；NULL/1 为空闲但 top waiter 正准备接管；task/0 为已持有且可
 * fast release；task/1 为已持有且存在等待者。只有 bit0 为 0 时原子 cmpxchg fastpath 才可能成功。
 *
 * NULL/1 也可出现在持有 wait_lock 的获取过渡期：慢路径先置 bit0 阻止并发 fastpath，再检查 owner，
 * 所以短时间内尚无 owner。task/1 也可能在 waiter 真正入树前出现：先置位可阻止原 owner 并发释放。
 * 两种暂态都由 wait_lock 串行，并必须在退出慢路径前由 set/fixup helper 收敛。
 */

/*
 * 按当前 waiter 树状态把 owner 编码成可写入 lock->owner 的带标志指针。
 * lock 是借用基础锁，owner 是可空借用任务指针；调用者必须持 wait_lock，函数不能睡眠。
 * 返回 owner 地址，若 waiter 树非空则附加 RT_MUTEX_HAS_WAITERS；不修改锁或任务引用。
 */
static __always_inline struct task_struct *
rt_mutex_owner_encode(struct rt_mutex_base *lock, struct task_struct *owner)
	__must_hold(&lock->wait_lock)
{
	/* val 以整数形式承载对齐任务指针和最低位标志，返回时再转回 tagged pointer 类型。 */
	unsigned long val = (unsigned long)owner;

	if (rt_mutex_has_waiters(lock))
		val |= RT_MUTEX_HAS_WAITERS;

	return (struct task_struct *)val;
}

/*
 * 在已持 wait_lock 的接管边界发布 lock 的新 owner。
 * lock 是输入输出对象，owner 是新持有者的可空借用任务指针；不能睡眠，也不转移 task 引用。
 * 返回：无直接返回值；xchg_acquire 原子写入 owner 与 waiter 位，为新 owner 建立获取语义。
 */
static __always_inline void
rt_mutex_set_owner(struct rt_mutex_base *lock, struct task_struct *owner)
	__must_hold(&lock->wait_lock)
{
	/*
	 * lock->wait_lock is held but explicit acquire semantics are needed
	 * for a new lock owner so WRITE_ONCE is insufficient.
	 */
	/*
	 *
	 * 即使已持 wait_lock，新锁 owner 仍需显式 acquire 语义来观察前一持有者 release 前的临界区写入；
	 * WRITE_ONCE 只保证一次写，不能提供这一同步边界。
	 */
	xchg_acquire(&lock->owner, rt_mutex_owner_encode(lock, owner));
}

/*
 * 在已持 wait_lock 的释放/交接路径清除 owner，并按 waiter 树保留或清除 bit0。
 * lock 是输入输出借用对象；返回：无直接返回值，不唤醒 waiter、不释放任务引用且不能睡眠。
 * 随后的 wait_lock unlock 提供 release 语义，因此字段更新只需 WRITE_ONCE。
 */
static __always_inline void rt_mutex_clear_owner(struct rt_mutex_base *lock)
	__must_hold(&lock->wait_lock)
{
	/* lock->wait_lock is held so the unlock provides release semantics. */
	/* 已持 wait_lock，稍后的自旋锁释放承担 release 语义。 */
	WRITE_ONCE(lock->owner, rt_mutex_owner_encode(lock, NULL));
}

/*
 * 清除 lock->owner 的 HAS_WAITERS 位，同时保留真实 owner 指针。
 * 调用者必须持 wait_lock 并已确认 waiter 树为空；lock 为输入输出借用对象，函数不能睡眠。
 * 返回：无直接返回值；这是 fastpath-aware unlock 在释放 wait_lock 前开放 cmpxchg 的准备步骤。
 */
static __always_inline void clear_rt_mutex_waiters(struct rt_mutex_base *lock)
	__must_hold(&lock->wait_lock)
{
	lock->owner = (struct task_struct *)
			((unsigned long)lock->owner & ~RT_MUTEX_HAS_WAITERS);
}

/*
 * waiter 树为空时修复可能残留的 HAS_WAITERS 过渡位。
 * lock 是输入输出借用对象且调用者必须持 wait_lock；acquire_lock=true 表示获取路径，需要
 * xchg_acquire，false 表示释放路径可用 WRITE_ONCE。返回：无直接返回值，不能睡眠。
 * 若树重新非空或 bit 已清则不修改；否则只清 bit0，不能用过期快照覆盖并发变化后的 owner。
 */
static __always_inline void
fixup_rt_mutex_waiters(struct rt_mutex_base *lock, bool acquire_lock)
	__must_hold(&lock->wait_lock)
{
	/* owner 是单次状态快照；p 让 tagged owner 可执行只触及最低位的整数 RMW。 */
	unsigned long owner, *p = (unsigned long *) &lock->owner;

	if (rt_mutex_has_waiters(lock))
		return;

	/*
	 * The rbtree has no waiters enqueued, now make sure that the
	 * lock->owner still has the waiters bit set, otherwise the
	 * following can happen:
	 *
	 * CPU 0	CPU 1		CPU2
	 * l->owner=T1
	 *		rt_mutex_lock(l)
	 *		lock(l->lock)
	 *		l->owner = T1 | HAS_WAITERS;
	 *		enqueue(T2)
	 *		boost()
	 *		  unlock(l->lock)
	 *		block()
	 *
	 *				rt_mutex_lock(l)
	 *				lock(l->lock)
	 *				l->owner = T1 | HAS_WAITERS;
	 *				enqueue(T3)
	 *				boost()
	 *				  unlock(l->lock)
	 *				block()
	 *		signal(->T2)	signal(->T3)
	 *		lock(l->lock)
	 *		dequeue(T2)
	 *		deboost()
	 *		  unlock(l->lock)
	 *				lock(l->lock)
	 *				dequeue(T3)
	 *				 ==> wait list is empty
	 *				deboost()
	 *				 unlock(l->lock)
	 *		lock(l->lock)
	 *		fixup_rt_mutex_waiters()
	 *		  if (wait_list_empty(l) {
	 *		    l->owner = owner
	 *		    owner = l->owner & ~HAS_WAITERS;
	 *		      ==> l->owner = T1
	 *		  }
	 *				lock(l->lock)
	 * rt_mutex_unlock(l)		fixup_rt_mutex_waiters()
	 *				  if (wait_list_empty(l) {
	 *				    owner = l->owner & ~HAS_WAITERS;
	 * cmpxchg(l->owner, T1, NULL)
	 *  ===> Success (l->owner = NULL)
	 *
	 *				    l->owner = owner
	 *				      ==> l->owner = T1
	 *				  }
	 *
	 * With the check for the waiter bit in place T3 on CPU2 will not
	 * overwrite. All tasks fiddling with the waiters bit are
	 * serialized by l->lock, so nothing else can modify the waiters
	 * bit. If the bit is set then nothing can change l->owner either
	 * so the simple RMW is safe. The cmpxchg() will simply fail if it
	 * happens in the middle of the RMW because the waiters bit is
	 * still set.
	 */
	/*
	 *
	 * waiter 树已经为空，但还要确认 owner 的 waiter 位仍为 1。原图展示：T2、T3 因信号先后出队，
	 * 两条 fixup 路径都准备清位；若后一条无条件写回旧 owner，CPU0 可能已经 fastpath 把 T1 释放为
	 * NULL，随后 CPU2 却将旧 T1 写回，令已释放的锁“复活”。增加 bit 检查后，所有置/清 waiter 位者
	 * 都由 wait_lock 串行；bit 为 1 时 fastpath 无法改变 owner，简单 RMW 安全。若 cmpxchg 恰在 RMW
	 * 中间发生，也会因为 waiter 位尚未清除而失败。
	 */
	owner = READ_ONCE(*p);
	if (owner & RT_MUTEX_HAS_WAITERS) {
		/*
		 * See rt_mutex_set_owner() and rt_mutex_clear_owner() on
		 * why xchg_acquire() is used for updating owner for
		 * locking and WRITE_ONCE() for unlocking.
		 *
		 * WRITE_ONCE() would work for the acquire case too, but
		 * in case that the lock acquisition failed it might
		 * force other lockers into the slow path unnecessarily.
		 */
		/*
		 *
		 * rt_mutex_set_owner()/clear_owner() 说明了两种更新：获取路径用 xchg_acquire，释放路径用
		 * WRITE_ONCE。获取路径用普通写理论上也能工作，但若获取随后失败，可能无谓地迫使其他任务
		 * 进入慢路径；这里按 acquire_lock 选择与调用阶段匹配的语义。
		 */
		if (acquire_lock)
			xchg_acquire(p, owner & ~RT_MUTEX_HAS_WAITERS);
		else
			WRITE_ONCE(*p, owner & ~RT_MUTEX_HAS_WAITERS);
	}
}

/*
 * We can speed up the acquire/release, if there's no debugging state to be
 * set up.
 */
/*
 *
 * CONFIG_DEBUG_RT_MUTEXES 关闭时无需维护额外调试状态，可用 owner 上的原子比较交换加速获取和释放。
 */
#ifndef CONFIG_DEBUG_RT_MUTEXES
/*
 * 非调试构建的 owner acquire 原语。
 * lock 为输入输出锁；old 是期望的旧 tagged owner，new 是要发布的新 owner，均为借用值。
 * 返回 true 表示原子替换成功并建立 acquire 语义，false 表示 owner 已变化；不睡眠、不管理任务引用。
 */
static __always_inline bool rt_mutex_cmpxchg_acquire(struct rt_mutex_base *lock,
						     struct task_struct *old,
						     struct task_struct *new)
{
	return try_cmpxchg_acquire(&lock->owner, &old, new);
}

/*
 * 尝试让 current 从完全空闲的 NULL/0 状态快速取得 lock。
 * lock 为输入输出借用对象；无须 wait_lock、不能睡眠。成功返回 true 并发布 current 为 owner，
 * 失败返回 false 且不改状态，调用者通常转入受 wait_lock 串行的慢路径。
 */
static __always_inline bool rt_mutex_try_acquire(struct rt_mutex_base *lock)
{
	return rt_mutex_cmpxchg_acquire(lock, NULL, current);
}

/*
 * 非调试构建的 owner release 原语。
 * old 通常是 current，new 通常为 NULL；两者为期望/目标借用值，lock 为输入输出对象。
 * 返回 true 表示 cmpxchg 成功并提供 release 语义，false 表示 waiter 位或 owner 已变化、需慢速交接。
 */
static __always_inline bool rt_mutex_cmpxchg_release(struct rt_mutex_base *lock,
						     struct task_struct *old,
						     struct task_struct *new)
{
	return try_cmpxchg_release(&lock->owner, &old, new);
}

/*
 * Callers must hold the ->wait_lock -- which is the whole purpose as we force
 * all future threads that attempt to [Rmw] the lock to the slowpath. As such
 * relaxed semantics suffice.
 */
/*
 *
 * 调用者必须持有 ->wait_lock；置位的目的正是迫使之后所有尝试读改写该锁的线程进入慢路径并在
 * wait_lock 上串行，因此 cmpxchg 本身使用 relaxed 语义即可，末尾屏障负责发布成功置位。
 */
/*
 * 原子置位 lock->owner 的 HAS_WAITERS 标志而不改变 owner 指针。
 * lock 是输入输出借用对象，调用者持有 wait_lock，不能睡眠；返回无直接值。
 * 循环处理 fastpath 可能同时改变 owner 的竞态，成功后其他 cmpxchg fastpath 将被强制失败。
 */
static __always_inline void mark_rt_mutex_waiters(struct rt_mutex_base *lock)
{
	/* p 指向 tagged owner 的整数表示；owner/new 分别是 cmpxchg 当前快照与置位候选。 */
	unsigned long *p = (unsigned long *) &lock->owner;
	unsigned long owner, new;

	owner = READ_ONCE(*p);
	do {
		new = owner | RT_MUTEX_HAS_WAITERS;
	} while (!try_cmpxchg_relaxed(p, &owner, new));

	/*
	 * The cmpxchg loop above is relaxed to avoid back-to-back ACQUIRE
	 * operations in the event of contention. Ensure the successful
	 * cmpxchg is visible.
	 */
	/*
	 *
	 * 上述循环使用 relaxed，避免竞争时连续执行 acquire 操作；成功后用原子后全屏障保证 bit0 的
	 * 发布先于调用者后续检查/入队，使并发 fastpath 不会越过慢路径保护窗口。
	 */
	smp_mb__after_atomic();
}

/*
 * Safe fastpath aware unlock:
 * 1) Clear the waiters bit
 * 2) Drop lock->wait_lock
 * 3) Try to unlock the lock with cmpxchg
 */
/*
 *
 * 感知 fastpath 的安全解锁顺序：先清 waiter 位，再释放 wait_lock，最后用 cmpxchg 尝试清 owner。
 */
/*
 * 在 waiter 树为空时尝试完成 fastpath-aware 解锁，并无条件释放 wait_lock/恢复 flags 中的中断状态。
 * lock 是输入输出对象；flags 是先前 irqsave 的不透明状态值。返回 true 表示 owner 已清为 NULL；
 * false 表示窗口内新 waiter 已置位，调用者必须重新取得 wait_lock 并走唤醒交接。函数不睡眠。
 */
static __always_inline bool unlock_rt_mutex_safe(struct rt_mutex_base *lock,
						 unsigned long flags)
	__releases(lock->wait_lock)
{
	/* owner 是清 waiter 位前的真实持有者快照，cmpxchg 只允许该精确状态释放。 */
	struct task_struct *owner = rt_mutex_owner(lock);

	clear_rt_mutex_waiters(lock);
	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);
	/*
	 * If a new waiter comes in between the unlock and the cmpxchg
	 * we have two situations:
	 *
	 * unlock(wait_lock);
	 *					lock(wait_lock);
	 * cmpxchg(p, owner, 0) == owner
	 *					mark_rt_mutex_waiters(lock);
	 *					acquire(lock);
	 * or:
	 *
	 * unlock(wait_lock);
	 *					lock(wait_lock);
	 *					mark_rt_mutex_waiters(lock);
	 *
	 * cmpxchg(p, owner, 0) != owner
	 *					enqueue_waiter();
	 *					unlock(wait_lock);
	 * lock(wait_lock);
	 * wake waiter();
	 * unlock(wait_lock);
	 *					lock(wait_lock);
	 *					acquire(lock);
	 */
	/*
	 *
	 * wait_lock 释放到 cmpxchg 之间若有新 waiter：若 cmpxchg 先成功，新 waiter 随后看到空闲锁并获取；
	 * 若 waiter 先置 HAS_WAITERS，cmpxchg 因 tagged owner 不匹配而失败，新 waiter 入队，当前线程重新
	 * 加 wait_lock 并负责唤醒。两种交错都不会让低优先级 fastpath 越过已发布的 top waiter。
	 */
	return rt_mutex_cmpxchg_release(lock, owner, NULL);
}

#else
/*
 * 调试构建禁用无 wait_lock 的 acquire cmpxchg fastpath。
 * lock、old、new 均为借用输入且刻意不访问；恒返回 false，迫使调用者进入具备调试检查的路径。
 * 不修改 owner、不取得引用且不能睡眠。
 */
static __always_inline bool rt_mutex_cmpxchg_acquire(struct rt_mutex_base *lock,
						     struct task_struct *old,
						     struct task_struct *new)
{
	return false;

}

/* 调试构建的 trylock 后备实现声明；定义位于公共排序/PI helper 之后，调用时自行管理 wait_lock。 */
static int __sched rt_mutex_slowtrylock(struct rt_mutex_base *lock);

/*
 * 调试构建中尝试获取 lock 的入口。
 * lock 是输入输出借用对象；不允许使用恒失败的 cmpxchg，而调用 rt_mutex_slowtrylock() 在 wait_lock
 * 下完成 owner 与调试检查。成功返回 true，竞争返回 false；只短暂持 raw spinlock，不调度睡眠。
 */
static __always_inline bool rt_mutex_try_acquire(struct rt_mutex_base *lock)
{
	/*
	 * With debug enabled rt_mutex_cmpxchg trylock() will always fail.
	 *
	 * Avoid unconditionally taking the slow path by using
	 * rt_mutex_slow_trylock() which is covered by the debug code and can
	 * acquire a non-contended rtmutex.
	 */
	/*
	 *
	 * 启用调试后 rt_mutex_cmpxchg trylock 恒失败。为避免所有无竞争获取也无条件走完整阻塞慢路径，
	 * 改用受调试代码覆盖的 rt_mutex_slowtrylock()，它仍能取得无竞争 rtmutex。
	 */
	return rt_mutex_slowtrylock(lock);
}

/*
 * 调试构建禁用无 wait_lock 的 release cmpxchg fastpath。
 * lock、old、new 均为借用输入且不访问；恒返回 false，使解锁进入 rt_mutex_slowunlock() 完成诊断。
 * 无字段或引用副作用，不能睡眠。
 */
static __always_inline bool rt_mutex_cmpxchg_release(struct rt_mutex_base *lock,
						     struct task_struct *old,
						     struct task_struct *new)
{
	return false;
}

/*
 * 调试构建中在已持 wait_lock 时直接置 HAS_WAITERS 位。
 * lock 是输入输出借用对象；所有 owner 访问已由 wait_lock 串行，无需原子 RMW。返回无直接值且不睡眠。
 */
static __always_inline void mark_rt_mutex_waiters(struct rt_mutex_base *lock)
	__must_hold(&lock->wait_lock)
{
	lock->owner = (struct task_struct *)
			((unsigned long)lock->owner | RT_MUTEX_HAS_WAITERS);
}

/*
 * Simple slow path only version: lock->owner is protected by lock->wait_lock.
 */
/*
 *
 * 这是仅有慢路径的简单版本：lock->owner 的全部访问均受 lock->wait_lock 保护。
 */
/*
 * 调试构建中清空 owner 并释放 wait_lock/恢复中断状态。
 * lock 是输入输出对象，flags 是对应 irqsave 快照；调用者进入时持锁，返回时必已释放。
 * 恒返回 true，表示无需处理 fastpath 窗口竞态；不直接唤醒 waiter且不能睡眠。
 */
static __always_inline bool unlock_rt_mutex_safe(struct rt_mutex_base *lock,
						 unsigned long flags)
	__releases(lock->wait_lock)
{
	lock->owner = NULL;
	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);
	return true;
}
#endif

/*
 * 把 task 的有效调度优先级转换成 RT mutex waiter 的主排序键。
 * task 是调用期间稳定的借用任务指针；函数只读 task->prio，不能睡眠且不要求在此取得引用。
 * RT/DL 任务保留数值优先级；其他调度类统一返回 DEFAULT_PRIO，使普通任务不参与细粒度 PI 排序。
 */
static __always_inline int __waiter_prio(struct task_struct *task)
{
	/* prio 是一次有效优先级快照；数值越小在 RT 排序中优先级越高。 */
	int prio = task->prio;

	if (!rt_or_dl_prio(prio))
		return DEFAULT_PRIO;

	return prio;
}

/*
 * Update the waiter->tree copy of the sort keys.
 */
/*
 *
 * 更新 waiter->tree 中属于“锁等待者树”的排序键副本。
 */
/*
 * 从 task 当前有效优先级/deadline 刷新尚未入 lock->waiters 的 waiter->tree。
 * waiter 是输入输出栈对象，task 是借用来源；调用者必须持 waiter->lock->wait_lock，tree 节点必须为空。
 * 返回：无直接返回值；只写 tree.prio/deadline，不入树、不改任务优先级且不能睡眠。
 */
static __always_inline void
waiter_update_prio(struct rt_mutex_waiter *waiter, struct task_struct *task)
{
	lockdep_assert_held(&waiter->lock->wait_lock);
	lockdep_assert(RB_EMPTY_NODE(&waiter->tree.entry));

	waiter->tree.prio = __waiter_prio(task);
	waiter->tree.deadline = task->dl.deadline;
}

/*
 * Update the waiter->pi_tree copy of the sort keys (from the tree copy).
 */
/*
 *
 * 从 tree 副本更新 waiter->pi_tree 中属于“owner PI 等待者树”的排序键副本。
 */
/*
 * 把 waiter->tree 已稳定的排序键复制到尚未入 task->pi_waiters 的 pi_tree 节点。
 * waiter 为输入输出对象，task 是目标 owner 借用指针；必须同时持 wait_lock 与 task->pi_lock，且
 * pi_tree 节点为空。返回无直接值，不重新读取 task 调度字段，从而让两棵树本次重排使用同一快照。
 */
static __always_inline void
waiter_clone_prio(struct rt_mutex_waiter *waiter, struct task_struct *task)
{
	lockdep_assert_held(&waiter->lock->wait_lock);
	lockdep_assert_held(&task->pi_lock);
	lockdep_assert(RB_EMPTY_NODE(&waiter->pi_tree.entry));

	waiter->pi_tree.prio = waiter->tree.prio;
	waiter->pi_tree.deadline = waiter->tree.deadline;
}

/*
 * Only use with rt_waiter_node_{less,equal}()
 */
/*
 *
 * 下列宏只可传给 rt_waiter_node_less()/equal() 做瞬时比较；复合字面量的生命周期限于当前完整作用域，
 * 不能保存指针或入树。task_to_waiter_node() 构造排序键，task_to_waiter() 再包装成临时 waiter。
 */
#define task_to_waiter_node(p)	\
	&(struct rt_waiter_node){ .prio = __waiter_prio(p), .deadline = (p)->dl.deadline }
#define task_to_waiter(p)	\
	&(struct rt_mutex_waiter){ .tree = *task_to_waiter_node(p) }

/*
 * 判断 left 排序键是否严格先于 right。
 * 两个参数均为只读借用节点；调用者负责用相应树锁稳定内容，函数不睡眠且无副作用。
 * 返回 1 表示 left 数值优先级更高，或二者同属 DL 优先级且 left deadline 更早；否则返回 0。
 */
static __always_inline int rt_waiter_node_less(struct rt_waiter_node *left,
					       struct rt_waiter_node *right)
{
	if (left->prio < right->prio)
		return 1;

	/*
	 * If both waiters have dl_prio(), we check the deadlines of the
	 * associated tasks.
	 * If left waiter has a dl_prio(), and we didn't return 1 above,
	 * then right waiter has a dl_prio() too.
	 */
	/*
	 *
	 * 两个 waiter 都是 DL 优先级时才继续比较关联任务的 deadline。若 left 为 DL 且上面的优先级
	 * 比较没有返回 1，则相同数值键保证 right 也是 DL；deadline 越早者排序越前。
	 */
	if (dl_prio(left->prio))
		return dl_time_before(left->deadline, right->deadline);

	return 0;
}

/*
 * 判断 left 与 right 是否具有相同的 RT mutex 排序等级。
 * 参数是只读借用排序键，调用者负责同步；返回 1 表示主 prio 相等，且在 DL 情况 deadline 也相等，
 * 否则返回 0。函数不修改节点、不睡眠，用于判断 PI 链是否仍需重排。
 */
static __always_inline int rt_waiter_node_equal(struct rt_waiter_node *left,
						 struct rt_waiter_node *right)
{
	if (left->prio != right->prio)
		return 0;

	/*
	 * If both waiters have dl_prio(), we check the deadlines of the
	 * associated tasks.
	 * If left waiter has a dl_prio(), and we didn't return 0 above,
	 * then right waiter has a dl_prio() too.
	 */
	/*
	 *
	 * 两个 waiter 都是 DL 优先级时还需比较 deadline；若 prio 已相同且 left 属于 DL，则 right 也属于
	 * DL。非 DL 任务只要归一化后的 prio 相同就属于同一排序等级。
	 */
	if (dl_prio(left->prio))
		return left->deadline == right->deadline;

	return 1;
}

/*
 * 判断 waiter 是否可越过当前 top_waiter 接管一个刚空闲的锁。
 * 两者均为受 wait_lock 稳定的只读借用 waiter；函数不改树、不睡眠。更高排序优先级恒可偷取；
 * RT_MUTEX_BUILD_SPINLOCKS 实例还允许普通任务同优先级横向偷取，其他实例同级返回 false。
 */
static inline bool rt_mutex_steal(struct rt_mutex_waiter *waiter,
				  struct rt_mutex_waiter *top_waiter)
{
	if (rt_waiter_node_less(&waiter->tree, &top_waiter->tree))
		return true;

#ifdef RT_MUTEX_BUILD_SPINLOCKS
	/*
	 * Note that RT tasks are excluded from same priority (lateral)
	 * steals to prevent the introduction of an unbounded latency.
	 */
	/*
	 *
	 * RT/DL 任务禁止同优先级横向偷取，否则新到任务可能反复越过已排队任务，引入无上界延迟；
	 * 普通任务在 RT spin/rwlock 替代实例中可同级接管，以减少不必要的睡眠/唤醒。
	 */
	if (rt_or_dl_prio(waiter->tree.prio))
		return false;

	return rt_waiter_node_equal(&waiter->tree, &top_waiter->tree);
#else
	return false;
#endif
}

/* 从嵌入的 tree.entry 红黑树节点恢复外层 rt_mutex_waiter 借用指针。 */
#define __node_2_waiter(node) \
	rb_entry((node), struct rt_mutex_waiter, tree.entry)

/*
 * lock->waiters 红黑树的严格排序回调。
 * a/b 是受目标 wait_lock 稳定的嵌入节点；返回 true 表示 a 应在 b 前，无副作用且不能睡眠。
 * 首先比较 prio/deadline；WW_RT 下完全同级时再以 acquire_ctx stamp 让更老事务靠前，NULL ctx 靠后。
 */
static __always_inline bool __waiter_less(struct rb_node *a, const struct rb_node *b)
{
	/* aw/bw 是从树节点恢复的只读 waiter 别名，生命周期仍由各阻塞任务栈帧承担。 */
	struct rt_mutex_waiter *aw = __node_2_waiter(a);
	struct rt_mutex_waiter *bw = __node_2_waiter(b);

	if (rt_waiter_node_less(&aw->tree, &bw->tree))
		return 1;

	if (!build_ww_mutex())
		return 0;

	if (rt_waiter_node_less(&bw->tree, &aw->tree))
		return 0;

	/* NOTE: relies on waiter->ww_ctx being set before insertion */
	/* 注意：这里依赖调用者在入树前已经设置 waiter->ww_ctx，否则 WW 同级次序会使用未初始化状态。 */
	if (aw->ww_ctx) {
		if (!bw->ww_ctx)
			return 1;

		return (signed long)(aw->ww_ctx->stamp -
				     bw->ww_ctx->stamp) < 0;
	}

	return 0;
}

/*
 * 把 waiter->tree 插入 lock 的 cached waiter 红黑树。
 * lock/waiter 均为输入输出借用对象；调用者必须持 wait_lock，waiter 排序键已初始化且节点尚未入树。
 * 返回无直接值；插入后 rb_leftmost 指向最高优先级 waiter，waiter 栈对象必须保持存活，函数不睡眠。
 */
static __always_inline void
rt_mutex_enqueue(struct rt_mutex_base *lock, struct rt_mutex_waiter *waiter)
	__must_hold(&lock->wait_lock)
{
	lockdep_assert_held(&lock->wait_lock);

	rb_add_cached(&waiter->tree.entry, &lock->waiters, __waiter_less);
}

/*
 * 将 waiter 从 lock->waiters 摘除并恢复为空节点状态。
 * 调用者必须持 wait_lock；参数均为输入输出借用对象。若节点本就为空则幂等返回；否则更新 cached
 * leftmost 并 RB_CLEAR_NODE，便于后续重排/释放。返回无直接值，不回收 waiter 且不能睡眠。
 */
static __always_inline void
rt_mutex_dequeue(struct rt_mutex_base *lock, struct rt_mutex_waiter *waiter)
	__must_hold(&lock->wait_lock)
{
	lockdep_assert_held(&lock->wait_lock);

	if (RB_EMPTY_NODE(&waiter->tree.entry))
		return;

	rb_erase_cached(&waiter->tree.entry, &lock->waiters);
	RB_CLEAR_NODE(&waiter->tree.entry);
}

/* 从任一 rt_waiter_node.entry 恢复外层排序节点，供 owner->pi_waiters 比较回调使用。 */
#define __node_2_rt_node(node) \
	rb_entry((node), struct rt_waiter_node, entry)

/*
 * task->pi_waiters 红黑树的排序回调。
 * a/b 是受 task->pi_lock 稳定的 pi_tree.entry 节点；返回 a 是否按 prio/deadline 严格先于 b。
 * 只委托通用键比较，不考虑 WW stamp，因为 owner 树只需每把锁的 top donor，函数无副作用且不睡眠。
 */
static __always_inline bool __pi_waiter_less(struct rb_node *a, const struct rb_node *b)
{
	return rt_waiter_node_less(__node_2_rt_node(a), __node_2_rt_node(b));
}

/*
 * 把某把锁的 top waiter 作为 donor 插入 owner task 的 pi_waiters 树。
 * task 是输入输出 owner，waiter 是借用栈对象；调用者必须持 task->pi_lock，pi_tree 节点应为空。
 * 返回无直接值；插入后 task 的 top PI donor 可改变，但本函数不调用调度器、不取得引用且不能睡眠。
 */
static __always_inline void
rt_mutex_enqueue_pi(struct task_struct *task, struct rt_mutex_waiter *waiter)
{
	lockdep_assert_held(&task->pi_lock);

	rb_add_cached(&waiter->pi_tree.entry, &task->pi_waiters, __pi_waiter_less);
}

/*
 * 从 task->pi_waiters 树摘除 waiter 的 donor 节点。
 * 参数均为输入输出借用对象且调用者持 task->pi_lock；空节点幂等返回，否则擦除并 RB_CLEAR_NODE。
 * 返回无直接值；只维护树，不自动重算 task 优先级、不释放 waiter 且不能睡眠。
 */
static __always_inline void
rt_mutex_dequeue_pi(struct task_struct *task, struct rt_mutex_waiter *waiter)
{
	lockdep_assert_held(&task->pi_lock);

	if (RB_EMPTY_NODE(&waiter->pi_tree.entry))
		return;

	rb_erase_cached(&waiter->pi_tree.entry, &task->pi_waiters);
	RB_CLEAR_NODE(&waiter->pi_tree.entry);
}

/*
 * 按 owner p 当前最高优先级 donor 调整其有效调度优先级。
 * lock 是 p 正持有的输入锁，p 是输入输出借用任务；调用者必须同时持 lock->wait_lock 与 p->pi_lock，
 * 且 owner 关系仍成立。无 donor 时向 rt_mutex_setprio() 传 NULL 以降回基础优先级；有 donor 时传其 task。
 * 返回无直接值；调度器会在 rq 锁下更新 p->prio/class/pi_top_task，函数不转移引用且不能睡眠。
 */
static __always_inline void rt_mutex_adjust_prio(struct rt_mutex_base *lock,
						 struct task_struct *p)
{
	/* pi_task 是最高优先级 waiter 的借用 task 指针；NULL 表示取消 PI boost。 */
	struct task_struct *pi_task = NULL;

	lockdep_assert_held(&lock->wait_lock);
	lockdep_assert(rt_mutex_owner(lock) == p);
	lockdep_assert_held(&p->pi_lock);

	if (task_has_pi_waiters(p))
		pi_task = task_top_pi_waiter(p)->task;

	rt_mutex_setprio(p, pi_task);
}

/* RT mutex specific wake_q wrappers */
/* RT mutex 专用 wake_q 包装：统一普通阻塞任务与 PREEMPT_RT sleeping spin/rwlock 的特殊唤醒。 */
/*
 * 把 task 按 wake_state 加入延迟唤醒容器 wqh。
 * wqh 是输入输出队列，task 是借用任务，wake_state 为 TASK_NORMAL 或 TASK_RTLOCK_WAIT；调用者通常仍
 * 持内部锁，故这里只排队不能直接唤醒。RT lock 路径取得 task 引用并存入唯一 rtlock_task 槽，
 * 普通路径由 wake_q_add() 管理队列引用。返回无直接值；引用在 rt_mutex_wake_up_q() 中消费，不能睡眠。
 */
static __always_inline void rt_mutex_wake_q_add_task(struct rt_wake_q_head *wqh,
						     struct task_struct *task,
						     unsigned int wake_state)
{
	if (IS_ENABLED(CONFIG_PREEMPT_RT) && wake_state == TASK_RTLOCK_WAIT) {
		/* 每次 owner 交接最多一个 RT lock top waiter；锁证明配置下检测重复填槽。 */
		if (IS_ENABLED(CONFIG_PROVE_LOCKING))
			WARN_ON_ONCE(wqh->rtlock_task);
		get_task_struct(task);
		wqh->rtlock_task = task;
	} else {
		wake_q_add(&wqh->head, task);
	}
}

/*
 * 从 waiter w 提取 task 与 wake_state，并加入 RT mutex 延迟唤醒队列。
 * wqh 为输入输出队列，w 是受 wait_lock 稳定的只读借用 waiter；返回无直接值，引用规则由
 * rt_mutex_wake_q_add_task() 承担。函数不出队 waiter、不直接唤醒且不能睡眠。
 */
static __always_inline void rt_mutex_wake_q_add(struct rt_wake_q_head *wqh,
						struct rt_mutex_waiter *w)
{
	rt_mutex_wake_q_add_task(wqh, w->task, w->wake_state);
}

/*
 * 在内部 wait_lock/pi_lock 均已释放后消费 wqh 中的所有延迟唤醒。
 * wqh 是输入输出队列且必须来自配对的 owner 交接；函数先唤醒专用 TASK_RTLOCK_WAIT 并归还其 task
 * 引用，再批量 wake 普通队列，最后 preempt_enable。返回无直接值，退出时队列为空且允许发生调度。
 */
static __always_inline void rt_mutex_wake_up_q(struct rt_wake_q_head *wqh)
{
	if (IS_ENABLED(CONFIG_PREEMPT_RT) && wqh->rtlock_task) {
		/* 专用槽的显式 get_task_struct() 在唤醒之后于此配对 put。 */
		wake_up_state(wqh->rtlock_task, TASK_RTLOCK_WAIT);
		put_task_struct(wqh->rtlock_task);
		wqh->rtlock_task = NULL;
	}

	if (!wake_q_empty(&wqh->head))
		wake_up_q(&wqh->head);

	/* Pairs with preempt_disable() in mark_wakeup_next_waiter() */
	/* 与 mark_wakeup_next_waiter() 的 preempt_disable() 配对，确保 donor 已入 wake_q 前不会被抢占。 */
	preempt_enable();
}

/*
 * Deadlock detection is conditional:
 *
 * If CONFIG_DEBUG_RT_MUTEXES=n, deadlock detection is only conducted
 * if the detect argument is == RT_MUTEX_FULL_CHAINWALK.
 *
 * If CONFIG_DEBUG_RT_MUTEXES=y, deadlock detection is always
 * conducted independent of the detect argument.
 *
 * If the waiter argument is NULL this indicates the deboost path and
 * deadlock detection is disabled independent of the detect argument
 * and the config settings.
 */
/*
 *
 * 死锁检测是有条件的：关闭 CONFIG_DEBUG_RT_MUTEXES 时，仅 chwalk==FULL 执行；开启调试时不理会
 * chwalk、只要 waiter 非 NULL 就执行。waiter==NULL 表示降优先级链路，无论配置和参数都关闭检测。
 */
/*
 * 把 waiter、chainwalk 模式和调试配置归并成“本轮是否检测死锁”的布尔决定。
 * waiter 是可空只读借用指针，chwalk 为 MIN/FULL 枚举；不要求取得对象引用，不修改状态且不能睡眠。
 * 返回 true 后链遍历即使无需重排也会继续查环；false 允许在 PI 已稳定时提前结束。
 */
static __always_inline bool
rt_mutex_cond_detect_deadlock(struct rt_mutex_waiter *waiter,
			      enum rtmutex_chainwalk chwalk)
{
	if (IS_ENABLED(CONFIG_DEBUG_RT_MUTEXES))
		return waiter != NULL;
	return chwalk == RT_MUTEX_FULL_CHAINWALK;
}

/*
 * 读取任务 p 当前因 PI 阻塞而等待的 rt_mutex_base。
 * p 是借用任务；调用者通常持 p->pi_lock 来稳定 pi_blocked_on。非阻塞返回 NULL，阻塞时返回 waiter
 * 中 lock 的借用指针。释放 pi_lock 后该返回值只能作身份比较，除非另有 wait_lock/引用保证生命周期。
 */
static __always_inline struct rt_mutex_base *task_blocked_on_lock(struct task_struct *p)
{
	return p->pi_blocked_on ? p->pi_blocked_on->lock : NULL;
}

/*
 * Adjust the priority chain. Also used for deadlock detection.
 * Decreases task's usage by one - may thus free the task.
 *
 * @task:	the task owning the mutex (owner) for which a chain walk is
 *		probably needed
 * @chwalk:	do we have to carry out deadlock detection?
 * @orig_lock:	the mutex (can be NULL if we are walking the chain to recheck
 *		things for a task that has just got its priority adjusted, and
 *		is waiting on a mutex)
 * @next_lock:	the mutex on which the owner of @orig_lock was blocked before
 *		we dropped its pi_lock. Is never dereferenced, only used for
 *		comparison to detect lock chain changes.
 * @orig_waiter: rt_mutex_waiter struct for the task that has just donated
 *		its priority to the mutex owner (can be NULL in the case
 *		depicted above or if the top waiter is gone away and we are
 *		actually deboosting the owner)
 * @top_task:	the current top waiter
 *
 * Returns 0 or -EDEADLK.
 *
 * Chain walk basics and protection scope
 *
 * [R] refcount on task
 * [Pn] task->pi_lock held
 * [L] rtmutex->wait_lock held
 *
 * Normal locking order:
 *
 *   rtmutex->wait_lock
 *     task->pi_lock
 *
 * Step	Description				Protected by
 *	function arguments:
 *	@task					[R]
 *	@orig_lock if != NULL			@top_task is blocked on it
 *	@next_lock				Unprotected. Cannot be
 *						dereferenced. Only used for
 *						comparison.
 *	@orig_waiter if != NULL			@top_task is blocked on it
 *	@top_task				current, or in case of proxy
 *						locking protected by calling
 *						code
 *	again:
 *	  loop_sanity_check();
 *	retry:
 * [1]	  lock(task->pi_lock);			[R] acquire [P1]
 * [2]	  waiter = task->pi_blocked_on;		[P1]
 * [3]	  check_exit_conditions_1();		[P1]
 * [4]	  lock = waiter->lock;			[P1]
 * [5]	  if (!try_lock(lock->wait_lock)) {	[P1] try to acquire [L]
 *	    unlock(task->pi_lock);		release [P1]
 *	    goto retry;
 *	  }
 * [6]	  check_exit_conditions_2();		[P1] + [L]
 * [7]	  requeue_lock_waiter(lock, waiter);	[P1] + [L]
 * [8]	  unlock(task->pi_lock);		release [P1]
 *	  put_task_struct(task);		release [R]
 * [9]	  check_exit_conditions_3();		[L]
 * [10]	  task = owner(lock);			[L]
 *	  get_task_struct(task);		[L] acquire [R]
 *	  lock(task->pi_lock);			[L] acquire [P2]
 * [11]	  requeue_pi_waiter(tsk, waiters(lock));[P2] + [L]
 * [12]	  check_exit_conditions_4();		[P2] + [L]
 * [13]	  unlock(task->pi_lock);		release [P2]
 *	  unlock(lock->wait_lock);		release [L]
 *	  goto again;
 *
 * Where P1 is the blocking task and P2 is the lock owner; going up one step
 * the owner becomes the next blocked task etc..
 *
*
 */
/*
 * 沿“任务等待锁、锁由下一任务持有”的 PI 链逐级提升/降低优先级，并可同时执行完整死锁检测。
 * 调用者：新 top waiter 入队、waiter 退出或 owner donor 改变后；后续由当前获取/清理路径继续。
 *
 * 入参和 ownership：
 * - task：首个待检查 owner 的持有引用；本函数必消费一次引用，可能在 put 后释放 task。
 * - chwalk：MIN 可在无需 PI 调整时停止，FULL 即使不重排也继续查环。
 * - orig_lock：触发遍历的原锁，可为 NULL；非空时由 top_task 阻塞关系间接稳定。
 * - next_lock：解锁前记录的 task 阻塞锁裸地址，仅作相等比较，绝不可解引用。
 * - orig_waiter：本次 donor waiter，可为 NULL 表示 deboost；非空由 top_task 阻塞状态稳定。
 * - top_task：发起整条链遍历的任务，普通路径为 current，proxy 路径由调用者保证存活。
 *
 * 入口不持 pi_lock/wait_lock，因此可抢占；每一级最多持阻塞任务 pi_lock 和下一把 wait_lock，使用
 * trylock 处理与常规 wait_lock→pi_lock 次序相反的获取。返回 0 表示链已稳定/结束，-EDEADLK 表示
 * 检出环或超过 max_lock_depth；退出时不持内部锁，task 输入引用已释放。副作用是重排两棵 waiter 树、
 * 调用 rt_mutex_setprio() 改变沿链任务的有效优先级，并可能唤醒空锁的新 top waiter。
 */
static int __sched rt_mutex_adjust_prio_chain(struct task_struct *task,
					      enum rtmutex_chainwalk chwalk,
					      struct rt_mutex_base *orig_lock,
					      struct rt_mutex_base *next_lock,
					      struct rt_mutex_waiter *orig_waiter,
					      struct task_struct *top_task)
{
	/*
	 * 变量地图：waiter 是当前 task 的阻塞节点；top_waiter 是上一层传播下来的 donor；
	 * prerequeue_top_waiter 保存重排前 lock top；lock 是当前 task 等待的下一把锁；depth 限制链长；
	 * detect_deadlock 决定是否继续查环，requeue=false 表示只查环而不再调整树和优先级。
	 */
	struct rt_mutex_waiter *waiter, *top_waiter = orig_waiter;
	struct rt_mutex_waiter *prerequeue_top_waiter;
	int ret = 0, depth = 0;
	struct rt_mutex_base *lock;
	bool detect_deadlock;
	bool requeue = true;

	detect_deadlock = rt_mutex_cond_detect_deadlock(orig_waiter, chwalk);

	/*
	 * The (de)boosting is a step by step approach with a lot of
	 * pitfalls. We want this to be preemptible and we want hold a
	 * maximum of two locks per step. So we have to check
	 * carefully whether things change under us.
	 */
	/*
	 *
	 * boost/deboost 必须逐级推进且充满竞态。为保持可抢占并把每步持锁数限制为两把，函数会频繁
	 * 放锁，因此每次重新进入都必须验证链是否已在并发解锁、重排或调度属性变化下改道。
	 */
 again:
	/*
	 * We limit the lock chain length for each invocation.
	 */
	/*
	 *
	 * 每次调用都限制 PI 链深度，既防损坏/异常环导致无限遍历，也限制最坏执行时间。
	 */
	if (++depth > max_lock_depth) {
		static int prev_max;

		/*
		 * Print this only once. If the admin changes the limit,
		 * print a new message when reaching the limit again.
		 */
		/*
		 *
		 * 对同一个 max_lock_depth 只打印一次；管理员改变上限后，首次再次触顶才输出新消息。
		 */
		if (prev_max != max_lock_depth) {
			prev_max = max_lock_depth;
			printk(KERN_WARNING "Maximum lock depth %d reached "
			       "task: %s (%d)\n", max_lock_depth,
			       top_task->comm, task_pid_nr(top_task));
		}
		/* 深度失败也必须消费当前层持有的 task 引用。 */
		put_task_struct(task);

		return -EDEADLK;
	}

	/*
	 * We are fully preemptible here and only hold the refcount on
	 * @task. So everything can have changed under us since the
	 * caller or our own code below (goto retry/again) dropped all
	 * locks.
	 */
	/*
	 *
	 * 此处完全可抢占，只持 task 引用；自调用者或本函数 goto retry/again 放掉全部锁以来，阻塞链的
	 * 字段和拓扑都可能改变，引用只保证 task 内存存在，不能保证状态不变。
	 */
 retry:
	/*
	 * [1] Task cannot go away as we did a get_task() before !
	 */
	/*
	 *
	 * [1] 先锁 task->pi_lock；进入本函数前取得的 task 引用保证加锁前对象不会消失。
	 */
	raw_spin_lock_irq(&task->pi_lock);

	/*
	 * [2] Get the waiter on which @task is blocked on.
	 */
	/*
	 *
	 * [2] 在 pi_lock 下读取 task 当前阻塞 waiter，得到的是受该锁稳定的借用指针。
	 */
	waiter = task->pi_blocked_on;

	/*
	 * [3] check_exit_conditions_1() protected by task->pi_lock.
	 */
	/*
	 *
	 * [3] 第一组退出条件只依赖 task->pi_lock 保护的阻塞关系。
	 */

	/*
	 * Check whether the end of the boosting chain has been
	 * reached or the state of the chain has changed while we
	 * dropped the locks.
	 */
	/*
	 *
	 * waiter 为空说明 boost 链已到末端，或放锁期间 task 已解除阻塞；无需继续传播。
	 */
	if (!waiter)
		goto out_unlock_pi;

	/*
	 * Check the orig_waiter state. After we dropped the locks,
	 * the previous owner of the lock might have released the lock.
	 */
	/*
	 *
	 * orig_waiter 非空时还要复核原锁 owner；放锁窗口内前一 owner 可能已经释放，原传播起点失效。
	 */
	if (orig_waiter && !rt_mutex_owner(orig_lock))
		goto out_unlock_pi;

	/*
	 * We dropped all locks after taking a refcount on @task, so
	 * the task might have moved on in the lock chain or even left
	 * the chain completely and blocks now on an unrelated lock or
	 * on @orig_lock.
	 *
	 * We stored the lock on which @task was blocked in @next_lock,
	 * so we can detect the chain change.
	 */
	/*
	 *
	 * 只持引用跨越无锁窗口时，task 可能沿链移动、完全离开或改为等待无关锁。next_lock 是此前保存的
	 * 地址哨兵；仅与当前 waiter->lock 比较即可发现拓扑变化，不能因地址相同之外的假设而解引用它。
	 */
	if (next_lock != waiter->lock)
		goto out_unlock_pi;

	/*
	 * There could be 'spurious' loops in the lock graph due to ww_mutex,
	 * consider:
	 *
	 *   P1: A, ww_A, ww_B
	 *   P2: ww_B, ww_A
	 *   P3: A
	 *
	 * P3 should not return -EDEADLK because it gets trapped in the cycle
	 * created by P1 and P2 (which will resolve -- and runs into
	 * max_lock_depth above). Therefore disable detect_deadlock such that
	 * the below termination condition can trigger once all relevant tasks
	 * are boosted.
	 *
	 * Even when we start with ww_mutex we can disable deadlock detection,
	 * since we would supress a ww_mutex induced deadlock at [6] anyway.
	 * Supressing it here however is not sufficient since we might still
	 * hit [6] due to adjustment driven iteration.
	 *
	 * NOTE: if someone were to create a deadlock between 2 ww_classes we'd
	 * utterly fail to report it; lockdep should.
	 */
	/*
	 *
	 * ww_mutex 可在锁图中形成会由 wound/die 协议自行消除的“假环”。示例中 P1/P2 的 WW 环会收敛，
	 * P3 只等待普通锁 A，不应因为被卷入该环而收到 -EDEADLK。因此 PREEMPT_RT 遇到带 ww_ctx 的 waiter
	 * 时关闭通用 deadlock 检测，让下方“PI 已稳定”条件结束遍历。即使起点就是 ww_mutex 也可如此，
	 * 因为 [6] 本来会抑制 WW 环；但这里只关闭仍不够，调整驱动的迭代仍可能到达 [6]，那里也要处理。
	 * 限制是跨两个 ww_class 的真实死锁不会由这里报告，必须依靠 lockdep。
	 */
	if (IS_ENABLED(CONFIG_PREEMPT_RT) && waiter->ww_ctx && detect_deadlock)
		detect_deadlock = false;

	/*
	 * Drop out, when the task has no waiters. Note,
	 * top_waiter can be NULL, when we are in the deboosting
	 * mode!
	 */
	/*
	 *
	 * top_waiter 非空表示 boost donor 仍需是 task 的最高 PI waiter；deboost 模式允许其为 NULL。
	 */
	if (top_waiter) {
		if (!task_has_pi_waiters(task))
			goto out_unlock_pi;
		/*
		 * If deadlock detection is off, we stop here if we
		 * are not the top pi waiter of the task. If deadlock
		 * detection is enabled we continue, but stop the
		 * requeueing in the chain walk.
		 */
		/*
		 *
		 * donor 已不是 task 的最高 PI waiter 时，关闭死锁检测可立即停止；开启检测则仍沿链查环，但将
		 * requeue 置 false，避免用已非主导的 donor 改写树和有效优先级。
		 */
		if (top_waiter != task_top_pi_waiter(task)) {
			if (!detect_deadlock)
				goto out_unlock_pi;
			else
				requeue = false;
		}
	}

	/*
	 * If the waiter priority is the same as the task priority
	 * then there is no further priority adjustment necessary.  If
	 * deadlock detection is off, we stop the chain walk. If its
	 * enabled we continue, but stop the requeueing in the chain
	 * walk.
	 */
	/*
	 *
	 * 当前 waiter 排序键已等于 task 有效优先级，说明无需继续 PI 调整。非检测模式在此结束；完整
	 * 检测模式继续遍历但停止重排，使“优先级已稳定”不掩盖更深处的锁环。
	 */
	if (rt_waiter_node_equal(&waiter->tree, task_to_waiter_node(task))) {
		if (!detect_deadlock)
			goto out_unlock_pi;
		else
			requeue = false;
	}

	/*
	 * [4] Get the next lock; per holding task->pi_lock we can't unblock
	 * and guarantee @lock's existence.
	 */
	/*
	 *
	 * [4] 在 pi_lock 下取得下一把 lock；task 此时不能解除阻塞，因此 waiter 与 lock 生命周期稳定。
	 */
	lock = waiter->lock;
	/*
	 * [5] We need to trylock here as we are holding task->pi_lock,
	 * which is the reverse lock order versus the other rtmutex
	 * operations.
	 *
	 * Per the above, holding task->pi_lock guarantees lock exists, so
	 * inverting this lock order is infeasible from a life-time
	 * perspective.
	 */
	/*
	 *
	 * [5] 常规次序是 wait_lock→pi_lock，此处已持 pi_lock，只能 trylock wait_lock；失败必须释放 pi_lock
	 * 后重试，不能阻塞等待形成 ABBA。pi_lock 已保证 lock 存活，因此无法通过先取长期引用来消除反序。
	 */
	if (!raw_spin_trylock(&lock->wait_lock)) {
		raw_spin_unlock_irq(&task->pi_lock);
		cpu_relax();
		goto retry;
	}

	/*
	 * [6] check_exit_conditions_2() protected by task->pi_lock and
	 * lock->wait_lock.
	 *
	 * Deadlock detection. If the lock is the same as the original
	 * lock which caused us to walk the lock chain or if the
	 * current lock is owned by the task which initiated the chain
	 * walk, we detected a deadlock.
	 */
	/*
	 *
	 * [6] 同时持 pi_lock 与 wait_lock 后检测闭环：下一把锁等于 orig_lock，或其 owner 回到 top_task，
	 * 都表示等待边形成环，先记录 -EDEADLK。
	 */
	if (lock == orig_lock || rt_mutex_owner(lock) == top_task) {
		ret = -EDEADLK;

		/*
		 * When the deadlock is due to ww_mutex; also see above. Don't
		 * report the deadlock and instead let the ww_mutex wound/die
		 * logic pick which of the contending threads gets -EDEADLK.
		 *
		 * NOTE: assumes the cycle only contains a single ww_class; any
		 * other configuration and we fail to report; also, see
		 * lockdep.
		 */
		/*
		 *
		 * 若环来自单一 ww_class，则不由通用 PI 代码报告；把结果恢复为 0，让 wound/die 选择真正应收到
		 * -EDEADLK 的事务。多 ww_class 环超出该假设，仍需 lockdep 检测。
		 */
		if (IS_ENABLED(CONFIG_PREEMPT_RT) && orig_waiter && orig_waiter->ww_ctx)
			ret = 0;

		raw_spin_unlock(&lock->wait_lock);
		goto out_unlock_pi;
	}

	/*
	 * If we just follow the lock chain for deadlock detection, no
	 * need to do all the requeue operations. To avoid a truckload
	 * of conditionals around the various places below, just do the
	 * minimum chain walk checks.
	 */
	/*
	 *
	 * requeue=false 表示当前只为死锁检测沿链前进，无需执行昂贵的双树重排。单独走最小检查分支，
	 * 避免在下方每个重排点散布条件判断。
	 */
	if (!requeue) {
		/*
		 * No requeue[7] here. Just release @task [8]
		 */
		/*
		 *
		 * 不执行 [7]，直接完成 [8]：释放阻塞任务 pi_lock，并消费当前 task 引用。
		 */
		raw_spin_unlock(&task->pi_lock);
		put_task_struct(task);

		/*
		 * [9] check_exit_conditions_3 protected by lock->wait_lock.
		 * If there is no owner of the lock, end of chain.
		 */
		/*
		 *
		 * [9] 只剩 wait_lock；若锁已无 owner，等待链到此终止，释放 wait_lock 后成功返回。
		 */
		if (!rt_mutex_owner(lock)) {
			raw_spin_unlock_irq(&lock->wait_lock);
			return 0;
		}

		/* [10] Grab the next task, i.e. owner of @lock */
		/* [10] wait_lock 稳定 owner，先取得其 task 引用再锁下一层 pi_lock。 */
		task = get_task_struct(rt_mutex_owner(lock));
		raw_spin_lock(&task->pi_lock);

		/*
		 * No requeue [11] here. We just do deadlock detection.
		 *
		 * [12] Store whether owner is blocked
		 * itself. Decision is made after dropping the locks
		 */
		/*
		 *
		 * 不做 [11] PI 树重排，只查环。[12] 记录新 owner 是否也阻塞，真正退出判断留到放锁后。
		 */
		next_lock = task_blocked_on_lock(task);
		/*
		 * Get the top waiter for the next iteration
		 */
		/*
		 *
		 * 保存当前锁 top waiter，作为下一层验证 donor 链是否仍连续的借用身份。
		 */
		top_waiter = rt_mutex_top_waiter(lock);

		/* [13] Drop locks */
		/* [13] 逆序释放 pi_lock 和 wait_lock，下一轮只携带 task 引用及不可解引用的地址快照。 */
		raw_spin_unlock(&task->pi_lock);
		raw_spin_unlock_irq(&lock->wait_lock);

		/* If owner is not blocked, end of chain. */
		/* 新 owner 未继续阻塞则链结束；out_put_task 消费刚取得的引用。 */
		if (!next_lock)
			goto out_put_task;
		goto again;
	}

	/*
	 * Store the current top waiter before doing the requeue
	 * operation on @lock. We need it for the boost/deboost
	 * decision below.
	 */
	/*
	 *
	 * 在重排当前 lock waiter 树前保存旧 top，用它判断 donor 是晋升、降级还是根本未变化。
	 */
	prerequeue_top_waiter = rt_mutex_top_waiter(lock);

	/* [7] Requeue the waiter in the lock waiter tree. */
	/* [7] 先摘除 waiter，节点为空后才能刷新排序键并按新位置重新插入。 */
	rt_mutex_dequeue(lock, waiter);

	/*
	 * Update the waiter prio fields now that we're dequeued.
	 *
	 * These values can have changed through either:
	 *
	 *   sys_sched_set_scheduler() / sys_sched_setattr()
	 *
	 * or
	 *
	 *   DL CBS enforcement advancing the effective deadline.
	 */
	/*
	 *
	 * waiter 出树后重新读取优先级键，因为 sys_sched_set_scheduler()/setattr() 可改变有效优先级，
	 * DL CBS enforcement 也可能推进有效 deadline；携旧键重入树会破坏 top waiter 与 PI donor 不变量。
	 */
	waiter_update_prio(waiter, task);

	rt_mutex_enqueue(lock, waiter);

	/*
	 * [8] Release the (blocking) task in preparation for
	 * taking the owner task in [10].
	 *
	 * Since we hold lock->waiter_lock, task cannot unblock, even if we
	 * release task->pi_lock.
	 */
	/*
	 *
	 * [8] 为 [10] 切换到 owner 任务，释放当前阻塞 task 的 pi_lock 和引用。原文 waiter_lock 在当前
	 * 结构中对应 lock->wait_lock；持有它可阻止 waiter 出队，因此放 pi_lock 后 task 仍不能解除阻塞。
	 */
	raw_spin_unlock(&task->pi_lock);
	put_task_struct(task);

	/*
	 * [9] check_exit_conditions_3 protected by lock->wait_lock.
	 *
	 * We must abort the chain walk if there is no lock owner even
	 * in the dead lock detection case, as we have nothing to
	 * follow here. This is the end of the chain we are walking.
	 */
	/*
	 *
	 * [9] 即使执行完整死锁检测，锁已无 owner 也没有下一条 owner→blocked 边可跟随，必须终止。
	 */
	if (!rt_mutex_owner(lock)) {
		/*
		 * If the requeue [7] above changed the top waiter,
		 * then we need to wake the new top waiter up to try
		 * to get the lock.
		 */
		/*
		 *
		 * [7] 重排若改变了空锁的 top waiter，要直接唤醒新 top 重新尝试接管，否则它可能继续睡眠而
		 * 没有 owner 再负责交接。
		 */
		top_waiter = rt_mutex_top_waiter(lock);
		if (prerequeue_top_waiter != top_waiter)
			wake_up_state(top_waiter->task, top_waiter->wake_state);
		raw_spin_unlock_irq(&lock->wait_lock);
		return 0;
	}

	/*
	 * [10] Grab the next task, i.e. the owner of @lock
	 *
	 * Per holding lock->wait_lock and checking for !owner above, there
	 * must be an owner and it cannot go away.
	 */
	/*
	 *
	 * [10] wait_lock 下已确认 owner 非空且稳定；取得下一 owner 的 task 引用后再锁其 pi_lock，
	 * 由此恢复常规 wait_lock→pi_lock 次序并为下一轮跨无锁窗口保活。
	 */
	task = get_task_struct(rt_mutex_owner(lock));
	raw_spin_lock(&task->pi_lock);

	/* [11] requeue the pi waiters if necessary */
	/* [11] 只有 lock 的 top waiter 身份改变时，才需同步 owner->pi_waiters 并重算 owner 优先级。 */
	if (waiter == rt_mutex_top_waiter(lock)) {
		/*
		 * The waiter became the new top (highest priority)
		 * waiter on the lock. Replace the previous top waiter
		 * in the owner tasks pi waiters tree with this waiter
		 * and adjust the priority of the owner.
		 */
		/*
		 *
		 * 当前 waiter 晋升为 lock 新 top：用它替换 owner PI 树中的旧 top donor，复制稳定排序键后
		 * 调用 rt_mutex_adjust_prio() 立即提升或调整 owner。
		 */
		rt_mutex_dequeue_pi(task, prerequeue_top_waiter);
		waiter_clone_prio(waiter, task);
		rt_mutex_enqueue_pi(task, waiter);
		rt_mutex_adjust_prio(lock, task);

	} else if (prerequeue_top_waiter == waiter) {
		/*
		 * The waiter was the top waiter on the lock, but is
		 * no longer the top priority waiter. Replace waiter in
		 * the owner tasks pi waiters tree with the new top
		 * (highest priority) waiter and adjust the priority
		 * of the owner.
		 * The new top waiter is stored in @waiter so that
		 * @waiter == @top_waiter evaluates to true below and
		 * we continue to deboost the rest of the chain.
		 */
		/*
		 *
		 * 当前 waiter 原是 top 但重排后降级：从 owner PI 树摘掉它，改挂 lock 的新 top 并调整 owner。
		 * 同时令 waiter 变量指向新 top，使下方 waiter==top_waiter，继续把 deboost 传播到更深 owner。
		 */
		rt_mutex_dequeue_pi(task, waiter);
		waiter = rt_mutex_top_waiter(lock);
		waiter_clone_prio(waiter, task);
		rt_mutex_enqueue_pi(task, waiter);
		rt_mutex_adjust_prio(lock, task);
	} else {
		/*
		 * Nothing changed. No need to do any priority
		 * adjustment.
		 */
		/*
		 *
		 * 重排未改变 top 身份，owner 所见 donor 不变，无需触碰 PI 树或调度优先级。
		 */
	}

	/*
	 * [12] check_exit_conditions_4() protected by task->pi_lock
	 * and lock->wait_lock. The actual decisions are made after we
	 * dropped the locks.
	 *
	 * Check whether the task which owns the current lock is pi
	 * blocked itself. If yes we store a pointer to the lock for
	 * the lock chain change detection above. After we dropped
	 * task->pi_lock next_lock cannot be dereferenced anymore.
	 */
	/*
	 *
	 * [12] 同时持两锁时记录当前 owner 是否也因 PI 阻塞；放掉 pi_lock 后 next_lock 仅是检测链变化的
	 * 地址哨兵，不能解引用。真正是否继续要在完全放锁后依据快照决定。
	 */
	next_lock = task_blocked_on_lock(task);
	/*
	 * Store the top waiter of @lock for the end of chain walk
	 * decision below.
	 */
	/*
	 *
	 * 保存重排后的 lock top waiter，用于判断当前传播 waiter 是否仍主导下一层。
	 */
	top_waiter = rt_mutex_top_waiter(lock);

	/* [13] Drop the locks */
	/* [13] 依次释放 owner pi_lock 与 lock wait_lock，恢复完全可抢占状态。 */
	raw_spin_unlock(&task->pi_lock);
	raw_spin_unlock_irq(&lock->wait_lock);

	/*
	 * Make the actual exit decisions [12], based on the stored
	 * values.
	 *
	 * We reached the end of the lock chain. Stop right here. No
	 * point to go back just to figure that out.
	 */
	/*
	 *
	 * [12] 快照显示 owner 未继续阻塞，链已结束；无需返回上一层再重复确认，直接释放 task 引用。
	 */
	if (!next_lock)
		goto out_put_task;

	/*
	 * If the current waiter is not the top waiter on the lock,
	 * then we can stop the chain walk here if we are not in full
	 * deadlock detection mode.
	 */
	/*
	 *
	 * 当前 waiter 已不是 lock top 时，其优先级不会继续影响下一层 owner；非完整检测模式可停止，
	 * 完整检测仍需沿地址链继续查环。
	 */
	if (!detect_deadlock && waiter != top_waiter)
		goto out_put_task;

	goto again;

 out_unlock_pi:
	/* 统一早退：此处仍持当前 task->pi_lock，先释放它再落入引用回收。 */
	raw_spin_unlock_irq(&task->pi_lock);
 out_put_task:
	/* 所有出口都消费本层 task 引用；调用者不得再依赖输入引用。 */
	put_task_struct(task);

	return ret;
}

/*
 * Try to take an rt-mutex
 *
 * Must be called with lock->wait_lock held and interrupts disabled
 *
 * @lock:   The lock to be acquired.
 * @task:   The task which wants to acquire the lock
 * @waiter: The waiter that is queued to the lock's wait tree if the
 *	    callsite called task_blocked_on_lock(), otherwise NULL
 */
/*
 * 在已持 wait_lock 且中断关闭时，尝试让 task 接管 rt_mutex，不等待也不执行 PI 链遍历。
 * 调用位置：slow trylock、已排队 waiter 的醒后重试及初始慢路径复查；成功后上层结束等待，失败继续阻塞。
 * lock 是输入输出借用锁；task 是期望 owner 的借用任务；waiter 可为 NULL 表示未排队 trylock，否则必须
 * 已在 lock->waiters 且属于 task。返回 1 表示已成为 owner，返回 0 表示仍有 owner或不具备 top/steal
 * 资格。函数不能睡眠；成功会按需出树、清 task->pi_blocked_on、建立新 owner PI donor 树并发布 owner。
 * 无论成败都会先置 HAS_WAITERS，失败或无剩余 waiter 时由调用者随后 fixup。
 */
static int __sched
try_to_take_rt_mutex(struct rt_mutex_base *lock, struct task_struct *task,
		     struct rt_mutex_waiter *waiter)
	__must_hold(&lock->wait_lock)
{
	lockdep_assert_held(&lock->wait_lock);

	/*
	 * Before testing whether we can acquire @lock, we set the
	 * RT_MUTEX_HAS_WAITERS bit in @lock->owner. This forces all
	 * other tasks which try to modify @lock into the slow path
	 * and they serialize on @lock->wait_lock.
	 *
	 * The RT_MUTEX_HAS_WAITERS bit can have a transitional state
	 * as explained at the top of this file if and only if:
	 *
	 * - There is a lock owner. The caller must fixup the
	 *   transient state if it does a trylock or leaves the lock
	 *   function due to a signal or timeout.
	 *
	 * - @task acquires the lock and there are no other
	 *   waiters. This is undone in rt_mutex_set_owner(@task) at
	 *   the end of this function.
	 */
	/*
	 *
	 * 检查能否获取前先置 HAS_WAITERS，强制其他 owner 修改者进入 wait_lock 慢路径。该位允许两种暂态：
	 * 已有 owner 时，trylock/信号/超时退出者必须随后 fixup；task 成功但树已无其他 waiter 时，末尾
	 * rt_mutex_set_owner(task) 会按空树清位。先置位是检查 owner 与操作队列之间的串行化边界。
	 */
	mark_rt_mutex_waiters(lock);

	/*
	 * If @lock has an owner, give up.
	 */
	/*
	 *
	 * owner 非空时不能接管；保留刚置的过渡位，交由外层统一修复，返回 0。
	 */
	if (rt_mutex_owner(lock))
		return 0;

	/*
	 * If @waiter != NULL, @task has already enqueued the waiter
	 * into @lock waiter tree. If @waiter == NULL then this is a
	 * trylock attempt.
	 */
	/*
	 *
	 * waiter 非空表示 task 已排入 lock 树；NULL 表示本次是不带入队状态的 trylock，两路资格规则不同。
	 */
	if (waiter) {
		/* top_waiter 是 wait_lock 下稳定的当前最高优先级排队者。 */
		struct rt_mutex_waiter *top_waiter = rt_mutex_top_waiter(lock);

		/*
		 * If waiter is the highest priority waiter of @lock,
		 * or allowed to steal it, take it over.
		 */
		/*
		 *
		 * 已排队 waiter 只有本身是 top，或排序严格优于/配置允许横向 steal 时才能接管。
		 */
		if (waiter == top_waiter || rt_mutex_steal(waiter, top_waiter)) {
			/*
			 * We can acquire the lock. Remove the waiter from the
			 * lock waiters tree.
			 */
			/*
			 *
			 * 资格成立后先从 lock 树摘除本 waiter；其栈对象仍由调用者持有，稍后清 blocked_on。
			 */
			rt_mutex_dequeue(lock, waiter);
		} else {
			return 0;
		}
	} else {
		/*
		 * If the lock has waiters already we check whether @task is
		 * eligible to take over the lock.
		 *
		 * If there are no other waiters, @task can acquire
		 * the lock.  @task->pi_blocked_on is NULL, so it does
		 * not need to be dequeued.
		 */
		/*
		 *
		 * NULL waiter 的 trylock 若树中已有 waiter，必须证明 task 可越过 top；若树为空则可直接获取。
		 * 该 task 从未排队，pi_blocked_on 应为 NULL，无需执行出树动作。
		 */
		if (rt_mutex_has_waiters(lock)) {
			/* Check whether the trylock can steal it. */
			/* 用 task 调度键构造只在本表达式有效的临时 waiter，检查其 steal 资格。 */
			if (!rt_mutex_steal(task_to_waiter(task),
					    rt_mutex_top_waiter(lock)))
				return 0;

			/*
			 * The current top waiter stays enqueued. We
			 * don't have to change anything in the lock
			 * waiters order.
			 */
			/*
			 *
			 * trylock 偷取成功也不摘除原 top；它仍等待新 owner，锁树次序无需改变。
			 */
		} else {
			/*
			 * No waiters. Take the lock without the
			 * pi_lock dance.@task->pi_blocked_on is NULL
			 * and we have no waiters to enqueue in @task
			 * pi waiters tree.
			 */
			/*
			 *
			 * 无 waiter 时直接跳到发布 owner：task 未阻塞，也没有 top donor 要挂入其 PI 树，省去 pi_lock。
			 */
			goto takeit;
		}
	}

	/*
	 * Clear @task->pi_blocked_on. Requires protection by
	 * @task->pi_lock. Redundant operation for the @waiter == NULL
	 * case, but conditionals are more expensive than a redundant
	 * store.
	 */
	/*
	 *
	 * 接管前在 task->pi_lock 下清 pi_blocked_on，表示它不再等待该锁。NULL waiter 路径本来就是 NULL，
	 * 但无条件写一次比增加热路径分支更便宜，并保持两路汇合后的统一状态。
	 */
	raw_spin_lock(&task->pi_lock);
	task->pi_blocked_on = NULL;
	/*
	 * Finish the lock acquisition. @task is the new owner. If
	 * other waiters exist we have to insert the highest priority
	 * waiter into @task->pi_waiters tree.
	 */
	/*
	 *
	 * 在同一 pi_lock 临界区完成新 owner 的 donor 建模：若锁仍有 waiter，把其 top 挂入 task PI 树，
	 * 使后续调度优先级调整能看到该 donor。这里不额外 clone，因为 top 的 pi_tree 键已在排队时准备。
	 */
	if (rt_mutex_has_waiters(lock))
		rt_mutex_enqueue_pi(task, rt_mutex_top_waiter(lock));
	raw_spin_unlock(&task->pi_lock);

takeit:
	/*
	 * This either preserves the RT_MUTEX_HAS_WAITERS bit if there
	 * are still waiters or clears it.
	 */
	/*
	 *
	 * 最终以 acquire 语义发布 task 为 owner；编码 helper 会在树仍非空时保留 HAS_WAITERS，否则清除
	 * 先前过渡位。成功边界之后调用者可进入临界区。
	 */
	rt_mutex_set_owner(lock, task);

	return 1;
}

/*
 * Task blocks on lock.
 *
 * Prepare waiter and propagate pi chain
 *
 * This must be called with lock->wait_lock held and interrupts disabled
 */
/*
 * 把 task 的预初始化 waiter 挂到 lock，并把新 donor 优先级传播到现 owner 的 PI 链。
 * 调用者已持 lock->wait_lock 且中断关闭；函数返回时重新持有同一锁，可短暂放锁执行可抢占 chainwalk。
 * lock 是输入输出借用锁；waiter 是 task 栈上输入输出节点，返回 0 后保持排队；task 是阻塞任务借用指针；
 * ww_ctx 可空，非空启用 wound/wait 立即退避；chwalk 选择死锁检测强度；wake_q 收集放锁后才可唤醒的任务。
 * 返回 0 表示 waiter 已成功建立且 PI 已传播或无需传播；返回 -EDEADLK/WW 错误表示不得继续等待，
 * 相应早期失败会撤销本函数的入队。副作用包括设置 task->pi_blocked_on、更新两棵树和 owner 有效优先级。
 */
static int __sched task_blocks_on_rt_mutex(struct rt_mutex_base *lock,
					   struct rt_mutex_waiter *waiter,
					   struct task_struct *task,
					   struct ww_acquire_ctx *ww_ctx,
					   enum rtmutex_chainwalk chwalk,
					   struct wake_q_head *wake_q)
	__must_hold(&lock->wait_lock)
{
	/*
	 * owner 是入口 owner 快照；top_waiter 是入队前 top（空树时暂以当前 waiter 作哨兵）；next_lock
	 * 只记录 owner 是否继续阻塞；chain_walk 决定是否传播，res 传递 WW/死锁结果。
	 */
	struct task_struct *owner = rt_mutex_owner(lock);
	struct rt_mutex_waiter *top_waiter = waiter;
	struct rt_mutex_base *next_lock;
	int chain_walk = 0, res;

	lockdep_assert_held(&lock->wait_lock);

	/*
	 * Early deadlock detection. We really don't want the task to
	 * enqueue on itself just to untangle the mess later. It's not
	 * only an optimization. We drop the locks, so another waiter
	 * can come in before the chain walk detects the deadlock. So
	 * the other will detect the deadlock and return -EDEADLOCK,
	 * which is wrong, as the other waiter is not in a deadlock
	 * situation.
	 *
	 * Except for ww_mutex, in that case the chain walk must already deal
	 * with spurious cycles, see the comments at [3] and [6].
	 */
	/*
	 *
	 * 普通 rtmutex 若 owner 就是 task，必须在入队前立即返回 -EDEADLK。这不仅是优化：若先入队再放锁，
	 * 另一 waiter 可能先在 chainwalk 中撞到该自环并错误地替真正自锁者返回死锁。WW mutex 例外，
	 * 因为其合法暂态会产生假环，必须交给链遍历 [3]/[6] 与 wound/die 协议处理。
	 */
	if (owner == task && !(build_ww_mutex() && ww_ctx))
		return -EDEADLK;

	/* 阶段一：同时持 wait_lock 与 task pi_lock，完整初始化 waiter 的身份和两棵树排序键。 */
	raw_spin_lock(&task->pi_lock);
	waiter->task = task;
	waiter->lock = lock;
	waiter_update_prio(waiter, task);
	waiter_clone_prio(waiter, task);

	/* Get the top priority waiter on the lock */
	/* 保存入队前 top donor，稍后若当前 waiter 晋升为 top，要在 owner PI 树中替换它。 */
	if (rt_mutex_has_waiters(lock))
		top_waiter = rt_mutex_top_waiter(lock);
	rt_mutex_enqueue(lock, waiter);

	/* 发布 task→waiter 阻塞关系；pi_lock 保护并供 chainwalk 从任务追到下一把锁。 */
	task->pi_blocked_on = waiter;

	raw_spin_unlock(&task->pi_lock);

	if (build_ww_mutex() && ww_ctx) {
		/* rtm 是从公共 rtmutex 子对象恢复的 WW 后端容器借用别名。 */
		struct rt_mutex *rtm;

		/* Check whether the waiter should back out immediately */
		/* WW helper 按 stamp/策略判断该事务是否应立即退避，并可把需唤醒任务加入 wake_q。 */
		rtm = container_of(lock, struct rt_mutex, rtmutex);
		__assume_ctx_lock(&rtm->rtmutex.wait_lock);
		res = __ww_mutex_add_waiter(waiter, rtm, ww_ctx, wake_q);
		if (res) {
			/* 立即退避尚未进入 owner PI 树，只需在 task pi_lock 下撤销 lock 树节点和 blocked_on。 */
			raw_spin_lock(&task->pi_lock);
			rt_mutex_dequeue(lock, waiter);
			task->pi_blocked_on = NULL;
			raw_spin_unlock(&task->pi_lock);
			return res;
		}
	}

	/* 入队期间锁若已无 owner，就没有可 boost 的任务；waiter 保持排队并等待接管空锁。 */
	if (!owner)
		return 0;

	/* 阶段二：wait_lock 保证 owner 存活，再取 owner->pi_lock 同步其 donor 树。 */
	raw_spin_lock(&owner->pi_lock);
	if (waiter == rt_mutex_top_waiter(lock)) {
		/* 新 waiter 成为 top：替换旧 top donor，并按新的最高 donor 立即调整 owner 有效优先级。 */
		rt_mutex_dequeue_pi(owner, top_waiter);
		rt_mutex_enqueue_pi(owner, waiter);

		rt_mutex_adjust_prio(lock, owner);
		/* owner 自身也阻塞时，刚发生的 boost 必须继续传播到下一 owner。 */
		if (owner->pi_blocked_on)
			chain_walk = 1;
	} else if (rt_mutex_cond_detect_deadlock(waiter, chwalk)) {
		/* 未成为 top 时无需 PI 传播，但 FULL/debug 模式仍要沿链检查死锁。 */
		chain_walk = 1;
	}

	/* Store the lock on which owner is blocked or NULL */
	/* pi_lock 下保存下一把锁身份；放锁后仅用于链变化比较，不能解引用。 */
	next_lock = task_blocked_on_lock(owner);

	raw_spin_unlock(&owner->pi_lock);
	/*
	 * Even if full deadlock detection is on, if the owner is not
	 * blocked itself, we can avoid finding this out in the chain
	 * walk.
	 */
	/*
	 *
	 * 即使请求 FULL 检测，owner 自身不阻塞也已经是链尾，无需进入昂贵遍历；chain_walk=false 同理。
	 */
	if (!chain_walk || !next_lock)
		return 0;

	/*
	 * The owner can't disappear while holding a lock,
	 * so the owner struct is protected by wait_lock.
	 * Gets dropped in rt_mutex_adjust_prio_chain()!
	 */
	/*
	 *
	 * owner 持锁期间不会消失，当前 wait_lock 稳定其指针；但即将放掉 wait_lock 跨入可抢占遍历，必须
	 * 先取得 task 引用。该引用的释放责任转移给 rt_mutex_adjust_prio_chain()。
	 */
	get_task_struct(owner);

	/* 阶段三：放 wait_lock 并处理普通 wake_q，chainwalk 内部按每级至多两锁规则推进。 */
	raw_spin_unlock_irq_wake(&lock->wait_lock, wake_q);

	res = rt_mutex_adjust_prio_chain(owner, chwalk, lock,
					 next_lock, waiter, task);

	/* 恢复函数入口锁契约：无论遍历结果如何，返回前重新取得 lock->wait_lock。 */
	raw_spin_lock_irq(&lock->wait_lock);

	return res;
}

/*
 * Remove the top waiter from the current tasks pi waiter tree and
 * queue it up.
 *
 * Called with lock->wait_lock held and interrupts disabled.
 */
/*
 * 在 unlock 交接中把当前 top waiter 从 current 的 PI donor 树摘除、降低 current 优先级并延迟唤醒它。
 * 调用者必须是 lock owner，持 lock->wait_lock 且中断关闭；wqh 是输入输出延迟唤醒队列，lock 是
 * 输入输出借用锁。返回无直接值；退出仍持 wait_lock，但 current->pi_lock 已释放、owner 被写成
 * NULL|HAS_WAITERS，并保持抢占关闭。后续必须释放 wait_lock 并调用 rt_mutex_wake_up_q() 配对唤醒/
 * preempt_enable。函数不睡眠，top waiter 仍留在 lock 树直到它实际接管。
 */
static void __sched mark_wakeup_next_waiter(struct rt_wake_q_head *wqh,
					    struct rt_mutex_base *lock)
	__must_hold(&lock->wait_lock)
{
	/* waiter 是 wait_lock 下稳定的当前 top donor 借用指针。 */
	struct rt_mutex_waiter *waiter;

	lockdep_assert_held(&lock->wait_lock);

	/* 同时持 wait_lock 与 owner pi_lock，原子更新 donor 树和 owner 有效优先级。 */
	raw_spin_lock(&current->pi_lock);

	waiter = rt_mutex_top_waiter(lock);

	/*
	 * Remove it from current->pi_waiters and deboost.
	 *
	 * We must in fact deboost here in order to ensure we call
	 * rt_mutex_setprio() to update p->pi_top_task before the
	 * task unblocks.
	 */
	/*
	 *
	 * 从 current->pi_waiters 摘除即将唤醒的 top，并在唤醒前实际 deboost。必须在此调用 setprio 更新
	 * p->pi_top_task，使 donor 开始运行时 owner 不再错误指向已解除阻塞的任务。
	 */
	rt_mutex_dequeue_pi(current, waiter);
	rt_mutex_adjust_prio(lock, current);

	/*
	 * As we are waking up the top waiter, and the waiter stays
	 * queued on the lock until it gets the lock, this lock
	 * obviously has waiters. Just set the bit here and this has
	 * the added benefit of forcing all new tasks into the
	 * slow path making sure no task of lower priority than
	 * the top waiter can steal this lock.
	 */
	/*
	 *
	 * top waiter 在真正获取前仍留在 lock 树，所以锁显然仍有 waiter。把 owner 直接写为 NULL|WAITERS
	 * 既表示当前 owner 已释放，又迫使新到任务进入慢路径，防止低优先级任务越过 top waiter 偷锁。
	 */
	lock->owner = (void *) RT_MUTEX_HAS_WAITERS;

	/*
	 * We deboosted before waking the top waiter task such that we don't
	 * run two tasks with the 'same' priority (and ensure the
	 * p->pi_top_task pointer points to a blocked task). This however can
	 * lead to priority inversion if we would get preempted after the
	 * deboost but before waking our donor task, hence the preempt_disable()
	 * before unlock.
	 *
	 * Pairs with preempt_enable() in rt_mutex_wake_up_q();
	 */
	/*
	 *
	 * 先 deboost 再唤醒避免 owner 与 donor 同时以“相同 boost 优先级”运行；但若 deboost 后、donor 入
	 * wake_q 前被抢占，会产生新的优先级反转。因此在放 pi_lock 前禁抢占，直至 wake_up_q() 唤醒后
	 * 配对 preempt_enable()。
	 */
	preempt_disable();
	rt_mutex_wake_q_add(wqh, waiter);
	raw_spin_unlock(&current->pi_lock);
}

/*
 * 在已持 wait_lock 时执行不排队的慢速 trylock 核心。
 * lock 是输入输出借用对象，调用者关闭中断且持 wait_lock；返回 1 表示 current 成为 owner，0 表示竞争。
 * try_to_take 无条件置 waiter 位，所以无论结果都按获取路径修复空树过渡位；函数不睡眠、不放锁。
 */
static int __sched __rt_mutex_slowtrylock(struct rt_mutex_base *lock)
	__must_hold(&lock->wait_lock)
{
	int ret = try_to_take_rt_mutex(lock, current, NULL);

	/*
	 * try_to_take_rt_mutex() sets the lock waiters bit
	 * unconditionally. Clean this up.
	 */
	/*
	 *
	 * try_to_take_rt_mutex() 无条件置 waiter 位；trylock 不入树，必须在返回前清理该暂态。
	 */
	fixup_rt_mutex_waiters(lock, true);

	return ret;
}

/*
 * Slow path try-lock function:
 */
/*
 * 慢速 trylock 外层：在无 owner 快照时取得 wait_lock 后复核并尝试接管。
 */
/*
 * 为 current 尝试获取 lock，覆盖 debug/无 cmpxchg 与 fastpath 失败场景。
 * lock 是输入输出借用锁；入口不持 wait_lock，可在早期启动调用。返回 1 成功、0 竞争；不排队、不睡眠。
 * 无锁 owner 预检只允许快速失败，真正成功由 irqsave wait_lock 临界区内 __rt_mutex_slowtrylock 决定。
 */
static int __sched rt_mutex_slowtrylock(struct rt_mutex_base *lock)
{
	unsigned long flags;
	int ret;

	/*
	 * If the lock already has an owner we fail to get the lock.
	 * This can be done without taking the @lock->wait_lock as
	 * it is only being read, and this is a trylock anyway.
	 */
	/*
	 *
	 * owner 快照非空可直接失败；即使结果稍后变化也符合 trylock 的瞬时语义，无需为失败取得 wait_lock。
	 */
	if (rt_mutex_owner(lock))
		return 0;

	/*
	 * The mutex has currently no owner. Lock the wait lock and try to
	 * acquire the lock. We use irqsave here to support early boot calls.
	 */
	/*
	 *
	 * 快照为空时必须在 wait_lock 下复核/接管。使用 irqsave 而非无条件关开中断，兼容早期启动时
	 * 调用者原本就关闭中断的情况。
	 */
	raw_spin_lock_irqsave(&lock->wait_lock, flags);

	ret = __rt_mutex_slowtrylock(lock);

	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);

	return ret;
}

/*
 * rtmutex trylock 的 fast/slow 汇合入口。
 * lock 是输入输出借用对象，调用者不得已持该锁；返回 1 表示 current 获得 ownership，0 表示未获取。
 * 先用 acquire cmpxchg 处理 NULL/0，无竞争成功不取 wait_lock；失败调用 slowtrylock 复核 tagged owner、
 * waiter 公平性与调试状态。函数不排队且不调度睡眠。
 */
static __always_inline int __rt_mutex_trylock(struct rt_mutex_base *lock)
{
	if (likely(rt_mutex_cmpxchg_acquire(lock, NULL, current)))
		return 1;

	return rt_mutex_slowtrylock(lock);
}

/*
 * Slow path to release a rt-mutex.
 */
/*
 * 释放 rt-mutex 的慢路径。
 */
/*
 * 在 fast release 失败后释放 current 持有的 lock，并在有 waiter 时完成 PI deboost 和 top 交接。
 * lock 是输入输出借用对象；入口不持 wait_lock，允许早期启动调用，函数可能在最终 preempt_enable 时
 * 触发调度但不作普通阻塞等待。返回无直接值；无 waiter 时安全清 owner，有 waiter 时把 top 加入
 * RT wake_q，释放内部锁后唤醒。成功出口 lock 不再由 current 持有，所有临时锁/抢占状态已恢复。
 */
static void __sched rt_mutex_slowunlock(struct rt_mutex_base *lock)
{
	/* wqh 延迟锁外唤醒；flags 保存进入 wait_lock 前的中断状态。 */
	DEFINE_RT_WAKE_Q(wqh);
	unsigned long flags;

	/* irqsave required to support early boot calls */
	/* 早期启动调用可能原已关中断，必须保存/恢复而非无条件 enable。 */
	raw_spin_lock_irqsave(&lock->wait_lock, flags);

	debug_rt_mutex_unlock(lock);

	/*
	 * We must be careful here if the fast path is enabled. If we
	 * have no waiters queued we cannot set owner to NULL here
	 * because of:
	 *
	 * foo->lock->owner = NULL;
	 *			rtmutex_lock(foo->lock);   <- fast path
	 *			free = atomic_dec_and_test(foo->refcnt);
	 *			rtmutex_unlock(foo->lock); <- fast path
	 *			if (free)
	 *				kfree(foo);
	 * raw_spin_unlock(foo->lock->wait_lock);
	 *
	 * So for the fastpath enabled kernel:
	 *
	 * Nothing can set the waiters bit as long as we hold
	 * lock->wait_lock. So we do the following sequence:
	 *
	 *	owner = rt_mutex_owner(lock);
	 *	clear_rt_mutex_waiters(lock);
	 *	raw_spin_unlock(&lock->wait_lock);
	 *	if (cmpxchg(&lock->owner, owner, 0) == owner)
	 *		return;
	 *	goto retry;
	 *
	 * The fastpath disabled variant is simple as all access to
	 * lock->owner is serialized by lock->wait_lock:
	 *
	 *	lock->owner = NULL;
	 *	raw_spin_unlock(&lock->wait_lock);
	 */
	/*
	 *
	 * fastpath 开启时，即便树为空也不能在仍持 wait_lock 时先写 owner=NULL：另一 CPU 可 fast acquire、
	 * 使用并释放包含该锁的对象，甚至 kfree，而本 CPU 随后还要访问 wait_lock，导致 UAF。安全序列是
	 * 清 waiter 位、释放 wait_lock，再用 owner 精确 cmpxchg 清空；若窗口中新 waiter 置位，cmpxchg
	 * 失败并重试慢路径。禁用 fastpath 时所有 owner 访问均受 wait_lock，可直接清空再放锁。
	 */
	while (!rt_mutex_has_waiters(lock)) {
		/* Drops lock->wait_lock ! */
		/* helper 无条件释放 wait_lock；true 已完成，false 表示新 waiter 抢先置位。 */
		if (unlock_rt_mutex_safe(lock, flags) == true)
			return;
		/* Relock the rtmutex and try again */
		/* cmpxchg 失败后重新取得锁并复查树，可能转入 top waiter 交接。 */
		raw_spin_lock_irqsave(&lock->wait_lock, flags);
	}

	trace_contended_release(lock);
	/*
	 * The wakeup next waiter path does not suffer from the above
	 * race. See the comments there.
	 *
	 * Queue the next waiter for wakeup once we release the wait_lock.
	 */
	/*
	 *
	 * 有 waiter 时不存在上述“树为空开放 fastpath”窗口：保持 WAITERS 位并在当前 owner PI 树中摘除
	 * top，待释放 wait_lock 后再唤醒，避免在内部锁下调度。
	 */
	mark_wakeup_next_waiter(&wqh, lock);
	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);

	/* 锁外完成唤醒、引用归还和与 mark_wakeup_next_waiter 配对的抢占恢复。 */
	rt_mutex_wake_up_q(&wqh);
}

/*
 * rtmutex unlock 的 fast/slow 汇合入口。
 * lock 必须由 current 持有且为输入输出借用对象；返回无直接值。若 owner 精确为 current/0，release
 * cmpxchg 直接清空并发布临界区写入；waiter 位、调试构建或竞态令其失败时转 slowunlock 完成交接。
 */
static __always_inline void __rt_mutex_unlock(struct rt_mutex_base *lock)
{
	if (likely(rt_mutex_cmpxchg_release(lock, current, NULL)))
		return;

	rt_mutex_slowunlock(lock);
}

#ifdef CONFIG_SMP
/*
 * 在 SMP 上对仍在 CPU 运行的 owner 做有限自适应自旋，避免立即睡眠/唤醒开销。
 * lock 是只读竞争锁，waiter 是 current 已排队节点，owner 是先前在 wait_lock 下取得的裸指针；调用者
 * 已释放 wait_lock。函数在 RCU 读侧保护 owner 内存，但不冻结 owner 字段；返回 true 表示 owner 已
 * 改变、调用者应立即重试获取，false 表示 owner 下线/本 waiter 失去 top/需调度/VCPU 被抢占而应睡眠。
 * 不修改锁或 waiter，不取得长期引用，不可睡眠。
 */
static bool rtmutex_spin_on_owner(struct rt_mutex_base *lock,
				  struct rt_mutex_waiter *waiter,
				  struct task_struct *owner)
{
	/* res 初始表示“继续重试”，遇到不宜自旋条件改为 false。 */
	bool res = true;

	/* RCU 只保证匹配 owner 的 task_struct 内存暂不释放，不保证它仍为 owner。 */
	rcu_read_lock();
	for (;;) {
		/* If owner changed, trylock again. */
		/* owner 身份一旦改变就停止自旋并返回 true，让外层重新取得 wait_lock/尝试接管。 */
		if (owner != rt_mutex_owner(lock))
			break;
		/*
		 * Ensure that @owner is dereferenced after checking that
		 * the lock owner still matches @owner. If that fails,
		 * @owner might point to freed memory. If it still matches,
		 * the rcu_read_lock() ensures the memory stays valid.
		 */
		/*
		 *
		 * 编译器屏障保证只有先确认 lock 当前 owner 仍等于 @owner，之后才解引用 owner 字段；若倒序，
		 * owner 可能已是悬空指针。匹配时 RCU 读锁只负责内存生命周期，后续循环仍需持续复核身份。
		 */
		barrier();
		/*
		 * Stop spinning when:
		 *  - the lock owner has been scheduled out
		 *  - current is not longer the top waiter
		 *  - current is requested to reschedule (redundant
		 *    for CONFIG_PREEMPT_RCU=y)
		 *  - the VCPU on which owner runs is preempted
		 */
		/*
		 *
		 * owner 已离开 CPU、自身不再是 top waiter、current 需要调度或 owner 所在 VCPU 被抢占时停止。
		 * top waiter 查询刻意包在 data_race：这里只作投机指针比较，变化会导致至多多/少自旋一轮，
		 * 真正接管仍在 wait_lock 下验证；PREEMPT_RCU 下 need_resched 条件是冗余安全检查。
		 */
		if (!owner_on_cpu(owner) || need_resched() ||
		    !data_race(rt_mutex_waiter_is_top_waiter(lock, waiter))) {
			res = false;
			break;
		}
		cpu_relax();
	}
	rcu_read_unlock();
	return res;
}
#else
/*
 * UP 构建没有可并行运行并即将释放锁的远端 owner，自适应自旋无收益。
 * 三个参数均为借用输入且不访问；恒返回 false，让调用者进入调度等待。无副作用且不能睡眠。
 */
static bool rtmutex_spin_on_owner(struct rt_mutex_base *lock,
				  struct rt_mutex_waiter *waiter,
				  struct task_struct *owner)
{
	return false;
}
#endif

#ifdef RT_MUTEX_BUILD_MUTEX
/*
 * Functions required for:
 *	- rtmutex, futex on all kernels
 *	- mutex and rwsem substitutions on RT kernels
 */
/*
 *
 * 本条件区生成所有内核上的 rtmutex/futex 所需函数，也生成 PREEMPT_RT 上 mutex/rwsem 替代实现所需函数。
 */

/*
 * Remove a waiter from a lock and give up
 *
 * Must be called with lock->wait_lock held and interrupts disabled. It must
 * have just failed to try_to_take_rt_mutex().
 *
 * When invoked from rt_mutex_start_proxy_lock() waiter::task != current !
 */
/*
 *
 * 从锁中移除一个 waiter 并放弃获取。调用者必须持 wait_lock、关闭中断，且刚刚尝试接管失败；
 * proxy lock 调用时 waiter->task 可能不是 current。
 */
/*
 * 撤销 waiter 排队、task->pi_blocked_on 和 owner PI donor，并按需沿链传播 deboost。
 * lock/waiter 为输入输出借用对象；waiter->task 若为空表示从未入队。入口/出口均持 wait_lock 且中断关闭，
 * 但传播链时会短暂放锁。返回无直接值；退出时 waiter 不在两棵树，waiter_task 不再阻塞于该锁；
 * 若它原是 top，则 owner 及更深 owner 的有效优先级已经按新 donor 调整。函数可能可抢占但不普通睡眠。
 */
static void __sched remove_waiter(struct rt_mutex_base *lock,
				  struct rt_mutex_waiter *waiter)
	__must_hold(&lock->wait_lock)
{
	/*
	 * is_top_waiter/owner 是 wait_lock 下入口快照；waiter_task 是实际阻塞任务（proxy 时非 current）；
	 * next_lock 仅记录 owner 是否继续阻塞，放 pi_lock 后不可解引用。
	 */
	bool is_top_waiter = (waiter == rt_mutex_top_waiter(lock));
	struct task_struct *owner = rt_mutex_owner(lock);
	struct task_struct *waiter_task = waiter->task;
	struct rt_mutex_base *next_lock;

	lockdep_assert_held(&lock->wait_lock);

	if (!waiter_task) /* never enqueued */
		/* waiter->task 只在正式排队时赋值；空值说明无需撤销任何状态。 */
		return;

	/* scoped_guard 在作用域结束自动释放 waiter_task->pi_lock，结构化保护双向摘除。 */
	scoped_guard(raw_spinlock, &waiter_task->pi_lock) {
		rt_mutex_dequeue(lock, waiter);
		waiter_task->pi_blocked_on = NULL;
	}

	/*
	 * Only update priority if the waiter was the highest priority
	 * waiter of the lock and there is an owner to update.
	 */
	/*
	 *
	 * 只有被移除者原是 lock top 且仍有 owner，owner PI 树的代表 donor 才发生变化；否则到此结束。
	 */
	if (!owner || !is_top_waiter)
		return;

	/* wait_lock 保证 owner 存活；在其 pi_lock 下用新 top 替换旧 donor（或彻底删除）。 */
	raw_spin_lock(&owner->pi_lock);

	rt_mutex_dequeue_pi(owner, waiter);

	if (rt_mutex_has_waiters(lock))
		rt_mutex_enqueue_pi(owner, rt_mutex_top_waiter(lock));

	rt_mutex_adjust_prio(lock, owner);

	/* Store the lock on which owner is blocked or NULL */
	/* 调整 owner 后记录它是否还在等待下一把锁，以决定 deboost 是否需继续传播。 */
	next_lock = task_blocked_on_lock(owner);

	raw_spin_unlock(&owner->pi_lock);

	/*
	 * Don't walk the chain, if the owner task is not blocked
	 * itself.
	 */
	/*
	 *
	 * owner 未阻塞即为 PI 链尾，局部 deboost 已足够，无需放锁遍历。
	 */
	if (!next_lock)
		return;

	/* gets dropped in rt_mutex_adjust_prio_chain()! */
	/* 跨越放 wait_lock 窗口前取得 owner 引用，释放责任转移给 adjust_prio_chain。 */
	get_task_struct(owner);

	/* chainwalk 以 NULL orig_waiter 表示 deboost，只做 MIN 遍历；随后恢复入口 wait_lock 契约。 */
	raw_spin_unlock_irq(&lock->wait_lock);

	rt_mutex_adjust_prio_chain(owner, RT_MUTEX_MIN_CHAINWALK, lock,
				   next_lock, NULL, waiter_task);

	raw_spin_lock_irq(&lock->wait_lock);
}

/**
 * rt_mutex_slowlock_block() - Perform the wait-wake-try-to-take loop
 * @lock:		 the rt_mutex to take
 * @ww_ctx:		 WW mutex context pointer
 * @state:		 the state the task should block in (TASK_INTERRUPTIBLE
 *			 or TASK_UNINTERRUPTIBLE)
 * @timeout:		 the pre-initialized and started timer, or NULL for none
 * @waiter:		 the pre-initialized rt_mutex_waiter
 * @wake_q:		 wake_q of tasks to wake when we drop the lock->wait_lock
 *
 * Must be called with lock->wait_lock held and interrupts disabled
 */
/*
 * 执行“尝试接管→检查退出→可选 owner 自旋→调度睡眠→醒后重试”的核心等待循环。
 * lock 是输入输出锁；ww_ctx 可空；state 为 INTERRUPTIBLE/UNINTERRUPTIBLE；timeout 可空且计时器已启动；
 * waiter 是 current 已排队的栈节点；wake_q 收集放 wait_lock 时的普通唤醒。入口和出口都持 wait_lock、
 * 中断关闭，但循环中反复释放/重取。返回 0 表示已获取，-ETIMEDOUT、-EINTR 或 WW -EDEADLK 表示
 * waiter 仍需由上层 remove；退出将 current 恢复 TASK_RUNNING。函数可调度睡眠，不改变 waiter ownership。
 */
static int __sched rt_mutex_slowlock_block(struct rt_mutex_base *lock,
					   struct ww_acquire_ctx *ww_ctx,
					   unsigned int state,
					   struct hrtimer_sleeper *timeout,
					   struct rt_mutex_waiter *waiter,
					   struct wake_q_head *wake_q)
	__releases(&lock->wait_lock) __acquires(&lock->wait_lock)
{
	/* rtm 是外层锁别名；owner 仅在当前 waiter 为 top 时用于锁外投机自旋；ret 汇总退出原因。 */
	struct rt_mutex *rtm = container_of(lock, struct rt_mutex, rtmutex);
	struct task_struct *owner;
	int ret = 0;

	__assume_ctx_lock(&rtm->rtmutex.wait_lock);

	lockevent_inc(rtmutex_slow_block);
	for (;;) {
		/* Try to acquire the lock: */
		/* 每次持 wait_lock 醒来先尝试接管；成功会自行出树并清 blocked_on。 */
		if (try_to_take_rt_mutex(lock, current, waiter)) {
			lockevent_inc(rtmutex_slow_acq3);
			break;
		}

		/* hrtimer sleeper 清空 task 表示期限已到；在再次睡眠前返回超时。 */
		if (timeout && !timeout->task) {
			ret = -ETIMEDOUT;
			break;
		}
		/* 只有所选 state 对 pending signal 可中断时才返回 -EINTR。 */
		if (signal_pending_state(state, current)) {
			ret = -EINTR;
			break;
		}

		if (build_ww_mutex() && ww_ctx) {
			/* WW waiter 每轮复查是否已被 wound/按 Wait-Die 应退出。 */
			ret = __ww_mutex_check_kill(rtm, waiter, ww_ctx);
			if (ret)
				break;
		}

		/* 只有 top waiter 值得跟随正在运行的 owner 自旋；非 top 直接准备睡眠。 */
		if (waiter == rt_mutex_top_waiter(lock))
			owner = rt_mutex_owner(lock);
		else
			owner = NULL;
		/* 睡眠/自旋前放 wait_lock 并消费其中普通 wake_q，避免持内部锁调度。 */
		raw_spin_unlock_irq_wake(&lock->wait_lock, wake_q);

		/* owner 不适合自旋或自旋条件失效时真正调度；owner 改变则跳过睡眠立即复查。 */
		if (!owner || !rtmutex_spin_on_owner(lock, waiter, owner)) {
			lockevent_inc(rtmutex_slow_sleep);
			rt_mutex_schedule();
		}

		/* 醒来重取 wait_lock，并在放锁前重新设置阻塞态以闭合条件检查—睡眠协议。 */
		raw_spin_lock_irq(&lock->wait_lock);
		set_current_state(state);
	}

	/* 所有出口都恢复运行态；waiter 出队和错误清理由调用者负责。 */
	__set_current_state(TASK_RUNNING);
	return ret;
}

/*
 * 处理未请求死锁检测却意外遇到普通 rtmutex 自锁环的不可恢复情况。
 * res 是慢路径结果，detect_deadlock 实际传入 chainwalk 枚举（0=MIN、1=FULL）；lock/w 是借用对象，
 * 入口持 wait_lock。非 -EDEADLOCK、显式检测或 WW waiter 均原样返回且仍持锁；普通 MIN 路径死锁则
 * 释放 wait_lock、WARN 并永久以可中断状态调度，函数不再返回，防止错误地继续执行临界区。
 */
static void __sched rt_mutex_handle_deadlock(int res, int detect_deadlock,
					     struct rt_mutex_base *lock,
					     struct rt_mutex_waiter *w)
	__must_hold(&lock->wait_lock)
{
	/*
	 * If the result is not -EDEADLOCK or the caller requested
	 * deadlock detection, nothing to do here.
	 */
	/*
	 *
	 * 只有“未显式请求检测却得到 -EDEADLOCK”需要转为诊断性永久阻塞；显式调用者会自行处理错误码。
	 */
	if (res != -EDEADLOCK || detect_deadlock)
		return;

	/* WW 协议把 -EDEADLK 作为正常退避信号，必须返回调用者而不能在此挂死。 */
	if (build_ww_mutex() && w->ww_ctx)
		return;

	raw_spin_unlock_irq(&lock->wait_lock);

	WARN(1, "rtmutex deadlock detected\n");

	/* 已经确认调用者无法安全恢复；循环调度而非带错误继续破坏 owner/PI 不变量。 */
	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		rt_mutex_schedule();
	}
}

/**
 * __rt_mutex_slowlock - Locking slowpath invoked with lock::wait_lock held
 * @lock:	The rtmutex to block lock
 * @ww_ctx:	WW mutex context pointer
 * @state:	The task state for sleeping
 * @chwalk:	Indicator whether full or partial chainwalk is requested
 * @waiter:	Initializer waiter for blocking
 * @wake_q:	The wake_q to wake tasks after we release the wait_lock
 */
/*
 * 在已持 wait_lock 时完成一次普通/WW rtmutex 慢速获取：复查、排队、PI 传播、等待与错误撤销。
 * lock 是输入输出锁；ww_ctx 可空；state 是睡眠态；chwalk 选择死锁检测；waiter 是已初始化未入树的
 * 栈对象；wake_q 收集放锁后唤醒。入口/出口均持 wait_lock，中间可调度。返回 0 表示 current 成为
 * owner；返回 -EDEADLK/-EINTR 等表示 waiter 已移除、任务恢复 RUNNING。成功 WW 路径还发布 ctx/acquired，
 * 所有出口修复 HAS_WAITERS 并结束 contention trace。
 */
static int __sched __rt_mutex_slowlock(struct rt_mutex_base *lock,
				       struct ww_acquire_ctx *ww_ctx,
				       unsigned int state,
				       enum rtmutex_chainwalk chwalk,
				       struct rt_mutex_waiter *waiter,
				       struct wake_q_head *wake_q)
	__must_hold(&lock->wait_lock)
{
	/* rtm 是公共外层别名，ww 在非 WW 实例恒 NULL；ret 传递排队/等待结果。 */
	struct rt_mutex *rtm = container_of(lock, struct rt_mutex, rtmutex);
	struct ww_mutex *ww = ww_container_of(rtm);
	int ret;

	__assume_ctx_lock(&rtm->rtmutex.wait_lock);
	lockdep_assert_held(&lock->wait_lock);
	lockevent_inc(rtmutex_slowlock);

	/* Try to acquire the lock again: */
	/* 阶段一：持 wait_lock 复查，可能在真正排队前就取得锁，避免无谓 PI 操作和睡眠。 */
	if (try_to_take_rt_mutex(lock, current, NULL)) {
		if (build_ww_mutex() && ww_ctx) {
			/* WW 快速慢路径成功也需检查现有 waiter，并在最终边界发布 ctx/acquired。 */
			__ww_mutex_check_waiters(rtm, ww_ctx, wake_q);
			ww_mutex_lock_acquired(ww, ww_ctx);
		}
		lockevent_inc(rtmutex_slow_acq1);
		return 0;
	}

	/* 阶段二：先设置阻塞态再发布 contention/入队，防止唤醒发生在状态设置之前而丢失。 */
	set_current_state(state);

	trace_contention_begin(lock, LCB_F_RT);

	/* 建立双树与 PI 链；成功后进入等待循环，错误则直接走统一撤销。 */
	ret = task_blocks_on_rt_mutex(lock, waiter, current, ww_ctx, chwalk, wake_q);
	if (likely(!ret))
		ret = rt_mutex_slowlock_block(lock, ww_ctx, state, NULL, waiter, wake_q);

	if (likely(!ret)) {
		/* acquired the lock */
		/* 阶段三成功：waiter 已由 try_to_take 出树，current 已成为 owner。 */
		if (build_ww_mutex() && ww_ctx) {
			/* Wound-Wait 成功后再 wound 较新 waiter；Wait-Die 不主动 wound，因此跳过扫描。 */
			if (!ww_ctx->is_wait_die)
				__ww_mutex_check_waiters(rtm, ww_ctx, wake_q);
			ww_mutex_lock_acquired(ww, ww_ctx);
		}
		lockevent_inc(rtmutex_slow_acq2);
	} else {
		/* 阶段三失败：恢复运行态，撤销 waiter/PI donor，必要时处理不可恢复的普通死锁。 */
		__set_current_state(TASK_RUNNING);
		remove_waiter(lock, waiter);
		rt_mutex_handle_deadlock(ret, chwalk, lock, waiter);
		lockevent_inc(rtmutex_deadlock);
	}

	/*
	 * try_to_take_rt_mutex() sets the waiter bit
	 * unconditionally. We might have to fix that up.
	 */
	/*
	 *
	 * 所有 try_to_take 尝试都会置 waiter 位；错误出队或成功后树为空时在返回前清除过渡位。
	 */
	fixup_rt_mutex_waiters(lock, true);

	/* contention trace 与 begin 配对，ret 精确记录成功或失败类别。 */
	trace_contention_end(lock, ret);

	return ret;
}

/*
 * 在调用者已持 wait_lock 时，用一个栈上 waiter 执行 MIN chainwalk 的标准慢速获取包装。
 * lock 是输入输出锁；ww_ctx 可空借用；state 是睡眠状态；wake_q 为输入输出延迟唤醒队列。
 * 返回 0 或底层错误，入口/出口均持 wait_lock；waiter 生命周期完全局限本函数，底层返回后已出树，
 * 随即调试毒化。函数可睡眠，WW ctx ownership 仅在底层成功时按协议发布。
 */
static inline int __rt_mutex_slowlock_locked(struct rt_mutex_base *lock,
					     struct ww_acquire_ctx *ww_ctx,
					     unsigned int state,
					     struct wake_q_head *wake_q)
	__must_hold(&lock->wait_lock)
{
	/* waiter 是 current 的栈上排队对象，ret 传递标准慢路径结果。 */
	struct rt_mutex_waiter waiter;
	int ret;

	/* 初始化双树空节点和普通 wake_state，再在入树前设置 WW ctx 排序字段。 */
	rt_mutex_init_waiter(&waiter);
	waiter.ww_ctx = ww_ctx;

	ret = __rt_mutex_slowlock(lock, ww_ctx, state, RT_MUTEX_MIN_CHAINWALK,
				  &waiter, wake_q);

	/* 底层已完成获取或撤销；毒化栈对象并统计是否产生了待锁外唤醒任务。 */
	debug_rt_mutex_free_waiter(&waiter);
	lockevent_cond_inc(rtmutex_slow_wake, !wake_q_empty(wake_q));
	return ret;
}

/*
 * rt_mutex_slowlock - Locking slowpath invoked when fast path fails
 * @lock:	The rtmutex to block lock
 * @ww_ctx:	WW mutex context pointer
 * @state:	The task state for sleeping
 */
/*
 * fastpath 失败后的标准 rtmutex/WW 外层慢路径，负责调度前后钩子、irqsave wait_lock 与锁外 wake_q。
 * lock 是输入输出借用锁；ww_ctx 可空；state 指定可中断性。入口不持 wait_lock，函数可睡眠；返回 0
 * 表示获得锁，负 errno 表示未获得且全部 waiter/PI 状态已撤销。所有中断、worker 调度记账和 wake_q
 * 在返回前恢复/消费；成功后 current 持有 lock，失败后不持有。
 */
static int __sched rt_mutex_slowlock(struct rt_mutex_base *lock,
				     struct ww_acquire_ctx *ww_ctx,
				     unsigned int state)
{
	/* wake_q 延迟普通任务唤醒；flags 保存中断状态；ret 传递慢路径结果。 */
	DEFINE_WAKE_Q(wake_q);
	unsigned long flags;
	int ret;

	/*
	 * Do all pre-schedule work here, before we queue a waiter and invoke
	 * PI -- any such work that trips on rtlock (PREEMPT_RT spinlock) would
	 * otherwise recurse back into task_blocks_on_rt_mutex() through
	 * rtlock_slowlock() and will then enqueue a second waiter for this
	 * same task and things get really confusing real fast.
	 */
	/*
	 *
	 * 所有可能触发调度前提交的工作必须在 waiter 入队和 PI 生效前完成。若这些工作自身碰到 PREEMPT_RT
	 * sleeping spinlock，会经 rtlock_slowlock() 再回到 task_blocks_on_rt_mutex()；若当前任务已入队，
	 * 就会为同一 task 建第二个 waiter，破坏唯一 pi_blocked_on 不变量。
	 */
	rt_mutex_pre_schedule();

	/*
	 * Technically we could use raw_spin_[un]lock_irq() here, but this can
	 * be called in early boot if the cmpxchg() fast path is disabled
	 * (debug, no architecture support). In this case we will acquire the
	 * rtmutex with lock->wait_lock held. But we cannot unconditionally
	 * enable interrupts in that early boot case. So we need to use the
	 * irqsave/restore variants.
	 */
	/*
	 *
	 * 理论上可直接 irq disable，但 debug 或无架构 cmpxchg 时早期启动也会走此路径；当时中断可能本就
	 * 关闭，返回时不能无条件 enable。因此保存/恢复原状态，并在 unlock helper 中顺带消费 wake_q。
	 */
	raw_spin_lock_irqsave(&lock->wait_lock, flags);
	ret = __rt_mutex_slowlock_locked(lock, ww_ctx, state, &wake_q);
	raw_spin_unlock_irqrestore_wake(&lock->wait_lock, flags, &wake_q);
	/* 锁外恢复 worker/调度记账，与 pre_schedule 严格配对。 */
	rt_mutex_post_schedule();

	return ret;
}

/*
 * 普通 rtmutex 阻塞式获取的 fast/slow 汇合入口。
 * lock 是输入输出借用对象，state 为调用者要求的睡眠态；入口要求 current 未因 PI 阻塞。
 * 返回 0 表示 current 已持锁，负 errno 来自可中断慢路径；fastpath 不睡眠，失败后转标准 slowlock。
 * 本包装不接受 ww_ctx，WW_RT 的公开包装直接调用相应共享慢路径。
 */
static __always_inline int __rt_mutex_lock(struct rt_mutex_base *lock,
					   unsigned int state)
{
	lockdep_assert(!current->pi_blocked_on);

	if (likely(rt_mutex_try_acquire(lock)))
		return 0;

	return rt_mutex_slowlock(lock, NULL, state);
}
#endif /* RT_MUTEX_BUILD_MUTEX */
/* 结束普通 rtmutex/futex 与 PREEMPT_RT mutex/rwsem 共用实现。 */

#ifdef RT_MUTEX_BUILD_SPINLOCKS
/*
 * Functions required for spin/rw_lock substitution on RT kernels
 */
/*
 *
 * 本条件区生成 PREEMPT_RT 上 spinlock/rwlock 睡眠替代路径；它使用 TASK_RTLOCK_WAIT 和专用调度接口，
 * 对外仍保持不可中断的“锁”语义。
 */

/**
 * rtlock_slowlock_locked - Slow path lock acquisition for RT locks
 * @lock:	The underlying RT mutex
 * @wake_q:	The wake_q to wake tasks after we release the wait_lock
 */
/*
 * 在已持 wait_lock 时完成 PREEMPT_RT sleeping spin/rwlock 的慢速获取循环。
 * lock 是底层输入输出 rt_mutex；wake_q 收集放锁时的普通唤醒。入口/出口均持 wait_lock 且中断关闭，
 * 中间反复放锁并用 schedule_rtlock() 睡眠。返回无直接值，只有成功取得 lock 才返回；栈 waiter 已出树、
 * current 原任务状态已恢复、HAS_WAITERS 已修复。该路径不可被信号/超时终止，也不使用 WW ctx。
 */
static void __sched rtlock_slowlock_locked(struct rt_mutex_base *lock,
					   struct wake_q_head *wake_q)
	__releases(&lock->wait_lock) __acquires(&lock->wait_lock)
{
	/* waiter 是专用 TASK_RTLOCK_WAIT 栈节点；owner 只为 top waiter 的锁外自适应自旋提供裸快照。 */
	struct rt_mutex_waiter waiter;
	struct task_struct *owner;

	lockdep_assert_held(&lock->wait_lock);
	lockevent_inc(rtlock_slowlock);

	/* 阶段一：在排队前复查空锁，成功则无需改变 current 状态。 */
	if (try_to_take_rt_mutex(lock, current, NULL)) {
		lockevent_inc(rtlock_slow_acq1);
		return;
	}

	/* 阶段二：初始化专用唤醒态 waiter，随后保存原 task state 并切换到 RTLOCK_WAIT。 */
	rt_mutex_init_rtlock_waiter(&waiter);

	/* Save current state and set state to TASK_RTLOCK_WAIT */
	/* 保存并覆盖 current 状态，防止替代自旋锁内部等待破坏外层调用者已有睡眠状态。 */
	current_save_and_set_rtlock_wait_state();

	trace_contention_begin(lock, LCB_F_RT);

	/* 无 WW、MIN chainwalk 排队并传播 PI；RT lock 路径不允许可恢复错误，调用结果无需分支。 */
	task_blocks_on_rt_mutex(lock, &waiter, current, NULL, RT_MUTEX_MIN_CHAINWALK, wake_q);

	for (;;) {
		/* Try to acquire the lock again */
		/* 每轮先在 wait_lock 下尝试接管；成功时 helper 出树、清 blocked_on 并发布 owner。 */
		if (try_to_take_rt_mutex(lock, current, &waiter)) {
			lockevent_inc(rtlock_slow_acq2);
			break;
		}

		/* 仅 top waiter 获取 owner 快照并尝试锁外自旋，其他 waiter 直接调度。 */
		if (&waiter == rt_mutex_top_waiter(lock))
			owner = rt_mutex_owner(lock);
		else
			owner = NULL;
		/* 调度/自旋前放内部锁并处理普通 wake_q，避免在 raw spinlock 下睡眠。 */
		raw_spin_unlock_irq_wake(&lock->wait_lock, wake_q);

		/* owner 不适合自旋时使用 RT lock 专用调度，不执行普通 sched_submit_work 递归。 */
		if (!owner || !rtmutex_spin_on_owner(lock, &waiter, owner)) {
			lockevent_inc(rtlock_slow_sleep);
			schedule_rtlock();
		}

		/* 醒来重取 wait_lock，并重新发布 RTLOCK_WAIT 状态后再检查接管条件。 */
		raw_spin_lock_irq(&lock->wait_lock);
		set_current_state(TASK_RTLOCK_WAIT);
	}

	/* Restore the task state */
	/* 成功后恢复进入替代锁之前保存的 task state，外层语义不感知内部睡眠。 */
	current_restore_rtlock_saved_state();

	/*
	 * try_to_take_rt_mutex() sets the waiter bit unconditionally.
	 * We might have to fix that up:
	 */
	/*
	 *
	 * 接管尝试无条件置 waiter 位；成功出树后按获取语义清除可能残留的过渡位。
	 */
	fixup_rt_mutex_waiters(lock, true);
	debug_rt_mutex_free_waiter(&waiter);

	trace_contention_end(lock, 0);
	lockevent_cond_inc(rtlock_slow_wake, !wake_q_empty(wake_q));
}

/*
 * PREEMPT_RT sleeping spin/rwlock 慢路径的外层 wait_lock/wake_q 包装。
 * lock 是输入输出借用底层锁；入口不持 wait_lock，返回无直接值且保证 current 已获得 lock。
 * 使用 irqsave 兼容原中断状态；内部可调度，退出时锁外消费 wake_q 并恢复中断。
 */
static __always_inline void __sched rtlock_slowlock(struct rt_mutex_base *lock)
{
	/* flags 保存中断状态；wake_q 在释放 wait_lock 的同一 helper 中被安全唤醒。 */
	unsigned long flags;
	DEFINE_WAKE_Q(wake_q);

	raw_spin_lock_irqsave(&lock->wait_lock, flags);
	rtlock_slowlock_locked(lock, &wake_q);
	raw_spin_unlock_irqrestore_wake(&lock->wait_lock, flags, &wake_q);
}

#endif /* RT_MUTEX_BUILD_SPINLOCKS */
/* 结束 PREEMPT_RT sleeping spin/rwlock 替代实现。 */
