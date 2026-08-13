// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (2004) Linus Torvalds
 *
 * Author: Zwane Mwaikambo <zwane@fsmlabs.com>
 *
 * Copyright (2004, 2005) Ingo Molnar
 *
 * This file contains the spinlock/rwlock implementations for the
 * SMP and the DEBUG_SPINLOCK cases. (UP-nondebug inlines them)
 *
 * Note that some architectures have special knowledge about the
 * stack frames of these functions in their profile_pc. If you
 * change anything significant here that could change the stack
 * frame contact the architecture maintainers.
 */
/*
 * 本文件为 SMP 或 DEBUG_SPINLOCK 配置提供 spinlock/rwlock 的函数体；无调试的单处理器构建会把
 * 对应操作内联。部分体系结构的 profile_pc 会识别这些函数的栈帧，因此改变函数边界、调用层次或
 * 栈布局前必须与体系结构维护者确认，不能把这里仅视为普通的包装代码。
 */

#include <linux/linkage.h>
#include <linux/preempt.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/debug_locks.h>
#include <linux/export.h>

#ifdef CONFIG_MMIOWB
#ifndef arch_mmiowb_state
/*
 * 默认 MMIOWB 状态按 CPU 保存：nesting_count 表示当前 CPU 嵌套持有的受跟踪自旋锁层数，
 * mmiowb_pending 记录锁内 MMIO 写需要在释放路径提交。体系结构未自带状态存储时才定义并导出它；
 * 调用方必须在禁抢占的锁区间访问，从而让 raw_cpu_ptr 始终指向同一 CPU 的实例。
 */
DEFINE_PER_CPU(struct mmiowb_state, __mmiowb_state);
EXPORT_PER_CPU_SYMBOL(__mmiowb_state);
#endif
#endif

/*
 * If lockdep is enabled then we use the non-preemption spin-ops
 * even on CONFIG_PREEMPT, because lockdep assumes that interrupts are
 * not re-enabled during lock-acquire (which the preempt-spin-ops do):
 */
/*
 * 启用 lockdep 时即使允许内核抢占，也使用不会在获取循环中重新打开中断的自旋操作，因为 lockdep
 * 假定一次锁获取期间中断状态连续。只有 GENERIC_LOCKBREAK 开启且未启用 DEBUG_LOCK_ALLOC 时，
 * 才走下方可在争用间隙恢复上下文的实现。
 */
#if !defined(CONFIG_GENERIC_LOCKBREAK) || defined(CONFIG_DEBUG_LOCK_ALLOC)
/*
 * The __lock_function inlines are taken from
 * spinlock : include/linux/spinlock_api_smp.h
 * rwlock   : include/linux/rwlock_api_smp.h
 */
/*
 * 此分支不另行生成函数：`__raw_spin_*` 内联体来自 spinlock_api_smp.h，非 RT 的
 * `__raw_read/write_*` 内联体来自 rwlock_api_smp.h，后面的导出包装会调用这些定义。
 */
#else

/*
 * 某些体系结构能在等待时向持锁 CPU 让出执行资源；未提供专用提示时退化为 cpu_relax()。
 * 参数 l 保留给体系结构观察具体锁字，默认实现不使用它。
 */
/*
 * Some architectures can relax in favour of the CPU owning the lock.
 */
#ifndef arch_read_relax
# define arch_read_relax(l)	cpu_relax()
#endif
#ifndef arch_write_relax
# define arch_write_relax(l)	cpu_relax()
#endif
#ifndef arch_spin_relax
# define arch_spin_relax(l)	cpu_relax()
#endif

