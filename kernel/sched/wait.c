// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic waiting primitives.
 *
 * (C) 2004 Nadia Yvette Chambers, Oracle
 */
#include "sched.h"

/*
 * 初始化等待队列头：自旋锁串行化链表增删和唤醒遍历，lockdep 的 class/name
 * 只用于锁依赖诊断；新队列从空链表开始，尚不拥有任何等待项。
 */
void __init_waitqueue_head(struct wait_queue_head *wq_head, const char *name, struct lock_class_key *key)
{
	spin_lock_init(&wq_head->lock);
	lockdep_set_class_and_name(&wq_head->lock, key, name);
	INIT_LIST_HEAD(&wq_head->head);
}

EXPORT_SYMBOL(__init_waitqueue_head);

/*
 * 把普通（非独占）等待项插到优先级等待项之后、其他普通项之前；持锁期间关本地
 * 中断，使进程和中断上下文不能并发破坏同一链表。调用者负责避免重复入队。
 */
void add_wait_queue(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
	unsigned long flags;

	wq_entry->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq_head->lock, flags);
	__add_wait_queue(wq_head, wq_entry);
	spin_unlock_irqrestore(&wq_head->lock, flags);
}
EXPORT_SYMBOL(add_wait_queue);

/*
 * 把独占等待项放在队尾。唤醒端可只消费指定数量的独占项，从而避免同一事件造成
 * 惊群；非独占项仍可能全部得到回调。
 */
void add_wait_queue_exclusive(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
	unsigned long flags;

	wq_entry->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq_head->lock, flags);
	__add_wait_queue_entry_tail(wq_head, wq_entry);
	spin_unlock_irqrestore(&wq_head->lock, flags);
}
EXPORT_SYMBOL(add_wait_queue_exclusive);

/*
 * 注册优先级等待项；__add_wait_queue() 会令它排在普通等待项之前，但并不赋予独占
 * 语义，因此一次唤醒仍可继续扫描其后的队列项。
 */
void add_wait_queue_priority(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
	unsigned long flags;

	wq_entry->flags |= WQ_FLAG_PRIORITY;
	spin_lock_irqsave(&wq_head->lock, flags);
	__add_wait_queue(wq_head, wq_entry);
	spin_unlock_irqrestore(&wq_head->lock, flags);
}
EXPORT_SYMBOL_GPL(add_wait_queue_priority);

/*
 * 注册队首的“优先级且独占”消费者。队首已有优先级项时返回 -EBUSY，保证这类可
 * 独占消费事件的等待者最多一个；guard 在成功和失败返回时都会恢复中断并解锁。
 */
int add_wait_queue_priority_exclusive(struct wait_queue_head *wq_head,
				      struct wait_queue_entry *wq_entry)
{
	struct list_head *head = &wq_head->head;

	wq_entry->flags |= WQ_FLAG_EXCLUSIVE | WQ_FLAG_PRIORITY;

	guard(spinlock_irqsave)(&wq_head->lock);

	if (!list_empty(head) &&
	    (list_first_entry(head, typeof(*wq_entry), entry)->flags & WQ_FLAG_PRIORITY))
		return -EBUSY;

	list_add(&wq_entry->entry, head);
	return 0;
}
EXPORT_SYMBOL_GPL(add_wait_queue_priority_exclusive);

/*
 * 在等待队列锁保护下摘除指定项；摘除不改变任务状态，也不释放等待项，因为其
 * 生命周期由调用者（通常是栈上的等待循环）负责。
 */
void remove_wait_queue(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
	unsigned long flags;

	spin_lock_irqsave(&wq_head->lock, flags);
	__remove_wait_queue(wq_head, wq_entry);
	spin_unlock_irqrestore(&wq_head->lock, flags);
}
EXPORT_SYMBOL(remove_wait_queue);

/*
 * The core wakeup function. Non-exclusive wakeups (nr_exclusive == 0) just
 * wake everything up. If it's an exclusive wakeup (nr_exclusive == small +ve
 * number) then we wake that number of exclusive tasks, and potentially all
 * the non-exclusive tasks. Normally, exclusive tasks will be at the end of
 * the list and any non-exclusive tasks will be woken first. A priority task
 * may be at the head of the list, and can consume the event without any other
 * tasks being woken if it's also an exclusive task.
 *
 * There are circumstances in which we can try to wake a task which has already
 * started to run but is not in state TASK_RUNNING. try_to_wake_up() returns
 * zero in this (rare) case, and we handle it by continuing to scan the queue.
 */
