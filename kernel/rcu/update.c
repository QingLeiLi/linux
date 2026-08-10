// SPDX-License-Identifier: GPL-2.0+
/*
 * Read-Copy Update mechanism for mutual exclusion
 *
 * Copyright IBM Corporation, 2001
 *
 * Authors: Dipankar Sarma <dipankar@in.ibm.com>
 *	    Manfred Spraul <manfred@colorfullife.com>
 *
 * Based on the original work by Paul McKenney <paulmck@linux.ibm.com>
 * and inputs from Rusty Russell, Andrea Arcangeli and Andi Kleen.
 * Papers:
 * http://www.rdrop.com/users/paulmck/paper/rclockpdcsproof.pdf
 * http://lse.sourceforge.net/locking/rclock_OLS.2001.05.01c.sc.pdf (OLS2001)
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *		http://lse.sourceforge.net/locking/rcupdate.html
 *
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/sched/signal.h>
#include <linux/sched/debug.h>
#include <linux/torture.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/percpu.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/mutex.h>
#include <linux/export.h>
#include <linux/hardirq.h>
#include <linux/delay.h>
#include <linux/moduleparam.h>
#include <linux/kthread.h>
#include <linux/tick.h>
#include <linux/rcupdate_wait.h>
#include <linux/sched/isolation.h>
#include <linux/kprobes.h>
#include <linux/slab.h>
#include <linux/irq_work.h>
#include <linux/rcupdate_trace.h>

#define CREATE_TRACE_POINTS

#include "rcu.h"

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "rcupdate."

#ifndef CONFIG_TINY_RCU
/*
 * ksysfs.c 定义的 rcu_expedited/rcu_normal 同时注册为只读模块参数：启动命令行
 * 可设初值，运行期则由 /sys/kernel 对应属性读写。rcu_normal 优先级更高；
 * TINY_RCU 没有可选择的树形宽限期实现，因此整组策略不存在。
 */
module_param(rcu_expedited, int, 0444);
module_param(rcu_normal, int, 0444);
/*
 * 启动结束后是否自动强制普通宽限期；PREEMPT_RT 默认启用以避免 expedited 路径
 * 的实时性代价。__read_mostly 不需要，因为该量主要在启动状态转换时读取。
 */
static int rcu_normal_after_boot = IS_ENABLED(CONFIG_PREEMPT_RT);
#if !defined(CONFIG_PREEMPT_RT) || defined(CONFIG_NO_HZ_FULL)
module_param(rcu_normal_after_boot, int, 0444);
#endif
#endif /* #ifndef CONFIG_TINY_RCU */

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/**
 * rcu_read_lock_held_common() - might we be in RCU-sched read-side critical section?
 * @ret:	Best guess answer if lockdep cannot be relied on
 *
 * Returns true if lockdep must be ignored, in which case ``*ret`` contains
 * the best guess described below.  Otherwise returns false, in which
 * case ``*ret`` tells the caller nothing and the caller should instead
 * consult lockdep.
 *
 * If CONFIG_DEBUG_LOCK_ALLOC is selected, set ``*ret`` to nonzero iff in an
 * RCU-sched read-side critical section.  In absence of
 * CONFIG_DEBUG_LOCK_ALLOC, this assumes we are in an RCU-sched read-side
 * critical section unless it can prove otherwise.  Note that disabling
 * of preemption (including disabling irqs) counts as an RCU-sched
 * read-side critical section.  This is useful for debug checks in functions
 * that required that they be called within an RCU-sched read-side
 * critical section.
 *
 * Check debug_lockdep_rcu_enabled() to prevent false positives during boot
 * and while lockdep is disabled.
 *
 * Note that if the CPU is in the idle loop from an RCU point of view (ie:
 * that we are in the section between ct_idle_enter() and ct_idle_exit())
 * then rcu_read_lock_held() sets ``*ret`` to false even if the CPU did an
 * rcu_read_lock().  The reason for this is that RCU ignores CPUs that are
 * in such a section, considering these as in extended quiescent state,
 * so such a CPU is effectively never in an RCU read-side critical section
 * regardless of what RCU primitives it invokes.  This state of affairs is
 * required --- we need to keep an RCU-free window in idle where the CPU may
 * possibly enter into low power mode. This way we can notice an extended
 * quiescent state to other CPUs that started a grace period. Otherwise
 * we would delay any grace period as long as we run in the idle task.
 *
 * Similarly, we avoid claiming an RCU read lock held if the current
 * CPU is offline.
 */