/*
 * 这里集中生成体积较大的内部获取函数，避免每个调用点重复内联。锁可能长期被占用：失败轮次先恢复
 * 本 CPU 的抢占/中断状态，使其可被调度，再调用体系结构 relax 提示持锁 CPU 尽快推进；成功轮次
 * 则保留相应上下文状态，交由配对 unlock 恢复。
 *
 * op 选择 spin/read/write 操作族，locktype 选择参数类型，lock_ctx_op 展开 sparse 的独占或共享
 * 获取标记；这些是预处理期标识符，不拥有运行期生命周期。
 *
 * 四个生成定义的契约依次为：普通获取阻塞至成功并保持禁抢占；irqsave 获取还保存/关闭 IRQ 并
 * 返回进入标志；irq 获取丢弃旧标志并保持 IRQ 关闭；bh 获取成功后禁止本地 BH，同时把硬 IRQ
 * 恢复到调用前状态。每轮失败都会完整恢复本轮改变的上下文再 relax，只有成功轮次把锁和对应的
 * 上下文恢复责任交给调用者。bh 路径必须同时排除 softirq，故先使用 irqsave 获取，再关闭 BH
 * 并恢复进入时的硬 IRQ 状态。
 */
/*
 * We build the __lock_function inlines here. They are too large for
 * inlining all over the place, but here is only one user per function
 * which embeds them into the calling _lock_function below.
 *
 * This could be a long-held lock. We both prepare to spin for a long
 * time (making _this_ CPU preemptible if possible), and we also signal
 * towards that other CPU that it should break the lock ASAP.
 */