/*
 * 核心遍历在队列锁内调用每项的唤醒函数。负返回值要求立即终止；正返回值仅在该项
 * 是独占项时消耗 nr_exclusive，失败的 try_to_wake_up 不消耗名额。返回尚未满足的
 * 独占名额，nr_exclusive 为 0 时其递减逻辑不会参与，因而扫描全部普通等待者。
 */
static int __wake_up_common(struct wait_queue_head *wq_head, unsigned int mode,
			int nr_exclusive, int wake_flags, void *key)
{
	wait_queue_entry_t *curr, *next;

	lockdep_assert_held(&wq_head->lock);

	curr = list_first_entry(&wq_head->head, wait_queue_entry_t, entry);

	if (&curr->entry == &wq_head->head)
		return nr_exclusive;

	list_for_each_entry_safe_from(curr, next, &wq_head->head, entry) {
		unsigned flags = curr->flags;
		int ret;

		ret = curr->func(curr, mode, wake_flags, key);
		if (ret < 0)
			break;
		if (ret && (flags & WQ_FLAG_EXCLUSIVE) && !--nr_exclusive)
			break;
	}

	return nr_exclusive;
}

/*
 * 为核心遍历提供 irq-safe 的锁封装，并把“剩余名额”转换成实际唤醒的独占项数；
 * 回调必须遵守等待队列锁内执行的约束，不能递归获取同一把锁。
 */
static int __wake_up_common_lock(struct wait_queue_head *wq_head, unsigned int mode,
			int nr_exclusive, int wake_flags, void *key)
{
	unsigned long flags;
	int remaining;

	spin_lock_irqsave(&wq_head->lock, flags);
	remaining = __wake_up_common(wq_head, mode, nr_exclusive, wake_flags,
			key);
	spin_unlock_irqrestore(&wq_head->lock, flags);

	return nr_exclusive - remaining;
}

/**
 * __wake_up - wake up threads blocked on a waitqueue.
 * @wq_head: the waitqueue
 * @mode: which threads
 * @nr_exclusive: how many wake-one or wake-many threads to wake up
 * @key: is directly passed to the wakeup function
 *
 * If this function wakes up a task, it executes a full memory barrier
 * before accessing the task state.  Returns the number of exclusive
 * tasks that were awaken.
 */
/*
 * 中文释义：唤醒 wq_head 上状态与 mode 匹配的任务，key 原样交给回调；成功唤醒
 * 任务时 try_to_wake_up 提供完整内存屏障，返回已唤醒的独占等待者数量。
 */
int __wake_up(struct wait_queue_head *wq_head, unsigned int mode,
	      int nr_exclusive, void *key)
{
	return __wake_up_common_lock(wq_head, mode, nr_exclusive, 0, key);
}
EXPORT_SYMBOL(__wake_up);

/*
 * 只请求一个独占名额，并提示调度器尽量在当前 CPU 完成唤醒；WF_CURRENT_CPU 是
 * 放置偏好而非成功保证，普通等待项仍按核心遍历规则处理。
 */
void __wake_up_on_current_cpu(struct wait_queue_head *wq_head, unsigned int mode, void *key)
{
	__wake_up_common_lock(wq_head, mode, 1, WF_CURRENT_CPU, key);
}

/*
 * Same as __wake_up but called with the spinlock in wait_queue_head_t held.
 */
/* 中文释义：与 __wake_up 相同，但调用者已经持有等待队列自旋锁，本函数不会重复加锁。 */
void __wake_up_locked(struct wait_queue_head *wq_head, unsigned int mode, int nr)
{
	__wake_up_common(wq_head, mode, nr, 0, NULL);
}
EXPORT_SYMBOL_GPL(__wake_up_locked);

/* 调用者持锁时按 key 唤醒至多一个独占等待者；NULL 与具体 key 的匹配由回调解释。 */
void __wake_up_locked_key(struct wait_queue_head *wq_head, unsigned int mode, void *key)
{
	__wake_up_common(wq_head, mode, 1, 0, key);
}
EXPORT_SYMBOL_GPL(__wake_up_locked_key);

/**
 * __wake_up_sync_key - wake up threads blocked on a waitqueue.
 * @wq_head: the waitqueue
 * @mode: which threads
 * @key: opaque value to be passed to wakeup targets
 *
 * The sync wakeup differs that the waker knows that it will schedule
 * away soon, so while the target thread will be woken up, it will not
 * be migrated to another CPU - ie. the two threads are 'synchronized'
 * with each other. This can prevent needless bouncing between CPUs.
 *
 * On UP it can prevent extra preemption.
 *
 * If this function wakes up a task, it executes a full memory barrier before
 * accessing the task state.
 */
