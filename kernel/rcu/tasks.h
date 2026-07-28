/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Tasks RCU 学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 文件职责：
 *   本文件实现按“任务经过安全状态”判定宽限期的 RCU 家族，并共享一套
 *   per-CPU 回调队列、GP kthread、barrier、诊断和自测试框架。classic
 *   Tasks RCU 等待自愿上下文切换/idle/用户态；Rude 通过每 CPU 调度工作
 *   强制制造上下文切换；Trace flavor 在文件末映射到 SRCU-fast。
 *
 * 主调用链：
 *   call_rcu_tasks*() -> call_rcu_tasks_generic() 入队
 *     -> GP kthread -> flavor gp_func 等待任务安全状态
 *     -> rcu_tasks_invoke_cbs() 摘取并执行成熟回调。
 *   同步 API 借助 wakeme_after_rcu 回调等待同一状态机完成。
 *
 * 核心对象与并发：
 *   struct rcu_tasks 是 flavor 全局状态；struct rcu_tasks_percpu 保存分片
 *   回调队列和退出任务列表。per-CPU raw spinlock 保护队列，cbs_gbl_lock
 *   保护队列扩缩发布，tasks_gp_mutex 串行 GP，barrier mutex/计数/completion
 *   串行并等待 barrier。
 *
 * 方案权衡：
 *   Tasks RCU 能覆盖动态修改函数前导、tracing/BPF hook 等执行区间；代价
 *   是 GP 可能很长、需扫描任务并维护 holdout，Rude 还会强制所有 CPU 调度。
 *   它不是普通 rcu_read_lock() 数据结构保护的替代品。
 */
/*
 * Task-based RCU implementations.
 *
 * Copyright (C) 2020 Paul E. McKenney
 */
/*
 * 本文件包含基于任务安全状态的 RCU 实现。上述版权信息保持上游原文；
 * 与普通 RCU 追踪 CPU 静止状态不同，这里证明旧任务执行区间已经结束。
 */

#ifdef CONFIG_TASKS_RCU_GENERIC
#include "rcu_segcblist.h"

////////////////////////////////////////////////////////////////////////
//
// Generic data structures.

/*
 * 通用回调类型把不同 flavor 的宽限期、扫描和收尾策略注入共享状态机。
 * 各函数均借用传入对象：gp_func 完成 GP；pregp/postgp 建立前后内存序；
 * pertask 可持有任务引用并加入 @hop；postscan 补退出盲区；holdouts 重查
 * 并移除已经安全的任务。
 */
struct rcu_tasks;
typedef void (*rcu_tasks_gp_func_t)(struct rcu_tasks *rtp);
typedef void (*pregp_func_t)(struct list_head *hop);
typedef void (*pertask_func_t)(struct task_struct *t, struct list_head *hop);
typedef void (*postscan_func_t)(struct list_head *hop);
typedef void (*holdouts_func_t)(struct list_head *hop, bool ndrpt, bool *frptp);
typedef void (*postgp_func_t)(struct rcu_tasks *rtp);

/**
 * struct rcu_tasks_percpu - Per-CPU component of definition for a Tasks-RCU-like mechanism.
 * @cblist: Callback list.
 * @lock: Lock protecting per-CPU callback list.
 * @rtp_jiffies: Jiffies counter value for statistics.
 * @lazy_timer: Timer to unlazify callbacks.
 * @urgent_gp: Number of additional non-lazy grace periods.
 * @rtp_n_lock_retries: Rough lock-contention statistic.
 * @rtp_work: Work queue for invoking callbacks.
 * @rtp_irq_work: IRQ work queue for deferred wakeups.
 * @barrier_q_head: RCU callback for barrier operation.
 * @rtp_blkd_tasks: List of tasks blocked as readers.
 * @rtp_exit_list: List of tasks in the latter portion of do_exit().
 * @cpu: CPU number corresponding to this entry.
 * @index: Index of this CPU in rtpcp_array of the rcu_tasks structure.
 * @rtpp: Pointer to the rcu_tasks structure.
 */
/*
 * struct rcu_tasks_percpu - 一个 Tasks RCU flavor 的 per-CPU 分片。
 *
 * cblist/lock 是分段回调队列及其 raw spinlock；入队者、GP kthread 和工作
 * 队列以该锁串行推进/摘取。rtp_jiffies 与 rtp_n_lock_retries 统计同一
 * jiffy 内的锁竞争，用于决定是否扩展为真正 per-CPU 队列。
 *
 * lazy_timer/urgent_gp 管理 lazy 回调到期和紧急 GP；rtp_work 在 workqueue
 * 执行成熟回调，rtp_irq_work 从关中断入队路径延迟唤醒 GP 线程。
 * barrier_q_head 是每队列专用 barrier 哨兵。
 *
 * rtp_blkd_tasks/rtp_exit_list 保存被 flavor 阻塞的读者和 do_exit() 扫描
 * 盲区任务；相关路径按需持有 task 引用。cpu/index 映射 CPU 与 workqueue
 * 扩散数组，rtpp 是全局 flavor 借用指针。分片静态创建、随内核存续。
 */
struct rcu_tasks_percpu {
	struct rcu_segcblist cblist;
	raw_spinlock_t __private lock;
	unsigned long rtp_jiffies;
	unsigned long rtp_n_lock_retries;
	struct timer_list lazy_timer;
	unsigned int urgent_gp;
	struct work_struct rtp_work;
	struct irq_work rtp_irq_work;
	struct rcu_head barrier_q_head;
	struct list_head rtp_blkd_tasks;
	struct list_head rtp_exit_list;
	int cpu;
	int index;
	struct rcu_tasks *rtpp;
};

/**
 * struct rcu_tasks - Definition for a Tasks-RCU-like mechanism.
 * @cbs_wait: RCU wait allowing a new callback to get kthread's attention.
 * @cbs_gbl_lock: Lock protecting callback list.
 * @tasks_gp_mutex: Mutex protecting grace period, needed during mid-boot dead zone.
 * @gp_func: This flavor's grace-period-wait function.
 * @gp_state: Grace period's most recent state transition (debugging).
 * @gp_sleep: Per-grace-period sleep to prevent CPU-bound looping.
 * @init_fract: Initial backoff sleep interval.
 * @gp_jiffies: Time of last @gp_state transition.
 * @gp_start: Most recent grace-period start in jiffies.
 * @tasks_gp_seq: Number of grace periods completed since boot in upper bits.
 * @n_ipis: Number of IPIs sent to encourage grace periods to end.
 * @n_ipis_fails: Number of IPI-send failures.
 * @kthread_ptr: This flavor's grace-period/callback-invocation kthread.
 * @lazy_jiffies: Number of jiffies to allow callbacks to be lazy.
 * @pregp_func: This flavor's pre-grace-period function (optional).
 * @pertask_func: This flavor's per-task scan function (optional).
 * @postscan_func: This flavor's post-task scan function (optional).
 * @holdouts_func: This flavor's holdout-list scan function (optional).
 * @postgp_func: This flavor's post-grace-period function (optional).
 * @call_func: This flavor's call_rcu()-equivalent function.
 * @wait_state: Task state for synchronous grace-period waits (default TASK_UNINTERRUPTIBLE).
 * @rtpcpu: This flavor's rcu_tasks_percpu structure.
 * @rtpcp_array: Array of pointers to rcu_tasks_percpu structure of CPUs in cpu_possible_mask.
 * @percpu_enqueue_shift: Shift down CPU ID this much when enqueuing callbacks.
 * @percpu_enqueue_lim: Number of per-CPU callback queues in use for enqueuing.
 * @percpu_dequeue_lim: Number of per-CPU callback queues in use for dequeuing.
 * @percpu_dequeue_gpseq: RCU grace-period number to propagate enqueue limit to dequeuers.
 * @barrier_q_mutex: Serialize barrier operations.
 * @barrier_q_count: Number of queues being waited on.
 * @barrier_q_completion: Barrier wait/wakeup mechanism.
 * @barrier_q_seq: Sequence number for barrier operations.
 * @barrier_q_start: Most recent barrier start in jiffies.
 * @name: This flavor's textual name.
 * @kname: This flavor's kthread name.
 */
/*
 * struct rcu_tasks - 一个 Tasks RCU flavor 的全局控制器。
 *
 * cbs_wait 让新回调唤醒 GP 线程；cbs_gbl_lock 串行 enqueue/dequeue 队列
 * 数切换；tasks_gp_mutex 串行 GP 和 mid-boot 同步调用。
 *
 * gp_state/gp_jiffies/gp_start 只用于阶段与 stall 诊断；tasks_gp_seq 是
 * rcu_seq 编码的 GP 序号。kthread_ptr 用 release/acquire 发布线程，读取
 * 它不取得 task 引用。gp_func 及 pregp/pertask/postscan/holdouts/postgp
 * 组成 flavor 状态机，call_func 为同步 waiter 登记异步回调。
 *
 * rtpcpu/rtpcp_array 指向静态 per-CPU 分片。enqueue_shift/enqueue_lim
 * 决定新回调落到几条队列，dequeue_lim 是 GP 侧仍须扫描的范围。收缩时
 * 必须先缩入队范围，经过普通 RCU GP 后再缩出队范围，gpseq 记录交接边界。
 *
 * barrier mutex/count/completion/seq 汇合每队列哨兵并支持并发 barrier。
 * name/kname 是静态字符串借用指针；整个对象静态创建且不释放。
 */
struct rcu_tasks {
	struct rcuwait cbs_wait;
	raw_spinlock_t cbs_gbl_lock;
	struct mutex tasks_gp_mutex;
	int gp_state;
	int gp_sleep;
	int init_fract;
	unsigned long gp_jiffies;
	unsigned long gp_start;
	unsigned long tasks_gp_seq;
	unsigned long n_ipis;
	unsigned long n_ipis_fails;
	struct task_struct *kthread_ptr;
	unsigned long lazy_jiffies;
	rcu_tasks_gp_func_t gp_func;
	pregp_func_t pregp_func;
	pertask_func_t pertask_func;
	postscan_func_t postscan_func;
	holdouts_func_t holdouts_func;
	postgp_func_t postgp_func;
	call_rcu_func_t call_func;
	unsigned int wait_state;
	struct rcu_tasks_percpu __percpu *rtpcpu;
	struct rcu_tasks_percpu **rtpcp_array;
	int percpu_enqueue_shift;
	int percpu_enqueue_lim;
	int percpu_dequeue_lim;
	unsigned long percpu_dequeue_gpseq;
	struct mutex barrier_q_mutex;
	atomic_t barrier_q_count;
	struct completion barrier_q_completion;
	unsigned long barrier_q_seq;
	unsigned long barrier_q_start;
	char *name;
	char *kname;
};

static void call_rcu_tasks_iw_wakeup(struct irq_work *iwp);

/*
 * DEFINE_RCU_TASKS() - 定义一个 flavor 的静态 per-CPU 分片与全局控制器。
 *
 * @rt_name：对象名；@gp：GP 等待函数；@call：异步登记入口；@n：日志名称。
 * 初始只启用一条聚合队列，generic 代码可按竞争扩到 per-CPU。所有锁、wait、
 * 函数表和 irq_work 都在 kthread 发布前静态初始化，避免半初始化可见。
 */
