// SPDX-License-Identifier: GPL-2.0

/*
 * Generic wait-for-completion handler;
 *
 * It differs from semaphores in that their default case is the opposite,
 * wait_for_completion default blocks whereas semaphore default non-block. The
 * interface also makes it easy to 'complete' multiple waiting threads,
 * something which isn't entirely natural for semaphores.
 *
 * But more importantly, the primitive documents the usage. Semaphores would
 * typically be used for exclusion which gives rise to priority inversion.
 * Waiting for completion is a typically sync point, but not an exclusion point.
 */

/*
 * completion 是“一方完成工作，另一方等待结果”的一次性/计数式同步点：
 *
 *   producer 写好结果 → complete()/complete_all()
 *                               │
 *                               ▼
 *   consumer wait_for_completion*() → 看到结果并继续
 *
 * 原英文首先比较默认语义：wait_for_completion() 默认阻塞等待，而 semaphore
 * 的常见获取在无资源时可选择非阻塞；completion 还自然支持一次完成多个等待
 * 线程，这并不是信号量最自然的表达。更重要的是接口本身记录了设计意图：
 * semaphore 通常表示排他访问，可能形成优先级反转；completion 表示同步点而非
 * exclusion point，不把被唤醒者变成某个临界区的 owner。
 *
 * 与信号量的区别不只在接口：completion 不授予临界区所有权，也不要求
 * waiter 在离开临界区时归还令牌。它表达的是工作已完成这一事件，因而能让
 * lockdep、维护者和调用者辨认真正的同步意图，避免把互斥与事件通知混为一谈。
 *
 * struct completion::done 的状态协议：
 *   0        尚无可消费的完成事件；waiter 需要入队并睡眠。
 *   1..MAX-1 每次 complete() 增加一个令牌，每个成功 waiter 消耗一个。
 *   UINT_MAX complete_all() 发布的永久完成状态；waiter 不再递减。
 * wait.lock 同时保护 done 和 FIFO swait 队列，使“观察令牌、入队、睡眠”与
 * “增加令牌、选择 waiter、唤醒”成为同一原子协议，避免丢失唤醒。
 */

#include <linux/linkage.h>
#include <linux/sched/debug.h>
#include <linux/completion.h>
#include "sched.h"

/*
 * complete_with_flags() - 发布一个完成令牌并唤醒一个等待者。
 *
 * 调用位置：complete()/complete_on_current_cpu() → 本函数 →
 * swake_up_locked() → try_to_wake_up()。这是单完成事件的状态提交点。
 *
 * @x: 输入，调用者借用且已初始化的 completion；不得为 NULL。调用期间对象
 *     必须存活，但本函数不取得长期引用，也不转移 ownership。
 * @wake_flags: 输入，传给调度器的唤醒放置提示；0 使用普通策略，
 *     WF_CURRENT_CPU 倾向把被唤醒任务留在当前 CPU。它不改变完成令牌语义。
 *
 * 可从进程、软中断或硬中断等不能睡眠的上下文调用。入口无需持有 wait.lock；
 * irqsave 自旋锁同时保护 done 和等待队列。返回无直接值；有限计数增加一个，
 * UINT_MAX 保持饱和，并至多唤醒一个 FIFO waiter。返回后调用者仍拥有 @x。
 * 锁释放也让 producer 在 complete() 前的写入先于被唤醒 waiter 继续执行。
 */
static void complete_with_flags(struct completion *x, int wake_flags)
{
	/* flags 保存本 CPU 的中断状态，解锁时精确恢复，而不是无条件开中断。 */
	unsigned long flags;

	/*
	 * 阶段 1：把令牌发布和 waiter 选择放进同一临界区。若 waiter 正在
	 * “检查 done → 入队”之间，双方只能有一方先取得锁，因此事件不会落在
	 * 检查与睡眠之间而丢失。
	 */
	raw_spin_lock_irqsave(&x->wait.lock, flags);

	/* UINT_MAX 是 complete_all() 的永久完成哨兵，必须饱和而不能回绕成 0。 */
	if (x->done != UINT_MAX)
		x->done++;
	/* 队列为空时只保留令牌；非空时把最早 waiter 交给调度器唤醒。 */
	swake_up_locked(&x->wait, wake_flags);
	raw_spin_unlock_irqrestore(&x->wait.lock, flags);
}

