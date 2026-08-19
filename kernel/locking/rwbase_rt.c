// SPDX-License-Identifier: GPL-2.0-only

/*
 * RT-specific reader/writer semaphores and reader/writer locks
 *
 * down_write/write_lock()
 *  1) Lock rtmutex
 *  2) Remove the reader BIAS to force readers into the slow path
 *  3) Wait until all readers have left the critical section
 *  4) Mark it write locked
 *
 * up_write/write_unlock()
 *  1) Remove the write locked marker
 *  2) Set the reader BIAS, so readers can use the fast path again
 *  3) Unlock rtmutex, to release blocked readers
 *
 * down_read/read_lock()
 *  1) Try fast path acquisition (reader BIAS is set)
 *  2) Take tmutex::wait_lock, which protects the writelocked flag
 *  3) If !writelocked, acquire it for read
 *  4) If writelocked, block on tmutex
 *  5) unlock rtmutex, goto 1)
 *
 * up_read/read_unlock()
 *  1) Try fast path release (reader count != 1)
 *  2) Wake the writer waiting in down_write()/write_lock() #3
 *
 * down_read/read_lock()#3 has the consequence, that rw semaphores and rw
 * locks on RT are not writer fair, but writers, which should be avoided in
 * RT tasks (think mmap_sem), are subject to the rtmutex priority/DL
 * inheritance mechanism.
 *
 * It's possible to make the rw primitives writer fair by keeping a list of
 * active readers. A blocked writer would force all newly incoming readers
 * to block on the rtmutex, but the rtmutex would have to be proxy locked
 * for one reader after the other. We can't use multi-reader inheritance
 * because there is no way to support that with SCHED_DEADLINE.
 * Implementing the one by one reader boosting/handover mechanism is a
 * major surgery for a very dubious value.
 *
 * The risk of writer starvation is there, but the pathological use cases
 * which trigger it are not necessarily the typical RT workloads.
 *
 * Fast-path orderings:
 * The lock/unlock of readers can run in fast paths: lock and unlock are only
 * atomic ops, and there is no inner lock to provide ACQUIRE and RELEASE
 * semantics of rwbase_rt. Atomic ops should thus provide _acquire()
 * and _release() (or stronger).
 *
 * Common code shared between RT rw_semaphore and rwlock
 */

/*
 * PREEMPT_RT 的 rw_semaphore 与 rwlock 共用此读写状态机，但由包含者提供调度和 rtmutex 适配器。
 *
 * readers 是整个协议的状态字：初始 READER_BIAS 为负数；每个活跃 reader 加一，所以负值表示仍可
 * 从读快路径进入。writer 先取得 rtmutex，再减去 READER_BIAS，使计数变为非负并关闭读快路径；
 * 待旧 readers 全部离开、计数归零后写入 WRITER_BIAS，才真正建立独占写持有。写释放恢复
 * READER_BIAS，随后释放 rtmutex，让排队参与者继续。
 *
 * 读获取/释放的 fast path 只有原子操作，没有内部锁替它们提供 ACQUIRE/RELEASE，因此成功加读者
 * 必须使用 acquire，减读者与恢复 bias 必须提供 release 或更强顺序。RT 版本不保证 writer 公平：
 * writer 不能把优先级同时继承给多个既有 reader；只要 writer 尚未成为 rtmutex owner 并移除 bias，
 * 新 reader 仍可绕过已排队 writer 从快路径进入，取得 rtmutex 的 reader 也可转成共享持有。选择逐个
 * 代理提升并交接 reader 会与 SCHED_DEADLINE 不兼容，复杂度也远高于收益。结果是
 * 病理读流下 writer 可能饥饿，但 writer 一旦持有 rtmutex，后来阻塞者仍可通过 rtmutex 的 PI/DL
 * 机制提升它。上述各段依次完整描述写获取、写释放、读获取、读释放、公平性取舍及快路径内存序。
 */

