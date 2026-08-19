// SPDX-License-Identifier: GPL-2.0-only
/*
 * 本文件实现计数信号量：初始 count=N 时，可以连续成功获取 N 次，此后获取者
 * 才进入睡眠。它没有 mutex 那样的严格 owner，许可可以由不同任务甚至中断
 * 上下文归还；需要单一所有者、所有权校验和优先级继承时应改用 mutex。
 */
/*
 * Copyright (c) 2008 Intel Corporation
 * Author: Matthew Wilcox <willy@linux.intel.com>
 *
 * This file implements counting semaphores.
 * A counting semaphore may be acquired 'n' times before sleeping.
 * See mutex.c for single-acquisition sleeping locks which enforce
 * rules which allow code to be debugged more easily.
 */

/*
 * 实现约束如下：sem->lock 串行保护 count、first_waiter 及整个等待者环。
 * down_trylock() 和 up() 允许在中断上下文调用，因此取得内部锁时必须关闭本地
 * 中断。历史调用者也可能在确信 down 类操作不会阻塞时从中断上下文使用它们，
 * 所以所有获取入口统一采用 irqsave 变体；一旦真的走到睡眠路径，仍违反约束。
 * count 仅表示还可直接分配的许可数，等于零时可能存在等待者，但不以负数编码
 * 等待者数量。first_waiter 是否为空才是慢路径队列的权威状态。
 */
/*
 * Some notes on the implementation:
 *
 * The spinlock controls access to the other members of the semaphore.
 * down_trylock() and up() can be called from interrupt context, so we
 * have to disable interrupts when taking the lock.  It turns out various
 * parts of the kernel expect to be able to use down() on a semaphore in
 * interrupt context when they know it will succeed, so we have to use
 * irqsave variants for down(), down_interruptible() and down_killable()
 * too.
 *
 * The ->count variable represents how many more tasks can acquire this
 * semaphore.  If it's zero, there may be waiters.
 */

#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/wake_q.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/ftrace.h>
#include <trace/events/lock.h>
#include <linux/hung_task.h>

static noinline void __down(struct semaphore *sem);
static noinline int __down_interruptible(struct semaphore *sem);
static noinline int __down_killable(struct semaphore *sem);
static noinline int __down_timeout(struct semaphore *sem, long timeout);
static noinline void __up(struct semaphore *sem, struct wake_q_head *wake_q);

#ifdef CONFIG_DETECT_HUNG_TASK_BLOCKER
/*
 * 记录当前成功取得许可的任务，供 hung-task 报告给出“最近持有者”线索。
 * 该字段不是所有权协议，只以 WRITE_ONCE 避免撕裂；计数信号量可以跨任务释放，
 * 所以诊断结果只能理解为 likely last holder。
 */
static inline void hung_task_sem_set_holder(struct semaphore *sem)
{
	WRITE_ONCE((sem)->last_holder, (unsigned long)current);
}

/*
 * 当前任务归还许可时，仅在自己仍是已记录的最近持有者时清零诊断字段。
 * 若已有另一任务后来取得许可，则保留较新的线索；不改变信号量正确性状态。
 */
static inline void hung_task_sem_clear_if_holder(struct semaphore *sem)
{
	if (READ_ONCE((sem)->last_holder) == (unsigned long)current)
		WRITE_ONCE((sem)->last_holder, 0UL);
}

/*
 * 无锁读取 hung-task 使用的最近持有者快照。返回值可能已过时且不附带任务引用，
 * 调用者只能在自己的生命周期保护下用于诊断，不能把它当成 owner 判定。
 */
unsigned long sem_last_holder(struct semaphore *sem)
{
	return READ_ONCE(sem->last_holder);
}
#else
/* 未启用阻塞者检测时，成功获取无需维护诊断状态。 */
static inline void hung_task_sem_set_holder(struct semaphore *sem)
{
}

/* 未启用阻塞者检测时，释放许可无需清理诊断状态。 */
static inline void hung_task_sem_clear_if_holder(struct semaphore *sem)
{
}

/* 未启用阻塞者检测时没有最近持有者信息，固定返回 0。 */
unsigned long sem_last_holder(struct semaphore *sem)
{
	return 0UL;
}
#endif

/*
 * 在持有 sem->lock 且 count>0 时消耗一个许可，并更新可选诊断线索。
 * 调用者负责 IRQ 状态和锁；返回后 count 恰减一，不涉及等待队列或调度。
 */
static inline void __sem_acquire(struct semaphore *sem)
{
	sem->count--;
	hung_task_sem_set_holder(sem);
}

