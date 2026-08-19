// SPDX-License-Identifier: GPL-2.0
/* kernel/rwsem.c: R/W semaphores, public implementation
 *
 * Written by David Howells (dhowells@redhat.com).
 * Derived from asm-i386/semaphore.h
 *
 * Writer lock-stealing by Alex Shi <alex.shi@intel.com>
 * and Michel Lespinasse <walken@google.com>
 *
 * Optimistic spinning by Tim Chen <tim.c.chen@intel.com>
 * and Davidlohr Bueso <davidlohr@hp.com>. Based on mutexes.
 *
 * Rwsem count bit fields re-definition and rwsem rearchitecture by
 * Waiman Long <longman@redhat.com> and
 * Peter Zijlstra <peterz@infradead.org>.
 */

/*
 * 本文件实现可睡眠读写信号量：非 PREEMPT_RT 使用 count 状态字、wait_list 与可选 OSQ 乐观自旋；
 * PREEMPT_RT 则把公共 API 适配到 rwbase_rt/rtmutex。读者可并发，writer 独占；所有慢路径都可能
 * 调度，因此公开阻塞入口只能在可睡眠上下文使用。文件头同时记录了 writer 偷锁、乐观自旋及 count
 * 位域重构的主要历史来源；作者与版权信息原样保留。
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/sched/task.h>
#include <linux/sched/debug.h>
#include <linux/sched/wake_q.h>
#include <linux/sched/signal.h>
#include <linux/sched/clock.h>
#include <linux/export.h>
#include <linux/rwsem.h>
#include <linux/atomic.h>
#include <linux/hung_task.h>
#include <trace/events/lock.h>

#ifndef CONFIG_PREEMPT_RT
#include "lock_events.h"

/*
 * The least significant 2 bits of the owner value has the following
 * meanings when set.
 *  - Bit 0: RWSEM_READER_OWNED - rwsem may be owned by readers (just a hint)
 *  - Bit 1: RWSEM_NONSPINNABLE - Cannot spin on a reader-owned lock
 *
 * When the rwsem is reader-owned and a spinning writer has timed out,
 * the nonspinnable bit will be set to disable optimistic spinning.

 * When a writer acquires a rwsem, it puts its task_struct pointer
 * into the owner field. It is cleared after an unlock.
 *
 * When a reader acquires a rwsem, it will also puts its task_struct
 * pointer into the owner field with the RWSEM_READER_OWNED bit set.
 * On unlock, the owner field will largely be left untouched. So
 * for a free or reader-owned rwsem, the owner value may contain
 * information about the last reader that acquires the rwsem.
 *
 * That information may be helpful in debugging cases where the system
 * seems to hang on a reader owned rwsem especially if only one reader
 * is involved. Ideally we would like to track all the readers that own
 * a rwsem, but the overhead is simply too big.
 *
 * A fast path reader optimistic lock stealing is supported when the rwsem
 * is previously owned by a writer and the following conditions are met:
 *  - rwsem is not currently writer owned
 *  - the handoff isn't set.
 */
/*
 * owner 低两位不是 task_struct 地址：bit0 仅提示“最近/可能由 reader 持有”，bit1 表示 reader owner
 * 不适合继续自旋。writer 获取时保存 current，释放后清零；reader 只留下最后获取者的调试线索，
 * 解锁通常不清理，所以绝不能把它当成稳定引用或真实 owner 解引用。若 writer 对 reader-owned 锁
 * 自旋超时则置 NONSPINNABLE；仅在没有 writer owner 且未 handoff 时允许 reader 快路径偷锁。
 * 跟踪全部 readers 成本过高，因此该字段只服务诊断与自旋决策，真正所有权由 count 决定。
 */
#define RWSEM_READER_OWNED	(1UL << 0)
#define RWSEM_NONSPINNABLE	(1UL << 1)
#define RWSEM_OWNER_FLAGS_MASK	(RWSEM_READER_OWNED | RWSEM_NONSPINNABLE)

#ifdef CONFIG_DEBUG_RWSEMS
# define DEBUG_RWSEMS_WARN_ON(c, sem)	do {			\
	if (!debug_locks_silent &&				\
	    WARN_ONCE(c, "DEBUG_RWSEMS_WARN_ON(%s): count = 0x%lx, magic = 0x%lx, owner = 0x%lx, curr 0x%lx, list %sempty\n",\
		#c, atomic_long_read(&(sem)->count),		\
		(unsigned long) sem->magic,			\
		atomic_long_read(&(sem)->owner), (long)current,	\
		rwsem_is_contended(sem) ? "" : "not "))		\
			debug_locks_off();			\
	} while (0)
#else
# define DEBUG_RWSEMS_WARN_ON(c, sem)
#endif
/*
 * DEBUG_RWSEMS_WARN_ON 在调试构建中报告 count/magic/owner/list 状态并关闭 debug_locks，关闭配置时
 * 完全消失；它只验证不变量，不参与锁的正确性或 ownership。
 */

/*
 * On 64-bit architectures, the bit definitions of the count are:
 *
 * Bit  0    - writer locked bit
 * Bit  1    - waiters present bit
 * Bit  2    - lock handoff bit
 * Bits 3-7  - reserved
 * Bits 8-62 - 55-bit reader count
 * Bit  63   - read fail bit
 *
 * On 32-bit architectures, the bit definitions of the count are:
 *
 * Bit  0    - writer locked bit
 * Bit  1    - waiters present bit
 * Bit  2    - lock handoff bit
 * Bits 3-7  - reserved
 * Bits 8-30 - 23-bit reader count
 * Bit  31   - read fail bit
 *
 * It is not likely that the most significant bit (read fail bit) will ever
 * be set. This guard bit is still checked anyway in the down_read() fastpath
 * just in case we need to use up more of the reader bits for other purpose
 * in the future.
 *
 * atomic_long_fetch_add() is used to obtain reader lock, whereas
 * atomic_long_cmpxchg() will be used to obtain writer lock.
 *
 * There are three places where the lock handoff bit may be set or cleared.
 * 1) rwsem_mark_wake() for readers		-- set, clear
 * 2) rwsem_try_write_lock() for writers	-- set, clear
 * 3) rwsem_del_waiter()			-- clear
 *
 * For all the above cases, wait_lock will be held. A writer must also
 * be the first one in the wait_list to be eligible for setting the handoff
 * bit. So concurrent setting/clearing of handoff bit is not possible.
 */
/*
 * count 的低位编码 writer、waiter 与强制 handoff，高位从 bit8 开始累计 reader，最高位作读失败
 * guard。读获取用 fetch-add，写获取用 cmpxchg；HANDOFF 只在 mark_wake、try_write_lock 和
 * del_waiter 三处、持 wait_lock 修改，并且只有队首 writer 可请求，因此不会并发互相覆盖。
 * 64 位提供 55 位 reader 计数，32 位提供 23 位；最高位现实中几乎不会溢出，但快路径仍把它纳入
 * 失败掩码，为将来缩减 reader 位数保留安全边界。
 */
#define RWSEM_WRITER_LOCKED	(1UL << 0)
#define RWSEM_FLAG_WAITERS	(1UL << 1)
#define RWSEM_FLAG_HANDOFF	(1UL << 2)
#define RWSEM_FLAG_READFAIL	(1UL << (BITS_PER_LONG - 1))

#define RWSEM_READER_SHIFT	8
#define RWSEM_READER_BIAS	(1UL << RWSEM_READER_SHIFT)
#define RWSEM_READER_MASK	(~(RWSEM_READER_BIAS - 1))
#define RWSEM_WRITER_MASK	RWSEM_WRITER_LOCKED
#define RWSEM_LOCK_MASK		(RWSEM_WRITER_MASK|RWSEM_READER_MASK)
#define RWSEM_READ_FAILED_MASK	(RWSEM_WRITER_MASK|RWSEM_FLAG_WAITERS|\
				 RWSEM_FLAG_HANDOFF|RWSEM_FLAG_READFAIL)

/*
 * All writes to owner are protected by WRITE_ONCE() to make sure that
 * store tearing can't happen as optimistic spinners may read and use
 * the owner value concurrently without lock. Read from owner, however,
 * may not need READ_ONCE() as long as the pointer value is only used
 * for comparison and isn't being dereferenced.
 *
 * Both rwsem_{set,clear}_owner() functions should be in the same
 * preempt disable section as the atomic op that changes sem->count.
 */
/*
 * owner 的无锁观察者可能并发读取，所有写入必须单次原子完成，避免指针撕裂；只用于比较时读取无需
 * 额外 READ_ONCE，若要解引用则必须依靠禁止抢占形成的 RCU 读侧窗口重新验证。owner 更新与 count
 * 所有权原子操作必须位于同一禁止抢占区，避免 current 被迁移/对象状态对观察者暂时不一致。
 */

/*
 * rwsem_set_owner() - 把当前 writer 记录为 sem 的 owner 提示。
 * @sem 是已由 current 成功取得写锁的非空借用对象；入口要求禁止抢占，不睡眠。返回无直接值，
 * owner 被设为 current，但不增加 task 引用；调用者继续持有真实 count 写锁并最终清除 owner。
 */
static inline void rwsem_set_owner(struct rw_semaphore *sem)
{
	lockdep_assert_preemption_disabled();
	atomic_long_set(&sem->owner, (long)current);
}

/*
 * rwsem_clear_owner() - 清除 writer owner 提示。
 * @sem 是 current 正在释放写锁的非空借用对象；入口禁止抢占，不睡眠。返回无直接值且不释放引用，
 * 只把 owner 置零；调用者随后以 release 语义清 WRITER_LOCKED，真正发布写临界区结果。
 */
static inline void rwsem_clear_owner(struct rw_semaphore *sem)
{
	lockdep_assert_preemption_disabled();
	atomic_long_set(&sem->owner, 0);
}

/*
 * Test the flags in the owner field.
 */
/* 检查 owner 字段中的提示标志，而非验证真实锁所有权。 */
/*
 * rwsem_test_oflags() - 测试 sem->owner 是否包含 @flags 中任一位。
 * @sem 为非空借用对象；@flags 是 RWSEM_READER_OWNED/NONSPINNABLE 掩码。无需持锁、不睡眠；返回
 * 布尔测试结果，不稳定快照只适合分支提示，函数不取得 task 引用或修改状态。
 */
static inline bool rwsem_test_oflags(struct rw_semaphore *sem, long flags)
{
	return atomic_long_read(&sem->owner) & flags;
}

/*
 * The task_struct pointer of the last owning reader will be left in
 * the owner field.
 *
 * Note that the owner value just indicates the task has owned the rwsem
 * previously, it may not be the real owner or one of the real owners
 * anymore when that field is examined, so take it with a grain of salt.
 *
 * The reader non-spinnable bit is preserved.
 */
/*
 * reader 获取后把最后 reader 的 task 指针留在 owner 中；以后观察时它可能已不是 owner，甚至没有
 * 任何 reader，因此只能作为调试线索。更新时保留 NONSPINNABLE，使本轮 reader phase 的自旋退化
 * 决策不会被新 reader 覆盖。
 */
/*
 * __rwsem_set_reader_owned() - 写入 reader-owned 提示及可选 reader 身份。
 * @sem 为非空借用 rwsem；@owner 为可空借用 task，NULL 用于 non-owner API。调用者保证对象有效，
 * 函数不睡眠、不持有 task 引用；返回无直接值，保留既有 NONSPINNABLE 并置 READER_OWNED。
 */
static inline void __rwsem_set_reader_owned(struct rw_semaphore *sem,
					    struct task_struct *owner)
{
	unsigned long val = (unsigned long)owner | RWSEM_READER_OWNED |
		(atomic_long_read(&sem->owner) & RWSEM_NONSPINNABLE);

	atomic_long_set(&sem->owner, val);
}

/*
 * rwsem_set_reader_owned() - 以 current 作为最近 reader 更新 owner 提示。
 * @sem 为已由 current 获得读锁的非空借用对象；调用点位于禁止抢占区，不睡眠。返回无直接值，
 * 只委托内部 helper 写提示，不增加 current 引用，真实 reader 计数仍在 sem->count。
 */
static inline void rwsem_set_reader_owned(struct rw_semaphore *sem)
{
	__rwsem_set_reader_owned(sem, current);
}

#if defined(CONFIG_DEBUG_RWSEMS) || defined(CONFIG_DETECT_HUNG_TASK_BLOCKER)
/*
 * Return just the real task structure pointer of the owner
 */
/* 返回 owner 中去掉低位标志后的 task 指针；该指针仍只是未持引用的诊断快照。 */
/*
 * rwsem_owner() - 取出不含提示位的 owner task 地址。
 * @sem 为非空借用对象；无需持锁且不睡眠。返回可为 NULL 的裸指针，不增加引用、不能据此证明真实
 * ownership，解引用者必须另有 RCU/锁生命周期保护；函数无副作用。
 */
