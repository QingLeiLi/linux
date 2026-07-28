/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * RCU 公共接口学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 文件职责：
 *   本头文件把 RCU 的公共契约组织成四组接口：读者用
 *   rcu_read_lock()/rcu_dereference() 建立受保护的读取窗口，更新者用
 *   rcu_assign_pointer() 发布完整对象，用 synchronize_rcu()/call_rcu()
 *   等待旧读者离开，最后通过回调或 kfree_rcu() 回收旧版本。它还为
 *   Tasks RCU、nocb CPU、lockdep、Sparse 和不同 RCU 实现提供配置分派。
 *   真正的宽限期检测、回调推进和 CPU 层级状态机位于 rcutree/rcutiny
 *   及其实现文件中，不由本头文件完成。
 *
 * 主协议：
 *   更新者先构造新对象
 *     -> rcu_assign_pointer() 以 release 语义发布
 *     -> 读者在 RCU 临界区内用 rcu_dereference() 取得借用指针
 *     -> 更新者摘除旧对象
 *     -> synchronize_rcu() 同步等待，或 call_rcu() 异步登记回调
 *     -> 已在摘除前进入的读者全部离开后，旧对象才可回收。
 *
 * 核心不变量：
 *   1. RCU 读锁只保证旧对象在读侧窗口内不因本次摘除而回收，不冻结字段；
 *   2. 发布端 release 与解引用端的依赖/获取语义保证读者看见发布前初始化；
 *   3. 更新者之间仍须用锁、原子操作或其他机制串行，RCU 不提供写者锁；
 *   4. 离开读侧窗口后若仍要使用对象，必须把借用关系交给引用计数或锁；
 *   5. 一个宽限期只等待该宽限期开始前已经存在的读者，不等待后来读者。
 *
 * 并发与代价：
 *   读侧通常极轻量，适合读多写少的数据结构；代价是更新者必须保留旧版本
 *   至宽限期结束，并承担额外内存、回调积压和更复杂的所有权证明。不同
 *   PREEMPT_RCU/TREE_RCU/TINY_RCU 配置改变实现细节，但上述发布、借用、
 *   摘除、等待和回收边界保持不变。
 */
/*
 * Read-Copy Update mechanism for mutual exclusion
 *
 * Copyright IBM Corporation, 2001
 *
 * Author: Dipankar Sarma <dipankar@in.ibm.com>
 *
 * Based on the original work by Paul McKenney <paulmck@vnet.ibm.com>
 * and inputs from Rusty Russell, Andrea Arcangeli and Andi Kleen.
 * Papers:
 * http://www.rdrop.com/users/paulmck/paper/rclockpdcsproof.pdf
 * http://lse.sourceforge.net/locking/rclock_OLS.2001.05.01c.sc.pdf (OLS2001)
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *		http://lse.sourceforge.net/locking/rcupdate.html
 *
 */
/*
 * 本文件提供用于并发互斥/生命周期管理的 Read-Copy Update 机制公共接口。
 * 上述版权、作者、论文和说明链接保持上游原文；其核心思想是读者借用当前
 * 版本，写者复制并发布新版本，再等旧读者离开后回收旧版本。
 */

#ifndef __LINUX_RCUPDATE_H
#define __LINUX_RCUPDATE_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/atomic.h>
#include <linux/irqflags.h>
#include <linux/sched.h>
#include <linux/bottom_half.h>
#include <linux/lockdep.h>
#include <linux/cleanup.h>
#include <asm/processor.h>
#include <linux/context_tracking_irq.h>

/*
 * 三个 token context lock 声明是编译器/静态分析模型：RCU 表示普通共享读侧
 * 上下文，RCU_SCHED 与 RCU_BH 是其 sched、softirq 子类别。它们不分配真实
 * 锁对象；后续 __acquire_shared/__release_shared 用这些 token 验证配对。
 */
token_context_lock(RCU, __reentrant_ctx_lock);
token_context_lock_instance(RCU, RCU_SCHED);
token_context_lock_instance(RCU, RCU_BH);

/*
 * A convenience macro that can be used for RCU-protected globals or struct
 * members; adds type qualifier __rcu, and also enforces __guarded_by(RCU).
 */
/*
 * 这是供 RCU 保护的全局变量或结构体成员使用的便捷标注：既增加 Sparse
 * 可检查的 __rcu 地址空间限定，又告诉上下文/锁分析器该对象受 RCU
 * token 保护。它不生成运行时代码，也不能替代真正的发布和读侧协议。
 */
#define __rcu_guarded __rcu __guarded_by(RCU)

/*
 * RCU 序号按无符号整数循环递增。这里用“半个数值空间”判断 a 是否不早于 b：
 * 只要两次比较的距离不跨越半圈，减法溢出仍能正确表达先后关系。调用者必须
 * 保证不会积累超过半个 ULONG 空间的未比较序号，否则环形顺序会变得含糊。
 */
#define ULONG_CMP_GE(a, b)	(ULONG_MAX / 2 >= (a) - (b))
#define ULONG_CMP_LT(a, b)	(ULONG_MAX / 2 < (a) - (b))

/*
 * rcu_seq 的低两位保存宽限期阶段状态，高位保存递增计数。SHIFT 是状态位数，
 * MASK 用来只取状态；二者共同约束 rcutree/rcutiny 中的序号编码。
 */
#define RCU_SEQ_CTR_SHIFT    2
#define RCU_SEQ_STATE_MASK   ((1 << RCU_SEQ_CTR_SHIFT) - 1)

/* Exported common interfaces */
/*
 * 导出的通用宽限期接口：
 *   call_rcu(@head, @func)：异步登记嵌入对象中的回调节点；调用后节点归 RCU
 *   回调系统管理，宽限期结束后在回调上下文调用 @func，登记本身不等待。
 *   rcu_barrier_tasks()：等待先前登记的 Tasks RCU 回调全部执行，常用于卸载。
 *   synchronize_rcu()：同步等待一个普通 RCU 宽限期；可能睡眠，不能在读侧
 *   临界区或原子上下文调用。它只给出等待边界，不自动释放任何对象。
 */
void call_rcu(struct rcu_head *head, rcu_callback_t func);
void rcu_barrier_tasks(void);
void synchronize_rcu(void);

/*
 * struct rcu_gp_oldstate 是轮询式宽限期 API 的不透明完整状态，由实现定义；
 * 调用者只能把它交还给配套接口，不能依赖字段布局。单个 unsigned long
 * 是压缩状态，full 版本通过输出参数写入更完整的快照，所有权仍在调用者。
 */
struct rcu_gp_oldstate;
/*
 * get_completed_synchronize_rcu() 无参数，返回一个保证会被 poll 接口视为
 * “宽限期已完成”的 unsigned long 哨兵；它不启动也不等待宽限期。
 * full(@rgosp) 把 normal/expedited 两种已完成哨兵写入调用者拥有的非空输出
 * 对象，无直接返回值、无所有权变化。两者用于把轮询状态初始化为已满足。
 */
unsigned long get_completed_synchronize_rcu(void);
void get_completed_synchronize_rcu_full(struct rcu_gp_oldstate *rgosp);

// Maximum number of unsigned long values corresponding to
// not-yet-completed RCU grace periods.
/*
 * 最多需要两个 unsigned long 旧状态值来代表尚未完成的 RCU 宽限期。
 * 该上界让把旧状态附着在待回收对象上的调用者能够固定数组大小，而不暴露
 * 底层宽限期状态机。
 */
#define NUM_ACTIVE_RCU_POLL_OLDSTATE 2

/**
 * same_state_synchronize_rcu - Are two old-state values identical?
 * @oldstate1: First old-state value.
 * @oldstate2: Second old-state value.
 *
 * The two old-state values must have been obtained from either
 * get_state_synchronize_rcu(), start_poll_synchronize_rcu(), or
 * get_completed_synchronize_rcu().  Returns @true if the two values are
 * identical and @false otherwise.  This allows structures whose lifetimes
 * are tracked by old-state values to push these values to a list header,
 * allowing those structures to be slightly smaller.
 */
/*
 * same_state_synchronize_rcu() - 比较两个轮询式宽限期旧状态是否完全相同。
 *
 * @oldstate1：由 get/start/completed 系列接口取得的第一个值，只读传入。
 * @oldstate2：同来源的第二个值，只读传入。
 * 返回：相等为 true，否则为 false；无副作用、无所有权变化、不会睡眠。
 *
 * 两值相同意味着多个对象可以把同一状态提升到链表头统一保存，从而省去
 * 每个对象的重复字段；本函数不判断宽限期是否已经完成，调用者仍须使用
 * 对应的 poll 接口解释该状态。
 */
static inline bool same_state_synchronize_rcu(unsigned long oldstate1, unsigned long oldstate2)
{
	return oldstate1 == oldstate2;
}

#ifdef CONFIG_PREEMPT_RCU

/*
 * 可抢占 RCU 需要在 task_struct 中记录嵌套深度和被抢占读者状态，因而入口/
 * 出口由实现函数处理。两者无参数、无直接返回值；必须成对嵌套使用，出口
 * 可能在最外层解锁时报告静止状态并推进延迟工作，但不会把对象所有权交出。
 */
void __rcu_read_lock(void);
void __rcu_read_unlock(void);

/*
 * Defined as a macro as it is a very low level header included from
 * areas that don't even know about current.  This gives the rcu_read_lock()
 * nesting depth, but makes sense only if CONFIG_PREEMPT_RCU -- in other
 * types of kernel builds, the rcu_read_lock() nesting depth is unknowable.
 */
/*
 * 宏形式避免低层头文件为 current 再引入依赖。仅在 PREEMPT_RCU 下，
 * current->rcu_read_lock_nesting 才是可观察的精确嵌套深度；READ_ONCE()
 * 防止编译器合并或重复取值，但不提供跨 CPU 的字段稳定性。
 */
#define rcu_preempt_depth() READ_ONCE(current->rcu_read_lock_nesting)

#else /* #ifdef CONFIG_PREEMPT_RCU */

/*
 * 非可抢占 RCU 以禁止抢占覆盖读侧窗口。严格宽限期调试配置在解锁前额外
 * 制造/确认静止状态；TINY_RCU 无需该钩子，因此保留零成本同签名 stub。
 */
#ifdef CONFIG_TINY_RCU
#define rcu_read_unlock_strict() do { } while (0)
#else
void rcu_read_unlock_strict(void);
#endif

/*
 * __rcu_read_lock() - 非 PREEMPT_RCU 的底层读锁。
 *
 * 入参：无。返回：无直接返回值。副作用：增加当前 CPU 的抢占禁用嵌套；
 * 调用期间不可迁移或被任务抢占，因此调度器可把这段区间计入普通 RCU
 * 读者。它不禁止中断，也不串行更新者。
 */
static inline void __rcu_read_lock(void)
{
	preempt_disable();
}

