// SPDX-License-Identifier: GPL-2.0
#include <linux/init_task.h>
#include <linux/export.h>
#include <linux/mqueue.h>
#include <linux/sched.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/rt.h>
#include <linux/sched/task.h>
#include <linux/sched/ext.h>
#include <linux/sched/exec_state.h>
#include <linux/user_namespace.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/audit.h>
#include <linux/numa.h>
#include <linux/scs.h>
#include <linux/plist.h>

#include <linux/uaccess.h>

/*
 * 初始任务对象地图：这些对象在链接期静态构造，供最早运行的 idle/swapper
 * task 使用；此时 slab、普通 fork 和动态引用获取尚不可依赖。init_task 自指
 * parent/group_leader，借用永久的 init_mm、init_fs、init_files、init_nsproxy
 * 和初始凭据。多数对象通过额外引用永久钉住，不能走普通进程退出释放路径。
 */

/*
 * init_signals 表示初始线程组共享的 signal_struct，静态存活且不动态释放。
 * nr_threads/thread_head 建立仅含 init_task 的线程组；wait/pending/timer 链表
 * 必须在调度和信号子系统启用前即为合法空表。各锁保护运行期 exec、凭据、
 * cgroup 和定时器状态；PID/TGID/PGID/SID 初始都指向永久 init_struct_pid。
 * 未显式列出的计数器、统计量和可选状态由 C 静态初始化规则置零。
 */
static struct signal_struct init_signals = {
	/* 初始线程组只有 init_task；thread_head 与 task.thread_node 互相闭合。 */
	.nr_threads	= 1,
	.thread_head	= LIST_HEAD_INIT(init_task.thread_node),
	/* 父进程等待子进程状态变化的 waitqueue，启动时没有 waiter。 */
	.wait_chldexit	= __WAIT_QUEUE_HEAD_INITIALIZER(init_signals.wait_chldexit),
	/* 线程组共享 pending signal 队列和位图均为空。 */
	.shared_pending	= {
		.list = LIST_HEAD_INIT(init_signals.shared_pending.list),
		.signal =  {{0}}
	},
	/* 跨进程信号/操作的临时 multiprocess 队列起始为空。 */
	.multiprocess	= HLIST_HEAD_INIT,
	/* 使用架构/内核定义的初始资源限制表，之后可按进程组语义修改。 */
	.rlim		= INIT_RLIMITS,
#ifdef CONFIG_CGROUPS
	/* 序列化线程组整体迁移与 fork/exit 对 cgroup membership 的观察。 */
	.cgroup_threadgroup_rwsem	= __RWSEM_INITIALIZER(init_signals.cgroup_threadgroup_rwsem),
#endif
	/* cred_guard_mutex 协调 exec/ptrace/凭据边界；exec_update_lock 保护 exec 状态更新。 */
	.cred_guard_mutex = __MUTEX_INITIALIZER(init_signals.cred_guard_mutex),
	.exec_update_lock = __RWSEM_INITIALIZER(init_signals.exec_update_lock),
#ifdef CONFIG_POSIX_TIMERS
	/* POSIX timer 两类链表为空，CPU timer 原子累计器从零开始。 */
	.posix_timers		= HLIST_HEAD_INIT,
	.ignored_posix_timers	= HLIST_HEAD_INIT,
	.cputimer		= {
		.cputime_atomic	= INIT_CPUTIME_ATOMIC,
	},
#endif
	/* 初始化三类 CPU timer 队列，即使当前没有任何定时器也必须是合法表头。 */
	INIT_CPU_TIMERS(init_signals)
	/* 初始 task 的四种 PID 角色共享同一个静态 struct pid。 */
	.pids = {
		[PIDTYPE_PID]	= &init_struct_pid,
		[PIDTYPE_TGID]	= &init_struct_pid,
		[PIDTYPE_PGID]	= &init_struct_pid,
		[PIDTYPE_SID]	= &init_struct_pid,
	},
	/* 进程级累计 CPU 时间的单调快照从零态开始。 */
	INIT_PREV_CPUTIME(init_signals)
};

