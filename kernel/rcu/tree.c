// SPDX-License-Identifier: GPL-2.0+
/*
 * 中文学习注释：OpenAI GPT-5 Codex（2026-07-28）。
 *
 * 文件地图
 * ========
 *
 * 本文件实现 Tree RCU 的常规宽限期、静止状态汇聚、callback 推进以及 CPU 热插拔
 * 协议。读侧临界区不在这里逐个登记；更新侧把回调交给每 CPU `rcu_data.cblist`，
 * Tree RCU 用 `rcu_node` 汇聚各 CPU/任务的静止状态（QS），根节点确认旧读者全部
 * 离开后推进 `rcu_state.gp_seq`，再把已成熟 callback 交给 softirq 或 rcuc kthread。
 *
 * 主链一：
 *   call_rcu()
 *     → __call_rcu_common() → rcutree_enqueue()
 *     → rcu_accelerate_cbs() 请求/关联未来 GP
 *     → GP 完成后 rcu_advance_cbs()
 *     → rcu_core() → rcu_do_batch() 调用 callback
 *
 * 主链二：
 *   synchronize_rcu()
 *     → 登记一个等待请求/启动 GP
 *     → rcu_gp_kthread() 执行 init → FQS loop → cleanup
 *     → QS 从 rcu_data 上报到叶 rcu_node，再沿树清 qsmask
 *     → 根 qsmask 清零，GP 完成并唤醒等待者
 *
 * 核心对象与同步：
 * - `rcu_state` 是全局 GP 状态机、rcu_node 树、barrier/expedited/synchronize 请求的
 *   根对象；贯穿系统生命周期。
 * - 每 CPU `rcu_data` 保存本 CPU 看到的 GP 序号、QS 状态、dynticks 快照和分段
 *   callback 链。CPU hotplug 会迁移 callback，但对象本身是静态 per-CPU 存储。
 * - `rcu_node.lock` 保护该节点的 qsmask、GP 快照及阻塞任务状态；锁序从叶向根逐层
 *   释放再获取，不能同时逆序持有。序号通过 rcu_seq/READ_ONCE/WRITE_ONCE 让无锁
 *   观察者取得可能过时但自洽的快照。
 * - dynticks/EQS 计数判断 CPU 是否在扩展静止状态；奇偶值与快照比较只证明某段
 *   时间内经过静止状态，不冻结 CPU。必要的屏障把“退出旧读侧”排在“上报 QS”之前。
 *
 * Tree 结构把全局锁竞争分摊到分层位图，适合大 CPU 数；代价是 GP 序号、每层
 * qsmask、dynticks、热插拔和 callback 分段必须共同维持复杂不变量。强制 QS/FQS
 * 只催促或重新采样，不允许把仍可能处于旧读侧的 CPU 误报为静止。加速版 RCU、
 * Tasks RCU、SRCU 和 nocb 的部分实现位于相邻文件，本文件只在接口处与它们协作。
 */
/*
 * Read-Copy Update mechanism for mutual exclusion (tree-based version)
 *
 * Copyright IBM Corporation, 2008
 *
 * Authors: Dipankar Sarma <dipankar@in.ibm.com>
 *	    Manfred Spraul <manfred@colorfullife.com>
 *	    Paul E. McKenney <paulmck@linux.ibm.com>
 *
 * Based on the original work by Paul McKenney <paulmck@linux.ibm.com>
 * and inputs from Rusty Russell, Andrea Arcangeli and Andi Kleen.
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *	Documentation/RCU
 */
/*
 * 本文件是树形 RCU 的互斥/延迟回收实现，源自 IBM 与早期 Tree RCU 工作。RCU
 * 允许读者低开销并发访问，更新者在发布新版本后等待一个宽限期，确保旧读者退出，
 * 才回收旧对象。机制、内存序和 API 背景见 Documentation/RCU；这里重点解释当前
 * 版本如何用分层 rcu_node、per-CPU rcu_data 和 GP kthread 实现这些保证。
 */

#define pr_fmt(fmt) "rcu: " fmt

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/rcupdate_wait.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/nmi.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/export.h>
#include <linux/completion.h>
#include <linux/kmemleak.h>
#include <linux/moduleparam.h>
#include <linux/panic.h>
#include <linux/panic_notifier.h>
#include <linux/percpu.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/mutex.h>
#include <linux/time.h>
#include <linux/kernel_stat.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <uapi/linux/sched/types.h>
#include <linux/prefetch.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/trace_events.h>
#include <linux/suspend.h>
#include <linux/ftrace.h>
#include <linux/tick.h>
#include <linux/sysrq.h>
#include <linux/kprobes.h>
#include <linux/gfp.h>
#include <linux/oom.h>
#include <linux/smpboot.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/sched/isolation.h>
#include <linux/sched/clock.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/kasan.h>
#include <linux/context_tracking.h>
#include "../time/tick-internal.h"

#include "tree.h"
#include "rcu.h"

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "rcutree."

/* Data structures. */
/*
 * 数据结构入口：rcu_data 是每 CPU 热状态，rcu_state 是全局状态与 rcu_node 树。
 * `gpwrap=true` 使启动期序号环绕测试路径处于已知状态；cleanup work 用进程上下文
 * 完成 synchronize_rcu() 请求批量回收。
 */
static void rcu_sr_normal_gp_cleanup_work(struct work_struct *);

static DEFINE_PER_CPU_SHARED_ALIGNED(struct rcu_data, rcu_data) = {
	.gpwrap = true,
};

/*
 * rcu_get_gpwrap_count() - 读取指定 CPU 检测到的 GP 序号环绕次数。
 * @cpu 必须是 possible CPU；返回 READ_ONCE 快照，仅供测试/诊断，不提供稳定同步点。
 */
/*
 * 补充契约：调用者为 torture/诊断代码；@cpu 是纯输入 CPU 编号，不改变任何
 * ownership。任意可读取 per-CPU 数据的上下文均可调用，不取锁、不睡眠。返回
 * gpwrap_count 的瞬时无符号计数，CPU 并发更新时允许结果立即过期，无失败类别。
 */
int rcu_get_gpwrap_count(int cpu)
{
	struct rcu_data *rdp = per_cpu_ptr(&rcu_data, cpu);

	return READ_ONCE(rdp->gpwrap_count);
}
EXPORT_SYMBOL_GPL(rcu_get_gpwrap_count);

static struct rcu_state rcu_state = {
	.level = { &rcu_state.node[0] },
	.gp_state = RCU_GP_IDLE,
	.gp_seq = (0UL - 300UL) << RCU_SEQ_CTR_SHIFT,
	.barrier_mutex = __MUTEX_INITIALIZER(rcu_state.barrier_mutex),
	.barrier_lock = __RAW_SPIN_LOCK_UNLOCKED(rcu_state.barrier_lock),
	.name = RCU_NAME,
	.abbr = RCU_ABBR,
	.exp_mutex = __MUTEX_INITIALIZER(rcu_state.exp_mutex),
	.exp_wake_mutex = __MUTEX_INITIALIZER(rcu_state.exp_wake_mutex),
	.ofl_lock = __ARCH_SPIN_LOCK_UNLOCKED,
	.srs_cleanup_work = __WORK_INITIALIZER(rcu_state.srs_cleanup_work,
		rcu_sr_normal_gp_cleanup_work),
	.srs_cleanups_pending = ATOMIC_INIT(0),
#ifdef CONFIG_RCU_NOCB_CPU
	.nocb_mutex = __MUTEX_INITIALIZER(rcu_state.nocb_mutex),
#endif
};
/*
 * rcu_state 初值把 gp_seq 放在零值之前 300 个计数单位，既留出启动期测试空间又避免
 * 初始序号与“未观察”哨兵混淆。level[0] 指向 node 数组根；barrier/expedited/
 * offline/synchronize-request 各有独立锁或 work。字段在 tree.h 定义，其保护规则
 * 需与 rcu_node.lock、ofl_lock 及相应 mutex 一起阅读。
 */

/* Dump rcu_node combining tree at boot to verify correct setup. */
/* 启动时打印 rcu_node 汇聚树，用于核对层级、扇出和 CPU 覆盖范围是否正确。 */
static bool dump_tree;
module_param(dump_tree, bool, 0444);
/* By default, use RCU_SOFTIRQ instead of rcuc kthreads. */
/*
 * 默认由 RCU_SOFTIRQ 执行本地 core 工作，而不是交给每 CPU 的 rcuc kthread。
 * PREEMPT_RT 会改变该选择，以免较长的 softirq 执行破坏实时调度约束。
 */
static bool use_softirq = !IS_ENABLED(CONFIG_PREEMPT_RT);
#ifndef CONFIG_PREEMPT_RT
module_param(use_softirq, bool, 0444);
#endif
/* Control rcu_node-tree auto-balancing at boot time. */
/* 控制启动期是否自动平衡 rcu_node 树；只读模块参数，建树完成后不再改变拓扑。 */
static bool rcu_fanout_exact;
module_param(rcu_fanout_exact, bool, 0444);
/* Increase (but not decrease) the RCU_FANOUT_LEAF at boot time. */
/*
 * 启动时允许增大、但不允许减小叶节点扇出；这样可降低树高，但会扩大单个叶节点
 * 锁和位图的竞争范围。该值只参与初始化几何计算。
 */
static int rcu_fanout_leaf = RCU_FANOUT_LEAF;
module_param(rcu_fanout_leaf, int, 0444);
int rcu_num_lvls __read_mostly = RCU_NUM_LVLS;
/* Number of rcu_nodes at specified level. */
/* num_rcu_lvl[level] 记录指定层实际使用的 rcu_node 数量，建树后供遍历边界使用。 */
int num_rcu_lvl[] = NUM_RCU_LVL_INIT;
int rcu_num_nodes __read_mostly = NUM_RCU_NODES; /* Total # rcu_nodes in use. */
/* 上一行行尾英文说明总节点数；该值初始化后主要读取，因此标为 __read_mostly。 */

/*
 * The rcu_scheduler_active variable is initialized to the value
 * RCU_SCHEDULER_INACTIVE and transitions RCU_SCHEDULER_INIT just before the
 * first task is spawned.  So when this variable is RCU_SCHEDULER_INACTIVE,
 * RCU can assume that there is but one task, allowing RCU to (for example)
 * optimize synchronize_rcu() to a simple barrier().  When this variable
 * is RCU_SCHEDULER_INIT, RCU must actually do all the hard work required
 * to detect real grace periods.  This variable is also used to suppress
 * boot-time false positives from lockdep-RCU error checking.  Finally, it
 * transitions from RCU_SCHEDULER_INIT to RCU_SCHEDULER_RUNNING after RCU
 * is fully initialized, including all of its kthreads having been spawned.
 */
/*
 * rcu_scheduler_active 初值为 RCU_SCHEDULER_INACTIVE，在创建第一个任务前变为
 * RCU_SCHEDULER_INIT。INACTIVE 期间系统只有一个任务，synchronize_rcu() 等待可
 * 退化为单纯屏障，并抑制 lockdep-RCU 的启动期误报；INIT 以后必须执行真实 GP
 * 检测。RCU 完整初始化且全部 kthread 已创建后，状态最终发布为 RUNNING。
 *
 * 该变量是启动阶段状态机而非互斥锁：读者只能据其判断哪些 RCU 服务已经可用，
 * 不能依赖它保护其他字段。
 */
int rcu_scheduler_active __read_mostly;
EXPORT_SYMBOL_GPL(rcu_scheduler_active);

/*
 * The rcu_scheduler_fully_active variable transitions from zero to one
 * during the early_initcall() processing, which is after the scheduler
 * is capable of creating new tasks.  So RCU processing (for example,
 * creating tasks for RCU priority boosting) must be delayed until after
 * rcu_scheduler_fully_active transitions from zero to one.  We also
 * currently delay invocation of any RCU callbacks until after this point.
 *
 * It might later prove better for people registering RCU callbacks during
 * early boot to take responsibility for these callbacks, but one step at
 * a time.
 */
/*
 * rcu_scheduler_fully_active 在 early_initcall 阶段由 0 变为 1，此时调度器已经能够
 * 创建新任务。RCU boost 线程创建和 callback 调用在此前都必须延后，否则会把工作
 * 交给尚未具备运行条件的调度基础设施。原文也保留了一种未来可能的设计：让早期
 * callback 注册者自行负责回调；当前实现尚未采用该方案。
 */
static int rcu_scheduler_fully_active __read_mostly;
/*
 * rcu_scheduler_active 的状态机为 INACTIVE→INIT→RUNNING。INACTIVE 只有启动任务，
 * synchronize_rcu() 可退化为屏障；INIT 后必须真实检测 GP；RUNNING 表示 GP/回调
 * kthread 全部就绪。fully_active 更晚发布，约束 boost 线程创建与 callback 调用，
 * 防止早期启动在调度器尚不能创建/运行任务时进入常规异步路径。
 */

static void rcu_report_qs_rnp(unsigned long mask, struct rcu_node *rnp,
			      unsigned long gps, unsigned long flags);
static void invoke_rcu_core(void);
static void rcu_report_exp_rdp(struct rcu_data *rdp);
static void rcu_report_qs_rdp(struct rcu_data *rdp);
static void check_cb_ovld_locked(struct rcu_data *rdp, struct rcu_node *rnp);
static bool rcu_rdp_is_offloaded(struct rcu_data *rdp);
static bool rcu_rdp_cpu_online(struct rcu_data *rdp);
static bool rcu_init_invoked(void);
static void rcu_cleanup_dead_rnp(struct rcu_node *rnp_leaf);
static void rcu_init_new_rnp(struct rcu_node *rnp_leaf);

/*
 * rcuc/rcub/rcuop kthread realtime priority. The "rcuop"
 * real-time priority(enabling/disabling) is controlled by
 * the extra CONFIG_RCU_NOCB_CPU_CB_BOOST configuration.
 */
/*
 * kthread_prio 是 rcuc、rcub 和 rcuop 线程的实时优先级。rcuop 是否采用实时优先级
 * 还受 CONFIG_RCU_NOCB_CPU_CB_BOOST 控制；参数只在启动时配置，运行期读取。
 */
static int kthread_prio = IS_ENABLED(CONFIG_RCU_BOOST) ? 1 : 0;
module_param(kthread_prio, int, 0444);

/* Delay in jiffies for grace-period initialization delays, debug only. */
/*
 * 下列 GP 延迟参数以 jiffy 为单位，仅用于调试时放大竞态窗口：分别插入 GP
 * 初始化前、初始化中和清理阶段。它们不属于 RCU 正确性协议，正常配置为零。
 */

static int gp_preinit_delay;
module_param(gp_preinit_delay, int, 0444);
static int gp_init_delay;
module_param(gp_init_delay, int, 0444);
static int gp_cleanup_delay;
module_param(gp_cleanup_delay, int, 0444);
static int nohz_full_patience_delay;
module_param(nohz_full_patience_delay, int, 0444);
static int nohz_full_patience_delay_jiffies;
/*
 * 这些模块参数只改变测试延时、FQS 耐心值和 RCU kthread 实时优先级，不改变正确性
 * 条件。延时用于扩大竞态窗口；nohz_full patience 避免过早打扰隔离 CPU；严格 GP
 * 配置下 unlock delay 强化“每次读侧退出都制造边界”的测试行为。
 */

// Add delay to rcu_read_unlock() for strict grace periods.
/*
 * 严格 GP 配置可在 rcu_read_unlock() 增加调试延迟，以扩大读侧退出与 GP 扫描的
 * 竞态窗口；关闭 CONFIG_RCU_STRICT_GRACE_PERIOD 时参数不暴露。
 */
static int rcu_unlock_delay;
#ifdef CONFIG_RCU_STRICT_GRACE_PERIOD
module_param(rcu_unlock_delay, int, 0444);
#endif

/* Retrieve RCU kthreads priority for rcutorture */
/* rcu_get_gp_kthreads_prio() 返回 RCU GP/回调线程配置优先级快照；无入参和副作用。 */
int rcu_get_gp_kthreads_prio(void)
{
	return kthread_prio;
}
EXPORT_SYMBOL_GPL(rcu_get_gp_kthreads_prio);

/*
 * Number of grace periods between delays, normalized by the duration of
 * the delay.  The longer the delay, the more the grace periods between
 * each delay.  The reason for this normalization is that it means that,
 * for non-zero delays, the overall slowdown of grace periods is constant
 * regardless of the duration of the delay.  This arrangement balances
 * the need for long delays to increase some race probabilities with the
 * need for fast grace periods to increase other race probabilities.
 */
/*
 * 调试延迟之间的 GP 间隔按延迟时长归一化：延迟越长，插入延迟的 GP 越稀疏，
 * 使非零配置造成的总体减速大致恒定。这样既能用长暂停放大一类竞态，又不会把
 * GP 频率压得过低而失去观察另一类竞态的机会。
 */
#define PER_RCU_NODE_PERIOD 3	/* Number of grace periods between delays for debugging. */

/*
 * Return true if an RCU grace period is in progress.  The READ_ONCE()s
 * permit this function to be invoked without holding the root rcu_node
 * structure's ->lock, but of course results can be subject to change.
 */
/*
 * rcu_gp_in_progress() - 无锁判断 gp_seq 当前是否处于“GP 已开始、未结束”状态。
 * 返回瞬时布尔值；READ_ONCE 只防撕裂/编译器重取，结果返回后可立即变化。
 */
/*
 * 补充契约：入参无；可从无锁 fast path、IRQ 或进程上下文调用，不睡眠、无副作用。
 * 非零表示采样到 gp_seq 的“进行中”状态，零表示空闲；调用者若据此修改 GP 状态，
 * 仍须在 root/leaf rcu_node 锁下重新验证。
 */
static int rcu_gp_in_progress(void)
{
	return rcu_seq_state(rcu_seq_current(&rcu_state.gp_seq));
}

/*
 * Return the number of callbacks queued on the specified CPU.
 * Handles both the nocbs and normal cases.
 */
/* rcu_get_n_cbs_cpu() 返回指定 CPU 已启用 segcblist 的 callback 数，否则零；诊断快照。 */
/*
 * 补充契约：@cpu 为有效 possible CPU 编号；函数借用其 rcu_data，不取得引用。
 * 不要求 cblist 锁且不睡眠，因此返回值只适合统计：列表禁用或为空返回 0，否则
 * 返回采样时 callback 总数；不保证返回后数量不变，也不修改队列。
 */
static long rcu_get_n_cbs_cpu(int cpu)
{
	struct rcu_data *rdp = per_cpu_ptr(&rcu_data, cpu);

	if (rcu_segcblist_is_enabled(&rdp->cblist))
		return rcu_segcblist_n_cbs(&rdp->cblist);
	return 0;
}

/**
 * rcu_softirq_qs - Provide a set of RCU quiescent states in softirq processing
 *
 * Mark a quiescent state for RCU, Tasks RCU, and Tasks Trace RCU.
 * This is a special-purpose function to be used in the softirq
 * infrastructure and perhaps the occasional long-running softirq
 * handler.
 *
 * Note that from RCU's viewpoint, a call to rcu_softirq_qs() is
 * equivalent to momentarily completely enabling preemption.  For
 * example, given this code::
 *
 *	local_bh_disable();
 *	do_something();
 *	rcu_softirq_qs();  // A
 *	do_something_else();
 *	local_bh_enable();  // B
 *
 * A call to synchronize_rcu() that began concurrently with the
 * call to do_something() would be guaranteed to wait only until
 * execution reached statement A.  Without that rcu_softirq_qs(),
 * that same synchronize_rcu() would instead be guaranteed to wait
 * until execution reached statement B.
 */
/*
 * rcu_softirq_qs() - 长 softirq 主动声明一次等价于短暂完全可抢占的 QS。
 * 无入参/返回；同时通知 Tree/Tasks/Tasks Trace RCU。调用点必须确认此前 RCU 读侧
 * 保护已结束，因为并发 synchronize_rcu() 可只等到这里而不再等 local_bh_enable()。
 */
void rcu_softirq_qs(void)
{
	RCU_LOCKDEP_WARN(lock_is_held(&rcu_bh_lock_map) ||
			 lock_is_held(&rcu_lock_map) ||
			 lock_is_held(&rcu_sched_lock_map),
			 "Illegal rcu_softirq_qs() in RCU read-side critical section");
	rcu_qs();
	rcu_preempt_deferred_qs(current);
	rcu_tasks_qs(current, false);
}

/*
 * Reset the current CPU's RCU_WATCHING counter to indicate that the
 * newly onlined CPU is no longer in an extended quiescent state.
 * This will either leave the counter unchanged, or increment it
 * to the next non-quiescent value.
 *
 * The non-atomic test/increment sequence works because the upper bits
 * of the ->state variable are manipulated only by the corresponding CPU,
 * or when the corresponding CPU is offline.
 */
/* rcu_watching_online() 在 CPU online 且 RCU watching 时完成本 CPU 状态核对；无返回。 */
/*
 * 补充契约：入参无，只能由正在上线的当前 CPU 在 hotplug 串行窗口调用；不睡眠。
 * 若 context-tracking 已处 watching 则无操作，否则把本 CPU 计数推进到下一个
 * watching 值。副作用只落在当前 CPU CT 状态，不转移对象 ownership。
 */
static void rcu_watching_online(void)
{
	if (ct_rcu_watching() & CT_RCU_WATCHING)
		return;
	ct_state_inc(CT_RCU_WATCHING);
}

/*
 * Return true if the snapshot returned from ct_rcu_watching()
 * indicates that RCU is in an extended quiescent state.
 */
/* rcu_watching_snap_in_eqs() 以 dynticks 快照奇偶判断采样瞬间是否处于 EQS。 */
/*
 * 补充契约：@snap 是 ct_rcu_watching*() 返回的纯值快照，可来自远端 CPU；无锁、
 * 不睡眠、无副作用。返回 true 表示快照的 watching 位为零，即采样点处于 EQS；
 * false 只说明当时被 RCU 监视，不代表 CPU 现在仍保持该状态。
 */
static bool rcu_watching_snap_in_eqs(int snap)
{
	return !(snap & CT_RCU_WATCHING);
}

/**
 * rcu_watching_snap_stopped_since() - Has RCU stopped watching a given CPU
 * since the specified @snap?
 *
 * @rdp: The rcu_data corresponding to the CPU for which to check EQS.
 * @snap: rcu_watching snapshot taken when the CPU wasn't in an EQS.
 *
 * Returns true if the CPU corresponding to @rdp has spent some time in an
 * extended quiescent state since @snap. Note that this doesn't check if it
 * /still/ is in an EQS, just that it went through one since @snap.
 *
 * This is meant to be used in a loop waiting for a CPU to go through an EQS.
 */
/*
 * rcu_watching_snap_stopped_since() - 判断 @rdp CPU 自 @snap 后是否经过 EQS/停止 watching。
 * 返回基于新旧 dynticks 的布尔结论；不锁 CPU，只用于 QS 证据采样。
 */
/*
 * 补充契约：@rdp 是远端 CPU rcu_data 的只读借用指针；@snap 必须是在该 CPU 非
 * EQS 时取得的旧 watching 值。调用者通常在 FQS 扫描中，可持 node 锁，函数自身
 * 不睡眠。true 表示 CPU 自快照后至少穿过一次 EQS，false 表示尚无此证据；它不
 * 判断 CPU 当前是否仍在 EQS，也不修改 @rdp。
 */
static bool rcu_watching_snap_stopped_since(struct rcu_data *rdp, int snap)
{
	/*
	 * The first failing snapshot is already ordered against the accesses
	 * performed by the remote CPU after it exits idle.
	 *
	 * The second snapshot therefore only needs to order against accesses
	 * performed by the remote CPU prior to entering idle and therefore can
	 * rely solely on acquire semantics.
	 */
	/*
	 * 第一次“未观察到 EQS”的快照，已经与远端 CPU 退出 idle 后的访问建立顺序。
	 * 因此第二次采样只需约束远端进入 idle 之前的访问，ct_rcu_watching_cpu_acquire()
	 * 的 acquire 语义即可；若缺少该顺序，更新侧可能把尚未完成的旧读侧访问误判为
	 * 已跨越宽限期。
	 */
	if (WARN_ON_ONCE(rcu_watching_snap_in_eqs(snap)))
		return true;

	return snap != ct_rcu_watching_cpu_acquire(rdp->cpu);
}

/*
 * Return true if the referenced integer is zero while the specified
 * CPU remains within a single extended quiescent state.
 */
/*
 * rcu_watching_zero_in_eqs() - 检查 CPU 当前 dynticks 值并可通过 @vp 输出快照。
 * 返回是否处于 EQS；@vp 可空。结果是瞬时证据，调用者仍需按 GP 序号验证适用代际。
 */
/*
 * 修正说明：@vp 不可为空，代码会无条件 READ_ONCE(*vp)；它是借用的只读整数指针，
 * 本函数不写入也不取得引用。返回 true 的精确含义是：观察到 *vp==0 的前后，@cpu
 * 始终处于同一个 EQS；false 表示值非零或 CPU 的 watching 序号发生变化。
 */
bool rcu_watching_zero_in_eqs(int cpu, int *vp)
{
	int snap;

	// If not quiescent, force back to earlier extended quiescent state.
	/*
	 * 先清 watching 位，把采样规整到最近的 EQS 序号；若 CPU 当时并非静止，这会
	 * 选择其更早一次 EQS，后续二次读取会因序号变化而拒绝错误匹配。
	 */
	snap = ct_rcu_watching_cpu(cpu) & ~CT_RCU_WATCHING;
	smp_rmb(); // Order CT state and *vp reads.
	/* 先固定 context-tracking 快照，再读取 *vp，防止两者被 CPU/编译器重排。 */
	if (READ_ONCE(*vp))
		return false;  // Non-zero, so report failure;
	/* 行尾英文说明：被观察值非零，条件不成立，立即返回失败。 */
	smp_rmb(); // Order *vp read and CT state re-read.
	/* 把 *vp 的零值读取排在第二次 CT 状态采样之前，封闭一致性检查窗口。 */

	// If still in the same extended quiescent state, we are good!
	/*
	 * 两次 CT 采样仍是同一 EQS，说明 *vp==0 的整个观察窗口 CPU 都未重新进入内核
	 * watching 区间，因此结果可作为稳定 QS 证据。
	 */
	return snap == ct_rcu_watching_cpu(cpu);
}

/*
 * Let the RCU core know that this CPU has gone through the scheduler,
 * which is a quiescent state.  This is called when the need for a
 * quiescent state is urgent, so we burn an atomic operation and full
 * memory barriers to let the RCU core know about it, regardless of what
 * this CPU might (or might not) do in the near future.
 *
 * We inform the RCU core by emulating a zero-duration dyntick-idle period.
 *
 * The caller must have disabled interrupts and must not be idle.
 */
/*
 * rcu_momentary_eqs() - 当前 CPU 在安全点制造一次可被 GP 观察的瞬时 EQS 边界。
 * notrace 防止递归；无返回。屏障保证此前读侧访问完成后才推进 dynticks/QS 证据。
 */
/*
 * 补充契约：入参无；调用者必须关闭 IRQ、不得处于 idle，也不得位于未结束的 RCU
 * 读侧临界区。函数不可睡眠，通过 CT 计数制造零时长 EQS 并处理 deferred QS；
 * 返回无直接值，成功后 GP 扫描可把该序号变化作为 QS 证据。
 */
notrace void rcu_momentary_eqs(void)
{
	int seq;

	raw_cpu_write(rcu_data.rcu_need_heavy_qs, false);
	seq = ct_state_inc(2 * CT_RCU_WATCHING);
	/* It is illegal to call this from idle state. */
	/*
	 * 不允许从 idle 状态调用：瞬时增加两个计数单位只能制造“经过一次 EQS”的序号
	 * 证据，不能替代 idle 路径自身的 watching 状态转换。
	 */
	WARN_ON_ONCE(!(seq & CT_RCU_WATCHING));
	rcu_preempt_deferred_qs(current);
}
EXPORT_SYMBOL_GPL(rcu_momentary_eqs);

/**
 * rcu_is_cpu_rrupt_from_idle - see if 'interrupted' from idle
 *
 * If the current CPU is idle and running at a first-level (not nested)
 * interrupt, or directly, from idle, return true.
 *
 * The caller must have at least disabled IRQs.
 */
/* rcu_is_cpu_rrupt_from_idle() 判断当前 IRQ/NMI 是否打断 idle/EQS，返回上下文分类快照。 */
/*
 * 补充契约：入参无；当前 CPU IRQ 至少已关闭，函数不睡眠、不修改状态。返回 1
 * 表示第一层中断打断 idle/not-watching，返回 0 表示普通/嵌套中断或非 idle；
 * nesting 下溢等非法状态会触发 lockdep 警告并保守返回 0。
 */
static int rcu_is_cpu_rrupt_from_idle(void)
{
	long nmi_nesting = ct_nmi_nesting();

	/*
	 * Usually called from the tick; but also used from smp_function_call()
	 * for expedited grace periods. This latter can result in running from
	 * the idle task, instead of an actual IPI.
	 */
	/*
	 * 通常由 scheduler tick 调用；expedited GP 也会经 smp_function_call() 到达。
	 * 后一种情况可能直接借 idle task 上下文执行，而不一定处在真实 IPI handler 中，
	 * 所以下面的判断使用 context-tracking nesting，而不能只检查 in_interrupt()。
	 */
	lockdep_assert_irqs_disabled();

	/* Check for counter underflows */
	/* 首先检查嵌套计数是否下溢；负值表示 enter/exit 配对已经损坏。 */
	RCU_LOCKDEP_WARN(ct_nesting() < 0,
			 "RCU nesting counter underflow!");

	/* Non-idle interrupt or nested idle interrupt */
	/* 大于一表示普通中断或 idle 上的嵌套中断，不能把当前层当成 idle 边界。 */
	if (nmi_nesting > 1)
		return false;

	/*
	 * Non nested idle interrupt (interrupting section where RCU
	 * wasn't watching).
	 */
	/*
	 * 等于一表示第一层中断打断了 RCU 未 watching 的 idle 区间；这正是调用者寻找的
	 * “从 idle 被打断”情形，可以把此前 idle 视为 QS。
	 */
	if (nmi_nesting == 1)
		return true;

	/* Not in an interrupt */
	/*
	 * 零表示不在中断中，只允许 idle task 直接调用；返回当前 CPU 是否仍处于
	 * not-watching 状态，从而兼容 expedited 路径借 idle task 执行的情况。
	 */
	if (!nmi_nesting) {
		RCU_LOCKDEP_WARN(!in_task() || !is_idle_task(current),
				 "RCU nmi_nesting counter not in idle task!");
		return !rcu_is_watching_curr_cpu();
	}

	RCU_LOCKDEP_WARN(1, "RCU nmi_nesting counter underflow/zero!");

	return false;
}

#define DEFAULT_RCU_BLIMIT (IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD) ? 1000 : 10)
				// Maximum callbacks per rcu_do_batch ...
				/* 每次 rcu_do_batch() 默认最多调用的 callback 数。 */
#define DEFAULT_MAX_RCU_BLIMIT 10000 // ... even during callback flood.
/* 即使 callback 洪峰进入排空模式，单批也最多处理 10000 个。 */
static long blimit = DEFAULT_RCU_BLIMIT;
#define DEFAULT_RCU_QHIMARK 10000 // If this many pending, ignore blimit.
/* pending 达到高水位时忽略普通 blimit，优先快速排空 backlog。 */
static long qhimark = DEFAULT_RCU_QHIMARK;
#define DEFAULT_RCU_QLOMARK 100   // Once only this many pending, use blimit.
/* backlog 回落到低水位后恢复普通 blimit，形成迟滞避免频繁切换。 */
static long qlowmark = DEFAULT_RCU_QLOMARK;
#define DEFAULT_RCU_QOVLD_MULT 2
#define DEFAULT_RCU_QOVLD (DEFAULT_RCU_QOVLD_MULT * DEFAULT_RCU_QHIMARK)
static long qovld = DEFAULT_RCU_QOVLD; // If this many pending, hammer QS.
/* pending 达到 qovld 时主动强催 QS，以免 callback 内存继续无界积压。 */
static long qovld_calc = -1;	  // No pre-initialization lock acquisitions!
/*
 * -1 是启动期通配哨兵：过载阈值尚未计算时禁止为检查 callback 压力而取得 node 锁。
 */

module_param(blimit, long, 0444);
module_param(qhimark, long, 0444);
module_param(qlowmark, long, 0444);
module_param(qovld, long, 0444);

static ulong jiffies_till_first_fqs = IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD) ? 0 : ULONG_MAX;
static ulong jiffies_till_next_fqs = ULONG_MAX;
static bool rcu_kick_kthreads;
static int rcu_divisor = 7;
module_param(rcu_divisor, int, 0644);

/* Force an exit from rcu_do_batch() after 3 milliseconds. */
/*
 * rcu_do_batch() 连续执行达到三毫秒后主动让出，防止 callback 洪峰长期占用
 * softirq/kthread；该纳秒阈值控制延迟公平性，不限制单个 callback 的运行时间。
 */
static long rcu_resched_ns = 3 * NSEC_PER_MSEC;
module_param(rcu_resched_ns, long, 0644);

/*
 * How long the grace period must be before we start recruiting
 * quiescent-state help from rcu_note_context_switch().
 */
/*
 * GP 经过多少 jiffy 后开始要求 rcu_note_context_switch() 协助制造 QS。
 * ULONG_MAX 表示尚未由启动期参数归一化；最终值写入 jiffies_to_sched_qs。
 */
static ulong jiffies_till_sched_qs = ULONG_MAX;
module_param(jiffies_till_sched_qs, ulong, 0444);
static ulong jiffies_to_sched_qs; /* See adjust_jiffies_till_sched_qs(). */
/* 上一行行尾英文表示该派生值由 adjust_jiffies_till_sched_qs() 计算，参数接口只读展示。 */
module_param(jiffies_to_sched_qs, ulong, 0444); /* Display only! */

/*
 * Make sure that we give the grace-period kthread time to detect any
 * idle CPUs before taking active measures to force quiescent states.
 * However, don't go below 100 milliseconds, adjusted upwards for really
 * large systems.
 */
/* adjust_jiffies_till_sched_qs() 根据 HZ/参数重算调度器强制 QS 的 jiffies 阈值。 */
/*
 * 补充契约：入参无，由启动参数 setter/geometry 初始化调用；进程上下文，不取
 * rcu_node 锁且不睡眠。若用户显式指定则原样发布，否则根据首次/后续 FQS 周期、
 * HZ 和 CPU 数计算下限，写 jiffies_to_sched_qs；返回无直接值，无失败出口。
 */
static void adjust_jiffies_till_sched_qs(void)
{
	unsigned long j;

	/* If jiffies_till_sched_qs was specified, respect the request. */
	/*
	 * 用户显式给出阈值时原样采用；这是调试/调优契约，不能再用自动下限覆盖其选择。
	 */
	if (jiffies_till_sched_qs != ULONG_MAX) {
		WRITE_ONCE(jiffies_to_sched_qs, jiffies_till_sched_qs);
		return;
	}
	/* Otherwise, set to third fqs scan, but bound below on large system. */
	/*
	 * 未指定时取“首次 FQS 等待 + 两次后续等待”，即大约第三轮扫描时开始征召调度器
	 * QS；同时以 100ms 加大系统规模修正为下限，先给 GP kthread 足够时间发现 idle。
	 */
	j = READ_ONCE(jiffies_till_first_fqs) +
		      2 * READ_ONCE(jiffies_till_next_fqs);
	if (j < HZ / 10 + nr_cpu_ids / RCU_JIFFIES_FQS_DIV)
		j = HZ / 10 + nr_cpu_ids / RCU_JIFFIES_FQS_DIV;
	pr_info("RCU calculated value of scheduler-enlistment delay is %ld jiffies.\n", j);
	WRITE_ONCE(jiffies_to_sched_qs, j);
}

/* first_fqs 参数 setter 解析非负 jiffies，更新首次 FQS 延时；成功 0，失败负 errno。 */
/*
 * 补充契约：@val 是不可空 NUL 结尾只读字符串，@kp 借用模块参数描述符；均不转移
 * ownership。模块参数串行化 setter，可睡眠。解析失败返回 kstrtoul 的负 errno；
 * 成功返回 0，发布 jiffies_till_first_fqs 并联动重算 scheduler-enlistment 阈值。
 */
static int param_set_first_fqs_jiffies(const char *val, const struct kernel_param *kp)
{
	ulong j;
	int ret = kstrtoul(val, 0, &j);

	if (!ret) {
		WRITE_ONCE(*(ulong *)kp->arg, (j > HZ) ? HZ : j);
		adjust_jiffies_till_sched_qs();
	}
	return ret;
}

/* next_fqs 参数 setter 更新后续 FQS 周期并联动调度 QS 阈值；返回 0 或负 errno。 */
/*
 * 补充契约：@val/@kp 的借用、可空性和上下文同 first setter。失败不修改有效参数；
 * 成功写入以 jiffy 为单位的 jiffies_till_next_fqs、重算派生阈值并返回 0。
 */
static int param_set_next_fqs_jiffies(const char *val, const struct kernel_param *kp)
{
	ulong j;
	int ret = kstrtoul(val, 0, &j);

	if (!ret) {
		WRITE_ONCE(*(ulong *)kp->arg, clamp_val(j, 1, HZ));
		adjust_jiffies_till_sched_qs();
	}
	return ret;
}

static const struct kernel_param_ops first_fqs_jiffies_ops = {
	.set = param_set_first_fqs_jiffies,
	.get = param_get_ulong,
};

static const struct kernel_param_ops next_fqs_jiffies_ops = {
	.set = param_set_next_fqs_jiffies,
	.get = param_get_ulong,
};

module_param_cb(jiffies_till_first_fqs, &first_fqs_jiffies_ops, &jiffies_till_first_fqs, 0644);
module_param_cb(jiffies_till_next_fqs, &next_fqs_jiffies_ops, &jiffies_till_next_fqs, 0644);
module_param(rcu_kick_kthreads, bool, 0644);

static void force_qs_rnp(int (*f)(struct rcu_data *rdp));
static int rcu_pending(int user);

/*
 * Return the number of RCU GPs completed thus far for debug & stats.
 */
/* rcu_get_gp_seq() 返回全局 gp_seq 的无锁快照；可用于轮询，不能单独充当内存屏障。 */
/*
 * 补充契约：入参无；任何上下文可调用，不取锁、不睡眠、无副作用。返回
 * rcu_state.gp_seq 的 READ_ONCE 编码快照，包含计数与进行状态；调用者必须使用
 * rcu_seq helper 比较，不能把裸数值或本函数当成 GP 完成屏障。
 */
unsigned long rcu_get_gp_seq(void)
{
	return READ_ONCE(rcu_state.gp_seq);
}
EXPORT_SYMBOL_GPL(rcu_get_gp_seq);

/*
 * Return the number of RCU expedited batches completed thus far for
 * debug & stats.  Odd numbers mean that a batch is in progress, even
 * numbers mean idle.  The value returned will thus be roughly double
 * the cumulative batches since boot.
 */
/* rcu_exp_batches_completed() 返回 expedited GP 序号快照，供测试/轮询判断批次推进。 */
/*
 * 补充契约：入参无；无锁、不可睡眠要求、无副作用。返回 expedited_sequence 的
 * 瞬时编码值，不承诺调用返回后仍未变化，也不启动 expedited GP。
 */
unsigned long rcu_exp_batches_completed(void)
{
	return rcu_state.expedited_sequence;
}
EXPORT_SYMBOL_GPL(rcu_exp_batches_completed);

/*
 * Return the root node of the rcu_state structure.
 */
/* rcu_get_root() 返回静态 rcu_node 树根借用指针；对象贯穿系统生命周期。 */
/*
 * 补充契约：入参无；任何上下文可调用，不取锁、不睡眠。返回永不为 NULL 的静态
 * 根节点借用指针，不增加引用；调用者访问可变字段仍须遵守 root->lock/READ_ONCE
 * 协议，不得释放返回对象。
 */
static struct rcu_node *rcu_get_root(void)
{
	return &rcu_state.node[0];
}

/*
 * Send along grace-period-related data for rcutorture diagnostics.
 */
/* rcutorture_get_gp_data() 把根节点 GP flags/seq 快照写入非空输出参数，仅供测试。 */
/*
 * 补充契约：@flags/@gp_seq 都是不可空纯输出借用指针，调用者提供存储且保留
 * ownership。任意上下文可调用，不取锁、不睡眠；分别写 gp_flags 的瞬时快照和
 * 当前编码 GP 序号，两次读取不组成原子联合快照，无失败返回。
 */
void rcutorture_get_gp_data(int *flags, unsigned long *gp_seq)
{
	*flags = READ_ONCE(rcu_state.gp_flags);
	*gp_seq = rcu_seq_current(&rcu_state.gp_seq);
}
EXPORT_SYMBOL_GPL(rcutorture_get_gp_data);

/* Gather grace-period sequence numbers for rcutorture diagnostics. */
/* rcutorture_gather_gp_seqs() 打包多个 GP 序号低位，供 torture 检测停滞/不一致。 */
/*
 * 补充契约：入参无；无锁、不睡眠、无副作用。返回值把 normal、expedited 和
 * polled 序号低位压入固定字段，仅供相邻采样比较；截断意味着它不是完整 GP
 * cookie，三个 READ_ONCE 也不保证来自同一瞬间。
 */
unsigned long long rcutorture_gather_gp_seqs(void)
{
	return ((READ_ONCE(rcu_state.gp_seq) & 0xffffULL) << 40) |
	       ((READ_ONCE(rcu_state.expedited_sequence) & 0xffffffULL) << 16) |
	       (READ_ONCE(rcu_state.gp_seq_polled) & 0xffffULL);
}
EXPORT_SYMBOL_GPL(rcutorture_gather_gp_seqs);

/* Format grace-period sequence numbers for rcutorture diagnostics. */
/* rcutorture_format_gp_seqs() 将打包序号格式化到 @cp/@len，不改变 RCU 状态。 */
/*
 * 补充契约：@seqs 是 gather helper 的纯输入打包值；@cp 是不可空调用者缓冲区，
 * @len 是其字节容量，ownership 均不变。函数可在允许 snprintf 的诊断上下文调用，
 * 不修改 RCU 状态、无直接返回；空间不足时按 snprintf 规则截断并保持 NUL 终止。
 */
void rcutorture_format_gp_seqs(unsigned long long seqs, char *cp, size_t len)
{
	unsigned int egp = (seqs >> 16) & 0xffffffULL;
	unsigned int ggp = (seqs >> 40) & 0xffffULL;
	unsigned int pgp = seqs & 0xffffULL;

	snprintf(cp, len, "g%04x:e%06x:p%04x", ggp, egp, pgp);
}
EXPORT_SYMBOL_GPL(rcutorture_format_gp_seqs);

#if defined(CONFIG_NO_HZ_FULL) && (!defined(CONFIG_GENERIC_ENTRY) || !defined(CONFIG_VIRT_XFER_TO_GUEST_WORK))
/*
 * An empty function that will trigger a reschedule on
 * IRQ tail once IRQs get re-enabled on userspace/guest resume.
 */
/* late_wakeup_func() 在 IRQ work 上下文唤醒当前 CPU 以摆脱过晚 EQS；不可睡眠。 */
/*
 * 补充契约：@work 是 irq_work 框架传入的当前 CPU 静态对象借用指针，本函数不保存
 * 或释放它。硬 IRQ/irq_work 上下文不可睡眠；副作用是 set_need_resched()，无直接
 * 返回值，后续调度边界帮助 RCU 观察 QS。
 */
static void late_wakeup_func(struct irq_work *work)
{
}

static DEFINE_PER_CPU(struct irq_work, late_wakeup_work) =
	IRQ_WORK_INIT(late_wakeup_func);

/*
 * If either:
 *
 * 1) the task is about to enter in guest mode and $ARCH doesn't support KVM generic work
 * 2) the task is about to enter in user mode and $ARCH doesn't support generic entry.
 *
 * In these cases the late RCU wake ups aren't supported in the resched loops and our
 * last resort is to fire a local irq_work that will trigger a reschedule once IRQs
 * get re-enabled again.
 */
/*
 * rcu_irq_work_resched() - 无插桩路径请求当前 CPU 尽快重调度/处理 RCU 紧急 QS。
 * 无返回；只在满足 IRQ-work 安全条件时排 late_wakeup_work，避免 tracing 递归。
 *
 * 补充契约：调用者可处于 entry/IRQ 等不可插桩上下文，因此本函数既不能睡眠，也不能
 * 依赖普通 trace 路径。它只操作当前 CPU 的 rcu_data；若通用 entry 代码会自行完成
 * 重调度，或 PF_VCPU 条件不成立，则直接返回。成功排队的副作用是稍后由 irq_work
 * 产生一次本地唤醒，不代表当前调用点已经经历静止状态。
 */
noinstr void rcu_irq_work_resched(void)
{
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);

	if (IS_ENABLED(CONFIG_GENERIC_ENTRY) && !(current->flags & PF_VCPU))
		return;

	if (IS_ENABLED(CONFIG_VIRT_XFER_TO_GUEST_WORK) && (current->flags & PF_VCPU))
		return;

	instrumentation_begin();
	if (do_nocb_deferred_wakeup(rdp) && need_resched()) {
		irq_work_queue(this_cpu_ptr(&late_wakeup_work));
	}
	instrumentation_end();
}
#endif /* #if defined(CONFIG_NO_HZ_FULL) && (!defined(CONFIG_GENERIC_ENTRY) || !defined(CONFIG_VIRT_XFER_TO_GUEST_WORK)) */
/*
 * 上述实现只在 NO_HZ_FULL 且通用 entry/guest-work 不能完整处理退出唤醒时编译，
 * 用 irq_work 补上延迟 nocb 唤醒；其余配置由通用入口代码承担。
 */

#ifdef CONFIG_PROVE_RCU
/**
 * rcu_irq_exit_check_preempt - Validate that scheduling is possible
 */
/*
 * rcu_irq_exit_check_preempt() 在 IRQ 退出点检查本 CPU 是否应报告 QS/请求重调度。
 *
 * 补充契约：调用时硬中断必须关闭，且上下文跟踪应表明 CPU 尚在内核中；函数无参数、
 * 无返回，只读取当前 CPU 的 RCU 状态并执行调试断言。其副作用仅是 WARN/lockdep
 * 诊断以及在需要时设置调度请求，不取得 rcu_node 锁，也不会直接推进 GP。
 */
void rcu_irq_exit_check_preempt(void)
{
	lockdep_assert_irqs_disabled();

	RCU_LOCKDEP_WARN(ct_nesting() <= 0,
			 "RCU nesting counter underflow/zero!");
	RCU_LOCKDEP_WARN(ct_nmi_nesting() !=
			 CT_NESTING_IRQ_NONIDLE,
			 "Bad RCU  nmi_nesting counter\n");
	RCU_LOCKDEP_WARN(!rcu_is_watching_curr_cpu(),
			 "RCU in extended quiescent state!");
}
#endif /* #ifdef CONFIG_PROVE_RCU */
/* 上述 IRQ 退出一致性检查只在 PROVE_RCU 下存在，生产配置不增加运行时检查成本。 */

#ifdef CONFIG_NO_HZ_FULL
/**
 * __rcu_irq_enter_check_tick - Enable scheduler tick on CPU if RCU needs it.
 *
 * The scheduler tick is not normally enabled when CPUs enter the kernel
 * from nohz_full userspace execution.  After all, nohz_full userspace
 * execution is an RCU quiescent state and the time executing in the kernel
 * is quite short.  Except of course when it isn't.  And it is not hard to
 * cause a large system to spend tens of seconds or even minutes looping
 * in the kernel, which can cause a number of problems, include RCU CPU
 * stall warnings.
 *
 * Therefore, if a nohz_full CPU fails to report a quiescent state
 * in a timely manner, the RCU grace-period kthread sets that CPU's
 * ->rcu_urgent_qs flag with the expectation that the next interrupt or
 * exception will invoke this function, which will turn on the scheduler
 * tick, which will enable RCU to detect that CPU's quiescent states,
 * for example, due to cond_resched() calls in CONFIG_PREEMPT=n kernels.
 * The tick will be disabled once a quiescent state is reported for
 * this CPU.
 *
 * Of course, in carefully tuned systems, there might never be an
 * interrupt or exception.  In that case, the RCU grace-period kthread
 * will eventually cause one to happen.  However, in less carefully
 * controlled environments, this function allows RCU to get what it
 * needs without creating otherwise useless interruptions.
 */
/*
 * __rcu_irq_enter_check_tick() 在从 EQS 进入 IRQ 时按需要恢复 scheduler tick 以催 QS。
 *
 * 补充契约：入口位于 IRQ/exception entry，当前 CPU 已退出 EQS；若其实来自 NMI，
 * 或该 CPU 没有 urgent-QS 请求，则无动作。函数无返回；真正的副作用是对 nohz_full
 * CPU 重新启 tick，并用 rdp->rcu_forced_tick 记录所有权，供经历 QS 后成对撤销。
 */
void __rcu_irq_enter_check_tick(void)
{
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);

	// If we're here from NMI there's nothing to do.
	/*
	 * NMI 不走普通 IRQ 的 tick 恢复协议，也不能安全获取这里的 node 锁；直接返回，
	 * 后续普通中断或 GP kthread 的强制动作会继续催促。
	 */
	if (in_nmi())
		return;

	RCU_LOCKDEP_WARN(!rcu_is_watching_curr_cpu(),
			 "Illegal rcu_irq_enter_check_tick() from extended quiescent state");

	if (!tick_nohz_full_cpu(rdp->cpu) ||
	    !READ_ONCE(rdp->rcu_urgent_qs) ||
	    READ_ONCE(rdp->rcu_forced_tick)) {
		// RCU doesn't need nohz_full help from this CPU, or it is
		// already getting that help.
		/*
		 * 非 nohz_full、没有 urgent 请求，或 tick 已被 RCU 强制打开时都无需重复
		 * 操作；保持 fast path，避免每次 IRQ 都争用叶节点锁。
		 */
		return;
	}

	// We get here only when not in an extended quiescent state and
	// from interrupts (as opposed to NMIs).  Therefore, (1) RCU is
	// already watching and (2) The fact that we are in an interrupt
	// handler and that the rcu_node lock is an irq-disabled lock
	// prevents self-deadlock.  So we can safely recheck under the lock.
	// Note that the nohz_full state currently cannot change.
	/*
	 * 到达这里的是普通 IRQ 且 CPU 已离开 EQS，RCU 正在 watching。节点锁会禁用 IRQ，
	 * 当前 handler 不会被同类路径重入自锁；在锁内复查 urgent/forced 可关闭无锁检查
	 * 与并发清除之间的竞态。CPU 的 nohz_full 属性在运行期不变，无需锁内重查。
	 */
	raw_spin_lock_rcu_node(rdp->mynode);
	if (READ_ONCE(rdp->rcu_urgent_qs) && !rdp->rcu_forced_tick) {
		// A nohz_full CPU is in the kernel and RCU needs a
		// quiescent state.  Turn on the tick!
		/*
		 * CPU 正在内核态且 GP 急需 QS：记录 forced_tick 并设置 tick dependency，
		 * 让 scheduler tick 恢复，随后可通过调度/cond_resched 观察 QS。
		 */
		WRITE_ONCE(rdp->rcu_forced_tick, true);
		tick_dep_set_cpu(rdp->cpu, TICK_DEP_BIT_RCU);
	}
	raw_spin_unlock_rcu_node(rdp->mynode);
}
NOKPROBE_SYMBOL(__rcu_irq_enter_check_tick);
#endif /* CONFIG_NO_HZ_FULL */
/* 上述条件块仅在完整 NO_HZ 支持启用时编译。 */

/*
 * Check to see if any future non-offloaded RCU-related work will need
 * to be done by the current CPU, even if none need be done immediately,
 * returning 1 if so.  This function is part of the RCU implementation;
 * it is -not- an exported member of the RCU API.  This is used by
 * the idle-entry code to figure out whether it is safe to disable the
 * scheduler-clock interrupt.
 *
 * Just check whether or not this CPU has non-offloaded RCU callbacks
 * queued.
 */
/*
 * rcu_needs_cpu() 返回当前 CPU 是否因 callback 工作不能继续停 tick；瞬时查询。
 *
 * 补充契约：无参数，调用者只获得当前 CPU callback 队列在这一刻的保守快照；非空且
 * 未 offload 时返回非零，否则返回零。函数不加锁、不改变队列，也不承诺返回后队列
 * 状态不变，调用者只能把结果用于本轮 NOHZ 决策。
 */
int rcu_needs_cpu(void)
{
	return !rcu_segcblist_empty(&this_cpu_ptr(&rcu_data)->cblist) &&
		!rcu_rdp_is_offloaded(this_cpu_ptr(&rcu_data));
}

/*
 * If any sort of urgency was applied to the current CPU (for example,
 * the scheduler-clock interrupt was enabled on a nohz_full CPU) in order
 * to get to a quiescent state, disable it.
 */
/*
 * rcu_disable_urgency_upon_qs() 在确认 QS 后清 urgent/heavy-QS 与 tick 依赖状态。
 *
 * 补充契约：@rdp 必须属于正在处理的 CPU，且调用者已持有 @rdp->mynode 锁；无返回。
 * 它清除本 CPU 的紧迫标记，并且只在本函数先前拥有 forced tick 的意义上停止该 tick，
 * 从而避免误关其他子系统需要的时钟中断。该操作只撤销催促机制，不负责上报 QS。
 */
static void rcu_disable_urgency_upon_qs(struct rcu_data *rdp)
{
	raw_lockdep_assert_held_rcu_node(rdp->mynode);
	WRITE_ONCE(rdp->rcu_urgent_qs, false);
	WRITE_ONCE(rdp->rcu_need_heavy_qs, false);
	if (tick_nohz_full_cpu(rdp->cpu) && rdp->rcu_forced_tick) {
		tick_dep_clear_cpu(rdp->cpu, TICK_DEP_BIT_RCU);
		WRITE_ONCE(rdp->rcu_forced_tick, false);
	}
}

/**
 * rcu_is_watching - RCU read-side critical sections permitted on current CPU?
 *
 * Return @true if RCU is watching the running CPU and @false otherwise.
 * An @true return means that this CPU can safely enter RCU read-side
 * critical sections.
 *
 * Although calls to rcu_is_watching() from most parts of the kernel
 * will return @true, there are important exceptions.  For example, if the
 * current CPU is deep within its idle loop, in kernel entry/exit code,
 * or offline, rcu_is_watching() will return @false.
 *
 * Make notrace because it can be called by the internal functions of
 * ftrace, and making this notrace removes unnecessary recursion calls.
 */
/*
 * rcu_is_watching() 无插桩读取当前 CPU dynticks，返回 RCU 是否监视普通读侧活动。
 *
 * 补充契约：可从 ftrace 等递归敏感路径调用，因此临时关闭抢占且不得被插桩；返回值
 * 只描述当前 CPU 在这次读取时是否处于 RCU watching 状态。函数不改变 dynticks，
 * 也不提供跨迁移保证，调用者不得把 true 当作长期持有的读侧保护。
 */
notrace bool rcu_is_watching(void)
{
	bool ret;

	preempt_disable_notrace();
	ret = rcu_is_watching_curr_cpu();
	preempt_enable_notrace();
	return ret;
}
EXPORT_SYMBOL_GPL(rcu_is_watching);

/*
 * If a holdout task is actually running, request an urgent quiescent
 * state from its CPU.  This is unsynchronized, so migrations can cause
 * the request to go to the wrong CPU.  Which is OK, all that will happen
 * is that the CPU's next context switch will be a bit slower and next
 * time around this task will generate another request.
 */
/*
 * rcu_request_urgent_qs_task() 若 @t 是某 CPU 当前运行任务则请求其尽快经历/上报 QS。
 *
 * 补充契约：@t 必须是仍然有效的 task_struct；本函数无锁读取其 CPU，迁移竞争允许把
 * 请求送到旧 CPU，这是刻意接受的保守误报。无返回；成功路径设置目标 rdp 的 urgent
 * 标志并可能请求重调度，失败路径不改变任务，后续扫描会再次尝试。
 */
void rcu_request_urgent_qs_task(struct task_struct *t)
{
	int cpu;

	barrier();
	cpu = task_cpu(t);
	if (!task_curr(t))
		return; /* This task is not running on that CPU. */
	/*
	 * 行尾英文说明：若任务已不在刚采样的 CPU 上运行，就不向旧 CPU 发布请求。
	 * 迁移竞态允许本次请求落空；下一轮扫描会重新定位，最多增加一次 GP 延迟。
	 */
	smp_store_release(per_cpu_ptr(&rcu_data.rcu_urgent_qs, cpu), true);
}

static unsigned long seq_gpwrap_lag = ULONG_MAX / 4;

/**
 * rcu_set_gpwrap_lag - Set RCU GP sequence overflow lag value.
 * @lag_gps: Set overflow lag to this many grace period worth of counters
 * which is used by rcutorture to quickly force a gpwrap situation.
 * @lag_gps = 0 means we reset it back to the boot-time value.
 */
/*
 * rcu_set_gpwrap_lag() 设置 torture 制造序号环绕时的滞后 GP 数；仅测试用途。
 *
 * 补充契约：@lag_gps 是希望模拟的 GP 落后量，零表示恢复启动默认值；函数无返回，
 * 将 GP 数换算为序列计数后更新全局 seq_gpwrap_lag。调用方负责避免与非测试控制面
 * 并发修改；该设置只改变环绕判定阈值，不立即启动或完成任何 GP。
 */
void rcu_set_gpwrap_lag(unsigned long lag_gps)
{
	unsigned long lag_seq_count;

	lag_seq_count = (lag_gps == 0)
			? ULONG_MAX / 4
			: lag_gps << RCU_SEQ_CTR_SHIFT;
	WRITE_ONCE(seq_gpwrap_lag, lag_seq_count);
}
EXPORT_SYMBOL_GPL(rcu_set_gpwrap_lag);

/*
 * When trying to report a quiescent state on behalf of some other CPU,
 * it is our responsibility to check for and handle potential overflow
 * of the rcu_node ->gp_seq counter with respect to the rcu_data counters.
 * After all, the CPU might be in deep idle state, and thus executing no
 * code whatsoever.
 */
/*
 * rcu_gpnum_ovf() 检测/修正 per-CPU 与 node GP 序号环绕，入口持相应 node lock。
 *
 * 补充契约：@rnp 是 @rdp 所属叶节点，调用者必须持有其 raw spinlock；无返回。若
 * @rdp 的序号相对节点落后超过阈值，则置 gpwrap 并把 per-CPU 已知/所需序号同步到
 * 节点值。副作用是修复比较域，不表示该 CPU 已报告当前 GP 的 QS。
 */
static void rcu_gpnum_ovf(struct rcu_node *rnp, struct rcu_data *rdp)
{
	raw_lockdep_assert_held_rcu_node(rnp);
	if (ULONG_CMP_LT(rcu_seq_current(&rdp->gp_seq) + seq_gpwrap_lag,
			 rnp->gp_seq)) {
		WRITE_ONCE(rdp->gpwrap, true);
		WRITE_ONCE(rdp->gpwrap_count, READ_ONCE(rdp->gpwrap_count) + 1);
	}
	if (ULONG_CMP_LT(rdp->rcu_iw_gp_seq + ULONG_MAX / 4, rnp->gp_seq))
		rdp->rcu_iw_gp_seq = rnp->gp_seq + ULONG_MAX / 4;
}

/*
 * Snapshot the specified CPU's RCU_WATCHING counter so that we can later
 * credit them with an implicit quiescent state.  Return 1 if this CPU
 * is in dynticks idle mode, which is an extended quiescent state.
 */
/*
 * rcu_watching_snap_save() 保存 @rdp CPU dynticks 快照，供本轮 FQS 后续复查。
 *
 * 补充契约：@rdp 指向待检查 CPU 的永久 per-CPU 状态，调用者在 FQS 扫描中序列化
 * 对 watching_snap 的使用；函数把 acquire 快照写回 @rdp。若快照已在 EQS 中返回
 * 1 并修正潜在 GP 环绕，否则返回 0；它只提供隐式 QS 证据，不直接清 qsmask。
 */
static int rcu_watching_snap_save(struct rcu_data *rdp)
{
	/*
	 * Full ordering between remote CPU's post idle accesses and updater's
	 * accesses prior to current GP (and also the started GP sequence number)
	 * is enforced by rcu_seq_start() implicit barrier and even further by
	 * smp_mb__after_unlock_lock() barriers chained all the way throughout the
	 * rnp locking tree since rcu_gp_init() and up to the current leaf rnp
	 * locking.
	 *
	 * Ordering between remote CPU's pre idle accesses and post grace period
	 * updater's accesses is enforced by the below acquire semantic.
	 */
	/*
	 * 完整顺序分成两半：远端 CPU 退出 idle 之后的访问，与当前 GP 开始前的更新侧
	 * 访问，由 rcu_seq_start() 的隐含屏障以及从根到叶的
	 * smp_mb__after_unlock_lock() 锁链保证；远端进入 idle 之前的访问，与 GP 结束后
	 * 更新侧访问，则由下面读取 watching 计数的 acquire 语义保证。两部分共同确保
	 * “观察到 EQS”确实切断 GP 前读侧与 GP 后回收，不能把 acquire 当成单独完成全部
	 * RCU 屏障协议。
	 */
	rdp->watching_snap = ct_rcu_watching_cpu_acquire(rdp->cpu);
	if (rcu_watching_snap_in_eqs(rdp->watching_snap)) {
		trace_rcu_fqs(rcu_state.name, rdp->gp_seq, rdp->cpu, TPS("dti"));
		rcu_gpnum_ovf(rdp->mynode, rdp);
		return 1;
	}
	return 0;
}

#ifndef arch_irq_stat_cpu
#define arch_irq_stat_cpu(cpu) 0
#endif

/*
 * Returns positive if the specified CPU has passed through a quiescent state
 * by virtue of being in or having passed through an dynticks idle state since
 * the last call to rcu_watching_snap_save() for this same CPU, or by
 * virtue of having been offline.
 *
 * Returns negative if the specified CPU needs a force resched.
 *
 * Returns zero otherwise.
 */
/*
 * rcu_watching_snap_recheck() 比较已存快照，判断目标 CPU 是否已给出隐式 QS 证据。
 *
 * 补充契约：@rdp->watching_snap 必须由同一轮 rcu_watching_snap_save() 初始化；
 * 调用者持有相应叶 rcu_node 锁并消费返回值。正值表示可清等待位，零表示仍需等待，
 * 负值表示应向目标 CPU 强制 resched；函数还可能更新 gpwrap/紧迫性统计，但自身不
 * 上报 QS，也不释放节点锁。局部 @jtsq 是超时阈值，@rnp 是锁与 GP 状态的归属节点。
 */
static int rcu_watching_snap_recheck(struct rcu_data *rdp)
{
	unsigned long jtsq;
	int ret = 0;
	struct rcu_node *rnp = rdp->mynode;

	/*
	 * If the CPU passed through or entered a dynticks idle phase with
	 * no active irq/NMI handlers, then we can safely pretend that the CPU
	 * already acknowledged the request to pass through a quiescent
	 * state.  Either way, that CPU cannot possibly be in an RCU
	 * read-side critical section that started before the beginning
	 * of the current RCU grace period.
	 */
	/*
	 * 若 CPU 自上次快照后进入过或穿过无活动 IRQ/NMI 的 dynticks-idle，就可以代表
	 * 它确认 QS：这种 CPU 不可能仍处在当前 GP 开始前进入的 RCU 读侧临界区。
	 * 返回 1 会使 force_qs_rnp() 清除该 CPU 在叶节点 qsmask 中的等待位。
	 */
	if (rcu_watching_snap_stopped_since(rdp, rdp->watching_snap)) {
		trace_rcu_fqs(rcu_state.name, rdp->gp_seq, rdp->cpu, TPS("dti"));
		rcu_gpnum_ovf(rnp, rdp);
		return 1;
	}

	/*
	 * Complain if a CPU that is considered to be offline from RCU's
	 * perspective has not yet reported a quiescent state.  After all,
	 * the offline CPU should have reported a quiescent state during
	 * the CPU-offline process, or, failing that, by rcu_gp_init()
	 * if it ran concurrently with either the CPU going offline or the
	 * last task on a leaf rcu_node structure exiting its RCU read-side
	 * critical section while all CPUs corresponding to that structure
	 * are offline.  This added warning detects bugs in any of these
	 * code paths.
	 *
	 * The rcu_node structure's ->lock is held here, which excludes
	 * the relevant portions the CPU-hotplug code, the grace-period
	 * initialization code, and the rcu_read_unlock() code paths.
	 *
	 * For more detail, please refer to the "Hotplug CPU" section
	 * of RCU's Requirements documentation.
	 */
	/*
	 * 若 RCU 已把 CPU 视为 offline，它本应在 CPU 下线流程中上报 QS；若与 GP 初始化
	 * 或离线叶节点最后一个阻塞 reader 退出并发，也应由 rcu_gp_init() 或
	 * rcu_read_unlock() 路径补报。持有当前 rcu_node->lock 会排除这些路径的关键
	 * 更新区段，因此此处仍缺 QS 表示热插拔、GP 初始化或 reader 解锁协议存在漏洞。
	 * 详细约束见 RCU Requirements 的 “Hotplug CPU” 一节。
	 */
	if (WARN_ON_ONCE(!rcu_rdp_cpu_online(rdp))) {
		struct rcu_node *rnp1;

		pr_info("%s: grp: %d-%d level: %d ->gp_seq %ld ->completedqs %ld\n",
			__func__, rnp->grplo, rnp->grphi, rnp->level,
			(long)rnp->gp_seq, (long)rnp->completedqs);
		for (rnp1 = rnp; rnp1; rnp1 = rnp1->parent)
			pr_info("%s: %d:%d ->qsmask %#lx ->qsmaskinit %#lx ->qsmaskinitnext %#lx ->rcu_gp_init_mask %#lx\n",
				__func__, rnp1->grplo, rnp1->grphi, rnp1->qsmask, rnp1->qsmaskinit, rnp1->qsmaskinitnext, rnp1->rcu_gp_init_mask);
		pr_info("%s %d: %c online: %ld(%d) offline: %ld(%d)\n",
			__func__, rdp->cpu, ".o"[rcu_rdp_cpu_online(rdp)],
			(long)rdp->rcu_onl_gp_seq, rdp->rcu_onl_gp_state,
			(long)rdp->rcu_ofl_gp_seq, rdp->rcu_ofl_gp_state);
		return 1; /* Break things loose after complaining. */
		/*
		 * 行尾英文说明：报警并打印树状态后仍返回 1，强制清掉等待位以解除 GP 卡死；
		 * 这是诊断后的保活措施，不表示离线协议实际上正确完成。
		 */
	}

	/*
	 * A CPU running for an extended time within the kernel can
	 * delay RCU grace periods: (1) At age jiffies_to_sched_qs,
	 * set .rcu_urgent_qs, (2) At age 2*jiffies_to_sched_qs, set
	 * both .rcu_need_heavy_qs and .rcu_urgent_qs.  Note that the
	 * unsynchronized assignments to the per-CPU rcu_need_heavy_qs
	 * variable are safe because the assignments are repeated if this
	 * CPU failed to pass through a quiescent state.  This code
	 * also checks .jiffies_resched in case jiffies_to_sched_qs
	 * is set way high.
	 */
	/*
	 * CPU 长期在内核态运行会拖住 GP。到一个 jiffies_to_sched_qs 周期时先置
	 * rcu_urgent_qs；到两个周期、全局 resched 截止点或 callback 过载时，再同时置
	 * rcu_need_heavy_qs，要求更强的 QS 动作。对 per-CPU heavy 标志的无锁写可接受，
	 * 因为 CPU 若仍未经过 QS，后续 FQS 扫描会重复写入，不依赖一次性事件。
	 */
	jtsq = READ_ONCE(jiffies_to_sched_qs);
	if (!READ_ONCE(rdp->rcu_need_heavy_qs) &&
	    (time_after(jiffies, rcu_state.gp_start + jtsq * 2) ||
	     time_after(jiffies, rcu_state.jiffies_resched) ||
	     rcu_state.cbovld)) {
		WRITE_ONCE(rdp->rcu_need_heavy_qs, true);
		/* Store rcu_need_heavy_qs before rcu_urgent_qs. */
		/*
		 * 必须先发布 heavy 再以 release 写 urgent；目标 CPU 以 acquire 观察 urgent
		 * 后即可同时看到 heavy，避免只执行轻量 QS 而错过更强请求。
		 */
		smp_store_release(&rdp->rcu_urgent_qs, true);
	} else if (time_after(jiffies, rcu_state.gp_start + jtsq)) {
		WRITE_ONCE(rdp->rcu_urgent_qs, true);
	}

	/*
	 * NO_HZ_FULL CPUs can run in-kernel without rcu_sched_clock_irq!
	 * The above code handles this, but only for straight cond_resched().
	 * And some in-kernel loops check need_resched() before calling
	 * cond_resched(), which defeats the above code for CPUs that are
	 * running in-kernel with scheduling-clock interrupts disabled.
	 * So hit them over the head with the resched_cpu() hammer!
	 */
	/*
	 * NO_HZ_FULL CPU 可在关闭 scheduler tick 时长期运行内核代码。仅设置 urgent 标志
	 * 对先检查 need_resched()、再决定是否 cond_resched() 的循环无效，因此这里按
	 * 周期返回负值，让 force_qs_rnp() 通过 resched_cpu() 发送强制调度请求。
	 */
	if (tick_nohz_full_cpu(rdp->cpu) &&
	    (time_after(jiffies, READ_ONCE(rdp->last_fqs_resched) + jtsq * 3) ||
	     rcu_state.cbovld)) {
		WRITE_ONCE(rdp->rcu_urgent_qs, true);
		WRITE_ONCE(rdp->last_fqs_resched, jiffies);
		ret = -1;
	}

	/*
	 * If more than halfway to RCU CPU stall-warning time, invoke
	 * resched_cpu() more frequently to try to loosen things up a bit.
	 * Also check to see if the CPU is getting hammered with interrupts,
	 * but only once per grace period, just to keep the IPIs down to
	 * a dull roar.
	 */
	/*
	 * 超过 stall 警告时间一半后，提高 resched IPI 的频率；同时每个 GP 至多安排一次
	 * irq_work 来判断目标 CPU 是否被中断洪峰占据。两个限频条件避免救援 IPI 自身
	 * 形成新的负载风暴。
	 */
	if (time_after(jiffies, rcu_state.jiffies_resched)) {
		if (time_after(jiffies,
			       READ_ONCE(rdp->last_fqs_resched) + jtsq)) {
			WRITE_ONCE(rdp->last_fqs_resched, jiffies);
			ret = -1;
		}
		if (IS_ENABLED(CONFIG_IRQ_WORK) &&
		    !rdp->rcu_iw_pending && rdp->rcu_iw_gp_seq != rnp->gp_seq &&
		    (rnp->ffmask & rdp->grpmask)) {
			rdp->rcu_iw_pending = true;
			rdp->rcu_iw_gp_seq = rnp->gp_seq;
			irq_work_queue_on(&rdp->rcu_iw, rdp->cpu);
		}

		if (rcu_cpu_stall_cputime && rdp->snap_record.gp_seq != rdp->gp_seq) {
			int cpu = rdp->cpu;
			struct rcu_snap_record *rsrp;

			rsrp = &rdp->snap_record;
			rsrp->cputime_irq     = kcpustat_field(CPUTIME_IRQ, cpu);
			rsrp->cputime_softirq = kcpustat_field(CPUTIME_SOFTIRQ, cpu);
			rsrp->cputime_system  = kcpustat_field(CPUTIME_SYSTEM, cpu);
			rsrp->nr_hardirqs = kstat_cpu_irqs_sum(cpu) + arch_irq_stat_cpu(cpu);
			rsrp->nr_softirqs = kstat_cpu_softirqs_sum(cpu);
			rsrp->nr_csw = nr_context_switches_cpu(cpu);
			rsrp->jiffies = jiffies;
			rsrp->gp_seq = rdp->gp_seq;
		}
	}

	return ret;
}

/* Trace-event wrapper function for trace_rcu_future_grace_period.  */
/*
 * trace_rcu_this_gp() 记录 callback 为何需要某个未来 GP；只影响 trace/诊断状态。
 *
 * 补充契约：@rnp/@rdp 都是调用者持有的借用状态，@gp_seq_req 是待记录的编码序号，
 * @s 是静态 trace 标签；函数无返回、可在持 rcu_node raw lock 的原子上下文执行。
 * 它只读取节点拓扑和序号并发出 tracepoint，不取得所有权，也不改变 GP 状态。
 */
static void trace_rcu_this_gp(struct rcu_node *rnp, struct rcu_data *rdp,
			      unsigned long gp_seq_req, const char *s)
{
	trace_rcu_future_grace_period(rcu_state.name, READ_ONCE(rnp->gp_seq),
				      gp_seq_req, rnp->level,
				      rnp->grplo, rnp->grphi, s);
}

/*
 * rcu_start_this_gp - Request the start of a particular grace period
 * @rnp_start: The leaf node of the CPU from which to start.
 * @rdp: The rcu_data corresponding to the CPU from which to start.
 * @gp_seq_req: The gp_seq of the grace period to start.
 *
 * Start the specified grace period, as needed to handle newly arrived
 * callbacks.  The required future grace periods are recorded in each
 * rcu_node structure's ->gp_seq_needed field.  Returns true if there
 * is reason to awaken the grace-period kthread.
 *
 * The caller must hold the specified rcu_node structure's ->lock, which
 * is why the caller is responsible for waking the grace-period kthread.
 *
 * Returns true if the GP thread needs to be awakened else false.
 */
/*
 * rcu_start_this_gp() - 把 callback 对目标 GP 的需求沿 rcu_node 树传播到根。
 * @rnp_start 起始节点，@rdp 可用于 trace，@gp_seq_req 是所需序号。逐层持锁更新
 * gp_seq_needed，必要时唤醒 GP kthread。返回是否首次产生新请求；不等待 GP。
 */
/*
 * 补充契约：
 * - 调用链：call_rcu()/callback 推进 → rcu_start_this_gp() → GP kthread。
 * - @rnp_start：借用的起始叶/内部节点，不能为空；调用者已持其 ->lock，函数返回时
 *   仍保持该锁，不转移对象或锁 ownership。
 * - @rdp：借用的发起 CPU 状态，仅用于同步 gp_seq_needed 和 trace，不增加引用。
 * - @gp_seq_req：RCU 编码的目标 GP 序号，不是已完成数量。
 * - 上下文：入口 IRQ 已禁用且持 raw spinlock，整个函数不可睡眠。
 * - 返回 true 只表示本次在根节点首次发布了需要唤醒 GP kthread 的新工作；false
 *   包括请求已记录、目标 GP 已开始、已有 GP 会在 cleanup 接住请求，或线程未创建。
 *   无论返回值如何，函数都可能推进 node/rdp 的 gp_seq_needed。
 */
static bool rcu_start_this_gp(struct rcu_node *rnp_start, struct rcu_data *rdp,
			      unsigned long gp_seq_req)
{
	bool ret = false;
	struct rcu_node *rnp;

	/*
	 * Use funnel locking to either acquire the root rcu_node
	 * structure's lock or bail out if the need for this grace period
	 * has already been recorded -- or if that grace period has in
	 * fact already started.  If there is already a grace period in
	 * progress in a non-leaf node, no recording is needed because the
	 * end of the grace period will scan the leaf rcu_node structures.
	 * Note that rnp_start->lock must not be released.
	 */
	/*
	 * 阶段 1：漏斗式向根传播请求。逐层只持当前节点锁；若目标序号已记录、GP 已启动，
	 * 或某个非叶节点已有 GP 在运行，就提前结束。运行中 GP 的 cleanup 会重新扫描叶
	 * 节点，因此无需把同一需求继续推到根。入口 rnp_start->lock 始终由调用者持有，
	 * 本函数不能释放它。
	 *
	 * 变量地图：rnp 是当前传播节点的借用指针；ret 仅记录是否需要调用者唤醒线程。
	 */
	raw_lockdep_assert_held_rcu_node(rnp_start);
	trace_rcu_this_gp(rnp_start, rdp, gp_seq_req, TPS("Startleaf"));
	for (rnp = rnp_start; 1; rnp = rnp->parent) {
		if (rnp != rnp_start)
			raw_spin_lock_rcu_node(rnp);
		if (ULONG_CMP_GE(rnp->gp_seq_needed, gp_seq_req) ||
		    rcu_seq_started(&rnp->gp_seq, gp_seq_req) ||
		    (rnp != rnp_start &&
		     rcu_seq_state(rcu_seq_current(&rnp->gp_seq)))) {
			trace_rcu_this_gp(rnp, rdp, gp_seq_req,
					  TPS("Prestarted"));
			goto unlock_out;
		}
		WRITE_ONCE(rnp->gp_seq_needed, gp_seq_req);
		if (rcu_seq_state(rcu_seq_current(&rnp->gp_seq))) {
			/*
			 * We just marked the leaf or internal node, and a
			 * grace period is in progress, which means that
			 * rcu_gp_cleanup() will see the marking.  Bail to
			 * reduce contention.
			 */
			/*
			 * 已把需求写入当前叶/内部节点，而 GP 正在运行；该 GP 的 cleanup 会看到
			 * gp_seq_needed。此处提前退出可减少上层锁竞争，又不会丢失下一轮 GP 请求。
			 */
			trace_rcu_this_gp(rnp_start, rdp, gp_seq_req,
					  TPS("Startedleaf"));
			goto unlock_out;
		}
		if (rnp != rnp_start && rnp->parent != NULL)
			raw_spin_unlock_rcu_node(rnp);
		if (!rnp->parent)
			break;  /* At root, and perhaps also leaf. */
		/* 行尾英文说明：循环到达根；单节点树中该节点也同时是叶节点。 */
	}

	/* If GP already in progress, just leave, otherwise start one. */
	/*
	 * 阶段 2：只有到达根且当前没有 GP 时才发布 INIT 请求。若 GP 已在运行，叶节点
	 * 标记足以让 cleanup 决定是否接续下一轮，不重复唤醒线程。
	 */
	if (rcu_gp_in_progress()) {
		trace_rcu_this_gp(rnp, rdp, gp_seq_req, TPS("Startedleafroot"));
		goto unlock_out;
	}
	trace_rcu_this_gp(rnp, rdp, gp_seq_req, TPS("Startedroot"));
	WRITE_ONCE(rcu_state.gp_flags, rcu_state.gp_flags | RCU_GP_FLAG_INIT);
	WRITE_ONCE(rcu_state.gp_req_activity, jiffies);
	if (!READ_ONCE(rcu_state.gp_kthread)) {
		trace_rcu_this_gp(rnp, rdp, gp_seq_req, TPS("NoGPkthread"));
		goto unlock_out;
	}
	trace_rcu_grace_period(rcu_state.name, data_race(rcu_state.gp_seq), TPS("newreq"));
	ret = true;  /* Caller must wake GP kthread. */
	/* 行尾英文说明：true 把实际 wakeup 责任留给调用者，以便其先释放 rnp_start->lock。 */
unlock_out:
	/* Push furthest requested GP to leaf node and rcu_data structure. */
	/*
	 * 阶段 3：把传播途中观察到的最远请求回写到起始节点和 per-CPU 缓存，使后续
	 * unlocked 快速路径无需重新向根取锁。最后只释放本函数额外取得的上层锁。
	 */
	if (ULONG_CMP_LT(gp_seq_req, rnp->gp_seq_needed)) {
		WRITE_ONCE(rnp_start->gp_seq_needed, rnp->gp_seq_needed);
		WRITE_ONCE(rdp->gp_seq_needed, rnp->gp_seq_needed);
	}
	if (rnp != rnp_start)
		raw_spin_unlock_rcu_node(rnp);
	return ret;
}

/*
 * Clean up any old requests for the just-ended grace period.  Also return
 * whether any additional grace periods have been requested.
 */
/*
 * rcu_future_gp_cleanup() 在 GP 完成后清已满足 future-GP 请求，返回是否仍需下一轮。
 *
 * 补充契约：@rnp 是 cleanup 正在处理且由调用者锁住的节点；返回 true 表示
 * gp_seq_needed 仍领先于已完成 gp_seq，GP kthread 应接续下一轮。false 路径会把
 * needed 收拢到当前值以规避环绕误判。函数不唤醒线程，@rdp 仅取当前 CPU 用于 trace。
 */
static bool rcu_future_gp_cleanup(struct rcu_node *rnp)
{
	bool needmore;
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);

	needmore = ULONG_CMP_LT(rnp->gp_seq, rnp->gp_seq_needed);
	if (!needmore)
		rnp->gp_seq_needed = rnp->gp_seq; /* Avoid counter wrap. */
	/*
	 * 行尾英文说明：没有未来请求时把 needed 收拢到当前序号，避免长期保留旧值在
	 * unsigned long 环绕后被误判为“更远的请求”。
	 */
	trace_rcu_this_gp(rnp, rdp, rnp->gp_seq,
			  needmore ? TPS("CleanupMore") : TPS("Cleanup"));
	return needmore;
}

/*
 * Awaken the grace-period kthread.  Don't do a self-awaken (unless in an
 * interrupt or softirq handler, in which case we just might immediately
 * sleep upon return, resulting in a grace-period hang), and don't bother
 * awakening when there is nothing for the grace-period kthread to do
 * (as in several CPUs raced to awaken, we lost), and finally don't try
 * to awaken a kthread that has not yet been created.  If all those checks
 * are passed, track some debug information and awaken.
 *
 * So why do the self-wakeup when in an interrupt or softirq handler
 * in the grace-period kthread's context?  Because the kthread might have
 * been interrupted just as it was going to sleep, and just after the final
 * pre-sleep check of the awaken condition.  In this case, a wakeup really
 * is required, and is therefore supplied.
 */
/*
 * rcu_gp_kthread_wake() 在 GP 请求待处理且线程可运行时唤醒全局 GP kthread。
 *
 * 补充契约：无参数、无返回，可从 IRQ/softirq/持锁路径调用且不能睡眠。若线程尚未
 * 创建、gp_flags 已被竞争者消费，或进程上下文正由 GP 线程自唤醒，则无动作；中断
 * GP 线程临睡窗口时允许自唤醒。成功路径记录诊断时间/序号并唤醒 swait 队列。
 */
static void rcu_gp_kthread_wake(void)
{
	struct task_struct *t = READ_ONCE(rcu_state.gp_kthread);

	if ((current == t && !in_hardirq() && !in_serving_softirq()) ||
	    !READ_ONCE(rcu_state.gp_flags) || !t)
		return;
	WRITE_ONCE(rcu_state.gp_wake_time, jiffies);
	WRITE_ONCE(rcu_state.gp_wake_seq, READ_ONCE(rcu_state.gp_seq));
	swake_up_one(&rcu_state.gp_wq);
}

/*
 * If there is room, assign a ->gp_seq number to any callbacks on this
 * CPU that have not already been assigned.  Also accelerate any callbacks
 * that were previously assigned a ->gp_seq number that has since proven
 * to be too conservative, which can happen if callbacks get assigned a
 * ->gp_seq number while RCU is idle, but with reference to a non-root
 * rcu_node structure.  This function is idempotent, so it does not hurt
 * to call it repeatedly.  Returns an flag saying that we should awaken
 * the RCU grace-period kthread.
 *
 * The caller must hold rnp->lock with interrupts disabled.
 */
/*
 * rcu_accelerate_cbs() - 把尚未分配代际的新 callback 推入最早可满足的等待段。
 * 入口持 rnp lock；更新 segcblist 并在需要时请求 GP。返回是否发生加速。
 */
/*
 * 修正说明：上一段最后一句不准确。返回值并不表示 callback 是否被重新分段；
 * true 仅表示 rcu_start_this_gp() 新发布了需要唤醒 GP kthread 的工作。@rnp 是借用
 * 的所属叶节点，入口持其 ->lock 且 IRQ 已禁用；@rdp 是借用的 per-CPU callback
 * 状态，cblist 也必须受当前上下文保护。函数不可睡眠，不转移 callback ownership。
 */
static bool rcu_accelerate_cbs(struct rcu_node *rnp, struct rcu_data *rdp)
{
	unsigned long gp_seq_req;
	bool ret = false;

	rcu_lockdep_assert_cblist_protected(rdp);
	raw_lockdep_assert_held_rcu_node(rnp);

	/* If no pending (not yet ready to invoke) callbacks, nothing to do. */
	/* 没有尚待 GP 的 callback 时直接返回 false，也不会创建或唤醒新的 GP。 */
	if (!rcu_segcblist_pend_cbs(&rdp->cblist))
		return false;

	trace_rcu_segcb_stats(&rdp->cblist, TPS("SegCbPreAcc"));

	/*
	 * Callbacks are often registered with incomplete grace-period
	 * information.  Something about the fact that getting exact
	 * information requires acquiring a global lock...  RCU therefore
	 * makes a conservative estimate of the grace period number at which
	 * a given callback will become ready to invoke.	The following
	 * code checks this estimate and improves it when possible, thus
	 * accelerating callback invocation to an earlier grace-period
	 * number.
	 */
	/*
	 * 阶段 1：callback 注册时为避免获取全局锁，只能保守估算完成所需 GP。这里在持
	 * 叶节点锁后取得更准确的全局序号，并让 segcblist 把 callback 前移到尽可能早的
	 * 等待段；若前移产生新的 GP 需求，再沿 rcu_node 树发布请求。
	 *
	 * 变量地图：gp_seq_req 是本次序列快照；ret 是“需唤醒 GP 线程”，不是“已前移”。
	 */
	gp_seq_req = rcu_seq_snap(&rcu_state.gp_seq);
	if (rcu_segcblist_accelerate(&rdp->cblist, gp_seq_req))
		ret = rcu_start_this_gp(rnp, rdp, gp_seq_req);

	/* Trace depending on how much we were able to accelerate. */
	/*
	 * 阶段 2：根据 WAIT 之后是否为空记录加速结果。trace 只观测状态，不改变 callback
	 * 所有权；函数最终把 GP 线程唤醒责任返回给外层解锁后的路径。
	 */
	if (rcu_segcblist_restempty(&rdp->cblist, RCU_WAIT_TAIL))
		trace_rcu_grace_period(rcu_state.name, gp_seq_req, TPS("AccWaitCB"));
	else
		trace_rcu_grace_period(rcu_state.name, gp_seq_req, TPS("AccReadyCB"));

	trace_rcu_segcb_stats(&rdp->cblist, TPS("SegCbPostAcc"));

	return ret;
}

/*
 * Similar to rcu_accelerate_cbs(), but does not require that the leaf
 * rcu_node structure's ->lock be held.  It consults the cached value
 * of ->gp_seq_needed in the rcu_data structure, and if that indicates
 * that a new grace-period request be made, invokes rcu_accelerate_cbs()
 * while holding the leaf rcu_node structure's ->lock.
 */
/* unlocked 包装负责取得 rnp lock、加速 @rdp callbacks 并按需唤醒 GP 线程。 */
static void rcu_accelerate_cbs_unlocked(struct rcu_node *rnp,
					struct rcu_data *rdp)
{
	unsigned long c;
	bool needwake;

	rcu_lockdep_assert_cblist_protected(rdp);
	c = rcu_seq_snap(&rcu_state.gp_seq);
	if (!READ_ONCE(rdp->gpwrap) && ULONG_CMP_GE(rdp->gp_seq_needed, c)) {
		/* Old request still live, so mark recent callbacks. */
	/*
	 * per-CPU 缓存表明旧 GP 请求仍覆盖当前快照，只需在本地 segcblist 标记新 callback；
	 * 无需获取 rnp->lock 或重复向根发布请求。
	 */
		(void)rcu_segcblist_accelerate(&rdp->cblist, c);
		return;
	}
	raw_spin_lock_rcu_node(rnp); /* irqs already disabled. */
	/* 行尾英文说明：调用者此前已经关闭 IRQ，此处只取得叶节点锁。 */
	needwake = rcu_accelerate_cbs(rnp, rdp);
	raw_spin_unlock_rcu_node(rnp); /* irqs remain disabled. */
	/* 行尾英文说明：解锁不恢复 IRQ，维持调用者的中断状态 ownership。 */
	if (needwake)
		rcu_gp_kthread_wake();
}

/*
 * Move any callbacks whose grace period has completed to the
 * RCU_DONE_TAIL sublist, then compact the remaining sublists and
 * assign ->gp_seq numbers to any callbacks in the RCU_NEXT_TAIL
 * sublist.  This function is idempotent, so it does not hurt to
 * invoke it repeatedly.  As long as it is not invoked -too- often...
 * Returns true if the RCU grace-period kthread needs to be awakened.
 *
 * The caller must hold rnp->lock with interrupts disabled.
 */
/*
 * rcu_advance_cbs() - 根据最新 gp_seq 把已过宽限期的 callback 推到 DONE 段。
 * 入口持 rnp lock；同时加速新 callback。返回是否出现可调用 callback。
 */
/*
 * 修正说明：上一段返回语义不准确。函数先把 gp_seq 已完成的 callback 移入 DONE，
 * 再调用 rcu_accelerate_cbs() 分类剩余 callback；返回 true 仅表示需要唤醒 GP
 * kthread，不表示 DONE 中一定出现 callback。@rnp/@rdp 均为借用指针，入口持
 * rnp->lock 且 IRQ 已禁用；函数不可睡眠，也不调用 callback。
 */
static bool rcu_advance_cbs(struct rcu_node *rnp, struct rcu_data *rdp)
{
	rcu_lockdep_assert_cblist_protected(rdp);
	raw_lockdep_assert_held_rcu_node(rnp);

	/* If no pending (not yet ready to invoke) callbacks, nothing to do. */
	/* 没有等待 GP 的 callback 时，DONE/WAIT/NEXT 各段均无需推进，返回 false。 */
	if (!rcu_segcblist_pend_cbs(&rdp->cblist))
		return false;

	/*
	 * Find all callbacks whose ->gp_seq numbers indicate that they
	 * are ready to invoke, and put them into the RCU_DONE_TAIL sublist.
	 */
	/*
	 * 阶段 1：以叶节点已发布的 gp_seq 为完成边界，把满足条件的 callback 移到 DONE。
	 * 这里只改变分段元数据；callback 仍由 rdp->cblist 持有，稍后由 rcu_do_batch()
	 * 在不持 rnp->lock 时真正调用。
	 */
	rcu_segcblist_advance(&rdp->cblist, rnp->gp_seq);

	/* Classify any remaining callbacks. */
	/* 阶段 2：为剩余 callback 分配尽可能早的目标 GP，并向上传播新的 GP 请求。 */
	return rcu_accelerate_cbs(rnp, rdp);
}

/*
 * Move and classify callbacks, but only if doing so won't require
 * that the RCU grace-period kthread be awakened.
 */
/* rcu_advance_cbs_nowake() 是不触发 core 唤醒的锁包装，供已有执行上下文批量推进。 */
static void __maybe_unused rcu_advance_cbs_nowake(struct rcu_node *rnp,
						  struct rcu_data *rdp)
{
	rcu_lockdep_assert_cblist_protected(rdp);
	if (!rcu_seq_state(rcu_seq_current(&rnp->gp_seq)) || !raw_spin_trylock_rcu_node(rnp))
		return;
	// The grace period cannot end while we hold the rcu_node lock.
	/*
	 * 持有叶节点锁期间，QS 上报和 GP cleanup 都不能完成该节点，因此二次读取 gp_seq
	 * 不会跨越结束边界；trylock 成功后可安全推进 callback。
	 */
	if (rcu_seq_state(rcu_seq_current(&rnp->gp_seq)))
		WARN_ON_ONCE(rcu_advance_cbs(rnp, rdp));
	raw_spin_unlock_rcu_node(rnp);
}

/*
 * In CONFIG_RCU_STRICT_GRACE_PERIOD=y kernels, attempt to generate a
 * quiescent state.  This is intended to be invoked when the CPU notices
 * a new grace period.
 */
/* strict GP 配置下在边界主动制造/检查 QS，非严格配置编译为低成本空操作。 */
static void rcu_strict_gp_check_qs(void)
{
	if (IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD)) {
		rcu_read_lock();
		rcu_read_unlock();
	}
}

/*
 * Update CPU-local rcu_data state to record the beginnings and ends of
 * grace periods.  The caller must hold the ->lock of the leaf rcu_node
 * structure corresponding to the current CPU, and must have irqs disabled.
 * Returns true if the grace-period kthread needs to be awakened.
 */
/*
 * __note_gp_changes() - 在 rnp lock 下让 per-CPU rdp 追上 node/global GP 状态。
 * 识别新 GP、已结束 GP、QS 需求和 callback 推进；返回是否需要唤醒 RCU core。
 */
/*
 * 修正说明：上一段的“唤醒 RCU core”应为“唤醒 GP kthread”。@rnp 是借用的当前
 * CPU 叶节点，入口持其 ->lock 且 IRQ 已禁用；@rdp 是借用的 per-CPU 状态。函数
 * 不睡眠，不转移对象所有权。返回 true 表示 callback 加速产生了新的 GP 线程工作；
 * false 还包括无变化、只记录 QS 需求或请求已经存在。副作用是同步 rdp 的 GP 序号、
 * gpwrap、core_needs_qs/cpu_no_qs，并可能推进其 callback 分段。
 */
static bool __note_gp_changes(struct rcu_node *rnp, struct rcu_data *rdp)
{
	bool ret = false;
	bool need_qs;
	const bool offloaded = rcu_rdp_is_offloaded(rdp);

	raw_lockdep_assert_held_rcu_node(rnp);

	if (rdp->gp_seq == rnp->gp_seq)
		return false; /* Nothing to do. */
		/* 行尾英文说明：per-CPU 与叶节点序号一致，本次无需同步任何 GP 状态。 */

	/* Handle the ends of any preceding grace periods first. */
	/*
	 * 阶段 1：先消费“旧 GP 已结束”。非 offloaded CPU 可把已满足 callback 推到 DONE；
	 * offloaded CPU 的 callback 由 nocb 路径负责，本函数只同步 GP/QS 状态。
	 */
	if (rcu_seq_completed_gp(rdp->gp_seq, rnp->gp_seq) ||
	    unlikely(rdp->gpwrap)) {
		if (!offloaded)
			ret = rcu_advance_cbs(rnp, rdp); /* Advance CBs. */
			/* 行尾英文说明：推进本地 callback 分段，ret 仍表示是否需唤醒 GP 线程。 */
		rdp->core_needs_qs = false;
		trace_rcu_grace_period(rcu_state.name, rdp->gp_seq, TPS("cpuend"));
	} else {
		if (!offloaded)
			ret = rcu_accelerate_cbs(rnp, rdp); /* Recent CBs. */
			/* 行尾英文说明：没有完成边界时，仅重新分类最近注册的 callback。 */
		if (rdp->core_needs_qs)
			rdp->core_needs_qs = !!(rnp->qsmask & rdp->grpmask);
	}

	/* Now handle the beginnings of any new-to-this-CPU grace periods. */
	/* 阶段 2：再消费“对本 CPU 而言的新 GP”，建立本轮是否需要本 CPU QS 的状态。 */
	if (rcu_seq_new_gp(rdp->gp_seq, rnp->gp_seq) ||
	    unlikely(rdp->gpwrap)) {
		/*
		 * If the current grace period is waiting for this CPU,
		 * set up to detect a quiescent state, otherwise don't
		 * go looking for one.
		 */
		/*
		 * 只有叶节点 qsmask 仍包含本 CPU 的 grpmask，才设置 cpu_no_qs/core_needs_qs。
		 * 若位已清除，说明 QS 已被其他路径代报，继续寻找会把旧需求带入新状态。
		 */
		trace_rcu_grace_period(rcu_state.name, rnp->gp_seq, TPS("cpustart"));
		need_qs = !!(rnp->qsmask & rdp->grpmask);
		rdp->cpu_no_qs.b.norm = need_qs;
		rdp->core_needs_qs = need_qs;
		zero_cpu_stall_ticks(rdp);
	}
	rdp->gp_seq = rnp->gp_seq;  /* Remember new grace-period state. */
	/* 行尾英文说明：最后发布本 CPU 已同步到的新 GP 序号，供无锁快速检查使用。 */
	if (ULONG_CMP_LT(rdp->gp_seq_needed, rnp->gp_seq_needed) || rdp->gpwrap)
		WRITE_ONCE(rdp->gp_seq_needed, rnp->gp_seq_needed);
	if (IS_ENABLED(CONFIG_PROVE_RCU) && rdp->gpwrap)
		WRITE_ONCE(rdp->last_sched_clock, jiffies);
	WRITE_ONCE(rdp->gpwrap, false);
	rcu_gpnum_ovf(rnp, rdp);
	return ret;
}

/*
 * note_gp_changes() 取得所属叶 rnp lock，让指定 CPU 状态追上最新 GP。
 *
 * 补充契约：@rdp 是借用的 per-CPU 状态；函数自行关闭并恢复本地 IRQ，以 trylock
 * 方式避免在 core 路径自旋。无返回；锁竞争或快照未变时延后处理，成功时调用
 * __note_gp_changes()，解锁后执行 strict-QS 检查，并按其返回值唤醒 GP kthread。
 */
static void note_gp_changes(struct rcu_data *rdp)
{
	unsigned long flags;
	bool needwake;
	struct rcu_node *rnp;

	local_irq_save(flags);
	rnp = rdp->mynode;
	if ((rdp->gp_seq == rcu_seq_current(&rnp->gp_seq) &&
	     !unlikely(READ_ONCE(rdp->gpwrap))) || /* w/out lock. */
	    /* 行尾英文说明：这里只做无锁快照，命中时表示暂未观察到代际变化。 */
	    !raw_spin_trylock_rcu_node(rnp)) { /* irqs already off, so later. */
		/* 行尾英文说明：IRQ 已关闭；trylock 失败就延后到下一次 core，而不是自旋。 */
		local_irq_restore(flags);
		return;
	}
	needwake = __note_gp_changes(rnp, rdp);
	raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
	rcu_strict_gp_check_qs();
	if (needwake)
		rcu_gp_kthread_wake();
}

static atomic_t *rcu_gp_slow_suppress;

/* Register a counter to suppress debugging grace-period delays. */
/*
 * rcu_gp_slow_register() 安装 torture 的 GP 抑制计数器；调用者保证对象长期存活。
 * @rgssp 不得为空且在 unregister 之前必须有效；无返回。接口只发布借用指针，不接管
 * 其内存，重复注册会告警；测试控制面负责与 unregister 串行化。
 */
void rcu_gp_slow_register(atomic_t *rgssp)
{
	WARN_ON_ONCE(rcu_gp_slow_suppress);

	WRITE_ONCE(rcu_gp_slow_suppress, rgssp);
}
EXPORT_SYMBOL_GPL(rcu_gp_slow_register);

/* Unregister a counter, with NULL for not caring which. */
/*
 * rcu_gp_slow_unregister() 移除同一计数器；@rgssp 可为 NULL 表示不校验身份。
 * 无返回；传入非当前对象会告警，随后仍清全局借用指针。调用者必须用外部测试协议
 * 保证清除与对象释放之间的并发安全，WRITE_ONCE 本身不是等待读者退出的屏障。
 */
void rcu_gp_slow_unregister(atomic_t *rgssp)
{
	WARN_ON_ONCE(rgssp && rgssp != rcu_gp_slow_suppress && rcu_gp_slow_suppress != NULL);

	WRITE_ONCE(rcu_gp_slow_suppress, NULL);
}
EXPORT_SYMBOL_GPL(rcu_gp_slow_unregister);

/*
 * rcu_gp_slow_is_suppressed() 无锁查询测试计数器是否要求跳过注入延时。
 * 无参数；返回 true 仅表示本次快照中已注册且计数非零。函数不修改计数器，指针
 * 生命周期由 register/unregister 的外部协议保证。
 */
static bool rcu_gp_slow_is_suppressed(void)
{
	atomic_t *rgssp = READ_ONCE(rcu_gp_slow_suppress);

	return rgssp && atomic_read(rgssp);
}

/*
 * rcu_gp_slow() 按调试参数延迟 GP 阶段，抑制开启或 @delay<=0 时无操作。
 * 必须从 GP kthread 等可睡眠上下文调用；无返回。命中稀疏注入条件时主动睡眠
 * @delay jiffies，其副作用只用于放大竞态，不参与正常 GP 正确性。
 */
static void rcu_gp_slow(int delay)
{
	if (!rcu_gp_slow_is_suppressed() && delay > 0 &&
	    !(rcu_seq_ctr(rcu_state.gp_seq) % (rcu_num_nodes * PER_RCU_NODE_PERIOD * delay)))
		schedule_timeout_idle(delay);
}

static unsigned long sleep_duration;

/* Allow rcutorture to stall the grace-period kthread. */
/*
 * rcu_gp_set_torture_wait() 设置下一次 GP torture 额外等待时长；测试接口。
 * @duration 必须为正且配置启用，否则忽略；无返回。WRITE_ONCE 只发布一次性请求，
 * 后续 rcu_gp_torture_wait() 用 xchg 消费，新的写入可能覆盖尚未消费的旧值。
 */
void rcu_gp_set_torture_wait(int duration)
{
	if (IS_ENABLED(CONFIG_RCU_TORTURE_TEST) && duration > 0)
		WRITE_ONCE(sleep_duration, duration);
}
EXPORT_SYMBOL_GPL(rcu_gp_set_torture_wait);

/* Actually implement the aforementioned wait. */
/*
 * rcu_gp_torture_wait() 在 GP 线程可睡眠上下文消费已配置测试等待。
 * 无参数、无返回；未启用 torture 或没有待消费时直接返回。xchg 同时取得并清零
 * sleep_duration，保证一次请求至多执行一次；实际副作用是打印并睡眠若干 jiffies。
 */
static void rcu_gp_torture_wait(void)
{
	unsigned long duration;

	if (!IS_ENABLED(CONFIG_RCU_TORTURE_TEST))
		return;
	duration = xchg(&sleep_duration, 0UL);
	if (duration > 0) {
		pr_alert("%s: Waiting %lu jiffies\n", __func__, duration);
		schedule_timeout_idle(duration);
		pr_alert("%s: Wait complete\n", __func__);
	}
}

/*
 * Handler for on_each_cpu() to invoke the target CPU's RCU core
 * processing.
 */
/*
 * rcu_strict_gp_boundary() 是 on_each_cpu() 回调，在严格 GP 边界触发本 CPU RCU core。
 * @unused 无语义且不转移所有权；无返回。调用上下文由 SMP call 机制提供，函数自身
 * 不等待 GP，只让每个在线 CPU 都经过一次可观察的 core 处理点。
 */
static void rcu_strict_gp_boundary(void *unused)
{
	invoke_rcu_core();
}

// Make the polled API aware of the beginning of a grace period.
/*
 * rcu_poll_gp_seq_start() 记录轮询请求所对应的辅助 GP 序号快照。
 * @snap 是调用者拥有的非空输出槽；调度器启动后调用者必须持根 rcu_node 锁。
 * 无返回；必要时开始 gp_seq_polled，再写出当前编码状态。它只维护 poll 记账，
 * 不启动正常或 expedited GP。
 */
static void rcu_poll_gp_seq_start(unsigned long *snap)
{
	struct rcu_node *rnp = rcu_get_root();

	if (rcu_scheduler_active != RCU_SCHEDULER_INACTIVE)
		raw_lockdep_assert_held_rcu_node(rnp);

	// If RCU was idle, note beginning of GP.
	/* 首次观察到 poll 状态空闲时发布开始态，建立本次轮询 API 的观察区间。 */
	if (!rcu_seq_state(rcu_state.gp_seq_polled))
		rcu_seq_start(&rcu_state.gp_seq_polled);

	// Either way, record current state.
	/* 无论是新开始还是复用在途 GP，都把当前编码序号写入调用者提供的输出快照。 */
	*snap = rcu_state.gp_seq_polled;
}

// Make the polled API aware of the end of a grace period.
/*
 * rcu_poll_gp_seq_end() 在等待完成后闭合辅助 poll GP 序号。
 * @snap 必须是 start 阶段写入的非空存储，且调用者遵守同样的根节点锁约束；无返回。
 * 快照仍匹配时结束全局 poll 序列并清 normal/exp 快照，否则只清调用者的陈旧值，
 * 防止序号环绕造成假完成。
 */
static void rcu_poll_gp_seq_end(unsigned long *snap)
{
	struct rcu_node *rnp = rcu_get_root();

	if (rcu_scheduler_active != RCU_SCHEDULER_INACTIVE)
		raw_lockdep_assert_held_rcu_node(rnp);

	// If the previously noted GP is still in effect, record the
	// end of that GP.  Either way, zero counter to avoid counter-wrap
	// problems.
	/*
	 * 若调用者快照仍对应当前 poll GP，就发布结束态；无论是否匹配都清辅助快照，
	 * 防止长期累积/序号环绕后把陈旧状态误认为当前 GP。
	 */
	if (*snap && *snap == rcu_state.gp_seq_polled) {
		rcu_seq_end(&rcu_state.gp_seq_polled);
		rcu_state.gp_seq_polled_snap = 0;
		rcu_state.gp_seq_polled_exp_snap = 0;
	} else {
		*snap = 0;
	}
}

// Make the polled API aware of the beginning of a grace period, but
// where caller does not hold the root rcu_node structure's lock.
/*
 * rcu_poll_gp_seq_start_unlocked() 为无外部锁调用者保存 poll 快照。
 * @snap 是非空输出槽；初始化完成后要求进程上下文 IRQ 可用，并在内部 irqsave
 * 获取根节点锁。无返回；早期启动阶段省略尚未可用的锁，但仍执行相同记账。
 */
static void rcu_poll_gp_seq_start_unlocked(unsigned long *snap)
{
	unsigned long flags;
	struct rcu_node *rnp = rcu_get_root();

	if (rcu_init_invoked()) {
		if (rcu_scheduler_active != RCU_SCHEDULER_INACTIVE)
			lockdep_assert_irqs_enabled();
		raw_spin_lock_irqsave_rcu_node(rnp, flags);
	}
	rcu_poll_gp_seq_start(snap);
	if (rcu_init_invoked())
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
}

// Make the polled API aware of the end of a grace period, but where
// caller does not hold the root rcu_node structure's lock.
/*
 * rcu_poll_gp_seq_end_unlocked() 为无外部锁调用者闭合 poll 快照。
 * @snap 必须来自对应 start；锁、IRQ 和早期启动约束与 unlocked start 相同。
 * 无返回，内部序列化后调用 locked helper，副作用仅限 poll 辅助状态。
 */
static void rcu_poll_gp_seq_end_unlocked(unsigned long *snap)
{
	unsigned long flags;
	struct rcu_node *rnp = rcu_get_root();

	if (rcu_init_invoked()) {
		if (rcu_scheduler_active != RCU_SCHEDULER_INACTIVE)
			lockdep_assert_irqs_enabled();
		raw_spin_lock_irqsave_rcu_node(rnp, flags);
	}
	rcu_poll_gp_seq_end(snap);
	if (rcu_init_invoked())
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
}

/*
 * There is a single llist, which is used for handling
 * synchronize_rcu() users' enqueued rcu_synchronize nodes.
 * Within this llist, there are two tail pointers:
 *
 * wait tail: Tracks the set of nodes, which need to
 *            wait for the current GP to complete.
 * done tail: Tracks the set of nodes, for which grace
 *            period has elapsed. These nodes processing
 *            will be done as part of the cleanup work
 *            execution by a kworker.
 *
 * At every grace period init, a new wait node is added
 * to the llist. This wait node is used as wait tail
 * for this new grace period. Given that there are a fixed
 * number of wait nodes, if all wait nodes are in use
 * (which can happen when kworker callback processing
 * is delayed) and additional grace period is requested.
 * This means, a system is slow in processing callbacks.
 *
 * TODO: If a slow processing is detected, a first node
 * in the llist should be used as a wait-tail for this
 * grace period, therefore users which should wait due
 * to a slow process are handled by _this_ grace period
 * and not next.
 *
 * Below is an illustration of how the done and wait
 * tail pointers move from one set of rcu_synchronize nodes
 * to the other, as grace periods start and finish and
 * nodes are processed by kworker.
 *
 *
 * a. Initial llist callbacks list:
 *
 * +----------+           +--------+          +-------+
 * |          |           |        |          |       |
 * |   head   |---------> |   cb2  |--------->| cb1   |
 * |          |           |        |          |       |
 * +----------+           +--------+          +-------+
 *
 *
 *
 * b. New GP1 Start:
 *
 *                    WAIT TAIL
 *                      |
 *                      |
 *                      v
 * +----------+     +--------+      +--------+        +-------+
 * |          |     |        |      |        |        |       |
 * |   head   ------> wait   |------>   cb2  |------> |  cb1  |
 * |          |     | head1  |      |        |        |       |
 * +----------+     +--------+      +--------+        +-------+
 *
 *
 *
 * c. GP completion:
 *
 * WAIT_TAIL == DONE_TAIL
 *
 *                   DONE TAIL
 *                     |
 *                     |
 *                     v
 * +----------+     +--------+      +--------+        +-------+
 * |          |     |        |      |        |        |       |
 * |   head   ------> wait   |------>   cb2  |------> |  cb1  |
 * |          |     | head1  |      |        |        |       |
 * +----------+     +--------+      +--------+        +-------+
 *
 *
 *
 * d. New callbacks and GP2 start:
 *
 *                    WAIT TAIL                          DONE TAIL
 *                      |                                 |
 *                      |                                 |
 *                      v                                 v
 * +----------+     +------+    +------+    +------+    +-----+    +-----+    +-----+
 * |          |     |      |    |      |    |      |    |     |    |     |    |     |
 * |   head   ------> wait |--->|  cb4 |--->| cb3  |--->|wait |--->| cb2 |--->| cb1 |
 * |          |     | head2|    |      |    |      |    |head1|    |     |    |     |
 * +----------+     +------+    +------+    +------+    +-----+    +-----+    +-----+
 *
 *
 *
 * e. GP2 completion:
 *
 * WAIT_TAIL == DONE_TAIL
 *                   DONE TAIL
 *                      |
 *                      |
 *                      v
 * +----------+     +------+    +------+    +------+    +-----+    +-----+    +-----+
 * |          |     |      |    |      |    |      |    |     |    |     |    |     |
 * |   head   ------> wait |--->|  cb4 |--->| cb3  |--->|wait |--->| cb2 |--->| cb1 |
 * |          |     | head2|    |      |    |      |    |head1|    |     |    |     |
 * +----------+     +------+    +------+    +------+    +-----+    +-----+    +-----+
 *
 *
 * While the llist state transitions from d to e, a kworker
 * can start executing rcu_sr_normal_gp_cleanup_work() and
 * can observe either the old done tail (@c) or the new
 * done tail (@e). So, done tail updates and reads need
 * to use the rel-acq semantics. If the concurrent kworker
 * observes the old done tail, the newly queued work
 * execution will process the updated done tail. If the
 * concurrent kworker observes the new done tail, then
 * the newly queued work will skip processing the done
 * tail, as workqueue semantics guarantees that the new
 * work is executed only after the previous one completes.
 *
 * f. kworker callbacks processing complete:
 *
 *
 *                   DONE TAIL
 *                     |
 *                     |
 *                     v
 * +----------+     +--------+
 * |          |     |        |
 * |   head   ------> wait   |
 * |          |     | head2  |
 * +----------+     +--------+
 *
 */
/*
 * rcu_sr_is_wait_head() 判断 @node 是否落在静态 synchronize wait-head 池中。
 * @node 必须是可比较的有效 llist 节点；返回 true 表示内部哨兵，false 表示普通
 * rcu_synchronize 请求。只读、无锁、无副作用，节点生命周期由全局池/请求队列保证。
 */
static bool rcu_sr_is_wait_head(struct llist_node *node)
{
	return &(rcu_state.srs_wait_nodes)[0].node <= node &&
		node <= &(rcu_state.srs_wait_nodes)[SR_NORMAL_GP_WAIT_HEAD_MAX - 1].node;
}

/* rcu_sr_get_wait_head() 从缓存/分配取得批次哨兵；成功返回 owned 节点，失败 NULL。 */
static struct llist_node *rcu_sr_get_wait_head(void)
{
	struct sr_wait_node *sr_wn;
	int i;

	for (i = 0; i < SR_NORMAL_GP_WAIT_HEAD_MAX; i++) {
		sr_wn = &(rcu_state.srs_wait_nodes)[i];

		if (!atomic_cmpxchg_acquire(&sr_wn->inuse, 0, 1))
			return &sr_wn->node;
	}

	return NULL;
}

/* rcu_sr_put_wait_head() 回收批次哨兵到缓存或释放；调用后不得再访问。 */
static void rcu_sr_put_wait_head(struct llist_node *node)
{
	struct sr_wait_node *sr_wn = container_of(node, struct sr_wait_node, node);

	atomic_set_release(&sr_wn->inuse, 0);
}

static int rcu_normal_wake_from_gp = 1;
module_param(rcu_normal_wake_from_gp, int, 0644);
static struct workqueue_struct *sync_wq;

#define RCU_SR_NORMAL_LATCH_THR 64

/* Number of in-flight synchronize_rcu() calls queued on srs_next. */
/*
 * rcu_sr_normal_count 统计已经发布到 srs_next、但尚未 complete 的
 * synchronize_rcu() 请求。它与 latched 共同决定是否继续使用批量快路径。
 */
static atomic_long_t rcu_sr_normal_count;
static int rcu_sr_normal_latched; /* 0/1 */

/*
 * 宽限期结束后，由 GP kthread/worker 对队列中的每个请求调用此函数，
 * 唤醒阻塞在 synchronize_rcu_normal() 里的调用者。
 *
 * 执行顺序的约束：
 *   complete() 必须在 dec 之前执行。
 *   如果先 dec 再 complete()：另一个线程可能看到 nr==0 并清除 latch，
 *   而此时 complete() 还没执行，唤醒丢失。先 complete() 再 dec 保证
 *   等待者被唤醒后，计数才减少，latch 清除时机正确。
 *
 * latch 清除时机：
 *   当 nr 降到 0（队列完全排空）且 latch 曾被置 1 时，将其清零，
 *   恢复 synchronize_rcu_normal() 走快路径。
 *   用 cmpxchg_relaxed 而非直接写 0，是为了避免在 latch 本就为 0 时
 *   产生不必要的写操作（保持 cacheline 干净）。
 */
/* rcu_sr_normal_complete() 遍历一个成熟请求批次，complete 每个同步等待者。 */
static void rcu_sr_normal_complete(struct llist_node *node)
{
	struct rcu_synchronize *rs = container_of(
		(struct rcu_head *) node, struct rcu_synchronize, head);
	long nr;

	/*
	 * 仅 CONFIG_PROVE_RCU：验证从提交请求到现在确实经历了完整宽限期。
	 * 用入队前抓取的 oldstate 快照做检查，防止 GP kthread 提前唤醒等待者。
	 */
	WARN_ONCE(IS_ENABLED(CONFIG_PROVE_RCU) &&
		!poll_state_synchronize_rcu_full(&rs->oldstate),
		"A full grace period is not passed yet!\n");

	/* Finally. */
	/* 最后完成等待者；必须先 complete()，再递减仍在途请求计数。 */
	complete(&rs->completion);
	nr = atomic_long_dec_return(&rcu_sr_normal_count);
	WARN_ON_ONCE(nr < 0);

	/*
	 * Unlatch: switch back to normal path when fully
	 * drained and if it has been latched.
	 */
	/*
	 * 队列完全排空且 latch 确实为 1 时才解除闩锁，恢复普通快路径。
	 * cmpxchg 避免覆盖并发请求刚设置的新状态。
	 */
	if (nr == 0)
		(void)cmpxchg_relaxed(&rcu_sr_normal_latched, 1, 0);
}

/*
 * cleanup_work 在进程上下文处理 GP 后积累的 synchronize_rcu() 请求链，完成等待者
 * 并回收哨兵；避免在 GP kthread 持锁/时延敏感阶段做大批 completion。
 */
static void rcu_sr_normal_gp_cleanup_work(struct work_struct *work)
{
	struct llist_node *done, *rcu, *next, *head;

	/*
	 * This work execution can potentially execute
	 * while a new done tail is being updated by
	 * grace period kthread in rcu_sr_normal_gp_cleanup().
	 * So, read and updates of done tail need to
	 * follow acq-rel semantics.
	 *
	 * Given that wq semantics guarantees that a single work
	 * cannot execute concurrently by multiple kworkers,
	 * the done tail list manipulations are protected here.
	 */
	/*
	 * cleanup work 可能与 GP kthread 在 rcu_sr_normal_gp_cleanup() 中发布新的
	 * done tail 并发，因此 tail 的读取/写入必须使用 acquire/release。workqueue
	 * 保证同一个 work 不会被多个 kworker 同时执行，所以取得 tail 后，本函数内
	 * 对已摘取链段的普通指针操作无需额外互斥。
	 */
	done = smp_load_acquire(&rcu_state.srs_done_tail);
	if (WARN_ON_ONCE(!done))
		return;

	WARN_ON_ONCE(!rcu_sr_is_wait_head(done));
	head = done->next;
	done->next = NULL;

	/*
	 * The dummy node, which is pointed to by the
	 * done tail which is acq-read above is not removed
	 * here.  This allows lockless additions of new
	 * rcu_synchronize nodes in rcu_sr_normal_add_req(),
	 * while the cleanup work executes. The dummy
	 * nodes is removed, in next round of cleanup
	 * work execution.
	 */
	/*
	 * acquire 读到的 done tail 指向一个 dummy wait-head，本轮故意不回收它。
	 * 这样 rcu_sr_normal_add_req() 可在 worker 遍历期间继续无锁追加新请求；该
	 * dummy 会在下一轮 cleanup 中回收，作为生产者与消费者之间的稳定分隔符。
	 */
	llist_for_each_safe(rcu, next, head) {
		if (!rcu_sr_is_wait_head(rcu)) {
			rcu_sr_normal_complete(rcu);
			continue;
		}

		rcu_sr_put_wait_head(rcu);
	}

	/* Order list manipulations with atomic access. */
	/*
	 * release 递减把此前 complete、摘链和哨兵回收先于 pending 计数发布；观察到
	 * pending 归零的 GP 路径因此可以安全执行仅剩哨兵的快速回收。
	 */
	atomic_dec_return_release(&rcu_state.srs_cleanups_pending);
}

/*
 * Helper function for rcu_gp_cleanup().
 */
/*
 * rcu_sr_normal_gp_cleanup() 在 GP cleanup 阶段摘取本轮请求并调度/直接完成批次。
 * 无参数、无返回，仅由串行 GP cleanup 调用；它取得当前 wait-tail 的 ownership，
 * 小批量请求直接 complete，余下链交给 sync_wq。release/acquire 发布 done-tail，
 * 并用 srs_cleanups_pending 防止哨兵在 worker 尚未消费时被提前复用。
 */
static void rcu_sr_normal_gp_cleanup(void)
{
	struct llist_node *wait_tail, *next = NULL, *rcu = NULL;
	int done = 0;

	wait_tail = rcu_state.srs_wait_tail;
	if (wait_tail == NULL)
		return;

	rcu_state.srs_wait_tail = NULL;
	ASSERT_EXCLUSIVE_WRITER(rcu_state.srs_wait_tail);
	WARN_ON_ONCE(!rcu_sr_is_wait_head(wait_tail));

	/*
	 * Process (a) and (d) cases. See an illustration.
	 */
	/*
	 * 先在 GP kthread 中直接完成链首有限数量的普通请求，直到遇到下一枚 wait-head
	 * 或达到批量上限；这对应原设计图中的 (a)/(d)，目的是让常见小批次无需调度
	 * worker，同时限制 GP cleanup 的最长执行时间。
	 */
	llist_for_each_safe(rcu, next, wait_tail->next) {
		if (rcu_sr_is_wait_head(rcu))
			break;

		rcu_sr_normal_complete(rcu);
		// It can be last, update a next on this step.
		/*
		 * 当前请求可能是本段最后一个普通节点；每完成一个就立即推进 wait_tail->next，
		 * 使被唤醒等待者释放栈上节点后，链表不再保留悬空指针。
		 */
		wait_tail->next = next;

		if (++done == SR_MAX_USERS_WAKE_FROM_GP)
			break;
	}

	/*
	 * Fast path, no more users to process except putting the second last
	 * wait head if no inflight-workers. If there are in-flight workers,
	 * they will remove the last wait head.
	 *
	 * Note that the ACQUIRE orders atomic access with list manipulation.
	 */
	/*
	 * 若只剩倒数第二个 wait-head 且没有 worker 在途，可在 GP 线程直接回收并清空
	 * 链。对 pending 的 acquire 读取把 worker 的 release 递减与链表操作排序；
	 * 若仍有 worker，则由它回收最后一个哨兵，避免两方重复释放。
	 */
	if (wait_tail->next && wait_tail->next->next == NULL &&
	    rcu_sr_is_wait_head(wait_tail->next) &&
	    !atomic_read_acquire(&rcu_state.srs_cleanups_pending)) {
		rcu_sr_put_wait_head(wait_tail->next);
		wait_tail->next = NULL;
	}

	/* Concurrent sr_normal_gp_cleanup work might observe this update. */
	/*
	 * release 发布新的 done tail，与 cleanup worker 的 acquire 读取配对；后者一旦
	 * 看见 tail，就必须同时看见此前对 wait_tail->next 的所有修改。
	 */
	ASSERT_EXCLUSIVE_WRITER(rcu_state.srs_done_tail);
	smp_store_release(&rcu_state.srs_done_tail, wait_tail);

	/*
	 * We schedule a work in order to perform a final processing
	 * of outstanding users(if still left) and releasing wait-heads
	 * added by rcu_sr_normal_gp_init() call.
	 */
	/*
	 * 链上仍有请求或 wait-head 时，把剩余完成与哨兵回收移交 sync_wq。pending 在
	 * queue_work 前递增；若 work 已在队列中而入队返回 false，则撤销本次计数。
	 */
	if (wait_tail->next) {
		atomic_inc(&rcu_state.srs_cleanups_pending);
		if (!queue_work(sync_wq, &rcu_state.srs_cleanup_work))
			atomic_dec(&rcu_state.srs_cleanups_pending);
	}
}

/*
 * Helper function for rcu_gp_init().
 */
/*
 * rcu_sr_normal_gp_init() - GP 开始时把当前 synchronize 请求封入本轮批次。
 * 返回是否存在需要本 GP 满足的请求；失败分配时使用保守 fallback，不能提前唤醒。
 */
/*
 * 修正说明：返回值不是“是否存在请求”。false 是正常结果，包括无待处理请求或已成功
 * 插入 wait-head；true 仅表示确有请求但 dummy 池耗尽，本 GP 无法封段，需要通过
 * start_poll_synchronize_rcu() 再请求一轮重试。无参数；GP kthread 调用，相关
 * srs_wait_tail 只有该线程写，本函数不可因 dummy 失败而完成任何等待者。
 */
static bool rcu_sr_normal_gp_init(void)
{
	struct llist_node *first;
	struct llist_node *wait_head;
	bool start_new_poll = false;

	first = READ_ONCE(rcu_state.srs_next.first);
	if (!first || rcu_sr_is_wait_head(first))
		return start_new_poll;

	wait_head = rcu_sr_get_wait_head();
	if (!wait_head) {
		// Kick another GP to retry.
		/* 固定 dummy 池耗尽，当前 GP 无法封段；请求下一 GP 再试，不能提前完成等待者。 */
		start_new_poll = true;
		return start_new_poll;
	}

	/* Inject a wait-dummy-node. */
	/*
	 * 插入 wait dummy，把“本 GP 应满足的请求”与随后到达的新请求分段；节点本身
	 * 不代表等待者，只提供 lockless llist 的代际边界。
	 */
	llist_add(wait_head, &rcu_state.srs_next);

	/*
	 * A waiting list of rcu_synchronize nodes should be empty on
	 * this step, since a GP-kthread, rcu_gp_init() -> gp_cleanup(),
	 * rolls it over. If not, it is a BUG, warn a user.
	 */
	/*
	 * 到达 GP init 时旧 wait 链应已由上一轮 init→cleanup 轮转走；非空表示 GP
	 * 批次状态机丢失了边界。报警后仍以新哨兵覆盖，便于暴露而非静默隐藏错误。
	 */
	WARN_ON_ONCE(rcu_state.srs_wait_tail != NULL);
	rcu_state.srs_wait_tail = wait_head;
	ASSERT_EXCLUSIVE_WRITER(rcu_state.srs_wait_tail);

	return start_new_poll;
}

/*

 * 将一个 synchronize_rcu() 请求加入全局待处理队列（srs_next llist），
 * 并在并发量超过阈值时设置 latch 标志切换到慢路径。
 *
 * 为什么要先递增计数再入队（而不是先入队再递增）？
 *   rcu_sr_normal_complete() 在回调中会递减计数，当计数降到 0 时清除 latch。
 *   如果先入队再递增：GP kthread 有可能在递增之前就看到节点并调用 complete()，
 *   导致计数先减到 -1 再加回 0，触发 WARN_ON_ONCE(nr < 0)，同时 latch 清除时机
 *   也会错乱。先递增再入队确保 complete() 永远在 inc 之后执行，计数始终 >= 0。
 *
 * latch 机制（rcu_sr_normal_latched）的作用：
 *   当并发请求数 == RCU_SR_NORMAL_LATCH_THR（64）时，将 latch 从 0 置 1。
 *   之后新来的 synchronize_rcu_normal() 看到 latch == 1 就走慢路径，
 *   避免队列继续膨胀加剧延迟。latch 在队列完全排空（nr 降到 0）时由
 *   rcu_sr_normal_complete() 自动清除，恢复快路径。
 *
 *   用精确匹配（nr == 阈值）而非 >= 阈值，是为了让 cmpxchg 只在一个上下文
 *   中触发，避免多个并发调用者同时竞争写 latch 带来不必要的 cacheline 争抢。
 *   latch 是 best-effort 的：并发的 set/clear 可能短暂丢失，但无关正确性，
 *   只影响快/慢路径的选择。
 */
/* rcu_sr_normal_add_req() 无锁追加栈上同步请求；等待者保持 @rs 存活至 completion。 */
static void rcu_sr_normal_add_req(struct rcu_synchronize *rs)
{
	/*
	 * Increment before publish to avoid a complete
	 * vs enqueue race on latch.
	 */
	/*
	 * 必须先增加在途数再发布链表节点：否则 GP cleanup 可在生产者增计数前完成并
	 * 递减该请求，导致负计数和错误解除 latch。
	 */
	long nr = atomic_long_inc_return(&rcu_sr_normal_count);

	/*
	 * Latch when threshold is reached. Checking for an exact match
	 * restricts cmpxchg() to a single context.
	 *
	 * This latch is intentionally relaxed and best-effort. Concurrent
	 * set/clear can race and temporarily lose the latch, which is OK
	 * because it only selects between the fast and fallback paths.
	 */
	/*
	 * 计数恰好到阈值时由唯一上下文尝试置 latch，避免所有超阈值提交者争抢同一
	 * cacheline。relaxed/best-effort 足够，因为 latch 只选择批量快路径或保守
	 * fallback；并发 set/clear 暂时丢失状态不会破坏 GP 正确性。
	 */
	if (nr == RCU_SR_NORMAL_LATCH_THR)
		(void)cmpxchg_relaxed(&rcu_sr_normal_latched, 0, 1);

	/* Publish for the GP kthread/worker. */
	/* 发布到 GP kthread/worker 可见的全局链表。 */
	llist_add((struct llist_node *) &rs->head, &rcu_state.srs_next);
}

/*
 * Initialize a new grace period.  Return false if no grace period required.
 */
/*
 * rcu_gp_init() - 启动一轮常规宽限期并给整棵 rcu_node 树建立 QS 快照。
 *
 * GP kthread 上下文可睡眠。先确认存在 future-GP/同步请求，推进全局 gp_seq 到开始态，
 * 再自根向叶初始化 gp_seq/qsmask/qsmaskinitnext，记录每 CPU dynticks 快照并处理
 * offline CPU。成功返回 true，表示进入 FQS loop；若请求已被别轮满足返回 false。
 * 发布开始序号前后的屏障保证更新者的发布先于 GP 观察，QS 上报不会跨错代际。
 */
static noinline_for_stack bool rcu_gp_init(void)
{
	unsigned long flags;
	unsigned long oldmask;
	unsigned long mask;
	struct rcu_data *rdp;
	struct rcu_node *rnp = rcu_get_root();
	bool start_new_poll;
	unsigned long old_gp_seq;

	WRITE_ONCE(rcu_state.gp_activity, jiffies);
	raw_spin_lock_irq_rcu_node(rnp);
	/* 阶段 1：在根锁下消费 GP 请求。无 flags 是伪唤醒，不能凭空推进序号。 */
	if (!rcu_state.gp_flags) {
		/* Spurious wakeup, tell caller to go back to sleep.  */
		/* 没有 INIT/FQS 请求，说明是伪唤醒；释放根锁并让 GP 线程重新睡眠。 */
		raw_spin_unlock_irq_rcu_node(rnp);
		return false;
	}
	WRITE_ONCE(rcu_state.gp_flags, 0); /* Clear all flags: New GP. */
	/* 行尾英文说明：开始新 GP 前消费全部旧唤醒标志，后续请求会重新置位。 */

	if (WARN_ON_ONCE(rcu_gp_in_progress())) {
		/*
		 * Grace period already in progress, don't start another.
		 * Not supposed to be able to happen.
		 */
		/*
		 * 根锁下发现序号已处于进行态，表示状态机发生异常或重复 init；不能重入启动
		 * 第二轮 GP，否则同一组 qsmask 会被新代覆盖。
		 */
		raw_spin_unlock_irq_rcu_node(rnp);
		return false;
	}

	/* Advance to a new grace period and initialize state. */
	/* 阶段 2：先建立同步等待者批次，再发布 gp_seq 开始态，避免旧等待者串入新 GP。 */
	record_gp_stall_check_time();
	/*
	 * A new wait segment must be started before gp_seq advanced, so
	 * that previous gp waiters won't observe the new gp_seq.
	 */
	/*
	 * 必须先用 dummy 建立新等待段，再推进全局 gp_seq；否则上一段等待者可能先观察
	 * 到新序号，却尚未被归入本轮完成集合，从而提前或永久等待。
	 */
	start_new_poll = rcu_sr_normal_gp_init();
	/* Record GP times before starting GP, hence rcu_seq_start(). */
	/*
	 * 在 rcu_seq_start() 发布“GP 进行中”之前记录时间和旧序号，保证 stall 统计与
	 * PROVE_RCU guardband 都引用正确的一轮。
	 */
	old_gp_seq = rcu_state.gp_seq;
	/*
	 * Critical ordering: rcu_seq_start() must happen BEFORE the CPU hotplug
	 * scan below. Otherwise we risk a race where a newly onlining CPU could
	 * be missed by the current grace period, potentially leading to
	 * use-after-free errors. For a detailed explanation of this race, see
	 * Documentation/RCU/Design/Requirements/Requirements.rst in the
	 * "Hotplug CPU" section.
	 *
	 * Also note that the root rnp's gp_seq is kept separate from, and lags,
	 * the rcu_state's gp_seq, for a reason. See the Quick-Quiz on
	 * Single-node systems for more details (in Data-Structures.rst).
	 */
	/*
	 * gp_seq 必须先于 hotplug 扫描开始：新上线 CPU 要么看见新序号并被本轮纳入，
	 * 要么明确属于发布之后、无需本轮等待。反序会漏掉恰在扫描窗口上线的旧读者，
	 * 使更新者过早回收。root rnp 序号刻意稍后同步，作为树初始化发布边界。
	 */
	rcu_seq_start(&rcu_state.gp_seq);
	/* Ensure that rcu_seq_done_exact() guardband doesn't give false positives. */
	/*
	 * PROVE_RCU 下确认旧快照不会被精确完成判断误认为已经跨过新 GP；这是序号环绕
	 * guardband 的自检，不参与正常控制流。
	 */
	WARN_ON_ONCE(IS_ENABLED(CONFIG_PROVE_RCU) &&
		     rcu_seq_done_exact(&old_gp_seq, rcu_seq_snap(&rcu_state.gp_seq)));

	ASSERT_EXCLUSIVE_WRITER(rcu_state.gp_seq);
	trace_rcu_grace_period(rcu_state.name, rcu_state.gp_seq, TPS("start"));
	rcu_poll_gp_seq_start(&rcu_state.gp_seq_polled_snap);
	raw_spin_unlock_irq_rcu_node(rnp);

	/*
	 * The "start_new_poll" is set to true, only when this GP is not able
	 * to handle anything and there are outstanding users. It happens when
	 * the rcu_sr_normal_gp_init() function was not able to insert a dummy
	 * separator to the llist, because there were no left any dummy-nodes.
	 *
	 * Number of dummy-nodes is fixed, it could be that we are run out of
	 * them, if so we start a new pool request to repeat a try. It is rare
	 * and it means that a system is doing a slow processing of callbacks.
	 */
	/*
	 * start_new_poll 只在仍有同步用户、但固定数量 dummy 已耗尽导致本 GP 无法封装
	 * 任何请求时为真。此时启动额外 poll 请求以便下一 GP 重试；该罕见分支通常说明
	 * callback/cleanup 消费过慢，而不是请求已经满足。
	 */
	if (start_new_poll)
		(void) start_poll_synchronize_rcu();

	/*
	 * Apply per-leaf buffered online and offline operations to
	 * the rcu_node tree. Note that this new grace period need not
	 * wait for subsequent online CPUs, and that RCU hooks in the CPU
	 * offlining path, when combined with checks in this function,
	 * will handle CPUs that are currently going offline or that will
	 * go offline later.  Please also refer to "Hotplug CPU" section
	 * of RCU's Requirements documentation.
	 */
	/*
	 * 阶段 3：把 hotplug 缓冲的 qsmaskinitnext 落到每个叶节点。ofl_lock 与叶锁
	 * 串行化 offline QS 上报；首 CPU 上线向父层增加位，最后 CPU 离线且无阻塞任务
	 * 才向父层清位。旧 GP 阻塞任务可让一个无在线 CPU 的节点继续留在树中。
	 */
	WRITE_ONCE(rcu_state.gp_state, RCU_GP_ONOFF);
	/* Exclude CPU hotplug operations. */
	/*
	 * 遍历期间逐叶取得 ofl_lock，把本轮 online/offline 快照与 CPU hotplug 串行化；
	 * 它保护 qsmaskinitnext 到 qsmaskinit 的拓扑提交。
	 */
	rcu_for_each_leaf_node(rnp) {
		local_irq_disable();
		/*
		 * Serialize with CPU offline. See Requirements.rst > Hotplug CPU >
		 * Concurrent Quiescent State Reporting for Offline CPUs.
		 */
		/*
		 * ofl_lock 与叶节点锁共同排除 CPU offline 的并发 QS 上报，确保既不会漏掉
		 * 刚离线 CPU，也不会把同一离线事件向父树传播两次。
		 */
		arch_spin_lock(&rcu_state.ofl_lock);
		raw_spin_lock_rcu_node(rnp);
		if (rnp->qsmaskinit == rnp->qsmaskinitnext &&
		    !rnp->wait_blkd_tasks) {
			/* Nothing to do on this leaf rcu_node structure. */
			/* 在线掩码未变且无旧阻塞任务，本叶无需更新父层，立即释放两把锁。 */
			raw_spin_unlock_rcu_node(rnp);
			arch_spin_unlock(&rcu_state.ofl_lock);
			local_irq_enable();
			continue;
		}

		/* Record old state, apply changes to ->qsmaskinit field. */
		/* 保存旧在线掩码并提交 next；后续仅在“是否为零”变化时调整父节点成员位。 */
		oldmask = rnp->qsmaskinit;
		rnp->qsmaskinit = rnp->qsmaskinitnext;

		/* If zero-ness of ->qsmaskinit changed, propagate up tree. */
	/* 叶节点从空变非空或从非空变空才改变父层拓扑；普通 CPU 增减留在本叶位图内。 */
		if (!oldmask != !rnp->qsmaskinit) {
			if (!oldmask) { /* First online CPU for rcu_node. */
				/* 本叶首个 CPU 上线，使整条祖先路径重新成为 GP 等待对象。 */
				if (!rnp->wait_blkd_tasks) /* Ever offline? */
					/* 无旧 reader 阻塞才需要重新挂接；否则节点从未真正退出树。 */
					rcu_init_new_rnp(rnp);
			} else if (rcu_preempt_has_tasks(rnp)) {
				rnp->wait_blkd_tasks = true; /* blocked tasks */
				/* 最后 CPU 离线但仍有旧 reader，保留节点直到任务解除阻塞。 */
			} else { /* Last offline CPU and can propagate. */
				/* 最后 CPU 离线且无阻塞 reader，可沿父树清除该叶的成员位。 */
				rcu_cleanup_dead_rnp(rnp);
			}
		}

		/*
		 * If all waited-on tasks from prior grace period are
		 * done, and if all this rcu_node structure's CPUs are
		 * still offline, propagate up the rcu_node tree and
		 * clear ->wait_blkd_tasks.  Otherwise, if one of this
		 * rcu_node structure's CPUs has since come back online,
		 * simply clear ->wait_blkd_tasks.
		 */
		/*
		 * 若旧 GP 等待的阻塞任务已全部退出且本叶仍离线，向父树传播死亡；若期间已有
		 * CPU 重新上线，则节点已经重新有效，只需清 wait_blkd_tasks 哨兵。
		 */
		if (rnp->wait_blkd_tasks &&
		    (!rcu_preempt_has_tasks(rnp) || rnp->qsmaskinit)) {
			rnp->wait_blkd_tasks = false;
			if (!rnp->qsmaskinit)
				rcu_cleanup_dead_rnp(rnp);
		}

		raw_spin_unlock_rcu_node(rnp);
		arch_spin_unlock(&rcu_state.ofl_lock);
		local_irq_enable();
	}
	rcu_gp_slow(gp_preinit_delay); /* Races with CPU hotplug. */
	/* 行尾英文说明：这里的测试延时故意与 CPU 热插拔并发，用来放大初始化竞态。 */

	/*
	 * Set the quiescent-state-needed bits in all the rcu_node
	 * structures for all currently online CPUs in breadth-first
	 * order, starting from the root rcu_node structure, relying on the
	 * layout of the tree within the rcu_state.node[] array.  Note that
	 * other CPUs will access only the leaves of the hierarchy, thus
	 * seeing that no grace period is in progress, at least until the
	 * corresponding leaf node has been initialized.
	 *
	 * The grace period cannot complete until the initialization
	 * process finishes, because this kthread handles both.
	 */
	/*
	 * 阶段 4：广度优先发布本轮 gp_seq/qsmask。父节点先就绪，CPU 只从叶节点观察，
	 * 因而不会在子节点初始化前把 QS 传播到未准备好的父层；GP kthread 自身负责完成
	 * 初始化，所以初始化未结束前也不可能进入 cleanup。
	 */
	WRITE_ONCE(rcu_state.gp_state, RCU_GP_INIT);
	rcu_for_each_node_breadth_first(rnp) {
		rcu_gp_slow(gp_init_delay);
		raw_spin_lock_irqsave_rcu_node(rnp, flags);
		rdp = this_cpu_ptr(&rcu_data);
		rcu_preempt_check_blocked_tasks(rnp);
		rnp->qsmask = rnp->qsmaskinit;
		WRITE_ONCE(rnp->gp_seq, rcu_state.gp_seq);
		if (rnp == rdp->mynode)
			(void)__note_gp_changes(rnp, rdp);
		rcu_preempt_boost_start_gp(rnp);
		trace_rcu_grace_period_init(rcu_state.name, rnp->gp_seq,
					    rnp->level, rnp->grplo,
					    rnp->grphi, rnp->qsmask);
		/*
		 * Quiescent states for tasks on any now-offline CPUs. Since we
		 * released the ofl and rnp lock before this loop, CPUs might
		 * have gone offline and we have to report QS on their behalf.
		 * See Requirements.rst > Hotplug CPU > Concurrent QS Reporting.
		 */
		/*
		 * 前一遍 hotplug 提交后已经释放 ofl_lock，CPU 可能又在此循环前离线。
		 * qsmask 与最新 qsmaskinitnext 的差集就是需由 GP kthread 代报的 CPU；
		 * 记录到 rcu_gp_init_mask 便于诊断，再按正常树上报协议清位。
		 */
		mask = rnp->qsmask & ~rnp->qsmaskinitnext;
		rnp->rcu_gp_init_mask = mask;
		if ((mask || rnp->wait_blkd_tasks) && rcu_is_leaf_node(rnp))
			/* 初始化期间刚离线的 CPU 由 GP 线程代报，避免永久等待死亡 CPU。 */
			rcu_report_qs_rnp(mask, rnp, rnp->gp_seq, flags);
		else
			raw_spin_unlock_irq_rcu_node(rnp);
		cond_resched_tasks_rcu_qs();
		WRITE_ONCE(rcu_state.gp_activity, jiffies);
	}

	// If strict, make all CPUs aware of new grace period.
	/* 严格模式用跨 CPU 调用强迫所有处理器观察新代际，换取更强测试边界与更高成本。 */
	if (IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD))
		on_each_cpu(rcu_strict_gp_boundary, NULL, 0);

	/*
	 * Immediately report QS for the GP kthread's CPU. The GP kthread
	 * cannot be in an RCU read-side critical section while running
	 * the FQS scan. This eliminates the need for a second FQS wait
	 * when all CPUs are idle.
	 */
	/*
	 * 阶段 5：GP kthread 正在进程上下文执行 FQS 初始化，不可能同时位于普通 RCU
	 * 读侧，立即为本 CPU 报 QS，避免全系统 idle 时还要多等一轮 FQS 超时。
	 */
	preempt_disable();
	rcu_qs();
	rcu_report_qs_rdp(this_cpu_ptr(&rcu_data));
	preempt_enable();

	return true;
}

/*
 * Helper function for swait_event_idle_exclusive() wakeup at force-quiescent-state
 * time.
 */
/*
 * rcu_gp_fqs_check_wake() 判断 FQS 等待是否应因 GP 完成或新 flags 提前醒来。
 * @gfp 是 GP 线程持有的 flags 快照输入/输出槽：函数先消费其中的 OVLD，再刷新全局
 * gp_flags。返回 true 表示 wait 条件已满足，false 表示根仍等待 QS；无锁快照允许
 * 保守唤醒，函数不清 qsmask，也不保证 GP 已完成。
 */
static bool rcu_gp_fqs_check_wake(int *gfp)
{
	struct rcu_node *rnp = rcu_get_root();

	// If under overload conditions, force an immediate FQS scan.
	/* callback 过载时不再等待自然 deadline，立即唤醒 GP 线程进行 FQS。 */
	if (*gfp & RCU_GP_FLAG_OVLD)
		return true;

	// Someone like call_rcu() requested a force-quiescent-state scan.
	/* call_rcu() 等路径显式置 FQS 标志时立即结束等待，避免 callback backlog 继续增长。 */
	*gfp = READ_ONCE(rcu_state.gp_flags);
	if (*gfp & RCU_GP_FLAG_FQS)
		return true;

	// The current grace period has completed.
	/* 根节点已无 CPU 位和阻塞 reader，GP 已满足，唤醒线程进入 cleanup。 */
	if (!READ_ONCE(rnp->qsmask) && !rcu_preempt_blocked_readers_cgp(rnp))
		return true;

	return false;
}

/*
 * Do one round of quiescent-state forcing.
 */
/*
 * rcu_gp_fqs() - 扫描仍未上报 CPU 的 dynticks/离线状态并强制催促 QS。
 * @first_time 决定保存初始快照还是比较变化；只把有可靠 EQS/离线证据的位清除，
 * 否则请求 resched/tick/irq-work，绝不直接假定 CPU 已静止。
 *
 * 补充契约：仅由可睡眠的 GP kthread 在一轮在途 GP 中调用；函数无返回。它更新
 * 全局 FQS 活动/统计，借 force_qs_rnp() 逐叶短暂持锁，并在根锁下消费 FQS 标志。
 * @rnp 是根节点借用指针，@nr_fqs 是 stall 调试预算；本函数不会结束 GP。
 */
static void rcu_gp_fqs(bool first_time)
{
	int nr_fqs = READ_ONCE(rcu_state.nr_fqs_jiffies_stall);
	struct rcu_node *rnp = rcu_get_root();

	WRITE_ONCE(rcu_state.gp_activity, jiffies);
	WRITE_ONCE(rcu_state.n_force_qs, rcu_state.n_force_qs + 1);
	/* 每轮先刷新 watchdog 活动时间与 FQS 统计，证明 GP 线程仍在推进而非 stall。 */

	WARN_ON_ONCE(nr_fqs > 3);
	/* Only countdown nr_fqs for stall purposes if jiffies moves. */
	/*
	 * stall 调试计数只在 jiffies 确实前进时递减；停表/注入延迟期间重复扫描不能冒充
	 * 时间进展，否则 stall 诊断会错误地认为已经完成多轮有效 FQS。
	 */
	if (nr_fqs) {
		if (nr_fqs == 1) {
			WRITE_ONCE(rcu_state.jiffies_stall,
				   jiffies + rcu_jiffies_till_stall_check());
		}
		WRITE_ONCE(rcu_state.nr_fqs_jiffies_stall, --nr_fqs);
	}

	if (first_time) {
		/* Collect dyntick-idle snapshots. */
		/* 首轮只保存基线；采样时已在 EQS 的 CPU 可立即成为 QS 证据。 */
		force_qs_rnp(rcu_watching_snap_save);
	} else {
		/* Handle dyntick-idle and offline CPUs. */
		/* 后续轮比较基线，发现计数变化/当前 EQS/离线即可为对应 CPU 清 QS 位。 */
		force_qs_rnp(rcu_watching_snap_recheck);
	}
	/* Clear flag to prevent immediate re-entry. */
	/* 消费显式 FQS 请求；若不清，wait 条件会立即再次唤醒形成忙循环。 */
	if (READ_ONCE(rcu_state.gp_flags) & RCU_GP_FLAG_FQS) {
		raw_spin_lock_irq_rcu_node(rnp);
		WRITE_ONCE(rcu_state.gp_flags, rcu_state.gp_flags & ~RCU_GP_FLAG_FQS);
		raw_spin_unlock_irq_rcu_node(rnp);
	}
}

/*
 * Loop doing repeated quiescent-state forcing until the grace period ends.
 */
/*
 * rcu_gp_fqs_loop() - GP 中段循环：等待自然 QS，超时后执行 FQS，直到根 qsmask 清零。
 * GP kthread 可睡眠；动态调整等待间隔，对 stall/urgent/nohz_full 情况升级催促。
 *
 * 补充契约：无参数、无返回，只能在 rcu_gp_init() 成功后由唯一 GP kthread 调用。
 * @j 是下一次 FQS 间隔，@gf 是被 wait 条件刷新的 flags，@ret 区分超时与事件唤醒，
 * @first_gp_fqs 选择保存或复查 dynticks 快照。退出只表示根已无 CPU/reader 阻塞，
 * 真正发布 GP 完成由随后 rcu_gp_cleanup() 负责。
 */
static noinline_for_stack void rcu_gp_fqs_loop(void)
{
	bool first_gp_fqs = true;
	int gf = 0;
	unsigned long j;
	int ret;
	struct rcu_node *rnp = rcu_get_root();

	j = READ_ONCE(jiffies_till_first_fqs);
	if (rcu_state.cbovld)
		gf = RCU_GP_FLAG_OVLD;
	ret = 0;
	for (;;) {
		/* 阶段 1：callback 过载时缩短等待，但至少保留一个 tick，避免纯自旋。 */
		if (rcu_state.cbovld) {
			j = (j + 2) / 3;
			if (j <= 0)
				j = 1;
		}
		if (!ret || time_before(jiffies + j, rcu_state.jiffies_force_qs)) {
			WRITE_ONCE(rcu_state.jiffies_force_qs, jiffies + j);
			/*
			 * jiffies_force_qs before RCU_GP_WAIT_FQS state
			 * update; required for stall checks.
			 */
			/* 先发布 deadline 再发布 WAIT 状态，stall 检查者不会看到无期限的等待态。 */
			smp_wmb();
			WRITE_ONCE(rcu_state.jiffies_kick_kthreads,
				   jiffies + (j ? 3 * j : 2));
		}
		trace_rcu_grace_period(rcu_state.name, rcu_state.gp_seq,
				       TPS("fqswait"));
		WRITE_ONCE(rcu_state.gp_state, RCU_GP_WAIT_FQS);
		(void)swait_event_idle_timeout_exclusive(rcu_state.gp_wq,
				 rcu_gp_fqs_check_wake(&gf), j);
		/* 阶段 2：自然 QS、显式 FQS、过载或超时任一条件唤醒；伪信号不完成 GP。 */
		rcu_gp_torture_wait();
		WRITE_ONCE(rcu_state.gp_state, RCU_GP_DOING_FQS);
		/* Locking provides needed memory barriers. */
	/*
	 * swait 内部的解锁—加锁序列提供检查根 qsmask 所需的屏障：唤醒者在节点锁下
	 * 清位并唤醒，GP 线程返回后不会把清位前的旧值当作当前状态。
	 */
		/*
		 * Exit the loop if the root rcu_node structure indicates that the grace period
		 * has ended, leave the loop.  The rcu_preempt_blocked_readers_cgp(rnp) check
		 * is required only for single-node rcu_node trees because readers blocking
		 * the current grace period are queued only on leaf rcu_node structures.
		 * For multi-node trees, checking the root node's ->qsmask suffices, because a
		 * given root node's ->qsmask bit is cleared only when all CPUs and tasks from
		 * the corresponding leaf nodes have passed through their quiescent state.
		 */
		/*
		 * 根 qsmask 清零表示所有对应叶的 CPU/任务已经静止；单节点树的被抢占 reader
		 * 直接挂在根/叶同一节点上，必须额外检查阻塞链。多节点树只有叶挂 reader，
		 * 叶未完成就不会清父位，因此根 mask 已经包含该信息。
		 */
		if (!READ_ONCE(rnp->qsmask) &&
		    !rcu_preempt_blocked_readers_cgp(rnp))
			break;
		/* 根 mask 清零只覆盖 CPU；单节点树还需单独确认被抢占旧读者链为空。 */
		/* If time for quiescent-state forcing, do it. */
		/* 到达 deadline、收到显式 FQS 请求或 callback 过载时执行一轮主动扫描。 */
		if (!time_after(rcu_state.jiffies_force_qs, jiffies) ||
		    (gf & (RCU_GP_FLAG_FQS | RCU_GP_FLAG_OVLD))) {
			/* 阶段 3：到期/请求/过载时采样 dynticks 并催促仍未静止的 CPU。 */
			trace_rcu_grace_period(rcu_state.name, rcu_state.gp_seq,
					       TPS("fqsstart"));
			rcu_gp_fqs(first_gp_fqs);
			gf = 0;
			if (first_gp_fqs) {
				first_gp_fqs = false;
				gf = rcu_state.cbovld ? RCU_GP_FLAG_OVLD : 0;
			}
			trace_rcu_grace_period(rcu_state.name, rcu_state.gp_seq,
					       TPS("fqsend"));
			cond_resched_tasks_rcu_qs();
			WRITE_ONCE(rcu_state.gp_activity, jiffies);
			ret = 0; /* Force full wait till next FQS. */
			/* 行尾英文说明：完成一次 FQS 后重置为完整等待周期，避免连续忙扫。 */
			j = READ_ONCE(jiffies_till_next_fqs);
		} else {
			/* Deal with stray signal. */
			/* 非完成唤醒保留原 deadline，只计算剩余等待，避免信号把 FQS 无限推迟。 */
			cond_resched_tasks_rcu_qs();
			WRITE_ONCE(rcu_state.gp_activity, jiffies);
			WARN_ON(signal_pending(current));
			trace_rcu_grace_period(rcu_state.name, rcu_state.gp_seq,
					       TPS("fqswaitsig"));
			ret = 1; /* Keep old FQS timing. */
			/* 行尾英文说明：杂散信号不重置 deadline，只等待原计划剩余时长。 */
			j = jiffies;
			if (time_after(jiffies, rcu_state.jiffies_force_qs))
				j = 1;
			else
				j = rcu_state.jiffies_force_qs - j;
			gf = 0;
		}
	}
}

/*
 * Clean up after the old grace period.
 */
/*
 * rcu_gp_cleanup() - 完成当前 GP，推进结束序号并唤醒 callback/等待者。
 * 在根/各 node 锁下核对 qsmask 已清、发布 gp_seq 结束态、清本轮状态并判断是否仍有
 * future GP；随后完成 synchronize 请求、唤醒 core/下一轮。返回无。
 */
static noinline void rcu_gp_cleanup(void)
{
	int cpu;
	bool needgp = false;
	unsigned long gp_duration;
	unsigned long new_gp_seq;
	bool offloaded;
	struct rcu_data *rdp;
	struct rcu_node *rnp = rcu_get_root();
	struct swait_queue_head *sq;

	WRITE_ONCE(rcu_state.gp_activity, jiffies);
	raw_spin_lock_irq_rcu_node(rnp);
	rcu_state.gp_end = jiffies;
	gp_duration = rcu_state.gp_end - rcu_state.gp_start;
	/* 阶段 1：根已无 QS/阻塞读者，记录持续时间；对外 gp_seq 仍暂时保持进行中。 */
	if (gp_duration > rcu_state.gp_max)
		rcu_state.gp_max = gp_duration;

	/*
	 * We know the grace period is complete, but to everyone else
	 * it appears to still be ongoing.  But it is also the case
	 * that to everyone else it looks like there is nothing that
	 * they can do to advance the grace period.  It is therefore
	 * safe for us to drop the lock in order to mark the grace
	 * period as completed in all of the rcu_node structures.
	 */
	/*
	 * 所有人都能看见“根已无法再推进但序号尚未结束”，因此可暂时解根锁逐节点发布
	 * 结束序号；没有并发路径会在这段窗口错误开始下一 GP。
	 */
	rcu_poll_gp_seq_end(&rcu_state.gp_seq_polled_snap);
	raw_spin_unlock_irq_rcu_node(rnp);

	/*
	 * Propagate new ->gp_seq value to rcu_node structures so that
	 * other CPUs don't have to wait until the start of the next grace
	 * period to process their callbacks.  This also avoids some nasty
	 * RCU grace-period initialization races by forcing the end of
	 * the current grace period to be completely recorded in all of
	 * the rcu_node structures before the beginning of the next grace
	 * period is recorded in any of the rcu_node structures.
	 */
	/*
	 * 阶段 2：广度优先把结束态写到每个 rcu_node。必须全树完成后才能更新全局结束态，
	 * 否则下一 GP 的开始可能与旧 GP 的叶节点结束交错，CPU 会错分 callback 代际。
	 */
	new_gp_seq = rcu_state.gp_seq;
	rcu_seq_end(&new_gp_seq);
	rcu_for_each_node_breadth_first(rnp) {
		raw_spin_lock_irq_rcu_node(rnp);
		if (WARN_ON_ONCE(rcu_preempt_blocked_readers_cgp(rnp)))
			dump_blkd_tasks(rnp, 10);
		WARN_ON_ONCE(rnp->qsmask);
		WRITE_ONCE(rnp->gp_seq, new_gp_seq);
		if (!rnp->parent)
			smp_mb(); // Order against failing poll_state_synchronize_rcu_full().
		/*
		 * 根节点完整屏障与 full poll 的失败检查排序，防止 poller 看见旧根序号却把
		 * 随后的访问重排到本 GP 完成发布之前。
		 */
		rdp = this_cpu_ptr(&rcu_data);
		if (rnp == rdp->mynode)
			needgp = __note_gp_changes(rnp, rdp) || needgp;
		/* smp_mb() provided by prior unlock-lock pair. */
		/*
		 * 前一个节点解锁再锁当前节点的组合经 smp_mb__after_unlock_lock() 提供完整
		 * 屏障，保证全树结束态按遍历顺序发布，无需在每个节点重复显式 smp_mb()。
		 */
		needgp = rcu_future_gp_cleanup(rnp) || needgp;
		/* 汇总仍未满足的 callback/synchronize 请求，决定 cleanup 后是否立即续开 GP。 */
		// Reset overload indication for CPUs no longer overloaded
		/* 叶节点 callback 已回落时清 cbovldmask，使下一轮 FQS 不再按过载策略催促。 */
		if (rcu_is_leaf_node(rnp))
			for_each_leaf_node_cpu_mask(rnp, cpu, rnp->cbovldmask) {
				rdp = per_cpu_ptr(&rcu_data, cpu);
				check_cb_ovld_locked(rdp, rnp);
			}
		sq = rcu_nocb_gp_get(rnp);
		raw_spin_unlock_irq_rcu_node(rnp);
		rcu_nocb_gp_cleanup(sq);
		cond_resched_tasks_rcu_qs();
		WRITE_ONCE(rcu_state.gp_activity, jiffies);
		rcu_gp_slow(gp_cleanup_delay);
	}
	rnp = rcu_get_root();
	raw_spin_lock_irq_rcu_node(rnp); /* GP before ->gp_seq update. */
	/* 阶段 3：重新取得根锁，所有 node 已发布结束态，现在提交全局 gp_seq 与 IDLE。 */

	/* Declare grace period done, trace first to use old GP number. */
	/*
	 * trace 必须先使用仍处于进行态的旧 GP 编号，再由 rcu_seq_end() 发布完成态；
	 * 若反序，结束事件会被错误归到下一序号，诊断工具也无法重建真实边界。
	 */
	trace_rcu_grace_period(rcu_state.name, rcu_state.gp_seq, TPS("end"));
	rcu_seq_end(&rcu_state.gp_seq);
	ASSERT_EXCLUSIVE_WRITER(rcu_state.gp_seq);
	WRITE_ONCE(rcu_state.gp_state, RCU_GP_IDLE);
	/* Check for GP requests since above loop. */
	/*
	 * 全树发布期间可能又到达 callback 请求。重新比较根 gp_seq_needed，防止只依据
	 * 循环前的 needgp 而漏开下一轮。
	 */
	rdp = this_cpu_ptr(&rcu_data);
	if (!needgp && ULONG_CMP_LT(rnp->gp_seq, rnp->gp_seq_needed)) {
		trace_rcu_this_gp(rnp, rdp, rnp->gp_seq_needed,
				  TPS("CleanupMore"));
		needgp = true;
	}
	/* Advance CBs to reduce false positives below. */
	/*
	 * 先推进当前 CPU callback，可让已有请求自行设置 INIT；随后只有确有 needgp 且
	 * 尚未置 INIT 时才显式写标志，减少不必要的下一轮 GP。
	 */
	offloaded = rcu_rdp_is_offloaded(rdp);
	if ((offloaded || !rcu_accelerate_cbs(rnp, rdp)) && needgp) {

		// We get here if a grace period was needed (“needgp”)
		// and the above call to rcu_accelerate_cbs() did not set
		// the RCU_GP_FLAG_INIT bit in ->gp_state (which records
		// the need for another grace period).  The purpose
		// of the “offloaded” check is to avoid invoking
		// rcu_accelerate_cbs() on an offloaded CPU because we do not
		// hold the ->nocb_lock needed to safely access an offloaded
		// ->cblist.  We do not want to acquire that lock because
		// it can be heavily contended during callback floods.
		/*
		 * 此分支表示 needgp 已确认，但普通 rcu_accelerate_cbs() 没有设置 INIT。
		 * offloaded CPU 因未持 nocb_lock 不能访问其 cblist，且 callback 洪峰时该锁
		 * 竞争很重；因此直接保守置 INIT，既避免漏开 GP，也避开 nocb 锁热点。
		 */

		WRITE_ONCE(rcu_state.gp_flags, RCU_GP_FLAG_INIT);
		/* nocb 列表不能无 nocb_lock 访问；直接置 INIT 保守请求下一 GP，避免漏请求。 */
		WRITE_ONCE(rcu_state.gp_req_activity, jiffies);
		trace_rcu_grace_period(rcu_state.name, rcu_state.gp_seq, TPS("newreq"));
	} else {

		// We get here either if there is no need for an
		// additional grace period or if rcu_accelerate_cbs() has
		// already set the RCU_GP_FLAG_INIT bit in ->gp_flags. 
		// So all we need to do is to clear all of the other
		// ->gp_flags bits.
		/*
		 * 这里要么无需下一 GP，要么 accelerate 已经保留 INIT；因此只清除 FQS/OVLD
		 * 等本轮临时标志，同时保留可能存在的 INIT 请求。
		 */

		WRITE_ONCE(rcu_state.gp_flags, rcu_state.gp_flags & RCU_GP_FLAG_INIT);
	}
	raw_spin_unlock_irq_rcu_node(rnp);

	// Make synchronize_rcu() users aware of the end of old grace period.
	/* 阶段 4：在序号完成发布后唤醒同步等待者，保证其返回时看到完整 GP 内存序。 */
	rcu_sr_normal_gp_cleanup();

	// If strict, make all CPUs aware of the end of the old grace period.
	/*
	 * 严格 GP 模式在结束后再次跨 CPU 制造边界，确保每个处理器都及时观察旧代完成；
	 * 这是测试型强保证，普通配置不承担 IPI 成本。
	 */
	if (IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD))
		on_each_cpu(rcu_strict_gp_boundary, NULL, 0);
}

/*
 * Body of kthread that handles grace periods.
 */
/*
 * rcu_gp_kthread() - 全局常规 GP 状态机线程，永久循环等待请求。
 * 每轮执行 init→FQS loop→cleanup；无请求时可冻结睡眠。标记 __noreturn，ownership
 * 属于 RCU 子系统，正常运行不退出。
 */
static int __noreturn rcu_gp_kthread(void *unused)
{
	rcu_bind_gp_kthread();
	for (;;) {

		/* Handle grace-period start. */
		/*
		 * 阶段 1：等待 INIT 请求并反复尝试 rcu_gp_init()。伪唤醒或竞态下请求已被
		 * 满足时重新等待；只有成功建立全树 qsmask 后才进入 FQS 阶段。
		 */
		for (;;) {
			trace_rcu_grace_period(rcu_state.name, rcu_state.gp_seq,
					       TPS("reqwait"));
			WRITE_ONCE(rcu_state.gp_state, RCU_GP_WAIT_GPS);
			swait_event_idle_exclusive(rcu_state.gp_wq,
					 READ_ONCE(rcu_state.gp_flags) &
					 RCU_GP_FLAG_INIT);
			rcu_gp_torture_wait();
			WRITE_ONCE(rcu_state.gp_state, RCU_GP_DONE_GPS);
			/* Locking provides needed memory barrier. */
			/*
			 * waitqueue 解锁/加锁把请求方对 gp_flags、gp_seq_needed 的发布排在本线程
			 * 初始化读取之前，避免看见唤醒却看不见请求内容。
			 */
			if (rcu_gp_init())
				break;
			cond_resched_tasks_rcu_qs();
			WRITE_ONCE(rcu_state.gp_activity, jiffies);
			WARN_ON(signal_pending(current));
			trace_rcu_grace_period(rcu_state.name, rcu_state.gp_seq,
					       TPS("reqwaitsig"));
		}

		/* Handle quiescent-state forcing. */
		/* 阶段 2：等待自然 QS，并在 deadline、过载或显式请求时执行 FQS。 */
		rcu_gp_fqs_loop();

		/* Handle grace-period end. */
		/* 阶段 3：全树发布完成态、推进 callback，并决定是否无缝接续下一轮。 */
		WRITE_ONCE(rcu_state.gp_state, RCU_GP_CLEANUP);
		rcu_gp_cleanup();
		WRITE_ONCE(rcu_state.gp_state, RCU_GP_CLEANED);
	}
}

/*
 * Report a full set of quiescent states to the rcu_state data structure.
 * Invoke rcu_gp_kthread_wake() to awaken the grace-period kthread if
 * another grace period is required.  Whether we wake the grace-period
 * kthread or it awakens itself for the next round of quiescent-state
 * forcing, that kthread will clean up after the just-completed grace
 * period.  Note that the caller must hold rnp->lock, which is released
 * before return.
 */
/* rcu_report_qs_rsp() 在根确认最后 QS 后解根锁并唤醒 GP kthread；消费 @flags IRQ 状态。 */
static void rcu_report_qs_rsp(unsigned long flags)
	__releases(rcu_get_root()->lock)
{
	raw_lockdep_assert_held_rcu_node(rcu_get_root());
	WARN_ON_ONCE(!rcu_gp_in_progress());
	WRITE_ONCE(rcu_state.gp_flags, rcu_state.gp_flags | RCU_GP_FLAG_FQS);
	raw_spin_unlock_irqrestore_rcu_node(rcu_get_root(), flags);
	rcu_gp_kthread_wake();
}

/*
 * Similar to rcu_report_qs_rdp(), for which it is a helper function.
 * Allows quiescent states for a group of CPUs to be reported at one go
 * to the specified rcu_node structure, though all the CPUs in the group
 * must be represented by the same rcu_node structure (which need not be a
 * leaf rcu_node structure, though it often will be).  The gps parameter
 * is the grace-period snapshot, which means that the quiescent states
 * are valid only if rnp->gp_seq is equal to gps.  That structure's lock
 * must be held upon entry, and it is released before return.
 *
 * As a special case, if mask is zero, the bit-already-cleared check is
 * disabled.  This allows propagating quiescent state due to resumed tasks
 * during grace-period initialization.
 */
/*
 * rcu_report_qs_rnp() - 清叶/中间节点的 QS 位，并在节点归零时逐层向根传播。
 * @mask 是本节点已静止成员位；@rnp 入口持 lock；@gps 防止把旧代 QS 报给新 GP；
 * @flags 保存 IRQ 状态。每层清 qsmask 后释放当前锁再锁父节点，避免同时持整条路径。
 * 到根最后一位时调用 rcu_report_qs_rsp() 唤醒 GP。
 */
static void rcu_report_qs_rnp(unsigned long mask, struct rcu_node *rnp,
			      unsigned long gps, unsigned long flags)
	__releases(rnp->lock)
{
	unsigned long oldmask = 0;
	struct rcu_node *rnp_c;

	raw_lockdep_assert_held_rcu_node(rnp);

	/* Walk up the rcu_node hierarchy. */
	/* 阶段 1：当前节点消费 mask。重复位或 gp_seq 已变化表示这份 QS 已过期，直接退出。 */
	for (;;) {
		if ((!(rnp->qsmask & mask) && mask) || rnp->gp_seq != gps) {

			/*
			 * Our bit has already been cleared, or the
			 * relevant grace period is already over, so done.
			 */
			/* 不能把旧 GP 的 QS 清到新 GP，否则可能使新 GP 提前完成并触发 UAF。 */
			raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
			return;
		}
		WARN_ON_ONCE(oldmask); /* Any child must be all zeroed! */
		/*
		 * 行尾英文说明：向父层传播前，刚完成的子节点 qsmask 必须已经全零；
		 * oldmask 非零说明在未完成整个子树时错误地清除了父位。
		 */
		WARN_ON_ONCE(!rcu_is_leaf_node(rnp) &&
			     rcu_preempt_blocked_readers_cgp(rnp));
		WRITE_ONCE(rnp->qsmask, rnp->qsmask & ~mask);
		/* 在节点锁下原子清本组位；trace 同时记录清前成员与清后剩余位。 */
		trace_rcu_quiescent_state_report(rcu_state.name, rnp->gp_seq,
						 mask, rnp->qsmask, rnp->level,
						 rnp->grplo, rnp->grphi,
						 !!rnp->gp_tasks);
		if (rnp->qsmask != 0 || rcu_preempt_blocked_readers_cgp(rnp)) {

			/* Other bits still set at this level, so done. */
			/* 同层尚有 CPU/子树或被抢占旧读者，当前上报到此为止。 */
			raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
			return;
		}
		rnp->completedqs = rnp->gp_seq;
		/* 阶段 2：本节点完整静止，发布 completedqs，并把自己在父节点的 grpmask 上报。 */
		mask = rnp->grpmask;
		if (rnp->parent == NULL) {

			/* No more levels.  Exit loop holding root lock. */
			/*
			 * 已到根节点，没有更高层可上报；保持根锁退出循环，交给
			 * rcu_report_qs_rsp() 原子发布 FQS 标志并负责解锁。
			 */

			break;
		}
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		rnp_c = rnp;
		rnp = rnp->parent;
		raw_spin_lock_irqsave_rcu_node(rnp, flags);
		/* 每上升一级先释放子锁再取父锁；父层并发上报由父锁串行，避免嵌套锁链。 */
		oldmask = READ_ONCE(rnp_c->qsmask);
	}

	/*
	 * Get here if we are the last CPU to pass through a quiescent
	 * state for this grace period.  Invoke rcu_report_qs_rsp()
	 * to clean up and start the next grace period if one is needed.
	 */
	/*
	 * 到达这里表示本次上报清掉了根节点最后一个等待位，也没有被抢占 reader 阻塞。
	 * rcu_report_qs_rsp() 唤醒 GP kthread 进入 cleanup，并在已有后续请求时接续新 GP。
	 */
	rcu_report_qs_rsp(flags); /* releases rnp->lock. */
	/* 行尾英文说明：被调函数消费 @flags 并释放根 rnp->lock，调用者不得再次解锁。 */
}

/*
 * Record a quiescent state for all tasks that were previously queued
 * on the specified rcu_node structure and that were blocking the current
 * RCU grace period.  The caller must hold the corresponding rnp->lock with
 * irqs disabled, and this lock is released upon return, but irqs remain
 * disabled.
 */
/*
 * rcu_report_unblock_qs_rnp() - 被抢占读者解除阻塞后，若本节点无其他阻塞则继续向根
 * 上报。入口持 rnp lock/IRQ flags；只在 PREEMPT_RCU 相关配置使用。
 */
static void __maybe_unused
rcu_report_unblock_qs_rnp(struct rcu_node *rnp, unsigned long flags)
	__releases(rnp->lock)
{
	unsigned long gps;
	unsigned long mask;
	struct rcu_node *rnp_p;

	raw_lockdep_assert_held_rcu_node(rnp);
	if (WARN_ON_ONCE(!IS_ENABLED(CONFIG_PREEMPT_RCU)) ||
	    WARN_ON_ONCE(rcu_preempt_blocked_readers_cgp(rnp)) ||
	    rnp->qsmask != 0) {
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		return;  /* Still need more quiescent states! */
		/*
		 * 行尾英文说明：配置不符、仍有阻塞 reader 或 CPU 位未清时，尚不能向父层
		 * 报告本节点完成；本函数只解锁并保留等待状态。
		 */
	}

	rnp->completedqs = rnp->gp_seq;
	rnp_p = rnp->parent;
	if (rnp_p == NULL) {
		/*
		 * Only one rcu_node structure in the tree, so don't
		 * try to report up to its nonexistent parent!
		 */
	/*
	 * 单节点树中当前节点就是根，不存在 parent；直接走根完成路径，避免解锁后访问
	 * 空父指针。
	 */
		rcu_report_qs_rsp(flags);
		return;
	}

	/* Report up the rest of the hierarchy, tracking current ->gp_seq. */
	/*
	 * 保存当前 GP 序号和本节点在父层的成员位，释放子锁后取得父锁，再由通用路径
	 * 继续传播；gps 防止换锁窗口跨入新 GP 后误清新一代位图。
	 */
	gps = rnp->gp_seq;
	mask = rnp->grpmask;
	raw_spin_unlock_rcu_node(rnp);	/* irqs remain disabled. */
	/* 行尾英文说明：只释放节点锁，IRQ 仍保持关闭，状态最终由 @flags 恢复。 */
	raw_spin_lock_rcu_node(rnp_p);	/* irqs already disabled. */
	/* 行尾英文说明：IRQ 已关闭，因此这里只获取父锁，不重复保存中断状态。 */
	rcu_report_qs_rnp(mask, rnp_p, gps, flags);
}

/*
 * Record a quiescent state for the specified CPU to that CPU's rcu_data
 * structure.  This must be called from the specified CPU.
 */
/*
 * rcu_report_qs_rdp() - 把本 CPU 已记录的 QS 提交给所属叶 rcu_node。
 * @rdp 必须为当前 CPU 且中断/抢占协议稳定；锁内核对 gp_seq 与 qsmask，清本 CPU 位，
 * 再按需要沿树传播。过期/重复 QS 只清本地标志，不影响新 GP。
 */
static void
rcu_report_qs_rdp(struct rcu_data *rdp)
{
	unsigned long flags;
	unsigned long mask;
	struct rcu_node *rnp;

	WARN_ON_ONCE(rdp->cpu != smp_processor_id());
	rnp = rdp->mynode;
	raw_spin_lock_irqsave_rcu_node(rnp, flags);
	if (rdp->cpu_no_qs.b.norm || rdp->gp_seq != rnp->gp_seq ||
	    rdp->gpwrap) {

		/*
		 * The grace period in which this quiescent state was
		 * recorded has ended, so don't report it upwards.
		 * We will instead need a new quiescent state that lies
		 * within the current grace period.
		 */
	/*
	 * 本地 QS 属于已经结束的 GP，或 rdp 尚未同步到叶节点当前序号；不能向上报告，
	 * 否则会把旧代 QS 用于新代。重新置 norm 表示必须在当前 GP 内再观察一次 QS。
	 */
		rdp->cpu_no_qs.b.norm = true;	/* need qs for new gp. */
		/* 行尾英文说明：把本地状态重置为“新 GP 仍欠一次 QS”。 */
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		return;
	}
	mask = rdp->grpmask;
	rdp->core_needs_qs = false;
	if ((rnp->qsmask & mask) == 0) {
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
	} else {
		/*
		 * This GP can't end until cpu checks in, so all of our
		 * callbacks can be processed during the next GP.
		 *
		 * NOCB kthreads have their own way to deal with that...
		 */
		/*
		 * 当前 GP 必须等待本 CPU，因此本地所有现有 callback 至少可以关联到下一 GP。
		 * 普通 CPU 在叶锁下加速分段；NOCB CPU 的 cblist 受 nocb_lock/对应 kthread
		 * 管理，不能在这里直接访问。
		 */
		if (!rcu_rdp_is_offloaded(rdp)) {
			/*
			 * The current GP has not yet ended, so it
			 * should not be possible for rcu_accelerate_cbs()
			 * to return true.  So complain, but don't awaken.
			 */
			/*
			 * 当前 GP 尚未完成且已覆盖这些 callback，正常情况下加速不应产生新的
			 * GP 唤醒请求；true 表示状态机不一致，报警但不在持叶锁路径中唤醒。
			 */
			WARN_ON_ONCE(rcu_accelerate_cbs(rnp, rdp));
		}

		rcu_disable_urgency_upon_qs(rdp);
		rcu_report_qs_rnp(mask, rnp, rnp->gp_seq, flags);
		/* ^^^ Released rnp->lock */
		/* 上方调用已经消费 flags 并释放 rnp->lock；此分支不得再次解锁。 */
	}
}

/*
 * Check to see if there is a new grace period of which this CPU
 * is not yet aware, and if so, set up local rcu_data state for it.
 * Otherwise, see if this CPU has just passed through its first
 * quiescent state for this grace period, and record that fact if so.
 */
/* rcu_check_quiescent_state() 先同步 GP 变化，再把当前代已观察 QS 交给 report 路径。 */
static void
rcu_check_quiescent_state(struct rcu_data *rdp)
{
	/* Check for grace-period ends and beginnings. */
	/*
	 * 阶段 1：先让 rdp 同步叶节点最新 gp_seq，推进完成 callback，并建立当前 GP
	 * 是否仍等待本 CPU；同步本身可能按需唤醒 GP kthread。
	 */
	note_gp_changes(rdp);

	/*
	 * Does this CPU still need to do its part for current grace period?
	 * If no, return and let the other CPUs do their part as well.
	 */
	/* 本 CPU 已不在 qsmask 中，无需重复上报；其余 CPU/reader 继续完成各自责任。 */
	if (!rdp->core_needs_qs)
		return;

	/*
	 * Was there a quiescent state since the beginning of the grace
	 * period? If no, then exit and wait for the next call.
	 */
	/* norm 仍为真表示尚未观察到本代 QS，等待后续 tick、context switch 或 EQS。 */
	if (rdp->cpu_no_qs.b.norm)
		return;

	/*
	 * Tell RCU we are done (but rcu_report_qs_rdp() will be the
	 * judge of that).
	 */
	/*
	 * 阶段 2：本地已观察 QS，提交时仍需在叶锁下复核 gp_seq 和 qsmask；
	 * rcu_report_qs_rdp() 才能裁决这份证据是否仍属于当前代。
	 */
	rcu_report_qs_rdp(rdp);
}

/* Return true if callback-invocation time limit exceeded. */
/* rcu_do_batch_check_time() 按 callback 数、时间预算和 resched 需求决定是否结束本批。 */
static bool rcu_do_batch_check_time(long count, long tlimit,
				    bool jlimit_check, unsigned long jlimit)
{
	// Invoke local_clock() only once per 32 consecutive callbacks.
	/*
	 * local_clock() 读取较贵，仅每连续 32 个 callback 检查一次纳秒预算；双重检查
	 * 配置还可由 jiffies 先筛选，兼顾限时准确性与 hot path 成本。
	 */
	return unlikely(tlimit) &&
	       (!likely(count & 31) ||
		(IS_ENABLED(CONFIG_RCU_DOUBLE_CHECK_CB_TIME) &&
		 jlimit_check && time_after(jiffies, jlimit))) &&
	       local_clock() >= tlimit;
}

/*
 * Invoke any RCU callbacks that have made it to the end of their grace
 * period.  Throttle as specified by rdp->blimit.
 */
/*
 * rcu_do_batch() - 从 @rdp segcblist 的 DONE 段摘取并调用一批成熟 callback。
 * RCU core/softirq/rcuc 上下文执行；先禁用列表修改并提取 batch，调用期间不持
 * rcu_node lock，callback 可释放其包含对象。受 blimit、时间和 need_resched 限制，
 * 未处理 DONE 留给下一轮；结束时更新长度/过载/懒回调统计并重新唤醒 core。
 */
static void rcu_do_batch(struct rcu_data *rdp)
{
	long bl;
	long count = 0;
	int div;
	bool __maybe_unused empty;
	unsigned long flags;
	unsigned long jlimit;
	bool jlimit_check = false;
	long pending;
	struct rcu_cblist rcl = RCU_CBLIST_INITIALIZER(rcl);
	struct rcu_head *rhp;
	long tlimit = 0;

	/* If no callbacks are ready, just return. */
	/* 快路径只发 start/end trace；NEXT/WAIT 段非空不等于已有可调用 DONE。 */
	if (!rcu_segcblist_ready_cbs(&rdp->cblist)) {
		trace_rcu_batch_start(rcu_state.name,
				      rcu_segcblist_n_cbs(&rdp->cblist), 0);
		trace_rcu_batch_end(rcu_state.name, 0,
				    !rcu_segcblist_empty(&rdp->cblist),
				    need_resched(), is_idle_task(current),
				    rcu_is_callbacks_kthread(rdp));
		return;
	}

	/*
	 * Extract the list of ready callbacks, disabling IRQs to prevent
	 * races with call_rcu() from interrupt handlers.  Leave the
	 * callback counts, as rcu_barrier() needs to be conservative.
	 *
	 * Callbacks execution is fully ordered against preceding grace period
	 * completion (materialized by rnp->gp_seq update) thanks to the
	 * smp_mb__after_unlock_lock() upon node locking required for callbacks
	 * advancing. In NOCB mode this ordering is then further relayed through
	 * the nocb locking that protects both callbacks advancing and extraction.
	 */
	/*
	 * 阶段 1：在 nocb/IRQ 锁下把 DONE 段摘到私有 rcl。callback 数暂不立即扣减，
	 * rcu_barrier() 宁可保守认为它仍在队列，也不能漏等已摘出但尚未调用的回调。
	 * node 解锁→再加锁的全屏障把 GP 完成发布传递到 callback 调用之前；nocb 模式
	 * 再通过自身锁链延续该顺序。
	 */
	rcu_nocb_lock_irqsave(rdp, flags);
	WARN_ON_ONCE(cpu_is_offline(smp_processor_id()));
	pending = rcu_segcblist_get_seglen(&rdp->cblist, RCU_DONE_TAIL);
	div = READ_ONCE(rcu_divisor);
	div = div < 0 ? 7 : div > sizeof(long) * 8 - 2 ? sizeof(long) * 8 - 2 : div;
	bl = max(rdp->blimit, pending >> div);
	/* backlog 越大 batch 越大，兼顾排空速度与 softirq 公平性。 */
	if ((in_serving_softirq() || rdp->rcu_cpu_kthread_status == RCU_KTHREAD_RUNNING) &&
	    (IS_ENABLED(CONFIG_RCU_DOUBLE_CHECK_CB_TIME) || unlikely(bl > 100))) {
		const long npj = NSEC_PER_SEC / HZ;
		long rrn = READ_ONCE(rcu_resched_ns);

		rrn = clamp(rrn, NSEC_PER_MSEC, NSEC_PER_SEC);
		tlimit = local_clock() + rrn;
		jlimit = jiffies + (rrn + npj + 1) / npj;
		jlimit_check = true;
	}
	trace_rcu_batch_start(rcu_state.name,
			      rcu_segcblist_n_cbs(&rdp->cblist), bl);
	rcu_segcblist_extract_done_cbs(&rdp->cblist, &rcl);
	if (rcu_rdp_is_offloaded(rdp))
		rdp->qlen_last_fqs_check = rcu_segcblist_n_cbs(&rdp->cblist);

	trace_rcu_segcb_stats(&rdp->cblist, TPS("SegCbDequeued"));
	rcu_nocb_unlock_irqrestore(rdp, flags);

	/* Invoke callbacks. */
	/*
	 * 阶段 2：离开列表锁逐个调用。先把 func 清零并从 debug 队列注销，回调随后可
	 * 释放包含 rcu_head 的对象，因此调用后不得再访问 rhp 字段。
	 */
	tick_dep_set_task(current, TICK_DEP_BIT_RCU);
	rhp = rcu_cblist_dequeue(&rcl);

	for (; rhp; rhp = rcu_cblist_dequeue(&rcl)) {
		rcu_callback_t f;

		count++;
		debug_rcu_head_unqueue(rhp);

		rcu_lock_acquire(&rcu_callback_map);
		trace_rcu_invoke_callback(rcu_state.name, rhp);

		f = rhp->func;
		debug_rcu_head_callback(rhp);
		WRITE_ONCE(rhp->func, (rcu_callback_t)0L);
		f(rhp);

		rcu_lock_release(&rcu_callback_map);

		/*
		 * Stop only if limit reached and CPU has something to do.
		 */
		/* softirq 只有达到 batch 限额且存在其他任务时才因数量退出，空闲 CPU 可多排空。 */
		if (in_serving_softirq()) {
			if (count >= bl && (need_resched() || !is_idle_task(current)))
				break;
			/*
			 * Make sure we don't spend too much time here and deprive other
			 * softirq vectors of CPU cycles.
			 */
			/*
			 * 即使 callback 数尚未达到动态 blimit，也要检查纳秒/jiffy 时间预算，
			 * 防止 RCU_SOFTIRQ 长时间占用 CPU 而饿死网络、定时器等其他 softirq。
			 */
			if (rcu_do_batch_check_time(count, tlimit, jlimit_check, jlimit))
				break;
		} else {
			// In rcuc/rcuoc context, so no worries about
			// depriving other softirq vectors of CPU cycles.
			/*
			 * rcuc/rcuoc 是独立 kthread，不占用 softirq 向量预算；可以临时启用 BH
			 * 并主动调度，而不必因其他 softirq 公平性提前停批。
			 */
			local_bh_enable();
			lockdep_assert_irqs_enabled();
			cond_resched_tasks_rcu_qs();
			lockdep_assert_irqs_enabled();
			local_bh_disable();
			/*
			 * kthread 不会饿死其他 softirq，但自身长时间运行会拖延 QS；临时开 BH、
			 * cond_resched 并仍用时间预算切批。
			 */
			// But rcuc kthreads can delay quiescent-state
			// reporting, so check time limits for them.
			/*
			 * 但 rcuc 自身长时间运行也会推迟本 CPU QS，因此仍需执行同一时间上限；
			 * 超限时保留 has_work，让线程下一轮继续。
			 */
			if (rdp->rcu_cpu_kthread_status == RCU_KTHREAD_RUNNING &&
			    rcu_do_batch_check_time(count, tlimit, jlimit_check, jlimit)) {
				rdp->rcu_cpu_has_work = 1;
				break;
			}
		}
	}

	rcu_nocb_lock_irqsave(rdp, flags);
	/* 阶段 3：重新加列表锁，把预算剩余 callback 放回 DONE 头部并精确扣除已调用数。 */
	rdp->n_cbs_invoked += count;
	trace_rcu_batch_end(rcu_state.name, count, !!rcl.head, need_resched(),
			    is_idle_task(current), rcu_is_callbacks_kthread(rdp));

	/* Update counts and requeue any remaining callbacks. */
	/*
	 * 把因预算退出而留在私有 rcl 的 callback 插回 DONE 头部，再只从总长度扣除已经
	 * 调用的 count；未调用 callback 的 ownership 重新回到 rdp->cblist。
	 */
	rcu_segcblist_insert_done_cbs(&rdp->cblist, &rcl);
	rcu_segcblist_add_len(&rdp->cblist, -count);

	/* Reinstate batch limit if we have worked down the excess. */
	/* backlog 降到 low watermark 后恢复默认 blimit，退出过载排空模式。 */
	count = rcu_segcblist_n_cbs(&rdp->cblist);
	if (rdp->blimit >= DEFAULT_MAX_RCU_BLIMIT && count <= qlowmark)
		rdp->blimit = blimit;

	/* Reset ->qlen_last_fqs_check trigger if enough CBs have drained. */
	/*
	 * callback 全排空时重置 FQS 水位和全局扫描快照；若虽未排空但已下降超过 qhimark，
	 * 也下移基准，形成高/低水位迟滞，避免每次 enqueue 都触发 FQS。
	 */
	if (count == 0 && rdp->qlen_last_fqs_check != 0) {
		rdp->qlen_last_fqs_check = 0;
		rdp->n_force_qs_snap = READ_ONCE(rcu_state.n_force_qs);
	} else if (count < rdp->qlen_last_fqs_check - qhimark)
		rdp->qlen_last_fqs_check = count;

	/*
	 * The following usually indicates a double call_rcu().  To track
	 * this down, try building with CONFIG_DEBUG_OBJECTS_RCU_HEAD=y.
	 */
	/*
	 * 下列不变量检查确保总长度、分段长度和 empty 标志一致；失配通常来自同一
	 * rcu_head 被重复 call_rcu() 导致链表损坏。启用
	 * CONFIG_DEBUG_OBJECTS_RCU_HEAD 可记录第一次入队位置。
	 */
	empty = rcu_segcblist_empty(&rdp->cblist);
	WARN_ON_ONCE(count == 0 && !empty);
	WARN_ON_ONCE(!IS_ENABLED(CONFIG_RCU_NOCB_CPU) &&
		     count != 0 && empty);
	WARN_ON_ONCE(count == 0 && rcu_segcblist_n_segment_cbs(&rdp->cblist) != 0);
	WARN_ON_ONCE(!empty && rcu_segcblist_n_segment_cbs(&rdp->cblist) == 0);

	rcu_nocb_unlock_irqrestore(rdp, flags);

	tick_dep_clear_task(current, TICK_DEP_BIT_RCU);
}

/*
 * This function is invoked from each scheduling-clock interrupt,
 * and checks to see if this CPU is in a non-context-switch quiescent
 * state, for example, user mode or idle loop.  It also schedules RCU
 * core processing.  If the current grace period has gone on too long,
 * it will ask the scheduler to manufacture a context switch for the sole
 * purpose of providing the needed quiescent state.
 */
/*
 * rcu_sched_clock_irq() - scheduler tick 上 RCU 的每 CPU 推进入口。
 * @user 表示 tick 是否打断用户/EQS，可据此直接记录 QS；同时检查 GP 变化、urgent
 * QS、callback 与 stall。IRQ 上下文不可睡眠，无返回。
 */
void rcu_sched_clock_irq(int user)
{
	unsigned long j;

	if (IS_ENABLED(CONFIG_PROVE_RCU)) {
		j = jiffies;
		WARN_ON_ONCE(time_before(j, __this_cpu_read(rcu_data.last_sched_clock)));
		__this_cpu_write(rcu_data.last_sched_clock, j);
	}
	trace_rcu_utilization(TPS("Start scheduler-tick"));
	lockdep_assert_irqs_disabled();
	raw_cpu_inc(rcu_data.ticks_this_gp);
	/* The load-acquire pairs with the store-release setting to true. */
	/*
	 * acquire 读取与 FQS 路径发布 urgent=true 的 release 写配对；一旦看见请求，
	 * 本 CPU 也必须看见此前发布的 rcu_need_heavy_qs 等附加状态。
	 */
	if (smp_load_acquire(this_cpu_ptr(&rcu_data.rcu_urgent_qs))) {
		/* Idle and userspace execution already are quiescent states. */
		/*
		 * tick 若打断 idle 或用户态，当前边界本身已经是 QS；只有持续运行内核代码
		 * 的 CPU 才需 set_need_resched_current() 制造上下文切换。
		 */
		if (!rcu_is_cpu_rrupt_from_idle() && !user)
			set_need_resched_current();
		__this_cpu_write(rcu_data.rcu_urgent_qs, false);
	}
	rcu_flavor_sched_clock_irq(user);
	if (rcu_pending(user))
		invoke_rcu_core();
	if (user || rcu_is_cpu_rrupt_from_idle())
		rcu_note_voluntary_context_switch(current);
	lockdep_assert_irqs_disabled();

	trace_rcu_utilization(TPS("End scheduler-tick"));
}

/*
 * Scan the leaf rcu_node structures.  For each structure on which all
 * CPUs have reported a quiescent state and on which there are tasks
 * blocking the current grace period, initiate RCU priority boosting.
 * Otherwise, invoke the specified function to check dyntick state for
 * each CPU that has not yet reported a quiescent state.
 */
/*
 * force_qs_rnp() - 遍历仍有 qsmask 的叶节点，对每 CPU 调 @f 采样/催促 QS。
 * 逐节点持锁，回调不可睡眠；只清有可靠证据的位，并沿树上报聚合结果。
 */
static void force_qs_rnp(int (*f)(struct rcu_data *rdp))
{
	int cpu;
	unsigned long flags;
	struct rcu_node *rnp;

	rcu_state.cbovld = rcu_state.cbovldnext;
	rcu_state.cbovldnext = false;
	rcu_for_each_leaf_node(rnp) {
		unsigned long mask = 0;
		unsigned long rsmask = 0;

		cond_resched_tasks_rcu_qs();
		raw_spin_lock_irqsave_rcu_node(rnp, flags);
		rcu_state.cbovldnext |= !!rnp->cbovldmask;
		if (rnp->qsmask == 0) {
			if (rcu_preempt_blocked_readers_cgp(rnp)) {
				/*
				 * No point in scanning bits because they
				 * are all zero.  But we might need to
				 * priority-boost blocked readers.
				 */
				/*
				 * CPU 位已经全清，无需逐位 dynticks 扫描；但被抢占的旧 reader 仍可
				 * 阻塞 GP，因此要启动优先级提升，帮助它尽快运行到 rcu_read_unlock()。
				 */
				rcu_initiate_boost(rnp, flags);
				/* rcu_initiate_boost() releases rnp->lock */
				/* 被调函数消费 @flags 并释放 rnp->lock，本分支直接 continue。 */
				continue;
			}
			raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
			continue;
		}
		for_each_leaf_node_cpu_mask(rnp, cpu, rnp->qsmask) {
			struct rcu_data *rdp;
			int ret;

			rdp = per_cpu_ptr(&rcu_data, cpu);
			ret = f(rdp);
			if (ret > 0) {
				mask |= rdp->grpmask;
				rcu_disable_urgency_upon_qs(rdp);
			}
			if (ret < 0)
				rsmask |= rdp->grpmask;
		}
		if (mask != 0) {
			/* Idle/offline CPUs, report (releases rnp->lock). */
			/*
			 * mask 中 CPU 已有 EQS/离线证据，批量清除叶节点等待位；被调函数会释放
			 * 当前锁，并在节点归零时继续向根传播。
			 */
			rcu_report_qs_rnp(mask, rnp, rnp->gp_seq, flags);
		} else {
			/* Nothing to do here, so just drop the lock. */
			/* 没有可靠 QS 证据时绝不猜测清位，只释放锁，随后按 rsmask 发送 resched。 */
			raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		}

		for_each_leaf_node_cpu_mask(rnp, cpu, rsmask)
			resched_cpu(cpu);
	}
}

/*
 * Force quiescent states on reluctant CPUs, and also detect which
 * CPUs are in dyntick-idle mode.
 */
/* rcu_force_quiescent_state() 请求 GP kthread 立即进行一轮 FQS；异步返回，不保证 GP 已完成。 */
void rcu_force_quiescent_state(void)
{
	unsigned long flags;
	bool ret;
	struct rcu_node *rnp;
	struct rcu_node *rnp_old = NULL;

	if (!rcu_gp_in_progress())
		return;
	/* Funnel through hierarchy to reduce memory contention. */
	/*
	 * 从本 CPU 叶节点沿 fqslock 向根漏斗竞争；每次只保留上一层锁。只要发现已有
	 * FQS 标志或某层锁竞争，就说明别的 CPU 正在代办，可立即退出以避免根 cacheline
	 * 惊群。
	 */
	rnp = raw_cpu_read(rcu_data.mynode);
	for (; rnp != NULL; rnp = rnp->parent) {
		ret = (READ_ONCE(rcu_state.gp_flags) & RCU_GP_FLAG_FQS) ||
		       !raw_spin_trylock(&rnp->fqslock);
		if (rnp_old != NULL)
			raw_spin_unlock(&rnp_old->fqslock);
		if (ret)
			return;
		rnp_old = rnp;
	}
	/* rnp_old == rcu_get_root(), rnp == NULL. */
	/* 英文说明：漏斗遍历结束时，rnp_old 持有根节点，rnp 已越过根成为 NULL。 */

	/* Reached the root of the rcu_node tree, acquire lock. */
	/*
	 * 只有独占走到根的请求者才取得根节点主锁并发布 FQS。先取主锁、再释放根
	 * fqslock，防止另一个漏斗请求在标志发布前穿过。
	 */
	raw_spin_lock_irqsave_rcu_node(rnp_old, flags);
	raw_spin_unlock(&rnp_old->fqslock);
	if (READ_ONCE(rcu_state.gp_flags) & RCU_GP_FLAG_FQS) {
		raw_spin_unlock_irqrestore_rcu_node(rnp_old, flags);
		return;  /* Someone beat us to it. */
		/* 行尾英文说明：在换锁窗口已有请求者置位，本次无需重复唤醒 GP 线程。 */
	}
	WRITE_ONCE(rcu_state.gp_flags, rcu_state.gp_flags | RCU_GP_FLAG_FQS);
	raw_spin_unlock_irqrestore_rcu_node(rnp_old, flags);
	rcu_gp_kthread_wake();
}
EXPORT_SYMBOL_GPL(rcu_force_quiescent_state);

// Workqueue handler for an RCU reader for kernels enforcing struct RCU
// grace periods.
/* strict_work_handler() 在严格 GP 配置用 workqueue 制造额外调度/QS 边界。 */
static void strict_work_handler(struct work_struct *work)
{
	rcu_read_lock();
	rcu_read_unlock();
}

/* Perform RCU core processing work for the current CPU.  */
/*
 * rcu_core() - 当前 CPU callback/GP 维护核心。
 * 检查本地 QS、同步 GP 变化、推进 callback 段、处理过载并调用 rcu_do_batch()。
 * 通常在 RCU_SOFTIRQ，RT/配置下可由 rcuc kthread；不可在持 rcu_node lock 时调用
 * callback。返回无，latent_entropy 只用于熵采集标注。
 */
static __latent_entropy void rcu_core(void)
{
	struct rcu_data *rdp = raw_cpu_ptr(&rcu_data);
	struct rcu_node *rnp = rdp->mynode;

	if (cpu_is_offline(smp_processor_id()))
		return;
	trace_rcu_utilization(TPS("Start RCU core"));
	WARN_ON_ONCE(!rdp->beenonline);

	/* Report any deferred quiescent states if preemption enabled. */
	/*
	 * 阶段 1：在允许抢占时直接提交 current 的 deferred QS；若仍处于不可抢占区，
	 * 只设置 need_resched，让安全边界稍后完成上报。
	 */
	if (IS_ENABLED(CONFIG_PREEMPT_COUNT) && (!(preempt_count() & PREEMPT_MASK))) {
		rcu_preempt_deferred_qs(current);
	} else if (rcu_preempt_need_deferred_qs(current)) {
		guard(irqsave)();
		set_need_resched_current();
	}

	/* Update RCU state based on any recent quiescent states. */
	/* 阶段 2：同步本 CPU GP 代际并把已观察到的 QS 提交到叶节点。 */
	rcu_check_quiescent_state(rdp);

	/* No grace period and unregistered callbacks? */
	/*
	 * 阶段 3：GP 空闲但 NEXT 段仍有普通 callback 时，在 IRQ guard 内将其关联到目标
	 * GP；offloaded CPU 由 nocb 线程完成同一职责。
	 */
	if (!rcu_gp_in_progress() &&
	    rcu_segcblist_is_enabled(&rdp->cblist) && !rcu_rdp_is_offloaded(rdp)) {
		guard(irqsave)();
		if (!rcu_segcblist_restempty(&rdp->cblist, RCU_NEXT_READY_TAIL))
			rcu_accelerate_cbs_unlocked(rnp, rdp);
	}

	rcu_check_gp_start_stall(rnp, rdp, rcu_jiffies_till_stall_check());

	/* If there are callbacks ready, invoke them. */
	/*
	 * 阶段 4：调度器完全可用后才调用 DONE callback；调用时不持 rcu_node 锁。
	 * offloaded 列表由 rcuoc 执行，避免两个消费者竞争同一 cblist。
	 */
	if (!rcu_rdp_is_offloaded(rdp) && rcu_segcblist_ready_cbs(&rdp->cblist) &&
	    likely(READ_ONCE(rcu_scheduler_fully_active))) {
		rcu_do_batch(rdp);
		/* Re-invoke RCU core processing if there are callbacks remaining. */
		/* 批次因数量/时间预算退出时再次排队 core，保证剩余 DONE 最终得到执行。 */
		if (rcu_segcblist_ready_cbs(&rdp->cblist))
			invoke_rcu_core();
	}

	/* Do any needed deferred wakeups of rcuo kthreads. */
	/* 阶段 5：完成本地处理后兑现此前延迟的 rcuo 唤醒，避免在持 nocb 锁时唤醒竞争。 */
	do_nocb_deferred_wakeup(rdp);
	trace_rcu_utilization(TPS("End RCU core"));

	// If strict GPs, schedule an RCU reader in a clean environment.
	/*
	 * 严格模式额外把一个短读侧 work 排到本 CPU 的干净 workqueue 上下文，制造明确
	 * 的读锁进入/退出边界，帮助发现过短 GP；普通模式跳过该测试成本。
	 */
	if (IS_ENABLED(CONFIG_RCU_STRICT_GRACE_PERIOD))
		queue_work_on(rdp->cpu, rcu_gp_wq, &rdp->strict_work);
}

/* rcu_core_si() 是 RCU_SOFTIRQ 包装，调用 rcu_core()；softirq 上下文不可睡眠。 */
static void rcu_core_si(void)
{
	rcu_core();
}

/* rcu_wake_cond() 仅在线程状态允许且 task 非空时唤醒，避免无意义跨 CPU IPI。 */
static void rcu_wake_cond(struct task_struct *t, int status)
{
	/*
	 * If the thread is yielding, only wake it when this
	 * is invoked from idle
	 */
	/*
	 * yielding 状态表示线程主动让出 CPU，普通调用不应立刻把它唤回形成忙循环；
	 * 只有 idle task 发现有工作时才打破 yielding，以免 CPU 空闲却留下 callback。
	 */
	if (t && (status != RCU_KTHREAD_YIELDING || is_idle_task(current)))
		wake_up_process(t);
}

/*
 * invoke_rcu_core_kthread() 设置当前 CPU rcuc 工作状态并条件唤醒 per-CPU 线程。
 * 无参数、无返回；函数自行关闭本地 IRQ来保护本 CPU work/status 状态。它把 work
 * 置为 1，并按线程状态决定 wake；不执行 callback，线程指针只借用且可能尚未建立。
 */
static void invoke_rcu_core_kthread(void)
{
	struct task_struct *t;
	unsigned long flags;

	local_irq_save(flags);
	__this_cpu_write(rcu_data.rcu_cpu_has_work, 1);
	t = __this_cpu_read(rcu_data.rcu_cpu_kthread_task);
	if (t != NULL && t != current)
		rcu_wake_cond(t, __this_cpu_read(rcu_data.rcu_cpu_kthread_status));
	local_irq_restore(flags);
}

/*
 * Wake up this CPU's rcuc kthread to do RCU core processing.
 */
/*
 * invoke_rcu_core() 按 use_softirq 选择 raise RCU_SOFTIRQ 或唤醒 rcuc kthread。
 * 无参数、无返回，可从不可睡眠路径调用；当前 CPU 离线时直接忽略。副作用只是安排
 * 一次本 CPU core 执行，允许重复合并，不保证函数返回前 callback 已被处理。
 */
static void invoke_rcu_core(void)
{
	if (!cpu_online(smp_processor_id()))
		return;
	if (use_softirq)
		raise_softirq(RCU_SOFTIRQ);
	else
		invoke_rcu_core_kthread();
}

/*
 * rcu_cpu_kthread_park() 是 smpboot park 回调；@cpu 指定被停用的 per-CPU 线程。
 * 无返回，只把该 CPU 状态置 OFFCPU；callback 队列的迁移和所有权由热插拔路径负责。
 */
static void rcu_cpu_kthread_park(unsigned int cpu)
{
	per_cpu(rcu_data.rcu_cpu_kthread_status, cpu) = RCU_KTHREAD_OFFCPU;
}

/*
 * rcu_cpu_kthread_should_run() 是 smpboot 的无副作用谓词；@cpu 由框架传入，但
 * 线程绑定保证直接读取 this_cpu work 标志。非零表示应运行一次，零表示继续休眠；
 * 它不清标志，真正消费发生在 rcu_cpu_kthread()。
 */
static int rcu_cpu_kthread_should_run(unsigned int cpu)
{
	return __this_cpu_read(rcu_data.rcu_cpu_has_work);
}

/*
 * Per-CPU kernel thread that invokes RCU callbacks.  This replaces
 * the RCU softirq used in configurations of RCU that do not support RCU
 * priority boosting.
 */
/* rcu_cpu_kthread() 绑定 CPU 执行 rcu_core 并维护等待/运行状态；可调度。 */
static void rcu_cpu_kthread(unsigned int cpu)
{
	unsigned int *statusp = this_cpu_ptr(&rcu_data.rcu_cpu_kthread_status);
	char work, *workp = this_cpu_ptr(&rcu_data.rcu_cpu_has_work);
	unsigned long *j = this_cpu_ptr(&rcu_data.rcuc_activity);
	int spincnt;

	trace_rcu_utilization(TPS("Start CPU kthread@rcu_run"));
	for (spincnt = 0; spincnt < 10; spincnt++) {
		WRITE_ONCE(*j, jiffies);
		local_bh_disable();
		*statusp = RCU_KTHREAD_RUNNING;
		local_irq_disable();
		work = *workp;
		WRITE_ONCE(*workp, 0);
		local_irq_enable();
		if (work)
			rcu_core();
		local_bh_enable();
		if (!READ_ONCE(*workp)) {
			trace_rcu_utilization(TPS("End CPU kthread@rcu_wait"));
			*statusp = RCU_KTHREAD_WAITING;
			return;
		}
	}
	*statusp = RCU_KTHREAD_YIELDING;
	trace_rcu_utilization(TPS("Start CPU kthread@rcu_yield"));
	schedule_timeout_idle(2);
	trace_rcu_utilization(TPS("End CPU kthread@rcu_yield"));
	*statusp = RCU_KTHREAD_WAITING;
	WRITE_ONCE(*j, jiffies);
}

static struct smp_hotplug_thread rcu_cpu_thread_spec = {
	.store			= &rcu_data.rcu_cpu_kthread_task,
	.thread_should_run	= rcu_cpu_kthread_should_run,
	.thread_fn		= rcu_cpu_kthread,
	.thread_comm		= "rcuc/%u",
	.setup			= rcu_cpu_kthread_setup,
	.park			= rcu_cpu_kthread_park,
};

/*
 * Spawn per-CPU RCU core processing kthreads.
 */
/* rcu_spawn_core_kthreads() 注册 per-CPU rcuc smpboot 线程；成功 0，失败负 errno。 */
/*
 * 修正说明：当前实现无论 use_softirq 快速路径还是注册失败都返回 0；失败只通过
 * WARN_ONCE 报告，并意味着后续 OOM/RCU callback 无法推进风险，不返回负 errno。
 * 入参：无；__init 进程上下文可睡眠，副作用是注册所有 possible CPU 的 rcuc 线程。
 */
static int __init rcu_spawn_core_kthreads(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		per_cpu(rcu_data.rcu_cpu_has_work, cpu) = 0;
	if (use_softirq)
		return 0;
	WARN_ONCE(smpboot_register_percpu_thread(&rcu_cpu_thread_spec),
		  "%s: Could not start rcuc kthread, OOM is now expected behavior\n", __func__);
	return 0;
}

/*
 * rcutree_enqueue() - 把已初始化 rcu_head 追加到当前 CPU segcblist 的 NEXT 段。
 * @rdp 当前 CPU 列表；@head ownership 转给 RCU；@func 在 GP 后调用。返回无，
 * 调用者在 callback 前不得复用/释放 head 所在对象。
 */
static void rcutree_enqueue(struct rcu_data *rdp, struct rcu_head *head, rcu_callback_t func)
{
	rcu_segcblist_enqueue(&rdp->cblist, head);
	trace_rcu_callback(rcu_state.name, head,
			   rcu_segcblist_n_cbs(&rdp->cblist));
	trace_rcu_segcb_stats(&rdp->cblist, TPS("SegCBQueued"));
}

/*
 * Handle any core-RCU processing required by a call_rcu() invocation.
 */
/*
 * call_rcu_core() 在 enqueue 后按列表长度/GP 状态加速 callback 并唤醒本 CPU core。
 *
 * 补充契约：@rdp 是关 IRQ 后固定的当前 CPU 状态，@head ownership 已转给 RCU，
 * @func 是其回调，@flags 保存 call_rcu() 入口 IRQ 状态。无返回；先入队，再按 EQS、
 * offline 和积压水位决定是否安排 core/FQS。函数不调用 @func，也不归还 @head。
 */
static void call_rcu_core(struct rcu_data *rdp, struct rcu_head *head,
			  rcu_callback_t func, unsigned long flags)
{
	rcutree_enqueue(rdp, head, func);
	/*
	 * If called from an extended quiescent state, invoke the RCU
	 * core in order to force a re-evaluation of RCU's idleness.
	 */
	/*
	 * 若从 EQS 注册 callback，CPU 原本可能被 RCU 当作无需 core 工作；显式唤醒使
	 * idleness 判定重新读取新队列，否则 callback 可能一直等到下一次偶然中断。
	 */
	if (!rcu_is_watching())
		invoke_rcu_core();

	/* If interrupts were disabled or CPU offline, don't invoke RCU core. */
	/*
	 * 调用前 IRQ 已关闭或 CPU 已离线时，不在这里触发本地 core：前者由恢复 IRQ 后
	 * 的正常路径处理，后者由热插拔/NOCB 迁移路径接管；callback 已经安全入队。
	 */
	if (irqs_disabled_flags(flags) || cpu_is_offline(smp_processor_id()))
		return;

	/*
	 * Force the grace period if too many callbacks or too long waiting.
	 * Enforce hysteresis, and don't invoke rcu_force_quiescent_state()
	 * if some other CPU has recently done so.  Also, don't bother
	 * invoking rcu_force_quiescent_state() if the newly enqueued callback
	 * is the only one waiting for a grace period to complete.
	 */
	/*
	 * backlog 相对上次 FQS 基准增长超过 qhimark 才介入，形成迟滞。先同步可能已完成
	 * 的 GP；若当前无 GP 则为 callback 请求新 GP，否则提升 blimit 并在没有其他 CPU
	 * 最近执行过 FQS、且队列不只含刚入队 callback 时催促当前 GP。这样避免所有 CPU
	 * 同时敲打 GP 线程，也避免为单个正常等待者支付强制扫描成本。
	 */
	if (unlikely(rcu_segcblist_n_cbs(&rdp->cblist) >
		     rdp->qlen_last_fqs_check + qhimark)) {

		/* Are we ignoring a completed grace period? */
		/* 先消费叶节点已发布的结束态，避免把已成熟 callback 误算为长期积压。 */
		note_gp_changes(rdp);

		/* Start a new grace period if one not already started. */
		/* 无在途 GP 时，给本地 callback 分代并沿树发布 INIT 请求。 */
		if (!rcu_gp_in_progress()) {
			rcu_accelerate_cbs_unlocked(rdp->mynode, rdp);
		} else {
			/* Give the grace period a kick. */
			/* 在途 GP 下放宽 batch 上限并按全局 FQS 快照去重强制扫描。 */
			rdp->blimit = DEFAULT_MAX_RCU_BLIMIT;
			if (READ_ONCE(rcu_state.n_force_qs) == rdp->n_force_qs_snap &&
			    rcu_segcblist_first_pend_cb(&rdp->cblist) != head)
				rcu_force_quiescent_state();
			rdp->n_force_qs_snap = READ_ONCE(rcu_state.n_force_qs);
			rdp->qlen_last_fqs_check = rcu_segcblist_n_cbs(&rdp->cblist);
		}
	}
}

/*
 * RCU callback function to leak a callback.
 */
/* rcu_leak_callback() 是调试故意泄漏回调：不释放对象，用于捕获非法 callback 编码。 */
static void rcu_leak_callback(struct rcu_head *rhp)
{
}

/*
 * Check and if necessary update the leaf rcu_node structure's
 * ->cbovldmask bit corresponding to the current CPU based on that CPU's
 * number of queued RCU callbacks.  The caller must hold the leaf rcu_node
 * structure's ->lock.
 */
/*
 * check_cb_ovld_locked() 在 rnp lock 下更新当前 CPU 对应的 callback 过载位。
 * @rdp 与 @rnp 必须匹配，调用者持叶节点 raw lock 并保护 cblist；无返回。阈值有效
 * 时依据总 callback 数置/清 grpmask，副作用供 FQS/全局压力聚合使用。
 */
static void check_cb_ovld_locked(struct rcu_data *rdp, struct rcu_node *rnp)
{
	raw_lockdep_assert_held_rcu_node(rnp);
	if (qovld_calc <= 0)
		return; // Early boot and wildcard value set.
	/*
	 * 行尾英文说明：启动早期阈值仍是非正通配哨兵，此时 node/cblist 初始化未完，
	 * 不取得额外锁也不发布过载状态。
	 */
	if (rcu_segcblist_n_cbs(&rdp->cblist) >= qovld_calc)
		WRITE_ONCE(rnp->cbovldmask, rnp->cbovldmask | rdp->grpmask);
	else
		WRITE_ONCE(rnp->cbovldmask, rnp->cbovldmask & ~rdp->grpmask);
}

/*
 * Check and if necessary update the leaf rcu_node structure's
 * ->cbovldmask bit corresponding to the current CPU based on that CPU's
 * number of queued RCU callbacks.  No locks need be held, but the
 * caller must have disabled interrupts.
 *
 * Note that this function ignores the possibility that there are a lot
 * of callbacks all of which have already seen the end of their respective
 * grace periods.  This omission is due to the need for no-CBs CPUs to
 * be holding ->nocb_lock to do this check, which is too heavy for a
 * common-case operation.
 */
/*
 * check_cb_ovld() 是自行取叶节点锁的过载检查包装。
 * @rdp 是当前 CPU 的借用状态，调用者必须已关闭本地 IRQ以稳定 cblist；无返回。
 * 无锁快照已一致时快速退出，否则取 @rdp->mynode 锁并复核更新，允许并发导致一次
 * 保守的延后而不影响 callback 正确性。
 */
static void check_cb_ovld(struct rcu_data *rdp)
{
	struct rcu_node *const rnp = rdp->mynode;

	if (qovld_calc <= 0 ||
	    ((rcu_segcblist_n_cbs(&rdp->cblist) >= qovld_calc) ==
	     !!(READ_ONCE(rnp->cbovldmask) & rdp->grpmask)))
		return; // Early boot wildcard value or already set correctly.
	/*
	 * 行尾英文说明：阈值尚未初始化，或无锁快照显示当前 bit 已与队列水位一致时，
	 * 无需争用叶锁；真正修改仍在 check_cb_ovld_locked() 中复核。
	 */
	raw_spin_lock_rcu_node(rnp);
	check_cb_ovld_locked(rdp, rnp);
	raw_spin_unlock_rcu_node(rnp);
}

/*
 * __call_rcu_common() - call_rcu 系列的公共发布入口。
 * @head 由调用者提供且未在队列；@func 非空回调；@lazy_in 指示可延后的懒回调。
 * 可从 IRQ/NMI 允许的 API 上下文调用，不睡眠。初始化 head、选择当前 rdp，处理
 * nocb/offline/早启例外后入 segcblist，并按需请求 GP/core。返回无，ownership
 * 直到 callback 执行才归还。
 */
static void
__call_rcu_common(struct rcu_head *head, rcu_callback_t func, bool lazy_in)
{
	static atomic_t doublefrees;
	unsigned long flags;
	bool lazy;
	struct rcu_data *rdp;

	/* Misaligned rcu_head! */
	/* 阶段 1：校验 head 对齐与 func。低位可能用于 callback 编码，对齐错误会破坏链。 */
	WARN_ON_ONCE((unsigned long)head & (sizeof(void *) - 1));

	/* Avoid NULL dereference if callback is NULL. */
	/*
	 * func 为 NULL 无法形成可调用 callback；报警并在取得任何队列 ownership 前返回，
	 * head 仍归调用者，不会被 RCU 访问。
	 */
	if (WARN_ON_ONCE(!func))
		return;

	if (debug_rcu_head_queue(head)) {
		/*
		 * Probable double call_rcu(), so leak the callback.
		 * Use rcu:rcu_callback trace event to find the previous
		 * time callback was passed to call_rcu().
		 */
		/*
		 * 同一 head 二次入队会让两条链共享 next，最终双重释放/链损坏。无法安全撤销
		 * 旧 ownership，因此把新请求改成泄漏回调并保留证据，宁可泄漏也不 UAF。
		 */
		if (atomic_inc_return(&doublefrees) < 4) {
			pr_err("%s(): Double-freed CB %p->%pS()!!!  ", __func__, head, head->func);
			mem_dump_obj(head);
		}
		WRITE_ONCE(head->func, rcu_leak_callback);
		return;
	}
	head->func = func;
	head->next = NULL;
	kasan_record_aux_stack(head);
	/* 阶段 2：初始化链节点并记录异步发布栈；从此 head ownership 交给 RCU。 */

	local_irq_save(flags);
	rdp = this_cpu_ptr(&rcu_data);
	/*
	 * 关本地 IRQ 固定 this_cpu rdp，并防止 IRQ 中 call_rcu() 同时修改同一 segcblist；
	 * CPU offline 调用属于错误，早启 disabled-list 是唯一受控例外。
	 */
	RCU_LOCKDEP_WARN(!rcu_rdp_cpu_online(rdp), "Callback enqueued on offline CPU!");

	lazy = lazy_in && !rcu_async_should_hurry();

	/* Add the callback to our list. */
	/*
	 * 阶段 3a：正常运行期 cblist 必须已启用。disabled 只允许出现在极早启动或错误的
	 * offline 调用；早启动且列表为空时就地初始化，使 boot callback 不丢失。
	 */
	if (unlikely(!rcu_segcblist_is_enabled(&rdp->cblist))) {
		// This can trigger due to call_rcu() from offline CPU:
		/*
		 * disabled 列表可能来自错误的 offline CPU call_rcu()；调度器运行后对此报警，
		 * 因为离线 CPU 的 callback 应由迁移/NOCB 路径接管。
		 */
		WARN_ON_ONCE(rcu_scheduler_active != RCU_SCHEDULER_INACTIVE);
		WARN_ON_ONCE(!rcu_is_watching());
		// Very early boot, before rcu_init().  Initialize if needed
		// and then drop through to queue the callback.
		/*
		 * 唯一正常例外是 rcu_init() 之前的 boot callback：若列表仍空就就地初始化，
		 * 然后继续普通入队，确保早期请求在 RCU 完整启动后仍会执行。
		 */
		if (rcu_segcblist_empty(&rdp->cblist))
			rcu_segcblist_init(&rdp->cblist);
	}
	/* 阶段 3：选择 lazy 策略并建立/核对本地列表；过载状态决定是否加速 GP/FQS。 */

	check_cb_ovld(rdp);

	if (unlikely(rcu_rdp_is_offloaded(rdp)))
		/* nocb CPU 把 ownership 交给 rcuog/rcuoc 锁协议，减少本 CPU 干扰。 */
		call_rcu_nocb(rdp, head, func, flags, lazy);
	else
		/* 普通 CPU 直接入本地 segcblist，并按需唤醒 core/GP。 */
		call_rcu_core(rdp, head, func, flags);
	local_irq_restore(flags);
}

#ifdef CONFIG_RCU_LAZY
static bool enable_rcu_lazy __read_mostly = !IS_ENABLED(CONFIG_RCU_LAZY_DEFAULT_OFF);
module_param(enable_rcu_lazy, bool, 0444);

/**
 * call_rcu_hurry() - Queue RCU callback for invocation after grace period, and
 * flush all lazy callbacks (including the new one) to the main ->cblist while
 * doing so.
 *
 * @head: structure to be used for queueing the RCU updates.
 * @func: actual callback function to be invoked after the grace period
 *
 * The callback function will be invoked some time after a full grace
 * period elapses, in other words after all pre-existing RCU read-side
 * critical sections have completed.
 *
 * Use this API instead of call_rcu() if you don't want the callback to be
 * delayed for very long periods of time, which can happen on systems without
 * memory pressure and on systems which are lightly loaded or mostly idle.
 * This function will cause callbacks to be invoked sooner than later at the
 * expense of extra power. Other than that, this function is identical to, and
 * reuses call_rcu()'s logic. Refer to call_rcu() for more details about memory
 * ordering and other functionality.
 */
/*
 * call_rcu_hurry() - 以非 lazy 方式排 callback，要求尽快启动/推进 GP。
 * ownership 与 call_rcu 相同；“hurry”只影响批处理/节能延迟，不绕过宽限期。
 */
void call_rcu_hurry(struct rcu_head *head, rcu_callback_t func)
{
	__call_rcu_common(head, func, false);
}
EXPORT_SYMBOL_GPL(call_rcu_hurry);
#else
#define enable_rcu_lazy		false
#endif

/**
 * call_rcu() - Queue an RCU callback for invocation after a grace period.
 * By default the callbacks are 'lazy' and are kept hidden from the main
 * ->cblist to prevent starting of grace periods too soon.
 * If you desire grace periods to start very soon, use call_rcu_hurry().
 *
 * @head: structure to be used for queueing the RCU updates.
 * @func: actual callback function to be invoked after the grace period
 *
 * The callback function will be invoked some time after a full grace
 * period elapses, in other words after all pre-existing RCU read-side
 * critical sections have completed.  However, the callback function
 * might well execute concurrently with RCU read-side critical sections
 * that started after call_rcu() was invoked.
 *
 * It is perfectly legal to repost an RCU callback, potentially with
 * a different callback function, from within its callback function.
 * The specified function will be invoked after another full grace period
 * has elapsed.  This use case is similar in form to the common practice
 * of reposting a timer from within its own handler.
 *
 * RCU read-side critical sections are delimited by rcu_read_lock()
 * and rcu_read_unlock(), and may be nested.  In addition, but only in
 * v5.0 and later, regions of code across which interrupts, preemption,
 * or softirqs have been disabled also serve as RCU read-side critical
 * sections.  This includes hardware interrupt handlers, softirq handlers,
 * and NMI handlers.
 *
 * Note that all CPUs must agree that the grace period extended beyond
 * all pre-existing RCU read-side critical section.  On systems with more
 * than one CPU, this means that when "func()" is invoked, each CPU is
 * guaranteed to have executed a full memory barrier since the end of its
 * last RCU read-side critical section whose beginning preceded the call
 * to call_rcu().  It also means that each CPU executing an RCU read-side
 * critical section that continues beyond the start of "func()" must have
 * executed a memory barrier after the call_rcu() but before the beginning
 * of that RCU read-side critical section.  Note that these guarantees
 * include CPUs that are offline, idle, or executing in user mode, as
 * well as CPUs that are executing in the kernel.
 *
 * Furthermore, if CPU A invoked call_rcu() and CPU B invoked the
 * resulting RCU callback function "func()", then both CPU A and CPU B are
 * guaranteed to execute a full memory barrier during the time interval
 * between the call to call_rcu() and the invocation of "func()" -- even
 * if CPU A and CPU B are the same CPU (but again only if the system has
 * more than one CPU).
 *
 * Implementation of these memory-ordering guarantees is described here:
 * Documentation/RCU/Design/Memory-Ordering/Tree-RCU-Memory-Ordering.rst.
 *
 * Specific to call_rcu() (as opposed to the other call_rcu*() functions),
 * in kernels built with CONFIG_RCU_LAZY=y, call_rcu() might delay for many
 * seconds before starting the grace period needed by the corresponding
 * callback.  This delay can significantly improve energy-efficiency
 * on low-utilization battery-powered devices.  To avoid this delay,
 * in latency-sensitive kernel code, use call_rcu_hurry().
 */
/*
 * call_rcu() - 在一个完整常规 RCU 宽限期后调用 @func(@head)。
 * 调用立即返回且不睡眠；成功无错误返回。@head 所在对象在回调前必须保持分配，
 * 回调通常承担最终释放。API 只等待调用前已存在的 RCU 读者，不等待之后开始者。
 */
void call_rcu(struct rcu_head *head, rcu_callback_t func)
{
	__call_rcu_common(head, func, enable_rcu_lazy);
}
EXPORT_SYMBOL_GPL(call_rcu);

/*
 * During early boot, any blocking grace-period wait automatically
 * implies a grace period.
 *
 * Later on, this could in theory be the case for kernels built with
 * CONFIG_SMP=y && CONFIG_PREEMPTION=y running on a single CPU, but this
 * is not a common case.  Furthermore, this optimization would cause
 * the rcu_gp_oldstate structure to expand by 50%, so this potential
 * grace-period optimization is ignored once the scheduler is running.
 * 判断当前上下文是否天然构成一个完整的宽限期，从而允许 synchronize_rcu()
 * 绕过真正的宽限期流程直接返回。
 *
 * 为什么早期启动阶段"阻塞即宽限期"？
 *   RCU 宽限期的本质是"等待所有已开始的 RCU 读临界区结束"。在调度器启动
 *   之前（INACTIVE 阶段），系统只有一个执行流，不存在任何并发读者，因此
 *   调用者一旦能执行到 synchronize_rcu()，就意味着不可能有任何读临界区还
 *   在进行中——宽限期条件已经天然满足，无需等待。
 *
 * 为什么 SMP+PREEMPTION 单 CPU 的情况不做同样优化？
 *   理论上单 CPU 运行时也满足上述条件，但实现这个优化需要在
 *   rcu_gp_oldstate 结构中多存一个状态位，导致结构体膨胀 50%。
 *   这是个极少出现的场景，性价比不高，所以故意忽略。
 */
/* rcu_blocking_is_gp() 判断启动/单 CPU 等环境中阻塞同步是否可用本地屏障视作 GP。 */
static int rcu_blocking_is_gp(void)
{
	if (rcu_scheduler_active != RCU_SCHEDULER_INACTIVE) {
		might_sleep();
		return false;
	}
	return true;
}

/*
 * Helper function for the synchronize_rcu() API.
 * synchronize_rcu() 在非 expedited 模式下的真正实现。
 *
 * 整体流程：把当前调用者挂在 GP kthread 的完成队列里，等 GP kthread 跑完
 * 一轮宽限期后唤醒所有等待者。这样多个并发的 synchronize_rcu() 调用可以
 * 共享同一个宽限期，而不是每人各自跑一轮，大幅降低开销。
 *
 * 快慢路径选择（rcu_normal_wake_from_gp / rcu_sr_normal_latched）：
 *   正常情况（快路径）：通过 llist + completion 批量排队，GP kthread 跑完后
 *   统一唤醒所有等待者，吞吐量高。
 *
 *   两种情况退回慢路径（wait_rcu_gp(call_rcu_hurry)）：
 *   1. rcu_normal_wake_from_gp < 1：功能被管理员/模块参数显式关闭。
 *   2. rcu_sr_normal_latched == 1：在飞途径中的请求数已超过阈值
 *      RCU_SR_NORMAL_LATCH_THR（64），说明系统正处于宽限期高压状态，
 *      快路径队列已经拥堵。退回慢路径可以独立提交一个 call_rcu 回调，
 *      避免新请求继续堆积导致延迟恶化。
 *   慢路径通过 call_rcu 提交一个回调，回调触发时意味着宽限期已过，
 *   再通过栈上的 completion 唤醒调用者。
 */
/*
 * synchronize_rcu_normal() - 登记栈上同步请求并睡眠，直到某轮常规 GP 完成。
 * 请求通过 llist 批量并入 GP；等待者保持 completion 对象存活。返回无。
 */
static void synchronize_rcu_normal(void)
{
	struct rcu_synchronize rs;

	init_rcu_head_on_stack(&rs.head);
	trace_rcu_sr_normal(rcu_state.name, &rs.head, TPS("request"));

	if (READ_ONCE(rcu_normal_wake_from_gp) < 1 ||
			READ_ONCE(rcu_sr_normal_latched)) {
		wait_rcu_gp(call_rcu_hurry);
		goto trace_complete_out;
	}

	init_completion(&rs.completion);

	/*
	 * This code might be preempted, therefore take a GP
	 * snapshot before adding a request.
	 * 在加入队列之前先抓一次 GP 快照（仅 CONFIG_PROVE_RCU）。
	 * 此处可能被抢占，如果在快照和入队之间发生了 GP 推进，
	 * 后续 rcu_sr_normal_complete() 中的 poll_state_synchronize_rcu_full()
	 * 会用这个快照验证"返回时确实经历了完整宽限期"，而不是误判为已完成。
	 */
	if (IS_ENABLED(CONFIG_PROVE_RCU))
		get_state_synchronize_rcu_full(&rs.oldstate);

	/* 将本次请求加入全局 llist，GP kthread 在宽限期结束时批量唤醒。 */
	rcu_sr_normal_add_req(&rs);

	/* Kick a GP and start waiting. */
	/*
	 * 踢一脚 GP kthread，确保它知道有新请求在等待。
	 * 如果 GP 已经在跑，这次调用是幂等的（GP kthread 会在本轮结束后
	 * 再处理队列里的新请求）。
	 */
	(void) start_poll_synchronize_rcu();

	/* Now we can wait. */
	/* 挂起，等 rcu_sr_normal_complete() 通过 complete() 唤醒我们。 */
	wait_for_completion(&rs.completion);

trace_complete_out:
	trace_rcu_sr_normal(rcu_state.name, &rs.head, TPS("complete"));
	destroy_rcu_head_on_stack(&rs.head);
}

/**
 * synchronize_rcu - wait until a grace period has elapsed.
 *
 * Control will return to the caller some time after a full grace
 * period has elapsed, in other words after all currently executing RCU
 * read-side critical sections have completed.  Note, however, that
 * upon return from synchronize_rcu(), the caller might well be executing
 * concurrently with new RCU read-side critical sections that began while
 * synchronize_rcu() was waiting.
 *
 * RCU read-side critical sections are delimited by rcu_read_lock()
 * and rcu_read_unlock(), and may be nested.  In addition, but only in
 * v5.0 and later, regions of code across which interrupts, preemption,
 * or softirqs have been disabled also serve as RCU read-side critical
 * sections.  This includes hardware interrupt handlers, softirq handlers,
 * and NMI handlers.
 *
 * Note that this guarantee implies further memory-ordering guarantees.
 * On systems with more than one CPU, when synchronize_rcu() returns,
 * each CPU is guaranteed to have executed a full memory barrier since
 * the end of its last RCU read-side critical section whose beginning
 * preceded the call to synchronize_rcu().  In addition, each CPU having
 * an RCU read-side critical section that extends beyond the return from
 * synchronize_rcu() is guaranteed to have executed a full memory barrier
 * after the beginning of synchronize_rcu() and before the beginning of
 * that RCU read-side critical section.  Note that these guarantees include
 * CPUs that are offline, idle, or executing in user mode, as well as CPUs
 * that are executing in the kernel.
 *
 * Furthermore, if CPU A invoked synchronize_rcu(), which returned
 * to its caller on CPU B, then both CPU A and CPU B are guaranteed
 * to have executed a full memory barrier during the execution of
 * synchronize_rcu() -- even if CPU A and CPU B are the same CPU (but
 * again only if the system has more than one CPU).
 *
 * Implementation of these memory-ordering guarantees is described here:
 * Documentation/RCU/Design/Memory-Ordering/Tree-RCU-Memory-Ordering.rst.
 *
 * synchronize_rcu - 等待一个完整的 RCU 宽限期结束后返回。
 *
 * 【作用】
 * 阻塞调用者，直到所有在本次调用开始之前已经开始的 RCU 读临界区全部结束。
 * 返回后，调用者可以安全地释放/修改被 RCU 保护的旧数据，因为不再有读者
 * 持有指向它的引用。注意：返回时可能已有新的读临界区开始，但它们看到的
 * 是更新后的数据，无需关心。
 *
 * 【典型用法】
 *   writer 侧用 RCU 替换一个指针后，调用 synchronize_rcu()，
 *   确认所有老读者离场，然后才能 kfree() 老对象。
 *   如果不想阻塞，可以用 call_rcu() 注册一个回调代替。
 *
 * 【什么算 RCU 读临界区】
 *   - rcu_read_lock() / rcu_read_unlock() 之间的代码（可嵌套）
 *   - v5.0 起：关中断、关抢占、关软中断的代码段也算（包括硬中断、
 *     软中断、NMI 处理函数）
 *
 * 【内存序保证（SMP）】
 * 返回时，每个 CPU 保证已在其"调用 synchronize_rcu() 之前最后一个读
 * 临界区结束后"执行过完整内存屏障；跨越返回点的读临界区则保证在
 * synchronize_rcu() 开始后、该临界区开始前执行过完整内存屏障。
 * 这些保证对离线、idle、用户态 CPU 同样成立。
 * 若调用发生在 CPU A、返回到 CPU B，则 A 和 B 都保证在执行期间各自
 * 经历过完整内存屏障（即使 A==B，但前提是系统有多于一个 CPU）。
 *
 * 【实现路径选择】
 *   INACTIVE 阶段（早期启动）：直接记账，无需真正等待（无并发读者）
 *   INIT/RUNNING + expedited：IPI 强制各 CPU 快速通过静止状态，延迟低
 *   INIT/RUNNING + normal：批量排队共享宽限期，吞吐高
 *
 * 内存序实现细节见：
 *   Documentation/RCU/Design/Memory-Ordering/Tree-RCU-Memory-Ordering.rst
 */
/*
 * synchronize_rcu() - 阻塞等待调用前开始的所有常规 RCU 读侧临界区结束。
 * 无入参/返回，可睡眠，不能从 RCU 读侧或原子上下文调用。早期启动/单任务环境可
 * 退化为屏障；常规环境选择普通或加速路径。返回建立完整 GP 内存序，但不等待
 * call_rcu callback 执行，也不等待调用之后新开始的读者。
 */
void synchronize_rcu(void)
{
	unsigned long flags;
	struct rcu_node *rnp;

	RCU_LOCKDEP_WARN(lock_is_held(&rcu_bh_lock_map) ||
			 lock_is_held(&rcu_lock_map) ||
			 lock_is_held(&rcu_sched_lock_map),
			 "Illegal synchronize_rcu() in RCU read-side critical section");
	if (!rcu_blocking_is_gp()) {
		/*
		 * 正常路径（调度器已启动）：根据当前是否处于 expedited 模式
		 * 选择实现。expedited 通过 IPI 强制所有 CPU 快速通过静止状态，
		 * 延迟低但对系统干扰大；normal 则批量排队共享宽限期，吞吐高。
		 * 启动阶段及 sysctl rcu_expedited=1 时走 expedited 路径。
		 */
		if (rcu_gp_is_expedited())
			synchronize_rcu_expedited();
		else
			synchronize_rcu_normal();
		return;
	}

	// Context allows vacuous grace periods.
	// Note well that this code runs with !PREEMPT && !SMP.
	// In addition, all code that advances grace periods runs at
	// process level.  Therefore, this normal GP overlaps with other
	// normal GPs only by being fully nested within them, which allows
	// reuse of ->gp_seq_polled_snap.
	/*
	 * 早期启动快路径（INACTIVE 阶段，!PREEMPT && !SMP）：
	 * rcu_blocking_is_gp() 返回 true，说明当前上下文天然就是一个完整的
	 * 宽限期（系统只有一个执行流，不存在并发读者）。
	 * 这里不需要真正等待，只需"记账"——让 GP 序号正确推进，
	 * 使后续的 poll_state_synchronize_rcu() 等函数能看到宽限期已完成。
	 *
	 * 注意：所有推进宽限期序号的代码都在进程上下文运行，因此并发的
	 * synchronize_rcu() 只能通过完全嵌套来重叠，不会出现交叉，
	 * 所以可以安全复用全局的 ->gp_seq_polled_snap。
	 */
	rcu_poll_gp_seq_start_unlocked(&rcu_state.gp_seq_polled_snap);
	rcu_poll_gp_seq_end_unlocked(&rcu_state.gp_seq_polled_snap);

	// Update the normal grace-period counters to record
	// this grace period, but only those used by the boot CPU.
	// The rcu_scheduler_starting() will take care of the rest of
	// these counters.
	/*
	 * 手动推进 GP 序号以记录本次"宽限期"。
	 * 只更新启动 CPU 用到的计数器；rcu_scheduler_starting() 稍后会把
	 * 整棵 rcu_node 树的序号全部对齐，这里不必也不能提前做全树更新。
	 * 关中断防止中断处理程序并发修改同一批序号。
	 */
	local_irq_save(flags);
	WARN_ON_ONCE(num_online_cpus() > 1);
	rcu_state.gp_seq += (1 << RCU_SEQ_CTR_SHIFT);
	for (rnp = this_cpu_ptr(&rcu_data)->mynode; rnp; rnp = rnp->parent)
		rnp->gp_seq_needed = rnp->gp_seq = rcu_state.gp_seq;
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(synchronize_rcu);

/**
 * get_completed_synchronize_rcu_full - Return a full pre-completed polled state cookie
 * @rgosp: Place to put state cookie
 *
 * Stores into @rgosp a value that will always be treated by functions
 * like poll_state_synchronize_rcu_full() as a cookie whose grace period
 * has already completed.
 */
/*
 * get_completed_synchronize_rcu_full() 填充保证已完成的 normal/exp poll cookie。
 * @rgosp 必须是调用者拥有的非空输出对象；无返回、不启动或等待 GP。写入的哨兵值
 * 使后续 full poll 无条件成功并执行其成功路径内存屏障。
 */
void get_completed_synchronize_rcu_full(struct rcu_gp_oldstate *rgosp)
{
	rgosp->rgos_norm = RCU_GET_STATE_COMPLETED;
	rgosp->rgos_exp = RCU_GET_STATE_COMPLETED;
}
EXPORT_SYMBOL_GPL(get_completed_synchronize_rcu_full);

/**
 * get_state_synchronize_rcu - Snapshot current RCU state
 *
 * Returns a cookie that is used by a later call to cond_synchronize_rcu()
 * or poll_state_synchronize_rcu() to determine whether or not a full
 * grace period has elapsed in the meantime.
 */
/* get_state_synchronize_rcu() 返回可供 cond/poll API 判断的当前常规 GP 快照。 */
unsigned long get_state_synchronize_rcu(void)
{
	/*
	 * Any prior manipulation of RCU-protected data must happen
	 * before the load from ->gp_seq.
	 */
	/*
	 * 完整屏障把调用者此前对 RCU 保护数据的摘除/修改排在 gp_seq 快照之前；
	 * 后续 poll 返回 true 时的屏障共同提供与 synchronize_rcu() 相同的前后顺序。
	 */
	smp_mb();  /* ^^^ */
	return rcu_seq_snap(&rcu_state.gp_seq_polled);
}
EXPORT_SYMBOL_GPL(get_state_synchronize_rcu);

/**
 * get_state_synchronize_rcu_full - Snapshot RCU state, both normal and expedited
 * @rgosp: location to place combined normal/expedited grace-period state
 *
 * Places the normal and expedited grace-period states in @rgosp.  This
 * state value can be passed to a later call to cond_synchronize_rcu_full()
 * or poll_state_synchronize_rcu_full() to determine whether or not a
 * grace period (whether normal or expedited) has elapsed in the meantime.
 * The rcu_gp_oldstate structure takes up twice the memory of an unsigned
 * long, but is guaranteed to see all grace periods.  In contrast, the
 * combined state occupies less memory, but can sometimes fail to take
 * grace periods into account.
 *
 * This does not guarantee that the needed grace period will actually
 * start.
 */
/*
 * get_state_synchronize_rcu_full() 同时保存常规/加速快照，不启动 GP也不等待。
 * @rgosp 是不可空输出对象，ownership 保持在调用者；可用于 cond/full poll。
 * 函数先用完整屏障发布此前更新，再记录两个序号；调用者须保持对象有效到消费结束。
 */
void get_state_synchronize_rcu_full(struct rcu_gp_oldstate *rgosp)
{
	/*
	 * Any prior manipulation of RCU-protected data must happen
	 * before the loads from ->gp_seq and ->expedited_sequence.
	 */
	/*
	 * 同一个前置屏障同时约束 normal 与 expedited 快照，保证调用者先发布对 RCU
	 * 数据的修改，再开始观察任一类型的完整 GP。
	 */
	smp_mb();  /* ^^^ */

	// Yes, rcu_state.gp_seq, not rnp_root->gp_seq, the latter's use
	// in poll_state_synchronize_rcu_full() notwithstanding.  Use of
	// the latter here would result in too-short grace periods due to
	// interactions with newly onlined CPUs.
	/*
	 * 这里必须快照全局 rcu_state.gp_seq，而不是 full poll 最终检查的根节点 gp_seq。
	 * GP 初始化先发布全局序号、后把新上线 CPU 纳入并同步根序号；若入口就取根快照，
	 * hotplug 交错可能让请求看似被一轮过短 GP 覆盖。
	 */
	rgosp->rgos_norm = rcu_seq_snap(&rcu_state.gp_seq);
	rgosp->rgos_exp = rcu_seq_snap(&rcu_state.expedited_sequence);
}
EXPORT_SYMBOL_GPL(get_state_synchronize_rcu_full);

/*
 * Helper function for start_poll_synchronize_rcu() and
 * start_poll_synchronize_rcu_full().
 */
/*
 * start_poll_synchronize_rcu_common() 异步确保未来有一轮 normal GP 覆盖请求点。
 * 无参数、无返回；函数关闭 IRQ、锁当前 CPU 叶节点并把需求沿树发布，解锁后才可能
 * 唤醒 GP kthread。它允许因竞争多启动一轮 GP，但不等待完成，也不启动 expedited GP。
 */
static void start_poll_synchronize_rcu_common(void)
{
	unsigned long flags;
	bool needwake;
	struct rcu_data *rdp;
	struct rcu_node *rnp;

	local_irq_save(flags);
	rdp = this_cpu_ptr(&rcu_data);
	rnp = rdp->mynode;
	raw_spin_lock_rcu_node(rnp); // irqs already disabled.
	/* 行尾英文说明：local_irq_save() 已关闭 IRQ，此处取得叶锁来串行化 GP 请求。 */
	// Note it is possible for a grace period to have elapsed between
	// the above call to get_state_synchronize_rcu() and the below call
	// to rcu_seq_snap.  This is OK, the worst that happens is that we
	// get a grace period that no one needed.  These accesses are ordered
	// by smp_mb(), and we are accessing them in the opposite order
	// from which they are updated at grace-period start, as required.
	/*
	 * get_state() 与此处重新 snap 之间允许完整 GP 结束；最坏只是多请求一轮无人需要的
	 * GP，不会少等。两次访问由 smp_mb() 排序，并按 GP start 更新顺序的反向读取，
	 * 从而避免同时错过旧 GP 与新请求发布。
	 */
	needwake = rcu_start_this_gp(rnp, rdp, rcu_seq_snap(&rcu_state.gp_seq));
	raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
	if (needwake)
		rcu_gp_kthread_wake();
}

/**
 * start_poll_synchronize_rcu - Snapshot and start RCU grace period
 *
 * Returns a cookie that is used by a later call to cond_synchronize_rcu()
 * or poll_state_synchronize_rcu() to determine whether or not a full
 * grace period has elapsed in the meantime.  If the needed grace period
 * is not already slated to start, notifies RCU core of the need for that
 * grace period.
 */
/* start_poll_synchronize_rcu() 返回请求后的常规快照；异步、不睡眠，供后续 poll。 */
unsigned long start_poll_synchronize_rcu(void)
{
	unsigned long gp_seq = get_state_synchronize_rcu();

	start_poll_synchronize_rcu_common();
	return gp_seq;
}
EXPORT_SYMBOL_GPL(start_poll_synchronize_rcu);

/**
 * start_poll_synchronize_rcu_full - Take a full snapshot and start RCU grace period
 * @rgosp: value from get_state_synchronize_rcu_full() or start_poll_synchronize_rcu_full()
 *
 * Places the normal and expedited grace-period states in *@rgos.  This
 * state value can be passed to a later call to cond_synchronize_rcu_full()
 * or poll_state_synchronize_rcu_full() to determine whether or not a
 * grace period (whether normal or expedited) has elapsed in the meantime.
 * If the needed grace period is not already slated to start, notifies
 * RCU core of the need for that grace period.
 */
/* full start_poll 同时启动/记录常规与加速 GP 需求，结果写 @rgosp。 */
/*
 * 修正说明：本函数记录 normal 与 expedited 两类快照，但只通过 common 路径确保
 * 常规 GP 已请求；它不会启动 expedited GP。@rgosp 是不可空纯输出借用指针，
 * ownership 不变；函数异步返回，不保证任一 GP 已完成。
 */
void start_poll_synchronize_rcu_full(struct rcu_gp_oldstate *rgosp)
{
	get_state_synchronize_rcu_full(rgosp);

	start_poll_synchronize_rcu_common();
}
EXPORT_SYMBOL_GPL(start_poll_synchronize_rcu_full);

/**
 * poll_state_synchronize_rcu - Has the specified RCU grace period completed?
 * @oldstate: value from get_state_synchronize_rcu() or start_poll_synchronize_rcu()
 *
 * If a full RCU grace period has elapsed since the earlier call from
 * which @oldstate was obtained, return @true, otherwise return @false.
 * If @false is returned, it is the caller's responsibility to invoke this
 * function later on until it does return @true.  Alternatively, the caller
 * can explicitly wait for a grace period, for example, by passing @oldstate
 * to either cond_synchronize_rcu() or cond_synchronize_rcu_expedited()
 * on the one hand or by directly invoking either synchronize_rcu() or
 * synchronize_rcu_expedited() on the other.
 *
 * Yes, this function does not take counter wrap into account.
 * But counter wrap is harmless.  If the counter wraps, we have waited for
 * more than a billion grace periods (and way more on a 64-bit system!).
 * Those needing to keep old state values for very long time periods
 * (many hours even on 32-bit systems) should check them occasionally and
 * either refresh them or set a flag indicating that the grace period has
 * completed.  Alternatively, they can use get_completed_synchronize_rcu()
 * to get a guaranteed-completed grace-period state.
 *
 * In addition, because oldstate compresses the grace-period state for
 * both normal and expedited grace periods into a single unsigned long,
 * it can miss a grace period when synchronize_rcu() runs concurrently
 * with synchronize_rcu_expedited().  If this is unacceptable, please
 * instead use the _full() variant of these polling APIs.
 *
 * This function provides the same memory-ordering guarantees that
 * would be provided by a synchronize_rcu() that was invoked at the call
 * to the function that provided @oldstate, and that returned at the end
 * of this function.
 */
/*
 * poll_state_synchronize_rcu() 无阻塞判断 @oldstate 所代表 GP 是否已结束。
 * @oldstate 必须来自配套 get/start API 或 completed 哨兵；true 路径执行后置完整
 * 屏障并提供 synchronize_rcu() 等价顺序，false 无顺序保证且不启动 GP。无副作用
 * 除成功屏障，调用者需在 false 后重试或改用阻塞等待。
 */
bool poll_state_synchronize_rcu(unsigned long oldstate)
{
	if (oldstate == RCU_GET_STATE_COMPLETED ||
	    rcu_seq_done_exact(&rcu_state.gp_seq_polled, oldstate)) {
		smp_mb(); /* Ensure GP ends before subsequent accesses. */
		/*
		 * 成功路径的后置完整屏障确保 GP 完成先于调用者随后访问；它与取得 oldstate
		 * 前的屏障组成 synchronize_rcu() 等价顺序。false 路径不作此承诺。
		 */
		return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(poll_state_synchronize_rcu);

/**
 * poll_state_synchronize_rcu_full - Has the specified RCU grace period completed?
 * @rgosp: value from get_state_synchronize_rcu_full() or start_poll_synchronize_rcu_full()
 *
 * If a full RCU grace period has elapsed since the earlier call from
 * which *rgosp was obtained, return @true, otherwise return @false.
 * If @false is returned, it is the caller's responsibility to invoke this
 * function later on until it does return @true.  Alternatively, the caller
 * can explicitly wait for a grace period, for example, by passing @rgosp
 * to cond_synchronize_rcu() or by directly invoking synchronize_rcu().
 *
 * Yes, this function does not take counter wrap into account.
 * But counter wrap is harmless.  If the counter wraps, we have waited
 * for more than a billion grace periods (and way more on a 64-bit
 * system!).  Those needing to keep rcu_gp_oldstate values for very
 * long time periods (many hours even on 32-bit systems) should check
 * them occasionally and either refresh them or set a flag indicating
 * that the grace period has completed.  Alternatively, they can use
 * get_completed_synchronize_rcu_full() to get a guaranteed-completed
 * grace-period state.
 *
 * This function provides the same memory-ordering guarantees that would
 * be provided by a synchronize_rcu() that was invoked at the call to
 * the function that provided @rgosp, and that returned at the end of this
 * function.  And this guarantee requires that the root rcu_node structure's
 * ->gp_seq field be checked instead of that of the rcu_state structure.
 * The problem is that the just-ending grace-period's callbacks can be
 * invoked between the time that the root rcu_node structure's ->gp_seq
 * field is updated and the time that the rcu_state structure's ->gp_seq
 * field is updated.  Therefore, if a single synchronize_rcu() is to
 * cause a subsequent poll_state_synchronize_rcu_full() to return @true,
 * then the root rcu_node structure is the one that needs to be polled.
 */
/* full poll 仅当常规与加速两种旧状态均满足时返回 true，并提供必要 acquire 顺序。 */
/*
 * 修正说明：这里是“任意一种”而不是“两种均满足”。normal 或 expedited 中只要
 * 有一轮完整 GP 覆盖对应快照，就足以提供 RCU 等待保证，因此条件使用 OR。
 * true 路径执行完整屏障；false 不睡眠、不启动 GP，调用者须稍后重试或显式等待。
 */
bool poll_state_synchronize_rcu_full(struct rcu_gp_oldstate *rgosp)
{
	struct rcu_node *rnp = rcu_get_root();

	smp_mb(); // Order against root rcu_node structure grace-period cleanup.
	/*
	 * 前置完整屏障与根节点 cleanup 发布排序，保证随后读取 root->gp_seq 时不会越过
	 * 刚结束 GP 的 callback 可见边界。
	 */
	if (rgosp->rgos_norm == RCU_GET_STATE_COMPLETED ||
	    rcu_seq_done_exact(&rnp->gp_seq, rgosp->rgos_norm) ||
	    rgosp->rgos_exp == RCU_GET_STATE_COMPLETED ||
	    rcu_seq_done_exact(&rcu_state.expedited_sequence, rgosp->rgos_exp)) {
		smp_mb(); /* Ensure GP ends before subsequent accesses. */
		/*
		 * 任一完整 GP 覆盖快照后，以后置屏障阻止调用者后续访问越过完成边界；
		 * 因而 true 路径具备阻塞 synchronize_rcu() 的内存序保证。
		 */
		return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(poll_state_synchronize_rcu_full);

/**
 * cond_synchronize_rcu - Conditionally wait for an RCU grace period
 * @oldstate: value from get_state_synchronize_rcu(), start_poll_synchronize_rcu(), or start_poll_synchronize_rcu_expedited()
 *
 * If a full RCU grace period has elapsed since the earlier call to
 * get_state_synchronize_rcu() or start_poll_synchronize_rcu(), just return.
 * Otherwise, invoke synchronize_rcu() to wait for a full grace period.
 *
 * Yes, this function does not take counter wrap into account.
 * But counter wrap is harmless.  If the counter wraps, we have waited for
 * more than 2 billion grace periods (and way more on a 64-bit system!),
 * so waiting for a couple of additional grace periods should be just fine.
 *
 * This function provides the same memory-ordering guarantees that
 * would be provided by a synchronize_rcu() that was invoked at the call
 * to the function that provided @oldstate and that returned at the end
 * of this function.
 */
/*
 * cond_synchronize_rcu() 若 @oldstate 已满足只执行顺序保证，否则阻塞等待 normal GP。
 * @oldstate 必须来自配套快照 API；函数可睡眠，不能在原子上下文调用。无返回；
 * 返回时至少一轮所需 GP 已覆盖快照，并具备 synchronize_rcu() 的内存序保证。
 */
void cond_synchronize_rcu(unsigned long oldstate)
{
	if (!poll_state_synchronize_rcu(oldstate))
		synchronize_rcu();
}
EXPORT_SYMBOL_GPL(cond_synchronize_rcu);

/**
 * cond_synchronize_rcu_full - Conditionally wait for an RCU grace period
 * @rgosp: value from get_state_synchronize_rcu_full(), start_poll_synchronize_rcu_full(), or start_poll_synchronize_rcu_expedited_full()
 *
 * If a full RCU grace period has elapsed since the call to
 * get_state_synchronize_rcu_full(), start_poll_synchronize_rcu_full(),
 * or start_poll_synchronize_rcu_expedited_full() from which @rgosp was
 * obtained, just return.  Otherwise, invoke synchronize_rcu() to wait
 * for a full grace period.
 *
 * Yes, this function does not take counter wrap into account.
 * But counter wrap is harmless.  If the counter wraps, we have waited for
 * more than 2 billion grace periods (and way more on a 64-bit system!),
 * so waiting for a couple of additional grace periods should be just fine.
 *
 * This function provides the same memory-ordering guarantees that
 * would be provided by a synchronize_rcu() that was invoked at the call
 * to the function that provided @rgosp and that returned at the end of
 * this function.
 */
/* full cond 对常规/加速快照作同样条件等待；可睡眠，返回时两者均跨过所需 GP。 */
/*
 * 修正说明：返回保证是至少一轮 normal 或 expedited GP 覆盖输入快照，并非两者都
 * 完成。若 poll 已满足则仅走屏障快路径；否则 synchronize_rcu() 睡眠等待一轮
 * normal GP。@rgosp 为不可空只读借用指针，函数不修改其内容或 ownership。
 */
void cond_synchronize_rcu_full(struct rcu_gp_oldstate *rgosp)
{
	if (!poll_state_synchronize_rcu_full(rgosp))
		synchronize_rcu();
}
EXPORT_SYMBOL_GPL(cond_synchronize_rcu_full);

/*
 * Check to see if there is any immediate RCU-related work to be done by
 * the current CPU, returning 1 if so and zero otherwise.  The checks are
 * in order of increasing expense: checks that can be carried out against
 * CPU-local state are performed first.  However, we must check for CPU
 * stalls first, else we might not get a chance.
 */
/* rcu_pending() 汇总本 CPU 是否有 QS、GP、callback、FQS 或过载工作待 core 处理。 */
static int rcu_pending(int user)
{
	bool gp_in_progress;
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);
	struct rcu_node *rnp = rdp->mynode;

	lockdep_assert_irqs_disabled();

	/* Check for CPU stalls, if enabled. */
	/* stall 检查优先执行；即使后续快速判断“无 core 工作”，也不能饿死诊断计时。 */
	check_cpu_stall(rdp);

	/* Does this CPU need a deferred NOCB wakeup? */
	/* 延迟的 nocb 唤醒需要尽快由 core 兑现，否则 offloaded callback 可能无人消费。 */
	if (rcu_nocb_need_deferred_wakeup(rdp, RCU_NOCB_WAKE))
		return 1;

	/* Is this a nohz_full CPU in userspace or idle?  (Ignore RCU if so.) */
	/*
	 * nohz_full CPU 位于用户态/EQS，或新 GP 仍处 patience 窗口时，优先维持隔离并返回
	 * 无工作；这些上下文本身已提供 QS，callback/GP 工作可由 housekeeper 处理。
	 */
	gp_in_progress = rcu_gp_in_progress();
	if ((user || rcu_is_cpu_rrupt_from_idle() ||
	     (gp_in_progress &&
	      time_before(jiffies, READ_ONCE(rcu_state.gp_start) +
			  nohz_full_patience_delay_jiffies))) &&
	    rcu_nohz_full_cpu())
		return 0;

	/* Is the RCU core waiting for a quiescent state from this CPU? */
	/* core_needs_qs 且 norm 已清表示本地已观察 QS，需进入 core 提交到树。 */
	if (rdp->core_needs_qs && !rdp->cpu_no_qs.b.norm && gp_in_progress)
		return 1;

	/* Does this CPU have callbacks ready to invoke? */
	/* 普通 CPU 的 DONE 段非空时需要 core 调用；NOCB CPU 由 rcuoc 负责。 */
	if (!rcu_rdp_is_offloaded(rdp) &&
	    rcu_segcblist_ready_cbs(&rdp->cblist))
		return 1;

	/* Has RCU gone idle with this CPU needing another grace period? */
	/* GP 空闲但 NEXT_READY 之后仍有 callback，需进入 core 分代并请求下一 GP。 */
	if (!gp_in_progress && rcu_segcblist_is_enabled(&rdp->cblist) &&
	    !rcu_rdp_is_offloaded(rdp) &&
	    !rcu_segcblist_restempty(&rdp->cblist, RCU_NEXT_READY_TAIL))
		return 1;

	/* Have RCU grace period completed or started?  */
	/* node 与 per-CPU 序号不同或发生环绕时，需要 core 同步新开始/结束状态。 */
	if (rcu_seq_current(&rnp->gp_seq) != rdp->gp_seq ||
	    unlikely(READ_ONCE(rdp->gpwrap))) /* outside lock */
		/* 行尾英文说明：这是无锁快速检查，最终状态必须在叶节点锁下重新验证。 */
		return 1;

	/* nothing to do */
	/* 所有便宜到昂贵的条件均未命中，本 CPU 当前无需调度 RCU core。 */
	return 0;
}

/*
 * Helper function for rcu_barrier() tracing.  If tracing is disabled,
 * the compiler is expected to optimize this away.
 */
/* rcu_barrier_trace() 记录 barrier 阶段、CPU 与完成计数，仅用于 trace。 */
static void rcu_barrier_trace(const char *s, int cpu, unsigned long done)
{
	trace_rcu_barrier(rcu_state.name, s, cpu,
			  atomic_read(&rcu_state.barrier_cpu_count), done);
}

/*
 * RCU callback function for rcu_barrier().  If we are last, wake
 * up the task executing rcu_barrier().
 *
 * Note that the value of rcu_state.barrier_sequence must be captured
 * before the atomic_dec_and_test().  Otherwise, if this CPU is not last,
 * other CPUs might count the value down to zero before this CPU gets
 * around to invoking rcu_barrier_trace(), which might result in bogus
 * data from the next instance of rcu_barrier().
 */
/*
 * rcu_barrier_callback() 是追加到每个相关 CPU 队尾的内部哨兵回调。
 * @rhp 指向永久 per-CPU barrier_head，RCU 把执行权暂交本函数但对象不可释放；无返回。
 * 它先以自指 next 标记已调用，再递减全局计数；最后一个回调完成 completion，唤醒
 * rcu_barrier()。可在 callback 上下文执行，不能睡眠。
 */
static void rcu_barrier_callback(struct rcu_head *rhp)
{
	unsigned long __maybe_unused s = rcu_state.barrier_sequence;

	rhp->next = rhp; // Mark the callback as having been invoked.
	/*
	 * 行尾英文说明：自指针是 barrier_head 的“已执行”哨兵，便于后续轮次区分尚在
	 * cblist 的节点；callback 后不释放该 per-CPU 静态对象。
	 */
	if (atomic_dec_and_test(&rcu_state.barrier_cpu_count)) {
		rcu_barrier_trace(TPS("LastCB"), -1, s);
		complete(&rcu_state.barrier_completion);
	} else {
		rcu_barrier_trace(TPS("CB"), -1, s);
	}
}

/*
 * If needed, entrain an rcu_barrier() callback on rdp->cblist.
 */
/*
 * rcu_barrier_entrain() - 在某 CPU callback 链尾追加专用 barrier callback。
 * 只有该 CPU 原有 callbacks 全部调用后它才运行；增加全局等待计数。入口按 cblist
 * 锁协议执行，返回无。
 */
static void rcu_barrier_entrain(struct rcu_data *rdp)
{
	unsigned long gseq = READ_ONCE(rcu_state.barrier_sequence);
	unsigned long lseq = READ_ONCE(rdp->barrier_seq_snap);
	bool wake_nocb = false;
	bool was_alldone = false;

	lockdep_assert_held(&rcu_state.barrier_lock);
	if (rcu_seq_state(lseq) || !rcu_seq_state(gseq) || rcu_seq_ctr(lseq) != rcu_seq_ctr(gseq))
		return;
	rcu_barrier_trace(TPS("IRQ"), -1, rcu_state.barrier_sequence);
	rdp->barrier_head.func = rcu_barrier_callback;
	debug_rcu_head_queue(&rdp->barrier_head);
	rcu_nocb_lock(rdp);
	/*
	 * Flush bypass and wakeup rcuog if we add callbacks to an empty regular
	 * queue. This way we don't wait for bypass timer that can reach seconds
	 * if it's fully lazy.
	 */
	/*
	 * NOCB CPU 可能把 callback 留在 bypass 队列并依赖最长数秒的 lazy 定时器。
	 * barrier 必须先 flush bypass；若常规队列原为空而 flush 后变为待处理，还要唤醒
	 * rcuog，否则 barrier 哨兵虽已排队却可能长时间无人推进。
	 */
	was_alldone = rcu_rdp_is_offloaded(rdp) && !rcu_segcblist_pend_cbs(&rdp->cblist);
	WARN_ON_ONCE(!rcu_nocb_flush_bypass(rdp, NULL, jiffies, false));
	wake_nocb = was_alldone && rcu_segcblist_pend_cbs(&rdp->cblist);
	if (rcu_segcblist_entrain(&rdp->cblist, &rdp->barrier_head)) {
		atomic_inc(&rcu_state.barrier_cpu_count);
	} else {
		debug_rcu_head_unqueue(&rdp->barrier_head);
		rcu_barrier_trace(TPS("IRQNQ"), -1, rcu_state.barrier_sequence);
	}
	rcu_nocb_unlock(rdp);
	if (wake_nocb)
		wake_nocb_gp(rdp);
	smp_store_release(&rdp->barrier_seq_snap, gseq);
}

/*
 * Called with preemption disabled, and from cross-cpu IRQ context.
 */
/* barrier_handler 在目标 CPU/代替离线 CPU 的上下文把 barrier callback 入其 rdp。 */
static void rcu_barrier_handler(void *cpu_in)
{
	uintptr_t cpu = (uintptr_t)cpu_in;
	struct rcu_data *rdp = per_cpu_ptr(&rcu_data, cpu);

	lockdep_assert_irqs_disabled();
	WARN_ON_ONCE(cpu != rdp->cpu);
	WARN_ON_ONCE(cpu != smp_processor_id());
	raw_spin_lock(&rcu_state.barrier_lock);
	rcu_barrier_entrain(rdp);
	raw_spin_unlock(&rcu_state.barrier_lock);
}

/**
 * rcu_barrier - Wait until all in-flight call_rcu() callbacks complete.
 *
 * Note that this primitive does not necessarily wait for an RCU grace period
 * to complete.  For example, if there are no RCU callbacks queued anywhere
 * in the system, then rcu_barrier() is within its rights to return
 * immediately, without waiting for anything, much less an RCU grace period.
 * In fact, rcu_barrier() will normally not result in any RCU grace periods
 * beyond those that were already destined to be executed.
 *
 * In kernels built with CONFIG_RCU_LAZY=y, this function also hurries all
 * pending lazy RCU callbacks.
 */
/*
 * rcu_barrier() - 等待调用前排入的所有 call_rcu callback 实际执行完毕。
 * 可睡眠、无返回。barrier_mutex 串行化并发 barrier；给每个有 callback 的 CPU 链尾
 * entrain 哨兵，处理 nocb/offline 竞态，等待所有哨兵完成。它不启动一个“覆盖未来
 * callback”的永久栅栏，新 callback 可在扫描后入队而不属于本轮。
 */
void rcu_barrier(void)
{
	uintptr_t cpu;
	unsigned long flags;
	unsigned long gseq;
	struct rcu_data *rdp;
	unsigned long s = rcu_seq_snap(&rcu_state.barrier_sequence);

	rcu_barrier_trace(TPS("Begin"), -1, s);

	/* Take mutex to serialize concurrent rcu_barrier() requests. */
	/* 阶段 1：全局串行 barrier；后来的调用可复用前一轮已经覆盖自己的工作。 */
	mutex_lock(&rcu_state.barrier_mutex);

	/* Did someone else do our work for us? */
	/*
	 * 进入 mutex 前保存序号。若等待 mutex 期间别的 barrier 已完整结束，它扫描时已
	 * 覆盖本调用之前的 callbacks，本调用只需全屏障后直接返回。
	 */
	if (rcu_seq_done(&rcu_state.barrier_sequence, s)) {
		rcu_barrier_trace(TPS("EarlyExit"), -1, rcu_state.barrier_sequence);
		smp_mb(); /* caller's subsequent code after above check. */
		/*
		 * 前一 barrier 已覆盖本调用开始前的 callback；该屏障确保其完成先于调用者
		 * 随后的资源释放或模块卸载访问。
		 */
		mutex_unlock(&rcu_state.barrier_mutex);
		return;
	}

	/* Mark the start of the barrier operation. */
	/* 阶段 2：在 barrier_lock 下发布奇数/进行中序号，CPU handler 用 gseq 去重。 */
	raw_spin_lock_irqsave(&rcu_state.barrier_lock, flags);
	rcu_seq_start(&rcu_state.barrier_sequence);
	gseq = rcu_state.barrier_sequence;
	rcu_barrier_trace(TPS("Inc1"), -1, rcu_state.barrier_sequence);

	/*
	 * Initialize the count to two rather than to zero in order
	 * to avoid a too-soon return to zero in case of an immediate
	 * invocation of the just-enqueued callback (or preemption of
	 * this task).  Exclude CPU-hotplug operations to ensure that no
	 * offline non-offloaded CPU has callbacks queued.
	 */
	/*
	 * 计数从 2 起步是“施工引用”：若刚 entrain 的 callback 立即执行，计数也不会在
	 * 扫描尚未完成时提前归零。全部 CPU 都登记后统一减 2，才允许真正 complete。
	 */
	init_completion(&rcu_state.barrier_completion);
	atomic_set(&rcu_state.barrier_cpu_count, 2);
	raw_spin_unlock_irqrestore(&rcu_state.barrier_lock, flags);

	/*
	 * Force each CPU with callbacks to register a new callback.
	 * When that callback is invoked, we will know that all of the
	 * corresponding CPU's preceding callbacks have been invoked.
	 */
	/*
	 * 阶段 3：扫描 possible CPU。空链只写 snap；offline 链在 barrier_lock 下直接
	 * entrain；online CPU 用同步 IPI 在其本地列表协议下 entrain。barrier callback
	 * 位于原链尾，因此它执行即证明该 CPU 本轮之前的 callback 全部执行。
	 */
	for_each_possible_cpu(cpu) {
		rdp = per_cpu_ptr(&rcu_data, cpu);
retry:
		if (smp_load_acquire(&rdp->barrier_seq_snap) == gseq)
			/* acquire 与 entrain 的 release snap 配对，已登记则无需重复 IPI。 */
			continue;
		raw_spin_lock_irqsave(&rcu_state.barrier_lock, flags);
		if (!rcu_segcblist_n_cbs(&rdp->cblist)) {
			WRITE_ONCE(rdp->barrier_seq_snap, gseq);
			raw_spin_unlock_irqrestore(&rcu_state.barrier_lock, flags);
			rcu_barrier_trace(TPS("NQ"), cpu, rcu_state.barrier_sequence);
			continue;
		}
		if (!rcu_rdp_cpu_online(rdp)) {
			rcu_barrier_entrain(rdp);
			WARN_ON_ONCE(READ_ONCE(rdp->barrier_seq_snap) != gseq);
			raw_spin_unlock_irqrestore(&rcu_state.barrier_lock, flags);
			rcu_barrier_trace(TPS("OfflineNoCBQ"), cpu, rcu_state.barrier_sequence);
			continue;
		}
		raw_spin_unlock_irqrestore(&rcu_state.barrier_lock, flags);
		if (smp_call_function_single(cpu, rcu_barrier_handler, (void *)cpu, 1)) {
			/* CPU 热插拔使同步 IPI 失败时短睡并重查 online/list 状态。 */
			schedule_timeout_uninterruptible(1);
			goto retry;
		}
		WARN_ON_ONCE(READ_ONCE(rdp->barrier_seq_snap) != gseq);
		rcu_barrier_trace(TPS("OnlineQ"), cpu, rcu_state.barrier_sequence);
	}

	/*
	 * Now that we have an rcu_barrier_callback() callback on each
	 * CPU, and thus each counted, remove the initial count.
	 */
	/*
	 * 每个需要等待的 CPU 都已 entrain 并增加计数，现在去掉初始两个施工引用。
	 * 若没有任何实际哨兵或它们已经同步完成，减到零者直接 complete；否则最后一个
	 * barrier callback 负责唤醒。
	 */
	if (atomic_sub_and_test(2, &rcu_state.barrier_cpu_count))
		complete(&rcu_state.barrier_completion);

	/* Wait for all rcu_barrier_callback() callbacks to be invoked. */
	/* 阶段 4：去掉施工引用后等待每个实际 entrain 哨兵各减一次。 */
	wait_for_completion(&rcu_state.barrier_completion);

	/* Mark the end of the barrier operation. */
	/* 阶段 5：发布偶数/完成序号，并把每 CPU snap 对齐，供下一轮快速判断。 */
	rcu_barrier_trace(TPS("Inc2"), -1, rcu_state.barrier_sequence);
	rcu_seq_end(&rcu_state.barrier_sequence);
	gseq = rcu_state.barrier_sequence;
	for_each_possible_cpu(cpu) {
		rdp = per_cpu_ptr(&rcu_data, cpu);

		WRITE_ONCE(rdp->barrier_seq_snap, gseq);
	}

	/* Other rcu_barrier() invocations can now safely proceed. */
	/*
	 * 本轮序号、各 CPU snap 和等待完成均已发布；释放 mutex 后，后续 barrier 可
	 * 根据 sequence 复用本轮结果或开始新的扫描。
	 */
	mutex_unlock(&rcu_state.barrier_mutex);
}
EXPORT_SYMBOL_GPL(rcu_barrier);

static unsigned long rcu_barrier_last_throttle;

/**
 * rcu_barrier_throttled - Do rcu_barrier(), but limit to one per second
 *
 * This can be thought of as guard rails around rcu_barrier() that
 * permits unrestricted userspace use, at least assuming the hardware's
 * try_cmpxchg() is robust.  There will be at most one call per second to
 * rcu_barrier() system-wide from use of this function, which means that
 * callers might needlessly wait a second or three.
 *
 * This is intended for use by test suites to avoid OOM by flushing RCU
 * callbacks from the previous test before starting the next.  See the
 * rcutree.do_rcu_barrier module parameter for more information.
 *
 * Why not simply make rcu_barrier() more scalable?  That might be
 * the eventual endpoint, but let's keep it simple for the time being.
 * Note that the module parameter infrastructure serializes calls to a
 * given .set() function, but should concurrent .set() invocation ever be
 * possible, we are ready!
 */
/* rcu_barrier_throttled() 对参数触发的频繁 barrier 按 jiffies 节流，避免自 DoS。 */
static void rcu_barrier_throttled(void)
{
	unsigned long j = jiffies;
	unsigned long old = READ_ONCE(rcu_barrier_last_throttle);
	unsigned long s = rcu_seq_snap(&rcu_state.barrier_sequence);

	while (time_in_range(j, old, old + HZ / 16) ||
	       !try_cmpxchg(&rcu_barrier_last_throttle, &old, j)) {
		schedule_timeout_idle(HZ / 16);
		if (rcu_seq_done(&rcu_state.barrier_sequence, s)) {
			smp_mb(); /* caller's subsequent code after above check. */
			/*
			 * 节流等待期间若别的 barrier 已完成，同样以完整屏障把其 callback 完成
			 * 排在调用者后续代码之前，然后无需再发起昂贵的全 CPU 扫描。
			 */
			return;
		}
		j = jiffies;
		old = READ_ONCE(rcu_barrier_last_throttle);
	}
	rcu_barrier();
}

/*
 * Invoke rcu_barrier_throttled() when a rcutree.do_rcu_barrier
 * request arrives.  We insist on a true value to allow for possible
 * future expansion.
 */
/* do_rcu_barrier setter 解析触发值并执行节流 barrier；返回解析状态。 */
static int param_set_do_rcu_barrier(const char *val, const struct kernel_param *kp)
{
	bool b;
	int ret;

	if (rcu_scheduler_active != RCU_SCHEDULER_RUNNING)
		return -EAGAIN;
	ret = kstrtobool(val, &b);
	if (!ret && b) {
		atomic_inc((atomic_t *)kp->arg);
		rcu_barrier_throttled();
		atomic_dec((atomic_t *)kp->arg);
	}
	return ret;
}

/*
 * Output the number of outstanding rcutree.do_rcu_barrier requests.
 */
/* getter 输出 barrier 参数计数/状态到 PAGE_SIZE 缓冲。 */
static int param_get_do_rcu_barrier(char *buffer, const struct kernel_param *kp)
{
	return sprintf(buffer, "%d\n", atomic_read((atomic_t *)kp->arg));
}

static const struct kernel_param_ops do_rcu_barrier_ops = {
	.set = param_set_do_rcu_barrier,
	.get = param_get_do_rcu_barrier,
};
static atomic_t do_rcu_barrier;
module_param_cb(do_rcu_barrier, &do_rcu_barrier_ops, &do_rcu_barrier, 0644);

/*
 * Compute the mask of online CPUs for the specified rcu_node structure.
 * This will not be stable unless the rcu_node structure's ->lock is
 * held, but the bit corresponding to the current CPU will be stable
 * in most contexts.
 */
/* rcu_rnp_online_cpus() 返回节点 qsmaskinitnext 的在线 CPU 位图快照。 */
static unsigned long rcu_rnp_online_cpus(struct rcu_node *rnp)
{
	return READ_ONCE(rnp->qsmaskinitnext);
}

/*
 * Is the CPU corresponding to the specified rcu_data structure online
 * from RCU's perspective?  This perspective is given by that structure's
 * ->qsmaskinitnext field rather than by the global cpu_online_mask.
 */
/* rcu_rdp_cpu_online() 以所属叶节点在线位判断 @rdp CPU 是否已被 RCU 发布 online。 */
static bool rcu_rdp_cpu_online(struct rcu_data *rdp)
{
	return !!(rdp->grpmask & rcu_rnp_online_cpus(rdp->mynode));
}

/* rcu_cpu_online() 对外返回指定 CPU 的 RCU 在线状态快照；不等同 cpu_online_mask。 */
bool rcu_cpu_online(int cpu)
{
	struct rcu_data *rdp = per_cpu_ptr(&rcu_data, cpu);

	return rcu_rdp_cpu_online(rdp);
}

#if defined(CONFIG_PROVE_RCU) && defined(CONFIG_HOTPLUG_CPU)

/*
 * Is the current CPU online as far as RCU is concerned?
 *
 * Disable preemption to avoid false positives that could otherwise
 * happen due to the current CPU number being sampled, this task being
 * preempted, its old CPU being taken offline, resuming on some other CPU,
 * then determining that its old CPU is now offline.
 *
 * Disable checking if in an NMI handler because we cannot safely
 * report errors from NMI handlers anyway.  In addition, it is OK to use
 * RCU on an offline processor during initial boot, hence the check for
 * rcu_scheduler_fully_active.
 */
/* lockdep helper 无插桩判断当前 CPU 对 RCU 是否 online，早启/idle 特例保守返回。 */
bool notrace rcu_lockdep_current_cpu_online(void)
{
	struct rcu_data *rdp;
	bool ret = false;

	if (in_nmi() || !rcu_scheduler_fully_active)
		return true;
	preempt_disable_notrace();
	rdp = this_cpu_ptr(&rcu_data);
	/*
	 * Strictly, we care here about the case where the current CPU is
	 * in rcutree_report_cpu_starting() and thus has an excuse for rdp->grpmask
	 * not being up to date. So arch_spin_is_locked() might have a
	 * false positive if it's held by some *other* CPU, but that's
	 * OK because that just means a false *negative* on the warning.
	 */
	/*
	 * 真正需豁免的是当前 CPU 正在 rcutree_report_cpu_starting()、grpmask 尚未完成更新
	 * 的窗口。这里只能测试全局 ofl_lock 是否被持有，若锁其实由别的 CPU 持有，会
	 * 把一次应报警情形误判为在线；这只是少报 lockdep 警告，不会放宽 RCU 正确性。
	 */
	if (rcu_rdp_cpu_online(rdp) || arch_spin_is_locked(&rcu_state.ofl_lock))
		ret = true;
	preempt_enable_notrace();
	return ret;
}
EXPORT_SYMBOL_GPL(rcu_lockdep_current_cpu_online);

#endif /* #if defined(CONFIG_PROVE_RCU) && defined(CONFIG_HOTPLUG_CPU) */
/*
 * 上述条件编译仅在 PROVE_RCU 与 CPU hotplug 同时启用时提供在线检查；其他配置
 * 不生成该调试 helper，也不影响实际 hotplug 协议。
 */

// Has rcu_init() been invoked?  This is used (for example) to determine
// whether spinlocks may be acquired safely.
/* rcu_init_invoked() 判断 rcu_init() 是否已进入可使用完整 per-CPU/tree 状态的阶段。 */
static bool rcu_init_invoked(void)
{
	return !!READ_ONCE(rcu_state.n_online_cpus);
}

/*
 * All CPUs for the specified rcu_node structure have gone offline,
 * and all tasks that were preempted within an RCU read-side critical
 * section while running on one of those CPUs have since exited their RCU
 * read-side critical section.  Some other CPU is reporting this fact with
 * the specified rcu_node structure's ->lock held and interrupts disabled.
 * This function therefore goes up the tree of rcu_node structures,
 * clearing the corresponding bits in the ->qsmaskinit fields.  Note that
 * the leaf rcu_node structure's ->qsmaskinit field has already been
 * updated.
 *
 * This function does check that the specified rcu_node structure has
 * all CPUs offline and no blocked tasks, so it is OK to invoke it
 * prematurely.  That said, invoking it after the fact will cost you
 * a needless lock acquisition.  So once it has done its work, don't
 * invoke it again.
 */
/* dead CPU 清理沿叶到根移除 qsmaskinitnext 位，并处理当前 GP 可能等待的最后位。 */
static void rcu_cleanup_dead_rnp(struct rcu_node *rnp_leaf)
{
	long mask;
	struct rcu_node *rnp = rnp_leaf;

	raw_lockdep_assert_held_rcu_node(rnp_leaf);
	if (!IS_ENABLED(CONFIG_HOTPLUG_CPU) ||
	    WARN_ON_ONCE(rnp_leaf->qsmaskinit) ||
	    WARN_ON_ONCE(rcu_preempt_has_tasks(rnp_leaf)))
		return;
	for (;;) {
		mask = rnp->grpmask;
		rnp = rnp->parent;
		if (!rnp)
			break;
		raw_spin_lock_rcu_node(rnp); /* irqs already disabled. */
		/* 行尾英文说明：叶调用者已关闭 IRQ，向父层传播时只需取得节点锁。 */
		rnp->qsmaskinit &= ~mask;
		/* Between grace periods, so better already be zero! */
		/*
		 * 该函数只调整下一轮 GP 的在线拓扑；当前 GP 的 qsmask 应已通过离线 QS
		 * 上报清零。非零表示在仍等待该子树时错误执行了拓扑摘除。
		 */
		WARN_ON_ONCE(rnp->qsmask);
		if (rnp->qsmaskinit) {
			raw_spin_unlock_rcu_node(rnp);
			/* irqs remain disabled. */
			/* 父节点仍有其他在线子树，传播到此结束；IRQ 状态继续归外层调用者管理。 */
			return;
		}
		raw_spin_unlock_rcu_node(rnp); /* irqs remain disabled. */
		/* 行尾英文说明：父层变空后继续向上，整个循环始终保持 IRQ 关闭。 */
	}
}

/*
 * Propagate ->qsinitmask bits up the rcu_node tree to account for the
 * first CPU in a given leaf rcu_node structure coming online.  The caller
 * must hold the corresponding leaf rcu_node ->lock with interrupts
 * disabled.
 */
/* new CPU 初始化沿叶到根增加在线位，使后续 GP 把它纳入 QS 初始掩码。 */
static void rcu_init_new_rnp(struct rcu_node *rnp_leaf)
{
	long mask;
	long oldmask;
	struct rcu_node *rnp = rnp_leaf;

	raw_lockdep_assert_held_rcu_node(rnp_leaf);
	WARN_ON_ONCE(rnp->wait_blkd_tasks);
	for (;;) {
		mask = rnp->grpmask;
		rnp = rnp->parent;
		if (rnp == NULL)
			return;
		raw_spin_lock_rcu_node(rnp); /* Interrupts already disabled. */
		/* 行尾英文说明：调用者已关闭 IRQ，逐级只取得父节点锁。 */
		oldmask = rnp->qsmaskinit;
		rnp->qsmaskinit |= mask;
		raw_spin_unlock_rcu_node(rnp); /* Interrupts remain disabled. */
		/* 行尾英文说明：释放父锁但不恢复 IRQ；只有父层原为空时才需继续上传。 */
		if (oldmask)
			return;
	}
}

/*
 * Do boot-time initialization of a CPU's per-CPU RCU data.
 */
/* 启动期初始化 @cpu 的 rcu_data 序号、node 关联、dynticks 与 callback 列表。 */
static void __init
rcu_boot_init_percpu_data(int cpu)
{
	struct context_tracking *ct = this_cpu_ptr(&context_tracking);
	struct rcu_data *rdp = per_cpu_ptr(&rcu_data, cpu);

	/* Set up local state, ensuring consistent view of global state. */
	/*
	 * 启动期单线程建立 per-CPU 状态：grpmask 绑定叶内 bit，barrier/online/offline
	 * 快照统一取当前全局 GP，context-tracking 必须处于 watching。初始化完成前
	 * @rdp 只由启动 CPU 持有，尚未发布给并发 callback/QS 路径。
	 */
	rdp->grpmask = leaf_node_cpu_bit(rdp->mynode, cpu);
	INIT_WORK(&rdp->strict_work, strict_work_handler);
	WARN_ON_ONCE(ct->nesting != 1);
	WARN_ON_ONCE(rcu_watching_snap_in_eqs(ct_rcu_watching_cpu(cpu)));
	rdp->barrier_seq_snap = rcu_state.barrier_sequence;
	rdp->rcu_ofl_gp_seq = rcu_state.gp_seq;
	rdp->rcu_ofl_gp_state = RCU_GP_CLEANED;
	rdp->rcu_onl_gp_seq = rcu_state.gp_seq;
	rdp->rcu_onl_gp_state = RCU_GP_CLEANED;
	rdp->last_sched_clock = jiffies;
	rdp->cpu = cpu;
	rcu_boot_init_nocb_percpu_data(rdp);
}

/* 将 RCU kthread affinity 绑定到 @rnp 覆盖 CPU，失败时保留可运行 fallback。 */
static void rcu_thread_affine_rnp(struct task_struct *t, struct rcu_node *rnp)
{
	cpumask_var_t affinity;
	int cpu;

	if (!zalloc_cpumask_var(&affinity, GFP_KERNEL))
		return;

	for_each_leaf_node_possible_cpu(rnp, cpu)
		cpumask_set_cpu(cpu, affinity);

	kthread_affine_preferred(t, affinity);

	free_cpumask_var(affinity);
}

struct kthread_worker *rcu_exp_gp_kworker;

/* 为叶/中间节点创建 expedited parallel GP kworker；失败退回串行加速路径。 */
static void rcu_spawn_exp_par_gp_kworker(struct rcu_node *rnp)
{
	struct kthread_worker *kworker;
	const char *name = "rcu_exp_par_gp_kthread_worker/%d";
	struct sched_param param = { .sched_priority = kthread_prio };
	int rnp_index = rnp - rcu_get_root();

	if (rnp->exp_kworker)
		return;

	kworker = kthread_create_worker(0, name, rnp_index);
	if (IS_ERR_OR_NULL(kworker)) {
		pr_err("Failed to create par gp kworker on %d/%d\n",
		       rnp->grplo, rnp->grphi);
		return;
	}
	WRITE_ONCE(rnp->exp_kworker, kworker);

	if (IS_ENABLED(CONFIG_RCU_EXP_KTHREAD))
		sched_setscheduler_nocheck(kworker->task, SCHED_FIFO, &param);

	rcu_thread_affine_rnp(kworker->task, rnp);
	wake_up_process(kworker->task);
}

/* 启动期创建 expedited GP 总控 kworker，供不可在调用者上下文完成的加速请求使用。 */
static void __init rcu_start_exp_gp_kworker(void)
{
	const char *name = "rcu_exp_gp_kthread_worker";
	struct sched_param param = { .sched_priority = kthread_prio };

	rcu_exp_gp_kworker = kthread_run_worker(0, name);
	if (IS_ERR_OR_NULL(rcu_exp_gp_kworker)) {
		pr_err("Failed to create %s!\n", name);
		rcu_exp_gp_kworker = NULL;
		return;
	}

	if (IS_ENABLED(CONFIG_RCU_EXP_KTHREAD))
		sched_setscheduler_nocheck(rcu_exp_gp_kworker->task, SCHED_FIFO, &param);
}

/* 为 @rnp 按配置创建 boost/expedited 辅助线程；可睡眠，重复调用会跳过已有线程。 */
static void rcu_spawn_rnp_kthreads(struct rcu_node *rnp)
{
	if (rcu_scheduler_fully_active) {
		mutex_lock(&rnp->kthread_mutex);
		rcu_spawn_one_boost_kthread(rnp);
		rcu_spawn_exp_par_gp_kworker(rnp);
		mutex_unlock(&rnp->kthread_mutex);
	}
}

/*
 * Invoked early in the CPU-online process, when pretty much all services
 * are available.  The incoming CPU is not present.
 *
 * Initializes a CPU's per-CPU RCU data.  Note that only one online or
 * offline event can be happening at a given time.  Note also that we can
 * accept some slop in the rsp->gp_seq access due to the fact that this
 * CPU cannot possibly have any non-offloaded RCU callbacks in flight yet.
 * And any offloaded callbacks are being numbered elsewhere.
 */
/*
 * rcutree_prepare_cpu() - CPU hotplug prepare 阶段初始化 rdp、callback 处理与 node 线程。
 * 可睡眠；成功 0，失败负 errno 阻止上线。尚未把 CPU 纳入当前/未来 GP qsmask。
 */
/*
 * 修正说明：当前实现没有失败出口，始终返回 0；辅助线程创建失败由各 helper 自行
 * 告警/降级，不通过本函数阻止 CPU 上线。@cpu 是有效 possible CPU 编号，函数借用
 * 其 rdp/context-tracking 对象，hotplug 串行保证无并发 online/offline 操作。
 */
int rcutree_prepare_cpu(unsigned int cpu)
{
	unsigned long flags;
	struct context_tracking *ct = per_cpu_ptr(&context_tracking, cpu);
	struct rcu_data *rdp = per_cpu_ptr(&rcu_data, cpu);
	struct rcu_node *rnp = rcu_get_root();

	/* Set up local state, ensuring consistent view of global state. */
	/*
	 * 阶段 1：在根锁与 IRQ 关闭区间重置 FQS 水位、batch 限额和 context-tracking。
	 * @rdp/@ct 是目标 CPU 的借用 per-CPU 对象；CPU 尚未运行，ct->nesting 普通写不会
	 * 与目标 CPU 撕裂。
	 */
	raw_spin_lock_irqsave_rcu_node(rnp, flags);
	rdp->qlen_last_fqs_check = 0;
	rdp->n_force_qs_snap = READ_ONCE(rcu_state.n_force_qs);
	rdp->blimit = blimit;
	ct->nesting = 1;	/* CPU not up, no tearing. */
	/* 行尾英文说明：目标 CPU 尚未启动，不会并发修改 nesting，普通赋值足够。 */
	raw_spin_unlock_rcu_node(rnp);		/* irqs remain disabled. */
	/* 行尾英文说明：只释放根锁，IRQ 继续关闭到叶节点状态初始化完成。 */

	/*
	 * Only non-NOCB CPUs that didn't have early-boot callbacks need to be
	 * (re-)initialized.
	 */
	/*
	 * 阶段 2：只有没有早期 callback 帮其初始化过的列表需要重新启用；已启用列表
	 * 可能持有 boot callback，绝不能清空重建。NOCB 列表也由其专用初始化路径维护。
	 */
	if (!rcu_segcblist_is_enabled(&rdp->cblist))
		rcu_segcblist_init(&rdp->cblist);  /* Re-enable callbacks. */

	/*
	 * Add CPU to leaf rcu_node pending-online bitmask.  Any needed
	 * propagation up the rcu_node tree will happen at the beginning
	 * of the next grace period.
	 */
	/*
	 * 阶段 3：在所属叶锁下让 per-CPU 序号追上节点并初始化 QS/irq_work。真正把 CPU
	 * 的在线位向父树传播延迟到下一次 rcu_gp_init()，使 hotplug 与建树在同一锁协议
	 * 下提交。
	 */
	rnp = rdp->mynode;
	raw_spin_lock_rcu_node(rnp);		/* irqs already disabled. */
	/* 行尾英文说明：沿用阶段 1 保存的 IRQ 状态，只获取叶节点锁。 */
	rdp->gp_seq = READ_ONCE(rnp->gp_seq);
	rdp->gp_seq_needed = rdp->gp_seq;
	rdp->cpu_no_qs.b.norm = true;
	rdp->core_needs_qs = false;
	rdp->rcu_iw_pending = false;
	rdp->rcu_iw = IRQ_WORK_INIT_HARD(rcu_iw_handler);
	rdp->rcu_iw_gp_seq = rdp->gp_seq - 1;
	trace_rcu_grace_period(rcu_state.name, rdp->gp_seq, TPS("cpuonl"));
	raw_spin_unlock_irqrestore_rcu_node(rnp, flags);

	rcu_preempt_deferred_qs_init(rdp);
	rcu_spawn_rnp_kthreads(rnp);
	rcu_spawn_cpu_nocb_kthread(cpu);
	ASSERT_EXCLUSIVE_WRITER(rcu_state.n_online_cpus);
	WRITE_ONCE(rcu_state.n_online_cpus, rcu_state.n_online_cpus + 1);

	return 0;
}

/*
 * Has the specified (known valid) CPU ever been fully online?
 */
/* 返回指定 CPU 是否曾完成 RCU online 全阶段；用于区分首次上线与热插拔恢复。 */
bool rcu_cpu_beenfullyonline(int cpu)
{
	struct rcu_data *rdp = per_cpu_ptr(&rcu_data, cpu);

	return smp_load_acquire(&rdp->beenonline);
}

/*
 * Near the end of the CPU-online process.  Pretty much all services
 * enabled, and the CPU is now very much alive.
 */
/*
 * rcutree_online_cpu - CPU 上线的最后一步：置 ffmask 位并释放 tick 依赖
 * @cpu: 正在上线的 CPU 编号
 *
 * 调用时机：CPU 热插拔 online 阶段（cpuhp_step），或 boot 时由 rcu_init()
 * 在 rcutree_report_cpu_starting() 之后调用，此时调度器基本可用。
 *
 * "fully functional"（ffmask）表示该 CPU 已完全就绪，可参与回调卸载决策。
 * 与 qsmaskinitnext（下次 GP 等待集合）不同，ffmask 用于 NOCB 子系统判断
 * 目标 CPU 是否能接收卸载的回调。
 */
/*
 * rcutree_online_cpu() - 把 CPU 发布到 rcu_node 在线掩码并启用本地 callback/QS 处理。
 * hotplug 串行，可睡眠；成功 0。返回后未来 GP 可等待该 CPU。
 */
int rcutree_online_cpu(unsigned int cpu)
{
	unsigned long flags;
	struct rcu_data *rdp;
	struct rcu_node *rnp;

	rdp = per_cpu_ptr(&rcu_data, cpu);
	rnp = rdp->mynode;
	raw_spin_lock_irqsave_rcu_node(rnp, flags);
	/* 将该 CPU 加入叶节点的 ffmask（fully functional mask），
	 * 标志该 CPU 已完全上线，可以安全地接收 NOCB 卸载回调。 */
	rnp->ffmask |= rdp->grpmask;
	raw_spin_unlock_irqrestore_rcu_node(rnp, flags);

	if (rcu_scheduler_active == RCU_SCHEDULER_INACTIVE)
		return 0; /* Too early in boot for scheduler work. */
	/*
	 * 行尾英文说明：调度器仍未启动时不能操作 tick dependency；ffmask 已发布，
	 * 剩余调度相关工作由后续启动阶段完成。
	 */

	// Stop-machine done, so allow nohz_full to disable tick.
	// CPU online 过程使用了 stop-machine，期间 RCU 强制 tick 持续运行以
	// 确保 QS（静默状态）能被及时检测。现在 CPU 完全在线，stop-machine 结束，
	// 清除 RCU 对 tick 的依赖位，允许 nohz_full CPU 再次停掉周期性 tick。
	tick_dep_clear(TICK_DEP_BIT_RCU);
	return 0;
}

/*
 * Mark the specified CPU as being online so that subsequent grace periods
 * (both expedited and normal) will wait on it.  Note that this means that
 * incoming CPUs are not allowed to use RCU read-side critical sections
 * until this function is called.  Failing to observe this restriction
 * will result in lockdep splats.
 *
 * Note that this function is special in that it is invoked directly
 * from the incoming CPU rather than from the cpuhp_step mechanism.
 * This is because this function must be invoked at a precise location.
 * This incoming CPU must not have enabled interrupts yet.
 *
 * This mirrors the effects of rcutree_report_cpu_dead().
 */
/*
 * rcutree_report_cpu_starting - 将正在启动的 CPU 注册到 RCU rcu_node 树
 * @cpu: 正在启动的 CPU 编号
 *
 * 调用时机：目标 CPU 自身在极早期启动（secondary_start_kernel）中调用，
 * 此时中断必须关闭（有 lockdep 断言保证）。
 *
 * 主要工作：
 *   1. 将该 CPU 加入叶节点的 qsmaskinitnext（下次 GP 开始时需等待的 CPU 集合）
 *   2. 将该 CPU 加入 expmaskinitnext（expedited GP 等待集合）
 *   3. 若是首次上线，递增 rcu_state.ncpus
 *   4. 处理 offline 期间可能发生的 GP 序号回绕
 *   5. 设置 beenonline 标志，发出完整内存屏障
 */
/* CPU starting 原子阶段同步 dynticks/GP 快照，确保首个中断/调度点不会误报旧代 QS。 */
void rcutree_report_cpu_starting(unsigned int cpu)
{
	unsigned long mask;
	struct rcu_data *rdp;
	struct rcu_node *rnp;
	bool newcpu;

	/* 严格要求中断关闭：arch_spin_lock（ofl_lock）不能在中断上下文中使用，
	 * 且整个注册过程需要原子性。 */
	lockdep_assert_irqs_disabled();
	rdp = per_cpu_ptr(&rcu_data, cpu);

	/* 幂等保护：CPU 热插拔路径可能重入，已执行过则直接返回。 */
	if (rdp->cpu_started)
		return;
	rdp->cpu_started = true;

	rnp = rdp->mynode;
	/* 该 CPU 在父节点 qsmask 中对应的位掩码。 */
	mask = rdp->grpmask;

	/* ofl_lock（offline lock）：arch spinlock，不参与调度器，
	 * 专为极早期/极晚期 CPU 状态变更设计，保护 CPU online/offline 的原子性。 */
	arch_spin_lock(&rcu_state.ofl_lock);

	/* 通知 RCU watching 机制该 CPU 即将在线，更新 dyntick-idle/EQS 账本。 */
	rcu_watching_online();

	/* barrier_lock 保护 rcu_barrier() 操作期间的 CPU 集合一致性：
	 * rcu_barrier() 需要看到一致的 qsmaskinitnext，
	 * 必须在修改该掩码时持有此锁。 */
	raw_spin_lock(&rcu_state.barrier_lock);
	raw_spin_lock_rcu_node(rnp);

	/* 将该 CPU 加入"下次 GP 开始时需等待"的掩码。
	 * 使用 WRITE_ONCE 避免编译器将此写操作与相邻操作合并或重排。 */
	WRITE_ONCE(rnp->qsmaskinitnext, rnp->qsmaskinitnext | mask);

	/* barrier_lock 只需要保护 qsmaskinitnext 的一致性，此处可提前释放。 */
	raw_spin_unlock(&rcu_state.barrier_lock);

	/* 判断是否是该 CPU 首次出现在 expedited 掩码中（区分新 CPU vs 热插拔重上线）。 */
	newcpu = !(rnp->expmaskinitnext & mask);
	/* 将该 CPU 加入 expedited GP 等待集合。 */
	rnp->expmaskinitnext |= mask;

	/* Allow lockless access for expedited grace periods.
	 * smp_store_release：release 语义写，确保 expmaskinitnext 的修改对
	 * 无锁读取 ncpus 的 expedited GP 扫描路径可见（^^^注释指向此保证）。
	 * 若是新 CPU（newcpu=true），递增全系统 RCU 感知的 CPU 计数。 */
	smp_store_release(&rcu_state.ncpus, rcu_state.ncpus + newcpu); /* ^^^ */
	ASSERT_EXCLUSIVE_WRITER(rcu_state.ncpus);

	/* 处理 GP 序号回绕：CPU offline 期间可能发生多个 GP 轮转，
	 * 导致 rdp->gp_seq 远落后于 rnp->gp_seq，需要同步修正。 */
	rcu_gpnum_ovf(rnp, rdp); /* Offline-induced counter wrap? */

	/* 记录上线时的 GP 序号和阶段，用于调试和 stall 超时检测。 */
	rdp->rcu_onl_gp_seq = READ_ONCE(rcu_state.gp_seq);
	rdp->rcu_onl_gp_state = READ_ONCE(rcu_state.gp_state);

	/* An incoming CPU should never be blocking a grace period.
	 * 正常情况下该 CPU offline 时应已从 qsmask 中清除。
	 * 若 qsmask 中仍有该 CPU 的位，说明出现了异常，
	 * 需要立即补报 QS，防止 GP 永久阻塞。 */
	if (WARN_ON_ONCE(rnp->qsmask & mask)) { /* RCU waiting on incoming CPU? */
		/* rcu_report_qs_rnp() *really* wants some flags to restore */
		/*
		 * rcu_report_qs_rnp() 的解锁契约要求一份可恢复的 IRQ flags；尽管当前 IRQ
		 * 本就关闭，仍需 local_irq_save() 生成与其接口匹配的状态。
		 */
		unsigned long flags;

		local_irq_save(flags);
		rcu_disable_urgency_upon_qs(rdp);
		/* Report QS -after- changing ->qsmaskinitnext! */
		/*
		 * CPU 已先加入 future-GP 在线掩码，再为异常遗留的当前 GP 位补报 QS；
		 * 该顺序防止并发 GP 初始化把刚上线 CPU 永久漏出下一轮等待集合。
		 */
		rcu_report_qs_rnp(mask, rnp, rnp->gp_seq, flags);
	} else {
		/* 正常路径：释放叶节点锁。 */
		raw_spin_unlock_rcu_node(rnp);
	}
	arch_spin_unlock(&rcu_state.ofl_lock);

	/* release 语义：标记该 CPU 已至少完整上线过一次。
	 * rcu_cpu_beenfullyonline() 用 smp_load_acquire 读取此标志。 */
	smp_store_release(&rdp->beenonline, true);

	/* Ensure RCU read-side usage follows above initialization.
	 * 完整内存屏障：确保后续的 RCU 读端临界区（rcu_read_lock/unlock）
	 * 能看到上述所有初始化操作的结果，防止乱序执行导致安全漏洞。 */
	smp_mb();
}

/*
 * The outgoing function has no further need of RCU, so remove it from
 * the rcu_node tree's ->qsmaskinitnext bit masks.
 *
 * Note that this function is special in that it is invoked directly
 * from the outgoing CPU rather than from the cpuhp_step mechanism.
 * This is because this function must be invoked at a precise location.
 *
 * This mirrors the effect of rcutree_report_cpu_starting().
 */
/* 当前 CPU 死亡末期标记 dynticks/EQS 与 RCU watching 状态，不能睡眠。 */
void rcutree_report_cpu_dead(void)
{
	unsigned long flags;
	unsigned long mask;
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);
	struct rcu_node *rnp = rdp->mynode;  /* Outgoing CPU's rdp & rnp. */
	/* 行尾英文说明：两个指针都借用自即将离线的当前 CPU，整个函数内对象不会迁移。 */

	/*
	 * IRQS must be disabled from now on and until the CPU dies, or an interrupt
	 * may introduce a new READ-side while it is actually off the QS masks.
	 */
	/*
	 * 从此处到 CPU 真正死亡必须持续关闭 IRQ；一旦先从 qsmask 摘除、又允许中断进入
	 * 新 RCU 读侧，GP 将不再等待该 CPU，可能在读侧结束前回收对象。
	 */
	lockdep_assert_irqs_disabled();
	/*
	 * CPUHP_AP_SMPCFD_DYING was the last call for rcu_exp_handler() execution.
	 * The requested QS must have been reported on the last context switch
	 * from stop machine to idle.
	 */
	/*
	 * CPUHP_AP_SMPCFD_DYING 是 rcu_exp_handler() 可在本 CPU 执行的最后阶段；任何
	 * expedited QS 请求都应已在 stop-machine 切换回 idle 的最后一次上下文切换中
	 * 上报。exp 位仍欠账表示加速 GP 与 hotplug 协议失配。
	 */
	WARN_ON_ONCE(rdp->cpu_no_qs.b.exp);
	// Do any dangling deferred wakeups.
	/*
	 * 在 CPU 消失前兑现悬挂的 nocb 唤醒，否则目标 kthread 可能永远不知道迁移/新增
	 * callback；该操作不重新开启 IRQ。
	 */
	do_nocb_deferred_wakeup(rdp);

	rcu_preempt_deferred_qs(current);

	/* Remove outgoing CPU from mask in the leaf rcu_node structure. */
	/* 取出本 CPU 在叶节点中的成员位，后续用于先报当前 GP QS、再摘未来 GP 在线位。 */
	mask = rdp->grpmask;

	/*
	 * Hold the ofl_lock and rnp lock to avoid races between CPU going
	 * offline and doing a QS report (as below), versus rcu_gp_init().
	 * See Requirements.rst > Hotplug CPU > Concurrent QS Reporting section
	 * for more details.
	 */
	/*
	 * ofl_lock 与叶锁把离线 QS/在线掩码更新同 rcu_gp_init() 串行化，防止 GP 初始化
	 * 在两步之间看到“当前 GP 仍等待 CPU、下一 GP 却已无 CPU”的不一致快照。
	 */
	arch_spin_lock(&rcu_state.ofl_lock);
	raw_spin_lock_irqsave_rcu_node(rnp, flags); /* Enforce GP memory-order guarantee. */
	/*
	 * 行尾英文说明：节点锁的 unlock-lock 屏障链参与 GP 内存序，确保离线前读侧访问
	 * 先于更新侧把本 CPU 视为已静止。
	 */
	rdp->rcu_ofl_gp_seq = READ_ONCE(rcu_state.gp_seq);
	rdp->rcu_ofl_gp_state = READ_ONCE(rcu_state.gp_state);
	if (rnp->qsmask & mask) { /* RCU waiting on outgoing CPU? */
		/* Report quiescent state -before- changing ->qsmaskinitnext! */
		/*
		 * 若当前 GP 仍等待本 CPU，必须在清 qsmaskinitnext 之前先上报本代 QS；反序会
		 * 让 GP init/offline 并发路径无法判断该位由谁负责，造成漏报或提前完成。
		 */
		rcu_disable_urgency_upon_qs(rdp);
		rcu_report_qs_rnp(mask, rnp, rnp->gp_seq, flags);
		raw_spin_lock_irqsave_rcu_node(rnp, flags);
	}
	/* Clear from ->qsmaskinitnext to mark offline. */
	/* 当前 GP 的责任已经结清，现在清 future-GP 在线位，后续 GP 不再等待死亡 CPU。 */
	WRITE_ONCE(rnp->qsmaskinitnext, rnp->qsmaskinitnext & ~mask);
	raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
	arch_spin_unlock(&rcu_state.ofl_lock);
	rdp->cpu_started = false;
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * The outgoing CPU has just passed through the dying-idle state, and we
 * are being invoked from the CPU that was IPIed to continue the offline
 * operation.  Migrate the outgoing CPU's callbacks to the current CPU.
 */
/*
 * rcutree_migrate_callbacks() - 把离线 @cpu 未执行 callbacks 合并到当前存活 CPU。
 * 同时转移长度/过载/lazy 账并请求 GP/core；迁移后死亡 rdp 列表为空。
 */
void rcutree_migrate_callbacks(int cpu)
{
	unsigned long flags;
	struct rcu_data *my_rdp;
	struct rcu_node *my_rnp;
	struct rcu_data *rdp = per_cpu_ptr(&rcu_data, cpu);
	bool needwake;

	if (rcu_rdp_is_offloaded(rdp))
		return;

	raw_spin_lock_irqsave(&rcu_state.barrier_lock, flags);
	if (rcu_segcblist_empty(&rdp->cblist)) {
		raw_spin_unlock_irqrestore(&rcu_state.barrier_lock, flags);
		return;  /* No callbacks to migrate. */
		/* 行尾英文说明：死亡 CPU 队列为空，无 ownership 需要转移。 */
	}

	WARN_ON_ONCE(rcu_rdp_cpu_online(rdp));
	rcu_barrier_entrain(rdp);
	my_rdp = this_cpu_ptr(&rcu_data);
	my_rnp = my_rdp->mynode;
	rcu_nocb_lock(my_rdp); /* irqs already disabled. */
	/* 行尾英文说明：barrier_lock 的 irqsave 已关闭 IRQ，这里只取得目标 cblist 锁。 */
	WARN_ON_ONCE(!rcu_nocb_flush_bypass(my_rdp, NULL, jiffies, false));
	raw_spin_lock_rcu_node(my_rnp); /* irqs already disabled. */
	/* 行尾英文说明：IRQ 仍关闭，叶锁把 callback 分代与 GP 序号快照串行化。 */
	/* Leverage recent GPs and set GP for new callbacks. */
	/*
	 * 合并前先分别按目标叶的最新 gp_seq 推进死亡队列和当前队列，尽量复用已经完成
	 * 的 GP；合并后再推进一次，为新组合列表发布必要的未来 GP 请求。
	 */
	needwake = rcu_advance_cbs(my_rnp, rdp) ||
		   rcu_advance_cbs(my_rnp, my_rdp);
	rcu_segcblist_merge(&my_rdp->cblist, &rdp->cblist);
	raw_spin_unlock(&rcu_state.barrier_lock); /* irqs remain disabled. */
	/* barrier 扫描已能在目标列表看到迁移 callback，可释放 barrier_lock；IRQ 保持关闭。 */
	needwake = needwake || rcu_advance_cbs(my_rnp, my_rdp);
	rcu_segcblist_disable(&rdp->cblist);
	WARN_ON_ONCE(rcu_segcblist_empty(&my_rdp->cblist) != !rcu_segcblist_n_cbs(&my_rdp->cblist));
	check_cb_ovld_locked(my_rdp, my_rnp);
	if (rcu_rdp_is_offloaded(my_rdp)) {
		raw_spin_unlock_rcu_node(my_rnp); /* irqs remain disabled. */
		/* NOCB 目标由 wake helper 在解叶锁后接管，并最终按 @flags 恢复 IRQ。 */
		__call_rcu_nocb_wake(my_rdp, true, flags);
	} else {
		rcu_nocb_unlock(my_rdp); /* irqs remain disabled. */
		/* 普通目标先释放 cblist 锁，IRQ 仍关闭以维持与叶锁的原子区间。 */
		raw_spin_unlock_rcu_node(my_rnp); /* irqs remain disabled. */
		/* 释放叶锁但继续保持 IRQ 关闭，随后统一 local_irq_restore(flags)。 */
	}
	local_irq_restore(flags);
	if (needwake)
		rcu_gp_kthread_wake();
	lockdep_assert_irqs_enabled();
	WARN_ONCE(rcu_segcblist_n_cbs(&rdp->cblist) != 0 ||
		  !rcu_segcblist_empty(&rdp->cblist),
		  "rcu_cleanup_dead_cpu: Callbacks on offline CPU %d: qlen=%lu, 1stCB=%p\n",
		  cpu, rcu_segcblist_n_cbs(&rdp->cblist),
		  rcu_segcblist_first_cb(&rdp->cblist));
}

/*
 * The CPU has been completely removed, and some other CPU is reporting
 * this fact from process context.  Do the remainder of the cleanup.
 * There can only be one CPU hotplug operation at a time, so no need for
 * explicit locking.
 */
/* dead hotplug 阶段迁移 callback、清 node 在线/QS 位并停辅助线程；返回 0。 */
int rcutree_dead_cpu(unsigned int cpu)
{
	ASSERT_EXCLUSIVE_WRITER(rcu_state.n_online_cpus);
	WRITE_ONCE(rcu_state.n_online_cpus, rcu_state.n_online_cpus - 1);
	// Stop-machine done, so allow nohz_full to disable tick.
	/*
	 * stop-machine 与死亡清理均已结束，不再需要 RCU 强制周期 tick；清 dependency 后
	 * nohz_full CPU 可恢复隔离模式。
	 */
	tick_dep_clear(TICK_DEP_BIT_RCU);
	return 0;
}

/*
 * Near the end of the offline process.  Trace the fact that this CPU
 * is going offline.
 */
/* dying 原子阶段停止本 CPU RCU core/tick 依赖并核对已进入 EQS；返回 0。 */
int rcutree_dying_cpu(unsigned int cpu)
{
	bool blkd;
	struct rcu_data *rdp = per_cpu_ptr(&rcu_data, cpu);
	struct rcu_node *rnp = rdp->mynode;

	blkd = !!(READ_ONCE(rnp->qsmask) & rdp->grpmask);
	trace_rcu_grace_period(rcu_state.name, READ_ONCE(rnp->gp_seq),
			       blkd ? TPS("cpuofl-bgp") : TPS("cpuofl"));
	return 0;
}

/*
 * Near the beginning of the process.  The CPU is still very much alive
 * with pretty much all services enabled.
 */
/* offline 阶段停用 rdp callback 列表并完成剩余 node 状态清理；返回 0。 */
int rcutree_offline_cpu(unsigned int cpu)
{
	unsigned long flags;
	struct rcu_data *rdp;
	struct rcu_node *rnp;

	rdp = per_cpu_ptr(&rcu_data, cpu);
	rnp = rdp->mynode;
	raw_spin_lock_irqsave_rcu_node(rnp, flags);
	rnp->ffmask &= ~rdp->grpmask;
	raw_spin_unlock_irqrestore_rcu_node(rnp, flags);

	// nohz_full CPUs need the tick for stop-machine to work quickly
	/*
	 * offline 前的 stop-machine 需要 scheduler tick 及时推进调度/QS，因此暂时设置
	 * 全局 RCU tick dependency；dead 阶段完成后再清除。
	 */
	tick_dep_set(TICK_DEP_BIT_RCU);
	return 0;
}
#endif /* #ifdef CONFIG_HOTPLUG_CPU */
/* 上述条件块仅在 CPU 热插拔支持启用时编译。 */

/*
 * On non-huge systems, use expedited RCU grace periods to make suspend
 * and hibernation run faster.
 */
/* rcu_pm_notify() 在 suspend/hibernate 通知中调整 RCU watching、barrier 与 GP 策略。 */
static int rcu_pm_notify(struct notifier_block *self,
			 unsigned long action, void *hcpu)
{
	switch (action) {
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		rcu_async_hurry();
		rcu_expedite_gp();
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
		rcu_unexpedite_gp();
		rcu_async_relax();
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

/*
 * Spawn the kthreads that handle RCU's grace periods.
 */
/*
 * rcu_spawn_gp_kthread() - 创建全局 GP kthread、sync cleanup wq 与 expedited/core
 * 辅助线程，随后发布 scheduler fully active。成功 0，失败为启动 fatal/负 errno。
 */
/*
 * 修正说明：当前函数没有负 errno 出口，始终返回 0。GP kthread 创建失败时只报警
 * 并返回，系统随后可能因 RCU 无法推进而出现 OOM/卡死；其他辅助线程也由各 helper
 * 自行告警或降级。入参：无；early-initcall 进程上下文可睡眠。
 */
static int __init rcu_spawn_gp_kthread(void)
{
	unsigned long flags;
	struct rcu_node *rnp;
	struct sched_param sp;
	struct task_struct *t;
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);

	rcu_scheduler_fully_active = 1;
	t = kthread_create(rcu_gp_kthread, NULL, "%s", rcu_state.name);
	if (WARN_ONCE(IS_ERR(t), "%s: Could not start grace-period kthread, OOM is now expected behavior\n", __func__))
		return 0;
	if (kthread_prio) {
		sp.sched_priority = kthread_prio;
		sched_setscheduler_nocheck(t, SCHED_FIFO, &sp);
	}
	rnp = rcu_get_root();
	raw_spin_lock_irqsave_rcu_node(rnp, flags);
	WRITE_ONCE(rcu_state.gp_activity, jiffies);
	WRITE_ONCE(rcu_state.gp_req_activity, jiffies);
	// Reset .gp_activity and .gp_req_activity before setting .gp_kthread.
	/*
	 * 必须先初始化两个活动时间，再以 release 发布 gp_kthread 指针；请求方一旦看见
	 * 非 NULL 线程，就同时能看见有效 watchdog 时间，避免启动瞬间误报 stall。
	 */
	smp_store_release(&rcu_state.gp_kthread, t);  /* ^^^ */
	raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
	wake_up_process(t);
	/* This is a pre-SMP initcall, we expect a single CPU */
	/*
	 * 此 early initcall 发生在 SMP 次级 CPU 上线前，理论上只有 boot CPU；若已多 CPU
	 * 在线，GP 线程与 per-CPU 辅助线程的发布顺序可能不再满足启动假设。
	 */
	WARN_ON(num_online_cpus() > 1);
	/*
	 * Those kthreads couldn't be created on rcu_init() -> rcutree_prepare_cpu()
	 * due to rcu_scheduler_fully_active.
	 */
	/*
	 * rcu_init() 调用 rcutree_prepare_cpu() 时 fully_active 尚为假，不能创建依赖
	 * 调度器的 nocb/boost/exp kthread；现在调度基础设施已可用，为 boot CPU 补建。
	 */
	rcu_spawn_cpu_nocb_kthread(smp_processor_id());
	rcu_spawn_rnp_kthreads(rdp->mynode);
	rcu_spawn_core_kthreads();
	/* Create kthread worker for expedited GPs */
	/* 最后创建 expedited GP 总控 worker，使加速请求可脱离调用者上下文执行。 */
	rcu_start_exp_gp_kworker();
	return 0;
}
early_initcall(rcu_spawn_gp_kthread);

/*
 * This function is invoked towards the end of the scheduler's
 * initialization process.  Before this is called, the idle task might
 * contain synchronous grace-period primitives (during which time, this idle
 * task is booting the system, and such primitives are no-ops).  After this
 * function is called, any synchronous grace-period primitives are run as
 * expedited, with the requesting task driving the grace period forward.
 * A later core_initcall() rcu_set_runtime_mode() will switch to full
 * runtime RCU functionality.
 */
/*
 * 在第一个任务（PID 1）创建之前、调度器首次运行之前调用。
 * 将 RCU 从早期启动模式（INACTIVE）推进到 INIT 模式。
 *
 * 为什么需要这个状态机，而不是一开始就让 RCU 完全工作？
 *
 *   RCU 的宽限期机制依赖"所有 CPU 都经历过一次上下文切换"来判断没有读者
 *   持有旧数据。但内核启动早期只有一个执行流，根本不会发生上下文切换，
 *   宽限期永远无法推进。为此 RCU 用 rcu_scheduler_active 区分三个阶段：
 *
 *   INACTIVE（初始值）：
 *     调度器尚未启动，整个系统只有一个执行流，不存在并发读者。
 *     此时 rcu_blocking_is_gp() 返回 true，synchronize_rcu() 直接退化为
 *     一条内存屏障（barrier()）就够了，完全不需要等待任何宽限期。
 *     call_rcu() 提交的回调也会检测到这一状态，做特殊的早期初始化处理。
 *
 *   INIT（本函数设置）：
 *     调度器即将启动，多任务即将出现，从现在起必须走完整的宽限期流程。
 *     但 RCU 的 GP kthread 还没有被创建，所以 RCU 处于"能检测宽限期但
 *     还没有专属线程来驱动"的过渡状态。
 *
 *   RUNNING（由 core_initcall(rcu_set_runtime_mode) 设置）：
 *     RCU kthread 全部启动完毕，进入全功能模式。
 *     kfree_rcu() 的批量回收等依赖 kthread 的功能也在此时激活。
 */
/*
 * rcu_scheduler_starting() - 首个普通任务创建前把 active 从 INACTIVE 推到 INIT。
 * 核对所有 possible CPU 初始 dynticks/online 状态；返回后 synchronize_rcu() 必须
 * 使用真实 GP，不再可退化为单任务屏障。
 */
void rcu_scheduler_starting(void)
{
	unsigned long flags;
	struct rcu_node *rnp;

	/*
	 * 时序断言：此函数必须在"只有一个 CPU 在线且从未发生过上下文切换"
	 * 的窗口内调用，也就是 rest_init() 创建第一个任务之前。
	 *
	 * 如果这两个断言触发，说明启动顺序被破坏了——某处在调度器启动后才
	 * 调用了本函数，RCU 的早期优化路径（INACTIVE 阶段的 barrier 退化）
	 * 就已经在多任务环境下被错误地使用了，可能导致数据损坏。
	 */
	WARN_ON(num_online_cpus() != 1);
	WARN_ON(nr_context_switches() > 0);

	/*
	 * 在切换状态之前，先用当前的 INACTIVE 语义跑一遍同步原语自检。
	 * 仅在开启 CONFIG_PROVE_RCU 时生效，目的是验证 INACTIVE 阶段的
	 * synchronize_rcu/synchronize_rcu_expedited 没有暗藏 bug，
	 * 为后续切换到真正的宽限期检测提供一个干净的基线。
	 */
	rcu_test_sync_prims();

	// Fix up the ->gp_seq counters.
	/*
	 * 对齐节点树中每个 rcu_node 的宽限期序号。
	 *
	 * 问题根源：rcu_init() 初始化各节点时，gp_seq/gp_seq_needed 都被设为 0，
	 * 但全局 rcu_state.gp_seq 在启动过程中可能已经因为某些早期操作被推进。
	 * 如果节点序号落后于全局序号，第一次真正的宽限期请求到来时，
	 * rcu_gp_needed() 会拿节点的 gp_seq_needed 与 rcu_state.gp_seq 比较，
	 * 发现"节点需要的宽限期序号已经满足"，从而直接跳过等待——实际上
	 * 宽限期根本还没有走完，等待它的调用者会拿到错误的"已完成"结论。
	 *
	 * 修复方式：把所有节点的两个序号强制拉齐到全局值，保证第一次宽限期
	 * 请求一定会触发一轮完整的宽限期流程。
	 *
	 * 为什么要关中断：此时硬件中断已经开启，中断处理程序里可能调用
	 * call_rcu() 从而触发对 rcu_node 序号的读写，需要防止并发破坏。
	 */
	local_irq_save(flags);
	rcu_for_each_node_breadth_first(rnp)
		rnp->gp_seq_needed = rnp->gp_seq = rcu_state.gp_seq;
	local_irq_restore(flags);

	// Switch out of early boot mode.
	/*
	 * 切换到 INIT 模式。这一行是本函数最核心的副作用：
	 *
	 *   - rcu_blocking_is_gp() 从此返回 false，synchronize_rcu() 不再走
	 *     barrier() 捷径，而是真正挂起调用者直到宽限期结束。
	 *   - lockdep-RCU 在 INACTIVE 阶段会压制某些误报（因为早期单任务环境
	 *     下很多锁规则还不适用），切换到 INIT 后这些检查全部恢复正常。
	 *   - rcu_poll_gp_seq_start/end 等函数中针对 INACTIVE 的跳过路径
	 *     也会失效，开始执行完整的锁断言和序号更新。
	 *
	 * 注意：此时 RCU GP kthread 尚未创建，宽限期的推进暂时依赖
	 * rcu_check_quiescent_state() 在普通进程上下文切换时被动触发，
	 * 直到 core_initcall(rcu_set_runtime_mode) 将状态推进到 RUNNING。
	 */
	rcu_scheduler_active = RCU_SCHEDULER_INIT;

	/*
	 * 切换后再跑一遍自检，验证 INIT 语义下 synchronize_rcu 走的是真正的
	 * 宽限期路径而非退化路径，确保两次自检之间的状态切换没有引入 bug。
	 */
	rcu_test_sync_prims();
}

/*
 * Helper function for rcu_init() that initializes the rcu_state structure.
 */
/*
 * rcu_init_one() - 根据已计算 geometry 构建 rcu_node 各层父子、grplo/grphi、位掩码
 * 与锁/等待队列。__init 无返回；完成后每 CPU 可映射到唯一叶节点与叶内 bit。
 */
static void __init rcu_init_one(void)
{
	static const char * const buf[] = RCU_NODE_NAME_INIT;
	static const char * const fqs[] = RCU_FQS_NAME_INIT;
	static struct lock_class_key rcu_node_class[RCU_NUM_LVLS];
	static struct lock_class_key rcu_fqs_class[RCU_NUM_LVLS];

	int levelspread[RCU_NUM_LVLS];		/* kids/node in each level. */
	/* 行尾英文说明：levelspread[level] 是该层每个节点拥有的直接子节点数。 */
	int cpustride = 1;
	int i;
	int j;
	struct rcu_node *rnp;

	BUILD_BUG_ON(RCU_NUM_LVLS > ARRAY_SIZE(buf));  /* Fix buf[] init! */
	/* 行尾英文说明：编译期保证锁类名称数组足以覆盖所有可能层级，避免 buf 越界。 */

	/* Silence gcc 4.8 false positive about array index out of range. */
	/*
	 * 运行期再次约束实际层数，使后续数组索引对旧 GCC 也显式可证明；异常值说明
	 * geometry 初始化损坏，无法安全建树，因此直接 panic。
	 */
	if (rcu_num_lvls <= 0 || rcu_num_lvls > RCU_NUM_LVLS)
		panic("rcu_init_one: rcu_num_lvls out of range");

	/* Initialize the level-tracking arrays. */
	/*
	 * 阶段 1：根据各层节点数计算 rcu_state.node[] 中每层的起始指针，并求每节点
	 * 子女数。数组按“根到叶”连续布局，后续广度优先遍历依赖这一物理顺序。
	 */

	for (i = 1; i < rcu_num_lvls; i++)
		rcu_state.level[i] =
			rcu_state.level[i - 1] + num_rcu_lvl[i - 1];
	rcu_init_levelspread(levelspread, num_rcu_lvl);

	/* Initialize the elements themselves, starting from the leaves. */
	/*
	 * 阶段 2：从叶向根计算每个节点覆盖的 CPU 区间、父节点和父层成员位，同时初始化
	 * GP/QS 序号、锁类、阻塞 reader 链和 expedited 等待设施。由叶向根累乘
	 * cpustride，确保 grplo/grphi 与 levelspread 一致。
	 */

	for (i = rcu_num_lvls - 1; i >= 0; i--) {
		cpustride *= levelspread[i];
		rnp = rcu_state.level[i];
		for (j = 0; j < num_rcu_lvl[i]; j++, rnp++) {
			raw_spin_lock_init(&ACCESS_PRIVATE(rnp, lock));
			lockdep_set_class_and_name(&ACCESS_PRIVATE(rnp, lock),
						   &rcu_node_class[i], buf[i]);
			raw_spin_lock_init(&rnp->fqslock);
			lockdep_set_class_and_name(&rnp->fqslock,
						   &rcu_fqs_class[i], fqs[i]);
			rnp->gp_seq = rcu_state.gp_seq;
			rnp->gp_seq_needed = rcu_state.gp_seq;
			rnp->completedqs = rcu_state.gp_seq;
			rnp->qsmask = 0;
			rnp->qsmaskinit = 0;
			rnp->grplo = j * cpustride;
			rnp->grphi = (j + 1) * cpustride - 1;
			if (rnp->grphi >= nr_cpu_ids)
				rnp->grphi = nr_cpu_ids - 1;
			if (i == 0) {
				rnp->grpnum = 0;
				rnp->grpmask = 0;
				rnp->parent = NULL;
			} else {
				rnp->grpnum = j % levelspread[i - 1];
				rnp->grpmask = BIT(rnp->grpnum);
				rnp->parent = rcu_state.level[i - 1] +
					      j / levelspread[i - 1];
			}
			rnp->level = i;
			INIT_LIST_HEAD(&rnp->blkd_tasks);
			rcu_init_one_nocb(rnp);
			init_waitqueue_head(&rnp->exp_wq[0]);
			init_waitqueue_head(&rnp->exp_wq[1]);
			init_waitqueue_head(&rnp->exp_wq[2]);
			init_waitqueue_head(&rnp->exp_wq[3]);
			spin_lock_init(&rnp->exp_lock);
			mutex_init(&rnp->kthread_mutex);
			raw_spin_lock_init(&rnp->exp_poll_lock);
			rnp->exp_seq_poll_rq = RCU_GET_STATE_COMPLETED;
			INIT_WORK(&rnp->exp_poll_wq, sync_rcu_do_polled_gp);
		}
	}

	init_swait_queue_head(&rcu_state.gp_wq);
	init_swait_queue_head(&rcu_state.expedited_wq);
	rnp = rcu_first_leaf_node();
	for_each_possible_cpu(i) {
		while (i > rnp->grphi)
			rnp++;
		per_cpu_ptr(&rcu_data, i)->mynode = rnp;
		per_cpu_ptr(&rcu_data, i)->barrier_head.next =
			&per_cpu_ptr(&rcu_data, i)->barrier_head;
		rcu_boot_init_percpu_data(i);
	}
}

/*
 * Force priority from the kernel command-line into range.
 */
/*
 * sanitize_kthread_prio - 将 RCU kthread 优先级限制到合法范围
 *
 * kthread_prio 可通过内核命令行参数 rcutree.kthread_prio=N 设置，
 * 用于控制 rcuc/N（QS 强制推进）、rcuo/N（NOCB 卸载）等 RCU kthread 的
 * 实时调度优先级（SCHED_FIFO，范围 0~99）。
 *
 * 各条件的来由：
 *   CONFIG_RCU_BOOST + torture：最低 2，torture 测试需要更高优先级以保证
 *     宽限期能及时推进（否则可能触发误报超时）
 *   CONFIG_RCU_BOOST 无 torture：最低 1，boosting kthread 必须是实时优先级，
 *     以便能抢占持有 rcu_read_lock 的低优先级任务
 *   无 CONFIG_RCU_BOOST：允许 0（普通 SCHED_OTHER），无实时优先级要求
 *   上界 99：SCHED_FIFO 的最大合法优先级
 */
/* sanitize_kthread_prio() 将模块参数限制到合法 RT priority，非法值告警并修正。 */
static void __init sanitize_kthread_prio(void)
{
	/* 保存原始值，供日志对比使用。 */
	int kthread_prio_in = kthread_prio;

	/* RCU_BOOST + torture：最低 2，确保 torture 场景下 kthread 能抢占目标任务。 */
	if (IS_ENABLED(CONFIG_RCU_BOOST) && kthread_prio < 2
	    && IS_BUILTIN(CONFIG_RCU_TORTURE_TEST))
		kthread_prio = 2;
	/* RCU_BOOST 无 torture：最低 1，boosting kthread 必须具备实时优先级。 */
	else if (IS_ENABLED(CONFIG_RCU_BOOST) && kthread_prio < 1)
		kthread_prio = 1;
	/* 不允许负值。 */
	else if (kthread_prio < 0)
		kthread_prio = 0;
	/* SCHED_FIFO 优先级上界为 99。 */
	else if (kthread_prio > 99)
		kthread_prio = 99;

	/* 若值被修正，发出 ALERT 级日志，提示用户参数超出合法范围。 */
	if (kthread_prio != kthread_prio_in)
		pr_alert("%s: Limited prio to %d from %d\n",
			 __func__, kthread_prio, kthread_prio_in);
}

/*
 * Compute the rcu_node tree geometry from kernel parameters.  This cannot
 * replace the definitions in tree.h because those are needed to size
 * the ->node array in the rcu_state structure.
 */
/*
 * rcu_init_geometry() - 根据 nr_cpu_ids、fanout/fanout_leaf 计算树层数与各层节点数。
 * 处理精确/自动平衡和参数上限，结果写 rcu_num_lvls/num_rcu_lvl/rcu_num_nodes。
 */
void rcu_init_geometry(void)
{
	ulong d;
	int i;
	static unsigned long old_nr_cpu_ids;
	int rcu_capacity[RCU_NUM_LVLS];
	static bool initialized;

	if (initialized) {
		/*
		 * Warn if setup_nr_cpu_ids() had not yet been invoked,
		 * unless nr_cpus_ids == NR_CPUS, in which case who cares?
		 */
		/*
		 * geometry 只应初始化一次；再次调用时核对 nr_cpu_ids 未变化。若首次调用早于
		 * setup_nr_cpu_ids()，后续实际 CPU 上限变化会使已建树覆盖范围错误；当
		 * nr_cpu_ids 本就等于编译期 NR_CPUS 时则不存在差异。
		 */
		WARN_ON_ONCE(old_nr_cpu_ids != nr_cpu_ids);
		return;
	}

	old_nr_cpu_ids = nr_cpu_ids;
	initialized = true;

	/*
	 * Initialize any unspecified boot parameters.
	 * The default values of jiffies_till_first_fqs and
	 * jiffies_till_next_fqs are set to the RCU_JIFFIES_TILL_FORCE_QS
	 * value, which is a function of HZ, then adding one for each
	 * RCU_JIFFIES_FQS_DIV CPUs that might be on the system.
	 */
	/*
	 * 阶段 1：补齐未指定的 FQS 时间参数。基础值由 HZ 决定，并按可能 CPU 数每
	 * RCU_JIFFIES_FQS_DIV 个增加一个 jiffy；大系统给予更长自然 QS 收集窗口，
	 * 避免过早发送跨 CPU 催促。
	 */
	d = RCU_JIFFIES_TILL_FORCE_QS + nr_cpu_ids / RCU_JIFFIES_FQS_DIV;
	if (jiffies_till_first_fqs == ULONG_MAX)
		jiffies_till_first_fqs = d;
	if (jiffies_till_next_fqs == ULONG_MAX)
		jiffies_till_next_fqs = d;
	adjust_jiffies_till_sched_qs();

	/* If the compile-time values are accurate, just leave. */
	/*
	 * 编译期 fanout 与实际 CPU 上限完全匹配时，tree.h 的静态 geometry 已准确，
	 * 无需重算全局层数和数组边界。
	 */
	if (rcu_fanout_leaf == RCU_FANOUT_LEAF &&
	    nr_cpu_ids == NR_CPUS)
		return;
	pr_info("Adjusting geometry for rcu_fanout_leaf=%d, nr_cpu_ids=%u\n",
		rcu_fanout_leaf, nr_cpu_ids);

	/*
	 * The boot-time rcu_fanout_leaf parameter must be at least two
	 * and cannot exceed the number of bits in the rcu_node masks.
	 * Complain and fall back to the compile-time values if this
	 * limit is exceeded.
	 */
	/*
	 * 阶段 2：验证叶扇出。至少为二才能形成有效汇聚；最多 BITS_PER_LONG，因为
	 * qsmask/grpmask 用 unsigned long 的一位表示一个成员。非法参数回退到编译期
	 * geometry，不能带着截断位图继续启动。
	 */
	if (rcu_fanout_leaf < 2 || rcu_fanout_leaf > BITS_PER_LONG) {
		rcu_fanout_leaf = RCU_FANOUT_LEAF;
		WARN_ON(1);
		return;
	}

	/*
	 * Compute number of nodes that can be handled an rcu_node tree
	 * with the given number of levels.
	 */
	/*
	 * 阶段 3：rcu_capacity[level] 表示使用 level+1 层时可覆盖的最大 CPU 数。
	 * 叶层由 rcu_fanout_leaf 决定，每增加一层再乘内部节点固定扇出。
	 */
	rcu_capacity[0] = rcu_fanout_leaf;
	for (i = 1; i < RCU_NUM_LVLS; i++)
		rcu_capacity[i] = rcu_capacity[i - 1] * RCU_FANOUT;

	/*
	 * The tree must be able to accommodate the configured number of CPUs.
	 * If this limit is exceeded, fall back to the compile-time values.
	 */
	/*
	 * 即使使用最大允许层数仍容纳不了 nr_cpu_ids，也必须回退；继续计算会令某些 CPU
	 * 无所属叶节点，QS 永远无法正确汇聚。
	 */
	if (nr_cpu_ids > rcu_capacity[RCU_NUM_LVLS - 1]) {
		rcu_fanout_leaf = RCU_FANOUT_LEAF;
		WARN_ON(1);
		return;
	}

	/* Calculate the number of levels in the tree. */
	/* 选择第一个容量足够的层级，得到满足 CPU 数的最浅树，减少上报路径和锁层数。 */
	for (i = 0; nr_cpu_ids > rcu_capacity[i]; i++) {
	}
	rcu_num_lvls = i + 1;

	/* Calculate the number of rcu_nodes at each level of the tree. */
	/*
	 * 阶段 4：从根到叶计算各层实际节点数。每个节点覆盖 cap 个 CPU，向上取整保留
	 * 最后一个不满节点；num_rcu_lvl[] 随后决定 node[] 的层起始偏移。
	 */
	for (i = 0; i < rcu_num_lvls; i++) {
		int cap = rcu_capacity[(rcu_num_lvls - 1) - i];
		num_rcu_lvl[i] = DIV_ROUND_UP(nr_cpu_ids, cap);
	}

	/* Calculate the total number of rcu_node structures. */
	/* 汇总所有层节点数，限定后续初始化与调试遍历的有效 node[] 区间。 */
	rcu_num_nodes = 0;
	for (i = 0; i < rcu_num_lvls; i++)
		rcu_num_nodes += num_rcu_lvl[i];
}

/*
 * Dump out the structure of the rcu_node combining tree associated
 * with the rcu_state structure.
 */
/* dump helper 打印启动后的 node 层级、CPU 范围和 mask，受 dump_tree 参数控制。 */
static void __init rcu_dump_rcu_node_tree(void)
{
	int level = 0;
	struct rcu_node *rnp;

	pr_info("rcu_node tree layout dump\n");
	pr_info(" ");
	rcu_for_each_node_breadth_first(rnp) {
		if (rnp->level != level) {
			pr_cont("\n");
			pr_info(" ");
			level = rnp->level;
		}
		pr_cont("%d:%d ^%d  ", rnp->grplo, rnp->grphi, rnp->grpnum);
	}
	pr_cont("\n");
}

struct workqueue_struct *rcu_gp_wq;

/*
 * rcu_init - 初始化 Tree RCU 子系统（SMP 版本）
 *
 * 调用时机：start_kernel() 中，内存分配器和 cpumask 就绪后，
 * 调度器完全启动前。此时只有 boot CPU 在运行，无需 CPU 热插拔保护。
 *
 * Tree RCU 是 SMP 系统上的 RCU 实现，使用层次化的 rcu_node 树来追踪
 * 各 CPU 的静默状态（QS），避免所有 CPU 都竞争同一把锁。
 *
 * 初始化顺序有严格依赖关系：
 *   rcu_init_geometry → rcu_init_one → prepare/report/online（三步 CPU 注册）
 * 每一步都依赖前一步的结果，不能调换顺序。
 */
/*
 * rcu_init() - Tree RCU 主初始化入口。
 * 初始化 geometry/node 树、所有 possible CPU rdp、softirq、hotplug/notifier、nocb 与
 * barrier/boost 状态；随后 initcall 再创建线程。无返回，关键不变量失败会 BUG/panic。
 */
void __init rcu_init(void)
{
	int cpu = smp_processor_id();

	/* 若开启 CONFIG_PROVE_RCU 自测，注册早期 RCU/SRCU 回调，
	 * 供 late_initcall 阶段的 rcu_verify_early_boot_tests() 验证正确性。
	 * 非 PROVE_RCU 配置下此函数为空实现，零开销。 */
	rcu_early_boot_tests();

	/* 打印 RCU 变体名称（Preemptible/Hierarchical）和所有非默认启动参数的警告。
	 * 同时修正 nohz_full_patience_delay 的越界值并转换为 jiffies。 */
	rcu_bootup_announce();

	/* 将 kthread_prio 夹在合法范围内（0~99），
	 * CONFIG_RCU_BOOST 时最低 1（torture 测试时最低 2）。 */
	sanitize_kthread_prio();

	/* 根据实际 nr_cpu_ids 和 rcu_fanout_leaf 计算 rcu_node 树的几何形状：
	 * rcu_num_lvls（层数）、num_rcu_lvl[]（每层节点数）、rcu_num_nodes（总节点数），
	 * 以及 FQS（Force Quiescent State）扫描的 jiffies 延迟。
	 * 必须在 rcu_init_one() 之前调用，因为后者依赖这些计算结果。 */
	rcu_init_geometry();

	/* 初始化 rcu_state 中所有 rcu_node 节点和每 CPU rcu_data：
	 * 建立父子指针、grplo/grphi CPU 范围、qsmask 位掩码，
	 * 初始化各节点的锁、等待队列、回调链表等数据结构。
	 * 完成后树结构就绪，可以开始接受 GP 请求。 */
	rcu_init_one();

	/* 可选：通过 rcutree.dump_tree=1 命令行参数触发，
	 * 将完整的 rcu_node 树结构打印到 dmesg，用于调试树形拓扑。 */
	if (dump_tree)
		rcu_dump_rcu_node_tree();

	/* 注册 RCU softirq 处理函数 rcu_core_si（处理回调执行和 QS 检测）。
	 * use_softirq=0（rcutree.use_softirq=0）时 RCU 核心处理由 rcuc/N kthread 承担，
	 * 避免 softirq 抢占实时任务；默认（use_softirq=1）用 softirq，延迟更低。 */
	if (use_softirq)
		open_softirq(RCU_SOFTIRQ, rcu_core_si);

	/*
	 * We don't need protection against CPU-hotplug here because
	 * this is called early in boot, before either interrupts
	 * or the scheduler are operational.
	 *
	 * 注册电源管理通知回调 rcu_pm_notify，在系统 suspend/resume 时
	 * 确保 RCU 宽限期能安全暂停和恢复，避免 suspend 时 GP 永久阻塞。
	 */
	pm_notifier(rcu_pm_notify, 0);

	/* 断言此时只有 boot CPU 在线——三步 CPU 注册假设单 CPU 环境，
	 * 无需持锁保护。若此时已有其他 CPU 上线则说明初始化顺序有误。 */
	WARN_ON(num_online_cpus() > 1); // Only one CPU this early in boot.
	/* 行尾英文说明：RCU 树注册发生在极早启动期，按设计此时只能有 boot CPU 在线。 */

	/* boot CPU 的三步注册（对应 CPU 热插拔的三个阶段）：
	 *
	 * prepare：初始化 per-CPU rcu_data（回调链表、GP 序号、IRQ work 等），
	 *          在叶节点中标记 CPU 为 pending-online，创建 rcuc/rcuog kthread。
	 *
	 * report_starting：将 CPU 加入 qsmaskinitnext 和 expmaskinitnext，
	 *                  递增 ncpus，设置 beenonline，发出完整内存屏障。
	 *                  必须在中断关闭时调用（arch_spin_lock 要求）。
	 *
	 * online：置叶节点 ffmask 位（fully functional），
	 *         清除 RCU 对 tick 的依赖（允许 nohz_full 停掉 tick）。 */
	rcutree_prepare_cpu(cpu);
	rcutree_report_cpu_starting(cpu);
	rcutree_online_cpu(cpu);

	/* Create workqueue for Tree SRCU and for expedited GPs.
	 *
	 * rcu_gp_wq（WQ_MEM_RECLAIM | WQ_PERCPU）：
	 *   用于 Tree SRCU 的宽限期推进和 expedited GP 的异步启动。
	 *   WQ_MEM_RECLAIM 确保内存压力下仍能分配 worker，防止死锁。
	 *
	 * sync_wq（WQ_MEM_RECLAIM | WQ_UNBOUND）：
	 *   用于 synchronize_rcu() 等同步操作的等待，WQ_UNBOUND 允许跨 CPU 调度。 */
	rcu_gp_wq = alloc_workqueue("rcu_gp", WQ_MEM_RECLAIM | WQ_PERCPU, 0);
	WARN_ON(!rcu_gp_wq);

	sync_wq = alloc_workqueue("sync_wq", WQ_MEM_RECLAIM | WQ_UNBOUND, 0);
	WARN_ON(!sync_wq);

	/* Fill in default value for rcutree.qovld boot parameter.
	 * -After- the rcu_node ->lock fields are initialized!
	 *
	 * qovld_calc：回调队列过载阈值，超过此值时 GP 推进会更积极。
	 * qovld < 0 表示用户未通过命令行指定，使用 DEFAULT_RCU_QOVLD_MULT * qhimark
	 * 作为默认值（qhimark 是回调队列高水位，乘以倍数得到过载阈值）。
	 * 必须在 rcu_node 锁初始化后计算，因为后续使用此值时需要持锁。 */
	if (qovld < 0)
		qovld_calc = DEFAULT_RCU_QOVLD_MULT * qhimark;
	else
		qovld_calc = qovld;

	// Kick-start in case any polled grace periods started early.
	// 若在 rcu_init 之前就有代码调用了 start_poll_synchronize_rcu_expedited()
	// 注册了 polled GP 请求，此处启动 expedited GP 确保它们能被处理。
	(void)start_poll_synchronize_rcu_expedited();

	/* 对同步原语（synchronize_rcu 等）做基本正确性断言测试，
	 * 验证 rcu_init 完成后基本 RCU 操作可以正常工作。 */
	rcu_test_sync_prims();

	/* 初始化 Tasks RCU 的回调链表（用于追踪任务上下文的 RCU 宽限期，
	 * 如 BPF、ftrace 等需要等待所有正在执行的任务通过安全点）。 */
	tasks_cblist_init_generic();
}

#include "tree_stall.h"
#include "tree_exp.h"
#include "tree_nocb.h"
#include "tree_plugin.h"
