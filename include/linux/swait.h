/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SWAIT_H
#define _LINUX_SWAIT_H

#include <linux/list.h>
#include <linux/stddef.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <asm/current.h>

/*
 * 简单等待队列（swait）接口总览：
 *
 * swait 是为可预测的实时唤醒路径裁剪出来的等待队列。队头只保存一把
 * raw_spinlock 和一条任务链表；等待项只保存任务指针与链表节点，不支持普通
 * wait_queue_entry 的自定义回调、非独占唤醒及同队列混用多种睡眠状态。
 *
 * 典型生命周期为：在调用方栈上创建 swait_queue，prepare 函数把节点挂入队头
 * 并设置当前任务状态，schedule 让出 CPU；唤醒方从队头取等待者并将其唤醒；
 * 最后 finish 函数恢复 TASK_RUNNING 并保证节点已脱链。节点和条件数据均由调用方
 * 拥有，队头只负责串行化链表关系，不替调用方管理条件对象的生命周期。
 */

/*
 * Simple waitqueues are semantically very different to regular wait queues
 * (wait.h). The most important difference is that the simple waitqueue allows
 * for deterministic behaviour -- IOW it has strictly bounded IRQ and lock hold
 * times.
 *
 * Mainly, this is accomplished by two things. Firstly not allowing swake_up_all
 * from IRQ disabled, and dropping the lock upon every wakeup, giving a higher
 * priority task a chance to run.
 *
 * Secondly, we had to drop a fair number of features of the other waitqueue
 * code; notably:
 *
 *  - mixing INTERRUPTIBLE and UNINTERRUPTIBLE sleeps on the same waitqueue;
 *    all wakeups are TASK_NORMAL in order to avoid O(n) lookups for the right
 *    sleeper state.
 *
 *  - the !exclusive mode; because that leads to O(n) wakeups, everything is
 *    exclusive. As such swake_up_one will only ever awake _one_ waiter.
 *
 *  - custom wake callback functions; because you cannot give any guarantees
 *    about random code. This also allows swait to be used in RT, such that
 *    raw spinlock can be used for the swait queue head.
 *
 * As a side effect of these; the data structures are slimmer albeit more ad-hoc.
 * For all the above, note that simple wait queues should _only_ be used under
 * very specific realtime constraints -- it is best to stick with the regular
 * wait queues in most cases.
 *
 * 简单等待队列与普通等待队列（wait.h）的语义差异很大。最关键的区别是：
 * swait 以严格限制中断关闭时间和锁持有时间换取确定性。为此，普通的
 * swake_up_all() 不允许在 IRQ 已关闭的上下文调用，并且每唤醒一个任务就释放
 * 一次锁，使更高优先级任务有机会立即运行。
 *
 * 这种确定性来自主动舍弃普通等待队列的若干能力：同一队列不能混合
 * TASK_INTERRUPTIBLE 与 TASK_UNINTERRUPTIBLE 等状态，唤醒统一按 TASK_NORMAL；
 * 所有等待者均视为独占等待者，swake_up_one() 至多唤醒一个；也不允许安装任意
 * 唤醒回调。最后一项使唤醒路径没有未知代码，并允许 RT 场景使用 raw_spinlock。
 * 数据结构因此更小，但用途也更专门；没有明确的实时确定性需求时应优先使用
 * 普通等待队列。
 */

/* 等待项直接保存 task_struct 指针；此处只需前置声明，不取得任务对象所有权。 */
struct task_struct;

/*
 * swait_queue_head - 简单等待队列的共享队头
 * @lock: 保护 @task_list 的 raw 自旋锁；使 PREEMPT_RT 下也保持原始自旋语义
 * @task_list: 按入队顺序链接等待项；空表表示当前没有已登记等待者
 *
 * 队头通常随被等待对象长期存在，必须在任何等待者登记前完成初始化，并在所有
 * 等待者退出后才可销毁。持锁只保护队列结构，不自动保护调用方的等待条件。
 */
struct swait_queue_head {
	raw_spinlock_t		lock;
	struct list_head	task_list;
};

/*
 * swait_queue - 一次等待操作的私有登记项
 * @task: 被唤醒的任务，通常是创建该等待项时的 current
 * @task_list: 挂入 swait_queue_head::task_list 的节点
 *
 * 该对象通常位于等待任务的栈上，因此只有等待任务拥有其存储期。返回调用方前
 * 必须经 finish_swait() 脱链，唤醒方不得保存此指针或延长它的生命周期。
 */
struct swait_queue {
	struct task_struct	*task;
	struct list_head	task_list;
};

/* 静态构造单个等待项：绑定 current，并把尚未入队的链表节点初始化为空环。 */
#define __SWAITQUEUE_INITIALIZER(name) {				\
	.task		= current,					\
	.task_list	= LIST_HEAD_INIT((name).task_list),		\
}

