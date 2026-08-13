/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef WW_RT

/* 普通 mutex 实例化：后续公共模板用这些别名访问链表 waiter 与 wait_lock。 */
#define MUTEX		mutex
#define MUTEX_WAITER	mutex_waiter
#define WAIT_LOCK	wait_lock

/*
 *           +--------+
 *           | first  |
 *           +--------+
 *                |
 *                v
 *  +----+     +----+     +----+
 *  | W3 | <-> | W1 | <-> | W2 |
 *  +----+     +----+     +----+
 *    ^                     ^
 *    +---------------------+
 */
/*
 * 普通 mutex 的 waiter 是以 first 为入口的环形双向链：示例物理链接为 W3↔W1↔W2，逻辑正向遍历
 * 从 W1 开始得到 W1、W2、W3，反向则得到 W3、W2、W1；重新遇到 first 即终止而非继续环绕。
 */

/*
 * __ww_waiter_first - 取得普通 mutex 的逻辑首 waiter
 * @lock: 调用者持有 wait_lock 的 mutex
 *
 * 返回 first_waiter 的借用指针，空队列返回 NULL；不修改链表，指针只在 wait_lock 保护下稳定。
 */
static inline struct mutex_waiter *
__ww_waiter_first(struct mutex *lock)
	__must_hold(&lock->wait_lock)
{
	return lock->first_waiter;
}

/*
 * for (cur = __ww_waiter_first(); cur; cur = __ww_waiter_next())
 *
 * Should iterate like: W1, W2, W3
 */
/* 正向循环应写成 first/next 组合，并按 W1、W2、W3 顺序迭代。 */
/*
 * __ww_waiter_next - 取得环形普通 waiter 的下一个逻辑节点
 * @lock: 调用者持有 wait_lock 的 mutex
 * @w: 当前已观察且仍在 @lock 链中的节点
 *
 * 先沿 list.next 前进；若重新到达 first，说明一圈已完成，返回 NULL，否则返回下一借用节点。
 */
static inline struct mutex_waiter *
__ww_waiter_next(struct mutex *lock, struct mutex_waiter *w)
	__must_hold(&lock->wait_lock)
{
	w = list_next_entry(w, list);
	/*
	 * Terminate if the next entry is the first again, that has already
	 * been observed.
	 */
	/* 下一项若再次是 first，说明它已经观察过，正向迭代应在此终止。 */
	if (lock->first_waiter == w)
		return NULL;

	return w;
}

/*
 * for (cur = __ww_waiter_last(); cur; cur = __ww_waiter_prev())
 *
 * Should iterate like: W3, W2, W1
 */
/* 反向循环应写成 last/prev 组合，并按 W3、W2、W1 顺序迭代。 */
/*
 * __ww_waiter_prev - 取得环形普通 waiter 的前一个逻辑节点
 * @lock: 调用者持有 wait_lock 的 mutex
 * @w: 当前已观察且仍在 @lock 链中的节点
 *
 * 当前已是 first 时返回 NULL，避免绕到已观察的 last；否则返回 list.prev 的借用节点。
 */
static inline struct mutex_waiter *
__ww_waiter_prev(struct mutex *lock, struct mutex_waiter *w)
	__must_hold(&lock->wait_lock)
{
	/*
	 * Terminate at the first entry, the previous entry of first is the
	 * last and that has already been observed.
	 */
	/* 到达 first 即终止；first 的物理前驱是已经观察过的 last。 */
	if (lock->first_waiter == w)
		return NULL;

	return list_prev_entry(w, list);
}

/*
 * __ww_waiter_last - 取得普通 mutex 的逻辑末 waiter
 * @lock: 调用者持有 wait_lock 的 mutex
 *
 * 空队列返回 NULL；否则返回 first 的物理前驱，即环形链逻辑末项。返回指针只在 wait_lock 下稳定。
 */
static inline struct mutex_waiter *
__ww_waiter_last(struct mutex *lock)
	__must_hold(&lock->wait_lock)
{
	struct mutex_waiter *w = lock->first_waiter;

	if (w)
		w = list_prev_entry(w, list);
	return w;
}

/*
 * __ww_waiter_add - 把公共 ww 插入位置适配到普通 mutex 队列
 * @lock: 调用者持有 wait_lock 的 mutex
 * @waiter: 当前栈帧拥有、尚未发布的 waiter
 * @pos: 可选插入位置；NULL 表示 FIFO 队尾
 *
 * 委托 __mutex_add_waiter() 完成环形链接、first 与 WAITERS 标志发布；无返回值，节点随后必须由
 * 普通 mutex 慢路径退队。
 */
static inline void
__ww_waiter_add(struct mutex *lock, struct mutex_waiter *waiter, struct mutex_waiter *pos)
	__must_hold(&lock->wait_lock)
{
	__mutex_add_waiter(lock, waiter, pos);
}

/*
 * __ww_mutex_owner - 取得普通 mutex owner 的无引用快照
 * @lock: 保持存活的 mutex
 *
 * 返回当前 task 指针或 NULL；只用于 wait_lock 协议下的比较/唤醒决策，不增加 task 引用。
 */