/*
 * init_sighand 是初始线程组共享的信号处理表。引用计数 1 对应 init_task；所有
 * disposition 默认为 SIG_DFL。siglock 保护 action/pending 相关修改，signalfd
 * waitqueue 供运行期消费者等待；对象随初始任务永久存在。
 */
static struct sighand_struct init_sighand = {
	.count		= REFCOUNT_INIT(1),
	.action		= { { { .sa_handler = SIG_DFL, } }, },
	.siglock	= __SPIN_LOCK_UNLOCKED(init_sighand.siglock),
	.signalfd_wqh	= __WAIT_QUEUE_HEAD_INITIALIZER(init_sighand.signalfd_wqh),
};

/* init to 2 - one for init_task, one to ensure it is never freed */
/*
 * 引用初始化为 2：一份属于 init_task，另一份永久钉住对象，确保初始 exec
 * 状态不会释放。默认仅 owner 可 dump，user_ns 借用永久 init_user_ns。
 */
struct task_exec_state init_task_exec_state = {
	.count		= REFCOUNT_INIT(2),
	.dumpable	= TASK_DUMPABLE_OWNER,
	.user_ns	= &init_user_ns,
};

#ifdef CONFIG_SHADOW_CALL_STACK
/*
 * 初始 task 的静态 shadow call stack。数组大小为 SCS_SIZE，最后一个槽写入
 * SCS_END_MAGIC 作为越界/完整性哨兵；对象贯穿初始 task 生命周期。
 */
unsigned long init_shadow_call_stack[SCS_SIZE / sizeof(long)] = {
	[(SCS_SIZE / sizeof(long)) - 1] = SCS_END_MAGIC
};
#endif

/* init to 2 - one for init_task, one to ensure it is never freed */
/* 引用 2 同样由 init_task 持有一份并永久钉住一份，初始 supplementary groups 为空。 */
static struct group_info init_groups = { .usage = REFCOUNT_INIT(2) };

/*
 * The initial credentials for the initial task
 */
/*
 * 初始任务的凭据：所有 UID/GID 角色都是 initial user namespace 的全局 root，
 * permitted/effective/bounding capability 为全集，inheritable 为空。real_cred 与
 * cred 均指向本对象，usage=4 还包含启动期永久钉住引用；user、user_ns、
 * group_info 和 ucounts 都借用相应永久初始对象。运行期凭据替换采用 copy-on-write，
 * 不会原地把这份全局初始凭据变成普通用户凭据。
 */
static struct cred init_cred = {
	.usage			= ATOMIC_INIT(4),
	.uid			= GLOBAL_ROOT_UID,
	.gid			= GLOBAL_ROOT_GID,
	.suid			= GLOBAL_ROOT_UID,
	.sgid			= GLOBAL_ROOT_GID,
	.euid			= GLOBAL_ROOT_UID,
	.egid			= GLOBAL_ROOT_GID,
	.fsuid			= GLOBAL_ROOT_UID,
	.fsgid			= GLOBAL_ROOT_GID,
	.securebits		= SECUREBITS_DEFAULT,
	.cap_inheritable	= CAP_EMPTY_SET,
	.cap_permitted		= CAP_FULL_SET,
	.cap_effective		= CAP_FULL_SET,
	.cap_bset		= CAP_FULL_SET,
	.user			= INIT_USER,
	.user_ns		= &init_user_ns,
	.group_info		= &init_groups,
	.ucounts		= &init_ucounts,
};

/*
 * Set up the first task table, touch at your own risk!. Base=0,
 * limit=0x1fffff (=2MB)
 */
/*
 * 建立第一个 task_struct，修改必须极其谨慎。历史注释中的地址段基址为 0、
 * 上限 0x1fffff（2 MiB）。该对象不是 fork 分配出来的：调度器、PID、信号、
 * 凭据、namespace 和 VFS 在动态初始化前都以这里的自指/静态引用为根。
 *
 * 生命周期：链接期创建，usage=2 包含活动引用与永久钉住引用，永不走普通
 * task_struct 最终释放。并发：初始化阶段单线程；运行期各字段分别由 rq lock、
 * pi_lock、alloc_lock、tasklist_lock、RCU、signal/sighand 锁等所属协议保护。
 * 未列字段依靠静态零初始化；这里仅显式写出必须为非零、非空或特殊哨兵的状态。
 */