/*
 * rwbase_read_trylock() - 仅用 readers 状态字尝试取得一个共享读持有。
 *
 * 调用位置：rwbase_read_lock() 的首选快路径，也直接服务 rwsem/rwlock 的 trylock 包装；成功后由
 * rwbase_read_unlock() 配对。@rwb 是非空、已初始化且仅借用的 rwbase；函数不转移其 ownership。
 * 入口不要求 wait_lock，可在不可睡眠路径调用，函数本身不睡眠。r 保存每轮观察到的状态快照：负值
 * 表示 READER_BIAS 仍存在，r + 1 是加入当前 reader 后的新计数；cmpxchg 失败会把 r 更新为最新值。
 * 返回 1 表示已以 acquire 语义计入当前 reader，调用者获得共享持有；返回 0 表示 writer 已关闭读
 * 快路径，状态未被本次调用改变。成功的 acquire 与 reader/写释放的 release 配对，使临界区读写
 * 不越过锁边界；没有引用、分配或独立失败清理。
 */

static __always_inline int rwbase_read_trylock(struct rwbase_rt *rwb)
{
	int r;

	/*
	 * Increment reader count, if sem->readers < 0, i.e. READER_BIAS is
	 * set.
	 */
	/*
	 * 仅当 readers 小于零、即 READER_BIAS 尚在时递增读者数。循环中的 CAS 同时验证“读入口仍开放”
	 * 与“基于同一快照加一”；若 writer 并发移除 bias，失败回填的 r 会变为非负，循环便退出而不会
	 * 越过 writer 的关闭边界。
	 */
	for (r = atomic_read(&rwb->readers); r < 0;) {
		if (likely(atomic_try_cmpxchg_acquire(&rwb->readers, &r, r + 1)))
			return 1;
	}
	return 0;
}

/*
 * __rwbase_read_lock() - 读快路径关闭后，通过底层 rtmutex 慢路径取得共享读持有。
 *
 * 调用位置：rwbase_read_lock() 在 rwbase_read_trylock() 失败后进入；成功后立即释放仅用于串行化
 * writer 的 rtmutex，把长期持有只记入 readers。@rwb 是非空的借用输入输出对象；@state 是包含者
 * 选择的任务等待状态，rwsem 可传 TASK_INTERRUPTIBLE/KILLABLE/UNINTERRUPTIBLE，RT rwlock 适配器
 * 则忽略它并使用 TASK_RTLOCK_WAIT。入口不持 rtm->wait_lock，允许睡眠，并要求 current 尚未在另一
 * PI 链上阻塞。rtm 是 rwb 内嵌 rtmutex 的借用别名；wake_q 收集必须延迟到 wait_lock 外的普通唤醒；
 * ret 保存适配后慢锁结果。
 *
 * 阶段一在 waiter 入队前完成调度记账并带关中断取得 wait_lock；阶段二保持该锁直接进入 rtmutex
 * 慢路径，闭合“旧 reader 唤醒 writer 与新 reader 排队”的竞态窗口；阶段三成功时把当前任务计为
 * reader，再在锁外消费 wake_q；阶段四释放临时持有的 rtmutex并结束记账。返回 0 表示已取得共享
 * 读持有；rwsem 可返回负 errno（通常 -EINTR），此时未增加 readers、未持 rtmutex，状态已清理；
 * RT rwlock 的适配器恒成功。函数不取得长期对象引用，调用者成功后必须配对读解锁。
 */