struct task_struct *rwsem_owner(struct rw_semaphore *sem)
{
	return (struct task_struct *)
		(atomic_long_read(&sem->owner) & ~RWSEM_OWNER_FLAGS_MASK);
}

/*
 * Return true if the rwsem is owned by a reader.
 */
/* count 若已标记 writer 则 reader 提示必不代表当前所有权；否则返回提示位作为近似诊断结果。 */
/*
 * is_rwsem_reader_owned() - 判断 sem 当前是否可视为 reader-owned。
 * @sem 为非空借用对象；无锁、不睡眠，返回 false 表示 writer-locked 或无 reader 提示，true 表示
 * count 非写锁且 owner 带 READER_OWNED。结果是瞬时诊断值，不冻结状态、不转移 ownership。
 */
bool is_rwsem_reader_owned(struct rw_semaphore *sem)
{
	/*
	 * Check the count to see if it is write-locked.
	 */
	/* 先查权威 count；写锁存在时即使陈旧 owner 仍带 READER_OWNED 也必须返回 false。 */
	long count = atomic_long_read(&sem->count);

	if (count & RWSEM_WRITER_MASK)
		return false;
	return rwsem_test_oflags(sem, RWSEM_READER_OWNED);
}

/*
 * With CONFIG_DEBUG_RWSEMS or CONFIG_DETECT_HUNG_TASK_BLOCKER configured,
 * it will make sure that the owner field of a reader-owned rwsem either
 * points to a real reader-owner(s) or gets cleared. The only exception is
 * when the unlock is done by up_read_non_owner().
 */
/*
 * 调试或 hung-task blocker 配置下，普通 reader 解锁会在 owner 仍指向 current 时清除裸 task 地址，
 * 避免诊断保留明显过期的 reader；non-owner 解锁无法按 current 匹配，是唯一例外。
 */
/*
 * rwsem_clear_reader_owned() - 清除指向 current 的最后 reader 地址，保留低位提示。
 * @sem 为 current 正在释放读锁的非空借用对象；无需外部锁、不睡眠。CAS 循环仅在地址仍等于 current
 * 时写回标志位，若并发新 reader 改写 owner 则停止；返回无直接值，不修改真实 count 或 task 引用。
 */
static inline void rwsem_clear_reader_owned(struct rw_semaphore *sem)
{
	unsigned long val = atomic_long_read(&sem->owner);

	while ((val & ~RWSEM_OWNER_FLAGS_MASK) == (unsigned long)current) {
		if (atomic_long_try_cmpxchg(&sem->owner, &val,
					    val & RWSEM_OWNER_FLAGS_MASK))
			return;
	}
}
#else
/*
 * rwsem_clear_reader_owned() - 无调试/阻塞者检测配置下的零开销空实现。
 * @sem 仅为保持统一调用接口的借用参数，不读取、不修改；不睡眠，返回无直接值。
 */
static inline void rwsem_clear_reader_owned(struct rw_semaphore *sem)
{
}
#endif

/*
 * Set the RWSEM_NONSPINNABLE bits if the RWSEM_READER_OWNED flag
 * remains set. Otherwise, the operation will be aborted.
 */
/* 仅在 owner 仍表示 reader phase 时置 NONSPINNABLE；owner 已切换或标志已置时立即放弃。 */
/*
 * rwsem_set_nonspinnable() - 原子地禁止当前 reader-owned 阶段继续乐观自旋。
 * @sem 为非空借用对象；无锁、不睡眠。owner 是 CAS 回填快照；成功只增加 NONSPINNABLE，若 reader
 * 提示消失或已置位则不修改。返回无直接值，不改变 count/真实 ownership。
 */
static inline void rwsem_set_nonspinnable(struct rw_semaphore *sem)
{
	unsigned long owner = atomic_long_read(&sem->owner);

	do {
		if (!(owner & RWSEM_READER_OWNED))
			break;
		if (owner & RWSEM_NONSPINNABLE)
			break;
	} while (!atomic_long_try_cmpxchg(&sem->owner, &owner,
					  owner | RWSEM_NONSPINNABLE));
}

/*
 * rwsem_read_trylock() - 先把 reader bias 加入 count，并判断能否在快路径保留它。
 * @sem 为非空借用输入输出锁；@cntp 为非空输出指针，返回时保存加一后的 count，即使获取失败也供
 * 慢路径解释/回滚。入口禁止抢占，不睡眠。返回 true 表示 acquire 读持有并更新 reader owner；false
 * 表示遇到 writer/waiter/handoff/readfail，已加的 bias 尚未撤销，调用者必须进入读慢路径处理。
 */
static inline bool rwsem_read_trylock(struct rw_semaphore *sem, long *cntp)
{
	*cntp = atomic_long_add_return_acquire(RWSEM_READER_BIAS, &sem->count);

	if (WARN_ON_ONCE(*cntp < 0))
		rwsem_set_nonspinnable(sem);

	if (!(*cntp & RWSEM_READ_FAILED_MASK)) {
		rwsem_set_reader_owned(sem);
		return true;
	}

	return false;
}

/*
 * rwsem_write_trylock() - 仅在 count 完全空闲时以 CAS 取得 writer 位。
 * @sem 为非空借用输入输出锁；入口禁止抢占，不睡眠。tmp 既是期望的 UNLOCKED 值又接收失败快照。
 * 返回 true 表示以 acquire 语义持有写锁并记录 current owner；false 表示状态不变，调用者可转慢路径。
 */
static inline bool rwsem_write_trylock(struct rw_semaphore *sem)
{
	long tmp = RWSEM_UNLOCKED_VALUE;

	if (atomic_long_try_cmpxchg_acquire(&sem->count, &tmp, RWSEM_WRITER_LOCKED)) {
		rwsem_set_owner(sem);
		return true;
	}

	return false;
}

/*
 * Return the real task structure pointer of the owner and the embedded
 * flags in the owner. pflags must be non-NULL.
 */
/* 返回 owner 的裸 task 地址并通过非空 pflags 输出低位提示；两者来自同一次原子快照。 */
/*
 * rwsem_owner_flags() - 拆分 owner 快照中的 task 地址和提示标志。
 * @sem 为非空借用对象；@pflags 为非空输出指针，写入 flags。无需持锁、不睡眠；返回可空、未持引用
 * 的 task 裸指针，只能在调用者已有 RCU/禁止抢占保护并重新验证时解引用；函数无状态副作用。
 */
static inline struct task_struct *
rwsem_owner_flags(struct rw_semaphore *sem, unsigned long *pflags)
{
	unsigned long owner = atomic_long_read(&sem->owner);

	*pflags = owner & RWSEM_OWNER_FLAGS_MASK;
	return (struct task_struct *)(owner & ~RWSEM_OWNER_FLAGS_MASK);
}

/*
 * Guide to the rw_semaphore's count field.
 *
 * When the RWSEM_WRITER_LOCKED bit in count is set, the lock is owned
 * by a writer.
 *
 * The lock is owned by readers when
 * (1) the RWSEM_WRITER_LOCKED isn't set in count,
 * (2) some of the reader bits are set in count, and
 * (3) the owner field has RWSEM_READ_OWNED bit set.
 *
 * Having some reader bits set is not enough to guarantee a readers owned
 * lock as the readers may be in the process of backing out from the count
 * and a writer has just released the lock. So another writer may steal
 * the lock immediately after that.
 */
/*
 * count 中 WRITER_LOCKED 是权威写持有；reader-owned 需同时满足无 writer 位、reader 计数非零和 owner
 * 提示位。仅有 reader 位仍不足以证明当前由 reader 持有，因为失败 reader 可能正在撤销已预加计数，
 * writer 刚释放后另一 writer 也可能立即偷锁；判断必须结合完整状态与同步窗口。
 */
/* 修正说明：原文的 RWSEM_READ_OWNED 在当前文件实际宏名为 RWSEM_READER_OWNED。 */

/*
 * Initialize an rwsem:
 */
/* 初始化一个 rwsem；只能在对象尚未并发可见或已完全停用时调用。 */
/*
 * __init_rwsem() - 初始化非 RT rwsem 的状态、等待队列、调试信息与可选 OSQ。
 * @sem 为非空输出对象，调用者拥有其存储；@name 为 lockdep 使用的稳定借用名称；@key 为稳定借用
 * 锁类 key。入口不得有并发使用者，不睡眠。返回无直接值；count/owner 清零，wait queue 为空，
 * first_waiter 为 NULL，按配置初始化 magic、dep_map 与 osq；不分配资源，生命周期仍由调用者管理。
 */
void __init_rwsem(struct rw_semaphore *sem, const char *name,
		  struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	/*
	 * Make sure we are not reinitializing a held semaphore:
	 */
	/* 调试构建先确认该内存范围没有已登记持锁对象，防止重初始化仍在使用的信号量。 */
	debug_check_no_locks_freed((void *)sem, sizeof(*sem));
	lockdep_init_map_wait(&sem->dep_map, name, key, 0, LD_WAIT_SLEEP);
#endif
#ifdef CONFIG_DEBUG_RWSEMS
	sem->magic = sem;
#endif
	atomic_long_set(&sem->count, RWSEM_UNLOCKED_VALUE);
	atomic_long_set(&sem->owner, 0L);
	/* scoped_guard 离开花括号时自动释放 wait_lock，保证 first_waiter 初始化与并发队列操作使用同一锁。 */
	scoped_guard (raw_spinlock_init, &sem->wait_lock) {
		sem->first_waiter = NULL;
	}
#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
	osq_lock_init(&sem->osq);
#endif
}
EXPORT_SYMBOL(__init_rwsem);

enum rwsem_waiter_type {
	/* waiter 请求独占写或共享读；类型决定队首唤醒/批量授予策略。 */
	RWSEM_WAITING_FOR_WRITE,
	RWSEM_WAITING_FOR_READ
};

/*
 * 一个栈上 rwsem 慢路径等待者：list 接入以 first_waiter 为锚的环形队列；task 是等待任务的借用指针，
 * 被授予的 reader 通过 release 写 NULL 发布成功；type 区分读写；timeout 是允许请求 handoff 的
 * jiffies 截止点；handoff_set 记录此 waiter/队首是否已强制交接。对象由阻塞函数创建，出队后失效。
 */
struct rwsem_waiter {
	struct list_head list;
	struct task_struct *task;
	enum rwsem_waiter_type type;
	unsigned long timeout;
	bool handoff_set;
};

enum rwsem_wake_type {
	RWSEM_WAKE_ANY,		/* Wake whatever's at head of wait list */
	RWSEM_WAKE_READERS,	/* Wake readers only */
	RWSEM_WAKE_READ_OWNED	/* Waker thread holds the read lock */
};
/* 依次表示唤醒队首任意类型、仅队首 reader phase、以及唤醒者本身已持读锁无需首个 reader 授予。 */

/*
 * The typical HZ value is either 250 or 1000. So set the minimum waiting
 * time to at least 4ms or 1 jiffy (if it is higher than 4ms) in the wait
 * queue before initiating the handoff protocol.
 */
/* 常见 HZ 为 250/1000，至少等待约 4ms（或一个更长 jiffy）后才请求强制 handoff，兼顾吞吐与饥饿。 */
#define RWSEM_WAIT_TIMEOUT	DIV_ROUND_UP(HZ, 250)

/*
 * Magic number to batch-wakeup waiting readers, even when writers are
 * also present in the queue. This both limits the amount of work the
 * waking thread must do and also prevents any potential counter overflow,
 * however unlikely.
 */
/* 单次最多批授予 256 个 readers，限制持 wait_lock 工作量并防止极端 reader count 溢出。 */
#define MAX_READERS_WAKEUP	0x100

/*
 * __rwsem_del_waiter() - 在 wait_lock 下从环形等待队列摘除一个 waiter。
 * @sem 为非空借用输入输出锁；@waiter 为仍有效的栈上借用节点。入口必须持 wait_lock，不睡眠。
 * 若节点是唯一 waiter，清 first_waiter 并返回 false；否则必要时推进 first_waiter、摘链并返回 true。
 * 本函数不清 count flags、不唤醒、不管理 task 引用，调用者承担这些后续责任。
 */
static inline
bool __rwsem_del_waiter(struct rw_semaphore *sem, struct rwsem_waiter *waiter)
	__must_hold(&sem->wait_lock)
{
	if (list_empty(&waiter->list)) {
		sem->first_waiter = NULL;
		return false;
	}

	if (sem->first_waiter == waiter) {
		sem->first_waiter = list_first_entry(&waiter->list,
						     struct rwsem_waiter, list);
	}
	list_del(&waiter->list);

	return true;
}

/*
 * Remove a waiter from the wait_list and clear flags.
 *
 * Both rwsem_mark_wake() and rwsem_try_write_lock() contain a full 'copy' of
 * this function. Modify with care.
 *
 * Return: true if wait_list isn't empty and false otherwise
 */
