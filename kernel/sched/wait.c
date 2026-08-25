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
/*
 * 业务背景：等待事件的生产者和消费者需要共享一个可加锁的队列头，本函数是对象启用前的初始化入口。
 * 入参：wq_head 是调用者拥有的可写输出对象且不可为空；name 和 key 是借给 lockdep 的诊断标识。
 * name/key 均不可为空；lockdep 保存其地址，因此字符串和 key 必须至少与队列头同寿命且不转移 ownership。
 * 出参/返回：无直接返回值；初始化 wq_head 的锁、锁类和空链表，不转移三个参数的 ownership。
 * 注意事项：对象尚未初始化或仍有等待者时都不能并发调用；本函数不睡眠，也不负责释放队列头。
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
/*
 * 业务背景：需要广播唤醒的普通消费者必须先登记到等待队列，供后续 wake_up 路径遍历。
 * 入参：wq_head 是已初始化的借用队列；wq_entry 是调用者拥有的可写输入输出项且两者均不可为空。
 * 出参/返回：无直接返回值；清除 entry 的独占标志并将其挂链，ownership 仍归调用者。
 * 注意事项：调用者保证 entry 尚未挂入其他队列并维持其生命周期；内部 irq-safe 锁不会睡眠。
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
/*
 * 业务背景：互斥消费同一事件的等待者应排在队尾并按唤醒名额领取事件，以抑制惊群。
 * 入参：wq_head 是已初始化的借用队列；wq_entry 是调用者拥有的可写输入输出项，均不可为空。
 * 出参/返回：无直接返回值；设置独占标志并把 entry 挂到队尾，不改变对象 ownership。
 * 注意事项：entry 不得已在任何链表中；返回后调用者须在释放 entry 前完成唤醒摘链或 remove。
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
/*
 * 业务背景：poll 等延迟敏感消费者可排到普通等待者之前接受回调，同时保留广播语义。
 * 入参：wq_head 是已初始化的借用队列；wq_entry 是调用者拥有的可写输入输出项，均不可为空。
 * 出参/返回：无直接返回值；设置优先级标志并挂链，entry 的存储和最终释放仍由调用者负责。
 * 注意事项：优先级不等于调度优先级或独占权；重复入队会破坏链表，临界区内不可睡眠。
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
/*
 * 业务背景：需要让一个高优先级消费者优先且独占领取事件时，必须先原子确认队首没有同类占用者。
 * 入参：wq_head 是已初始化的借用队列；wq_entry 是调用者拥有、尚未挂链的可写输入输出项，均不可空。
 * 出参/返回：成功返回 0 并挂到队首；冲突返回 -EBUSY 且不挂链；ownership 始终留给调用者。
 * 注意事项：失败时 entry 的标志已含 PRIORITY/EXCLUSIVE，重用前调用者需理解该副作用；函数不睡眠。
 */
int add_wait_queue_priority_exclusive(struct wait_queue_head *wq_head,
				      struct wait_queue_entry *wq_entry)
{
	struct list_head *head = &wq_head->head;

	wq_entry->flags |= WQ_FLAG_EXCLUSIVE | WQ_FLAG_PRIORITY;

	guard(spinlock_irqsave)(&wq_head->lock);