/*
 * __rcu_read_unlock() - 结束非 PREEMPT_RCU 底层读侧窗口。
 *
 * 入参：无。返回：无直接返回值。严格模式先执行额外静止状态处理，再降低
 * 抢占禁用计数；顺序不能颠倒，否则严格检查可能在已经允许迁移/调度后观察
 * 错误的当前 CPU 状态。最外层 preempt_enable() 还可能触发一次调度。
 */
static inline void __rcu_read_unlock(void)
{
	if (IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD))
		rcu_read_unlock_strict();
	preempt_enable();
}

/*
 * 非 PREEMPT_RCU 没有 task 级嵌套计数可查询，返回 0 是“接口不可提供该
 * 信息”的固定结果，不应被调用者解释为当前一定不在 RCU 读侧区间。
 */
static inline int rcu_preempt_depth(void)
{
	return 0;
}

#endif /* #else #ifdef CONFIG_PREEMPT_RCU */

#ifdef CONFIG_RCU_LAZY
/*
 * call_rcu_hurry() 登记 @head/@func 并要求跳过 lazy 批处理延迟，适用于
 * 对回调时延敏感的更新路径；节点所有权和回调契约与 call_rcu() 相同。
 */
void call_rcu_hurry(struct rcu_head *head, rcu_callback_t func);
#else
/*
 * 未启用 lazy RCU 时没有额外延迟可跳过，因此 hurry 退化为 call_rcu()。
 * @head 是调用对象内嵌的回调节点，@func 是宽限期后的回调；无直接返回值，
 * 登记后调用者不得在回调执行前重复使用同一节点。
 */
static inline void call_rcu_hurry(struct rcu_head *head, rcu_callback_t func)
{
	call_rcu(head, func);
}
#endif

/* Internal to kernel */
/*
 * 内核内部生命周期钩子：
 *   rcu_init() 在启动期初始化全局/per-CPU RCU 状态；
 *   rcu_scheduler_active 表示调度器已进入 RCU 可依赖的阶段；
 *   rcu_sched_clock_irq(@user) 由时钟中断路径报告 tick 上下文，@user 指示
 *   中断是否来自用户态。它们修改 RCU 内部状态，不是数据结构读者 API。
 */
void rcu_init(void);
extern int rcu_scheduler_active;
void rcu_sched_clock_irq(int user);

#ifdef CONFIG_RCU_STALL_COMMON
/*
 * SysRq 处理开始/结束钩子暂时调整 stall 检测的时间基准，避免人工停顿被
 * 误报为 RCU stall；无参数、无返回值，必须按同一处理流程成对调用。
 */
void rcu_sysrq_start(void);
void rcu_sysrq_end(void);
#else /* #ifdef CONFIG_RCU_STALL_COMMON */
/* 未编译公共 stall 检测时保留无副作用 stub，使调用点无需条件编译。 */
static inline void rcu_sysrq_start(void) { }
static inline void rcu_sysrq_end(void) { }
#endif /* #else #ifdef CONFIG_RCU_STALL_COMMON */

#if defined(CONFIG_NO_HZ_FULL) && (!defined(CONFIG_GENERIC_ENTRY) || !defined(CONFIG_VIRT_XFER_TO_GUEST_WORK))
/*
 * 特定 NO_HZ_FULL 入口组合需要显式安排 IRQ work 重新调度，以便在长期无
 * tick/虚拟机切换场景继续推进 RCU；其他组合已由通用入口完成，stub 无副作用。
 */
void rcu_irq_work_resched(void);
#else
static __always_inline void rcu_irq_work_resched(void) { }
#endif

#ifdef CONFIG_RCU_NOCB_CPU
/*
 * nocb CPU 把回调处理卸载到专用 kthread：
 *   rcu_init_nohz() 初始化卸载基础设施；
 *   offload/deoffload(@cpu) 动态切换指定逻辑 CPU；目标已处于所需状态返回
 *   0，在线 CPU 不能切换而返回 -EINVAL，底层迁移失败时继续返回其负 errno；
 *   flush_deferred_wakeup() 冲刷当前推迟的 nocb 唤醒请求。
 * 切换会接触回调队列并可能等待，调用者不能把它当作纯标志写入。
 */
void rcu_init_nohz(void);
int rcu_nocb_cpu_offload(int cpu);
int rcu_nocb_cpu_deoffload(int cpu);
void rcu_nocb_flush_deferred_wakeup(void);

/* nocb 存在时保留锁依赖检查，条件 @c 为真便以消息 @s 报告协议违例。 */
#define RCU_NOCB_LOCKDEP_WARN(c, s) RCU_LOCKDEP_WARN(c, s)

#else /* #ifdef CONFIG_RCU_NOCB_CPU */

/*
 * 未启用 nocb 时的兼容契约：初始化/冲刷无事可做；请求 offload 返回
 * -EINVAL 表示功能不存在，deoffload 返回成功是因为系统本来就在非卸载态。
 */
static inline void rcu_init_nohz(void) { }
static inline int rcu_nocb_cpu_offload(int cpu) { return -EINVAL; }
static inline int rcu_nocb_cpu_deoffload(int cpu) { return 0; }
static inline void rcu_nocb_flush_deferred_wakeup(void) { }

/* 没有 nocb 队列就没有相应锁协议可检查；参数不求值，不能依赖其副作用。 */
#define RCU_NOCB_LOCKDEP_WARN(c, s)

#endif /* #else #ifdef CONFIG_RCU_NOCB_CPU */

/*
 * Note a quasi-voluntary context switch for RCU-tasks's benefit.
 * This is a macro rather than an inline function to avoid #include hell.
 */
/*
 * 为 Tasks RCU 记录“近似自愿上下文切换”。使用宏是为避免这个基础头文件
 * 继续引入 task_struct 相关头文件。这里报告的是任务级静止状态，不等同于
 * 普通 RCU 的 CPU 静止状态。
 */
#ifdef CONFIG_TASKS_RCU_GENERIC

# ifdef CONFIG_TASKS_RCU
/*
 * @t 是被检查任务的借用指针，@preempt 表示本次切换是否仅由抢占造成。
 * classic Tasks RCU 只接受非抢占切换作为静止状态；READ_ONCE/WRITE_ONCE
 * 与扫描器并发访问 holdout 标志，避免编译器制造丢失观察，但标志本身不是
 * 引用保护。
 *
 * call_rcu_tasks(@head, @func) 异步登记内嵌节点与回调，节点责任转给 Tasks
 * RCU 至回调执行；synchronize_rcu_tasks() 同步等待并可能睡眠；
 * rcu_tasks_torture_stats_print(@tt, @tf) 把 torture 状态写入调用者提供的
 * 可写字符缓冲区，无所有权变化。
 */
# define rcu_tasks_classic_qs(t, preempt)				\
	do {								\
		if (!(preempt) && READ_ONCE((t)->rcu_tasks_holdout))	\
			WRITE_ONCE((t)->rcu_tasks_holdout, false);	\
	} while (0)
void call_rcu_tasks(struct rcu_head *head, rcu_callback_t func);
void synchronize_rcu_tasks(void);
void rcu_tasks_torture_stats_print(char *tt, char *tf);
# else
# define rcu_tasks_classic_qs(t, preempt) do { } while (0)
# define call_rcu_tasks call_rcu
# define synchronize_rcu_tasks synchronize_rcu
# endif

/* generic 调用点统一进入 classic flavor；参数加括号避免宏表达式优先级泄漏。 */
#define rcu_tasks_qs(t, preempt) rcu_tasks_classic_qs((t), (preempt))

# ifdef CONFIG_TASKS_RUDE_RCU
/*
 * Rude Tasks RCU 通过更强制的方式取得任务静止状态；同步接口可能睡眠，
 * stats 接口把诊断文本写入调用者提供的缓冲区 @tt/@tf。
 */
void synchronize_rcu_tasks_rude(void);
void rcu_tasks_rude_torture_stats_print(char *tt, char *tf);
# endif

/*
 * 自愿切换明确传入 false；退出协议的 start/finish 包围 task 退出过程中
 * Tasks RCU 需要特殊观察的窗口，二者无参数、无直接返回值并须配对。
 */
#define rcu_note_voluntary_context_switch(t) rcu_tasks_qs(t, false)
void exit_tasks_rcu_start(void);
void exit_tasks_rcu_finish(void);
#else /* #ifdef CONFIG_TASKS_RCU_GENERIC */
/*
 * 完全没有 Tasks RCU 时，静止状态与退出钩子为空；call/synchronize 别名
 * 到普通 RCU，维持调用点的等待语义，但不应据此声称启用了任务级 flavor。
 */
#define rcu_tasks_classic_qs(t, preempt) do { } while (0)
#define rcu_tasks_qs(t, preempt) do { } while (0)
#define rcu_note_voluntary_context_switch(t) do { } while (0)
#define call_rcu_tasks call_rcu
#define synchronize_rcu_tasks synchronize_rcu
static inline void exit_tasks_rcu_start(void) { }
static inline void exit_tasks_rcu_finish(void) { }
#endif /* #else #ifdef CONFIG_TASKS_RCU_GENERIC */

/**
 * cond_resched_tasks_rcu_qs - Report potential quiescent states to RCU
 *
 * This macro resembles cond_resched(), except that it is defined to
 * report potential quiescent states to RCU-tasks even if the cond_resched()
 * machinery were to be shut off, as some advocate for PREEMPTION kernels.
 */
/*
 * cond_resched_tasks_rcu_qs() - 在长循环中同时提供 Tasks RCU 进展机会。
 *
 * 入参：无。返回：无直接返回值。先把 current 的这次非抢占式检查报告给
 * Tasks RCU，再执行 cond_resched()；即使后者在可抢占内核被优化为空，前者
 * 仍保留任务级静止状态报告。宏可能触发调度，因此只能用于允许调度的上下文。
 */
#define cond_resched_tasks_rcu_qs() \
do { \
	rcu_tasks_qs(current, false); \
	cond_resched(); \
} while (0)

/**
 * rcu_softirq_qs_periodic - Report RCU and RCU-Tasks quiescent states
 * @old_ts: jiffies at start of processing.
 *
 * This helper is for long-running softirq handlers, such as NAPI threads in
 * networking. The caller should initialize the variable passed in as @old_ts
 * at the beginning of the softirq handler. When invoked frequently, this macro
 * will invoke rcu_softirq_qs() every 100 milliseconds thereafter, which will
 * provide both RCU and RCU-Tasks quiescent states. Note that this macro
 * modifies its old_ts argument.
 *
 * Because regions of code that have disabled softirq act as RCU read-side
 * critical sections, this macro should be invoked with softirq (and
 * preemption) enabled.
 *
 * The macro is not needed when CONFIG_PREEMPT_RT is defined. RT kernels would
 * have more chance to invoke schedule() calls and provide necessary quiescent
 * states. As a contrast, calling cond_resched() only won't achieve the same
 * effect because cond_resched() does not provide RCU-Tasks quiescent states.
 */