/*
 * 从 wait_list 删除 waiter；mark_wake 与 try_write_lock 内各复制了完整语义，修改时必须同步审计三处。
 * 返回 true 表示删除后仍有等待者，false 表示队列已空并已清 HANDOFF/WAITERS。
 */
/*
 * rwsem_del_waiter() - 摘除 waiter，并在队列清空时同步清 count 等待标志。
 * @sem/@waiter 均为借用输入输出对象；入口持 wait_lock，不睡眠。返回是否仍有 waiter；不直接唤醒，
 * 不释放栈节点或 task 引用。调用者依据返回值决定是否 mark_wake。
 */
static inline bool
rwsem_del_waiter(struct rw_semaphore *sem, struct rwsem_waiter *waiter)
{
	lockdep_assert_held(&sem->wait_lock);
	if (__rwsem_del_waiter(sem, waiter))
		return true;
	atomic_long_andnot(RWSEM_FLAG_HANDOFF | RWSEM_FLAG_WAITERS, &sem->count);
	return false;
}

/*
 * next_waiter() - 在以 first_waiter 为边界的环形链表中取得 waiter 的逻辑后继。
 * @sem 为非空借用队列；@waiter 为已链接的借用节点；入口持 wait_lock、不睡眠。返回下一 waiter，
 * 若物理 next 回绕到 first_waiter 则返回 NULL 表示逻辑队尾；无修改、引用或 ownership 转移。
 */
static inline
struct rwsem_waiter *next_waiter(const struct rw_semaphore *sem,
				 const struct rwsem_waiter *waiter)
	__must_hold(&sem->wait_lock)
{
	struct rwsem_waiter *next = list_first_entry(&waiter->list,
						     struct rwsem_waiter, list);
	if (next == sem->first_waiter)
		return NULL;
	return next;
}

/*
 * handle the lock release when processes blocked on it that can now run
 * - if we come here from up_xxxx(), then the RWSEM_FLAG_WAITERS bit must
 *   have been set.
 * - there must be someone on the queue
 * - the wait_lock must be held by the caller
 * - tasks are marked for wakeup, the caller must later invoke wake_up_q()
 *   to actually wakeup the blocked task(s) and drop the reference count,
 *   preferably when the wait_lock is released
 * - woken process blocks are discarded from the list after having task zeroed
 * - writers are only marked woken if downgrading is false
 *
 * Implies rwsem_del_waiter() for all woken readers.
 */
/*
 * 处理释放后的延迟唤醒：调用者持 wait_lock，确认 WAITERS 且队列非空；函数只把任务加入 wake_q，
 * 真正 wake_up_q 与引用消费必须在放锁后进行。reader 被授予时从队列摘除并把 task 清 NULL；writer
 * 仅在 WAKE_ANY 时加入唤醒队列，降级场景不唤醒 writer。所有被授予 readers 的效果等同逐个执行
 * rwsem_del_waiter()，但为保证计数先于任务运行而批量实现。
 */
/*
 * rwsem_mark_wake() - 按队首阶段授予并收集可唤醒的 rwsem waiter。
 * @sem 为非空借用输入输出锁；@wake_type 指定任意/reader-only/唤醒者已读持有；@wake_q 为非空输入
 * 输出延迟唤醒队列。入口必须持 wait_lock、不可睡眠。返回无直接值；可能更新 count/owner/first_waiter、
 * 摘除最多 MAX_READERS_WAKEUP 个 reader 并取得 task 临时引用，引用由调用者后续 wake_up_q 消费。
 * writer 仅排队唤醒而不预授予，reader 则先完整计数再发布 task==NULL，避免早退 reader 先解锁。
 */
static void rwsem_mark_wake(struct rw_semaphore *sem,
			    enum rwsem_wake_type wake_type,
			    struct wake_q_head *wake_q)
{
	struct rwsem_waiter *waiter, *next;
	long oldcount, woken = 0, adjustment = 0;
	struct list_head wlist;

	lockdep_assert_held(&sem->wait_lock);

	/*
	 * Take a peek at the queue head waiter such that we can determine
	 * the wakeup(s) to perform.
	 */
	/* 先查看逻辑队首，决定当前 phase 是单 writer 还是可批量 reader。 */
	waiter = sem->first_waiter;

	if (waiter->type == RWSEM_WAITING_FOR_WRITE) {
		if (wake_type == RWSEM_WAKE_ANY) {
			/*
			 * Mark writer at the front of the queue for wakeup.
			 * Until the task is actually later awoken later by
			 * the caller, other writers are able to steal it.
			 * Readers, on the other hand, will block as they
			 * will notice the queued writer.
			 */
			/*
			 * WAKE_ANY 时仅把队首 writer 标为待唤醒；实际唤醒前其他 writer 仍可能偷锁，而 reader
			 * 会看到队列中的 writer/WAITERS 而阻塞。其他 wake_type 不越过队首 writer。
			 */
			wake_q_add(wake_q, waiter->task);
			lockevent_inc(rwsem_wake_writer);
		}

		return;
	}

	/*
	 * No reader wakeup if there are too many of them already.
	 */
	/* count 为负说明 reader 计数已触及 guard/溢出边界，不能再批量增加。 */
	if (unlikely(atomic_long_read(&sem->count) < 0))
		return;

	/*
	 * Writers might steal the lock before we grant it to the next reader.
	 * We prefer to do the first reader grant before counting readers
	 * so we can bail out early if a writer stole the lock.
	 */
	/*
	 * 若唤醒者尚未持读锁，先只为首个 reader 加一次 bias 并检查 writer 是否已偷锁；先授予后统计
	 * 其余 readers，才能在 writer 抢到时立即撤销，避免虚增整批计数。
	 */
	if (wake_type != RWSEM_WAKE_READ_OWNED) {
		struct task_struct *owner;

		adjustment = RWSEM_READER_BIAS;
		oldcount = atomic_long_fetch_add(adjustment, &sem->count);
		if (unlikely(oldcount & RWSEM_WRITER_MASK)) {
			/*
			 * When we've been waiting "too" long (for writers
			 * to give up the lock), request a HANDOFF to
			 * force the issue.
			 */
			/* 首 reader 与 writer 冲突且等待超时后请求 HANDOFF，迫使后续偷锁者让出；本轮撤销预加 bias。 */
			if (time_after(jiffies, waiter->timeout)) {
				if (!(oldcount & RWSEM_FLAG_HANDOFF)) {
					adjustment -= RWSEM_FLAG_HANDOFF;
					lockevent_inc(rwsem_rlock_handoff);
				}
				waiter->handoff_set = true;
			}

			atomic_long_add(-adjustment, &sem->count);
			return;
		}
		/*
		 * Set it to reader-owned to give spinners an early
		 * indication that readers now have the lock.
		 * The reader nonspinnable bit seen at slowpath entry of
		 * the reader is copied over.
		 */
		/* 首 reader 已获授予，先发布 reader-owned 提示，并保留慢路径入口观察到的 NONSPINNABLE。 */
		owner = waiter->task;
		__rwsem_set_reader_owned(sem, owner);
	}

	/*
	 * Grant up to MAX_READERS_WAKEUP read locks to all the readers in the
	 * queue. We know that the woken will be at least 1 as we accounted
	 * for above. Note we increment the 'active part' of the count by the
	 * number of readers before waking any processes up.
	 *
	 * This is an adaptation of the phase-fair R/W locks where at the
	 * reader phase (first waiter is a reader), all readers are eligible
	 * to acquire the lock at the same time irrespective of their order
	 * in the queue. The writers acquire the lock according to their
	 * order in the queue.
	 *
	 * We have to do wakeup in 2 passes to prevent the possibility that
	 * the reader count may be decremented before it is incremented. It
	 * is because the to-be-woken waiter may not have slept yet. So it
	 * may see waiter->task got cleared, finish its critical section and
	 * do an unlock before the reader count increment.
	 *
	 * 1) Collect the read-waiters in a separate list, count them and
	 *    fully increment the reader count in rwsem.
	 * 2) For each waiters in the new list, clear waiter->task and
	 *    put them into wake_q to be woken up later.
	 */
	/*
	 * reader phase 最多批量授予 256 个 reader，即便其间夹有 writer 也跳过 writer 继续收集 readers；
	 * writer 仍按队列次序争用。必须两遍：第一遍搬到临时表并一次性增加全部 reader count；第二遍才
	 * 清 task 并排队唤醒。待唤醒任务可能尚未真正睡下，若先清 task，它可立即完成临界区并 up_read，
	 * 造成“先减后加”破坏计数。
	 */
	INIT_LIST_HEAD(&wlist);
	do {
		next = next_waiter(sem, waiter);
		if (waiter->type == RWSEM_WAITING_FOR_WRITE)
			continue;

		woken++;
		list_move_tail(&waiter->list, &wlist);
		if (sem->first_waiter == waiter)
			sem->first_waiter = next;

		/*
		 * Limit # of readers that can be woken up per wakeup call.
		 */
		/* 限制单轮锁内扫描/唤醒规模，并给剩余 waiter 留待后续轮次。 */
		if (unlikely(woken >= MAX_READERS_WAKEUP))
			break;
	} while ((waiter = next) != NULL);

	adjustment = woken * RWSEM_READER_BIAS - adjustment;
	lockevent_cond_inc(rwsem_wake_reader, woken);

	oldcount = atomic_long_read(&sem->count);
	if (!sem->first_waiter) {
		/*
		 * Combined with list_move_tail() above, this implies
		 * rwsem_del_waiter().
		 */
		/* 临时搬链后 first_waiter 为空等价于逐个删除完队列，需同步清 WAITERS/HANDOFF。 */
		adjustment -= RWSEM_FLAG_WAITERS;
		if (oldcount & RWSEM_FLAG_HANDOFF)
			adjustment -= RWSEM_FLAG_HANDOFF;
	} else if (woken) {
		/*
		 * When we've woken a reader, we no longer need to force
		 * writers to give up the lock and we can clear HANDOFF.
		 */
		/* 已成功进入 reader phase 后不再需要强迫 writer 交接，清掉 HANDOFF 重新按队列协议推进。 */
		if (oldcount & RWSEM_FLAG_HANDOFF)
			adjustment -= RWSEM_FLAG_HANDOFF;
	}

	if (adjustment)
		atomic_long_add(adjustment, &sem->count);

	/* 2nd pass */
	/* 第二遍在 reader count 已完全发布后，逐个取得 task 引用、发布授予并加入锁外 wake_q。 */
	list_for_each_entry_safe(waiter, next, &wlist, list) {
		struct task_struct *tsk;

		tsk = waiter->task;
		get_task_struct(tsk);

		/*
		 * Ensure calling get_task_struct() before setting the reader
		 * waiter to nil such that rwsem_down_read_slowpath() cannot
		 * race with do_exit() by always holding a reference count
		 * to the task to wakeup.
		 */
		/*
		 * 必须先 get_task_struct() 再以 release 把 waiter->task 清 NULL；慢路径 reader 可能看到 NULL
		 * 后立即返回甚至 do_exit，临时引用保证 wake_q 中 task 直到实际唤醒仍存活。
		 */
		smp_store_release(&waiter->task, NULL);
		/*
		 * Ensure issuing the wakeup (either by us or someone else)
		 * after setting the reader waiter to nil.
		 */
		/* wake_q_add_safe 的唤醒严格发生在 task=NULL 发布之后，与 waiter 的 acquire 读取配对。 */
		wake_q_add_safe(wake_q, tsk);
	}
}

/*
 * Remove a waiter and try to wake up other waiters in the wait queue
 * This function is called from the out_nolock path of both the reader and
 * writer slowpaths with wait_lock held. It releases the wait_lock and
 * optionally wake up waiters before it returns.
 */
/*
 * 读/写慢路径的取消出口在持 wait_lock 时调用：删除自身，若原为队首则重新评估其余 waiter；函数
 * 自行释放 wait_lock，并在锁外可选唤醒。调用后 waiter 已不可再按在队状态使用。
 */
/*
 * rwsem_del_wake_waiter() - 取消当前 waiter 并完成必要的后继唤醒与放锁。
 * @sem/@waiter 为借用输入输出对象；@wake_q 为非空临时队列。入口持 wait_lock，退出不持；函数不
 * 睡眠但可唤醒任务。返回无直接值；若删除的是队首且队列仍非空，mark_wake 选择新队首 phase，随后
 * 锁外消费 wake_q。栈 waiter ownership 仍归调用慢路径。
 */