/*
 * complete_on_current_cpu() - 发布一个完成令牌，并提示在当前 CPU 唤醒。
 *
 * 调用者借用非 NULL 的 @x；入口无需持锁且不能依赖本函数睡眠。它与
 * complete() 的计数、FIFO 和内存序语义完全相同，只把 WF_CURRENT_CPU
 * 传给唤醒路径以改善局部性。返回无直接值，ownership 不变。
 */
void complete_on_current_cpu(struct completion *x)
{
	return complete_with_flags(x, WF_CURRENT_CPU);
}

/**
 * complete: - signals a single thread waiting on this completion
 * @x:  holds the state of this particular completion
 *
 * This will wake up a single thread waiting on this completion. Threads will be
 * awakened in the same order in which they were queued.
 *
 * See also complete_all(), wait_for_completion() and related routines.
 *
 * If this function wakes up a task, it executes a full memory barrier before
 * accessing the task state.
 */
/*
 * complete() - 为一个 waiter 发布一个可消费的完成事件。
 *
 * 主要调用者是异步工作的完成端；后续由 wait_for_completion*() 消费令牌。
 * @x 是借用的、已初始化且在整个调用期间存活的同步对象，不可为 NULL。
 * 本函数可用于原子/中断上下文，不睡眠；内部自行取得 irqsave raw spinlock。
 * 返回无直接值。副作用是有限 done 加一（已为 UINT_MAX 时保持不变），并按
 * FIFO 次序至多唤醒一个 waiter；调用者仍负责 @x 的生命周期。
 *
 * 上述英文所说的 full memory barrier 位于实际任务唤醒协议中；结合
 * wait.lock，保证完成端在调用前写好的结果不会被成功返回的 waiter 漏看。
 */
void complete(struct completion *x)
{
	complete_with_flags(x, 0);
}
EXPORT_SYMBOL(complete);

/**
 * complete_all: - signals all threads waiting on this completion
 * @x:  holds the state of this particular completion
 *
 * This will wake up all threads waiting on this particular completion event.
 *
 * If this function wakes up a task, it executes a full memory barrier before
 * accessing the task state.
 *
 * Since complete_all() sets the completion of @x permanently to done
 * to allow multiple waiters to finish, a call to reinit_completion()
 * must be used on @x if @x is to be used again. The code must make
 * sure that all waiters have woken and finished before reinitializing
 * @x. Also note that the function completion_done() can not be used
 * to know if there are still waiters after complete_all() has been called.
 */
/*
 * complete_all() - 把 completion 永久置为完成并唤醒当前所有 waiter。
 *
 * 调用位置：广播式完成端 → 本函数 → swake_up_all_locked()；等待端随后从
 * wait_for_completion*() 返回，且不会消耗 UINT_MAX。@x 是不可空的借用输入，
 * 调用者保证对象存活。函数可在不能睡眠的上下文使用，但 PREEMPT_RT 要求调用
 * 点位于允许该 raw-lock 唤醒协议的 threaded context，lockdep 负责断言。
 *
 * 返回无直接值；副作用是 done=UINT_MAX 并唤醒队列中全部任务。若要复用，
 * 调用者必须等所有旧 waiter 完全离开后再 reinit_completion()；否则清零会与
 * 尚未完成的 waiter 竞争。completion_done() 只能确认完成发布者已退出锁区，
 * 不能统计 complete_all() 后是否仍有 waiter 正在返回途中。
 */
void complete_all(struct completion *x)
{
	/* flags 只在此临界区保存/恢复本 CPU 中断状态，不表示 completion ownership。 */
	unsigned long flags;

	lockdep_assert_RT_in_threaded_ctx();

	/* 设置永久状态和遍历唤醒必须受同一把 wait.lock 保护。 */
	raw_spin_lock_irqsave(&x->wait.lock, flags);
	x->done = UINT_MAX;
	swake_up_all_locked(&x->wait);
	raw_spin_unlock_irqrestore(&x->wait.lock, flags);
}
EXPORT_SYMBOL(complete_all);

