// SPDX-License-Identifier: GPL-2.0-only
/*
 * rtmutex API
 */
/*
 * RT mutex 对外 API 层：把本文件下方的公开接口连接到 rtmutex.c 的 PI、等待和 owner 交接核心。
 * 本层还负责 lockdep、sysctl、PI-futex proxy，以及 PREEMPT_RT 普通 mutex 的兼容包装。
 */
#include <linux/spinlock.h>
#include <linux/export.h>

/* 选择 rtmutex.c 中普通 rtmutex/futex 所需的文本实例；该包含会在本翻译单元生成内部 helper。 */
#define RT_MUTEX_BUILD_MUTEX
#include "rtmutex.c"

/*
 * Max number of times we'll walk the boosting chain:
 */
/*
 * 单次 PI boost/deboost 链遍历允许经过的最大锁层数；rtmutex.c 读取它来截断损坏或过深的依赖链。
 * 管理员可通过 kernel.max_lock_depth 调整，整数单位为“锁链层数”，默认 1024。
 */
int max_lock_depth = 1024;

/*
 * 早期注册后常驻的 sysctl 描述表；data 借用全局 max_lock_depth，proc_dointvec 负责整数读写。
 * mode=0644 允许所有用户读取、特权写入；表和数据均为静态生命周期，不发生 ownership 转移。
 */
static const struct ctl_table rtmutex_sysctl_table[] = {
	{
		.procname	= "max_lock_depth",
		.data		= &max_lock_depth,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
};

/*
 * 在 subsys initcall 阶段把 rtmutex 调试/保护参数发布到 /proc/sys/kernel/max_lock_depth。
 * 入参：无；入口处尚无并发销毁该静态表的可能，函数不持 rtmutex 锁，注册过程可执行初始化分配。
 * 返回：恒为 0，让 initcall 继续；register_sysctl_init() 持有注册项并由 sysctl 核心管理其生命周期。
 */
static int __init init_rtmutex_sysctl(void)
{
	/* “kernel” 是目录名，表中 procname 决定叶子名；初始化专用接口无需保存 ctl_table_header。 */
	register_sysctl_init("kernel", rtmutex_sysctl_table);
	return 0;
}

/* 将上面的注册函数安排在子系统初始化阶段执行一次，完成后其 __init 代码可被回收。 */
subsys_initcall(init_rtmutex_sysctl);

/*
 * Debug aware fast / slowpath lock,trylock,unlock
 *
 * The atomic acquire/release ops are compiled away, when either the
 * architecture does not support cmpxchg or when debugging is enabled.
 */
/*
 * 以下公共包装同时维护真实 rtmutex 与 lockdep 状态。架构不支持 cmpxchg 或启用 RT mutex 调试时，
 * rtmutex.c 会把原子 fastpath 编译掉，但本层的调用契约和成功/失败语义保持不变。
 */
/*
 * 为各种公开阻塞式 rt_mutex 获取入口提供统一的 lockdep 与 PI 慢/快路径编排。
 * lock 是输入输出、由调用者借用的完整 rt_mutex；state 指定 UNINTERRUPTIBLE、INTERRUPTIBLE 或
 * KILLABLE 睡眠语义；nest_lock 可空，是 lockdep 嵌套依赖来源；subclass 是 lockdep 子类编号。
 * 入口不持 lock，必须在可睡眠任务上下文；返回 0 表示 current 已持锁，负 errno 表示未持锁。
 * 先向 lockdep 发布“准备获取”，再调用 __rt_mutex_lock()；失败时撤销 lockdep 获取记录，避免模型
 * 与真实 owner 分离。函数不转移 lock/nest_lock ownership，成功后的解锁责任交给调用者。
 */
static __always_inline int __rt_mutex_lock_common(struct rt_mutex *lock,
						  unsigned int state,
						  struct lockdep_map *nest_lock,
						  unsigned int subclass)
	__cond_acquires(0, lock)
{
	/* ret 是底层阻塞获取结果；仅 0 对应真实锁和 lockdep map 都已处于 acquired 状态。 */
	int ret;

	/* 阶段一：校验睡眠上下文并登记包含 subclass/nest_lock 的依赖边。 */
	might_sleep();
	mutex_acquire_nest(&lock->dep_map, subclass, 0, nest_lock, _RET_IP_);
	/* 阶段二：底层可走 owner cmpxchg fastpath，或排队、传播 PI 并按 state 睡眠。 */
	ret = __rt_mutex_lock(&lock->rtmutex, state);
	/* 可中断获取失败并未得到真实锁，必须对称撤销刚才的 lockdep acquire。 */
	if (ret)
		mutex_release(&lock->dep_map, _RET_IP_);
	return ret;
}

/*
 * 初始化一个仅含 owner/waiter 核心的 rt_mutex_base，供 futex、RCU boost 等内部用户使用。
 * rtb 是调用者拥有的输入输出对象，初始化前不得并发可见或正被持有；函数不取得其长期引用。
 * 返回：无直接返回值；退出时 owner 为空、waiter 树和 wait_lock 已初始化。函数不睡眠且不加外部锁。
 */
void rt_mutex_base_init(struct rt_mutex_base *rtb)
{
	__rt_mutex_base_init(rtb);
}
EXPORT_SYMBOL(rt_mutex_base_init);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/**
 * rt_mutex_lock_nested - lock a rt_mutex
 *
 * @lock: the rt_mutex to be locked
 * @subclass: the lockdep subclass
 */
/*
 * 在启用 lockdep 分配跟踪时，不可中断地获取 rt_mutex，并按 subclass 区分同一锁类的嵌套层级。
 * lock 是调用者借用的输入输出锁，subclass 是 lockdep 整数子类；入口为可睡眠任务上下文且不持 lock。
 * 返回：无直接返回值；正常退出保证 current 持有 lock，调用者随后必须 rt_mutex_unlock()。
 * 底层使用 TASK_UNINTERRUPTIBLE，不接受信号/超时，因此理论上不会返回错误；异常分支仅用于诊断。
 */
void __sched rt_mutex_lock_nested(struct rt_mutex *lock, unsigned int subclass)
{
	/* 统一包装同步更新 lockdep 与真实 owner；成功即完成本接口全部工作。 */
	if (__rt_mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, NULL, subclass) == 0)
		return;
	/*
	 * The code below is never reached because __rt_mutex_lock_common() only
	 * returns an error code if interrupted by a signal or upon a timeout.
	 */
	/*
	 * 下方理论上不可达：公共 helper 只会因信号或超时返回错误，而不可中断且无超时的本调用没有
	 * 这两种出口。若不变量被破坏，只告警一次，并用 __acquire 修正静态上下文分析的控制流模型。
	 */
	WARN_ON_ONCE(true);
	__acquire(lock);
}
EXPORT_SYMBOL_GPL(rt_mutex_lock_nested);