static inline struct task_struct *
__ww_mutex_owner(struct mutex *lock)
{
	return __mutex_owner(lock);
}

/*
 * __ww_mutex_has_waiters - 从普通 mutex owner 字读取 WAITERS 快照
 * @lock: 保持存活的 mutex
 *
 * 返回 WAITERS 位是否非零；无锁读取可能立即过期，fastpath 依靠后续全屏障和必要时取得 wait_lock
 * 避免漏过需要 die/wound 的 waiter。
 */
static inline bool
__ww_mutex_has_waiters(struct mutex *lock)
{
	return atomic_long_read(&lock->owner) & MUTEX_FLAG_WAITERS;
}

/*
 * lock_wait_lock - 保存 IRQ 状态并取得普通 mutex wait_lock
 * @lock: 保持存活的 mutex
 * @flags: 调用者栈上的输出槽，保存本地 IRQ 状态供配对解锁
 *
 * 返回时 raw spinlock 已持有且本地 IRQ 关闭；不可睡眠，必须用 unlock_wait_lock() 和同一 flags 配对。
 */
static inline void lock_wait_lock(struct mutex *lock, unsigned long *flags)
	__acquires(&lock->wait_lock)
{
	raw_spin_lock_irqsave(&lock->wait_lock, *flags);
}

/*
 * unlock_wait_lock - 释放普通 mutex wait_lock 并恢复 IRQ 状态
 * @lock: current CPU 正持有其 wait_lock 的 mutex
 * @flags: 配对 lock_wait_lock() 写入且尚未消费的 IRQ 状态
 *
 * 返回时内部锁已释放、IRQ 恢复；函数不唤醒 wake_q，调用者在保护区外另行执行唤醒。
 */
static inline void unlock_wait_lock(struct mutex *lock, unsigned long *flags)
	__releases(&lock->wait_lock)
{
	raw_spin_unlock_irqrestore(&lock->wait_lock, *flags);
}

/*
 * lockdep_assert_wait_lock_held - 断言普通 mutex wait_lock 已持有
 * @lock: 正在执行 ww 队列操作的 mutex
 *
 * 仅作 lockdep 诊断，不取得或释放锁；返回后实际锁状态不变。
 */
static inline void lockdep_assert_wait_lock_held(struct mutex *lock)
	__must_hold(&lock->wait_lock)
{
	lockdep_assert_held(&lock->wait_lock);
}

#else /* WW_RT */

/* RT 实例化：公共模板改用 rtmutex 红黑树 waiter 以及嵌套的 wait_lock。 */
#define MUTEX		rt_mutex
#define MUTEX_WAITER	rt_mutex_waiter
#define WAIT_LOCK	rtmutex.wait_lock

/*
 * __ww_waiter_first - 取得 RT waiter 红黑树的首节点
 * @lock: 调用者持有 rtmutex.wait_lock 的 RT mutex
 *
 * 空树返回 NULL，否则把最左 rb_node 转回永久有效到退队的 rt_mutex_waiter 借用指针。
 */
static inline struct rt_mutex_waiter *
__ww_waiter_first(struct rt_mutex *lock)
	__must_hold(&lock->rtmutex.wait_lock)
{
	struct rb_node *n = rb_first(&lock->rtmutex.waiters.rb_root);
	if (!n)
		return NULL;
	return rb_entry(n, struct rt_mutex_waiter, tree.entry);
}

/*
 * __ww_waiter_next - 取得 RT waiter 红黑树的中序后继
 * @lock: 包含 @w 的 RT mutex；参数用于模板统一，函数不直接访问
 * @w: 仍在 waiter 树中的当前节点
 *
 * 没有后继返回 NULL，否则返回 rb_next 对应的借用 waiter；调用者必须持有 rtmutex.wait_lock。
 */
static inline struct rt_mutex_waiter *
__ww_waiter_next(struct rt_mutex *lock, struct rt_mutex_waiter *w)
{
	struct rb_node *n = rb_next(&w->tree.entry);
	if (!n)
		return NULL;
	return rb_entry(n, struct rt_mutex_waiter, tree.entry);
}

/*
 * __ww_waiter_prev - 取得 RT waiter 红黑树的中序前驱
 * @lock: 包含 @w 的 RT mutex；参数用于模板统一
 * @w: 仍在 waiter 树中的当前节点
 *
 * 没有前驱返回 NULL，否则返回 rb_prev 对应的借用 waiter；调用者必须持有 rtmutex.wait_lock。
 */
static inline struct rt_mutex_waiter *
__ww_waiter_prev(struct rt_mutex *lock, struct rt_mutex_waiter *w)
{
	struct rb_node *n = rb_prev(&w->tree.entry);
	if (!n)
		return NULL;
	return rb_entry(n, struct rt_mutex_waiter, tree.entry);
}

/*
 * __ww_waiter_last - 取得 RT waiter 红黑树的末节点
 * @lock: 调用者持有 rtmutex.wait_lock 的 RT mutex
 *
 * 空树返回 NULL，否则把最右 rb_node 转为借用 rt_mutex_waiter。
 */