static bool rcu_read_lock_held_common(bool *ret)
{
	if (!debug_lockdep_rcu_enabled()) {
		*ret = true;
		return true;
	}
	if (!rcu_is_watching()) {
		*ret = false;
		return true;
	}
	if (!rcu_lockdep_current_cpu_online()) {
		*ret = false;
		return true;
	}
	return false;
}

int notrace rcu_read_lock_sched_held(void)
{
	bool ret;

	if (rcu_read_lock_held_common(&ret))
		return ret;
	return lock_is_held(&rcu_sched_lock_map) || !preemptible();
}
EXPORT_SYMBOL(rcu_read_lock_sched_held);
#endif

#ifndef CONFIG_TINY_RCU

/*
 * Should expedited grace-period primitives always fall back to their
 * non-expedited counterparts?  Intended for use within RCU.  Note
 * that if the user specifies both rcu_expedited and rcu_normal, then
 * rcu_normal wins.  (Except during the time period during boot from
 * when the first task is spawned until the rcu_set_runtime_mode()
 * core_initcall() is invoked, at which point everything is expedited.)
 */
/*
 * rcu_gp_is_normal() - 判断 expedited 请求是否必须退回普通宽限期。
 * 入参：无；返回布尔值，无 ownership 或可睡眠副作用。运行期 rcu_normal 非零
 * 即返回真；但 RCU_SCHEDULER_INIT 启动窗口强制加速，暂时忽略 normal。READ_ONCE
 * 只稳定这次策略读取，不等待并发 sysfs writer，也不改变已开始的宽限期。
 */
bool rcu_gp_is_normal(void)
{
	return READ_ONCE(rcu_normal) &&
	       rcu_scheduler_active != RCU_SCHEDULER_INIT;
}
EXPORT_SYMBOL_GPL(rcu_gp_is_normal);

/*
 * async-hurry 嵌套计数初值 1，使启动期 call_rcu() 回调不采用 lazy 延迟；只有
 * CONFIG_RCU_LAZY 下计数才参与决策。每次 hurry 必须与一次 relax 配对。
 */
static atomic_t rcu_async_hurry_nesting = ATOMIC_INIT(1);
/*
 * Should call_rcu() callbacks be processed with urgency or are
 * they OK being executed with arbitrary delays?
 */
/*
 * rcu_async_should_hurry() - 判断异步 call_rcu() 回调是否必须及时处理。
 * 入参：无；不启用 RCU_LAZY 时恒真，否则返回嵌套计数是否非零。原子读取无锁、
 * 不睡眠，只影响后续回调调度策略，不改变已经排队回调的 ownership。
 */
bool rcu_async_should_hurry(void)
{
	return !IS_ENABLED(CONFIG_RCU_LAZY) ||
	       atomic_read(&rcu_async_hurry_nesting);
}
EXPORT_SYMBOL_GPL(rcu_async_should_hurry);

/**
 * rcu_async_hurry - Make future async RCU callbacks not lazy.
 *
 * After a call to this function, future calls to call_rcu()
 * will be processed in a timely fashion.
 */
/*
 * rcu_async_hurry() - 取得一次“未来异步回调不可 lazy”的嵌套票据。
 * 入参：无；返回无直接值。仅 RCU_LAZY 配置执行原子递增；调用者随后必须配对
 * rcu_async_relax()，否则系统会长期保持及时处理策略。
 */
void rcu_async_hurry(void)
{
	if (IS_ENABLED(CONFIG_RCU_LAZY))
		atomic_inc(&rcu_async_hurry_nesting);
}
EXPORT_SYMBOL_GPL(rcu_async_hurry);

/**
 * rcu_async_relax - Make future async RCU callbacks lazy.
 *
 * After a call to this function, future calls to call_rcu()
 * will be processed in a lazy fashion.
 */
/*
 * rcu_async_relax() - 归还此前取得的一次 async-hurry 票据。
 * 入参：无；返回无直接值。仅 RCU_LAZY 配置原子递减；计数归零后未来回调才可
 * 延迟批处理。函数不校验配对，额外调用会破坏嵌套计数。
 */
void rcu_async_relax(void)
{
	if (IS_ENABLED(CONFIG_RCU_LAZY))
		atomic_dec(&rcu_async_hurry_nesting);
}
EXPORT_SYMBOL_GPL(rcu_async_relax);