/*
 * 以另一把锁的 lockdep map 作为嵌套关系来源，不可中断地获取 rt_mutex。
 * lock 是待获取的输入输出借用锁；nest_lock 是只读借用 map，必须在调用期间有效且描述外层依赖。
 * 入口为可睡眠任务上下文、不持 lock；返回无直接值，正常退出保证持锁且 ownership 不转移。
 * CONFIG_DEBUG_LOCK_ALLOC 才生成此入口；底层无错误来源，诊断尾部仅防未来契约变化。
 */
void __sched _rt_mutex_lock_nest_lock(struct rt_mutex *lock, struct lockdep_map *nest_lock)
{
	/* subclass=0，但 nest_lock 使 lockdep 把此次获取连接到指定外层锁类。 */
	if (__rt_mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, nest_lock, 0) == 0)
		return;
	/*
	 * The code below is never reached because __rt_mutex_lock_common() only
	 * returns an error code if interrupted by a signal or upon a timeout.
	 */
	/*
	 * 不可中断、无超时的获取不应失败；若发生则告警一次，并补充静态分析所需的 acquire 状态。
	 */
	WARN_ON_ONCE(true);
	__acquire(lock);
}
EXPORT_SYMBOL_GPL(_rt_mutex_lock_nest_lock);

#else /* !CONFIG_DEBUG_LOCK_ALLOC */
/* 未启用 lockdep 分配跟踪时只导出无 subclass/nest_lock 参数的普通获取入口。 */

/**
 * rt_mutex_lock - lock a rt_mutex
 *
 * @lock: the rt_mutex to be locked
 */
/*
 * 不可中断地获取普通 rt_mutex；这是关闭 CONFIG_DEBUG_LOCK_ALLOC 时的公开阻塞入口。
 * lock 是调用者借用的输入输出对象；入口为可睡眠任务上下文且不持 lock，不转移对象 ownership。
 * 返回：无直接返回值；退出保证 current 持锁，调用者必须配对 rt_mutex_unlock()。TASK_UNINTERRUPTIBLE
 * 使信号不能形成错误出口，底层仍可能排队、传播 PI 并调度睡眠。
 */
void __sched rt_mutex_lock(struct rt_mutex *lock)
{
	/* 无 lockdep 子类信息的标准公共获取；成功立即返回。 */
	if (__rt_mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, NULL, 0) == 0)
		return;
	/*
	 * The code below is never reached because __rt_mutex_lock_common() only
	 * returns an error code if interrupted by a signal or upon a timeout.
	 */
	/*
	 * 公共 helper 只有信号/超时才返回错误，本路径两者都未启用，故尾部只保留防御性告警和分析标注。
	 */
	WARN_ON_ONCE(true);
	__acquire(lock);
}
EXPORT_SYMBOL_GPL(rt_mutex_lock);
#endif

/**
 * rt_mutex_lock_interruptible - lock a rt_mutex interruptible
 *
 * @lock:		the rt_mutex to be locked
 *
 * Returns:
 *  0		on success
 * -EINTR	when interrupted by a signal
 */
/*
 * 以 TASK_INTERRUPTIBLE 等待普通 rt_mutex，使任意待处理信号可以中止排队。
 * lock 是调用者借用的输入输出锁；入口必须是可睡眠任务上下文且不持 lock，不转移对象 ownership。
 * 返回 0 表示 current 已持锁、调用者负责解锁；返回 -EINTR 表示 waiter/PI 状态已撤销且未持锁。
 * 此入口通过公共 helper 同步维护 lockdep，成功后通常进入调用者临界区，失败由调用者处理信号。
 */
int __sched rt_mutex_lock_interruptible(struct rt_mutex *lock)
{
	return __rt_mutex_lock_common(lock, TASK_INTERRUPTIBLE, NULL, 0);
}
EXPORT_SYMBOL_GPL(rt_mutex_lock_interruptible);

/**
 * rt_mutex_lock_killable - lock a rt_mutex killable
 *
 * @lock:		the rt_mutex to be locked
 *
 * Returns:
 *  0		on success
 * -EINTR	when interrupted by a signal
 */
/*
 * 以 TASK_KILLABLE 等待 rt_mutex，只允许致命信号中止锁等待。
 * lock 是调用者借用的输入输出锁；入口为可睡眠任务上下文且不持 lock，无引用或 ownership 转移。
 * 返回 0 表示 current 已持锁并由调用者负责释放；-EINTR 表示致命信号打断且锁/PI 排队已清理。
 * 与 interruptible 版本的区别仅是 task state 的信号筛选范围，lockdep 与底层 PI 协议相同。
 */
int __sched rt_mutex_lock_killable(struct rt_mutex *lock)
{
	return __rt_mutex_lock_common(lock, TASK_KILLABLE, NULL, 0);
}
EXPORT_SYMBOL_GPL(rt_mutex_lock_killable);

/**
 * rt_mutex_trylock - try to lock a rt_mutex
 *
 * @lock:	the rt_mutex to be locked
 *
 * This function can only be called in thread context. It's safe to call it
 * from atomic regions, but not from hard or soft interrupt context.
 *
 * Returns:
 *  1 on success
 *  0 on contention
 */
/*
 * 非阻塞尝试获取 rt_mutex，不把 current 排入 waiter 树。
 * lock 是调用者借用的输入输出锁；只能从线程上下文调用，可位于原子区，但禁止 hardirq/softirq。
 * 返回 1 表示 current 已成为 owner、调用者必须解锁；返回 0 表示竞争或调试构建发现非任务上下文，
 * 此时不持锁。函数不睡眠、不转移引用；成功时补记 trylock 类型的 lockdep acquire。
 */
int __sched rt_mutex_trylock(struct rt_mutex *lock)
{
	/* ret 同时作为真实 owner 获取结果和公开布尔返回值；1 成功、0 失败。 */
	int ret;

	/* 调试构建显式诊断中断上下文误用；非调试构建仍由 API 契约禁止该调用。 */
	if (IS_ENABLED(CONFIG_DEBUG_RT_MUTEXES) && WARN_ON_ONCE(!in_task()))
		return 0;

	/* 底层先尝试 cmpxchg，必要时只在 wait_lock 下复核，不排队也不调度。 */
	ret = __rt_mutex_trylock(&lock->rtmutex);
	/* 只有真实获取成功才向 lockdep 发布 trylock acquire，保持两种状态一致。 */
	if (ret)
		mutex_acquire(&lock->dep_map, 0, 1, _RET_IP_);

	return ret;
}
EXPORT_SYMBOL_GPL(rt_mutex_trylock);

/**
 * rt_mutex_unlock - unlock a rt_mutex
 *
 * @lock: the rt_mutex to be unlocked
 */
/*
 * 释放 current 持有的 rt_mutex，并在有 waiter 时完成 deboost 与最高优先级 waiter 交接。
 * lock 是输入输出借用对象，入口要求 current 为 owner；函数不转移 lock 引用，不能从中断上下文调用。
 * 返回：无直接返回值；退出时 current 不再持锁，lockdep 记录已释放，top waiter 已被延迟唤醒或锁为空。
 * 必须先撤销 lockdep 状态再执行真实解锁，后者的慢路径可能在恢复抢占时调度。
 */
