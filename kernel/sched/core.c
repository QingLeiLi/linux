// SPDX-License-Identifier: GPL-2.0-only
/*
 * 中文学习导读（OpenAI GPT-5 Codex，2026-07-28）
 *
 * 本文件是通用调度框架的“状态转换中枢”，而不是某一种调度策略的实现：
 * fair/rt/deadline/ext/idle 等 class 在各自文件决定“同一队列中谁更合适”，
 * core.c 则负责把 task、每 CPU runqueue 与体系结构 context switch 串成统一协议。
 *
 * 建议沿以下主线阅读：
 *
 *   阻塞方设置 task state
 *       → try_to_wake_up() 稳定 task 状态与 CPU
 *       → ttwu_queue() 在目标 rq 发布 runnable task
 *       → resched_curr() 通知目标 CPU
 *       → schedule()/__schedule() 选择 next
 *       → context_switch() 切换 mm、寄存器栈与 rq 锁的归属
 *       → finish_task_switch() 完成 prev 的延迟清理
 *
 * fork 的另一条发布路径是 sched_fork() → wake_up_new_task()；CPU 亲和性由
 * __set_cpus_allowed_ptr() 与迁移 stopper 协作；CPU hotplug 则逐步阻止新任务
 * 进入、排空 rq，再通知各调度类。文件后半的 CPU cgroup 接口把用户配置转换为
 * task_group 层级约束；MM CID 维护同一 mm 在 CPU 间迁移时的并发标识。
 *
 * 核心对象与 ownership：
 * - task_struct 由进程生命周期持有；调度器通常只借用指针，跨越解锁或异步
 *   wake-list 边界时必须依靠 task 引用、PI 锁或明确的 task 生命周期规则。
 * - 每个 CPU 唯一拥有一个 struct rq。rq->lock 串行化该 CPU 的可运行集合、
 *   curr 和调度类队列；p->pi_lock 则稳定 task 的状态、CPU 归属及 PI 相关字段。
 * - task_group 通过 cgroup 层级发布；先从调度器索引摘除，再经 RCU 宽限期释放，
 *   因而“读侧仍能解引用”与“配置仍可改变”是两种不同保证。
 *
 * 主要并发规则：
 * - 单 rq 操作持 rq->lock；双 rq 操作按地址顺序取锁，避免 ABBA。
 * - 唤醒路径用 p->pi_lock、task state 的 acquire/release 语义和 rq 锁共同避免
 *   lost wakeup；远端入队可先放入 lockless wake_list，再由目标 CPU 批量接收。
 * - schedule() 跨 context_switch() 交接 rq 锁：prev CPU 上的 prepare 阶段加锁，
 *   next task 返回后由 finish_task_switch() 完成解锁和 prev 的最终处理。
 *
 * 方案权衡：per-CPU rq 让常见 enqueue/pick/switch 不争用全局锁，但任务迁移、
 * SMT core scheduling、热插拔和分层带宽必须额外协调多个 CPU；异步 wake-list
 * 减少远端 rq 锁竞争，却增加了“已请求唤醒但尚未真正入队”的过渡状态。
 */
/*
 *  kernel/sched/core.c
 *
 *  Core kernel CPU scheduler code
 *
 *  Copyright (C) 1991-2002  Linus Torvalds
 *  Copyright (C) 1998-2024  Ingo Molnar, Red Hat
 */
#define INSTANTIATE_EXPORTED_MIGRATE_DISABLE
#include <linux/sched.h>
#include <linux/highmem.h>
#include <linux/hrtimer_api.h>
#include <linux/ktime_api.h>
#include <linux/sched/signal.h>
#include <linux/syscalls_api.h>
#include <linux/debug_locks.h>
#include <linux/prefetch.h>
#include <linux/capability.h>
#include <linux/pgtable_api.h>
#include <linux/wait_bit.h>
#include <linux/jiffies.h>
#include <linux/spinlock_api.h>
#include <linux/cpumask_api.h>
#include <linux/lockdep_api.h>
#include <linux/hardirq.h>
#include <linux/softirq.h>
#include <linux/refcount_api.h>
#include <linux/topology.h>
#include <linux/sched/clock.h>
#include <linux/sched/cond_resched.h>
#include <linux/sched/cputime.h>
#include <linux/sched/debug.h>
#include <linux/sched/hotplug.h>
#include <linux/sched/init.h>
#include <linux/sched/isolation.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/mm.h>
#include <linux/sched/nohz.h>
#include <linux/sched/rseq_api.h>
#include <linux/sched/rt.h>

#include <linux/blkdev.h>
#include <linux/context_tracking.h>
#include <linux/cpuset.h>
#include <linux/delayacct.h>
#include <linux/init_task.h>
#include <linux/interrupt.h>
#include <linux/ioprio.h>
#include <linux/kallsyms.h>
#include <linux/kcov.h>
#include <linux/kprobes.h>
#include <linux/llist_api.h>
#include <linux/mmu_context.h>
#include <linux/mmzone.h>
#include <linux/mutex_api.h>
#include <linux/nmi.h>
#include <linux/nospec.h>
#include <linux/perf_event_api.h>
#include <linux/profile.h>
#include <linux/psi.h>
#include <linux/rcuwait_api.h>
#include <linux/rseq.h>
#include <linux/sched/wake_q.h>
#include <linux/scs.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/vtime.h>
#include <linux/wait_api.h>
#include <linux/workqueue_api.h>
#include <linux/livepatch_sched.h>

#ifdef CONFIG_PREEMPT_DYNAMIC
# ifdef CONFIG_GENERIC_IRQ_ENTRY
#  include <linux/irq-entry-common.h>
# endif
#endif

#include <uapi/linux/sched/types.h>

#include <asm/irq_regs.h>
#include <asm/switch_to.h>
#include <asm/tlb.h>

#define CREATE_TRACE_POINTS
#include <linux/sched/rseq_api.h>
#include <trace/events/sched.h>
#include <trace/events/ipi.h>
#undef CREATE_TRACE_POINTS

#include "sched.h"
#include "stats.h"

#include "autogroup.h"
#include "pelt.h"
#include "smp.h"

#include "../workqueue_internal.h"
#include "../../io_uring/io-wq.h"
#include "../smpboot.h"
#include "../locking/mutex.h"

EXPORT_TRACEPOINT_SYMBOL_GPL(ipi_send_cpu);
EXPORT_TRACEPOINT_SYMBOL_GPL(ipi_send_cpumask);

/*
 * Export tracepoints that act as a bare tracehook (ie: have no trace event
 * associated with them) to allow external modules to probe them.
 */
/*
 * 上述 tracepoint 只充当裸 trace hook，没有配套的普通 trace event；导出符号后，
 * 外部模块才能注册 probe。它们观察 PELT、容量和调度状态的瞬时值，不持有被观察
 * task/rq 的引用，也不构成状态提交或同步屏障。
 */
EXPORT_TRACEPOINT_SYMBOL_GPL(pelt_cfs_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(pelt_rt_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(pelt_dl_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(pelt_irq_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(pelt_se_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(pelt_hw_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_cpu_capacity_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_overutilized_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_util_est_cfs_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_util_est_se_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_update_nr_running_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_compute_energy_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_entry_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_exit_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_set_need_resched_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_dl_throttle_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_dl_replenish_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_dl_update_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_dl_server_start_tp);
EXPORT_TRACEPOINT_SYMBOL_GPL(sched_dl_server_stop_tp);

DEFINE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);
DEFINE_PER_CPU(struct rnd_state, sched_rnd_state);

/*
 * runqueues 是整个文件最重要的长期对象：每个 possible CPU 一个 rq，并按共享
 * cacheline 对齐以避免无关 CPU 的锁和热字段互相 false sharing。调度器初始化后
 * 对象本身永久存在；CPU offline 只改变 rq 的 online/active 状态，不释放它。
 * sched_rnd_state 同样是 per-CPU 状态，只为调度中的随机化选择提供局部熵，
 * 不需要用一把全局锁串行化所有 CPU。
 */

#ifdef CONFIG_SCHED_PROXY_EXEC
DEFINE_STATIC_KEY_TRUE(__sched_proxy_exec);
/*
 * setup_proxy_exec() - 解析 sched_proxy_exec= 启动参数并切换静态分支。
 *
 * @str 指向 __setup 框架借出的参数文本；函数只在启动期读取，不保存指针。
 * 返回 1 表示参数已被本处理器消费；解析失败返回 0 并保留编译期默认值。
 * static_branch_enable/disable 在启动期改写跳转标签，使运行期热路径无需反复
 * 读取普通布尔变量。这里没有 task/rq 状态变化，也不存在可恢复的资源分配。
 */
static int __init setup_proxy_exec(char *str)
{
	bool proxy_enable = true;

	if (*str && kstrtobool(str + 1, &proxy_enable)) {
		pr_warn("Unable to parse sched_proxy_exec=\n");
		return 0;
	}

	if (proxy_enable) {
		pr_info("sched_proxy_exec enabled via boot arg\n");
		static_branch_enable(&__sched_proxy_exec);
	} else {
		pr_info("sched_proxy_exec disabled via boot arg\n");
		static_branch_disable(&__sched_proxy_exec);
	}
	return 1;
}
#else
/*
 * CONFIG_SCHED_PROXY_EXEC=n 时仍保留参数处理器，以便明确告诉用户该启动参数
 * 无法改变未编译进内核的功能。@str 仅为借用输入且无需解析；返回 0 表示未启用。
 */
static int __init setup_proxy_exec(char *str)
{
	pr_warn("CONFIG_SCHED_PROXY_EXEC=n, so it cannot be enabled or disabled at boot time\n");
	return 0;
}
#endif
__setup("sched_proxy_exec", setup_proxy_exec);

/*
 * Debugging: various feature bits
 *
 * If SCHED_DEBUG is disabled, each compilation unit has its own copy of
 * sysctl_sched_features, defined in sched.h, to allow constants propagation
 * at compile time and compiler optimization based on features default.
 */
/*
 * 原文说明这些位控制调度器调试/实验特性。关闭 SCHED_DEBUG 时，每个编译单元
 * 可拥有可常量传播的副本，使编译器裁掉关闭功能；开启时则由统一 sysctl 暴露。
 * features.h 通过重复展开 SCHED_FEAT 生成初值，宏续行中不能插入独立注释。
 */
#define SCHED_FEAT(name, enabled)	\
	(1UL << __SCHED_FEAT_##name) * enabled |
__read_mostly unsigned int sysctl_sched_features =
#include "features.h"
	0;
#undef SCHED_FEAT

/*
 * Print a warning if need_resched is set for the given duration (if
 * LATENCY_WARN is enabled).
 *
 * If sysctl_resched_latency_warn_once is set, only one warning will be shown
 * per boot.
 */
/*
 * 原文说明 LATENCY_WARN 开启时，need_resched 持续超过给定毫秒数会报警；
 * *_once 把噪声限制为每次启动一次。这些 __read_mostly 调优量由 sysctl 读写，
 * 热路径频繁读取，修改不要求与 rq 状态形成事务。
 */
__read_mostly int sysctl_resched_latency_warn_ms = 100;
__read_mostly int sysctl_resched_latency_warn_once = 1;

/*
 * Number of tasks to iterate in a single balance run.
 * Limited because this is done with IRQs disabled.
 */
/*
 * 原文说明一次 load-balance 最多扫描的 task 数。平衡阶段常持 rq 锁并关闭 IRQ，
 * 因而上限不仅是吞吐参数，也是 IRQ latency 的保护阈值；未扫描完的工作留给
 * 后续平衡轮次，而不是在一次临界区内无限完成。
 */
__read_mostly unsigned int sysctl_sched_nr_migrate = SCHED_NR_MIGRATE_BREAK;

__read_mostly int scheduler_running;

/*
 * scheduler_running 是启动阶段的发布标志：初始化代码写入，调度/调试路径读取，
 * 用来区分“基础对象尚未完全可用”和正常运行期。它不是 rq 锁，也不能保护某个
 * task 的字段；真正的对象一致性仍由各自锁和发布顺序保证。
 */

#ifdef CONFIG_SCHED_CORE

DEFINE_STATIC_KEY_FALSE(__sched_core_enabled);

/*
 * Core scheduling 在同一 SMT core 的 sibling 间联合选 task，以 cookie 隔离
 * 不应共享硬件线程的安全域。静态键关闭时普通系统几乎不支付分支成本；开启后，
 * 每个 sibling rq 维护 cookie 有序树，并在选取阶段共同满足“cookie 相容”。
 * 代价是跨 sibling rq 加锁、强制 idle 以及启停时的 stop-machine 式协调。
 */

/* kernel prio, less is more */
/*
 * 原文：这里得到内核内部排序优先级，数值越小越优先。
 *
 * __task_prio() 只借用 @p，在调用者已经稳定其调度类/优先级的上下文中生成
 * core-tree 的粗粒度键；DL server、RT/DL、ext、fair 和 idle 被压缩到可比较
 * 区间。返回值仅用于候选排序，不修改 task，也不替代具体 sched_class 的 pick。
 */
static inline int __task_prio(const struct task_struct *p)
{
	if (p->sched_class == &stop_sched_class) /* trumps deadline */
		return -2;

	if (p->dl_server)
		return -1; /* deadline */

	if (rt_or_dl_prio(p->prio))
		return p->prio; /* [-1, 99] */

	if (p->sched_class == &idle_sched_class)
		return MAX_RT_PRIO + NICE_WIDTH; /* 140 */

	if (task_on_scx(p))
		return MAX_RT_PRIO + MAX_NICE + 1; /* 120, squash ext */

	return MAX_RT_PRIO + MAX_NICE; /* 119, squash fair */
}

/*
 * l(a,b)
 * le(a,b) := !l(b,a)
 * g(a,b)  := l(b,a)
 * ge(a,b) := !l(a,b)
 */

/* real prio, less is less */
/*
 * 原文：真正比较两个 task 的优先关系，结果为 true 表示 @a 应排在 @b 前。
 * cookie 相同只是说明可以共跑；最终顺序仍需结合 class 的优先级和各 class
 * 提供的 prio_less 规则。两个参数均为借用指针，调用者必须稳定相关字段。
 */
static inline bool prio_less(const struct task_struct *a,
			     const struct task_struct *b, bool in_fi)
{

	int pa = __task_prio(a), pb = __task_prio(b);

	if (-pa < -pb)
		return true;

	if (-pb < -pa)
		return false;

	if (pa == -1) { /* dl_prio() doesn't work because of stop_class above */
		const struct sched_dl_entity *a_dl, *b_dl;

		a_dl = &a->dl;
		/*
		 * Since,'a' and 'b' can be CFS tasks served by DL server,
		 * __task_prio() can return -1 (for DL) even for those. In that
		 * case, get to the dl_server's DL entity.
		 */
		/*
		 * 中文：a/b 也可能是由 DL server 服务的 CFS task，此时
		 * __task_prio() 同样返回 DL 的 -1；比较 deadline 必须改取对应
		 * dl_server 的调度实体。
		 */
		if (a->dl_server)
			a_dl = a->dl_server;

		b_dl = &b->dl;
		if (b->dl_server)
			b_dl = b->dl_server;

		return !dl_time_before(a_dl->deadline, b_dl->deadline);
	}

	if (pa == MAX_RT_PRIO + MAX_NICE)	/* fair */
		return cfs_prio_less(a, b, in_fi);

#ifdef CONFIG_SCHED_CLASS_EXT
	if (pa == MAX_RT_PRIO + MAX_NICE + 1)	/* ext */
		return scx_prio_less(a, b, in_fi);
#endif

	return false;
}

/*
 * __sched_core_less() 定义 core cookie 红黑树的严格弱序：先按 cookie 升序，
 * cookie 相同再反转 prio_less()，使最高优先级位于最左侧。比较期间 rq/core
 * 状态由调用者锁稳定，函数不取得 task 引用。
 */
static inline bool __sched_core_less(const struct task_struct *a,
				     const struct task_struct *b)
{
	if (a->core_cookie < b->core_cookie)
		return true;

	if (a->core_cookie > b->core_cookie)
		return false;

	/* flip prio, so high prio is leftmost */
	if (prio_less(b, a, !!task_rq(a)->core->core_forceidle_count))
		return true;

	return false;
}

/*
 * 下面三个红黑树适配 helper 把嵌入 task_struct 的 core_node 转回 task，并建立
 * “先 cookie、同 cookie 再按有效优先级”的稳定排序。less 用于插入，cmp 用于
 * 按 cookie 查找首个候选；参数全是树持有期间的借用指针，不增加 task 引用。
 * core_forceidle_count 会影响同 cookie task 的优先比较，使强制 idle 记账期间
 * 仍选择能够推进该 core 的最高优先级实体。
 */
#define __node_2_sc(node) rb_entry((node), struct task_struct, core_node)

static inline bool rb_sched_core_less(struct rb_node *a, const struct rb_node *b)
{
	return __sched_core_less(__node_2_sc(a), __node_2_sc(b));
}

static inline int rb_sched_core_cmp(const void *key, const struct rb_node *node)
{
	const struct task_struct *p = __node_2_sc(node);
	unsigned long cookie = (unsigned long)key;

	if (cookie < p->core_cookie)
		return -1;

	if (cookie > p->core_cookie)
		return 1;

	return 0;
}

/*
 * sched_core_enqueue()/sched_core_dequeue() - 同步 task 在 core-cookie 红黑树中的
 * 可发现性。@rq 是已持锁的目标 runqueue；@p 是正在普通 class 队列中入/出队的
 * 借用 task；dequeue 的 @flags 沿用调度类出队原因。树节点嵌在 task_struct，
 * 不发生分配或引用转移。调用顺序必须和普通队列状态一致，否则 sibling 选择
 * 可能看到幽灵候选或遗漏 runnable task。
 */
void sched_core_enqueue(struct rq *rq, struct task_struct *p)
{
	if (p->se.sched_delayed)
		return;

	rq->core->core_task_seq++;

	if (!p->core_cookie)
		return;

	rb_add(&p->core_node, &rq->core_tree, rb_sched_core_less);
}

void sched_core_dequeue(struct rq *rq, struct task_struct *p, int flags)
{
	if (p->se.sched_delayed)
		return;

	rq->core->core_task_seq++;

	if (sched_core_enqueued(p)) {
		rb_erase(&p->core_node, &rq->core_tree);
		RB_CLEAR_NODE(&p->core_node);
	}

	/*
	 * Migrating the last task off the cpu, with the cpu in forced idle
	 * state. Reschedule to create an accounting edge for forced idle,
	 * and re-examine whether the core is still in forced idle state.
	 */
	if (!(flags & DEQUEUE_SAVE) && rq->nr_running == 1 &&
	    rq->core->core_forceidle_count && rq->curr == rq->idle)
		resched_curr(rq);
}

/*
 * sched_task_is_throttled() - 询问 @p 的调度类是否禁止它在 @cpu 当前运行。
 *
 * @p 为 core-tree 中的借用候选，@cpu 是逻辑 CPU 编号。class 没有回调时默认
 * 返回 0；非零表示因带宽等原因应跳过。该检查不出队、不持引用，调用者仍须在
 * rq/core 锁保护的候选扫描中使用结果。
 */
static int sched_task_is_throttled(struct task_struct *p, int cpu)
{
	if (p->sched_class->task_is_throttled)
		return p->sched_class->task_is_throttled(p, cpu);

	return 0;
}

/*
 * sched_core_next() - 从 @p 后方寻找同 @cookie 的下一个未 throttled 候选。
 *
 * 输入 @p 必须已在 core_tree，调用者持相应 rq/core 锁；返回树内借用 task，
 * 到达树尾或 cookie 区间结束返回 NULL。它只遍历、不改变红黑树或引用计数。
 */
static struct task_struct *sched_core_next(struct task_struct *p, unsigned long cookie)
{
	struct rb_node *node = &p->core_node;
	int cpu = task_cpu(p);

	do {
		node = rb_next(node);
		if (!node)
			return NULL;

		p = __node_2_sc(node);
		if (p->core_cookie != cookie)
			return NULL;

	} while (sched_task_is_throttled(p, cpu));

	return p;
}

/*
 * Find left-most (aka, highest priority) and unthrottled task matching @cookie.
 * If no suitable task is found, NULL will be returned.
 */
/*
 * sched_core_find() - 在已锁定 @rq 的 core_tree 中找到 @cookie 首个可运行候选。
 *
 * 返回值是 rq 锁有效期内的借用 task；不存在或全部被 throttled 时返回 NULL。
 * rb_find_first 先定位 cookie 区间，再沿区间跳过受限 task，避免扫描其他安全域。
 */
static struct task_struct *sched_core_find(struct rq *rq, unsigned long cookie)
{
	struct task_struct *p;
	struct rb_node *node;

	node = rb_find_first((void *)cookie, &rq->core_tree, rb_sched_core_cmp);
	if (!node)
		return NULL;

	p = __node_2_sc(node);
	if (!sched_task_is_throttled(p, rq->cpu))
		return p;

	return sched_core_next(p, cookie);
}

/*
 * Magic required such that:
 *
 *	raw_spin_rq_lock(rq);
 *	...
 *	raw_spin_rq_unlock(rq);
 *
 * ends up locking and unlocking the _same_ lock, and all CPUs
 * always agree on what rq has what lock.
 *
 * XXX entirely possible to selectively enable cores, don't bother for now.
 */

static DEFINE_MUTEX(sched_core_mutex);
static atomic_t sched_core_count;
static struct cpumask sched_core_mask;

/*
 * sched_core_lock()/sched_core_unlock() - 联合锁住一个 SMT core 的全部 sibling rq。
 *
 * @cpu 只用于定位 sibling mask；@flags 是 IRQ 状态输出/恢复令牌。加锁按 cpumask
 * 固定次序并使用不同 lockdep subclass，返回时本地 IRQ 关闭且所有物理 rq 锁均
 * 持有；unlock 逐一释放后恢复原 IRQ 状态。调用区间不能睡眠。
 */
static void sched_core_lock(int cpu, unsigned long *flags)
	__context_unsafe(/* acquires multiple */)
	__acquires(&runqueues.__lock) /* overapproximation */
{
	const struct cpumask *smt_mask = cpu_smt_mask(cpu);
	int t, i = 0;

	local_irq_save(*flags);
	for_each_cpu(t, smt_mask)
		raw_spin_lock_nested(&cpu_rq(t)->__lock, i++);
}

static void sched_core_unlock(int cpu, unsigned long *flags)
	__context_unsafe(/* releases multiple */)
	__releases(&runqueues.__lock) /* overapproximation */
{
	const struct cpumask *smt_mask = cpu_smt_mask(cpu);
	int t;

	for_each_cpu(t, smt_mask)
		raw_spin_unlock(&cpu_rq(t)->__lock);
	local_irq_restore(*flags);
}

/*
 * __sched_core_flip() - 在所有相关 rq 达到静止边界时切换 core scheduling。
 *
 * @enabled 是目标模式。函数逐个 SMT sibling 集合联合加锁，更新 core_enabled，
 * 再处理 offline CPU；cpus_read_lock() 阻止 hotplug 在中途改变集合。无失败返回。
 * 先建立 rq 数据结构不变量、后让热路径观察新模式，是启停正确性的关键顺序。
 */
static void __sched_core_flip(bool enabled)
{
	unsigned long flags;
	int cpu, t;

	cpus_read_lock();

	/*
	 * Toggle the online cores, one by one.
	 */
	cpumask_copy(&sched_core_mask, cpu_online_mask);
	for_each_cpu(cpu, &sched_core_mask) {
		const struct cpumask *smt_mask = cpu_smt_mask(cpu);

		sched_core_lock(cpu, &flags);

		for_each_cpu(t, smt_mask)
			cpu_rq(t)->core_enabled = enabled;

		cpu_rq(cpu)->core->core_forceidle_start = 0;

		sched_core_unlock(cpu, &flags);

		cpumask_andnot(&sched_core_mask, &sched_core_mask, smt_mask);
	}

	/*
	 * Toggle the offline CPUs.
	 */
	for_each_cpu_andnot(cpu, cpu_possible_mask, cpu_online_mask)
		cpu_rq(cpu)->core_enabled = enabled;

	cpus_read_unlock();
}

/*
 * sched_core_assert_empty() 在模式切换边界检查所有 possible CPU 的 cookie 树为空。
 * 无参数、无返回值；WARN 只报告不变量破坏，不负责修复。只有树为空，切换 rq 锁
 * 关联才不会遗失仍由旧模式索引的 task。
 */
static void sched_core_assert_empty(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		WARN_ON_ONCE(!RB_EMPTY_ROOT(&cpu_rq(cpu)->core_tree));
}

/*
 * __sched_core_enable()/__sched_core_disable() 只由 sched_core_mutex 串行化的 0↔1
 * 引用边界调用。enable 先开静态键，再用 synchronize_rcu() 等待所有按旧锁指针
 * 取锁的读者离开，随后 flip；disable 在确认树空后先 flip 回独立 rq 锁，最后关
 * 静态键。二者无返回值且可等待 RCU，不能在原子上下文调用。
 */
static void __sched_core_enable(void)
{
	static_branch_enable(&__sched_core_enabled);
	/*
	 * Ensure all previous instances of raw_spin_rq_*lock() have finished
	 * and future ones will observe !sched_core_disabled().
	 */
	synchronize_rcu();
	__sched_core_flip(true);
	sched_core_assert_empty();
}

static void __sched_core_disable(void)
{
	sched_core_assert_empty();
	__sched_core_flip(false);
	static_branch_disable(&__sched_core_enabled);
}

/*
 * sched_core_get()/sched_core_put() - 引用计数式启停 core scheduling。
 *
 * get 在第一个用户到来时执行全局 enable；put 把可能较重的最后一次 disable
 * 延后给 work。引用保护的是功能启用状态，不是 task 生命周期。mutex 只串行化
 * 0↔1 边界，常见的非零增减走原子快速路径；无返回值，调用者必须成对使用。
 */
void sched_core_get(void)
{
	if (atomic_inc_not_zero(&sched_core_count))
		return;

	mutex_lock(&sched_core_mutex);
	if (!atomic_read(&sched_core_count))
		__sched_core_enable();

	smp_mb__before_atomic();
	atomic_inc(&sched_core_count);
	mutex_unlock(&sched_core_mutex);
}

/*
 * __sched_core_put() 是最后一个 core-scheduling 用户的延迟释放回调。
 * atomic_dec_and_mutex_lock() 原子判断 1->0 并取得 mutex，只有真正归零者关闭
 * 静态键；work 对象负责把可能较重的全局停用移出调用者路径。
 */
static void __sched_core_put(struct work_struct *work)
{
	if (atomic_dec_and_mutex_lock(&sched_core_count, &sched_core_mutex)) {
		__sched_core_disable();
		mutex_unlock(&sched_core_mutex);
	}
}

void sched_core_put(void)
{
	static DECLARE_WORK(_work, __sched_core_put);

	/*
	 * "There can be only one"
	 *
	 * Either this is the last one, or we don't actually need to do any
	 * 'work'. If it is the last *again*, we rely on
	 * WORK_STRUCT_PENDING_BIT.
	 */
	/*
	 * 中文：“只能有一个”最后释放工作。若不是最后用户，无需 work；若又一次
	 * 成为最后用户，WORK_STRUCT_PENDING_BIT 会合并重复调度。
	 */
	if (!atomic_add_unless(&sched_core_count, -1, 1))
		schedule_work(&_work);
}

#else /* !CONFIG_SCHED_CORE: */

/*
 * 未编译 core scheduling 时，这组空 stub 固定“无需额外 cookie 索引”的策略。
 * 参数仍由调用点借入但不读取；无返回值、无副作用，编译器会完全消除调用。
 */

static inline void sched_core_enqueue(struct rq *rq, struct task_struct *p) { }
static inline void
sched_core_dequeue(struct rq *rq, struct task_struct *p, int flags) { }

#endif /* !CONFIG_SCHED_CORE */

/* need a wrapper since we may need to trace from modules */
/*
 * 原文：模块可能需要跟踪 task state 写入，因此保留导出 wrapper。
 * @state_value 是写入 current->__state 的状态位；目标恒为 current。trace hook
 * 只观察写入，不取得 task 引用，也不代替阻塞/唤醒所需的内存序协议。
 */
EXPORT_TRACEPOINT_SYMBOL(sched_set_state_tp);

/*
 * Call via the helper macro trace_set_current_state.
 * Calls to this function MUST be guarded by a
 * tracepoint_enabled(sched_set_state_tp)
 */
void __trace_set_current_state(int state_value)
{
	trace_call__sched_set_state_tp(current, state_value);
}
EXPORT_SYMBOL(__trace_set_current_state);

int task_llc(const struct task_struct *p)
{
	return per_cpu(sd_llc_id, task_cpu(p));
}

/*
 * Serialization rules:
 *
 * Lock order:
 *
 *   p->pi_lock
 *     rq->lock
 *       hrtimer_cpu_base->lock (hrtimer_start() for bandwidth controls)
 *
 *  rq1->lock
 *    rq2->lock  where: rq1 < rq2
 *
 * Regular state:
 *
 * Normal scheduling state is serialized by rq->lock. __schedule() takes the
 * local CPU's rq->lock, it optionally removes the task from the runqueue and
 * always looks at the local rq data structures to find the most eligible task
 * to run next.
 *
 * Task enqueue is also under rq->lock, possibly taken from another CPU.
 * Wakeups from another LLC domain might use an IPI to transfer the enqueue to
 * the local CPU to avoid bouncing the runqueue state around [ see
 * ttwu_queue_wakelist() ]
 *
 * Task wakeup, specifically wakeups that involve migration, are horribly
 * complicated to avoid having to take two rq->locks.
 *
 * Special state:
 *
 * System-calls and anything external will use task_rq_lock() which acquires
 * both p->pi_lock and rq->lock. As a consequence the state they change is
 * stable while holding either lock:
 *
 *  - sched_setaffinity()/
 *    set_cpus_allowed_ptr():	p->cpus_ptr, p->nr_cpus_allowed
 *  - set_user_nice():		p->se.load, p->*prio
 *  - __sched_setscheduler():	p->sched_class, p->policy, p->*prio,
 *				p->se.load, p->rt_priority,
 *				p->dl.dl_{runtime, deadline, period, flags, bw, density}
 *  - sched_setnuma():		p->numa_preferred_nid
 *  - sched_move_task():	p->sched_task_group
 *  - uclamp_update_active()	p->uclamp*
 *
 * p->state <- TASK_*:
 *
 *   is changed locklessly using set_current_state(), __set_current_state() or
 *   set_special_state(), see their respective comments, or by
 *   try_to_wake_up(). This latter uses p->pi_lock to serialize against
 *   concurrent self.
 *
 * p->on_rq <- { 0, 1 = TASK_ON_RQ_QUEUED, 2 = TASK_ON_RQ_MIGRATING }:
 *
 *   is set by activate_task() and cleared by deactivate_task()/block_task(),
 *   under rq->lock. Non-zero indicates the task is runnable, the special
 *   ON_RQ_MIGRATING state is used for migration without holding both
 *   rq->locks. It indicates task_cpu() is not stable, see task_rq_lock().
 *
 *   Additionally it is possible to be ->on_rq but still be considered not
 *   runnable when p->se.sched_delayed is true. These tasks are on the runqueue
 *   but will be dequeued as soon as they get picked again. See the
 *   task_is_runnable() helper.
 *
 * p->on_cpu <- { 0, 1 }:
 *
 *   is set by prepare_task() and cleared by finish_task() such that it will be
 *   set before p is scheduled-in and cleared after p is scheduled-out, both
 *   under rq->lock. Non-zero indicates the task is running on its CPU.
 *
 *   [ The astute reader will observe that it is possible for two tasks on one
 *     CPU to have ->on_cpu = 1 at the same time. ]
 *
 * p->is_blocked <- { 0, 1 }:
 *
 *   is set by try_to_block_task() and cleared by ttwu_do_wakeup() and tracks
 *   if the task is blocked. Traditionally this would mirror p->on_rq, however
 *   due things like DELAY_DEQUEUE and PROXY_EXEC, this can diverge.
 *
 * task_cpu(p): is changed by set_task_cpu(), the rules are:
 *
 *  - Don't call set_task_cpu() on a blocked task:
 *
 *    We don't care what CPU we're not running on, this simplifies hotplug,
 *    the CPU assignment of blocked tasks isn't required to be valid.
 *
 *  - for try_to_wake_up(), called under p->pi_lock:
 *
 *    This allows try_to_wake_up() to only take one rq->lock, see its comment.
 *
 *  - for migration called under rq->lock:
 *    [ see task_on_rq_migrating() in task_rq_lock() ]
 *
 *    o move_queued_task()
 *    o detach_task()
 *
 *  - for migration called under double_rq_lock():
 *
 *    o __migrate_swap_task()
 *    o push_rt_task() / pull_rt_task()
 *    o push_dl_task() / pull_dl_task()
 *    o dl_task_offline_migration()
 *
 */
/*
 * 上述原文是 core.c 的状态/锁地图。关键点是：task 的“阻塞状态”
 * “是否在 rq”“是否仍在 CPU 上执行”和“归属 CPU”是四个相关但不同的状态轴。
 * p->pi_lock 串行化唤醒与 task 自身状态转换；rq->lock 保护队列成员关系和 curr；
 * on_cpu 的 release/acquire 配对跨越 context switch 窗口；迁移中的特殊 on_rq
 * 值告诉无锁观察者 task_cpu() 暂时不稳定。只读取其中一个字段，不能推断其余
 * 三个状态，也不能替代相应锁协议。
 */

/*
 * raw_spin_rq_lock_nested() - 在 core scheduling 动态启停期间取得 @rq 的有效锁。
 *
 * @rq 为借用队列，@subclass 只用于 lockdep 嵌套分类。返回时持有效 rq 锁且由
 * 调用者释放。preempt_disable() 与 enable 路径的 synchronize_rcu() 配对，保证
 * 读到锁指针并加锁期间关联不会悄然切换；复核失败则放锁重试。函数不能睡眠。
 */
void raw_spin_rq_lock_nested(struct rq *rq, int subclass)
	__context_unsafe()
{
	raw_spinlock_t *lock;

	/* Matches synchronize_rcu() in __sched_core_enable() */
	preempt_disable();
	if (sched_core_disabled()) {
		raw_spin_lock_nested(&rq->__lock, subclass);
		/* preempt_count *MUST* be > 1 */
		/* 中文：preempt_count 必须大于 1，确保切换锁关联期间不可抢占。 */
		preempt_enable_no_resched();
		return;
	}

	for (;;) {
		lock = __rq_lockp(rq);
		raw_spin_lock_nested(lock, subclass);
		if (likely(lock == __rq_lockp(rq))) {
			/* preempt_count *MUST* be > 1 */
			preempt_enable_no_resched();
			return;
		}
		raw_spin_unlock(lock);
	}
}

/*
 * raw_spin_rq_trylock() - 尝试取得 @rq 的当前有效锁而不等待。
 *
 * true 表示调用者获得锁并负责释放；false 表示没有 ownership 变化。启用 core
 * scheduling 时必须在 trylock 成功后复核 lock pointer，若关联已变则释放旧锁
 * 重试，避免拿着不再保护该 rq 的锁访问热字段。
 */
bool raw_spin_rq_trylock(struct rq *rq)
	__context_unsafe()
{
	raw_spinlock_t *lock;
	bool ret;

	/* Matches synchronize_rcu() in __sched_core_enable() */
	preempt_disable();
	if (sched_core_disabled()) {
		ret = raw_spin_trylock(&rq->__lock);
		preempt_enable();
		return ret;
	}

	for (;;) {
		lock = __rq_lockp(rq);
		ret = raw_spin_trylock(lock);
		if (!ret || (likely(lock == __rq_lockp(rq)))) {
			preempt_enable();
			return ret;
		}
		raw_spin_unlock(lock);
	}
}

/*
 * double_rq_lock - safely lock two runqueues
 */
/*
 * 原文：安全地同时锁住两个 runqueue。
 *
 * @rq1/@rq2 均为借用指针；入口 IRQ 已关闭且未持这两把锁。不同 rq 按稳定的
 * rq_order 顺序获取，消除两个迁移者反向取锁导致的 ABBA；若 core scheduling
 * 使二者共享实际锁，只做一次物理加锁并用 lockdep 假获取保持分析模型。返回时
 * 两个逻辑 rq 均已锁定，无失败返回。
 */
void double_rq_lock(struct rq *rq1, struct rq *rq2)
{
	lockdep_assert_irqs_disabled();

	if (rq_order_less(rq2, rq1))
		swap(rq1, rq2);

	raw_spin_rq_lock(rq1);
	if (__rq_lockp(rq1) != __rq_lockp(rq2))
		raw_spin_rq_lock_nested(rq2, SINGLE_DEPTH_NESTING);
	else
		__acquire_ctx_lock(__rq_lockp(rq2)); /* fake acquire */

	double_rq_clock_clear_update(rq1, rq2);
}

/*
 * ___task_rq_lock - lock the rq @p resides on.
 */
/*
 * 原文：锁住 @p 当前所在的 rq。
 *
 * 调用者已经持有 p->pi_lock；@rf 是锁状态输出。函数读取 task_rq(p)、锁住它，
 * 再复核 p 没处于 ON_RQ_MIGRATING 且 rq 未改变；失败就释放并重试。成功返回
 * 借用 rq 且 rq->lock 仍持有，调用者必须用 task_rq_unlock 系列释放。该双重
 * 检查把“先读 CPU、后取锁”的竞态收敛为稳定快照。
 */
struct rq *___task_rq_lock(struct task_struct *p, struct rq_flags *rf)
{
	struct rq *rq;

	lockdep_assert_held(&p->pi_lock);

	for (;;) {
		rq = task_rq(p);
		raw_spin_rq_lock(rq);
		if (likely(rq == task_rq(p) && !task_on_rq_migrating(p))) {
			rq_pin_lock(rq, rf);
			return rq;
		}
		raw_spin_rq_unlock(rq);

		while (unlikely(task_on_rq_migrating(p)))
			cpu_relax();
	}
}

/*
 * task_rq_lock - lock p->pi_lock and lock the rq @p resides on.
 */
/*
 * 中文：同时锁住 p->pi_lock 与 @p 当前所在 rq。读取 CPU 后取 rq 锁，再复核
 * rq 与 MIGRATING 状态；失败则完整释放并等待迁移结束后重试。成功返回持锁 rq，
 * @rf 保存 IRQ/锁状态，必须由配对 unlock 恢复。
 */
struct rq *_task_rq_lock(struct task_struct *p, struct rq_flags *rf)
{
	struct rq *rq;

	for (;;) {
		raw_spin_lock_irqsave(&p->pi_lock, rf->flags);
		rq = task_rq(p);
		raw_spin_rq_lock(rq);
		/*
		 *	move_queued_task()		task_rq_lock()
		 *
		 *	ACQUIRE (rq->lock)
		 *	[S] ->on_rq = MIGRATING		[L] rq = task_rq()
		 *	WMB (__set_task_cpu())		ACQUIRE (rq->lock);
		 *	[S] ->cpu = new_cpu		[L] task_rq()
		 *					[L] ->on_rq
		 *	RELEASE (rq->lock)
		 *
		 * If we observe the old CPU in task_rq_lock(), the acquire of
		 * the old rq->lock will fully serialize against the stores.
		 *
		 * If we observe the new CPU in task_rq_lock(), the address
		 * dependency headed by '[L] rq = task_rq()' and the acquire
		 * will pair with the WMB to ensure we then also see migrating.
		 */
		/*
		 * 中文：若看到旧 CPU，取得旧 rq 锁会与迁移写入完全串行化；若看到新
		 * CPU，task_rq() 的地址依赖与 acquire 会配对迁移侧 WMB，保证同时观察
		 * 到 MIGRATING。由此不会把 task 锁在错误 rq 上。
		 */
		if (likely(rq == task_rq(p) && !task_on_rq_migrating(p))) {
			rq_pin_lock(rq, rf);
			return rq;
		}
		raw_spin_rq_unlock(rq);
		raw_spin_unlock_irqrestore(&p->pi_lock, rf->flags);

		while (unlikely(task_on_rq_migrating(p)))
			cpu_relax();
	}
}

/*
 * RQ-clock updating methods:
 */

/* Use CONFIG_PARAVIRT as this will avoid more #ifdef in arch code. */
#ifdef CONFIG_PARAVIRT
struct static_key paravirt_steal_rq_enabled;
#endif

/*
 * update_rq_clock_task() 从 rq 原始时钟增量中扣除 IRQ 时间和虚拟机 steal 时间，
 * 剩余部分累加到 clock_task，并更新 IRQ/PELT 负载。调用者持 rq 锁；各扣减均
 * 截断到本次 delta，保证 clock_task 单调，即使 IRQ 记账只在 irq_exit 更新。
 */
static void update_rq_clock_task(struct rq *rq, s64 delta)
{
/*
 * In theory, the compile should just see 0 here, and optimize out the call
 * to sched_rt_avg_update. But I don't trust it...
 */
/*
 * 中文：理论上编译器应看出这里恒为 0 并消除相关调用，但显式保留变量更稳妥。
 */
	s64 __maybe_unused steal = 0, irq_delta = 0;

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	if (irqtime_enabled()) {
		irq_delta = irq_time_read(cpu_of(rq)) - rq->prev_irq_time;

		/*
		 * Since irq_time is only updated on {soft,}irq_exit, we might run into
		 * this case when a previous update_rq_clock() happened inside a
		 * {soft,}IRQ region.
		 *
		 * When this happens, we stop ->clock_task and only update the
		 * prev_irq_time stamp to account for the part that fit, so that a next
		 * update will consume the rest. This ensures ->clock_task is
		 * monotonic.
		 *
		 * It does however cause some slight miss-attribution of {soft,}IRQ
		 * time, a more accurate solution would be to update the irq_time using
		 * the current rq->clock timestamp, except that would require using
		 * atomic ops.
		 */
		/*
		 * 中文：irq_time 只在软/硬中断退出时更新，若上次 rq 时钟更新发生在
		 * 中断区间，本次 irq_delta 可能超过 wall delta。此时截断并只推进
		 * prev_irq_time 已消费部分，余量下次再扣，保证 clock_task 单调；代价是
		 * 少量 IRQ 时间归属偏差，更精确方案需要原子操作更新 irq_time。
		 */
		if (irq_delta > delta)
			irq_delta = delta;

		rq->prev_irq_time += irq_delta;
		delta -= irq_delta;
		delayacct_irq(rq->curr, irq_delta);
	}
#endif
#ifdef CONFIG_PARAVIRT_TIME_ACCOUNTING
	if (static_key_false((&paravirt_steal_rq_enabled))) {
		u64 prev_steal;

		steal = prev_steal = paravirt_steal_clock(cpu_of(rq));
		steal -= rq->prev_steal_time_rq;

		if (unlikely(steal > delta))
			steal = delta;

		rq->prev_steal_time_rq = prev_steal;
		delta -= steal;
	}
#endif

	rq->clock_task += delta;

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	if ((irq_delta + steal) && sched_feat(NONTASK_CAPACITY))
		update_irq_load_avg(rq, irq_delta + steal);
#endif
	update_rq_clock_pelt(rq, delta);
}

/*
 * update_rq_clock() 在 rq 锁下把 sched_clock_cpu() 推进量发布到 rq->clock，
 * 同时通知 sched_ext 并更新 task clock。ACT_SKIP 抑制本次更新，UPDATED 用于
 * 诊断重复更新；负 delta 视为底层时钟异常而丢弃，绝不让 rq 时钟倒退。
 */
void update_rq_clock(struct rq *rq)
{
	s64 delta;
	u64 clock;

	lockdep_assert_rq_held(rq);

	if (rq->clock_update_flags & RQCF_ACT_SKIP)
		return;

	if (sched_feat(WARN_DOUBLE_CLOCK))
		WARN_ON_ONCE(rq->clock_update_flags & RQCF_UPDATED);
	rq->clock_update_flags |= RQCF_UPDATED;

	clock = sched_clock_cpu(cpu_of(rq));
	scx_rq_clock_update(rq, clock);

	delta = clock - rq->clock;
	if (delta < 0)
		return;
	rq->clock += delta;

	update_rq_clock_task(rq, delta);
}

#ifdef CONFIG_SCHED_HRTICK
/*
 * Use HR-timers to deliver accurate preemption points.
 */

enum {
	HRTICK_SCHED_NONE		= 0,
	HRTICK_SCHED_DEFER		= BIT(1),
	HRTICK_SCHED_START		= BIT(2),
	HRTICK_SCHED_REARM_HRTIMER	= BIT(3)
};

/* hrtick_clear() 取消 rq 上仍 active 的高精度调度定时器；调用者稳定该 rq。 */
static void __used hrtick_clear(struct rq *rq)
{
	if (hrtimer_active(&rq->hrtick_timer))
		hrtimer_cancel(&rq->hrtick_timer);
}

/*
 * High-resolution timer tick.
 * Runs from hardirq context with interrupts disabled.
 */
/*
 * 原文：这是高精度调度 tick，在 IRQ 关闭的 hardirq 上下文运行。
 *
 * @timer 嵌在 rq 中；函数恢复所属 rq，锁队列、更新时钟并把精确 tick 交给 donor
 * 的 sched_class。始终返回 HRTIMER_NORESTART，下一次到期由 class 显式编程。
 * 无引用转移，持 rq 锁与 hardirq 阶段都不能睡眠。
 */
static enum hrtimer_restart hrtick(struct hrtimer *timer)
{
	struct rq *rq = container_of(timer, struct rq, hrtick_timer);
	struct rq_flags rf;

	WARN_ON_ONCE(cpu_of(rq) != smp_processor_id());

	rq_lock(rq, &rf);
	update_rq_clock(rq);
	rq->donor->sched_class->task_tick(rq, rq->donor, 1);
	rq_unlock(rq, &rf);

	return HRTIMER_NORESTART;
}

/*
 * hrtick_needs_rearm() 判断 timer 未排队或新旧绝对到期差超过 5us 时需要重编程。
 * @timer/@expires 仅借用读取；返回值是优化提示，不在检查后冻结 timer 状态。
 */
static inline bool hrtick_needs_rearm(struct hrtimer *timer, ktime_t expires)
{
	/*
	 * Queued is false when the timer is not started or currently
	 * running the callback. In both cases, restart. If queued check
	 * whether the expiry time actually changes substantially.
	 */
	return !hrtimer_is_queued(timer) ||
		abs(expires - hrtimer_get_expires(timer)) > 5000;
}

/*
 * hrtick_cond_restart() 以 @rq 记录的绝对时间按需重启 pinned hard timer。
 * 调用者已在正确 CPU 或持 rq 锁稳定状态；无返回值，也不改变 rq ownership。
 */
static void hrtick_cond_restart(struct rq *rq)
{
	struct hrtimer *timer = &rq->hrtick_timer;
	ktime_t time = rq->hrtick_time;

	if (hrtick_needs_rearm(timer, time))
		hrtimer_start(timer, time, HRTIMER_MODE_ABS_PINNED_HARD);
}

/*
 * called from hardirq (IPI) context
 */
/*
 * 原文：该跨 CPU 落点在 IPI hardirq 上下文调用。@arg 是永久 rq 的借用指针；
 * 函数锁 rq 后完成本地 timer base 重启，确保 pinned timer 不被远端直接编程。
 */
static void __hrtick_start(void *arg)
{
	struct rq *rq = arg;
	struct rq_flags rf;

	rq_lock(rq, &rf);
	hrtick_cond_restart(rq);
	rq_unlock(rq, &rf);
}

/*
 * Called to set the hrtick timer state.
 *
 * called with rq->lock held and IRQs disabled
 */
/*
 * 原文：设置 hrtick timer 状态；入口持 rq 锁且 IRQ 已关闭。
 *
 * @delay 为纳秒并钳到至少 10us，防止极短 slice 形成 timer DoS。若正处于 schedule
 * 临界阶段，只记录延迟交给 exit；否则计算绝对期限，本 rq 直接启动、远端 rq
 * 通过异步 IPI 启动。无返回值，只保证重编程请求已发布。
 */
void hrtick_start(struct rq *rq, u64 delay)
{
	s64 delta;

	/*
	 * Don't schedule slices shorter than 10000ns, that just
	 * doesn't make sense and can cause timer DoS.
	 */
	delta = max_t(s64, delay, 10000LL);

	/*
	 * If this is in the middle of schedule() only note the delay
	 * and let hrtick_schedule_exit() deal with it.
	 */
	/*
	 * 中文：若正处于 schedule() 的锁交接阶段，只记录 delay；由
	 * hrtick_schedule_exit() 在安全退出点完成实际编程。
	 */
	if (rq->hrtick_sched) {
		rq->hrtick_sched |= HRTICK_SCHED_START;
		rq->hrtick_delay = delta;
		return;
	}

	rq->hrtick_time = ktime_add_ns(ktime_get(), delta);
	if (!hrtick_needs_rearm(&rq->hrtick_timer, rq->hrtick_time))
		return;

	if (rq == this_rq())
		hrtimer_start(&rq->hrtick_timer, rq->hrtick_time, HRTIMER_MODE_ABS_PINNED_HARD);
	else
		smp_call_function_single_async(cpu_of(rq), &rq->hrtick_csd);
}

/*
 * hrtick_schedule_enter()/exit() 包围 pick/switch：enter 标记延迟编程并接管 timer
 * core 的 deferred rearm；exit 为新 curr 启动期限，idle 时取消旧 timer，再归还
 * deferred rearm。@rq 为已锁借用对象，二者无直接返回值。
 */
static inline void hrtick_schedule_enter(struct rq *rq)
{
	rq->hrtick_sched = HRTICK_SCHED_DEFER;
	if (hrtimer_test_and_clear_rearm_deferred())
		rq->hrtick_sched |= HRTICK_SCHED_REARM_HRTIMER;
}

static inline void hrtick_schedule_exit(struct rq *rq)
{
	if (rq->hrtick_sched & HRTICK_SCHED_START) {
		rq->hrtick_time = ktime_add_ns(ktime_get(), rq->hrtick_delay);
		hrtick_cond_restart(rq);
	} else if (idle_rq(rq)) {
		/*
		 * No need for using hrtimer_is_active(). The timer is CPU local
		 * and interrupts are disabled, so the callback cannot be
		 * running and the queued state is valid.
		 */
		if (hrtimer_is_queued(&rq->hrtick_timer))
			hrtimer_cancel(&rq->hrtick_timer);
	}

	if (rq->hrtick_sched & HRTICK_SCHED_REARM_HRTIMER)
		__hrtimer_rearm_deferred();

	rq->hrtick_sched = HRTICK_SCHED_NONE;
}

/*
 * hrtick_rq_init() 在启动期初始化永久 @rq 的异步 CSD 和 pinned hard hrtimer。
 * 此时对象尚未并发可见；无动态分配、无失败返回。
 */
static void hrtick_rq_init(struct rq *rq)
{
	INIT_CSD(&rq->hrtick_csd, __hrtick_start, rq);
	rq->hrtick_sched = HRTICK_SCHED_NONE;
	hrtimer_setup(&rq->hrtick_timer, hrtick, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_HARD | HRTIMER_MODE_LAZY_REARM);
}
#else /* !CONFIG_SCHED_HRTICK: */
static inline void hrtick_clear(struct rq *rq) { }
static inline void hrtick_rq_init(struct rq *rq) { }
static inline void hrtick_schedule_enter(struct rq *rq) { }
static inline void hrtick_schedule_exit(struct rq *rq) { }
#endif /* !CONFIG_SCHED_HRTICK */

/*
 * try_cmpxchg based fetch_or() macro so it works for different integer types:
 */
/*
 * 中文：基于 try_cmpxchg 实现类型通用的原子 fetch_or。循环直到比较交换成功，
 * 表达式返回 OR 之前的旧值；调用者可在一次原子 RMW 中同时设置标志并裁决旧态。
 */
#define fetch_or(ptr, mask)						\
	({								\
		typeof(ptr) _ptr = (ptr);				\
		typeof(mask) _mask = (mask);				\
		typeof(*_ptr) _val = *_ptr;				\
									\
		do {							\
		} while (!try_cmpxchg(_ptr, &_val, _val | _mask));	\
	_val;								\
})

#ifdef TIF_POLLING_NRFLAG
/*
 * Atomically set TIF_NEED_RESCHED and test for TIF_POLLING_NRFLAG,
 * this avoids any races wrt polling state changes and thereby avoids
 * spurious IPIs.
 */
/*
 * 原文：原子设置重调度位并测试 polling 位，使它与 idle 切换 polling 状态形成
 * 一次不可分割的裁决，避免无谓 IPI。返回 true 表示目标不在 polling、调用者应
 * 发 IPI；false 表示 polling idle 会自行观察 flag。@ti 是借用 thread_info。
 */
static inline bool set_nr_and_not_polling(struct thread_info *ti, int tif)
{
	return !(fetch_or(&ti->flags, 1 << tif) & _TIF_POLLING_NRFLAG);
}

/*
 * Atomically set TIF_NEED_RESCHED if TIF_POLLING_NRFLAG is set.
 *
 * If this returns true, then the idle task promises to call
 * sched_ttwu_pending() and reschedule soon.
 */
/*
 * 原文：仅在 polling 标志存在时原子设置 NEED_RESCHED。true 表示 idle task 承诺
 * 很快处理 pending wakeup 并重调度，waker 可省 IPI；false 必须走普通通知路径。
 * cmpxchg 循环处理 flags 并发改变，不能拆为普通读写。
 */
static bool set_nr_if_polling(struct task_struct *p)
{
	struct thread_info *ti = task_thread_info(p);
	typeof(ti->flags) val = READ_ONCE(ti->flags);

	do {
		if (!(val & _TIF_POLLING_NRFLAG))
			return false;
		if (val & _TIF_NEED_RESCHED)
			return true;
	} while (!try_cmpxchg(&ti->flags, &val, val | _TIF_NEED_RESCHED));

	return true;
}

#else
static inline bool set_nr_and_not_polling(struct thread_info *ti, int tif)
{
	set_ti_thread_flag(ti, tif);
	return true;
}

static inline bool set_nr_if_polling(struct task_struct *p)
{
	return false;
}
#endif

/*
 * __wake_q_add() - 无引用计数版本的 wake_q 去重与链接原语。
 *
 * @head 是当前上下文本地队列，@task 为调用者稳定的借用 task。cmpxchg 把嵌入
 * wake_q.next 从 NULL 变为尾哨兵，成功者获得唯一入链权；失败表示已有 waker
 * 负责唤醒。前置完整屏障使即使去重失败，既有 waker 也能观察本调用点前的条件
 * 写。返回 true 只表示入链成功，外层仍需取得 task 引用。
 */
static bool __wake_q_add(struct wake_q_head *head, struct task_struct *task)
{
	struct wake_q_node *node = &task->wake_q;

	/*
	 * Atomically grab the task, if ->wake_q is !nil already it means
	 * it's already queued (either by us or someone else) and will get the
	 * wakeup due to that.
	 *
	 * In order to ensure that a pending wakeup will observe our pending
	 * state, even in the failed case, an explicit smp_mb() must be used.
	 */
	smp_mb__before_atomic();
	if (unlikely(cmpxchg_relaxed(&node->next, NULL, WAKE_Q_TAIL)))
		return false;

	/*
	 * The head is context local, there can be no concurrency.
	 */
	/* 中文：wake_q 的 head 属于当前上下文，不会被其他执行流并发修改。 */
	*head->lastp = node;
	head->lastp = &node->next;
	return true;
}

/**
 * wake_q_add() - queue a wakeup for 'later' waking.
 * @head: the wake_q_head to add @task to
 * @task: the task to queue for 'later' wakeup
 *
 * Queue a task for later wakeup, most likely by the wake_up_q() call in the
 * same context, _HOWEVER_ this is not guaranteed, the wakeup can come
 * instantly.
 *
 * This function must be used as-if it were wake_up_process(); IOW the task
 * must be ready to be woken at this location.
 */
/*
 * 原文完整含义：把 @task 加到 @head，供稍后的 wake_up_q() 唤醒，但并不保证
 * 唤醒一定延迟发生，所以调用点此刻就必须满足 wake_up_process() 的全部前置
 * 条件。首次入链时取得 task 引用；引用责任转给消费该队列的 wake_up_q()。
 * @head 是调用上下文本地队列，常用于持对象锁时收集唤醒、解锁后批量执行。
 */
void wake_q_add(struct wake_q_head *head, struct task_struct *task)
{
	if (__wake_q_add(head, task))
		get_task_struct(task);
}

/**
 * wake_q_add_safe() - safely queue a wakeup for 'later' waking.
 * @head: the wake_q_head to add @task to
 * @task: the task to queue for 'later' wakeup
 *
 * Queue a task for later wakeup, most likely by the wake_up_q() call in the
 * same context, _HOWEVER_ this is not guaranteed, the wakeup can come
 * instantly.
 *
 * This function must be used as-if it were wake_up_process(); IOW the task
 * must be ready to be woken at this location.
 *
 * This function is essentially a task-safe equivalent to wake_q_add(). Callers
 * that already hold reference to @task can call the 'safe' version and trust
 * wake_q to do the right thing depending whether or not the @task is already
 * queued for wakeup.
 */
/*
 * 原文补充：safe 版本供调用者已经持有 @task 引用时使用。首次入链则该引用随
 * 节点转移给 wake_up_q()；若 task 已由其他路径入链，本函数立即 put 掉重复引用。
 * 因而调用后调用者都不再拥有传入引用，不能继续以该引用保证 task 生命周期。
 */
void wake_q_add_safe(struct wake_q_head *head, struct task_struct *task)
{
	if (!__wake_q_add(head, task))
		put_task_struct(task);
}

void wake_up_q(struct wake_q_head *head)
{
	struct wake_q_node *node = head->first;

	while (node != WAKE_Q_TAIL) {
		struct task_struct *task;

		task = container_of(node, struct task_struct, wake_q);
		node = node->next;
		/* pairs with cmpxchg_relaxed() in __wake_q_add() */
		/* 中文：与 __wake_q_add() 的 cmpxchg_relaxed() 配对。 */
		WRITE_ONCE(task->wake_q.next, NULL);
		/* Task can safely be re-inserted now. */

		/*
		 * wake_up_process() executes a full barrier, which pairs with
		 * the queueing in wake_q_add() so as not to miss wakeups.
		 */
		wake_up_process(task);
		put_task_struct(task);
	}
}

/*
 * wake_up_q() 消费整个 @head：先清嵌入 task 的 wake_q.next，使其可再次排队；
 * 再用 wake_up_process() 发布 runnable 状态，最后释放队列持有的 task 引用。
 * wake_up_process() 的完整屏障与 __wake_q_add() 前的屏障共同避免“状态写入与
 * 唤醒去重互相错过”。返回时队列节点均已消费，无单独节点内存需要释放。
 */

/*
 * resched_curr - mark rq's current task 'to be rescheduled now'.
 *
 * On UP this means the setting of the need_resched flag, on SMP it
 * might also involve a cross-CPU call to trigger the scheduler on
 * the target CPU.
 */
/*
 * 原文：把 @rq 的 current 标记为“现在需要重新调度”；UP 只需设置 thread flag，
 * SMP 对远端 rq 还可能发送 IPI。调用者持 rq 锁，@tif 可选择立即或 lazy 标志。
 * idle task 的 lazy 请求会升级成立即请求；已有请求则直接返回。本 CPU 同步更新
 * preempt 标志，远端 polling idle 通过原子 flag 自行观察，否则发送 reschedule
 * IPI。返回只证明请求已发布，不证明 context switch 已发生。
 */
static void __resched_curr(struct rq *rq, int tif)
{
	struct task_struct *curr = rq->curr;
	struct thread_info *cti = task_thread_info(curr);
	int cpu;

	lockdep_assert_rq_held(rq);

	/*
	 * Always immediately preempt the idle task; no point in delaying doing
	 * actual work.
	 */
	if (is_idle_task(curr) && tif == TIF_NEED_RESCHED_LAZY)
		tif = TIF_NEED_RESCHED;

	if (cti->flags & ((1 << tif) | _TIF_NEED_RESCHED))
		return;

	cpu = cpu_of(rq);

	trace_sched_set_need_resched_tp(curr, cpu, tif);
	if (cpu == smp_processor_id()) {
		set_ti_thread_flag(cti, tif);
		if (tif == TIF_NEED_RESCHED)
			set_preempt_need_resched();
		return;
	}

	if (set_nr_and_not_polling(cti, tif)) {
		if (tif == TIF_NEED_RESCHED)
			smp_send_reschedule(cpu);
	} else {
		trace_sched_wake_idle_without_ipi(cpu);
	}
}

/*
 * Calls to this function MUST be guarded by a
 * tracepoint_enabled(sched_set_need_resched_tp)
 */
/*
 * 原文：调用者必须先用 tracepoint_enabled() 守卫，避免关闭 tracepoint 时仍支付
 * 参数准备成本。@curr 是借用 task，@tif 是被发布的重调度位；本函数仅调用裸
 * trace hook，不修改调度状态，也不延长 curr 生命周期。
 */
void __trace_set_need_resched(struct task_struct *curr, int tif)
{
	trace_call__sched_set_need_resched_tp(curr, smp_processor_id(), tif);
}
EXPORT_SYMBOL_GPL(__trace_set_need_resched);

void resched_curr(struct rq *rq)
{
	__resched_curr(rq, TIF_NEED_RESCHED);
}

#ifdef CONFIG_PREEMPT_DYNAMIC
static DEFINE_STATIC_KEY_FALSE(sk_dynamic_preempt_lazy);
/*
 * dynamic_preempt_lazy() 把编译期 PREEMPT_LAZY 或运行期静态键统一成布尔查询；
 * 无入参、副作用和 ownership。静态键关闭时热路径接近常量分支。
 */
static __always_inline bool dynamic_preempt_lazy(void)
{
	return static_branch_unlikely(&sk_dynamic_preempt_lazy);
}
#else
static __always_inline bool dynamic_preempt_lazy(void)
{
	return IS_ENABLED(CONFIG_PREEMPT_LAZY);
}
#endif

/*
 * get_lazy_tif_bit() 返回当前模型应设置的 thread flag：支持 lazy 时延迟到安全点，
 * 否则退化成立即 NEED_RESCHED。返回的是位编号，不是位掩码。
 */
static __always_inline int get_lazy_tif_bit(void)
{
	if (dynamic_preempt_lazy())
		return TIF_NEED_RESCHED_LAZY;

	return TIF_NEED_RESCHED;
}

/*
 * resched_curr_lazy() 要求 @rq 已锁，按当前动态抢占模型发布 lazy/立即重调度请求。
 * 无直接返回值，不保证目标已切换。
 */
void resched_curr_lazy(struct rq *rq)
{
	__resched_curr(rq, get_lazy_tif_bit());
}

/*
 * resched_cpu() - 在锁定目标 rq 后请求 @cpu 重调度。
 *
 * @cpu 是逻辑 CPU 编号。函数保存 IRQ、锁永久 rq，只有 CPU online 或目标就是
 * 当前 CPU 时发布请求，随后恢复 IRQ。可从非睡眠上下文使用；offline 远端目标
 * 被忽略且无错误返回。
 */
void resched_cpu(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	raw_spin_rq_lock_irqsave(rq, flags);
	if (cpu_online(cpu) || cpu == smp_processor_id())
		resched_curr(rq);
	raw_spin_rq_unlock_irqrestore(rq, flags);
}

#ifdef CONFIG_NO_HZ_COMMON
/*
 * In the semi idle case, use the nearest busy CPU for migrating timers
 * from an idle CPU.  This is good for power-savings.
 *
 * We don't do similar optimization for completely idle system, as
 * selecting an idle CPU will add more delays to the timers than intended
 * (as that CPU's timer base may not be up to date wrt jiffies etc).
 */
/*
 * 原文：半空闲系统把 idle CPU 的 timer 迁到拓扑上最近的 busy housekeeping CPU，
 * 有利于省电；完全空闲时不刻意选 idle CPU，因为其 timer base 可能落后并增加
 * 到期延迟。函数在 RCU 下遍历 sched_domain，返回瞬时 CPU 编号，不预留目标；
 * 找不到 busy CPU 时回退到本 CPU 或任一 housekeeping CPU。
 */
int get_nohz_timer_target(void)
{
	int i, cpu = smp_processor_id(), default_cpu = -1;
	struct sched_domain *sd;
	const struct cpumask *hk_mask;

	if (housekeeping_cpu(cpu, HK_TYPE_KERNEL_NOISE)) {
		if (!idle_cpu(cpu))
			return cpu;
		default_cpu = cpu;
	}

	hk_mask = housekeeping_cpumask(HK_TYPE_KERNEL_NOISE);

	guard(rcu)();

	for_each_domain(cpu, sd) {
		for_each_cpu_and(i, sched_domain_span(sd), hk_mask) {
			if (cpu == i)
				continue;

			if (!idle_cpu(i))
				return i;
		}
	}

	if (default_cpu == -1)
		default_cpu = housekeeping_any_cpu(HK_TYPE_KERNEL_NOISE);

	return default_cpu;
}

/*
 * When add_timer_on() enqueues a timer into the timer wheel of an
 * idle CPU then this timer might expire before the next timer event
 * which is scheduled to wake up that CPU. In case of a completely
 * idle system the next event might even be infinite time into the
 * future. wake_up_idle_cpu() ensures that the CPU is woken up and
 * leaves the inner idle loop so the newly added timer is taken into
 * account when the CPU goes back to idle and evaluates the timer
 * wheel for the next timer event.
 */
/*
 * 原文：远端 add_timer_on() 把 timer 放到 idle CPU 时，新期限可能早于该 CPU
 * 已编程的下一事件；完全 idle 时下一事件甚至无限远。这里设置 NEED_RESCHED，
 * 必要时发 IPI，迫使 CPU 离开内层 idle loop 并重新计算 timer wheel。@cpu
 * 为瞬时目标，本 CPU 无需唤醒；无返回值，offline 竞态由上层 hotplug 协议处理。
 */
static void wake_up_idle_cpu(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	if (cpu == smp_processor_id())
		return;

	/*
	 * Set TIF_NEED_RESCHED and send an IPI if in the non-polling
	 * part of the idle loop. This forces an exit from the idle loop
	 * and a round trip to schedule(). Now this could be optimized
	 * because a simple new idle loop iteration is enough to
	 * re-evaluate the next tick. Provided some re-ordering of tick
	 * nohz functions that would need to follow TIF_NR_POLLING
	 * clearing:
	 *
	 * - On most architectures, a simple fetch_or on ti::flags with a
	 *   "0" value would be enough to know if an IPI needs to be sent.
	 *
	 * - x86 needs to perform a last need_resched() check between
	 *   monitor and mwait which doesn't take timers into account.
	 *   There a dedicated TIF_TIMER flag would be required to
	 *   fetch_or here and be checked along with TIF_NEED_RESCHED
	 *   before mwait().
	 *
	 * However, remote timer enqueue is not such a frequent event
	 * and testing of the above solutions didn't appear to report
	 * much benefits.
	 */
	if (set_nr_and_not_polling(task_thread_info(rq->idle), TIF_NEED_RESCHED))
		smp_send_reschedule(cpu);
	else
		trace_sched_wake_idle_without_ipi(cpu);
}

/*
 * wake_up_full_nohz_cpu() 处理 full-nohz 特例：offline CPU 返回 true 表示不要再
 * 尝试；full-nohz CPU 必要时发送 tick kick 并返回 true；普通 CPU 返回 false
 * 交给 idle 唤醒。返回值表示“已处理该类别”，不是 CPU 已经醒来。
 */
static bool wake_up_full_nohz_cpu(int cpu)
{
	/*
	 * We just need the target to call irq_exit() and re-evaluate
	 * the next tick. The nohz full kick at least implies that.
	 * If needed we can still optimize that later with an
	 * empty IRQ.
	 */
	if (cpu_is_offline(cpu))
		return true;  /* Don't try to wake offline CPUs. */
	if (tick_nohz_full_cpu(cpu)) {
		if (cpu != smp_processor_id() ||
		    tick_nohz_tick_stopped())
			tick_nohz_full_kick_cpu(cpu);
		return true;
	}

	return false;
}

/*
 * Wake up the specified CPU.  If the CPU is going offline, it is the
 * caller's responsibility to deal with the lost wakeup, for example,
 * by hooking into the CPU_DEAD notifier like timers and hrtimers do.
 */
/*
 * 原文：唤醒指定 @cpu；若它正在 offline，丢失唤醒的恢复责任属于调用者，例如
 * timer/hrtimer 通过 CPU_DEAD notifier 迁移状态。本函数先处理 full-nohz，再走
 * idle IPI；无返回值，因此不能把调用完成当成远端已观察事件。
 */
void wake_up_nohz_cpu(int cpu)
{
	if (!wake_up_full_nohz_cpu(cpu))
		wake_up_idle_cpu(cpu);
}

/*
 * nohz_csd_func() 是目标 CPU 上的异步 CSD 回调。@info 借用永久 rq；原子清除
 * NOHZ kick 位既消费本次请求又取得其 flags 快照。若 CPU 此刻 idle，则把 flags
 * 保存到 rq 并抬起 SCHED_SOFTIRQ 执行 idle balance。回调在 IRQ 上下文不能睡眠。
 */
static void nohz_csd_func(void *info)
{
	struct rq *rq = info;
	int cpu = cpu_of(rq);
	unsigned int flags;

	/*
	 * Release the rq::nohz_csd.
	 */
	flags = atomic_fetch_andnot(NOHZ_KICK_MASK | NOHZ_NEWILB_KICK, nohz_flags(cpu));
	WARN_ON(!(flags & NOHZ_KICK_MASK));

	rq->idle_balance = idle_cpu(cpu);
	if (rq->idle_balance) {
		rq->nohz_idle_balance = flags;
		__raise_softirq_irqoff(SCHED_SOFTIRQ);
	}
}

#endif /* CONFIG_NO_HZ_COMMON */

#ifdef CONFIG_NO_HZ_FULL
/*
 * __need_bw_check() 只在 rq 恰有一个 runnable、且 @p 是已排队 fair task 时返回
 * true，提示 stop-tick 判定继续检查 CFS bandwidth。它是 rq 锁下的瞬时谓词，
 * 不改变 task/rq。
 */
static inline bool __need_bw_check(struct rq *rq, struct task_struct *p)
{
	if (rq->nr_running != 1)
		return false;

	if (p->sched_class != &fair_sched_class)
		return false;

	if (!task_on_rq_queued(p))
		return false;

	return true;
}

/*
 * sched_can_stop_tick() - 判断 full-nohz @rq 是否仍需要周期调度 tick。
 *
 * 调用者稳定 rq。DL 始终需要 tick；多个 RR 需要轮转，FIFO 无强制时间片；CFS
 * 多于一个实体需要抢占，SCX 由自身回调裁决；唯一 CFS task 若受带宽限制仍需
 * tick 执行 throttle。true 只是当前快照允许停 tick，后续 enqueue 会重新 kick。
 */
bool sched_can_stop_tick(struct rq *rq)
{
	int fifo_nr_running;

	/* Deadline tasks, even if single, need the tick */
	if (rq->dl.dl_nr_running)
		return false;

	/*
	 * If there are more than one RR tasks, we need the tick to affect the
	 * actual RR behaviour.
	 */
	if (rq->rt.rr_nr_running) {
		if (rq->rt.rr_nr_running == 1)
			return true;
		else
			return false;
	}

	/*
	 * If there's no RR tasks, but FIFO tasks, we can skip the tick, no
	 * forced preemption between FIFO tasks.
	 */
	/* 中文：没有 RR、只有 FIFO 时可停 tick，因为 FIFO task 之间不会强制定时抢占。 */
	fifo_nr_running = rq->rt.rt_nr_running - rq->rt.rr_nr_running;
	if (fifo_nr_running)
		return true;

	/*
	 * If there are no DL,RR/FIFO tasks, there must only be CFS or SCX tasks
	 * left. For CFS, if there's more than one we need the tick for
	 * involuntary preemption. For SCX, ask.
	 */
	/*
	 * 中文：排除 DL/RR/FIFO 后只剩 CFS 或 SCX；CFS 多于一个实体时需 tick
	 * 触发非自愿抢占，SCX 则必须询问其策略。
	 */
	if (scx_enabled() && !scx_can_stop_tick(rq))
		return false;

	if (rq->cfs.h_nr_queued > 1)
		return false;

	/*
	 * If there is one task and it has CFS runtime bandwidth constraints
	 * and it's on the cpu now we don't want to stop the tick.
	 * This check prevents clearing the bit if a newly enqueued task here is
	 * dequeued by migrating while the constrained task continues to run.
	 * E.g. going from 2->1 without going through pick_next_task().
	 */
	/*
	 * 中文：唯一运行 task 若受 CFS runtime 带宽限制便不能停 tick。额外复核还
	 * 覆盖 2->1 由迁移出队、未经过 pick_next_task() 的情形，避免错误清状态位。
	 */
	if (__need_bw_check(rq, rq->curr)) {
		if (cfs_task_bw_constrained(rq->curr))
			return false;
	}

	return true;
}
#endif /* CONFIG_NO_HZ_FULL */

#if defined(CONFIG_RT_GROUP_SCHED) || defined(CONFIG_FAIR_GROUP_SCHED)
/*
 * Iterate task_group tree rooted at *from, calling @down when first entering a
 * node and @up when leaving it for the final time.
 *
 * Caller must hold rcu_lock or sufficient equivalent.
 */
/*
 * 原文：从 @from 深度优先遍历 task_group 树，首次进入节点调用 @down，最终离开
 * 调用 @up；@data 原样借给回调。调用者必须持 RCU 或等价保护，回调返回非零会
 * 立即中止并向外返回该值。遍历只借用 tg 指针，回调不得把裸指针带出保护区。
 */
int walk_tg_tree_from(struct task_group *from,
			     tg_visitor down, tg_visitor up, void *data)
{
	struct task_group *parent, *child;
	int ret;

	parent = from;

down:
	ret = (*down)(parent, data);
	if (ret)
		goto out;
	list_for_each_entry_rcu(child, &parent->children, siblings) {
		parent = child;
		goto down;

up:
		continue;
	}
	ret = (*up)(parent, data);
	if (ret || parent == from)
		goto out;

	child = parent;
	parent = parent->parent;
	if (parent)
		goto up;
out:
	return ret;
}

/*
 * tg_nop() 是 walk_tg_tree_from() 的无操作 visitor：忽略借用的 @tg/@data，
 * 始终返回 0 允许遍历继续，用于只关心进入或离开一侧的调用者。
 */
int tg_nop(struct task_group *tg, void *data)
{
	return 0;
}
#endif

/*
 * set_load_weight() - 从 policy/static_prio 重建 @p 的调度权重。
 *
 * @p 是调用者已稳定策略和队列状态的借用 task；@update_load 为 true 表示旧值
 * 已进入 rq 的负载统计，必须通过 sched_class->reweight_task 原子地撤销/重加；
 * false 时可直接写尚未发布的 p->se.load。无返回值，副作用会改变公平调度份额，
 * 不改变 task 引用或 CPU 归属。
 */
void set_load_weight(struct task_struct *p, bool update_load)
{
	int prio = p->static_prio - MAX_RT_PRIO;
	struct load_weight lw;

	if (task_has_idle_policy(p)) {
		lw.weight = scale_load(WEIGHT_IDLEPRIO);
		lw.inv_weight = WMULT_IDLEPRIO;
	} else {
		lw.weight = scale_load(sched_prio_to_weight[prio]);
		lw.inv_weight = sched_prio_to_wmult[prio];
	}

	/*
	 * SCHED_OTHER tasks have to update their load when changing their
	 * weight
	 */
	/* 中文：SCHED_OTHER task 改权重时还必须同步其调度类负载。 */
	if (update_load && p->sched_class->reweight_task)
		p->sched_class->reweight_task(task_rq(p), p, &lw);
	else
		p->se.load = lw;
}

#ifdef CONFIG_UCLAMP_TASK
/*
 * Serializes updates of utilization clamp values
 *
 * The (slow-path) user-space triggers utilization clamp value updates which
 * can require updates on (fast-path) scheduler's data structures used to
 * support enqueue/dequeue operations.
 * While the per-CPU rq lock protects fast-path update operations, user-space
 * requests are serialized using a mutex to reduce the risk of conflicting
 * updates or API abuses.
 */
/*
 * 原文说明 uclamp_mutex 串行化用户态慢路径的 clamp 配置更新；每 CPU rq 锁仍
 * 保护 enqueue/dequeue 热路径。二者职责不同：mutex 防止多个控制面请求互相
 * 覆盖，rq 锁保证桶计数与 runnable 集合一致。uclamp 是容量选择约束，并不直接
 * 锁定硬件频率；task/task_group 的最终有效值还会经过层级限制。
 */
static __maybe_unused DEFINE_MUTEX(uclamp_mutex);

/* Max allowed minimum utilization */
/* 原文：系统允许的 UCLAMP_MIN 上限，容量单位为 0..SCHED_CAPACITY_SCALE。 */
static unsigned int __maybe_unused sysctl_sched_uclamp_util_min = SCHED_CAPACITY_SCALE;

/* Max allowed maximum utilization */
/* 原文：系统允许的 UCLAMP_MAX 上限，和 min 一起约束所有 task/group 请求。 */
static unsigned int __maybe_unused sysctl_sched_uclamp_util_max = SCHED_CAPACITY_SCALE;

/*
 * By default RT tasks run at the maximum performance point/capacity of the
 * system. Uclamp enforces this by always setting UCLAMP_MIN of RT tasks to
 * SCHED_CAPACITY_SCALE.
 *
 * This knob allows admins to change the default behavior when uclamp is being
 * used. In battery powered devices, particularly, running at the maximum
 * capacity and frequency will increase energy consumption and shorten the
 * battery life.
 *
 * This knob only affects RT tasks that their uclamp_se->user_defined == false.
 *
 * This knob will not override the system default sched_util_clamp_min defined
 * above.
 */
/*
 * 原文：RT task 默认把 UCLAMP_MIN 设为系统满容量，确保最高性能；管理员可为电池
 * 设备降低该默认值，以功耗换取实时 task 的峰值能力。只影响未被用户显式设置的
 * RT task，并且不能越过系统 sched_util_clamp_min 默认约束。变量由 sysctl 慢
 * 路径写、fork/更新路径读，单位同调度容量。
 */
unsigned int sysctl_sched_uclamp_util_min_rt_default = SCHED_CAPACITY_SCALE;

/* All clamps are required to be less or equal than these values */
/* 原文：所有有效 clamp 都不得超过该数组保存的系统默认上界。 */
static struct uclamp_se uclamp_default[UCLAMP_CNT];

/*
 * This static key is used to reduce the uclamp overhead in the fast path. It
 * primarily disables the call to uclamp_rq_{inc, dec}() in
 * enqueue/dequeue_task().
 *
 * This allows users to continue to enable uclamp in their kernel config with
 * minimum uclamp overhead in the fast path.
 *
 * As soon as userspace modifies any of the uclamp knobs, the static key is
 * enabled, since we have an actual users that make use of uclamp
 * functionality.
 *
 * The knobs that would enable this static key are:
 *
 *   * A task modifying its uclamp value with sched_setattr().
 *   * An admin modifying the sysctl_sched_uclamp_{min, max} via procfs.
 *   * An admin modifying the cgroup cpu.uclamp.{min, max}
 */
/*
 * 原文：sched_uclamp_used 静态键让未实际使用 uclamp 的系统跳过 enqueue/dequeue
 * 桶记账；任一 task、sysctl 或 cgroup clamp 首次配置后永久启用热路径。这样保留
 * 配置能力而默认成本接近零，代价是启用瞬间必须允许“旧 task 尚未 active”的
 * 不平衡情况，dequeue 侧通过 active 位防御。
 */
DEFINE_STATIC_KEY_FALSE(sched_uclamp_used);

/*
 * uclamp_idle_value()/uclamp_idle_reset() 管理 rq 进入/退出 idle 时的 clamp 缓存。
 * UCLAMP_MAX 在 idle 时保留最后值，避免 blocked utilization 瞬间推高频率；
 * UCLAMP_MIN 则回到无约束值。@rq 已锁，返回/写入值单位为调度容量，无引用变化。
 */
static inline unsigned int
uclamp_idle_value(struct rq *rq, enum uclamp_id clamp_id,
		  unsigned int clamp_value)
{
	/*
	 * Avoid blocked utilization pushing up the frequency when we go
	 * idle (which drops the max-clamp) by retaining the last known
	 * max-clamp.
	 */
	if (clamp_id == UCLAMP_MAX) {
		rq->uclamp_flags |= UCLAMP_FLAG_IDLE;
		return clamp_value;
	}

	return uclamp_none(UCLAMP_MIN);
}

/*
 * uclamp_idle_reset() 仅在 rq 带 IDLE 保留标志时，以首个重新入队 task 的 clamp
 * 恢复聚合值；普通非 idle 更新无需动作。调用者持 rq 锁。
 */
static inline void uclamp_idle_reset(struct rq *rq, enum uclamp_id clamp_id,
				     unsigned int clamp_value)
{
	/* Reset max-clamp retention only on idle exit */
	if (!(rq->uclamp_flags & UCLAMP_FLAG_IDLE))
		return;

	uclamp_rq_set(rq, clamp_id, clamp_value);
}

/*
 * uclamp_rq_max_value() 从最高 bucket 向下查找仍有 runnable task 的最大聚合值；
 * UCLAMP_MIN/MAX 都按最大值聚合。无 task 时进入 idle 保留规则。调用者持 rq 锁。
 */
static inline
unsigned int uclamp_rq_max_value(struct rq *rq, enum uclamp_id clamp_id,
				   unsigned int clamp_value)
{
	struct uclamp_bucket *bucket = rq->uclamp[clamp_id].bucket;
	int bucket_id = UCLAMP_BUCKETS - 1;

	/*
	 * Since both min and max clamps are max aggregated, find the
	 * top most bucket with tasks in.
	 */
	/* 中文：两类 clamp 都取最大聚合值，因此从最高非空 bucket 开始查找。 */
	for ( ; bucket_id >= 0; bucket_id--) {
		if (!bucket[bucket_id].tasks)
			continue;
		return bucket[bucket_id].value;
	}

	/* No tasks -- default clamp values */
	/* 中文：没有 runnable task，返回 idle/default clamp。 */
	return uclamp_idle_value(rq, clamp_id, clamp_value);
}

/*
 * __uclamp_update_util_min_rt_default() 要求持 p->pi_lock，只更新没有 user_defined
 * 请求的 RT task UCLAMP_MIN；wrapper 先过滤非 RT，再用 task_rq_lock 同时稳定
 * task 与队列。两者无返回值，已显式配置的 task 保持不变。
 */
static void __uclamp_update_util_min_rt_default(struct task_struct *p)
{
	unsigned int default_util_min;
	struct uclamp_se *uc_se;

	lockdep_assert_held(&p->pi_lock);

	uc_se = &p->uclamp_req[UCLAMP_MIN];

	/* Only sync if user didn't override the default */
	/* 中文：用户显式覆盖后不得再被系统 RT 默认值改写。 */
	if (uc_se->user_defined)
		return;

	default_util_min = sysctl_sched_uclamp_util_min_rt_default;
	uclamp_se_set(uc_se, default_util_min, false);
}

/*
 * uclamp_update_util_min_rt_default() 只处理 RT task，并用 task_rq_lock 同时稳定
 * task 请求和所在 rq；用户显式设置的 UCLAMP_MIN 由内部 helper 保留。
 */
static void uclamp_update_util_min_rt_default(struct task_struct *p)
{
	if (!rt_task(p))
		return;

	/* Protect updates to p->uclamp_* */
	/* 中文：锁住 task/rq，串行化 p->uclamp_* 更新与入队、迁移。 */
	guard(task_rq_lock)(p);
	__uclamp_update_util_min_rt_default(p);
}

/*
 * uclamp_tg_restrict() 复制 task 请求，并在启用 task-group uclamp 时把它限制在
 * 组的有效 min/max 范围内。返回值按值传递，不暴露 task 内部存储；autogroup
 * 与根组使用系统默认规则，无需额外层级裁剪。
 */
static inline struct uclamp_se
uclamp_tg_restrict(struct task_struct *p, enum uclamp_id clamp_id)
{
	/* Copy by value as we could modify it */
	/* 中文：按值复制，因为后续可能修改这个临时请求。 */
	struct uclamp_se uc_req = p->uclamp_req[clamp_id];
#ifdef CONFIG_UCLAMP_TASK_GROUP
	unsigned int tg_min, tg_max, value;

	/*
	 * Tasks in autogroups or root task group will be
	 * restricted by system defaults.
	 */
	/* 中文：autogroup 和根 task_group 由系统默认值约束。 */
	if (task_group_is_autogroup(task_group(p)))
		return uc_req;
	if (task_group(p) == &root_task_group)
		return uc_req;

	tg_min = task_group(p)->uclamp[UCLAMP_MIN].value;
	tg_max = task_group(p)->uclamp[UCLAMP_MAX].value;
	value = uc_req.value;
	value = clamp(value, tg_min, tg_max);
	uclamp_se_set(&uc_req, value, false);
#endif

	return uc_req;
}

/*
 * uclamp_tg_restrict() 按值复制 task 请求，再把非 root/非 autogroup task 钳在其
 * task_group 有效 min/max 内。返回副本携带 bucket/value，不借出 group 内存；
 * 调用者须在 group 配置稳定的调度锁/RCU上下文使用。
 */

/*
 * The effective clamp bucket index of a task depends on, by increasing
 * priority:
 * - the task specific clamp value, when explicitly requested from userspace
 * - the task group effective clamp value, for tasks not either in the root
 *   group or in an autogroup
 * - the system default clamp value, defined by the sysadmin
 */
/*
 * 原文：有效 bucket 的优先约束依次来自 task 显式请求、普通 task_group 的有效
 * clamp、最后是系统默认值。这里“优先”指逐层收紧而非后者覆盖前者；返回按值
 * uclamp_se，不转移任何对象 ownership。
 */
static inline struct uclamp_se
uclamp_eff_get(struct task_struct *p, enum uclamp_id clamp_id)
{
	struct uclamp_se uc_req = uclamp_tg_restrict(p, clamp_id);
	struct uclamp_se uc_max = uclamp_default[clamp_id];

	/* System default restrictions always apply */
	if (unlikely(uc_req.value > uc_max.value))
		return uc_max;

	return uc_req;
}

/*
 * uclamp_eff_value() 返回 @p 指定 clamp 的容量值。task 当前已在 rq 桶中 active
 * 时读取反向写回的有效快照，否则现场计算层级限制。返回 unsigned long 数值，
 * 不保证 task 随后仍处于同一 group/队列。
 */
unsigned long uclamp_eff_value(struct task_struct *p, enum uclamp_id clamp_id)
{
	struct uclamp_se uc_eff;

	/* Task currently refcounted: use back-annotated (effective) value */
	if (p->uclamp[clamp_id].active)
		return (unsigned long)p->uclamp[clamp_id].value;

	uc_eff = uclamp_eff_get(p, clamp_id);

	return (unsigned long)uc_eff.value;
}

/*
 * When a task is enqueued on a rq, the clamp bucket currently defined by the
 * task's uclamp::bucket_id is refcounted on that rq. This also immediately
 * updates the rq's clamp value if required.
 *
 * Tasks can have a task-specific value requested from user-space, track
 * within each bucket the maximum value for tasks refcounted in it.
 * This "local max aggregation" allows to track the exact "requested" value
 * for each bucket when all its RUNNABLE tasks require the same clamp.
 */
/*
 * 原文：task 入队时，其有效 bucket 在 @rq 上增加 task 计数，并在需要时立即抬高
 * rq clamp。bucket 还保存该桶所有 runnable task 的最大精确请求，避免仅用桶
 * 边界造成不必要 overboost。调用者持 rq 锁；@p 由队列生命周期稳定。函数把
 * p->uclamp[id].active 置真，建立 dequeue 必须对称撤销的记账，不取得 task 引用。
 */
static inline void uclamp_rq_inc_id(struct rq *rq, struct task_struct *p,
				    enum uclamp_id clamp_id)
{
	struct uclamp_rq *uc_rq = &rq->uclamp[clamp_id];
	struct uclamp_se *uc_se = &p->uclamp[clamp_id];
	struct uclamp_bucket *bucket;

	lockdep_assert_rq_held(rq);

	/* Update task effective clamp */
	/* 中文：先根据 task 请求与 task-group 限制刷新有效 clamp。 */
	p->uclamp[clamp_id] = uclamp_eff_get(p, clamp_id);

	bucket = &uc_rq->bucket[uc_se->bucket_id];
	bucket->tasks++;
	uc_se->active = true;

	uclamp_idle_reset(rq, clamp_id, uc_se->value);

	/*
	 * Local max aggregation: rq buckets always track the max
	 * "requested" clamp value of its RUNNABLE tasks.
	 */
	/* 中文：局部最大值聚合：每个 rq bucket 保存其 runnable task 的最大请求值。 */
	if (bucket->tasks == 1 || uc_se->value > bucket->value)
		bucket->value = uc_se->value;

	if (uc_se->value > uclamp_rq_get(rq, clamp_id))
		uclamp_rq_set(rq, clamp_id, uc_se->value);
}

/*
 * When a task is dequeued from a rq, the clamp bucket refcounted by the task
 * is released. If this is the last task reference counting the rq's max
 * active clamp value, then the rq's clamp value is updated.
 *
 * Both refcounted tasks and rq's cached clamp values are expected to be
 * always valid. If it's detected they are not, as defensive programming,
 * enforce the expected state and warn.
 */
/*
 * 原文：task 出队时释放它对 bucket 的计数；若它是支撑 rq 最大 clamp 的最后
 * task，则重新从桶中求最大值。task active 与 bucket/rq 缓存按设计应始终有效，
 * WARN 分支用于未来修改破坏不变量时自愈。动态静态键可能在 task 入队后才开启，
 * 所以 active=false 必须直接返回，防止减掉从未增加的计数。
 */
static inline void uclamp_rq_dec_id(struct rq *rq, struct task_struct *p,
				    enum uclamp_id clamp_id)
{
	struct uclamp_rq *uc_rq = &rq->uclamp[clamp_id];
	struct uclamp_se *uc_se = &p->uclamp[clamp_id];
	struct uclamp_bucket *bucket;
	unsigned int bkt_clamp;
	unsigned int rq_clamp;

	lockdep_assert_rq_held(rq);

	/*
	 * If sched_uclamp_used was enabled after task @p was enqueued,
	 * we could end up with unbalanced call to uclamp_rq_dec_id().
	 *
	 * In this case the uc_se->active flag should be false since no uclamp
	 * accounting was performed at enqueue time and we can just return
	 * here.
	 *
	 * Need to be careful of the following enqueue/dequeue ordering
	 * problem too
	 *
	 *	enqueue(taskA)
	 *	// sched_uclamp_used gets enabled
	 *	enqueue(taskB)
	 *	dequeue(taskA)
	 *	// Must not decrement bucket->tasks here
	 *	dequeue(taskB)
	 *
	 * where we could end up with stale data in uc_se and
	 * bucket[uc_se->bucket_id].
	 *
	 * The following check here eliminates the possibility of such race.
	 */
	/*
	 * 中文：若静态键在 @p 入队后才开启，出队会缺少配对 inc。active=false 表明
	 * 当时未记账，必须直接返回；这也处理 A 入队、启用、B 入队、A/B 依次出队
	 * 时 A 的陈旧 bucket_id 不能误减 B 所在桶的竞态。
	 */
	if (unlikely(!uc_se->active))
		return;

	bucket = &uc_rq->bucket[uc_se->bucket_id];

	WARN_ON_ONCE(!bucket->tasks);
	if (likely(bucket->tasks))
		bucket->tasks--;

	uc_se->active = false;

	/*
	 * Keep "local max aggregation" simple and accept to (possibly)
	 * overboost some RUNNABLE tasks in the same bucket.
	 * The rq clamp bucket value is reset to its base value whenever
	 * there are no more RUNNABLE tasks refcounting it.
	 */
	/*
	 * 中文：为保持局部最大聚合简单，允许同一 bucket 内短暂 overboost；只有最后
	 * 一个 runnable 引用离开时，才把 bucket 值恢复为基础边界并重算 rq 最大值。
	 */
	if (likely(bucket->tasks))
		return;

	rq_clamp = uclamp_rq_get(rq, clamp_id);
	/*
	 * Defensive programming: this should never happen. If it happens,
	 * e.g. due to future modification, warn and fix up the expected value.
	 */
	WARN_ON_ONCE(bucket->value > rq_clamp);
	if (bucket->value >= rq_clamp) {
		bkt_clamp = uclamp_rq_max_value(rq, clamp_id, uc_se->value);
		uclamp_rq_set(rq, clamp_id, bkt_clamp);
	}
}

/*
 * uclamp_rq_inc()/dec() 是 enqueue/dequeue 的双 clamp wrapper。静态键未启用、
 * sched_class 不支持或 delayed task 尚未真正唤醒时跳过；否则在已锁 @rq 上逐项
 * 增减桶计数。inc 的 @flags 区分 ENQUEUE_DELAYED，首个 runnable task 会清 idle
 * 保留状态。无返回值，调用顺序必须与 task 队列成员关系对称。
 */
static inline void uclamp_rq_inc(struct rq *rq, struct task_struct *p, int flags)
{
	enum uclamp_id clamp_id;

	/*
	 * Avoid any overhead until uclamp is actually used by the userspace.
	 *
	 * The condition is constructed such that a NOP is generated when
	 * sched_uclamp_used is disabled.
	 */
	/*
	 * 中文：用户态尚未启用 uclamp 时完全跳过开销；条件形式使静态键关闭后编译为
	 * NOP。
	 */
	if (!uclamp_is_used())
		return;

	if (unlikely(!p->sched_class->uclamp_enabled))
		return;

	/* Only inc the delayed task which being woken up. */
	/* 中文：delayed task 只有在真正被唤醒入队时才计入 bucket。 */
	if (p->se.sched_delayed && !(flags & ENQUEUE_DELAYED))
		return;

	for_each_clamp_id(clamp_id)
		uclamp_rq_inc_id(rq, p, clamp_id);

	/* Reset clamp idle holding when there is one RUNNABLE task */
	/* 中文：出现第一个 runnable task 后撤销 idle 时的 MAX 保留状态。 */
	if (rq->uclamp_flags & UCLAMP_FLAG_IDLE)
		rq->uclamp_flags &= ~UCLAMP_FLAG_IDLE;
}

/*
 * uclamp_rq_dec() 在 task 离开 runnable 集合时对两个 clamp bucket 对称减计数。
 * 静态键关闭、不支持 uclamp 的调度类及仍处于 delayed 状态的 task 都不参与。
 */
static inline void uclamp_rq_dec(struct rq *rq, struct task_struct *p)
{
	enum uclamp_id clamp_id;

	/*
	 * Avoid any overhead until uclamp is actually used by the userspace.
	 *
	 * The condition is constructed such that a NOP is generated when
	 * sched_uclamp_used is disabled.
	 */
	/* 中文：静态键关闭时让该快速路径退化为 NOP。 */
	if (!uclamp_is_used())
		return;

	if (unlikely(!p->sched_class->uclamp_enabled))
		return;

	if (p->se.sched_delayed)
		return;

	for_each_clamp_id(clamp_id)
		uclamp_rq_dec_id(rq, p, clamp_id);
}

/*
 * uclamp_rq_reinc_id() 在 task 请求/group 默认改变后，对仍 active 的 @p 先撤销
 * 旧 bucket 再按新有效值加入。短暂降到零可能设置 idle flag，因此 MAX 更新后
 * 必须清掉该假 idle 状态。调用者持 rq 锁，无引用转移。
 */
static inline void uclamp_rq_reinc_id(struct rq *rq, struct task_struct *p,
				      enum uclamp_id clamp_id)
{
	if (!p->uclamp[clamp_id].active)
		return;

	uclamp_rq_dec_id(rq, p, clamp_id);
	uclamp_rq_inc_id(rq, p, clamp_id);

	/*
	 * Make sure to clear the idle flag if we've transiently reached 0
	 * active tasks on rq.
	 */
	if (clamp_id == UCLAMP_MAX && (rq->uclamp_flags & UCLAMP_FLAG_IDLE))
		rq->uclamp_flags &= ~UCLAMP_FLAG_IDLE;
}

/*
 * uclamp_update_active() 在 task 请求或组约束变化后重算其 active bucket。
 * task_rq_lock 串行化入队、出队与迁移；非 runnable task 即使锁到旧 rq 也安全，
 * 因为它尚未影响 bucket，下次入队会直接看到新值。
 */
static inline void
uclamp_update_active(struct task_struct *p)
{
	enum uclamp_id clamp_id;
	struct rq_flags rf;
	struct rq *rq;

	/*
	 * Lock the task and the rq where the task is (or was) queued.
	 *
	 * We might lock the (previous) rq of a !RUNNABLE task, but that's the
	 * price to pay to safely serialize util_{min,max} updates with
	 * enqueues, dequeues and migration operations.
	 * This is the same locking schema used by __set_cpus_allowed_ptr().
	 */
	/*
	 * 中文：锁住 task 及其当前或先前 rq。非 runnable task 可能锁到旧 rq，这是
	 * 为与 clamp 更新、入出队和迁移安全串行化所付出的代价，锁法与
	 * __set_cpus_allowed_ptr() 相同。
	 */
	rq = task_rq_lock(p, &rf);

	/*
	 * Setting the clamp bucket is serialized by task_rq_lock().
	 * If the task is not yet RUNNABLE and its task_struct is not
	 * affecting a valid clamp bucket, the next time it's enqueued,
	 * it will already see the updated clamp bucket value.
	 */
	/*
	 * 中文：task_rq_lock 串行化 bucket 设置；尚未 runnable 且未影响有效 bucket
	 * 的 task 无需立即改 rq，下次入队会直接使用更新后的值。
	 */
	for_each_clamp_id(clamp_id)
		uclamp_rq_reinc_id(rq, p, clamp_id);

	task_rq_unlock(rq, p, &rf);
}

/*
 * uclamp_update_active() 通过 task_rq_lock() 串行化 @p 的 clamp 变更与 enqueue、
 * dequeue、migration；即使 blocked task 锁到旧 rq，也能确保其下次入队看到新值。
 * 对 active bucket 执行成对 reinc，非 active task 延迟到下次 enqueue。无返回值。
 */

#ifdef CONFIG_UCLAMP_TASK_GROUP
static inline void
uclamp_update_active_tasks(struct cgroup_subsys_state *css)
{
	struct css_task_iter it;
	struct task_struct *p;

	css_task_iter_start(css, 0, &it);
	while ((p = css_task_iter_next(&it)))
		uclamp_update_active(p);
	css_task_iter_end(&it);
}

/*
 * uclamp_update_active_tasks() 遍历 @css 当前所有 task，为 group clamp 更新刷新每个
 * runnable task 的 rq 桶。css iterator 为每个返回 task 提供遍历期稳定性；函数
 * 可逐 task 取 rq 锁，属于配置慢路径而非调度热路径。
 */

static void cpu_util_update_eff(struct cgroup_subsys_state *css);
#endif

#ifdef CONFIG_SYSCTL
#ifdef CONFIG_UCLAMP_TASK_GROUP
/*
 * uclamp_update_root_tg() 把 sysctl 系统 min/max 写入 root_task_group 请求，再在
 * RCU 下自顶向下重算所有组的有效 clamp。无参数/返回值；调用者持 uclamp_mutex
 * 串行化控制面更新。
 */
static void uclamp_update_root_tg(void)
{
	struct task_group *tg = &root_task_group;

	uclamp_se_set(&tg->uclamp_req[UCLAMP_MIN],
		      sysctl_sched_uclamp_util_min, false);
	uclamp_se_set(&tg->uclamp_req[UCLAMP_MAX],
		      sysctl_sched_uclamp_util_max, false);

	guard(rcu)();
	cpu_util_update_eff(&root_task_group.css);
}
#else
static void uclamp_update_root_tg(void) { }
#endif

/*
 * uclamp_sync_util_min_rt_default() 把新的 RT 默认 min 同步到所有未显式配置的 RT
 * task。tasklist_lock 的读锁+屏障与 fork 写侧配对：新 task 要么在 sched_post_fork
 * 看到新默认，要么已链接并被本次遍历看到，避免落网。函数在 RCU 下遍历，不持有
 * 返回 task 指针到循环外。
 */
static void uclamp_sync_util_min_rt_default(void)
{
	struct task_struct *g, *p;

	/*
	 * copy_process()			sysctl_uclamp
	 *					  uclamp_min_rt = X;
	 *   write_lock(&tasklist_lock)		  read_lock(&tasklist_lock)
	 *   // link thread			  smp_mb__after_spinlock()
	 *   write_unlock(&tasklist_lock)	  read_unlock(&tasklist_lock);
	 *   sched_post_fork()			  for_each_process_thread()
	 *     __uclamp_sync_rt()		    __uclamp_sync_rt()
	 *
	 * Ensures that either sched_post_fork() will observe the new
	 * uclamp_min_rt or for_each_process_thread() will observe the new
	 * task.
	 */
	read_lock(&tasklist_lock);
	smp_mb__after_spinlock();
	read_unlock(&tasklist_lock);

	guard(rcu)();
	for_each_process_thread(g, p)
		uclamp_update_util_min_rt_default(p);
}

/*
 * sysctl_sched_uclamp_handler() - 读写系统 uclamp 三个控制量并事务式校验。
 *
 * @table/@buffer/@lenp/@ppos 遵循 proc handler 借用契约；@write 为 0 只读取。
 * uclamp_mutex 串行化整个慢路径。写入先保存旧值，proc_dointvec 解析后校验
 * min<=max 且不超过容量；成功更新默认、root tg/RT task 并启用静态键，失败从
 * undo 恢复全部旧值。返回 0 或 proc/校验 errno，不留下部分 sysctl 更新。
 */
static int sysctl_sched_uclamp_handler(const struct ctl_table *table, int write,
				void *buffer, size_t *lenp, loff_t *ppos)
{
	bool update_root_tg = false;
	int old_min, old_max, old_min_rt;
	int result;

	guard(mutex)(&uclamp_mutex);

	old_min = sysctl_sched_uclamp_util_min;
	old_max = sysctl_sched_uclamp_util_max;
	old_min_rt = sysctl_sched_uclamp_util_min_rt_default;

	result = proc_dointvec(table, write, buffer, lenp, ppos);
	if (result)
		goto undo;
	if (!write)
		return 0;

	if (sysctl_sched_uclamp_util_min > sysctl_sched_uclamp_util_max ||
	    sysctl_sched_uclamp_util_max > SCHED_CAPACITY_SCALE	||
	    sysctl_sched_uclamp_util_min_rt_default > SCHED_CAPACITY_SCALE) {

		result = -EINVAL;
		goto undo;
	}

	if (old_min != sysctl_sched_uclamp_util_min) {
		uclamp_se_set(&uclamp_default[UCLAMP_MIN],
			      sysctl_sched_uclamp_util_min, false);
		update_root_tg = true;
	}
	if (old_max != sysctl_sched_uclamp_util_max) {
		uclamp_se_set(&uclamp_default[UCLAMP_MAX],
			      sysctl_sched_uclamp_util_max, false);
		update_root_tg = true;
	}

	if (update_root_tg) {
		sched_uclamp_enable();
		uclamp_update_root_tg();
	}

	if (old_min_rt != sysctl_sched_uclamp_util_min_rt_default) {
		sched_uclamp_enable();
		uclamp_sync_util_min_rt_default();
	}

	/*
	 * We update all RUNNABLE tasks only when task groups are in use.
	 * Otherwise, keep it simple and do just a lazy update at each next
	 * task enqueue time.
	 */
	return 0;

undo:
	sysctl_sched_uclamp_util_min = old_min;
	sysctl_sched_uclamp_util_max = old_max;
	sysctl_sched_uclamp_util_min_rt_default = old_min_rt;
	return result;
}
#endif /* CONFIG_SYSCTL */

/*
 * uclamp_fork() 初始化尚处 fork 早期的 @p：先把继承的运行时 active 记账清零，
 * 因为 child 尚未入任何 rq；reset-on-fork 时再把请求恢复为无约束默认。此阶段
 * child 未并发可见，无需 task_rq_lock；无返回值或引用转移。
 */
static void uclamp_fork(struct task_struct *p)
{
	enum uclamp_id clamp_id;

	/*
	 * We don't need to hold task_rq_lock() when updating p->uclamp_* here
	 * as the task is still at its early fork stages.
	 */
	for_each_clamp_id(clamp_id)
		p->uclamp[clamp_id].active = false;

	if (likely(!p->sched_reset_on_fork))
		return;

	for_each_clamp_id(clamp_id) {
		uclamp_se_set(&p->uclamp_req[clamp_id],
			      uclamp_none(clamp_id), false);
	}
}

/*
 * uclamp_post_fork() 在 child 调度类已确定后应用 RT 默认 UCLAMP_MIN。@p 仍由
 * fork 路径稳定持有；无直接返回值，普通 task 快速返回。
 */
static void uclamp_post_fork(struct task_struct *p)
{
	uclamp_update_util_min_rt_default(p);
}

/*
 * init_uclamp_rq() 在启动期把 @rq 的两个聚合值设为“无约束”，并标记 idle。
 * @rq 是永久 per-CPU 对象，尚无并发 enqueue；无失败返回。
 */
static void __init init_uclamp_rq(struct rq *rq)
{
	enum uclamp_id clamp_id;
	struct uclamp_rq *uc_rq = rq->uclamp;

	for_each_clamp_id(clamp_id) {
		uc_rq[clamp_id] = (struct uclamp_rq) {
			.value = uclamp_none(clamp_id)
		};
	}

	rq->uclamp_flags = UCLAMP_FLAG_IDLE;
}

/*
 * init_uclamp() 初始化所有 possible rq、init_task 请求、系统默认及 root group。
 * 无入参/返回值，只在 sched_init() 期间调用；完成后热路径可假设每个 bucket 和
 * 默认 uclamp_se 已有合法初值。
 */
static void __init init_uclamp(void)
{
	struct uclamp_se uc_max = {};
	enum uclamp_id clamp_id;
	int cpu;

	for_each_possible_cpu(cpu)
		init_uclamp_rq(cpu_rq(cpu));

	for_each_clamp_id(clamp_id) {
		uclamp_se_set(&init_task.uclamp_req[clamp_id],
			      uclamp_none(clamp_id), false);
	}

	/* System defaults allow max clamp values for both indexes */
	/* 中文：系统默认允许两个 clamp 索引都达到各自的最大无约束值。 */
	uclamp_se_set(&uc_max, uclamp_none(UCLAMP_MAX), false);
	for_each_clamp_id(clamp_id) {
		uclamp_default[clamp_id] = uc_max;
#ifdef CONFIG_UCLAMP_TASK_GROUP
		root_task_group.uclamp_req[clamp_id] = uc_max;
		root_task_group.uclamp[clamp_id] = uc_max;
#endif
	}
}

#else /* !CONFIG_UCLAMP_TASK: */
/*
 * CONFIG_UCLAMP_TASK=n 时这些 stub 固定“无容量钳制记账”策略。所有参数仅保持统一
 * 调用接口，均无副作用；编译器会从 enqueue/fork/init 热路径消除它们。
 */
static inline void uclamp_rq_inc(struct rq *rq, struct task_struct *p, int flags) { }
static inline void uclamp_rq_dec(struct rq *rq, struct task_struct *p) { }
static inline void uclamp_fork(struct task_struct *p) { }
static inline void uclamp_post_fork(struct task_struct *p) { }
static inline void init_uclamp(void) { }
#endif /* !CONFIG_UCLAMP_TASK */

/*
 * sched_task_on_rq() 返回 @p 是否处于普通 QUEUED 状态；MIGRATING 和 delayed 的
 * 进一步语义由底层 helper 处理。@p 为调用者稳定的借用 task，结果只是瞬时快照。
 */
bool sched_task_on_rq(struct task_struct *p)
{
	return task_on_rq_queued(p);
}

/*
 * get_wchan() - 获取 blocked task 的内核等待地址，用于 /proc 等诊断。
 *
 * @p 可为 NULL；current 或无法稳定为 blocked 时返回 0。函数在 p->pi_lock 下读取
 * state，经 rmb 与 try_to_wake_up() 配对，并仅在非 RUNNING/WAKING 且不在 rq 时
 * 调用体系结构 unwinder。返回 instruction pointer 数值，不取得 task/栈引用；
 * 调用者必须另外保证 p 生命周期。
 */
unsigned long get_wchan(struct task_struct *p)
{
	unsigned long ip = 0;
	unsigned int state;

	if (!p || p == current)
		return 0;

	/* Only get wchan if task is blocked and we can keep it that way. */
	/*
	 * 原文：只有 task 确认阻塞且能在检查期间保持阻塞，才允许展开其栈；否则它
	 * 可能正在另一个 CPU 改写栈帧，得到无意义地址甚至发生并发访问错误。
	 */
	raw_spin_lock_irq(&p->pi_lock);
	state = READ_ONCE(p->__state);
	smp_rmb(); /* see try_to_wake_up() */
	if (state != TASK_RUNNING && state != TASK_WAKING && !p->on_rq)
		ip = __get_wchan(p);
	raw_spin_unlock_irq(&p->pi_lock);

	return ip;
}

/*
 * enqueue_task()/dequeue_task() - 通用 rq 与具体 sched_class 之间的入出队边界。
 *
 * @rq 已由调用者锁定；@p 为归属该 rq 的借用 task；@flags 描述 wakeup、迁移、
 * 保存状态等原因。通用层维护 core scheduling、PSI、uclamp 和 sched_info，
 * class 回调维护 fair/rt/dl/ext 的私有队列。顺序保证任何观察到 runnable task
 * 的消费者也能看到配套统计；dequeue 以相反方向撤销。无 task 引用转移。
 */
void enqueue_task(struct rq *rq, struct task_struct *p, int flags)
{
	if (!(flags & ENQUEUE_NOCLOCK))
		update_rq_clock(rq);

	/*
	 * Can be before ->enqueue_task() because uclamp considers the
	 * ENQUEUE_DELAYED task before its ->sched_delayed gets cleared
	 * in ->enqueue_task().
	 */
	uclamp_rq_inc(rq, p, flags);

	p->sched_class->enqueue_task(rq, p, flags);

	psi_enqueue(p, flags);

	if (!(flags & ENQUEUE_RESTORE))
		sched_info_enqueue(rq, p);

	if (sched_core_enabled(rq))
		sched_core_enqueue(rq, p);
}

/*
 * Must only return false when DEQUEUE_SLEEP.
 */
/*
 * 原文：dequeue_task() 只有携带 DEQUEUE_SLEEP 时才允许返回 false；这表示调度类
 * 采用 delayed dequeue，task 物理仍在队但逻辑上将阻塞。其他原因必须真正完成
 * 出队。调用者据此决定是否清 on_rq，不能忽略返回值。
 */
inline bool dequeue_task(struct rq *rq, struct task_struct *p, int flags)
{
	if (sched_core_enabled(rq))
		sched_core_dequeue(rq, p, flags);

	if (!(flags & DEQUEUE_NOCLOCK))
		update_rq_clock(rq);

	if (!(flags & DEQUEUE_SAVE))
		sched_info_dequeue(rq, p);

	psi_dequeue(p, flags);

	/*
	 * Must be before ->dequeue_task() because ->dequeue_task() can 'fail'
	 * and mark the task ->sched_delayed.
	 */
	uclamp_rq_dec(rq, p);
	return p->sched_class->dequeue_task(rq, p, flags);
}

/*
 * activate_task()/deactivate_task() 把 p->on_rq 状态与真正 class 队列修改绑在一起。
 * @rq 必须持锁，@p 为借用 task。activate 完成 enqueue 后才发布 QUEUED；
 * deactivate 先发布 MIGRATING 再出队，使 task_rq_lock() 知道 CPU 归属处于过渡期。
 * DELAY_DEQUEUE 可让物理在队与逻辑 runnable 暂时分离，读者应使用专用 helper。
 */
void activate_task(struct rq *rq, struct task_struct *p, int flags)
{
	if (task_on_rq_migrating(p))
		flags |= ENQUEUE_MIGRATED;

	enqueue_task(rq, p, flags);

	WRITE_ONCE(p->on_rq, TASK_ON_RQ_QUEUED);
	ASSERT_EXCLUSIVE_WRITER(p->on_rq);
}

void deactivate_task(struct rq *rq, struct task_struct *p, int flags)
{
	WARN_ON_ONCE(flags & DEQUEUE_SLEEP);

	WRITE_ONCE(p->on_rq, TASK_ON_RQ_MIGRATING);
	ASSERT_EXCLUSIVE_WRITER(p->on_rq);

	/*
	 * Code explicitly relies on TASK_ON_RQ_MIGRATING begin set *before*
	 * dequeue_task() and cleared *after* enqueue_task().
	 */

	dequeue_task(rq, p, flags);
}

/*
 * block_task() 由 __schedule() 在 prev 主动睡眠时调用。@rq 已锁，@p 是 curr，
 * @task_state 是锁内快照。它计算 load 贡献并请求 class 出队；只有 class 确认
 * 真正出队才清 runnable 状态。控制依赖与 ttwu acquire 配对，裁决 block/wake。
 */
static void block_task(struct rq *rq, struct task_struct *p, unsigned long task_state)
{
	int flags = DEQUEUE_NOCLOCK;

	p->sched_contributes_to_load =
		(task_state & TASK_UNINTERRUPTIBLE) &&
		!(task_state & TASK_NOLOAD) &&
		!(task_state & TASK_FROZEN);

	if (unlikely(is_special_task_state(task_state)))
		flags |= DEQUEUE_SPECIAL;

	/*
	 * __schedule()			ttwu()
	 *   prev_state = prev->state;    if (p->on_rq && ...)
	 *   if (prev_state)		    goto out;
	 *     p->on_rq = 0;		  smp_acquire__after_ctrl_dep();
	 *				  p->state = TASK_WAKING
	 *
	 * Where __schedule() and ttwu() have matching control dependencies.
	 *
	 * After this, schedule() must not care about p->state any more.
	 */
	/*
	 * 中文：__schedule 的状态读取/清 on_rq 与 ttwu 的 on_rq 检查/写 TASK_WAKING
	 * 形成匹配控制依赖。越过此点后，schedule 不得再依赖 p->state。
	 */
	if (dequeue_task(rq, p, DEQUEUE_SLEEP | flags))
		__block_task(rq, p);
}

/**
 * task_curr - is this task currently executing on a CPU?
 * @p: the task in question.
 *
 * Return: 1 if the task is currently executing. 0 otherwise.
 */
/*
 * 原文：查询 @p 是否正作为某 CPU 的 current 执行。返回 1/0 的瞬时快照；调用者
 * 必须自行保证 p 生命周期，且返回后 task 可立即切换，不能把它当作锁。
 */
inline int task_curr(const struct task_struct *p)
{
	return cpu_curr(task_cpu(p)) == p;
}

/*
 * wakeup_preempt() - 新 task 入队后让当前 next_class 判断是否应抢占。
 *
 * @rq 已锁，@p 已 queued，@flags 描述唤醒原因。若 p class 高于当前候选 class，
 * 除调用 class hook 外还立即 resched 并提升 rq->next_class。若 donor 仍在队且
 * curr 已需调度，标记跳过紧邻的重复 clock update。无返回值或引用转移。
 */
void wakeup_preempt(struct rq *rq, struct task_struct *p, int flags)
{
	struct task_struct *donor = rq->donor;

	if (p->sched_class == rq->next_class) {
		rq->next_class->wakeup_preempt(rq, p, flags);

	} else if (sched_class_above(p->sched_class, rq->next_class)) {
		rq->next_class->wakeup_preempt(rq, p, flags);
		resched_curr(rq);
		rq->next_class = p->sched_class;
	}

	/*
	 * A queue event has occurred, and we're going to schedule.  In
	 * this case, we can save a useless back to back clock update.
	 */
	if (task_on_rq_queued(donor) && test_tsk_need_resched(rq->curr))
		rq_clock_skip_update(rq);
}

/*
 * __task_state_match() 无锁检查普通 __state 与 PREEMPT_RT/freezer 使用的
 * saved_state：返回 1 表示普通状态匹配，-1 表示保存状态匹配，0 表示均不匹配。
 * 仅内部锁定 wrapper 可用其结果参与稳定决策。
 */
static __always_inline
int __task_state_match(struct task_struct *p, unsigned int state)
{
	if (READ_ONCE(p->__state) & state)
		return 1;

	if (READ_ONCE(p->saved_state) & state)
		return -1;

	return 0;
}

/*
 * __task_state_match() 无锁检查普通 __state 与 PREEMPT_RT/freezer 使用的 saved_state：
 * 返回 1 表示普通状态匹配，-1 表示保存状态匹配，0 表示均不匹配。带锁 wrapper
 * task_state_match() 用 p->pi_lock 与保存/恢复路径串行化。@p 仅借用。
 */

static __always_inline
int task_state_match(struct task_struct *p, unsigned int state)
{
	/*
	 * Serialize against current_save_and_set_rtlock_wait_state(),
	 * current_restore_rtlock_saved_state(), and __refrigerator().
	 */
	/*
	 * 中文：用 p->pi_lock 与 RT 锁等待状态保存/恢复及 freezer 状态变换串行化。
	 */
	guard(raw_spinlock_irq)(&p->pi_lock);
	return __task_state_match(p, state);
}

/*
 * wait_task_inactive - wait for a thread to unschedule.
 *
 * Wait for the thread to block in any of the states set in @match_state.
 * If it changes, i.e. @p might have woken up, then return zero.  When we
 * succeed in waiting for @p to be off its CPU, we return a positive number
 * (its total switch count).  If a second call a short while later returns the
 * same number, the caller can be sure that @p has remained unscheduled the
 * whole time.
 *
 * The caller must ensure that the task *will* unschedule sometime soon,
 * else this function might spin for a *long* time. This function can't
 * be called with interrupts off, or it may introduce deadlock with
 * smp_call_function() if an IPI is sent by the same process we are
 * waiting to become inactive.
 */
/*
 * wait_task_inactive() - 等待 @p 既不在 CPU 上运行，也不处于 rq 可运行集合。
 *
 * @p 必须由调用者持有稳定生命周期引用；@match_state 为 0 表示不校验状态，
 * 否则状态不匹配返回 0。函数先无锁观察以避免争锁，再锁 rq 复核；on_cpu 时自旋，
 * queued 时短暂 sleep 后重试，所以只能在可睡眠上下文调用。成功返回非零 ncsw
 * 快照，调用者可检测 task 是否又运行过；成功并不永久冻结 task。
 */
unsigned long wait_task_inactive(struct task_struct *p, unsigned int match_state)
{
	int running, queued, match;
	struct rq_flags rf;
	unsigned long ncsw;
	struct rq *rq;

	for (;;) {
		/*
		 * We do the initial early heuristics without holding
		 * any task-queue locks at all. We'll only try to get
		 * the runqueue lock when things look like they will
		 * work out!
		 */
		rq = task_rq(p);

		/*
		 * If the task is actively running on another CPU
		 * still, just relax and busy-wait without holding
		 * any locks.
		 *
		 * NOTE! Since we don't hold any locks, it's not
		 * even sure that "rq" stays as the right runqueue!
		 * But we don't care, since "task_on_cpu()" will
		 * return false if the runqueue has changed and p
		 * is actually now running somewhere else!
		 */
		/*
		 * 中文：若 task 仍在别的 CPU 执行，就不持锁地 relax 忙等。rq 快照
		 * 可能已过时也无妨；task_on_cpu() 在 task 已迁走时会让本轮退出并重验。
		 */
		while (task_on_cpu(rq, p)) {
			if (!task_state_match(p, match_state))
				return 0;
			cpu_relax();
		}

		/*
		 * Ok, time to look more closely! We need the rq
		 * lock now, to be *sure*. If we're wrong, we'll
		 * just go back and repeat.
		 */
		/* 中文：启发式条件满足后才取 rq 锁做确定性复核；竞态失败就重新循环。 */
		rq = task_rq_lock(p, &rf);
		/*
		 * If task is sched_delayed, force dequeue it, to avoid always
		 * hitting the tick timeout in the queued case
		 */
		/* 中文：delayed task 强制真正出队，避免 queued 分支持续等一个 tick 超时。 */
		if (p->se.sched_delayed)
			dequeue_task(rq, p, DEQUEUE_SLEEP | DEQUEUE_DELAYED);
		trace_sched_wait_task(p);
		running = task_on_cpu(rq, p);
		queued = task_on_rq_queued(p);
		ncsw = 0;
		if ((match = __task_state_match(p, match_state))) {
			/*
			 * When matching on p->saved_state, consider this task
			 * still queued so it will wait.
			 */
			/* 中文：若命中 saved_state，仍按 queued 处理并继续等待状态恢复路径。 */
			if (match < 0)
				queued = 1;
			ncsw = p->nvcsw | LONG_MIN; /* sets MSB */
		}
		task_rq_unlock(rq, p, &rf);

		/*
		 * If it changed from the expected state, bail out now.
		 */
		/* 中文：状态已偏离调用者期望，立即以 0 失败退出。 */
		if (unlikely(!ncsw))
			break;

		/*
		 * Was it really running after all now that we
		 * checked with the proper locks actually held?
		 *
		 * Oops. Go back and try again..
		 */
		/* 中文：持正确锁后发现仍在运行，说明早期观察失效，放松后重试。 */
		if (unlikely(running)) {
			cpu_relax();
			continue;
		}

		/*
		 * It's not enough that it's not actively running,
		 * it must be off the runqueue _entirely_, and not
		 * preempted!
		 *
		 * So if it was still runnable (but just not actively
		 * running right now), it's preempted, and we should
		 * yield - it could be a while.
		 */
		/*
		 * 中文：仅“当前未执行”还不够，task 必须完全离开 rq；仍 runnable 说明
		 * 只是被抢占，可能等待较久，因此让出 CPU 后再查。
		 */
		if (unlikely(queued)) {
			ktime_t to = NSEC_PER_SEC / HZ;

			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_hrtimeout(&to, HRTIMER_MODE_REL_HARD);
			continue;
		}

		/*
		 * Ahh, all good. It wasn't running, and it wasn't
		 * runnable, which means that it will never become
		 * running in the future either. We're all done!
		 */
		/* 中文：既不 running 也不 runnable，当前状态约束下已稳定 inactive。 */
		break;
	}

	return ncsw;
}

static void
do_set_cpus_allowed(struct task_struct *p, struct affinity_context *ctx);

/*
 * migrate_disable_switch() 在即将切入 migration_disabled task 时，把其 cpus_ptr
 * 从普通 cpus_mask 临时收窄为当前 @rq CPU。@p 是已选 next；仅外层 disable
 * 深度非零且尚未 pin 时执行，在 task_rq_lock 下调用 class set-affinity。
 */
static void migrate_disable_switch(struct rq *rq, struct task_struct *p)
{
	struct affinity_context ac = {
		.new_mask  = cpumask_of(rq->cpu),
		.flags     = SCA_MIGRATE_DISABLE,
	};

	if (likely(!p->migration_disabled))
		return;

	if (p->cpus_ptr != &p->cpus_mask)
		return;

	scoped_guard (task_rq_lock, p)
		do_set_cpus_allowed(p, &ac);
}

/*
 * ___migrate_enable() 是最外层 migrate_enable 的慢路径：把 current->cpus_ptr
 * 恢复到长期 cpus_mask，并兑现 disable 区间积累的 affinity pending/迁移。
 * 无直接返回值，但可能等待/触发 stopper；调用者必须遵守 migrate_disable 嵌套。
 */
void ___migrate_enable(void)
{
	struct task_struct *p = current;
	struct affinity_context ac = {
		.new_mask  = &p->cpus_mask,
		.flags     = SCA_MIGRATE_ENABLE,
	};

	__set_cpus_allowed_ptr(p, &ac);
}
EXPORT_SYMBOL_GPL(___migrate_enable);

void migrate_disable(void)
{
	__migrate_disable();
}
EXPORT_SYMBOL_GPL(migrate_disable);

/* migrate_enable() 是导出的包装，嵌套计数归零时由内部路径兑现待迁移请求。 */
void migrate_enable(void)
{
	__migrate_enable();
}
EXPORT_SYMBOL_GPL(migrate_enable);

/* rq_has_pinned_tasks() 判断 rq 上是否仍有 migration-disabled task。 */
static inline bool rq_has_pinned_tasks(struct rq *rq)
{
	return rq->nr_pinned;
}

/*
 * Per-CPU kthreads are allowed to run on !active && online CPUs, see
 * __set_cpus_allowed_ptr() and select_fallback_rq().
 */
/*
 * 原文：per-CPU kthread 可以在 online 但尚未 active 的 CPU 运行。
 * is_cpu_allowed() 还综合 cpumask、migrate_disable、普通用户 task、per-CPU
 * kthread 和 dying CPU 的不同规则。返回值只是瞬时判断，不预留 CPU，也不阻止
 * hotplug；真正迁移/入队仍需在相应 rq 与 CPU-hotplug 协议下复核。
 */
static inline bool is_cpu_allowed(struct task_struct *p, int cpu)
{
	/* When not in the task's cpumask, no point in looking further. */
	/* 中文：CPU 不在 task mask 中时立即拒绝，无需检查其他例外。 */
	if (!task_allowed_on_cpu(p, cpu))
		return false;

	/* migrate_disabled() must be allowed to finish. */
	/* 中文：migration-disabled task 必须获准在 online CPU 上完成临界区。 */
	if (is_migration_disabled(p))
		return cpu_online(cpu);

	/* Non kernel threads are not allowed during either online or offline. */
	/* 中文：普通用户 task 只能使用 active CPU，online/offline 过渡期均不可。 */
	if (!(p->flags & PF_KTHREAD))
		return cpu_active(cpu);

	/* KTHREAD_IS_PER_CPU is always allowed. */
	/* 中文：per-CPU kthread 只要 CPU online 就始终允许。 */
	if (kthread_is_per_cpu(p))
		return cpu_online(cpu);

	/* Regular kernel threads don't get to stay during offline. */
	/* 中文：普通 kthread 在 CPU dying 阶段也不能继续停留。 */
	if (cpu_dying(cpu))
		return false;

	/* But are allowed during online. */
	/* 中文：但 CPU 上线、尚未 active 的阶段允许普通 kthread 运行。 */
	return cpu_online(cpu);
}

/*
 * This is how migration works:
 *
 * 1) we invoke migration_cpu_stop() on the target CPU using
 *    stop_one_cpu().
 * 2) stopper starts to run (implicitly forcing the migrated thread
 *    off the CPU)
 * 3) it checks whether the migrated task is still in the wrong runqueue.
 * 4) if it's in the wrong runqueue then the migration thread removes
 *    it and puts it into the right queue.
 * 5) stopper completes and stop_one_cpu() returns and the migration
 *    is done.
 */
/*
 * 原文描述迁移事务：stopper 先把目标 task 从 CPU 上排挤出去，再复核它是否仍在
 * 错误 rq；只有复核成立才执行出队、set_task_cpu 和目标 rq 入队。stop 请求与
 * task 状态变化可并发，所以“条件已自行消失”也是成功完成，而不是错误。
 */

/*
 * move_queued_task - move a queued task to new rq.
 *
 * Returns (locked) new rq. Old rq's lock is released.
 */
/*
 * 原文：把已 queued 的 @p 移到 @new_cpu，并返回仍锁定的新 rq；旧 rq 锁已释放，
 * @rf 的 pin 状态随 helper 转移。阶段严格为旧 rq deactivate→发布新 CPU→解旧锁
 * →锁新 rq→activate/preempt。调用者进入持旧 rq 锁，返回必须按新 rq 解锁。
 */
static struct rq *move_queued_task(struct rq *rq, struct rq_flags *rf,
				   struct task_struct *p, int new_cpu)
	__must_hold(__rq_lockp(rq))
{
	lockdep_assert_rq_held(rq);

	deactivate_task(rq, p, DEQUEUE_NOCLOCK);
	set_task_cpu(p, new_cpu);
	rq_unlock(rq, rf);

	rq = cpu_rq(new_cpu);

	rq_lock(rq, rf);
	WARN_ON_ONCE(task_cpu(p) != new_cpu);
	activate_task(rq, p, 0);
	wakeup_preempt(rq, p, 0);

	return rq;
}

/*
 * migration_arg 是 stopper 的稳定参数快照：task 由发起协议保证存活，dest_cpu
 * 是目标 CPU，pending 可空；非空时连接等待者、stop_work 与 completion。
 */
struct migration_arg {
	struct task_struct		*task;
	int				dest_cpu;
	struct set_affinity_pending	*pending;
};

/*
 * @refs: number of wait_for_completion()
 * @stop_pending: is @stop_work in use
 */
/*
 * 原文：refs 统计正在 wait_for_completion() 的调用者，stop_pending 表示内嵌
 * stop_work 已提交且不能重复使用。done 广播迁移约束已兑现；arg 可被后续请求在
 * p->pi_lock 下更新为最新 dest_cpu。首个请求者的栈对象要等 refs 归零才可离开。
 */
struct set_affinity_pending {
	refcount_t		refs;
	unsigned int		stop_pending;
	struct completion	done;
	struct cpu_stop_work	stop_work;
	struct migration_arg	arg;
};

/*
 * Move (not current) task off this CPU, onto the destination CPU. We're doing
 * this because either it can't run here any more (set_cpus_allowed()
 * away from this CPU, or CPU going down), or because we're
 * attempting to rebalance this task on exec (sched_exec).
 *
 * So we race with normal scheduler movements, but that's OK, as long
 * as the task is no longer on this CPU.
 */
/*
 * 原文：为 affinity、CPU 下线或 exec rebalance 把非 current task 移出本 CPU。
 * 普通调度器可能已并发迁走它，这不算失败；最终要求只是 task 不再留在不合适 CPU。
 * 调用者持源 rq 锁；若 @dest_cpu 又变得非法，保持原 rq 并交给更新后的请求重试。
 */
static struct rq *__migrate_task(struct rq *rq, struct rq_flags *rf,
				 struct task_struct *p, int dest_cpu)
	__must_hold(__rq_lockp(rq))
{
	/* Affinity changed (again). */
	if (!is_cpu_allowed(p, dest_cpu))
		return rq;

	rq = move_queued_task(rq, rf, p, dest_cpu);

	return rq;
}

/*
 * migration_cpu_stop - this will be executed by a high-prio stopper thread
 * and performs thread migration by bumping thread off CPU then
 * 'pushing' onto another runqueue.
 */
/*
 * migration_cpu_stop() - 在 stopper 上下文完成需排除 task 并发执行的迁移。
 *
 * @data 是调用者在 stop 回调完成前保持有效的 migration_arg；其中 task 为稳定
 * 引用，dest_cpu 是候选目标，pending 协调多个亲和性请求。stopper 使本 CPU 的
 * 普通 task 不再并发执行，但仍须取得 p->pi_lock/rq 锁并复核 task 是否在错误 rq。
 * 条件已消失或迁移完成返回 0；无 task ownership 转移，pending 的完成/引用由
 * 既定 affine_move_task 协议收尾。
 */
static int migration_cpu_stop(void *data)
{
	struct migration_arg *arg = data;
	struct set_affinity_pending *pending = arg->pending;
	struct task_struct *p = arg->task;
	struct rq *rq = this_rq();
	bool complete = false;
	struct rq_flags rf;

	/*
	 * The original target CPU might have gone down and we might
	 * be on another CPU but it doesn't matter.
	 */
	/* 中文：原目标 CPU 可能已经下线、stopper 也可能改在别处执行；后续会重新复核。 */
	local_irq_save(rf.flags);
	/*
	 * We need to explicitly wake pending tasks before running
	 * __migrate_task() such that we will not miss enforcing cpus_ptr
	 * during wakeups, see set_cpus_allowed_ptr()'s TASK_WAKING test.
	 */
	/*
	 * 中文：迁移前先冲刷待处理 smp-call，确保唤醒路径已经观察最新 cpus_ptr；
	 * 否则可能绕过 set_cpus_allowed_ptr() 对 TASK_WAKING 的约束。
	 */
	flush_smp_call_function_queue();

	/*
	 * We may change the underlying rq, but the locks held will
	 * appropriately be "transferred" when switching.
	 */
	/* 中文：迁移可能更换 rq，锁包装会把持锁/pin 状态正确转移到新 rq。 */
	context_unsafe_alias(rq);

	raw_spin_lock(&p->pi_lock);
	rq_lock(rq, &rf);

	/*
	 * If we were passed a pending, then ->stop_pending was set, thus
	 * p->migration_pending must have remained stable.
	 */
	/* 中文：传入 pending 意味 stop_pending 已置位，p->migration_pending 必须保持同一对象。 */
	WARN_ON_ONCE(pending && pending != p->migration_pending);

	/*
	 * If task_rq(p) != rq, it cannot be migrated here, because we're
	 * holding rq->lock, if p->on_rq == 0 it cannot get enqueued because
	 * we're holding p->pi_lock.
	 */
	/*
	 * 中文：task 不在本 rq 就不能由此 stopper 迁移；rq 锁稳定在队 task，
	 * pi_lock 又阻止已出队 task 被并发重新入队。
	 */
	if (task_rq(p) == rq) {
		if (is_migration_disabled(p))
			goto out;

		if (pending) {
			p->migration_pending = NULL;
			complete = true;

			if (cpumask_test_cpu(task_cpu(p), &p->cpus_mask))
				goto out;
		}

		if (task_on_rq_queued(p)) {
			update_rq_clock(rq);
			rq = __migrate_task(rq, &rf, p, arg->dest_cpu);
		} else {
			p->wake_cpu = arg->dest_cpu;
		}

		/*
		 * XXX __migrate_task() can fail, at which point we might end
		 * up running on a dodgy CPU, AFAICT this can only happen
		 * during CPU hotplug, at which point we'll get pushed out
		 * anyway, so it's probably not a big deal.
		 */
		/*
		 * 中文：__migrate_task() 仍可能因 hotplug 竞态静默失败并暂留不理想 CPU；
		 * CPU 下线排空随后会再次把它推出。这里记录的是已知折衷而非普通成功保证。
		 */

	} else if (pending) {
		/*
		 * This happens when we get migrated between migrate_enable()'s
		 * preempt_enable() and scheduling the stopper task. At that
		 * point we're a regular task again and not current anymore.
		 *
		 * A !PREEMPT kernel has a giant hole here, which makes it far
		 * more likely.
		 */
		/*
		 * 中文：task 可能在 migrate_enable() 的 preempt_enable 与 stopper 真正
		 * 调度之间已被迁走，此时它已恢复普通 task 且不再是 current；非抢占内核
		 * 的窗口更大。pending 仍需按当前归属完成，不能沿用旧 rq。
		 */

		/*
		 * The task moved before the stopper got to run. We're holding
		 * ->pi_lock, so the allowed mask is stable - if it got
		 * somewhere allowed, we're done.
		 */
		if (cpumask_test_cpu(task_cpu(p), p->cpus_ptr)) {
			p->migration_pending = NULL;
			complete = true;
			goto out;
		}

		/*
		 * When migrate_enable() hits a rq mis-match we can't reliably
		 * determine is_migration_disabled() and so have to chase after
		 * it.
		 */
		/*
		 * 中文：migrate_enable() 发现 rq 不匹配时，无法可靠判断远端 task 的
		 * migration_disabled 状态，只能把 stopper 追排到 task 当前 CPU。
		 */
		WARN_ON_ONCE(!pending->stop_pending);
		preempt_disable();
		rq_unlock(rq, &rf);
		raw_spin_unlock_irqrestore(&p->pi_lock, rf.flags);
		stop_one_cpu_nowait(task_cpu(p), migration_cpu_stop,
				    &pending->arg, &pending->stop_work);
		preempt_enable();
		return 0;
	}
out:
	if (pending)
		pending->stop_pending = false;
	rq_unlock(rq, &rf);
	raw_spin_unlock_irqrestore(&p->pi_lock, rf.flags);

	if (complete)
		complete_all(&pending->done);

	return 0;
}

/*
 * push_cpu_stop() - stopper 上下文把需 push 的 @p 移到 sched_class 选择的最低 rq。
 *
 * @arg 是带引用的 task，函数无论是否迁移都在出口 put。先锁 p->pi_lock 和本 rq
 * 复核归属；migration_disabled 时记录 MDF_PUSH 留给 enable；否则 class 回调可
 * 返回已锁目标 rq，双 rq 锁下移动并请求目标抢占。始终返回 0，失败表现为本轮
 * 不移动而由后续 balance/hotplug 重试。
 */
int push_cpu_stop(void *arg)
{
	struct rq *lowest_rq = NULL, *rq = this_rq();
	struct task_struct *p = arg;

	raw_spin_lock_irq(&p->pi_lock);
	raw_spin_rq_lock(rq);

	if (task_rq(p) != rq)
		goto out_unlock;

	if (is_migration_disabled(p)) {
		p->migration_flags |= MDF_PUSH;
		goto out_unlock;
	}

	p->migration_flags &= ~MDF_PUSH;

	if (p->sched_class->find_lock_rq)
		lowest_rq = p->sched_class->find_lock_rq(p, rq);

	if (!lowest_rq)
		goto out_unlock;

	lockdep_assert_rq_held(lowest_rq);

	// XXX validate p is still the highest prio task
	if (task_rq(p) == rq) {
		move_queued_task_locked(rq, lowest_rq, p);
		resched_curr(lowest_rq);
	}

	double_unlock_balance(rq, lowest_rq);

out_unlock:
	rq->push_busy = false;
	raw_spin_rq_unlock(rq);
	raw_spin_unlock_irq(&p->pi_lock);

	put_task_struct(p);
	return 0;
}

static inline void mm_update_cpus_allowed(struct mm_struct *mm, const cpumask_t *affmask);

/*
 * sched_class::set_cpus_allowed must do the below, but is not required to
 * actually call this function.
 */
/*
 * 原文：每个 sched_class 的 set_cpus_allowed 必须实现这些语义，但不要求直接调用
 * 本 helper。普通更新复制长期 cpus_mask、重算 nr_cpus_allowed、同步 mm CID；
 * MIGRATE_ENABLE/DISABLE 只切换 cpus_ptr；SCA_USER 还交换动态 user_cpus_ptr
 * ownership，使旧指针返回到 ctx 由上层释放。调用者持 task/rq 锁。
 */
void set_cpus_allowed_common(struct task_struct *p, struct affinity_context *ctx)
{
	if (ctx->flags & (SCA_MIGRATE_ENABLE | SCA_MIGRATE_DISABLE)) {
		p->cpus_ptr = ctx->new_mask;
		return;
	}

	cpumask_copy(&p->cpus_mask, ctx->new_mask);
	p->nr_cpus_allowed = cpumask_weight(ctx->new_mask);
	mm_update_cpus_allowed(p->mm, ctx->new_mask);

	/*
	 * Swap in a new user_cpus_ptr if SCA_USER flag set
	 */
	if (ctx->flags & SCA_USER)
		swap(p->user_cpus_ptr, ctx->user_mask);
}

/*
 * do_set_cpus_allowed() 用 sched_change scope 在 class 回调期间暂时撤出并恢复
 * queued/running 状态，使队列位置与新 affinity 原子更新。调用者持 task_rq_lock。
 */
static void
do_set_cpus_allowed(struct task_struct *p, struct affinity_context *ctx)
{
	scoped_guard (sched_change, p, DEQUEUE_SAVE)
		p->sched_class->set_cpus_allowed(p, ctx);
}

/*
 * Used for kthread_bind() and select_fallback_rq(), in both cases the user
 * affinity (if any) should be destroyed too.
 */
/*
 * 原文：kthread_bind() 和 fallback 选择需要强制 mask，同时销毁旧用户 affinity。
 * 函数在 p->pi_lock/rq 锁下用 SCA_USER 交换出 user mask；PREEMPT_RT 下持 pi_lock
 * 不能直接 kfree，故把旧 mask 解释为带 rcu_head 的 union 后 kfree_rcu。@p 由
 * 调用者保证存活，@new_mask 只在调用期间借用。
 */
void set_cpus_allowed_force(struct task_struct *p, const struct cpumask *new_mask)
{
	struct affinity_context ac = {
		.new_mask  = new_mask,
		.user_mask = NULL,
		.flags     = SCA_USER,	/* clear the user requested mask */
	};
	union cpumask_rcuhead {
		cpumask_t cpumask;
		struct rcu_head rcu;
	};

	scoped_guard (__task_rq_lock, p)
		do_set_cpus_allowed(p, &ac);

	/*
	 * Because this is called with p->pi_lock held, it is not possible
	 * to use kfree() here (when PREEMPT_RT=y), therefore punt to using
	 * kfree_rcu().
	 */
	kfree_rcu((union cpumask_rcuhead *)ac.user_mask, rcu);
}

/*
 * dup_user_cpus_ptr() - fork 时复制 @src 的用户原始 affinity 到未发布 @dst。
 *
 * @node 指定 NUMA 分配节点。先清 dst 指针；无锁初筛允许与 force-clear 竞态，
 * 因为丢失复制只是合法的“当前已无 user mask”。需要时分配 mask，再在 src pi_lock
 * 下复核和复制。返回 0 或 -ENOMEM；成功后 dst 拥有独立 mask，src 不变。
 */
int dup_user_cpus_ptr(struct task_struct *dst, struct task_struct *src,
		      int node)
{
	cpumask_t *user_mask;
	unsigned long flags;

	/*
	 * Always clear dst->user_cpus_ptr first as their user_cpus_ptr's
	 * may differ by now due to racing.
	 */
	dst->user_cpus_ptr = NULL;

	/*
	 * This check is racy and losing the race is a valid situation.
	 * It is not worth the extra overhead of taking the pi_lock on
	 * every fork/clone.
	 */
	/*
	 * 中文：无锁初筛可与清除 user mask 竞态；输掉竞态只意味着当前无需复制，
	 * 属于合法结果，不值得让每次 fork/clone 都获取 pi_lock。
	 */
	if (data_race(!src->user_cpus_ptr))
		return 0;

	user_mask = alloc_user_cpus_ptr(node);
	if (!user_mask)
		return -ENOMEM;

	/*
	 * Use pi_lock to protect content of user_cpus_ptr
	 *
	 * Though unlikely, user_cpus_ptr can be reset to NULL by a concurrent
	 * set_cpus_allowed_force().
	 */
	/*
	 * 中文：真正复制时用 pi_lock 保护指针内容；并发
	 * set_cpus_allowed_force() 仍可能在取锁前把它重置为 NULL，故锁内再查。
	 */
	raw_spin_lock_irqsave(&src->pi_lock, flags);
	if (src->user_cpus_ptr) {
		swap(dst->user_cpus_ptr, user_mask);
		cpumask_copy(dst->user_cpus_ptr, src->user_cpus_ptr);
	}
	raw_spin_unlock_irqrestore(&src->pi_lock, flags);

	if (unlikely(user_mask))
		kfree(user_mask);

	return 0;
}

/*
 * clear_user_cpus_ptr() 把 @p->user_cpus_ptr ownership 原子式移出字段并返回旧指针；
 * 调用者必须处于排除并发更新的上下文并负责最终 kfree/RCU free。NULL 表示无对象。
 */
static inline struct cpumask *clear_user_cpus_ptr(struct task_struct *p)
{
	struct cpumask *user_mask = NULL;

	swap(p->user_cpus_ptr, user_mask);

	return user_mask;
}

/*
 * release_user_cpus_ptr() 用于 task 销毁期，清字段并直接释放旧 user mask。调用者
 * 已保证没有并发读者需要 RCU 延迟；无返回值。
 */
void release_user_cpus_ptr(struct task_struct *p)
{
	kfree(clear_user_cpus_ptr(p));
}

/*
 * This function is wildly self concurrent; here be dragons.
 *
 *
 * When given a valid mask, __set_cpus_allowed_ptr() must block until the
 * designated task is enqueued on an allowed CPU. If that task is currently
 * running, we have to kick it out using the CPU stopper.
 *
 * Migrate-Disable comes along and tramples all over our nice sandcastle.
 * Consider:
 *
 *     Initial conditions: P0->cpus_mask = [0, 1]
 *
 *     P0@CPU0                  P1
 *
 *     migrate_disable();
 *     <preempted>
 *                              set_cpus_allowed_ptr(P0, [1]);
 *
 * P1 *cannot* return from this set_cpus_allowed_ptr() call until P0 executes
 * its outermost migrate_enable() (i.e. it exits its Migrate-Disable region).
 * This means we need the following scheme:
 *
 *     P0@CPU0                  P1
 *
 *     migrate_disable();
 *     <preempted>
 *                              set_cpus_allowed_ptr(P0, [1]);
 *                                <blocks>
 *     <resumes>
 *     migrate_enable();
 *       __set_cpus_allowed_ptr();
 *       <wakes local stopper>
 *                         `--> <woken on migration completion>
 *
 * Now the fun stuff: there may be several P1-like tasks, i.e. multiple
 * concurrent set_cpus_allowed_ptr(P0, [*]) calls. CPU affinity changes of any
 * task p are serialized by p->pi_lock, which we can leverage: the one that
 * should come into effect at the end of the Migrate-Disable region is the last
 * one. This means we only need to track a single cpumask (i.e. p->cpus_mask),
 * but we still need to properly signal those waiting tasks at the appropriate
 * moment.
 *
 * This is implemented using struct set_affinity_pending. The first
 * __set_cpus_allowed_ptr() caller within a given Migrate-Disable region will
 * setup an instance of that struct and install it on the targeted task_struct.
 * Any and all further callers will reuse that instance. Those then wait for
 * a completion signaled at the tail of the CPU stopper callback (1), triggered
 * on the end of the Migrate-Disable region (i.e. outermost migrate_enable()).
 *
 *
 * (1) In the cases covered above. There is one more where the completion is
 * signaled within affine_move_task() itself: when a subsequent affinity request
 * occurs after the stopper bailed out due to the targeted task still being
 * Migrate-Disable. Consider:
 *
 *     Initial conditions: P0->cpus_mask = [0, 1]
 *
 *     CPU0		  P1				P2
 *     <P0>
 *       migrate_disable();
 *       <preempted>
 *                        set_cpus_allowed_ptr(P0, [1]);
 *                          <blocks>
 *     <migration/0>
 *       migration_cpu_stop()
 *         is_migration_disabled()
 *           <bails>
 *                                                       set_cpus_allowed_ptr(P0, [0, 1]);
 *                                                         <signal completion>
 *                          <awakes>
 *
 * Note that the above is safe vs a concurrent migrate_enable(), as any
 * pending affinity completion is preceded by an uninstallation of
 * p->migration_pending done with p->pi_lock held.
 */
/*
 * affine_move_task() 是 affinity 更新的提交/等待核心。它在 p->pi_lock 与 rq 锁
 * 下判断可原地完成、直接迁移还是安装/复用 set_affinity_pending 并启动 stopper。
 * migration-disabled 期间多个请求共享一个 completion，最后写入的 cpus_mask 生效；
 * 函数按注解释放入口两把锁，并在需要时阻塞到 task 已位于允许 CPU。
 */
static int affine_move_task(struct rq *rq, struct task_struct *p, struct rq_flags *rf,
			    int dest_cpu, unsigned int flags)
	__releases(__rq_lockp(rq), &p->pi_lock)
{
	struct set_affinity_pending my_pending = { }, *pending = NULL;
	bool stop_pending, complete = false;

	/*
	 * Can the task run on the task's current CPU? If so, we're done
	 *
	 * We are also done if the task is the current donor, boosting a lock-
	 * holding proxy, (and potentially has been migrated outside its
	 * current or previous affinity mask)
	 */
	if (cpumask_test_cpu(task_cpu(p), &p->cpus_mask) ||
	    (task_current_donor(rq, p) && !task_current(rq, p))) {
		struct task_struct *push_task = NULL;

		if ((flags & SCA_MIGRATE_ENABLE) &&
		    (p->migration_flags & MDF_PUSH) && !rq->push_busy) {
			rq->push_busy = true;
			push_task = get_task_struct(p);
		}

		/*
		 * If there are pending waiters, but no pending stop_work,
		 * then complete now.
		 */
	/* 中文：已有等待者但 stopper 已不再 pending 时，本路径直接完成并唤醒所有等待者。 */
		pending = p->migration_pending;
		if (pending && !pending->stop_pending) {
			p->migration_pending = NULL;
			complete = true;
		}

		preempt_disable();
		task_rq_unlock(rq, p, rf);
		if (push_task) {
			stop_one_cpu_nowait(rq->cpu, push_cpu_stop,
					    p, &rq->push_work);
		}
		preempt_enable();

		if (complete)
			complete_all(&pending->done);

		return 0;
	}

	if (!(flags & SCA_MIGRATE_ENABLE)) {
		/* serialized by p->pi_lock */
		/* 中文：以下 pending 安装与更新均由 p->pi_lock 串行化。 */
		if (!p->migration_pending) {
			/* Install the request */
			/* 中文：首个请求在调用栈上建立共享 pending，并发布到 task。 */
			refcount_set(&my_pending.refs, 1);
			init_completion(&my_pending.done);
			my_pending.arg = (struct migration_arg) {
				.task = p,
				.dest_cpu = dest_cpu,
				.pending = &my_pending,
			};

			p->migration_pending = &my_pending;
		} else {
			pending = p->migration_pending;
			refcount_inc(&pending->refs);
			/*
			 * Affinity has changed, but we've already installed a
			 * pending. migration_cpu_stop() *must* see this, else
			 * we risk a completion of the pending despite having a
			 * task on a disallowed CPU.
			 *
			 * Serialized by p->pi_lock, so this is safe.
			 */
			/*
			 * 中文：已有 pending 时只更新最后目标；stopper 必须观察新目标，
			 * 否则可能在 task 仍位于非法 CPU 时错误完成。pi_lock 保证发布安全。
			 */
			pending->arg.dest_cpu = dest_cpu;
		}
	}
	pending = p->migration_pending;
	/*
	 * - !MIGRATE_ENABLE:
	 *   we'll have installed a pending if there wasn't one already.
	 *
	 * - MIGRATE_ENABLE:
	 *   we're here because the current CPU isn't matching anymore,
	 *   the only way that can happen is because of a concurrent
	 *   set_cpus_allowed_ptr() call, which should then still be
	 *   pending completion.
	 *
	 * Either way, we really should have a @pending here.
	 */
	/*
	 * 中文：普通设置会自行安装 pending；MIGRATE_ENABLE 到此说明并发 affinity
	 * 请求尚待完成。两种情形都必须存在 pending，否则是协议错误。
	 */
	if (WARN_ON_ONCE(!pending)) {
		task_rq_unlock(rq, p, rf);
		return -EINVAL;
	}

	if (task_on_cpu(rq, p) || READ_ONCE(p->__state) == TASK_WAKING) {
		/*
		 * MIGRATE_ENABLE gets here because 'p == current', but for
		 * anything else we cannot do is_migration_disabled(), punt
		 * and have the stopper function handle it all race-free.
		 */
	/*
	 * 中文：MIGRATE_ENABLE 可因 p==current 在本地判断；其他 task 无法无竞态地
	 * 查询 migration_disabled，统一交给 stopper 在完整锁保护下裁决。
	 */
		stop_pending = pending->stop_pending;
		if (!stop_pending)
			pending->stop_pending = true;

		if (flags & SCA_MIGRATE_ENABLE)
			p->migration_flags &= ~MDF_PUSH;

		preempt_disable();
		task_rq_unlock(rq, p, rf);
		if (!stop_pending) {
			stop_one_cpu_nowait(cpu_of(rq), migration_cpu_stop,
					    &pending->arg, &pending->stop_work);
		}
		preempt_enable();

		if (flags & SCA_MIGRATE_ENABLE)
			return 0;
	} else {

		if (!is_migration_disabled(p)) {
			if (task_on_rq_queued(p))
				rq = move_queued_task(rq, rf, p, dest_cpu);

			if (!pending->stop_pending) {
				p->migration_pending = NULL;
				complete = true;
			}
		}
		task_rq_unlock(rq, p, rf);

		if (complete)
			complete_all(&pending->done);
	}

	wait_for_completion(&pending->done);

	if (refcount_dec_and_test(&pending->refs))
		wake_up_var(&pending->refs); /* No UaF, just an address */

	/*
	 * Block the original owner of &pending until all subsequent callers
	 * have seen the completion and decremented the refcount
	 */
	wait_var_event(&my_pending.refs, !refcount_read(&my_pending.refs));

	/* ARGH */
	WARN_ON_ONCE(my_pending.stop_pending);

	return 0;
}

/*
 * affine_move_task() 是 affinity 更新的提交/等待状态机。入口同时持 p->pi_lock
 * 和 @rq 锁，函数保证出口释放二者。若当前 CPU 已合法，清理可完成的 pending；
 * 若 task 正在运行/WAKING，唯一提交 stopper；若 queued 且可迁移则直接移 rq。
 * migration_disabled 时首个调用者在栈上安装 pending，后续调用者复用并更新最后
 * dest_cpu，所有人等待同一 completion；原 owner 再等 refs 归零防止栈 UAF。
 * 返回 0 表示调用返回时 task 已位于允许 CPU 或请求已由 migrate_enable 兑现；
 * 内部不取得永久 task ownership。
 */

/*
 * Called with both p->pi_lock and rq->lock held; drops both before returning.
 */
/*
 * 原文：入口持 p->pi_lock 与 rq 锁，所有出口都会释放两者。
 *
 * locked helper 选择用户 task 的 possible mask 或 kthread/migration-disabled 的
 * online mask，校验 PF_NO_SETAFFINITY、空 mask 和当前 disable 自排除；从合法
 * 集合分散选择 dest_cpu 后先调用 class 更新 mask，再交 affine_move_task 兑现
 * 迁移。返回 0 或 -EINVAL/-EBUSY，校验失败保持旧 affinity。
 */
static int __set_cpus_allowed_ptr_locked(struct task_struct *p,
					 struct affinity_context *ctx,
					 struct rq *rq,
					 struct rq_flags *rf)
	__releases(__rq_lockp(rq), &p->pi_lock)
{
	const struct cpumask *cpu_allowed_mask = task_cpu_possible_mask(p);
	const struct cpumask *cpu_valid_mask = cpu_active_mask;
	bool kthread = p->flags & PF_KTHREAD;
	unsigned int dest_cpu;
	int ret = 0;

	if (kthread || is_migration_disabled(p)) {
		/*
		 * Kernel threads are allowed on online && !active CPUs,
		 * however, during cpu-hot-unplug, even these might get pushed
		 * away if not KTHREAD_IS_PER_CPU.
		 *
		 * Specifically, migration_disabled() tasks must not fail the
		 * cpumask_any_and_distribute() pick below, esp. so on
		 * SCA_MIGRATE_ENABLE, otherwise we'll not call
		 * set_cpus_allowed_common() and actually reset p->cpus_ptr.
		 */
		/*
		 * 中文：kthread 可用 online 但未 active 的 CPU，普通非 per-CPU kthread
		 * 在下线时仍会被推出。migration-disabled task 也必须基于 online mask
		 * 选到 CPU，尤其 MIGRATE_ENABLE 不能因选核失败而漏掉 cpus_ptr 恢复。
		 */
		cpu_valid_mask = cpu_online_mask;
	}

	if (!kthread && !cpumask_subset(ctx->new_mask, cpu_allowed_mask)) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Must re-check here, to close a race against __kthread_bind(),
	 * sched_setaffinity() is not guaranteed to observe the flag.
	 */
	/* 中文：锁内重查 PF_NO_SETAFFINITY，关闭与 __kthread_bind() 的无锁观察竞态。 */
	if ((ctx->flags & SCA_CHECK) && (p->flags & PF_NO_SETAFFINITY)) {
		ret = -EINVAL;
		goto out;
	}

	if (!(ctx->flags & SCA_MIGRATE_ENABLE)) {
		if (cpumask_equal(&p->cpus_mask, ctx->new_mask)) {
			if (ctx->flags & SCA_USER)
				swap(p->user_cpus_ptr, ctx->user_mask);
			goto out;
		}

		if (WARN_ON_ONCE(p == current &&
				 is_migration_disabled(p) &&
				 !cpumask_test_cpu(task_cpu(p), ctx->new_mask))) {
			ret = -EBUSY;
			goto out;
		}
	}

	/*
	 * Picking a ~random cpu helps in cases where we are changing affinity
	 * for groups of tasks (ie. cpuset), so that load balancing is not
	 * immediately required to distribute the tasks within their new mask.
	 */
	/*
	 * 中文：在新 mask 中近似随机分散选核，使 cpuset 批量改 affinity 时不会把
	 * task 全压到首个 CPU，减少紧随其后的负载均衡。
	 */
	dest_cpu = cpumask_any_and_distribute(cpu_valid_mask, ctx->new_mask);
	if (dest_cpu >= nr_cpu_ids) {
		ret = -EINVAL;
		goto out;
	}

	do_set_cpus_allowed(p, ctx);

	return affine_move_task(rq, p, rf, dest_cpu, ctx->flags);

out:
	task_rq_unlock(rq, p, rf);

	return ret;
}

/*
 * Change a given task's CPU affinity. Migrate the thread to a
 * proper CPU and schedule it away if the CPU it's executing on
 * is removed from the allowed bitmask.
 *
 * NOTE: the caller must have a valid reference to the task, the
 * task must not exit() & deallocate itself prematurely. The
 * call is not atomic; no spinlocks may be held.
 */
/*
 * 原文完整契约：@p 必须由调用者持有有效引用，整个非原子调用期间不能自行 exit
 * 并释放；入口不能持 spinlock，因为等待迁移 stopper 时可以睡眠。@ctx 的 mask
 * 与 flags 在调用期间借用。函数锁住 p/rq、应用 user mask 限制，再进入 locked
 * helper；返回 0 表示新亲和性及必要迁移完成，负 errno 表示未能建立合法目标。
 */
int __set_cpus_allowed_ptr(struct task_struct *p, struct affinity_context *ctx)
{
	struct rq_flags rf;
	struct rq *rq;

	rq = task_rq_lock(p, &rf);
	/*
	 * Masking should be skipped if SCA_USER or any of the SCA_MIGRATE_*
	 * flags are set.
	 */
	if (p->user_cpus_ptr &&
	    !(ctx->flags & (SCA_USER | SCA_MIGRATE_ENABLE | SCA_MIGRATE_DISABLE)) &&
	    cpumask_and(rq->scratch_mask, ctx->new_mask, p->user_cpus_ptr))
		ctx->new_mask = rq->scratch_mask;

	return __set_cpus_allowed_ptr_locked(p, ctx, rq, &rf);
}

/*
 * set_cpus_allowed_ptr() 是无特殊 flags 的公共 wrapper。@p 必须有稳定引用，
 * @new_mask 在可睡眠调用期间借用；返回 __set_cpus_allowed_ptr() 的 0/errno。
 */
int set_cpus_allowed_ptr(struct task_struct *p, const struct cpumask *new_mask)
{
	struct affinity_context ac = {
		.new_mask  = new_mask,
		.flags     = 0,
	};

	return __set_cpus_allowed_ptr(p, &ac);
}
EXPORT_SYMBOL_GPL(set_cpus_allowed_ptr);

/*
 * Change a given task's CPU affinity to the intersection of its current
 * affinity mask and @subset_mask, writing the resulting mask to @new_mask.
 * If user_cpus_ptr is defined, use it as the basis for restricting CPU
 * affinity or use cpu_online_mask instead.
 *
 * If the resulting mask is empty, leave the affinity unchanged and return
 * -EINVAL.
 */
/*
 * 原文：把 @p affinity 限制为当前用户/online mask 与 @subset_mask 的交集，并把
 * 结果写入调用者拥有的 @new_mask；交集为空保持原配置并返回 -EINVAL。DL 带宽
 * 开启时强制收窄返回 -EPERM，防止破坏 admission 假设。成功路径在持锁状态直接
 * 进入 locked helper 并由其释放锁。
 */
static int restrict_cpus_allowed_ptr(struct task_struct *p,
				     struct cpumask *new_mask,
				     const struct cpumask *subset_mask)
{
	struct affinity_context ac = {
		.new_mask  = new_mask,
		.flags     = 0,
	};
	struct rq_flags rf;
	struct rq *rq;
	int err;

	rq = task_rq_lock(p, &rf);

	/*
	 * Forcefully restricting the affinity of a deadline task is
	 * likely to cause problems, so fail and noisily override the
	 * mask entirely.
	 */
	/*
	 * 中文：强制收窄 deadline task 可能破坏已准入的带宽条件，因此显式失败，
	 * 由更高层告警并采用完整 override mask。
	 */
	if (task_has_dl_policy(p) && dl_bandwidth_enabled()) {
		err = -EPERM;
		goto err_unlock;
	}

	if (!cpumask_and(new_mask, task_user_cpus(p), subset_mask)) {
		err = -EINVAL;
		goto err_unlock;
	}

	return __set_cpus_allowed_ptr_locked(p, &ac, rq, &rf);

err_unlock:
	task_rq_unlock(rq, p, &rf);
	return err;
}

/*
 * Restrict the CPU affinity of task @p so that it is a subset of
 * task_cpu_possible_mask() and point @p->user_cpus_ptr to a copy of the
 * old affinity mask. If the resulting mask is empty, we warn and walk
 * up the cpuset hierarchy until we find a suitable mask.
 */
/*
 * 原文：强制让 @p mask 成为 task_cpu_possible_mask 的子集，并保存旧用户 mask；
 * 若直接交集为空，沿 cpuset 层级找 fallback。函数可睡眠和分配 cpumask，持
 * cpus_read_lock 防目的 CPU 并发下线；分配失败也会用 possible mask 兜底。
 * 无返回值，失败通过警告暴露但必须给 task 留下可运行集合。
 */
void force_compatible_cpus_allowed_ptr(struct task_struct *p)
{
	cpumask_var_t new_mask;
	const struct cpumask *override_mask = task_cpu_possible_mask(p);

	alloc_cpumask_var(&new_mask, GFP_KERNEL);

	/*
	 * __migrate_task() can fail silently in the face of concurrent
	 * offlining of the chosen destination CPU, so take the hotplug
	 * lock to ensure that the migration succeeds.
	 */
	cpus_read_lock();
	if (!cpumask_available(new_mask))
		goto out_set_mask;

	if (!restrict_cpus_allowed_ptr(p, new_mask, override_mask))
		goto out_free_mask;

	/*
	 * We failed to find a valid subset of the affinity mask for the
	 * task, so override it based on its cpuset hierarchy.
	 */
	/* 中文：原 affinity 找不到合法子集时，改用 cpuset 层级计算的允许集合兜底。 */
	cpuset_cpus_allowed(p, new_mask);
	override_mask = new_mask;

out_set_mask:
	if (printk_ratelimit()) {
		printk_deferred("Overriding affinity for process %d (%s) to CPUs %*pbl\n",
				task_pid_nr(p), p->comm,
				cpumask_pr_args(override_mask));
	}

	WARN_ON(set_cpus_allowed_ptr(p, override_mask));
out_free_mask:
	cpus_read_unlock();
	free_cpumask_var(new_mask);
}

/*
 * Restore the affinity of a task @p which was previously restricted by a
 * call to force_compatible_cpus_allowed_ptr().
 *
 * It is the caller's responsibility to serialise this with any calls to
 * force_compatible_cpus_allowed_ptr(@p).
 */
/*
 * 原文：恢复曾被 force_compatible 限制的 @p 用户 affinity；调用者必须与 force
 * 串行化。函数通过 __sched_setaffinity 重新应用 cpuset 掩码，失败只 WARN，
 * 无直接返回值，user_cpus_ptr 的 ownership 仍由 task affinity 协议管理。
 */
void relax_compatible_cpus_allowed_ptr(struct task_struct *p)
{
	struct affinity_context ac = {
		.new_mask  = task_user_cpus(p),
		.flags     = 0,
	};
	int ret;

	/*
	 * Try to restore the old affinity mask with __sched_setaffinity().
	 * Cpuset masking will be done there too.
	 */
	ret = __sched_setaffinity(p, &ac);
	WARN_ON_ONCE(ret);
}

#ifdef CONFIG_SMP

/*
 * set_task_cpu() - 在既定迁移/唤醒锁协议中发布 @p 的新 CPU 归属。
 *
 * @p 为借用 task，@new_cpu 必须 online。waking task 持 p->pi_lock，runnable
 * task 持 rq 锁；fair runnable 迁移还必须先标记 ON_RQ_MIGRATING。函数先调用
 * class migrate hook、更新迁移统计/perf，再由 __set_task_cpu 真正写字段。
 * blocked 或 migration_disabled task 调用属于协议错误并 WARN；无返回值。
 */
void set_task_cpu(struct task_struct *p, unsigned int new_cpu)
{
	unsigned int state = READ_ONCE(p->__state);

	/*
	 * We should never call set_task_cpu() on a blocked task,
	 * ttwu() will sort out the placement.
	 */
	WARN_ON_ONCE(state != TASK_RUNNING && state != TASK_WAKING && !p->on_rq);

	/*
	 * Migrating fair class task must have p->on_rq = TASK_ON_RQ_MIGRATING,
	 * because schedstat_wait_{start,end} rebase migrating task's wait_start
	 * time relying on p->on_rq.
	 */
	/*
	 * 中文：迁移 fair task 时 on_rq 必须为 MIGRATING；schedstat 的 wait_start
	 * 重基准依赖该状态区分普通在队与跨 rq 过渡。
	 */
	WARN_ON_ONCE(state == TASK_RUNNING &&
		     p->sched_class == &fair_sched_class &&
		     (p->on_rq && !task_on_rq_migrating(p)));

#ifdef CONFIG_LOCKDEP
	/*
	 * The caller should hold either p->pi_lock or rq->lock, when changing
	 * a task's CPU. ->pi_lock for waking tasks, rq->lock for runnable tasks.
	 *
	 * sched_move_task() holds both and thus holding either pins the cgroup,
	 * see task_group().
	 *
	 * Furthermore, all task_rq users should acquire both locks, see
	 * task_rq_lock().
	 */
	/*
	 * 中文：唤醒 task 改 CPU 需 pi_lock，在队 task 需 rq 锁；group move 同时
	 * 持有二者。所有读取 task_rq 的稳定用户则应按 task_rq_lock 取得两锁。
	 */
	WARN_ON_ONCE(debug_locks && !(lockdep_is_held(&p->pi_lock) ||
				      lockdep_is_held(__rq_lockp(task_rq(p)))));
#endif
	/*
	 * Clearly, migrating tasks to offline CPUs is a fairly daft thing.
	 */
	/* 中文：迁往 offline CPU 明显违反目标可运行性，调试路径直接告警。 */
	WARN_ON_ONCE(!cpu_online(new_cpu));

	WARN_ON_ONCE(is_migration_disabled(p));

	trace_sched_migrate_task(p, new_cpu);

	if (task_cpu(p) != new_cpu) {
		if (p->sched_class->migrate_task_rq)
			p->sched_class->migrate_task_rq(p, new_cpu);
		p->se.nr_migrations++;
		perf_event_task_migrate(p);
	}

	__set_task_cpu(p, new_cpu);
}
#endif /* CONFIG_SMP */

#ifdef CONFIG_NUMA_BALANCING
/*
 * __migrate_swap_task() 在两个 rq 与两个 task pi_lock 均稳定后，把 @p 迁往 @cpu。
 * queued task 直接跨已锁 rq 移动并检查抢占；已睡眠 task 只设置 wake_cpu，使下次
 * 唤醒看起来从新目标继续。无返回值，不改变 task 引用。
 */
static void __migrate_swap_task(struct task_struct *p, int cpu)
{
	if (task_on_rq_queued(p)) {
		struct rq *src_rq, *dst_rq;
		struct rq_flags srf, drf;

		src_rq = task_rq(p);
		dst_rq = cpu_rq(cpu);

		rq_pin_lock(src_rq, &srf);
		rq_pin_lock(dst_rq, &drf);

		move_queued_task_locked(src_rq, dst_rq, p);
		wakeup_preempt(dst_rq, p, 0);

		rq_unpin_lock(dst_rq, &drf);
		rq_unpin_lock(src_rq, &srf);

	} else {
		/*
		 * Task isn't running anymore; make it appear like we migrated
		 * it before it went to sleep. This means on wakeup we make the
		 * previous CPU our target instead of where it really is.
		 */
		p->wake_cpu = cpu;
	}
}

/*
 * migration_swap_arg 保存 NUMA 成对迁移的 task 借用指针与 CPU 快照；stopper
 * 取得全部锁后必须重新验证这些快照。
 */
struct migration_swap_arg {
	struct task_struct *src_task, *dst_task;
	int src_cpu, dst_cpu;
};

/*
 * migration_swap_arg 是 stop_two_cpus() 的按值事务描述：两 task 是调用期间稳定的
 * 借用指针，src/dst_cpu 是提交前快照。stop 回调必须逐项复核，任何变化都返回
 * -EAGAIN 而不是交换过时对象。
 */

/*
 * migrate_swap_stop() 在两个 CPU stopper 上下文联合锁两 task pi_lock 和两 rq，
 * 复核 CPU active、task 归属和交叉 affinity 后执行成对迁移。返回 0 表示交换
 * 完成，-EAGAIN 表示快照已失效可由上层重试；任一失败都不做半边交换。
 */
static int migrate_swap_stop(void *data)
{
	struct migration_swap_arg *arg = data;
	struct rq *src_rq, *dst_rq;

	if (!cpu_active(arg->src_cpu) || !cpu_active(arg->dst_cpu))
		return -EAGAIN;

	src_rq = cpu_rq(arg->src_cpu);
	dst_rq = cpu_rq(arg->dst_cpu);

	guard(double_raw_spinlock)(&arg->src_task->pi_lock, &arg->dst_task->pi_lock);
	guard(double_rq_lock)(src_rq, dst_rq);

	if (task_cpu(arg->dst_task) != arg->dst_cpu)
		return -EAGAIN;

	if (task_cpu(arg->src_task) != arg->src_cpu)
		return -EAGAIN;

	if (!cpumask_test_cpu(arg->dst_cpu, arg->src_task->cpus_ptr))
		return -EAGAIN;

	if (!cpumask_test_cpu(arg->src_cpu, arg->dst_task->cpus_ptr))
		return -EAGAIN;

	__migrate_swap_task(arg->src_task, arg->dst_cpu);
	__migrate_swap_task(arg->dst_task, arg->src_cpu);

	return 0;
}

/*
 * Cross migrate two tasks
 */
/*
 * 中文：交叉迁移两个 task。前置无锁检查只用于快速失败，stop_two_cpus() 回调会
 * 在 task pi_lock 和两 rq 锁下全部复核，因此竞态只导致 -EAGAIN/失败，不会半迁移。
 */
int migrate_swap(struct task_struct *cur, struct task_struct *p,
		int target_cpu, int curr_cpu)
{
	struct migration_swap_arg arg;
	int ret = -EINVAL;

	arg = (struct migration_swap_arg){
		.src_task = cur,
		.src_cpu = curr_cpu,
		.dst_task = p,
		.dst_cpu = target_cpu,
	};

	if (arg.src_cpu == arg.dst_cpu)
		goto out;

	/*
	 * These three tests are all lockless; this is OK since all of them
	 * will be re-checked with proper locks held further down the line.
	 */
	/* 中文：三项检查均无锁，但稍后会在正确锁保护下重新验证。 */
	if (!cpu_active(arg.src_cpu) || !cpu_active(arg.dst_cpu))
		goto out;

	if (!cpumask_test_cpu(arg.dst_cpu, arg.src_task->cpus_ptr))
		goto out;

	if (!cpumask_test_cpu(arg.src_cpu, arg.dst_task->cpus_ptr))
		goto out;

	trace_sched_swap_numa(cur, arg.src_cpu, p, arg.dst_cpu);
	ret = stop_two_cpus(arg.dst_cpu, arg.src_cpu, migrate_swap_stop, &arg);

out:
	return ret;
}
#endif /* CONFIG_NUMA_BALANCING */

/***
 * kick_process - kick a running thread to enter/exit the kernel
 * @p: the to-be-kicked thread
 *
 * Cause a process which is running on another CPU to enter
 * kernel-mode, without any delay. (to get signals handled.)
 *
 * NOTE: this function doesn't have to take the runqueue lock,
 * because all it wants to ensure is that the remote task enters
 * the kernel. If the IPI races and the task has been migrated
 * to another CPU then no harm is done and the purpose has been
 * achieved as well.
 */
/*
 * 原文：若 @p 正在远端 CPU 用户态执行，发送 reschedule IPI 促使它立即进内核，
 * 以处理 signal 等事件。无需 rq 锁：若 task 已迁移，迁移本身已使其经过内核，
 * 目标同样达成。函数临时禁抢占以稳定本 CPU 编号；无返回值或 task 引用获取。
 */
void kick_process(struct task_struct *p)
{
	guard(preempt)();
	int cpu = task_cpu(p);

	if ((cpu != smp_processor_id()) && task_curr(p))
		smp_send_reschedule(cpu);
}
EXPORT_SYMBOL_GPL(kick_process);

/*
 * ->cpus_ptr is protected by both rq->lock and p->pi_lock
 *
 * A few notes on cpu_active vs cpu_online:
 *
 *  - cpu_active must be a subset of cpu_online
 *
 *  - on CPU-up we allow per-CPU kthreads on the online && !active CPU,
 *    see __set_cpus_allowed_ptr(). At this point the newly online
 *    CPU isn't yet part of the sched domains, and balancing will not
 *    see it.
 *
 *  - on CPU-down we clear cpu_active() to mask the sched domains and
 *    avoid the load balancer to place new tasks on the to be removed
 *    CPU. Existing tasks will remain running there and will be taken
 *    off.
 *
 * This means that fallback selection must not select !active CPUs.
 * And can assume that any active CPU must be online. Conversely
 * select_task_rq() below may allow selection of !active CPUs in order
 * to satisfy the above rules.
 */
/*
 * 原文说明 cpus_ptr 同时受 rq 锁和 p->pi_lock 保护，并区分 online 与 active：
 * active 必为 online 子集；CPU-up 期间 per-CPU kthread 可先用 online 非 active CPU，
 * CPU-down 先清 active 阻止普通 placement，再迁走存量 task。因此 fallback 不能
 * 选择非 active CPU，而普通 select_task_rq() 为特殊 kthread 规则可以。
 *
 * select_fallback_rq() 先找同 NUMA node 合法 CPU，再尝试当前 cpuset；无解时让
 * cpuset 放宽，最后强制 possible fallback，仍无解 BUG，因为 runnable task 必须
 * 有落点。返回 CPU 编号并可能改变 p affinity；调用者持 p->pi_lock。
 */
static int select_fallback_rq(int cpu, struct task_struct *p)
{
	int nid = cpu_to_node(cpu);
	const struct cpumask *nodemask = NULL;
	enum { cpuset, possible, fail } state = cpuset;
	int dest_cpu;

	/*
	 * If the node that the CPU is on has been offlined, cpu_to_node()
	 * will return -1. There is no CPU on the node, and we should
	 * select the CPU on the other node.
	 */
	/*
	 * 中文：源 CPU 所在 NUMA node 若已离线，cpu_to_node() 返回 -1；此时跳过
	 * 同 node 优选，直接在其他节点寻找合法 CPU。
	 */
	if (nid != -1) {
		nodemask = cpumask_of_node(nid);

		/* Look for allowed, online CPU in same node. */
		/* 中文：优先保持 NUMA 局部性，在同节点查找允许且可用的 CPU。 */
		for_each_cpu(dest_cpu, nodemask) {
			if (is_cpu_allowed(p, dest_cpu))
				return dest_cpu;
		}
	}

	for (;;) {
		/* Any allowed, online CPU? */
		/* 中文：在当前 cpus_ptr 中查找任意仍合法的 CPU。 */
		for_each_cpu(dest_cpu, p->cpus_ptr) {
			if (!is_cpu_allowed(p, dest_cpu))
				continue;

			goto out;
		}

		/* No more Mr. Nice Guy. */
		/* 中文：严格 affinity 已无落点，开始逐级放宽约束以保证 task 可运行。 */
		switch (state) {
		case cpuset:
			if (cpuset_cpus_allowed_fallback(p)) {
				state = possible;
				break;
			}
			fallthrough;
		case possible:
			set_cpus_allowed_force(p, task_cpu_fallback_mask(p));
			state = fail;
			break;
		case fail:
			BUG();
			break;
		}
	}

out:
	if (state != cpuset) {
		/*
		 * Don't tell them about moving exiting tasks or
		 * kernel threads (both mm NULL), since they never
		 * leave kernel.
		 */
		/*
		 * 中文：退出中的 task 与 kernel thread 都没有 mm、不会再回用户态，
		 * 因而无需向用户报告 affinity 被强制改写。
		 */
		if (p->mm && printk_ratelimit()) {
			printk_deferred("process %d (%s) no longer affine to cpu%d\n",
					task_pid_nr(p), p->comm, cpu);
		}
	}

	return dest_cpu;
}

/*
 * The caller (fork, wakeup) owns p->pi_lock, ->cpus_ptr is stable.
 */
/*
 * 原文：fork/wakeup 调用者持 p->pi_lock，因此 cpus_ptr 稳定。多 CPU 且未禁迁移
 * 时让 sched_class 选择并回写 WF_RQ_SELECTED；否则取唯一允许 CPU。class 返回
 * 非法/热插拔失效目标时统一走 fallback。@wake_flags 是输入输出参数；返回合法
 * CPU 编号，不预留该 CPU。
 */
static inline
int select_task_rq(struct task_struct *p, int cpu, int *wake_flags)
{
	lockdep_assert_held(&p->pi_lock);

	if (p->nr_cpus_allowed > 1 && !is_migration_disabled(p)) {
		cpu = p->sched_class->select_task_rq(p, cpu, *wake_flags);
		*wake_flags |= WF_RQ_SELECTED;
	} else {
		cpu = cpumask_any(p->cpus_ptr);
	}

	/*
	 * In order not to call set_task_cpu() on a blocking task we need
	 * to rely on ttwu() to place the task on a valid ->cpus_ptr
	 * CPU.
	 *
	 * Since this is common to all placement strategies, this lives here.
	 *
	 * [ this allows ->select_task() to simply return task_cpu(p) and
	 *   not worry about this generic constraint ]
	 */
	if (unlikely(!is_cpu_allowed(p, cpu)))
		cpu = select_fallback_rq(task_cpu(p), p);

	return cpu;
}

/*
 * sched_set_stop_task() - 安装或替换 @cpu 的最高优先级 stopper task。
 *
 * @stop 可空；新 stopper 先伪装成用户可理解的最高 FIFO，再切 stop_sched_class，
 * 并把 pi_lock 放入独立 lockdep class，因为 stopper 永不阻塞、不进入 PI 链。
 * 旧 stopper 切回 RT class 以便正常退出。rq/stop 生命周期由 CPU stopper 初始化
 * 协议保证；无返回值。
 */
void sched_set_stop_task(int cpu, struct task_struct *stop)
{
	static struct lock_class_key stop_pi_lock;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };
	struct task_struct *old_stop = cpu_rq(cpu)->stop;

	if (stop) {
		/*
		 * Make it appear like a SCHED_FIFO task, its something
		 * userspace knows about and won't get confused about.
		 *
		 * Also, it will make PI more or less work without too
		 * much confusion -- but then, stop work should not
		 * rely on PI working anyway.
		 */
		sched_setscheduler_nocheck(stop, SCHED_FIFO, &param);

		stop->sched_class = &stop_sched_class;

		/*
		 * The PI code calls rt_mutex_setprio() with ->pi_lock held to
		 * adjust the effective priority of a task. As a result,
		 * rt_mutex_setprio() can trigger (RT) balancing operations,
		 * which can then trigger wakeups of the stop thread to push
		 * around the current task.
		 *
		 * The stop task itself will never be part of the PI-chain, it
		 * never blocks, therefore that ->pi_lock recursion is safe.
		 * Tell lockdep about this by placing the stop->pi_lock in its
		 * own class.
		 */
		/*
		 * 中文：PI 在持 pi_lock 时调 rt_mutex_setprio()，可能触发 RT balance
		 * 并唤醒 stopper，形式上形成 stopper pi_lock 递归。stop task 永不阻塞、
		 * 不加入 PI chain，因此递归实际安全；单独 lockdep class 用于表达该例外。
		 */
		lockdep_set_class(&stop->pi_lock, &stop_pi_lock);
	}

	cpu_rq(cpu)->stop = stop;

	if (old_stop) {
		/*
		 * Reset it back to a normal scheduling class so that
		 * it can die in pieces.
		 */
		old_stop->sched_class = &rt_sched_class;
	}
}

/*
 * ttwu_stat() 仅在 schedstats 启用时统计本地/远端、跨 domain、迁移和同步唤醒。
 * @p 由唤醒锁协议稳定，@cpu 是最终目标快照；统计不参与正确性，关闭时无热路径成本。
 */
static void
ttwu_stat(struct task_struct *p, int cpu, int wake_flags)
{
	struct rq *rq;

	if (!schedstat_enabled())
		return;

	rq = this_rq();

	if (cpu == rq->cpu) {
		__schedstat_inc(rq->ttwu_local);
		__schedstat_inc(p->stats.nr_wakeups_local);
	} else {
		struct sched_domain *sd;

		__schedstat_inc(p->stats.nr_wakeups_remote);

		guard(rcu)();
		for_each_domain(rq->cpu, sd) {
			if (cpumask_test_cpu(cpu, sched_domain_span(sd))) {
				__schedstat_inc(sd->ttwu_wake_remote);
				break;
			}
		}
	}

	if (wake_flags & WF_MIGRATED)
		__schedstat_inc(p->stats.nr_wakeups_migrate);

	__schedstat_inc(rq->ttwu_count);
	__schedstat_inc(p->stats.nr_wakeups);

	if (wake_flags & WF_SYNC)
		__schedstat_inc(p->stats.nr_wakeups_sync);
}

/*
 * Mark the task runnable.
 */
/*
 * 原文：把 @p 标记为 runnable。先清 proxy/block 语义位，再 WRITE_ONCE 发布
 * TASK_RUNNING 并发 tracepoint。调用者已经完成 rq 入队或确认仍 queued；无引用
 * 转移。不能提前写 RUNNING，否则并发观察者会看到尚未可运行的 task。
 */
static inline void ttwu_do_wakeup(struct task_struct *p)
{
	p->is_blocked = 0;
	WRITE_ONCE(p->__state, TASK_RUNNING);
	trace_sched_wakeup(p);
}

/*
 * update_rq_avg_idle() 在 idle 结束时把本次 idle 纳秒差加入平滑平均，并钳到两倍
 * max_idle_balance_cost，避免过久 idle 让下一次平衡扫描预算失真。@rq 已锁，
 * 无返回值，最后清 idle_stamp 表示该区间已消费。
 */
void update_rq_avg_idle(struct rq *rq)
{
	u64 delta = rq_clock(rq) - rq->idle_stamp;
	u64 max = 2*rq->max_idle_balance_cost;

	update_avg(&rq->avg_idle, delta);

	if (rq->avg_idle > max)
		rq->avg_idle = max;
	rq->idle_stamp = 0;
}

#ifdef CONFIG_SCHED_PROXY_EXEC
static void zap_balance_callbacks(struct rq *rq);

/*
 * proxy_reset_donor() 终止当前 proxy donor：把调度类状态切回真实 curr，重置 donor，
 * 丢弃本轮普通 balance callbacks 并请求重调度。调用者持 rq 锁。
 */
static inline void proxy_reset_donor(struct rq *rq)
{
	WARN_ON_ONCE(rq->donor == rq->curr);

	put_prev_set_next_task(rq, rq->donor, rq->curr);
	rq_set_donor(rq, rq->curr);
	zap_balance_callbacks(rq);
	resched_curr(rq);
}

/*
 * Checks to see if task p has been proxy-migrated to another rq
 * and needs to be returned. If so, we deactivate the task here
 * so that it can be properly woken up on the p->wake_cpu
 * (or whichever cpu select_task_rq() picks at the bottom of
 * try_to_wake_up()
 */
/*
 * 中文：检查 @p 是否因 proxy execution 被临时迁到其他 rq、现需返回 wake_cpu。
 * 若需要，先在 blocked_lock 下清依赖并处理 donor，再把 task 变为 TASK_WAKING，
 * 让 try_to_wake_up() 底部按正常选核路径重新放置。返回 true 表示已撤出当前 rq。
 */
static inline bool proxy_needs_return(struct rq *rq, struct task_struct *p)
{
	/*
	 * Typically per __set_task_cpu(), task_cpu(p) == p->wake_cpu.
	 *
	 * However, proxy_set_task_cpu() is such that it preserves the
	 * original cpu in p->wake_cpu while migrating p for proxy reasons
	 * (possibly outside of the allowed p->cpus_ptr).
	 *
	 * Furthermore, migration_cpu_stop() / __migrate_swap_task(), will
	 * only set p->wake_cpu when !p->on_rq, and since here p->on_rq, this
	 * will not apply. But if it did, this check is the safe way around
	 * and would migrate.
	 */
	/*
	 * 中文：普通迁移保持 task_cpu==wake_cpu；proxy 迁移刻意保存原 wake_cpu。
	 * 即使其他迁移路径理论上不会在 on_rq 时改 wake_cpu，此比较仍是安全裁决。
	 */
	if (task_cpu(p) == p->wake_cpu)
		return false;

	scoped_guard(raw_spinlock, &p->blocked_lock) {
		/* Task is waking up; clear any blocked_on relationship */
		/* 中文：task 正在唤醒，先解除 blocked_on 关系。 */
		__clear_task_blocked_on(p, NULL);

		/* If already current, don't need to return migrate */
		/* 中文：已经是 curr 时不能在这里返回迁移。 */
		if (task_current(rq, p))
			return false;

		/* If we're return migrating the rq->donor, switch it out for idle */
		/* 中文：若返回的是 donor，先安全切出 donor。 */
		if (task_current_donor(rq, p))
			proxy_reset_donor(rq);
	}
	block_task(rq, p, TASK_WAKING);
	return true;
}
#else /* !CONFIG_SCHED_PROXY_EXEC */
static inline bool proxy_needs_return(struct rq *rq, struct task_struct *p)
{
	return false;
}
#endif /* CONFIG_SCHED_PROXY_EXEC */

static void
ttwu_do_activate(struct rq *rq, struct task_struct *p, int wake_flags,
		 struct rq_flags *rf)
{
	int en_flags = ENQUEUE_WAKEUP | ENQUEUE_NOCLOCK;

	lockdep_assert_rq_held(rq);

	if (p->sched_contributes_to_load)
		rq->nr_uninterruptible--;

	if (wake_flags & WF_RQ_SELECTED)
		en_flags |= ENQUEUE_RQ_SELECTED;
	if (wake_flags & WF_MIGRATED)
		en_flags |= ENQUEUE_MIGRATED;
	else if (p->in_iowait) {
		delayacct_blkio_end(p);
		atomic_dec(&task_rq(p)->nr_iowait);
	}

	activate_task(rq, p, en_flags);
	wakeup_preempt(rq, p, wake_flags);

	ttwu_do_wakeup(p);

	if (p->sched_class->task_woken) {
		/*
		 * Our task @p is fully woken up and running; so it's safe to
		 * drop the rq->lock, hereafter rq is only used for statistics.
		 */
		rq_unpin_lock(rq, rf);
		p->sched_class->task_woken(rq, p);
		rq_repin_lock(rq, rf);
	}
}

/*
 * ttwu_do_activate() 在已锁目标 @rq 完成 full wakeup：撤销 uninterruptible/iowait
 * 记账，组合 enqueue flags，activate_task() 入队，检查抢占，最后发布 RUNNING。
 * 若 class task_woken 回调要求 rq 锁外运行，使用 @rf 暂时 unpin/repin；回调时 p
 * 已完全唤醒，rq 仅供统计。无返回值或引用转移。
 */

/*
 * Consider @p being inside a wait loop:
 *
 *   for (;;) {
 *      set_current_state(TASK_UNINTERRUPTIBLE);
 *
 *      if (CONDITION)
 *         break;
 *
 *      schedule();
 *   }
 *   __set_current_state(TASK_RUNNING);
 *
 * between set_current_state() and schedule(). In this case @p is still
 * runnable, so all that needs doing is change p->state back to TASK_RUNNING in
 * an atomic manner.
 *
 * By taking task_rq(p)->lock we serialize against schedule(), if @p->on_rq
 * then schedule() must still happen and p->state can be changed to
 * TASK_RUNNING. Otherwise we lost the race, schedule() has happened, and we
 * need to do a full wakeup with enqueue.
 *
 * Returns: %true when the wakeup is done,
 *          %false otherwise.
 */
/*
 * 原文：若 waker 落在 sleeper 设置 state 与真正 schedule 之间，task 仍在 rq，
 * 只需在 rq 锁下把 state 改回 RUNNING；若 on_rq 已清，说明 block 已提交，必须走
 * 完整重新入队。返回 1 表示 fast wake 完成，0 表示调用者继续 full wakeup。
 * 锁内还处理 delayed dequeue 与 proxy return，保证物理/逻辑在队状态一致。
 */
static int ttwu_runnable(struct task_struct *p, int wake_flags)
{
	ACQUIRE(__task_rq_lock, guard)(p);
	struct rq *rq = guard.rq;

	if (!task_on_rq_queued(p))
		return 0;

	update_rq_clock(rq);
	if (p->is_blocked) {
		if (p->se.sched_delayed)
			enqueue_task(rq, p, ENQUEUE_NOCLOCK | ENQUEUE_DELAYED);
		if (proxy_needs_return(rq, p))
			return 0;
	}
	if (!task_on_cpu(rq, p)) {
		/*
		 * When on_rq && !on_cpu the task is preempted, see if
		 * it should preempt the task that is current now.
		 */
		wakeup_preempt(rq, p, wake_flags);
	}
	ttwu_do_wakeup(p);
	return 1;
}

/*
 * sched_ttwu_pending() - 目标 CPU 批量消费 lockless remote wake_list。
 *
 * @arg 是一次性 llist 链表头，节点嵌在各 task；异步队列协议保证 task 存活。
 * 函数一次锁本 rq，逐 task 等待旧 on_cpu release、修正 CPU 归属并 activate。
 * 全部入队后才清 rq->ttwu_pending，避免 idle_cpu() 短暂误判为空而堆叠新任务。
 */
void sched_ttwu_pending(void *arg)
{
	struct llist_node *llist = arg;
	struct rq *rq = this_rq();
	struct task_struct *p, *t;
	struct rq_flags rf;

	if (!llist)
		return;

	rq_lock_irqsave(rq, &rf);
	update_rq_clock(rq);

	llist_for_each_entry_safe(p, t, llist, wake_entry.llist) {
		if (WARN_ON_ONCE(p->on_cpu))
			smp_cond_load_acquire(&p->on_cpu, !VAL);

		if (WARN_ON_ONCE(task_cpu(p) != cpu_of(rq)))
			set_task_cpu(p, cpu_of(rq));

		ttwu_do_activate(rq, p, p->sched_remote_wakeup ? WF_MIGRATED : 0, &rf);
	}

	/*
	 * Must be after enqueueing at least once task such that
	 * idle_cpu() does not observe a false-negative -- if it does,
	 * it is possible for select_idle_siblings() to stack a number
	 * of tasks on this CPU during that window.
	 *
	 * It is OK to clear ttwu_pending when another task pending.
	 * We will receive IPI after local IRQ enabled and then enqueue it.
	 * Since now nr_running > 0, idle_cpu() will always get correct result.
	 */
	WRITE_ONCE(rq->ttwu_pending, 0);
	rq_unlock_irqrestore(rq, &rf);
}

/*
 * Prepare the scene for sending an IPI for a remote smp_call
 *
 * Returns true if the caller can proceed with sending the IPI.
 * Returns false otherwise.
 */
/*
 * 原文：为远端 smp_call IPI 做准备。若目标 idle 正 polling，原子设置 resched 后
 * 返回 false，调用者无需发 IPI；否则返回 true 可继续发送。@cpu 是瞬时编号。
 */
bool call_function_single_prep_ipi(int cpu)
{
	if (set_nr_if_polling(cpu_rq(cpu)->idle)) {
		trace_sched_wake_idle_without_ipi(cpu);
		return false;
	}

	return true;
}

/*
 * Queue a task on the target CPUs wake_list and wake the CPU via IPI if
 * necessary. The wakee CPU on receipt of the IPI will queue the task
 * via sched_ttwu_wakeup() for activation so the wakee incurs the cost
 * of the wakeup instead of the waker.
 */
/*
 * 原文：把 @p 嵌入节点放入目标 @cpu wake_list，必要时 IPI；由 wakee 承担锁 rq、
 * 入队和统计成本，减少 waker 触碰远端 cacheline。异步提交后节点由目标 CPU 消费。
 */
static void __ttwu_queue_wakelist(struct task_struct *p, int cpu, int wake_flags)
{
	struct rq *rq = cpu_rq(cpu);

	p->sched_remote_wakeup = !!(wake_flags & WF_MIGRATED);

	WRITE_ONCE(rq->ttwu_pending, 1);
#ifdef CONFIG_SMP
	__smp_call_single_queue(cpu, &p->wake_entry.llist);
#endif
}

/*
 * wake_up_if_idle() 仅当 @cpu 两次检查都仍以 idle 为 curr 时请求重调度。第一次
 * RCU 检查避免争锁，第二次 rq 锁下复核消除切换竞态；非 idle 时无动作。
 */
void wake_up_if_idle(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	guard(rcu)();
	if (is_idle_task(rcu_dereference(rq->curr))) {
		guard(rq_lock_irqsave)(rq);
		if (is_idle_task(rq->curr))
			resched_curr(rq);
	}
}

/*
 * cpus_equal_capacity()/cpus_share_cache()/cpus_share_resources() 是 placement 的
 * 拓扑谓词。参数是 CPU 编号，返回瞬时 bool；它们不锁 hotplug 或预留资源。
 */
bool cpus_equal_capacity(int this_cpu, int that_cpu)
{
	if (!sched_asym_cpucap_active())
		return true;

	if (this_cpu == that_cpu)
		return true;

	return arch_scale_cpu_capacity(this_cpu) == arch_scale_cpu_capacity(that_cpu);
}

bool cpus_share_cache(int this_cpu, int that_cpu)
{
	if (this_cpu == that_cpu)
		return true;

	return per_cpu(sd_llc_id, this_cpu) == per_cpu(sd_llc_id, that_cpu);
}

/*
 * Whether CPUs are share cache resources, which means LLC on non-cluster
 * machines and LLC tag or L2 on machines with clusters.
 */
/*
 * 原文：share_resources 表示 CPU 共享调度相关 cache 资源；普通机器为 LLC，
 * cluster 机器可能是 LLC tag 或 L2。相同 CPU 恒为 true。
 */
bool cpus_share_resources(int this_cpu, int that_cpu)
{
	if (this_cpu == that_cpu)
		return true;

	return per_cpu(sd_share_id, this_cpu) == per_cpu(sd_share_id, that_cpu);
}

/*
 * ttwu_queue_cond() 判断是否值得把 @p 异步交给 @cpu：SCX/stop class 必须允许，
 * CPU 要 active、仍在 cpus_ptr；跨 LLC 优先 offload，同 LLC 仅远端 rq 无其他
 * runnable task 时使用。返回值只选择实现路径，不改变 task。
 */
static inline bool ttwu_queue_cond(struct task_struct *p, int cpu)
{
	int this_cpu = smp_processor_id();

	/* See SCX_OPS_ALLOW_QUEUED_WAKEUP. */
	if (!scx_allow_ttwu_queue(p))
		return false;

#ifdef CONFIG_SMP
	if (p->sched_class == &stop_sched_class)
		return false;
#endif

	/*
	 * Do not complicate things with the async wake_list while the CPU is
	 * in hotplug state.
	 */
	/* 中文：目标 CPU 处于 hotplug 过渡时不使用异步 wake_list，避免叠加生命周期竞态。 */
	if (!cpu_active(cpu))
		return false;

	/* Ensure the task will still be allowed to run on the CPU. */
	/* 中文：再次确认目标仍在 task cpus_ptr 中，关闭 affinity 更新竞态。 */
	if (!cpumask_test_cpu(cpu, p->cpus_ptr))
		return false;

	/*
	 * If the CPU does not share cache, then queue the task on the
	 * remote rqs wakelist to avoid accessing remote data.
	 */
	/* 中文：跨 cache 域时交给远端 wake_list，避免 waker 写远端 rq cacheline。 */
	if (!cpus_share_cache(this_cpu, cpu))
		return true;

	if (cpu == this_cpu)
		return false;

	/*
	 * If the wakee cpu is idle, or the task is descheduling and the
	 * only running task on the CPU, then use the wakelist to offload
	 * the task activation to the idle (or soon-to-be-idle) CPU as
	 * the current CPU is likely busy. nr_running is checked to
	 * avoid unnecessary task stacking.
	 *
	 * Note that we can only get here with (wakee) p->on_rq=0,
	 * p->on_cpu can be whatever, we've done the dequeue, so
	 * the wakee has been accounted out of ->nr_running.
	 */
	if (!cpu_rq(cpu)->nr_running)
		return true;

	return false;
}

/*
 * ttwu_queue_wakelist() 在 feature 与条件成立时同步两 CPU sched clock，再异步
 * 排队并返回 true；false 表示调用者应直接锁目标 rq 入队。
 */
static bool ttwu_queue_wakelist(struct task_struct *p, int cpu, int wake_flags)
{
	if (sched_feat(TTWU_QUEUE) && ttwu_queue_cond(p, cpu)) {
		sched_clock_cpu(cpu); /* Sync clocks across CPUs */
		__ttwu_queue_wakelist(p, cpu, wake_flags);
		return true;
	}

	return false;
}

/*
 * ttwu_queue() 是 full wakeup 的最终入队分派：优先 remote wake_list，否则直接
 * 锁 @cpu rq、更新 clock 并 activate。@p 已处 TASK_WAKING；返回时入队已完成
 * 或已可靠提交给目标 CPU。
 */
static void ttwu_queue(struct task_struct *p, int cpu, int wake_flags)
{
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;

	if (ttwu_queue_wakelist(p, cpu, wake_flags))
		return;

	rq_lock(rq, &rf);
	update_rq_clock(rq);
	ttwu_do_activate(rq, p, wake_flags, &rf);
	rq_unlock(rq, &rf);
}

/*
 * Invoked from try_to_wake_up() to check whether the task can be woken up.
 *
 * The caller holds p::pi_lock if p != current or has preemption
 * disabled when p == current.
 *
 * The rules of saved_state:
 *
 *   The related locking code always holds p::pi_lock when updating
 *   p::saved_state, which means the code is fully serialized in both cases.
 *
 *   For PREEMPT_RT, the lock wait and lock wakeups happen via TASK_RTLOCK_WAIT.
 *   No other bits set. This allows to distinguish all wakeup scenarios.
 *
 *   For FREEZER, the wakeup happens via TASK_FROZEN. No other bits set. This
 *   allows us to prevent early wakeup of tasks before they can be run on
 *   asymmetric ISA architectures (eg ARMv9).
 */
/*
 * 原文：该 helper 在 try_to_wake_up 中检查普通或 saved_state。非 current 调用者
 * 持 p->pi_lock，current 已禁抢占。PREEMPT_RT 只用 TASK_RTLOCK_WAIT，freezer
 * 只用 TASK_FROZEN，从而区分内部 lock/thaw wake 与真实 task wake。saved_state
 * 匹配时把它改为 RUNNING 并报告请求成功，但不实际入队；稍后恢复 __state 的路径
 * 会消费该结果，保证普通 wake 不丢失。@success 是输出布尔值。
 */
static __always_inline
bool ttwu_state_match(struct task_struct *p, unsigned int state, int *success)
{
	int match;

	if (IS_ENABLED(CONFIG_DEBUG_PREEMPT)) {
		WARN_ON_ONCE((state & TASK_RTLOCK_WAIT) &&
			     state != TASK_RTLOCK_WAIT);
	}

	*success = !!(match = __task_state_match(p, state));

	/*
	 * Saved state preserves the task state across blocking on
	 * an RT lock or TASK_FREEZABLE tasks.  If the state matches,
	 * set p::saved_state to TASK_RUNNING, but do not wake the task
	 * because it waits for a lock wakeup or __thaw_task(). Also
	 * indicate success because from the regular waker's point of
	 * view this has succeeded.
	 *
	 * After acquiring the lock the task will restore p::__state
	 * from p::saved_state which ensures that the regular
	 * wakeup is not lost. The restore will also set
	 * p::saved_state to TASK_RUNNING so any further tests will
	 * not result in false positives vs. @success
	 */
	/*
	 * 中文：saved_state 跨 RT lock 等待或 FREEZABLE 阻塞保存原 task 状态。若
	 * 匹配，只把 saved_state 改成 RUNNING，不直接唤醒仍等待锁/thaw 的 task；
	 * 同时向普通 waker 报告成功。取得锁后恢复路径会把 saved_state 写回 __state，
	 * 因而不会丢掉这次普通唤醒，并把 saved_state 复位避免后续假阳性。
	 */
	if (match < 0)
		p->saved_state = TASK_RUNNING;

	return match > 0;
}

/*
 * Notes on Program-Order guarantees on SMP systems.
 *
 *  MIGRATION
 *
 * The basic program-order guarantee on SMP systems is that when a task [t]
 * migrates, all its activity on its old CPU [c0] happens-before any subsequent
 * execution on its new CPU [c1].
 *
 * For migration (of runnable tasks) this is provided by the following means:
 *
 *  A) UNLOCK of the rq(c0)->lock scheduling out task t
 *  B) migration for t is required to synchronize *both* rq(c0)->lock and
 *     rq(c1)->lock (if not at the same time, then in that order).
 *  C) LOCK of the rq(c1)->lock scheduling in task
 *
 * Release/acquire chaining guarantees that B happens after A and C after B.
 * Note: the CPU doing B need not be c0 or c1
 *
 * Example:
 *
 *   CPU0            CPU1            CPU2
 *
 *   LOCK rq(0)->lock
 *   sched-out X
 *   sched-in Y
 *   UNLOCK rq(0)->lock
 *
 *                                   LOCK rq(0)->lock // orders against CPU0
 *                                   dequeue X
 *                                   UNLOCK rq(0)->lock
 *
 *                                   LOCK rq(1)->lock
 *                                   enqueue X
 *                                   UNLOCK rq(1)->lock
 *
 *                   LOCK rq(1)->lock // orders against CPU2
 *                   sched-out Z
 *                   sched-in X
 *                   UNLOCK rq(1)->lock
 *
 *
 *  BLOCKING -- aka. SLEEP + WAKEUP
 *
 * For blocking we (obviously) need to provide the same guarantee as for
 * migration. However the means are completely different as there is no lock
 * chain to provide order. Instead we do:
 *
 *   1) smp_store_release(X->on_cpu, 0)   -- finish_task()
 *   2) smp_cond_load_acquire(!X->on_cpu) -- try_to_wake_up()
 *
 * Example:
 *
 *   CPU0 (schedule)  CPU1 (try_to_wake_up) CPU2 (schedule)
 *
 *   LOCK rq(0)->lock LOCK X->pi_lock
 *   dequeue X
 *   sched-out X
 *   smp_store_release(X->on_cpu, 0);
 *
 *                    smp_cond_load_acquire(&X->on_cpu, !VAL);
 *                    X->state = WAKING
 *                    set_task_cpu(X,2)
 *
 *                    LOCK rq(2)->lock
 *                    enqueue X
 *                    X->state = RUNNING
 *                    UNLOCK rq(2)->lock
 *
 *                                          LOCK rq(2)->lock // orders against CPU1
 *                                          sched-out Z
 *                                          sched-in X
 *                                          UNLOCK rq(2)->lock
 *
 *                    UNLOCK X->pi_lock
 *   UNLOCK rq(0)->lock
 *
 *
 * However, for wakeups there is a second guarantee we must provide, namely we
 * must ensure that CONDITION=1 done by the caller can not be reordered with
 * accesses to the task state; see try_to_wake_up() and set_current_state().
 */
/*
 * 中文：SMP 上的程序顺序保证。
 *
 * 对 runnable task 迁移，旧 CPU c0 上的所有活动必须 happens-before 新 CPU c1
 * 的后续执行。旧 rq 解锁是 release；迁移者依次同步旧、新 rq 锁；新 CPU 再以
 * acquire 取得新 rq 锁。该 release/acquire 锁链把 sched-out、迁移出入队和
 * sched-in 严格排序，执行迁移的 CPU 本身不必是 c0/c1。
 *
 * 对 sleep+wakeup 没有可用的双 rq 锁链，因此改用 on_cpu 做直接交接：
 * finish_task() 以 release 写 0，try_to_wake_up() 以 acquire 等到 0；之后才写
 * WAKING、set_task_cpu 并在新 rq 入队。这样旧 CPU 的最后访问先于 waker，而
 * 新 rq 锁又把 waker 先于新 CPU sched-in。
 *
 * 唤醒还必须额外保证调用者的 CONDITION=1 不会与 task state 访问互相重排；
 * 该条件/状态握手由 try_to_wake_up() 与 set_current_state() 的完整屏障保证。
 */

/**
 * try_to_wake_up - wake up a thread
 * @p: the thread to be awakened
 * @state: the mask of task states that can be woken
 * @wake_flags: wake modifier flags (WF_*)
 *
 * Conceptually does:
 *
 *   If (@state & @p->state) @p->state = TASK_RUNNING.
 *
 * If the task was not queued/runnable, also place it back on a runqueue.
 *
 * This function is atomic against schedule() which would dequeue the task.
 *
 * It issues a full memory barrier before accessing @p->state, see the comment
 * with set_current_state().
 *
 * Uses p->pi_lock to serialize against concurrent wake-ups.
 *
 * Relies on p->pi_lock stabilizing:
 *  - p->sched_class
 *  - p->cpus_ptr
 *  - p->sched_task_group
 * in order to do migration, see its use of select_task_rq()/set_task_cpu().
 *
 * Tries really hard to only take one task_rq(p)->lock for performance.
 * Takes rq->lock in:
 *  - ttwu_runnable()    -- old rq, unavoidable, see comment there;
 *  - ttwu_queue()       -- new rq, for enqueue of the task;
 *  - psi_ttwu_dequeue() -- much sadness :-( accounting will kill us.
 *
 * As a consequence we race really badly with just about everything. See the
 * many memory barriers and their comments for details.
 *
 * Return: %true if @p->state changes (an actual wakeup was done),
 *	   %false otherwise.
 */
/*
 * 中文契约与阶段：
 *
 * @p 是调用者保证存活的目标 task；@state 是允许唤醒的状态位集合；@wake_flags
 * 描述同步唤醒、fork、迁移等上下文。函数可从普通进程/中断相关路径调用，内部
 * 关闭抢占并取得 p->pi_lock，不能睡眠。
 *
 * 1. 用完整屏障后读取 __state，与 set_current_state() 的“先写状态、后检查条件”
 *    配对，避免 waker 与 sleeper 都看不到对方更新。
 * 2. 若 task 已 runnable，只在旧 rq 锁下完成远端唤醒记账；否则等待 on_cpu 清零，
 *    acquire 读取与 finish_task() 的 release 写配对，保证旧 CPU 的执行全部结束。
 * 3. 发布 TASK_WAKING，选择合法目标 CPU，必要时 set_task_cpu()，再由 ttwu_queue()
 *    直接锁目标 rq 入队或加入远端 lockless wake_list。
 * 4. ttwu_do_wakeup() 最终发布 TASK_RUNNING，并按优先级设置目标 CPU resched。
 *
 * 返回 true 表示本次请求匹配了普通或 saved_state 的可唤醒状态；false 表示状态
 * 不匹配。true 不等于调用返回时 task 已经执行，异步 wake-list 可能尚待目标 CPU
 * 入队。函数不转移调用者的 task 引用。
 */
int try_to_wake_up(struct task_struct *p, unsigned int state, int wake_flags)
{
	guard(preempt)();
	int cpu, success = 0;

	wake_flags |= WF_TTWU;

	if (p == current) {
		/*
		 * We're waking current, this means 'p->on_rq' and 'task_cpu(p)
		 * == smp_processor_id()'. Together this means we can special
		 * case the whole 'p->on_rq && ttwu_runnable()' case below
		 * without taking any locks.
		 *
		 * Specifically, given current runs ttwu() we must be before
		 * schedule()'s block_task(), as such this must not observe
		 * sched_delayed.
		 *
		 * In particular:
		 *  - we rely on Program-Order guarantees for all the ordering,
		 *  - we're serialized against set_special_state() by virtue of
		 *    it disabling IRQs (this allows not taking ->pi_lock).
		 */
		WARN_ON_ONCE(p->se.sched_delayed);
		WARN_ON_ONCE(p->is_blocked);
		/* If p is current, we know we can run here, so clear blocked_on */
		/* 中文：p 就是 current，确定可在本 CPU 继续运行，因此清除 proxy blocked_on。 */
		clear_task_blocked_on(p, NULL);
		if (!ttwu_state_match(p, state, &success))
			goto out;

		trace_sched_waking(p);
		ttwu_do_wakeup(p);
		goto out;
	}

	/*
	 * If we are going to wake up a thread waiting for CONDITION we
	 * need to ensure that CONDITION=1 done by the caller can not be
	 * reordered with p->state check below. This pairs with smp_store_mb()
	 * in set_current_state() that the waiting thread does.
	 */
	/*
	 * 中文：waker 写 CONDITION=1 不能重排到读取 p->state 之后；取 pi_lock 后的
	 * 完整屏障与 waiter 的 set_current_state()/smp_store_mb() 配对，防止双方
	 * 同时错过“条件已真/状态已睡眠”。
	 */
	scoped_guard (raw_spinlock_irqsave, &p->pi_lock) {
		smp_mb__after_spinlock();

		if (!ttwu_state_match(p, state, &success))
			break;

		trace_sched_waking(p);

		/*
		 * Ensure we load p->on_rq _after_ p->state, otherwise it would
		 * be possible to, falsely, observe p->on_rq == 0 and get stuck
		 * in smp_cond_load_acquire() below.
		 *
		 * sched_ttwu_pending()			try_to_wake_up()
		 *   STORE p->on_rq = 1			  LOAD p->state
		 *   UNLOCK rq->lock
		 *
		 * __schedule() (switch to task 'p')
		 *   LOCK rq->lock			  smp_rmb();
		 *   smp_mb__after_spinlock();
		 *   UNLOCK rq->lock
		 *
		 * [task p]
		 *   STORE p->state = UNINTERRUPTIBLE	  LOAD p->on_rq
		 *
		 * Pairs with the LOCK+smp_mb__after_spinlock() on rq->lock in
		 * __schedule().  See the comment for smp_mb__after_spinlock().
		 *
		 * A similar smp_rmb() lives in __task_needs_rq_lock().
		 */
		/*
		 * 中文：先读 state、后读 on_rq。若反序，可能误见 on_rq=0 并在后续
		 * 等待 on_cpu 永不满足。该 rmb 与 __schedule 取得 rq 锁后的全屏障配对；
		 * __task_needs_rq_lock() 使用相同序列。
		 */
		smp_rmb();
		if (READ_ONCE(p->on_rq) && ttwu_runnable(p, wake_flags))
			break;

		/*
		 * Ensure we load p->on_cpu _after_ p->on_rq, otherwise it would be
		 * possible to, falsely, observe p->on_cpu == 0.
		 *
		 * One must be running (->on_cpu == 1) in order to remove oneself
		 * from the runqueue.
		 *
		 * __schedule() (switch to task 'p')	try_to_wake_up()
		 *   STORE p->on_cpu = 1		  LOAD p->on_rq
		 *   UNLOCK rq->lock
		 *
		 * __schedule() (put 'p' to sleep)
		 *   LOCK rq->lock			  smp_rmb();
		 *   smp_mb__after_spinlock();
		 *   STORE p->on_rq = 0			  LOAD p->on_cpu
		 *
		 * Pairs with the LOCK+smp_mb__after_spinlock() on rq->lock in
		 * __schedule().  See the comment for smp_mb__after_spinlock().
		 *
		 * Form a control-dep-acquire with p->on_rq == 0 above, to ensure
		 * schedule()'s block_task() has 'happened' and p will no longer
		 * care about it's own p->state. See the comment in __schedule().
		 */
		/*
		 * 中文：再保证 on_cpu 的读取晚于 on_rq。on_rq==0 的控制依赖加 acquire
		 * 证明 schedule 的 block_task 已完成，此后旧 task 不再依赖自己的 state，
		 * waker 才能安全发布 TASK_WAKING。
		 */
		smp_acquire__after_ctrl_dep();

		/*
		 * We're doing the wakeup (@success == 1), they did a dequeue (p->on_rq
		 * == 0), which means we need to do an enqueue, change p->state to
		 * TASK_WAKING such that we can unlock p->pi_lock before doing the
		 * enqueue, such as ttwu_queue_wakelist().
		 */
		/*
		 * 中文：状态已匹配且 sleeper 已出队，后续必须重新 enqueue。先写
		 * TASK_WAKING，便可在实际入队（甚至异步 wake_list）前释放 pi_lock，
		 * 同时阻止其他路径把它误当普通 blocked task。
		 */
		WRITE_ONCE(p->__state, TASK_WAKING);

		/*
		 * If the owning (remote) CPU is still in the middle of schedule() with
		 * this task as prev, considering queueing p on the remote CPUs wake_list
		 * which potentially sends an IPI instead of spinning on p->on_cpu to
		 * let the waker make forward progress. This is safe because IRQs are
		 * disabled and the IPI will deliver after on_cpu is cleared.
		 *
		 * Ensure we load task_cpu(p) after p->on_cpu:
		 *
		 * set_task_cpu(p, cpu);
		 *   STORE p->cpu = @cpu
		 * __schedule() (switch to task 'p')
		 *   LOCK rq->lock
		 *   smp_mb__after_spin_lock()		smp_cond_load_acquire(&p->on_cpu)
		 *   STORE p->on_cpu = 1		LOAD p->cpu
		 *
		 * to ensure we observe the correct CPU on which the task is currently
		 * scheduling.
		 */
		/*
		 * 中文：若远端 CPU 尚在以 p 为 prev 完成 schedule，优先把 p 放进其
		 * wake_list，避免 waker 自旋；IRQ 关闭保证 IPI 在 on_cpu 清零后处理。
		 * acquire 读取 on_cpu 还排序随后 task_cpu 读取，与迁移/切换侧屏障配对，
		 * 确保把请求送到真正仍在切换 p 的 CPU。
		 */
		if (smp_load_acquire(&p->on_cpu) &&
		    ttwu_queue_wakelist(p, task_cpu(p), wake_flags))
			break;

		/*
		 * If the owning (remote) CPU is still in the middle of schedule() with
		 * this task as prev, wait until it's done referencing the task.
		 *
		 * Pairs with the smp_store_release() in finish_task().
		 *
		 * This ensures that tasks getting woken will be fully ordered against
		 * their previous state and preserve Program Order.
		 */
		/*
		 * 中文：异步交付不可用时，等待远端 finish_task() release 清 on_cpu；
		 * acquire 配对确保旧 CPU 对 task 的最后访问全部先于本次重新入队，保持
		 * 前后两次执行的程序顺序。
		 */
		smp_cond_load_acquire(&p->on_cpu, !VAL);

		cpu = select_task_rq(p, p->wake_cpu, &wake_flags);
		if (task_cpu(p) != cpu) {
			if (p->in_iowait) {
				delayacct_blkio_end(p);
				atomic_dec(&task_rq(p)->nr_iowait);
			}

			wake_flags |= WF_MIGRATED;
			psi_ttwu_dequeue(p);
			set_task_cpu(p, cpu);
		} else if (cpu != p->wake_cpu) {
			/*
			 * If we were proxy-migrated to cpu, then
			 * select_task_rq() picks cpu instead of wake_cpu
			 * to return to, we won't call set_task_cpu(),
			 * leaving a stale wake_cpu pointing to where we
			 * proxy-migrated from. So just fixup wake_cpu here
			 * if its not correct
			 */
			/*
			 * 中文：proxy 临时迁移会保留旧 wake_cpu；若选核返回当前 CPU 而无需
			 * set_task_cpu()，必须单独修正 wake_cpu，避免留下指向 proxy 来源的
			 * 陈旧值。
			 */
			p->wake_cpu = cpu;
		}

		ttwu_queue(p, cpu, wake_flags);
	}
out:
	if (success)
		ttwu_stat(p, task_cpu(p), wake_flags);

	return success;
}

/*
 * __task_needs_rq_lock() 在已持 p->pi_lock 时判断稳定访问 @p 是否还需 rq 锁。
 * RUNNING/WAKING/on_rq 返回 true；blocked 且已完成 schedule 时，用 rmb 和 acquire
 * 等待 on_cpu 清零后返回 false，防止误读 on_rq=0 后仍碰到旧 CPU 正在切出的 task。
 */
static bool __task_needs_rq_lock(struct task_struct *p)
{
	unsigned int state = READ_ONCE(p->__state);

	/*
	 * Since pi->lock blocks try_to_wake_up(), we don't need rq->lock when
	 * the task is blocked. Make sure to check @state since ttwu() can drop
	 * locks at the end, see ttwu_queue_wakelist().
	 */
	/*
	 * 中文：pi_lock 阻挡新的 ttwu，blocked task 通常无需 rq 锁；但必须先检查
	 * RUNNING/WAKING，因为异步 wake-list 可在函数末尾先放锁、后完成入队。
	 */
	if (state == TASK_RUNNING || state == TASK_WAKING)
		return true;

	/*
	 * Ensure we load p->on_rq after p->__state, otherwise it would be
	 * possible to, falsely, observe p->on_rq == 0.
	 *
	 * See try_to_wake_up() for a longer comment.
	 */
	/* 中文：rmb 保证先读 state 后读 on_rq，避免把仍在队 task 误判为 blocked。 */
	smp_rmb();
	if (p->on_rq)
		return true;

	/*
	 * Ensure the task has finished __schedule() and will not be referenced
	 * anymore. Again, see try_to_wake_up() for a longer comment.
	 */
	/*
	 * 中文：确认 on_rq=0 后仍要 acquire 等待 on_cpu 清零，保证旧 CPU 已完成
	 * __schedule() 且不再引用 task，随后才可无 rq 锁调用回调。
	 */
	smp_rmb();
	smp_cond_load_acquire(&p->on_cpu, !VAL);

	return false;
}

/**
 * task_call_func - Invoke a function on task in fixed state
 * @p: Process for which the function is to be invoked, can be @current.
 * @func: Function to invoke.
 * @arg: Argument to function.
 *
 * Fix the task in it's current state by avoiding wakeups and or rq operations
 * and call @func(@arg) on it.  This function can use task_is_runnable() and
 * task_curr() to work out what the state is, if required.  Given that @func
 * can be invoked with a runqueue lock held, it had better be quite
 * lightweight.
 *
 * Returns:
 *   Whatever @func returns
 */
/*
 * 原文：把 @p 固定在 blocked/waking/queued/running 状态后调用 @func(p,arg)。
 * 始终持 p->pi_lock，需要时再持 rq 锁；回调可读 task_curr/on_rq/state 区分状态，
 * 但可能在 IRQ-off rq 锁内执行，必须轻量且不能睡眠。返回值原样来自回调，函数
 * 不改变 task ownership。
 */
int task_call_func(struct task_struct *p, task_call_f func, void *arg)
{
	struct rq_flags rf;
	int ret;

	raw_spin_lock_irqsave(&p->pi_lock, rf.flags);

	if (__task_needs_rq_lock(p)) {
		struct rq *rq = __task_rq_lock(p, &rf);

		/*
		 * At this point the task is pinned; either:
		 *  - blocked and we're holding off wakeups	 (pi->lock)
		 *  - woken, and we're holding off enqueue	 (rq->lock)
		 *  - queued, and we're holding off schedule	 (rq->lock)
		 *  - running, and we're holding off de-schedule (rq->lock)
		 *
		 * The called function (@func) can use: task_curr(), p->on_rq and
		 * p->__state to differentiate between these states.
		 */
		/*
		 * 中文：此时 task 状态已固定：blocked 由 pi_lock 阻止唤醒，woken 由
		 * rq 锁阻止入队，queued/running 也由 rq 锁阻止调度或迁移。
		 */
		ret = func(p, arg);

		__task_rq_unlock(rq, p, &rf);
	} else {
		ret = func(p, arg);
	}

	raw_spin_unlock_irqrestore(&p->pi_lock, rf.flags);
	return ret;
}

/**
 * cpu_curr_snapshot - Return a snapshot of the currently running task
 * @cpu: The CPU on which to snapshot the task.
 *
 * Returns the task_struct pointer of the task "currently" running on
 * the specified CPU.
 *
 * If the specified CPU was offline, the return value is whatever it
 * is, perhaps a pointer to the task_struct structure of that CPU's idle
 * task, but there is no guarantee.  Callers wishing a useful return
 * value must take some action to ensure that the specified CPU remains
 * online throughout.
 *
 * This function executes full memory barriers before and after fetching
 * the pointer, which permits the caller to confine this function's fetch
 * with respect to the caller's accesses to other shared variables.
 */
/*
 * 原文：返回 @cpu 当前 task 的带顺序快照。offline CPU 结果可能只是 idle 指针，
 * 调用者若要可靠值必须自行保持 CPU online。读取前后完整屏障的具体配对由调用者
 * 设计决定。返回裸 task 指针且不增加引用，只能在外层生命周期保护下即时使用。
 */
struct task_struct *cpu_curr_snapshot(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct task_struct *t;
	struct rq_flags rf;

	rq_lock_irqsave(rq, &rf);
	smp_mb__after_spinlock(); /* Pairing determined by caller's synchronization design. */
	t = rcu_dereference(cpu_curr(cpu));
	rq_unlock_irqrestore(rq, &rf);
	smp_mb(); /* Pairing determined by caller's synchronization design. */

	return t;
}

/**
 * wake_up_process - Wake up a specific process
 * @p: The process to be woken up.
 *
 * Attempt to wake up the nominated process and move it to the set of runnable
 * processes.
 *
 * Return: 1 if the process was woken up, 0 if it was already running.
 *
 * This function executes a full memory barrier before accessing the task state.
 */
/*
 * 原文：尝试唤醒 @p 并放入 runnable 集合；返回 1 表示实际匹配并执行唤醒，
 * 0 表示已运行或状态不匹配。访问 state 前的完整屏障与 sleeper 条件检查配对。
 * 调用者保证 p 生命周期，本 wrapper 不取得长期引用。
 */
int wake_up_process(struct task_struct *p)
{
	return try_to_wake_up(p, TASK_NORMAL, 0);
}
EXPORT_SYMBOL(wake_up_process);

/*
 * wake_up_state() 只唤醒 @state 位集合匹配的 @p；返回 try_to_wake_up 的 1/0。
 * @p 为调用期间借用，函数不等待它真正获得 CPU。
 */
int wake_up_state(struct task_struct *p, unsigned int state)
{
	return try_to_wake_up(p, state, 0);
}

/*
 * Perform scheduler related setup for a newly forked process p.
 * p is forked by current.
 *
 * __sched_fork() is basic setup which is also used by sched_init() to
 * initialize the boot CPU's idle task.
 */
/*
 * 原文：为 current fork 出的新 @p 做调度器基础初始化；同一 helper 也被 sched_init
 * 用于 boot CPU idle task。@clone_flags 是创建策略输入，@p 尚未发布，调用者独占。
 * 函数清 on_rq/on_cpu、fair/rt/dl/ext 运行时统计、PI/preempt notifier、NUMA 等
 * 派生状态，但保留稍后 sched_fork 决定的 policy/class。无返回值和资源失败。
 */
static void __sched_fork(u64 clone_flags, struct task_struct *p)
{
	p->on_rq			= 0;

	p->se.on_rq			= 0;
	p->se.exec_start		= 0;
	p->se.sum_exec_runtime		= 0;
	p->se.prev_sum_exec_runtime	= 0;
	p->se.nr_migrations		= 0;
	p->se.vruntime			= 0;
	p->se.vlag			= 0;
	p->se.rel_deadline		= 0;
	INIT_LIST_HEAD(&p->se.group_node);

	/* A delayed task cannot be in clone(). */
	/* 中文：clone 中的未发布 child 不可能继承 delayed-dequeue 状态。 */
	WARN_ON_ONCE(p->se.sched_delayed);
	WARN_ON_ONCE(p->is_blocked);

#ifdef CONFIG_FAIR_GROUP_SCHED
	p->se.cfs_rq			= NULL;
#ifdef CONFIG_CFS_BANDWIDTH
	init_cfs_throttle_work(p);
#endif
#endif

#ifdef CONFIG_SCHEDSTATS
	/* Even if schedstat is disabled, there should not be garbage */
	/* 中文：即使运行期关闭 schedstat，child 统计结构也必须清零，不能遗留父值。 */
	memset(&p->stats, 0, sizeof(p->stats));
#endif

	init_dl_entity(&p->dl);

	INIT_LIST_HEAD(&p->rt.run_list);
	p->rt.timeout		= 0;
	p->rt.time_slice	= sched_rr_timeslice;
	p->rt.on_rq		= 0;
	p->rt.on_list		= 0;

#ifdef CONFIG_SCHED_CLASS_EXT
	init_scx_entity(&p->scx);
#endif

#ifdef CONFIG_PREEMPT_NOTIFIERS
	INIT_HLIST_HEAD(&p->preempt_notifiers);
#endif

#ifdef CONFIG_COMPACTION
	p->capture_control = NULL;
#endif
	init_numa_balancing(clone_flags, p);
	p->wake_entry.u_flags = CSD_TYPE_TTWU;
	p->migration_pending = NULL;
	init_sched_mm(p);
}

DEFINE_STATIC_KEY_FALSE(sched_numa_balancing);

/*
 * sched_numa_balancing 静态键是自动 NUMA balancing 热路径总开关；关闭时 fault/
 * placement 检查可由编译器跳过。sysctl_numa_balancing_mode 保存 NORMAL、MEMORY_
 * TIERING 等策略位；控制面更新它们，不保护单个 task 的 NUMA 状态。
 */

#ifdef CONFIG_NUMA_BALANCING

int sysctl_numa_balancing_mode;

/*
 * __set_numabalancing_state()/set_numabalancing_state() 分别负责静态键和公开策略值。
 * @enabled 为 false 清模式并关热路径，为 true 设 NORMAL 并开静态键；无返回值，
 * 调用者在控制面串行上下文使用。
 */
static void __set_numabalancing_state(bool enabled)
{
	if (enabled)
		static_branch_enable(&sched_numa_balancing);
	else
		static_branch_disable(&sched_numa_balancing);
}

void set_numabalancing_state(bool enabled)
{
	if (enabled)
		sysctl_numa_balancing_mode = NUMA_BALANCING_NORMAL;
	else
		sysctl_numa_balancing_mode = NUMA_BALANCING_DISABLED;
	__set_numabalancing_state(enabled);
}

#ifdef CONFIG_PROC_SYSCTL
/*
 * reset_memory_tiering() 在首次开启 NUMA memory tiering 时重置每个 online node 的
 * promotion 阈值、候选基线与起始毫秒时间。pgdat 是永久 node 对象的借用指针；
 * 无返回值，后续采样从新基线计算而不混入旧模式历史。
 */
static void reset_memory_tiering(void)
{
	struct pglist_data *pgdat;

	for_each_online_pgdat(pgdat) {
		pgdat->nbp_threshold = 0;
		pgdat->nbp_th_nr_cand = node_page_state(pgdat, PGPROMOTE_CANDIDATE);
		pgdat->nbp_th_start = jiffies_to_msecs(jiffies);
	}
}

/*
 * sysctl_numa_balancing() - 读写 NUMA balancing 位图。写入要求 CAP_SYS_ADMIN，
 * proc helper 通过临时 state 校验 0..NUMA_BALANCING_MAX；首次打开 tiering 时先
 * reset node 统计，再发布 mode/静态键。返回 0 或权限/解析 errno，读取无副作用。
 */
static int sysctl_numa_balancing(const struct ctl_table *table, int write,
			  void *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	int err;
	int state = sysctl_numa_balancing_mode;

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = *table;
	t.data = &state;
	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;
	if (write) {
		if (!(sysctl_numa_balancing_mode & NUMA_BALANCING_MEMORY_TIERING) &&
		    (state & NUMA_BALANCING_MEMORY_TIERING))
			reset_memory_tiering();
		sysctl_numa_balancing_mode = state;
		__set_numabalancing_state(state);
	}
	return err;
}
#endif /* CONFIG_PROC_SYSCTL */
#endif /* CONFIG_NUMA_BALANCING */

#ifdef CONFIG_SCHEDSTATS

DEFINE_STATIC_KEY_FALSE(sched_schedstats);

/*
 * sched_schedstats 静态键控制高频调度统计；set/force/setup/sysctl 只改变这把热
 * 路径开关。它不清零已有计数，开关前后的数据可混合，消费者需按采集区间解释。
 */

/*
 * set_schedstats() 是内部布尔 setter；force_schedstat_enabled() 供 profiling
 * 依赖方强制开启并只在状态变化时提示。二者无返回值，静态键更新可执行 text
 * patch，不能从任意原子热路径频繁调用。
 */
static void set_schedstats(bool enabled)
{
	if (enabled)
		static_branch_enable(&sched_schedstats);
	else
		static_branch_disable(&sched_schedstats);
}

void force_schedstat_enabled(void)
{
	if (!schedstat_enabled()) {
		pr_info("kernel profiling enabled schedstats, disable via kernel.sched_schedstats.\n");
		static_branch_enable(&sched_schedstats);
	}
}

/*
 * setup_schedstats() 解析启动参数 schedstats=enable|disable。@str 是启动期借用
 * 文本；返回 1 表示成功消费，0 表示无效并警告。__init 代码启动后可回收。
 */
static int __init setup_schedstats(char *str)
{
	int ret = 0;
	if (!str)
		goto out;

	if (!strcmp(str, "enable")) {
		set_schedstats(true);
		ret = 1;
	} else if (!strcmp(str, "disable")) {
		set_schedstats(false);
		ret = 1;
	}
out:
	if (!ret)
		pr_warn("Unable to parse schedstats=\n");

	return ret;
}
__setup("schedstats=", setup_schedstats);

#ifdef CONFIG_PROC_SYSCTL
/*
 * sysctl_schedstats() 用临时 int 在 proc_dointvec_minmax 中读写 0/1；写入要求
 * CAP_SYS_ADMIN，成功后调用 set_schedstats。参数遵循 proc 借用/偏移契约，
 * 返回 0 或权限/解析 errno。
 */
static int sysctl_schedstats(const struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	int err;
	int state = static_branch_likely(&sched_schedstats);

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = *table;
	t.data = &state;
	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;
	if (write)
		set_schedstats(state);
	return err;
}
#endif /* CONFIG_PROC_SYSCTL */
#endif /* CONFIG_SCHEDSTATS */

#ifdef CONFIG_SYSCTL
static const struct ctl_table sched_core_sysctls[] = {
#ifdef CONFIG_SCHEDSTATS
	{
		.procname       = "sched_schedstats",
		.data           = NULL,
		.maxlen         = sizeof(unsigned int),
		.mode           = 0644,
		.proc_handler   = sysctl_schedstats,
		.extra1         = SYSCTL_ZERO,
		.extra2         = SYSCTL_ONE,
	},
#endif /* CONFIG_SCHEDSTATS */
#ifdef CONFIG_UCLAMP_TASK
	{
		.procname       = "sched_util_clamp_min",
		.data           = &sysctl_sched_uclamp_util_min,
		.maxlen         = sizeof(unsigned int),
		.mode           = 0644,
		.proc_handler   = sysctl_sched_uclamp_handler,
	},
	{
		.procname       = "sched_util_clamp_max",
		.data           = &sysctl_sched_uclamp_util_max,
		.maxlen         = sizeof(unsigned int),
		.mode           = 0644,
		.proc_handler   = sysctl_sched_uclamp_handler,
	},
	{
		.procname       = "sched_util_clamp_min_rt_default",
		.data           = &sysctl_sched_uclamp_util_min_rt_default,
		.maxlen         = sizeof(unsigned int),
		.mode           = 0644,
		.proc_handler   = sysctl_sched_uclamp_handler,
	},
#endif /* CONFIG_UCLAMP_TASK */
#ifdef CONFIG_NUMA_BALANCING
	{
		.procname	= "numa_balancing",
		.data		= NULL, /* filled in by handler */
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= sysctl_numa_balancing,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_FOUR,
	},
#endif /* CONFIG_NUMA_BALANCING */
};
/*
 * sched_core_sysctls 是启动后只读的控制表：把 schedstats、uclamp 与 NUMA 的
 * 用户 ABI 名称映射到各自 handler/范围。表本身不持 task/rq 引用。
 */
static int __init sched_core_sysctl_init(void)
{
	register_sysctl_init("kernel", sched_core_sysctls);
	return 0;
}
late_initcall(sched_core_sysctl_init);
#endif /* CONFIG_SYSCTL */

/*
 * fork()/clone()-time setup:
 */
/*
 * 原文：这里完成 fork/clone 时的调度器侧初始化。
 *
 * sched_fork() 的 @clone_flags 描述 clone 策略，@p 是 copy_process() 正在构造、
 * 尚未发布运行的新 task；调用者仍拥有其生命周期。函数把状态设为 TASK_NEW，
 * 防止 signal/wakeup 在初始化完成前入队；清除从 current 复制来的 PI boost，
 * 应用 reset-on-fork/uclamp/class 策略并初始化实体统计。
 *
 * 返回 0 时 p 仍不可运行，但调度字段已可供 sched_cgroup_fork() 和后续
 * wake_up_new_task() 使用；-EAGAIN 表示继承出的 DL 属性不允许直接 fork，
 * 调用者负责销毁尚未发布的 child。函数不睡眠，不把 p 引用转给调度器。
 */
int sched_fork(u64 clone_flags, struct task_struct *p)
{
	__sched_fork(clone_flags, p);
	/*
	 * We mark the process as NEW here. This guarantees that
	 * nobody will actually run it, and a signal or other external
	 * event cannot wake it up and insert it on the runqueue either.
	 */
	p->__state = TASK_NEW;

	/*
	 * Make sure we do not leak PI boosting priority to the child.
	 */
	/* 中文：child 必须继承父 task 的正常优先级，不能泄漏父 task 当前的 PI 提升。 */
	p->prio = current->normal_prio;

	uclamp_fork(p);

	/*
	 * Revert to default priority/policy on fork if requested.
	 */
	/* 中文：设置 reset-on-fork 时把 child 的策略与优先级恢复为默认非特权值。 */
	if (unlikely(p->sched_reset_on_fork)) {
		if (task_has_dl_policy(p) || task_has_rt_policy(p)) {
			p->policy = SCHED_NORMAL;
			p->static_prio = NICE_TO_PRIO(0);
			p->rt_priority = 0;
			p->timer_slack_ns = p->default_timer_slack_ns;
		} else if (PRIO_TO_NICE(p->static_prio) < 0)
			p->static_prio = NICE_TO_PRIO(0);

		p->prio = p->normal_prio = p->static_prio;
		set_load_weight(p, false);
		p->se.custom_slice = 0;
		p->se.slice = sysctl_sched_base_slice;

		/*
		 * We don't need the reset flag anymore after the fork. It has
		 * fulfilled its duty:
		 */
	/* 中文：reset-on-fork 已完成一次性职责，child 不应继续把该标志传给后代。 */
		p->sched_reset_on_fork = 0;
	}

	if (dl_prio(p->prio))
		return -EAGAIN;

	scx_pre_fork(p);

	if (rt_prio(p->prio)) {
		p->sched_class = &rt_sched_class;
#ifdef CONFIG_SCHED_CLASS_EXT
	} else if (task_should_scx(p->policy)) {
		p->sched_class = &ext_sched_class;
#endif
	} else {
		p->sched_class = &fair_sched_class;
	}

	init_entity_runnable_average(&p->se);


#ifdef CONFIG_SCHED_INFO
	if (likely(sched_info_on()))
		memset(&p->sched_info, 0, sizeof(p->sched_info));
#endif
	p->on_cpu = 0;
	init_task_preempt_count(p);
	plist_node_init(&p->pushable_tasks, MAX_PRIO);
	RB_CLEAR_NODE(&p->pushable_dl_tasks);

	return 0;
}

/*
 * sched_cgroup_fork() - 在 child 发布前绑定其初始 task_group、CPU 和 class fork。
 *
 * @p 是未进 pid hash 的新 task；@kargs 借出已稳定 cset。为遵守统一锁规则仍持
 * p->pi_lock，选择 cgroup/autogroup，使用 __set_task_cpu() 做首次 CPU 赋值，
 * 再调用 class->task_fork。返回 scx_fork 的 0/errno；失败由 copy_process 调用
 * sched_cancel_fork() 回滚，p ownership 始终在创建路径。
 */
int sched_cgroup_fork(struct task_struct *p, struct kernel_clone_args *kargs)
{
	unsigned long flags;

	/*
	 * Because we're not yet on the pid-hash, p->pi_lock isn't strictly
	 * required yet, but lockdep gets upset if rules are violated.
	 */
	raw_spin_lock_irqsave(&p->pi_lock, flags);
#ifdef CONFIG_CGROUP_SCHED
	if (1) {
		struct task_group *tg;
		tg = container_of(kargs->cset->subsys[cpu_cgrp_id],
				  struct task_group, css);
		tg = autogroup_task_group(p, tg);
		p->sched_task_group = tg;
	}
#endif
	/*
	 * We're setting the CPU for the first time, we don't migrate,
	 * so use __set_task_cpu().
	 */
	__set_task_cpu(p, smp_processor_id());
	if (p->sched_class->task_fork)
		p->sched_class->task_fork(p);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

	return scx_fork(p, kargs);
}

/*
 * sched_cancel_fork() 只撤销 sched_ext 在 sched_cgroup_fork 阶段取得的资源。
 * @p 尚未发布且由 fork 失败路径拥有；无返回值。
 */
void sched_cancel_fork(struct task_struct *p)
{
	scx_cancel_fork(p);
}

static void sched_mm_cid_fork(struct task_struct *t);

/*
 * sched_post_fork() 在基础 fork 成功后完成 MM CID、RT uclamp 默认和 sched_ext
 * 发布后处理。@p 仍由创建路径稳定持有且尚未首次 wake；无返回值。
 */
void sched_post_fork(struct task_struct *p)
{
	sched_mm_cid_fork(p);
	uclamp_post_fork(p);
	scx_post_fork(p);
}

/*
 * to_ratio() 把 @runtime/@period 转为 BW_SHIFT 定点带宽。RUNTIME_INF 返回满额
 * BW_UNIT，period=0 防御性返回 0，否则先左移再 64 位除法。两个参数单位必须相同；
 * 函数纯计算，无副作用。
 */
u64 to_ratio(u64 period, u64 runtime)
{
	if (runtime == RUNTIME_INF)
		return BW_UNIT;

	/*
	 * Doing this here saves a lot of checks in all
	 * the calling paths, and returning zero seems
	 * safe for them anyway.
	 */
	if (period == 0)
		return 0;

	return div64_u64(runtime << BW_SHIFT, period);
}

/*
 * wake_up_new_task - wake up a newly created task for the first time.
 *
 * This function will do some initial scheduler statistics housekeeping
 * that must be done for every newly created context, then puts the task
 * on the runqueue and wakes it.
 */
/*
 * 原文完整含义：这是新 task 的第一次唤醒，先完成每个新执行上下文必需的统计，
 * 再把它加入 runqueue 并使其可运行。
 *
 * @p 必须是 sched_fork() 成功后仍为 TASK_NEW、由创建路径稳定持有的 child。
 * 函数在 p->pi_lock 下先发布 TASK_RUNNING，然后重新做 fork CPU 选择：此前
 * cpus_ptr 可能改变、CPU 也可能 hotplug 消失。由于 child 尚未完整发布，使用
 * __set_task_cpu() 而不调用普通 migrate hook。随后锁目标 rq、activate_task()、
 * wakeup_preempt() 并触发 sched_process_fork 相关记账。
 *
 * 返回：无直接返回值。成功后 child 已在某个 rq 可运行，并可能在本函数解锁后
 * 立刻由另一 CPU 执行；调用者不能再假设对 child 独占。无引用 ownership 转移，
 * 但“不可运行→并发可运行”是不可回滚的发布边界。
 */
void wake_up_new_task(struct task_struct *p)
{
	struct rq_flags rf;
	struct rq *rq;
	int wake_flags = WF_FORK;

	raw_spin_lock_irqsave(&p->pi_lock, rf.flags);
	WRITE_ONCE(p->__state, TASK_RUNNING);
	/*
	 * Fork balancing, do it here and not earlier because:
	 *  - cpus_ptr can change in the fork path
	 *  - any previously selected CPU might disappear through hotplug
	 *
	 * Use __set_task_cpu() to avoid calling sched_class::migrate_task_rq,
	 * as we're not fully set-up yet.
	 */
	p->recent_used_cpu = task_cpu(p);
	__set_task_cpu(p, select_task_rq(p, task_cpu(p), &wake_flags));
	rq = __task_rq_lock(p, &rf);
	update_rq_clock(rq);
	post_init_entity_util_avg(p);

	activate_task(rq, p, ENQUEUE_NOCLOCK | ENQUEUE_INITIAL);
	trace_sched_wakeup_new(p);
	wakeup_preempt(rq, p, wake_flags);
	if (p->sched_class->task_woken) {
		/*
		 * Nothing relies on rq->lock after this, so it's fine to
		 * drop it.
		 */
	/* 中文：后续不再依赖 rq 锁保护的状态，可临时 unpin 调用 class task_woken。 */
		rq_unpin_lock(rq, &rf);
		p->sched_class->task_woken(rq, p);
		rq_repin_lock(rq, &rf);
	}
	task_rq_unlock(rq, p, &rf);
}

#ifdef CONFIG_PREEMPT_NOTIFIERS

static DEFINE_STATIC_KEY_FALSE(preempt_notifier_key);

/*
 * preempt_notifier_key 的引用由使用该 API 的子系统成对 inc/dec；非零时 context
 * switch 热路径才遍历 current 的 notifier hlist。引用控制功能开关，不是每个
 * notifier 对象的生命周期引用。
 */

/*
 * preempt_notifier_inc()/dec() 调整全局静态键引用。调用者必须成对使用，并在
 * register 前先 inc、最后一个 unregister 后再 dec；无直接返回值。
 */
void preempt_notifier_inc(void)
{
	static_branch_inc(&preempt_notifier_key);
}
EXPORT_SYMBOL_GPL(preempt_notifier_inc);

void preempt_notifier_dec(void)
{
	static_branch_dec(&preempt_notifier_key);
}
EXPORT_SYMBOL_GPL(preempt_notifier_dec);

/**
 * preempt_notifier_register - tell me when current is being preempted & rescheduled
 * @notifier: notifier struct to register
 */
/*
 * 原文：把调用者拥有的 @notifier 嵌入 current 链表，以接收被抢占/重新调入通知。
 * notifier 对象必须活到 unregister，register 不分配也不取引用；静态键未启用
 * 仍注册会 WARN。接口针对 current，无跨 task 安装。
 */
void preempt_notifier_register(struct preempt_notifier *notifier)
{
	if (!static_branch_unlikely(&preempt_notifier_key))
		WARN(1, "registering preempt_notifier while notifiers disabled\n");

	hlist_add_head(&notifier->link, &current->preempt_notifiers);
}
EXPORT_SYMBOL_GPL(preempt_notifier_register);

/**
 * preempt_notifier_unregister - no longer interested in preemption notifications
 * @notifier: notifier struct to unregister
 *
 * This is *not* safe to call from within a preemption notifier.
 */
/*
 * 原文：删除 @notifier 后不再接收通知；不能在 notifier 回调内部调用，因为遍历
 * 正使用当前节点且没有支持自删除的安全迭代协议。无返回值，删除后 ownership
 * 完全回到调用者。
 */
void preempt_notifier_unregister(struct preempt_notifier *notifier)
{
	hlist_del(&notifier->link);
}
EXPORT_SYMBOL_GPL(preempt_notifier_unregister);

/*
 * __fire_sched_in/out_* 在 rq 锁/context-switch 协议稳定 @curr 链表时同步遍历
 * 回调；sched_in 传当前 CPU，sched_out 传借用 @next。外层 inline 用静态键消除
 * 未启用成本。回调不能睡眠或修改当前遍历链表。
 */
static void __fire_sched_in_preempt_notifiers(struct task_struct *curr)
{
	struct preempt_notifier *notifier;

	hlist_for_each_entry(notifier, &curr->preempt_notifiers, link)
		notifier->ops->sched_in(notifier, raw_smp_processor_id());
}

static __always_inline void fire_sched_in_preempt_notifiers(struct task_struct *curr)
{
	if (static_branch_unlikely(&preempt_notifier_key))
		__fire_sched_in_preempt_notifiers(curr);
}

/* __fire_sched_out_preempt_notifiers() 遍历 curr 链并把 next 借给各 sched_out 回调。 */
static void
__fire_sched_out_preempt_notifiers(struct task_struct *curr,
				   struct task_struct *next)
{
	struct preempt_notifier *notifier;

	hlist_for_each_entry(notifier, &curr->preempt_notifiers, link)
		notifier->ops->sched_out(notifier, next);
}

/* fire_sched_out_preempt_notifiers() 用静态键消除未启用 notifier 的切换成本。 */
static __always_inline void
fire_sched_out_preempt_notifiers(struct task_struct *curr,
				 struct task_struct *next)
{
	if (static_branch_unlikely(&preempt_notifier_key))
		__fire_sched_out_preempt_notifiers(curr, next);
}

#else /* !CONFIG_PREEMPT_NOTIFIERS: */

/*
 * 未编译 notifier 时 sched_in/out stub 保持 context-switch 调用点统一，无副作用。
 */

static inline void fire_sched_in_preempt_notifiers(struct task_struct *curr)
{
}

static inline void
fire_sched_out_preempt_notifiers(struct task_struct *curr,
				 struct task_struct *next)
{
}

#endif /* !CONFIG_PREEMPT_NOTIFIERS */

/*
 * prepare_task()/finish_task() 在 context switch 两侧维护 on_cpu 发布协议。
 * prepare 在真正切入前写 1；finish 必须在旧 CPU 对 prev 的最后一次访问之后用
 * release 写 0，与 try_to_wake_up() 的 acquire 等待配对。它保证旧 CPU 活动先于
 * 新 CPU 迁移/执行，不等同于 task 引用计数。
 */
static inline void prepare_task(struct task_struct *next)
{
	/*
	 * Claim the task as running, we do this before switching to it
	 * such that any running task will have this set.
	 *
	 * See the smp_load_acquire(&p->on_cpu) case in ttwu() and
	 * its ordering comment.
	 */
	WRITE_ONCE(next->on_cpu, 1);
}

static inline void finish_task(struct task_struct *prev)
{
	/*
	 * This must be the very last reference to @prev from this CPU. After
	 * p->on_cpu is cleared, the task can be moved to a different CPU. We
	 * must ensure this doesn't happen until the switch is completely
	 * finished.
	 *
	 * In particular, the load of prev->state in finish_task_switch() must
	 * happen before this.
	 *
	 * Pairs with the smp_cond_load_acquire() in try_to_wake_up().
	 */
	/*
	 * 中文：这是本 CPU 对 prev 的最后访问。release 清 on_cpu 后，task 可立即
	 * 被迁到其他 CPU；因此必须晚于完整 switch 以及 finish_task_switch 对 state
	 * 的读取，并与 ttwu 的 acquire 等待配对。
	 */
	smp_store_release(&prev->on_cpu, 0);
}

/*
 * Only called from __schedule context
 *
 * There are some cases where we are going to re-do the action
 * that added the balance callbacks. We may not be in a state
 * where we can run them, so just zap them so they can be
 * properly re-added on the next time around. This is similar
 * handling to running the callbacks, except we just don't call
 * them.
 */
/*
 * 原文：仅从 __schedule 调用。当本轮动作会被重做、当前状态又不适合执行 callback
 * 时，摘下普通 callback 并清 next，保留特殊 balance_push_callback 以免 CPU
 * hotplug 排空请求丢失。@rq 已锁；无回调执行、无动态对象 ownership。
 */
static void zap_balance_callbacks(struct rq *rq)
{
	struct balance_callback *next, *head;
	bool found = false;

	lockdep_assert_rq_held(rq);

	head = rq->balance_callback;
	while (head) {
		if (head == &balance_push_callback)
			found = true;
		next = head->next;
		head->next = NULL;
		head = next;
	}
	rq->balance_callback = found ? &balance_push_callback : NULL;
}

/*
 * do_balance_callbacks() 在已锁 @rq 上消费调用者交入的单链表：先摘节点并清 next，
 * 再调用 func(rq)，使 callback 可安全重新排队。@head 节点通常嵌在长期对象中，
 * 本函数不释放内存；无返回值。
 */
static void do_balance_callbacks(struct rq *rq, struct balance_callback *head)
{
	void (*func)(struct rq *rq);
	struct balance_callback *next;

	lockdep_assert_rq_held(rq);

	while (head) {
		func = (void (*)(struct rq *))head->func;
		next = head->next;
		head->next = NULL;
		head = next;

		func(rq);
	}
}

static void balance_push(struct rq *rq);

/*
 * balance_push_callback is a right abuse of the callback interface and plays
 * by significantly different rules.
 *
 * Where the normal balance_callback's purpose is to be ran in the same context
 * that queued it (only later, when it's safe to drop rq->lock again),
 * balance_push_callback is specifically targeted at __schedule().
 *
 * This abuse is tolerated because it places all the unlikely/odd cases behind
 * a single test, namely: rq->balance_callback == NULL.
 */
/*
 * 原文：balance_push_callback 是 callback 接口的特殊用法。普通 callback 延迟到
 * 同一上下文可安全放 rq 锁时运行；push 专供 __schedule 的 CPU-hotplug 排空。
 * 允许这种例外是为了把罕见分支都压在 rq->balance_callback 非空的一次测试后。
 * 对象全局永久存在，不能按普通一次性节点释放。
 */
struct balance_callback balance_push_callback = {
	.next = NULL,
	.func = balance_push,
};

/*
 * __splice_balance_callbacks() 在 rq 锁下移交 callback 链。split 模式若只有特殊
 * balance_push 节点则将其留在 rq，避免解锁窗口中 __schedule 误认为无需排空。
 */
static inline struct balance_callback *
__splice_balance_callbacks(struct rq *rq, bool split)
{
	struct balance_callback *head = rq->balance_callback;

	if (likely(!head))
		return NULL;

	lockdep_assert_rq_held(rq);
	/*
	 * Must not take balance_push_callback off the list when
	 * splice_balance_callbacks() and balance_callbacks() are not
	 * in the same rq->lock section.
	 *
	 * In that case it would be possible for __schedule() to interleave
	 * and observe the list empty.
	 */
	/*
	 * 中文：splice 与执行不在同一 rq 锁区间时不能摘掉 balance_push，否则
	 * __schedule 可能插入并观察到空链，丢失 CPU-hotplug 排空请求。
	 */
	if (split && head == &balance_push_callback)
		head = NULL;
	else
		rq->balance_callback = NULL;

	return head;
}

/*
 * __splice_balance_callbacks() 在 rq 锁下把 callback 链 ownership 移给调用者。
 * @split=true 且链头仅为 balance_push 时必须保留在 rq，避免 splice 与稍后执行
 * 不在同一锁区间时 __schedule 误见空链。返回 NULL 或待消费链头。
 */

struct balance_callback *splice_balance_callbacks(struct rq *rq)
{
	return __splice_balance_callbacks(rq, true);
}

/*
 * __balance_callbacks() 在仍持 @rq 锁时摘链并执行；@rf 非空时临时 unpin/repin
 * lockdep pin 状态，让 callback 内嵌套锁模型正确。balance_callbacks() 则接收已
 * splice 的链，重新锁 rq 后消费。两者均不释放 callback 对象。
 */
void __balance_callbacks(struct rq *rq, struct rq_flags *rf)
{
	if (rf)
		rq_unpin_lock(rq, rf);
	do_balance_callbacks(rq, __splice_balance_callbacks(rq, false));
	if (rf)
		rq_repin_lock(rq, rf);
}

void balance_callbacks(struct rq *rq, struct balance_callback *head)
{
	unsigned long flags;

	if (unlikely(head)) {
		raw_spin_rq_lock_irqsave(rq, flags);
		do_balance_callbacks(rq, head);
		raw_spin_rq_unlock_irqrestore(rq, flags);
	}
}

/*
 * prepare_lock_switch() 在 prev 栈上为 rq 锁跨 context switch 交接修正 lockdep：
 * 物理锁仍持有，但先 unpin/release，并在调试配置下把 owner 改为 next。
 */
static inline void
prepare_lock_switch(struct rq *rq, struct task_struct *next, struct rq_flags *rf)
	__releases(__rq_lockp(rq))
	__acquires(__rq_lockp(this_rq()))
{
	/*
	 * Since the runqueue lock will be released by the next
	 * task (which is an invalid locking op but in the case
	 * of the scheduler it's an obvious special-case), so we
	 * do an early lockdep release here:
	 */
	/*
	 * 中文：rq 锁将由 next task 释放；普通锁语义不允许跨 task 解锁，因此这里
	 * 提前告诉 lockdep 已释放，物理锁仍连续保护切换。
	 */
	rq_unpin_lock(rq, rf);
	spin_release(&__rq_lockp(rq)->dep_map, _THIS_IP_);
#ifdef CONFIG_DEBUG_SPINLOCK
	/* this is a valid case when another task releases the spinlock */
	rq_lockp(rq)->owner = next;
#endif
	/*
	 * Model the rq reference switcheroo.
	 */
	__release(__rq_lockp(rq));
	__acquire(__rq_lockp(this_rq()));
}

/*
 * prepare_lock_switch()/finish_lock_switch() 为 rq 锁跨 task/栈交接修正 lockdep。
 * prepare 在 prev 栈上提前 unpin 并把 debug owner 指向 @next；finish 在 next 栈
 * 上重新声明 acquire、执行 balance/hrtick 收尾并真正 unlock_irq。物理锁始终未
 * 在中间释放，所以 curr/rq 状态连续受保护。
 */

static inline void finish_lock_switch(struct rq *rq)
	__releases(__rq_lockp(rq))
{
	/*
	 * If we are tracking spinlock dependencies then we have to
	 * fix up the runqueue lock - which gets 'carried over' from
	 * prev into current:
	 */
	spin_acquire(&__rq_lockp(rq)->dep_map, 0, 0, _THIS_IP_);
	__balance_callbacks(rq, NULL);
	hrtick_schedule_exit(rq);
	raw_spin_rq_unlock_irq(rq);
}

/*
 * NOP if the arch has not defined these:
 */
/*
 * 原文：架构未提供 switch hook 时定义为空操作，保持通用 context-switch 调用点
 * 一致。宏参数不求值，不产生副作用。
 */

#ifndef prepare_arch_switch
# define prepare_arch_switch(next)	do { } while (0)
#endif

#ifndef finish_arch_post_lock_switch
# define finish_arch_post_lock_switch()	do { } while (0)
#endif

/*
 * kmap_local_sched_out()/in() 仅在 CONFIG_KMAP_LOCAL 且 current 有嵌套 local map
 * 时保存/恢复 CPU-local 映射状态，防止 task 切换后另一个 task 继承临时映射。
 * 无返回值，具体页表操作由 kmap 子系统完成。
 */
static inline void kmap_local_sched_out(void)
{
#ifdef CONFIG_KMAP_LOCAL
	if (unlikely(current->kmap_ctrl.idx))
		__kmap_local_sched_out();
#endif
}

static inline void kmap_local_sched_in(void)
{
#ifdef CONFIG_KMAP_LOCAL
	if (unlikely(current->kmap_ctrl.idx))
		__kmap_local_sched_in();
#endif
}

/**
 * prepare_task_switch - prepare to switch tasks
 * @rq: the runqueue preparing to switch
 * @prev: the current task that is being switched out
 * @next: the task we are going to switch to.
 *
 * This is called with the rq lock held and interrupts off. It must
 * be paired with a subsequent finish_task_switch after the context
 * switch.
 *
 * prepare_task_switch sets up locking and calls architecture specific
 * hooks.
 */
/*
 * 原文：@rq 已锁且 IRQ 关闭，@prev 是将切出的 current，@next 是将切入的 task。
 * 必须与 switch 后的 finish_task_switch() 配对。函数依次通知 KCOV、sched_info、
 * perf、preempt notifier，保存 local kmap，发布 next->on_cpu，并执行架构预切换
 * hook。无返回值；此时寄存器/栈尚未切换，失败不允许中断该序列。
 */
static inline void
prepare_task_switch(struct rq *rq, struct task_struct *prev,
		    struct task_struct *next)
	__must_hold(__rq_lockp(rq))
{
	kcov_prepare_switch(prev);
	sched_info_switch(rq, prev, next);
	perf_event_task_sched_out(prev, next);
	fire_sched_out_preempt_notifiers(prev, next);
	kmap_local_sched_out();
	prepare_task(next);
	prepare_arch_switch(next);
}

/**
 * finish_task_switch - clean up after a task-switch
 * @prev: the thread we just switched away from.
 *
 * finish_task_switch must be called after the context switch, paired
 * with a prepare_task_switch call before the context switch.
 * finish_task_switch will reconcile locking set up by prepare_task_switch,
 * and do any other architecture-specific cleanup actions.
 *
 * Note that we may have delayed dropping an mm in context_switch(). If
 * so, we finish that here outside of the runqueue lock. (Doing it
 * with the lock held can cause deadlocks; see schedule() for
 * details.)
 *
 * The context switch have flipped the stack from under us and restored the
 * local variables which were saved when this task called schedule() in the
 * past. 'prev == current' is still correct but we need to recalculate this_rq
 * because prev may have moved to another CPU.
 */
/*
 * 中文补充：@prev 是刚被切出的 task，借用指针由 context-switch 协议保证在本
 * 函数完成前有效。函数实际运行在 next 的栈上，必须重新读取 this_rq()，不能用
 * prev 切换前保存的 CPU 假设。
 *
 * 主要阶段是：修复/校验跨栈继承的 preempt_count；取走 rq->prev_mm；读取 prev
 * 退出状态后以 release 清 on_cpu；完成架构、perf、kmap、preempt notifier；
 * 释放跨 switch 传递的 rq 锁；最后在锁外 mmdrop 延迟 mm，若 prev 为 TASK_DEAD
 * 则释放其“作为 current”的最后引用。返回当前 rq 借用指针。锁外释放 mm 是为
 * 避免页表销毁路径反向获取调度相关锁造成死锁。
 */
static struct rq *finish_task_switch(struct task_struct *prev)
	__releases(__rq_lockp(this_rq()))
{
	struct rq *rq = this_rq();
	struct mm_struct *mm = rq->prev_mm;
	unsigned int prev_state;

	/*
	 * The previous task will have left us with a preempt_count of 2
	 * because it left us after:
	 *
	 *	schedule()
	 *	  preempt_disable();			// 1
	 *	  __schedule()
	 *	    raw_spin_lock_irq(&rq->lock)	// 2
	 *
	 * Also, see FORK_PREEMPT_COUNT.
	 */
	if (WARN_ONCE(preempt_count() != 2*PREEMPT_DISABLE_OFFSET,
		      "corrupted preempt_count: %s/%d/0x%x\n",
		      current->comm, current->pid, preempt_count()))
		preempt_count_set(FORK_PREEMPT_COUNT);

	rq->prev_mm = NULL;

	/*
	 * A task struct has one reference for the use as "current".
	 * If a task dies, then it sets TASK_DEAD in tsk->state and calls
	 * schedule one last time. The schedule call will never return, and
	 * the scheduled task must drop that reference.
	 *
	 * We must observe prev->state before clearing prev->on_cpu (in
	 * finish_task), otherwise a concurrent wakeup can get prev
	 * running on another CPU and we could rave with its RUNNING -> DEAD
	 * transition, resulting in a double drop.
	 */
	/*
	 * 中文：task_struct 有一份“作为 current”的引用。TASK_DEAD task 最后一次
	 * schedule 不再返回，切入者必须释放该引用。必须先读取 prev state、再以
	 * release 清 on_cpu；否则并发唤醒可让 prev 在另一 CPU 运行并与
	 * RUNNING->DEAD 竞争，导致重复 put。
	 */
	prev_state = READ_ONCE(prev->__state);
	vtime_task_switch(prev);
	perf_event_task_sched_in(prev, current);
	finish_task(prev);
	tick_nohz_task_switch();
	finish_lock_switch(rq);
	finish_arch_post_lock_switch();
	kcov_finish_switch(current);
	/*
	 * kmap_local_sched_out() is invoked with rq::lock held and
	 * interrupts disabled. There is no requirement for that, but the
	 * sched out code does not have an interrupt enabled section.
	 * Restoring the maps on sched in does not require interrupts being
	 * disabled either.
	 */
	/*
	 * 中文：sched-out 因调用位置自然处在 rq 锁和 IRQ-off 下，kmap 本身不要求；
	 * sched-in 恢复 local map 同样无需保持 IRQ 关闭。
	 */
	kmap_local_sched_in();

	/*
	 * Any cached block-layer timestamp (plug->cur_ktime) is stale now,
	 * invalidate it.
	 */
	/* 中文：跨 task switch 后 block plug 缓存时间戳已陈旧，必须作废。 */
	blk_plug_invalidate_ts();

	fire_sched_in_preempt_notifiers(current);
	/*
	 * When switching through a kernel thread, the loop in
	 * membarrier_{private,global}_expedited() may have observed that
	 * kernel thread and not issued an IPI. It is therefore possible to
	 * schedule between user->kernel->user threads without passing though
	 * switch_mm(). Membarrier requires a barrier after storing to
	 * rq->curr, before returning to userspace, so provide them here:
	 *
	 * - a full memory barrier for {PRIVATE,GLOBAL}_EXPEDITED, implicitly
	 *   provided by mmdrop_lazy_tlb(),
	 * - a sync_core for SYNC_CORE.
	 */
	/*
	 * 中文：user->kthread->user 可能不经过 switch_mm，expedited membarrier 也可能
	 * 因观察到 kthread 而不发 IPI。返回用户态前仍需在 rq->curr 发布后提供屏障：
	 * mmdrop_lazy_tlb() 隐含 full barrier，并为 SYNC_CORE 执行 core 同步。
	 */
	if (mm) {
		membarrier_mm_sync_core_before_usermode(mm);
		mmdrop_lazy_tlb_sched(mm);
	}

	if (unlikely(prev_state == TASK_DEAD)) {
		if (prev->sched_class->task_dead)
			prev->sched_class->task_dead(prev);

		/*
		 * sched_ext_dead() must come before cgroup_task_dead() to
		 * prevent cgroups from being removed while its member tasks are
		 * visible to SCX schedulers.
		 */
	/*
	 * 中文：必须先通知 sched_ext task 已死亡，再让 cgroup 移除成员；否则 SCX
	 * 仍可见 task 时其 cgroup 已被销毁。
	 */
		sched_ext_dead(prev);
		cgroup_task_dead(prev);

		/* Task is done with its stack. */
		/* 中文：所有切换后收尾已结束，可释放 prev 的内核栈引用。 */
		put_task_stack(prev);

		put_task_struct_rcu_user(prev);
	}

	return rq;
}

/**
 * schedule_tail - first thing a freshly forked thread must call.
 * @prev: the thread we just switched away from.
 */
/*
 * 原文：新 fork 线程第一次获得 CPU 后执行的第一段调度器代码。@prev 是被它切出
 * 的 task；函数在 child 新栈上接管并释放 rq 锁，修正 FORK_PREEMPT_COUNT，然后
 * 开抢占、写 set_child_tid 并重算 pending signal。无直接返回值；完成后 child
 * 才进入正常返回 fork 的执行环境。
 */
asmlinkage __visible void schedule_tail(struct task_struct *prev)
	__releases(__rq_lockp(this_rq()))
{
	/*
	 * New tasks start with FORK_PREEMPT_COUNT, see there and
	 * finish_task_switch() for details.
	 *
	 * finish_task_switch() will drop rq->lock() and lower preempt_count
	 * and the preempt_enable() will end up enabling preemption (on
	 * PREEMPT_COUNT kernels).
	 */

	finish_task_switch(prev);
	/*
	 * This is a special case: the newly created task has just
	 * switched the context for the first time. It is returning from
	 * schedule for the first time in this path.
	 */
	/* 中文：新 child 首次完成 context switch，第一次从其历史 schedule 点返回。 */
	trace_sched_exit_tp(true);
	preempt_enable();

	if (current->set_child_tid)
		put_user(task_pid_vnr(current), current->set_child_tid);

	calculate_sigpending();
}

/*
 * context_switch - switch to the new MM and the new thread's register state.
 */
/*
 * 原文：切换到 next 的地址空间和线程寄存器状态。
 *
 * @rq 已锁且 IRQ 关闭；@prev 是当前 task，@next 是 pick 完成的借用 task；
 * @rf 携带 rq 锁 pin 状态。函数先 prepare_task_switch() 发布 next->on_cpu 并调用
 * 各观察 hook；随后按 user↔kernel 四种组合转移 active_mm：内核线程借用前一
 * user mm 并持 lazy-tlb 引用，切回用户 task 时把待释放 mm 放到 rq->prev_mm。
 * switch_to() 真正切寄存器、栈和 current，返回已经运行在 next 的历史调用栈上；
 * finish_task_switch() 将释放跨切换携带的 rq 锁和延迟 mm/task 引用。
 *
 * 返回当前 CPU 的 rq 借用指针。函数不允许失败；一旦进入 switch_to 就跨过本轮
 * 调度提交点。membarrier 的屏障必须覆盖 rq->curr/mm 切换到返回用户态之间。
 */
static __always_inline struct rq *
context_switch(struct rq *rq, struct task_struct *prev,
	       struct task_struct *next, struct rq_flags *rf)
	__releases(__rq_lockp(rq))
{
	prepare_task_switch(rq, prev, next);

	/*
	 * For paravirt, this is coupled with an exit in switch_to to
	 * combine the page table reload and the switch backend into
	 * one hypercall.
	 */
	/* 中文：paravirt 将这里的 enter 与 switch_to 中 exit 合并为一次 hypercall。 */
	arch_start_context_switch(prev);

	/*
	 * kernel -> kernel   lazy + transfer active
	 *   user -> kernel   lazy + mmgrab_lazy_tlb() active
	 *
	 * kernel ->   user   switch + mmdrop_lazy_tlb() active
	 *   user ->   user   switch
	 */
	/*
	 * 中文：切入 kthread 时采用 lazy TLB 并转交/增加 active_mm 引用；切入用户
	 * task 时真正 switch_mm，从 kthread 离开还要把借用 active_mm 延迟到锁外 put。
	 */
	if (!next->mm) {				// to kernel
		enter_lazy_tlb(prev->active_mm, next);

		next->active_mm = prev->active_mm;
		if (prev->mm)				// from user
			mmgrab_lazy_tlb(prev->active_mm);
		else
			prev->active_mm = NULL;
	} else {					// to user
		membarrier_switch_mm(rq, prev->active_mm, next->mm);
		/*
		 * sys_membarrier() requires an smp_mb() between setting
		 * rq->curr / membarrier_switch_mm() and returning to userspace.
		 *
		 * The below provides this either through switch_mm(), or in
		 * case 'prev->active_mm == next->mm' through
		 * finish_task_switch()'s mmdrop().
		 */
	/*
	 * 中文：membarrier 要求 rq->curr/mm 发布与返回用户态之间有完整屏障；
	 * 不同 mm 由 switch_mm 提供，相同 mm 则由 finish_task_switch 的延迟 mmdrop
	 * 路径提供。
	 */
		switch_mm_irqs_off(prev->active_mm, next->mm, next);
		lru_gen_use_mm(next->mm);

		if (!prev->mm) {			// from kernel
			/* will mmdrop_lazy_tlb() in finish_task_switch(). */
			/* 中文：kthread 借用的 active_mm 交给 finish_task_switch() 锁外释放。 */
			rq->prev_mm = prev->active_mm;
			prev->active_mm = NULL;
		}
	}

	mm_cid_switch_to(prev, next);

	/*
	 * Tell rseq that the task was scheduled in. Must be after
	 * switch_mm_cid() to get the TIF flag set.
	 */
	/* 中文：rseq sched-in 事件必须晚于 MM CID 切换，才能看到已设置的 TIF 标志。 */
	rseq_sched_switch_event(next);

	prepare_lock_switch(rq, next, rf);

	/* Here we just switch the register state and the stack. */
	/* 中文：此点真正切换寄存器与栈；返回时已处在 next 的历史调用帧。 */
	switch_to(prev, next, prev);
	barrier();

	return finish_task_switch(prev);
}

/*
 * nr_running and nr_context_switches:
 *
 * externally visible scheduler statistics: current number of runnable
 * threads, total number of context switches performed since bootup.
 */
/*
 * 原文：nr_running() 和 nr_context_switches* 是对外可见的近似统计。前者累加
 * online rq 当前 runnable 数，后者累加启动以来切换次数；遍历不冻结 hotplug/rq，
 * 因而结果可跨 CPU 时刻不一致，只适合观测而非同步决策。
 */
unsigned int nr_running(void)
{
	unsigned int i, sum = 0;

	for_each_online_cpu(i)
		sum += cpu_rq(i)->nr_running;

	return sum;
}

/*
 * Check if only the current task is running on the CPU.
 *
 * Caution: this function does not check that the caller has disabled
 * preemption, thus the result might have a time-of-check-to-time-of-use
 * race.  The caller is responsible to use it correctly, for example:
 *
 * - from a non-preemptible section (of course)
 *
 * - from a thread that is bound to a single CPU
 *
 * - in a loop with very short iterations (e.g. a polling loop)
 */
/*
 * 原文：判断本 CPU 是否只有 current runnable，但函数本身不禁抢占，检查与使用
 * 可能跨 CPU/状态变化。调用者应已不可抢占、绑定单 CPU，或只在极短轮询中把结果
 * 当提示。返回瞬时 bool，不取得 rq 锁。
 */
bool single_task_running(void)
{
	return raw_rq()->nr_running == 1;
}
EXPORT_SYMBOL(single_task_running);

/*
 * nr_context_switches_cpu() 返回指定 rq 的无锁计数快照；nr_context_switches()
 * 累加所有 possible CPU，包括 offline CPU 保存的历史。单位为切换次数。
 */
unsigned long long nr_context_switches_cpu(int cpu)
{
	return cpu_rq(cpu)->nr_switches;
}

unsigned long long nr_context_switches(void)
{
	int i;
	unsigned long long sum = 0;

	for_each_possible_cpu(i)
		sum += cpu_rq(i)->nr_switches;

	return sum;
}

/*
 * Consumers of these two interfaces, like for example the cpuidle menu
 * governor, are using nonsensical data. Preferring shallow idle state selection
 * for a CPU that has IO-wait which might not even end up running the task when
 * it does become runnable.
 */
/*
 * 原文警告：cpuidle menu 等消费者用 nr_iowait_cpu 做逐 CPU idle 深度选择，其
 * 语义并不可靠；blocked task 未来可能在另一 CPU 运行，计数不代表该 CPU 即将有
 * 工作。接口只返回 atomic 快照。
 */

unsigned int nr_iowait_cpu(int cpu)
{
	return atomic_read(&cpu_rq(cpu)->nr_iowait);
}

/*
 * IO-wait accounting, and how it's mostly bollocks (on SMP).
 *
 * The idea behind IO-wait account is to account the idle time that we could
 * have spend running if it were not for IO. That is, if we were to improve the
 * storage performance, we'd have a proportional reduction in IO-wait time.
 *
 * This all works nicely on UP, where, when a task blocks on IO, we account
 * idle time as IO-wait, because if the storage were faster, it could've been
 * running and we'd not be idle.
 *
 * This has been extended to SMP, by doing the same for each CPU. This however
 * is broken.
 *
 * Imagine for instance the case where two tasks block on one CPU, only the one
 * CPU will have IO-wait accounted, while the other has regular idle. Even
 * though, if the storage were faster, both could've ran at the same time,
 * utilising both CPUs.
 *
 * This means, that when looking globally, the current IO-wait accounting on
 * SMP is a lower bound, by reason of under accounting.
 *
 * Worse, since the numbers are provided per CPU, they are sometimes
 * interpreted per CPU, and that is nonsensical. A blocked task isn't strictly
 * associated with any one particular CPU, it can wake to another CPU than it
 * blocked on. This means the per CPU IO-wait number is meaningless.
 *
 * Task CPU affinities can make all that even more 'interesting'.
 */
/*
 * 原文完整结论：IO-wait 想估算“若存储更快，本可用于运行 task 的 idle 时间”。
 * UP 上 blocked task 与唯一 CPU 对应尚合理；SMP 上多个 task 可在一个 CPU 阻塞、
 * 醒到多个 CPU，导致全局和逐 CPU 统计都失真，通常只是低估下界，affinity 还会
 * 加剧偏差。因此 nr_iowait() 仅供兼容观测，不能解释为精确可并行 I/O 等待量。
 */

unsigned int nr_iowait(void)
{
	unsigned int i, sum = 0;

	for_each_possible_cpu(i)
		sum += nr_iowait_cpu(i);

	return sum;
}

/*
 * sched_exec - execve() is a valuable balancing opportunity, because at
 * this point the task has the smallest effective memory and cache footprint.
 */
/*
 * 原文：execve 时 task 的有效内存/cache 足迹最小，是低成本重新平衡机会。
 * sched_exec() 对 current 在 pi_lock 下让 class 选 @dest_cpu，目标不同且 active
 * 时构造栈上 migration_arg，再同步 stop_one_cpu 迁移。无返回值；选择失效则放弃，
 * exec 正确性不依赖迁移成功。
 */
void sched_exec(void)
{
	struct task_struct *p = current;
	struct migration_arg arg;
	int dest_cpu;

	scoped_guard (raw_spinlock_irqsave, &p->pi_lock) {
		dest_cpu = p->sched_class->select_task_rq(p, task_cpu(p), WF_EXEC);
		if (dest_cpu == smp_processor_id())
			return;

		if (unlikely(!cpu_active(dest_cpu)))
			return;

		arg = (struct migration_arg){ p, dest_cpu };
	}
	stop_one_cpu(task_cpu(p), migration_cpu_stop, &arg);
}

DEFINE_PER_CPU(struct kernel_stat, kstat);
DEFINE_PER_CPU(struct kernel_cpustat, kernel_cpustat) = {
#ifdef CONFIG_NO_HZ_COMMON
	.idle_sleeptime_seq = SEQCNT_ZERO(kernel_cpustat.idle_sleeptime_seq)
#endif
};

/*
 * kstat/kernel_cpustat 是永久 per-CPU 记账对象，分别承载 IRQ 等内核统计和 user/
 * system/idle 等 CPU 时间。NO_HZ 下 idle_sleeptime_seq 保护跨更新快照；导出符号
 * 允许其他内核组件读取，但读取者仍需遵守各字段的同步协议。
 */

EXPORT_PER_CPU_SYMBOL(kstat);
EXPORT_PER_CPU_SYMBOL(kernel_cpustat);

/*
 * The function fair_sched_class.update_curr accesses the struct curr
 * and its field curr->exec_start; when called from task_sched_runtime(),
 * we observe a high rate of cache misses in practice.
 * Prefetching this data results in improved performance.
 */
/*
 * 原文：task_sched_runtime() 调 fair update_curr 时会访问 cfs_rq->curr 及 exec_start，
 * 实测 cache miss 较高，提前 prefetch 可隐藏部分延迟。@p 仅用于定位借用 curr；
 * prefetch 是性能提示，不提供一致性或生命周期保证。
 */
static inline void prefetch_curr_exec_start(struct task_struct *p)
{
#ifdef CONFIG_FAIR_GROUP_SCHED
	struct sched_entity *curr = p->se.cfs_rq->curr;
#else
	struct sched_entity *curr = task_rq(p)->cfs.curr;
#endif
	prefetch(curr);
	prefetch(&curr->exec_start);
}

/*
 * Return accounted runtime for the task.
 * In case the task is currently running, return the runtime plus current's
 * pending runtime that have not been accounted yet.
 */
/*
 * 原文：返回 @p 已记账运行时间；若 task 当前运行，再把尚未结算的当前区间更新进去。
 * 单位纳秒。64 位平台对明确不在 CPU/rq 的 task 可无锁读 sum 快速返回；否则
 * task_rq_lock 稳定状态，只有同时为 current donor 且 queued 才 update_curr，
 * 避免把永不会归属该 task 的投影周期计入 clock_gettime。返回快照，无引用转移。
 */
unsigned long long task_sched_runtime(struct task_struct *p)
{
	struct rq_flags rf;
	struct rq *rq;
	u64 ns;

#ifdef CONFIG_64BIT
	/*
	 * 64-bit doesn't need locks to atomically read a 64-bit value.
	 * So we have a optimization chance when the task's delta_exec is 0.
	 * Reading ->on_cpu is racy, but this is OK.
	 *
	 * If we race with it leaving CPU, we'll take a lock. So we're correct.
	 * If we race with it entering CPU, unaccounted time is 0. This is
	 * indistinguishable from the read occurring a few cycles earlier.
	 * If we see ->on_cpu without ->on_rq, the task is leaving, and has
	 * been accounted, so we're correct here as well.
	 */
	if (!p->on_cpu || !task_on_rq_queued(p))
		return p->se.sum_exec_runtime;
#endif

	rq = task_rq_lock(p, &rf);
	/*
	 * Must be ->curr _and_ ->on_rq.  If dequeued, we would
	 * project cycles that may never be accounted to this
	 * thread, breaking clock_gettime().
	 */
	/*
	 * 中文：只有 task 同时是 curr/donor 且仍在 rq 才能投影当前执行时间；若已
	 * 出队，投影的周期可能永不正式记账，会破坏 clock_gettime 单调/准确性。
	 */
	if (task_current_donor(rq, p) && task_on_rq_queued(p)) {
		prefetch_curr_exec_start(p);
		update_rq_clock(rq);
		p->sched_class->update_curr(rq);
	}
	ns = p->se.sum_exec_runtime;
	task_rq_unlock(rq, p, &rf);

	return ns;
}

/*
 * cpu_resched_latency() 在 rq 锁下跟踪 NEED_RESCHED 从首次观察到当前 tick 的纳秒
 * 延迟。阈值关闭、启动期或无需调度返回 0；超过 sysctl 毫秒阈值返回延迟，并可
 * 由 once 开关抑制后续报告。静态 warned_once 是全系统提示门，不保护调度状态。
 */
static u64 cpu_resched_latency(struct rq *rq)
{
	int latency_warn_ms = READ_ONCE(sysctl_resched_latency_warn_ms);
	u64 resched_latency, now = rq_clock(rq);
	static bool warned_once;

	if (sysctl_resched_latency_warn_once && warned_once)
		return 0;

	if (!need_resched() || !latency_warn_ms)
		return 0;

	if (system_state == SYSTEM_BOOTING)
		return 0;

	if (!rq->last_seen_need_resched_ns) {
		rq->last_seen_need_resched_ns = now;
		rq->ticks_without_resched = 0;
		return 0;
	}

	rq->ticks_without_resched++;
	resched_latency = now - rq->last_seen_need_resched_ns;
	if (resched_latency <= latency_warn_ms * NSEC_PER_MSEC)
		return 0;

	warned_once = true;

	return resched_latency;
}

/*
 * setup_resched_latency_warn_ms() 解析启动参数为 long 毫秒值并写 sysctl；@str 为
 * 启动期借用文本。无论解析成功与否都返回 1 表示参数已消费，失败保留旧值并警告。
 */
static int __init setup_resched_latency_warn_ms(char *str)
{
	long val;

	if ((kstrtol(str, 0, &val))) {
		pr_warn("Unable to set resched_latency_warn_ms\n");
		return 1;
	}

	sysctl_resched_latency_warn_ms = val;
	return 1;
}
__setup("resched_latency_warn_ms=", setup_resched_latency_warn_ms);

/*
 * This function gets called by the timer code, with HZ frequency.
 * We call it with interrupts disabled.
 */
/*
 * 原文：timer 以 HZ 频率、在 IRQ 关闭状态调用 sched_tick()。
 *
 * 入参/直接返回值均无；目标恒为本 CPU rq。函数更新 sched_clock、PSI IRQ 时间、
 * hardware pressure 与 rq 时钟，再把 tick 交给当前 donor 的 sched_class，
 * 由 class 更新时间片/虚拟时间并按需 resched_curr()。之后推进全局负载、core
 * scheduling 和 sched_ext。rq 锁保护 curr/donor 与统计快照；回调不能睡眠。
 * tick 只发布重调度请求，真正 context switch 在 IRQ 返回的安全点发生。
 */
void sched_tick(void)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	/* accounting goes to the donor task */
	struct task_struct *donor;
	struct rq_flags rf;
	unsigned long hw_pressure;
	u64 resched_latency;

	if (housekeeping_cpu(cpu, HK_TYPE_KERNEL_NOISE))
		arch_scale_freq_tick();

	sched_clock_tick();

	rq_lock(rq, &rf);
	donor = rq->donor;

	psi_account_irqtime(rq, donor, NULL);

	update_rq_clock(rq);
	hw_pressure = arch_scale_hw_pressure(cpu_of(rq));
	update_hw_load_avg(rq_clock_task(rq), rq, hw_pressure);

	if (dynamic_preempt_lazy() && tif_test_bit(TIF_NEED_RESCHED_LAZY))
		resched_curr(rq);

	donor->sched_class->task_tick(rq, donor, 0);
	if (sched_feat(LATENCY_WARN))
		resched_latency = cpu_resched_latency(rq);
	calc_global_load_tick(rq);
	sched_core_tick(rq);
	scx_tick(rq);

	rq_unlock(rq, &rf);

	if (sched_feat(LATENCY_WARN) && resched_latency)
		resched_latency_warn(cpu, resched_latency);

	perf_event_task_tick();

	if (donor->flags & PF_WQ_WORKER)
		wq_worker_tick(donor);

	if (!scx_switched_all()) {
		rq->idle_balance = idle_cpu(cpu);
		sched_balance_trigger(rq);
	}
}

#ifdef CONFIG_NO_HZ_FULL

/*
 * tick_work 是每个 full-nohz CPU 的远端 1Hz tick 载体：cpu 是固定目标，state
 * 原子协调 work/hotplug，delayed_work 在 housekeeper 上运行。
 */
struct tick_work {
	int			cpu;
	atomic_t		state;
	struct delayed_work	work;
};
/*
 * tick_work 是每个 isolated/full-nohz CPU 的远端 1Hz 调度 tick 状态。cpu 固定目标，
 * state 在 work 与 hotplug 之间原子转换，delayed_work 实际运行在 housekeeper。
 * per-CPU 容器启动期分配，work 排队期间由 workqueue 保证对象仍存活。
 */
/* Values for ->state, see diagram below. */
#define TICK_SCHED_REMOTE_OFFLINE	0
#define TICK_SCHED_REMOTE_OFFLINING	1
#define TICK_SCHED_REMOTE_RUNNING	2

/*
 * State diagram for ->state:
 *
 *
 *          TICK_SCHED_REMOTE_OFFLINE
 *                    |   ^
 *                    |   |
 *                    |   | sched_tick_remote()
 *                    |   |
 *                    |   |
 *                    +--TICK_SCHED_REMOTE_OFFLINING
 *                    |   ^
 *                    |   |
 * sched_tick_start() |   | sched_tick_stop()
 *                    |   |
 *                    V   |
 *          TICK_SCHED_REMOTE_RUNNING
 *
 *
 * Other transitions get WARN_ON_ONCE(), except that sched_tick_remote()
 * and sched_tick_start() are happy to leave the state in RUNNING.
 */
/*
 * 原图状态机：OFFLINE 经 start 进入 RUNNING；stop 把 RUNNING 置 OFFLINING；下一次
 * remote work 观察 OFFLINING 后收敛到 OFFLINE。remote/start 重复观察 RUNNING
 * 被允许，其余非法跃迁 WARN。stop 不能 cancel work，否则状态可能永远停在
 * OFFLINING 且 work/cancel 的 ownership 关系混乱。
 */

static struct tick_work __percpu *tick_work_cpu;

/*
 * sched_tick_remote() 在 housekeeper 上代替 full-nohz @cpu 执行约 1Hz 调度 tick。
 * @work 嵌入 tick_work；若目标 tick 确实停止，锁远端 rq、更新 clock/class tick
 * 和 load。竞态导致多/少一次 tick 可接受，因为统计按时间差更新。尾部原子状态
 * 机决定重排下一次 work 或完成 offlining；函数可睡眠于 workqueue 上下文。
 */
static void sched_tick_remote(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct tick_work *twork = container_of(dwork, struct tick_work, work);
	int cpu = twork->cpu;
	struct rq *rq = cpu_rq(cpu);
	int os;

	/*
	 * Handle the tick only if it appears the remote CPU is running in full
	 * dynticks mode. The check is racy by nature, but missing a tick or
	 * having one too much is no big deal because the scheduler tick updates
	 * statistics and checks timeslices in a time-independent way, regardless
	 * of when exactly it is running.
	 */
	if (tick_nohz_tick_stopped_cpu(cpu)) {
		guard(rq_lock_irq)(rq);
		struct task_struct *curr = rq->curr;

		if (cpu_online(cpu)) {
			/*
			 * Since this is a remote tick for full dynticks mode,
			 * we are always sure that there is no proxy (only a
			 * single task is running).
			 */
			/* 中文：full-nohz 远端 tick 只服务单运行 task，不应存在 proxy donor 分离。 */
			WARN_ON_ONCE(rq->curr != rq->donor);
			update_rq_clock(rq);

			if (!is_idle_task(curr)) {
				/*
				 * Make sure the next tick runs within a
				 * reasonable amount of time.
				 */
				/* 中文：检查远端 tick 没有延迟到不合理的 30 秒以上。 */
				u64 delta = rq_clock_task(rq) - curr->se.exec_start;
				WARN_ON_ONCE(delta > (u64)NSEC_PER_SEC * 30);
			}
			curr->sched_class->task_tick(rq, curr, 0);

			calc_load_nohz_remote(rq);
		}
	}

	/*
	 * Run the remote tick once per second (1Hz). This arbitrary
	 * frequency is large enough to avoid overload but short enough
	 * to keep scheduler internal stats reasonably up to date.  But
	 * first update state to reflect hotplug activity if required.
	 */
	os = atomic_fetch_add_unless(&twork->state, -1, TICK_SCHED_REMOTE_RUNNING);
	WARN_ON_ONCE(os == TICK_SCHED_REMOTE_OFFLINE);
	if (os == TICK_SCHED_REMOTE_RUNNING)
		queue_delayed_work(system_dfl_wq, dwork, HZ);
}

/*
 * sched_tick_start() 为非-housekeeping @cpu 启动远端 tick：原子置 RUNNING，若从
 * OFFLINE 首次进入则初始化并排队 delayed_work；已 RUNNING 属重复错误并 WARN。
 * 无返回值，tick_work_cpu 必须已由 offload_init 分配。
 */
static void sched_tick_start(int cpu)
{
	int os;
	struct tick_work *twork;

	if (housekeeping_cpu(cpu, HK_TYPE_KERNEL_NOISE))
		return;

	WARN_ON_ONCE(!tick_work_cpu);

	twork = per_cpu_ptr(tick_work_cpu, cpu);
	os = atomic_xchg(&twork->state, TICK_SCHED_REMOTE_RUNNING);
	WARN_ON_ONCE(os == TICK_SCHED_REMOTE_RUNNING);
	if (os == TICK_SCHED_REMOTE_OFFLINE) {
		twork->cpu = cpu;
		INIT_DELAYED_WORK(&twork->work, sched_tick_remote);
		queue_delayed_work(system_dfl_wq, &twork->work, HZ);
	}
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * sched_tick_stop() 在 hot-unplug 把目标状态置 OFFLINING，但故意不 cancel work；
 * 已排队回调负责完成 OFFLINE 转换。外层 hotplug 串行化竞争，atomic xchg 仍用于
 * 校验状态机。housekeeping CPU 无远端 work，直接返回。
 */
static void sched_tick_stop(int cpu)
{
	struct tick_work *twork;
	int os;

	if (housekeeping_cpu(cpu, HK_TYPE_KERNEL_NOISE))
		return;

	WARN_ON_ONCE(!tick_work_cpu);

	twork = per_cpu_ptr(tick_work_cpu, cpu);
	/* There cannot be competing actions, but don't rely on stop-machine. */
	os = atomic_xchg(&twork->state, TICK_SCHED_REMOTE_OFFLINING);
	WARN_ON_ONCE(os != TICK_SCHED_REMOTE_RUNNING);
	/* Don't cancel, as this would mess up the state machine. */
}
#endif /* CONFIG_HOTPLUG_CPU */

/*
 * sched_tick_offload_init() - 为 full-nohz CPU 初始化远端调度 tick 存储。
 *
 * housekeeping_init() 在 HK_TYPE_KERNEL_NOISE 已配置时调用。入参：无。
 * 函数处于启动期并标记 __init，不存在并发启动/停止 tick_work；alloc_percpu()
 * 为每个 possible CPU 建立 struct tick_work，后续 sched_tick_start/stop 路径
 * 使用它在 housekeeper 上代替隔离 CPU 执行必要调度 tick。
 *
 * 返回 0 表示初始化完成。分配失败通过 BUG_ON 终止启动而不是返回 -ENOMEM，
 * 因为继续运行会让已承诺的 nohz_full 调度记账缺少必需状态。
 */
int __init sched_tick_offload_init(void)
{
	tick_work_cpu = alloc_percpu(struct tick_work);
	BUG_ON(!tick_work_cpu);
	return 0;
}

#else /* !CONFIG_NO_HZ_FULL: */
/* 未启用 full-nohz 时 start/stop stub 无副作用，普通本地 tick 负责调度记账。 */
static inline void sched_tick_start(int cpu) { }
static inline void sched_tick_stop(int cpu) { }
#endif /* !CONFIG_NO_HZ_FULL */

#if defined(CONFIG_PREEMPTION) && (defined(CONFIG_DEBUG_PREEMPT) || \
				defined(CONFIG_TRACE_PREEMPT_TOGGLE))
/*
 * If the value passed in is equal to the current preempt count
 * then we just disabled preemption. Start timing the latency.
 */
/*
 * 原文：加上 @val 后若 preempt_count 恰等于该值，说明从可抢占跨到第一次禁抢占，
 * 记录调用 IP 并发出 preempt_off trace；嵌套 disable 不重复开始区间。
 */
static inline void preempt_latency_start(int val)
{
	if (preempt_count() == val) {
		unsigned long ip = get_lock_parent_ip();
#ifdef CONFIG_DEBUG_PREEMPT
		current->preempt_disable_ip = ip;
#endif
		trace_preempt_off(CALLER_ADDR0, ip);
	}
}

/*
 * preempt_count_add()/sub() 是导出的调试包装：检查下溢/接近溢出，在最外层边界
 * 开始/结束 latency trace，再修改架构 preempt_count。@val 是计数增减量，不是
 * 新绝对值；函数无返回值，错误只 WARN 并拒绝破坏计数。
 */
void preempt_count_add(int val)
{
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Underflow?
	 */
	if (DEBUG_LOCKS_WARN_ON((preempt_count() < 0)))
		return;
#endif
	__preempt_count_add(val);
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Spinlock count overflowing soon?
	 */
	/* 中文：抢占/自旋锁计数接近位域上限时提前告警。 */
	DEBUG_LOCKS_WARN_ON((preempt_count() & PREEMPT_MASK) >=
				PREEMPT_MASK - 10);
#endif
	preempt_latency_start(val);
}
EXPORT_SYMBOL(preempt_count_add);
NOKPROBE_SYMBOL(preempt_count_add);

/*
 * If the value passed in equals to the current preempt count
 * then we just enabled preemption. Stop timing the latency.
 */
/*
 * 原文：减去 @val 前若当前计数等于 @val，操作将回到可抢占状态，因此结束 latency
 * trace；嵌套 enable 不结束外层区间。
 */
static inline void preempt_latency_stop(int val)
{
	if (preempt_count() == val)
		trace_preempt_on(CALLER_ADDR0, get_lock_parent_ip());
}

void preempt_count_sub(int val)
{
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Underflow?
	 */
	/* 中文：禁止一次减量超过当前 preempt_count。 */
	if (DEBUG_LOCKS_WARN_ON(val > preempt_count()))
		return;
	/*
	 * Is the spinlock portion underflowing?
	 */
	/* 中文：小于 PREEMPT_MASK 的减量不能让本已为零的自旋锁子计数下溢。 */
	if (DEBUG_LOCKS_WARN_ON((val < PREEMPT_MASK) &&
			!(preempt_count() & PREEMPT_MASK)))
		return;
#endif

	preempt_latency_stop(val);
	__preempt_count_sub(val);
}
EXPORT_SYMBOL(preempt_count_sub);
NOKPROBE_SYMBOL(preempt_count_sub);

#else
static inline void preempt_latency_start(int val) { }
static inline void preempt_latency_stop(int val) { }
#endif

/*
 * get_preempt_disable_ip() 返回 DEBUG_PREEMPT 记录的首次禁抢占调用地址；未编译
 * 调试时固定返回 0。@p 为诊断期借用 task。
 */
static inline unsigned long get_preempt_disable_ip(struct task_struct *p)
{
#ifdef CONFIG_DEBUG_PREEMPT
	return p->preempt_disable_ip;
#else
	return 0;
#endif
}

/*
 * Print scheduling while atomic bug:
 */
/*
 * 原文：报告 atomic 上下文调用 schedule 的错误。@prev 是当前 task 的稳定借用；
 * 函数 noinline 以保留有意义栈，先保存 disable IP，再打印锁、模块、IRQ trace
 * 并触发可能的 panic。无返回值；oops 已进行时避免递归诊断。
 */
static noinline void __schedule_bug(struct task_struct *prev)
{
	/* Save this before calling printk(), since that will clobber it */
	unsigned long preempt_disable_ip = get_preempt_disable_ip(current);

	if (oops_in_progress)
		return;

	printk(KERN_ERR "BUG: scheduling while atomic: %s/%d/0x%08x\n",
		prev->comm, prev->pid, preempt_count());

	debug_show_held_locks(prev);
	print_modules();
	if (irqs_disabled())
		print_irqtrace_events(prev);
	if (IS_ENABLED(CONFIG_DEBUG_PREEMPT)) {
		pr_err("Preemption disabled at:");
		print_ip_sym(KERN_ERR, preempt_disable_ip);
	}
	check_panic_on_warn("scheduling while atomic");

	dump_stack();
	add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
}

/*
 * Various schedule()-time debugging checks and statistics:
 */
/*
 * 原文：schedule_debug() 在每次进入 __schedule 前执行调试与统计。@prev 是 curr，
 * @preempt 区分抢占和主动阻塞；检查普通/影子栈尾、non_block_count、atomic 调度、
 * RCU sleep 与 context tracking，并增加 sched_count。严重栈损坏 panic，其余
 * 协议错误 WARN/taint 后尽量修复 preempt_count 继续诊断。
 */
static inline void schedule_debug(struct task_struct *prev, bool preempt)
{
#ifdef CONFIG_SCHED_STACK_END_CHECK
	if (task_stack_end_corrupted(prev))
		panic("corrupted stack end detected inside scheduler\n");

	if (task_scs_end_corrupted(prev))
		panic("corrupted shadow stack detected inside scheduler\n");
#endif

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
	if (!preempt && READ_ONCE(prev->__state) && prev->non_block_count) {
		printk(KERN_ERR "BUG: scheduling in a non-blocking section: %s/%d/%i\n",
			prev->comm, prev->pid, prev->non_block_count);
		dump_stack();
		add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
	}
#endif

	if (unlikely(in_atomic_preempt_off())) {
		__schedule_bug(prev);
		preempt_count_set(PREEMPT_DISABLED);
	}
	rcu_sleep_check();
	WARN_ON_ONCE(ct_state() == CT_STATE_USER);

	profile_hit(SCHED_PROFILING, __builtin_return_address(0));

	schedstat_inc(this_rq()->sched_count);
}

/*
 * prev_balance() 在 put_prev_task 前从 donor 所属 class 向低优先级 class 执行
 * balance pull。这样 callback 临时放锁时 donor 仍保持进入 rq 锁前的完整状态；
 * 一旦某 class 已有 runnable 同级或更高任务即可停止。@rq 已锁，@rf 供回调管理
 * lock pin；无返回值。
 */
static void prev_balance(struct rq *rq, struct rq_flags *rf)
{
	const struct sched_class *start_class = rq->donor->sched_class;
	const struct sched_class *class;

	/*
	 * We must do the balancing pass before put_prev_task(), such
	 * that when we release the rq->lock the task is in the same
	 * state as before we took rq->lock.
	 *
	 * We can terminate the balance pass as soon as we know there is
	 * a runnable task of @class priority or higher.
	 */
	for_active_class_range(class, start_class, &idle_sched_class) {
		if (class->balance && class->balance(rq, rf))
			break;
	}
}

/*
 * Pick up the highest-prio task:
 */
/*
 * 原文：选择最高优先级 task。@rq 已锁，@rf 传给可能临时放锁的 class callback。
 * 全为 fair 时直接走快速路径；否则先 balance，再按 active class 从高到低 pick。
 * RETRY_TASK 代表队列在 callback 后需从头重选；idle class 保证最终非 NULL。
 * 成功同时 put_prev_set_next_task，返回 rq 锁保护下的借用 next。
 */
static inline struct task_struct *
__pick_next_task(struct rq *rq, struct rq_flags *rf)
	__must_hold(__rq_lockp(rq))
{
	const struct sched_class *class;
	struct task_struct *p;

	rq->dl_server = NULL;

	if (scx_enabled())
		goto restart;

	/*
	 * Optimization: we know that if all tasks are in the fair class we can
	 * call that function directly, but only if the @prev task wasn't of a
	 * higher scheduling class, because otherwise those lose the
	 * opportunity to pull in more work from other CPUs.
	 */
	/*
	 * 中文：若所有 runnable 都属 fair 且 prev/donor 不高于 fair，可直接调用
	 * fair pick；否则高优先类即使本 rq 暂空，也必须获得从其他 CPU 拉任务机会。
	 */
	if (likely(!sched_class_above(rq->donor->sched_class, &fair_sched_class) &&
		   rq->nr_running == rq->cfs.h_nr_queued)) {

		p = pick_task_fair(rq, rf);
		if (unlikely(p == RETRY_TASK))
			goto restart;

		/* Assume the next prioritized class is idle_sched_class */
	/* 中文：fair 未选到 task 时，下一个有效优先类可直接视为 idle。 */
		if (!p)
			p = pick_task_idle(rq, rf);

		put_prev_set_next_task(rq, rq->donor, p);
		return p;
	}

restart:
	prev_balance(rq, rf);

	for_each_active_class(class) {
		p = class->pick_task(rq, rf);
		if (unlikely(p == RETRY_TASK))
			goto restart;
		if (p) {
			put_prev_set_next_task(rq, rq->donor, p);
			return p;
		}
	}

	BUG(); /* The idle class should always have a runnable task. */
}

#ifdef CONFIG_SCHED_CORE
/*
 * is_task_rq_idle()/cookie_equals()/cookie_match() 是 core scheduling 的相容谓词。
 * idle task 可与任意 cookie 共跑；普通 task 必须 cookie 相等。参数均为 sibling
 * rq 联合锁保护的借用 task，返回瞬时 bool。
 */
static inline bool is_task_rq_idle(struct task_struct *t)
{
	return (task_rq(t)->idle == t);
}

static inline bool cookie_equals(struct task_struct *a, unsigned long cookie)
{
	return is_task_rq_idle(a) || (a->core_cookie == cookie);
}

static inline bool cookie_match(struct task_struct *a, struct task_struct *b)
{
	if (is_task_rq_idle(a) || is_task_rq_idle(b))
		return true;

	return a->core_cookie == b->core_cookie;
}

/*
 * Careful; this can return RETRY_TASK, it does not include the retry-loop
 * itself due to the whole SMT pick retry thing below.
 */
/*
 * 原文警告：pick_task() 只按 class 选择一个 sibling 的候选，可能返回 RETRY_TASK；
 * 它故意不在内部循环，因为 core-wide 选择必须让所有 sibling 一起重新开始。
 * @rq 已在联合锁范围，返回借用 task。
 */
static inline struct task_struct *pick_task(struct rq *rq, struct rq_flags *rf)
{
	const struct sched_class *class;
	struct task_struct *p;

	rq->dl_server = NULL;

	for_each_active_class(class) {
		p = class->pick_task(rq, rf);
		if (p)
			return p;
	}

	BUG(); /* The idle class should always have a runnable task. */
}

extern void task_vruntime_update(struct rq *rq, struct task_struct *p, bool in_fi);

static void queue_core_balance(struct rq *rq);

/*
 * pick_next_task() - 在 core scheduling 开启时为整个 SMT sibling 集合联合选 task。
 *
 * @rq 是当前 CPU 已锁 runqueue，@rf 供 class/balance 管理锁；返回当前 CPU 将运行
 * 的借用 task，并为每个 sibling 缓存 core_pick。关闭 core scheduling 或 CPU
 * offline 时退回 __pick_next_task()。
 *
 * 阶段：
 * 1. 若 task_seq 未变化且上次 core-wide pick 尚未被本 sibling 消费，直接复用；
 * 2. 结算上一段 forced-idle，普通无 cookie 快速路径可单 rq 选择；
 * 3. 每个 sibling 先各选最高优先候选，从中确定全 core 最高优先 cookie；
 * 4. 每个 sibling 查找同 cookie task，找不到则强制选 idle，并记录安全隔离导致
 *    的 occupation/force-idle 时间；
 * 5. 发布 pick/sched 序列并 IPI 需要改变选择的 sibling，最后为当前 rq 安装 next。
 *
 * RETRY_TASK 会让整个 sibling 选择重来，不能只重选一个 CPU，否则 cookie 快照
 * 可能混合不同队列时刻。函数不增加 task 引用；联合 rq 锁与序列号保证缓存有效。
 */
static struct task_struct *
pick_next_task(struct rq *rq, struct rq_flags *rf)
	__must_hold(__rq_lockp(rq))
{
	struct task_struct *next, *p, *max;
	const struct cpumask *smt_mask;
	bool fi_before = false;
	bool core_clock_updated = (rq == rq->core);
	unsigned long cookie;
	int i, cpu, occ = 0;
	struct rq *rq_i;
	bool need_sync;

	if (!sched_core_enabled(rq))
		return __pick_next_task(rq, rf);

	cpu = cpu_of(rq);

	/* Stopper task is switching into idle, no need core-wide selection. */
	/* 中文：offline CPU 的 stopper 正切向 idle，无需再做 core-wide 联合选择。 */
	if (cpu_is_offline(cpu)) {
		/*
		 * Reset core_pick so that we don't enter the fastpath when
		 * coming online. core_pick would already be migrated to
		 * another cpu during offline.
		 */
	/*
	 * 中文：清掉缓存 pick，防止 CPU 再上线时错误命中快速路径；offline 期间有效
	 * core pick 已迁移给其他 sibling。
	 */
		rq->core_pick = NULL;
		rq->core_dl_server = NULL;
		return __pick_next_task(rq, rf);
	}

	/*
	 * If there were no {en,de}queues since we picked (IOW, the task
	 * pointers are all still valid), and we haven't scheduled the last
	 * pick yet, do so now.
	 *
	 * rq->core_pick can be NULL if no selection was made for a CPU because
	 * it was either offline or went offline during a sibling's core-wide
	 * selection. In this case, do a core-wide selection.
	 */
	/*
	 * 中文：task_seq 未变且本 sibling 尚未消费同一 pick_seq 时可复用缓存指针；
	 * 若离线竞态导致 core_pick 为空，则必须重做整个 core 选择。
	 */
	if (rq->core->core_pick_seq == rq->core->core_task_seq &&
	    rq->core->core_pick_seq != rq->core_sched_seq &&
	    rq->core_pick) {
		WRITE_ONCE(rq->core_sched_seq, rq->core->core_pick_seq);

		next = rq->core_pick;
		rq->dl_server = rq->core_dl_server;
		rq->core_pick = NULL;
		rq->core_dl_server = NULL;
		goto out_set_next;
	}

	prev_balance(rq, rf);

	smt_mask = cpu_smt_mask(cpu);
	need_sync = !!rq->core->core_cookie;

	/* reset state */
	/* 中文：开始新一轮前清除共享 cookie。 */
	rq->core->core_cookie = 0UL;
	if (rq->core->core_forceidle_count) {
		if (!core_clock_updated) {
			update_rq_clock(rq->core);
			core_clock_updated = true;
		}
		sched_core_account_forceidle(rq);
		/* reset after accounting force idle */
	/* 中文：结算上一 forced-idle 区间后清零开始时间、数量与 occupation。 */
		rq->core->core_forceidle_start = 0;
		rq->core->core_forceidle_count = 0;
		rq->core->core_forceidle_occupation = 0;
		need_sync = true;
		fi_before = true;
	}

	/*
	 * core->core_task_seq, core->core_pick_seq, rq->core_sched_seq
	 *
	 * @task_seq guards the task state ({en,de}queues)
	 * @pick_seq is the @task_seq we did a selection on
	 * @sched_seq is the @pick_seq we scheduled
	 *
	 * However, preemptions can cause multiple picks on the same task set.
	 * 'Fix' this by also increasing @task_seq for every pick.
	 */
	/*
	 * 中文：task_seq 保护入出队集合，pick_seq 表示在哪个集合上选择，sched_seq
	 * 表示 sibling 已消费哪次选择。抢占会对同一集合重复 pick，因此每次 pick
	 * 也递增 task_seq，使缓存代际保持唯一。
	 */
	rq->core->core_task_seq++;

	/*
	 * Optimize for common case where this CPU has no cookies
	 * and there are no cookied tasks running on siblings.
	 */
	/* 中文：本 CPU 无 cookie 且 sibling 也未运行 cookied task 时走单 rq 快速路径。 */
	if (!need_sync) {
restart_single:
		next = pick_task(rq, rf);
		if (unlikely(next == RETRY_TASK))
			goto restart_single;
		if (!next->core_cookie) {
			rq->core_pick = NULL;
			rq->core_dl_server = NULL;
			/*
			 * For robustness, update the min_vruntime_fi for
			 * unconstrained picks as well.
			 */
			/* 中文：即使无 cookie 约束也更新 min_vruntime_fi，保持后续切入 FI 的基线。 */
			WARN_ON_ONCE(fi_before);
			task_vruntime_update(rq, next, false);
			goto out_set_next;
		}
	}

	/*
	 * For each thread: do the regular task pick and find the max prio task
	 * amongst them.
	 *
	 * Tie-break prio towards the current CPU
	 */
	/*
	 * 中文：第一遍为每个 sibling 做普通最高优先选择，再找全 core 的最高优先
	 * 候选以确定 cookie；优先级相同偏向当前 CPU，减少不必要切换。
	 */
restart_multi:
	max = NULL;
	for_each_cpu_wrap(i, smt_mask, cpu) {
		rq_i = cpu_rq(i);

		/*
		 * Current cpu always has its clock updated on entrance to
		 * pick_next_task(). If the current cpu is not the core,
		 * the core may also have been updated above.
		 */
	/*
	 * 中文：当前 CPU 入口已更新 clock；其他 sibling 必须更新，core leader 若
	 * 已在 forced-idle 结算阶段更新则不重复。
	 */
		if (i != cpu && (rq_i != rq->core || !core_clock_updated))
			update_rq_clock(rq_i);

		p = pick_task(rq_i, rf);
		if (unlikely(p == RETRY_TASK))
			goto restart_multi;

		rq_i->core_pick = p;
		rq_i->core_dl_server = rq_i->dl_server;

		if (!max || prio_less(max, p, fi_before))
			max = p;
	}

	cookie = rq->core->core_cookie = max->core_cookie;

	/*
	 * For each thread: try and find a runnable task that matches @max or
	 * force idle.
	 */
	/*
	 * 中文：第二遍在每个 sibling 找与全 core cookie 匹配的 runnable task；
	 * 找不到就选 idle，形成安全所需的 forced idle。
	 */
	for_each_cpu(i, smt_mask) {
		rq_i = cpu_rq(i);
		p = rq_i->core_pick;

		if (!cookie_equals(p, cookie)) {
			p = NULL;
			if (cookie)
				p = sched_core_find(rq_i, cookie);
			if (!p)
				p = idle_sched_class.pick_task(rq_i, rf);
		}

		rq_i->core_pick = p;
		rq_i->core_dl_server = NULL;

		if (p == rq_i->idle) {
			if (rq_i->nr_running) {
				rq->core->core_forceidle_count++;
				if (!fi_before)
					rq->core->core_forceidle_seq++;
			}
		} else {
			occ++;
		}
	}

	if (schedstat_enabled() && rq->core->core_forceidle_count) {
		rq->core->core_forceidle_start = rq_clock(rq->core);
		rq->core->core_forceidle_occupation = occ;
	}

	rq->core->core_pick_seq = rq->core->core_task_seq;
	next = rq->core_pick;
	rq->core_sched_seq = rq->core->core_pick_seq;

	/* Something should have been selected for current CPU */
	/* 中文：当前 CPU 必须至少选到 idle；为空表示联合选择协议损坏。 */
	WARN_ON_ONCE(!next);

	/*
	 * Reschedule siblings
	 *
	 * NOTE: L1TF -- at this point we're no longer running the old task and
	 * sending an IPI (below) ensures the sibling will no longer be running
	 * their task. This ensures there is no inter-sibling overlap between
	 * non-matching user state.
	 */
	/*
	 * 中文：对选择改变的 sibling 发 resched IPI。当前 CPU 已离开旧用户 task，
	 * IPI 又迫使 sibling 离开旧 task，从而避免不同 cookie 用户状态跨 sibling
	 * 重叠，满足 L1TF 隔离要求。
	 */
	for_each_cpu(i, smt_mask) {
		rq_i = cpu_rq(i);

		/*
		 * An online sibling might have gone offline before a task
		 * could be picked for it, or it might be offline but later
		 * happen to come online, but its too late and nothing was
		 * picked for it.  That's Ok - it will pick tasks for itself,
		 * so ignore it.
		 */
	/*
	 * 中文：sibling 可能在联合选择中途下线，或原本离线后才上线，因而没有
	 * core_pick；忽略即可，它会在自己的调度入口重新联合选择。
	 */
		if (!rq_i->core_pick)
			continue;

		/*
		 * Update for new !FI->FI transitions, or if continuing to be in !FI:
		 * fi_before     fi      update?
		 *  0            0       1
		 *  0            1       1
		 *  1            0       1
		 *  1            1       0
		 */
	/*
	 * 中文：除“上一轮已 FI 且本轮继续 FI”外都更新 task vruntime；连续 FI 已由
	 * 同一受限基线记账，无需重复推进。
	 */
		if (!(fi_before && rq->core->core_forceidle_count))
			task_vruntime_update(rq_i, rq_i->core_pick, !!rq->core->core_forceidle_count);

		rq_i->core_pick->core_occupation = occ;

		if (i == cpu) {
			rq_i->core_pick = NULL;
			rq_i->core_dl_server = NULL;
			continue;
		}

		/* Did we break L1TF mitigation requirements? */
	/* 中文：最终逐 sibling 复核 cookie 相容性，任何不匹配都会破坏 L1TF 缓解。 */
		WARN_ON_ONCE(!cookie_match(next, rq_i->core_pick));

		if (rq_i->curr == rq_i->core_pick) {
			rq_i->core_pick = NULL;
			rq_i->core_dl_server = NULL;
			continue;
		}

		resched_curr(rq_i);
	}

out_set_next:
	put_prev_set_next_task(rq, rq->donor, next);
	if (rq->core->core_forceidle_count && next == rq->idle)
		queue_core_balance(rq);

	return next;
}

/*
 * try_steal_cookie() - 为 forced-idle @this 从 @that 偷取同 core cookie task。
 *
 * 函数关闭 IRQ并按序锁两 rq，要求目标 core 有 cookie 且 dst curr 为 idle；遍历
 * src 同 cookie、未 throttled、允许在 this 且不会恶化 occupation 的候选，成功
 * 跨 rq 移动并 resched dst。返回 true 表示迁移一项，false 表示无合适候选。
 */
static bool try_steal_cookie(int this, int that)
{
	struct rq *dst = cpu_rq(this), *src = cpu_rq(that);
	struct task_struct *p;
	unsigned long cookie;
	bool success = false;

	guard(irq)();
	guard(double_rq_lock)(dst, src);

	cookie = dst->core->core_cookie;
	if (!cookie)
		return false;

	if (dst->curr != dst->idle)
		return false;

	p = sched_core_find(src, cookie);
	if (!p)
		return false;

	do {
		if (p == src->core_pick || p == src->curr)
			goto next;

		if (!is_cpu_allowed(p, this))
			goto next;

		if (p->core_occupation > dst->idle->core_occupation)
			goto next;
		/*
		 * sched_core_find() and sched_core_next() will ensure
		 * that task @p is not throttled now, we also need to
		 * check whether the runqueue of the destination CPU is
		 * being throttled.
		 */
	/*
	 * 中文：树遍历保证 @p 当前未被节流，但迁到目标 CPU 后其目标 cfs_rq 仍可能
	 * 受祖先带宽节流，故必须按目的 CPU 再检查。
	 */
		if (sched_task_is_throttled(p, this))
			goto next;

		move_queued_task_locked(src, dst, p);
		resched_curr(dst);

		success = true;
		break;

next:
		p = sched_core_next(p, cookie);
	} while (p);

	return success;
}

/*
 * steal_cookie_task() 在 @sd span 内从 @cpu 后循环尝试 donor CPU；若本 CPU 已需
 * resched 提前停止。返回是否成功偷到一个 task，不持有跨调用的 rq 锁。
 */
static bool steal_cookie_task(int cpu, struct sched_domain *sd)
{
	int i;

	for_each_cpu_wrap(i, sched_domain_span(sd), cpu + 1) {
		if (i == cpu)
			continue;

		if (need_resched())
			break;

		if (try_steal_cookie(cpu, i))
			return true;
	}

	return false;
}

/*
 * sched_core_balance() 是 forced-idle balance callback：入口/出口持 rq 锁，中间
 * 在禁抢占+RCU 下解锁并沿 sched_domain 尝试 cookie steal，迁移 helper 自行复核。
 */
static void sched_core_balance(struct rq *rq)
	__must_hold(__rq_lockp(rq))
{
	struct sched_domain *sd;
	int cpu = cpu_of(rq);

	guard(preempt)();
	guard(rcu)();

	raw_spin_rq_unlock_irq(rq);
	for_each_domain(cpu, sd) {
		if (need_resched())
			break;

		if (steal_cookie_task(cpu, sd))
			break;
	}
	raw_spin_rq_lock_irq(rq);
}

/*
 * sched_core_balance() 是 balance callback：入口/出口均持 @rq 锁，但中间临时解锁，
 * 在 RCU sched_domain 中尝试 cookie steal，再重新锁回。@rq 可能在解锁期变化，
 * 所有迁移 helper 会自行复核；无返回值。
 */

static DEFINE_PER_CPU(struct balance_callback, core_balance_head);

/*
 * queue_core_balance() 仅在 core scheduling 开启、core 有 cookie、当前 rq 因
 * cookie 被迫 idle 且仍有 runnable task 时排入 per-CPU core balance callback。
 * @rq 已锁；重复排队由 callback 基础设施去重。
 */
static void queue_core_balance(struct rq *rq)
{
	if (!sched_core_enabled(rq))
		return;

	if (!rq->core->core_cookie)
		return;

	if (!rq->nr_running) /* not forced idle */
		return;

	queue_balance_callback(rq, &per_cpu(core_balance_head, rq->cpu), sched_core_balance);
}

DEFINE_LOCK_GUARD_1(core_lock, int,
		    sched_core_lock(*_T->lock, &_T->flags),
		    sched_core_unlock(*_T->lock, &_T->flags),
		    unsigned long flags)

/*
 * sched_core_cpu_starting()/deactivate()/dying() 维护 SMT sibling 的共享 core leader。
 * starting 在联合 core 锁下寻找现有 leader 并让新 sibling 指向它；deactivate
 * 若下线 CPU 是 leader，则复制序列/cookie/force-idle 状态到新 leader 后重定向
 * 所有 sibling；dying 最终把离线 rq 恢复自指。CPU hotplug 外层稳定 sibling mask。
 */
static void sched_core_cpu_starting(unsigned int cpu)
{
	const struct cpumask *smt_mask = cpu_smt_mask(cpu);
	struct rq *rq = cpu_rq(cpu), *core_rq = NULL;
	int t;

	guard(core_lock)(&cpu);

	WARN_ON_ONCE(rq->core != rq);

	/* if we're the first, we'll be our own leader */
	/* 中文：若是该 SMT core 首个 online CPU，保持 rq->core 自指即可。 */
	if (cpumask_weight(smt_mask) == 1)
		return;

	/* find the leader */
	/* 中文：从已 online sibling 中寻找当前自指的共享 leader。 */
	for_each_cpu(t, smt_mask) {
		if (t == cpu)
			continue;
		rq = cpu_rq(t);
		if (rq->core == rq) {
			core_rq = rq;
			break;
		}
	}

	if (WARN_ON_ONCE(!core_rq)) /* whoopsie */
		return;

	/* install and validate core_rq */
	for_each_cpu(t, smt_mask) {
		rq = cpu_rq(t);

		if (t == cpu)
			rq->core = core_rq;

		WARN_ON_ONCE(rq->core != core_rq);
	}
}

/*
 * sched_core_cpu_deactivate() 在 SMT leader 下线前选择新 leader，复制共享选择序列、
 * cookie 与 forced-idle 状态，再重定向 sibling。core_lock 与 hotplug 稳定拓扑。
 */
static void sched_core_cpu_deactivate(unsigned int cpu)
{
	const struct cpumask *smt_mask = cpu_smt_mask(cpu);
	struct rq *rq = cpu_rq(cpu), *core_rq = NULL;
	int t;

	guard(core_lock)(&cpu);

	/* if we're the last man standing, nothing to do */
	if (cpumask_weight(smt_mask) == 1) {
		WARN_ON_ONCE(rq->core != rq);
		return;
	}

	/* if we're not the leader, nothing to do */
	/* 中文：下线 CPU 不是 leader 时，共享状态无需迁移。 */
	if (rq->core != rq)
		return;

	/* find a new leader */
	/* 中文：选择任一剩余 sibling 接管 leader。 */
	for_each_cpu(t, smt_mask) {
		if (t == cpu)
			continue;
		core_rq = cpu_rq(t);
		break;
	}

	if (WARN_ON_ONCE(!core_rq)) /* impossible */
		return;

	/* copy the shared state to the new leader */
	/* 中文：在重定向 sibling 前复制所有共享序列、cookie 与 forced-idle 记账状态。 */
	core_rq->core_task_seq             = rq->core_task_seq;
	core_rq->core_pick_seq             = rq->core_pick_seq;
	core_rq->core_cookie               = rq->core_cookie;
	core_rq->core_forceidle_count      = rq->core_forceidle_count;
	core_rq->core_forceidle_seq        = rq->core_forceidle_seq;
	core_rq->core_forceidle_occupation = rq->core_forceidle_occupation;

	/*
	 * Accounting edge for forced idle is handled in pick_next_task().
	 * Don't need another one here, since the hotplug thread shouldn't
	 * have a cookie.
	 */
	core_rq->core_forceidle_start = 0;

	/* install new leader */
	for_each_cpu(t, smt_mask) {
		rq = cpu_rq(t);
		rq->core = core_rq;
	}
}

/*
 * sched_core_cpu_dying() 在 CPU 最终 dying 阶段把离线 rq->core 恢复自指；
 * 共享 leader 状态已于 deactivate 阶段迁走。
 */
static inline void sched_core_cpu_dying(unsigned int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	if (rq->core != rq)
		rq->core = rq;
}

#else /* !CONFIG_SCHED_CORE: */

/* 未启用 core scheduling 时 hotplug stub 无副作用，pick 直接走普通 class 选择。 */

static inline void sched_core_cpu_starting(unsigned int cpu) {}
static inline void sched_core_cpu_deactivate(unsigned int cpu) {}
static inline void sched_core_cpu_dying(unsigned int cpu) {}

static struct task_struct *
pick_next_task(struct rq *rq, struct rq_flags *rf)
	__must_hold(__rq_lockp(rq))
{
	return __pick_next_task(rq, rf);
}

#endif /* !CONFIG_SCHED_CORE */

/*
 * Constants for the sched_mode argument of __schedule().
 *
 * The mode argument allows RT enabled kernels to differentiate a
 * preemption from blocking on an 'sleeping' spin/rwlock.
 */
/*
 * 原文：sched_mode 让 PREEMPT_RT 区分普通抢占与在可睡眠 spin/rwlock 上等待。
 * SM_IDLE 是 idle 专用，SM_NONE 为自愿阻塞，SM_PREEMPT 为抢占，SM_RTLOCK_WAIT
 * 让 RCU/debug 记作抢占而不把锁等待误判成普通 task state 阻塞。
 */
#define SM_IDLE			(-1)
#define SM_NONE			0
#define SM_PREEMPT		1
#define SM_RTLOCK_WAIT		2

/*
 * Helper function for __schedule()
 *
 * Tries to deactivate the task, unless the should_block arg
 * is false or if a signal is pending. In the case a signal
 * is pending, marks the task's __state as RUNNING (and clear
 * blocked_on).
 */
/*
 * 原文：try_to_block_task() 在 __schedule 锁内尝试让 @p 阻塞。@task_state_p 是
 * 输入输出快照；pending signal 时恢复 RUNNING、清 blocked_on 并返回 false。
 * 否则先置 is_blocked；@should_block=false（proxy mutex donor）保留在 rq，
 * true 才 block_task 出队。返回 true 表示物理阻塞已提交。
 */
static bool try_to_block_task(struct rq *rq, struct task_struct *p,
			      unsigned long *task_state_p, bool should_block)
{
	unsigned long task_state = *task_state_p;

	WARN_ON_ONCE(p->is_blocked);

	if (signal_pending_state(task_state, p)) {
		WRITE_ONCE(p->__state, TASK_RUNNING);
		*task_state_p = TASK_RUNNING;
		clear_task_blocked_on(p, NULL);

		return false;
	}

	p->is_blocked = 1;

	/*
	 * We check should_block after signal_pending because we
	 * will want to wake the task in that case. But if
	 * should_block is false, its likely due to the task being
	 * blocked on a mutex, and we want to keep it on the runqueue
	 * to be selectable for proxy-execution.
	 */
	if (!should_block)
		return false;

	block_task(rq, p, task_state);
	return true;
}

#ifdef CONFIG_SCHED_PROXY_EXEC
/*
 * proxy_set_task_cpu() 允许 blocked scheduling context 暂时越过自身 affinity 迁到
 * lock owner CPU，但保存原 wake_cpu，真实唤醒时可回归合法执行 CPU。调用者持 rq
 * 锁并保证 @p blocked；无返回值。
 */
static inline void proxy_set_task_cpu(struct task_struct *p, int cpu)
{
	unsigned int wake_cpu;

	/*
	 * Since we are enqueuing a blocked task on a cpu it may
	 * not be able to run on, preserve wake_cpu when we
	 * __set_task_cpu so we can return the task to where it
	 * was previously runnable.
	 */
	wake_cpu = p->wake_cpu;
	__set_task_cpu(p, cpu);
	p->wake_cpu = wake_cpu;
}

/*
 * proxy_resched_idle() 在不能安全继续 donor chain 时把 next/donor 都切到 rq->idle，
 * 设置 resched 让锁竞争或迁移有机会完成。返回 idle 借用指针，@rq 已锁。
 */
static inline struct task_struct *proxy_resched_idle(struct rq *rq)
{
	put_prev_set_next_task(rq, rq->donor, rq->idle);
	rq->next_class = &idle_sched_class;
	rq_set_donor(rq, rq->idle);
	set_tsk_need_resched(rq->idle);
	return rq->idle;
}

/*
 * proxy_deactivate() 必须先让 rq 放弃所有 @donor 引用，再 block_task；一旦 on_rq
 * 清零，ttwu 可在另一 rq 立即唤醒，旧 rq 再触碰裸 donor 会 UAF/状态错乱。
 */
static void proxy_deactivate(struct rq *rq, struct task_struct *donor)
{
	unsigned long state = READ_ONCE(donor->__state);

	WARN_ON_ONCE(state == TASK_RUNNING);
	WARN_ON_ONCE(donor->blocked_on);
	/*
	 * Because we got donor from pick_next_task(), it is *crucial*
	 * that we call proxy_resched_idle() before we deactivate it.
	 * As once we deactivate donor, donor->on_rq is set to zero,
	 * which allows ttwu() to immediately try to wake the task on
	 * another rq. So we cannot use *any* references to donor
	 * after that point. So things like cfs_rq->curr or rq->donor
	 * need to be changed from next *before* we deactivate.
	 */
	proxy_resched_idle(rq);
	block_task(rq, donor, state);
}

/*
 * proxy_release_rq_lock() 在 proxy 路径临时释放 rq 前清除普通 balance callbacks，
 * 避免其他 CPU 进 balance 时撞上 pin 状态；稍后重新加锁会强制 pick_again 重建。
 */
static inline void proxy_release_rq_lock(struct rq *rq, struct rq_flags *rf)
	__releases(__rq_lockp(rq))
{
	/*
	 * The class scheduler may have queued a balance callback
	 * from pick_next_task() called earlier.
	 *
	 * So here we have to zap callbacks before unlocking the rq
	 * as another CPU may jump in and call sched_balance_rq
	 * which can trip the warning in rq_pin_lock() if we
	 * leave callbacks set.
	 *
	 * After we later reaquire the rq lock, we will force __schedule()
	 * to pick_again, so the callbacks will get re-established.
	 */
	/*
	 * 中文：此前 pick_next_task 可能排了 callback；解锁前必须清理，否则其他 CPU
	 * 进入 sched_balance_rq 会触发 rq_pin_lock 警告。重锁后的重新选取会再排队。
	 */
	zap_balance_callbacks(rq);
	rq_unpin_lock(rq, rf);
	raw_spin_rq_unlock(rq);
}

/*
 * proxy_release/reacquire_rq_lock() 包围 attach_one_task 的跨 rq 窗口。release 先
 * zap balance callback，unpin 后解锁；reacquire 重新锁、repin 并刷新 clock。
 * 调用者必须强制 __schedule pick_again 以重建被清掉的 callback/next 状态。
 */

static inline void proxy_reacquire_rq_lock(struct rq *rq, struct rq_flags *rf)
	__acquires(__rq_lockp(rq))
{
	raw_spin_rq_lock(rq);
	rq_repin_lock(rq, rf);
	update_rq_clock(rq);
}

/*
 * If the blocked-on relationship crosses CPUs, migrate @p to the
 * owner's CPU.
 *
 * This is because we must respect the CPU affinity of execution
 * contexts (owner) but we can ignore affinity for scheduling
 * contexts (@p). So we have to move scheduling contexts towards
 * potential execution contexts.
 *
 * Note: The owner can disappear, but simply migrate to @target_cpu
 * and leave that CPU to sort things out.
 */
/*
 * 原文：blocked_on 链跨 CPU 时，把 scheduling context @p 迁向 owner 的执行 CPU。
 * owner affinity 必须遵守，而 blocked donor 的 scheduling affinity 可暂时忽略。
 * owner 即使消失也可先迁到快照 target，由目标 CPU 重查。函数持源 rq 锁，先切
 * idle 清引用、deactivate/改 CPU，放源锁后 attach 目标，再重取源锁。
 */
static void proxy_migrate_task(struct rq *rq, struct rq_flags *rf,
			       struct task_struct *p, int target_cpu)
	__must_hold(__rq_lockp(rq))
{
	struct rq *target_rq = cpu_rq(target_cpu);

	lockdep_assert_rq_held(rq);
	WARN_ON(p == rq->curr);
	/*
	 * Since we are migrating a blocked donor, it could be rq->donor,
	 * and we want to make sure there aren't any references from this
	 * rq to it before we drop the lock. This avoids another cpu
	 * jumping in and grabbing the rq lock and referencing rq->donor
	 * or cfs_rq->curr, etc after we have migrated it to another cpu,
	 * and before we pick_again in __schedule.
	 *
	 * So call proxy_resched_idle() to drop the rq->donor references
	 * before we release the lock.
	 */
	proxy_resched_idle(rq);

	deactivate_task(rq, p, DEQUEUE_NOCLOCK);
	proxy_set_task_cpu(p, target_cpu);

	proxy_release_rq_lock(rq, rf);

	attach_one_task(target_rq, p);

	proxy_reacquire_rq_lock(rq, rf);
}

/*
 * Find runnable lock owner to proxy for mutex blocked donor
 *
 * Follow the blocked-on relation:
 *
 *                ,-> task
 *                |     | blocked-on
 *                |     v
 *  blocked_donor |   mutex
 *                |     | owner
 *                |     v
 *                `-- task
 *
 * and set the blocked_donor relation, this latter is used by the mutex
 * code to find which (blocked) task to hand-off to.
 *
 * Lock order:
 *
 *   p->pi_lock
 *     rq->lock
 *       mutex->wait_lock
 *         p->blocked_lock
 *
 * Returns the task that is going to be used as execution context (the one
 * that is actually going to be run on cpu_of(rq)).
 */
/*
 * 原文：沿 donor→blocked mutex→owner 链找到真正执行 context，并反向设置
 * owner->blocked_donor 供 mutex handoff。锁序固定为 p->pi_lock→rq→wait_lock
 * →blocked_lock。链变化、owner 迁移/延迟出队时返回 NULL 让 __schedule 重选；
 * 跨 CPU 则迁 donor，闭环/正在迁入则临时选 idle。返回 rq 锁保护的借用 owner。
 */
static struct task_struct *
find_proxy_task(struct rq *rq, struct task_struct *donor, struct rq_flags *rf)
	__must_hold(__rq_lockp(rq))
{
	struct task_struct *owner = NULL;
	bool curr_in_chain = false;
	int this_cpu = cpu_of(rq);
	struct task_struct *p;
	int owner_cpu;

	/* Follow blocked_on chain. */
	/* 中文：从 donor 开始逐层跟随 blocked_on mutex 的 owner 链。 */
	for (p = donor; p->is_blocked; p = owner) {
		/* if its PROXY_WAKING, do return migration or run if current */
	/* 中文：PROXY_WAKING 时要么让 current 直接运行，要么执行返回迁移。 */
		struct mutex *mutex = p->blocked_on;
		if (!mutex) {
			clear_task_blocked_on(p, mutex);
			if (task_current(rq, p)) {
				p->is_blocked = 0;
				return p;
			}
			goto deactivate;
		}

		/*
		 * By taking mutex->wait_lock we hold off concurrent mutex_unlock()
		 * and ensure @owner sticks around.
		 */
	/* 中文：wait_lock 阻挡并发 mutex_unlock，保证读取到的 owner 在本层检查期间稳定。 */
		guard(raw_spinlock)(&mutex->wait_lock);
		guard(raw_spinlock)(&p->blocked_lock);

		/* Check again that p is blocked with blocked_lock held */
	/* 中文：取得 blocked_lock 后重查 p 仍阻塞在同一 mutex，关闭链变化竞态。 */
		if (mutex != __get_task_blocked_on(p)) {
			/*
			 * Something changed in the blocked_on chain and
			 * we don't know if only at this level. So, let's
			 * just bail out completely and let __schedule()
			 * figure things out (pick_again loop).
			 */
	/*
	 * 中文：blocked_on 链已变化且无法确定只影响本层，放弃整个结果，让
	 * __schedule 的 pick_again 从新快照重建。
	 */
			return NULL;
		}

		if (task_current(rq, p))
			curr_in_chain = true;

		owner = __mutex_owner(mutex);
		if (!owner) {
			/*
			 * If there is no owner, either clear blocked_on
			 * and return p (if it is current and safe to
			 * just run on this rq), or return-migrate the task.
			 */
	/*
	 * 中文：mutex 已无 owner 时清 blocked_on；若 p 就是 current 可原地继续，
	 * 否则把 proxy 迁移的 scheduling context 返回合法 CPU。
	 */
			__clear_task_blocked_on(p, NULL);
			if (task_current(rq, p)) {
				p->is_blocked = 0;
				return p;
			}
			goto deactivate;
		}

		if (!READ_ONCE(owner->on_rq) || owner->se.sched_delayed) {
			/* XXX Don't handle blocked owners/delayed dequeue yet */
	/* 中文：当前尚不处理 owner 自身 blocked 或 delayed-dequeue，退化为 idle/重选。 */
			if (curr_in_chain)
				return proxy_resched_idle(rq);
			__clear_task_blocked_on(p, NULL);
			goto deactivate;
		}

		owner_cpu = task_cpu(owner);
		if (owner_cpu != this_cpu) {
			/*
			 * @owner can disappear, simply migrate to @owner_cpu
			 * and leave that CPU to sort things out.
			 */
	/* 中文：owner 可在锁外消失；先迁向 owner_cpu 快照，由目标 CPU 重新解析链即可。 */
			if (curr_in_chain)
				return proxy_resched_idle(rq);
			goto migrate_task;
		}

		if (task_on_rq_migrating(owner)) {
			/*
			 * One of the chain of mutex owners is currently migrating to this
			 * CPU, but has not yet been enqueued because we are holding the
			 * rq lock. As a simple solution, just schedule rq->idle to give
			 * the migration a chance to complete. Much like the migrate_task
			 * case we should end up back in find_proxy_task(), this time
			 * hopefully with all relevant tasks already enqueued.
			 */
	/*
	 * 中文：owner 正迁入本 CPU，但 rq 锁使它尚不能完成入队；临时运行 idle
	 * 释放 rq 锁，待迁移完成后下一轮 find_proxy_task 再解析。
	 */
			return proxy_resched_idle(rq);
		}

		/*
		 * Its possible to race where after we check owner->on_rq
		 * but before we check (owner_cpu != this_cpu) that the
		 * task on another cpu was migrated back to this cpu. In
		 * that case it could slip by our  checks. So double check
		 * we are still on this cpu and not migrating. If we get
		 * inconsistent results, try again.
		 */
	/*
	 * 中文：on_rq 与 owner_cpu 两次无锁读取间，owner 可能迁回本 CPU；因此再
	 * 联合复核 queued、CPU 与 MIGRATING，结果不一致就整轮重试。
	 */
		if (!task_on_rq_queued(owner) || task_cpu(owner) != this_cpu)
			return NULL;

		if (owner == p) {
			/*
			 * It's possible we interleave with mutex_unlock like:
			 *
			 *				lock(&rq->lock);
			 *				  find_proxy_task()
			 * mutex_unlock()
			 *   lock(&wait_lock);
			 *   donor(owner) = current->blocked_donor;
			 *   unlock(&wait_lock);
			 *
			 *   wake_up_q();
			 *     ...
			 *       ttwu_runnable()
			 *         __task_rq_lock()
			 *				  lock(&wait_lock);
			 *				  owner == p
			 *
			 * Which leaves us to finish the ttwu_runnable() and make it go.
			 *
			 * So schedule rq->idle so that ttwu_runnable() can get the rq
			 * lock and mark owner as running.
			 */
	/*
	 * 中文：mutex_unlock 与查链可交错形成 owner==p 的短暂闭环，同时
	 * ttwu_runnable 正等待 rq 锁。选 idle 释放锁，让唤醒完成并把 owner 标 RUNNING。
	 */
			return proxy_resched_idle(rq);
		}
		/*
		 * OK, now we're absolutely sure @owner is on this
		 * rq, therefore holding @rq->lock is sufficient to
		 * guarantee its existence, as per ttwu_remote().
		 */
	/*
	 * 中文：确认 owner 已稳定在本 rq 后，rq 锁足以保证其生命周期和队列成员关系；
	 * 可安全发布反向 blocked_donor 链接。
	 */
		owner->blocked_donor = p;
	}
	WARN_ON_ONCE(owner && !owner->on_rq);
	return owner;

deactivate:
	proxy_deactivate(rq, p);
	return NULL;
migrate_task:
	proxy_migrate_task(rq, rf, p, owner_cpu);
	return NULL;
}
#else /* SCHED_PROXY_EXEC */
static struct task_struct *
find_proxy_task(struct rq *rq, struct task_struct *donor, struct rq_flags *rf)
{
	WARN_ONCE(1, "This should never be called in the !SCHED_PROXY_EXEC case\n");
	return donor;
}
#endif /* SCHED_PROXY_EXEC */

/*
 * __schedule() is the main scheduler function.
 *
 * The main means of driving the scheduler and thus entering this function are:
 *
 *   1. Explicit blocking: mutex, semaphore, waitqueue, etc.
 *
 *   2. TIF_NEED_RESCHED flag is checked on interrupt and userspace return
 *      paths. For example, see arch/x86/entry_64.S.
 *
 *      To drive preemption between tasks, the scheduler sets the flag in timer
 *      interrupt handler sched_tick().
 *
 *   3. Wakeups don't really cause entry into schedule(). They add a
 *      task to the run-queue and that's it.
 *
 *      Now, if the new task added to the run-queue preempts the current
 *      task, then the wakeup sets TIF_NEED_RESCHED and schedule() gets
 *      called on the nearest possible occasion:
 *
 *       - If the kernel is preemptible (CONFIG_PREEMPTION=y):
 *
 *         - in syscall or exception context, at the next outmost
 *           preempt_enable(). (this might be as soon as the wake_up()'s
 *           spin_unlock()!)
 *
 *         - in IRQ context, return from interrupt-handler to
 *           preemptible context
 *
 *       - If the kernel is not preemptible (CONFIG_PREEMPTION is not set)
 *         then at the next:
 *
 *          - cond_resched() call
 *          - explicit schedule() call
 *          - return from syscall or exception to user-space
 *          - return from interrupt-handler to user-space
 *
 * WARNING: must be called with preemption disabled!
 */
/*
 * 原文主旨：__schedule() 是统一调度状态机。阻塞、tick/唤醒设置的 NEED_RESCHED、
 * syscall/IRQ 返回和 cond_resched 最终都汇入这里；唤醒本身只入队，是否立即切换
 * 由重调度标志和当前抢占模型决定。入口必须已禁抢占。
 *
 * @sched_mode 区分自愿调度、内核抢占和 RT-lock 等模式；无直接返回值，但可能
 * 换 task、mm 和执行栈。核心阶段：
 * 1. 定位本 CPU rq/curr，完成 atomic-sleep、栈和 RCU 调试检查；
 * 2. 关 IRQ 并锁 rq，更新时钟；若 prev 主动设置阻塞 state，和并发唤醒通过
 *    状态/屏障协议裁决是 block_task() 出队，还是唤醒已胜出而继续 runnable；
 * 3. 处理 worker/io-wait/PSI 记账，经 pick_next_task() 让各 sched_class 或
 *    core-cookie 联合选择 next，并清本轮重调度请求；
 * 4. prev != next 时递增切换计数、prepare_task_switch()，由 context_switch()
 *    切 mm 和体系结构寄存器；新栈上的 finish_task_switch() 接管 rq 锁与清理。
 *    相同 task 则只解锁并执行 balance callback。
 *
 * rq->lock 从 prev 跨 context switch 交给 next，故普通函数式“谁加锁谁解锁”在
 * 这里表现为跨 task 配对。返回只保证本轮请求已处理；期间若又产生 NEED_RESCHED，
 * 外层 schedule loop 会再次进入。
 */
static void __sched notrace __schedule(int sched_mode)
{
	struct task_struct *prev, *next;
	/*
	 * On PREEMPT_RT kernel, SM_RTLOCK_WAIT is noted
	 * as a preemption by schedule_debug() and RCU.
	 */
	bool preempt = sched_mode > SM_NONE;
	bool is_switch = false;
	unsigned long *switch_count;
	unsigned long prev_state;
	struct rq_flags rf;
	struct rq *rq;
	int cpu;

	/* Trace preemptions consistently with task switches */
	/* 中文：只有真正 SM_PREEMPT 才按抢占事件 trace，保持与 switch 语义一致。 */
	trace_sched_entry_tp(sched_mode == SM_PREEMPT);

	cpu = smp_processor_id();
	rq = cpu_rq(cpu);
	prev = rq->curr;

	schedule_debug(prev, preempt);

	klp_sched_try_switch(prev);

	local_irq_disable();
	rcu_note_context_switch(preempt);
	migrate_disable_switch(rq, prev);

	/*
	 * Make sure that signal_pending_state()->signal_pending() below
	 * can't be reordered with __set_current_state(TASK_INTERRUPTIBLE)
	 * done by the caller to avoid the race with signal_wake_up():
	 *
	 * __set_current_state(@state)		signal_wake_up()
	 * schedule()				  set_tsk_thread_flag(p, TIF_SIGPENDING)
	 *					  wake_up_state(p, state)
	 *   LOCK rq->lock			    LOCK p->pi_state
	 *   smp_mb__after_spinlock()		    smp_mb__after_spinlock()
	 *     if (signal_pending_state())	    if (p->state & @state)
	 *
	 * Also, the membarrier system call requires a full memory barrier
	 * after coming from user-space, before storing to rq->curr; this
	 * barrier matches a full barrier in the proximity of the membarrier
	 * system call exit.
	 */
	/*
	 * 中文：rq 锁后的全屏障同时解决两件事：与 signal_wake_up 的 pi_lock 屏障
	 * 配对，避免 signal/state 互相错过；并满足 membarrier 从用户态进入后、
	 * 发布 rq->curr 前的完整排序要求。
	 */
	rq_lock(rq, &rf);
	smp_mb__after_spinlock();

	hrtick_schedule_enter(rq);

	/* Promote REQ to ACT */
	/* 中文：把 rq clock 的“请求跳过”位提升为本轮 active 状态后再统一更新。 */
	rq->clock_update_flags <<= 1;
	update_rq_clock(rq);
	rq->clock_update_flags = RQCF_UPDATED;

	switch_count = &prev->nivcsw;

	/* Task state changes only considers SM_PREEMPT as preemption */
	/* 中文：task state 裁决仅把 SM_PREEMPT 视为真正抢占，其他模式仍可能主动阻塞。 */
	preempt = sched_mode == SM_PREEMPT;

	/*
	 * We must load prev->state once (task_struct::state is volatile), such
	 * that we form a control dependency vs deactivate_task() below.
	 */
	/* 中文：只读取一次 volatile state，以此形成到后续 deactivate/block 的控制依赖。 */
	prev_state = READ_ONCE(prev->__state);
	if (sched_mode == SM_IDLE) {
		/* SCX must consult the BPF scheduler to tell if rq is empty */
	/* 中文：SCX 启用时即使 nr_running 为 0，也必须询问 BPF scheduler 是否有任务。 */
		if (!rq->nr_running && !scx_enabled()) {
			next = prev;
			rq->next_class = &idle_sched_class;
			goto picked;
		}
	} else if (!preempt && prev_state) {
		/*
		 * We pass task_is_blocked() as the should_block arg
		 * in order to keep mutex-blocked tasks on the runqueue
		 * for slection with proxy-exec (without proxy-exec
		 * task_is_blocked() will always be false).
		 */
	/*
	 * 中文：proxy-exec 要把 mutex-blocked donor 保留在 rq 作为 scheduling context，
	 * 因而 should_block 取 !task_is_blocked；未启用 proxy 时该谓词恒假，行为退回
	 * 普通睡眠出队。
	 */
		try_to_block_task(rq, prev, &prev_state,
				  !task_is_blocked(prev));
		switch_count = &prev->nvcsw;
	}

pick_again:
	assert_balance_callbacks_empty(rq);
	next = pick_next_task(rq, &rf);
	rq->next_class = next->sched_class;
	if (sched_proxy_exec()) {
		struct task_struct *prev_donor = rq->donor;

		rq_set_donor(rq, next);
		next->blocked_donor = NULL;
		if (unlikely(next->is_blocked)) {
			next = find_proxy_task(rq, next, &rf);
			if (!next) {
				zap_balance_callbacks(rq);
				goto pick_again;
			}
			if (next == rq->idle) {
				zap_balance_callbacks(rq);
				goto keep_resched;
			}
		}
		if (rq->donor == prev_donor && prev != next) {
			struct task_struct *donor = rq->donor;
			/*
			 * When transitioning like:
			 *
			 *         prev         next
			 * donor:    B            B
			 * curr:     A          B or C
			 *
			 * then put_prev_set_next_task() will not have done
			 * anything, since B == B. However, A might have
			 * missed a RT/DL balance opportunity due to being
			 * on_cpu.
			 */
	/*
	 * 中文：donor B 未变但执行 context 从 A 换到 B/C 时，通用
	 * put_prev_set_next 因 B==B 不动作；A 之前因 on_cpu 可能错过 RT/DL balance，
	 * 故显式重放 donor class 的 put/set 回调。
	 */
			donor->sched_class->put_prev_task(rq, donor, donor);
			donor->sched_class->set_next_task(rq, donor, true);
		}
	} else {
		rq_set_donor(rq, next);
	}

picked:
	clear_tsk_need_resched(prev);
	clear_preempt_need_resched();
keep_resched:
	rq->last_seen_need_resched_ns = 0;

	is_switch = prev != next;
	if (likely(is_switch)) {
		rq->nr_switches++;
		/*
		 * RCU users of rcu_dereference(rq->curr) may not see
		 * changes to task_struct made by pick_next_task().
		 */
	/*
	 * 中文：RCU 读者只保证看到 curr 指针本身，不保证同步看到 pick_next_task
	 * 对 task_struct 的其他修改；这里按该弱发布语义更新指针。
	 */
		RCU_INIT_POINTER(rq->curr, next);

		/*
		 * The membarrier system call requires each architecture
		 * to have a full memory barrier after updating
		 * rq->curr, before returning to user-space.
		 *
		 * Here are the schemes providing that barrier on the
		 * various architectures:
		 * - mm ? switch_mm() : mmdrop() for x86, s390, sparc, PowerPC,
		 *   RISC-V.  switch_mm() relies on membarrier_arch_switch_mm()
		 *   on PowerPC and on RISC-V.
		 * - finish_lock_switch() for weakly-ordered
		 *   architectures where spin_unlock is a full barrier,
		 * - switch_to() for arm64 (weakly-ordered, spin_unlock
		 *   is a RELEASE barrier),
		 *
		 * The barrier matches a full barrier in the proximity of
		 * the membarrier system call entry.
		 *
		 * On RISC-V, this barrier pairing is also needed for the
		 * SYNC_CORE command when switching between processes, cf.
		 * the inline comments in membarrier_arch_switch_mm().
		 */
	/*
	 * 中文：更新 rq->curr 后、返回用户态前必须有完整屏障。各架构分别由
	 * switch_mm/mmdrop、full-barrier spin_unlock 或 switch_to 提供；它与
	 * membarrier syscall 入口附近屏障配对，RISC-V 的 SYNC_CORE 也依赖此序列。
	 */
		++*switch_count;

		psi_account_irqtime(rq, prev, next);
		psi_sched_switch(prev, next, !task_on_rq_queued(prev) ||
					     prev->se.sched_delayed);

		trace_sched_switch(preempt, prev, next, prev_state);

		/* Also unlocks the rq: */
		rq = context_switch(rq, prev, next, &rf);
	} else {
		rq_unpin_lock(rq, &rf);
		__balance_callbacks(rq, NULL);
		hrtick_schedule_exit(rq);
		raw_spin_rq_unlock_irq(rq);
	}
	trace_sched_exit_tp(is_switch);
}

/*
 * do_task_dead() - current 的最终调度出口，永不返回。
 *
 * 先以 special-state 协议发布 TASK_DEAD，并置 PF_NOFREEZE，随后最后一次自愿
 * __schedule。next 栈上的 finish_task_switch() 观察 DEAD 后释放 current 引用和
 * 栈；若异常返回则 BUG 并永久 cpu_relax，绝不能继续使用已交出生命周期的 task。
 */
void __noreturn do_task_dead(void)
{
	/* Causes final put_task_struct in finish_task_switch(): */
	set_special_state(TASK_DEAD);

	/* Tell freezer to ignore us: */
	current->flags |= PF_NOFREEZE;

	__schedule(SM_NONE);
	BUG();

	/* Avoid "noreturn function does return" - but don't continue if BUG() is a NOP: */
	for (;;)
		cpu_relax();
}

/*
 * sched_submit_work() 在 @tsk 真正睡眠前提交其外围工作：通知 workqueue/io-wq
 * 补充并发，并 flush blk plug，防止持有尚未发出的 I/O 等待自身完成。LD_WAIT_
 * CONFIG override 禁止这些回调再次使用阻塞原语造成 schedule 递归；RT-lock wait
 * 不允许 flush。无返回值，可睡眠语义由调用 schedule 的进程上下文承担。
 */
static inline void sched_submit_work(struct task_struct *tsk)
{
	static DEFINE_WAIT_OVERRIDE_MAP(sched_map, LD_WAIT_CONFIG);
	unsigned int task_flags;

	/*
	 * Establish LD_WAIT_CONFIG context to ensure none of the code called
	 * will use a blocking primitive -- which would lead to recursion.
	 */
	lock_map_acquire_try(&sched_map);

	task_flags = tsk->flags;
	/*
	 * If a worker goes to sleep, notify and ask workqueue whether it
	 * wants to wake up a task to maintain concurrency.
	 */
	/* 中文：worker 睡眠前通知所属池，必要时唤醒替代 worker 维持并发度。 */
	if (task_flags & PF_WQ_WORKER)
		wq_worker_sleeping(tsk);
	else if (task_flags & PF_IO_WORKER)
		io_wq_worker_sleeping(tsk);

	/*
	 * spinlock and rwlock must not flush block requests.  This will
	 * deadlock if the callback attempts to acquire a lock which is
	 * already acquired.
	 */
	/*
	 * 中文：RT spin/rwlock 等待路径不能 flush block plug；回调若重取当前已持锁
	 * 会自死锁，因此以 TASK_RTLOCK_WAIT 告警这种非法组合。
	 */
	WARN_ON_ONCE(current->__state & TASK_RTLOCK_WAIT);

	/*
	 * If we are going to sleep and we have plugged IO queued,
	 * make sure to submit it to avoid deadlocks.
	 */
	blk_flush_plug(tsk->plug, true);

	lock_map_release(&sched_map);
}

/*
 * sched_update_worker() 在 schedule 返回后通知 WQ/IO worker 已重新运行，和
 * sched_submit_work 的 sleeping 通知配对。普通 task 无动作；无返回值。
 */
static void sched_update_worker(struct task_struct *tsk)
{
	if (tsk->flags & (PF_WQ_WORKER | PF_IO_WORKER)) {
		if (tsk->flags & PF_WQ_WORKER)
			wq_worker_running(tsk);
		else
			io_wq_worker_running(tsk);
	}
}

/*
 * __schedule_loop() 每轮禁抢占进入 __schedule，返回后只降低计数但不立即再次调度；
 * 若窗口内又置 need_resched 就重来。@sched_mode 原样传递；返回时无 pending
 * resched，抢占状态恢复到调用前约定。
 */
static __always_inline void __schedule_loop(int sched_mode)
{
	do {
		preempt_disable();
		__schedule(sched_mode);
		sched_preempt_enable_no_resched();
	} while (need_resched());
}

/*
 * schedule() - 进程上下文的公共自愿调度入口。
 *
 * 无入参/返回值；目标恒为 current。若 current 非 RUNNING，先提交 worker/I/O
 * work，再以 SM_NONE 循环调度，返回后恢复 worker running 记账。可睡眠并切换
 * 任意多次；RT mutex 专用路径不得误入普通 schedule。
 */
asmlinkage __visible void __sched schedule(void)
{
	struct task_struct *tsk = current;

#ifdef CONFIG_RT_MUTEXES
	lockdep_assert(!tsk->sched_rt_mutex);
#endif

	if (!task_is_running(tsk))
		sched_submit_work(tsk);
	__schedule_loop(SM_NONE);
	sched_update_worker(tsk);
}
EXPORT_SYMBOL(schedule);

/*
 * synchronize_rcu_tasks() makes sure that no task is stuck in preempted
 * state (have scheduled out non-voluntarily) by making sure that all
 * tasks have either left the run queue or have gone into user space.
 * As idle tasks do not do either, they must not ever be preempted
 * (schedule out non-voluntarily).
 *
 * schedule_idle() is similar to schedule_preempt_disable() except that it
 * never enables preemption because it does not call sched_submit_work().
 */
/*
 * 原文：RCU Tasks 要求非自愿切出的 task 最终离开 rq 或进用户态，idle 两者都不
 * 做，所以 idle 绝不能被普通抢占。schedule_idle 类似 preempt-disabled 调度，
 * 但跳过 submit_work 且始终保持不可抢占；要求 current state 恒为 RUNNING。
 */
void __sched schedule_idle(void)
{
	/*
	 * As this skips calling sched_submit_work(), which the idle task does
	 * regardless because that function is a NOP when the task is in a
	 * TASK_RUNNING state, make sure this isn't used someplace that the
	 * current task can be in any other state. Note, idle is always in the
	 * TASK_RUNNING state.
	 */
	WARN_ON_ONCE(current->__state);
	do {
		__schedule(SM_IDLE);
	} while (need_resched());
}

#if defined(CONFIG_CONTEXT_TRACKING_USER) && !defined(CONFIG_HAVE_CONTEXT_TRACKING_USER_OFFSTACK)
/*
 * schedule_user() - 为仍被记录为 USER/EQS 的特殊调度入口临时恢复内核状态。
 *
 * 入参、返回均无。仅 USER Context Tracking 且架构未在独立入口栈处理状态时
 * 编译。调用 exception_enter() 保存并退出原 USER 状态，schedule() 可睡眠
 * 并切换任务，返回后 exception_exit() 按令牌恢复；无对象 ownership 转移。
 */
asmlinkage __visible void __sched schedule_user(void)
{
	/*
	 * If we come here after a random call to set_need_resched(),
	 * or we have been woken up remotely but the IPI has not yet arrived,
	 * we haven't yet exited the RCU idle mode. Do it here manually until
	 * we find a better solution.
	 *
	 * NB: There are buggy callers of this function.  Ideally we
	 * should warn if prev_state != CT_STATE_USER, but that will trigger
	 * too frequently to make sense yet.
	 */
	/*
	 * 原文说明随机 set_need_resched() 或远端唤醒但 IPI 尚未到达时，CPU 可能
	 * 尚未退出 RCU idle 就进入这里，因此暂时手工恢复 watching。
	 * 原文也承认存在错误调用者；理想上 prev_state 非 USER 应告警，
	 * 但当前会过于频繁。
	 */
	/* prev_state 是跨 schedule 保存的按值恢复令牌，不持有 task 引用。 */
	enum ctx_state prev_state = exception_enter();
	/* RCU 已 watching 后才允许调度器执行其普通读侧工作。 */
	schedule();
	/*
	 * 调度返回后恢复入口上下文；
	 * 令牌允许任务迁移后的当前 CPU 正确落回状态。
	 */
	exception_exit(prev_state);
}
#endif

/**
 * schedule_preempt_disabled - called with preemption disabled
 *
 * Returns with preemption disabled. Note: preempt_count must be 1
 */
/*
 * 原文：入口 preempt_count 必须恰为 1，函数临时降到可由 schedule 管理的状态，
 * 调度返回后再次禁抢占，因此出口仍为 1。无直接返回值。
 */
void __sched schedule_preempt_disabled(void)
{
	sched_preempt_enable_no_resched();
	schedule();
	preempt_disable();
}

#ifdef CONFIG_PREEMPT_RT
/*
 * schedule_rtlock() 是 PREEMPT_RT sleeping lock 等待入口，以 SM_RTLOCK_WAIT 循环；
 * 不走普通 worker/blk plug 提交流程，且 notrace 防止锁/trace 递归。
 */
void __sched notrace schedule_rtlock(void)
{
	__schedule_loop(SM_RTLOCK_WAIT);
}
NOKPROBE_SYMBOL(schedule_rtlock);
#endif

/*
 * preempt_schedule_common() 处理内核抢占。先用 notrace 操作禁抢占，避免 tracer
 * 在 preempt_enable_notrace 上递归，再单独记录 latency，调用 SM_PREEMPT；
 * 返回窗口若又出现 need_resched 则循环。无入参/返回值。
 */
static void __sched notrace preempt_schedule_common(void)
{
	do {
		/*
		 * Because the function tracer can trace preempt_count_sub()
		 * and it also uses preempt_enable/disable_notrace(), if
		 * NEED_RESCHED is set, the preempt_enable_notrace() called
		 * by the function tracer will call this function again and
		 * cause infinite recursion.
		 *
		 * Preemption must be disabled here before the function
		 * tracer can trace. Break up preempt_disable() into two
		 * calls. One to disable preemption without fear of being
		 * traced. The other to still record the preemption latency,
		 * which can also be traced by the function tracer.
		 */
		preempt_disable_notrace();
		preempt_latency_start(1);
		__schedule(SM_PREEMPT);
		preempt_latency_stop(1);
		preempt_enable_no_resched_notrace();

		/*
		 * Check again in case we missed a preemption opportunity
		 * between schedule and now.
		 */
	} while (need_resched());
}

#ifdef CONFIG_PREEMPTION
/*
 * This is the entry point to schedule() from in-kernel preemption
 * off of preempt_enable.
 */
/*
 * 原文：preempt_enable 发现 need_resched 后进入这里。若 preempt_count 非零或
 * IRQ 关闭，当前点不可抢占而直接返回；否则调用 common 循环。notrace/NOKPROBE
 * 防止观测基础设施递归进入调度器。
 */
asmlinkage __visible void __sched notrace preempt_schedule(void)
{
	/*
	 * If there is a non-zero preempt_count or interrupts are disabled,
	 * we do not want to preempt the current task. Just return..
	 */
	if (likely(!preemptible()))
		return;
	preempt_schedule_common();
}
NOKPROBE_SYMBOL(preempt_schedule);
EXPORT_SYMBOL(preempt_schedule);

#ifdef CONFIG_PREEMPT_DYNAMIC
# ifdef CONFIG_HAVE_PREEMPT_DYNAMIC_CALL
#  ifndef preempt_schedule_dynamic_enabled
#   define preempt_schedule_dynamic_enabled	preempt_schedule
#   define preempt_schedule_dynamic_disabled	NULL
#  endif
DEFINE_STATIC_CALL(preempt_schedule, preempt_schedule_dynamic_enabled);
EXPORT_STATIC_CALL_TRAMP(preempt_schedule);
# elif defined(CONFIG_HAVE_PREEMPT_DYNAMIC_KEY)
static DEFINE_STATIC_KEY_TRUE(sk_dynamic_preempt_schedule);
/*
 * dynamic_preempt_schedule() 是 static-key 架构的运行期门：模型关闭时立即返回，
 * 开启时进入 preempt_schedule。static-call 架构则直接改写调用目标，无此 wrapper。
 */
void __sched notrace dynamic_preempt_schedule(void)
{
	if (!static_branch_unlikely(&sk_dynamic_preempt_schedule))
		return;
	preempt_schedule();
}
NOKPROBE_SYMBOL(dynamic_preempt_schedule);
EXPORT_SYMBOL(dynamic_preempt_schedule);
# endif
#endif /* CONFIG_PREEMPT_DYNAMIC */

/**
 * preempt_schedule_notrace - preempt_schedule called by tracing
 *
 * The tracing infrastructure uses preempt_enable_notrace to prevent
 * recursion and tracing preempt enabling caused by the tracing
 * infrastructure itself. But as tracing can happen in areas coming
 * from userspace or just about to enter userspace, a preempt enable
 * can occur before user_exit() is called. This will cause the scheduler
 * to be called when the system is still in usermode.
 *
 * To prevent this, the preempt_enable_notrace will use this function
 * instead of preempt_schedule() to exit user context if needed before
 * calling the scheduler.
 */
/*
 * preempt_schedule_notrace() - tracing 触发抢占时先修复可能残留的 USER 状态。
 *
 * 原文说明 tracing 用 preempt_enable_notrace 防递归，但 tracing 可能发生在
 * 用户入口/出口附近，使抢占早于 user_exit()，调度器遂在仍标记 USER/EQS 时
 * 运行。本接口替代普通 preempt_schedule()，必要时先退出用户上下文。
 * 入参、返回均无；不可抢占或 IRQ-off 时直接返回。循环中可切换任务；
 * prev_ctx 只保存状态值，无 ownership。
 */
asmlinkage __visible void __sched notrace preempt_schedule_notrace(void)
{
	/* prev_ctx 是每轮调度前由 exception_enter() 返回的恢复令牌。 */
	enum ctx_state prev_ctx;

	/* 非零 preempt_count 或 IRQ-off 表示当前点不允许内核抢占。 */
	if (likely(!preemptible()))
		return;

	do {
		/*
		 * Because the function tracer can trace preempt_count_sub()
		 * and it also uses preempt_enable/disable_notrace(), if
		 * NEED_RESCHED is set, the preempt_enable_notrace() called
		 * by the function tracer will call this function again and
		 * cause infinite recursion.
		 *
		 * Preemption must be disabled here before the function
		 * tracer can trace. Break up preempt_disable() into two
		 * calls. One to disable preemption without fear of being
		 * traced. The other to still record the preemption latency,
		 * which can also be traced by the function tracer.
		 */
		/*
		 * 原文说明 tracer 本身会跟踪 preempt_count_sub()，又会调用 notrace
		 * 抢占开关；若 NEED_RESCHED 已置位，递归可无限发生。
		 * 因此先用不插桩
		 * 操作禁抢占，再单独开始仍可被跟踪的抢占延迟记账。
		 */
		preempt_disable_notrace();
		preempt_latency_start(1);
		/*
		 * Needs preempt disabled in case user_exit() is traced
		 * and the tracer calls preempt_enable_notrace() causing
		 * an infinite recursion.
		 */
		/*
		 * 原文要求 exception_enter() 期间保持抢占关闭：若 user_exit() 被
		 * tracing，而 tracer 再次 preempt_enable_notrace()，否则会递归。
		 */
		prev_ctx = exception_enter();
		/*
		 * 此时已是 KERNEL/watching，可进入真正调度；
		 * 返回后按令牌恢复上下文。
		 */
		__schedule(SM_PREEMPT);
		exception_exit(prev_ctx);

		preempt_latency_stop(1);
		preempt_enable_no_resched_notrace();
		/*
		 * 若调度窗口内再次产生 NEED_RESCHED，
		 * 循环重新建立完整状态边界。
		 */
	} while (need_resched());
}
EXPORT_SYMBOL_GPL(preempt_schedule_notrace);

#ifdef CONFIG_PREEMPT_DYNAMIC
# if defined(CONFIG_HAVE_PREEMPT_DYNAMIC_CALL)
#  ifndef preempt_schedule_notrace_dynamic_enabled
#   define preempt_schedule_notrace_dynamic_enabled	preempt_schedule_notrace
#   define preempt_schedule_notrace_dynamic_disabled	NULL
#  endif
DEFINE_STATIC_CALL(preempt_schedule_notrace, preempt_schedule_notrace_dynamic_enabled);
EXPORT_STATIC_CALL_TRAMP(preempt_schedule_notrace);
# elif defined(CONFIG_HAVE_PREEMPT_DYNAMIC_KEY)
static DEFINE_STATIC_KEY_TRUE(sk_dynamic_preempt_schedule_notrace);
/*
 * dynamic_preempt_schedule_notrace() 对 tracing 专用抢占入口执行相同运行期门控。
 */
void __sched notrace dynamic_preempt_schedule_notrace(void)
{
	if (!static_branch_unlikely(&sk_dynamic_preempt_schedule_notrace))
		return;
	preempt_schedule_notrace();
}
NOKPROBE_SYMBOL(dynamic_preempt_schedule_notrace);
EXPORT_SYMBOL(dynamic_preempt_schedule_notrace);
# endif
#endif

#endif /* CONFIG_PREEMPTION */

/*
 * This is the entry point to schedule() from kernel preemption
 * off of IRQ context.
 * Note, that this is called and return with IRQs disabled. This will
 * protect us against recursive calling from IRQ contexts.
 */
/*
 * preempt_schedule_irq() - IRQ 退出路径在 IRQ-off 条件下执行内核抢占。
 *
 * 原文说明这是 IRQ context 触发 schedule() 的入口，调用与返回均保持 IRQ
 * 关闭，以阻止 IRQ 上下文递归。入参、返回均无；要求 preempt_count 为 0。
 * exception_enter/exit 保护可能残留的 USER/EQS 状态；循环中临时开 IRQ并可
 * 切换任务，最终恢复入口 Context Tracking 状态，无 ownership。
 */
asmlinkage __visible void __sched preempt_schedule_irq(void)
{
	/* prev_state 是跨整个重调度循环保存的状态令牌。 */
	enum ctx_state prev_state;

	/* Catch callers which need to be fixed */
	/*
	 * 原文：捕获必须修复的调用者；
	 * 持抢占计数或 IRQ-on 均违反入口协议。
	 */
	BUG_ON(preempt_count() || !irqs_disabled());

	/* 在调度器使用 RCU 前退出潜在 USER/EQS，并保存恢复目标。 */
	prev_state = exception_enter();

	/*
	 * 每轮先禁抢占再临时开 IRQ，让调度过程可处理中断；
	 * 返回后关 IRQ 并恢复
	 * preempt_count。need_resched 仍为真则继续，出口始终 IRQ-off。
	 */
	do {
		preempt_disable();
		local_irq_enable();
		__schedule(SM_PREEMPT);
		local_irq_disable();
		sched_preempt_enable_no_resched();
	} while (need_resched());

	/* 所有调度完成后才恢复入口上下文，避免循环中间过早重入 EQS。 */
	exception_exit(prev_state);
}

/*
 * default_wake_function() 是 waitqueue 默认回调：@curr->private 借用保存目标 task，
 * @mode 是可唤醒 state mask，@wake_flags 只接受 WF_SYNC/WF_CURRENT_CPU，@key
 * 未使用。返回 try_to_wake_up 的 1/0，不消费 wait entry。
 */
int default_wake_function(wait_queue_entry_t *curr, unsigned mode, int wake_flags,
			  void *key)
{
	WARN_ON_ONCE(wake_flags & ~(WF_SYNC|WF_CURRENT_CPU));
	return try_to_wake_up(curr->private, mode, wake_flags);
}
EXPORT_SYMBOL(default_wake_function);

/*
 * __setscheduler_class() 按有效 @prio 优先选择 DL/RT，再按 @policy 选择 sched_ext，
 * 否则 fair。返回全局永久 sched_class 操作表的借用指针，无失败或副作用。
 */
const struct sched_class *__setscheduler_class(int policy, int prio)
{
	if (dl_prio(prio))
		return &dl_sched_class;

	if (rt_prio(prio))
		return &rt_sched_class;

#ifdef CONFIG_SCHED_CLASS_EXT
	if (task_should_scx(policy))
		return &ext_sched_class;
#endif

	return &fair_sched_class;
}

#ifdef CONFIG_RT_MUTEXES

/*
 * Would be more useful with typeof()/auto_type but they don't mix with
 * bit-fields. Since it's a local thing, use int. Keep the generic sounding
 * name such that if someone were to implement this function we get to compare
 * notes.
 */
/*
 * 原文：fetch_and_set 返回 bit-field 旧 int 值并写新值；typeof/auto_type 对位域
 * 不适用，局部宏故意用 int。这里只用于 sched_rt_mutex 状态断言，不是原子操作，
 * 正确性依赖 current 单线程执行。
 */
#define fetch_and_set(x, v) ({ int _x = (x); (x) = (v); _x; })

/*
 * rt_mutex_pre/schedule/post_schedule() 把 sleeping rt_mutex 调度分成三段：
 * pre 标记防普通 schedule 并提交 worker/I/O；schedule 以 SM_NONE 循环；post
 * 恢复 worker 记账并清标志。仅 current 使用，无返回值，必须严格成对。
 */
void rt_mutex_pre_schedule(void)
{
	lockdep_assert(!fetch_and_set(current->sched_rt_mutex, 1));
	sched_submit_work(current);
}

void rt_mutex_schedule(void)
{
	lockdep_assert(current->sched_rt_mutex);
	__schedule_loop(SM_NONE);
}

void rt_mutex_post_schedule(void)
{
	sched_update_worker(current);
	lockdep_assert(fetch_and_set(current->sched_rt_mutex, 0));
}

/*
 * rt_mutex_setprio - set the current priority of a task
 * @p: task to boost
 * @pi_task: donor task
 *
 * This function changes the 'effective' priority of a task. It does
 * not touch ->normal_prio like __setscheduler().
 *
 * Used by the rt_mutex code to implement priority inheritance
 * logic. Call site only calls if the priority of the task changed.
 */
/*
 * 原文：按 @pi_task donor 改变 @p 的有效优先级，不改变 normal_prio；rt_mutex 只
 * 在计算结果变化时调用。调用者持 p->pi_lock，函数再锁 rq，稳定 pi_top_task，
 * 用 sched_change scope 在 queued/running 时撤出旧 class、更新 DL pi_se/RT
 * timeout/class/prio 后恢复，并触发必要 balance。@pi_task 是 blocked donor 借用
 * 指针，其存活由 slowunlock/postunlock 在 task 再运行前先 deboost 的协议保证。
 */
void rt_mutex_setprio(struct task_struct *p, struct task_struct *pi_task)
{
	int prio, oldprio, queue_flag =
		DEQUEUE_SAVE | DEQUEUE_MOVE | DEQUEUE_NOCLOCK;
	const struct sched_class *prev_class, *next_class;
	struct rq_flags rf;
	struct rq *rq;

	/* XXX used to be waiter->prio, not waiter->task->prio */
	prio = __rt_effective_prio(pi_task, p->normal_prio);

	/*
	 * If nothing changed; bail early.
	 */
	/* 中文：donor 指针、有效优先级和非 DL 语义均未变化时直接返回。 */
	if (p->pi_top_task == pi_task && prio == p->prio && !dl_prio(prio))
		return;

	rq = __task_rq_lock(p, &rf);
	update_rq_clock(rq);
	/*
	 * Set under pi_lock && rq->lock, such that the value can be used under
	 * either lock.
	 *
	 * Note that there is loads of tricky to make this pointer cache work
	 * right. rt_mutex_slowunlock()+rt_mutex_postunlock() work together to
	 * ensure a task is de-boosted (pi_task is set to NULL) before the
	 * task is allowed to run again (and can exit). This ensures the pointer
	 * points to a blocked task -- which guarantees the task is present.
	 */
	/*
	 * 中文：pi_top_task 在 pi_lock+rq 锁下发布，使任一锁的读者都可使用。其裸
	 * 指针安全依赖 slowunlock/postunlock：task 再运行/退出前先 deboost 置 NULL，
	 * 因此非 NULL 始终指向仍被阻塞关系保活的 task。
	 */
	p->pi_top_task = pi_task;

	/*
	 * For FIFO/RR we only need to set prio, if that matches we're done.
	 */
	/* 中文：FIFO/RR 不携带 DL 实体替换语义，有效 prio 相同即可结束。 */
	if (prio == p->prio && !dl_prio(prio))
		goto out_unlock;

	/*
	 * Idle task boosting is a no-no in general. There is one
	 * exception, when PREEMPT_RT and NOHZ is active:
	 *
	 * The idle task calls get_next_timer_interrupt() and holds
	 * the timer wheel base->lock on the CPU and another CPU wants
	 * to access the timer (probably to cancel it). We can safely
	 * ignore the boosting request, as the idle CPU runs this code
	 * with interrupts disabled and will complete the lock
	 * protected section without being interrupted. So there is no
	 * real need to boost.
	 */
	/*
	 * 中文：通常禁止提升 idle task。PREEMPT_RT+NOHZ 下 idle 可能持 timer base
	 * 锁被远端等待，但本 CPU 此段 IRQ 关闭、必会很快完成，忽略 PI 请求不会形成
	 * 实际反转；仍以 WARN 验证 idle 必为 curr 且未进入普通 PI 阻塞链。
	 */
	if (unlikely(p == rq->idle)) {
		WARN_ON(p != rq->curr);
		WARN_ON(p->pi_blocked_on);
		goto out_unlock;
	}

	trace_sched_pi_setprio(p, pi_task);
	oldprio = p->prio;

	if (oldprio == prio && !dl_prio(prio))
		queue_flag &= ~DEQUEUE_MOVE;

	prev_class = p->sched_class;
	next_class = __setscheduler_class(p->policy, prio);

	if (prev_class != next_class)
		queue_flag |= DEQUEUE_CLASS;

	scoped_guard (sched_change, p, queue_flag) {
		/*
		 * Boosting condition are:
		 * 1. -rt task is running and holds mutex A
		 *      --> -dl task blocks on mutex A
		 *
		 * 2. -dl task is running and holds mutex A
		 *      --> -dl task blocks on mutex A and could preempt the
		 *          running task
		 */
	/*
	 * 中文：需要借用 donor DL 实体的两种情况：非 DL owner 被 DL waiter 阻塞，
	 * 或 DL owner 被 deadline 更早、足以抢占它的 DL waiter 阻塞；此时还需
	 * ENQUEUE_REPLENISH 重新建立 DL runtime/deadline。
	 */
		if (dl_prio(prio)) {
			if (!dl_prio(p->normal_prio) ||
			    (pi_task && dl_prio(pi_task->prio) &&
			     dl_entity_preempt(&pi_task->dl, &p->dl))) {
				p->dl.pi_se = pi_task->dl.pi_se;
				scope->flags |= ENQUEUE_REPLENISH;
			} else {
				p->dl.pi_se = &p->dl;
			}
		} else if (rt_prio(prio)) {
			if (dl_prio(oldprio))
				p->dl.pi_se = &p->dl;
			if (oldprio < prio)
				scope->flags |= ENQUEUE_HEAD;
		} else {
			if (dl_prio(oldprio))
				p->dl.pi_se = &p->dl;
			if (rt_prio(oldprio))
				p->rt.timeout = 0;
		}

		p->sched_class = next_class;
		p->prio = prio;
	}
out_unlock:
	/* Caller holds task_struct::pi_lock, IRQs are still disabled */

	__balance_callbacks(rq, &rf);
	__task_rq_unlock(rq, p, &rf);
}
#endif /* CONFIG_RT_MUTEXES */

#if !defined(CONFIG_PREEMPTION) || defined(CONFIG_PREEMPT_DYNAMIC)
/*
 * __cond_resched() 是非全抢占/动态模型的显式让出点。若 should_resched 且 IRQ-on，
 * 执行 preempt common 并返回 1；否则在非 PREEMPT_RCU 内核按紧急需要报告 RCU
 * quiescent state，返回 0。它可切换 task，调用者不能持不允许放弃的状态。
 */
int __sched __cond_resched(void)
{
	if (should_resched(0) && !irqs_disabled()) {
		preempt_schedule_common();
		return 1;
	}
	/*
	 * In PREEMPT_RCU kernels, ->rcu_read_lock_nesting tells the tick
	 * whether the current CPU is in an RCU read-side critical section,
	 * so the tick can report quiescent states even for CPUs looping
	 * in kernel context.  In contrast, in non-preemptible kernels,
	 * RCU readers leave no in-memory hints, which means that CPU-bound
	 * processes executing in kernel context might never report an
	 * RCU quiescent state.  Therefore, the following code causes
	 * cond_resched() to report a quiescent state, but only when RCU
	 * is in urgent need of one.
	 * A third case, preemptible, but non-PREEMPT_RCU provides for
	 * urgently needed quiescent states via rcu_flavor_sched_clock_irq().
	 */
#ifndef CONFIG_PREEMPT_RCU
	rcu_all_qs();
#endif
	return 0;
}
EXPORT_SYMBOL(__cond_resched);
#endif

#ifdef CONFIG_PREEMPT_DYNAMIC
# ifdef CONFIG_HAVE_PREEMPT_DYNAMIC_CALL
#  define cond_resched_dynamic_enabled	__cond_resched
#  define cond_resched_dynamic_disabled	((void *)&__static_call_return0)
DEFINE_STATIC_CALL_RET0(cond_resched, __cond_resched);
EXPORT_STATIC_CALL_TRAMP(cond_resched);

#  define might_resched_dynamic_enabled	__cond_resched
#  define might_resched_dynamic_disabled ((void *)&__static_call_return0)
DEFINE_STATIC_CALL_RET0(might_resched, __cond_resched);
EXPORT_STATIC_CALL_TRAMP(might_resched);
# elif defined(CONFIG_HAVE_PREEMPT_DYNAMIC_KEY)
static DEFINE_STATIC_KEY_FALSE(sk_dynamic_cond_resched);
/*
 * dynamic_cond_resched()/dynamic_might_resched() 是 static-key 门控 wrapper：关闭
 * 返回 0，开启转到 __cond_resched。static-call 配置用改写目标达到同样效果。
 */
int __sched dynamic_cond_resched(void)
{
	if (!static_branch_unlikely(&sk_dynamic_cond_resched))
		return 0;
	return __cond_resched();
}
EXPORT_SYMBOL(dynamic_cond_resched);

static DEFINE_STATIC_KEY_FALSE(sk_dynamic_might_resched);
int __sched dynamic_might_resched(void)
{
	if (!static_branch_unlikely(&sk_dynamic_might_resched))
		return 0;
	return __cond_resched();
}
EXPORT_SYMBOL(dynamic_might_resched);
# endif
#endif /* CONFIG_PREEMPT_DYNAMIC */

/*
 * __cond_resched_lock() - if a reschedule is pending, drop the given lock,
 * call schedule, and on return reacquire the lock.
 *
 * This works OK both with and without CONFIG_PREEMPTION. We do strange low-level
 * operations here to prevent schedule() from being called twice (once via
 * spin_unlock(), once by hand).
 */
/*
 * 原文：若需调度或锁竞争，临时释放调用者持有的 @lock，执行一次 cond_resched，
 * 再重新获取。低级计数操作避免 spin_unlock 自身触发抢占后手工又调度一次。
 * 返回 1 表示锁曾释放（共享状态必须重验），0 表示连续持锁。
 */
int __cond_resched_lock(spinlock_t *lock)
{
	int resched = should_resched(PREEMPT_LOCK_OFFSET);
	int ret = 0;

	lockdep_assert_held(lock);

	if (spin_needbreak(lock) || resched) {
		spin_unlock(lock);
		if (!_cond_resched())
			cpu_relax();
		ret = 1;
		spin_lock(lock);
	}
	return ret;
}
EXPORT_SYMBOL(__cond_resched_lock);

/*
 * rwlock read/write 版本与 spinlock 契约相同：入口和出口持同模式锁，返回 1 表示
 * 中途释放过，调用者必须重新验证受保护条件；等待窗口可让其他 writer/reader 改变
 * 状态。
 */
int __cond_resched_rwlock_read(rwlock_t *lock)
{
	int resched = should_resched(PREEMPT_LOCK_OFFSET);
	int ret = 0;

	lockdep_assert_held_read(lock);

	if (rwlock_needbreak(lock) || resched) {
		read_unlock(lock);
		if (!_cond_resched())
			cpu_relax();
		ret = 1;
		read_lock(lock);
	}
	return ret;
}
EXPORT_SYMBOL(__cond_resched_rwlock_read);

/*
 * __cond_resched_rwlock_write() 在需要调度或锁竞争时临时释放写锁、让出 CPU 后
 * 重取；返回 1 表示保护窗口曾打开，调用者必须重验共享状态。
 */
int __cond_resched_rwlock_write(rwlock_t *lock)
{
	int resched = should_resched(PREEMPT_LOCK_OFFSET);
	int ret = 0;

	lockdep_assert_held_write(lock);

	if (rwlock_needbreak(lock) || resched) {
		write_unlock(lock);
		if (!_cond_resched())
			cpu_relax();
		ret = 1;
		write_lock(lock);
	}
	return ret;
}
EXPORT_SYMBOL(__cond_resched_rwlock_write);

#ifdef CONFIG_PREEMPT_DYNAMIC

# ifdef CONFIG_GENERIC_IRQ_ENTRY
#  include <linux/irq-entry-common.h>
# endif

/*
 * SC:cond_resched
 * SC:might_resched
 * SC:preempt_schedule
 * SC:preempt_schedule_notrace
 * SC:irqentry_exit_cond_resched
 *
 *
 * NONE:
 *   cond_resched               <- __cond_resched
 *   might_resched              <- RET0
 *   preempt_schedule           <- NOP
 *   preempt_schedule_notrace   <- NOP
 *   irqentry_exit_cond_resched <- NOP
 *   dynamic_preempt_lazy       <- false
 *
 * VOLUNTARY:
 *   cond_resched               <- __cond_resched
 *   might_resched              <- __cond_resched
 *   preempt_schedule           <- NOP
 *   preempt_schedule_notrace   <- NOP
 *   irqentry_exit_cond_resched <- NOP
 *   dynamic_preempt_lazy       <- false
 *
 * FULL:
 *   cond_resched               <- RET0
 *   might_resched              <- RET0
 *   preempt_schedule           <- preempt_schedule
 *   preempt_schedule_notrace   <- preempt_schedule_notrace
 *   irqentry_exit_cond_resched <- irqentry_exit_cond_resched
 *   dynamic_preempt_lazy       <- false
 *
 * LAZY:
 *   cond_resched               <- RET0
 *   might_resched              <- RET0
 *   preempt_schedule           <- preempt_schedule
 *   preempt_schedule_notrace   <- preempt_schedule_notrace
 *   irqentry_exit_cond_resched <- irqentry_exit_cond_resched
 *   dynamic_preempt_lazy       <- true
 */
/*
 * 原表给出四种动态模型对五个调用点的精确映射：NONE 只保留显式 cond_resched；
 * VOLUNTARY 还让 might_resched 真正让出；FULL/LAZY 改由 preempt_enable 和 IRQ
 * 退出触发抢占，显式让出变成 RET0；LAZY 额外让普通 resched 可延迟。static-call
 * 或 static-key 只改变分派成本，语义由这张矩阵定义。
 */

enum {
	preempt_dynamic_undefined = -1,
	preempt_dynamic_none,
	preempt_dynamic_voluntary,
	preempt_dynamic_full,
	preempt_dynamic_lazy,
};

int preempt_dynamic_mode = preempt_dynamic_undefined;

/*
 * preempt_dynamic_mode 是运行期已发布枚举；undefined 只允许存在于早期初始化。
 * READ_ONCE accessor 可无锁查询，更新由 sched_dynamic_mutex 串行。
 */

/*
 * sched_dynamic_mode() 把借用字符串解析为当前架构/RT 配置支持的枚举；成功返回
 * none/voluntary/full/lazy，未支持或未知返回 -EINVAL，无副作用。
 */
int sched_dynamic_mode(const char *str)
{
# if !(defined(CONFIG_PREEMPT_RT) || defined(CONFIG_ARCH_HAS_PREEMPT_LAZY))
	if (!strcmp(str, "none"))
		return preempt_dynamic_none;

	if (!strcmp(str, "voluntary"))
		return preempt_dynamic_voluntary;
# endif

	if (!strcmp(str, "full"))
		return preempt_dynamic_full;

# ifdef CONFIG_ARCH_HAS_PREEMPT_LAZY
	if (!strcmp(str, "lazy"))
		return preempt_dynamic_lazy;
# endif

	return -EINVAL;
}

/*
 * 这组宏把统一的 enable/disable 操作映射到架构选择的 static_call 或 static_key；
 * 调用点不关心补丁机制，更新顺序由 __sched_dynamic_update() 统一控制。
 */
# define preempt_dynamic_key_enable(f)	static_key_enable(&sk_dynamic_##f.key)
# define preempt_dynamic_key_disable(f)	static_key_disable(&sk_dynamic_##f.key)

# if defined(CONFIG_HAVE_PREEMPT_DYNAMIC_CALL)
#  define preempt_dynamic_enable(f)	static_call_update(f, f##_dynamic_enabled)
#  define preempt_dynamic_disable(f)	static_call_update(f, f##_dynamic_disabled)
# elif defined(CONFIG_HAVE_PREEMPT_DYNAMIC_KEY)
#  define preempt_dynamic_enable(f)	preempt_dynamic_key_enable(f)
#  define preempt_dynamic_disable(f)	preempt_dynamic_key_disable(f)
# else
#  error "Unsupported PREEMPT_DYNAMIC mechanism"
# endif

static DEFINE_MUTEX(sched_dynamic_mutex);

/*
 * sched_dynamic_mutex 串行化一组 static-call/key 的多点切换，使两个控制请求不会
 * 交错；它不能让正在执行的所有 CPU 瞬间观察同一模型，过渡正确性由先全启用、
 * 后按目标关闭的安全顺序保证。
 */

/*
 * __sched_dynamic_update() 先把所有调度入口置为可用的非零安全状态，避免 NONE/
 * VOLUNTARY→FULL 过渡期间所有入口同时 NOP；再按 @mode 应用矩阵，最后 WRITE_ONCE
 * 发布枚举。调用者持 sched_dynamic_mutex；无失败返回，mode 必须已验证。
 */
static void __sched_dynamic_update(int mode)
{
	/*
	 * Avoid {NONE,VOLUNTARY} -> FULL transitions from ever ending up in
	 * the ZERO state, which is invalid.
	 */
	preempt_dynamic_enable(cond_resched);
	preempt_dynamic_enable(might_resched);
	preempt_dynamic_enable(preempt_schedule);
	preempt_dynamic_enable(preempt_schedule_notrace);
	preempt_dynamic_enable(irqentry_exit_cond_resched);
	preempt_dynamic_key_disable(preempt_lazy);

	switch (mode) {
	case preempt_dynamic_none:
		preempt_dynamic_enable(cond_resched);
		preempt_dynamic_disable(might_resched);
		preempt_dynamic_disable(preempt_schedule);
		preempt_dynamic_disable(preempt_schedule_notrace);
		preempt_dynamic_disable(irqentry_exit_cond_resched);
		preempt_dynamic_key_disable(preempt_lazy);
		if (mode != preempt_dynamic_mode)
			pr_info("Dynamic Preempt: none\n");
		break;

	case preempt_dynamic_voluntary:
		preempt_dynamic_enable(cond_resched);
		preempt_dynamic_enable(might_resched);
		preempt_dynamic_disable(preempt_schedule);
		preempt_dynamic_disable(preempt_schedule_notrace);
		preempt_dynamic_disable(irqentry_exit_cond_resched);
		preempt_dynamic_key_disable(preempt_lazy);
		if (mode != preempt_dynamic_mode)
			pr_info("Dynamic Preempt: voluntary\n");
		break;

	case preempt_dynamic_full:
		preempt_dynamic_disable(cond_resched);
		preempt_dynamic_disable(might_resched);
		preempt_dynamic_enable(preempt_schedule);
		preempt_dynamic_enable(preempt_schedule_notrace);
		preempt_dynamic_enable(irqentry_exit_cond_resched);
		preempt_dynamic_key_disable(preempt_lazy);
		if (mode != preempt_dynamic_mode)
			pr_info("Dynamic Preempt: full\n");
		break;

	case preempt_dynamic_lazy:
		preempt_dynamic_disable(cond_resched);
		preempt_dynamic_disable(might_resched);
		preempt_dynamic_enable(preempt_schedule);
		preempt_dynamic_enable(preempt_schedule_notrace);
		preempt_dynamic_enable(irqentry_exit_cond_resched);
		preempt_dynamic_key_enable(preempt_lazy);
		if (mode != preempt_dynamic_mode)
			pr_info("Dynamic Preempt: lazy\n");
		break;
	}

	WRITE_ONCE(preempt_dynamic_mode, mode);
}

/*
 * sched_dynamic_update() 是加 mutex 的公开内部 wrapper；@mode 为已解析枚举。
 */
void sched_dynamic_update(int mode)
{
	mutex_lock(&sched_dynamic_mutex);
	__sched_dynamic_update(mode);
	mutex_unlock(&sched_dynamic_mutex);
}

/*
 * setup_preempt_mode() 解析 preempt= 启动参数并立即切模型；成功返回 1，未知模式
 * 警告并返回 0。@str 是启动期借用文本。
 */
static int __init setup_preempt_mode(char *str)
{
	int mode = sched_dynamic_mode(str);
	if (mode < 0) {
		pr_warn("Dynamic Preempt: unsupported mode: %s\n", str);
		return 0;
	}

	sched_dynamic_update(mode);
	return 1;
}
__setup("preempt=", setup_preempt_mode);

/*
 * preempt_dynamic_init() 若启动参数尚未选模式，则从编译期 NONE/VOLUNTARY/LAZY/
 * FULL 默认建立运行期矩阵。FULL 的 static-call 默认已正确时只发布枚举。
 */
static void __init preempt_dynamic_init(void)
{
	if (preempt_dynamic_mode == preempt_dynamic_undefined) {
		if (IS_ENABLED(CONFIG_PREEMPT_NONE)) {
			sched_dynamic_update(preempt_dynamic_none);
		} else if (IS_ENABLED(CONFIG_PREEMPT_VOLUNTARY)) {
			sched_dynamic_update(preempt_dynamic_voluntary);
		} else if (IS_ENABLED(CONFIG_PREEMPT_LAZY)) {
			sched_dynamic_update(preempt_dynamic_lazy);
		} else {
			/* Default static call setting, nothing to do */
			/* 中文：FULL 的默认 static-call 补丁已正确，只需发布当前 mode。 */
			WARN_ON_ONCE(!IS_ENABLED(CONFIG_PREEMPT));
			preempt_dynamic_mode = preempt_dynamic_full;
			pr_info("Dynamic Preempt: full\n");
		}
	}
}

# define PREEMPT_MODEL_ACCESSOR(mode)					\
	bool preempt_model_##mode(void)					\
	{								\
		int mode = READ_ONCE(preempt_dynamic_mode);		\
		WARN_ON_ONCE(mode == preempt_dynamic_undefined);	\
		return mode == preempt_dynamic_##mode;			\
	}								\
	EXPORT_SYMBOL_GPL(preempt_model_##mode)

/*
 * PREEMPT_MODEL_ACCESSOR 为四个模型生成无锁布尔 ABI；读取 undefined 会 WARN，
 * 提醒调用者不得早于 preempt_dynamic_init 使用。宏续行保持原样。
 */

PREEMPT_MODEL_ACCESSOR(none);
PREEMPT_MODEL_ACCESSOR(voluntary);
PREEMPT_MODEL_ACCESSOR(full);
PREEMPT_MODEL_ACCESSOR(lazy);

#else /* !CONFIG_PREEMPT_DYNAMIC: */

/* 非动态配置把 init 编译为空，模型由 IS_ENABLED 分支和 preempt_model_str 固定。 */

#define preempt_dynamic_mode -1

static inline void preempt_dynamic_init(void) { }

#endif /* CONFIG_PREEMPT_DYNAMIC */

const char *preempt_modes[] = {
	"none", "voluntary", "full", "lazy", NULL,
};

/*
 * preempt_modes 是枚举到稳定显示名的只读映射，以 NULL 结尾；preempt_model_str()
 * 用静态 128 字节缓冲拼出 PREEMPT、RT、动态/LAZY 组合。返回指针由内核永久持有，
 * 非重入并发调用可能写同一缓冲，但内容只用于诊断展示。
 */

const char *preempt_model_str(void)
{
	bool brace = IS_ENABLED(CONFIG_PREEMPT_RT) &&
		(IS_ENABLED(CONFIG_PREEMPT_DYNAMIC) ||
		 IS_ENABLED(CONFIG_PREEMPT_LAZY));
	static char buf[128];

	if (IS_ENABLED(CONFIG_PREEMPT_BUILD)) {
		struct seq_buf s;

		seq_buf_init(&s, buf, sizeof(buf));
		seq_buf_puts(&s, "PREEMPT");

		if (IS_ENABLED(CONFIG_PREEMPT_RT))
			seq_buf_printf(&s, "%sRT%s",
				       brace ? "_{" : "_",
				       brace ? "," : "");

		if (IS_ENABLED(CONFIG_PREEMPT_DYNAMIC)) {
			seq_buf_printf(&s, "(%s)%s",
				       preempt_dynamic_mode >= 0 ?
				       preempt_modes[preempt_dynamic_mode] : "undef",
				       brace ? "}" : "");
			return seq_buf_str(&s);
		}

		if (IS_ENABLED(CONFIG_PREEMPT_LAZY)) {
			seq_buf_printf(&s, "LAZY%s",
				       brace ? "}" : "");
			return seq_buf_str(&s);
		}

		return seq_buf_str(&s);
	}

	if (IS_ENABLED(CONFIG_PREEMPT_VOLUNTARY_BUILD))
		return "VOLUNTARY";

	return "NONE";
}

/*
 * io_schedule_prepare()/finish() 成对维护 current->in_iowait。prepare 返回旧值令牌，
 * 置 1 并先 flush blk plug；finish 用 token 精确恢复嵌套调用前状态。无对象引用
 * 转移，目标恒为 current。
 */
int io_schedule_prepare(void)
{
	int old_iowait = current->in_iowait;

	current->in_iowait = 1;
	blk_flush_plug(current->plug, true);
	return old_iowait;
}

void io_schedule_finish(int token)
{
	current->in_iowait = token;
}

/*
 * This task is about to go to sleep on IO. Increment rq->nr_iowait so
 * that process accounting knows that this is a task in IO wait state.
 */
/*
 * 原文：current 即将因 I/O 睡眠，临时置 in_iowait，使 rq/process accounting
 * 统计该等待。@timeout 为 jiffies，返回 schedule_timeout 的剩余 jiffies；无超时
 * 版本调用普通 schedule。两者即使嵌套也通过 token 恢复原标志。
 */
long __sched io_schedule_timeout(long timeout)
{
	int token;
	long ret;

	token = io_schedule_prepare();
	ret = schedule_timeout(timeout);
	io_schedule_finish(token);

	return ret;
}
EXPORT_SYMBOL(io_schedule_timeout);

void __sched io_schedule(void)
{
	int token;

	token = io_schedule_prepare();
	schedule();
	io_schedule_finish(token);
}
EXPORT_SYMBOL(io_schedule);

/*
 * sched_show_task() - 输出 @p 的状态、栈余量、PID/parent、worker/stop/SCX 信息
 * 与内核栈。try_get_task_stack() 取得栈引用，失败说明栈正释放而静默返回；parent
 * 在 RCU 下读取，最后 put_task_stack。函数用于诊断，输出是非原子快照且可能较慢。
 */
void sched_show_task(struct task_struct *p)
{
	unsigned long free;
	int ppid;

	if (!try_get_task_stack(p))
		return;

	pr_info("task:%-15.15s state:%c", p->comm, task_state_to_char(p));

	if (task_is_running(p))
		pr_cont("  running task    ");
	free = stack_not_used(p);
	ppid = 0;
	rcu_read_lock();
	if (pid_alive(p))
		ppid = task_pid_nr(rcu_dereference(p->real_parent));
	rcu_read_unlock();
	pr_cont(" stack:%-5lu pid:%-5d tgid:%-5d ppid:%-6d task_flags:0x%04x flags:0x%08lx\n",
		free, task_pid_nr(p), task_tgid_nr(p),
		ppid, p->flags, read_task_thread_flags(p));

	print_worker_info(KERN_INFO, p);
	print_stop_info(KERN_INFO, p);
	print_scx_info(KERN_INFO, p);
	show_stack(p, NULL, KERN_INFO);
	put_task_stack(p);
}
EXPORT_SYMBOL_GPL(sched_show_task);

/*
 * state_filter_match() 为 show_state_filter 提供无锁筛选：0 匹配全部；普通掩码
 * 匹配 __state；查询 TASK_UNINTERRUPTIBLE 时排除带 TASK_NOLOAD 的 TASK_IDLE，
 * 但保留 TASK_KILLABLE。结果是诊断快照。
 */
static inline bool
state_filter_match(unsigned long state_filter, struct task_struct *p)
{
	unsigned int state = READ_ONCE(p->__state);

	/* no filter, everything matches */
	/* 中文：无过滤器时全部匹配。 */
	if (!state_filter)
		return true;

	/* filter, but doesn't match */
	/* 中文：状态位与过滤器无交集。 */
	if (!(state & state_filter))
		return false;

	/*
	 * When looking for TASK_UNINTERRUPTIBLE skip TASK_IDLE (allows
	 * TASK_KILLABLE).
	 */
	/* 中文：查 D 状态时排除 TASK_IDLE，但允许 TASK_KILLABLE。 */
	if (state_filter == TASK_UNINTERRUPTIBLE && (state & TASK_NOLOAD))
		return false;

	return true;
}

/*
 * state_filter_match() 对 @p 的无锁 state 快照应用过滤：0 匹配全部；普通按位相交；
 * 专门查询 UNINTERRUPTIBLE 时排除带 TASK_NOLOAD 的 TASK_IDLE，但保留 KILLABLE。
 */


/*
 * show_state_filter() 在 RCU 下遍历所有进程线程并输出匹配 task。慢 console 输出前
 * 持续触碰 NMI/softlockup watchdog，避免诊断本身触发假锁死；无过滤时额外输出
 * sched debug 和全部锁。@state_filter 为 task state 位图，函数可产生大量日志。
 */
void show_state_filter(unsigned int state_filter)
{
	struct task_struct *g, *p;

	rcu_read_lock();
	for_each_process_thread(g, p) {
		/*
		 * reset the NMI-timeout, listing all files on a slow
		 * console might take a lot of time:
		 * Also, reset softlockup watchdogs on all CPUs, because
		 * another CPU might be blocked waiting for us to process
		 * an IPI.
		 */
		touch_nmi_watchdog();
		touch_all_softlockup_watchdogs();
		if (state_filter_match(state_filter, p))
			sched_show_task(p);
	}

	if (!state_filter)
		sysrq_sched_debug_show();

	rcu_read_unlock();
	/*
	 * Only show locks if all tasks are dumped:
	 */
	/* 中文：只有无过滤地 dump 全部 task 时，锁依赖输出才具有完整上下文。 */
	if (!state_filter)
		debug_show_all_locks();
}

/**
 * init_idle - set up an idle thread for a given CPU
 * @idle: task in question
 * @cpu: CPU the idle task belongs to
 *
 * NOTE: this function does not set the idle thread's NEED_RESCHED
 * flag, to make booting more robust.
 */
/*
 * 原文：为 @cpu 构造专属 @idle task，但故意不预置 NEED_RESCHED，避免启动时入口
 * 尚未完整却被迫调度。函数在 idle pi_lock/rq 锁下发布 RUNNING、per-CPU kthread
 * 属性、单 CPU affinity、rq->idle/donor/curr、on_rq/on_cpu；出锁后再设特殊
 * preempt_count/class、ftrace/vtime/name。@idle 生命周期永久，无失败返回。
 */
void __init init_idle(struct task_struct *idle, int cpu)
{
	struct affinity_context ac = (struct affinity_context) {
		.new_mask  = cpumask_of(cpu),
		.flags     = 0,
	};
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	raw_spin_lock_irqsave(&idle->pi_lock, flags);
	raw_spin_rq_lock(rq);

	idle->__state = TASK_RUNNING;
	idle->se.exec_start = sched_clock();
	/*
	 * PF_KTHREAD should already be set at this point; regardless, make it
	 * look like a proper per-CPU kthread.
	 */
	/* 中文：无论早期标志是否已设，都把 idle 明确塑造成禁止改 affinity 的 per-CPU kthread。 */
	idle->flags |= PF_KTHREAD | PF_NO_SETAFFINITY;
	kthread_set_per_cpu(idle, cpu);

	/*
	 * No validation and serialization required at boot time and for
	 * setting up the idle tasks of not yet online CPUs.
	 */
	/* 中文：启动期或 CPU 尚未 online 时对象未并发可见，无需运行期校验与序列化。 */
	set_cpus_allowed_common(idle, &ac);
	/*
	 * We're having a chicken and egg problem, even though we are
	 * holding rq->lock, the CPU isn't yet set to this CPU so the
	 * lockdep check in task_group() will fail.
	 *
	 * Similar case to sched_fork(). / Alternatively we could
	 * use task_rq_lock() here and obtain the other rq->lock.
	 *
	 * Silence PROVE_RCU
	 */
	/*
	 * 中文：存在先有 CPU 归属才能通过 task_group lockdep、又需正确 rq 锁才能设
	 * CPU 的鸡生蛋问题；启动期对象未发布，临时 RCU 读侧只用于表达安全并静默
	 * PROVE_RCU，类似 sched_fork 初始赋值。
	 */
	rcu_read_lock();
	__set_task_cpu(idle, cpu);
	rcu_read_unlock();

	rq->idle = idle;
	rq_set_donor(rq, idle);
	rcu_assign_pointer(rq->curr, idle);
	idle->on_rq = TASK_ON_RQ_QUEUED;
	idle->on_cpu = 1;
	raw_spin_rq_unlock(rq);
	raw_spin_unlock_irqrestore(&idle->pi_lock, flags);

	/* Set the preempt count _outside_ the spinlocks! */
	/* 中文：必须在释放自旋锁后设置 idle 特殊 preempt_count，避免污染锁嵌套计数。 */
	init_idle_preempt_count(idle, cpu);

	/*
	 * The idle tasks have their own, simple scheduling class:
	 */
	idle->sched_class = &idle_sched_class;
	ftrace_graph_init_idle_task(idle, cpu);
	vtime_init_idle(idle, cpu);
	sprintf(idle->comm, "%s/%d", INIT_TASK_COMM, cpu);
}

/*
 * cpuset_cpumask_can_shrink() 询问 DL admission 是否允许从 @cur 收窄到 @trial。
 * 当前 mask 为空视为可收窄；返回 1 或 DL helper 的许可/错误值，不修改 mask。
 */
int cpuset_cpumask_can_shrink(const struct cpumask *cur,
			      const struct cpumask *trial)
{
	int ret = 1;

	if (cpumask_empty(cur))
		return ret;

	ret = dl_cpuset_cpumask_can_shrink(cur, trial);

	return ret;
}

/*
 * task_can_attach() 在 cpuset attach 前拒绝 PF_NO_SETAFFINITY kthread：这些线程
 * 不能改变 CPU affinity，以 allowed node 隔离也无意义。返回 0 或 -EINVAL，
 * @p 为 cgroup 迁移事务稳定的借用 task。
 */
int task_can_attach(struct task_struct *p)
{
	int ret = 0;

	/*
	 * Kthreads which disallow setaffinity shouldn't be moved
	 * to a new cpuset; we don't want to change their CPU
	 * affinity and isolating such threads by their set of
	 * allowed nodes is unnecessary.  Thus, cpusets are not
	 * applicable for such threads.  This prevents checking for
	 * success of set_cpus_allowed_ptr() on all attached tasks
	 * before cpus_mask may be changed.
	 */
	if (p->flags & PF_NO_SETAFFINITY)
		ret = -EINVAL;

	return ret;
}

bool sched_smp_initialized __read_mostly;

/*
 * sched_smp_initialized 在 sched_init_smp 完成 domain/各 SMP class 初始化后发布；
 * CPU hotplug 早期路径据此跳过尚不可用的 NUMA/domain 更新。启动后只读为主。
 */

#ifdef CONFIG_NUMA_BALANCING
/* Migrate current task p to target_cpu */
/*
 * 原文：把 @p 迁到 @target_cpu。相同 CPU 返回 0，目标不在 cpus_ptr 返回 -EINVAL；
 * 否则在源 CPU 同步 stopper 中迁移并返回其结果。TODO 指出 schedstats 尚未精确
 * 更新；@p 生命周期由调用者保证。
 */
int migrate_task_to(struct task_struct *p, int target_cpu)
{
	struct migration_arg arg = { p, target_cpu };
	int curr_cpu = task_cpu(p);

	if (curr_cpu == target_cpu)
		return 0;

	if (!cpumask_test_cpu(target_cpu, p->cpus_ptr))
		return -EINVAL;

	/* TODO: This is not properly updating schedstats */

	trace_sched_move_numa(p, curr_cpu, target_cpu);
	return stop_one_cpu(curr_cpu, migration_cpu_stop, &arg);
}

/*
 * Requeue a task on a given node and accurately track the number of NUMA
 * tasks on the runqueues
 */
/*
 * 原文：在 rq 锁和 sched_change 事务内修改 @p->numa_preferred_nid，使 queued/
 * running task 先撤出后按新 node 重新入队，精确维护 rq NUMA task 计数。无返回值。
 */
void sched_setnuma(struct task_struct *p, int nid)
{
	guard(task_rq_lock)(p);
	scoped_guard (sched_change, p, DEQUEUE_SAVE)
		p->numa_preferred_nid = nid;
}
#endif /* CONFIG_NUMA_BALANCING */

#ifdef CONFIG_HOTPLUG_CPU
/*
 * Invoked on the outgoing CPU in context of the CPU hotplug thread
 * after ensuring that there are no user space tasks left on the CPU.
 *
 * If there is a lazy mm in use on the hotplug thread, drop it and
 * switch to init_mm.
 *
 * The reference count on init_mm is dropped in finish_cpu().
 */
/*
 * 原文：在 outgoing CPU hotplug thread 上、确认无用户 task 后，将其可能借用的
 * lazy active_mm 切到 init_mm。先取得 init_mm lazy 引用，IRQ-off 切页表，完成
 * arch hook 并释放旧 mm；init_mm 引用由 boot CPU 的 finish_cpu() 最终释放。
 * 无入参/返回值，current 必为 hotplug thread。
 */
static void sched_force_init_mm(void)
{
	struct mm_struct *mm = current->active_mm;

	if (mm != &init_mm) {
		mmgrab_lazy_tlb(&init_mm);
		local_irq_disable();
		current->active_mm = &init_mm;
		switch_mm_irqs_off(mm, &init_mm, current);
		local_irq_enable();
		finish_arch_post_lock_switch();
		mmdrop_lazy_tlb(mm);
	}

	/* finish_cpu(), as ran on the BP, will clean up the active_mm state */
}

/*
 * __balance_push_cpu_stop() 在 stopper 上下文把带引用 @p 从 dying rq 移到 fallback。
 * pi_lock 下选目标，锁/更新源 rq 后复核 queued 再迁移；出口总 put_task_struct，
 * 返回 0，竞态导致未移动由后续 hotplug 循环继续处理。
 */
static int __balance_push_cpu_stop(void *arg)
{
	struct task_struct *p = arg;
	struct rq *rq = this_rq();
	struct rq_flags rf;
	int cpu;

	scoped_guard (raw_spinlock_irq, &p->pi_lock) {
		/*
		 * We may change the underlying rq, but the locks held will
		 * appropriately be "transferred" when switching.
		 */
		context_unsafe_alias(rq);

		cpu = select_fallback_rq(rq->cpu, p);

		rq_lock(rq, &rf);
		update_rq_clock(rq);
		if (task_rq(p) == rq && task_on_rq_queued(p))
			rq = __migrate_task(rq, &rf, p, cpu);
		rq_unlock(rq, &rf);
	}

	put_task_struct(p);

	return 0;
}

static DEFINE_PER_CPU(struct cpu_stop_work, push_work);

/*
 * Ensure we only run per-cpu kthreads once the CPU goes !active.
 *
 * This is enabled below SCHED_AP_ACTIVE; when !cpu_active(), but only
 * effective when the hotplug motion is down.
 */
/*
 * 原文：CPU 清 active 后只允许 per-CPU kthread 和 migration-disabled task 暂留。
 * balance_push 是持 rq 锁的永久 callback：非 dying/非本 CPU 无动作；允许暂留者
 * 若 rq 已排空则唤醒 hotplug waiter；普通 curr 取得引用，临时放 rq 锁并异步唤醒
 * stopper 将其迁走，再重锁等待 schedule 选 stopper。callback 保持安装直到关闭。
 */
static void balance_push(struct rq *rq)
	__must_hold(__rq_lockp(rq))
{
	struct task_struct *push_task = rq->curr;

	lockdep_assert_rq_held(rq);

	/*
	 * Ensure the thing is persistent until balance_push_set(.on = false);
	 */
	rq->balance_callback = &balance_push_callback;

	/*
	 * Only active while going offline and when invoked on the outgoing
	 * CPU.
	 */
	/* 中文：callback 只在 CPU 下线方向、且确实运行于 outgoing CPU 时生效。 */
	if (!cpu_dying(rq->cpu) || rq != this_rq())
		return;

	/*
	 * Both the cpu-hotplug and stop task are in this case and are
	 * required to complete the hotplug process.
	 */
	/*
	 * 中文：per-CPU hotplug/stop task 以及 migration-disabled task 都必须暂留，
	 * 前两者推进下线流程，后者要先退出不可迁移区。
	 */
	if (kthread_is_per_cpu(push_task) ||
	    is_migration_disabled(push_task)) {

		/*
		 * If this is the idle task on the outgoing CPU try to wake
		 * up the hotplug control thread which might wait for the
		 * last task to vanish. The rcuwait_active() check is
		 * accurate here because the waiter is pinned on this CPU
		 * and can't obviously be running in parallel.
		 *
		 * On RT kernels this also has to check whether there are
		 * pinned and scheduled out tasks on the runqueue. They
		 * need to leave the migrate disabled section first.
		 */
	/*
	 * 中文：若 outgoing CPU 只剩 idle 且无 pin，唤醒等待“最后 task 消失”的
	 * hotplug 控制线程。waiter 固定在本 CPU，rcuwait_active 快照可靠；RT 还须
	 * 等已调度出但仍 pinned 的 task 退出 migrate-disable。
	 */
		if (!rq->nr_running && !rq_has_pinned_tasks(rq) &&
		    rcuwait_active(&rq->hotplug_wait)) {
			raw_spin_rq_unlock(rq);
			rcuwait_wake_up(&rq->hotplug_wait);
			raw_spin_rq_lock(rq);
		}
		return;
	}

	get_task_struct(push_task);
	/*
	 * Temporarily drop rq->lock such that we can wake-up the stop task.
	 * Both preemption and IRQs are still disabled.
	 */
	/* 中文：临时释放 rq 锁以唤醒 stopper；抢占和 IRQ 仍关闭，当前 CPU 上下文稳定。 */
	preempt_disable();
	raw_spin_rq_unlock(rq);
	stop_one_cpu_nowait(rq->cpu, __balance_push_cpu_stop, push_task,
			    this_cpu_ptr(&push_work));
	preempt_enable();
	/*
	 * At this point need_resched() is true and we'll take the loop in
	 * schedule(). The next pick is obviously going to be the stop task
	 * which kthread_is_per_cpu() and will push this task away.
	 */
	raw_spin_rq_lock(rq);
}

/*
 * balance_push_set() 在目标 rq 锁下安装/移除全局 balance_push_callback。@on=true
 * 要求此前无其他 callback；false 只移除同一特殊节点，不误删普通 balance 工作。
 */
static void balance_push_set(int cpu, bool on)
{
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;

	rq_lock_irqsave(rq, &rf);
	if (on) {
		WARN_ON_ONCE(rq->balance_callback);
		rq->balance_callback = &balance_push_callback;
	} else if (rq->balance_callback == &balance_push_callback) {
		rq->balance_callback = NULL;
	}
	rq_unlock_irqrestore(rq, &rf);
}

/*
 * Invoked from a CPUs hotplug control thread after the CPU has been marked
 * inactive. All tasks which are not per CPU kernel threads are either
 * pushed off this CPU now via balance_push() or placed on a different CPU
 * during wakeup. Wait until the CPU is quiescent.
 */
/*
 * 原文：CPU 已 inactive 后，普通 task 会被 balance_push 或 wakeup placement 移走；
 * hotplug thread 在本 rq 的 rcuwait 上不可中断等待，直到只剩自己且无 pinned task。
 * 无入参/返回值，可睡眠。
 */
static void balance_hotplug_wait(void)
{
	struct rq *rq = this_rq();

	rcuwait_wait_event(&rq->hotplug_wait,
			   rq->nr_running == 1 && !rq_has_pinned_tasks(rq),
			   TASK_UNINTERRUPTIBLE);
}

#else /* !CONFIG_HOTPLUG_CPU: */

/* 无 CPU hotplug 时 push/set/wait stub 无副作用，rq 永不进入 dying 排空状态。 */

static inline void balance_push(struct rq *rq)
{
}

static inline void balance_push_set(int cpu, bool on)
{
}

static inline void balance_hotplug_wait(void)
{
}

#endif /* !CONFIG_HOTPLUG_CPU */

/*
 * set_rq_online()/offline() 要求调用者持 @rq 锁。online 先把 CPU 加入 root_domain
 * online mask、置标志，再通知所有 sched_class；offline 先更新 clock/通知 class，
 * 再从 mask 摘除并清标志。顺序让 class callback 看到与目标阶段一致的 domain。
 */
void set_rq_online(struct rq *rq)
{
	if (!rq->online) {
		const struct sched_class *class;

		cpumask_set_cpu(rq->cpu, rq->rd->online);
		rq->online = 1;

		for_each_class(class) {
			if (class->rq_online)
				class->rq_online(rq);
		}
	}
}

/*
 * set_rq_offline() 在 rq 锁下先推进时钟并通知各 sched_class，再从 root_domain
 * online mask 摘除 CPU、清 online。该顺序让 class 能在旧 domain 仍可见时收尾。
 */
void set_rq_offline(struct rq *rq)
{
	if (rq->online) {
		const struct sched_class *class;

		update_rq_clock(rq);
		for_each_class(class) {
			if (class->rq_offline)
				class->rq_offline(rq);
		}

		cpumask_clear_cpu(rq->cpu, rq->rd->online);
		rq->online = 0;
	}
}

/*
 * sched_set_rq_online/offline() 是保存 IRQ并锁 rq 的 hotplug wrapper；仅 rq 已挂
 * root_domain 时执行，并 BUG 检查 @cpu 属于 rd span。无返回值。
 */
static inline void sched_set_rq_online(struct rq *rq, int cpu)
{
	struct rq_flags rf;

	rq_lock_irqsave(rq, &rf);
	if (rq->rd) {
		BUG_ON(!cpumask_test_cpu(cpu, rq->rd->span));
		set_rq_online(rq);
	}
	rq_unlock_irqrestore(rq, &rf);
}

static inline void sched_set_rq_offline(struct rq *rq, int cpu)
{
	struct rq_flags rf;

	rq_lock_irqsave(rq, &rf);
	if (rq->rd) {
		BUG_ON(!cpumask_test_cpu(cpu, rq->rd->span));
		set_rq_offline(rq);
	}
	rq_unlock_irqrestore(rq, &rf);
}

/*
 * used to mark begin/end of suspend/resume:
 */
/*
 * 原文：num_cpus_frozen 统计 suspend/resume 批次中尚未恢复的 CPU，用于暂时忽略
 * cpuset 拓扑、只建立单一 sched domain；最后一个 CPU online 时重建原配置。
 * hotplug core 串行更新，无需普通运行期锁。
 */
static int num_cpus_frozen;

/*
 * Update cpusets according to cpu_active mask.  If cpusets are
 * disabled, cpuset_update_active_cpus() becomes a simple wrapper
 * around partition_sched_domains().
 *
 * If we come here as part of a suspend/resume, don't touch cpusets because we
 * want to restore it back to its original state upon resume anyway.
 */
/*
 * 原文：普通 hotplug 按 cpu_active mask 更新 cpuset/domain；suspend/resume 中
 * 暂不改用户 cpuset，因为 resume 最终应恢复原拓扑。inactive 增 frozen 计数并
 * reset domain；active 递减，最后一个恢复时 force rebuild。
 */
static void cpuset_cpu_active(void)
{
	if (cpuhp_tasks_frozen) {
		/*
		 * num_cpus_frozen tracks how many CPUs are involved in suspend
		 * resume sequence. As long as this is not the last online
		 * operation in the resume sequence, just build a single sched
		 * domain, ignoring cpusets.
		 */
		cpuset_reset_sched_domains();
		if (--num_cpus_frozen)
			return;
		/*
		 * This is the last CPU online operation. So fall through and
		 * restore the original sched domains by considering the
		 * cpuset configurations.
		 */
		cpuset_force_rebuild();
	}
	cpuset_update_active_cpus();
}

/*
 * cpuset_cpu_inactive() 普通 hotplug 立即按 active mask 更新 cpuset；suspend
 * 批次则只递增 frozen 计数并折叠为单 sched domain，待最后恢复 CPU 时整体重建。
 */
static void cpuset_cpu_inactive(unsigned int cpu)
{
	if (!cpuhp_tasks_frozen) {
		cpuset_update_active_cpus();
	} else {
		num_cpus_frozen++;
		cpuset_reset_sched_domains();
	}
}

/*
 * sched_smt_present_inc()/dec() 在 cpu-hotplug 锁下维护“至少一个双线程 SMT core”
 * 静态键引用。只有 sibling mask 权重跨到/离开 2 时调整，避免每 CPU 重复计数。
 */
static inline void sched_smt_present_inc(int cpu)
{
	if (cpumask_weight(cpu_smt_mask(cpu)) == 2)
		static_branch_inc_cpuslocked(&sched_smt_present);
}

static inline void sched_smt_present_dec(int cpu)
{
	if (cpumask_weight(cpu_smt_mask(cpu)) == 2)
		static_branch_dec_cpuslocked(&sched_smt_present);
}

/*
 * sched_cpu_activate() - 把 @cpu 从 online-but-inactive 推进到可接收普通 task。
 *
 * CPU hotplug 核心线程调用，外层 cpus_write_lock 稳定拓扑。先关闭 balance_push，
 * 更新 SMT 静态键并发布 cpu_active；随后重建 NUMA/domain/cpuset 视图、激活 SCX
 * 和 rq。返回 0；无对象 ownership 转移。发布 active 后唤醒路径可选择该 CPU，
 * 所以此前必须完成阻止旧排空逻辑的切换。
 */
int sched_cpu_activate(unsigned int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	/*
	 * Clear the balance_push callback and prepare to schedule
	 * regular tasks.
	 */
	balance_push_set(cpu, false);

	/*
	 * When going up, increment the number of cores with SMT present.
	 */
	/* 中文：CPU active 前更新 SMT-present 静态键引用。 */
	sched_smt_present_inc(cpu);
	set_cpu_active(cpu, true);

	if (sched_smp_initialized) {
		sched_update_numa(cpu, true);
		sched_domains_numa_masks_set(cpu);
		cpuset_cpu_active();
	}

	scx_rq_activate(rq);

	/*
	 * Put the rq online, if not already. This happens:
	 *
	 * 1) In the early boot process, because we build the real domains
	 *    after all CPUs have been brought up.
	 *
	 * 2) At runtime, if cpuset_cpu_active() fails to rebuild the
	 *    domains.
	 */
	sched_set_rq_online(rq, cpu);

	return 0;
}

/*
 * sched_cpu_deactivate() - 阻止 @cpu 接收普通 task 并启动 runqueue 排空。
 *
 * 先让 deadline 带宽层确认可下线；失败直接返回 errno，CPU 保持 active。成功后
 * 从 nohz 平衡集合摘除、清 cpu_active 并开启 balance_push。synchronize_rcu()
 * 使旧的无锁/RCU 观察者退出，保证后续 ttwu 不再把新 task 放到该 CPU；随后更新
 * domain/cpuset/SMT 等视图。返回 0 只表示 deactivate 阶段完成，真正 dying 阶段
 * 还会检查并迁走剩余 task。
 */
int sched_cpu_deactivate(unsigned int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	int ret;

	ret = dl_bw_deactivate(cpu);

	if (ret)
		return ret;

	/*
	 * Remove CPU from nohz.idle_cpus_mask to prevent participating in
	 * load balancing when not active
	 */
	/* 中文：清 active 前先从 nohz idle 平衡集合移除，防止继续参与拉/推任务。 */
	scoped_guard (rcu)
		nohz_balance_exit_idle(rq);

	set_cpu_active(cpu, false);

	/*
	 * From this point forward, this CPU will refuse to run any task that
	 * is not: migrate_disable() or KTHREAD_IS_PER_CPU, and will actively
	 * push those tasks away until this gets cleared, see
	 * sched_cpu_dying().
	 */
	/*
	 * 中文：从此仅允许 migration-disabled 与 per-CPU kthread；balance_push 会持续
	 * 推走其他 task，直到 dying 阶段撤销该机制。
	 */
	balance_push_set(cpu, true);

	/*
	 * We've cleared cpu_active_mask / set balance_push, wait for all
	 * preempt-disabled and RCU users of this state to go away such that
	 * all new such users will observe it.
	 *
	 * Specifically, we rely on ttwu to no longer target this CPU, see
	 * ttwu_queue_cond() and is_cpu_allowed().
	 *
	 * Do sync before park smpboot threads to take care the RCU boost case.
	 */
	/*
	 * 中文：清 active 并安装 push 后等待所有旧 RCU/不可抢占读者退出，使新 ttwu
	 * 必然看到 CPU 不可选；在 park smpboot 线程前同步也覆盖 RCU boost 场景。
	 */
	synchronize_rcu();

	sched_domains_free_llc_id(cpu);

	sched_set_rq_offline(rq, cpu);

	scx_rq_deactivate(rq);

	/*
	 * When going down, decrement the number of cores with SMT present.
	 */
	/* 中文：CPU 下线时对称减少 SMT-present 静态键引用。 */
	sched_smt_present_dec(cpu);

	sched_core_cpu_deactivate(cpu);

	if (!sched_smp_initialized)
		return 0;

	sched_update_numa(cpu, false);
	cpuset_cpu_inactive(cpu);
	sched_domains_numa_masks_clear(cpu);
	return 0;
}

/*
 * sched_rq_cpu_starting() 初始化新启动 rq 的全局负载更新时间并重算最大平衡间隔；
 * sched_cpu_starting() 还连接 core leader、启动 full-nohz 远端 tick。返回 0。
 */
static void sched_rq_cpu_starting(unsigned int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	rq->calc_load_update = calc_load_update;
	update_max_interval();
}

int sched_cpu_starting(unsigned int cpu)
{
	sched_core_cpu_starting(cpu);
	sched_rq_cpu_starting(cpu);
	sched_tick_start(cpu);
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU

/*
 * Invoked immediately before the stopper thread is invoked to bring the
 * CPU down completely. At this point all per CPU kthreads except the
 * hotplug thread (current) and the stopper thread (inactive) have been
 * either parked or have been unbound from the outgoing CPU. Ensure that
 * any of those which might be on the way out are gone.
 *
 * If after this point a bound task is being woken on this CPU then the
 * responsible hotplug callback has failed to do it's job.
 * sched_cpu_dying() will catch it with the appropriate fireworks.
 */
/*
 * 原文：在 stopper 真正下线 CPU 前调用，此时除 hotplug current 与 inactive stopper
 * 外的 per-CPU kthread 已 park/unbind；等待在途 task 完全离开，再把 current 的
 * lazy mm 切到 init_mm。返回 0，若此后仍唤醒 bound task 是对应 hotplug callback
 * 的严重错误，dying 阶段会检查。
 */
int sched_cpu_wait_empty(unsigned int cpu)
{
	balance_hotplug_wait();
	sched_force_init_mm();
	return 0;
}

/*
 * Since this CPU is going 'away' for a while, fold any nr_active delta we
 * might have. Called from the CPU stopper task after ensuring that the
 * stopper is the last running task on the CPU, so nr_active count is
 * stable. We need to take the tear-down thread which is calling this into
 * account, so we hand in adjust = 1 to the load calculation.
 *
 * Also see the comment "Global load-average calculations".
 */
/*
 * 原文：CPU 即将长期离线，把 rq 尚未折叠的 nr_active delta 加回全局 load；
 * stopper 已是最后 task，计数稳定，但 teardown thread 本身需以 adjust=1 扣除。
 * 无返回值。
 */
static void calc_load_migrate(struct rq *rq)
{
	long delta = calc_load_fold_active(rq, 1);

	if (delta)
		atomic_long_add(delta, &calc_load_tasks);
}

/*
 * dump_rq_tasks() 在已锁 @rq 下遍历全 task，输出仍归属该 CPU 且 queued 的条目。
 * @loglvl 是借用 printk 前缀；仅诊断 dying 排空失败，不取得 task 引用，外层锁
 * 稳定目标 rq 成员关系。
 */
static void dump_rq_tasks(struct rq *rq, const char *loglvl)
{
	struct task_struct *g, *p;
	int cpu = cpu_of(rq);

	lockdep_assert_rq_held(rq);

	printk("%sCPU%d enqueued tasks (%u total):\n", loglvl, cpu, rq->nr_running);
	for_each_process_thread(g, p) {
		if (task_cpu(p) != cpu)
			continue;

		if (!task_on_rq_queued(p))
			continue;

		printk("%s\tpid: %d, name: %s\n", loglvl, p->pid, p->comm);
	}
}

/*
 * sched_cpu_dying() 在 outgoing CPU stopper 上执行最终收尾：停远端 tick，锁 rq
 * 验证只剩 stopper且无 pinned task，停止 fair/ext DL server；随后折叠 load、
 * 清 hrtick/core leader。异常存量 task WARN 并 dump，但返回 0 让 hotplug 框架
 * 继续其错误处理。
 */
int sched_cpu_dying(unsigned int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;

	/* Handle pending wakeups and then migrate everything off */
	sched_tick_stop(cpu);

	rq_lock_irqsave(rq, &rf);
	update_rq_clock(rq);
	if (rq->nr_running != 1 || rq_has_pinned_tasks(rq)) {
		WARN(true, "Dying CPU not properly vacated!");
		dump_rq_tasks(rq, KERN_WARNING);
	}
	dl_server_stop(&rq->fair_server);
#ifdef CONFIG_SCHED_CLASS_EXT
	dl_server_stop(&rq->ext_server);
#endif
	rq_unlock_irqrestore(rq, &rf);

	calc_load_migrate(rq);
	update_max_interval();
	hrtick_clear(rq);
	sched_core_cpu_dying(cpu);
	return 0;
}
#endif /* CONFIG_HOTPLUG_CPU */

/*
 * sched_init_smp() 在用户态/hotplug 尚未出现时建立真实 sched domains、per-CPU
 * 随机状态和 RT/DL SMP class；把 init task 移到非隔离 housekeeping CPU，最后
 * 发布 sched_smp_initialized。无返回值，关键 affinity 失败 BUG。
 */
void __init sched_init_smp(void)
{
	sched_init_numa(NUMA_NO_NODE);

	prandom_init_once(&sched_rnd_state);

	/*
	 * There's no userspace yet to cause hotplug operations; hence all the
	 * CPU masks are stable and all blatant races in the below code cannot
	 * happen.
	 */
	sched_domains_mutex_lock();
	sched_init_domains(cpu_active_mask);
	sched_domains_mutex_unlock();

	/* Move init over to a non-isolated CPU */
	/* 中文：把 init 迁到非隔离 housekeeping CPU，避免占用用户专用隔离核。 */
	if (set_cpus_allowed_ptr(current, housekeeping_cpumask(HK_TYPE_DOMAIN)) < 0)
		BUG();
	current->flags &= ~PF_NO_SETAFFINITY;
	sched_init_granularity();

	init_sched_rt_class();
	init_sched_dl_class();

	sched_init_dl_servers();

	sched_smp_initialized = true;
}

/*
 * migration_init() 是 early_initcall，为 boot CPU 执行与 secondary CPU 相同的
 * sched_cpu_starting 初始化。返回 0。
 */
static int __init migration_init(void)
{
	sched_cpu_starting(smp_processor_id());
	return 0;
}
early_initcall(migration_init);

/*
 * in_sched_functions() 判断指令地址是否位于通用锁函数或 __sched_text 区间，用于
 * stack unwinding/wchan 过滤调度器内部帧。返回 bool 语义的 int，纯查询。
 */
int in_sched_functions(unsigned long addr)
{
	return in_lock_functions(addr) ||
		(addr >= (unsigned long)__sched_text_start
		&& addr < (unsigned long)__sched_text_end);
}

#ifdef CONFIG_CGROUP_SCHED
/*
 * Default task group.
 * Every task in system belongs to this group at bootup.
 */
/*
 * 原文：root_task_group 是启动时所有 task 的默认组；task_groups 是 RCU 可遍历的
 * 全局组链。root 生命周期永久，不经普通 create/destroy。
 */
struct task_group root_task_group;
LIST_HEAD(task_groups);

/* Cacheline aligned slab cache for task_group */
/* 原文：task_group_cache 是启动后只读的 cacheline 对齐 slab cache 描述符。 */
static struct kmem_cache *task_group_cache __ro_after_init;
#endif

/*
 * sched_init() - 在 SMP/用户进程启动前构造全局与每 CPU 调度器基础状态。
 *
 * 入参、直接返回值均无，__init 表示代码启动后可回收。此时只有启动 CPU 且没有
 * 并发用户态/hotplug，函数可初始化永久 runqueues、root_task_group、各 class
 * per-CPU 队列、带宽 timer、idle/current 指针、锁和静态键，而无需正常运行期的
 * 全套锁协议。分配 root RT group 采用 GFP_NOWAIT；关键基础对象不足属于无法继续
 * 启动的条件。最后发布 scheduler_running，之后普通调度入口才可依赖这些不变量。
 */
void __init sched_init(void)
{
	unsigned long __maybe_unused ptr = 0;
	int i;

	/* Make sure the linker didn't screw up */
	BUG_ON(!sched_class_above(&stop_sched_class, &dl_sched_class));
	BUG_ON(!sched_class_above(&dl_sched_class, &rt_sched_class));
	BUG_ON(!sched_class_above(&rt_sched_class, &fair_sched_class));
	BUG_ON(!sched_class_above(&fair_sched_class, &idle_sched_class));
#ifdef CONFIG_SCHED_CLASS_EXT
	BUG_ON(!sched_class_above(&fair_sched_class, &ext_sched_class));
	BUG_ON(!sched_class_above(&ext_sched_class, &idle_sched_class));
#endif

	wait_bit_init();

#ifdef CONFIG_FAIR_GROUP_SCHED
	root_task_group.cfs_rq = &runqueues.cfs;

	root_task_group.shares = ROOT_TASK_GROUP_LOAD;
	init_cfs_bandwidth(&root_task_group.cfs_bandwidth, NULL);
#endif /* CONFIG_FAIR_GROUP_SCHED */
#ifdef CONFIG_EXT_GROUP_SCHED
	scx_tg_init(&root_task_group);
#endif /* CONFIG_EXT_GROUP_SCHED */
#ifdef CONFIG_RT_GROUP_SCHED
	ptr += 2 * nr_cpu_ids * sizeof(void **);
	ptr = (unsigned long)kzalloc(ptr, GFP_NOWAIT);
	root_task_group.rt_se = (struct sched_rt_entity **)ptr;
	ptr += nr_cpu_ids * sizeof(void **);

	root_task_group.rt_rq = (struct rt_rq **)ptr;
	ptr += nr_cpu_ids * sizeof(void **);

#endif /* CONFIG_RT_GROUP_SCHED */

	init_defrootdomain();

#ifdef CONFIG_RT_GROUP_SCHED
	init_rt_bandwidth(&root_task_group.rt_bandwidth,
			global_rt_period(), global_rt_runtime());
#endif /* CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_CGROUP_SCHED
	task_group_cache = KMEM_CACHE(task_group, 0);

	list_add(&root_task_group.list, &task_groups);
	INIT_LIST_HEAD(&root_task_group.children);
	INIT_LIST_HEAD(&root_task_group.siblings);
	autogroup_init(&init_task);
#endif /* CONFIG_CGROUP_SCHED */

	for_each_possible_cpu(i) {
		struct rq *rq;

		rq = cpu_rq(i);
		raw_spin_lock_init(&rq->__lock);
		rq->nr_running = 0;
		rq->calc_load_active = 0;
		rq->calc_load_update = jiffies + LOAD_FREQ;
		init_cfs_rq(&rq->cfs);
		init_rt_rq(&rq->rt);
		init_dl_rq(&rq->dl);
#ifdef CONFIG_FAIR_GROUP_SCHED
		INIT_LIST_HEAD(&rq->leaf_cfs_rq_list);
		rq->tmp_alone_branch = &rq->leaf_cfs_rq_list;
		/*
		 * How much CPU bandwidth does root_task_group get?
		 *
		 * In case of task-groups formed through the cgroup filesystem, it
		 * gets 100% of the CPU resources in the system. This overall
		 * system CPU resource is divided among the tasks of
		 * root_task_group and its child task-groups in a fair manner,
		 * based on each entity's (task or task-group's) weight
		 * (se->load.weight).
		 *
		 * In other words, if root_task_group has 10 tasks of weight
		 * 1024) and two child groups A0 and A1 (of weight 1024 each),
		 * then A0's share of the CPU resource is:
		 *
		 *	A0's bandwidth = 1024 / (10*1024 + 1024 + 1024) = 8.33%
		 *
		 * We achieve this by letting root_task_group's tasks sit
		 * directly in rq->cfs (i.e root_task_group->se[] = NULL).
		 */
		/*
		 * 中文：root_task_group 获得系统 100% CPU 资源，再按实体权重在根 task
		 * 与子组间公平分配。例如 10 个权重 1024 的根 task 加两个同权子组时，
		 * A0 份额为 1024/(10*1024+1024+1024)=8.33%。根 task 直接位于 rq->cfs，
		 * 因而 root_task_group->se[] 为 NULL，不再套一层组实体。
		 */
		init_tg_cfs_entry(&root_task_group, &rq->cfs, NULL, i, NULL);
#endif /* CONFIG_FAIR_GROUP_SCHED */

#ifdef CONFIG_RT_GROUP_SCHED
		/*
		 * This is required for init cpu because rt.c:__enable_runtime()
		 * starts working after scheduler_running, which is not the case
		 * yet.
		 */
	/*
	 * 中文：init CPU 此时 scheduler_running 尚未置位，rt.c 的 __enable_runtime()
	 * 还不会补 runtime，因此必须显式初始化根 RT rq 的全局 runtime。
	 */
		rq->rt.rt_runtime = global_rt_runtime();
		init_tg_rt_entry(&root_task_group, &rq->rt, NULL, i, NULL);
#endif
		rq->next_class = &idle_sched_class;

		rq->sd = NULL;
		rq->rd = NULL;
		rq->cpu_capacity = SCHED_CAPACITY_SCALE;
		rq->balance_callback = &balance_push_callback;
		rq->active_balance = 0;
		rq->next_balance = jiffies;
		rq->push_cpu = 0;
		rq->cpu = i;
		rq->online = 0;
		rq->idle_stamp = 0;
		rq->avg_idle = 2*sysctl_sched_migration_cost;
		rq->max_idle_balance_cost = sysctl_sched_migration_cost;

		INIT_LIST_HEAD(&rq->cfs_tasks);

		rq_attach_root(rq, &def_root_domain);
#ifdef CONFIG_NO_HZ_COMMON
		rq->last_blocked_load_update_tick = jiffies;
		atomic_set(&rq->nohz_flags, 0);

		INIT_CSD(&rq->nohz_csd, nohz_csd_func, rq);
#endif
#ifdef CONFIG_HOTPLUG_CPU
		rcuwait_init(&rq->hotplug_wait);
#endif
		hrtick_rq_init(rq);
		atomic_set(&rq->nr_iowait, 0);
		fair_server_init(rq);
#ifdef CONFIG_SCHED_CLASS_EXT
		ext_server_init(rq);
#endif

#ifdef CONFIG_SCHED_CORE
		rq->core = rq;
		rq->core_pick = NULL;
		rq->core_dl_server = NULL;
		rq->core_enabled = 0;
		rq->core_tree = RB_ROOT;
		rq->core_forceidle_count = 0;
		rq->core_forceidle_occupation = 0;
		rq->core_forceidle_start = 0;

		rq->core_cookie = 0UL;
#endif
#ifdef CONFIG_SCHED_CACHE
		raw_spin_lock_init(&rq->cpu_epoch_lock);
		rq->cpu_epoch_next = jiffies;
#endif

		zalloc_cpumask_var_node(&rq->scratch_mask, GFP_KERNEL, cpu_to_node(i));
	}

	set_load_weight(&init_task, false);
	init_task.se.slice = sysctl_sched_base_slice,

	/*
	 * The boot idle thread does lazy MMU switching as well:
	 */
	/* 中文：boot idle 线程同样借用 init_mm 并进入 lazy TLB 模式。 */
	mmgrab_lazy_tlb(&init_mm);
	enter_lazy_tlb(&init_mm, current);

	/*
	 * The idle task doesn't need the kthread struct to function, but it
	 * is dressed up as a per-CPU kthread and thus needs to play the part
	 * if we want to avoid special-casing it in code that deals with per-CPU
	 * kthreads.
	 */
	/*
	 * 中文：idle 本身不依赖 kthread_struct，但既然伪装为 per-CPU kthread，就需
	 * 建立该结构以复用所有通用 kthread 路径，避免处处特判 idle。
	 */
	WARN_ON(!set_kthread_struct(current));

	/*
	 * Make us the idle thread. Technically, schedule() should not be
	 * called from this thread, however somewhere below it might be,
	 * but because we are the idle thread, we just pick up running again
	 * when this runqueue becomes "idle".
	 */
	/*
	 * 中文：把 boot task 转成 idle。原则上它不应主动 schedule，但早期代码若
	 * 调用，作为 idle 仍会在 rq 再次空闲时被选回继续执行。
	 */
	__sched_fork(0, current);
	init_idle(current, smp_processor_id());

	calc_load_update = jiffies + LOAD_FREQ;

	idle_thread_set_boot_cpu();

	balance_push_set(smp_processor_id(), false);
	init_sched_fair_class();
	init_sched_ext_class();

	psi_init();

	init_uclamp();

	preempt_dynamic_init();

	scheduler_running = 1;
}

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP

/*
 * __might_sleep()/__might_resched() 是阻塞 API 的调试断言。@file/@line 标识调用点；
 * 前者先警告 current 非 RUNNING 时阻塞会破坏已有 state，再由后者核对 preempt/
 * RCU nesting、IRQ、idle、non_block_count。非法上下文按 1Hz 限速打印锁、IRQ、
 * disable IP 和栈并 taint；无返回值，不改变调度语义。
 */
void __might_sleep(const char *file, int line)
{
	unsigned int state = get_current_state();
	/*
	 * Blocking primitives will set (and therefore destroy) current->state,
	 * since we will exit with TASK_RUNNING make sure we enter with it,
	 * otherwise we will destroy state.
	 */
	WARN_ONCE(state != TASK_RUNNING && current->task_state_change,
			"do not call blocking ops when !TASK_RUNNING; "
			"state=%x set at [<%p>] %pS\n", state,
			(void *)current->task_state_change,
			(void *)current->task_state_change);

	__might_resched(file, line, 0);
}
EXPORT_SYMBOL(__might_sleep);

/*
 * print_preempt_disable_ip() 仅 DEBUG_PREEMPT 且实际计数不同于允许 @offset 时打印
 * 首次 disable @ip；纯诊断 helper。
 */
static void print_preempt_disable_ip(int preempt_offset, unsigned long ip)
{
	if (!IS_ENABLED(CONFIG_DEBUG_PREEMPT))
		return;

	if (preempt_count() == preempt_offset)
		return;

	pr_err("Preemption disabled at:");
	print_ip_sym(KERN_ERR, ip);
}

/*
 * resched_offsets_ok() 把 preempt_count 与 RCU nesting 编码成统一值，与调用点允许
 * @offsets 比较。返回 bool 快照，不修改计数。
 */
static inline bool resched_offsets_ok(unsigned int offsets)
{
	unsigned int nested = preempt_count();

	nested += rcu_preempt_depth() << MIGHT_RESCHED_RCU_SHIFT;

	return nested == offsets;
}

void __might_resched(const char *file, int line, unsigned int offsets)
{
	/* Ratelimiting timestamp: */
	/* 中文：保存上次告警 jiffy，限制无效上下文睡眠日志频率。 */
	static unsigned long prev_jiffy;

	unsigned long preempt_disable_ip;

	/* WARN_ON_ONCE() by default, no rate limit required: */
	/* 中文：RCU 睡眠检查自身默认 WARN_ON_ONCE，无需套本函数的限频。 */
	rcu_sleep_check();

	if ((resched_offsets_ok(offsets) && !irqs_disabled() &&
	     !is_idle_task(current) && !current->non_block_count) ||
	    system_state == SYSTEM_BOOTING || system_state > SYSTEM_RUNNING ||
	    oops_in_progress)
		return;

	if (time_before(jiffies, prev_jiffy + HZ) && prev_jiffy)
		return;
	prev_jiffy = jiffies;

	/* Save this before calling printk(), since that will clobber it: */
	/* 中文：printk 会改变当前调用上下文信息，故预先保存禁抢占调用点。 */
	preempt_disable_ip = get_preempt_disable_ip(current);

	pr_err("BUG: sleeping function called from invalid context at %s:%d\n",
	       file, line);
	pr_err("in_atomic(): %d, irqs_disabled(): %d, non_block: %d, pid: %d, name: %s\n",
	       in_atomic(), irqs_disabled(), current->non_block_count,
	       current->pid, current->comm);
	pr_err("preempt_count: %x, expected: %x\n", preempt_count(),
	       offsets & MIGHT_RESCHED_PREEMPT_MASK);

	if (IS_ENABLED(CONFIG_PREEMPT_RCU)) {
		pr_err("RCU nest depth: %d, expected: %u\n",
		       rcu_preempt_depth(), offsets >> MIGHT_RESCHED_RCU_SHIFT);
	}

	if (task_stack_end_corrupted(current))
		pr_emerg("Thread overran stack, or stack corrupted\n");

	debug_show_held_locks(current);
	if (irqs_disabled())
		print_irqtrace_events(current);

	print_preempt_disable_ip(offsets & MIGHT_RESCHED_PREEMPT_MASK,
				 preempt_disable_ip);

	dump_stack();
	add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
}
EXPORT_SYMBOL(__might_resched);

/*
 * __cant_sleep() 反向断言调用点确实处于 atomic/不可睡眠上下文；IRQ-off、无
 * PREEMPT_COUNT 或计数高于 @preempt_offset 均满足。否则限速打印并 taint。
 */
void __cant_sleep(const char *file, int line, int preempt_offset)
{
	static unsigned long prev_jiffy;

	if (irqs_disabled())
		return;

	if (!IS_ENABLED(CONFIG_PREEMPT_COUNT))
		return;

	if (preempt_count() > preempt_offset)
		return;

	if (time_before(jiffies, prev_jiffy + HZ) && prev_jiffy)
		return;
	prev_jiffy = jiffies;

	printk(KERN_ERR "BUG: assuming atomic context at %s:%d\n", file, line);
	printk(KERN_ERR "in_atomic(): %d, irqs_disabled(): %d, pid: %d, name: %s\n",
			in_atomic(), irqs_disabled(),
			current->pid, current->comm);

	debug_show_held_locks(current);
	dump_stack();
	add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
}
EXPORT_SYMBOL_GPL(__cant_sleep);

# ifdef CONFIG_SMP
/*
 * __cant_migrate() 断言 current 不可迁移；IRQ-off、migration_disabled、无
 * PREEMPT_COUNT 或 preempt_count>0 均满足，否则限速报告。无返回值。
 */
void __cant_migrate(const char *file, int line)
{
	static unsigned long prev_jiffy;

	if (irqs_disabled())
		return;

	if (is_migration_disabled(current))
		return;

	if (!IS_ENABLED(CONFIG_PREEMPT_COUNT))
		return;

	if (preempt_count() > 0)
		return;

	if (time_before(jiffies, prev_jiffy + HZ) && prev_jiffy)
		return;
	prev_jiffy = jiffies;

	pr_err("BUG: assuming non migratable context at %s:%d\n", file, line);
	pr_err("in_atomic(): %d, irqs_disabled(): %d, migration_disabled() %u pid: %d, name: %s\n",
	       in_atomic(), irqs_disabled(), is_migration_disabled(current),
	       current->pid, current->comm);

	debug_show_held_locks(current);
	dump_stack();
	add_taint(TAINT_WARN, LOCKDEP_STILL_OK);
}
EXPORT_SYMBOL_GPL(__cant_migrate);
# endif /* CONFIG_SMP */
#endif /* CONFIG_DEBUG_ATOMIC_SLEEP */

#ifdef CONFIG_MAGIC_SYSRQ
/*
 * normalize_rt_tasks() 是 Magic SysRq 恢复工具：tasklist_lock 读侧遍历所有用户
 * task，清调度时间戳；负 nice 恢复 0，RT/DL 强制改 SCHED_NORMAL。kthread 跳过。
 * 无返回值，目的是从失控实时优先级中恢复系统，而非保持原 policy。
 */
void normalize_rt_tasks(void)
{
	struct task_struct *g, *p;
	struct sched_attr attr = {
		.sched_policy = SCHED_NORMAL,
	};

	read_lock(&tasklist_lock);
	for_each_process_thread(g, p) {
		/*
		 * Only normalize user tasks:
		 */
	/* 中文：仅规范化用户 task，内核线程的策略由其子系统不变量管理。 */
		if (p->flags & PF_KTHREAD)
			continue;

		p->se.exec_start = 0;
		schedstat_set(p->stats.wait_start,  0);
		schedstat_set(p->stats.sleep_start, 0);
		schedstat_set(p->stats.block_start, 0);

		if (!rt_or_dl_task(p)) {
			/*
			 * Renice negative nice level userspace
			 * tasks back to 0:
			 */
			/* 中文：非 RT/DL 用户 task 若为负 nice，也恢复到普通 nice 0。 */
			if (task_nice(p) < 0)
				set_user_nice(p, 0);
			continue;
		}

		__sched_setscheduler(p, &attr, false, false);
	}
	read_unlock(&tasklist_lock);
}

#endif /* CONFIG_MAGIC_SYSRQ */

#ifdef CONFIG_KGDB_KDB
/*
 * These functions are only useful for KDB.
 *
 * They can only be called when the whole system has been
 * stopped - every CPU needs to be quiescent, and no scheduling
 * activity can take place. Using them for anything else would
 * be a serious bug, and as a result, they aren't even visible
 * under any other configuration.
 */
/*
 * 原文：以下 KDB helper 只在全系统 stop、每 CPU 静止且无调度活动时有效；其他
 * 场景读取裸 curr 会产生严重竞态，所以仅在 KDB 配置可见。
 */

/**
 * curr_task - return the current task for a given CPU.
 * @cpu: the processor in question.
 *
 * ONLY VALID WHEN THE WHOLE SYSTEM IS STOPPED!
 *
 * Return: The current task for @cpu.
 */
/*
 * 原文：返回 @cpu 的裸 current task 指针，仅全系统停止时有效，不增加引用。
 */
struct task_struct *curr_task(int cpu)
{
	return cpu_curr(cpu);
}

#endif /* CONFIG_KGDB_KDB */

#ifdef CONFIG_CGROUP_SCHED
/* task_group_lock serializes the addition/removal of task groups */
/*
 * 原文：task_group_lock 串行化 task group 的加入/删除。它保护全局 task_groups
 * 与 parent->children 链接的结构修改；RCU 读者可在不持锁时遍历，但对象释放
 * 必须等宽限期。该锁不保护每个 cfs_rq/rt_rq 的运行时负载，后者仍由 rq 锁负责。
 */
static DEFINE_SPINLOCK(task_group_lock);

/*
 * alloc_uclamp_sched_group() 为未发布 @tg 初始化无显式请求，并继承 @parent 有效
 * clamp；未启用 group uclamp 时为空。无分配/失败，两个指针均由 create 路径稳定。
 */
static inline void alloc_uclamp_sched_group(struct task_group *tg,
					    struct task_group *parent)
{
#ifdef CONFIG_UCLAMP_TASK_GROUP
	enum uclamp_id clamp_id;

	for_each_clamp_id(clamp_id) {
		uclamp_se_set(&tg->uclamp_req[clamp_id],
			      uclamp_none(clamp_id), false);
		tg->uclamp[clamp_id] = parent->uclamp[clamp_id];
	}
#endif
}

/*
 * sched_free_group() 是 task_group 最终析构：按 class/autogroup 子资源后释放 slab。
 * 只可在已摘除且所有 RCU 读者退出后调用；@tg ownership 在此被消费。
 */
static void sched_free_group(struct task_group *tg)
{
	free_fair_sched_group(tg);
	free_rt_sched_group(tg);
	autogroup_free(tg);
	kmem_cache_free(task_group_cache, tg);
}

/*
 * sched_free_group_rcu() 从内嵌 rcu_head 恢复 tg 并执行最终析构；运行在 RCU
 * callback 上下文，不能睡眠。
 */
static void sched_free_group_rcu(struct rcu_head *rcu)
{
	sched_free_group(container_of(rcu, struct task_group, rcu));
}

/*
 * sched_unregister_group() 注销 fair/rt 每 CPU 状态后再排第二个 RCU callback。
 * 额外宽限期保护仍可能并发的 print_cfs_stats；此阶段 tg 内存仍存在但不可新发现。
 */
static void sched_unregister_group(struct task_group *tg)
{
	unregister_fair_sched_group(tg);
	unregister_rt_sched_group(tg);
	/*
	 * We have to wait for yet another RCU grace period to expire, as
	 * print_cfs_stats() might run concurrently.
	 */
	call_rcu(&tg->rcu, sched_free_group_rcu);
}

/* allocate runqueue etc for a new task group */
/*
 * 原文：为新 task group 分配各 CPU runqueue 等私有状态。
 *
 * @parent 是已发布且存活的父组借用指针。函数可睡眠，先从 slab 分配零化 tg，
 * 再依次分配 fair/rt 状态并初始化 SCX/uclamp；成功返回尚未 online 的持有指针，
 * ownership 交给 cgroup 创建路径。任一步失败逆序由 sched_free_group() 释放，
 * 返回 ERR_PTR(-ENOMEM)，没有半初始化组进入 RCU 可见链表。
 */
struct task_group *sched_create_group(struct task_group *parent)
{
	struct task_group *tg;

	tg = kmem_cache_alloc(task_group_cache, GFP_KERNEL | __GFP_ZERO);
	if (!tg)
		return ERR_PTR(-ENOMEM);

	if (!alloc_fair_sched_group(tg, parent))
		goto err;

	if (!alloc_rt_sched_group(tg, parent))
		goto err;

	scx_tg_init(tg);
	alloc_uclamp_sched_group(tg, parent);

	return tg;

err:
	sched_free_group(tg);
	return ERR_PTR(-ENOMEM);
}

/*
 * sched_online_group() 在 task_group_lock 下把完整 @tg 发布到全局 RCU 链及
 * @parent children，随后 online fair 状态。@parent 必须非空已发布；无返回值，
 * 此点后并发遍历者可获得 tg。
 */
void sched_online_group(struct task_group *tg, struct task_group *parent)
{
	unsigned long flags;

	spin_lock_irqsave(&task_group_lock, flags);
	list_add_tail_rcu(&tg->list, &task_groups);

	/* Root should already exist: */
	WARN_ON(!parent);

	tg->parent = parent;
	INIT_LIST_HEAD(&tg->children);
	list_add_rcu(&tg->siblings, &parent->children);
	spin_unlock_irqrestore(&task_group_lock, flags);

	online_fair_sched_group(tg);
}

/* RCU callback to free various structures associated with a task group */
/*
 * 中文：RCU 回调在所有旧遍历者退出后，把 rcu_head 还原为 task_group 并执行
 * 真正注销；此时释放 per-CPU cfs_rq 等结构才安全。
 */
static void sched_unregister_group_rcu(struct rcu_head *rhp)
{
	/* Now it should be safe to free those cfs_rqs: */
	sched_unregister_group(container_of(rhp, struct task_group, rcu));
}

/*
 * sched_destroy_group() 启动 task_group 延迟销毁的 RCU 宽限期；函数返回时内存
 * 仍可能存活，最终 ownership 由 sched_unregister_group_rcu() 收尾。
 */
void sched_destroy_group(struct task_group *tg)
{
	/* Wait for possible concurrent references to cfs_rqs complete: */
	call_rcu(&tg->rcu, sched_unregister_group_rcu);
}

/*
 * sched_release_group() 负责第一阶段“摘除”：在 task_group_lock 下从两个 RCU
 * 链表删除，阻止新遍历者找到 tg；sched_destroy_group() 负责第二阶段“延迟销毁”，
 * 第一个宽限期后注销 per-class 状态，再经过额外宽限期释放内存。两阶段是因为
 * print_cfs_stats 等读者可能持有从旧 cfs_rq 得到的引用。调用者不能把摘除等同于
 * 内存已经释放。
 */

void sched_release_group(struct task_group *tg)
{
	unsigned long flags;

	/*
	 * Unlink first, to avoid walk_tg_tree_from() from finding us (via
	 * sched_cfs_period_timer()).
	 *
	 * For this to be effective, we have to wait for all pending users of
	 * this task group to leave their RCU critical section to ensure no new
	 * user will see our dying task group any more. Specifically ensure
	 * that tg_unthrottle_up() won't add decayed cfs_rq's to it.
	 *
	 * We therefore defer calling unregister_fair_sched_group() to
	 * sched_unregister_group() which is guarantied to get called only after the
	 * current RCU grace period has expired.
	 */
	spin_lock_irqsave(&task_group_lock, flags);
	list_del_rcu(&tg->list);
	list_del_rcu(&tg->siblings);
	spin_unlock_irqrestore(&task_group_lock, flags);
}

/*
 * sched_change_group() 要求 task_rq_lock 已稳定 @tsk；读取新 css、应用 autogroup
 * 映射并更新 sched_task_group，再由 class 修复调度实体 parent/cfs_rq。
 */
static void sched_change_group(struct task_struct *tsk)
{
	struct task_group *tg;

	/*
	 * All callers are synchronized by task_rq_lock(); we do not use RCU
	 * which is pointless here. Thus, we pass "true" to task_css_check()
	 * to prevent lockdep warnings.
	 */
	tg = container_of(task_css_check(tsk, cpu_cgrp_id, true),
			  struct task_group, css);
	tg = autogroup_task_group(tsk, tg);
	tsk->sched_task_group = tg;

#ifdef CONFIG_FAIR_GROUP_SCHED
	if (tsk->sched_class->task_change_group)
		tsk->sched_class->task_change_group(tsk);
	else
#endif
		set_task_rq(tsk, task_cpu(tsk));
}

/*
 * sched_change_group() 要求 task_rq_lock 已稳定 @tsk；直接读取当前 css 并经过
 * autogroup 映射，更新 sched_task_group，再让 class 修正实体 parent/cfs_rq。
 * 不需要 RCU，因为锁协议阻止并发 group/CPU 变化；无返回值。
 */

/*
 * Change task's runqueue when it moves between groups.
 *
 * The caller of this function should have put the task in its new group by
 * now. This function just updates tsk->se.cfs_rq and tsk->se.parent to reflect
 * its new group.
 */
/*
 * 原文：调用者已把 @tsk 的 cgroup membership 改为新组，本函数只同步调度实体
 * 的 cfs_rq/parent 等派生链接。@tsk 为稳定借用指针；@for_autogroup 区分自动组
 * 与真实 cgroup 移动。task_rq_lock() 稳定 CPU/队列，sched_change scope 在 task
 * queued/running 时临时 dequeue/put_prev，修改 group 后再按原状态恢复；必要时
 * resched 或 wakeup_preempt。无失败返回、无 task 引用转移。
 */
void sched_move_task(struct task_struct *tsk, bool for_autogroup)
{
	unsigned int queue_flags = DEQUEUE_SAVE | DEQUEUE_MOVE;
	bool resched = false;
	bool queued = false;
	struct rq *rq;

	CLASS(task_rq_lock, rq_guard)(tsk);
	rq = rq_guard.rq;

	scoped_guard (sched_change, tsk, queue_flags) {
		sched_change_group(tsk);
		if (!for_autogroup)
			scx_cgroup_move_task(tsk);
		if (scope->running)
			resched = true;
		queued = scope->queued;
	}

	if (resched)
		resched_curr(rq);
	else if (queued)
		wakeup_preempt(rq, tsk, 0);

	__balance_callbacks(rq, &rq_guard.rf);
}

/*
 * cpu_cgroup_css_alloc() 为 root 早期初始化返回永久 root css；普通子组分配新的
 * task_group 并返回内嵌 css。成功 ownership 交给 cgroup core，失败为 -ENOMEM。
 */
static struct cgroup_subsys_state *
cpu_cgroup_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct task_group *parent = css_tg(parent_css);
	struct task_group *tg;

	if (!parent) {
		/* This is early initialization for the top cgroup */
		/* 中文：顶层 cgroup 的早期初始化复用永久 root_task_group。 */
		return &root_task_group.css;
	}

	tg = sched_create_group(parent);
	if (IS_ERR(tg))
		return ERR_PTR(-ENOMEM);

	return &tg->css;
}

/* Expose task group only after completing cgroup initialization */
/*
 * 原文：只有 cgroup 初始化完成后才发布 task group。先让 sched_ext online，
 * 失败原样返回且不进入全局链；再发布到 parent，最后在 uclamp_mutex+RCU 下传播
 * 层级 clamp。返回 0 或 SCX errno。
 */
static int cpu_cgroup_css_online(struct cgroup_subsys_state *css)
{
	struct task_group *tg = css_tg(css);
	struct task_group *parent = css_tg(css->parent);
	int ret;

	ret = scx_tg_online(tg);
	if (ret)
		return ret;

	if (parent)
		sched_online_group(tg, parent);

#ifdef CONFIG_UCLAMP_TASK_GROUP
	/* Propagate the effective uclamp value for the new group */
	/* 中文：为新组传播父层级的有效 uclamp 限制。 */
	guard(mutex)(&uclamp_mutex);
	guard(rcu)();
	cpu_util_update_eff(css);
#endif

	return 0;
}

/*
 * css_offline 先通知 sched_ext 停止接收新活动；css_released 从 RCU 索引摘除；
 * css_free 依赖两回调间已有宽限期，注销并进入最终释放链。三个参数都是内嵌 css
 * 借用指针，无直接返回值。
 */
static void cpu_cgroup_css_offline(struct cgroup_subsys_state *css)
{
	struct task_group *tg = css_tg(css);

	scx_tg_offline(tg);
}

static void cpu_cgroup_css_released(struct cgroup_subsys_state *css)
{
	struct task_group *tg = css_tg(css);

	sched_release_group(tg);
}

static void cpu_cgroup_css_free(struct cgroup_subsys_state *css)
{
	struct task_group *tg = css_tg(css);

	/*
	 * Relies on the RCU grace period between css_released() and this.
	 */
	sched_unregister_group(tg);
}

/*
 * cpu_cgroup_can_attach() 预检整个 @tset：RT group scheduling 开启时每个 task
 * 必须通过带宽/组约束，再交 sched_ext 检查；首个失败返回 -EINVAL/SCX errno，
 * 不改变 task membership。
 */
static int cpu_cgroup_can_attach(struct cgroup_taskset *tset)
{
#ifdef CONFIG_RT_GROUP_SCHED
	struct task_struct *task;
	struct cgroup_subsys_state *css;

	if (!rt_group_sched_enabled())
		goto scx_check;

	cgroup_taskset_for_each(task, css, tset) {
		if (!sched_rt_can_attach(css_tg(css), task))
			return -EINVAL;
	}
scx_check:
#endif /* CONFIG_RT_GROUP_SCHED */
	return scx_cgroup_can_attach(tset);
}

/*
 * cpu_cgroup_attach() 在 cgroup core 已切换 css 后逐 task 调 sched_move_task，
 * 同步调度实体派生链接；cancel_attach 只撤销 sched_ext 的预提交状态。
 */
static void cpu_cgroup_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *css;

	cgroup_taskset_for_each(task, css, tset)
		sched_move_task(task, false);
}

static void cpu_cgroup_cancel_attach(struct cgroup_taskset *tset)
{
	scx_cgroup_cancel_attach(tset);
}

#ifdef CONFIG_UCLAMP_TASK_GROUP
/*
 * cpu_util_update_eff() 要求持 uclamp_mutex 和 RCU，从 @css 先序遍历后代。每组
 * 请求先受 parent 有效值限制，再保证 MIN<=MAX；值变化则更新 bucket_id 并立即
 * 重新记账该 css runnable task。若节点未变化，整棵后代可跳过，因为父约束也未
 * 改。无返回值，是 cgroup uclamp 配置的提交阶段。
 */
static void cpu_util_update_eff(struct cgroup_subsys_state *css)
{
	struct cgroup_subsys_state *top_css = css;
	struct uclamp_se *uc_parent = NULL;
	struct uclamp_se *uc_se = NULL;
	unsigned int eff[UCLAMP_CNT];
	enum uclamp_id clamp_id;
	unsigned int clamps;

	lockdep_assert_held(&uclamp_mutex);
	WARN_ON_ONCE(!rcu_read_lock_held());

	css_for_each_descendant_pre(css, top_css) {
		uc_parent = css_tg(css)->parent
			? css_tg(css)->parent->uclamp : NULL;

		for_each_clamp_id(clamp_id) {
			/* Assume effective clamps matches requested clamps */
			/* 中文：先假定有效 clamp 等于当前组请求值。 */
			eff[clamp_id] = css_tg(css)->uclamp_req[clamp_id].value;
			/* Cap effective clamps with parent's effective clamps */
			/* 中文：再用父组已生效 clamp 对子组请求取更严格上限。 */
			if (uc_parent &&
			    eff[clamp_id] > uc_parent[clamp_id].value) {
				eff[clamp_id] = uc_parent[clamp_id].value;
			}
		}
		/* Ensure protection is always capped by limit */
	/* 中文：保证 UCLAMP_MIN 的性能保护绝不超过 UCLAMP_MAX 的容量限制。 */
		eff[UCLAMP_MIN] = min(eff[UCLAMP_MIN], eff[UCLAMP_MAX]);

		/* Propagate most restrictive effective clamps */
	/* 中文：把变化后的最严格有效值传播到本组，并记录需刷新 active task 的索引。 */
		clamps = 0x0;
		uc_se = css_tg(css)->uclamp;
		for_each_clamp_id(clamp_id) {
			if (eff[clamp_id] == uc_se[clamp_id].value)
				continue;
			uc_se[clamp_id].value = eff[clamp_id];
			uc_se[clamp_id].bucket_id = uclamp_bucket_id(eff[clamp_id]);
			clamps |= (0x1 << clamp_id);
		}
		if (!clamps) {
			css = css_rightmost_descendant(css);
			continue;
		}

		/* Immediately update descendants RUNNABLE tasks */
		uclamp_update_active_tasks(css);
	}
}

/*
 * Integer 10^N with a given N exponent by casting to integer the literal "1eN"
 * C expression. Since there is no way to convert a macro argument (N) into a
 * character constant, use two levels of macros.
 */
/*
 * 原文：用浮点字面量 1eN 在编译期得到整数 10^N；宏参数不能直接变成字符，
 * 因此两级展开。这里只为保留两位百分比精度，不执行运行期浮点运算。
 */
#define _POW10(exp) ((unsigned int)1e##exp)
#define POW10(exp) _POW10(exp)

struct uclamp_request {
#define UCLAMP_PERCENT_SHIFT	2
#define UCLAMP_PERCENT_SCALE	(100 * POW10(UCLAMP_PERCENT_SHIFT))
	s64 percent;
	u64 util;
	int ret;
};

/*
 * uclamp_request 是解析结果快照：percent 为放大 100 倍的百分比，util 为
 * 0..SCHED_CAPACITY_SCALE，ret 为 0/errno。对象按值返回，无外部资源。
 */

static inline struct uclamp_request
capacity_from_percent(char *buf)
{
	struct uclamp_request req = {
		.percent = UCLAMP_PERCENT_SCALE,
		.util = SCHED_CAPACITY_SCALE,
		.ret = 0,
	};

	buf = strim(buf);
	if (strcmp(buf, "max")) {
		req.ret = cgroup_parse_float(buf, UCLAMP_PERCENT_SHIFT,
					     &req.percent);
		if (req.ret)
			return req;
		if ((u64)req.percent > UCLAMP_PERCENT_SCALE) {
			req.ret = -ERANGE;
			return req;
		}

		req.util = req.percent << SCHED_CAPACITY_SHIFT;
		req.util = DIV_ROUND_CLOSEST_ULL(req.util, UCLAMP_PERCENT_SCALE);
	}

	return req;
}

/*
 * capacity_from_percent() 原地 strim 调用者可写 @buf；"max" 映射满容量，否则按
 * 两位小数解析 0..100%，越界 -ERANGE、格式错误透传 errno。返回按值结果。
 */

/*
 * cpu_uclamp_write() 解析 kernfs 写入，启用静态键，在 uclamp_mutex+RCU 下更新
 * tg 请求与不可逆舍入前的精确 percent，再传播层级有效值。成功返回 @nbytes，
 * 失败返回解析 errno；@of/@buf 由 kernfs 借用。
 */
static ssize_t cpu_uclamp_write(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off,
				enum uclamp_id clamp_id)
{
	struct uclamp_request req;
	struct task_group *tg;

	req = capacity_from_percent(buf);
	if (req.ret)
		return req.ret;

	sched_uclamp_enable();

	guard(mutex)(&uclamp_mutex);
	guard(rcu)();

	tg = css_tg(of_css(of));
	if (tg->uclamp_req[clamp_id].value != req.util)
		uclamp_se_set(&tg->uclamp_req[clamp_id], req.util, false);

	/*
	 * Because of not recoverable conversion rounding we keep track of the
	 * exact requested value
	 */
	/* 中文：util 转换舍入不可逆，因此另存用户请求的精确百分比用于回显。 */
	tg->uclamp_pct[clamp_id] = req.percent;

	/* Update effective clamps to track the most restrictive value */
	/* 中文：重算层级有效 clamp，使每个子组跟随最严格祖先限制。 */
	cpu_util_update_eff(of_css(of));

	return nbytes;
}

static ssize_t cpu_uclamp_min_write(struct kernfs_open_file *of,
				    char *buf, size_t nbytes,
				    loff_t off)
{
	return cpu_uclamp_write(of, buf, nbytes, off, UCLAMP_MIN);
}

static ssize_t cpu_uclamp_max_write(struct kernfs_open_file *of,
				    char *buf, size_t nbytes,
				    loff_t off)
{
	return cpu_uclamp_write(of, buf, nbytes, off, UCLAMP_MAX);
}

/*
 * cpu_uclamp_print() 在 RCU 下读取请求 util；满容量输出 max，否则使用保存的精确
 * percent 格式化两位小数，避免 util 反算造成舍入漂移。@sf 由 seq_file 管理。
 */
static inline void cpu_uclamp_print(struct seq_file *sf,
				    enum uclamp_id clamp_id)
{
	struct task_group *tg;
	u64 util_clamp;
	u64 percent;
	u32 rem;

	scoped_guard (rcu) {
		tg = css_tg(seq_css(sf));
		util_clamp = tg->uclamp_req[clamp_id].value;
	}

	if (util_clamp == SCHED_CAPACITY_SCALE) {
		seq_puts(sf, "max\n");
		return;
	}

	percent = tg->uclamp_pct[clamp_id];
	percent = div_u64_rem(percent, POW10(UCLAMP_PERCENT_SHIFT), &rem);
	seq_printf(sf, "%llu.%0*u\n", percent, UCLAMP_PERCENT_SHIFT, rem);
}

/* cpu_uclamp_min_show() 输出当前 cgroup 请求的 UCLAMP_MIN。 */
static int cpu_uclamp_min_show(struct seq_file *sf, void *v)
{
	cpu_uclamp_print(sf, UCLAMP_MIN);
	return 0;
}

/* cpu_uclamp_max_show() 输出当前 cgroup 请求的 UCLAMP_MAX。 */
static int cpu_uclamp_max_show(struct seq_file *sf, void *v)
{
	cpu_uclamp_print(sf, UCLAMP_MAX);
	return 0;
}
#endif /* CONFIG_UCLAMP_TASK_GROUP */

#ifdef CONFIG_GROUP_SCHED_WEIGHT
/*
 * tg_weight() 将 task_group 当前权重统一还原为 cgroup 接口使用的尺度。
 * CFS 组调度保存的是内部缩放 shares；若由 sched_ext 承担组调度，则从其状态取值。
 */
static unsigned long tg_weight(struct task_group *tg)
{
#ifdef CONFIG_FAIR_GROUP_SCHED
	return scale_load_down(tg->shares);
#else
	return sched_weight_from_cgroup(tg->scx.weight);
#endif
}

/*
 * cpu_shares_write_u64() 实现 cgroup v1 cpu.shares 写入：先限制可缩放上界，
 * 再更新传统调度组；只有成功后才同步 sched_ext，避免两个后端观察到半更新状态。
 */
static int cpu_shares_write_u64(struct cgroup_subsys_state *css,
				struct cftype *cftype, u64 shareval)
{
	int ret;

	if (shareval > scale_load_down(ULONG_MAX))
		shareval = MAX_SHARES;
	ret = sched_group_set_shares(css_tg(css), scale_load(shareval));
	if (!ret)
		scx_group_set_weight(css_tg(css),
				     sched_weight_to_cgroup(shareval));
	return ret;
}

/* cpu_shares_read_u64() 返回已转换到用户可见尺度的当前组权重。 */
static u64 cpu_shares_read_u64(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	return tg_weight(css_tg(css));
}
#endif /* CONFIG_GROUP_SCHED_WEIGHT */

#ifdef CONFIG_CFS_BANDWIDTH
/*
 * cfs_constraints_mutex 串行化整个层级的 CFS 配额可行性检查与提交。
 * 单个 cfs_bandwidth.lock 只能保护一个组，不能保证父子约束在遍历期间稳定。
 */
static DEFINE_MUTEX(cfs_constraints_mutex);

static int __cfs_schedulable(struct task_group *tg, u64 period, u64 runtime);

/*
 * tg_set_cfs_bandwidth() 是 CFS 带宽参数的核心提交事务：
 *  1. 将用户态微秒值转换成纳秒，并在 cpus_read_lock 与层级约束互斥锁下验证；
 *  2. 若从无限配额切到有限配额，先开启全局静态分支，保证后续状态已可被消费；
 *  3. 在 cfs_b->lock 下原子替换 period/quota/burst、补充运行时并重启周期定时器；
 *  4. 逐 CPU 持 rq 锁更新该组 cfs_rq，解除旧配置留下的 throttled 状态；
 *  5. 从有限切回无限时最后关闭全局使用计数。
 *
 * 这种启用“先开后发布”、禁用“先撤状态后关”的顺序避免快速路径在中途错过带宽
 * 状态。任一可行性检查失败均在修改前返回，调用者不会看到部分提交。
 */
static int tg_set_cfs_bandwidth(struct task_group *tg,
				u64 period_us, u64 quota_us, u64 burst_us)
{
	int i, ret = 0, runtime_enabled, runtime_was_enabled;
	struct cfs_bandwidth *cfs_b = &tg->cfs_bandwidth;
	u64 period, quota, burst;

	period = (u64)period_us * NSEC_PER_USEC;

	if (quota_us == RUNTIME_INF)
		quota = RUNTIME_INF;
	else
		quota = (u64)quota_us * NSEC_PER_USEC;

	burst = (u64)burst_us * NSEC_PER_USEC;

	/*
	 * Prevent race between setting of cfs_rq->runtime_enabled and
	 * unthrottle_offline_cfs_rqs().
	 */
	/*
	 * 中文：防止设置 cfs_rq->runtime_enabled 与
	 * unthrottle_offline_cfs_rqs() 并发。CPU 热插拔读锁使在线 CPU 集合及离线
	 * 清理阶段在整个传播过程中保持稳定。
	 */
	guard(cpus_read_lock)();
	guard(mutex)(&cfs_constraints_mutex);

	ret = __cfs_schedulable(tg, period, quota);
	if (ret)
		return ret;

	runtime_enabled = quota != RUNTIME_INF;
	runtime_was_enabled = cfs_b->quota != RUNTIME_INF;
	/*
	 * If we need to toggle cfs_bandwidth_used, off->on must occur
	 * before making related changes, and on->off must occur afterwards
	 */
	/*
	 * 中文：若需切换 cfs_bandwidth_used，关闭到开启必须发生在相关修改之前，
	 * 开启到关闭必须发生在相关修改之后；这是一种发布/撤销顺序约束。
	 */
	if (runtime_enabled && !runtime_was_enabled)
		cfs_bandwidth_usage_inc();

	scoped_guard (raw_spinlock_irq, &cfs_b->lock) {
		cfs_b->period = ns_to_ktime(period);
		cfs_b->quota = quota;
		cfs_b->burst = burst;

		__refill_cfs_bandwidth_runtime(cfs_b);

		/*
		 * Restart the period timer (if active) to handle new
		 * period expiry:
		 */
		/* 中文：若带宽启用，重启周期定时器，使新的周期长度立即决定下一次到期。 */
		if (runtime_enabled)
			start_cfs_bandwidth(cfs_b);
	}

	for_each_online_cpu(i) {
		struct cfs_rq *cfs_rq = tg_cfs_rq(tg, i);
		struct rq *rq = cfs_rq->rq;

		guard(rq_lock_irq)(rq);

		cfs_rq->runtime_enabled = runtime_enabled;
		cfs_rq->runtime_remaining = 1;

		if (cfs_rq->throttled) {
			update_rq_clock(rq);
			unthrottle_cfs_rq(cfs_rq);
		}
	}

	if (runtime_was_enabled && !runtime_enabled)
		cfs_bandwidth_usage_dec();

	return 0;
}

/* tg_get_cfs_period() 将内部 ktime 周期转换为 cgroup ABI 使用的微秒。 */
static u64 tg_get_cfs_period(struct task_group *tg)
{
	u64 cfs_period_us;

	cfs_period_us = ktime_to_ns(tg->cfs_bandwidth.period);
	do_div(cfs_period_us, NSEC_PER_USEC);

	return cfs_period_us;
}

/* tg_get_cfs_quota() 保留 RUNTIME_INF 哨兵，否则把纳秒配额转换为微秒。 */
static u64 tg_get_cfs_quota(struct task_group *tg)
{
	u64 quota_us;

	if (tg->cfs_bandwidth.quota == RUNTIME_INF)
		return RUNTIME_INF;

	quota_us = tg->cfs_bandwidth.quota;
	do_div(quota_us, NSEC_PER_USEC);

	return quota_us;
}

/* tg_get_cfs_burst() 将允许跨周期借用的突发额度从纳秒转换为微秒。 */
static u64 tg_get_cfs_burst(struct task_group *tg)
{
	u64 burst_us;

	burst_us = tg->cfs_bandwidth.burst;
	do_div(burst_us, NSEC_PER_USEC);

	return burst_us;
}

/*
 * cfs_schedulable_data 描述一次“假设写入”：目标组使用待提交的 period/quota，
 * 其他组仍读取现值，供树遍历在真正修改前验证整个层级。
 */
struct cfs_schedulable_data {
	struct task_group *tg;
	u64 period, quota;
};

/*
 * normalize group quota/period to be quota/max_period
 * note: units are usecs
 */
/*
 * 中文：把组的 quota/period 归一化为统一比例（注：输入单位为微秒）。
 * 对目标组使用候选值，对其余组使用已提交值；无限配额保持哨兵，不能参与普通除法。
 */
static u64 normalize_cfs_quota(struct task_group *tg,
			       struct cfs_schedulable_data *d)
{
	u64 quota, period;

	if (tg == d->tg) {
		period = d->period;
		quota = d->quota;
	} else {
		period = tg_get_cfs_period(tg);
		quota = tg_get_cfs_quota(tg);
	}

	/* note: these should typically be equivalent */
	/* 中文：这两个无限配额表示通常等价，同时兼容无符号哨兵与历史 -1 表达。 */
	if (quota == RUNTIME_INF || quota == -1)
		return RUNTIME_INF;

	return to_ratio(period, quota);
}

/*
 * tg_cfs_schedulable_down() 自根向叶计算 hierarchical_quota。
 * v2 允许子组声明高于父组的 max，运行时取父子有限值中更严格者；v1 则把这种显式
 * 超额视为无效配置，仅在子组无限制时继承父限制。返回 -EINVAL 会中止整次写入。
 */
static int tg_cfs_schedulable_down(struct task_group *tg, void *data)
{
	struct cfs_schedulable_data *d = data;
	struct cfs_bandwidth *cfs_b = &tg->cfs_bandwidth;
	s64 quota = 0, parent_quota = -1;

	if (!tg->parent) {
		quota = RUNTIME_INF;
	} else {
		struct cfs_bandwidth *parent_b = &tg->parent->cfs_bandwidth;

		quota = normalize_cfs_quota(tg, d);
		parent_quota = parent_b->hierarchical_quota;

		/*
		 * Ensure max(child_quota) <= parent_quota.  On cgroup2,
		 * always take the non-RUNTIME_INF min.  On cgroup1, only
		 * inherit when no limit is set. In both cases this is used
		 * by the scheduler to determine if a given CFS task has a
		 * bandwidth constraint at some higher level.
		 */
		/*
		 * 中文：保证有效 child_quota 不超过 parent_quota。cgroup v2 总是选取
		 * 非无限值中的较小者；cgroup v1 仅在子组未设限制时继承，否则拒绝超过
		 * 父组的配置。最终值也用于判断任务是否受任一祖先的带宽限制。
		 */
		if (cgroup_subsys_on_dfl(cpu_cgrp_subsys)) {
			if (quota == RUNTIME_INF)
				quota = parent_quota;
			else if (parent_quota != RUNTIME_INF)
				quota = min(quota, parent_quota);
		} else {
			if (quota == RUNTIME_INF)
				quota = parent_quota;
			else if (parent_quota != RUNTIME_INF && quota > parent_quota)
				return -EINVAL;
		}
	}
	cfs_b->hierarchical_quota = quota;

	return 0;
}

/*
 * __cfs_schedulable() 构造候选配置并在 RCU 读侧遍历 task_group 树。
 * RCU 保证父子节点生命周期稳定；遍历回调写 hierarchical_quota，而外层
 * cfs_constraints_mutex 保证没有另一个配置者同时重算。
 */
static int __cfs_schedulable(struct task_group *tg, u64 period, u64 quota)
{
	struct cfs_schedulable_data data = {
		.tg = tg,
		.period = period,
		.quota = quota,
	};

	if (quota != RUNTIME_INF) {
		do_div(data.period, NSEC_PER_USEC);
		do_div(data.quota, NSEC_PER_USEC);
	}

	guard(rcu)();
	return walk_tg_tree(tg_cfs_schedulable_down, tg_nop, &data);
}

/*
 * cpu_cfs_stat_show() 输出组级带宽周期、节流与突发统计；启用 schedstat 时，
 * 非根组还汇总所有可能 CPU 上调度实体的 wait_sum。读取是统计快照，不承诺跨字段
 * 原子一致性，适合观测而非同步。
 */
static int cpu_cfs_stat_show(struct seq_file *sf, void *v)
{
	struct task_group *tg = css_tg(seq_css(sf));
	struct cfs_bandwidth *cfs_b = &tg->cfs_bandwidth;

	seq_printf(sf, "nr_periods %d\n", cfs_b->nr_periods);
	seq_printf(sf, "nr_throttled %d\n", cfs_b->nr_throttled);
	seq_printf(sf, "throttled_time %llu\n", cfs_b->throttled_time);

	if (schedstat_enabled() && tg != &root_task_group) {
		struct sched_statistics *stats;
		u64 ws = 0;
		int i;

		for_each_possible_cpu(i) {
			stats = __schedstats_from_se(tg_se(tg, i));
			ws += schedstat_val(stats->wait_sum);
		}

		seq_printf(sf, "wait_sum %llu\n", ws);
	}

	seq_printf(sf, "nr_bursts %d\n", cfs_b->nr_burst);
	seq_printf(sf, "burst_time %llu\n", cfs_b->burst_time);

	return 0;
}

/* throttled_time_self() 汇总本组自身各 CPU cfs_rq 的节流时间，不含子组。 */
static u64 throttled_time_self(struct task_group *tg)
{
	int i;
	u64 total = 0;

	for_each_possible_cpu(i) {
		total += READ_ONCE(tg_cfs_rq(tg, i)->throttled_clock_self_time);
	}

	return total;
}

/* cpu_cfs_local_stat_show() 暴露仅属于当前组的本地节流时间。 */
static int cpu_cfs_local_stat_show(struct seq_file *sf, void *v)
{
	struct task_group *tg = css_tg(seq_css(sf));

	seq_printf(sf, "throttled_time %llu\n", throttled_time_self(tg));

	return 0;
}
#endif /* CONFIG_CFS_BANDWIDTH */

#ifdef CONFIG_GROUP_SCHED_BANDWIDTH
/*
 * 带宽 ABI 边界：周期限制在 1ms..1s；运行时还需受 MAX_BW 限制，确保从微秒
 * 转成纳秒及内部带宽移位时不溢出。
 */
const u64 max_bw_quota_period_us = 1 * USEC_PER_SEC; /* 1s */
static const u64 min_bw_quota_period_us = 1 * USEC_PER_MSEC; /* 1ms */
/* More than 203 days if BW_SHIFT equals 20. */
/* 中文：当 BW_SHIFT 为 20 时，该上限对应超过 203 天的运行时间。 */
static const u64 max_bw_runtime_us = MAX_BW;

/*
 * tg_bandwidth() 统一读取 task_group 的 period/quota/burst。启用 CFS 带宽时读取
 * cfs_bandwidth；否则仍为 sched_ext 保留同一 cgroup ABI，并从 scx 状态读取。
 */
static void tg_bandwidth(struct task_group *tg,
			 u64 *period_us_p, u64 *quota_us_p, u64 *burst_us_p)
{
#ifdef CONFIG_CFS_BANDWIDTH
	if (period_us_p)
		*period_us_p = tg_get_cfs_period(tg);
	if (quota_us_p)
		*quota_us_p = tg_get_cfs_quota(tg);
	if (burst_us_p)
		*burst_us_p = tg_get_cfs_burst(tg);
#else /* !CONFIG_CFS_BANDWIDTH */
	if (period_us_p)
		*period_us_p = tg->scx.bw_period_us;
	if (quota_us_p)
		*quota_us_p = tg->scx.bw_quota_us;
	if (burst_us_p)
		*burst_us_p = tg->scx.bw_burst_us;
#endif /* CONFIG_CFS_BANDWIDTH */
}

/* cpu_period_read_u64() 是 cgroup v1 cfs_period_us 的读取适配器。 */
static u64 cpu_period_read_u64(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	u64 period_us;

	tg_bandwidth(css_tg(css), &period_us, NULL, NULL);
	return period_us;
}

/*
 * tg_set_bandwidth() 是 CFS 与 sched_ext 共用的参数校验入口。
 * 根组不可限流；所有值必须可安全转换为纳秒，周期和有限配额不得过小/过大，
 * burst 不得超过 quota，且两者之和不得使内部带宽运算溢出。
 * CFS 提交失败时不会同步 sched_ext；成功后两个后端共享相同配置。
 */
static int tg_set_bandwidth(struct task_group *tg,
			    u64 period_us, u64 quota_us, u64 burst_us)
{
	const u64 max_usec = U64_MAX / NSEC_PER_USEC;
	int ret = 0;

	if (tg == &root_task_group)
		return -EINVAL;

	/* Values should survive translation to nsec */
	/* 中文：所有输入都必须能无损地转换为纳秒。 */
	if (period_us > max_usec ||
	    (quota_us != RUNTIME_INF && quota_us > max_usec) ||
	    burst_us > max_usec)
		return -EINVAL;

	/*
	 * Ensure we have some amount of bandwidth every period. This is to
	 * prevent reaching a state of large arrears when throttled via
	 * entity_tick() resulting in prolonged exit starvation.
	 */
	/*
	 * 中文：保证每周期至少有可实际执行的带宽，避免 entity_tick() 节流后形成
	 * 巨额欠账，使任务长时间无法退出节流状态。
	 */
	if (quota_us < min_bw_quota_period_us ||
	    period_us < min_bw_quota_period_us)
		return -EINVAL;

	/*
	 * Likewise, bound things on the other side by preventing insane quota
	 * periods.  This also allows us to normalize in computing quota
	 * feasibility.
	 */
	/* 中文：同时限制过大的周期，并使层级配额可行性计算能够使用统一归一化范围。 */
	if (period_us > max_bw_quota_period_us)
		return -EINVAL;

	/*
	 * Bound quota to defend quota against overflow during bandwidth shift.
	 */
	/* 中文：限制配额，防止内部按 BW_SHIFT 缩放时溢出。 */
	if (quota_us != RUNTIME_INF && quota_us > max_bw_runtime_us)
		return -EINVAL;

	if (quota_us != RUNTIME_INF && (burst_us > quota_us ||
					burst_us + quota_us > max_bw_runtime_us))
		return -EINVAL;

#ifdef CONFIG_CFS_BANDWIDTH
	ret = tg_set_cfs_bandwidth(tg, period_us, quota_us, burst_us);
#endif /* CONFIG_CFS_BANDWIDTH */
	if (!ret)
		scx_group_set_bandwidth(tg, period_us, quota_us, burst_us);
	return ret;
}

/* 以下 read/write 适配器保留未修改的两个参数，只替换用户写入的那一项。 */
static s64 cpu_quota_read_s64(struct cgroup_subsys_state *css,
			      struct cftype *cft)
{
	u64 quota_us;

	tg_bandwidth(css_tg(css), NULL, &quota_us, NULL);
	/* 中文：无符号 RUNTIME_INF 转为 s64 后按 ABI 表现为 -1。 */
	return quota_us;	/* (s64)RUNTIME_INF becomes -1 */
}

/* cpu_burst_read_u64() 读取当前突发额度（微秒）。 */
static u64 cpu_burst_read_u64(struct cgroup_subsys_state *css,
			      struct cftype *cft)
{
	u64 burst_us;

	tg_bandwidth(css_tg(css), NULL, NULL, &burst_us);
	return burst_us;
}

/* cpu_period_write_u64() 仅替换周期，并复用当前 quota/burst 完整校验后提交。 */
static int cpu_period_write_u64(struct cgroup_subsys_state *css,
				struct cftype *cftype, u64 period_us)
{
	struct task_group *tg = css_tg(css);
	u64 quota_us, burst_us;

	tg_bandwidth(tg, NULL, &quota_us, &burst_us);
	return tg_set_bandwidth(tg, period_us, quota_us, burst_us);
}

/* cpu_quota_write_s64() 将任意负值规范化为无限配额，再保留 period/burst 提交。 */
static int cpu_quota_write_s64(struct cgroup_subsys_state *css,
			       struct cftype *cftype, s64 quota_us)
{
	struct task_group *tg = css_tg(css);
	u64 period_us, burst_us;

	if (quota_us < 0)
		quota_us = RUNTIME_INF;

	tg_bandwidth(tg, &period_us, NULL, &burst_us);
	return tg_set_bandwidth(tg, period_us, quota_us, burst_us);
}

/* cpu_burst_write_u64() 仅替换突发额度，并与现有 period/quota 一起校验提交。 */
static int cpu_burst_write_u64(struct cgroup_subsys_state *css,
			       struct cftype *cftype, u64 burst_us)
{
	struct task_group *tg = css_tg(css);
	u64 period_us, quota_us;

	tg_bandwidth(tg, &period_us, &quota_us, NULL);
	return tg_set_bandwidth(tg, period_us, quota_us, burst_us);
}
#endif /* CONFIG_GROUP_SCHED_BANDWIDTH */

#ifdef CONFIG_RT_GROUP_SCHED
/* RT cgroup 文件处理器直接委托 RT 调度类完成范围校验和层级状态更新。 */
static int cpu_rt_runtime_write(struct cgroup_subsys_state *css,
				struct cftype *cft, s64 val)
{
	return sched_group_set_rt_runtime(css_tg(css), val);
}

static s64 cpu_rt_runtime_read(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	return sched_group_rt_runtime(css_tg(css));
}

static int cpu_rt_period_write_uint(struct cgroup_subsys_state *css,
				    struct cftype *cftype, u64 rt_period_us)
{
	return sched_group_set_rt_period(css_tg(css), rt_period_us);
}

/* cpu_rt_period_read_uint() 读取 RT task_group 的周期微秒值。 */
static u64 cpu_rt_period_read_uint(struct cgroup_subsys_state *css,
				   struct cftype *cft)
{
	return sched_group_rt_period(css_tg(css));
}
#endif /* CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_GROUP_SCHED_WEIGHT
/* cpu_idle_read_s64() 读取组的 idle 属性：该组是否只消耗空闲 CPU 时间。 */
static s64 cpu_idle_read_s64(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	return css_tg(css)->idle;
}

/* cpu_idle_write_s64() 先提交传统调度组，成功后再镜像到 sched_ext。 */
static int cpu_idle_write_s64(struct cgroup_subsys_state *css,
				struct cftype *cft, s64 idle)
{
	int ret;

	ret = sched_group_set_idle(css_tg(css), idle);
	if (!ret)
		scx_group_set_idle(css_tg(css), idle);
	return ret;
}
#endif /* CONFIG_GROUP_SCHED_WEIGHT */

/*
 * cpu_legacy_files 是 cgroup v1 cpu 控制器 ABI 映射表。条件编译决定暴露 shares、
 * CFS/RT 带宽、idle 与 uclamp 文件；空项是 cgroup 核心要求的终止哨兵。
 */
static struct cftype cpu_legacy_files[] = {
#ifdef CONFIG_GROUP_SCHED_WEIGHT
	{
		.name = "shares",
		.read_u64 = cpu_shares_read_u64,
		.write_u64 = cpu_shares_write_u64,
	},
	{
		.name = "idle",
		.read_s64 = cpu_idle_read_s64,
		.write_s64 = cpu_idle_write_s64,
	},
#endif
#ifdef CONFIG_GROUP_SCHED_BANDWIDTH
	{
		.name = "cfs_period_us",
		.read_u64 = cpu_period_read_u64,
		.write_u64 = cpu_period_write_u64,
	},
	{
		.name = "cfs_quota_us",
		.read_s64 = cpu_quota_read_s64,
		.write_s64 = cpu_quota_write_s64,
	},
	{
		.name = "cfs_burst_us",
		.read_u64 = cpu_burst_read_u64,
		.write_u64 = cpu_burst_write_u64,
	},
#endif
#ifdef CONFIG_CFS_BANDWIDTH
	{
		.name = "stat",
		.seq_show = cpu_cfs_stat_show,
	},
	{
		.name = "stat.local",
		.seq_show = cpu_cfs_local_stat_show,
	},
#endif
#ifdef CONFIG_UCLAMP_TASK_GROUP
	{
		.name = "uclamp.min",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cpu_uclamp_min_show,
		.write = cpu_uclamp_min_write,
	},
	{
		.name = "uclamp.max",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cpu_uclamp_max_show,
		.write = cpu_uclamp_max_write,
	},
#endif
	/* 中文：下一个空项是 cftype 表终止哨兵。 */
	{ }	/* Terminate */
};

#ifdef CONFIG_RT_GROUP_SCHED
/* rt_group_files 单独注册 v1 RT runtime/period 文件，便于启动参数整体关闭。 */
static struct cftype rt_group_files[] = {
	{
		.name = "rt_runtime_us",
		.read_s64 = cpu_rt_runtime_read,
		.write_s64 = cpu_rt_runtime_write,
	},
	{
		.name = "rt_period_us",
		.read_u64 = cpu_rt_period_read_uint,
		.write_u64 = cpu_rt_period_write_uint,
	},
	/* 中文：下一个空项是 cftype 表终止哨兵。 */
	{ }	/* Terminate */
};

# ifdef CONFIG_RT_GROUP_SCHED_DEFAULT_DISABLED
DEFINE_STATIC_KEY_FALSE(rt_group_sched);
# else
DEFINE_STATIC_KEY_TRUE(rt_group_sched);
# endif

/*
 * setup_rt_group_sched() 解析早期启动参数 rt_group_sched=0/1，通过静态键决定
 * 是否启用 RT 组调度接口。无效值打印告警但仍标记参数已消费。
 */
static int __init setup_rt_group_sched(char *str)
{
	long val;

	if (kstrtol(str, 0, &val) || val < 0 || val > 1) {
		pr_warn("Unable to set rt_group_sched\n");
		return 1;
	}
	if (val)
		static_branch_enable(&rt_group_sched);
	else
		static_branch_disable(&rt_group_sched);

	return 1;
}
__setup("rt_group_sched=", setup_rt_group_sched);

/* cpu_rt_group_init() 仅在静态键启用时把 RT 文件追加到 cpu v1 控制器。 */
static int __init cpu_rt_group_init(void)
{
	if (!rt_group_sched_enabled())
		return 0;

	WARN_ON(cgroup_add_legacy_cftypes(&cpu_cgrp_subsys, rt_group_files));
	return 0;
}
subsys_initcall(cpu_rt_group_init);
#endif /* CONFIG_RT_GROUP_SCHED */

/*
 * cpu_extra_stat_show() 生成 cgroup v2 cpu.stat 的带宽扩展字段，并把内部纳秒
 * 转为 ABI 约定的微秒。未启用 CFS 带宽时保持空扩展且成功返回。
 */
static int cpu_extra_stat_show(struct seq_file *sf,
			       struct cgroup_subsys_state *css)
{
#ifdef CONFIG_CFS_BANDWIDTH
	{
		struct task_group *tg = css_tg(css);
		struct cfs_bandwidth *cfs_b = &tg->cfs_bandwidth;
		u64 throttled_usec, burst_usec;

		throttled_usec = cfs_b->throttled_time;
		do_div(throttled_usec, NSEC_PER_USEC);
		burst_usec = cfs_b->burst_time;
		do_div(burst_usec, NSEC_PER_USEC);

		seq_printf(sf, "nr_periods %d\n"
			   "nr_throttled %d\n"
			   "throttled_usec %llu\n"
			   "nr_bursts %d\n"
			   "burst_usec %llu\n",
			   cfs_b->nr_periods, cfs_b->nr_throttled,
			   throttled_usec, cfs_b->nr_burst, burst_usec);
	}
#endif /* CONFIG_CFS_BANDWIDTH */
	return 0;
}

/* cpu_local_stat_show() 生成 cpu.stat.local，仅报告当前组自身的节流时间。 */
static int cpu_local_stat_show(struct seq_file *sf,
			       struct cgroup_subsys_state *css)
{
#ifdef CONFIG_CFS_BANDWIDTH
	{
		struct task_group *tg = css_tg(css);
		u64 throttled_self_usec;

		throttled_self_usec = throttled_time_self(tg);
		do_div(throttled_self_usec, NSEC_PER_USEC);

		seq_printf(sf, "throttled_usec %llu\n",
			   throttled_self_usec);
	}
#endif
	return 0;
}

#ifdef CONFIG_GROUP_SCHED_WEIGHT

/* cpu_weight_read_u64() 把内部调度权重映射到 cgroup v2 的 1..10000 范围。 */
static u64 cpu_weight_read_u64(struct cgroup_subsys_state *css,
			       struct cftype *cft)
{
	return sched_weight_to_cgroup(tg_weight(css_tg(css)));
}

/*
 * cpu_weight_write_u64() 校验 v2 权重范围，转换并更新传统调度组；成功后同步
 * sched_ext。失败不会让两个调度后端出现不同权重。
 */
static int cpu_weight_write_u64(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 cgrp_weight)
{
	unsigned long weight;
	int ret;

	if (cgrp_weight < CGROUP_WEIGHT_MIN || cgrp_weight > CGROUP_WEIGHT_MAX)
		return -ERANGE;

	weight = sched_weight_from_cgroup(cgrp_weight);

	ret = sched_group_set_shares(css_tg(css), scale_load(weight));
	if (!ret)
		scx_group_set_weight(css_tg(css), cgrp_weight);
	return ret;
}

/*
 * cpu_weight_nice_read_s64() 在离散 nice 权重表中寻找距当前组权重最近的项。
 * 当误差开始不再减小时，前一项即最近值；该接口是近似反映，转换并非双射。
 */
static s64 cpu_weight_nice_read_s64(struct cgroup_subsys_state *css,
				    struct cftype *cft)
{
	unsigned long weight = tg_weight(css_tg(css));
	int last_delta = INT_MAX;
	int prio, delta;

	/* find the closest nice value to the current weight */
	/* 中文：寻找与当前权重最接近的 nice 值。 */
	for (prio = 0; prio < ARRAY_SIZE(sched_prio_to_weight); prio++) {
		delta = abs(sched_prio_to_weight[prio] - weight);
		if (delta >= last_delta)
			break;
		last_delta = delta;
	}

	return PRIO_TO_NICE(prio - 1 + MAX_RT_PRIO);
}

/* cpu_weight_nice_write_s64() 将合法 nice 精确映射到权重并同步两个调度后端。 */
static int cpu_weight_nice_write_s64(struct cgroup_subsys_state *css,
				     struct cftype *cft, s64 nice)
{
	unsigned long weight;
	int idx, ret;

	if (nice < MIN_NICE || nice > MAX_NICE)
		return -ERANGE;

	idx = NICE_TO_PRIO(nice) - MAX_RT_PRIO;
	idx = array_index_nospec(idx, 40);
	weight = sched_prio_to_weight[idx];

	ret = sched_group_set_shares(css_tg(css), scale_load(weight));
	if (!ret)
		scx_group_set_weight(css_tg(css),
				     sched_weight_to_cgroup(weight));
	return ret;
}
#endif /* CONFIG_GROUP_SCHED_WEIGHT */

/* cpu_period_quota_print() 按 cgroup v2 “max|quota period” 格式输出带宽上限。 */
static void __maybe_unused cpu_period_quota_print(struct seq_file *sf,
						  long period, long quota)
{
	if (quota < 0)
		seq_puts(sf, "max");
	else
		seq_printf(sf, "%ld", quota);

	seq_printf(sf, " %ld\n", period);
}

/* caller should put the current value in *@periodp before calling */
/*
 * 中文：调用前应把当前周期放入 *@period_us_p；若输入只给 quota，就保留该周期。
 * cpu_period_quota_parse() 接受 “max [period]” 或 “quota [period]”，解析失败返回
 * -EINVAL，且不直接提交任何调度状态。
 */
static int __maybe_unused cpu_period_quota_parse(char *buf, u64 *period_us_p,
						 u64 *quota_us_p)
{
	char tok[21];	/* U64_MAX */

	if (sscanf(buf, "%20s %llu", tok, period_us_p) < 1)
		return -EINVAL;

	if (sscanf(tok, "%llu", quota_us_p) < 1) {
		if (!strcmp(tok, "max"))
			*quota_us_p = RUNTIME_INF;
		else
			return -EINVAL;
	}

	return 0;
}

#ifdef CONFIG_GROUP_SCHED_BANDWIDTH
/* cpu_max_show() 输出 v2 cpu.max 的当前 quota 与 period。 */
static int cpu_max_show(struct seq_file *sf, void *v)
{
	struct task_group *tg = css_tg(seq_css(sf));
	u64 period_us, quota_us;

	tg_bandwidth(tg, &period_us, &quota_us, NULL);
	cpu_period_quota_print(sf, period_us, quota_us);
	return 0;
}

/*
 * cpu_max_write() 先读取当前 period/burst，让省略 period 的写入保持旧值；
 * 解析成功后走统一验证与提交路径，成功按 kernfs 约定返回消费的字节数。
 */
static ssize_t cpu_max_write(struct kernfs_open_file *of,
			     char *buf, size_t nbytes, loff_t off)
{
	struct task_group *tg = css_tg(of_css(of));
	u64 period_us, quota_us, burst_us;
	int ret;

	tg_bandwidth(tg, &period_us, NULL, &burst_us);
	ret = cpu_period_quota_parse(buf, &period_us, &quota_us);
	if (!ret)
		ret = tg_set_bandwidth(tg, period_us, quota_us, burst_us);
	return ret ?: nbytes;
}
#endif /* CONFIG_CFS_BANDWIDTH */

/*
 * cpu_files 是 cgroup v2 CPU 控制器 ABI：weight/weight.nice/idle、max/max.burst
 * 与 uclamp。CFTYPE_NOT_ON_ROOT 表示根组由系统整体策略定义，不能通过这些文件改写。
 */
static struct cftype cpu_files[] = {
#ifdef CONFIG_GROUP_SCHED_WEIGHT
	{
		.name = "weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_u64 = cpu_weight_read_u64,
		.write_u64 = cpu_weight_write_u64,
	},
	{
		.name = "weight.nice",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_s64 = cpu_weight_nice_read_s64,
		.write_s64 = cpu_weight_nice_write_s64,
	},
	{
		.name = "idle",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_s64 = cpu_idle_read_s64,
		.write_s64 = cpu_idle_write_s64,
	},
#endif
#ifdef CONFIG_GROUP_SCHED_BANDWIDTH
	{
		.name = "max",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cpu_max_show,
		.write = cpu_max_write,
	},
	{
		.name = "max.burst",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_u64 = cpu_burst_read_u64,
		.write_u64 = cpu_burst_write_u64,
	},
#endif /* CONFIG_CFS_BANDWIDTH */
#ifdef CONFIG_UCLAMP_TASK_GROUP
	{
		.name = "uclamp.min",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cpu_uclamp_min_show,
		.write = cpu_uclamp_min_write,
	},
	{
		.name = "uclamp.max",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cpu_uclamp_max_show,
		.write = cpu_uclamp_max_write,
	},
#endif /* CONFIG_UCLAMP_TASK_GROUP */
	/* 中文：下一个空项是 cftype 表终止哨兵。 */
	{ }	/* terminate */
};

/*
 * cpu_cgrp_subsys 把 task_group 生命周期、任务迁移验证/提交、统计输出及 v1/v2
 * 文件表注册给 cgroup 核心。early_init 保证调度器早期可用，threaded 允许线程化
 * cgroup；各回调中的 css 引用与序列化由 cgroup 核心负责。
 */
struct cgroup_subsys cpu_cgrp_subsys = {
	.css_alloc	= cpu_cgroup_css_alloc,
	.css_online	= cpu_cgroup_css_online,
	.css_offline	= cpu_cgroup_css_offline,
	.css_released	= cpu_cgroup_css_released,
	.css_free	= cpu_cgroup_css_free,
	.css_extra_stat_show = cpu_extra_stat_show,
	.css_local_stat_show = cpu_local_stat_show,
	.can_attach	= cpu_cgroup_can_attach,
	.attach		= cpu_cgroup_attach,
	.cancel_attach	= cpu_cgroup_cancel_attach,
	.legacy_cftypes	= cpu_legacy_files,
	.dfl_cftypes	= cpu_files,
	.early_init	= true,
	.threaded	= true,
};

#endif /* CONFIG_CGROUP_SCHED */

/*
 * dump_cpu_task() 尽力输出指定 CPU 的当前执行上下文：若正在本 CPU 硬中断且有
 * pt_regs，直接打印寄存器；否则先尝试触发单 CPU 回溯，平台不支持或触发失败时
 * 退化为打印 cpu_curr() 的 task 信息。它用于诊断，读取结果允许瞬时变化。
 */
void dump_cpu_task(int cpu)
{
	if (in_hardirq() && cpu == smp_processor_id()) {
		struct pt_regs *regs;

		regs = get_irq_regs();
		if (regs) {
			show_regs(regs);
			return;
		}
	}

	if (trigger_single_cpu_backtrace(cpu))
		return;

	pr_info("Task dump for CPU %d:\n", cpu);
	sched_show_task(cpu_curr(cpu));
}

/*
 * Nice levels are multiplicative, with a gentle 10% change for every
 * nice level changed. I.e. when a CPU-bound task goes from nice 0 to
 * nice 1, it will get ~10% less CPU time than another CPU-bound task
 * that remained on nice 0.
 *
 * The "10% effect" is relative and cumulative: from _any_ nice level,
 * if you go up 1 level, it's -10% CPU usage, if you go down 1 level
 * it's +10% CPU usage. (to achieve that we use a multiplier of 1.25.
 * If a task goes up by ~10% and another task goes down by ~10% then
 * the relative distance between them is ~25%.)
 */
/*
 * 中文：nice 等级按乘法改变权重，相邻等级约产生温和的 10% CPU 份额变化。
 * 该效应相对且累积：从任意 nice 上调一级约少 10%，下调一级约多 10%。
 * 权重表使用约 1.25 的相邻比例，因为两个任务分别上下变化约 10% 后，相对距离
 * 约为 25%。sched_prio_to_weight[] 是 CFS 份额计算的固定语义数据。
 */
const int sched_prio_to_weight[40] = {
 /* -20 */     88761,     71755,     56483,     46273,     36291,
 /* -15 */     29154,     23254,     18705,     14949,     11916,
 /* -10 */      9548,      7620,      6100,      4904,      3906,
 /*  -5 */      3121,      2501,      1991,      1586,      1277,
 /*   0 */      1024,       820,       655,       526,       423,
 /*   5 */       335,       272,       215,       172,       137,
 /*  10 */       110,        87,        70,        56,        45,
 /*  15 */        36,        29,        23,        18,        15,
};

/*
 * Inverse (2^32/x) values of the sched_prio_to_weight[] array, pre-calculated.
 *
 * In cases where the weight does not change often, we can use the
 * pre-calculated inverse to speed up arithmetics by turning divisions
 * into multiplications:
 */
/*
 * 中文：sched_prio_to_wmult[] 预计算上述权重的 2^32/x 倒数。权重不常变化时，
 * 热路径可用乘法和移位代替除法，减少调度实体虚拟运行时间计算成本。
 */
const u32 sched_prio_to_wmult[40] = {
 /* -20 */     48388,     59856,     76040,     92818,    118348,
 /* -15 */    147320,    184698,    229616,    287308,    360437,
 /* -10 */    449829,    563644,    704093,    875809,   1099582,
 /*  -5 */   1376151,   1717300,   2157191,   2708050,   3363326,
 /*   0 */   4194304,   5237765,   6557202,   8165337,  10153587,
 /*   5 */  12820798,  15790321,  19976592,  24970740,  31350126,
 /*  10 */  39045157,  49367440,  61356676,  76695844,  95443717,
 /*  15 */ 119304647, 148102320, 186737708, 238609294, 286331153,
};

/* call_trace_sched_update_nr_running() 是 rq 可运行任务数 tracepoint 的稳定包装层。 */
void call_trace_sched_update_nr_running(struct rq *rq, int count)
{
        trace_sched_update_nr_running_tp(rq, count);
}

#ifdef CONFIG_SCHED_MM_CID
/*
 * Concurrency IDentifier management
 *
 * Serialization rules:
 *
 * mm::mm_cid::mutex:	Serializes fork() and exit() and therefore
 *			protects mm::mm_cid::users and mode switch
 *			transitions
 *
 * mm::mm_cid::lock:	Serializes mm_update_max_cids() and
 *			mm_update_cpus_allowed(). Nests in mm_cid::mutex
 *			and runqueue lock.
 *
 * The mm_cidmask bitmap is not protected by any of the mm::mm_cid locks
 * and can only be modified with atomic operations.
 *
 * The mm::mm_cid:pcpu per CPU storage is protected by the CPUs runqueue
 * lock.
 *
 * CID ownership:
 *
 * A CID is either owned by a task (stored in task_struct::mm_cid.cid) or
 * by a CPU (stored in mm::mm_cid.pcpu::cid). CIDs owned by CPUs have the
 * MM_CID_ONCPU bit set.
 *
 * During the transition of ownership mode, the MM_CID_TRANSIT bit is set
 * on the CIDs. When this bit is set the tasks drop the CID back into the
 * pool when scheduling out.
 *
 * Both bits (ONCPU and TRANSIT) are filtered out by task_cid() when the
 * CID is actually handed over to user space in the RSEQ memory.
 *
 * Mode switching:
 *
 * The ownership mode is per process and stored in mm:mm_cid::mode with the
 * following possible states:
 *
 *	0:				Per task ownership
 *	0 | MM_CID_TRANSIT:		Transition from per CPU to per task
 *	MM_CID_ONCPU:			Per CPU ownership
 *	MM_CID_ONCPU | MM_CID_TRANSIT:	Transition from per task to per CPU
 *
 * All transitions of ownership mode happen in two phases:
 *
 *  1) mm:mm_cid::mode has the MM_CID_TRANSIT bit set. This is OR'ed on the
 *     CIDs and denotes that the CID is only temporarily owned by a
 *     task. When the task schedules out it drops the CID back into the
 *     pool if this bit is set.
 *
 *  2) The initiating context walks the per CPU space or the tasks to fixup
 *     or drop the CIDs and after completion it clears MM_CID_TRANSIT in
 *     mm:mm_cid::mode. After that point the CIDs are strictly task or CPU
 *     owned again.
 *
 * This two phase transition is required to prevent CID space exhaustion
 * during the transition as a direct transfer of ownership would fail:
 *
 *   - On task to CPU mode switch if a task is scheduled in on one CPU and
 *     then migrated to another CPU before the fixup freed enough per task
 *     CIDs.
 *
 *   - On CPU to task mode switch if two tasks are scheduled in on the same
 *     CPU before the fixup freed per CPU CIDs.
 *
 *   Both scenarios can result in a live lock because sched_in() is invoked
 *   with runqueue lock held and loops in search of a CID and the fixup
 *   thread can't make progress freeing them up because it is stuck on the
 *   same runqueue lock.
 *
 * While MM_CID_TRANSIT is active during the transition phase the MM_CID
 * bitmap can be contended, but that's a temporary contention bound to the
 * transition period. After that everything goes back into steady state and
 * nothing except fork() and exit() will touch the bitmap. This is an
 * acceptable tradeoff as it completely avoids complex serialization,
 * memory barriers and atomic operations for the common case.
 *
 * Aside of that this mechanism also ensures RT compability:
 *
 *   - The task which runs the fixup is fully preemptible except for the
 *     short runqueue lock held sections.
 *
 *   - The transient impact of the bitmap contention is only problematic
 *     when there is a thundering herd scenario of tasks scheduling in and
 *     out concurrently. There is not much which can be done about that
 *     except for avoiding mode switching by a proper overall system
 *     configuration.
 *
 * Switching to per CPU mode happens when the user count becomes greater
 * than the maximum number of CIDs, which is calculated by:
 *
 *	opt_cids = min(mm_cid::nr_cpus_allowed, mm_cid::users);
 *	max_cids = min(1.25 * opt_cids, num_possible_cpus());
 *
 * The +25% allowance is useful for tight CPU masks in scenarios where only
 * a few threads are created and destroyed to avoid frequent mode
 * switches. Though this allowance shrinks, the closer opt_cids becomes to
 * num_possible_cpus(), which is the (unfortunate) hard ABI limit.
 *
 * At the point of switching to per CPU mode the new user is not yet
 * visible in the system, so the task which initiated the fork() runs the
 * fixup function. mm_cid_fixup_tasks_to_cpu() walks the thread list and
 * either marks each task owned CID with MM_CID_TRANSIT if the task is
 * running on a CPU or drops it into the CID pool if a task is not on a
 * CPU. Tasks which schedule in before the task walk reaches them do the
 * handover in mm_cid_schedin(). When mm_cid_fixup_tasks_to_cpus()
 * completes it is guaranteed that no task related to that MM owns a CID
 * anymore.
 *
 * Switching back to task mode happens when the user count goes below the
 * threshold which was recorded on the per CPU mode switch:
 *
 *	pcpu_thrs = min(opt_cids - (opt_cids / 4), num_possible_cpus() / 2);
 *
 * This threshold is updated when a affinity change increases the number of
 * allowed CPUs for the MM, which might cause a switch back to per task
 * mode.
 *
 * If the switch back was initiated by a exiting task, then that task runs
 * the fixup function. If it was initiated by a affinity change, then it's
 * run either in the deferred update function in context of a workqueue or
 * by a task which forks a new one or by a task which exits. Whatever
 * happens first. mm_cid_fixup_cpus_to_task() walks through the possible
 * CPUs and either marks the CPU owned CIDs with MM_CID_TRANSIT if a
 * related task is running on the CPU or drops it into the pool. Tasks
 * which are scheduled in before the fixup covered them do the handover
 * themself. When mm_cid_fixup_cpus_to_tasks() completes it is guaranteed
 * that no CID related to that MM is owned by a CPU anymore.
 */
/*
 * 中文总览——并发标识符（CID）管理：
 *
 * mm_cid.mutex 串行化 fork、exit 及 ownership 模式切换并保护 users；
 * mm_cid.lock 串行化 CID 数量与亲和性约束更新。CID 位图只以原子位操作修改；
 * 每 CPU pcpu 槽由对应 CPU 的 rq 锁保护，远端访问也必须取得该 rq 锁。
 *
 * CID 稳态要么归 task 所有并存于 task_struct::mm_cid.cid，要么归 CPU 所有并
 * 存于 mm_cid.pcpu::cid，后者带 MM_CID_ONCPU。转换时再加 MM_CID_TRANSIT：
 * 调度出看到该位会归还临时 CID；task_cid() 向用户态 RSEQ 发布前屏蔽内部位。
 *
 * mode 有 per-task、CPU->task 过渡、per-CPU、task->CPU 过渡四种状态。切换先
 * 发布 TRANSIT，让并发调度点能主动释放，再遍历 CPU 或线程修复，最后清除
 * TRANSIT。直接搬迁可能在任务迁移或同 CPU 快速换入多个线程时耗尽 CID；
 * sched-in 持 rq 锁等待 CID，而修复线程又等待同一 rq 锁，造成活锁。
 *
 * users 超过 max_cids 时切到 per-CPU：opt=min(allowed CPUs, users)，
 * max=min(1.25*opt, possible CPUs)。25% 余量和更低的回切阈值构成滞回，减少
 * 线程频繁创建/销毁引起的模式抖动。亲和性扩展可能经 irq_work->workqueue
 * 延迟回切。修复在 rq 锁下把运行中的所有权标为 TRANSIT，陈旧槽则立即释放。
 */

/*
 * Update the CID range properties when the constraints change. Invoked via
 * fork(), exit() and affinity changes
 */
/*
 * 中文：fork、exit 或亲和性变化时更新 CID 范围属性。
 * __mm_update_max_cids() 在外部锁保护下计算最优值和带 25% 余量的硬上限，
 * 并用 WRITE_ONCE 发布给无锁读者。
 */
static void __mm_update_max_cids(struct mm_mm_cid *mc)
{
	unsigned int opt_cids, max_cids;

	/* Calculate the new optimal constraint */
	/* 中文：最优 CID 数不超过允许 CPU 数或实际用户数。 */
	opt_cids = min(mc->nr_cpus_allowed, mc->users);

	/* Adjust the maximum CIDs to +25% limited by the number of possible CPUs */
	/* 中文：加入 25% 回差，但仍受 possible CPU 数这个 ABI 硬上限约束。 */
	max_cids = min(opt_cids + (opt_cids / 4), num_possible_cpus());
	WRITE_ONCE(mc->max_cids, max_cids);
}

/*
 * mm_cid_calc_pcpu_thrs() 计算从 per-CPU 回切 per-task 的较低阈值。
 * 返回值至少为 1，因为 0 专门表示 per-CPU 模式关闭。
 */
static inline unsigned int mm_cid_calc_pcpu_thrs(struct mm_mm_cid *mc)
{
	unsigned int opt_cids;

	opt_cids = min(mc->nr_cpus_allowed, mc->users);
	/* Has to be at least 1 because 0 indicates PCPU mode off */
	/* 中文：必须至少为 1，因为 0 表示 PCPU 模式未启用。 */
	return max(min(opt_cids - opt_cids / 4, num_possible_cpus() / 2), 1);
}

/*
 * mm_update_max_cids() 在 mm_cid.lock 下重算阈值并判断是否需要模式切换。
 * 切换同时翻转 ONCPU 并置 TRANSIT；随后的全屏障保证任何取得 rq 锁的修复者
 * 不会在 mode 发布之前先执行槽位修复。返回 true 表示调用者必须完成 fixup。
 */
static bool mm_update_max_cids(struct mm_struct *mm)
{
	struct mm_mm_cid *mc = &mm->mm_cid;
	bool percpu = cid_on_cpu(mc->mode);

	lockdep_assert_held(&mm->mm_cid.lock);

	/* Clear deferred mode switch flag. A change is handled by the caller */
	/* 中文：清除延迟标志；当前调用者已经接管这次约束变化。 */
	mc->update_deferred = false;
	__mm_update_max_cids(mc);

	/* Check whether owner mode must be changed */
	/* 中文：依据当前 ownership 模式使用不同方向的滞回阈值。 */
	if (!percpu) {
		/* Enable per CPU mode when the number of users is above max_cids */
		/* 中文：用户数超过 CID 上限时启用 per-CPU ownership。 */
		if (mc->users > mc->max_cids)
			mc->pcpu_thrs = mm_cid_calc_pcpu_thrs(mc);
	} else {
		/* Switch back to per task if user count under threshold */
		/* 中文：用户数降到较低阈值以下时回到 per-task ownership。 */
		if (mc->users < mc->pcpu_thrs)
			mc->pcpu_thrs = 0;
	}

	/* Mode change required? */
	/* 中文：pcpu_thrs 非零代表期望 per-CPU；与当前模式一致则无需修复。 */
	if (percpu == !!mc->pcpu_thrs)
		return false;

	/* Flip the mode and set the transition flag to bridge the transfer */
	/* 中文：翻转所有权位并设置过渡位，为并发调度点建立桥接窗口。 */
	WRITE_ONCE(mc->mode, mc->mode ^ (MM_CID_TRANSIT | MM_CID_ONCPU));
	/*
	 * Order the store against the subsequent fixups so that
	 * acquire(rq::lock) cannot be reordered by the CPU before the
	 * store.
	 */
	/*
	 * 中文：该全屏障排序 mode 存储与后续修复，避免 CPU 把 acquire(rq->lock)
	 * 提前到 mode 发布之前。
	 */
	smp_mb();
	return true;
}

/*
 * mm_update_cpus_allowed() 把线程新亲和性 OR 入 mm 的单调扩展 CPU 超集。
 * per-CPU 模式下若扩展触发回切，它只在自旋锁内设置 deferred 并排 irq_work；
 * 真正可能唤醒和遍历 rq 的修复留给 workqueue，避免 rq 锁反向嵌套。
 */
static inline void mm_update_cpus_allowed(struct mm_struct *mm, const struct cpumask *affmsk)
{
	struct cpumask *mm_allowed;
	struct mm_mm_cid *mc;
	unsigned int weight;

	if (!mm || !READ_ONCE(mm->mm_cid.users))
		return;
	/*
	 * mm::mm_cid::mm_cpus_allowed is the superset of each threads
	 * allowed CPUs mask which means it can only grow.
	 */
	/* 中文：mm_cpus_allowed 是所有线程 allowed mask 的超集，因此只增不减。 */
	mc = &mm->mm_cid;
	guard(raw_spinlock)(&mc->lock);
	mm_allowed = mm_cpus_allowed(mm);
	weight = cpumask_weighted_or(mm_allowed, mm_allowed, affmsk);
	if (weight == mc->nr_cpus_allowed)
		return;

	WRITE_ONCE(mc->nr_cpus_allowed, weight);
	__mm_update_max_cids(mc);
	if (!cid_on_cpu(mc->mode))
		return;

	/* Adjust the threshold to the wider set */
	/* 中文：根据扩展后的集合重算回切阈值。 */
	mc->pcpu_thrs = mm_cid_calc_pcpu_thrs(mc);
	/* Switch back to per task mode? */
	/* 中文：用户数仍不低于阈值时继续保持 per-CPU。 */
	if (mc->users >= mc->pcpu_thrs)
		return;

	/* Don't queue twice */
	/* 中文：一个 mm 最多保留一个待处理更新。 */
	if (mc->update_deferred)
		return;

	/* Queue the irq work, which schedules the real work */
	/* 中文：先排硬 irq_work，再由它安全调度普通 work。 */
	mc->update_deferred = true;
	irq_work_queue(&mc->irq_work);
}

/*
 * mm_cid_complete_transit() 在所有槽位/任务修复完成后发布稳态 mode。
 * 前置全屏障保证 TRANSIT 的清除绝不会越过修复写入。
 */
static inline void mm_cid_complete_transit(struct mm_struct *mm, unsigned int mode)
{
	/*
	 * Ensure that the store removing the TRANSIT bit cannot be
	 * reordered by the CPU before the fixups have been completed.
	 */
	/* 中文：确保清除 TRANSIT 不会被重排到全部 fixup 完成之前。 */
	smp_mb();
	WRITE_ONCE(mm->mm_cid.mode, mode);
}

/*
 * mm_cid_transit_to_task() 把 CPU 所有的 CID 转成 task 临时所有，并镜像到 pcpu
 * 槽；调用者持相应锁，后续调度出负责最终归还。
 */
static inline void mm_cid_transit_to_task(struct task_struct *t, struct mm_cid_pcpu *pcp)
{
	if (cid_on_cpu(t->mm_cid.cid)) {
		unsigned int cid = cpu_cid_to_cid(t->mm_cid.cid);

		t->mm_cid.cid = cid_to_transit_cid(cid);
		pcp->cid = t->mm_cid.cid;
	}
}

/*
 * mm_cid_fixup_cpus_to_tasks() 遍历所有可能 CPU，在各 rq 锁下回收 per-CPU CID。
 * 若 CPU 正运行同 mm 的活跃 task，则转交带 TRANSIT 的 task CID；否则立即归还。
 * 特别排除 MM_CID_UNSET，避免其被当作位号传给 clear_bit 而越界。
 */
static void mm_cid_fixup_cpus_to_tasks(struct mm_struct *mm)
{
	unsigned int cpu;

	/* Walk the CPUs and fixup all stale CIDs */
	/* 中文：遍历所有 CPU 并修复陈旧 CID。 */
	for_each_possible_cpu(cpu) {
		struct mm_cid_pcpu *pcp = per_cpu_ptr(mm->mm_cid.pcpu, cpu);
		struct rq *rq = cpu_rq(cpu);

		/* Remote access to mm::mm_cid::pcpu requires rq_lock */
		/* 中文：远端访问 mm_cid.pcpu 必须持目标 CPU 的 rq 锁。 */
		guard(rq_lock_irq)(rq);
		/* Is the CID still owned by the CPU? */
		/* 中文：先处理仍由 CPU 所有的稳态 CID。 */
		if (cid_on_cpu(pcp->cid)) {
			/*
			 * If rq->curr has @mm, transfer it with the
			 * transition bit set. Otherwise drop it.
			 */
			/* 中文：若 curr 使用该 mm，则带过渡位转交；否则直接释放。 */
			if (rq->curr->mm == mm && rq->curr->mm_cid.active)
				mm_cid_transit_to_task(rq->curr, pcp);
			else
				mm_drop_cid_on_cpu(mm, pcp);

		} else if (rq->curr->mm == mm && rq->curr->mm_cid.active) {
			unsigned int cid = rq->curr->mm_cid.cid;

			/*
			 * Set the transition bit only on a genuine task-owned
			 * CID. A running active task can legitimately have
			 * MM_CID_UNSET here: in per-CPU mode CIDs are assigned
			 * lazily on schedule-in, so the fork()/execve() window
			 * leaves the task active with no owned CID. Setting the
			 * transition bit on MM_CID_UNSET would later feed
			 * clear_bit() an out-of-bounds bit number via
			 * mm_cid_schedout(), so exclude it. A CPU-owned
			 * (MM_CID_ONCPU) CID is handled by the cid_on_cpu()
			 * branch above and never reaches here.
			 */
			/*
			 * 中文：只给真实 task-owned CID 设置 TRANSIT。per-CPU 惰性分配
			 * 窗口允许活跃 task 暂为 MM_CID_UNSET；给哨兵加标志会使调度出
			 * 把越界位号传给 clear_bit。CPU-owned 情况已由前一分支处理。
			 */
			if (cid != MM_CID_UNSET && !cid_in_transit(cid)) {
				cid = cid_to_transit_cid(cid);
				rq->curr->mm_cid.cid = cid;
				pcp->cid = cid;
			}
		}
	}
	mm_cid_complete_transit(mm, 0);
}

/* mm_cid_transit_to_cpu() 将真实 task CID 标为过渡态并交到当前 CPU 槽。 */
static inline void mm_cid_transit_to_cpu(struct task_struct *t, struct mm_cid_pcpu *pcp)
{
	if (cid_on_task(t->mm_cid.cid)) {
		t->mm_cid.cid = cid_to_transit_cid(t->mm_cid.cid);
		pcp->cid = t->mm_cid.cid;
	}
}

/*
 * mm_cid_fixup_task_to_cpu() 持 task 所在 rq 锁修复一个线程：运行中则把 CID
 * 交给 CPU 并保留 TRANSIT，未运行则立即归还；任务的迁移和调度状态因此稳定。
 */
static void mm_cid_fixup_task_to_cpu(struct task_struct *t, struct mm_struct *mm)
{
	/* Remote access to mm::mm_cid::pcpu requires rq_lock */
	/* 中文：访问任务所在 CPU 的 pcpu 状态必须持该 rq 锁。 */
	guard(task_rq_lock)(t);
	if (cid_on_task(t->mm_cid.cid)) {
		/* If running on the CPU, put the CID in transit mode, otherwise drop it */
		/* 中文：运行中进入过渡态，否则直接释放 task CID。 */
		if (task_rq(t)->curr == t)
			mm_cid_transit_to_cpu(t, per_cpu_ptr(mm->mm_cid.pcpu, task_cpu(t)));
		else
			mm_unset_cid_on_task(t);
	}
}

/*
 * mm_cid_fixup_tasks_to_cpus() 在 mm_cid.mutex 下遍历 user_list，将 task
 * ownership 转为 CPU ownership；current 已在发起切换时处理，故跳过。
 */
static void mm_cid_fixup_tasks_to_cpus(void)
{
	struct mm_struct *mm = current->mm;
	struct task_struct *t;

	lockdep_assert_held(&mm->mm_cid.mutex);

	hlist_for_each_entry(t, &mm->mm_cid.user_list, mm_cid.node) {
		/* Current has already transferred before invoking the fixup. */
		/* 中文：current 在调用修复前已经完成转交。 */
		if (t != current)
			mm_cid_fixup_task_to_cpu(t, mm);
	}

	mm_cid_complete_transit(mm, MM_CID_ONCPU);
}

/*
 * sched_mm_cid_add_user() 在 mm_cid.lock 下把 task 发布进 user_list、递增 users，
 * 随后重算模式。active 先置位，使调度路径能识别其已受 CID 管理。
 */
static bool sched_mm_cid_add_user(struct task_struct *t, struct mm_struct *mm)
{
	lockdep_assert_held(&mm->mm_cid.lock);

	t->mm_cid.active = 1;
	hlist_add_head(&t->mm_cid.node, &mm->mm_cid.user_list);
	mm->mm_cid.users++;
	return mm_update_max_cids(mm);
}

/*
 * sched_mm_cid_fork() 把新 task 加入共享 mm 的 CID 管理。首用户直接取得 CID；
 * 稳态加入按当前模式分配或惰性等待；跨阈值时先在锁内转换 current 的 CID，
 * 再于锁外完成全线程/全 CPU 修复。mutex 保证 fork/exit 不交叉切换模式。
 */
static void sched_mm_cid_fork(struct task_struct *t)
{
	struct mm_struct *mm = t->mm;
	bool percpu;

	if (!mm)
		return;

	WARN_ON_ONCE(t->mm_cid.cid != MM_CID_UNSET);

	guard(mutex)(&mm->mm_cid.mutex);
	scoped_guard(raw_spinlock_irq, &mm->mm_cid.lock) {
		struct mm_cid_pcpu *pcp = this_cpu_ptr(mm->mm_cid.pcpu);

		/* First user ? */
		/* 中文：首用户建立 task CID，并为紧随其后的 execve 保存 pcpu 镜像。 */
		if (!mm->mm_cid.users) {
			sched_mm_cid_add_user(t, mm);
			t->mm_cid.cid = mm_get_cid(mm);
			/* Required for execve() */
			pcp->cid = t->mm_cid.cid;
			return;
		}

		if (!sched_mm_cid_add_user(t, mm)) {
			if (!cid_on_cpu(mm->mm_cid.mode))
				t->mm_cid.cid = mm_get_cid(mm);
			return;
		}

		/* Handle the mode change and transfer current's CID */
		/* 中文：处理模式变化，并先转交发起者 current 的 CID。 */
		percpu = cid_on_cpu(mm->mm_cid.mode);
		if (!percpu)
			mm_cid_transit_to_task(current, pcp);
		else
			mm_cid_transit_to_cpu(current, pcp);
	}

	if (percpu) {
		mm_cid_fixup_tasks_to_cpus();
	} else {
		mm_cid_fixup_cpus_to_tasks(mm);
		t->mm_cid.cid = mm_get_cid(mm);
	}
}

/*
 * sched_mm_cid_remove_user() 在 mm_cid.lock 下撤销 task：清 active/TRANSIT，
 * 归还 CID、摘链并递减 users，最后返回是否触发模式变化。
 */
static bool sched_mm_cid_remove_user(struct task_struct *t)
{
	lockdep_assert_held(&t->mm->mm_cid.lock);

	t->mm_cid.active = 0;
	/* Clear the transition bit */
	/* 中文：先清过渡位，再按普通 task CID 释放，避免把标志位当作位图索引。 */
	t->mm_cid.cid = cid_from_transit_cid(t->mm_cid.cid);
	mm_unset_cid_on_task(t);
	hlist_del_init(&t->mm_cid.node);
	t->mm->mm_cid.users--;
	return mm_update_max_cids(t->mm);
}

/*
 * __sched_mm_cid_exit() 处理非末用户退出并判断是否需要 per-CPU->per-task 修复。
 * 退出触发的变化方向必须是回切；current 与 @t 也必须共享 mm，否则无法安全处理
 * 本 CPU 槽。违反不变量时告警并拒绝继续修复。
 */
static bool __sched_mm_cid_exit(struct task_struct *t)
{
	struct mm_struct *mm = t->mm;

	if (!sched_mm_cid_remove_user(t))
		return false;
	/*
	 * Contrary to fork() this only deals with a switch back to per
	 * task mode either because the above decreased users or an
	 * affinity change increased the number of allowed CPUs and the
	 * deferred fixup did not run yet.
	 */
	/*
	 * 中文：与 fork 不同，这里只处理回到 per-task：可能由 users 减少触发，也
	 * 可能接手尚未执行的亲和性扩展延迟修复。
	 */
	if (WARN_ON_ONCE(cid_on_cpu(mm->mm_cid.mode)))
		return false;
	/*
	 * A failed fork(2) cleanup never gets here, so @current must have
	 * the same MM as @t. That's true for exit() and the failed
	 * pthread_create() cleanup case.
	 */
	/*
	 * 中文：失败 fork(2) 的清理不会到这里；exit 与失败 pthread_create 清理中，
	 * current 必须和 @t 使用相同 mm。
	 */
	if (WARN_ON_ONCE(current->mm != mm))
		return false;
	return true;
}

/*
 * When a task exits, the MM CID held by the task is not longer required as
 * the task cannot return to user space.
 */
/*
 * 原文：task 退出后不可能再返回用户态，因此不再需要其 MM CID。
 *
 * @t 是退出/exec 清理中的稳定 task；无直接返回值。函数先用 mm_cid.mutex
 * 串行化 fork/exit/affinity 的模式切换，再在 raw spinlock 下从 user_list 摘除
 * task。若用户数阈值使模式从 per-CPU 退回 per-task，先撤销本 CPU CID，再修复
 * 其他 CPU/task 的过渡标志；最后一个用户则同步 irq_work/work，确保 mm 销毁前
 * 没有异步回调保留悬空引用。常见无 mm/未激活路径直接返回。
 */
void sched_mm_cid_exit(struct task_struct *t)
{
	struct mm_struct *mm = t->mm;

	if (!mm || !t->mm_cid.active)
		return;
	/*
	 * Ensure that only one instance is doing MM CID operations within
	 * a MM. The common case is uncontended. The rare fixup case adds
	 * some overhead.
	 */
	/*
	 * 中文：保证同一 mm 同时只有一个 CID 操作者；常见路径无竞争，只有少见的
	 * 模式修复会增加额外开销。
	 */
	scoped_guard(mutex, &mm->mm_cid.mutex) {
		/* mm_cid::mutex is sufficient to protect mm_cid::users */
		/* 中文：在此生命周期协议中 mutex 已足以稳定 users。 */
		if (likely(mm->mm_cid.users > 1)) {
			scoped_guard(raw_spinlock_irq, &mm->mm_cid.lock) {
				if (!__sched_mm_cid_exit(t))
					return;
				/*
				 * Mode change. The task has the CID unset
				 * already and dealt with an eventually set
				 * TRANSIT bit. If the CID is owned by the CPU
				 * then drop it.
				 */
				/*
				 * 中文：模式已改变；task CID 已撤销且 TRANSIT 已处理，若本
				 * CPU 仍拥有 CID，则在锁内归还。
				 */
				mm_drop_cid_on_cpu(mm, this_cpu_ptr(mm->mm_cid.pcpu));
			}
			mm_cid_fixup_cpus_to_tasks(mm);
			return;
		}
		/* Last user */
		/* 中文：最后一个用户无需再维护模式，只需完成资源释放。 */
		scoped_guard(raw_spinlock_irq, &mm->mm_cid.lock) {
			/* Required across execve() */
			/* 中文：execve 跨 mm 切换要求先把本 CPU CID 暂转到 current。 */
			if (t == current)
				mm_cid_transit_to_task(t, this_cpu_ptr(mm->mm_cid.pcpu));
			/* Ignore mode change. There is nothing to do. */
			/* 中文：最后用户离开后没有消费者，模式变化无需修复。 */
			sched_mm_cid_remove_user(t);
		}
	}

	/*
	 * As this is the last user (execve(), process exit or failed
	 * fork(2)) there is no concurrency anymore.
	 *
	 * Synchronize eventually pending work to ensure that there are no
	 * dangling references left. @t->mm_cid.users is zero so nothing
	 * can queue this work anymore.
	 */
	/*
	 * 中文：最后用户意味着已无并发；同步可能尚在队列中的 irq_work/work，
	 * 确保 mm 释放前不再有悬空引用。users 为 0 也保证不会再有新工作入队。
	 */
	irq_work_sync(&mm->mm_cid.irq_work);
	cancel_work_sync(&mm->mm_cid.work);
}

/* Deactivate MM CID allocation across execve() */
/*
 * 原文：execve 提交地址空间期间暂时停用 MM CID，避免旧 mm/task 的 CID 状态
 * 穿越不可回滚的 mm 替换边界。
 */
void sched_mm_cid_before_execve(struct task_struct *t)
{
	sched_mm_cid_exit(t);
}

/* Reactivate MM CID after execve() */
/*
 * 原文：execve 完成后，若 current 仍有用户 mm，则按 fork/add-user 协议重新加入
 * CID 管理。@t 为借用 task，无返回值；失败路径由内部降级策略处理。
 */
void sched_mm_cid_after_execve(struct task_struct *t)
{
	if (t->mm)
		sched_mm_cid_fork(t);
}

/*
 * mm_cid_work_fn() 在可睡眠的 workqueue 上接手亲和性触发的延迟回切。
 * mutex 与 spinlock 下重新验证用户、deferred 和阈值，避免 fork/exit 已经处理后
 * 重复修复；只有真正切入 per-task 后才在锁外遍历 CPU。
 */
static void mm_cid_work_fn(struct work_struct *work)
{
	struct mm_struct *mm = container_of(work, struct mm_struct, mm_cid.work);

	guard(mutex)(&mm->mm_cid.mutex);
	/* Did the last user task exit already? */
	/* 中文：最后用户可能已退出，此时工作直接结束。 */
	if (!mm->mm_cid.users)
		return;

	scoped_guard(raw_spinlock_irq, &mm->mm_cid.lock) {
		/* Have fork() or exit() handled it already? */
		/* 中文：fork/exit 可能抢先接管并完成这次延迟变化。 */
		if (!mm->mm_cid.update_deferred)
			return;
		/* This clears mm_cid::update_deferred */
		/* 中文：该调用清除 update_deferred，并决定是否真正切换 mode。 */
		if (!mm_update_max_cids(mm))
			return;
		/* Affinity changes can only switch back to task mode */
		/* 中文：allowed 超集扩大只可能触发回到 per-task。 */
		if (WARN_ON_ONCE(cid_on_cpu(mm->mm_cid.mode)))
			return;
	}
	mm_cid_fixup_cpus_to_tasks(mm);
}

/*
 * mm_cid_irq_work() 运行在硬 irq_work 上下文，只负责无条件排普通 work。
 * 不能在持 mm_cid.lock 时直接 schedule_work：该锁可能嵌套于 rq 锁，而工作唤醒
 * 又会进入调度器，形成锁反转。
 */
static void mm_cid_irq_work(struct irq_work *work)
{
	struct mm_struct *mm = container_of(work, struct mm_struct, mm_cid.irq_work);

	/*
	 * Needs to be unconditional because mm_cid::lock cannot be held
	 * when scheduling work as mm_update_cpus_allowed() nests inside
	 * rq::lock and schedule_work() might end up in wakeup...
	 */
	/*
	 * 中文：必须无条件调度；mm_update_cpus_allowed() 可能位于 rq 锁内，
	 * schedule_work() 的唤醒路径又会进入调度器，因此不能在 mm_cid.lock 内调用。
	 */
	schedule_work(&mm->mm_cid.work);
}

/*
 * mm_init_cid() - 初始化新 mm 的 CID 生命周期根对象。
 *
 * @mm 是尚未向其他 task 发布的新地址空间，由调用者拥有；@p 提供初始亲和性
 * 快照，二者均不转移 ownership。函数清零阈值/模式/用户数，初始化 spinlock、
 * mutex、irq_work、普通 work 与用户链表，并复制 allowed CPU/mask。无失败返回；
 * 成功后 sched_mm_cid_fork() 才会加入首个用户并可能启动动态模式切换。
 */
void mm_init_cid(struct mm_struct *mm, struct task_struct *p)
{
	mm->mm_cid.max_cids = 0;
	mm->mm_cid.mode = 0;
	mm->mm_cid.nr_cpus_allowed = p->nr_cpus_allowed;
	mm->mm_cid.users = 0;
	mm->mm_cid.pcpu_thrs = 0;
	mm->mm_cid.update_deferred = 0;
	raw_spin_lock_init(&mm->mm_cid.lock);
	mutex_init(&mm->mm_cid.mutex);
	mm->mm_cid.irq_work = IRQ_WORK_INIT_HARD(mm_cid_irq_work);
	INIT_WORK(&mm->mm_cid.work, mm_cid_work_fn);
	INIT_HLIST_HEAD(&mm->mm_cid.user_list);
	cpumask_copy(mm_cpus_allowed(mm), &p->cpus_mask);
	bitmap_zero(mm_cidmask(mm), num_possible_cpus());
}
#else /* CONFIG_SCHED_MM_CID */
static inline void mm_update_cpus_allowed(struct mm_struct *mm, const struct cpumask *affmsk) { }
static inline void sched_mm_cid_fork(struct task_struct *t) { }
#endif /* !CONFIG_SCHED_MM_CID */

/*
 * sched_change_ctx 是每 CPU 的短生命周期事务记录。调用者已禁抢占并持 rq 锁，
 * begin 到 end 之间不会被同 CPU 另一 task 复用；它保存 task 原来的 queued、
 * running、class 和优先级，以便策略/group 变化后精确恢复。它不持 task 引用。
 */
static DEFINE_PER_CPU(struct sched_change_ctx, sched_change_ctx);

/*
 * sched_change_begin() - 暂时把 @p 从旧调度状态中撤出，建立可修改窗口。
 *
 * @p 为当前 rq 上的借用 task，调用者持 rq 锁；@flags 同时作为 dequeue/enqueue
 * 原因，低 16 位必须成对。返回 per-CPU ctx 借用指针，必须在同一不可抢占、持锁
 * 窗口交给 sched_change_end()。函数最多更新一次 rq clock，记录旧 class/prio，
 * queued 时 dequeue、running 时 put_prev。无可恢复失败；中途不得睡眠或迁移 CPU。
 */
struct sched_change_ctx *sched_change_begin(struct task_struct *p, unsigned int flags)
{
	struct sched_change_ctx *ctx = this_cpu_ptr(&sched_change_ctx);
	struct rq *rq = task_rq(p);

	/*
	 * Must exclusively use matched flags since this is both dequeue and
	 * enqueue.
	 */
	/* 中文：同一 flags 同时用于 dequeue/enqueue，只允许低 16 位的成对标志。 */
	WARN_ON_ONCE(flags & 0xFFFF0000);

	lockdep_assert_rq_held(rq);

	if (!(flags & DEQUEUE_NOCLOCK)) {
		update_rq_clock(rq);
		flags |= DEQUEUE_NOCLOCK;
	}

	if ((flags & DEQUEUE_CLASS) && p->sched_class->switching_from)
		p->sched_class->switching_from(rq, p);

	*ctx = (struct sched_change_ctx){
		.p = p,
		.class = p->sched_class,
		.flags = flags,
		.queued = task_on_rq_queued(p),
		.running = task_current_donor(rq, p),
	};

	if (!(flags & DEQUEUE_CLASS)) {
		if (p->sched_class->get_prio)
			ctx->prio = p->sched_class->get_prio(rq, p);
		else
			ctx->prio = p->prio;
	}

	if (ctx->queued)
		dequeue_task(rq, p, flags);
	if (ctx->running)
		put_prev_task(rq, p);

	if ((flags & DEQUEUE_CLASS) && p->sched_class->switched_from)
		p->sched_class->switched_from(rq, p);

	return ctx;
}

/*
 * sched_change_end() - 在策略、优先级或组属性修改后恢复 @ctx 中的 task。
 *
 * 调用者必须保持 begin 时的同一 rq 锁与不可抢占窗口。函数按旧 queued/running
 * 状态重新 enqueue/set_next；若跨调度类，则调用新类切入回调，并根据升类/降类
 * 分别通知旧类被抢占或请求当前 CPU 重调度；未换类则把旧优先级传给 prio_changed。
 * @ctx 是 per-CPU 借用对象，返回后即失效；无错误返回。
 */
void sched_change_end(struct sched_change_ctx *ctx)
{
	struct task_struct *p = ctx->p;
	struct rq *rq = task_rq(p);

	lockdep_assert_rq_held(rq);

	/*
	 * Changing class without *QUEUE_CLASS is bad.
	 */
	/* 中文：若没有 ENQUEUE_CLASS 却改变 sched_class，调用协议已被破坏。 */
	WARN_ON_ONCE(p->sched_class != ctx->class && !(ctx->flags & ENQUEUE_CLASS));

	if ((ctx->flags & ENQUEUE_CLASS) && p->sched_class->switching_to)
		p->sched_class->switching_to(rq, p);

	if (ctx->queued)
		enqueue_task(rq, p, ctx->flags);
	if (ctx->running)
		set_next_task(rq, p);

	if (ctx->flags & ENQUEUE_CLASS) {
		if (p->sched_class->switched_to)
			p->sched_class->switched_to(rq, p);

		if (ctx->running) {
			/*
			 * If this was a class promotion; let the old class
			 * know it got preempted. Note that none of the
			 * switch*_from() methods know the new class and none
			 * of the switch*_to() methods know the old class.
			 */
			/*
			 * 中文：若任务升到更高调度类，让旧类知道它已被抢占。from 系列
			 * 回调不知道新类，to 系列也不知道旧类，故由核心补足跨类动作。
			 */
			if (sched_class_above(p->sched_class, ctx->class)) {
				rq->next_class->wakeup_preempt(rq, p, 0);
				rq->next_class = p->sched_class;
			}
			/*
			 * If this was a degradation in class; make sure to
			 * reschedule.
			 */
			/* 中文：若降到较低类，必须重调度，让更高类候选及时运行。 */
			if (sched_class_above(ctx->class, p->sched_class))
				resched_curr(rq);
		}
	} else {
		p->sched_class->prio_changed(rq, p, ctx->prio);
	}
}
