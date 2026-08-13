/*
 * Copyright 2005, Red Hat, Inc., Ingo Molnar
 * Released under the General Public License (GPL).
 *
 * This file contains the spinlock/rwlock implementations for
 * DEBUG_SPINLOCK.
 */
/*
 * 本文件为 DEBUG_SPINLOCK 配置实现 spinlock 以及非 PREEMPT_RT rwlock 的初始化、错误诊断和
 * 体系结构原语包装。调试字段用于尽早发现未初始化、递归和错误所有者操作，不替代锁本身的同步。
 */

#include <linux/spinlock.h>
#include <linux/nmi.h>
#include <linux/interrupt.h>
#include <linux/debug_locks.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/pid.h>

/*
 * 初始化调用者提供的 raw spinlock lock；name/key/inner 仅在 DEBUG_LOCK_ALLOC 下建立 lockdep
 * 类与等待类型，随后无条件写入体系结构未锁状态、magic 和无 owner 哨兵。无返回值；调用前不得
 * 持有或并发使用该对象，初始化后对象仍归调用者管理。
 */
void __raw_spin_lock_init(raw_spinlock_t *lock, const char *name,
			  struct lock_class_key *key, short inner)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	/*
	 * Make sure we are not reinitializing a held lock:
	 */
	/* 先确认目标内存不含当前仍由任务持有的锁，防止把活动依赖图节点重新初始化。 */
	debug_check_no_locks_freed((void *)lock, sizeof(*lock));
	/* 以调用者提供的稳定名称、类 key 和 inner 等待类型初始化 lockdep map。 */
	lockdep_init_map_wait(&lock->dep_map, name, key, 0, inner);
#endif
	/* 重建实际锁字和调试哨兵；owner_cpu=-1 表示没有已登记的持有 CPU。 */
	lock->raw_lock = (arch_spinlock_t)__ARCH_SPIN_LOCK_UNLOCKED;
	lock->magic = SPINLOCK_MAGIC;
	lock->owner = SPINLOCK_OWNER_INIT;
	lock->owner_cpu = -1;
}

EXPORT_SYMBOL(__raw_spin_lock_init);

#ifndef CONFIG_PREEMPT_RT
/*
 * 初始化调用者提供的非 RT rwlock lock；name/key 在 DEBUG_LOCK_ALLOC 下标识 lockdep 类，等待
 * 类型固定为 LD_WAIT_CONFIG。无返回值；要求对象未被持有或并发访问，完成后由调用者管理其生命周期。
 */
void __rwlock_init(rwlock_t *lock, const char *name,
		   struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	/*
	 * Make sure we are not reinitializing a held lock:
	 */
	/* 与 spin 初始化相同，先检查并报告目标范围内仍由当前任务持有的 lockdep 对象。 */
	debug_check_no_locks_freed((void *)lock, sizeof(*lock));
	/* 普通 rwlock 可在 RT 外配置等待，故使用 LD_WAIT_CONFIG 初始化依赖图映射。 */
	lockdep_init_map_wait(&lock->dep_map, name, key, 0, LD_WAIT_CONFIG);
#endif
	/* 写入体系结构未锁值、rwlock 专用 magic 和 writer 未登记哨兵。 */
	lock->raw_lock = (arch_rwlock_t) __ARCH_RW_LOCK_UNLOCKED;
	lock->magic = RWLOCK_MAGIC;
	lock->owner = SPINLOCK_OWNER_INIT;
	lock->owner_cpu = -1;
}

EXPORT_SYMBOL(__rwlock_init);
#endif

/*
 * 打印 raw spinlock lock 的错误原因 msg、当前 CPU/任务以及 magic/owner 快照，最后输出调用栈。
 * 无返回值且不获取目标锁；诊断可能运行在锁已损坏的原子上下文。owner 不持有 task 引用，因此只作
 * 尽力诊断，READ_ONCE 仅防止撕裂/重复读取，不能赋予被指对象生命周期保证。
 */