/*
 * do_wait_for_common() - 在已持 wait.lock 的条件下消费令牌或排队睡眠。
 *
 * 调用位置：所有 wait_for_completion*() → wait_for_common[_io]() →
 * __wait_for_common() → 本函数。它是等待协议真正修改 current 状态、swait
 * 队列和 done 计数的核心；调用者负责外层加锁与 acquire/release 标注。
 *
 * @x: 输入/输出，借用的非 NULL completion；wait.lock 在入口已持有且本函数
 *     会在每次睡眠前释放、醒来后重新取得，返回时仍保持持锁。
 * @action: 输入，睡眠动作函数；普通路径为 schedule_timeout()，I/O 记账路径
 *     为 io_schedule_timeout()。函数指针不保存，调用期间可能调度睡眠。
 * @timeout: 输入/输出式值，单位 jiffies；MAX_SCHEDULE_TIMEOUT 表示无限等待，
 *     每次 @action 返回后变为剩余 jiffies，0 表示超时。
 * @state: 输入，发布给唤醒器的 task state，如 TASK_UNINTERRUPTIBLE、
 *     TASK_INTERRUPTIBLE 或 TASK_KILLABLE；决定哪些 pending signal 可中断。
 *
 * 仅能从可睡眠的进程上下文调用。返回 -ERESTARTSYS 表示相应信号中断，0 表示
 * 超时，正数表示已消费一个完成令牌（至少为 1，有限等待时通常是剩余 jiffies）。
 * 若 done==UINT_MAX 则成功但不递减；失败/超时不会消费令牌。局部 wait 节点只在
 * 本栈帧和锁保护期有效，所有出口前都由 __finish_swait() 摘除并恢复 task 状态。
 */
static inline long __sched
do_wait_for_common(struct completion *x,
		   long (*action)(long), long timeout, int state)
{
	/* 有现成令牌时走不睡眠快速路径；锁保证检查与后续递减不可竞争。 */
	if (!x->done) {
		/* wait 是嵌在当前栈上的借用队列节点，返回前必须摘除。 */
		DECLARE_SWAITQUEUE(wait);

		do {
			/*
			 * 阶段 1：可中断状态先处理信号。这里仍持锁，因此不会在决定
			 * 退出与 producer 发布令牌之间留下一个已排队却无人清理的节点。
			 */
			if (signal_pending_state(state, current)) {
				timeout = -ERESTARTSYS;
				break;
			}
			/*
			 * 阶段 2：先在锁内入队，再发布 current 的睡眠状态，最后才解锁。
			 * complete() 也必须取得同一把锁：它要么先留下 done 令牌，要么
			 * 在 waiter 已可见后唤醒它，因而不会发生“事件落在入队前”的丢失。
			 */
			__prepare_to_swait(&x->wait, &wait);
			__set_current_state(state);
			raw_spin_unlock_irq(&x->wait.lock);
			/* action 允许真正调度出去；醒来后必须重新持锁才能检查共享状态。 */
			timeout = action(timeout);
			raw_spin_lock_irq(&x->wait.lock);
		} while (!x->done && timeout);
		/* 无论完成、超时还是信号退出，都摘除栈节点并恢复 TASK_RUNNING。 */
		__finish_swait(&x->wait, &wait);
		/* 未观察到 done 时，原样返回超时 0 或信号错误，不消费任何事件。 */
		if (!x->done)
			return timeout;
	}
	/* 普通 complete() 的令牌只能交给一个 waiter；complete_all 哨兵不可递减。 */
	if (x->done != UINT_MAX)
		x->done--;
	/* 无限等待的成功值原本可能是 0，规范化为 1 以与超时明确区分。 */
	return timeout ?: 1;
}

/*
 * __wait_for_common() - 为 completion 等待建立睡眠检查、锁和注解边界。
 *
 * @x: 借用的已初始化 completion，不可为 NULL；调用者保证等待期间存活。
 * @action: 借用的睡眠函数指针，仅在本次调用中同步执行。
 * @timeout: jiffies 超时或 MAX_SCHEDULE_TIMEOUT，语义传递给核心等待循环。
 * @state: current 等待时使用的 task state，决定信号中断规则。
 *
 * 入口不得位于原子上下文且不得预持 x->wait.lock；本函数可能睡眠。返回类别与
 * do_wait_for_common() 相同。complete_acquire/release() 是锁依赖/同步语义钩子，
 * 当前为空实现，真正互斥及跨 CPU 可见性由 wait.lock 和调度唤醒协议提供。
 */