static inline struct rt_mutex_waiter *
__ww_waiter_last(struct rt_mutex *lock)
	__must_hold(&lock->rtmutex.wait_lock)
{
	struct rb_node *n = rb_last(&lock->rtmutex.waiters.rb_root);
	if (!n)
		return NULL;
	return rb_entry(n, struct rt_mutex_waiter, tree.entry);
}

/*
 * __ww_waiter_add - RT 后端的公共插入适配空操作
 * @lock: 已由 RT 慢路径管理 waiter 树的 mutex
 * @waiter: RT 路径在进入公共 ww 检查前已插入的节点
 * @pos: 公共 stamp 扫描得到的位置，RT 后端不使用
 *
 * RT 无条件先入队，错误时再由外层移除，因此这里不能重复插入；无返回值，实参仍按 C 规则求值。
 */
static inline void
__ww_waiter_add(struct rt_mutex *lock, struct rt_mutex_waiter *waiter, struct rt_mutex_waiter *pos)
{
	/* RT unconditionally adds the waiter first and then removes it on error */
	/* RT 总是先加入 waiter，发生错误后再移除。 */
}

/*
 * __ww_mutex_owner - 取得 RT mutex 当前 owner 快照
 * @lock: 保持存活的 RT mutex
 *
 * 返回 task 借用指针或 NULL；公共 wound 路径在 rtmutex.wait_lock 下使用，不增加 task 引用。
 */
static inline struct task_struct *
__ww_mutex_owner(struct rt_mutex *lock)
{
	return rt_mutex_owner(&lock->rtmutex);
}

/*
 * __ww_mutex_has_waiters - 在 RT wait_lock 下查询 waiter 树
 * @lock: 调用者持有 rtmutex.wait_lock 的 RT mutex
 *
 * 返回红黑树是否含 waiter；与普通后端的原子标志快照不同，此结果由内部锁稳定。
 */
static inline bool
__ww_mutex_has_waiters(struct rt_mutex *lock)
	__must_hold(&lock->rtmutex.wait_lock)
{
	return rt_mutex_has_waiters(&lock->rtmutex);
}

/*
 * lock_wait_lock - 保存 IRQ 状态并取得 RT mutex 内部 wait_lock
 * @lock: 保持存活的 RT mutex
 * @flags: 调用者栈上的 IRQ 状态输出槽
 *
 * 返回时 raw spinlock 已持有且 IRQ 关闭；必须用 RT 侧 unlock_wait_lock() 成对恢复。
 */
static inline void lock_wait_lock(struct rt_mutex *lock, unsigned long *flags)
	__acquires(&lock->rtmutex.wait_lock)
{
	raw_spin_lock_irqsave(&lock->rtmutex.wait_lock, *flags);
}

/*
 * unlock_wait_lock - 释放 RT mutex 内部 wait_lock 并恢复 IRQ
 * @lock: current CPU 正持有其 rtmutex.wait_lock 的对象
 * @flags: 配对获取写入的 IRQ 状态
 *
 * 返回时内部锁与 IRQ 状态均恢复；wake_q 由公共调用者在锁外处理。
 */
static inline void unlock_wait_lock(struct rt_mutex *lock, unsigned long *flags)
	__releases(&lock->rtmutex.wait_lock)
{
	raw_spin_unlock_irqrestore(&lock->rtmutex.wait_lock, *flags);
}

/*
 * lockdep_assert_wait_lock_held - 断言 RT mutex wait_lock 已持有
 * @lock: 正在执行公共 ww 队列算法的 RT mutex
 *
 * 只作 lockdep 检查，不改变运行时锁状态。
 */
static inline void lockdep_assert_wait_lock_held(struct rt_mutex *lock)
	__must_hold(&lock->rtmutex.wait_lock)
{
	lockdep_assert_held(&lock->rtmutex.wait_lock);
}

#endif /* WW_RT */

/*
 * Wait-Die:
 *   The newer transactions are killed when:
 *     It (the new transaction) makes a request for a lock being held
 *     by an older transaction.
 *
 * Wound-Wait:
 *   The newer transactions are wounded when:
 *     An older transaction makes a request for a lock being held by
 *     the newer transaction.
 */
/*
 * Wait-Die：较新的事务请求由较老事务持有的锁时，较新者立即退出。
 * Wound-Wait：较老事务请求由较新事务持有的锁时，较新持有者被 wound，并在检查点自行退出。
 * 两者都用全局 stamp 建立无环年龄序；同一 ww_class 只能选择其中一种，不能在同一等待队列混用。
 */

/*
 * Associate the ww_mutex @ww with the context @ww_ctx under which we acquired
 * it.
 */