static int __sched __rwbase_read_lock(struct rwbase_rt *rwb,
				      unsigned int state)
{
	struct rt_mutex_base *rtm = &rwb->rtmutex;
	DEFINE_WAKE_Q(wake_q);
	int ret;

	rwbase_pre_schedule();
	/* wait_lock 同时稳定 rtmutex waiter/owner 与 rwbase 的 writer 转换；持有期间不可普通调度。 */
	raw_spin_lock_irq(&rtm->wait_lock);

	/*
	 * Call into the slow lock path with the rtmutex->wait_lock
	 * held, so this can't result in the following race:
	 *
	 * Reader1		Reader2		Writer
	 *			down_read()
	 *					down_write()
	 *					rtmutex_lock(m)
	 *					wait()
	 * down_read()
	 * unlock(m->wait_lock)
	 *			up_read()
	 *			wake(Writer)
	 *					lock(m->wait_lock)
	 *					sem->writelocked=true
	 *					unlock(m->wait_lock)
	 *
	 *					up_write()
	 *					sem->writelocked=false
	 *					rtmutex_unlock(m)
	 *			down_read()
	 *					down_write()
	 *					rtmutex_lock(m)
	 *					wait()
	 * rtmutex_lock(m)
	 *
	 * That would put Reader1 behind the writer waiting on
	 * Reader2 to call up_read(), which might be unbound.
	 */
	/*
	 * 必须在仍持 rtmutex->wait_lock 时进入慢锁，否则会出现上图竞态：Reader1 先放开 wait_lock、尚未
	 * 排到 rtmutex；Reader2 的最后一次 up_read() 唤醒 writer，writer 完成一次写持有和释放后又再次
	 * 取写锁并等待 Reader2；此时 Reader1 才排到 writer 后面。writer 会等待可能无限延迟的 Reader2
	 * up_read()，而 Reader1 又被放到该 writer 之后，破坏“被 reader 唤醒的 writer 与新 reader”之间
	 * 的顺序。锁内入队使 Reader1 要么先取得 rtmutex，要么成为能参与 PI 的明确 waiter。
	 */

	/* 从此处开始记录真实慢路径争用；失败码会原样交给结束事件。 */
	trace_contention_begin(rwb, LCB_F_RT | LCB_F_READ);

	/*
	 * For rwlocks this returns 0 unconditionally, so the below
	 * !ret conditionals are optimized out.
	 */
	/*
	 * 对 RT rwlock，适配函数只在成功取得 rtmutex 后返回 0，因此编译器会消除后续 !ret 判断；对
	 * rwsem，可中断等待可能返回负 errno，当前仍持 wait_lock，wake_q 也仍由本函数负责消费。
	 */
	ret = rwbase_rtmutex_slowlock_locked(rtm, state, &wake_q);

	/*
	 * On success the rtmutex is held, so there can't be a writer
	 * active. Increment the reader count and immediately drop the
	 * rtmutex again.
	 *
	 * rtmutex->wait_lock has to be unlocked in any case of course.
	 */
	/*
	 * 成功时 current 临时持有 rtmutex，故 writer 不可能活跃；此时在 wait_lock 下增加 readers，把
	 * “排他通行权”转换为长期共享持有。无论成功失败都必须释放 wait_lock，失败则不能改读者计数。
	 */
	if (!ret)
		atomic_inc(&rwb->readers);

	/*
	 * wake_up_q() 必须在 raw spinlock 外执行。显式禁止抢占把“释放 wait_lock → 消费 wake_q”保持为
	 * 一个调度安全段；普通 wake_q 自身不负责恢复这里关闭的抢占。
	 */
	preempt_disable();
	raw_spin_unlock_irq(&rtm->wait_lock);
	wake_up_q(&wake_q);
	preempt_enable();

	if (!ret)
		rwbase_rtmutex_unlock(rtm);

	/* 所有临时锁和延迟唤醒责任已清除，随后关闭 trace 并与 pre_schedule 配对恢复调度记账。 */
	trace_contention_end(rwb, ret);
	rwbase_post_schedule();
	return ret;
}

/*
 * rwbase_read_lock() - 先试无等待读快路径，必要时转入可调度慢路径。
 *
 * 调用位置：RT rwsem 的 down_read*() 与 RT rwlock 的 rt_read_lock() 公共核心。@rwb 是非空、已初始化
 * 的借用输入输出对象；@state 指定慢路径等待状态，快路径不读取它。入口不得已经通过 pi_blocked_on
 * 阻塞于另一把 PI 锁；函数本身可能睡眠，仅快路径保证不睡眠。返回 0 表示 readers 已包含当前读者；
 * 慢路径可向 rwsem 调用者返回负 errno 且未持锁，RT rwlock 配置不会失败。成功没有对象 ownership
 * 转移，调用者下一步进入共享临界区并最终调用 rwbase_read_unlock()。
 */