	/* 队首已有优先级消费者时拒绝注册，避免两个独占者同时宣称事件所有权。 */
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
/*
 * 业务背景：显式登记的等待者在条件满足、取消或销毁前必须退队，阻止唤醒端再访问其存储。
 * 入参：wq_head 是 entry 当前所在的已初始化借用队列；wq_entry 是调用者拥有的可写挂链项，均不可空。
 * 出参/返回：无直接返回值；从队列摘除 entry，但不恢复任务状态、不释放对象也不转移 ownership。
 * 注意事项：entry 必须确实属于该队列；调用者在返回后仍负责状态恢复，并须确保没有协议外引用。
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
/*
 * 业务背景：所有等待队列唤醒封装最终都需按优先/独占规则分派事件，本函数承担锁内核心遍历。
 * 入参：wq_head 是已持锁且不可空的借用队列；mode/wake_flags 是任务状态与放置位图；
 * nr_exclusive 是待满足的独占名额，0 表示不以名额截止；key 是原样借给回调、允许为空的筛选上下文。
 * 出参/返回：初始名额为正时返回剩余名额；初始为 0 时返回成功独占回调数的负值；回调可唤醒并摘链。
 * 注意事项：调用者必须持有 wq_head->lock 且不可睡眠；回调负值终止扫描，正值才可能消费独占名额。
 */
static int __wake_up_common(struct wait_queue_head *wq_head, unsigned int mode,
			int nr_exclusive, int wake_flags, void *key)
{
	wait_queue_entry_t *curr, *next;

	lockdep_assert_held(&wq_head->lock);

	/* 空表的哨兵会被转换成伪条目，因此解引用回调前必须先与表头比较。 */
	curr = list_first_entry(&wq_head->head, wait_queue_entry_t, entry);

	if (&curr->entry == &wq_head->head)
		return nr_exclusive;

	list_for_each_entry_safe_from(curr, next, &wq_head->head, entry) {
		unsigned flags = curr->flags;
		int ret;

		/* safe 遍历允许成功回调把当前栈项从队列摘除而不丢失下一项。 */
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
/*
 * 业务背景：外部唤醒者通常未持队列锁，需要一个能安全进入核心遍历并报告实际消费数的封装。
 * 入参：wq_head 是不可空的借用队列；mode 是任务状态位图；nr_exclusive 是非负独占名额；
 * wake_flags 是纯输入的调度放置提示位图；key 可空且只在调用期间借给回调，ownership 均不变。
 * 出参/返回：返回实际成功消费的独占名额数；等待项/任务可被回调修改，但参数 ownership 不变。
 * 注意事项：函数以 irqsave 自旋锁调用回调，回调不得睡眠或递归取该锁；nr_exclusive 应为非负值。
 */
static int __wake_up_common_lock(struct wait_queue_head *wq_head, unsigned int mode,
			int nr_exclusive, int wake_flags, void *key)
{
	unsigned long flags;
	int remaining;

	/* 关中断持锁，使普通、IRQ 唤醒与等待者的入队/摘链观察到同一顺序。 */
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
/*
 * 业务背景：驱动和内核子系统在发布条件后通过该通用入口唤醒普通广播者及限定数量的独占者。
 * 入参：wq_head 是不可空的借用队列；mode 是可唤醒任务状态位图；nr_exclusive 是非负独占名额；
 * key 是允许为空、原样借给回调的事件筛选值，四者都是纯输入且 ownership 不变。
 * 出参/返回：返回实际唤醒的独占等待者数量；普通项也可能被唤醒，但不计入返回值。
 * 注意事项：内部持 irq-safe 自旋锁且回调不能睡眠；调用者须先发布条件，成功唤醒路径提供完整屏障。
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
/*
 * 业务背景：对缓存局部性敏感的事件希望目标留在唤醒者 CPU，本封装把该放置提示送入通用唤醒路径。
 * 入参：wq_head 是不可空借用队列；mode 是任务状态位图；key 是允许为空、仅借给回调的筛选值。
 * 出参/返回：无直接返回值和输出参数；最多消费一个独占名额，普通等待者仍可能一并被唤醒。
 * 注意事项：WF_CURRENT_CPU 只是调度提示而非 CPU 归属保证；内部持 irq-safe 锁，调用路径不可睡眠。
 */
void __wake_up_on_current_cpu(struct wait_queue_head *wq_head, unsigned int mode, void *key)
{
	__wake_up_common_lock(wq_head, mode, 1, WF_CURRENT_CPU, key);
}

/*
 * Same as __wake_up but called with the spinlock in wait_queue_head_t held.
 */
/* 中文释义：与 __wake_up 相同，但调用者已经持有等待队列自旋锁，本函数不会重复加锁。 */
/*
 * 业务背景：已在等待队列临界区内发布条件的代码需要避免重复加锁，直接复用锁内遍历。
 * 入参：wq_head 是不可空且其 lock 已由调用者持有的借用队列；mode 是状态位图；nr 是非负独占名额。
 * 出参/返回：无直接返回值和输出参数；可能唤醒普通项并消费至多 nr 个独占项，ownership 不变。
 * 注意事项：本函数不验证锁所有权、不解锁且不可睡眠；未持锁调用会造成链表竞态。
 */
void __wake_up_locked(struct wait_queue_head *wq_head, unsigned int mode, int nr)
{
	__wake_up_common(wq_head, mode, nr, 0, NULL);
}
EXPORT_SYMBOL_GPL(__wake_up_locked);

/* 调用者持锁时按 key 唤醒至多一个独占等待者；NULL 与具体 key 的匹配由回调解释。 */
/*
 * 业务背景：锁内条件发布者需要按事件 key 精确唤醒一个独占消费者，而不重新进入锁封装。
 * 入参：wq_head 是不可空且已持 lock 的借用队列；mode 是状态位图；key 可空并仅借给回调。
 * 出参/返回：无直接返回值和输出参数；最多消费一个独占项，也可能唤醒匹配的普通项。
 * 注意事项：调用者负责锁与中断状态，回调不可睡眠；key 的类型、匹配和有效期由具体回调协议定义。
 */
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
/*
 * 业务背景：即将睡眠或让出 CPU 的生产者可提示调度器复用当前 CPU，减少唤醒目标迁移和抢占开销。
 * 入参：wq_head 是允许为空的借用队列；mode 是任务状态位图；key 可空且仅在回调期间借用。
 * 出参/返回：无直接返回值和输出参数；非空时最多消费一个独占项，空队列指针时无副作用。
 * 注意事项：WF_SYNC 只表达调度提示，不保证目标 CPU；非空路径持 irq-safe 锁且回调不可睡眠。
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
/*
 * 业务背景：已经处于队列临界区且即将让出 CPU 的生产者，需要锁内版本传递同步唤醒提示。
 * 入参：wq_head 是不可空且已持 lock 的借用队列；mode 是状态位图；key 可空并仅借给回调。
 * 出参/返回：无直接返回值和输出参数；最多消费一个独占项，参数与等待项 ownership 均不转移。
 * 注意事项：本函数不判空、不加锁也不解锁；调用者锁契约错误会破坏队列，WF_SYNC 仍只是提示。
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
/*
 * 业务背景：不需要事件筛选的同步生产者使用该便捷层，统一进入带 WF_SYNC 的通用实现。
 * 入参：wq_head 是允许为空的借用队列；mode 是纯输入的任务状态位图，二者 ownership 不变。
 * 出参/返回：无直接返回值和输出参数；非空时最多消费一个独占项，普通项也可能被唤醒。
 * 注意事项：调用者应确实将很快调度出去才使用同步提示；函数不保证目标留在当前 CPU。
 */
void __wake_up_sync(struct wait_queue_head *wq_head, unsigned int mode)
{
	__wake_up_sync_key(wq_head, mode, NULL);
}
EXPORT_SYMBOL_GPL(__wake_up_sync);	/* For internal use only */

/*
 * 向 poll/epoll 观察者广播 POLLFREE，声明等待队列所属对象即将消失。回调必须同步
 * 摘空队列；残留项意味着观察者可能继续引用即将释放的 wq_head，故用 WARN 报错。
 */
/*
 * 业务背景：动态等待队列销毁前必须通知 poll/epoll 撤销观察关系，防止对象释放后的悬空引用。
 * 入参：wq_head 是即将失效、不可为空的借用队列，调用者仍拥有其存储且必须保持到本函数返回。
 * 出参/返回：无直接返回值和输出参数；广播 HUP|POLLFREE，观察者回调应摘空队列，ownership 不转移。
 * 注意事项：返回后的非空队列触发一次 WARN；调用者还必须用 synchronize_rcu/call_rcu 等延迟释放队列头。
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
/*
 * 业务背景：手写等待循环在检查条件和 schedule 之间需要原子建立“已入队且可被唤醒”的状态。
 * 入参：wq_head 是不可空借用队列；wq_entry 是当前任务拥有的可写输入输出项；state 是合法任务状态位图。
 * 出参/返回：无直接返回值；必要时挂链 entry，并把 current 发布为 state，entry ownership 不变。
 * 注意事项：调用者必须循环重查条件并最终 finish_wait；set_current_state 的屏障次序不可前移到入队之前。
 */
void
prepare_to_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state)
{
	unsigned long flags;

	wq_entry->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq_head->lock, flags);
	/* 已挂链的等待项只更新任务状态，重复准备不会制造第二个链表节点。 */
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
/*
 * 业务背景：独占等待循环既要避免惊群，也可能依据“我是首个等待者”决定是否启动生产动作。
 * 入参：wq_head 是不可空借用队列；wq_entry 是当前任务拥有的可写输入输出项；state 是任务状态位图。
 * 出参/返回：首次挂链时若入队前队列为空返回 true，否则返回 false；同时发布 current 状态，无 ownership 转移。
 * 注意事项：返回值只是锁内瞬时快照，不授予持续队首权；调用者仍须重查条件并以 finish_wait 收尾。
 */
bool
prepare_to_wait_exclusive(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state)
{
	unsigned long flags;
	bool was_empty = false;

	wq_entry->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock_irqsave(&wq_head->lock, flags);
	/* was_empty 与队尾插入在同一临界区完成，返回的是无竞态的入队前快照。 */
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
/*
 * 业务背景：wait_event 类循环需要把栈上描述符绑定到 current，并安装成功唤醒后自动摘链的回调。
 * 入参：wq_entry 是调用者拥有且不可空的可写输出对象；flags 是纯输入的 WQ_FLAG_* 策略位图。
 * 出参/返回：无直接返回值；写入 flags/current/回调并初始化为空链，entry ownership 仍归调用者。
 * 注意事项：只能在 entry 未挂链且无人并发访问时初始化；返回后 current 与 entry 的生命周期必须匹配。
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
/*
 * 业务背景：可中断 wait_event 循环需在条件事件与信号竞争时既不漏唤醒，也不占住独占名额。
 * 入参：wq_head 是不可空借用队列；wq_entry 是 current 拥有的可写输入输出项；state 是任务状态位图。
 * 出参/返回：无待处理信号返回 0 并保证已挂链/发布状态；有信号返回 -ERESTARTSYS 并摘链，无 ownership 转移。
 * 注意事项：调用者必须在解释错误码前重查条件并最终 finish_wait；同一队列锁为信号与唤醒建立顺序。
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
		/* 独占者排队尾按名额消费，普通等待者排在独占区之前参与广播唤醒。 */
		if (list_empty(&wq_entry->entry)) {
			if (wq_entry->flags & WQ_FLAG_EXCLUSIVE)
				__add_wait_queue_entry_tail(wq_head, wq_entry);
			else
				__add_wait_queue(wq_head, wq_entry);
		}
		/* 状态发布仍在队列锁内，解锁后唤醒者不会错过已经入队的任务。 */
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
/*
 * 业务背景：锁内条件循环需要一个“登记独占等待者—可中断睡眠—重新持锁”的完整迭代原语。
 * 入参：wq 是不可空且调用者已持 lock 的借用队列；wait 是调用者拥有的可写输入输出等待项。
 * 出参/返回：正常醒来返回 0；信号待处理返回 -ERESTARTSYS；返回时仍持锁，wait 可能保持挂链。
 * 注意事项：入口中断保持开启且函数会睡眠；调用者须处理返回码、重查条件并负责最终摘链。
 */
int do_wait_intr(wait_queue_head_t *wq, wait_queue_entry_t *wait)
{
	if (likely(list_empty(&wait->entry)))
		__add_wait_queue_entry_tail(wq, wait);

	/* 信号检查发生在发布 TASK_INTERRUPTIBLE 之后，待处理信号不会让任务睡死。 */
	set_current_state(TASK_INTERRUPTIBLE);
	if (signal_pending(current))
		return -ERESTARTSYS;

	/* schedule 期间必须释放队列锁，让唤醒端能够摘链并改变任务状态。 */
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
/*
 * 业务背景：关中断持队列锁的条件循环也要安全睡眠，本变体在调度窗口临时恢复中断处理。
 * 入参：wq 是不可空、已持 lock 且本地中断关闭的借用队列；wait 是调用者拥有的可写输入输出项。
 * 出参/返回：正常醒来返回 0；信号待处理返回 -ERESTARTSYS；两类返回都保持入口锁/中断契约。
 * 注意事项：函数可睡眠，严禁在其他不可睡眠上下文调用；调用者仍需重查条件并最终摘链。
 */
int do_wait_intr_irq(wait_queue_head_t *wq, wait_queue_entry_t *wait)
{
	if (likely(list_empty(&wait->entry)))
		__add_wait_queue_entry_tail(wq, wait);

	/* 与非 irq 版本相同，先发布可中断状态，再决定信号失败或真正调度。 */
	set_current_state(TASK_INTERRUPTIBLE);
	if (signal_pending(current))
		return -ERESTARTSYS;

	/* 睡眠期间同时解锁并开中断，返回后恢复“锁定且关中断”的入口契约。 */
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
/*
 * 业务背景：任意等待循环离开前都要恢复运行态并撤销登记，才能安全复用或销毁栈上等待项。
 * 入参：wq_head 是 entry 可能所在的不可空借用队列；wq_entry 是调用者拥有的可写输入输出项。
 * 出参/返回：无直接返回值；current 变为 TASK_RUNNING，entry 最终为空链且 ownership 不变。
 * 注意事项：其他所有链表访问者必须遵守同一队列锁；返回前不得释放 entry，函数本身不睡眠。
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
	/* 快速检查只决定是否需要加锁；实际摘链始终由 wq_head->lock 串行化。 */
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
/*
 * 业务背景：栈上等待项成功唤醒后应立即脱离队列，减少调用者清理与唤醒遍历之间的生命周期竞态。
 * 入参：wq_entry 是不可空、仍由调用者拥有的可写等待项；mode/sync 是纯输入唤醒位；
 * key 是允许为空且仅借给默认回调的筛选上下文，所有参数 ownership 均不转移。
 * 出参/返回：原样返回 default_wake_function 的正成功值或 0 失败值；仅正值时自动摘链。
 * 注意事项：由持队列锁的唤醒遍历调用且不可睡眠；返回 0 时 entry 必须保留以等待后续事件。
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
/*
 * 业务背景：使用 WQ_FLAG_WOKEN 的循环需协调任务状态、事件标志和条件发布，避免唤醒落在睡眠缝隙。
 * 入参：wq_entry 是 current 拥有且不可空的可写输入输出项；mode 是任务状态位图；timeout 是非负剩余
 * jiffies，MAX_SCHEDULE_TIMEOUT 表示不设有限截止时间，三个参数均不转移 ownership。
 * 出参/返回：返回 schedule_timeout 更新后的剩余 jiffies；清除 WOKEN 并恢复 TASK_RUNNING，无 ownership 转移。
 * 注意事项：调用者先登记 entry、返回后重查条件；A/B/C 屏障配对不可删除，函数可能调度睡眠。
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
/*
 * 业务背景：wait_woken 的事件生产端必须同时记录持久 WOKEN 标志并尝试改变任务运行状态。
 * 入参：wq_entry 是不可空、由等待者拥有的可写输入输出项；mode/sync 是纯输入唤醒位；
 * key 是允许为空、仅借给默认回调的筛选上下文，参数 ownership 均不转移。
 * 出参/返回：返回 default_wake_function 的正成功值或 0 未唤醒值；无论结果都设置 WOKEN。
 * 注意事项：由队列锁内回调路径调用且不可睡眠；C 屏障必须与等待端 B 屏障配对以免漏事件。
 */
int woken_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key)
{
	/* Pairs with the smp_store_mb() in wait_woken(). */
	smp_mb(); /* C */
	wq_entry->flags |= WQ_FLAG_WOKEN;

	return default_wake_function(wq_entry, mode, sync, key);
}
EXPORT_SYMBOL(woken_wake_function);