static void spin_dump(raw_spinlock_t *lock, const char *msg)
{
	/* owner 是一次诊断快照；初始化哨兵转换为 NULL，统一走“无 owner”打印分支。 */
	struct task_struct *owner = READ_ONCE(lock->owner);

	if (owner == SPINLOCK_OWNER_INIT)
		owner = NULL;
	printk(KERN_EMERG "BUG: spinlock %s on CPU#%d, %s/%d\n",
		msg, raw_smp_processor_id(),
		current->comm, task_pid_nr(current));
	printk(KERN_EMERG " lock: %pS, .magic: %08x, .owner: %s/%d, "
			".owner_cpu: %d\n",
		lock, READ_ONCE(lock->magic),
		owner ? owner->comm : "<none>",
		owner ? task_pid_nr(owner) : -1,
		READ_ONCE(lock->owner_cpu));
	dump_stack();
}

/*
 * 报告 raw spinlock lock 的首个调试错误 msg；无返回值，不持锁也不修复锁状态。
 * `debug_locks_off()` 只有本次成功关闭全局调试且非 silent 时返回真，因此只打印首错，并压制静默
 * 自测或级联错误；并发报告者中只有赢得全局关闭转换的一方进入 spin_dump()。
 */
static void spin_bug(raw_spinlock_t *lock, const char *msg)
{
	if (!debug_locks_off())
		return;

	spin_dump(lock, msg);
}

/*
 * cond 为异常条件，lock/msg 为诊断上下文；仅在 unlikely(cond) 为真时调用首错门控。
 * 宏没有 do/while 包装，当前所有调用都作为独立语句使用；实参按实际分支求值且无返回值语义。
 */
#define SPIN_BUG_ON(cond, lock, msg) if (unlikely(cond)) spin_bug(lock, msg)

/*
 * 在阻塞获取 lock 前校验 magic，并拒绝当前任务递归或同一 CPU 的其他上下文递归；无返回值。
 * 只读取诊断字段，不取得实际锁；发现错误只关闭调试并报告，调用路径仍会继续进入体系结构获取。
 */
static inline void
debug_spin_lock_before(raw_spinlock_t *lock)
{
	SPIN_BUG_ON(READ_ONCE(lock->magic) != SPINLOCK_MAGIC, lock, "bad magic");
	SPIN_BUG_ON(READ_ONCE(lock->owner) == current, lock, "recursion");
	SPIN_BUG_ON(READ_ONCE(lock->owner_cpu) == raw_smp_processor_id(),
							lock, "cpu recursion");
}

/*
 * 在体系结构锁已成功取得后，把当前 CPU 和任务发布为 lock 的诊断 owner；无返回值。
 * 调用者已持有实际锁，WRITE_ONCE 使故障读取获得单次字段写，但这不是额外的所有权同步协议。
 */
static inline void debug_spin_lock_after(raw_spinlock_t *lock)
{
	WRITE_ONCE(lock->owner_cpu, raw_smp_processor_id());
	WRITE_ONCE(lock->owner, current);
}

/*
 * 在实际释放 lock 前验证 magic、锁定状态、任务 owner 和 CPU owner，随后清除诊断 owner；无返回值。
 * 调用者必须仍持有体系结构锁；即使诊断失败，首错报告返回后也继续清字段，实际解锁由外层完成。
 */
static inline void debug_spin_unlock(raw_spinlock_t *lock)
{
	SPIN_BUG_ON(lock->magic != SPINLOCK_MAGIC, lock, "bad magic");
	SPIN_BUG_ON(!raw_spin_is_locked(lock), lock, "already unlocked");
	SPIN_BUG_ON(lock->owner != current, lock, "wrong owner");
	SPIN_BUG_ON(lock->owner_cpu != raw_smp_processor_id(),
							lock, "wrong CPU");
	WRITE_ONCE(lock->owner, SPINLOCK_OWNER_INIT);
	WRITE_ONCE(lock->owner_cpu, -1);
}

/*
 * We are now relying on the NMI watchdog to detect lockup instead of doing
 * the detection here with an unfair lock which can cause problem of its own.
 */
/*
 * 锁死检测交给 NMI watchdog；这里不再用不公平的额外锁自行计时，以免诊断锁本身制造饥饿或死锁。
 */
/*
 * 阻塞取得 raw spinlock lock；进入时外层已建立所需抢占/IRQ 上下文，返回时持有实际锁，并登记
 * MMIOWB 嵌套及当前诊断 owner。无返回值；调用者负责配对 do_raw_spin_unlock()。
 */