static __always_inline int rwbase_read_lock(struct rwbase_rt *rwb,
					    unsigned int state)
{
	lockdep_assert(!current->pi_blocked_on);

	/* 快路径以 acquire 原子递增完成发布后的观察；writer 已清 bias 时才支付排队和调度成本。 */
	if (rwbase_read_trylock(rwb))
		return 0;

	return __rwbase_read_lock(rwb, state);
}

/*
 * __rwbase_read_unlock() - 最后一个活跃 reader 离开时唤醒正在等待读者清空的 writer。
 *
 * 仅由 rwbase_read_unlock() 在 atomic_dec_and_test() 把 readers 变为 0 后调用。@rwb 是非空借用的
 * 输入输出对象；@state 指定应唤醒的任务状态，rwsem 使用 TASK_NORMAL，RT rwlock 使用
 * TASK_RTLOCK_WAIT。入口不持 wait_lock，函数不普通睡眠；rtm 是内嵌 rtmutex 的借用别名，owner 是
 * 在 wait_lock 下取得的 writer task 借用快照，wqh 暂存锁外唤醒及必要的临时 task 引用。
 *
 * 函数在关中断的 wait_lock 下读取 rtmutex owner 并把它加入专用 wake 队列，随后保持抢占关闭释放
 * wait_lock，在 rt_mutex_wake_up_q() 中完成唤醒和引用归还。返回无直接值；正常副作用是让 writer
 * 从等待 readers==0 的循环重新检查条件。writer 因信号并发退出时允许一次伪唤醒，但它也必须取得
 * 同一 wait_lock 才能恢复 readers，因此 owner 快照和状态转换不会失去同步。调用者已释放当前读
 * 持有，本函数不转移 rwb ownership。
 */
static void __sched __rwbase_read_unlock(struct rwbase_rt *rwb,
					 unsigned int state)
{
	struct rt_mutex_base *rtm = &rwb->rtmutex;
	struct task_struct *owner;
	DEFINE_RT_WAKE_Q(wqh);

	/* wait_lock 使 owner 观察与 writer 的取消/清理互斥；锁内仅排队，不直接调用调度器唤醒。 */
	raw_spin_lock_irq(&rtm->wait_lock);
	/*
	 * Wake the writer, i.e. the rtmutex owner. It might release the
	 * rtmutex concurrently in the fast path (due to a signal), but to
	 * clean up rwb->readers it needs to acquire rtm->wait_lock. The
	 * worst case which can happen is a spurious wakeup.
	 */
	/*
	 * 唤醒对象就是持有 rtmutex、正等待读者清空的 writer。它可能因信号在快退出路径并发释放
	 * rtmutex，但恢复 readers 必须经过同一 wait_lock；因此这里要么看到稳定 owner 并排队唤醒，
	 * 要么看到 NULL。竞态最坏只多一次无害唤醒，不会漏掉 bias 清理或访问已释放的 task。
	 */
	owner = rt_mutex_owner(rtm);
	if (owner)
		rt_mutex_wake_q_add_task(&wqh, owner, state);

	/* Pairs with the preempt_enable in rt_mutex_wake_up_q() */
	/* 与 rt_mutex_wake_up_q() 尾部的 preempt_enable() 配对；队列消费前 current 不能被抢占切走。 */
	preempt_disable();
	raw_spin_unlock_irq(&rtm->wait_lock);
	rt_mutex_wake_up_q(&wqh);
}

