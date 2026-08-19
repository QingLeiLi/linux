// SPDX-License-Identifier: GPL-2.0-only
/*
 * percpu-rwsem 用“每 CPU 读计数 + 集中的写侧门闩”降低读多写少场景的
 * 共享缓存行争用。无写者时，头文件中的读侧快速路径只修改本 CPU 计数；
 * 写者到来后，rcu_sync 先让后续读者转入本文件的慢路径，再用 block 排斥
 * 其他写者和新读者，最后等待所有旧读者的计数归零。
 *
 * 阅读主线可以概括为：
 *   percpu_down_read() -> 本 CPU 计数或 __percpu_down_read()
 *   percpu_down_write() -> rcu_sync_enter() -> block -> 等待读者清空
 *   percpu_up_write() -> 释放 block -> 唤醒队首 -> 延后恢复读侧快速路径
 * A/D 与 B/C 两组内存屏障分别封住“读者进入”和“读者退出”的竞态窗口。
 */
#include <linux/atomic.h>
#include <linux/percpu.h>
#include <linux/wait.h>
#include <linux/lockdep.h>
#include <linux/percpu-rwsem.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/debug.h>
#include <linux/errno.h>
#include <trace/events/lock.h>

/*
 * 初始化动态 percpu-rwsem。
 *
 * 输入：sem 指向调用者提供的对象；name/key 仅用于 lockdep 分类。
 * 输出：成功返回 0；每 CPU 计数分配失败返回 -ENOMEM，此时其余成员尚未初始化。
 * 并发约束：对象尚未发布，调用者必须保证没有读写者并发访问。
 * 状态变化：建立读计数、RCU 快慢路径状态、单写者等待点、FIFO 等待队列及
 * block=0 的初始状态。静态定义的对象则由头文件初始化器完成等价工作。
 */
int __percpu_init_rwsem(struct percpu_rw_semaphore *sem,
			const char *name, struct lock_class_key *key)
{
	sem->read_count = alloc_percpu(int);
	if (unlikely(!sem->read_count))
		return -ENOMEM;

	rcu_sync_init(&sem->rss);
	rcuwait_init(&sem->writer);
	init_waitqueue_head(&sem->waiters);
	atomic_set(&sem->block, 0);
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	debug_check_no_locks_freed((void *)sem, sizeof(*sem));
	lockdep_init_map(&sem->dep_map, name, key, 0);
#endif
	return 0;
}
EXPORT_SYMBOL_GPL(__percpu_init_rwsem);

/*
 * 销毁动态 percpu-rwsem 并释放每 CPU 读计数。
 *
 * 前置条件：调用者已阻止新的使用，并已保证没有持锁者或等待者；本函数不会
 * 替调用者排空并发访问。rcu_sync_dtor() 会等待遗留 RCU 回调结束。
 * 特殊兼容：read_count 为 NULL 时直接返回，以兼容仅 kzalloc 后就进入的清理
 * 路径。释放后将指针清零，使后续误用更容易暴露。
 */
void percpu_free_rwsem(struct percpu_rw_semaphore *sem)
{
	/*
	 * 这是临时兼容措施：alloc_super() 的错误路径假定对象只经过 kzalloc，
	 * 尚未成功初始化时也可以安全调用 percpu_free_rwsem()。
	 */
	/*
	 * XXX: temporary kludge. The error path in alloc_super()
	 * assumes that percpu_free_rwsem() is safe after kzalloc().
	 */
	if (!sem->read_count)
		return;

	rcu_sync_dtor(&sem->rss);
	free_percpu(sem->read_count);
	/* 清空已释放的地址，让释放后使用更快表现为明显错误。 */
	sem->read_count = NULL; /* catch use after free bugs */
}
EXPORT_SYMBOL_GPL(percpu_free_rwsem);

/*
 * 尝试取得慢路径读锁；调用者必须已经禁止抢占。
 *
 * 先增加当前 CPU 的读计数，再检查 block。若未封锁，则 acquire 读取与写侧
 * release 解锁配对，读临界区从此开始；若已封锁，则撤销同一 CPU 上的计数，
 * 提醒可能正在等待读者清空的写者重查条件并返回 false。
 */