/*
 * 中文释义：同步唤醒表示唤醒者即将主动调度出去，WF_SYNC 可避免把目标无谓迁往
 * 其他 CPU（UP 上也可避免额外抢占）；空队列头被容错忽略，成功唤醒仍有完整屏障。
 */
void __wake_up_sync_key(struct wait_queue_head *wq_head, unsigned int mode,
			void *key)
{
	if (unlikely(!wq_head))
		return;

	__wake_up_common_lock(wq_head, mode, 1, WF_SYNC, key);
}
EXPORT_SYMBOL_GPL(__wake_up_sync_key);

/**
 * __wake_up_locked_sync_key - wake up a thread blocked on a locked waitqueue.
 * @wq_head: the waitqueue
 * @mode: which threads
 * @key: opaque value to be passed to wakeup targets
 *
 * The sync wakeup differs in that the waker knows that it will schedule
 * away soon, so while the target thread will be woken up, it will not
 * be migrated to another CPU - ie. the two threads are 'synchronized'
 * with each other. This can prevent needless bouncing between CPUs.
 *
 * On UP it can prevent extra preemption.
 *
 * If this function wakes up a task, it executes a full memory barrier before
 * accessing the task state.
 */
/*
 * 中文释义：这是调用者已持有等待队列锁的同步唤醒版本；本层不检查空指针也不
 * 管理解锁，错误的锁所有权会破坏链表和中断状态。
 */
void __wake_up_locked_sync_key(struct wait_queue_head *wq_head,
			       unsigned int mode, void *key)
{
        __wake_up_common(wq_head, mode, 1, WF_SYNC, key);
}
EXPORT_SYMBOL_GPL(__wake_up_locked_sync_key);

/*
 * __wake_up_sync - see __wake_up_sync_key()
 */
/* 中文释义：无 key 的同步唤醒便捷入口，具体放置和屏障语义见 __wake_up_sync_key()。 */
void __wake_up_sync(struct wait_queue_head *wq_head, unsigned int mode)
{
	__wake_up_sync_key(wq_head, mode, NULL);
}
EXPORT_SYMBOL_GPL(__wake_up_sync);	/* For internal use only */

/*
 * 向 poll/epoll 观察者广播 POLLFREE，声明等待队列所属对象即将消失。回调必须同步
 * 摘空队列；残留项意味着观察者可能继续引用即将释放的 wq_head，故用 WARN 报错。
 */
void __wake_up_pollfree(struct wait_queue_head *wq_head)
{
	__wake_up(wq_head, TASK_NORMAL, 0, poll_to_key(EPOLLHUP | POLLFREE));
	/* POLLFREE must have cleared the queue. */
	WARN_ON_ONCE(waitqueue_active(wq_head));
}

/*
 * Note: we use "set_current_state()" _after_ the wait-queue add,
 * because we need a memory barrier there on SMP, so that any
 * wake-function that tests for the wait-queue being active
 * will be guaranteed to see waitqueue addition _or_ subsequent
 * tests in this thread will see the wakeup having taken place.
 *
 * The spin_unlock() itself is semi-permeable and only protects
 * one way (it only protects stuff inside the critical region and
 * stops them from bleeding out - it would still allow subsequent
 * loads to move into the critical region).
 */
/*
 * 中文释义：必须先在锁内入队，再用 set_current_state() 发布睡眠状态；后者的屏障
 * 保证唤醒者看到入队，或等待者看到条件已经成立，避免两边互相错过。普通等待项
 * 放在非优先级区域前部，重复调用只更新状态而不重复挂链。
 */
void
prepare_to_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state)
{
	unsigned long flags;

	wq_entry->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq_head->lock, flags);
	if (list_empty(&wq_entry->entry))
		__add_wait_queue(wq_head, wq_entry);
	set_current_state(state);
	spin_unlock_irqrestore(&wq_head->lock, flags);
}
EXPORT_SYMBOL(prepare_to_wait);

/* Returns true if we are the first waiter in the queue, false otherwise. */
/*
 * 中文释义：独占项首次入队时放到队尾，并返回入队前队列是否为空；此返回值只描述
 * 当次持锁快照，不给调用者保留队首所有权。状态发布与普通版本具有相同屏障语义。
 */
bool
prepare_to_wait_exclusive(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state)
{
	unsigned long flags;
	bool was_empty = false;

	wq_entry->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq_head->lock, flags);
	if (list_empty(&wq_entry->entry)) {
		was_empty = list_empty(&wq_head->head);
		__add_wait_queue_entry_tail(wq_head, wq_entry);
	}
	set_current_state(state);
	spin_unlock_irqrestore(&wq_head->lock, flags);
	return was_empty;
}
EXPORT_SYMBOL(prepare_to_wait_exclusive);