/*
 * expedited 嵌套计数初值 1，使内核启动期默认加速同步宽限期；每个
 * rcu_expedite_gp() 必须由 rcu_unexpedite_gp() 配对。它表达叠加请求数量，
 * 与 sysfs 的永久策略位 rcu_expedited 是“或”关系，不能互相抵消。
 */
static atomic_t rcu_expedited_nesting = ATOMIC_INIT(1);
/*
 * Should normal grace-period primitives be expedited?  Intended for
 * use within RCU.  Note that this function takes the rcu_expedited
 * sysfs/boot variable and rcu_scheduler_active into account as well
 * as the rcu_expedite_gp() nesting.  So looping on rcu_unexpedite_gp()
 * until rcu_gp_is_expedited() returns false is a -really- bad idea.
 */
/*
 * rcu_gp_is_expedited() - 判断普通同步 RCU API 是否应走 expedited 实现。
 * 入参：无；返回全局策略位或嵌套请求是否为真。查询无锁、不可睡眠且不取得
 * ownership。不能通过循环 unexpedite 逼迫其变假：计数属于配对调用者，sysfs
 * 策略位也独立存在，额外递减会破坏其他调用者的嵌套不变量。原英文所说的
 * rcu_scheduler_active 影响在当前实现中通过启动期嵌套票据间接体现，并非本函数
 * 直接读取该变量。
 */
bool rcu_gp_is_expedited(void)
{
	return rcu_expedited || atomic_read(&rcu_expedited_nesting);
}
EXPORT_SYMBOL_GPL(rcu_gp_is_expedited);

/**
 * rcu_expedite_gp - Expedite future RCU grace periods
 *
 * After a call to this function, future calls to synchronize_rcu() and
 * friends act as the corresponding synchronize_rcu_expedited() function
 * had instead been called.
 */
/*
 * rcu_expedite_gp() - 为调用者增加一个“后续宽限期需加速”的嵌套请求。
 * 入参：无；返回无直接值。原子递增可在并发上下文使用，不睡眠；它只影响随后
 * 的策略选择，不追溯转换已开始的宽限期。调用者拥有一次必须配对撤销的逻辑票据。
 */
void rcu_expedite_gp(void)
{
	atomic_inc(&rcu_expedited_nesting);
}
EXPORT_SYMBOL_GPL(rcu_expedite_gp);

/**
 * rcu_unexpedite_gp - Cancel prior rcu_expedite_gp() invocation
 *
 * Undo a prior call to rcu_expedite_gp().  If all prior calls to
 * rcu_expedite_gp() are undone by a subsequent call to rcu_unexpedite_gp(),
 * and if the rcu_expedited sysfs/boot parameter is not set, then all
 * subsequent calls to synchronize_rcu() and friends will return to
 * their normal non-expedited behavior.
 */
/*
 * rcu_unexpedite_gp() - 撤销当前调用者此前取得的一次 expedited 请求。
 * 入参：无；返回无直接值。原子递减后，只有嵌套计数归零且 sysfs/boot 的
 * rcu_expedited 未设置，后续同步调用才可能恢复普通实现；函数不验证调用者是否
 * 真正持有票据，错误的额外调用会破坏全局计数。
 */
void rcu_unexpedite_gp(void)
{
	atomic_dec(&rcu_expedited_nesting);
}
EXPORT_SYMBOL_GPL(rcu_unexpedite_gp);

/* 启动状态发布位：只在启动结束时置真，之后供 rcutorture 等观察而不再清零。 */
static bool rcu_boot_ended __read_mostly;

/*
 * Inform RCU of the end of the in-kernel boot sequence.
 */
/*
 * rcu_end_inkernel_boot() - 结束 RCU 的内核启动加速阶段。
 * 入参：无；返回无直接值。调用者在启动序列末端单次调用；函数撤销初始 expedited
 * 和 async-hurry 票据，按 rcu_normal_after_boot 发布 normal 策略，最后置启动结束
 * 标志。顺序保证观察到 boot_ended 的测试代码面对的已是运行期策略。
 */
void rcu_end_inkernel_boot(void)
{
	rcu_unexpedite_gp();
	rcu_async_relax();
	if (rcu_normal_after_boot)
		WRITE_ONCE(rcu_normal, 1);
	rcu_boot_ended = true;
}

/*
 * Let rcutorture know when it is OK to turn it up to eleven.
 */