static bool __percpu_down_read_trylock(struct percpu_rw_semaphore *sem)
{
	this_cpu_inc(*sem->read_count);

	/*
	 * 禁止抢占保证增减发生在同一 CPU，避免迁移后把两个 CPU 的计数都改坏。
	 * A 屏障与写侧 D 配对：读者若没看到 block=1，写者就必须看到本次计数增加；
	 * 写者若先扫描而漏掉稍后的增加，该读者则必须看到 block=1 并立即撤销计数。
	 */
	/*
	 * Due to having preemption disabled the decrement happens on
	 * the same CPU as the increment, avoiding the
	 * increment-on-one-CPU-and-decrement-on-another problem.
	 *
	 * If the reader misses the writer's assignment of sem->block, then the
	 * writer is guaranteed to see the reader's increment.
	 *
	 * Conversely, any readers that increment their sem->read_count after
	 * the writer looks are guaranteed to see the sem->block value, which
	 * in turn means that they are guaranteed to immediately decrement
	 * their sem->read_count, so that it doesn't matter that the writer
	 * missed them.
	 */

	/* A 与写侧成功设置 block 后的 D 配对，禁止计数增加和门闩检查重排。 */
	smp_mb(); /* A matches D */

	/*
	 * block=0 时，acquire 读取承接 percpu_up_write() 的 release 写入，确保读者
	 * 能看到上一写临界区的结果；成功返回后才真正进入读临界区。
	 */
	/*
	 * If !sem->block the critical section starts here, matched by the
	 * release in percpu_up_write().
	 */
	if (likely(!atomic_read_acquire(&sem->block)))
		return true;

	this_cpu_dec(*sem->read_count);

	/* 计数已撤销，推动正在 readers_active_check() 中睡眠的写者重新求和。 */
	/* Prod writer to re-evaluate readers_active_check() */
	rcuwait_wake_up(&sem->writer);

	return false;
}

/*
 * 尝试把 block 从 0 改为 1，以同时取得写者互斥权并关闭慢路径读者入口。
 * 先读是无争用优化；atomic_xchg() 才是决定胜负的原子操作，并在成功时提供
 * 后续 D 屏障所需的全屏障语义。失败不会睡眠，由上层决定排队。
 */
static inline bool __percpu_down_write_trylock(struct percpu_rw_semaphore *sem)
{
	if (atomic_read(&sem->block))
		return false;

	return atomic_xchg(&sem->block, 1) == 0;
}

/*
 * 等待队列唤醒回调共用的非阻塞取锁入口。
 * reader=true 时临时禁止抢占，保证每 CPU 计数成对落在同一 CPU；reader=false
 * 时竞争 block。返回 true 表示调用者已替被唤醒任务取得相应读锁或写锁。
 */
static bool __percpu_rwsem_trylock(struct percpu_rw_semaphore *sem, bool reader)
{
	if (reader) {
		bool ret;

		preempt_disable();
		ret = __percpu_down_read_trylock(sem);
		preempt_enable();

		return ret;
	}
	return __percpu_down_write_trylock(sem);
}

/*
 * wait_queue_entry::func 的返回值约定如下：负数终止唤醒并返回错误；0 表示
 * 当前项未唤醒，继续检查下一项；正数表示已唤醒，独占项会计入唤醒额度。
 *
 * 这里把读者和写者都作为 EXCLUSIVE 项，借此维持 FIFO 次序；回调却让成功
 * 获锁的读者返回 0，使通用唤醒循环继续批量放行相邻读者。遇到第一个成功
 * 获锁的写者，或任一因 trylock 失败而不能越过的队首项时，扫描才停止。
 */
/*
 * The return value of wait_queue_entry::func means:
 *
 *  <0 - error, wakeup is terminated and the error is returned
 *   0 - no wakeup, a next waiter is tried
 *  >0 - woken, if EXCLUSIVE, counted towards @nr_exclusive.
 *
 * We use EXCLUSIVE for both readers and writers to preserve FIFO order,
 * and play games with the return value to allow waking multiple readers.
 *
 * Specifically, we wake readers until we've woken a single writer, or until a
 * trylock fails.
 */
/*
 * 在持有 waiters.lock 的唤醒扫描中，尝试替一个排队任务取得锁并交付所有权。
 *
 * 输入：WQ_FLAG_CUSTOM 区分读者，key 是被释放的 sem；mode/wake_flags 由通用
 * waitqueue 接口传入但本回调不需要。
 * 成功：从队列摘除节点，以 release 清空 private，再唤醒并安全释放任务引用。
 * 失败：返回 1 终止本轮扫描，防止破坏 FIFO。读者成功返回 0 以继续成批唤醒，
 * 写者成功返回 1，确保一次只交付一个写者。
 */