void __sched rt_mutex_unlock(struct rt_mutex *lock)
{
	/* lockdep release 与获取包装配对；随后底层用 release cmpxchg 或 wait_lock 慢路径交接 owner。 */
	mutex_release(&lock->dep_map, _RET_IP_);
	__rt_mutex_unlock(&lock->rtmutex);
	/* 告知静态上下文分析本接口已经释放 lock，不产生运行时代码。 */
	__release(lock);
}
EXPORT_SYMBOL_GPL(rt_mutex_unlock);

/*
 * Futex variants, must not use fastpath.
 */
/*
 * 以下 PI-futex 变体必须绕过普通 owner cmpxchg fastpath：futex 调用者需要在 wait_lock 下把
 * pi_state、waiter 与用户态 owner 更新编排成一个协议，不能让无锁 fastpath 越过这些状态。
 */
/*
 * 为 PI-futex 非阻塞地尝试取得底层 rt_mutex_base。
 * lock 是 futex pi_state 内的输入输出借用锁；入口不持 wait_lock，不排队且不转移引用。
 * 返回 1 表示 current 成为 owner，0 表示竞争；只短暂取得 raw wait_lock，不调度睡眠。
 * 与普通 trylock 不同，它固定进入 rt_mutex_slowtrylock()，由 futex 调用者管理后续用户态状态。
 */
int __sched rt_mutex_futex_trylock(struct rt_mutex_base *lock)
{
	return rt_mutex_slowtrylock(lock);
}

/*
 * 提供同一 PI-futex slow trylock 语义的内部命名入口，供需要显式下划线层级的调用链使用。
 * lock 是输入输出借用锁；入口不持 wait_lock。返回 1 表示 current 获取成功、0 表示竞争。
 * 函数不排队、不睡眠、不维护完整 rt_mutex 的 dep_map，也不改变对象 ownership。
 */
int __sched __rt_mutex_futex_trylock(struct rt_mutex_base *lock)
{
	return __rt_mutex_slowtrylock(lock);
}

/**
 * __rt_mutex_futex_unlock - Futex variant, that since futex variants
 * do not use the fast-path, can be simple and will not need to retry.
 *
 * @lock:	The rt_mutex to be unlocked
 * @wqh:	The wake queue head from which to get the next lock waiter
 */
/*
 * 在已持 wait_lock 时执行 PI-futex 解锁的交接前半段；futex 不使用 fastpath，因此无需释放后重试。
 * lock 是 current 持有的输入输出底层锁；wqh 是调用者拥有的输入输出延迟唤醒队列。
 * 入口必须持 lock->wait_lock 且中断按调用者协议关闭，函数不能睡眠、不释放 wait_lock。
 * 返回 false 表示无 waiter、owner 已直接清空且全部完成；true 表示 top waiter 已排入 wqh，调用者
 * 放 wait_lock 后必须调用 rt_mutex_postunlock() 完成唤醒、引用归还和 preempt_enable()。
 */
bool __sched __rt_mutex_futex_unlock(struct rt_mutex_base *lock,
				     struct rt_wake_q_head *wqh)
	__must_hold(&lock->wait_lock)
{
	lockdep_assert_held(&lock->wait_lock);

	/* 先执行调试 owner/锁状态检查；此时 wait_lock 使 owner 与 waiter 树保持稳定。 */
	debug_rt_mutex_unlock(lock);

	/* 无 waiter 时可在 wait_lock 保护下直接清 owner；futex 从不开放并发 owner fastpath。 */
	if (!rt_mutex_has_waiters(lock)) {
		lock->owner = NULL;
		/* 原文 done：false 告诉外层无需执行锁外 postunlock。 */
		return false; /* done */
	}

	/*
	 * mark_wakeup_next_waiter() deboosts and retains preemption
	 * disabled when dropping the wait_lock, to avoid inversion prior
	 * to the wakeup.  preempt_disable() therein pairs with the
	 * preempt_enable() in rt_mutex_postunlock().
	 */
	/*
	 * mark_wakeup_next_waiter() 先从 current 的 PI 树移除 top donor 并 deboost，再把 top 加入 wqh。
	 * 它在 wait_lock 后仍保持抢占关闭，防止 donor 真正唤醒前 current 被抢占而形成优先级反转；其中
	 * preempt_disable() 必须由 rt_mutex_postunlock() 内的 preempt_enable() 配对。
	 */
	mark_wakeup_next_waiter(wqh, lock);

	/* 原文 call postunlock：true 把锁外唤醒和抢占恢复责任显式交给调用者。 */
	return true; /* call postunlock() */
}

/*
 * 提供自包含的 PI-futex 解锁包装：取得 wait_lock、执行交接前半段，再在锁外按需唤醒 top waiter。
 * lock 是 current 持有的输入输出底层锁；入口不持 wait_lock，函数不转移 lock ownership。
 * 返回：无直接返回值；退出时 current 不再是 owner，内部中断/自旋锁状态已恢复，必要唤醒已完成。
 * 函数不作普通阻塞等待，但 postunlock 恢复抢占时可能触发调度。
 */
void __sched rt_mutex_futex_unlock(struct rt_mutex_base *lock)
{
	/* wqh 保存必须延迟到 wait_lock 外执行的唤醒；flags 保存调用前中断状态。 */
	DEFINE_RT_WAKE_Q(wqh);
	unsigned long flags;
	bool postunlock;

	/* 阶段一：在 irqsave wait_lock 下稳定 owner/waiter，并取得是否需后半段的布尔结果。 */
	raw_spin_lock_irqsave(&lock->wait_lock, flags);
	postunlock = __rt_mutex_futex_unlock(lock, &wqh);
	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);

	/* 阶段二：只有 helper 已禁抢占并排入 waiter 时，才消费 wqh 和恢复抢占。 */
	if (postunlock)
		rt_mutex_postunlock(&wqh);
}

/**
 * __rt_mutex_init - initialize the rt_mutex
 *
 * @lock:	The rt_mutex to be initialized
 * @name:	The lock name used for debugging
 * @key:	The lock class key used for debugging
 *
 * Initialize the rt_mutex to unlocked state.
 *
 * Initializing of a locked rt_mutex is not allowed
 */
/*
 * 把完整 rt_mutex 初始化为空闲状态，并建立名称/类键对应的 sleeping-lock lockdep map。
 * lock 是调用者拥有的输入输出对象；name 是常驻调试名称借用指针，key 是常驻 lock class key 借用指针。
 * 初始化前 lock 不得处于持有状态或并发可见；函数不需要外部锁、不睡眠，也不取得参数引用。
 * 返回：无直接返回值；退出时 base owner/树/wait_lock 与 dep_map 均可用，后续可进入公开锁 API。
 */
void __sched __rt_mutex_init(struct rt_mutex *lock, const char *name,
			     struct lock_class_key *key)
{
	/* 先让 debugobjects/lockdep 确认该内存范围没有遗留的活锁，再重建内部状态。 */
	debug_check_no_locks_freed((void *)lock, sizeof(*lock));
	__rt_mutex_base_init(&lock->rtmutex);
	/* LD_WAIT_SLEEP 标记该锁等待会调度，name/key 的生命周期由静态初始化宏或调用者保证。 */
	lockdep_init_map_wait(&lock->dep_map, name, key, 0, LD_WAIT_SLEEP);
}
EXPORT_SYMBOL_GPL(__rt_mutex_init);