/*
 * rcu_softirq_qs_periodic(@old_ts) - 为长时间运行的 softirq/NAPI 处理周期性
 * 报告普通 RCU 与 Tasks RCU 静止状态。
 *
 * @old_ts 是以 jiffies 为单位的输入输出时间戳，调用者在循环开始初始化；
 * 超过 100ms 后宏会更新它。调用时 softirq 与抢占必须已启用，因为禁用
 * softirq 的区域本身就是 RCU 读侧区间。临时 preempt_disable() 让
 * rcu_softirq_qs() 在稳定 CPU 上完成报告，再恢复原上下文。PREEMPT_RT
 * 依靠更频繁的 schedule() 推进，因此整个分支跳过。
 */
#define rcu_softirq_qs_periodic(old_ts) \
do { \
	if (!IS_ENABLED(CONFIG_PREEMPT_RT) && \
	    time_after(jiffies, (old_ts) + HZ / 10)) { \
		preempt_disable(); \
		rcu_softirq_qs(); \
		preempt_enable(); \
		(old_ts) = jiffies; \
	} \
} while (0)

/*
 * Infrastructure to implement the synchronize_() primitives in
 * TREE_RCU and rcu_barrier_() primitives in TINY_RCU.
 */
/*
 * 下列实现头提供同步等待、轮询和 barrier 等具体原语：SMP/大型系统选择
 * TREE_RCU，极小单处理器系统选择 TINY_RCU。配置必须且只能命中一个分支，
 * 否则编译失败，避免生成没有宽限期后端的内核。
 */

#if defined(CONFIG_TREE_RCU)
#include <linux/rcutree.h>
#elif defined(CONFIG_TINY_RCU)
#include <linux/rcutiny.h>
#else
#error "Unknown RCU implementation specified to kernel configuration"
#endif

/*
 * The init_rcu_head_on_stack() and destroy_rcu_head_on_stack() calls
 * are needed for dynamic initialization and destruction of rcu_head
 * on the stack, and init_rcu_head()/destroy_rcu_head() are needed for
 * dynamic initialization and destruction of statically allocated rcu_head
 * structures.  However, rcu_head structures allocated dynamically in the
 * heap don't need any initialization.
 */
/*
 * debugobjects 需要跟踪 rcu_head 的动态启用/销毁：栈上节点必须使用
 * on_stack 成对接口，静态节点使用普通接口；随所属堆对象一起分配的节点
 * 由分配生命周期自然覆盖，无需单独初始化。@head 均为借用指针，接口只改
 * 调试元数据，不转移回调节点所有权。关闭调试时 stub 不产生任何代码。
 */
#ifdef CONFIG_DEBUG_OBJECTS_RCU_HEAD
void init_rcu_head(struct rcu_head *head);
void destroy_rcu_head(struct rcu_head *head);
void init_rcu_head_on_stack(struct rcu_head *head);
void destroy_rcu_head_on_stack(struct rcu_head *head);
#else /* !CONFIG_DEBUG_OBJECTS_RCU_HEAD */
static inline void init_rcu_head(struct rcu_head *head) { }
static inline void destroy_rcu_head(struct rcu_head *head) { }
static inline void init_rcu_head_on_stack(struct rcu_head *head) { }
static inline void destroy_rcu_head_on_stack(struct rcu_head *head) { }
#endif	/* #else !CONFIG_DEBUG_OBJECTS_RCU_HEAD */

#if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_PROVE_RCU)
/*
 * 同时启用热插拔和 RCU 证明时，检查当前 CPU 是否在线以抑制离线过渡期的
 * 假阳性；其他配置恒真，因为不存在需要建模的这类竞态。
 */
bool rcu_lockdep_current_cpu_online(void);
#else /* #if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_PROVE_RCU) */
static inline bool rcu_lockdep_current_cpu_online(void) { return true; }
#endif /* #else #if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_PROVE_RCU) */

extern struct lockdep_map rcu_lock_map;
extern struct lockdep_map rcu_bh_lock_map;
extern struct lockdep_map rcu_sched_lock_map;
extern struct lockdep_map rcu_callback_map;
/*
 * 四张 lockdep map 是逻辑锁类别而非真实自旋锁，分别建模普通、BH、sched
 * 读侧窗口和回调执行上下文。它们让 lockdep 能验证嵌套/睡眠协议，但不参与
 * 生产配置下的互斥或对象生命周期。
 */

#ifdef CONFIG_DEBUG_LOCK_ALLOC

/*
 * 三个包装器把逻辑 RCU 锁的 acquire、try-acquire、release 事件送入
 * lockdep。@map 是上述逻辑类别的借用指针；无直接返回值、不会取得对象引用。
 * 参数中的 read=2 表示递归共享读锁模型，_THIS_IP_ 保留诊断调用点。
 */
static inline void rcu_lock_acquire(struct lockdep_map *map)
{
	lock_acquire(map, 0, 0, 2, 0, NULL, _THIS_IP_);
}

static inline void rcu_try_lock_acquire(struct lockdep_map *map)
{
	lock_acquire(map, 0, 1, 2, 0, NULL, _THIS_IP_);
}

static inline void rcu_lock_release(struct lockdep_map *map)
{
	lock_release(map, _THIS_IP_);
}

/*
 * debug_lockdep_rcu_enabled() 返回 1/0 表示 RCU lockdep 检查当前是否可用；
 * 四个 held 查询分别判断普通、BH、sched 或任一 vanilla RCU 读者上下文。
 * 它们只查询诊断状态，不取得真实保护；调用者不能用返回值延长对象生命周期。
 */
int debug_lockdep_rcu_enabled(void);
int rcu_read_lock_held(void);
int rcu_read_lock_bh_held(void);
int rcu_read_lock_sched_held(void);
int rcu_read_lock_any_held(void);

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/* 关闭 lockdep 时 acquire/release 不求值地退化为空，不能承载功能副作用。 */
# define rcu_lock_acquire(a)		do { } while (0)
# define rcu_try_lock_acquire(a)	do { } while (0)
# define rcu_lock_release(a)		do { } while (0)

/*
 * 关闭 lockdep 后无法精确证明普通/BH 逻辑读锁，held 查询采取保守结果以避免
 * 假阳性；这些值只是让诊断宏退化，不能作为业务路径中的同步条件。
 */
static inline int rcu_read_lock_held(void)
{
	return 1;
}

static inline int rcu_read_lock_bh_held(void)
{
	return 1;
}

static inline int rcu_read_lock_sched_held(void)
{
	return !preemptible();
}

static inline int rcu_read_lock_any_held(void)
{
	return !preemptible();
}

static inline int debug_lockdep_rcu_enabled(void)
{
	return 0;
}
/*
 * 普通/BH 查询固定为 1；sched/any 仍用 preemptible() 判断广义不可抢占
 * 读者；debug 查询固定为 0，关闭运行时 RCU 锁依赖诊断。
 */

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

#ifdef CONFIG_PROVE_RCU

/**
 * RCU_LOCKDEP_WARN - emit lockdep splat if specified condition is met
 * @c: condition to check
 * @s: informative message
 *
 * This checks debug_lockdep_rcu_enabled() before checking (c) to
 * prevent early boot splats due to lockdep not yet being initialized,
 * and rechecks it after checking (c) to prevent false-positive splats
 * due to races with lockdep being disabled.  See commit 3066820034b5dd
 * ("rcu: Reject RCU_LOCKDEP_WARN() false positives") for more detail.
 */
/*
 * RCU_LOCKDEP_WARN(@c, @s) - 在条件成立时至多报告一次 RCU 协议违例。
 *
 * @c 是仅供诊断的布尔表达式，@s 是静态说明文本。前后两次检查
 * debug_lockdep_rcu_enabled()：第一次避免启动早期尚未初始化的误报，第二次
 * 关闭与条件求值并发时过滤假阳性。每个宏展开点各有一个低频段 __warned，
 * 因而只压制同一调用点的重复报告，不压制别处。宏无业务返回值，也不修复
 * 状态；生产逻辑不得依赖是否触发告警。
 */
#define RCU_LOCKDEP_WARN(c, s)						\
	do {								\
		static bool __section(".data..unlikely") __warned;	\
		if (debug_lockdep_rcu_enabled() && (c) &&		\
		    debug_lockdep_rcu_enabled() && !__warned) {		\
			__warned = true;				\
			lockdep_rcu_suspicious(__FILE__, __LINE__, s);	\
		}							\
	} while (0)

#ifndef CONFIG_PREEMPT_RCU
/*
 * 非可抢占 RCU 中显式调度会破坏“禁止抢占即读侧保护”的基本契约，因此检查
 * 普通 RCU 逻辑锁是否仍被持有。无参数、无直接返回值，只可能输出 lockdep
 * 告警；PREEMPT_RCU 的任务级读者可被抢占，使用空实现避免错误套用该规则。
 */
static inline void rcu_preempt_sleep_check(void)
{
	RCU_LOCKDEP_WARN(lock_is_held(&rcu_lock_map),
			 "Illegal context switch in RCU read-side critical section");
}
#else // #ifndef CONFIG_PREEMPT_RCU
static inline void rcu_preempt_sleep_check(void) { }
#endif // #else // #ifndef CONFIG_PREEMPT_RCU

/*
 * rcu_sleep_check() 汇总三种禁止睡眠的读侧类别。普通 RCU 的配置差异由上述
 * helper 处理；非 RT 内核还禁止 BH 读侧睡眠；sched 读侧始终要求不可调度。
 * PREEMPT_RT 的 BH/自旋锁语义不同，所以跳过 BH 告警。该宏只诊断，不建立
 * 任何保护。
 */
#define rcu_sleep_check()						\
	do {								\
		rcu_preempt_sleep_check();				\
		if (!IS_ENABLED(CONFIG_PREEMPT_RT))			\
		    RCU_LOCKDEP_WARN(lock_is_held(&rcu_bh_lock_map),	\
				 "Illegal context switch in RCU-bh read-side critical section"); \
		RCU_LOCKDEP_WARN(lock_is_held(&rcu_sched_lock_map),	\
				 "Illegal context switch in RCU-sched read-side critical section"); \
	} while (0)

// See RCU_LOCKDEP_WARN() for an explanation of the double call to
// debug_lockdep_rcu_enabled().
/*
 * 与 RCU_LOCKDEP_WARN() 一样，两次读取调试开关用于抵抗 lockdep 动态关闭
 * 造成的竞态。helper 同时接受布尔失败条件 @c 与静态上下文锁 token @ctx；
 * 仅当 lockdep 有效、且当前 CPU 正被 RCU 观察并在线时，@c 才构成真正违例。
 * 返回 true 表示上层 WARN_ON_ONCE 应告警，无副作用且不取得任何锁。
 */
static __always_inline bool lockdep_assert_rcu_helper(bool c, const struct __ctx_lock_RCU *ctx)
	__assumes_shared_ctx_lock(RCU) __assumes_shared_ctx_lock(ctx)
{
	return debug_lockdep_rcu_enabled() &&
	       (c || !rcu_is_watching() || !rcu_lockdep_current_cpu_online()) &&
	       debug_lockdep_rcu_enabled();
}