/*
 * rcu_inkernel_boot_has_ended() - 告知 rcutorture 何时可以开始高强度测试。
 * 入参：无；返回启动结束快照，无副作用、无 ownership 转移。“turn it up to
 * eleven”表示在基础启动约束解除后把压力提升到极限。
 */
bool rcu_inkernel_boot_has_ended(void)
{
	return rcu_boot_ended;
}
EXPORT_SYMBOL_GPL(rcu_inkernel_boot_has_ended);

#endif /* #ifndef CONFIG_TINY_RCU */

/*
 * 对所有非 SRCU 的同步宽限期等待 API 做一次端到端自检。
 * 仅在开启 CONFIG_PROVE_RCU 时编译进内核，用于在 RCU 状态机发生
 * 模式切换的"边界点"验证新模式下的同步原语行为是否符合预期。
 *
 * 为什么要在模式切换前后各调用一次？
 *   RCU 的同步原语在不同状态下语义不同：
 *     INACTIVE 阶段：synchronize_rcu() 退化为 barrier()（无宽限期）
 *     INIT     阶段：必须走完整宽限期，但 GP kthread 尚未启动
 *     RUNNING  阶段：GP kthread 驱动，全功能宽限期
 *   切换前调用可以捕捉旧语义下的 bug，切换后调用可以验证新语义是否
 *   正常工作，两次测试夹住切换点，精确定位问题出在哪个阶段。
 *
 * 为什么同时测 synchronize_rcu 和 synchronize_rcu_expedited？
 *   二者底层路径不同：expedited 版本通过 IPI 强制所有 CPU 快速通过
 *   静止状态，不依赖调度器自然切换；非 expedited 版本依赖各 CPU 自行
 *   上报静止状态。启动早期调度器行为受限，两条路径的正确性需分别验证。
 *
 * 注意：此函数在 CONFIG_PROVE_RCU=n 时是空操作，生产内核零开销。
 */
void rcu_test_sync_prims(void)
{
	if (!IS_ENABLED(CONFIG_PROVE_RCU))
		return;
	pr_info("Running RCU synchronous self tests\n");
	synchronize_rcu();
	synchronize_rcu_expedited();
}

#if !defined(CONFIG_TINY_RCU)

/*
 * 将 RCU 从 INIT 模式推进到 RUNNING 模式，在所有 RCU kthread 启动完毕后
 * 由 core_initcall 机制自动调用。
 *
 * 为什么需要 RUNNING 这个独立阶段，不能在 rcu_scheduler_starting() 里
 * 一步到位直接设置 RUNNING？
 *
 *   rcu_scheduler_starting() 在第一个任务创建之前调用，此时 GP kthread
 *   还不存在。若直接设为 RUNNING，依赖 kthread 的代码路径（如 kfree_rcu
 *   的批量回收 monitor）会立即尝试唤醒一个根本不存在的线程，导致崩溃。
 *   INIT 状态充当"多任务已启动、但 kthread 尚未就绪"的过渡缓冲区。
 *
 * 这里的调用顺序有严格要求：
 *   1. rcu_test_sync_prims()         ← 验证 INIT 阶段语义基线
 *   2. rcu_scheduler_active = RUNNING ← 原子切换到全功能模式
 *   3. kfree_rcu_scheduler_running() ← 激活 kfree_rcu 的 monitor kthread：
 *                                       遍历所有 CPU 的 krc 结构，若有积压
 *                                       的待释放对象则立即调度 drain work。
 *                                       必须在 RUNNING 之后调用，因为它依赖
 *                                       调度器能正常唤醒 delayed work。
 *   4. rcu_test_sync_prims()         ← 验证 RUNNING 阶段语义正确
 *
 * 另外，整个启动阶段（从 rcu_expedite_gp 到 rcu_end_inkernel_boot 调用
 * rcu_unexpedite_gp 之前）所有宽限期都被强制 expedited，以加快启动速度。
 * 切换到 RUNNING 本身不影响这一行为，expedited 的关闭由 rcu_end_inkernel_boot
 * 在 do_initcalls 结束后完成。
 */
static int __init rcu_set_runtime_mode(void)
{
	rcu_test_sync_prims();
	rcu_scheduler_active = RCU_SCHEDULER_RUNNING;
	kfree_rcu_scheduler_running();
	rcu_test_sync_prims();
	return 0;
}
core_initcall(rcu_set_runtime_mode);

#endif /* #if !defined(CONFIG_TINY_RCU) */