/* 把刚取得的 ww_mutex @ww 与本次 acquire context @ww_ctx 关联。 */
/*
 * ww_mutex_lock_acquired - 发布成功获取的 context 并增加持锁计数
 * @ww: current 刚取得且保持存活的 ww_mutex
 * @ww_ctx: 本次事务的有效 context
 *
 * DEBUG_WW_MUTEXES 下诊断普通 mutex_unlock 混用、done 后继续获取、-EDEADLK 回退顺序和 class
 * 不匹配；随后 acquired++ 并把 ww->ctx 指向 context。无返回值；context 必须活到该锁经
 * ww_mutex_unlock() 清关联，调用者仍负责 fastpath/waiter 屏障与唤醒。
 */
static __always_inline void
ww_mutex_lock_acquired(struct ww_mutex *ww, struct ww_acquire_ctx *ww_ctx)
{
#ifdef DEBUG_WW_MUTEXES
	/*
	 * If this WARN_ON triggers, you used ww_mutex_lock to acquire,
	 * but released with a normal mutex_unlock in this call.
	 *
	 * This should never happen, always use ww_mutex_unlock.
	 */
		/* 若触发，说明上次用 ww API 获取却以普通 mutex_unlock 释放；必须始终使用 ww_mutex_unlock。 */
	DEBUG_LOCKS_WARN_ON(ww->ctx);

	/*
	 * Not quite done after calling ww_acquire_done() ?
	 */
		/* 调用 ww_acquire_done() 后不得再继续获取。 */
	DEBUG_LOCKS_WARN_ON(ww_ctx->done_acquire);

	if (ww_ctx->contending_lock) {
		/*
		 * After -EDEADLK you tried to
		 * acquire a different ww_mutex? Bad!
		 */
			/* 收到 -EDEADLK 后只能先慢取记录的 contending_lock，不能改取另一把 ww_mutex。 */
		DEBUG_LOCKS_WARN_ON(ww_ctx->contending_lock != ww);

		/*
		 * You called ww_mutex_lock after receiving -EDEADLK,
		 * but 'forgot' to unlock everything else first?
		 */
			/* -EDEADLK 后重试前必须先释放 context 下其余全部锁。 */
		DEBUG_LOCKS_WARN_ON(ww_ctx->acquired > 0);
		ww_ctx->contending_lock = NULL;
	}

	/*
	 * Naughty, using a different class will lead to undefined behavior!
	 */
		/* context 与 mutex 必须属于同一 ww_class，混用会产生未定义的死锁行为。 */
	DEBUG_LOCKS_WARN_ON(ww_ctx->ww_class != ww->ww_class);
#endif
	ww_ctx->acquired++;
	ww->ctx = ww_ctx;
}

/*
 * Determine if @a is 'less' than @b. IOW, either @a is a lower priority task
 * or, when of equal priority, a younger transaction than @b.
 *
 * Depending on the algorithm, @a will either need to wait for @b, or die.
 */
/*
 * 判断 @a 是否比 @b “弱”：即任务优先级更低，或优先级相同但事务更年轻。依据算法，@a 随后等待
 * @b 或自行退出。
 *
 * __ww_ctx_less - 比较两个 ww context 的调度重要性与年龄
 * @a: 待判断的有效 acquire context
 * @b: 比较基准 context
 *
 * RT 实例先比较动态 RT/DL 优先级，同优先级 DL 再比较 deadline；仍相等或普通后端时，以有符号
 * stamp 差处理回绕，stamp 更大即更年轻。返回 true 表示 @a 较弱，false 表示不较弱；不改状态。
 */
static inline bool
__ww_ctx_less(struct ww_acquire_ctx *a, struct ww_acquire_ctx *b)
{
/*
 * Can only do the RT prio for WW_RT, because task->prio isn't stable due to PI,
 * so the wait_list ordering will go wobbly. rt_mutex re-queues the waiter and
 * isn't affected by this.
 */
/*
 * 只有 WW_RT 能比较 task->prio：普通 waiter 链不会因 PI 变化重排，顺序会失真；rt_mutex 会重新
 * 排队 waiter，所以不受此问题影响。
 */
#ifdef WW_RT
	/* kernel prio; less is more */
	/* 内核优先级数值越小越高。 */
	int a_prio = a->task->prio;
	int b_prio = b->task->prio;

	if (rt_or_dl_prio(a_prio) || rt_or_dl_prio(b_prio)) {

		if (a_prio > b_prio)
			return true;

		if (a_prio < b_prio)
			return false;

		/* equal static prio */
			/* 静态优先级相同，DL 任务继续比较 deadline。 */

		if (dl_prio(a_prio)) {
			if (dl_time_before(b->task->dl.deadline,
					   a->task->dl.deadline))
				return true;

			if (dl_time_before(a->task->dl.deadline,
					   b->task->dl.deadline))
				return false;
		}

		/* equal prio */
			/* 调度优先级仍相同，退回 stamp 打破平局。 */
	}
#endif

	/* FIFO order tie break -- bigger is younger */
	/* FIFO 年龄打破平局：stamp 更大表示更年轻；有符号差支持自然回绕窗口。 */
	return (signed long)(a->stamp - b->stamp) > 0;
}

