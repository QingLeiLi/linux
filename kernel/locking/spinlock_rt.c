// SPDX-License-Identifier: GPL-2.0-only
/*
 * PREEMPT_RT substitution for spin/rw_locks
 *
 * spinlocks and rwlocks on RT are based on rtmutexes, with a few twists to
 * resemble the non RT semantics:
 *
 * - Contrary to plain rtmutexes, spinlocks and rwlocks are state
 *   preserving. The task state is saved before blocking on the underlying
 *   rtmutex, and restored when the lock has been acquired. Regular wakeups
 *   during that time are redirected to the saved state so no wake up is
 *   missed.
 *
 * - Non RT spin/rwlocks disable preemption and eventually interrupts.
 *   Disabling preemption has the side effect of disabling migration and
 *   preventing RCU grace periods.
 *
 *   The RT substitutions explicitly disable migration and take
 *   rcu_read_lock() across the lock held section.
 */
/*
 * PREEMPT_RT 用可睡眠且支持优先级继承的 rtmutex/rwbase 替代普通 spin/rwlock。等待前保存任务原状态，
 * 普通唤醒改写 saved_state，获取后再恢复，从而不会因锁等待吞掉唤醒；持锁期间显式禁止迁移并进入
 * RCU 读侧，分别补回非 RT 禁抢占隐含的 CPU 固定和阻止 grace period 语义。
 */
#include <linux/spinlock.h>
#include <linux/export.h>

/* 选择 rtmutex.c 中专供 PREEMPT_RT spin/rwlock 的状态保持慢路径，并在本翻译单元内生成它。 */
#define RT_MUTEX_BUILD_SPINLOCKS
#include "rtmutex.c"

/*
 * __might_resched() skips the state check as rtlocks are state
 * preserving. Take RCU nesting into account as spin/read/write_lock() can
 * legitimately nest into an RCU read side critical section.
 */
/*
 * RT 锁等待会保存任务状态，因此这里只做可调度上下文检查而跳过普通阻塞 API 的 state 检查；把当前
 * RCU 嵌套深度编码进允许偏移，使调用者原本位于 RCU 读侧时不会被误报，真实抢占/IRQ 错误仍可见。
 */
#define RTLOCK_RESCHED_OFFSETS						\
	(rcu_preempt_depth() << MIGHT_RESCHED_RCU_SHIFT)

#define rtlock_might_resched()						\
	__might_resched(__FILE__, __LINE__, RTLOCK_RESCHED_OFFSETS)

/*
 * 获取底层 rtmutex rtm；无返回值，成功出口 owner 为 current。要求当前任务未正作为另一 PI 锁的
 * waiter；先用 acquire cmpxchg 处理无 owner 快路径，失败进入会保存任务状态并可调度的 RT-lock
 * 慢路径。rtm 由外层锁对象拥有，调用者负责释放。
 */
static __always_inline void rtlock_lock(struct rt_mutex_base *rtm)
{
	lockdep_assert(!current->pi_blocked_on);

	if (unlikely(!rt_mutex_cmpxchg_acquire(rtm, NULL, current)))
		rtlock_slowlock(rtm);
}

/*
 * 获取 PREEMPT_RT spinlock lock 的底层 rtmutex，再建立 RCU 读侧和 migration-disable 区间；
 * 无返回值，出口持锁。可能睡眠的获取必须先完成，之后才添加持锁期约束；调用者负责逆向配对释放。
 */
static __always_inline void __rt_spin_lock(spinlock_t *lock)
{
	rtlock_might_resched();
	rtlock_lock(&lock->lock);
	rcu_read_lock();
	migrate_disable();
}

/*
 * 按默认子类 0 登记 lock 的 lockdep 获取，再阻塞取得 RT spinlock；无返回值，出口持锁、位于 RCU
 * 读侧且禁止迁移。调用前必须允许 RT-lock 调度，调用者以 rt_spin_unlock() 配对。
 */
void __sched rt_spin_lock(spinlock_t *lock) __acquires(RCU)
{
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
	__rt_spin_lock(lock);
}
EXPORT_SYMBOL(rt_spin_lock);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/*
 * 以 subclass 指定的同类嵌套层级登记 lockdep 后阻塞取得 lock；无返回值，成功出口持锁、进入 RCU
 * 并禁止迁移。subclass 仅描述依赖关系，不改变底层 rtmutex 获取算法。
 */
