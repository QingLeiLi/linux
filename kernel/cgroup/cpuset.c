// SPDX-License-Identifier: GPL-2.0
/*
 * cpuset 控制器学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5 Codex（2026-07-30）。
 *
 * 本文件把 cgroup 层级中的 CPU 与 NUMA 内存节点配置，落实为任务可运行 CPU、
 * 可分配内存节点、调度域和 housekeeping CPU 集合。它负责 cpuset v2 的通用
 * 实现并调用 cpuset-v1 helper 兼容旧层级；调度器如何构造 sched_domain、页分配器
 * 如何扫描 zonelist，以及内存迁移的底层页表操作不在本文件实现。
 *
 * 主调用链：
 *   控制文件写入
 *     -> cpuset_write_resmask()/cpuset_partition_write()
 *     -> 构造 trial cpuset 并 validate_change()
 *     -> update_{cpu,node}mask()/update_prstate()
 *     -> 在 callback_lock 下提交短小的可见状态
 *     -> 向后代传播 effective mask 并更新任务
 *     -> 必要时重建 sched_domain、更新 housekeeping。
 *
 *   cgroup 迁移/创建
 *     -> cpuset_can_attach()/cpuset_can_fork() 预留迁移条件
 *     -> cpuset_attach()/cpuset_fork() 更新任务亲和性和 mempolicy
 *     -> 异步迁移 mm 页面，最后解除 attach_in_progress。
 *
 *   CPU/内存热插拔
 *     -> cpuset_handle_hotplug()
 *     -> 先同步 top_cpuset，再自顶向下修复后代 effective mask
 *     -> 使失去资源的 partition 失效或重新生效
 *     -> 同步任务、调度域和 housekeeping。
 *
 * 核心对象与不变量：
 *   struct cpuset 是 css 的宿主对象。cpus_allowed/mems_allowed 保存用户请求，
 *   effective_cpus/effective_mems 保存层级、在线状态和分区分配共同裁剪后的结果；
 *   exclusive_cpus 是用户请求的独占集合，effective_xcpus 是实际获批集合。
 *   partition root 从父分区“取走”CPU，所以同一 CPU 不能同时属于互相冲突的
 *   partition；有任务的普通 cpuset 必须始终能得到可运行 CPU 和可用内存节点。
 *
 * 并发模型：
 *   cpuset_top_mutex 串行化可能进入 housekeeping_update() 的控制面事务；
 *   cpu hotplug lock 固定 cpu_active_mask；cpuset_mutex 固定层级和成员关系；
 *   callback_lock 让调度器、页分配器等短读路径看到成组一致的 mask/flag；
 *   task 的 mems_allowed 与 mempolicy 由 task_lock 和 mems_allowed_seq 配对保护。
 *   RCU 只稳定 css 层级遍历和对象存期；跨越可睡眠调用前还必须 css_tryget_online()。
 *
 * 方案权衡：
 *   用户请求与 effective 值分离，使热插拔不必改写 v2 配置，并允许资源恢复后自动
 *   重新生效；代价是每次上游资源变化都要做层级传播，并维护 partition、调度域与
 *   housekeeping 三套派生状态。trial 副本把可能失败的校验放在发布前，真正提交时
 *   只持有短时自旋锁，从而兼顾可回滚性与调度/分配快路径的低开销读取。
 */
/*
 *  kernel/cpuset.c
 *
 *  Processor and Memory placement constraints for sets of tasks.
 *
 *  Copyright (C) 2003 BULL SA.
 *  Copyright (C) 2004-2007 Silicon Graphics, Inc.
 *  Copyright (C) 2006 Google, Inc
 *
 *  Portions derived from Patrick Mochel's sysfs code.
 *  sysfs is Copyright (c) 2001-3 Patrick Mochel
 *
 *  2003-10-10 Written by Simon Derr.
 *  2003-10-22 Updates by Stephen Hemminger.
 *  2004 May-July Rework by Paul Jackson.
 *  2006 Rework by Paul Menage to use generic cgroups
 *  2008 Rework of the scheduler domains and CPU hotplug handling
 *       by Max Krasnyansky
 */
#include "cpuset-internal.h"

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mempolicy.h>
#include <linux/mm.h>
#include <linux/memory.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/deadline.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/security.h>
#include <linux/oom.h>
#include <linux/sched/isolation.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/task_work.h>

DEFINE_STATIC_KEY_FALSE(cpusets_pre_enable_key);
DEFINE_STATIC_KEY_FALSE(cpusets_enabled_key);

/*
 * 两个 static key 把“系统从未启用 cpuset”变成近乎零成本的静态分支。
 * pre_enable_key 覆盖启用过程的过渡窗口，enabled_key 表示控制器已经生效；
 * 更新由 cgroup 生命周期在 CPU hotplug 锁协议下完成，热路径只做 jump-label 判断。
 */

/*
 * There could be abnormal cpuset configurations for cpu or memory
 * node binding, add this key to provide a quick low-cost judgment
 * of the situation.
 */
/*
 * CPU 或内存节点绑定可能出现异常 cpuset 配置；该静态键提供一次快速、低成本的
 * 状态判断。只有异常配置曾被观察到时才启用慢路径，正常系统不承担持续分支开销。
 */
DEFINE_STATIC_KEY_FALSE(cpusets_insane_config_key);

/*
 * 一旦发现仅含 movable 节点的异常 mems 配置，此 key 永久开启相应慢路径保护。
 * 这种单向切换避免每次内存分配都重新扫描配置；代价是异常曾出现过后保守检查常驻。
 */

static const char * const perr_strings[] = {
	[PERR_INVCPUS]   = "Invalid cpu list in cpuset.cpus.exclusive",
	[PERR_INVPARENT] = "Parent is an invalid partition root",
	[PERR_NOTPART]   = "Parent is not a partition root",
	[PERR_NOTEXCL]   = "Cpu list in cpuset.cpus not exclusive",
	[PERR_NOCPUS]    = "Parent unable to distribute cpu downstream",
	[PERR_HOTPLUG]   = "No cpu available due to hotplug",
	[PERR_CPUSEMPTY] = "cpuset.cpus and cpuset.cpus.exclusive are empty",
	[PERR_HKEEPING]  = "partition config conflicts with housekeeping setup",
	[PERR_ACCESS]    = "Enable partition not permitted",
	[PERR_REMOTE]    = "Have remote partition underneath",
};

/*
 * perr_strings 把 prs_err 枚举映射成 cpuset.cpus.partition 的用户可读原因。
 * 下标来自受 READ_ONCE/WRITE_ONCE 访问的 prs_err；NULL 表示当前没有附加原因。
 */

/*
 * CPUSET Locking Convention
 * -------------------------
 *
 * Below are the four global/local locks guarding cpuset structures in lock
 * acquisition order:
 *  - cpuset_top_mutex
 *  - cpu_hotplug_lock (cpus_read_lock/cpus_write_lock)
 *  - cpuset_mutex
 *  - callback_lock (raw spinlock)
 *
 * As cpuset will now indirectly flush a number of different workqueues in
 * housekeeping_update() to update housekeeping cpumasks when the set of
 * isolated CPUs is going to be changed, it may be vulnerable to deadlock
 * if we hold cpus_read_lock while calling into housekeeping_update().
 *
 * The first cpuset_top_mutex will be held except when calling into
 * cpuset_handle_hotplug() from the CPU hotplug code where cpus_write_lock
 * and cpuset_mutex will be held instead. The main purpose of this mutex
 * is to prevent regular cpuset control file write actions from interfering
 * with the call to housekeeping_update(), though CPU hotplug operation can
 * still happen in parallel. This mutex also provides protection for some
 * internal variables.
 *
 * A task must hold all the remaining three locks to modify externally visible
 * or used fields of cpusets, though some of the internally used cpuset fields
 * and internal variables can be modified without holding callback_lock. If only
 * reliable read access of the externally used fields are needed, a task can
 * hold either cpuset_mutex or callback_lock which are exposed to other
 * external subsystems.
 *
 * If a task holds cpu_hotplug_lock and cpuset_mutex, it blocks others,
 * ensuring that it is the only task able to also acquire callback_lock and
 * be able to modify cpusets.  It can perform various checks on the cpuset
 * structure first, knowing nothing will change. It can also allocate memory
 * without holding callback_lock. While it is performing these checks, various
 * callback routines can briefly acquire callback_lock to query cpusets.  Once
 * it is ready to make the changes, it takes callback_lock, blocking everyone
 * else.
 *
 * Calls to the kernel memory allocator cannot be made while holding
 * callback_lock which is a spinlock, as the memory allocator may sleep or
 * call back into cpuset code and acquire callback_lock.
 *
 * Now, the task_struct fields mems_allowed and mempolicy may be changed
 * by other task, we use alloc_lock in the task_struct fields to protect
 * them.
 *
 * The cpuset_common_seq_show() handlers only hold callback_lock across
 * small pieces of code, such as when reading out possibly multi-word
 * cpumasks and nodemasks.
 */
/*
 * cpuset 锁协议与 housekeeping 更新的关系：
 *
 * 修改 cpuset 外部可见状态时，锁顺序是 cpuset_top_mutex → CPU hotplug lock
 * → cpuset_mutex → callback_lock。callback_lock 是 raw spinlock，持有期间既不能
 * 睡眠分配，也不能调用可能回入 cpuset 的路径。
 *
 * isolated partition 变化后，housekeeping_update() 会 synchronize_rcu()、
 * flush 多个 workqueue 并迁移 kthread/timer 状态，明显可能睡眠。若仍持有
 * cpus_read_lock 或 cpuset_mutex 调用，下游 worker/热插拔路径可能反向等待这些
 * 锁而死锁。因此提交路径只保留最外层 cpuset_top_mutex 来串行化普通控制文件
 * 写者，先释放 hotplug/cpuset 内层锁，再进入 housekeeping 更新。
 *
 * cpuset_top_mutex 不冻结 CPU hotplug；热插拔路径使用 cpus_write_lock 加
 * cpuset_mutex 维持自身一致性。读取外部字段可持 cpuset_mutex 或 callback_lock，
 * 修改则遵循完整协议。task_struct 的 mems_allowed/mempolicy 另由 task 的
 * alloc_lock 保护，不能把 cpuset 全局锁误当作任务字段的唯一保护。
 */

static DEFINE_MUTEX(cpuset_top_mutex);
static DEFINE_MUTEX(cpuset_mutex);

/*
 * cpuset_top_mutex 保护跨越内层解锁的完整控制面事务；cpuset_mutex 保护层级配置、
 * 成员迁移计数和需要睡眠的传播过程。二者生命周期覆盖整个系统，不隶属某个 css。
 */

/*
 * File level internal variables below follow one of the following exclusion
 * rules.
 *
 * RWCS: Read/write-able by holding either cpus_write_lock (and optionally
 *	 cpuset_mutex) or both cpus_read_lock and cpuset_mutex.
 *
 * CSCB: Readable by holding either cpuset_mutex or callback_lock. Writable
 *	 by holding both cpuset_mutex and callback_lock.
 *
 * T:	 Read/write-able by holding the cpuset_top_mutex.
 */
/*
 * 下方文件级变量按三套互斥规则访问：RWCS 的写侧持 CPU hotplug 写锁（可再持
 * cpuset_mutex），或以 CPU 读锁与 cpuset_mutex 的组合串行化；CSCB 的读侧可持
 * cpuset_mutex 或 callback_lock，写侧必须同时持有二者；T 类状态完全由
 * cpuset_top_mutex 串行化。后续声明末尾的缩写就是对应的锁契约。
 */

/*
 * For local partitions, update to subpartitions_cpus & isolated_cpus is done
 * in update_parent_effective_cpumask(). For remote partitions, it is done in
 * the remote_partition_*() and remote_cpus_update() helpers.
 */
/*
 * 本地 partition 通过 update_parent_effective_cpumask() 更新
 * subpartitions_cpus 与 isolated_cpus；远程 partition 则由
 * remote_partition_*() 和 remote_cpus_update() 维护。两条路径必须汇合到同一
 * 全局不变量，避免 top_cpuset 把已经分配给 partition 的 CPU 再交给普通任务。
 */
/*
 * Exclusive CPUs distributed out to local or remote sub-partitions of
 * top_cpuset
 */
/*
 * subpartitions_cpus 是 top_cpuset 已分发给本地或远程子 partition 的独占 CPU
 * 并集；它是派生状态而非用户输入，更新时必须与各 partition 的 effective_xcpus
 * 保持一致。
 */
static cpumask_var_t	subpartitions_cpus;	/* RWCS */
/* RWCS 表示该掩码遵循上方 CPU hotplug 锁与 cpuset_mutex 的组合访问规则。 */

/*
 * Exclusive CPUs in isolated partitions (shown in cpuset.cpus.isolated)
 */
/*
 * isolated_cpus 记录处于 isolated partition 的独占 CPU，并作为
 * cpuset.cpus.isolated 的可见结果；写入必须与 partition 状态转换同步提交。
 */
static cpumask_var_t	isolated_cpus;		/* CSCB */
/* CSCB 表示读者持 cpuset_mutex 或 callback_lock，写者同时持有二者。 */

/*
 * Set if housekeeping cpumasks are to be updated.
 */
/*
 * update_housekeeping 是待提交标志：isolated_cpus 改变后置位，稍后的工作项据此
 * 更新 housekeeping 掩码，把可能睡眠或 flush workqueue 的操作移出内层锁区。
 */
static bool		update_housekeeping;	/* RWCS */
/* RWCS 表示该标志与 partition CPU 派生状态使用同一写侧串行化协议。 */

/*
 * Copy of isolated_cpus to be passed to housekeeping_update()
 */
/*
 * isolated_hk_cpus 是交给 housekeeping_update() 的 isolated_cpus 稳定副本；
 * cpuset_top_mutex 使复制、消费与再次排队之间不会丢失更新。
 */
static cpumask_var_t	isolated_hk_cpus;	/* T */
/* T 表示该跨工作项快照只由 cpuset_top_mutex 保护。 */

/*
 * 四个全局派生状态共同描述 top_cpuset 被子分区取走的 CPU：
 *   subpartitions_cpus  所有本地/远程子分区的独占 CPU 并集；
 *   isolated_cpus      其中禁止调度负载均衡的 CPU；
 *   update_housekeeping 表示 isolated 集合尚未传播给 HK 子系统；
 *   isolated_hk_cpus   在释放内层锁后仍可安全使用的稳定提交快照。
 * 它们不是用户配置源，必须随 partition_xcpus_add/del 原子地维护。
 */

/*
 * A flag to force sched domain rebuild at the end of an operation.
 * It can be set in
 *  - update_partition_sd_lb()
 *  - update_cpumasks_hier()
 *  - cpuset_update_flag()
 *  - cpuset_hotplug_update_tasks()
 *  - cpuset_handle_hotplug()
 *
 * Protected by cpuset_mutex (with cpus_read_lock held) or cpus_write_lock.
 *
 * Note that update_relax_domain_level() in cpuset-v1.c can still call
 * rebuild_sched_domains_locked() directly without using this flag.
 */
/*
 * force_sd_rebuild 是调度域布局的事务脏位。列出的更新入口只负责置位，外层提交点
 * 再合并执行一次 rebuild；它由持 CPU 读锁的 cpuset_mutex 路径或 CPU 写锁路径
 * 保护。v1 的 update_relax_domain_level() 是例外，会直接重建而不经过此标志。
 */
static bool force_sd_rebuild;			/* RWCS */

/*
 * force_sd_rebuild 是事务内的“脏位”：多个层级变化可合并成一次调度域重建。
 * 重建函数消费并清零它；CPU 热插拔路径也可在持有写侧 hotplug lock 时设置。
 */

/*
 * Partition root states:
 *
 *   0 - member (not a partition root)
 *   1 - partition root
 *   2 - partition root without load balancing (isolated)
 *  -1 - invalid partition root
 *  -2 - invalid isolated partition root
 *
 *  There are 2 types of partitions - local or remote. Local partitions are
 *  those whose parents are partition root themselves. Setting of
 *  cpuset.cpus.exclusive are optional in setting up local partitions.
 *  Remote partitions are those whose parents are not partition roots. Passing
 *  down exclusive CPUs by setting cpuset.cpus.exclusive along its ancestor
 *  nodes are mandatory in creating a remote partition.
 *
 *  For simplicity, a local partition can be created under a local or remote
 *  partition but a remote partition cannot have any partition root in its
 *  ancestor chain except the cgroup root.
 *
 *  A valid partition can be formed by setting exclusive_cpus or cpus_allowed
 *  if exclusive_cpus is not set. In the case of partition with empty
 *  exclusive_cpus, all the conflicting exclusive CPUs specified in the
 *  following cpumasks of sibling cpusets will be removed from its
 *  cpus_allowed in determining its effective_xcpus.
 *  - effective_xcpus
 *  - exclusive_cpus
 *
 *  The "cpuset.cpus.exclusive" control file should be used for setting up
 *  partition if the users want to get as many CPUs as possible.
 */
/*
 * partition_root_state 用 0 表示普通成员，用 1/2 表示普通或隔离 partition root，
 * 对应负值表示同类 partition 当前无效。父节点本身是 partition root 时为本地
 * partition，否则必须沿祖先逐级下传 cpuset.cpus.exclusive 才能形成远程
 * partition；远程 partition 的祖先链除根外不能再含 partition root。
 *
 * 未显式配置 exclusive_cpus 时，有效独占集合退化为 cpus_allowed，再扣除兄弟
 * 已声明或已获批的独占 CPU。希望尽可能稳定取得 CPU 的用户应显式写
 * cpuset.cpus.exclusive，使所有权意图在层级传播前就可校验。
 */
#define PRS_MEMBER		0
#define PRS_ROOT		1
#define PRS_ISOLATED		2
#define PRS_INVALID_ROOT	-1
#define PRS_INVALID_ISOLATED	-2

/*
 * Temporary cpumasks for working with partitions that are passed among
 * functions to avoid memory allocation in inner functions.
 */
/*
 * tmpmasks 把 partition 更新所需的临时 CPU 位图由外层一次分配并沿 helper 传递，
 * 从而避免深层锁区再次分配内存，也让同一事务复用一致的中间结果。
 */
struct tmpmasks {
	cpumask_var_t addmask, delmask;	/* For partition root */
	/* addmask/delmask 分别描述从子 partition 归还给父级、从父级划出的 CPU。 */
	cpumask_var_t new_cpus;		/* For update_cpumasks_hier() */
	/* new_cpus 保存层级传播时当前节点即将发布的 effective CPU 集合。 */
};

/*
 * tmpmasks 是一次 cpuset 更新事务独占的工作区，不发布给读者。
 * addmask/delmask 以父 cpuset 视角记录归还/取走的 CPU，new_cpus 承载层级传播的
 * 中间结果。由外层一次性分配并传入深层 helper，避免持 callback_lock 时分配内存。
 */

/*
 * inc_dl_tasks_cs()/dec_dl_tasks_cs() - 维护任务所属 cpuset 的 DL 任务计数。
 *
 * 调度器在 deadline 任务进入或离开 cpuset 时调用；@p 是调用期间稳定的借用任务。
 * 调用方已提供使 task_cs(p) 和计数更新安全的同步，本函数不加锁、不睡眠。
 * 无直接返回值；副作用是 nr_deadline_tasks 加一或减一，供调度域重建跳过空集合。
 */
void inc_dl_tasks_cs(struct task_struct *p)
{
	struct cpuset *cs = task_cs(p);

	cs->nr_deadline_tasks++;
}

/* DL 任务离开所属 cpuset 的计数；与 inc 路径配对，不转移 @p 所有权。 */
void dec_dl_tasks_cs(struct task_struct *p)
{
	struct cpuset *cs = task_cs(p);

	cs->nr_deadline_tasks--;
}

/*
 * 三个 partition 状态谓词只借用 @cs 并读取带符号状态：
 * 正数表示有效 root/isolated，负数保留期望类型但表示失效，零表示普通 member。
 * 它们不取得引用、不加锁；调用者必须用 cpuset_mutex、callback_lock 或事务上下文
 * 保证读值满足所需一致性。返回仅是瞬时分类，不延长对象生命周期。
 */
static inline bool is_partition_valid(const struct cpuset *cs)
{
	return cs->partition_root_state > 0;
}

static inline bool is_partition_invalid(const struct cpuset *cs)
{
	return cs->partition_root_state < 0;
}

static inline bool cs_is_member(const struct cpuset *cs)
{
	return cs->partition_root_state == PRS_MEMBER;
}

/*
 * Callers should hold callback_lock to modify partition_root_state.
 */
/*
 * make_partition_invalid() 只把有效状态取负，保留 root 与 isolated 的期望类型，
 * 便于资源恢复时原样转回。@cs 为借用输入输出对象，调用者必须持 callback_lock；
 * 已失效/member 时无动作，无返回值，也不负责设置 prs_err 或发送文件通知。
 */
static inline void make_partition_invalid(struct cpuset *cs)
{
	if (cs->partition_root_state > 0)
		cs->partition_root_state = -cs->partition_root_state;
}

/*
 * Send notification event of whenever partition_root_state changes.
 */
/*
 * notify_partition_change() 比较事务前后的 partition 状态并唤醒 kernfs poll 读者。
 * @old_prs 是进入事务时的值，@cs 为仍在线的借用对象。状态未变时无副作用；
 * 转为有效状态时用 WRITE_ONCE 清除旧错误，避免无锁 show 路径读到撕裂/陈旧原因。
 */
static inline void notify_partition_change(struct cpuset *cs, int old_prs)
{
	if (old_prs == cs->partition_root_state)
		return;
	cgroup_file_notify(&cs->partition_file);

	/* Reset prs_err if not invalid */
	/* 状态恢复为有效 partition 时，旧失败原因已失效，必须同步清为 PERR_NONE。 */
	if (is_partition_valid(cs))
		WRITE_ONCE(cs->prs_err, PERR_NONE);
}

/*
 * The top_cpuset is always synchronized to cpu_active_mask and we should avoid
 * using cpu_online_mask as much as possible. An active CPU is always an online
 * CPU, but not vice versa. cpu_active_mask and cpu_online_mask can differ
 * during hotplug operations. A CPU is marked active at the last stage of CPU
 * bringup (CPUHP_AP_ACTIVE). It is also the stage where cpuset hotplug code
 * will be called to update the sched domains so that the scheduler can move
 * a normal task to a newly active CPU or remove tasks away from a newly
 * inactivated CPU. The online bit is set much earlier in the CPU bringup
 * process and cleared much later in CPU teardown.
 *
 * If cpu_online_mask is used while a hotunplug operation is happening in
 * parallel, we may leave an offline CPU in cpu_allowed or some other masks.
 */
/*
 * top_cpuset 始终跟随 cpu_active_mask，而不是更早置位、更晚清除的
 * cpu_online_mask。CPU 只有到 CPUHP_AP_ACTIVE 阶段才真正可承载普通调度任务，
 * cpuset 也在该阶段重建 sched_domain；热拔插并发时若依据 online 位，可能把已经
 * 不可调度的 CPU 残留在 cpu_allowed 或其他派生掩码中。
 */
struct cpuset top_cpuset = {
	.flags = BIT(CS_CPU_EXCLUSIVE) |
		 BIT(CS_MEM_EXCLUSIVE) | BIT(CS_SCHED_LOAD_BALANCE),
	.partition_root_state = PRS_ROOT,
	.dl_bw_cpu = -1,
};

/*
 * top_cpuset 是静态根对象，不经过 css_alloc/free，生命周期覆盖整个系统。
 * 它天然是 CPU/内存独占且参与负载均衡的 partition root；启动和热插拔路径把其
 * effective 集合与 cpu_active_mask、N_MEMORY 同步，再扣除分给子 partition 的 CPU。
 * dl_bw_cpu=-1 表示尚未为迁移中的 deadline 带宽选择记账 CPU。
 */

/**
 * cpuset_lock - Acquire the global cpuset mutex
 *
 * This locks the global cpuset mutex to prevent modifications to cpuset
 * hierarchy and configurations. This helper is not enough to make modification.
 */
/*
 * cpuset_lock()/cpuset_unlock() 为外部子系统暴露 cpuset_mutex 的睡眠锁接口。
 * lock 可能睡眠，返回时层级配置不会被并发写者修改；unlock 结束该保证。
 * 这对不足以单独发布修改，写路径仍需 CPU hotplug lock 和 callback_lock。
 */
void cpuset_lock(void)
{
	mutex_lock(&cpuset_mutex);
}

/* 释放当前线程持有的 cpuset_mutex；必须与 cpuset_lock() 配对。 */
void cpuset_unlock(void)
{
	mutex_unlock(&cpuset_mutex);
}

/*
 * lockdep_assert_cpuset_lock_held() 仅在调试构建中验证当前线程已持 cpuset_mutex。
 * 无参数、无运行时状态副作用；违反约定会触发 lockdep 报告，不能替代真正加锁。
 */
void lockdep_assert_cpuset_lock_held(void)
{
	lockdep_assert_held(&cpuset_mutex);
}

/**
 * cpuset_full_lock - Acquire full protection for cpuset modification
 *
 * Takes both CPU hotplug read lock (cpus_read_lock()) and cpuset mutex
 * to safely modify cpuset data.
 */
/*
 * cpuset_full_lock()/cpuset_full_unlock() 获取/释放普通配置事务的完整睡眠锁组。
 * 顺序固定为 top mutex -> CPU hotplug read lock -> cpuset mutex，防止与热插拔及
 * housekeeping 更新形成锁反转。lock 可能睡眠；返回时可校验并修改层级，但在
 * 修改供外部短读路径使用的字段前仍须短暂取得 callback_lock。
 */
void cpuset_full_lock(void)
{
	mutex_lock(&cpuset_top_mutex);
	cpus_read_lock();
	mutex_lock(&cpuset_mutex);
}

/* 按逆序释放完整锁组；必须与 cpuset_full_lock() 在同一事务中配对。 */
void cpuset_full_unlock(void)
{
	mutex_unlock(&cpuset_mutex);
	cpus_read_unlock();
	mutex_unlock(&cpuset_top_mutex);
}

#ifdef CONFIG_LOCKDEP
/*
 * lockdep_is_cpuset_held() 是 lockdep 条件断言回调：持有 cpuset_mutex 或最外层
 * top mutex 都返回 true。它只描述当前线程锁状态，不获取锁、不提供数据一致性。
 */
bool lockdep_is_cpuset_held(void)
{
	return lockdep_is_held(&cpuset_mutex) ||
	       lockdep_is_held(&cpuset_top_mutex);
}
#endif

static DEFINE_SPINLOCK(callback_lock);

/*
 * callback_lock 是调度器、页分配器等不能长期睡眠的读侧与控制面提交点之间的
 * raw spinlock。它保护外部可见 mask/flag 的成组快照；持锁期间禁止睡眠和分配。
 */

/*
 * cpuset_callback_lock_irq()/unlock_irq() - 给外部调用者暴露禁中断锁对。
 *
 * 无参数、无直接返回值。lock 返回时本 CPU 中断关闭且 callback_lock 已持有；
 * unlock 恢复对应状态。调用者必须严格配对，且临界区内不能调用睡眠函数。
 */
void cpuset_callback_lock_irq(void)
{
	spin_lock_irq(&callback_lock);
}

/* 释放 callback_lock 并恢复对应中断状态。 */
void cpuset_callback_unlock_irq(void)
{
	spin_unlock_irq(&callback_lock);
}

static struct workqueue_struct *cpuset_migrate_mm_wq;

static DECLARE_WAIT_QUEUE_HEAD(cpuset_attach_wq);

/*
 * cpuset_migrate_mm_wq 是启动后创建、全局存活的有序工作队列，串行保持 mm 迁移
 * 的提交顺序；cpuset_attach_wq 让热插拔等待 attach_in_progress 归零，避免把任务
 * 正在迁入的 cpuset 同时裁剪为空。二者只保存同步状态，不拥有 cpuset 对象。
 */

/*
 * check_insane_mems_config() - 检测只包含 movable 节点的高风险内存约束。
 *
 * @nodes 是调用期间有效的借用 nodemask；调用者位于可睡眠的配置/热插拔路径。
 * 首次发现异常时开启全局 static key 并打印一次诊断；无直接返回值，不修改输入。
 */
static inline void check_insane_mems_config(nodemask_t *nodes)
{
	if (!cpusets_insane_config() &&
		movable_only_nodes(nodes)) {
		static_branch_enable_cpuslocked(&cpusets_insane_config_key);
		pr_info("Unsupported (movable nodes only) cpuset configuration detected (nmask=%*pbl)!\n"
			"Cpuset allocations might fail even with a lot of memory available.\n",
			nodemask_pr_args(nodes));
	}
}

/*
 * decrease cs->attach_in_progress.
 * wake_up cpuset_attach_wq if cs->attach_in_progress==0.
 */
/*
 * dec_attach_in_progress_locked() 在 cpuset_mutex 下完成迁移预留的提交/撤销。
 * @cs 是在线借用对象，计数必须大于零；减到零时唤醒热插拔等待者。
 * wake_up 可调度等待任务但本函数不转移对象所有权，无直接返回值。
 */
static inline void dec_attach_in_progress_locked(struct cpuset *cs)
{
	lockdep_assert_cpuset_lock_held();

	cs->attach_in_progress--;
	if (!cs->attach_in_progress)
		wake_up(&cpuset_attach_wq);
}

/*
 * dec_attach_in_progress() 是上述 helper 的可独立调用包装，内部获取 cpuset_mutex。
 * @cs 在调用期间须保持存活；函数可能睡眠，返回时本次 attach 预留已解除。
 */
static inline void dec_attach_in_progress(struct cpuset *cs)
{
	mutex_lock(&cpuset_mutex);
	dec_attach_in_progress_locked(cs);
	mutex_unlock(&cpuset_mutex);
}

/*
 * cpuset_v2() 判断当前控制器是否采用统一层级语义：未编译 v1 或挂载在 default
 * hierarchy 时返回 true。它只读取稳定的挂载配置，不加锁、不睡眠。
 */
static inline bool cpuset_v2(void)
{
	return !IS_ENABLED(CONFIG_CPUSETS_V1) ||
		cgroup_subsys_on_dfl(cpuset_cgrp_subsys);
}

/*
 * Cgroup v2 behavior is used on the "cpus" and "mems" control files when
 * on default hierarchy or when the cpuset_v2_mode flag is set by mounting
 * the v1 cpuset cgroup filesystem with the "cpuset_v2_mode" mount option.
 * With v2 behavior, "cpus" and "mems" are always what the users have
 * requested and won't be changed by hotplug events. Only the effective
 * cpus or mems will be affected.
 */
/*
 * is_in_v2_mode() 还把 v1 挂载的 cpuset_v2_mode 兼容选项计入判断。
 * 返回 true 时用户请求 mask 在热插拔时保持不变，只有 effective mask 被裁剪；
 * 返回 false 时 v1 路径会同步改写配置 mask。返回值是瞬时模式，不持有引用。
 */