/*
 * Wait-Die; wake a lesser waiter context (when locks held) such that it can
 * die.
 *
 * Among waiters with context, only the first one can have other locks acquired
 * already (ctx->acquired > 0), because __ww_mutex_add_waiter() and
 * __ww_mutex_check_kill() wake any but the earliest context.
 */
/*
 * Wait-Die 下，唤醒一个较弱且已经持锁的 waiter，让它运行并自行 die。带 context 的 waiter 中只有
 * 最前者可能还持有其他锁，因为 add/check_kill 会唤醒除最早者外的这类 context。
 *
 * __ww_mutex_die - 对 Wait-Die 队列中的较年轻持锁 waiter 安排退出
 * @lock: 调用者持有模板 WAIT_LOCK 的 mutex/rt_mutex
 * @waiter: 正在扫描、仍在队列且带有效 ww_ctx 的 waiter
 * @ww_ctx: 刚取得或正在插入的比较基准 context
 * @wake_q: 受保护区外执行的唤醒队列
 *
 * 非 Wait-Die 返回 false。Wait-Die 下若 waiter 已持其他锁且比 @ww_ctx 弱，清其 blocked_on 为
 * PROXY_WAKING 并加入 wake_q；无论是否实际唤醒都返回 true，通知扫描者本算法分支已决定停止。
 */
static bool
__ww_mutex_die(struct MUTEX *lock, struct MUTEX_WAITER *waiter,
	       struct ww_acquire_ctx *ww_ctx, struct wake_q_head *wake_q)
{
	if (!ww_ctx->is_wait_die)
		return false;

	if (waiter->ww_ctx->acquired > 0 && __ww_ctx_less(waiter->ww_ctx, ww_ctx)) {
#ifndef WW_RT
		debug_mutex_wake_waiter(lock, waiter);
#endif
		/*
		 * When waking up the task to die, be sure to set the
		 * blocked_on to PROXY_WAKING. Otherwise we can see
		 * circular blocked_on relationships that can't resolve.
		 */
			/* 唤醒其退出前先转成 PROXY_WAKING，避免留下无法消解的环形 blocked_on 关系。 */
		clear_task_blocked_on(waiter->task, lock);
		wake_q_add(wake_q, waiter->task);
	}

	return true;
}

/*
 * Wound-Wait; wound a lesser @hold_ctx if it holds the lock.
 *
 * Wound the lock holder if there are waiters with more important transactions
 * than the lock holders. Even if multiple waiters may wound the lock holder,
 * it's sufficient that only one does.
 */
/*
 * Wound-Wait 下，若 @hold_ctx 较弱且确实持有锁就 wound 它；多个更重要 waiter 可能同时满足条件，
 * 但只需一个成功置位。
 *
 * __ww_mutex_wound - 标记较年轻 owner context 并安排其重新运行
 * @lock: 调用者持有模板 WAIT_LOCK 的 mutex/rt_mutex
 * @ww_ctx: 发起 wound 的有效请求 context
 * @hold_ctx: 当前 owner 的 context 快照，竞态窗口内可为 NULL
 * @wake_q: 延迟到内部锁外执行的唤醒队列
 *
 * 无 context/owner、请求者未持其他锁或 owner 不较弱时返回 false。成功置 wounded，owner 不是
 * current 时清其 blocked_on 并加入 wake_q，返回 true；owner 指针在 WAITERS+wait_lock 下稳定。
 */
static bool __ww_mutex_wound(struct MUTEX *lock,
			     struct ww_acquire_ctx *ww_ctx,
			     struct ww_acquire_ctx *hold_ctx,
			     struct wake_q_head *wake_q)
	__must_hold(&lock->WAIT_LOCK)
{
	struct task_struct *owner = __ww_mutex_owner(lock);

	lockdep_assert_wait_lock_held(lock);

	/*
	 * Possible through __ww_mutex_add_waiter() when we race with
	 * ww_mutex_set_context_fastpath(). In that case we'll get here again
	 * through __ww_mutex_check_waiters().
	 */
	/*
	 * add_waiter 可能与 fastpath 发布 ctx 竞争而暂见 NULL；之后 check_waiters 会再次进入，不在此误判。
	 */
	if (!hold_ctx)
		return false;

	/*
	 * Can have !owner because of __mutex_unlock_slowpath(), but if owner,
	 * it cannot go away because we'll have FLAG_WAITERS set and hold
	 * wait_lock.
	 */
	/* unlock 慢路径可能已清 owner；若 owner 非空，WAITERS 与 wait_lock 保证其在本窗口不会消失。 */
	if (!owner)
		return false;

	if (ww_ctx->acquired > 0 && __ww_ctx_less(hold_ctx, ww_ctx)) {
		hold_ctx->wounded = 1;

		/*
		 * wake_up_process() paired with set_current_state()
		 * inserts sufficient barriers to make sure @owner either sees
		 * it's wounded in __ww_mutex_check_kill() or has a
		 * wakeup pending to re-read the wounded state.
		 */
			/*
			 * wake_up_process() 与 set_current_state() 的屏障配对，保证 owner 要么在 check_kill 看见 wounded，
			 * 要么保留一次待处理唤醒并重新读取它。
			 */
		if (owner != current) {
			/*
			 * When waking up the task to wound, be sure to set the
			 * blocked_on to PROXY_WAKING. Otherwise we can see
			 * circular blocked_on relationships that can't resolve.
			 *
			 * NOTE: We pass NULL here instead of lock, because we
			 * are waking the mutex owner, who may be currently
			 * blocked on a different mutex.
			 */
				/*
				 * 唤醒被 wound 的任务前先置 PROXY_WAKING，避免环形 blocked_on。这里传 NULL 而非 @lock，
				 * 因为被唤醒的是本锁 owner，它此刻可能阻塞在另一把 mutex 上。
				 */
			clear_task_blocked_on(owner, NULL);
			wake_q_add(wake_q, owner);
		}
		return true;
	}

	return false;
}