void __sched rt_spin_lock_nested(spinlock_t *lock, int subclass)
{
	spin_acquire(&lock->dep_map, subclass, 0, _RET_IP_);
	__rt_spin_lock(lock);
}
EXPORT_SYMBOL(rt_spin_lock_nested);

/*
 * 以调用者已持有的 nest_lock 作为 lockdep 嵌套锚点，再阻塞取得 RT spinlock lock；无返回值。
 * nest_lock 只被借用来记录依赖，本函数不获取或释放它；出口对 lock 建立 RCU/禁止迁移持有责任。
 */
void __sched rt_spin_lock_nest_lock(spinlock_t *lock,
				    struct lockdep_map *nest_lock)
{
	spin_acquire_nest(&lock->dep_map, 0, 0, nest_lock, _RET_IP_);
	__rt_spin_lock(lock);
}
EXPORT_SYMBOL(rt_spin_lock_nest_lock);
#endif

/*
 * 释放当前任务持有的 RT spinlock lock；无返回值。先结束 lockdep/migration 责任，再以 release
 * cmpxchg 或慢路径释放底层 rtmutex，最后退出 RCU 读侧。调用前必须持锁，函数不管理外层对象寿命。
 */
void __sched rt_spin_unlock(spinlock_t *lock) __releases(RCU)
{
	/* 阶段一结束逻辑持有与 CPU 固定；阶段二释放可能触发 PI 去提升/唤醒的底层 rtmutex。 */
	spin_release(&lock->dep_map, _RET_IP_);
	migrate_enable();

	if (unlikely(!rt_mutex_cmpxchg_release(&lock->lock, current, NULL)))
		rt_mutex_slowunlock(&lock->lock);

	/*
	 * This must be last to prevent the following UAF:
	 *
	 * T1					T2
	 * spin_lock(&p->lock);			rcu_read_lock();
	 * invalidate(p);			p = rcu_dereference(ptr);
	 * rcu_assign_pointer(ptr, NULL);	if (!p) return;
	 * spin_unlock(&p->lock);		spin_lock(&p->lock);
	 * kfree_rcu(p);			rcu_read_unlock();
	 *					....
	 *					spin_unlock(&p->lock)
	 *					  rcu_read_unlock(); // Ends grace period
	 * rcu_do_batch()
	 *   kfree(p);
	 *			    UAF ->	  rt_mutex_cmpxchg_release(&p->lock.lock...)
	 */
	/*
	 * RCU 退出必须最后执行：若 T2 在释放内嵌 p->lock 前先结束读侧区间，T1 的 kfree_rcu(p)
	 * 可能完成 grace period 并释放 p，随后 T2 对 lock.lock 的 owner cmpxchg 就会访问已释放内存。
	 * 先完成底层解锁，再 rcu_read_unlock()，可保证最后一次锁字段访问仍受对象生命周期保护。
	 */
	rcu_read_unlock();
}
EXPORT_SYMBOL(rt_spin_unlock);

/*
 * Wait for the lock to get unlocked: instead of polling for an unlock
 * (like raw spinlocks do), lock and unlock, to force the kernel to
 * schedule if there's contention:
 */
/*
 * 等待 lock 经历一次可获取状态；无返回值，返回时不持锁。与 raw spinlock 轮询不同，本函数实际
 * lock 后立即 unlock，存在争用时可进入 RT 调度/PI 慢路径，适合只需等待前一持有者结束的调用者。
 */
void __sched rt_spin_lock_unlock(spinlock_t *lock)
{
	spin_lock(lock);
	spin_unlock(lock);
}
EXPORT_SYMBOL(rt_spin_lock_unlock);

/*
 * 尝试一次取得 RT spinlock lock；成功返回非零并登记 try-lock lockdep、进入 RCU、禁止迁移，失败
 * 返回 0 且不建立这些状态。不阻塞等待 owner 释放；cmpxchg 不可用或遇到特殊状态时慢 trylock
 * 仍只作即时尝试。调用者仅在成功时承担释放责任。
 */
static __always_inline int __rt_spin_trylock(spinlock_t *lock)
{
	/* 默认对应无 owner cmpxchg 成功；快路径失败时由慢 trylock 覆盖为真实结果。 */
	int ret = 1;

	if (unlikely(!rt_mutex_cmpxchg_acquire(&lock->lock, NULL, current)))
		ret = rt_mutex_slowtrylock(&lock->lock);

	if (ret) {
		spin_acquire(&lock->dep_map, 0, 1, _RET_IP_);
		rcu_read_lock();
		migrate_disable();
	}
	return ret;
}