static int percpu_rwsem_wake_function(struct wait_queue_entry *wq_entry,
				      unsigned int mode, int wake_flags,
				      void *key)
{
	bool reader = wq_entry->flags & WQ_FLAG_CUSTOM;
	struct percpu_rw_semaphore *sem = key;
	struct task_struct *p;

	/* 与新到达的 percpu_down_write() 并发时，block 可能先被对方抢走。 */
	/* concurrent against percpu_down_write(), can get stolen */
	if (!__percpu_rwsem_trylock(sem, reader))
		return 1;

	p = get_task_struct(wq_entry->private);
	list_del_init(&wq_entry->entry);
	/* release 发布摘队和锁交付；等待任务以 acquire 观察 private 变为 NULL。 */
	smp_store_release(&wq_entry->private, NULL);

	wake_up_process(p);
	put_task_struct(p);

	/* 连续放行读者；一旦成功交付一个写者便停止本轮唤醒。 */
	return !reader; /* wake (readers until) 1 writer */
}

/*
 * 在 percpu-rwsem 的 FIFO 队列中等待，并由唤醒回调替当前任务取得锁。
 *
 * 调用者尚未持锁且允许睡眠。持 waiters.lock 再重试一次可与 up_write() 的
 * 唤醒串行化：重试若失败，节点一定先入队，释放方不会发生“检查为空后漏睡”。
 * reader 标记决定竞争读锁还是写锁，freeze 决定睡眠状态是否可被冻结。
 * 返回时锁已经由当前任务自己重试或由回调交付，无需再次 trylock。
 */