/*
 * 初始化由当前任务拥有的等待项，默认回调在成功唤醒后自动摘链；flags 决定普通、
 * 独占等策略，链表自环表示尚未入队，可安全交给 prepare/finish 协议。
 */
void init_wait_entry(struct wait_queue_entry *wq_entry, int flags)
{
	wq_entry->flags = flags;
	wq_entry->private = current;
	wq_entry->func = autoremove_wake_function;
	INIT_LIST_HEAD(&wq_entry->entry);
}
EXPORT_SYMBOL(init_wait_entry);

/*
 * 为 wait_event* 宏准备一次睡眠。信号已待处理时先在同一队列锁下摘链再返回
 * -ERESTARTSYS，使稍后产生的条件事件能交给别的独占等待者；否则按 flags 入队并
 * 发布 state。调用者返回后必须重新检查条件，才能区分已获事件与信号失败。
 */
long prepare_to_wait_event(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state)
{
	unsigned long flags;
	long ret = 0;

	spin_lock_irqsave(&wq_head->lock, flags);
	if (signal_pending_state(state, current)) {
		/*
		 * Exclusive waiter must not fail if it was selected by wakeup,
		 * it should "consume" the condition we were waiting for.
		 *
		 * The caller will recheck the condition and return success if
		 * we were already woken up, we can not miss the event because
		 * wakeup locks/unlocks the same wq_head->lock.
		 *
		 * But we need to ensure that set-condition + wakeup after that
		 * can't see us, it should wake up another exclusive waiter if
		 * we fail.
		 */
		/*
		 * 中文释义：若独占等待者已被唤醒选中，唤醒回调已将它摘链；此处再次
		 * list_del_init 仍安全，外层重查条件会把事件当作成功而不是被信号抢走。
		 */
		list_del_init(&wq_entry->entry);
		ret = -ERESTARTSYS;
	} else {
		if (list_empty(&wq_entry->entry)) {
			if (wq_entry->flags & WQ_FLAG_EXCLUSIVE)
				__add_wait_queue_entry_tail(wq_head, wq_entry);
			else
				__add_wait_queue(wq_head, wq_entry);
		}
		set_current_state(state);
	}
	spin_unlock_irqrestore(&wq_head->lock, flags);

	return ret;
}
EXPORT_SYMBOL(prepare_to_wait_event);

/*
 * Note! These two wait functions are entered with the
 * wait-queue lock held (and interrupts off in the _irq
 * case), so there is no race with testing the wakeup
 * condition in the caller before they add the wait
 * entry to the wake queue.
 */
/*
 * 中文释义：调用者带着 wq->lock 进入，函数确保等待项在队尾并发布可中断状态。
 * 有信号时保持锁不变返回 -ERESTARTSYS；正常路径临时解锁调度，醒来后重新持锁，
 * 因而调用者可在同一临界区再次判断条件或清理等待项。
 */
int do_wait_intr(wait_queue_head_t *wq, wait_queue_entry_t *wait)
{
	if (likely(list_empty(&wait->entry)))
		__add_wait_queue_entry_tail(wq, wait);

	set_current_state(TASK_INTERRUPTIBLE);
	if (signal_pending(current))
		return -ERESTARTSYS;

	spin_unlock(&wq->lock);
	schedule();
	spin_lock(&wq->lock);

	return 0;
}
EXPORT_SYMBOL(do_wait_intr);

/*
 * do_wait_intr() 的中断关闭版本：入口要求锁已持有且本地中断已关闭，睡眠前用
 * spin_unlock_irq() 同时开放中断，醒来用 spin_lock_irq() 恢复原协议。
 */
int do_wait_intr_irq(wait_queue_head_t *wq, wait_queue_entry_t *wait)
{
	if (likely(list_empty(&wait->entry)))
		__add_wait_queue_entry_tail(wq, wait);

	set_current_state(TASK_INTERRUPTIBLE);
	if (signal_pending(current))
		return -ERESTARTSYS;

	spin_unlock_irq(&wq->lock);
	schedule();
	spin_lock_irq(&wq->lock);

	return 0;
}
EXPORT_SYMBOL(do_wait_intr_irq);

/**
 * finish_wait - clean up after waiting in a queue
 * @wq_head: waitqueue waited on
 * @wq_entry: wait descriptor
 *
 * Sets current thread back to running state and removes
 * the wait descriptor from the given waitqueue if still
 * queued.
 */