static inline void
rwsem_del_wake_waiter(struct rw_semaphore *sem, struct rwsem_waiter *waiter,
		      struct wake_q_head *wake_q)
		      __releases(&sem->wait_lock)
{
	bool first = sem->first_waiter == waiter;

	wake_q_init(wake_q);

	/*
	 * If the wait_list isn't empty and the waiter to be deleted is
	 * the first waiter, we wake up the remaining waiters as they may
	 * be eligible to acquire or spin on the lock.
	 */
	/* 只有删除逻辑队首才会改变谁有资格获取/自旋，因此仅该情况主动重评估并唤醒。 */
	if (rwsem_del_waiter(sem, waiter) && first)
		rwsem_mark_wake(sem, RWSEM_WAKE_ANY, wake_q);
	raw_spin_unlock_irq(&sem->wait_lock);
	if (!wake_q_empty(wake_q))
		wake_up_q(wake_q);
}

/*
 * This function must be called with the sem->wait_lock held to prevent
 * race conditions between checking the rwsem wait list and setting the
 * sem->count accordingly.
 *
 * Implies rwsem_del_waiter() on success.
 */
/*
 * 必须持 wait_lock 才能把“检查队列顺序、设置 HANDOFF、CAS count、成功摘除”作为一个协议。成功
 * 完全蕴含 rwsem_del_waiter；失败可能仅请求 handoff，waiter 仍在队列。
 */
/*
 * rwsem_try_write_lock() - 让等待 writer 尝试取得写锁或在饥饿时请求强制 handoff。
 * @sem 为非空借用输入输出锁；@waiter 为已入队的 writer 借用节点。入口持 wait_lock、不睡眠。
 * 返回 true 表示 acquire 写锁、清 handoff、摘除 waiter 并记录 owner；false 表示未获锁，可能把
 * HANDOFF 与队首 handoff_set 置位。first/count/new 是锁内稳定队首与 CAS 新旧快照，无引用转移。
 */
static inline bool rwsem_try_write_lock(struct rw_semaphore *sem,
					struct rwsem_waiter *waiter)
	__must_hold(&sem->wait_lock)
{
	struct rwsem_waiter *first = sem->first_waiter;
	long count, new;

	lockdep_assert_held(&sem->wait_lock);

	count = atomic_long_read(&sem->count);
	do {
		bool has_handoff = !!(count & RWSEM_FLAG_HANDOFF);

		if (has_handoff) {
			/*
			 * Honor handoff bit and yield only when the first
			 * waiter is the one that set it. Otherwisee, we
			 * still try to acquire the rwsem.
			 */
			/* 只有实际设置 handoff 的队首 waiter 能阻止其他 writer；否则仍允许尝试，避免陈旧位停滞。 */
			if (first->handoff_set && (waiter != first))
				return false;
		}

		new = count;

		if (count & RWSEM_LOCK_MASK) {
			/*
			 * A waiter (first or not) can set the handoff bit
			 * if it is an RT task or wait in the wait queue
			 * for too long.
			 */
			/* 锁仍忙时，RT/DL writer 或超时 waiter 可首次置 HANDOFF；已有 handoff 则只等待交接。 */
			if (has_handoff || (!rt_or_dl_task(waiter->task) &&
					    !time_after(jiffies, waiter->timeout)))
				return false;

			new |= RWSEM_FLAG_HANDOFF;
		} else {
			new |= RWSEM_WRITER_LOCKED;
			new &= ~RWSEM_FLAG_HANDOFF;

			if (list_empty(&first->list))
				new &= ~RWSEM_FLAG_WAITERS;
		}
	} while (!atomic_long_try_cmpxchg_acquire(&sem->count, &count, new));

	/*
	 * We have either acquired the lock with handoff bit cleared or set
	 * the handoff bit. Only the first waiter can have its handoff_set
	 * set here to enable optimistic spinning in slowpath loop.
	 */
	/* CAS 后要么成功获取并清 handoff，要么只发布 handoff；仅队首记录 handoff_set 并可在慢循环自旋。 */
	if (new & RWSEM_FLAG_HANDOFF) {
		first->handoff_set = true;
		lockevent_inc(rwsem_wlock_handoff);
		return false;
	}

	/*
	 * Have rwsem_try_write_lock() fully imply rwsem_del_waiter() on
	 * success.
	 */
	/* 成功路径在 wait_lock 下直接完成摘链/推进 first_waiter，避免再做一次 count flag 更新。 */
	__rwsem_del_waiter(sem, waiter);

	rwsem_set_owner(sem);
	return true;
}

/*
 * The rwsem_spin_on_owner() function returns the following 4 values
 * depending on the lock owner state.
 *   OWNER_NULL  : owner is currently NULL
 *   OWNER_WRITER: when owner changes and is a writer
 *   OWNER_READER: when owner changes and the new owner may be a reader.
 *   OWNER_NONSPINNABLE:
 *		   when optimistic spinning has to stop because either the
 *		   owner stops running, is unknown, or its timeslice has
 *		   been used up.
 */
/* rwsem_spin_on_owner() 以四种状态汇报 owner 为空、writer、reader 或必须停止自旋，供外层决定重试。 */
enum owner_state {
	OWNER_NULL		= 1 << 0,
	OWNER_WRITER		= 1 << 1,
	OWNER_READER		= 1 << 2,
	OWNER_NONSPINNABLE	= 1 << 3,
};

#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
/*
 * Try to acquire write lock before the writer has been put on wait queue.
 */
/* 在 writer 尚未入 wait_list 前尝试偷取完全空闲且无 handoff 的锁。 */
/*
 * rwsem_try_write_lock_unqueued() - 无排队地 CAS 获取 writer 位。
 * @sem 为非空借用输入输出锁；调用者持 OSQ、禁止抢占，不持 wait_lock，函数不睡眠。count 是 CAS
 * 回填快照；返回 true 表示 acquire 写锁并记录 current owner，false 表示锁忙或 handoff，状态不变。
 */
static inline bool rwsem_try_write_lock_unqueued(struct rw_semaphore *sem)
{
	long count = atomic_long_read(&sem->count);

	while (!(count & (RWSEM_LOCK_MASK|RWSEM_FLAG_HANDOFF))) {
		if (atomic_long_try_cmpxchg_acquire(&sem->count, &count,
					count | RWSEM_WRITER_LOCKED)) {
			rwsem_set_owner(sem);
			lockevent_inc(rwsem_opt_lock);
			return true;
		}
	}
	return false;
}

/*
 * rwsem_can_spin_on_owner() - 在进入 OSQ 前快速判断 owner 是否值得自旋。
 * @sem 为非空借用锁；调用者禁止抢占，形成 RCU 读侧生命周期窗口，函数不睡眠。返回 false 表示需要
 * 调度、reader phase 已禁自旋或明确 writer 已不在 CPU；true 只表示可以尝试，不保证最终获取。
 * owner/flags 是无引用快照，reader owner 可能陈旧，故不对它调用 owner_on_cpu()。
 */
static inline bool rwsem_can_spin_on_owner(struct rw_semaphore *sem)
{
	struct task_struct *owner;
	unsigned long flags;
	bool ret = true;

	if (need_resched()) {
		lockevent_inc(rwsem_opt_fail);
		return false;
	}

	/*
	 * Disable preemption is equal to the RCU read-side crital section,
	 * thus the task_strcut structure won't go away.
	 */
	/* 禁止抢占等价于 RCU 读侧临界区，保证重新验证后的 writer task_struct 暂不回收。 */
	owner = rwsem_owner_flags(sem, &flags);
	/*
	 * Don't check the read-owner as the entry may be stale.
	 */
	/* reader 地址只是历史提示，可能陈旧；仅明确 writer owner 才检查是否仍在 CPU。 */
	if ((flags & RWSEM_NONSPINNABLE) ||
	    (owner && !(flags & RWSEM_READER_OWNED) && !owner_on_cpu(owner)))
		ret = false;

	lockevent_cond_inc(rwsem_opt_fail, !ret);
	return ret;
}

/*
 * rwsem_owner_state() - 把 owner 裸指针与低位 flags 归一化为自旋状态。
 * @owner 为可空、受调用者 RCU 窗口保护的借用快照；@flags 为同次读取的提示位。函数不睡眠、无
 * 副作用；NONSPINNABLE 优先，其次 reader，最后依据 owner 是否为空区分 writer/NULL。
 */
static inline enum owner_state
rwsem_owner_state(struct task_struct *owner, unsigned long flags)
{
	if (flags & RWSEM_NONSPINNABLE)
		return OWNER_NONSPINNABLE;

	if (flags & RWSEM_READER_OWNED)
		return OWNER_READER;

	return owner ? OWNER_WRITER : OWNER_NULL;
}

/*
 * rwsem_spin_on_owner() - 对当前明确 writer owner 自旋，直到 owner 改变或不再适合自旋。
 * @sem 为非空借用锁；入口必须禁止抢占，借此形成 RCU 读侧窗口，不持 wait_lock，函数不睡眠。
 * owner/flags 与 new/new_flags 是连续快照，state 返回 OWNER_NULL/WRITER/READER/NONSPINNABLE；函数
 * 不获取锁、不持 task 引用。只有在确认 sem->owner 仍匹配后才可读 owner->on_cpu，barrier 防止编译器
 * 把解引用提前到验证前造成 UAF；need_resched 或 owner 下 CPU时停止。
 */
static noinline enum owner_state
rwsem_spin_on_owner(struct rw_semaphore *sem)
{
	struct task_struct *new, *owner;
	unsigned long flags, new_flags;
	enum owner_state state;

	lockdep_assert_preemption_disabled();

	owner = rwsem_owner_flags(sem, &flags);
	state = rwsem_owner_state(owner, flags);
	if (state != OWNER_WRITER)
		return state;

	for (;;) {
		/*
		 * When a waiting writer set the handoff flag, it may spin
		 * on the owner as well. Once that writer acquires the lock,
		 * we can spin on it. So we don't need to quit even when the
		 * handoff bit is set.
		 */
		/* handoff writer 也可能成为新 owner；owner 真正切换后可继续对新 writer 自旋，无需因 handoff 退出。 */
		new = rwsem_owner_flags(sem, &new_flags);
		if ((new != owner) || (new_flags != flags)) {
			state = rwsem_owner_state(new, new_flags);
			break;
		}

		/*
		 * Ensure we emit the owner->on_cpu, dereference _after_
		 * checking sem->owner still matches owner, if that fails,
		 * owner might point to free()d memory, if it still matches,
		 * our spinning context already disabled preemption which is
		 * equal to RCU read-side crital section ensures the memory
		 * stays valid.
		 */
		/*
		 * 编译器屏障确保先确认 owner 字段仍等于裸指针，再解引用 owner->on_cpu；验证失败时 task 可能已
		 * 释放，验证成功则当前禁止抢占的 RCU 窗口保证内存仍存活。
		 */
		barrier();

		if (need_resched() || !owner_on_cpu(owner)) {
			state = OWNER_NONSPINNABLE;
			break;
		}

		cpu_relax();
	}

	return state;
}

/*
 * Calculate reader-owned rwsem spinning threshold for writer
 *
 * The more readers own the rwsem, the longer it will take for them to
 * wind down and free the rwsem. So the empirical formula used to
 * determine the actual spinning time limit here is:
 *
 *   Spinning threshold = (10 + nr_readers/2)us
 *
 * The limit is capped to a maximum of 25us (30 readers). This is just
 * a heuristic and is subjected to change in the future.
 */
/* reader 越多预计清空越慢；经验阈值为 (10 + readers/2) 微秒，最多按 30 readers 封顶为 25 微秒。 */
/*
 * rwsem_rspin_threshold() - 计算当前 reader phase 的绝对自旋截止时间。
 * @sem 为非空借用锁；无需 wait_lock、不睡眠。count/readers 是瞬时计数，delta 单位纳秒；返回
 * sched_clock() 时间域中的截止值。结果是启发式快照，不保证 reader 数稳定，也不改变锁状态。
 */
static inline u64 rwsem_rspin_threshold(struct rw_semaphore *sem)
{
	long count = atomic_long_read(&sem->count);
	int readers = count >> RWSEM_READER_SHIFT;
	u64 delta;

	if (readers > 30)
		readers = 30;
	delta = (20 + readers) * NSEC_PER_USEC / 2;

	return sched_clock() + delta;
}

/*
 * rwsem_optimistic_spin() - 以 OSQ 串行化 writers，对 owner 自旋并反复尝试无排队偷锁。
 * @sem 为非空借用输入输出锁；入口禁止抢占、不持 wait_lock，函数忙等但不睡眠。taken 为最终获取
 * 结果，prev_owner_state/loop/rspin_threshold 跟踪 reader phase 的限时启发式。返回 true 表示 acquire
 * 写锁并持有真实 rwsem；false 表示未持锁。函数不会加入 wait_list；取得 OSQ 的路径在返回前释放，
 * osq_lock() 失败路径从未获得 OSQ ownership，直接转阻塞慢路径。
 */