/**
 * down - acquire the semaphore
 * @sem: the semaphore to be acquired
 *
 * Acquires the semaphore.  If no more tasks are allowed to acquire the
 * semaphore, calling this function will put the task to sleep until the
 * semaphore is released.
 *
 * Use of this function is deprecated, please use down_interruptible() or
 * down_killable() instead.
 */
/*
 * 不可中断地取得一个许可；成功无返回值。
 * 快路径在 sem->lock 下直接减少正 count；无许可时进入 TASK_UNINTERRUPTIBLE
 * FIFO 等待，直到 up() 把许可直接交付。函数可能睡眠，官方建议新代码优先用
 * 可中断或可致命信号中断的版本，避免永久 D 状态等待。
 */
void __sched down(struct semaphore *sem)
{
	unsigned long flags;

	might_sleep();
	raw_spin_lock_irqsave(&sem->lock, flags);
	if (likely(sem->count > 0))
		__sem_acquire(sem);
	else
		__down(sem);
	raw_spin_unlock_irqrestore(&sem->lock, flags);
}
EXPORT_SYMBOL(down);

/**
 * down_interruptible - acquire the semaphore unless interrupted
 * @sem: the semaphore to be acquired
 *
 * Attempts to acquire the semaphore.  If no more tasks are allowed to
 * acquire the semaphore, calling this function will put the task to sleep.
 * If the sleep is interrupted by a signal, this function will return -EINTR.
 * If the semaphore is successfully acquired, this function returns 0.
 */
/*
 * 可中断地取得一个许可。成功返回 0；等待期间出现当前状态接受的普通信号时，
 * 在 sem->lock 下安全摘除自己的栈上 waiter 并返回 -EINTR。失败不消耗许可，
 * 成功则与 down() 一样由快路径扣减或由 up() 直接交付。函数可能睡眠。
 */
int __sched down_interruptible(struct semaphore *sem)
{
	unsigned long flags;
	int result = 0;

	might_sleep();
	raw_spin_lock_irqsave(&sem->lock, flags);
	if (likely(sem->count > 0))
		__sem_acquire(sem);
	else
		result = __down_interruptible(sem);
	raw_spin_unlock_irqrestore(&sem->lock, flags);

	return result;
}
EXPORT_SYMBOL(down_interruptible);

/**
 * down_killable - acquire the semaphore unless killed
 * @sem: the semaphore to be acquired
 *
 * Attempts to acquire the semaphore.  If no more tasks are allowed to
 * acquire the semaphore, calling this function will put the task to sleep.
 * If the sleep is interrupted by a fatal signal, this function will return
 * -EINTR.  If the semaphore is successfully acquired, this function returns
 * 0.
 */
/*
 * 以 TASK_KILLABLE 取得一个许可。成功返回 0；只有致命信号能终止等待并返回
 * -EINTR，普通非致命信号不会打断。超时为无限，失败摘队后不消耗许可。
 * 函数可能睡眠，适合既需 D 状态语义又必须允许任务被杀死的路径。
 */
int __sched down_killable(struct semaphore *sem)
{
	unsigned long flags;
	int result = 0;

	might_sleep();
	raw_spin_lock_irqsave(&sem->lock, flags);
	if (likely(sem->count > 0))
		__sem_acquire(sem);
	else
		result = __down_killable(sem);
	raw_spin_unlock_irqrestore(&sem->lock, flags);

	return result;
}
EXPORT_SYMBOL(down_killable);

/**
 * down_trylock - try to acquire the semaphore, without waiting
 * @sem: the semaphore to be acquired
 *
 * Try to acquire the semaphore atomically.  Returns 0 if the semaphore has
 * been acquired successfully or 1 if it cannot be acquired.
 *
 * NOTE: This return value is inverted from both spin_trylock and
 * mutex_trylock!  Be careful about this when converting code.
 *
 * Unlike mutex_trylock, this function can be used from interrupt context,
 * and the semaphore can be released by any task or interrupt.
 */
/*
 * 原子尝试取得一个许可而不等待，可从中断上下文调用。
 * 返回约定与 spin_trylock()/mutex_trylock() 相反：0 表示成功且 count 已减一，
 * 1 表示当时无许可且状态不变。信号量无所有者，成功取得的许可可由其他任务
 * 或中断通过 up() 归还；迁移代码时尤其不能把返回值直接当布尔“成功”。
 */