/* 在当前作用域声明并初始化一个由当前任务拥有的等待项。 */
#define DECLARE_SWAITQUEUE(name)					\
	struct swait_queue name = __SWAITQUEUE_INITIALIZER(name)

/* 静态构造队头：初始化 raw 锁及空等待者链表。 */
#define __SWAIT_QUEUE_HEAD_INITIALIZER(name) {				\
	.lock		= __RAW_SPIN_LOCK_UNLOCKED(name.lock),		\
	.task_list	= LIST_HEAD_INIT((name).task_list),		\
}

/* 声明可静态初始化的队头；适合与其宿主对象具有相同的静态/长期生命周期。 */
#define DECLARE_SWAIT_QUEUE_HEAD(name)					\
	struct swait_queue_head name = __SWAIT_QUEUE_HEAD_INITIALIZER(name)

/*
 * 运行时初始化队头，并把 @name/@key 交给 lockdep 建立锁类；调用时尚不能有等待者。
 */
extern void __init_swait_queue_head(struct swait_queue_head *q, const char *name,
				    struct lock_class_key *key);

/* 每个宏展开点拥有独立的静态 lock_class_key，便于 lockdep 区分不同初始化地点。 */
#define init_swait_queue_head(q)				\
	do {							\
		static struct lock_class_key __key;		\
		__init_swait_queue_head((q), #q, &__key);	\
	} while (0)

#ifdef CONFIG_LOCKDEP
/* 栈上队头在 lockdep 配置下也走运行时初始化，以保留准确的锁依赖身份。 */
# define __SWAIT_QUEUE_HEAD_INIT_ONSTACK(name)			\
	({ init_swait_queue_head(&name); name; })
# define DECLARE_SWAIT_QUEUE_HEAD_ONSTACK(name)			\
	struct swait_queue_head name = __SWAIT_QUEUE_HEAD_INIT_ONSTACK(name)
#else
/* 无 lockdep 时无需登记动态锁类，复用普通静态初始化即可。 */
# define DECLARE_SWAIT_QUEUE_HEAD_ONSTACK(name)			\
	DECLARE_SWAIT_QUEUE_HEAD(name)
#endif

/**
 * swait_active -- locklessly test for waiters on the queue
 * @wq: the waitqueue to test for waiters
 *
 * returns true if the wait list is not empty
 *
 * NOTE: this function is lockless and requires care, incorrect usage _will_
 * lead to sporadic and non-obvious failure.
 *
 * NOTE2: this function has the same above implications as regular waitqueues.
 *
 * Use either while holding swait_queue_head::lock or when used for wakeups
 * with an extra smp_mb() like:
 *
 *      CPU0 - waker                    CPU1 - waiter
 *
 *                                      for (;;) {
 *      @cond = true;                     prepare_to_swait_exclusive(&wq_head, &wait, state);
 *      smp_mb();                         // smp_mb() from set_current_state()
 *      if (swait_active(wq_head))        if (@cond)
 *        wake_up(wq_head);                      break;
 *                                        schedule();
 *                                      }
 *                                      finish_swait(&wq_head, &wait);
 *
 * Because without the explicit smp_mb() it's possible for the
 * swait_active() load to get hoisted over the @cond store such that we'll
 * observe an empty wait list while the waiter might not observe @cond.
 * This, in turn, can trigger missing wakeups.
 *
 * Also note that this 'optimization' trades a spin_lock() for an smp_mb(),
 * which (when the lock is uncontended) are of roughly equal cost.
 *
 * 无锁判断队列中是否存在等待者。返回非零只表示读取瞬间链表非空，并不固定任何
 * 等待项的生命周期；除非持有 wq->lock，否则结果随时可能失效。
 *
 * 在“先发布条件、再按需唤醒”的优化中，唤醒方必须在条件写入与本次链表读取之间
 * 执行 smp_mb()，与等待方 set_current_state() 内的屏障配对。否则 CPU0 可能先观察
 * 到空队列，而 CPU1 又尚未观察到条件成立，最终双方都不再推进，形成丢失唤醒。
 * 该优化只是用一个全屏障替换无竞争自旋锁，成本通常相近，使用前应证明它确有价值。
 */
static inline int swait_active(struct swait_queue_head *wq)
{
	return !list_empty(&wq->task_list);
}

/**
 * swq_has_sleeper - check if there are any waiting processes
 * @wq: the waitqueue to test for waiters
 *
 * Returns true if @wq has waiting processes
 *
 * Please refer to the comment for swait_active.
 *
 * 判断是否已有睡眠者，并在读取链表前执行全内存屏障。该屏障用于与等待方入队及
 * 设置任务状态一侧配对，确保条件数据和 task_list 的可见顺序一致；它仍不是锁，
 * 因而不能让返回结果在函数退出后持续有效。其余约束与 swait_active() 相同。
 */
static inline bool swq_has_sleeper(struct swait_queue_head *wq)
{
	/*
	 * We need to be sure we are in sync with the list_add()
	 * modifications to the wait queue (task_list).
	 *
	 * This memory barrier should be paired with one on the
	 * waiting side.
	 *
	 * 必须确保本 CPU 的观察顺序与等待方对 task_list 执行 list_add() 的修改同步。
	 * 这里的内存屏障应与等待方的屏障配对，而不是把链表本身变成无锁可修改的数据结构。
	 */
	smp_mb();
	return swait_active(wq);
}

/* 唤醒 API：one 至多唤醒队首一个任务；all 分批唤醒全部；locked 要求调用方已持锁。 */
extern void swake_up_one(struct swait_queue_head *q);
extern void swake_up_all(struct swait_queue_head *q);
extern void swake_up_locked(struct swait_queue_head *q, int wake_flags);

/* prepare 将当前任务登记并设置 @state；event 还会处理可中断等待中的待决信号。 */
extern void prepare_to_swait_exclusive(struct swait_queue_head *q, struct swait_queue *wait, int state);
extern long prepare_to_swait_event(struct swait_queue_head *q, struct swait_queue *wait, int state);

/* finish 负责脱链和恢复运行态；双下划线版本供已满足其锁/状态前提的内部路径使用。 */
extern void __finish_swait(struct swait_queue_head *q, struct swait_queue *wait);
extern void finish_swait(struct swait_queue_head *q, struct swait_queue *wait);

/* as per ___wait_event() but for swait, therefore "exclusive == 1" */
/*
 * 与 ___wait_event() 的控制骨架相同，但用于 swait，所以等待项始终是独占的，
 * 等价于普通等待队列中的 exclusive == 1。
 *
 * 宏先在调用者栈上建立 __wait，然后循环执行“登记并设置状态 -> 检查条件 ->
 * 检查信号 -> 执行调度命令”。条件必须在登记之后再次检查，这是避免条件在检查与
 * 入队之间成立而丢失唤醒的关键。正常条件成立时由 finish_swait() 脱链并恢复运行态；
 * prepare_to_swait_event() 若因信号返回错误，已经在持锁区把节点移除且不会设置新的
 * 睡眠状态，因而可直接跳到 __out。@cmd 可以是 schedule() 或带超时的
 * schedule_timeout()。
 */
#define ___swait_event(wq, condition, state, ret, cmd)			\
({									\
	__label__ __out;						\
	struct swait_queue __wait;					\
	long __ret = ret;						\
									\
	INIT_LIST_HEAD(&__wait.task_list);				\
	for (;;) {							\
		long __int = prepare_to_swait_event(&wq, &__wait, state);\
									\
		if (condition)						\
			break;						\
									\
		if (___wait_is_interruptible(state) && __int) {		\
			__ret = __int;					\
			goto __out;					\
		}							\
									\
		cmd;							\
	}								\
	finish_swait(&wq, &__wait);					\
__out:	__ret;								\
})

/* 不可中断、无超时的内部等待：条件成立后没有有意义的返回值。 */
#define __swait_event(wq, condition)					\
	(void)___swait_event(wq, condition, TASK_UNINTERRUPTIBLE, 0,	\
			    schedule())

/*
 * 不可中断地等待 @condition。入口快速检查避免条件已成立时构造等待项和取锁；
 * 条件表达式可能被求值多次，调用方不得依赖其中的破坏性副作用。
 */
#define swait_event_exclusive(wq, condition)				\
do {									\
	if (condition)							\
		break;							\
	__swait_event(wq, condition);					\
} while (0)

/* 超时内部版本以剩余 jiffies 作为循环状态，并由 schedule_timeout() 更新。 */
#define __swait_event_timeout(wq, condition, timeout)			\
	___swait_event(wq, ___wait_cond_timeout(condition),		\
		      TASK_UNINTERRUPTIBLE, timeout,			\
		      __ret = schedule_timeout(__ret))

/*
 * 不可中断的限时等待。返回 0 表示超时且条件仍为假；条件在截止点成立时返回 1；
 * 提前成立时返回至少 1 的剩余 jiffies。timeout 与 condition 都可能被宏多次读取。
 */
#define swait_event_timeout_exclusive(wq, condition, timeout)		\
({									\
	long __ret = timeout;						\
	if (!___wait_cond_timeout(condition))				\
		__ret = __swait_event_timeout(wq, condition, timeout);	\
	__ret;								\
})

/* 可中断、无超时的内部版本：成功为 0，待决信号由 prepare 路径返回负错误码。 */
#define __swait_event_interruptible(wq, condition)			\
	___swait_event(wq, condition, TASK_INTERRUPTIBLE, 0,		\
		      schedule())

/*
 * 以 TASK_INTERRUPTIBLE 等待。条件先成立时返回 0；否则可能因信号提前返回
 * -ERESTARTSYS。调用方必须同时处理“条件满足”和“等待被信号中断”两类结果。
 */
#define swait_event_interruptible_exclusive(wq, condition)		\
({									\
	int __ret = 0;							\
	if (!(condition))						\
		__ret = __swait_event_interruptible(wq, condition);	\
	__ret;								\
})

/* 把可中断语义与剩余 jiffies 状态组合到通用等待循环中。 */
#define __swait_event_interruptible_timeout(wq, condition, timeout)	\
	___swait_event(wq, ___wait_cond_timeout(condition),		\
		      TASK_INTERRUPTIBLE, timeout,			\
		      __ret = schedule_timeout(__ret))

/*
 * 可中断的限时等待：正值表示条件提前成立及剩余时间，0 表示超时，负值表示信号中断。
 * 这三类返回值不可仅按真假判断，否则会把负错误码误当成成功。
 */
#define swait_event_interruptible_timeout_exclusive(wq, condition, timeout)\
({									\
	long __ret = timeout;						\
	if (!___wait_cond_timeout(condition))				\
		__ret = __swait_event_interruptible_timeout(wq,		\
						condition, timeout);	\
	__ret;								\
})

/* TASK_IDLE 等待不会计入系统负载；信号不会终止等待。 */
#define __swait_event_idle(wq, condition)				\
	(void)___swait_event(wq, condition, TASK_IDLE, 0, schedule())

/**
 * swait_event_idle_exclusive - wait without system load contribution
 * @wq: the waitqueue to wait on
 * @condition: a C expression for the event to wait for
 *
 * The process is put to sleep (TASK_IDLE) until the @condition evaluates to
 * true. The @condition is checked each time the waitqueue @wq is woken up.
 *
 * This function is mostly used when a kthread or workqueue waits for some
 * condition and doesn't want to contribute to system load. Signals are
 * ignored.
 *
 * 让当前任务以 TASK_IDLE 状态等待 @condition，等待期间不计入系统负载。
 * 每次队列被唤醒后都会重新检查条件；主要用于不希望影响负载统计的内核线程或
 * 工作队列。信号被忽略，因此退出责任完全由条件提供者承担。
 */
#define swait_event_idle_exclusive(wq, condition)			\
do {									\
	if (condition)							\
		break;							\
	__swait_event_idle(wq, condition);				\
} while (0)

/* TASK_IDLE 的限时内部循环，使用 schedule_timeout() 维护剩余 jiffies。 */
#define __swait_event_idle_timeout(wq, condition, timeout)		\
	___swait_event(wq, ___wait_cond_timeout(condition),		\
		       TASK_IDLE, timeout,				\
		       __ret = schedule_timeout(__ret))

/**
 * swait_event_idle_timeout_exclusive - wait up to timeout without load contribution
 * @wq: the waitqueue to wait on
 * @condition: a C expression for the event to wait for
 * @timeout: timeout at which we'll give up in jiffies
 *
 * The process is put to sleep (TASK_IDLE) until the @condition evaluates to
 * true. The @condition is checked each time the waitqueue @wq is woken up.
 *
 * This function is mostly used when a kthread or workqueue waits for some
 * condition and doesn't want to contribute to system load. Signals are
 * ignored.
 *
 * Returns:
 * 0 if the @condition evaluated to %false after the @timeout elapsed,
 * 1 if the @condition evaluated to %true after the @timeout elapsed,
 * or the remaining jiffies (at least 1) if the @condition evaluated
 * to %true before the @timeout elapsed.
 *
 * 最多以 TASK_IDLE 等待 @timeout 个 jiffies，期间不贡献系统负载且忽略信号。
 * 返回 0 表示超时后条件仍为假；返回 1 表示恰在截止点观察到条件为真；提前满足时
 * 返回至少 1 的剩余 jiffies。调用方应使用返回值区分超时，并在必要时再次验证条件。
 */
#define swait_event_idle_timeout_exclusive(wq, condition, timeout)	\
({									\
	long __ret = timeout;						\
	if (!___wait_cond_timeout(condition))				\
		__ret = __swait_event_idle_timeout(wq,			\
						   condition, timeout);	\
	__ret;								\
})

/* 结束 swait 公共接口；所有实体均只在本头文件保护范围内定义一次。 */
#endif /* _LINUX_SWAIT_H */