#define DEFINE_RCU_TASKS(rt_name, gp, call, n)						\
static DEFINE_PER_CPU(struct rcu_tasks_percpu, rt_name ## __percpu) = {			\
	.lock = __RAW_SPIN_LOCK_UNLOCKED(rt_name ## __percpu.cbs_pcpu_lock),		\
	.rtp_irq_work = IRQ_WORK_INIT_HARD(call_rcu_tasks_iw_wakeup),			\
};											\
static struct rcu_tasks rt_name =							\
{											\
	.cbs_wait = __RCUWAIT_INITIALIZER(rt_name.wait),				\
	.cbs_gbl_lock = __RAW_SPIN_LOCK_UNLOCKED(rt_name.cbs_gbl_lock),			\
	.tasks_gp_mutex = __MUTEX_INITIALIZER(rt_name.tasks_gp_mutex),			\
	.gp_func = gp,									\
	.call_func = call,								\
	.wait_state = TASK_UNINTERRUPTIBLE,						\
	.rtpcpu = &rt_name ## __percpu,							\
	.lazy_jiffies = DIV_ROUND_UP(HZ, 4),						\
	.name = n,									\
	.percpu_enqueue_shift = order_base_2(CONFIG_NR_CPUS),				\
	.percpu_enqueue_lim = 1,							\
	.percpu_dequeue_lim = 1,							\
	.barrier_q_mutex = __MUTEX_INITIALIZER(rt_name.barrier_q_mutex),		\
	.barrier_q_seq = (0UL - 50UL) << RCU_SEQ_CTR_SHIFT,				\
	.kname = #rt_name,								\
}

#ifdef CONFIG_TASKS_RCU

/* Report delay of scan exiting tasklist in rcu_tasks_postscan(). */
/*
 * postscan 合并退出任务列表时启动该定时器；扫描迟迟不结束便周期报告 GP
 * 阶段。@unused 不使用，timer 对象静态存续。
 */
static void tasks_rcu_exit_srcu_stall(struct timer_list *unused);
static DEFINE_TIMER(tasks_rcu_exit_srcu_stall_timer, tasks_rcu_exit_srcu_stall);
#endif

/* Control stall timeouts.  Disable with <= 0, otherwise jiffies till stall. */
/*
 * stall 参数单位均为 jiffies：启动默认 30 秒，运行期默认 10 分钟；
 * timeout <= 0 关闭正式报告。info 默认每 10 秒给出预警，info_mult 控制
 * 后续提示指数退避且启动时限制在 1..10。
 */
#define RCU_TASK_BOOT_STALL_TIMEOUT (HZ * 30)
#define RCU_TASK_STALL_TIMEOUT (HZ * 60 * 10)
static int rcu_task_stall_timeout __read_mostly = RCU_TASK_STALL_TIMEOUT;
module_param(rcu_task_stall_timeout, int, 0644);
#define RCU_TASK_STALL_INFO (HZ * 10)
static int rcu_task_stall_info __read_mostly = RCU_TASK_STALL_INFO;
module_param(rcu_task_stall_info, int, 0644);
static int rcu_task_stall_info_mult __read_mostly = 3;
module_param(rcu_task_stall_info_mult, int, 0444);

/*
 * enqueue_lim 是启动队列上限：-1 表示一条起步并允许自适应，0 规范化为 1，
 * 正值固定初始并行度。cb_adjust 记录是否自适应；contend_lim 是扩容竞争
 * 阈值，collapse_lim 是收缩阈值，lazy_lim 是积压后催促 GP 的阈值。
 */
static int rcu_task_enqueue_lim __read_mostly = -1;
module_param(rcu_task_enqueue_lim, int, 0444);

static bool rcu_task_cb_adjust;
static int rcu_task_contend_lim __read_mostly = 100;
module_param(rcu_task_contend_lim, int, 0444);
static int rcu_task_collapse_lim __read_mostly = 10;
module_param(rcu_task_collapse_lim, int, 0444);
static int rcu_task_lazy_lim __read_mostly = 32;
module_param(rcu_task_lazy_lim, int, 0444);

/* possible CPU 最高编号加一，用于队列映射和遍历上限，不是在线 CPU 数。 */
static int rcu_task_cpu_ids;

/* RCU tasks grace-period state for debugging. */
/*
 * RTGS_* 仅记录 GP kthread 最近阶段用于诊断：初始化、等待回调、扫描任务/
 * holdout、GP 后处理、执行回调等。它们不参与正确性判定；字符串数组与数值
 * 下标一一对应。
 */
#define RTGS_INIT		 0
#define RTGS_WAIT_WAIT_CBS	 1
#define RTGS_WAIT_GP		 2
#define RTGS_PRE_WAIT_GP	 3
#define RTGS_SCAN_TASKLIST	 4
#define RTGS_POST_SCAN_TASKLIST	 5
#define RTGS_WAIT_SCAN_HOLDOUTS	 6
#define RTGS_SCAN_HOLDOUTS	 7
#define RTGS_POST_GP		 8
#define RTGS_WAIT_READERS	 9
#define RTGS_INVOKE_CBS		10
#define RTGS_WAIT_CBS		11
#ifndef CONFIG_TINY_RCU
static const char * const rcu_tasks_gp_state_names[] = {
	"RTGS_INIT",
	"RTGS_WAIT_WAIT_CBS",
	"RTGS_WAIT_GP",
	"RTGS_PRE_WAIT_GP",
	"RTGS_SCAN_TASKLIST",
	"RTGS_POST_SCAN_TASKLIST",
	"RTGS_WAIT_SCAN_HOLDOUTS",
	"RTGS_SCAN_HOLDOUTS",
	"RTGS_POST_GP",
	"RTGS_WAIT_READERS",
	"RTGS_INVOKE_CBS",
	"RTGS_WAIT_CBS",
};
#endif /* #ifndef CONFIG_TINY_RCU */

////////////////////////////////////////////////////////////////////////
//
// Generic code.
/* 通用代码只驱动队列和 GP 生命周期，具体安全状态由 flavor 函数表决定。 */

static void rcu_tasks_invoke_cbs_wq(struct work_struct *wp);

/* Record grace-period phase and time. */
/*
 * set_tasks_gp_state() - 更新诊断阶段及其开始时间。
 * @rtp 是静态 flavor 借用指针，@newstate 是 RTGS_*；无返回、不会睡眠。
 * 写入不加锁，诊断读者接受近似快照，真正 GP 同步不依赖这些字段。
 */
static void set_tasks_gp_state(struct rcu_tasks *rtp, int newstate)
{
	rtp->gp_state = newstate;
	rtp->gp_jiffies = jiffies;
}

#ifndef CONFIG_TINY_RCU
/* Return state name. */
/*
 * tasks_gp_state_getname() - 把瞬时 GP 状态转换为静态字符串。
 * @rtp 只读借用；返回静态名称或越界时的 "???"，无需释放。data_race()
 * 接受诊断竞态，READ_ONCE 防止数组检查与索引使用不同快照。
 */
static const char *tasks_gp_state_getname(struct rcu_tasks *rtp)
{
	int i = data_race(rtp->gp_state); // Let KCSAN detect update races
	/* 有意保留竞态读取，让 KCSAN 仍能发现并报告诊断字段的并发更新。 */
	int j = READ_ONCE(i); // Prevent the compiler from reading twice
	/* 固定局部快照，防止编译器为范围检查和数组索引各取一次。 */

	if (j >= ARRAY_SIZE(rcu_tasks_gp_state_names))
		return "???";
	return rcu_tasks_gp_state_names[j];
}
#endif /* #ifndef CONFIG_TINY_RCU */

// Initialize per-CPU callback lists for the specified flavor of
// Tasks RCU.  Do not enqueue callbacks before this function is invoked.
/*
 * cblist_init_generic() - 启动期初始化指定 flavor 的全部回调分片。
 *
 * @rtp 尚未向入队者发布。无返回；rtpcp_array 分配失败会 BUG，因为全部回调
 * 路径都依赖它。要求启动单线程/关中断阶段调用，之前禁止入队。
 *
 * 先规范化队列上限，再初始化每个 possible CPU 的锁、segcblist、work、
 * 索引、退出/阻塞列表和 barrier 哨兵，最后计算 CPU ID 到 lim 个 bucket
 * 的右移映射并同时发布初始 enqueue/dequeue 范围。
 */
static void cblist_init_generic(struct rcu_tasks *rtp)
{
	/*
	 * lim 是队列数，shift 是 CPU ID 映射位移，maxcpu 表示编号上界，index
	 * 是 rtpcp_array 紧凑下标；这些局部量只在启动构造阶段有效。
	 */
	int cpu;
	int lim;
	int shift;
	int maxcpu;
	int index = 0;

	if (rcu_task_enqueue_lim < 0) {
		rcu_task_enqueue_lim = 1;
		rcu_task_cb_adjust = true;
	} else if (rcu_task_enqueue_lim == 0) {
		rcu_task_enqueue_lim = 1;
	}
	lim = rcu_task_enqueue_lim;

	rtp->rtpcp_array = kzalloc_objs(struct rcu_tasks_percpu *,
					num_possible_cpus());
	BUG_ON(!rtp->rtpcp_array);

	for_each_possible_cpu(cpu) {
		struct rcu_tasks_percpu *rtpcp = per_cpu_ptr(rtp->rtpcpu, cpu);

		WARN_ON_ONCE(!rtpcp);
		if (cpu)
			raw_spin_lock_init(&ACCESS_PRIVATE(rtpcp, lock));
		if (rcu_segcblist_empty(&rtpcp->cblist))
			rcu_segcblist_init(&rtpcp->cblist);
		INIT_WORK(&rtpcp->rtp_work, rcu_tasks_invoke_cbs_wq);
		rtpcp->cpu = cpu;
		rtpcp->rtpp = rtp;
		rtpcp->index = index;
		rtp->rtpcp_array[index] = rtpcp;
		index++;
		if (!rtpcp->rtp_blkd_tasks.next)
			INIT_LIST_HEAD(&rtpcp->rtp_blkd_tasks);
		if (!rtpcp->rtp_exit_list.next)
			INIT_LIST_HEAD(&rtpcp->rtp_exit_list);
		rtpcp->barrier_q_head.next = &rtpcp->barrier_q_head;
		maxcpu = cpu;
	}

	rcu_task_cpu_ids = maxcpu + 1;
	/* 若 ilog2 结果仍会产生等于 lim 的桶索引，再右移一位保证不越界。 */
	if (lim > rcu_task_cpu_ids)
		lim = rcu_task_cpu_ids;
	shift = ilog2(rcu_task_cpu_ids / lim);
	if (((rcu_task_cpu_ids - 1) >> shift) >= lim)
		shift++;
	rtp->percpu_enqueue_shift = shift;
	rtp->percpu_dequeue_lim = lim;
	rtp->percpu_enqueue_lim = lim;

	pr_info("%s: Setting shift to %d and lim to %d rcu_task_cb_adjust=%d rcu_task_cpu_ids=%d.\n",
			rtp->name, data_race(rtp->percpu_enqueue_shift), data_race(rtp->percpu_enqueue_lim),
			rcu_task_cb_adjust, rcu_task_cpu_ids);
}

// Compute wakeup time for lazy callback timer.
/*
 * rcu_tasks_lazy_time() 返回 jiffies + lazy_jiffies 的绝对到期值；@rtp 只读
 * 借用、无副作用。timer 比较使用环形时间语义，因此允许 unsigned 溢出。
 */
static unsigned long rcu_tasks_lazy_time(struct rcu_tasks *rtp)
{
	return jiffies + rtp->lazy_jiffies;
}

// Timer handler that unlazifies lazy callbacks.
/*
 * call_rcu_tasks_generic_timer() - 把等待过久的 lazy 回调升级为紧急 GP。
 *
 * @tlp 嵌入 per-CPU 分片；timer/softirq 上下文不可睡眠。持分片 raw lock
 * 时检查队列、至少设置一次 urgent GP 并重置定时器，解锁后唤醒 cbs_wait，
 * 避免在队列锁内扩大唤醒锁依赖。
 */
static void call_rcu_tasks_generic_timer(struct timer_list *tlp)
{
	unsigned long flags;
	bool needwake = false;
	struct rcu_tasks *rtp;
	struct rcu_tasks_percpu *rtpcp = timer_container_of(rtpcp, tlp,
						            lazy_timer);

	rtp = rtpcp->rtpp;
	raw_spin_lock_irqsave_rcu_node(rtpcp, flags);
	if (!rcu_segcblist_empty(&rtpcp->cblist) && rtp->lazy_jiffies) {
		if (!rtpcp->urgent_gp)
			rtpcp->urgent_gp = 1;
		needwake = true;
		mod_timer(&rtpcp->lazy_timer, rcu_tasks_lazy_time(rtp));
	}
	raw_spin_unlock_irqrestore_rcu_node(rtpcp, flags);
	if (needwake)
		rcuwait_wake_up(&rtp->cbs_wait);
}

// IRQ-work handler that does deferred wakeup for call_rcu_tasks_generic().
/*
 * call_rcu_tasks_iw_wakeup() - 从 irq_work 安全边界唤醒 GP kthread。
 * @iwp 嵌入 per-CPU 分片；函数只借用对象、无返回且不可睡眠。
 */
static void call_rcu_tasks_iw_wakeup(struct irq_work *iwp)
{
	struct rcu_tasks *rtp;
	struct rcu_tasks_percpu *rtpcp = container_of(iwp, struct rcu_tasks_percpu, rtp_irq_work);

	rtp = rtpcp->rtpp;
	rcuwait_wake_up(&rtp->cbs_wait);
}

// Enqueue a callback for the specified flavor of Tasks RCU.
/*
 * call_rcu_tasks_generic() - 把回调节点发布到选定 flavor 的分段队列。
 *
 * @rhp 入队后交给 RCU，回调前不得复用；@func 是非空回调，特殊的
 * wakeme_after_rcu 代表同步 waiter；@rtp 是静态 flavor 借用指针。
 * 无返回、不会睡眠，可在允许本地关中断的上下文调用。
 *
 * kthread_ptr 的 acquire 读取与 GP 线程 release 发布配对。关中断加普通 RCU
 * 读锁稳定队列映射；分片 raw lock 保护 segcblist。竞争过高时先释放分片锁，
 * 再在 cbs_gbl_lock 下扩容，避免反向锁序。
 */
static void call_rcu_tasks_generic(struct rcu_head *rhp, rcu_callback_t func,
				   struct rcu_tasks *rtp)
{
	/*
	 * ideal/chosen_cpu 是映射桶和实际 possible CPU；needwake 决定立即催促；
	 * needadjust 把扩容延迟到解锁后；rtpcp 是静态分片借用指针。
	 */
	int chosen_cpu;
	unsigned long flags;
	bool havekthread = smp_load_acquire(&rtp->kthread_ptr);
	int ideal_cpu;
	unsigned long j;
	bool needadjust = false;
	bool needwake;
	struct rcu_tasks_percpu *rtpcp;

	rhp->next = NULL;
	rhp->func = func;
	/* 阶段 1：稳定当前队列映射并取得目标分片锁。 */
	local_irq_save(flags);
	rcu_read_lock();
	ideal_cpu = smp_processor_id() >> READ_ONCE(rtp->percpu_enqueue_shift);
	chosen_cpu = cpumask_next(ideal_cpu - 1, cpu_possible_mask);
	WARN_ON_ONCE(chosen_cpu >= rcu_task_cpu_ids);
	rtpcp = per_cpu_ptr(rtp->rtpcpu, chosen_cpu);
	if (!raw_spin_trylock_rcu_node(rtpcp)) { // irqs already disabled.
		raw_spin_lock_rcu_node(rtpcp); // irqs already disabled.
		j = jiffies;
		if (rtpcp->rtp_jiffies != j) {
			rtpcp->rtp_jiffies = j;
			rtpcp->rtp_n_lock_retries = 0;
		}
		if (rcu_task_cb_adjust && ++rtpcp->rtp_n_lock_retries > rcu_task_contend_lim &&
		    READ_ONCE(rtp->percpu_enqueue_lim) != rcu_task_cpu_ids)
			needadjust = true;  // Defer adjustment to avoid deadlock.
	}
	// Queuing callbacks before initialization not yet supported.
	/* 启动协议违例时告警并补初始化，正常路径必须先运行 cblist_init_generic()。 */
	if (WARN_ON_ONCE(!rcu_segcblist_is_enabled(&rtpcp->cblist)))
		rcu_segcblist_init(&rtpcp->cblist);
	needwake = (!havekthread && rcu_segcblist_empty(&rtpcp->cblist)) ||
		   (func == wakeme_after_rcu) ||
		   (rcu_segcblist_n_cbs(&rtpcp->cblist) == rcu_task_lazy_lim);
	if (havekthread && !needwake && !timer_pending(&rtpcp->lazy_timer)) {
		if (rtp->lazy_jiffies)
			mod_timer(&rtpcp->lazy_timer, rcu_tasks_lazy_time(rtp));
		else
			needwake = rcu_segcblist_empty(&rtpcp->cblist);
	}
	if (needwake)
		rtpcp->urgent_gp = 3;
	/* 入队完成所有权转移；urgent_gp=3 让后续数轮跳过 lazy 合并。 */
	rcu_segcblist_enqueue(&rtpcp->cblist, rhp);
	raw_spin_unlock_irqrestore_rcu_node(rtpcp, flags);
	if (unlikely(needadjust)) {
		/*
		 * 先扩大 shift/dequeue 范围，再以 release 写 enqueue_lim；入队者不会
		 * 看见“新桶可用但 GP 侧尚不扫描”的半提交状态。
		 */
		raw_spin_lock_irqsave(&rtp->cbs_gbl_lock, flags);
		if (rtp->percpu_enqueue_lim != rcu_task_cpu_ids) {
			WRITE_ONCE(rtp->percpu_enqueue_shift, 0);
			WRITE_ONCE(rtp->percpu_dequeue_lim, rcu_task_cpu_ids);
			smp_store_release(&rtp->percpu_enqueue_lim, rcu_task_cpu_ids);
			pr_info("Switching %s to per-CPU callback queuing.\n", rtp->name);
		}
		raw_spin_unlock_irqrestore(&rtp->cbs_gbl_lock, flags);
	}
	rcu_read_unlock();
	/* We can't create the thread unless interrupts are enabled. */
	/* 线程已存在且需催促时排 irq_work，避免在本地关中断区直接唤醒线程。 */
	if (needwake && READ_ONCE(rtp->kthread_ptr))
		irq_work_queue(&rtpcp->rtp_irq_work);
}

// RCU callback function for rcu_barrier_tasks_generic().
/*
 * rcu_barrier_tasks_generic_cb() - 标记一条分片队列已越过 barrier。
 * @rhp 必须是 barrier_q_head；next 指回自身表示已调用，最后一个哨兵把
 * completion 置完成。无返回，不释放静态分片。
 */
static void rcu_barrier_tasks_generic_cb(struct rcu_head *rhp)
{
	struct rcu_tasks *rtp;
	struct rcu_tasks_percpu *rtpcp;

	rhp->next = rhp; // Mark the callback as having been invoked.
	rtpcp = container_of(rhp, struct rcu_tasks_percpu, barrier_q_head);
	rtp = rtpcp->rtpp;
	if (atomic_dec_and_test(&rtp->barrier_q_count))
		complete(&rtp->barrier_q_completion);
}

// Wait for all in-flight callbacks for the specified RCU Tasks flavor.
// Operates in a manner similar to rcu_barrier().
/*
 * rcu_barrier_tasks_generic() - 等待调用前已登记的该 flavor 回调全部执行。
 *
 * @rtp 静态借用；可能睡眠。barrier mutex 串行调用，序号允许后来调用者
 * 复用已覆盖其起点的 barrier。每条活动队列 entrain 一个专用哨兵；count
 * 初值 2 防止遍历中提前归零，遍历后统一减 2，再等待最后回调完成。
 */
static void __maybe_unused rcu_barrier_tasks_generic(struct rcu_tasks *rtp)
{
	int cpu;
	unsigned long flags;
	struct rcu_tasks_percpu *rtpcp;
	unsigned long s = rcu_seq_snap(&rtp->barrier_q_seq);

	mutex_lock(&rtp->barrier_q_mutex);
	/* 已有 barrier 覆盖本次快照时，以全屏障维持前后顺序后直接返回。 */
	if (rcu_seq_done(&rtp->barrier_q_seq, s)) {
		smp_mb();
		mutex_unlock(&rtp->barrier_q_mutex);
		return;
	}
	rtp->barrier_q_start = jiffies;
	rcu_seq_start(&rtp->barrier_q_seq);
	init_completion(&rtp->barrier_q_completion);
	atomic_set(&rtp->barrier_q_count, 2);
	for_each_possible_cpu(cpu) {
		if (cpu >= smp_load_acquire(&rtp->percpu_dequeue_lim))
			break;
		rtpcp = per_cpu_ptr(rtp->rtpcpu, cpu);
		rtpcp->barrier_q_head.func = rcu_barrier_tasks_generic_cb;
		raw_spin_lock_irqsave_rcu_node(rtpcp, flags);
		if (rcu_segcblist_entrain(&rtpcp->cblist, &rtpcp->barrier_q_head))
			atomic_inc(&rtp->barrier_q_count);
		raw_spin_unlock_irqrestore_rcu_node(rtpcp, flags);
	}
	if (atomic_sub_and_test(2, &rtp->barrier_q_count))
		complete(&rtp->barrier_q_completion);
	wait_for_completion(&rtp->barrier_q_completion);
	/* completion 后发布结束序号，调用者可安全卸载此前回调所需资源。 */
	rcu_seq_end(&rtp->barrier_q_seq);
	mutex_unlock(&rtp->barrier_q_mutex);
}

// Advance callbacks and indicate whether either a grace period or
// callback invocation is needed.
/*
 * rcu_tasks_need_gpcb() - 推进所有活动队列并判断是否需要 GP/执行回调。
 *
 * @rtp 静态借用；返回位图：bit0 表示有 ready 回调，bit1 表示需再跑一个 GP，
 * 0 表示可继续睡眠。函数在 GP 线程上下文运行，可取 raw lock 但不睡眠。
 *
 * 同时统计总回调与非 CPU0 回调。自适应收缩分两阶段：先把新入队限制到 CPU0
 * 并记录普通 RCU cookie；只有该 GP 完成且其他队列已空，才缩 dequeue 范围。
 * 这与入队路径的 RCU 读侧区配对，防止漏扫按旧映射入队的回调。
 */
static int rcu_tasks_need_gpcb(struct rcu_tasks *rtp)
{
	/* ncbs/ncbsnz 驱动收缩，needgpcb 聚合 GP/执行需求，gpdone 验证交接 GP。 */
	int cpu;
	int dequeue_limit;
	unsigned long flags;
	bool gpdone = poll_state_synchronize_rcu(rtp->percpu_dequeue_gpseq);
	long n;
	long ncbs = 0;
	long ncbsnz = 0;
	int needgpcb = 0;

	dequeue_limit = smp_load_acquire(&rtp->percpu_dequeue_lim);
	for (cpu = 0; cpu < dequeue_limit; cpu++) {
		if (!cpu_possible(cpu))
			continue;
		struct rcu_tasks_percpu *rtpcp = per_cpu_ptr(rtp->rtpcpu, cpu);

		/* Advance and accelerate any new callbacks. */
		/* 空队列无需取锁；非空时推进已完成段并把新回调绑定到当前 GP 快照。 */
		if (!rcu_segcblist_n_cbs(&rtpcp->cblist))
			continue;
		raw_spin_lock_irqsave_rcu_node(rtpcp, flags);
		// Should we shrink down to a single callback queue?
		/* n 同时用于诊断负载与确认 CPU0 之外是否仍有不能遗漏的回调。 */
		n = rcu_segcblist_n_cbs(&rtpcp->cblist);
		if (n) {
			ncbs += n;
			if (cpu > 0)
				ncbsnz += n;
		}
		rcu_segcblist_advance(&rtpcp->cblist, rcu_seq_current(&rtp->tasks_gp_seq));
		(void)rcu_segcblist_accelerate(&rtpcp->cblist, rcu_seq_snap(&rtp->tasks_gp_seq));
		if (rtpcp->urgent_gp > 0 && rcu_segcblist_pend_cbs(&rtpcp->cblist)) {
			if (rtp->lazy_jiffies)
				rtpcp->urgent_gp--;
			needgpcb |= 0x3;
		} else if (rcu_segcblist_empty(&rtpcp->cblist)) {
			rtpcp->urgent_gp = 0;
		}
		if (rcu_segcblist_ready_cbs(&rtpcp->cblist))
			needgpcb |= 0x1;
		raw_spin_unlock_irqrestore_rcu_node(rtpcp, flags);
	}

	// Shrink down to a single callback queue if appropriate.
	// This is done in two stages: (1) If there are no more than
	// rcu_task_collapse_lim callbacks on CPU 0 and none on any other
	// CPU, limit enqueueing to CPU 0.  (2) After an RCU grace period,
	// if there has not been an increase in callbacks, limit dequeuing
	// to CPU 0.  Note the matching RCU read-side critical section in
	// call_rcu_tasks_generic().
	/*
	 * 阶段 2：先 release 发布 enqueue_lim=1，阻止新的非 CPU0 入队；cookie
	 * 等待所有按旧范围运行的 RCU 入队者退出。GP 前绝不能提前缩 dequeue。
	 */
	if (rcu_task_cb_adjust && ncbs <= rcu_task_collapse_lim) {
		raw_spin_lock_irqsave(&rtp->cbs_gbl_lock, flags);
		if (rtp->percpu_enqueue_lim > 1) {
			WRITE_ONCE(rtp->percpu_enqueue_shift, order_base_2(rcu_task_cpu_ids));
			smp_store_release(&rtp->percpu_enqueue_lim, 1);
			rtp->percpu_dequeue_gpseq = get_state_synchronize_rcu();
			gpdone = false;
			pr_info("Starting switch %s to CPU-0 callback queuing.\n", rtp->name);
		}
		raw_spin_unlock_irqrestore(&rtp->cbs_gbl_lock, flags);
	}
	if (rcu_task_cb_adjust && !ncbsnz && gpdone) {
		/* 阶段 3：旧映射读者已退出且其他队列为空，安全完成 dequeue 收缩。 */
		raw_spin_lock_irqsave(&rtp->cbs_gbl_lock, flags);
		if (rtp->percpu_enqueue_lim < rtp->percpu_dequeue_lim) {
			WRITE_ONCE(rtp->percpu_dequeue_lim, 1);
			pr_info("Completing switch %s to CPU-0 callback queuing.\n", rtp->name);
		}
		if (rtp->percpu_dequeue_lim == 1) {
			for (cpu = rtp->percpu_dequeue_lim; cpu < rcu_task_cpu_ids; cpu++) {
				if (!cpu_possible(cpu))
					continue;
				struct rcu_tasks_percpu *rtpcp = per_cpu_ptr(rtp->rtpcpu, cpu);

				WARN_ON_ONCE(rcu_segcblist_n_cbs(&rtpcp->cblist));
			}
		}
		raw_spin_unlock_irqrestore(&rtp->cbs_gbl_lock, flags);
	}

	return needgpcb;
}

// Advance callbacks and invoke any that are ready.
/*
 * rcu_tasks_invoke_cbs() - 扩散工作并执行一个分片上的成熟回调。
 *
 * @rtp/@rtpcp 都是静态借用；无返回。函数在 GP kthread 或 workqueue 进程
 * 上下文运行，可以 cond_resched()。先按 rtpcp_array 的二叉树下标把子分片
 * 排到对应在线 CPU（否则 unbound），避免一个线程串行扫描全部 CPU。
 *
 * 当前分片锁内推进 segcblist 并把 done 回调摘到本地 rcl，解锁后逐个调用，
 * 因而回调不能反向持有队列锁。local_bh_disable() 保持与普通 RCU 回调上下文
 * 相近的 BH 约束。最后重新加锁扣减全局队列长度并加速新到回调。
 */
static void rcu_tasks_invoke_cbs(struct rcu_tasks *rtp, struct rcu_tasks_percpu *rtpcp)
{
	/* len 是本批摘取数量，rcl 临时拥有 done 节点，回调执行后 ownership 结束。 */
	int cpuwq;
	unsigned long flags;
	int len;
	int index;
	struct rcu_head *rhp;
	struct rcu_cblist rcl = RCU_CBLIST_INITIALIZER(rcl);
	struct rcu_tasks_percpu *rtpcp_next;

	index = rtpcp->index * 2 + 1;
	if (index < num_possible_cpus()) {
		rtpcp_next = rtp->rtpcp_array[index];
		if (rtpcp_next->cpu < smp_load_acquire(&rtp->percpu_dequeue_lim)) {
			cpuwq = rcu_cpu_beenfullyonline(rtpcp_next->cpu) ? rtpcp_next->cpu : WORK_CPU_UNBOUND;
			queue_work_on(cpuwq, system_percpu_wq, &rtpcp_next->rtp_work);
			index++;
			if (index < num_possible_cpus()) {
				rtpcp_next = rtp->rtpcp_array[index];
				if (rtpcp_next->cpu < smp_load_acquire(&rtp->percpu_dequeue_lim)) {
					cpuwq = rcu_cpu_beenfullyonline(rtpcp_next->cpu) ? rtpcp_next->cpu : WORK_CPU_UNBOUND;
					queue_work_on(cpuwq, system_percpu_wq, &rtpcp_next->rtp_work);
				}
			}
		}
	}

	if (rcu_segcblist_empty(&rtpcp->cblist))
		return;
	/* 只在锁内摘链，不在锁内执行任意回调。 */
	raw_spin_lock_irqsave_rcu_node(rtpcp, flags);
	rcu_segcblist_advance(&rtpcp->cblist, rcu_seq_current(&rtp->tasks_gp_seq));
	rcu_segcblist_extract_done_cbs(&rtpcp->cblist, &rcl);
	raw_spin_unlock_irqrestore_rcu_node(rtpcp, flags);
	len = rcl.len;
	for (rhp = rcu_cblist_dequeue(&rcl); rhp; rhp = rcu_cblist_dequeue(&rcl)) {
		debug_rcu_head_callback(rhp);
		local_bh_disable();
		rhp->func(rhp);
		local_bh_enable();
		cond_resched();
	}
	raw_spin_lock_irqsave_rcu_node(rtpcp, flags);
	/* 回调全部执行后再修正原队列计数，随后为并发新回调安排当前 GP。 */
	rcu_segcblist_add_len(&rtpcp->cblist, -len);
	(void)rcu_segcblist_accelerate(&rtpcp->cblist, rcu_seq_snap(&rtp->tasks_gp_seq));
	raw_spin_unlock_irqrestore_rcu_node(rtpcp, flags);
}

// Workqueue flood to advance callbacks and invoke any that are ready.
/*
 * rcu_tasks_invoke_cbs_wq() - workqueue 包装器。
 * @wp 嵌入 per-CPU 分片；container_of 取回分片并调用通用执行器。无返回，
 * workqueue 保证进程上下文，函数不取得额外对象引用。
 */
static void rcu_tasks_invoke_cbs_wq(struct work_struct *wp)
{
	struct rcu_tasks *rtp;
	struct rcu_tasks_percpu *rtpcp = container_of(wp, struct rcu_tasks_percpu, rtp_work);

	rtp = rtpcp->rtpp;
	rcu_tasks_invoke_cbs(rtp, rtpcp);
}

// Wait for one grace period.
/*
 * rcu_tasks_one_gp() - 完成一次“检查需求 -> 可选 GP -> 执行回调”循环。
 *
 * @rtp 静态借用；@midboot 表示 GP kthread 尚不可用，由同步调用者直接驱动。
 * 无返回，可能长时间睡眠。tasks_gp_mutex 确保同一 flavor 只有一个 GP/回调
 * 推进者；正常线程在无需求时先解锁睡在 cbs_wait，避免阻塞 mid-boot 调用。
 *
 * needgpcb bit1 启动 flavor GP：先 rcu_seq_start 发布进行中序号，gp_func 等待，
 * 再 rcu_seq_end 发布完成。最后无论是否新跑 GP 都尝试执行已经成熟的回调。
 */
static void rcu_tasks_one_gp(struct rcu_tasks *rtp, bool midboot)
{
	int needgpcb;

	mutex_lock(&rtp->tasks_gp_mutex);

	// If there were none, wait a bit and start over.
	/* midboot 强制一次 GP；正常线程睡到 need_gpcb 返回非零。 */
	if (unlikely(midboot)) {
		needgpcb = 0x2;
	} else {
		mutex_unlock(&rtp->tasks_gp_mutex);
		set_tasks_gp_state(rtp, RTGS_WAIT_CBS);
		rcuwait_wait_event(&rtp->cbs_wait,
				   (needgpcb = rcu_tasks_need_gpcb(rtp)),
				   TASK_IDLE);
		mutex_lock(&rtp->tasks_gp_mutex);
	}

	if (needgpcb & 0x2) {
		// Wait for one grace period.
		/* bit1 表示仍有 pending/urgent 回调，需要真正启动并完成一个 GP。 */
		set_tasks_gp_state(rtp, RTGS_WAIT_GP);
		rtp->gp_start = jiffies;
		rcu_seq_start(&rtp->tasks_gp_seq);
		rtp->gp_func(rtp);
		rcu_seq_end(&rtp->tasks_gp_seq);
	}

	// Invoke callbacks.
	/* 从数组根分片启动二叉 workqueue flood，mutex 到此仍串行 GP 状态。 */
	set_tasks_gp_state(rtp, RTGS_INVOKE_CBS);
	rcu_tasks_invoke_cbs(rtp, per_cpu_ptr(rtp->rtpcpu, 0));
	mutex_unlock(&rtp->tasks_gp_mutex);
}

// RCU-tasks kthread that detects grace periods and invokes callbacks.
/*
 * rcu_tasks_kthread() - 永久运行的 Tasks RCU GP/回调线程。
 *
 * @arg 必须是静态 rcu_tasks；函数不返回。启动时初始化所有 lazy timer 并
 * 令每个分片至少需要一次紧急 GP，默认绑定 RCU housekeeping CPU。随后以
 * store-release 发布 kthread_ptr，使入队者 acquire 读取后可安全看见全部
 * 初始化。无限循环逐轮推进 GP/回调，并额外 idle sleep 防止异常紧循环。
 */
static int __noreturn rcu_tasks_kthread(void *arg)
{
	int cpu;
	struct rcu_tasks *rtp = arg;

	for_each_possible_cpu(cpu) {
		struct rcu_tasks_percpu *rtpcp = per_cpu_ptr(rtp->rtpcpu, cpu);

		timer_setup(&rtpcp->lazy_timer, call_rcu_tasks_generic_timer, 0);
		rtpcp->urgent_gp = 1;
	}

	/* Run on housekeeping CPUs by default.  Sysadm can move if desired. */
	/* 默认亲和 housekeeping RCU CPU，但管理员之后可以重新设置亲和性。 */
	housekeeping_affine(current, HK_TYPE_RCU);
	smp_store_release(&rtp->kthread_ptr, current); // Let GPs start!

	/*
	 * Each pass through the following loop makes one check for
	 * newly arrived callbacks, and, if there are some, waits for
	 * one RCU-tasks grace period and then invokes the callbacks.
	 * This loop is terminated by the system going down.  ;-)
	 */
	/*
	 * 每轮只检查一次新回调；有需求便等待一个 flavor GP 并执行成熟回调。
	 * 正常终止只发生在系统停机，因此循环没有资源回滚出口。
	 */
	for (;;) {
		// Wait for one grace period and invoke any callbacks
		// that are ready.
		/* 处理一次状态机后主动 idle，给调度器和其他 RCU 工作让出 CPU。 */
		rcu_tasks_one_gp(rtp, false);

		// Paranoid sleep to keep this from entering a tight loop.
		/* 即使状态异常持续唤醒，也额外 idle，防止 GP 线程形成 CPU 忙循环。 */
		schedule_timeout_idle(rtp->gp_sleep);
	}
}

// Wait for a grace period for the specified flavor of Tasks RCU.
/*
 * synchronize_rcu_tasks_generic() - 同步等待指定 flavor 的一个 GP。
 *
 * @rtp 静态借用；无返回，通常会睡眠。调度器尚未启动时只告警并返回，无法
 * 提供完成保证。GP kthread 已发布时用 call_func 登记 wakeme 回调并按
 * wait_state 睡眠；mid-boot 死区则由当前线程直接调用 one_gp()。
 */
static void synchronize_rcu_tasks_generic(struct rcu_tasks *rtp)
{
	/* Complain if the scheduler has not started.  */
	/* 过早调用不能扫描/调度任务，返回仅是失败保护而非成功 GP。 */
	if (WARN_ONCE(rcu_scheduler_active == RCU_SCHEDULER_INACTIVE,
			 "synchronize_%s() called too soon", rtp->name))
		return;

	// If the grace-period kthread is running, use it.
	/* kthread 路径复用异步回调状态机，避免多个同步调用者各自扫描。 */
	if (READ_ONCE(rtp->kthread_ptr)) {
		wait_rcu_gp_state(rtp->wait_state, rtp->call_func);
		return;
	}
	rcu_tasks_one_gp(rtp, true);
}

/* Spawn RCU-tasks grace-period kthread. */
/*
 * rcu_spawn_tasks_kthread_generic() - 创建并发布 flavor GP 线程。
 *
 * @rtp 静态借用；启动期、可睡眠。kthread_run 失败时告警并保留 NULL 指针，
 * 后续同步路径仍可 midboot 驱动，但异步积压可能导致 OOM。成功时线程自身
 * release 发布 kthread_ptr；这里的 smp_mb() 确保创建前初始化对其他 CPU
 * 完整可见。无直接返回值。
 */
static void __init rcu_spawn_tasks_kthread_generic(struct rcu_tasks *rtp)
{
	struct task_struct *t;

	t = kthread_run(rcu_tasks_kthread, rtp, "%s_kthread", rtp->kname);
	if (WARN_ONCE(IS_ERR(t), "%s: Could not start %s grace-period kthread, OOM is now expected behavior\n", __func__, rtp->name))
		return;
	smp_mb(); /* Ensure others see full kthread. */
	/* 全屏障保证其他 CPU 看见完整 kthread/控制器初始化，而非仅看见创建结果。 */
}

#ifndef CONFIG_TINY_RCU

/*
 * Print any non-default Tasks RCU settings.
 */
/*
 * rcu_tasks_bootup_oddness() - 启动时打印非默认参数和已启用 flavor。
 * 无参数、无返回，仅修改越界的 info_mult 为 1..10 并输出诊断；TINY 不编译。
 */
static void __init rcu_tasks_bootup_oddness(void)
{
#if defined(CONFIG_TASKS_RCU) || defined(CONFIG_TASKS_TRACE_RCU)
	int rtsimc;

	if (rcu_task_stall_timeout != RCU_TASK_STALL_TIMEOUT)
		pr_info("\tTasks-RCU CPU stall warnings timeout set to %d (rcu_task_stall_timeout).\n", rcu_task_stall_timeout);
	rtsimc = clamp(rcu_task_stall_info_mult, 1, 10);
	if (rtsimc != rcu_task_stall_info_mult) {
		pr_info("\tTasks-RCU CPU stall info multiplier clamped to %d (rcu_task_stall_info_mult).\n", rtsimc);
		rcu_task_stall_info_mult = rtsimc;
	}
#endif /* #ifdef CONFIG_TASKS_RCU */
#ifdef CONFIG_TASKS_RCU
	pr_info("\tTrampoline variant of Tasks RCU enabled.\n");
#endif /* #ifdef CONFIG_TASKS_RCU */
#ifdef CONFIG_TASKS_RUDE_RCU
	pr_info("\tRude variant of Tasks RCU enabled.\n");
#endif /* #ifdef CONFIG_TASKS_RUDE_RCU */
#ifdef CONFIG_TASKS_TRACE_RCU
	pr_info("\tTracing variant of Tasks RCU enabled.\n");
#endif /* #ifdef CONFIG_TASKS_TRACE_RCU */
}

/* Dump out rcutorture-relevant state common to all RCU-tasks flavors. */
/*
 * show_rcu_tasks_generic_gp_kthread() - 打印一个 flavor 的 GP/回调摘要。
 *
 * @rtp 静态借用，@s 是只读后缀字符串。无锁遍历使用 data_race 接受近似
 * 诊断快照，输出阶段、年龄、GP 序号、IPI、线程/回调/urgent 与 lazy 状态；
 * 不用于正确性判断、无 ownership 变化。
 */
static void show_rcu_tasks_generic_gp_kthread(struct rcu_tasks *rtp, char *s)
{
	/*
	 * havecbs/haveurgent/haveurgentcbs 是跨全部 possible CPU 的布尔摘要；只要
	 * 三者都为真即可提前结束扫描，避免为诊断遍历剩余分片。
	 */
	int cpu;
	bool havecbs = false;
	bool haveurgent = false;
	bool haveurgentcbs = false;

	for_each_possible_cpu(cpu) {
		struct rcu_tasks_percpu *rtpcp = per_cpu_ptr(rtp->rtpcpu, cpu);

		if (!data_race(rcu_segcblist_empty(&rtpcp->cblist)))
			havecbs = true;
		if (data_race(rtpcp->urgent_gp))
			haveurgent = true;
		if (!data_race(rcu_segcblist_empty(&rtpcp->cblist)) && data_race(rtpcp->urgent_gp))
			haveurgentcbs = true;
		if (havecbs && haveurgent && haveurgentcbs)
			break;
	}
	pr_info("%s: %s(%d) since %lu g:%lu i:%lu/%lu %c%c%c%c l:%lu %s\n",
		rtp->kname,
		tasks_gp_state_getname(rtp), data_race(rtp->gp_state),
		jiffies - data_race(rtp->gp_jiffies),
		data_race(rcu_seq_current(&rtp->tasks_gp_seq)),
		data_race(rtp->n_ipis_fails), data_race(rtp->n_ipis),
		".k"[!!data_race(rtp->kthread_ptr)],
		".C"[havecbs],
		".u"[haveurgent],
		".U"[haveurgentcbs],
		rtp->lazy_jiffies,
		s);
}

/* Dump out more rcutorture-relevant state common to all RCU-tasks flavors. */
/*
 * rcu_tasks_torture_stats_print_generic() - 输出更完整的 torture/barrier 状态。
 *
 * @rtp 静态借用；@tt/@tf/@tst 是只读日志前缀。函数可用 GFP_KERNEL 临时分配
 * cpumask，必须在可睡眠上下文；分配失败仍继续打印回调数，只省略 holdout
 * CPU 位图。所有读取都是诊断快照，不改变 GP/回调 ownership。
 */
static void rcu_tasks_torture_stats_print_generic(struct rcu_tasks *rtp, char *tt,
						  char *tf, char *tst)
{
	/*
	 * cm 暂存 barrier 尚未完成的 CPU；gotcb 区分空/非空回调输出；j 固定本次
	 * 报告时间基准，避免多处 jiffies 读取造成互相不一致的“年龄”。
	 */
	cpumask_var_t cm;
	int cpu;
	bool gotcb = false;
	unsigned long j = jiffies;

	pr_alert("%s%s Tasks%s RCU g%ld gp_start %lu gp_jiffies %lu gp_state %d (%s).\n",
		 tt, tf, tst, data_race(rtp->tasks_gp_seq),
		 j - data_race(rtp->gp_start), j - data_race(rtp->gp_jiffies),
		 data_race(rtp->gp_state), tasks_gp_state_getname(rtp));
	pr_alert("\tEnqueue shift %d limit %d Dequeue limit %d gpseq %lu.\n",
		 data_race(rtp->percpu_enqueue_shift),
		 data_race(rtp->percpu_enqueue_lim),
		 data_race(rtp->percpu_dequeue_lim),
		 data_race(rtp->percpu_dequeue_gpseq));
	(void)zalloc_cpumask_var(&cm, GFP_KERNEL);
	/* cm 标出 barrier 哨兵尚未完成的分片；不可用时仍保留其余统计。 */
	pr_alert("\tCallback counts:");
	/* 阶段 1：逐分片汇总回调数量，并把未完成 barrier 哨兵记录到 cm。 */
	for_each_possible_cpu(cpu) {
		long n;
		struct rcu_tasks_percpu *rtpcp = per_cpu_ptr(rtp->rtpcpu, cpu);

		if (cpumask_available(cm) && !rcu_barrier_cb_is_done(&rtpcp->barrier_q_head))
			cpumask_set_cpu(cpu, cm);
		n = rcu_segcblist_n_cbs(&rtpcp->cblist);
		if (!n)
			continue;
		pr_cont(" %d:%ld", cpu, n);
		gotcb = true;
	}
	if (gotcb)
		pr_cont(".\n");
	else
		pr_cont(" (none).\n");
	/*
	 * 阶段 2：输出 barrier 序号、持续时间、剩余计数和分片位图；这些值
	 * 是并发诊断快照，不用于决定 barrier 是否真的完成。
	 */
	pr_alert("\tBarrier seq %lu start %lu count %d holdout CPUs ",
		 data_race(rtp->barrier_q_seq), j - data_race(rtp->barrier_q_start),
		 atomic_read(&rtp->barrier_q_count));
	if (cpumask_available(cm) && !cpumask_empty(cm))
		pr_cont(" %*pbl.\n", cpumask_pr_args(cm));
	else
		pr_cont("(none).\n");
	free_cpumask_var(cm);
}

#endif // #ifndef CONFIG_TINY_RCU

#if defined(CONFIG_TASKS_RCU)

////////////////////////////////////////////////////////////////////////
//
// Shared code between task-list-scanning variants of Tasks RCU.
/* tasklist 扫描型 flavor 共享 holdout 构造、退避等待和 stall 报告框架。 */

/* Wait for one RCU-tasks grace period. */
/*
 * rcu_tasks_wait_gp() - 运行 tasklist 扫描型 flavor 的完整 GP。
 *
 * @rtp 静态借用；无返回、可能长时间睡眠。先调用 pregp，随后在普通 RCU
 * 读侧区扫描全部进程/线程并由 pertask 建 holdout，再由 postscan 补盲区。
 * 然后以逐渐退避的睡眠循环调用 holdouts_func，直到链表为空，最后 postgp。
 *
 * stall_timeout 控制正式报告，stall_info 控制超时前信息并按 multiplier
 * 退避。PREEMPT_RT 使用高分辨率硬定时睡眠，其他配置用 idle timeout。
 */
static void rcu_tasks_wait_gp(struct rcu_tasks *rtp)
{
	/*
	 * holdouts 拥有 pertask 取得的 task 引用；fract 是退避间隔；lastinfo/
	 * lastreport 是两类诊断时间；reported 防止超时后继续打印预警信息。
	 */
	struct task_struct *g;
	int fract;
	LIST_HEAD(holdouts);
	unsigned long j;
	unsigned long lastinfo;
	unsigned long lastreport;
	bool reported = false;
	int rtsi;
	struct task_struct *t;

	set_tasks_gp_state(rtp, RTGS_PRE_WAIT_GP);
	rtp->pregp_func(&holdouts);

	/*
	 * There were callbacks, so we need to wait for an RCU-tasks
	 * grace period.  Start off by scanning the task list for tasks
	 * that are not already voluntarily blocked.  Mark these tasks
	 * and make a list of them in holdouts.
	 */
	/*
	 * 已有回调需要一个 GP：扫描 tasklist，把尚未自愿阻塞的任务标记并加入
	 * holdout。链上任务取得独立引用，所以可跨越 tasklist RCU 解锁继续检查。
	 */
	set_tasks_gp_state(rtp, RTGS_SCAN_TASKLIST);
	/* tasklist RCU 保护枚举关系；被加入 holdout 的任务另取引用跨越解锁。 */
	if (rtp->pertask_func) {
		rcu_read_lock();
		for_each_process_thread(g, t)
			rtp->pertask_func(t, &holdouts);
		rcu_read_unlock();
	}

	set_tasks_gp_state(rtp, RTGS_POST_SCAN_TASKLIST);
	rtp->postscan_func(&holdouts);

	/*
	 * Each pass through the following loop scans the list of holdout
	 * tasks, removing any that are no longer holdouts.  When the list
	 * is empty, we are done.
	 */
	/* 每轮移除已经安全的任务；链表为空即所有旧执行区间均已结束。 */
	lastreport = jiffies;
	lastinfo = lastreport;
	rtsi = READ_ONCE(rcu_task_stall_info);

	// Start off with initial wait and slowly back off to 1 HZ wait.
	/* 从 flavor 初始间隔起步，逐次增至最多 1 秒。 */
	fract = rtp->init_fract;

	while (!list_empty(&holdouts)) {
		ktime_t exp;
		bool firstreport;
		bool needreport;
		int rtst;

		// Slowly back off waiting for holdouts
		/* 等待从 init_fract 缓慢增至 1 秒，兼顾短 GP 延迟与长期扫描开销。 */
		set_tasks_gp_state(rtp, RTGS_WAIT_SCAN_HOLDOUTS);
		if (!IS_ENABLED(CONFIG_PREEMPT_RT)) {
			schedule_timeout_idle(fract);
		} else {
			exp = jiffies_to_nsecs(fract);
			__set_current_state(TASK_IDLE);
			schedule_hrtimeout_range(&exp, jiffies_to_nsecs(HZ / 2), HRTIMER_MODE_REL_HARD);
		}

		if (fract < HZ)
			fract++;

		rtst = READ_ONCE(rcu_task_stall_timeout);
		needreport = rtst > 0 && time_after(jiffies, lastreport + rtst);
		if (needreport) {
			lastreport = jiffies;
			reported = true;
		}
		firstreport = true;
		WARN_ON(signal_pending(current));
		set_tasks_gp_state(rtp, RTGS_SCAN_HOLDOUTS);
		rtp->holdouts_func(&holdouts, needreport, &firstreport);
		/* holdouts_func 负责摘链并释放安全任务的引用；空链即 ownership 清零。 */

		// Print pre-stall informational messages if needed.
		/* 正式 stall 前按倍增间隔打印信息；一旦正式报告便停止这类预警。 */
		j = jiffies;
		if (rtsi > 0 && !reported && time_after(j, lastinfo + rtsi)) {
			lastinfo = j;
			rtsi = rtsi * rcu_task_stall_info_mult;
			pr_info("%s: %s grace period number %lu (since boot) is %lu jiffies old.\n",
				__func__, rtp->kname, rtp->tasks_gp_seq, j - rtp->gp_start);
		}
	}

	set_tasks_gp_state(rtp, RTGS_POST_GP);
	/* 所有持有引用均已释放，再执行 flavor 的最终内存序/退出窗口收尾。 */
	rtp->postgp_func(rtp);
}

#endif /* #if defined(CONFIG_TASKS_RCU) */

#ifdef CONFIG_TASKS_RCU

////////////////////////////////////////////////////////////////////////
//
// Simple variant of RCU whose quiescent states are voluntary context
// switch, cond_resched_tasks_rcu_qs(), user-space execution, and idle.
// As such, grace periods can take one good long time.  There are no
// read-side primitives similar to rcu_read_lock() and rcu_read_unlock()
// because this implementation is intended to get the system into a safe
// state for some of the manipulations involved in tracing and the like.
// Finally, this implementation does not support high call_rcu_tasks()
// rates from multiple CPUs.  If this is required, per-CPU callback lists
// will be needed.
//
// The implementation uses rcu_tasks_wait_gp(), which relies on function
// pointers in the rcu_tasks structure.  The rcu_spawn_tasks_kthread()
// function sets these function pointers up so that rcu_tasks_wait_gp()
// invokes these functions in this order:
//
// rcu_tasks_pregp_step():
//	Invokes synchronize_rcu() in order to wait for all in-flight
//	t->on_rq and t->nvcsw transitions to complete.	This works because
//	all such transitions are carried out with interrupts disabled.
// rcu_tasks_pertask(), invoked on every non-idle task:
//	For every runnable non-idle task other than the current one, use
//	get_task_struct() to pin down that task, snapshot that task's
//	number of voluntary context switches, and add that task to the
//	holdout list.
// rcu_tasks_postscan():
//	Gather per-CPU lists of tasks in do_exit() to ensure that all
//	tasks that were in the process of exiting (and which thus might
//	not know to synchronize with this RCU Tasks grace period) have
//	completed exiting.  The synchronize_rcu() in rcu_tasks_postgp()
//	will take care of any tasks stuck in the non-preemptible region
//	of do_exit() following its call to exit_tasks_rcu_finish().
// check_all_holdout_tasks(), repeatedly until holdout list is empty:
//	Scans the holdout list, attempting to identify a quiescent state
//	for each task on the list.  If there is a quiescent state, the
//	corresponding task is removed from the holdout list.
// rcu_tasks_postgp():
//	Invokes synchronize_rcu() in order to ensure that all prior
//	t->on_rq and t->nvcsw transitions are seen by all CPUs and tasks
//	to have happened before the end of this RCU Tasks grace period.
//	Again, this works because all such transitions are carried out
//	with interrupts disabled.
//
// For each exiting task, the exit_tasks_rcu_start() and
// exit_tasks_rcu_finish() functions add and remove, respectively, the
// current task to a per-CPU list of tasks that rcu_tasks_postscan() must
// wait on.  This is necessary because rcu_tasks_postscan() must wait on
// tasks that have already been removed from the global list of tasks.
//
// Pre-grace-period update-side code is ordered before the grace
// via the raw_spin_lock.*rcu_node().  Pre-grace-period read-side code
// is ordered before the grace period via synchronize_rcu() call in
// rcu_tasks_pregp_step() and by the scheduler's locks and interrupt
// disabling.
/*
 * classic Tasks RCU 的安全状态是自愿上下文切换、显式
 * cond_resched_tasks_rcu_qs()、用户态或 idle；被动抢占不算。因此 GP 可能
 * 很长，也没有普通 RCU 式显式读锁，主要用于 tracing 等执行区间。
 *
 * wait_gp() 依次执行 pregp 稳定调度字段、pertask 持引用建立 holdout、
 * postscan 合并退出盲区、holdouts 反复重查、postgp 补最终内存序。
 * exit_start/finish 把已从 tasklist 摘除但尚未 TASK_DEAD 调度的任务挂入
 * per-CPU 列表。更新侧锁与前后 synchronize_rcu() 共同建立完整协议。
 */

/* Pre-grace-period preparation. */
/*
 * rcu_tasks_pregp_step() - 扫描前建立普通 RCU 排序边界。
 * @hop 本步骤不使用。synchronize_rcu() 等待关中断的 on_rq/nvcsw 更新完成，
 * 防止把 GP 前读者误判为 GP 后开始；同时令首次 holdout 写发生在 GP 起点后。
 * 无返回、可能睡眠。
 */
static void rcu_tasks_pregp_step(struct list_head *hop)
{
	/*
	 * Wait for all pre-existing t->on_rq and t->nvcsw transitions
	 * to complete.  Invoking synchronize_rcu() suffices because all
	 * these transitions occur with interrupts disabled.  Without this
	 * synchronize_rcu(), a read-side critical section that started
	 * before the grace period might be incorrectly seen as having
	 * started after the grace period.
	 *
	 * This synchronize_rcu() also dispenses with the need for a
	 * memory barrier on the first store to t->rcu_tasks_holdout,
	 * as it forces the store to happen after the beginning of the
	 * grace period.
	 */
	/*
	 * 等待 GP 前所有 on_rq/nvcsw 在途转换完成；这些转换均关中断，普通 RCU
	 * GP 足以排序。否则旧读者可能被误看作 GP 后才开始。该等待也令首次
	 * rcu_tasks_holdout 写位于 GP 起点之后，省去单独写屏障。
	 */
	synchronize_rcu();
}

/* Check for quiescent states since the pregp's synchronize_rcu() */
/*
 * rcu_tasks_is_holdout() - 判断 @t 是否仍可能处于旧执行区间。
 *
 * @t 由 tasklist RCU/引用稳定。true 表示继续等待，false 表示已到安全状态；
 * 无引用变化、不会睡眠。!on_rq 表示自愿睡眠；idle loop 是安全状态，但
 * idle task 的 CPU 启动代码不是。sched_delayed 瞬态被保守视为 holdout，
 * 只会延长 GP，不会提前结束。
 */
static bool rcu_tasks_is_holdout(struct task_struct *t)
{
	int cpu;

	/* Has the task been seen voluntarily sleeping? */
	/* on_rq 的稳定快照为 false，说明任务已经自愿阻塞并到达安全状态。 */
	if (!READ_ONCE(t->on_rq))
		return false;

	/*
	 * t->on_rq && !t->se.sched_delayed *could* be considered sleeping but
	 * since it is a spurious state (it will transition into the
	 * traditional blocked state or get woken up without outside
	 * dependencies), not considering it such should only affect timing.
	 *
	 * Be conservative for now and not include it.
	 */
	/*
	 * sched_delayed 是会转为传统阻塞或重新唤醒的短暂状态；保守地不把它算作
	 * 睡眠只会推迟 GP，不会造成过早回收。
	 */

	/*
	 * Idle tasks (or idle injection) within the idle loop are RCU-tasks
	 * quiescent states. But CPU boot code performed by the idle task
	 * isn't a quiescent state.
	 */
	/* idle loop/idle injection 是 QS，但 idle task 执行 CPU 启动代码不是。 */
	if (is_idle_task(t))
		return false;

	cpu = task_cpu(t);

	/* Idle tasks on offline CPUs are RCU-tasks quiescent states. */
	/* 离线 CPU 的 idle task 不会继续旧执行区间，可直接视为 QS。 */
	if (t == idle_task(cpu) && !rcu_cpu_online(cpu))
		return false;

	return true;
}

/* Per-task initial processing. */
/*
 * rcu_tasks_pertask() - 把仍活跃的扫描任务加入本 GP holdout。
 *
 * @t 在 tasklist RCU 下借用，@hop 是 GP 私有链表。跳过 current；对 holdout
 * 先 get_task_struct()，再快照 nvcsw、发布 holdout 位并挂链。引用最终由
 * check_holdout_task() 摘链时 put，形成 ownership 闭环。
 */
static void rcu_tasks_pertask(struct task_struct *t, struct list_head *hop)
{
	if (t != current && rcu_tasks_is_holdout(t)) {
		get_task_struct(t);
		t->rcu_tasks_nvcsw = READ_ONCE(t->nvcsw);
		WRITE_ONCE(t->rcu_tasks_holdout, true);
		list_add(&t->rcu_tasks_holdout_list, hop);
	}
}

void call_rcu_tasks(struct rcu_head *rhp, rcu_callback_t func);
DEFINE_RCU_TASKS(rcu_tasks, rcu_tasks_wait_gp, call_rcu_tasks, "RCU Tasks");

/* Processing between scanning taskslist and draining the holdout list. */
/*
 * rcu_tasks_postscan() - 合并 tasklist 扫描期间的退出任务盲区。
 *
 * @hop 是持有 task 引用的 holdout 链表。函数遍历每 CPU rtp_exit_list 时持
 * raw lock，并可能 cond_resched()。非 TINY 启动 stall timer，结束时同步
 * 删除。do_exit() 的脆弱区由两个重叠窗口覆盖：exit_start 到最终
 * preempt_disable 由 exit_list 覆盖；其后到 TASK_DEAD schedule 由普通
 * RCU 覆盖并在 postgp 等待。
 */
static void rcu_tasks_postscan(struct list_head *hop)
{
	int cpu;
	int rtsi = READ_ONCE(rcu_task_stall_info);

	if (!IS_ENABLED(CONFIG_TINY_RCU)) {
		tasks_rcu_exit_srcu_stall_timer.expires = jiffies + rtsi;
		add_timer(&tasks_rcu_exit_srcu_stall_timer);
	}

	/*
	 * Exiting tasks may escape the tasklist scan. Those are vulnerable
	 * until their final schedule() with TASK_DEAD state. To cope with
	 * this, divide the fragile exit path part in two intersecting
	 * read side critical sections:
	 *
	 * 1) A task_struct list addition before calling exit_notify(),
	 *    which may remove the task from the tasklist, with the
	 *    removal after the final preempt_disable() call in do_exit().
	 *
	 * 2) An _RCU_ read side starting with the final preempt_disable()
	 *    call in do_exit() and ending with the final call to schedule()
	 *    with TASK_DEAD state.
	 *
	 * This handles the part 1). And postgp will handle part 2) with a
	 * call to synchronize_rcu().
	 */
	/* 本函数处理第一个退出窗口；第二窗口留给 postgp 的 synchronize_rcu()。 */

	for_each_possible_cpu(cpu) {
		unsigned long j = jiffies + 1;
		struct rcu_tasks_percpu *rtpcp = per_cpu_ptr(rcu_tasks.rtpcpu, cpu);
		struct task_struct *t;
		struct task_struct *t1;
		struct list_head tmp;

		raw_spin_lock_irq_rcu_node(rtpcp);
		list_for_each_entry_safe(t, t1, &rtpcp->rtp_exit_list, rcu_tasks_exit_list) {
			if (list_empty(&t->rcu_tasks_holdout_list))
				rcu_tasks_pertask(t, hop);

			// RT kernels need frequent pauses, otherwise
			// pause at least once per pair of jiffies.
			/* RT 每次都允许暂停；非 RT 至少每两个 jiffies 让出一次 CPU。 */
			if (!IS_ENABLED(CONFIG_PREEMPT_RT) && time_before(jiffies, j))
				continue;

			// Keep our place in the list while pausing.
			// Nothing else traverses this list, so adding a
			// bare list_head is OK.
			/*
			 * tmp 仅作游标；没有其他遍历者，解锁调度后可从 tmp.next 恢复，
			 * 不需要把裸 list_head 包装成 task_struct。
			 */
			list_add(&tmp, &t->rcu_tasks_exit_list);
			raw_spin_unlock_irq_rcu_node(rtpcp);
			cond_resched(); // For CONFIG_PREEMPT=n kernels
			/* 即使 CONFIG_PREEMPT=n，也在此显式提供一次可调度点。 */
			raw_spin_lock_irq_rcu_node(rtpcp);
			t1 = list_entry(tmp.next, struct task_struct, rcu_tasks_exit_list);
			list_del(&tmp);
			j = jiffies + 1;
		}
		raw_spin_unlock_irq_rcu_node(rtpcp);
	}

	if (!IS_ENABLED(CONFIG_TINY_RCU))
		timer_delete_sync(&tasks_rcu_exit_srcu_stall_timer);
}

/* See if tasks are still holding out, complain if so. */
/*
 * check_holdout_task() - 重查一个持有引用的 holdout 并按需报告 stall。
 *
 * @t 由 holdout 引用保活；@needreport 控制打印；@firstreport 是输入输出
 * 标志，使一轮只打印一次标题。holdout 位清除、nvcsw 改变、任务已安全或
 * NO_HZ_FULL 报告 idle CPU 时，清标志、摘链并 put 引用；否则请求紧急 QS，
 * 必要时打印状态并保留引用等待下轮。
 */
static void check_holdout_task(struct task_struct *t,
			       bool needreport, bool *firstreport)
{
	int cpu;

	if (!READ_ONCE(t->rcu_tasks_holdout) ||
	    t->rcu_tasks_nvcsw != READ_ONCE(t->nvcsw) ||
	    !rcu_tasks_is_holdout(t) ||
	    (IS_ENABLED(CONFIG_NO_HZ_FULL) &&
	     !is_idle_task(t) && READ_ONCE(t->rcu_tasks_idle_cpu) >= 0)) {
		WRITE_ONCE(t->rcu_tasks_holdout, false);
		/* list_del_init 与 put 配对 pertask 的挂链/get；返回后不能再用裸 t。 */
		list_del_init(&t->rcu_tasks_holdout_list);
		put_task_struct(t);
		return;
	}
	rcu_request_urgent_qs_task(t);
	if (!needreport)
		return;
	if (*firstreport) {
		pr_err("INFO: rcu_tasks detected stalls on tasks:\n");
		*firstreport = false;
	}
	cpu = task_cpu(t);
	pr_alert("%p: %c%c nvcsw: %lu/%lu holdout: %d idle_cpu: %d/%d\n",
		 t, ".I"[is_idle_task(t)],
		 "N."[cpu < 0 || !tick_nohz_full_cpu(cpu)],
		 t->rcu_tasks_nvcsw, t->nvcsw, t->rcu_tasks_holdout,
		 data_race(t->rcu_tasks_idle_cpu), cpu);
	sched_show_task(t);
}

/* Scan the holdout lists for tasks no longer holding out. */
/*
 * check_all_holdout_tasks() - 安全遍历当前 GP 的全部 holdout。
 * @hop 为私有链表，其他参数透传单任务检查；safe 迭代允许 callee 摘链并
 * 释放引用，每项后 cond_resched() 防止大任务集长期霸占 CPU。
 */
static void check_all_holdout_tasks(struct list_head *hop,
				    bool needreport, bool *firstreport)
{
	struct task_struct *t, *t1;

	list_for_each_entry_safe(t, t1, hop, rcu_tasks_holdout_list) {
		check_holdout_task(t, needreport, firstreport);
		cond_resched();
	}
}

/* Finish off the Tasks-RCU grace period. */
/*
 * rcu_tasks_postgp() - 用普通 RCU GP 完成 classic Tasks RCU 的最终排序。
 *
 * @rtp 当前不直接使用。synchronize_rcu() 与调度器关中断的 on_rq/nvcsw
 * 更新配对，把全部 holdout 访问限制在当前 GP 内，并等待退出任务最终
 * preempt-disable 到 TASK_DEAD schedule 的第二窗口。无返回、可能睡眠。
 */
static void rcu_tasks_postgp(struct rcu_tasks *rtp)
{
	/*
	 * Because ->on_rq and ->nvcsw are not guaranteed to have a full
	 * memory barriers prior to them in the schedule() path, memory
	 * reordering on other CPUs could cause their RCU-tasks read-side
	 * critical sections to extend past the end of the grace period.
	 * However, because these ->nvcsw updates are carried out with
	 * interrupts disabled, we can use synchronize_rcu() to force the
	 * needed ordering on all such CPUs.
	 *
	 * This synchronize_rcu() also confines all ->rcu_tasks_holdout
	 * accesses to be within the grace period, avoiding the need for
	 * memory barriers for ->rcu_tasks_holdout accesses.
	 *
	 * In addition, this synchronize_rcu() waits for exiting tasks
	 * to complete their final preempt_disable() region of execution,
	 * enforcing the whole region before tasklist removal until
	 * the final schedule() with TASK_DEAD state to be an RCU TASKS
	 * read side critical section.
	 */
	/*
	 * 调度路径未保证 on_rq/nvcsw 前有全屏障，其他 CPU 可能因重排把旧读者
	 * 延伸到 GP 结束之后；这些更新均关中断，所以普通 synchronize_rcu()
	 * 强制所需全局顺序。它同时把 holdout 访问约束在 GP 内，并等待退出任务
	 * 从 tasklist 摘除前直到最终 TASK_DEAD 调度的完整重叠窗口。
	 */
	synchronize_rcu();
}

/*
 * tasks_rcu_exit_srcu_stall() - 报告 postscan 卡在退出任务窗口。
 * @unused 不使用。timer 上下文不可睡眠；非 TINY 下打印 GP 阶段/年龄并
 * 重新挂定时器，直到 postscan 用 timer_delete_sync() 停止。
 */
static void tasks_rcu_exit_srcu_stall(struct timer_list *unused)
{
#ifndef CONFIG_TINY_RCU
	int rtsi;

	rtsi = READ_ONCE(rcu_task_stall_info);
	pr_info("%s: %s grace period number %lu (since boot) gp_state: %s is %lu jiffies old.\n",
		__func__, rcu_tasks.kname, rcu_tasks.tasks_gp_seq,
		tasks_gp_state_getname(&rcu_tasks), jiffies - rcu_tasks.gp_jiffies);
	pr_info("Please check any exiting tasks stuck between calls to exit_tasks_rcu_start() and exit_tasks_rcu_finish()\n");
	tasks_rcu_exit_srcu_stall_timer.expires = jiffies + rtsi;
	add_timer(&tasks_rcu_exit_srcu_stall_timer);
#endif // #ifndef CONFIG_TINY_RCU
}

/**
 * call_rcu_tasks() - Queue an RCU for invocation task-based grace period
 * @rhp: structure to be used for queueing the RCU updates.
 * @func: actual callback function to be invoked after the grace period
 *
 * The callback function will be invoked some time after a full grace
 * period elapses, in other words after all currently executing RCU
 * read-side critical sections have completed. call_rcu_tasks() assumes
 * that the read-side critical sections end at a voluntary context
 * switch (not a preemption!), cond_resched_tasks_rcu_qs(), entry into idle,
 * or transition to usermode execution.  As such, there are no read-side
 * primitives analogous to rcu_read_lock() and rcu_read_unlock() because
 * this primitive is intended to determine that all tasks have passed
 * through a safe state, not so much for data-structure synchronization.
 *
 * See the description of call_rcu() for more detailed information on
 * memory ordering guarantees.
 */
/*
 * call_rcu_tasks() - 在 classic Tasks RCU 宽限期后异步执行回调。
 *
 * @rhp：调用者对象中内嵌的 rcu_head；登记成功后节点交给 Tasks RCU 管理，
 *       回调执行前不得重复登记或释放所属对象。
 * @func：宽限期结束后调用的非空回调，负责完成对象的后续处理/回收。
 * 返回：无直接返回值；登记本身不等待完整宽限期。
 *
 * Tasks RCU 关注“每个任务都经过一次安全状态”，安全状态包括自愿调度、
 * cond_resched_tasks_rcu_qs()、idle 和进入用户态；单纯被抢占不算。因此它
 * 没有普通 rcu_read_lock()/unlock() 式显式读锁，主要服务 tracing、函数
 * 前导和 profiling hook 更新。generic helper 把节点排入 &rcu_tasks 的
 * 回调队列；更新者必须在登记前先阻止新的旧版本使用者。
 */
void call_rcu_tasks(struct rcu_head *rhp, rcu_callback_t func)
{
	call_rcu_tasks_generic(rhp, func, &rcu_tasks);
}
EXPORT_SYMBOL_GPL(call_rcu_tasks);

/**
 * synchronize_rcu_tasks - wait until an rcu-tasks grace period has elapsed.
 *
 * Control will return to the caller some time after a full rcu-tasks
 * grace period has elapsed, in other words after all currently
 * executing rcu-tasks read-side critical sections have elapsed.  These
 * read-side critical sections are delimited by calls to schedule(),
 * cond_resched_tasks_rcu_qs(), idle execution, userspace execution, calls
 * to synchronize_rcu_tasks(), and (in theory, anyway) cond_resched().
 *
 * This is a very specialized primitive, intended only for a few uses in
 * tracing and other situations requiring manipulation of function
 * preambles and profiling hooks.  The synchronize_rcu_tasks() function
 * is not (yet) intended for heavy use from multiple CPUs.
 *
 * See the description of synchronize_rcu() for more detailed information
 * on memory ordering guarantees.
 */
/*
 * synchronize_rcu_tasks() - 同步等待一个 classic Tasks RCU 宽限期。
 *
 * 入参：无。返回：无直接返回值。函数可能长时间睡眠，只能在允许阻塞的
 * 进程上下文调用；返回时，调用前仍在旧执行区间的任务都已经经过自愿调度、
 * Tasks RCU 显式静止状态、idle 或用户态等安全边界。
 *
 * 该原语面向少量 tracing/profiling 文本修改场景，不适合多 CPU 高频调用。
 * 它提供与 synchronize_rcu() 类似的宽限期内存序边界，但等待的读者定义
 * 不同；返回并不会自动释放对象，释放责任仍归调用者。
 */
void synchronize_rcu_tasks(void)
{
	synchronize_rcu_tasks_generic(&rcu_tasks);
}
EXPORT_SYMBOL_GPL(synchronize_rcu_tasks);

/**
 * rcu_barrier_tasks - Wait for in-flight call_rcu_tasks() callbacks.
 *
 * Although the current implementation is guaranteed to wait, it is not
 * obligated to, for example, if there are no pending callbacks.
 */
/*
 * rcu_barrier_tasks() - 等待调用前已经排队的 call_rcu_tasks() 回调处理完毕。
 *
 * 入参：无。返回：无直接返回值。可能睡眠，典型调用者是模块卸载/子系统
 * teardown：只有它返回后，先前回调才不会再进入即将卸载的代码或访问已销毁
 * 资源。接口语义允许在没有待处理回调时直接返回；当前实现即使走等待框架，
 * 调用者也不能依赖具体等待时长。它与 synchronize_rcu_tasks() 不同，后者
 * 等待读者安全状态，但不保证此前所有异步回调均已执行。
 */
void rcu_barrier_tasks(void)
{
	rcu_barrier_tasks_generic(&rcu_tasks);
}
EXPORT_SYMBOL_GPL(rcu_barrier_tasks);

/*
 * rcu_tasks_lazy_ms 是只读模块参数：负值表示采用 generic 默认回调惰性延迟，
 * 非负值按毫秒转换为 lazy_jiffies。它只在启动 Tasks RCU kthread 前读取，
 * 运行期不作为并发可变状态。
 */
static int rcu_tasks_lazy_ms = -1;
module_param(rcu_tasks_lazy_ms, int, 0444);

/*
 * rcu_spawn_tasks_kthread() - 在启动期配置并创建 classic Tasks RCU GP 线程。
 *
 * 入参：无。返回：固定 0，表示 initcall 已提交初始化；generic 创建过程自行
 * 建立线程状态。函数在启动期、可睡眠上下文运行，&rcu_tasks 为静态全局对象，
 * 无所有权转移给调用者。
 *
 * 核心过程：设置 GP 睡眠/初始化扫描节拍；应用可选 lazy 毫秒参数；安装
 * pre-GP、逐任务扫描、扫描后处理、holdout 检查和 post-GP 回调表；指定
 * TASK_IDLE 等待状态；最后由 generic helper 创建并发布 kthread。
 */
static int __init rcu_spawn_tasks_kthread(void)
{
	/* 阶段 1：建立时间策略；lazy 参数只有显式非负时才覆盖默认值。 */
	rcu_tasks.gp_sleep = HZ / 10;
	rcu_tasks.init_fract = HZ / 10;
	if (rcu_tasks_lazy_ms >= 0)
		rcu_tasks.lazy_jiffies = msecs_to_jiffies(rcu_tasks_lazy_ms);
	/*
	 * 阶段 2：把 classic flavor 的各阶段实现装入 generic Tasks RCU 状态机。
	 * 发布 kthread 之前完成所有函数指针和等待状态初始化，避免线程启动后
	 * 观察到半初始化操作表。
	 */
	rcu_tasks.pregp_func = rcu_tasks_pregp_step;
	rcu_tasks.pertask_func = rcu_tasks_pertask;
	rcu_tasks.postscan_func = rcu_tasks_postscan;
	rcu_tasks.holdouts_func = check_all_holdout_tasks;
	rcu_tasks.postgp_func = rcu_tasks_postgp;
	rcu_tasks.wait_state = TASK_IDLE;
	/* 阶段 3：创建/唤醒 GP 线程；后续宽限期请求由该线程消费。 */
	rcu_spawn_tasks_kthread_generic(&rcu_tasks);
	return 0;
}

#if !defined(CONFIG_TINY_RCU)
/*
 * show_rcu_tasks_classic_gp_kthread() - 输出 classic Tasks RCU GP 线程状态。
 *
 * 入参：无。返回：无直接返回值。仅非 TINY_RCU 构建提供；把静态 &rcu_tasks
 * 借给 generic 诊断器并使用空 flavor 后缀，不修改队列 ownership，主要用于
 * stall/调试输出。
 */
void show_rcu_tasks_classic_gp_kthread(void)
{
	show_rcu_tasks_generic_gp_kthread(&rcu_tasks, "");
}
EXPORT_SYMBOL_GPL(show_rcu_tasks_classic_gp_kthread);

/*
 * rcu_tasks_torture_stats_print() - 输出 classic Tasks RCU 的 torture 统计。
 *
 * @tt、@tf：调用者提供的只读字符串前缀，generic 打印器把它们拼入日志，
 *            函数只借用、不保存也不释放；按调用契约应为有效 NUL 结尾字符串。
 * 返回：无直接返回值。仅非 TINY_RCU 构建存在；打印过程可能以 GFP_KERNEL
 * 临时分配 cpumask，因而应在可睡眠上下文调用，但不改变 GP/回调 ownership。
 */
void rcu_tasks_torture_stats_print(char *tt, char *tf)
{
	rcu_tasks_torture_stats_print_generic(&rcu_tasks, tt, tf, "");
}
EXPORT_SYMBOL_GPL(rcu_tasks_torture_stats_print);
#endif // !defined(CONFIG_TINY_RCU)

/*
 * get_rcu_tasks_gp_kthread() - 查询 classic Tasks RCU 当前 GP 线程。
 *
 * 入参：无。返回：rcu_tasks.kthread_ptr 的瞬时借用指针，尚未创建时可为 NULL。
 * 本函数不增加 task_struct 引用，也不稳定线程状态；调用者若要跨越并发销毁/
 * 替换边界长期使用，必须按上层协议另取引用。
 */
struct task_struct *get_rcu_tasks_gp_kthread(void)
{
	return rcu_tasks.kthread_ptr;
}
EXPORT_SYMBOL_GPL(get_rcu_tasks_gp_kthread);

/*
 * rcu_tasks_get_gp_data() - 导出 classic Tasks RCU 当前 GP 诊断快照。
 *
 * @flags：非空输出指针；classic flavor 没有额外状态位，返回时固定写 0。
 * @gp_seq：非空输出指针；返回当前 tasks_gp_seq 的编码序号快照。
 * 返回：无直接返回值。两个值只用于观测，读取不持锁、不启动宽限期，也不
 * 保证返回后序号不再推进。
 */
void rcu_tasks_get_gp_data(int *flags, unsigned long *gp_seq)
{
	*flags = 0;
	*gp_seq = rcu_seq_current(&rcu_tasks.tasks_gp_seq);
}
EXPORT_SYMBOL_GPL(rcu_tasks_get_gp_data);

/*
 * Protect against tasklist scan blind spot while the task is exiting and
 * may be removed from the tasklist.  Do this by adding the task to yet
 * another list.
 *
 * Note that the task will remove itself from this list, so there is no
 * need for get_task_struct(), except in the case where rcu_tasks_pertask()
 * adds it to the holdout list, in which case rcu_tasks_pertask() supplies
 * the needed get_task_struct().
 */
/*
 * exit_tasks_rcu_start() - 在 tasklist 摘除前把 current 发布到 per-CPU 退出列表。
 *
 * 入参/返回：无。仅 do_exit() 的 current 调用；不可睡眠。任务自己拥有
 * exit_list 节点，因此这里只借用 task，不增加引用；若 postscan 把它加入
 * holdout，pertask 会单独 get_task_struct()。
 *
 * 禁止抢占后记录 CPU 并取得该分片 raw lock，保证 CPU 选择与挂链一致。
 * 挂链后即使 exit_notify() 从全局 tasklist 摘除任务，Tasks RCU 仍能发现它。
 */
void exit_tasks_rcu_start(void)
{
	unsigned long flags;
	struct rcu_tasks_percpu *rtpcp;
	struct task_struct *t = current;

	WARN_ON_ONCE(!list_empty(&t->rcu_tasks_exit_list));
	preempt_disable();
	rtpcp = this_cpu_ptr(rcu_tasks.rtpcpu);
	t->rcu_tasks_exit_cpu = smp_processor_id();
	raw_spin_lock_irqsave_rcu_node(rtpcp, flags);
	WARN_ON_ONCE(!rtpcp->rtp_exit_list.next);
	list_add(&t->rcu_tasks_exit_list, &rtpcp->rtp_exit_list);
	raw_spin_unlock_irqrestore_rcu_node(rtpcp, flags);
	preempt_enable();
}

/*
 * Remove the task from the "yet another list" because do_exit() is now
 * non-preemptible, allowing synchronize_rcu() to wait beyond this point.
 */
/*
 * exit_tasks_rcu_finish() - 在 do_exit() 进入最终不可抢占区后摘除退出节点。
 *
 * 入参/返回：无。使用 start 记录的 CPU 找回原分片并在同一 raw lock 下摘链。
 * 此时后续区间已由普通 RCU 读者语义覆盖，postgp synchronize_rcu() 会等待
 * 到 TASK_DEAD 最终调度，因此两个窗口无缝交接。函数不释放 task 引用，
 * 因为 start 本身没有取得引用。
 */
void exit_tasks_rcu_finish(void)
{
	unsigned long flags;
	struct rcu_tasks_percpu *rtpcp;
	struct task_struct *t = current;

	WARN_ON_ONCE(list_empty(&t->rcu_tasks_exit_list));
	rtpcp = per_cpu_ptr(rcu_tasks.rtpcpu, t->rcu_tasks_exit_cpu);
	raw_spin_lock_irqsave_rcu_node(rtpcp, flags);
	list_del_init(&t->rcu_tasks_exit_list);
	raw_spin_unlock_irqrestore_rcu_node(rtpcp, flags);
}

#else /* #ifdef CONFIG_TASKS_RCU */
/*
 * 未启用 classic Tasks RCU 时退出盲区协议不存在，两个同签名 stub 无副作用，
 * 让通用 do_exit() 调用点无需条件编译。
 */
void exit_tasks_rcu_start(void) { }
void exit_tasks_rcu_finish(void) { }
#endif /* #else #ifdef CONFIG_TASKS_RCU */

#ifdef CONFIG_TASKS_RUDE_RCU

////////////////////////////////////////////////////////////////////////
//
// "Rude" variant of Tasks RCU, inspired by Steve Rostedt's
// trick of passing an empty function to schedule_on_each_cpu().
// This approach provides batching of concurrent calls to the synchronous
// synchronize_rcu_tasks_rude() API.  This invokes schedule_on_each_cpu()
// in order to send IPIs far and wide and induces otherwise unnecessary
// context switches on all online CPUs, whether idle or not.
//
// Callback handling is provided by the rcu_tasks_kthread() function.
//
// Ordering is provided by the scheduler's context-switch code.
/*
 * Rude flavor 借助 schedule_on_each_cpu() 向所有在线 CPU 排空 work：即使 CPU
 * idle，也强制经过调度器上下文切换，从而批量满足并发同步调用。回调仍由
 * 通用 GP kthread 处理，内存序来自调度器切换。收益是无需扫描 holdout，
 * 代价是全系统 IPI/调度扰动，因此只适合少量 tracing 场景。
 */

// Empty function to allow workqueues to force a context switch.
/*
 * rcu_tasks_be_rude() 是空 work 回调；@work 只作为 workqueue 载体。真正
 * 效果是 work 被每 CPU worker 执行时必然经历调度边界，无返回、无对象变化。
 */
static void rcu_tasks_be_rude(struct work_struct *work)
{
}

// Wait for one rude RCU-tasks grace period.
/*
 * rcu_tasks_rude_wait_gp() - 强制所有在线 CPU 各经历一次调度。
 * @rtp 静态借用；先按在线 CPU 数累计 IPI 统计，再同步
 * schedule_on_each_cpu()。无返回、可能睡眠且开销高；返回即本轮 Rude GP 完成。
 */
static void rcu_tasks_rude_wait_gp(struct rcu_tasks *rtp)
{
	rtp->n_ipis += cpumask_weight(cpu_online_mask);
	schedule_on_each_cpu(rcu_tasks_be_rude);
}

static void call_rcu_tasks_rude(struct rcu_head *rhp, rcu_callback_t func);
DEFINE_RCU_TASKS(rcu_tasks_rude, rcu_tasks_rude_wait_gp, call_rcu_tasks_rude,
		 "RCU Tasks Rude");

/*
 * call_rcu_tasks_rude() - Queue a callback rude task-based grace period
 * @rhp: structure to be used for queueing the RCU updates.
 * @func: actual callback function to be invoked after the grace period
 *
 * The callback function will be invoked some time after a full grace
 * period elapses, in other words after all currently executing RCU
 * read-side critical sections have completed. call_rcu_tasks_rude()
 * assumes that the read-side critical sections end at context switch,
 * cond_resched_tasks_rcu_qs(), or transition to usermode execution (as
 * usermode execution is schedulable). As such, there are no read-side
 * primitives analogous to rcu_read_lock() and rcu_read_unlock() because
 * this primitive is intended to determine that all tasks have passed
 * through a safe state, not so much for data-structure synchronization.
 *
 * See the description of call_rcu() for more detailed information on
 * memory ordering guarantees.
 *
 * This is no longer exported, and is instead reserved for use by
 * synchronize_rcu_tasks_rude().
 */
/*
 * call_rcu_tasks_rude() - 在 Rude Tasks GP 后登记内部回调。
 *
 * @rhp 入队后交给 RCU，@func 为宽限期后回调；无返回、不等待。Rude 把任一
 * 上下文切换、cond_resched 或用户态视为安全状态，没有显式读锁。该函数
 * 不再导出，只供 synchronize_rcu_tasks_rude() 通过通用同步等待框架使用。
 */
static void call_rcu_tasks_rude(struct rcu_head *rhp, rcu_callback_t func)
{
	call_rcu_tasks_generic(rhp, func, &rcu_tasks_rude);
}

/**
 * synchronize_rcu_tasks_rude - wait for a rude rcu-tasks grace period
 *
 * Control will return to the caller some time after a rude rcu-tasks
 * grace period has elapsed, in other words after all currently
 * executing rcu-tasks read-side critical sections have elapsed.  These
 * read-side critical sections are delimited by calls to schedule(),
 * cond_resched_tasks_rcu_qs(), userspace execution (which is a schedulable
 * context), and (in theory, anyway) cond_resched().
 *
 * This is a very specialized primitive, intended only for a few uses in
 * tracing and other situations requiring manipulation of function preambles
 * and profiling hooks.  The synchronize_rcu_tasks_rude() function is not
 * (yet) intended for heavy use from multiple CPUs.
 *
 * See the description of synchronize_rcu() for more detailed information
 * on memory ordering guarantees.
 */
/*
 * synchronize_rcu_tasks_rude() - 同步等待 Rude Tasks RCU GP。
 *
 * 入参/返回：无。允许实现时在进程上下文睡眠并触发全 CPU 调度；返回表示
 * 调用前执行区间已越过调度安全点，但不自动释放对象。ARCH_WANTS_NO_INSTR
 * 且未强制 Rude 时为空操作，这是架构选择而非实际完成一次 GP。
 * 该接口面向少量 tracing/profiling 更新，不适合高频多 CPU 调用。
 */
void synchronize_rcu_tasks_rude(void)
{
	if (!IS_ENABLED(CONFIG_ARCH_WANTS_NO_INSTR) || IS_ENABLED(CONFIG_FORCE_TASKS_RUDE_RCU))
		synchronize_rcu_tasks_generic(&rcu_tasks_rude);
}
EXPORT_SYMBOL_GPL(synchronize_rcu_tasks_rude);

/*
 * rcu_spawn_tasks_rude_kthread() - 配置并创建 Rude GP 线程。
 * 无参数，启动期可睡眠；设置 100ms 防紧循环间隔后调用 generic 创建器。
 * 返回固定 0，创建失败由 generic 告警。
 */
static int __init rcu_spawn_tasks_rude_kthread(void)
{
	rcu_tasks_rude.gp_sleep = HZ / 10;
	rcu_spawn_tasks_kthread_generic(&rcu_tasks_rude);
	return 0;
}

#if !defined(CONFIG_TINY_RCU)
/*
 * show_rcu_tasks_rude_gp_kthread() - 输出 Rude flavor GP 摘要。
 * 无参数/返回，仅非 TINY；借用静态控制器，不改变状态。
 */
void show_rcu_tasks_rude_gp_kthread(void)
{
	show_rcu_tasks_generic_gp_kthread(&rcu_tasks_rude, "");
}
EXPORT_SYMBOL_GPL(show_rcu_tasks_rude_gp_kthread);

/*
 * rcu_tasks_rude_torture_stats_print() - 打印 Rude torture 统计。
 * @tt/@tf 是只读日志前缀；可能 GFP_KERNEL 分配临时 cpumask，需可睡眠上下文。
 * 无返回、无 ownership 变化。
 */
void rcu_tasks_rude_torture_stats_print(char *tt, char *tf)
{
	rcu_tasks_torture_stats_print_generic(&rcu_tasks_rude, tt, tf, "");
}
EXPORT_SYMBOL_GPL(rcu_tasks_rude_torture_stats_print);
#endif // !defined(CONFIG_TINY_RCU)

/*
 * get_rcu_tasks_rude_gp_kthread() 返回 GP 线程瞬时借用指针，未创建时可为 NULL；
 * 不增加 task 引用，调用者跨并发边界使用时须自行稳定。
 */
struct task_struct *get_rcu_tasks_rude_gp_kthread(void)
{
	return rcu_tasks_rude.kthread_ptr;
}
EXPORT_SYMBOL_GPL(get_rcu_tasks_rude_gp_kthread);

/*
 * rcu_tasks_rude_get_gp_data() - 导出 Rude GP 诊断快照。
 * @flags 非空输出且固定写 0；@gp_seq 非空输出当前编码序号。无返回，不启动
 * GP，也不保证快照后序号不变。
 */
void rcu_tasks_rude_get_gp_data(int *flags, unsigned long *gp_seq)
{
	*flags = 0;
	*gp_seq = rcu_seq_current(&rcu_tasks_rude.tasks_gp_seq);
}
EXPORT_SYMBOL_GPL(rcu_tasks_rude_get_gp_data);

#endif /* #ifdef CONFIG_TASKS_RUDE_RCU */

#ifndef CONFIG_TINY_RCU
/*
 * show_rcu_tasks_gp_kthreads() 汇总 classic 与 Rude GP 线程状态。无参数/返回，
 * 仅诊断、不稳定任何线程引用；仅非 TINY 构建。
 */
void show_rcu_tasks_gp_kthreads(void)
{
	show_rcu_tasks_classic_gp_kthread();
	show_rcu_tasks_rude_gp_kthread();
}
#endif /* #ifndef CONFIG_TINY_RCU */

#ifdef CONFIG_PROVE_RCU
/*
 * 自测试描述符把一个 rcu_head 与测试名称、尚未回调标志和启动 jiffies 绑定。
 * tests[] 静态存续；notrun 在登记前表示该配置是否应测试，回调清零，验证器
 * 据此区分通过、仍等待和超时。
 */
struct rcu_tasks_test_desc {
	struct rcu_head rh;
	const char *name;
	bool notrun;
	unsigned long runstart;
};

static struct rcu_tasks_test_desc tests[] = {
	{
		.name = "call_rcu_tasks()",
		/* If not defined, the test is skipped. */
		/* 未编译相应 flavor 时测试视为已跳过；编译时初值为“尚未回调”。 */
		.notrun = IS_ENABLED(CONFIG_TASKS_RCU),
	},
	{
		.name = "call_rcu_tasks_trace()",
		/* If not defined, the test is skipped. */
		/* Trace flavor 同样只在配置存在时等待回调证明。 */
		.notrun = IS_ENABLED(CONFIG_TASKS_TRACE_RCU)
	}
};

#if defined(CONFIG_TASKS_RCU) || defined(CONFIG_TASKS_TRACE_RCU)
/*
 * test_rcu_tasks_callback() - 标记一个异步自测试回调已经执行。
 * @rhp 嵌入静态描述符；container_of 恢复对象，打印名称并清 notrun。
 * 无返回、不释放静态对象。
 */
static void test_rcu_tasks_callback(struct rcu_head *rhp)
{
	struct rcu_tasks_test_desc *rttd =
		container_of(rhp, struct rcu_tasks_test_desc, rh);

	pr_info("Callback from %s invoked.\n", rttd->name);

	rttd->notrun = false;
}
#endif // #if defined(CONFIG_TASKS_RCU) || defined(CONFIG_TASKS_TRACE_RCU)

/*
 * rcu_tasks_initiate_self_tests() - 启动已配置 flavor 的启动期自测试。
 *
 * 无参数/返回，可能同步等待。classic/trace 记录起始时间、先验证同步 API，
 * 再登记异步回调；Rude 验证同步 API。异步结果由稍后的 delayed work 检查。
 */
static void rcu_tasks_initiate_self_tests(void)
{
#ifdef CONFIG_TASKS_RCU
	pr_info("Running RCU Tasks wait API self tests\n");
	tests[0].runstart = jiffies;
	synchronize_rcu_tasks();
	call_rcu_tasks(&tests[0].rh, test_rcu_tasks_callback);
#endif

#ifdef CONFIG_TASKS_RUDE_RCU
	pr_info("Running RCU Tasks Rude wait API self tests\n");
	synchronize_rcu_tasks_rude();
#endif

#ifdef CONFIG_TASKS_TRACE_RCU
	pr_info("Running RCU Tasks Trace wait API self tests\n");
	tests[1].runstart = jiffies;
	synchronize_rcu_tasks_trace();
	call_rcu_tasks_trace(&tests[1].rh, test_rcu_tasks_callback);
#endif
}

/*
 * Return:  0 - test passed
 *	    1 - test failed, but have not timed out yet
 *	   -1 - test failed and timed out
 */
/*
 * rcu_tasks_verify_self_tests() - 检查所有异步自测试状态。
 *
 * 无参数。返回 0 表示全部回调完成，1 表示仍等待但未超时，-1 表示至少一项
 * 超时并触发 WARN。启动等待上限取 stall_timeout 与 30 秒默认中的较小有效
 * 值，避免用户把常规 stall 参数设得过大而拖延启动失败发现。
 */
static int rcu_tasks_verify_self_tests(void)
{
	int ret = 0;
	int i;
	unsigned long bst = rcu_task_stall_timeout;

	if (bst <= 0 || bst > RCU_TASK_BOOT_STALL_TIMEOUT)
		bst = RCU_TASK_BOOT_STALL_TIMEOUT;
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		while (tests[i].notrun) {		// still hanging.
			/* 对应异步回调仍未执行，当前测试尚在挂起。 */
			/* while 结构只执行一次判断；1 由 delayed work 在下一秒重试。 */
			if (time_after(jiffies, tests[i].runstart + bst)) {
				pr_err("%s has failed boot-time tests.\n", tests[i].name);
				ret = -1;
				break;
			}
			ret = 1;
			break;
		}
	}
	WARN_ON(ret < 0);

	return ret;
}