static bool rwsem_optimistic_spin(struct rw_semaphore *sem)
{
	bool taken = false;
	int prev_owner_state = OWNER_NULL;
	int loop = 0;
	u64 rspin_threshold = 0;

	/* sem->wait_lock should not be held when doing optimistic spinning */
	/* 乐观自旋期间绝不能持 wait_lock；OSQ 获取失败表示当前任务应停止排队自旋并转睡眠慢路径。 */
	if (!osq_lock(&sem->osq))
		goto done;

	/*
	 * Optimistically spin on the owner field and attempt to acquire the
	 * lock whenever the owner changes. Spinning will be stopped when:
	 *  1) the owning writer isn't running; or
	 *  2) readers own the lock and spinning time has exceeded limit.
	 */
	/* 对 owner 变化反复偷锁；owner writer 不再运行，或 reader phase 超出经验时限时终止。 */
	for (;;) {
		enum owner_state owner_state;

		owner_state = rwsem_spin_on_owner(sem);
		if (owner_state == OWNER_NONSPINNABLE)
			break;

		/*
		 * Try to acquire the lock
		 */
		/* 每轮 owner 检查后尝试在无 handoff 条件下 CAS writer 位。 */
		taken = rwsem_try_write_lock_unqueued(sem);

		if (taken)
			break;

		/*
		 * Time-based reader-owned rwsem optimistic spinning
		 */
		/* reader-owned 锁没有单一可跟随 owner，只允许受时间预算约束的短自旋。 */
		if (owner_state == OWNER_READER) {
			/*
			 * Re-initialize rspin_threshold every time when
			 * the owner state changes from non-reader to reader.
			 * This allows a writer to steal the lock in between
			 * 2 reader phases and have the threshold reset at
			 * the beginning of the 2nd reader phase.
			 */
			/* 从非 reader 转入新 reader phase 时重置截止时间，允许中间 writer 偷锁后重新计时。 */
			if (prev_owner_state != OWNER_READER) {
				if (rwsem_test_oflags(sem, RWSEM_NONSPINNABLE))
					break;
				rspin_threshold = rwsem_rspin_threshold(sem);
				loop = 0;
			}

			/*
			 * Check time threshold once every 16 iterations to
			 * avoid calling sched_clock() too frequently so
			 * as to reduce the average latency between the times
			 * when the lock becomes free and when the spinner
			 * is ready to do a trylock.
			 */
			/* 每 16 轮才读 sched_clock，降低计时开销和锁空闲到 trylock 之间的平均延迟。 */
			else if (!(++loop & 0xf) && (sched_clock() > rspin_threshold)) {
				rwsem_set_nonspinnable(sem);
				lockevent_inc(rwsem_opt_nospin);
				break;
			}
		}

		/*
		 * An RT task cannot do optimistic spinning if it cannot
		 * be sure the lock holder is running or live-lock may
		 * happen if the current task and the lock holder happen
		 * to run in the same CPU. However, aborting optimistic
		 * spinning while a NULL owner is detected may miss some
		 * opportunity where spinning can continue without causing
		 * problem.
		 *
		 * There are 2 possible cases where an RT task may be able
		 * to continue spinning.
		 *
		 * 1) The lock owner is in the process of releasing the
		 *    lock, sem->owner is cleared but the lock has not
		 *    been released yet.
		 * 2) The lock was free and owner cleared, but another
		 *    task just comes in and acquire the lock before
		 *    we try to get it. The new owner may be a spinnable
		 *    writer.
		 *
		 * To take advantage of two scenarios listed above, the RT
		 * task is made to retry one more time to see if it can
		 * acquire the lock or continue spinning on the new owning
		 * writer. Of course, if the time lag is long enough or the
		 * new owner is not a writer or spinnable, the RT task will
		 * quit spinning.
		 *
		 * If the owner is a writer, the need_resched() check is
		 * done inside rwsem_spin_on_owner(). If the owner is not
		 * a writer, need_resched() check needs to be done here.
		 */
		/*
		 * RT/DL 任务若不能确认 owner 正在运行，可能与同 CPU owner 活锁。NULL 可能只是 writer 正在清
		 * owner 尚未清锁，或锁刚空闲又被新 writer 抢到，所以 writer→NULL 时允许再试一次；连续非
		 * writer 状态则退出。writer 分支已在 spin_on_owner 内查 need_resched，其余在这里检查。
		 */
		if (owner_state != OWNER_WRITER) {
			if (need_resched())
				break;
			if (rt_or_dl_task(current) &&
			   (prev_owner_state != OWNER_WRITER))
				break;
		}
		prev_owner_state = owner_state;

		/*
		 * The cpu_relax() call is a compiler barrier which forces
		 * everything in this loop to be re-loaded. We don't need
		 * memory barriers as we'll eventually observe the right
		 * values at the cost of a few extra spins.
		 */
		/* cpu_relax 也是编译器屏障，迫使循环重新取值；无需硬件屏障，允许多转几轮后收敛。 */
		cpu_relax();
	}
	osq_unlock(&sem->osq);
done:
	lockevent_cond_inc(rwsem_opt_fail, !taken);
	return taken;
}

/*
 * Clear the owner's RWSEM_NONSPINNABLE bit if it is set. This should
 * only be called when the reader count reaches 0.
 */
/* 仅当 reader count 归零时清 NONSPINNABLE，避免同一 reader phase 中过早恢复自旋。 */
/*
 * clear_nonspinnable() - reader phase 结束后清除禁自旋提示。
 * @sem 为非空借用输入输出锁；调用者保证 reader count 为 0，不睡眠。返回无直接值；仅按需原子清
 * NONSPINNABLE，不改变地址、READER_OWNED 或 count。
 */
static inline void clear_nonspinnable(struct rw_semaphore *sem)
{
	if (unlikely(rwsem_test_oflags(sem, RWSEM_NONSPINNABLE)))
		atomic_long_andnot(RWSEM_NONSPINNABLE, &sem->owner);
}

#else
/*
 * rwsem_can_spin_on_owner() - 关闭 owner 自旋配置时恒拒绝自旋。
 * @sem 为未使用的借用参数；不睡眠、无副作用，返回 false。
 */
static inline bool rwsem_can_spin_on_owner(struct rw_semaphore *sem)
{
	return false;
}

/*
 * rwsem_optimistic_spin() - 关闭 owner 自旋配置时的恒失败 stub。
 * @sem 未使用；不睡眠、不获取锁，返回 false。
 */
static inline bool rwsem_optimistic_spin(struct rw_semaphore *sem)
{
	return false;
}

/*
 * clear_nonspinnable() - 无 owner 自旋配置下无需清理的空 stub。
 * @sem 未使用；返回无直接值且无副作用。
 */
static inline void clear_nonspinnable(struct rw_semaphore *sem) { }

/*
 * rwsem_spin_on_owner() - 无 owner 自旋配置下强制报告不可自旋。
 * @sem 未使用；不睡眠，返回 OWNER_NONSPINNABLE，调用者转阻塞慢路径。
 */
static inline enum owner_state
rwsem_spin_on_owner(struct rw_semaphore *sem)
{
	return OWNER_NONSPINNABLE;
}
#endif

/*
 * Prepare to wake up waiter(s) in the wait queue by putting them into the
 * given wake_q if the rwsem lock owner isn't a writer. If rwsem is likely
 * reader-owned, wake up read lock waiters in queue front or wake up any
 * front waiter otherwise.

 * This is being called from both reader and writer slow paths.
 */
/*
 * 读写慢路径在 wait_lock 下调用：若没有 writer owner，则 reader-owned 时仅推进 reader phase，完全
 * 空闲时可唤醒任意队首并清 NONSPINNABLE；所有实际唤醒仍延迟到调用者放锁后。
 */
/*
 * rwsem_cond_wake_waiter() - 根据 count 快照选择是否及如何标记等待者唤醒。
 * @sem 为非空借用输入输出锁；@count 为调用者在同一协议窗口得到的状态快照；@wake_q 为非空输入
 * 输出队列。入口持 wait_lock、不睡眠。writer 位存在时无副作用；否则调用 rwsem_mark_wake()，可能
 * 授予 readers/标记队首并更新队列，引用由后续 wake_up_q 消费。
 */
static inline void rwsem_cond_wake_waiter(struct rw_semaphore *sem, long count,
					  struct wake_q_head *wake_q)
{
	enum rwsem_wake_type wake_type;

	if (count & RWSEM_WRITER_MASK)
		return;

	if (count & RWSEM_READER_MASK) {
		wake_type = RWSEM_WAKE_READERS;
	} else {
		wake_type = RWSEM_WAKE_ANY;
		clear_nonspinnable(sem);
	}
	rwsem_mark_wake(sem, wake_type, wake_q);
}

/*
 * Wait for the read lock to be granted
 */
/* 等待读锁被直接偷取或由唤醒者预授予；失败 reader 已在快路径预加 bias，本函数负责保留或撤销。 */
/*
 * rwsem_down_read_slowpath() - 处理读快路径失败、排队、睡眠、授予与信号取消。
 * @sem 为非空借用输入输出锁；@count 是已预加 READER_BIAS 后的快路径快照；@state 指定不可中断、
 * interruptible 或 killable 等待。入口禁止抢占、不持 wait_lock，函数可睡眠。adjustment 初始撤销预加
 * bias；rcnt 是快照 reader 数；waiter/first 为栈节点和队首借用，wake_q 管理锁外唤醒。
 * 返回 sem 表示 acquire 读锁，ERR_PTR(-EINTR) 表示已撤销计数/队列且未持锁；所有队列、hung-task、
 * task state 与 trace 责任均在返回前闭环，成功者由调用者最终 up_read()。
 */
static struct rw_semaphore __sched *
rwsem_down_read_slowpath(struct rw_semaphore *sem, long count, unsigned int state)
{
	long adjustment = -RWSEM_READER_BIAS;
	long rcnt = (count >> RWSEM_READER_SHIFT);
	struct rwsem_waiter waiter, *first;
	DEFINE_WAKE_Q(wake_q);

	/*
	 * To prevent a constant stream of readers from starving a sleeping
	 * writer, don't attempt optimistic lock stealing if the lock is
	 * very likely owned by readers.
	 */
	/* 多 reader 明确持有时不偷锁，避免持续新 readers 让已睡眠 writer 饥饿。 */
	if ((atomic_long_read(&sem->owner) & RWSEM_READER_OWNED) &&
	    (rcnt > 1) && !(count & RWSEM_WRITER_LOCKED))
		goto queue;

	/*
	 * Reader optimistic lock stealing.
	 */
	/* 无 writer/handoff 时保留快路径已加的 bias，直接把当前任务视为偷取成功。 */
	if (!(count & (RWSEM_WRITER_LOCKED | RWSEM_FLAG_HANDOFF))) {
		rwsem_set_reader_owned(sem);
		lockevent_inc(rwsem_rlock_steal);

		/*
		 * Wake up other readers in the wait queue if it is
		 * the first reader.
		 */
		/* 当前是本 phase 首 reader 且存在 waiter 时，批量授予队首等待 readers。 */
		if ((rcnt == 1) && (count & RWSEM_FLAG_WAITERS)) {
			raw_spin_lock_irq(&sem->wait_lock);
			if (sem->first_waiter)
				rwsem_mark_wake(sem, RWSEM_WAKE_READ_OWNED,
						&wake_q);
			raw_spin_unlock_irq(&sem->wait_lock);
			wake_up_q(&wake_q);
		}
		return sem;
	}

queue:
	waiter.task = current;
	waiter.type = RWSEM_WAITING_FOR_READ;
	waiter.timeout = jiffies + RWSEM_WAIT_TIMEOUT;
	waiter.handoff_set = false;

	raw_spin_lock_irq(&sem->wait_lock);
	first = sem->first_waiter;
	if (!first) {
		/*
		 * In case the wait queue is empty and the lock isn't owned
		 * by a writer, this reader can exit the slowpath and return
		 * immediately as its RWSEM_READER_BIAS has already been set
		 * in the count.
		 */
		/*
		 * 取得 wait_lock 后若队列仍空且无 writer，快路径预加 bias 已足够；补 acquire 控制依赖屏障后
		 * 直接成功。否则首个 waiter 建立环形队列并置 WAITERS。
		 */
		if (!(atomic_long_read(&sem->count) & RWSEM_WRITER_MASK)) {
			/* Provide lock ACQUIRE */
			/* 分支判断确认无 writer 后补 acquire，禁止临界区访问越过该成功判定。 */
			smp_acquire__after_ctrl_dep();
			raw_spin_unlock_irq(&sem->wait_lock);
			rwsem_set_reader_owned(sem);
			lockevent_inc(rwsem_rlock_fast);
			return sem;
		}
		adjustment += RWSEM_FLAG_WAITERS;
		INIT_LIST_HEAD(&waiter.list);
		sem->first_waiter = &waiter;
	} else {
		list_add_tail(&waiter.list, &first->list);
	}

	/* we're now waiting on the lock, but no longer actively locking */
	/* 正式入队后撤销本任务预加的 reader bias；此后锁只会由 mark_wake 重新预授予。 */
	count = atomic_long_add_return(adjustment, &sem->count);

	rwsem_cond_wake_waiter(sem, count, &wake_q);
	raw_spin_unlock_irq(&sem->wait_lock);

	if (!wake_q_empty(&wake_q))
		wake_up_q(&wake_q);

	trace_contention_begin(sem, LCB_F_READ);
	set_current_state(state);

	if (state == TASK_UNINTERRUPTIBLE)
		hung_task_set_blocker(sem, BLOCKER_TYPE_RWSEM_READER);

	/* wait to be given the lock */
	/* 睡眠直到 waker 以 release 把 waiter.task 清 NULL，或可中断状态收到信号。 */
	for (;;) {
		if (!smp_load_acquire(&waiter.task)) {
			/* Matches rwsem_mark_wake()'s smp_store_release(). */
			/* acquire 读取与 mark_wake 的 release 清 NULL 配对，随后可安全进入读临界区。 */
			break;
		}
		if (signal_pending_state(state, current)) {
			raw_spin_lock_irq(&sem->wait_lock);
			if (waiter.task)
				goto out_nolock;
			raw_spin_unlock_irq(&sem->wait_lock);
			/* Ordered by sem->wait_lock against rwsem_mark_wake(). */
			/* 信号与授予并发时由 wait_lock 定序；task 已清 NULL 表示授予胜出，不能再按失败撤销。 */
			break;
		}
		schedule_preempt_disabled();
		lockevent_inc(rwsem_sleep_reader);
		set_current_state(state);
	}

	if (state == TASK_UNINTERRUPTIBLE)
		hung_task_clear_blocker();

	__set_current_state(TASK_RUNNING);
	lockevent_inc(rwsem_rlock);
	trace_contention_end(sem, 0);
	return sem;

out_nolock:
	/* 信号胜出时仍持 wait_lock；helper 摘链、释放该锁并在锁外按需唤醒后继。 */
	rwsem_del_wake_waiter(sem, &waiter, &wake_q);
	__set_current_state(TASK_RUNNING);
	lockevent_inc(rwsem_rlock_fail);
	trace_contention_end(sem, -EINTR);
	return ERR_PTR(-EINTR);
}