static inline bool is_in_v2_mode(void)
{
	return cpuset_v2() ||
	      (cpuset_cgrp_subsys.root->flags & CGRP_ROOT_CPUSET_V2_MODE);
}

/**
 * partition_is_populated - check if partition has tasks
 * @cs: partition root to be checked
 * @excluded_child: a child cpuset to be excluded in task checking
 * Return: true if there are tasks, false otherwise
 *
 * @cs should be a valid partition root or going to become a partition root.
 * @excluded_child should be non-NULL when this cpuset is going to become a
 * partition itself.
 *
 * Note that a remote partition is not allowed underneath a valid local
 * or remote partition. So if a non-partition root child is populated,
 * the whole partition is considered populated.
 */
/*
 * partition_is_populated() - 判断一个 partition 域内是否存在任务或正在迁入的任务。
 *
 * @cs 是待检查的有效/候选 partition root；@excluded_child 可空，非空时把正要独立
 * 成 partition 的该子树排除。函数在 RCU 下自顶向下遍历，但遇到有效子 partition
 * 会跳过其整棵子树，因为任务资源责任已经在新的 partition 边界处截断。
 * 返回 true 表示不能让该 partition 的 effective CPU 变空；不修改任何对象。
 */
static inline bool partition_is_populated(struct cpuset *cs,
					  struct cpuset *excluded_child)
{
	struct cpuset *cp;
	struct cgroup_subsys_state *pos_css;

	/*
	 * We cannot call cs_is_populated(cs) directly, as
	 * nr_populated_domain_children may include populated
	 * csets from descendants that are partitions.
	 */
	/*
	 * 不能直接使用 cs_is_populated()：其 domain 子节点计数会把已经被有效
	 * partition 边界隔离的后代 cset 也算进来。这里显式遍历并在 partition root
	 * 处剪枝，得到“仍由当前 partition 承担资源责任”的真实 populated 状态。
	 */
	if (cgroup_has_tasks(cs->css.cgroup) ||
	    cs->attach_in_progress)
		return true;

	/*
	 * 当前节点本身为空后，再扫描仍属于本 partition 资源域的后代；有效子 partition
	 * 是新的责任边界，直接跳过其子树，普通 populated 后代则立即确认当前域非空。
	 */
	rcu_read_lock();
	cpuset_for_each_descendant_pre(cp, pos_css, cs) {
		if (cp == cs || cp == excluded_child)
			continue;

		if (is_partition_valid(cp)) {
			pos_css = css_rightmost_descendant(pos_css);
			continue;
		}

		if (cpuset_is_populated(cp)) {
			rcu_read_unlock();
			return true;
		}
	}
	rcu_read_unlock();
	return false;
}

/*
 * Return in pmask the portion of a task's cpusets's cpus_allowed that
 * are online and are capable of running the task.  If none are found,
 * walk up the cpuset hierarchy until we find one that does have some
 * appropriate cpus.
 *
 * One way or another, we guarantee to return some non-empty subset
 * of cpu_active_mask.
 *
 * Call with callback_lock or cpuset_mutex held.
 */
/*
 * guarantee_active_cpus() - 为任务求得必定非空且当前可运行的 CPU 集合。
 *
 * @tsk 是稳定借用任务；@pmask 是输入输出工作 mask。调用者持 cpuset_mutex 或
 * callback_lock，函数在 RCU 下从 task 所属 cpuset 向祖先回退，并同时裁剪任务
 * 架构可能 CPU 与 cpu_active_mask。正常返回保证 @pmask 非空；极端不一致时先
 * WARN 并退到 active mask。函数不改变任务亲和性，只计算候选集合。
 */
static void guarantee_active_cpus(struct task_struct *tsk,
				  struct cpumask *pmask)
{
	const struct cpumask *possible_mask = task_cpu_possible_mask(tsk);
	struct cpuset *cs;

	/* 先构造“任务架构可运行 CPU 与 active CPU”的上限，异常为空时保守退到 active。 */
	if (WARN_ON(!cpumask_and(pmask, possible_mask, cpu_active_mask)))
		cpumask_copy(pmask, cpu_active_mask);

	rcu_read_lock();
	cs = task_cs(tsk);

	/* 沿父链寻找首个与候选上限有交集的 effective 集合，根节点保证搜索终止。 */
	while (!cpumask_intersects(cs->effective_cpus, pmask))
		cs = parent_cs(cs);

	cpumask_and(pmask, pmask, cs->effective_cpus);
	rcu_read_unlock();
}

/*
 * Return in *pmask the portion of a cpusets's mems_allowed that
 * are online, with memory.  If none are online with memory, walk
 * up the cpuset hierarchy until we find one that does have some
 * online mems.  The top cpuset always has some mems online.
 *
 * One way or another, we guarantee to return some non-empty subset
 * of node_states[N_MEMORY].
 *
 * Call with callback_lock or cpuset_mutex held.
 */
/*
 * guarantee_online_mems() - 为 cpuset 求得非空的在线有内存节点集合。
 *
 * @cs 是起始 cpuset 的借用指针；@pmask 为输出 nodemask。调用者持 callback_lock
 * 或 cpuset_mutex，函数逐级向父节点回退；top_cpuset 保证终止并提供 N_MEMORY。
 * 无直接返回值，不修改 cpuset，只把最终集合写入 @pmask。
 */
static void guarantee_online_mems(struct cpuset *cs, nodemask_t *pmask)
{
	/* 每级同时裁剪 N_MEMORY；若为空就继承父级，直到 top_cpuset 提供非空集合。 */
	while (!nodes_and(*pmask, cs->effective_mems, node_states[N_MEMORY]))
		cs = parent_cs(cs);
}

/**
 * alloc_cpumasks - Allocate an array of cpumask variables
 * @pmasks: Pointer to array of cpumask_var_t pointers
 * @size: Number of cpumasks to allocate
 * Return: 0 if successful, -ENOMEM otherwise.
 *
 * Allocates @size cpumasks and initializes them to empty. Returns 0 on
 * success, -ENOMEM on allocation failure. On failure, any previously
 * allocated cpumasks are freed.
 */
/*
 * alloc_cpumasks() - 为一组 cpumask_var_t 字段建立全有或全无的分配结果。
 *
 * @pmasks 是 @size 个“字段地址”，每个成功项都被零初始化；函数可睡眠。
 * 成功返回 0，所有字段归调用者并需 free_cpumask_var；任一失败返回 -ENOMEM，
 * 已分配项按逆序释放，因此失败出口不转移任何 mask 所有权。
 */
static inline int alloc_cpumasks(cpumask_var_t *pmasks[], u32 size)
{
	int i;

	/* 按数组顺序零初始化；任一项失败就逆序释放此前成功项，保持全有或全无语义。 */
	for (i = 0; i < size; i++) {
		if (!zalloc_cpumask_var(pmasks[i], GFP_KERNEL)) {
			while (--i >= 0)
				free_cpumask_var(*pmasks[i]);
			return -ENOMEM;
		}
	}
	return 0;
}

/**
 * alloc_tmpmasks - Allocate temporary cpumasks for cpuset operations.
 * @tmp: Pointer to tmpmasks structure to populate
 * Return: 0 on success, -ENOMEM on allocation failure
 */
/*
 * alloc_tmpmasks() 为一次更新事务初始化 @tmp 的三个可变 mask。
 * @tmp 由调用者持有且不可空；成功返回 0 并把释放责任交给 free_tmpmasks()，
 * 失败返回 -ENOMEM 且没有残留分配。函数使用 GFP_KERNEL，允许睡眠。
 */
static inline int alloc_tmpmasks(struct tmpmasks *tmp)
{
	/*
	 * Array of pointers to the three cpumask_var_t fields in tmpmasks.
	 * Note: Array size must match actual number of masks (3)
	 */
	/*
	 * 该指针数组把 tmpmasks 的三个 cpumask_var_t 字段交给统一分配器；数组长度必须
	 * 与字段数保持为 3，否则成功/回滚循环会漏分配或越界访问。
	 */
	cpumask_var_t *pmask[3] = {
		&tmp->new_cpus,
		&tmp->addmask,
		&tmp->delmask
	};

	return alloc_cpumasks(pmask, ARRAY_SIZE(pmask));
}

/**
 * free_tmpmasks - free cpumasks in a tmpmasks structure
 * @tmp: the tmpmasks structure pointer
 */
/*
 * free_tmpmasks() 释放 alloc_tmpmasks() 建立的三个 mask；@tmp 可为 NULL。
 * 无直接返回值，返回后字段不可再解引用。它不释放 tmp 结构本身。
 */
static inline void free_tmpmasks(struct tmpmasks *tmp)
{
	if (!tmp)
		return;

	free_cpumask_var(tmp->new_cpus);
	free_cpumask_var(tmp->addmask);
	free_cpumask_var(tmp->delmask);
}

/**
 * dup_or_alloc_cpuset - Duplicate or allocate a new cpuset
 * @cs: Source cpuset to duplicate (NULL for a fresh allocation)
 *
 * Creates a new cpuset by either:
 * 1. Duplicating an existing cpuset (if @cs is non-NULL), or
 * 2. Allocating a fresh cpuset with zero-initialized masks (if @cs is NULL)
 *
 * Return: Pointer to newly allocated cpuset on success, NULL on failure
 */
/*
 * dup_or_alloc_cpuset() - 构造尚未发布的 cpuset 或 trial 快照。
 *
 * @cs 非空时只借用源对象并复制标量与四个 mask；为空时创建清零对象。
 * 函数可睡眠。成功返回由调用者独占、必须 free_cpuset() 的对象；失败返回 NULL，
 * 已取得的结构或 mask 全部回滚。副本没有接入 css 层级，修改它不会被并发读者看见。
 */
static struct cpuset *dup_or_alloc_cpuset(struct cpuset *cs)
{
	struct cpuset *trial;

	/* Allocate base structure */
	/* 先分配结构本体；复制模式保留标量快照，新建模式则从全零状态开始。 */
	trial = cs ? kmemdup(cs, sizeof(*cs), GFP_KERNEL) :
		     kzalloc_obj(*cs);
	if (!trial)
		return NULL;

	trial->dl_bw_cpu = -1;

	/* Setup cpumask pointer array */
	/* 把四个动态位图字段组成统一分配/失败回滚表，结构本体此时尚未发布。 */
	cpumask_var_t *pmask[4] = {
		&trial->cpus_allowed,
		&trial->effective_cpus,
		&trial->effective_xcpus,
		&trial->exclusive_cpus
	};

	if (alloc_cpumasks(pmask, ARRAY_SIZE(pmask))) {
		kfree(trial);
		return NULL;
	}

	/* Copy masks if duplicating */
	/* 复制模式在所有位图均分配成功后再拷贝内容，避免半初始化副本逃逸。 */
	if (cs) {
		cpumask_copy(trial->cpus_allowed, cs->cpus_allowed);
		cpumask_copy(trial->effective_cpus, cs->effective_cpus);
		cpumask_copy(trial->effective_xcpus, cs->effective_xcpus);
		cpumask_copy(trial->exclusive_cpus, cs->exclusive_cpus);
	}

	return trial;
}

/**
 * free_cpuset - free the cpuset
 * @cs: the cpuset to be freed
 */
/*
 * free_cpuset() 是 dup_or_alloc_cpuset()/css_alloc 的最终析构器。
 * @cs 必须非空且已从 cgroup 生命周期中摘除；依次释放内嵌 mask 再释放宿主结构。
 * 无返回值，调用后所有裸指针失效；top_cpuset 绝不能传入。
 */
static inline void free_cpuset(struct cpuset *cs)
{
	free_cpumask_var(cs->cpus_allowed);
	free_cpumask_var(cs->effective_cpus);
	free_cpumask_var(cs->effective_xcpus);
	free_cpumask_var(cs->exclusive_cpus);
	kfree(cs);
}

/* Return user specified exclusive CPUs */
/*
 * user_xcpus()/xcpus_empty() 解释 partition 的用户请求来源：显式 exclusive_cpus
 * 非空时优先使用它，否则 cpus_allowed 充当隐式独占请求；两者都空才无可分配 CPU。
 * 返回的 mask 是借用内部指针，不增加引用、不得越过保护 @cs 的锁或生命周期。
 */
static inline struct cpumask *user_xcpus(struct cpuset *cs)
{
	return cpumask_empty(cs->exclusive_cpus) ? cs->cpus_allowed
						 : cs->exclusive_cpus;
}

/* 仅当 @cs 两种用户 CPU 请求都为空时返回 true。 */
static inline bool xcpus_empty(struct cpuset *cs)
{
	return cpumask_empty(cs->cpus_allowed) &&
	       cpumask_empty(cs->exclusive_cpus);
}

/*
 * cpusets_are_exclusive() - check if two cpusets are exclusive
 *
 * Return true if exclusive, false if not
 */
/*
 * cpusets_are_exclusive() 比较两个借用 cpuset 的用户独占请求是否不相交。
 * 调用者必须稳定两对象及 mask；函数不考虑在线状态或实际获批集合，只返回配置层
 * 冲突判断，不修改输入、不睡眠。
 */
static inline bool cpusets_are_exclusive(struct cpuset *cs1, struct cpuset *cs2)
{
	struct cpumask *xcpus1 = user_xcpus(cs1);
	struct cpumask *xcpus2 = user_xcpus(cs2);

	if (cpumask_intersects(xcpus1, xcpus2))
		return false;
	return true;
}

/**
 * cpus_excl_conflict - Check if two cpusets have exclusive CPU conflicts
 * @trial:	the trial cpuset to be checked
 * @sibling:	a sibling cpuset to be checked against
 * @xcpus_changed: set if exclusive_cpus has been set
 *
 * Returns: true if CPU exclusivity conflict exists, false otherwise
 *
 * Conflict detection rules:
 *  o cgroup v1
 *    See cpuset1_cpus_excl_conflict()
 *  o cgroup v2
 *    - The exclusive_cpus values cannot overlap.
 *    - New exclusive_cpus cannot be a superset of a sibling's cpus_allowed.
 */
/*
 * cpus_excl_conflict() 校验候选 @trial 与真实兄弟 @sibling 的 CPU 独占约束。
 * @xcpus_changed 表示本次确实改写 exclusive_cpus；v1 委托旧语义，v2 同时禁止
 * exclusive 集合相交，以及新 exclusive 完全吞掉兄弟 cpus_allowed。
 * 两指针均为借用，返回 true 表示事务必须拒绝，无副作用。
 */
static inline bool cpus_excl_conflict(struct cpuset *trial, struct cpuset *sibling,
				      bool xcpus_changed)
{
	if (!cpuset_v2())
		return cpuset1_cpus_excl_conflict(trial, sibling);

	/* The cpus_allowed of a sibling cpuset cannot be a subset of the new exclusive_cpus */
	/* 新独占集合不能完整吞掉兄弟的 cpus_allowed，否则兄弟可能失去全部可运行 CPU。 */
	if (xcpus_changed && !cpumask_empty(sibling->cpus_allowed) &&
	    cpumask_subset(sibling->cpus_allowed, trial->exclusive_cpus))
		return true;

	/* Exclusive_cpus cannot intersect */
	/* 两个兄弟显式声明的 exclusive_cpus 必须互斥，交集意味着 CPU 所有权重复。 */
	return cpumask_intersects(trial->exclusive_cpus, sibling->exclusive_cpus);
}

/*
 * mems_excl_conflict() 对两个兄弟 cpuset 执行 v1 内存独占冲突检查。
 * 只要任一声明 mem_exclusive，mems_allowed 相交就返回 true；不改变输入。
 */
static inline bool mems_excl_conflict(struct cpuset *cs1, struct cpuset *cs2)
{
	if ((is_mem_exclusive(cs1) || is_mem_exclusive(cs2)))
		return nodes_intersects(cs1->mems_allowed, cs2->mems_allowed);
	return false;
}

/*
 * validate_change() - Used to validate that any proposed cpuset change
 *		       follows the structural rules for cpusets.
 *
 * If we replaced the flag and mask values of the current cpuset
 * (cur) with those values in the trial cpuset (trial), would
 * our various subset and exclusive rules still be valid?  Presumes
 * cpuset_mutex held.
 *
 * 'cur' is the address of an actual, in-use cpuset.  Operations
 * such as list traversal that depend on the actual address of the
 * cpuset in the list must use cur below, not trial.
 *
 * 'trial' is the address of bulk structure copy of cur, with
 * perhaps one or more of the fields cpus_allowed, mems_allowed,
 * or flags changed to new, trial values.
 *
 * Return 0 if valid, -errno if not.
 */

/*
 * validate_change() - 在发布前验证 trial 是否保持 cpuset 层级不变量。
 *
 * @cur 是在线真实对象，遍历身份必须使用它；@trial 是调用者独占的未发布副本，
 * 其中一个或多个 mask/flag 已被候选值替换。调用者持 cpuset_mutex，函数以 RCU
 * 稳定兄弟遍历，可睡眠约束由外层保证。
 *
 * 校验顺序先执行 v1 规则，再跳过根的子集约束，随后检查 SCHED_DEADLINE 带宽是否
 * 允许缩小 CPU，最后检查兄弟 CPU/内存独占冲突。成功返回 0；-EBUSY 表示 DL
 * 容量不能收缩，-EINVAL 表示结构/独占规则失败。任何失败都不修改 @cur。
 */
static int validate_change(struct cpuset *cur, struct cpuset *trial)
{
	struct cgroup_subsys_state *css;
	struct cpuset *c, *par;
	bool xcpus_changed;
	int ret = 0;

	rcu_read_lock();

	if (!is_in_v2_mode())
		ret = cpuset1_validate_change(cur, trial);
	if (ret)
		goto out;

	/* Remaining checks don't apply to root cpuset */
	/* 根 cpuset 没有父级和兄弟约束，v1 通用校验通过后即可结束。 */
	if (cur == &top_cpuset)
		goto out;

	par = parent_cs(cur);

	/*
	 * We can't shrink if we won't have enough room for SCHED_DEADLINE
	 * tasks. This check is not done when scheduling is disabled as the
	 * users should know what they are doing.
	 *
	 * For v1, effective_cpus == cpus_allowed & user_xcpus() returns
	 * cpus_allowed.
	 *
	 * For v2, is_cpu_exclusive() & is_sched_load_balance() are true only
	 * for non-isolated partition root. At this point, the target
	 * effective_cpus isn't computed yet. user_xcpus() is the best
	 * approximation.
	 *
	 * TBD: May need to precompute the real effective_cpus here in case
	 * incorrect scheduling of SCHED_DEADLINE tasks in a partition
	 * becomes an issue.
	 */
	/*
	 * 启用负载均衡的 CPU 独占 cpuset 若缩小后容纳不了现有 SCHED_DEADLINE 带宽，
	 * 必须以 -EBUSY 拒绝；调度关闭时由管理员自行承担后果。v1 的 user_xcpus()
	 * 等于 cpus_allowed；v2 此时尚未算出候选 effective_cpus，只能用候选用户集合
	 * 近似，若未来出现 DL 任务错误归属问题，需要在这里预计算真实派生掩码。
	 */
	ret = -EBUSY;
	if (is_cpu_exclusive(cur) && is_sched_load_balance(cur) &&
	    !cpuset_cpumask_can_shrink(cur->effective_cpus, user_xcpus(trial)))
		goto out;

	/*
	 * If either I or some sibling (!= me) is exclusive, we can't
	 * overlap. exclusive_cpus cannot overlap with each other if set.
	 */
	/*
	 * 当前 cpuset 或任一兄弟声明独占后，双方资源不能重叠；显式 exclusive_cpus
	 * 之间更是无条件互斥。失败统一返回 -EINVAL，真实 @cur 保持未修改。
	 */
	ret = -EINVAL;
	xcpus_changed = !cpumask_equal(cur->exclusive_cpus, trial->exclusive_cpus);
	cpuset_for_each_child(c, css, par) {
		if (c == cur)
			continue;
		if (cpus_excl_conflict(trial, c, xcpus_changed))
			goto out;
		if (mems_excl_conflict(trial, c))
			goto out;
	}

	ret = 0;
out:
	rcu_read_unlock();
	return ret;
}

#ifdef CONFIG_SMP

/*
 * generate_sched_domains()
 *
 * This function builds a partial partition of the systems CPUs
 * A 'partial partition' is a set of non-overlapping subsets whose
 * union is a subset of that set.
 * The output of this function needs to be passed to kernel/sched/core.c
 * partition_sched_domains() routine, which will rebuild the scheduler's
 * load balancing domains (sched domains) as specified by that partial
 * partition.
 *
 * See "What is sched_load_balance" in Documentation/admin-guide/cgroup-v1/cpusets.rst
 * for a background explanation of this.
 *
 * Does not return errors, on the theory that the callers of this
 * routine would rather not worry about failures to rebuild sched
 * domains when operating in the severe memory shortage situations
 * that could cause allocation failures below.
 *
 * Must be called with cpuset_mutex held.
 *
 * The three key local variables below are:
 *    cp - cpuset pointer, used (together with pos_css) to perform a
 *	   top-down scan of all cpusets. For our purposes, rebuilding
 *	   the schedulers sched domains, we can ignore !is_sched_load_
 *	   balance cpusets.
 *  csa  - (for CpuSet Array) Array of pointers to all the cpusets
 *	   that need to be load balanced, for convenient iterative
 *	   access by the subsequent code that finds the best partition,
 *	   i.e the set of domains (subsets) of CPUs such that the
 *	   cpus_allowed of every cpuset marked is_sched_load_balance
 *	   is a subset of one of these domains, while there are as
 *	   many such domains as possible, each as small as possible.
 * doms  - Conversion of 'csa' to an array of cpumasks, for passing to
 *	   the kernel/sched/core.c routine partition_sched_domains() in a
 *	   convenient format, that can be easily compared to the prior
 *	   value to determine what partition elements (sched domains)
 *	   were changed (added or removed.)
 */
/*
 * generate_sched_domains() - 把有效 partition roots 转换为调度器分区数组。
 *
 * 调用者持 cpuset_mutex。@domains、@attributes 是输出参数：成功/降级返回正的
 * domain 数，输出内存交给 partition_sched_domains() 消费；内存不足时返回默认
 * ndoms=1，允许 domains 为 NULL，让调度器恢复单一默认域，而不让控制文件操作失败。
 * v2 跳过 isolated/空 partition，并保证各 domain 不重叠；v1 委托旧实现。
 */
static int generate_sched_domains(cpumask_var_t **domains,
			struct sched_domain_attr **attributes)
{
	struct cpuset *cp;	/* top-down scan of cpusets */
	/* cp 是 RCU 遍历中的借用 cpuset，按层级自顶向下扫描。 */
	struct cpuset **csa;	/* array of all cpuset ptrs */
	/* csa 暂存满足条件的 partition root 裸指针，仅在 cpuset_mutex 保护期内有效。 */
	int i, j;		/* indices for partition finding loops */
	/* i/j 是 partition 去重与重叠校验的数组下标。 */
	cpumask_var_t *doms;	/* resulting partition; i.e. sched domains */
	/* doms 是返回给调度器的 domain CPU 掩码数组，成功后由调度器接口接管。 */
	struct sched_domain_attr *dattr;  /* attributes for custom domains */
	/* dattr 与 doms 同下标描述 domain 属性；分配失败允许为 NULL 并使用默认属性。 */
	int ndoms = 0;		/* number of sched domains in result */
	/* ndoms 既是 csa 中候选数量，也是最终交给调度器的 domain 数量。 */
	struct cgroup_subsys_state *pos_css;

	if (!cpuset_v2())
		return cpuset1_generate_sched_domains(domains, attributes);

	doms = NULL;
	dattr = NULL;
	csa = NULL;

	/* Special case for the 99% of systems with one, full, sched domain */
	/* 没有 CPU 被划给子 partition 时直接生成单一完整 domain，覆盖绝大多数系统。 */
	if (cpumask_empty(subpartitions_cpus)) {
		ndoms = 1;
		/* !csa will be checked and can be correctly handled */
		/* csa 保持 NULL 是有意哨兵，生成阶段据此选择 top_cpuset。 */
		goto generate_doms;
	}

	csa = kmalloc_objs(cp, nr_cpusets());
	if (!csa)
		goto done;

	/* Find how many partitions and cache them to csa[] */
	/* 自顶向下收集可参与负载均衡的有效 partition，并把数量累积到 ndoms。 */
	rcu_read_lock();
	cpuset_for_each_descendant_pre(cp, pos_css, &top_cpuset) {
		/*
		 * Only valid partition roots that are not isolated and with
		 * non-empty effective_cpus will be saved into csa[].
		 */
		/*
		 * 只有非 isolated、状态有效且 effective_cpus 非空的 root 才形成 sched_domain；
		 * isolated partition 刻意不参与负载均衡，空集合也无法形成调度跨度。
		 */
		if ((cp->partition_root_state == PRS_ROOT) &&
		    !cpumask_empty(cp->effective_cpus))
			csa[ndoms++] = cp;

		/*
		 * Skip @cp's subtree if not a partition root and has no
		 * exclusive CPUs to be granted to child cpusets.
		 */
		/*
		 * 当前节点既不是有效 partition，又没有可继续下传的独占 CPU 时，后代不可能
		 * 产生新的 partition root，直接跳过整棵子树以降低层级扫描成本。
		 */
		if (!is_partition_valid(cp) && cpumask_empty(cp->exclusive_cpus))
			pos_css = css_rightmost_descendant(pos_css);
	}
	rcu_read_unlock();

	for (i = 0; i < ndoms; i++) {
		for (j = i + 1; j < ndoms; j++) {
			if (cpusets_overlap(csa[i], csa[j]))
				/*
				 * Cgroup v2 shouldn't pass down overlapping
				 * partition root cpusets.
				 */
				/* v2 的 partition CPU 所有权应互斥；重叠说明上游层级不变量已被破坏。 */
				WARN_ON_ONCE(1);
		}
	}

generate_doms:
	doms = alloc_sched_domains(ndoms);
	if (!doms)
		goto done;

	/*
	 * The rest of the code, including the scheduler, can deal with
	 * dattr==NULL case. No need to abort if alloc fails.
	 */
	/*
	 * 调度器把 NULL 属性数组解释为全部使用默认属性，因此属性分配失败只降低定制能力，
	 * 不必撤销已经生成的 domain CPU 掩码。
	 */
	dattr = kmalloc_objs(struct sched_domain_attr, ndoms);

	/*
	 * Cgroup v2 doesn't support domain attributes, just set all of them
	 * to SD_ATTR_INIT. Also non-isolating partition root CPUs are a
	 * subset of HK_TYPE_DOMAIN_BOOT housekeeping CPUs.
	 */
	/*
	 * v2 不暴露自定义 domain 属性，统一初始化为 SD_ATTR_INIT；非隔离 partition 的
	 * CPU 还必须属于启动时允许参与 domain 的 housekeeping 集合。
	 */
	for (i = 0; i < ndoms; i++) {
		/*
		 * The top cpuset may contain some boot time isolated
		 * CPUs that need to be excluded from the sched domain.
		 */
		/* top_cpuset 可能包含启动参数隔离的 CPU，生成默认 domain 时必须显式扣除。 */
		if (!csa || csa[i] == &top_cpuset)
			cpumask_and(doms[i], top_cpuset.effective_cpus,
				    housekeeping_cpumask(HK_TYPE_DOMAIN_BOOT));
		else
			cpumask_copy(doms[i], csa[i]->effective_cpus);
		if (dattr)
			dattr[i] = SD_ATTR_INIT;
	}

done:
	kfree(csa);

	/*
	 * Fallback to the default domain if kmalloc() failed.
	 * See comments in partition_sched_domains().
	 */
	/*
	 * domain 掩码分配失败时以 ndoms=1 请求调度器恢复默认单域；NULL doms 是该接口
	 * 约定的降级输入，所以不把内存压力转化为 cpuset 配置失败。
	 */
	if (doms == NULL)
		ndoms = 1;

	*domains    = doms;
	*attributes = dattr;
	return ndoms;
}

/*
 * dl_update_tasks_root_domain() 把 @cs 中所有 SCHED_DEADLINE 任务重新计入其当前
 * root_domain。@cs 由调用者持有 css 引用且 cpuset 拓扑已稳定；计数为零走快路径。
 * 迭代器为每个任务提供临时稳定引用，函数无直接返回值，副作用落在调度器 DL 记账。
 */
static void dl_update_tasks_root_domain(struct cpuset *cs)
{
	struct css_task_iter it;
	struct task_struct *task;

	if (cs->nr_deadline_tasks == 0)
		return;

	css_task_iter_start(&cs->css, 0, &it);

	while ((task = css_task_iter_next(&it)))
		dl_add_task_root_domain(task);

	css_task_iter_end(&it);
}

/*
 * dl_rebuild_rd_accounting() - 在 sched_domain 重分区后重建 DL 带宽归属。
 *
 * 入口同时持 cpuset_mutex、CPU hotplug lock 和 sched_domains_mutex。
 * 函数先用 cookie 对每个 root_domain 只清理一次，再按 cpuset 层级对非空子树临时
 * css_get；释放 RCU 后调用可能较重的任务迭代，再恢复 RCU。无直接返回值，成功后
 * 所有 DL 任务与新的 root_domain 一致，临时 css 引用均已归还。
 */
void dl_rebuild_rd_accounting(void)
{
	struct cpuset *cs = NULL;
	struct cgroup_subsys_state *pos_css;
	int cpu;
	u64 cookie = ++dl_cookie;

	lockdep_assert_cpuset_lock_held();
	lockdep_assert_cpus_held();
	lockdep_assert_held(&sched_domains_mutex);

	/* 阶段一：在 RCU 快照内按 root_domain cookie 去重，清空旧的 DL CPU 归属。 */
	rcu_read_lock();

	for_each_possible_cpu(cpu) {
		if (dl_bw_visited(cpu, cookie))
			continue;

		dl_clear_root_domain_cpu(cpu);
	}

	/*
	 * 阶段二：自顶向下遍历有 effective CPU 的 cpuset。css 引用跨越退出 RCU 的窗口，
	 * 允许任务迭代和 root_domain 记账使用可睡眠/较重的锁协议。
	 */
	cpuset_for_each_descendant_pre(cs, pos_css, &top_cpuset) {

		if (cpumask_empty(cs->effective_cpus)) {
			pos_css = css_rightmost_descendant(pos_css);
			continue;
		}

		css_get(&cs->css);

		rcu_read_unlock();

		dl_update_tasks_root_domain(cs);

		rcu_read_lock();
		css_put(&cs->css);
	}
	rcu_read_unlock();
}

/*
 * Rebuild scheduler domains.
 *
 * If the flag 'sched_load_balance' of any cpuset with non-empty
 * 'cpus' changes, or if the 'cpus' allowed changes in any cpuset
 * which has that flag enabled, or if any cpuset with a non-empty
 * 'cpus' is removed, then call this routine to rebuild the
 * scheduler's dynamic sched domains.
 *
 * Call with cpuset_mutex held.  Takes cpus_read_lock().
 */