#define BUILD_LOCK_OPS(op, locktype, lock_ctx_op)			\
static void __lockfunc __raw_##op##_lock(locktype##_t *lock)		\
	lock_ctx_op(lock)						\
{									\
	for (;;) {							\
		preempt_disable();					\
		if (likely(do_raw_##op##_trylock(lock)))		\
			break;						\
		preempt_enable();					\
									\
		arch_##op##_relax(&lock->raw_lock);			\
	}								\
}									\
									\
static unsigned long __lockfunc __raw_##op##_lock_irqsave(locktype##_t *lock) \
	lock_ctx_op(lock)						\
{									\
	unsigned long flags;						\
									\
	for (;;) {							\
		preempt_disable();					\
		local_irq_save(flags);					\
		if (likely(do_raw_##op##_trylock(lock)))		\
			break;						\
		local_irq_restore(flags);				\
		preempt_enable();					\
									\
		arch_##op##_relax(&lock->raw_lock);			\
	}								\
									\
	return flags;							\
}									\
									\
static void __lockfunc __raw_##op##_lock_irq(locktype##_t *lock)	\
	lock_ctx_op(lock)						\
{									\
	_raw_##op##_lock_irqsave(lock);					\
}									\
									\
static void __lockfunc __raw_##op##_lock_bh(locktype##_t *lock)		\
	lock_ctx_op(lock)						\
{									\
	unsigned long flags;						\
									\
	/*							*/	\
	/* Careful: we must exclude softirqs too, hence the	*/	\
	/* irq-disabling. We use the generic preemption-aware	*/	\
	/* function:						*/	\
	/**/								\
	flags = _raw_##op##_lock_irqsave(lock);				\
	local_bh_disable();						\
	local_irq_restore(flags);					\
}									\

/*
 * Build preemption-friendly versions of the following
 * lock-spinning functions:
 *
 *         __[spin|read|write]_lock()
 *         __[spin|read|write]_lock_irq()
 *         __[spin|read|write]_lock_irqsave()
 *         __[spin|read|write]_lock_bh()
 */
/*
 * 为 spin 生成普通、irq、irqsave、bh 四种可恢复争用上下文的获取函数；非 PREEMPT_RT 构建还为
 * read/write 各生成同样四种。PREEMPT_RT 的普通 rwlock 由可睡眠替代实现负责，不能使用这里的
 * 原始忙等读写锁路径。
 */
BUILD_LOCK_OPS(spin, raw_spinlock, __acquires);

#ifndef CONFIG_PREEMPT_RT
BUILD_LOCK_OPS(read, rwlock, __acquires_shared);
BUILD_LOCK_OPS(write, rwlock, __acquires);
#endif

#endif

/*
 * 下列配置门只在相应 API 未被头文件映射为内联实现时生成真实函数，并导出同名符号供内核其他对象
 * 调用。包装层不增加新的状态机；获取/释放的上下文契约完全继承其 `__raw_*` 内部实现。
 */
#ifndef CONFIG_INLINE_SPIN_TRYLOCK
/*
 * 尝试获取 lock 一次；成功返回 1，并由当前执行流持锁且保持禁抢占，失败返回 0 且恢复抢占状态。
 * 不等待、不转移 lock 所指对象的所有权；调用者只在成功时执行配对释放。
 */
noinline int __lockfunc _raw_spin_trylock(raw_spinlock_t *lock)
{
	return __raw_spin_trylock(lock);
}
EXPORT_SYMBOL(_raw_spin_trylock);
#endif

#ifndef CONFIG_INLINE_SPIN_TRYLOCK_BH
/*
 * 在禁止本地 BH 的条件下尝试获取 lock；成功返回 1 并保持 BH 禁止，失败返回 0 并恢复 BH 状态。
 * 锁对象由调用者管理，仅成功路径取得临界区责任。
 */
noinline int __lockfunc _raw_spin_trylock_bh(raw_spinlock_t *lock)
{
	return __raw_spin_trylock_bh(lock);
}
EXPORT_SYMBOL(_raw_spin_trylock_bh);
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK
/*
 * 阻塞获取 raw spinlock lock；返回时当前执行流持锁且抢占保持禁止，无返回值。
 * 此非内联入口只委托内部实现，调用者必须用匹配的 raw spin unlock 释放。
 */
noinline void __lockfunc _raw_spin_lock(raw_spinlock_t *lock)
{
	__raw_spin_lock(lock);
}
EXPORT_SYMBOL(_raw_spin_lock);
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK_IRQSAVE
/*
 * 保存本地 IRQ 状态后阻塞获取 lock；返回进入时 flags，成功出口保持 IRQ 关闭及抢占禁止。
 * flags 必须原样交给 irqrestore 释放，锁对象本身仍归调用者管理。
 */
noinline unsigned long __lockfunc _raw_spin_lock_irqsave(raw_spinlock_t *lock)
{
	return __raw_spin_lock_irqsave(lock);
}
EXPORT_SYMBOL(_raw_spin_lock_irqsave);
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK_IRQ
/*
 * 关闭本地 IRQ 并阻塞获取 lock；成功后持锁、IRQ 关闭且抢占禁止，无返回值。
 * 该接口不保存旧 IRQ 状态，要求调用者以 raw_spin_unlock_irq() 配对。
 */
noinline void __lockfunc _raw_spin_lock_irq(raw_spinlock_t *lock)
{
	__raw_spin_lock_irq(lock);
}
EXPORT_SYMBOL(_raw_spin_lock_irq);
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK_BH
/*
 * 禁止本地 BH 后阻塞获取 lock；返回时持锁且 BH 保持禁止，硬 IRQ 恢复为调用前状态。
 * 无返回值，调用者负责以 raw_spin_unlock_bh() 同时释放锁并恢复 BH。
 */
noinline void __lockfunc _raw_spin_lock_bh(raw_spinlock_t *lock)
{
	__raw_spin_lock_bh(lock);
}
EXPORT_SYMBOL(_raw_spin_lock_bh);
#endif

#ifdef CONFIG_UNINLINE_SPIN_UNLOCK
/*
 * 释放当前执行流持有的 lock，并恢复普通获取路径留下的抢占状态；无返回值。
 * 调用前必须持有该锁，本函数不销毁或转移锁对象。
 */
noinline void __lockfunc _raw_spin_unlock(raw_spinlock_t *lock)
{
	__raw_spin_unlock(lock);
}
EXPORT_SYMBOL(_raw_spin_unlock);
#endif

#ifndef CONFIG_INLINE_SPIN_UNLOCK_IRQRESTORE
/*
 * 释放当前执行流持有的 lock，再按配对 irqsave 获取返回的 flags 恢复本地 IRQ 与抢占状态。
 * flags 是值参数且仅用于恢复上下文；调用前必须持锁，无返回值。
 */
noinline void __lockfunc _raw_spin_unlock_irqrestore(raw_spinlock_t *lock, unsigned long flags)
{
	__raw_spin_unlock_irqrestore(lock, flags);
}
EXPORT_SYMBOL(_raw_spin_unlock_irqrestore);
#endif

#ifndef CONFIG_INLINE_SPIN_UNLOCK_IRQ
/*
 * 释放当前执行流持有的 lock，随后打开本地 IRQ 并恢复抢占；无返回值。
 * 仅与明确关闭 IRQ 的 raw_spin_lock_irq() 配对，不能代替 irqrestore 版本。
 */
noinline void __lockfunc _raw_spin_unlock_irq(raw_spinlock_t *lock)
{
	__raw_spin_unlock_irq(lock);
}
EXPORT_SYMBOL(_raw_spin_unlock_irq);
#endif

#ifndef CONFIG_INLINE_SPIN_UNLOCK_BH
/*
 * 释放当前执行流持有的 lock，并恢复配对 bh 获取所禁止的本地 bottom half；无返回值。
 * 调用前必须持锁，锁对象的存储期仍由调用者负责。
 */
noinline void __lockfunc _raw_spin_unlock_bh(raw_spinlock_t *lock)
{
	__raw_spin_unlock_bh(lock);
}
EXPORT_SYMBOL(_raw_spin_unlock_bh);
#endif

#ifndef CONFIG_PREEMPT_RT
/*
 * 非 RT 构建的普通 rwlock 是禁抢占的原始读写自旋锁，因而在这里提供与 spin 包装对称的读写入口；
 * PREEMPT_RT 把普通 rwlock 替换为可睡眠实现，由 spinlock_rt.c 提供，必须跳过本组符号。
 */

#ifndef CONFIG_INLINE_READ_TRYLOCK
/*
 * 尝试取得 lock 的共享读持有；成功返回 1 并保持禁抢占，失败返回 0 且恢复抢占状态。
 * 不等待；只有成功调用者取得读侧释放责任，rwlock 对象的存储所有权不变。
 */
noinline int __lockfunc _raw_read_trylock(rwlock_t *lock)
{
	return __raw_read_trylock(lock);
}
EXPORT_SYMBOL(_raw_read_trylock);
#endif

#ifndef CONFIG_INLINE_READ_LOCK
/*
 * 阻塞取得 lock 的共享读持有；返回时当前执行流持有读锁且抢占保持禁止，无返回值。
 * 多个 reader 可并存，调用者必须执行匹配的 raw read unlock。
 */
noinline void __lockfunc _raw_read_lock(rwlock_t *lock)
{
	__raw_read_lock(lock);
}
EXPORT_SYMBOL(_raw_read_lock);
#endif

#ifndef CONFIG_INLINE_READ_LOCK_IRQSAVE
/*
 * 保存并关闭本地 IRQ 后阻塞取得共享读锁；返回旧 flags，出口保持 IRQ 关闭与抢占禁止。
 * 调用者必须把 flags 交给 read_unlock_irqrestore()，锁对象不会被本函数接管。
 */
noinline unsigned long __lockfunc _raw_read_lock_irqsave(rwlock_t *lock)
{
	return __raw_read_lock_irqsave(lock);
}
EXPORT_SYMBOL(_raw_read_lock_irqsave);
#endif

#ifndef CONFIG_INLINE_READ_LOCK_IRQ
/*
 * 关闭本地 IRQ 后阻塞取得 lock 的共享读持有；成功出口保持 IRQ 关闭与抢占禁止，无返回值。
 * 该入口与 read_unlock_irq() 配对，不保存调用前 IRQ 状态。
 */
noinline void __lockfunc _raw_read_lock_irq(rwlock_t *lock)
{
	__raw_read_lock_irq(lock);
}
EXPORT_SYMBOL(_raw_read_lock_irq);
#endif

#ifndef CONFIG_INLINE_READ_LOCK_BH
/*
 * 禁止本地 BH 后阻塞取得 lock 的共享读持有；出口保持 BH 禁止而硬 IRQ 状态不额外改变。
 * 无返回值，调用者以 read_unlock_bh() 释放共享持有并恢复 BH。
 */
noinline void __lockfunc _raw_read_lock_bh(rwlock_t *lock)
{
	__raw_read_lock_bh(lock);
}
EXPORT_SYMBOL(_raw_read_lock_bh);
#endif

#ifndef CONFIG_INLINE_READ_UNLOCK
/*
 * 释放当前执行流对 lock 的共享读持有并恢复普通读获取留下的抢占状态；无返回值。
 * 调用前必须持有一个读侧引用，本函数不等待也不销毁锁对象。
 */
noinline void __lockfunc _raw_read_unlock(rwlock_t *lock)
{
	__raw_read_unlock(lock);
}
EXPORT_SYMBOL(_raw_read_unlock);
#endif

#ifndef CONFIG_INLINE_READ_UNLOCK_IRQRESTORE
/*
 * 释放共享读持有，再用配对 irqsave 获取产生的 flags 恢复 IRQ 与抢占状态；无返回值。
 * flags 只描述调用者原上下文，必须与同一次获取配对。
 */
noinline void __lockfunc _raw_read_unlock_irqrestore(rwlock_t *lock, unsigned long flags)
{
	__raw_read_unlock_irqrestore(lock, flags);
}
EXPORT_SYMBOL(_raw_read_unlock_irqrestore);
#endif

#ifndef CONFIG_INLINE_READ_UNLOCK_IRQ
/*
 * 释放当前共享读持有，随后打开本地 IRQ 并恢复抢占；无返回值。
 * 仅用于与 read_lock_irq() 配对的路径，调用前必须持有 lock。
 */
noinline void __lockfunc _raw_read_unlock_irq(rwlock_t *lock)
{
	__raw_read_unlock_irq(lock);
}
EXPORT_SYMBOL(_raw_read_unlock_irq);
#endif

#ifndef CONFIG_INLINE_READ_UNLOCK_BH
/*
 * 释放当前共享读持有并重新允许本地 bottom half；无返回值。
 * 调用前必须持有由 read_lock_bh() 取得的 lock，存储所有权仍归调用者。
 */
noinline void __lockfunc _raw_read_unlock_bh(rwlock_t *lock)
{
	__raw_read_unlock_bh(lock);
}
EXPORT_SYMBOL(_raw_read_unlock_bh);
#endif

#ifndef CONFIG_INLINE_WRITE_TRYLOCK
/*
 * 尝试一次取得 lock 的独占写持有；成功返回 1 并保持禁抢占，失败返回 0 且恢复抢占状态。
 * 不等待；只有成功调用者承担配对写释放责任。
 */
noinline int __lockfunc _raw_write_trylock(rwlock_t *lock)
{
	return __raw_write_trylock(lock);
}
EXPORT_SYMBOL(_raw_write_trylock);
#endif

#ifndef CONFIG_INLINE_WRITE_LOCK
/*
 * 阻塞取得 lock 的独占写持有；返回时排除所有 reader/writer 且抢占保持禁止，无返回值。
 * 调用者必须以匹配的 raw write unlock 释放，锁对象的生命周期不变。
 */
noinline void __lockfunc _raw_write_lock(rwlock_t *lock)
{
	__raw_write_lock(lock);
}
EXPORT_SYMBOL(_raw_write_lock);

#ifndef CONFIG_DEBUG_LOCK_ALLOC
/*
 * 未启用 lockdep 分配跟踪时没有子类关系可记录：先以 void 求值并丢弃 subclass，再退化为普通
 * `__raw_write_lock()`；逗号表达式仍保证实参只按 C 表达式规则求值一次。
 */
#define __raw_write_lock_nested(lock, subclass)	__raw_write_lock(((void)(subclass), (lock)))
#endif

/*
 * 以 subclass 指定的 lockdep 嵌套层级阻塞取得 lock 的独占写持有；无返回值，出口保持禁抢占。
 * DEBUG_LOCK_ALLOC 关闭时 subclass 仅被求值后丢弃；调用者始终负责配对写释放。
 */
void __lockfunc _raw_write_lock_nested(rwlock_t *lock, int subclass)
{
	__raw_write_lock_nested(lock, subclass);
}
EXPORT_SYMBOL(_raw_write_lock_nested);
#endif

#ifndef CONFIG_INLINE_WRITE_LOCK_IRQSAVE
/*
 * 保存并关闭本地 IRQ 后阻塞取得独占写锁；返回旧 flags，出口保持 IRQ 关闭与抢占禁止。
 * flags 必须交给同一临界区的 write_unlock_irqrestore()，本函数不接管锁存储。
 */
noinline unsigned long __lockfunc _raw_write_lock_irqsave(rwlock_t *lock)
{
	return __raw_write_lock_irqsave(lock);
}
EXPORT_SYMBOL(_raw_write_lock_irqsave);
#endif

#ifndef CONFIG_INLINE_WRITE_LOCK_IRQ
/*
 * 关闭本地 IRQ 后阻塞取得 lock 的独占写持有；出口保持 IRQ 关闭与抢占禁止，无返回值。
 * 不保存旧 IRQ 状态，调用者必须用 write_unlock_irq() 配对。
 */
noinline void __lockfunc _raw_write_lock_irq(rwlock_t *lock)
{
	__raw_write_lock_irq(lock);
}
EXPORT_SYMBOL(_raw_write_lock_irq);
#endif

#ifndef CONFIG_INLINE_WRITE_LOCK_BH
/*
 * 禁止本地 BH 后阻塞取得 lock 的独占写持有；硬 IRQ 恢复为进入状态，BH 保持禁止。
 * 无返回值，调用者以 write_unlock_bh() 释放锁并恢复 bottom half。
 */
noinline void __lockfunc _raw_write_lock_bh(rwlock_t *lock)
{
	__raw_write_lock_bh(lock);
}
EXPORT_SYMBOL(_raw_write_lock_bh);
#endif

#ifndef CONFIG_INLINE_WRITE_UNLOCK
/*
 * 释放当前执行流对 lock 的独占写持有并恢复普通写获取留下的抢占状态；无返回值。
 * 调用前必须持有写锁，本函数不改变锁对象的存储生命周期。
 */
noinline void __lockfunc _raw_write_unlock(rwlock_t *lock)
{
	__raw_write_unlock(lock);
}
EXPORT_SYMBOL(_raw_write_unlock);
#endif

#ifndef CONFIG_INLINE_WRITE_UNLOCK_IRQRESTORE
/*
 * 释放独占写持有，再按配对 irqsave 获取的 flags 恢复本地 IRQ 与抢占状态；无返回值。
 * flags 必须来自同一次获取，调用前当前执行流必须持有 lock。
 */
noinline void __lockfunc _raw_write_unlock_irqrestore(rwlock_t *lock, unsigned long flags)
{
	__raw_write_unlock_irqrestore(lock, flags);
}
EXPORT_SYMBOL(_raw_write_unlock_irqrestore);
#endif

#ifndef CONFIG_INLINE_WRITE_UNLOCK_IRQ
/*
 * 释放当前独占写持有，随后打开本地 IRQ 并恢复抢占；无返回值。
 * 仅与 write_lock_irq() 配对，不能用于需要恢复任意旧 IRQ 状态的路径。
 */
noinline void __lockfunc _raw_write_unlock_irq(rwlock_t *lock)
{
	__raw_write_unlock_irq(lock);
}
EXPORT_SYMBOL(_raw_write_unlock_irq);
#endif

#ifndef CONFIG_INLINE_WRITE_UNLOCK_BH
/*
 * 释放当前独占写持有并重新允许本地 bottom half；无返回值。
 * 调用前必须持有由 write_lock_bh() 取得的 lock，锁对象仍归调用者管理。
 */
noinline void __lockfunc _raw_write_unlock_bh(rwlock_t *lock)
{
	__raw_write_unlock_bh(lock);
}
EXPORT_SYMBOL(_raw_write_unlock_bh);
#endif

#endif /* !CONFIG_PREEMPT_RT */

#ifdef CONFIG_DEBUG_LOCK_ALLOC

/*
 * 按 subclass 登记 lockdep 嵌套关系并阻塞获取 raw spinlock lock；出口持锁且保持禁抢占。
 * subclass 只描述同一锁类的合法嵌套层级，调用者仍必须以普通 raw spin unlock 配对。
 */
void __lockfunc _raw_spin_lock_nested(raw_spinlock_t *lock, int subclass)
{
	/* 阶段一：固定当前 CPU，避免获取中的执行上下文迁移。 */
	preempt_disable();
	/* 阶段二：先把指定子类的依赖边加入 lockdep，再尝试快路径并在需要时进入争用慢路径。 */
	spin_acquire(&lock->dep_map, subclass, 0, _RET_IP_);
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}
EXPORT_SYMBOL(_raw_spin_lock_nested);

/*
 * 保存并关闭本地 IRQ，以 subclass 登记嵌套关系后阻塞获取 lock；返回旧 IRQ flags。
 * 成功出口持锁、IRQ 关闭且抢占禁止；调用者须用 raw_spin_unlock_irqrestore() 配对。
 */
unsigned long __lockfunc _raw_spin_lock_irqsave_nested(raw_spinlock_t *lock,
						   int subclass)
{
	/* flags 保存调用者进入时的本地 IRQ 状态，只能交给本临界区的恢复路径。 */
	unsigned long flags;

	/* 阶段一：先关闭 IRQ 再禁止抢占，使 lockdep 观察到连续且稳定的获取上下文。 */
	local_irq_save(flags);
	preempt_disable();
	/* 阶段二：登记子类依赖并完成实际竞争；成功后把进入状态返回给调用者保管。 */
	spin_acquire(&lock->dep_map, subclass, 0, _RET_IP_);
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
	return flags;
}
EXPORT_SYMBOL(_raw_spin_lock_irqsave_nested);

/*
 * 以已持有的 nest_lock 作为依赖锚点，登记后阻塞获取 raw spinlock lock；无返回值。
 * nest_lock 只供 lockdep 表达嵌套关系，本函数不获取或释放它；出口持有 lock 且保持禁抢占。
 */
void __lockfunc _raw_spin_lock_nest_lock(raw_spinlock_t *lock,
				     struct lockdep_map *nest_lock)
{
	/* 阶段一固定执行 CPU；阶段二登记相对于 nest_lock 的依赖并进入实际锁竞争。 */
	preempt_disable();
	spin_acquire_nest(&lock->dep_map, 0, 0, nest_lock, _RET_IP_);
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}
EXPORT_SYMBOL(_raw_spin_lock_nest_lock);

#endif

/*
 * 判断代码地址 addr 是否落在链接器汇集的 `__lockfunc` 函数区间；命中返回 1，否则返回 0。
 * 只读取链接器符号，不取得锁、无对象所有权变化；notrace 避免分析锁函数时再次进入跟踪路径。
 */
notrace int in_lock_functions(unsigned long addr)
{
	/* Linker adds these: start and end of __lockfunc functions */
	/* 链接器为 `.spinlock.text` 生成首尾符号；end 指向区间后一地址，因此判定采用左闭右开。 */
	extern char __lock_text_start[], __lock_text_end[];

	return addr >= (unsigned long)__lock_text_start
	&& addr < (unsigned long)__lock_text_end;
}
EXPORT_SYMBOL(in_lock_functions);

#if defined(CONFIG_PROVE_LOCKING) && defined(CONFIG_PREEMPT_RT)
/*
 * 验证当前上下文是 softirq 而非 hardirq/NMI；无参数和返回值，失败时由 lockdep 断言告警。
 * PREEMPT_RT 的 local-lock 嵌套 BH 路径因头文件包含依赖无法直接展开断言，故把检查移到此处；
 * notrace 防止断言自身再次进入跟踪路径。
 */
void notrace lockdep_assert_in_softirq_func(void)
{
	lockdep_assert_in_softirq();
}
EXPORT_SYMBOL(lockdep_assert_in_softirq_func);
#endif