/**
 * rt_mutex_init_proxy_locked - initialize and lock a rt_mutex on behalf of a
 *				proxy owner
 *
 * @lock:	the rt_mutex to be locked
 * @proxy_owner:the task to set as owner
 *
 * No locking. Caller has to do serializing itself
 *
 * Special API call for PI-futex support. This initializes the rtmutex and
 * assigns it to @proxy_owner. Concurrent operations on the rtmutex are not
 * possible at this point because the pi_state which contains the rtmutex
 * is not yet visible to other tasks.
 */
/*
 * 为 PI-futex 构造一把“已由 proxy_owner 持有”的底层 rt_mutex，建立用户态 owner 的内核 PI 表示。
 * lock 是尚未发布的 pi_state 内输入输出对象；proxy_owner 是只读借用 task，函数不增加其引用。
 * 本函数自身不加锁、不能睡眠；调用者必须保证 pi_state 尚未对其他任务可见或提供等价串行化。
 * 返回：无直接返回值；退出时锁已初始化、使用独立 futex wait_lock 类，并发布 proxy_owner 为 owner。
 * 之后竞争者获取该人工锁即可向 proxy_owner 捐赠优先级；task/pi_state 生命周期仍由 futex 层管理。
 */
void __sched rt_mutex_init_proxy_locked(struct rt_mutex_base *lock,
					struct task_struct *proxy_owner)
{
	/* 所有 PI-futex wait_lock 共用这一静态类键；对象常驻，满足 lockdep map 生命周期要求。 */
	static struct lock_class_key pi_futex_key;

	/* 阶段一：在对象发布前建立空 base；此处的无锁串行化来自 pi_state 尚不可见。 */
	__rt_mutex_base_init(lock);
	/*
	 * On PREEMPT_RT the futex hashbucket spinlock becomes 'sleeping'
	 * and rtmutex based. That causes a lockdep false positive, because
	 * some of the futex functions invoke spin_unlock(&hb->lock) with
	 * the wait_lock of the rtmutex associated to the pi_futex held.
	 * spin_unlock() in turn takes wait_lock of the rtmutex on which
	 * the spinlock is based, which makes lockdep notice a lock
	 * recursion. Give the futex/rtmutex wait_lock a separate key.
	 */
	/*
	 * PREEMPT_RT 把 futex hashbucket spinlock 变成基于 rtmutex 的 sleeping lock。部分 futex 路径在
	 * 持 PI-futex 的 wait_lock 时调用 spin_unlock(&hb->lock)，而该 unlock 内部又会取得其底层
	 * rtmutex wait_lock；若二者共享同一 lockdep 类，lockdep 会误判为递归。为 PI-futex wait_lock
	 * 分配独立类键，只修正依赖模型，不改变真实锁顺序。
	 */
	lockdep_set_class(&lock->wait_lock, &pi_futex_key);
	/* 阶段二：用统一 tagged-owner helper 写入代理 owner；调用者随后才可发布包含它的 pi_state。 */
	rt_mutex_set_owner(lock, proxy_owner);
}

/**
 * rt_mutex_proxy_unlock - release a lock on behalf of owner
 *
 * @lock:	the rt_mutex to be locked
 *
 * No locking. Caller has to do serializing itself
 *
 * Special API call for PI-futex support. This just cleans up the rtmutex
 * (debugging) state. Concurrent operations on this rt_mutex are not
 * possible because it belongs to the pi_state which is about to be freed
 * and it is not longer visible to other tasks.
 */
/*
 * 在真实 owner 之外代表 proxy owner 清理 PI-futex rtmutex 的调试状态与 owner 字段。
 * lock 是即将随 pi_state 销毁的输入输出借用对象；原文参数说明写作“to be locked”，实际是代理解锁。
 * 函数自身不加锁、不睡眠；调用者必须已摘除 pi_state，使其他任务不可能并发操作该锁。
 * 返回：无直接返回值；退出时 owner 已清空，调试状态已结束，不释放 pi_state 或 task 引用。
 */
void __sched rt_mutex_proxy_unlock(struct rt_mutex_base *lock)
{
	/* 先让调试后端核对代理解锁，再在外部不可见保证下清除 owner。 */
	debug_rt_mutex_proxy_unlock(lock);
	rt_mutex_clear_owner(lock);
}

/**
 * __rt_mutex_start_proxy_lock() - Start lock acquisition for another task
 * @lock:		the rt_mutex to take
 * @waiter:		the pre-initialized rt_mutex_waiter
 * @task:		the task to prepare
 * @wake_q:		the wake_q to wake tasks after we release the wait_lock
 *
 * Starts the rt_mutex acquire; it enqueues the @waiter and does deadlock
 * detection. It does not wait, see rt_mutex_wait_proxy_lock() for that.
 *
 * NOTE: does _NOT_ remove the @waiter on failure; must either call
 * rt_mutex_wait_proxy_lock() or rt_mutex_cleanup_proxy_lock() after this.
 *
 * Returns:
 *  0 - task blocked on lock
 *  1 - acquired the lock for task, caller should wake it up
 * <0 - error
 *
 * Special API call for PI-futex support.
 */
/*
 * 为另一个 task 启动 PI-futex 锁获取：立即接管或入队并执行完整 PI 链死锁检测，但本函数不等待。
 * lock 是输入输出底层锁；waiter 是调用者预初始化、由调用者保持存活的输入输出节点；task 是将成为
 * owner/阻塞者的借用任务；wake_q 是输入输出延迟唤醒队列。入口必须持 wait_lock 且中断关闭。
 * 返回 1 表示 task 已成为 owner、调用者应唤醒它；0 表示 waiter 已排队；负 errno 表示检测到错误。
 * 关键 ownership：即使返回负值，本入口也不移除 waiter；调用者必须继续 wait 或 cleanup。函数在
 * chainwalk 中可能短暂放 wait_lock 并可抢占，返回前重新持锁；不取得 task/waiter 长期引用。
 */
int __sched __rt_mutex_start_proxy_lock(struct rt_mutex_base *lock,
					struct rt_mutex_waiter *waiter,
					struct task_struct *task,
					struct wake_q_head *wake_q)
	__must_hold(&lock->wait_lock)
{
	/* ret 记录 FULL chainwalk 的 0/负错误；直接接管成功在赋值前返回 1。 */
	int ret;

	lockdep_assert_held(&lock->wait_lock);

	/* 阶段一：先在既有 wait_lock 下尝试无排队代理接管，成功后 caller 负责唤醒 task。 */
	if (try_to_take_rt_mutex(lock, task, NULL))
		return 1;

	/* We enforce deadlock detection for futexes */
	/* futex 强制完整死锁检测；本包装不撤销期间已经建立的 waiter 状态。 */
	ret = task_blocks_on_rt_mutex(lock, waiter, task, NULL,
				      RT_MUTEX_FULL_CHAINWALK, wake_q);

	if (ret && !rt_mutex_owner(lock)) {
		/*
		 * Reset the return value. We might have
		 * returned with -EDEADLK and the owner
		 * released the lock while we were walking the
		 * pi chain.  Let the waiter sort it out.
		 */
		/*
		 * chainwalk 返回 -EDEADLK 等错误时，原 owner 可能已并发释放，使锁现已可被该 waiter 获取。
		 * 此时把结果重置为 0，不把已经消失的环报告给 futex 层；保留 waiter 让后续 wait/cleanup 在
		 * wait_lock 下重新竞争并决定最终 ownership。
		 */
		ret = 0;
	}

	return ret;
}