/*
 * rebuild_sched_domains_locked() - 提交 cpuset 派生的调度域布局。
 *
 * 调用者持 CPU hotplug lock 与 cpuset_mutex；函数消费 force_sd_rebuild，
 * 生成 domain 后再次确认每个 CPU 都 active，避免把离线 CPU 交给调度器触发崩溃。
 * 无直接返回值；正常路径由 partition_sched_domains() 接管/比较输出数组，异常 mask
 * 路径释放临时数组并保留旧布局。内存不足由 generate_sched_domains() 降级处理。
 */
void rebuild_sched_domains_locked(void)
{
	struct sched_domain_attr *attr;
	cpumask_var_t *doms;
	int ndoms;
	int i;

	lockdep_assert_cpus_held();
	lockdep_assert_cpuset_lock_held();
	force_sd_rebuild = false;

	/* Generate domain masks and attrs */
	/* 先把 cpuset 层级快照转换成调度器可消费的 domain 掩码和可选属性数组。 */
	ndoms = generate_sched_domains(&doms, &attr);

	/*
	* cpuset_hotplug_workfn is invoked synchronously now, thus this
	* function should not race with CPU hotplug. And the effective CPUs
	* must not include any offline CPUs. Passing an offline CPU in the
	* doms to partition_sched_domains() will trigger a kernel panic.
	*
	* We perform a final check here: if the doms contains any
	* offline CPUs, a warning is emitted and we return directly to
	* prevent the panic.
	*/
	/*
	 * 热插拔处理现在同步执行，因此这里理论上不会与 CPU 状态变化竞态，生成的 domain
	 * 也必须完全属于 cpu_active_mask。提交前仍做最后防线：一旦发现离线 CPU，先释放
	 * 临时数组并退出，避免 partition_sched_domains() 因非法 domain 触发 panic。
	 */
	for (i = 0; doms && i < ndoms; i++) {
		if (WARN_ON_ONCE(!cpumask_subset(doms[i], cpu_active_mask))) {
			free_sched_domains(doms, ndoms);
			kfree(attr);
			return;
		}
	}

	/* Have scheduler rebuild the domains */
	/* 校验通过后把新布局提交给调度器；该调用消费本次生成的 domain 描述。 */
	partition_sched_domains(ndoms, doms, attr);
}

#else /* !CONFIG_SMP */
/* !SMP 配置的空实现：没有跨 CPU sched_domain 需要重建。 */
void rebuild_sched_domains_locked(void)
{
}

/*
 * !CONFIG_SMP 时没有跨 CPU 调度域可重建，此 stub 保持调用接口和锁契约，
 * 无参数、无返回值、无副作用。
 */
#endif /* CONFIG_SMP */

/*
 * rebuild_sched_domains_cpuslocked() 供已经持有 CPU hotplug lock 的路径使用，
 * 内部只补 cpuset_mutex。函数可能睡眠，无返回值，返回时调度域已尝试同步。
 */
static void rebuild_sched_domains_cpuslocked(void)
{
	mutex_lock(&cpuset_mutex);
	rebuild_sched_domains_locked();
	mutex_unlock(&cpuset_mutex);
}

/*
 * rebuild_sched_domains() 是无锁调用者的公共包装：先固定 active CPU 集合，
 * 再进入 cpuset 锁内重建。无参数、无直接返回值，函数可能睡眠。
 */
void rebuild_sched_domains(void)
{
	cpus_read_lock();
	rebuild_sched_domains_cpuslocked();
	cpus_read_unlock();
}

/*
 * cpuset_reset_sched_domains() 强制调度器恢复一个默认 domain，供控制器停用/重置。
 * 它只取 cpuset_mutex，没有改变用户 mask；无返回值，可能睡眠。
 */
void cpuset_reset_sched_domains(void)
{
	mutex_lock(&cpuset_mutex);
	partition_sched_domains(1, NULL, NULL);
	mutex_unlock(&cpuset_mutex);
}

/**
 * cpuset_update_tasks_cpumask - Update the cpumasks of tasks in the cpuset.
 * @cs: the cpuset in which each task's cpus_allowed mask needs to be changed
 * @new_cpus: the temp variable for the new effective_cpus mask
 *
 * Iterate through each task of @cs updating its cpus_allowed to the
 * effective cpuset's.  As this function is called with cpuset_mutex held,
 * cpuset membership stays stable.
 *
 * For top_cpuset, task_cpu_possible_mask() is used instead of effective_cpus
 * to make sure all offline CPUs are also included as hotplug code won't
 * update cpumasks for tasks in top_cpuset.
 *
 * As task_cpu_possible_mask() can be task dependent in arm64, we have to
 * do cpu masking per task instead of doing it once for all.
 */
/*
 * cpuset_update_tasks_cpumask() - 把 @cs 的 effective CPU 约束发布给所有成员任务。
 *
 * @cs 与 @new_cpus 均为借用对象，调用者持 cpuset_mutex使成员集合稳定；函数逐任务
 * 按架构 possible mask 裁剪，并通过 set_cpus_allowed_ptr() 进入调度器锁协议。
 * top_cpuset 额外排除已分给子 partition 的 CPU，但保留 offline possible CPU 供
 * 热插拔恢复；内核线程与不可设亲和任务由 housekeeping/自身协议管理而跳过。
 * 无直接返回值，可能睡眠；返回时所有可更新成员的调度亲和性已提交。
 */
void cpuset_update_tasks_cpumask(struct cpuset *cs, struct cpumask *new_cpus)
{
	struct css_task_iter it;
	struct task_struct *task;
	bool top_cs = cs == &top_cpuset;

	css_task_iter_start(&cs->css, 0, &it);
	while ((task = css_task_iter_next(&it))) {
		const struct cpumask *possible_mask = task_cpu_possible_mask(task);

		if (top_cs) {
			/*
			 * PF_KTHREAD tasks are handled by housekeeping.
			 * PF_NO_SETAFFINITY tasks are ignored.
			 */
			/*
			 * 内核线程由 housekeeping 亲和策略统一管理；PF_NO_SETAFFINITY 明确禁止
			 * cpuset 改写。跳过两类任务可避免覆盖更高优先级的调度约束。
			 */
			if (task->flags & (PF_KTHREAD | PF_NO_SETAFFINITY))
				continue;
			cpumask_andnot(new_cpus, possible_mask, subpartitions_cpus);
		} else {
			cpumask_and(new_cpus, possible_mask, cs->effective_cpus);
		}
		set_cpus_allowed_ptr(task, new_cpus);
	}
	css_task_iter_end(&it);
}

/**
 * compute_effective_cpumask - Compute the effective cpumask of the cpuset
 * @new_cpus: the temp variable for the new effective_cpus mask
 * @cs: the cpuset the need to recompute the new effective_cpus mask
 * @parent: the parent cpuset
 *
 * The result is valid only if the given cpuset isn't a partition root.
 */
/*
 * compute_effective_cpumask() 为非 partition cpuset 计算普通层级交集：
 * @new_cpus 是输出工作区，@cs/@parent 是稳定借用对象。结果等于用户请求与父
 * effective 集合的交集；不检查在线回退、不发布状态、无返回值。
 */
static void compute_effective_cpumask(struct cpumask *new_cpus,
				      struct cpuset *cs, struct cpuset *parent)
{
	cpumask_and(new_cpus, cs->cpus_allowed, parent->effective_cpus);
}

/*
 * Commands for update_parent_effective_cpumask
 */
/*
 * partition_cmd 描述父子 CPU 所有权事务的五种意图：建立普通/隔离 root、撤销 root、
 * 根据外部变化重算父池，以及强制把当前 partition 标记为无效。
 */
enum partition_cmd {
	partcmd_enable,		/* Enable partition root	  */
	/* 建立参与负载均衡的 partition root。 */
	partcmd_enablei,	/* Enable isolated partition root */
	/* 建立不参与负载均衡的 isolated partition root。 */
	partcmd_disable,	/* Disable partition root	  */
	/* 撤销 partition root，并把其独占 CPU 归还父级。 */
	partcmd_update,		/* Update parent's effective_cpus */
	/* 配置或 hotplug 改变后重新计算父级 effective_cpus。 */
	partcmd_invalidate,	/* Make partition invalid	  */
	/* 保留用户期望状态，但把当前 partition 转为对应负状态。 */
};

/*
 * partition_cmd 描述“子 partition 对父 effective CPU 池”的事务方向：
 * enable/enablei 取走 CPU，disable 归还，update 重算差量，invalidate 只做失效
 * 转换。它是内部协议枚举，不直接暴露给用户态。
 */

/* 父池变化后传播普通兄弟子树；有效 partition 作为独立边界跳过。 */
static void update_sibling_cpumasks(struct cpuset *parent, struct cpuset *cs,
				    struct tmpmasks *tmp);

/*
 * Update partition exclusive flag
 *
 * Return: 0 if successful, an error code otherwise
 */
/*
 * update_partition_exclusive_flag() 让 CS_CPU_EXCLUSIVE 与候选 partition 状态一致。
 * @cs 为在线借用对象，@new_prs 使用 PRS_* 编码；调用者持 cpuset_mutex。
 * 开启可能因层级独占校验失败返回 PERR_NOTEXCL，关闭按协议不会失败；成功返回 0。
 * flag 的实际提交委托 cpuset_update_flag()，可能触发调度域脏标记。
 */
static int update_partition_exclusive_flag(struct cpuset *cs, int new_prs)
{
	bool exclusive = (new_prs > PRS_MEMBER);

	if (exclusive && !is_cpu_exclusive(cs)) {
		if (cpuset_update_flag(CS_CPU_EXCLUSIVE, cs, 1))
			return PERR_NOTEXCL;
	} else if (!exclusive && is_cpu_exclusive(cs)) {
		/* Turning off CS_CPU_EXCLUSIVE will not return error */
		/* 清除独占只放宽资源约束，不会制造兄弟冲突，因此该方向按协议不会返回错误。 */
		cpuset_update_flag(CS_CPU_EXCLUSIVE, cs, 0);
	}
	return 0;
}

/*
 * Update partition load balance flag and/or rebuild sched domain
 *
 * Changing load balance flag will automatically call
 * rebuild_sched_domains_locked().
 * This function is for cgroup v2 only.
 */
/*
 * update_partition_sd_lb() - 让负载均衡 flag 与 partition 状态转换保持一致。
 *
 * @cs 是已提交新 partition_root_state 的在线对象；@old_prs 是事务前快照。
 * 调用者持 cpuset_mutex。有效 root 开启负载均衡，isolated 关闭；无效/member
 * 继承父状态。函数无直接返回值，只更新 flag 并在可能影响 domain 时设置重建脏位。
 */
static void update_partition_sd_lb(struct cpuset *cs, int old_prs)
{
	int new_prs = cs->partition_root_state;
	bool rebuild_domains = (new_prs > 0) || (old_prs > 0);
	bool new_lb;

	/*
	 * If cs is not a valid partition root, the load balance state
	 * will follow its parent.
	 */
	/* 非有效 partition 不再拥有独立 sched_domain，负载均衡策略必须继承父节点。 */
	if (new_prs > 0) {
		new_lb = (new_prs != PRS_ISOLATED);
	} else {
		new_lb = is_sched_load_balance(parent_cs(cs));
	}
	if (new_lb != !!is_sched_load_balance(cs)) {
		rebuild_domains = true;
		if (new_lb)
			set_bit(CS_SCHED_LOAD_BALANCE, &cs->flags);
		else
			clear_bit(CS_SCHED_LOAD_BALANCE, &cs->flags);
	}

	if (rebuild_domains)
		cpuset_force_rebuild();
}

/*
 * tasks_nocpu_error - Return true if tasks will have no effective_cpus
 */
/*
 * tasks_nocpu_error() 在真正从父池取走 @xcpus 前检查“有任务却无 CPU”的禁态。
 * @parent、@cs、@xcpus 都是事务内稳定借用值。父 partition 被全部取空且仍有任务，
 * 或子 partition 得不到任何 active CPU 且已 populated 时返回 true；无副作用。
 */
static bool tasks_nocpu_error(struct cpuset *parent, struct cpuset *cs,
			      struct cpumask *xcpus)
{
	/*
	 * A populated partition (cs or parent) can't have empty effective_cpus
	 */
	/* 有任务或迁入中的父/子 partition 都必须保留至少一个 active effective CPU。 */
	return (cpumask_subset(parent->effective_cpus, xcpus) &&
		partition_is_populated(parent, cs)) ||
	       (!cpumask_intersects(xcpus, cpu_active_mask) &&
		partition_is_populated(cs, NULL));
}

/*
 * reset_partition_data() - 把非有效 partition 的派生字段恢复为普通 cpuset 语义。
 *
 * @cs 为借用输入输出对象，调用者持 callback_lock；v1 无动作。若无显式独占请求，
 * 清 effective_xcpus/CPU_EXCLUSIVE；effective_cpus 为空时继承父有效集合。
 * 无直接返回值，不改变用户 cpus_allowed。
 */
static void reset_partition_data(struct cpuset *cs)
{
	struct cpuset *parent = parent_cs(cs);

	/* v1 没有 v2 partition 派生字段语义，保持原状态直接返回。 */
	if (!cpuset_v2())
		return;

	lockdep_assert_held(&callback_lock);

	/* 无显式独占请求时清除隐式获批集合与独占 flag，避免负状态继续占有旧资源。 */
	if (cpumask_empty(cs->exclusive_cpus)) {
		cpumask_clear(cs->effective_xcpus);
		if (is_cpu_exclusive(cs))
			clear_bit(CS_CPU_EXCLUSIVE, &cs->flags);
	}
	/* 普通节点按父/用户集合重算；交集为空时继承父级，保持任务可运行。 */
	if (!cpumask_and(cs->effective_cpus, parent->effective_cpus, cs->cpus_allowed))
		cpumask_copy(cs->effective_cpus, parent->effective_cpus);
}

/*
 * isolated_cpus_update - Update the isolated_cpus mask
 * @old_prs: old partition_root_state
 * @new_prs: new partition_root_state
 * @xcpus: exclusive CPUs with state change
 */
/*
 * isolated_cpus_update() 更新全局 isolated CPU 并集。
 * @old_prs/@new_prs 必须不同，@xcpus 是发生归属变化的稳定借用 mask；入口同时持
 * cpuset_mutex 与 callback_lock。只有集合真实改变才设置 update_housekeeping，
 * 由事务尾部在可睡眠上下文传播。无返回值，不直接调用 housekeeping。
 */
static void isolated_cpus_update(int old_prs, int new_prs, struct cpumask *xcpus)
{
	WARN_ON_ONCE(old_prs == new_prs);
	lockdep_assert_held(&callback_lock);
	lockdep_assert_held(&cpuset_mutex);
	/* 按新旧类型决定向 isolated 并集添加或删除；集合已满足时避免无意义 HK 更新。 */
	if (new_prs == PRS_ISOLATED) {
		if (cpumask_subset(xcpus, isolated_cpus))
			return;
		cpumask_or(isolated_cpus, isolated_cpus, xcpus);
	} else {
		if (!cpumask_intersects(xcpus, isolated_cpus))
			return;
		cpumask_andnot(isolated_cpus, isolated_cpus, xcpus);
	}
	update_housekeeping = true;
}

/*
 * partition_xcpus_add - Add new exclusive CPUs to partition
 * @new_prs: new partition_root_state
 * @parent: parent cpuset
 * @xcpus: exclusive CPUs to be added
 *
 * Remote partition if parent == NULL
 */
/*
 * partition_xcpus_add() - 把 @xcpus 从父 effective 池转交给新/扩大的 partition。
 *
 * @parent 为空表示远程 partition，逻辑父池为 top_cpuset；@new_prs 必须有效。
 * 调用者持 callback_lock。函数同步维护 subpartitions_cpus、isolated_cpus 和父
 * effective_cpus；无返回值。返回时这些 CPU 对父任务不可用，但子对象字段由调用者
 * 在同一事务中继续提交。
 */
static void partition_xcpus_add(int new_prs, struct cpuset *parent,
				struct cpumask *xcpus)
{
	WARN_ON_ONCE(new_prs < 0);
	lockdep_assert_held(&callback_lock);
	if (!parent)
		parent = &top_cpuset;


	/* 远程/根下 partition 还需登记到全局 subpartitions_cpus，供根任务与 hotplug 扣除。 */
	if (parent == &top_cpuset)
		cpumask_or(subpartitions_cpus, subpartitions_cpus, xcpus);

	/* 父子隔离类型不同才改变 isolated 并集；同类型内部转移不会改变全局集合。 */
	if (new_prs != parent->partition_root_state)
		isolated_cpus_update(parent->partition_root_state, new_prs,
				     xcpus);

	/* 最后从父 effective 池删除已转交 CPU，完成对并发读者可见的所有权提交。 */
	cpumask_andnot(parent->effective_cpus, parent->effective_cpus, xcpus);
}

/*
 * partition_xcpus_del - Remove exclusive CPUs from partition
 * @old_prs: old partition_root_state
 * @parent: parent cpuset
 * @xcpus: exclusive CPUs to be removed
 *
 * Remote partition if parent == NULL
 */
/*
 * partition_xcpus_del() 是 add 的逆转换：把 @xcpus 归还父 effective 池。
 * @old_prs 描述归还前的 partition 类型，@parent 为空仍代表 top 池。
 * 调用者持 callback_lock；归还结果再次与 cpu_active_mask 相交，避免离线 CPU
 * 进入父 effective 集合。无直接返回值。
 */
static void partition_xcpus_del(int old_prs, struct cpuset *parent,
				struct cpumask *xcpus)
{
	WARN_ON_ONCE(old_prs < 0);
	lockdep_assert_held(&callback_lock);
	if (!parent)
		parent = &top_cpuset;

	/* 先撤销根级分配登记，再按隔离类型差量维护 housekeeping 派生集合。 */
	if (parent == &top_cpuset)
		cpumask_andnot(subpartitions_cpus, subpartitions_cpus, xcpus);

	if (old_prs != parent->partition_root_state)
		isolated_cpus_update(old_prs, parent->partition_root_state,
				     xcpus);

	/* 归还父池后与 active mask 相交，离线 CPU 只保留在用户配置而不进入 effective。 */
	cpumask_or(parent->effective_cpus, parent->effective_cpus, xcpus);
	cpumask_and(parent->effective_cpus, parent->effective_cpus, cpu_active_mask);
}

/*
 * isolated_cpus_can_update - check for isolated & nohz_full conflicts
 * @add_cpus: cpu mask for cpus that are going to be isolated
 * @del_cpus: cpu mask for cpus that are no longer isolated, can be NULL
 * Return: false if there is conflict, true otherwise
 *
 * If nohz_full is enabled and we have isolated CPUs, their combination must
 * still leave housekeeping CPUs.
 *
 * TBD: Should consider merging this function into
 *      prstate_housekeeping_conflict().
 */
/*
 * isolated_cpus_can_update() - 预演 isolated 集合变化是否仍保留 housekeeping CPU。
 *
 * @add_cpus 是将隔离的集合，@del_cpus 可空且表示解除隔离集合；两者只借用。
 * 未启用 kernel-noise housekeeping 或删除本身可恢复 HK CPU 时快速成功。
 * 慢路径可睡眠分配临时 mask；返回 false 同时覆盖配置冲突和 -ENOMEM，调用者据此
 * 拒绝事务，保证 nohz_full/domain/active 交集至少留下一个 HK CPU。
 */
static bool isolated_cpus_can_update(struct cpumask *add_cpus,
				     struct cpumask *del_cpus)
{
	cpumask_var_t full_hk_cpus;
	int res = true;

	if (!housekeeping_enabled(HK_TYPE_KERNEL_NOISE))
		return true;

	if (del_cpus && cpumask_weight_and(del_cpus,
			housekeeping_cpumask(HK_TYPE_KERNEL_NOISE)))
		return true;

	/* 删除不能直接恢复 HK CPU时，分配工作 mask 精确模拟变更后的完整 HK 交集。 */
	if (!alloc_cpumask_var(&full_hk_cpus, GFP_KERNEL))
		return false;

	cpumask_and(full_hk_cpus, housekeeping_cpumask(HK_TYPE_KERNEL_NOISE),
		    housekeeping_cpumask(HK_TYPE_DOMAIN));
	/* 扣除当前 isolated、限制 active，再预扣 add_cpus；结果为空就拒绝本次隔离。 */
	cpumask_andnot(full_hk_cpus, full_hk_cpus, isolated_cpus);
	cpumask_and(full_hk_cpus, full_hk_cpus, cpu_active_mask);
	if (!cpumask_weight_andnot(full_hk_cpus, add_cpus))
		res = false;

	free_cpumask_var(full_hk_cpus);
	return res;
}

/*
 * prstate_housekeeping_conflict - check for partition & housekeeping conflicts
 * @prstate: partition root state to be checked
 * @new_cpus: cpu mask
 * Return: true if there is conflict, false otherwise
 *
 * CPUs outside of HK_TYPE_DOMAIN_BOOT, if defined, can only be used in an
 * isolated partition.
 */
/*
 * prstate_housekeeping_conflict() 校验启动期 HK_TYPE_DOMAIN_BOOT 的硬边界。
 * @prstate 是候选 PRS_*，@new_cpus 是候选有效 CPU 借用集合。非 isolated
 * partition 若包含 boot 时排除在 domain housekeeping 外的 CPU就返回 true；
 * 未启用该 HK 类型时恒 false。无分配、无副作用。
 */
static bool prstate_housekeeping_conflict(int prstate, struct cpumask *new_cpus)
{
	if (!housekeeping_enabled(HK_TYPE_DOMAIN_BOOT))
		return false;

	if ((prstate != PRS_ISOLATED) &&
	    !cpumask_subset(new_cpus, housekeeping_cpumask(HK_TYPE_DOMAIN_BOOT)))
		return true;

	return false;
}

/*
 * cpuset_update_sd_hk_unlock - Rebuild sched domains, update HK & unlock
 *
 * Update housekeeping cpumasks and rebuild sched domains if necessary and
 * then do a cpuset_full_unlock().
 * This should be called at the end of cpuset operation.
 */
/*
 * cpuset_update_sd_hk_unlock() - 在 cpuset 操作尾部重建调度域、传播 HK 并解锁。
 *
 * 调用者进入时持有 cpuset_top_mutex、cpus_read_lock 和 cpuset_mutex；
 * 函数无参数、无直接返回值，并通过 __releases 标注承诺释放两个 mutex
 * （cpus_read_lock 也由路径配套释放）。若 force_sd_rebuild 置位，先让调度域
 * 与已提交 cpuset CPU 状态一致；若 update_housekeeping 置位，再复制稳定的
 * isolated_cpus 快照并在只持顶层串行锁时调用 housekeeping_update()。
 *
 * housekeeping_update() 的 errno 只通过 WARN 暴露，因为 cpuset 状态和调度域
 * 已经提交，不能在这里简单回滚整个分区事务。无 HK 更新时走统一 full unlock。
 */
static void cpuset_update_sd_hk_unlock(void)
	__releases(&cpuset_mutex)
	__releases(&cpuset_top_mutex)
{
	/* force_sd_rebuild will be cleared in rebuild_sched_domains_locked() */
	/*
	 * force_sd_rebuild 会在 rebuild_sched_domains_locked() 内清除。必须先重建
	 * 调度域，再传播 housekeeping 缓存，避免消费者基于旧 domain 拓扑继续工作。
	 */
	if (force_sd_rebuild)
		rebuild_sched_domains_locked();

	if (update_housekeeping) {
		/*
		 * isolated_hk_cpus 是专门传给可能睡眠更新路径的稳定副本；先清 pending
		 * 标志并复制，后面释放 cpuset_mutex 后不再直接读取可变 isolated_cpus。
		 */
		update_housekeeping = false;
		cpumask_copy(isolated_hk_cpus, isolated_cpus);

		/*
		 * housekeeping_update() is now called without holding
		 * cpus_read_lock and cpuset_mutex. Only cpuset_top_mutex
		 * is still being held for mutual exclusion.
		 */
		/*
		 * 调用 housekeeping_update() 时不再持有 cpus_read_lock 和
		 * cpuset_mutex，只保留 cpuset_top_mutex 做写者互斥。这样下游 flush
		 * 的 workqueue 可以自由经过 CPU hotplug/cpuset 路径，不形成锁反转。
		 */
		mutex_unlock(&cpuset_mutex);
		cpus_read_unlock();
		WARN_ON_ONCE(housekeeping_update(isolated_hk_cpus));
		/* HK 发布与下游传播均已结束，最后释放本次 cpuset 操作的顶层串行锁。 */
		mutex_unlock(&cpuset_top_mutex);
	} else {
		/* 没有动态 HK 工作时，由公共 helper 按常规逆序释放完整锁组。 */
		cpuset_full_unlock();
	}
}

/*
 * Work function to invoke cpuset_update_sd_hk_unlock()
 */
/*
 * hk_sd_workfn() 是延迟处理 hotplug 后 housekeeping 更新的无载荷 work 回调。
 * @work 只用于 workqueue 调度，本函数不读取其内容；它重新获取完整锁组并消费当前
 * 全局脏位，因此多次变化可合并且不会使用入队时旧快照。无返回值，允许睡眠。
 */
static void hk_sd_workfn(struct work_struct *work)
{
	cpuset_full_lock();
	cpuset_update_sd_hk_unlock();
}

/**
 * rm_siblings_excl_cpus - Remove exclusive CPUs that are used by sibling cpusets
 * @parent: Parent cpuset containing all siblings
 * @cs: Current cpuset (will be skipped)
 * @excpus:  exclusive effective CPU mask to modify
 *
 * This function ensures the given @excpus mask doesn't include any CPUs that
 * are exclusively allocated to sibling cpusets. It walks through all siblings
 * of @cs under @parent and removes their exclusive CPUs from @excpus.
 */
/*
 * rm_siblings_excl_cpus() 从候选 @excpus 中扣除所有兄弟已请求/获批的独占 CPU。
 * @parent/@cs 是 RCU 层级内稳定的借用对象，@excpus 是调用者拥有的输入输出工作区。
 * 返回发生相交的兄弟数量，0 表示没有冲突；函数只修改工作 mask，不修改任何 cpuset。
 */
static int rm_siblings_excl_cpus(struct cpuset *parent, struct cpuset *cs,
					struct cpumask *excpus)
{
	struct cgroup_subsys_state *css;
	struct cpuset *sibling;
	int retval = 0;

	if (cpumask_empty(excpus))
		return 0;

	/*
	 * Remove exclusive CPUs from siblings
	 */
	/* 遍历兄弟并扣除其独占集合；RCU 只稳定遍历和对象寿命，不冻结字段内容。 */
	rcu_read_lock();
	cpuset_for_each_child(sibling, css, parent) {
		struct cpumask *sibling_xcpus;

		if (sibling == cs)
			continue;

		/*
		 * If exclusive_cpus is defined, effective_xcpus will always
		 * be a subset. Otherwise, effective_xcpus will only be set
		 * in a valid partition root.
		 */
		/*
		 * 兄弟显式配置 exclusive_cpus 时，effective_xcpus 必为其子集，使用前者才能
		 * 保留尚未获批但已声明的所有权；否则只有有效 partition 的 effective_xcpus
		 * 才代表真实占用。
		 */
		sibling_xcpus = cpumask_empty(sibling->exclusive_cpus)
			      ? sibling->effective_xcpus
			      : sibling->exclusive_cpus;

		if (cpumask_intersects(excpus, sibling_xcpus)) {
			cpumask_andnot(excpus, excpus, sibling_xcpus);
			retval++;
		}
	}
	rcu_read_unlock();

	return retval;
}

/*
 * compute_excpus - compute effective exclusive CPUs
 * @cs: cpuset
 * @xcpus: effective exclusive CPUs value to be set
 * Return: 0 if there is no sibling conflict, > 0 otherwise
 *
 * If exclusive_cpus isn't explicitly set , we have to scan the sibling cpusets
 * and exclude their exclusive_cpus or effective_xcpus as well.
 */
/*
 * compute_excpus() 计算真实 @cs 的 effective exclusive CPU。
 * 输出 @excpus 先受父 effective_xcpus 裁剪；未显式设置 exclusive_cpus 时再扣除
 * 兄弟隐式/显式独占集合。返回 0 表示无兄弟扣减，正数表示发生冲突裁剪；无发布。
 */
static int compute_excpus(struct cpuset *cs, struct cpumask *excpus)
{
	struct cpuset *parent = parent_cs(cs);

	cpumask_and(excpus, user_xcpus(cs), parent->effective_xcpus);

	if (!cpumask_empty(cs->exclusive_cpus))
		return 0;

	return rm_siblings_excl_cpus(parent, cs, excpus);
}

/*
 * compute_trialcs_excpus - Compute effective exclusive CPUs for a trial cpuset
 * @trialcs: The trial cpuset containing the proposed new configuration
 * @cs: The original cpuset that the trial configuration is based on
 * Return: 0 if successful with no sibling conflict, >0 if a conflict is found
 *
 * Computes the effective_xcpus for a trial configuration. @cs is provided to represent
 * the real cs.
 */
/*
 * compute_trialcs_excpus() 为未发布 @trialcs 预计算 effective_xcpus。
 * @cs 提供真实层级身份，不能用 trial 地址遍历；member 的 cpus_allowed 不产生隐式
 * partition 独占，只有 exclusive_cpus 参与。结果写入 trial，返回兄弟冲突数量。
 */
static int compute_trialcs_excpus(struct cpuset *trialcs, struct cpuset *cs)
{
	struct cpuset *parent = parent_cs(trialcs);
	struct cpumask *excpus = trialcs->effective_xcpus;

	/* trialcs is member, cpuset.cpus has no impact to excpus */
	/* 普通 member 的 cpuset.cpus 只限制运行范围，不声明 partition 独占所有权。 */
	if (cs_is_member(cs))
		cpumask_and(excpus, trialcs->exclusive_cpus,
				parent->effective_xcpus);
	else
		cpumask_and(excpus, user_xcpus(trialcs), parent->effective_xcpus);

	return rm_siblings_excl_cpus(parent, cs, excpus);
}

/*
 * is_remote_partition()/is_local_partition() 对稳定借用 @cs 分类。
 * remote 位表示 CPU 直接来自 top_cpuset；local 必须同时是有效 partition 且该位清零。
 * 返回值不锁定状态，调用者仍需 cpuset_mutex/callback_lock 维持事务一致性。
 */
static inline bool is_remote_partition(struct cpuset *cs)
{
	return cs->remote_partition;
}

static inline bool is_local_partition(struct cpuset *cs)
{
	return is_partition_valid(cs) && !is_remote_partition(cs);
}

/*
 * remote_partition_enable - Enable current cpuset as a remote partition root
 * @cs: the cpuset to update
 * @new_prs: new partition_root_state
 * @tmp: temporary masks
 * Return: 0 if successful, errcode if error
 *
 * Enable the current cpuset to become a remote partition root taking CPUs
 * directly from the top cpuset. cpuset_mutex must be held by the caller.
 */