/**
 * lockdep_assert_in_rcu_read_lock - WARN if not protected by rcu_read_lock()
 *
 * Splats if lockdep is enabled and there is no rcu_read_lock() in effect.
 */
/*
 * lockdep_assert_in_rcu_read_lock() - 断言当前确有显式普通 RCU 读锁。
 *
 * 无参数、无业务返回值；缺失时仅告警。它要求 rcu_lock_map 的逻辑锁事件，
 * 不能用“碰巧禁止抢占”替代，适合验证 API 契约而非测试对象是否仍存活。
 */
#define lockdep_assert_in_rcu_read_lock() \
	WARN_ON_ONCE(lockdep_assert_rcu_helper(!lock_is_held(&rcu_lock_map), RCU))

/**
 * lockdep_assert_in_rcu_read_lock_bh - WARN if not protected by rcu_read_lock_bh()
 *
 * Splats if lockdep is enabled and there is no rcu_read_lock_bh() in effect.
 * Note that local_bh_disable() and friends do not suffice here, instead an
 * actual rcu_read_lock_bh() is required.
 */
/*
 * lockdep_assert_in_rcu_read_lock_bh() - 断言调用者使用了显式 BH 版 RCU
 * 临界区。单独 local_bh_disable() 虽可形成宽限期意义上的读者，却没有
 * rcu_bh_lock_map 事件，因而不满足这个更严格的接口契约。
 */
#define lockdep_assert_in_rcu_read_lock_bh() \
	WARN_ON_ONCE(lockdep_assert_rcu_helper(!lock_is_held(&rcu_bh_lock_map), RCU_BH))

/**
 * lockdep_assert_in_rcu_read_lock_sched - WARN if not protected by rcu_read_lock_sched()
 *
 * Splats if lockdep is enabled and there is no rcu_read_lock_sched()
 * in effect.  Note that preempt_disable() and friends do not suffice here,
 * instead an actual rcu_read_lock_sched() is required.
 */
/*
 * lockdep_assert_in_rcu_read_lock_sched() - 断言显式 sched 版 RCU 读锁存在。
 * 单独 preempt_disable() 在宽限期语义上足够，但这里有意要求调用者走成对
 * API，以便 lockdep 能追踪上下文和解锁配对。
 */
#define lockdep_assert_in_rcu_read_lock_sched() \
	WARN_ON_ONCE(lockdep_assert_rcu_helper(!lock_is_held(&rcu_sched_lock_map), RCU_SCHED))

/**
 * lockdep_assert_in_rcu_reader - WARN if not within some type of RCU reader
 *
 * Splats if lockdep is enabled and there is no RCU reader of any
 * type in effect.  Note that regions of code protected by things like
 * preempt_disable, local_bh_disable(), and local_irq_disable() all qualify
 * as RCU readers.
 *
 * Note that this will never trigger in PREEMPT_NONE or PREEMPT_VOLUNTARY
 * kernels that are not also built with PREEMPT_COUNT.  But if you have
 * lockdep enabled, you might as well also enable PREEMPT_COUNT.
 */
/*
 * lockdep_assert_in_rcu_reader() - 接受任一普通 RCU 读者形式。
 *
 * 普通、BH、sched 逻辑锁或任何不可抢占区间均可满足，因此它比前三个断言
 * 宽松。未启用 PREEMPT_COUNT 的 NONE/VOLUNTARY 内核无法通过 preemptible()
 * 发现缺失保护，所以可能永不告警；这是诊断覆盖限制，不是额外同步保证。
 */
#define lockdep_assert_in_rcu_reader()								\
	WARN_ON_ONCE(lockdep_assert_rcu_helper(!lock_is_held(&rcu_lock_map) &&			\
					       !lock_is_held(&rcu_bh_lock_map) &&		\
					       !lock_is_held(&rcu_sched_lock_map) &&		\
					       preemptible(), RCU))

#else /* #ifdef CONFIG_PROVE_RCU */

/*
 * 未启用 PROVE_RCU 时，告警与睡眠检查生成零运行时代码；lockdep 断言变成
 * 静态分析器的上下文假设，帮助编译期检查后续访问，但不会在真实 CPU 上
 * 获取锁或延长对象寿命。
 */
#define RCU_LOCKDEP_WARN(c, s) do { } while (0 && (c))
#define rcu_sleep_check() do { } while (0)

#define lockdep_assert_in_rcu_read_lock() __assume_shared_ctx_lock(RCU)
#define lockdep_assert_in_rcu_read_lock_bh() __assume_shared_ctx_lock(RCU_BH)
#define lockdep_assert_in_rcu_read_lock_sched() __assume_shared_ctx_lock(RCU_SCHED)
#define lockdep_assert_in_rcu_reader() __assume_shared_ctx_lock(RCU)

#endif /* #else #ifdef CONFIG_PROVE_RCU */

/*
 * Helper functions for rcu_dereference_check(), rcu_dereference_protected()
 * and rcu_assign_pointer().  Some of these could be folded into their
 * callers, but they are left separate in order to ease introduction of
 * multiple pointers markings to match different RCU implementations
 * (e.g., __srcu), should this make sense in the future.
 */
/*
 * 下列内部 helper 统一 RCU 指针的三层契约：READ_ONCE/发布内存序、lockdep
 * 运行时条件检查、Sparse 的 __rcu 地址空间检查。保持它们独立可让未来
 * __srcu 等地址空间复用同一骨架。参数 local 是每次宏展开的唯一临时变量名，
 * space 指定 Sparse 地址空间；返回值均是借用指针，不增加引用。
 */

#ifdef __CHECKER__
/*
 * Sparse 构建时用不会执行的类型比较验证 @p 带有期望的 @space 标注；
 * 普通编译时为空。它只发现标注误用，不提供运行时屏障或生命周期保护。
 */
#define rcu_check_sparse(p, space) \
	((void)(((typeof(*p) space *)p) == p))
#else /* #ifdef __CHECKER__ */
#define rcu_check_sparse(p, space)
#endif /* #else #ifdef __CHECKER__ */

#define __unrcu_pointer(p, local)					\
context_unsafe(								\
	typeof(*p) *local = (typeof(*p) *__force)(p);			\
	rcu_check_sparse(p, __rcu);					\
	((typeof(*p) __force __kernel *)(local))			\
)
/**
 * unrcu_pointer - mark a pointer as not being RCU protected
 * @p: pointer needing to lose its __rcu property
 *
 * Converts @p from an __rcu pointer to a __kernel pointer.
 * This allows an __rcu pointer to be used with xchg() and friends.
 */
/*
 * unrcu_pointer(@p) - 显式去掉 __rcu 类型属性，供 xchg() 等原子更新原语
 * 接受普通内核指针。
 *
 * @p 只求值一次，返回相同地址的借用指针；不做 READ_ONCE、屏障、锁检查或
 * 引用获取。调用者必须已经用更新侧锁/原子协议保证并发与生命周期，不能把
 * 类型转换误当成安全解引用。
 */
#define unrcu_pointer(p) __unrcu_pointer(p, __UNIQUE_ID(rcu))

/*
 * 四个底层读取形态：
 *   access：READ_ONCE 取指针，只允许比较/交给原子更新，不能据此解引用；
 *   dereference_check：READ_ONCE + 条件/lockdep 检查，保留地址依赖顺序；
 *   protected：更新侧条件已阻止指针改变，故不做 READ_ONCE；
 *   raw：仅稳定取值并保留依赖，不验证锁和 Sparse 地址空间。
 * 它们都不取得引用，裸返回值只能在各自保护条件持续成立时使用。
 */
#define __rcu_access_pointer(p, local, space) \
({ \
	typeof(*p) *local = (typeof(*p) *__force)READ_ONCE(p); \
	rcu_check_sparse(p, space); \
	((typeof(*p) __force __kernel *)(local)); \
})
#define __rcu_dereference_check(p, local, c, space) \
({ \
	/* Dependency order vs. p above. */ \
	/* 与上面对 p 的读取保持地址依赖顺序。 */ \
	/* 这防止先读取尚未发布完整的对象字段。 */ \
	typeof(*p) *local = (typeof(*p) *__force)READ_ONCE(p); \
	RCU_LOCKDEP_WARN(!(c), "suspicious rcu_dereference_check() usage"); \
	rcu_check_sparse(p, space); \
	((typeof(*p) __force __kernel *)(local)); \
})
#define __rcu_dereference_protected(p, local, c, space) \
({ \
	RCU_LOCKDEP_WARN(!(c), "suspicious rcu_dereference_protected() usage"); \
	rcu_check_sparse(p, space); \
	((typeof(*p) __force __kernel *)(p)); \
})
#define __rcu_dereference_raw(p, local) \
({ \
	/* Dependency order vs. p above. */ \
	/* raw 版本仍保留依赖顺序，但有意跳过保护条件证明。 */ \
	typeof(p) local = READ_ONCE(p); \
	((typeof(*p) __force __kernel *)(local)); \
})
#define rcu_dereference_raw(p) __rcu_dereference_raw(p, __UNIQUE_ID(rcu))
/*
 * rcu_dereference_raw(@p) 适合 tracing/RCU 内部已经另有保护证明的路径。
 * 返回 @p 当前值的借用指针；无 lockdep 检查意味着误用更难发现，普通数据
 * 结构读者应优先选择 rcu_dereference() 系列。
 */

/**
 * RCU_INITIALIZER() - statically initialize an RCU-protected global variable
 * @v: The value to statically initialize with.
 */
/*
 * RCU_INITIALIZER(@v) - 仅在静态初始化/其他已证明安全的上下文中把普通指针
 * 转成 __rcu 类型。它不生成存储、屏障或引用变化；@v 指向对象必须已经具备
 * 与其可见时刻相符的生命周期。
 */
#define RCU_INITIALIZER(v) (typeof(*(v)) __force __rcu *)(v)

/**
 * rcu_assign_pointer() - assign to RCU-protected pointer
 * @p: pointer to assign to
 * @v: value to assign (publish)
 *
 * Assigns the specified value to the specified RCU-protected
 * pointer, ensuring that any concurrent RCU readers will see
 * any prior initialization.
 *
 * Inserts memory barriers on architectures that require them
 * (which is most of them), and also prevents the compiler from
 * reordering the code that initializes the structure after the pointer
 * assignment.  More importantly, this call documents which pointers
 * will be dereferenced by RCU read-side code.
 *
 * In some special cases, you may use RCU_INIT_POINTER() instead
 * of rcu_assign_pointer().  RCU_INIT_POINTER() is a bit faster due
 * to the fact that it does not constrain either the CPU or the compiler.
 * That said, using RCU_INIT_POINTER() when you should have used
 * rcu_assign_pointer() is a very bad thing that results in
 * impossible-to-diagnose memory corruption.  So please be careful.
 * See the RCU_INIT_POINTER() comment header for details.
 *
 * Note that rcu_assign_pointer() evaluates each of its arguments only
 * once, appearances notwithstanding.  One of the "extra" evaluations
 * is in typeof() and the other visible only to sparse (__CHECKER__),
 * neither of which actually execute the argument.  As with most cpp
 * macros, this execute-arguments-only-once property is important, so
 * please be careful when making changes to rcu_assign_pointer() and the
 * other macros that it invokes.
 */