/*
 * rwbase_read_unlock() - 释放一个共享读持有，并在计数归零时交棒给 writer。
 *
 * 调用位置：rwsem up_read() 与 RT rwlock read_unlock() 的公共状态转换。@rwb 是非空、已由调用者持有
 * 一份读锁的借用输入输出对象；@state 只在最后 reader 需唤醒 writer 时使用，选择普通或 RT-lock
 * 唤醒类型。入口不要求 wait_lock，快路径不睡眠；最后 reader 会进入不可普通睡眠的内部唤醒段。
 * 返回无直接返回值；readers 减一，以 full-order 原子操作提供至少 RELEASE，之后调用者不再持读锁。
 * 只有结果为 0 才存在等待旧 readers 清空的 writer，并调用 __rwbase_read_unlock()。函数不释放 rwb
 * 内存或引用，外层调用者继续完成 lockdep/RCU 等包装层退出。
 */
static __always_inline void rwbase_read_unlock(struct rwbase_rt *rwb,
					       unsigned int state)
{
	if (trace_contended_release_enabled() && rt_mutex_owner(&rwb->rtmutex))
		trace_call__contended_release(rwb);
	/*
	 * rwb->readers can only hit 0 when a writer is waiting for the
	 * active readers to leave the critical section.
	 *
	 * dec_and_test() is fully ordered, provides RELEASE.
	 */
	/*
	 * readers 只有在 writer 已减去 READER_BIAS、关闭新读快路径后才可能从活跃读者数减到 0；没有
	 * writer 时它始终保持负偏置。dec_and_test() 是全序原子操作并提供 RELEASE，把读临界区写入发布
	 * 给 writer 随后的 acquire 读取；仅最后 reader 承担唤醒责任。
	 */
	if (unlikely(atomic_dec_and_test(&rwb->readers)))
		__rwbase_read_unlock(rwb, state);
}

/*
 * __rwbase_write_unlock() - 在已持 wait_lock 时恢复 reader bias，并释放 writer 的底层 rtmutex。
 *
 * 调用位置：rwbase_write_unlock()、rwbase_write_downgrade() 及写获取/trylock 的失败回滚公共尾部。
 * @rwb 是非空借用的输入输出对象；@bias 描述当前 readers 中需撤销的写状态：WRITER_BIAS 表示正常
 * 写释放，WRITER_BIAS-1 表示降级后保留 current 为一个 reader，0 表示尚未写入 writer 标志的失败
 * 回滚；@flags 是调用者取得 wait_lock 前保存的 IRQ 状态，作为输入交给 irqrestore。入口必须由
 * current 持有 rtm 和 rtm->wait_lock，函数不可睡眠到 wait_lock 释放之前。
 *
 * 原子 release 更新把 readers 变为 READER_BIAS（降级时为 READER_BIAS+1），重新开放读快路径；随后
 * 恢复 IRQ 并释放 rtmutex，可能唤醒其 waiter。返回无直接值，出口不持两把内部锁；正常/失败调用者
 * 均已完成相应 writer ownership 的释放，本函数不管理 rwb 生命周期。
 */
static inline void __rwbase_write_unlock(struct rwbase_rt *rwb, int bias,
					 unsigned long flags)
	__releases(&rwb->rtmutex.wait_lock)
{
	struct rt_mutex_base *rtm = &rwb->rtmutex;

	/*
	 * _release() is needed in case that reader is in fast path, pairing
	 * with atomic_try_cmpxchg_acquire() in rwbase_read_trylock().
	 */
	/*
	 * release 必须与 rwbase_read_trylock() 中成功 CAS 的 acquire 配对：新 reader 一旦观察到恢复后的
	 * READER_BIAS，就必须同时观察到 writer 临界区内的全部写入。仅依赖后面的 rtmutex 解锁不够，
	 * 因为 reader 快路径根本不取得该 rtmutex。
	 */
	(void)atomic_add_return_release(READER_BIAS - bias, &rwb->readers);
	raw_spin_unlock_irqrestore(&rtm->wait_lock, flags);
	rwbase_rtmutex_unlock(rtm);
}