static inline long __sched
__wait_for_common(struct completion *x,
		  long (*action)(long), long timeout, int state)
{
	/* 将错误的原子上下文调用尽早报告给调试设施。 */
	might_sleep();

	complete_acquire(x);

	/*
	 * 等待路径约定入口中断开启；lock_irq 在检查 done、入队和消费期间关中断，
	 * 与可能来自本 CPU 中断的 complete() 避免自死锁。
	 */
	raw_spin_lock_irq(&x->wait.lock);
	timeout = do_wait_for_common(x, action, timeout, state);
	raw_spin_unlock_irq(&x->wait.lock);

	complete_release(x);

	return timeout;
}

/*
 * wait_for_common() - 使用普通调度记账执行 completion 等待。
 *
 * @x、@timeout、@state 均为借用输入，语义与 __wait_for_common() 相同；本包装
 * 选择 schedule_timeout()，因此等待不计入 I/O wait。可睡眠，返回核心等待结果，
 * 不改变对象 ownership。所有公开的非 I/O wait_for_completion*() 由此进入。
 */
static long __sched
wait_for_common(struct completion *x, long timeout, int state)
{
	return __wait_for_common(x, schedule_timeout, timeout, state);
}

/*
 * wait_for_common_io() - 使用 I/O 等待记账执行 completion 等待。
 *
 * 参数、锁、睡眠和返回契约与 wait_for_common() 相同；唯一差别是选用
 * io_schedule_timeout()，使调度统计把 current 记为等待 I/O。它传统上主要影响
 * blkio 相关统计，不改变 completion 的令牌、超时或信号语义。
 */
static long __sched
wait_for_common_io(struct completion *x, long timeout, int state)
{
	return __wait_for_common(x, io_schedule_timeout, timeout, state);
}

/**
 * wait_for_completion: - waits for completion of a task
 * @x:  holds the state of this particular completion
 *
 * This waits to be signaled for completion of a specific task. It is NOT
 * interruptible and there is no timeout.
 *
 * See also similar routines (i.e. wait_for_completion_timeout()) with timeout
 * and interrupt capability. Also see complete().
 */
/*
 * wait_for_completion() - 不可中断且无限期等待一个完成令牌。
 *
 * 调用者借用不可空、已初始化并在整个等待期间存活的 @x；入口不能持有会阻止
 * 完成端运行的锁，也不能处于原子/中断上下文，因为本函数可能长期睡眠。
 * 它以 TASK_UNINTERRUPTIBLE 经普通调度记账进入核心等待循环。返回无直接值；
 * 返回即保证已观察并消费一个有限令牌，或观察到 complete_all() 的永久状态。
 * ownership 不变，下一步通常读取 producer 在 complete() 前发布的结果。
 */