int __sched down_trylock(struct semaphore *sem)
{
	unsigned long flags;
	int count;

	raw_spin_lock_irqsave(&sem->lock, flags);
	count = sem->count - 1;
	if (likely(count >= 0))
		__sem_acquire(sem);
	raw_spin_unlock_irqrestore(&sem->lock, flags);

	return (count < 0);
}
EXPORT_SYMBOL(down_trylock);

/**
 * down_timeout - acquire the semaphore within a specified time
 * @sem: the semaphore to be acquired
 * @timeout: how long to wait before failing
 *
 * Attempts to acquire the semaphore.  If no more tasks are allowed to
 * acquire the semaphore, calling this function will put the task to sleep.
 * If the semaphore is not released within the specified number of jiffies,
 * this function returns -ETIME.  It returns 0 if the semaphore was acquired.
 */
/*
 * 在指定 jiffies 预算内取得一个许可。成功返回 0；预算耗尽时在内部锁下摘除
 * waiter 并返回 -ETIME，失败不消耗许可。等待状态不可被信号中断；timeout 会
 * 随 schedule_timeout() 返回的剩余时间递减，函数可能睡眠。
 */
int __sched down_timeout(struct semaphore *sem, long timeout)
{
	unsigned long flags;
	int result = 0;

	might_sleep();
	raw_spin_lock_irqsave(&sem->lock, flags);
	if (likely(sem->count > 0))
		__sem_acquire(sem);
	else
		result = __down_timeout(sem, timeout);
	raw_spin_unlock_irqrestore(&sem->lock, flags);

	return result;
}
EXPORT_SYMBOL(down_timeout);

/**
 * up - release the semaphore
 * @sem: the semaphore to release
 *
 * Release the semaphore.  Unlike mutexes, up() may be called from any
 * context and even by tasks which have never called down().
 */
/*
 * 归还一个许可，可从任意任务或中断上下文调用，调用者不必是获取者。
 * 无等待者时直接增加 count；有等待者时不增加 count，而是在 sem->lock 下把
 * 许可交给 FIFO 队首并加入局部 wake_q。解锁和恢复 IRQ 后才执行真正唤醒，
 * 既避免在内部锁下进入调度器，也由 wake_q 持有 task 引用保护其生命周期。
 */
void __sched up(struct semaphore *sem)
{
	unsigned long flags;
	DEFINE_WAKE_Q(wake_q);

	raw_spin_lock_irqsave(&sem->lock, flags);

	hung_task_sem_clear_if_holder(sem);

	if (likely(!sem->first_waiter))
		sem->count++;
	else
		__up(sem, &wake_q);

	if (trace_contended_release_enabled() && !wake_q_empty(&wake_q))
		trace_call__contended_release(sem);

	raw_spin_unlock_irqrestore(&sem->lock, flags);
	if (!wake_q_empty(&wake_q))
		wake_up_q(&wake_q);
}
EXPORT_SYMBOL(up);

/* 以下实体与函数处理 count 已耗尽后的争用路径。 */
/* Functions for the contended case */

/*
 * 每个睡眠获取者在自己的内核栈上保存一个 waiter。
 * list 把所有等待者组织为以首节点为锚的环；task 是待唤醒任务；up 是受
 * sem->lock 保护的交付标志，只有释放方摘队并把许可授予该任务时才置真。
 */
struct semaphore_waiter {
	struct list_head list;
	struct task_struct *task;
	bool up;
};

/*
 * 在持有 sem->lock 时从环形 FIFO 中删除 waiter，并维护 first_waiter。
 * 单节点环删除后把 first_waiter 清空；删除队首时先推进到下一节点；删除普通
 * 节点只需摘链。waiter 位于等待任务栈上，返回后调用者负责其剩余生命周期。
 */
static inline
void sem_del_waiter(struct semaphore *sem, struct semaphore_waiter *waiter)
{
	if (list_empty(&waiter->list)) {
		sem->first_waiter = NULL;
		return;
	}

	if (sem->first_waiter == waiter) {
		sem->first_waiter = list_first_entry(&waiter->list,
						     struct semaphore_waiter, list);
	}
	list_del(&waiter->list);
}

/*
 * 内联使 state 在四个包装器中成为编译期常量，未使用的信号判断可被裁剪；
 * 无限等待版本的 timeout 同样会被常量传播，避免为统一状态机支付运行时分支。
 */
/*
 * Because this function is inlined, the 'state' parameter will be
 * constant, and thus optimised away by the compiler.  Likewise the
 * 'timeout' parameter for the cases without timeouts.
 */