/*
 * rwbase_write_unlock() - 释放完整写持有并重新开放 reader 快路径。
 *
 * 调用位置：rwsem up_write() 与 RT rwlock write_unlock() 的公共核心。@rwb 是非空、由 current 持有
 * 写锁的借用输入输出对象；入口不持 wait_lock，但 current 必须持其 rtmutex 且 readers 为
 * WRITER_BIAS。函数不普通睡眠，flags 保存本地 IRQ 状态。它在 wait_lock 下按需记录争用释放事件，
 * 再由 __rwbase_write_unlock() 把 readers 恢复为 READER_BIAS、释放 wait_lock 和 rtmutex。返回无
 * 直接值；出口 current 不再持写锁，等待者可继续，调用者下一步完成外层 lockdep/RCU 清理。
 */
static inline void rwbase_write_unlock(struct rwbase_rt *rwb)
{
	struct rt_mutex_base *rtm = &rwb->rtmutex;
	unsigned long flags;

	/* wait_lock 将 trace 的 waiter 快照与 bias 恢复、rtmutex owner 释放组成同一 writer 提交阶段。 */
	raw_spin_lock_irqsave(&rtm->wait_lock, flags);
	if (trace_contended_release_enabled() && rt_mutex_has_waiters(rtm))
		trace_call__contended_release(rwb);
	__rwbase_write_unlock(rwb, WRITER_BIAS, flags);
}

/*
 * rwbase_write_downgrade() - 原子地把 current 的独占写持有转换为一个共享读持有。
 *
 * 调用位置：RT rwsem downgrade_write() 的公共状态转换；RT rwlock 当前没有对外降级包装。@rwb 是
 * 非空、由 current 持有写锁的借用输入输出对象；入口 readers 为 WRITER_BIAS 且 current 持有底层
 * rtmutex，不持 wait_lock。函数不普通睡眠，flags 保存 IRQ 状态。它在 wait_lock 下记录可能的争用
 * 释放，再恢复 reader bias 的同时额外保留计数 1，最后释放 rtmutex。返回无直接值；出口 current
 * 持有一份读锁而不再持写锁，其他 readers 可立即从 acquire 快路径加入，等待 writers 也只能继续
 * 等待这份读持有释放。对象 ownership 不变，调用者下一步按读锁规则配对解锁。
 */
static inline void rwbase_write_downgrade(struct rwbase_rt *rwb)
{
	struct rt_mutex_base *rtm = &rwb->rtmutex;
	unsigned long flags;

	raw_spin_lock_irqsave(&rtm->wait_lock, flags);
	if (trace_contended_release_enabled() && rt_mutex_has_waiters(rtm))
		trace_call__contended_release(rwb);
	/* Release it and account current as reader */
	/* 释放写持有并把 current 计作一个 reader；release 更新也发布此前写临界区内的修改。 */
	__rwbase_write_unlock(rwb, WRITER_BIAS - 1, flags);
}

/*
 * __rwbase_write_trylock() - 在 wait_lock 串行化下检查旧 readers 是否清空并标记写持有。
 *
 * 调用位置：rwbase_write_lock() 的等待循环及 rwbase_write_trylock() 的一次性检查。@rwb 是非空借用
 * 的输入输出对象；调用前 writer 已持底层 rtmutex、已移除 READER_BIAS，并且必须持
 * rwb->rtmutex.wait_lock。函数不睡眠。readers==0 表示关闭读入口前进入的所有 readers 已离开；成功
 * 时写入 WRITER_BIAS，返回 true，current 正式获得独占写状态；非零时返回 false，状态不变，调用者
 * 选择睡眠等待或回滚。acquire 读取与最后 reader 的 release 递减配对，使 writer 观察到读临界区的
 * 写入；wait_lock 已串行化所有 writer，故无需 CAS。无引用或对象 ownership 转移。
 */