#ifdef CONFIG_DEBUG_LOCK_ALLOC
static struct lock_class_key rcu_lock_key;
struct lockdep_map rcu_lock_map = {
	.name = "rcu_read_lock",
	.key = &rcu_lock_key,
	.wait_type_outer = LD_WAIT_FREE,
	.wait_type_inner = LD_WAIT_CONFIG, /* PREEMPT_RT implies PREEMPT_RCU */
};
EXPORT_SYMBOL_GPL(rcu_lock_map);

static struct lock_class_key rcu_bh_lock_key;
struct lockdep_map rcu_bh_lock_map = {
	.name = "rcu_read_lock_bh",
	.key = &rcu_bh_lock_key,
	.wait_type_outer = LD_WAIT_FREE,
	.wait_type_inner = LD_WAIT_CONFIG, /* PREEMPT_RT makes BH preemptible. */
};
EXPORT_SYMBOL_GPL(rcu_bh_lock_map);

static struct lock_class_key rcu_sched_lock_key;
struct lockdep_map rcu_sched_lock_map = {
	.name = "rcu_read_lock_sched",
	.key = &rcu_sched_lock_key,
	.wait_type_outer = LD_WAIT_FREE,
	.wait_type_inner = LD_WAIT_SPIN,
};
EXPORT_SYMBOL_GPL(rcu_sched_lock_map);

// Tell lockdep when RCU callbacks are being invoked.
static struct lock_class_key rcu_callback_key;
struct lockdep_map rcu_callback_map =
	STATIC_LOCKDEP_MAP_INIT("rcu_callback", &rcu_callback_key);
EXPORT_SYMBOL_GPL(rcu_callback_map);

noinstr int notrace debug_lockdep_rcu_enabled(void)
{
	return rcu_scheduler_active != RCU_SCHEDULER_INACTIVE && READ_ONCE(debug_locks) &&
	       current->lockdep_recursion == 0;
}
EXPORT_SYMBOL_GPL(debug_lockdep_rcu_enabled);

/**
 * rcu_read_lock_held() - might we be in RCU read-side critical section?
 *
 * If CONFIG_DEBUG_LOCK_ALLOC is selected, returns nonzero iff in an RCU
 * read-side critical section.  In absence of CONFIG_DEBUG_LOCK_ALLOC,
 * this assumes we are in an RCU read-side critical section unless it can
 * prove otherwise.  This is useful for debug checks in functions that
 * require that they be called within an RCU read-side critical section.
 *
 * Checks debug_lockdep_rcu_enabled() to prevent false positives during boot
 * and while lockdep is disabled.
 *
 * Note that rcu_read_lock() and the matching rcu_read_unlock() must
 * occur in the same context, for example, it is illegal to invoke
 * rcu_read_unlock() in process context if the matching rcu_read_lock()
 * was invoked from within an irq handler.
 *
 * Note that rcu_read_lock() is disallowed if the CPU is either idle or
 * offline from an RCU perspective, so check for those as well.
 */
int notrace rcu_read_lock_held(void)
{
	bool ret;

	if (rcu_read_lock_held_common(&ret))
		return ret;
	return lock_is_held(&rcu_lock_map);
}
EXPORT_SYMBOL_GPL(rcu_read_lock_held);

/**
 * rcu_read_lock_bh_held() - might we be in RCU-bh read-side critical section?
 *
 * Check for bottom half being disabled, which covers both the
 * CONFIG_PROVE_RCU and not cases.  Note that if someone uses
 * rcu_read_lock_bh(), but then later enables BH, lockdep (if enabled)
 * will show the situation.  This is useful for debug checks in functions
 * that require that they be called within an RCU read-side critical
 * section.
 *
 * Check debug_lockdep_rcu_enabled() to prevent false positives during boot.
 *
 * Note that rcu_read_lock_bh() is disallowed if the CPU is either idle or
 * offline from an RCU perspective, so check for those as well.
 */
int notrace rcu_read_lock_bh_held(void)
{
	bool ret;

	if (rcu_read_lock_held_common(&ret))
		return ret;
	return in_softirq() || irqs_disabled();
}
EXPORT_SYMBOL_GPL(rcu_read_lock_bh_held);

int notrace rcu_read_lock_any_held(void)
{
	bool ret;

	if (rcu_read_lock_held_common(&ret))
		return ret;
	if (lock_is_held(&rcu_lock_map) ||
	    lock_is_held(&rcu_bh_lock_map) ||
	    lock_is_held(&rcu_sched_lock_map))
		return 1;
	return !preemptible();
}
EXPORT_SYMBOL_GPL(rcu_read_lock_any_held);