void do_raw_spin_lock(raw_spinlock_t *lock)
{
	/* 阶段一先检查静态标记与递归，再由体系结构原语真正等待并取得锁。 */
	debug_spin_lock_before(lock);
	arch_spin_lock(&lock->raw_lock);
	/* 阶段二在持锁后登记 MMIOWB 嵌套，最后发布仅供诊断的 owner。 */
	mmiowb_spin_lock();
	debug_spin_lock_after(lock);
}

/*
 * 尝试一次取得 raw spinlock lock；成功返回非零，并登记 MMIOWB/owner，失败返回 0 且不改这些状态。
 * 不阻塞；调用者只在成功时持锁并承担释放责任。此入口不执行阻塞路径的前置 magic/递归检查。
 */
int do_raw_spin_trylock(raw_spinlock_t *lock)
{
	/* ret 完整保留体系结构 trylock 的真假结果，并决定是否建立成功后的调试状态。 */
	int ret = arch_spin_trylock(&lock->raw_lock);

	if (ret) {
		/* 只有真实取得锁才能增加 MMIOWB 嵌套并发布 owner，失败路径必须无副作用。 */
		mmiowb_spin_lock();
		debug_spin_lock_after(lock);
	}
#ifndef CONFIG_SMP
	/*
	 * Must not happen on UP:
	 */
	/* UP 上没有并行持锁者，trylock 失败只可能暴露递归或损坏，因此强制进入错误报告。 */
	SPIN_BUG_ON(!ret, lock, "trylock failure on UP");
#endif
	return ret;
}

/*
 * 释放当前执行流持有的 raw spinlock lock；无返回值。先完成可能待提交的 MMIO 写屏障，再验证并
 * 清除诊断 owner，最后开放体系结构锁；调用前必须持锁，函数不改变锁对象生命周期。
 */
void do_raw_spin_unlock(raw_spinlock_t *lock)
{
	/* 屏障必须先于实际 unlock，保证锁交接观察到临界区 MMIO 写；诊断字段也在开放锁前清除。 */
	mmiowb_spin_unlock();
	debug_spin_unlock(lock);
	arch_spin_unlock(&lock->raw_lock);
}

#ifndef CONFIG_PREEMPT_RT
/*
 * 报告非 RT rwlock lock 的首个调试错误 msg，打印当前 CPU/任务、锁地址和栈；无返回值。
 * 与 spin_bug 相同，只有首个非 silent 的 `debug_locks_off()` 转换者输出，函数不持锁也不修复状态。
 */
static void rwlock_bug(rwlock_t *lock, const char *msg)
{
	if (!debug_locks_off())
		return;

	printk(KERN_EMERG "BUG: rwlock %s on CPU#%d, %s/%d, %p\n",
		msg, raw_smp_processor_id(), current->comm,
		task_pid_nr(current), lock);
	dump_stack();
}

/*
 * cond 为 rwlock 异常条件，lock/msg 提供诊断上下文；条件为真时调用首错门控。
 * 宏按独立语句使用且不返回值；未采用 do/while 是既有形式，调用处不能把它直接接入 else 链。
 */
#define RWLOCK_BUG_ON(cond, lock, msg) if (unlikely(cond)) rwlock_bug(lock, msg)

/*
 * 校验 magic 后阻塞取得 lock 的共享读持有；无返回值，出口由当前执行流持有一个 reader 引用。
 * reader 不写 owner/owner_cpu，因为多个读者可并存；调用者负责配对 do_raw_read_unlock()。
 */
void do_raw_read_lock(rwlock_t *lock)
{
	/* 调试层只验证对象初始化，真正的等待与 acquire 语义由体系结构 read-lock 原语提供。 */
	RWLOCK_BUG_ON(lock->magic != RWLOCK_MAGIC, lock, "bad magic");
	arch_read_lock(&lock->raw_lock);
}

/*
 * 尝试一次取得 lock 的共享读持有；成功返回非零，失败返回 0，不阻塞且不维护 reader owner。
 * 调用者只在成功时承担读释放责任；与阻塞读获取不同，本入口不预检 magic。
 */
int do_raw_read_trylock(rwlock_t *lock)
{
	/* ret 直接保存体系结构 trylock 结果；reader 成功路径没有额外共享状态要发布。 */
	int ret = arch_read_trylock(&lock->raw_lock);

#ifndef CONFIG_SMP
	/*
	 * Must not happen on UP:
	 */
	/* UP 的读锁原语恒可成功，失败说明对象或调用关系异常，故触发首错报告。 */
	RWLOCK_BUG_ON(!ret, lock, "trylock failure on UP");
#endif
	return ret;
}