static inline bool __rwbase_write_trylock(struct rwbase_rt *rwb)
{
	/* Can do without CAS because we're serialized by wait_lock. */
	/* 已由 wait_lock 串行化 writer 检查与标记，所以普通 atomic_set 足够，无需再用 CAS 仲裁。 */
	lockdep_assert_held(&rwb->rtmutex.wait_lock);

	/*
	 * _acquire is needed in case the reader is in the fast path, pairing
	 * with rwbase_read_unlock(), provides ACQUIRE.
	 */
	/*
	 * acquire 读取与读快路径在 rwbase_read_unlock() 中的 release 递减配对；看到 0 不仅表示计数
	 * 清空，也保证先前 reader 临界区的访问已经完成并对 writer 可见。
	 */
	if (!atomic_read_acquire(&rwb->readers)) {
		atomic_set(&rwb->readers, WRITER_BIAS);
		return 1;
	}

	return 0;
}

/*
 * rwbase_write_lock() - 阻塞取得 rwbase 的独占写持有，并在需要时等待读者全部离开。
 *
 * 调用位置：RT rwsem down_write*() 与 RT rwlock write_lock() 的公共核心。@rwb 是非空、已初始化的
 * 借用输入输出对象；@state 是底层 rtmutex 获取及等待读者时的任务状态，rwsem 可选择不可中断或
 * killable，RT rwlock 适配为不可由信号打断的 TASK_RTLOCK_WAIT。入口不持内部锁，current 不得已有
 * 冲突持有；函数可能睡眠。rtm 是内嵌 rtmutex 的借用别名，flags 保存每轮 wait_lock 前 IRQ 状态。
 *
 * 阶段一取得 rtmutex，在 writers 之间建立 PI/DL 排序；失败直接返回 -EINTR 且尚未碰 reader bias。
 * 阶段二减去 READER_BIAS，关闭新 reader 快路径，此刻 readers 的非负值就是仍活跃的旧 readers。
 * 阶段三在 wait_lock 下以 acquire 检查 0；未清空则保存任务状态、记录争用并循环“检查信号→重试
 * →放锁调度→重新设等待态”。成功把 0 标为 WRITER_BIAS，恢复任务状态并返回 0，current 同时持有
 * rtmutex 与独占写状态；信号失败则恢复状态、加回 bias、释放 wait_lock/rtmutex 并返回 -EINTR。
 * 函数不取得对象引用；成功调用者进入写临界区并以 rwbase_write_unlock()/downgrade() 配对。
 */