#endif /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/**
 * wakeme_after_rcu() - Callback function to awaken a task after grace period
 * @head: Pointer to rcu_head member within rcu_synchronize structure
 *
 * Awaken the corresponding task now that a grace period has elapsed.
 */
void wakeme_after_rcu(struct rcu_head *head)
{
	struct rcu_synchronize *rcu;

	rcu = container_of(head, struct rcu_synchronize, head);
	complete(&rcu->completion);
}
EXPORT_SYMBOL_GPL(wakeme_after_rcu);

void __wait_rcu_gp(bool checktiny, unsigned int state, int n, call_rcu_func_t *crcu_array,
		   struct rcu_synchronize *rs_array)
{
	int i;
	int j;

	/* Initialize and register callbacks for each crcu_array element. */
	for (i = 0; i < n; i++) {
		if (checktiny &&
		    (crcu_array[i] == call_rcu)) {
			might_sleep();
			continue;
		}
		for (j = 0; j < i; j++)
			if (crcu_array[j] == crcu_array[i])
				break;
		if (j == i) {
			init_rcu_head_on_stack(&rs_array[i].head);
			init_completion(&rs_array[i].completion);
			(crcu_array[i])(&rs_array[i].head, wakeme_after_rcu);
		}
	}

	/* Wait for all callbacks to be invoked. */
	for (i = 0; i < n; i++) {
		if (checktiny &&
		    (crcu_array[i] == call_rcu))
			continue;
		for (j = 0; j < i; j++)
			if (crcu_array[j] == crcu_array[i])
				break;
		if (j == i) {
			wait_for_completion_state(&rs_array[i].completion, state);
			destroy_rcu_head_on_stack(&rs_array[i].head);
		}
	}
}
EXPORT_SYMBOL_GPL(__wait_rcu_gp);

void finish_rcuwait(struct rcuwait *w)
{
	rcu_assign_pointer(w->task, NULL);
	__set_current_state(TASK_RUNNING);
}
EXPORT_SYMBOL_GPL(finish_rcuwait);

#ifdef CONFIG_DEBUG_OBJECTS_RCU_HEAD
void init_rcu_head(struct rcu_head *head)
{
	debug_object_init(head, &rcuhead_debug_descr);
}
EXPORT_SYMBOL_GPL(init_rcu_head);