/*
 * 对 lock 执行无 BH/IRQ 附加语义的 RT trylock；成功返回非零并持锁，失败返回 0。
 * 所有状态建立由内部 helper 完成，调用者只在成功时配对 rt_spin_unlock()。
 */
int __sched rt_spin_trylock(spinlock_t *lock)
{
	return __rt_spin_trylock(lock);
}
EXPORT_SYMBOL(rt_spin_trylock);

/*
 * 禁止本地 BH 后尝试取得 lock；成功返回非零并保持 BH 禁止、持锁/RCU/禁止迁移，失败返回 0 并
 * 立即恢复 BH。成功调用者必须走 spin_unlock_bh() 一类配对路径，不能只释放底层锁而遗漏 BH。
 */
int __sched rt_spin_trylock_bh(spinlock_t *lock)
{
	/* ret 决定本函数是否回滚入口建立的 BH 禁止状态。 */
	int ret;

	local_bh_disable();
	ret = __rt_spin_trylock(lock);
	if (!ret)
		local_bh_enable();
	return ret;
}
EXPORT_SYMBOL(rt_spin_trylock_bh);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/*
 * 初始化 RT spinlock lock 的 lockdep map；name/key 标识类，percpu 选择 LD_LOCK_PERCPU 或普通类型。
 * 无返回值；调用者须先初始化底层 rt_mutex_base，且不得并发使用/重初始化仍持有的对象。
 */
void __rt_spin_lock_init(spinlock_t *lock, const char *name,
			 struct lock_class_key *key, bool percpu)
{
	/* type 只编码 lockdep 的对象类别，不改变 rtmutex 的运行期获取语义。 */
	u8 type = percpu ? LD_LOCK_PERCPU : LD_LOCK_NORMAL;

	debug_check_no_locks_freed((void *)lock, sizeof(*lock));
	lockdep_init_map_type(&lock->dep_map, name, key, 0, LD_WAIT_CONFIG,
			      LD_WAIT_INV, type);
}
EXPORT_SYMBOL(__rt_spin_lock_init);
#endif

/*
 * RT-specific reader/writer locks
 */

/*
 * 以下 hook 把共享 rwbase_rt.c 专门化为状态保持的 RT rwlock：忽略调用方 state，统一保存原任务
 * 状态并以 TASK_RTLOCK_WAIT 等待；信号永不打断获取；调度使用 schedule_rtlock()，前后钩子为空。
 * 因此 rwbase 的 -EINTR 分支会被编译优化掉，公开 RT rwlock 获取没有可中断失败出口。
 */
#define rwbase_set_and_save_current_state(state)	\
	current_save_and_set_rtlock_wait_state()

#define rwbase_restore_current_state()			\
	current_restore_rtlock_saved_state()

/*
 * 为 rwbase 阻塞取得底层 rtmutex rtm；state 因 RT rwlock 状态保持语义而不参与选择。
 * 成功恒返回 0 且出口由 current 持有 rtm；快路径失败时进入可调度慢路径，调用者负责释放。
 */
static __always_inline int
rwbase_rtmutex_lock_state(struct rt_mutex_base *rtm, unsigned int state)
{
	if (unlikely(!rt_mutex_cmpxchg_acquire(rtm, NULL, current)))
		rtlock_slowlock(rtm);
	return 0;
}

/*
 * 在调用者已持有 rtm->wait_lock 时执行 rwbase 的 RT-lock 慢获取；wake_q 收集待在释放 wait_lock
 * 后唤醒的任务。成功恒返回 0、出口持有 rtm，并维持 wait_lock 的持有后置条件。
 */
static __always_inline int
rwbase_rtmutex_slowlock_locked(struct rt_mutex_base *rtm, unsigned int state,
			       struct wake_q_head *wake_q)
{
	/* RT rwlock 忽略 state 的可中断选择；wake_q 由调用者持有并在退出 wait_lock 后统一消费。 */
	rtlock_slowlock_locked(rtm, wake_q);
	return 0;
}

/*
 * 释放 current 持有的 rwbase 底层 rtmutex rtm；无返回值。无 waiter/调试特殊状态时直接清 owner，
 * 否则进入慢释放完成 PI 去提升和 waiter 唤醒；对象生命周期仍由外层 rwbase 管理。
 */