/*
 * rcu_assign_pointer(@p, @v) - 向 RCU 读者发布完整初始化的对象。
 *
 * @p 是被更新的 __rcu 指针左值；@v 是要发布的普通指针，可为 NULL，二者
 * 各只在运行时求值一次。非 NULL 通常通过 smp_store_release() 保证此前对
 * 对象字段的写入先于指针可见；编译期常量 NULL 无需发布对象内容，故用
 * WRITE_ONCE() 快速清空。宏无返回值，不负责串行多个更新者，也不转移/增加
 * 被指对象引用；更新者必须另行保护 @p，并在摘除旧值后等待宽限期再回收。
 *
 * RCU_INIT_POINTER() 更快但无 CPU/编译器顺序，仅可用于其文档列出的无并发
 * 初始化场景；错用会让读者拿到已发布地址却看到预初始化字段。
 */
#define rcu_assign_pointer(p, v)					      \
context_unsafe(							      \
	uintptr_t _r_a_p__v = (uintptr_t)(v);				      \
	rcu_check_sparse(p, __rcu);					      \
									      \
	if (__builtin_constant_p(v) && (_r_a_p__v) == (uintptr_t)NULL)	      \
		WRITE_ONCE((p), (typeof(p))(_r_a_p__v));		      \
	else								      \
		smp_store_release(&p, RCU_INITIALIZER((typeof(p))_r_a_p__v)); \
)

/**
 * rcu_replace_pointer() - replace an RCU pointer, returning its old value
 * @rcu_ptr: RCU pointer, whose old value is returned
 * @ptr: regular pointer
 * @c: the lockdep conditions under which the dereference will take place
 *
 * Perform a replacement, where @rcu_ptr is an RCU-annotated
 * pointer and @c is the lockdep argument that is passed to the
 * rcu_dereference_protected() call used to read that pointer.  The old
 * value of @rcu_ptr is returned, and @rcu_ptr is set to @ptr.
 */
/*
 * rcu_replace_pointer(@rcu_ptr, @ptr, @c) - 在更新侧保护条件 @c 成立时，
 * 先取得旧借用指针，再以发布语义安装 @ptr。
 *
 * 返回旧值供调用者在解锁/摘除后安排宽限期回收；宏不等待宽限期，也不替
 * 调用者持有旧对象引用。@c 必须真实描述更新锁或独占阶段，否则 protected
 * 读取可被编译器重复/合并并与并发写竞争。
 */
#define rcu_replace_pointer(rcu_ptr, ptr, c)				\
({									\
	typeof(ptr) __tmp = rcu_dereference_protected((rcu_ptr), (c));	\
	rcu_assign_pointer((rcu_ptr), (ptr));				\
	__tmp;								\
})

/**
 * rcu_access_pointer() - fetch RCU pointer with no dereferencing
 * @p: The pointer to read
 *
 * Return the value of the specified RCU-protected pointer, but omit the
 * lockdep checks for being in an RCU read-side critical section.  This is
 * useful when the value of this pointer is accessed, but the pointer is
 * not dereferenced, for example, when testing an RCU-protected pointer
 * against NULL.  Within an RCU read-side critical section, there is little
 * reason to use rcu_access_pointer().  Although rcu_access_pointer() may
 * also be used in cases where update-side locks prevent the value of the
 * pointer from changing, you should instead use rcu_dereference_protected()
 * for this use case.  It is also permissible to use rcu_access_pointer()
 * within lockless updaters to obtain the old value for an atomic operation,
 * for example, for cmpxchg().
 *
 * It is usually best to test the rcu_access_pointer() return value
 * directly in order to avoid accidental dereferences being introduced
 * by later inattentive changes.  In other words, assigning the
 * rcu_access_pointer() return value to a local variable results in an
 * accident waiting to happen.
 *
 * It is also permissible to use rcu_access_pointer() when read-side
 * access to the pointer was removed at least one grace period ago, as is
 * the case in the context of the RCU callback that is freeing up the data,
 * or after a synchronize_rcu() returns.  This can be useful when tearing
 * down multi-linked structures after a grace period has elapsed.  However,
 * rcu_dereference_protected() is normally preferred for this use case.
 */
/*
 * rcu_access_pointer(@p) - 只观察 RCU 指针值而不解引用对象。
 *
 * 返回 READ_ONCE() 得到的瞬时借用地址，无读锁检查、无引用和无对象字段
 * 可见性保证。典型用途是直接与 NULL 比较，或把旧值交给 cmpxchg()；最好
 * 不保存到局部变量，以免维护者后来误加 `local->field`。若更新侧锁稳定了
 * 指针，应改用 rcu_dereference_protected() 明确证明。宽限期之后虽然对象
 * 已无旧读者，通常也优先用 protected 版本表达销毁阶段。
 */
#define rcu_access_pointer(p) __rcu_access_pointer((p), __UNIQUE_ID(rcu), __rcu)

/**
 * rcu_dereference_check() - rcu_dereference with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * Do an rcu_dereference(), but check that the conditions under which the
 * dereference will take place are correct.  Typically the conditions
 * indicate the various locking conditions that should be held at that
 * point.  The check should return true if the conditions are satisfied.
 * An implicit check for being in an RCU read-side critical section
 * (rcu_read_lock()) is included.
 *
 * For example:
 *
 *	bar = rcu_dereference_check(foo->bar, lockdep_is_held(&foo->lock));
 *
 * could be used to indicate to lockdep that foo->bar may only be dereferenced
 * if either rcu_read_lock() is held, or that the lock required to replace
 * the bar struct at foo->bar is held.
 *
 * Note that the list of conditions may also include indications of when a lock
 * need not be held, for example during initialisation or destruction of the
 * target struct:
 *
 *	bar = rcu_dereference_check(foo->bar, lockdep_is_held(&foo->lock) ||
 *					      atomic_read(&foo->usage) == 0);
 *
 * Inserts memory barriers on architectures that require them
 * (currently only the Alpha), prevents the compiler from refetching
 * (and from merging fetches), and, more importantly, documents exactly
 * which pointers are protected by RCU and checks that the pointer is
 * annotated as __rcu.
 */
/*
 * rcu_dereference_check(@p, @c) - 在普通 RCU 读锁或替代条件 @c 下安全取得
 * 可解引用的借用指针。
 *
 * @p 是 __rcu 指针左值；@c 常为更新侧锁已持有、初始化期或销毁期条件。
 * 返回值不增加引用，只能在读侧窗口或 @c 对应保护持续期间使用。READ_ONCE
 * 防止重复/合并取指针，Alpha 等架构所需依赖屏障由底层原语补足；lockdep
 * 检查 `(c || rcu_read_lock_held())`，Sparse 同时验证 __rcu 标注。
 */
#define rcu_dereference_check(p, c) \
	__rcu_dereference_check((p), __UNIQUE_ID(rcu), \
				(c) || rcu_read_lock_held(), __rcu)

/**
 * rcu_dereference_bh_check() - rcu_dereference_bh with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * This is the RCU-bh counterpart to rcu_dereference_check().  However,
 * please note that starting in v5.0 kernels, vanilla RCU grace periods
 * wait for local_bh_disable() regions of code in addition to regions of
 * code demarked by rcu_read_lock() and rcu_read_unlock().  This means
 * that synchronize_rcu(), call_rcu, and friends all take not only
 * rcu_read_lock() but also rcu_read_lock_bh() into account.
 */
/*
 * rcu_dereference_bh_check(@p, @c) 是 BH 读侧版本。自 v5.0 起普通 RCU
 * 宽限期也等待 softirq-disabled 区域，所以其生命周期保证归入 vanilla
 * RCU；这里仍用 BH held 检查表达调用点要求。返回借用指针，作用域不得超过
 * BH/替代锁保护。
 */
#define rcu_dereference_bh_check(p, c) \
	__rcu_dereference_check((p), __UNIQUE_ID(rcu), \
				(c) || rcu_read_lock_bh_held(), __rcu)

/**
 * rcu_dereference_sched_check() - rcu_dereference_sched with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * This is the RCU-sched counterpart to rcu_dereference_check().
 * However, please note that starting in v5.0 kernels, vanilla RCU grace
 * periods wait for preempt_disable() regions of code in addition to
 * regions of code demarked by rcu_read_lock() and rcu_read_unlock().
 * This means that synchronize_rcu(), call_rcu, and friends all take not
 * only rcu_read_lock() but also rcu_read_lock_sched() into account.
 */
/*
 * rcu_dereference_sched_check(@p, @c) 对应不可抢占/sched 读者。自 v5.0 起
 * synchronize_rcu()/call_rcu() 同样等待这些区域；返回值仍只是保护窗口内
 * 的借用指针，禁止抢占不会冻结对象字段或自动获得长期引用。
 */
#define rcu_dereference_sched_check(p, c) \
	__rcu_dereference_check((p), __UNIQUE_ID(rcu), \
				(c) || rcu_read_lock_sched_held(), \
				__rcu)

/**
 * rcu_dereference_all_check() - rcu_dereference_all with debug checking
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * This is similar to rcu_dereference_check(), but allows protection
 * by all forms of vanilla RCU readers, including preemption disabled,
 * bh-disabled, and interrupt-disabled regions of code.  Note that "vanilla
 * RCU" excludes SRCU and the various Tasks RCU flavors.  Please note
 * that this macro should not be backported to any Linux-kernel version
 * preceding v5.0 due to changes in synchronize_rcu() semantics prior
 * to that version.
 */
/*
 * rcu_dereference_all_check(@p, @c) 接受普通显式读锁、禁止抢占、禁止 BH
 * 或禁止中断形成的任一 vanilla RCU 读者，但不覆盖 SRCU 和 Tasks RCU。
 * 该统一语义依赖 v5.0 之后的宽限期定义，不能向旧内核机械回移。
 */
#define rcu_dereference_all_check(p, c) \
	__rcu_dereference_check((p), __UNIQUE_ID(rcu), \
				(c) || rcu_read_lock_any_held(), \
				__rcu)

/*
 * The tracing infrastructure traces RCU (we want that), but unfortunately
 * some of the RCU checks causes tracing to lock up the system.
 *
 * The no-tracing version of rcu_dereference_raw() must not call
 * rcu_read_lock_held().
 */
/*
 * tracing 会反过来追踪 RCU；若 raw 解引用再调用 lockdep 查询，可能递归进入
 * tracing 并锁死。该版本把条件固定为真，保留稳定取值/依赖顺序但跳过检查，
 * 仅供已经由 tracing/RCU 内部协议保证安全的路径。
 */
#define rcu_dereference_raw_check(p) \
	__rcu_dereference_check((p), __UNIQUE_ID(rcu), 1, __rcu)

