/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mutexes: blocking mutual exclusion locks
 *
 * started by Ingo Molnar:
 *
 *  Copyright (C) 2004, 2005, 2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 */
/* 本内部头描述可睡眠互斥锁的慢路径私有协议；PREEMPT_RT 的 mutex 改由 rtmutex 实现，跳过本文件。 */
#ifndef CONFIG_PREEMPT_RT
#include <linux/mutex.h>
/*
 * This is the control structure for tasks blocked on mutex, which resides
 * on the blocked task's kernel stack:
 */
/*
 * 每个阻塞任务在自己的内核栈上创建一个 waiter，等待期间由 mutex 队列借用；函数返回前必须退队，
 * 因而任何指针都不得越过该任务的慢路径栈帧。list 连接等待队列，task 指向等待者，ww_ctx 携带
 * wound/wait 获取上下文；开启调试时 magic 用自指/poison 检查初始化、入退队和释放生命周期。
 */
struct mutex_waiter {
	/* 等待队列节点；单 waiter 时可形成自环，first_waiter 另行指向队首。 */
	struct list_head	list;
	/* 正在等待的任务；只在 waiter 栈帧有效期间由队列借用。 */
	struct task_struct	*task;
	/* 普通 mutex 为 NULL，ww_mutex 路径借用调用者的死锁规避上下文。 */
	struct ww_acquire_ctx	*ww_ctx;
#ifdef CONFIG_DEBUG_MUTEXES
	/* 调试身份/poison 字段，不参与互斥或队列排序。 */
	void			*magic;
#endif
};

/*
 * @owner: contains: 'struct task_struct *' to the current lock owner,
 * NULL means not owned. Since task_struct pointers are aligned at
 * at least L1_CACHE_BYTES, we have low bits to store extra state.
 *
 * Bit0 indicates a non-empty waiter list; unlock must issue a wakeup.
 * Bit1 indicates unlock needs to hand the lock to the top-waiter
 * Bit2 indicates handoff has been done and we're waiting for pickup.
 */
/*
 * owner 原子字的高位保存对齐的 task_struct 指针，NULL 表示无 owner；低三位编码竞争状态。
 * WAITERS 表示等待队列非空并要求 unlock 检查唤醒；队首可在 owner 仍存在时置 HANDOFF 请求直接
 * 交接；unlock 随后写入目标 task、保留 WAITERS、清 HANDOFF 并置 PICKUP，目标 waiter 最终以
 * acquire cmpxchg 清 PICKUP 完成交接。三位总掩码用于从任意 owner 快照分离指针与状态。
 */
#define MUTEX_FLAG_WAITERS	0x01
#define MUTEX_FLAG_HANDOFF	0x02
#define MUTEX_FLAG_PICKUP	0x04

#define MUTEX_FLAGS		0x07

/*
 * Internal helper function; C doesn't allow us to hide it :/
 *
 * DO NOT USE (outside of mutex & scheduler code).
 */
/*
 * 返回 lock 当前 owner 的去标志快照；lock 为 NULL 或无 owner 时返回 NULL。读取不加 wait_lock、
 * 不增加 task 引用，也不保证返回后 owner 不变；仅 mutex/调度器在自身 RCU、抢占或比较协议内使用。
 */
static inline struct task_struct *__mutex_owner(struct mutex *lock)
{
	if (!lock)
		return NULL;
	return (struct task_struct *)(atomic_long_read(&lock->owner) & ~MUTEX_FLAGS);
}

/*
 * 在 p->blocked_lock 的 irqsave 保护下读取 p 正等待的 mutex，并返回无引用快照；未阻塞时为 NULL。
 * scoped guard 在函数退出前自动释放锁，因此调用者不能把返回值视为稳定关系或据此接管对象所有权。
 */
static inline struct mutex *get_task_blocked_on(struct task_struct *p)
{
	guard(raw_spinlock_irqsave)(&p->blocked_lock);
	return __get_task_blocked_on(p);
}

#ifdef CONFIG_DEBUG_MUTEXES
/* 在已持 lock->wait_lock 时初始化栈上 waiter 的 list、magic 与 ww_ctx poison；无返回值。 */
extern void debug_mutex_lock_common(struct mutex *lock,
				    struct mutex_waiter *waiter);
/* 在已持 wait_lock 的唤醒路径校验队首存在及 waiter 身份；无返回值，不改变队列 ownership。 */
extern void debug_mutex_wake_waiter(struct mutex *lock,
				    struct mutex_waiter *waiter);
/* 校验 waiter 已退队后 poison 整个栈上记录；无返回值，调用后不得再读取其字段。 */
extern void debug_mutex_free_waiter(struct mutex_waiter *waiter);
/* 在已持 wait_lock 的入队前校验 task 尚未登记 blocked_on；无返回值，参数均为借用。 */
extern void debug_mutex_add_waiter(struct mutex *lock,
				   struct mutex_waiter *waiter,
				   struct task_struct *task);
/* 校验 waiter/task/blocked_on 一致性并清理 waiter 的调试字段；无返回值，要求受 wait_lock 串行化。 */
extern void debug_mutex_remove_waiter(struct mutex *lock, struct mutex_waiter *waiter,
				      struct task_struct *task);
/* 在 unlock 慢路径校验 lock 的初始化 magic；无返回值，不执行实际 owner 释放。 */
extern void debug_mutex_unlock(struct mutex *lock);
/* 把 lock->magic 初始化为自指调试哨兵；无返回值，不初始化 owner 或 wait_lock。 */
extern void debug_mutex_init(struct mutex *lock);
#else /* CONFIG_DEBUG_MUTEXES */
/*
 * 关闭 DEBUG_MUTEXES 时保留同名空 hook，使主状态机无需配置分叉；宏形参不会被展开求值，因此传入
 * 只为调试准备的表达式也无运行期开销或副作用。真实 mutex/等待队列语义完全由非调试路径承担。
 */
# define debug_mutex_lock_common(lock, waiter)		do { } while (0)
# define debug_mutex_wake_waiter(lock, waiter)		do { } while (0)
# define debug_mutex_free_waiter(waiter)		do { } while (0)
# define debug_mutex_add_waiter(lock, waiter, ti)	do { } while (0)
# define debug_mutex_remove_waiter(lock, waiter, ti)	do { } while (0)
# define debug_mutex_unlock(lock)			do { } while (0)
# define debug_mutex_init(lock)				do { } while (0)
#endif /* !CONFIG_DEBUG_MUTEXES */
#endif /* CONFIG_PREEMPT_RT */