/*
 * Wait until we successfully acquire the write lock
 */
/* 等待直到 writer 通过乐观偷锁或 wait_list/handoff 协议取得独占锁。 */
/*
 * rwsem_down_write_slowpath() - 写快路径失败后的 OSQ 自旋、排队睡眠、handoff 与信号回滚核心。
 * @sem 为非空借用输入输出锁；@state 为不可中断或 killable 等待状态。入口禁止抢占、不持 wait_lock，
 * 函数可睡眠。waiter 是 current 的栈上 writer 节点，first 为队首借用，wake_q 收集锁外唤醒。
 * 返回 sem 表示 acquire 写锁、owner=current 且已出队；ERR_PTR(-EINTR) 表示未持锁、节点已删除并按需
 * 推进后继。所有 task state、hung blocker、trace 与队列责任在返回前闭环，成功者最终 up_write()。
 */
static struct rw_semaphore __sched *
rwsem_down_write_slowpath(struct rw_semaphore *sem, int state)
{
	struct rwsem_waiter waiter, *first;
	DEFINE_WAKE_Q(wake_q);

	/* do optimistic spinning and steal lock if possible */
	/* 先尝试不入队的 OSQ owner 自旋；成功已经提供 ACQUIRE 并记录 owner。 */
	if (rwsem_can_spin_on_owner(sem) && rwsem_optimistic_spin(sem)) {
		/* rwsem_optimistic_spin() implies ACQUIRE on success */
		/* 成功后无需 wait_list 状态，直接把真实写持有返回。 */
		return sem;
	}

	/*
	 * Optimistic spinning failed, proceed to the slowpath
	 * and block until we can acquire the sem.
	 */
	/* 自旋失败后构造栈 waiter，准备进入受 wait_lock 保护的环形队列并睡眠。 */
	waiter.task = current;
	waiter.type = RWSEM_WAITING_FOR_WRITE;
	waiter.timeout = jiffies + RWSEM_WAIT_TIMEOUT;
	waiter.handoff_set = false;

	raw_spin_lock_irq(&sem->wait_lock);

	first = sem->first_waiter;
	if (first) {
		list_add_tail(&waiter.list, &first->list);
		rwsem_cond_wake_waiter(sem, atomic_long_read(&sem->count),
				       &wake_q);
		if (!wake_q_empty(&wake_q)) {
			/*
			 * We want to minimize wait_lock hold time especially
			 * when a large number of readers are to be woken up.
			 */
			/* 大批 reader 唤醒可能耗时；临时放开 wait_lock 消费 wake_q，再重取后继续 writer 入队流程。 */
			raw_spin_unlock_irq(&sem->wait_lock);
			wake_up_q(&wake_q);
			raw_spin_lock_irq(&sem->wait_lock);
		}
	} else {
		INIT_LIST_HEAD(&waiter.list);
		sem->first_waiter = &waiter;
		atomic_long_or(RWSEM_FLAG_WAITERS, &sem->count);
	}

	/* wait until we successfully acquire the lock */
	/* waiter 已发布：设置等待态并开启写争用 trace，循环在 wait_lock 下尝试获取/handoff。 */
	set_current_state(state);
	trace_contention_begin(sem, LCB_F_WRITE);

	if (state == TASK_UNINTERRUPTIBLE)
		hung_task_set_blocker(sem, BLOCKER_TYPE_RWSEM_WRITER);

	for (;;) {
		if (rwsem_try_write_lock(sem, &waiter)) {
			/* rwsem_try_write_lock() implies ACQUIRE on success */
			/* 成功同时摘除 waiter、清 handoff 并记录 owner，退出循环时仍持 wait_lock。 */
			break;
		}

		raw_spin_unlock_irq(&sem->wait_lock);

		if (signal_pending_state(state, current))
			goto out_nolock;

		/*
		 * After setting the handoff bit and failing to acquire
		 * the lock, attempt to spin on owner to accelerate lock
		 * transfer. If the previous owner is a on-cpu writer and it
		 * has just released the lock, OWNER_NULL will be returned.
		 * In this case, we attempt to acquire the lock again
		 * without sleeping.
		 */
		/*
		 * waiter 已请求 handoff 后可在 owner 上短自旋加速交接；若前 writer 刚清 owner 尚未完成释放，
		 * 返回 OWNER_NULL 时立即重取 wait_lock 再试，避免一次无谓睡眠。
		 */
		if (waiter.handoff_set) {
			enum owner_state owner_state;

			owner_state = rwsem_spin_on_owner(sem);
			if (owner_state == OWNER_NULL)
				goto trylock_again;
		}

		schedule_preempt_disabled();
		lockevent_inc(rwsem_sleep_writer);
		set_current_state(state);
trylock_again:
		/* 睡眠或短自旋后重新持 wait_lock，在循环顶部以最新 count/队首状态重试。 */
		raw_spin_lock_irq(&sem->wait_lock);
	}

	if (state == TASK_UNINTERRUPTIBLE)
		hung_task_clear_blocker();

	__set_current_state(TASK_RUNNING);
	raw_spin_unlock_irq(&sem->wait_lock);
	lockevent_inc(rwsem_wlock);
	trace_contention_end(sem, 0);
	return sem;

out_nolock:
	/* 信号在不持 wait_lock 时被观察；恢复运行态后重取锁，删除 waiter 并推进可能合格的后继。 */
	__set_current_state(TASK_RUNNING);
	raw_spin_lock_irq(&sem->wait_lock);
	rwsem_del_wake_waiter(sem, &waiter, &wake_q);
	lockevent_inc(rwsem_wlock_fail);
	trace_contention_end(sem, -EINTR);
	return ERR_PTR(-EINTR);
}

/*
 * handle waking up a waiter on the semaphore
 * - up_read/up_write has decremented the active part of count if we come here
 */
/* up_read/up_write 已先以 release 更新活跃 count；这里在 wait_lock 下选择队首并锁外真正唤醒。 */
/*
 * rwsem_wake() - 锁释放后标记并唤醒下一批合格 waiter。
 * @sem 为非空借用输入输出锁；入口不持 wait_lock，函数不睡眠。flags 保存 IRQ 状态，wake_q 持临时
 * task 引用；返回原 @sem 便于调用链，可能推进 wait_list/count/owner，退出已消费全部 wake_q。
 */
static struct rw_semaphore *rwsem_wake(struct rw_semaphore *sem)
{
	unsigned long flags;
	DEFINE_WAKE_Q(wake_q);

	raw_spin_lock_irqsave(&sem->wait_lock, flags);

	if (sem->first_waiter)
		rwsem_mark_wake(sem, RWSEM_WAKE_ANY, &wake_q);

	raw_spin_unlock_irqrestore(&sem->wait_lock, flags);
	wake_up_q(&wake_q);

	return sem;
}

/*
 * downgrade a write lock into a read lock
 * - caller incremented waiting part of count and discovered it still negative
 * - just wake up any readers at the front of the queue
 */
/* writer 已把 count 原子转为一份读持有；这里只批量授予队首 readers，不越过 writer。 */
/*
 * rwsem_downgrade_wake() - 写降级为读后唤醒可共享的等待 readers。
 * @sem 为 current 已降级持读锁的非空借用对象；入口不持 wait_lock、不睡眠。返回原 sem；仅以
 * RWSEM_WAKE_READ_OWNED 推进 reader phase，锁外消费 wake_q，队首 writer 保持等待。
 */
static struct rw_semaphore *rwsem_downgrade_wake(struct rw_semaphore *sem)
{
	unsigned long flags;
	DEFINE_WAKE_Q(wake_q);

	raw_spin_lock_irqsave(&sem->wait_lock, flags);

	if (sem->first_waiter)
		rwsem_mark_wake(sem, RWSEM_WAKE_READ_OWNED, &wake_q);

	raw_spin_unlock_irqrestore(&sem->wait_lock, flags);
	wake_up_q(&wake_q);

	return sem;
}

/*
 * lock for reading
 */
/* 获取读锁的内部公共入口；公开包装在外层处理 lockdep。 */
/*
 * __down_read_common() - 用快路径或指定任务状态的慢路径取得读锁。
 * @sem 为非空借用输入输出锁；@state 指定等待可中断性。入口可睡眠且不持内部锁；函数在整个 owner/
 * count 转换期间禁止抢占。返回 0 表示持有读锁，-EINTR 表示慢路径已完整回滚；count 是快路径输出，
 * ret 汇总结果。无对象引用转移，成功者由调用者配对 __up_read()。
 */
static __always_inline int __down_read_common(struct rw_semaphore *sem, int state)
{
	int ret = 0;
	long count;

	preempt_disable();
	if (!rwsem_read_trylock(sem, &count)) {
		if (IS_ERR(rwsem_down_read_slowpath(sem, count, state))) {
			ret = -EINTR;
			goto out;
		}
		DEBUG_RWSEMS_WARN_ON(!is_rwsem_reader_owned(sem), sem);
	}
out:
	/* 成功或回滚后统一恢复抢占；此后 owner 裸指针不再受本函数的 RCU 窗口保护。 */
	preempt_enable();
	return ret;
}

/*
 * __down_read() - 不可中断地阻塞取得读锁。
 * @sem 为非空借用锁；可睡眠，无直接返回值，返回时保证持读锁，调用者配对 __up_read()。
 */
static __always_inline void __down_read(struct rw_semaphore *sem)
{
	__down_read_common(sem, TASK_UNINTERRUPTIBLE);
}

/*
 * __down_read_interruptible() - 以 TASK_INTERRUPTIBLE 获取读锁。
 * @sem 为非空借用锁；可睡眠。返回 0 表示持锁，-EINTR 表示信号取消且未持锁。
 */
static __always_inline int __down_read_interruptible(struct rw_semaphore *sem)
{
	return __down_read_common(sem, TASK_INTERRUPTIBLE);
}

/*
 * __down_read_killable() - 以 TASK_KILLABLE 获取读锁。
 * @sem 为非空借用锁；可睡眠。返回 0 表示持锁，-EINTR 表示致命信号取消且未持锁。
 */
static __always_inline int __down_read_killable(struct rw_semaphore *sem)
{
	return __down_read_common(sem, TASK_KILLABLE);
}

/*
 * __down_read_trylock() - 不睡眠地尝试增加一个 reader count。
 * @sem 为非空借用输入输出锁；入口不持内部锁，函数短暂禁止抢占。返回 1 表示 acquire 读持有并更新
 * owner 提示，0 表示 writer/waiter/handoff/readfail 阻止获取且 count 未被本次调用改变。
 */