/*
 * Repeat the rcu_tasks_verify_self_tests() call once every second until the
 * test passes or has timed out.
 */
/*
 * delayed work 每秒调用验证器：通过或超时即停止，返回 1 才重新排队。
 * work 参数未使用，静态 delayed_work 保证重排期间对象持续存活。
 */
static struct delayed_work rcu_tasks_verify_work;
static void rcu_tasks_verify_work_fn(struct work_struct *work __maybe_unused)
{
	int ret = rcu_tasks_verify_self_tests();

	if (ret <= 0)
		return;

	/* Test fails but not timed out yet, reschedule another check */
	/* 测试尚未通过但未超时，一秒后再检查，避免启动线程忙等。 */
	schedule_delayed_work(&rcu_tasks_verify_work, HZ);
}

/*
 * rcu_tasks_verify_schedule_work() - late_initcall 启动验证轮询。
 * 初始化静态 delayed_work 后立即执行第一次检查；返回固定 0。
 */
static int rcu_tasks_verify_schedule_work(void)
{
	INIT_DELAYED_WORK(&rcu_tasks_verify_work, rcu_tasks_verify_work_fn);
	rcu_tasks_verify_work_fn(NULL);
	return 0;
}
late_initcall(rcu_tasks_verify_schedule_work);
#else /* #ifdef CONFIG_PROVE_RCU */
/* 未启用 PROVE_RCU 时自测试入口为空，不改变启动流程。 */
static void rcu_tasks_initiate_self_tests(void) { }
#endif /* #else #ifdef CONFIG_PROVE_RCU */