/**
 * rcu_dereference_protected() - fetch RCU pointer when updates prevented
 * @p: The pointer to read, prior to dereferencing
 * @c: The conditions under which the dereference will take place
 *
 * Return the value of the specified RCU-protected pointer, but omit
 * the READ_ONCE().  This is useful in cases where update-side locks
 * prevent the value of the pointer from changing.  Please note that this
 * primitive does *not* prevent the compiler from repeating this reference
 * or combining it with other references, so it should not be used without
 * protection of appropriate locks.
 *
 * This function is only for update-side use.  Using this function
 * when protected only by rcu_read_lock() will result in infrequent
 * but very ugly failures.
 */
/*
 * rcu_dereference_protected(@p, @c) - 更新侧已阻止 @p 变化时取得旧值。
 *
 * @c 必须证明更新锁、初始化独占或销毁独占；返回借用指针，无 READ_ONCE、
 * acquire/依赖读取和引用增加。正因为编译器可重复或合并访问，只有保护条件
 * 在整个使用期间保持为真才安全。仅持 rcu_read_lock() 的读者必须使用普通
 * rcu_dereference()，否则会出现低概率但严重的并发错误。
 */
#define rcu_dereference_protected(p, c) \
	__rcu_dereference_protected((p), __UNIQUE_ID(rcu), (c), __rcu)


/**
 * rcu_dereference() - fetch RCU-protected pointer for dereferencing
 * @p: The pointer to read, prior to dereferencing
 *
 * This is a simple wrapper around rcu_dereference_check().
 */
/*
 * rcu_dereference(@p) - 普通读侧最常用入口，要求当前显式 RCU 读锁有效。
 * 返回对象借用指针，不增加引用；必须在 rcu_read_unlock() 前完成使用，或
 * 通过 rcu_pointer_handoff() 所表达的引用计数/锁协议取得长期保护。
 */
#define rcu_dereference(p) rcu_dereference_check(p, 0)

/**
 * rcu_dereference_bh() - fetch an RCU-bh-protected pointer for dereferencing
 * @p: The pointer to read, prior to dereferencing
 *
 * Makes rcu_dereference_check() do the dirty work.
 */
/*
 * rcu_dereference_bh(@p) 固定替代条件为假，因而要求 BH 版读侧保护。
 * 返回借用指针并继承 bh_check 的 v5.0+ 宽限期语义。
 */
#define rcu_dereference_bh(p) rcu_dereference_bh_check(p, 0)

/**
 * rcu_dereference_sched() - fetch RCU-sched-protected pointer for dereferencing
 * @p: The pointer to read, prior to dereferencing
 *
 * Makes rcu_dereference_check() do the dirty work.
 */
/*
 * rcu_dereference_sched(@p) 要求 sched/不可抢占读侧保护，返回值只在该
 * 同上下文保护窗口内有效。
 */
#define rcu_dereference_sched(p) rcu_dereference_sched_check(p, 0)

/**
 * rcu_dereference_all() - fetch RCU-all-protected pointer for dereferencing
 * @p: The pointer to read, prior to dereferencing
 *
 * Makes rcu_dereference_check() do the dirty work.
 */
/*
 * rcu_dereference_all(@p) 接受任一 vanilla RCU 读者类别；它扩大可接受的
 * 保护证明，不扩大对象生命周期，也不包含 SRCU/Tasks RCU。
 */
#define rcu_dereference_all(p) rcu_dereference_all_check(p, 0)

/**
 * rcu_pointer_handoff() - Hand off a pointer from RCU to other mechanism
 * @p: The pointer to hand off
 *
 * This is simply an identity function, but it documents where a pointer
 * is handed off from RCU to some other synchronization mechanism, for
 * example, reference counting or locking.  In C11, it would map to
 * kill_dependency().  It could be used as follows::
 *
 *	rcu_read_lock();
 *	p = rcu_dereference(gp);
 *	long_lived = is_long_lived(p);
 *	if (long_lived) {
 *		if (!atomic_inc_not_zero(p->refcnt))
 *			long_lived = false;
 *		else
 *			p = rcu_pointer_handoff(p);
 *	}
 *	rcu_read_unlock();
 */
/*
 * rcu_pointer_handoff(@p) - 标记对象保护责任从 RCU 借用转交给其他机制。
 *
 * 它运行时原样返回 @p，不增加引用、不加锁；安全性来自调用者此前在 RCU
 * 窗口内成功执行的 atomic_inc_not_zero() 或等价操作。该标记阻断“后续访问
 * 仍依赖原地址依赖链”的错误推理，也向审阅者指出离开读锁后由谁保活对象。
 * 若引用获取失败，绝不能执行交接并在解锁后继续使用。
 */
#define rcu_pointer_handoff(p) (p)

/**
 * rcu_read_lock() - mark the beginning of an RCU read-side critical section
 *
 * When synchronize_rcu() is invoked on one CPU while other CPUs
 * are within RCU read-side critical sections, then the
 * synchronize_rcu() is guaranteed to block until after all the other
 * CPUs exit their critical sections.  Similarly, if call_rcu() is invoked
 * on one CPU while other CPUs are within RCU read-side critical
 * sections, invocation of the corresponding RCU callback is deferred
 * until after the all the other CPUs exit their critical sections.
 *
 * Both synchronize_rcu() and call_rcu() also wait for regions of code
 * with preemption disabled, including regions of code with interrupts or
 * softirqs disabled.
 *
 * Note, however, that RCU callbacks are permitted to run concurrently
 * with new RCU read-side critical sections.  One way that this can happen
 * is via the following sequence of events: (1) CPU 0 enters an RCU
 * read-side critical section, (2) CPU 1 invokes call_rcu() to register
 * an RCU callback, (3) CPU 0 exits the RCU read-side critical section,
 * (4) CPU 2 enters a RCU read-side critical section, (5) the RCU
 * callback is invoked.  This is legal, because the RCU read-side critical
 * section that was running concurrently with the call_rcu() (and which
 * therefore might be referencing something that the corresponding RCU
 * callback would free up) has completed before the corresponding
 * RCU callback is invoked.
 *
 * RCU read-side critical sections may be nested.  Any deferred actions
 * will be deferred until the outermost RCU read-side critical section
 * completes.
 *
 * You can avoid reading and understanding the next paragraph by
 * following this rule: don't put anything in an rcu_read_lock() RCU
 * read-side critical section that would block in a !PREEMPTION kernel.
 * But if you want the full story, read on!
 *
 * In non-preemptible RCU implementations (pure TREE_RCU and TINY_RCU),
 * it is illegal to block while in an RCU read-side critical section.
 * In preemptible RCU implementations (PREEMPT_RCU) in CONFIG_PREEMPTION
 * kernel builds, RCU read-side critical sections may be preempted,
 * but explicit blocking is illegal.  Finally, in preemptible RCU
 * implementations in real-time (with -rt patchset) kernel builds, RCU
 * read-side critical sections may be preempted and they may also block, but
 * only when acquiring spinlocks that are subject to priority inheritance.
 */
/*
 * rcu_read_lock() - 开始普通 RCU 读侧临界区。
 *
 * 入参：无。返回：无直接返回值。读路径随后通常用 rcu_dereference() 取得
 * 借用指针，并由同一任务/上下文中的 rcu_read_unlock() 结束窗口。临界区
 * 允许嵌套，只有最外层退出才让本读者不再阻挡对应宽限期。
 *
 * synchronize_rcu() 等待调用时已经存在的相关读者离开；call_rcu() 异步
 * 推迟回调到这些旧读者离开之后。后来才进入的新读者可与回调并发，因为
 * 它们不可能持有回调登记前取得、随后被摘除的旧借用指针。普通宽限期还把
 * 禁止抢占、禁止中断和禁止 softirq 的区间视为读者。
 *
 * 非可抢占 RCU 中临界区不得阻塞；PREEMPT_RCU 下可以被调度器抢占但不得
 * 主动阻塞；RT 特例只允许因具有优先级继承语义的 spinlock 获取而阻塞。
 * 底层调用建立读者状态，lockdep 检查 idle/EQS 下的非法调用。它不锁住
 * 更新者、不冻结对象字段，PREEMPT_RCU 下也不保证任务留在当前 CPU。
 */
static __always_inline void rcu_read_lock(void)
	__acquires_shared(RCU)
{
	__rcu_read_lock();
	__acquire_shared(RCU);
	rcu_lock_acquire(&rcu_lock_map);
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_lock() used illegally while idle");
}

/*
 * So where is rcu_write_lock()?  It does not exist, as there is no
 * way for writers to lock out RCU readers.  This is a feature, not
 * a bug -- this property is what provides RCU's performance benefits.
 * Of course, writers must coordinate with each other.  The normal
 * spinlock primitives work well for this, but any other technique may be
 * used as well.  RCU does not care how the writers keep out of each
 * others' way, as long as they do so.
 */
/*
 * 不存在 rcu_write_lock()：写者不能阻止新读者进入，这正是 RCU 读侧低
 * 开销的来源。写者必须自行用 spinlock、mutex、原子替换等方式彼此串行，
 * 再通过发布与宽限期协议保护读者。RCU 只关心旧版本何时无人再借用，不会
 * 防止两个写者同时破坏更新侧结构不变量。
 */

/**
 * rcu_read_unlock() - marks the end of an RCU read-side critical section.
 *
 * In almost all situations, rcu_read_unlock() is immune from deadlock.
 * This deadlock immunity also extends to the scheduler's runqueue
 * and priority-inheritance spinlocks, courtesy of the quiescent-state
 * deferral that is carried out when rcu_read_unlock() is invoked with
 * interrupts disabled.
 *
 * See rcu_read_lock() for more information.
 */
/*
 * rcu_read_unlock() - 结束当前普通 RCU 读侧临界区。
 *
 * 入参：无。返回：无直接返回值。入口必须与当前上下文中尚未配对的读锁
 * 对应；先完成 idle/EQS 合法性检查和 lockdep 释放，再由底层更新真实嵌套
 * 状态。最外层退出后宽限期可能取得进展；实现对中断关闭时的静止状态作
 * 延后处理，从而避免与调度器 runqueue/PI 锁形成死锁。
 *
 * 返回后原 RCU 借用指针不再安全，除非此前已成功交接给引用计数或锁。
 */
static inline void rcu_read_unlock(void)
	__releases_shared(RCU)
{
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_unlock() used illegally while idle");
	rcu_lock_release(&rcu_lock_map); /* Keep acq info for rls diags. */
	__release_shared(RCU);
	__rcu_read_unlock();
}

/**
 * rcu_read_lock_bh() - mark the beginning of an RCU-bh critical section
 *
 * This is equivalent to rcu_read_lock(), but also disables softirqs.
 * Note that anything else that disables softirqs can also serve as an RCU
 * read-side critical section.  However, please note that this equivalence
 * applies only to v5.0 and later.  Before v5.0, rcu_read_lock() and
 * rcu_read_lock_bh() were unrelated.
 *
 * Note that rcu_read_lock_bh() and the matching rcu_read_unlock_bh()
 * must occur in the same context, for example, it is illegal to invoke
 * rcu_read_unlock_bh() from one task if the matching rcu_read_lock_bh()
 * was invoked from some other task.
 */
