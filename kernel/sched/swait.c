// SPDX-License-Identifier: GPL-2.0
/*
 * <linux/swait.h> (simple wait queues ) implementation:
 */
/*
 * 本文件实现实时约束下的 simple wait queue。它只保存 task 指针和 FIFO 链表，
 * 所有 waiter 都按 exclusive 语义一次唤醒一个；通过 raw spinlock 和广播时每次
 * 唤醒后短暂放锁，把 IRQ-off/锁持有时间限制在可预测范围。与普通 waitqueue
 * 相比，它主动放弃混合等待状态、非 exclusive waiter 和自定义 wake callback。
 * completion.c 直接使用锁内 helper，把 done 令牌和队列变化组成同一事务。
 */
#include "sched.h"

/*
 * __init_swait_queue_head() - 初始化动态 simple-wait 队列头及 lockdep 身份。
 *
 * @q 是不可空输入/输出借用对象，尚未对并发使用者发布；@name 是 lockdep 使用的
 * 只读借用字符串；@key 是静态 lock-class key 的借用指针。函数不睡眠、无返回、
 * 不取得 ownership；初始化 raw lock、设置锁类/名称并建立空 task_list。调用者
 * 随后才可发布 @q，重复初始化一个仍有 waiter 的队列会破坏栈节点生命周期。
 */
void __init_swait_queue_head(struct swait_queue_head *q, const char *name,
			     struct lock_class_key *key)
{
	raw_spin_lock_init(&q->lock);
	lockdep_set_class_and_name(&q->lock, key, name);
	INIT_LIST_HEAD(&q->task_list);
}
EXPORT_SYMBOL(__init_swait_queue_head);

/*
 * The thing about the wake_up_state() return value; I think we can ignore it.
 *
 * If for some reason it would return 0, that means the previously waiting
 * task is already running, so it will observe condition true (or has already).
 */
/*
 * swake_up_locked() - 在已持 q->lock 时摘下并唤醒 FIFO 首个 waiter。
 *
 * 原英文说明：这里可忽略 wake_up_state()/try_to_wake_up() 的返回值；若返回 0，
 * 该 waiter 已经运行，因而会观察到条件为真或此前已观察到，不需要重复补救。
 * @q 是不可空输入/输出借用队列，入口必须持 raw lock；@wake_flags 是传给调度器
 * 的放置提示。函数不睡眠、无返回；空队列无副作用，非空时局部 @curr 借用首个
 * 栈节点，尝试 TASK_NORMAL 唤醒并无条件摘链。摘链把该节点重新交还 waiter，
 * 不取得 task 引用；condition 的发布/观察仍由调用者的锁或屏障协议保证。
 */
void swake_up_locked(struct swait_queue_head *q, int wake_flags)
{
	/* curr 只在 q->lock 临界区借用，节点所有权始终属于等待任务的栈帧。 */
	struct swait_queue *curr;

	if (list_empty(&q->task_list))
		return;

	/* FIFO 队首是等待最久者；一次只摘一个，保持有界 one-wakeup 行为。 */
	curr = list_first_entry(&q->task_list, typeof(*curr), task_list);
	try_to_wake_up(curr->task, TASK_NORMAL, wake_flags);
	list_del_init(&curr->task_list);
}
EXPORT_SYMBOL(swake_up_locked);

/*
 * Wake up all waiters. This is an interface which is solely exposed for
 * completions and not for general usage.
 *
 * It is intentionally different from swake_up_all() to allow usage from
 * hard interrupt context and interrupt disabled regions.
 */
/*
 * swake_up_all_locked() - 在调用者持锁/关中断条件下唤醒并摘除全部 waiter。
 *
 * 原英文规定它只为 completion 暴露，不是通用 API；与 swake_up_all() 不同，它
 * 始终保持 q->lock，由调用者控制 IRQ 状态，因此可在 hardirq 或 IRQ-disabled
 * 区域使用。@q 为不可空输入/输出借用，入口已持 q->lock，函数不睡眠、无返回；
 * 循环复用单唤醒 helper，退出保证队列为空。代价是 waiter 多时锁持有时间随 N
 * 增长，所以一般 swait 广播应使用会逐次放锁的 swake_up_all()。
 */
void swake_up_all_locked(struct swait_queue_head *q)
{
	while (!list_empty(&q->task_list))
		swake_up_locked(q, 0);
}

/*
 * swake_up_one() - 自行加锁后唤醒一个 simple-wait waiter。
 *
 * @q 是不可空输入/输出借用队列，调用者无需持锁；函数用 irqsave raw lock，
 * 可从进程、软中断或硬中断上下文调用且不睡眠。返回无直接值；空队列不变，
 * 非空时摘除并尝试唤醒 FIFO 首项。局部 @flags 只保存本 CPU 原 IRQ 状态。
 */
void swake_up_one(struct swait_queue_head *q)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&q->lock, flags);
	swake_up_locked(q, 0);
	raw_spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(swake_up_one);

/*
 * Does not allow usage from IRQ disabled, since we must be able to
 * release IRQs to guarantee bounded hold time.
 */
/*
 * swake_up_all() - 以逐 waiter 放锁方式广播唤醒 simple-wait 队列。
 *
 * 原英文要求入口 IRQ 开启，因为函数必须在相邻唤醒之间解锁并重新开 IRQ，才能
 * 保证单次锁/IRQ-off 时间有界并让更高优先级任务运行。@q 是不可空输入/输出
 * 借用对象；函数不调度睡眠、无返回。先在锁内把共享 task_list 整体转移到局部
 * @tmp，之后新 waiter 可进入原队列且不会混入本批；@curr 逐个借用临时链节点，
 * wake_up_state(TASK_NORMAL) 后摘除。最后一项无需额外放锁/重锁循环。
 */