/*
 * We just acquired @lock under @ww_ctx, if there are more important contexts
 * waiting behind us on the wait-list, check if they need to die, or wound us.
 *
 * See __ww_mutex_add_waiter() for the list-order construction; basically the
 * list is ordered by stamp, smallest (oldest) first.
 *
 * This relies on never mixing wait-die/wound-wait on the same wait-list;
 * which is currently ensured by that being a ww_class property.
 *
 * The current task must not be on the wait list.
 */
/*
 * current 刚在 @ww_ctx 下取得锁；若其后的更重要 context 正等待，检查它们应 die 还是 wound current。
 * 队列由 add_waiter 按 stamp 从老到新构造；同一队列绝不能混用两种算法，这由 ww_class 保证。
 * current 本身不得仍在等待链中。
 *
 * __ww_mutex_check_waiters - 获取成功后扫描等待者的死锁动作
 * @lock: 调用者持有模板 WAIT_LOCK、且 current 已取得的锁
 * @ww_ctx: 当前 owner 的有效 context
 * @wake_q: 收集需在内部锁外唤醒的任务
 *
 * 从逻辑首项正向跳过无 context waiter；Wait-Die helper 处理并要求停止，或 Wound-Wait 成功 wound
 * current 时停止。无返回值，不摘队列；调用者随后释放 wait_lock 并执行 wake_q。
 */
static void
__ww_mutex_check_waiters(struct MUTEX *lock, struct ww_acquire_ctx *ww_ctx,
			 struct wake_q_head *wake_q)
	__must_hold(&lock->WAIT_LOCK)
{
	struct MUTEX_WAITER *cur;

	lockdep_assert_wait_lock_held(lock);

	for (cur = __ww_waiter_first(lock); cur;
	     cur = __ww_waiter_next(lock, cur)) {

		if (!cur->ww_ctx)
			continue;

		if (__ww_mutex_die(lock, cur, ww_ctx, wake_q) ||
		    __ww_mutex_wound(lock, cur->ww_ctx, ww_ctx, wake_q))
			break;
	}
}

/*
 * After acquiring lock with fastpath, where we do not hold wait_lock, set ctx
 * and wake up any waiters so they can recheck.
 */
/* fastpath 获取时没有 wait_lock：先发布 ctx，再唤醒可能存在的 waiter 让它们重查。 */
/*
 * ww_mutex_set_context_fastpath - 完成快速获取的 ctx 发布与竞争补检
 * @lock: current 已快速取得且保持存活的 ww_mutex
 * @ctx: 本次获取的有效 context
 *
 * 先登记 acquired/ctx，再以全屏障把 ctx 发布排在 WAITERS 读取前。无 waiter 时直接返回；存在竞态
 * 则取得 wait_lock，运行 check_waiters，把 wake_q 带到锁外唤醒。函数无失败返回，返回时仍持
 * ww_mutex；局部 flags 保存 IRQ 状态，has_waiters 只是竞争快照。
 */
static __always_inline void
ww_mutex_set_context_fastpath(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	DEFINE_WAKE_Q(wake_q);
	unsigned long flags;
	bool has_waiters;

	ww_mutex_lock_acquired(lock, ctx);

	/*
	 * The lock->ctx update should be visible on all cores before
	 * the WAITERS check is done, otherwise contended waiters might be
	 * missed. The contended waiters will either see ww_ctx == NULL
	 * and keep spinning, or it will acquire wait_lock, add itself
	 * to waiter list and sleep.
	 */
	/*
	 * 必须让 lock->ctx 在检查 WAITERS 前对所有 CPU 可见，否则会漏掉竞争者；等待者要么暂见 NULL
	 * 继续自旋，要么取得 wait_lock、入队并睡眠。
	 */
	smp_mb(); /* See comments above and below. */
	/* 全屏障的配对关系见上下两段说明。 */

	/*
	 * [W] ww->ctx = ctx	    [W] MUTEX_FLAG_WAITERS
	 *     MB		        MB
	 * [R] MUTEX_FLAG_WAITERS   [R] ww->ctx
	 *
	 * The memory barrier above pairs with the memory barrier in
	 * __ww_mutex_add_waiter() and makes sure we either observe ww->ctx
	 * and/or !empty list.
	 */
	/*
	 * fastpath 的 [写 ctx→MB→读 WAITERS] 与 add_waiter 的 [写 WAITERS→MB→读 ctx] 配对，保证双方
	 * 至少一方观察到对方发布：要么这里看到非空队列，要么 waiter 看到 ctx 并执行 wound/die。
	 */
	has_waiters = data_race(__ww_mutex_has_waiters(&lock->base));
	if (likely(!has_waiters))
		return;

	/*
	 * Uh oh, we raced in fastpath, check if any of the waiters need to
	 * die or wound us.
	 */
	/* fastpath 与入队竞争时，取得 wait_lock 并补查是否有 waiter 应 die 或 wound 当前 owner。 */
	lock_wait_lock(&lock->base, &flags);
	__ww_mutex_check_waiters(&lock->base, ctx, &wake_q);
	preempt_disable();
	unlock_wait_lock(&lock->base, &flags);
	wake_up_q(&wake_q);
	preempt_enable();
}