/*
 * 校验 magic 后释放当前执行流持有的一个共享读引用；无返回值。
 * 不检查 owner，因为调试结构不枚举并发 readers；实际 release 语义由体系结构原语提供。
 */
void do_raw_read_unlock(rwlock_t *lock)
{
	RWLOCK_BUG_ON(lock->magic != RWLOCK_MAGIC, lock, "bad magic");
	arch_read_unlock(&lock->raw_lock);
}

/*
 * 在阻塞写获取前校验 lock 的 magic，并拒绝当前任务递归或同 CPU 不同上下文递归；无返回值。
 * 只检查 writer 诊断字段，不取得实际锁；报告后调用路径仍继续尝试体系结构写获取。
 */
static inline void debug_write_lock_before(rwlock_t *lock)
{
	RWLOCK_BUG_ON(lock->magic != RWLOCK_MAGIC, lock, "bad magic");
	RWLOCK_BUG_ON(READ_ONCE(lock->owner) == current, lock, "recursion");
	RWLOCK_BUG_ON(READ_ONCE(lock->owner_cpu) == raw_smp_processor_id(),
							lock, "cpu recursion");
}

/*
 * 在独占写锁已取得后记录当前 CPU 和任务为诊断 owner；无返回值。
 * caller 已排除 readers/writers，WRITE_ONCE 只稳定单次诊断访问，不额外延长 current 的生命周期。
 */
static inline void debug_write_lock_after(rwlock_t *lock)
{
	WRITE_ONCE(lock->owner_cpu, raw_smp_processor_id());
	WRITE_ONCE(lock->owner, current);
}

/*
 * 在实际写释放前校验 magic、任务 owner 和 CPU owner，再清除 writer 诊断字段；无返回值。
 * 调用者必须持有独占写锁；错误报告不会阻止后续字段清理和外层体系结构 unlock。
 */
static inline void debug_write_unlock(rwlock_t *lock)
{
	RWLOCK_BUG_ON(lock->magic != RWLOCK_MAGIC, lock, "bad magic");
	RWLOCK_BUG_ON(lock->owner != current, lock, "wrong owner");
	RWLOCK_BUG_ON(lock->owner_cpu != raw_smp_processor_id(),
							lock, "wrong CPU");
	WRITE_ONCE(lock->owner, SPINLOCK_OWNER_INIT);
	WRITE_ONCE(lock->owner_cpu, -1);
}

/*
 * 阻塞取得非 RT rwlock lock 的独占写持有；无返回值，出口排除所有 readers/writers，并登记当前
 * writer owner。进入时外层已建立所需执行上下文，调用者负责配对 do_raw_write_unlock()。
 */
void do_raw_write_lock(rwlock_t *lock)
{
	/* 先验证对象和递归，再取得体系结构写锁；只有真实成功后才发布 writer owner。 */
	debug_write_lock_before(lock);
	arch_write_lock(&lock->raw_lock);
	debug_write_lock_after(lock);
}

/*
 * 尝试一次取得 lock 的独占写持有；成功返回非零并登记 writer owner，失败返回 0 且不改诊断字段。
 * 不阻塞；调用者只在成功时承担写释放责任。本入口与读 trylock 一样不执行前置 magic 检查。
 */
int do_raw_write_trylock(rwlock_t *lock)
{
	/* ret 保存体系结构结果，严格控制 owner 只在成功取得独占锁后发布。 */
	int ret = arch_write_trylock(&lock->raw_lock);

	if (ret)
		debug_write_lock_after(lock);
#ifndef CONFIG_SMP
	/*
	 * Must not happen on UP:
	 */
	/* UP 的写 trylock 恒可成功；失败表明递归或状态损坏，必须触发首错诊断。 */
	RWLOCK_BUG_ON(!ret, lock, "trylock failure on UP");
#endif
	return ret;
}

/*
 * 释放当前执行流持有的独占写锁 lock；无返回值。先校验并清除 writer owner，再执行体系结构
 * write unlock，使其他 CPU 只能在调试字段已回到无 owner 状态后取得锁。
 */
void do_raw_write_unlock(rwlock_t *lock)
{
	debug_write_unlock(lock);
	arch_write_unlock(&lock->raw_lock);
}

#endif /* !CONFIG_PREEMPT_RT */