/*
 * remote_partition_enable() - 让非 partition 祖先链下的 @cs 直接从根取得 CPU。
 *
 * 调用者持 cpuset_mutex；@new_prs 为 ROOT/ISOLATED，@tmp 是事务私有工作区。
 * 函数依次检查 CAP_SYS_ADMIN、根池余量、active CPU、housekeeping 和层级限制；
 * 校验失败返回 PERR_*，不提交任何状态。成功时在 callback_lock 下从 top 池取走
 * CPU并发布 remote/effective_xcpus，随后更新 top 任务和兄弟层级，返回 0。
 */
static int remote_partition_enable(struct cpuset *cs, int new_prs,
				   struct tmpmasks *tmp)
{
	/*
	 * The user must have sysadmin privilege.
	 */
	/* 远程 partition 直接改变根级 CPU 所有权，只允许具备 CAP_SYS_ADMIN 的调用者。 */
	if (!capable(CAP_SYS_ADMIN))
		return PERR_ACCESS;

	/*
	 * The requested exclusive_cpus must not be allocated to other
	 * partitions and it can't use up all the root's effective_cpus.
	 *
	 * The effective_xcpus mask can contain offline CPUs, but there must
	 * be at least one or more online CPUs present before it can be enabled.
	 *
	 * Note that creating a remote partition with any local partition root
	 * above it or remote partition root underneath it is not allowed.
	 */
	/*
	 * 候选 exclusive_cpus 不能与既有 partition 重叠，也不能取光 top_cpuset 的
	 * effective CPU。掩码可保留离线 CPU 以支持将来上线，但启用时至少要含一个
	 * active CPU；祖先存在本地 partition 或后代存在远程 partition 的混合拓扑也被
	 * 禁止，否则 CPU 将同时受两条所有权链控制。
	 */
	compute_excpus(cs, tmp->new_cpus);
	WARN_ON_ONCE(cpumask_intersects(tmp->new_cpus, subpartitions_cpus));
	if (!cpumask_intersects(tmp->new_cpus, cpu_active_mask) ||
	    cpumask_subset(top_cpuset.effective_cpus, tmp->new_cpus))
		return PERR_INVCPUS;
	if (((new_prs == PRS_ISOLATED) &&
	     !isolated_cpus_can_update(tmp->new_cpus, NULL)) ||
	    prstate_housekeeping_conflict(new_prs, tmp->new_cpus))
		return PERR_HKEEPING;

	spin_lock_irq(&callback_lock);
	partition_xcpus_add(new_prs, NULL, tmp->new_cpus);
	cs->remote_partition = true;
	cpumask_copy(cs->effective_xcpus, tmp->new_cpus);
	spin_unlock_irq(&callback_lock);
	cpuset_force_rebuild();
	cs->prs_err = 0;

	/*
	 * Propagate changes in top_cpuset's effective_cpus down the hierarchy.
	 */
	/* 根池已经扣除新 partition 的 CPU，必须更新根任务并向所有受影响兄弟子树传播。 */
	cpuset_update_tasks_cpumask(&top_cpuset, tmp->new_cpus);
	update_sibling_cpumasks(&top_cpuset, NULL, tmp);
	return 0;
}

/*
 * remote_partition_disable - Remove current cpuset from remote partition list
 * @cs: the cpuset to update
 * @tmp: temporary masks
 *
 * The effective_cpus is also updated.
 *
 * cpuset_mutex must be held by the caller.
 */
/*
 * remote_partition_disable() 撤销远程 partition 并把 CPU 归还 top_cpuset。
 * @cs 必须当前为 remote，@tmp 为调用者工作区，入口持 cpuset_mutex。
 * callback_lock 内清 remote 位、归还全局集合并把状态转为 member 或保留负的期望
 * 状态；随后重算普通派生 mask、标记调度域并传播根池变化。无直接返回值。
 */
static void remote_partition_disable(struct cpuset *cs, struct tmpmasks *tmp)
{
	WARN_ON_ONCE(!is_remote_partition(cs));
	/*
	 * When a CPU is offlined, top_cpuset may end up with no available CPUs,
	 * which should clear subpartitions_cpus. We should not emit a warning for this
	 * scenario: the hierarchy is updated from top to bottom, so subpartitions_cpus
	 * may already be cleared when disabling the partition.
	 */
	/*
	 * CPU 下线可能先把 top_cpuset 清空并清除 subpartitions_cpus；层级按自顶向下修复，
	 * 因此稍后撤销子 partition 时看不到原集合是合法时序，不能为此误报 WARN。
	 */
	WARN_ON_ONCE(!cpumask_subset(cs->effective_xcpus, subpartitions_cpus) &&
		     !cpumask_empty(subpartitions_cpus));

	spin_lock_irq(&callback_lock);
	cs->remote_partition = false;
	partition_xcpus_del(cs->partition_root_state, NULL, cs->effective_xcpus);
	if (cs->prs_err)
		cs->partition_root_state = -cs->partition_root_state;
	else
		cs->partition_root_state = PRS_MEMBER;

	/* effective_xcpus may need to be changed */
	/* 撤销 remote 身份后按普通层级规则重算 effective_xcpus，再清理 partition 派生数据。 */
	compute_excpus(cs, cs->effective_xcpus);
	reset_partition_data(cs);
	spin_unlock_irq(&callback_lock);
	cpuset_force_rebuild();

	/*
	 * Propagate changes in top_cpuset's effective_cpus down the hierarchy.
	 */
	/* 归还后的根 CPU 池已经扩大，更新根任务亲和性并传播到可能继承根资源的兄弟。 */
	cpuset_update_tasks_cpumask(&top_cpuset, tmp->new_cpus);
	update_sibling_cpumasks(&top_cpuset, NULL, tmp);
}

/*
 * remote_cpus_update - cpus_exclusive change of remote partition
 * @cs: the cpuset to be updated
 * @xcpus: the new exclusive_cpus mask, if non-NULL
 * @excpus: the new effective_xcpus mask
 * @tmp: temporary masks
 *
 * top_cpuset and subpartitions_cpus will be updated or partition can be
 * invalidated.
 */
/*
 * remote_cpus_update() - 原子调整远程 partition 的请求和实际独占集合。
 *
 * @xcpus 可空，非空时是待提交的用户 exclusive_cpus；@excpus 是已预计算有效集合；
 * @tmp 保存相对旧 effective_xcpus 的增删差量。新增 CPU 要重新验证权限、根池余量
 * 和 HK；失败或结果为空会使整个 remote partition 失效并归还资源。
 * 成功路径先在 callback_lock 下更新全局池和 cs 字段，再传播 top/兄弟任务；
 * 无返回值，错误原因写入 prs_err 并通过 partition 状态对用户可见。
 */
static void remote_cpus_update(struct cpuset *cs, struct cpumask *xcpus,
			       struct cpumask *excpus, struct tmpmasks *tmp)
{
	bool adding, deleting;
	int prs = cs->partition_root_state;

	if (WARN_ON_ONCE(!is_remote_partition(cs)))
		return;

	WARN_ON_ONCE(!cpumask_subset(cs->effective_xcpus, subpartitions_cpus));

	if (cpumask_empty(excpus)) {
		cs->prs_err = PERR_CPUSEMPTY;
		goto invalidate;
	}

	adding   = cpumask_andnot(tmp->addmask, excpus, cs->effective_xcpus);
	deleting = cpumask_andnot(tmp->delmask, cs->effective_xcpus, excpus);

	/*
	 * Additions of remote CPUs is only allowed if those CPUs are
	 * not allocated to other partitions and there are effective_cpus
	 * left in the top cpuset.
	 */
	/*
	 * 新增远程 CPU 必须仍未分配给其他 partition，并且不能取光 top_cpuset；isolated
	 * 变更还要保证至少保留一个完整 housekeeping CPU。任一检查失败都转入 invalidate，
	 * 由统一撤销路径归还旧资源，而不是发布部分差量。
	 */
	if (adding) {
		WARN_ON_ONCE(cpumask_intersects(tmp->addmask, subpartitions_cpus));
		if (!capable(CAP_SYS_ADMIN))
			cs->prs_err = PERR_ACCESS;
		else if (cpumask_intersects(tmp->addmask, subpartitions_cpus) ||
			 cpumask_subset(top_cpuset.effective_cpus, tmp->addmask))
			cs->prs_err = PERR_NOCPUS;
		else if ((prs == PRS_ISOLATED) &&
			 !isolated_cpus_can_update(tmp->addmask, tmp->delmask))
			cs->prs_err = PERR_HKEEPING;
		if (cs->prs_err)
			goto invalidate;
	}

	spin_lock_irq(&callback_lock);
	if (adding)
		partition_xcpus_add(prs, NULL, tmp->addmask);
	if (deleting)
		partition_xcpus_del(prs, NULL, tmp->delmask);
	/*
	 * Need to update effective_xcpus and exclusive_cpus now as
	 * update_sibling_cpumasks() below may iterate back to the same cs.
	 */
	/*
	 * 必须在释放 callback_lock 前同时发布 effective_xcpus 与可选用户 exclusive_cpus；
	 * 后续兄弟传播可能再次遍历当前 cs，若字段分两阶段更新会观察到不一致快照。
	 */
	cpumask_copy(cs->effective_xcpus, excpus);
	if (xcpus)
		cpumask_copy(cs->exclusive_cpus, xcpus);
	spin_unlock_irq(&callback_lock);
	if (adding || deleting)
		cpuset_force_rebuild();

	/*
	 * Propagate changes in top_cpuset's effective_cpus down the hierarchy.
	 */
	/* 根池差量提交后再更新任务和兄弟子树；这些 helper 可能睡眠，不能置于自旋锁内。 */
	cpuset_update_tasks_cpumask(&top_cpuset, tmp->new_cpus);
	update_sibling_cpumasks(&top_cpuset, NULL, tmp);
	return;

invalidate:
	remote_partition_disable(cs, tmp);
}

/**
 * update_parent_effective_cpumask - update effective_cpus mask of parent cpuset
 * @cs:      The cpuset that requests change in partition root state
 * @cmd:     Partition root state change command
 * @newmask: Optional new cpumask for partcmd_update
 * @tmp:     Temporary addmask and delmask
 * Return:   0 or a partition root state error code
 *
 * For partcmd_enable*, the cpuset is being transformed from a non-partition
 * root to a partition root. The effective_xcpus (cpus_allowed if
 * effective_xcpus not set) mask of the given cpuset will be taken away from
 * parent's effective_cpus. The function will return 0 if all the CPUs listed
 * in effective_xcpus can be granted or an error code will be returned.
 *
 * For partcmd_disable, the cpuset is being transformed from a partition
 * root back to a non-partition root. Any CPUs in effective_xcpus will be
 * given back to parent's effective_cpus. 0 will always be returned.
 *
 * For partcmd_update, if the optional newmask is specified, the cpu list is
 * to be changed from effective_xcpus to newmask. Otherwise, effective_xcpus is
 * assumed to remain the same. The cpuset should either be a valid or invalid
 * partition root. The partition root state may change from valid to invalid
 * or vice versa. An error code will be returned if transitioning from
 * invalid to valid violates the exclusivity rule.
 *
 * For partcmd_invalidate, the current partition will be made invalid.
 *
 * The partcmd_enable* and partcmd_disable commands are used by
 * update_prstate(). An error code may be returned and the caller will check
 * for error.
 *
 * The partcmd_update command is used by update_cpumasks_hier() with newmask
 * NULL and update_cpumask() with newmask set. The partcmd_invalidate is used
 * by update_cpumask() with NULL newmask. In both cases, the callers won't
 * check for error and so partition_root_state and prs_err will be updated
 * directly.
 */
/*
 * update_parent_effective_cpumask() - 本地 partition CPU 所有权事务的核心提交器。
 *
 * @cs 是请求状态变化的在线子 cpuset；@cmd 指定启用、隔离启用、禁用、重算或失效；
 * @newmask 仅 update 时可非空，表示新的 effective_xcpus；@tmp 为事务工作区。
 * 调用者持 cpuset_mutex，函数可睡眠；callback_lock 只覆盖最终父池/全局集合写入。
 *
 * 前半段以父视角计算 addmask（子归还）与 delmask（子取走），校验父/子 populated
 * 状态、兄弟独占、active CPU 和 housekeeping。可恢复的外部变化会写 prs_err 并
 * 在有效/无效状态间转换；显式 enable 的硬错误直接返回 PERR_* 且不发布。
 * 提交后更新父任务、受影响兄弟、负载均衡和 kernfs 通知。返回 0 表示事务已处理，
 * 包括 update 命令把 partition 标记为 invalid 的可观察结果。
 */
static int update_parent_effective_cpumask(struct cpuset *cs, int cmd,
					   struct cpumask *newmask,
					   struct tmpmasks *tmp)
{
	struct cpuset *parent = parent_cs(cs);
	int adding;	/* Adding cpus to parent's effective_cpus	*/
	/* adding 表示本事务会把 tmp->addmask 中的 CPU 从子级归还父级。 */
	int deleting;	/* Deleting cpus from parent's effective_cpus	*/
	/* deleting 表示会把 tmp->delmask 中的 CPU 从父级划给子级。 */
	int old_prs, new_prs;
	int part_error = PERR_NONE;	/* Partition error? */
	/* part_error 保存要向用户发布的 PERR_*，PERR_NONE 表示候选状态有效。 */
	struct cpumask *xcpus = user_xcpus(cs);
	int parent_prs = parent->partition_root_state;
	bool nocpu;

	lockdep_assert_cpuset_lock_held();
	WARN_ON_ONCE(is_remote_partition(cs));	/* For local partition only */
	/* 本 helper 只处理本地 partition；remote 路径必须由 remote_cpus_update() 提交。 */

	/*
	 * new_prs will only be changed for the partcmd_update and
	 * partcmd_invalidate commands.
	 */
	/*
	 * 先把旧状态作为默认新状态；只有 update/invalidate 或显式 enable/disable 分支才
	 * 改写 new_prs，确保无实际差量时可以安全快速返回。
	 */
	adding = deleting = false;
	old_prs = new_prs = cs->partition_root_state;

	if (cmd == partcmd_invalidate) {
		if (is_partition_invalid(cs))
			return 0;

		/*
		 * Make the current partition invalid.
		 */
		/*
		 * 强制失效时，若父 partition 仍有效，就把当前子级已占用且父级可接收的 CPU
		 * 记入归还集合；正状态取负保留用户期望的普通/隔离类型。
		 */
		if (is_partition_valid(parent))
			adding = cpumask_and(tmp->addmask,
					     cs->effective_xcpus,
					     parent->effective_xcpus);
		if (old_prs > 0)
			new_prs = -old_prs;

		goto write_error;
	}

	/*
	 * The parent must be a partition root.
	 * The new cpumask, if present, or the current cpus_allowed must
	 * not be empty.
	 */
	/*
	 * 本地 partition 必须挂在有效 partition root 下；没有显式 newmask 时，用户请求
	 * 集合也不能为空。这里失败尚未发布任何状态，可直接把精确 PERR_* 返回调用者。
	 */
	if (!is_partition_valid(parent)) {
		return is_partition_invalid(parent)
		       ? PERR_INVPARENT : PERR_NOTPART;
	}
	if (!newmask && xcpus_empty(cs))
		return PERR_CPUSEMPTY;

	nocpu = tasks_nocpu_error(parent, cs, xcpus);

	if ((cmd == partcmd_enable) || (cmd == partcmd_enablei)) {
		/*
		 * Need to call compute_excpus() in case
		 * exclusive_cpus not set. Sibling conflict should only happen
		 * if exclusive_cpus isn't set.
		 */
		/*
		 * enable 允许用户省略 exclusive_cpus，此时必须现场从 cpus_allowed 推导并扣除
		 * 兄弟所有权。显式 exclusive 不应再发生兄弟裁剪，若发生说明前置校验失效。
		 */
		xcpus = tmp->delmask;
		if (compute_excpus(cs, xcpus))
			WARN_ON_ONCE(!cpumask_empty(cs->exclusive_cpus));
		new_prs = (cmd == partcmd_enable) ? PRS_ROOT : PRS_ISOLATED;

		/*
		 * Enabling partition root is not allowed if its
		 * effective_xcpus is empty.
		 */
		/* 空 effective_xcpus 无法建立实际 CPU 边界，启用请求以 PERR_INVCPUS 拒绝。 */
		if (cpumask_empty(xcpus))
			return PERR_INVCPUS;

		if (prstate_housekeeping_conflict(new_prs, xcpus))
			return PERR_HKEEPING;

		if ((new_prs == PRS_ISOLATED) && (new_prs != parent_prs) &&
		    !isolated_cpus_can_update(xcpus, NULL))
			return PERR_HKEEPING;

		if (tasks_nocpu_error(parent, cs, xcpus))
			return PERR_NOCPUS;

		/*
		 * This function will only be called when all the preliminary
		 * checks have passed. At this point, the following condition
		 * should hold.
		 *
		 * (cs->effective_xcpus & cpu_active_mask) ⊆ parent->effective_cpus
		 *
		 * Warn if it is not the case.
		 */
		/*
		 * 前置检查后，候选集合中的 active CPU 必须全部仍在父 effective_cpus 中；这是
		 * 随后从父池删除 CPU 的安全前提，WARN 用来捕获层级或 hotplug 同步漏洞。
		 */
		cpumask_and(tmp->new_cpus, xcpus, cpu_active_mask);
		WARN_ON_ONCE(!cpumask_subset(tmp->new_cpus, parent->effective_cpus));

		deleting = true;
	} else if (cmd == partcmd_disable) {
		/*
		 * May need to add cpus back to parent's effective_cpus
		 * (and maybe removed from subpartitions_cpus/isolated_cpus)
		 * for valid partition root. xcpus may contain CPUs that
		 * shouldn't be removed from the two global cpumasks.
		 */
		/*
		 * disable 只为当前仍有效的 partition 归还 effective_xcpus；无效 root 的全局
		 * 集合可能早已在失效时修正，不能再次盲目从 subpartitions/isolated 中删除。
		 */
		if (is_partition_valid(cs)) {
			cpumask_copy(tmp->addmask, cs->effective_xcpus);
			adding = true;
		}
		new_prs = PRS_MEMBER;
	} else if (newmask) {
		/*
		 * Empty cpumask is not allowed
		 */
		/* 显式更新为全空集合没有可恢复语义，记录 PERR_CPUSEMPTY 后走统一状态提交。 */
		if (cpumask_empty(newmask)) {
			part_error = PERR_CPUSEMPTY;
			goto write_error;
		}

		/* Check newmask again, whether cpus are available for parent/cs */
		/* 用新集合重新判断父或子是否会让 populated cpuset 失去全部 active CPU。 */
		nocpu |= tasks_nocpu_error(parent, cs, newmask);

		/*
		 * partcmd_update with newmask:
		 *
		 * Compute add/delete mask to/from effective_cpus
		 *
		 * For valid partition:
		 *   addmask = effective_xcpus & ~newmask
		 *			      & parent->effective_xcpus
		 *   delmask = newmask & ~effective_xcpus
		 *		       & parent->effective_xcpus
		 *
		 * For invalid partition:
		 *   delmask = newmask & parent->effective_xcpus
		 *   The partition may become valid soon.
		 */
		/*
		 * 显式 newmask 更新以父视角计算差量：addmask 是旧集合中不再由子占有、可归还
		 * 父级的 CPU；delmask 是新增且当前仍在父池中的 CPU。无效 partition 没有可归还
		 * 的已发布所有权，只计算可能使其恢复有效的 delmask。
		 */
		if (is_partition_invalid(cs)) {
			adding = false;
			deleting = cpumask_and(tmp->delmask,
					newmask, parent->effective_xcpus);
		} else {
			cpumask_andnot(tmp->addmask, cs->effective_xcpus, newmask);
			adding = cpumask_and(tmp->addmask, tmp->addmask,
					     parent->effective_xcpus);

			cpumask_andnot(tmp->delmask, newmask, cs->effective_xcpus);
			deleting = cpumask_and(tmp->delmask, tmp->delmask,
					       parent->effective_xcpus);
		}

		/*
		 * TBD: Invalidate a currently valid child root partition may
		 * still break isolated_cpus_can_update() rule if parent is an
		 * isolated partition.
		 */
		/*
		 * 待办限制：有效子 root 失效后，若父本身 isolated，HK 完整性仍可能被破坏。
		 * 当前代码按父子 partition 类型换算 add/del 对 isolated 集合的方向并执行检查。
		 */
		if (is_partition_valid(cs) && (old_prs != parent_prs)) {
			if ((parent_prs == PRS_ROOT) &&
			    /* Adding to parent means removing isolated CPUs */
			    /* 归还普通父级等价于从全局 isolated 集合删除这些 CPU。 */
			    !isolated_cpus_can_update(tmp->delmask, tmp->addmask))
				part_error = PERR_HKEEPING;
			if ((parent_prs == PRS_ISOLATED) &&
			    /* Adding to parent means adding isolated CPUs */
			    /* 归还 isolated 父级等价于把这些 CPU 加回全局 isolated 集合。 */
			    !isolated_cpus_can_update(tmp->addmask, tmp->delmask))
				part_error = PERR_HKEEPING;
		}

		/*
		 * The new CPUs to be removed from parent's effective CPUs
		 * must be present.
		 */
		/* 从父级划出的新增 active CPU 必须确实属于父 effective_cpus，否则不得提交。 */
		if (deleting) {
			cpumask_and(tmp->new_cpus, tmp->delmask, cpu_active_mask);
			WARN_ON_ONCE(!cpumask_subset(tmp->new_cpus, parent->effective_cpus));
		}

		/*
		 * Make partition invalid if parent's effective_cpus could
		 * become empty and there are tasks in the parent.
		 */
		/*
		 * 若父级有任务且差量会取光 active CPU，候选 partition 转为无效：取消向子新增
		 * CPU，并把子原有可归还集合记入 addmask，以恢复父任务的可运行性。
		 */
		if (nocpu && (!adding ||
		    !cpumask_intersects(tmp->addmask, cpu_active_mask))) {
			part_error = PERR_NOCPUS;
			deleting = false;
			adding = cpumask_and(tmp->addmask,
					     cs->effective_xcpus, parent->effective_xcpus);
		}
	} else {
		/*
		 * partcmd_update w/o newmask
		 *
		 * delmask = effective_xcpus & parent->effective_cpus
		 *
		 * This can be called from:
		 * 1) update_cpumasks_hier()
		 * 2) cpuset_hotplug_update_tasks()
		 *
		 * Check to see if it can be transitioned from valid to
		 * invalid partition or vice versa.
		 *
		 * A partition error happens when parent has tasks and all
		 * its effective CPUs will have to be distributed out.
		 */
		/*
		 * 无 newmask 的 update 来自层级传播或 hotplug：用户集合不变，只重新判断现有
		 * effective_xcpus 能否继续由父级提供。父有任务却会被取光时使子失效并归还旧
		 * CPU；无效子重新满足父集合时，还必须重新扫描兄弟独占性后才能恢复有效。
		 */
		if (nocpu) {
			part_error = PERR_NOCPUS;
			if (is_partition_valid(cs))
				adding = cpumask_and(tmp->addmask,
						     cs->effective_xcpus,
						     parent->effective_xcpus);
		} else if (is_partition_invalid(cs) && !cpumask_empty(xcpus) &&
			   cpumask_subset(xcpus, parent->effective_xcpus)) {
			struct cgroup_subsys_state *css;
			struct cpuset *child;
			bool exclusive = true;

			/*
			 * Convert invalid partition to valid has to
			 * pass the cpu exclusivity test.
			 */
			/*
			 * 负状态恢复为正状态会重新取得 CPU 所有权，因此必须在 RCU 下逐个验证兄弟
			 * exclusivity；只有全部互斥才生成从父池删除的 delmask。
			 */
			rcu_read_lock();
			cpuset_for_each_child(child, css, parent) {
				if (child == cs)
					continue;
				if (!cpusets_are_exclusive(cs, child)) {
					exclusive = false;
					break;
				}
			}
			rcu_read_unlock();
			if (exclusive)
				deleting = cpumask_and(tmp->delmask,
						xcpus, parent->effective_cpus);
			else
				part_error = PERR_NOTEXCL;
		}
	}

write_error:
	if (part_error)
		WRITE_ONCE(cs->prs_err, part_error);

	if (cmd == partcmd_update) {
		/*
		 * Check for possible transition between valid and invalid
		 * partition root.
		 */
		/* 根据 part_error 在正/负同类状态间切换，保留普通与 isolated 类型信息。 */
		switch (cs->partition_root_state) {
		case PRS_ROOT:
		case PRS_ISOLATED:
			if (part_error)
				new_prs = -old_prs;
			break;
		case PRS_INVALID_ROOT:
		case PRS_INVALID_ISOLATED:
			if (!part_error)
				new_prs = -old_prs;
			break;
		}
	}

	if (!adding && !deleting && (new_prs == old_prs))
		return 0;

	/*
	 * Transitioning between invalid to valid or vice versa may require
	 * changing CS_CPU_EXCLUSIVE. In the case of partcmd_update,
	 * validate_change() has already been successfully called and
	 * CPU lists in cs haven't been updated yet. So defer it to later.
	 */
	/*
	 * 正负状态转换可能要求同步 CS_CPU_EXCLUSIVE。普通命令在发布 CPU 差量前完成该
	 * 校验；partcmd_update 已由 validate_change() 检过候选列表，而真实 cs 尚未写入，
	 * 因此把 flag 更新延后到 CPU 状态提交之后。
	 */
	if ((old_prs != new_prs) && (cmd != partcmd_update))  {
		int err = update_partition_exclusive_flag(cs, new_prs);

		if (err)
			return err;
	}

	/*
	 * Change the parent's effective_cpus & effective_xcpus (top cpuset
	 * only).
	 *
	 * Newly added CPUs will be removed from effective_cpus and
	 * newly deleted ones will be added back to effective_cpus.
	 */
	/*
	 * callback_lock 是不可分割提交边界：先发布新 partition 状态，再把 delmask 从父
	 * effective 池移出、把 addmask 归还。top_cpuset 还会同步 effective_xcpus，读者
	 * 因而不会看到状态与 CPU 所有权不匹配的中间值。
	 */
	spin_lock_irq(&callback_lock);
	if (old_prs != new_prs)
		cs->partition_root_state = new_prs;

	/*
	 * Adding to parent's effective_cpus means deletion CPUs from cs
	 * and vice versa.
	 */
	/* addmask/delmask 均以父视角命名：父增加就是子删除，父删除就是子新增。 */
	if (adding)
		partition_xcpus_del(old_prs, parent, tmp->addmask);
	if (deleting)
		partition_xcpus_add(new_prs, parent, tmp->delmask);

	spin_unlock_irq(&callback_lock);

	if ((old_prs != new_prs) && (cmd == partcmd_update))
		update_partition_exclusive_flag(cs, new_prs);

	if (adding || deleting) {
		cpuset_update_tasks_cpumask(parent, tmp->addmask);
		update_sibling_cpumasks(parent, cs, tmp);
	}

	/*
	 * For partcmd_update without newmask, it is being called from
	 * cpuset_handle_hotplug(). Update the load balance flag and
	 * scheduling domain accordingly.
	 */
	/*
	 * 无 newmask 的 update 代表 hotplug/层级外因，状态变化还要重新计算负载均衡位并
	 * 标记 sched_domain；显式用户写入的对应处理已由外层更新路径完成。
	 */
	if ((cmd == partcmd_update) && !newmask)
		update_partition_sd_lb(cs, old_prs);

	notify_partition_change(cs, old_prs);
	return 0;
}

/**
 * compute_partition_effective_cpumask - compute effective_cpus for partition
 * @cs: partition root cpuset
 * @new_ecpus: previously computed effective_cpus to be updated
 *
 * Compute the effective_cpus of a partition root by scanning effective_xcpus
 * of child partition roots and excluding their effective_xcpus.
 *
 * This has the side effect of invalidating valid child partition roots,
 * if necessary. Since it is called from either cpuset_hotplug_update_tasks()
 * or update_cpumasks_hier() where parent and children are modified
 * successively, we don't need to call update_parent_effective_cpumask()
 * and the child's effective_cpus will be updated in later iterations.
 *
 * Note that rcu_read_lock() is assumed to be held.
 */
/*
 * compute_partition_effective_cpumask() - 计算 partition 留给自身普通成员的 CPU。
 *
 * @cs 是 partition root，@new_ecpus 是输入输出工作区。先求自身获批独占集合，再
 * 扣除有效子 partition 的 effective_xcpus。子集合越界或会取光 populated 父的
 * CPU 时，在 callback_lock 下把子状态取负并通知。无直接返回值；除输出 mask 外，
 * 可能使子 partition 失效，其 effective_cpus 留给随后的自顶向下迭代修正。
 */
static void compute_partition_effective_cpumask(struct cpuset *cs,
						struct cpumask *new_ecpus)
{
	struct cgroup_subsys_state *css;
	struct cpuset *child;
	bool populated = partition_is_populated(cs, NULL);

	/*
	 * Check child partition roots to see if they should be
	 * invalidated when
	 *  1) child effective_xcpus not a subset of new
	 *     excluisve_cpus
	 *  2) All the effective_cpus will be used up and cp
	 *     has tasks
	 */
	/*
	 * 先从父级获批独占集合求当前可用 active CPU，再检查每个有效子 partition：子级
	 * effective_xcpus 若已不再是父级所有权子集，或会取光一个 populated 父级的全部
	 * CPU，就必须失效；其余子级的 CPU 从父级成员可用集合中扣除。
	 */
	compute_excpus(cs, new_ecpus);
	cpumask_and(new_ecpus, new_ecpus, cpu_active_mask);

	rcu_read_lock();
	cpuset_for_each_child(child, css, cs) {
		if (!is_partition_valid(child))
			continue;

		/*
		 * There shouldn't be a remote partition underneath another
		 * partition root.
		 */
		/* partition root 下不应再出现直接从 top 取 CPU 的 remote 子级，否则所有权链重复。 */
		WARN_ON_ONCE(is_remote_partition(child));
		child->prs_err = 0;
		if (!cpumask_subset(child->effective_xcpus,
				    cs->effective_xcpus))
			child->prs_err = PERR_INVCPUS;
		else if (populated &&
			 cpumask_subset(new_ecpus, child->effective_xcpus))
			child->prs_err = PERR_NOCPUS;

		if (child->prs_err) {
			int old_prs = child->partition_root_state;

			/*
			 * Invalidate child partition
			 */
			/* 在 callback_lock 下把子状态取负并归还派生资源，再通知用户可见状态变化。 */
			spin_lock_irq(&callback_lock);
			make_partition_invalid(child);
			spin_unlock_irq(&callback_lock);
			notify_partition_change(child, old_prs);
			continue;
		}
		cpumask_andnot(new_ecpus, new_ecpus,
			       child->effective_xcpus);
	}
	rcu_read_unlock();
}