/*
 * __ww_mutex_kill - 把已持其他锁的当前 context 转为 -EDEADLK 回退
 * @lock: 当前竞争的模板 mutex，用于调试记录 contending_lock
 * @ww_ctx: current 的有效 acquire context
 *
 * acquired>0 时返回 -EDEADLK；调试构建还断言没有旧 contending_lock，并记录本次锁供 slow retry。
 * acquired==0 返回 0，因为没有其他锁需要回退。函数不摘 waiter、不释放锁，调用者负责错误清理。
 */
static __always_inline int
__ww_mutex_kill(struct MUTEX *lock, struct ww_acquire_ctx *ww_ctx)
{
	if (ww_ctx->acquired > 0) {
#ifdef DEBUG_WW_MUTEXES
		struct ww_mutex *ww;

		ww = container_of(lock, struct ww_mutex, base);
		DEBUG_LOCKS_WARN_ON(ww_ctx->contending_lock);
		ww_ctx->contending_lock = ww;
#endif
		return -EDEADLK;
	}

	return 0;
}

/*
 * Check the wound condition for the current lock acquire.
 *
 * Wound-Wait: If we're wounded, kill ourself.
 *
 * Wait-Die: If we're trying to acquire a lock already held by an older
 *           context, kill ourselves.
 *
 * Since __ww_mutex_add_waiter() orders the wait-list on stamp, we only have to
 * look at waiters before us in the wait-list.
 */
/*
 * 检查当前获取是否应退出：Wound-Wait 中 current 已被 wound 就 kill 自己；Wait-Die 中若目标由更老
 * context 持有就 kill 自己。add_waiter 已按 stamp 排序，所以 Wait-Die 还只需查看本节点之前的 waiter。
 *
 * __ww_mutex_check_kill - 在睡眠循环中判断 current 的 ww 回退条件
 * @lock: 调用者持有模板 WAIT_LOCK 的竞争锁
 * @waiter: current 已入队且仍有效的 waiter
 * @ctx: current 的 acquire context
 *
 * 未持其他锁直接返回 0。Wound-Wait 仅在 wounded 时调用 kill；Wait-Die 比较 owner context，再反向
 * 扫描前置带 context waiter。应回退时返回 -EDEADLK，否则 0；不负责退队或释放，外层错误路径处理。
 */
static inline int
__ww_mutex_check_kill(struct MUTEX *lock, struct MUTEX_WAITER *waiter,
		      struct ww_acquire_ctx *ctx)
	__must_hold(&lock->WAIT_LOCK)
{
	struct ww_mutex *ww = container_of(lock, struct ww_mutex, base);
	struct ww_acquire_ctx *hold_ctx = READ_ONCE(ww->ctx);
	struct MUTEX_WAITER *cur;

	if (ctx->acquired == 0)
		return 0;

	if (!ctx->is_wait_die) {
		if (ctx->wounded)
			return __ww_mutex_kill(lock, ctx);

		return 0;
	}

	if (hold_ctx && __ww_ctx_less(ctx, hold_ctx))
		return __ww_mutex_kill(lock, ctx);

	/*
	 * If there is a waiter in front of us that has a context, then its
	 * stamp is earlier than ours and we must kill ourself.
	 */
	/* 若前方存在带 context 的 waiter，其 stamp 必然更早，current 必须自行退出。 */
	for (cur = __ww_waiter_prev(lock, waiter); cur;
	     cur = __ww_waiter_prev(lock, cur)) {

		if (!cur->ww_ctx)
			continue;

		return __ww_mutex_kill(lock, ctx);
	}

	return 0;
}

/*
 * Add @waiter to the wait-list, keep the wait-list ordered by stamp, smallest
 * first. Such that older contexts are preferred to acquire the lock over
 * younger contexts.
 *
 * Waiters without context are interspersed in FIFO order.
 *
 * Furthermore, for Wait-Die kill ourself immediately when possible (there are
 * older contexts already waiting) to avoid unnecessary waiting and for
 * Wound-Wait ensure we wound the owning context when it is younger.
 */