static __always_inline void rwbase_rtmutex_unlock(struct rt_mutex_base *rtm)
{
	if (likely(rt_mutex_cmpxchg_acquire(rtm, current, NULL)))
		return;

	rt_mutex_slowunlock(rtm);
}

/*
 * 即时尝试取得 rwbase 的底层 rtmutex rtm；成功返回 1 且 owner 为 current，失败返回 0。
 * 快 cmpxchg 不可完成时由慢 trylock 在 wait_lock 下重试，但不会排队睡眠；仅成功者负责释放。
 */
static __always_inline int  rwbase_rtmutex_trylock(struct rt_mutex_base *rtm)
{
	if (likely(rt_mutex_cmpxchg_acquire(rtm, NULL, current)))
		return 1;

	return rt_mutex_slowtrylock(rtm);
}

#define rwbase_signal_pending_state(state, current)	(0)

#define rwbase_pre_schedule()

#define rwbase_schedule()				\
	schedule_rtlock()

#define rwbase_post_schedule()

/*
 * 以上 adapter/hook 就绪后包含共享 rwbase 实现：reader 以 acquire 原子偏置走快路径，writer 先持
 * rtmutex、移除 READER_BIAS，再等活跃 readers 清空；后来阻塞在该 rtmutex 上的任务可提升 writer，
 * 但无法把 writer 优先级同时传给多个活跃 readers，因此算法不保证 writer 公平。
 */
#include "rwbase_rt.c"
/*
 * The common functions which get wrapped into the rwlock API.
 */
/* 下列公共入口把共享 rwbase 状态机包装成带 lockdep、RCU 与禁止迁移语义的 RT rwlock API。 */

/*
 * 尝试取得 rwlock 的共享读持有；成功返回非零并登记共享 try-lock、进入 RCU、禁止迁移，失败返回
 * 0 且不建立这些状态。不等待；调用者仅在成功时负责 rt_read_unlock()。
 */
int __sched rt_read_trylock(rwlock_t *rwlock)
{
	/* ret 保存 reader 偏置快路径或 rwbase 即时慢路径的最终结果。 */
	int ret;

	ret = rwbase_read_trylock(&rwlock->rwbase);
	if (ret) {
		rwlock_acquire_read(&rwlock->dep_map, 0, 1, _RET_IP_);
		rcu_read_lock();
		migrate_disable();
	}
	return ret;
}
EXPORT_SYMBOL(rt_read_trylock);

/*
 * 尝试取得 rwlock 的独占写持有；成功返回非零并登记独占 try-lock、进入 RCU、禁止迁移，失败返回
 * 0 且不改外层持有状态。不排队睡眠，调用者仅在成功时负责 rt_write_unlock()。
 */
int __sched rt_write_trylock(rwlock_t *rwlock)
{
	/* ret 控制成功后 lockdep/RCU/migration 三项持有责任是否建立。 */
	int ret;

	ret = rwbase_write_trylock(&rwlock->rwbase);
	if (ret) {
		rwlock_acquire(&rwlock->dep_map, 0, 1, _RET_IP_);
		rcu_read_lock();
		migrate_disable();
	}
	return ret;
}
EXPORT_SYMBOL(rt_write_trylock);

/*
 * 阻塞取得 rwlock 的共享读持有；无返回值，出口登记共享 lockdep、位于 RCU 读侧并禁止迁移。
 * 等待使用 TASK_RTLOCK_WAIT 保存/恢复调用者原状态；调用者以 rt_read_unlock() 配对。
 */
void __sched rt_read_lock(rwlock_t *rwlock) __acquires(RCU)
{
	/* 阶段一确认允许调度并登记共享依赖；阶段二通过 reader bias/rtmutex 取得真实读持有。 */
	rtlock_might_resched();
	rwlock_acquire_read(&rwlock->dep_map, 0, 0, _RET_IP_);
	rwbase_read_lock(&rwlock->rwbase, TASK_RTLOCK_WAIT);
	/* 可能睡眠的获取完成后，才补建非 RT 锁持有期间隐含的 RCU 与 CPU 固定语义。 */
	rcu_read_lock();
	migrate_disable();
}
EXPORT_SYMBOL(rt_read_lock);