/*
 * 争用获取的核心睡眠循环；进入和返回时都持有 sem->lock 且本地 IRQ 关闭。
 *
 * 先把栈上 waiter 追加到 first_waiter 锚定的 FIFO 环并记录 current。循环中在
 * 锁内仲裁信号、超时和 up 交付，设置任务状态后解锁开 IRQ、按剩余 timeout
 * 调度，再关 IRQ 重取锁。看到 waiter.up=true 表示释放方已摘队并直接交付许可，
 * 返回 0 且不再修改 count；信号或超时则自行摘队，分别返回 -EINTR/-ETIME。
 */
static inline int __sched ___down_common(struct semaphore *sem, long state,
								long timeout)
{
	struct semaphore_waiter waiter, *first;

	first = sem->first_waiter;
	if (first) {
		list_add_tail(&waiter.list, &first->list);
	} else {
		INIT_LIST_HEAD(&waiter.list);
		sem->first_waiter = &waiter;
	}
	waiter.task = current;
	waiter.up = false;

	for (;;) {
		if (signal_pending_state(state, current))
			goto interrupted;
		if (unlikely(timeout <= 0))
			goto timed_out;
		__set_current_state(state);
		raw_spin_unlock_irq(&sem->lock);
		timeout = schedule_timeout(timeout);
		raw_spin_lock_irq(&sem->lock);
		if (waiter.up) {
			hung_task_sem_set_holder(sem);
			return 0;
		}
	}

 timed_out:
	sem_del_waiter(sem, &waiter);
	return -ETIME;

 interrupted:
	sem_del_waiter(sem, &waiter);
	return -EINTR;
}

/*
 * 为核心睡眠循环补充 hung-task 阻塞对象和锁争用 trace 生命周期。
 * 继承 ___down_common() 的锁/IRQ 前后条件，原样返回 0、-EINTR 或 -ETIME；
 * 无论结果如何都在返回前清除 current->blocker，避免诊断状态泄漏到后续代码。
 */
static inline int __sched __down_common(struct semaphore *sem, long state,
					long timeout)
{
	int ret;

	hung_task_set_blocker(sem, BLOCKER_TYPE_SEM);

	trace_contention_begin(sem, 0);
	ret = ___down_common(sem, state, timeout);
	trace_contention_end(sem, ret);

	hung_task_clear_blocker();

	return ret;
}

/*
 * down() 的不可中断、无限期慢路径适配器。
 * 调用时 sem->lock 已持有且 IRQ 已关闭；只有 up() 交付许可后返回，返回时仍
 * 保持该锁和 IRQ 状态，供公开入口统一解锁恢复。
 */
static noinline void __sched __down(struct semaphore *sem)
{
	__down_common(sem, TASK_UNINTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT);
}

/*
 * down_interruptible() 的可中断、无限期慢路径适配器。
 * 返回 0 表示获得许可，-EINTR 表示信号先赢得锁内仲裁；返回时内部锁仍持有。
 */
static noinline int __sched __down_interruptible(struct semaphore *sem)
{
	return __down_common(sem, TASK_INTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT);
}

/*
 * down_killable() 的致命信号可中断、无限期慢路径适配器。
 * 返回 0 表示获得许可，-EINTR 表示致命信号先赢得锁内仲裁；普通信号被忽略。
 */
static noinline int __sched __down_killable(struct semaphore *sem)
{
	return __down_common(sem, TASK_KILLABLE, MAX_SCHEDULE_TIMEOUT);
}

/*
 * down_timeout() 的不可中断、有限期慢路径适配器。
 * timeout 以 jiffies 表示；返回 0 表示在期限内获许可，-ETIME 表示预算先耗尽。
 */
static noinline int __sched __down_timeout(struct semaphore *sem, long timeout)
{
	return __down_common(sem, TASK_UNINTERRUPTIBLE, timeout);
}

/*
 * 在持有 sem->lock 时把一个许可直接交给 FIFO 队首。
 * 摘除 first_waiter 后把其 up 标志置真，再用 wake_q_add() 取得 task 引用并登记
 * 延后唤醒；count 保持为零，因为许可已经保留给该 waiter。调用者必须在解锁后
 * 消费 wake_q，返回前队首任务即使尚未运行，其栈对象也受睡眠循环和任务引用保护。
 */
static noinline void __sched __up(struct semaphore *sem,
				  struct wake_q_head *wake_q)
{
	struct semaphore_waiter *waiter = sem->first_waiter;

	sem_del_waiter(sem, waiter);
	waiter->up = true;
	wake_q_add(wake_q, waiter->task);
}