struct task_struct init_task __aligned(L1_CACHE_BYTES) = {
#ifdef CONFIG_THREAD_INFO_IN_TASK
	/* thread_info 内嵌配置下直接初始化，并以一份 stack 引用稳定 init_stack。 */
	.thread_info	= INIT_THREAD_INFO(init_task),
	.stack_refcount	= REFCOUNT_INIT(1),
#endif
	/* 初始 task 从可运行状态和静态 init_stack 开始；usage 额外钉住防止释放。 */
	.__state	= 0,
	.stack		= init_stack,
	.usage		= REFCOUNT_INIT(2),
	/* swapper 没有用户地址空间，按内核线程处理。 */
	.flags		= PF_KTHREAD,
	/* 普通策略默认 nice 0 对应 MAX_PRIO-20，三个优先级视图初始一致。 */
	.prio		= MAX_PRIO - 20,
	.static_prio	= MAX_PRIO - 20,
	.normal_prio	= MAX_PRIO - 20,
	.policy		= SCHED_NORMAL,
	/* cpus_ptr 指向对象内全 CPU mask；尚无用户指定 affinity 覆盖。 */
	.cpus_ptr	= &init_task.cpus_mask,
	.user_cpus_ptr	= NULL,
	.cpus_mask	= CPU_MASK_ALL,
	/* 最大容量为完整 scale，并允许所有 NR_CPUS 可能 CPU。 */
	.max_allowed_capacity	= SCHED_CAPACITY_SCALE,
	.nr_cpus_allowed= NR_CPUS,
	/* 内核线程没有自有 mm，但借用 init_mm 作为 active_mm 执行内核映射。 */
	.mm		= NULL,
	.active_mm	= &init_mm,
	/* exec_state 指向永久初始对象；restart 默认拒绝重启当前系统调用。 */
	.exec_state	= &init_task_exec_state,
	.restart_block	= {
		.fn = do_no_restart_syscall,
	},
	/* 各调度类嵌入节点必须先成为合法空节点，尚未排入普通/RT 队列。 */
	.se		= {
		.group_node 	= LIST_HEAD_INIT(init_task.se.group_node),
	},
	.rt		= {
		.run_list	= LIST_HEAD_INIT(init_task.rt.run_list),
		.time_slice	= RR_TIMESLICE,
	},
	/* 全局 task list 以 init_task 自环为根。 */
	.tasks		= LIST_HEAD_INIT(init_task.tasks),
#ifdef CONFIG_SMP
	/* SMP 实时调度迁移链表从未入链状态开始，MAX_PRIO 是其初始排序键。 */
	.pushable_tasks	= PLIST_NODE_INIT(init_task.pushable_tasks, MAX_PRIO),
#endif
#ifdef CONFIG_CGROUP_SCHED
	/* 初始任务属于根调度组，不受子 cgroup CPU 策略约束。 */
	.sched_task_group = &root_task_group,
#endif
#ifdef CONFIG_SCHED_CLASS_EXT
	/* sched_ext 节点均为空；-1/INVALID 表示未绑定 CPU/DSQ，slice 使用默认值。 */
	.scx		= {
		.dsq_list.node	= LIST_HEAD_INIT(init_task.scx.dsq_list.node),
		.sticky_cpu	= -1,
		.holding_cpu	= -1,
		.runnable_node	= LIST_HEAD_INIT(init_task.scx.runnable_node),
		.runnable_at	= INITIAL_JIFFIES,
		.ddsp_dsq_id	= SCX_DSQ_INVALID,
		.slice		= SCX_SLICE_DFL,
	},
#endif
	/* ptrace、父子和兄弟关系以空表/自指建立没有祖先的拓扑根。 */
	.ptraced	= LIST_HEAD_INIT(init_task.ptraced),
	.ptrace_entry	= LIST_HEAD_INIT(init_task.ptrace_entry),
	.real_parent	= &init_task,
	.parent		= &init_task,
	.children	= LIST_HEAD_INIT(init_task.children),
	.sibling	= LIST_HEAD_INIT(init_task.sibling),
	.group_leader	= &init_task,
	/* real/effective cred 都由 RCU 发布为永久 init_cred，读者按凭据协议访问。 */
	RCU_POINTER_INITIALIZER(real_cred, &init_cred),
	RCU_POINTER_INITIALIZER(cred, &init_cred),
	/* 名称和架构线程寄存器/上下文由架构初始化宏提供。 */
	.comm		= INIT_TASK_COMM,
	.thread		= INIT_THREAD,
	/* 借用静态 VFS、namespace、signal/sighand 根对象，均不转移释放责任。 */
	.fs		= &init_fs,
	.files		= &init_files,
#ifdef CONFIG_IO_URING
	/* 初始内核线程没有 io_uring 上下文，后续也不能误继承用户任务的实例。 */
	.io_uring	= NULL,
#endif
	.signal		= &init_signals,
	.sighand	= &init_sighand,
	.nsproxy	= &init_nsproxy,
	/* 线程私有 pending signal 队列/位图为空，blocked mask 也为空。 */
	.pending	= {
		.list = LIST_HEAD_INIT(init_task.pending.list),
		.signal = {{0}}
	},
	.blocked	= {{0}},
	/* alloc_lock 保护部分 task 资源/亲和状态；journal_info 尚无文件系统事务。 */
	.alloc_lock	= __SPIN_LOCK_UNLOCKED(init_task.alloc_lock),
	.journal_info	= NULL,
	/* 初始化线程级 CPU timers；PI/blocked 锁在调度与锁路径启用前必须可用。 */
	INIT_CPU_TIMERS(init_task)
	.pi_lock	= __RAW_SPIN_LOCK_UNLOCKED(init_task.pi_lock),
	.blocked_lock	= __RAW_SPIN_LOCK_UNLOCKED(init_task.blocked_lock),
	.timer_slack_ns = 50000, /* 50 usec default slack */
	/* 默认定时器松弛为 50 微秒，允许非精确定时器合并唤醒以节能。 */
	/* thread_pid 与线程组链节点把 init_task 接到 init_signals 的唯一成员关系。 */
	.thread_pid	= &init_struct_pid,
	.thread_node	= LIST_HEAD_INIT(init_signals.thread_head),
#ifdef CONFIG_AUDIT
	/* 初始任务尚未由用户登录会话创建，loginuid/sessionid 使用未设置哨兵。 */
	.loginuid	= INVALID_UID,
	.sessionid	= AUDIT_SID_UNSET,
#endif
#ifdef CONFIG_PERF_EVENTS
	/* perf 事件表为空，mutex 保护后续 attach/detach。 */
	.perf_event_mutex = __MUTEX_INITIALIZER(init_task.perf_event_mutex),
	.perf_event_list = LIST_HEAD_INIT(init_task.perf_event_list),
#endif
#ifdef CONFIG_PREEMPT_RCU
	/* 初始时不在 RCU 读侧临界区、无特殊解锁工作，也未阻塞在 RCU node。 */
	.rcu_read_lock_nesting = 0,
	.rcu_read_unlock_special.s = 0,
	.rcu_node_entry = LIST_HEAD_INIT(init_task.rcu_node_entry),
	.rcu_blocked_node = NULL,
#endif
#ifdef CONFIG_TASKS_RCU
	/* Tasks RCU 未把初始任务列为 holdout/exit；idle CPU 哨兵为 -1。 */
	.rcu_tasks_holdout = false,
	.rcu_tasks_holdout_list = LIST_HEAD_INIT(init_task.rcu_tasks_holdout_list),
	.rcu_tasks_idle_cpu = -1,
	.rcu_tasks_exit_list = LIST_HEAD_INIT(init_task.rcu_tasks_exit_list),
#endif
#ifdef CONFIG_TASKS_TRACE_RCU
	/* Tasks Trace RCU 读侧嵌套从 0 开始。 */
	.trc_reader_nesting = 0,
#endif
#ifdef CONFIG_CPUSETS
	/* seqcount 与 alloc_lock 配对，保护 mems_allowed 更新快照的一致读取。 */
	.mems_allowed_seq = SEQCNT_SPINLOCK_ZERO(init_task.mems_allowed_seq,
						 &init_task.alloc_lock),
#endif
	/* 尚无代理执行 donor，也没有 RT mutex PI waiter/top task。 */
	.blocked_donor = NULL,
#ifdef CONFIG_RT_MUTEXES
	.pi_waiters	= RB_ROOT_CACHED,
	.pi_top_task	= NULL,
#endif
	/* 线程级前次 CPU 时间快照初始化为零态。 */
	INIT_PREV_CPUTIME(init_task)
#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
	/* vtime seqcount 从 0 开始，初始任务处于内核态 VTIME_SYS。 */
	.vtime.seqcount	= SEQCNT_ZERO(init_task.vtime_seqcount),
	.vtime.starttime = 0,
	.vtime.state	= VTIME_SYS,
#endif
#ifdef CONFIG_NUMA_BALANCING
	/* 没有首选 NUMA node、group 或 fault 统计，运行期首次使用再分配。 */
	.numa_preferred_nid = NUMA_NO_NODE,
	.numa_group	= NULL,
	.numa_faults	= NULL,
#endif
#ifdef CONFIG_SCHED_CACHE
	/* 尚未选择首选 LLC，也没有排队执行 locality 更新。 */
	.preferred_llc  = -1,
	.pref_llc_queued  = 0,
#endif
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	/* 最早启动阶段先抑制一层 KASAN 检查，待运行期状态准备后再平衡。 */
	.kasan_depth	= 1,
#endif
#ifdef CONFIG_KCSAN
	/* KCSAN scoped access 表用 poison/NULL 哨兵建立空状态。 */
	.kcsan_ctx = {
		.scoped_accesses	= {LIST_POISON1, NULL},
	},
#endif
#ifdef CONFIG_TRACE_IRQFLAGS
	/* 启动初态把 softirq 逻辑状态记为 enabled。 */
	.softirqs_enabled = 1,
#endif
#ifdef CONFIG_LOCKDEP
	.lockdep_depth = 0, /* no locks held yet */
	/* 尚未持锁；依赖深度、chain key 和递归保护从空状态开始。 */
	.curr_chain_key = INITIAL_CHAIN_KEY,
	.lockdep_recursion = 0,
#endif
#ifdef CONFIG_FUNCTION_GRAPH_TRACER
	/* 尚未分配函数图返回栈，暂停计数为 0。 */
	.ret_stack		= NULL,
	.tracing_graph_pause	= ATOMIC_INIT(0),
#endif
#if defined(CONFIG_TRACING) && defined(CONFIG_PREEMPTION)
	/* tracing recursion 防护计数从 0 开始。 */
	.trace_recursion = 0,
#endif
#ifdef CONFIG_LIVEPATCH
	/* 初始任务不处于 livepatch transition。 */
	.patch_state	= KLP_TRANSITION_IDLE,
#endif
#ifdef CONFIG_SECURITY
	/* LSM task blob 尚未附着，安全框架运行期按需建立。 */
	.security	= NULL,
#endif
#ifdef CONFIG_SECCOMP_FILTER
	/* 初始任务没有 seccomp filter 引用。 */
	.seccomp	= { .filter_count = ATOMIC_INIT(0) },
#endif
#ifdef CONFIG_SCHED_MM_CID
	/* 没有 mm concurrency ID，使用 MM_CID_UNSET 哨兵。 */
	.mm_cid		= { .cid = MM_CID_UNSET, },
#endif
};
EXPORT_SYMBOL(init_task);
/* 导出的是永久静态对象地址；使用者仍必须遵守 task 字段各自的同步协议。 */

/*
 * Initial thread structure. Alignment of this is handled by a special
 * linker map entry.
 */
/* 初始 thread_info；其对齐由链接脚本专用条目保证，而不是本声明的普通属性。 */
#ifndef CONFIG_THREAD_INFO_IN_TASK
/*
 * thread_info 未内嵌 task_struct 时，单独静态建立并指回 init_task。对象位于
 * __init_thread_info 约定 section，架构通过 INIT_THREAD_INFO 填充 flags/CPU
 * 等初态；无动态 ownership，生命周期与初始线程栈绑定。
 */
struct thread_info init_thread_info __init_thread_info = INIT_THREAD_INFO(init_task);
#endif
