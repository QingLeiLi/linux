/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RT Mutexes: blocking mutual exclusion locks with PI support
 * RT Mutex：支持优先级继承（PI）的阻塞式互斥锁。
 *
 * started by Ingo Molnar and Thomas Gleixner:
 * 最初由 Ingo Molnar 与 Thomas Gleixner 开始实现：
 *
 *  Copyright (C) 2004-2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *  Copyright (C) 2006, Timesys Corp., Thomas Gleixner <tglx@timesys.com>
 *
 * This file contains the private data structure and API definitions.
 *
 * 本文件保存 RT mutex 核心私有的数据结构与 API 定义；普通调用者应使用公开的 rtmutex 接口，
 * 这里的 waiter、PI 树和 proxy helper 由 rtmutex 核心与 PI-futex 等紧密协作者共享。
 */

#ifndef __KERNEL_RTMUTEX_COMMON_H
#define __KERNEL_RTMUTEX_COMMON_H

#include <linux/debug_locks.h>
#include <linux/rtmutex.h>
#include <linux/sched/wake_q.h>


/*
 * This is a helper for the struct rt_mutex_waiter below. A waiter goes in two
 * separate trees and they need their own copy of the sort keys because of
 * different locking requirements.
 *
 * @entry:		rbtree node to enqueue into the waiters tree
 * @prio:		Priority of the waiter
 * @deadline:		Deadline of the waiter if applicable
 *
 * See rt_waiter_node_less() and waiter_*_prio().
 *
 * 这是下方 rt_mutex_waiter 的节点辅助结构。一个 waiter 会同时进入两棵树；由于两棵树受不同锁
 * 保护，不能共享会被并发改写的排序键副本。
 *
 * @entry：插入 waiter 红黑树的节点。
 * @prio：waiter 的有效优先级排序键。
 * @deadline：适用 deadline 调度时的截止期排序键。
 * 排序规则见 rt_waiter_node_less() 与 waiter_*_prio()。
 */
struct rt_waiter_node {
	struct rb_node	entry;
	int		prio;
	u64		deadline;
};

/*
 * This is the control structure for tasks blocked on a rt_mutex,
 * which is allocated on the kernel stack on of the blocked task.
 *
 * @tree:		node to enqueue into the mutex waiters tree
 * @pi_tree:		node to enqueue into the mutex owner waiters tree
 * @task:		task reference to the blocked task
 * @lock:		Pointer to the rt_mutex on which the waiter blocks
 * @wake_state:		Wakeup state to use (TASK_NORMAL or TASK_RTLOCK_WAIT)
 * @ww_ctx:		WW context pointer
 *
 * @tree is ordered by @lock->wait_lock
 * @pi_tree is ordered by rt_mutex_owner(@lock)->pi_lock
 *
 * 这是任务阻塞在 rt_mutex 上时的控制结构，通常分配在被阻塞任务自己的内核栈上，因此从排队到
 * 清理完成期间不得离开该栈帧。
 *
 * @tree：进入 mutex waiter 树的节点，由 @lock->wait_lock 排序和保护。
 * @pi_tree：进入 owner PI waiter 树的节点，由 rt_mutex_owner(@lock)->pi_lock 排序和保护。
 * @task：被阻塞任务的借用指针；@lock：正在等待的 rt_mutex_base。
 * @wake_state：使用 TASK_NORMAL 或 TASK_RTLOCK_WAIT 唤醒；@ww_ctx：可选 wound/wait 获取上下文。
 */
struct rt_mutex_waiter {
	struct rt_waiter_node	tree;
	struct rt_waiter_node	pi_tree;
	struct task_struct	*task;
	struct rt_mutex_base	*lock;
	unsigned int		wake_state;
	struct ww_acquire_ctx	*ww_ctx;
};

/**
 * struct rt_wake_q_head - Wrapper around regular wake_q_head to support
 *			   "sleeping" spinlocks on RT
 * @head:		The regular wake_q_head for sleeping lock variants
 * @rtlock_task:	Task pointer for RT lock (spin/rwlock) wakeups
 *
 * rt_wake_q_head 在普通 wake_q 外再保存一个 RT sleeping spin/rwlock 的直接唤醒目标。
 * @head 收集普通睡眠锁任务；@rtlock_task 保存至多一个 TASK_RTLOCK_WAIT 任务，二者在释放
 * wait_lock 后统一处理，避免在内部自旋锁下直接唤醒引入锁序和调度问题。
 */
struct rt_wake_q_head {
	struct wake_q_head	head;
	struct task_struct	*rtlock_task;
};