/*
 * 中文释义：等待结束时先无屏障地恢复 TASK_RUNNING，再在必要时摘除仍挂链的描述符。
 * 无锁快速检查仅因 careful 同时核对 next/prev 且其他访问者都持同一锁才安全；真正
 * 修改仍在 irq-safe 锁内，返回后栈上的 wq_entry 才可销毁。
 */
void finish_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
	unsigned long flags;

	__set_current_state(TASK_RUNNING);
	/*
	 * We can check for list emptiness outside the lock
	 * IFF:
	 *  - we use the "careful" check that verifies both
	 *    the next and prev pointers, so that there cannot
	 *    be any half-pending updates in progress on other
	 *    CPU's that we haven't seen yet (and that might
	 *    still change the stack area.
	 * and
	 *  - all other users take the lock (ie we can only
	 *    have _one_ other CPU that looks at or modifies
	 *    the list).
	 */
	if (!list_empty_careful(&wq_entry->entry)) {
		spin_lock_irqsave(&wq_head->lock, flags);
		list_del_init(&wq_entry->entry);
		spin_unlock_irqrestore(&wq_head->lock, flags);
	}
}
EXPORT_SYMBOL(finish_wait);

/*
 * 先调用默认任务唤醒函数；仅当其报告成功时用 careful 版本自动摘链。失败时保留
 * 等待项供后续事件重试，避免一次竞态中的失败唤醒永久丢失等待者。
 */
int autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key)
{
	int ret = default_wake_function(wq_entry, mode, sync, key);

	if (ret)
		list_del_init_careful(&wq_entry->entry);

	return ret;
}
EXPORT_SYMBOL(autoremove_wake_function);

/*
 * DEFINE_WAIT_FUNC(wait, woken_wake_func);
 *
 * add_wait_queue(&wq_head, &wait);
 * for (;;) {
 *     if (condition)
 *         break;
 *
 *     // in wait_woken()			// in woken_wake_function()
 *
 *     p->state = mode;				wq_entry->flags |= WQ_FLAG_WOKEN;
 *     smp_mb(); // A				try_to_wake_up():
 *     if (!(wq_entry->flags & WQ_FLAG_WOKEN))	   <full barrier>
 *         schedule()				   if (p->state & mode)
 *     p->state = TASK_RUNNING;			      p->state = TASK_RUNNING;
 *     wq_entry->flags &= ~WQ_FLAG_WOKEN;	~~~~~~~~~~~~~~~~~~
 *     smp_mb(); // B				condition = true;
 * }						smp_mb(); // C
 * remove_wait_queue(&wq_head, &wait);		wq_entry->flags |= WQ_FLAG_WOKEN;
 */
/*
 * 中文释义：等待端 A 屏障与唤醒端 try_to_wake_up 的完整屏障配对，保证至少一方
 * 看见对方的状态/标志写入；清除 WOKEN 的 B 屏障再与唤醒端 C 配对，保证条件写入
 * 和下一轮 WOKEN 不会同时被漏看。超时返回剩余 jiffies，停止/park 时不再睡眠。
 */
long wait_woken(struct wait_queue_entry *wq_entry, unsigned mode, long timeout)
{
	/*
	 * The below executes an smp_mb(), which matches with the full barrier
	 * executed by the try_to_wake_up() in woken_wake_function() such that
	 * either we see the store to wq_entry->flags in woken_wake_function()
	 * or woken_wake_function() sees our store to current->state.
	 */
	set_current_state(mode); /* A */
	if (!(wq_entry->flags & WQ_FLAG_WOKEN) && !kthread_should_stop_or_park())
		timeout = schedule_timeout(timeout);
	__set_current_state(TASK_RUNNING);

	/*
	 * The below executes an smp_mb(), which matches with the smp_mb() (C)
	 * in woken_wake_function() such that either we see the wait condition
	 * being true or the store to wq_entry->flags in woken_wake_function()
	 * follows ours in the coherence order.
	 */
	smp_store_mb(wq_entry->flags, wq_entry->flags & ~WQ_FLAG_WOKEN); /* B */

	return timeout;
}
EXPORT_SYMBOL(wait_woken);

/*
 * 先用 C 屏障建立与等待端清标志操作的次序，再设置 WOKEN 并尝试唤醒任务。即使
 * 默认回调因状态不匹配返回 0，WOKEN 仍记录事件，等待端下一次也不会盲目睡眠。
 */
int woken_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key)
{
	/* Pairs with the smp_store_mb() in wait_woken(). */
	smp_mb(); /* C */
	wq_entry->flags |= WQ_FLAG_WOKEN;

	return default_wake_function(wq_entry, mode, sync, key);
}
EXPORT_SYMBOL(woken_wake_function);