/*
 * update_cpumasks_hier - Update effective cpumasks and tasks in the subtree
 * @cs:  the cpuset to consider
 * @tmp: temp variables for calculating effective_cpus & partition setup
 * @force: don't skip any descendant cpusets if set
 *
 * When configured cpumask is changed, the effective cpumasks of this cpuset
 * and all its descendants need to be updated.
 *
 * On legacy hierarchy, effective_cpus will be the same with cpu_allowed.
 *
 * Called with cpuset_mutex held
 */
/*
 * update_cpumasks_hier() - 自顶向下重算子树 CPU 派生状态并更新任务亲和性。
 *
 * @cs 是传播起点，@tmp 为事务私有工作区，@force 禁止“值相同即剪枝”的优化。
 * 调用者持 cpuset_mutex。遍历在 RCU 下进行；进入可睡眠 helper 前以
 * css_tryget_online() 固定对象并退出 RCU，完成后归还引用。
 * remote/local partition 分别经过自己的所有权事务；每个节点的 effective mask、
 * 状态和独占派生值在 callback_lock 下成组发布，再于锁外更新任务与调度域脏位。
 * 无直接返回值，返回时所有在线后代与新的父资源约束一致。
 */
static void update_cpumasks_hier(struct cpuset *cs, struct tmpmasks *tmp,
				 bool force)
{
	struct cpuset *cp;
	struct cgroup_subsys_state *pos_css;
	int old_prs, new_prs;

	rcu_read_lock();
	cpuset_for_each_descendant_pre(cp, pos_css, cs) {
		struct cpuset *parent = parent_cs(cp);
		bool remote = is_remote_partition(cp);
		bool update_parent = false;

		old_prs = new_prs = cp->partition_root_state;

		/*
		 * For child remote partition root (!= cs), we need to call
		 * remote_cpus_update() if effective_xcpus will be changed.
		 * Otherwise, we can skip the whole subtree.
		 *
		 * remote_cpus_update() will reuse tmp->new_cpus only after
		 * its value is being processed.
		 */
		/*
		 * 对传播起点以下的 remote root，先重算从 top 获批的集合；未变化即可连同其
		 * 子树剪枝。发生变化时退出 RCU，因为 remote_cpus_update() 会取锁并可能睡眠；
		 * 该 helper 消费完 tmp->new_cpus 后才会复用工作区。
		 */
		if (remote && (cp != cs)) {
			compute_excpus(cp, tmp->new_cpus);
			if (cpumask_equal(cp->effective_xcpus, tmp->new_cpus)) {
				pos_css = css_rightmost_descendant(pos_css);
				continue;
			}
			rcu_read_unlock();
			remote_cpus_update(cp, NULL, tmp->new_cpus, tmp);
			rcu_read_lock();

			/* Remote partition may be invalidated */
			/* remote 更新可能把 partition 置为无效，重新读取状态决定后续计算规则。 */
			new_prs = cp->partition_root_state;
			remote = (new_prs == old_prs);
		}

		if (remote || (is_partition_valid(parent) && is_partition_valid(cp)))
			compute_partition_effective_cpumask(cp, tmp->new_cpus);
		else
			compute_effective_cpumask(tmp->new_cpus, cp, parent);

		if (remote)
			goto get_css;	/* Ready to update cpuset data */
			/* remote 路径的候选掩码已算好，直接进入取得在线引用与发布阶段。 */

		/*
		 * A partition with no effective_cpus is allowed as long as
		 * there is no task associated with it. Call
		 * update_parent_effective_cpumask() to check it.
		 */
		/*
		 * partition 自身成员集合可以为空，但前提是没有任务依赖它。让父子所有权事务
		 * 重新检查 populated 条件，并在必要时使 partition 失效、归还 CPU。
		 */
		if (is_partition_valid(cp) && cpumask_empty(tmp->new_cpus)) {
			update_parent = true;
			goto update_parent_effective;
		}

		/*
		 * If it becomes empty, inherit the effective mask of the
		 * parent, which is guaranteed to have some CPUs unless
		 * it is a partition root that has explicitly distributed
		 * out all its CPUs.
		 */
		/*
		 * v2 普通非 remote 节点若交集为空，就继承父 effective_cpus 以保证层级成员仍可
		 * 运行；唯一可能让父也为空的是主动把全部 CPU 分给子级的 partition root。
		 */
		if (is_in_v2_mode() && !remote && cpumask_empty(tmp->new_cpus))
			cpumask_copy(tmp->new_cpus, parent->effective_cpus);

		/*
		 * Skip the whole subtree if
		 * 1) the cpumask remains the same,
		 * 2) has no partition root state,
		 * 3) force flag not set, and
		 * 4) for v2 load balance state same as its parent.
		 */
		/*
		 * 普通节点的 CPU 掩码与负载均衡继承状态均未变化，且调用者未强制刷新时，后代
		 * 的输入也不会变化，可把遍历位置跳到最右后代以剪掉整棵子树。
		 */
		if (!cp->partition_root_state && !force &&
		    cpumask_equal(tmp->new_cpus, cp->effective_cpus) &&
		    (!cpuset_v2() ||
		    (is_sched_load_balance(parent) == is_sched_load_balance(cp)))) {
			pos_css = css_rightmost_descendant(pos_css);
			continue;
		}

update_parent_effective:
		/*
		 * update_parent_effective_cpumask() should have been called
		 * for cs already in update_cpumask(). We should also call
		 * cpuset_update_tasks_cpumask() again for tasks in the parent
		 * cpuset if the parent's effective_cpus changes.
		 */
		/*
		 * 传播起点已由 update_cpumask() 处理父级差量；其他带 partition 状态的后代仍需
		 * 执行本地所有权事务。若父池改变，该 helper 也会同步更新父任务亲和性。
		 */
		if ((cp != cs) && old_prs) {
			switch (parent->partition_root_state) {
			case PRS_ROOT:
			case PRS_ISOLATED:
				update_parent = true;
				break;

			default:
				/*
				 * When parent is not a partition root or is
				 * invalid, child partition roots become
				 * invalid too.
				 */
				/*
				 * 父级不是有效 partition root 时，子级失去合法 CPU 授权链；保留类型并把
				 * 状态取负，同时发布“父无效”或“父非 partition”的精确原因。
				 */
				if (is_partition_valid(cp))
					new_prs = -cp->partition_root_state;
				WRITE_ONCE(cp->prs_err,
					   is_partition_invalid(parent)
					   ? PERR_INVPARENT : PERR_NOTPART);
				break;
			}
		}
get_css:
		if (!css_tryget_online(&cp->css))
			continue;
		rcu_read_unlock();

		if (update_parent) {
			update_parent_effective_cpumask(cp, partcmd_update, NULL, tmp);
			/*
			 * The cpuset partition_root_state may become
			 * invalid. Capture it.
			 */
			/* 父级事务可能使当前 partition 正负翻转，发布前必须捕获其最终状态。 */
			new_prs = cp->partition_root_state;
		}

		spin_lock_irq(&callback_lock);
		cpumask_copy(cp->effective_cpus, tmp->new_cpus);
		cp->partition_root_state = new_prs;
		/*
		 * Need to compute effective_xcpus if either exclusive_cpus
		 * is non-empty or it is a valid partition root.
		 */
		/*
		 * 显式 exclusive_cpus 或有效 partition 都需要维护 effective_xcpus；非正状态
		 * 随后清除只对有效 partition 有意义的派生数据。整个字段组在 callback_lock 下
		 * 成组发布给并发亲和性读者。
		 */
		if ((new_prs > 0) || !cpumask_empty(cp->exclusive_cpus))
			compute_excpus(cp, cp->effective_xcpus);
		if (new_prs <= 0)
			reset_partition_data(cp);
		spin_unlock_irq(&callback_lock);

		notify_partition_change(cp, old_prs);

		WARN_ON(!is_in_v2_mode() &&
			!cpumask_equal(cp->cpus_allowed, cp->effective_cpus));

		cpuset_update_tasks_cpumask(cp, tmp->new_cpus);

		/*
		 * On default hierarchy, inherit the CS_SCHED_LOAD_BALANCE
		 * from parent if current cpuset isn't a valid partition root
		 * and their load balance states differ.
		 */
		/*
		 * v2 中只有有效 partition root 能建立自己的负载均衡边界；普通或无效节点必须
		 * 继承父标志，确保层级传播后调度域语义连续。
		 */
		if (cpuset_v2() && !is_partition_valid(cp) &&
		    (is_sched_load_balance(parent) != is_sched_load_balance(cp))) {
			if (is_sched_load_balance(parent))
				set_bit(CS_SCHED_LOAD_BALANCE, &cp->flags);
			else
				clear_bit(CS_SCHED_LOAD_BALANCE, &cp->flags);
		}

		/*
		 * On legacy hierarchy, if the effective cpumask of any non-
		 * empty cpuset is changed, we need to rebuild sched domains.
		 * On default hierarchy, the cpuset needs to be a partition
		 * root as well.
		 */
		/*
		 * v1 任一非空、启用负载均衡节点的 effective CPU 改变都影响 sched_domain；v2
		 * 只有有效 partition root 才定义边界。这里只置脏位，由事务尾部合并重建。
		 */
		if (!cpumask_empty(cp->cpus_allowed) &&
		    is_sched_load_balance(cp) &&
		   (!cpuset_v2() || is_partition_valid(cp)))
			cpuset_force_rebuild();

		rcu_read_lock();
		css_put(&cp->css);
	}
	rcu_read_unlock();
}

/**
 * update_sibling_cpumasks - Update siblings cpumasks
 * @parent:  Parent cpuset
 * @cs:      Current cpuset
 * @tmp:     Temp variables
 */
/*
 * 父 partition 的 effective CPU 池发生变化后，本函数只传播可能继承该变化的普通
 * 兄弟；@cs 是触发者且可为 NULL，@tmp 是调用者独占工作区。调用者持
 * cpuset_mutex；需要进入可睡眠的子树传播前，以 online css 引用替代 RCU 存期。
 * 无直接返回值，返回时有效兄弟 partition 保持独立，其余变化子树已完成修复。
 */
static void update_sibling_cpumasks(struct cpuset *parent, struct cpuset *cs,
				    struct tmpmasks *tmp)
{
	struct cpuset *sibling;
	struct cgroup_subsys_state *pos_css;

	lockdep_assert_cpuset_lock_held();

	/*
	 * Check all its siblings and call update_cpumasks_hier()
	 * if their effective_cpus will need to be changed.
	 *
	 * It is possible a change in parent's effective_cpus
	 * due to a change in a child partition's effective_xcpus will impact
	 * its siblings even if they do not inherit parent's effective_cpus
	 * directly. It should not impact valid partition.
	 *
	 * The update_cpumasks_hier() function may sleep. So we have to
	 * release the RCU read lock before calling it.
	 */
	/*
	 * 子 partition 改变父 effective 池时，即使普通兄弟没有直接继承用户 cpus，也可能
	 * 因父交集改变而需要传播；有效 partition 拥有独立边界，可跳过。先以 online css
	 * 引用稳定 sibling，再退出 RCU 调用可能睡眠的 update_cpumasks_hier()，返回后恢复
	 * 遍历并归还引用。
	 */
	rcu_read_lock();
	cpuset_for_each_child(sibling, pos_css, parent) {
		if (sibling == cs || is_partition_valid(sibling))
			continue;

		compute_effective_cpumask(tmp->new_cpus, sibling,
					  parent);
		if (cpumask_equal(tmp->new_cpus, sibling->effective_cpus))
			continue;

		if (!css_tryget_online(&sibling->css))
			continue;

		rcu_read_unlock();
		update_cpumasks_hier(sibling, tmp, false);
		rcu_read_lock();
		css_put(&sibling->css);
	}
	rcu_read_unlock();
}

/*
 * parse_cpuset_cpulist() 把用户 cpulist 文本解析到 @out_mask，并限制为根配置
 * cpus_allowed 的子集。@buf 为 NUL 结尾借用字符串，@out_mask 为输出工作区。
 * 成功返回 0；语法错误透传负 errno，越界返回 -EINVAL；不发布任何状态。
 */
static int parse_cpuset_cpulist(const char *buf, struct cpumask *out_mask)
{
	int retval;

	retval = cpulist_parse(buf, out_mask);
	if (retval < 0)
		return retval;
	if (!cpumask_subset(out_mask, top_cpuset.cpus_allowed))
		return -EINVAL;

	return 0;
}

/**
 * validate_partition - Validate a cpuset partition configuration
 * @cs: The cpuset to validate
 * @trialcs: The trial cpuset containing proposed configuration changes
 *
 * If any validation check fails, the appropriate error code is set in the
 * cpuset's prs_err field.
 *
 * Return: PRS error code (0 if valid, non-zero error code if invalid)
 */
/*
 * validate_partition() 对已算好 effective_xcpus 的 @trialcs 做专项校验。
 * @cs 仅提供真实父关系，trial 是未发布借用副本。member 恒成功；partition 要求
 * 获批 CPU 非空、不冲突 housekeeping，且不会让 populated 的父或子失去全部 CPU。
 * 返回 PERR_NONE 或精确 PERR_*，不直接修改真实对象。
 */
static enum prs_errcode validate_partition(struct cpuset *cs, struct cpuset *trialcs)
{
	struct cpuset *parent = parent_cs(cs);

	/* member 不建立 CPU 所有权边界，partition 专项约束均不适用。 */
	if (cs_is_member(trialcs))
		return PERR_NONE;

	/* 依次验证实际获批集合、housekeeping 硬边界和 populated 父子非空不变量。 */
	if (cpumask_empty(trialcs->effective_xcpus))
		return PERR_INVCPUS;

	if (prstate_housekeeping_conflict(trialcs->partition_root_state,
					  trialcs->effective_xcpus))
		return PERR_HKEEPING;

	if (tasks_nocpu_error(parent, cs, trialcs->effective_xcpus))
		return PERR_NOCPUS;

	return PERR_NONE;
}

/**
 * partition_cpus_change - Handle partition state changes due to CPU mask updates
 * @cs: The target cpuset being modified
 * @trialcs: The trial cpuset containing proposed configuration changes
 * @tmp: Temporary masks for intermediate calculations
 *
 * This function handles partition state transitions triggered by CPU mask changes.
 * CPU modifications may cause a partition to be disabled or require state updates.
 */
/*
 * partition_cpus_change() 把 CPU mask 改动转换为 partition 资源事务。
 * @trialcs 已通过通用校验，@tmp 为工作区。member 无动作；专项失败会记录 prs_err
 * 并按 remote/local 路径使真实 partition 失效；成功则调整获批 CPU。
 * 无直接返回值，调用者随后提交用户 mask 并传播层级。
 */
static void partition_cpus_change(struct cpuset *cs, struct cpuset *trialcs,
					struct tmpmasks *tmp)
{
	enum prs_errcode prs_err;

	/* 普通 member 的 CPU mask 变化不涉及 partition 所有权，由外层直接提交与传播。 */
	if (cs_is_member(cs))
		return;

	/* 先在未发布 trial 上做专项校验；错误同时写入候选和真实状态供失效路径观察。 */
	prs_err = validate_partition(cs, trialcs);
	if (prs_err)
		trialcs->prs_err = cs->prs_err = prs_err;

	/*
	 * remote 与 local 使用不同资源池事务：remote 直接调整 top 池，local 调整父
	 * partition。失败统一撤销/失效，成功提交候选 effective_xcpus 差量。
	 */
	if (is_remote_partition(cs)) {
		if (trialcs->prs_err)
			remote_partition_disable(cs, tmp);
		else
			remote_cpus_update(cs, trialcs->exclusive_cpus,
					   trialcs->effective_xcpus, tmp);
	} else {
		if (trialcs->prs_err)
			update_parent_effective_cpumask(cs, partcmd_invalidate,
							NULL, tmp);
		else
			update_parent_effective_cpumask(cs, partcmd_update,
							trialcs->effective_xcpus, tmp);
	}
}

/**
 * update_cpumask - update the cpus_allowed mask of a cpuset and all tasks in it
 * @cs: the cpuset to consider
 * @trialcs: trial cpuset
 * @buf: buffer of cpu numbers written to this cpuset
 */
/*
 * update_cpumask() - 处理 cpuset.cpus 写入的可回滚事务。
 *
 * @cs 是在线目标，@trialcs 是未发布副本，@buf 是用户文本；调用者持完整锁组。
 * 解析、候选 effective_xcpus、结构校验和临时分配全部发生在发布前。随后处理
 * partition CPU 所有权，在 callback_lock 下提交 cpus_allowed/effective_xcpus，
 * 再传播后代和任务。成功返回 0；失败返回解析/校验 errno 或 -ENOMEM。
 */
static int update_cpumask(struct cpuset *cs, struct cpuset *trialcs,
			  const char *buf)
{
	int retval;
	struct tmpmasks tmp;
	bool force = false;
	int old_prs = cs->partition_root_state;

	retval = parse_cpuset_cpulist(buf, trialcs->cpus_allowed);
	if (retval < 0)
		return retval;

	/* Nothing to do if the cpus didn't change */
	/* 用户 CPU 请求与当前值完全相同，既无派生状态变化也无需重新传播。 */
	if (cpumask_equal(cs->cpus_allowed, trialcs->cpus_allowed))
		return 0;

	compute_trialcs_excpus(trialcs, cs);
	trialcs->prs_err = PERR_NONE;

	retval = validate_change(cs, trialcs);
	if (retval < 0)
		return retval;

	if (alloc_tmpmasks(&tmp))
		return -ENOMEM;

	/*
	 * Check all the descendants in update_cpumasks_hier() if
	 * effective_xcpus is to be changed.
	 */
	/* effective_xcpus 改变会影响后代输入，force 禁止同值剪枝并检查整棵子树。 */
	force = !cpumask_equal(cs->effective_xcpus, trialcs->effective_xcpus);

	partition_cpus_change(cs, trialcs, &tmp);

	spin_lock_irq(&callback_lock);
	cpumask_copy(cs->cpus_allowed, trialcs->cpus_allowed);
	cpumask_copy(cs->effective_xcpus, trialcs->effective_xcpus);
	if ((old_prs > 0) && !is_partition_valid(cs))
		reset_partition_data(cs);
	spin_unlock_irq(&callback_lock);

	/* effective_cpus/effective_xcpus will be updated here */
	/* 用户字段已提交；现在自顶向下发布 effective_cpus/effective_xcpus 并更新任务。 */
	update_cpumasks_hier(cs, &tmp, force);

	/* Update CS_SCHED_LOAD_BALANCE and/or sched_domains, if necessary */
	/* partition 状态存在时，同步其负载均衡位并标记需要重建的 sched_domain。 */
	if (cs->partition_root_state)
		update_partition_sd_lb(cs, old_prs);

	free_tmpmasks(&tmp);
	return retval;
}

/**
 * update_exclusive_cpumask - update the exclusive_cpus mask of a cpuset
 * @cs: the cpuset to consider
 * @trialcs: trial cpuset
 * @buf: buffer of cpu numbers written to this cpuset
 *
 * The tasks' cpumask will be updated if cs is a valid partition root.
 */
/*
 * update_exclusive_cpumask() - 处理 cpuset.cpus.exclusive 写入。
 *
 * 参数、锁和 trial 所有权与 update_cpumask() 相同，但不改 cpus_allowed。
 * 它先拒绝兄弟独占冲突并完成所有分配，再提交 exclusive/effective_xcpus；
 * partition 有效或获批集合变化时传播子树。成功返回 0，失败保持用户字段未提交。
 */
static int update_exclusive_cpumask(struct cpuset *cs, struct cpuset *trialcs,
				    const char *buf)
{
	int retval;
	struct tmpmasks tmp;
	bool force = false;
	int old_prs = cs->partition_root_state;

	retval = parse_cpuset_cpulist(buf, trialcs->exclusive_cpus);
	if (retval < 0)
		return retval;

	/* Nothing to do if the CPUs didn't change */
	/* 独占请求未变化时无需重做兄弟冲突校验或层级传播。 */
	if (cpumask_equal(cs->exclusive_cpus, trialcs->exclusive_cpus))
		return 0;

	/*
	 * Reject the change if there is exclusive CPUs conflict with
	 * the siblings.
	 */
	/* 候选 effective_xcpus 若被兄弟独占集合裁剪，说明用户请求存在所有权冲突。 */
	if (compute_trialcs_excpus(trialcs, cs))
		return -EINVAL;

	/*
	 * Check all the descendants in update_cpumasks_hier() if
	 * effective_xcpus is to be changed.
	 */
	/* 获批独占集合改变时，后代的 effective 输入也可能变化，必须强制完整传播。 */
	force = !cpumask_equal(cs->effective_xcpus, trialcs->effective_xcpus);

	retval = validate_change(cs, trialcs);
	if (retval)
		return retval;

	if (alloc_tmpmasks(&tmp))
		return -ENOMEM;

	trialcs->prs_err = PERR_NONE;
	partition_cpus_change(cs, trialcs, &tmp);

	spin_lock_irq(&callback_lock);
	cpumask_copy(cs->exclusive_cpus, trialcs->exclusive_cpus);
	cpumask_copy(cs->effective_xcpus, trialcs->effective_xcpus);
	if ((old_prs > 0) && !is_partition_valid(cs))
		reset_partition_data(cs);
	spin_unlock_irq(&callback_lock);

	/*
	 * Call update_cpumasks_hier() to update effective_cpus/effective_xcpus
	 * of the subtree when it is a valid partition root or effective_xcpus
	 * is updated.
	 */
	/*
	 * 有效 partition 总要重算成员可用 CPU；即使状态无效，只要 effective_xcpus 有
	 * 差量也要传播，以免后代继续保留旧的父级资源快照。
	 */
	if (is_partition_valid(cs) || force)
		update_cpumasks_hier(cs, &tmp, force);

	/* Update CS_SCHED_LOAD_BALANCE and/or sched_domains, if necessary */
	/* partition 状态存在时，把负载均衡位和 sched_domain 脏位同步到最终状态。 */
	if (cs->partition_root_state)
		update_partition_sd_lb(cs, old_prs);

	free_tmpmasks(&tmp);
	return 0;
}

/*
 * Migrate memory region from one set of nodes to another.  This is
 * performed asynchronously as it can be called from process migration path
 * holding locks involved in process management.  All mm migrations are
 * performed in the queued order and can be waited for by flushing
 * cpuset_migrate_mm_wq.
 */
/*
 * NUMA 内存迁移可能从持有进程管理锁的 attach 路径发起，直接执行会获取 mmap_lock
 * 并造成锁顺序问题。因此请求排入专用 workqueue 串行处理；队列顺序就是迁移顺序，
 * 调用者可 flush cpuset_migrate_mm_wq 等待此前所有 mm 迁移完成。
 */

struct cpuset_migrate_mm_work {
	struct work_struct	work;
	struct mm_struct	*mm;
	nodemask_t		from;
	nodemask_t		to;
};

/*
 * cpuset_migrate_mm_work 把一次 mm 页面迁移的所有权交给有序 workqueue：
 * work 是队列节点；mm 持有一份引用；from/to 是按值保存的稳定 nodemask 快照。
 * workfn 完成后释放 mm 引用和结构本身，排队者不得再访问。
 */

/*
 * cpuset_migrate_mm_workfn() 在进程上下文按队列顺序迁移一个 mm。
 * @work 内嵌于 mwork，container_of 恢复由 workqueue 独占的宿主。页面迁移是
 * 尽力而为，结果不再反馈给原控制事务；最后无条件 mmput/kfree。无返回值，可睡眠。
 */
static void cpuset_migrate_mm_workfn(struct work_struct *work)
{
	struct cpuset_migrate_mm_work *mwork =
		container_of(work, struct cpuset_migrate_mm_work, work);

	/* on a wq worker, no need to worry about %current's mems_allowed */
	/* workqueue worker 不代表被迁移任务，do_migrate_pages() 不受 current 的 cpuset 限制。 */
	do_migrate_pages(mwork->mm, &mwork->from, &mwork->to, MPOL_MF_MOVE_ALL);
	mmput(mwork->mm);
	kfree(mwork);
}

/*
 * cpuset_migrate_mm() 异步接管一份 @mm 引用并安排 NUMA 页面迁移。
 * @from/@to 只是借用快照源。集合相同或工作项分配失败时直接 mmput；成功时引用和
 * mask 所有权转给有序 work。无返回值；排队成功不表示迁移已经完成。
 */
static void cpuset_migrate_mm(struct mm_struct *mm, const nodemask_t *from,
							const nodemask_t *to)
{
	struct cpuset_migrate_mm_work *mwork;

	if (nodes_equal(*from, *to)) {
		mmput(mm);
		return;
	}

	mwork = kzalloc_obj(*mwork);
	if (mwork) {
		mwork->mm = mm;
		mwork->from = *from;
		mwork->to = *to;
		INIT_WORK(&mwork->work, cpuset_migrate_mm_workfn);
		queue_work(cpuset_migrate_mm_wq, &mwork->work);
	} else {
		mmput(mm);
	}
}

/*
 * flush_migrate_mm_task_workfn() 在 current 恢复用户态前等待迁移队列清空。
 * @head 由 schedule helper 分配并已转交 task_work；flush 后释放回调对象。
 * 无返回值，可睡眠，提供“用户继续执行前已完成此前迁移”的同步点。
 */
static void flush_migrate_mm_task_workfn(struct callback_head *head)
{
	flush_workqueue(cpuset_migrate_mm_wq);
	kfree(head);
}

/*
 * schedule_flush_migrate_mm() 为 current 安排 TWA_RESUME flush。
 * 无参数、无直接返回值；分配或 task_work_add 失败时释放对象并静默降级，因为迁移
 * work 仍会完成，只是不再保证当前任务返回用户态前等待它。
 */
static void schedule_flush_migrate_mm(void)
{
	struct callback_head *flush_cb;

	flush_cb = kzalloc_obj(struct callback_head);
	if (!flush_cb)
		return;

	init_task_work(flush_cb, flush_migrate_mm_task_workfn);

	if (task_work_add(current, flush_cb, TWA_RESUME))
		kfree(flush_cb);
}

/*
 * cpuset_change_task_nodemask - change task's mems_allowed and mempolicy
 * @tsk: the task to change
 * @newmems: new nodes that the task will be set
 *
 * We use the mems_allowed_seq seqlock to safely update both tsk->mems_allowed
 * and rebind an eventual tasks' mempolicy. If the task is allocating in
 * parallel, it might temporarily see an empty intersection, which results in
 * a seqlock check and retry before OOM or allocation failure.
 */
/*
 * cpuset_change_task_nodemask() - 原子更新任务 mems_allowed 与绑定 mempolicy。
 *
 * @tsk 是稳定借用任务，@newmems 是输入集合。task_lock 保护策略对象，关中断的
 * seqcount 写段让并发分配者识别中间状态并重试。先 OR 入新节点再 rebind，避免
 * 过渡交集为空；最后精确赋值。无返回值，不负责迁移已有页面。
 */
static void cpuset_change_task_nodemask(struct task_struct *tsk,
					nodemask_t *newmems)
{
	/* task_lock 稳定任务 mempolicy；seqcount 写段让无锁分配读者发现并重试中间状态。 */
	task_lock(tsk);

	local_irq_disable();
	write_seqcount_begin(&tsk->mems_allowed_seq);

	/*
	 * 先把旧、新集合取并集，保证 mpol_rebind_task() 期间不会出现空交集；策略完成
	 * rebind 后再把任务 mask 收敛为精确新值，整个窗口由 seqcount 标记。
	 */
	nodes_or(tsk->mems_allowed, tsk->mems_allowed, *newmems);
	mpol_rebind_task(tsk, newmems);
	tsk->mems_allowed = *newmems;

	write_seqcount_end(&tsk->mems_allowed_seq);
	local_irq_enable();

	task_unlock(tsk);
}

static void *cpuset_being_rebound;

/*
 * cpuset_being_rebound 在 cpuset_mutex 下指向唯一正在批量 rebind 的 cpuset。
 * fork 的 mpol_dup() 用它补上复制策略与父 mm 重绑并发的窗口；NULL 表示无事务。
 * 裸指针不持有 css 引用，其有效期由在线遍历和 cpuset_mutex 保证。
 */

/**
 * cpuset_update_tasks_nodemask - Update the nodemasks of tasks in the cpuset.
 * @cs: the cpuset in which each task's mems_allowed mask needs to be changed
 *
 * Iterate through each task of @cs updating its mems_allowed to the
 * effective cpuset's.  As this function is called with cpuset_mutex held,
 * cpuset membership stays stable.
 */
/*
 * cpuset_update_tasks_nodemask() - 把 effective NUMA 约束传播到任务和 mm。
 *
 * 调用者持 cpuset_mutex，@cs 在线。函数求得非空在线集合并设置全局 rebound 标记，
 * 逐任务更新 mems_allowed、VMA mempolicy；CS_MEMORY_MIGRATE 置位时把 mm 引用
 * 交给异步迁移。无直接返回值；策略在返回前已重绑，页面迁移可能仍在队列。
 */
void cpuset_update_tasks_nodemask(struct cpuset *cs)
{
	static nodemask_t newmems;	/* protected by cpuset_mutex */
	/* newmems 是跨任务复用的静态工作区，cpuset_mutex 保证同一时刻只有一次传播。 */
	struct css_task_iter it;
	struct task_struct *task;

	cpuset_being_rebound = cs;		/* causes mpol_dup() rebind */
	/* 发布当前 rebind 目标，使并发 fork 的 mpol_dup() 主动重绑新复制的策略。 */

	guarantee_online_mems(cs, &newmems);

	/*
	 * The mpol_rebind_mm() call takes mmap_lock, which we couldn't
	 * take while holding tasklist_lock.  Forks can happen - the
	 * mpol_dup() cpuset_being_rebound check will catch such forks,
	 * and rebind their vma mempolicies too.  Because we still hold
	 * the global cpuset_mutex, we know that no other rebind effort
	 * will be contending for the global variable cpuset_being_rebound.
	 * It's ok if we rebind the same mm twice; mpol_rebind_mm()
	 * is idempotent.  Also migrate pages in each mm to new nodes.
	 */
	/*
	 * mpol_rebind_mm() 要取得 mmap_lock，不能在 tasklist_lock 内执行；遍历期间 fork
	 * 仍可能发生，cpuset_being_rebound 让 mpol_dup() 补绑新 VMA 策略。全局
	 * cpuset_mutex 排除了另一批 rebind；同一 mm 被处理两次也安全，因为 rebind 幂等。
	 * 每个任务策略更新后，可按 CS_MEMORY_MIGRATE 把已有页面异步迁到新节点。
	 */
	css_task_iter_start(&cs->css, 0, &it);
	while ((task = css_task_iter_next(&it))) {
		struct mm_struct *mm;
		bool migrate;

		cpuset_change_task_nodemask(task, &newmems);

		mm = get_task_mm(task);
		if (!mm)
			continue;

		migrate = is_memory_migrate(cs);

		/*
		 * For v1 we can have empty effective_mems, but we cannot
		 * attach any tasks (see cpuset_can_attach_check()). For v2,
		 * effective_mems is guaranteed to not be empty.
		 */
		/*
		 * v1 允许空 effective_mems，但 can_attach 会阻止任务进入；v2 则保证该集合非空。
		 * 因而这里可直接用 effective_mems 重绑 mm，不需要额外 fallback。
		 */
		mpol_rebind_mm(mm, &cs->effective_mems);
		if (migrate)
			cpuset_migrate_mm(mm, &cs->old_mems_allowed, &newmems);
		else
			mmput(mm);
	}
	css_task_iter_end(&it);

	/*
	 * All the tasks' nodemasks have been updated, update
	 * cs->old_mems_allowed.
	 */
	/* 所有任务已看到新集合，把本次目标保存为下次页面迁移的来源快照。 */
	cs->old_mems_allowed = newmems;

	/* We're done rebinding vmas to this cpuset's new mems_allowed. */
	/* 批量 VMA rebind 完成，清除 fork 路径使用的全局补绑提示。 */
	cpuset_being_rebound = NULL;
}