/*
 * 按 stamp 从小到大插入 @waiter，使老 context 优先；无 context waiter 仍按 FIFO 穿插。Wait-Die 在
 * 已有更老 waiter 时尽早 kill current，避免无用睡眠；Wound-Wait 则在 owner 更年轻时 wound 它。
 *
 * __ww_mutex_add_waiter - 排序发布 ww waiter 并执行即时死锁动作
 * @waiter: 当前栈帧拥有、已初始化但尚未由普通后端插入的 waiter
 * @lock: 调用者持有模板 WAIT_LOCK 的 mutex/rt_mutex
 * @ww_ctx: 可选 acquire context；NULL 表示普通 FIFO waiter
 * @wake_q: 收集需延后唤醒的任务
 *
 * 无 context 时直接按后端规则加入并返回 0。有 context 时从末向前跳过普通 waiter，找 stamp 插入
 * 点；Wait-Die 可能在入队前返回 -EDEADLK，RT 后端由外层移除已预插节点；成功加入后 Wound-Wait
 * 以全屏障和 fastpath 配对，再尝试 wound owner。返回 0 表示可继续等待，负值表示外层必须取消。
 */
static inline int
__ww_mutex_add_waiter(struct MUTEX_WAITER *waiter,
		      struct MUTEX *lock,
		      struct ww_acquire_ctx *ww_ctx,
		      struct wake_q_head *wake_q)
	__must_hold(&lock->WAIT_LOCK)
{
	struct MUTEX_WAITER *cur, *pos = NULL;
	bool is_wait_die;

	if (!ww_ctx) {
		__ww_waiter_add(lock, waiter, NULL);
		return 0;
	}

	is_wait_die = ww_ctx->is_wait_die;

	/*
	 * Add the waiter before the first waiter with a higher stamp.
	 * Waiters without a context are skipped to avoid starving
	 * them. Wait-Die waiters may die here. Wound-Wait waiters
	 * never die here, but they are sorted in stamp order and
	 * may wound the lock holder.
	 */
	/*
	 * 从末端寻找首个更大 stamp 之前的位置；跳过无 context waiter，避免其饥饿。Wait-Die 可在这里
	 * die，Wound-Wait 不在这里 die，只按 stamp 排序并可能 wound owner。
	 */
	for (cur = __ww_waiter_last(lock); cur;
	     cur = __ww_waiter_prev(lock, cur)) {

		if (!cur->ww_ctx)
			continue;

		if (__ww_ctx_less(ww_ctx, cur->ww_ctx)) {
			/*
			 * Wait-Die: if we find an older context waiting, there
			 * is no point in queueing behind it, as we'd have to
			 * die the moment it would acquire the lock.
			 */
				/* Wait-Die 遇到已等待的更老 context 时无需排在其后，因为它一得锁 current 就必须 die。 */
			if (is_wait_die) {
				int ret = __ww_mutex_kill(lock, ww_ctx);

				if (ret)
					return ret;
			}

			break;
		}

		pos = cur;

		/* Wait-Die: ensure younger waiters die. */
		/* Wait-Die：让已经排队且更年轻、又持其他锁的 waiter 及时退出。 */
		__ww_mutex_die(lock, cur, ww_ctx, wake_q);
	}

	__ww_waiter_add(lock, waiter, pos);

	/*
	 * Wound-Wait: if we're blocking on a mutex owned by a younger context,
	 * wound that such that we might proceed.
	 */
	/* Wound-Wait：若本次阻塞的 mutex 由更年轻 context 持有，wound 它以推动当前请求。 */
	if (!is_wait_die) {
		struct ww_mutex *ww = container_of(lock, struct ww_mutex, base);

		/*
		 * See ww_mutex_set_context_fastpath(). Orders setting
		 * MUTEX_FLAG_WAITERS vs the ww->ctx load,
		 * such that either we or the fastpath will wound @ww->ctx.
		 */
		/*
		 * 与 ww_mutex_set_context_fastpath() 配对，对 WAITERS 的发布与 ww->ctx 读取排序，保证这里或
		 * fastpath 至少一方会观察并 wound owner context。
		 */
		smp_mb();
		__ww_mutex_wound(lock, ww_ctx, ww->ctx, wake_q);
	}

	return 0;
}

/*
 * __ww_mutex_unlock - 在释放 base mutex 前撤销 ww context 记账
 * @lock: current 持有且保持存活的 ww_mutex
 *
 * 有 ctx 时诊断 acquired 已为零，正常情况下递减一次并清 lock->ctx；无 ctx 表示单锁模式，只需返回。
 * 函数不释放 base owner，调用者必须紧接着执行 mutex/rtmutex 解锁；ctx 存储仍由事务调用者拥有。
 */
static inline void __ww_mutex_unlock(struct ww_mutex *lock)
{
	if (lock->ctx) {
#ifdef DEBUG_WW_MUTEXES
		DEBUG_LOCKS_WARN_ON(!lock->ctx->acquired);
#endif
		if (lock->ctx->acquired > 0)
			lock->ctx->acquired--;
		lock->ctx = NULL;
	}
}