/*
 * rcu_read_lock_bh() - 开始同时禁止 softirq 的 RCU 读侧窗口。
 *
 * 入参：无。返回：无直接返回值。local_bh_disable() 先阻止本 CPU softirq
 * 与当前路径并发，再登记普通和 RCU_BH 两个共享 token。必须由同一任务/
 * 上下文中的 rcu_read_unlock_bh() 配对，不能把入口和出口拆到不同任务。
 *
 * 自 v5.0 起，任何 softirq-disabled 区域在宽限期意义上也属于普通 RCU
 * 读者；显式 API 额外提供 lockdep 配对诊断。它不取得对象引用，也不能
 * 阻止其他 CPU 的更新者。
 */
static inline void rcu_read_lock_bh(void)
	__acquires_shared(RCU) __acquires_shared(RCU_BH)
{
	local_bh_disable();
	__acquire_shared(RCU);
	__acquire_shared(RCU_BH);
	rcu_lock_acquire(&rcu_bh_lock_map);
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_lock_bh() used illegally while idle");
}

/**
 * rcu_read_unlock_bh() - marks the end of a softirq-only RCU critical section
 *
 * See rcu_read_lock_bh() for more information.
 */
/*
 * rcu_read_unlock_bh() - 结束同上下文的 BH RCU 窗口。
 *
 * 入参：无。返回：无直接返回值。先检查/释放逻辑锁，再按 BH、普通 RCU
 * 的逆序释放静态 token，最后 local_bh_enable()；最后一步可能立即运行
 * 待处理 softirq，因此在此之前必须停止使用仅由该窗口保护的借用对象。
 */
static inline void rcu_read_unlock_bh(void)
	__releases_shared(RCU) __releases_shared(RCU_BH)
{
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_unlock_bh() used illegally while idle");
	rcu_lock_release(&rcu_bh_lock_map);
	__release_shared(RCU_BH);
	__release_shared(RCU);
	local_bh_enable();
}

/**
 * rcu_read_lock_sched() - mark the beginning of a RCU-sched critical section
 *
 * This is equivalent to rcu_read_lock(), but also disables preemption.
 * Read-side critical sections can also be introduced by anything else that
 * disables preemption, including local_irq_disable() and friends.  However,
 * please note that the equivalence to rcu_read_lock() applies only to
 * v5.0 and later.  Before v5.0, rcu_read_lock() and rcu_read_lock_sched()
 * were unrelated.
 *
 * Note that rcu_read_lock_sched() and the matching rcu_read_unlock_sched()
 * must occur in the same context, for example, it is illegal to invoke
 * rcu_read_unlock_sched() from process context if the matching
 * rcu_read_lock_sched() was invoked from an NMI handler.
 */
/*
 * rcu_read_lock_sched() - 开始禁止抢占的 sched RCU 读侧窗口。
 *
 * 入参：无。返回：无直接返回值。preempt_disable() 固定当前任务不被调度/
 * 迁移，再登记普通与 RCU_SCHED token。local_irq_disable() 等更强不可抢占
 * 区域自 v5.0 起也被普通宽限期等待，但显式 API 提供严格配对诊断。
 *
 * 入口和出口必须处于同一上下文；例如 NMI 中入口不能在进程上下文解锁。
 * 该保护只覆盖当前执行窗口，不增加对象引用，也不阻止中断。
 */
static inline void rcu_read_lock_sched(void)
	__acquires_shared(RCU) __acquires_shared(RCU_SCHED)
{
	preempt_disable();
	__acquire_shared(RCU);
	__acquire_shared(RCU_SCHED);
	rcu_lock_acquire(&rcu_sched_lock_map);
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_lock_sched() used illegally while idle");
}

/* Used by lockdep and tracing: cannot be traced, cannot call lockdep. */
/*
 * 供 lockdep/tracing 自身使用的不可追踪版本，避免诊断基础设施递归调用自己。
 * 无参数、无直接返回值；只禁止抢占并更新静态 token，不调用 lockdep，必须
 * 与同样 notrace 的出口在同一上下文配对。
 */
static inline notrace void rcu_read_lock_sched_notrace(void)
	__acquires_shared(RCU) __acquires_shared(RCU_SCHED)
{
	preempt_disable_notrace();
	__acquire_shared(RCU);
	__acquire_shared(RCU_SCHED);
}

/**
 * rcu_read_unlock_sched() - marks the end of a RCU-classic critical section
 *
 * See rcu_read_lock_sched() for more information.
 */
/*
 * rcu_read_unlock_sched() - 结束显式 sched RCU 临界区。
 *
 * 先完成 idle 检查与逻辑锁释放，再逆序释放 sched/普通 token，最后允许
 * 抢占；preempt_enable() 可能随即调度，所以所有借用对象必须在它之前用完。
 * 无参数、无直接返回值，也不等待完整宽限期。
 */
static inline void rcu_read_unlock_sched(void)
	__releases_shared(RCU) __releases_shared(RCU_SCHED)
{
	RCU_LOCKDEP_WARN(!rcu_is_watching(),
			 "rcu_read_unlock_sched() used illegally while idle");
	rcu_lock_release(&rcu_sched_lock_map);
	__release_shared(RCU_SCHED);
	__release_shared(RCU);
	preempt_enable();
}

/* Used by lockdep and tracing: cannot be traced, cannot call lockdep. */
/*
 * notrace 出口与上面的 notrace 入口配对：逆序释放静态 token 后调用
 * preempt_enable_notrace()。它有意跳过 lockdep/trace，以免形成递归。
 */
static inline notrace void rcu_read_unlock_sched_notrace(void)
	__releases_shared(RCU) __releases_shared(RCU_SCHED)
{
	__release_shared(RCU_SCHED);
	__release_shared(RCU);
	preempt_enable_notrace();
}

/*
 * rcu_read_lock_dont_migrate() - 建立普通 RCU 保护并保证窗口内不迁移。
 *
 * 入参：无。返回：无直接返回值。适用于同时借用 RCU 对象和 per-CPU 状态的
 * 路径；必须用 rcu_read_unlock_migrate() 配对。它保证 CPU 身份稳定，不等于
 * 取得对象引用。
 */
static __always_inline void rcu_read_lock_dont_migrate(void)
	__acquires_shared(RCU)
{
	/*
	 * PREEMPT_RCU 的普通读锁允许任务迁移；需要稳定 per-CPU 身份的调用者
	 * 先增加迁移禁用计数。非 PREEMPT_RCU 已由读锁禁止抢占，额外操作为空。
	 */
	if (IS_ENABLED(CONFIG_PREEMPT_RCU))
		migrate_disable();
	rcu_read_lock();
}

/*
 * rcu_read_unlock_migrate() - 结束 dont_migrate 组合保护。
 *
 * 无参数、无直接返回值；返回后既没有 RCU 借用保护，也不能继续假设 CPU
 * 身份不变。最外层解锁/启用迁移可能触发调度。
 */
static inline void rcu_read_unlock_migrate(void)
	__releases_shared(RCU)
{
	/*
	 * 先结束对象借用窗口，再恢复迁移。若反序，任务可能迁移后仍按旧 CPU 的
	 * per-CPU 假设访问 RCU 对象。PREEMPT_RCU 才需显式降低迁移禁用计数。
	 */
	rcu_read_unlock();
	if (IS_ENABLED(CONFIG_PREEMPT_RCU))
		migrate_enable();
}

/**
 * RCU_INIT_POINTER() - initialize an RCU protected pointer
 * @p: The pointer to be initialized.
 * @v: The value to initialized the pointer to.
 *
 * Initialize an RCU-protected pointer in special cases where readers
 * do not need ordering constraints on the CPU or the compiler.  These
 * special cases are:
 *
 * 1.	This use of RCU_INIT_POINTER() is NULLing out the pointer *or*
 * 2.	The caller has taken whatever steps are required to prevent
 *	RCU readers from concurrently accessing this pointer *or*
 * 3.	The referenced data structure has already been exposed to
 *	readers either at compile time or via rcu_assign_pointer() *and*
 *
 *	a.	You have not made *any* reader-visible changes to
 *		this structure since then *or*
 *	b.	It is OK for readers accessing this structure from its
 *		new location to see the old state of the structure.  (For
 *		example, the changes were to statistical counters or to
 *		other state where exact synchronization is not required.)
 *
 * Failure to follow these rules governing use of RCU_INIT_POINTER() will
 * result in impossible-to-diagnose memory corruption.  As in the structures
 * will look OK in crash dumps, but any concurrent RCU readers might
 * see pre-initialized values of the referenced data structure.  So
 * please be very careful how you use RCU_INIT_POINTER()!!!
 *
 * If you are creating an RCU-protected linked structure that is accessed
 * by a single external-to-structure RCU-protected pointer, then you may
 * use RCU_INIT_POINTER() to initialize the internal RCU-protected
 * pointers, but you must use rcu_assign_pointer() to initialize the
 * external-to-structure pointer *after* you have completely initialized
 * the reader-accessible portions of the linked structure.
 *
 * Note that unlike rcu_assign_pointer(), RCU_INIT_POINTER() provides no
 * ordering guarantees for either the CPU or the compiler.
 */
/*
 * RCU_INIT_POINTER(@p, @v) - 在无需发布顺序的特殊阶段初始化 RCU 指针。
 *
 * @p 是 __rcu 指针左值，@v 是初值，宏无返回值且不改变对象引用。合法情形
 * 仅包括：写 NULL；已经完全排除并发读者；或对象先前已安全发布，之后没有
 * 读者可见改动（或允许新位置读者看见旧状态）。WRITE_ONCE 只保证指针存储
 * 不被编译器拆分/合并，不保证对象初始化先于地址可见。
 *
 * 构造 RCU 链表时可用它填写尚未发布的内部链接，但对外根指针必须在所有
 * 读者可见字段完成后用 rcu_assign_pointer() 发布。违反此顺序会产生极难
 * 复现的半初始化读取，即使崩溃转储中的对象后来看来完全正常。
 */
#define RCU_INIT_POINTER(p, v) \
	context_unsafe( \
		rcu_check_sparse(p, __rcu); \
		WRITE_ONCE(p, RCU_INITIALIZER(v)); \
	)

/**
 * RCU_POINTER_INITIALIZER() - statically initialize an RCU protected pointer
 * @p: The pointer to be initialized.
 * @v: The value to initialized the pointer to.
 *
 * GCC-style initialization for an RCU-protected pointer in a structure field.
 */
/*
 * RCU_POINTER_INITIALIZER(@p, @v) - 用于结构体静态初始化器的 `.p = ...`
 * 形式。它只完成编译期类型标注与初值设置，无运行时屏障；静态对象在并发
 * 可见前已由启动/装载发布边界完成初始化。
 */
#define RCU_POINTER_INITIALIZER(p, v) \
		.p = RCU_INITIALIZER(v)