/*
 * update_nodemasks_hier - Update effective nodemasks and tasks in the subtree
 * @cs: the cpuset to consider
 * @new_mems: a temp variable for calculating new effective_mems
 *
 * When configured nodemask is changed, the effective nodemasks of this cpuset
 * and all its descendants need to be updated.
 *
 * On legacy hierarchy, effective_mems will be the same with mems_allowed.
 *
 * Called with cpuset_mutex held
 */
/*
 * update_nodemasks_hier() 自顶向下重算 @cs 子树的 effective_mems。
 * @new_mems 是复用的输出工作区，调用者持 cpuset_mutex。v2 交集为空时继承父集合；
 * 值未变可剪掉整棵子树。变化节点通过 online css 引用跨越退出 RCU和可睡眠的任务
 * 更新，并在 callback_lock 下发布 mask。无返回值，返回时在线后代均已同步。
 */
static void update_nodemasks_hier(struct cpuset *cs, nodemask_t *new_mems)
{
	struct cpuset *cp;
	struct cgroup_subsys_state *pos_css;

	rcu_read_lock();
	cpuset_for_each_descendant_pre(cp, pos_css, cs) {
		struct cpuset *parent = parent_cs(cp);

		bool has_mems = nodes_and(*new_mems, cp->mems_allowed, parent->effective_mems);

		/*
		 * If it becomes empty, inherit the effective mask of the
		 * parent, which is guaranteed to have some MEMs.
		 */
		/* v2 子节点交集为空时继承父 effective_mems，保证普通层级任务仍有可分配节点。 */
		if (is_in_v2_mode() && !has_mems)
			*new_mems = parent->effective_mems;

		/* Skip the whole subtree if the nodemask remains the same. */
		/* 当前 effective_mems 未变意味着后代输入也未变，可安全剪去整棵子树。 */
		if (nodes_equal(*new_mems, cp->effective_mems)) {
			pos_css = css_rightmost_descendant(pos_css);
			continue;
		}

		if (!css_tryget_online(&cp->css))
			continue;
		rcu_read_unlock();

		spin_lock_irq(&callback_lock);
		cp->effective_mems = *new_mems;
		spin_unlock_irq(&callback_lock);

		WARN_ON(!is_in_v2_mode() &&
			!nodes_equal(cp->mems_allowed, cp->effective_mems));

		cpuset_update_tasks_nodemask(cp);

		rcu_read_lock();
		css_put(&cp->css);
	}
	rcu_read_unlock();
}

/*
 * Handle user request to change the 'mems' memory placement
 * of a cpuset.  Needs to validate the request, update the
 * cpusets mems_allowed, and for each task in the cpuset,
 * update mems_allowed and rebind task's mempolicy and any vma
 * mempolicies and if the cpuset is marked 'memory_migrate',
 * migrate the tasks pages to the new memory.
 *
 * Call with cpuset_mutex held. May take callback_lock during call.
 * Will take tasklist_lock, scan tasklist for tasks in cpuset cs,
 * lock each such tasks mm->mmap_lock, scan its vma's and rebind
 * their mempolicies to the cpusets new mems_allowed.
 */
/*
 * update_nodemask() - 处理 cpuset.mems 写入并触发策略重绑/可选页面迁移。
 *
 * @cs 为在线目标，@trialcs 为未发布副本，@buf 为用户 nodelist；调用者持
 * cpuset_mutex。函数校验语法、根节点子集和 populated 不变量，在 callback_lock
 * 下提交 mems_allowed，再传播 effective_mems。成功返回 0；失败返回负 errno，
 * 发布前失败不会修改真实对象。
 */
static int update_nodemask(struct cpuset *cs, struct cpuset *trialcs,
			   const char *buf)
{
	int retval;

	/*
	 * An empty mems_allowed is ok iff there are no tasks in the cpuset.
	 * The validate_change() call ensures that cpusets with tasks have memory.
	 */
	/*
	 * 空 mems_allowed 仅对没有任务的 cpuset 合法；这里先允许解析空集合，随后由
	 * validate_change() 结合 populated 状态统一拒绝会让现有任务无内存节点的配置。
	 */
	retval = nodelist_parse(buf, trialcs->mems_allowed);
	if (retval < 0)
		return retval;

	if (!nodes_subset(trialcs->mems_allowed,
			  top_cpuset.mems_allowed))
		return -EINVAL;

	/* No change? nothing to do */
	/* 用户节点集合未变化时不需要重绑策略或扫描后代。 */
	if (nodes_equal(cs->mems_allowed, trialcs->mems_allowed))
		return 0;

	retval = validate_change(cs, trialcs);
	if (retval < 0)
		return retval;

	check_insane_mems_config(&trialcs->mems_allowed);

	spin_lock_irq(&callback_lock);
	cs->mems_allowed = trialcs->mems_allowed;
	spin_unlock_irq(&callback_lock);

	/* use trialcs->mems_allowed as a temp variable */
	/* trial 已完成校验且不再需要原候选值，可复用其 nodemask 作为层级传播工作区。 */
	update_nodemasks_hier(cs, &trialcs->mems_allowed);
	return 0;
}

/*
 * current_cpuset_is_being_rebound() 判断 current 是否属于正在批量重绑的 cpuset。
 * 函数在 RCU 下比较裸指针，不取得引用、不睡眠；返回值供 fork/mempolicy 复制路径
 * 决定是否额外 rebind，新事务可在返回后开始或结束。
 */
bool current_cpuset_is_being_rebound(void)
{
	bool ret;

	rcu_read_lock();
	ret = task_cs(current) == cpuset_being_rebound;
	rcu_read_unlock();

	return ret;
}

/*
 * cpuset_update_flag - read a 0 or a 1 in a file and update associated flag
 * bit:		the bit to update (see cpuset_flagbits_t)
 * cs:		the cpuset to update
 * turning_on: 	whether the flag is being set or cleared
 *
 * Call with cpuset_mutex held.
 */

/*
 * cpuset_update_flag() - 用 trial 副本校验并提交一个 cpuset flag。
 *
 * @bit 是 cpuset_flagbits_t 位号，@cs 是在线目标，@turning_on 为布尔语义；
 * 调用者持 cpuset_mutex。函数可睡眠分配 trial，validate_change 成功后在
 * callback_lock 下发布 flags；负载均衡变化触发 domain 重建/置脏，spread 变化
 * 更新 v1 任务标志。成功返回 0，失败返回 -ENOMEM 或校验 errno。
 */
int cpuset_update_flag(cpuset_flagbits_t bit, struct cpuset *cs,
		       int turning_on)
{
	struct cpuset *trialcs;
	int balance_flag_changed;
	int spread_flag_changed;
	int err;

	trialcs = dup_or_alloc_cpuset(cs);
	if (!trialcs)
		return -ENOMEM;

	/* 阶段一：仅在未发布 trial 中修改目标位，随后复用完整结构校验全部层级不变量。 */
	if (turning_on)
		set_bit(bit, &trialcs->flags);
	else
		clear_bit(bit, &trialcs->flags);

	err = validate_change(cs, trialcs);
	if (err < 0)
		goto out;

	/* 阶段二：在提交前记录哪些下游消费者需要刷新，避免发布后再猜测旧值。 */
	balance_flag_changed = (is_sched_load_balance(cs) !=
				is_sched_load_balance(trialcs));

	spread_flag_changed = ((is_spread_slab(cs) != is_spread_slab(trialcs))
			|| (is_spread_page(cs) != is_spread_page(trialcs)));

	/* 阶段三：在 callback_lock 下原子发布 flags；trial 仍由本函数持有。 */
	spin_lock_irq(&callback_lock);
	cs->flags = trialcs->flags;
	spin_unlock_irq(&callback_lock);

	/* 阶段四：锁外通知调度域或 v1 spread 任务；失败出口统一释放 trial。 */
	if (!cpumask_empty(trialcs->cpus_allowed) && balance_flag_changed) {
		if (cpuset_v2())
			cpuset_force_rebuild();
		else
			rebuild_sched_domains_locked();
	}

	if (spread_flag_changed)
		cpuset1_update_tasks_flags(cs);
out:
	free_cpuset(trialcs);
	return err;
}

/**
 * update_prstate - update partition_root_state
 * @cs: the cpuset to update
 * @new_prs: new partition root state
 * Return: 0 if successful, != 0 if error
 *
 * Call with cpuset_mutex held.
 */
/*
 * update_prstate() - 处理 member/root/isolated 的显式状态转换。
 *
 * @cs 是在线目标，@new_prs 为非负 PRS_* 用户请求；调用者持 cpuset_mutex。
 * member->partition 选择本地或远程资源事务；root<->isolated 只改变负载均衡和
 * isolated/HK 集合；partition->member 总是允许并归还 CPU。资源/权限失败会把
 * 请求状态取负并记录 prs_err，而接口仍返回 0 让用户通过 partition 文件观察
 * invalid 状态；只有临时分配失败返回 -ENOMEM。返回前传播子树、调度域和通知。
 */
static int update_prstate(struct cpuset *cs, int new_prs)
{
	int err = PERR_NONE, old_prs = cs->partition_root_state;
	struct cpuset *parent = parent_cs(cs);
	struct tmpmasks tmpmask;
	bool isolcpus_updated = false;

	if (old_prs == new_prs)
		return 0;

	/*
	 * Treat a previously invalid partition root as if it is a "member".
	 */
	/*
	 * 对新的非 member 请求，无效 root 在资源事务上等同尚未建立的 member；其负值只
	 * 记录用户期望类型，不能假定仍持有有效 partition CPU。
	 */
	if (new_prs && is_partition_invalid(cs))
		old_prs = PRS_MEMBER;

	if (alloc_tmpmasks(&tmpmask))
		return -ENOMEM;

	err = update_partition_exclusive_flag(cs, new_prs);
	if (err)
		goto out;

	if (!old_prs) {
		/*
		 * cpus_allowed and exclusive_cpus cannot be both empty.
		 */
		/* 新 partition 至少要有 cpus_allowed 或 exclusive_cpus 作为所有权请求来源。 */
		if (xcpus_empty(cs)) {
			err = PERR_CPUSEMPTY;
			goto out;
		}

		/*
		 * We don't support the creation of a new local partition with
		 * a remote partition underneath it. This unsupported
		 * setting can happen only if parent is the top_cpuset because
		 * a remote partition cannot be created underneath an existing
		 * local or remote partition.
		 */
		/*
		 * top_cpuset 下若候选本地 partition 的 exclusive_cpus 与已分发 remote CPU
		 * 相交，就会形成“本地父拥有远程后代”的不支持拓扑，必须以 PERR_REMOTE 拒绝。
		 */
		if ((parent == &top_cpuset) &&
		    cpumask_intersects(cs->exclusive_cpus, subpartitions_cpus)) {
			err = PERR_REMOTE;
			goto out;
		}

		/*
		 * If parent is valid partition, enable local partiion.
		 * Otherwise, enable a remote partition.
		 */
		/* 父级有效时沿本地父子事务取 CPU；否则直接从 top_cpuset 建立 remote 所有权。 */
		if (is_partition_valid(parent)) {
			enum partition_cmd cmd = (new_prs == PRS_ROOT)
					       ? partcmd_enable : partcmd_enablei;

			err = update_parent_effective_cpumask(cs, cmd, NULL, &tmpmask);
		} else {
			err = remote_partition_enable(cs, new_prs, &tmpmask);
		}
	} else if (old_prs && new_prs) {
		/*
		 * A change in load balance state only, no change in cpumasks.
		 * Need to update isolated_cpus.
		 */
		/*
		 * root 与 isolated 互转不改变 CPU 所有权，只改变是否参与负载均衡；提交前仍要
		 * 验证 housekeeping 完整性，成功后再更新全局 isolated_cpus。
		 */
		if (((new_prs == PRS_ISOLATED) &&
		     !isolated_cpus_can_update(cs->effective_xcpus, NULL)) ||
		    prstate_housekeeping_conflict(new_prs, cs->effective_xcpus))
			err = PERR_HKEEPING;
		else
			isolcpus_updated = true;
	} else {
		/*
		 * Switching back to member is always allowed even if it
		 * disables child partitions.
		 */
		/*
		 * 撤回 member 是放宽当前节点约束，因此总是允许；它会归还 CPU，后代 partition
		 * 若因此失去合法父边界，将在随后的层级传播中转为无效。
		 */
		if (is_remote_partition(cs))
			remote_partition_disable(cs, &tmpmask);
		else
			update_parent_effective_cpumask(cs, partcmd_disable,
							NULL, &tmpmask);

		/*
		 * Invalidation of child partitions will be done in
		 * update_cpumasks_hier().
		 */
		/* 子 partition 不在此处逐个修改，由统一自顶向下传播按最终父状态处理。 */
	}
out:
	/*
	 * Make partition invalid & disable CS_CPU_EXCLUSIVE if an error
	 * happens.
	 */
	/*
	 * 资源或权限检查失败时仍保留用户请求的类型，但以负状态发布并撤销
	 * CS_CPU_EXCLUSIVE；prs_err 给出失败原因，用户可修复配置后再次触发恢复。
	 */
	if (err) {
		new_prs = -new_prs;
		update_partition_exclusive_flag(cs, new_prs);
	}

	spin_lock_irq(&callback_lock);
	cs->partition_root_state = new_prs;
	WRITE_ONCE(cs->prs_err, err);
	if (!is_partition_valid(cs))
		reset_partition_data(cs);
	else if (isolcpus_updated)
		isolated_cpus_update(old_prs, new_prs, cs->effective_xcpus);
	spin_unlock_irq(&callback_lock);

	/* Force update if switching back to member & update effective_xcpus */
	/* 回到 member 会改变后代继承边界，force=true 禁止同值剪枝并重算整棵子树。 */
	update_cpumasks_hier(cs, &tmpmask, !new_prs);

	/* A newly created partition must have effective_xcpus set */
	/* 新建且有效的 partition 必须已经取得 effective_xcpus；否则说明提交协议破坏。 */
	WARN_ON_ONCE(!old_prs && (new_prs > 0)
			      && cpumask_empty(cs->effective_xcpus));

	/* Update sched domains and load balance flag */
	/* 最后同步负载均衡位并在需要时重建 sched_domain，使调度器看到完整的新边界。 */
	update_partition_sd_lb(cs, old_prs);

	notify_partition_change(cs, old_prs);
	if (force_sd_rebuild)
		rebuild_sched_domains_locked();
	free_tmpmasks(&tmpmask);
	return 0;
}

static struct cpuset *cpuset_attach_old_cs;

/*
 * cpuset_attach_old_cs 在 cgroup attach 两阶段回调之间保存源 cpuset。
 * cgroup 迁移框架串行化这对回调，cpuset_mutex 保护消费阶段；它是借用裸指针，
 * 不额外持有 css 引用，不能在该协议外缓存。
 */

/*
 * Check to see if a cpuset can accept a new task
 * For v1, cpus_allowed and mems_allowed can't be empty.
 * For v2, effective_cpus can't be empty.
 * Note that in v1, effective_cpus = cpus_allowed.
 */
/*
 * cpuset_can_attach_check() 对目标 @cs 做最小可运行性检查。
 * v2 要求 effective_cpus 非空；v1 还要求用户 mems_allowed 非空。
 * 返回 0 或 -ENOSPC，不锁定对象、不修改状态，调用者必须稳定 @cs。
 */
static int cpuset_can_attach_check(struct cpuset *cs)
{
	if (cpumask_empty(cs->effective_cpus) ||
	   (!is_in_v2_mode() && nodes_empty(cs->mems_allowed)))
		return -ENOSPC;
	return 0;
}

/*
 * reset_migrate_dl_data() 清空一次 attach 在目标 @cs 上积累的 deadline 临时记账。
 * 调用者持 cpuset_mutex；无返回值，dl_bw_cpu=-1 恢复“未预留带宽”的哨兵状态。
 */
static void reset_migrate_dl_data(struct cpuset *cs)
{
	cs->nr_migrate_dl_tasks = 0;
	cs->sum_migrate_dl_bw = 0;
	cs->dl_bw_cpu = -1;
}

/* Called by cgroups to determine if a cpuset is usable; cpuset_mutex held */
/*
 * cpuset_can_attach() - cgroup 迁移的预检与 SCHED_DEADLINE 带宽预留阶段。
 *
 * @tset 是迁移框架持有的任务集合；函数借用源/目标 css，内部持 cpuset_mutex。
 * 逐任务检查可迁移性与 setscheduler 权限，并累计真正需要跨 root_domain 的 DL
 * 带宽；必要时在目标 active CPU 上预留。成功增加 attach_in_progress 并返回 0，
 * 把预留交给 attach/cancel；失败返回 errno，释放临时 DL 记账且不建立迁移预留。
 */
static int cpuset_can_attach(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct cpuset *cs, *oldcs;
	struct task_struct *task;
	bool setsched_check;
	int cpu, ret;

	/* used later by cpuset_attach() */
	/* 保存预检时的源 cpuset，提交回调用它计算 CPU/NUMA 差量和迁移来源。 */
	cpuset_attach_old_cs = task_cs(cgroup_taskset_first(tset, &css));
	oldcs = cpuset_attach_old_cs;
	cs = css_cs(css);

	mutex_lock(&cpuset_mutex);

	/* Check to see if task is allowed in the cpuset */
	/* 目标必须至少提供可运行 CPU；v1 还必须提供可分配内存节点。 */
	ret = cpuset_can_attach_check(cs);
	if (ret)
		goto out_unlock;

	/*
	 * Skip rights over task setsched check in v2 when nothing changes,
	 * migration permission derives from hierarchy ownership in
	 * cgroup_procs_write_permission()).
	 */
	/*
	 * v2 且 CPU/NUMA 有效集合完全相同的迁移不改变调度能力，权限已由 cgroup 层级
	 * 所有权检查覆盖，可跳过逐任务 setscheduler LSM 检查；其他情况仍需检查。
	 */
	setsched_check = !cpuset_v2() ||
		!cpumask_equal(cs->effective_cpus, oldcs->effective_cpus) ||
		!nodes_equal(cs->effective_mems, oldcs->effective_mems);

	/*
	 * A v1 cpuset with tasks will have no CPU left only when CPU hotplug
	 * brings the last online CPU offline as users are not allowed to empty
	 * cpuset.cpus when there are active tasks inside. When that happens,
	 * we should allow tasks to migrate out without security check to make
	 * sure they will be able to run after migration.
	 */
	/*
	 * v1 中含任务 cpuset 只有在 hotplug 下线最后一个 CPU 后才会变空。此时迁出是恢复
	 * 可运行性的唯一途径，必须跳过调度权限检查，避免把任务永久困在空集合中。
	 */
	if (!is_in_v2_mode() && cpumask_empty(oldcs->effective_cpus))
		setsched_check = false;

	cgroup_taskset_for_each(task, css, tset) {
		ret = task_can_attach(task);
		if (ret)
			goto out_unlock;

		if (setsched_check) {
			ret = security_task_setscheduler(task);
			if (ret)
				goto out_unlock;
		}

		if (dl_task(task)) {
			/*
			 * Count all migrating DL tasks for cpuset task accounting.
			 * Only tasks that need a root-domain bandwidth move
			 * contribute to sum_migrate_dl_bw.
			 */
			/*
			 * 所有迁移 DL 任务都计入 cpuset 任务数转移；只有跨 root_domain、需要重新
			 * 预留运行带宽的任务才把 dl_bw 累加到本批次请求。
			 */
			cs->nr_migrate_dl_tasks++;
			if (dl_task_needs_bw_move(task, cs->effective_cpus))
				cs->sum_migrate_dl_bw += task->dl.dl_bw;
		}
	}

	if (!cs->sum_migrate_dl_bw)
		goto out_success;

	cpu = cpumask_any_and(cpu_active_mask, cs->effective_cpus);
	if (unlikely(cpu >= nr_cpu_ids)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = dl_bw_alloc(cpu, cs->sum_migrate_dl_bw);
	if (ret)
		goto out_unlock;

	cs->dl_bw_cpu = cpu;

out_success:
	/*
	 * Mark attach is in progress.  This makes validate_change() fail
	 * changes which zero cpus/mems_allowed.
	 */
	/*
	 * 预检成功后增加 attach_in_progress，使并发 validate_change() 不能把目标 CPU/内存
	 * 清空；提交或取消回调必须成对递减并唤醒可能等待的 hotplug 路径。
	 */
	cs->attach_in_progress++;

out_unlock:
	if (ret)
		reset_migrate_dl_data(cs);
	mutex_unlock(&cpuset_mutex);
	return ret;
}

/*
 * cpuset_cancel_attach() 撤销 can_attach 已成功建立、但迁移框架未提交的预留。
 * @tset 提供目标 css；函数在 cpuset_mutex 下减 attach_in_progress，释放已预留
 * DL 带宽并清临时计数。无返回值，返回时热插拔等待者可在计数归零后继续。
 */
static void cpuset_cancel_attach(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct cpuset *cs;

	cgroup_taskset_first(tset, &css);
	cs = css_cs(css);

	mutex_lock(&cpuset_mutex);
	dec_attach_in_progress_locked(cs);

	if (cs->dl_bw_cpu >= 0)
		dl_bw_free(cs->dl_bw_cpu, cs->sum_migrate_dl_bw);

	if (cs->nr_migrate_dl_tasks)
		reset_migrate_dl_data(cs);

	mutex_unlock(&cpuset_mutex);
}

/*
 * Protected by cpuset_mutex. cpus_attach is used only by cpuset_attach_task()
 * but we can't allocate it dynamically there.  Define it global and
 * allocate from cpuset_init().
 */
/*
 * cpus_attach 只供 cpuset_attach_task() 使用，但提交阶段不能可靠动态分配，因此在
 * cpuset_init() 预分配为全局工作区；cpuset_mutex 串行化每批迁移。相邻 nodemask
 * 工作区遵循同一有效期。
 */
static cpumask_var_t cpus_attach;
static nodemask_t cpuset_attach_nodemask_to;

/*
 * cpus_attach 与 cpuset_attach_nodemask_to 是 cpuset_mutex 保护的全局迁移工作区。
 * 迁移框架串行调用使它们可复用，避免在 attach 提交阶段动态分配；值只在当前批次
 * 有效，不是任何任务或 cpuset 的持久配置。
 */

/*
 * cpuset_attach_task() 把一个 @task 的 CPU、NUMA 和 v1 spread 状态切到目标 @cs。
 * 两指针均为迁移框架稳定的借用对象，调用者持 cpuset_mutex。先求非空 CPU 集合并
 * 进入调度器 affinity 协议，再用预计算 nodemask 重绑任务策略。无返回值；
 * set_cpus_allowed_ptr 异常只 WARN，因为 can_attach 已承诺该提交应成功。
 */
static void cpuset_attach_task(struct cpuset *cs, struct task_struct *task)
{
	lockdep_assert_cpuset_lock_held();

	if (cs != &top_cpuset)
		guarantee_active_cpus(task, cpus_attach);
	else
		cpumask_andnot(cpus_attach, task_cpu_possible_mask(task),
			       subpartitions_cpus);
	/*
	 * can_attach beforehand should guarantee that this doesn't
	 * fail.  TODO: have a better way to handle failure here
	 */
	/*
	 * can_attach 已验证目标并完成必要预留，set_cpus_allowed_ptr() 理论上不应失败；
	 * 当前只能 WARN 暴露异常，TODO 是让提交阶段具备比告警更完整的失败处理协议。
	 */
	WARN_ON_ONCE(set_cpus_allowed_ptr(task, cpus_attach));

	cpuset_change_task_nodemask(task, &cpuset_attach_nodemask_to);
	cpuset1_update_task_spread_flags(cs, task);
}

/*
 * cpuset_attach() - cgroup 迁移的不可回滚提交阶段。
 *
 * @tset 由迁移框架持有，入口已持 CPU hotplug lock；函数再取 cpuset_mutex。
 * v2 资源不变时跳过逐任务快路径。否则更新每个任务 affinity/mems_allowed，并按
 * thread-group leader 重绑 mm；需要迁页时转交异步 work 并安排返回用户态前 flush。
 * 最后转移 DL 任务计数、释放 can_attach 临时记账并减 attach_in_progress。
 * 无直接返回值；返回后迁移的 cpuset 约束已提交，页面迁移可能由 task_work 等待。
 */
static void cpuset_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct task_struct *leader;
	struct cgroup_subsys_state *css;
	struct cpuset *cs;
	struct cpuset *oldcs = cpuset_attach_old_cs;
	bool cpus_updated, mems_updated;
	bool queue_task_work = false;

	cgroup_taskset_first(tset, &css);
	cs = css_cs(css);

	lockdep_assert_cpus_held();	/* see cgroup_attach_lock() */
	/* cgroup_attach_lock() 提供 CPU hotplug 读侧保护，使本批次使用的 active 集合稳定。 */
	mutex_lock(&cpuset_mutex);
	cpus_updated = !cpumask_equal(cs->effective_cpus,
				      oldcs->effective_cpus);
	mems_updated = !nodes_equal(cs->effective_mems, oldcs->effective_mems);

	/*
	 * In the default hierarchy, enabling cpuset in the child cgroups
	 * will trigger a number of cpuset_attach() calls with no change
	 * in effective cpus and mems. In that case, we can optimize out
	 * by skipping the task iteration and update.
	 */
	/*
	 * v2 启用子控制器会触发不改变资源集合的形式迁移；此时无需逐任务改 affinity 或
	 * mempolicy，只保存最终 nodemask 并进入统一记账清理。
	 */
	if (cpuset_v2() && !cpus_updated && !mems_updated) {
		cpuset_attach_nodemask_to = cs->effective_mems;
		goto out;
	}

	guarantee_online_mems(cs, &cpuset_attach_nodemask_to);

	cgroup_taskset_for_each(task, css, tset)
		cpuset_attach_task(cs, task);

	/*
	 * Change mm for all threadgroup leaders. This is expensive and may
	 * sleep and should be moved outside migration path proper. Skip it
	 * if there is no change in effective_mems and CS_MEMORY_MIGRATE is
	 * not set.
	 */
	/*
	 * mm 级 VMA 策略重绑会取得 mmap_lock、成本高且可能睡眠；仅当 effective_mems
	 * 改变或显式要求页面迁移时按 thread-group leader 处理一次，避免同一 mm 重复扫描。
	 */
	cpuset_attach_nodemask_to = cs->effective_mems;
	if (!is_memory_migrate(cs) && !mems_updated)
		goto out;

	cgroup_taskset_for_each_leader(leader, css, tset) {
		struct mm_struct *mm = get_task_mm(leader);

		if (mm) {
			mpol_rebind_mm(mm, &cpuset_attach_nodemask_to);

			/*
			 * old_mems_allowed is the same with mems_allowed
			 * here, except if this task is being moved
			 * automatically due to hotplug.  In that case
			 * @mems_allowed has been updated and is empty, so
			 * @old_mems_allowed is the right nodesets that we
			 * migrate mm from.
			 */
			/*
			 * 通常 old_mems_allowed 与旧 mems_allowed 相同；hotplug 自动迁移会先把任务
			 * 当前集合更新为空，此时只有保存的 old_mems_allowed 能准确表示页面迁出来源。
			 */
			if (is_memory_migrate(cs)) {
				cpuset_migrate_mm(mm, &oldcs->old_mems_allowed,
						  &cpuset_attach_nodemask_to);
				queue_task_work = true;
			} else
				mmput(mm);
		}
	}

out:
	if (queue_task_work)
		schedule_flush_migrate_mm();
	cs->old_mems_allowed = cpuset_attach_nodemask_to;

	if (cs->nr_migrate_dl_tasks) {
		cs->nr_deadline_tasks += cs->nr_migrate_dl_tasks;
		oldcs->nr_deadline_tasks -= cs->nr_migrate_dl_tasks;
		reset_migrate_dl_data(cs);
	}

	dec_attach_in_progress_locked(cs);

	mutex_unlock(&cpuset_mutex);
}

/*
 * Common handling for a write to a "cpus" or "mems" file.
 */
/*
 * cpuset_write_resmask() - cpus/mems/exclusive 三类控制文件的统一写入口。
 *
 * @of 提供目标 css 与文件类型，@buf 是可原地去空白的内核缓冲，@nbytes 是原写入
 * 字节数，@off 不参与这些整值文件语义。根只读。函数取得完整锁组、确认 css 在线，
 * 分配 trial 并分派具体更新；事务尾部统一重建 domain/HK 和解锁。
 * 成功返回 @nbytes；失败返回 -EACCES/-ENODEV/-ENOMEM/-EINVAL 或下层 errno。
 * mems 写无论成败都安排 migration flush task_work，以覆盖已排队的尽力迁移。
 */
ssize_t cpuset_write_resmask(struct kernfs_open_file *of,
				    char *buf, size_t nbytes, loff_t off)
{
	struct cpuset *cs = css_cs(of_css(of));
	struct cpuset *trialcs;
	int retval = -ENODEV;

	/* root is read-only */
	/* 根 cpuset 由 hotplug 与控制器生命周期维护，用户不能通过资源文件直接改写。 */
	if (cs == &top_cpuset)
		return -EACCES;

	buf = strstrip(buf);
	/* 阶段一：取得完整配置事务锁并确认目标 css 仍在线，离线对象不得再接受写入。 */
	cpuset_full_lock();
	if (!is_cpuset_online(cs))
		goto out_unlock;

	/* 阶段二：复制未发布 trial；解析和校验失败都只丢弃副本，不污染真实对象。 */
	trialcs = dup_or_alloc_cpuset(cs);
	if (!trialcs) {
		retval = -ENOMEM;
		goto out_unlock;
	}

	/* 阶段三：按文件私有类型分派 CPU、独占 CPU 或 NUMA 的专用提交协议。 */
	switch (of_cft(of)->private) {
	case FILE_CPULIST:
		retval = update_cpumask(cs, trialcs, buf);
		break;
	case FILE_EXCLUSIVE_CPULIST:
		retval = update_exclusive_cpumask(cs, trialcs, buf);
		break;
	case FILE_MEMLIST:
		retval = update_nodemask(cs, trialcs, buf);
		break;
	default:
		retval = -EINVAL;
		break;
	}

	free_cpuset(trialcs);
out_unlock:
	/*
	 * 阶段四：统一消费 sched_domain/HK 脏位并释放完整锁组。mems 写还安排 task_work，
	 * 使 current 返回用户态前等待此前已经排队的异步页面迁移。
	 */
	cpuset_update_sd_hk_unlock();
	if (of_cft(of)->private == FILE_MEMLIST)
		schedule_flush_migrate_mm();
	return retval ?: nbytes;
}