/*
 * tasks_cblist_init_generic() - 在 SMP/入队发布前初始化所有已配置 flavor 队列。
 *
 * 入参/返回：无。要求关中断且当前最多一个在线 CPU；分别调用 generic
 * 初始化器。违反前置条件告警，因为并发入队会看见半初始化 segcblist。
 */
void __init tasks_cblist_init_generic(void)
{
	lockdep_assert_irqs_disabled();
	WARN_ON(num_online_cpus() > 1);

#ifdef CONFIG_TASKS_RCU
	cblist_init_generic(&rcu_tasks);
#endif

#ifdef CONFIG_TASKS_RUDE_RCU
	cblist_init_generic(&rcu_tasks_rude);
#endif
}

/*
 * rcu_init_tasks_generic() - core_initcall 创建 GP 线程并启动自测试。
 *
 * 无参数；按配置创建 classic/Rude kthread，随后运行自测试。返回固定 0；
 * 单个线程创建失败已在 generic 创建器中告警并保留退化状态。
 */
static int __init rcu_init_tasks_generic(void)
{
#ifdef CONFIG_TASKS_RCU
	rcu_spawn_tasks_kthread();
#endif

#ifdef CONFIG_TASKS_RUDE_RCU
	rcu_spawn_tasks_rude_kthread();
#endif

	// Run the self-tests.
	/* 此时 flavor 对象/队列已初始化，异步测试可以安全登记回调。 */
	rcu_tasks_initiate_self_tests();

	return 0;
}
core_initcall(rcu_init_tasks_generic);

#else /* #ifdef CONFIG_TASKS_RCU_GENERIC */
/* 完全未启用 generic Tasks RCU 时，启动参数诊断入口为空。 */
static inline void rcu_tasks_bootup_oddness(void) {}
#endif /* #else #ifdef CONFIG_TASKS_RCU_GENERIC */

#ifdef CONFIG_TASKS_TRACE_RCU

////////////////////////////////////////////////////////////////////////
//
// Tracing variant of Tasks RCU.  This variant is designed to be used
// to protect tracing hooks, including those of BPF.  This variant
// is implemented via a straightforward mapping onto SRCU-fast.
/*
 * Trace flavor 专门保护 tracing/BPF hook，直接映射到 SRCU-fast：读者可显式
 * 嵌套并按 SRCU 规则睡眠，更新者等待对应 SRCU GP。这里仅定义并导出静态
 * srcu_struct，具体接口由 SRCU/rcupdate_trace 代码提供。
 */

DEFINE_SRCU_FAST(rcu_tasks_trace_srcu_struct);
EXPORT_SYMBOL_GPL(rcu_tasks_trace_srcu_struct);

#endif /* #else #ifdef CONFIG_TASKS_TRACE_RCU */