/**
 * kfree_rcu() - kfree an object after a grace period.
 * @ptr: pointer to kfree for double-argument invocations.
 * @rhf: the name of the struct rcu_head within the type of @ptr.
 *
 * Many rcu callbacks functions just call kfree() on the base structure.
 * These functions are trivial, but their size adds up, and furthermore
 * when they are used in a kernel module, that module must invoke the
 * high-latency rcu_barrier() function at module-unload time.
 *
 * The kfree_rcu() function handles this issue. In order to have a universal
 * callback function handling different offsets of rcu_head, the callback needs
 * to determine the starting address of the freed object, which can be a large
 * kmalloc or vmalloc allocation. To allow simply aligning the pointer down to
 * page boundary for those, only offsets up to 4095 bytes can be accommodated.
 * If the offset is larger than 4095 bytes, a compile-time error will
 * be generated in kvfree_rcu_arg_2(). If this error is triggered, you can
 * either fall back to use of call_rcu() or rearrange the structure to
 * position the rcu_head structure into the first 4096 bytes.
 *
 * The object to be freed can be allocated either by kmalloc(),
 * kmalloc_nolock(), or kmem_cache_alloc().
 *
 * Note that the allowable offset might decrease in the future.
 *
 * The BUILD_BUG_ON check must not involve any function calls, hence the
 * checks are done in macros here.
 */
/*
 * kfree_rcu(@ptr, @rhf) / kvfree_rcu(@ptr, @rhf) - 使用对象内嵌 rcu_head
 * 在宽限期后释放整个对象。
 *
 * @ptr 是待释放对象指针，宏只求值一次；@rhf 是该类型内 rcu_head 字段名。
 * 非 NULL 时节点随对象一起移交给 RCU 回收基础设施，调用者随后不得再访问
 * 对象。统一回调通过 head 到对象基址的偏移反推分配起点，可处理 kmalloc、
 * kmalloc_nolock 或 kmem_cache_alloc 对象，并避免每种类型各写一个回调及
 * 模块卸载时为这些回调承担额外 rcu_barrier()。
 *
 * 为兼容页边界向下对齐的回推方案，head 偏移必须小于 4096 字节；宏中的
 * BUILD_BUG_ON 在编译期拒绝更大偏移。若触发，须调整结构体布局或改用
 * call_rcu() 自定义回调。该允许上界未来可能缩小。
 */
#define kfree_rcu(ptr, rhf) kvfree_rcu_arg_2(ptr, rhf)
#define kvfree_rcu(ptr, rhf) kvfree_rcu_arg_2(ptr, rhf)

/**
 * kfree_rcu_mightsleep() - kfree an object after a grace period.
 * @ptr: pointer to kfree for single-argument invocations.
 *
 * When it comes to head-less variant, only one argument
 * is passed and that is just a pointer which has to be
 * freed after a grace period. Therefore the semantic is
 *
 *     kfree_rcu_mightsleep(ptr);
 *
 * where @ptr is the pointer to be freed by kvfree().
 *
 * Please note, head-less way of freeing is permitted to
 * use from a context that has to follow might_sleep()
 * annotation. Otherwise, please switch and embed the
 * rcu_head structure within the type of @ptr.
 */
/*
 * kfree_rcu_mightsleep(@ptr) / kvfree_rcu_mightsleep(@ptr) - 无内嵌 head
 * 的延迟释放形式。
 *
 * @ptr 是唯一参数，最终由 kvfree() 释放；登记路径可能睡眠，因此调用上下文
 * 必须满足 might_sleep()。原子上下文应在对象中嵌入 rcu_head 并使用双参数
 * 版本。非 NULL 对象的释放责任在调用后转给 RCU，宏无直接返回值。
 */
#define kfree_rcu_mightsleep(ptr) kvfree_rcu_arg_1(ptr)
#define kvfree_rcu_mightsleep(ptr) kvfree_rcu_arg_1(ptr)

/*
 * In mm/slab_common.c, no suitable header to include here.
 */
/*
 * 实现在 mm/slab_common.c；此处没有更合适的公共头可纳入。@head 非 NULL
 * 表示内嵌-head 形式，@ptr 是最终交给 kvfree 的对象地址；headless 形式
 * 传 NULL。函数登记延迟回收，不把结果返回给调用者。
 */
void kvfree_call_rcu(struct rcu_head *head, void *ptr);

/*
 * The BUILD_BUG_ON() makes sure the rcu_head offset can be handled. See the
 * comment of kfree_rcu() for details.
 */
/*
 * 双参数底层宏先把 @ptr 保存到 ___p，保证带副作用表达式只求值一次；
 * NULL 是无操作。非 NULL 时编译期验证 @rhf 偏移，再把内嵌 head 和对象
 * 基址一并登记。登记成功后对象所有权已转移，调用者没有取消接口。
 */
#define kvfree_rcu_arg_2(ptr, rhf)					\
do {									\
	typeof (ptr) ___p = (ptr);					\
									\
	if (___p) {							\
		BUILD_BUG_ON(offsetof(typeof(*(ptr)), rhf) >= 4096);	\
		kvfree_call_rcu(&((___p)->rhf), (void *) (___p));	\
	}								\
} while (0)

/*
 * 单参数底层宏同样只求值一次；没有可复用的内嵌 head，故把 NULL 与对象
 * 地址交给可能睡眠的 headless 回收路径。NULL 输入保持无副作用。
 */
#define kvfree_rcu_arg_1(ptr)					\
do {								\
	typeof(ptr) ___p = (ptr);				\
								\
	if (___p)						\
		kvfree_call_rcu(NULL, (void *) (___p));		\
} while (0)

/*
 * Place this after a lock-acquisition primitive to guarantee that
 * an UNLOCK+LOCK pair acts as a full barrier.  This guarantee applies
 * if the UNLOCK and LOCK are executed by the same CPU or if the
 * UNLOCK and LOCK operate on the same lock variable.
 */
/*
 * smp_mb__after_unlock_lock() 应紧跟一次加锁原语，用来把前一个 unlock 与
 * 当前 lock 组合提升为全屏障；无论两次操作由同一 CPU 执行，还是作用于
 * 同一锁变量，屏障前后的内存访问都不得跨越这对操作重排。弱 release/
 * acquire 架构需要显式 smp_mb()，强架构的锁语义已满足，宏为空。
 */
#ifdef CONFIG_ARCH_WEAK_RELEASE_ACQUIRE
#define smp_mb__after_unlock_lock()	smp_mb()  /* Full ordering for lock. */
/* 为锁的 unlock+lock 组合补足全序；这是内存序保证，不是额外互斥。 */
#else /* #ifdef CONFIG_ARCH_WEAK_RELEASE_ACQUIRE */
#define smp_mb__after_unlock_lock()	do { } while (0)
#endif /* #else #ifdef CONFIG_ARCH_WEAK_RELEASE_ACQUIRE */


/* Has the specified rcu_head structure been handed to call_rcu()? */
/*
 * 下列接口只用于诊断指定 rcu_head 是否已经交给 call_rcu()，不是通用的
 * “回调是否完成”状态机。调用者必须额外排除与登记、检查和回调执行的竞态。
 */

/**
 * rcu_head_init - Initialize rcu_head for rcu_head_after_call_rcu()
 * @rhp: The rcu_head structure to initialize.
 *
 * If you intend to invoke rcu_head_after_call_rcu() to test whether a
 * given rcu_head structure has already been passed to call_rcu(), then
 * you must also invoke this rcu_head_init() function on it just after
 * allocating that structure.  Calls to this function must not race with
 * calls to call_rcu(), rcu_head_after_call_rcu(), or callback invocation.
 */
/*
 * rcu_head_init(@rhp) - 为 after_call_rcu 诊断协议写入“尚未登记”哨兵。
 *
 * @rhp 是刚随所属对象分配、由调用者独占的内嵌节点借用指针；不可为 NULL。
 * 返回：无直接返回值。副作用：把 func 设为全一非法回调地址。初始化不能与
 * call_rcu()、状态检查或回调执行并发；它不向 RCU 登记节点，也不初始化
 * debugobjects 生命周期。
 */
static inline void rcu_head_init(struct rcu_head *rhp)
{
	rhp->func = (rcu_callback_t)~0L;
}

/**
 * rcu_head_after_call_rcu() - Has this rcu_head been passed to call_rcu()?
 * @rhp: The rcu_head structure to test.
 * @f: The function passed to call_rcu() along with @rhp.
 *
 * Returns @true if the @rhp has been passed to call_rcu() with @func,
 * and @false otherwise.  Emits a warning in any other case, including
 * the case where @rhp has already been invoked after a grace period.
 * Calls to this function must not race with callback invocation.  One way
 * to avoid such races is to enclose the call to rcu_head_after_call_rcu()
 * in an RCU read-side critical section that includes a read-side fetch
 * of the pointer to the structure containing @rhp.
 */
/*
 * rcu_head_after_call_rcu(@rhp, @f) - 检查节点是否正以指定回调登记。
 *
 * @rhp 是被测节点的借用指针，所属对象必须在整个检查期间存活；@f 是预期
 * 传给 call_rcu() 的函数。READ_ONCE 稳定取得 func：等于 @f 返回 true；
 * 仍为 rcu_head_init() 哨兵返回 false；其他值（包括回调已执行后的状态）
 * 告警并返回 false。
 *
 * 本函数不能与回调执行并发，读侧窗口只有在它同时保护“取得包含 @rhp 的
 * 对象指针”时才可排除释放竞态。返回 true 只证明观察到登记回调，不证明
 * 宽限期或回调已经完成；无所有权变化、不会睡眠。
 */
static inline bool
rcu_head_after_call_rcu(struct rcu_head *rhp, rcu_callback_t f)
{
	rcu_callback_t func = READ_ONCE(rhp->func);

	/*
	 * func 是节点状态的瞬时快照：匹配预期回调是唯一 true 路径；任何非哨兵
	 * 的不匹配状态都说明调用协议或时序不符合该诊断 API 的要求。
	 */
	if (func == f)
		return true;
	WARN_ON_ONCE(func != (rcu_callback_t)~0L);
	return false;
}

/* kernel/ksysfs.c definitions */
/*
 * kernel/ksysfs.c 暴露的全局策略开关：rcu_expedited 请求加速同步宽限期，
 * rcu_normal 强制普通模式。它们由启动参数/sysfs 路径读写、由 RCU 实现
 * 读取，是策略状态而非每次调用的所有权对象。
 */
extern int rcu_expedited;
extern int rcu_normal;

/*
 * scope guard 支持：guard(rcu) 在进入作用域时调用 rcu_read_lock()，离开
 * 任一出口时自动调用 rcu_read_unlock()；属性声明把同一共享 token 契约
 * 告诉静态分析器。它结构化配对关系，但不会把作用域内借用指针自动升级成
 * 长期引用。
 */
DEFINE_LOCK_GUARD_0(rcu, rcu_read_lock(), rcu_read_unlock())
DECLARE_LOCK_GUARD_0_ATTRS(rcu, __acquires_shared(RCU), __releases_shared(RCU))

#endif /* __LINUX_RCUPDATE_H */