/*
 * These ascii lists should be read in a single call, by using a user
 * buffer large enough to hold the entire map.  If read in smaller
 * chunks, there is no guarantee of atomicity.  Since the display format
 * used, list of ranges of sequential numbers, is variable length,
 * and since these maps can change value dynamically, one could read
 * gibberish by doing partial reads while a list was changing.
 */
/*
 * cpuset_common_seq_show() - 输出 mask 类 cpuset 控制文件的单次一致快照。
 *
 * @sf 提供目标 css/文件类型，@v 未使用。函数在 callback_lock 禁中断临界区内完成
 * 整个多字 mask 格式化，避免同一次 read 拼接新旧字；用户仍须一次读完整文本。
 * 成功返回 0，未知类型返回 -EINVAL；不修改 cpuset。
 */
int cpuset_common_seq_show(struct seq_file *sf, void *v)
{
	struct cpuset *cs = css_cs(seq_css(sf));
	cpuset_filetype_t type = seq_cft(sf)->private;
	int ret = 0;

	/* callback_lock 覆盖选择与格式化全过程，保证单次 seq 输出不会混合两个 mask 版本。 */
	spin_lock_irq(&callback_lock);

	/* private 枚举把多个控制文件映射到对应用户/派生/全局掩码。 */
	switch (type) {
	case FILE_CPULIST:
		seq_printf(sf, "%*pbl\n", cpumask_pr_args(cs->cpus_allowed));
		break;
	case FILE_MEMLIST:
		seq_printf(sf, "%*pbl\n", nodemask_pr_args(&cs->mems_allowed));
		break;
	case FILE_EFFECTIVE_CPULIST:
		seq_printf(sf, "%*pbl\n", cpumask_pr_args(cs->effective_cpus));
		break;
	case FILE_EFFECTIVE_MEMLIST:
		seq_printf(sf, "%*pbl\n", nodemask_pr_args(&cs->effective_mems));
		break;
	case FILE_EXCLUSIVE_CPULIST:
		seq_printf(sf, "%*pbl\n", cpumask_pr_args(cs->exclusive_cpus));
		break;
	case FILE_EFFECTIVE_XCPULIST:
		seq_printf(sf, "%*pbl\n", cpumask_pr_args(cs->effective_xcpus));
		break;
	case FILE_SUBPARTS_CPULIST:
		seq_printf(sf, "%*pbl\n", cpumask_pr_args(subpartitions_cpus));
		break;
	case FILE_ISOLATED_CPULIST:
		seq_printf(sf, "%*pbl\n", cpumask_pr_args(isolated_cpus));
		break;
	default:
		ret = -EINVAL;
	}

	spin_unlock_irq(&callback_lock);
	return ret;
}

/*
 * cpuset_partition_show() 输出 root/isolated/member 或带 prs_err 原因的 invalid 状态。
 * @seq 提供目标 css，@v 未使用。prs_err 用 READ_ONCE 与写侧配对；返回 0，不锁定
 * 后续状态，因此输出只承诺本次格式化观察。
 */
static int cpuset_partition_show(struct seq_file *seq, void *v)
{
	struct cpuset *cs = css_cs(seq_css(seq));
	const char *err, *type = NULL;

	/* 正状态直接输出类型；负状态先恢复期望类型，再附加 READ_ONCE 取得的失败原因。 */
	switch (cs->partition_root_state) {
	case PRS_ROOT:
		seq_puts(seq, "root\n");
		break;
	case PRS_ISOLATED:
		seq_puts(seq, "isolated\n");
		break;
	case PRS_MEMBER:
		seq_puts(seq, "member\n");
		break;
	case PRS_INVALID_ROOT:
		type = "root";
		fallthrough;
	case PRS_INVALID_ISOLATED:
		if (!type)
			type = "isolated";
		err = perr_strings[READ_ONCE(cs->prs_err)];
		if (err)
			seq_printf(seq, "%s invalid (%s)\n", type, err);
		else
			seq_printf(seq, "%s invalid\n", type);
		break;
	}
	return 0;
}

/*
 * cpuset_partition_write() - cpuset.cpus.partition 的用户写入口。
 * 只接受 root/member/isolated；在完整锁组和 online 检查下调用 update_prstate，
 * 再统一处理调度域/HK 并解锁。成功返回 @nbytes，非法文本返回 -EINVAL，
 * 离线返回 -ENODEV，分配失败可返回 -ENOMEM；@off 不参与整值语义。
 */
static ssize_t cpuset_partition_write(struct kernfs_open_file *of, char *buf,
				     size_t nbytes, loff_t off)
{
	struct cpuset *cs = css_cs(of_css(of));
	int val;
	int retval = -ENODEV;

	buf = strstrip(buf);

	if (!strcmp(buf, "root"))
		val = PRS_ROOT;
	else if (!strcmp(buf, "member"))
		val = PRS_MEMBER;
	else if (!strcmp(buf, "isolated"))
		val = PRS_ISOLATED;
	else
		return -EINVAL;

	cpuset_full_lock();
	if (is_cpuset_online(cs))
		retval = update_prstate(cs, val);
	cpuset_update_sd_hk_unlock();
	return retval ?: nbytes;
}

/*
 * This is currently a minimal set for the default hierarchy. It can be
 * expanded later on by migrating more features and control files from v1.
 */
/*
 * dfl_files 目前只暴露 cgroup v2 所需的最小接口；未来只有在相应 v1 语义完成统一
 * 迁移后才扩展，避免把旧层级专有行为未经适配直接带入默认层级。
 */
static struct cftype dfl_files[] = {
	{
		.name = "cpus",
		.seq_show = cpuset_common_seq_show,
		.write = cpuset_write_resmask,
		.max_write_len = (100U + 6 * NR_CPUS),
		.private = FILE_CPULIST,
		.flags = CFTYPE_NOT_ON_ROOT,
	},

	{
		.name = "mems",
		.seq_show = cpuset_common_seq_show,
		.write = cpuset_write_resmask,
		.max_write_len = (100U + 6 * MAX_NUMNODES),
		.private = FILE_MEMLIST,
		.flags = CFTYPE_NOT_ON_ROOT,
	},

	{
		.name = "cpus.effective",
		.seq_show = cpuset_common_seq_show,
		.private = FILE_EFFECTIVE_CPULIST,
	},

	{
		.name = "mems.effective",
		.seq_show = cpuset_common_seq_show,
		.private = FILE_EFFECTIVE_MEMLIST,
	},

	{
		.name = "cpus.partition",
		.seq_show = cpuset_partition_show,
		.write = cpuset_partition_write,
		.private = FILE_PARTITION_ROOT,
		.flags = CFTYPE_NOT_ON_ROOT,
		.file_offset = offsetof(struct cpuset, partition_file),
	},

	{
		.name = "cpus.exclusive",
		.seq_show = cpuset_common_seq_show,
		.write = cpuset_write_resmask,
		.max_write_len = (100U + 6 * NR_CPUS),
		.private = FILE_EXCLUSIVE_CPULIST,
		.flags = CFTYPE_NOT_ON_ROOT,
	},

	{
		.name = "cpus.exclusive.effective",
		.seq_show = cpuset_common_seq_show,
		.private = FILE_EFFECTIVE_XCPULIST,
		.flags = CFTYPE_NOT_ON_ROOT,
	},

	{
		.name = "cpus.subpartitions",
		.seq_show = cpuset_common_seq_show,
		.private = FILE_SUBPARTS_CPULIST,
		.flags = CFTYPE_ONLY_ON_ROOT | CFTYPE_DEBUG,
	},

	{
		.name = "cpus.isolated",
		.seq_show = cpuset_common_seq_show,
		.private = FILE_ISOLATED_CPULIST,
		.flags = CFTYPE_ONLY_ON_ROOT,
	},

	{ }	/* terminate */
	/* 空 cftype 是表结束哨兵，cgroup core 遍历到此停止。 */
};

/*
 * dfl_files 是 cgroup v2 cpuset 接口表，生命周期为静态只读配置。
 * seq_show/write 回调通过 private 区分用户请求、effective、partition 和调试 mask；
 * file_offset 把 partition_file 嵌入节点交给通知机制。结尾空项是 cgroup core 哨兵。
 */

/**
 * cpuset_css_alloc - Allocate a cpuset css
 * @parent_css: Parent css of the control group that the new cpuset will be
 *              part of
 * Return: cpuset css on success, -ENOMEM on failure.
 *
 * Allocate and initialize a new cpuset css, for non-NULL @parent_css, return
 * top cpuset css otherwise.
 */
/*
 * cpuset_css_alloc() - 为新 cgroup 构造尚未 online 的 cpuset css。
 *
 * @parent_css 为空时返回静态 top css，不转移所有权；非空时分配独立 cpuset/mask，
 * 初始化 v1 状态，并为 v2 默认开启 memory_migrate。成功返回由 cgroup core 接管
 * 生命周期的 css；失败返回 ERR_PTR(-ENOMEM)，尚未发布且已回滚中间分配。
 */
static struct cgroup_subsys_state *
cpuset_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct cpuset *cs;

	if (!parent_css)
		return &top_cpuset.css;

	cs = dup_or_alloc_cpuset(NULL);
	if (!cs)
		return ERR_PTR(-ENOMEM);

	__set_bit(CS_SCHED_LOAD_BALANCE, &cs->flags);
	cpuset1_init(cs);

	/* Set CS_MEMORY_MIGRATE for default hierarchy */
	/* v2 新 cpuset 默认随内存节点变化迁移页面，保持 placement 与有效节点一致。 */
	if (cpuset_v2())
		__set_bit(CS_MEMORY_MIGRATE, &cs->flags);

	return &cs->css;
}

/*
 * cpuset_css_online() 把已分配 css 接入可使用状态并继承父 effective 资源。
 * @css 由 cgroup core 持有；根直接成功。普通节点在完整锁组下同步负载均衡 flag、
 * static key 计数和 v2 effective mask，callback_lock 下发布后调用 v1 hook。
 * 返回 0，无失败分支；函数可睡眠。
 */
static int cpuset_css_online(struct cgroup_subsys_state *css)
{
	struct cpuset *cs = css_cs(css);
	struct cpuset *parent = parent_cs(cs);

	if (!parent)
		return 0;

	cpuset_full_lock();
	/*
	 * For v2, clear CS_SCHED_LOAD_BALANCE if parent is isolated
	 */
	/* isolated 父级不参与负载均衡，v2 子节点上线时必须继承这一边界。 */
	if (cpuset_v2() && !is_sched_load_balance(parent))
		clear_bit(CS_SCHED_LOAD_BALANCE, &cs->flags);

	cpuset_inc();

	spin_lock_irq(&callback_lock);
	if (is_in_v2_mode()) {
		cpumask_copy(cs->effective_cpus, parent->effective_cpus);
		cs->effective_mems = parent->effective_mems;
	}
	spin_unlock_irq(&callback_lock);
	cpuset1_online_css(css);

	cpuset_full_unlock();
	return 0;
}

/*
 * If the cpuset being removed has its flag 'sched_load_balance'
 * enabled, then simulate turning sched_load_balance off, which
 * will call rebuild_sched_domains_locked(). That is not needed
 * in the default hierarchy where only changes in partition
 * will cause repartitioning.
 */
/*
 * cpuset_css_offline() 从对外在线状态撤下 @css。
 * v1 若参与负载均衡先模拟关闭 flag，确保 sched_domain 不再引用该 cpuset；随后减少
 * static key 启用计数。无返回值，cgroup core 仍持对象，最终释放在 css_free。
 */
static void cpuset_css_offline(struct cgroup_subsys_state *css)
{
	struct cpuset *cs = css_cs(css);

	cpuset_full_lock();
	if (!cpuset_v2() && is_sched_load_balance(cs))
		cpuset_update_flag(CS_SCHED_LOAD_BALANCE, cs, 0);

	cpuset_dec();
	cpuset_full_unlock();
}

/*
 * If a dying cpuset has the 'cpus.partition' enabled, turn it off by
 * changing it back to member to free its exclusive CPUs back to the pool to
 * be used by other online cpusets.
 */
/*
 * cpuset_css_killed() 在 css 已死亡但尚未释放时归还 partition 独占 CPU。
 * @css 由 core 保证存活；有效 partition 在完整锁组下转 member，随后统一提交
 * sched_domain/HK 并释放锁。无返回值；invalid partition 已不占有效资源而跳过。
 */
static void cpuset_css_killed(struct cgroup_subsys_state *css)
{
	struct cpuset *cs = css_cs(css);

	cpuset_full_lock();
	/* Reset valid partition back to member */
	/* css 即将消亡时把有效 partition 转回 member，将独占 CPU 归还父级资源池。 */
	if (is_partition_valid(cs))
		update_prstate(cs, PRS_MEMBER);
	cpuset_update_sd_hk_unlock();
}

/*
 * cpuset_css_free() 是非根 cpuset 的 cgroup 最终析构回调。
 * @css 已离线、无外部引用，container 转换得到宿主后交给 free_cpuset。
 * 无返回值，调用后对象及其 mask 不再有效。
 */
static void cpuset_css_free(struct cgroup_subsys_state *css)
{
	struct cpuset *cs = css_cs(css);

	free_cpuset(cs);
}

/*
 * cpuset_bind() 在控制器绑定/挂载模式切换时重设根用户配置语义。
 * @root_css 接口参数未直接使用；函数在 cpuset_mutex+callback_lock 下，v2 使用
 * possible 全集保持用户配置稳定，v1 把配置同步到当前 effective 集合。
 * 无返回值，不改后代；调用者由 cgroup core 串行化挂载生命周期。
 */
static void cpuset_bind(struct cgroup_subsys_state *root_css)
{
	mutex_lock(&cpuset_mutex);
	spin_lock_irq(&callback_lock);

	if (is_in_v2_mode()) {
		cpumask_copy(top_cpuset.cpus_allowed, cpu_possible_mask);
		cpumask_copy(top_cpuset.effective_xcpus, cpu_possible_mask);
		top_cpuset.mems_allowed = node_possible_map;
	} else {
		cpumask_copy(top_cpuset.cpus_allowed,
			     top_cpuset.effective_cpus);
		top_cpuset.mems_allowed = top_cpuset.effective_mems;
	}

	spin_unlock_irq(&callback_lock);
	mutex_unlock(&cpuset_mutex);
}

/*
 * In case the child is cloned into a cpuset different from its parent,
 * additional checks are done to see if the move is allowed.
 */
/*
 * cpuset_can_fork() - 为 CLONE_INTO_CGROUP 的跨 cpuset 子任务做预检。
 *
 * @task 是尚未发布的新任务，@cset 提供目标 cpuset；两者由 fork/cgroup core 持有。
 * 与 current 同 cpuset 时无需动作。否则在 cgroup_mutex+cpuset_mutex 协议下检查
 * 目标资源、任务可迁移性和调度权限；成功增加 attach_in_progress 并返回 0，
 * 失败返回 errno 且不建立预留。函数可能睡眠。
 */
static int cpuset_can_fork(struct task_struct *task, struct css_set *cset)
{
	struct cpuset *cs = css_cs(cset->subsys[cpuset_cgrp_id]);
	bool same_cs;
	int ret;

	rcu_read_lock();
	same_cs = (cs == task_cs(current));
	rcu_read_unlock();

	if (same_cs)
		return 0;

	lockdep_assert_held(&cgroup_mutex);
	mutex_lock(&cpuset_mutex);

	/* Check to see if task is allowed in the cpuset */
	/* 跨 cpuset fork 与普通 attach 使用同一目标可运行性条件。 */
	ret = cpuset_can_attach_check(cs);
	if (ret)
		goto out_unlock;

	ret = task_can_attach(task);
	if (ret)
		goto out_unlock;

	ret = security_task_setscheduler(task);
	if (ret)
		goto out_unlock;

	/*
	 * Mark attach is in progress.  This makes validate_change() fail
	 * changes which zero cpus/mems_allowed.
	 */
	/*
	 * 预检成功后标记迁入进行中，阻止并发配置把目标资源清空；fork 完成或取消时
	 * 由配对回调递减。
	 */
	cs->attach_in_progress++;
out_unlock:
	mutex_unlock(&cpuset_mutex);
	return ret;
}

/*
 * cpuset_cancel_fork() 撤销 can_fork 为跨 cpuset clone 建立的迁入预留。
 * @task 仅用于遵循回调接口，目标来自 @cset；同 cpuset 无动作，否则减计数并可能
 * 唤醒热插拔等待者。无返回值，函数可能获取睡眠锁。
 */
static void cpuset_cancel_fork(struct task_struct *task, struct css_set *cset)
{
	struct cpuset *cs = css_cs(cset->subsys[cpuset_cgrp_id]);
	bool same_cs;

	rcu_read_lock();
	same_cs = (cs == task_cs(current));
	rcu_read_unlock();

	if (same_cs)
		return;

	dec_attach_in_progress(cs);
}

/*
 * Make sure the new task conform to the current state of its parent,
 * which could have been changed by cpuset just after it inherits the
 * state from the parent and before it sits on the cgroup's task list.
 */
/*
 * cpuset_fork() - 在新任务进入 cgroup task list 前修正继承到的资源约束。
 *
 * @task 尚未对普通运行路径发布且由 fork 持有。普通 fork 同 cpuset 时直接复制父
 * 当前 affinity/mems（根无需修正）；CLONE_INTO_CGROUP 则在 cpuset_mutex 下按
 * 目标 effective 集合执行 attach，并解除 can_fork 预留。无返回值；成功返回时
 * 新任务约束与最终目标 cpuset 一致。
 */
static void cpuset_fork(struct task_struct *task)
{
	struct cpuset *cs;
	bool same_cs;

	rcu_read_lock();
	cs = task_cs(task);
	same_cs = (cs == task_cs(current));
	rcu_read_unlock();

	if (same_cs) {
		if (cs == &top_cpuset)
			return;

		set_cpus_allowed_ptr(task, current->cpus_ptr);
		task->mems_allowed = current->mems_allowed;
		return;
	}

	/* CLONE_INTO_CGROUP */
	/* 跨 cgroup clone 使用预检过的目标 cpuset，并在发布新任务前完成资源约束提交。 */
	mutex_lock(&cpuset_mutex);
	guarantee_online_mems(cs, &cpuset_attach_nodemask_to);
	cpuset_attach_task(cs, task);

	dec_attach_in_progress_locked(cs);
	mutex_unlock(&cpuset_mutex);
}

struct cgroup_subsys cpuset_cgrp_subsys = {
	.css_alloc	= cpuset_css_alloc,
	.css_online	= cpuset_css_online,
	.css_offline	= cpuset_css_offline,
	.css_killed	= cpuset_css_killed,
	.css_free	= cpuset_css_free,
	.can_attach	= cpuset_can_attach,
	.cancel_attach	= cpuset_cancel_attach,
	.attach		= cpuset_attach,
	.bind		= cpuset_bind,
	.can_fork	= cpuset_can_fork,
	.cancel_fork	= cpuset_cancel_fork,
	.fork		= cpuset_fork,
#ifdef CONFIG_CPUSETS_V1
	.legacy_cftypes	= cpuset1_files,
#endif
	.dfl_cftypes	= dfl_files,
	.early_init	= true,
	.threaded	= true,
};

/*
 * cpuset_cgrp_subsys 把本文件生命周期/迁移/fork 回调发布给 cgroup core。
 * early_init 使控制器在启动早期可用，threaded 允许 threaded cgroup；v1/v2 文件表
 * 按配置注册。结构为静态全局，core 只借用，不负责释放。
 */

/**
 * cpuset_init - initialize cpusets at system boot
 *
 * Description: Initialize top_cpuset
 **/

/*
 * cpuset_init() - 启动早期建立根 cpuset 和全局 mask 存储。
 *
 * 无参数；在 __init 单线程上下文可睡眠分配。任何关键分配失败都 BUG，因为后续
 * 调度/分配接口无法安全降级。成功把根请求/effective 集合初始化为 possible 全集，
 * 初始化 v1 状态与 attach 工作区，并导入 boot domain 隔离 CPU。返回 0。
 */
int __init cpuset_init(void)
{
	/* 阶段一：为根对象和三个全局 partition/HK 集合分配永久位图，失败无法降级。 */
	BUG_ON(!alloc_cpumask_var(&top_cpuset.cpus_allowed, GFP_KERNEL));
	BUG_ON(!alloc_cpumask_var(&top_cpuset.effective_cpus, GFP_KERNEL));
	BUG_ON(!alloc_cpumask_var(&top_cpuset.effective_xcpus, GFP_KERNEL));
	BUG_ON(!alloc_cpumask_var(&top_cpuset.exclusive_cpus, GFP_KERNEL));
	BUG_ON(!zalloc_cpumask_var(&subpartitions_cpus, GFP_KERNEL));
	BUG_ON(!zalloc_cpumask_var(&isolated_cpus, GFP_KERNEL));
	BUG_ON(!zalloc_cpumask_var(&isolated_hk_cpus, GFP_KERNEL));

	/* 阶段二：在 CPU/node 拓扑完成前先以 possible 全集建立根请求与 effective 初值。 */
	cpumask_setall(top_cpuset.cpus_allowed);
	nodes_setall(top_cpuset.mems_allowed);
	cpumask_setall(top_cpuset.effective_cpus);
	cpumask_setall(top_cpuset.effective_xcpus);
	cpumask_setall(top_cpuset.exclusive_cpus);
	nodes_setall(top_cpuset.effective_mems);

	/* 阶段三：初始化 v1 扩展、attach 工作区，并导入启动时 domain 隔离集合。 */
	cpuset1_init(&top_cpuset);

	BUG_ON(!alloc_cpumask_var(&cpus_attach, GFP_KERNEL));

	if (housekeeping_enabled(HK_TYPE_DOMAIN_BOOT))
		cpumask_andnot(isolated_cpus, cpu_possible_mask,
			       housekeeping_cpumask(HK_TYPE_DOMAIN_BOOT));

	return 0;
}

/*
 * hotplug_update_tasks() 提交一个普通节点热插拔后的 CPU/内存 effective 值。
 * @cs 在线，@new_cpus/@new_mems 是工作区，两个布尔值指出需更新哪类任务约束；
 * 调用者持 cpuset_mutex。普通空 CPU 继承父集合，空 mem 总是继承父集合；在
 * callback_lock 下发布后执行可睡眠的任务更新。无返回值。
 */
static void
hotplug_update_tasks(struct cpuset *cs,
		     struct cpumask *new_cpus, nodemask_t *new_mems,
		     bool cpus_updated, bool mems_updated)
{
	/* A partition root is allowed to have empty effective cpus */
	/* 有效 partition root 可把全部 CPU 分给子 partition；普通节点为空时必须继承父级。 */
	if (cpumask_empty(new_cpus) && !is_partition_valid(cs))
		cpumask_copy(new_cpus, parent_cs(cs)->effective_cpus);
	if (nodes_empty(*new_mems))
		*new_mems = parent_cs(cs)->effective_mems;

	spin_lock_irq(&callback_lock);
	cpumask_copy(cs->effective_cpus, new_cpus);
	cs->effective_mems = *new_mems;
	spin_unlock_irq(&callback_lock);

	if (cpus_updated)
		cpuset_update_tasks_cpumask(cs, new_cpus);
	if (mems_updated)
		cpuset_update_tasks_nodemask(cs);
}

/*
 * cpuset_force_rebuild() 设置当前 CPU 资源事务的 sched_domain 脏位。
 * 调用者遵循 RWCS 规则；无参数、无返回值，实际重建由事务尾部或 hotplug 完成。
 */
void cpuset_force_rebuild(void)
{
	force_sd_rebuild = true;
}

/**
 * cpuset_hotplug_update_tasks - update tasks in a cpuset for hotunplug
 * @cs: cpuset in interest
 * @tmp: the tmpmasks structure pointer
 *
 * Compare @cs's cpu and mem masks against top_cpuset and if some have gone
 * offline, update @cs accordingly.  If @cs ends up with no CPU or memory,
 * all its tasks are moved to the nearest ancestor with both resources.
 */
/*
 * cpuset_hotplug_update_tasks() - 修复单个非根 cpuset 的热插拔派生状态。
 *
 * @cs 由调用者持 online css 引用；@tmp 在 v2 可非空，用于 partition 事务。
 * 函数先等待 attach_in_progress 归零，再取 cpuset_mutex 并二次检查，避免任务被
 * 迁入即将变空的集合。随后重算 CPU/mem，按资源变化使 remote/local partition
 * 失效或恢复，最后走 v2/v1 任务更新。
 * 无返回值；等待和任务/mm 更新均可睡眠，返回时该节点与当前父/active 状态一致。
 */
static void cpuset_hotplug_update_tasks(struct cpuset *cs, struct tmpmasks *tmp)
{
	static cpumask_t new_cpus;
	static nodemask_t new_mems;
	bool cpus_updated;
	bool mems_updated;
	bool remote;
	int partcmd = -1;
	struct cpuset *parent;
retry:
	wait_event(cpuset_attach_wq, cs->attach_in_progress == 0);

	mutex_lock(&cpuset_mutex);

	/*
	 * We have raced with task attaching. We wait until attaching
	 * is finished, so we won't attach a task to an empty cpuset.
	 */
	/*
	 * wait_event 返回与取得 cpuset_mutex 之间仍可能开始一次 attach；锁内二次检查若
	 * 发现竞态就释放锁重试，确保不会一边把 cpuset 变空、一边把任务迁入其中。
	 */
	if (cs->attach_in_progress) {
		mutex_unlock(&cpuset_mutex);
		goto retry;
	}

	parent = parent_cs(cs);
	compute_effective_cpumask(&new_cpus, cs, parent);
	nodes_and(new_mems, cs->mems_allowed, parent->effective_mems);

	if (!tmp || !cs->partition_root_state)
		goto update_tasks;

	/*
	 * Compute effective_cpus for valid partition root, may invalidate
	 * child partition roots if necessary.
	 */
	/*
	 * 有效 partition 的成员 CPU 还要扣除子 root 的 effective_xcpus；计算过程中若
	 * 子级所有权不再满足新拓扑，可同步把子 partition 标为无效。
	 */
	remote = is_remote_partition(cs);
	if (remote || (is_partition_valid(cs) && is_partition_valid(parent)))
		compute_partition_effective_cpumask(cs, &new_cpus);

	if (remote && (cpumask_empty(subpartitions_cpus) ||
			(cpumask_empty(&new_cpus) &&
			 partition_is_populated(cs, NULL)))) {
		cs->prs_err = PERR_HOTPLUG;
		remote_partition_disable(cs, tmp);
		compute_effective_cpumask(&new_cpus, cs, parent);
		remote = false;
	}

	/*
	 * Force the partition to become invalid if either one of
	 * the following conditions hold:
	 * 1) empty effective cpus but not valid empty partition.
	 * 2) parent is invalid or doesn't grant any cpus to child
	 *    partitions.
	 * 3) subpartitions_cpus is empty.
	 */
	/*
	 * 本地 partition 在以下任一情况下强制失效：自身空且不能作为合法空 partition、
	 * 父级无效或不再授予 CPU、全局已无 partition CPU。失效事务会归还现有资源并
	 * 保留负状态供配置恢复。
	 */
	if (is_local_partition(cs) &&
	    (!is_partition_valid(parent) ||
	     tasks_nocpu_error(parent, cs, &new_cpus) ||
	     cpumask_empty(subpartitions_cpus)))
		partcmd = partcmd_invalidate;
	/*
	 * On the other hand, an invalid partition root may be transitioned
	 * back to a regular one with a non-empty effective xcpus.
	 */
	/* 反过来，父级恢复有效且当前仍有 effective_xcpus 时，可尝试把无效 root 恢复为正状态。 */
	else if (is_partition_valid(parent) && is_partition_invalid(cs) &&
		 !cpumask_empty(cs->effective_xcpus))
		partcmd = partcmd_update;

	if (partcmd >= 0) {
		update_parent_effective_cpumask(cs, partcmd, NULL, tmp);
		if ((partcmd == partcmd_invalidate) || is_partition_valid(cs)) {
			compute_partition_effective_cpumask(cs, &new_cpus);
			cpuset_force_rebuild();
		}
	}

update_tasks:
	cpus_updated = !cpumask_equal(&new_cpus, cs->effective_cpus);
	mems_updated = !nodes_equal(new_mems, cs->effective_mems);
	if (!cpus_updated && !mems_updated)
		goto unlock;	/* Hotplug doesn't affect this cpuset */
		/* 两类 effective 资源都未变化时，hotplug 对当前节点无影响，直接解锁。 */

	if (mems_updated)
		check_insane_mems_config(&new_mems);

	if (is_in_v2_mode())
		hotplug_update_tasks(cs, &new_cpus, &new_mems,
				     cpus_updated, mems_updated);
	else
		cpuset1_hotplug_update_tasks(cs, &new_cpus, &new_mems,
					    cpus_updated, mems_updated);

unlock:
	mutex_unlock(&cpuset_mutex);
}

/**
 * cpuset_handle_hotplug - handle CPU/memory hot{,un}plug for a cpuset
 *
 * This function is called after either CPU or memory configuration has
 * changed and updates cpuset accordingly.  The top_cpuset is always
 * synchronized to cpu_active_mask and N_MEMORY, which is necessary in
 * order to make cpusets transparent (of no affect) on systems that are
 * actively using CPU hotplug but making no active use of cpusets.
 *
 * Non-root cpusets are only affected by offlining.  If any CPUs or memory
 * nodes have been taken down, cpuset_hotplug_update_tasks() is invoked on
 * all descendants.
 *
 * Note that CPU offlining during suspend is ignored.  We don't modify
 * cpusets across suspend/resume cycles at all.
 *
 * CPU / memory hotplug is handled synchronously.
 */
/*
 * cpuset_handle_hotplug() - 同步处理一次 CPU 或内存节点拓扑变化。
 *
 * 无参数；入口已持 CPU hotplug 锁。函数先在 cpuset_mutex 下把 top_cpuset 同步到
 * cpu_active_mask/N_MEMORY，并扣除子 partition CPU；再以 RCU+online css 引用
 * 自顶向下修复后代。调度域必须同步重建；housekeeping 更新因可能 flush 工作队列
 * 而延迟到 system_dfl_wq，避免 hotplug 锁反转。无返回值，可睡眠。
 */