static void percpu_rwsem_wait(struct percpu_rw_semaphore *sem, bool reader,
			      bool freeze)
{
	DEFINE_WAIT_FUNC(wq_entry, percpu_rwsem_wake_function);
	bool wait;

	spin_lock_irq(&sem->waiters.lock);
	/*
	 * 与 percpu_up_write() 的唤醒在同一队列锁下串行：若此处重试仍失败，
	 * 释放方随后检查队列时必然能看到本节点。
	 */
	/*
	 * Serialize against the wakeup in percpu_up_write(), if we fail
	 * the trylock, the wakeup must see us on the list.
	 */
	wait = !__percpu_rwsem_trylock(sem, reader);
	if (wait) {
		wq_entry.flags |= WQ_FLAG_EXCLUSIVE | reader * WQ_FLAG_CUSTOM;
		__add_wait_queue_entry_tail(&sem->waiters, &wq_entry);
	}
	spin_unlock_irq(&sem->waiters.lock);

	while (wait) {
		set_current_state(TASK_UNINTERRUPTIBLE |
				  (freeze ? TASK_FREEZABLE : 0));
		/* 回调以 release 清空 private；acquire 读取既是交付完成信号也承接其写入。 */
		if (!smp_load_acquire(&wq_entry.private))
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
}

/*
 * 头文件读侧包装器进入慢路径后的实际取锁函数。
 *
 * 前置条件：包装器已经禁止抢占。先做一次不会睡眠的慢路径尝试；try=true 时
 * 失败即返回 false。普通获取失败后，为允许调度而临时恢复抢占，进入 FIFO
 * 等待；回调交付读锁后重新禁止抢占，以满足外层包装器的成对约定。
 * 返回 true 表示已取得读锁，争用区间通过 lock trace 记录。
 */
bool __sched __percpu_down_read(struct percpu_rw_semaphore *sem, bool try,
				bool freeze)
{
	if (__percpu_down_read_trylock(sem))
		return true;

	if (try)
		return false;

	trace_contention_begin(sem, LCB_F_PERCPU | LCB_F_READ);
	/* 外层在调用前禁止了抢占；睡眠前必须暂时恢复，返回后再还原。 */
	preempt_enable();
	/* 参数标签表明当前排队项按读者规则竞争。 */
	percpu_rwsem_wait(sem, /* .reader = */ true, freeze);
	preempt_disable();
	trace_contention_end(sem, 0);

	return true;
}
EXPORT_SYMBOL_GPL(__percpu_down_read);

/*
 * 对一个每 CPU 原子宽度变量做模算术求和。这里不追求普通意义上的精确快照；
 * 在 block=1 阻止新读者留下有效计数后，零和才成为稳定的“读者已排空”条件。
 * compiletime_assert_atomic_type() 保证单个 CPU 槽位可被并发安全读取。
 */
#define per_cpu_sum(var)						\
({									\
	TYPEOF_UNQUAL(var) __sum = 0;					\
	int cpu;							\
	compiletime_assert_atomic_type(__sum);				\
	for_each_possible_cpu(cpu)					\
		__sum += per_cpu(var, cpu);				\
	__sum;								\
})

/*
 * 提供诊断性的读锁状态查询：仅当计数模和非零且当前没有写者封锁时返回 true。
 * 它不是获取锁的同步原语，也不是跨 CPU 一致快照；调用者不能据此决定随后
 * 是否可以无锁访问受保护数据。
 */
bool percpu_is_read_locked(struct percpu_rw_semaphore *sem)
{
	return per_cpu_sum(*sem->read_count) != 0 && !atomic_read(&sem->block);
}
EXPORT_SYMBOL_GPL(percpu_is_read_locked);

/*
 * 在 sem->block 已经置位的前提下，判断所有每 CPU 读计数的模和是否为零。
 * 一旦读到零，该结论是稳定的：后来到达的读者即使先增加本 CPU 计数，也会
 * 看到 block=1 并立即在同一 CPU 撤销，不可能成为新的有效读临界区。
 */
/*
 * Return true if the modular sum of the sem->read_count per-CPU variable is
 * zero.  If this sum is zero, then it is stable due to the fact that if any
 * newly arriving readers increment a given counter, they will immediately
 * decrement that same counter.
 *
 * Assumes sem->block is set.
 */
/*
 * 写者等待条件检查。返回 false 时 rcuwait 继续睡眠；返回 true 前的 C 屏障
 * 与读侧退出的 B 配对，保证写者一旦观察到计数归零，也能观察到这些读者在
 * 临界区内完成的全部访问。
 */
static bool readers_active_check(struct percpu_rw_semaphore *sem)
{
	if (per_cpu_sum(*sem->read_count) != 0)
		return false;

	/* 观察到读计数的减少后，必须再保证整个先前读临界区都对写者可见。 */
	/*
	 * If we observed the decrement; ensure we see the entire critical
	 * section.
	 */

	/* C 与 __percpu_up_read() 中的 B 配对，封住读者退出一侧的重排。 */
	smp_mb(); /* C matches B */

	return true;
}

/*
 * 阻塞取得 percpu-rwsem 写锁。
 *
 * 过程分两道门：rcu_sync_enter() 先关闭头文件中的读侧快速路径并等待一个 RCU
 * 宽限期，使此前可能未检查 block 的快速读者都已可由每 CPU 计数追踪；随后
 * 原子设置 block，排斥其他写者并迫使慢路径新读者排队。最后通过 rcuwait 等待
 * 所有旧读者计数归零。返回时调用者独占受保护数据，函数可能睡眠。
 */
void __sched percpu_down_write(struct percpu_rw_semaphore *sem)
{
	bool contended = false;

	might_sleep();
	rwsem_acquire(&sem->dep_map, 0, 0, _RET_IP_);

	/* 通知读者转入慢路径；首次进入还要等待宽限期覆盖既有快速路径读者。 */
	/* Notify readers to take the slow path. */
	rcu_sync_enter(&sem->rss);

	/*
	 * 尝试设置 block：它既提供写者之间的互斥，也让此后到达的慢路径读者阻塞。
	 * 竞争失败时按 FIFO 等待，唤醒回调会在真正唤醒任务前替它取得 block。
	 */
	/*
	 * Try set sem->block; this provides writer-writer exclusion.
	 * Having sem->block set makes new readers block.
	 */
	if (!__percpu_down_write_trylock(sem)) {
		trace_contention_begin(sem, LCB_F_PERCPU | LCB_F_WRITE);
		/* 参数标签表明当前排队项按写者规则竞争。 */
		percpu_rwsem_wait(sem, /* .reader = */ false, false);
		contended = true;
	}

	/* 成功的 atomic_xchg() 隐含全屏障 D，与读者进入路径的 A 配对。 */
	/* smp_mb() implied by __percpu_down_write_trylock() on success -- D matches A */

	/*
	 * A/D 配对保证二选一：旧读者若没看到 block=1，写者就一定看到它增加的
	 * read_count 并等待；后来增加计数的读者则一定看到 block=1 并撤销、排队。
	 */
	/*
	 * If they don't see our store of sem->block, then we are guaranteed to
	 * see their sem->read_count increment, and therefore will wait for
	 * them.
	 */

	/* block 已关闭新入口，现在等待所有仍有效的旧读者完成。 */
	/* Wait for all active readers to complete. */
	rcuwait_wait_event(&sem->writer, readers_active_check(sem), TASK_UNINTERRUPTIBLE);
	if (contended)
		trace_contention_end(sem, 0);
}
EXPORT_SYMBOL_GPL(percpu_down_write);

/*
 * 释放写锁并按 FIFO 把锁交给等待者，随后安排恢复读侧快速路径。
 *
 * atomic_set_release(block, 0) 发布本写临界区的结果；等待队列回调替队首任务
 * trylock，能够连续交付相邻读者或只交付一个写者。rcu_sync_exit() 只是启动或
 * 合并宽限期，等它完成后 rcu_sync_is_idle() 才重新允许无共享门闩的读快路径。
 */
void percpu_up_write(struct percpu_rw_semaphore *sem)
{
	rwsem_release(&sem->dep_map, _RET_IP_);

	if (trace_contended_release_enabled() && wq_has_sleeper(&sem->waiters))
		trace_call__contended_release(sem);

	/*
	 * 先声明写者完成，但暂不恢复快速路径。新读者仍经慢路径的 acquire 操作，
	 * 因而能承接这里的 release 并看见刚结束的写临界区结果。
	 */
	/*
	 * Signal the writer is done, no fast path yet.
	 *
	 * One reason that we cannot just immediately flip to readers_fast is
	 * that new readers might fail to see the results of this writer's
	 * critical section.
	 *
	 * Therefore we force it through the slow path which guarantees an
	 * acquire and thereby guarantees the critical section's consistency.
	 */
	atomic_set_release(&sem->block, 0);

	/* 推动队首等待者；回调可批量放行读者，或只交付一个写者。 */
	/*
	 * Prod any pending reader/writer to make progress.
	 */
	__wake_up(&sem->waiters, TASK_NORMAL, 1, sem);

	/*
	 * 至少再经过一个 RCU-sched 宽限期后才重新开放读快路径；内部有引用计数，
	 * 因此相邻写者可在写锁外安全合并 enter/exit，而不会过早切回快速模式。
	 */
	/*
	 * Once this completes (at least one RCU-sched grace period hence) the
	 * reader fast path will be available again. Safe to use outside the
	 * exclusive write lock because its counting.
	 */
	rcu_sync_exit(&sem->rss);
}
EXPORT_SYMBOL_GPL(percpu_up_write);

/*
 * 读侧慢路径释放函数；头文件包装器保证调用期间禁止抢占。
 *
 * B 屏障先把读临界区内的访问排在本 CPU 计数递减之前，与写者零计数检查后的
 * C 配对。递减后唤醒唯一可能等待“旧读者排空”的写者，让它重新求和。
 * rcu_sync 退出宽限期内虽已没有真正等待写者，读者仍可能经过这里，因此跟踪
 * 事件只在 rcuwait 确实活跃时上报。
 */
void __percpu_up_read(struct percpu_rw_semaphore *sem)
{
	lockdep_assert_preemption_disabled();
	/*
	 * percpu_up_write() 返回后到 RCU 宽限期结束前，rcu_sync_is_idle() 仍为假，
	 * 读者会继续走此慢路径；只有写者确实在等待读计数排空时才记录争用释放。
	 */
	/*
	 * After percpu_up_write() completes, rcu_sync_is_idle() can still
	 * return false during the grace period, forcing readers into this
	 * slowpath. Only trace when a writer is actually waiting for
	 * readers to drain.
	 */
	if (trace_contended_release_enabled() && rcuwait_active(&sem->writer))
		trace_call__contended_release(sem);
	/* 慢路径读者每次只需唤醒那个等待全部读者清空的单一写者。 */
	/*
	 * slowpath; reader will only ever wake a single blocked
	 * writer.
	 */
	/* B 与 readers_active_check() 中的 C 配对，先发布读临界区再减少计数。 */
	smp_mb(); /* B matches C */
	/*
	 * 换言之，写者若观察到本次递减——尤其是使总和变为零的那次——也必须
	 * 同时观察到本读临界区此前完成的访问。
	 */
	/*
	 * In other words, if they see our decrement (presumably to
	 * aggregate zero, as that is the only time it matters) they
	 * will also see our critical section.
	 */
	this_cpu_dec(*sem->read_count);
	rcuwait_wake_up(&sem->writer);
}
EXPORT_SYMBOL_GPL(__percpu_up_read);