void destroy_rcu_head(struct rcu_head *head)
{
	debug_object_free(head, &rcuhead_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_rcu_head);

static bool rcuhead_is_static_object(void *addr)
{
	return true;
}

/**
 * init_rcu_head_on_stack() - initialize on-stack rcu_head for debugobjects
 * @head: pointer to rcu_head structure to be initialized
 *
 * This function informs debugobjects of a new rcu_head structure that
 * has been allocated as an auto variable on the stack.  This function
 * is not required for rcu_head structures that are statically defined or
 * that are dynamically allocated on the heap.  This function has no
 * effect for !CONFIG_DEBUG_OBJECTS_RCU_HEAD kernel builds.
 */
void init_rcu_head_on_stack(struct rcu_head *head)
{
	debug_object_init_on_stack(head, &rcuhead_debug_descr);
}
EXPORT_SYMBOL_GPL(init_rcu_head_on_stack);

/**
 * destroy_rcu_head_on_stack() - destroy on-stack rcu_head for debugobjects
 * @head: pointer to rcu_head structure to be initialized
 *
 * This function informs debugobjects that an on-stack rcu_head structure
 * is about to go out of scope.  As with init_rcu_head_on_stack(), this
 * function is not required for rcu_head structures that are statically
 * defined or that are dynamically allocated on the heap.  Also as with
 * init_rcu_head_on_stack(), this function has no effect for
 * !CONFIG_DEBUG_OBJECTS_RCU_HEAD kernel builds.
 */
void destroy_rcu_head_on_stack(struct rcu_head *head)
{
	debug_object_free(head, &rcuhead_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_rcu_head_on_stack);

const struct debug_obj_descr rcuhead_debug_descr = {
	.name = "rcu_head",
	.is_static_object = rcuhead_is_static_object,
};
EXPORT_SYMBOL_GPL(rcuhead_debug_descr);
#endif /* #ifdef CONFIG_DEBUG_OBJECTS_RCU_HEAD */

#if defined(CONFIG_TREE_RCU) || defined(CONFIG_RCU_TRACE)
void do_trace_rcu_torture_read(const char *rcutorturename, struct rcu_head *rhp,
			       unsigned long secs,
			       unsigned long c_old, unsigned long c)
{
	trace_rcu_torture_read(rcutorturename, rhp, secs, c_old, c);
}
EXPORT_SYMBOL_GPL(do_trace_rcu_torture_read);
#else
#define do_trace_rcu_torture_read(rcutorturename, rhp, secs, c_old, c) \
	do { } while (0)
#endif

#if IS_ENABLED(CONFIG_RCU_TORTURE_TEST) || IS_MODULE(CONFIG_RCU_TORTURE_TEST) || IS_ENABLED(CONFIG_LOCK_TORTURE_TEST) || IS_MODULE(CONFIG_LOCK_TORTURE_TEST)
/* Get rcutorture access to sched_setaffinity(). */
long torture_sched_setaffinity(pid_t pid, const struct cpumask *in_mask, bool dowarn)
{
	int ret;

	ret = sched_setaffinity(pid, in_mask);
	WARN_ONCE(dowarn && ret, "%s: sched_setaffinity(%d) returned %d\n", __func__, pid, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(torture_sched_setaffinity);
#endif

#if IS_ENABLED(CONFIG_TRIVIAL_PREEMPT_RCU)
// Trivial and stupid grace-period wait.  Defined here so that lockdep
// kernels can find tasklist_lock.
void synchronize_rcu_trivial_preempt(void)
{
	struct task_struct *g;
	struct task_struct *t;

	smp_mb(); // Order prior accesses before grace-period start.
	rcu_read_lock(); // Protect task list.
	for_each_process_thread(g, t) {
		if (t == current)
			continue;  // Don't deadlock on ourselves!
		// Order later rcu_read_lock() on other tasks after QS.
		while (smp_load_acquire(&t->rcu_trivial_preempt_nesting))
			continue;
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(synchronize_rcu_trivial_preempt);
#endif // #if IS_ENABLED(CONFIG_TRIVIAL_PREEMPT_RCU)

int rcu_cpu_stall_notifiers __read_mostly; // !0 = provide stall notifiers (rarely useful)
EXPORT_SYMBOL_GPL(rcu_cpu_stall_notifiers);

#ifdef CONFIG_RCU_STALL_COMMON
int rcu_cpu_stall_ftrace_dump __read_mostly;
module_param(rcu_cpu_stall_ftrace_dump, int, 0644);
#ifdef CONFIG_RCU_CPU_STALL_NOTIFIER
module_param(rcu_cpu_stall_notifiers, int, 0444);
#endif // #ifdef CONFIG_RCU_CPU_STALL_NOTIFIER
int rcu_cpu_stall_suppress __read_mostly; // !0 = suppress stall warnings.
EXPORT_SYMBOL_GPL(rcu_cpu_stall_suppress);
module_param(rcu_cpu_stall_suppress, int, 0644);
int rcu_cpu_stall_timeout __read_mostly = CONFIG_RCU_CPU_STALL_TIMEOUT;
module_param(rcu_cpu_stall_timeout, int, 0644);
int rcu_exp_cpu_stall_timeout __read_mostly = CONFIG_RCU_EXP_CPU_STALL_TIMEOUT;
module_param(rcu_exp_cpu_stall_timeout, int, 0644);
int rcu_cpu_stall_cputime __read_mostly = IS_ENABLED(CONFIG_RCU_CPU_STALL_CPUTIME);
module_param(rcu_cpu_stall_cputime, int, 0644);
bool rcu_exp_stall_task_details __read_mostly;
module_param(rcu_exp_stall_task_details, bool, 0644);
#endif /* #ifdef CONFIG_RCU_STALL_COMMON */

// Suppress boot-time RCU CPU stall warnings and rcutorture writer stall
// warnings.  Also used by rcutorture even if stall warnings are excluded.
int rcu_cpu_stall_suppress_at_boot __read_mostly; // !0 = suppress boot stalls.
EXPORT_SYMBOL_GPL(rcu_cpu_stall_suppress_at_boot);
module_param(rcu_cpu_stall_suppress_at_boot, int, 0444);

/**
 * get_completed_synchronize_rcu - Return a pre-completed polled state cookie
 *
 * Returns a value that will always be treated by functions like
 * poll_state_synchronize_rcu() as a cookie whose grace period has already
 * completed.
 */
/*
 * get_completed_synchronize_rcu() - 返回一个预先满足的轮询式宽限期 cookie。
 *
 * 入参：无。返回：RCU_GET_STATE_COMPLETED 哨兵；poll_state_synchronize_rcu()
 * 等配套接口会无条件把它解释为“对应宽限期已经完成”。本函数不读取当前
 * gp_seq、不启动或等待宽限期、不会睡眠，也不改变任何对象 ownership。
 *
 * 它适合把可选等待状态初始化为“无需等待”，让调用者复用统一 poll 路径；
 * 返回值不是当前宽限期快照，不能用来推断系统实际推进到哪个序号。
 */
unsigned long get_completed_synchronize_rcu(void)
{
	return RCU_GET_STATE_COMPLETED;
}
EXPORT_SYMBOL_GPL(get_completed_synchronize_rcu);

#ifdef CONFIG_PROVE_RCU

/*
 * Early boot self test parameters.
 */
static bool rcu_self_test;
module_param(rcu_self_test, bool, 0444);

static int rcu_self_test_counter;

static void test_callback(struct rcu_head *r)
{
	rcu_self_test_counter++;
	pr_info("RCU test callback executed %d\n", rcu_self_test_counter);
}

DEFINE_STATIC_SRCU(early_srcu);
static unsigned long early_srcu_cookie;

struct early_boot_kfree_rcu {
	struct rcu_head rh;
};

static void early_boot_test_call_rcu(void)
{
	static struct rcu_head head;
	int idx;
	static struct rcu_head shead;
	struct early_boot_kfree_rcu *rhp;

	idx = srcu_down_read(&early_srcu);
	srcu_up_read(&early_srcu, idx);
	call_rcu(&head, test_callback);
	early_srcu_cookie = start_poll_synchronize_srcu(&early_srcu);
	call_srcu(&early_srcu, &shead, test_callback);
	rhp = kmalloc_obj(*rhp);
	if (!WARN_ON_ONCE(!rhp))
		kfree_rcu(rhp, rh);
}

void rcu_early_boot_tests(void)
{
	pr_info("Running RCU self tests\n");

	if (rcu_self_test)
		early_boot_test_call_rcu();
	rcu_test_sync_prims();
}

static int rcu_verify_early_boot_tests(void)
{
	int ret = 0;
	int early_boot_test_counter = 0;

	if (rcu_self_test) {
		early_boot_test_counter++;
		rcu_barrier();
		early_boot_test_counter++;
		srcu_barrier(&early_srcu);
		WARN_ON_ONCE(!poll_state_synchronize_srcu(&early_srcu, early_srcu_cookie));
		cleanup_srcu_struct(&early_srcu);
	}
	if (rcu_self_test_counter != early_boot_test_counter) {
		WARN_ON(1);
		ret = -1;
	}

	return ret;
}
late_initcall(rcu_verify_early_boot_tests);
#else
void rcu_early_boot_tests(void) {}
#endif /* CONFIG_PROVE_RCU */

#include "tasks.h"

#ifndef CONFIG_TINY_RCU

/*
 * Print any significant non-default boot-time settings.
 */
/*
 * rcupdate_announce_bootup_oddness() - 打印影响 RCU 行为的非默认启动策略。
 * 入参：无；返回无直接值。仅在 __init 阶段读取策略快照并输出诊断，不改变
 * rcu_expedited/rcu_normal。normal 与 normal_after_boot 优先于 expedited 的打印
 * 顺序对应实际策略优先级，最后继续让 RCU Tasks 报告其特殊配置。
 */
void __init rcupdate_announce_bootup_oddness(void)
{
	if (rcu_normal)
		pr_info("\tNo expedited grace period (rcu_normal).\n");
	else if (rcu_normal_after_boot)
		pr_info("\tNo expedited grace period (rcu_normal_after_boot).\n");
	else if (rcu_expedited)
		pr_info("\tAll grace periods are expedited (rcu_expedited).\n");
	if (rcu_cpu_stall_suppress)
		pr_info("\tRCU CPU stall warnings suppressed (rcu_cpu_stall_suppress).\n");
	if (rcu_cpu_stall_timeout != CONFIG_RCU_CPU_STALL_TIMEOUT)
		pr_info("\tRCU CPU stall warnings timeout set to %d (rcu_cpu_stall_timeout).\n", rcu_cpu_stall_timeout);
	rcu_tasks_bootup_oddness();
}

#endif /* #ifndef CONFIG_TINY_RCU */