/**
 * rt_mutex_start_proxy_lock() - Start lock acquisition for another task
 * @lock:		the rt_mutex to take
 * @waiter:		the pre-initialized rt_mutex_waiter
 * @task:		the task to prepare
 *
 * Starts the rt_mutex acquire; it enqueues the @waiter and does deadlock
 * detection. It does not wait, see rt_mutex_wait_proxy_lock() for that.
 *
 * NOTE: unlike __rt_mutex_start_proxy_lock this _DOES_ remove the @waiter
 * on failure.
 *
 * Returns:
 *  0 - task blocked on lock
 *  1 - acquired the lock for task, caller should wake it up
 * <0 - error
 *
 * Special API call for PI-futex support.
 */
/*
 * 为另一个 task 启动完整 PI-futex 代理获取，并把内部 helper 的“错误不撤销”语义包装成错误即清理。
 * lock 是输入输出底层锁；waiter 是调用者预初始化且持续存活的输入输出节点；task 是借用目标任务。
 * 入口不持 wait_lock，函数只在内部短暂关中断并持锁，不等待锁；chainwalk 本身可抢占。
 * 返回 1 表示 task 已成为 owner且调用者应唤醒它；0 表示 waiter 保持排队；负 errno 表示 waiter 已
 * 从锁/PI 树撤销。函数不取得参数长期引用，所有 wake_q 项在释放 wait_lock 后已消费。
 */
int __sched rt_mutex_start_proxy_lock(struct rt_mutex_base *lock,
				      struct rt_mutex_waiter *waiter,
				      struct task_struct *task)
{
	/* ret 保存代理启动结果；wake_q 收集 chainwalk/PI 调整期间必须延迟到 wait_lock 外的唤醒。 */
	int ret;
	DEFINE_WAKE_Q(wake_q);

	/* 阶段一：关中断并串行化 owner/waiter 树，内部入口返回时仍持同一 wait_lock。 */
	raw_spin_lock_irq(&lock->wait_lock);
	ret = __rt_mutex_start_proxy_lock(lock, waiter, task, &wake_q);
	/* 与双下划线入口不同，公开包装在负错误时立即撤销可能已建立的 waiter 和 PI donation。 */
	if (unlikely(ret < 0))
		remove_waiter(lock, waiter);
	/*
	 * 放 wait_lock 到消费 wake_q 之间保持抢占关闭，避免高优先级待唤醒任务尚未 runnable 时当前任务
	 * 被抢占；wake_up_q 完成后再恢复抢占，可能在该边界触发调度。
	 */
	preempt_disable();
	raw_spin_unlock_irq(&lock->wait_lock);
	wake_up_q(&wake_q);
	preempt_enable();

	return ret;
}

/**
 * rt_mutex_wait_proxy_lock() - Wait for lock acquisition
 * @lock:		the rt_mutex we were woken on
 * @to:			the timeout, null if none. hrtimer should already have
 *			been started.
 * @waiter:		the pre-initialized rt_mutex_waiter
 *
 * Wait for the lock acquisition started on our behalf by
 * rt_mutex_start_proxy_lock(). Upon failure, the caller must call
 * rt_mutex_cleanup_proxy_lock().
 *
 * Returns:
 *  0 - success
 * <0 - error, one of -EINTR, -ETIMEDOUT
 *
 * Special API call for PI-futex support
 */
/*
 * 等待此前为 current 启动的 PI-futex proxy 获取完成；本函数只负责等待，不负责失败时最终摘除 waiter。
 * lock 是输入输出底层锁；to 可空，非空时是调用者已启动的 hrtimer_sleeper 借用指针；waiter 是
 * rt_mutex_start_proxy_lock() 留在树中的输入输出栈节点，调用期间必须有效并代表 current。
 * 入口不持 wait_lock、必须是可睡眠任务上下文；返回 0 表示 current 已成为 owner，-EINTR 或
 * -ETIMEDOUT 表示仍可能排队，调用者必须调用 rt_mutex_cleanup_proxy_lock() 消除与并发交接的竞态。
 */
int __sched rt_mutex_wait_proxy_lock(struct rt_mutex_base *lock,
				     struct hrtimer_sleeper *to,
				     struct rt_mutex_waiter *waiter)
{
	/* ret 是等待循环结果；本层不把错误转换为 cleanup 是否成功。 */
	int ret;

	/* 阶段一：在 wait_lock 下先发布可中断状态，再进入醒来—重试—睡眠循环以避免丢失唤醒。 */
	raw_spin_lock_irq(&lock->wait_lock);
	/* sleep on the mutex */
	/* 在 mutex 上睡眠：慢循环每次放锁前都已设置状态，成功或失败退出会恢复 TASK_RUNNING。 */
	set_current_state(TASK_INTERRUPTIBLE);
	ret = rt_mutex_slowlock_block(lock, NULL, TASK_INTERRUPTIBLE, to, waiter, NULL);
	/*
	 * try_to_take_rt_mutex() sets the waiter bit unconditionally. We might
	 * have to fix that up.
	 */
	/*
	 * try_to_take_rt_mutex() 为阻断 fastpath 会无条件置 HAS_WAITERS；等待结束时按真实树状态清理暂态位。
	 */
	fixup_rt_mutex_waiters(lock, true);
	/* 阶段二：恢复中断并交还锁；错误出口故意保留 waiter，留给 cleanup 重新判定 ownership。 */
	raw_spin_unlock_irq(&lock->wait_lock);

	return ret;
}

/**
 * rt_mutex_cleanup_proxy_lock() - Cleanup failed lock acquisition
 * @lock:		the rt_mutex we were woken on
 * @waiter:		the pre-initialized rt_mutex_waiter
 *
 * Attempt to clean up after a failed __rt_mutex_start_proxy_lock() or
 * rt_mutex_wait_proxy_lock().
 *
 * Unless we acquired the lock; we're still enqueued on the wait-list and can
 * in fact still be granted ownership until we're removed. Therefore we can
 * find we are in fact the owner and must disregard the
 * rt_mutex_wait_proxy_lock() failure.
 *
 * Returns:
 *  true  - did the cleanup, we done.
 *  false - we acquired the lock after rt_mutex_wait_proxy_lock() returned,
 *          caller should disregards its return value.
 *
 * Special API call for PI-futex support
 */