/* 定义一个空 RT 唤醒队列：普通 head 使用标准初始化器，专用 rtlock_task 初始为空。 */
#define DEFINE_RT_WAKE_Q(name)						\
	struct rt_wake_q_head name = {					\
		.head		= WAKE_Q_HEAD_INITIALIZER(name.head),	\
		.rtlock_task	= NULL,					\
	}

/*
 * PI-futex support (proxy locking functions, etc.):
 *
 * PI-futex 支持使用 proxy locking：内核可代表另一个任务建立 owner 或 waiter，再由该任务等待、
 * 清理；下列内部变体的 wait_lock 约束由 __must_hold 明确表达。
 */
/* 在持有 wait_lock 时把尚未公开竞争的 lock 初始化为由 proxy_owner 所有；无返回值。 */
extern void rt_mutex_init_proxy_locked(struct rt_mutex_base *lock,
				       struct task_struct *proxy_owner)
	__must_hold(&lock->wait_lock);

/* 在持有 wait_lock 且锁不会再被并发访问的 PI-futex 清理路径中清除 proxy owner。 */
extern void rt_mutex_proxy_unlock(struct rt_mutex_base *lock)
	__must_hold(&lock->wait_lock);

/*
 * 在已持 wait_lock 时代表 task 启动获取，把预初始化 waiter 无条件排队并把延迟唤醒加入 wake_q。
 * 返回 0 表示已阻塞、1 表示直接取得锁、负值表示死锁；失败也不移除 waiter，调用者随后必须 wait
 * 或 cleanup。末参数在本声明中匿名，是基线 checkpatch warning 的既有来源。
 */
extern int __rt_mutex_start_proxy_lock(struct rt_mutex_base *lock,
				     struct rt_mutex_waiter *waiter,
				     struct task_struct *task,
				     struct wake_q_head *)
	__must_hold(&lock->wait_lock);

/* 锁装版 proxy start：内部管理 wait_lock/wake_q，负值失败时会移除 waiter；其余返回语义同上。 */
extern int rt_mutex_start_proxy_lock(struct rt_mutex_base *lock,
				     struct rt_mutex_waiter *waiter,
				     struct task_struct *task);
/* 等待已由 proxy start 建立的获取；to 可为空，0 成功、负值失败，失败后调用者必须 cleanup。 */
extern int rt_mutex_wait_proxy_lock(struct rt_mutex_base *lock,
			       struct hrtimer_sleeper *to,
			       struct rt_mutex_waiter *waiter);
/* 清理失败的 proxy 等待；true 表示完成清理，false 表示竞态中已获锁、调用者应忽略先前失败。 */
extern bool rt_mutex_cleanup_proxy_lock(struct rt_mutex_base *lock,
				 struct rt_mutex_waiter *waiter);

/* PI-futex 专用 trylock 外层：自行取得 wait_lock，成功返回 1、竞争返回 0，不走普通 fastpath。 */
extern int rt_mutex_futex_trylock(struct rt_mutex_base *lock);
/* 上述 trylock 的已持 wait_lock 变体，返回语义相同。 */
extern int __rt_mutex_futex_trylock(struct rt_mutex_base *lock)
	__must_hold(&lock->wait_lock);

/* PI-futex 完整 unlock：管理 wait_lock，并在锁外执行必要的 owner 交接唤醒和优先级恢复。 */
extern void rt_mutex_futex_unlock(struct rt_mutex_base *lock);
/*
 * 已持 wait_lock 的 unlock 半部：无 waiter 时清 owner 并返回 false；有 waiter 时填充 wqh、完成交接
 * 前半部并返回 true。调用者释放 wait_lock 后必须对 true 配对调用 rt_mutex_postunlock()。
 */
extern bool __rt_mutex_futex_unlock(struct rt_mutex_base *lock,
				struct rt_wake_q_head *wqh);

/* 在 wait_lock 外消费 wqh，唤醒选中任务并完成与解锁前半部配对的抢占恢复。 */
extern void rt_mutex_postunlock(struct rt_wake_q_head *wqh);

/*
 * Must be guarded because this header is included from rcu/tree_plugin.h
 * unconditionally.
 *
 * 必须受 CONFIG_RT_MUTEXES 保护，因为 rcu/tree_plugin.h 会无条件包含本头文件。
 */
#ifdef CONFIG_RT_MUTEXES
/*
 * 判断 lock 的 waiter 红黑树是否非空。
 * 调用者必须持有 lock->wait_lock；返回非零表示至少存在一个排队者，不改变树或 owner 状态。
 */
static inline int rt_mutex_has_waiters(struct rt_mutex_base *lock)
	__must_hold(&lock->wait_lock)
{
	return !RB_EMPTY_ROOT(&lock->waiters.rb_root);
}