void __sched wait_for_completion(struct completion *x)
{
	wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(wait_for_completion);

/**
 * wait_for_completion_timeout: - waits for completion of a task (w/timeout)
 * @x:  holds the state of this particular completion
 * @timeout:  timeout value in jiffies
 *
 * This waits for either a completion of a specific task to be signaled or for a
 * specified timeout to expire. The timeout is in jiffies. It is not
 * interruptible.
 *
 * Return: 0 if timed out, and positive (at least 1, or number of jiffies left
 * till timeout) if completed.
 */
/*
 * wait_for_completion_timeout() - 不可中断地等待，最多 @timeout jiffies。
 *
 * @x 是不可空的借用 completion；@timeout 是输入超时，0 表示只做一次受锁检查。
 * 只能在可睡眠进程上下文调用。返回 0 表示期限内没有令牌且未消费状态；正数
 * 表示完成，至少为 1，通常是剩余 jiffies。成功时有限 done 减一，UINT_MAX
 * 不变；对象和返回值均不转移 ownership。
 */
unsigned long __sched
wait_for_completion_timeout(struct completion *x, unsigned long timeout)
{
	return wait_for_common(x, timeout, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(wait_for_completion_timeout);

/**
 * wait_for_completion_io: - waits for completion of a task
 * @x:  holds the state of this particular completion
 *
 * This waits to be signaled for completion of a specific task. It is NOT
 * interruptible and there is no timeout. The caller is accounted as waiting
 * for IO (which traditionally means blkio only).
 */
/*
 * wait_for_completion_io() - 以 I/O wait 记账无限等待一个完成令牌。
 *
 * @x 为不可空借用输入，调用者保证生命周期并处于可睡眠进程上下文。它不可被
 * 信号中断、无超时，返回无直接值；成功消费有限令牌或观察永久完成状态。
 * 与 wait_for_completion() 的同步保证相同，区别仅是 current 的等待时间按
 * I/O（传统上主要是 blkio）统计，便于负载与调度观测。
 */
void __sched wait_for_completion_io(struct completion *x)
{
	wait_for_common_io(x, MAX_SCHEDULE_TIMEOUT, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(wait_for_completion_io);

/**
 * wait_for_completion_io_timeout: - waits for completion of a task (w/timeout)
 * @x:  holds the state of this particular completion
 * @timeout:  timeout value in jiffies
 *
 * This waits for either a completion of a specific task to be signaled or for a
 * specified timeout to expire. The timeout is in jiffies. It is not
 * interruptible. The caller is accounted as waiting for IO (which traditionally
 * means blkio only).
 *
 * Return: 0 if timed out, and positive (at least 1, or number of jiffies left
 * till timeout) if completed.
 */
/*
 * wait_for_completion_io_timeout() - 以 I/O wait 记账执行有期限的不可中断等待。
 *
 * @x 是不可空借用 completion；@timeout 单位 jiffies，0 仅尝试当前令牌。
 * 函数可能睡眠。返回 0 表示超时，正数（至少 1）表示成功及可能的剩余时间；
 * 成功消费一个有限令牌，失败不改变 done，UINT_MAX 永不递减。ownership 不变。
 */
unsigned long __sched
wait_for_completion_io_timeout(struct completion *x, unsigned long timeout)
{
	return wait_for_common_io(x, timeout, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(wait_for_completion_io_timeout);

/**
 * wait_for_completion_interruptible: - waits for completion of a task (w/intr)
 * @x:  holds the state of this particular completion
 *
 * This waits for completion of a specific task to be signaled. It is
 * interruptible.
 *
 * Return: -ERESTARTSYS if interrupted, 0 if completed.
 */
/*
 * wait_for_completion_interruptible() - 可被普通待处理信号中断地无限等待。
 *
 * @x 为不可空借用 completion，调用者保证其跨睡眠存活；只能从可睡眠进程
 * 上下文调用。返回 0 表示成功消费令牌/观察永久完成，-ERESTARTSYS 表示信号
 * 先胜出且未消费令牌，系统调用层可据此决定重启或向用户返回 EINTR。
 */
int __sched wait_for_completion_interruptible(struct completion *x)
{
	/* t 保留核心层的正成功值或 -ERESTARTSYS；公开无超时接口把成功归一为 0。 */
	long t = wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_INTERRUPTIBLE);

	if (t == -ERESTARTSYS)
		return t;
	return 0;
}
EXPORT_SYMBOL(wait_for_completion_interruptible);

/**
 * wait_for_completion_interruptible_timeout: - waits for completion (w/(to,intr))
 * @x:  holds the state of this particular completion
 * @timeout:  timeout value in jiffies
 *
 * This waits for either a completion of a specific task to be signaled or for a
 * specified timeout to expire. It is interruptible. The timeout is in jiffies.
 *
 * Return: -ERESTARTSYS if interrupted, 0 if timed out, positive (at least 1,
 * or number of jiffies left till timeout) if completed.
 */
/*
 * wait_for_completion_interruptible_timeout() - 可中断且有期限地等待完成。
 *
 * @x 为不可空借用输入；@timeout 为 jiffies，0 表示非阻塞检查。函数可能睡眠。
 * 返回 -ERESTARTSYS 表示信号，0 表示超时，正数表示已完成（至少 1 或剩余
 * jiffies）。只有成功路径消费有限 done；所有权和 @timeout 输入均不转移。
 */
long __sched
wait_for_completion_interruptible_timeout(struct completion *x,
					  unsigned long timeout)
{
	return wait_for_common(x, timeout, TASK_INTERRUPTIBLE);
}
EXPORT_SYMBOL(wait_for_completion_interruptible_timeout);

/**
 * wait_for_completion_killable: - waits for completion of a task (killable)
 * @x:  holds the state of this particular completion
 *
 * This waits to be signaled for completion of a specific task. It can be
 * interrupted by a kill signal.
 *
 * Return: -ERESTARTSYS if interrupted, 0 if completed.
 */
/*
 * wait_for_completion_killable() - 仅允许致命信号中断的无限 completion 等待。
 *
 * @x 是不可空借用对象且必须覆盖整个睡眠期。只能从可睡眠进程上下文调用。
 * TASK_KILLABLE 比不可中断等待更易终止，又不会被所有普通信号打断；返回 0
 * 表示成功，-ERESTARTSYS 表示致命信号先到，后者不消费完成令牌。
 */
int __sched wait_for_completion_killable(struct completion *x)
{
	/* t 的正成功值被该无超时 API 归一化为 0。 */
	long t = wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_KILLABLE);

	if (t == -ERESTARTSYS)
		return t;
	return 0;
}
EXPORT_SYMBOL(wait_for_completion_killable);

/*
 * wait_for_completion_state() - 由调用者指定 task state 的无限 completion 等待。
 *
 * 调用位置：需要自定义等待状态的内核路径 → 本函数 → wait_for_common()。
 * @x: 不可空的借用 completion，调用者保证等待期间存活。
 * @state: 输入的 task state 位图；必须是 signal_pending_state() 和调度器认可的
 *     睡眠状态，决定哪些信号中断等待。函数不验证任意位组合是否具备业务意义。
 *
 * 只能从可睡眠进程上下文调用。返回 0 表示完成，-ERESTARTSYS 表示 @state
 * 允许的信号先到且令牌未消费。返回无正成功值，ownership 不变。
 */
int __sched wait_for_completion_state(struct completion *x, unsigned int state)
{
	/* t 保留核心等待结果；公开接口只暴露成功 0 或可重启信号错误。 */
	long t = wait_for_common(x, MAX_SCHEDULE_TIMEOUT, state);

	if (t == -ERESTARTSYS)
		return t;
	return 0;
}
EXPORT_SYMBOL(wait_for_completion_state);

/**
 * wait_for_completion_killable_timeout: - waits for completion of a task (w/(to,killable))
 * @x:  holds the state of this particular completion
 * @timeout:  timeout value in jiffies
 *
 * This waits for either a completion of a specific task to be
 * signaled or for a specified timeout to expire. It can be
 * interrupted by a kill signal. The timeout is in jiffies.
 *
 * Return: -ERESTARTSYS if interrupted, 0 if timed out, positive (at least 1,
 * or number of jiffies left till timeout) if completed.
 */
/*
 * wait_for_completion_killable_timeout() - 仅受致命信号影响的有期限等待。
 *
 * @x 为不可空借用 completion；@timeout 输入单位为 jiffies，0 只检查现有令牌。
 * 可睡眠。返回 -ERESTARTSYS、0、正数分别表示致命信号、超时、完成（至少 1
 * 或剩余 jiffies）；只有完成路径消费有限令牌，UINT_MAX 和 ownership 不变。
 */
long __sched
wait_for_completion_killable_timeout(struct completion *x,
				     unsigned long timeout)
{
	return wait_for_common(x, timeout, TASK_KILLABLE);
}
EXPORT_SYMBOL(wait_for_completion_killable_timeout);

/**
 *	try_wait_for_completion - try to decrement a completion without blocking
 *	@x:	completion structure
 *
 *	Return: 0 if a decrement cannot be done without blocking
 *		 1 if a decrement succeeded.
 *
 *	If a completion is being used as a counting completion,
 *	attempt to decrement the counter without blocking. This
 *	enables us to avoid waiting if the resource the completion
 *	is protecting is not available.
 */
/*
 * try_wait_for_completion() - 在不睡眠的前提下尝试消费一个完成令牌。
 *
 * 上述英文说明的是计数式用法：资源可用次数编码在 done 中，本函数允许调用者
 * 在资源不可用时立即返回，而不是进入等待队列。@x 是不可空、已初始化且在
 * 调用期间存活的借用对象。任何上下文均可调用；内部 irqsave raw spinlock
 * 不睡眠。返回 false 表示锁下确认无令牌，true 表示消费一个有限令牌或观察到
 * UINT_MAX 永久完成。它不报告 waiter 数量，也不转移 @x ownership。
 *
 * 无锁 READ_ONCE 只是避免 done==0 常见失败路径的锁开销，不是提交结果；若读到
 * 非零，仍必须在锁内复查，因为并发 waiter 可能先消费最后一个令牌。
 */
bool try_wait_for_completion(struct completion *x)
{
	/* flags 保存中断状态；ret 在锁内可被并发复查结果改为 false。 */
	unsigned long flags;
	bool ret = true;

	/*
	 * Since x->done will need to be locked only
	 * in the non-blocking case, we check x->done
	 * first without taking the lock so we can
	 * return early in the blocking case.
	 */
	/*
	 * 英文中的 “blocking case” 指“若继续就必须阻塞的无令牌情况”。这里返回
	 * false 而不真正阻塞。READ_ONCE 防止编译器合并/撕裂读取，但不替代锁。
	 */
	if (!READ_ONCE(x->done))
		return false;

	/* 阶段 2：锁内重新验证并原子消费，关闭多个 try-waiter 的竞争窗口。 */
	raw_spin_lock_irqsave(&x->wait.lock, flags);
	if (!x->done)
		ret = false;
	else if (x->done != UINT_MAX)
		x->done--;
	raw_spin_unlock_irqrestore(&x->wait.lock, flags);
	return ret;
}
EXPORT_SYMBOL(try_wait_for_completion);

/**
 *	completion_done - Test to see if a completion has any waiters
 *	@x:	completion structure
 *
 *	Return: 0 if there are waiters (wait_for_completion() in progress)
 *		 1 if there are no waiters.
 *
 *	Note, this will always return true if complete_all() was called on @X.
 */
/*
 * completion_done() - 判断 completion 当前是否已发布完成状态并同步发布者退出。
 *
 * 上述历史英文把返回值描述为“是否有 waiter”，但当前实现实际首先检查 done：
 * false 表示当前没有可消费令牌；true 表示存在令牌或 UINT_MAX。它不能可靠统计
 * 排队/刚被唤醒的 waiter，尤其 complete_all() 后永远返回 true。
 *
 * @x 是不可空、已初始化并在调用期间存活的借用对象。函数不睡眠，可在原子或
 * 中断上下文调用；返回 bool，不消费 done。若返回 true，空的锁临界区还保证
 * 并发 complete()/complete_all() 已释放 wait.lock，不再引用该对象；但别的
 * 使用者仍可能持有 @x，调用者必须用更高层生命周期协议决定能否释放。
 */
bool completion_done(struct completion *x)
{
	/* flags 用于 irqsave 锁同步；本函数没有 completion 令牌 ownership。 */
	unsigned long flags;

	/* 常见未完成路径无需碰热锁；false 是瞬时观察，不承诺未来不会立刻完成。 */
	if (!READ_ONCE(x->done))
		return false;

	/*
	 * If ->done, we need to wait for complete() to release ->wait.lock
	 * otherwise we can end up freeing the completion before complete()
	 * is done referencing it.
	 */
	/*
	 * 若无锁读到 done，发布者可能刚写计数但仍在 swake_up*() 中使用 x。
	 * 取得再释放同一把锁，会等它走出临界区；这提供的是调用结束同步，而非
	 * 对任意 waiter 生命周期的引用计数保证。
	 */
	raw_spin_lock_irqsave(&x->wait.lock, flags);
	raw_spin_unlock_irqrestore(&x->wait.lock, flags);
	return true;
}
EXPORT_SYMBOL(completion_done);