/*
 * 阻塞取得 rwlock 的独占写持有；无返回值，出口排除 readers/writers、登记 lockdep、进入 RCU 并
 * 禁止迁移。算法无法把 writer 优先级同时传给多个活跃 readers；但随后阻塞在其 rtmutex 上的任务
 * 可向 writer 提供 PI/DL 提升，不能据此推导 writer 公平性。
 */
void __sched rt_write_lock(rwlock_t *rwlock) __acquires(RCU)
{
	/* 先做睡眠上下文/lockdep 检查，再经 rtmutex 排他并等待 reader 计数清空。 */
	rtlock_might_resched();
	rwlock_acquire(&rwlock->dep_map, 0, 0, _RET_IP_);
	rwbase_write_lock(&rwlock->rwbase, TASK_RTLOCK_WAIT);
	/* writer 真正取得锁后再进入 RCU 并禁止迁移，覆盖整个对外临界区。 */
	rcu_read_lock();
	migrate_disable();
}
EXPORT_SYMBOL(rt_write_lock);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/*
 * 以 subclass 登记同类嵌套关系后阻塞取得 rwlock 的独占写持有；无返回值，出口带 RCU/禁止迁移
 * 责任。subclass 只影响 lockdep，底层仍走相同的状态保持 rwbase writer 路径。
 */
void __sched rt_write_lock_nested(rwlock_t *rwlock, int subclass) __acquires(RCU)
{
	rtlock_might_resched();
	rwlock_acquire(&rwlock->dep_map, subclass, 0, _RET_IP_);
	rwbase_write_lock(&rwlock->rwbase, TASK_RTLOCK_WAIT);
	rcu_read_lock();
	migrate_disable();
}
EXPORT_SYMBOL(rt_write_lock_nested);
#endif

/*
 * 释放当前任务对 rwlock 的一个共享读持有；无返回值。先结束 lockdep/migration，再递减 reader
 * 计数并在最后 reader 离开时唤醒 writer，最后退出 RCU。调用前必须持有共享锁。
 */
void __sched rt_read_unlock(rwlock_t *rwlock) __releases(RCU)
{
	/* 先结束逻辑持有/CPU 固定，再释放 reader 计数；最后的 RCU 退出留到所有对象访问之后。 */
	rwlock_release(&rwlock->dep_map, _RET_IP_);
	migrate_enable();
	rwbase_read_unlock(&rwlock->rwbase, TASK_RTLOCK_WAIT);

	/* This must be last. See comment in rt_spin_unlock() */
	/* 必须最后退出 RCU；理由同 rt_spin_unlock()，底层 rwbase 的最后一次对象访问必须仍受保护。 */
	rcu_read_unlock();
}
EXPORT_SYMBOL(rt_read_unlock);

/*
 * 释放当前任务对 rwlock 的独占写持有；无返回值。先结束 lockdep/migration，再恢复 reader bias、
 * 释放底层 rtmutex 并唤醒等待者，最后退出 RCU；调用前必须持有写锁。
 */
void __sched rt_write_unlock(rwlock_t *rwlock) __releases(RCU)
{
	/* 先结束逻辑持有/CPU 固定，再恢复 reader 快路径并释放 writer rtmutex。 */
	rwlock_release(&rwlock->dep_map, _RET_IP_);
	migrate_enable();
	rwbase_write_unlock(&rwlock->rwbase);

	/* This must be last. See comment in rt_spin_unlock() */
	/* 必须最后退出 RCU，避免外层对象被回收后仍访问其中的 rwbase/rtmutex 字段。 */
	rcu_read_unlock();
}
EXPORT_SYMBOL(rt_write_unlock);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/*
 * 初始化 RT rwlock 的 lockdep map；name/key 标识稳定锁类，等待类型为 LD_WAIT_CONFIG。
 * 无返回值；调用者须先初始化 rwbase，且不得并发重初始化仍在使用的对象。
 */
void __rt_rwlock_init(rwlock_t *rwlock, const char *name,
		      struct lock_class_key *key)
{
	/* 先检查目标范围未含当前任务仍持有的 lockdep map，再建立可配置等待的普通锁类。 */
	debug_check_no_locks_freed((void *)rwlock, sizeof(*rwlock));
	lockdep_init_map_wait(&rwlock->dep_map, name, key, 0, LD_WAIT_CONFIG);
}
EXPORT_SYMBOL(__rt_rwlock_init);
#endif