static inline int __down_read_trylock(struct rw_semaphore *sem)
{
	int ret = 0;
	long tmp;

	DEBUG_RWSEMS_WARN_ON(sem->magic != sem, sem);

	preempt_disable();
	tmp = atomic_long_read(&sem->count);
	while (!(tmp & RWSEM_READ_FAILED_MASK)) {
		if (atomic_long_try_cmpxchg_acquire(&sem->count, &tmp,
						    tmp + RWSEM_READER_BIAS)) {
			rwsem_set_reader_owned(sem);
			ret = 1;
			break;
		}
	}
	preempt_enable();
	return ret;
}

/*
 * lock for writing
 */
/* 获取写锁的内部公共入口；快路径失败时进入可睡眠 writer 慢路径。 */
/*
 * __down_write_common() - 以指定等待状态取得独占写锁。
 * @sem 为非空借用输入输出锁；@state 为不可中断或 killable。函数可睡眠并在 owner/count 操作期间
 * 禁止抢占。返回 0 表示持写锁，-EINTR 表示慢路径已回滚且未持锁；成功由调用者配对 __up_write()。
 */
static __always_inline int __down_write_common(struct rw_semaphore *sem, int state)
{
	int ret = 0;

	preempt_disable();
	if (unlikely(!rwsem_write_trylock(sem))) {
		if (IS_ERR(rwsem_down_write_slowpath(sem, state)))
			ret = -EINTR;
	}
	preempt_enable();
	return ret;
}

/*
 * __down_write() - 不可中断地阻塞取得写锁。
 * @sem 为非空借用锁；可睡眠，无直接返回值，返回时 current 独占持锁。
 */
static __always_inline void __down_write(struct rw_semaphore *sem)
{
	__down_write_common(sem, TASK_UNINTERRUPTIBLE);
}

/*
 * __down_write_killable() - 以 TASK_KILLABLE 阻塞取得写锁。
 * @sem 为非空借用锁；可睡眠。返回 0 表示持锁，-EINTR 表示致命信号取消且未持锁。
 */
static __always_inline int __down_write_killable(struct rw_semaphore *sem)
{
	return __down_write_common(sem, TASK_KILLABLE);
}

/*
 * __down_write_trylock() - 不睡眠地尝试从完全空闲状态取得写锁。
 * @sem 为非空借用输入输出锁；短暂禁止抢占。返回 1 表示 acquire 写持有并记录 owner，0 表示无修改。
 */
static inline int __down_write_trylock(struct rw_semaphore *sem)
{
	int ret;

	preempt_disable();
	DEBUG_RWSEMS_WARN_ON(sem->magic != sem, sem);
	ret = rwsem_write_trylock(sem);
	preempt_enable();

	return ret;
}

/*
 * unlock after reading
 */
/* 释放一个 reader count；若最后 reader 离开且有 waiter，则推进下一 phase。 */
/*
 * __up_read() - 释放 current 的一个普通读持有并按需唤醒等待者。
 * @sem 为非空借用输入输出锁；入口必须持读锁，不睡眠。返回无直接值；清理调试 owner，release 减
 * reader bias，若锁已无活跃持有且 WAITERS 存在则清禁自旋并 rwsem_wake()。出口不再持该读锁。
 */
static inline void __up_read(struct rw_semaphore *sem)
{
	long tmp;

	DEBUG_RWSEMS_WARN_ON(sem->magic != sem, sem);
	DEBUG_RWSEMS_WARN_ON(!is_rwsem_reader_owned(sem), sem);

	preempt_disable();
	rwsem_clear_reader_owned(sem);
	tmp = atomic_long_add_return_release(-RWSEM_READER_BIAS, &sem->count);
	DEBUG_RWSEMS_WARN_ON(tmp < 0, sem);
	if (trace_contended_release_enabled() && (tmp & RWSEM_FLAG_WAITERS))
		trace_call__contended_release(sem);
	if (unlikely((tmp & (RWSEM_LOCK_MASK|RWSEM_FLAG_WAITERS)) ==
		      RWSEM_FLAG_WAITERS)) {
		clear_nonspinnable(sem);
		rwsem_wake(sem);
	}
	preempt_enable();
}

/*
 * unlock after writing
 */
/* 释放 writer 位并发布写临界区；有 waiter 时锁外推进队列。 */
/*
 * __up_write() - 释放 current 的独占写持有并按需唤醒等待者。
 * @sem 为非空借用输入输出锁；入口必须持写锁，不睡眠。返回无直接值；禁止抢占区内先清 owner，再以
 * release 清 WRITER_LOCKED；若旧 count 带 WAITERS 则记录争用释放并 rwsem_wake()。出口不再持锁。
 */
static inline void __up_write(struct rw_semaphore *sem)
{
	long tmp;

	DEBUG_RWSEMS_WARN_ON(sem->magic != sem, sem);
	/*
	 * sem->owner may differ from current if the ownership is transferred
	 * to an anonymous writer by setting the RWSEM_NONSPINNABLE bits.
	 */
	/* NONSPINNABLE 可把 owner 视为匿名 writer 提示，因此调试检查允许 task 地址不等于 current。 */
	DEBUG_RWSEMS_WARN_ON((rwsem_owner(sem) != current) &&
			    !rwsem_test_oflags(sem, RWSEM_NONSPINNABLE), sem);

	preempt_disable();
	rwsem_clear_owner(sem);
	tmp = atomic_long_fetch_add_release(-RWSEM_WRITER_LOCKED, &sem->count);
	if (unlikely(tmp & RWSEM_FLAG_WAITERS)) {
		trace_contended_release(sem);
		rwsem_wake(sem);
	}
	preempt_enable();
}

/*
 * downgrade write lock to read lock
 */
/* 原子把独占 writer 转为 current 的一份共享 reader，并只唤醒可共享 readers。 */
/*
 * __downgrade_write() - 将 current 的写持有降级为读持有而不出现无锁窗口。
 * @sem 为非空借用输入输出锁；入口必须持写锁，不睡眠。返回无直接值；release 原子同时清 writer 位、
 * 加 reader bias，随后记录 reader owner；若有 waiter 则只唤醒 reader phase。出口持一份读锁。
 */
static inline void __downgrade_write(struct rw_semaphore *sem)
{
	long tmp;

	/*
	 * When downgrading from exclusive to shared ownership,
	 * anything inside the write-locked region cannot leak
	 * into the read side. In contrast, anything in the
	 * read-locked region is ok to be re-ordered into the
	 * write side. As such, rely on RELEASE semantics.
	 */
	/*
	 * 降级需阻止写临界区访问泄漏到后续读侧之外；读侧访问提前到原写侧不破坏独占保证，因此 release
	 * 足够，无需完整 acquire。该原子操作也是 writer→reader 的不可分割发布点。
	 */
	DEBUG_RWSEMS_WARN_ON(rwsem_owner(sem) != current, sem);
	preempt_disable();
	tmp = atomic_long_fetch_add_release(
		-RWSEM_WRITER_LOCKED+RWSEM_READER_BIAS, &sem->count);
	rwsem_set_reader_owned(sem);
	if (tmp & RWSEM_FLAG_WAITERS) {
		trace_contended_release(sem);
		rwsem_downgrade_wake(sem);
	}
	preempt_enable();
}

#else /* !CONFIG_PREEMPT_RT */
/*
 * 修正说明：本 #else 与文件前部的 #ifndef CONFIG_PREEMPT_RT 配对，实际编译条件是
 * CONFIG_PREEMPT_RT=y；原有尾注释方向相反但按追加式规则保留。以下把 rwsem 适配到 rtmutex/rwbase。
 */

/*
 * RT_MUTEX_BUILD_MUTEX 让文本包含的 rtmutex.c 生成 rwbase 所需普通 rtmutex 内部入口。随后这些宏把
 * rwbase 的任务状态、可中断锁、锁内慢路径、解锁/trylock、信号判断和调度三段分别映射到 rtmutex
 * 实现；最后文本包含 rwbase_rt.c 生成本编译单元私有 helper。宏只做适配，不建立额外对象 ownership；
 * rwbase_pre/schedule/post 必须严格配对，wake_q 仍由共享实现锁外消费。
 */

#define RT_MUTEX_BUILD_MUTEX
#include "rtmutex.c"

#define rwbase_set_and_save_current_state(state)	\
	set_current_state(state)

#define rwbase_restore_current_state()			\
	__set_current_state(TASK_RUNNING)

#define rwbase_rtmutex_lock_state(rtm, state)		\
	__rt_mutex_lock(rtm, state)

#define rwbase_rtmutex_slowlock_locked(rtm, state, wq)	\
	__rt_mutex_slowlock_locked(rtm, NULL, state, wq)

#define rwbase_rtmutex_unlock(rtm)			\
	__rt_mutex_unlock(rtm)

#define rwbase_rtmutex_trylock(rtm)			\
	__rt_mutex_trylock(rtm)

#define rwbase_signal_pending_state(state, current)	\
	signal_pending_state(state, current)

#define rwbase_pre_schedule()				\
	rt_mutex_pre_schedule()

#define rwbase_schedule()				\
	rt_mutex_schedule()

#define rwbase_post_schedule()				\
	rt_mutex_post_schedule()

#include "rwbase_rt.c"

/*
 * __init_rwsem() - 初始化 PREEMPT_RT rwsem 的 rwbase 与 lockdep map。
 * @sem 为调用者拥有的非空输出对象；@name/@key 为稳定借用 lockdep 身份。不得并发重初始化，不睡眠；
 * 返回无直接值，rwbase readers/rtmutex 进入初始状态，存储 ownership 不变。
 */
void __init_rwsem(struct rw_semaphore *sem, const char *name,
		  struct lock_class_key *key)
{
	init_rwbase_rt(&(sem)->rwbase);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
	debug_check_no_locks_freed((void *)sem, sizeof(*sem));
	lockdep_init_map_wait(&sem->dep_map, name, key, 0, LD_WAIT_SLEEP);
#endif
}
EXPORT_SYMBOL(__init_rwsem);

/*
 * __down_read() - RT 上不可中断地经 rwbase 获取读锁。
 * @sem 非空借用；可睡眠，无返回值，返回时持读锁，后续 __up_read() 配对。
 */
static inline void __down_read(struct rw_semaphore *sem)
{
	rwbase_read_lock(&sem->rwbase, TASK_UNINTERRUPTIBLE);
}

/*
 * __down_read_interruptible() - RT 上以 TASK_INTERRUPTIBLE 获取读锁。
 * @sem 非空借用；可睡眠，返回 0 持锁或 -EINTR 未持锁。
 */
static inline int __down_read_interruptible(struct rw_semaphore *sem)
{
	return rwbase_read_lock(&sem->rwbase, TASK_INTERRUPTIBLE);
}

/*
 * __down_read_killable() - RT 上以 TASK_KILLABLE 获取读锁。
 * @sem 非空借用；可睡眠，返回 0 持锁或 -EINTR 未持锁。
 */
static inline int __down_read_killable(struct rw_semaphore *sem)
{
	return rwbase_read_lock(&sem->rwbase, TASK_KILLABLE);
}

/*
 * __down_read_trylock() - RT 上即时尝试 rwbase reader bias。
 * @sem 非空借用；不睡眠，返回非零表示持读锁，0 表示无副作用失败。
 */
static inline int __down_read_trylock(struct rw_semaphore *sem)
{
	return rwbase_read_trylock(&sem->rwbase);
}

/*
 * __up_read() - RT 上释放一份 rwbase 读持有并按需唤醒 writer。
 * @sem 非空借用且调用者持读锁；不睡眠，返回无直接值，出口不再持该读锁。
 */
static inline void __up_read(struct rw_semaphore *sem)
{
	rwbase_read_unlock(&sem->rwbase, TASK_NORMAL);
}

/*
 * __down_write() - RT 上不可中断地取得 rwbase 写锁。
 * @sem 非空借用；可睡眠，无返回值，返回时持写锁。
 */
static inline void __sched __down_write(struct rw_semaphore *sem)
{
	rwbase_write_lock(&sem->rwbase, TASK_UNINTERRUPTIBLE);
}

/*
 * __down_write_killable() - RT 上可由致命信号取消的写获取。
 * @sem 非空借用；可睡眠，返回 0 持写锁或 -EINTR 未持锁。
 */
static inline int __sched __down_write_killable(struct rw_semaphore *sem)
{
	return rwbase_write_lock(&sem->rwbase, TASK_KILLABLE);
}

/*
 * __down_write_trylock() - RT 上即时尝试 rtmutex 与零 reader 条件。
 * @sem 非空借用；不睡眠，返回 1 持写锁或 0 且完整回滚。
 */
static inline int __down_write_trylock(struct rw_semaphore *sem)
{
	return rwbase_write_trylock(&sem->rwbase);
}

/*
 * __up_write() - RT 上恢复 reader bias 并释放底层 rtmutex。
 * @sem 非空借用且调用者持写锁；不睡眠，返回无直接值，出口不持锁。
 */
static inline void __up_write(struct rw_semaphore *sem)
{
	rwbase_write_unlock(&sem->rwbase);
}

