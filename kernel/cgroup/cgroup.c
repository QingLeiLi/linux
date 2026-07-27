// SPDX-License-Identifier: GPL-2.0
/*
 * cgroup 核心实现学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5），2026-07-27。
 * 分析基线：分支 doc/lql，提交 83476cc97bc6。
 * 方法依据：doc/linux-kernel-source-learning-methodology.md。
 *
 * 文件职责：
 *   实现 cgroup v1/v2 层级的挂载与重配置、cgroup/css/css_set 生命周期、
 *   task 迁移、控制器启停、kernfs 文件接口、创建销毁以及 fork/exit
 *   挂接。本文件是 cgroup core 的状态编排层；具体资源策略由各
 *   cgroup_subsys 回调实现，kernfs/VFS 负责目录文件和挂载通用机制。
 *
 * 核心对象：
 *   cgroup_root  一棵层级及其控制器集合，挂入 cgroup_roots 后可发现；
 *   cgroup       层级中的目录节点，self 是 core 自身的 css；
 *   css          某控制器在某 cgroup 上的状态，经历 alloc -> online
 *                -> kill/offline -> release/free；
 *   css_set      一个 task 在所有层级/控制器上的成员关系组合，多 task
 *                可共享；task->cgroups 通过 RCU 指向它；
 *   cftype       控制文件描述，发布为 kernfs node 后由文件操作借用。
 *
 * 主路径：
 *   mount/reconfigure -> cgroup_setup_root()/rebind_subsystems()
 *   mkdir             -> cgroup_mkdir() -> css_create()/online_css()
 *   写 cgroup.procs   -> cgroup_migrate_*() -> subsystem attach 回调
 *   写 subtree_control-> apply/finish control -> css online/offline
 *   rmdir             -> cgroup_destroy_locked() -> kill_css_*()
 *                       -> offline/release/free 三段工作队列
 *   fork/exit          -> cgroup_can_fork()/post_fork()
 *                       -> cgroup_task_exit()/dead()/free()
 *
 * 并发模型：
 *   cgroup_mutex 串行化层级结构和控制器绑定；css_set_lock 保护 css_set
 *   索引、task 成员链和 task->cgroups 更新；cgroup_threadgroup_rwsem
 *   协调线程组迁移与 fork/exit；RCU 让读者观察已发布的 cgroup/css_set
 *   指针；percpu_ref 把 css 的 online 引用与最终 kill/release 分开。
 *   kernfs active reference 保证文件回调期间关联 css 不被摘除。
 *
 * 方案权衡：
 *   共享 css_set 避免每 task 保存完整控制器组合，代价是迁移时需
 *   哈希查找、预装载和批量重连。销毁被拆成 offline/release/free
 *   工作队列以满足
 *   回调可睡眠和依赖顺序，代价是对象摘除后仍可能延迟存活。v2 的
 *   domain/threaded 模型提供进程级资源域与线程级控制器共存，但要求
 *   持续维护 dom_cgrp、subtree_control 和 populated 计数不变量。
 */
/*
 *  Generic process-grouping system.
 *
 *  Based originally on the cpuset system, extracted by Paul Menage
 *  Copyright (C) 2006 Google, Inc
 *
 *  Notifications support
 *  Copyright (C) 2009 Nokia Corporation
 *  Author: Kirill A. Shutemov
 *
 *  Copyright notices from the original cpuset code:
 *  --------------------------------------------------
 *  Copyright (C) 2003 BULL SA.
 *  Copyright (C) 2004-2006 Silicon Graphics, Inc.
 *
 *  Portions derived from Patrick Mochel's sysfs code.
 *  sysfs is Copyright (c) 2001-3 Patrick Mochel
 *
 *  2003-10-10 Written by Simon Derr.
 *  2003-10-22 Updates by Stephen Hemminger.
 *  2004 May-July Rework by Paul Jackson.
 *  ---------------------------------------------------
 */
/*
 * 中文对译与学习补充：
 * 本文件实现通用的进程分组系统。最初从 Paul Menage 拆出的 cpuset
 * 机制演化而来，通知支持由 Nokia 贡献；其中还保留了原 cpuset 与
 * Patrick Mochel sysfs 代码的版权和主要演进记录。
 *
 * 这些来源说明了 cgroup core 的两条历史主线：一条负责把 task 组织成
 * 可共享的层级集合，另一条借助 sysfs/kernfs 把层级及 controller 状态
 * 暴露给用户空间；当前实现仍围绕这两类对象的生命周期协同展开。
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "cgroup-internal.h"

#include <linux/bpf-cgroup.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/init_task.h>
#include <linux/kernel.h>
#include <linux/magic.h>
#include <linux/mutex.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/percpu-rwsem.h>
#include <linux/string.h>
#include <linux/hashtable.h>
#include <linux/idr.h>
#include <linux/kthread.h>
#include <linux/atomic.h>
#include <linux/cpuset.h>
#include <linux/proc_ns.h>
#include <linux/nsproxy.h>
#include <linux/file.h>
#include <linux/fs_parser.h>
#include <linux/sched/cputime.h>
#include <linux/sched/deadline.h>
#include <linux/psi.h>
#include <linux/nstree.h>
#include <linux/irq_work.h>
#include <net/sock.h>

#define CREATE_TRACE_POINTS
#include <trace/events/cgroup.h>

#define CGROUP_FILE_NAME_MAX		(MAX_CGROUP_TYPE_NAMELEN +	\
					 MAX_CFTYPE_NAME + 2)
/* let's not notify more than 100 times per second */
/*
 * 文件通知最短间隔限制为每秒至多 100 次，合并高频状态变化，避免
 * poll/inotify 消费者和 timer 被控制器更新风暴淹没。
 */
#define CGROUP_FILE_NOTIFY_MIN_INTV	DIV_ROUND_UP(HZ, 100)

/*
 * cgroup_mutex is the master lock.  Any modification to cgroup or its
 * hierarchy must be performed while holding it.
 *
 * css_set_lock protects task->cgroups pointer, the list of css_set
 * objects, and the chain of tasks off each css_set.
 *
 * These locks are exported if CONFIG_PROVE_RCU so that accessors in
 * cgroup.h can use them for lockdep annotations.
 */
/*
 * 中文锁图：
 * - cgroup_mutex：层级拓扑、控制器绑定、css 创建/摘除等慢路径总锁，
 *   允许睡眠；
 * - css_set_lock：IRQ-safe 自旋锁，保护 task 成员链、css_set 全局索引
 *   及 task->cgroups 发布；引用只保证存活，字段一致性仍依赖此锁/RCU；
 * - CONFIG_PROVE_RCU/LOCKDEP 导出锁对象只是让跨文件 accessor 能表达
 *   lockdep 条件，并未把锁变成公共修改接口。
 */
DEFINE_MUTEX(cgroup_mutex);
DEFINE_SPINLOCK(css_set_lock);

#if (defined CONFIG_PROVE_RCU || defined CONFIG_LOCKDEP)
EXPORT_SYMBOL_GPL(cgroup_mutex);
EXPORT_SYMBOL_GPL(css_set_lock);
#endif

struct blocking_notifier_head cgroup_lifetime_notifier =
	BLOCKING_NOTIFIER_INIT(cgroup_lifetime_notifier);

/*
 * trace_cgroup_path_lock 串行化共享 trace 路径暂存区；tracepoint 构造路径
 * 时短暂借用 trace_cgroup_path，不能在解锁后保存其指针。
 * cgroup_debug 是启动参数开启的只读热点开关。
 */
DEFINE_SPINLOCK(trace_cgroup_path_lock);
char trace_cgroup_path[TRACE_CGROUP_PATH_LEN];
static bool cgroup_debug __read_mostly;

/*
 * Protects cgroup_idr and css_idr so that IDs can be released without
 * grabbing cgroup_mutex.
 */
/*
 * IDR 删除可能发生在延迟释放阶段，不能要求重新取得 cgroup_mutex；
 * 此 BH-safe 锁只保护 ID->对象映射本身，不保护对象其余字段或生命周期。
 */
static DEFINE_SPINLOCK(cgroup_idr_lock);

/*
 * 线程组读写信号量让 fork/exit 等读侧与整组迁移写侧互斥。是否在所有
 * 路径启用由 cgroup_enable_per_threadgroup_rwsem 的单向静态状态决定。
 */
DEFINE_PERCPU_RWSEM(cgroup_threadgroup_rwsem);

/* accessor 的 lockdep 契约：调用者必须持层级总锁或处于 RCU 读侧。 */
#define cgroup_assert_mutex_or_rcu_locked()				\
	RCU_LOCKDEP_WARN(!rcu_read_lock_held() &&			\
			   !lockdep_is_held(&cgroup_mutex),		\
			   "cgroup_mutex or RCU read lock required");

/*
 * cgroup destruction makes heavy use of work items and there can be a lot
 * of concurrent destructions.  Use a separate workqueue so that cgroup
 * destruction work items don't end up filling up max_active of system_percpu_wq
 * which may lead to deadlock.
 *
 * A cgroup destruction should enqueue work sequentially to:
 * cgroup_offline_wq: use for css offline work
 * cgroup_release_wq: use for css release work
 * cgroup_free_wq: use for free work
 *
 * Rationale for using separate workqueues:
 * The cgroup root free work may depend on completion of other css offline
 * operations. If all tasks were enqueued to a single workqueue, this could
 * create a deadlock scenario where:
 * - Free work waits for other css offline work to complete.
 * - But other css offline work is queued after free work in the same queue.
 *
 * Example deadlock scenario with single workqueue (cgroup_destroy_wq):
 * 1. umount net_prio
 * 2. net_prio root destruction enqueues work to cgroup_destroy_wq (CPUx)
 * 3. perf_event CSS A offline enqueues work to same cgroup_destroy_wq (CPUx)
 * 4. net_prio cgroup_destroy_root->cgroup_lock_and_drain_offline.
 * 5. net_prio root destruction blocks waiting for perf_event CSS A offline,
 *    which can never complete as it's behind in the same queue and
 *    workqueue's max_active is 1.
 */
/*
 * 中文补充：销毁是明确的三级流水线，不能合并成一个 max_active 受限
 * 队列。offline 先阻止控制器继续提供服务，release 等引用归零后拆除
 * 外部关系，free 最终归还内存。分队列既维持先后依赖，也避免“较早
 * 排队的 free 等待同队列中排在其后的 offline”形成自阻塞。
 */
static struct workqueue_struct *cgroup_offline_wq;
static struct workqueue_struct *cgroup_release_wq;
static struct workqueue_struct *cgroup_free_wq;

/* generate an array of cgroup subsystem pointers */
/*
 * 多次包含 cgroup_subsys.h 并重定义 SUBSYS，是 X-macro：同一控制器清单
 * 生成按 ssid 索引的对象表、名称表和 static-key 表，避免编号漂移。
 */
#define SUBSYS(_x) [_x ## _cgrp_id] = &_x ## _cgrp_subsys,
struct cgroup_subsys *cgroup_subsys[] = {
#include <linux/cgroup_subsys.h>
};
#undef SUBSYS

/* array of cgroup subsystem names */
/* 与 cgroup_subsys[] 使用相同 ssid，下标可稳定映射到用户可见名称。 */
#define SUBSYS(_x) [_x ## _cgrp_id] = #_x,
static const char *cgroup_subsys_name[] = {
#include <linux/cgroup_subsys.h>
};
#undef SUBSYS

/* array of static_keys for cgroup_subsys_enabled() and cgroup_subsys_on_dfl() */
/*
 * 每个控制器生成“全局启用”和“位于默认层级”两个 jump-label。
 * 热路径可近似零成本跳过关闭控制器；绑定变化的慢路径负责更新 key。
 */
#define SUBSYS(_x)								\
	DEFINE_STATIC_KEY_TRUE(_x ## _cgrp_subsys_enabled_key);			\
	DEFINE_STATIC_KEY_TRUE(_x ## _cgrp_subsys_on_dfl_key);			\
	EXPORT_SYMBOL_GPL(_x ## _cgrp_subsys_enabled_key);			\
	EXPORT_SYMBOL_GPL(_x ## _cgrp_subsys_on_dfl_key);
#include <linux/cgroup_subsys.h>
#undef SUBSYS

#define SUBSYS(_x) [_x ## _cgrp_id] = &_x ## _cgrp_subsys_enabled_key,
static struct static_key_true *cgroup_subsys_enabled_key[] = {
#include <linux/cgroup_subsys.h>
};
#undef SUBSYS

#define SUBSYS(_x) [_x ## _cgrp_id] = &_x ## _cgrp_subsys_on_dfl_key,
static struct static_key_true *cgroup_subsys_on_dfl_key[] = {
#include <linux/cgroup_subsys.h>
};
#undef SUBSYS

/*
 * default root 的两组永久 per-CPU 统计存储：root_rstat_cpu 保存 controller
 * css rstat 刷新状态，root_rstat_base_cpu 保存 core CPU 基础时间。启动时
 * 由 cgrp_dfl_root 指向，写者在本 CPU 记账，聚合读者按 rstat 锁协议刷新；
 * 它们与静态 default root 同寿命，不参与普通 cgroup 分配和释放。
 */
static DEFINE_PER_CPU(struct css_rstat_cpu, root_rstat_cpu);
static DEFINE_PER_CPU(struct cgroup_rstat_base_cpu, root_rstat_base_cpu);

/* the default hierarchy */
/*
 * v2 默认根在编译期永久存在，根 css 的 rstat 指向专用 per-CPU 存储。
 * 它不走普通 cgroup 分配路径，启动初始化后成为所有 v2 节点的祖先。
 */
struct cgroup_root cgrp_dfl_root = {
	.cgrp.self.rstat_cpu = &root_rstat_cpu,
	.cgrp.rstat_base_cpu = &root_rstat_base_cpu,
};
EXPORT_SYMBOL_GPL(cgrp_dfl_root);

/*
 * The default hierarchy always exists but is hidden until mounted for the
 * first time.  This is for backward compatibility.
 */
/*
 * visible 由首次成功挂载发布且之后保持可见；隐藏初始状态避免未挂载 v2
 * 时改变旧系统观察到的层级集合。
 */
bool cgrp_dfl_visible;

/* some controllers are not supported in the default hierarchy */
/* 位为 1 的控制器即使存在也不能通过 v2 对用户暴露。 */
static u32 cgrp_dfl_inhibit_ss_mask;

/* some controllers are implicitly enabled on the default hierarchy */
/* 隐式控制器参与内部 css 关联，但不作为可显式切换的 subtree 控制位。 */
static u32 cgrp_dfl_implicit_ss_mask;

/* some controllers can be threaded on the default hierarchy */
/* threaded mask 限定能跨线程域工作的控制器，其他控制器只能驻留 domain。 */
static u32 cgrp_dfl_threaded_ss_mask;

/*
 * Set across rebind_subsystems() to the controllers leaving a hierarchy.
 * Guarded by cgroup_mutex. Makes find_existing_css_set() resolve them to the
 * root css so the affected tasks are migrated there before
 * cgroup_apply_control_disable() kills the per-cgroup csses.
 */
/*
 * 这是 rebind 的短期“逻辑已离开、物理 css 尚未销毁”状态。持锁读者把
 * 这些控制器解析到 root css，先迁走 task 关联，再安全 kill 旧 css。
 */
static u32 cgroup_rebind_ss_mask;

/* The list of hierarchy roots */
/* cgroup_mutex 下维护所有已发布 root；count 与链表成员数同步。 */
LIST_HEAD(cgroup_roots);
static int cgroup_root_count;

/* hierarchy ID allocation and mapping, protected by cgroup_mutex */
/* 层级 ID 在 root 发布期有效；cgroup_mutex 串行化分配、查找和删除。 */
static DEFINE_IDR(cgroup_hierarchy_idr);

/*
 * Assign a monotonically increasing serial number to csses.  It guarantees
 * cgroups with bigger numbers are newer than those with smaller numbers.
 * Also, as csses are always appended to the parent's ->children list, it
 * guarantees that sibling csses are always sorted in the ascending serial
 * number order on the list.  Protected by cgroup_mutex.
 */
/*
 * 单调序号不回收，既提供创建先后关系，也依靠 children 尾插建立兄弟有序
 * 不变量；遍历器可据此判断新旧节点。只有 cgroup_mutex 写者推进。
 */
static u64 css_serial_nr_next = 1;

/*
 * These bitmasks identify subsystems with specific features to avoid
 * having to do iterative checks repeatedly.
 */
/*
 * 四个位图在启动时汇总各 subsystem operation 是否存在。fork/exit 热路径
 * 先测位图再分派回调，避免每次遍历并检查 NULL；初始化后只读。
 */
static u32 have_fork_callback __read_mostly;
static u32 have_exit_callback __read_mostly;
static u32 have_release_callback __read_mostly;
static u32 have_canfork_callback __read_mostly;

/*
 * 控制器动态模块偏好：配置给出初值，启动参数可单向确定最终策略，
 * init 后只读。它在“预创建更多 css”与“按需降低模块开销”之间取舍。
 */
static bool have_favordynmods __ro_after_init = IS_ENABLED(CONFIG_CGROUP_FAVOR_DYNMODS);

/*
 * Write protected by cgroup_mutex and write-lock of cgroup_threadgroup_rwsem,
 * read protected by either.
 *
 * Can only be turned on, but not turned off.
 */
/*
 * 读写双方可任选 cgroup_mutex 或 threadgroup rwsem 保护该单向布尔值。
 * 一旦启用就不关闭，避免读者在锁协议切换途中观察不兼容的迁移规则。
 */
bool cgroup_enable_per_threadgroup_rwsem __read_mostly;

/* cgroup namespace for init task */
/*
 * 初始 cgroup namespace 永久存在，借用 init_user_ns，并以 init_css_set
 * 作为命名空间根视图；普通 namespace 从它派生并独立引用 root_cset。
 */
struct cgroup_namespace init_cgroup_ns = {
	.ns		= NS_COMMON_INIT(init_cgroup_ns),
	.user_ns	= &init_user_ns,
	.root_cset	= &init_css_set,
};

static struct file_system_type cgroup2_fs_type;
static struct cftype cgroup_base_files[];
static struct cftype cgroup_psi_files[];

/* cgroup optional features */
/* 枚举下标同时索引名称表和 disable mask；条件编译必须保持两者同序。 */
enum cgroup_opt_features {
#ifdef CONFIG_PSI
	OPT_FEATURE_PRESSURE,
#endif
	OPT_FEATURE_COUNT
};

static const char *cgroup_opt_feature_names[OPT_FEATURE_COUNT] = {
#ifdef CONFIG_PSI
	"pressure",
#endif
};

/*
 * 可选用户接口禁用位图，启动配置写入、运行期频繁读取；位为 1 时对应
 * cgroup2 文件不发布。u16 足以覆盖当前 OPT_FEATURE_COUNT。
 */
static u16 cgroup_feature_disable_mask __read_mostly;

static int cgroup_apply_control(struct cgroup *cgrp);
static void cgroup_finalize_control(struct cgroup *cgrp, int ret);
static void css_task_iter_skip(struct css_task_iter *it,
			       struct task_struct *task);
static int cgroup_destroy_locked(struct cgroup *cgrp);
static void kill_css_sync(struct cgroup_subsys_state *css);
static void kill_css_finish(struct cgroup_subsys_state *css);
static struct cgroup_subsys_state *css_create(struct cgroup *cgrp,
					      struct cgroup_subsys *ss);
static void css_release(struct percpu_ref *ref);
static int cgroup_addrm_files(struct cgroup_subsys_state *css,
			      struct cgroup *cgrp, struct cftype cfts[],
			      bool is_add);
static void cgroup_rt_init(void);

#ifdef CONFIG_DEBUG_CGROUP_REF
#define CGROUP_REF_FN_ATTRS	noinline
#define CGROUP_REF_EXPORT(fn)	EXPORT_SYMBOL_GPL(fn);
#include <linux/cgroup_refcnt.h>
#endif

/**
 * cgroup_ssid_enabled - cgroup subsys enabled test by subsys ID
 * @ssid: subsys ID of interest
 *
 * cgroup_subsys_enabled() can only be used with literal subsys names which
 * is fine for individual subsystems but unsuitable for cgroup core.  This
 * is slower static_key_enabled() based test indexed by @ssid.
 */
/*
 * 中文契约：ssid 是 [0, CGROUP_SUBSYS_COUNT) 的控制器编号，由调用者保证
 * 有效。本函数是 core 需要运行期索引时的只读查询，不睡眠、不取引用。
 * 未编译任何控制器时返回 false；否则读取对应 static key。返回 true 只
 * 表示控制器全局启用，不保证某个 cgroup 已创建该控制器的 css。
 */
bool cgroup_ssid_enabled(int ssid)
{
	if (!CGROUP_HAS_SUBSYS_CONFIG)
		return false;

	return static_key_enabled(cgroup_subsys_enabled_key[ssid]);
}

/**
 * cgroup_on_dfl - test whether a cgroup is on the default hierarchy
 * @cgrp: the cgroup of interest
 *
 * The default hierarchy is the v2 interface of cgroup and this function
 * can be used to test whether a cgroup is on the default hierarchy for
 * cases where a subsystem should behave differently depending on the
 * interface version.
 *
 * List of changed behaviors:
 *
 * - Mount options "noprefix", "xattr", "clone_children", "release_agent"
 *   and "name" are disallowed.
 *
 * - When mounting an existing superblock, mount options should match.
 *
 * - rename(2) is disallowed.
 *
 * - "tasks" is removed.  Everything should be at process granularity.  Use
 *   "cgroup.procs" instead.
 *
 * - "cgroup.procs" is not sorted.  pids will be unique unless they got
 *   recycled in-between reads.
 *
 * - "release_agent" and "notify_on_release" are removed.  Replacement
 *   notification mechanism will be implemented.
 *
 * - "cgroup.clone_children" is removed.
 *
 * - "cgroup.subtree_populated" is available.  Its value is 0 if the cgroup
 *   and its descendants contain no task; otherwise, 1.  The file also
 *   generates kernfs notification which can be monitored through poll and
 *   [di]notify when the value of the file changes.
 *
 * - cpuset: tasks will be kept in empty cpusets when hotplug happens and
 *   take masks of ancestors with non-empty cpus/mems, instead of being
 *   moved to an ancestor.
 *
 * - cpuset: a task can be moved into an empty cpuset, and again it takes
 *   masks of ancestors.
 *
 * - blkcg: blk-throttle becomes properly hierarchical.
 */
/*
 * 中文契约与上文行为表：
 * cgrp 是调用者以锁、RCU 或引用稳定的借用对象；函数仅比较其 root 与
 * 永久的 cgrp_dfl_root，不睡眠、无副作用。true 表示采用统一 v2 接口，
 * 因而上文列出的挂载限制、进程粒度、层级统计、cpuset 继承和 blkcg
 * 真层级语义均适用；false 表示 legacy/named v1 层级。
 */
bool cgroup_on_dfl(const struct cgroup *cgrp)
{
	return cgrp->root == &cgrp_dfl_root;
}

/* IDR wrappers which synchronize using cgroup_idr_lock */
/*
 * cgroup_idr_alloc() - 在 cgroup 私有锁协议下分配 ID 并发布 ptr。
 *
 * idr/ptr 是调用者借用对象，start/end 是 ID 半开范围，gfp_mask 描述可
 * 睡眠预装载策略。idr_preload() 在加自旋锁前完成可能 reclaim 的内存
 * 准备；锁内清除 __GFP_DIRECT_RECLAIM，禁止睡眠。成功返回非负 ID，
 * 失败返回 errno 且不发布 ptr；调用者仍拥有 ptr。
 */
static int cgroup_idr_alloc(struct idr *idr, void *ptr, int start, int end,
			    gfp_t gfp_mask)
{
	int ret;

	/* 阶段 1：在可睡眠上下文预备 IDR 节点，缩短 BH-disabled 临界区。 */
	idr_preload(gfp_mask);
	/* 阶段 2：串行化映射修改；锁内分配只能消费 preload 储备。 */
	spin_lock_bh(&cgroup_idr_lock);
	ret = idr_alloc(idr, ptr, start, end, gfp_mask & ~__GFP_DIRECT_RECLAIM);
	spin_unlock_bh(&cgroup_idr_lock);
	idr_preload_end();
	return ret;
}

/*
 * cgroup_idr_replace() - 原子替换既有 ID 对应的指针。
 *
 * idr/ptr 为借用，id 必须已存在。BH-safe 锁只保证映射修改互斥；
 * 调用者另行保证旧/新对象生命周期。返回旧指针或 IDR 错误指针，
 * 不改变引用数，可用于把占位对象切换为完全初始化对象。
 */
static void *cgroup_idr_replace(struct idr *idr, void *ptr, int id)
{
	void *ret;

	spin_lock_bh(&cgroup_idr_lock);
	ret = idr_replace(idr, ptr, id);
	spin_unlock_bh(&cgroup_idr_lock);
	return ret;
}

/*
 * cgroup_idr_remove() - 从 IDR 摘除 id，阻止后续按 ID 查找。
 *
 * 调用者借用 idr 并保证 id 的业务有效性；函数可在没有 cgroup_mutex
 * 的延迟释放路径调用。摘除不释放被映射对象，也不等待既有读者；
 * 生命周期必须由引用/RCU 协议另行收尾。返回：无。
 */
static void cgroup_idr_remove(struct idr *idr, int id)
{
	spin_lock_bh(&cgroup_idr_lock);
	idr_remove(idr, id);
	spin_unlock_bh(&cgroup_idr_lock);
}

/*
 * cgroup_is_threaded() - 判断节点是否借用祖先 domain 作为资源域。
 *
 * cgrp 为受 cgroup_mutex/RCU 稳定的借用对象；dom_cgrp==自身表示 domain，
 * 不等则为 threaded 节点。纯字段查询，不取引用、不睡眠。
 */
static bool cgroup_is_threaded(struct cgroup *cgrp)
{
	return cgrp->dom_cgrp != cgrp;
}

/* can @cgrp host both domain and threaded children? */
/*
 * 根节点不受 no-internal-process 约束，可同时容纳 domain 与 threaded
 * 子节点；普通节点不可混用。返回值只描述结构资格，不执行状态转换。
 */
static bool cgroup_is_mixable(struct cgroup *cgrp)
{
	/*
	 * Root isn't under domain level resource control exempting it from
	 * the no-internal-process constraint, so it can serve as a thread
	 * root and a parent of resource domains at the same time.
	 */
	/*
	 * 中文补充：没有 parent 即层级 root；它本身不代表受祖先控制的资源
	 * domain，因此内部 task 不会与向下分配 domain 资源产生同一冲突。
	 */
	return !cgroup_parent(cgrp);
}

/* can @cgrp become a thread root? Should always be true for a thread root */
/*
 * cgroup_can_be_thread_root() - 验证 cgrp 能否成为 threaded 子树入口。
 *
 * 调用者在 cgroup_mutex 下提供稳定 cgrp。根总是可混合；普通节点必须
 * 自身仍是 domain、没有 populated domain 子节点，且向下只启用了允许
 * threaded 的控制器。返回布尔资格，不修改任何字段、引用或拓扑。
 */
static bool cgroup_can_be_thread_root(struct cgroup *cgrp)
{
	/* mixables don't care */
	/* 层级根是上述约束的显式例外。 */
	if (cgroup_is_mixable(cgrp))
		return true;

	/* domain roots can't be nested under threaded */
	/* threaded 节点已经属于别的 domain，不能再充当新的 domain 根。 */
	if (cgroup_is_threaded(cgrp))
		return false;

	/* can only have either domain or threaded children */
	/* 已有活跃 domain 后代时切换会把同一子树解释成两种资源模型。 */
	if (READ_ONCE(cgrp->nr_populated_domain_children))
		return false;

	/* and no domain controllers can be enabled */
	/* 非 threaded 控制器要求完整 domain 语义，因此与 thread root 冲突。 */
	if (cgrp->subtree_control & ~cgrp_dfl_threaded_ss_mask)
		return false;

	return true;
}

/* is @cgrp root of a threaded subtree? */
/*
 * cgroup_is_thread_root() - 根据子节点、task 和控制器状态识别 thread root。
 *
 * cgrp 为稳定借用对象；函数不加锁，调用者负责 cgroup_mutex 或可接受
 * READ_ONCE 计数快照的上下文。threaded 节点本身不是 root；domain 只要
 * 已有 threaded child，或同时有 task 且显式下放 threaded 控制器，即
 * 成为 thread root。返回只读判断，无引用变化。
 */
static bool cgroup_is_thread_root(struct cgroup *cgrp)
{
	/* thread root should be a domain */
	/* thread root 自身必须是 domain；threaded 节点只能作为其成员。 */
	if (cgroup_is_threaded(cgrp))
		return false;

	/* a domain w/ threaded children is a thread root */
	/* domain 一旦拥有 threaded child，就成为该 threaded subtree 的根。 */
	if (cgrp->nr_threaded_children)
		return true;

	/*
	 * A domain which has tasks and explicit threaded controllers
	 * enabled is a thread root.
	 */
	/*
	 * domain 同时容纳 task 并显式下放 threaded controller 时，也必须按
	 * thread root 约束；这样 task 与子线程域共享资源域的边界是明确的。
	 */
	if (cgroup_has_tasks(cgrp) &&
	    (cgrp->subtree_control & cgrp_dfl_threaded_ss_mask))
		return true;

	return false;
}

/* a domain which isn't connected to the root w/o brekage can't be used */
/*
 * cgroup_is_valid_domain() - 验证 cgrp 到层级根的路径保持连续 domain。
 *
 * 调用者稳定整个祖先链，通常持 cgroup_mutex。自身不能是 threaded；
 * 祖先若是普通 thread root 或 threaded 节点，就会切断 domain 资源传播。
 * mixable 层级根可例外。true 仅表示拓扑有效，不保证控制器 online。
 */
static bool cgroup_is_valid_domain(struct cgroup *cgrp)
{
	/* the cgroup itself can be a thread root */
	/* 待验证节点可作为 thread root，但不能已经是 threaded 成员。 */
	if (cgroup_is_threaded(cgrp))
		return false;

	/* but the ancestors can't be unless mixable */
	/*
	 * 除 mixable 例外外，祖先若已是 thread root，domain 传播链就在此
	 * 断开；任何 threaded 祖先同样使当前节点不能成为有效 domain。
	 */
	while ((cgrp = cgroup_parent(cgrp))) {
		if (!cgroup_is_mixable(cgrp) && cgroup_is_thread_root(cgrp))
			return false;
		if (cgroup_is_threaded(cgrp))
			return false;
	}

	return true;
}

/* subsystems visibly enabled on a cgroup */
/*
 * cgroup_control() - 计算 cgrp 对用户可见、可控制的控制器位图。
 *
 * 普通节点继承 parent->subtree_control，threaded 节点再限制为 threaded
 * 控制器；root 使用层级绑定 mask，v2 root 还隐藏 inhibit 与 implicit
 * 位。返回瞬时 u32，不取得 css 引用；调用者负责拓扑同步。
 */
static u32 cgroup_control(struct cgroup *cgrp)
{
	struct cgroup *parent = cgroup_parent(cgrp);
	u32 root_ss_mask = cgrp->root->subsys_mask;

	if (parent) {
		u32 ss_mask = parent->subtree_control;

		/* threaded cgroups can only have threaded controllers */
		/* threaded 节点只能看到声明支持线程粒度的 controller。 */
		if (cgroup_is_threaded(cgrp))
			ss_mask &= cgrp_dfl_threaded_ss_mask;
		return ss_mask;
	}

	if (cgroup_on_dfl(cgrp))
		root_ss_mask &= ~(cgrp_dfl_inhibit_ss_mask |
				  cgrp_dfl_implicit_ss_mask);
	return root_ss_mask;
}

/* subsystems enabled on a cgroup */
/*
 * cgroup_ss_mask() - 计算 cgrp 实际应拥有 css 的控制器位图。
 *
 * 与 cgroup_control() 的“用户可见”不同，本函数使用父节点已经物化的
 * subtree_ss_mask，根直接使用 root->subsys_mask，因此包括必要的隐式
 * 状态。threaded 节点仍只能保留 threaded 控制器。
 */
static u32 cgroup_ss_mask(struct cgroup *cgrp)
{
	struct cgroup *parent = cgroup_parent(cgrp);

	if (parent) {
		u32 ss_mask = parent->subtree_ss_mask;

		/* threaded cgroups can only have threaded controllers */
		/*
		 * 物化 css 时也应用同一限制，避免可见 mask 与实际状态
		 * 不一致。
		 */
		if (cgroup_is_threaded(cgrp))
			ss_mask &= cgrp_dfl_threaded_ss_mask;
		return ss_mask;
	}

	return cgrp->root->subsys_mask;
}

/**
 * cgroup_e_css_by_mask - obtain a cgroup's effective css for the specified ss
 * @cgrp: the cgroup of interest
 * @ss: the subsystem of interest (%NULL returns @cgrp->self)
 *
 * Similar to cgroup_css() but returns the effective css, which is defined
 * as the matching css of the nearest ancestor including self which has @ss
 * enabled.  If @ss is associated with the hierarchy @cgrp is on, this
 * function is guaranteed to return non-NULL css.
 */
/*
 * 中文契约：
 * 调用者必须持 cgroup_mutex；cgrp/ss 均为借用，ss==NULL 特指 core self
 * css。函数用于控制器关联正在变化的窗口，不能直接依据 subsys[] 是否
 * 非空，而按计算出的 ss_mask 向祖先寻找有效层级。返回裸借用 css，
 * 不增加引用；控制器未绑定到该层级时可返回 NULL。
 */
static struct cgroup_subsys_state *cgroup_e_css_by_mask(struct cgroup *cgrp,
							struct cgroup_subsys *ss)
{
	lockdep_assert_held(&cgroup_mutex);

	if (!ss)
		return &cgrp->self;

	/*
	 * This function is used while updating css associations and thus
	 * can't test the csses directly.  Test ss_mask.
	 */
	/*
	 * rebind/apply-control 期间 subsys[] 可能正处于构造或摘除过渡态，
	 * mask 才是本次事务决定的目标状态。
	 */
	while (!(cgroup_ss_mask(cgrp) & (1 << ss->id))) {
		cgrp = cgroup_parent(cgrp);
		if (!cgrp)
			return NULL;
	}

	return cgroup_css(cgrp, ss);
}

/**
 * cgroup_e_css - obtain a cgroup's effective css for the specified subsystem
 * @cgrp: the cgroup of interest
 * @ss: the subsystem of interest
 *
 * Find and get the effective css of @cgrp for @ss.  The effective css is
 * defined as the matching css of the nearest ancestor including self which
 * has @ss enabled.  If @ss is not mounted on the hierarchy @cgrp is on,
 * the root css is returned, so this function always returns a valid css.
 *
 * The returned css is not guaranteed to be online, and therefore it is the
 * callers responsibility to try get a reference for it.
 */
/*
 * 中文契约：
 * cgrp/ss 是由调用者在 cgroup_mutex、RCU 或其他生命周期保护下提供的
 * 借用对象。本函数沿祖先读取 subsys[]，找不到则退到永久
 * init_css_set 的 root css，所以启用控制器配置时总返回非 NULL 裸指针；
 * 未编译控制器则返回 NULL。
 *
 * 这里不增加引用，也不保证结果 online。调用者若要越过现有保护窗口，
 * 必须自行 css_tryget_online()/css_get()；返回值只保证查询时的关联。
 */
struct cgroup_subsys_state *cgroup_e_css(struct cgroup *cgrp,
					 struct cgroup_subsys *ss)
{
	struct cgroup_subsys_state *css;

	if (!CGROUP_HAS_SUBSYS_CONFIG)
		return NULL;

	do {
		css = cgroup_css(cgrp, ss);

		if (css)
			return css;
		cgrp = cgroup_parent(cgrp);
	} while (cgrp);

	return init_css_set.subsys[ss->id];
}

/**
 * cgroup_get_e_css - get a cgroup's effective css for the specified subsystem
 * @cgrp: the cgroup of interest
 * @ss: the subsystem of interest
 *
 * Find and get the effective css of @cgrp for @ss.  The effective css is
 * defined as the matching css of the nearest ancestor including self which
 * has @ss enabled.  If @ss is not mounted on the hierarchy @cgrp is on,
 * the root css is returned, so this function always returns a valid css.
 * The returned css must be put using css_put().
 */
/*
 * 中文契约：
 * 输入是借用的 cgrp/ss；函数在内部 RCU 读侧沿祖先查找，并只接受
 * css_tryget_online() 成功的状态。若某层 css 正在 dying，就继续向上，
 * 最终对永久 root css 取得普通引用。成功返回持有引用，调用者必须
 * css_put()；无 subsystem 配置返回 NULL。
 *
 * RCU 只保证遍历窗口内对象存储期，真正允许出锁后使用的是取得的 css
 * 引用。引用保证不释放，不冻结控制器字段；字段同步仍遵循 subsystem
 * 自己的锁规则。
 */
struct cgroup_subsys_state *cgroup_get_e_css(struct cgroup *cgrp,
					     struct cgroup_subsys *ss)
{
	struct cgroup_subsys_state *css;

	if (!CGROUP_HAS_SUBSYS_CONFIG)
		return NULL;

	/* 阶段 1：RCU 稳定祖先和 subsys 指针，尝试领取一个 online 引用。 */
	rcu_read_lock();

	do {
		css = cgroup_css(cgrp, ss);

		if (css && css_tryget_online(css))
			goto out_unlock;
		cgrp = cgroup_parent(cgrp);
	} while (cgrp);

	/*
	 * 阶段 2：层级未挂载该控制器或所有候选正离线，退到永久 root css。
	 * root css 不会走 online 获取失败路径，普通 css_get 足以持有。
	 */
	css = init_css_set.subsys[ss->id];
	css_get(css);
out_unlock:
	rcu_read_unlock();
	return css;
}
EXPORT_SYMBOL_GPL(cgroup_get_e_css);

/*
 * cgroup_get_live() - 对确认尚未死亡的 cgroup 增加引用。
 *
 * cgrp 是调用者在阻止并发销毁的锁/RCU 窗口内借用的对象。dead 检查
 * 失败只告警，不改变控制流；随后 cgroup_get() 取得必须由 cgroup_put()
 * 配对的持有引用。函数不睡眠，返回：无直接返回值。
 */
static void cgroup_get_live(struct cgroup *cgrp)
{
	WARN_ON_ONCE(cgroup_is_dead(cgrp));
	cgroup_get(cgrp);
}

/**
 * __cgroup_task_count - count the number of tasks in a cgroup. The caller
 * is responsible for taking the css_set_lock.
 * @cgrp: the cgroup in question
 */
/*
 * 中文契约：
 * 调用者必须持 css_set_lock；cgrp 是借用对象。函数遍历该 cgroup 与
 * 各 css_set 的链接，把每个共享 css_set 的 nr_tasks 相加。返回瞬时
 * task 数量，不取 task 引用；锁阻止迁移/exit 同时改链和计数。
 */
int __cgroup_task_count(const struct cgroup *cgrp)
{
	int count = 0;
	struct cgrp_cset_link *link;

	lockdep_assert_held(&css_set_lock);

	list_for_each_entry(link, &cgrp->cset_links, cset_link)
		count += link->cset->nr_tasks;

	return count;
}

/**
 * cgroup_task_count - count the number of tasks in a cgroup.
 * @cgrp: the cgroup in question
 */
/*
 * 中文契约：
 * cgrp 在调用期间必须存活；本包装器自行以 irq-safe css_set_lock 获取
 * 一致计数，可从允许短暂关中断的上下文调用且不睡眠。返回调用瞬间的
 * task 数，解锁后可立即变化，不代表后续 populated 状态承诺。
 */
int cgroup_task_count(const struct cgroup *cgrp)
{
	int count;

	spin_lock_irq(&css_set_lock);
	count = __cgroup_task_count(cgrp);
	spin_unlock_irq(&css_set_lock);

	return count;
}

/*
 * kn_priv() - 从 cgroup 控制文件 kernfs 节点取得其父目录 cgroup。
 *
 * kn 是 active 的文件节点借用指针。cgroup 文件自身 priv 通常是 cftype，
 * 父目录节点 priv 才是 cgroup。KERNFS_ROOT_INVARIANT_PARENT 保证 parent
 * 指针不会被替换，所以 rcu_dereference_check 可在 RCU 外使用。返回
 * 裸借用 cgroup，其有效期由 kernfs active reference 保证。
 */
static struct cgroup *kn_priv(struct kernfs_node *kn)
{
	struct kernfs_node *parent;
	/*
	 * The parent can not be replaced due to KERNFS_ROOT_INVARIANT_PARENT.
	 * Therefore it is always safe to dereference this pointer outside of a
	 * RCU section.
	 */
	/*
	 * 中文补充：该 flag 只保证父指针关系不变；父节点及 priv 的存活
	 * 来自当前 kernfs 操作持有的 active reference，而不是本次解引用。
	 */
	parent = rcu_dereference_check(kn->__parent,
				       kernfs_root_flags(kn) & KERNFS_ROOT_INVARIANT_PARENT);
	return parent->priv;
}

/*
 * of_css() - 把打开的 cgroup 文件解析为本次操作对应的 css。
 *
 * of 持有 kernfs active reference；函数借用其 kn/cft，不增加任何引用。
 * subsystem 文件从 cgrp->subsys[ssid] 返回对应 css，core 文件返回 self。
 * 文件摘除会先 drain active 操作，再断开 css，因此 raw RCU 解引用在这个
 * 特定协议下安全。返回 css 仅供当前文件回调使用，不能越过操作保存。
 */
struct cgroup_subsys_state *of_css(struct kernfs_open_file *of)
{
	struct cgroup *cgrp = kn_priv(of->kn);
	struct cftype *cft = of_cft(of);

	/*
	 * This is open and unprotected implementation of cgroup_css().
	 * seq_css() is only called from a kernfs file operation which has
	 * an active reference on the file.  Because all the subsystem
	 * files are drained before a css is disassociated with a cgroup,
	 * the matching css from the cgroup's subsys table is guaranteed to
	 * be and stay valid until the enclosing operation is complete.
	 */
	/*
	 * 中文补充：这里刻意不是通用无锁读取范式；安全性依赖“先 drain
	 * subsystem files，再从 cgroup 摘除 css”的销毁顺序。
	 */
	if (CGROUP_HAS_SUBSYS_CONFIG && cft->ss)
		return rcu_dereference_raw(cgrp->subsys[cft->ss->id]);
	else
		return &cgrp->self;
}
EXPORT_SYMBOL_GPL(of_css);

/**
 * for_each_css - iterate all css's of a cgroup
 * @css: the iteration cursor
 * @ssid: the index of the subsystem, CGROUP_SUBSYS_COUNT after reaching the end
 * @cgrp: the target cgroup to iterate css's of
 *
 * Should be called under cgroup_mutex.
 */
/*
 * 中文宏语义：按 ssid 遍历 cgrp->subsys[]，在 cgroup_mutex 的 lockdep
 * 条件下 RCU 解引用；NULL 项跳过，循环体只看到真实 css。css 是借用
 * 指针，宏不增加引用，ssid 结束时等于 CGROUP_SUBSYS_COUNT。
 */
#define for_each_css(css, ssid, cgrp)					\
	for ((ssid) = 0; (ssid) < CGROUP_SUBSYS_COUNT; (ssid)++)	\
		if (!((css) = rcu_dereference_check(			\
				(cgrp)->subsys[(ssid)],			\
				lockdep_is_held(&cgroup_mutex)))) { }	\
		else

/**
 * do_each_subsys_mask - filter for_each_subsys with a bitmask
 * @ss: the iteration cursor
 * @ssid: the index of @ss, CGROUP_SUBSYS_COUNT after reaching the end
 * @ss_mask: the bitmask
 *
 * The block will only run for cases where the ssid-th bit (1 << ssid) of
 * @ss_mask is set.
 */
/*
 * 中文宏语义：先把 ss_mask 求值一次到局部 unsigned long，再只遍历置位
 * ssid，并令 ss 指向全局 subsystem 描述。do/while_each 必须成对使用；
 * 宏只做筛选，不取得 subsystem/css 引用。无控制器配置时循环体不执行。
 */
#define do_each_subsys_mask(ss, ssid, ss_mask) do {			\
	unsigned long __ss_mask = (ss_mask);				\
	if (!CGROUP_HAS_SUBSYS_CONFIG) {				\
		(ssid) = 0;						\
		break;							\
	}								\
	for_each_set_bit(ssid, &__ss_mask, CGROUP_SUBSYS_COUNT) {	\
		(ss) = cgroup_subsys[ssid];				\
		{

#define while_each_subsys_mask()					\
		}							\
	}								\
} while (false)

/*
 * The default css_set - used by init and its children prior to any
 * hierarchies being mounted. It contains a pointer to the root state
 * for each subsystem. Also used to anchor the list of css_sets. Not
 * reference-counted, to improve performance when child cgroups
 * haven't been created.
 */
/*
 * 中文生命周期：
 * init_css_set 是启动前即可使用的永久组合，init task 和早期 child 的
 * task->cgroups 指向它。它既是 css_set 全局链表锚点，也为每个控制器
 * 保存 root css；不按普通零引用路径销毁，refcount 初值主要维持统一接口。
 * 所有嵌入链表静态初始化，确保 cgroup_init() 前的 fork/查询也安全。
 */
struct css_set init_css_set = {
	.refcount		= REFCOUNT_INIT(1),
	.dom_cset		= &init_css_set,
	.tasks			= LIST_HEAD_INIT(init_css_set.tasks),
	.mg_tasks		= LIST_HEAD_INIT(init_css_set.mg_tasks),
	.dying_tasks		= LIST_HEAD_INIT(init_css_set.dying_tasks),
	.task_iters		= LIST_HEAD_INIT(init_css_set.task_iters),
	.threaded_csets		= LIST_HEAD_INIT(init_css_set.threaded_csets),
	.cgrp_links		= LIST_HEAD_INIT(init_css_set.cgrp_links),
	.mg_src_preload_node	= LIST_HEAD_INIT(init_css_set.mg_src_preload_node),
	.mg_dst_preload_node	= LIST_HEAD_INIT(init_css_set.mg_dst_preload_node),
	.mg_node		= LIST_HEAD_INIT(init_css_set.mg_node),

	/*
	 * The following field is re-initialized when this cset gets linked
	 * in cgroup_init().  However, let's initialize the field
	 * statically too so that the default cgroup can be accessed safely
	 * early during boot.
	 */
	/*
	 * 中文补充：cgroup_init() 会在正式建立链接时重写 dfl_cgrp，但静态
	 * 指向默认根可保证更早的 task 查询不解引用未初始化指针。
	 */
	.dfl_cgrp		= &cgrp_dfl_root.cgrp,
};

/*
 * css_set 总数调试/统计值；初值 1 只计永久 init_css_set，
 * 受 css_set_lock 保护。
 */
static int css_set_count	= 1;	/* 1 for init_css_set */

/*
 * css_set_threaded() - 判断成员组合是否把 domain 归属委托给另一 cset。
 *
 * cset 为锁/引用稳定的借用对象；dom_cset != 自身即 threaded 组合。
 * 纯查询，不取引用、不睡眠。
 */
static bool css_set_threaded(struct css_set *cset)
{
	return cset->dom_cset != cset;
}

/**
 * css_set_populated - does a css_set contain any tasks?
 * @cset: target css_set
 *
 * css_set_populated() should be the same as !!cset->nr_tasks at steady
 * state. However, css_set_populated() can be called while a task is being
 * added to or removed from the linked list before the nr_tasks is
 * properly updated. Hence, we can't just look at ->nr_tasks here.
 */
/*
 * 中文契约：
 * 调用者必须持 css_set_lock。迁移会先在 tasks 与 mg_tasks 之间移动
 * 链表节点，再更新 nr_tasks，所以过渡窗口中计数可能暂时不等于真实
 * 非空状态。本函数以两条实际成员链判断“至少一个 task”，不取 task
 * 引用、无副作用。
 */
static bool css_set_populated(struct css_set *cset)
{
	lockdep_assert_held(&css_set_lock);

	return !list_empty(&cset->tasks) || !list_empty(&cset->mg_tasks);
}

/**
 * css_update_populated - update the populated state of a css and ancestors
 * @css: leaf css whose own populated count is changing
 * @populated: inc or dec
 *
 * One of the css_sets pinned by @css is getting its first task or losing the
 * last. Propagate the transition up the parent chain so that a css's
 * nr_populated_children is zero iff none of its descendants contain any tasks.
 *
 * For a cgroup->self walk, also runs cgroup-side bookkeeping at each level:
 * domain/threaded child split, deferred-destroy trigger, and notification via
 * "cgroup.populated" (zero iff cgrp->self has neither populated csets nor
 * populated children; userland is notified on transitions).
 */
/*
 * 中文契约与并发：
 * css 是其某个 css_set 首次变为非空或最后变为空的叶状态，populated
 * 指示 +1/-1；调用者持 css_set_lock，因此成员关系和所有 populated
 * 计数在整条祖先传播期间一致。函数不取输入 ownership、不能睡眠。
 *
 * 只有 css_is_populated() 的布尔值真正翻转时才继续向父传播，避免多个
 * populated cset 重复计入祖先。self css 还维护 domain/threaded 子计数、
 * v1 release 检查及 cgroup.events 通知。若 dying css 的子树刚清空，
 * 额外持有 css 引用并把 kill_finish_work 交给 offline 队列；worker
 * 最终负责归还该引用。
 */
static void css_update_populated(struct cgroup_subsys_state *css, bool populated)
{
	/*
	 * child 标识本轮变化来自哪个直接子 css；NULL 表示叶自身 cset 计数。
	 * adj 是计数增量；cgrp 只在 core self 链上存在，was_populated 保存
	 * 更新前布尔状态，用于判断是否需要继续传播。
	 */
	struct cgroup_subsys_state *child = NULL;
	int adj = populated ? 1 : -1;

	lockdep_assert_held(&css_set_lock);

	do {
		/* non-NULL only on the cgroup->self walk */
		/*
		 * 只有沿 core self css 向上时才能恢复 struct cgroup；controller
		 * css 没有对应的 domain/threaded cgroup 计数需要同步。
		 */
		struct cgroup *cgrp = css_is_self(css) ? css->cgroup : NULL;
		bool was_populated = css_is_populated(css);

		/* 阶段 1：更新叶 cset 数或父级 populated-child 数。 */
		if (!child) {
			WRITE_ONCE(css->nr_populated_csets,
				   css->nr_populated_csets + adj);
		} else {
			WRITE_ONCE(css->nr_populated_children,
				   css->nr_populated_children + adj);
			if (cgrp) {
				if (cgroup_is_threaded(child->cgroup))
					WRITE_ONCE(cgrp->nr_populated_threaded_children,
						   cgrp->nr_populated_threaded_children + adj);
				else
					WRITE_ONCE(cgrp->nr_populated_domain_children,
						   cgrp->nr_populated_domain_children + adj);
			}
		}

		/*
		 * 计数虽变化但“是否 populated”未翻转时，祖先看到的
		 * 布尔贡献不变，传播可以在此停止。
		 */
		if (was_populated == css_is_populated(css))
			break;

		/*
		 * Pair with smp_mb() in kill_css_sync(). Either we observe
		 * CSS_DYING and queue, or the caller observes our decrement
		 * and fires synchronously.
		 */
		/*
		 * 中文内存序：与 kill_css_sync() 的 smp_mb() 配对，关闭
		 * “销毁方没看到最后一次减计数、减计数方没看到 CSS_DYING”
		 * 的双向漏检窗口。至少一方会负责触发 kill_finish。
		 */
		smp_mb();

		/*
		 * Subtree just emptied below a dying css. Fire deferred kill.
		 * The transition is one-shot for a dying css.
		 */
		/*
		 * 空转移对 dying css 只发生一次。css_get 把对象生命延长到
		 * worker 执行；queue_work 失败意味着重复排队，属于不变量破坏。
		 */
		if (was_populated && css_is_dying(css)) {
			css_get(css);
			WARN_ON_ONCE(!queue_work(cgroup_offline_wq, &css->kill_finish_work));
		}

		/*
		 * core self 链还向 v1 release_agent、trace 和 kernfs poll
		 * 消费者发布状态变化；控制器私有 css 链只维护计数与销毁。
		 */
		if (cgrp) {
			cgroup1_check_for_release(cgrp);
			TRACE_CGROUP_PATH(notify_populated, cgrp,
					  cgroup_is_populated(cgrp));
			cgroup_file_notify(&cgrp->events_file);
		}

		child = css;
		css = css->parent;
	} while (css);
}

/**
 * css_set_update_populated - update populated state of a css_set
 * @cset: target css_set
 * @populated: whether @cset is populated or depopulated
 *
 * @cset is either getting the first task or losing the last. Update the
 * populated counters along each linked cgroup's self chain and each
 * subsystem css that @cset pins.
 */
/*
 * 中文契约：
 * cset 是正在发生首 task/末 task 转换的借用对象，调用者持
 * css_set_lock；populated 选择增加或减少。函数先更新它链接到的每棵
 * cgroup 层级 self 链，再更新各控制器 css 链。返回：无；通知和可能的
 * 延迟销毁排队是可观察副作用。
 */
static void css_set_update_populated(struct css_set *cset, bool populated)
{
	struct cgrp_cset_link *link;
	struct cgroup_subsys *ss;
	int ssid;

	lockdep_assert_held(&css_set_lock);

	list_for_each_entry(link, &cset->cgrp_links, cgrp_link)
		css_update_populated(&link->cgrp->self, populated);

	for_each_subsys(ss, ssid) {
		struct cgroup_subsys_state *css = cset->subsys[ssid];

		if (css)
			css_update_populated(css, populated);
	}
}

/*
 * @task is leaving, advance task iterators which are pointing to it so
 * that they can resume at the next position.  Advancing an iterator might
 * remove it from the list, use safe walk.  See css_task_iter_skip() for
 * details.
 */
/*
 * 中文契约：
 * task 正从 cset 成员链离开，调用者持 css_set_lock；二者均为借用对象。
 * 所有当前指向 task 的 css_task_iter 必须先推进，否则节点摘链后迭代器
 * 会保存失效位置。推进本身可能把 iterator 从链上移除，故使用 safe
 * 遍历缓存下一个节点。返回：无。
 */
static void css_set_skip_task_iters(struct css_set *cset,
				    struct task_struct *task)
{
	struct css_task_iter *it, *pos;

	list_for_each_entry_safe(it, pos, &cset->task_iters, iters_node)
		css_task_iter_skip(it, task);
}

/**
 * css_set_move_task - move a task from one css_set to another
 * @task: task being moved
 * @from_cset: css_set @task currently belongs to (may be NULL)
 * @to_cset: new css_set @task is being moved to (may be NULL)
 * @use_mg_tasks: move to @to_cset->mg_tasks instead of ->tasks
 *
 * Move @task from @from_cset to @to_cset.  If @task didn't belong to any
 * css_set, @from_cset can be NULL.  If @task is being disassociated
 * instead of moved, @to_cset can be NULL.
 *
 * This function automatically handles populated counter updates and
 * css_task_iter adjustments but the caller is responsible for managing
 * @from_cset and @to_cset's reference counts.
 */
/*
 * 中文契约：
 * 调用者持 css_set_lock，并另行管理 from/to cset 引用。task 是待迁移
 * 对象；from 可空表示尚未关联，to 可空表示仅摘除；use_mg_tasks=true
 * 把 task 放入迁移暂存链，事务提交后才转回正式 tasks 链。
 *
 * 函数负责在摘链前推进 iterator、维护首/末 task 引起的 populated
 * 传播，并用 RCU 发布 task->cgroups 新指针。无错误返回、不可睡眠；
 * 成功后成员链与 nr_tasks/计数保持一致，但引用责任仍在调用者。
 */
static void css_set_move_task(struct task_struct *task,
			      struct css_set *from_cset, struct css_set *to_cset,
			      bool use_mg_tasks)
{
	lockdep_assert_held(&css_set_lock);

	if (to_cset && !css_set_populated(to_cset))
		css_set_update_populated(to_cset, true);

	if (from_cset) {
		WARN_ON_ONCE(list_empty(&task->cg_list));

		css_set_skip_task_iters(from_cset, task);
		list_del_init(&task->cg_list);
		if (!css_set_populated(from_cset))
			css_set_update_populated(from_cset, false);
	} else {
		WARN_ON_ONCE(!list_empty(&task->cg_list));
	}

	if (to_cset) {
		/*
		 * We are synchronized through cgroup_threadgroup_rwsem
		 * against PF_EXITING setting such that we can't race
		 * against cgroup_task_dead()/cgroup_task_free() dropping
		 * the css_set.
		 */
		/*
		 * 中文补充：threadgroup rwsem 让迁移与 task 进入 PF_EXITING 的
		 * 路径互斥；否则刚发布 to_cset 后，退出路径可能重复 put
		 * 或漏链。
		 */
		WARN_ON_ONCE(task->flags & PF_EXITING);

		cgroup_move_task(task, to_cset);
		list_add_tail(&task->cg_list, use_mg_tasks ? &to_cset->mg_tasks :
							     &to_cset->tasks);
	}
}

/*
 * hash table for cgroup groups. This improves the performance to find
 * an existing css_set. This hash doesn't (currently) take into
 * account cgroups in empty hierarchies.
 */
/*
 * css_set_table 按控制器 css 指针组合散列，减少迁移时复用组合的查找成本。
 * 空层级不进入 key，因此命中后仍必须比较各 hierarchy 的 cgroup 链接。
 * 表、hlist 和 css_set_count 都由 css_set_lock 保护。
 */
#define CSS_SET_HASH_BITS	7
static DEFINE_HASHTABLE(css_set_table, CSS_SET_HASH_BITS);

/*
 * css_set_hash() - 为按 ssid 排列的 css 指针数组生成候选桶 key。
 *
 * css 是包含 CGROUP_SUBSYS_COUNT 项的借用数组；函数只读指针值并混合高低
 * 位，不取 css 引用、不睡眠。不同组合允许碰撞，compare_css_sets() 负责
 * 完整判等，因此该和式只影响性能而不影响正确性。
 */
static unsigned long css_set_hash(struct cgroup_subsys_state **css)
{
	unsigned long key = 0UL;
	struct cgroup_subsys *ss;
	int i;

	for_each_subsys(ss, i)
		key += (unsigned long)css[i];
	key = (key >> 16) ^ key;

	return key;
}

/*
 * put_css_set_locked() - 在 css_set_lock 下归还引用并执行最后释放。
 *
 * cset 是调用者持有的引用；函数消费一份引用。非最后引用时立即返回；
 * 最后引用意味着无 task/外部持有者可再使用它，于是从各 css 的反向链、
 * 哈希表和每棵 root 的链接中摘除，逐一 css_put()/cgroup_put()，最后
 * kfree_rcu()，让此前 RCU 读者越过宽限期。
 *
 * threaded cset 还持有 dom_cset 引用，必须摘链并递归归还。函数不睡眠；
 * 返回后原 cset 指针不得使用。
 */
void put_css_set_locked(struct css_set *cset)
{
	struct cgrp_cset_link *link, *tmp_link;
	struct cgroup_subsys *ss;
	int ssid;

	lockdep_assert_held(&css_set_lock);

	/* 快速路径：仍有持有者，结构继续保持全局可发现。 */
	if (!refcount_dec_and_test(&cset->refcount))
		return;

	WARN_ON_ONCE(!list_empty(&cset->threaded_csets));

	/* This css_set is dead. Unlink it and release cgroup and css refs */
	/*
	 * 最后引用是不可回滚边界。先从所有索引摘除，阻止新查找命中，
	 * 再归还构造时取得的 css/cgroup 引用。
	 */
	for_each_subsys(ss, ssid) {
		list_del(&cset->e_cset_node[ssid]);
		css_put(cset->subsys[ssid]);
	}
	hash_del(&cset->hlist);
	css_set_count--;

	list_for_each_entry_safe(link, tmp_link, &cset->cgrp_links, cgrp_link) {
		list_del(&link->cset_link);
		list_del(&link->cgrp_link);
		if (cgroup_parent(link->cgrp))
			cgroup_put(link->cgrp);
		kfree(link);
	}

	/* threaded 组合额外持有 domain 组合；逆序拆除该 ownership。 */
	if (css_set_threaded(cset)) {
		list_del(&cset->threaded_csets_node);
		put_css_set_locked(cset->dom_cset);
	}

	/* task->cgroups 的旧 RCU 读者可能仍持裸指针，故不能立即 kfree。 */
	kfree_rcu(cset, rcu_head);
}

/**
 * compare_css_sets - helper function for find_existing_css_set().
 * @cset: candidate css_set being tested
 * @old_cset: existing css_set for a task
 * @new_cgrp: cgroup that's being entered by the task
 * @template: desired set of css pointers in css_set (pre-calculated)
 *
 * Returns true if "cset" matches "old_cset" except for the hierarchy
 * which "new_cgrp" belongs to, for which it should match "new_cgrp".
 */
/*
 * 中文契约：
 * cset 是哈希候选；old_cset 是迁移前组合；new_cgrp 是本次替换的层级节点；
 * template 是预计算目标 css 指针数组。四者均为 css_set_lock/cgroup_mutex
 * 保护下的借用对象。函数依次比较 css 组合、v2 domain 归属和按 root
 * 排序的 cgroup 链接；全部一致才返回 true，无引用或状态副作用。
 */
static bool compare_css_sets(struct css_set *cset,
			     struct css_set *old_cset,
			     struct cgroup *new_cgrp,
			     struct cgroup_subsys_state *template[])
{
	struct cgroup *new_dfl_cgrp;
	struct list_head *l1, *l2;

	/*
	 * On the default hierarchy, there can be csets which are
	 * associated with the same set of cgroups but different csses.
	 * Let's first ensure that csses match.
	 */
	/*
	 * 有效 css 不同即资源状态不同，即使 cgroup 链接相同也不能复用。
	 */
	if (memcmp(template, cset->subsys, sizeof(cset->subsys)))
		return false;


	/* @cset's domain should match the default cgroup's */
	/*
	 * 候选 cset 的 dom_cset 必须与迁移后 default cgroup 的 dom_cgrp
	 * 一致，否则线程域虽有相同 controller css，资源域归属却不同。
	 */
	if (cgroup_on_dfl(new_cgrp))
		new_dfl_cgrp = new_cgrp;
	else
		new_dfl_cgrp = old_cset->dfl_cgrp;

	if (new_dfl_cgrp->dom_cgrp != cset->dom_cset->dfl_cgrp)
		return false;

	/*
	 * Compare cgroup pointers in order to distinguish between
	 * different cgroups in hierarchies.  As different cgroups may
	 * share the same effective css, this comparison is always
	 * necessary.
	 */
	/*
	 * 多个 cgroup 可能因控制器未启用而共享同一 effective css，故仅比较
	 * template 会错误合并成员关系；必须逐层级比较实际目录节点。
	 */
	l1 = &cset->cgrp_links;
	l2 = &old_cset->cgrp_links;
	while (1) {
		struct cgrp_cset_link *link1, *link2;
		struct cgroup *cgrp1, *cgrp2;

		l1 = l1->next;
		l2 = l2->next;
		/* See if we reached the end - both lists are equal length. */
		/*
		 * 两个 cset 都覆盖全部 root；一方结束时另一方也必须同步
		 * 结束。
		 */
		if (l1 == &cset->cgrp_links) {
			BUG_ON(l2 != &old_cset->cgrp_links);
			break;
		} else {
			BUG_ON(l2 == &old_cset->cgrp_links);
		}
		/* Locate the cgroups associated with these links. */
		/* 从侵入式 link 节点恢复各自实际关联的 cgroup。 */
		link1 = list_entry(l1, struct cgrp_cset_link, cgrp_link);
		link2 = list_entry(l2, struct cgrp_cset_link, cgrp_link);
		cgrp1 = link1->cgrp;
		cgrp2 = link2->cgrp;
		/* Hierarchies should be linked in the same order. */
		/*
		 * root 全局链接顺序一致，因此同一位置必须属于同一
		 * hierarchy。
		 */
		BUG_ON(cgrp1->root != cgrp2->root);

		/*
		 * If this hierarchy is the hierarchy of the cgroup
		 * that's changing, then we need to check that this
		 * css_set points to the new cgroup; if it's any other
		 * hierarchy, then this css_set should point to the
		 * same cgroup as the old css_set.
		 */
		/*
		 * 只允许 new_cgrp 所属 root 的节点改变；其他 root 必须保持与
		 * old_cset 完全相同；这正是“一次迁移只改变一棵层级”
		 * 的不变量。
		 */
		if (cgrp1->root == new_cgrp->root) {
			if (cgrp1 != new_cgrp)
				return false;
		} else {
			if (cgrp1 != cgrp2)
				return false;
		}
	}
	return true;
}

/**
 * find_existing_css_set - init css array and find the matching css_set
 * @old_cset: the css_set that we're using before the cgroup transition
 * @cgrp: the cgroup that we're moving into
 * @template: out param for the new set of csses, should be clear on entry
 */
/*
 * 中文契约：
 * 调用者同时稳定 cgroup 拓扑和 css_set_table（实际由 find_css_set()
 * 在 cgroup_mutex + css_set_lock 下调用）。old_cset/cgrp 是借用输入；
 * template 是已清零的输出数组，函数填成迁移后的 effective css 组合。
 *
 * 返回匹配 css_set 的裸指针或 NULL，不增加引用；调用者仍在锁内时必须
 * get_css_set()。rebind 过渡中的控制器被强制解析到 root css，避免 task
 * 继续钉住即将 kill 的 per-cgroup css。
 */
static struct css_set *find_existing_css_set(struct css_set *old_cset,
					struct cgroup *cgrp,
					struct cgroup_subsys_state **template)
{
	struct cgroup_root *root = cgrp->root;
	struct cgroup_subsys *ss;
	struct css_set *cset;
	unsigned long key;
	int i;

	/*
	 * Build the set of subsystem state objects that we want to see in the
	 * new css_set. While subsystems can change globally, the entries here
	 * won't change, so no need for locking.
	 */
	/*
	 * 阶段 1：按每个控制器的绑定状态构造目标组合。全局 subsystem 表
	 * 稳定；cgroup_mutex 保证本次 root/mask 事务不会并发改变。
	 */
	for_each_subsys(ss, i) {
		if (unlikely(cgroup_rebind_ss_mask & (1UL << i))) {
			/*
			 * @ss is leaving this hierarchy and its per-cgroup
			 * csses are about to be killed. Resolve to the
			 * surviving root css so the tasks are migrated there.
			 */
			/*
			 * rebind 的逻辑摘除先于物理 css 销毁，root css 是
			 * 存活落点。
			 */
			template[i] = cgroup_css(&root->cgrp, ss);
			WARN_ON_ONCE(!template[i]);
		} else if (root->subsys_mask & (1UL << i)) {
			/*
			 * @ss is in this hierarchy, so we want the
			 * effective css from @cgrp.
			 */
			/*
			 * 控制器属于目标 root，本次组合应解析 cgrp 的
			 * effective css。
			 */
			template[i] = cgroup_e_css_by_mask(cgrp, ss);
		} else {
			/*
			 * @ss is not in this hierarchy, so we don't want
			 * to change the css.
			 */
			/* 其他 hierarchy 不受本次迁移影响，沿用 old_cset 的 css。 */
			template[i] = old_cset->subsys[i];
		}
	}

	/* 阶段 2：哈希只缩小候选范围，完整层级关系由 compare 再验证。 */
	key = css_set_hash(template);
	hash_for_each_possible(css_set_table, cset, hlist, key) {
		if (!compare_css_sets(cset, old_cset, cgrp, template))
			continue;

		/* This css_set matches what we need */
		/* 完整 css 与 root/cgroup 关系均匹配，可安全复用该共享组合。 */
		return cset;
	}

	/* No existing cgroup group matched */
	/* 哈希桶中无等价组合，调用者需分配并发布新的 css_set。 */
	return NULL;
}

/*
 * free_cgrp_cset_links() - 释放尚未发布的临时 cgroup/css_set 链接。
 *
 * links_to_free 是 allocate_cgrp_cset_links() 构造的私有 cset_link 链；
 * 节点尚未接入 cgroup，因而不持 cgroup 引用。函数摘链并 kfree 全部节点，
 * 不睡眠、无失败返回；调用后链表为空。
 */
static void free_cgrp_cset_links(struct list_head *links_to_free)
{
	struct cgrp_cset_link *link, *tmp_link;

	list_for_each_entry_safe(link, tmp_link, links_to_free, cset_link) {
		list_del(&link->cset_link);
		kfree(link);
	}
}

/**
 * allocate_cgrp_cset_links - allocate cgrp_cset_links
 * @count: the number of links to allocate
 * @tmp_links: list_head the allocated links are put on
 *
 * Allocate @count cgrp_cset_link structures and chain them on @tmp_links
 * through ->cset_link.  Returns 0 on success or -errno.
 */
/*
 * 中文契约：
 * count 是需要覆盖当前所有 hierarchy 的链接数量，必须非负；tmp_links
 * 是输出链表。函数可睡眠分配，成功返回 0 且调用者取得全部节点
 * ownership；任一分配失败返回 -ENOMEM，并在返回前释放此前节点，
 * 输出保持空链。
 */
static int allocate_cgrp_cset_links(int count, struct list_head *tmp_links)
{
	struct cgrp_cset_link *link;
	int i;

	INIT_LIST_HEAD(tmp_links);

	for (i = 0; i < count; i++) {
		link = kzalloc_obj(*link);
		if (!link) {
			free_cgrp_cset_links(tmp_links);
			return -ENOMEM;
		}
		list_add(&link->cset_link, tmp_links);
	}
	return 0;
}

/**
 * link_css_set - a helper function to link a css_set to a cgroup
 * @tmp_links: cgrp_cset_link objects allocated by allocate_cgrp_cset_links()
 * @cset: the css_set to be linked
 * @cgrp: the destination cgroup
 */
/*
 * 中文契约：
 * 调用者持 css_set_lock；tmp_links 至少有一个未发布节点；cset/cgrp 是
 * 正在构造且稳定的借用对象。函数消费一个临时 link，把它双向接入
 * cgrp->cset_links 与 cset->cgrp_links；非 root cgroup 同时取得一份
 * live 引用，由 put_css_set_locked() 最终归还。返回：无。
 */
static void link_css_set(struct list_head *tmp_links, struct css_set *cset,
			 struct cgroup *cgrp)
{
	struct cgrp_cset_link *link;

	BUG_ON(list_empty(tmp_links));

	if (cgroup_on_dfl(cgrp))
		cset->dfl_cgrp = cgrp;

	link = list_first_entry(tmp_links, struct cgrp_cset_link, cset_link);
	link->cset = cset;
	link->cgrp = cgrp;

	/*
	 * Always add links to the tail of the lists so that the lists are
	 * in chronological order.
	 */
	/*
	 * 两侧都按尾部插入维持 root/创建时间顺序，compare_css_sets() 才能
	 * 线性并行比较两个 cset，而不需排序。
	 */
	list_move_tail(&link->cset_link, &cgrp->cset_links);
	list_add_tail(&link->cgrp_link, &cset->cgrp_links);

	if (cgroup_parent(cgrp))
		cgroup_get_live(cgrp);
}

/**
 * find_css_set - return a new css_set with one cgroup updated
 * @old_cset: the baseline css_set
 * @cgrp: the cgroup to be updated
 *
 * Return a new css_set that's equivalent to @old_cset, but with @cgrp
 * substituted into the appropriate hierarchy.
 */
/*
 * 中文契约：
 * 调用者持 cgroup_mutex；old_cset 是迁移前借用组合，cgrp 是目标节点。
 * 函数先在 css_set_lock 下查找可复用组合；命中时返回新增引用。未命中
 * 则可睡眠分配 cset 和每-root 链接，初始化全部链表/引用，再在锁下统一
 * 发布到双向链接、哈希表和各 css 的 e_csets。
 *
 * threaded 组合发布后还递归查找其 domain cset 并取得引用。成功返回调用者
 * 持有的 css_set；内存失败返回 NULL，已发布的部分通过 put_css_set()
 * 完整回滚。新 cset 返回前保持无 task，因此中途可见但不会被 task 使用。
 */
static struct css_set *find_css_set(struct css_set *old_cset,
				    struct cgroup *cgrp)
{
	struct cgroup_subsys_state *template[CGROUP_SUBSYS_COUNT] = { };
	struct css_set *cset;
	struct list_head tmp_links;
	struct cgrp_cset_link *link;
	struct cgroup_subsys *ss;
	unsigned long key;
	int ssid;

	lockdep_assert_held(&cgroup_mutex);

	/* First see if we already have a cgroup group that matches
	 * the desired set */
	/*
	 * 先查找目标组合是否已存在；css_set 是可共享规范化对象，命中即可
	 * 避免重复分配并维持“同一组合对应同一活 cset”的哈希不变量。
	 */
	/* 阶段 1：锁内查重；引用必须在解锁前取得，防止候选并发到零。 */
	spin_lock_irq(&css_set_lock);
	cset = find_existing_css_set(old_cset, cgrp, template);
	if (cset)
		get_css_set(cset);
	spin_unlock_irq(&css_set_lock);

	if (cset)
		return cset;

	/* 阶段 2：锁外完成可能睡眠的对象和链接预分配，尚未全局可见。 */
	cset = kzalloc_obj(*cset);
	if (!cset)
		return NULL;

	/* Allocate all the cgrp_cset_link objects that we'll need */
	/*
	 * 每个已发布 root 都需要一条反向 link；一次性预分配使后续持
	 * css_set_lock 的提交阶段不再睡眠或发生部分链接失败。
	 */
	if (allocate_cgrp_cset_links(cgroup_root_count, &tmp_links) < 0) {
		kfree(cset);
		return NULL;
	}

	refcount_set(&cset->refcount, 1);
	cset->dom_cset = cset;
	INIT_LIST_HEAD(&cset->tasks);
	INIT_LIST_HEAD(&cset->mg_tasks);
	INIT_LIST_HEAD(&cset->dying_tasks);
	INIT_LIST_HEAD(&cset->task_iters);
	INIT_LIST_HEAD(&cset->threaded_csets);
	INIT_HLIST_NODE(&cset->hlist);
	INIT_LIST_HEAD(&cset->cgrp_links);
	INIT_LIST_HEAD(&cset->mg_src_preload_node);
	INIT_LIST_HEAD(&cset->mg_dst_preload_node);
	INIT_LIST_HEAD(&cset->mg_node);

	/* Copy the set of subsystem state objects generated in
	 * find_existing_css_set() */
	/*
	 * template 已由持锁拓扑快照生成，复制后成为新 cset 的
	 * 控制器组合。
	 */
	memcpy(cset->subsys, template, sizeof(cset->subsys));

	/* 阶段 3：一次临界区建立全部反向链接、引用并发布到哈希索引。 */
	spin_lock_irq(&css_set_lock);
	/* Add reference counts and links from the new css_set. */
	/*
	 * link_css_set() 同时取得 cgroup 引用并建立双向链；从此 cset 的
	 * hierarchy 关系完整，但尚未加入全局哈希供并发查找。
	 */
	list_for_each_entry(link, &old_cset->cgrp_links, cgrp_link) {
		struct cgroup *c = link->cgrp;

		if (c->root == cgrp->root)
			c = cgrp;
		link_css_set(&tmp_links, cset, c);
	}

	BUG_ON(!list_empty(&tmp_links));

	css_set_count++;

	/* Add @cset to the hash table */
	/*
	 * 哈希插入是新组合的全局发布点，必须晚于全部字段和反向链接
	 * 初始化。
	 */
	key = css_set_hash(cset->subsys);
	hash_add(css_set_table, &cset->hlist, key);

	for_each_subsys(ss, ssid) {
		struct cgroup_subsys_state *css = cset->subsys[ssid];

		list_add_tail(&cset->e_cset_node[ssid],
			      &css->cgroup->e_csets[ssid]);
		css_get(css);
	}

	spin_unlock_irq(&css_set_lock);

	/*
	 * If @cset should be threaded, look up the matching dom_cset and
	 * link them up.  We first fully initialize @cset then look for the
	 * dom_cset.  It's simpler this way and safe as @cset is guaranteed
	 * to stay empty until we return.
	 */
	/*
	 * 阶段 4：threaded cset 的资源域指向独立 dom_cset。先完整发布当前
	 * 对象再递归复用通用查找；无 task 保证尚无读者依赖临时
	 * dom_cset。
	 */
	if (cgroup_is_threaded(cset->dfl_cgrp)) {
		struct css_set *dcset;

		dcset = find_css_set(cset, cset->dfl_cgrp->dom_cgrp);
		if (!dcset) {
			put_css_set(cset);
			return NULL;
		}

		spin_lock_irq(&css_set_lock);
		cset->dom_cset = dcset;
		list_add_tail(&cset->threaded_csets_node,
			      &dcset->threaded_csets);
		spin_unlock_irq(&css_set_lock);
	}

	return cset;
}

/*
 * cgroup_root_from_kf() - 从 kernfs root 还原所属 cgroup_root。
 *
 * kf_root 是挂载/文件系统操作稳定的借用对象；kernfs 根节点 priv 指向
 * root cgroup，再由其 root 字段取得容器。返回裸借用指针，不取引用、
 * 不睡眠，生命周期仍由 kernfs root 的持有者保证。
 */
struct cgroup_root *cgroup_root_from_kf(struct kernfs_root *kf_root)
{
	struct cgroup *root_cgrp = kernfs_root_to_node(kf_root)->priv;

	return root_cgrp->root;
}

/*
 * cgroup_favor_dynmods() - 切换 root 的动态控制器模块迁移同步策略。
 *
 * root 是 cgroup_mutex 下稳定的借用对象，favor 是目标布尔状态。函数取得
 * 全局 per-CPU threadgroup rwsem 写锁，可能睡眠；启用时进入 rcu_sync
 * 并发布 root flag，使新迁移遵守更强同步。由于启用全局 rwsem 机制后
 * 旧的轻量读侧不能安全恢复，关闭请求只退出该 root 的 rcu_sync/flag，
 * 并对不可逆的全局机制告警。返回：无。
 */
void cgroup_favor_dynmods(struct cgroup_root *root, bool favor)
{
	bool favoring = root->flags & CGRP_ROOT_FAVOR_DYNMODS;

	/*
	 * see the comment above CGRP_ROOT_FAVOR_DYNMODS definition.
	 * favordynmods can flip while task is between
	 * cgroup_threadgroup_change_begin() and end(), so down_write global
	 * cgroup_threadgroup_rwsem to synchronize them.
	 *
	 * Once cgroup_enable_per_threadgroup_rwsem is enabled, holding
	 * cgroup_threadgroup_rwsem doesn't exlude tasks between
	 * cgroup_thread_group_change_begin() and end() and thus it's unsafe to
	 * turn off. As the scenario is unlikely, simply disallow disabling once
	 * enabled and print out a warning.
	 */
	/*
	 * 中文补充：写锁跨越 flag 与 rcu_sync 更新，保证没有 task 正卡在
	 * begin/end 之间观察半切换协议。单向全局开关避免两套读侧失配。
	 */
	percpu_down_write(&cgroup_threadgroup_rwsem);
	if (favor && !favoring) {
		cgroup_enable_per_threadgroup_rwsem = true;
		rcu_sync_enter(&cgroup_threadgroup_rwsem.rss);
		root->flags |= CGRP_ROOT_FAVOR_DYNMODS;
	} else if (!favor && favoring) {
		if (cgroup_enable_per_threadgroup_rwsem)
			pr_warn_once("cgroup favordynmods: per threadgroup rwsem mechanism can't be disabled\n");
		rcu_sync_exit(&cgroup_threadgroup_rwsem.rss);
		root->flags &= ~CGRP_ROOT_FAVOR_DYNMODS;
	}
	percpu_up_write(&cgroup_threadgroup_rwsem);
}

/*
 * cgroup_init_root_id() - 为尚未发布的 root 分配层级 ID。
 *
 * 调用者持 cgroup_mutex；root 是输出对象。idr_alloc_cyclic() 可睡眠，
 * 成功把非负 ID 写入 hierarchy_id 并让 IDR 可查找 root；失败返回 errno，
 * 字段/映射不建立，调用者负责销毁 root。
 */
static int cgroup_init_root_id(struct cgroup_root *root)
{
	int id;

	lockdep_assert_held(&cgroup_mutex);

	id = idr_alloc_cyclic(&cgroup_hierarchy_idr, root, 0, 0, GFP_KERNEL);
	if (id < 0)
		return id;

	root->hierarchy_id = id;
	return 0;
}

/*
 * cgroup_exit_root_id() - 摘除 root 的层级 ID 映射。
 *
 * 调用者持 cgroup_mutex，root 尚存活且其 hierarchy_id 已分配。摘除后新
 * ID 查找不能获得 root，但函数不释放 root，也不等待既有引用。返回：无。
 */
static void cgroup_exit_root_id(struct cgroup_root *root)
{
	lockdep_assert_held(&cgroup_mutex);

	idr_remove(&cgroup_hierarchy_idr, root->hierarchy_id);
}

/*
 * cgroup_free_root() - 在 RCU 宽限期后最终释放动态 root。
 *
 * root 已从所有发布索引摘除，调用者转移最后 ownership。kfree_rcu()
 * 保护仍在 RCU 读侧使用 root 的旧读者；函数不睡眠、无直接返回值。
 */
void cgroup_free_root(struct cgroup_root *root)
{
	kfree_rcu(root, rcu);
}

/*
 * cgroup_destroy_root() - 完成已卸载 legacy root 的全局拆除。
 *
 * root 由 kernfs superblock 销毁路径交入；进入时 cgroup_mutex 已持有，
 * root 无普通 cgroup/children。函数可能睡眠：等待默认根 offline 工作、
 * 调 notifier、把所有控制器 rebind 回 v2 根，随后在 css_set_lock 下
 * 拆除所有 cset link，从 root_list/IDR 摘除并解开 cgroup_mutex。
 *
 * 解锁后销毁 kernfs root 并通过 RCU 释放 root。无错误返回；rebind 或
 * notifier 异常只能 WARN，因为卸载已越过提交点，不能恢复已拆层级。
 */
static void cgroup_destroy_root(struct cgroup_root *root)
{
	struct cgroup *cgrp = &root->cgrp;
	struct cgrp_cset_link *link, *tmp_link;
	int ret;

	/*
	 * 阶段 1：先记录销毁事件，并等待影响 rebind 的 offline 工作排空。
	 */
	trace_cgroup_destroy_root(root);

	cgroup_lock_and_drain_offline(&cgrp_dfl_root.cgrp);

	BUG_ON(atomic_read(&root->nr_cgrps));
	BUG_ON(!list_empty(&cgrp->self.children));

	ret = blocking_notifier_call_chain(&cgroup_lifetime_notifier,
					   CGROUP_LIFETIME_OFFLINE, cgrp);
	WARN_ON_ONCE(notifier_to_errno(ret));

	/* Rebind all subsystems back to the default hierarchy */
	/* 控制器必须先回到永久默认根，之后才可拆 root 与 css_set 关联。 */
	WARN_ON(rebind_subsystems(&cgrp_dfl_root, root->subsys_mask));

	/*
	 * Release all the links from cset_links to this hierarchy's
	 * root cgroup
	 */
	/*
	 * 阶段 2：root cgroup 自身不由普通 link 引用计数，直接双向摘链；
	 * css_set_lock 阻止 task 查找/迁移同时遍历这些链接。
	 */
	spin_lock_irq(&css_set_lock);

	list_for_each_entry_safe(link, tmp_link, &cgrp->cset_links, cset_link) {
		list_del(&link->cset_link);
		list_del(&link->cgrp_link);
		kfree(link);
	}

	spin_unlock_irq(&css_set_lock);

	WARN_ON_ONCE(list_empty(&root->root_list));
	/*
	 * 阶段 3：从全局发现路径和 IDR 摘除，再释放总锁进入物理销毁。
	 */
	list_del_rcu(&root->root_list);
	cgroup_root_count--;

	if (!have_favordynmods)
		cgroup_favor_dynmods(root, false);

	cgroup_exit_root_id(root);

	cgroup_unlock();

	kernfs_destroy_root(root->kf_root);
	cgroup_free_root(root);
}

/*
 * Returned cgroup is without refcount but it's valid as long as cset pins it.
 */
/*
 * 中文契约：
 * cset/root 是锁或 RCU 稳定的借用对象。init cset 映射到任意 root 根；
 * v2 使用缓存 dfl_cgrp；v1 在 css_set_lock 下遍历按 root 排列的链接。
 * 返回裸 cgroup 或 NULL，不增加引用；只要 cset 仍被持有，对应非根 cgroup
 * 引用也被其 link 钉住。未持 cgroup_mutex 时 root 并发卸载可导致 NULL。
 */
static inline struct cgroup *__cset_cgroup_from_root(struct css_set *cset,
					    struct cgroup_root *root)
{
	struct cgroup *res_cgroup = NULL;

	if (cset == &init_css_set) {
		res_cgroup = &root->cgrp;
	} else if (root == &cgrp_dfl_root) {
		res_cgroup = cset->dfl_cgrp;
	} else {
		struct cgrp_cset_link *link;
		lockdep_assert_held(&css_set_lock);

		list_for_each_entry(link, &cset->cgrp_links, cgrp_link) {
			struct cgroup *c = link->cgrp;

			if (c->root == root) {
				res_cgroup = c;
				break;
			}
		}
	}

	/*
	 * If cgroup_mutex is not held, the cgrp_cset_link will be freed
	 * before we remove the cgroup root from the root_list. Consequently,
	 * when accessing a cgroup root, the cset_link may have already been
	 * freed, resulting in a NULL res_cgroup. However, by holding the
	 * cgroup_mutex, we ensure that res_cgroup can't be NULL.
	 * If we don't hold cgroup_mutex in the caller, we must do the NULL
	 * check.
	 */
	/*
	 * 中文补充：root_list 摘除晚于 link 释放，因此无 mutex 读者必须把
	 * NULL 视为合法卸载竞态，而不是假定每个 cset 永远含该 root 链接。
	 */
	return res_cgroup;
}

/*
 * look up cgroup associated with current task's cgroup namespace on the
 * specified hierarchy
 */
/*
 * 中文契约：
 * 调用者持 css_set_lock，且 current 持 namespace_sem 使其 cgroup
 * namespace/root 不能卸载。函数在 RCU 读侧借用 root_cset 并解析目标
 * hierarchy；返回无引用 cgroup，但 namespace 钉住其生命周期，NULL
 * 属于不变量破坏并告警。不可睡眠、无副作用。
 */
static struct cgroup *
current_cgns_cgroup_from_root(struct cgroup_root *root)
{
	struct cgroup *res = NULL;
	struct css_set *cset;

	lockdep_assert_held(&css_set_lock);

	rcu_read_lock();

	cset = current->nsproxy->cgroup_ns->root_cset;
	res = __cset_cgroup_from_root(cset, root);

	rcu_read_unlock();

	/*
	 * The namespace_sem is held by current, so the root cgroup can't
	 * be umounted. Therefore, we can ensure that the res is non-NULL.
	 */
	/* namespace_sem 提供 root 存活保证，RCU 只覆盖 root_cset 指针读取。 */
	WARN_ON_ONCE(!res);
	return res;
}

/*
 * Look up cgroup associated with current task's cgroup namespace on the default
 * hierarchy.
 *
 * Unlike current_cgns_cgroup_from_root(), this doesn't need locks:
 * - Internal rcu_read_lock is unnecessary because we don't dereference any rcu
 *   pointers.
 * - css_set_lock is not needed because we just read cset->dfl_cgrp.
 * - As a bonus returned cgrp is pinned with the current because it cannot
 *   switch cgroup_ns asynchronously.
 */
/*
 * 中文契约：
 * 无需显式锁地取得 current namespace 在 v2 根的可见起点。current 不会
 * 异步切换自己的 cgroup_ns，root_cset 钉住 dfl_cgrp，因此返回裸指针
 * 在当前操作内有效。若退出已清空 nsproxy，则退到全局 v2 根，让 ID
 * 查找不被一个已消失 namespace 人为限制。函数不取引用、不睡眠。
 */
static struct cgroup *current_cgns_cgroup_dfl(void)
{
	struct css_set *cset;

	if (current->nsproxy) {
		cset = current->nsproxy->cgroup_ns->root_cset;
		return __cset_cgroup_from_root(cset, &cgrp_dfl_root);
	} else {
		/*
		 * NOTE: This function may be called from bpf_cgroup_from_id()
		 * on a task which has already passed exit_nsproxy_namespaces()
		 * and nsproxy == NULL. Fall back to cgrp_dfl_root which will
		 * make all cgroups visible for lookups.
		 */
		/*
		 * 中文补充：这是 task 退出后 BPF 查询的明确 fallback，不表示
		 * task 重新加入根；它只决定本次 namespace 可见性边界。
		 */
		return &cgrp_dfl_root.cgrp;
	}
}

/* look up cgroup associated with given css_set on the specified hierarchy */
/*
 * cset_cgroup_from_root() - 有锁版本的 css_set 到 hierarchy 节点映射。
 *
 * 调用者持 css_set_lock，cset/root 均为借用对象；返回由 cset link 钉住
 * 的裸 cgroup 或卸载竞态下 NULL，不增加引用、无副作用。
 */
static struct cgroup *cset_cgroup_from_root(struct css_set *cset,
					    struct cgroup_root *root)
{
	lockdep_assert_held(&css_set_lock);

	return __cset_cgroup_from_root(cset, root);
}

/*
 * Return the cgroup for "task" from the given hierarchy. Must be
 * called with css_set_lock held to prevent task's groups from being modified.
 * Must be called with either cgroup_mutex or rcu read lock to prevent the
 * cgroup root from being destroyed.
 */
/*
 * 中文契约：
 * task/root 是借用对象；调用者必须持 css_set_lock 防止 task->cgroups
 * 迁移，并持 cgroup_mutex 或 RCU 读锁防止 root 销毁。返回 cset 钉住的
 * 裸 cgroup，可能在未满足 mutex 保证的卸载窗口为 NULL；不取 task/cgroup
 * 引用，不睡眠。
 */
struct cgroup *task_cgroup_from_root(struct task_struct *task,
				     struct cgroup_root *root)
{
	/*
	 * No need to lock the task - since we hold css_set_lock the
	 * task can't change groups.
	 */
	/* css_set_lock 使 task_css_set() 的 RCU 指针在此次映射期间不改变。 */
	return cset_cgroup_from_root(task_css_set(task), root);
}

/*
 * A task must hold cgroup_mutex to modify cgroups.
 *
 * Any task can increment and decrement the count field without lock.
 * So in general, code holding cgroup_mutex can't rely on the count
 * field not changing.  However, if the count goes to zero, then only
 * cgroup_attach_task() can increment it again.  Because a count of zero
 * means that no tasks are currently attached, therefore there is no
 * way a task attached to that cgroup can fork (the other way to
 * increment the count).  So code holding cgroup_mutex can safely
 * assume that if the count is zero, it will stay zero. Similarly, if
 * a task holds cgroup_mutex on a cgroup with zero count, it
 * knows that the cgroup won't be removed, as cgroup_rmdir()
 * needs that mutex.
 *
 * A cgroup can only be deleted if both its 'count' of using tasks
 * is zero, and its list of 'children' cgroups is empty.  Since all
 * tasks in the system use _some_ cgroup, and since there is always at
 * least one task in the system (init, pid == 1), therefore, root cgroup
 * always has either children cgroups and/or using tasks.  So we don't
 * need a special hack to ensure that root cgroup cannot be deleted.
 *
 * P.S.  One more locking exception.  RCU is used to guard the
 * update of a tasks cgroup pointer by cgroup_attach_task()
 */
/*
 * 中文并发总结：cgroup_mutex 让“空 cgroup 保持空直到本次修改结束”成立，
 * 因为重新附加必须取得同一锁；普通 fork/exit 可无锁改 task 数，故非零
 * 计数不是稳定快照。删除还要求 children 为空。task->cgroups 的读取则
 * 另由 css_set_lock/RCU 协议保护，不能拿引用计数替代结构锁。
 */

static struct kernfs_syscall_ops cgroup_kf_syscall_ops;

/*
 * cgroup_file_name() - 根据层级版本和 cftype 标志生成 kernfs 文件名。
 *
 * cgrp/cft 是发布配置期的借用对象；buf 至少 CGROUP_FILE_NAME_MAX 字节，
 * 由调用者拥有并接收 NUL 结尾结果。默认带 subsystem 前缀，v2 使用
 * ss->name、v1 使用 legacy_name；NO_PREFIX/root noprefix 直接使用 cft
 * 名称，DEBUG 文件插入专用标识。返回 buf 本身，无分配、无失败。
 */
static char *cgroup_file_name(struct cgroup *cgrp, const struct cftype *cft,
			      char *buf)
{
	struct cgroup_subsys *ss = cft->ss;

	if (cft->ss && !(cft->flags & CFTYPE_NO_PREFIX) &&
	    !(cgrp->root->flags & CGRP_ROOT_NOPREFIX)) {
		const char *dbg = (cft->flags & CFTYPE_DEBUG) ? ".__DEBUG__." : "";

		snprintf(buf, CGROUP_FILE_NAME_MAX, "%s%s.%s",
			 dbg, cgroup_on_dfl(cgrp) ? ss->name : ss->legacy_name,
			 cft->name);
	} else {
		strscpy(buf, cft->name, CGROUP_FILE_NAME_MAX);
	}
	return buf;
}

/**
 * cgroup_file_mode - deduce file mode of a control file
 * @cft: the control file in question
 *
 * S_IRUGO for read, S_IWUSR for write.
 */
/*
 * 中文契约：
 * cft 是静态或已注册控制文件描述的借用指针。存在任一 read/seq 回调就
 * 赋所有用户可读；存在 write 回调默认仅 owner 可写，WORLD_WRITABLE
 * 才扩为所有用户。返回权限位，不含文件类型，不修改 cft、不会睡眠。
 */
static umode_t cgroup_file_mode(const struct cftype *cft)
{
	umode_t mode = 0;

	if (cft->read_u64 || cft->read_s64 || cft->seq_show)
		mode |= S_IRUGO;

	if (cft->write_u64 || cft->write_s64 || cft->write) {
		if (cft->flags & CFTYPE_WORLD_WRITABLE)
			mode |= S_IWUGO;
		else
			mode |= S_IWUSR;
	}

	return mode;
}

/**
 * cgroup_calc_subtree_ss_mask - calculate subtree_ss_mask
 * @subtree_control: the new subtree_control mask to consider
 * @this_ss_mask: available subsystems
 *
 * On the default hierarchy, a subsystem may request other subsystems to be
 * enabled together through its ->depends_on mask.  In such cases, more
 * subsystems than specified in "cgroup.subtree_control" may be enabled.
 *
 * This function calculates which subsystems need to be enabled if
 * @subtree_control is to be applied while restricted to @this_ss_mask.
 */
/*
 * 中文契约：
 * 调用者持 cgroup_mutex。subtree_control 是用户显式请求位，
 * this_ss_mask 是当前节点实际可用控制器集合；函数加入隐式控制器，并
 * 反复闭包所有 ->depends_on，最后每轮与可用集合相交。
 *
 * 返回稳定依赖闭包，不修改全局状态、不取引用。若依赖控制器绑定在
 * 其他 hierarchy，它被掩掉；调用者随后据此创建/销毁 css。循环单调
 * 加位且位宽有限，必然收敛。
 */
static u32 cgroup_calc_subtree_ss_mask(u32 subtree_control, u32 this_ss_mask)
{
	u32 cur_ss_mask = subtree_control;
	struct cgroup_subsys *ss;
	int ssid;

	lockdep_assert_held(&cgroup_mutex);

	cur_ss_mask |= cgrp_dfl_implicit_ss_mask;

	while (true) {
		u32 new_ss_mask = cur_ss_mask;

		do_each_subsys_mask(ss, ssid, cur_ss_mask) {
			new_ss_mask |= ss->depends_on;
		} while_each_subsys_mask();

		/*
		 * Mask out subsystems which aren't available.  This can
		 * happen only if some depended-upon subsystems were bound
		 * to non-default hierarchies.
		 */
		/*
		 * 中文补充：depends_on 只是需求声明，不能越过 root 的实际
		 * 绑定；缺失依赖会在这里裁掉，而不生成跨 hierarchy 的 css。
		 */
		new_ss_mask &= this_ss_mask;

		if (new_ss_mask == cur_ss_mask)
			break;
		cur_ss_mask = new_ss_mask;
	}

	return cur_ss_mask;
}

/**
 * cgroup_kn_unlock - unlocking helper for cgroup kernfs methods
 * @kn: the kernfs_node being serviced
 *
 * This helper undoes cgroup_kn_lock_live() and should be invoked before
 * the method finishes if locking succeeded.  Note that once this function
 * returns the cgroup returned by cgroup_kn_lock_live() may become
 * inaccessible any time.  If the caller intends to continue to access the
 * cgroup, it should pin it before invoking this function.
 */
/*
 * 中文契约：
 * kn 是已由 cgroup_kn_lock_live() 成功锁定的当前 kernfs 节点。函数先
 * 解析并借用对应 cgroup，释放 cgroup_mutex，再恢复 kernfs active
 * protection，最后归还 tryget 引用。返回：无；完成后先前返回的 cgrp
 * 可立即被并发删除，继续使用者必须在调用前另取引用。
 */
void cgroup_kn_unlock(struct kernfs_node *kn)
{
	struct cgroup *cgrp;

	if (kernfs_type(kn) == KERNFS_DIR)
		cgrp = kn->priv;
	else
		cgrp = kn_priv(kn);

	cgroup_unlock();

	kernfs_unbreak_active_protection(kn);
	cgroup_put(cgrp);
}

/**
 * cgroup_kn_lock_live - locking helper for cgroup kernfs methods
 * @kn: the kernfs_node being serviced
 * @drain_offline: perform offline draining on the cgroup
 *
 * This helper is to be used by a cgroup kernfs method currently servicing
 * @kn.  It breaks the active protection, performs cgroup locking and
 * verifies that the associated cgroup is alive.  Returns the cgroup if
 * alive; otherwise, %NULL.  A successful return should be undone by a
 * matching cgroup_kn_unlock() invocation.  If @drain_offline is %true, the
 * cgroup is drained of offlining csses before return.
 *
 * Any cgroup kernfs method implementation which requires locking the
 * associated cgroup should use this helper.  It avoids nesting cgroup
 * locking under kernfs active protection and allows all kernfs operations
 * including self-removal.
 */
/*
 * 中文契约：
 * kn 是正在执行回调且受 active protection 的目录或文件节点；
 * drain_offline 决定加 cgroup_mutex 后是否等待离线 css 排空。函数先
 * cgroup_tryget() 稳定对象，再主动打破 kernfs active 保护，避免锁序
 * kernfs-active -> cgroup_mutex 与删除路径反转。
 *
 * 活 cgroup 返回持有引用且 cgroup_mutex 仍锁定，调用者必须配对
 * cgroup_kn_unlock()；已死亡/取引用失败返回 NULL，内部恢复全部状态。
 * 可能睡眠，尤其 drain_offline=true 时会等待 workqueue。
 */
struct cgroup *cgroup_kn_lock_live(struct kernfs_node *kn, bool drain_offline)
{
	struct cgroup *cgrp;

	if (kernfs_type(kn) == KERNFS_DIR)
		cgrp = kn->priv;
	else
		cgrp = kn_priv(kn);

	/*
	 * We're gonna grab cgroup_mutex which nests outside kernfs
	 * active_ref.  cgroup liveliness check alone provides enough
	 * protection against removal.  Ensure @cgrp stays accessible and
	 * break the active_ref protection.
	 */
	/*
	 * tryget 必须早于 break active：active 保护撤销后 kernfs 可删除节点，
	 * 独立 cgroup 引用成为跨越该窗口的唯一存活保证。
	 */
	if (!cgroup_tryget(cgrp))
		return NULL;
	kernfs_break_active_protection(kn);

	if (drain_offline)
		cgroup_lock_and_drain_offline(cgrp);
	else
		cgroup_lock();

	if (!cgroup_is_dead(cgrp))
		return cgrp;

	cgroup_kn_unlock(kn);
	return NULL;
}

/*
 * cgroup_rm_file() - 从 cgrp 目录撤销一个 cftype 文件实例。
 *
 * 调用者持 cgroup_mutex；cgrp/cft 为借用对象。若 cft 在 css 内嵌
 * cgroup_file，先在其自旋锁下把 kn 发布为 NULL，阻止新通知，再同步
 * 删除 timer；最后按生成名称从 kernfs 摘除并等待 active 使用按 kernfs
 * 协议收尾。返回：无，文件不存在也按幂等删除处理。
 */
static void cgroup_rm_file(struct cgroup *cgrp, const struct cftype *cft)
{
	char name[CGROUP_FILE_NAME_MAX];

	lockdep_assert_held(&cgroup_mutex);

	if (cft->file_offset) {
		struct cgroup_subsys_state *css = cgroup_css(cgrp, cft->ss);
		struct cgroup_file *cfile = (void *)css + cft->file_offset;

		spin_lock_irq(&cfile->lock);
		WRITE_ONCE(cfile->kn, NULL);
		spin_unlock_irq(&cfile->lock);

		timer_delete_sync(&cfile->notify_timer);
	}

	kernfs_remove_by_name(cgrp->kn, cgroup_file_name(cgrp, cft, name));
}

/**
 * css_clear_dir - remove subsys files in a cgroup directory
 * @css: target css
 */
/*
 * 中文契约：
 * css 是 cgroup_mutex 下稳定的借用状态。不可见时幂等返回；否则先清
 * CSS_VISIBLE，阻止逻辑上的新使用，再按 self/v1/v2/subsystem 分类删除
 * 对应 cftype 集。函数可能因 timer/kernfs drain 睡眠；返回后目录中不再
 * 发布该 css 文件，但 css 对象本身仍由后续 offline/release 管理。
 */
static void css_clear_dir(struct cgroup_subsys_state *css)
{
	struct cgroup *cgrp = css->cgroup;
	struct cftype *cfts;

	if (!(css->flags & CSS_VISIBLE))
		return;

	css->flags &= ~CSS_VISIBLE;

	if (css_is_self(css)) {
		if (cgroup_on_dfl(cgrp)) {
			cgroup_addrm_files(css, cgrp,
					   cgroup_base_files, false);
			if (cgroup_psi_enabled())
				cgroup_addrm_files(css, cgrp,
						   cgroup_psi_files, false);
		} else {
			cgroup_addrm_files(css, cgrp,
					   cgroup1_base_files, false);
		}
	} else {
		list_for_each_entry(cfts, &css->ss->cfts, node)
			cgroup_addrm_files(css, cgrp, cfts, false);
	}
}

/**
 * css_populate_dir - create subsys files in a cgroup directory
 * @css: target css
 *
 * On failure, no file is added.
 */
/*
 * 中文契约：
 * css 在 cgroup_mutex 下已初始化但可能尚未对用户可见。已 visible 时
 * 幂等成功。函数按 core v1/v2 或 subsystem cfts 创建 kernfs 文件，
 * 可睡眠分配；全部成功才设置 CSS_VISIBLE。
 *
 * 任一创建失败返回 errno，并逆序/按已完成集合删除所有本次文件，保证
 * 英文契约所述“失败后一个也不留下”。css/cftype ownership 不转移。
 */
static int css_populate_dir(struct cgroup_subsys_state *css)
{
	struct cgroup *cgrp = css->cgroup;
	struct cftype *cfts, *failed_cfts;
	int ret;

	if (css->flags & CSS_VISIBLE)
		return 0;

	/* 阶段 1：core self 选择 v2 base+可选 PSI 或 v1 base 文件集。 */
	if (css_is_self(css)) {
		if (cgroup_on_dfl(cgrp)) {
			ret = cgroup_addrm_files(css, cgrp,
						 cgroup_base_files, true);
			if (ret < 0)
				return ret;

			if (cgroup_psi_enabled()) {
				ret = cgroup_addrm_files(css, cgrp,
							 cgroup_psi_files, true);
				if (ret < 0) {
					cgroup_addrm_files(css, cgrp,
							   cgroup_base_files, false);
					return ret;
				}
			}
		} else {
			ret = cgroup_addrm_files(css, cgrp,
						 cgroup1_base_files, true);
			if (ret < 0)
				return ret;
		}
	} else {
		/* 控制器可能注册多组 cfts；逐组发布并记住首个失败组。 */
		list_for_each_entry(cfts, &css->ss->cfts, node) {
			ret = cgroup_addrm_files(css, cgrp, cfts, true);
			if (ret < 0) {
				failed_cfts = cfts;
				goto err;
			}
		}
	}

	css->flags |= CSS_VISIBLE;

	return 0;
err:
	/* 仅撤销 failed_cfts 之前已成功的组，失败组由 addrm_files 自清理。 */
	list_for_each_entry(cfts, &css->ss->cfts, node) {
		if (cfts == failed_cfts)
			break;
		cgroup_addrm_files(css, cgrp, cfts, false);
	}
	return ret;
}

/*
 * rebind_subsystems() - 在默认根与一个 legacy 根之间迁移控制器归属。
 *
 * 调用者持 cgroup_mutex；dst_root 是目标层级，ss_mask 是待迁移控制器位。
 * 函数可能睡眠并调用控制器 bind/online/offline 回调。预检拒绝仍有非根
 * css 的控制器（隐式 v2 控制器例外）及两个非默认根之间的直接移动。
 *
 * 迁出时先用 cgroup_rebind_ss_mask 让所有 task 的 effective css 回落到
 * root，再 kill per-cgroup css；随后 RCU 重挂 root css、在 css_set_lock
 * 下移动反向 cset 链和修补迭代器，更新 static key，最后在目标根应用
 * 控制器并激活 kernfs。
 *
 * 预检失败返回 -EBUSY 且无状态变化；通过预检后事务基本不可回滚，
 * apply 的局部失败只告警但函数返回 0，调用者得到已重绑定的控制器。
 */
int rebind_subsystems(struct cgroup_root *dst_root, u32 ss_mask)
{
	struct cgroup *dcgrp = &dst_root->cgrp;
	struct cgroup_subsys *ss;
	int ssid, ret;
	u32 dfl_disable_ss_mask = 0;

	lockdep_assert_held(&cgroup_mutex);

	/* 阶段 1：在任何修改前验证整组控制器都可迁移。 */
	do_each_subsys_mask(ss, ssid, ss_mask) {
		/*
		 * If @ss has non-root csses attached to it, can't move.
		 * If @ss is an implicit controller, it is exempt from this
		 * rule and can be stolen.
		 */
		/*
		 * 普通控制器仍有非 root css 时重绑会遗失其 per-cgroup 状态；
		 * implicit controller 的状态由 core 事务统一迁移，允许被收回。
		 */
		if (css_next_child(NULL, cgroup_css(&ss->root->cgrp, ss)) &&
		    !ss->implicit_on_dfl)
			return -EBUSY;

		/* can't move between two non-dummy roots either */
		/*
		 * 不支持两个 legacy 实根直接互迁，必须以 default root
		 * 作为中转。
		 */
		if (ss->root != &cgrp_dfl_root && dst_root != &cgrp_dfl_root)
			return -EBUSY;

		/*
		 * Collect ssid's that need to be disabled from default
		 * hierarchy.
		 */
		/*
		 * 汇总从 v2 离开的控制器，以一次 apply/finalize 事务
		 * 整批禁用。
		 */
		if (ss->root == &cgrp_dfl_root)
			dfl_disable_ss_mask |= 1 << ssid;

	} while_each_subsys_mask();

	if (dfl_disable_ss_mask) {
		struct cgroup *scgrp = &cgrp_dfl_root.cgrp;

		/*
		 * Controllers leaving the default hierarchy are disabled
		 * together. cgroup_rebind_ss_mask makes cgroup_apply_control()
		 * migrate their tasks to the root css, so the per-cgroup csses
		 * are unpopulated when cgroup_finalize_control() kills them.
		 * Clear it before cgroup_finalize_control(), which does no
		 * css_set lookup.
		 */
		/*
		 * 阶段 2：整批先从 v2 禁用。临时 mask 只覆盖 apply/migrate，
		 * finalize 时 task 已不再引用旧 per-cgroup css，必须先清零。
		 */
		cgrp_dfl_root.subsys_mask &= ~dfl_disable_ss_mask;
		cgroup_rebind_ss_mask = dfl_disable_ss_mask;
		WARN_ON(cgroup_apply_control(scgrp));
		cgroup_rebind_ss_mask = 0;
		cgroup_finalize_control(scgrp, 0);
	}

	/* 阶段 3：逐控制器重挂唯一 root css 和全部反向索引。 */
	do_each_subsys_mask(ss, ssid, ss_mask) {
		struct cgroup_root *src_root = ss->root;
		struct cgroup *scgrp = &src_root->cgrp;
		struct cgroup_subsys_state *css = cgroup_css(scgrp, ss);
		struct css_set *cset, *cset_pos;
		struct css_task_iter *it;

		WARN_ON(!css || cgroup_css(dcgrp, ss));

		if (src_root != &cgrp_dfl_root) {
			/*
			 * Disable from the source, migrating its tasks to the
			 * root css first (see cgroup_rebind_ss_mask).
			 */
			/*
			 * 从 legacy 源解绑前先令 task 回落到该层级 root css，
			 * 确保旧 per-cgroup css 不再 populated，随后才可销毁。
			 */
			src_root->subsys_mask &= ~(1 << ssid);
			cgroup_rebind_ss_mask = 1 << ssid;
			WARN_ON(cgroup_apply_control(scgrp));
			cgroup_rebind_ss_mask = 0;
			cgroup_finalize_control(scgrp, 0);
		}

		/* rebind */
		/*
		 * RCU 发布顺序先从源断开再挂目标；cgroup_mutex 阻止并发
		 * 重绑定。RCU 读者看到旧关联或新关联，root 钉住 css 本体。
		 */
		RCU_INIT_POINTER(scgrp->subsys[ssid], NULL);
		rcu_assign_pointer(dcgrp->subsys[ssid], css);
		ss->root = dst_root;

		spin_lock_irq(&css_set_lock);
		css->cgroup = dcgrp;
		WARN_ON(!list_empty(&dcgrp->e_csets[ss->id]));
		list_for_each_entry_safe(cset, cset_pos, &scgrp->e_csets[ss->id],
					 e_cset_node[ss->id]) {
			list_move_tail(&cset->e_cset_node[ss->id],
				       &dcgrp->e_csets[ss->id]);
			/*
			 * all css_sets of scgrp together in same order to dcgrp,
			 * patch in-flight iterators to preserve correct iteration.
			 * since the iterator is always advanced right away and
			 * finished when it->cset_pos meets it->cset_head, so only
			 * update it->cset_head is enough here.
			 */
			/*
			 * 中文补充：迭代器下一步立即推进且以 head 判结束；
			 * 整段 cset 顺序不变，只需把 head 改为目标链表。
			 */
			list_for_each_entry(it, &cset->task_iters, iters_node)
				if (it->cset_head == &scgrp->e_csets[ss->id])
					it->cset_head = &dcgrp->e_csets[ss->id];
		}
		spin_unlock_irq(&css_set_lock);

		/* default hierarchy doesn't enable controllers by default */
		/*
		 * v2 只把控制器绑定到 root，仍需用户通过 subtree_control 下放；
		 * legacy root 则在根处立即启用，故同时更新根控制位。
		 */
		dst_root->subsys_mask |= 1 << ssid;
		if (dst_root == &cgrp_dfl_root) {
			static_branch_enable(cgroup_subsys_on_dfl_key[ssid]);
		} else {
			dcgrp->subtree_control |= 1 << ssid;
			static_branch_disable(cgroup_subsys_on_dfl_key[ssid]);
		}

		/*
		 * 阶段 4：物化目标层级 css/文件；失败已不能恢复旧 root。
		 */
		ret = cgroup_apply_control(dcgrp);
		if (ret)
			pr_warn("partial failure to rebind %s controller (err=%d)\n",
				ss->name, ret);

		if (ss->bind)
			ss->bind(css);
	} while_each_subsys_mask();

	kernfs_activate(dcgrp->kn);
	return 0;
}

/*
 * cgroup_show_path() - 相对 current cgroup namespace 输出 kernfs 路径。
 *
 * sf 是 seq_file 输出，kf_node 是目标节点，kf_root 是所属层级；均为调用
 * 期间借用。函数可睡眠分配 PATH_MAX 缓冲；css_set_lock 下稳定 current
 * namespace 根并调用 kernfs 生成相对路径，随后转义空白和反斜杠。
 *
 * 成功向 sf 追加文本并返回 0；-ENOMEM 表示缓冲分配失败，过长的 -E2BIG
 * 规范化为 -ERANGE，其他 kernfs errno 原样返回。缓冲在所有出口释放。
 */
int cgroup_show_path(struct seq_file *sf, struct kernfs_node *kf_node,
		     struct kernfs_root *kf_root)
{
	int len = 0;
	char *buf = NULL;
	struct cgroup_root *kf_cgroot = cgroup_root_from_kf(kf_root);
	struct cgroup *ns_cgroup;

	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	spin_lock_irq(&css_set_lock);
	ns_cgroup = current_cgns_cgroup_from_root(kf_cgroot);
	len = kernfs_path_from_node(kf_node, ns_cgroup->kn, buf, PATH_MAX);
	spin_unlock_irq(&css_set_lock);

	if (len == -E2BIG)
		len = -ERANGE;
	else if (len > 0) {
		seq_escape(sf, buf, " \t\n\\");
		len = 0;
	}
	kfree(buf);
	return len;
}

/* cgroup2 挂载参数编号与下方 fs_parameter_spec/解析 switch 一一对应。 */
enum cgroup2_param {
	Opt_nsdelegate,
	Opt_favordynmods,
	Opt_memory_localevents,
	Opt_memory_recursiveprot,
	Opt_memory_hugetlb_accounting,
	Opt_pids_localevents,
	nr__cgroup2_params
};

static const struct fs_parameter_spec cgroup2_fs_parameters[] = {
	fsparam_flag("nsdelegate",		Opt_nsdelegate),
	fsparam_flag("favordynmods",		Opt_favordynmods),
	fsparam_flag("memory_localevents",	Opt_memory_localevents),
	fsparam_flag("memory_recursiveprot",	Opt_memory_recursiveprot),
	fsparam_flag("memory_hugetlb_accounting", Opt_memory_hugetlb_accounting),
	fsparam_flag("pids_localevents",	Opt_pids_localevents),
	{}
};

/*
 * cgroup2_parse_param() - 把一个 v2 挂载 flag 累积到 fs context。
 *
 * fc/param 是 VFS fsconfig 调用借用对象，ctx 由 fc 私有数据持有；函数不
 * 接管字符串。fs_parse() 负责名称/类型校验，成功后只在 ctx->flags 置位，
 * 允许多参数累积。返回 0、解析 errno 或未知枚举 -EINVAL；不挂载 root，
 * 因此失败不需要撤销此前已接受的独立 flag。
 */
static int cgroup2_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct cgroup_fs_context *ctx = cgroup_fc2context(fc);
	struct fs_parse_result result;
	int opt;

	opt = fs_parse(fc, cgroup2_fs_parameters, param, &result);
	if (opt < 0)
		return opt;

	/*
	 * 每个枚举只映射到一个 root flag，是机械策略表；成功后立即
	 * 返回，未知值不能落入已接受状态。
	 */
	switch (opt) {
	case Opt_nsdelegate:
		ctx->flags |= CGRP_ROOT_NS_DELEGATE;
		return 0;
	case Opt_favordynmods:
		ctx->flags |= CGRP_ROOT_FAVOR_DYNMODS;
		return 0;
	case Opt_memory_localevents:
		ctx->flags |= CGRP_ROOT_MEMORY_LOCAL_EVENTS;
		return 0;
	case Opt_memory_recursiveprot:
		ctx->flags |= CGRP_ROOT_MEMORY_RECURSIVE_PROT;
		return 0;
	case Opt_memory_hugetlb_accounting:
		ctx->flags |= CGRP_ROOT_MEMORY_HUGETLB_ACCOUNTING;
		return 0;
	case Opt_pids_localevents:
		ctx->flags |= CGRP_ROOT_PIDS_LOCAL_EVENTS;
		return 0;
	}
	return -EINVAL;
}

/*
 * of_peak() - 取得当前打开文件私有上下文中的峰值统计槽。
 *
 * of->priv 在 cgroup_file_open() 成功后指向 cgroup_file_ctx；返回值是
 * peak 字段的借用指针，只在 open_file 生命周期内有效。无引用变化。
 */
struct cgroup_of_peak *of_peak(struct kernfs_open_file *of)
{
	struct cgroup_file_ctx *ctx = of->priv;

	return &ctx->peak;
}

/*
 * apply_cgroup_root_flags() - 把 v2 挂载选项提交到默认 root。
 *
 * root_flags 是 fs_context 累积的目标状态。只有初始 cgroup namespace
 * 有权改变全局 root；其他 namespace 调用无副作用。函数逐项替换可变位，
 * favordynmods 通过专用 helper 更新 rwsem/RCU 协议。函数可能睡眠，
 * 无错误返回。
 */
static void apply_cgroup_root_flags(unsigned int root_flags)
{
	if (current->nsproxy->cgroup_ns == &init_cgroup_ns) {
		if (root_flags & CGRP_ROOT_NS_DELEGATE)
			cgrp_dfl_root.flags |= CGRP_ROOT_NS_DELEGATE;
		else
			cgrp_dfl_root.flags &= ~CGRP_ROOT_NS_DELEGATE;

		cgroup_favor_dynmods(&cgrp_dfl_root,
				     root_flags & CGRP_ROOT_FAVOR_DYNMODS);

		/*
		 * 其余选项是彼此独立的 ABI 策略位，remount 目标值既可设置
		 * 也可清除，因此逐项覆盖而不是只做 OR 累积。
		 */
		if (root_flags & CGRP_ROOT_MEMORY_LOCAL_EVENTS)
			cgrp_dfl_root.flags |= CGRP_ROOT_MEMORY_LOCAL_EVENTS;
		else
			cgrp_dfl_root.flags &= ~CGRP_ROOT_MEMORY_LOCAL_EVENTS;

		if (root_flags & CGRP_ROOT_MEMORY_RECURSIVE_PROT)
			cgrp_dfl_root.flags |= CGRP_ROOT_MEMORY_RECURSIVE_PROT;
		else
			cgrp_dfl_root.flags &= ~CGRP_ROOT_MEMORY_RECURSIVE_PROT;

		if (root_flags & CGRP_ROOT_MEMORY_HUGETLB_ACCOUNTING)
			cgrp_dfl_root.flags |= CGRP_ROOT_MEMORY_HUGETLB_ACCOUNTING;
		else
			cgrp_dfl_root.flags &= ~CGRP_ROOT_MEMORY_HUGETLB_ACCOUNTING;

		if (root_flags & CGRP_ROOT_PIDS_LOCAL_EVENTS)
			cgrp_dfl_root.flags |= CGRP_ROOT_PIDS_LOCAL_EVENTS;
		else
			cgrp_dfl_root.flags &= ~CGRP_ROOT_PIDS_LOCAL_EVENTS;
	}
}

/*
 * cgroup_show_options() - 把当前 v2 root flags 序列化为挂载选项。
 *
 * seq 是输出对象；kf_root 仅满足 VFS 回调签名，v2 只有全局默认 root。
 * 每个已启用位追加带前导逗号的名称。返回 0；seq_file 自行记录错误，
 * 函数不修改 root。
 */
static int cgroup_show_options(struct seq_file *seq, struct kernfs_root *kf_root)
{
	if (cgrp_dfl_root.flags & CGRP_ROOT_NS_DELEGATE)
		seq_puts(seq, ",nsdelegate");
	if (cgrp_dfl_root.flags & CGRP_ROOT_FAVOR_DYNMODS)
		seq_puts(seq, ",favordynmods");
	if (cgrp_dfl_root.flags & CGRP_ROOT_MEMORY_LOCAL_EVENTS)
		seq_puts(seq, ",memory_localevents");
	if (cgrp_dfl_root.flags & CGRP_ROOT_MEMORY_RECURSIVE_PROT)
		seq_puts(seq, ",memory_recursiveprot");
	if (cgrp_dfl_root.flags & CGRP_ROOT_MEMORY_HUGETLB_ACCOUNTING)
		seq_puts(seq, ",memory_hugetlb_accounting");
	if (cgrp_dfl_root.flags & CGRP_ROOT_PIDS_LOCAL_EVENTS)
		seq_puts(seq, ",pids_localevents");
	return 0;
}

/*
 * cgroup_reconfigure() - 提交 remount/fsconfig 收集的 v2 root flags。
 *
 * fc 借用其 cgroup_fs_context；函数不创建 superblock。当前提交接口无可
 * 传播失败，返回 0；非初始 namespace 请求不会改变全局状态。
 */
static int cgroup_reconfigure(struct fs_context *fc)
{
	struct cgroup_fs_context *ctx = cgroup_fc2context(fc);

	apply_cgroup_root_flags(ctx->flags);
	return 0;
}

/*
 * init_cgroup_housekeeping() - 初始化新 cgroup 的 core 固有状态。
 *
 * cgrp 是尚未发布、已清零的输出对象。函数初始化 self/children/cset/pid
 * 链、mutex、offline waitqueue 和 release work，令 self 初始 online、
 * dom_cgrp 指向自身，并设置无限 descendants/depth 默认值。BPF revision
 * 从 1 开始以区分零初值。无分配、无失败、无 ownership 转移。
 */
static void init_cgroup_housekeeping(struct cgroup *cgrp)
{
	struct cgroup_subsys *ss;
	int ssid;

	INIT_LIST_HEAD(&cgrp->self.sibling);
	INIT_LIST_HEAD(&cgrp->self.children);
	INIT_LIST_HEAD(&cgrp->cset_links);
	INIT_LIST_HEAD(&cgrp->pidlists);
	mutex_init(&cgrp->pidlist_mutex);
	cgrp->self.cgroup = cgrp;
	cgrp->self.flags |= CSS_ONLINE;
	cgrp->dom_cgrp = cgrp;
	cgrp->max_descendants = INT_MAX;
	cgrp->max_depth = INT_MAX;
	prev_cputime_init(&cgrp->prev_cputime);

	for_each_subsys(ss, ssid)
		INIT_LIST_HEAD(&cgrp->e_csets[ssid]);

#ifdef CONFIG_CGROUP_BPF
	for (int i = 0; i < ARRAY_SIZE(cgrp->bpf.revisions); i++)
		cgrp->bpf.revisions[i] = 1;
#endif

	init_waitqueue_head(&cgrp->offline_waitq);
	INIT_WORK(&cgrp->release_agent_work, cgroup1_release_agent);
}

/*
 * init_cgroup_root() - 初始化 fs_context 已分配的 hierarchy root。
 *
 * ctx->root 由挂载构造路径拥有且尚未发布。函数建立 root/self 关系、
 * core housekeeping 和用户选项副本；DYNMODS 位留给后续专用 helper，
 * 避免只改 flag 未切换同步协议。无失败返回。
 */
void init_cgroup_root(struct cgroup_fs_context *ctx)
{
	struct cgroup_root *root = ctx->root;
	struct cgroup *cgrp = &root->cgrp;

	INIT_LIST_HEAD_RCU(&root->root_list);
	atomic_set(&root->nr_cgrps, 1);
	cgrp->root = root;
	init_cgroup_housekeeping(cgrp);

	/* DYNMODS must be modified through cgroup_favor_dynmods() */
	/* 该位带 rcu_sync/rwsem 副作用，不能像纯策略位直接复制。 */
	root->flags = ctx->flags & ~CGRP_ROOT_FAVOR_DYNMODS;
	if (ctx->release_agent)
		strscpy(root->release_agent_path, ctx->release_agent, PATH_MAX);
	if (ctx->name)
		strscpy(root->name, ctx->name, MAX_CGROUP_ROOT_NAMELEN);
	if (ctx->cpuset_clone_children)
		set_bit(CGRP_CPUSET_CLONE_CHILDREN, &root->cgrp.flags);
}

/*
 * cgroup_setup_root() - 构造并发布一棵新 hierarchy 的 root。
 *
 * 调用者持 cgroup_mutex；root 已完成基础初始化，ss_mask 是待绑定控制器。
 * 函数依次初始化 root css 引用、预分配 cset links、分配 ID、创建未激活
 * kernfs、发布 core 文件/rstat并 rebind 控制器。root_list 插入是不可
 * 回滚的全局发布点，此后只补齐 cset 链接。
 *
 * 成功返回 0。发布前失败按逆序撤销 rstat、kernfs、IDR、percpu_ref 和
 * 临时 links，返回 errno；函数可能睡眠。
 */
int cgroup_setup_root(struct cgroup_root *root, u32 ss_mask)
{
	LIST_HEAD(tmp_links);
	struct cgroup *root_cgrp = &root->cgrp;
	struct kernfs_syscall_ops *kf_sops;
	struct css_set *cset;
	int i, ret;

	lockdep_assert_held(&cgroup_mutex);

	ret = percpu_ref_init(&root_cgrp->self.refcnt, css_release,
			      0, GFP_KERNEL);
	if (ret)
		goto out;

	/*
	 * We're accessing css_set_count without locking css_set_lock here,
	 * but that's OK - it can only be increased by someone holding
	 * cgroup_lock, and that's us.  Later rebinding may disable
	 * controllers on the default hierarchy and thus create new csets,
	 * which can't be more than the existing ones.  Allocate 2x.
	 */
	/*
	 * 此处虽未持 css_set_lock，但持 cgroup_mutex，计数只能增加；后续重绑
	 * 最多按现有组合派生新组合，按两倍数量预分配可覆盖提交需求。
	 */
	ret = allocate_cgrp_cset_links(2 * css_set_count, &tmp_links);
	if (ret)
		goto cancel_ref;

	ret = cgroup_init_root_id(root);
	if (ret)
		goto cancel_ref;

	/*
	 * 阶段 2：建立尚未激活的 kernfs root。v1/v2 选择不同 syscall
	 * operations，但都在全部文件和 controller 成功前对用户不可见。
	 */
	kf_sops = root == &cgrp_dfl_root ?
		&cgroup_kf_syscall_ops : &cgroup1_kf_syscall_ops;

	root->kf_root = kernfs_create_root(kf_sops,
					   KERNFS_ROOT_CREATE_DEACTIVATED |
					   KERNFS_ROOT_SUPPORT_EXPORTOP |
					   KERNFS_ROOT_SUPPORT_USER_XATTR |
					   KERNFS_ROOT_INVARIANT_PARENT,
					   root_cgrp);
	if (IS_ERR(root->kf_root)) {
		ret = PTR_ERR(root->kf_root);
		goto exit_root_id;
	}
	root_cgrp->kn = kernfs_root_to_node(root->kf_root);
	WARN_ON_ONCE(cgroup_ino(root_cgrp) != 1);
	root_cgrp->ancestors[0] = root_cgrp;

	/*
	 * 阶段 3：在未发布 root 上创建 core 文件、统计和 controller
	 * 绑定；任一步失败仍可沿标签逆序撤销。
	 */
	ret = css_populate_dir(&root_cgrp->self);
	if (ret)
		goto destroy_root;

	ret = css_rstat_init(&root_cgrp->self);
	if (ret)
		goto destroy_root;

	ret = rebind_subsystems(root, ss_mask);
	if (ret)
		goto exit_stats;

	ret = blocking_notifier_call_chain(&cgroup_lifetime_notifier,
					   CGROUP_LIFETIME_ONLINE, root_cgrp);
	WARN_ON_ONCE(notifier_to_errno(ret));

	trace_cgroup_setup_root(root);

	/*
	 * There must be no failure case after here, since rebinding takes
	 * care of subsystems' refcounts, which are explicitly dropped in
	 * the failure exit path.
	 */
	/*
	 * 从此处开始重绑定会接管 subsystem 引用；后续不得再返回失败，
	 * 否则 failure exit 的显式 put 会与新 ownership 冲突。
	 */
	list_add_rcu(&root->root_list, &cgroup_roots);
	cgroup_root_count++;

	/*
	 * Link the root cgroup in this hierarchy into all the css_set
	 * objects.
	 */
	/* 把新 hierarchy root 链入全部 css_set，使每个组合覆盖新增 root。 */
	spin_lock_irq(&css_set_lock);
	hash_for_each(css_set_table, i, cset, hlist) {
		link_css_set(&tmp_links, cset, root_cgrp);
		if (css_set_populated(cset))
			css_update_populated(&root_cgrp->self, true);
	}
	spin_unlock_irq(&css_set_lock);

	BUG_ON(!list_empty(&root_cgrp->self.children));
	BUG_ON(atomic_read(&root->nr_cgrps) != 1);

	ret = 0;
	goto out;

exit_stats:
	/* rebind 失败时 rstat 已初始化，后续 kernfs/ID/ref 仍待释放。 */
	css_rstat_exit(&root_cgrp->self);
destroy_root:
	/* kernfs root 尚未激活，无并发用户，可直接销毁。 */
	kernfs_destroy_root(root->kf_root);
	root->kf_root = NULL;
exit_root_id:
	cgroup_exit_root_id(root);
cancel_ref:
	/* 取消 self percpu_ref；out 再释放未消费的临时 links。 */
	percpu_ref_exit(&root_cgrp->self.refcnt);
out:
	free_cgrp_cset_links(&tmp_links);
	return ret;
}

/*
 * cgroup_do_get_tree() - 建立/复用 kernfs superblock 并裁剪 namespace 视图。
 *
 * ctx->root 已持 live 引用。函数设置 v1/v2 magic 后调用 kernfs_get_tree()。
 * 非初始 namespace 将物理 root dentry 替换为 root_cset 对应目录，锁内
 * 稳定 cset 映射，dentry 引用接管存活。成功返回 0；失败返回 kernfs/VFS
 * errno并清理临时 superblock。复用 superblock 时归还多取的 root 引用。
 */
int cgroup_do_get_tree(struct fs_context *fc)
{
	struct cgroup_fs_context *ctx = cgroup_fc2context(fc);
	int ret;

	ctx->kfc.root = ctx->root->kf_root;
	if (fc->fs_type == &cgroup2_fs_type)
		ctx->kfc.magic = CGROUP2_SUPER_MAGIC;
	else
		ctx->kfc.magic = CGROUP_SUPER_MAGIC;
	ret = kernfs_get_tree(fc);

	/*
	 * In non-init cgroup namespace, instead of root cgroup's dentry,
	 * we return the dentry corresponding to the cgroupns->root_cgrp.
	 */
	/* 非 init namespace 以 root_cgrp dentry 为视图根，隐藏其上级路径。 */
	if (!ret && ctx->ns != &init_cgroup_ns) {
		struct dentry *nsdentry;
		struct super_block *sb = fc->root->d_sb;
		struct cgroup *cgrp;

		cgroup_lock();
		spin_lock_irq(&css_set_lock);

		cgrp = cset_cgroup_from_root(ctx->ns->root_cset, ctx->root);

		spin_unlock_irq(&css_set_lock);
		cgroup_unlock();

		nsdentry = kernfs_node_dentry(cgrp->kn, sb);
		dput(fc->root);
		if (IS_ERR(nsdentry)) {
			deactivate_locked_super(sb);
			ret = PTR_ERR(nsdentry);
			nsdentry = NULL;
		}
		fc->root = nsdentry;
	}

	if (!ctx->kfc.new_sb_created)
		cgroup_put(&ctx->root->cgrp);

	return ret;
}

/*
 * Destroy a cgroup filesystem context.
 */
/*
 * 中文契约：释放 context 拥有的 name/release_agent、cgroup namespace
 * 引用、kernfs 子上下文和 ctx 本体。fc 为借用，返回：无。
 */
static void cgroup_fs_context_free(struct fs_context *fc)
{
	struct cgroup_fs_context *ctx = cgroup_fc2context(fc);

	kfree(ctx->name);
	kfree(ctx->release_agent);
	put_cgroup_ns(ctx->ns);
	kernfs_free_fs_context(fc);
	kfree(ctx);
}

/*
 * cgroup_get_tree() - v2 get_tree 包装器。
 *
 * 单向发布默认层级可见，取得 root live 引用交给共同 get_tree；成功后
 * 提交挂载 flags。函数可能睡眠，返回共同路径 errno。
 */
static int cgroup_get_tree(struct fs_context *fc)
{
	struct cgroup_fs_context *ctx = cgroup_fc2context(fc);
	int ret;

	WRITE_ONCE(cgrp_dfl_visible, true);
	cgroup_get_live(&cgrp_dfl_root.cgrp);
	ctx->root = &cgrp_dfl_root;

	ret = cgroup_do_get_tree(fc);
	if (!ret)
		apply_cgroup_root_flags(ctx->flags);
	return ret;
}

static const struct fs_context_operations cgroup_fs_context_ops = {
	.free		= cgroup_fs_context_free,
	.parse_param	= cgroup2_parse_param,
	.get_tree	= cgroup_get_tree,
	.reconfigure	= cgroup_reconfigure,
};

static const struct fs_context_operations cgroup1_fs_context_ops = {
	.free		= cgroup_fs_context_free,
	.parse_param	= cgroup1_parse_param,
	.get_tree	= cgroup1_get_tree,
	.reconfigure	= cgroup1_reconfigure,
};

/*
 * Initialise the cgroup filesystem creation/reconfiguration context.  Notably,
 * we select the namespace we're going to use.
 */
/*
 * 中文契约：
 * fc 是新 VFS context。函数可睡眠分配私有 ctx，取得 current cgroup_ns
 * 及其 user_ns 引用，安装 v1/v2 operations 并标记 global。成功后 free
 * 回调接管资源；-ENOMEM 时 fc 尚未取得私有状态。
 */
static int cgroup_init_fs_context(struct fs_context *fc)
{
	struct cgroup_fs_context *ctx;

	ctx = kzalloc_obj(struct cgroup_fs_context);
	if (!ctx)
		return -ENOMEM;

	ctx->ns = current->nsproxy->cgroup_ns;
	get_cgroup_ns(ctx->ns);
	fc->fs_private = &ctx->kfc;
	if (fc->fs_type == &cgroup2_fs_type)
		fc->ops = &cgroup_fs_context_ops;
	else
		fc->ops = &cgroup1_fs_context_ops;
	put_user_ns(fc->user_ns);
	fc->user_ns = get_user_ns(ctx->ns->user_ns);
	fc->global = true;

	if (have_favordynmods)
		ctx->flags |= CGRP_ROOT_FAVOR_DYNMODS;

	return 0;
}

/*
 * cgroup_kill_sb() - 卸载 superblock 并按条件启动 legacy root 销毁。
 *
 * 无 child、非默认根且 percpu_ref 仍 live 时执行 kill，阻止新挂载取得
 * 引用；随后归还 superblock root 引用并交给 kernfs 卸载。默认 v2 root
 * 永不 kill。函数可能睡眠，无直接返回值。
 */
static void cgroup_kill_sb(struct super_block *sb)
{
	struct kernfs_root *kf_root = kernfs_root_from_sb(sb);
	struct cgroup_root *root = cgroup_root_from_kf(kf_root);

	/*
	 * If @root doesn't have any children, start killing it.
	 * This prevents new mounts by disabling percpu_ref_tryget_live().
	 *
	 * And don't kill the default root.
	 */
	/*
	 * 空 legacy root 在卸载时 kill self ref，阻止新 mount 取得 live 引用；
	 * default root 永久存在，不能进入该销毁路径。
	 */
	if (list_empty(&root->cgrp.self.children) && root != &cgrp_dfl_root &&
	    !percpu_ref_is_dying(&root->cgrp.self.refcnt))
		percpu_ref_kill(&root->cgrp.self.refcnt);
	cgroup_put(&root->cgrp);
	kernfs_kill_sb(sb);
}

struct file_system_type cgroup_fs_type = {
	.name			= "cgroup",
	.init_fs_context	= cgroup_init_fs_context,
	.parameters		= cgroup1_fs_parameters,
	.kill_sb		= cgroup_kill_sb,
	.fs_flags		= FS_USERNS_MOUNT,
};

static struct file_system_type cgroup2_fs_type = {
	.name			= "cgroup2",
	.init_fs_context	= cgroup_init_fs_context,
	.parameters		= cgroup2_fs_parameters,
	.kill_sb		= cgroup_kill_sb,
	.fs_flags		= FS_USERNS_MOUNT,
};

#ifdef CONFIG_CPUSETS_V1
enum cpuset_param {
	Opt_cpuset_v2_mode,
};

static const struct fs_parameter_spec cpuset_fs_parameters[] = {
	fsparam_flag  ("cpuset_v2_mode", Opt_cpuset_v2_mode),
	{}
};

/*
 * cpuset_parse_param() - 解析 legacy cpuset mount 的专用兼容参数。
 *
 * fc/param 均由 fs_context 框架借用；函数可写 ctx flags，不取得引用。
 * 成功识别 cpuset_v2_mode 返回 0；通用解析错误原样返回，未知枚举返回
 * -EINVAL。无部分资源需要回滚。
 */
static int cpuset_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct cgroup_fs_context *ctx = cgroup_fc2context(fc);
	struct fs_parse_result result;
	int opt;

	/*
	 * 独立 "cpuset" 文件系统只保留一个兼容参数。fs_parse() 负责类型与
	 * 名称校验；识别后把语义折叠为通用 cgroup root flag，后续仍复用
	 * cgroup1 的建树路径。
	 */
	opt = fs_parse(fc, cpuset_fs_parameters, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_cpuset_v2_mode:
		ctx->flags |= CGRP_ROOT_CPUSET_V2_MODE;
		return 0;
	}
	return -EINVAL;
}

static const struct fs_context_operations cpuset_fs_context_ops = {
	.get_tree	= cgroup1_get_tree,
	.free		= cgroup_fs_context_free,
	.parse_param	= cpuset_parse_param,
};

/*
 * This is ugly, but preserves the userspace API for existing cpuset
 * users. If someone tries to mount the "cpuset" filesystem, we
 * silently switch it to mount "cgroup" instead
 */
/*
 * 中文契约：
 * fc 是 VFS 新建且由调用者拥有的 mount context。函数可睡眠分配默认
 * release_agent，复用 cgroup1 初始化后把 fs_type 引用从 cpuset 转交
 * cgroup。成功返回 0，ctx 接管 agent；失败返回 errno，并释放本函数已
 * 分配对象，fc 仍由 VFS 清理。
 */
static int cpuset_init_fs_context(struct fs_context *fc)
{
	char *agent = kstrdup("/sbin/cpuset_release_agent", GFP_USER);
	struct cgroup_fs_context *ctx;
	int err;

	/*
	 * 先建立普通 cgroup fs_context，再强制选择 cpuset controller、
	 * noprefix 和历史 release_agent 默认值。agent 在初始化失败时由
	 * 本函数释放，成功后所有权转交 ctx，由 context free 路径回收。
	 */
	err = cgroup_init_fs_context(fc);
	if (err) {
		kfree(agent);
		return err;
	}

	fc->ops = &cpuset_fs_context_ops;

	ctx = cgroup_fc2context(fc);
	ctx->subsys_mask = 1 << cpuset_cgrp_id;
	ctx->flags |= CGRP_ROOT_NOPREFIX;
	ctx->release_agent = agent;

	get_filesystem(&cgroup_fs_type);
	put_filesystem(fc->fs_type);
	/*
	 * 用引用计数对称替换 fs_type，使 VFS 表面上的 "cpuset" mount 最终
	 * 进入 cgroup1 实现，同时维持旧用户空间 ABI。
	 */
	fc->fs_type = &cgroup_fs_type;

	return 0;
}

static struct file_system_type cpuset_fs_type = {
	.name			= "cpuset",
	.init_fs_context	= cpuset_init_fs_context,
	.parameters		= cpuset_fs_parameters,
	.fs_flags		= FS_USERNS_MOUNT,
};
#endif

/*
 * cgroup_path_ns_locked() - 在给定 cgroup namespace 中格式化相对路径。
 *
 * cgrp/ns 为借用，buf 是 buflen 字节输出。调用者必须稳定 namespace
 * root_cset 与层级关系，通常持 css_set_lock；函数不睡眠、不取引用。
 * 返回 kernfs 路径长度或 errno，输出 ownership 始终属于调用者。
 */
int cgroup_path_ns_locked(struct cgroup *cgrp, char *buf, size_t buflen,
			  struct cgroup_namespace *ns)
{
	/*
	 * namespace 的 root_cset 为每个 hierarchy 指定可见根。调用者已稳定
	 * root_cset/css_set 关系，本函数只把目标 kn 相对该根格式化；目标在
	 * 可见根之外时 kernfs helper 按其路径规则返回错误。
	 */
	struct cgroup *root = cset_cgroup_from_root(ns->root_cset, cgrp->root);

	return kernfs_path_from_node(cgrp->kn, root->kn, buf, buflen);
}

/*
 * cgroup_path_ns() - 带完整锁协议的 namespace 相对路径公共接口。
 *
 * 参数与返回语义同 locked 版本；函数按 cgroup_mutex -> css_set_lock
 * 顺序稳定拓扑和 root_cset，可睡眠取得 mutex。所有锁在返回前释放，
 * 不改变 cgrp/ns 引用。
 */
int cgroup_path_ns(struct cgroup *cgrp, char *buf, size_t buflen,
		   struct cgroup_namespace *ns)
{
	int ret;

	/*
	 * cgroup_mutex 稳定层级结构，css_set_lock 稳定 namespace root_cset
	 * 到各 root 的映射；锁顺序与全文件其余双锁路径一致。返回值原样
	 * 传递 kernfs 的长度或 errno。
	 */
	cgroup_lock();
	spin_lock_irq(&css_set_lock);

	ret = cgroup_path_ns_locked(cgrp, buf, buflen, ns);

	spin_unlock_irq(&css_set_lock);
	cgroup_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(cgroup_path_ns);

/**
 * cgroup_attach_lock - Lock for ->attach()
 * @lock_mode: whether acquire and acquire which rwsem
 * @tsk: thread group to lock
 *
 * cgroup migration sometimes needs to stabilize threadgroups against forks and
 * exits by write-locking cgroup_threadgroup_rwsem. However, some ->attach()
 * implementations (e.g. cpuset), also need to disable CPU hotplug.
 * Unfortunately, letting ->attach() operations acquire cpus_read_lock() can
 * lead to deadlocks.
 *
 * Bringing up a CPU may involve creating and destroying tasks which requires
 * read-locking threadgroup_rwsem, so threadgroup_rwsem nests inside
 * cpus_read_lock(). If we call an ->attach() which acquires the cpus lock while
 * write-locking threadgroup_rwsem, the locking order is reversed and we end up
 * waiting for an on-going CPU hotplug operation which in turn is waiting for
 * the threadgroup_rwsem to be released to create new tasks. For more details:
 *
 *   http://lkml.kernel.org/r/20220711174629.uehfmqegcwn2lqzu@wubuntu
 *
 * Resolve the situation by always acquiring cpus_read_lock() before optionally
 * write-locking cgroup_threadgroup_rwsem. This allows ->attach() to assume that
 * CPU hotplug is disabled on entry.
 *
 * When favordynmods is enabled, take per threadgroup rwsem to reduce overhead
 * on dynamic cgroup modifications. see the comment above
 * CGRP_ROOT_FAVOR_DYNMODS definition.
 *
 * tsk is not NULL only when writing to cgroup.procs.
 */
/*
 * 中文契约与锁序：
 * lock_mode 选择不取 rwsem、全局 per-CPU 写锁或 tsk 线程组写锁；tsk 仅
 * PER_THREADGROUP 模式非空。函数总先取得 cpus_read_lock()，再取可选
 * threadgroup 锁，固定 CPU hotplug -> threadgroup 顺序，避免 cpuset
 * attach 回调反向获取 CPU 锁导致死锁。可能睡眠，返回：无；必须配对
 * cgroup_attach_unlock()。
 */
void cgroup_attach_lock(enum cgroup_attach_lock_mode lock_mode,
			struct task_struct *tsk)
{
	cpus_read_lock();

	switch (lock_mode) {
	case CGRP_ATTACH_LOCK_NONE:
		break;
	case CGRP_ATTACH_LOCK_GLOBAL:
		percpu_down_write(&cgroup_threadgroup_rwsem);
		break;
	case CGRP_ATTACH_LOCK_PER_THREADGROUP:
		down_write(&tsk->signal->cgroup_threadgroup_rwsem);
		break;
	default:
		pr_warn("cgroup: Unexpected attach lock mode.");
		break;
	}
}

/**
 * cgroup_attach_unlock - Undo cgroup_attach_lock()
 * @lock_mode: whether release and release which rwsem
 * @tsk: thread group to lock
 */
/*
 * 中文契约：参数必须与成功的 cgroup_attach_lock() 完全一致。函数按逆序
 * 先释放可选 threadgroup rwsem，再释放 CPU hotplug 读锁。无返回值；
 * 调用后 fork/exit/hotplug 可重新并发。
 */
void cgroup_attach_unlock(enum cgroup_attach_lock_mode lock_mode,
			  struct task_struct *tsk)
{
	switch (lock_mode) {
	case CGRP_ATTACH_LOCK_NONE:
		break;
	case CGRP_ATTACH_LOCK_GLOBAL:
		percpu_up_write(&cgroup_threadgroup_rwsem);
		break;
	case CGRP_ATTACH_LOCK_PER_THREADGROUP:
		up_write(&tsk->signal->cgroup_threadgroup_rwsem);
		break;
	default:
		pr_warn("cgroup: Unexpected attach lock mode.");
		break;
	}

	cpus_read_unlock();
}

/**
 * cgroup_migrate_add_task - add a migration target task to a migration context
 * @task: target task
 * @mgctx: target migration context
 *
 * Add @task, which is a migration target, to @mgctx->tset.  This function
 * becomes noop if @task doesn't need to be migrated.  @task's css_set
 * should have been added as a migration source and @task->cg_list will be
 * moved from the css_set's tasks list to mg_tasks one.
 */
/*
 * 中文契约：
 * 调用者持 css_set_lock，外层 threadgroup 锁阻止相关 fork/exit。task 和
 * mgctx 为借用；退出 task 或其 cset 未预装载为 source 时幂等跳过。
 * 命中时把 task 从正式 tasks 链移到 mg_tasks 暂存链，推进迭代器，并把
 * source/destination cset 各加入 taskset 一次。无引用转移、无失败返回。
 */
static void cgroup_migrate_add_task(struct task_struct *task,
				    struct cgroup_mgctx *mgctx)
{
	struct css_set *cset;

	lockdep_assert_held(&css_set_lock);

	/* @task either already exited or can't exit until the end */
	/*
	 * task 已经退出，或因当前持有的 task 引用与同步范围不能在
	 * 本函数结束前消失；因此可稳定读取 signal/threadgroup 状态。
	 */
	if (task->flags & PF_EXITING)
		return;

	/* cgroup_threadgroup_rwsem protects racing against forks */
	/*
	 * threadgroup rwsem 与 fork 的成员发布配对，保证整组扫描不会漏掉在
	 * 遍历过程中刚创建的线程。
	 */
	WARN_ON_ONCE(list_empty(&task->cg_list));

	cset = task_css_set(task);
	if (!cset->mg_src_cgrp)
		return;

	mgctx->tset.nr_tasks++;

	css_set_skip_task_iters(cset, task);
	list_move_tail(&task->cg_list, &cset->mg_tasks);
	if (list_empty(&cset->mg_node))
		list_add_tail(&cset->mg_node,
			      &mgctx->tset.src_csets);
	if (list_empty(&cset->mg_dst_cset->mg_node))
		list_add_tail(&cset->mg_dst_cset->mg_node,
			      &mgctx->tset.dst_csets);
}

/**
 * cgroup_taskset_first - reset taskset and return the first task
 * @tset: taskset of interest
 * @dst_cssp: output variable for the destination css
 *
 * @tset iteration is initialized and the first task is returned.
 */
/*
 * 中文契约：tset 已由 migrate prepare 填充；dst_cssp 是输出借用指针。
 * 函数重置游标并委托 next，返回首 task 裸指针或 NULL。调用者的迁移锁
 * 保证 task/cset 链不变，函数不取 task 引用。
 */
struct task_struct *cgroup_taskset_first(struct cgroup_taskset *tset,
					 struct cgroup_subsys_state **dst_cssp)
{
	tset->cur_cset = list_first_entry(tset->csets, struct css_set, mg_node);
	tset->cur_task = NULL;

	return cgroup_taskset_next(tset, dst_cssp);
}

/**
 * cgroup_taskset_next - iterate to the next task in taskset
 * @tset: taskset of interest
 * @dst_cssp: output variable for the destination css
 *
 * Return the next task in @tset.  Iteration must have been initialized
 * with cgroup_taskset_first().
 */
/*
 * 中文契约：
 * tset 游标已初始化，dst_cssp 输出当前 subsystem 在目标 cset 的 css。
 * commit 前 mg_dst_cset 非空，输出目标 css；commit 后该字段清理，输出
 * task 当前 css。返回下一 task 借用指针，结束返回 NULL，无引用变化。
 */
struct task_struct *cgroup_taskset_next(struct cgroup_taskset *tset,
					struct cgroup_subsys_state **dst_cssp)
{
	struct css_set *cset = tset->cur_cset;
	struct task_struct *task = tset->cur_task;

	while (CGROUP_HAS_SUBSYS_CONFIG && &cset->mg_node != tset->csets) {
		if (!task)
			task = list_first_entry(&cset->mg_tasks,
						struct task_struct, cg_list);
		else
			task = list_next_entry(task, cg_list);

		if (&task->cg_list != &cset->mg_tasks) {
			tset->cur_cset = cset;
			tset->cur_task = task;

			/*
			 * This function may be called both before and
			 * after cgroup_migrate_execute().  The two cases
			 * can be distinguished by looking at whether @cset
			 * has its ->mg_dst_cset set.
			 */
			/*
			 * 中文补充：同一 iterator 同时服务 can_attach（提交前）
			 * 和 attach（提交后），mg_dst_cset 是事务阶段标志。
			 */
			if (cset->mg_dst_cset)
				*dst_cssp = cset->mg_dst_cset->subsys[tset->ssid];
			else
				*dst_cssp = cset->subsys[tset->ssid];

			return task;
		}

		cset = list_next_entry(cset, mg_node);
		task = NULL;
	}

	return NULL;
}

/**
 * cgroup_migrate_execute - migrate a taskset
 * @mgctx: migration context
 *
 * Migrate tasks in @mgctx as setup by migration preparation functions.
 * This function fails iff one of the ->can_attach callbacks fails and
 * guarantees that either all or none of the tasks in @mgctx are migrated.
 * @mgctx is consumed regardless of success.
 */
/*
 * 中文契约与原子性：
 * mgctx 已由 add_src/prepare_dst/add_task 构造，函数消费其 taskset。
 * 第一阶段逐控制器调用 can_attach；任一失败按已成功控制器顺序调用
 * cancel_attach，task 尚未移动，返回该 errno。
 *
 * 全部许可后进入不可失败提交点：css_set_lock 下为目标取引用、更新计数、
 * RCU 切换 task->cgroups、移动成员链并归还源引用；再调用 attach 通知
 * 控制器。最后无论成功失败都把 mg_tasks 归回正式链并重置 taskset。
 * 成功返回 0，保证全体 task 已迁移；不会出现部分迁移。
 */
static int cgroup_migrate_execute(struct cgroup_mgctx *mgctx)
{
	struct cgroup_taskset *tset = &mgctx->tset;
	struct cgroup_subsys *ss;
	struct task_struct *task, *tmp_task;
	struct css_set *cset, *tmp_cset;
	int ssid, failed_ssid, ret;

	/* check that we can legitimately attach to the cgroup */
	/* 阶段 1：纯预检；回调可拒绝，但此时成员关系尚未改变。 */
	if (tset->nr_tasks) {
		do_each_subsys_mask(ss, ssid, mgctx->ss_mask) {
			if (ss->can_attach) {
				tset->ssid = ssid;
				ret = ss->can_attach(tset);
				if (ret) {
					failed_ssid = ssid;
					goto out_cancel_attach;
				}
			}
		} while_each_subsys_mask();
	}

	/*
	 * Now that we're guaranteed success, proceed to move all tasks to
	 * the new cgroup.  There are no failure cases after here, so this
	 * is the commit point.
	 */
	/*
	 * 中文补充：锁内循环只调用保证不失败/不睡眠的 core 状态操作。
	 * get 目标引用早于发布，put 源引用晚于摘链，避免任何 UAF 窗口。
	 */
	spin_lock_irq(&css_set_lock);
	list_for_each_entry(cset, &tset->src_csets, mg_node) {
		list_for_each_entry_safe(task, tmp_task, &cset->mg_tasks, cg_list) {
			struct css_set *from_cset = task_css_set(task);
			struct css_set *to_cset = cset->mg_dst_cset;

			get_css_set(to_cset);
			to_cset->nr_tasks++;
			css_set_move_task(task, from_cset, to_cset, true);
			from_cset->nr_tasks--;
			/*
			 * If the source or destination cgroup is frozen,
			 * the task might require to change its state.
			 */
		/*
		 * 成员关系已经切换；freezer 必须比较源/目标 effective freeze，
		 * 必要时同步 task jobctl/冻结状态，避免归属与调度状态
		 * 不一致。
		 */
			cgroup_freezer_migrate_task(task, from_cset->dfl_cgrp,
						    to_cset->dfl_cgrp);
			put_css_set_locked(from_cset);

		}
	}
	spin_unlock_irq(&css_set_lock);

	/*
	 * Migration is committed, all target tasks are now on dst_csets.
	 * Nothing is sensitive to fork() after this point.  Notify
	 * controllers that migration is complete.
	 */
	/*
	 * 阶段 3：成员关系已提交，attach 只同步控制器状态，不能再拒绝。
	 */
	tset->csets = &tset->dst_csets;

	if (tset->nr_tasks) {
		do_each_subsys_mask(ss, ssid, mgctx->ss_mask) {
			if (ss->attach) {
				tset->ssid = ssid;
				ss->attach(tset);
			}
		} while_each_subsys_mask();
	}

	ret = 0;
	goto out_release_tset;

out_cancel_attach:
	/*
	 * 只撤销 failed_ssid 之前已成功的 can_attach 回调，task 仍在
	 * 原 cset。
	 */
	if (tset->nr_tasks) {
		do_each_subsys_mask(ss, ssid, mgctx->ss_mask) {
			if (ssid == failed_ssid)
				break;
			if (ss->cancel_attach) {
				tset->ssid = ssid;
				ss->cancel_attach(tset);
			}
		} while_each_subsys_mask();
	}
out_release_tset:
	/*
	 * 统一清理：成功时 dst csets、失败时 src csets 最终都作为当前正式
	 * tasks 链；mg_node 摘除后 mgctx 可安全复用。
	 */
	spin_lock_irq(&css_set_lock);
	list_splice_init(&tset->dst_csets, &tset->src_csets);
	list_for_each_entry_safe(cset, tmp_cset, &tset->src_csets, mg_node) {
		list_splice_tail_init(&cset->mg_tasks, &cset->tasks);
		list_del_init(&cset->mg_node);
	}
	spin_unlock_irq(&css_set_lock);

	/*
	 * Re-initialize the cgroup_taskset structure in case it is reused
	 * again in another cgroup_migrate_add_task()/cgroup_migrate_execute()
	 * iteration.
	 */
	/*
	 * taskset 可能在批量迁移下一轮复用；清零数量并恢复 src 游标，防止
	 * 已提交的 dst 视图被误当成下一事务输入。
	 */
	tset->nr_tasks = 0;
	tset->csets    = &tset->src_csets;
	return ret;
}

/**
 * cgroup_migrate_vet_dst - verify whether a cgroup can be migration destination
 * @dst_cgrp: destination cgroup to test
 *
 * On the default hierarchy, except for the mixable, (possible) thread root
 * and threaded cgroups, subtree_control must be zero for migration
 * destination cgroups with tasks so that child cgroups don't compete
 * against tasks.
 */
/*
 * 中文契约：
 * dst_cgrp 在 cgroup_mutex 下稳定。v1 无限制；v2 必须有连续有效 domain。
 * 可成为 thread root 或已 threaded 的节点例外，否则有 subtree_control
 * 时违反 no-internal-process，返回 -EBUSY。拓扑不支持返回
 * -EOPNOTSUPP，合法返回 0；纯校验、无副作用。
 */
int cgroup_migrate_vet_dst(struct cgroup *dst_cgrp)
{
	/* v1 doesn't have any restriction */
	/* v1 没有 v2 no-internal-process 与 threaded domain 目的地约束。 */
	if (!cgroup_on_dfl(dst_cgrp))
		return 0;

	/* verify @dst_cgrp can host resources */
	/* 目的地的 effective domain 必须沿祖先连续，才能承载进程资源。 */
	if (!cgroup_is_valid_domain(dst_cgrp->dom_cgrp))
		return -EOPNOTSUPP;

	/*
	 * If @dst_cgrp is already or can become a thread root or is
	 * threaded, it doesn't matter.
	 */
	/*
	 * thread root/threaded 节点允许线程粒度成员与下级 controller 共存，
	 * 因而不应用普通 domain 的 no-internal-process 检查。
	 */
	if (cgroup_can_be_thread_root(dst_cgrp) || cgroup_is_threaded(dst_cgrp))
		return 0;

	/* apply no-internal-process constraint */
	/* 普通 v2 domain 已下放 controller 时不能再直接接收 task。 */
	if (dst_cgrp->subtree_control)
		return -EBUSY;

	return 0;
}

/**
 * cgroup_migrate_finish - cleanup after attach
 * @mgctx: migration context
 *
 * Undo cgroup_migrate_add_src() and cgroup_migrate_prepare_dst().  See
 * those functions for details.
 */
/*
 * 中文契约：
 * 调用者持 cgroup_mutex，mgctx 已完成或中止迁移。函数在 css_set_lock 下
 * 清空每个预装载 source/destination 的迁移字段与链节点，并消费预装载时
 * 取得的 cset 引用。无错误返回；完成后 mgctx 不再钉住任何 css_set。
 */
void cgroup_migrate_finish(struct cgroup_mgctx *mgctx)
{
	struct css_set *cset, *tmp_cset;

	lockdep_assert_held(&cgroup_mutex);

	spin_lock_irq(&css_set_lock);

	list_for_each_entry_safe(cset, tmp_cset, &mgctx->preloaded_src_csets,
				 mg_src_preload_node) {
		cset->mg_src_cgrp = NULL;
		cset->mg_dst_cgrp = NULL;
		cset->mg_dst_cset = NULL;
		list_del_init(&cset->mg_src_preload_node);
		put_css_set_locked(cset);
	}

	list_for_each_entry_safe(cset, tmp_cset, &mgctx->preloaded_dst_csets,
				 mg_dst_preload_node) {
		cset->mg_src_cgrp = NULL;
		cset->mg_dst_cgrp = NULL;
		cset->mg_dst_cset = NULL;
		list_del_init(&cset->mg_dst_preload_node);
		put_css_set_locked(cset);
	}

	spin_unlock_irq(&css_set_lock);
}

/**
 * cgroup_migrate_add_src - add a migration source css_set
 * @src_cset: the source css_set to add
 * @dst_cgrp: the destination cgroup
 * @mgctx: migration context
 *
 * Tasks belonging to @src_cset are about to be migrated to @dst_cgrp.  Pin
 * @src_cset and add it to @mgctx->src_csets, which should later be cleaned
 * up by cgroup_migrate_finish().
 *
 * This function may be called without holding cgroup_threadgroup_rwsem
 * even if the target is a process.  Threads may be created and destroyed
 * but as long as cgroup_mutex is not dropped, no new css_set can be put
 * into play and the preloaded css_sets are guaranteed to cover all
 * migrations.
 */
/*
 * 中文契约：
 * 调用者持 cgroup_mutex 和 css_set_lock。src_cset/dst_cgrp/mgctx 为借用；
 * dead 或已加入的 source 幂等跳过。否则记录本 root 的源/目标 cgroup，
 * 取得一份 cset 引用并加入 preloaded_src_csets；该引用和字段必须由
 * cgroup_migrate_finish() 清理。无分配、无失败返回。
 */
void cgroup_migrate_add_src(struct css_set *src_cset,
			    struct cgroup *dst_cgrp,
			    struct cgroup_mgctx *mgctx)
{
	struct cgroup *src_cgrp;

	lockdep_assert_held(&cgroup_mutex);
	lockdep_assert_held(&css_set_lock);

	/*
	 * If ->dead, @src_set is associated with one or more dead cgroups
	 * and doesn't contain any migratable tasks.  Ignore it early so
	 * that the rest of migration path doesn't get confused by it.
	 */
	/* dead cset 没有可迁移 task，提前跳过以免被事务误作有效来源。 */
	if (src_cset->dead)
		return;

	if (!list_empty(&src_cset->mg_src_preload_node))
		return;

	src_cgrp = cset_cgroup_from_root(src_cset, dst_cgrp->root);

	WARN_ON(src_cset->mg_src_cgrp);
	WARN_ON(src_cset->mg_dst_cgrp);
	WARN_ON(!list_empty(&src_cset->mg_tasks));
	WARN_ON(!list_empty(&src_cset->mg_node));

	src_cset->mg_src_cgrp = src_cgrp;
	src_cset->mg_dst_cgrp = dst_cgrp;
	get_css_set(src_cset);
	list_add_tail(&src_cset->mg_src_preload_node, &mgctx->preloaded_src_csets);
}

/**
 * cgroup_migrate_prepare_dst - prepare destination css_sets for migration
 * @mgctx: migration context
 *
 * Tasks are about to be moved and all the source css_sets have been
 * preloaded to @mgctx->preloaded_src_csets.  This function looks up and
 * pins all destination css_sets, links each to its source, and append them
 * to @mgctx->preloaded_dst_csets.
 *
 * This function must be called after cgroup_migrate_add_src() has been
 * called on each migration source css_set.  After migration is performed
 * using cgroup_migrate(), cgroup_migrate_finish() must be called on
 * @mgctx.
 */
/*
 * 中文契约：
 * 调用者持 cgroup_mutex，所有 source 已预装载。函数可睡眠调用
 * find_css_set()，为每个 source 取得/创建目标组合并建立 mg_dst_cset。
 * source==destination 是 no-op，会立即撤销该 source 的预装载引用。
 *
 * 成功返回 0，mgctx 持有所有目标引用；-ENOMEM 时已完成项仍保留在
 * mgctx，由强制要求的 cgroup_migrate_finish() 统一回滚。
 */
int cgroup_migrate_prepare_dst(struct cgroup_mgctx *mgctx)
{
	struct css_set *src_cset, *tmp_cset;

	lockdep_assert_held(&cgroup_mutex);

	/* look up the dst cset for each src cset and link it to src */
	/*
	 * 为每个源组合查找目标组合并建立事务期映射；此阶段只准备
	 * 引用，尚未移动 task。
	 */
	list_for_each_entry_safe(src_cset, tmp_cset, &mgctx->preloaded_src_csets,
				 mg_src_preload_node) {
		struct css_set *dst_cset;
		struct cgroup_subsys *ss;
		int ssid;

		dst_cset = find_css_set(src_cset, src_cset->mg_dst_cgrp);
		if (!dst_cset)
			return -ENOMEM;

		WARN_ON_ONCE(src_cset->mg_dst_cset || dst_cset->mg_dst_cset);

		/*
		 * If src cset equals dst, it's noop.  Drop the src.
		 * cgroup_migrate() will skip the cset too.  Note that we
		 * can't handle src == dst as some nodes are used by both.
		 */
	/*
	 * 源目标相同是 no-op，立即撤销预装载；同一侵入式节点不能
	 * 同时充当 src/dst，因此不能留给共同迁移链处理。
	 */
		if (src_cset == dst_cset) {
			src_cset->mg_src_cgrp = NULL;
			src_cset->mg_dst_cgrp = NULL;
			list_del_init(&src_cset->mg_src_preload_node);
			put_css_set(src_cset);
			put_css_set(dst_cset);
			continue;
		}

		src_cset->mg_dst_cset = dst_cset;

		if (list_empty(&dst_cset->mg_dst_preload_node))
			list_add_tail(&dst_cset->mg_dst_preload_node,
				      &mgctx->preloaded_dst_csets);
		else
			put_css_set(dst_cset);

		for_each_subsys(ss, ssid)
			if (src_cset->subsys[ssid] != dst_cset->subsys[ssid])
				mgctx->ss_mask |= 1 << ssid;
	}

	return 0;
}

/**
 * cgroup_migrate - migrate a process or task to a cgroup
 * @leader: the leader of the process or the task to migrate
 * @threadgroup: whether @leader points to the whole process or a single task
 * @mgctx: migration context
 *
 * Migrate a process or task denoted by @leader.  If migrating a process,
 * the caller must be holding cgroup_threadgroup_rwsem.  The caller is also
 * responsible for invoking cgroup_migrate_add_src() and
 * cgroup_migrate_prepare_dst() on the targets before invoking this
 * function and following up with cgroup_migrate_finish().
 *
 * As long as a controller's ->can_attach() doesn't fail, this function is
 * guaranteed to succeed.  This means that, excluding ->can_attach()
 * failure, when migrating multiple targets, the success or failure can be
 * decided for all targets by invoking group_migrate_prepare_dst() before
 * actually starting migrating.
 */
/*
 * 中文契约：
 * leader 是持有引用的 task；threadgroup 决定遍历整个线程组或仅该 task；
 * mgctx 已完成 source/destination 预装载。整组迁移时调用者持对应
 * threadgroup rwsem。css_set_lock（同时提供 RCU 读侧保证）下拍摄并加入
 * task 快照，然后交给原子执行器。
 *
 * 返回 0 或 can_attach errno；函数消费 taskset，但 mgctx 的预装载引用
 * 仍需 cgroup_migrate_finish() 清理。
 */
int cgroup_migrate(struct task_struct *leader, bool threadgroup,
		   struct cgroup_mgctx *mgctx)
{
	struct task_struct *task;

	/*
	 * The following thread iteration should be inside an RCU critical
	 * section to prevent tasks from being freed while taking the snapshot.
	 * spin_lock_irq() implies RCU critical section here.
	 */
	/*
	 * 锁阻止成员链变化并隐含 RCU 临界区，使 while_each_thread 取得的
	 * task 在加入迁移暂存链前不会被释放。
	 */
	spin_lock_irq(&css_set_lock);
	task = leader;
	do {
		cgroup_migrate_add_task(task, mgctx);
		if (!threadgroup)
			break;
	} while_each_thread(leader, task);
	spin_unlock_irq(&css_set_lock);

	return cgroup_migrate_execute(mgctx);
}

/**
 * cgroup_attach_task - attach a task or a whole threadgroup to a cgroup
 * @dst_cgrp: the cgroup to attach to
 * @leader: the task or the leader of the threadgroup to be attached
 * @threadgroup: attach the whole threadgroup?
 *
 * Call holding cgroup_mutex and cgroup_threadgroup_rwsem.
 */
/*
 * 中文契约：
 * 调用者持 cgroup_mutex 及 attach_lock 选择的线程组同步；dst_cgrp、
 * leader 为借用，threadgroup 选择粒度。函数为所有源 cset 预装载目标，
 * 执行全有或全无迁移，并无条件 finish 清理引用。
 *
 * 成功返回 0 并发 trace；-ENOMEM 或 can_attach errno 时 task 保持原
 * cgroup，所有临时 cset 引用已释放。函数可能睡眠。
 */
int cgroup_attach_task(struct cgroup *dst_cgrp, struct task_struct *leader,
		       bool threadgroup)
{
	DEFINE_CGROUP_MGCTX(mgctx);
	struct task_struct *task;
	int ret = 0;

	/* look up all src csets */
	/* 收集目标子树涉及的全部源 cset，并为迁移事务固定引用。 */
	spin_lock_irq(&css_set_lock);
	task = leader;
	do {
		cgroup_migrate_add_src(task_css_set(task), dst_cgrp, &mgctx);
		if (!threadgroup)
			break;
	} while_each_thread(leader, task);
	spin_unlock_irq(&css_set_lock);

	/* prepare dst csets and commit */
	/*
	 * 完成目标组合预分配后进入原子迁移提交；失败仍由 finish
	 * 统一清理。
	 */
	ret = cgroup_migrate_prepare_dst(&mgctx);
	if (!ret)
		ret = cgroup_migrate(leader, threadgroup, &mgctx);

	cgroup_migrate_finish(&mgctx);

	if (!ret)
		TRACE_CGROUP_PATH(attach_task, dst_cgrp, leader, threadgroup);

	return ret;
}

/*
 * cgroup_procs_write_start() - 解析写入 PID，稳定 task 并取得 attach 锁。
 *
 * buf 是可修改的 NUL 字符串；threadgroup 表示 cgroup.procs（否则
 * cgroup.threads）；lock_mode 是输出。PID 0 选择 current，正数在 RCU
 * 下查找并 get_task_struct()。禁止不可迁移/NO_SETAFFINITY kthread。
 *
 * 成功返回持有引用的 task，且 CPU hotplug/所选 threadgroup 锁
 * 仍持有，必须交给 finish；失败返回 ERR_PTR(-EINVAL/-ESRCH)。
 * 与 exec de_thread 竞争失去 leader 时释放全部资源并重试。
 */
struct task_struct *cgroup_procs_write_start(char *buf, bool threadgroup,
					     enum cgroup_attach_lock_mode *lock_mode)
{
	struct task_struct *tsk;
	pid_t pid;

	if (kstrtoint(strstrip(buf), 0, &pid) || pid < 0)
		return ERR_PTR(-EINVAL);

retry_find_task:
	rcu_read_lock();
	if (pid) {
		tsk = find_task_by_vpid(pid);
		if (!tsk) {
			tsk = ERR_PTR(-ESRCH);
			goto out_unlock_rcu;
		}
	} else {
		tsk = current;
	}

	if (threadgroup)
		tsk = tsk->group_leader;

	/*
	 * kthreads may acquire PF_NO_SETAFFINITY during initialization.
	 * If userland migrates such a kthread to a non-root cgroup, it can
	 * become trapped in a cpuset, or RT kthread may be born in a
	 * cgroup with no rt_runtime allocated.  Just say no.
	 */
	/*
	 * 初始化 kthread 若随后禁止 affinity，迁入非 root cpuset/RT cgroup
	 * 可能被困或缺少 runtime，因此直接拒绝。
	 */
	if (tsk->no_cgroup_migration || (tsk->flags & PF_NO_SETAFFINITY)) {
		tsk = ERR_PTR(-EINVAL);
		goto out_unlock_rcu;
	}
	get_task_struct(tsk);
	rcu_read_unlock();

	/*
	 * If we migrate a single thread, we don't care about threadgroup
	 * stability. If the thread is `current`, it won't exit(2) under our
	 * hands or change PID through exec(2). We exclude
	 * cgroup_update_dfl_csses and other cgroup_{proc,thread}s_write callers
	 * by cgroup_mutex. Therefore, we can skip the global lock.
	 */
	/*
	 * 单线程 current 不会并发 exit/exec 改身份，其他 cgroup 写者又被
	 * mutex 排除，故无需稳定整个 threadgroup。
	 */
	lockdep_assert_held(&cgroup_mutex);

	if (pid || threadgroup) {
		if (cgroup_enable_per_threadgroup_rwsem)
			*lock_mode = CGRP_ATTACH_LOCK_PER_THREADGROUP;
		else
			*lock_mode = CGRP_ATTACH_LOCK_GLOBAL;
	} else {
		*lock_mode = CGRP_ATTACH_LOCK_NONE;
	}

	cgroup_attach_lock(*lock_mode, tsk);

	if (threadgroup) {
		if (!thread_group_leader(tsk)) {
			/*
			 * A race with de_thread from another thread's exec()
			 * may strip us of our leadership. If this happens,
			 * throw this task away and try again.
			 */
			/*
			 * leader 身份是在取锁前观察的；exec 可能在窗口内改组。
			 * 锁后重新验证；不满足时连 task 引用一起丢弃再查。
			 */
			cgroup_attach_unlock(*lock_mode, tsk);
			put_task_struct(tsk);
			goto retry_find_task;
		}
	}

	return tsk;

out_unlock_rcu:
	rcu_read_unlock();
	return tsk;
}

/*
 * cgroup_procs_write_finish() - 配对释放 start 返回的锁与 task 引用。
 *
 * task/lock_mode 必须来自成功 start。先解除 attach 锁，让 fork/exit/
 * hotplug 恢复并发，再 put_task_struct()。返回：无，调用后 task 指针
 * 不再由本路径保证有效。
 */
void cgroup_procs_write_finish(struct task_struct *task,
			       enum cgroup_attach_lock_mode lock_mode)
{
	cgroup_attach_unlock(lock_mode, task);

	/* release reference from cgroup_procs_write_start() */
	/* 归还 start 为跨越权限检查与迁移阶段取得的 task_struct 引用。 */
	put_task_struct(task);
}

/*
 * cgroup_print_ss_mask() - 按 ssid 顺序输出空格分隔的控制器名。
 *
 * seq 是输出对象，ss_mask 是值输入。空集合不输出；非空集合末尾追加
 * 换行。返回：无；seq_file 自行保存写错误，不修改控制器状态。
 */
static void cgroup_print_ss_mask(struct seq_file *seq, u32 ss_mask)
{
	struct cgroup_subsys *ss;
	bool printed = false;
	int ssid;

	do_each_subsys_mask(ss, ssid, ss_mask) {
		if (printed)
			seq_putc(seq, ' ');
		seq_puts(seq, ss->name);
		printed = true;
	} while_each_subsys_mask();
	if (printed)
		seq_putc(seq, '\n');
}

/* show controllers which are enabled from the parent */
/* 输出本 cgroup 从 parent 继承且对用户可见的 controller 集合；返回 0。 */
/*
 * seq 为输出对象，v 未使用；当前 css 由 seq_file/kernfs active ref 固定。
 * 函数只读取控制 mask，不加锁、不转移引用，seq_file 自行记录写错误。
 */
static int cgroup_controllers_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;

	cgroup_print_ss_mask(seq, cgroup_control(cgrp));
	return 0;
}

/* show controllers which are enabled for a given cgroup's children */
/* 输出本节点显式下放给 children 的 subtree_control 位图；返回 0。 */
/*
 * seq 为输出对象，v 未使用；调用期间 cgroup 由打开文件固定。函数只输出
 * 当前位图并返回 0，无 ownership 或控制器状态变化。
 */
static int cgroup_subtree_control_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;

	cgroup_print_ss_mask(seq, cgrp->subtree_control);
	return 0;
}

/**
 * cgroup_update_dfl_csses - update css assoc of a subtree in default hierarchy
 * @cgrp: root of the subtree to update csses for
 *
 * @cgrp's control masks have changed and its subtree's css associations
 * need to be updated accordingly.  This function looks up all css_sets
 * which are attached to the subtree, creates the matching updated css_sets
 * and migrates the tasks to the new ones.
 */
/*
 * 中文契约：
 * 调用者持 cgroup_mutex；cgrp 的控制 mask 已改但后代 task 仍引用旧组合。
 * 函数遍历所有 live 后代，把其 cset 预装载为迁移源；若确有 task 才取得
 * 全局 attach 写锁，创建匹配新 effective css 的目标组合，把每个 task
 * 加入事务并原子提交。
 *
 * 成功返回 0，后代 task 全部指向新 css 组合；-ENOMEM/can_attach errno
 * 时成员关系不变。所有出口 finish mgctx 并释放 attach 锁。
 */
static int cgroup_update_dfl_csses(struct cgroup *cgrp)
{
	DEFINE_CGROUP_MGCTX(mgctx);
	struct cgroup_subsys_state *d_css;
	struct cgroup *dsct;
	struct css_set *src_cset;
	enum cgroup_attach_lock_mode lock_mode;
	bool has_tasks;
	int ret;

	lockdep_assert_held(&cgroup_mutex);

	/* look up all csses currently attached to @cgrp's subtree */
	/*
	 * 扫描子树当前关联的所有 css_set，建立控制 mask 改变后的
	 * 迁移源集合。
	 */
	spin_lock_irq(&css_set_lock);
	cgroup_for_each_live_descendant_pre(dsct, d_css, cgrp) {
		struct cgrp_cset_link *link;

		/*
		 * As cgroup_update_dfl_csses() is only called by
		 * cgroup_apply_control(). The csses associated with the
		 * given cgrp will not be affected by changes made to
		 * its subtree_control file. We can skip them.
		 */
		/*
		 * subtree_control 只改变 children 的 css 物化；cgrp 自己使用的
		 * controller 由 parent 决定，因此无需迁移直接挂在 cgrp 的 cset。
		 */
		if (dsct == cgrp)
			continue;

		list_for_each_entry(link, &dsct->cset_links, cset_link)
			cgroup_migrate_add_src(link->cset, dsct, &mgctx);
	}
	spin_unlock_irq(&css_set_lock);

	/*
	 * We need to write-lock threadgroup_rwsem while migrating tasks.
	 * However, if there are no source csets for @cgrp, changing its
	 * controllers isn't gonna produce any task migrations and the
	 * write-locking can be skipped safely.
	 */
	/* 空子树快速路径仍走 prepare/apply，但不承担全局线程组写锁成本。 */
	has_tasks = !list_empty(&mgctx.preloaded_src_csets);

	if (has_tasks)
		lock_mode = CGRP_ATTACH_LOCK_GLOBAL;
	else
		lock_mode = CGRP_ATTACH_LOCK_NONE;

	cgroup_attach_lock(lock_mode, NULL);

	/* NULL dst indicates self on default hierarchy */
	/*
	 * default hierarchy 以 NULL 目标编码“重新解析到当前
	 * self/effective css”。
	 */
	ret = cgroup_migrate_prepare_dst(&mgctx);
	if (ret)
		goto out_finish;

	spin_lock_irq(&css_set_lock);
	list_for_each_entry(src_cset, &mgctx.preloaded_src_csets,
			    mg_src_preload_node) {
		struct task_struct *task, *ntask;

		/* all tasks in src_csets need to be migrated */
	/*
	 * 源 cset 内全部 task 都必须进入事务，避免同一组合残留旧
	 * css 关联。
	 */
		list_for_each_entry_safe(task, ntask, &src_cset->tasks, cg_list)
			cgroup_migrate_add_task(task, &mgctx);
	}
	spin_unlock_irq(&css_set_lock);

	ret = cgroup_migrate_execute(&mgctx);
out_finish:
	cgroup_migrate_finish(&mgctx);
	cgroup_attach_unlock(lock_mode, NULL);
	return ret;
}

/**
 * cgroup_lock_and_drain_offline - lock cgroup_mutex and drain offlined csses
 * @cgrp: root of the target subtree
 *
 * Because css offlining is asynchronous, userland may try to re-enable a
 * controller while the previous css is still around.  This function grabs
 * cgroup_mutex and drains the previous css instances of @cgrp's subtree.
 */
/*
 * 中文契约：
 * cgrp 是待操作子树根的借用对象。函数返回时持有 cgroup_mutex；若发现
 * dying css，则先 get 所在 cgroup、挂入不可中断 waitqueue，释放 mutex
 * 睡眠，醒后 put 并从头重扫。重扫防止等待期间拓扑变化使旧游标失效。
 * 无错误返回，调用者必须最终 cgroup_unlock()。
 */
void cgroup_lock_and_drain_offline(struct cgroup *cgrp)
	__acquires(&cgroup_mutex)
{
	struct cgroup *dsct;
	struct cgroup_subsys_state *d_css;
	struct cgroup_subsys *ss;
	int ssid;

restart:
	cgroup_lock();

	cgroup_for_each_live_descendant_post(dsct, d_css, cgrp) {
		for_each_subsys(ss, ssid) {
			struct cgroup_subsys_state *css = cgroup_css(dsct, ss);
			DEFINE_WAIT(wait);

			if (!css || !css_is_dying(css))
				continue;

			cgroup_get_live(dsct);
			prepare_to_wait(&dsct->offline_waitq, &wait,
					TASK_UNINTERRUPTIBLE);

			/*
			 * 不能持 cgroup_mutex 等待 offline worker，因为 worker
			 * 可能需要同一锁完成状态推进；引用保证睡眠时 dsct
			 * 仍然存活。
			 */
			cgroup_unlock();
			schedule();
			finish_wait(&dsct->offline_waitq, &wait);

			cgroup_put(dsct);
			goto restart;
		}
	}
}

/**
 * cgroup_save_control - save control masks and dom_cgrp of a subtree
 * @cgrp: root of the target subtree
 *
 * Save ->subtree_control, ->subtree_ss_mask and ->dom_cgrp to the
 * respective old_ prefixed fields for @cgrp's subtree including @cgrp
 * itself.
 */
/*
 * 中文契约：调用者持 cgroup_mutex；把整个 live 子树的三个事务字段保存
 * 到 old_*。不取引用、不失败；快照只供紧随其后的 apply/finalize 使用，
 * 不能跨并发控制事务。
 */
static void cgroup_save_control(struct cgroup *cgrp)
{
	struct cgroup *dsct;
	struct cgroup_subsys_state *d_css;

	cgroup_for_each_live_descendant_pre(dsct, d_css, cgrp) {
		dsct->old_subtree_control = dsct->subtree_control;
		dsct->old_subtree_ss_mask = dsct->subtree_ss_mask;
		dsct->old_dom_cgrp = dsct->dom_cgrp;
	}
}

/**
 * cgroup_propagate_control - refresh control masks of a subtree
 * @cgrp: root of the target subtree
 *
 * For @cgrp and its subtree, ensure ->subtree_ss_mask matches
 * ->subtree_control and propagate controller availability through the
 * subtree so that descendants don't have unavailable controllers enabled.
 */
/*
 * 中文契约：cgroup_mutex 下自顶向下传播。每个节点先把显式控制位裁剪到
 * parent 实际可见集合，再计算含 implicit/depends_on 的物化 mask。
 * 自顶向下保证 child 总以 parent 已更新结果为输入。返回：无。
 */
static void cgroup_propagate_control(struct cgroup *cgrp)
{
	struct cgroup *dsct;
	struct cgroup_subsys_state *d_css;

	cgroup_for_each_live_descendant_pre(dsct, d_css, cgrp) {
		dsct->subtree_control &= cgroup_control(dsct);
		dsct->subtree_ss_mask =
			cgroup_calc_subtree_ss_mask(dsct->subtree_control,
						    cgroup_ss_mask(dsct));
	}
}

/**
 * cgroup_restore_control - restore control masks and dom_cgrp of a subtree
 * @cgrp: root of the target subtree
 *
 * Restore ->subtree_control, ->subtree_ss_mask and ->dom_cgrp from the
 * respective old_ prefixed fields for @cgrp's subtree including @cgrp
 * itself.
 */
/*
 * 中文契约：失败事务在 cgroup_mutex 下自底向上恢复 old_* mask 和
 * dom_cgrp。后序遍历避免 child 在 parent 恢复前观察混合拓扑；不恢复
 * 已分配对象，后续 disable 阶段负责物理收敛。
 */
static void cgroup_restore_control(struct cgroup *cgrp)
{
	struct cgroup *dsct;
	struct cgroup_subsys_state *d_css;

	cgroup_for_each_live_descendant_post(dsct, d_css, cgrp) {
		dsct->subtree_control = dsct->old_subtree_control;
		dsct->subtree_ss_mask = dsct->old_subtree_ss_mask;
		dsct->dom_cgrp = dsct->old_dom_cgrp;
	}
}

/*
 * css_visible() - 判断物化 css 是否应向用户发布控制文件。
 *
 * 显式可控制时可见；完全不在实际 mask 时不可见；仅因 v2 implicit
 * dependency 存在时保持内部 css 但隐藏。css 为借用，纯查询。
 */
static bool css_visible(struct cgroup_subsys_state *css)
{
	struct cgroup_subsys *ss = css->ss;
	struct cgroup *cgrp = css->cgroup;

	if (cgroup_control(cgrp) & (1 << ss->id))
		return true;
	if (!(cgroup_ss_mask(cgrp) & (1 << ss->id)))
		return false;
	return cgroup_on_dfl(cgrp) && ss->implicit_on_dfl;
}

/**
 * cgroup_apply_control_enable - enable or show csses according to control
 * @cgrp: root of the target subtree
 *
 * Walk @cgrp's subtree and create new csses or make the existing ones
 * visible.  A css is created invisible if it's being implicitly enabled
 * through dependency.  An invisible css is made visible when the userland
 * explicitly enables it.
 *
 * Returns 0 on success, -errno on failure.  On failure, csses which have
 * been processed already aren't cleaned up.  The caller is responsible for
 * cleaning up with cgroup_apply_control_disable().
 */
/*
 * 中文契约：
 * cgroup_mutex 下自顶向下遍历目标子树，为新 mask 缺失的 css 执行
 * css_create()，并仅对 css_visible() 的状态发布文件。隐式依赖 css 可
 * 存在但隐藏。成功返回 0；分配/文件失败返回 errno，已处理 css 不回滚，
 * 调用者必须在 finalize 中执行 disable 收敛。
 */
static int cgroup_apply_control_enable(struct cgroup *cgrp)
{
	struct cgroup *dsct;
	struct cgroup_subsys_state *d_css;
	struct cgroup_subsys *ss;
	int ssid, ret;

	cgroup_for_each_live_descendant_pre(dsct, d_css, cgrp) {
		for_each_subsys(ss, ssid) {
			struct cgroup_subsys_state *css = cgroup_css(dsct, ss);

			if (!(cgroup_ss_mask(dsct) & (1 << ss->id)))
				continue;

			if (!css) {
				css = css_create(dsct, ss);
				if (IS_ERR(css))
					return PTR_ERR(css);
			}

			WARN_ON_ONCE(percpu_ref_is_dying(&css->refcnt));

			if (css_visible(css)) {
				ret = css_populate_dir(css);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

/**
 * cgroup_apply_control_disable - kill or hide csses according to control
 * @cgrp: root of the target subtree
 *
 * Walk @cgrp's subtree and kill and hide csses so that they match
 * cgroup_ss_mask() and cgroup_visible_mask().
 *
 * A css is hidden when the userland requests it to be disabled while other
 * subsystems are still depending on it.  The css must not actively control
 * resources and be in the vanilla state if it's made visible again later.
 * Controllers which may be depended upon should provide ->css_reset() for
 * this purpose.
 */
/*
 * 中文契约：
 * cgroup_mutex 下自底向上处理。已不在实际 mask 且非 root 的 css 先
 * kill；若未 populated 可立即 finish，否则等待 populated 路径延迟触发。
 * 仍被依赖但不再显式可见的 css 只清文件并调用 css_reset 回到中性状态。
 * 返回：无，异步 offline 可能在函数后继续。
 */
static void cgroup_apply_control_disable(struct cgroup *cgrp)
{
	struct cgroup *dsct;
	struct cgroup_subsys_state *d_css;
	struct cgroup_subsys *ss;
	int ssid;

	cgroup_for_each_live_descendant_post(dsct, d_css, cgrp) {
		for_each_subsys(ss, ssid) {
			struct cgroup_subsys_state *css = cgroup_css(dsct, ss);

			if (!css)
				continue;

			WARN_ON_ONCE(percpu_ref_is_dying(&css->refcnt));

			if (css->parent &&
			    !(cgroup_ss_mask(dsct) & (1 << ss->id))) {
				kill_css_sync(css);
				if (!css_is_populated(css))
					kill_css_finish(css);
			} else if (!css_visible(css)) {
				css_clear_dir(css);
				if (ss->css_reset)
					ss->css_reset(css);
			}
		}
	}
}

/**
 * cgroup_apply_control - apply control mask updates to the subtree
 * @cgrp: root of the target subtree
 *
 * subsystems can be enabled and disabled in a subtree using the following
 * steps.
 *
 * 1. Call cgroup_save_control() to stash the current state.
 * 2. Update ->subtree_control masks in the subtree as desired.
 * 3. Call cgroup_apply_control() to apply the changes.
 * 4. Optionally perform other related operations.
 * 5. Call cgroup_finalize_control() to finish up.
 *
 * This function implements step 3 and propagates the mask changes
 * throughout @cgrp's subtree, updates csses accordingly and perform
 * process migrations.
 */
/*
 * 中文契约：
 * 调用者已 save 并修改目标 mask，且持 cgroup_mutex。本函数先传播依赖，
 * 再创建/显示所需 css；成功后 effective css 已反映新状态，才迁移后代
 * task 到新 css_set。返回 0、分配/文件 errno 或 can_attach errno。
 * 失败不自行恢复，必须调用 cgroup_finalize_control(cgrp, ret)。
 */
static int cgroup_apply_control(struct cgroup *cgrp)
{
	int ret;

	cgroup_propagate_control(cgrp);

	ret = cgroup_apply_control_enable(cgrp);
	if (ret)
		return ret;

	/*
	 * At this point, cgroup_e_css_by_mask() results reflect the new csses
	 * making the following cgroup_update_dfl_csses() properly update
	 * css associations of all tasks in the subtree.
	 */
	/*
	 * 此处是顺序关键点：先发布所有新 css，find_css_set 才能为 task
	 * 构造完整目标组合；反序会把 task 迁移到祖先 fallback css。
	 */
	return cgroup_update_dfl_csses(cgrp);
}

/**
 * cgroup_finalize_control - finalize control mask update
 * @cgrp: root of the target subtree
 * @ret: the result of the update
 *
 * Finalize control mask update.  See cgroup_apply_control() for more info.
 */
/*
 * 中文契约：
 * ret==0 提交已传播的 mask；ret!=0 先恢复逻辑快照并重新传播。两种情况
 * 都运行 disable，使额外创建/不再需要的 css 和文件最终匹配提交状态。
 * 无返回值；offline 可能异步完成。
 */
static void cgroup_finalize_control(struct cgroup *cgrp, int ret)
{
	if (ret) {
		cgroup_restore_control(cgrp);
		cgroup_propagate_control(cgrp);
	}

	cgroup_apply_control_disable(cgrp);
}

/*
 * cgroup_vet_subtree_control_enable() - 校验新增控制器是否符合 v2 域规则。
 *
 * cgrp 在 cgroup_mutex 下稳定；enable 是只含新开位的 mask。空集合、层级
 * root 和允许 threaded 的情况快速成功。domain 控制器不能进入 threaded
 * 子树；普通 domain 节点有 task 时受 no-internal-process 约束。
 * 返回 0、-EOPNOTSUPP 或 -EBUSY，无副作用。
 */
static int cgroup_vet_subtree_control_enable(struct cgroup *cgrp, u32 enable)
{
	u32 domain_enable = enable & ~cgrp_dfl_threaded_ss_mask;

	/* if nothing is getting enabled, nothing to worry about */
	/* 仅禁用不会引入新的资源承载约束，可直接通过 enable 预检。 */
	if (!enable)
		return 0;

	/* can @cgrp host any resources? */
	/* 新启用 controller 前先确认该节点位于可承载资源的有效 domain。 */
	if (!cgroup_is_valid_domain(cgrp->dom_cgrp))
		return -EOPNOTSUPP;

	/* mixables don't care */
	/* mixable 节点明确允许 domain/threaded 组合，不应用下面的排斥规则。 */
	if (cgroup_is_mixable(cgrp))
		return 0;

	if (domain_enable) {
		/* can't enable domain controllers inside a thread subtree */
	/* threaded subtree 内只能启用 threaded controller，否则资源域边界断裂。 */
		if (cgroup_is_thread_root(cgrp) || cgroup_is_threaded(cgrp))
			return -EOPNOTSUPP;
	} else {
		/*
		 * Threaded controllers can handle internal competitions
		 * and are always allowed inside a (prospective) thread
		 * subtree.
		 */
	/*
	 * threaded controller 可处理内部竞争，在 prospective thread
	 * subtree 中允许。
	 */
		if (cgroup_can_be_thread_root(cgrp) || cgroup_is_threaded(cgrp))
			return 0;
	}

	/*
	 * Controllers can't be enabled for a cgroup with tasks to avoid
	 * child cgroups competing against tasks.
	 */
	/* 普通 domain controller 不允许 task 与 child 同时竞争内部节点资源。 */
	if (cgroup_has_tasks(cgrp))
		return -EBUSY;

	return 0;
}

/* change the enabled child controllers for a cgroup in the default hierarchy */
/*
 * cgroup_subtree_control_write() - 事务性修改 v2 child 控制器。
 *
 * buf/nbytes 是 kernfs 可修改输入，格式为以空格分隔的
 * +name/-name；off 不参与语义。函数先构造最终 enable/disable
 * 位，再取得 live cgroup、cgroup_mutex 并排空旧 offline。
 *
 * 锁内校验可用性、domain/threaded/no-internal-process 规则和 child 状态，
 * save 事务快照、修改 mask、apply 并 finalize。成功返回 nbytes；格式、
 * 权限/拓扑、资源或回调失败返回 errno，逻辑 mask 与 css/file 状态恢复。
 */
static ssize_t cgroup_subtree_control_write(struct kernfs_open_file *of,
					    char *buf, size_t nbytes,
					    loff_t off)
{
	u32 enable = 0, disable = 0;
	struct cgroup *cgrp, *child;
	struct cgroup_subsys *ss;
	char *tok;
	int ssid, ret;

	/*
	 * Parse input - space separated list of subsystem names prefixed
	 * with either + or -.
	 */
	/* 解析阶段只构造 +enable/-disable 位图，尚不修改层级状态。 */
	buf = strstrip(buf);
	while ((tok = strsep(&buf, " "))) {
		if (tok[0] == '\0')
			continue;
		do_each_subsys_mask(ss, ssid, ~cgrp_dfl_inhibit_ss_mask) {
			if (!cgroup_ssid_enabled(ssid) ||
			    strcmp(tok + 1, ss->name))
				continue;

			if (*tok == '+') {
				enable |= 1 << ssid;
				disable &= ~(1 << ssid);
			} else if (*tok == '-') {
				disable |= 1 << ssid;
				enable &= ~(1 << ssid);
			} else {
				return -EINVAL;
			}
			break;
		} while_each_subsys_mask();
		if (ssid == CGROUP_SUBSYS_COUNT)
			return -EINVAL;
	}

	cgrp = cgroup_kn_lock_live(of->kn, true);
	if (!cgrp)
		return -ENODEV;

	/*
	 * 校验阶段先把重复 enable/disable 归一化为空操作，再检查 controller
	 * 是否可由本节点支配，以及关闭时是否仍被任一 child 继续下放。
	 * 这一阶段不改变 subtree_control，因此任意失败都无需回滚。
	 */
	for_each_subsys(ss, ssid) {
		if (enable & (1 << ssid)) {
			if (cgrp->subtree_control & (1 << ssid)) {
				enable &= ~(1 << ssid);
				continue;
			}

			if (!(cgroup_control(cgrp) & (1 << ssid))) {
				ret = -ENOENT;
				goto out_unlock;
			}
		} else if (disable & (1 << ssid)) {
			if (!(cgrp->subtree_control & (1 << ssid))) {
				disable &= ~(1 << ssid);
				continue;
			}

			/* a child has it enabled? */
			/*
			 * 任一 child 仍下放该 controller 时，parent 不能先关闭
			 * 其来源。
			 */
			cgroup_for_each_live_child(child, cgrp) {
				if (child->subtree_control & (1 << ssid)) {
					ret = -EBUSY;
					goto out_unlock;
				}
			}
		}
	}

	if (!enable && !disable) {
		ret = 0;
		goto out_unlock;
	}

	/*
	 * 集合层面的规则（domain/threaded、内部进程等）必须在单个
	 * controller 检查之后、保存事务快照之前完成。
	 */
	ret = cgroup_vet_subtree_control_enable(cgrp, enable);
	if (ret)
		goto out_unlock;

	/* save and update control masks and prepare csses */
	/*
	 * 保存旧 mask 后写入候选状态，并预创建所需 css；失败由 restore 阶段
	 * 恢复旧 mask 和已构造对象。
	 */
	cgroup_save_control(cgrp);

	cgrp->subtree_control |= enable;
	cgrp->subtree_control &= ~disable;

	ret = cgroup_apply_control(cgrp);
	cgroup_finalize_control(cgrp, ret);
	if (ret)
		goto out_unlock;

	kernfs_activate(cgrp->kn);
out_unlock:
	cgroup_kn_unlock(of->kn);
	return ret ?: nbytes;
}

/**
 * cgroup_enable_threaded - make @cgrp threaded
 * @cgrp: the target cgroup
 *
 * Called when "threaded" is written to the cgroup.type interface file and
 * tries to make @cgrp threaded and join the parent's resource domain.
 * This function is never called on the root cgroup as cgroup.type doesn't
 * exist on it.
 */
/*
 * 中文契约：
 * 调用者持 cgroup_mutex；cgrp 非 root 且存活。已 threaded 幂等成功。
 * populated 或启用 domain controller 的节点不可切换；parent domain 必须
 * 连续有效且能成为 thread root。通过后保存事务快照，把 cgrp 及已有
 * threaded 后代的 dom_cgrp 改为 parent domain，再 apply/finalize。
 * 成功返回 0 并增加 parent->nr_threaded_children；失败返回 errno 并恢复。
 */
static int cgroup_enable_threaded(struct cgroup *cgrp)
{
	struct cgroup *parent = cgroup_parent(cgrp);
	struct cgroup *dom_cgrp = parent->dom_cgrp;
	struct cgroup *dsct;
	struct cgroup_subsys_state *d_css;
	int ret;

	lockdep_assert_held(&cgroup_mutex);

	/* noop if already threaded */
	/* 已处于 threaded 模式时幂等成功，不重复修改祖先计数或 css。 */
	if (cgroup_is_threaded(cgrp))
		return 0;

	/*
	 * If @cgroup is populated or has domain controllers enabled, it
	 * can't be switched.  While the below cgroup_can_be_thread_root()
	 * test can catch the same conditions, that's only when @parent is
	 * not mixable, so let's check it explicitly.
	 */
	/* mixable parent 会绕过后续通用判断，故 populated/domain mask 必须显查。 */
	if (cgroup_is_populated(cgrp) ||
	    cgrp->subtree_control & ~cgrp_dfl_threaded_ss_mask)
		return -EOPNOTSUPP;

	/* we're joining the parent's domain, ensure its validity */
	/*
	 * 切换后将加入 parent 的资源域，提交前必须验证该 domain
	 * 连续有效。
	 */
	if (!cgroup_is_valid_domain(dom_cgrp) ||
	    !cgroup_can_be_thread_root(dom_cgrp))
		return -EOPNOTSUPP;

	/*
	 * The following shouldn't cause actual migrations and should
	 * always succeed.
	 */
	/* 预检保证这里只重解释拓扑，不应触发真实迁移或可恢复失败。 */
	cgroup_save_control(cgrp);

	cgroup_for_each_live_descendant_pre(dsct, d_css, cgrp)
		if (dsct == cgrp || cgroup_is_threaded(dsct))
			dsct->dom_cgrp = dom_cgrp;

	ret = cgroup_apply_control(cgrp);
	if (!ret)
		parent->nr_threaded_children++;

	cgroup_finalize_control(cgrp, ret);
	return ret;
}

/*
 * cgroup_type_show() - 输出节点当前 v2 domain/threaded 状态。
 *
 * seq 隐含 active css；根据 dom_cgrp、祖先有效性和 thread-root
 * 判定输出 threaded/domain invalid/domain threaded/domain。
 * 返回 0，无状态变化。
 */
static int cgroup_type_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;

	if (cgroup_is_threaded(cgrp))
		seq_puts(seq, "threaded\n");
	else if (!cgroup_is_valid_domain(cgrp))
		seq_puts(seq, "domain invalid\n");
	else if (cgroup_is_thread_root(cgrp))
		seq_puts(seq, "domain threaded\n");
	else
		seq_puts(seq, "domain\n");

	return 0;
}

/*
 * cgroup_type_write() - 处理 cgroup.type 的单向 threaded 转换。
 *
 * 仅接受字符串 "threaded"；取得 live cgroup、cgroup_mutex 并排空 dying
 * css 后调用 cgroup_enable_threaded()。成功返回 nbytes；格式、死亡或
 * 拓扑错误返回 errno。domain 转 threaded 不支持反向恢复。
 */
static ssize_t cgroup_type_write(struct kernfs_open_file *of, char *buf,
				 size_t nbytes, loff_t off)
{
	struct cgroup *cgrp;
	int ret;

	/* only switching to threaded mode is supported */
	/* 用户接口只允许单向切换到 threaded，不支持从 threaded 恢复 domain。 */
	if (strcmp(strstrip(buf), "threaded"))
		return -EINVAL;

	/* drain dying csses before we re-apply (threaded) subtree control */
	/*
	 * 重算 threaded subtree_control 前等待旧 dying css 排空，避免新旧实例
	 * 对同一 controller 状态重叠。
	 */
	cgrp = cgroup_kn_lock_live(of->kn, true);
	if (!cgrp)
		return -ENOENT;

	/* threaded can only be enabled */
	/* 本接口只能增加 threaded 状态，其他目标类型一律拒绝。 */
	ret = cgroup_enable_threaded(cgrp);

	cgroup_kn_unlock(of->kn);
	return ret ?: nbytes;
}

/*
 * cgroup_max_descendants_show() - 输出该节点允许创建的最大后代数量。
 *
 * READ_ONCE 与无锁写者配对；INT_MAX 序列化为 "max"，否则输出十进制。
 * 返回 0，读值是瞬时策略快照。
 */
static int cgroup_max_descendants_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	int descendants = READ_ONCE(cgrp->max_descendants);

	if (descendants == INT_MAX)
		seq_puts(seq, "max\n");
	else
		seq_printf(seq, "%d\n", descendants);

	return 0;
}

/*
 * cgroup_max_descendants_write() - 更新后代数量上限。
 *
 * 接受非负整数或 "max"；解析后取得 live cgroup/cgroup_mutex 并用
 * WRITE_ONCE 发布。新限制不删除既有节点，只影响后续 mkdir。成功返回
 * nbytes；格式、负值或死亡分别返回解析 errno、-ERANGE、-ENOENT。
 */
static ssize_t cgroup_max_descendants_write(struct kernfs_open_file *of,
					   char *buf, size_t nbytes, loff_t off)
{
	struct cgroup *cgrp;
	int descendants;
	ssize_t ret;

	buf = strstrip(buf);
	if (!strcmp(buf, "max")) {
		descendants = INT_MAX;
	} else {
		ret = kstrtoint(buf, 0, &descendants);
		if (ret)
			return ret;
	}

	if (descendants < 0)
		return -ERANGE;

	cgrp = cgroup_kn_lock_live(of->kn, false);
	if (!cgrp)
		return -ENOENT;

	WRITE_ONCE(cgrp->max_descendants, descendants);

	cgroup_kn_unlock(of->kn);

	return nbytes;
}

/* 与 max_descendants_show 相同，但输出从本节点向下允许的最大相对深度。 */
/*
 * seq/v 的生命周期由 kernfs 管理；depth 单位是 hierarchy 边数，
 * INT_MAX 输出为 "max"。函数用 READ_ONCE 取得配置快照，返回 0，
 * 不修改限制或引用。
 */
static int cgroup_max_depth_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	int depth = READ_ONCE(cgrp->max_depth);

	if (depth == INT_MAX)
		seq_puts(seq, "max\n");
	else
		seq_printf(seq, "%d\n", depth);

	return 0;
}

/*
 * cgroup_max_depth_write() - 更新后续 mkdir 的层级深度上限。
 *
 * 输入/锁/错误语义同 max_descendants_write；数值单位是层级边数，
 * INT_MAX 表示无限制，既有深层节点不会被回收。
 */
static ssize_t cgroup_max_depth_write(struct kernfs_open_file *of,
				      char *buf, size_t nbytes, loff_t off)
{
	struct cgroup *cgrp;
	ssize_t ret;
	int depth;

	buf = strstrip(buf);
	if (!strcmp(buf, "max")) {
		depth = INT_MAX;
	} else {
		ret = kstrtoint(buf, 0, &depth);
		if (ret)
			return ret;
	}

	if (depth < 0)
		return -ERANGE;

	cgrp = cgroup_kn_lock_live(of->kn, false);
	if (!cgrp)
		return -ENOENT;

	WRITE_ONCE(cgrp->max_depth, depth);

	cgroup_kn_unlock(of->kn);

	return nbytes;
}

/*
 * cgroup_events_show() - 输出用户可轮询的 populated/frozen 状态。
 *
 * seq_css 钉住 cgroup；两个值分别来自维护的子树 populated 计数和
 * CGRP_FROZEN 位。返回 0；状态可能在读取后变化，变化路径会 notify 文件。
 */
static int cgroup_events_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;

	seq_printf(seq, "populated %d\n", cgroup_is_populated(cgrp));
	seq_printf(seq, "frozen %d\n", test_bit(CGRP_FROZEN, &cgrp->flags));

	return 0;
}

/*
 * cgroup_stat_show() - 输出 core 后代和各 v2 subsystem 的 live/dying 数量。
 *
 * RCU 读侧保护 subsys 指针存活，但原注释明确这些计数没有统一结构锁，
 * 所以跨行不构成原子快照。被 inhibit 或不绑定 v2 的控制器跳过。
 * 返回 0，仅读取统计、不取长期 css 引用。
 */
static int cgroup_stat_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgroup = seq_css(seq)->cgroup;
	struct cgroup_subsys_state *css;
	int dying_cnt[CGROUP_SUBSYS_COUNT];
	int ssid;

	seq_printf(seq, "nr_descendants %d\n",
		   cgroup->nr_descendants);

	/*
	 * Show the number of live and dying csses associated with each of
	 * non-inhibited cgroup subsystems that is bound to cgroup v2.
	 *
	 * Without proper lock protection, racing is possible. So the
	 * numbers may not be consistent when that happens.
	 */
	/*
	 * 中文补充：RCU 防 UAF，不冻结 nr_descendants/nr_dying_*；用户只能
	 * 把每项视为近似观测，不能用多项之间的算术关系做同步判断。
	 */
	rcu_read_lock();
	for (ssid = 0; ssid < CGROUP_SUBSYS_COUNT; ssid++) {
		dying_cnt[ssid] = -1;
		if ((BIT(ssid) & cgrp_dfl_inhibit_ss_mask) ||
		    (cgroup_subsys[ssid]->root !=  &cgrp_dfl_root))
			continue;
		css = rcu_dereference_raw(cgroup->subsys[ssid]);
		dying_cnt[ssid] = cgroup->nr_dying_subsys[ssid];
		seq_printf(seq, "nr_subsys_%s %d\n", cgroup_subsys[ssid]->name,
			   css ? (css->nr_descendants + 1) : 0);
	}

	seq_printf(seq, "nr_dying_descendants %d\n",
		   cgroup->nr_dying_descendants);
	for (ssid = 0; ssid < CGROUP_SUBSYS_COUNT; ssid++) {
		if (dying_cnt[ssid] >= 0)
			seq_printf(seq, "nr_dying_subsys_%s %d\n",
				   cgroup_subsys[ssid]->name, dying_cnt[ssid]);
	}
	rcu_read_unlock();
	return 0;
}

/*
 * cgroup_core_local_stat_show() - 输出本 cgroup 累计冻结时间。
 *
 * freezer seqcount 让 frozen_nsec、freeze_start_nsec 和 FREEZE 状态
 * 来自同一写序列；若正在冻结，再加当前区间。重试无副作用，纳秒换算
 * 为微秒后输出。返回 0。
 */
static int cgroup_core_local_stat_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	unsigned int sequence;
	u64 freeze_time;

	do {
		sequence = read_seqcount_begin(&cgrp->freezer.freeze_seq);
		freeze_time = cgrp->freezer.frozen_nsec;
		/* Add in current freezer interval if the cgroup is freezing. */
		/*
		 * 若仍在冻结过程中，把本轮尚未闭合的时间区间计入
		 * 读取快照。
		 */
		if (test_bit(CGRP_FREEZE, &cgrp->flags))
			freeze_time += (ktime_get_ns() -
					cgrp->freezer.freeze_start_nsec);
	} while (read_seqcount_retry(&cgrp->freezer.freeze_seq, sequence));

	do_div(freeze_time, NSEC_PER_USEC);
	seq_printf(seq, "frozen_usec %llu\n", freeze_time);

	return 0;
}

#ifdef CONFIG_CGROUP_SCHED
/**
 * cgroup_tryget_css - try to get a cgroup's css for the specified subsystem
 * @cgrp: the cgroup of interest
 * @ss: the subsystem of interest
 *
 * Find and get @cgrp's css associated with @ss.  If the css doesn't exist
 * or is offline, %NULL is returned.
 */
/*
 * 中文契约：
 * cgrp/ss 为借用。RCU 下读取 css 并尝试取得 online 引用；成功返回持有
 * 引用，调用者必须 css_put()；不存在或 dying 返回 NULL。引用只保生命，
 * 不冻结 controller 统计字段。
 */
static struct cgroup_subsys_state *cgroup_tryget_css(struct cgroup *cgrp,
						     struct cgroup_subsys *ss)
{
	struct cgroup_subsys_state *css;

	rcu_read_lock();
	css = cgroup_css(cgrp, ss);
	if (css && !css_tryget_online(css))
		css = NULL;
	rcu_read_unlock();

	return css;
}

/*
 * cgroup_extra_stat_show() - 分派指定 controller 的聚合附加统计。
 *
 * ssid 有效；无回调或 css offline 时静默成功。取得 online css
 * 引用跨越回调，返回值原样传播，随后始终 css_put()。
 */
static int cgroup_extra_stat_show(struct seq_file *seq, int ssid)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	struct cgroup_subsys *ss = cgroup_subsys[ssid];
	struct cgroup_subsys_state *css;
	int ret;

	if (!ss->css_extra_stat_show)
		return 0;

	css = cgroup_tryget_css(cgrp, ss);
	if (!css)
		return 0;

	ret = ss->css_extra_stat_show(seq, css);
	css_put(css);
	return ret;
}

/*
 * cgroup_local_stat_show() - 分派指定 cgroup/controller 的本地统计。
 *
 * 语义同 extra_stat，但调用 css_local_stat_show；不沿子树聚合。
 */
static int cgroup_local_stat_show(struct seq_file *seq,
				  struct cgroup *cgrp, int ssid)
{
	struct cgroup_subsys *ss = cgroup_subsys[ssid];
	struct cgroup_subsys_state *css;
	int ret;

	if (!ss->css_local_stat_show)
		return 0;

	css = cgroup_tryget_css(cgrp, ss);
	if (!css)
		return 0;

	ret = ss->css_local_stat_show(seq, css);
	css_put(css);
	return ret;
}
#endif

/* 输出 core 聚合 CPU 时间，并在启用调度 cgroup 时追加 cpu controller 统计。 */
/*
 * seq 是输出，v 未使用；基础统计总会输出，CONFIG_CGROUP_SCHED 下再分派
 * controller。返回 controller show errno 或 0，无引用转移；统计允许是
 * 读取时快照。
 */
static int cpu_stat_show(struct seq_file *seq, void *v)
{
	int ret = 0;

	cgroup_base_stat_cputime_show(seq);
#ifdef CONFIG_CGROUP_SCHED
	ret = cgroup_extra_stat_show(seq, cpu_cgrp_id);
#endif
	return ret;
}

/* 输出当前 cgroup 的 controller 本地 CPU 统计；未配置调度 cgroup 时为空。 */
/*
 * seq/v 契约同 cpu_stat_show；本接口不沿后代聚合。配置关闭时直接返回 0，
 * 开启时返回 cpu controller local-stat 结果，不修改统计状态。
 */
static int cpu_local_stat_show(struct seq_file *seq, void *v)
{
	struct cgroup __maybe_unused *cgrp = seq_css(seq)->cgroup;
	int ret = 0;

#ifdef CONFIG_CGROUP_SCHED
	ret = cgroup_local_stat_show(seq, cgrp, cpu_cgrp_id);
#endif
	return ret;
}

#ifdef CONFIG_PSI
/*
 * 三个 pressure_show wrapper 分别固定 PSI_IO/PSI_MEM/PSI_CPU，把当前
 * cgroup 的 psi_group 借给 psi_show()；返回其状态，无引用转移。
 */
static int cgroup_io_pressure_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	struct psi_group *psi = cgroup_psi(cgrp);

	return psi_show(seq, psi, PSI_IO);
}
/*
 * cgroup_memory_pressure_show() - 输出当前 cgroup 的内存压力窗口。
 *
 * seq 为输出、v 未使用；psi_group 为 cgroup 生命周期内借用。返回
 * psi_show() 状态，无锁与引用变化。
 */
static int cgroup_memory_pressure_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	struct psi_group *psi = cgroup_psi(cgrp);

	return psi_show(seq, psi, PSI_MEM);
}
/*
 * cgroup_cpu_pressure_show() - 输出当前 cgroup 的 CPU 压力窗口。
 *
 * 参数、上下文和返回规则同 memory wrapper，仅固定资源类型 PSI_CPU。
 */
static int cgroup_cpu_pressure_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	struct psi_group *psi = cgroup_psi(cgrp);

	return psi_show(seq, psi, PSI_CPU);
}

/*
 * pressure_write() - 为一个打开的 PSI 文件创建唯一触发器。
 *
 * of/buf 为 kernfs 借用，res 指定资源类型。锁定 live cgroup 后拒绝
 * 同一 fd 第二个 trigger；create 可分配并返回错误指针。成功以 release
 * store 发布 trigger，使 poll 只看到完整对象，返回 nbytes；失败返回
 * errno且不留 trigger。
 */
static ssize_t pressure_write(struct kernfs_open_file *of, char *buf,
			      size_t nbytes, enum psi_res res)
{
	struct cgroup_file_ctx *ctx;
	struct psi_trigger *new;
	struct cgroup *cgrp;
	struct psi_group *psi;
	ssize_t ret = 0;

	cgrp = cgroup_kn_lock_live(of->kn, false);
	if (!cgrp)
		return -ENODEV;

	ctx = of->priv;
	if (!ctx) {
		ret = -ENODEV;
		goto out_unlock;
	}

	/* Allow only one trigger per file descriptor */
	/*
	 * trigger 生命周期绑定 open fd；第二次写若替换对象会与 poll 竞争，
	 * 因此返回 -EBUSY。
	 */
	if (ctx->psi.trigger) {
		ret = -EBUSY;
		goto out_unlock;
	}

	psi = cgroup_psi(cgrp);
	new = psi_trigger_create(psi, buf, res, of->file, of);
	if (IS_ERR(new)) {
		ret = PTR_ERR(new);
		goto out_unlock;
	}

	smp_store_release(&ctx->psi.trigger, new);

out_unlock:
	cgroup_kn_unlock(of->kn);
	if (ret)
		return ret;

	return nbytes;
}

/* PSI write wrappers 只固定资源类型；off 不参与 trigger 语法。 */
/*
 * cgroup_io_pressure_write() - 为本 fd 创建 IO PSI trigger。
 *
 * of/buf/nbytes 为 kernfs 写入借用，off 忽略；返回完整写入长度或
 * pressure_write() errno，trigger ownership 交给该 fd 的 release 回调。
 */
static ssize_t cgroup_io_pressure_write(struct kernfs_open_file *of,
					  char *buf, size_t nbytes,
					  loff_t off)
{
	return pressure_write(of, buf, nbytes, PSI_IO);
}

/*
 * cgroup_memory_pressure_write() - 为本 fd 创建 memory PSI trigger。
 *
 * 参数、返回和 trigger 生命周期同 IO wrapper，仅固定 PSI_MEM。
 */
static ssize_t cgroup_memory_pressure_write(struct kernfs_open_file *of,
					  char *buf, size_t nbytes,
					  loff_t off)
{
	return pressure_write(of, buf, nbytes, PSI_MEM);
}

/*
 * cgroup_cpu_pressure_write() - 为本 fd 创建 CPU PSI trigger。
 *
 * 参数、返回和 trigger 生命周期同 IO wrapper，仅固定 PSI_CPU。
 */
static ssize_t cgroup_cpu_pressure_write(struct kernfs_open_file *of,
					  char *buf, size_t nbytes,
					  loff_t off)
{
	return pressure_write(of, buf, nbytes, PSI_CPU);
}

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
/* IRQ pressure 文件仅在 IRQ 时间记账配置下发布，契约同其他 PSI 文件。 */
/*
 * cgroup_irq_pressure_show() - 输出当前 cgroup 的 IRQ 压力。
 *
 * seq 为输出、v 未使用；返回 psi_show() 结果，无引用变化。仅在 IRQ
 * 时间记账配置下编译和发布。
 */
static int cgroup_irq_pressure_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	struct psi_group *psi = cgroup_psi(cgrp);

	return psi_show(seq, psi, PSI_IRQ);
}

/*
 * cgroup_irq_pressure_write() - 为本 fd 创建 IRQ PSI trigger。
 *
 * of/buf/nbytes/off 契约同其他 PSI writer，仅固定 PSI_IRQ；返回完整长度
 * 或 errno，trigger 由 release 回调销毁。
 */
static ssize_t cgroup_irq_pressure_write(struct kernfs_open_file *of,
					 char *buf, size_t nbytes,
					 loff_t off)
{
	return pressure_write(of, buf, nbytes, PSI_IRQ);
}
#endif

/* 输出该 cgroup PSI 文件组总开关；psi_group 由 cgroup 生命周期持有。 */
/*
 * seq 为输出、v 未使用；函数读取 psi->enabled 快照并返回 0，无锁、引用
 * 或启停副作用。
 */
static int cgroup_pressure_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	struct psi_group *psi = cgroup_psi(cgrp);

	seq_printf(seq, "%d\n", psi->enabled);

	return 0;
}

/*
 * cgroup_pressure_write() - 启停 cgroup PSI 采集及资源文件可见性。
 *
 * 只接受 0/1；锁定 live cgroup 后显示/隐藏所有 PSI 资源文件，再发布
 * enabled；启用还重启采样。成功返回 nbytes，解析/范围/死亡返回 errno。
 */
static ssize_t cgroup_pressure_write(struct kernfs_open_file *of,
				     char *buf, size_t nbytes,
				     loff_t off)
{
	ssize_t ret;
	int enable;
	struct cgroup *cgrp;
	struct psi_group *psi;

	ret = kstrtoint(strstrip(buf), 0, &enable);
	if (ret)
		return ret;

	if (enable < 0 || enable > 1)
		return -ERANGE;

	cgrp = cgroup_kn_lock_live(of->kn, false);
	if (!cgrp)
		return -ENOENT;

	psi = cgroup_psi(cgrp);
	if (psi->enabled != enable) {
		int i;

		/* show or hide {cpu,memory,io,irq}.pressure files */
		/* enabled 翻转时同步全部资源文件，避免只暴露部分 PSI ABI。 */
		for (i = 0; i < NR_PSI_RESOURCES; i++)
			cgroup_file_show(&cgrp->psi_files[i], enable);

		psi->enabled = enable;
		if (enable)
			psi_cgroup_restart(psi);
	}

	cgroup_kn_unlock(of->kn);

	return nbytes;
}

/* poll 将等待注册交给当前 fd 的 PSI trigger；返回标准 poll mask。 */
/*
 * of/pt 为 poll 框架借用；函数通过 fd 私有 ctx 访问已发布 trigger，
 * 不取得 ownership。返回就绪 mask，等待项生命周期由 poll 核心管理。
 */
static __poll_t cgroup_pressure_poll(struct kernfs_open_file *of,
					  poll_table *pt)
{
	struct cgroup_file_ctx *ctx = of->priv;

	return psi_trigger_poll(&ctx->psi.trigger, of->file, pt);
}

/* release 销毁本 fd 唯一 trigger；与 pressure_write 的发布配对。 */
/*
 * of 在关闭期间稳定；函数消费 ctx 中 trigger 的 ownership。返回无直接
 * 值，调用后该 trigger 不可再 poll，NULL 情形由 PSI helper 幂等处理。
 */
static void cgroup_pressure_release(struct kernfs_open_file *of)
{
	struct cgroup_file_ctx *ctx = of->priv;

	psi_trigger_destroy(ctx->psi.trigger);
}

/*
 * cgroup_psi_enabled() - 查询全局 PSI 与 cgroup feature 是否均启用。
 *
 * static key 或 feature disable mask 任一关闭即 false。纯热路径查询。
 */
bool cgroup_psi_enabled(void)
{
	if (static_branch_likely(&psi_disabled))
		return false;

	return (cgroup_feature_disable_mask & (1 << OPT_FEATURE_PRESSURE)) == 0;
}

#else /* CONFIG_PSI */
/* CONFIG_PSI=n 时固定返回 false，使调用者无需条件编译。 */
/* 入参、锁和副作用均无；返回 false 表示不发布任何 PSI cgroup 文件。 */
bool cgroup_psi_enabled(void)
{
	return false;
}

#endif /* CONFIG_PSI */

/* 输出 freezer.freeze 的用户目标值；完成状态另见 cgroup.events。 */
/*
 * seq 是输出、v 未使用；打开文件固定 cgroup。函数读取请求态 freeze，
 * 返回 0 且无副作用；该值不承诺所有 task 已完成冻结。
 */
static int cgroup_freeze_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;

	seq_printf(seq, "%d\n", cgrp->freezer.freeze);

	return 0;
}

/*
 * cgroup_freeze_write() - 请求冻结或解冻 cgroup 子树。
 *
 * 输入只允许 0/1；锁定 live cgroup 后调用 freezer 状态机。成功返回
 * nbytes，不保证所有 task 已同步完成，完成转换由 events frozen 通知。
 * 解析、范围或死亡返回 errno。
 */
static ssize_t cgroup_freeze_write(struct kernfs_open_file *of,
				   char *buf, size_t nbytes, loff_t off)
{
	struct cgroup *cgrp;
	ssize_t ret;
	int freeze;

	ret = kstrtoint(strstrip(buf), 0, &freeze);
	if (ret)
		return ret;

	if (freeze < 0 || freeze > 1)
		return -ERANGE;

	cgrp = cgroup_kn_lock_live(of->kn, false);
	if (!cgrp)
		return -ENOENT;

	cgroup_freeze(cgrp, freeze);

	cgroup_kn_unlock(of->kn);

	return nbytes;
}

/*
 * __cgroup_kill() - 向一个 cgroup 中所有用户进程发送 SIGKILL。
 *
 * 调用者持 cgroup_mutex。先在 css_set_lock 下推进 kill_seq，标记本轮；
 * 再用支持 threaded 的 process 迭代器扫描。kernel thread 和已有 fatal
 * signal 的 task 跳过。只发信号、不等待退出，返回：无。
 */
static void __cgroup_kill(struct cgroup *cgrp)
{
	struct css_task_iter it;
	struct task_struct *task;

	lockdep_assert_held(&cgroup_mutex);

	spin_lock_irq(&css_set_lock);
	cgrp->kill_seq++;
	spin_unlock_irq(&css_set_lock);

	css_task_iter_start(&cgrp->self, CSS_TASK_ITER_PROCS | CSS_TASK_ITER_THREADED, &it);
	while ((task = css_task_iter_next(&it))) {
		/* Ignore kernel threads here. */
		/* kernel thread 不属于 cgroup.kill 面向的用户进程终止语义。 */
		if (task->flags & PF_KTHREAD)
			continue;

		/* Skip tasks that are already dying. */
		/* 已有 fatal signal 的 task 正在退出，无需重复发送 SIGKILL。 */
		if (__fatal_signal_pending(task))
			continue;

		send_sig(SIGKILL, task, 0);
	}
	css_task_iter_end(&it);
}

/* 前序遍历 live 子树，对每个节点执行 __cgroup_kill；不等待 task 消亡。 */
/*
 * cgrp 为借用的子树根，调用者持 cgroup_mutex；函数以前序遍历覆盖所有
 * live 后代并推进各自 kill_seq。返回无直接值，不取得引用、不等待退出，
 * 但发送信号产生异步 task 状态副作用。
 */
static void cgroup_kill(struct cgroup *cgrp)
{
	struct cgroup_subsys_state *css;
	struct cgroup *dsct;

	lockdep_assert_held(&cgroup_mutex);

	cgroup_for_each_live_descendant_pre(dsct, css, cgrp)
		__cgroup_kill(dsct);
}

/*
 * cgroup_kill_write() - 处理 cgroup.kill 的一次性 kill 请求。
 *
 * 仅接受数值 1；锁定 live cgroup 后，threaded 节点因操作是进程粒度而
 * 返回 -EOPNOTSUPP，domain 节点向整个子树发 SIGKILL。成功返回 nbytes，
 * 不等待 populated 变零。
 */
static ssize_t cgroup_kill_write(struct kernfs_open_file *of, char *buf,
				 size_t nbytes, loff_t off)
{
	ssize_t ret = 0;
	int kill;
	struct cgroup *cgrp;

	ret = kstrtoint(strstrip(buf), 0, &kill);
	if (ret)
		return ret;

	if (kill != 1)
		return -ERANGE;

	cgrp = cgroup_kn_lock_live(of->kn, false);
	if (!cgrp)
		return -ENOENT;

	/*
	 * Killing is a process directed operation, i.e. the whole thread-group
	 * is taken down so act like we do for cgroup.procs and only make this
	 * writable in non-threaded cgroups.
	 */
	/*
	 * kill 面向整个进程而非单线程，语义与 cgroup.procs 一致；threaded
	 * 节点不能作为该进程级操作的独立目标。
	 */
	if (cgroup_is_threaded(cgrp))
		ret = -EOPNOTSUPP;
	else
		cgroup_kill(cgrp);

	cgroup_kn_unlock(of->kn);

	return ret ?: nbytes;
}

/*
 * cgroup_file_open() - 为每个打开的控制文件建立私有上下文。
 *
 * kernfs active reference 稳定 of/cft。函数可睡眠分配 ctx，取得打开时
 * current cgroup namespace 引用并发布到 of->priv，再分派可选 cft open。
 * 成功后 release 接管；回调失败释放全部资源并返回 errno。
 */
static int cgroup_file_open(struct kernfs_open_file *of)
{
	struct cftype *cft = of_cft(of);
	struct cgroup_file_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;

	ctx->ns = current->nsproxy->cgroup_ns;
	get_cgroup_ns(ctx->ns);
	of->priv = ctx;

	if (!cft->open)
		return 0;

	ret = cft->open(of);
	if (ret) {
		put_cgroup_ns(ctx->ns);
		kfree(ctx);
	}
	return ret;
}

/*
 * cgroup_file_release() - 配对销毁 open 建立的文件上下文。
 *
 * 先调用可选 cft release，再 put namespace、释放 ctx 并清空 of->priv。
 * 返回：无；release 回调期间 ctx 仍完整有效。
 */
static void cgroup_file_release(struct kernfs_open_file *of)
{
	struct cftype *cft = of_cft(of);
	struct cgroup_file_ctx *ctx = of->priv;

	if (cft->release)
		cft->release(of);
	put_cgroup_ns(ctx->ns);
	kfree(ctx);
	of->priv = NULL;
}

/*
 * cgroup_file_write() - cftype 写操作的统一分派和数值适配层。
 *
 * namespace delegation 禁止非 init namespace 修改其根上的非授权文件。
 * 自定义 write 取得原始输入；否则解析 u64/s64 并调用 typed 回调。active
 * ref 保证 css 在回调期间存活。成功返回 nbytes；权限、解析、回调错误
 * 原样返回。
 */
static ssize_t cgroup_file_write(struct kernfs_open_file *of, char *buf,
				 size_t nbytes, loff_t off)
{
	struct cgroup_file_ctx *ctx = of->priv;
	struct cgroup *cgrp = kn_priv(of->kn);
	struct cftype *cft = of_cft(of);
	struct cgroup_subsys_state *css;
	int ret;

	if (!nbytes)
		return 0;

	/*
	 * If namespaces are delegation boundaries, disallow writes to
	 * files in an non-init namespace root from inside the namespace
	 * except for the files explicitly marked delegatable -
	 * eg. cgroup.procs, cgroup.threads and cgroup.subtree_control.
	 */
	/*
	 * namespace root 是 delegation 边界；内部调用者只能写显式标记
	 * delegatable 的文件，避免修改委托方保留的控制接口。
	 */
	if ((cgrp->root->flags & CGRP_ROOT_NS_DELEGATE) &&
	    !(cft->flags & CFTYPE_NS_DELEGATABLE) &&
	    ctx->ns != &init_cgroup_ns && ctx->ns->root_cset->dfl_cgrp == cgrp)
		return -EPERM;

	if (cft->write)
		return cft->write(of, buf, nbytes, off);

	/*
	 * kernfs guarantees that a file isn't deleted with operations in
	 * flight, which means that the matching css is and stays alive and
	 * doesn't need to be pinned.  The RCU locking is not necessary
	 * either.  It's just for the convenience of using cgroup_css().
	 */
	/*
	 * 中文补充：RCU 只满足 accessor 断言；真正的存活保证来自 kernfs
	 * 删除前 drain active 操作的协议。
	 */
	rcu_read_lock();
	css = cgroup_css(cgrp, cft->ss);
	rcu_read_unlock();

	if (cft->write_u64) {
		unsigned long long v;
		ret = kstrtoull(buf, 0, &v);
		if (!ret)
			ret = cft->write_u64(css, cft, v);
	} else if (cft->write_s64) {
		long long v;
		ret = kstrtoll(buf, 0, &v);
		if (!ret)
			ret = cft->write_s64(css, cft, v);
	} else {
		ret = -EINVAL;
	}

	return ret ?: nbytes;
}

/* 优先分派 cft 自定义 poll，否则使用 kernfs 通知序号的通用 poll。 */
/*
 * of/pt 为 poll 框架借用；打开文件 active ref 固定 cft/css。返回自定义
 * poll mask 或 kernfs 通用通知结果，等待项由 poll 核心管理，无引用转移。
 */
static __poll_t cgroup_file_poll(struct kernfs_open_file *of, poll_table *pt)
{
	struct cftype *cft = of_cft(of);

	if (cft->poll)
		return cft->poll(of, pt);

	return kernfs_generic_poll(of, pt);
}

/*
 * seq start/next/stop wrapper 把 kernfs 游标转交 cftype 自定义迭代器；
 * stop 可空。游标对象、锁和返回项生命周期由具体 cft 回调定义。
 */
static void *cgroup_seqfile_start(struct seq_file *seq, loff_t *ppos)
{
	return seq_cft(seq)->seq_start(seq, ppos);
}

/*
 * cgroup_seqfile_next() - 分派 cftype 自定义 seq next。
 *
 * seq/v/ppos 均由 seq_file 借用；回调负责推进位置并返回下一项借用指针、
 * NULL 或错误指针。wrapper 不持锁、不改变 ownership。
 */
static void *cgroup_seqfile_next(struct seq_file *seq, void *v, loff_t *ppos)
{
	return seq_cft(seq)->seq_next(seq, v, ppos);
}

/*
 * cgroup_seqfile_stop() - 结束可选的 cftype 自定义遍历。
 *
 * seq/v 为借用；存在 stop 回调时由其释放 iterator 资源。返回无直接值，
 * 无回调即无副作用。
 */
static void cgroup_seqfile_stop(struct seq_file *seq, void *v)
{
	if (seq_cft(seq)->seq_stop)
		seq_cft(seq)->seq_stop(seq, v);
}

/*
 * cgroup_seqfile_show() - 统一分派文本读取。
 *
 * 自定义 seq_show 优先；否则调用 typed read_u64/read_s64 并追加换行。
 * css 由打开文件 active reference 钉住。返回回调 errno、-EINVAL 或 0。
 */
static int cgroup_seqfile_show(struct seq_file *m, void *arg)
{
	struct cftype *cft = seq_cft(m);
	struct cgroup_subsys_state *css = seq_css(m);

	if (cft->seq_show)
		return cft->seq_show(m, arg);

	if (cft->read_u64)
		seq_printf(m, "%llu\n", cft->read_u64(css, cft));
	else if (cft->read_s64)
		seq_printf(m, "%lld\n", cft->read_s64(css, cft));
	else
		return -EINVAL;
	return 0;
}

/*
 * 两张 kernfs_ops 分别服务 single_open 与自定义 seq iterator。两者共享
 * open/release/write/poll/show，并把单次原子写限制为 PAGE_SIZE。
 */
static struct kernfs_ops cgroup_kf_single_ops = {
	.atomic_write_len	= PAGE_SIZE,
	.open			= cgroup_file_open,
	.release		= cgroup_file_release,
	.write			= cgroup_file_write,
	.poll			= cgroup_file_poll,
	.seq_show		= cgroup_seqfile_show,
};

static struct kernfs_ops cgroup_kf_ops = {
	.atomic_write_len	= PAGE_SIZE,
	.open			= cgroup_file_open,
	.release		= cgroup_file_release,
	.write			= cgroup_file_write,
	.poll			= cgroup_file_poll,
	.seq_start		= cgroup_seqfile_start,
	.seq_next		= cgroup_seqfile_next,
	.seq_stop		= cgroup_seqfile_stop,
	.seq_show		= cgroup_seqfile_show,
};

/* timer 回调从内嵌节点还原 cgroup_file，再进入带节流的通知路径。 */
/*
 * timer 由 cgroup_file 生命周期持有，回调不可睡眠；container_of 恢复
 * owner 后只触发/重排通知。返回无直接值，不取得长期引用。
 */
static void cgroup_file_notify_timer(struct timer_list *timer)
{
	cgroup_file_notify(container_of(timer, struct cgroup_file,
					notify_timer));
}

/*
 * cgroup_add_file() - 将一个 cftype 实例化为 cgrp 下的 kernfs 文件。
 *
 * css/cgrp/cft 在 cgroup_mutex 下稳定。函数生成名称和权限并创建 node；
 * file_offset 非零时初始化 css 内嵌 cgroup_file 的 timer/锁并发布 kn。
 * 成功返回 0；创建失败返回 errno且不发布内嵌状态。
 */
static int cgroup_add_file(struct cgroup_subsys_state *css, struct cgroup *cgrp,
			   struct cftype *cft)
{
	char name[CGROUP_FILE_NAME_MAX];
	struct kernfs_node *kn;
	struct lock_class_key *key = NULL;

#ifdef CONFIG_DEBUG_LOCK_ALLOC
	key = &cft->lockdep_key;
#endif
	kn = __kernfs_create_file(cgrp->kn, cgroup_file_name(cgrp, cft, name),
				  cgroup_file_mode(cft),
				  current_fsuid(), current_fsgid(),
				  0, cft->kf_ops, cft,
				  NULL, key);
	if (IS_ERR(kn))
		return PTR_ERR(kn);

	if (cft->file_offset) {
		struct cgroup_file *cfile = (void *)css + cft->file_offset;

		timer_setup(&cfile->notify_timer, cgroup_file_notify_timer, 0);
		spin_lock_init(&cfile->lock);
		cfile->kn = kn;
	}

	return 0;
}

/**
 * cgroup_addrm_files - add or remove files to a cgroup directory
 * @css: the target css
 * @cgrp: the target cgroup (usually css->cgroup)
 * @cfts: array of cftypes to be added
 * @is_add: whether to add or remove
 *
 * Depending on @is_add, add or remove files defined by @cfts on @cgrp.
 * For removals, this function never fails.
 */
/*
 * 中文契约：
 * 调用者持 cgroup_mutex；cfts 以空 name 结尾。函数按 v1/v2/root/debug/
 * feature flag 筛选；add 首次失败后切到 remove，从头撤销此前成功文件并
 * 返回原 errno。remove 模式幂等且返回 0，输入 ownership 不转移。
 */
static int cgroup_addrm_files(struct cgroup_subsys_state *css,
			      struct cgroup *cgrp, struct cftype cfts[],
			      bool is_add)
{
	struct cftype *cft, *cft_end = NULL;
	int ret = 0;

	lockdep_assert_held(&cgroup_mutex);

restart:
	for (cft = cfts; cft != cft_end && cft->name[0] != '\0'; cft++) {
		/* does cft->flags tell us to skip this file on @cgrp? */
		/* 综合 root、层级版本和 delegation flags 判断此文件是否适用。 */
		if ((cft->flags & __CFTYPE_ONLY_ON_DFL) && !cgroup_on_dfl(cgrp))
			continue;
		if ((cft->flags & __CFTYPE_NOT_ON_DFL) && cgroup_on_dfl(cgrp))
			continue;
		if ((cft->flags & CFTYPE_NOT_ON_ROOT) && !cgroup_parent(cgrp))
			continue;
		if ((cft->flags & CFTYPE_ONLY_ON_ROOT) && cgroup_parent(cgrp))
			continue;
		if ((cft->flags & CFTYPE_DEBUG) && !cgroup_debug)
			continue;
		if (is_add) {
			ret = cgroup_add_file(css, cgrp, cft);
			if (ret) {
				pr_warn("%s: failed to add %s, err=%d\n",
					__func__, cft->name, ret);
				cft_end = cft;
				is_add = false;
				goto restart;
			}
		} else {
			cgroup_rm_file(cgrp, cft);
		}
	}
	return ret;
}

/*
 * cgroup_apply_cftypes() - 把一组已初始化 cftype 应用到所有既有 cgroup。
 *
 * 调用者持 cgroup_mutex；cfts[0].ss 确定目标 controller，is_add 选择创建
 * 或删除。只处理 CSS_VISIBLE 后代；add 成功后激活 root kernfs。返回首个
 * errno或 0，单个目录的局部回滚由 cgroup_addrm_files() 完成。
 */
static int cgroup_apply_cftypes(struct cftype *cfts, bool is_add)
{
	struct cgroup_subsys *ss = cfts[0].ss;
	struct cgroup *root = &ss->root->cgrp;
	struct cgroup_subsys_state *css;
	int ret = 0;

	lockdep_assert_held(&cgroup_mutex);

	/* add/rm files for all cgroups created before */
	/*
	 * 注册变化必须同步到此前已创建的全部 cgroup，而非只影响
	 * 未来节点。
	 */
	css_for_each_descendant_pre(css, cgroup_css(root, ss)) {
		struct cgroup *cgrp = css->cgroup;

		if (!(css->flags & CSS_VISIBLE))
			continue;

		ret = cgroup_addrm_files(css, cgrp, cfts, is_add);
		if (ret)
			break;
	}

	if (is_add && !ret)
		kernfs_activate(root->kn);
	return ret;
}

/*
 * cgroup_exit_cftypes() - 撤销 cftype 数组的 core 初始化状态。
 *
 * 释放为自定义 max_write_len 复制的 kernfs_ops，清空 kf_ops/ss 并移除
 * core 私有 flags；数组及回调由 subsystem 拥有，不释放。可用于注册失败
 * 回滚或注销，返回：无。
 */
static void cgroup_exit_cftypes(struct cftype *cfts)
{
	struct cftype *cft;

	for (cft = cfts; cft->name[0] != '\0'; cft++) {
		/* free copy for custom atomic_write_len, see init_cftypes() */
		/* 自定义 atomic_write_len 使用复制表，注销时释放该副本。 */
		if (cft->max_write_len && cft->max_write_len != PAGE_SIZE)
			kfree(cft->kf_ops);
		cft->kf_ops = NULL;
		cft->ss = NULL;

		/* revert flags set by cgroup core while adding @cfts */
		/* 撤销 core 注入的内部 flags，使原 cftype 描述可安全复用。 */
		cft->flags &= ~(__CFTYPE_ONLY_ON_DFL | __CFTYPE_NOT_ON_DFL |
				__CFTYPE_ADDED);
	}
}

/*
 * cgroup_init_cftypes() - 为 subsystem 的 cftype 数组准备 kernfs 分派表。
 *
 * ss/cfts 为调用者拥有；函数逐项选择 single 或 iterator ops。需要非默认
 * atomic_write_len 时可睡眠复制 ops，再设置 ss 和 ADDED 发布标志。
 * 成功返回 0；重复注册 -EBUSY，分配失败 -ENOMEM，并撤销此前所有项。
 */
static int cgroup_init_cftypes(struct cgroup_subsys *ss, struct cftype *cfts)
{
	struct cftype *cft;
	int ret = 0;

	for (cft = cfts; cft->name[0] != '\0'; cft++) {
		struct kernfs_ops *kf_ops;

		WARN_ON(cft->ss || cft->kf_ops);

		if (cft->flags & __CFTYPE_ADDED) {
			ret = -EBUSY;
			break;
		}

		if (cft->seq_start)
			kf_ops = &cgroup_kf_ops;
		else
			kf_ops = &cgroup_kf_single_ops;

		/*
		 * Ugh... if @cft wants a custom max_write_len, we need to
		 * make a copy of kf_ops to set its atomic_write_len.
		 */
		/*
		 * 共享静态 ops 不能为单个文件改写；只在需求不同于 PAGE_SIZE
		 * 时复制，注销路径依据同一条件释放。
		 */
		if (cft->max_write_len && cft->max_write_len != PAGE_SIZE) {
			kf_ops = kmemdup(kf_ops, sizeof(*kf_ops), GFP_KERNEL);
			if (!kf_ops) {
				ret = -ENOMEM;
				break;
			}
			kf_ops->atomic_write_len = cft->max_write_len;
		}

		cft->kf_ops = kf_ops;
		cft->ss = ss;
		cft->flags |= __CFTYPE_ADDED;
	}

	if (ret)
		cgroup_exit_cftypes(cfts);
	return ret;
}

/*
 * cgroup_rm_cftypes_locked() - 在总锁下完成 cftype 注销。
 *
 * 先从 ss->cfts 摘除，阻止未来 cgroup 创建文件；再删除所有既有实例，
 * 最后清理数组 core 状态。返回：无，调用后可重新初始化注册。
 */
static void cgroup_rm_cftypes_locked(struct cftype *cfts)
{
	lockdep_assert_held(&cgroup_mutex);

	list_del(&cfts->node);
	cgroup_apply_cftypes(cfts, false);
	cgroup_exit_cftypes(cfts);
}

/**
 * cgroup_rm_cftypes - remove an array of cftypes from a subsystem
 * @cfts: zero-length name terminated array of cftypes
 *
 * Unregister @cfts.  Files described by @cfts are removed from all
 * existing cgroups and all future cgroups won't have them either.  This
 * function can be called anytime whether @cfts' subsys is attached or not.
 *
 * Returns 0 on successful unregistration, -ENOENT if @cfts is not
 * registered.
 */
/*
 * 中文契约：
 * 空数组是幂等成功；未注册返回 -ENOENT。其余路径自行取得 cgroup_mutex，
 * 删除未来注册入口和全部既有文件，返回 0。函数可能等待 kernfs active
 * 操作；不释放 cfts 数组本身。
 */
int cgroup_rm_cftypes(struct cftype *cfts)
{
	if (!cfts || cfts[0].name[0] == '\0')
		return 0;

	if (!(cfts[0].flags & __CFTYPE_ADDED))
		return -ENOENT;

	cgroup_lock();
	cgroup_rm_cftypes_locked(cfts);
	cgroup_unlock();
	return 0;
}

/**
 * cgroup_add_cftypes - add an array of cftypes to a subsystem
 * @ss: target cgroup subsystem
 * @cfts: zero-length name terminated array of cftypes
 *
 * Register @cfts to @ss.  Files described by @cfts are created for all
 * existing cgroups to which @ss is attached and all future cgroups will
 * have them too.  This function can be called anytime whether @ss is
 * attached or not.
 *
 * Returns 0 on successful registration, -errno on failure.  Note that this
 * function currently returns 0 as long as @cfts registration is successful
 * even if some file creation attempts on existing cgroups fail.
 */
/*
 * 中文契约：
 * disabled controller 或空数组按无操作成功。否则先在锁外初始化 cft，
 * 再持 cgroup_mutex 将数组加入 ss->cfts（使未来节点可见），并应用到所有
 * 既有 css。失败时完整注销并返回 errno；成功返回 0。调用者持续拥有
 * 数组，但注册期间不得修改其字段。
 */
int cgroup_add_cftypes(struct cgroup_subsys *ss, struct cftype *cfts)
{
	int ret;

	if (!cgroup_ssid_enabled(ss->id))
		return 0;

	if (!cfts || cfts[0].name[0] == '\0')
		return 0;

	ret = cgroup_init_cftypes(ss, cfts);
	if (ret)
		return ret;

	cgroup_lock();

	list_add_tail(&cfts->node, &ss->cfts);
	ret = cgroup_apply_cftypes(cfts, true);
	if (ret)
		cgroup_rm_cftypes_locked(cfts);

	cgroup_unlock();
	return ret;
}

/**
 * cgroup_add_dfl_cftypes - add an array of cftypes for default hierarchy
 * @ss: target cgroup subsystem
 * @cfts: zero-length name terminated array of cftypes
 *
 * Similar to cgroup_add_cftypes() but the added files are only used for
 * the default hierarchy.
 */
/*
 * 中文契约：先给每项加 core 私有 ONLY_ON_DFL 位，再走通用注册。失败时
 * 通用 cleanup 会清该位；成功后文件只实例化于 v2。
 */
int cgroup_add_dfl_cftypes(struct cgroup_subsys *ss, struct cftype *cfts)
{
	struct cftype *cft;

	for (cft = cfts; cft && cft->name[0] != '\0'; cft++)
		cft->flags |= __CFTYPE_ONLY_ON_DFL;
	return cgroup_add_cftypes(ss, cfts);
}

/**
 * cgroup_add_legacy_cftypes - add an array of cftypes for legacy hierarchies
 * @ss: target cgroup subsystem
 * @cfts: zero-length name terminated array of cftypes
 *
 * Similar to cgroup_add_cftypes() but the added files are only used for
 * the legacy hierarchies.
 */
/*
 * 中文契约：与 dfl wrapper 对称，设置 NOT_ON_DFL 后注册，仅在 v1 实例化。
 */
int cgroup_add_legacy_cftypes(struct cgroup_subsys *ss, struct cftype *cfts)
{
	struct cftype *cft;

	for (cft = cfts; cft && cft->name[0] != '\0'; cft++)
		cft->flags |= __CFTYPE_NOT_ON_DFL;
	return cgroup_add_cftypes(ss, cfts);
}

/**
 * cgroup_file_notify - generate a file modified event for a cgroup_file
 * @cfile: target cgroup_file
 *
 * @cfile must have been obtained by setting cftype->file_offset.
 */
/*
 * 中文契约与节流：
 * cfile 是 css 内嵌、由 file_offset 初始化的对象。kn==NULL 表示文件已
 * 摘除，直接返回。若距上次通知不足最短间隔，timer_reduce 合并到最早
 * 允许时刻；否则在 cfile->lock 下取得 kn 引用并更新时间戳，出锁后
 * kernfs_notify()，避免在自旋锁内唤醒等待者。返回：无，可从 IRQ-safe
 * 上下文调用。
 */
void cgroup_file_notify(struct cgroup_file *cfile)
{
	unsigned long flags, last, next;
	struct kernfs_node *kn = NULL;

	if (!READ_ONCE(cfile->kn))
		return;

	last = READ_ONCE(cfile->notified_at);
	next = last + CGROUP_FILE_NOTIFY_MIN_INTV;
	if (time_in_range(jiffies, last, next)) {
		timer_reduce(&cfile->notify_timer, next);
		if (timer_pending(&cfile->notify_timer))
			return;
	}

	spin_lock_irqsave(&cfile->lock, flags);
	if (cfile->kn) {
		kn = cfile->kn;
		kernfs_get(kn);
		WRITE_ONCE(cfile->notified_at, jiffies);
	}
	spin_unlock_irqrestore(&cfile->lock, flags);

	if (kn) {
		kernfs_notify(kn);
		kernfs_put(kn);
	}
}
EXPORT_SYMBOL_GPL(cgroup_file_notify);

/**
 * cgroup_file_show - show or hide a hidden cgroup file
 * @cfile: target cgroup_file obtained by setting cftype->file_offset
 * @show: whether to show or hide
 */
/*
 * 中文契约：
 * cfile 来自 file_offset，show 选择 kernfs 可见性。锁内快照 kn 并
 * kernfs_get()，使并发 cgroup_rm_file() 清空 kn 后仍可安全调用 show；
 * NULL 时无操作。函数不改变 cfile ownership，返回：无。
 */
void cgroup_file_show(struct cgroup_file *cfile, bool show)
{
	struct kernfs_node *kn;

	spin_lock_irq(&cfile->lock);
	kn = cfile->kn;
	kernfs_get(kn);
	spin_unlock_irq(&cfile->lock);

	if (kn)
		kernfs_show(kn, show);

	kernfs_put(kn);
}

/**
 * css_next_child - find the next child of a given css
 * @pos: the current position (%NULL to initiate traversal)
 * @parent: css whose children to walk
 *
 * This function returns the next child of @parent and should be called
 * under either cgroup_mutex or RCU read lock.  The only requirement is
 * that @parent and @pos are accessible.  The next sibling is guaranteed to
 * be returned regardless of their states.
 *
 * If a subsystem synchronizes ->css_online() and the start of iteration, a
 * css which finished ->css_online() is guaranteed to be visible in the
 * future iterations and will stay visible until the last reference is put.
 * A css which hasn't finished ->css_online() or already finished
 * ->css_offline() may show up during traversal.  It's each subsystem's
 * responsibility to synchronize against on/offlining.
 */
/*
 * 中文契约与并发：
 * 调用者每次调用持 cgroup_mutex 或 RCU 读锁，并保证 parent/pos 可访问；
 * pos==NULL 从首 child 开始。返回下一 child 裸指针或 NULL，不过滤
 * online/offline，也不取引用。
 *
 * 快速路径沿 sibling.next；若 pos 已 CSS_RELEASED，next 不再可靠，就用
 * 单调 serial_nr 从 parent 头部找第一个更新 sibling。这样遍历可在两次
 * 调用间释放 RCU，但 subsystem 仍须自行同步 online 可见性。
 */
struct cgroup_subsys_state *css_next_child(struct cgroup_subsys_state *pos,
					   struct cgroup_subsys_state *parent)
{
	struct cgroup_subsys_state *next;

	cgroup_assert_mutex_or_rcu_locked();

	/*
	 * @pos could already have been unlinked from the sibling list.
	 * Once a cgroup is removed, its ->sibling.next is no longer
	 * updated when its next sibling changes.  CSS_RELEASED is set when
	 * @pos is taken off list, at which time its next pointer is valid,
	 * and, as releases are serialized, the one pointed to by the next
	 * pointer is guaranteed to not have started release yet.  This
	 * implies that if we observe !CSS_RELEASED on @pos in this RCU
	 * critical section, the one pointed to by its next pointer is
	 * guaranteed to not have finished its RCU grace period even if we
	 * have dropped rcu_read_lock() in-between iterations.
	 *
	 * If @pos has CSS_RELEASED set, its next pointer can't be
	 * dereferenced; however, as each css is given a monotonically
	 * increasing unique serial number and always appended to the
	 * sibling list, the next one can be found by walking the parent's
	 * children until the first css with higher serial number than
	 * @pos's.  While this path can be slower, it happens iff iteration
	 * races against release and the race window is very small.
	 */
	/*
	 * serial fallback 只在释放竞态发生，牺牲一次线性扫描换取无需长期
	 * 持有 pos 引用的 iterator 接口。
	 */
	if (!pos) {
		next = list_entry_rcu(parent->children.next, struct cgroup_subsys_state, sibling);
	} else if (likely(!(pos->flags & CSS_RELEASED))) {
		next = list_entry_rcu(pos->sibling.next, struct cgroup_subsys_state, sibling);
	} else {
		list_for_each_entry_rcu(next, &parent->children, sibling,
					lockdep_is_held(&cgroup_mutex))
			if (next->serial_nr > pos->serial_nr)
				break;
	}

	/*
	 * @next, if not pointing to the head, can be dereferenced and is
	 * the next sibling.
	 */
	/*
	 * next 未回到 head 时仍可在当前保护域解引用并作为下一
	 * sibling 返回。
	 */
	if (&next->sibling != &parent->children)
		return next;
	return NULL;
}

/**
 * css_next_descendant_pre - find the next descendant for pre-order walk
 * @pos: the current position (%NULL to initiate traversal)
 * @root: css whose descendants to walk
 *
 * To be used by css_for_each_descendant_pre().  Find the next descendant
 * to visit for pre-order traversal of @root's descendants.  @root is
 * included in the iteration and the first node to be visited.
 *
 * While this function requires cgroup_mutex or RCU read locking, it
 * doesn't require the whole traversal to be contained in a single critical
 * section. Additionally, it isn't necessary to hold onto a reference to @pos.
 * This function will return the correct next descendant as long as both @pos
 * and @root are accessible and @pos is a descendant of @root.
 *
 * If a subsystem synchronizes ->css_online() and the start of iteration, a
 * css which finished ->css_online() is guaranteed to be visible in the
 * future iterations and will stay visible until the last reference is put.
 * A css which hasn't finished ->css_online() or already finished
 * ->css_offline() may show up during traversal.  It's each subsystem's
 * responsibility to synchronize against on/offlining.
 */
/*
 * 中文契约：
 * pre-order 包含 root 且先访问 parent 再访问 children。pos==NULL 返回
 * root；否则优先首 child，无 child 就沿祖先找下一个 sibling。锁、引用
 * 和 online 可见性契约同 css_next_child()；结束返回 NULL。
 */
struct cgroup_subsys_state *
css_next_descendant_pre(struct cgroup_subsys_state *pos,
			struct cgroup_subsys_state *root)
{
	struct cgroup_subsys_state *next;

	cgroup_assert_mutex_or_rcu_locked();

	/* if first iteration, visit @root */
	/* 前序遍历首次调用先返回 root 本身。 */
	if (!pos)
		return root;

	/* visit the first child if exists */
	/* 有 child 时按 children 链顺序下钻到第一个子节点。 */
	next = css_next_child(NULL, pos);
	if (next)
		return next;

	/* no child, visit my or the closest ancestor's next sibling */
	/* 无 child 时逐级回溯，寻找当前节点或最近祖先的下一个 sibling。 */
	while (pos != root) {
		next = css_next_child(pos, pos->parent);
		if (next)
			return next;
		pos = pos->parent;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(css_next_descendant_pre);

/**
 * css_rightmost_descendant - return the rightmost descendant of a css
 * @pos: css of interest
 *
 * Return the rightmost descendant of @pos.  If there's no descendant, @pos
 * is returned.  This can be used during pre-order traversal to skip
 * subtree of @pos.
 *
 * While this function requires cgroup_mutex or RCU read locking, it
 * doesn't require the whole traversal to be contained in a single critical
 * section. Additionally, it isn't necessary to hold onto a reference to @pos.
 * This function will return the correct rightmost descendant as long as @pos
 * is accessible.
 */
/*
 * 中文契约：
 * 在 cgroup_mutex/RCU 下反复取每层最后 child，返回 pos 子树前序遍历中
 * 最后一个节点；无后代返回自身。由于 list prev 不具 RCU 安全性，必须
 * 用 next 完整走到末尾，代价是按每层 child 数线性扫描。
 */
struct cgroup_subsys_state *
css_rightmost_descendant(struct cgroup_subsys_state *pos)
{
	struct cgroup_subsys_state *last, *tmp;

	cgroup_assert_mutex_or_rcu_locked();

	do {
		last = pos;
		/* ->prev isn't RCU safe, walk ->next till the end */
		/* prev 链不满足 RCU 读取协议，只能沿 next 正向扫描到末项。 */
		pos = NULL;
		css_for_each_child(tmp, last)
			pos = tmp;
	} while (pos);

	return last;
}

/*
 * css_leftmost_descendant() - 沿首 child 下钻到最深左叶。
 *
 * 调用者遵循 css_next_child 锁协议；返回后序遍历的首节点，无引用变化。
 */
static struct cgroup_subsys_state *
css_leftmost_descendant(struct cgroup_subsys_state *pos)
{
	struct cgroup_subsys_state *last;

	do {
		last = pos;
		pos = css_next_child(NULL, pos);
	} while (pos);

	return last;
}

/**
 * css_next_descendant_post - find the next descendant for post-order walk
 * @pos: the current position (%NULL to initiate traversal)
 * @root: css whose descendants to walk
 *
 * To be used by css_for_each_descendant_post().  Find the next descendant
 * to visit for post-order traversal of @root's descendants.  @root is
 * included in the iteration and the last node to be visited.
 *
 * While this function requires cgroup_mutex or RCU read locking, it
 * doesn't require the whole traversal to be contained in a single critical
 * section. Additionally, it isn't necessary to hold onto a reference to @pos.
 * This function will return the correct next descendant as long as both @pos
 * and @cgroup are accessible and @pos is a descendant of @cgroup.
 *
 * If a subsystem synchronizes ->css_online() and the start of iteration, a
 * css which finished ->css_online() is guaranteed to be visible in the
 * future iterations and will stay visible until the last reference is put.
 * A css which hasn't finished ->css_online() or already finished
 * ->css_offline() may show up during traversal.  It's each subsystem's
 * responsibility to synchronize against on/offlining.
 */
/*
 * 中文契约：
 * post-order 包含 root 且最后访问 root。首次返回最左叶；之后若有 sibling
 * 则进入其最左叶，否则返回 parent。pos==root 后结束。锁/生命周期和
 * online 状态契约同 child iterator。
 */
struct cgroup_subsys_state *
css_next_descendant_post(struct cgroup_subsys_state *pos,
			 struct cgroup_subsys_state *root)
{
	struct cgroup_subsys_state *next;

	cgroup_assert_mutex_or_rcu_locked();

	/* if first iteration, visit leftmost descendant which may be @root */
	/* 后序首次调用先下钻到最左叶；无 child 时该节点就是 root。 */
	if (!pos)
		return css_leftmost_descendant(root);

	/* if we visited @root, we're done */
	/* root 是后序遍历最后节点，访问后即结束。 */
	if (pos == root)
		return NULL;

	/* if there's an unvisited sibling, visit its leftmost descendant */
	/* 尚有 sibling 时，下一项是该 sibling 子树的最左叶。 */
	next = css_next_child(pos, pos->parent);
	if (next)
		return css_leftmost_descendant(next);

	/* no sibling left, visit parent */
	/* sibling 全部完成后访问 parent，维持 child-before-parent 顺序。 */
	return pos->parent;
}

/**
 * css_has_online_children - does a css have online children
 * @css: the target css
 *
 * Returns %true if @css has any online children; otherwise, %false.  This
 * function can be called from any context but the caller is responsible
 * for synchronizing against on/offlining as necessary.
 */
/*
 * 中文契约：
 * 任意上下文可调用；函数内部 RCU 遍历 child，发现任一 CSS_ONLINE 返回
 * true，否则 false。结果是瞬时快照；RCU 只防释放，若业务需要与
 * online/offline 串行仍应持 subsystem 自己的锁。
 */
bool css_has_online_children(struct cgroup_subsys_state *css)
{
	struct cgroup_subsys_state *child;
	bool ret = false;

	rcu_read_lock();
	css_for_each_child(child, css) {
		if (css_is_online(child)) {
			ret = true;
			break;
		}
	}
	rcu_read_unlock();
	return ret;
}

/*
 * css_task_iter_next_css_set() - 在 iterator 的两级 cset 空间推进。
 *
 * 调用者持 css_set_lock。先耗尽当前 domain cset 的 threaded_csets，再
 * 前进到 css 的 effective-cset 链或 core cgrp_cset_link 链。THREADED
 * 模式切换 domain 时用 cur_dcset 引用钉住 threaded 链表 owner。
 * 返回下一 cset 借用指针或 NULL；引用最终由 iterator end 归还。
 */
static struct css_set *css_task_iter_next_css_set(struct css_task_iter *it)
{
	struct list_head *l;
	struct cgrp_cset_link *link;
	struct css_set *cset;

	lockdep_assert_held(&css_set_lock);

	/* find the next threaded cset */
	/* 在当前 domain 的 threaded_csets 中选择下一组合。 */
	if (it->tcset_pos) {
		l = it->tcset_pos->next;

		if (l != it->tcset_head) {
			it->tcset_pos = l;
			return container_of(l, struct css_set,
					    threaded_csets_node);
		}

		it->tcset_pos = NULL;
	}

	/* find the next cset */
	/* 当前链结束后切换到 effective-cset 空间的下一 css_set。 */
	l = it->cset_pos;
	l = l->next;
	if (l == it->cset_head) {
		it->cset_pos = NULL;
		return NULL;
	}

	if (it->ss) {
		cset = container_of(l, struct css_set, e_cset_node[it->ss->id]);
	} else {
		link = list_entry(l, struct cgrp_cset_link, cset_link);
		cset = link->cset;
	}

	it->cset_pos = l;

	/* initialize threaded css_set walking */
	/* THREADED 模式固定 domain cset，并初始化其 threaded 子组合游标。 */
	if (it->flags & CSS_TASK_ITER_THREADED) {
		if (it->cur_dcset)
			put_css_set_locked(it->cur_dcset);
		it->cur_dcset = cset;
		get_css_set(cset);

		it->tcset_head = &cset->threaded_csets;
		it->tcset_pos = &cset->threaded_csets;
	}

	return cset;
}

/**
 * css_task_iter_advance_css_set - advance a task iterator to the next css_set
 * @it: the iterator to advance
 *
 * Advance @it to the next css_set to walk.
 */
/*
 * 中文契约：
 * css_set_lock 下跳过空 cset，按 tasks -> mg_tasks -> dying_tasks 选择首个
 * 非空成员链。切换时注销旧 cset 的 iterator、归还引用，再持有新 cset
 * 并登记到 task_iters；迁移路径据此能在 task 摘链前推进受影响 iterator。
 */
static void css_task_iter_advance_css_set(struct css_task_iter *it)
{
	struct css_set *cset;

	lockdep_assert_held(&css_set_lock);

	/* Advance to the next non-empty css_set and find first non-empty tasks list*/
	/* 跳过空 cset，并选择 tasks/mg_tasks/dying_tasks 中首条非空链。 */
	while ((cset = css_task_iter_next_css_set(it))) {
		if (!list_empty(&cset->tasks)) {
			it->cur_tasks_head = &cset->tasks;
			break;
		} else if (!list_empty(&cset->mg_tasks)) {
			it->cur_tasks_head = &cset->mg_tasks;
			break;
		} else if (!list_empty(&cset->dying_tasks)) {
			it->cur_tasks_head = &cset->dying_tasks;
			break;
		}
	}
	if (!cset) {
		it->task_pos = NULL;
		return;
	}
	it->task_pos = it->cur_tasks_head->next;

	/*
	 * We don't keep css_sets locked across iteration steps and thus
	 * need to take steps to ensure that iteration can be resumed after
	 * the lock is re-acquired.  Iteration is performed at two levels -
	 * css_sets and tasks in them.
	 *
	 * Once created, a css_set never leaves its cgroup lists, so a
	 * pinned css_set is guaranteed to stay put and we can resume
	 * iteration afterwards.
	 *
	 * Tasks may leave @cset across iteration steps.  This is resolved
	 * by registering each iterator with the css_set currently being
	 * walked and making css_set_move_task() advance iterators whose
	 * next task is leaving.
	 */
	/*
	 * 中文补充：cset 引用保证两次 next 之间链表 owner 存活；task 本身
	 * 可移动，所以 iterator 注册让写者主动修补 task_pos。两者缺一都会
	 * 在解锁窗口产生悬空位置。
	 */
	if (it->cur_cset) {
		list_del(&it->iters_node);
		put_css_set_locked(it->cur_cset);
	}
	get_css_set(cset);
	it->cur_cset = cset;
	list_add(&it->iters_node, &cset->task_iters);
}

/*
 * css_task_iter_skip() - 在 task 离开前修补正指向它的 iterator。
 *
 * css_set_lock 下调用；仅当 task_pos 命中时推进到 next，并设置 SKIPPED，
 * 防止下次正常 advance 再跳过一个 task。返回：无。
 */
static void css_task_iter_skip(struct css_task_iter *it,
			       struct task_struct *task)
{
	lockdep_assert_held(&css_set_lock);

	if (it->task_pos == &task->cg_list) {
		it->task_pos = it->task_pos->next;
		it->flags |= CSS_TASK_ITER_SKIPPED;
	}
}

/*
 * css_task_iter_advance() - 在 cset 和三条 task 链之间推进到下一可见 task。
 *
 * 调用者持 css_set_lock。依次消费 tasks、迁移暂存 mg_tasks、退出暂存
 * dying_tasks，再切换 cset；默认过滤已彻底退出 task，PROCS 还过滤非
 * leader 和无 live 线程的 dying leader，WITH_DEAD 可保留退出对象。
 * 只更新 iterator，不取得 task 引用。
 */
static void css_task_iter_advance(struct css_task_iter *it)
{
	struct task_struct *task;

	lockdep_assert_held(&css_set_lock);
repeat:
	if (it->task_pos) {
		/*
		 * Advance iterator to find next entry. We go through cset
		 * tasks, mg_tasks and dying_tasks, when consumed we move onto
		 * the next cset.
		 */
		/* 依次耗尽 tasks、mg_tasks、dying_tasks，再推进到下一 cset。 */
		if (it->flags & CSS_TASK_ITER_SKIPPED)
			it->flags &= ~CSS_TASK_ITER_SKIPPED;
		else
			it->task_pos = it->task_pos->next;

		if (it->task_pos == &it->cur_cset->tasks) {
			it->cur_tasks_head = &it->cur_cset->mg_tasks;
			it->task_pos = it->cur_tasks_head->next;
		}
		if (it->task_pos == &it->cur_cset->mg_tasks) {
			it->cur_tasks_head = &it->cur_cset->dying_tasks;
			it->task_pos = it->cur_tasks_head->next;
		}
		if (it->task_pos == &it->cur_cset->dying_tasks)
			css_task_iter_advance_css_set(it);
	} else {
		/* called from start, proceed to the first cset */
		/* start 尚无当前位置，先推进到遍历范围内第一个 cset。 */
		css_task_iter_advance_css_set(it);
	}

	if (!it->task_pos)
		return;

	task = list_entry(it->task_pos, struct task_struct, cg_list);
	/*
	 * Hide tasks that are exiting but not yet removed by default. Keep
	 * zombie leaders with live threads visible. Usages that need to walk
	 * every existing task can opt out via CSS_TASK_ITER_WITH_DEAD.
	 */
	/*
	 * 默认隐藏已退出且无 live 线程的 task；仍有活线程的 zombie leader
	 * 保持可见。内部完整扫描可显式请求 WITH_DEAD。
	 */
	if (!(it->flags & CSS_TASK_ITER_WITH_DEAD) &&
	    (task->flags & PF_EXITING) && !atomic_read(&task->signal->live))
		goto repeat;

	if (it->flags & CSS_TASK_ITER_PROCS) {
		/* if PROCS, skip over tasks which aren't group leaders */
		/* 进程视图由 thread-group leader 代表整个进程，跳过普通线程。 */
		if (!thread_group_leader(task))
			goto repeat;

		/* and dying leaders w/o live member threads */
		/* dying leader 已无 live member 时不再代表可见进程。 */
		if (it->cur_tasks_head == &it->cur_cset->dying_tasks &&
		    !atomic_read(&task->signal->live))
			goto repeat;
	} else {
		/* skip all dying ones */
		/* thread 视图默认排除 dying_tasks，避免返回已退出成员。 */
		if (it->cur_tasks_head == &it->cur_cset->dying_tasks)
			goto repeat;
	}
}

/**
 * css_task_iter_start - initiate task iteration
 * @css: the css to walk tasks of
 * @flags: CSS_TASK_ITER_* flags
 * @it: the task iterator to use
 *
 * Initiate iteration through the tasks of @css.  The caller can call
 * css_task_iter_next() to walk through the tasks until the function
 * returns NULL.  On completion of iteration, css_task_iter_end() must be
 * called.
 */
/*
 * 中文契约：
 * css 是锁/引用稳定的遍历目标；flags 选择线程/进程、threaded 子树和
 * dead 可见性；it 是调用者输出对象。函数清零并在 css_set_lock 下选择
 * 起始 cset 链、预推进到首项。返回：无；成功后必须 css_task_iter_end()。
 */
void css_task_iter_start(struct cgroup_subsys_state *css, unsigned int flags,
			 struct css_task_iter *it)
{
	unsigned long irqflags;

	memset(it, 0, sizeof(*it));

	spin_lock_irqsave(&css_set_lock, irqflags);

	it->ss = css->ss;
	it->flags = flags;

	if (CGROUP_HAS_SUBSYS_CONFIG && it->ss)
		it->cset_pos = &css->cgroup->e_csets[css->ss->id];
	else
		it->cset_pos = &css->cgroup->cset_links;

	it->cset_head = it->cset_pos;

	css_task_iter_advance(it);

	spin_unlock_irqrestore(&css_set_lock, irqflags);
}

/**
 * css_task_iter_next - return the next task for the iterator
 * @it: the task iterator being iterated
 *
 * The "next" function for task iteration.  @it should have been
 * initialized via css_task_iter_start().  Returns NULL when the iteration
 * reaches the end.
 */
/*
 * 中文契约：
 * it 已 start。先 put 上次返回 task 引用，锁内处理迁移写者留下的
 * SKIPPED 状态；若有当前项则 get_task_struct 后预推进游标。返回持有引用
 * 的 task（由下次 next 或 end 释放）或 NULL。调用者不能自行 put 返回值。
 */
struct task_struct *css_task_iter_next(struct css_task_iter *it)
{
	unsigned long irqflags;

	if (it->cur_task) {
		put_task_struct(it->cur_task);
		it->cur_task = NULL;
	}

	spin_lock_irqsave(&css_set_lock, irqflags);

	/* @it may be half-advanced by skips, finish advancing */
	/* skip 修补可能留下半推进游标，返回前先恢复到完整下一项。 */
	if (it->flags & CSS_TASK_ITER_SKIPPED)
		css_task_iter_advance(it);

	if (it->task_pos) {
		it->cur_task = list_entry(it->task_pos, struct task_struct,
					  cg_list);
		get_task_struct(it->cur_task);
		css_task_iter_advance(it);
	}

	spin_unlock_irqrestore(&css_set_lock, irqflags);

	return it->cur_task;
}

/**
 * css_task_iter_end - finish task iteration
 * @it: the task iterator to finish
 *
 * Finish task iteration started by css_task_iter_start().
 */
/*
 * 中文契约：
 * 解除 iterator 在 cur_cset->task_iters 的注册，归还 current cset、
 * domain cset 和最后 task 的引用。可在提前终止或遍历结束时调用一次；
 * 返回后 it 中所有借用位置均失效。
 */
void css_task_iter_end(struct css_task_iter *it)
{
	unsigned long irqflags;

	if (it->cur_cset) {
		spin_lock_irqsave(&css_set_lock, irqflags);
		list_del(&it->iters_node);
		put_css_set_locked(it->cur_cset);
		spin_unlock_irqrestore(&css_set_lock, irqflags);
	}

	if (it->cur_dcset)
		put_css_set(it->cur_dcset);

	if (it->cur_task)
		put_task_struct(it->cur_task);
}

/* 若本 fd 已启动 procs iterator，release 时无条件 end 归还全部引用。 */
/*
 * of 在关闭路径稳定，ctx 为 fd 私有对象；函数消费已启动 iterator 的
 * cset/task 引用。返回无直接值，未启动时幂等无副作用。
 */
static void cgroup_procs_release(struct kernfs_open_file *of)
{
	struct cgroup_file_ctx *ctx = of->priv;

	if (ctx->procs.started)
		css_task_iter_end(&ctx->procs.iter);
}

/* seq next 增加逻辑位置并返回 iterator 下一 task；v 参数无需使用。 */
/*
 * s/pos 由 seq_file 借用，v 未使用；返回 css iterator 持引用的下一 task
 * 借用指针或 NULL。函数不转移 task ownership，release/end 统一归还。
 */
static void *cgroup_procs_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct kernfs_open_file *of = s->private;
	struct cgroup_file_ctx *ctx = of->priv;

	if (pos)
		(*pos)++;

	return css_task_iter_next(&ctx->procs.iter);
}

/*
 * __cgroup_procs_start() - 让一个 open fd 的 task iterator 支持 seq seek。
 *
 * 首次 start 必须从 pos 0 建立 iterator；重新从 0 读取时 end 后重启；
 * 非零 pos 的 seq_file seek 会由框架从头顺序调用，因此可复用当前 task。
 * iter_flags 区分 procs/threads。返回首 task、当前 task 或 ERR_PTR(-EINVAL)。
 */
static void *__cgroup_procs_start(struct seq_file *s, loff_t *pos,
				  unsigned int iter_flags)
{
	struct kernfs_open_file *of = s->private;
	struct cgroup *cgrp = seq_css(s)->cgroup;
	struct cgroup_file_ctx *ctx = of->priv;
	struct css_task_iter *it = &ctx->procs.iter;

	/*
	 * When a seq_file is seeked, it's always traversed sequentially
	 * from position 0, so we can simply keep iterating on !0 *pos.
	 */
	/*
	 * ctx->procs.started 把 iterator 生命周期绑定到 fd 而非单次 read，
	 * release 即使用户中途停止读取也能完成清理。
	 */
	if (!ctx->procs.started) {
		if (WARN_ON_ONCE((*pos)))
			return ERR_PTR(-EINVAL);
		css_task_iter_start(&cgrp->self, iter_flags, it);
		ctx->procs.started = true;
	} else if (!(*pos)) {
		css_task_iter_end(it);
		css_task_iter_start(&cgrp->self, iter_flags, it);
	} else
		return it->cur_task;

	return cgroup_procs_next(s, NULL, NULL);
}

/*
 * cgroup_procs_start() - 以进程 leader 粒度启动含 threaded cset 的遍历。
 *
 * threaded 子树内部只有 thread 可分散，全部 process 归 domain cgroup，
 * 因而在 threaded 节点读取 procs 返回 -EOPNOTSUPP；domain 节点进入共同
 * iterator。
 */
static void *cgroup_procs_start(struct seq_file *s, loff_t *pos)
{
	struct cgroup *cgrp = seq_css(s)->cgroup;

	/*
	 * All processes of a threaded subtree belong to the domain cgroup
	 * of the subtree.  Only threads can be distributed across the
	 * subtree.  Reject reads on cgroup.procs in the subtree proper.
	 * They're always empty anyway.
	 */
	/*
	 * threaded 子树的进程统一属于 domain cgroup，仅 thread 可分散；
	 * 因而内部 cgroup.procs 逻辑为空并返回不支持。
	 */
	if (cgroup_is_threaded(cgrp))
		return ERR_PTR(-EOPNOTSUPP);

	return __cgroup_procs_start(s, pos, CSS_TASK_ITER_PROCS |
					    CSS_TASK_ITER_THREADED);
}

/* 输出 task 在 reader PID namespace 中的可见 PID；task 引用由 iterator 持有。 */
/*
 * s 是输出，v 是当前 task 借用；按 current PID namespace 格式化后返回 0。
 * 不改变 iterator 位置、task 引用或 cgroup 状态。
 */
static int cgroup_procs_show(struct seq_file *s, void *v)
{
	seq_printf(s, "%d\n", task_pid_vnr(v));
	return 0;
}

/*
 * cgroup_may_write() - 用 VFS 权限模型检查 current 能否写 cgroup.procs。
 *
 * 调用者持 cgroup_mutex；从 cgrp 的 kernfs node 取得临时 inode，按
 * nop_mnt_idmap 检查 MAY_WRITE 后 iput。返回权限 errno；inode 分配失败
 * 返回 -ENOMEM。该检查不迁移 task。
 */
static int cgroup_may_write(const struct cgroup *cgrp, struct super_block *sb)
{
	int ret;
	struct inode *inode;

	lockdep_assert_held(&cgroup_mutex);

	inode = kernfs_get_inode(sb, cgrp->procs_file.kn);
	if (!inode)
		return -ENOMEM;

	ret = inode_permission(&nop_mnt_idmap, inode, MAY_WRITE);
	iput(inode);
	return ret;
}

/*
 * cgroup_procs_write_permission() - 校验 v2 delegation 的共同祖先规则。
 *
 * 调用者持 cgroup_mutex；src/dst/ns/sb 均为借用。迁移者必须能写 source
 * 与 destination 的最低共同祖先 procs 文件，防止仅控制目标的一方“偷走”
 * task；NS_DELEGATE 下两端还必须在调用者 namespace 根可见。返回 0、
 * VFS权限 errno 或 -ENOENT，无副作用。
 */
static int cgroup_procs_write_permission(struct cgroup *src_cgrp,
					 struct cgroup *dst_cgrp,
					 struct super_block *sb,
					 struct cgroup_namespace *ns)
{
	struct cgroup *com_cgrp = src_cgrp;
	int ret;

	lockdep_assert_held(&cgroup_mutex);

	/* find the common ancestor */
	/* 最低共同祖先是跨分支迁移需要写权限的 delegation 边界。 */
	while (!cgroup_is_descendant(dst_cgrp, com_cgrp))
		com_cgrp = cgroup_parent(com_cgrp);

	/* %current should be authorized to migrate to the common ancestor */
	/* current 必须能写共同祖先的 procs 文件，才可改变两侧成员关系。 */
	ret = cgroup_may_write(com_cgrp, sb);
	if (ret)
		return ret;

	/*
	 * If namespaces are delegation boundaries, %current must be able
	 * to see both source and destination cgroups from its namespace.
	 */
	/* delegation 模式要求 source/destination 都位于 current 可见子树。 */
	if ((cgrp_dfl_root.flags & CGRP_ROOT_NS_DELEGATE) &&
	    (!cgroup_is_descendant(src_cgrp, ns->root_cset->dfl_cgrp) ||
	     !cgroup_is_descendant(dst_cgrp, ns->root_cset->dfl_cgrp)))
		return -ENOENT;

	return 0;
}

/*
 * cgroup_attach_permissions() - 汇总 delegation、目标域和线程迁移约束。
 *
 * 先检查共同祖先写权限，再验证 dst 可承载 task。单线程迁移不得跨
 * dom_cgrp，否则同一进程的线程会跨资源 domain，返回 -EOPNOTSUPP。
 */
static int cgroup_attach_permissions(struct cgroup *src_cgrp,
				     struct cgroup *dst_cgrp,
				     struct super_block *sb, bool threadgroup,
				     struct cgroup_namespace *ns)
{
	int ret = 0;

	ret = cgroup_procs_write_permission(src_cgrp, dst_cgrp, sb, ns);
	if (ret)
		return ret;

	ret = cgroup_migrate_vet_dst(dst_cgrp);
	if (ret)
		return ret;

	if (!threadgroup && (src_cgrp->dom_cgrp != dst_cgrp->dom_cgrp))
		ret = -EOPNOTSUPP;

	return ret;
}

/*
 * __cgroup_procs_write() - cgroup.procs/threads 的共同迁移入口。
 *
 * 先锁定目标 cgroup，再解析 PID、持有 task 引用并按需锁 threadgroup/
 * CPU hotplug；css_set_lock 下取得 source。权限检查临时采用打开 fd 时
 * 的 f_cred，而非 write 时 current 凭据，阻止高权限打开后把 fd 交给
 * 无权限进程的继承攻击。通过后调用全有或全无 attach。
 *
 * 所有出口逆序释放 task/attach 锁、cgroup_mutex 和 cgroup 引用。返回
 * 0 或精确 errno；wrapper 成功时转换为 nbytes。
 */
static ssize_t __cgroup_procs_write(struct kernfs_open_file *of, char *buf,
				    bool threadgroup)
{
	struct cgroup_file_ctx *ctx = of->priv;
	struct cgroup *src_cgrp, *dst_cgrp;
	struct task_struct *task;
	ssize_t ret;
	enum cgroup_attach_lock_mode lock_mode;

	dst_cgrp = cgroup_kn_lock_live(of->kn, false);
	if (!dst_cgrp)
		return -ENODEV;

	task = cgroup_procs_write_start(buf, threadgroup, &lock_mode);
	ret = PTR_ERR_OR_ZERO(task);
	if (ret)
		goto out_unlock;

	/* find the source cgroup */
	/* 从 task 当前 cset 解析 default hierarchy 源节点，供权限比较。 */
	spin_lock_irq(&css_set_lock);
	src_cgrp = task_cgroup_from_root(task, &cgrp_dfl_root);
	spin_unlock_irq(&css_set_lock);

	/*
	 * Process and thread migrations follow same delegation rule. Check
	 * permissions using the credentials from file open to protect against
	 * inherited fd attacks.
	 */
	/*
	 * scoped_with_creds 离开作用域自动恢复原凭据，权限判断期间即使失败
	 * goto 也不会泄漏 override creds。
	 */
	scoped_with_creds(of->file->f_cred)
		ret = cgroup_attach_permissions(src_cgrp, dst_cgrp,
						of->file->f_path.dentry->d_sb,
						threadgroup, ctx->ns);
	if (ret)
		goto out_finish;

	ret = cgroup_attach_task(dst_cgrp, task, threadgroup);

out_finish:
	cgroup_procs_write_finish(task, lock_mode);
out_unlock:
	cgroup_kn_unlock(of->kn);

	return ret;
}

/* procs wrapper 固定 threadgroup=true，并把共同入口的 0 转成完整写入长度。 */
/*
 * of/buf/nbytes/off 为 kernfs 写入借用，off 不参与迁移。成功迁移整个
 * threadgroup 返回 nbytes；解析、权限、目标或 controller 错误原样返回。
 */
static ssize_t cgroup_procs_write(struct kernfs_open_file *of,
				  char *buf, size_t nbytes, loff_t off)
{
	return __cgroup_procs_write(of, buf, true) ?: nbytes;
}

/* threads reader 使用 task 粒度 flags，不跨 threaded cset 汇总进程 leader。 */
/*
 * s/pos 为 seq_file 借用；返回首个 thread task、NULL 或错误指针。
 * iterator ownership 绑定 fd，并由 cgroup_procs_release() 结束。
 */
static void *cgroup_threads_start(struct seq_file *s, loff_t *pos)
{
	return __cgroup_procs_start(s, pos, 0);
}

/* threads writer 固定 threadgroup=false；权限层额外禁止跨 domain 移单线程。 */
/*
 * 写入参数契约同 procs wrapper；成功只迁移指定 thread 并返回 nbytes，
 * 跨 domain、权限、PID 或迁移回调失败返回精确 errno。
 */
static ssize_t cgroup_threads_write(struct kernfs_open_file *of,
				    char *buf, size_t nbytes, loff_t off)
{
	return __cgroup_procs_write(of, buf, false) ?: nbytes;
}

/* cgroup core interface files for the default hierarchy */
/*
 * v2 core cftype 表把用户 ABI 名称映射到上述 show/write/iterator 回调。
 * 静态数组以空 name 结尾，cgroup core 在 css 可见时逐项实例化；flags
 * 决定 root 可见性和 namespace delegation，file_offset 连接通知对象。
 */
static struct cftype cgroup_base_files[] = {
	{
		.name = "cgroup.type",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cgroup_type_show,
		.write = cgroup_type_write,
	},
	{
		.name = "cgroup.procs",
		.flags = CFTYPE_NS_DELEGATABLE,
		.file_offset = offsetof(struct cgroup, procs_file),
		.release = cgroup_procs_release,
		.seq_start = cgroup_procs_start,
		.seq_next = cgroup_procs_next,
		.seq_show = cgroup_procs_show,
		.write = cgroup_procs_write,
	},
	{
		.name = "cgroup.threads",
		.flags = CFTYPE_NS_DELEGATABLE,
		.release = cgroup_procs_release,
		.seq_start = cgroup_threads_start,
		.seq_next = cgroup_procs_next,
		.seq_show = cgroup_procs_show,
		.write = cgroup_threads_write,
	},
	{
		.name = "cgroup.controllers",
		.seq_show = cgroup_controllers_show,
	},
	{
		.name = "cgroup.subtree_control",
		.flags = CFTYPE_NS_DELEGATABLE,
		.seq_show = cgroup_subtree_control_show,
		.write = cgroup_subtree_control_write,
	},
	{
		.name = "cgroup.events",
		.flags = CFTYPE_NOT_ON_ROOT,
		.file_offset = offsetof(struct cgroup, events_file),
		.seq_show = cgroup_events_show,
	},
	{
		.name = "cgroup.max.descendants",
		.seq_show = cgroup_max_descendants_show,
		.write = cgroup_max_descendants_write,
	},
	{
		.name = "cgroup.max.depth",
		.seq_show = cgroup_max_depth_show,
		.write = cgroup_max_depth_write,
	},
	{
		.name = "cgroup.stat",
		.seq_show = cgroup_stat_show,
	},
	{
		.name = "cgroup.stat.local",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cgroup_core_local_stat_show,
	},
	{
		.name = "cgroup.freeze",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cgroup_freeze_show,
		.write = cgroup_freeze_write,
	},
	{
		.name = "cgroup.kill",
		.flags = CFTYPE_NOT_ON_ROOT,
		.write = cgroup_kill_write,
	},
	{
		.name = "cpu.stat",
		.seq_show = cpu_stat_show,
	},
	{
		.name = "cpu.stat.local",
		.seq_show = cpu_local_stat_show,
	},
	{ }	/* terminate */
	/* 空 name 项终止 cgroup_base_files 遍历，不会实例化文件。 */
};

static struct cftype cgroup_psi_files[] = {
#ifdef CONFIG_PSI
	{
		.name = "io.pressure",
		.file_offset = offsetof(struct cgroup, psi_files[PSI_IO]),
		.seq_show = cgroup_io_pressure_show,
		.write = cgroup_io_pressure_write,
		.poll = cgroup_pressure_poll,
		.release = cgroup_pressure_release,
	},
	{
		.name = "memory.pressure",
		.file_offset = offsetof(struct cgroup, psi_files[PSI_MEM]),
		.seq_show = cgroup_memory_pressure_show,
		.write = cgroup_memory_pressure_write,
		.poll = cgroup_pressure_poll,
		.release = cgroup_pressure_release,
	},
	{
		.name = "cpu.pressure",
		.file_offset = offsetof(struct cgroup, psi_files[PSI_CPU]),
		.seq_show = cgroup_cpu_pressure_show,
		.write = cgroup_cpu_pressure_write,
		.poll = cgroup_pressure_poll,
		.release = cgroup_pressure_release,
	},
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	{
		.name = "irq.pressure",
		.file_offset = offsetof(struct cgroup, psi_files[PSI_IRQ]),
		.seq_show = cgroup_irq_pressure_show,
		.write = cgroup_irq_pressure_write,
		.poll = cgroup_pressure_poll,
		.release = cgroup_pressure_release,
	},
#endif
	{
		.name = "cgroup.pressure",
		.seq_show = cgroup_pressure_show,
		.write = cgroup_pressure_write,
	},
#endif /* CONFIG_PSI */
	/* CONFIG_PSI=n 时不编译上述资源项，只保留终止哨兵。 */
	{ }	/* terminate */
	/* 空 name 项终止 cgroup_psi_files 遍历。 */
};

/*
 * css destruction is four-stage process.
 *
 * 1. Destruction starts.  Killing of the percpu_ref is initiated.
 *    Implemented in kill_css_finish().
 *
 * 2. When the percpu_ref is confirmed to be visible as killed on all CPUs
 *    and thus css_tryget_online() is guaranteed to fail, the css can be
 *    offlined by invoking offline_css().  After offlining, the base ref is
 *    put.  Implemented in css_killed_work_fn().
 *
 * 3. When the percpu_ref reaches zero, the only possible remaining
 *    accessors are inside RCU read sections.  css_release() schedules the
 *    RCU callback.
 *
 * 4. After the grace period, the css can be freed.  Implemented in
 *    css_free_rwork_fn().
 *
 * It is actually hairier because both step 2 and 4 require process context
 * and thus involve punting to css->destroy_work adding two additional
 * steps to the already complex sequence.
 */
/*
 * 中文生命周期总图：
 * 1. kill_css_sync 标记 CSS_DYING、隐藏文件并更新层级计数；
 * 2. populated 归零后 kill_css_finish kill percpu_ref，禁止新 online 引用；
 * 3. kill confirm 在进程上下文调用 css_offline，并归还 base ref；
 * 4. refcount 到零进入 css_release_work，从树/IDR 可见索引摘除；
 * 5. RCU 宽限期后 css_free_rwork_fn 才释放 controller/cgroup 内存。
 *
 * workqueue 与 rcu_work 的额外跳转不是冗余：offline/free 回调可能睡眠，
 * 而 percpu_ref confirm、RCU callback 所在上下文不能直接执行这些操作。
 */
/*
 * css_free_rwork_fn() - RCU 宽限期后的最终物理释放阶段。
 *
 * work 内嵌于 css，进入时对象已从 sibling/IDR 可发现路径摘除，且没有
 * 引用或旧 RCU 读者。先销毁 percpu_ref/rstat。controller css 调
 * css_free、删除 ID、归还 cgroup/parent 引用；self css 则销毁 cgroup
 * 附属资源，非 root 最终 kfree，root 转入 cgroup_destroy_root。
 * 函数消费对象最后 ownership，无返回。
 */
static void css_free_rwork_fn(struct work_struct *work)
{
	struct cgroup_subsys_state *css = container_of(to_rcu_work(work),
				struct cgroup_subsys_state, destroy_rwork);
	struct cgroup_subsys *ss = css->ss;
	struct cgroup *cgrp = css->cgroup;

	percpu_ref_exit(&css->refcnt);
	css_rstat_exit(css);

	if (!css_is_self(css)) {
		/* css free path */
		/* subsystem 回调真正释放包含 css 的具体控制器对象。 */
		struct cgroup_subsys_state *parent = css->parent;
		int id = css->id;

		ss->css_free(css);
		cgroup_idr_remove(&ss->css_idr, id);
		cgroup_put(cgrp);

		if (parent)
			css_put(parent);
	} else {
		/* cgroup free path */
		/* self css 与 cgroup 共用存储期，此分支负责整个目录对象。 */
		atomic_dec(&cgrp->root->nr_cgrps);
		if (!cgroup_on_dfl(cgrp))
			cgroup1_pidlist_destroy_all(cgrp);
		cancel_work_sync(&cgrp->release_agent_work);
		bpf_cgrp_storage_free(cgrp);

		if (cgroup_parent(cgrp)) {
			/*
			 * We get a ref to the parent, and put the ref when
			 * this cgroup is being freed, so it's guaranteed
			 * that the parent won't be destroyed before its
			 * children.
			 */
			/*
			 * child 创建时持有 parent 引用，直到此处才归还。
			 * 即使 parent 先 rmdir，其内存也必须晚于最后 child。
			 */
			cgroup_put(cgroup_parent(cgrp));
			kernfs_put(cgrp->kn);
			psi_cgroup_free(cgrp);
			kfree(cgrp);
		} else {
			/*
			 * This is root cgroup's refcnt reaching zero,
			 * which indicates that the root should be
			 * released.
			 */
			/*
			 * root 的 self ref 到零表示卸载引用已排空，进入 root
			 * 销毁。
			 */
			cgroup_destroy_root(cgrp->root);
		}
	}
}

/*
 * css_release_work_fn() - percpu_ref 到零后的全局摘除阶段。
 *
 * release workqueue 进程上下文取得 cgroup_mutex，设置 CSS_RELEASED 并从
 * sibling 树 RCU 摘除。controller css 清 IDR、调用 css_released 并维护
 * dying 计数；self css 清 kernfs priv 后门和 ancestor dying 计数。
 * 解锁后排队 RCU work，宽限期结束才最终 free。
 */
static void css_release_work_fn(struct work_struct *work)
{
	struct cgroup_subsys_state *css =
		container_of(work, struct cgroup_subsys_state, destroy_work);
	struct cgroup_subsys *ss = css->ss;
	struct cgroup *cgrp = css->cgroup;

	cgroup_lock();

	css->flags |= CSS_RELEASED;
	list_del_rcu(&css->sibling);

	if (!css_is_self(css)) {
		struct cgroup *parent_cgrp;

		css_rstat_flush(css);

		cgroup_idr_replace(&ss->css_idr, NULL, css->id);
		if (ss->css_released)
			ss->css_released(css);

		cgrp->nr_dying_subsys[ss->id]--;
		/*
		 * When a css is released and ready to be freed, its
		 * nr_descendants must be zero. However, the corresponding
		 * cgrp->nr_dying_subsys[ss->id] may not be 0 if a subsystem
		 * is activated and deactivated multiple times with one or
		 * more of its previous activation leaving behind dying csses.
		 */
		/*
		 * 当前 css 可释放时自身后代必为零；但 controller 多次启停可能
		 * 留下更早世代 dying css，所以 cgrp 聚合计数仍可非零。
		 */
		WARN_ON_ONCE(css->nr_descendants);
		parent_cgrp = cgroup_parent(cgrp);
		while (parent_cgrp) {
			parent_cgrp->nr_dying_subsys[ss->id]--;
			parent_cgrp = cgroup_parent(parent_cgrp);
		}
	} else {
		struct cgroup *tcgrp;

		/* cgroup release path */
		/* 到此用户目录早已删除；trace 表示内核引用也已归零。 */
		TRACE_CGROUP_PATH(release, cgrp);

		css_rstat_flush(&cgrp->self);

		spin_lock_irq(&css_set_lock);
		for (tcgrp = cgroup_parent(cgrp); tcgrp;
		     tcgrp = cgroup_parent(tcgrp))
			tcgrp->nr_dying_descendants--;
		spin_unlock_irq(&css_set_lock);

		/*
		 * There are two control paths which try to determine
		 * cgroup from dentry without going through kernfs -
		 * cgroupstats_build() and css_tryget_online_from_dir().
		 * Those are supported by RCU protecting clearing of
		 * cgrp->kn->priv backpointer.
		 */
		/*
		 * 两条历史查询路径不持 kernfs active ref，只在 RCU 下读 priv；
		 * 先清 backpointer，再等待后续 RCU work，避免返回待释放 cgrp。
		 */
		if (cgrp->kn)
			RCU_INIT_POINTER(*(void __rcu __force **)&cgrp->kn->priv,
					 NULL);
	}

	cgroup_unlock();

	INIT_RCU_WORK(&css->destroy_rwork, css_free_rwork_fn);
	queue_rcu_work(cgroup_free_wq, &css->destroy_rwork);
}

/*
 * css_release() - percpu_ref 的零引用回调。
 *
 * ref 唯一定位 css；当前上下文不保证可睡眠，故只初始化并排队 release
 * work。workqueue 接管 css 存活，返回：无。
 */
static void css_release(struct percpu_ref *ref)
{
	struct cgroup_subsys_state *css =
		container_of(ref, struct cgroup_subsys_state, refcnt);

	INIT_WORK(&css->destroy_work, css_release_work_fn);
	queue_work(cgroup_release_wq, &css->destroy_work);
}

/*
 * Deferred kill_css_finish() fired from css_update_populated() once a dying
 * css's hierarchical populated state drops to zero. Pinned by css_get() at the
 * queue site; matched by css_put() here.
 */
/*
 * 中文契约：populated 路径为 dying css 取得额外引用后排队本 work。
 * worker 在 cgroup_mutex 下执行 finish，再 css_put() 精确归还排队引用。
 */
static void kill_css_finish_work_fn(struct work_struct *work)
{
	struct cgroup_subsys_state *css =
		container_of(work, struct cgroup_subsys_state, kill_finish_work);

	cgroup_lock();
	kill_css_finish(css);
	cgroup_unlock();
	css_put(css);
}

/*
 * init_and_link_css() - 初始化 controller css 的 core 字段和父引用。
 *
 * 调用者持 cgroup_mutex；css 是 subsystem 新分配且尚未发布的对象。函数
 * 先取得 cgroup live 引用，再清零/设置 ss、id 哨兵、链表、work、单调
 * serial 和 online_cnt；非 root 还取得 parent css 引用。尚不接入
 * cgrp->subsys/children，也不初始化 percpu_ref。
 */
static void init_and_link_css(struct cgroup_subsys_state *css,
			      struct cgroup_subsys *ss, struct cgroup *cgrp)
{
	lockdep_assert_held(&cgroup_mutex);

	cgroup_get_live(cgrp);

	memset(css, 0, sizeof(*css));
	css->cgroup = cgrp;
	css->ss = ss;
	css->id = -1;
	INIT_LIST_HEAD(&css->sibling);
	INIT_LIST_HEAD(&css->children);
	INIT_WORK(&css->kill_finish_work, kill_css_finish_work_fn);
	css->serial_nr = css_serial_nr_next++;
	atomic_set(&css->online_cnt, 0);

	if (cgroup_parent(cgrp)) {
		css->parent = cgroup_css(cgroup_parent(cgrp), ss);
		css_get(css->parent);
	}

	BUG_ON(cgroup_css(cgrp, ss));
}

/* invoke ->css_online() on a new CSS and mark it online if successful */
/*
 * online_css() - 调 controller online 回调并发布 css。
 *
 * 调用者持 cgroup_mutex。回调成功后先设 CSS_ONLINE，再以
 * rcu_assign_pointer 发布到 cgrp->subsys，并增加自身/parent online_cnt
 * 与祖先 descendant 计数。成功返回 0；回调 errno 时一个字段也不发布。
 */
static int online_css(struct cgroup_subsys_state *css)
{
	struct cgroup_subsys *ss = css->ss;
	int ret = 0;

	lockdep_assert_held(&cgroup_mutex);

	if (ss->css_online)
		ret = ss->css_online(css);
	if (!ret) {
		css->flags |= CSS_ONLINE;
		rcu_assign_pointer(css->cgroup->subsys[ss->id], css);

		atomic_inc(&css->online_cnt);
		if (css->parent) {
			atomic_inc(&css->parent->online_cnt);
			while ((css = css->parent))
				css->nr_descendants++;
		}
	}
	return ret;
}

/* if the CSS is online, invoke ->css_offline() on it and mark it offline */
/*
 * offline_css() - 让已被 kill confirm 的 css 离线。
 *
 * 调用者持 cgroup_mutex。非 online 幂等返回；否则先让 subsystem 停止
 * 资源控制，再清 ONLINE 并从 cgrp->subsys 断开，最后唤醒 drain 等待者。
 * 到此不能再由普通关联查到 css，但内存仍等待引用/RCU 回收。
 */
static void offline_css(struct cgroup_subsys_state *css)
{
	struct cgroup_subsys *ss = css->ss;

	lockdep_assert_held(&cgroup_mutex);

	if (!css_is_online(css))
		return;

	if (ss->css_offline)
		ss->css_offline(css);

	css->flags &= ~CSS_ONLINE;
	RCU_INIT_POINTER(css->cgroup->subsys[ss->id], NULL);

	wake_up_all(&css->cgroup->offline_waitq);
}

/**
 * css_create - create a cgroup_subsys_state
 * @cgrp: the cgroup new css will be associated with
 * @ss: the subsys of new css
 *
 * Create a new css associated with @cgrp - @ss pair.  On success, the new
 * css is online and installed in @cgrp.  This function doesn't create the
 * interface files.  Returns 0 on success, -errno on failure.
 */
/*
 * 中文契约：
 * 调用者持 cgroup_mutex；cgrp/ss 稳定且当前组合无 css。函数可睡眠调用
 * css_alloc，完成 core/percpu_ref/IDR/rstat 初始化，随后把 sibling 和 IDR
 * 占位替换发布，再 online。
 *
 * 成功返回 online css 借用指针，cgroup/parent/base ref 由生命周期链持有；
 * 失败返回 ERR_PTR(errno)，已取得资源统一排入 RCU free work，避免已发布
 * sibling 的旧读者 UAF。本函数不创建接口文件。
 */
static struct cgroup_subsys_state *css_create(struct cgroup *cgrp,
					      struct cgroup_subsys *ss)
{
	struct cgroup *parent = cgroup_parent(cgrp);
	struct cgroup_subsys_state *parent_css = cgroup_css(parent, ss);
	struct cgroup_subsys_state *css;
	int err;

	lockdep_assert_held(&cgroup_mutex);

	/*
	 * 阶段 1：只让 controller 构造私有 css，并补齐核心身份关系。
	 * 此时对象尚未进入 sibling/IDR，失败可直接走统一异步释放路径。
	 */
	css = ss->css_alloc(parent_css);
	if (!css)
		css = ERR_PTR(-ENOMEM);
	if (IS_ERR(css))
		return css;

	init_and_link_css(css, ss, cgrp);

	/*
	 * 阶段 2：依次建立引用计数、全局 ID 与 rstat。后一步失败时，
	 * css_free_rwork_fn() 依据初始化状态释放此前成功取得的资源。
	 */
	err = percpu_ref_init(&css->refcnt, css_release, 0, GFP_KERNEL);
	if (err)
		goto err_free_css;

	err = cgroup_idr_alloc(&ss->css_idr, NULL, 2, 0, GFP_KERNEL);
	if (err < 0)
		goto err_free_css;
	css->id = err;

	err = css_rstat_init(css);
	if (err)
		goto err_free_css;

	/* @css is ready to be brought online now, make it visible */
	/*
	 * sibling/IDR 是预发布点；online 成功后 cgrp->subsys 才成为正式关联。
	 * online 失败必须 list_del_rcu，并等待宽限期释放。
	 */
	list_add_tail_rcu(&css->sibling, &parent_css->children);
	cgroup_idr_replace(&ss->css_idr, css, css->id);

	err = online_css(css);
	if (err)
		goto err_list_del;

	return css;

err_list_del:
	/* online 失败：先撤销读者可见的 sibling 链，再延迟回收对象。 */
	list_del_rcu(&css->sibling);
err_free_css:
	/*
	 * 即使尚未发布，controller 的 css_free 也可能要求 RCU 上下文；
	 * 因而所有构造失败统一排入 cgroup_free_wq，而不在当前栈同步 free。
	 */
	INIT_RCU_WORK(&css->destroy_rwork, css_free_rwork_fn);
	queue_rcu_work(cgroup_free_wq, &css->destroy_rwork);
	return ERR_PTR(err);
}

/*
 * The returned cgroup is fully initialized including its control mask, but
 * it doesn't have the control mask applied.
 */
/*
 * 中文契约：
 * 调用者持 cgroup_mutex；parent 存活，name/mode 来自 kernfs mkdir。
 * 函数可睡眠分配含 ancestors 柔性区的 cgroup，初始化 self ref、未激活
 * kernfs 目录、rstat/PSI、祖先缓存、freezer 与 v1 继承标志，并调用
 * lifetime ONLINE notifier。
 *
 * notifier 成功后进入提交点：更新祖先计数，RCU 接入 children、增加 root
 * 计数并取得 parent 引用。成功返回尚未 apply 控制器的完整 cgroup；
 * 失败返回 ERR_PTR(errno)，按 PSI、rstat、kernfs、ref、内存逆序回滚。
 */
static struct cgroup *cgroup_create(struct cgroup *parent, const char *name,
				    umode_t mode)
{
	struct cgroup_root *root = parent->root;
	struct cgroup *cgrp, *tcgrp;
	struct kernfs_node *kn;
	int i, level = parent->level + 1;
	int ret;

	/* allocate the cgroup and its ID, 0 is reserved for the root */
	/*
	 * 分配本体和按 level 索引的 ancestors 柔性数组；普通节点不使用 root
	 * 保留 ID 0。当前只有内存 ownership，尚未创建名字或对外发布。
	 */
	cgrp = kzalloc_flex(*cgrp, _low_ancestors, level);
	if (!cgrp)
		return ERR_PTR(-ENOMEM);

	ret = percpu_ref_init(&cgrp->self.refcnt, css_release, 0, GFP_KERNEL);
	if (ret)
		goto out_free_cgrp;

	/* create the directory */
	/*
	 * 创建未激活 kernfs 目录并把 priv 指向 cgrp；失败时没有目录引用可
	 * 回收。成功后 kn 由本构造事务持有，尚不能被普通用户操作。
	 */
	kn = kernfs_create_dir_ns(parent->kn, name, mode,
				  current_fsuid(), current_fsgid(),
				  cgrp, NULL);
	if (IS_ERR(kn)) {
		ret = PTR_ERR(kn);
		goto out_cancel_ref;
	}
	cgrp->kn = kn;

	init_cgroup_housekeeping(cgrp);

	cgrp->self.parent = &parent->self;
	cgrp->root = root;
	cgrp->level = level;

	/*
	 * Now that init_cgroup_housekeeping() has been called and cgrp->self
	 * is setup, it is safe to perform rstat initialization on it.
	 */
	/*
	 * housekeeping 已建立 self css 的锁、链表和基础字段，rstat 才能把
	 * per-CPU 统计与该 css 关联；失败仍处于未发布构造阶段。
	 */
	ret = css_rstat_init(&cgrp->self);
	if (ret)
		goto out_kernfs_remove;

	ret = psi_cgroup_alloc(cgrp);
	if (ret)
		goto out_stat_exit;

	for (tcgrp = cgrp; tcgrp; tcgrp = cgroup_parent(tcgrp))
		cgrp->ancestors[tcgrp->level] = tcgrp;

	/*
	 * New cgroup inherits effective freeze counter, and
	 * if the parent has to be frozen, the child has too.
	 */
	/*
	 * 新节点继承父节点 effective freeze 深度，保证冻结子树中新建目录
	 * 不能绕过冻结；此时尚无 task，状态可直接初始化为 frozen。
	 */
	cgrp->freezer.e_freeze = parent->freezer.e_freeze;
	seqcount_spinlock_init(&cgrp->freezer.freeze_seq, &css_set_lock);
	if (cgrp->freezer.e_freeze) {
		/*
		 * Set the CGRP_FREEZE flag, so when a process will be
		 * attached to the child cgroup, it will become frozen.
		 * At this point the new cgroup is unpopulated, so we can
		 * consider it frozen immediately.
		 */
		/*
		 * 先发布 FREEZE 要求，使未来 attach 的 task 进入 freezer；
		 * 当前为空，因此同时置 FROZEN 并记录起点不会产生虚假
		 * 成员遗漏。
		 */
		set_bit(CGRP_FREEZE, &cgrp->flags);
		cgrp->freezer.freeze_start_nsec = ktime_get_ns();
		set_bit(CGRP_FROZEN, &cgrp->flags);
	}

	if (notify_on_release(parent))
		set_bit(CGRP_NOTIFY_ON_RELEASE, &cgrp->flags);

	if (test_bit(CGRP_CPUSET_CLONE_CHILDREN, &parent->flags))
		set_bit(CGRP_CPUSET_CLONE_CHILDREN, &cgrp->flags);

	cgrp->self.serial_nr = css_serial_nr_next++;

	ret = blocking_notifier_call_chain_robust(&cgroup_lifetime_notifier,
						  CGROUP_LIFETIME_ONLINE,
						  CGROUP_LIFETIME_OFFLINE, cgrp);
	ret = notifier_to_errno(ret);
	if (ret)
		goto out_psi_free;

	/* allocation complete, commit to creation */
	/*
	 * 所有可失败初始化已完成，下面把对象接入祖先计数与 RCU
	 * children 链。
	 */
	/*
	 * notifier 已接受对象，此后必须形成可由统一销毁链回收的 cgroup，
	 * 不能再走简单 kfree 回滚。
	 */
	spin_lock_irq(&css_set_lock);
	for (i = 0; i < level; i++) {
		tcgrp = cgrp->ancestors[i];
		tcgrp->nr_descendants++;

		/*
		 * If the new cgroup is frozen, all ancestor cgroups get a new
		 * frozen descendant, but their state can't change because of
		 * this.
		 */
			/*
			 * frozen descendant 数随新节点递增；祖先自身冻结
			 * 布尔状态由 task/populated 条件决定，不因空后代
			 * 创建而重新转换。
			 */
		if (cgrp->freezer.e_freeze)
			tcgrp->freezer.nr_frozen_descendants++;
	}
	spin_unlock_irq(&css_set_lock);

	list_add_tail_rcu(&cgrp->self.sibling, &cgroup_parent(cgrp)->self.children);
	atomic_inc(&root->nr_cgrps);
	cgroup_get_live(parent);

	/*
	 * On the default hierarchy, a child doesn't automatically inherit
	 * subtree_control from the parent.  Each is configured manually.
	 */
	/*
	 * v2 的下放是逐层显式管理，新 child 初始不继续下放 controller；
	 * v1 则沿层级继承当前可控制集合以维持 legacy 语义。
	 */
	if (!cgroup_on_dfl(cgrp))
		cgrp->subtree_control = cgroup_control(cgrp);

	cgroup_propagate_control(cgrp);

	return cgrp;

out_psi_free:
	/* 创建未提交：按资源获取的逆序撤销，此时 parent 引用尚未取得。 */
	psi_cgroup_free(cgrp);
out_stat_exit:
	css_rstat_exit(&cgrp->self);
out_kernfs_remove:
	kernfs_remove(cgrp->kn);
out_cancel_ref:
	percpu_ref_exit(&cgrp->self.refcnt);
out_free_cgrp:
	kfree(cgrp);
	return ERR_PTR(ret);
}

/*
 * cgroup_check_hierarchy_limits() - 验证 parent 的全部祖先配额。
 *
 * cgroup_mutex 下向上遍历：nr_descendants 必须低于 max_descendants，
 * 新节点相对每个祖先的 level 必须低于 max_depth。返回布尔值，纯校验；
 * 总锁保证随后创建前不会有并发 mkdir 穿透限额。
 */
static bool cgroup_check_hierarchy_limits(struct cgroup *parent)
{
	struct cgroup *cgroup;
	int ret = false;
	int level = 0;

	lockdep_assert_held(&cgroup_mutex);

	for (cgroup = parent; cgroup; cgroup = cgroup_parent(cgroup)) {
		if (cgroup->nr_descendants >= cgroup->max_descendants)
			goto fail;

		if (level >= cgroup->max_depth)
			goto fail;

		level++;
	}

	ret = true;
fail:
	return ret;
}

/*
 * cgroup_mkdir() - kernfs mkdir 的 cgroup 创建入口。
 *
 * 拒绝换行名称；锁定 live parent 后检查配额、创建对象，额外 kernfs_get
 * 钉住 kn 到最终 free，再发布 core 文件和 controller css，最后 activate。
 * 成功返回 0；任何发布前失败调用统一 cgroup_destroy_locked() 回收。
 */
int cgroup_mkdir(struct kernfs_node *parent_kn, const char *name, umode_t mode)
{
	struct cgroup *parent, *cgrp;
	int ret;

	/* do not accept '\n' to prevent making /proc/<pid>/cgroup unparsable */
	/*
	 * proc 接口以一行表示一个 hierarchy，没有转义 cgroup 名中的换行；
	 * 创建入口因此必须拒绝换行，维持用户 ABI 的可解析性。
	 */
	if (strchr(name, '\n'))
		return -EINVAL;

	parent = cgroup_kn_lock_live(parent_kn, false);
	if (!parent)
		return -ENODEV;

	if (!cgroup_check_hierarchy_limits(parent)) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	cgrp = cgroup_create(parent, name, mode);
	if (IS_ERR(cgrp)) {
		ret = PTR_ERR(cgrp);
		goto out_unlock;
	}

	/*
	 * This extra ref will be put in css_free_rwork_fn() and guarantees
	 * that @cgrp->kn is always accessible.
	 */
	/*
	 * 即使目录已 remove，延迟 release 仍需 kn->priv；这份额外引用由
	 * css_free_rwork_fn() 在最终释放 cgroup 时归还。
	 */
	kernfs_get(cgrp->kn);

	ret = css_populate_dir(&cgrp->self);
	if (ret)
		goto out_destroy;

	ret = cgroup_apply_control_enable(cgrp);
	if (ret)
		goto out_destroy;

	TRACE_CGROUP_PATH(mkdir, cgrp);

	/* let's create and online css's */
	/* activate 是用户可见发布点；此前目录文件和全部 css 已构造成功。 */
	kernfs_activate(cgrp->kn);

	ret = 0;
	goto out_unlock;

out_destroy:
	cgroup_destroy_locked(cgrp);
out_unlock:
	cgroup_kn_unlock(parent_kn);
	return ret;
}

/*
 * This is called when the refcnt of a css is confirmed to be killed.
 * css_tryget_online() is now guaranteed to fail.  Tell the subsystem to
 * initiate destruction and put the css ref from kill_css_finish().
 */
/*
 * 中文契约：
 * confirm 通过 rcu_work 转到可睡眠上下文。持 cgroup_mutex 从当前 css
 * 向 parent 传播，逐个 offline 并 put finish 额外引用；online_cnt 到零
 * 才继续父级，保证 parent offline 晚于所有 online child。返回：无。
 */
static void css_killed_work_fn(struct work_struct *work)
{
	struct cgroup_subsys_state *css;

	css = container_of(to_rcu_work(work), struct cgroup_subsys_state, destroy_rwork);

	cgroup_lock();

	do {
		offline_css(css);
		css_put(css);
		/* @css can't go away while we're holding cgroup_mutex */
		/* cgroup_mutex 阻止拓扑摘除，读取 parent 前无需再取 css 引用。 */
		css = css->parent;
	} while (css && atomic_dec_and_test(&css->online_cnt));

	cgroup_unlock();
}

/* css kill confirmation processing requires process context, bounce */
/*
 * percpu_ref 在所有 CPU 可见 killed 后，原子减少 online_cnt；到零者排队
 * offline rcu_work。确认回调本身不睡眠、不直接调用 subsystem。
 */
static void css_killed_ref_fn(struct percpu_ref *ref)
{
	struct cgroup_subsys_state *css =
		container_of(ref, struct cgroup_subsys_state, refcnt);

	if (atomic_dec_and_test(&css->online_cnt)) {
		INIT_RCU_WORK(&css->destroy_rwork, css_killed_work_fn);
		queue_rcu_work(cgroup_offline_wq, &css->destroy_rwork);
	}
}

/**
 * kill_css_sync - synchronous half of css teardown
 * @css: css being killed
 *
 * See cgroup_destroy_locked().
 */
/*
 * 中文契约：
 * cgroup_mutex 下执行同步半部。重复 DYING 幂等返回；先调 css_killed，
 * 再发布 CSS_DYING，并以 smp_mb 与 populated 最后减计数配对，确保至少
 * 一方触发 finish。随后隐藏文件并维护本节点/祖先 dying 计数；尚未
 * kill percpu_ref。
 */
static void kill_css_sync(struct cgroup_subsys_state *css)
{
	struct cgroup_subsys *ss = css->ss;

	lockdep_assert_held(&cgroup_mutex);

	if (css->flags & CSS_DYING)
		return;

	/*
	 * Call css_killed(), if defined, before setting the CSS_DYING flag
	 */
	/* 先通知 controller kill 开始，再发布 DYING，保持回调观察顺序。 */
	if (css->ss->css_killed)
		css->ss->css_killed(css);

	css->flags |= CSS_DYING;

	/*
	 * Pair with smp_mb() in css_update_populated(). Either our
	 * caller observes the walker's decrement and fires
	 * synchronously, or the walker observes CSS_DYING and queues.
	 */
	/*
	 * 与 populated walker 的屏障配对：要么本路径看到计数归零并同步
	 * finish，要么 walker 看到 DYING 后排队，不能双方都错过。
	 */
	smp_mb();

	/*
	 * This must happen before css is disassociated with its cgroup.
	 * See seq_css() for details.
	 */
	/* 必须在 css 与 cgroup 断开前隐藏目录，使 seq_css 不返回失联对象。 */
	css_clear_dir(css);

	css->cgroup->nr_dying_subsys[ss->id]++;
	/*
	 * Parent css and cgroup cannot be freed until after the freeing
	 * of child css, see css_free_rwork_fn().
	 */
	/* child 最终 free 前持续钉住 parent css/cgroup，保证自底向上回收。 */
	while ((css = css->parent)) {
		css->nr_descendants--;
		css->cgroup->nr_dying_subsys[ss->id]++;
	}
}

/**
 * kill_css_finish - deferred half of css teardown
 * @css: css being killed
 *
 * See cgroup_destroy_locked().
 */
/*
 * 中文契约：
 * cgroup_mutex 下执行延迟半部，仅在层级 populated 为零时调用。重复 kill
 * 幂等返回；否则先 css_get 保留到 offline 后，再 kill_and_confirm。
 * confirm 保证所有 CPU 后续 css_tryget_online 失败，异步链才会 offline。
 */
static void kill_css_finish(struct cgroup_subsys_state *css)
{
	lockdep_assert_held(&cgroup_mutex);

	/*
	 * Skip on re-entry: cgroup_apply_control_disable() may have killed @css
	 * earlier. cgroup_destroy_locked() can still walk it because
	 * offline_css() (which NULLs cgrp->subsys[ssid]) runs async.
	 */
	/* 重入时已 kill 的 css 只等待异步 offline，不得再次消费 base ref。 */
	if (percpu_ref_is_dying(&css->refcnt))
		return;

	/*
	 * Killing would put the base ref, but we need to keep it alive until
	 * after ->css_offline().
	 */
	/* kill 会消费 base ref，先额外 get 使对象活到 offline 回调完成。 */
	css_get(css);

	/*
	 * cgroup core guarantees that, by the time ->css_offline() is invoked,
	 * no new css reference will be given out via css_tryget_online(). We
	 * can't simply call percpu_ref_kill() and proceed to offlining css's
	 * because percpu_ref_kill() doesn't guarantee that the ref is seen as
	 * killed on all CPUs on return.
	 *
	 * Use percpu_ref_kill_and_confirm() to get notifications as each css is
	 * confirmed to be seen as killed on all CPUs.
	 */
	/*
	 * kill() 返回时其他 CPU 可能尚未观察 killed；confirm 回调给出全 CPU
	 * 可见边界，之后才能保证 tryget_online 全失败并安全 offline。
	 */
	percpu_ref_kill_and_confirm(&css->refcnt, css_killed_ref_fn);
}

/**
 * cgroup_destroy_locked - destroy @cgrp (called on rmdir)
 * @cgrp: cgroup to be destroyed
 *
 * Tear down @cgrp on behalf of rmdir. Constraints:
 *
 * - Userspace: rmdir must succeed when cgroup.procs and friends are empty.
 *
 * - Kernel: subsystem ->css_offline() must not run while any task in @cgrp's
 *   subtree is still doing kernel work. A task hidden from cgroup.procs (past
 *   exit_signals() with signal->live cleared) can still schedule, allocate, and
 *   consume resources until its final context switch. Dying descendants in the
 *   subtree can host such tasks too.
 *
 * - Kernel: css_tryget_online() must fail by the time ->css_offline() runs.
 *
 * The destruction runs in three parts:
 *
 * - This function: synchronous user-visible state teardown plus kill_css_sync()
 *   on each subsystem css.
 *
 * - For each subsys css: fire kill_css_finish() synchronously if the subtree is
 *   already drained, otherwise rely on css_update_populated() to queue
 *   kill_finish_work when the last populated cset under the css empties.
 *
 * - The percpu_ref kill chain: css_killed_ref_fn -> css_killed_work_fn ->
 *   ->css_offline() -> release/free.
 *
 * Return 0 on success, -EBUSY if a userspace-visible task or an online child
 * remains.
 */
/*
 * 中文契约与提交点：
 * cgroup_mutex 下先检查用户可见 task 和 online child，存在则 -EBUSY 且
 * 无状态改变。通过后清 self ONLINE、标记关联 cset dead、同步 kill 每个
 * controller css、删除目录并更新 ancestor/threaded/freezer 计数。
 *
 * notifier 后 kill self base ref 是不可回滚点。已 unpopulated css 立即
 * finish，其余等待最后 hidden/dying task 离开时由 populated 路径排队。
 * 成功返回 0；offline、release 和 free 均可在返回后异步发生。
 */
static int cgroup_destroy_locked(struct cgroup *cgrp)
{
	struct cgroup *tcgrp, *parent = cgroup_parent(cgrp);
	struct cgroup_subsys_state *css;
	struct cgrp_cset_link *link;
	struct css_task_iter it;
	struct task_struct *task;
	int ssid, ret;

	lockdep_assert_held(&cgroup_mutex);

	css_task_iter_start(&cgrp->self, 0, &it);
	task = css_task_iter_next(&it);
	css_task_iter_end(&it);
	if (task)
		return -EBUSY;

	/*
	 * Make sure there's no live children.  We can't test emptiness of
	 * ->self.children as dead children linger on it while being
	 * drained; otherwise, "rmdir parent/child parent" may fail.
	 */
	/*
	 * children 链包含已逻辑删除但等待异步释放的后代，不能用链表非空
	 * 判断 EBUSY；只检查仍 online 的 child，允许父子连续 rmdir。
	 */
	if (css_has_online_children(&cgrp->self))
		return -EBUSY;

	/*
	 * Mark @cgrp and the associated csets dead.  The former prevents
	 * further task migration and child creation by disabling
	 * cgroup_kn_lock_live().  The latter makes the csets ignored by
	 * the migration path.
	 */
	/*
	 * 清 ONLINE 关闭新的 mkdir/migrate 入口；关联 cset 标 dead 让预装载
	 * 路径跳过它们。两者共同构成用户可见的逻辑摘除点。
	 */
	cgrp->self.flags &= ~CSS_ONLINE;

	spin_lock_irq(&css_set_lock);
	list_for_each_entry(link, &cgrp->cset_links, cset_link)
		link->cset->dead = true;
	spin_unlock_irq(&css_set_lock);

	for_each_css(css, ssid, cgrp)
		kill_css_sync(css);

	/* clear and remove @cgrp dir, @cgrp has an extra ref on its kn */
	/*
	 * 先删除控制文件再移除目录名字；cgrp 自身额外持有 kn 引用，所以
	 * kernfs 名字消失后节点内存仍活到 cgroup 最终 free。
	 */
	css_clear_dir(&cgrp->self);
	kernfs_remove(cgrp->kn);

	if (cgroup_is_threaded(cgrp))
		parent->nr_threaded_children--;

	spin_lock_irq(&css_set_lock);
	for (tcgrp = parent; tcgrp; tcgrp = cgroup_parent(tcgrp)) {
		tcgrp->nr_descendants--;
		tcgrp->nr_dying_descendants++;
		/*
		 * If the dying cgroup is frozen, decrease frozen descendants
		 * counters of ancestor cgroups.
		 */
			/* dying 节点不再计入祖先的活 frozen descendant 统计。 */
		if (test_bit(CGRP_FROZEN, &cgrp->flags))
			tcgrp->freezer.nr_frozen_descendants--;
	}
	spin_unlock_irq(&css_set_lock);

	cgroup1_check_for_release(parent);

	ret = blocking_notifier_call_chain(&cgroup_lifetime_notifier,
					   CGROUP_LIFETIME_OFFLINE, cgrp);
	WARN_ON_ONCE(notifier_to_errno(ret));

	/* put the base reference */
	/*
	 * kill self base ref 后禁止新的在线引用；剩余持有者排空后进入
	 * release/free 异步链，这是销毁的不可回滚边界。
	 */
	percpu_ref_kill(&cgrp->self.refcnt);

	for_each_css(css, ssid, cgrp) {
		if (!css_is_populated(css))
			kill_css_finish(css);
	}

	return 0;
};

/*
 * cgroup_rmdir() - kernfs rmdir 包装器。
 *
 * 锁定 live cgroup 后调用核心销毁；节点已死亡视为幂等成功。成功发 trace，
 * 最后配对 kn_unlock。返回 0 或 -EBUSY，物理释放异步进行。
 */
int cgroup_rmdir(struct kernfs_node *kn)
{
	struct cgroup *cgrp;
	int ret = 0;

	cgrp = cgroup_kn_lock_live(kn, false);
	if (!cgrp)
		return 0;

	ret = cgroup_destroy_locked(cgrp);
	if (!ret)
		TRACE_CGROUP_PATH(rmdir, cgrp);

	cgroup_kn_unlock(kn);
	return ret;
}

static struct kernfs_syscall_ops cgroup_kf_syscall_ops = {
	.show_options		= cgroup_show_options,
	.mkdir			= cgroup_mkdir,
	.rmdir			= cgroup_rmdir,
	.show_path		= cgroup_show_path,
};

/*
 * cgroup_init_subsys() - 初始化一个 controller 及其永久 root css。
 *
 * ss 是静态 subsystem 描述，early 决定是否处于尚不能安全分配 ID/rstat
 * 的早期启动。函数持 cgroup_mutex 初始化 css_idr/cft 列表，调用
 * css_alloc(NULL)，把 root css 关联到默认根并设 CSS_NO_REF（永久不销毁）。
 * 非 early 分配固定 root ID=1 并初始化统计；随后写入 init_css_set、
 * 汇总 fork/exit 回调位图并 online。
 *
 * 启动期失败均 BUG，而非可恢复 errno；返回：无。调用完成后所有现存 task
 * 通过 init_css_set 使用该 root css。
 */
static void __init cgroup_init_subsys(struct cgroup_subsys *ss, bool early)
{
	struct cgroup_subsys_state *css;

	pr_debug("Initializing cgroup subsys %s\n", ss->name);

	cgroup_lock();

	idr_init(&ss->css_idr);
	INIT_LIST_HEAD(&ss->cfts);

	/* Create the root cgroup state for this subsystem */
	/* 为 controller 构造永久 root css，并接入 default hierarchy。 */
	ss->root = &cgrp_dfl_root;
	css = ss->css_alloc(NULL);
	/* We don't handle early failures gracefully */
	/* early init 无恢复环境，分配失败视为启动期致命不变量破坏。 */
	BUG_ON(IS_ERR(css));
	init_and_link_css(css, ss, &cgrp_dfl_root.cgrp);

	/*
	 * Root csses are never destroyed and we can't initialize
	 * percpu_ref during early init.  Disable refcnting.
	 */
	/* 永久 root css 不参加普通 kill/release 链，CSS_NO_REF 使 get/put 为空。 */
	css->flags |= CSS_NO_REF;

	if (early) {
		/* allocation can't be done safely during early init */
		/* 分配器/IDR 尚未完整可用，先使用永久 root 保留 ID 1。 */
		css->id = 1;
	} else {
		css->id = cgroup_idr_alloc(&ss->css_idr, css, 1, 2, GFP_KERNEL);
		BUG_ON(css->id < 0);

		BUG_ON(ss_rstat_init(ss));
		BUG_ON(css_rstat_init(css));
	}

	/* Update the init_css_set to contain a subsys
	 * pointer to this state - since the subsystem is
	 * newly registered, all tasks and hence the
	 * init_css_set is in the subsystem's root cgroup. */
	/*
	 * 此时尚无 child task，直接更新唯一 init_css_set 即覆盖系统中全部
	 * task 的 controller 视图，无需逐 task 迁移或调用 fork hook。
	 */
	init_css_set.subsys[ss->id] = css;

	have_fork_callback |= (bool)ss->fork << ss->id;
	have_exit_callback |= (bool)ss->exit << ss->id;
	have_release_callback |= (bool)ss->release << ss->id;
	have_canfork_callback |= (bool)ss->can_fork << ss->id;

	/* At system boot, before all subsystems have been
	 * registered, no tasks have been forked, so we don't
	 * need to invoke fork callbacks here. */
	/* 此时尚无派生 task，直接建立 init 归属即可，无需补发 fork 回调。 */
	BUG_ON(!list_empty(&init_task.tasks));

	BUG_ON(online_css(css));

	cgroup_unlock();
}

/**
 * cgroup_init_early - cgroup initialization at system boot
 *
 * Initialize cgroups at system boot, and initialize any
 * subsystems that request early init.
 */
/*
 * 中文契约：
 * 最早启动阶段初始化默认 root 的纯内存结构，将 init_task.cgroups 以 RCU
 * 指向永久 init_css_set，校验/填充每个 subsystem 的 id/name，仅初始化
 * early_init 控制器。此时不能依赖 slab ID/rstat 完整能力。
 * 返回 0；配置/回调不变量违例以 WARN/BUG 暴露。
 */
int __init cgroup_init_early(void)
{
	static struct cgroup_fs_context __initdata ctx;
	struct cgroup_subsys *ss;
	int i;

	ctx.root = &cgrp_dfl_root;
	init_cgroup_root(&ctx);
	cgrp_dfl_root.cgrp.self.flags |= CSS_NO_REF;

	RCU_INIT_POINTER(init_task.cgroups, &init_css_set);

	for_each_subsys(ss, i) {
		WARN(!ss->css_alloc || !ss->css_free || ss->name || ss->id,
		     "invalid cgroup_subsys %d:%s css_alloc=%p css_free=%p id:name=%d:%s\n",
		     i, cgroup_subsys_name[i], ss->css_alloc, ss->css_free,
		     ss->id, ss->name);
		WARN(strlen(cgroup_subsys_name[i]) > MAX_CGROUP_TYPE_NAMELEN,
		     "cgroup_subsys_name %s too long\n", cgroup_subsys_name[i]);
		WARN(ss->early_init && ss->css_rstat_flush,
		     "cgroup rstat cannot be used with early init subsystem\n");

		ss->id = i;
		ss->name = cgroup_subsys_name[i];
		if (!ss->legacy_name)
			ss->legacy_name = cgroup_subsys_name[i];

		if (ss->early_init)
			cgroup_init_subsys(ss, true);
	}
	return 0;
}

/**
 * cgroup_init - cgroup initialization
 *
 * Register cgroup filesystem and /proc file, and initialize
 * any subsystems that didn't request early init.
 */
/*
 * 中文契约：
 * 正常 init 阶段初始化 core/v1/PSI cftype、rstat、RT 支持和 init namespace
 * 引用；把 init_css_set 加入哈希，建立默认 kernfs root，再补完所有
 * controller 的 ID/rstat/root css、能力 mask、v1/v2 文件和 bind 回调。
 *
 * 这是启动不可恢复路径，关键失败以 BUG_ON；成功返回 0 后 cgroup 文件
 * 系统及控制器元数据已可进入后续注册阶段。
 */
int __init cgroup_init(void)
{
	struct cgroup_subsys *ss;
	int ssid;

	BUILD_BUG_ON(CGROUP_SUBSYS_COUNT > 32);
	BUG_ON(cgroup_init_cftypes(NULL, cgroup_base_files));
	BUG_ON(cgroup_init_cftypes(NULL, cgroup_psi_files));
	BUG_ON(cgroup_init_cftypes(NULL, cgroup1_base_files));

	BUG_ON(ss_rstat_init(NULL));

	get_user_ns(init_cgroup_ns.user_ns);
	cgroup_rt_init();

	cgroup_lock();

	/*
	 * Add init_css_set to the hash table so that dfl_root can link to
	 * it during init.
	 */
	/* 先发布 init_css_set 哈希项，default root 初始化才能建立反向 link。 */
	hash_add(css_set_table, &init_css_set.hlist,
		 css_set_hash(init_css_set.subsys));

	cgroup_bpf_lifetime_notifier_init();

	BUG_ON(cgroup_setup_root(&cgrp_dfl_root, 0));

	cgroup_unlock();

	for_each_subsys(ss, ssid) {
		if (ss->early_init) {
			struct cgroup_subsys_state *css =
				init_css_set.subsys[ss->id];

			css->id = cgroup_idr_alloc(&ss->css_idr, css, 1, 2,
						   GFP_KERNEL);
			BUG_ON(css->id < 0);
		} else {
			cgroup_init_subsys(ss, false);
		}

		list_add_tail(&init_css_set.e_cset_node[ssid],
			      &cgrp_dfl_root.cgrp.e_csets[ssid]);

		/*
		 * Setting dfl_root subsys_mask needs to consider the
		 * disabled flag and cftype registration needs kmalloc,
		 * both of which aren't available during early_init.
		 */
	/*
	 * enabled 策略与 cftype 分配依赖常规 init 设施；early 阶段只构造
	 * 最小 root css，留到这里补齐 mask 和接口文件。
	 */
		if (!cgroup_ssid_enabled(ssid))
			continue;

		if (cgroup1_ssid_disabled(ssid))
			pr_info("Disabling %s control group subsystem in v1 mounts\n",
				ss->legacy_name);

		cgrp_dfl_root.subsys_mask |= 1 << ss->id;

		/* implicit controllers must be threaded too */
		/* 隐式 controller 必须支持 threaded，才能在 v2 内部自动传播。 */
		WARN_ON(ss->implicit_on_dfl && !ss->threaded);

		if (ss->implicit_on_dfl)
			cgrp_dfl_implicit_ss_mask |= 1 << ss->id;
		else if (!ss->dfl_cftypes)
			cgrp_dfl_inhibit_ss_mask |= 1 << ss->id;

		if (ss->threaded)
			cgrp_dfl_threaded_ss_mask |= 1 << ss->id;

		if (ss->dfl_cftypes == ss->legacy_cftypes) {
			WARN_ON(cgroup_add_cftypes(ss, ss->dfl_cftypes));
		} else {
			WARN_ON(cgroup_add_dfl_cftypes(ss, ss->dfl_cftypes));
			WARN_ON(cgroup_add_legacy_cftypes(ss, ss->legacy_cftypes));
		}

		if (ss->bind)
			ss->bind(init_css_set.subsys[ssid]);

		cgroup_lock();
		css_populate_dir(init_css_set.subsys[ssid]);
		cgroup_unlock();
	}

	/* init_css_set.subsys[] has been updated, re-hash */
	/* controller root css 已补齐，旧哈希键失效，按新组合重新插入。 */
	hash_del(&init_css_set.hlist);
	hash_add(css_set_table, &init_css_set.hlist,
		 css_set_hash(init_css_set.subsys));

	WARN_ON(sysfs_create_mount_point(fs_kobj, "cgroup"));
	WARN_ON(register_filesystem(&cgroup_fs_type));
	WARN_ON(register_filesystem(&cgroup2_fs_type));
	WARN_ON(!proc_create_single("cgroups", 0, NULL, proc_cgroupstats_show));
#ifdef CONFIG_CPUSETS_V1
	WARN_ON(register_filesystem(&cpuset_fs_type));
#endif

	ns_tree_add(&init_cgroup_ns);
	return 0;
}

/*
 * cgroup_wq_init() - 在 workqueue 子系统可用后建立 cgroup 销毁队列。
 *
 * 调用关系：core_initcall 调用；后续 offline/release/free 路径分别排队。
 * 入参：无。入口处系统 workqueue 已初始化，进程上下文，可睡眠。
 * 返回：成功返回 0；分配失败触发 BUG，不存在可交给调用者处理的 errno。
 * 副作用：发布三个全局 workqueue，内核余下生命周期内保持有效。
 */
static int __init cgroup_wq_init(void)
{
	/*
	 * cgroup_init() 执行时 workqueue 子系统尚不可用，因此把销毁流水线的
	 * 三个队列推迟到 core_initcall。三阶段分队列不是为了并行提速，而是
	 * 把 offline、release 和 RCU 后 free 的上下文及先后关系明确隔离。
	 */
	/*
	 * There isn't much point in executing destruction path in
	 * parallel.  Good chunk is serialized with cgroup_mutex anyway.
	 * Use 1 for @max_active.
	 *
	 * We would prefer to do this in cgroup_init() above, but that
	 * is called before init_workqueues(): so leave this until after.
	 */
	/*
	 * 销毁大部分受 cgroup_mutex 串行，max_active=1 足够；三个阶段仍分队列
	 * 防依赖死锁。由于 cgroup_init 早于 workqueue 初始化，只能延至此处。
	 */
	cgroup_offline_wq = alloc_workqueue("cgroup_offline", WQ_PERCPU, 1);
	BUG_ON(!cgroup_offline_wq);

	cgroup_release_wq = alloc_workqueue("cgroup_release", WQ_PERCPU, 1);
	BUG_ON(!cgroup_release_wq);

	cgroup_free_wq = alloc_workqueue("cgroup_free", WQ_PERCPU, 1);
	BUG_ON(!cgroup_free_wq);
	return 0;
}
core_initcall(cgroup_wq_init);

/*
 * 根据 default hierarchy 的 kernfs 节点 ID 生成路径。
 *
 * kernfs_find_and_get_node_by_id() 返回带引用的节点，保证路径格式化期间
 * 节点内存稳定；找不到时保持调用者缓冲区不变。该接口不做 cgroup
 * namespace 可见性过滤，调用者应把它视为内核全局 ID 查询。
 */
void cgroup_path_from_kernfs_id(u64 id, char *buf, size_t buflen)
{
	struct kernfs_node *kn;

	kn = kernfs_find_and_get_node_by_id(cgrp_dfl_root.kf_root, id);
	if (!kn)
		return;
	kernfs_path(kn, buf, buflen);
	kernfs_put(kn);
}

/*
 * __cgroup_get_from_id : get the cgroup associated with cgroup id
 * @id: cgroup id
 * On success return the cgrp or ERR_PTR on failure
 * There are no cgroup NS restrictions.
 */
/*
 * 中文契约：
 * id 是 default hierarchy 的全局 kernfs ID，不受调用者 cgroup namespace
 * 限制。函数可睡眠查找节点；成功返回持有引用的 cgroup，调用者必须
 * cgroup_put()。不存在、不是目录、priv 已清除或引用已进入 dying 均返回
 * ERR_PTR(-ENOENT)。RCU 只保护 priv 读取窗口，长期存活由 tryget 引用保证。
 */
struct cgroup *__cgroup_get_from_id(u64 id)
{
	struct kernfs_node *kn;
	struct cgroup *cgrp;

	kn = kernfs_find_and_get_node_by_id(cgrp_dfl_root.kf_root, id);
	if (!kn)
		return ERR_PTR(-ENOENT);

	if (kernfs_type(kn) != KERNFS_DIR) {
		kernfs_put(kn);
		return ERR_PTR(-ENOENT);
	}

	rcu_read_lock();

	/*
	 * kn 的引用只保护 kernfs_node，自身并不保证 kn->priv 指向的 cgroup
	 * 仍存活。销毁路径先在 RCU 下清 priv；这里在同一保护域读取并通过
	 * cgroup_tryget() 把瞬时观察转换为可带出 RCU 临界区的稳定引用。
	 */
	cgrp = rcu_dereference(*(void __rcu __force **)&kn->priv);
	if (cgrp && !cgroup_tryget(cgrp))
		cgrp = NULL;

	rcu_read_unlock();
	kernfs_put(kn);

	if (!cgrp)
		return ERR_PTR(-ENOENT);
	return cgrp;
}

/*
 * cgroup_get_from_id : get the cgroup associated with cgroup id
 * @id: cgroup id
 * On success return the cgrp or ERR_PTR on failure
 * Only cgroups within current task's cgroup NS are valid.
 */
/*
 * 中文契约：
 * id 的单位和 ownership 与 __cgroup_get_from_id() 相同，但额外以 current
 * 的 default cgroup namespace 根过滤可见性。成功引用转交调用者；越界
 * 对象先 put 再返回 -ENOENT，以免泄露命名空间外层级是否存在。
 */
struct cgroup *cgroup_get_from_id(u64 id)
{
	struct cgroup *cgrp, *root_cgrp;

	cgrp = __cgroup_get_from_id(id);
	if (IS_ERR(cgrp))
		return cgrp;

	root_cgrp = current_cgns_cgroup_dfl();
	/*
	 * 对命名空间外对象返回 ENOENT 而非权限错误，避免借 ID 探测不可见
	 * 层级。成功返回的引用来自 __cgroup_get_from_id()，由调用者 put。
	 */
	if (!cgroup_is_descendant(cgrp, root_cgrp)) {
		cgroup_put(cgrp);
		return ERR_PTR(-ENOENT);
	}

	return cgrp;
}
EXPORT_SYMBOL_GPL(cgroup_get_from_id);

/*
 * proc_cgroup_show()
 *  - Print task's cgroup paths into seq_file, one line for each hierarchy
 *  - Used for /proc/<pid>/cgroup.
 */
/*
 * 中文契约：
 * m 是 seq_file 输出；ns/pid 描述 proc 视图，tsk 是读取期间稳定的目标
 * task，均为借用。函数分配 PATH_MAX 缓冲，可睡眠；RCU 与 css_set_lock
 * 下遍历每个 hierarchy，并按读取者 cgroup namespace 格式化路径。
 *
 * 成功返回 0；分配失败返回 -ENOMEM，路径过长返回 -ENAMETOOLONG，其他
 * 路径错误原样传播。输出可能已写入前面若干 hierarchy，失败不回滚文本；
 * 所有锁和缓冲在返回前释放。
 */
int proc_cgroup_show(struct seq_file *m, struct pid_namespace *ns,
		     struct pid *pid, struct task_struct *tsk)
{
	char *buf;
	int retval;
	struct cgroup_root *root;

	retval = -ENOMEM;
	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buf)
		goto out;

	rcu_read_lock();
	spin_lock_irq(&css_set_lock);

	/*
	 * css_set 与各 hierarchy 的关联可被迁移路径更新；css_set_lock 保证
	 * 一行中 controller 列表、目标 cgroup 和路径来自同一稳定视图。
	 * RCU 则保护遍历中的 root/css_set 链接对象。
	 */
	for_each_root(root) {
		struct cgroup_subsys *ss;
		struct cgroup *cgrp;
		int ssid, count = 0;

		if (root == &cgrp_dfl_root && !READ_ONCE(cgrp_dfl_visible))
			continue;

		cgrp = task_cgroup_from_root(tsk, root);
		/* The root has already been unmounted. */
		/* root 已卸载时该 hierarchy 不再产生 /proc 输出行。 */
		if (!cgrp)
			continue;

		seq_printf(m, "%d:", root->hierarchy_id);
		if (root != &cgrp_dfl_root)
			for_each_subsys(ss, ssid)
				if (root->subsys_mask & (1 << ssid))
					seq_printf(m, "%s%s", count++ ? "," : "",
						   ss->legacy_name);
		if (strlen(root->name))
			seq_printf(m, "%sname=%s", count ? "," : "",
				   root->name);
		seq_putc(m, ':');
		/*
		 * On traditional hierarchies, all zombie tasks show up as
		 * belonging to the root cgroup.  On the default hierarchy,
		 * while a zombie doesn't show up in "cgroup.procs" and
		 * thus can't be migrated, its /proc/PID/cgroup keeps
		 * reporting the cgroup it belonged to before exiting.  If
		 * the cgroup is removed before the zombie is reaped,
		 * " (deleted)" is appended to the cgroup path.
		 */
	/*
	 * v1 退出 task 统一显示 root；v2 保留退出前归属，即使目录已删除也
	 * 输出原路径并追加 deleted，使 zombie 的历史归属仍可观察。
	 */
		if (cgroup_on_dfl(cgrp) || !(tsk->flags & PF_EXITING)) {
			/*
			 * 路径以当前读取者的 cgroup namespace 为根，而不是目标
			 * 进程的 namespace；这与 /proc 的观察者视角语义一致。
			 */
			retval = cgroup_path_ns_locked(cgrp, buf, PATH_MAX,
						current->nsproxy->cgroup_ns);
			if (retval == -E2BIG)
				retval = -ENAMETOOLONG;
			if (retval < 0)
				goto out_unlock;

			seq_puts(m, buf);
		} else {
			seq_puts(m, "/");
		}

		if (cgroup_on_dfl(cgrp) && cgroup_is_dead(cgrp))
			seq_puts(m, " (deleted)\n");
		else
			seq_putc(m, '\n');
	}

	retval = 0;
out_unlock:
	spin_unlock_irq(&css_set_lock);
	rcu_read_unlock();
	kfree(buf);
out:
	return retval;
}

/**
 * cgroup_fork - initialize cgroup related fields during copy_process()
 * @child: pointer to task_struct of forking parent process.
 *
 * A task is associated with the init_css_set until cgroup_post_fork()
 * attaches it to the target css_set.
 */
/*
 * 中文契约：
 * child 是 copy_process() 正在构造、尚未发布运行的 task，调用者独占其
 * cgroup 字段。函数不睡眠、不加锁，把 cgroups 临时指向永久 init_css_set
 * 并初始化 cg_list；不取得引用。返回：无直接返回值。下一阶段由
 * cgroup_can_fork()/cgroup_post_fork() 选择并发布真实 css_set。
 */
void cgroup_fork(struct task_struct *child)
{
	/*
	 * copy_process() 尚未完成时 child 不能进入正常 css_set 链表。先放置
	 * init_css_set 哨兵并初始化链节点，真正归属由 can/post_fork 两阶段
	 * 决定；失败路径因而总能面对一个结构合法但尚未发布的 child。
	 */
	RCU_INIT_POINTER(child->cgroups, &init_css_set);
	INIT_LIST_HEAD(&child->cg_list);
}

/**
 * cgroup_v1v2_get_from_file - get a cgroup pointer from a file pointer
 * @f: file corresponding to cgroup_dir
 *
 * Find the cgroup from a file pointer associated with a cgroup directory.
 * Returns a pointer to the cgroup on success. ERR_PTR is returned if the
 * cgroup cannot be found.
 */
/*
 * 中文契约：
 * f 是已固定的目录 file 借用指针，可来自 cgroup1 或 cgroup2。函数通过
 * dentry 在 RCU 下取得 online self css；成功返回持有引用的 cgroup，
 * 调用者负责 put。错误指针表示 fd 不是 cgroup 目录、目录已摘除或 css
 * 已离线。函数本身不修改层级。
 */
static struct cgroup *cgroup_v1v2_get_from_file(struct file *f)
{
	struct cgroup_subsys_state *css;

	/*
	 * dentry 可能与并发 rmdir 相遇。css_tryget_online_from_dir() 只有在
	 * css 仍 online 时才取得引用；self css 的引用等价于 cgroup 引用，
	 * 所以成功返回后调用者必须 cgroup_put()。
	 */
	css = css_tryget_online_from_dir(f->f_path.dentry, NULL);
	if (IS_ERR(css))
		return ERR_CAST(css);

	return css->cgroup;
}

/**
 * cgroup_get_from_file - same as cgroup_v1v2_get_from_file, but only supports
 * cgroup2.
 * @f: file corresponding to cgroup2_dir
 */
/*
 * 中文契约：
 * f 的借用与返回引用规则同 cgroup_v1v2_get_from_file()，但只接受
 * default hierarchy。成功返回需 cgroup_put()；v1 目录会先归还已取得
 * 引用再返回 ERR_PTR(-EBADF)，其他错误原样传播。
 */
static struct cgroup *cgroup_get_from_file(struct file *f)
{
	struct cgroup *cgrp = cgroup_v1v2_get_from_file(f);

	if (IS_ERR(cgrp))
		return ERR_CAST(cgrp);

	if (!cgroup_on_dfl(cgrp)) {
		/* CLONE_INTO_CGROUP 等 cgroup2-only 接口拒绝 v1 目录 fd。 */
		cgroup_put(cgrp);
		return ERR_PTR(-EBADF);
	}

	return cgrp;
}

/**
 * cgroup_css_set_fork - find or create a css_set for a child process
 * @kargs: the arguments passed to create the child process
 *
 * This functions finds or creates a new css_set which the child
 * process will be attached to in cgroup_post_fork(). By default,
 * the child process will be given the same css_set as its parent.
 *
 * If CLONE_INTO_CGROUP is specified this function will try to find an
 * existing css_set which includes the requested cgroup and if not create
 * a new css_set that the child will be attached to later. If this function
 * succeeds it will hold cgroup_threadgroup_rwsem on return. If
 * CLONE_INTO_CGROUP is requested this function will grab cgroup mutex
 * before grabbing cgroup_threadgroup_rwsem and will hold a reference
 * to the target cgroup.
 */
/*
 * 中文契约：
 * kargs 是 copy_process() 的输入输出事务对象；flags/cgroup fd/cgrp 为
 * 输入，cset、cgrp 引用和 kill_seq 为输出。普通 fork 继承 current cset；
 * CLONE_INTO_CGROUP 则验证目录 fd、写权限和迁移权限，并查找/创建目标
 * css_set。函数在进程上下文执行，查找路径可睡眠。
 *
 * 成功返回 0，并保持 cgroup_threadgroup_rwsem；CLONE_INTO_CGROUP 时还
 * 保持 cgroup_mutex 和目标 cgroup 引用，统一由 post/cancel 阶段消费。
 * 失败返回精确 errno，逆序释放全部临时引用和锁，kargs 不留下待清资源。
 */
static int cgroup_css_set_fork(struct kernel_clone_args *kargs)
	__acquires(&cgroup_mutex) __acquires(&cgroup_threadgroup_rwsem)
{
	int ret;
	struct cgroup *dst_cgrp = NULL;
	struct css_set *cset;
	struct super_block *sb;

	if (kargs->flags & CLONE_INTO_CGROUP)
		cgroup_lock();

	/*
	 * 锁顺序固定为 cgroup_mutex -> cgroup_threadgroup_rwsem ->
	 * css_set_lock，与迁移路径一致。即使普通 fork 不取 mutex，也必须持
	 * threadgroup 写侧语义，阻止 fork 与整线程组迁移交错发布成员。
	 */
	cgroup_threadgroup_change_begin(current);

	spin_lock_irq(&css_set_lock);
	cset = task_css_set(current);
	/* 预留给 child 的 css_set 引用，直到 post/cancel_fork 消费。 */
	get_css_set(cset);
	/*
	 * 记录准备阶段看到的 kill_seq。post_fork 再比较目标的当前序号，
	 * 可封住“检查后、挂入前”发生 cgroup.kill 而漏杀新任务的窗口。
	 */
	if (kargs->cgrp)
		kargs->kill_seq = kargs->cgrp->kill_seq;
	else
		kargs->kill_seq = cset->dfl_cgrp->kill_seq;
	spin_unlock_irq(&css_set_lock);

	if (!(kargs->flags & CLONE_INTO_CGROUP)) {
		/* 普通 fork 继承父进程 cset，并把锁留给 post/cancel 阶段释放。 */
		kargs->cset = cset;
		return 0;
	}

	CLASS(fd_raw, f)(kargs->cgroup);
	if (fd_empty(f)) {
		ret = -EBADF;
		goto err;
	}
	sb = fd_file(f)->f_path.dentry->d_sb;

	dst_cgrp = cgroup_get_from_file(fd_file(f));
	if (IS_ERR(dst_cgrp)) {
		ret = PTR_ERR(dst_cgrp);
		dst_cgrp = NULL;
		goto err;
	}

	if (cgroup_is_dead(dst_cgrp)) {
		/*
		 * 在线引用不代表目录仍可接收新任务，rmdir 后必须显式
		 * 拒绝。
		 */
		ret = -ENODEV;
		goto err;
	}

	/*
	 * Verify that we the target cgroup is writable for us. This is
	 * usually done by the vfs layer but since we're not going through
	 * the vfs layer here we need to do it "manually".
	 */
	/*
	 * CLONE_INTO_CGROUP 不经过普通 VFS write 路径，必须显式复用 inode
	 * 写权限检查，否则目录 fd 会绕过 cgroup.procs 的访问控制。
	 */
	ret = cgroup_may_write(dst_cgrp, sb);
	if (ret)
		goto err;

	/*
	 * Spawning a task directly into a cgroup works by passing a file
	 * descriptor to the target cgroup directory. This can even be an O_PATH
	 * file descriptor. But it can never be a cgroup.procs file descriptor.
	 * This was done on purpose so spawning into a cgroup could be
	 * conceptualized as an atomic
	 *
	 *   fd = openat(dfd_cgroup, "cgroup.procs", ...);
	 *   write(fd, <child-pid>, ...);
	 *
	 * sequence, i.e. it's a shorthand for the caller opening and writing
	 * cgroup.procs of the cgroup indicated by @dfd_cgroup. This allows us
	 * to always use the caller's credentials.
	 */
	/*
	 * 目录 fd 表示对目标 cgroup.procs 的原子 open+write；允许 O_PATH，
	 * 但拒绝直接传 procs 文件 fd。权限始终按 clone 调用者凭据判断，
	 * 避免 fd 传递改变授权主体。
	 */
	ret = cgroup_attach_permissions(cset->dfl_cgrp, dst_cgrp, sb,
					!(kargs->flags & CLONE_THREAD),
					current->nsproxy->cgroup_ns);
	if (ret)
		goto err;

	kargs->cset = find_css_set(cset, dst_cgrp);
	if (!kargs->cset) {
		ret = -ENOMEM;
		goto err;
	}

	/*
	 * find_css_set() 已为目标组合返回一份引用，父 cset 的临时引用可还。
	 * dst_cgrp 则留在 kargs 中固定目标，直到 post/cancel 的统一清理。
	 */
	put_css_set(cset);
	kargs->cgrp = dst_cgrp;
	return ret;

err:
	/*
	 * 所有失败出口按加锁/取引用的逆序回滚。kargs->cset 可能由更早阶段
	 * 写入，故也作条件释放，保证 copy_process() 可直接返回错误。
	 */
	cgroup_threadgroup_change_end(current);
	cgroup_unlock();
	if (dst_cgrp)
		cgroup_put(dst_cgrp);
	put_css_set(cset);
	if (kargs->cset)
		put_css_set(kargs->cset);
	return ret;
}

/**
 * cgroup_css_set_put_fork - drop references we took during fork
 * @kargs: the arguments passed to create the child process
 *
 * Drop references to the prepared css_set and target cgroup if
 * CLONE_INTO_CGROUP was requested.
 */
/*
 * 中文契约：
 * kargs 必须来自成功的 cgroup_css_set_fork() 或其后续阶段；函数消费其中
 * prepared cset/目标 cgroup 引用，结束 threadgroup change，并按 flags
 * 释放 cgroup_mutex。返回：无直接返回值；完成后相关字段清 NULL、锁与
 * 引用恢复到 fork 准备前状态。解锁过程可睡眠。
 */
static void cgroup_css_set_put_fork(struct kernel_clone_args *kargs)
	__releases(&cgroup_threadgroup_rwsem) __releases(&cgroup_mutex)
{
	struct cgroup *cgrp = kargs->cgrp;
	struct css_set *cset = kargs->cset;

	/*
	 * post_fork 和两类取消路径共享此收尾：先结束 threadgroup change，
	 * 再归还 css_set，最后解 mutex 并放目标 cgroup。字段清 NULL 防止
	 * copy_process() 后续错误清理重复 put。
	 */
	cgroup_threadgroup_change_end(current);

	if (cset) {
		put_css_set(cset);
		kargs->cset = NULL;
	}

	if (kargs->flags & CLONE_INTO_CGROUP) {
		cgroup_unlock();
		if (cgrp) {
			cgroup_put(cgrp);
			kargs->cgrp = NULL;
		}
	}
}

/**
 * cgroup_can_fork - called on a new task before the process is exposed
 * @child: the child process
 * @kargs: the arguments passed to create the child process
 *
 * This prepares a new css_set for the child process which the child will
 * be attached to in cgroup_post_fork().
 * This calls the subsystem can_fork() callbacks. If the cgroup_can_fork()
 * callback returns an error, the fork aborts with that error code. This
 * allows for a cgroup subsystem to conditionally allow or deny new forks.
 */
/*
 * 中文契约：
 * child 是未发布的新 task；kargs 是可写 fork 事务对象。函数可睡眠，
 * 先准备目标 cset 并持有 threadgroup/mutex 锁，再按控制器顺序调用
 * can_fork。成功返回 0，资源和锁留给 post/cancel；任一回调拒绝时只对
 * 已成功回调执行 cancel_fork，完整回滚并返回其 errno。
 */
int cgroup_can_fork(struct task_struct *child, struct kernel_clone_args *kargs)
{
	struct cgroup_subsys *ss;
	int i, j, ret;

	ret = cgroup_css_set_fork(kargs);
	if (ret)
		return ret;

	/*
	 * controller 回调按 subsystem 顺序执行。一旦第 i 个拒绝，只能撤销
	 * 已成功的 [0, i)；尚未调用的 controller 没有状态可回滚。
	 */
	do_each_subsys_mask(ss, i, have_canfork_callback) {
		ret = ss->can_fork(child, kargs->cset);
		if (ret)
			goto out_revert;
	} while_each_subsys_mask();

	return 0;

out_revert:
	for_each_subsys(ss, j) {
		if (j >= i)
			break;
		if (ss->cancel_fork)
			ss->cancel_fork(child, kargs->cset);
	}

	cgroup_css_set_put_fork(kargs);

	return ret;
}

/**
 * cgroup_cancel_fork - called if a fork failed after cgroup_can_fork()
 * @child: the child process
 * @kargs: the arguments passed to create the child process
 *
 * This calls the cancel_fork() callbacks if a fork failed *after*
 * cgroup_can_fork() succeeded and cleans up references we took to
 * prepare a new css_set for the child process in cgroup_can_fork().
 */
/*
 * 中文契约：
 * 仅在 cgroup_can_fork() 已整体成功、copy_process() 后续又失败时调用。
 * child/kargs 均为借用，但函数消费 kargs 内的 cset/cgroup 引用和所持锁。
 * 返回：无直接返回值；所有 controller 收到对称 cancel_fork，完成后不再
 * 有 cgroup fork 事务资源。回调及解锁可能睡眠。
 */
void cgroup_cancel_fork(struct task_struct *child,
			struct kernel_clone_args *kargs)
{
	struct cgroup_subsys *ss;
	int i;

	/*
	 * 此入口只在所有 can_fork 已成功、但 copy_process 后续失败时调用，
	 * 因而每个实现了 cancel_fork 的 controller 都需要收到对称通知。
	 */
	for_each_subsys(ss, i)
		if (ss->cancel_fork)
			ss->cancel_fork(child, kargs->cset);

	cgroup_css_set_put_fork(kargs);
}

/**
 * cgroup_post_fork - finalize cgroup setup for the child process
 * @child: the child process
 * @kargs: the arguments passed to create the child process
 *
 * Attach the child process to its css_set calling the subsystem fork()
 * callbacks.
 */
/*
 * 中文契约：
 * child 已完成构造但尚未返回用户态；kargs 持 prepared cset、可选目标
 * cgroup 引用和 fork 阶段锁。函数在 css_set_lock 下原子发布成员关系，
 * 处理继承冻结状态与 kill_seq 竞态，随后调用 controller fork 回调并
 * 更新新 cgroup namespace 的 root_cset。
 *
 * 返回：无直接返回值；成功后 child 的 task 引用拥有一份 css_set 归属，
 * kargs 资源及 mutex/threadgroup 锁全部释放。目标在准备后被 kill 时，
 * child 在进入用户态前收到 SIGKILL，不会逃过 cgroup.kill。
 */
void cgroup_post_fork(struct task_struct *child,
		      struct kernel_clone_args *kargs)
	__releases(&cgroup_threadgroup_rwsem) __releases(&cgroup_mutex)
{
	unsigned int cgrp_kill_seq = 0;
	unsigned long cgrp_flags = 0;
	bool kill = false;
	struct cgroup_subsys *ss;
	struct css_set *cset;
	int i;

	cset = kargs->cset;
	/* 从这里开始局部变量拥有 prepared cset，公共清理不应再 put 它。 */
	kargs->cset = NULL;

	spin_lock_irq(&css_set_lock);

	/* init tasks are special, only link regular threads */
	/* pid 0 init 类 task 不进入普通 css_set task 链，只发布常规线程。 */
	if (likely(child->pid)) {
		if (kargs->cgrp) {
			cgrp_flags = kargs->cgrp->flags;
			cgrp_kill_seq = kargs->cgrp->kill_seq;
		} else {
			cgrp_flags = cset->dfl_cgrp->flags;
			cgrp_kill_seq = cset->dfl_cgrp->kill_seq;
		}

		WARN_ON_ONCE(!list_empty(&child->cg_list));
		cset->nr_tasks++;
		/*
		 * 在 css_set_lock 内同时发布 child->cgroups 和 cg_list 链接，
		 * 迭代器不会看到“指针已变但链表未挂”，或相反的
		 * 半完成状态。
		 */
		css_set_move_task(child, NULL, cset, false);
	} else {
		/* pid 0 的特殊 init task 不进入普通任务成员链。 */
		put_css_set(cset);
		cset = NULL;
	}

	if (!(child->flags & PF_KTHREAD)) {
		if (unlikely(test_bit(CGRP_FREEZE, &cgrp_flags))) {
			/*
			 * If the cgroup has to be frozen, the new task has
			 * too. Let's set the JOBCTL_TRAP_FREEZE jobctl bit to
			 * get the task into the frozen state.
			 */
			/*
			 * 目标要求冻结时先置 jobctl trap，使 child 首次运行
			 * 即进入冻结。
			 */
			spin_lock(&child->sighand->siglock);
			WARN_ON_ONCE(child->frozen);
			child->jobctl |= JOBCTL_TRAP_FREEZE;
			spin_unlock(&child->sighand->siglock);

			/*
			 * Calling cgroup_update_frozen() isn't required here,
			 * because it will be called anyway a bit later from
			 * do_freezer_trap(). So we avoid cgroup's transient
			 * switch from the frozen state and back.
			 */
			/*
			 * do_freezer_trap 稍后会统一更新 frozen 计数；此处
			 * 提前更新会产生短暂“解冻再冻结”的错误可见状态。
			 */
		}

		/*
		 * If the cgroup is to be killed notice it now and take the
		 * child down right after we finished preparing it for
		 * userspace.
		 */
		/*
		 * kill_seq 已变化说明并发 kill 覆盖本次创建，发布完成后
		 * 立即终止。
		 */
		kill = kargs->kill_seq != cgrp_kill_seq;
	}

	spin_unlock_irq(&css_set_lock);

	/*
	 * Call ss->fork().  This must happen after @child is linked on
	 * css_set; otherwise, @child might change state between ->fork()
	 * and addition to css_set.
	 */
	/*
	 * controller fork 回调必须晚于成员链发布；否则 child 可在回调与挂链
	 * 之间改变状态，controller 观察到未受 core 成员关系约束的 task。
	 */
	do_each_subsys_mask(ss, i, have_fork_callback) {
		ss->fork(child);
	} while_each_subsys_mask();

	/* Make the new cset the root_cset of the new cgroup namespace. */
	/* 新 namespace 独立持有 child 当前 cset，作为其所有 hierarchy 可见根。 */
	if (kargs->flags & CLONE_NEWCGROUP) {
		struct css_set *rcset = child->nsproxy->cgroup_ns->root_cset;

		/*
		 * 新 namespace 的可见根必须与 child 已发布的 css_set 相同；
		 * root_cset 独立持有引用，不能借用 task 的成员引用。
		 */
		get_css_set(cset);
		child->nsproxy->cgroup_ns->root_cset = cset;
		put_css_set(rcset);
	}

	/* Cgroup has to be killed so take down child immediately. */
	/* child 尚未返回用户态，此处发进程级 SIGKILL 封闭并发 kill 窗口。 */
	if (unlikely(kill))
		/*
		 * child 已完成内核侧挂接但尚未返回用户态，此时发 SIGKILL 可使
		 * cgroup.kill 与并发 CLONE_INTO_CGROUP 呈现不漏任务的效果。
		 */
		do_send_sig_info(SIGKILL, SEND_SIG_NOINFO, child, PIDTYPE_TGID);

	cgroup_css_set_put_fork(kargs);
}

/**
 * cgroup_task_exit - detach cgroup from exiting task
 * @tsk: pointer to task_struct of exiting process
 *
 * Description: Detach cgroup from @tsk.
 *
 */
/*
 * 中文契约：
 * tsk 正处于 exit 路径且仍关联原 css_set，为调用期间借用。函数按启用
 * 位图调用 controller exit 回调，不负责摘除成员链或释放 cset。返回：
 * 无直接返回值；副作用由各 controller 完成，后续 cgroup_task_dead()
 * 才执行 core 摘链。回调必须遵守退出路径的上下文约束。
 */
void cgroup_task_exit(struct task_struct *tsk)
{
	struct cgroup_subsys *ss;
	int i;

	/*
	 * exit 回调发生在 task 仍关联原 css_set 时，controller 可读取退出
	 * 归属并结算状态；真正从成员链摘除由稍后的 cgroup_task_dead 完成。
	 */
	/* see cgroup_post_fork() for details */
	/* 回调顺序与 post_fork 对称，且发生在 core 摘除 task 成员关系之前。 */
	do_each_subsys_mask(ss, i, have_exit_callback) {
		ss->exit(tsk);
	} while_each_subsys_mask();
}

/*
 * do_cgroup_task_dead() - 把完成最后一次调度的 task 从活成员关系摘除。
 *
 * tsk 在调用期间必须保持 task_struct 存活；普通配置由调度尾部直接借用，
 * PREEMPT_RT worker 则持显式引用。函数取得 IRQ-safe css_set_lock，不能
 * 睡眠；更新 task 链、nr_tasks、dying leader、deadline 和 freezer 计数。
 * 返回：无直接返回值。task 的 css_set 所有权仍留到 cgroup_task_free()。
 */
static void do_cgroup_task_dead(struct task_struct *tsk)
{
	struct css_set *cset;
	unsigned long flags;

	spin_lock_irqsave(&css_set_lock, flags);

	WARN_ON_ONCE(list_empty(&tsk->cg_list));
	cset = task_css_set(tsk);
	/*
	 * 一次锁内完成活任务摘链和 nr_tasks 递减。leader 虽已退出，但只要
	 * signal->live 表明线程组仍有活线程，就挂到 dying_tasks，使
	 * css_task_iter 能继续以线程组语义找到它，直到最后成员退出。
	 */
	css_set_move_task(tsk, cset, NULL, false);
	cset->nr_tasks--;
	/* matches the signal->live check in css_task_iter_advance() */
	/*
	 * 该条件与 iterator 可见性判断配对，只保留仍代表 live
	 * 线程组的 leader。
	 */
	if (thread_group_leader(tsk) && atomic_read(&tsk->signal->live))
		list_add_tail(&tsk->cg_list, &cset->dying_tasks);

	if (dl_task(tsk))
		/* deadline task 的 cgroup 调度统计必须随死亡同步扣减。 */
		dec_dl_tasks_cs(tsk);

	WARN_ON_ONCE(cgroup_task_frozen(tsk));
	if (unlikely(!(tsk->flags & PF_KTHREAD) &&
		     test_bit(CGRP_FREEZE, &task_dfl_cgroup(tsk)->flags)))
		/*
		 * 冻结计数包含任务状态；成员死亡可能令整个 cgroup 达到
		 * frozen，必须在仍持 css_set_lock 的一致视图中重新传播。
		 */
		cgroup_update_frozen(task_dfl_cgroup(tsk));

	spin_unlock_irqrestore(&css_set_lock, flags);
}

#ifdef CONFIG_PREEMPT_RT
/*
 * cgroup_task_dead() is called from finish_task_switch() which doesn't allow
 * scheduling even in RT. As the task_dead path requires grabbing css_set_lock,
 * this lead to sleeping in the invalid context warning bug. css_set_lock is too
 * big to become a raw_spinlock. The task_dead path doesn't need to run
 * synchronously but can't be delayed indefinitely either as the dead task pins
 * the cgroup and task_struct can be pinned indefinitely. Bounce through lazy
 * irq_work to allow batching while ensuring timely completion.
 */
/*
 * PREEMPT_RT 的 finish_task_switch 仍不可调度，而 css_set_lock 可能睡眠；
 * 同步摘链会触发非法上下文。per-CPU lazy irq_work 批量转交到合法上下文，
 * 同时及时释放 dead task 对 cgroup/task_struct 的长期钉住。
 */
static DEFINE_PER_CPU(struct llist_head, cgrp_dead_tasks);
static DEFINE_PER_CPU(struct irq_work, cgrp_dead_tasks_iwork);

/*
 * cgrp_dead_tasks_iwork_fn() - 批量消费本 CPU 的 RT task-dead 延迟队列。
 *
 * iwork 标识当前 CPU 的 lazy irq_work，为借用；队列中每个 task 都持有
 * 入队引用。函数原子取走当前批次，逐项执行同步摘链并 put task 引用；
 * 返回无直接值。新并发入队留给下一轮，不会与已取走链表混合。
 */
static void cgrp_dead_tasks_iwork_fn(struct irq_work *iwork)
{
	struct llist_node *lnode;
	struct task_struct *task, *next;

	/*
	 * 无锁取走本 CPU 全批任务，既摊薄 irq_work 开销，也允许新加入者留
	 * 给下一轮。每个节点入队前额外 get_task_struct()，处理后对称 put。
	 */
	lnode = llist_del_all(this_cpu_ptr(&cgrp_dead_tasks));
	llist_for_each_entry_safe(task, next, lnode, cg_dead_lnode) {
		do_cgroup_task_dead(task);
		put_task_struct(task);
	}
}

/*
 * cgroup_rt_init() - 为每个 possible CPU 初始化 RT 延迟死亡队列。
 *
 * 入参：无；仅在启动期调用，尚无任务进入这些队列，可安全直接
 * 初始化。
 * 返回：无直接返回值；发布 per-CPU llist 与 irq_work，供调度尾部使用。
 */
static void __init cgroup_rt_init(void)
{
	int cpu;

	/* possible CPU 均预建队列，避免 hotplug/死亡路径临时分配。 */
	for_each_possible_cpu(cpu) {
		init_llist_head(per_cpu_ptr(&cgrp_dead_tasks, cpu));
		per_cpu(cgrp_dead_tasks_iwork, cpu) =
			IRQ_WORK_INIT_LAZY(cgrp_dead_tasks_iwork_fn);
	}
}

/*
 * cgroup_task_dead() - PREEMPT_RT 下异步安排 task 的 cgroup 摘链。
 *
 * task 是 finish_task_switch() 提供的借用对象；函数通过 get_task_struct()
 * 把生命周期延长到 irq_work 消费完成。当前上下文不可睡眠，只做 per-CPU
 * 无锁入队。返回无直接值；真正状态变化由 cgrp_dead_tasks_iwork_fn 完成。
 */
void cgroup_task_dead(struct task_struct *task)
{
	/*
	 * PREEMPT_RT 下 finish_task_switch() 不能获取可能睡眠化的
	 * css_set_lock，故只执行 per-CPU 无锁入队，再由 lazy irq_work
	 * 转到合法上下文。task 引用覆盖排队到消费之间的生命周期。
	 */
	get_task_struct(task);
	llist_add(&task->cg_dead_lnode, this_cpu_ptr(&cgrp_dead_tasks));
	irq_work_queue(this_cpu_ptr(&cgrp_dead_tasks_iwork));
}
#else	/* CONFIG_PREEMPT_RT */
/* 非 PREEMPT_RT 无需延迟队列；启动 stub 无入参、返回值或副作用。 */
static void __init cgroup_rt_init(void) {}

/*
 * cgroup_task_dead() - 非 RT 配置下同步完成死亡 task 的成员摘除。
 *
 * task 在 finish_task_switch() 调用期间稳定；普通 spinlock 可在此上下文
 * 获取，因此直接调用 do_cgroup_task_dead()。返回无直接值，不转移引用。
 */
void cgroup_task_dead(struct task_struct *task)
{
	/* 非 RT 的 css_set_lock 是普通自旋锁，可在 task-switch 尾部同步处理。 */
	do_cgroup_task_dead(task);
}
#endif	/* CONFIG_PREEMPT_RT */

/*
 * cgroup_task_release() - 在 task_struct 回收前通知具有 release 回调的控制器。
 *
 * task 为释放路径借用，仍可读取其最终 cgroup 归属；函数不改变 core
 * 成员链、不取得引用，按只读回调位图分派。返回无直接值；controller
 * 负责清理自身 task 状态，回调必须符合释放上下文约束。
 */
void cgroup_task_release(struct task_struct *task)
{
	struct cgroup_subsys *ss;
	int ssid;

	/*
	 * release 回调属于 task_struct 释放前的 controller 通知阶段，不负责
	 * css_set 成员关系；各 controller 只能清理由自己持有的 task 状态。
	 */
	do_each_subsys_mask(ss, ssid, have_release_callback) {
		ss->release(task);
	} while_each_subsys_mask();
}

/*
 * cgroup_task_free() - 清除 task 的最后迭代器链接并归还 css_set 引用。
 *
 * task 即将释放且调用者独占其生命周期；若仍位于 dying_tasks，函数取得
 * css_set_lock 让所有 iterator 跳过它并摘链。返回无直接值；最后 put
 * 消费 task 对 css_set 的归属引用，之后不得再读取 task->cgroups。
 */
void cgroup_task_free(struct task_struct *task)
{
	struct css_set *cset = task_css_set(task);

	if (!list_empty(&task->cg_list)) {
		/*
		 * 正常路径已在 task_dead 摘链；仍在 dying_tasks 的 leader 会到
		 * 此处最终摘除。先让活跃迭代器跳过该 task，再删链，
		 * 避免游标持有即将释放的 task_struct。
		 */
		spin_lock_irq(&css_set_lock);
		css_set_skip_task_iters(task_css_set(task), task);
		list_del_init(&task->cg_list);
		spin_unlock_irq(&css_set_lock);
	}

	/* cgroup_fork/post_fork 为任务归属持有的 cset 引用到此最终归还。 */
	put_css_set(cset);
}

/*
 * 解析 cgroup_disable= 的逗号列表，在任何层级注册前关闭 controller
 * static key 或可选特性。返回 1 表示启动参数已被消费；未知 token 被
 * 忽略，以允许同一内核命令行跨不同配置复用。
 */
static int __init cgroup_disable(char *str)
{
	struct cgroup_subsys *ss;
	char *token;
	int i;

	while ((token = strsep(&str, ",")) != NULL) {
		if (!*token)
			continue;

		/*
		 * 第一类 token 是 controller 名称：关闭 static key，使后续
		 * 注册、挂载和热路径都把该 controller 当作编译存在但禁用。
		 */
		for_each_subsys(ss, i) {
			if (strcmp(token, ss->name) &&
			    strcmp(token, ss->legacy_name))
				continue;

			static_branch_disable(cgroup_subsys_enabled_key[i]);
			pr_info("Disabling %s control group subsystem\n",
				ss->name);
		}

		/*
		 * 第二类 token 是 core 可选 feature：记录到全局 mask。
		 * controller 和 feature 名称空间分开扫描，便于兼容 legacy_name。
		 */
		for (i = 0; i < OPT_FEATURE_COUNT; i++) {
			if (strcmp(token, cgroup_opt_feature_names[i]))
				continue;
			cgroup_feature_disable_mask |= 1 << i;
			pr_info("Disabling %s control group feature\n",
				cgroup_opt_feature_names[i]);
			break;
		}
	}
	return 1;
}
__setup("cgroup_disable=", cgroup_disable);

/*
 * enable_debug_cgroup() - 为架构或配置提供 cgroup_debug 启用扩展点。
 *
 * 入参、返回值和 core 默认副作用均无；启动期强实现可以覆盖弱符号，
 * 但不得依赖运行期再次调用。
 */
void __init __weak enable_debug_cgroup(void) { }

/*
 * enable_cgroup_debug() - 消费 cgroup_debug 启动参数并启用调试模式。
 *
 * str 为启动参数框架借用，本开关不读取其内容。启动期设置只读
 * 热点布尔值并调用弱扩展钩子；返回 1 表示参数已处理，无失败回滚。
 */
static int __init enable_cgroup_debug(char *str)
{
	/*
	 * 弱符号钩子允许具体架构或配置在不改核心代码的情况下附加
	 * 调试启用。
	 */
	cgroup_debug = true;
	enable_debug_cgroup();
	return 1;
}
__setup("cgroup_debug", enable_cgroup_debug);

/*
 * cgroup_favordynmods_setup() - 解析动态控制器模块偏好启动参数。
 *
 * str 是 NUL 结尾的内核布尔字符串借用；成功写入
 * __ro_after_init 策略并返回 1，格式错误不改变可靠配置并返回 0。
 * 仅启动期调用，可睡眠性不构成运行期约束。
 */
static int __init cgroup_favordynmods_setup(char *str)
{
	/*
	 * 仅接受内核布尔语法；解析失败返回 0，让启动参数框架报告
	 * 未处理。
	 */
	return (kstrtobool(str, &have_favordynmods) == 0);
}
__setup("cgroup_favordynmods=", cgroup_favordynmods_setup);

/**
 * css_tryget_online_from_dir - get corresponding css from a cgroup dentry
 * @dentry: directory dentry of interest
 * @ss: subsystem of interest
 *
 * If @dentry is a directory for a cgroup which has @ss enabled on it, try
 * to get the corresponding css and return it.  If such css doesn't exist
 * or can't be pinned, an ERR_PTR value is returned.
 */
/*
 * 中文契约：
 * dentry 是调用期间稳定的目录项借用；ss 可为 NULL，表示 core self css。
 * 函数不要求请求源自 kernfs，因此在 RCU 下读取 kn->priv，再以
 * css_tryget_online() 取得可带出临界区的引用。成功返回持有引用的 css，
 * 调用者必须 css_put()；非 cgroup 目录返回 -EBADF，不存在或离线返回
 * -ENOENT。函数不修改层级，也不保证 css 字段内容冻结。
 */
struct cgroup_subsys_state *css_tryget_online_from_dir(struct dentry *dentry,
						       struct cgroup_subsys *ss)
{
	struct kernfs_node *kn = kernfs_node_from_dentry(dentry);
	struct file_system_type *s_type = dentry->d_sb->s_type;
	struct cgroup_subsys_state *css = NULL;
	struct cgroup *cgrp;

	/* is @dentry a cgroup dir? */
	/*
	 * 先验证 superblock 类型与节点类型，拒绝普通 kernfs 目录及
	 * cgroup 控制文件。@ss 为 NULL 时 cgroup_css() 选择 self css。
	 */
	if ((s_type != &cgroup_fs_type && s_type != &cgroup2_fs_type) ||
	    !kn || kernfs_type(kn) != KERNFS_DIR)
		return ERR_PTR(-EBADF);

	rcu_read_lock();

	/*
	 * This path doesn't originate from kernfs and @kn could already
	 * have been or be removed at any point.  @kn->priv is RCU
	 * protected for this access.  See css_release_work_fn() for details.
	 */
	/*
	 * 此入口绕过 kernfs active ref，kn 可并发 remove；RCU 只保证 priv
	 * 读取期内存安全，随后仍须 tryget_online 才能获得长期 css 引用。
	 */
	cgrp = rcu_dereference(*(void __rcu __force **)&kn->priv);
	if (cgrp)
		css = cgroup_css(cgrp, ss);

	/*
	 * css_tryget_online() 把 RCU 下的裸指针提升为稳定在线引用。若销毁
	 * 已 kill percpu_ref，即使 priv 尚未清除也会失败，封闭 rmdir 竞态。
	 */
	if (!css || !css_tryget_online(css))
		css = ERR_PTR(-ENOENT);

	rcu_read_unlock();
	return css;
}

/**
 * css_from_id - lookup css by id
 * @id: the cgroup id
 * @ss: cgroup subsys to be looked into
 *
 * Returns the css if there's valid one with @id, otherwise returns NULL.
 * Should be called under rcu_read_lock().
 */
/*
 * 中文契约：
 * id 是 ss 私有 IDR 中的 css ID，ss 为稳定 subsystem 借用。调用者必须
 * 持 RCU；成功返回仅为临界区内裸指针，未增加引用，槽为空返回 NULL。
 * 若需跨出 RCU 使用，调用者必须另行 css_tryget_online()/css_get。
 */
struct cgroup_subsys_state *css_from_id(int id, struct cgroup_subsys *ss)
{
	/*
	 * IDR 槽在 release 阶段以 NULL 替换，最终 free 才移除；调用者必须
	 * 已持 RCU，并自行 tryget 才能把结果带出临界区。
	 */
	WARN_ON_ONCE(!rcu_read_lock_held());
	return idr_find(&ss->css_idr, id);
}

/**
 * cgroup_get_from_path - lookup and get a cgroup from its default hierarchy path
 * @path: path on the default hierarchy
 *
 * Find the cgroup at @path on the default hierarchy, increment its
 * reference count and return it.  Returns pointer to the found cgroup on
 * success, ERR_PTR(-ENOENT) if @path doesn't exist or if the cgroup has already
 * been released and ERR_PTR(-ENOTDIR) if @path points to a non-directory.
 */
/*
 * 中文契约：
 * path 是相对 current default cgroup namespace 根的 NUL 结尾路径借用。
 * kernfs walk 可睡眠；成功返回持有引用的 cgroup，调用者必须 put。
 * 不存在、已 release 返回 -ENOENT，目标非目录返回 -ENOTDIR；任何失败都
 * 已归还 kernfs 节点引用，不产生层级副作用。
 */
struct cgroup *cgroup_get_from_path(const char *path)
{
	struct kernfs_node *kn;
	struct cgroup *cgrp = ERR_PTR(-ENOENT);
	struct cgroup *root_cgrp;

	root_cgrp = current_cgns_cgroup_dfl();
	/*
	 * 从当前 namespace 的 default root 开始 walk，天然禁止使用绝对
	 * default hierarchy 路径越出可见子树。kernfs 引用只稳定 kn。
	 */
	kn = kernfs_walk_and_get(root_cgrp->kn, path);
	if (!kn)
		goto out;

	if (kernfs_type(kn) != KERNFS_DIR) {
		cgrp = ERR_PTR(-ENOTDIR);
		goto out_kernfs;
	}

	rcu_read_lock();

	/* 与 ID 查询相同：RCU 读 priv，再用 tryget 固化 cgroup 生命周期。 */
	cgrp = rcu_dereference(*(void __rcu __force **)&kn->priv);
	if (!cgrp || !cgroup_tryget(cgrp))
		cgrp = ERR_PTR(-ENOENT);

	rcu_read_unlock();

out_kernfs:
	kernfs_put(kn);
out:
	return cgrp;
}
EXPORT_SYMBOL_GPL(cgroup_get_from_path);

/**
 * cgroup_v1v2_get_from_fd - get a cgroup pointer from a fd
 * @fd: fd obtained by open(cgroup_dir)
 *
 * Find the cgroup from a fd which should be obtained
 * by opening a cgroup directory.  Returns a pointer to the
 * cgroup on success. ERR_PTR is returned if the cgroup
 * cannot be found.
 */
/*
 * 中文契约：
 * fd 必须引用已打开的 cgroup1/v2 目录，函数通过 cleanup class 临时固定
 * file。成功返回独立持有引用的 cgroup，fd 随后关闭不影响它；调用者负责
 * put。无效 fd、非目录或离线对象返回错误指针，函数不改变 fd 所有权。
 */
struct cgroup *cgroup_v1v2_get_from_fd(int fd)
{
	/*
	 * fd_raw 的清理类只临时固定 struct file；成功返回的 cgroup 引用由
	 * cgroup_v1v2_get_from_file() 取得，生命周期独立于 fd 是否随后关闭。
	 */
	CLASS(fd_raw, f)(fd);
	if (fd_empty(f))
		return ERR_PTR(-EBADF);

	return cgroup_v1v2_get_from_file(fd_file(f));
}

/**
 * cgroup_get_from_fd - same as cgroup_v1v2_get_from_fd, but only supports
 * cgroup2.
 * @fd: fd obtained by open(cgroup2_dir)
 */
/*
 * 中文契约：
 * fd 与返回引用规则同 cgroup_v1v2_get_from_fd()，但仅接受 cgroup2
 * default hierarchy。v1 对象会归还临时引用并返回 -EBADF；成功对象由
 * 调用者 cgroup_put()。
 */
struct cgroup *cgroup_get_from_fd(int fd)
{
	struct cgroup *cgrp = cgroup_v1v2_get_from_fd(fd);

	if (IS_ERR(cgrp))
		return ERR_CAST(cgrp);

	if (!cgroup_on_dfl(cgrp)) {
		cgroup_put(cgrp);
		return ERR_PTR(-EBADF);
	}
	return cgrp;
}
EXPORT_SYMBOL_GPL(cgroup_get_from_fd);

/*
 * power_of_ten() - 计算十进制定点缩放因子。
 *
 * power 是非负十进制指数，由解析器的小数位差产生；函数不睡眠、无共享
 * 状态。返回 10^power，调用者保证结果不超出 u64。
 */
static u64 power_of_ten(int power)
{
	/*
	 * 仅供受控的小数位数缩放使用；调用者保证 power 非负且乘法
	 * 不溢出。
	 */
	u64 v = 1;
	while (power--)
		v *= 10;
	return v;
}

/**
 * cgroup_parse_float - parse a floating number
 * @input: input string
 * @dec_shift: number of decimal digits to shift
 * @v: output
 *
 * Parse a decimal floating point number in @input and store the result in
 * @v with decimal point right shifted @dec_shift times.  For example, if
 * @input is "12.3456" and @dec_shift is 3, *@v will be set to 12345.
 * Returns 0 on success, -errno otherwise.
 *
 * There's nothing cgroup specific about this function except that it's
 * currently the only user.
 */
/*
 * 中文契约：
 * input 是 NUL 结尾十进数字符串借用；dec_shift 是目标小数位数；v 为
 * 必须非 NULL 的 s64 输出，不转移 ownership。函数把小数点右移并对多余
 * 位最近舍入。成功写入 *v 并返回 0；格式非法或负小数部分返回 -EINVAL，
 * 失败时 *v 保持原值。调用者负责输入范围不会导致乘法溢出。
 */
int cgroup_parse_float(const char *input, unsigned dec_shift, s64 *v)
{
	s64 whole, frac = 0;
	int fstart = 0, fend = 0, flen;

	if (!sscanf(input, "%lld.%n%lld%n", &whole, &fstart, &frac, &fend))
		return -EINVAL;
	/*
	 * %n 记录小数数字的边界，保留前导零所表达的精度；负号只能属于
	 * whole，拒绝负 frac，避免 "1.-2" 被组合成意外结果。
	 */
	if (frac < 0)
		return -EINVAL;

	flen = fend > fstart ? fend - fstart : 0;
	if (flen < dec_shift)
		frac *= power_of_ten(dec_shift - flen);
	else
		/*
		 * 输入精度高于输出刻度时做最近舍入，而非直接截断。
		 * 例如四位小数缩到三位会舍入最低位。调用者负责约束
		 * 不会发生 s64 溢出。
		 */
		frac = DIV_ROUND_CLOSEST_ULL(frac, power_of_ten(flen - dec_shift));

	*v = whole * power_of_ten(dec_shift) + frac;
	return 0;
}

/*
 * sock->sk_cgrp_data handling.  For more info, see sock_cgroup_data
 * definition in cgroup-defs.h.
 */
/*
 * sock_cgroup_data 把 socket 固定到创建时 default cgroup；普通引用保护
 * cgroup 生命周期，独立 BPF 引用保护 per-cgroup socket/BPF 状态。
 */
#ifdef CONFIG_SOCK_CGROUP_DATA

/*
 * cgroup_sk_alloc() - 为新 socket 固定创建时的 default cgroup 归属。
 *
 * skcd 是待初始化输出对象，调用者拥有；进程上下文继承 current cgroup，
 * 中断上下文固定 root。函数在 RCU 下重试取得普通 cgroup 引用，并增加
 * BPF 存储引用；返回无直接值。两份引用均转交 skcd，由 cgroup_sk_free()
 * 释放。路径不分配内存、不睡眠。
 */
void cgroup_sk_alloc(struct sock_cgroup_data *skcd)
{
	struct cgroup *cgroup;

	rcu_read_lock();
	/* Don't associate the sock with unrelated interrupted task's cgroup. */
	/* 中断打断的 current 与 socket 无语义关系，因此统一归 default root。 */
	if (in_interrupt()) {
		/*
		 * 中断可能打断任意任务，current 的 cgroup 与 socket 创建者无
		 * 语义关系；统一归 default root，避免随机继承被打断任务。
		 */
		cgroup = &cgrp_dfl_root.cgrp;
		cgroup_get(cgroup);
		goto out;
	}

	while (true) {
		struct css_set *cset;

		cset = task_css_set(current);
		/*
		 * 迁移/销毁可令刚读到的 cgroup 进入 dying。tryget 失败便重读
		 * current cset；RCU 保证循环中的裸指针内存仍可访问。
		 */
		if (likely(cgroup_tryget(cset->dfl_cgrp))) {
			cgroup = cset->dfl_cgrp;
			break;
		}
		cpu_relax();
	}
out:
	/*
	 * socket 同时持普通 cgroup 生命周期引用和 BPF 存储引用；free 必须
	 * 分别归还，二者服务于不同对象的存活条件。
	 */
	skcd->cgroup = cgroup;
	cgroup_bpf_get(cgroup);
	rcu_read_unlock();
}

/*
 * cgroup_sk_clone() - 为克隆 socket 增加已继承 cgroup 的两类引用。
 *
 * skcd 已复制原 socket 的 cgroup 指针，为调用者所有；目标即使 rmdir
 * 也仍可被引用。函数不睡眠、无失败返回，增加普通与 BPF 引用，之后由
 * 新 socket 的 cgroup_sk_free() 对称归还。
 */
void cgroup_sk_clone(struct sock_cgroup_data *skcd)
{
	struct cgroup *cgrp = sock_cgroup_ptr(skcd);

	/*
	 * We might be cloning a socket which is left in an empty
	 * cgroup and the cgroup might have already been rmdir'd.
	 * Don't use cgroup_get_live().
	 */
	/*
	 * clone 继承原 socket 的归属，即便目录已 rmdir 也必须继续引用该
	 * dying cgroup，不能要求 live/online，否则无法复制已有 socket。
	 */
	cgroup_get(cgrp);
	cgroup_bpf_get(cgrp);
}

/*
 * cgroup_sk_free() - 释放 socket 持有的 BPF 与 cgroup 生命周期引用。
 *
 * skcd 必须完成 alloc/clone 且尚未 free；函数借用其中指针，先释放 BPF
 * 关联再 put cgroup。返回无直接值，调用后 skcd->cgroup 不得再解引用。
 */
void cgroup_sk_free(struct sock_cgroup_data *skcd)
{
	struct cgroup *cgrp = sock_cgroup_ptr(skcd);

	/* 与 alloc/clone 的两类引用严格对称，BPF 状态先于 cgroup 引用释放。 */
	cgroup_bpf_put(cgrp);
	cgroup_put(cgrp);
}

#endif	/* CONFIG_SOCK_CGROUP_DATA */
/* CONFIG_SOCK_CGROUP_DATA=n 时上述 socket 归属接口不参与编译。 */

#ifdef CONFIG_SYSFS
/*
 * show_delegatable_files() - 把一组可委托 cftype 名称追加到 sysfs 缓冲。
 *
 * files 是空 name 结尾的只读表；buf/size 描述剩余输出区；prefix 可空，
 * 非空时为 controller 名。函数不持有这些对象、不睡眠。返回追加的字节
 * 数；静态预算不足只 WARN 并停止，调用者以返回长度继续拼接。
 */
static ssize_t show_delegatable_files(struct cftype *files, char *buf,
				      ssize_t size, const char *prefix)
{
	struct cftype *cft;
	ssize_t ret = 0;

	/*
	 * 只导出标记为 namespace 可委托的 ABI 文件；controller 文件加
	 * "controller." 前缀，核心文件无前缀。返回累计写入长度供调用者
	 * 继续追加，WARN 负责发现静态 PAGE_SIZE 预算估算失效。
	 */
	for (cft = files; cft && cft->name[0] != '\0'; cft++) {
		if (!(cft->flags & CFTYPE_NS_DELEGATABLE))
			continue;

		if (prefix)
			ret += snprintf(buf + ret, size - ret, "%s.", prefix);

		ret += snprintf(buf + ret, size - ret, "%s\n", cft->name);

		if (WARN_ON(ret >= size))
			break;
	}

	return ret;
}

/*
 * delegate_show() - 输出内核支持的 cgroup2 namespace 委托文件清单。
 *
 * kobj/attr 是 sysfs 借用且不影响选择；buf 是 PAGE_SIZE 输出。函数遍历
 * core、可选 PSI 和全部 controller 的 dfl_cftypes，返回写入字节数；
 * 无 errno 出口和 ownership 变化，内容描述 ABI 能力而非某节点当前配置。
 */
static ssize_t delegate_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	struct cgroup_subsys *ss;
	int ssid;
	ssize_t ret = 0;

	/*
	 * /sys/kernel/cgroup/delegate 是委托工具的能力清单：先列核心/PSI，
	 * 再列各 controller 的 default-hierarchy 文件。它描述 ABI，而不
	 * 表示当前具体 cgroup 已启用这些 controller。
	 */
	ret = show_delegatable_files(cgroup_base_files, buf + ret,
				     PAGE_SIZE - ret, NULL);
	if (cgroup_psi_enabled())
		ret += show_delegatable_files(cgroup_psi_files, buf + ret,
					      PAGE_SIZE - ret, NULL);

	for_each_subsys(ss, ssid)
		ret += show_delegatable_files(ss->dfl_cftypes, buf + ret,
					      PAGE_SIZE - ret,
					      cgroup_subsys_name[ssid]);

	return ret;
}
static struct kobj_attribute cgroup_delegate_attr = __ATTR_RO(delegate);

/*
 * features_show() - 输出当前内核构建支持的 cgroup2 功能名称。
 *
 * kobj/attr 为未使用的 sysfs 借用参数，buf 为 PAGE_SIZE 输出。返回写入
 * 字节数，无失败分支、锁和引用变化；列表不是运行时 mount option 状态。
 */
static ssize_t features_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	/*
	 * features 是内核支持的 mount/行为选项清单，不读取运行时启用状态。
	 * 用户空间据此避免依靠内核版本号猜测能力。
	 */
	return snprintf(buf, PAGE_SIZE,
			"nsdelegate\n"
			"favordynmods\n"
			"memory_localevents\n"
			"memory_recursiveprot\n"
			"memory_hugetlb_accounting\n"
			"pids_localevents\n");
}
static struct kobj_attribute cgroup_features_attr = __ATTR_RO(features);

static struct attribute *cgroup_sysfs_attrs[] = {
	&cgroup_delegate_attr.attr,
	&cgroup_features_attr.attr,
	NULL,
};

static const struct attribute_group cgroup_sysfs_attr_group = {
	.attrs = cgroup_sysfs_attrs,
	.name = "cgroup",
};

/*
 * cgroup_sysfs_init() - 在 /sys/kernel/cgroup 下注册能力属性组。
 *
 * 入参：无；subsys_initcall 的进程上下文可睡眠。成功返回 0；失败返回
 * sysfs errno，属性组不发布。成功后的 kobject/sysfs 生命周期由核心管理。
 */
static int __init cgroup_sysfs_init(void)
{
	/* subsystem init 阶段在 /sys/kernel 下原子注册 cgroup 属性组。 */
	return sysfs_create_group(kernel_kobj, &cgroup_sysfs_attr_group);
}
subsys_initcall(cgroup_sysfs_init);

#endif /* CONFIG_SYSFS */
/* CONFIG_SYSFS=n 时不注册 /sys/kernel/cgroup 能力属性组。 */