/*
 * 清理失败的 proxy 获取，并解决“等待已报错但并发 unlock 随后把锁授予本任务”的竞态。
 * lock 是输入输出底层锁；waiter 是此前为 current 排队的输入输出节点，调用期间由调用者保持存活。
 * 入口不持 wait_lock；函数不睡眠，只在关中断的 wait_lock/pi_lock 临界区操作，不转移引用。
 * 返回 true 表示 current 未获锁、waiter/PI donation 已撤销，调用者应保留原等待错误；false 表示
 * current 在竞态窗口中已成为 owner，waiter 已由获取路径摘除，调用者必须忽略原错误并按成功处理。
 */
bool __sched rt_mutex_cleanup_proxy_lock(struct rt_mutex_base *lock,
					 struct rt_mutex_waiter *waiter)
{
	/* cleanup 是公开布尔结果：初值 false 同时表示“最终发现已获得锁”。 */
	bool cleanup = false;

	/* 阶段一：wait_lock 将 owner 交接与当前 cleanup 判定串行化。 */
	raw_spin_lock_irq(&lock->wait_lock);
	/*
	 * Do an unconditional try-lock, this deals with the lock stealing
	 * state where __rt_mutex_futex_unlock() -> mark_wakeup_next_waiter()
	 * sets a NULL owner.
	 *
	 * We're not interested in the return value, because the subsequent
	 * test on rt_mutex_owner() will infer that. If the trylock succeeded,
	 * we will own the lock and it will have removed the waiter. If we
	 * failed the trylock, we're still not owner and we need to remove
	 * ourselves.
	 */
	/*
	 * 无条件再次 trylock，用来覆盖 __rt_mutex_futex_unlock()→mark_wakeup_next_waiter() 留下的
	 * NULL|HAS_WAITERS 交接状态。无需直接使用返回值：成功会把 current 写为 owner并自动摘除 waiter；
	 * 失败仍非 owner且 waiter 仍在树中，下面通过 owner 身份得到唯一最终结论。
	 */
	try_to_take_rt_mutex(lock, current, waiter);
	/*
	 * Unless we're the owner; we're still enqueued on the wait_list.
	 * So check if we became owner, if not, take us off the wait_list.
	 */
	/*
	 * 除非 current 已成为 owner，否则它仍在等待树中；此时 remove_waiter() 同步撤销 blocked_on、
	 * owner PI donor 与必要的 deboost chain，并把 cleanup 置 true。
	 */
	if (rt_mutex_owner(lock) != current) {
		remove_waiter(lock, waiter);
		cleanup = true;
	}
	/*
	 * try_to_take_rt_mutex() sets the waiter bit unconditionally. We might
	 * have to fix that up.
	 */
	/*
	 * try_to_take_rt_mutex() 无条件置 waiter 位；最终 owner 身份已确定，按当前树状态清除可能的暂态位。
	 * 成功获取已经由 set_owner 建立 acquire 语义，因此此处无需再次使用 acquire 型修复。
	 */
	fixup_rt_mutex_waiters(lock, false);

	/* 阶段二：释放 wait_lock/恢复中断后，返回最终由谁赢得交接竞态。 */
	raw_spin_unlock_irq(&lock->wait_lock);

	return cleanup;
}

/*
 * Recheck the pi chain, in case we got a priority setting
 *
 * Called from sched_setscheduler
 */
/*
 * 当调度器改变一个任务的有效排序参数后，重新检查它作为 rtmutex waiter 时是否需要重排整条 PI 链。
 * task 是调度器保证存活的输入输出借用任务；调用位置为 sched_setscheduler 路径释放 rq 锁之后。
 * 入口不持 task->pi_lock，本函数自行 irqsave 加锁；不作普通睡眠，但链遍历会反复放锁并可抢占。
 * 返回：无直接返回值；无阻塞或 waiter 键未变化时无副作用，否则更新 waiter 双树及沿链 owner 优先级。
 * 为跨越 pi_lock 释放窗口取得的 task 引用会转移给 rt_mutex_adjust_prio_chain() 并由其消费。
 */
void __sched rt_mutex_adjust_pi(struct task_struct *task)
{
	/* waiter 在 pi_lock 下稳定；next_lock 放锁后只作链变化地址哨兵；flags 保存原中断状态。 */
	struct rt_mutex_waiter *waiter;
	struct rt_mutex_base *next_lock;
	unsigned long flags;

	/* 阶段一：稳定 task 当前阻塞关系，并比较 waiter 旧键与 task 新调度键。 */
	raw_spin_lock_irqsave(&task->pi_lock, flags);

	waiter = task->pi_blocked_on;
	/* 未因 PI 阻塞或排序键仍相同，都不会影响锁树顺序和 donor，直接恢复现场。 */
	if (!waiter || rt_waiter_node_equal(&waiter->tree, task_to_waiter_node(task))) {
		raw_spin_unlock_irqrestore(&task->pi_lock, flags);
		return;
	}
	next_lock = waiter->lock;
	raw_spin_unlock_irqrestore(&task->pi_lock, flags);

	/* gets dropped in rt_mutex_adjust_prio_chain()! */
	/* 该引用保证无锁窗口中的 task 生命周期，并在 adjust_prio_chain() 任一出口统一 put。 */
	get_task_struct(task);

	/* 阶段二：orig_waiter=NULL 表示按 MIN 模式重排/deboost，而不是为新 donor 执行完整死锁检测。 */
	rt_mutex_adjust_prio_chain(task, RT_MUTEX_MIN_CHAINWALK, NULL,
				   next_lock, NULL, task);
}

/*
 * Performs the wakeup of the top-waiter and re-enables preemption.
 */
/*
 * 完成分段 futex 解锁的锁外后半段：唤醒 top waiter、消费 wake_q 引用并重新启用抢占。
 * wqh 是 __rt_mutex_futex_unlock() 填充的输入输出队列，调用者借用且必须已释放 wait_lock。
 * 入口要求前半段返回 true并保持抢占关闭；函数不持内部锁，返回无直接值，队列被消费且抢占恢复。
 * 唤醒和最终 preempt_enable() 可能触发调度；不得对 false/空前半段结果无配对调用。
 */
void __sched rt_mutex_postunlock(struct rt_wake_q_head *wqh)
{
	rt_mutex_wake_up_q(wqh);
}

#ifdef CONFIG_DEBUG_RT_MUTEXES
/*
 * 在 task_struct 即将释放时验证其 RT mutex PI 状态已经完全摘除。
 * task 是释放路径持有的输入只读任务对象；调用者保证其生命周期，不转移引用，函数不睡眠。
 * 返回：无直接返回值；pi_waiters 非空或 pi_blocked_on 仍存在时触发 DEBUG_LOCKS_WARN_ON，字段不修改。
 * 仅 CONFIG_DEBUG_RT_MUTEXES 生成，用于发现带活跃 donation/阻塞节点释放任务造成的悬空指针。
 */