/*
 * Lockless speculative check whether @waiter is still the top waiter on
 * @lock. This is solely comparing pointers and not derefencing the
 * leftmost entry which might be about to vanish.
 *
 * 无额外加锁地推测 waiter 是否仍是 lock 的最高优先级等待者。这里只比较 leftmost 计算出的容器
 * 地址，不解引用可能即将消失的节点内容；常规调用受 wait_lock 约束，owner-spin 路径则以 data_race
 * 明确接受并发变化。返回值只是一瞬间的推测，不能赋予节点生命周期。
 */
static inline bool rt_mutex_waiter_is_top_waiter(struct rt_mutex_base *lock,
						 struct rt_mutex_waiter *waiter)
	__must_hold(&lock->wait_lock)
{
	/* cached leftmost 给出排序最前节点；空树时 rb_entry 结果仅参与地址比较，不被解引用。 */
	struct rb_node *leftmost = rb_first_cached(&lock->waiters);

	return rb_entry(leftmost, struct rt_mutex_waiter, tree.entry) == waiter;
}

/*
 * 在已持 wait_lock 时取得 lock 当前排序最前的 rt_mutex_waiter。
 * 空树返回 NULL；非空时从 tree.entry 恢复 waiter，并用 BUG_ON 验证其反向 lock 指针一致。
 * 返回的是受 wait_lock 稳定的借用指针，调用者不得在释放保护后无条件解引用。
 */
static inline struct rt_mutex_waiter *rt_mutex_top_waiter(struct rt_mutex_base *lock)
	__must_hold(&lock->wait_lock)
{
	/* leftmost 是缓存的最高优先级节点，w 保存最终可空返回值。 */
	struct rb_node *leftmost = rb_first_cached(&lock->waiters);
	struct rt_mutex_waiter *w = NULL;

	lockdep_assert_held(&lock->wait_lock);

	/* 仅非空时进行 container 恢复和归属一致性检查。 */
	if (leftmost) {
		w = rb_entry(leftmost, struct rt_mutex_waiter, tree.entry);
		BUG_ON(w->lock != lock);
	}
	return w;
}

/*
 * 判断任务 p 的 PI waiter 树是否非空。
 * 调用者应以 p->pi_lock 稳定该树；返回非零表示 p 正被至少一个 waiter 请求优先级继承。
 * 本 helper 只读根节点，不改变任务优先级或树内容。
 */
static inline int task_has_pi_waiters(struct task_struct *p)
{
	return !RB_EMPTY_ROOT(&p->pi_waiters.rb_root);
}

/*
 * 返回任务 p 的最高优先级 PI waiter。
 * 调用者必须持有 p->pi_lock，且必须先确认树非空；函数从缓存 leftmost 恢复 waiter 借用指针。
 * 空树调用不受支持，因为 rb_entry(NULL, ...) 不会产生可安全解引用的 NULL waiter。
 */
static inline struct rt_mutex_waiter *task_top_pi_waiter(struct task_struct *p)
{
	lockdep_assert_held(&p->pi_lock);

	return rb_entry(p->pi_waiters.rb_leftmost, struct rt_mutex_waiter,
			pi_tree.entry);
}

/*
 * Constants for rt mutex functions which have a selectable deadlock
 * detection.
 *
 * RT_MUTEX_MIN_CHAINWALK:	Stops the lock chain walk when there are
 *				no further PI adjustments to be made.
 *
 * RT_MUTEX_FULL_CHAINWALK:	Invoke deadlock detection with a full
 *				walk of the lock chain.
 *
 * 这些常量供可选择死锁检测强度的 RT mutex 函数使用：
 * RT_MUTEX_MIN_CHAINWALK 在 PI 已无需继续调整时提前停止，适合已知无需完整查环的常规路径；
 * RT_MUTEX_FULL_CHAINWALK 强制遍历完整阻塞链以检测死锁，代价更高但能给 proxy/futex 返回 -EDEADLK。
 */
enum rtmutex_chainwalk {
	RT_MUTEX_MIN_CHAINWALK,
	RT_MUTEX_FULL_CHAINWALK,
};

/*
 * 初始化一个尚未发布的 rt_mutex_base。
 * scoped_guard(raw_spinlock_init) 的构造动作只初始化 wait_lock，析构动作为空，并不会在运行时持锁；
 * 其词法作用域同时满足稀疏锁注解，再建立空 cached waiter 树和 NULL owner。调用者必须保证对象尚未
 * 发布、没有并发访问；函数不初始化外层 lockdep map。
 */