static void cpuset_handle_hotplug(void)
{
	static DECLARE_WORK(hk_sd_work, hk_sd_workfn);
	static cpumask_t new_cpus;
	static nodemask_t new_mems;
	bool cpus_updated, mems_updated;
	bool on_dfl = is_in_v2_mode();
	struct tmpmasks tmp, *ptmp = NULL;

	if (on_dfl && !alloc_tmpmasks(&tmp))
		ptmp = &tmp;

	lockdep_assert_cpus_held();
	mutex_lock(&cpuset_mutex);

	/* fetch the available cpus/mems and find out which changed how */
	/* 阶段一：取得新的 active CPU 与 N_MEMORY 快照，作为根 effective 状态的候选值。 */
	cpumask_copy(&new_cpus, cpu_active_mask);
	new_mems = node_states[N_MEMORY];

	/*
	 * If subpartitions_cpus is populated, it is likely that the check
	 * below will produce a false positive on cpus_updated when the cpu
	 * list isn't changed. It is extra work, but it is better to be safe.
	 */
	/*
	 * subpartitions_cpus 非空时，new_cpus 尚未扣除子 partition，直接与根 effective
	 * 比较可能产生“CPU 有变化”的假阳性。这里宁可多做一次传播，也不能漏掉实际
	 * partition/hotplug 差量。
	 */
	cpus_updated = !cpumask_equal(top_cpuset.effective_cpus, &new_cpus) ||
		       !cpumask_empty(subpartitions_cpus);
	mems_updated = !nodes_equal(top_cpuset.effective_mems, new_mems);

	/* For v1, synchronize cpus_allowed to cpu_active_mask */
	/* 阶段二：v1 的根用户 CPU 集合随 active mask 同步；v2 保留 possible 配置语义。 */
	if (cpus_updated) {
		cpuset_force_rebuild();
		spin_lock_irq(&callback_lock);
		if (!on_dfl)
			cpumask_copy(top_cpuset.cpus_allowed, &new_cpus);
		/*
		 * Make sure that CPUs allocated to child partitions
		 * do not show up in effective_cpus. If no CPU is left,
		 * we clear the subpartitions_cpus & let the child partitions
		 * fight for the CPUs again.
		 */
		/*
		 * 根 effective_cpus 不能包含已分给子 partition 的 CPU。若所有 active CPU 都在
		 * subpartitions_cpus 中，先清全局分配记录，让后续自顶向下传播重新竞争并使不再
		 * 满足条件的 partition 失效；否则正常扣除子级集合。
		 */
		if (!cpumask_empty(subpartitions_cpus)) {
			if (cpumask_subset(&new_cpus, subpartitions_cpus)) {
				cpumask_clear(subpartitions_cpus);
			} else {
				cpumask_andnot(&new_cpus, &new_cpus,
					       subpartitions_cpus);
			}
		}
		cpumask_copy(top_cpuset.effective_cpus, &new_cpus);
		spin_unlock_irq(&callback_lock);
		/* we don't mess with cpumasks of tasks in top_cpuset */
		/* 根任务的 affinity 保留 possible CPU，hotplug 本身和调度器负责处理上下线。 */
	}

	/* synchronize mems_allowed to N_MEMORY */
	/* 阶段三：根内存 effective 集合同步 N_MEMORY，并把新节点约束传播给根任务。 */
	if (mems_updated) {
		spin_lock_irq(&callback_lock);
		if (!on_dfl)
			top_cpuset.mems_allowed = new_mems;
		top_cpuset.effective_mems = new_mems;
		spin_unlock_irq(&callback_lock);
		cpuset_update_tasks_nodemask(&top_cpuset);
	}

	mutex_unlock(&cpuset_mutex);

	/* if cpus or mems changed, we need to propagate to descendants */
	/*
	 * 阶段四：根状态发布后自顶向下修复所有在线后代。online css 引用允许退出 RCU
	 * 调用可睡眠 helper；每个节点完成后归还引用再继续遍历。
	 */
	if (cpus_updated || mems_updated) {
		struct cpuset *cs;
		struct cgroup_subsys_state *pos_css;

		rcu_read_lock();
		cpuset_for_each_descendant_pre(cs, pos_css, &top_cpuset) {
			if (cs == &top_cpuset || !css_tryget_online(&cs->css))
				continue;
			rcu_read_unlock();

			cpuset_hotplug_update_tasks(cs, ptmp);

			rcu_read_lock();
			css_put(&cs->css);
		}
		rcu_read_unlock();
	}

	/*
	 * rebuild_sched_domains() will always be called directly if needed
	 * to make sure that newly added or removed CPU will be reflected in
	 * the sched domains. However, if isolated partition invalidation
	 * or recreation is being done (update_housekeeping set), a work item
	 * will be queued to call housekeeping_update() to update the
	 * corresponding housekeeping cpumasks after some slight delay.
	 *
	 * We rely on WORK_STRUCT_PENDING_BIT to not requeue a work item that
	 * is still pending. Before the pending bit is cleared, the work data
	 * is copied out and work item dequeued. So it is possible to queue
	 * the work again before the hk_sd_workfn() is invoked to process the
	 * previously queued work. Since hk_sd_workfn() doesn't use the work
	 * item at all, this is not a problem.
	 */
	/*
	 * 调度域变化必须立即反映 CPU hotplug 结果，所以直接重建。isolated partition
	 * 的 HK 更新则可稍后由 system_dfl_wq 合并执行；WORK_STRUCT_PENDING_BIT 避免
	 * 同一 work 在 pending 状态重复入队。workfn 不读取 work 私有载荷，而是重新
	 * 在锁内读取当前全局状态，因此即使 dequeue 与再次 queue 交错，也不会使用
	 * 过期的 isolated CPU 快照。
	 */
	if (force_sd_rebuild)
		rebuild_sched_domains_cpuslocked();
	if (update_housekeeping)
		queue_work(system_dfl_wq, &hk_sd_work);

	free_tmpmasks(ptmp);
}

/*
 * cpuset_update_active_cpus() 是 CPU hotplug 状态机调用的 cpuset 入口。
 * 无参数、无返回值；调用环境已在 hotplug 临界区，函数同步委托完整处理，不再另行
 * 排队，从而保证返回时 sched_domain 已反映 active CPU。
 */
void cpuset_update_active_cpus(void)
{
	/*
	 * We're inside cpu hotplug critical region which usually nests
	 * inside cgroup synchronization.  Bounce actual hotplug processing
	 * to a work item to avoid reverse locking order.
	 */
	/*
	 * 原注释描述把处理转交 work 以避免“CPU hotplug 锁位于 cgroup 同步内层”造成的
	 * 反向锁序。当前实现已经改为直接同步调用 cpuset_handle_hotplug()；该函数内部只
	 * 把可能 flush 的 housekeeping 更新延迟到 workqueue，sched_domain 仍同步完成。
	 */
	cpuset_handle_hotplug();
}

/*
 * Keep top_cpuset.mems_allowed tracking node_states[N_MEMORY].
 * Call this routine anytime after node_states[N_MEMORY] changes.
 * See cpuset_update_active_cpus() for CPU hotplug handling.
 */
/*
 * cpuset_track_online_nodes() 是 N_MEMORY 变化通知回调。
 * @self/@action/@arg 不承载 cpuset 私有数据；通知链已提供所需 hotplug 序列化。
 * 函数同步修复全层级并返回 NOTIFY_OK，不消费事件、不转移参数所有权。
 */
static int cpuset_track_online_nodes(struct notifier_block *self,
				unsigned long action, void *arg)
{
	cpuset_handle_hotplug();
	return NOTIFY_OK;
}

/**
 * cpuset_init_smp - initialize cpus_allowed
 *
 * Description: Finish top cpuset after cpu, node maps are initialized
 */
/*
 * cpuset_init_smp() - CPU/node mask 可用后完成 cpuset 的 SMP 初始化。
 *
 * 无参数、无返回值，__init 上下文可睡眠。它建立根 old/effective 快照，注册内存
 * hotplug notifier，并创建全局有序 mm 迁移队列；队列分配失败为不可恢复 BUG。
 */
void __init cpuset_init_smp(void)
{
	/*
	 * cpus_allowd/mems_allowed set to v2 values in the initial
	 * cpuset_bind() call will be reset to v1 values in another
	 * cpuset_bind() call when v1 cpuset is mounted.
	 */
	/*
	 * 首次 bind 按 v2 把根用户集合设为 possible 全集；若随后挂载 v1，会再次调用
	 * cpuset_bind()，把 cpus_allowed/mems_allowed 重置为 v1 的当前 effective 语义。
	 */
	top_cpuset.old_mems_allowed = top_cpuset.mems_allowed;

	cpumask_copy(top_cpuset.effective_cpus, cpu_active_mask);
	top_cpuset.effective_mems = node_states[N_MEMORY];

	hotplug_node_notifier(cpuset_track_online_nodes, CPUSET_CALLBACK_PRI);

	cpuset_migrate_mm_wq = alloc_ordered_workqueue("cpuset_migrate_mm", 0);
	BUG_ON(!cpuset_migrate_mm_wq);
}

/*
 * Return cpus_allowed mask from a task's cpuset.
 */
/*
 * __cpuset_cpus_allowed_locked() 计算调度器可采用的任务 cpuset CPU mask。
 * @tsk 是稳定借用任务，@pmask 为输出；调用者持 cpuset_mutex 或 callback_lock。
 * 普通节点向祖先回退保证 active 非空；根节点排除子 partition 后若无 active CPU，
 * 最终退到任务 possible 集合。无返回值，不直接改变任务 affinity。
 */
static void __cpuset_cpus_allowed_locked(struct task_struct *tsk, struct cpumask *pmask)
{
	struct cpuset *cs;

	cs = task_cs(tsk);
	if (cs != &top_cpuset)
		guarantee_active_cpus(tsk, pmask);
	/*
	 * Tasks in the top cpuset won't get update to their cpumasks
	 * when a hotplug online/offline event happens. So we include all
	 * offline cpus in the allowed cpu list.
	 */
	/*
	 * hotplug 不逐任务改写 top_cpuset 成员 affinity，因此根返回值保留所有 possible
	 * CPU（包括暂时 offline），以便 CPU 再上线后任务无需重新配置即可使用。
	 */
	if ((cs == &top_cpuset) || cpumask_empty(pmask)) {
		const struct cpumask *possible_mask = task_cpu_possible_mask(tsk);

		/*
		 * We first exclude cpus allocated to partitions. If there is no
		 * allowable online cpu left, we fall back to all possible cpus.
		 */
		/*
		 * 先扣除已划给 partition 的 CPU；若结果与 active mask 无交集，为避免任务永久
		 * 无处运行，最终退回任务的 possible 全集，由调度器 fallback 选择可行 CPU。
		 */
		cpumask_andnot(pmask, possible_mask, subpartitions_cpus);
		if (!cpumask_intersects(pmask, cpu_active_mask))
			cpumask_copy(pmask, possible_mask);
	}
}

/**
 * cpuset_cpus_allowed_locked - return cpus_allowed mask from a task's cpuset.
 * @tsk: pointer to task_struct from which to obtain cpuset->cpus_allowed.
 * @pmask: pointer to struct cpumask variable to receive cpus_allowed set.
 *
 * Similir to cpuset_cpus_allowed() except that the caller must have acquired
 * cpuset_mutex.
 */
/*
 * cpuset_cpus_allowed_locked() 是已持 cpuset_mutex 调用者的公共包装。
 * 参数均为借用，@pmask 接收结果；函数不睡眠、不取得新引用，lockdep 验证锁契约。
 */
void cpuset_cpus_allowed_locked(struct task_struct *tsk, struct cpumask *pmask)
{
	lockdep_assert_cpuset_lock_held();
	__cpuset_cpus_allowed_locked(tsk, pmask);
}

/**
 * cpuset_cpus_allowed - return cpus_allowed mask from a task's cpuset.
 * @tsk: pointer to task_struct from which to obtain cpuset->cpus_allowed.
 * @pmask: pointer to struct cpumask variable to receive cpus_allowed set.
 *
 * Description: Returns the cpumask_var_t cpus_allowed of the cpuset
 * attached to the specified @tsk.  Guaranteed to return some non-empty
 * subset of cpu_active_mask, even if this means going outside the
 * tasks cpuset, except when the task is in the top cpuset.
 **/

/*
 * cpuset_cpus_allowed() 为无锁调用者短暂获取 callback_lock 并输出稳定 mask。
 * @tsk 必须存活，@pmask 由调用者提供；无直接返回值，返回后 mask 是值快照，
 * 不随 cpuset 后续变化自动更新。
 */
void cpuset_cpus_allowed(struct task_struct *tsk, struct cpumask *pmask)
{
	unsigned long flags;

	spin_lock_irqsave(&callback_lock, flags);
	__cpuset_cpus_allowed_locked(tsk, pmask);
	spin_unlock_irqrestore(&callback_lock, flags);
}

/**
 * cpuset_cpus_allowed_fallback - final fallback before complete catastrophe.
 * @tsk: pointer to task_struct with which the scheduler is struggling
 *
 * Description: In the case that the scheduler cannot find an allowed cpu in
 * tsk->cpus_allowed, we fall back to task_cs(tsk)->cpus_allowed. In legacy
 * mode however, this value is the same as task_cs(tsk)->effective_cpus,
 * which will not contain a sane cpumask during cases such as cpu hotplugging.
 * This is the absolute last resort for the scheduler and it is only used if
 * _every_ other avenue has been traveled.
 *
 * Returns true if the affinity of @tsk was changed, false otherwise.
 **/

/*
 * cpuset_cpus_allowed_fallback() 是调度器找不到可运行 CPU 时的最终 v2 补救。
 * @tsk 由调度器锁协议稳定；函数在 RCU 下无锁读取所属 cpuset 用户 mask，若为
 * possible 子集则强制写入任务 affinity 并返回 true，否则返回 false。
 * 读值可与 attach/update 竞争，但那些路径随后持 task_rq_lock 再次修正；因此这里
 * 允许短暂不精确，只要求避免任务永久无 CPU。
 */
bool cpuset_cpus_allowed_fallback(struct task_struct *tsk)
{
	const struct cpumask *possible_mask = task_cpu_possible_mask(tsk);
	const struct cpumask *cs_mask;
	bool changed = false;

	rcu_read_lock();
	cs_mask = task_cs(tsk)->cpus_allowed;
	if (is_in_v2_mode() && cpumask_subset(cs_mask, possible_mask)) {
		set_cpus_allowed_force(tsk, cs_mask);
		changed = true;
	}
	rcu_read_unlock();

	/*
	 * We own tsk->cpus_allowed, nobody can change it under us.
	 *
	 * But we used cs && cs->cpus_allowed lockless and thus can
	 * race with cgroup_attach_task() or update_cpumask() and get
	 * the wrong tsk->cpus_allowed. However, both cases imply the
	 * subsequent cpuset_change_cpumask()->set_cpus_allowed_ptr()
	 * which takes task_rq_lock().
	 *
	 * If we are called after it dropped the lock we must see all
	 * changes in tsk_cs()->cpus_allowed. Otherwise we can temporary
	 * set any mask even if it is not right from task_cs() pov,
	 * the pending set_cpus_allowed_ptr() will fix things.
	 *
	 * select_fallback_rq() will fix things ups and set cpu_possible_mask
	 * if required.
	 */
	/*
	 * 调度器当前独占 tsk->cpus_allowed，不会被同步改写；但这里对 task_cs 与
	 * cs->cpus_allowed 的读取无锁，可能和 attach/update_cpumask 竞态而临时采用旧
	 * 集合。两条写路径随后都通过持 task_rq_lock 的 set_cpus_allowed_ptr() 修正；若
	 * 本函数发生在其解锁之后则必须看到新值。即使短暂集合不精确，最终的
	 * select_fallback_rq() 仍会在必要时退到 cpu_possible_mask，避免彻底失去 CPU。
	 */
	return changed;
}

/*
 * cpuset_init_current_mems_allowed() 在启动早期把 current 的内存允许集设为所有节点。
 * 无参数、无返回值；此时尚无并发任务需要 seqcount 协议。
 */
void __init cpuset_init_current_mems_allowed(void)
{
	nodes_setall(current->mems_allowed);
}

/**
 * cpuset_mems_allowed - return mems_allowed mask from a tasks cpuset.
 * @tsk: pointer to task_struct from which to obtain cpuset->mems_allowed.
 *
 * Description: Returns the nodemask_t mems_allowed of the cpuset
 * attached to the specified @tsk.  Guaranteed to return some non-empty
 * subset of node_states[N_MEMORY], even if this means going outside the
 * tasks cpuset.
 **/

/*
 * cpuset_mems_allowed() 返回 @tsk 所属 cpuset 可保证非空的在线内存节点值快照。
 * @tsk 为稳定借用任务；函数短暂持 callback_lock，从当前节点向祖先回退。
 * 返回 nodemask 按值交给调用者，不携带引用；函数不改变任务 mems_allowed。
 */
nodemask_t cpuset_mems_allowed(struct task_struct *tsk)
{
	nodemask_t mask;
	unsigned long flags;

	spin_lock_irqsave(&callback_lock, flags);
	guarantee_online_mems(task_cs(tsk), &mask);
	spin_unlock_irqrestore(&callback_lock, flags);

	return mask;
}

/**
 * cpuset_nodemask_valid_mems_allowed - check nodemask vs. current mems_allowed
 * @nodemask: the nodemask to be checked
 *
 * Are any of the nodes in the nodemask allowed in current->mems_allowed?
 */
/*
 * cpuset_nodemask_valid_mems_allowed() 判断输入 @nodemask 是否与 current 的缓存
 * mems_allowed 相交。参数只借用，返回非零表示至少一个可用节点；不加锁，调用者
 * 接受该值是并发 rebind 下的瞬时判断。
 */
int cpuset_nodemask_valid_mems_allowed(nodemask_t *nodemask)
{
	return nodes_intersects(*nodemask, current->mems_allowed);
}

/*
 * nearest_hardwall_ancestor() - Returns the nearest mem_exclusive or
 * mem_hardwall ancestor to the specified cpuset.  Call holding
 * callback_lock.  If no ancestor is mem_exclusive or mem_hardwall
 * (an unusual configuration), then returns the root cpuset.
 */
/*
 * nearest_hardwall_ancestor() 从 @cs 向上寻找最近 mem_exclusive/mem_hardwall 边界。
 * 调用者持 callback_lock；返回的是借用 cpuset 指针，不增加 css 引用，只能在保护
 * 窗口内使用。异常配置下最终返回 top_cpuset，保证搜索终止。
 */
static struct cpuset *nearest_hardwall_ancestor(struct cpuset *cs)
{
	while (!(is_mem_exclusive(cs) || is_mem_hardwall(cs)) && parent_cs(cs))
		cs = parent_cs(cs);
	return cs;
}

/*
 * cpuset_current_node_allowed - Can current task allocate on a memory node?
 * @node: is this an allowed node?
 * @gfp_mask: memory allocation flags
 *
 * If we're in interrupt, yes, we can always allocate.  If @node is set in
 * current's mems_allowed, yes.  If it's not a __GFP_HARDWALL request and this
 * node is set in the nearest hardwalled cpuset ancestor to current's cpuset,
 * yes.  If current has access to memory reserves as an oom victim, yes.
 * If the current task is PF_EXITING, yes. Otherwise, no.
 *
 * GFP_USER allocations are marked with the __GFP_HARDWALL bit,
 * and do not allow allocations outside the current tasks cpuset
 * unless the task has been OOM killed or is exiting.
 * GFP_KERNEL allocations are not so marked, so can escape to the
 * nearest enclosing hardwalled ancestor cpuset.
 *
 * Scanning up parent cpusets requires callback_lock.  The
 * __alloc_pages() routine only calls here with __GFP_HARDWALL bit
 * _not_ set if it's a GFP_KERNEL allocation, and all nodes in the
 * current tasks mems_allowed came up empty on the first pass over
 * the zonelist.  So only GFP_KERNEL allocations, if all nodes in the
 * cpuset are short of memory, might require taking the callback_lock.
 *
 * The first call here from mm/page_alloc:get_page_from_freelist()
 * has __GFP_HARDWALL set in gfp_mask, enforcing hardwall cpusets,
 * so no allocation on a node outside the cpuset is allowed (unless
 * in interrupt, of course).  The PF_EXITING check must therefore
 * come before the __GFP_HARDWALL check, otherwise a dying task
 * would be blocked on the fast path.
 *
 * The second pass through get_page_from_freelist() doesn't even call
 * here for GFP_ATOMIC calls.  For those calls, the __alloc_pages()
 * variable 'wait' is not set, and the bit ALLOC_CPUSET is not set
 * in alloc_flags.  That logic and the checks below have the combined
 * affect that:
 *	in_interrupt - any node ok (current task context irrelevant)
 *	GFP_ATOMIC   - any node ok
 *	tsk_is_oom_victim   - any node ok
 *	PF_EXITING   - any node ok (let dying task exit quickly)
 *	GFP_KERNEL   - any node in enclosing hardwalled cpuset ok
 *	GFP_USER     - only nodes in current tasks mems allowed ok.
 */
/*
 * cpuset_current_node_allowed() - 页分配器判断 current 能否逃逸到 @node。
 *
 * @node 是 NUMA 节点号，@gfp_mask 决定 hardwall/原子语义。中断、已允许节点、
 * OOM victim 和退出任务优先放行；__GFP_HARDWALL 拒绝越界。v2 对非 hardwall
 * 内核分配允许逃逸；v1 在 callback_lock 下只放宽到最近 hardwall 祖先。
 * 返回布尔值，无分配、不可睡眠；顺序保证垂死任务不会被 hardwall 阻塞退出。
 */
bool cpuset_current_node_allowed(int node, gfp_t gfp_mask)
{
	struct cpuset *cs;		/* current cpuset ancestors */
	/* cs 是沿 current 所属 cpuset 向上找到的最近 hardwall 祖先借用指针。 */
	bool allowed;			/* is allocation in zone z allowed? */
	/* allowed 记录目标 NUMA zone 是否位于该 hardwall 边界允许集合中。 */
	unsigned long flags;

	if (in_interrupt())
		return true;
	if (node_isset(node, current->mems_allowed))
		return true;
	/*
	 * Allow tasks that have access to memory reserves because they have
	 * been OOM killed to get memory anywhere.
	 */
	/* OOM victim 已获准使用内存保留，允许跨 cpuset 节点分配以完成回收或退出。 */
	if (unlikely(tsk_is_oom_victim(current)))
		return true;
	if (current->flags & PF_EXITING) /* Let dying task have memory */
		/* 垂死任务优先获得内存以完成退出，必须早于 hardwall 拒绝分支。 */
		return true;
	if (gfp_mask & __GFP_HARDWALL)	/* If hardwall request, stop here */
		/* 用户/hardwall 分配不得逃逸 current->mems_allowed。 */
		return false;

	if (cpuset_v2())
		return true;

	/* Not hardwall and node outside mems_allowed: scan up cpusets */
	/* v1 内核分配可向上放宽到最近 hardwall 祖先；callback_lock 稳定层级和 mask。 */
	spin_lock_irqsave(&callback_lock, flags);

	cs = nearest_hardwall_ancestor(task_cs(current));
	allowed = node_isset(node, cs->mems_allowed);

	spin_unlock_irqrestore(&callback_lock, flags);
	return allowed;
}

/**
 * cpuset_nodes_allowed - return effective_mems mask from a cgroup cpuset.
 * @cgroup: pointer to struct cgroup.
 * @mask: pointer to struct nodemask_t to be returned.
 *
 * Returns effective_mems mask from a cgroup cpuset if it is cgroup v2 and
 * has cpuset subsys. Otherwise, returns node_states[N_MEMORY].
 *
 * This function intentionally avoids taking the cpuset_mutex or callback_lock
 * when accessing effective_mems. This is because the obtained effective_mems
 * is stale immediately after the query anyway (e.g., effective_mems is updated
 * immediately after releasing the lock but before returning).
 *
 * As a result, returned @mask may be empty because cs->effective_mems can be
 * rebound during this call. Besides, nodes in @mask are not guaranteed to be
 * online due to hot plugins. Callers should check the mask for validity on
 * return based on its subsequent use.
 **/
/*
 * cpuset_nodes_allowed() 为指定 @cgroup 输出 v2 effective_mems 的低成本提示快照。
 * @cgroup 可空，@mask 为输出。v1、无控制器 css 时返回 N_MEMORY；成功取得 e_css
 * 引用只保证对象存活，不冻结字段，因此有意无锁复制并允许空/离线的瞬时结果。
 * 无返回值，调用者必须按后续用途重新验证 mask；函数归还 css 引用。
 */
void cpuset_nodes_allowed(struct cgroup *cgroup, nodemask_t *mask)
{
	struct cgroup_subsys_state *css;
	struct cpuset *cs;

	/*
	 * In v1, mem_cgroup and cpuset are unlikely in the same hierarchy
	 * and mems_allowed is likely to be empty even if we could get to it,
	 * so return directly to avoid taking a global lock on the empty check.
	 */
	/*
	 * v1 中 memcg 与 cpuset 通常不在同一层级，即使查到 css，mems_allowed 也很可能
	 * 为空；直接返回当前 N_MEMORY，避免为了无价值的空判断取得全局锁。空 cgroup
	 * 同样使用该安全默认值。
	 */
	if (!cgroup || !cpuset_v2()) {
		nodes_copy(*mask, node_states[N_MEMORY]);
		return;
	}

	css = cgroup_get_e_css(cgroup, &cpuset_cgrp_subsys);
	if (!css) {
		nodes_copy(*mask, node_states[N_MEMORY]);
		return;
	}

	/*
	 * The reference taken via cgroup_get_e_css is sufficient to
	 * protect css, but it does not imply safe accesses to effective_mems.
	 *
	 * Normally, accessing effective_mems would require the cpuset_mutex
	 * or callback_lock - but the correctness of this information is stale
	 * immediately after the query anyway. We do not acquire the lock
	 * during this process to save lock contention in exchange for racing
	 * against mems_allowed rebinds.
	 */
	/*
	 * cgroup_get_e_css() 的引用只保证 css 存储期，不保证 effective_mems 字段一致。
	 * 正常稳定读取需要 cpuset_mutex 或 callback_lock，但即使持锁复制，返回前字段仍
	 * 可能变化；因此这里有意用可能与 rebind 竞态的无锁快照换取较低锁争用，调用者
	 * 必须接受空或已离线节点并按用途重新校验。
	 */
	cs = container_of(css, struct cpuset, css);
	nodes_copy(*mask, cs->effective_mems);
	css_put(css);
}

/**
 * cpuset_spread_node() - On which node to begin search for a page
 * @rotor: round robin rotor
 *
 * If a task is marked PF_SPREAD_PAGE or PF_SPREAD_SLAB (as for
 * tasks in a cpuset with is_spread_page or is_spread_slab set),
 * and if the memory allocation used cpuset_mem_spread_node()
 * to determine on which node to start looking, as it will for
 * certain page cache or slab cache pages such as used for file
 * system buffers and inode caches, then instead of starting on the
 * local node to look for a free page, rather spread the starting
 * node around the tasks mems_allowed nodes.
 *
 * We don't have to worry about the returned node being offline
 * because "it can't happen", and even if it did, it would be ok.
 *
 * The routines calling guarantee_online_mems() are careful to
 * only set nodes in task->mems_allowed that are online.  So it
 * should not be possible for the following code to return an
 * offline node.  But if it did, that would be ok, as this routine
 * is not returning the node where the allocation must be, only
 * the node where the search should start.  The zonelist passed to
 * __alloc_pages() will include all nodes.  If the slab allocator
 * is passed an offline node, it will fall back to the local node.
 * See kmem_cache_alloc_node().
 */
/*
 * cpuset_spread_node() 在 current->mems_allowed 中把 @rotor 推进到下一节点。
 * @rotor 是输入输出轮转游标，返回值与写回值相同；它只选择搜索起点，不承诺最终
 * 分配节点，因此瞬时离线也可由 zonelist/slab fallback 处理。
 */
static int cpuset_spread_node(int *rotor)
{
	return *rotor = next_node_in(*rotor, current->mems_allowed);
}

/**
 * cpuset_mem_spread_node() - On which node to begin search for a file page
 */
/*
 * cpuset_mem_spread_node() 为页缓存等 spread 分配选择本任务的轮转起点。
 * 无参数；首次用随机允许节点初始化 per-task rotor，之后循环推进。
 * 返回 NUMA 节点号，不分配页面、不锁定节点在线状态。
 */
int cpuset_mem_spread_node(void)
{
	if (current->cpuset_mem_spread_rotor == NUMA_NO_NODE)
		current->cpuset_mem_spread_rotor =
			node_random(&current->mems_allowed);

	return cpuset_spread_node(&current->cpuset_mem_spread_rotor);
}

/**
 * cpuset_mems_allowed_intersects - Does @tsk1's mems_allowed intersect @tsk2's?
 * @tsk1: pointer to task_struct of some task.
 * @tsk2: pointer to task_struct of some other task.
 *
 * Description: Return true if @tsk1's mems_allowed intersects the
 * mems_allowed of @tsk2.  Used by the OOM killer to determine if
 * one of the task's memory usage might impact the memory available
 * to the other.
 **/

/*
 * cpuset_mems_allowed_intersects() 比较两个稳定借用任务的缓存 mems_allowed。
 * 返回非零表示内存可用范围相交，OOM killer 用它判断资源影响域；函数不加锁，
 * 接受并发 rebind 下的瞬时结果，不取得任务引用。
 */
int cpuset_mems_allowed_intersects(const struct task_struct *tsk1,
				   const struct task_struct *tsk2)
{
	return nodes_intersects(tsk1->mems_allowed, tsk2->mems_allowed);
}

/**
 * cpuset_print_current_mems_allowed - prints current's cpuset and mems_allowed
 *
 * Description: Prints current's name, cpuset name, and cached copy of its
 * mems_allowed to the kernel log.
 */
/*
 * cpuset_print_current_mems_allowed() 把 current 的 cpuset 名和缓存 mems_allowed
 * 追加到当前内核日志行。RCU 保证 cgroup 名称对象存活；无参数、无返回值，
 * 只产生诊断输出，不提供配置的一致事务快照。
 */
void cpuset_print_current_mems_allowed(void)
{
	struct cgroup *cgrp;

	rcu_read_lock();

	cgrp = task_cs(current)->css.cgroup;
	pr_cont(",cpuset=");
	pr_cont_cgroup_name(cgrp);
	pr_cont(",mems_allowed=%*pbl",
		nodemask_pr_args(&current->mems_allowed));

	rcu_read_unlock();
}

/*
 * cpuset_task_status_allowed() 为 /proc/<pid>/status 输出位图和列表两种 mems_allowed。
 * @m 为 seq_file 输出端，@task 由 proc 读取路径稳定；无返回值、不加 cpuset 锁，
 * 两行反映读取时任务缓存的尽力快照。
 */
/* Display task mems_allowed in /proc/<pid>/status file. */
/* 为 /proc/<pid>/status 输出任务 mems_allowed 的位图和列表格式。 */
void cpuset_task_status_allowed(struct seq_file *m, struct task_struct *task)
{
	seq_printf(m, "Mems_allowed:\t%*pb\n",
		   nodemask_pr_args(&task->mems_allowed));
	seq_printf(m, "Mems_allowed_list:\t%*pbl\n",
		   nodemask_pr_args(&task->mems_allowed));
}
