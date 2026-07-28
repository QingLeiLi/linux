// SPDX-License-Identifier: GPL-2.0-only
/*
 * 中文学习注释：OpenAI GPT-5 Codex（2026-07-27）。
 *
 * 文件地图
 * ========
 *
 * 本文件实现内核通用异步执行框架。调用者把 `work_struct` 交给某个
 * `workqueue_struct`；workqueue 根据 per-CPU/unbound、优先级、并发上限和
 * affinity 选择 `pool_workqueue`，再把 work 发布到共享 `worker_pool`。
 * kworker 从 pool 领取 work，在进程上下文调用 `work->func()`。主路径为：
 *
 *   queue_work()/queue_delayed_work()
 *       → 选择 pwq 和 pool → 设置 PENDING 并入队
 *       → 唤醒/创建 worker → process_one_work()
 *       → 回调完成、并发名额归还、flush/cancel 观察完成
 *
 * `work_struct` 是一次可排队执行的状态载体，不拥有独立线程；`workqueue_struct`
 * 是调用者看到的策略对象；`pool_workqueue` 把策略对象连接到实际执行池；
 * `worker_pool` 拥有待执行链表和 kworker；`worker` 代表一个执行线程。
 *
 * 主要并发协议有四条：
 * 1. work->data 同时编码 PENDING、pwq 指针或 off-queue pool id。PENDING 是排队
 *    ownership 的原子仲裁位，取得它的路径才有权移动或取消该 work。
 * 2. pool->lock 保护 pool 工作链表、worker 状态及 pwq 的执行计数；它是热路径
 *    raw spinlock，持锁期间不能睡眠。wq->mutex 管理 flush、drain 和 pwq 列表；
 *    wq_pool_mutex 管理全局 pool/workqueue 拓扑，二者属于可睡眠慢路径。
 * 3. flush 使用 color 给“本轮 flush 之前进入”的 work 建立代际边界，而不是停止
 *    新 work 入队；barrier work 把对某个正在执行/等待 work 的完成观察串起来。
 * 4. unbound pwq/pool 和全局 workqueue 列表允许 RCU 读。摘除后必须经过宽限期
 *    才释放；引用计数保证对象存活，但不替代字段锁。
 *
 * 方案收益是大量子系统共享少量动态管理的线程，同时保持 per-CPU 局部性、并发
 * 限流、CPU 热插拔与内存回收救援能力；代价是 work->data 编码、双层 pwq/pool
 * 映射、flush 代际和多把锁共同形成较复杂的生命周期协议。本文件不定义各调用者
 * 回调的业务逻辑，也不保证回调执行时仍在 queue_work() 的调用 CPU 上；具体 API
 * 契约还应结合 Documentation/core-api/workqueue.rst 阅读。
 */
/*
 * kernel/workqueue.c - generic async execution with shared worker pool
 *
 * Copyright (C) 2002		Ingo Molnar
 *
 *   Derived from the taskqueue/keventd code by:
 *     David Woodhouse <dwmw2@infradead.org>
 *     Andrew Morton
 *     Kai Petzke <wpp@marie.physik.tu-berlin.de>
 *     Theodore Ts'o <tytso@mit.edu>
 *
 * Made to use alloc_percpu by Christoph Lameter.
 *
 * Copyright (C) 2010		SUSE Linux Products GmbH
 * Copyright (C) 2010		Tejun Heo <tj@kernel.org>
 *
 * This is the generic async execution mechanism.  Work items as are
 * executed in process context.  The worker pool is shared and
 * automatically managed.  There are two worker pools for each CPU (one for
 * normal work items and the other for high priority ones) and some extra
 * pools for workqueues which are not bound to any specific CPU - the
 * number of these backing pools is dynamic.
 *
 * Please read Documentation/core-api/workqueue.rst for details.
 */
/*
 * 本文件是“共享 worker pool 上的通用异步执行”实现，最初由 taskqueue/keventd
 * 演进而来。work 回调在进程上下文执行，因此通常可以睡眠；实际限制仍取决于回调
 * 自己持有的锁和所用 API。每个 CPU 有普通与高优先级两个标准池，unbound
 * workqueue 则按属性动态共享额外 pool。完整用户契约见
 * Documentation/core-api/workqueue.rst；本文件关注这些契约如何通过状态位、锁、
 * worker 管理和 pool 拓扑落地。
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/signal.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/kthread.h>
#include <linux/hardirq.h>
#include <linux/mempolicy.h>
#include <linux/freezer.h>
#include <linux/debug_locks.h>
#include <linux/device/devres.h>
#include <linux/lockdep.h>
#include <linux/idr.h>
#include <linux/jhash.h>
#include <linux/hashtable.h>
#include <linux/rculist.h>
#include <linux/nodemask.h>
#include <linux/moduleparam.h>
#include <linux/uaccess.h>
#include <linux/sched/isolation.h>
#include <linux/sched/debug.h>
#include <linux/nmi.h>
#include <linux/kvm_para.h>
#include <linux/delay.h>
#include <linux/irq_work.h>

#include "workqueue_internal.h"

enum worker_pool_flags {
	/*
	 * worker_pool flags
	 *
	 * A bound pool is either associated or disassociated with its CPU.
	 * While associated (!DISASSOCIATED), all workers are bound to the
	 * CPU and none has %WORKER_UNBOUND set and concurrency management
	 * is in effect.
	 *
	 * While DISASSOCIATED, the cpu may be offline and all workers have
	 * %WORKER_UNBOUND set and concurrency management disabled, and may
	 * be executing on any CPU.  The pool behaves as an unbound one.
	 *
	 * Note that DISASSOCIATED should be flipped only while holding
	 * wq_pool_attach_mutex to avoid changing binding state while
	 * worker_attach_to_pool() is in progress.
	 *
	 * As there can only be one concurrent BH execution context per CPU, a
	 * BH pool is per-CPU and always DISASSOCIATED.
	 */
	/*
	 * pool 状态位描述“这个执行池当前如何被调度和管理”：
	 * - POOL_BH：回调在 softirq/BH 执行器中跑，不创建普通 kworker；
	 * - MANAGER_ACTIVE：已有 worker 承担创建/裁剪线程的 manager 角色；
	 * - DISASSOCIATED：pool 不再与指定 CPU 建立强绑定，通常出现在 CPU 离线期；
	 * - BH_DRAINING：CPU 离线后仍在排空该 CPU 的 BH work。
	 *
	 * bound pool 正常在线时，worker 固定在 pool->cpu，`nr_running` 并发管理生效。
	 * DISASSOCIATED 后 worker 可在任意 CPU 执行，并发管理关闭以保证离线期间仍能
	 * 前进。绑定状态只能在 wq_pool_attach_mutex 下翻转，否则 attach 中的 worker
	 * 可能观察到一半旧、一半新的 affinity 规则。每 CPU 同时最多一个 BH 上下文，
	 * 所以 BH pool 虽按 CPU 建立，却始终按 DISASSOCIATED 的并发规则处理。
	 */
	POOL_BH			= 1 << 0,	/* is a BH pool */
	POOL_MANAGER_ACTIVE	= 1 << 1,	/* being managed */
	POOL_DISASSOCIATED	= 1 << 2,	/* cpu can't serve workers */
	POOL_BH_DRAINING	= 1 << 3,	/* draining after CPU offline */
};

enum worker_flags {
	/* worker flags */
	/*
	 * worker 状态位由 pool->lock 协调。DIE 要求线程退出；IDLE 表示已进入 idle_list；
	 * PREP 表示尚未正式领取 work；CPU_INTENSIVE 让长时间占 CPU 的回调退出普通并发
	 * 计数；UNBOUND/REBOUND 描述 CPU 绑定过渡。WORKER_NOT_RUNNING 汇总所有“不应
	 * 计入 pool->nr_running”的状态，worker_set_flags()/worker_clr_flags() 必须在
	 * 状态变化时同步维护计数，否则 manager 会误判是否需要唤醒或新建 worker。
	 */
	WORKER_DIE		= 1 << 1,	/* die die die */
	WORKER_IDLE		= 1 << 2,	/* is idle */
	WORKER_PREP		= 1 << 3,	/* preparing to run works */
	WORKER_CPU_INTENSIVE	= 1 << 6,	/* cpu intensive */
	WORKER_UNBOUND		= 1 << 7,	/* worker is unbound */
	WORKER_REBOUND		= 1 << 8,	/* worker was rebound */

	WORKER_NOT_RUNNING	= WORKER_PREP | WORKER_CPU_INTENSIVE |
				  WORKER_UNBOUND | WORKER_REBOUND,
};

enum work_cancel_flags {
	/*
	 * cancel 内部标志区分 delayed_work（还需与 timer 争夺 PENDING）以及 disable
	 * 操作（成功取消后增加 off-queue disable 深度）。它们只描述取消协议，不会写入
	 * workqueue 的公开 WQ_* 属性。
	 */
	WORK_CANCEL_DELAYED	= 1 << 0,	/* canceling a delayed_work */
	WORK_CANCEL_DISABLE	= 1 << 1,	/* canceling to disable */
};

enum wq_internal_consts {
	/*
	 * 这些常量给 worker 数量、hash、超时和 rescuer 批量设置边界。idle worker
	 * 最多约为 busy worker 的四分之一并保留五分钟，避免突发负载后频繁创建线程；
	 * MAYDAY 在普通 worker 因内存压力等原因无法创建时周期请求 rescuer；创建失败
	 * 后冷却一秒，避免持续分配失败形成忙循环。
	 */
	NR_STD_WORKER_POOLS	= 2,		/* # standard pools per cpu */

	UNBOUND_POOL_HASH_ORDER	= 6,		/* hashed by pool->attrs */
	BUSY_WORKER_HASH_ORDER	= 6,		/* 64 pointers */

	MAX_IDLE_WORKERS_RATIO	= 4,		/* 1/4 of busy can be idle */
	IDLE_WORKER_TIMEOUT	= 300 * HZ,	/* keep idle ones for 5 mins */

	MAYDAY_INITIAL_TIMEOUT  = HZ / 100 >= 2 ? HZ / 100 : 2,
						/* call for help after 10ms
						   (min two ticks) */
	MAYDAY_INTERVAL		= HZ / 10,	/* and then every 100ms */
	CREATE_COOLDOWN		= HZ,		/* time to breath after fail */

	RESCUER_BATCH		= 16,		/* process items per turn */

	/*
	 * Rescue workers are used only on emergencies and shared by
	 * all cpus.  Give MIN_NICE.
	 */
	RESCUER_NICE_LEVEL	= MIN_NICE,
	HIGHPRI_NICE_LEVEL	= MIN_NICE,

	WQ_NAME_LEN		= 32,
	WORKER_ID_LEN		= 10 + WQ_NAME_LEN, /* "kworker/R-" + WQ_NAME_LEN */
};

/* Layout of shards within one LLC pod */
struct llc_shard_layout {
	int nr_large_shards;	/* number of large shards (cores_per_shard + 1) */
	int cores_per_shard;	/* base number of cores per default shard */
	int nr_shards;		/* total number of shards */
	/* nr_default shards = (nr_shards - nr_large_shards) */
};
/*
 * `llc_shard_layout` 是把一个 LLC pod 切成 cache shard 的计算结果。不能整除时，
 * 前 `nr_large_shards` 个 shard 比基础 `cores_per_shard` 多一个 core；总 shard
 * 数由 `nr_shards` 给出。它只在拓扑初始化/计算期间使用，不代表运行期可热变对象。
 */

/*
 * We don't want to trap softirq for too long. See MAX_SOFTIRQ_TIME and
 * MAX_SOFTIRQ_RESTART in kernel/softirq.c. These are macros because
 * msecs_to_jiffies() can't be an initializer.
 */
#define BH_WORKER_JIFFIES	msecs_to_jiffies(2)
#define BH_WORKER_RESTARTS	10
/*
 * BH worker 每轮最多运行约 2ms 或经历 10 次 restart，随后把执行权还给 softirq
 * 框架，避免 workqueue 长时间独占 softirq。宏形式是因为静态初始化阶段不能调用
 * msecs_to_jiffies()；这两个限制是公平性预算，不是每个 work 的超时。
 */

/*
 * Structure fields follow one of the following exclusion rules.
 *
 * I: Modifiable by initialization/destruction paths and read-only for
 *    everyone else.
 *
 * P: Preemption protected.  Disabling preemption is enough and should
 *    only be modified and accessed from the local cpu.
 *
 * L: pool->lock protected.  Access with pool->lock held.
 *
 * LN: pool->lock and wq_node_nr_active->lock protected for writes. Either for
 *     reads.
 *
 * K: Only modified by worker while holding pool->lock. Can be safely read by
 *    self, while holding pool->lock or from IRQ context if %current is the
 *    kworker.
 *
 * S: Only modified by worker self.
 *
 * A: wq_pool_attach_mutex protected.
 *
 * PL: wq_pool_mutex protected.
 *
 * PR: wq_pool_mutex protected for writes.  RCU protected for reads.
 *
 * PW: wq_pool_mutex and wq->mutex protected for writes.  Either for reads.
 *
 * PWR: wq_pool_mutex and wq->mutex protected for writes.  Either or
 *      RCU for reads.
 *
 * WQ: wq->mutex protected.
 *
 * WR: wq->mutex protected for writes.  RCU protected for reads.
 *
 * WO: wq->mutex protected for writes. Updated with WRITE_ONCE() and can be read
 *     with READ_ONCE() without locking.
 *
 * MD: wq_mayday_lock protected.
 *
 * WD: Used internally by the watchdog.
 */
/*
 * 上述字母是本文件字段旁的同步契约，而不是普通描述：
 * I 只在构造/销毁期写；P 依赖本 CPU 且禁抢占；L 由 pool->lock 保护；LN 写入需
 * 同时持 pool->lock 与 node-active lock；K/S 分别限定 kworker 自身及其持锁访问；
 * A、PL、WQ 分别对应 attach mutex、全局 pool mutex、单个 wq mutex；带 R 的规则
 * 允许 RCU 读；WO 用 WRITE_ONCE/READ_ONCE 发布单字段快照；MD 属于 mayday lock；
 * WD 只由 watchdog 使用。阅读字段时必须同时考虑“对象寿命由谁保证”和“字段值
 * 由谁稳定”：RCU/引用负责前者，锁或单次访问协议负责后者。
 */

/* struct worker is defined in workqueue_internal.h */

struct worker_pool {
	raw_spinlock_t		lock;		/* the pool lock */
	int			cpu;		/* I: the associated cpu */
	int			node;		/* I: the associated node ID */
	int			id;		/* I: pool ID */
	unsigned int		flags;		/* L: flags */

	unsigned long		last_progress_ts;	/* L: last forward progress timestamp */
	bool			cpu_stall;	/* WD: stalled cpu bound pool */

	/*
	 * The counter is incremented in a process context on the associated CPU
	 * w/ preemption disabled, and decremented or reset in the same context
	 * but w/ pool->lock held. The readers grab pool->lock and are
	 * guaranteed to see if the counter reached zero.
	 */
	int			nr_running;

	struct list_head	worklist;	/* L: list of pending works */

	int			nr_workers;	/* L: total number of workers */
	int			nr_idle;	/* L: currently idle workers */

	struct list_head	idle_list;	/* L: list of idle workers */
	struct timer_list	idle_timer;	/* L: worker idle timeout */
	struct work_struct      idle_cull_work; /* L: worker idle cleanup */

	struct timer_list	mayday_timer;	  /* L: SOS timer for workers */

	/* a workers is either on busy_hash or idle_list, or the manager */
	DECLARE_HASHTABLE(busy_hash, BUSY_WORKER_HASH_ORDER);
						/* L: hash of busy workers */

	struct worker		*manager;	/* L: purely informational */
	struct list_head	workers;	/* A: attached workers */

	struct ida		worker_ida;	/* worker IDs for task name */

	struct workqueue_attrs	*attrs;		/* I: worker attributes */
	struct hlist_node	hash_node;	/* PL: unbound_pool_hash node */
	int			refcnt;		/* PL: refcnt for unbound pools */
#ifdef CONFIG_PREEMPT_RT
	spinlock_t		cb_lock;	/* BH worker cancel lock */
#endif
	/*
	 * Destruction of pool is RCU protected to allow dereferences
	 * from get_work_pool().
	 */
	struct rcu_head		rcu;
};
/*
 * `worker_pool` 是真正调度 kworker 的执行域。
 *
 * 生命周期：per-CPU/BH 标准池为静态对象；unbound pool 由属性查找或创建，挂入
 * IDR/hash 后被 pwq 引用，refcnt 归零时先从全局索引摘除，再经 RCU 释放。
 * `lock` 保护热路径：worklist、nr_running、worker 数量/idle 状态、busy_hash、
 * manager 和定时器决策。`workers` 的 attach/detach 另由 wq_pool_attach_mutex
 * 串行化；attrs 在发布后只读。
 *
 * `nr_running` 不是线程总数，而是关联 CPU 上当前计入并发管理的 worker 数。
 * `nr_workers/nr_idle/idle_list` 共同决定是否需要创建、唤醒或裁剪线程。
 * busy_hash 用 work 地址快速找到正在执行它的 worker；一个 worker 在 busy_hash、
 * idle_list 或 manager 角色中三选一。mayday_timer 在无法及时造出 worker 时通知
 * 带 WQ_MEM_RECLAIM 的 workqueue 使用 rescuer。`rcu` 只延迟最终释放，不冻结字段。
 */

/*
 * Per-pool_workqueue statistics. These can be monitored using
 * tools/workqueue/wq_monitor.py.
 */
enum pool_workqueue_stats {
	PWQ_STAT_STARTED,	/* work items started execution */
	PWQ_STAT_COMPLETED,	/* work items completed execution */
	PWQ_STAT_CPU_TIME,	/* total CPU time consumed */
	PWQ_STAT_CPU_INTENSIVE,	/* wq_cpu_intensive_thresh_us violations */
	PWQ_STAT_CM_WAKEUP,	/* concurrency-management worker wakeups */
	PWQ_STAT_REPATRIATED,	/* unbound workers brought back into scope */
	PWQ_STAT_MAYDAY,	/* maydays to rescuer */
	PWQ_STAT_RESCUED,	/* linked work items executed by rescuer */

	PWQ_NR_STATS,
};
/*
 * 每个 pwq 的统计从“开始/完成次数、CPU 时间、CPU-intensive 违规、并发管理唤醒、
 * unbound worker 归位、mayday、rescuer 执行”观察运行行为。它们用于监控而不是
 * 正确性判定；读者可用 tools/workqueue/wq_monitor.py 聚合这些计数。
 */

/*
 * The per-pool workqueue.  While queued, bits below WORK_PWQ_SHIFT
 * of work_struct->data are used for flags and the remaining high bits
 * point to the pwq; thus, pwqs need to be aligned at two's power of the
 * number of flag bits.
 */
struct pool_workqueue {
	struct worker_pool	*pool;		/* I: the associated pool */
	struct workqueue_struct *wq;		/* I: the owning workqueue */
	int			work_color;	/* L: current color */
	int			flush_color;	/* L: flushing color */
	int			refcnt;		/* L: reference count */
	int			nr_in_flight[WORK_NR_COLORS];
						/* L: nr of in_flight works */
	bool			plugged;	/* L: execution suspended */

	/*
	 * nr_active management and WORK_STRUCT_INACTIVE:
	 *
	 * When pwq->nr_active >= max_active, new work item is queued to
	 * pwq->inactive_works instead of pool->worklist and marked with
	 * WORK_STRUCT_INACTIVE.
	 *
	 * All work items marked with WORK_STRUCT_INACTIVE do not participate in
	 * nr_active and all work items in pwq->inactive_works are marked with
	 * WORK_STRUCT_INACTIVE. But not all WORK_STRUCT_INACTIVE work items are
	 * in pwq->inactive_works. Some of them are ready to run in
	 * pool->worklist or worker->scheduled. Those work itmes are only struct
	 * wq_barrier which is used for flush_work() and should not participate
	 * in nr_active. For non-barrier work item, it is marked with
	 * WORK_STRUCT_INACTIVE iff it is in pwq->inactive_works.
	 */
	int			nr_active;	/* L: nr of active works */
	struct list_head	inactive_works;	/* L: inactive works */
	struct list_head	pending_node;	/* LN: node on wq_node_nr_active->pending_pwqs */
	struct list_head	pwqs_node;	/* WR: node on wq->pwqs */
	struct list_head	mayday_node;	/* MD: node on wq->maydays */
	struct work_struct	mayday_cursor;	/* L: cursor on pool->worklist */

	u64			stats[PWQ_NR_STATS];

	/*
	 * Release of unbound pwq is punted to a kthread_worker. See put_pwq()
	 * and pwq_release_workfn() for details. pool_workqueue itself is also
	 * RCU protected so that the first pwq can be determined without
	 * grabbing wq->mutex.
	 */
	struct kthread_work	release_work;
	struct rcu_head		rcu;
} __aligned(1 << WORK_STRUCT_PWQ_SHIFT);
/*
 * `pool_workqueue`（pwq）是一个 workqueue 策略对象到一个 worker_pool 的连接。
 *
 * `pool/wq` 在初始化后固定；work_color/flush_color 和每个 color 的
 * nr_in_flight 实现 flush 代际；refcnt 覆盖基础关联引用以及仍引用该 pwq 的 work。
 * nr_active/inactive_works 实现 max_active：未获执行名额的普通 work 留在 inactive
 * 链并带 INACTIVE 位。barrier 即使已移到 pool/worker 链仍保留 INACTIVE，因为它
 * 只观察完成顺序，不应消耗调用者的并发额度。
 *
 * unbound workqueue 的 pwq 可能随 affinity 重配而替换。旧 pwq 从 wq 列表摘除后，
 * 仍可能被 work->data 或 RCU reader 看见，因此释放被转交专用 kthread_worker，
 * 最后再经 RCU 回收。该结构按 `1 << WORK_STRUCT_PWQ_SHIFT` 对齐，使低地址位可安全
 * 借给 work->data 标志，任何改变该对齐的修改都会破坏编码协议。
 */

/*
 * Structure used to wait for workqueue flush.
 */
struct wq_flusher {
	struct list_head	list;		/* WQ: list of flushers */
	int			flush_color;	/* WQ: flush color waiting for */
	struct completion	done;		/* flush completion */
};
/*
 * 一个 `wq_flusher` 表示一次 flush 等待者：flush_color 指定它要等完的代际，
 * list 把并发 flusher 串成队列，done 在该代际所有 pwq 的 in-flight 计数归零时
 * 完成。对象通常位于调用者栈上，所以返回前必须已从队列摘除且不再被异步路径引用。
 */

struct wq_device;

/*
 * Unlike in a per-cpu workqueue where max_active limits its concurrency level
 * on each CPU, in an unbound workqueue, max_active applies to the whole system.
 * As sharing a single nr_active across multiple sockets can be very expensive,
 * the counting and enforcement is per NUMA node.
 *
 * The following struct is used to enforce per-node max_active. When a pwq wants
 * to start executing a work item, it should increment ->nr using
 * tryinc_node_nr_active(). If acquisition fails due to ->nr already being over
 * ->max, the pwq is queued on ->pending_pwqs. As in-flight work items finish
 * and decrement ->nr, node_activate_pending_pwq() activates the pending pwqs in
 * round-robin order.
 */
struct wq_node_nr_active {
	int			max;		/* per-node max_active */
	atomic_t		nr;		/* per-node nr_active */
	raw_spinlock_t		lock;		/* nests inside pool locks */
	struct list_head	pending_pwqs;	/* LN: pwqs with inactive works */
};
/*
 * unbound workqueue 的 max_active 对全系统生效，但用一个跨 socket 的原子热点会
 * 扩展性很差，因此按 NUMA node 分摊额度。`nr` 是正在占用的 node 名额，`max`
 * 是该 node 上限；获取失败的 pwq 挂到 pending_pwqs。完成路径在内层 `lock`
 * （锁序位于 pool lock 内）下轮转激活 pending pwq，避免一个 pwq 长期独占额度。
 */

/*
 * The externally visible workqueue.  It relays the issued work items to
 * the appropriate worker_pool through its pool_workqueues.
 */
struct workqueue_struct {
	struct list_head	pwqs;		/* WR: all pwqs of this wq */
	struct list_head	list;		/* PR: list of all workqueues */

	struct mutex		mutex;		/* protects this wq */
	int			work_color;	/* WQ: current work color */
	int			flush_color;	/* WQ: current flush color */
	atomic_t		nr_pwqs_to_flush; /* flush in progress */
	struct wq_flusher	*first_flusher;	/* WQ: first flusher */
	struct list_head	flusher_queue;	/* WQ: flush waiters */
	struct list_head	flusher_overflow; /* WQ: flush overflow list */

	struct list_head	maydays;	/* MD: pwqs requesting rescue */
	struct worker		*rescuer;	/* MD: rescue worker */

	int			nr_drainers;	/* WQ: drain in progress */

	/* See alloc_workqueue() function comment for info on min/max_active */
	int			max_active;	/* WO: max active works */
	int			min_active;	/* WO: min active works */
	int			saved_max_active; /* WQ: saved max_active */
	int			saved_min_active; /* WQ: saved min_active */

	struct workqueue_attrs	*unbound_attrs;	/* PW: only for unbound wqs */
	struct pool_workqueue __rcu *dfl_pwq;   /* PW: only for unbound wqs */

#ifdef CONFIG_SYSFS
	struct wq_device	*wq_dev;	/* I: for sysfs interface */
#endif
#ifdef CONFIG_LOCKDEP
	char			*lock_name;
	struct lock_class_key	key;
	struct lockdep_map	__lockdep_map;
	struct lockdep_map	*lockdep_map;
#endif
	char			name[WQ_NAME_LEN]; /* I: workqueue name */

	/*
	 * Destruction of workqueue_struct is RCU protected to allow walking
	 * the workqueues list without grabbing wq_pool_mutex.
	 * This is used to dump all workqueues from sysrq.
	 */
	struct rcu_head		rcu;

	/* hot fields used during command issue, aligned to cacheline */
	unsigned int		flags ____cacheline_aligned; /* WQ: WQ_* flags */
	struct pool_workqueue __rcu * __percpu *cpu_pwq; /* I: per-cpu pwqs */
	struct wq_node_nr_active *node_nr_active[]; /* I: per-node nr_active */
};
/*
 * `workqueue_struct` 是对调用者可见的策略与生命周期对象，而不是线程集合。
 * bound wq 通过 per-CPU cpu_pwq 连接固定池；unbound wq 根据 affinity pod 选择
 * pwq，并以 dfl_pwq 提供无锁快速默认项。pwqs/list 的 RCU 可遍历性支持调试输出和
 * 重配期间读侧继续工作。
 *
 * mutex 下的 color/flusher 字段把多次并发 flush 排队；nr_drainers 阻止 drain
 * 期间无限链式入队；maydays/rescuer 为 WQ_MEM_RECLAIM 保留内存回收前进保证。
 * max_active/min_active 通过 READ_ONCE/WRITE_ONCE 对无锁读者发布，实际 pwq/node
 * 限额更新仍需相应锁。结构尾部把发命令热字段放到独立 cacheline，减少管理字段与
 * queue_work() 热路径的伪共享。destroy 时先停止新使用并摘链，最终经 RCU 释放。
 */

/*
 * Each pod type describes how CPUs should be grouped for unbound workqueues.
 * See the comment above workqueue_attrs->affn_scope.
 */
struct wq_pod_type {
	int			nr_pods;	/* number of pods */
	cpumask_var_t		*pod_cpus;	/* pod -> cpus */
	int			*pod_node;	/* pod -> node */
	int			*cpu_pod;	/* cpu -> pod */
};
/*
 * `wq_pod_type` 缓存某种 affinity scope 下 CPU→pod 的拓扑映射：pod_cpus 给每个
 * pod 的 CPU 集，pod_node 给其 NUMA node，cpu_pod 反查 CPU 所属 pod。初始化后
 * 供 unbound pwq 选择共享范围；它描述分组策略，不持有 worker/pwq 引用。
 */

struct work_offq_data {
	u32			pool_id;
	u32			disable;
	u32			flags;
};
/*
 * 当 work 不直接编码 pwq 指针时，`work_offq_data` 是对 work->data 高位的解码：
 * pool_id 记录最近执行池，disable 是可嵌套禁用深度，flags 保存 OFFQ 标志。该值
 * 只是在持有/争得 PENDING 后搬运的状态快照，pack/unpack 必须保留所有位域。
 */

static const char * const wq_affn_names[WQ_AFFN_NR_TYPES] = {
	[WQ_AFFN_DFL]		= "default",
	[WQ_AFFN_CPU]		= "cpu",
	[WQ_AFFN_SMT]		= "smt",
	[WQ_AFFN_CACHE]		= "cache",
	[WQ_AFFN_CACHE_SHARD]	= "cache_shard",
	[WQ_AFFN_NUMA]		= "numa",
	[WQ_AFFN_SYSTEM]	= "system",
};
/* affinity scope 枚举到 sysfs/日志名称的稳定映射；数组只读且与枚举索引一一对应。 */

/*
 * Per-cpu work items which run for longer than the following threshold are
 * automatically considered CPU intensive and excluded from concurrency
 * management to prevent them from noticeably delaying other per-cpu work items.
 * ULONG_MAX indicates that the user hasn't overridden it with a boot parameter.
 * The actual value is initialized in wq_cpu_intensive_thresh_init().
 */
static unsigned long wq_cpu_intensive_thresh_us = ULONG_MAX;
module_param_named(cpu_intensive_thresh_us, wq_cpu_intensive_thresh_us, ulong, 0644);
#ifdef CONFIG_WQ_CPU_INTENSIVE_REPORT
static unsigned int wq_cpu_intensive_warning_thresh = 4;
module_param_named(cpu_intensive_warning_thresh, wq_cpu_intensive_warning_thresh, uint, 0644);
#endif
/*
 * per-CPU work 连续运行超过 `wq_cpu_intensive_thresh_us` 后会暂时退出普通并发
 * 管理，防止长回调压住同池短 work。ULONG_MAX 表示启动参数尚未覆盖，初始化时再
 * 计算默认阈值；warning_thresh 只控制同一回调的告警频率，不改变调度结果。
 */

/* see the comment above the definition of WQ_POWER_EFFICIENT */
static bool wq_power_efficient = IS_ENABLED(CONFIG_WQ_POWER_EFFICIENT_DEFAULT);
module_param_named(power_efficient, wq_power_efficient, bool, 0444);

static unsigned int wq_cache_shard_size = 8;
module_param_named(cache_shard_size, wq_cache_shard_size, uint, 0444);

static bool wq_online;			/* can kworkers be created yet? */
static bool wq_topo_initialized __read_mostly = false;
/*
 * power_efficient 决定带相应标志的系统 workqueue 是否倾向共享 unbound pool；
 * cache_shard_size 控制 LLC pod 进一步分片大小。wq_online 是“现在允许创建
 * kworker”的启动阶段门；wq_topo_initialized 发布拓扑表已经可用，初始化后几乎
 * 只读。它们的时序由 workqueue_init_early()/workqueue_init() 建立。
 */

static struct kmem_cache *pwq_cache;

static struct wq_pod_type wq_pod_types[WQ_AFFN_NR_TYPES];
static enum wq_affn_scope wq_affn_dfl = WQ_AFFN_CACHE_SHARD;
/* pwq_cache 分配 pwq；wq_pod_types 缓存各 scope 拓扑；wq_affn_dfl 是默认 scope。 */

/* buf for wq_update_unbound_pod_attrs(), protected by CPU hotplug exclusion */
static struct workqueue_attrs *unbound_wq_update_pwq_attrs_buf;
/*
 * CPU hotplug 排他区内复用的 attrs 临时缓冲。它不发布给 wq，只承载一次 unbound
 * pod 属性重算；依赖 hotplug 串行化而不是自带锁。
 */

static DEFINE_MUTEX(wq_pool_mutex);	/* protects pools and workqueues list */
static DEFINE_MUTEX(wq_pool_attach_mutex); /* protects worker attach/detach */
static DEFINE_RAW_SPINLOCK(wq_mayday_lock);	/* protects wq->maydays list */
/* wait for manager to go away */
static struct rcuwait manager_wait = __RCUWAIT_INITIALIZER(manager_wait);
/*
 * 锁层次概览：wq_pool_mutex 串行化全局 pool/workqueue 拓扑；attach_mutex 串行化
 * worker 与 pool 的归属；mayday_lock 可在不能睡眠的求救路径保护 maydays。
 * manager_wait 让竞争者等待当前 pool manager 退出管理角色；条件在 pool->lock
 * 下改变，rcuwait 负责无丢失唤醒地睡眠。
 */

static LIST_HEAD(workqueues);		/* PR: list of all workqueues */
static bool workqueue_freezing;		/* PL: have wqs started freezing? */
/* workqueues 是 RCU 可读全局目录；freezing 在 wq_pool_mutex 下发布冻结阶段。 */

/* PL: mirror the cpu_online_mask excluding the CPU in the midst of hotplugging */
static cpumask_var_t wq_online_cpumask;

/* PL&A: allowable cpus for unbound wqs and work items */
static cpumask_var_t wq_unbound_cpumask;

/* PL: user requested unbound cpumask via sysfs */
static cpumask_var_t wq_requested_unbound_cpumask;

/* PL: isolated cpumask to be excluded from unbound cpumask */
static cpumask_var_t wq_isolated_cpumask;
/*
 * 四个全局 CPU mask 的关系：
 * online 是热插拔过程可供 workqueue 使用的在线镜像；requested 是用户/sysfs
 * 请求；isolated 是 housekeeping 策略排除集；unbound 是综合 requested、online、
 * 隔离与启动参数后的实际允许集。更新需 wq_pool_mutex，worker attach 还需遵循
 * attach_mutex；mask 对象本身贯穿 workqueue 子系统生命周期。
 */

/* for further constrain wq_unbound_cpumask by cmdline parameter*/
static struct cpumask wq_cmdline_cpumask __initdata;
/* 仅启动期保存 `workqueue.unbound_cpus=` 约束，初始化合并后其存储可回收。 */

/* CPU where unbound work was last round robin scheduled from this CPU */
static DEFINE_PER_CPU(int, wq_rr_cpu_last);
/* 每个发起 CPU 记录上次 round-robin 目标，避免 unbound work 总偏向同一 CPU。 */

/*
 * Local execution of unbound work items is no longer guaranteed.  The
 * following always forces round-robin CPU selection on unbound work items
 * to uncover usages which depend on it.
 */
#ifdef CONFIG_DEBUG_WQ_FORCE_RR_CPU
static bool wq_debug_force_rr_cpu = true;
#else
static bool wq_debug_force_rr_cpu = false;
#endif
module_param_named(debug_force_rr_cpu, wq_debug_force_rr_cpu, bool, 0644);
/*
 * 调试选项强制 unbound work 不依赖本地执行，用于暴露错误假设。它改变目标选择，
 * 不改变 queue_work() 的“至多一个 pending 实例”语义。
 */

/* to raise softirq for the BH worker pools on other CPUs */
static DEFINE_PER_CPU_SHARED_ALIGNED(struct irq_work [NR_STD_WORKER_POOLS], bh_pool_irq_works);

/* the BH worker pools */
static DEFINE_PER_CPU_SHARED_ALIGNED(struct worker_pool [NR_STD_WORKER_POOLS], bh_worker_pools);

/* the per-cpu worker pools */
static DEFINE_PER_CPU_SHARED_ALIGNED(struct worker_pool [NR_STD_WORKER_POOLS], cpu_worker_pools);
/*
 * 每 CPU 两组标准 pool：BH 数组由 softirq 驱动，cpu_worker_pools 由 kworker 驱动；
 * irq_work 用于跨 CPU 触发 BH softirq。shared-aligned 避免不同 CPU 热字段共享
 * cacheline。
 */

static DEFINE_IDR(worker_pool_idr);	/* PR: idr of all pools */

/* PL: hash of all unbound pools keyed by pool->attrs */
static DEFINE_HASHTABLE(unbound_pool_hash, UNBOUND_POOL_HASH_ORDER);

/* I: attributes used when instantiating standard unbound pools on demand */
static struct workqueue_attrs *unbound_std_wq_attrs[NR_STD_WORKER_POOLS];

/* I: attributes used when instantiating ordered pools on demand */
static struct workqueue_attrs *ordered_wq_attrs[NR_STD_WORKER_POOLS];
/*
 * worker_pool_idr 提供 pool id→对象的 RCU 查找；unbound_pool_hash 按 attrs 复用
 * 已有池；两组标准 attrs 分别作为普通 unbound 和 ordered workqueue 的创建模板。
 * 写侧由 wq_pool_mutex 串行化，模板初始化后只读。
 */

/*
 * I: kthread_worker to release pwq's. pwq release needs to be bounced to a
 * process context while holding a pool lock. Bounce to a dedicated kthread
 * worker to avoid A-A deadlocks.
 */
static struct kthread_worker *pwq_release_worker __ro_after_init;
/*
 * unbound pwq 的最后 put 可能发生在持 pool spinlock 的路径，不能原地执行会取得
 * 其他可睡眠锁的销毁。专用 kthread_worker 把释放弹到进程上下文，避免 A-A
 * workqueue 互相等待形成死锁；指针初始化后只读。
 */

struct workqueue_struct *system_wq __ro_after_init;
EXPORT_SYMBOL(system_wq);
struct workqueue_struct *system_percpu_wq __ro_after_init;
EXPORT_SYMBOL(system_percpu_wq);
struct workqueue_struct *system_highpri_wq __ro_after_init;
EXPORT_SYMBOL_GPL(system_highpri_wq);
struct workqueue_struct *system_long_wq __ro_after_init;
EXPORT_SYMBOL_GPL(system_long_wq);
struct workqueue_struct *system_unbound_wq __ro_after_init;
EXPORT_SYMBOL_GPL(system_unbound_wq);
struct workqueue_struct *system_dfl_wq __ro_after_init;
EXPORT_SYMBOL_GPL(system_dfl_wq);
struct workqueue_struct *system_freezable_wq __ro_after_init;
EXPORT_SYMBOL_GPL(system_freezable_wq);
struct workqueue_struct *system_power_efficient_wq __ro_after_init;
EXPORT_SYMBOL_GPL(system_power_efficient_wq);
struct workqueue_struct *system_freezable_power_efficient_wq __ro_after_init;
EXPORT_SYMBOL_GPL(system_freezable_power_efficient_wq);
struct workqueue_struct *system_bh_wq;
EXPORT_SYMBOL_GPL(system_bh_wq);
struct workqueue_struct *system_bh_highpri_wq;
EXPORT_SYMBOL_GPL(system_bh_highpri_wq);
struct workqueue_struct *system_dfl_long_wq __ro_after_init;
EXPORT_SYMBOL_GPL(system_dfl_long_wq);
/*
 * system_* workqueue 是内核通用实例：普通/percpu/highpri/long/unbound/default、
 * freezable、power-efficient、BH 等组合各自固定不同策略。它们由早期初始化创建，
 * 大多 `__ro_after_init`，模块只借用指针，不负责销毁；选择实例必须匹配回调是否
 * 可睡眠、是否参与 suspend freeze、是否需要 reclaim 前进保证和延迟容忍度。
 */

static int worker_thread(void *__worker);
static void workqueue_sysfs_unregister(struct workqueue_struct *wq);
static void show_pwq(struct pool_workqueue *pwq);
static void show_one_worker_pool(struct worker_pool *pool);

#define CREATE_TRACE_POINTS
#include <trace/events/workqueue.h>

#define assert_rcu_or_pool_mutex()					\
	RCU_LOCKDEP_WARN(!rcu_read_lock_any_held() &&			\
			 !lockdep_is_held(&wq_pool_mutex),		\
			 "RCU or wq_pool_mutex should be held")

#define for_each_bh_worker_pool(pool, cpu)				\
	for ((pool) = &per_cpu(bh_worker_pools, cpu)[0];		\
	     (pool) < &per_cpu(bh_worker_pools, cpu)[NR_STD_WORKER_POOLS]; \
	     (pool)++)

#define for_each_cpu_worker_pool(pool, cpu)				\
	for ((pool) = &per_cpu(cpu_worker_pools, cpu)[0];		\
	     (pool) < &per_cpu(cpu_worker_pools, cpu)[NR_STD_WORKER_POOLS]; \
	     (pool)++)

/**
 * for_each_pool - iterate through all worker_pools in the system
 * @pool: iteration cursor
 * @pi: integer used for iteration
 *
 * This must be called either with wq_pool_mutex held or RCU read
 * locked.  If the pool needs to be used beyond the locking in effect, the
 * caller is responsible for guaranteeing that the pool stays online.
 *
 * The if/else clause exists only for the lockdep assertion and can be
 * ignored.
 */
/*
 * `for_each_pool` 在 IDR 上遍历所有 pool。调用者必须持 wq_pool_mutex 或处于 RCU
 * 读侧；若把 pool 带出保护区，还需另行保证其未离线/释放。if/else 只嵌入 lockdep
 * 断言，不是运行期筛选条件。
 */
#define for_each_pool(pool, pi)						\
	idr_for_each_entry(&worker_pool_idr, pool, pi)			\
		if (({ assert_rcu_or_pool_mutex(); false; })) { }	\
		else

/**
 * for_each_pool_worker - iterate through all workers of a worker_pool
 * @worker: iteration cursor
 * @pool: worker_pool to iterate workers of
 *
 * This must be called with wq_pool_attach_mutex.
 *
 * The if/else clause exists only for the lockdep assertion and can be
 * ignored.
 */
/*
 * 遍历 pool->workers 时必须持 attach_mutex，因为该链与 worker 归属会在 CPU
 * 热插拔/销毁中改变。宏内分支同样只服务 lockdep，循环会访问每个 worker。
 */
#define for_each_pool_worker(worker, pool)				\
	list_for_each_entry((worker), &(pool)->workers, node)		\
		if (({ lockdep_assert_held(&wq_pool_attach_mutex); false; })) { } \
		else

/**
 * for_each_pwq - iterate through all pool_workqueues of the specified workqueue
 * @pwq: iteration cursor
 * @wq: the target workqueue
 *
 * This must be called either with wq->mutex held or RCU read locked.
 * If the pwq needs to be used beyond the locking in effect, the caller is
 * responsible for guaranteeing that the pwq stays online.
 *
 * The if/else clause exists only for the lockdep assertion and can be
 * ignored.
 */
/*
 * pwq 链可在 wq->mutex 下稳定遍历，也可在 RCU 读侧观察；离开保护区后继续使用
 * 必须取得额外寿命保证。该协议允许属性重配摘除旧 pwq，而读者仍安全完成本轮访问。
 */
#define for_each_pwq(pwq, wq)						\
	list_for_each_entry_rcu((pwq), &(wq)->pwqs, pwqs_node,		\
				 lockdep_is_held(&(wq->mutex)))

#ifdef CONFIG_DEBUG_OBJECTS_WORK

static const struct debug_obj_descr work_debug_descr;

/*
 * work_debug_hint() - 为 debugobjects 返回最能标识 work 的回调地址。
 * @addr：借用的 work_struct 地址，debugobjects 保证对象存储期；不可为 NULL。
 * 返回：借用的函数指针，仅用于诊断显示，不取得引用、不改变 work，也不会睡眠。
 */
static void *work_debug_hint(void *addr)
{
	return ((struct work_struct *) addr)->func;
}

/*
 * work_is_static_object() - 判断 work 是否由静态初始化宏创建。
 * @addr：借用 work 地址。返回 STATIC 位的布尔值；只读原子 data，不改变所有权，
 * 可在 debugobjects 检查上下文调用且不睡眠。
 */
static bool work_is_static_object(void *addr)
{
	struct work_struct *work = addr;

	return test_bit(WORK_STRUCT_STATIC_BIT, work_data_bits(work));
}

/*
 * fixup_init is called when:
 * - an active object is initialized
 */
/*
 * debugobjects 在“仍为 active 的 work 被重新初始化”时调用此修复钩子。
 * @addr 是借用 work；@state 是框架观测的旧状态。仅 ACTIVE 可修复：同步取消旧执行，
 * 再把同一存储重新登记为已初始化，返回 true；其他状态返回 false 交给框架报错。
 * cancel_work_sync() 可睡眠，因此该修复只适合允许阻塞的调试路径。
 */
static bool work_fixup_init(void *addr, enum debug_obj_state state)
{
	struct work_struct *work = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		cancel_work_sync(work);
		debug_object_init(work, &work_debug_descr);
		return true;
	default:
		return false;
	}
}

/*
 * fixup_free is called when:
 * - an active object is freed
 */
/*
 * debugobjects 在“仍为 active 的 work 即将释放”时调用此修复钩子。
 * ACTIVE 时先同步取消，确保没有 worker 再访问对象，再从 debugobjects 注销并返回
 * true；其他状态不处理并返回 false。它只修复调试期误用，不能替代调用者正确的
 * cancel/flush 生命周期协议。
 */
static bool work_fixup_free(void *addr, enum debug_obj_state state)
{
	struct work_struct *work = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		cancel_work_sync(work);
		debug_object_free(work, &work_debug_descr);
		return true;
	default:
		return false;
	}
}

static const struct debug_obj_descr work_debug_descr = {
	.name		= "work_struct",
	.debug_hint	= work_debug_hint,
	.is_static_object = work_is_static_object,
	.fixup_init	= work_fixup_init,
	.fixup_free	= work_fixup_free,
};
/* work_debug_descr 把 work 的诊断名称、提示信息、静态对象识别和两类修复钩子注册成一组。 */

/*
 * debug_work_activate()/debug_work_deactivate() - 镜像 work 的排队执行状态。
 * @work 为调用者持有的 work 借用指针；activate 在入队 ownership 建立后登记活跃，
 * deactivate 在执行/取消摘除后登记非活跃。返回无；仅影响调试元数据。
 */
static inline void debug_work_activate(struct work_struct *work)
{
	debug_object_activate(work, &work_debug_descr);
}

static inline void debug_work_deactivate(struct work_struct *work)
{
	debug_object_deactivate(work, &work_debug_descr);
}

/*
 * __init_work() - 向 debugobjects 登记一个刚初始化的 work。
 * @work：由调用者拥有、已分配但尚未排队的对象；@onstack：非零表示存储位于栈上，
 * debugobjects 会使用栈对象规则。返回无，不接管 work；正常路径不睡眠。
 */
void __init_work(struct work_struct *work, int onstack)
{
	if (onstack)
		debug_object_init_on_stack(work, &work_debug_descr);
	else
		debug_object_init(work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(__init_work);

/*
 * destroy_work_on_stack() - 注销一个栈上 work 的调试生命周期。
 * @work 必须已取消/flush 且不再 pending/running；函数不释放栈内存、不返回值。
 * 省略该步骤会留下 debugobjects 假阳性，尚在运行时调用则是调用者的 UAF 错误。
 */
void destroy_work_on_stack(struct work_struct *work)
{
	debug_object_free(work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_work_on_stack);

/*
 * destroy_delayed_work_on_stack() - 同时结束栈上 delayed_work 的 timer 与 work 登记。
 * @work 由调用者拥有且必须已静止；先销毁栈 timer 的调试状态，再注销内嵌 work。
 * 返回无、不释放存储；调用者离开作用域前必须完成此操作。
 */
void destroy_delayed_work_on_stack(struct delayed_work *work)
{
	timer_destroy_on_stack(&work->timer);
	debug_object_free(&work->work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_delayed_work_on_stack);

#else
/*
 * 未启用 CONFIG_DEBUG_OBJECTS_WORK 时，activate/deactivate 是零副作用 stub。
 * 参数只为保持调用点一致；不返回状态、不取得引用，也不改变正式 workqueue 协议。
 */
static inline void debug_work_activate(struct work_struct *work) { }
static inline void debug_work_deactivate(struct work_struct *work) { }
#endif

/**
 * worker_pool_assign_id - allocate ID and assign it to @pool
 * @pool: the pool pointer of interest
 *
 * Returns 0 if ID in [0, WORK_OFFQ_POOL_NONE) is allocated and assigned
 * successfully, -errno on failure.
 */
/*
 * worker_pool_assign_id() - 给尚未发布的 pool 分配可编码进 work->data 的 ID。
 * @pool：调用者拥有、尚未加入全局索引的输入输出对象；入口必须持 wq_pool_mutex，
 * 可睡眠。成功返回 0 并写 pool->id、IDR 开始持有可查找关联；失败返回负 errno，
 * pool 未获得 ID，仍由调用者回滚。
 */
static int worker_pool_assign_id(struct worker_pool *pool)
{
	int ret;

	lockdep_assert_held(&wq_pool_mutex);

	ret = idr_alloc(&worker_pool_idr, pool, 0, WORK_OFFQ_POOL_NONE,
			GFP_KERNEL);
	if (ret >= 0) {
		pool->id = ret;
		return 0;
	}
	return ret;
}

/*
 * unbound_pwq_slot() - 找到 unbound wq 中某 CPU 或默认 pwq 指针槽。
 * @wq：借用且保证存活；@cpu >= 0 选择该 CPU 的 percpu 槽，负值选择 dfl_pwq。
 * 返回槽地址的借用指针，不做 RCU 解引用、不取得 pwq 引用、不睡眠；调用者负责用
 * wq_pool_mutex/wq->mutex 或 RCU 协议访问槽中对象。
 */
static struct pool_workqueue __rcu **
unbound_pwq_slot(struct workqueue_struct *wq, int cpu)
{
       if (cpu >= 0)
               return per_cpu_ptr(wq->cpu_pwq, cpu);
       else
               return &wq->dfl_pwq;
}

/* @cpu < 0 for dfl_pwq */
/*
 * @cpu < 0 表示读取默认 pwq；非负值读取对应 CPU 映射。
 * unbound_pwq() 在 RCU 读侧或持 wq_pool_mutex/wq->mutex 时解引用槽，返回 pwq
 * 借用指针；不增加 refcnt，离开保护范围后不能继续使用，也无失败错误码。
 */
static struct pool_workqueue *unbound_pwq(struct workqueue_struct *wq, int cpu)
{
	return rcu_dereference_check(*unbound_pwq_slot(wq, cpu),
				     lockdep_is_held(&wq_pool_mutex) ||
				     lockdep_is_held(&wq->mutex));
}

/**
 * unbound_effective_cpumask - effective cpumask of an unbound workqueue
 * @wq: workqueue of interest
 *
 * @wq->unbound_attrs->cpumask contains the cpumask requested by the user which
 * is masked with wq_unbound_cpumask to determine the effective cpumask. The
 * default pwq is always mapped to the pool with the current effective cpumask.
 */
/*
 * unbound_effective_cpumask() - 返回 unbound wq 当前真正生效的 CPU 集。
 * @wq 为存活的 unbound workqueue 借用指针，调用者需满足 unbound_pwq() 的锁/RCU
 * 条件。用户 attrs->cpumask 还会与全局允许集相交；默认 pwq 总指向按该结果建立的
 * pool，因此从其 attrs->__pod_cpumask 返回借用只读 mask。无 NULL/错误返回。
 */
static struct cpumask *unbound_effective_cpumask(struct workqueue_struct *wq)
{
	return unbound_pwq(wq, -1)->pool->attrs->__pod_cpumask;
}

/* 把 flush color 移到 work->data 的位域位置；输入须在合法 color 范围，返回编码标志。 */
static unsigned int work_color_to_flags(int color)
{
	return color << WORK_STRUCT_COLOR_SHIFT;
}

/* 从 work->data 快照提取 color；纯位运算，无锁无副作用，返回 [0, WORK_NR_COLORS)。 */
static int get_work_color(unsigned long work_data)
{
	return (work_data >> WORK_STRUCT_COLOR_SHIFT) &
		((1 << WORK_STRUCT_COLOR_BITS) - 1);
}

/* work_next_color() 环形推进 flush 代际；到末端回零，不改变任何全局状态。 */
static int work_next_color(int color)
{
	return (color + 1) % WORK_NR_COLORS;
}

/* pool_offq_flags() 把 BH pool 属性保存为 work 离队后的 OFFQ_BH 标志。 */
static unsigned long pool_offq_flags(struct worker_pool *pool)
{
	return (pool->flags & POOL_BH) ? WORK_OFFQ_BH : 0;
}

/*
 * While queued, %WORK_STRUCT_PWQ is set and non flag bits of a work's data
 * contain the pointer to the queued pwq.  Once execution starts, the flag
 * is cleared and the high bits contain OFFQ flags and pool ID.
 *
 * set_work_pwq(), set_work_pool_and_clear_pending() and mark_work_canceling()
 * can be used to set the pwq, pool or clear work->data. These functions should
 * only be called while the work is owned - ie. while the PENDING bit is set.
 *
 * get_work_pool() and get_work_pwq() can be used to obtain the pool or pwq
 * corresponding to a work.  Pool is available once the work has been
 * queued anywhere after initialization until it is sync canceled.  pwq is
 * available only while the work item is queued.
 */
/*
 * work->data 有两种互斥形态：排队时带 WORK_STRUCT_PWQ，高位是对齐后的 pwq 指针；
 * 开始执行后清 PWQ，高位改存 pool id 和 OFFQ 状态。只有持有 PENDING 的路径才拥有
 * 改写权。pool 从初始化后首次入队直到同步取消前可追溯，pwq 只在“仍排队”窗口有效。
 */
/*
 * set_work_data() - 在已持有 PENDING ownership 时原子替换 work 状态字。
 * @work 为输入输出对象，@data 不含需要保留的 STATIC 位；函数保留 STATIC、无返回，
 * WARN 捕获无 ownership 写入。它只做单次发布，具体顺序由调用者的屏障建立。
 */
static inline void set_work_data(struct work_struct *work, unsigned long data)
{
	WARN_ON_ONCE(!work_pending(work));
	atomic_long_set(&work->data, data | work_static(work));
}

/*
 * set_work_pwq() - 把 owned work 标记为 pending 并关联排队 pwq。
 * @pwq 是在 work 排队期间由其引用保证存活的目标；@flags 含 color/INACTIVE 等低位。
 * 返回无；成功后 work 可由队列/取消路径解码到 pwq。
 */
static void set_work_pwq(struct work_struct *work, struct pool_workqueue *pwq,
			 unsigned long flags)
{
	set_work_data(work, (unsigned long)pwq | WORK_STRUCT_PENDING |
		      WORK_STRUCT_PWQ | flags);
}

/*
 * set_work_pool_and_keep_pending() - 从 pwq 形态切到 off-queue pool id，但保留
 * PENDING ownership。用于移动/取消尚未把 work 交给下一所有者的中间态；无返回。
 */
static void set_work_pool_and_keep_pending(struct work_struct *work,
					   int pool_id, unsigned long flags)
{
	set_work_data(work, ((unsigned long)pool_id << WORK_OFFQ_POOL_SHIFT) |
		      WORK_STRUCT_PENDING | flags);
}

/*
 * set_work_pool_and_clear_pending() - 发布离队状态并把 work 交还给下一次 queue。
 * @work 当前由执行/取消路径拥有；@pool_id 记录最近 pool；@flags 保存 OFFQ/disable。
 * 返回无。前置 wmb 使当前 owner 对 work 的更新先于下一 PENDING owner；清位后的
 * 全屏障阻止即将执行的回调读越过交接点，避免一次并发 queue 因看到旧 PENDING 而
 * 放弃、同时本次回调又因投机旧读漏掉事件，最终造成 work 丢执行。
 */
static void set_work_pool_and_clear_pending(struct work_struct *work,
					    int pool_id, unsigned long flags)
{
	/*
	 * The following wmb is paired with the implied mb in
	 * test_and_set_bit(PENDING) and ensures all updates to @work made
	 * here are visible to and precede any updates by the next PENDING
	 * owner.
	 */
	/*
	 * 该写屏障与下一 queue 路径 test_and_set_bit(PENDING) 隐含的全屏障配对：
	 * 下一 owner 只要成功取得 PENDING，就必须看到本 owner 在交接前完成的修改。
	 */
	smp_wmb();
	set_work_data(work, ((unsigned long)pool_id << WORK_OFFQ_POOL_SHIFT) |
		      flags);
	/*
	 * The following mb guarantees that previous clear of a PENDING bit
	 * will not be reordered with any speculative LOADS or STORES from
	 * work->current_func, which is executed afterwards.  This possible
	 * reordering can lead to a missed execution on attempt to queue
	 * the same @work.  E.g. consider this case:
	 *
	 *   CPU#0                         CPU#1
	 *   ----------------------------  --------------------------------
	 *
	 * 1  STORE event_indicated
	 * 2  queue_work_on() {
	 * 3    test_and_set_bit(PENDING)
	 * 4 }                             set_..._and_clear_pending() {
	 * 5                                 set_work_data() # clear bit
	 * 6                                 smp_mb()
	 * 7                               work->current_func() {
	 * 8				      LOAD event_indicated
	 *				   }
	 *
	 * Without an explicit full barrier speculative LOAD on line 8 can
	 * be executed before CPU#0 does STORE on line 1.  If that happens,
	 * CPU#0 observes the PENDING bit is still set and new execution of
	 * a @work is not queued in a hope, that CPU#1 will eventually
	 * finish the queued @work.  Meanwhile CPU#1 does not see
	 * event_indicated is set, because speculative LOAD was executed
	 * before actual STORE.
	 */
	/*
	 * 上述双 CPU 例子说明仅“原子清 PENDING”还不够：CPU0 可能因旧 PENDING 放弃
	 * 再排队，而 CPU1 的回调若提前读取 event_indicated 又看不到 CPU0 的新事件。
	 * 清位后的 smp_mb() 把回调的 LOAD/STORE 压到 ownership 交接之后，堵住该窗口。
	 */
	smp_mb();
}

/* work_struct_pwq() 从带 PWQ 的 data 中清低位标志并恢复借用 pwq 指针。 */
static inline struct pool_workqueue *work_struct_pwq(unsigned long data)
{
	return (struct pool_workqueue *)(data & WORK_STRUCT_PWQ_MASK);
}

/*
 * get_work_pwq() - 查询 work 当前排队到的 pwq。
 * @work 为并发可变的借用对象；原子读取 data，带 PWQ 时返回借用 pwq，否则 NULL。
 * 不加引用，所以调用者还需以 PENDING ownership、pool lock 或 RCU/上层锁保证寿命。
 */
static struct pool_workqueue *get_work_pwq(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);

	if (data & WORK_STRUCT_PWQ)
		return work_struct_pwq(data);
	else
		return NULL;
}

/**
 * get_work_pool - return the worker_pool a given work was associated with
 * @work: the work item of interest
 *
 * Pools are created and destroyed under wq_pool_mutex, and allows read
 * access under RCU read lock.  As such, this function should be
 * called under wq_pool_mutex or inside of a rcu_read_lock() region.
 *
 * All fields of the returned pool are accessible as long as the above
 * mentioned locking is in effect.  If the returned pool needs to be used
 * beyond the critical section, the caller is responsible for ensuring the
 * returned pool is and stays online.
 *
 * Return: The worker_pool @work was last associated with.  %NULL if none.
 */
/*
 * get_work_pool() - 查询 work 当前或最近关联的执行池。
 * @work 为借用对象；调用者必须持 wq_pool_mutex 或 RCU 读锁。排队态从 pwq 直达
 * pool，离队态用保存的 pool_id 查 IDR；从未关联/同步取消后返回 NULL，否则返回
 * 无新增引用的 pool。若跨出保护区使用，调用者必须另行保证 pool 在线且未释放。
 */
static struct worker_pool *get_work_pool(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);
	int pool_id;

	assert_rcu_or_pool_mutex();

	if (data & WORK_STRUCT_PWQ)
		return work_struct_pwq(data)->pool;

	pool_id = data >> WORK_OFFQ_POOL_SHIFT;
	if (pool_id == WORK_OFFQ_POOL_NONE)
		return NULL;

	return idr_find(&worker_pool_idr, pool_id);
}

/* shift_and_mask() 提取无符号位域；@bits 必须小于类型宽度，返回右移后的低 bits 位。 */
static unsigned long shift_and_mask(unsigned long v, u32 shift, u32 bits)
{
	return (v >> shift) & ((1U << bits) - 1);
}

/*
 * work_offqd_unpack() - 把非 PWQ 形态的 data 解为 pool_id/disable/flags。
 * @offqd 为调用者提供的输出对象；@data 是一致快照。返回无、不取得引用；若 data
 * 仍是排队 pwq 形态则 WARN，因为两种编码不可混解。
 */
static void work_offqd_unpack(struct work_offq_data *offqd, unsigned long data)
{
	WARN_ON_ONCE(data & WORK_STRUCT_PWQ);

	offqd->pool_id = shift_and_mask(data, WORK_OFFQ_POOL_SHIFT,
					WORK_OFFQ_POOL_BITS);
	offqd->disable = shift_and_mask(data, WORK_OFFQ_DISABLE_SHIFT,
					WORK_OFFQ_DISABLE_BITS);
	offqd->flags = data & WORK_OFFQ_FLAG_MASK;
}

/*
 * work_offqd_pack_flags() - 把 disable 深度和 OFFQ flags 重新编码为低/中位状态。
 * @offqd 只读借用；返回值故意不含 pool_id，调用者在确定目标 pool 后另行拼接。
 */
static unsigned long work_offqd_pack_flags(struct work_offq_data *offqd)
{
	return ((unsigned long)offqd->disable << WORK_OFFQ_DISABLE_SHIFT) |
		((unsigned long)offqd->flags);
}

/*
 * Policy functions.  These define the policies on how the global worker
 * pools are managed.  Unless noted otherwise, these functions assume that
 * they're being called with pool->lock held.
 */
/*
 * 以下 policy helper 都在 pool->lock 下读取同一份 worklist、nr_running 和 idle
 * 快照，用于决定唤醒、继续执行、创建与裁剪；它们不自行加锁，也不转移对象所有权。
 */

/*
 * Need to wake up a worker?  Called from anything but currently
 * running workers.
 *
 * Note that, because unbound workers never contribute to nr_running, this
 * function will always return %true for unbound pools as long as the
 * worklist isn't empty.
 */
/*
 * need_more_worker() - worklist 非空且当前没有计入运行的 worker 时需要唤醒。
 * @pool 借用且必须持 lock；返回决策布尔值。unbound worker 不计 nr_running，
 * 所以 unbound pool 只要有 work 就返回 true，这是保证推进而非精确负载估计。
 */
static bool need_more_worker(struct worker_pool *pool)
{
	return !list_empty(&pool->worklist) && !pool->nr_running;
}

/* Can I start working?  Called from busy but !running workers. */
/*
 * may_start_working() - 忙但未计入 running 的 worker 判断是否可接替执行。
 * pool->nr_idle 非零意味着还有 idle worker 可作为并发后备；返回布尔值，无副作用。
 */
static bool may_start_working(struct worker_pool *pool)
{
	return pool->nr_idle;
}

/* Do I need to keep working?  Called from currently running workers. */
/*
 * keep_working() - 当前 running worker 判断本轮后是否继续领取。
 * 只有 worklist 非空且 running 不超过 1 才继续，避免最后一个执行者离开造成停顿；
 * 有其他 running worker 时允许当前线程休息，由同伴维持前进。
 */
static bool keep_working(struct worker_pool *pool)
{
	return !list_empty(&pool->worklist) && (pool->nr_running <= 1);
}

/* Do we need a new worker?  Called from manager. */
/*
 * need_to_create_worker() - manager 判断“有待办、无人运行、也无 idle 可唤醒”。
 * 返回 true 才需要承担可能睡眠/失败的线程创建成本；纯查询，无状态变化。
 */
static bool need_to_create_worker(struct worker_pool *pool)
{
	return need_more_worker(pool) && !may_start_working(pool);
}

/* Do we have too many workers and should some go away? */
/*
 * too_many_workers() - 根据 idle/busy 比例判断是否应裁剪。
 * manager 本身暂不执行 work，按 idle 计入；至少保留两个 idle，超过部分达到 busy
 * 的四分之一才返回 true，避免小幅负载波动引起线程反复创建/销毁。
 */
static bool too_many_workers(struct worker_pool *pool)
{
	bool managing = pool->flags & POOL_MANAGER_ACTIVE;
	int nr_idle = pool->nr_idle + managing; /* manager is considered idle */
	int nr_busy = pool->nr_workers - nr_idle;

	return nr_idle > 2 && (nr_idle - 2) * MAX_IDLE_WORKERS_RATIO >= nr_busy;
}

/**
 * worker_set_flags - set worker flags and adjust nr_running accordingly
 * @worker: self
 * @flags: flags to set
 *
 * Set @flags in @worker->flags and adjust nr_running accordingly.
 */
/*
 * worker_set_flags() - 原子协议内设置 worker 状态并维护 pool->nr_running。
 * @worker 必须是当前 pool 成员，@flags 是要增加的位；入口持 pool->lock。
 * 返回无。只有从“所有 NOT_RUNNING 位都未置”跨入任一 NOT_RUNNING 状态时减一次
 * nr_running；叠加第二个非运行原因不能重复减。
 */
static inline void worker_set_flags(struct worker *worker, unsigned int flags)
{
	struct worker_pool *pool = worker->pool;

	lockdep_assert_held(&pool->lock);

	/* If transitioning into NOT_RUNNING, adjust nr_running. */
	/* 首次进入 NOT_RUNNING 才撤销并发名额，保证组合状态仍只计一次。 */
	if ((flags & WORKER_NOT_RUNNING) &&
	    !(worker->flags & WORKER_NOT_RUNNING)) {
		pool->nr_running--;
	}

	worker->flags |= flags;
}

/**
 * worker_clr_flags - clear worker flags and adjust nr_running accordingly
 * @worker: self
 * @flags: flags to clear
 *
 * Clear @flags in @worker->flags and adjust nr_running accordingly.
 */
/*
 * worker_clr_flags() - 清 worker 状态，并在最后一个 NOT_RUNNING 原因消失时恢复
 * nr_running。@worker/@flags 均为输入输出协议参数，入口持 pool->lock；返回无。
 */
static inline void worker_clr_flags(struct worker *worker, unsigned int flags)
{
	struct worker_pool *pool = worker->pool;
	unsigned int oflags = worker->flags;

	lockdep_assert_held(&pool->lock);

	worker->flags &= ~flags;

	/*
	 * If transitioning out of NOT_RUNNING, increment nr_running.  Note
	 * that the nested NOT_RUNNING is not a noop.  NOT_RUNNING is mask
	 * of multiple flags, not a single flag.
	 */
	/*
	 * NOT_RUNNING 是多个位的掩码：清掉 PREP 后若仍有 CPU_INTENSIVE/UNBOUND，
	 * worker 仍不能计入 running。只有旧状态非运行且新状态所有原因都清空才加一。
	 */
	if ((flags & WORKER_NOT_RUNNING) && (oflags & WORKER_NOT_RUNNING))
		if (!(worker->flags & WORKER_NOT_RUNNING))
			pool->nr_running++;
}

/* Return the first idle worker.  Called with pool->lock held. */
/*
 * first_idle_worker() - 取 idle_list 表头 worker。
 * @pool 借用且持 lock；空链返回 NULL，否则返回无新增引用的 worker。调用者在同一
 * 锁域内立即让其离开 idle，不能把裸指针带到解锁后。
 */
static struct worker *first_idle_worker(struct worker_pool *pool)
{
	if (unlikely(list_empty(&pool->idle_list)))
		return NULL;

	return list_first_entry(&pool->idle_list, struct worker, entry);
}

/**
 * worker_enter_idle - enter idle state
 * @worker: worker which is entering idle state
 *
 * @worker is entering idle state.  Update stats and idle timer if
 * necessary.
 *
 * LOCKING:
 * raw_spin_lock_irq(pool->lock).
 */
/*
 * worker_enter_idle() - 把 worker 发布到 pool 的 idle 集合。
 * @worker 由其线程/创建路径持有，入口需 raw_spin_lock_irq(pool->lock)；返回无。
 * 成功后设置 IDLE、递增 nr_idle、记录最后活跃时间并以 LIFO 入链；idle 过多时启动
 * 延迟裁剪 timer。create_worker() 也会调用，所以不能用会假设 nr_running 已建立的
 * worker_set_flags()。一致性检查确保“所有 worker 都 idle”时 nr_running 为零。
 */
static void worker_enter_idle(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (WARN_ON_ONCE(worker->flags & WORKER_IDLE) ||
	    WARN_ON_ONCE(!list_empty(&worker->entry) &&
			 (worker->hentry.next || worker->hentry.pprev)))
		return;

	/* can't use worker_set_flags(), also called from create_worker() */
	/* 新建 worker 尚未进入正常 running 计数，直接置位可避免把 nr_running 错减为负。 */
	worker->flags |= WORKER_IDLE;
	pool->nr_idle++;
	worker->last_active = jiffies;

	/* idle_list is LIFO */
	/* LIFO 优先复用最近活跃、缓存仍热的 worker；老 worker 留在尾部供超时裁剪。 */
	list_add(&worker->entry, &pool->idle_list);

	if (too_many_workers(pool) && !timer_pending(&pool->idle_timer))
		mod_timer(&pool->idle_timer, jiffies + IDLE_WORKER_TIMEOUT);

	/* Sanity check nr_running. */
	/* 若 worker 全 idle 却仍有 running 计数，说明状态位与并发账户已经失配。 */
	WARN_ON_ONCE(pool->nr_workers == pool->nr_idle && pool->nr_running);
}

/**
 * worker_leave_idle - leave idle state
 * @worker: worker which is leaving idle state
 *
 * @worker is leaving idle state.  Update stats.
 *
 * LOCKING:
 * raw_spin_lock_irq(pool->lock).
 */
/*
 * worker_leave_idle() - 从 idle 集合领取 worker。
 * @worker 必须带 IDLE 且入口持 pool->lock；返回无。清 IDLE、减少 nr_idle 并摘链，
 * 但 IDLE 本身不属于 WORKER_NOT_RUNNING，真正 running 计数由 PREP 等状态转换维护。
 */
static void worker_leave_idle(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (WARN_ON_ONCE(!(worker->flags & WORKER_IDLE)))
		return;
	worker_clr_flags(worker, WORKER_IDLE);
	pool->nr_idle--;
	list_del_init(&worker->entry);
}

/**
 * find_worker_executing_work - find worker which is executing a work
 * @pool: pool of interest
 * @work: work to find worker for
 *
 * Find a worker which is executing @work on @pool by searching
 * @pool->busy_hash which is keyed by the address of @work.  For a worker
 * to match, its current execution should match the address of @work and
 * its work function.  This is to avoid unwanted dependency between
 * unrelated work executions through a work item being recycled while still
 * being executed.
 *
 * This is a bit tricky.  A work item may be freed once its execution
 * starts and nothing prevents the freed area from being recycled for
 * another work item.  If the same work item address ends up being reused
 * before the original execution finishes, workqueue will identify the
 * recycled work item as currently executing and make it wait until the
 * current execution finishes, introducing an unwanted dependency.
 *
 * This function checks the work item address and work function to avoid
 * false positives.  Note that this isn't complete as one may construct a
 * work function which can introduce dependency onto itself through a
 * recycled work item.  Well, if somebody wants to shoot oneself in the
 * foot that badly, there's only so much we can do, and if such deadlock
 * actually occurs, it should be easy to locate the culprit work function.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 *
 * Return:
 * Pointer to worker which is executing @work if found, %NULL
 * otherwise.
 */
/*
 * find_worker_executing_work() - 在 pool 的 busy_hash 中查找正在跑同一 work 实例
 * 且回调也相同的 worker。@pool/@work 均为借用指针，入口持 pool->lock。
 * 返回无新增引用的 worker 或 NULL，不睡眠、不改变状态。
 *
 * 地址与 func 必须同时匹配：回调开始后调用者可能释放 work 存储，并在同一地址构造
 * 新 work；只比地址会让无关新对象错误等待旧执行。仍无法阻止“同一回调主动复用同址
 * 并依赖自己”的病态自死锁，但可避免正常 slab 地址复用造成的假依赖。
 */
static struct worker *find_worker_executing_work(struct worker_pool *pool,
						 struct work_struct *work)
{
	struct worker *worker;

	hash_for_each_possible(pool->busy_hash, worker, hentry,
			       (unsigned long)work)
		if (worker->current_work == work &&
		    worker->current_func == work->func)
			return worker;

	return NULL;
}

/*
 * mayday_cursor_func() - 仅作为 rescuer 扫描 pool->worklist 的哨兵回调。
 * @work 不应被真正执行；若控制流到达这里说明 cursor 被当成普通 work，BUG 立即
 * 暴露链表协议破坏。返回无、正常路径永不调用。
 */
static void mayday_cursor_func(struct work_struct *work)
{
	/* should not be processed, only for marking position */
	BUG();
}

/**
 * move_linked_works - move linked works to a list
 * @work: start of series of works to be scheduled
 * @head: target list to append @work to
 * @nextp: out parameter for nested worklist walking
 *
 * Schedule linked works starting from @work to @head. Work series to be
 * scheduled starts at @work and includes any consecutive work with
 * WORK_STRUCT_LINKED set in its predecessor. See assign_work() for details on
 * @nextp.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
/*
 * move_linked_works() - 把 @work 起始的 linked work 串整体移动到 @head 尾部。
 * @work/@head 为同一 pool 锁保护下的输入输出链；@nextp 可空，非空时更新安全遍历
 * 的下一游标。LINKED 位标在前驱上，循环遇到首个未带 LINKED 的成员后结束。
 * 返回无，不改变 PENDING/pwq ownership，只改变队列归属并保持串内顺序。
 */
static void move_linked_works(struct work_struct *work, struct list_head *head,
			      struct work_struct **nextp)
{
	struct work_struct *n;

	/*
	 * Linked worklist will always end before the end of the list,
	 * use NULL for list head.
	 */
	/*
	 * linked 串保证在物理链表末尾前结束，所以 safe_from 可以用 NULL 作为哨兵头；
	 * 每个成员移到目标尾部，直到当前成员没有声明“下一项也属于本串”。
	 */
	list_for_each_entry_safe_from(work, n, NULL, entry) {
		list_move_tail(&work->entry, head);
		if (!(*work_data_bits(work) & WORK_STRUCT_LINKED))
			break;
	}

	/*
	 * If we're already inside safe list traversal and have moved
	 * multiple works to the scheduled queue, the next position
	 * needs to be updated.
	 */
	/* 嵌套 safe 遍历若不改 nextp，会继续访问已被整体搬走的旧下一项。 */
	if (nextp)
		*nextp = n;
}

/**
 * assign_work - assign a work item and its linked work items to a worker
 * @work: work to assign
 * @worker: worker to assign to
 * @nextp: out parameter for nested worklist walking
 *
 * Assign @work and its linked work items to @worker. If @work is already being
 * executed by another worker in the same pool, it'll be punted there.
 *
 * If @nextp is not NULL, it's updated to point to the next work of the last
 * scheduled work. This allows assign_work() to be nested inside
 * list_for_each_entry_safe().
 *
 * Returns %true if @work was successfully assigned to @worker. %false if @work
 * was punted to another worker already executing it.
 */
/*
 * assign_work() - 把一个 work 及其 linked 后继交给指定 worker 的 scheduled 链。
 * @work 当前在 pool worklist；@worker 为目标且同 pool；@nextp 可空输出安全遍历
 * 游标。入口持 pool->lock。成功移到目标并返回 true；若同 work 已由同池另一 worker
 * 执行，则串接到该 worker 后面以维持同一 work 非重入，返回 false。
 */
static bool assign_work(struct work_struct *work, struct worker *worker,
			struct work_struct **nextp)
{
	struct worker_pool *pool = worker->pool;
	struct worker *collision;

	lockdep_assert_held(&pool->lock);

	/* The cursor work should not be processed */
	/*
	 * mayday cursor 只标记 rescuer 扫描位置：普通 worker 取到时摘掉并推进 nextp，
	 * 绝不能执行。rescuer 自己不应走此分支，否则说明 cursor ownership 混乱。
	 */
	if (unlikely(work->func == mayday_cursor_func)) {
		/* only worker_thread() can possibly take this branch */
		WARN_ON_ONCE(worker->rescue_wq);
		if (nextp)
			*nextp = list_next_entry(work, entry);
		list_del_init(&work->entry);
		return false;
	}

	/*
	 * A single work shouldn't be executed concurrently by multiple workers.
	 * __queue_work() ensures that @work doesn't jump to a different pool
	 * while still running in the previous pool. Here, we should ensure that
	 * @work is not executed concurrently by multiple workers from the same
	 * pool. Check whether anyone is already processing the work. If so,
	 * defer the work to the currently executing one.
	 */
	/*
	 * 跨 pool 非重入由 __queue_work() 处理；这里解决同 pool 碰撞。把新实例追加到
	 * 当前执行 worker->scheduled，保证旧回调结束后由同一 worker 顺序再执行。
	 */
	collision = find_worker_executing_work(pool, work);
	if (unlikely(collision)) {
		move_linked_works(work, &collision->scheduled, nextp);
		return false;
	}

	move_linked_works(work, &worker->scheduled, nextp);
	return true;
}

/*
 * bh_pool_irq_work() - 由 BH pool 的 nice 选择本 CPU 普通/高优先级 irq_work 槽。
 * @pool 必须是已初始化 BH pool；返回静态 per-CPU 对象借用指针，无引用与副作用。
 */
static struct irq_work *bh_pool_irq_work(struct worker_pool *pool)
{
	int high = pool->attrs->nice == HIGHPRI_NICE_LEVEL ? 1 : 0;

	return &per_cpu(bh_pool_irq_works, pool->cpu)[high];
}

/*
 * kick_bh_pool() - 触发目标 BH pool 对应 softirq。
 * @pool 借用且调用点已关 IRQ/持 pool lock。远端且非 draining 时通过 irq_work 发到
 * pool->cpu；本地或离线排空时直接 raise HI_SOFTIRQ/TASKLET_SOFTIRQ。返回无，
 * 只保证 softirq 被标记待处理，不保证 work 已完成。
 */
static void kick_bh_pool(struct worker_pool *pool)
{
#ifdef CONFIG_SMP
	/* see drain_dead_softirq_workfn() for BH_DRAINING */
	/* CPU 离线排空时不能再把 irq_work 投向死亡 CPU，改由当前 CPU 直接处理。 */
	if (unlikely(pool->cpu != smp_processor_id() &&
		     !(pool->flags & POOL_BH_DRAINING))) {
		irq_work_queue_on(bh_pool_irq_work(pool), pool->cpu);
		return;
	}
#endif
	if (pool->attrs->nice == HIGHPRI_NICE_LEVEL)
		raise_softirq_irqoff(HI_SOFTIRQ);
	else
		raise_softirq_irqoff(TASKLET_SOFTIRQ);
}

/**
 * kick_pool - wake up an idle worker if necessary
 * @pool: pool to kick
 *
 * @pool may have pending work items. Wake up worker if necessary. Returns
 * whether a worker was woken up.
 */
/*
 * kick_pool() - 在 worklist 需要推进且存在 idle worker 时触发执行。
 * @pool 借用并持 lock；返回是否实际触发了 BH 或唤醒线程。普通 pool 唤醒最新 idle
 * worker；non-strict unbound worker 若漂出 affinity pod，会在唤醒成本最低的时点
 * best-effort 改 wake_cpu 并计 REPATRIATED。失败仅表示当前无需/无 worker 可唤醒，
 * manager 后续可创建线程。
 */
static bool kick_pool(struct worker_pool *pool)
{
	struct worker *worker = first_idle_worker(pool);
	struct task_struct *p;

	lockdep_assert_held(&pool->lock);

	if (!need_more_worker(pool) || !worker)
		return false;

	if (pool->flags & POOL_BH) {
		kick_bh_pool(pool);
		return true;
	}

	p = worker->task;

#ifdef CONFIG_SMP
	/*
	 * Idle @worker is about to execute @work and waking up provides an
	 * opportunity to migrate @worker at a lower cost by setting the task's
	 * wake_cpu field. Let's see if we want to move @worker to improve
	 * execution locality.
	 *
	 * We're waking the worker that went idle the latest and there's some
	 * chance that @worker is marked idle but hasn't gone off CPU yet. If
	 * so, setting the wake_cpu won't do anything. As this is a best-effort
	 * optimization and the race window is narrow, let's leave as-is for
	 * now. If this becomes pronounced, we can skip over workers which are
	 * still on cpu when picking an idle worker.
	 *
	 * If @pool has non-strict affinity, @worker might have ended up outside
	 * its affinity scope. Repatriate.
	 */
	/*
	 * 唤醒 idle 线程是低成本迁移窗口。选择最新 idle 可能碰到它尚未真正下 CPU，
	 * 此时写 wake_cpu 不生效，但只是局部性优化失败，不影响正确性。non-strict
	 * affinity 允许执行时漂出 pod，这里尽量把它带回有效范围。
	 */
	if (!pool->attrs->affn_strict &&
	    !cpumask_test_cpu(p->wake_cpu, pool->attrs->__pod_cpumask)) {
		struct work_struct *work = list_first_entry(&pool->worklist,
						struct work_struct, entry);
		int wake_cpu = cpumask_any_and_distribute(pool->attrs->__pod_cpumask,
							  cpu_online_mask);
		if (wake_cpu < nr_cpu_ids) {
			p->wake_cpu = wake_cpu;
			get_work_pwq(work)->stats[PWQ_STAT_REPATRIATED]++;
		}
	}
#endif
	wake_up_process(p);
	return true;
}

#ifdef CONFIG_WQ_CPU_INTENSIVE_REPORT

/*
 * Concurrency-managed per-cpu work items that hog CPU for longer than
 * wq_cpu_intensive_thresh_us trigger the automatic CPU_INTENSIVE mechanism,
 * which prevents them from stalling other concurrency-managed work items. If a
 * work function keeps triggering this mechanism, it's likely that the work item
 * should be using an unbound workqueue instead.
 *
 * wq_cpu_intensive_report() tracks work functions which trigger such conditions
 * and report them so that they can be examined and converted to use unbound
 * workqueues as appropriate. To avoid flooding the console, each violating work
 * function is tracked and reported with exponential backoff.
 */
/*
 * 并发管理的 per-CPU work 若长期占 CPU，会自动标成 CPU_INTENSIVE，使其不再阻塞
 * 同池其他 work 的并发补位。反复触发通常说明该回调更适合 WQ_UNBOUND。以下诊断表
 * 按 func 聚合违规次数，并采用指数退避告警，既保留定位证据又避免刷屏。
 */
#define WCI_MAX_ENTS 128

struct wci_ent {
	work_func_t		func;
	atomic64_t		cnt;
	struct hlist_node	hash_node;
};
/*
 * 每个 wci_ent 对应一个违规回调：func 初始化后不变，cnt 原子累加，hash_node 在
 * RCU hash 中发布。条目来自固定数组、从不复用/释放，因此 RCU reader 无 UAF。
 */

static struct wci_ent wci_ents[WCI_MAX_ENTS];
static int wci_nr_ents;
static DEFINE_RAW_SPINLOCK(wci_lock);
static DEFINE_HASHTABLE(wci_hash, ilog2(WCI_MAX_ENTS));

/*
 * wci_find_ent() - 在 RCU 可读 hash 中查找 @func 的固定诊断条目。
 * 返回借用条目或 NULL；调用者需处于允许 RCU hash 读取的上下文，条目永不释放，
 * 函数无副作用、不睡眠。
 */
static struct wci_ent *wci_find_ent(work_func_t func)
{
	struct wci_ent *ent;

	hash_for_each_possible_rcu(wci_hash, ent, hash_node,
				   (unsigned long)func) {
		if (ent->func == func)
			return ent;
	}
	return NULL;
}

/*
 * wq_cpu_intensive_report() - 记录一次回调超时并按指数间隔告警。
 * @func 是正在执行的函数指针，不取得模块引用。已有条目用 relaxed atomic 计数；
 * 新条目在 wci_lock 下从 128 个静态槽中分配并 RCU 发布。返回无、不可睡眠；
 * 表满时静默停止新增，因为大量不同违规已足以说明系统配置/回调存在问题。
 */
static void wq_cpu_intensive_report(work_func_t func)
{
	struct wci_ent *ent;

restart:
	ent = wci_find_ent(func);
	if (ent) {
		u64 cnt;

		/*
		 * Start reporting from the warning_thresh and back off
		 * exponentially.
		 */
	/*
	 * 从 warning_thresh 次开始，在 `cnt + 1 - threshold` 为 2 的幂时打印，
	 * 形成 threshold、threshold+1、threshold+3、threshold+7... 的指数退避。
	 */
		cnt = atomic64_inc_return_relaxed(&ent->cnt);
		if (wq_cpu_intensive_warning_thresh &&
		    cnt >= wq_cpu_intensive_warning_thresh &&
		    is_power_of_2(cnt + 1 - wq_cpu_intensive_warning_thresh))
			printk_deferred(KERN_WARNING "workqueue: %ps hogged CPU for >%luus %llu times, consider switching to WQ_UNBOUND\n",
					ent->func, wq_cpu_intensive_thresh_us,
					atomic64_read(&ent->cnt));
		return;
	}

	/*
	 * @func is a new violation. Allocate a new entry for it. If wcn_ents[]
	 * is exhausted, something went really wrong and we probably made enough
	 * noise already.
	 */
	/* 新 func 先快查、再在锁内复查，解决两个 CPU 同时创建相同条目的竞态。 */
	if (wci_nr_ents >= WCI_MAX_ENTS)
		return;

	raw_spin_lock(&wci_lock);

	if (wci_nr_ents >= WCI_MAX_ENTS) {
		raw_spin_unlock(&wci_lock);
		return;
	}

	if (wci_find_ent(func)) {
		raw_spin_unlock(&wci_lock);
		goto restart;
	}

	ent = &wci_ents[wci_nr_ents++];
	ent->func = func;
	atomic64_set(&ent->cnt, 0);
	hash_add_rcu(wci_hash, &ent->hash_node, (unsigned long)func);

	raw_spin_unlock(&wci_lock);

	goto restart;
}

#else	/* CONFIG_WQ_CPU_INTENSIVE_REPORT */
/* 未启用诊断配置时只关闭报告，自动 CPU_INTENSIVE 调度机制仍然生效。 */
static void wq_cpu_intensive_report(work_func_t func) {}
#endif	/* CONFIG_WQ_CPU_INTENSIVE_REPORT */

/**
 * wq_worker_running - a worker is running again
 * @task: task waking up
 *
 * This function is called when a worker returns from schedule()
 */
/*
 * wq_worker_running() - 调度器在 kworker 从 schedule() 返回时恢复并发账户。
 * @task 必须是当前 kworker，函数从 kthread_data 借用 worker。若此前未标 sleeping
 * 则直接返回；否则在禁抢占区确认仍受并发管理并增加 pool->nr_running，重置本段
 * CPU 占用起点，再用 WRITE_ONCE 清 sleeping。无直接返回值、不可睡眠。
 */
void wq_worker_running(struct task_struct *task)
{
	struct worker *worker = kthread_data(task);

	if (!READ_ONCE(worker->sleeping))
		return;

	/*
	 * If preempted by unbind_workers() between the WORKER_NOT_RUNNING check
	 * and the nr_running increment below, we may ruin the nr_running reset
	 * and leave with an unexpected pool->nr_running == 1 on the newly unbound
	 * pool. Protect against such race.
	 */
	/*
	 * unbind_workers() 可在检查 NOT_RUNNING 与加计数之间抢占并把 pool 计数清零。
	 * 禁抢占把“确认绑定状态+加计数”固定在关联 CPU 上，避免新 unbound pool 留下
	 * 幽灵 nr_running=1。
	 */
	preempt_disable();
	if (!(worker->flags & WORKER_NOT_RUNNING))
		worker->pool->nr_running++;
	preempt_enable();

	/*
	 * CPU intensive auto-detection cares about how long a work item hogged
	 * CPU without sleeping. Reset the starting timestamp on wakeup.
	 */
	/* 自愿睡眠切断“连续占 CPU”区间，醒来从新的 sched runtime 快照重新计时。 */
	worker->current_at = worker->task->se.sum_exec_runtime;

	WRITE_ONCE(worker->sleeping, 0);
}

/**
 * wq_worker_sleeping - a worker is going to sleep
 * @task: task going to sleep
 *
 * This function is called from schedule() when a busy worker is
 * going to sleep.
 */
/*
 * wq_worker_sleeping() - 调度器在 busy kworker 自愿睡眠前归还并发名额。
 * @task 为当前 kworker；rescuer/已 NOT_RUNNING worker 不参与。通过 sleeping 单次
 * 标记防止与 running 钩子重复记账，持 pool->lock 复查 unbind 竞态后减 nr_running，
 * 必要时唤醒替补并计 CM_WAKEUP。返回无，不等待；调度器上下文要求路径不可睡眠。
 */
void wq_worker_sleeping(struct task_struct *task)
{
	struct worker *worker = kthread_data(task);
	struct worker_pool *pool;

	/*
	 * Rescuers, which may not have all the fields set up like normal
	 * workers, also reach here, let's not access anything before
	 * checking NOT_RUNNING.
	 */
	/* rescuer 的 worker 字段并不完整，必须先用 NOT_RUNNING 把它挡在解引用前。 */
	if (worker->flags & WORKER_NOT_RUNNING)
		return;

	pool = worker->pool;

	/* Return if preempted before wq_worker_running() was reached */
	/* sleeping 已置表示上次睡眠尚未完成 running 配对，不能再次减计数。 */
	if (READ_ONCE(worker->sleeping))
		return;

	WRITE_ONCE(worker->sleeping, 1);
	raw_spin_lock_irq(&pool->lock);

	/*
	 * Recheck in case unbind_workers() preempted us. We don't
	 * want to decrement nr_running after the worker is unbound
	 * and nr_running has been reset.
	 */
	/* 锁内复查关闭“置 sleeping 后被 unbind 抢占”的二次减计数窗口。 */
	if (worker->flags & WORKER_NOT_RUNNING) {
		raw_spin_unlock_irq(&pool->lock);
		return;
	}

	pool->nr_running--;
	if (kick_pool(pool))
		worker->current_pwq->stats[PWQ_STAT_CM_WAKEUP]++;

	raw_spin_unlock_irq(&pool->lock);
}

/**
 * wq_worker_tick - a scheduler tick occurred while a kworker is running
 * @task: task currently running
 *
 * Called from sched_tick(). We're in the IRQ context and the current
 * worker's fields which follow the 'K' locking rule can be accessed safely.
 */
/*
 * wq_worker_tick() - scheduler tick 中统计当前 work CPU 时间并检测 CPU hog。
 * @task 为当前 kworker；IRQ 上下文可按 K 规则读取其 current_* 字段。无 current_pwq
 * 或阈值关闭时返回。达到阈值后持 pool lock 设置 CPU_INTENSIVE、报告并唤醒替补。
 * 返回无、不能睡眠；每 tick 增加的 TICK_USEC 是采样统计而非精确运行时。
 */
void wq_worker_tick(struct task_struct *task)
{
	struct worker *worker = kthread_data(task);
	struct pool_workqueue *pwq = worker->current_pwq;
	struct worker_pool *pool = worker->pool;

	if (!pwq)
		return;

	pwq->stats[PWQ_STAT_CPU_TIME] += TICK_USEC;

	if (!wq_cpu_intensive_thresh_us)
		return;

	/*
	 * If the current worker is concurrency managed and hogged the CPU for
	 * longer than wq_cpu_intensive_thresh_us, it's automatically marked
	 * CPU_INTENSIVE to avoid stalling other concurrency-managed work items.
	 *
	 * Set @worker->sleeping means that @worker is in the process of
	 * switching out voluntarily and won't be contributing to
	 * @pool->nr_running until it wakes up. As wq_worker_sleeping() also
	 * decrements ->nr_running, setting CPU_INTENSIVE here can lead to
	 * double decrements. The task is releasing the CPU anyway. Let's skip.
	 * We probably want to make this prettier in the future.
	 */
	/*
	 * sleeping 表示 schedule 切出正在进行，它也会减 nr_running；此时 tick 再设置
	 * CPU_INTENSIVE 会通过 worker_set_flags() 重复减一次。因此跳过，反正该线程
	 * 已释放 CPU，不会阻塞同池前进。
	 */
	if ((worker->flags & WORKER_NOT_RUNNING) || READ_ONCE(worker->sleeping) ||
	    worker->task->se.sum_exec_runtime - worker->current_at <
	    wq_cpu_intensive_thresh_us * NSEC_PER_USEC)
		return;

	raw_spin_lock(&pool->lock);

	worker_set_flags(worker, WORKER_CPU_INTENSIVE);
	wq_cpu_intensive_report(worker->current_func);
	pwq->stats[PWQ_STAT_CPU_INTENSIVE]++;

	if (kick_pool(pool))
		pwq->stats[PWQ_STAT_CM_WAKEUP]++;

	raw_spin_unlock(&pool->lock);
}

/**
 * wq_worker_last_func - retrieve worker's last work function
 * @task: Task to retrieve last work function of.
 *
 * Determine the last function a worker executed. This is called from
 * the scheduler to get a worker's last known identity.
 *
 * CONTEXT:
 * raw_spin_lock_irq(rq->lock)
 *
 * This function is called during schedule() when a kworker is going
 * to sleep. It's used by psi to identify aggregation workers during
 * dequeuing, to allow periodic aggregation to shut-off when that
 * worker is the last task in the system or cgroup to go to sleep.
 *
 * As this function doesn't involve any workqueue-related locking, it
 * only returns stable values when called from inside the scheduler's
 * queuing and dequeuing paths, when @task, which must be a kworker,
 * is guaranteed to not be processing any works.
 *
 * Return:
 * The last work function %current executed as a worker, NULL if it
 * hasn't executed any work yet.
 */
/*
 * wq_worker_last_func() - 为 scheduler/PSI 返回 kworker 最近一次回调身份。
 * @task 必须是 kworker，入口持 rq lock 且处于入/出队路径，保证此刻未处理 work；
 * 返回借用函数指针或从未执行时 NULL。函数不取 workqueue 锁、不增模块引用，只在
 * 该调度器窗口内保证读值稳定。
 */
work_func_t wq_worker_last_func(struct task_struct *task)
{
	struct worker *worker = kthread_data(task);

	return worker->last_func;
}

/**
 * wq_node_nr_active - Determine wq_node_nr_active to use
 * @wq: workqueue of interest
 * @node: NUMA node, can be %NUMA_NO_NODE
 *
 * Determine wq_node_nr_active to use for @wq on @node. Returns:
 *
 * - %NULL for per-cpu workqueues as they don't need to use shared nr_active.
 *
 * - node_nr_active[nr_node_ids] if @node is %NUMA_NO_NODE.
 *
 * - Otherwise, node_nr_active[@node].
 */
/*
 * wq_node_nr_active() - 选择 workqueue 在指定 NUMA node 使用的共享并发账户。
 * @wq 借用；@node 可为 NUMA_NO_NODE。per-CPU/BH 无需共享账户，返回 NULL；
 * unbound 的无 node pool 使用数组额外哨兵槽，否则返回对应 node 对象。返回借用
 * 指针，寿命随 wq，不改变计数且不睡眠。
 */
static struct wq_node_nr_active *wq_node_nr_active(struct workqueue_struct *wq,
						   int node)
{
	if (!(wq->flags & WQ_UNBOUND))
		return NULL;

	if (node == NUMA_NO_NODE)
		node = nr_node_ids;

	return wq->node_nr_active[node];
}

/**
 * wq_update_node_max_active - Update per-node max_actives to use
 * @wq: workqueue to update
 * @off_cpu: CPU that's going down, -1 if a CPU is not going down
 *
 * Update @wq->node_nr_active[]->max. @wq must be unbound. max_active is
 * distributed among nodes according to the proportions of numbers of online
 * cpus. The result is always between @wq->min_active and max_active.
 */
/*
 * wq_update_node_max_active() - 按有效在线 CPU 比例重算 unbound wq 的 node 配额。
 * @wq 必须是 unbound 且入口持 wq->mutex；@off_cpu 是正在离线、尚可能仍在全局
 * online mask 中的 CPU，-1 表示无离线。返回无。各 node 结果 clamp 在 min/max；
 * NUMA_NO_NODE 账户始终取全局 max。
 *
 * 拓扑未发布时不更新。若有效 CPU 全离线，各真实 node 保留 min_active，no-node
 * 保留 max_active，确保冻结之外仍有正配额和前进能力，而不是按零 CPU 算出零。
 */
static void wq_update_node_max_active(struct workqueue_struct *wq, int off_cpu)
{
	struct cpumask *effective = unbound_effective_cpumask(wq);
	int min_active = READ_ONCE(wq->min_active);
	int max_active = READ_ONCE(wq->max_active);
	int total_cpus, node;

	lockdep_assert_held(&wq->mutex);

	if (!wq_topo_initialized)
		return;

	if (off_cpu >= 0 && !cpumask_test_cpu(off_cpu, effective))
		off_cpu = -1;

	total_cpus = cpumask_weight_and(effective, cpu_online_mask);
	if (off_cpu >= 0)
		total_cpus--;

	/* If all CPUs of the wq get offline, use the default values */
	/* 全部离线时比例无定义，使用保活默认值而不是除零或永久封住 inactive work。 */
	if (unlikely(!total_cpus)) {
		for_each_node(node)
			wq_node_nr_active(wq, node)->max = min_active;

		wq_node_nr_active(wq, NUMA_NO_NODE)->max = max_active;
		return;
	}

	for_each_node(node) {
		int node_cpus;

		node_cpus = cpumask_weight_and(effective, cpumask_of_node(node));
		if (off_cpu >= 0 && cpu_to_node(off_cpu) == node)
			node_cpus--;

		wq_node_nr_active(wq, node)->max =
			clamp(DIV_ROUND_UP(max_active * node_cpus, total_cpus),
			      min_active, max_active);
	}

	wq_node_nr_active(wq, NUMA_NO_NODE)->max = max_active;
}

/**
 * get_pwq - get an extra reference on the specified pool_workqueue
 * @pwq: pool_workqueue to get
 *
 * Obtain an extra reference on @pwq.  The caller should guarantee that
 * @pwq has positive refcnt and be holding the matching pool->lock.
 */
/*
 * get_pwq() - 在 pool lock 下给仍存活的 pwq 增加一份引用。
 * @pwq 的 refcnt 必须已为正；返回无。该引用通常由排队 work 持有，保证重配摘链后
 * work->data 仍可安全指向旧 pwq；对应完成/取消路径必须 put_pwq()。
 */
static void get_pwq(struct pool_workqueue *pwq)
{
	lockdep_assert_held(&pwq->pool->lock);
	WARN_ON_ONCE(pwq->refcnt <= 0);
	pwq->refcnt++;
}

/**
 * put_pwq - put a pool_workqueue reference
 * @pwq: pool_workqueue to put
 *
 * Drop a reference of @pwq.  If its refcnt reaches zero, schedule its
 * destruction.  The caller should be holding the matching pool->lock.
 */
/*
 * put_pwq() - 归还 pwq 引用，最后一份时异步安排销毁。
 * @pwq 输入输出且入口持其 pool lock；返回无。不能在 raw spinlock 下直接释放，
 * 所以把 release_work 交给专用 kthread_worker；从此调用者不再拥有该引用。
 */
static void put_pwq(struct pool_workqueue *pwq)
{
	lockdep_assert_held(&pwq->pool->lock);
	if (likely(--pwq->refcnt))
		return;
	/*
	 * @pwq can't be released under pool->lock, bounce to a dedicated
	 * kthread_worker to avoid A-A deadlocks.
	 */
	/* 弹到独立线程还避免用普通 workqueue 销毁 workqueue 自身形成 A-A 依赖。 */
	kthread_queue_work(pwq_release_worker, &pwq->release_work);
}

/**
 * put_pwq_unlocked - put_pwq() with surrounding pool lock/unlock
 * @pwq: pool_workqueue to put (can be %NULL)
 *
 * put_pwq() with locking.  This function also allows %NULL @pwq.
 */
/*
 * put_pwq_unlocked() - 可空版本的引用归还包装。
 * @pwq 为调用者持有的引用或 NULL；依靠 pwq/pool 都有 RCU 延迟释放，先取得其 pool
 * lock 再 put 是安全的。返回无，可能只调度异步释放，不在本函数等待销毁。
 */
static void put_pwq_unlocked(struct pool_workqueue *pwq)
{
	if (pwq) {
		/*
		 * As both pwqs and pools are RCU protected, the
		 * following lock operations are safe.
		 */
		raw_spin_lock_irq(&pwq->pool->lock);
		put_pwq(pwq);
		raw_spin_unlock_irq(&pwq->pool->lock);
	}
}

/*
 * pwq_is_empty() - 判断 pwq 是否既无 active 额度占用，也无 inactive 等待项。
 * 调用者需用 pool lock 稳定字段；返回布尔值，不考虑仅由外部引用维持的已离队 work。
 */
static bool pwq_is_empty(struct pool_workqueue *pwq)
{
	return !pwq->nr_active && list_empty(&pwq->inactive_works);
}

/*
 * __pwq_activate_work() - 把一个 INACTIVE work（含 linked 串）发布到 pool worklist。
 * @pwq/@work 均在 pool lock 下；调用者已取得 nr_active 名额。函数 trace 激活事件，
 * 空→非空时刷新前进时间，移动链并清首 work 的 INACTIVE。返回无，不负责 kick。
 */
static void __pwq_activate_work(struct pool_workqueue *pwq,
				struct work_struct *work)
{
	unsigned long *wdb = work_data_bits(work);

	WARN_ON_ONCE(!(*wdb & WORK_STRUCT_INACTIVE));
	trace_workqueue_activate_work(work);
	if (list_empty(&pwq->pool->worklist))
		pwq->pool->last_progress_ts = jiffies;
	move_linked_works(work, &pwq->pool->worklist, NULL);
	__clear_bit(WORK_STRUCT_INACTIVE_BIT, wdb);
}

/*
 * tryinc_node_nr_active() - 无锁尝试领取一个 NUMA node 并发名额。
 * @nna 借用；读取动态 max，以 relaxed cmpxchg 把 nr 从 old 加一。达到上限返回 false，
 * 成功返回 true。这里只仲裁计数，和 pending_pwqs 的“不丢唤醒”顺序由外层屏障保证。
 */
static bool tryinc_node_nr_active(struct wq_node_nr_active *nna)
{
	int max = READ_ONCE(nna->max);
	int old = atomic_read(&nna->nr);

	do {
		if (old >= max)
			return false;
	} while (!atomic_try_cmpxchg_relaxed(&nna->nr, &old, old + 1));

	return true;
}

/**
 * pwq_tryinc_nr_active - Try to increment nr_active for a pwq
 * @pwq: pool_workqueue of interest
 * @fill: max_active may have increased, try to increase concurrency level
 *
 * Try to increment nr_active for @pwq. Returns %true if an nr_active count is
 * successfully obtained. %false otherwise.
 */
/*
 * pwq_tryinc_nr_active() - 为 pwq 领取 local 与（若为 unbound）node 两层名额。
 * @pwq 入口持 pool lock；@fill 表示 max 刚增大，允许越过已有 pending 排队补足并发。
 * 成功递增 pwq->nr_active 并返回 true；失败返回 false，必要时把 pwq 排到
 * nna->pending_pwqs。plugged ordered pwq 永不获名额。
 */
static bool pwq_tryinc_nr_active(struct pool_workqueue *pwq, bool fill)
{
	struct workqueue_struct *wq = pwq->wq;
	struct worker_pool *pool = pwq->pool;
	struct wq_node_nr_active *nna = wq_node_nr_active(wq, pool->node);
	bool obtained = false;

	lockdep_assert_held(&pool->lock);

	if (!nna) {
		/* BH or per-cpu workqueue, pwq->nr_active is sufficient */
		/* BH/per-CPU 不跨 pool 共享额度，只需比较本 pwq 与 wq->max_active。 */
		obtained = pwq->nr_active < READ_ONCE(wq->max_active);
		goto out;
	}

	if (unlikely(pwq->plugged))
		return false;

	/*
	 * Unbound workqueue uses per-node shared nr_active $nna. If @pwq is
	 * already waiting on $nna, pwq_dec_nr_active() will maintain the
	 * concurrency level. Don't jump the line.
	 *
	 * We need to ignore the pending test after max_active has increased as
	 * pwq_dec_nr_active() can only maintain the concurrency level but not
	 * increase it. This is indicated by @fill.
	 */
	/*
	 * 普通入队不能越过已在 pending 队列中等待的同 pwq；fill 是上限提高后的补洞
	 * 特例，否则完成路径只能维持旧并发，无法主动增长到新上限。
	 */
	if (!list_empty(&pwq->pending_node) && likely(!fill))
		goto out;

	obtained = tryinc_node_nr_active(nna);
	if (obtained)
		goto out;

	/*
	 * Lockless acquisition failed. Lock, add ourself to $nna->pending_pwqs
	 * and try again. The smp_mb() is paired with the implied memory barrier
	 * of atomic_dec_return() in pwq_dec_nr_active() to ensure that either
	 * we see the decremented $nna->nr or they see non-empty
	 * $nna->pending_pwqs.
	 */
	/*
	 * 无锁失败后先入 pending，再用全屏障复查计数；完成方的 atomic_dec_return()
	 * 提供配对屏障。于是两方至少一方会看到另一方：这里看到释放后的名额，或完成方
	 * 看到非空 pending 并负责激活，避免永久停顿。
	 */
	raw_spin_lock(&nna->lock);

	if (list_empty(&pwq->pending_node))
		list_add_tail(&pwq->pending_node, &nna->pending_pwqs);
	else if (likely(!fill))
		goto out_unlock;

	smp_mb();

	obtained = tryinc_node_nr_active(nna);

	/*
	 * If @fill, @pwq might have already been pending. Being spuriously
	 * pending in cold paths doesn't affect anything. Let's leave it be.
	 */
	if (obtained && likely(!fill))
		list_del_init(&pwq->pending_node);

out_unlock:
	raw_spin_unlock(&nna->lock);
out:
	if (obtained)
		pwq->nr_active++;
	return obtained;
}

/**
 * pwq_activate_first_inactive - Activate the first inactive work item on a pwq
 * @pwq: pool_workqueue of interest
 * @fill: max_active may have increased, try to increase concurrency level
 *
 * Activate the first inactive work item of @pwq if available and allowed by
 * max_active limit.
 *
 * Returns %true if an inactive work item has been activated. %false if no
 * inactive work item is found or max_active limit is reached.
 */
/*
 * pwq_activate_first_inactive() - 在有待办且取得名额时激活 pwq 最老 inactive work。
 * @pwq 由 pool lock 保护；@fill 透传“扩容补足”语义。成功移动 work 并返回 true；
 * 链空、plugged 或额度满返回 false，不负责唤醒 pool。
 */
static bool pwq_activate_first_inactive(struct pool_workqueue *pwq, bool fill)
{
	struct work_struct *work =
		list_first_entry_or_null(&pwq->inactive_works,
					 struct work_struct, entry);

	if (work && pwq_tryinc_nr_active(pwq, fill)) {
		__pwq_activate_work(pwq, work);
		return true;
	} else {
		return false;
	}
}

/**
 * unplug_oldest_pwq - unplug the oldest pool_workqueue
 * @wq: workqueue_struct where its oldest pwq is to be unplugged
 *
 * This function should only be called for ordered workqueues where only the
 * oldest pwq is unplugged, the others are plugged to suspend execution to
 * ensure proper work item ordering::
 *
 *    dfl_pwq --------------+     [P] - plugged
 *                          |
 *                          v
 *    pwqs -> A -> B [P] -> C [P] (newest)
 *            |    |        |
 *            1    3        5
 *            |    |        |
 *            2    4        6
 *
 * When the oldest pwq is drained and removed, this function should be called
 * to unplug the next oldest one to start its work item execution. Note that
 * pwq's are linked into wq->pwqs with the oldest first, so the first one in
 * the list is the oldest.
 */
/*
 * unplug_oldest_pwq() - ordered wq 切换 pwq 后，只放行最老一代。
 * @wq 入口持 wq->mutex，pwqs 必须非空；返回无。重配可产生 A→B→C 多个 pwq，
 * B/C 保持 plugged，直到 A 排空摘除才调用本函数放开 B，从而跨 pool 保持提交顺序。
 * 解 plug 后第一次激活真正 work，第二次调用为剩余 inactive 恢复 pending_pwqs 账户，
 * 最后 kick pool。
 */
static void unplug_oldest_pwq(struct workqueue_struct *wq)
{
	struct pool_workqueue *pwq;

	lockdep_assert_held(&wq->mutex);

	/* Caller should make sure that pwqs isn't empty before calling */
	pwq = list_first_entry_or_null(&wq->pwqs, struct pool_workqueue,
				       pwqs_node);
	raw_spin_lock_irq(&pwq->pool->lock);
	if (pwq->plugged) {
		pwq->plugged = false;
		if (pwq_activate_first_inactive(pwq, true)) {
			/*
			 * While plugged, queueing skips activation which
			 * includes bumping the nr_active count and adding the
			 * pwq to nna->pending_pwqs if the count can't be
			 * obtained. We need to restore both for the pwq being
			 * unplugged. The first call activates the first
			 * inactive work item and the second, if there are more
			 * inactive, puts the pwq on pending_pwqs.
			 */
			/*
			 * plugged 期间入队跳过了“取得 nr_active/加入 node pending”两步。
			 * 第一次调用领取名额并激活，第二次为剩余 work 建立等待关系。
			 */
			pwq_activate_first_inactive(pwq, false);

			kick_pool(pwq->pool);
		}
	}
	raw_spin_unlock_irq(&pwq->pool->lock);
}

/**
 * node_activate_pending_pwq - Activate a pending pwq on a wq_node_nr_active
 * @nna: wq_node_nr_active to activate a pending pwq for
 * @caller_pool: worker_pool the caller is locking
 *
 * Activate a pwq in @nna->pending_pwqs. Called with @caller_pool locked.
 * @caller_pool may be unlocked and relocked to lock other worker_pools.
 */
/*
 * node_activate_pending_pwq() - 归还一个 node 名额后，轮转激活某个等待 pwq。
 * @nna 为共享账户；@caller_pool 入口/出口都保持其 lock，但函数中间可为锁另一个
 * pool 而暂时释放它。返回无。锁序始终 pool lock → nna lock；切换 pool 时必须
 * 释放内层 nna lock，重新取锁后从队首 retry，因为列表可能已变化。
 */
static void node_activate_pending_pwq(struct wq_node_nr_active *nna,
				      struct worker_pool *caller_pool)
{
	struct worker_pool *locked_pool = caller_pool;
	struct pool_workqueue *pwq;
	struct work_struct *work;

	lockdep_assert_held(&caller_pool->lock);

	raw_spin_lock(&nna->lock);
retry:
	pwq = list_first_entry_or_null(&nna->pending_pwqs,
				       struct pool_workqueue, pending_node);
	if (!pwq)
		goto out_unlock;

	/*
	 * If @pwq is for a different pool than @locked_pool, we need to lock
	 * @pwq->pool->lock. Let's trylock first. If unsuccessful, do the unlock
	 * / lock dance. For that, we also need to release @nna->lock as it's
	 * nested inside pool locks.
	 */
	/*
	 * trylock 成功可在不放 nna lock 时切换；失败则严格按层级释放 nna、阻塞取得
	 * 新 pool、再取 nna 并重查，避免 ABBA，也不依赖过期 pwq 游标。
	 */
	if (pwq->pool != locked_pool) {
		raw_spin_unlock(&locked_pool->lock);
		locked_pool = pwq->pool;
		if (!raw_spin_trylock(&locked_pool->lock)) {
			raw_spin_unlock(&nna->lock);
			raw_spin_lock(&locked_pool->lock);
			raw_spin_lock(&nna->lock);
			goto retry;
		}
	}

	/*
	 * $pwq may not have any inactive work items due to e.g. cancellations.
	 * Drop it from pending_pwqs and see if there's another one.
	 */
	/* cancel 可能清空 inactive 链；这种 stale pending 节点直接摘除并找下一项。 */
	work = list_first_entry_or_null(&pwq->inactive_works,
					struct work_struct, entry);
	if (!work) {
		list_del_init(&pwq->pending_node);
		goto retry;
	}

	/*
	 * Acquire an nr_active count and activate the inactive work item. If
	 * $pwq still has inactive work items, rotate it to the end of the
	 * pending_pwqs so that we round-robin through them. This means that
	 * inactive work items are not activated in queueing order which is fine
	 * given that there has never been any ordering across different pwqs.
	 */
	/*
	 * 成功后若仍有待办把 pwq 移到队尾，按 pwq 公平轮转。不同 pwq 原本就没有全局
	 * queue 顺序保证，因此这里不按每个 work 的绝对入队时间排序。
	 */
	if (likely(tryinc_node_nr_active(nna))) {
		pwq->nr_active++;
		__pwq_activate_work(pwq, work);

		if (list_empty(&pwq->inactive_works))
			list_del_init(&pwq->pending_node);
		else
			list_move_tail(&pwq->pending_node, &nna->pending_pwqs);

		/* if activating a foreign pool, make sure it's running */
		if (pwq->pool != caller_pool)
			kick_pool(pwq->pool);
	}

out_unlock:
	raw_spin_unlock(&nna->lock);
	if (locked_pool != caller_pool) {
		raw_spin_unlock(&locked_pool->lock);
		raw_spin_lock(&caller_pool->lock);
	}
}

/**
 * pwq_dec_nr_active - Retire an active count
 * @pwq: pool_workqueue of interest
 *
 * Decrement @pwq's nr_active and try to activate the first inactive work item.
 * For unbound workqueues, this function may temporarily drop @pwq->pool->lock.
 */
/*
 * pwq_dec_nr_active() - work 完成/取消时归还 pwq active 名额并推动下一项。
 * @pwq 入口持其 pool lock；返回无。per-CPU 只在本 pwq 激活最老 inactive；
 * unbound 还原子归还 node 名额，并可能跨 pool 调 node_activate_pending_pwq()，
 * 因而调用者在此之前必须完成当前 work 其他所有需 pool lock 的状态更新。
 */
static void pwq_dec_nr_active(struct pool_workqueue *pwq)
{
	struct worker_pool *pool = pwq->pool;
	struct wq_node_nr_active *nna = wq_node_nr_active(pwq->wq, pool->node);

	lockdep_assert_held(&pool->lock);

	/*
	 * @pwq->nr_active should be decremented for both percpu and unbound
	 * workqueues.
	 */
	/* 无论是否共享 node 账户，本 pwq 的 active 账都先且只减一次。 */
	pwq->nr_active--;

	/*
	 * For a percpu workqueue, it's simple. Just need to kick the first
	 * inactive work item on @pwq itself.
	 */
	if (!nna) {
		pwq_activate_first_inactive(pwq, false);
		return;
	}

	/*
	 * If @pwq is for an unbound workqueue, it's more complicated because
	 * multiple pwqs and pools may be sharing the nr_active count. When a
	 * pwq needs to wait for an nr_active count, it puts itself on
	 * $nna->pending_pwqs. The following atomic_dec_return()'s implied
	 * memory barrier is paired with smp_mb() in pwq_tryinc_nr_active() to
	 * guarantee that either we see non-empty pending_pwqs or they see
	 * decremented $nna->nr.
	 *
	 * $nna->max may change as CPUs come online/offline and @pwq->wq's
	 * max_active gets updated. However, it is guaranteed to be equal to or
	 * larger than @pwq->wq->min_active which is above zero unless freezing.
	 * This maintains the forward progress guarantee.
	 */
	/*
	 * atomic_dec_return 的全序与领取方入 pending 后的 smp_mb 配对，保证不会出现
	 * “释放者没看见等待者、等待者也没看见已释放名额”。动态 max 至少为正的
	 * min_active（冻结期除外），因此系统保留逐步排空能力。
	 */
	if (atomic_dec_return(&nna->nr) >= READ_ONCE(nna->max))
		return;

	if (!list_empty(&nna->pending_pwqs))
		node_activate_pending_pwq(nna, pool);
}

/**
 * pwq_dec_nr_in_flight - decrement pwq's nr_in_flight
 * @pwq: pwq of interest
 * @work_data: work_data of work which left the queue
 *
 * A work either has completed or is removed from pending queue,
 * decrement nr_in_flight of its pwq and handle workqueue flushing.
 *
 * NOTE:
 * For unbound workqueues, this function may temporarily drop @pwq->pool->lock
 * and thus should be called after all other state updates for the in-flight
 * work item is complete.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
/*
 * pwq_dec_nr_in_flight() - 结束一个 work 对 pwq 的全部账并推进 flush。
 * @pwq 入口持 pool lock；@work_data 必须是离队前保存的含 color/INACTIVE 快照。
 * active work 先归还 active 名额，随后减该 color in-flight；若恰是当前 flush color
 * 的最后一项，清 pwq flush_color，并在最后一个 pwq 完成时 complete 首 flusher。
 * 最后始终 put work 持有的 pwq 引用。unbound 路径可能临时换锁，故必须作为当前
 * work 状态更新的最后一步。
 */
static void pwq_dec_nr_in_flight(struct pool_workqueue *pwq, unsigned long work_data)
{
	int color = get_work_color(work_data);

	if (!(work_data & WORK_STRUCT_INACTIVE))
		pwq_dec_nr_active(pwq);

	pwq->nr_in_flight[color]--;

	/* is flush in progress and are we at the flushing tip? */
	/* 只有当前被 flush 的 color 才参与唤醒；其他代际只做正常记账。 */
	if (likely(pwq->flush_color != color))
		goto out_put;

	/* are there still in-flight works? */
	/* 同 pwq 同 color 仍有 work 时，首 flusher 的完成条件尚未建立。 */
	if (pwq->nr_in_flight[color])
		goto out_put;

	/* this pwq is done, clear flush_color */
	/* -1 表示本 pwq 不再承担当前 flush 代际。 */
	pwq->flush_color = -1;

	/*
	 * If this was the last pwq, wake up the first flusher.  It
	 * will handle the rest.
	 */
	if (atomic_dec_and_test(&pwq->wq->nr_pwqs_to_flush))
		complete(&pwq->wq->first_flusher->done);
out_put:
	put_pwq(pwq);
}

/**
 * try_to_grab_pending - steal work item from worklist and disable irq
 * @work: work item to steal
 * @cflags: %WORK_CANCEL_ flags
 * @irq_flags: place to store irq state
 *
 * Try to grab PENDING bit of @work.  This function can handle @work in any
 * stable state - idle, on timer or on worklist.
 *
 * Return:
 *
 *  ========	================================================================
 *  1		if @work was pending and we successfully stole PENDING
 *  0		if @work was idle and we claimed PENDING
 *  -EAGAIN	if PENDING couldn't be grabbed at the moment, safe to busy-retry
 *  ========	================================================================
 *
 * Note:
 * On >= 0 return, the caller owns @work's PENDING bit.  To avoid getting
 * interrupted while holding PENDING and @work off queue, irq must be
 * disabled on entry.  This, combined with delayed_work->timer being
 * irqsafe, ensures that we return -EAGAIN for finite short period of time.
 *
 * On successful return, >= 0, irq is disabled and the caller is
 * responsible for releasing it using local_irq_restore(*@irq_flags).
 *
 * This function is safe to call from any context including IRQ handler.
 */
/*
 * try_to_grab_pending() - 为 cancel/modify 路径抢到 work 的唯一 PENDING ownership。
 * @work 可处于 idle、timer、排队或短暂 queue 过渡态；@cflags 指明 delayed/disable；
 * @irq_flags 输出入口 IRQ 状态。返回 1 表示从 timer/list 偷到原本 pending 的 work，
 * 0 表示 idle 且本调用新取得 PENDING，-EAGAIN 表示 queue/执行交接中可短暂重试。
 *
 * 非负返回时 IRQ 保持关闭，work 离队且 PENDING 归调用者，调用者最终必须恢复 IRQ
 * 并清/转移 PENDING。timer 为 irqsafe，加上本地关 IRQ，使 -EAGAIN 窗口有限；
 * 函数可从 IRQ 调用且不睡眠。排队态在 RCU + pool lock 下验证 work->data 与 pool
 * 仍匹配，防止使用重配/离队后的旧 pwq。
 */
static int try_to_grab_pending(struct work_struct *work, u32 cflags,
			       unsigned long *irq_flags)
{
	struct worker_pool *pool;
	struct pool_workqueue *pwq;

	local_irq_save(*irq_flags);

	/* try to steal the timer if it exists */
	/* delayed_work 先删除 timer；成功时 timer 持有的 PENDING 原样转给调用者。 */
	if (cflags & WORK_CANCEL_DELAYED) {
		struct delayed_work *dwork = to_delayed_work(work);

		/*
		 * dwork->timer is irqsafe.  If timer_delete() fails, it's
		 * guaranteed that the timer is not queued anywhere and not
		 * running on the local CPU.
		 */
	/*
	 * timer_delete() 失败时 timer 已不在任何队列且不在本 CPU 执行；远端回调可能
	 * 正处于把 work 交给 __queue_work() 的窗口，后续按普通 PENDING 协议处理。
	 */
		if (likely(timer_delete(&dwork->timer)))
			return 1;
	}

	/* try to claim PENDING the normal way */
	/* idle work 的 PENDING 原来为 0；test_and_set 成功完成 ownership 原子仲裁。 */
	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)))
		return 0;

	rcu_read_lock();
	/*
	 * The queueing is in progress, or it is already queued. Try to
	 * steal it from ->worklist without clearing WORK_STRUCT_PENDING.
	 */
	/* PENDING 已为 1 时可能是入队中或已排队；保留该位并尝试从 pool 链表偷走。 */
	pool = get_work_pool(work);
	if (!pool)
		goto fail;

	raw_spin_lock(&pool->lock);
	/*
	 * work->data is guaranteed to point to pwq only while the work
	 * item is queued on pwq->wq, and both updating work->data to point
	 * to pwq on queueing and to pool on dequeueing are done under
	 * pwq->pool->lock.  This in turn guarantees that, if work->data
	 * points to pwq which is associated with a locked pool, the work
	 * item is currently queued on that pool.
	 */
	/*
	 * queue 与 dequeue 都在同一 pool lock 下切换 data 形态。因此锁住 get_work_pool()
	 * 得到的 pool 后再次读取 pwq 且确认归属相同，就证明 work 当前确在该池链上。
	 */
	pwq = get_work_pwq(work);
	if (pwq && pwq->pool == pool) {
		unsigned long work_data = *work_data_bits(work);

		debug_work_deactivate(work);

		/*
		 * A cancelable inactive work item must be in the
		 * pwq->inactive_works since a queued barrier can't be
		 * canceled (see the comments in insert_wq_barrier()).
		 *
		 * An inactive work item cannot be deleted directly because
		 * it might have linked barrier work items which, if left
		 * on the inactive_works list, will confuse pwq->nr_active
		 * management later on and cause stall.  Move the linked
		 * barrier work items to the worklist when deleting the grabbed
		 * item. Also keep WORK_STRUCT_INACTIVE in work_data, so that
		 * it doesn't participate in nr_active management in later
		 * pwq_dec_nr_in_flight().
		 */
		/*
		 * inactive 普通 work 必在 inactive_works；其后可能链接不可取消 barrier。
		 * 删除首 work 前先把 linked barrier 推到可执行 worklist，并在快照保留
		 * INACTIVE，使稍后的 in-flight 结算不会错误减少 nr_active。
		 */
		if (work_data & WORK_STRUCT_INACTIVE)
			move_linked_works(work, &pwq->pool->worklist, NULL);

		list_del_init(&work->entry);

		/*
		 * work->data points to pwq iff queued. Let's point to pool. As
		 * this destroys work->data needed by the next step, stash it.
		 */
		/* 先保存完整 work_data，再改成 off-queue pool id；flush/color 结算仍需旧值。 */
		set_work_pool_and_keep_pending(work, pool->id,
					       pool_offq_flags(pool));

		/* must be the last step, see the function comment */
		/* 此调用可能为 unbound 换锁，故所有链表/data 更新必须已经完成。 */
		pwq_dec_nr_in_flight(pwq, work_data);

		raw_spin_unlock(&pool->lock);
		rcu_read_unlock();
		return 1;
	}
	raw_spin_unlock(&pool->lock);
fail:
	rcu_read_unlock();
	local_irq_restore(*irq_flags);
	return -EAGAIN;
}

/**
 * work_grab_pending - steal work item from worklist and disable irq
 * @work: work item to steal
 * @cflags: %WORK_CANCEL_ flags
 * @irq_flags: place to store IRQ state
 *
 * Grab PENDING bit of @work. @work can be in any stable state - idle, on timer
 * or on worklist.
 *
 * Can be called from any context. IRQ is disabled on return with IRQ state
 * stored in *@irq_flags. The caller is responsible for re-enabling it using
 * local_irq_restore().
 *
 * Returns %true if @work was pending. %false if idle.
 */
/*
 * work_grab_pending() - 对 try_to_grab_pending() 做有限窗口忙重试。
 * 参数与 IRQ ownership 相同；直到取得 PENDING 才返回，true 表示原本 pending，
 * false 表示原本 idle。返回时 IRQ 必关。cpu_relax() 只缓解自旋，不可在可能永远
 * 不完成的错误状态上提供超时，因此依赖底层 queue/timer 交接必然前进。
 */
static bool work_grab_pending(struct work_struct *work, u32 cflags,
			      unsigned long *irq_flags)
{
	int ret;

	while (true) {
		ret = try_to_grab_pending(work, cflags, irq_flags);
		if (ret >= 0)
			return ret;
		cpu_relax();
	}
}

/**
 * insert_work - insert a work into a pool
 * @pwq: pwq @work belongs to
 * @work: work to insert
 * @head: insertion point
 * @extra_flags: extra WORK_STRUCT_* flags to set
 *
 * Insert @work which belongs to @pwq after @head.  @extra_flags is or'd to
 * work_struct flags.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
/*
 * insert_work() - 在持 pool lock 且拥有 PENDING 时把 work 发布到指定链表。
 * @pwq 是目标关联；@work 为输入输出对象；@head 是插入点；@extra_flags 含 color、
 * LINKED/INACTIVE 等。返回无。依次登记 debug 活跃、记录 KASAN 辅助栈、发布 data
 * 的 pwq 形态、入链并给 pwq 加引用；从此队列/worker 负责完成或取消时归还引用。
 */
static void insert_work(struct pool_workqueue *pwq, struct work_struct *work,
			struct list_head *head, unsigned int extra_flags)
{
	debug_work_activate(work);

	/* record the work call stack in order to print it in KASAN reports */
	/* 保存“本次排队者”调用栈，未来 work UAF 报告可连接异步因果链。 */
	kasan_record_aux_stack(work);

	/* we own @work, set data and link */
	/* PENDING owner 在同一锁区先写可解码 pwq，再入链，读者不会看到半发布对象。 */
	set_work_pwq(work, pwq, extra_flags);
	list_add_tail(&work->entry, head);
	get_pwq(pwq);
}

/*
 * Test whether @work is being queued from another work executing on the
 * same workqueue.
 */
/*
 * is_chained_work() - 判断当前上下文是否正从同一 wq 的回调链式入队。
 * @wq 借用；current_wq_worker() 返回当前 worker 或 NULL。worker 只被自身读取，
 * 无需锁即可比 current_pwq->wq。返回布尔值，无副作用；drain 只允许这种有限链式
 * 扩展，拒绝外部生产者继续注入。
 */
static bool is_chained_work(struct workqueue_struct *wq)
{
	struct worker *worker;

	worker = current_wq_worker();
	/*
	 * Return %true iff I'm a worker executing a work item on @wq.  If
	 * I'm @worker, it's safe to dereference it without locking.
	 */
	/* worker 自指针在当前 kworker 执行期间稳定，读取 current_pwq 遵循 S/K 规则。 */
	return worker && worker->current_pwq->wq == wq;
}

/*
 * When queueing an unbound work item to a wq, prefer local CPU if allowed
 * by wq_unbound_cpumask.  Otherwise, round robin among the allowed ones to
 * avoid perturbing sensitive tasks.
 */
/*
 * wq_select_unbound_cpu() - 为 unbound work 选择用于索引 pwq 的允许 CPU。
 * @cpu 是首选/当前 CPU。正常模式若在 wq_unbound_cpumask 内直接返回以保局部性；
 * 否则按发起 CPU 的 rr 游标在“允许∩在线”集合轮转。无可用候选时退回 @cpu，
 * 后续 pool affinity/热插拔协议保证前进。返回 CPU id，不迁移当前线程。
 */
static int wq_select_unbound_cpu(int cpu)
{
	int new_cpu;

	if (likely(!wq_debug_force_rr_cpu)) {
		if (cpumask_test_cpu(cpu, wq_unbound_cpumask))
			return cpu;
	} else {
		pr_warn_once("workqueue: round-robin CPU selection forced, expect performance impact\n");
	}

	new_cpu = __this_cpu_read(wq_rr_cpu_last);
	new_cpu = cpumask_next_and_wrap(new_cpu, wq_unbound_cpumask, cpu_online_mask);
	if (unlikely(new_cpu >= nr_cpu_ids))
		return cpu;
	__this_cpu_write(wq_rr_cpu_last, new_cpu);

	return new_cpu;
}

/*
 * __queue_work() - 在调用者已取得 PENDING 且关闭本地 IRQ 后完成真正入队。
 *
 * @cpu：请求 CPU，WORK_CPU_UNBOUND 表示按 wq 策略选择；@wq：存活的目标策略对象，
 * 调用者借用且不得处于已销毁状态；@work：输入输出 work，入口为 PENDING=1、
 * off-queue。返回无。成功后 work->data 指向 pwq、pwq ref/in-flight 已增加并位于
 * active 或 inactive 链；拒绝 destroying/draining 的外部入队时会清 PENDING。
 *
 * 阶段：检查生命周期 → 在 RCU 下选 pwq/pool → 若旧 pool 仍执行同 work 则回投旧
 * worker 以保证非重入 → 锁定并复查 pwq 寿命 → 计 color/in-flight → 领取 active
 * 额度或进 inactive 链 → 必要时 kick。函数不睡眠，可由 IRQ 路径调用。
 */
static void __queue_work(int cpu, struct workqueue_struct *wq,
			 struct work_struct *work)
{
	struct pool_workqueue *pwq;
	struct worker_pool *last_pool, *pool;
	unsigned int work_flags;
	unsigned int req_cpu = cpu;

	/*
	 * NOTE: Check whether the used workqueue is deprecated and warn
	 */
	/* deprecated 仅诊断一次，仍执行入队以保持旧调用者行为。 */
	if (unlikely(wq->flags & __WQ_DEPRECATED))
		pr_warn_once("workqueue: work func %ps enqueued on deprecated workqueue. "
			"Use system_{percpu|dfl}_wq instead.\n",
			work->func);

	/*
	 * While a work item is PENDING && off queue, a task trying to
	 * steal the PENDING will busy-loop waiting for it to either get
	 * queued or lose PENDING.  Grabbing PENDING and queueing should
	 * happen with IRQ disabled.
	 */
	/*
	 * PENDING=1 但尚未入链是 ownership 交接窗口，cancel 会忙等。关本地 IRQ 防止
	 * 同 CPU timer/IRQ 重入持有这个半状态过久或形成自旋互等。
	 */
	lockdep_assert_irqs_disabled();

	/*
	 * For a draining wq, only works from the same workqueue are
	 * allowed. The __WQ_DESTROYING helps to spot the issue that
	 * queues a new work item to a wq after destroy_workqueue(wq).
	 */
	/*
	 * drain 允许当前 wq 回调产生 chained work，使已有依赖闭包能收敛；外部生产者
	 * 会让 drain 永不结束。DESTROYING 额外捕获释放后继续入队的生命周期错误。
	 */
	if (unlikely(wq->flags & (__WQ_DESTROYING | __WQ_DRAINING) &&
		     WARN_ONCE(!is_chained_work(wq), "workqueue: cannot queue %ps on wq %s\n",
			       work->func, wq->name))) {
		struct work_offq_data offqd;

		/*
		 * State on entry: PENDING is set, work is off-queue (no
		 * insert_work() has run).
		 *
		 * Returning without clearing PENDING would leave the work
		 * in a weird state (PENDING=1, PWQ=0, entry empty)
		 */
		/* 拒绝后必须完成 ownership 交还，否则 work 永远看似 pending 却无执行者。 */
		work_offqd_unpack(&offqd, *work_data_bits(work));
		set_work_pool_and_clear_pending(work, offqd.pool_id,
						work_offqd_pack_flags(&offqd));
		return;
	}
	rcu_read_lock();
retry:
	/* pwq which will be used unless @work is executing elsewhere */
	/* 第一次按请求 CPU 选候选；仍在旧 pool 执行时下面会覆盖为旧 worker 的 pwq。 */
	if (req_cpu == WORK_CPU_UNBOUND) {
		if (wq->flags & WQ_UNBOUND)
			cpu = wq_select_unbound_cpu(raw_smp_processor_id());
		else
			cpu = raw_smp_processor_id();
	}

	pwq = rcu_dereference(*per_cpu_ptr(wq->cpu_pwq, cpu));
	pool = pwq->pool;

	/*
	 * If @work was previously on a different pool, it might still be
	 * running there, in which case the work needs to be queued on that
	 * pool to guarantee non-reentrancy.
	 *
	 * For ordered workqueue, work items must be queued on the newest pwq
	 * for accurate order management.  Guaranteed order also guarantees
	 * non-reentrancy.  See the comments above unplug_oldest_pwq().
	 */
	/*
	 * 同一 work 在旧 pool 回调未结束时必须排回该 worker，否则两个 pool 可并发调用
	 * 同一 func/对象。ordered wq 始终投最新 pwq，它的 plugged 代际本身提供非重入。
	 */
	last_pool = get_work_pool(work);
	if (last_pool && last_pool != pool && !(wq->flags & __WQ_ORDERED)) {
		struct worker *worker;

		raw_spin_lock(&last_pool->lock);

		worker = find_worker_executing_work(last_pool, work);

		if (worker && worker->current_pwq->wq == wq) {
			pwq = worker->current_pwq;
			pool = pwq->pool;
			WARN_ON_ONCE(pool != last_pool);
		} else {
			/* meh... not running there, queue here */
			/* 旧 pool 已无该执行实例，释放旧锁后锁定本次选择的目标 pool。 */
			raw_spin_unlock(&last_pool->lock);
			raw_spin_lock(&pool->lock);
		}
	} else {
		raw_spin_lock(&pool->lock);
	}

	/*
	 * pwq is determined and locked. For unbound pools, we could have raced
	 * with pwq release and it could already be dead. If its refcnt is zero,
	 * repeat pwq selection. Note that unbound pwqs never die without
	 * another pwq replacing it in cpu_pwq or while work items are executing
	 * on it, so the retrying is guaranteed to make forward-progress.
	 */
	/*
	 * RCU 只保住旧 pwq 内存，refcnt=0 表示逻辑死亡、不能再接新 work。unbound 映射
	 * 在其死亡前已换成新 pwq，故解锁重读最终会前进；per-CPU 出现零引用是内部错误。
	 */
	if (unlikely(!pwq->refcnt)) {
		if (wq->flags & WQ_UNBOUND) {
			raw_spin_unlock(&pool->lock);
			cpu_relax();
			goto retry;
		}
		/* oops */
		WARN_ONCE(true, "workqueue: per-cpu pwq for %s on cpu%d has 0 refcnt",
			  wq->name, cpu);
	}

	/* pwq determined, queue */
	/* 从这里起 pwq/pool 已锁定且可接收 work，trace 记录最终落点而非最初候选。 */
	trace_workqueue_queue_work(req_cpu, pwq, work);

	if (WARN_ON(!list_empty(&work->entry)))
		goto out;

	pwq->nr_in_flight[pwq->work_color]++;
	work_flags = work_color_to_flags(pwq->work_color);

	/*
	 * Limit the number of concurrently active work items to max_active.
	 * @work must also queue behind existing inactive work items to maintain
	 * ordering when max_active changes. See wq_adjust_max_active().
	 */
	/*
	 * 即使当前计数低于上限，只要已有 inactive，也必须排其后，防止 max_active 调整
	 * 时后来 work 越过先前受限 work。active 分支计名额并进 pool，失败则带 INACTIVE
	 * 进 pwq 私链等待完成/扩容路径激活。
	 */
	if (list_empty(&pwq->inactive_works) && pwq_tryinc_nr_active(pwq, false)) {
		if (list_empty(&pool->worklist))
			pool->last_progress_ts = jiffies;

		trace_workqueue_activate_work(work);
		insert_work(pwq, work, &pool->worklist, work_flags);
		kick_pool(pool);
	} else {
		work_flags |= WORK_STRUCT_INACTIVE;
		insert_work(pwq, work, &pwq->inactive_works, work_flags);
	}

out:
	raw_spin_unlock(&pool->lock);
	rcu_read_unlock();
}

/*
 * clear_pending_if_disabled() - 新 queue 取得 PENDING 后检查 work 是否被禁用。
 * @work 为 owned、off-queue 对象。若仍是 PWQ 排队态或 disable 深度为零返回 false；
 * 若禁用则保留 pool/flags、清 PENDING 并返回 true，使本次 queue 报成功仲裁失败且
 * 不实际入队。无睡眠，调用者保持 IRQ 关闭。
 */
static bool clear_pending_if_disabled(struct work_struct *work)
{
	unsigned long data = *work_data_bits(work);
	struct work_offq_data offqd;

	if (likely((data & WORK_STRUCT_PWQ) ||
		   !(data & WORK_OFFQ_DISABLE_MASK)))
		return false;

	work_offqd_unpack(&offqd, data);
	set_work_pool_and_clear_pending(work, offqd.pool_id,
					work_offqd_pack_flags(&offqd));
	return true;
}

/**
 * queue_work_on - queue work on specific cpu
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @work: work to queue
 *
 * We queue the work to a specific CPU, the caller must ensure it
 * can't go away.  Callers that fail to ensure that the specified
 * CPU cannot go away will execute on a randomly chosen CPU.
 * But note well that callers specifying a CPU that never has been
 * online will get a splat.
 *
 * Return: %false if @work was already on a queue, %true otherwise.
 */
/*
 * queue_work_on() - 尝试把 idle work 排到指定 CPU 对应的 wq。
 * @cpu 需由调用者通过 hotplug 保护保证可用；否则可能退化到其他 CPU，从未上线的
 * CPU 会告警。@wq/@work 均为借用，work 存储必须持续到完成/取消。
 * 原子取得 PENDING 且 work 未禁用时调用 __queue_work()，返回 true；已 pending 或
 * disabled 返回 false。返回 true 只表示成功入队，不表示回调已开始/完成。
 */
bool queue_work_on(int cpu, struct workqueue_struct *wq,
		   struct work_struct *work)
{
	bool ret = false;
	unsigned long irq_flags;

	local_irq_save(irq_flags);

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)) &&
	    !clear_pending_if_disabled(work)) {
		__queue_work(cpu, wq, work);
		ret = true;
	}

	local_irq_restore(irq_flags);
	return ret;
}
EXPORT_SYMBOL(queue_work_on);

/**
 * select_numa_node_cpu - Select a CPU based on NUMA node
 * @node: NUMA node ID that we want to select a CPU from
 *
 * This function will attempt to find a "random" cpu available on a given
 * node. If there are no CPUs available on the given node it will return
 * WORK_CPU_UNBOUND indicating that we should just schedule to any
 * available CPU if we need to schedule this work.
 */
/*
 * select_numa_node_cpu() - 为 NUMA node 选择 best-effort CPU。
 * @node 无效/离线时返回 WORK_CPU_UNBOUND；当前 CPU 已在该 node 时返回当前 CPU，
 * 否则返回该 node 与 online mask 的第一个交集，交集空也返回 UNBOUND。纯选择，
 * 不取得 hotplug 引用，所以最终执行节点仍非硬保证。
 */
static int select_numa_node_cpu(int node)
{
	int cpu;

	/* Delay binding to CPU if node is not valid or online */
	/* 无有效 node 时把决定推迟给普通 unbound 选择，而不是构造非法 percpu 指针。 */
	if (node < 0 || node >= MAX_NUMNODES || !node_online(node))
		return WORK_CPU_UNBOUND;

	/* Use local node/cpu if we are already there */
	cpu = raw_smp_processor_id();
	if (node == cpu_to_node(cpu))
		return cpu;

	/* Use "random" otherwise know as "first" online CPU of node */
	cpu = cpumask_any_and(cpumask_of_node(node), cpu_online_mask);

	/* If CPU is valid return that, otherwise just defer */
	return cpu < nr_cpu_ids ? cpu : WORK_CPU_UNBOUND;
}

/**
 * queue_work_node - queue work on a "random" cpu for a given NUMA node
 * @node: NUMA node that we are targeting the work for
 * @wq: workqueue to use
 * @work: work to queue
 *
 * We queue the work to a "random" CPU within a given NUMA node. The basic
 * idea here is to provide a way to somehow associate work with a given
 * NUMA node.
 *
 * This function will only make a best effort attempt at getting this onto
 * the right NUMA node. If no node is requested or the requested node is
 * offline then we just fall back to standard queue_work behavior.
 *
 * Currently the "random" CPU ends up being the first available CPU in the
 * intersection of cpu_online_mask and the cpumask of the node, unless we
 * are running on the node. In that case we just use the current CPU.
 *
 * Return: %false if @work was already on a queue, %true otherwise.
 */
/*
 * queue_work_node() - best-effort 把 work 关联到指定 NUMA node 的 unbound pwq。
 * @node 可无效/离线并退化；@wq 应带 WQ_UNBOUND；@work 为调用者拥有的 idle/pending
 * 对象。返回语义与 queue_work_on() 相同。当前实现选本 node 当前 CPU或第一个在线
 * CPU，并不在 node 内轮转，因此这是局部性提示，不是执行位置契约。
 */
bool queue_work_node(int node, struct workqueue_struct *wq,
		     struct work_struct *work)
{
	unsigned long irq_flags;
	bool ret = false;

	/*
	 * This current implementation is specific to unbound workqueues.
	 * Specifically we only return the first available CPU for a given
	 * node instead of cycling through individual CPUs within the node.
	 *
	 * If this is used with a per-cpu workqueue then the logic in
	 * workqueue_select_cpu_near would need to be updated to allow for
	 * some round robin type logic.
	 */
	/* per-CPU wq 会把选择结果解释成硬 CPU，缺少 node 内轮转，因此仅告警支持误用。 */
	WARN_ON_ONCE(!(wq->flags & WQ_UNBOUND));

	local_irq_save(irq_flags);

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)) &&
	    !clear_pending_if_disabled(work)) {
		int cpu = select_numa_node_cpu(node);

		__queue_work(cpu, wq, work);
		ret = true;
	}

	local_irq_restore(irq_flags);
	return ret;
}
EXPORT_SYMBOL_GPL(queue_work_node);

/*
 * delayed_work_timer_fn() - delayed_work timer 到期时把内嵌 work 交给目标 wq。
 * @t 为 irqsafe timer，container_of 恢复调用者仍持有的 dwork；dwork->wq/cpu 已在
 * 装 timer 前保存。timer 持有 PENDING ownership，IRQ 已关闭，故可直接调用
 * __queue_work()。返回无；执行后 ownership 转给 workqueue。
 */
void delayed_work_timer_fn(struct timer_list *t)
{
	struct delayed_work *dwork = timer_container_of(dwork, t, timer);

	/* should have been called from irqsafe timer with irq already off */
	__queue_work(dwork->cpu, dwork->wq, &dwork->work);
}
EXPORT_SYMBOL(delayed_work_timer_fn);

/*
 * __queue_delayed_work() - 在已取得 PENDING/关 IRQ 后立即入队或安装延时 timer。
 * @cpu/@wq/@dwork 为未来执行目标，@delay 单位 jiffies。delay=0 必须直接入队：
 * timer 最早也要下一 tick，API 则承诺零延时没有这段额外等待。非零时保存 wq/cpu/
 * expires，再把 timer 放到 housekeeping CPU、global timer 或指定 CPU。
 * 返回无；成功后 PENDING ownership 由 timer 持有，timer 到期再转给 workqueue。
 */
static void __queue_delayed_work(int cpu, struct workqueue_struct *wq,
				struct delayed_work *dwork, unsigned long delay)
{
	struct timer_list *timer = &dwork->timer;
	struct work_struct *work = &dwork->work;

	WARN_ON_ONCE(timer->function != delayed_work_timer_fn);
	WARN_ON_ONCE(timer_pending(timer));
	WARN_ON_ONCE(!list_empty(&work->entry));

	/*
	 * If @delay is 0, queue @dwork->work immediately.  This is for
	 * both optimization and correctness.  The earliest @timer can
	 * expire is on the closest next tick and delayed_work users depend
	 * on that there's no such delay when @delay is 0.
	 */
	/* 零延时既是快路径也是可观察语义，不能统一走 timer。 */
	if (!delay) {
		__queue_work(cpu, wq, &dwork->work);
		return;
	}

	WARN_ON_ONCE(cpu != WORK_CPU_UNBOUND && !cpu_online(cpu));
	dwork->wq = wq;
	dwork->cpu = cpu;
	timer->expires = jiffies + delay;

	if (housekeeping_enabled(HK_TYPE_TIMER)) {
		/* If the current cpu is a housekeeping cpu, use it. */
		/* 隔离系统把 timer 放到 housekeeping CPU，避免打扰 nohz_full/隔离 CPU。 */
		cpu = smp_processor_id();
		if (!housekeeping_test_cpu(cpu, HK_TYPE_TIMER))
			cpu = housekeeping_any_cpu(HK_TYPE_TIMER);
		add_timer_on(timer, cpu);
	} else {
		if (likely(cpu == WORK_CPU_UNBOUND))
			add_timer_global(timer);
		else
			add_timer_on(timer, cpu);
	}
}

/**
 * queue_delayed_work_on - queue work on specific CPU after delay
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @dwork: work to queue
 * @delay: number of jiffies to wait before queueing
 *
 * We queue the delayed_work to a specific CPU, for non-zero delays the
 * caller must ensure it is online and can't go away. Callers that fail
 * to ensure this, may get @dwork->timer queued to an offlined CPU and
 * this will prevent queueing of @dwork->work unless the offlined CPU
 * becomes online again.
 *
 * Return: %false if @work was already on a queue, %true otherwise.  If
 * @delay is zero and @dwork is idle, it will be scheduled for immediate
 * execution.
 */
/*
 * queue_delayed_work_on() - 在指定 CPU 上延后 @delay jiffies 再排 work。
 * 非零 delay 时调用者必须保证 CPU 在线且不会离线，否则 timer 可滞留到 CPU 再上线。
 * @wq/dwork 借用但对象必须存活到 timer+work 完成。返回 false 表示已 pending 或被
 * disable，true 表示 timer/立即入队 ownership 已建立；不表示回调完成。
 */
bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
			   struct delayed_work *dwork, unsigned long delay)
{
	struct work_struct *work = &dwork->work;
	bool ret = false;
	unsigned long irq_flags;

	/* read the comment in __queue_work() */
	local_irq_save(irq_flags);

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)) &&
	    !clear_pending_if_disabled(work)) {
		__queue_delayed_work(cpu, wq, dwork, delay);
		ret = true;
	}

	local_irq_restore(irq_flags);
	return ret;
}
EXPORT_SYMBOL(queue_delayed_work_on);

/**
 * mod_delayed_work_on - modify delay of or queue a delayed work on specific CPU
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @dwork: work to queue
 * @delay: number of jiffies to wait before queueing
 *
 * If @dwork is idle, equivalent to queue_delayed_work_on(); otherwise,
 * modify @dwork's timer so that it expires after @delay.  If @delay is
 * zero, @work is guaranteed to be scheduled immediately regardless of its
 * current state.
 *
 * Return: %false if @dwork was idle and queued, %true if @dwork was
 * pending and its timer was modified.
 *
 * This function is safe to call from any context including IRQ handler.
 * See try_to_grab_pending() for details.
 */
bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq,
			 struct delayed_work *dwork, unsigned long delay)
{
	unsigned long irq_flags;
	bool ret;

	ret = work_grab_pending(&dwork->work, WORK_CANCEL_DELAYED, &irq_flags);

	if (!clear_pending_if_disabled(&dwork->work))
		__queue_delayed_work(cpu, wq, dwork, delay);

	local_irq_restore(irq_flags);
	return ret;
}
EXPORT_SYMBOL_GPL(mod_delayed_work_on);

/*
 * rcu_work_rcufn() - RCU 宽限期结束后把 rcu_work 的内嵌 work 入队。
 * @rcu 恢复仍由调用者保持存活的 rwork；call_rcu 持有 pending 生命周期。返回无，
 * 在 RCU callback/原子上下文运行，通过 queue_work() 把执行转到进程上下文。
 */
static void rcu_work_rcufn(struct rcu_head *rcu)
{
	struct rcu_work *rwork = container_of(rcu, struct rcu_work, rcu);

	/* read the comment in __queue_work() */
	local_irq_disable();
	__queue_work(WORK_CPU_UNBOUND, rwork->wq, &rwork->work);
	local_irq_enable();
}

/**
 * queue_rcu_work - queue work after a RCU grace period
 * @wq: workqueue to use
 * @rwork: work to queue
 *
 * Return: %false if @rwork was already pending, %true otherwise.  Note
 * that a full RCU grace period is guaranteed only after a %true return.
 * While @rwork is guaranteed to be executed after a %false return, the
 * execution may happen before a full RCU grace period has passed.
 */
/*
 * queue_rcu_work() - 保证至少跨过一个 RCU grace period 后再执行 work。
 * @wq/rwork 均为借用且须活到完成；成功取得 PENDING 后 call_rcu 并返回 true，
 * 已 pending 返回 false。true 只表示 callback 已挂起；回调实际执行还要再经 wq。
 */
bool queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rwork)
{
	struct work_struct *work = &rwork->work;

	/*
	 * rcu_work can't be canceled or disabled. Warn if the user reached
	 * inside @rwork and disabled the inner work.
	 */
	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)) &&
	    !WARN_ON_ONCE(clear_pending_if_disabled(work))) {
		rwork->wq = wq;
		call_rcu_hurry(&rwork->rcu, rcu_work_rcufn);
		return true;
	}

	return false;
}
EXPORT_SYMBOL(queue_rcu_work);

/*
 * alloc_worker() - 在 @node 分配并初始化尚未附着线程的 worker 元数据。
 * 可睡眠；成功返回由调用者拥有的 worker，失败 NULL。只初始化链表/锁/节点字段，
 * 尚未加入 pool、没有 task，失败清理由调用者负责。
 */
static struct worker *alloc_worker(int node)
{
	struct worker *worker;

	worker = kzalloc_node(sizeof(*worker), GFP_KERNEL, node);
	if (worker) {
		INIT_LIST_HEAD(&worker->entry);
		INIT_LIST_HEAD(&worker->scheduled);
		INIT_LIST_HEAD(&worker->node);
		/* on creation a worker is in !idle && prep state */
		worker->flags = WORKER_PREP;
	}
	return worker;
}

/*
 * pool_allowed_cpus() - 返回创建/绑定 worker 时使用的允许 CPU mask。
 * @pool 借用；bound 在线池通常用单 CPU mask，disassociated/unbound 用 attrs mask。
 * 返回借用 mask，不取得引用、不修改 affinity。
 */
static cpumask_t *pool_allowed_cpus(struct worker_pool *pool)
{
	if (pool->cpu < 0 && pool->attrs->affn_strict)
		return pool->attrs->__pod_cpumask;
	else
		return pool->attrs->cpumask;
}

/**
 * worker_attach_to_pool() - attach a worker to a pool
 * @worker: worker to be attached
 * @pool: the target pool
 *
 * Attach @worker to @pool.  Once attached, the %WORKER_UNBOUND flag and
 * cpu-binding of @worker are kept coordinated with the pool across
 * cpu-[un]hotplugs.
 */
/*
 * worker_attach_to_pool() - 在 attach_mutex 下把新 worker/task 纳入 pool。
 * @worker 输入输出，@pool 目标；设置 worker->pool、task affinity/flags 并加入 workers
 * 链。返回无，成功后 pool 管理 worker 生命周期；调用可睡眠且不得与 detach 并发。
 */
static void worker_attach_to_pool(struct worker *worker,
				  struct worker_pool *pool)
{
	mutex_lock(&wq_pool_attach_mutex);

	/*
	 * The wq_pool_attach_mutex ensures %POOL_DISASSOCIATED remains stable
	 * across this function. See the comments above the flag definition for
	 * details. BH workers are, while per-CPU, always DISASSOCIATED.
	 */
	if (pool->flags & POOL_DISASSOCIATED) {
		worker->flags |= WORKER_UNBOUND;
	} else {
		WARN_ON_ONCE(pool->flags & POOL_BH);
		kthread_set_per_cpu(worker->task, pool->cpu);
	}

	if (worker->rescue_wq)
		set_cpus_allowed_ptr(worker->task, pool_allowed_cpus(pool));

	list_add_tail(&worker->node, &pool->workers);
	worker->pool = pool;

	mutex_unlock(&wq_pool_attach_mutex);
}

/*
 * unbind_worker() - 把 worker 标成 UNBOUND 并放宽 affinity，供 CPU 离线期间继续执行。
 * @worker 入口持 attach_mutex 与相应同步；返回无。它不从 pool 摘除，只改变调度绑定
 * 和 nr_running 参与规则。
 */
static void unbind_worker(struct worker *worker)
{
	lockdep_assert_held(&wq_pool_attach_mutex);

	kthread_set_per_cpu(worker->task, -1);
	if (cpumask_intersects(wq_unbound_cpumask, cpu_active_mask))
		WARN_ON_ONCE(set_cpus_allowed_ptr(worker->task, wq_unbound_cpumask) < 0);
	else
		WARN_ON_ONCE(set_cpus_allowed_ptr(worker->task, cpu_possible_mask) < 0);
}


/* detach_worker() 清 worker→pool 运行关联；入口已建立停机条件，返回无且不释放内存。 */
static void detach_worker(struct worker *worker)
{
	lockdep_assert_held(&wq_pool_attach_mutex);

	unbind_worker(worker);
	list_del(&worker->node);
}

/**
 * worker_detach_from_pool() - detach a worker from its pool
 * @worker: worker which is attached to its pool
 *
 * Undo the attaching which had been done in worker_attach_to_pool().  The
 * caller worker shouldn't access to the pool after detached except it has
 * other reference to the pool.
 */
/*
 * worker_detach_from_pool() - 在 attach_mutex 下把已停止 worker 从 pool workers 链摘除。
 * 返回无；摘除后不再可由 pool 遍历，worker 内存仍由后续 reap 路径释放。
 */
static void worker_detach_from_pool(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	/* there is one permanent BH worker per CPU which should never detach */
	WARN_ON_ONCE(pool->flags & POOL_BH);

	mutex_lock(&wq_pool_attach_mutex);
	detach_worker(worker);
	worker->pool = NULL;
	mutex_unlock(&wq_pool_attach_mutex);

	/* clear leftover flags without pool->lock after it is detached */
	worker->flags &= ~(WORKER_UNBOUND | WORKER_REBOUND);
}

/*
 * format_worker_id() - 根据 pool/worker/rescuer 属性生成 kworker comm 身份。
 * @buf/@size 为输出缓冲，@worker 借用，@desc 可空；返回 snprintf 风格长度。
 * 只格式化诊断名称，不改变调度或 worker ownership。
 */
static int format_worker_id(char *buf, size_t size, struct worker *worker,
			    struct worker_pool *pool)
{
	if (worker->rescue_wq)
		return scnprintf(buf, size, "kworker/R-%s",
				 worker->rescue_wq->name);

	if (pool) {
		if (pool->cpu >= 0)
			return scnprintf(buf, size, "kworker/%d:%d%s",
					 pool->cpu, worker->id,
					 pool->attrs->nice < 0  ? "H" : "");
		else
			return scnprintf(buf, size, "kworker/u%d:%d",
					 pool->id, worker->id);
	} else {
		return scnprintf(buf, size, "kworker/dying");
	}
}

/**
 * create_worker - create a new workqueue worker
 * @pool: pool the new worker will belong to
 *
 * Create and start a new worker which is attached to @pool.
 *
 * CONTEXT:
 * Might sleep.  Does GFP_KERNEL allocations.
 *
 * Return:
 * Pointer to the newly created worker.
 */
/*
 * create_worker() - 为 pool 分配 worker 元数据、ID 和 kthread，并以 idle 状态发布。
 * @pool 在管理路径中稳定；函数可睡眠。成功返回 pool 已拥有的 worker 借用指针；
 * 任一步失败返回 NULL 并逆序释放 task/ID/metadata。线程在完全 attach/idle 后才
 * 唤醒，避免观察半初始化 pool 关系。
 */
static struct worker *create_worker(struct worker_pool *pool)
{
	struct worker *worker;
	int id;

	/* ID is needed to determine kthread name */
	id = ida_alloc(&pool->worker_ida, GFP_KERNEL);
	if (id < 0) {
		pr_err_once("workqueue: Failed to allocate a worker ID: %pe\n",
			    ERR_PTR(id));
		return NULL;
	}

	worker = alloc_worker(pool->node);
	if (!worker) {
		pr_err_once("workqueue: Failed to allocate a worker\n");
		goto fail;
	}

	worker->id = id;

	if (!(pool->flags & POOL_BH)) {
		char id_buf[WORKER_ID_LEN];

		format_worker_id(id_buf, sizeof(id_buf), worker, pool);
		worker->task = kthread_create_on_node(worker_thread, worker,
						      pool->node, "%s", id_buf);
		if (IS_ERR(worker->task)) {
			if (PTR_ERR(worker->task) == -EINTR) {
				pr_err("workqueue: Interrupted when creating a worker thread \"%s\"\n",
				       id_buf);
			} else {
				pr_err_once("workqueue: Failed to create a worker thread: %pe",
					    worker->task);
			}
			goto fail;
		}

		set_user_nice(worker->task, pool->attrs->nice);
		kthread_bind_mask(worker->task, pool_allowed_cpus(pool));
	}

	/* successful, attach the worker to the pool */
	worker_attach_to_pool(worker, pool);

	/* start the newly created worker */
	raw_spin_lock_irq(&pool->lock);

	worker->pool->nr_workers++;
	worker_enter_idle(worker);

	/*
	 * @worker is waiting on a completion in kthread() and will trigger hung
	 * check if not woken up soon. As kick_pool() is noop if @pool is empty,
	 * wake it up explicitly.
	 */
	if (worker->task)
		wake_up_process(worker->task);

	raw_spin_unlock_irq(&pool->lock);

	return worker;

fail:
	ida_free(&pool->worker_ida, id);
	kfree(worker);
	return NULL;
}

/* detach_dying_workers() 把 cull_list 中已标 DIE 的 worker 从各 pool 归属摘除；不 reap task。 */
static void detach_dying_workers(struct list_head *cull_list)
{
	struct worker *worker;

	list_for_each_entry(worker, cull_list, entry)
		detach_worker(worker);
}

/*
 * reap_dying_workers() - 在不持 pool 热锁时 stop/join 并释放 cull_list 的 worker。
 * 可睡眠；返回无。detach 与 reap 分阶段避免在 spinlock/attach 临界区等待线程退出。
 */
static void reap_dying_workers(struct list_head *cull_list)
{
	struct worker *worker, *tmp;

	list_for_each_entry_safe(worker, tmp, cull_list, entry) {
		list_del_init(&worker->entry);
		kthread_stop_put(worker->task);
		kfree(worker);
	}
}

/**
 * set_worker_dying - Tag a worker for destruction
 * @worker: worker to be destroyed
 * @list: transfer worker away from its pool->idle_list and into list
 *
 * Tag @worker for destruction and adjust @pool stats accordingly.  The worker
 * should be idle.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
/*
 * set_worker_dying() - 在 pool lock 下把 idle worker 从可用集合转入待回收链。
 * 设置 DIE、更新 nr_workers/nr_idle 并转移链表 ownership；返回无，真正停止由 reap。
 */
static void set_worker_dying(struct worker *worker, struct list_head *list)
{
	struct worker_pool *pool = worker->pool;

	lockdep_assert_held(&pool->lock);
	lockdep_assert_held(&wq_pool_attach_mutex);

	/* sanity check frenzy */
	if (WARN_ON(worker->current_work) ||
	    WARN_ON(!list_empty(&worker->scheduled)) ||
	    WARN_ON(!(worker->flags & WORKER_IDLE)))
		return;

	pool->nr_workers--;
	pool->nr_idle--;

	worker->flags |= WORKER_DIE;

	list_move(&worker->entry, list);

	/* get an extra task struct reference for later kthread_stop_put() */
	get_task_struct(worker->task);
}

/**
 * idle_worker_timeout - check if some idle workers can now be deleted.
 * @t: The pool's idle_timer that just expired
 *
 * The timer is armed in worker_enter_idle(). Note that it isn't disarmed in
 * worker_leave_idle(), as a worker flicking between idle and active while its
 * pool is at the too_many_workers() tipping point would cause too much timer
 * housekeeping overhead. Since IDLE_WORKER_TIMEOUT is long enough, we just let
 * it expire and re-evaluate things from there.
 */
/*
 * idle_worker_timeout() - idle 裁剪 timer 到期时选择超过保留期的老 worker。
 * timer/原子上下文只在 pool lock 下标记并安排 idle_cull_work，不直接等待/释放线程。
 */
static void idle_worker_timeout(struct timer_list *t)
{
	struct worker_pool *pool = timer_container_of(pool, t, idle_timer);
	bool do_cull = false;

	if (work_pending(&pool->idle_cull_work))
		return;

	raw_spin_lock_irq(&pool->lock);

	if (too_many_workers(pool)) {
		struct worker *worker;
		unsigned long expires;

		/* idle_list is kept in LIFO order, check the last one */
		worker = list_last_entry(&pool->idle_list, struct worker, entry);
		expires = worker->last_active + IDLE_WORKER_TIMEOUT;
		do_cull = !time_before(jiffies, expires);

		if (!do_cull)
			mod_timer(&pool->idle_timer, expires);
	}
	raw_spin_unlock_irq(&pool->lock);

	if (do_cull)
		queue_work(system_dfl_wq, &pool->idle_cull_work);
}

/**
 * idle_cull_fn - cull workers that have been idle for too long.
 * @work: the pool's work for handling these idle workers
 *
 * This goes through a pool's idle workers and gets rid of those that have been
 * idle for at least IDLE_WORKER_TIMEOUT seconds.
 *
 * We don't want to disturb isolated CPUs because of a pcpu kworker being
 * culled, so this also resets worker affinity. This requires a sleepable
 * context, hence the split between timer callback and work item.
 */
/*
 * idle_cull_fn() - 进程上下文完成已标记 idle worker 的 detach 与 reap。
 * @work 恢复 pool->idle_cull_work；可睡眠。返回无，执行后 cull_list 全部释放。
 */
static void idle_cull_fn(struct work_struct *work)
{
	struct worker_pool *pool = container_of(work, struct worker_pool, idle_cull_work);
	LIST_HEAD(cull_list);

	/*
	 * Grabbing wq_pool_attach_mutex here ensures an already-running worker
	 * cannot proceed beyong set_pf_worker() in its self-destruct path.
	 * This is required as a previously-preempted worker could run after
	 * set_worker_dying() has happened but before detach_dying_workers() did.
	 */
	mutex_lock(&wq_pool_attach_mutex);
	raw_spin_lock_irq(&pool->lock);

	while (too_many_workers(pool)) {
		struct worker *worker;
		unsigned long expires;

		worker = list_last_entry(&pool->idle_list, struct worker, entry);
		expires = worker->last_active + IDLE_WORKER_TIMEOUT;

		if (time_before(jiffies, expires)) {
			mod_timer(&pool->idle_timer, expires);
			break;
		}

		set_worker_dying(worker, &cull_list);
	}

	raw_spin_unlock_irq(&pool->lock);
	detach_dying_workers(&cull_list);
	mutex_unlock(&wq_pool_attach_mutex);

	reap_dying_workers(&cull_list);
}

/*
 * send_mayday() - 把缺 worker 前进能力的 pwq 加入其 wq->maydays 并唤醒 rescuer。
 * 入口持 pool lock，内部用 wq_mayday_lock 去重；返回无。只有配置 WQ_MEM_RECLAIM
 * 的 wq 有 rescuer，普通 wq 的资源枯竭不获得这项保证。
 */
static void send_mayday(struct pool_workqueue *pwq)
{
	struct workqueue_struct *wq = pwq->wq;

	lockdep_assert_held(&wq_mayday_lock);

	if (!wq->rescuer)
		return;

	/* mayday mayday mayday */
	if (list_empty(&pwq->mayday_node)) {
		/*
		 * If @pwq is for an unbound wq, its base ref may be put at
		 * any time due to an attribute change.  Pin @pwq until the
		 * rescuer is done with it.
		 */
		get_pwq(pwq);
		list_add_tail(&pwq->mayday_node, &wq->maydays);
		wake_up_process(wq->rescuer->task);
		pwq->stats[PWQ_STAT_MAYDAY]++;
	}
}

/*
 * pool_mayday_timeout() - worker 创建迟迟不成功时扫描 pool worklist 发出周期求救。
 * timer 上下文、持 pool lock；为相关 pwq 调 send_mayday，若仍需 worker 则重装 timer。
 */
static void pool_mayday_timeout(struct timer_list *t)
{
	struct worker_pool *pool = timer_container_of(pool, t, mayday_timer);
	struct work_struct *work;

	raw_spin_lock_irq(&pool->lock);
	raw_spin_lock(&wq_mayday_lock);		/* for wq->maydays */

	if (need_to_create_worker(pool)) {
		/*
		 * We've been trying to create a new worker but
		 * haven't been successful.  We might be hitting an
		 * allocation deadlock.  Send distress signals to
		 * rescuers.
		 */
		list_for_each_entry(work, &pool->worklist, entry)
			send_mayday(get_work_pwq(work));
	}

	raw_spin_unlock(&wq_mayday_lock);
	raw_spin_unlock_irq(&pool->lock);

	mod_timer(&pool->mayday_timer, jiffies + MAYDAY_INTERVAL);
}

/**
 * maybe_create_worker - create a new worker if necessary
 * @pool: pool to create a new worker for
 *
 * Create a new worker for @pool if necessary.  @pool is guaranteed to
 * have at least one idle worker on return from this function.  If
 * creating a new worker takes longer than MAYDAY_INTERVAL, mayday is
 * sent to all rescuers with works scheduled on @pool to resolve
 * possible allocation deadlock.
 *
 * On return, need_to_create_worker() is guaranteed to be %false and
 * may_start_working() %true.
 *
 * LOCKING:
 * raw_spin_lock_irq(pool->lock) which may be released and regrabbed
 * multiple times.  Does GFP_KERNEL allocations.  Called only from
 * manager.
 */
/*
 * maybe_create_worker() - manager 在确实缺 worker 时暂时释放 pool lock 创建线程。
 * @pool 入口/出口都持 lock（sparse 注解明确 release/acquire），函数可睡眠。
 * 创建前启动 mayday timer；失败按 CREATE_COOLDOWN 等待再重试，期间若其他路径已
 * 解决短缺则停止。返回无，保证锁状态恢复。
 */
static void maybe_create_worker(struct worker_pool *pool)
__releases(&pool->lock)
__acquires(&pool->lock)
{
restart:
	raw_spin_unlock_irq(&pool->lock);

	/* if we don't make progress in MAYDAY_INITIAL_TIMEOUT, call for help */
	mod_timer(&pool->mayday_timer, jiffies + MAYDAY_INITIAL_TIMEOUT);

	while (true) {
		if (create_worker(pool) || !need_to_create_worker(pool))
			break;

		schedule_timeout_interruptible(CREATE_COOLDOWN);

		if (!need_to_create_worker(pool))
			break;
	}

	timer_delete_sync(&pool->mayday_timer);
	raw_spin_lock_irq(&pool->lock);
	/*
	 * This is necessary even after a new worker was just successfully
	 * created as @pool->lock was dropped and the new worker might have
	 * already become busy.
	 */
	if (need_to_create_worker(pool))
		goto restart;
}

#ifdef CONFIG_PREEMPT_RT
/* RT 下回调锁钩子在执行 work 前取得 pool->cb_lock，串行化 cancel 与 BH 回调。 */
static void worker_lock_callback(struct worker_pool *pool)
{
	spin_lock(&pool->cb_lock);
}

/* worker_unlock_callback() 与上述钩子配对释放 cb_lock；返回无。 */
static void worker_unlock_callback(struct worker_pool *pool)
{
	spin_unlock(&pool->cb_lock);
}

/* cancel wait 钩子在 PREEMPT_RT 上与正在执行的 BH callback 通过 cb_lock 同步。 */
static void workqueue_callback_cancel_wait_running(struct worker_pool *pool)
{
	spin_lock(&pool->cb_lock);
	spin_unlock(&pool->cb_lock);
}

#else

static void worker_lock_callback(struct worker_pool *pool) { }
static void worker_unlock_callback(struct worker_pool *pool) { }
static void workqueue_callback_cancel_wait_running(struct worker_pool *pool) { }

#endif

/**
 * manage_workers - manage worker pool
 * @worker: self
 *
 * Assume the manager role and manage the worker pool @worker belongs
 * to.  At any given time, there can be only zero or one manager per
 * pool.  The exclusion is handled automatically by this function.
 *
 * The caller can safely start processing works on false return.  On
 * true return, it's guaranteed that need_to_create_worker() is false
 * and may_start_working() is true.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock) which may be released and regrabbed
 * multiple times.  Does GFP_KERNEL allocations.
 *
 * Return:
 * %false if the pool doesn't need management and the caller can safely
 * start processing works, %true if management function was performed and
 * the conditions that the caller verified before calling the function may
 * no longer be true.
 */
/*
 * manage_workers() - 让一个 worker 独占 pool manager 角色并补足线程。
 * @worker 为当前线程且入口持 pool lock。已有 manager 时放弃并返回 false；成功成为
 * manager 后可释放锁创建 worker，结束时清 MANAGER_ACTIVE、唤醒等待者并返回 true。
 * 角色只保证一次管理者，不表示 worker 离开 pool。
 */
static bool manage_workers(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (pool->flags & POOL_MANAGER_ACTIVE)
		return false;

	pool->flags |= POOL_MANAGER_ACTIVE;
	pool->manager = worker;

	maybe_create_worker(pool);

	pool->manager = NULL;
	pool->flags &= ~POOL_MANAGER_ACTIVE;
	rcuwait_wake_up(&manager_wait);
	return true;
}

/**
 * process_one_work - process single work
 * @worker: self
 * @work: work to process
 *
 * Process @work.  This function contains all the logics necessary to
 * process a single work including synchronization against and
 * interaction with other workers on the same cpu, queueing and
 * flushing.  As long as context requirement is met, any worker can
 * call this function to process a work.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock) which is released and regrabbed.
 */
/*
 * process_one_work() - 执行一个已分配 work 的完整状态机。
 *
 * @worker：当前 kworker；@work：位于 worker->scheduled 的 owned work。入口/出口
 * 均持 pool lock，但调用回调期间释放；函数可睡眠取决于 work->func。阶段为：
 * 从 scheduled 摘除并保存 work_data/pwq → busy_hash 发布“正在执行” → 清 PENDING
 * 完成交接 → 设置 current_* 与锁依赖 → 解锁调用 func → 重新加锁清执行身份 →
 * 归还 CPU-intensive/active/in-flight/pwq 账。无直接返回值。
 *
 * 同一 pool 的再次入队会在 assign_work() 检测 busy_hash 并排到本 worker 后面，
 * 保证同一 work 非重入。回调可能释放 work 自身，所以调用后不得再解引用 @work，
 * 只能使用事先保存的 pwq/data/func。
 */
static void process_one_work(struct worker *worker, struct work_struct *work)
__releases(&pool->lock)
__acquires(&pool->lock)
{
	struct pool_workqueue *pwq = get_work_pwq(work);
	struct worker_pool *pool = worker->pool;
	unsigned long work_data;
	int lockdep_start_depth, rcu_start_depth;
	bool bh_draining = pool->flags & POOL_BH_DRAINING;
#ifdef CONFIG_LOCKDEP
	/*
	 * It is permissible to free the struct work_struct from
	 * inside the function that is called from it, this we need to
	 * take into account for lockdep too.  To avoid bogus "held
	 * lock freed" warnings as well as problems when looking into
	 * work->lockdep_map, make a copy and use that here.
	 */
	struct lockdep_map lockdep_map;

	lockdep_copy_map(&lockdep_map, &work->lockdep_map);
#endif
	/* ensure we're on the correct CPU */
	/* bound pool 的回调必须仍在关联 CPU；disassociated/unbound 才允许跨 CPU。 */
	WARN_ON_ONCE(!(pool->flags & POOL_DISASSOCIATED) &&
		     raw_smp_processor_id() != pool->cpu);

	/* claim and dequeue */
	/*
	 * 阶段 1：发布执行身份。先从 debug pending 状态退出，把 worker 插入 busy_hash，
	 * 再保存 func/pwq/color/起始时间。之后同 pool 新入队可找到本 worker 并串行追加。
	 */
	debug_work_deactivate(work);
	hash_add(pool->busy_hash, &worker->hentry, (unsigned long)work);
	worker->current_work = work;
	worker->current_func = work->func;
	worker->current_pwq = pwq;
	if (worker->task)
		worker->current_at = worker->task->se.sum_exec_runtime;
	worker->current_start = jiffies;
	work_data = *work_data_bits(work);
	worker->current_color = get_work_color(work_data);

	/*
	 * Record wq name for cmdline and debug reporting, may get
	 * overridden through set_worker_desc().
	 */
	/* 默认描述使用 wq 名，回调可用 set_worker_desc() 覆盖为更具体的阻塞原因。 */
	strscpy(worker->desc, pwq->wq->name, WORKER_DESC_LEN);

	list_del_init(&work->entry);

	/*
	 * CPU intensive works don't participate in concurrency management.
	 * They're the scheduler's responsibility.  This takes @worker out
	 * of concurrency management and the next code block will chain
	 * execution of the pending work items.
	 */
	/*
	 * WQ_CPU_INTENSIVE 回调交给 scheduler 公平性而不占普通并发名额；设置状态后下面
	 * kick 另一 idle worker，使同池短 work 不被长回调压住。
	 */
	if (unlikely(pwq->wq->flags & WQ_CPU_INTENSIVE))
		worker_set_flags(worker, WORKER_CPU_INTENSIVE);

	/*
	 * Kick @pool if necessary. It's always noop for per-cpu worker pools
	 * since nr_running would always be >= 1 at this point. This is used to
	 * chain execution of the pending work items for WORKER_NOT_RUNNING
	 * workers such as the UNBOUND and CPU_INTENSIVE ones.
	 */
	kick_pool(pool);

	/*
	 * Record the last pool and clear PENDING which should be the last
	 * update to @work.  Also, do this inside @pool->lock so that
	 * PENDING and queued state changes happen together while IRQ is
	 * disabled.
	 */
	/*
	 * 阶段 2：完成 queue→callback ownership 交接。清 PENDING 必须是最后一次访问
	 * work 可变状态，且与摘链同处 pool lock/关 IRQ 区；此后另一 CPU 可重新 queue。
	 */
	set_work_pool_and_clear_pending(work, pool->id, pool_offq_flags(pool));

	pwq->stats[PWQ_STAT_STARTED]++;
	raw_spin_unlock_irq(&pool->lock);
	/*
	 * 阶段 3：解开热锁执行任意回调。先记录 RCU/lockdep 深度，建立虚拟依赖图；
	 * 回调可以睡眠，也可以释放包含 work 的对象。
	 */

	rcu_start_depth = rcu_preempt_depth();
	lockdep_start_depth = lockdep_depth(current);
	/* see drain_dead_softirq_workfn() */
	if (!bh_draining)
		lock_map_acquire(pwq->wq->lockdep_map);
	lock_map_acquire(&lockdep_map);
	/*
	 * Strictly speaking we should mark the invariant state without holding
	 * any locks, that is, before these two lock_map_acquire()'s.
	 *
	 * However, that would result in:
	 *
	 *   A(W1)
	 *   WFC(C)
	 *		A(W1)
	 *		C(C)
	 *
	 * Which would create W1->C->W1 dependencies, even though there is no
	 * actual deadlock possible. There are two solutions, using a
	 * read-recursive acquire on the work(queue) 'locks', but this will then
	 * hit the lockdep limitation on recursive locks, or simply discard
	 * these locks.
	 *
	 * AFAICT there is no possible deadlock scenario between the
	 * flush_work() and complete() primitives (except for single-threaded
	 * workqueues), so hiding them isn't a problem.
	 */
	lockdep_invariant_state(true);
	trace_workqueue_execute_start(work);
	worker->current_func(work);
	/*
	 * While we must be careful to not use "work" after this, the trace
	 * point will only record its address.
	 */
	/* execute_end 仅记录数值地址；绝不能因为 trace 调用而再次解引用已可能释放的 work。 */
	trace_workqueue_execute_end(work, worker->current_func);

	lock_map_release(&lockdep_map);
	if (!bh_draining)
		lock_map_release(pwq->wq->lockdep_map);

	if (unlikely((worker->task && in_atomic()) ||
		     lockdep_depth(current) != lockdep_start_depth ||
		     rcu_preempt_depth() != rcu_start_depth)) {
		pr_err("BUG: workqueue leaked atomic, lock or RCU: %s[%d]\n"
		       "     preempt=0x%08x lock=%d->%d RCU=%d->%d workfn=%ps\n",
		       current->comm, task_pid_nr(current), preempt_count(),
		       lockdep_start_depth, lockdep_depth(current),
		       rcu_start_depth, rcu_preempt_depth(),
		       worker->current_func);
		debug_show_held_locks(current);
		dump_stack();
	}

	/*
	 * The following prevents a kworker from hogging CPU on !PREEMPTION
	 * kernels, where a requeueing work item waiting for something to
	 * happen could deadlock with stop_machine as such work item could
	 * indefinitely requeue itself while all other CPUs are trapped in
	 * stop_machine. At the same time, report a quiescent RCU state so
	 * the same condition doesn't freeze RCU.
	 */
	if (worker->task)
		cond_resched();

	raw_spin_lock_irq(&pool->lock);
	/* 阶段 4：回到 pool 锁域，清执行身份并结算所有并发、flush 与引用账户。 */

	pwq->stats[PWQ_STAT_COMPLETED]++;

	/*
	 * In addition to %WQ_CPU_INTENSIVE, @worker may also have been marked
	 * CPU intensive by wq_worker_tick() if @work hogged CPU longer than
	 * wq_cpu_intensive_thresh_us. Clear it.
	 */
	worker_clr_flags(worker, WORKER_CPU_INTENSIVE);

	/* tag the worker for identification in schedule() */
	worker->last_func = worker->current_func;

	/* we're done with it, release */
	/* 先从 busy_hash 摘除再清 current_*，之后新入队不再把实例串到本 worker。 */
	hash_del(&worker->hentry);
	worker->current_work = NULL;
	worker->current_func = NULL;
	worker->current_pwq = NULL;
	worker->current_color = INT_MAX;

	/* must be the last step, see the function comment */
	/* 结算可能跨 pool 换锁且可能唤醒 flusher，故必须是本函数最后一个状态操作。 */
	pwq_dec_nr_in_flight(pwq, work_data);
}

/**
 * process_scheduled_works - process scheduled works
 * @worker: self
 *
 * Process all scheduled works.  Please note that the scheduled list
 * may change while processing a work, so this function repeatedly
 * fetches a work from the top and executes it.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock) which may be released and regrabbed
 * multiple times.
 */
/*
 * process_scheduled_works() - 顺序排空 worker->scheduled。
 * @worker 为当前线程且持 pool lock；每次由 process_one_work() 临时解锁执行回调，
 * 返回时链为空或线程状态要求退出。返回无。
 */
static void process_scheduled_works(struct worker *worker)
{
	struct work_struct *work;
	bool first = true;

	while ((work = list_first_entry_or_null(&worker->scheduled,
						struct work_struct, entry))) {
		if (first) {
			worker->pool->last_progress_ts = jiffies;
			first = false;
		}
		process_one_work(worker, work);
	}
}

/*
 * set_pf_worker() - 设置/清除 current 的 PF_WQ_WORKER，并通知调度器 worker 身份变化。
 * @val 为目标状态；只作用当前 task，无返回。标志使 schedule 钩子能调用
 * wq_worker_sleeping/running，必须与 worker 主循环边界严格配对。
 */
static void set_pf_worker(bool val)
{
	mutex_lock(&wq_pool_attach_mutex);
	if (val)
		current->flags |= PF_WQ_WORKER;
	else
		current->flags &= ~PF_WQ_WORKER;
	mutex_unlock(&wq_pool_attach_mutex);
}

/**
 * worker_thread - the worker thread function
 * @__worker: self
 *
 * The worker thread function.  All workers belong to a worker_pool -
 * either a per-cpu one or dynamic unbound one.  These workers process all
 * work items regardless of their specific target workqueue.  The only
 * exception is work items which belong to workqueues with a rescuer which
 * will be explained in rescuer_thread().
 *
 * Return: 0
 */
/*
 * worker_thread() - 普通 kworker 主循环。
 * @__worker 是 create_worker() 转交、由 pool 管理的 worker。线程设 PF_WQ_WORKER，
 * 在 pool lock 下 idle/领取 work；无可执行项时 schedule，有短缺时竞选 manager，
 * 有 work 时 assign/process。收到 DIE 后摘除自身运行关联并返回 0，由 reap 回收。
 * 循环可睡眠，且必须保证最后 running worker 不离开仍非空 worklist。
 */
static int worker_thread(void *__worker)
{
	struct worker *worker = __worker;
	struct worker_pool *pool = worker->pool;

	/* tell the scheduler that this is a workqueue worker */
	/* 阶段 1：发布 task 的 worker 身份，调度器从此为睡眠/唤醒维护 nr_running。 */
	set_pf_worker(true);
woke_up:
	raw_spin_lock_irq(&pool->lock);

	/* am I supposed to die? */
	/* DIE 只在醒来持 pool lock 时消费；清 PF 后断开 pool，避免退出尾声误用旧归属。 */
	if (unlikely(worker->flags & WORKER_DIE)) {
		raw_spin_unlock_irq(&pool->lock);
		set_pf_worker(false);
		/*
		 * The worker is dead and PF_WQ_WORKER is cleared, worker->pool
		 * shouldn't be accessed, reset it to NULL in case otherwise.
		 */
		worker->pool = NULL;
		ida_free(&pool->worker_ida, worker->id);
		return 0;
	}

	worker_leave_idle(worker);
recheck:
	/* no more worker necessary? */
	/* 阶段 2：重新评估需求；worklist 空或已有 running 同伴时进入 idle。 */
	if (!need_more_worker(pool))
		goto sleep;

	/* do we need to manage? */
	/* 有待办却无 idle 后备时竞选 manager；管理结束必须重新读全部条件。 */
	if (unlikely(!may_start_working(pool)) && manage_workers(worker))
		goto recheck;

	/*
	 * ->scheduled list can only be filled while a worker is
	 * preparing to process a work or actually processing it.
	 * Make sure nobody diddled with it while I was sleeping.
	 */
	WARN_ON_ONCE(!list_empty(&worker->scheduled));

	/*
	 * Finish PREP stage.  We're guaranteed to have at least one idle
	 * worker or that someone else has already assumed the manager
	 * role.  This is where @worker starts participating in concurrency
	 * management if applicable and concurrency management is restored
	 * after being rebound.  See rebind_workers() for details.
	 */
	/*
	 * 阶段 3：清 PREP/REBOUND 正式计入 nr_running。此刻已有 idle 后备或 manager，
	 * 因而当前回调若睡眠，调度钩子能安全唤醒/补充其他 worker。
	 */
	worker_clr_flags(worker, WORKER_PREP | WORKER_REBOUND);

	do {
		struct work_struct *work =
			list_first_entry(&pool->worklist,
					 struct work_struct, entry);

		if (assign_work(work, worker, NULL))
			process_scheduled_works(worker);
	} while (keep_working(pool));
	/*
	 * 阶段 4：每轮从 pool 表头领取一条 linked 串。只要当前是最后 running worker
	 * 且 worklist 仍非空就继续，防止所有执行者同时离开造成队列停顿。
	 */

	worker_set_flags(worker, WORKER_PREP);
sleep:
	/*
	 * pool->lock is held and there's no work to process and no need to
	 * manage, sleep.  Workers are woken up only while holding
	 * pool->lock or from local cpu, so setting the current state
	 * before releasing pool->lock is enough to prevent losing any
	 * event.
	 */
	/*
	 * 阶段 5：在持锁时先入 idle_list、设置 TASK_IDLE，再解锁 schedule。唤醒者也在
	 * 同一锁或本 CPU 协议下操作，因此不会发生“检查为空后、睡下前”丢失唤醒。
	 */
	worker_enter_idle(worker);
	__set_current_state(TASK_IDLE);
	raw_spin_unlock_irq(&pool->lock);
	schedule();
	goto woke_up;
}

/*
 * assign_rescuer_work() - 从 mayday pwq 的 pool 中批量挑可救援 work 给 rescuer。
 * @pwq 借用且相关锁已建立；@rescuer 为该 wq 专用线程。最多 RESCUER_BATCH，跳过
 * 不属于该 pwq/不安全碰撞项；成功分配至少一项返回 true，否则 false。work 的
 * PENDING/pwq 账不在此结算，仍由 process_one_work() 完成。
 */
static bool assign_rescuer_work(struct pool_workqueue *pwq, struct worker *rescuer)
{
	struct worker_pool *pool = pwq->pool;
	struct work_struct *cursor = &pwq->mayday_cursor;
	struct work_struct *work, *n;

	/* have work items to rescue? */
	if (!pwq->nr_active)
		return false;

	/* need rescue? */
	if (!need_to_create_worker(pool)) {
		/*
		 * The pool has idle workers and doesn't need the rescuer, so it
		 * could simply return false here.
		 *
		 * However, the memory pressure might not be fully relieved.
		 * In PERCPU pool with concurrency enabled, having idle workers
		 * does not necessarily mean memory pressure is gone; it may
		 * simply mean regular workers have woken up, completed their
		 * work, and gone idle again due to concurrency limits.
		 *
		 * In this case, those working workers may later sleep again,
		 * the pool may run out of idle workers, and it will have to
		 * allocate new ones and wait for the timer to send mayday,
		 * causing unnecessary delay - especially if memory pressure
		 * was never resolved throughout.
		 *
		 * Do more work if memory pressure is still on to reduce
		 * relapse, using (pool->flags & POOL_MANAGER_ACTIVE), though
		 * not precisely, unless there are other PWQs needing help.
		 */
		if (!(pool->flags & POOL_MANAGER_ACTIVE) ||
		    !list_empty(&pwq->wq->maydays))
			return false;
	}

	/* search from the start or cursor if available */
	if (list_empty(&cursor->entry))
		work = list_first_entry(&pool->worklist, struct work_struct, entry);
	else
		work = list_next_entry(cursor, entry);

	/* find the next work item to rescue */
	list_for_each_entry_safe_from(work, n, &pool->worklist, entry) {
		if (get_work_pwq(work) == pwq && assign_work(work, rescuer, &n)) {
			pwq->stats[PWQ_STAT_RESCUED]++;
			/* put the cursor for next search */
			list_move_tail(&cursor->entry, &n->entry);
			return true;
		}
	}

	return false;
}

/**
 * rescuer_thread - the rescuer thread function
 * @__rescuer: self
 *
 * Workqueue rescuer thread function.  There's one rescuer for each
 * workqueue which has WQ_MEM_RECLAIM set.
 *
 * Regular work processing on a pool may block trying to create a new
 * worker which uses GFP_KERNEL allocation which has slight chance of
 * developing into deadlock if some works currently on the same queue
 * need to be processed to satisfy the GFP_KERNEL allocation.  This is
 * the problem rescuer solves.
 *
 * When such condition is possible, the pool summons rescuers of all
 * workqueues which have works queued on the pool and let them process
 * those works so that forward progress can be guaranteed.
 *
 * This should happen rarely.
 *
 * Return: 0
 */
/*
 * rescuer_thread() - WQ_MEM_RECLAIM workqueue 的保底执行线程。
 * @__rescuer 为 wq 独占 rescuer；等待 maydays，逐个从求救 pwq 搬运小批 work 并
 * 直接按 worker 协议执行。它不依赖目标 pool 再分配 kworker 内存，因此打破 reclaim
 * “等待 work、work 又等内存”死锁。线程可睡眠，停止时返回 0。
 */
static int rescuer_thread(void *__rescuer)
{
	struct worker *rescuer = __rescuer;
	struct workqueue_struct *wq = rescuer->rescue_wq;
	bool should_stop;

	set_user_nice(current, RESCUER_NICE_LEVEL);

	/*
	 * Mark rescuer as worker too.  As WORKER_PREP is never cleared, it
	 * doesn't participate in concurrency management.
	 */
	set_pf_worker(true);
repeat:
	set_current_state(TASK_IDLE);

	/*
	 * By the time the rescuer is requested to stop, the workqueue
	 * shouldn't have any work pending, but @wq->maydays may still have
	 * pwq(s) queued.  This can happen by non-rescuer workers consuming
	 * all the work items before the rescuer got to them.  Go through
	 * @wq->maydays processing before acting on should_stop so that the
	 * list is always empty on exit.
	 */
	should_stop = kthread_should_stop();

	/* see whether any pwq is asking for help */
	raw_spin_lock_irq(&wq_mayday_lock);

	while (!list_empty(&wq->maydays)) {
		struct pool_workqueue *pwq = list_first_entry(&wq->maydays,
					struct pool_workqueue, mayday_node);
		struct worker_pool *pool = pwq->pool;
		unsigned int count = 0;

		__set_current_state(TASK_RUNNING);
		list_del_init(&pwq->mayday_node);

		raw_spin_unlock_irq(&wq_mayday_lock);

		worker_attach_to_pool(rescuer, pool);

		raw_spin_lock_irq(&pool->lock);

		WARN_ON_ONCE(!list_empty(&rescuer->scheduled));

		while (assign_rescuer_work(pwq, rescuer)) {
			process_scheduled_works(rescuer);

			/*
			 * If the per-turn work item limit is reached and other
			 * PWQs are in mayday, requeue mayday for this PWQ and
			 * let the rescuer handle the other PWQs first.
			 */
			if (++count > RESCUER_BATCH && !list_empty(&pwq->wq->maydays) &&
			    pwq->nr_active && need_to_create_worker(pool)) {
				raw_spin_lock(&wq_mayday_lock);
				send_mayday(pwq);
				raw_spin_unlock(&wq_mayday_lock);
				break;
			}
		}

		/* The cursor can not be left behind without the rescuer watching it. */
		if (!list_empty(&pwq->mayday_cursor.entry) && list_empty(&pwq->mayday_node))
			list_del_init(&pwq->mayday_cursor.entry);

		/*
		 * Leave this pool. Notify regular workers; otherwise, we end up
		 * with 0 concurrency and stalling the execution.
		 */
		kick_pool(pool);

		raw_spin_unlock_irq(&pool->lock);

		worker_detach_from_pool(rescuer);

		/*
		 * Put the reference grabbed by send_mayday().  @pool might
		 * go away any time after it.
		 */
		put_pwq_unlocked(pwq);

		raw_spin_lock_irq(&wq_mayday_lock);
	}

	raw_spin_unlock_irq(&wq_mayday_lock);

	if (should_stop) {
		__set_current_state(TASK_RUNNING);
		set_pf_worker(false);
		return 0;
	}

	/* rescuers should never participate in concurrency management */
	WARN_ON_ONCE(!(rescuer->flags & WORKER_NOT_RUNNING));
	schedule();
	goto repeat;
}

/*
 * bh_worker() - 在单 CPU softirq 上执行 BH pool 的 scheduled work。
 * @worker 是该次栈上/临时执行上下文，不能睡眠。按时间与 restart 预算处理，
 * 超限后重新 raise softirq；回调必须符合 BH 原子上下文约束。
 */
static void bh_worker(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;
	int nr_restarts = BH_WORKER_RESTARTS;
	unsigned long end = jiffies + BH_WORKER_JIFFIES;

	worker_lock_callback(pool);
	raw_spin_lock_irq(&pool->lock);
	worker_leave_idle(worker);

	/*
	 * This function follows the structure of worker_thread(). See there for
	 * explanations on each step.
	 */
	if (!need_more_worker(pool))
		goto done;

	WARN_ON_ONCE(!list_empty(&worker->scheduled));
	worker_clr_flags(worker, WORKER_PREP | WORKER_REBOUND);

	do {
		struct work_struct *work =
			list_first_entry(&pool->worklist,
					 struct work_struct, entry);

		if (assign_work(work, worker, NULL))
			process_scheduled_works(worker);
	} while (keep_working(pool) &&
		 --nr_restarts && time_before(jiffies, end));

	worker_set_flags(worker, WORKER_PREP);
done:
	worker_enter_idle(worker);
	kick_pool(pool);
	raw_spin_unlock_irq(&pool->lock);
	worker_unlock_callback(pool);
}

/*
 * TODO: Convert all tasklet users to workqueue and use softirq directly.
 *
 * This is currently called from tasklet[_hi]action() and thus is also called
 * whenever there are tasklets to run. Let's do an early exit if there's nothing
 * queued. Once conversion from tasklet is complete, the need_more_worker() test
 * can be dropped.
 *
 * After full conversion, we'll add worker->softirq_action, directly use the
 * softirq action and obtain the worker pointer from the softirq_action pointer.
 */
/*
 * workqueue_softirq_action() - softirq 入口，选择当前 CPU 普通或 highpri BH pool。
 * @highpri 决定 pool 索引；返回无、IRQ/softirq 上下文不可睡眠。构造执行 worker
 * 状态并调用 bh_worker() 排空预算内 work。
 */
void workqueue_softirq_action(bool highpri)
{
	struct worker_pool *pool =
		&per_cpu(bh_worker_pools, smp_processor_id())[highpri];
	if (need_more_worker(pool))
		bh_worker(list_first_entry(&pool->workers, struct worker, node));
}

struct wq_drain_dead_softirq_work {
	struct work_struct	work;
	struct worker_pool	*pool;
	struct completion	done;
};

/*
 * drain_dead_softirq_workfn() - CPU 离线后在存活 CPU 进程上下文触发死亡 CPU 的
 * BH pool 排空。@work 是临时同步桥；返回无，完成时目标 BH work 已推进到可安全
 * 结束离线阶段的状态。
 */
static void drain_dead_softirq_workfn(struct work_struct *work)
{
	struct wq_drain_dead_softirq_work *dead_work =
		container_of(work, struct wq_drain_dead_softirq_work, work);
	struct worker_pool *pool = dead_work->pool;
	bool repeat;

	/*
	 * @pool's CPU is dead and we want to execute its still pending work
	 * items from this BH work item which is running on a different CPU. As
	 * its CPU is dead, @pool can't be kicked and, as work execution path
	 * will be nested, a lockdep annotation needs to be suppressed. Mark
	 * @pool with %POOL_BH_DRAINING for the special treatments.
	 */
	raw_spin_lock_irq(&pool->lock);
	pool->flags |= POOL_BH_DRAINING;
	raw_spin_unlock_irq(&pool->lock);

	bh_worker(list_first_entry(&pool->workers, struct worker, node));

	raw_spin_lock_irq(&pool->lock);
	pool->flags &= ~POOL_BH_DRAINING;
	repeat = need_more_worker(pool);
	raw_spin_unlock_irq(&pool->lock);

	/*
	 * bh_worker() might hit consecutive execution limit and bail. If there
	 * still are pending work items, reschedule self and return so that we
	 * don't hog this CPU's BH.
	 */
	if (repeat) {
		if (pool->attrs->nice == HIGHPRI_NICE_LEVEL)
			queue_work(system_bh_highpri_wq, work);
		else
			queue_work(system_bh_wq, work);
	} else {
		complete(&dead_work->done);
	}
}

/*
 * @cpu is dead. Drain the remaining BH work items on the current CPU. It's
 * possible to allocate dead_work per CPU and avoid flushing. However, then we
 * have to worry about draining overlapping with CPU coming back online or
 * nesting (one CPU's dead_work queued on another CPU which is also dead and so
 * on). Let's keep it simple and drain them synchronously. These are BH work
 * items which shouldn't be requeued on the same pool. Shouldn't take long.
 */
/*
 * workqueue_softirq_dead() - CPU 已死亡时排空其两个 BH pool。
 * @cpu 为离线 CPU；设置 BH_DRAINING 阻止再向其发 irq_work，通过存活 CPU 执行
 * drain helper，完成后清理。可睡眠，返回时死亡 CPU 不再遗留 BH work。
 */
void workqueue_softirq_dead(unsigned int cpu)
{
	int i;

	for (i = 0; i < NR_STD_WORKER_POOLS; i++) {
		struct worker_pool *pool = &per_cpu(bh_worker_pools, cpu)[i];
		struct wq_drain_dead_softirq_work dead_work;

		if (!need_more_worker(pool))
			continue;

		INIT_WORK_ONSTACK(&dead_work.work, drain_dead_softirq_workfn);
		dead_work.pool = pool;
		init_completion(&dead_work.done);

		if (pool->attrs->nice == HIGHPRI_NICE_LEVEL)
			queue_work(system_bh_highpri_wq, &dead_work.work);
		else
			queue_work(system_bh_wq, &dead_work.work);

		wait_for_completion(&dead_work.done);
		destroy_work_on_stack(&dead_work.work);
	}
}

/**
 * check_flush_dependency - check for flush dependency sanity
 * @target_wq: workqueue being flushed
 * @target_work: work item being flushed (NULL for workqueue flushes)
 * @from_cancel: are we called from the work cancel path
 *
 * %current is trying to flush the whole @target_wq or @target_work on it.
 * If this is not the cancel path (which implies work being flushed is either
 * already running, or will not be at all), check if @target_wq doesn't have
 * %WQ_MEM_RECLAIM and verify that %current is not reclaiming memory or running
 * on a workqueue which doesn't have %WQ_MEM_RECLAIM as that can break forward-
 * progress guarantee leading to a deadlock.
 */
/*
 * check_flush_dependency() - 用 lockdep 检查当前 work flush 目标 wq 是否形成依赖环。
 * @target_wq 借用，@target_work 可空。只做调试告警，无返回、不改变执行；尤其捕获
 * reclaim workqueue 等待不具 WQ_MEM_RECLAIM 前进保证的队列。
 */
static void check_flush_dependency(struct workqueue_struct *target_wq,
				   struct work_struct *target_work,
				   bool from_cancel)
{
	work_func_t target_func;
	struct worker *worker;

	if (from_cancel || target_wq->flags & WQ_MEM_RECLAIM)
		return;

	worker = current_wq_worker();
	target_func = target_work ? target_work->func : NULL;

	WARN_ONCE(current->flags & PF_MEMALLOC,
		  "workqueue: PF_MEMALLOC task %d(%s) is flushing !WQ_MEM_RECLAIM %s:%ps",
		  current->pid, current->comm, target_wq->name, target_func);
	WARN_ONCE(worker && ((worker->current_pwq->wq->flags &
			      (WQ_MEM_RECLAIM | __WQ_LEGACY)) == WQ_MEM_RECLAIM),
		  "workqueue: WQ_MEM_RECLAIM %s:%ps is flushing !WQ_MEM_RECLAIM %s:%ps",
		  worker->current_pwq->wq->name, worker->current_func,
		  target_wq->name, target_func);
}

struct wq_barrier {
	struct work_struct	work;
	struct completion	done;
	struct task_struct	*task;	/* purely informational */
};

/*
 * wq_barrier_func() - barrier work 到达执行点时完成其 completion。
 * @work 内嵌于栈/等待者持有的 wq_barrier；回调不睡眠、无返回。complete 发布
 * “目标 work 及其之前串接项已越过观察点”，等待者醒来后才可释放 barrier。
 */
static void wq_barrier_func(struct work_struct *work)
{
	struct wq_barrier *barr = container_of(work, struct wq_barrier, work);
	complete(&barr->done);
}

/**
 * insert_wq_barrier - insert a barrier work
 * @pwq: pwq to insert barrier into
 * @barr: wq_barrier to insert
 * @target: target work to attach @barr to
 * @worker: worker currently executing @target, NULL if @target is not executing
 *
 * @barr is linked to @target such that @barr is completed only after
 * @target finishes execution.  Please note that the ordering
 * guarantee is observed only with respect to @target and on the local
 * cpu.
 *
 * Currently, a queued barrier can't be canceled.  This is because
 * try_to_grab_pending() can't determine whether the work to be
 * grabbed is at the head of the queue and thus can't clear LINKED
 * flag of the previous work while there must be a valid next work
 * after a work with LINKED flag set.
 *
 * Note that when @worker is non-NULL, @target may be modified
 * underneath us, so we can't reliably determine pwq from @target.
 *
 * CONTEXT:
 * raw_spin_lock_irq(pool->lock).
 */
/*
 * insert_wq_barrier() - 把 barrier 链到待执行 work 或当前 worker 后面。
 * @pwq 目标关联；@barr 由等待者栈持有并保持到 completion；@target 是观察对象；
 * @worker 非空表示 target 正在执行。入口持 pool lock，返回无。barrier 带 LINKED/
 * INACTIVE，既不能被普通 cancel 偷走，也不消耗 max_active；它通过队列顺序把
 * “目标完成”转换成 completion。
 */
static void insert_wq_barrier(struct pool_workqueue *pwq,
			      struct wq_barrier *barr,
			      struct work_struct *target, struct worker *worker)
{
	static __maybe_unused struct lock_class_key bh_key, thr_key;
	unsigned int work_flags = 0;
	unsigned int work_color;
	struct list_head *head;

	/*
	 * debugobject calls are safe here even with pool->lock locked
	 * as we know for sure that this will not trigger any of the
	 * checks and call back into the fixup functions where we
	 * might deadlock.
	 *
	 * BH and threaded workqueues need separate lockdep keys to avoid
	 * spuriously triggering "inconsistent {SOFTIRQ-ON-W} -> {IN-SOFTIRQ-W}
	 * usage".
	 */
	INIT_WORK_ONSTACK_KEY(&barr->work, wq_barrier_func,
			      (pwq->wq->flags & WQ_BH) ? &bh_key : &thr_key);
	__set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&barr->work));

	init_completion_map(&barr->done, &target->lockdep_map);

	barr->task = current;

	/* The barrier work item does not participate in nr_active. */
	work_flags |= WORK_STRUCT_INACTIVE;

	/*
	 * If @target is currently being executed, schedule the
	 * barrier to the worker; otherwise, put it after @target.
	 */
	if (worker) {
		head = worker->scheduled.next;
		work_color = worker->current_color;
	} else {
		unsigned long *bits = work_data_bits(target);

		head = target->entry.next;
		/* there can already be other linked works, inherit and set */
		work_flags |= *bits & WORK_STRUCT_LINKED;
		work_color = get_work_color(*bits);
		__set_bit(WORK_STRUCT_LINKED_BIT, bits);
	}

	pwq->nr_in_flight[work_color]++;
	work_flags |= work_color_to_flags(work_color);

	insert_work(pwq, &barr->work, head, work_flags);
}

/**
 * flush_workqueue_prep_pwqs - prepare pwqs for workqueue flushing
 * @wq: workqueue being flushed
 * @flush_color: new flush color, < 0 for no-op
 * @work_color: new work color, < 0 for no-op
 *
 * Prepare pwqs for workqueue flushing.
 *
 * If @flush_color is non-negative, flush_color on all pwqs should be
 * -1.  If no pwq has in-flight commands at the specified color, all
 * pwq->flush_color's stay at -1 and %false is returned.  If any pwq
 * has in flight commands, its pwq->flush_color is set to
 * @flush_color, @wq->nr_pwqs_to_flush is updated accordingly, pwq
 * wakeup logic is armed and %true is returned.
 *
 * The caller should have initialized @wq->first_flusher prior to
 * calling this function with non-negative @flush_color.  If
 * @flush_color is negative, no flush color update is done and %false
 * is returned.
 *
 * If @work_color is non-negative, all pwqs should have the same
 * work_color which is previous to @work_color and all will be
 * advanced to @work_color.
 *
 * CONTEXT:
 * mutex_lock(wq->mutex).
 *
 * Return:
 * %true if @flush_color >= 0 and there's something to flush.  %false
 * otherwise.
 */
/*
 * flush_workqueue_prep_pwqs() - 为一次 color flush 标记所有仍有该代 work 的 pwq。
 * @wq 入口持 mutex；@flush_color/-1 决定安装或清理，@work_color 可推进入队代际。
 * 返回是否至少一个 pwq 需要等待；对每个参与者设置 flush_color 并递增
 * nr_pwqs_to_flush。只准备账，不等待。
 */
static bool flush_workqueue_prep_pwqs(struct workqueue_struct *wq,
				      int flush_color, int work_color)
{
	bool wait = false;
	struct pool_workqueue *pwq;
	struct worker_pool *current_pool = NULL;

	if (flush_color >= 0) {
		WARN_ON_ONCE(atomic_read(&wq->nr_pwqs_to_flush));
		atomic_set(&wq->nr_pwqs_to_flush, 1);
	}

	/*
	 * For unbound workqueue, pwqs will map to only a few pools.
	 * Most of the time, pwqs within the same pool will be linked
	 * sequentially to wq->pwqs by cpu index. So in the majority
	 * of pwq iters, the pool is the same, only doing lock/unlock
	 * if the pool has changed. This can largely reduce expensive
	 * lock operations.
	 */
	for_each_pwq(pwq, wq) {
		if (current_pool != pwq->pool) {
			if (likely(current_pool))
				raw_spin_unlock_irq(&current_pool->lock);
			current_pool = pwq->pool;
			raw_spin_lock_irq(&current_pool->lock);
		}

		if (flush_color >= 0) {
			WARN_ON_ONCE(pwq->flush_color != -1);

			if (pwq->nr_in_flight[flush_color]) {
				pwq->flush_color = flush_color;
				atomic_inc(&wq->nr_pwqs_to_flush);
				wait = true;
			}
		}

		if (work_color >= 0) {
			WARN_ON_ONCE(work_color != work_next_color(pwq->work_color));
			pwq->work_color = work_color;
		}

	}

	if (current_pool)
		raw_spin_unlock_irq(&current_pool->lock);

	if (flush_color >= 0 && atomic_dec_and_test(&wq->nr_pwqs_to_flush))
		complete(&wq->first_flusher->done);

	return wait;
}

/* touch_wq_lockdep_map() 人工 acquire/release wq map，让 lockdep 建立 flush 依赖边。 */
static void touch_wq_lockdep_map(struct workqueue_struct *wq)
{
#ifdef CONFIG_LOCKDEP
	if (unlikely(!wq->lockdep_map))
		return;

	if (wq->flags & WQ_BH)
		local_bh_disable();

	lock_map_acquire(wq->lockdep_map);
	lock_map_release(wq->lockdep_map);

	if (wq->flags & WQ_BH)
		local_bh_enable();
#endif
}

/* touch_work_lockdep_map() 为具体 work/barrier 建立 lockdep 等待关系；仅调试元数据。 */
static void touch_work_lockdep_map(struct work_struct *work,
				   struct workqueue_struct *wq)
{
#ifdef CONFIG_LOCKDEP
	if (wq->flags & WQ_BH)
		local_bh_disable();

	lock_map_acquire(&work->lockdep_map);
	lock_map_release(&work->lockdep_map);

	if (wq->flags & WQ_BH)
		local_bh_enable();
#endif
}

/**
 * __flush_workqueue - ensure that any scheduled work has run to completion.
 * @wq: workqueue to flush
 *
 * This function sleeps until all work items which were queued on entry
 * have finished execution, but it is not livelocked by new incoming ones.
 */
/*
 * __flush_workqueue() - 等待调用前已进入 @wq 的所有 work 完成。
 *
 * @wq 必须存活，函数可睡眠，返回无。它在 wq->mutex 下为本轮分配 flush color；
 * 若 color 用尽则进入 overflow 队列，等待前序 flusher 释放代际。首 flusher 给各
 * pwq 安装 flush_color 后等待 nr_pwqs_to_flush 归零，再推进队列中后续 flusher。
 * 新入队 work 使用下一 work_color，不属于本轮，因此 flush 不会封锁生产者。
 *
 * flush 不是 drain：回调在 flush 期间新排的 work 通常属于新代际，调用者若要求
 * 链式闭包完全静止应使用 drain_workqueue()，同时避免从目标 wq 自身形成等待环。
 */
void __flush_workqueue(struct workqueue_struct *wq)
{
	struct wq_flusher this_flusher = {
		.list = LIST_HEAD_INIT(this_flusher.list),
		.flush_color = -1,
		.done = COMPLETION_INITIALIZER_ONSTACK_MAP(this_flusher.done, (*wq->lockdep_map)),
	};
	int next_color;

	if (WARN_ON(!wq_online))
		return;

	touch_wq_lockdep_map(wq);

	mutex_lock(&wq->mutex);

	/*
	 * Start-to-wait phase
	 */
	/*
	 * 阶段 1：取得代际或进入 overflow。work_color 是新入队使用的颜色，
	 * flush_color 是当前正在排空的颜色；环形空间未满时本轮领取旧 work_color 并
	 * 立即推进生产者颜色，空间满则不能复用仍在途颜色，只能等前序释放。
	 */
	next_color = work_next_color(wq->work_color);

	if (next_color != wq->flush_color) {
		/*
		 * Color space is not full.  The current work_color
		 * becomes our flush_color and work_color is advanced
		 * by one.
		 */
		WARN_ON_ONCE(!list_empty(&wq->flusher_overflow));
		this_flusher.flush_color = wq->work_color;
		wq->work_color = next_color;

		if (!wq->first_flusher) {
			/* no flush in progress, become the first flusher */
			/* 当前无 leader，本栈对象成为 first_flusher 并给所有相关 pwq 装观察色。 */
			WARN_ON_ONCE(wq->flush_color != this_flusher.flush_color);

			wq->first_flusher = &this_flusher;

			if (!flush_workqueue_prep_pwqs(wq, wq->flush_color,
						       wq->work_color)) {
				/* nothing to flush, done */
				/* 没有任何该代 in-flight，直接推进 flush_color，无需睡眠。 */
				wq->flush_color = next_color;
				wq->first_flusher = NULL;
				goto out_unlock;
			}
		} else {
			/* wait in queue */
			/* 已有 leader 时按 color 排到 flusher_queue，由 leader 完成后级联唤醒。 */
			WARN_ON_ONCE(wq->flush_color == this_flusher.flush_color);
			list_add_tail(&this_flusher.list, &wq->flusher_queue);
			flush_workqueue_prep_pwqs(wq, -1, wq->work_color);
		}
	} else {
		/*
		 * Oops, color space is full, wait on overflow queue.
		 * The next flush completion will assign us
		 * flush_color and transfer to flusher_queue.
		 */
		/* 所有 color 在用：先不赋色，下一次 leader 释放一个槽后整批转正。 */
		list_add_tail(&this_flusher.list, &wq->flusher_overflow);
	}

	check_flush_dependency(wq, NULL, false);

	mutex_unlock(&wq->mutex);

	wait_for_completion(&this_flusher.done);
	/* completion 由最后一个相关 pwq 的 in-flight 归零触发，至此本轮目标 work 已完成。 */

	/*
	 * Wake-up-and-cascade phase
	 *
	 * First flushers are responsible for cascading flushes and
	 * handling overflow.  Non-first flushers can simply return.
	 */
	/*
	 * 阶段 2：只有 first flusher 负责推进全局状态和级联后继；普通等待者被 leader
	 * complete 后即可返回。无锁预判后还需 mutex 内复查，防止 leader 身份刚转移。
	 */
	if (READ_ONCE(wq->first_flusher) != &this_flusher)
		return;

	mutex_lock(&wq->mutex);

	/* we might have raced, check again with mutex held */
	if (wq->first_flusher != &this_flusher)
		goto out_unlock;

	WRITE_ONCE(wq->first_flusher, NULL);

	WARN_ON_ONCE(!list_empty(&this_flusher.list));
	WARN_ON_ONCE(wq->flush_color != this_flusher.flush_color);

	while (true) {
		struct wq_flusher *next, *tmp;

		/* complete all the flushers sharing the current flush color */
		/* 同 color 的等待者观察同一代 work，可一次性全部完成。 */
		list_for_each_entry_safe(next, tmp, &wq->flusher_queue, list) {
			if (next->flush_color != wq->flush_color)
				break;
			list_del_init(&next->list);
			complete(&next->done);
		}

		WARN_ON_ONCE(!list_empty(&wq->flusher_overflow) &&
			     wq->flush_color != work_next_color(wq->work_color));

		/* this flush_color is finished, advance by one */
		/* 释放一个环形 color 槽，flush 前沿向 work_color 靠近。 */
		wq->flush_color = work_next_color(wq->flush_color);

		/* one color has been freed, handle overflow queue */
		/* overflow 等待者共享当前 work_color，再推进生产者色并转入正式 flusher_queue。 */
		if (!list_empty(&wq->flusher_overflow)) {
			/*
			 * Assign the same color to all overflowed
			 * flushers, advance work_color and append to
			 * flusher_queue.  This is the start-to-wait
			 * phase for these overflowed flushers.
			 */
			list_for_each_entry(tmp, &wq->flusher_overflow, list)
				tmp->flush_color = wq->work_color;

			wq->work_color = work_next_color(wq->work_color);

			list_splice_tail_init(&wq->flusher_overflow,
					      &wq->flusher_queue);
			flush_workqueue_prep_pwqs(wq, -1, wq->work_color);
		}

		if (list_empty(&wq->flusher_queue)) {
			WARN_ON_ONCE(wq->flush_color != wq->work_color);
			break;
		}

		/*
		 * Need to flush more colors.  Make the next flusher
		 * the new first flusher and arm pwqs.
		 */
		/* 队列仍有更年轻代际：选其首项为新 leader，给 pwq 安装新 flush_color。 */
		WARN_ON_ONCE(wq->flush_color == wq->work_color);
		WARN_ON_ONCE(wq->flush_color != next->flush_color);

		list_del_init(&next->list);
		wq->first_flusher = next;

		if (flush_workqueue_prep_pwqs(wq, wq->flush_color, -1))
			break;

		/*
		 * Meh... this color is already done, clear first
		 * flusher and repeat cascading.
		 */
		/* 该代已经自然排空，无需睡眠；清 leader 并在循环中继续级联下一代。 */
		wq->first_flusher = NULL;
	}

out_unlock:
	mutex_unlock(&wq->mutex);
}
EXPORT_SYMBOL(__flush_workqueue);

/**
 * drain_workqueue - drain a workqueue
 * @wq: workqueue to drain
 *
 * Wait until the workqueue becomes empty.  While draining is in progress,
 * only chain queueing is allowed.  IOW, only currently pending or running
 * work items on @wq can queue further work items on it.  @wq is flushed
 * repeatedly until it becomes empty.  The number of flushing is determined
 * by the depth of chaining and should be relatively short.  Whine if it
 * takes too long.
 */
/*
 * drain_workqueue() - 阻止外部新入队并反复 flush，直到链式 work 闭包也排空。
 * @wq 借用且必须存活；可睡眠、返回无。nr_drainers 支持嵌套，首个 drainer 设置
 * DRAINING；只有同 wq 当前回调可继续 chained queue。循环 flush 至所有 pwq idle，
 * 最后一个 drainer 清标志。它不应从该 wq 自身回调调用，否则可能自等。
 */
void drain_workqueue(struct workqueue_struct *wq)
{
	unsigned int flush_cnt = 0;
	struct pool_workqueue *pwq;

	/*
	 * __queue_work() needs to test whether there are drainers, is much
	 * hotter than drain_workqueue() and already looks at @wq->flags.
	 * Use __WQ_DRAINING so that queue doesn't have to check nr_drainers.
	 */
	mutex_lock(&wq->mutex);
	if (!wq->nr_drainers++)
		wq->flags |= __WQ_DRAINING;
	mutex_unlock(&wq->mutex);
reflush:
	__flush_workqueue(wq);

	mutex_lock(&wq->mutex);

	for_each_pwq(pwq, wq) {
		bool drained;

		raw_spin_lock_irq(&pwq->pool->lock);
		drained = pwq_is_empty(pwq);
		raw_spin_unlock_irq(&pwq->pool->lock);

		if (drained)
			continue;

		if (++flush_cnt == 10 ||
		    (flush_cnt % 100 == 0 && flush_cnt <= 1000))
			pr_warn("workqueue %s: %s() isn't complete after %u tries\n",
				wq->name, __func__, flush_cnt);

		mutex_unlock(&wq->mutex);
		goto reflush;
	}

	if (!--wq->nr_drainers)
		wq->flags &= ~__WQ_DRAINING;
	mutex_unlock(&wq->mutex);
}
EXPORT_SYMBOL_GPL(drain_workqueue);

/*
 * start_flush_work() - 为单个 pending/running work 安装 barrier。
 * @work 借用；@barr 由等待者持有；@from_cancel 调整锁依赖/RT 同步。返回 true 表示
 * barrier 已插入、调用者必须等待 completion；目标已 idle 返回 false。通过 RCU
 * 找 pool、锁内同时检查 queued pwq 与 busy worker，竞态时重试。
 */
static bool start_flush_work(struct work_struct *work, struct wq_barrier *barr,
			     bool from_cancel)
{
	struct worker *worker = NULL;
	struct worker_pool *pool;
	struct pool_workqueue *pwq;
	struct workqueue_struct *wq;

	rcu_read_lock();
	pool = get_work_pool(work);
	if (!pool) {
		rcu_read_unlock();
		return false;
	}

	raw_spin_lock_irq(&pool->lock);
	/* see the comment in try_to_grab_pending() with the same code */
	pwq = get_work_pwq(work);
	if (pwq) {
		if (unlikely(pwq->pool != pool))
			goto already_gone;
	} else {
		worker = find_worker_executing_work(pool, work);
		if (!worker)
			goto already_gone;
		pwq = worker->current_pwq;
	}

	wq = pwq->wq;
	check_flush_dependency(wq, work, from_cancel);

	insert_wq_barrier(pwq, barr, work, worker);
	raw_spin_unlock_irq(&pool->lock);

	touch_work_lockdep_map(work, wq);

	/*
	 * Force a lock recursion deadlock when using flush_work() inside a
	 * single-threaded or rescuer equipped workqueue.
	 *
	 * For single threaded workqueues the deadlock happens when the work
	 * is after the work issuing the flush_work(). For rescuer equipped
	 * workqueues the deadlock happens when the rescuer stalls, blocking
	 * forward progress.
	 */
	if (!from_cancel && (wq->saved_max_active == 1 || wq->rescuer))
		touch_wq_lockdep_map(wq);

	rcu_read_unlock();
	return true;
already_gone:
	raw_spin_unlock_irq(&pool->lock);
	rcu_read_unlock();
	return false;
}

/*
 * __flush_work() - 等待一个 work 当前已排队或正在执行的实例完成。
 * @work 必须在等待期间存活；@from_cancel 表示调用者已参与取消协议。可睡眠。
 * 成功安装 barrier 时等待并返回 true；目标本来 idle 返回 false。该 API 只等待
 * 调用时可观察实例，不禁止其他 CPU 在完成后重新 queue 同一 work。
 */
static bool __flush_work(struct work_struct *work, bool from_cancel)
{
	struct wq_barrier barr;

	if (WARN_ON(!wq_online))
		return false;

	if (WARN_ON(!work->func))
		return false;

	if (!start_flush_work(work, &barr, from_cancel))
		return false;

	/*
	 * start_flush_work() returned %true. If @from_cancel is set, we know
	 * that @work must have been executing during start_flush_work() and
	 * can't currently be queued. Its data must contain OFFQ bits. If @work
	 * was queued on a BH workqueue, we also know that it was running in the
	 * BH context and thus can be busy-waited.
	 */
	if (from_cancel) {
		unsigned long data = *work_data_bits(work);

		if (!WARN_ON_ONCE(data & WORK_STRUCT_PWQ) &&
		    (data & WORK_OFFQ_BH)) {
			/*
			 * On RT, prevent a live lock when %current preempted
			 * soft interrupt processing by blocking on lock which
			 * is owned by the thread invoking the callback.
			 */
			while (!try_wait_for_completion(&barr.done)) {
				if (IS_ENABLED(CONFIG_PREEMPT_RT)) {
					struct worker_pool *pool;

					guard(rcu)();
					pool = get_work_pool(work);
					if (pool)
						workqueue_callback_cancel_wait_running(pool);
				} else {
					cpu_relax();
				}
			}
			goto out_destroy;
		}
	}

	wait_for_completion(&barr.done);

out_destroy:
	destroy_work_on_stack(&barr.work);
	return true;
}

/**
 * flush_work - wait for a work to finish executing the last queueing instance
 * @work: the work to flush
 *
 * Wait until @work has finished execution.  @work is guaranteed to be idle
 * on return if it hasn't been requeued since flush started.
 *
 * Return:
 * %true if flush_work() waited for the work to finish execution,
 * %false if it was already idle.
 */
/* flush_work() 是公开单 work 等待接口；参数/返回契约同 __flush_work(..., false)。 */
bool flush_work(struct work_struct *work)
{
	might_sleep();
	return __flush_work(work, false);
}
EXPORT_SYMBOL_GPL(flush_work);

/**
 * flush_delayed_work - wait for a dwork to finish executing the last queueing
 * @dwork: the delayed work to flush
 *
 * Delayed timer is cancelled and the pending work is queued for
 * immediate execution.  Like flush_work(), this function only
 * considers the last queueing instance of @dwork.
 *
 * Return:
 * %true if flush_work() waited for the work to finish execution,
 * %false if it was already idle.
 */
/*
 * flush_delayed_work() - 若 timer 尚待触发，先把 delayed work 立即入队，再等待回调。
 * @dwork 必须存活；可睡眠。返回是否确有 timer/pending/running 实例被推进或等待。
 * 它改变剩余延时为立即执行，不等同于只观察 timer。
 */
bool flush_delayed_work(struct delayed_work *dwork)
{
	local_irq_disable();
	if (timer_delete_sync(&dwork->timer))
		__queue_work(dwork->cpu, dwork->wq, &dwork->work);
	local_irq_enable();
	return flush_work(&dwork->work);
}
EXPORT_SYMBOL(flush_delayed_work);

/**
 * flush_rcu_work - wait for a rwork to finish executing the last queueing
 * @rwork: the rcu work to flush
 *
 * Return:
 * %true if flush_rcu_work() waited for the work to finish execution,
 * %false if it was already idle.
 */
/*
 * flush_rcu_work() - 等待 rcu_work 的 RCU callback 阶段及其 workqueue 执行阶段。
 * @rwork 必须存活；可睡眠。返回是否有 pending 实例；必要时先等待 grace callback
 * 把 work 入队，再 flush 内嵌 work。
 */
bool flush_rcu_work(struct rcu_work *rwork)
{
	if (test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&rwork->work))) {
		rcu_barrier();
		flush_work(&rwork->work);
		return true;
	} else {
		return flush_work(&rwork->work);
	}
}
EXPORT_SYMBOL(flush_rcu_work);

/* work_offqd_disable() 增加可嵌套 disable 深度，饱和/非法状态会 WARN；只改离队快照。 */
static void work_offqd_disable(struct work_offq_data *offqd)
{
	const unsigned long max = (1lu << WORK_OFFQ_DISABLE_BITS) - 1;

	if (likely(offqd->disable < max))
		offqd->disable++;
	else
		WARN_ONCE(true, "workqueue: work disable count overflowed\n");
}

/* work_offqd_enable() 减少 disable 深度；归零后下一次 queue 才可真正发布。 */
static void work_offqd_enable(struct work_offq_data *offqd)
{
	if (likely(offqd->disable > 0))
		offqd->disable--;
	else
		WARN_ONCE(true, "workqueue: work disable count underflowed\n");
}

/*
 * __cancel_work() - 非同步抢走 pending work，可选择 delayed/disable 语义。
 * @work 存活；@cflags 指定 timer 与禁用深度操作。可在原子上下文调用，不等待正在
 * 执行回调。返回 true 表示取消了 pending 实例，false 表示原本 idle/running；
 * 最终恢复 IRQ 并清 PENDING（disable 时携带新深度）。
 */
static bool __cancel_work(struct work_struct *work, u32 cflags)
{
	struct work_offq_data offqd;
	unsigned long irq_flags;
	int ret;

	ret = work_grab_pending(work, cflags, &irq_flags);

	work_offqd_unpack(&offqd, *work_data_bits(work));

	if (cflags & WORK_CANCEL_DISABLE)
		work_offqd_disable(&offqd);

	set_work_pool_and_clear_pending(work, offqd.pool_id,
					work_offqd_pack_flags(&offqd));
	local_irq_restore(irq_flags);
	return ret;
}

/*
 * __cancel_work_sync() - 抢走 pending 实例并等待任何正在执行实例退出。
 * @work 在函数返回前必须存活，函数可睡眠；返回是否取消到 pending 实例。
 * 它循环 grab + flush 关闭“执行完成同时又排队”的交接窗口，但仍要求调用者控制
 * 外部生产者，不能承诺返回后永远不会被再次 queue。
 */
static bool __cancel_work_sync(struct work_struct *work, u32 cflags)
{
	bool ret;

	ret = __cancel_work(work, cflags | WORK_CANCEL_DISABLE);

	if (*work_data_bits(work) & WORK_OFFQ_BH)
		WARN_ON_ONCE(in_hardirq());
	else
		might_sleep();

	/*
	 * Skip __flush_work() during early boot when we know that @work isn't
	 * executing. This allows canceling during early boot.
	 */
	if (wq_online)
		__flush_work(work, true);

	if (!(cflags & WORK_CANCEL_DISABLE))
		enable_work(work);

	return ret;
}

/*
 * See cancel_delayed_work()
 */
/*
 * cancel_work() - 取消尚未开始的普通 work，不等待正在执行实例。
 * @work 必须存活；可从原子上下文调用。返回 true 表示从 pending 队列取下，false
 * 表示 idle 或已运行。调用者若随后释放对象且可能正在执行，必须用 sync 版本。
 */
bool cancel_work(struct work_struct *work)
{
	return __cancel_work(work, 0);
}
EXPORT_SYMBOL(cancel_work);

/**
 * cancel_work_sync - cancel a work and wait for it to finish
 * @work: the work to cancel
 *
 * Cancel @work and wait for its execution to finish. This function can be used
 * even if the work re-queues itself or migrates to another workqueue. On return
 * from this function, @work is guaranteed to be not pending or executing on any
 * CPU as long as there aren't racing enqueues.
 *
 * cancel_work_sync(&delayed_work->work) must not be used for delayed_work's.
 * Use cancel_delayed_work_sync() instead.
 *
 * Must be called from a sleepable context if @work was last queued on a non-BH
 * workqueue. Can also be called from non-hardirq atomic contexts including BH
 * if @work was last queued on a BH workqueue.
 *
 * Returns %true if @work was pending, %false otherwise.
 */
/*
 * cancel_work_sync() - 取消 pending 并等待正在执行的普通 work。
 * 可睡眠；返回 true 仅表示入口时取消到 pending，false 也可能等待了 running 实例。
 * 返回后当前实例静止，但外部并发 queue 仍需调用者自行排除。
 */
bool cancel_work_sync(struct work_struct *work)
{
	return __cancel_work_sync(work, 0);
}
EXPORT_SYMBOL_GPL(cancel_work_sync);

/**
 * cancel_delayed_work - cancel a delayed work
 * @dwork: delayed_work to cancel
 *
 * Kill off a pending delayed_work.
 *
 * Return: %true if @dwork was pending and canceled; %false if it wasn't
 * pending.
 *
 * Note:
 * The work callback function may still be running on return, unless
 * it returns %true and the work doesn't re-arm itself.  Explicitly flush or
 * use cancel_delayed_work_sync() to wait on it.
 *
 * This function is safe to call from any context including IRQ handler.
 */
/*
 * cancel_delayed_work() - 尝试删除 timer 或队列中的 delayed work，不等待 running。
 * 原子上下文可用；返回 true 表示 pending 实例被取消。对象释放前需要 sync 版本。
 */
bool cancel_delayed_work(struct delayed_work *dwork)
{
	return __cancel_work(&dwork->work, WORK_CANCEL_DELAYED);
}
EXPORT_SYMBOL(cancel_delayed_work);

/**
 * cancel_delayed_work_sync - cancel a delayed work and wait for it to finish
 * @dwork: the delayed work cancel
 *
 * This is cancel_work_sync() for delayed works.
 *
 * Return:
 * %true if @dwork was pending, %false otherwise.
 */
/*
 * cancel_delayed_work_sync() - 删除 delayed timer/队列项并等待回调退出。
 * @dwork 必须持续存活，函数可睡眠；返回是否取消到 pending 实例。
 */
bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
	return __cancel_work_sync(&dwork->work, WORK_CANCEL_DELAYED);
}
EXPORT_SYMBOL(cancel_delayed_work_sync);

/**
 * disable_work - Disable and cancel a work item
 * @work: work item to disable
 *
 * Disable @work by incrementing its disable count and cancel it if currently
 * pending. As long as the disable count is non-zero, any attempt to queue @work
 * will fail and return %false. The maximum supported disable depth is 2 to the
 * power of %WORK_OFFQ_DISABLE_BITS, currently 65536.
 *
 * Can be called from any context. Returns %true if @work was pending, %false
 * otherwise.
 */
/*
 * disable_work() - 增加 work 的 disable 深度并取消 pending，但不等 running。
 * 返回是否取消 pending；禁用可嵌套，每次成功调用必须由 enable_work() 配平。
 * 原子上下文可用；running 回调仍可能访问对象。
 */
bool disable_work(struct work_struct *work)
{
	return __cancel_work(work, WORK_CANCEL_DISABLE);
}
EXPORT_SYMBOL_GPL(disable_work);

/**
 * disable_work_sync - Disable, cancel and drain a work item
 * @work: work item to disable
 *
 * Similar to disable_work() but also wait for @work to finish if currently
 * executing.
 *
 * Must be called from a sleepable context if @work was last queued on a non-BH
 * workqueue. Can also be called from non-hardirq atomic contexts including BH
 * if @work was last queued on a BH workqueue.
 *
 * Returns %true if @work was pending, %false otherwise.
 */
/*
 * disable_work_sync() - 增加 disable 深度、取消 pending 并等待 running 退出。
 * 可睡眠；返回是否取消 pending。返回后在对应 enable 前新 queue 会被抑制。
 */
bool disable_work_sync(struct work_struct *work)
{
	return __cancel_work_sync(work, WORK_CANCEL_DISABLE);
}
EXPORT_SYMBOL_GPL(disable_work_sync);

/**
 * enable_work - Enable a work item
 * @work: work item to enable
 *
 * Undo disable_work[_sync]() by decrementing @work's disable count. @work can
 * only be queued if its disable count is 0.
 *
 * Can be called from any context. Returns %true if the disable count reached 0.
 * Otherwise, %false.
 */
/*
 * enable_work() - 减少一层 disable，不自动补排被抑制的 work。
 * @work 必须 off-queue 且由调用者串行管理；返回 true 表示深度降到零、现在可 queue，
 * false 表示仍有嵌套禁用。无睡眠。
 */
bool enable_work(struct work_struct *work)
{
	struct work_offq_data offqd;
	unsigned long irq_flags;

	work_grab_pending(work, 0, &irq_flags);

	work_offqd_unpack(&offqd, *work_data_bits(work));
	work_offqd_enable(&offqd);
	set_work_pool_and_clear_pending(work, offqd.pool_id,
					work_offqd_pack_flags(&offqd));
	local_irq_restore(irq_flags);

	return !offqd.disable;
}
EXPORT_SYMBOL_GPL(enable_work);

/**
 * disable_delayed_work - Disable and cancel a delayed work item
 * @dwork: delayed work item to disable
 *
 * disable_work() for delayed work items.
 */
/* disable_delayed_work() 对 timer/队列 pending 实例执行非同步取消并增加 disable 深度。 */
bool disable_delayed_work(struct delayed_work *dwork)
{
	return __cancel_work(&dwork->work,
			     WORK_CANCEL_DELAYED | WORK_CANCEL_DISABLE);
}
EXPORT_SYMBOL_GPL(disable_delayed_work);

/**
 * disable_delayed_work_sync - Disable, cancel and drain a delayed work item
 * @dwork: delayed work item to disable
 *
 * disable_work_sync() for delayed work items.
 */
/* disable_delayed_work_sync() 还等待 delayed work 当前回调退出；可睡眠。 */
bool disable_delayed_work_sync(struct delayed_work *dwork)
{
	return __cancel_work_sync(&dwork->work,
				  WORK_CANCEL_DELAYED | WORK_CANCEL_DISABLE);
}
EXPORT_SYMBOL_GPL(disable_delayed_work_sync);

/**
 * enable_delayed_work - Enable a delayed work item
 * @dwork: delayed work item to enable
 *
 * enable_work() for delayed work items.
 */
/* enable_delayed_work() 减内嵌 work 的 disable 深度，不重新启动已取消 timer。 */
bool enable_delayed_work(struct delayed_work *dwork)
{
	return enable_work(&dwork->work);
}
EXPORT_SYMBOL_GPL(enable_delayed_work);

/**
 * schedule_on_each_cpu - execute a function synchronously on each online CPU
 * @func: the function to call
 *
 * schedule_on_each_cpu() executes @func on each online CPU using the
 * system workqueue and blocks until all CPUs have completed.
 * schedule_on_each_cpu() is very slow.
 *
 * Return:
 * 0 on success, -errno on failure.
 */
/*
 * schedule_on_each_cpu() - 为每个在线 CPU 排一个同步 work 并等待全部完成。
 * @func 在各 CPU 的 system_percpu_wq 进程上下文调用；函数可睡眠。成功返回 0，
 * 分配 per-CPU work 失败返回 -ENOMEM。hotplug 读锁稳定在线集合，返回前释放所有
 * 临时 work 存储。
 */
int schedule_on_each_cpu(work_func_t func)
{
	int cpu;
	struct work_struct __percpu *works;

	works = alloc_percpu(struct work_struct);
	if (!works)
		return -ENOMEM;

	cpus_read_lock();

	for_each_online_cpu(cpu) {
		struct work_struct *work = per_cpu_ptr(works, cpu);

		INIT_WORK(work, func);
		schedule_work_on(cpu, work);
	}

	for_each_online_cpu(cpu)
		flush_work(per_cpu_ptr(works, cpu));

	cpus_read_unlock();
	free_percpu(works);
	return 0;
}

/**
 * execute_in_process_context - reliably execute the routine with user context
 * @fn:		the function to execute
 * @ew:		guaranteed storage for the execute work structure (must
 *		be available when the work executes)
 *
 * Executes the function immediately if process context is available,
 * otherwise schedules the function for delayed execution.
 *
 * Return:	0 - function was executed
 *		1 - function was scheduled for execution
 */
/*
 * execute_in_process_context() - 若已在进程上下文直接调用 @fn，否则排到 system_wq。
 * @ew 是调用者提供且须活到完成的桥接 work。直接执行返回 0；异步排队返回 1。
 * IRQ 路径不睡眠，调用者可用返回值判断 @ew 生命周期是否已转给 workqueue。
 */
int execute_in_process_context(work_func_t fn, struct execute_work *ew)
{
	if (!in_interrupt()) {
		fn(&ew->work);
		return 0;
	}

	INIT_WORK(&ew->work, fn);
	schedule_work(&ew->work);

	return 1;
}
EXPORT_SYMBOL_GPL(execute_in_process_context);

/**
 * free_workqueue_attrs - free a workqueue_attrs
 * @attrs: workqueue_attrs to free
 *
 * Undo alloc_workqueue_attrs().
 */
/*
 * free_workqueue_attrs() - 释放 attrs 及其动态 cpumask。
 * @attrs 可为 NULL，ownership 由调用者转交；返回无。只能释放 alloc_workqueue_attrs()
 * 创建且未被 pool/wq 继续引用的对象。
 */
void free_workqueue_attrs(struct workqueue_attrs *attrs)
{
	if (attrs) {
		free_cpumask_var(attrs->cpumask);
		free_cpumask_var(attrs->__pod_cpumask);
		kfree(attrs);
	}
}

/**
 * alloc_workqueue_attrs - allocate a workqueue_attrs
 *
 * Allocate a new workqueue_attrs, initialize with default settings and
 * return it.
 *
 * Return: The allocated new workqueue_attr on success. %NULL on failure.
 */
/*
 * alloc_workqueue_attrs_noprof() - 分配带默认 nice/cpumask/affinity 的 attrs。
 * 可睡眠；成功返回调用者拥有的对象，失败 NULL。noprof 表示本分配不归因到调用者
 * 内存分析标签，不改变对象释放契约。
 */
struct workqueue_attrs *alloc_workqueue_attrs_noprof(void)
{
	struct workqueue_attrs *attrs;

	attrs = kzalloc_obj(*attrs);
	if (!attrs)
		goto fail;
	if (!alloc_cpumask_var(&attrs->cpumask, GFP_KERNEL))
		goto fail;
	if (!alloc_cpumask_var(&attrs->__pod_cpumask, GFP_KERNEL))
		goto fail;

	cpumask_copy(attrs->cpumask, cpu_possible_mask);
	attrs->affn_scope = WQ_AFFN_DFL;
	return attrs;
fail:
	free_workqueue_attrs(attrs);
	return NULL;
}

/* copy_workqueue_attrs() 深拷贝 mask 与标量策略；@to 已分配，返回无且不转移 @from。 */
static void copy_workqueue_attrs(struct workqueue_attrs *to,
				 const struct workqueue_attrs *from)
{
	to->nice = from->nice;
	cpumask_copy(to->cpumask, from->cpumask);
	cpumask_copy(to->__pod_cpumask, from->__pod_cpumask);
	to->affn_strict = from->affn_strict;

	/*
	 * Unlike hash and equality test, copying shouldn't ignore wq-only
	 * fields as copying is used for both pool and wq attrs. Instead,
	 * get_unbound_pool() explicitly clears the fields.
	 */
	to->affn_scope = from->affn_scope;
	to->ordered = from->ordered;
}

/*
 * Some attrs fields are workqueue-only. Clear them for worker_pool's. See the
 * comments in 'struct workqueue_attrs' definition.
 */
/* wqattrs_clear_for_pool() 清只属于 wq 请求层、不参与 pool 复用键的临时字段。 */
static void wqattrs_clear_for_pool(struct workqueue_attrs *attrs)
{
	attrs->affn_scope = WQ_AFFN_NR_TYPES;
	attrs->ordered = false;
	if (attrs->affn_strict)
		cpumask_copy(attrs->cpumask, cpu_possible_mask);
}

/* hash value of the content of @attr */
/* wqattrs_hash() 对决定 pool 等价性的 attrs 字段生成 hash；纯读、无引用变化。 */
static u32 wqattrs_hash(const struct workqueue_attrs *attrs)
{
	u32 hash = 0;

	hash = jhash_1word(attrs->nice, hash);
	hash = jhash_1word(attrs->affn_strict, hash);
	hash = jhash(cpumask_bits(attrs->__pod_cpumask),
		     BITS_TO_LONGS(nr_cpumask_bits) * sizeof(long), hash);
	if (!attrs->affn_strict)
		hash = jhash(cpumask_bits(attrs->cpumask),
			     BITS_TO_LONGS(nr_cpumask_bits) * sizeof(long), hash);
	return hash;
}

/* content equality test */
/* wqattrs_equal() 比较两个 attrs 是否可安全共享同一 unbound pool。 */
static bool wqattrs_equal(const struct workqueue_attrs *a,
			  const struct workqueue_attrs *b)
{
	if (a->nice != b->nice)
		return false;
	if (a->affn_strict != b->affn_strict)
		return false;
	if (!cpumask_equal(a->__pod_cpumask, b->__pod_cpumask))
		return false;
	if (!a->affn_strict && !cpumask_equal(a->cpumask, b->cpumask))
		return false;
	return true;
}

/* Update @attrs with actually available CPUs */
/*
 * wqattrs_actualize_cpumask() - 把请求 mask 与全局允许集/在线集落实为 pool 实际 mask。
 * @attrs 输入输出，@unbound_cpumask 借用；返回无。交集为空时采用保活 fallback，
 * 不让 pool 失去所有可执行 CPU。
 */
static void wqattrs_actualize_cpumask(struct workqueue_attrs *attrs,
				      const cpumask_t *unbound_cpumask)
{
	/*
	 * Calculate the effective CPU mask of @attrs given @unbound_cpumask. If
	 * @attrs->cpumask doesn't overlap with @unbound_cpumask, we fallback to
	 * @unbound_cpumask.
	 */
	cpumask_and(attrs->cpumask, attrs->cpumask, unbound_cpumask);
	if (unlikely(cpumask_empty(attrs->cpumask)))
		cpumask_copy(attrs->cpumask, unbound_cpumask);
}

/* find wq_pod_type to use for @attrs */
/*
 * wqattrs_pod_type() - 根据 attrs affinity scope 选择已初始化 pod 拓扑。
 * 返回全局只读对象借用指针或无需分 pod 时 NULL；不睡眠。
 */
static const struct wq_pod_type *
wqattrs_pod_type(const struct workqueue_attrs *attrs)
{
	enum wq_affn_scope scope;
	struct wq_pod_type *pt;

	/* to synchronize access to wq_affn_dfl */
	lockdep_assert_held(&wq_pool_mutex);

	if (attrs->affn_scope == WQ_AFFN_DFL)
		scope = wq_affn_dfl;
	else
		scope = attrs->affn_scope;

	pt = &wq_pod_types[scope];

	if (!WARN_ON_ONCE(attrs->affn_scope == WQ_AFFN_NR_TYPES) &&
	    likely(pt->nr_pods))
		return pt;

	/*
	 * Before workqueue_init_topology(), only SYSTEM is available which is
	 * initialized in workqueue_init_early().
	 */
	pt = &wq_pod_types[WQ_AFFN_SYSTEM];
	BUG_ON(!pt->nr_pods);
	return pt;
}

/**
 * init_worker_pool - initialize a newly zalloc'd worker_pool
 * @pool: worker_pool to initialize
 *
 * Initialize a newly zalloc'd @pool.  It also allocates @pool->attrs.
 *
 * Return: 0 on success, -errno on failure.  Even on failure, all fields
 * inside @pool proper are initialized and put_unbound_pool() can be called
 * on @pool safely to release it.
 */
/*
 * init_worker_pool() - 初始化一个尚未发布的 pool 的锁、链表、timer、work 和默认字段。
 * @pool 由调用者拥有；可睡眠的分配失败返回负 errno，成功 0。成功不等于已分配 ID、
 * attrs 或 worker，后续构造阶段负责，失败由调用者释放已建资源。
 */
static int init_worker_pool(struct worker_pool *pool)
{
	raw_spin_lock_init(&pool->lock);
	pool->id = -1;
	pool->cpu = -1;
	pool->node = NUMA_NO_NODE;
	pool->flags |= POOL_DISASSOCIATED;
	pool->last_progress_ts = jiffies;
	INIT_LIST_HEAD(&pool->worklist);
	INIT_LIST_HEAD(&pool->idle_list);
	hash_init(pool->busy_hash);

	timer_setup(&pool->idle_timer, idle_worker_timeout, TIMER_DEFERRABLE);
	INIT_WORK(&pool->idle_cull_work, idle_cull_fn);

	timer_setup(&pool->mayday_timer, pool_mayday_timeout, 0);

	INIT_LIST_HEAD(&pool->workers);

	ida_init(&pool->worker_ida);
	INIT_HLIST_NODE(&pool->hash_node);
	pool->refcnt = 1;
#ifdef CONFIG_PREEMPT_RT
	spin_lock_init(&pool->cb_lock);
#endif

	/* shouldn't fail above this point */
	pool->attrs = alloc_workqueue_attrs();
	if (!pool->attrs)
		return -ENOMEM;

	wqattrs_clear_for_pool(pool->attrs);

	return 0;
}

#ifdef CONFIG_LOCKDEP
/* wq_init_lockdep() 为新 wq 建立独立 lock class/map，供 flush 依赖检测；仅调试状态。 */
static void wq_init_lockdep(struct workqueue_struct *wq)
{
	char *lock_name;

	lockdep_register_key(&wq->key);
	lock_name = kasprintf(GFP_KERNEL, "%s%s", "(wq_completion)", wq->name);
	if (!lock_name)
		lock_name = wq->name;

	wq->lock_name = lock_name;
	wq->lockdep_map = &wq->__lockdep_map;
	lockdep_init_map(wq->lockdep_map, lock_name, &wq->key, 0);
}

/* wq_unregister_lockdep() 在 wq 摘除时停止其 lockdep key 注册，防止后续新依赖。 */
static void wq_unregister_lockdep(struct workqueue_struct *wq)
{
	if (wq->lockdep_map != &wq->__lockdep_map)
		return;

	lockdep_unregister_key(&wq->key);
}

/* wq_free_lockdep() 最终释放动态 lock_name；调用时已无并发 lockdep 使用者。 */
static void wq_free_lockdep(struct workqueue_struct *wq)
{
	if (wq->lockdep_map != &wq->__lockdep_map)
		return;

	if (wq->lock_name != wq->name)
		kfree(wq->lock_name);
}
#else
/* CONFIG_LOCKDEP 关闭时三者为零副作用 stub，正式 workqueue 生命周期不依赖调试图。 */
static void wq_init_lockdep(struct workqueue_struct *wq)
{
}

static void wq_unregister_lockdep(struct workqueue_struct *wq)
{
}

static void wq_free_lockdep(struct workqueue_struct *wq)
{
}
#endif

/* free_node_nr_active() 释放 wq 的逐 node active 账户数组；调用前必须无并发使用者。 */
static void free_node_nr_active(struct wq_node_nr_active **nna_ar)
{
	int node;

	for_each_node(node) {
		kfree(nna_ar[node]);
		nna_ar[node] = NULL;
	}

	kfree(nna_ar[nr_node_ids]);
	nna_ar[nr_node_ids] = NULL;
}

/* init_node_nr_active() 初始化单个共享计数、内层锁和 pending 链；对象尚未发布。 */
static void init_node_nr_active(struct wq_node_nr_active *nna)
{
	nna->max = WQ_DFL_MIN_ACTIVE;
	atomic_set(&nna->nr, 0);
	raw_spin_lock_init(&nna->lock);
	INIT_LIST_HEAD(&nna->pending_pwqs);
}

/*
 * Each node's nr_active counter will be accessed mostly from its own node and
 * should be allocated in the node.
 */
/*
 * alloc_node_nr_active() - 为所有可能 node 加 NUMA_NO_NODE 哨兵槽分配并初始化账户。
 * 可睡眠；成功 0 并填数组，失败 -ENOMEM 且回滚已分配项。
 */
static int alloc_node_nr_active(struct wq_node_nr_active **nna_ar)
{
	struct wq_node_nr_active *nna;
	int node;

	for_each_node(node) {
		nna = kzalloc_node(sizeof(*nna), GFP_KERNEL, node);
		if (!nna)
			goto err_free;
		init_node_nr_active(nna);
		nna_ar[node] = nna;
	}

	/* [nr_node_ids] is used as the fallback */
	nna = kzalloc_node(sizeof(*nna), GFP_KERNEL, NUMA_NO_NODE);
	if (!nna)
		goto err_free;
	init_node_nr_active(nna);
	nna_ar[nr_node_ids] = nna;

	return 0;

err_free:
	free_node_nr_active(nna_ar);
	return -ENOMEM;
}

/* rcu_free_wq() 在宽限期后释放已摘除 wq 及 node 账户/名称等最终存储。 */
static void rcu_free_wq(struct rcu_head *rcu)
{
	struct workqueue_struct *wq =
		container_of(rcu, struct workqueue_struct, rcu);

	if (wq->flags & WQ_UNBOUND)
		free_node_nr_active(wq->node_nr_active);

	wq_free_lockdep(wq);
	free_percpu(wq->cpu_pwq);
	free_workqueue_attrs(wq->unbound_attrs);
	kfree(wq);
}

/* rcu_free_pool() 在 IDR/hash 读者退出后释放 unbound pool attrs 与对象内存。 */
static void rcu_free_pool(struct rcu_head *rcu)
{
	struct worker_pool *pool = container_of(rcu, struct worker_pool, rcu);

	ida_destroy(&pool->worker_ida);
	free_workqueue_attrs(pool->attrs);
	kfree(pool);
}

/**
 * put_unbound_pool - put a worker_pool
 * @pool: worker_pool to put
 *
 * Put @pool.  If its refcnt reaches zero, it gets destroyed in RCU
 * safe manner.  get_unbound_pool() calls this function on its failure path
 * and this function should be able to release pools which went through,
 * successfully or not, init_worker_pool().
 *
 * Should be called with wq_pool_mutex held.
 */
/*
 * put_unbound_pool() - 在 wq_pool_mutex 下归还 unbound pool 共享引用。
 * 非最后引用仅减计数；最后引用先从 hash/IDR 摘除、停止 worker/timer，再 call_rcu
 * 最终释放。返回无，可睡眠，调用后不得再用裸 pool。
 */
static void put_unbound_pool(struct worker_pool *pool)
{
	struct worker *worker;
	LIST_HEAD(cull_list);

	lockdep_assert_held(&wq_pool_mutex);

	if (--pool->refcnt)
		return;

	/* sanity checks */
	if (WARN_ON(!(pool->cpu < 0)) ||
	    WARN_ON(!list_empty(&pool->worklist)))
		return;

	/* release id and unhash */
	if (pool->id >= 0)
		idr_remove(&worker_pool_idr, pool->id);
	hash_del(&pool->hash_node);

	/*
	 * Become the manager and destroy all workers.  This prevents
	 * @pool's workers from blocking on attach_mutex.  We're the last
	 * manager and @pool gets freed with the flag set.
	 *
	 * Having a concurrent manager is quite unlikely to happen as we can
	 * only get here with
	 *   pwq->refcnt == pool->refcnt == 0
	 * which implies no work queued to the pool, which implies no worker can
	 * become the manager. However a worker could have taken the role of
	 * manager before the refcnts dropped to 0, since maybe_create_worker()
	 * drops pool->lock
	 */
	while (true) {
		rcuwait_wait_event(&manager_wait,
				   !(pool->flags & POOL_MANAGER_ACTIVE),
				   TASK_UNINTERRUPTIBLE);

		mutex_lock(&wq_pool_attach_mutex);
		raw_spin_lock_irq(&pool->lock);
		if (!(pool->flags & POOL_MANAGER_ACTIVE)) {
			pool->flags |= POOL_MANAGER_ACTIVE;
			break;
		}
		raw_spin_unlock_irq(&pool->lock);
		mutex_unlock(&wq_pool_attach_mutex);
	}

	while ((worker = first_idle_worker(pool)))
		set_worker_dying(worker, &cull_list);
	WARN_ON(pool->nr_workers || pool->nr_idle);
	raw_spin_unlock_irq(&pool->lock);

	detach_dying_workers(&cull_list);

	mutex_unlock(&wq_pool_attach_mutex);

	reap_dying_workers(&cull_list);

	/* shut down the timers */
	timer_delete_sync(&pool->idle_timer);
	cancel_work_sync(&pool->idle_cull_work);
	timer_delete_sync(&pool->mayday_timer);

	/* RCU protected to allow dereferences from get_work_pool() */
	call_rcu(&pool->rcu, rcu_free_pool);
}

/**
 * get_unbound_pool - get a worker_pool with the specified attributes
 * @attrs: the attributes of the worker_pool to get
 *
 * Obtain a worker_pool which has the same attributes as @attrs, bump the
 * reference count and return it.  If there already is a matching
 * worker_pool, it will be used; otherwise, this function attempts to
 * create a new one.
 *
 * Should be called with wq_pool_mutex held.
 *
 * Return: On success, a worker_pool with the same attributes as @attrs.
 * On failure, %NULL.
 */
/*
 * get_unbound_pool() - 按实际 attrs 复用或创建 unbound pool。
 * 入口持 wq_pool_mutex，可睡眠。命中返回增加 refcnt 的 pool；否则完整初始化、分 ID、
 * 建首 worker、发布 hash 后返回；失败 NULL 并逆序回滚，调用者仍拥有 attrs 输入。
 */
static struct worker_pool *get_unbound_pool(const struct workqueue_attrs *attrs)
{
	struct wq_pod_type *pt = &wq_pod_types[WQ_AFFN_NUMA];
	u32 hash = wqattrs_hash(attrs);
	struct worker_pool *pool;
	int pod, node = NUMA_NO_NODE;

	lockdep_assert_held(&wq_pool_mutex);

	/* do we already have a matching pool? */
	hash_for_each_possible(unbound_pool_hash, pool, hash_node, hash) {
		if (wqattrs_equal(pool->attrs, attrs)) {
			pool->refcnt++;
			return pool;
		}
	}

	/* If __pod_cpumask is contained inside a NUMA pod, that's our node */
	for (pod = 0; pod < pt->nr_pods; pod++) {
		if (cpumask_subset(attrs->__pod_cpumask, pt->pod_cpus[pod])) {
			node = pt->pod_node[pod];
			break;
		}
	}

	/* nope, create a new one */
	pool = kzalloc_node(sizeof(*pool), GFP_KERNEL, node);
	if (!pool || init_worker_pool(pool) < 0)
		goto fail;

	pool->node = node;
	copy_workqueue_attrs(pool->attrs, attrs);
	wqattrs_clear_for_pool(pool->attrs);

	if (worker_pool_assign_id(pool) < 0)
		goto fail;

	/* create and start the initial worker */
	if (wq_online && !create_worker(pool))
		goto fail;

	/* install */
	hash_add(unbound_pool_hash, &pool->hash_node, hash);

	return pool;
fail:
	if (pool)
		put_unbound_pool(pool);
	return NULL;
}

/*
 * Scheduled on pwq_release_worker by put_pwq() when an unbound pwq hits zero
 * refcnt and needs to be destroyed.
 */
/*
 * pwq_release_workfn() - 在专用 kthread 中完成 refcnt 已归零的 pwq 销毁。
 * 从 wq/pool 拓扑摘除、归还 unbound pool，并经 RCU 释放 pwq。可睡眠；返回无。
 * ordered wq 摘掉最老 pwq 后会放行下一代。
 */
static void pwq_release_workfn(struct kthread_work *work)
{
	struct pool_workqueue *pwq = container_of(work, struct pool_workqueue,
						  release_work);
	struct workqueue_struct *wq = pwq->wq;
	struct worker_pool *pool = pwq->pool;
	bool is_last = false;

	/*
	 * When @pwq is not linked, it doesn't hold any reference to the
	 * @wq, and @wq is invalid to access.
	 */
	if (!list_empty(&pwq->pwqs_node)) {
		mutex_lock(&wq->mutex);
		list_del_rcu(&pwq->pwqs_node);
		is_last = list_empty(&wq->pwqs);

		/*
		 * For ordered workqueue with a plugged dfl_pwq, restart it now.
		 */
		if (!is_last && (wq->flags & __WQ_ORDERED))
			unplug_oldest_pwq(wq);

		mutex_unlock(&wq->mutex);
	}

	if (wq->flags & WQ_UNBOUND) {
		mutex_lock(&wq_pool_mutex);
		put_unbound_pool(pool);
		mutex_unlock(&wq_pool_mutex);
	}

	if (!list_empty(&pwq->pending_node)) {
		struct wq_node_nr_active *nna =
			wq_node_nr_active(pwq->wq, pwq->pool->node);

		raw_spin_lock_irq(&nna->lock);
		list_del_init(&pwq->pending_node);
		raw_spin_unlock_irq(&nna->lock);
	}

	kfree_rcu(pwq, rcu);

	/*
	 * If we're the last pwq going away, @wq is already dead and no one
	 * is gonna access it anymore.  Schedule RCU free.
	 */
	if (is_last) {
		wq_unregister_lockdep(wq);
		call_rcu(&wq->rcu, rcu_free_wq);
	}
}

/* initialize newly allocated @pwq which is associated with @wq and @pool */
/*
 * init_pwq() - 初始化尚未发布的 wq↔pool 连接及基础引用/统计/链表。
 * @pwq 输入输出，@wq/@pool 借用并将由 pwq 关联；返回无，不入全局链。
 */
static void init_pwq(struct pool_workqueue *pwq, struct workqueue_struct *wq,
		     struct worker_pool *pool)
{
	BUG_ON((unsigned long)pwq & ~WORK_STRUCT_PWQ_MASK);

	memset(pwq, 0, sizeof(*pwq));

	pwq->pool = pool;
	pwq->wq = wq;
	pwq->flush_color = -1;
	pwq->refcnt = 1;
	INIT_LIST_HEAD(&pwq->inactive_works);
	INIT_LIST_HEAD(&pwq->pending_node);
	INIT_LIST_HEAD(&pwq->pwqs_node);
	INIT_LIST_HEAD(&pwq->mayday_node);
	kthread_init_work(&pwq->release_work, pwq_release_workfn);

	/*
	 * Set the dummy cursor work with valid function and get_work_pwq().
	 *
	 * The cursor work should only be in the pwq->pool->worklist, and
	 * should not be treated as a processable work item.
	 *
	 * WORK_STRUCT_PENDING and WORK_STRUCT_INACTIVE just make it less
	 * surprise for kernel debugging tools and reviewers.
	 */
	INIT_WORK(&pwq->mayday_cursor, mayday_cursor_func);
	atomic_long_set(&pwq->mayday_cursor.data, (unsigned long)pwq |
			WORK_STRUCT_PENDING | WORK_STRUCT_PWQ | WORK_STRUCT_INACTIVE);
}

/* sync @pwq with the current state of its associated wq and link it */
/*
 * link_pwq() - 在 wq mutex/pool lock 协议下把已初始化 pwq 发布到 wq->pwqs。
 * 建立可被 RCU/debug/flush 发现的关系；返回无，重复 link 属内部错误。
 */
static void link_pwq(struct pool_workqueue *pwq)
{
	struct workqueue_struct *wq = pwq->wq;

	lockdep_assert_held(&wq->mutex);

	/* may be called multiple times, ignore if already linked */
	if (!list_empty(&pwq->pwqs_node))
		return;

	/* set the matching work_color */
	pwq->work_color = wq->work_color;

	/* link in @pwq */
	list_add_tail_rcu(&pwq->pwqs_node, &wq->pwqs);
}

/* obtain a pool matching @attr and create a pwq associating the pool and @wq */
/*
 * alloc_unbound_pwq() - 为 attrs 获取共享 pool 并分配/初始化连接对象。
 * 可睡眠；成功返回调用者拥有、尚待安装的 pwq；失败 NULL 并归还 pool 引用。
 */
static struct pool_workqueue *alloc_unbound_pwq(struct workqueue_struct *wq,
					const struct workqueue_attrs *attrs)
{
	struct worker_pool *pool;
	struct pool_workqueue *pwq;

	lockdep_assert_held(&wq_pool_mutex);

	pool = get_unbound_pool(attrs);
	if (!pool)
		return NULL;

	pwq = kmem_cache_alloc_node(pwq_cache, GFP_KERNEL, pool->node);
	if (!pwq) {
		put_unbound_pool(pool);
		return NULL;
	}

	init_pwq(pwq, wq, pool);
	return pwq;
}

/**
 * wq_calc_pod_cpumask - calculate a wq_attrs' cpumask for a pod
 * @attrs: the wq_attrs of the default pwq of the target workqueue
 * @cpu: the target CPU
 *
 * Calculate the cpumask a workqueue with @attrs should use on @pod.
 * The result is stored in @attrs->__pod_cpumask.
 *
 * If pod affinity is not enabled, @attrs->cpumask is always used. If enabled
 * and @pod has online CPUs requested by @attrs, the returned cpumask is the
 * intersection of the possible CPUs of @pod and @attrs->cpumask.
 *
 * The caller is responsible for ensuring that the cpumask of @pod stays stable.
 */
/* wq_calc_pod_cpumask() 按 @cpu 所属 affinity pod 填 attrs->__pod_cpumask。 */
static void wq_calc_pod_cpumask(struct workqueue_attrs *attrs, int cpu)
{
	const struct wq_pod_type *pt = wqattrs_pod_type(attrs);
	int pod = pt->cpu_pod[cpu];

	/* calculate possible CPUs in @pod that @attrs wants */
	cpumask_and(attrs->__pod_cpumask, pt->pod_cpus[pod], attrs->cpumask);
	/* does @pod have any online CPUs @attrs wants? */
	if (!cpumask_intersects(attrs->__pod_cpumask, wq_online_cpumask)) {
		cpumask_copy(attrs->__pod_cpumask, attrs->cpumask);
		return;
	}
}

/* install @pwq into @wq and return the old pwq, @cpu < 0 for dfl_pwq */
/*
 * install_unbound_pwq() - 把准备好的 pwq 安装到 unbound wq 的 CPU/default 槽。
 * 相关 mutex 已持有；返回被替换旧 pwq 的持有引用供稍后 put，或 NULL。RCU 发布后
 * 新 queue 立即可见新连接，旧对象靠引用/RCU 完成在途访问。
 */
static struct pool_workqueue *install_unbound_pwq(struct workqueue_struct *wq,
					int cpu, struct pool_workqueue *pwq)
{
	struct pool_workqueue __rcu **slot = unbound_pwq_slot(wq, cpu);
	struct pool_workqueue *old_pwq;

	lockdep_assert_held(&wq_pool_mutex);
	lockdep_assert_held(&wq->mutex);

	/* link_pwq() can handle duplicate calls */
	link_pwq(pwq);

	old_pwq = rcu_access_pointer(*slot);
	rcu_assign_pointer(*slot, pwq);
	return old_pwq;
}

/* context to store the prepared attrs & pwqs before applying */
struct apply_wqattrs_ctx {
	struct workqueue_struct	*wq;		/* target workqueue */
	struct workqueue_attrs	*attrs;		/* attrs to apply */
	struct list_head	list;		/* queued for batching commit */
	struct pool_workqueue	*dfl_pwq;
	struct pool_workqueue	*pwq_tbl[];
};

/* free the resources after success or abort */
/* apply_wqattrs_cleanup() 释放 prepare 阶段未提交的新 pwq/attrs 与上下文；可空。 */
static void apply_wqattrs_cleanup(struct apply_wqattrs_ctx *ctx)
{
	if (ctx) {
		int cpu;

		for_each_possible_cpu(cpu)
			put_pwq_unlocked(ctx->pwq_tbl[cpu]);
		put_pwq_unlocked(ctx->dfl_pwq);

		free_workqueue_attrs(ctx->attrs);

		kfree(ctx);
	}
}

/* allocate the attrs and pwqs for later installation */
/*
 * apply_wqattrs_prepare() - 为一次 unbound attrs 更新预建所有可能需要的 pwq。
 * 可睡眠；成功返回调用者拥有的事务上下文，失败 ERR_PTR(errno) 且无部分发布。
 * prepare/commit 分离保证分配失败时现有 wq 拓扑保持可用。
 */
static struct apply_wqattrs_ctx *
apply_wqattrs_prepare(struct workqueue_struct *wq,
		      const struct workqueue_attrs *attrs,
		      const cpumask_var_t unbound_cpumask)
{
	struct apply_wqattrs_ctx *ctx;
	struct workqueue_attrs *new_attrs;
	int cpu;

	lockdep_assert_held(&wq_pool_mutex);

	if (WARN_ON(attrs->affn_scope < 0 ||
		    attrs->affn_scope >= WQ_AFFN_NR_TYPES))
		return ERR_PTR(-EINVAL);

	ctx = kzalloc_flex(*ctx, pwq_tbl, nr_cpu_ids);

	new_attrs = alloc_workqueue_attrs();
	if (!ctx || !new_attrs)
		goto out_free;

	/*
	 * If something goes wrong during CPU up/down, we'll fall back to
	 * the default pwq covering whole @attrs->cpumask.  Always create
	 * it even if we don't use it immediately.
	 */
	copy_workqueue_attrs(new_attrs, attrs);
	wqattrs_actualize_cpumask(new_attrs, unbound_cpumask);
	cpumask_copy(new_attrs->__pod_cpumask, new_attrs->cpumask);
	ctx->dfl_pwq = alloc_unbound_pwq(wq, new_attrs);
	if (!ctx->dfl_pwq)
		goto out_free;

	for_each_possible_cpu(cpu) {
		if (new_attrs->ordered) {
			ctx->dfl_pwq->refcnt++;
			ctx->pwq_tbl[cpu] = ctx->dfl_pwq;
		} else {
			wq_calc_pod_cpumask(new_attrs, cpu);
			ctx->pwq_tbl[cpu] = alloc_unbound_pwq(wq, new_attrs);
			if (!ctx->pwq_tbl[cpu])
				goto out_free;
		}
	}

	/* save the user configured attrs and sanitize it. */
	copy_workqueue_attrs(new_attrs, attrs);
	cpumask_and(new_attrs->cpumask, new_attrs->cpumask, cpu_possible_mask);
	cpumask_copy(new_attrs->__pod_cpumask, new_attrs->cpumask);
	ctx->attrs = new_attrs;

	/*
	 * For initialized ordered workqueues, there should only be one pwq
	 * (dfl_pwq). Set the plugged flag of ctx->dfl_pwq to suspend execution
	 * of newly queued work items until execution of older work items in
	 * the old pwq's have completed.
	 */
	if ((wq->flags & __WQ_ORDERED) && !list_empty(&wq->pwqs))
		ctx->dfl_pwq->plugged = true;

	ctx->wq = wq;
	return ctx;

out_free:
	free_workqueue_attrs(new_attrs);
	apply_wqattrs_cleanup(ctx);
	return ERR_PTR(-ENOMEM);
}

/* set attrs and install prepared pwqs, @ctx points to old pwqs on return */
/*
 * apply_wqattrs_commit() - 在锁内原子切换 prepare 好的 attrs/pwq 映射。
 * @ctx ownership 被消费；返回无。旧 pwq 引用在发布新槽后归还，ordered wq 通过
 * plugged 代际保证跨映射顺序。
 */
static void apply_wqattrs_commit(struct apply_wqattrs_ctx *ctx)
{
	int cpu;

	/* all pwqs have been created successfully, let's install'em */
	mutex_lock(&ctx->wq->mutex);

	copy_workqueue_attrs(ctx->wq->unbound_attrs, ctx->attrs);

	/* save the previous pwqs and install the new ones */
	for_each_possible_cpu(cpu)
		ctx->pwq_tbl[cpu] = install_unbound_pwq(ctx->wq, cpu,
							ctx->pwq_tbl[cpu]);
	ctx->dfl_pwq = install_unbound_pwq(ctx->wq, -1, ctx->dfl_pwq);

	/* update node_nr_active->max */
	wq_update_node_max_active(ctx->wq, -1);

	mutex_unlock(&ctx->wq->mutex);
}

/*
 * apply_workqueue_attrs_locked() - 在 wq_pool_mutex 下对 unbound wq 执行 prepare/commit。
 * @attrs 只读借用；可睡眠。成功 0，失败负 errno 且旧配置继续生效。
 */
static int apply_workqueue_attrs_locked(struct workqueue_struct *wq,
					const struct workqueue_attrs *attrs)
{
	struct apply_wqattrs_ctx *ctx;

	/* only unbound workqueues can change attributes */
	if (WARN_ON(!(wq->flags & WQ_UNBOUND)))
		return -EINVAL;

	ctx = apply_wqattrs_prepare(wq, attrs, wq_unbound_cpumask);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	/* the ctx has been prepared successfully, let's commit it */
	apply_wqattrs_commit(ctx);
	apply_wqattrs_cleanup(ctx);

	return 0;
}

/**
 * apply_workqueue_attrs - apply new workqueue_attrs to an unbound workqueue
 * @wq: the target workqueue
 * @attrs: the workqueue_attrs to apply, allocated with alloc_workqueue_attrs()
 *
 * Apply @attrs to an unbound workqueue @wq. Unless disabled, this function maps
 * a separate pwq to each CPU pod with possibles CPUs in @attrs->cpumask so that
 * work items are affine to the pod it was issued on. Older pwqs are released as
 * in-flight work items finish. Note that a work item which repeatedly requeues
 * itself back-to-back will stay on its current pwq.
 *
 * Performs GFP_KERNEL allocations.
 *
 * Return: 0 on success and -errno on failure.
 */
int apply_workqueue_attrs(struct workqueue_struct *wq,
			  const struct workqueue_attrs *attrs)
{
	int ret;

	mutex_lock(&wq_pool_mutex);
	ret = apply_workqueue_attrs_locked(wq, attrs);
	mutex_unlock(&wq_pool_mutex);

	return ret;
}

/**
 * unbound_wq_update_pwq - update a pwq slot for CPU hot[un]plug
 * @wq: the target workqueue
 * @cpu: the CPU to update the pwq slot for
 *
 * This function is to be called from %CPU_DOWN_PREPARE, %CPU_ONLINE and
 * %CPU_DOWN_FAILED.  @cpu is in the same pod of the CPU being hot[un]plugged.
 *
 *
 * If pod affinity can't be adjusted due to memory allocation failure, it falls
 * back to @wq->dfl_pwq which may not be optimal but is always correct.
 *
 * Note that when the last allowed CPU of a pod goes offline for a workqueue
 * with a cpumask spanning multiple pods, the workers which were already
 * executing the work items for the workqueue will lose their CPU affinity and
 * may execute on any CPU. This is similar to how per-cpu workqueues behave on
 * CPU_DOWN. If a workqueue user wants strict affinity, it's the user's
 * responsibility to flush the work item from CPU_DOWN_PREPARE.
 */
/*
 * unbound_wq_update_pwq() - CPU 热插拔/拓扑变化时重算一个 CPU 槽的 unbound pwq。
 * @wq 借用、@cpu 目标；hotplug 排他与相关 mutex 稳定全局缓冲。返回无，分配失败
 * 时保留可用旧映射。
 */
static void unbound_wq_update_pwq(struct workqueue_struct *wq, int cpu)
{
	struct pool_workqueue *old_pwq = NULL, *pwq;
	struct workqueue_attrs *target_attrs;

	lockdep_assert_held(&wq_pool_mutex);

	if (!(wq->flags & WQ_UNBOUND) || wq->unbound_attrs->ordered)
		return;

	/*
	 * We don't wanna alloc/free wq_attrs for each wq for each CPU.
	 * Let's use a preallocated one.  The following buf is protected by
	 * CPU hotplug exclusion.
	 */
	target_attrs = unbound_wq_update_pwq_attrs_buf;

	copy_workqueue_attrs(target_attrs, wq->unbound_attrs);
	wqattrs_actualize_cpumask(target_attrs, wq_unbound_cpumask);

	/* nothing to do if the target cpumask matches the current pwq */
	wq_calc_pod_cpumask(target_attrs, cpu);
	if (wqattrs_equal(target_attrs, unbound_pwq(wq, cpu)->pool->attrs))
		return;

	/* create a new pwq */
	pwq = alloc_unbound_pwq(wq, target_attrs);
	if (!pwq) {
		pr_warn("workqueue: allocation failed while updating CPU pod affinity of \"%s\"\n",
			wq->name);
		goto use_dfl_pwq;
	}

	/* Install the new pwq. */
	mutex_lock(&wq->mutex);
	old_pwq = install_unbound_pwq(wq, cpu, pwq);
	goto out_unlock;

use_dfl_pwq:
	mutex_lock(&wq->mutex);
	pwq = unbound_pwq(wq, -1);
	raw_spin_lock_irq(&pwq->pool->lock);
	get_pwq(pwq);
	raw_spin_unlock_irq(&pwq->pool->lock);
	old_pwq = install_unbound_pwq(wq, cpu, pwq);
out_unlock:
	mutex_unlock(&wq->mutex);
	put_pwq_unlocked(old_pwq);
}

/*
 * alloc_and_link_pwqs() - 为新 wq 建立全部 bound 或 unbound pwq 映射并发布链表。
 * @wq 尚未对外可见；可睡眠。成功 0，失败负 errno 并回滚已分配连接。
 */
static int alloc_and_link_pwqs(struct workqueue_struct *wq)
{
	bool highpri = wq->flags & WQ_HIGHPRI;
	int cpu, ret;

	lockdep_assert_held(&wq_pool_mutex);

	wq->cpu_pwq = alloc_percpu(struct pool_workqueue *);
	if (!wq->cpu_pwq)
		goto enomem;

	if (!(wq->flags & WQ_UNBOUND)) {
		struct worker_pool __percpu *pools;

		if (wq->flags & WQ_BH)
			pools = bh_worker_pools;
		else
			pools = cpu_worker_pools;

		for_each_possible_cpu(cpu) {
			struct pool_workqueue **pwq_p;
			struct worker_pool *pool;

			pool = &(per_cpu_ptr(pools, cpu)[highpri]);
			pwq_p = per_cpu_ptr(wq->cpu_pwq, cpu);

			*pwq_p = kmem_cache_alloc_node(pwq_cache, GFP_KERNEL,
						       pool->node);
			if (!*pwq_p)
				goto enomem;

			init_pwq(*pwq_p, wq, pool);

			mutex_lock(&wq->mutex);
			link_pwq(*pwq_p);
			mutex_unlock(&wq->mutex);
		}
		return 0;
	}

	if (wq->flags & __WQ_ORDERED) {
		struct pool_workqueue *dfl_pwq;

		ret = apply_workqueue_attrs_locked(wq, ordered_wq_attrs[highpri]);
		/* there should only be single pwq for ordering guarantee */
		dfl_pwq = rcu_access_pointer(wq->dfl_pwq);
		WARN(!ret && (wq->pwqs.next != &dfl_pwq->pwqs_node ||
			      wq->pwqs.prev != &dfl_pwq->pwqs_node),
		     "ordering guarantee broken for workqueue %s\n", wq->name);
	} else {
		ret = apply_workqueue_attrs_locked(wq, unbound_std_wq_attrs[highpri]);
	}

	if (ret)
		goto enomem;
	return 0;

enomem:
	if (wq->cpu_pwq) {
		for_each_possible_cpu(cpu) {
			struct pool_workqueue *pwq = *per_cpu_ptr(wq->cpu_pwq, cpu);

			if (pwq) {
				/*
				 * Unlink pwq from wq->pwqs since link_pwq()
				 * may have already added it. wq->mutex is not
				 * needed as the wq has not been published yet.
				 */
				if (!list_empty(&pwq->pwqs_node))
					list_del_rcu(&pwq->pwqs_node);
				kmem_cache_free(pwq_cache, pwq);
			}
		}
		free_percpu(wq->cpu_pwq);
		wq->cpu_pwq = NULL;
	}
	return -ENOMEM;
}

/* wq_clamp_max_active() 按 BH/ordered/普通边界规范化请求并发值，返回合法上限。 */
static int wq_clamp_max_active(int max_active, unsigned int flags,
			       const char *name)
{
	if (max_active < 1 || max_active > WQ_MAX_ACTIVE)
		pr_warn("workqueue: max_active %d requested for %s is out of range, clamping between %d and %d\n",
			max_active, name, 1, WQ_MAX_ACTIVE);

	return clamp_val(max_active, 1, WQ_MAX_ACTIVE);
}

/*
 * Workqueues which may be used during memory reclaim should have a rescuer
 * to guarantee forward progress.
 */
/*
 * init_rescuer() - 为 WQ_MEM_RECLAIM wq 创建尚未启动的专用救援线程。
 * @wq 构造中对象；可睡眠。成功 0 并让 wq 拥有 rescuer，失败负 errno 并清理。
 */
static int init_rescuer(struct workqueue_struct *wq)
{
	struct worker *rescuer;
	char id_buf[WORKER_ID_LEN];
	int ret;

	lockdep_assert_held(&wq_pool_mutex);

	if (!(wq->flags & WQ_MEM_RECLAIM))
		return 0;

	rescuer = alloc_worker(NUMA_NO_NODE);
	if (!rescuer) {
		pr_err("workqueue: Failed to allocate a rescuer for wq \"%s\"\n",
		       wq->name);
		return -ENOMEM;
	}

	rescuer->rescue_wq = wq;
	format_worker_id(id_buf, sizeof(id_buf), rescuer, NULL);

	rescuer->task = kthread_create(rescuer_thread, rescuer, "%s", id_buf);
	if (IS_ERR(rescuer->task)) {
		ret = PTR_ERR(rescuer->task);
		pr_err("workqueue: Failed to create a rescuer kthread for wq \"%s\": %pe",
		       wq->name, ERR_PTR(ret));
		kfree(rescuer);
		return ret;
	}

	wq->rescuer = rescuer;

	/* initial cpumask is consistent with the detached rescuer and unbind_worker() */
	if (cpumask_intersects(wq_unbound_cpumask, cpu_active_mask))
		kthread_bind_mask(rescuer->task, wq_unbound_cpumask);
	else
		kthread_bind_mask(rescuer->task, cpu_possible_mask);

	wake_up_process(rescuer->task);

	return 0;
}

/**
 * wq_adjust_max_active - update a wq's max_active to the current setting
 * @wq: target workqueue
 *
 * If @wq isn't freezing, set @wq->max_active to the saved_max_active and
 * activate inactive work items accordingly. If @wq is freezing, clear
 * @wq->max_active to zero.
 */
/*
 * wq_adjust_max_active() - 把 wq 当前 min/max/freeze 策略落实到所有 pwq 并补激活。
 * 入口持 wq mutex；逐 pool 锁更新，返回无。上限增大时用 fill 路径主动从 inactive
 * 补到新并发度；冻结时 freezable wq 可降到零。
 */
static void wq_adjust_max_active(struct workqueue_struct *wq)
{
	bool activated;
	int new_max, new_min;

	lockdep_assert_held(&wq->mutex);

	if ((wq->flags & WQ_FREEZABLE) && workqueue_freezing) {
		new_max = 0;
		new_min = 0;
	} else {
		new_max = wq->saved_max_active;
		new_min = wq->saved_min_active;
	}

	if (wq->max_active == new_max && wq->min_active == new_min)
		return;

	/*
	 * Update @wq->max/min_active and then kick inactive work items if more
	 * active work items are allowed. This doesn't break work item ordering
	 * because new work items are always queued behind existing inactive
	 * work items if there are any.
	 */
	WRITE_ONCE(wq->max_active, new_max);
	WRITE_ONCE(wq->min_active, new_min);

	if (wq->flags & WQ_UNBOUND)
		wq_update_node_max_active(wq, -1);

	if (new_max == 0)
		return;

	/*
	 * Round-robin through pwq's activating the first inactive work item
	 * until max_active is filled.
	 */
	do {
		struct pool_workqueue *pwq;

		activated = false;
		for_each_pwq(pwq, wq) {
			unsigned long irq_flags;

			/* can be called during early boot w/ irq disabled */
			raw_spin_lock_irqsave(&pwq->pool->lock, irq_flags);
			if (pwq_activate_first_inactive(pwq, true)) {
				activated = true;
				kick_pool(pwq->pool);
			}
			raw_spin_unlock_irqrestore(&pwq->pool->lock, irq_flags);
		}
	} while (activated);
}

__printf(1, 0)
/*
 * __alloc_workqueue() - workqueue 构造总入口。
 * @fmt/@args 生成持久名称；@flags 定义 bound/unbound、优先级、reclaim、freezable、
 * ordered/BH 等策略；@max_active/@min_active 规范化并发范围。函数可睡眠。
 * 成功返回调用者拥有的 wq；失败 NULL 并逆序撤销 lockdep、rescuer、pwq/node/对象。
 * 只有所有内部对象完整后才加入全局 workqueues 并启动 rescuer，形成发布边界。
 */
static struct workqueue_struct *__alloc_workqueue(const char *fmt,
						  unsigned int flags,
						  int max_active, va_list args)
{
	struct workqueue_struct *wq;
	size_t wq_size;
	int name_len;

	if (flags & WQ_BH) {
		if (WARN_ON_ONCE(flags & ~__WQ_BH_ALLOWS))
			return NULL;
		if (WARN_ON_ONCE(max_active))
			return NULL;
	}

	/* see the comment above the definition of WQ_POWER_EFFICIENT */
	if ((flags & WQ_POWER_EFFICIENT) && wq_power_efficient)
		flags = (flags & ~WQ_PERCPU) | WQ_UNBOUND;

	/* allocate wq and format name */
	if (flags & WQ_UNBOUND)
		wq_size = struct_size(wq, node_nr_active, nr_node_ids + 1);
	else
		wq_size = sizeof(*wq);

	wq = kzalloc_noprof(wq_size, GFP_KERNEL);
	if (!wq)
		return NULL;

	if (flags & WQ_UNBOUND) {
		wq->unbound_attrs = alloc_workqueue_attrs_noprof();
		if (!wq->unbound_attrs)
			goto err_free_wq;
	}

	name_len = vsnprintf(wq->name, sizeof(wq->name), fmt, args);

	if (name_len >= WQ_NAME_LEN)
		pr_warn_once("workqueue: name exceeds WQ_NAME_LEN. Truncating to: %s\n",
			     wq->name);

	/*
	 * One among WQ_PERCPU and WQ_UNBOUND must be set, but not both.
	 * - If neither is set, default to WQ_PERCPU
	 * - If both are set, default to WQ_UNBOUND
	 *
	 * This code can be removed after workqueue are unbound by default
	 */
	if (unlikely(!(flags & (WQ_UNBOUND | WQ_PERCPU)))) {
		WARN_ONCE(1, "workqueue: %s is using neither WQ_PERCPU or WQ_UNBOUND. "
			  "Setting WQ_PERCPU.\n", wq->name);
		flags |= WQ_PERCPU;
	} else if (unlikely((flags & WQ_PERCPU) && (flags & WQ_UNBOUND))) {
		WARN_ONCE(1, "workqueue: %s uses both WQ_PERCPU and WQ_UNBOUND. "
			  "Dropped WQ_PERCPU, keeping WQ_UNBOUND.\n", wq->name);
		flags &= ~WQ_PERCPU;
	}

	if (flags & WQ_BH) {
		/*
		 * BH workqueues always share a single execution context per CPU
		 * and don't impose any max_active limit.
		 */
		max_active = INT_MAX;
	} else {
		max_active = max_active ?: WQ_DFL_ACTIVE;
		max_active = wq_clamp_max_active(max_active, flags, wq->name);
	}

	/* init wq */
	wq->flags = flags;
	wq->max_active = max_active;
	wq->min_active = min(max_active, WQ_DFL_MIN_ACTIVE);
	wq->saved_max_active = wq->max_active;
	wq->saved_min_active = wq->min_active;
	mutex_init(&wq->mutex);
	atomic_set(&wq->nr_pwqs_to_flush, 0);
	INIT_LIST_HEAD(&wq->pwqs);
	INIT_LIST_HEAD(&wq->flusher_queue);
	INIT_LIST_HEAD(&wq->flusher_overflow);
	INIT_LIST_HEAD(&wq->maydays);

	INIT_LIST_HEAD(&wq->list);

	if (flags & WQ_UNBOUND) {
		if (alloc_node_nr_active(wq->node_nr_active) < 0)
			goto err_free_wq;
	}

	/*
	 * wq_pool_mutex protects the workqueues list, allocations of PWQs,
	 * and the global freeze state.
	 */
	mutex_lock(&wq_pool_mutex);

	if (alloc_and_link_pwqs(wq) < 0)
		goto err_unlock_free_node_nr_active;

	mutex_lock(&wq->mutex);
	wq_adjust_max_active(wq);
	mutex_unlock(&wq->mutex);

	list_add_tail_rcu(&wq->list, &workqueues);

	if (wq_online && init_rescuer(wq) < 0)
		goto err_unlock_destroy;

	mutex_unlock(&wq_pool_mutex);

	if ((wq->flags & WQ_SYSFS) && workqueue_sysfs_register(wq))
		goto err_destroy;

	return wq;

err_unlock_free_node_nr_active:
	mutex_unlock(&wq_pool_mutex);
	/*
	 * Failed alloc_and_link_pwqs() may leave pending pwq->release_work,
	 * flushing the pwq_release_worker ensures that the pwq_release_workfn()
	 * completes before calling kfree(wq).
	 */
	if (wq->flags & WQ_UNBOUND) {
		kthread_flush_worker(pwq_release_worker);
		free_node_nr_active(wq->node_nr_active);
	}
err_free_wq:
	free_workqueue_attrs(wq->unbound_attrs);
	kfree(wq);
	return NULL;
err_unlock_destroy:
	mutex_unlock(&wq_pool_mutex);
err_destroy:
	destroy_workqueue(wq);
	return NULL;
}

__printf(1, 0)
/* alloc_workqueue_va() 是 va_list 包装，参数 ownership/失败语义完全透传 __alloc_workqueue。 */
static struct workqueue_struct *alloc_workqueue_va(const char *fmt,
						   unsigned int flags,
						   int max_active,
						   va_list args)
{
	struct workqueue_struct *wq;

	wq = __alloc_workqueue(fmt, flags, max_active, args);
	if (wq)
		wq_init_lockdep(wq);

	return wq;
}

__printf(1, 4)
struct workqueue_struct *alloc_workqueue_noprof(const char *fmt,
						unsigned int flags,
						int max_active, ...)
{
	struct workqueue_struct *wq;
	va_list args;

	va_start(args, max_active);
	wq = alloc_workqueue_va(fmt, flags, max_active, args);
	va_end(args);

	return wq;
}
EXPORT_SYMBOL_GPL(alloc_workqueue_noprof);

/* devm_workqueue_release() 在 device 资源回收时 destroy 先前登记的 wq。 */
static void devm_workqueue_release(void *res)
{
	destroy_workqueue(res);
}

__printf(2, 5) struct workqueue_struct *
devm_alloc_workqueue_noprof(struct device *dev, const char *fmt,
			    unsigned int flags, int max_active, ...)
{
	struct workqueue_struct *wq;
	va_list args;
	int ret;

	va_start(args, max_active);
	wq = alloc_workqueue_va(fmt, flags, max_active, args);
	va_end(args);
	if (!wq)
		return NULL;

	ret = devm_add_action_or_reset(dev, devm_workqueue_release, wq);
	if (ret)
		return NULL;

	return wq;
}
EXPORT_SYMBOL_GPL(devm_alloc_workqueue_noprof);

#ifdef CONFIG_LOCKDEP
__printf(1, 5)
struct workqueue_struct *
alloc_workqueue_lockdep_map(const char *fmt, unsigned int flags,
			    int max_active, struct lockdep_map *lockdep_map, ...)
{
	struct workqueue_struct *wq;
	va_list args;

	va_start(args, lockdep_map);
	wq = __alloc_workqueue(fmt, flags, max_active, args);
	va_end(args);
	if (!wq)
		return NULL;

	wq->lockdep_map = lockdep_map;

	return wq;
}
EXPORT_SYMBOL_GPL(alloc_workqueue_lockdep_map);
#endif

/*
 * pwq_busy() - 判断销毁路径是否仍有 active/inactive、mayday 或引用中的 work。
 * 调用者持相应锁；返回诊断布尔值，不改变状态。
 */
static bool pwq_busy(struct pool_workqueue *pwq)
{
	int i;

	for (i = 0; i < WORK_NR_COLORS; i++)
		if (pwq->nr_in_flight[i])
			return true;

	if ((pwq != rcu_access_pointer(pwq->wq->dfl_pwq)) && (pwq->refcnt > 1))
		return true;
	if (!pwq_is_empty(pwq))
		return true;

	return false;
}

/**
 * destroy_workqueue - safely terminate a workqueue
 * @wq: target workqueue
 *
 * Safely destroy a workqueue. All work currently pending will be done first.
 *
 * This function does NOT guarantee that non-pending work that has been
 * submitted with queue_delayed_work() and similar functions will be done
 * before destroying the workqueue. The fundamental problem is that, currently,
 * the workqueue has no way of accessing non-pending delayed_work. delayed_work
 * is only linked on the timer-side. All delayed_work must, therefore, be
 * canceled before calling this function.
 *
 * TODO: It would be better if the problem described above wouldn't exist and
 * destroy_workqueue() would cleanly cancel all pending and non-pending
 * delayed_work.
 */
/*
 * 上述限制必须由调用者处理：非零延时 delayed_work 在 timer 到期前只挂 timer 侧，
 * wq 没有反向目录可找到它，因此 destroy 无法等待或取消这种“尚未进入 wq pending
 * 链”的对象。调用者必须先 cancel_delayed_work_sync() 所有相关 dwork。理想方案是
 * 让 wq 能追踪 timer 阶段，但当前实现尚未具备，不能把 drain 成功误当成 timer 已空。
 */
/*
 * destroy_workqueue() - 排空、停止发布并最终释放一个调用者拥有的 wq。
 *
 * @wq 不可再有外部生产者；函数可睡眠、返回无。先置 DESTROYING 捕获误入队并
 * drain 闭包，再从 sysfs/全局 RCU 列表摘除；停止 rescuer，断开并 put 所有 pwq，
 * 销毁 lockdep，最后 call_rcu 释放 wq。返回后调用者不得再解引用；RCU 只保护已
 * 在读侧的内部观察者，不授权新 API 调用。
 */
void destroy_workqueue(struct workqueue_struct *wq)
{
	struct pool_workqueue *pwq;
	int cpu;

	/*
	 * Remove it from sysfs first so that sanity check failure doesn't
	 * lead to sysfs name conflicts.
	 */
	/* 阶段 1：先撤用户态入口，即使后面 sanity 失败保留对象，也不会发生同名重注册冲突。 */
	workqueue_sysfs_unregister(wq);

	/* mark the workqueue destruction is in progress */
	/* DESTROYING 让后续外部 queue 告警并交还 PENDING，阻止新的生命周期进入。 */
	mutex_lock(&wq->mutex);
	wq->flags |= __WQ_DESTROYING;
	mutex_unlock(&wq->mutex);

	/* drain it before proceeding with destruction */
	/* 阶段 2：反复 flush 已发布 work 及其允许的 chained 后继，建立逻辑空队列。 */
	drain_workqueue(wq);

	/* kill rescuer, if sanity checks fail, leave it w/o rescuer */
	/* 阶段 3：停止 rescuer；它退出前自行清 maydays，随后 wq 不再具备救援执行者。 */
	if (wq->rescuer) {
		/* rescuer will empty maydays list before exiting */
		kthread_stop(wq->rescuer->task);
		kfree(wq->rescuer);
		wq->rescuer = NULL;
	}

	/*
	 * Sanity checks - grab all the locks so that we wait for all
	 * in-flight operations which may do put_pwq().
	 */
	/*
	 * 阶段 4：按全局→wq→pool 锁序等待所有在途 put 并核对每个 pwq 真空。若失败，
	 * 为避免 UAF 立即停止销毁并保留 wq，虽然 sysfs/rescuer 已撤，日志给出状态。
	 */
	mutex_lock(&wq_pool_mutex);
	mutex_lock(&wq->mutex);
	for_each_pwq(pwq, wq) {
		raw_spin_lock_irq(&pwq->pool->lock);
		if (WARN_ON(pwq_busy(pwq))) {
			pr_warn("%s: %s has the following busy pwq\n",
				__func__, wq->name);
			show_pwq(pwq);
			raw_spin_unlock_irq(&pwq->pool->lock);
			mutex_unlock(&wq->mutex);
			mutex_unlock(&wq_pool_mutex);
			show_one_workqueue(wq);
			return;
		}
		raw_spin_unlock_irq(&pwq->pool->lock);
	}
	mutex_unlock(&wq->mutex);

	/*
	 * wq list is used to freeze wq, remove from list after
	 * flushing is complete in case freeze races us.
	 */
	/* drain 完成后才从 freeze 全局目录 RCU 摘除，避免 freezer 漏等仍在途回调。 */
	list_del_rcu(&wq->list);
	mutex_unlock(&wq_pool_mutex);

	/*
	 * We're the sole accessor of @wq. Directly access cpu_pwq and dfl_pwq
	 * to put the base refs. @wq will be auto-destroyed from the last
	 * pwq_put. RCU read lock prevents @wq from going away from under us.
	 */
	/*
	 * 阶段 5：此时只有销毁者访问槽。逐 CPU 和默认槽归还基础 pwq 引用并置 NULL；
	 * 最后 pwq 的异步 release 会完成 wq 最终回收。RCU 防止循环中底层对象先释放。
	 */
	rcu_read_lock();

	for_each_possible_cpu(cpu) {
		put_pwq_unlocked(unbound_pwq(wq, cpu));
		RCU_INIT_POINTER(*unbound_pwq_slot(wq, cpu), NULL);
	}

	put_pwq_unlocked(unbound_pwq(wq, -1));
	RCU_INIT_POINTER(*unbound_pwq_slot(wq, -1), NULL);

	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(destroy_workqueue);

/**
 * workqueue_set_max_active - adjust max_active of a workqueue
 * @wq: target workqueue
 * @max_active: new max_active value.
 *
 * Set max_active of @wq to @max_active. See the alloc_workqueue() function
 * comment.
 *
 * CONTEXT:
 * Don't call from IRQ context.
 */
/*
 * workqueue_set_max_active() - 动态修改 wq 并发上限并激活新获得的额度。
 * @wq 存活，@max_active 经策略 clamp；可睡眠、返回无。WRITE_ONCE 发布配置，
 * mutex 下重算 node/pwq 账户；不会取消已超出新上限的 running work，只限制后续。
 */
void workqueue_set_max_active(struct workqueue_struct *wq, int max_active)
{
	/* max_active doesn't mean anything for BH workqueues */
	if (WARN_ON(wq->flags & WQ_BH))
		return;
	/* disallow meddling with max_active for ordered workqueues */
	if (WARN_ON(wq->flags & __WQ_ORDERED))
		return;

	max_active = wq_clamp_max_active(max_active, wq->flags, wq->name);

	mutex_lock(&wq->mutex);

	wq->saved_max_active = max_active;
	if (wq->flags & WQ_UNBOUND)
		wq->saved_min_active = min(wq->saved_min_active, max_active);

	wq_adjust_max_active(wq);

	mutex_unlock(&wq->mutex);
}
EXPORT_SYMBOL_GPL(workqueue_set_max_active);

/**
 * workqueue_set_min_active - adjust min_active of an unbound workqueue
 * @wq: target unbound workqueue
 * @min_active: new min_active value
 *
 * Set min_active of an unbound workqueue. Unlike other types of workqueues, an
 * unbound workqueue is not guaranteed to be able to process max_active
 * interdependent work items. Instead, an unbound workqueue is guaranteed to be
 * able to process min_active number of interdependent work items which is
 * %WQ_DFL_MIN_ACTIVE by default.
 *
 * Use this function to adjust the min_active value between 0 and the current
 * max_active.
 */
/*
 * workqueue_set_min_active() - 设置 unbound node 分配的保底并发值。
 * 规范化后不超过 max；可睡眠、返回无。它保证小 node/热插拔退化仍有前进额度，
 * 不强制立即创建同数量线程。
 */
void workqueue_set_min_active(struct workqueue_struct *wq, int min_active)
{
	/* min_active is only meaningful for non-ordered unbound workqueues */
	if (WARN_ON((wq->flags & (WQ_BH | WQ_UNBOUND | __WQ_ORDERED)) !=
		    WQ_UNBOUND))
		return;

	mutex_lock(&wq->mutex);
	wq->saved_min_active = clamp(min_active, 0, wq->saved_max_active);
	wq_adjust_max_active(wq);
	mutex_unlock(&wq->mutex);
}

/**
 * current_work - retrieve %current task's work struct
 *
 * Determine if %current task is a workqueue worker and what it's working on.
 * Useful to find out the context that the %current task is running in.
 *
 * Return: work struct if %current task is a workqueue worker, %NULL otherwise.
 */
/*
 * current_work() - 若 current 是正在执行回调的 kworker，返回其 current_work。
 * 返回借用指针或 NULL，不增引用；只在当前回调期间稳定，回调结束后 work 甚至可
 * 已释放。无睡眠无副作用。
 */
struct work_struct *current_work(void)
{
	struct worker *worker = current_wq_worker();

	return worker ? worker->current_work : NULL;
}
EXPORT_SYMBOL(current_work);

/**
 * current_is_workqueue_rescuer - is %current workqueue rescuer?
 *
 * Determine whether %current is a workqueue rescuer.  Can be used from
 * work functions to determine whether it's being run off the rescuer task.
 *
 * Return: %true if %current is a workqueue rescuer. %false otherwise.
 */
/* current_is_workqueue_rescuer() 判断当前 task 是否处于 rescuer 回调，纯查询。 */
bool current_is_workqueue_rescuer(void)
{
	struct worker *worker = current_wq_worker();

	return worker && worker->rescue_wq;
}

/**
 * workqueue_congested - test whether a workqueue is congested
 * @cpu: CPU in question
 * @wq: target workqueue
 *
 * Test whether @wq's cpu workqueue for @cpu is congested.  There is
 * no synchronization around this function and the test result is
 * unreliable and only useful as advisory hints or for debugging.
 *
 * If @cpu is WORK_CPU_UNBOUND, the test is performed on the local CPU.
 *
 * With the exception of ordered workqueues, all workqueues have per-cpu
 * pool_workqueues, each with its own congested state. A workqueue being
 * congested on one CPU doesn't mean that the workqueue is contested on any
 * other CPUs.
 *
 * Return:
 * %true if congested, %false otherwise.
 */
/*
 * workqueue_congested() - 近似判断指定 CPU/pwq 是否已有 inactive work。
 * 返回瞬时提示而非同步保证，适合启发式退避；@cpu 可 UNBOUND，@wq 借用。
 */
bool workqueue_congested(int cpu, struct workqueue_struct *wq)
{
	struct pool_workqueue *pwq;
	bool ret;

	preempt_disable();

	if (cpu == WORK_CPU_UNBOUND)
		cpu = smp_processor_id();

	pwq = *per_cpu_ptr(wq->cpu_pwq, cpu);
	ret = !list_empty(&pwq->inactive_works);

	preempt_enable();

	return ret;
}
EXPORT_SYMBOL_GPL(workqueue_congested);

/**
 * work_busy - test whether a work is currently pending or running
 * @work: the work to be tested
 *
 * Test whether @work is currently pending or running.  There is no
 * synchronization around this function and the test result is
 * unreliable and only useful as advisory hints or for debugging.
 *
 * Return:
 * OR'd bitmask of WORK_BUSY_* bits.
 */
/*
 * work_busy() - 返回 work 当前 PENDING/RUNNING 位的瞬时诊断快照。
 * @work 借用；结果可能返回后立即变化，不能据此释放对象或替代 cancel/flush。
 */
unsigned int work_busy(struct work_struct *work)
{
	struct worker_pool *pool;
	unsigned long irq_flags;
	unsigned int ret = 0;

	if (work_pending(work))
		ret |= WORK_BUSY_PENDING;

	rcu_read_lock();
	pool = get_work_pool(work);
	if (pool) {
		raw_spin_lock_irqsave(&pool->lock, irq_flags);
		if (find_worker_executing_work(pool, work))
			ret |= WORK_BUSY_RUNNING;
		raw_spin_unlock_irqrestore(&pool->lock, irq_flags);
	}
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(work_busy);

/**
 * set_worker_desc - set description for the current work item
 * @fmt: printf-style format string
 * @...: arguments for the format string
 *
 * This function can be called by a running work function to describe what
 * the work item is about.  If the worker task gets dumped, this
 * information will be printed out together to help debugging.  The
 * description can be at most WORKER_DESC_LEN including the trailing '\0'.
 */
/*
 * set_worker_desc() - 当前 kworker 回调为阻塞/诊断路径设置临时描述。
 * printf 参数只在调用内消费；返回无。描述不改变 task comm 与调度，只供卡死输出。
 */
void set_worker_desc(const char *fmt, ...)
{
	struct worker *worker = current_wq_worker();
	va_list args;

	if (worker) {
		va_start(args, fmt);
		vsnprintf(worker->desc, sizeof(worker->desc), fmt, args);
		va_end(args);
	}
}
EXPORT_SYMBOL_GPL(set_worker_desc);

/**
 * print_worker_info - print out worker information and description
 * @log_lvl: the log level to use when printing
 * @task: target task
 *
 * If @task is a worker and currently executing a work item, print out the
 * name of the workqueue being serviced and worker description set with
 * set_worker_desc() by the currently executing work item.
 *
 * This function can be safely called on any task as long as the
 * task_struct itself is accessible.  While safe, this function isn't
 * synchronized and may print out mixups or garbages of limited length.
 */
/* print_worker_info() 打印 kworker 当前/最近 work 与 pool 身份；只做 best-effort 诊断。 */
void print_worker_info(const char *log_lvl, struct task_struct *task)
{
	work_func_t fn = NULL;
	char name[WQ_NAME_LEN] = { };
	char desc[WORKER_DESC_LEN] = { };
	struct pool_workqueue *pwq = NULL;
	struct workqueue_struct *wq = NULL;
	struct worker *worker;

	if (!(task->flags & PF_WQ_WORKER))
		return;

	/*
	 * This function is called without any synchronization and @task
	 * could be in any state.  Be careful with dereferences.
	 */
	worker = kthread_probe_data(task);

	/*
	 * Carefully copy the associated workqueue's workfn, name and desc.
	 * Keep the original last '\0' in case the original is garbage.
	 */
	copy_from_kernel_nofault(&fn, &worker->current_func, sizeof(fn));
	copy_from_kernel_nofault(&pwq, &worker->current_pwq, sizeof(pwq));
	copy_from_kernel_nofault(&wq, &pwq->wq, sizeof(wq));
	copy_from_kernel_nofault(name, wq->name, sizeof(name) - 1);
	copy_from_kernel_nofault(desc, worker->desc, sizeof(desc) - 1);

	if (fn || name[0] || desc[0]) {
		printk("%sWorkqueue: %s %ps", log_lvl, name, fn);
		if (strcmp(name, desc))
			pr_cont(" (%s)", desc);
		pr_cont("\n");
	}
}

/* pr_cont_pool_info() 以续行格式输出 pool id/CPU/node/flags，供组合诊断消息复用。 */
static void pr_cont_pool_info(struct worker_pool *pool)
{
	pr_cont(" cpus=%*pbl", nr_cpumask_bits, pool->attrs->cpumask);
	if (pool->node != NUMA_NO_NODE)
		pr_cont(" node=%d", pool->node);
	pr_cont(" flags=0x%x", pool->flags);
	if (pool->flags & POOL_BH)
		pr_cont(" bh%s",
			pool->attrs->nice == HIGHPRI_NICE_LEVEL ? "-hi" : "");
	else
		pr_cont(" nice=%d", pool->attrs->nice);
}

/* pr_cont_worker_id() 续行输出 worker task/desc 身份；worker 仅在当前保护窗口借用。 */
static void pr_cont_worker_id(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (pool->flags & POOL_BH)
		pr_cont("bh%s",
			pool->attrs->nice == HIGHPRI_NICE_LEVEL ? "-hi" : "");
	else
		pr_cont("%d%s", task_pid_nr(worker->task),
			worker->rescue_wq ? "(RESCUER)" : "");
}

struct pr_cont_work_struct {
	bool comma;
	work_func_t func;
	long ctr;
};

/* pr_cont_work_flush() 去重并输出 func 符号，@comma 控制列表标点；只改打印游标。 */
static void pr_cont_work_flush(bool comma, work_func_t func, struct pr_cont_work_struct *pcwsp)
{
	if (!pcwsp->ctr)
		goto out_record;
	if (func == pcwsp->func) {
		pcwsp->ctr++;
		return;
	}
	if (pcwsp->ctr == 1)
		pr_cont("%s %ps", pcwsp->comma ? "," : "", pcwsp->func);
	else
		pr_cont("%s %ld*%ps", pcwsp->comma ? "," : "", pcwsp->ctr, pcwsp->func);
	pcwsp->ctr = 0;
out_record:
	if ((long)func == -1L)
		return;
	pcwsp->comma = comma;
	pcwsp->func = func;
	pcwsp->ctr = 1;
}

/* pr_cont_work() 从 work 提取 func 后复用去重输出；不得把 work 指针带出锁域。 */
static void pr_cont_work(bool comma, struct work_struct *work, struct pr_cont_work_struct *pcwsp)
{
	if (work->func == wq_barrier_func) {
		struct wq_barrier *barr;

		barr = container_of(work, struct wq_barrier, work);

		pr_cont_work_flush(comma, (work_func_t)-1, pcwsp);
		pr_cont("%s BAR(%d)", comma ? "," : "",
			task_pid_nr(barr->task));
	} else {
		if (!comma)
			pr_cont_work_flush(comma, (work_func_t)-1, pcwsp);
		pr_cont_work_flush(comma, work->func, pcwsp);
	}
}

/* show_pwq() 在调试输出中展示 pwq 的 pool、active/inactive、ref 与统计快照。 */
static void show_pwq(struct pool_workqueue *pwq)
{
	struct pr_cont_work_struct pcws = { .ctr = 0, };
	struct worker_pool *pool = pwq->pool;
	struct work_struct *work;
	struct worker *worker;
	bool has_in_flight = false, has_pending = false;
	int bkt;

	pr_info("  pwq %d:", pool->id);
	pr_cont_pool_info(pool);

	pr_cont(" active=%d refcnt=%d%s\n",
		pwq->nr_active, pwq->refcnt,
		!list_empty(&pwq->mayday_node) ? " MAYDAY" : "");

	hash_for_each(pool->busy_hash, bkt, worker, hentry) {
		if (worker->current_pwq == pwq) {
			has_in_flight = true;
			break;
		}
	}
	if (has_in_flight) {
		bool comma = false;

		pr_info("    in-flight:");
		hash_for_each(pool->busy_hash, bkt, worker, hentry) {
			if (worker->current_pwq != pwq)
				continue;

			pr_cont(" %s", comma ? "," : "");
			pr_cont_worker_id(worker);
			pr_cont(":%ps", worker->current_func);
			pr_cont(" for %us",
				jiffies_to_msecs(jiffies - worker->current_start) / 1000);
			list_for_each_entry(work, &worker->scheduled, entry)
				pr_cont_work(false, work, &pcws);
			pr_cont_work_flush(comma, (work_func_t)-1L, &pcws);
			comma = true;
		}
		pr_cont("\n");
	}

	list_for_each_entry(work, &pool->worklist, entry) {
		if (get_work_pwq(work) == pwq) {
			has_pending = true;
			break;
		}
	}
	if (has_pending) {
		bool comma = false;

		pr_info("    pending:");
		list_for_each_entry(work, &pool->worklist, entry) {
			if (get_work_pwq(work) != pwq)
				continue;

			pr_cont_work(comma, work, &pcws);
			comma = !(*work_data_bits(work) & WORK_STRUCT_LINKED);
		}
		pr_cont_work_flush(comma, (work_func_t)-1L, &pcws);
		pr_cont("\n");
	}

	if (!list_empty(&pwq->inactive_works)) {
		bool comma = false;

		pr_info("    inactive:");
		list_for_each_entry(work, &pwq->inactive_works, entry) {
			pr_cont_work(comma, work, &pcws);
			comma = !(*work_data_bits(work) & WORK_STRUCT_LINKED);
		}
		pr_cont_work_flush(comma, (work_func_t)-1L, &pcws);
		pr_cont("\n");
	}
}

/**
 * show_one_workqueue - dump state of specified workqueue
 * @wq: workqueue whose state will be printed
 */
/* show_one_workqueue() 在适当锁/RCU 下打印一个 wq 及全部 pwq；不改变运行状态。 */
void show_one_workqueue(struct workqueue_struct *wq)
{
	struct pool_workqueue *pwq;
	bool idle = true;
	unsigned long irq_flags;

	for_each_pwq(pwq, wq) {
		if (!pwq_is_empty(pwq)) {
			idle = false;
			break;
		}
	}
	if (idle) /* Nothing to print for idle workqueue */
		return;

	pr_info("workqueue %s: flags=0x%x\n", wq->name, wq->flags);

	for_each_pwq(pwq, wq) {
		raw_spin_lock_irqsave(&pwq->pool->lock, irq_flags);
		if (!pwq_is_empty(pwq)) {
			/*
			 * Defer printing to avoid deadlocks in console
			 * drivers that queue work while holding locks
			 * also taken in their write paths.
			 */
			printk_deferred_enter();
			show_pwq(pwq);
			printk_deferred_exit();
		}
		raw_spin_unlock_irqrestore(&pwq->pool->lock, irq_flags);
		/*
		 * We could be printing a lot from atomic context, e.g.
		 * sysrq-t -> show_all_workqueues(). Avoid triggering
		 * hard lockup.
		 */
		touch_nmi_watchdog();
	}

}

/**
 * show_one_worker_pool - dump state of specified worker pool
 * @pool: worker pool whose state will be printed
 */
/* show_one_worker_pool() 打印 pool worklist、worker/idle/manager 与 watchdog 信息。 */
static void show_one_worker_pool(struct worker_pool *pool)
{
	struct worker *worker;
	bool first = true;
	unsigned long irq_flags;
	unsigned long hung = 0;

	raw_spin_lock_irqsave(&pool->lock, irq_flags);
	if (pool->nr_workers == pool->nr_idle)
		goto next_pool;

	/* How long the first pending work is waiting for a worker. */
	if (!list_empty(&pool->worklist))
		hung = jiffies_to_msecs(jiffies - pool->last_progress_ts) / 1000;

	/*
	 * Defer printing to avoid deadlocks in console drivers that
	 * queue work while holding locks also taken in their write
	 * paths.
	 */
	printk_deferred_enter();
	pr_info("pool %d:", pool->id);
	pr_cont_pool_info(pool);
	pr_cont(" hung=%lus workers=%d", hung, pool->nr_workers);
	if (pool->manager)
		pr_cont(" manager: %d",
			task_pid_nr(pool->manager->task));
	list_for_each_entry(worker, &pool->idle_list, entry) {
		pr_cont(" %s", first ? "idle: " : "");
		pr_cont_worker_id(worker);
		first = false;
	}
	pr_cont("\n");
	printk_deferred_exit();
next_pool:
	raw_spin_unlock_irqrestore(&pool->lock, irq_flags);
	/*
	 * We could be printing a lot from atomic context, e.g.
	 * sysrq-t -> show_all_workqueues(). Avoid triggering
	 * hard lockup.
	 */
	touch_nmi_watchdog();

}

/**
 * show_all_workqueues - dump workqueue state
 *
 * Called from a sysrq handler and prints out all busy workqueues and pools.
 */
/* show_all_workqueues() 为 sysrq/诊断遍历 RCU 全局目录，只打印当前快照。 */
void show_all_workqueues(void)
{
	struct workqueue_struct *wq;
	struct worker_pool *pool;
	int pi;

	rcu_read_lock();

	pr_info("Showing busy workqueues and worker pools:\n");

	list_for_each_entry_rcu(wq, &workqueues, list)
		show_one_workqueue(wq);

	for_each_pool(pool, pi)
		show_one_worker_pool(pool);

	rcu_read_unlock();
}

/**
 * show_freezable_workqueues - dump freezable workqueue state
 *
 * Called from try_to_freeze_tasks() and prints out all freezable workqueues
 * still busy.
 */
/* show_freezable_workqueues() 只输出冻结过程中仍繁忙的 WQ_FREEZABLE 队列。 */
void show_freezable_workqueues(void)
{
	struct workqueue_struct *wq;

	rcu_read_lock();

	pr_info("Showing freezable workqueues that are still busy:\n");

	list_for_each_entry_rcu(wq, &workqueues, list) {
		if (!(wq->flags & WQ_FREEZABLE))
			continue;
		show_one_workqueue(wq);
	}

	rcu_read_unlock();
}

/* used to show worker information through /proc/PID/{comm,stat,status} */
/*
 * wq_worker_comm() - 根据 worker 当前 scheduled work 生成信息更丰富的 task comm。
 * @buf/@size 输出，@task 必须是稳定 kworker；返回无。只用于观测，不持有 work。
 */
void wq_worker_comm(char *buf, size_t size, struct task_struct *task)
{
	/* stabilize PF_WQ_WORKER and worker pool association */
	mutex_lock(&wq_pool_attach_mutex);

	if (task->flags & PF_WQ_WORKER) {
		struct worker *worker = kthread_data(task);
		struct worker_pool *pool = worker->pool;
		int off;

		off = format_worker_id(buf, size, worker, pool);

		if (pool) {
			raw_spin_lock_irq(&pool->lock);
			/*
			 * ->desc tracks information (wq name or
			 * set_worker_desc()) for the latest execution.  If
			 * current, prepend '+', otherwise '-'.
			 */
			if (worker->desc[0] != '\0') {
				if (worker->current_work)
					scnprintf(buf + off, size - off, "+%s",
						  worker->desc);
				else
					scnprintf(buf + off, size - off, "-%s",
						  worker->desc);
			}
			raw_spin_unlock_irq(&pool->lock);
		}
	} else {
		strscpy(buf, task->comm, size);
	}

	mutex_unlock(&wq_pool_attach_mutex);
}

#ifdef CONFIG_SMP

/*
 * CPU hotplug.
 *
 * There are two challenges in supporting CPU hotplug.  Firstly, there
 * are a lot of assumptions on strong associations among work, pwq and
 * pool which make migrating pending and scheduled works very
 * difficult to implement without impacting hot paths.  Secondly,
 * worker pools serve mix of short, long and very long running works making
 * blocked draining impractical.
 *
 * This is solved by allowing the pools to be disassociated from the CPU
 * running as an unbound one and allowing it to be reattached later if the
 * cpu comes back online.
 */

/*
 * unbind_workers() - CPU 离线准备时把其两个普通 pool 的 worker 全部解除硬绑定。
 * @cpu 由 hotplug 排他保护；持 attach/pool 锁设置 DISASSOCIATED/UNBOUND 并重置
 * nr_running。返回无，worker 仍可在其他 CPU 推进已有 work。
 */
static void unbind_workers(int cpu)
{
	struct worker_pool *pool;
	struct worker *worker;

	for_each_cpu_worker_pool(pool, cpu) {
		mutex_lock(&wq_pool_attach_mutex);
		raw_spin_lock_irq(&pool->lock);

		/*
		 * We've blocked all attach/detach operations. Make all workers
		 * unbound and set DISASSOCIATED.  Before this, all workers
		 * must be on the cpu.  After this, they may become diasporas.
		 * And the preemption disabled section in their sched callbacks
		 * are guaranteed to see WORKER_UNBOUND since the code here
		 * is on the same cpu.
		 */
		for_each_pool_worker(worker, pool)
			worker->flags |= WORKER_UNBOUND;

		pool->flags |= POOL_DISASSOCIATED;

		/*
		 * The handling of nr_running in sched callbacks are disabled
		 * now.  Zap nr_running.  After this, nr_running stays zero and
		 * need_more_worker() and keep_working() are always true as
		 * long as the worklist is not empty.  This pool now behaves as
		 * an unbound (in terms of concurrency management) pool which
		 * are served by workers tied to the pool.
		 */
		pool->nr_running = 0;

		/*
		 * With concurrency management just turned off, a busy
		 * worker blocking could lead to lengthy stalls.  Kick off
		 * unbound chain execution of currently pending work items.
		 */
		kick_pool(pool);

		raw_spin_unlock_irq(&pool->lock);

		for_each_pool_worker(worker, pool)
			unbind_worker(worker);

		mutex_unlock(&wq_pool_attach_mutex);
	}
}

/**
 * rebind_workers - rebind all workers of a pool to the associated CPU
 * @pool: pool of interest
 *
 * @pool->cpu is coming online.  Rebind all workers to the CPU.
 */
/*
 * rebind_workers() - CPU 上线后把 disassociated pool worker 重新绑定并恢复并发管理。
 * @pool 对应在线 CPU；返回无。先恢复 affinity，再清 UNBOUND/REBOUND，避免计数先
 * 生效而线程仍在错误 CPU 范围。
 */
static void rebind_workers(struct worker_pool *pool)
{
	struct worker *worker;

	lockdep_assert_held(&wq_pool_attach_mutex);

	/*
	 * Restore CPU affinity of all workers.  As all idle workers should
	 * be on the run-queue of the associated CPU before any local
	 * wake-ups for concurrency management happen, restore CPU affinity
	 * of all workers first and then clear UNBOUND.  As we're called
	 * from CPU_ONLINE, the following shouldn't fail.
	 */
	for_each_pool_worker(worker, pool) {
		kthread_set_per_cpu(worker->task, pool->cpu);
		WARN_ON_ONCE(set_cpus_allowed_ptr(worker->task,
						  pool_allowed_cpus(pool)) < 0);
	}

	raw_spin_lock_irq(&pool->lock);

	pool->flags &= ~POOL_DISASSOCIATED;

	for_each_pool_worker(worker, pool) {
		unsigned int worker_flags = worker->flags;

		/*
		 * We want to clear UNBOUND but can't directly call
		 * worker_clr_flags() or adjust nr_running.  Atomically
		 * replace UNBOUND with another NOT_RUNNING flag REBOUND.
		 * @worker will clear REBOUND using worker_clr_flags() when
		 * it initiates the next execution cycle thus restoring
		 * concurrency management.  Note that when or whether
		 * @worker clears REBOUND doesn't affect correctness.
		 *
		 * WRITE_ONCE() is necessary because @worker->flags may be
		 * tested without holding any lock in
		 * wq_worker_running().  Without it, NOT_RUNNING test may
		 * fail incorrectly leading to premature concurrency
		 * management operations.
		 */
		WARN_ON_ONCE(!(worker_flags & WORKER_UNBOUND));
		worker_flags |= WORKER_REBOUND;
		worker_flags &= ~WORKER_UNBOUND;
		WRITE_ONCE(worker->flags, worker_flags);
	}

	raw_spin_unlock_irq(&pool->lock);
}

/**
 * restore_unbound_workers_cpumask - restore cpumask of unbound workers
 * @pool: unbound pool of interest
 * @cpu: the CPU which is coming up
 *
 * An unbound pool may end up with a cpumask which doesn't have any online
 * CPUs.  When a worker of such pool get scheduled, the scheduler resets
 * its cpus_allowed.  If @cpu is in @pool's cpumask which didn't have any
 * online CPU before, cpus_allowed of all its workers should be restored.
 */
/* restore_unbound_workers_cpumask() 在热插拔后把 unbound worker affinity 恢复到 attrs mask。 */
static void restore_unbound_workers_cpumask(struct worker_pool *pool, int cpu)
{
	static cpumask_t cpumask;
	struct worker *worker;

	lockdep_assert_held(&wq_pool_attach_mutex);

	/* is @cpu allowed for @pool? */
	if (!cpumask_test_cpu(cpu, pool->attrs->cpumask))
		return;

	cpumask_and(&cpumask, pool->attrs->cpumask, cpu_online_mask);

	/* as we're called from CPU_ONLINE, the following shouldn't fail */
	for_each_pool_worker(worker, pool)
		WARN_ON_ONCE(set_cpus_allowed_ptr(worker->task, &cpumask) < 0);
}

/*
 * workqueue_prepare_cpu() - CPU 上线 prepare 阶段为其标准 pool 创建首 worker。
 * @cpu 尚未 online；可睡眠。成功 0，失败负 errno 并回滚，阻止 CPU 上线进入无执行者状态。
 */
int workqueue_prepare_cpu(unsigned int cpu)
{
	struct worker_pool *pool;

	for_each_cpu_worker_pool(pool, cpu) {
		if (pool->nr_workers)
			continue;
		if (!create_worker(pool))
			return -ENOMEM;
	}
	return 0;
}

/*
 * workqueue_online_cpu() - 发布 CPU 可供 workqueue 使用并重绑/更新 pwq。
 * hotplug 回调可睡眠；成功 0。更新 online mask、bound worker 与所有 unbound 映射，
 * 返回时新 queue 可选择该 CPU。
 */
int workqueue_online_cpu(unsigned int cpu)
{
	struct worker_pool *pool;
	struct workqueue_struct *wq;
	int pi;

	mutex_lock(&wq_pool_mutex);

	cpumask_set_cpu(cpu, wq_online_cpumask);

	for_each_pool(pool, pi) {
		/* BH pools aren't affected by hotplug */
		if (pool->flags & POOL_BH)
			continue;

		mutex_lock(&wq_pool_attach_mutex);
		if (pool->cpu == cpu)
			rebind_workers(pool);
		else if (pool->cpu < 0)
			restore_unbound_workers_cpumask(pool, cpu);
		mutex_unlock(&wq_pool_attach_mutex);
	}

	/* update pod affinity of unbound workqueues */
	list_for_each_entry(wq, &workqueues, list) {
		struct workqueue_attrs *attrs = wq->unbound_attrs;

		if (attrs) {
			const struct wq_pod_type *pt = wqattrs_pod_type(attrs);
			int tcpu;

			for_each_cpu(tcpu, pt->pod_cpus[pt->cpu_pod[cpu]])
				unbound_wq_update_pwq(wq, tcpu);

			mutex_lock(&wq->mutex);
			wq_update_node_max_active(wq, -1);
			mutex_unlock(&wq->mutex);
		}
	}

	mutex_unlock(&wq_pool_mutex);
	return 0;
}

/*
 * workqueue_offline_cpu() - 从选择 mask 移除 CPU、更新 unbound 映射并解除 bound worker。
 * hotplug 串行上下文可睡眠；返回 0。已有 work 不丢失，worker 转 unbound 后在其他
 * CPU 继续执行，BH 留给 dead 阶段专门排空。
 */
int workqueue_offline_cpu(unsigned int cpu)
{
	struct workqueue_struct *wq;

	/* unbinding per-cpu workers should happen on the local CPU */
	if (WARN_ON(cpu != smp_processor_id()))
		return -1;

	unbind_workers(cpu);

	/* update pod affinity of unbound workqueues */
	mutex_lock(&wq_pool_mutex);

	cpumask_clear_cpu(cpu, wq_online_cpumask);

	list_for_each_entry(wq, &workqueues, list) {
		struct workqueue_attrs *attrs = wq->unbound_attrs;

		if (attrs) {
			const struct wq_pod_type *pt = wqattrs_pod_type(attrs);
			int tcpu;

			for_each_cpu(tcpu, pt->pod_cpus[pt->cpu_pod[cpu]])
				unbound_wq_update_pwq(wq, tcpu);

			mutex_lock(&wq->mutex);
			wq_update_node_max_active(wq, cpu);
			mutex_unlock(&wq->mutex);
		}
	}
	mutex_unlock(&wq_pool_mutex);

	return 0;
}

struct work_for_cpu {
	struct work_struct work;
	long (*fn)(void *);
	void *arg;
	long ret;
};

/*
 * work_for_cpu_fn() - work_on_cpu() 桥接回调，在目标 CPU 进程上下文调用用户函数。
 * @work 内嵌于栈上 work_for_cpu；保存返回值并 complete。等待者保证对象寿命。
 */
static void work_for_cpu_fn(struct work_struct *work)
{
	struct work_for_cpu *wfc = container_of(work, struct work_for_cpu, work);

	wfc->ret = wfc->fn(wfc->arg);
}

/**
 * work_on_cpu_key - run a function in thread context on a particular cpu
 * @cpu: the cpu to run on
 * @fn: the function to run
 * @arg: the function arg
 * @key: The lock class key for lock debugging purposes
 *
 * It is up to the caller to ensure that the cpu doesn't go offline.
 * The caller must not hold any locks which would prevent @fn from completing.
 *
 * Return: The value @fn returns.
 */
long work_on_cpu_key(int cpu, long (*fn)(void *),
		     void *arg, struct lock_class_key *key)
{
	struct work_for_cpu wfc = { .fn = fn, .arg = arg };

	INIT_WORK_ONSTACK_KEY(&wfc.work, work_for_cpu_fn, key);
	schedule_work_on(cpu, &wfc.work);
	flush_work(&wfc.work);
	destroy_work_on_stack(&wfc.work);
	return wfc.ret;
}
EXPORT_SYMBOL_GPL(work_on_cpu_key);
#endif /* CONFIG_SMP */

#ifdef CONFIG_FREEZER

/**
 * freeze_workqueues_begin - begin freezing workqueues
 *
 * Start freezing workqueues.  After this function returns, all freezable
 * workqueues will queue new works to their inactive_works list instead of
 * pool->worklist.
 *
 * CONTEXT:
 * Grabs and releases wq_pool_mutex, wq->mutex and pool->lock's.
 */
/*
 * freeze_workqueues_begin() - suspend 冻结开始时把所有 WQ_FREEZABLE 的并发额度降为零。
 * 可睡眠、返回无。wq_pool_mutex 串行化全局遍历；已运行回调允许结束，新 work 留在
 * inactive，随后 freeze_workqueues_busy() 等待在途项归零。
 */
void freeze_workqueues_begin(void)
{
	struct workqueue_struct *wq;

	mutex_lock(&wq_pool_mutex);

	WARN_ON_ONCE(workqueue_freezing);
	workqueue_freezing = true;

	list_for_each_entry(wq, &workqueues, list) {
		mutex_lock(&wq->mutex);
		wq_adjust_max_active(wq);
		mutex_unlock(&wq->mutex);
	}

	mutex_unlock(&wq_pool_mutex);
}

/**
 * freeze_workqueues_busy - are freezable workqueues still busy?
 *
 * Check whether freezing is complete.  This function must be called
 * between freeze_workqueues_begin() and thaw_workqueues().
 *
 * CONTEXT:
 * Grabs and releases wq_pool_mutex.
 *
 * Return:
 * %true if some freezable workqueues are still busy.  %false if freezing
 * is complete.
 */
/*
 * freeze_workqueues_busy() - 检查 freezable wq 是否仍有正在执行/已激活 work。
 * 返回 true 表示 freezer 需继续等待，false 表示可进入冻结；是瞬时检查但调用协议
 * 在 begin 后已阻止新激活。
 */
bool freeze_workqueues_busy(void)
{
	bool busy = false;
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	mutex_lock(&wq_pool_mutex);

	WARN_ON_ONCE(!workqueue_freezing);

	list_for_each_entry(wq, &workqueues, list) {
		if (!(wq->flags & WQ_FREEZABLE))
			continue;
		/*
		 * nr_active is monotonically decreasing.  It's safe
		 * to peek without lock.
		 */
		rcu_read_lock();
		for_each_pwq(pwq, wq) {
			WARN_ON_ONCE(pwq->nr_active < 0);
			if (pwq->nr_active) {
				busy = true;
				rcu_read_unlock();
				goto out_unlock;
			}
		}
		rcu_read_unlock();
	}
out_unlock:
	mutex_unlock(&wq_pool_mutex);
	return busy;
}

/**
 * thaw_workqueues - thaw workqueues
 *
 * Thaw workqueues.  Normal queueing is restored and all collected
 * frozen works are transferred to their respective pool worklists.
 *
 * CONTEXT:
 * Grabs and releases wq_pool_mutex, wq->mutex and pool->lock's.
 */
/*
 * thaw_workqueues() - suspend 恢复时清 freezing 并恢复各 freezable wq 的并发额度。
 * 可睡眠、返回无；wq_adjust_max_active() 会激活冻结期间积压项并 kick pool。
 */
void thaw_workqueues(void)
{
	struct workqueue_struct *wq;

	mutex_lock(&wq_pool_mutex);

	if (!workqueue_freezing)
		goto out_unlock;

	workqueue_freezing = false;

	/* restore max_active and repopulate worklist */
	list_for_each_entry(wq, &workqueues, list) {
		mutex_lock(&wq->mutex);
		wq_adjust_max_active(wq);
		mutex_unlock(&wq->mutex);
	}

out_unlock:
	mutex_unlock(&wq_pool_mutex);
}
#endif /* CONFIG_FREEZER */

/*
 * workqueue_apply_unbound_cpumask() - 事务式更新全局 unbound 允许 CPU 集及所有 wq。
 * @unbound_cpumask 借用且入口持 wq_pool_mutex；可睡眠。先为每个 unbound wq prepare
 * 新 attrs/pwq，全部成功后才 commit 并复制全局 mask；失败负 errno 且清理上下文、
 * 旧配置不变。成功返回 0。
 */
static int workqueue_apply_unbound_cpumask(const cpumask_var_t unbound_cpumask)
{
	LIST_HEAD(ctxs);
	int ret = 0;
	struct workqueue_struct *wq;
	struct apply_wqattrs_ctx *ctx, *n;

	lockdep_assert_held(&wq_pool_mutex);

	list_for_each_entry(wq, &workqueues, list) {
		if (!(wq->flags & WQ_UNBOUND) || (wq->flags & __WQ_DESTROYING))
			continue;

		ctx = apply_wqattrs_prepare(wq, wq->unbound_attrs, unbound_cpumask);
		if (IS_ERR(ctx)) {
			ret = PTR_ERR(ctx);
			break;
		}

		list_add_tail(&ctx->list, &ctxs);
	}

	list_for_each_entry_safe(ctx, n, &ctxs, list) {
		if (!ret)
			apply_wqattrs_commit(ctx);
		apply_wqattrs_cleanup(ctx);
	}

	if (!ret) {
		int cpu;
		struct worker_pool *pool;
		struct worker *worker;

		mutex_lock(&wq_pool_attach_mutex);
		cpumask_copy(wq_unbound_cpumask, unbound_cpumask);
		/* rescuer needs to respect cpumask changes when it is not attached */
		list_for_each_entry(wq, &workqueues, list) {
			if (wq->rescuer && !wq->rescuer->pool)
				unbind_worker(wq->rescuer);
		}
		/* DISASSOCIATED worker needs to respect wq_unbound_cpumask */
		for_each_possible_cpu(cpu) {
			for_each_cpu_worker_pool(pool, cpu) {
				if (!(pool->flags & POOL_DISASSOCIATED))
					continue;
				for_each_pool_worker(worker, pool)
					unbind_worker(worker);
			}
		}
		mutex_unlock(&wq_pool_attach_mutex);
	}
	return ret;
}

/**
 * workqueue_unbound_housekeeping_update - Propagate housekeeping cpumask update
 * @hk: the new housekeeping cpumask
 *
 * Update the unbound workqueue cpumask on top of the new housekeeping cpumask such
 * that the effective unbound affinity is the intersection of the new housekeeping
 * with the requested affinity set via nohz_full=/isolcpus= or sysfs.
 *
 * Return: 0 on success and -errno on failure.
 */
/*
 * workqueue_unbound_housekeeping_update() - 把新 DOMAIN 掩码传播给 unbound WQ。
 *
 * @hk 是 kernel/sched/isolation.c 持有的新 housekeeping cpumask 借用指针，
 * 只读、不可为 NULL，函数不保存或释放它。调用者已发布全局 DOMAIN 掩码，并在
 * 调用前排空若干依赖旧策略的专用 workqueue。
 *
 * 函数可睡眠：会 GFP_KERNEL 分配临时 cpumask、获取 wq_pool_mutex，并可能通过
 * workqueue_apply_unbound_cpumask() 重建/切换 worker-pool 属性。成功返回 0；
 * 分配或应用失败返回负 errno。失败不会修改 wq_isolated_cpumask，现有 pool
 * 状态由下层 apply 的提交/清理协议保持可用。
 *
 * 有效 affinity = 用户/启动参数请求的 wq_requested_unbound_cpumask ∩ 新 @hk。
 * 若交集为空，为避免 unbound work 无 CPU 可执行，退化回 requested 集合；这时
 * 隔离偏好让位于系统活性。
 */
int workqueue_unbound_housekeeping_update(const struct cpumask *hk)
{
	/*
	 * cpumask 是本次计算的临时有效集合；ret 保存 apply 结果。临时对象始终由
	 * 本函数释放，不会发布给 worker-pool。
	 */
	cpumask_var_t cpumask;
	int ret = 0;

	/* 分配失败发生在加锁和全局状态变化前，可直接把 -ENOMEM 交还调用者。 */
	if (!zalloc_cpumask_var(&cpumask, GFP_KERNEL))
		return -ENOMEM;

	/*
	 * wq_pool_mutex 串行化 unbound cpumask、pool 属性和 worker 绑定变更；
	 * 持锁区可睡眠，不能从原子上下文调用。
	 */
	mutex_lock(&wq_pool_mutex);

	/*
	 * If the operation fails, it will fall back to
	 * wq_requested_unbound_cpumask which is initially set to
	 * HK_TYPE_DOMAIN house keeping mask and rewritten
	 * by any subsequent write to workqueue/cpumask sysfs file.
	 */
	/*
	 * 如果交集为空，操作将回退到 wq_requested_unbound_cpumask。该请求掩码最初
	 * 是 HK_TYPE_DOMAIN housekeeping 集合，之后可由 workqueue/cpumask sysfs
	 * 写入覆盖。回退确保至少保留用户请求的可执行目标，但可能暂时跨越新 HK 边界。
	 */
	if (!cpumask_and(cpumask, wq_requested_unbound_cpumask, hk))
		cpumask_copy(cpumask, wq_requested_unbound_cpumask);
	/*
	 * 掩码无变化时跳过昂贵的 pool 属性更新；变化时下层先准备所有 apply context，
	 * 再统一提交，失败则清理临时 context 并保留可用旧配置。
	 */
	if (!cpumask_equal(cpumask, wq_unbound_cpumask))
		ret = workqueue_apply_unbound_cpumask(cpumask);

	/* Save the current isolated cpumask & export it via sysfs */
	/*
	 * 保存当前 isolated 掩码并通过 sysfs 导出。只有 apply 成功后才更新，保证
	 * 用户看到的 wq_isolated_cpumask 与实际生效的 unbound pool 策略一致。
	 */
	if (!ret)
		cpumask_andnot(wq_isolated_cpumask, cpu_possible_mask, hk);

	/* 发布/回退决策完成后解锁，并结束临时 cpumask 的 ownership。 */
	mutex_unlock(&wq_pool_mutex);
	free_cpumask_var(cpumask);
	return ret;
}

/* parse_affn_scope() 把启动/sysfs 文本映射到 WQ_AFFN_*，未知值返回负 errno。 */
static int parse_affn_scope(const char *val)
{
	return sysfs_match_string(wq_affn_names, val);
}

/* wq_affn_dfl_set() 校验并更新默认 unbound affinity scope，必要时重配现有队列。 */
static int wq_affn_dfl_set(const char *val, const struct kernel_param *kp)
{
	struct workqueue_struct *wq;
	int affn, cpu;

	affn = parse_affn_scope(val);
	if (affn < 0)
		return affn;
	if (affn == WQ_AFFN_DFL)
		return -EINVAL;

	cpus_read_lock();
	mutex_lock(&wq_pool_mutex);

	wq_affn_dfl = affn;

	list_for_each_entry(wq, &workqueues, list) {
		for_each_online_cpu(cpu)
			unbound_wq_update_pwq(wq, cpu);
	}

	mutex_unlock(&wq_pool_mutex);
	cpus_read_unlock();

	return 0;
}

/* wq_affn_dfl_get() 把当前默认 scope 名称格式化到参数输出缓冲。 */
static int wq_affn_dfl_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%s\n", wq_affn_names[wq_affn_dfl]);
}

static const struct kernel_param_ops wq_affn_dfl_ops = {
	.set	= wq_affn_dfl_set,
	.get	= wq_affn_dfl_get,
};

module_param_cb(default_affinity_scope, &wq_affn_dfl_ops, NULL, 0644);

#ifdef CONFIG_SYSFS
/*
 * Workqueues with WQ_SYSFS flag set is visible to userland via
 * /sys/bus/workqueue/devices/WQ_NAME.  All visible workqueues have the
 * following attributes.
 *
 *  per_cpu		RO bool	: whether the workqueue is per-cpu or unbound
 *  max_active		RW int	: maximum number of in-flight work items
 *
 * Unbound workqueues have the following extra attributes.
 *
 *  nice		RW int	: nice value of the workers
 *  cpumask		RW mask	: bitmask of allowed CPUs for the workers
 *  affinity_scope	RW str  : worker CPU affinity scope (cache, numa, none)
 *  affinity_strict	RW bool : worker CPU affinity is strict
 */
/*
 * 只有带 WQ_SYSFS 的队列才出现在 `/sys/bus/workqueue/devices/WQ_NAME`。所有队列
 * 暴露只读 per_cpu 和可写 max_active；unbound 还允许调整 worker nice、请求 CPU
 * mask、affinity scope 与 strict。store 路径先解析到临时 attrs，再走 prepare/
 * commit 重配，因此写入失败不应留下半套 pool 映射。show 返回的是策略快照，不是
 * “下一 work 一定在哪个 CPU 执行”的承诺。wq_device 持有 wq 借用关联，device
 * release 只释放包装对象；destroy 先 unregister，阻止新 sysfs 访问。
 */
struct wq_device {
	struct workqueue_struct		*wq;
	struct device			dev;
};

static struct workqueue_struct *dev_to_wq(struct device *dev)
{
	struct wq_device *wq_dev = container_of(dev, struct wq_device, dev);

	return wq_dev->wq;
}

static ssize_t per_cpu_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", (bool)!(wq->flags & WQ_UNBOUND));
}
static DEVICE_ATTR_RO(per_cpu);

static ssize_t max_active_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", wq->saved_max_active);
}

static ssize_t max_active_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int val;

	if (sscanf(buf, "%d", &val) != 1 || val <= 0)
		return -EINVAL;

	workqueue_set_max_active(wq, val);
	return count;
}
static DEVICE_ATTR_RW(max_active);

static struct attribute *wq_sysfs_attrs[] = {
	&dev_attr_per_cpu.attr,
	&dev_attr_max_active.attr,
	NULL,
};

/* wq_sysfs_is_visible() 按 wq 类型隐藏不适用属性并返回最终 mode；纯策略查询。 */
static umode_t wq_sysfs_is_visible(struct kobject *kobj, struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct workqueue_struct *wq = dev_to_wq(dev);

	/*
	 * Adjusting max_active breaks ordering guarantee. Changing it has no
	 * effect on BH worker. Limit max_active to RO in such case.
	 */
	if (wq->flags & (WQ_BH | __WQ_ORDERED))
		return 0444;
	return a->mode;
}

static const struct attribute_group wq_sysfs_group = {
	.is_visible = wq_sysfs_is_visible,
	.attrs = wq_sysfs_attrs,
};
__ATTRIBUTE_GROUPS(wq_sysfs);

static ssize_t wq_nice_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int written;

	mutex_lock(&wq->mutex);
	written = scnprintf(buf, PAGE_SIZE, "%d\n", wq->unbound_attrs->nice);
	mutex_unlock(&wq->mutex);

	return written;
}

/* prepare workqueue_attrs for sysfs store operations */
/*
 * wq_sysfs_prep_attrs() - 为一次 sysfs store 分配并复制 wq 当前 unbound attrs。
 * 成功返回调用者拥有的可编辑副本，失败 NULL；原 wq 配置不变。
 */
static struct workqueue_attrs *wq_sysfs_prep_attrs(struct workqueue_struct *wq)
{
	struct workqueue_attrs *attrs;

	lockdep_assert_held(&wq_pool_mutex);

	attrs = alloc_workqueue_attrs();
	if (!attrs)
		return NULL;

	copy_workqueue_attrs(attrs, wq->unbound_attrs);
	return attrs;
}

static ssize_t wq_nice_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int ret = -ENOMEM;

	mutex_lock(&wq_pool_mutex);

	attrs = wq_sysfs_prep_attrs(wq);
	if (!attrs)
		goto out_unlock;

	if (sscanf(buf, "%d", &attrs->nice) == 1 &&
	    attrs->nice >= MIN_NICE && attrs->nice <= MAX_NICE)
		ret = apply_workqueue_attrs_locked(wq, attrs);
	else
		ret = -EINVAL;

out_unlock:
	mutex_unlock(&wq_pool_mutex);
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static ssize_t wq_cpumask_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int written;

	mutex_lock(&wq->mutex);
	written = scnprintf(buf, PAGE_SIZE, "%*pb\n",
			    cpumask_pr_args(wq->unbound_attrs->cpumask));
	mutex_unlock(&wq->mutex);
	return written;
}

static ssize_t wq_cpumask_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int ret = -ENOMEM;

	mutex_lock(&wq_pool_mutex);

	attrs = wq_sysfs_prep_attrs(wq);
	if (!attrs)
		goto out_unlock;

	ret = cpumask_parse(buf, attrs->cpumask);
	if (!ret)
		ret = apply_workqueue_attrs_locked(wq, attrs);

out_unlock:
	mutex_unlock(&wq_pool_mutex);
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static ssize_t wq_affn_scope_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int written;

	mutex_lock(&wq->mutex);
	if (wq->unbound_attrs->affn_scope == WQ_AFFN_DFL)
		written = scnprintf(buf, PAGE_SIZE, "%s (%s)\n",
				    wq_affn_names[WQ_AFFN_DFL],
				    wq_affn_names[wq_affn_dfl]);
	else
		written = scnprintf(buf, PAGE_SIZE, "%s\n",
				    wq_affn_names[wq->unbound_attrs->affn_scope]);
	mutex_unlock(&wq->mutex);

	return written;
}

static ssize_t wq_affn_scope_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int affn, ret = -ENOMEM;

	affn = parse_affn_scope(buf);
	if (affn < 0)
		return affn;

	mutex_lock(&wq_pool_mutex);
	attrs = wq_sysfs_prep_attrs(wq);
	if (attrs) {
		attrs->affn_scope = affn;
		ret = apply_workqueue_attrs_locked(wq, attrs);
	}
	mutex_unlock(&wq_pool_mutex);
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static ssize_t wq_affinity_strict_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 wq->unbound_attrs->affn_strict);
}

static ssize_t wq_affinity_strict_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int v, ret = -ENOMEM;

	if (sscanf(buf, "%d", &v) != 1)
		return -EINVAL;

	mutex_lock(&wq_pool_mutex);
	attrs = wq_sysfs_prep_attrs(wq);
	if (attrs) {
		attrs->affn_strict = (bool)v;
		ret = apply_workqueue_attrs_locked(wq, attrs);
	}
	mutex_unlock(&wq_pool_mutex);
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static struct device_attribute wq_sysfs_unbound_attrs[] = {
	__ATTR(nice, 0644, wq_nice_show, wq_nice_store),
	__ATTR(cpumask, 0644, wq_cpumask_show, wq_cpumask_store),
	__ATTR(affinity_scope, 0644, wq_affn_scope_show, wq_affn_scope_store),
	__ATTR(affinity_strict, 0644, wq_affinity_strict_show, wq_affinity_strict_store),
	__ATTR_NULL,
};

static const struct bus_type wq_subsys = {
	.name				= "workqueue",
	.dev_groups			= wq_sysfs_groups,
};

/**
 *  workqueue_set_unbound_cpumask - Set the low-level unbound cpumask
 *  @cpumask: the cpumask to set
 *
 *  The low-level workqueues cpumask is a global cpumask that limits
 *  the affinity of all unbound workqueues.  This function check the @cpumask
 *  and apply it to all unbound workqueues and updates all pwqs of them.
 *
 *  Return:	0	- Success
 *		-EINVAL	- Invalid @cpumask
 *		-ENOMEM	- Failed to allocate memory for attrs or pwqs.
 */
/*
 * workqueue_set_unbound_cpumask() - 应用用户请求 mask 与 cmdline/housekeeping 约束。
 * @cpumask 借用；可睡眠。成功 0 并更新 requested/实际 mask，失败负 errno 且旧配置
 * 保持。空交集采用受约束的保活回退。
 */
static int workqueue_set_unbound_cpumask(cpumask_var_t cpumask)
{
	int ret = -EINVAL;

	/*
	 * Not excluding isolated cpus on purpose.
	 * If the user wishes to include them, we allow that.
	 */
	cpumask_and(cpumask, cpumask, cpu_possible_mask);
	if (!cpumask_empty(cpumask)) {
		ret = 0;
		mutex_lock(&wq_pool_mutex);
		if (!cpumask_equal(cpumask, wq_unbound_cpumask))
			ret = workqueue_apply_unbound_cpumask(cpumask);
		if (!ret)
			cpumask_copy(wq_requested_unbound_cpumask, cpumask);
		mutex_unlock(&wq_pool_mutex);
	}

	return ret;
}

static ssize_t __wq_cpumask_show(struct device *dev,
		struct device_attribute *attr, char *buf, cpumask_var_t mask)
{
	int written;

	mutex_lock(&wq_pool_mutex);
	written = scnprintf(buf, PAGE_SIZE, "%*pb\n", cpumask_pr_args(mask));
	mutex_unlock(&wq_pool_mutex);

	return written;
}

static ssize_t cpumask_requested_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return __wq_cpumask_show(dev, attr, buf, wq_requested_unbound_cpumask);
}
static DEVICE_ATTR_RO(cpumask_requested);

static ssize_t cpumask_isolated_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return __wq_cpumask_show(dev, attr, buf, wq_isolated_cpumask);
}
static DEVICE_ATTR_RO(cpumask_isolated);

static ssize_t cpumask_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return __wq_cpumask_show(dev, attr, buf, wq_unbound_cpumask);
}

static ssize_t cpumask_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	cpumask_var_t cpumask;
	int ret;

	if (!zalloc_cpumask_var(&cpumask, GFP_KERNEL))
		return -ENOMEM;

	ret = cpumask_parse(buf, cpumask);
	if (!ret)
		ret = workqueue_set_unbound_cpumask(cpumask);

	free_cpumask_var(cpumask);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(cpumask);

static struct attribute *wq_sysfs_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	&dev_attr_cpumask_requested.attr,
	&dev_attr_cpumask_isolated.attr,
	NULL,
};
ATTRIBUTE_GROUPS(wq_sysfs_cpumask);

/* wq_sysfs_init() 启动期注册 workqueue bus 与全局 cpumask 属性组，返回 0 或负 errno。 */
static int __init wq_sysfs_init(void)
{
	return subsys_virtual_register(&wq_subsys, wq_sysfs_cpumask_groups);
}
core_initcall(wq_sysfs_init);

/* wq_device_release() 在最后 device 引用归零时释放 wq_device 包装，不释放其 wq。 */
static void wq_device_release(struct device *dev)
{
	struct wq_device *wq_dev = container_of(dev, struct wq_device, dev);

	kfree(wq_dev);
}

/**
 * workqueue_sysfs_register - make a workqueue visible in sysfs
 * @wq: the workqueue to register
 *
 * Expose @wq in sysfs under /sys/bus/workqueue/devices.
 * alloc_workqueue*() automatically calls this function if WQ_SYSFS is set
 * which is the preferred method.
 *
 * Workqueue user should use this function directly iff it wants to apply
 * workqueue_attrs before making the workqueue visible in sysfs; otherwise,
 * apply_workqueue_attrs() may race against userland updating the
 * attributes.
 *
 * Return: 0 on success, -errno on failure.
 */
/*
 * workqueue_sysfs_register() - 为带 WQ_SYSFS 的存活 wq 创建并发布 device。
 * 可睡眠；成功 0，失败负 errno 并回滚包装/属性，不改变 wq 执行能力。
 */
int workqueue_sysfs_register(struct workqueue_struct *wq)
{
	struct wq_device *wq_dev;
	int ret;

	wq->wq_dev = wq_dev = kzalloc_obj(*wq_dev);
	if (!wq_dev)
		return -ENOMEM;

	wq_dev->wq = wq;
	wq_dev->dev.bus = &wq_subsys;
	wq_dev->dev.release = wq_device_release;
	dev_set_name(&wq_dev->dev, "%s", wq->name);

	/*
	 * unbound_attrs are created separately.  Suppress uevent until
	 * everything is ready.
	 */
	dev_set_uevent_suppress(&wq_dev->dev, true);

	ret = device_register(&wq_dev->dev);
	if (ret) {
		put_device(&wq_dev->dev);
		wq->wq_dev = NULL;
		return ret;
	}

	if (wq->flags & WQ_UNBOUND) {
		struct device_attribute *attr;

		for (attr = wq_sysfs_unbound_attrs; attr->attr.name; attr++) {
			ret = device_create_file(&wq_dev->dev, attr);
			if (ret) {
				device_unregister(&wq_dev->dev);
				wq->wq_dev = NULL;
				return ret;
			}
		}
	}

	dev_set_uevent_suppress(&wq_dev->dev, false);
	kobject_uevent(&wq_dev->dev.kobj, KOBJ_ADD);
	return 0;
}

/**
 * workqueue_sysfs_unregister - undo workqueue_sysfs_register()
 * @wq: the workqueue to unregister
 *
 * If @wq is registered to sysfs by workqueue_sysfs_register(), unregister.
 */
/*
 * workqueue_sysfs_unregister() - 若已注册则清 wq_dev 并 unregister device。
 * 可睡眠；返回无。device 引用延迟包装释放，但新 sysfs 操作已被阻止。
 */
static void workqueue_sysfs_unregister(struct workqueue_struct *wq)
{
	struct wq_device *wq_dev = wq->wq_dev;

	if (!wq->wq_dev)
		return;

	wq->wq_dev = NULL;
	device_unregister(&wq_dev->dev);
}
#else	/* CONFIG_SYSFS */
static void workqueue_sysfs_unregister(struct workqueue_struct *wq)	{ }
#endif	/* CONFIG_SYSFS */

/*
 * Workqueue watchdog.
 *
 * Stall may be caused by various bugs - missing WQ_MEM_RECLAIM, illegal
 * flush dependency, a concurrency managed work item which stays RUNNING
 * indefinitely.  Workqueue stalls can be very difficult to debug as the
 * usual warning mechanisms don't trigger and internal workqueue state is
 * largely opaque.
 *
 * Workqueue watchdog monitors all worker pools periodically and dumps
 * state if some pools failed to make forward progress for a while where
 * forward progress is defined as the first item on ->worklist changing.
 *
 * This mechanism is controlled through the kernel parameter
 * "workqueue.watchdog_thresh" which can be updated at runtime through the
 * corresponding sysfs parameter file.
 */
/*
 * workqueue watchdog 周期检查每个 pool 的 worklist 表头是否发生变化，把“表头长期
 * 不变”定义为缺少前进。成因可能是遗漏 WQ_MEM_RECLAIM、非法 flush 环或 running
 * 回调永久阻塞；普通 hung-task/lockup 不一定能看见这些内部等待。阈值由
 * workqueue.watchdog_thresh 动态控制。touched 全局/per-CPU 时间允许调试器或已知
 * 长临界区主动刷新观察点；cpu_stall 只供本轮诊断标记。watchdog 输出使用 deferred
 * printk，避免 console 驱动自身排 work 时形成递归锁死；可选策略按累计次数或持续
 * 秒数 panic。
 */
#ifdef CONFIG_WQ_WATCHDOG

static unsigned long wq_watchdog_thresh = 30;
static struct timer_list wq_watchdog_timer;

static unsigned long wq_watchdog_touched = INITIAL_JIFFIES;
static DEFINE_PER_CPU(unsigned long, wq_watchdog_touched_cpu) = INITIAL_JIFFIES;

static unsigned int wq_panic_on_stall = CONFIG_BOOTPARAM_WQ_STALL_PANIC;
module_param_named(panic_on_stall, wq_panic_on_stall, uint, 0644);

static unsigned int wq_panic_on_stall_time;
module_param_named(panic_on_stall_time, wq_panic_on_stall_time, uint, 0644);
MODULE_PARM_DESC(panic_on_stall_time, "Panic if stall exceeds this many seconds (0=disabled)");

/*
 * Show workers that might prevent the processing of pending work items.
 * A busy worker that is not running on the CPU (e.g. sleeping in
 * wait_event_idle() with PF_WQ_WORKER cleared) can stall the pool just as
 * effectively as a CPU-bound one, so dump every in-flight worker.
 */
/*
 * show_cpu_pool_busy_workers() - 打印可能阻挡 stalled pool 的全部 busy worker。
 * @pool RCU 下存活，函数用 pool lock 稳定 busy_hash。不能只看正在 CPU 上运行者：
 * 清 PF_WQ_WORKER 后睡眠的回调同样占着 in-flight/依赖。返回无，仅诊断。
 */
static void show_cpu_pool_busy_workers(struct worker_pool *pool)
{
	struct worker *worker;
	unsigned long irq_flags;
	int bkt;

	raw_spin_lock_irqsave(&pool->lock, irq_flags);

	hash_for_each(pool->busy_hash, bkt, worker, hentry) {
		/*
		 * Defer printing to avoid deadlocks in console
		 * drivers that queue work while holding locks
		 * also taken in their write paths.
		 */
		printk_deferred_enter();

		pr_info("pool %d:\n", pool->id);
		sched_show_task(worker->task);

		printk_deferred_exit();
	}

	raw_spin_unlock_irqrestore(&pool->lock, irq_flags);
}

/* show_cpu_pools_busy_workers() 在 RCU 下遍历本轮标为 cpu_stall 的 pool 并打印线程栈。 */
static void show_cpu_pools_busy_workers(void)
{
	struct worker_pool *pool;
	int pi;

	pr_info("Showing backtraces of busy workers in stalled worker pools:\n");

	rcu_read_lock();

	for_each_pool(pool, pi) {
		if (pool->cpu_stall)
			show_cpu_pool_busy_workers(pool);

	}

	rcu_read_unlock();
}

/*
 * It triggers a panic in two scenarios: when the total number of stalls
 * exceeds a threshold, and when a stall lasts longer than
 * wq_panic_on_stall_time
 */
/*
 * panic_on_wq_watchdog() - 按累计 stall 次数或本次持续时间执行可配置 panic 策略。
 * @stall_time_sec 为本次最长停顿秒数；未达阈值只记账，无返回状态。
 */
static void panic_on_wq_watchdog(unsigned int stall_time_sec)
{
	static unsigned int wq_stall;

	if (wq_panic_on_stall) {
		wq_stall++;
		if (wq_stall >= wq_panic_on_stall)
			panic("workqueue: %u stall(s) exceeded threshold %u\n",
			      wq_stall, wq_panic_on_stall);
	}

	if (wq_panic_on_stall_time && stall_time_sec >= wq_panic_on_stall_time)
		panic("workqueue: stall lasted %us, exceeding threshold %us\n",
		      stall_time_sec, wq_panic_on_stall_time);
}

/* wq_watchdog_reset_touched() 把全局与每 CPU 前进基准统一刷新到当前 jiffies。 */
static void wq_watchdog_reset_touched(void)
{
	int cpu;

	wq_watchdog_touched = jiffies;
	for_each_possible_cpu(cpu)
		per_cpu(wq_watchdog_touched_cpu, cpu) = jiffies;
}

/*
 * wq_watchdog_timer_fn() - timer 上下文扫描所有 pool 的 last_progress_ts/worklist。
 * 标记并报告超过阈值的 stall，必要时 panic，然后按当前阈值重装 timer。不能睡眠。
 */
static void wq_watchdog_timer_fn(struct timer_list *unused)
{
	unsigned long thresh = READ_ONCE(wq_watchdog_thresh) * HZ;
	unsigned int max_stall_time = 0;
	bool lockup_detected = false;
	bool cpu_pool_stall = false;
	unsigned long now = jiffies;
	struct worker_pool *pool;
	unsigned int stall_time;
	int pi;

	if (!thresh)
		return;

	for_each_pool(pool, pi) {
		unsigned long pool_ts, touched, ts;

		pool->cpu_stall = false;
		if (list_empty(&pool->worklist))
			continue;

		/*
		 * If a virtual machine is stopped by the host it can look to
		 * the watchdog like a stall.
		 */
		kvm_check_and_clear_guest_paused();

		/* get the latest of pool and touched timestamps */
		if (pool->cpu >= 0)
			touched = READ_ONCE(per_cpu(wq_watchdog_touched_cpu, pool->cpu));
		else
			touched = READ_ONCE(wq_watchdog_touched);
		pool_ts = READ_ONCE(pool->last_progress_ts);

		if (time_after(pool_ts, touched))
			ts = pool_ts;
		else
			ts = touched;

		/*
		 * Did we stall?
		 *
		 * Do a lockless check first to do not disturb the system.
		 *
		 * Prevent false positives by double checking the timestamp
		 * under pool->lock. The lock makes sure that the check reads
		 * an updated pool->last_progress_ts when this CPU saw
		 * an already updated pool->worklist above. It seems better
		 * than adding another barrier into __queue_work() which
		 * is a hotter path.
		 */
		if (time_after(now, ts + thresh)) {
			scoped_guard(raw_spinlock_irqsave, &pool->lock) {
				pool_ts = pool->last_progress_ts;
				if (time_after(pool_ts, touched))
					ts = pool_ts;
				else
					ts = touched;
			}
			if (!time_after(now, ts + thresh))
				continue;

			lockup_detected = true;
			stall_time = jiffies_to_msecs(now - pool_ts) / 1000;
			max_stall_time = max(max_stall_time, stall_time);
			if (pool->cpu >= 0 && !(pool->flags & POOL_BH)) {
				pool->cpu_stall = true;
				cpu_pool_stall = true;
			}
			pr_emerg("BUG: workqueue lockup - pool");
			pr_cont_pool_info(pool);
			pr_cont(" stuck for %us!\n", stall_time);
		}
	}

	if (lockup_detected)
		show_all_workqueues();

	if (cpu_pool_stall)
		show_cpu_pools_busy_workers();

	if (lockup_detected)
		panic_on_wq_watchdog(max_stall_time);

	wq_watchdog_reset_touched();
	mod_timer(&wq_watchdog_timer, jiffies + thresh);
}

/*
 * wq_watchdog_touch() - 告知 watchdog 当前/指定 CPU 的长路径仍有已知前进。
 * notrace 防止追踪递归；@cpu < 0 刷新全局，否则刷新 per-CPU 时间。无睡眠。
 */
notrace void wq_watchdog_touch(int cpu)
{
	unsigned long thresh = READ_ONCE(wq_watchdog_thresh) * HZ;
	unsigned long touch_ts = READ_ONCE(wq_watchdog_touched);
	unsigned long now = jiffies;

	if (cpu >= 0)
		per_cpu(wq_watchdog_touched_cpu, cpu) = now;
	else
		WARN_ONCE(1, "%s should be called with valid CPU", __func__);

	/* Don't unnecessarily store to global cacheline */
	if (time_after(now, touch_ts + thresh / 4))
		WRITE_ONCE(wq_watchdog_touched, jiffies);
}

/* wq_watchdog_set_thresh() 更新秒级阈值并安全重置/停止 watchdog timer。 */
static void wq_watchdog_set_thresh(unsigned long thresh)
{
	wq_watchdog_thresh = 0;
	timer_delete_sync(&wq_watchdog_timer);

	if (thresh) {
		wq_watchdog_thresh = thresh;
		wq_watchdog_reset_touched();
		mod_timer(&wq_watchdog_timer, jiffies + thresh * HZ);
	}
}

static int wq_watchdog_param_set_thresh(const char *val,
					const struct kernel_param *kp)
{
	unsigned long thresh;
	int ret;

	ret = kstrtoul(val, 0, &thresh);
	if (ret)
		return ret;

	if (system_percpu_wq)
		wq_watchdog_set_thresh(thresh);
	else
		wq_watchdog_thresh = thresh;

	return 0;
}

static const struct kernel_param_ops wq_watchdog_thresh_ops = {
	.set	= wq_watchdog_param_set_thresh,
	.get	= param_get_ulong,
};

module_param_cb(watchdog_thresh, &wq_watchdog_thresh_ops, &wq_watchdog_thresh,
		0644);

/* wq_watchdog_init() 在 workqueue 可运行后初始化并启动 watchdog timer。 */
static void wq_watchdog_init(void)
{
	timer_setup(&wq_watchdog_timer, wq_watchdog_timer_fn, TIMER_DEFERRABLE);
	wq_watchdog_set_thresh(wq_watchdog_thresh);
}

#else	/* CONFIG_WQ_WATCHDOG */

static inline void wq_watchdog_init(void) { }

#endif	/* CONFIG_WQ_WATCHDOG */

/* bh_pool_kick_normal() 是远端 irq_work 回调，只负责 raise 当前 CPU 普通 TASKLET softirq。 */
static void bh_pool_kick_normal(struct irq_work *irq_work)
{
	raise_softirq_irqoff(TASKLET_SOFTIRQ);
}

/* bh_pool_kick_highpri() 对称地触发 HI_SOFTIRQ；IRQ 上下文不可睡眠。 */
static void bh_pool_kick_highpri(struct irq_work *irq_work)
{
	raise_softirq_irqoff(HI_SOFTIRQ);
}

/*
 * restrict_unbound_cpumask - 将 wq_unbound_cpumask 收窄为与 mask 的交集
 * @name: mask 的名称（仅用于警告日志）
 * @mask: 要与当前 wq_unbound_cpumask 取交集的 CPU 集合
 *
 * unbound workqueue 的 worker 只能运行在 wq_unbound_cpumask 内的 CPU 上。
 * 此函数用于逐步缩减该集合：先排除被 nohz_full/isolcpus 隔离的 CPU，
 * 再排除命令行 workqueue.unbound_cpus= 指定的 CPU。
 *
 * 安全检查：若交集为空（收窄后无任何 CPU 可用），则忽略本次限制并打印警告，
 * 保留原有 wq_unbound_cpumask 不变，确保系统始终有 CPU 可处理 unbound work。
 */
/*
 * restrict_unbound_cpumask() - 启动期把命名约束 mask 合并进全局 unbound 允许集。
 * @name 仅用于日志，@mask 借用；交集为空时保留保活集合并告警。__init 返回无。
 */
static void __init restrict_unbound_cpumask(const char *name, const struct cpumask *mask)
{
	/* 若当前 wq_unbound_cpumask 与 mask 无交集，收窄后将没有任何 CPU 可用，
	 * 忽略本次限制并打印警告，否则系统无法处理 unbound workqueue 中的 work。 */
	if (!cpumask_intersects(wq_unbound_cpumask, mask)) {
		pr_warn("workqueue: Restricting unbound_cpumask (%*pb) with %s (%*pb) leaves no CPU, ignoring\n",
			cpumask_pr_args(wq_unbound_cpumask), name, cpumask_pr_args(mask));
		return;
	}

	/* 取交集：只保留两个 mask 共同包含的 CPU。
	 * 结果写回 wq_unbound_cpumask，单调递减，不会增加 CPU。 */
	cpumask_and(wq_unbound_cpumask, wq_unbound_cpumask, mask);
}

/*
 * init_cpu_worker_pool - 初始化一个 per-CPU worker pool
 * @pool: 指向 per-CPU 静态数组中的 worker_pool 对象（已被 BSS 清零）
 * @cpu:  该 pool 绑定的物理 CPU 编号
 * @nice: worker 线程的调度优先级（0=普通，HIGHPRI_NICE_LEVEL=-20=高优先级）
 *
 * 每个 CPU 有两个标准 worker pool（普通 + 高优先级），本函数对其中一个做
 * 完整初始化。初始化后 pool 有全局唯一 ID，可以接受 work 入队，但尚无
 * worker 线程——线程在后续 workqueue_init() 中创建。
 */
/*
 * init_cpu_worker_pool() - 启动期初始化一个 per-CPU 普通/高优先级静态 pool。
 * @pool 静态存储，@cpu 关联 CPU，@nice 决定优先级；初始化 attrs、严格单 CPU mask、
 * NUMA node 与全局 ID。失败属于不可恢复早期启动错误，以 BUG 暴露。
 */
static void __init init_cpu_worker_pool(struct worker_pool *pool, int cpu, int nice)
{
	/* init_worker_pool() 做通用初始化：
	 *   - 初始化 pool->lock（raw spinlock）
	 *   - 初始化 worklist、idle_list、busy_hash 等链表/哈希表
	 *   - 设置 idle_timer（延迟定时器）和 mayday_timer（救援定时器）
	 *   - 初始化 worker_ida（为 worker 分配编号的 ID 分配器）
	 *   - 分配并初始化 pool->attrs（workqueue_attrs 结构体）
	 * 返回非零表示失败，此时直接 BUG_ON 崩溃（系统无法运行）。 */
	BUG_ON(init_worker_pool(pool));

	/* 将 pool 绑定到指定物理 CPU。 */
	pool->cpu = cpu;

	/* 设置 worker 线程的调度亲和性为单个 CPU（cpumask_of(cpu) 仅含该 CPU），
	 * 确保该 pool 的 worker 只在 cpu 上运行。 */
	cpumask_copy(pool->attrs->cpumask, cpumask_of(cpu));

	/* __pod_cpumask 是 pod 内部调度用的 cpumask，per-CPU pool 同样只含一个 CPU。
	 * pod 是 workqueue 亲和性系统的基本调度单位，per-CPU pool 的 pod 就是自身。 */
	cpumask_copy(pool->attrs->__pod_cpumask, cpumask_of(cpu));

	/* 设置 worker 线程的 nice 值，决定调度优先级。 */
	pool->attrs->nice = nice;

	/* affn_strict = true：严格亲和性模式，worker 线程只能在 cpumask 指定的 CPU
	 * 上运行，不允许迁移到其他 CPU（per-CPU pool 必须严格绑定单个 CPU）。 */
	pool->attrs->affn_strict = true;

	/* 将 pool 关联到 CPU 所在的 NUMA 节点，后续 worker 分配内存时优先在本节点，
	 * 降低跨 NUMA 内存访问延迟。 */
	pool->node = cpu_to_node(cpu);

	/* alloc pool ID
	 * 在全局 worker_pool_idr 中为该 pool 分配唯一整数 ID（写入 pool->id）。
	 * work 项的 WORK_OFFQ 字段用此 ID 记录"上次运行在哪个 pool"，
	 * 用于 work 的 CPU 亲和性保持（cache 热度优化）。
	 * 必须持 wq_pool_mutex 互斥锁，防止并发分配 ID 冲突。 */
	mutex_lock(&wq_pool_mutex);
	BUG_ON(worker_pool_assign_id(pool));
	mutex_unlock(&wq_pool_mutex);
}

/**
 * workqueue_init_early - early init for workqueue subsystem
 *
 * This is the first step of three-staged workqueue subsystem initialization and
 * invoked as soon as the bare basics - memory allocation, cpumasks and idr are
 * up. It sets up all the data structures and system workqueues and allows early
 * boot code to create workqueues and queue/cancel work items. Actual work item
 * execution starts only after kthreads can be created and scheduled right
 * before early initcalls.
 *
 * workqueue 子系统三阶段初始化的第一阶段：
 *   1. workqueue_init_early()  ← 本函数，内存/cpumask/IDR 就绪后立即调用
 *   2. workqueue_init()        ← kthread 可创建后调用，启动实际 worker 线程
 *   3. workqueue_init_topology() ← CPU 拓扑可用后调用，建立 NUMA/LLC pod 分组
 *
 * 完成后 work 可以入队，但实际执行要等到 workqueue_init() 创建 worker 线程后。
 */
/*
 * workqueue_init_early() - 三阶段初始化第一步：建立全部静态 pool、全局 mask/cache、
 * 系统 workqueue 与拓扑占位。此时允许创建/排队/取消 work，但尚不能执行，因为
 * kthread 未启动。无入参/返回，早期关键分配失败会 panic/BUG。
 */
void __init workqueue_init_early(void)
{
	/* pt：WQ_AFFN_SYSTEM pod 类型，"整个系统只有一个调度 pod"的策略，
	 * 适用于不需要 NUMA/LLC 亲和性的系统级 workqueue。
	 * wq_pod_types[] 按 wq_affn_scope 枚举索引，每种亲和策略一个 pod 类型。 */
	struct wq_pod_type *pt = &wq_pod_types[WQ_AFFN_SYSTEM];

	/* 两个标准 worker pool 的优先级：
	 * [0]=0 普通优先级，[1]=HIGHPRI_NICE_LEVEL(-20) 高优先级。
	 * 每个 CPU 有这两种 pool，分别服务普通 work 和高优先级 work。 */
	int std_nice[NR_STD_WORKER_POOLS] = { 0, HIGHPRI_NICE_LEVEL };

	/* BH pool 的 irq_work 回调函数：
	 * BH（Bottom Half）pool 中的 work 运行在 softirq 上下文，
	 * 需要通过 irq_work 机制跨 CPU 安全触发 softirq 来唤醒工作：
	 * [0]=bh_pool_kick_normal 触发 TASKLET_SOFTIRQ（普通软中断）
	 * [1]=bh_pool_kick_highpri 触发 HI_SOFTIRQ（高优先级软中断）*/
	void (*irq_work_fns[NR_STD_WORKER_POOLS])(struct irq_work *) =
		{ bh_pool_kick_normal, bh_pool_kick_highpri };
	int i, cpu;

	/* 编译期断言：pool_workqueue 的对齐至少为 sizeof(long long)（8 字节）。
	 * pool_workqueue 指针的低位 bit 被用于存储标志位（如 PWQ_LINKED），
	 * 必须保证低位可用，8 字节对齐确保低 3 位始终为 0。 */
	BUILD_BUG_ON(__alignof__(struct pool_workqueue) < __alignof__(long long));

	/* ── 阶段1：分配并初始化全局 cpumask ────────────────────────────────── */

	/* 分配四个核心 cpumask，失败则 BUG_ON（系统无法运行）：
	 * wq_online_cpumask：当前在线 CPU 的镜像，排除热插拔过渡中的 CPU
	 * wq_unbound_cpumask：unbound workqueue 实际可用的 CPU 集合（逐步缩减）
	 * wq_requested_unbound_cpumask：用户通过 sysfs 请求的 unbound CPU 集合
	 * wq_isolated_cpumask：被 nohz_full/isolcpus 隔离的 CPU 集合（初始清零）*/
	BUG_ON(!alloc_cpumask_var(&wq_online_cpumask, GFP_KERNEL));
	BUG_ON(!alloc_cpumask_var(&wq_unbound_cpumask, GFP_KERNEL));
	BUG_ON(!alloc_cpumask_var(&wq_requested_unbound_cpumask, GFP_KERNEL));
	BUG_ON(!zalloc_cpumask_var(&wq_isolated_cpumask, GFP_KERNEL));

	/* 初始化在线 CPU 掩码。 */
	cpumask_copy(wq_online_cpumask, cpu_online_mask);

	/* unbound 掩码从所有 possible CPU 开始，然后逐步收窄：
	 * 第一步：排除被 nohz_full/isolcpus 隔离的 CPU（HK_TYPE_DOMAIN 是管家 CPU
	 *         集合，即未被隔离的 CPU）。隔离 CPU 专跑实时任务，不接受内核杂务。 */
	cpumask_copy(wq_unbound_cpumask, cpu_possible_mask);
	restrict_unbound_cpumask("HK_TYPE_DOMAIN", housekeeping_cpumask(HK_TYPE_DOMAIN));

	/* 第二步：若用户通过命令行参数 workqueue.unbound_cpus= 指定了额外限制，
	 * 进一步缩减 unbound 可用 CPU 集合。 */
	if (!cpumask_empty(&wq_cmdline_cpumask))
		restrict_unbound_cpumask("workqueue.unbound_cpus", &wq_cmdline_cpumask);

	/* 保存收窄后的 unbound 掩码作为"用户请求值"基准，
	 * 供后续 sysfs 接口（/sys/devices/virtual/workqueue/cpumask）使用。 */
	cpumask_copy(wq_requested_unbound_cpumask, wq_unbound_cpumask);

	/* 计算被隔离的 CPU 集合：possible - housekeeping = isolated。
	 * 用于后续判断某个 CPU 是否被隔离（如调整 work 亲和性时跳过隔离 CPU）。 */
	cpumask_andnot(wq_isolated_cpumask, cpu_possible_mask,
						housekeeping_cpumask(HK_TYPE_DOMAIN));

	/* ── 阶段2：创建 slab 缓存和全局缓冲区 ──────────────────────────────── */

	/* 为 pool_workqueue 创建专用 slab 缓存。
	 * pool_workqueue（pwq）是 workqueue 与 worker_pool 之间的绑定结构，
	 * 每次 workqueue 绑定到一个 pool 时分配，解绑时释放，频繁操作，
	 * 专用 slab 缓存比 kmalloc 更快且内存利用率更高。 */
	pwq_cache = KMEM_CACHE(pool_workqueue, SLAB_PANIC);

	/* 分配全局共用的 workqueue_attrs 临时缓冲区，
	 * 供 wq_update_unbound_pod_attrs() 在更新 unbound workqueue 配置时使用，
	 * 受 CPU hotplug 排他锁保护，无需每次分配/释放。 */
	unbound_wq_update_pwq_attrs_buf = alloc_workqueue_attrs();
	BUG_ON(!unbound_wq_update_pwq_attrs_buf);

	/*
	 * If nohz_full is enabled, set power efficient workqueue as unbound.
	 * This allows workqueue items to be moved to HK CPUs.
	 *
	 * nohz_full 启用时（HK_TYPE_TICK 等同于 HK_TYPE_KERNEL_NOISE），
	 * 将 WQ_POWER_EFFICIENT workqueue 设为 unbound 模式，
	 * 使 work 项可以迁移到管家 CPU 上执行，避免打扰隔离 CPU 的 nohz 状态。
	 */
	if (housekeeping_enabled(HK_TYPE_TICK))
		wq_power_efficient = true;

	/* ── 阶段3：初始化 WQ_AFFN_SYSTEM pod ───────────────────────────────── */

	/* initialize WQ_AFFN_SYSTEM pods
	 *
	 * pod（工作组）是 workqueue 亲和性调度的基本单位。WQ_AFFN_SYSTEM 是最宽松
	 * 的策略：整个系统只有 1 个 pod，所有 CPU 共享同一组 unbound worker。
	 *
	 * 分配三个数组（kzalloc_objs 使用类型安全的 typeof 推导分配大小）：
	 * pod_cpus：pod 编号 → 该 pod 包含哪些 CPU（1 个 cpumask_var_t）
	 * pod_node：pod 编号 → NUMA 节点（1 个 int，AFFN_SYSTEM 不绑定节点）
	 * cpu_pod： CPU 编号 → 所属 pod 编号（nr_cpu_ids 个 int，每个 CPU 一项） */
	pt->pod_cpus = kzalloc_objs(pt->pod_cpus[0], 1);
	pt->pod_node = kzalloc_objs(pt->pod_node[0], 1);
	pt->cpu_pod = kzalloc_objs(pt->cpu_pod[0], nr_cpu_ids);
	BUG_ON(!pt->pod_cpus || !pt->pod_node || !pt->cpu_pod);

	/* 为 pod 0 的 cpumask 分配 bitmap 内存，NUMA_NO_NODE 表示不偏好特定节点。 */
	BUG_ON(!zalloc_cpumask_var_node(&pt->pod_cpus[0], GFP_KERNEL, NUMA_NO_NODE));

	/* 配置系统级 pod：1 个 pod，包含所有 possible CPU，不绑定 NUMA 节点，
	 * CPU 0（以及后续通过 cpu hotplug 加入的 CPU）都映射到 pod 0。
	 * 其他亲和策略（NUMA/LLC/SMT）的 pod 在 workqueue_init_topology() 中建立。 */
	pt->nr_pods = 1;
	cpumask_copy(pt->pod_cpus[0], cpu_possible_mask);
	pt->pod_node[0] = NUMA_NO_NODE;
	pt->cpu_pod[0] = 0;

	/* ── 阶段4：初始化每个 CPU 的 BH 和普通 worker pool ────────────────── */

	/* initialize BH and CPU pools
	 * 遍历所有 possible CPU，为每个 CPU 初始化 4 个 worker pool（2 BH + 2 普通）。
	 * 此时只建立数据结构，没有创建任何 worker 线程（线程在 workqueue_init() 创建）。 */
	for_each_possible_cpu(cpu) {
		struct worker_pool *pool;

		/* BH worker pool：运行在 softirq 上下文，而非 kthread。
		 * 适用于 WQ_BH 类型的 work，延迟极低但不能睡眠。
		 * POOL_BH 标志告知调度器此 pool 由 softirq 驱动而非 kthread。
		 * init_irq_work 绑定跨 CPU 触发 softirq 的回调，使其他 CPU 能唤醒本 pool。 */
		i = 0;
		for_each_bh_worker_pool(pool, cpu) {
			init_cpu_worker_pool(pool, cpu, std_nice[i]);
			pool->flags |= POOL_BH;
			/* 绑定 irq_work 回调：普通 BH pool 用 bh_pool_kick_normal
			 * 触发 TASKLET_SOFTIRQ，高优先级 BH pool 用 bh_pool_kick_highpri
			 * 触发 HI_SOFTIRQ，实现跨 CPU 安全唤醒。 */
			init_irq_work(bh_pool_irq_work(pool), irq_work_fns[i]);
			i++;
		}

		/* 普通 CPU worker pool：由 kthread（worker 线程）驱动，可以睡眠。
		 * 适用于绝大多数 work，两个 pool 分别对应普通和高优先级。 */
		i = 0;
		for_each_cpu_worker_pool(pool, cpu)
			init_cpu_worker_pool(pool, cpu, std_nice[i++]);
	}

	/* ── 阶段5：创建 unbound/ordered workqueue 属性模板 ─────────────────── */

	/* create default unbound and ordered wq attrs
	 * 为两种优先级（普通 + 高优先级）各创建一对 attrs 模板，
	 * 作为创建 unbound/ordered workqueue 时的默认参数。 */
	for (i = 0; i < NR_STD_WORKER_POOLS; i++) {
		struct workqueue_attrs *attrs;

		/* unbound workqueue 属性模板：不绑定特定 CPU，
		 * worker 可以在 wq_unbound_cpumask 内的任意 CPU 上运行。 */
		BUG_ON(!(attrs = alloc_workqueue_attrs()));
		attrs->nice = std_nice[i];
		unbound_std_wq_attrs[i] = attrs;

		/*
		 * An ordered wq should have only one pwq as ordering is
		 * guaranteed by max_active which is enforced by pwqs.
		 *
		 * ordered workqueue 属性模板：ordered=true 强制 max_active=1，
		 * 确保同一时刻只有一个 work 在执行，实现严格串行顺序。
		 */
		BUG_ON(!(attrs = alloc_workqueue_attrs()));
		attrs->nice = std_nice[i];
		attrs->ordered = true;
		ordered_wq_attrs[i] = attrs;
	}

	/* ── 阶段6：创建系统内置 workqueue ──────────────────────────────────── */

	/* 创建 12 个系统级 workqueue，供内核各子系统直接使用。
	 * 此时 worker 线程尚未创建，work 入队后会排队等待 workqueue_init() 后执行。
	 *
	 * WQ_PERCPU：每个 CPU 独立的 worker pool，work 在提交时的 CPU 上执行
	 * WQ_UNBOUND：不绑定 CPU，worker 可迁移，适合长时间运行的 work
	 * WQ_HIGHPRI：使用高优先级 worker pool（nice=-20）
	 * WQ_FREEZABLE：系统 suspend 时冻结，不执行新 work
	 * WQ_POWER_EFFICIENT：省电模式，nohz_full 时自动变为 WQ_UNBOUND
	 * WQ_BH：在 softirq 上下文执行，不能睡眠，延迟极低
	 * __WQ_DEPRECATED：已弃用，保留是为了兼容旧代码 */
	system_wq = alloc_workqueue("events", WQ_PERCPU | __WQ_DEPRECATED, 0);
	system_percpu_wq = alloc_workqueue("events", WQ_PERCPU, 0);
	/* 高优先级 per-CPU workqueue，网络、存储等延迟敏感路径使用。 */
	system_highpri_wq = alloc_workqueue("events_highpri",
					    WQ_HIGHPRI | WQ_PERCPU, 0);
	/* 长时间运行的 work 应提交到此 wq，避免占用普通 wq 的 worker 导致饥饿。 */
	system_long_wq = alloc_workqueue("events_long", WQ_PERCPU, 0);
	system_unbound_wq = alloc_workqueue("events_unbound", WQ_UNBOUND | __WQ_DEPRECATED, WQ_MAX_ACTIVE);
	/* system_dfl_wq 是 system_unbound_wq 的继任者，新代码应使用此 wq。 */
	system_dfl_wq = alloc_workqueue("events_unbound", WQ_UNBOUND, WQ_MAX_ACTIVE);
	system_freezable_wq = alloc_workqueue("events_freezable",
					      WQ_FREEZABLE | WQ_PERCPU, 0);
	system_power_efficient_wq = alloc_workqueue("events_power_efficient",
					      WQ_POWER_EFFICIENT | WQ_PERCPU, 0);
	system_freezable_power_efficient_wq = alloc_workqueue("events_freezable_pwr_efficient",
					      WQ_FREEZABLE | WQ_POWER_EFFICIENT | WQ_PERCPU, 0);
	/* BH workqueue：在 softirq 上下文执行，替代传统 tasklet 的推荐方式。 */
	system_bh_wq = alloc_workqueue("events_bh", WQ_BH | WQ_PERCPU, 0);
	system_bh_highpri_wq = alloc_workqueue("events_bh_highpri",
					       WQ_BH | WQ_HIGHPRI | WQ_PERCPU, 0);
	system_dfl_long_wq = alloc_workqueue("events_dfl_long", WQ_UNBOUND, WQ_MAX_ACTIVE);
	BUG_ON(!system_wq || !system_percpu_wq|| !system_highpri_wq || !system_long_wq ||
	       !system_unbound_wq || !system_freezable_wq || !system_dfl_wq ||
	       !system_power_efficient_wq ||
	       !system_freezable_power_efficient_wq ||
	       !system_bh_wq || !system_bh_highpri_wq || !system_dfl_long_wq);
}

/* wq_cpu_intensive_thresh_init() 根据调度 tick/CPU 能力计算未被启动参数覆盖的默认阈值。 */
static void __init wq_cpu_intensive_thresh_init(void)
{
	unsigned long thresh;
	unsigned long bogo;

	pwq_release_worker = kthread_run_worker(0, "pool_workqueue_release");
	BUG_ON(IS_ERR(pwq_release_worker));

	/* if the user set it to a specific value, keep it */
	if (wq_cpu_intensive_thresh_us != ULONG_MAX)
		return;

	/*
	 * The default of 10ms is derived from the fact that most modern (as of
	 * 2023) processors can do a lot in 10ms and that it's just below what
	 * most consider human-perceivable. However, the kernel also runs on a
	 * lot slower CPUs including microcontrollers where the threshold is way
	 * too low.
	 *
	 * Let's scale up the threshold upto 1 second if BogoMips is below 4000.
	 * This is by no means accurate but it doesn't have to be. The mechanism
	 * is still useful even when the threshold is fully scaled up. Also, as
	 * the reports would usually be applicable to everyone, some machines
	 * operating on longer thresholds won't significantly diminish their
	 * usefulness.
	 */
	thresh = 10 * USEC_PER_MSEC;

	/* see init/calibrate.c for lpj -> BogoMIPS calculation */
	bogo = max_t(unsigned long, loops_per_jiffy / 500000 * HZ, 1);
	if (bogo < 4000)
		thresh = min_t(unsigned long, thresh * 4000 / bogo, USEC_PER_SEC);

	pr_debug("wq_cpu_intensive_thresh: lpj=%lu BogoMIPS=%lu thresh_us=%lu\n",
		 loops_per_jiffy, bogo, thresh);

	wq_cpu_intensive_thresh_us = thresh;
}

/**
 * workqueue_init - bring workqueue subsystem fully online
 *
 * This is the second step of three-staged workqueue subsystem initialization
 * and invoked as soon as kthreads can be created and scheduled. Workqueues have
 * been created and work items queued on them, but there are no kworkers
 * executing the work items yet. Populate the worker pools with the initial
 * workers and enable future kworker creations.
 */
/*
 * workqueue_init() - 第二阶段：创建并唤醒各在线 CPU 标准 kworker、pwq release
 * kthread 和系统 rescuer，置 wq_online 后 work 开始真实执行，并启动 watchdog。
 * 无入参/返回；初始化失败为系统无法继续的 fatal 条件。
 */
void __init workqueue_init(void)
{
	struct workqueue_struct *wq;
	struct worker_pool *pool;
	int cpu, bkt;

	wq_cpu_intensive_thresh_init();

	mutex_lock(&wq_pool_mutex);

	/*
	 * Per-cpu pools created earlier could be missing node hint. Fix them
	 * up. Also, create a rescuer for workqueues that requested it.
	 */
	for_each_possible_cpu(cpu) {
		for_each_bh_worker_pool(pool, cpu)
			pool->node = cpu_to_node(cpu);
		for_each_cpu_worker_pool(pool, cpu)
			pool->node = cpu_to_node(cpu);
	}

	list_for_each_entry(wq, &workqueues, list) {
		WARN(init_rescuer(wq),
		     "workqueue: failed to create early rescuer for %s",
		     wq->name);
	}

	mutex_unlock(&wq_pool_mutex);

	/*
	 * Create the initial workers. A BH pool has one pseudo worker that
	 * represents the shared BH execution context and thus doesn't get
	 * affected by hotplug events. Create the BH pseudo workers for all
	 * possible CPUs here.
	 */
	for_each_possible_cpu(cpu)
		for_each_bh_worker_pool(pool, cpu)
			BUG_ON(!create_worker(pool));

	for_each_online_cpu(cpu) {
		for_each_cpu_worker_pool(pool, cpu) {
			pool->flags &= ~POOL_DISASSOCIATED;
			BUG_ON(!create_worker(pool));
		}
	}

	hash_for_each(unbound_pool_hash, bkt, pool, hash_node)
		BUG_ON(!create_worker(pool));

	wq_online = true;
	wq_watchdog_init();
}

/*
 * Initialize @pt by first initializing @pt->cpu_pod[] with pod IDs according to
 * @cpu_shares_pod(). Each subset of CPUs that share a pod is assigned a unique
 * and consecutive pod ID. The rest of @pt is initialized accordingly.
 */
/*
 * init_pod_type() - 按“两个 CPU 是否共享资源”的谓词构建一种 affinity pod 映射。
 * @pt 输出全局拓扑，@cpu_shares 只读判定；__init。分配各 pod mask/node 与 cpu 反查，
 * 初始化完成后运行期只读。
 */
static void __init init_pod_type(struct wq_pod_type *pt,
				 bool (*cpus_share_pod)(int, int))
{
	int cur, pre, cpu, pod;

	pt->nr_pods = 0;

	/* init @pt->cpu_pod[] according to @cpus_share_pod() */
	pt->cpu_pod = kzalloc_objs(pt->cpu_pod[0], nr_cpu_ids);
	BUG_ON(!pt->cpu_pod);

	for_each_possible_cpu(cur) {
		for_each_possible_cpu(pre) {
			if (pre >= cur) {
				pt->cpu_pod[cur] = pt->nr_pods++;
				break;
			}
			if (cpus_share_pod(cur, pre)) {
				pt->cpu_pod[cur] = pt->cpu_pod[pre];
				break;
			}
		}
	}

	/* init the rest to match @pt->cpu_pod[] */
	pt->pod_cpus = kzalloc_objs(pt->pod_cpus[0], pt->nr_pods);
	pt->pod_node = kzalloc_objs(pt->pod_node[0], pt->nr_pods);
	BUG_ON(!pt->pod_cpus || !pt->pod_node);

	for (pod = 0; pod < pt->nr_pods; pod++)
		BUG_ON(!zalloc_cpumask_var(&pt->pod_cpus[pod], GFP_KERNEL));

	for_each_possible_cpu(cpu) {
		cpumask_set_cpu(cpu, pt->pod_cpus[pt->cpu_pod[cpu]]);
		pt->pod_node[pt->cpu_pod[cpu]] = cpu_to_node(cpu);
	}
}

/* cpus_dont_share() 让每个 CPU 独立成 pod，仅用于 WQ_AFFN_CPU 拓扑构造。 */
static bool __init cpus_dont_share(int cpu0, int cpu1)
{
	return false;
}

/* cpus_share_smt() 判断两个 CPU 是否属于同一 SMT sibling 集。 */
static bool __init cpus_share_smt(int cpu0, int cpu1)
{
	return cpumask_test_cpu(cpu0, cpu_smt_mask(cpu1));
}

/* cpus_share_numa() 以 cpu_to_node 相等定义 NUMA affinity pod。 */
static bool __init cpus_share_numa(int cpu0, int cpu1)
{
	return cpu_to_node(cpu0) == cpu_to_node(cpu1);
}

/* Maps each CPU to its shard index within the LLC pod it belongs to */
static int cpu_shard_id[NR_CPUS] __initdata;

/**
 * llc_count_cores - count distinct cores (SMT groups) within an LLC pod
 * @pod_cpus:  the cpumask of CPUs in the LLC pod
 * @smt_pods:  the SMT pod type, used to identify sibling groups
 *
 * A core is represented by the lowest-numbered CPU in its SMT group. Returns
 * the number of distinct cores found in @pod_cpus.
 */
/* llc_count_cores() 统计 LLC pod 中物理 core，并可输出每 CPU 的临时 core id。 */
static int __init llc_count_cores(const struct cpumask *pod_cpus,
				  struct wq_pod_type *smt_pods)
{
	const struct cpumask *sibling_cpus;
	int nr_cores = 0, c;

	/*
	 * Count distinct cores by only counting the first CPU in each
	 * SMT sibling group.
	 */
	for_each_cpu(c, pod_cpus) {
		sibling_cpus = smt_pods->pod_cpus[smt_pods->cpu_pod[c]];
		if (cpumask_first(sibling_cpus) == c)
			nr_cores++;
	}

	return nr_cores;
}

/*
 * llc_shard_size - number of cores in a given shard
 *
 * Cores are spread as evenly as possible. The first @nr_large_shards shards are
 * "large shards" with (cores_per_shard + 1) cores; the rest are "default
 * shards" with cores_per_shard cores.
 */
/* llc_shard_size() 返回指定 shard 的 core 数：前 nr_large 个比基础值多一。 */
static int __init llc_shard_size(int shard_id, int cores_per_shard, int nr_large_shards)
{
	/* The first @nr_large_shards shards are large shards */
	if (shard_id < nr_large_shards)
		return cores_per_shard + 1;

	/* The remaining shards are default shards */
	return cores_per_shard;
}

/*
 * llc_calc_shard_layout - compute the shard layout for an LLC pod
 * @nr_cores:  number of distinct cores in the LLC pod
 *
 * Chooses the number of shards that keeps average shard size closest to
 * wq_cache_shard_size. Returns a struct describing the total number of shards,
 * the base size of each, and how many are large shards.
 */
/* llc_calc_shard_layout() 按 wq_cache_shard_size 均衡切分 core，返回大小差至多一的布局。 */
static struct llc_shard_layout __init llc_calc_shard_layout(int nr_cores)
{
	struct llc_shard_layout layout;

	/* Ensure at least one shard; pick the count closest to the target size */
	layout.nr_shards = max(1, DIV_ROUND_CLOSEST(nr_cores, wq_cache_shard_size));
	layout.cores_per_shard = nr_cores / layout.nr_shards;
	layout.nr_large_shards = nr_cores % layout.nr_shards;

	return layout;
}

/*
 * llc_shard_is_full - check whether a shard has reached its core capacity
 * @cores_in_shard: number of cores already assigned to this shard
 * @shard_id:       index of the shard being checked
 * @layout:         the shard layout computed by llc_calc_shard_layout()
 *
 * Returns true if @cores_in_shard equals the expected size for @shard_id.
 */
/* llc_shard_is_full() 用布局判断当前 shard 是否已达到其大/默认目标容量。 */
static bool __init llc_shard_is_full(int cores_in_shard, int shard_id,
				     const struct llc_shard_layout *layout)
{
	return cores_in_shard == llc_shard_size(shard_id, layout->cores_per_shard,
						layout->nr_large_shards);
}

/**
 * llc_populate_cpu_shard_id - populate cpu_shard_id[] for each CPU in an LLC pod
 * @pod_cpus:  the cpumask of CPUs in the LLC pod
 * @smt_pods:  the SMT pod type, used to identify sibling groups
 * @nr_cores:  number of distinct cores in @pod_cpus (from llc_count_cores())
 *
 * Walks @pod_cpus in order. At each SMT group leader, advances to the next
 * shard once the current shard is full. Results are written to cpu_shard_id[].
 */
/* llc_populate_cpu_shard_id() 保持 SMT siblings 同 shard，按 core 填全局 cpu_shard_id。 */
static void __init llc_populate_cpu_shard_id(const struct cpumask *pod_cpus,
					     struct wq_pod_type *smt_pods,
					     int nr_cores)
{
	struct llc_shard_layout layout = llc_calc_shard_layout(nr_cores);
	const struct cpumask *sibling_cpus;
	/* Count the number of cores in the current shard_id */
	int cores_in_shard = 0;
	unsigned int leader;
	/* This is a cursor for the shards. Go from zero to nr_shards - 1*/
	int shard_id = 0;
	int c;

	/* Iterate at every CPU for a given LLC pod, and assign it a shard */
	for_each_cpu(c, pod_cpus) {
		sibling_cpus = smt_pods->pod_cpus[smt_pods->cpu_pod[c]];
		if (cpumask_first(sibling_cpus) == c) {
			/* This is the CPU leader for the siblings */
			if (llc_shard_is_full(cores_in_shard, shard_id, &layout)) {
				shard_id++;
				cores_in_shard = 0;
			}
			cores_in_shard++;
			cpu_shard_id[c] = shard_id;
		} else {
			/*
			 * The siblings' shard MUST be the same as the leader.
			 * never split threads in the same core.
			 */
			leader = cpumask_first(sibling_cpus);

			/*
			 * This check silences a Warray-bounds warning on UP
			 * configs where NR_CPUS=1 makes cpu_shard_id[]
			 * a single-element array, and the compiler can't
			 * prove the index is always 0.
			 */
			if (WARN_ON_ONCE(leader >= nr_cpu_ids))
				continue;
			cpu_shard_id[c] = cpu_shard_id[leader];
		}
	}

	WARN_ON_ONCE(shard_id != (layout.nr_shards - 1));
}

/**
 * precompute_cache_shard_ids - assign each CPU its shard index within its LLC
 *
 * Iterates over all LLC pods. For each pod, counts distinct cores then assigns
 * shard indices to all CPUs in the pod. Must be called after WQ_AFFN_CACHE and
 * WQ_AFFN_SMT have been initialized.
 */
/* precompute_cache_shard_ids() 遍历 LLC pod，预计算所有可能 CPU 的 cache shard id。 */
static void __init precompute_cache_shard_ids(void)
{
	struct wq_pod_type *llc_pods = &wq_pod_types[WQ_AFFN_CACHE];
	struct wq_pod_type *smt_pods = &wq_pod_types[WQ_AFFN_SMT];
	const struct cpumask *cpus_sharing_llc;
	int nr_cores;
	int pod;

	if (!wq_cache_shard_size) {
		pr_warn("workqueue: cache_shard_size must be > 0, setting to 1\n");
		wq_cache_shard_size = 1;
	}

	for (pod = 0; pod < llc_pods->nr_pods; pod++) {
		cpus_sharing_llc = llc_pods->pod_cpus[pod];

		/* Number of cores in this given LLC */
		nr_cores = llc_count_cores(cpus_sharing_llc, smt_pods);
		llc_populate_cpu_shard_id(cpus_sharing_llc, smt_pods, nr_cores);
	}
}

/*
 * cpus_share_cache_shard - test whether two CPUs belong to the same cache shard
 *
 * Two CPUs share a cache shard if they are in the same LLC and have the same
 * shard index. Used as the pod affinity callback for WQ_AFFN_CACHE_SHARD.
 */
/* cpus_share_cache_shard() 比较预计算 shard id，供通用 init_pod_type() 分组。 */
static bool __init cpus_share_cache_shard(int cpu0, int cpu1)
{
	if (!cpus_share_cache(cpu0, cpu1))
		return false;

	return cpu_shard_id[cpu0] == cpu_shard_id[cpu1];
}

/**
 * workqueue_init_topology - initialize CPU pods for unbound workqueues
 *
 * This is the third step of three-staged workqueue subsystem initialization and
 * invoked after SMP and topology information are fully initialized. It
 * initializes the unbound CPU pods accordingly.
 */
/*
 * workqueue_init_topology() - 第三阶段：CPU 拓扑可用后建立 CPU/SMT/cache-shard/cache/
 * NUMA/system 各 pod type，发布 wq_topo_initialized，并重算所有 unbound wq 映射。
 * 无入参/返回；之后 queue_work() 才能按最终 locality scope 选择共享 pool。
 */
void __init workqueue_init_topology(void)
{
	struct workqueue_struct *wq;
	int cpu;

	init_pod_type(&wq_pod_types[WQ_AFFN_CPU], cpus_dont_share);
	init_pod_type(&wq_pod_types[WQ_AFFN_SMT], cpus_share_smt);
	init_pod_type(&wq_pod_types[WQ_AFFN_CACHE], cpus_share_cache);
	precompute_cache_shard_ids();
	init_pod_type(&wq_pod_types[WQ_AFFN_CACHE_SHARD], cpus_share_cache_shard);
	init_pod_type(&wq_pod_types[WQ_AFFN_NUMA], cpus_share_numa);

	wq_topo_initialized = true;

	mutex_lock(&wq_pool_mutex);

	/*
	 * Workqueues allocated earlier would have all CPUs sharing the default
	 * worker pool. Explicitly call unbound_wq_update_pwq() on all workqueue
	 * and CPU combinations to apply per-pod sharing.
	 */
	list_for_each_entry(wq, &workqueues, list) {
		for_each_online_cpu(cpu)
			unbound_wq_update_pwq(wq, cpu);
		if (wq->flags & WQ_UNBOUND) {
			mutex_lock(&wq->mutex);
			wq_update_node_max_active(wq, -1);
			mutex_unlock(&wq->mutex);
		}
	}

	mutex_unlock(&wq_pool_mutex);
}

/* __warn_flushing_systemwide_wq() 为 flush 系统全局 wq 的高风险用法发一次告警。 */
void __warn_flushing_systemwide_wq(void)
{
	pr_warn("WARNING: Flushing system-wide workqueues will be prohibited in near future.\n");
	dump_stack();
}
EXPORT_SYMBOL(__warn_flushing_systemwide_wq);

/*
 * workqueue_unbound_cpus_setup() - 解析启动参数 CPU list 到 __initdata cmdline mask。
 * 成功返回 1 表示参数已处理，解析失败返回 0/告警；最终由 early init 合并约束。
 */
static int __init workqueue_unbound_cpus_setup(char *str)
{
	if (cpulist_parse(str, &wq_cmdline_cpumask) < 0) {
		cpumask_clear(&wq_cmdline_cpumask);
		pr_warn("workqueue.unbound_cpus: incorrect CPU range, using default\n");
	}

	return 1;
}
__setup("workqueue.unbound_cpus=", workqueue_unbound_cpus_setup);