static inline void __rt_mutex_base_init(struct rt_mutex_base *lock)
{
	/* 初始化 guard 先建立 wait_lock，再在同一词法初始化区间写入其保护的两个字段。 */
	scoped_guard (raw_spinlock_init, &lock->wait_lock) {
		lock->waiters = RB_ROOT_CACHED;
		lock->owner = NULL;
	}
}

/* Debug functions */
/* 以下是调试辅助：关闭 CONFIG_DEBUG_RT_MUTEXES 时编译器会消除检查或填充，不改变正常路径。 */
/*
 * 在解锁前验证当前任务确为 lock owner。
 * 仅调试配置执行 DEBUG_LOCKS_WARN_ON；失败会报告并影响 debug_locks 状态，但本 helper 不清 owner。
 */
static inline void debug_rt_mutex_unlock(struct rt_mutex_base *lock)
{
	if (IS_ENABLED(CONFIG_DEBUG_RT_MUTEXES))
		DEBUG_LOCKS_WARN_ON(rt_mutex_owner(lock) != current);
}

/*
 * proxy unlock 不要求 owner 等于 current，但调试配置要求 lock 至少存在 owner。
 * 本 helper 只诊断空 owner，真正清除 proxy ownership 由调用者随后完成。
 */
static inline void debug_rt_mutex_proxy_unlock(struct rt_mutex_base *lock)
{
	if (IS_ENABLED(CONFIG_DEBUG_RT_MUTEXES))
		DEBUG_LOCKS_WARN_ON(!rt_mutex_owner(lock));
}

/*
 * 初始化栈上 waiter 前的调试填充。
 * 调试配置以 0x11 覆盖整个对象，使后续遗漏字段初始化更易暴露；非调试配置无副作用。
 * 调用者随后必须显式建立红黑树节点、wake_state 和 task 等有效字段。
 */
static inline void debug_rt_mutex_init_waiter(struct rt_mutex_waiter *waiter)
{
	if (IS_ENABLED(CONFIG_DEBUG_RT_MUTEXES))
		memset(waiter, 0x11, sizeof(*waiter));
}

/*
 * waiter 完成出队且不再被两棵树引用后的调试释放填充。
 * 调试配置以 0x22 毒化整个对象，帮助发现释放后使用；调用者必须确保没有并发读者。
 */
static inline void debug_rt_mutex_free_waiter(struct rt_mutex_waiter *waiter)
{
	if (IS_ENABLED(CONFIG_DEBUG_RT_MUTEXES))
		memset(waiter, 0x22, sizeof(*waiter));
}

/*
 * 把一个未排队的 rt_mutex_waiter 初始化为普通睡眠锁等待者。
 * 先执行调试填充，再清除两棵红黑树节点的链接标记，设置 TASK_NORMAL 并清空 task。
 * lock、ww_ctx 及排序键会在实际排队路径按目标锁和任务填写；初始化后尚不属于任何树。
 */
static inline void rt_mutex_init_waiter(struct rt_mutex_waiter *waiter)
{
	/* 顺序很重要：毒化必须先于有效字段赋值，避免覆盖刚建立的节点状态。 */
	debug_rt_mutex_init_waiter(waiter);
	RB_CLEAR_NODE(&waiter->pi_tree.entry);
	RB_CLEAR_NODE(&waiter->tree.entry);
	waiter->wake_state = TASK_NORMAL;
	waiter->task = NULL;
}

/*
 * 初始化 PREEMPT_RT sleeping spin/rwlock 使用的 waiter。
 * 复用普通 waiter 全部不变量，仅把 wake_state 改为 TASK_RTLOCK_WAIT，使唤醒进入 rtlock_task 专用通道。
 * 返回后仍未排队，后续字段填写和释放规则与 rt_mutex_init_waiter() 相同。
 */
static inline void rt_mutex_init_rtlock_waiter(struct rt_mutex_waiter *waiter)
{
	rt_mutex_init_waiter(waiter);
	waiter->wake_state = TASK_RTLOCK_WAIT;
}

#else /* CONFIG_RT_MUTEXES */
/* Used in rcu/tree_plugin.h */
/* CONFIG_RT_MUTEXES 关闭时仍供 rcu/tree_plugin.h 使用。 */
/*
 * 无 RT mutex 支持构建中的 owner 查询占位实现。
 * lock 参数不被访问，恒返回 NULL，使无条件包含本头文件的 RCU 代码可在编译期折叠相关判断。
 */
static inline struct task_struct *rt_mutex_owner(struct rt_mutex_base *lock)
{
	return NULL;
}
#endif  /* !CONFIG_RT_MUTEXES */
/* 结束 CONFIG_RT_MUTEXES 分支。 */

#endif