void rt_mutex_debug_task_free(struct task_struct *task)
{
	/* 任务既不能仍向 owner 提供 PI donor，也不能仍挂在某把锁的 waiter 树上。 */
	DEBUG_LOCKS_WARN_ON(!RB_EMPTY_ROOT(&task->pi_waiters.rb_root));
	DEBUG_LOCKS_WARN_ON(task->pi_blocked_on);
}
#endif

#ifdef CONFIG_PREEMPT_RT
/* Mutexes */
/*
 * PREEMPT_RT 下普通 mutex 由 rtmutex 核心实现；以下接口保持通用 mutex ABI，同时增加 PI 与可调度等待。
 */
/*
 * 初始化 PREEMPT_RT mutex 的 rtmutex base，并确认整块 mutex 内存没有遗留的活锁调试状态。
 * mutex 是调用者拥有的输入输出对象，初始化前不得被持有或并发可见；函数不转移引用、不睡眠。
 * 返回：无直接返回值；退出时 rtmutex owner/树/wait_lock 为空闲，dep_map 由相应配置包装另行初始化。
 */
static void __mutex_rt_init_generic(struct mutex *mutex)
{
	/* base 初始化提供真实 PI 锁状态；debug 检查覆盖外层完整 mutex 大小。 */
	rt_mutex_base_init(&mutex->rtmutex);
	debug_check_no_locks_freed((void *)mutex, sizeof(*mutex));
}

/*
 * PREEMPT_RT 普通 mutex 各阻塞入口的共同核心：维护 lockdep，再通过 rtmutex 获取并确认真实 acquire。
 * lock 是输入输出借用 mutex；state 为三种允许的任务睡眠态；subclass 是 lockdep 子类；nest_lock
 * 可空且为只读借用依赖 map；ip 是调用点指令地址，仅用于 lockdep 归因。
 * 入口不持 lock、必须可睡眠；返回 0 表示 current 持锁，负 errno 表示未持锁且 lockdep 已回滚。
 * 不转移参数 ownership。成功调用 lock_acquired() 提交等待锁的实际获得时刻，调用者随后进入临界区。
 */
static __always_inline int __mutex_lock_common(struct mutex *lock,
					       unsigned int state,
					       unsigned int subclass,
					       struct lockdep_map *nest_lock,
					       unsigned long ip)
	__acquires(lock) __no_context_analysis
{
	/* ret 是底层 PI 获取结果，决定 lockdep acquire 是提交还是撤销。 */
	int ret;

	/* 阶段一：验证睡眠上下文并向 lockdep 登记待获取关系及嵌套信息。 */
	might_sleep();
	mutex_acquire_nest(&lock->dep_map, subclass, 0, nest_lock, ip);
	/* 阶段二：rtmutex 核心尝试 fastpath，竞争时入队、传播 PI 并按 state 等待。 */
	ret = __rt_mutex_lock(&lock->rtmutex, state);
	/* 失败未得到真实 owner，撤销依赖记录；成功则提交 acquired 时间点。 */
	if (ret)
		mutex_release(&lock->dep_map, ip);
	else
		lock_acquired(&lock->dep_map, ip);
	return ret;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/*
 * 初始化启用 lockdep 的 PREEMPT_RT mutex，包括底层 rtmutex base 与 sleeping-lock dep_map。
 * mutex 是调用者拥有的输入输出对象；name/key 是必须长期有效的只读调试元数据借用指针。
 * 初始化前对象不得被持有或并发可见；返回无直接值，不睡眠、不转移引用，退出后可供 mutex API 使用。
 */
void mutex_rt_init_lockdep(struct mutex *mutex, const char *name, struct lock_class_key *key)
{
	/* 先建立真实锁状态，再发布与该实例对应的 lockdep 名称和类别。 */
	__mutex_rt_init_generic(mutex);
	lockdep_init_map_wait(&mutex->dep_map, name, key, 0, LD_WAIT_SLEEP);
}
EXPORT_SYMBOL(mutex_rt_init_lockdep);

/*
 * 在启用 lockdep 时按 subclass 不可中断地获取 PREEMPT_RT mutex。
 * lock 是输入输出借用锁；subclass 是嵌套层级编号。入口为可睡眠任务上下文且不持 lock。
 * 返回：无直接返回值；TASK_UNINTERRUPTIBLE 保证正常返回时 current 持锁，调用者负责 mutex_unlock()。
 */
void __sched mutex_lock_nested(struct mutex *lock, unsigned int subclass)
{
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, subclass, NULL, _RET_IP_);
}
EXPORT_SYMBOL_GPL(mutex_lock_nested);

/*
 * 以 nest_lock 的 lockdep map 描述外层依赖，不可中断地获取 PREEMPT_RT mutex。
 * lock 是输入输出借用目标；nest_lock 是调用期间有效的只读借用 map，可让 lockdep 区分合法嵌套。
 * 入口可睡眠且不持 lock；返回无直接值，退出保证持锁，无参数 ownership 转移。
 */
void __sched _mutex_lock_nest_lock(struct mutex *lock,
				   struct lockdep_map *nest_lock)
{
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, 0, nest_lock, _RET_IP_);
}
EXPORT_SYMBOL_GPL(_mutex_lock_nest_lock);

/*
 * 按 lockdep subclass 可中断地获取 PREEMPT_RT mutex。
 * lock 是输入输出借用锁，subclass 是依赖子类；入口可睡眠且不持锁，不转移引用。
 * 返回 0 表示 current 持锁，-EINTR 表示任意信号中止且底层 waiter/lockdep 状态已撤销。
 */
int __sched mutex_lock_interruptible_nested(struct mutex *lock,
					    unsigned int subclass)
{
	return __mutex_lock_common(lock, TASK_INTERRUPTIBLE, subclass, NULL, _RET_IP_);
}
EXPORT_SYMBOL_GPL(mutex_lock_interruptible_nested);

/*
 * 以 TASK_KILLABLE 和可选 nest_lock/subclass 获取 PREEMPT_RT mutex。
 * lock 是输入输出借用锁；subclass 为 lockdep 子类；nest_lock 可空、为只读借用外层依赖 map。
 * 入口可睡眠且不持锁；返回 0 表示成功持锁，-EINTR 表示致命信号中止并已完整回滚，无 ownership 转移。
 */
int __sched _mutex_lock_killable(struct mutex *lock, unsigned int subclass,
				 struct lockdep_map *nest_lock)
{
	return __mutex_lock_common(lock, TASK_KILLABLE, subclass, nest_lock, _RET_IP_);
}
EXPORT_SYMBOL_GPL(_mutex_lock_killable);

/*
 * 以 I/O wait 记账不可中断地获取指定 subclass 的 PREEMPT_RT mutex。
 * lock 是输入输出借用锁，subclass 是 lockdep 子类；入口可睡眠且不持锁，返回无直接值并保证持锁。
 * prepare 的 token 保存 current 原 in_iowait 状态并刷新 blk plug，finish 在获取后精确恢复，支持嵌套。
 */