void swake_up_all(struct swait_queue_head *q)
{
	struct swait_queue *curr;
	LIST_HEAD(tmp);

	raw_spin_lock_irq(&q->lock);
	list_splice_init(&q->task_list, &tmp);
	while (!list_empty(&tmp)) {
		curr = list_first_entry(&tmp, typeof(*curr), task_list);

		wake_up_state(curr->task, TASK_NORMAL);
		list_del_init(&curr->task_list);

		if (list_empty(&tmp))
			break;

		raw_spin_unlock_irq(&q->lock);
		raw_spin_lock_irq(&q->lock);
	}
	raw_spin_unlock_irq(&q->lock);
}
EXPORT_SYMBOL(swake_up_all);

/*
 * __prepare_to_swait() - 在已持 q->lock 时把 current 的栈节点加入 FIFO 尾部。
 *
 * @q 是输入/输出借用队列且入口必须持锁；@wait 是调用者栈上输入/输出节点，
 * task_list 已初始化并在整个等待期存活。函数不睡眠、无返回；更新 wait->task 为
 * current，仅当节点尚未入链时追加，因而等待循环可重复 prepare 而不重复插入。
 * 本 helper 不设置 task state，调用者必须在同一锁协议下紧接着完成状态发布。
 */
void __prepare_to_swait(struct swait_queue_head *q, struct swait_queue *wait)
{
	wait->task = current;
	if (list_empty(&wait->task_list))
		list_add_tail(&wait->task_list, &q->task_list);
}

/*
 * prepare_to_swait_exclusive() - 原子地排队 current 并发布指定睡眠状态。
 *
 * @q 为不可空输入/输出借用队列；@wait 为调用者持有的栈节点；@state 是调度器
 * 认可的 TASK_* 状态。入口无需持锁，函数不睡眠、无返回；irqsave raw lock 把
 * “节点可见 → current state 可唤醒”与 waker 串行化，关闭丢失唤醒窗口。返回后
 * 调用者通常复查 condition 再 schedule()，最终必须 finish_swait() 摘链。
 */
void prepare_to_swait_exclusive(struct swait_queue_head *q, struct swait_queue *wait, int state)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&q->lock, flags);
	__prepare_to_swait(q, wait);
	set_current_state(state);
	raw_spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL(prepare_to_swait_exclusive);

/*
 * prepare_to_swait_event() - 为宏等待循环排队，或在信号已到时撤销可见节点。
 *
 * @q/@wait 是不可空输入/输出借用队列和栈节点；@state 决定信号是否可中断。
 * 函数不睡眠，自行 irqsave 加锁。返回 0 表示已排队并设置 @state，调用者可复查
 * condition/调度；返回 -ERESTARTSYS 表示相应 signal pending，节点已摘除且不会
 * 被后续 swake_up_one() 看到，并且本轮不会再设置新的睡眠状态。ownership 不变；
 * 正常条件路径由 finish_swait() 收尾，信号错误路径因已脱链而可直接返回。
 */
long prepare_to_swait_event(struct swait_queue_head *q, struct swait_queue *wait, int state)
{
	unsigned long flags;
	long ret = 0;

	raw_spin_lock_irqsave(&q->lock, flags);
	if (signal_pending_state(state, current)) {
		/*
		 * See prepare_to_wait_event(). TL;DR, subsequent swake_up_one()
		 * must not see us.
		 */
		/*
		 * 原英文指向普通 waitqueue 的同类协议：信号胜出后必须在锁内先摘链，
		 * 否则 waker 可能同时领取一个已经决定返回用户态的栈节点。
		 */
		list_del_init(&wait->task_list);
		ret = -ERESTARTSYS;
	} else {
		__prepare_to_swait(q, wait);
		set_current_state(state);
	}
	raw_spin_unlock_irqrestore(&q->lock, flags);

	return ret;
}
EXPORT_SYMBOL(prepare_to_swait_event);

/*
 * __finish_swait() - 在已持 q->lock 时恢复 current 并摘除剩余等待节点。
 *
 * @q 是入口已锁的借用队列；@wait 是当前栈节点。函数不睡眠、无返回；先把
 * current 状态恢复为 TASK_RUNNING，再在节点仍链接时摘除。若 waker 已摘链，
 * list_empty() 使本路径无重复删除。调用者保留节点 ownership，返回后可销毁栈帧。
 */
void __finish_swait(struct swait_queue_head *q, struct swait_queue *wait)
{
	__set_current_state(TASK_RUNNING);
	if (!list_empty(&wait->task_list))
		list_del_init(&wait->task_list);
}

/*
 * finish_swait() - 无需预持锁地结束一次 simple-wait 等待。
 *
 * @q 为不可空借用队列，@wait 为调用者栈节点。函数不睡眠、无返回；先恢复
 * TASK_RUNNING，再用 list_empty_careful() 快速判断 waker 是否已摘链，必要时
 * irqsave 加锁并删除。锁保证不会与 swake_up*() 并发操作同一节点；返回后队列
 * 不再引用 @wait，调用者才能离开作用域。
 */
void finish_swait(struct swait_queue_head *q, struct swait_queue *wait)
{
	unsigned long flags;

	__set_current_state(TASK_RUNNING);

	if (!list_empty_careful(&wait->task_list)) {
		raw_spin_lock_irqsave(&q->lock, flags);
		list_del_init(&wait->task_list);
		raw_spin_unlock_irqrestore(&q->lock, flags);
	}
}
EXPORT_SYMBOL(finish_swait);