/*
 * __downgrade_write() - RT 上把 rwbase 写持有原子降级为一份读持有。
 * @sem 非空借用且调用者持写锁；不睡眠，无返回值，出口持读锁。
 */
static inline void __downgrade_write(struct rw_semaphore *sem)
{
	rwbase_write_downgrade(&sem->rwbase);
}

/* Debug stubs for the common API */
/* RT 分支无需非 RT owner 调试字段，保留统一 API 的空检查宏与 helper。 */
#define DEBUG_RWSEMS_WARN_ON(c, sem)

/*
 * __rwsem_set_reader_owned() - RT 分支无 owner 提示字段的空实现。
 * @sem/@owner 均未使用且仅借用；不睡眠、无返回值、无副作用。
 */
static inline void __rwsem_set_reader_owned(struct rw_semaphore *sem,
					    struct task_struct *owner)
{
}

/*
 * is_rwsem_reader_owned() - 依据 rwbase readers 编码判断 RT rwsem 是否由 reader 持有。
 * @sem 非空借用；不睡眠。返回 true 表示 readers 为负且不等于空闲 READER_BIAS；快照不冻结状态。
 */
static inline bool is_rwsem_reader_owned(struct rw_semaphore *sem)
{
	int count = atomic_read(&sem->rwbase.readers);

	return count < 0 && count != READER_BIAS;
}

#endif /* CONFIG_PREEMPT_RT */

/*
 * lock for reading
 */
/* 公开的不可中断读获取入口。 */
/*
 * down_read() - 登记 lockdep 后阻塞取得共享读锁。
 * @sem 为非空借用输入输出锁；必须处于可睡眠进程上下文。无直接返回值，返回时持读锁；
 * LOCK_CONTENDED 先试不睡眠路径再转 __down_read()，调用者最终以 up_read() 配对。
 */
void __sched down_read(struct rw_semaphore *sem)
	__no_context_analysis
{
	might_sleep();
	rwsem_acquire_read(&sem->dep_map, 0, 0, _RET_IP_);

	LOCK_CONTENDED(sem, __down_read_trylock, __down_read);
}
EXPORT_SYMBOL(down_read);

/*
 * down_read_interruptible() - 可由普通信号中断的公开读获取。
 * @sem 非空借用；可睡眠。返回 0 且持读锁，或 -EINTR 且撤销 lockdep/真实状态、未持锁。
 */
int __sched down_read_interruptible(struct rw_semaphore *sem)
	__no_context_analysis
{
	might_sleep();
	rwsem_acquire_read(&sem->dep_map, 0, 0, _RET_IP_);

	if (LOCK_CONTENDED_RETURN(sem, __down_read_trylock, __down_read_interruptible)) {
		rwsem_release(&sem->dep_map, _RET_IP_);
		return -EINTR;
	}

	return 0;
}
EXPORT_SYMBOL(down_read_interruptible);

/*
 * down_read_killable() - 仅致命信号可中断的公开读获取。
 * @sem 非空借用；可睡眠。返回 0 持锁或 -EINTR 未持锁；失败路径配对撤销 lockdep acquire。
 */
int __sched down_read_killable(struct rw_semaphore *sem)
	__no_context_analysis
{
	might_sleep();
	rwsem_acquire_read(&sem->dep_map, 0, 0, _RET_IP_);

	if (LOCK_CONTENDED_RETURN(sem, __down_read_trylock, __down_read_killable)) {
		rwsem_release(&sem->dep_map, _RET_IP_);
		return -EINTR;
	}

	return 0;
}
EXPORT_SYMBOL(down_read_killable);

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
/* 读 trylock 成功返回 1，竞争时返回 0，绝不睡眠。 */
/*
 * down_read_trylock() - 即时尝试共享读获取并仅在成功时登记 lockdep。
 * @sem 非空借用输入输出锁；不睡眠。返回 1 表示 acquire 读持有，0 表示无持有/无清理责任。
 */
int down_read_trylock(struct rw_semaphore *sem)
	__no_context_analysis
{
	int ret = __down_read_trylock(sem);

	if (ret == 1)
		rwsem_acquire_read(&sem->dep_map, 0, 1, _RET_IP_);
	return ret;
}
EXPORT_SYMBOL(down_read_trylock);

/*
 * lock for writing
 */
/* 公开的不可中断独占写获取入口。 */
/*
 * down_write() - 登记 lockdep 后阻塞取得独占写锁。
 * @sem 非空借用；必须可睡眠。无直接返回值，返回时持写锁，调用者最终 up_write()。
 */
void __sched down_write(struct rw_semaphore *sem)
	__no_context_analysis
{
	might_sleep();
	rwsem_acquire(&sem->dep_map, 0, 0, _RET_IP_);
	LOCK_CONTENDED(sem, __down_write_trylock, __down_write);
}
EXPORT_SYMBOL(down_write);

/*
 * lock for writing
 */
/* 可由致命信号取消的公开写获取。 */
/*
 * down_write_killable() - 以 killable 状态阻塞取得独占写锁。
 * @sem 非空借用；可睡眠。返回 0 持锁或 -EINTR 未持锁；失败时撤销 lockdep 状态。
 */
int __sched down_write_killable(struct rw_semaphore *sem)
	__no_context_analysis
{
	might_sleep();
	rwsem_acquire(&sem->dep_map, 0, 0, _RET_IP_);

	if (LOCK_CONTENDED_RETURN(sem, __down_write_trylock,
				  __down_write_killable)) {
		rwsem_release(&sem->dep_map, _RET_IP_);
		return -EINTR;
	}

	return 0;
}
EXPORT_SYMBOL(down_write_killable);

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
/* 写 trylock 成功返回 1，竞争时返回 0，绝不睡眠。 */
/*
 * down_write_trylock() - 即时尝试独占写获取并仅在成功时登记 lockdep。
 * @sem 非空借用输入输出锁；不睡眠。返回 1 表示持写锁，0 表示状态未建立。
 */
int down_write_trylock(struct rw_semaphore *sem)
	__no_context_analysis
{
	int ret = __down_write_trylock(sem);

	if (ret == 1)
		rwsem_acquire(&sem->dep_map, 0, 1, _RET_IP_);

	return ret;
}
EXPORT_SYMBOL(down_write_trylock);

/*
 * release a read lock
 */
/* 释放一份公开读持有。 */
/*
 * up_read() - 先结束 lockdep 读持有，再释放真实 reader count。
 * @sem 非空借用且调用者持一份读锁；不睡眠，无返回值，出口不再持该份锁并可能唤醒 waiter。
 */
void up_read(struct rw_semaphore *sem)
	__no_context_analysis
{
	rwsem_release(&sem->dep_map, _RET_IP_);
	__up_read(sem);
}
EXPORT_SYMBOL(up_read);

/*
 * release a write lock
 */
/* 释放公开独占写持有。 */
/*
 * up_write() - 先结束 lockdep 写持有，再 release 真实 writer 状态。
 * @sem 非空借用且 current 持写锁；不睡眠，无返回值，出口不持锁并可能推进等待队列。
 */
void up_write(struct rw_semaphore *sem)
	__no_context_analysis
{
	rwsem_release(&sem->dep_map, _RET_IP_);
	__up_write(sem);
}
EXPORT_SYMBOL(up_write);

/*
 * downgrade write lock to read lock
 */
/* 把公开写持有降级为读持有，不出现无锁窗口。 */
/*
 * downgrade_write() - 同步降级 lockdep 与真实 rwsem 状态。
 * @sem 非空借用且 current 持写锁；不睡眠，无返回值，出口持一份读锁，后续以 up_read() 释放。
 */
void downgrade_write(struct rw_semaphore *sem)
	__no_context_analysis
{
	lock_downgrade(&sem->dep_map, _RET_IP_);
	__downgrade_write(sem);
}
EXPORT_SYMBOL(downgrade_write);

#ifdef CONFIG_DEBUG_LOCK_ALLOC

/*
 * down_read_nested() - 以 @subclass 登记同类嵌套后不可中断地获取读锁。
 * @sem 非空借用；@subclass 是 lockdep 嵌套层级。可睡眠，无返回值，出口持读锁并由 up_read() 配对。
 */
void down_read_nested(struct rw_semaphore *sem, int subclass)
	__no_context_analysis
{
	might_sleep();
	rwsem_acquire_read(&sem->dep_map, subclass, 0, _RET_IP_);
	LOCK_CONTENDED(sem, __down_read_trylock, __down_read);
}
EXPORT_SYMBOL(down_read_nested);

/*
 * down_read_killable_nested() - 带 subclass 的 killable 读获取。
 * @sem 非空借用；@subclass 仅影响 lockdep。可睡眠，返回 0 持锁或 -EINTR 且撤销登记、未持锁。
 */
int down_read_killable_nested(struct rw_semaphore *sem, int subclass)
	__no_context_analysis
{
	might_sleep();
	rwsem_acquire_read(&sem->dep_map, subclass, 0, _RET_IP_);

	if (LOCK_CONTENDED_RETURN(sem, __down_read_trylock, __down_read_killable)) {
		rwsem_release(&sem->dep_map, _RET_IP_);
		return -EINTR;
	}

	return 0;
}
EXPORT_SYMBOL(down_read_killable_nested);

/*
 * _down_write_nest_lock() - 把 @nest 作为外层依赖登记后不可中断地获取写锁。
 * @sem 为非空借用输入输出锁；@nest 为非空稳定借用 lockdep map，不是运行时互斥对象。可睡眠，
 * 无返回值，出口持写锁，调用者以 up_write() 配对。
 */
void _down_write_nest_lock(struct rw_semaphore *sem, struct lockdep_map *nest)
	__no_context_analysis
{
	might_sleep();
	rwsem_acquire_nest(&sem->dep_map, 0, 0, nest, _RET_IP_);
	LOCK_CONTENDED(sem, __down_write_trylock, __down_write);
}
EXPORT_SYMBOL(_down_write_nest_lock);

/*
 * down_read_non_owner() - 获取可由另一上下文释放、且不登记 lockdep owner 的读锁。
 * @sem 非空借用；可睡眠。无返回值，出口持一份 non-owner 读锁；调用者必须用 up_read_non_owner()。
 * 非 RT 获取后清 task 地址只保留 reader 提示，防止调试误报 current 是必须配对的 owner；RT helper
 * 为空操作，因为 rwbase 不维护该调试地址。
 */
void down_read_non_owner(struct rw_semaphore *sem)
	__no_context_analysis
{
	might_sleep();
	__down_read(sem);
	/*
	 * The owner value for a reader-owned lock is mostly for debugging
	 * purpose only and is not critical to the correct functioning of
	 * rwsem. So it is perfectly fine to set it in a preempt-enabled
	 * context here.
	 */
	/*
	 * reader owner 仅供调试，不参与 rwsem 正确性；因此这里即使已恢复抢占，也可把地址清为 NULL，
	 * 只保留 READER_OWNED/NONSPINNABLE 提示以表达 non-owner 语义。
	 */
	__rwsem_set_reader_owned(sem, NULL);
}
EXPORT_SYMBOL(down_read_non_owner);

/*
 * down_write_nested() - 以 @subclass 登记后不可中断地获取写锁。
 * @sem 非空借用；@subclass 仅供 lockdep。可睡眠，无返回值，出口持写锁。
 */
void down_write_nested(struct rw_semaphore *sem, int subclass)
	__no_context_analysis
{
	might_sleep();
	rwsem_acquire(&sem->dep_map, subclass, 0, _RET_IP_);
	LOCK_CONTENDED(sem, __down_write_trylock, __down_write);
}
EXPORT_SYMBOL(down_write_nested);

/*
 * down_write_killable_nested() - 带 subclass 的 killable 写获取。
 * @sem 非空借用；@subclass 为 lockdep 层级。可睡眠，返回 0 持锁或 -EINTR 未持锁并撤销登记。
 */
int __sched down_write_killable_nested(struct rw_semaphore *sem, int subclass)
	__no_context_analysis
{
	might_sleep();
	rwsem_acquire(&sem->dep_map, subclass, 0, _RET_IP_);

	if (LOCK_CONTENDED_RETURN(sem, __down_write_trylock,
				  __down_write_killable)) {
		rwsem_release(&sem->dep_map, _RET_IP_);
		return -EINTR;
	}

	return 0;
}
EXPORT_SYMBOL(down_write_killable_nested);

/*
 * up_read_non_owner() - 释放一份由 non-owner API 获取的读锁。
 * @sem 非空借用且存在对应读持有；不睡眠，无返回值。不执行普通 lockdep release，由 __up_read()
 * release count 并按需唤醒；可与获取者不同上下文调用。
 */
void up_read_non_owner(struct rw_semaphore *sem)
	__no_context_analysis
{
	DEBUG_RWSEMS_WARN_ON(!is_rwsem_reader_owned(sem), sem);
	__up_read(sem);
}
EXPORT_SYMBOL(up_read_non_owner);

#endif