static int __sched rwbase_write_lock(struct rwbase_rt *rwb,
				     unsigned int state)
{
	struct rt_mutex_base *rtm = &rwb->rtmutex;
	unsigned long flags;

	/* Take the rtmutex as a first step */
	/* 先取得 rtmutex，序列化所有 writer 并让后续 rtmutex waiter 能向当前 writer 传播 PI/DL 提升。 */
	if (rwbase_rtmutex_lock_state(rtm, state))
		return -EINTR;

	/* Force readers into slow path */
	/*
	 * 原子减去 READER_BIAS 关闭新读快路径；并发 reader 的 CAS 要么先完成并计入剩余值，要么在看到
	 * 非负值后失败转慢路径，因此不会漏算已获读锁者。
	 */
	atomic_sub(READER_BIAS, &rwb->readers);

	/* 在改变任务状态和可能调度前提交 worker/I/O 工作并设置 sched_rt_mutex；所有出口都由 post 配对。 */
	rwbase_pre_schedule();

	/* wait_lock 保护 writer 对 readers==0 的检查、睡眠准备及信号回滚状态。 */
	raw_spin_lock_irqsave(&rtm->wait_lock, flags);
	/* 无旧 reader 时直接写入 WRITER_BIAS，跳过 trace、状态保存与调度循环。 */
	if (__rwbase_write_trylock(rwb))
		goto out_unlock;

	/* 旧 readers 尚在：先发布可唤醒的等待状态，再开始一次完整的争用区间。 */
	rwbase_set_and_save_current_state(state);
	trace_contention_begin(rwb, LCB_F_RT | LCB_F_WRITE);
	for (;;) {
		/* Optimized out for rwlocks */
		/* RT rwlock 的适配器恒为 false；可中断 rwsem 则在每次睡眠前检查信号并完整回滚。 */
		if (rwbase_signal_pending_state(state, current)) {
			/*
			 * 此时仍持 wait_lock 和 rtmutex，readers 仍是去掉 bias 后的活跃数。先恢复任务状态，再以
			 * bias=0 加回 READER_BIAS 并释放两锁，确保调用者收到 -EINTR 时锁状态已回到可读。
			 */
			rwbase_restore_current_state();
			__rwbase_write_unlock(rwb, 0, flags);
			rwbase_post_schedule();
			trace_contention_end(rwb, -EINTR);
			return -EINTR;
		}

		if (__rwbase_write_trylock(rwb))
			break;

		/* 条件未满足时必须放开 wait_lock，最后 reader 才能取得它并把 current 加入唤醒队列。 */
		raw_spin_unlock_irqrestore(&rtm->wait_lock, flags);
		rwbase_schedule();
		raw_spin_lock_irqsave(&rtm->wait_lock, flags);

		/* 唤醒可能是伪唤醒；重新发布等待状态后回到循环顶部，在持锁条件下复查信号和 readers。 */
		set_current_state(state);
	}
	/* 成功标记 WRITER_BIAS 后 current 不再等待；trace 以 0 结束，但 rtmutex 保留到写解锁。 */
	rwbase_restore_current_state();
	trace_contention_end(rwb, 0);

out_unlock:
	/* 快慢成功路径均在此仅释放内部 wait_lock；writer 的 rtmutex ownership 是对外写持有的一部分。 */
	raw_spin_unlock_irqrestore(&rtm->wait_lock, flags);
	rwbase_post_schedule();
	return 0;
}

/*
 * rwbase_write_trylock() - 不排队、不睡眠地尝试取得独占写持有。
 *
 * 调用位置：RT rwsem down_write_trylock() 与 RT rwlock write_trylock() 的公共核心。@rwb 是非空、已
 * 初始化的借用输入输出对象；入口不持内部锁，函数不睡眠。rtm 是内嵌 rtmutex 的借用别名，flags
 * 保存 IRQ 状态。阶段一尝试独占底层 rtmutex；失败返回 0 且所有状态不变。阶段二成功后移除
 * READER_BIAS、在 wait_lock 下检查是否没有活跃 readers：若为 0，写入 WRITER_BIAS 并返回 1，
 * current 持有 rtmutex 和写锁；若仍有 readers，则以 bias=0 恢复 READER_BIAS、释放 wait_lock 与
 * rtmutex，返回 0。失败不会排队等待旧 readers，也不留下 ownership；成功者下一步进入写临界区并
 * 必须调用 rwbase_write_unlock()，函数不取得 rwb 引用。
 */
static inline int rwbase_write_trylock(struct rwbase_rt *rwb)
{
	struct rt_mutex_base *rtm = &rwb->rtmutex;
	unsigned long flags;

	/* writer 之间先由 rtmutex 仲裁；失败不能关闭 reader 快路径，否则 trylock 会产生可见副作用。 */
	if (!rwbase_rtmutex_trylock(rtm))
		return 0;

	/* 已独占 writer 通道后移除 bias；并发读 CAS 要么已计入，要么观察非负值并失败。 */
	atomic_sub(READER_BIAS, &rwb->readers);

	raw_spin_lock_irqsave(&rtm->wait_lock, flags);
	/* trylock 只接受立即清空的 reader 集合；成功保留 rtmutex，作为写持有的一部分。 */
	if (__rwbase_write_trylock(rwb)) {
		raw_spin_unlock_irqrestore(&rtm->wait_lock, flags);
		return 1;
	}
	/* 活跃 readers 尚未离开：完整撤销 bias 与 rtmutex，向调用者提供“失败等同未尝试”的保证。 */
	__rwbase_write_unlock(rwb, 0, flags);
	return 0;
}