void __sched mutex_lock_io_nested(struct mutex *lock, unsigned int subclass)
{
	/* token 是 current->in_iowait 的旧布尔值，仅在本函数成对 prepare/finish 之间有效。 */
	int token;

	might_sleep();

	/* 阶段一标记 I/O 等待并刷新 plug；阶段二阻塞获取；阶段三无论等待多久都恢复原记账状态。 */
	token = io_schedule_prepare();
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, subclass, NULL, _RET_IP_);
	io_schedule_finish(token);
}
EXPORT_SYMBOL_GPL(mutex_lock_io_nested);

/*
 * 带可选嵌套 lockdep map 非阻塞尝试获取 PREEMPT_RT mutex。
 * lock 是输入输出借用锁；nest_lock 可空，是只读依赖 map。只能在线程上下文调用，可处于原子区，
 * 但禁止 hardirq/softirq；返回 1 表示 current 持锁、调用者负责解锁，0 表示竞争或调试发现上下文错误。
 * 函数不排队、不睡眠、不转移引用；成功后登记 trylock 类型 lockdep acquire。
 */
int __sched _mutex_trylock_nest_lock(struct mutex *lock,
				     struct lockdep_map *nest_lock)
{
	/* ret 是真实 owner 获取结果，也是公开布尔返回；只在成功时更新 dep_map。 */
	int ret;

	/* 调试 RT mutex 配置主动报告中断上下文误用；失败保持真实锁和 lockdep 都未获取。 */
	if (IS_ENABLED(CONFIG_DEBUG_RT_MUTEXES) && WARN_ON_ONCE(!in_task()))
		return 0;

	ret = __rt_mutex_trylock(&lock->rtmutex);
	if (ret)
		mutex_acquire_nest(&lock->dep_map, 0, 1, nest_lock, _RET_IP_);

	return ret;
}
EXPORT_SYMBOL_GPL(_mutex_trylock_nest_lock);
#else /* CONFIG_DEBUG_LOCK_ALLOC */
/* 关闭 lockdep 分配跟踪时导出不带 subclass/nest_lock 参数的精简 mutex 包装。 */

/*
 * 初始化关闭 lockdep 分配跟踪时的 PREEMPT_RT mutex 真实 rtmutex 状态。
 * mutex 是调用者拥有的输入输出对象，初始化前不得持有或并发可见；函数不睡眠、不转移引用。
 * 返回：无直接返回值；退出后 owner/等待树为空且 wait_lock 可用，dep_map 无需本配置的运行时初始化。
 */
void mutex_rt_init_generic(struct mutex *mutex)
{
	__mutex_rt_init_generic(mutex);
}
EXPORT_SYMBOL(mutex_rt_init_generic);

/*
 * 不可中断地获取关闭 lockdep 分配跟踪配置下的 PREEMPT_RT mutex。
 * lock 是输入输出借用对象；入口必须可睡眠且不持锁，无 ownership 转移。
 * 返回：无直接返回值；正常退出保证 current 持锁，竞争时底层通过 rtmutex 排队和 PI 后再返回。
 */
void __sched mutex_lock(struct mutex *lock)
{
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, 0, NULL, _RET_IP_);
}
EXPORT_SYMBOL(mutex_lock);

/*
 * 可被任意信号中断地获取 PREEMPT_RT mutex。
 * lock 是输入输出借用锁；入口为可睡眠任务上下文且不持锁。返回 0 表示 current 持锁，-EINTR 表示
 * 未获锁且 waiter/PI/lockdep 状态已回滚；不转移引用，失败后由调用者处理信号或重试。
 */
int __sched mutex_lock_interruptible(struct mutex *lock)
{
	return __mutex_lock_common(lock, TASK_INTERRUPTIBLE, 0, NULL, _RET_IP_);
}
EXPORT_SYMBOL(mutex_lock_interruptible);

/*
 * 只允许致命信号中断地获取 PREEMPT_RT mutex。
 * lock 是输入输出借用锁；入口可睡眠且不持锁。返回 0 表示成功并由调用者解锁，-EINTR 表示未获锁
 * 且内部排队已清理；TASK_KILLABLE 与 interruptible 版本仅在可响应信号集合上不同。
 */
int __sched mutex_lock_killable(struct mutex *lock)
{
	return __mutex_lock_common(lock, TASK_KILLABLE, 0, NULL, _RET_IP_);
}
EXPORT_SYMBOL(mutex_lock_killable);

/*
 * 以 I/O wait 记账不可中断地获取 PREEMPT_RT mutex。
 * lock 是输入输出借用锁；入口可睡眠且不持锁，返回无直接值并保证 current 持锁。
 * token 保存 current 原 in_iowait，prepare 刷新 blk plug并置位，获取完成后 finish 精确恢复旧状态。
 */
void __sched mutex_lock_io(struct mutex *lock)
{
	/* token 仅在本函数内跨越锁等待，确保嵌套 I/O 记账不会被无条件清零。 */
	int token = io_schedule_prepare();

	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, 0, NULL, _RET_IP_);
	io_schedule_finish(token);
}
EXPORT_SYMBOL(mutex_lock_io);

/*
 * 非阻塞尝试获取关闭 lockdep 分配跟踪配置下的 PREEMPT_RT mutex。
 * lock 是输入输出借用对象；只能在线程上下文调用，可位于原子区但禁止 hardirq/softirq。
 * 返回 1 表示 current 成为 owner且调用者负责解锁，0 表示竞争或调试发现上下文错误；不排队、不睡眠。
 */
int __sched mutex_trylock(struct mutex *lock)
{
	/* 调试构建拒绝非 task 上下文；普通构建仍要求调用者遵守同一 API 边界。 */
	if (IS_ENABLED(CONFIG_DEBUG_RT_MUTEXES) && WARN_ON_ONCE(!in_task()))
		return 0;

	return __rt_mutex_trylock(&lock->rtmutex);
}
EXPORT_SYMBOL(mutex_trylock);
#endif /* !CONFIG_DEBUG_LOCK_ALLOC */
/* 结束关闭/启用 lockdep 分配跟踪的两套 PREEMPT_RT mutex 包装选择。 */

/*
 * 释放 current 持有的 PREEMPT_RT mutex，并完成 PI deboost 与最高优先级 waiter 交接。
 * lock 是输入输出借用对象；入口要求 current 为 owner，函数不转移引用，禁止从中断上下文调用。
 * 返回：无直接返回值；退出时 lockdep 与真实 owner 均已释放，必要 waiter 已被唤醒。
 * 先记录 mutex_release，再调用 rtmutex slow/fast unlock；后者在恢复抢占时可能触发调度。
 */
void __sched mutex_unlock(struct mutex *lock)
	__releases(lock) __no_context_analysis
{
	/* 依赖模型先结束临界区，随后真实 rtmutex 以 release 语义清 owner或执行等待者交接。 */
	mutex_release(&lock->dep_map, _RET_IP_);
	__rt_mutex_unlock(&lock->rtmutex);
}
EXPORT_SYMBOL(mutex_unlock);

#endif /* CONFIG_PREEMPT_RT */
/* 结束仅在 PREEMPT_RT 中用 rtmutex 替代普通 mutex 实现的 API 区域。 */
