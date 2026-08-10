/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_H
#define _LINUX_SCHED_H

/*
 * Define 'struct task_struct' and provide the main scheduler
 * APIs (schedule(), wakeup variants, etc.)
 */
/*
 * 本头文件是任务与调度器的核心公共契约：定义 task_struct、任务状态/标志以及调度、
 * 唤醒、亲和性和迁移 API。字段跨 fork/exec/exit、信号、内存、cgroup 和各调度类共享，
 * 阅读时必须同时跟踪字段 ownership、保护锁、发布时点及 CONFIG_* 下的布局变化。
 */

#include <uapi/linux/sched.h>

#include <asm/current.h>
#include <asm/processor.h>
#include <linux/thread_info.h>
#include <linux/preempt.h>
#include <linux/cpumask_types.h>

#include <linux/cache.h>
#include <linux/futex_types.h>
#include <linux/irqflags_types.h>
#include <linux/smp_types.h>
#include <linux/pid_types.h>
#include <linux/sem_types.h>
#include <linux/shm.h>
#include <linux/kmsan_types.h>
#include <linux/mutex_types.h>
#include <linux/plist_types.h>
#include <linux/hrtimer_types.h>
#include <linux/timer_types.h>
#include <linux/seccomp_types.h>
#include <linux/nodemask_types.h>
#include <linux/refcount_types.h>
#include <linux/resource.h>
#include <linux/latencytop.h>
#include <linux/sched/prio.h>
#include <linux/sched/types.h>
#include <linux/signal_types.h>
#include <linux/spinlock.h>
#include <linux/syscall_user_dispatch_types.h>
#include <linux/mm_types_task.h>
#include <linux/netdevice_xmit.h>
#include <linux/task_io_accounting.h>
#include <linux/posix-timers_types.h>
#include <linux/restart_block.h>
#include <linux/rseq_types.h>
#include <linux/seqlock_types.h>
#include <linux/kcsan.h>
#include <linux/rv.h>
#include <linux/uidgid_types.h>
#include <linux/tracepoint-defs.h>
#include <linux/unwind_deferred_types.h>
#include <asm/kmap_size.h>
#include <linux/time64.h>
#ifndef COMPILE_OFFSETS
#include <generated/rq-offsets.h>
#endif

/* task_struct member predeclarations (sorted alphabetically): */
/* 以下仅声明 task_struct 指针成员的类型，避免为一个核心头文件递归包含所有子系统布局。 */
struct audit_context;
struct bio_list;
struct blk_plug;
struct bpf_local_storage;
struct bpf_run_ctx;
struct bpf_net_context;
struct capture_control;
struct cfs_rq;
struct fs_struct;
struct io_context;
struct io_uring_task;
struct mempolicy;
struct nameidata;
struct nsproxy;
struct perf_event_context;
struct perf_ctx_data;
struct pid_namespace;
struct pipe_inode_info;
struct rcu_node;
struct reclaim_state;
struct root_domain;
struct rq;
struct sched_attr;
struct sched_dl_entity;
struct seq_file;
struct sighand_struct;
struct signal_struct;
struct task_delay_info;
struct task_exec_state;
struct task_group;
struct task_struct;
struct timespec64;
struct user_event_mm;

#include <linux/sched/ext.h>

/*
 * Task state bitmask. NOTE! These bits are also
 * encoded in fs/proc/array.c: get_task_state().
 *
 * We have two separate sets of flags: task->__state
 * is about runnability, while task->exit_state are
 * about the task exiting. Confusing, but this way
 * modifying one set can't modify the other one by
 * mistake.
 */
/*
 * task->__state 描述“当前能否被调度/由何种事件唤醒”，exit_state 描述退出阶段；两组位
 * 刻意存于不同字段，虽共享下面的数值命名空间，却绝不能对同一字段混用。proc 状态映射
 * 还复制了这些位，新增/调整状态必须同步 fs/proc/array.c。状态写入须使用下方配对宏。
 */

/* Used in tsk->__state: */
/* 正在运行或处于 runqueue 可运行状态；特殊之处是编码为 0。 */
#define TASK_RUNNING			0x00000000
/* 可被显式 wakeup 或未屏蔽信号唤醒的普通可中断睡眠。 */
#define TASK_INTERRUPTIBLE		0x00000001
/* 只响应显式条件唤醒的不可中断睡眠，通常计入负载。 */
#define TASK_UNINTERRUPTIBLE		0x00000002
/* job-control 停止与 ptrace 停止的内部特殊状态位。 */
#define __TASK_STOPPED			0x00000004
#define __TASK_TRACED			0x00000008
/* Used in tsk->exit_state: */
/* 已由父进程回收和等待父回收的两个退出阶段；EXIT_TRACE 是查询掩码。 */
#define EXIT_DEAD			0x00000010
#define EXIT_ZOMBIE			0x00000020
#define EXIT_TRACE			(EXIT_ZOMBIE | EXIT_DEAD)
/* Used in tsk->__state again: */
/* kthread park 握手、最终死亡、唤醒过渡、负载排除、新建、RT 锁等待和 freezer 状态。 */
#define TASK_PARKED			0x00000040
#define TASK_DEAD			0x00000080
#define TASK_WAKEKILL			0x00000100
#define TASK_WAKING			0x00000200
#define TASK_NOLOAD			0x00000400
#define TASK_NEW			0x00000800
#define TASK_RTLOCK_WAIT		0x00001000
#define TASK_FREEZABLE			0x00002000
#define __TASK_FREEZABLE_UNSAFE	       (0x00004000 * IS_ENABLED(CONFIG_LOCKDEP))
#define TASK_FROZEN			0x00008000
#define TASK_STATE_MAX			0x00010000

/* 覆盖全部合法非零 __state 位，用于“不限定睡眠状态”的匹配。 */
#define TASK_ANY			(TASK_STATE_MAX-1)

/*
 * DO NOT ADD ANY NEW USERS !
 */
/* 该不安全 freezer 组合只为兼容旧用户；新代码不得扩大无法由 lockdep 正确验证的协议。 */
#define TASK_FREEZABLE_UNSAFE		(TASK_FREEZABLE | __TASK_FREEZABLE_UNSAFE)

/* Convenience macros for the sake of set_current_state: */
/* 复合状态是传给状态写/唤醒 API 的匹配掩码，不是新的独立存储位。 */
#define TASK_KILLABLE			(TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)
#define TASK_STOPPED			(TASK_WAKEKILL | __TASK_STOPPED)
#define TASK_TRACED			__TASK_TRACED

#define TASK_IDLE			(TASK_UNINTERRUPTIBLE | TASK_NOLOAD)

/* Convenience macros for the sake of wake_up(): */
#define TASK_NORMAL			(TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)

/* get_task_state(): */
/* proc 对外报告的状态集合；TASK_IDLE 等派生状态由索引 helper 另行折叠。 */
#define TASK_REPORT			(TASK_RUNNING | TASK_INTERRUPTIBLE | \
					 TASK_UNINTERRUPTIBLE | __TASK_STOPPED | \
					 __TASK_TRACED | EXIT_DEAD | EXIT_ZOMBIE | \
					 TASK_PARKED)

/* task_is_running() - 无锁读取 @task->__state 快照；true 不保证目标仍在当前 CPU。 */
#define task_is_running(task)		(READ_ONCE((task)->__state) == TASK_RUNNING)

/* jobctl 状态由信号/ptrace 锁协议保护；这些宏只提供 READ_ONCE 快照。 */
#define task_is_traced(task)		((READ_ONCE(task->jobctl) & JOBCTL_TRACED) != 0)
#define task_is_stopped(task)		((READ_ONCE(task->jobctl) & JOBCTL_STOPPED) != 0)
#define task_is_stopped_or_traced(task)	((READ_ONCE(task->jobctl) & (JOBCTL_STOPPED | JOBCTL_TRACED)) != 0)

/*
 * Special states are those that do not use the normal wait-loop pattern. See
 * the comment with set_special_state().
 */
/* 特殊状态不能依赖普通“写状态→测条件→schedule”循环，必须与 wakeup 严格串行。 */
#define is_special_task_state(state)					\
	((state) & (__TASK_STOPPED | __TASK_TRACED | TASK_PARKED |	\
		    TASK_DEAD | TASK_WAKING | TASK_FROZEN))

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
/* 调试宏记录最近状态改变地址，并验证普通/特殊 API 没有混用；关闭配置后为空操作。 */
# define debug_normal_state_change(state_value)				\
	do {								\
		WARN_ON_ONCE(is_special_task_state(state_value));	\
		current->task_state_change = _THIS_IP_;			\
	} while (0)

# define debug_special_state_change(state_value)			\
	do {								\
		WARN_ON_ONCE(!is_special_task_state(state_value));	\
		current->task_state_change = _THIS_IP_;			\
	} while (0)

# define debug_rtlock_wait_set_state()					\
	do {								 \
		current->saved_state_change = current->task_state_change;\
		current->task_state_change = _THIS_IP_;			 \
	} while (0)

# define debug_rtlock_wait_restore_state()				\
	do {								 \
		current->task_state_change = current->saved_state_change;\
	} while (0)

#else
# define debug_normal_state_change(cond)	do { } while (0)
# define debug_special_state_change(cond)	do { } while (0)
# define debug_rtlock_wait_set_state()		do { } while (0)
# define debug_rtlock_wait_restore_state()	do { } while (0)
#endif

#define trace_set_current_state(state_value)                     \
	do {                                                     \
		if (tracepoint_enabled(sched_set_state_tp))      \
			__trace_set_current_state(state_value); \
	} while (0)

/*
 * set_current_state() includes a barrier so that the write of current->__state
 * is correctly serialised wrt the caller's subsequent test of whether to
 * actually sleep:
 *
 *   for (;;) {
 *	set_current_state(TASK_UNINTERRUPTIBLE);
 *	if (CONDITION)
 *	   break;
 *
 *	schedule();
 *   }
 *   __set_current_state(TASK_RUNNING);
 *
 * If the caller does not need such serialisation (because, for instance, the
 * CONDITION test and condition change and wakeup are under the same lock) then
 * use __set_current_state().
 *
 * The above is typically ordered against the wakeup, which does:
 *
 *   CONDITION = 1;
 *   wake_up_state(p, TASK_UNINTERRUPTIBLE);
 *
 * where wake_up_state()/try_to_wake_up() executes a full memory barrier before
 * accessing p->__state.
 *
 * Wakeup will do: if (@state & p->__state) p->__state = TASK_RUNNING, that is,
 * once it observes the TASK_UNINTERRUPTIBLE store the waking CPU can issue a
 * TASK_RUNNING store which can collide with __set_current_state(TASK_RUNNING).
 *
 * However, with slightly different timing the wakeup TASK_RUNNING store can
 * also collide with the TASK_UNINTERRUPTIBLE store. Losing that store is not
 * a problem either because that will result in one extra go around the loop
 * and our @cond test will save the day.
 *
 * Also see the comments of try_to_wake_up().
 */
/*
 * 普通等待循环必须先以 set_current_state() 的全屏障发布睡眠状态，再检查条件；唤醒侧
 * 先发布条件再由 try_to_wake_up() 的屏障读取状态，二者保证不会同时“看不到对方”。
 * 若条件、状态与 wakeup 已由同一把锁串行，才可用无额外屏障的 __set_current_state()。
 * wakeup 与等待者都写 TASK_RUNNING 的碰撞是允许的；丢失睡眠状态最多多循环一次。
 */
/* __set_current_state() - 无额外内存屏障写 current 普通状态；仅已有外部串行时使用。 */
#define __set_current_state(state_value)				\
	do {								\
		debug_normal_state_change((state_value));		\
		trace_set_current_state(state_value);			\
		WRITE_ONCE(current->__state, (state_value));		\
	} while (0)

/* set_current_state() - 以 smp_store_mb 发布状态，供标准无锁条件等待循环使用。 */
#define set_current_state(state_value)					\
	do {								\
		debug_normal_state_change((state_value));		\
		trace_set_current_state(state_value);			\
		smp_store_mb(current->__state, (state_value));		\
	} while (0)

/*
 * set_special_state() should be used for those states when the blocking task
 * can not use the regular condition based wait-loop. In that case we must
 * serialize against wakeups such that any possible in-flight TASK_RUNNING
 * stores will not collide with our state change.
 */
/*
 * set_special_state() - 在 current->pi_lock 和 IRQ-off 下写特殊状态。
 * @state_value 必须通过 is_special_task_state；锁与 try_to_wake_up() 串行，避免在途
 * TASK_RUNNING 覆盖 park/stop/frozen 等一次性状态。宏恢复原 IRQ 状态，无返回值。
 */
#define set_special_state(state_value)					\
	do {								\
		unsigned long flags; /* may shadow */			\
									\
		raw_spin_lock_irqsave(&current->pi_lock, flags);	\
		debug_special_state_change((state_value));		\
		trace_set_current_state(state_value);			\
		WRITE_ONCE(current->__state, (state_value));		\
		raw_spin_unlock_irqrestore(&current->pi_lock, flags);	\
	} while (0)

/*
 * PREEMPT_RT specific variants for "sleeping" spin/rwlocks
 *
 * RT's spin/rwlock substitutions are state preserving. The state of the
 * task when blocking on the lock is saved in task_struct::saved_state and
 * restored after the lock has been acquired.  These operations are
 * serialized by task_struct::pi_lock against try_to_wake_up(). Any non RT
 * lock related wakeups while the task is blocked on the lock are
 * redirected to operate on task_struct::saved_state to ensure that these
 * are not dropped. On restore task_struct::saved_state is set to
 * TASK_RUNNING so any wakeup attempt redirected to saved_state will fail.
 *
 * The lock operation looks like this:
 *
 *	current_save_and_set_rtlock_wait_state();
 *	for (;;) {
 *		if (try_lock())
 *			break;
 *		raw_spin_unlock_irq(&lock->wait_lock);
 *		schedule_rtlock();
 *		raw_spin_lock_irq(&lock->wait_lock);
 *		set_current_state(TASK_RTLOCK_WAIT);
 *	}
 *	current_restore_rtlock_saved_state();
 */
/*
 * PREEMPT_RT 把可睡眠 spin/rwlock 的临时 TASK_RTLOCK_WAIT 与调用者原状态分离：pi_lock
 * 保护 saved_state，并把非锁唤醒重定向到保存槽。获取锁后恢复原状态，再把 saved_state
 * 置 RUNNING，使迟到的重定向唤醒安全失败；两个宏都要求 IRQ 已关闭。
 */
#define current_save_and_set_rtlock_wait_state()			\
	do {								\
		lockdep_assert_irqs_disabled();				\
		raw_spin_lock(&current->pi_lock);			\
		current->saved_state = current->__state;		\
		debug_rtlock_wait_set_state();				\
		trace_set_current_state(TASK_RTLOCK_WAIT);		\
		WRITE_ONCE(current->__state, TASK_RTLOCK_WAIT);		\
		raw_spin_unlock(&current->pi_lock);			\
	} while (0);

#define current_restore_rtlock_saved_state()				\
	do {								\
		lockdep_assert_irqs_disabled();				\
		raw_spin_lock(&current->pi_lock);			\
		debug_rtlock_wait_restore_state();			\
		trace_set_current_state(current->saved_state);		\
		WRITE_ONCE(current->__state, current->saved_state);	\
		current->saved_state = TASK_RUNNING;			\
		raw_spin_unlock(&current->pi_lock);			\
	} while (0);

/* get_current_state() - 返回 current->__state 的无锁快照，不建立额外顺序。 */
#define get_current_state()	READ_ONCE(current->__state)

/*
 * Define the task command name length as enum, then it can be visible to
 * BPF programs.
 */
/* comm 固定为 16 字节（含 NUL）；使用 enum 让 BTF/BPF 能取得该常量。 */
enum {
	TASK_COMM_LEN = 16,
};

/* sched_tick() - 每个调度 tick 更新 current 和 runqueue 记账，运行于 tick IRQ 上下文。 */
extern void sched_tick(void);

/* 无限 schedule_timeout 的哨兵；返回时仍以 LONG_MAX 表示未递减。 */
#define	MAX_SCHEDULE_TIMEOUT		LONG_MAX

/*
 * schedule_timeout() - 按 current 已设置的状态最多睡眠 @timeout 个 jiffies。
 * 可调度上下文调用；返回剩余 jiffies，0 表示到期。各包装先选择 interruptible、killable、
 * uninterruptible 或 idle 状态；调用者必须在返回后按条件/信号协议继续判断。
 */
extern long schedule_timeout(long timeout);
/* 可被普通信号唤醒的 timeout 包装；返回剩余 jiffies。 */
extern long schedule_timeout_interruptible(long timeout);
/* 只被致命信号唤醒的 timeout 包装；返回剩余 jiffies。 */
extern long schedule_timeout_killable(long timeout);
/* 不响应信号且计入负载的 timeout 包装。 */
extern long schedule_timeout_uninterruptible(long timeout);
/* TASK_IDLE timeout 包装：不可中断且不计入负载。 */
extern long schedule_timeout_idle(long timeout);
/* schedule() - 让 current 离开 CPU 并运行调度器；返回时 current 再次被选中。 */
asmlinkage void schedule(void);
/* 抢占已关闭调用点的显式调度包装；返回时保持调用者原抢占语义。 */
extern void schedule_preempt_disabled(void);
/* IRQ 退出路径的抢占调度入口；仅架构中断尾部按约束调用。 */
asmlinkage void preempt_schedule_irq(void);
#ifdef CONFIG_PREEMPT_RT
 /* PREEMPT_RT 可睡眠锁等待的专用调度入口。 */
 extern void schedule_rtlock(void);
#endif

/* I/O 调度记账两阶段协议：prepare 返回 token，finish 必须用同一 token 配对。 */
extern int __must_check io_schedule_prepare(void);
extern void io_schedule_finish(int token);
/* io_schedule_timeout() - 以 I/O wait 记账睡眠最多 @timeout jiffies，返回剩余值。 */
extern long io_schedule_timeout(long timeout);
/* io_schedule() - 无 timeout 的 I/O wait 调度点，返回时 current 已恢复运行。 */
extern void io_schedule(void);

/* wrapper functions to trace from this header file */
/* 头文件状态/need_resched 宏使用的 tracepoint 声明和慢路径；关闭 trace 时近似零开销。 */
DECLARE_TRACEPOINT(sched_set_state_tp);
extern void __trace_set_current_state(int state_value);
DECLARE_TRACEPOINT(sched_set_need_resched_tp);
extern void __trace_set_need_resched(struct task_struct *curr, int tif);

/**
 * struct prev_cputime - snapshot of system and user cputime
 * @utime: time spent in user mode
 * @stime: time spent in system mode
 * @lock: protects the above two fields
 *
 * Stores previous user/system time values such that we can guarantee
 * monotonicity.
 */
/*
 * prev_cputime 保存上次对外发布的 user/system CPU 时间，raw lock 串行并发读写以保证
 * 观测值单调。原生虚拟 CPU 记账配置无需该缓存，因此结构体按配置为空，不拥有外部资源。
 */
struct prev_cputime {
#ifndef CONFIG_VIRT_CPU_ACCOUNTING_NATIVE
	/* 纳秒级累计用户/内核时间；lock 同时保护两字段的一致快照。 */
	u64				utime;
	u64				stime;
	raw_spinlock_t			lock;
#endif
};

/* vtime_state 标记当前虚拟时间应记入 idle/system/user/guest，inactive 表示不用该机制。 */
enum vtime_state {
	/* Task is sleeping or running in a CPU with VTIME inactive: */
	/* 任务睡眠或所在 CPU 未启用 vtime。 */
	VTIME_INACTIVE = 0,
	/* Task is idle */
	/* 当前区间计入 idle。 */
	VTIME_IDLE,
	/* Task runs in kernelspace in a CPU with VTIME active: */
	/* 当前区间计入内核态。 */
	VTIME_SYS,
	/* Task runs in userspace in a CPU with VTIME active: */
	/* 当前区间计入用户态。 */
	VTIME_USER,
	/* Task runs as guests in a CPU with VTIME active: */
	/* 当前区间计入虚拟机 guest。 */
	VTIME_GUEST,
};

/* vtime 是每 task 的虚拟时间状态机；seqcount 让读者取得字段一致快照。 */
struct vtime {
	/* 写侧更新序列；无锁读者重试以避开跨状态切换。 */
	seqcount_t		seqcount;
	/* 当前记账区间起始时间、状态及发生 CPU。 */
	unsigned long long	starttime;
	enum vtime_state	state;
	unsigned int		cpu;
	u64			utime;
	u64			stime;
	u64			gtime;
};

/*
 * Utilization clamp constraints.
 * @UCLAMP_MIN:	Minimum utilization
 * @UCLAMP_MAX:	Maximum utilization
 * @UCLAMP_CNT:	Utilization clamp constraints count
 */
/* 利用率夹取有最小/最大两个维度；CNT 只作数组界限，取值按容量标度。 */
enum uclamp_id {
	UCLAMP_MIN = 0,
	UCLAMP_MAX,
	UCLAMP_CNT
};

/* 默认 root_domain 及调度域重建全局 mutex；lock/unlock 包装形成唯一配置串行点。 */
extern struct root_domain def_root_domain;
extern struct mutex sched_domains_mutex;
extern void sched_domains_mutex_lock(void);
extern void sched_domains_mutex_unlock(void);

/* 传统调度参数载体；sched_priority 对普通类为 0，对 RT 类为有效优先级。 */
struct sched_param {
	int sched_priority;
};

/* sched_info 是可选的每 task 运行/排队延迟统计，不参与调度决策。 */
struct sched_info {
#ifdef CONFIG_SCHED_INFO
	/* Cumulative counters: */
	/* 以下为累计计数与时间；只有 CONFIG_SCHED_INFO 布局存在。 */

	/* # of times we have run on this CPU: */
	/* 被调度上 CPU 的累计次数。 */
	unsigned long			pcount;

	/* Time spent waiting on a runqueue: */
	/* runqueue 等待总时长。 */
	unsigned long long		run_delay;

	/* Max time spent waiting on a runqueue: */
	/* 单次最大等待及其发生时间。 */
	unsigned long long		max_run_delay;

	/* Min time spent waiting on a runqueue: */
	/* 单次最小等待。 */
	unsigned long long		min_run_delay;

	/* Timestamps: */
	/* 最近执行、最近入队及最大等待对应的时间戳。 */

	/* When did we last run on a CPU? */
	/* last_arrival：最近一次真正开始在 CPU 运行的时间戳。 */
	unsigned long long		last_arrival;

	/* When were we last queued to run? */
	/* last_queued：最近一次进入 runqueue 的时间戳。 */
	unsigned long long		last_queued;

	/* Timestamp of max time spent waiting on a runqueue: */
	/* max_run_delay_ts：出现最大 runqueue 等待时对应的时间戳。 */
	struct timespec64		max_run_delay_ts;

#endif /* CONFIG_SCHED_INFO */
};

/*
 * Integer metrics need fixed point arithmetic, e.g., sched/fair
 * has a few: load, load_avg, util_avg, freq, and capacity.
 *
 * We define a basic fixed point arithmetic range, and then formalize
 * all these metrics based on that basic range.
 */
/* 调度器用 10 位小数的定点标度表示 load/util/capacity，避免热路径浮点运算。 */
# define SCHED_FIXEDPOINT_SHIFT		10
# define SCHED_FIXEDPOINT_SCALE		(1L << SCHED_FIXEDPOINT_SHIFT)

/* Increase resolution of cpu_capacity calculations */
/* CPU capacity 复用同一 1024 标度，使 util 与 capacity 可直接比较。 */
# define SCHED_CAPACITY_SHIFT		SCHED_FIXEDPOINT_SHIFT
# define SCHED_CAPACITY_SCALE		(1L << SCHED_CAPACITY_SHIFT)

/* load_weight 保存调度权重及其预计算倒数；倒数避免公平调度热路径重复除法。 */
struct load_weight {
	unsigned long			weight;
	u32				inv_weight;
};

/*
 * The load/runnable/util_avg accumulates an infinite geometric series
 * (see __update_load_avg_cfs_rq() in kernel/sched/pelt.c).
 *
 * [load_avg definition]
 *
 *   load_avg = runnable% * scale_load_down(load)
 *
 * [runnable_avg definition]
 *
 *   runnable_avg = runnable% * SCHED_CAPACITY_SCALE
 *
 * [util_avg definition]
 *
 *   util_avg = running% * SCHED_CAPACITY_SCALE
 *
 * where runnable% is the time ratio that a sched_entity is runnable and
 * running% the time ratio that a sched_entity is running.
 *
 * For cfs_rq, they are the aggregated values of all runnable and blocked
 * sched_entities.
 *
 * The load/runnable/util_avg doesn't directly factor frequency scaling and CPU
 * capacity scaling. The scaling is done through the rq_clock_pelt that is used
 * for computing those signals (see update_rq_clock_pelt())
 *
 * N.B., the above ratios (runnable% and running%) themselves are in the
 * range of [0, 1]. To do fixed point arithmetics, we therefore scale them
 * to as large a range as necessary. This is for example reflected by
 * util_avg's SCHED_CAPACITY_SCALE.
 *
 * [Overflow issue]
 *
 * The 64-bit load_sum can have 4353082796 (=2^64/47742/88761) entities
 * with the highest load (=88761), always runnable on a single cfs_rq,
 * and should not overflow as the number already hits PID_MAX_LIMIT.
 *
 * For all other cases (including 32-bit kernels), struct load_weight's
 * weight will overflow first before we do, because:
 *
 *    Max(load_avg) <= Max(load.weight)
 *
 * Then it is the load_weight's responsibility to consider overflow
 * issues.
 */
/*
 * PELT 以指数衰减累计 entity/cfs_rq 的 load、runnable 和 running 比例；sum 是历史积分，
 * avg 是按 1024 容量标度归一后的决策输入。rq_clock_pelt 已吸收频率/CPU capacity 缩放，
 * 因而字段本身不再直接乘这些系数。64 位 load_sum 的上界足以覆盖 PID 极限。
 */
struct sched_avg {
	/* 上次衰减更新时间，以及三个可运行/执行信号的积分和当前周期余量。 */
	u64				last_update_time;
	u64				load_sum;
	u64				runnable_sum;
	u32				util_sum;
	u32				period_contrib;
	unsigned long			load_avg;
	unsigned long			runnable_avg;
	unsigned long			util_avg;
	unsigned int			util_est;
} ____cacheline_aligned;

/*
 * The UTIL_AVG_UNCHANGED flag is used to synchronize util_est with util_avg
 * updates. When a task is dequeued, its util_est should not be updated if its
 * util_avg has not been updated in the meantime.
 * This information is mapped into the MSB bit of util_est at dequeue time.
 * Since max value of util_est for a task is 1024 (PELT util_avg for a task)
 * it is safe to use MSB.
 */
/* util_est 低位采用 1/4 权重更新；最高位标记 dequeue 期间 util_avg 未变化。 */
#define UTIL_EST_WEIGHT_SHIFT		2
#define UTIL_AVG_UNCHANGED		0x80000000

/* sched_statistics 是可选诊断统计：等待、睡眠、阻塞、执行、迁移和唤醒路径的累计数据。 */
struct sched_statistics {
#ifdef CONFIG_SCHEDSTATS
	/* wait_*：runqueue 等待开始、极值、次数和总量。 */
	u64				wait_start;
	u64				wait_max;
	u64				wait_count;
	u64				wait_sum;
	u64				iowait_count;
	u64				iowait_sum;

	/* sleep_* 和 block_* 分别统计可中断睡眠与不可中断阻塞。 */
	u64				sleep_start;
	u64				sleep_max;
	s64				sum_sleep_runtime;

	u64				block_start;
	u64				block_max;
	s64				sum_block_runtime;

	/* 单次执行和单个时间片的历史最大值。 */
	s64				exec_max;
	u64				slice_max;

	u64				nr_migrations_cold;
	u64				nr_failed_migrations_affine;
	u64				nr_failed_migrations_running;
	u64				nr_failed_migrations_hot;
	u64				nr_forced_migrations;

	/* 唤醒总数以及同步、迁移、远近端、亲和性和 idle 等路径分类。 */
	u64				nr_wakeups;
	u64				nr_wakeups_sync;
	u64				nr_wakeups_migrate;
	u64				nr_wakeups_local;
	u64				nr_wakeups_remote;
	u64				nr_wakeups_affine;
	u64				nr_wakeups_affine_attempts;
	u64				nr_wakeups_passive;
	u64				nr_wakeups_idle;

#ifdef CONFIG_SCHED_CORE
	u64				core_forceidle_sum;
#endif
#endif /* CONFIG_SCHEDSTATS */
} ____cacheline_aligned;

/* sched_entity 是 CFS/EEVDF 的可调度实体；task 或 task_group 都可嵌入并进入 cfs_rq。 */
struct sched_entity {
	/* For load-balancing: */
	/* 负载权重、时间线红黑树节点以及 EEVDF deadline/vruntime/slice 边界。 */
	struct load_weight		load;
	struct rb_node			run_node;
	u64				deadline;
	u64				min_vruntime;
	u64				min_slice;
	u64				max_slice;

	struct list_head		group_node;
	/* 入队/延迟出队/相对期限/自定义 slice 的紧凑状态。 */
	unsigned char			on_rq;
	unsigned char			sched_delayed;
	unsigned char			rel_deadline;
	unsigned char			custom_slice;
					/* hole */

	/* 本轮起点、累计执行、上次快照、虚拟时间及获得的 slice。 */
	u64				exec_start;
	u64				sum_exec_runtime;
	u64				prev_sum_exec_runtime;
	u64				vruntime;
	/* Approximated virtual lag: */
	/* vlag 近似实体相对公平基准的虚拟滞后。 */
	s64				vlag;
	/* 'Protected' deadline, to give out minimum quantums: */
	/* vprot 防止最小运行量尚未满足时过早丢失 deadline 保护。 */
	u64				vprot;
	u64				slice;

	u64				nr_migrations;

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* 组调度层级、父实体、所在 cfs_rq 和本实体拥有的子 cfs_rq。 */
	int				depth;
	struct sched_entity		*parent;
	/* rq on which this entity is (to be) queued: */
	/* cfs_rq 是入队位置，my_q 是组实体代表的下一级队列。 */
	struct cfs_rq			*cfs_rq;
	/* rq "owned" by this entity/group: */
	/* 由当前组实体拥有、承载其子实体的 cfs_rq。 */
	struct cfs_rq			*my_q;
	/* cached value of my_q->h_nr_running */
	/* runnable_weight 缓存子队列层次可运行权重，避免重复遍历。 */
	unsigned long			runnable_weight;
#endif

	/*
	 * Per entity load average tracking.
	 *
	 * Put into separate cache line so it does not
	 * collide with read-mostly values above.
	 */
	/* PELT 写热点独占 cacheline，降低与上方调度决策只读字段的伪共享。 */
	struct sched_avg		avg;
};

/* sched_rt_entity 是 FIFO/RR task 或 RT group 的队列节点和时间片/层级状态。 */
struct sched_rt_entity {
	/* run_list 连接 RT 队列；timeout/watchdog/time_slice 约束连续运行。 */
	struct list_head		run_list;
	unsigned long			timeout;
	unsigned long			watchdog_stamp;
	unsigned int			time_slice;
	unsigned short			on_rq;
	unsigned short			on_list;

	/* back 支持层级遍历返回；组调度字段连接父实体和上下级 RT 队列。 */
	struct sched_rt_entity		*back;
#ifdef CONFIG_RT_GROUP_SCHED
	struct sched_rt_entity		*parent;
	/* rq on which this entity is (to be) queued: */
	/* 当前 RT 实体入队或即将入队的 rt_rq。 */
	struct rt_rq			*rt_rq;
	/* rq "owned" by this entity/group: */
	/* 由当前 RT 组实体拥有、承载子实体的 rt_rq。 */
	struct rt_rq			*my_q;
#endif
} __randomize_layout;

/* rq_flags 前向声明供 DL server 选取回调传递 runqueue 锁状态。 */
struct rq_flags;
/* server 回调借用 DL entity/rq_flags，返回选中的 task 借用或 NULL。 */
typedef struct task_struct *(*dl_server_pick_f)(struct sched_dl_entity *, struct rq_flags *rf);

/* sched_dl_entity 保存 SCHED_DEADLINE/CBS 的配置、运行态、timer 和 server 状态。 */
struct sched_dl_entity {
	/* 按绝对 deadline 排序的红黑树节点。 */
	struct rb_node			rb_node;

	/*
	 * Original scheduling parameters. Copied here from sched_attr
	 * during sched_setattr(), they will remain the same until
	 * the next sched_setattr().
	 */
	/* sched_setattr 发布的原始 runtime/deadline/period 及两个预计算带宽比例。 */
	u64				dl_runtime;	/* Maximum runtime for each instance	*/
	/* dl_runtime：每个 job 可消耗的最大运行时间。 */
	u64				dl_deadline;	/* Relative deadline of each instance	*/
	/* dl_deadline：每个 job 的相对期限。 */
	u64				dl_period;	/* Separation of two instances (period) */
	/* dl_period：相邻两个 job 的最小释放间隔。 */
	u64				dl_bw;		/* dl_runtime / dl_period		*/
	/* dl_bw：按 period 计算的预留带宽。 */
	u64				dl_density;	/* dl_runtime / dl_deadline		*/
	/* dl_density：按 deadline 计算的密度约束。 */

	/*
	 * Actual scheduling parameters. Initialized with the values above,
	 * they are continuously updated during task execution. Note that
	 * the remaining runtime could be < 0 in case we are in overrun.
	 */
	/* 每个 job 动态递减的剩余 runtime、绝对 deadline 和行为 flags；overrun 时 runtime 可负。 */
	s64				runtime;	/* Remaining runtime for this instance	*/
	/* runtime：当前 job 剩余预算，overrun 时允许为负。 */
	u64				deadline;	/* Absolute deadline for this instance	*/
	/* deadline：当前 job 的绝对期限。 */
	unsigned int			flags;		/* Specifying the scheduler behaviour	*/
	/* flags：来自 sched_attr 的 deadline 行为开关。 */

	/*
	 * Some bool flags:
	 *
	 * @dl_throttled tells if we exhausted the runtime. If so, the
	 * task has to wait for a replenishment to be performed at the
	 * next firing of dl_timer.
	 *
	 * @dl_yielded tells if task gave up the CPU before consuming
	 * all its available runtime during the last job.
	 *
	 * @dl_non_contending tells if the task is inactive while still
	 * contributing to the active utilization. In other words, it
	 * indicates if the inactive timer has been armed and its handler
	 * has not been executed yet. This flag is useful to avoid race
	 * conditions between the inactive timer handler and the wakeup
	 * code.
	 *
	 * @dl_overrun tells if the task asked to be informed about runtime
	 * overruns.
	 *
	 * @dl_server tells if this is a server entity.
	 *
	 * @dl_server_active tells if the dlserver is active(started).
	 * dlserver is started on first cfs enqueue on an idle runqueue
	 * and is stopped when a dequeue results in 0 cfs tasks on the
	 * runqueue. In other words, dlserver is active only when cpu's
	 * runqueue has atleast one cfs task.
	 *
	 * @dl_defer tells if this is a deferred or regular server. For
	 * now only defer server exists.
	 *
	 * @dl_defer_armed tells if the deferrable server is waiting
	 * for the replenishment timer to activate it.
	 *
	 * @dl_defer_running tells if the deferrable server is actually
	 * running, skipping the defer phase.
	 *
	 * @dl_defer_idle tracks idle state
	 *
	 * @dl_bw_attached tells if this server's bandwidth currently
	 * contributes to the root domain's total_bw. Only meaningful for server
	 * entities (@dl_server == 1). Allows toggling the reservation on/off
	 * without losing the configured @dl_runtime/@dl_period.
	 */
	/*
	 * 位域描述 CBS/GRUB 与 DL server 状态：throttled 等补充，yielded 主动让出；
	 * non_contending 在 0-lag timer 结束前仍计 active util，overrun 请求通知；server 及
	 * active/defer/armed/running/idle 控制公平类带宽服务器；bw_attached 表示 reservation
	 * 当前是否计入 root_domain total_bw。所有位随 entity 锁/runqueue 协议更新。
	 */
	unsigned int			dl_throttled      : 1;
	unsigned int			dl_yielded        : 1;
	unsigned int			dl_non_contending : 1;
	unsigned int			dl_overrun	  : 1;
	unsigned int			dl_server         : 1;
	unsigned int			dl_server_active  : 1;
	unsigned int			dl_defer	  : 1;
	unsigned int			dl_defer_armed	  : 1;
	unsigned int			dl_defer_running  : 1;
	unsigned int			dl_defer_idle     : 1;
	unsigned int			dl_bw_attached    : 1;

	/*
	 * Bandwidth enforcement timer. Each -deadline task has its
	 * own bandwidth to be enforced, thus we need one timer per task.
	 */
	/* dl_timer 在 runtime 耗尽后按补充时刻解除 throttled；每 task/entity 独立。 */
	struct hrtimer			dl_timer;

	/*
	 * Inactive timer, responsible for decreasing the active utilization
	 * at the "0-lag time". When a -deadline task blocks, it contributes
	 * to GRUB's active utilization until the "0-lag time", hence a
	 * timer is needed to decrease the active utilization at the correct
	 * time.
	 */
	/* inactive_timer 在 0-lag 时刻撤销阻塞 DL task 对 GRUB active utilization 的贡献。 */
	struct hrtimer			inactive_timer;

	/*
	 * Bits for DL-server functionality. Also see the comment near
	 * dl_server_update().
	 *
	 * @rq the runqueue this server is for
	 */
	/* server 专属：rq 为服务的 runqueue 借用，回调选择由该 server 承载的 task。 */
	struct rq			*rq;
	dl_server_pick_f		server_pick_task;

#ifdef CONFIG_RT_MUTEXES
	/*
	 * Priority Inheritance. When a DEADLINE scheduling entity is boosted
	 * pi_se points to the donor, otherwise points to the dl_se it belongs
	 * to (the original one/itself).
	 */
	/* PI boost 时 pi_se 借用 donor DL entity，否则指向自身原始 entity。 */
	struct sched_dl_entity *pi_se;
#endif
};

#ifdef CONFIG_UCLAMP_TASK
/* Number of utilization clamp buckets (shorter alias) */
/* runqueue 按配置数量离散化 clamp 值；别名供位宽和数组声明复用。 */
#define UCLAMP_BUCKETS CONFIG_UCLAMP_BUCKETS_COUNT

/*
 * Utilization clamp for a scheduling entity
 * @value:		clamp value "assigned" to a se
 * @bucket_id:		bucket index corresponding to the "assigned" value
 * @active:		the se is currently refcounted in a rq's bucket
 * @user_defined:	the requested clamp value comes from user-space
 *
 * The bucket_id is the index of the clamp bucket matching the clamp value
 * which is pre-computed and stored to avoid expensive integer divisions from
 * the fast path.
 *
 * The active bit is set whenever a task has got an "effective" value assigned,
 * which can be different from the clamp value "requested" from user-space.
 * This allows to know a task is refcounted in the rq's bucket corresponding
 * to the "effective" bucket_id.
 *
 * The user_defined bit is set whenever a task has got a task-specific clamp
 * value requested from userspace, i.e. the system defaults apply to this task
 * just as a restriction. This allows to relax default clamps when a less
 * restrictive task-specific value has been requested, thus allowing to
 * implement a "nice" semantic. For example, a task running with a 20%
 * default boost can still drop its own boosting to 0%.
 */
/*
 * uclamp_se 保存 task 的有效 clamp 值、预计算 bucket、是否已计入 rq bucket，以及是否
 * 来自用户请求。active 决定 refcount ownership；user_defined 允许 task 放宽系统默认值。
 */
struct uclamp_se {
	/* 位宽按容量标度和 bucket 数自动计算，保持 task_struct 紧凑。 */
	unsigned int value		: bits_per(SCHED_CAPACITY_SCALE);
	unsigned int bucket_id		: bits_per(UCLAMP_BUCKETS);
	unsigned int active		: 1;
	unsigned int user_defined	: 1;
};
#endif /* CONFIG_UCLAMP_TASK */

/* rcu_special 可按字节访问各延迟 RCU 动作，也可用 u32 原子快照整组状态。 */
union rcu_special {
	struct {
		/* blocked/need_qs/exp_hint/need_mb 分别表示阻塞、需静止点、加速提示和读侧屏障。 */
		u8			blocked;
		u8			need_qs;
		u8			exp_hint; /* Hint for performance. */
		/* exp_hint：供写侧选择更便宜发布路径的性能提示。 */
		u8			need_mb; /* Readers need smp_mb(). */
		/* need_mb：读侧必须执行 smp_mb() 才能消费这些位。 */
	} b; /* Bits. */
	/* b 是逐位视图。 */
	u32 s; /* Set of bits. */
	/* s 是同一状态的整体原子快照视图。 */
};

/* task 可拥有硬件和软件两类 perf context；invalid/CNT 供错误值和数组边界。 */
enum perf_event_task_context {
	perf_invalid_context = -1,
	perf_hw_context = 0,
	perf_sw_context,
	perf_nr_task_contexts,
};

/*
 * Number of contexts where an event can trigger:
 *      task, softirq, hardirq, nmi.
 */
/* perf 事件可从 task、softirq、hardirq、NMI 四种嵌套上下文触发。 */
#define PERF_NR_CONTEXTS	4

/* wake_q_node 是一次批量唤醒链节点；next 由 wake_q 锁外收集协议独占更新。 */
struct wake_q_node {
	struct wake_q_node *next;
};

/* kmap_ctrl 保存 task 的 kmap_local 嵌套深度和每层旧 PTE，调度切换时恢复映射。 */
struct kmap_ctrl {
#ifdef CONFIG_KMAP_LOCAL
	int				idx;
	pte_t				pteval[KM_MAX_IDX];
#endif
};

/*
 * task_struct - 一个线程从创建、运行、睡眠到退出/最终释放的核心对象。
 * usage 是对象引用计数；大量裸指针只借用 task 或子对象，必须由 tasklist/RCU/专用锁或
 * get_task_struct 稳定。字段按调度热路径、随机化区域和 CONFIG 子系统分组，不能仅凭
 * 相邻位置推断同锁保护；下方学习注释逐组标明角色，具体写侧仍以对应实现为准。
 */
struct task_struct {
#ifdef CONFIG_THREAD_INFO_IN_TASK
	/*
	 * For reasons of header soup (see current_thread_info()), this
	 * must be the first element of task_struct.
	 */
	/* thread_info 嵌入配置要求它位于偏移 0，汇编/current_thread_info 依赖该布局。 */
	struct thread_info		thread_info;
#endif
	/* 调度可运行状态；由状态宏/try_to_wake_up 配对访问，不能随意直接写。 */
	unsigned int			__state;

	/* saved state for "spinlock sleepers" */
	/* PREEMPT_RT 可睡眠锁临时覆盖 __state 时保存调用者原状态，由 pi_lock 保护。 */
	unsigned int			saved_state;

	/*
	 * This begins the randomizable portion of task_struct. Only
	 * scheduling-critical items should be added above here.
	 */
	/* 从这里起允许 randstruct 重排；只有汇编/调度极热且有固定要求的字段能放在上方。 */
	randomized_struct_fields_start

	/* 内核栈基址和 task_struct 引用计数；usage 归零后才进入最终释放。 */
	void				*stack;
	refcount_t			usage;
	/* Per task flags (PF_*), defined further below: */
	/* flags 是 PF_* 生命周期/身份位，ptrace 保存被跟踪及事件选择状态。 */
	unsigned int			flags;
	unsigned int			ptrace;

#ifdef CONFIG_MEM_ALLOC_PROFILING
	/* 当前 task 的分配归因标签借用。 */
	struct alloc_tag		*alloc_tag;
#endif

	/* on_cpu/on_rq 是调度器在 rq/pi 锁协议下维护的执行和排队状态。 */
	u8				on_cpu;
	u8				on_rq;
	u8				is_blocked;
	u8				__pad;

	/* 跨 CPU 唤醒队列节点，以及 wake-affine 关系的翻转计数、衰减时间和最近 wakee 借用。 */
	struct __call_single_node	wake_entry;
	unsigned int			wakee_flips;
	unsigned long			wakee_flip_decay_ts;
	struct task_struct		*last_wakee;

	/*
	 * recent_used_cpu is initially set as the last CPU used by a task
	 * that wakes affine another task. Waker/wakee relationships can
	 * push tasks around a CPU where each wakeup moves to the next one.
	 * Tracking a recently used CPU allows a quick search for a recently
	 * used CPU that may be idle.
	 */
	/* recent_used_cpu/wake_cpu 为唤醒放置提示，不是硬亲和性；并发变化只影响启发式质量。 */
	int				recent_used_cpu;
	int				wake_cpu;

	/* 有效、静态、normal 和 RT 优先级；调度策略更新在 rq/pi 锁下保持彼此一致。 */
	int				prio;
	int				static_prio;
	int				normal_prio;
	unsigned int			rt_priority;

	/* 各调度类内嵌实体；sched_class 决定当前实际使用哪一套，dl_server 为可空借用。 */
	struct sched_entity		se;
	struct sched_rt_entity		rt;
	struct sched_dl_entity		dl;
	struct sched_dl_entity		*dl_server;
#ifdef CONFIG_SCHED_CLASS_EXT
	/* sched_ext 调度类的每 task 扩展实体。 */
	struct sched_ext_entity		scx;
#endif
	const struct sched_class	*sched_class;

#ifdef CONFIG_SCHED_CORE
	/* core scheduling 红黑树节点、安全域 cookie 和同核占用计数。 */
	struct rb_node			core_node;
	unsigned long			core_cookie;
	unsigned int			core_occupation;
#endif

#ifdef CONFIG_CGROUP_SCHED
	/* task 所属调度组借用；带宽配置下 throttle callback 延后处理限流状态。 */
	struct task_group		*sched_task_group;
#ifdef CONFIG_CFS_BANDWIDTH
	struct callback_head		sched_throttle_work;
	struct list_head		throttle_node;
	bool				throttled;
#endif
#endif


#ifdef CONFIG_UCLAMP_TASK
	/*
	 * Clamp values requested for a scheduling entity.
	 * Must be updated with task_rq_lock() held.
	 */
	/* 用户请求的 min/max clamp；task_rq_lock 保证与 rq bucket 记账同步。 */
	struct uclamp_se		uclamp_req[UCLAMP_CNT];
	/*
	 * Effective clamp values used for a scheduling entity.
	 * Must be updated with task_rq_lock() held.
	 */
	/* 层级/系统约束后的有效 clamp；active 位决定当前 rq bucket 引用。 */
	struct uclamp_se		uclamp[UCLAMP_CNT];
#endif

	/* 可选调度诊断统计，随 task 生命周期存在。 */
	struct sched_statistics         stats;

#ifdef CONFIG_PREEMPT_NOTIFIERS
	/* List of struct preempt_notifier: */
	/* 虚拟化等用户注册的抢占通知链，由 notifier 生命周期协议保护。 */
	struct hlist_head		preempt_notifiers;
#endif

#ifdef CONFIG_BLK_DEV_IO_TRACE
	/* 块 I/O trace 的每 task 序列号。 */
	unsigned int			btrace_seq;
#endif

	/*
	 * policy 与 max capacity、允许 CPU 数/掩码构成调度策略和亲和性状态。cpus_ptr 指向
	 * 当前生效掩码（通常 cpus_mask 或用户副本），user_cpus_ptr 由 task 拥有并最终释放；
	 * migration_pending/disabled/flags 串行异步迁移与 migrate_disable 嵌套。
	 */
	unsigned int			policy;
	unsigned long			max_allowed_capacity;
	int				nr_cpus_allowed;
	const cpumask_t			*cpus_ptr;
	cpumask_t			*user_cpus_ptr;
	cpumask_t			cpus_mask;
	void				*migration_pending;
	unsigned short			migration_disabled;
	unsigned short			migration_flags;

#ifdef CONFIG_PREEMPT_RCU
	/* 可抢占 RCU 读侧嵌套、延迟动作、阻塞节点链和所属 rcu_node 借用。 */
	int				rcu_read_lock_nesting;
	union rcu_special		rcu_read_unlock_special;
	struct list_head		rcu_node_entry;
	struct rcu_node			*rcu_blocked_node;
#endif /* #ifdef CONFIG_PREEMPT_RCU */

#ifdef CONFIG_TASKS_RCU
	/* Tasks RCU 用上下文切换、holdout/exit CPU 和两条链跟踪任务静止/退出阶段。 */
	unsigned long			rcu_tasks_nvcsw;
	u8				rcu_tasks_holdout;
	u8				rcu_tasks_idx;
	int				rcu_tasks_idle_cpu;
	struct list_head		rcu_tasks_holdout_list;
	int				rcu_tasks_exit_cpu;
	struct list_head		rcu_tasks_exit_list;
#endif /* #ifdef CONFIG_TASKS_RCU */

#ifdef CONFIG_TASKS_TRACE_RCU
	/* Tasks Trace RCU 读侧嵌套与每 CPU SRCU 计数器借用。 */
	int				trc_reader_nesting;
	struct srcu_ctr __percpu	*trc_reader_scp;
#endif /* #ifdef CONFIG_TASKS_TRACE_RCU */

#ifdef CONFIG_TRIVIAL_PREEMPT_RCU
	/* 简化实现的抢占 RCU 嵌套计数。 */
	int				rcu_trivial_preempt_nesting;
#endif /* #ifdef CONFIG_TRIVIAL_PREEMPT_RCU */

	/* 每 task 调度信息，以及连接全局 tasks、RT pushable、DL pushable 集合的节点。 */
	struct sched_info		sched_info;

	struct list_head		tasks;
	struct plist_node		pushable_tasks;
	struct rb_node			pushable_dl_tasks;

	/*
	 * mm 是真实用户地址空间引用；内核线程通常为 NULL。active_mm 是当前硬件页表/lazy-TLB
	 * 借用，二者引用类别不同，切换和 kthread_use_mm 必须按 mmgrab/mmdrop 协议转换。
	 */
	struct mm_struct		*mm;
	struct mm_struct		*active_mm;

	/* exec_state 是 RCU 发布的可执行状态引用，exec/final free 路径负责替换与 put。 */
	struct task_exec_state __rcu	*exec_state;

	/* exit_state 阶段、退出码和通知父进程的信号；由退出/wait 锁协议发布。 */
	int				exit_state;
	int				exit_code;
	int				exit_signal;
	/* The signal sent when the parent dies: */
	/* 父进程死亡时内核向本 task 发送的信号编号。 */
	int				pdeath_signal;
	/* JOBCTL_*, siglock protected: */
	/* job-control/ptrace 位统一由 sighand->siglock 保护。 */
	unsigned long			jobctl;

	/* Used for emulating ABI behavior of previous Linux versions: */
	/* personality 选择旧 Linux ABI 兼容行为。 */
	unsigned int			personality;

	/* Scheduler bits, serialized by scheduler locks: */
	/* fork 重置、负载贡献、迁移和热任务提示，由调度器锁串行更新。 */
	unsigned			sched_reset_on_fork:1;
	unsigned			sched_contributes_to_load:1;
	unsigned			sched_migrated:1;
	unsigned			sched_task_hot:1;

	/* Force alignment to the next boundary: */
	/* 零宽位域强制后续 current-only 标志从新的存储单元开始。 */
	unsigned			:0;

	/* Unserialized, strictly 'current' */
	/* 以下多数位只允许 current 自写，因而不要求外部锁；跨 task 读取需专用协议。 */

	/*
	 * This field must not be in the scheduler word above due to wakelist
	 * queueing no longer being serialized by p->on_cpu. However:
	 *
	 * p->XXX = X;			ttwu()
	 * schedule()			  if (p->on_rq && ..) // false
	 *   smp_mb__after_spinlock();	  if (smp_load_acquire(&p->on_cpu) && //true
	 *   deactivate_task()		      ttwu_queue_wakelist())
	 *     p->on_rq = 0;			p->sched_remote_wakeup = Y;
	 *
	 * guarantees all stores of 'current' are visible before
	 * ->sched_remote_wakeup gets used, so it can be in this word.
	 */
	/* remote_wakeup 借 on_cpu acquire 与 schedule 的锁后屏障获得 current 先前写入可见性。 */
	unsigned			sched_remote_wakeup:1;
#ifdef CONFIG_RT_MUTEXES
	unsigned			sched_rt_mutex:1;
#endif

	/* Bit to tell TOMOYO we're in execve(): */
	/* exec、I/O wait、信号掩码恢复及各可选子系统的 current-only 递归/状态标志。 */
	unsigned			in_execve:1;
	unsigned			in_iowait:1;
#ifndef TIF_RESTORE_SIGMASK
	unsigned			restore_sigmask:1;
#endif
#ifdef CONFIG_MEMCG_V1
	unsigned			in_user_fault:1;
#endif
#ifdef CONFIG_LRU_GEN
	/* whether the LRU algorithm may apply to this access */
	/* LRU fault 路径是否允许本次访问参与代际更新。 */
	unsigned			in_lru_fault:1;
#endif
#ifdef CONFIG_COMPAT_BRK
	unsigned			brk_randomized:1;
#endif
#ifdef CONFIG_CGROUPS
	/* disallow userland-initiated cgroup migration */
	/* 禁止用户发起 cgroup 迁移。 */
	unsigned			no_cgroup_migration:1;
	/* task is frozen/stopped (used by the cgroup freezer) */
	/* cgroup freezer 已把 task 冻结/停止。 */
	unsigned			frozen:1;
#endif
#ifdef CONFIG_BLK_CGROUP
	unsigned			use_memdelay:1;
#endif
#ifdef CONFIG_PSI
	/* Stalled due to lack of memory */
	/* PSI 将 current 记为内存压力停顿。 */
	unsigned			in_memstall:1;
#endif
#ifdef CONFIG_PAGE_OWNER
	/* Used by page_owner=on to detect recursion in page tracking. */
	/* page_owner 记录递归保护。 */
	unsigned			in_page_owner:1;
#endif
#ifdef CONFIG_EVENTFD
	/* Recursion prevention for eventfd_signal() */
	/* eventfd signal 递归保护。 */
	unsigned			in_eventfd:1;
#endif
#ifdef CONFIG_ARCH_HAS_CPU_PASID
	unsigned			pasid_activated:1;
#endif
#ifdef CONFIG_X86_BUS_LOCK_DETECT
	unsigned			reported_split_lock:1;
#endif
#ifdef CONFIG_TASK_DELAY_ACCT
	/* delay due to memory thrashing */
	/* delayacct 将 current 标记为内存抖动等待。 */
	unsigned                        in_thrashing:1;
#endif
	unsigned			in_nf_duplicate:1;
#ifdef CONFIG_PREEMPT_RT
	struct netdev_xmit		net_xmit;
#endif
	/* 必须用原子 bitops 更新的 task flags，与上方 current-only 位分开。 */
	unsigned long			atomic_flags; /* Flags requiring atomic access. */
	/* atomic_flags：必须用 bitops 并发访问的 PFA_* 位集合。 */

	/* 被信号中断的可重启系统调用状态，由 syscall exit/信号路径消费。 */
	struct restart_block		restart_block;

	/* 线程 PID 与线程组 ID，在对应 PID namespace 语义下由 fork/exit 管理。 */
	pid_t				pid;
	pid_t				tgid;

#ifdef CONFIG_STACKPROTECTOR
	/* Canary value for the -fstack-protector GCC feature: */
	/* 每 task 栈保护 canary，由架构切换到 current 时使用。 */
	unsigned long			stack_canary;
#endif
	/*
	 * Pointers to the (original) parent process, youngest child, younger sibling,
	 * older sibling, respectively.  (p->father can be replaced with
	 * p->real_parent->pid)
	 */
	/* 亲子、兄弟和线程组拓扑由 tasklist_lock/RCU 保护；这些指针均为借用。 */

	/* Real parent process: */
	/* real_parent 是血缘父进程，RCU 发布。 */
	struct task_struct __rcu	*real_parent;

	/* Recipient of SIGCHLD, wait4() reports: */
	/* parent 是当前 SIGCHLD/wait 接收者，ptrace 可使其不同于 real_parent。 */
	struct task_struct __rcu	*parent;

	/*
	 * Children/sibling form the list of natural children:
	 */
	/* children 为本 task 子链表头，sibling 把本 task 接入父链；group_leader 借用组长。 */
	struct list_head		children;
	struct list_head		sibling;
	struct task_struct		*group_leader;

	/*
	 * 'ptraced' is the list of tasks this task is using ptrace() on.
	 *
	 * This includes both natural children and PTRACE_ATTACH targets.
	 * 'ptrace_entry' is this task's link on the p->parent->ptraced list.
	 */
	/* ptraced 是本 task 正跟踪目标的链头；ptrace_entry 接入 tracer 的该链。 */
	struct list_head		ptraced;
	struct list_head		ptrace_entry;

	/* PID/PID hash table linkage. */
	/* thread_pid 持 PID 对象引用；pid_links/thread_node 接入各 PID 类型和线程组链。 */
	struct pid			*thread_pid;
	struct hlist_node		pid_links[PIDTYPE_MAX];
	struct list_head		thread_node;

	/*
	 * vfork_done 指向父调用栈上的 completion；vfork 父等待子 exec/exit。kthread 创建复用
	 * 它指向私有 exited completion，使 kthread_stop() 等退出。mm_release() 在清指针前
	 * complete；task_lock 与等待侧清 NULL 竞争，禁止在该生命周期外保留此借用。
	 */
	struct completion		*vfork_done;

	/* CLONE_CHILD_SETTID: */
	/* 子线程启动时写入的用户地址；只借用用户指针，写入用 uaccess。 */
	int __user			*set_child_tid;

	/* CLONE_CHILD_CLEARTID: */
	/* 退出时清零并 futex wake 的用户地址。 */
	int __user			*clear_child_tid;

	/* PF_KTHREAD | PF_IO_WORKER */
	/* 类型相关私有指针：PF_KTHREAD 指向 struct kthread，IO worker 有其自身含义。 */
	void				*worker_private;

	/* 用户、系统、guest CPU 时间及可选缩放值；prev/vtime 保证对外单调和精确记账。 */
	u64				utime;
	u64				stime;
#ifdef CONFIG_ARCH_HAS_SCALED_CPUTIME
	u64				utimescaled;
	u64				stimescaled;
#endif
	u64				gtime;
	struct prev_cputime		prev_cputime;
#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
	struct vtime			vtime;
#endif

#ifdef CONFIG_NO_HZ_FULL
	/* full-nohz 强制保留 tick 的依赖位集合。 */
	atomic_t			tick_dep_mask;
#endif
	/* Context switch counts: */
	/* 主动和被动上下文切换累计次数。 */
	unsigned long			nvcsw;
	unsigned long			nivcsw;

	/* Monotonic time in nsecs: */
	/* fork 时记录的单调时钟创建时间，单位纳秒。 */
	u64				start_time;

	/* Boot based time in nsecs: */
	/* 包含 suspend 的 boottime 创建时间，单位纳秒。 */
	u64				start_boottime;

	/* MM fault and swap info: this can arguably be seen as either mm-specific or thread-specific: */
	/* minor/major fault累计计数；按线程保存但也可聚合为进程视角。 */
	unsigned long			min_flt;
	unsigned long			maj_flt;

	/* Empty if CONFIG_POSIX_CPUTIMERS=n */
	/* POSIX CPU timer 状态；关闭配置时类型为空，task 布局仍保留统一成员。 */
	struct posix_cputimers		posix_cputimers;

#ifdef CONFIG_POSIX_CPU_TIMERS_TASK_WORK
	struct posix_cputimers_work	posix_cputimers_work;
#endif

	/* Process credentials: */
	/* 凭据指针按 RCU+COW 管理；task 借用并在替换/释放路径 put。 */

	/* Tracer's credentials at attach: */
	/* attach 时冻结的 tracer 凭据，用于后续 ptrace 权限判断。 */
	const struct cred __rcu		*ptracer_cred;

	/* Objective and real subjective task credentials (COW): */
	/* real_cred 是客观/真实主观身份。 */
	const struct cred __rcu		*real_cred;

	/* Effective (overridable) subjective task credentials (COW): */
	/* cred 是系统调用实际采用的可覆盖主观身份。 */
	const struct cred __rcu		*cred;

#ifdef CONFIG_KEYS
	/* Cached requested key. */
	/* request_key 快路径缓存，持有 key 引用并在替换/退出时 put。 */
	struct key			*cached_requested_key;
#endif

	/*
	 * executable name, excluding path.
	 *
	 * - normally initialized by begin_new_exec()
	 * - set it with set_task_comm() to ensure it is always
	 *   NUL-terminated and zero-padded
	 */
	/* comm 是不含路径的固定长度线程名；只能用 set_task_comm 保证 NUL 和零填充。 */
	char				comm[TASK_COMM_LEN];

	/* 当前路径解析上下文借用，只在 pathname lookup 动态作用域有效。 */
	struct nameidata		*nameidata;

#ifdef CONFIG_SYSVIPC
	/* 每 task SysV semaphore undo 与 shared-memory attach 状态。 */
	struct sysv_sem			sysvsem;
	struct sysv_shm			sysvshm;
#endif
#ifdef CONFIG_DETECT_HUNG_TASK
	/* hung-task 最近调度计数和时间快照。 */
	unsigned long			last_switch_count;
	unsigned long			last_switch_time;
#endif
	/* Filesystem information: */
	/* cwd/root/umask 等 fs_struct 引用，fork/exit 按 CLONE_FS 管理。 */
	struct fs_struct		*fs;

	/* Open file information: */
	/* fdtable/files_struct 引用，fork/exit 按 CLONE_FILES 管理。 */
	struct files_struct		*files;

#ifdef CONFIG_IO_URING
	/* io_uring 每 task 状态和可选限制对象引用。 */
	struct io_uring_task		*io_uring;
	struct io_restriction		*io_uring_restrict;
#endif

	/* Namespaces: */
	/* namespace 集合引用，setns/unshare/exit 负责替换与 put。 */
	struct nsproxy			*nsproxy;

	/* Signal handlers: */
	/*
	 * signal 是线程组共享状态，sighand 是 RCU 发布的处理表；blocked/real/saved 是线程级
	 * 信号掩码，pending 是线程私有队列，sas_* 描述备用信号栈。多数写侧持 siglock。
	 */
	struct signal_struct		*signal;
	struct sighand_struct __rcu		*sighand;
	sigset_t			blocked;
	sigset_t			real_blocked;
	/* Restored if set_restore_sigmask() was used: */
	/* saved_sigmask 在系统调用/信号返回时恢复。 */
	sigset_t			saved_sigmask;
	struct sigpending		pending;
	unsigned long			sas_ss_sp;
	size_t				sas_ss_size;
	unsigned int			sas_ss_flags;

	/* 返回用户态或退出前执行的 task_work 链头，由 current/原子发布协议管理。 */
	struct callback_head		*task_works;

#ifdef CONFIG_AUDIT
	/* audit syscall 上下文、登录 UID 和会话 ID。 */
#ifdef CONFIG_AUDITSYSCALL
	struct audit_context		*audit_context;
#endif
	kuid_t				loginuid;
	unsigned int			sessionid;
#endif
	/* seccomp 过滤状态及 syscall user dispatch 控制状态。 */
	struct seccomp			seccomp;
	struct syscall_user_dispatch	syscall_dispatch;

	/* Thread group tracking: */
	/* exec 序列号帮助等待/记账路径区分父子两次不同的 exec 世代。 */
	u64				parent_exec_id;
	u64				self_exec_id;

	/* Protection against (de-)allocation: mm, files, fs, tty, keyrings, mems_allowed, mempolicy: */
	/* alloc_lock 串行化下列共享对象的安装/拆除；不得据此保护对象内部内容。 */
	spinlock_t			alloc_lock;

	/* Protection of the PI data structures: */
	/* pi_lock 保护优先级继承树及 effective priority；与 rq 锁有严格嵌套顺序。 */
	raw_spinlock_t			pi_lock;

	/* 无锁唤醒队列节点；任务同一时刻只能挂入一条 wake_q。 */
	struct wake_q_node		wake_q;

#ifdef CONFIG_RT_MUTEXES
	/* PI waiters blocked on a rt_mutex held by this task: */
	/* pi_waiters 按最高优先级组织等待者；pi_top_task 是当前捐赠链顶端借用指针。 */
	struct rb_root_cached		pi_waiters;
	/* Updated under owner's pi_lock and rq lock */
	struct task_struct		*pi_top_task;
	/* Deadlock detection and priority inheritance handling: */
	struct rt_mutex_waiter		*pi_blocked_on;
#endif

	/* blocked_on 只在 blocked_lock 下读写，表示普通 mutex 等待关系。 */
	struct mutex			*blocked_on;	/* lock we're blocked on */
	raw_spinlock_t			blocked_lock;

	/*
	 * The task that is boosting this task; a back link for the current
	 * donor stack. Set in schedule() -> find_proxy_task() and only stable
	 * under preempt_disable().
	 */
	/* blocked_donor 是 donor 栈反向链接，仅在禁抢占区间稳定。 */
	struct task_struct		*blocked_donor;

#ifdef CONFIG_DETECT_HUNG_TASK_BLOCKER
	/*
	 * Encoded lock address causing task block (lower 2 bits = type from
	 * <linux/hung_task.h>). Accessed via hung_task_*() helpers.
	 */
	/* blocker 低两位编码锁类型，其余位为导致阻塞的锁地址，只能经 hung_task helper 访问。 */
	unsigned long			blocker;
#endif

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
	int				non_block_count;
#endif

#ifdef CONFIG_TRACE_IRQFLAGS
	/* IRQ/softirq 状态和链键供 irqflags tracing 检查错误开关中断及嵌套。 */
	struct irqtrace_events		irqtrace;
	unsigned int			hardirq_threaded;
	u64				hardirq_chain_key;
	int				softirqs_enabled;
	int				softirq_context;
	int				irq_config;
#endif
#ifdef CONFIG_PREEMPT_RT
	int				softirq_disable_cnt;
#endif

#ifdef CONFIG_LOCKDEP
# define MAX_LOCK_DEPTH			48UL
	/* lockdep 的当前依赖链、递归深度及已持锁栈；仅调试配置存在。 */
	u64				curr_chain_key;
	int				lockdep_depth;
	unsigned int			lockdep_recursion;
	struct held_lock		held_locks[MAX_LOCK_DEPTH];
#endif

#if defined(CONFIG_UBSAN) && !defined(CONFIG_UBSAN_TRAP)
	unsigned int			in_ubsan;
#endif

	/* Journalling filesystem info: */
	/* journal_info 是当前 task 私有的文件系统事务上下文，具体类型由文件系统解释。 */
	void				*journal_info;

	/* Stacked block device info: */
	/* bio_list 与 plug 保存递归块 I/O 和合并提交上下文，只由当前 task 动态借用。 */
	struct bio_list			*bio_list;

	/* Stack plugging: */
	/* plug：当前 task 的块层 plug，聚合请求后在作用域结束时批量提交。 */
	struct blk_plug			*plug;

	/* VM state: */
	/* 内存回收/压缩以及 I/O context 的每 task 动态上下文。 */
	struct reclaim_state		*reclaim_state;

	struct io_context		*io_context;

#ifdef CONFIG_COMPACTION
	struct capture_control		*capture_control;
#endif
	/* Ptrace state: */
	/* ptrace_message 与 last_siginfo 在 stop/通知期间向 tracer 传递事件载荷。 */
	unsigned long			ptrace_message;
	kernel_siginfo_t		*last_siginfo;

	/* task_io_accounting 累计该 task 的字符数、系统调用数和块 I/O 字节。 */
	struct task_io_accounting	ioac;
#ifdef CONFIG_PSI
	/* Pressure stall state */
	/* psi_flags 标记当前 task 正贡献 CPU/memory/I/O stall 的哪些状态。 */
	unsigned int			psi_flags;
#endif
#ifdef CONFIG_TASK_XACCT
	/* Accumulated RSS usage: */
	/* acct_rss_mem1：扩展记账累计的 RSS 使用。 */
	u64				acct_rss_mem1;
	/* Accumulated virtual memory usage: */
	/* acct_vm_mem1：扩展记账累计的虚拟内存使用。 */
	u64				acct_vm_mem1;
	/* stime + utime since last update: */
	/* acct_timexpd：距上次记账更新累计的 system+user CPU 时间。 */
	u64				acct_timexpd;
#endif
#ifdef CONFIG_CPUSETS
	/* Protected by ->alloc_lock: */
	/* mems_allowed 是 cpuset 允许的 NUMA 节点；seqcount 让无锁读者检测并重试更新。 */
	nodemask_t			mems_allowed;
	/* Sequence number to catch updates: */
	/* mems_allowed_seq 让无锁读者发现允许节点集合在读取期间发生变化。 */
	seqcount_spinlock_t		mems_allowed_seq;
	int				cpuset_mem_spread_rotor;
#endif
#ifdef CONFIG_CGROUPS
	/* Control Group info protected by css_set_lock: */
	/* cgroups 是 RCU 发布的 css_set；cg_list 同时受 css_set_lock 与 alloc_lock 约束。 */
	struct css_set __rcu		*cgroups;
	/* cg_list protected by css_set_lock and tsk->alloc_lock: */
	/* cg_list 把 task 链入 css_set，写侧必须同时遵守 css_set_lock 与 alloc_lock。 */
	struct list_head		cg_list;
#ifdef CONFIG_PREEMPT_RT
	struct llist_node		cg_dead_lnode;
#endif	/* CONFIG_PREEMPT_RT */
#endif	/* CONFIG_CGROUPS */
#ifdef CONFIG_X86_CPU_RESCTRL
	u32				closid;
	u32				rmid;
#endif

	/* futex PI/排序所需的调度器侧每 task 状态。 */
	struct futex_sched_data		futex;

#ifdef CONFIG_PERF_EVENTS
	/* perf 上下文和事件链的拥有关系由 perf_event_mutex/RCU 协调。 */
	u8				perf_recursion[PERF_NR_CONTEXTS];
	struct perf_event_context	*perf_event_ctxp;
	struct mutex			perf_event_mutex;
	struct list_head		perf_event_list;
	struct perf_ctx_data __rcu	*perf_ctx_data;
#endif
#ifdef CONFIG_DEBUG_PREEMPT
	unsigned long			preempt_disable_ip;
#endif
#ifdef CONFIG_NUMA
	/* Protected by alloc_lock: */
	/* mempolicy 引用及 interleave/preferred-node 的 fork 快照受 alloc_lock 保护。 */
	struct mempolicy		*mempolicy;
	short				il_prev;
	u8				il_weight;
	short				pref_node_fork;
#endif
#ifdef CONFIG_NUMA_BALANCING
	/* 自动 NUMA balancing 的扫描周期、首选节点、重试时间和延迟 task_work。 */
	int				numa_scan_seq;
	unsigned int			numa_scan_period;
	unsigned int			numa_scan_period_max;
	int				numa_preferred_nid;
	unsigned long			numa_migrate_retry;
	/* Migration stamp: */
	/* node_stamp：最近一次 NUMA 迁移/放置评估的时间戳。 */
	u64				node_stamp;
	u64				last_task_numa_placement;
	u64				last_sum_exec_runtime;
	struct callback_head		numa_work;

	/*
	 * This pointer is only modified for current in syscall and
	 * pagefault context (and for tasks being destroyed), so it can be read
	 * from any of the following contexts:
	 *  - RCU read-side critical section
	 *  - current->numa_group from everywhere
	 *  - task's runqueue locked, task not running
	 */
	/*
	 * numa_group 仅由 current 的 syscall/page-fault 或销毁路径修改；其他 task 读取必须处于
	 * RCU 读侧，或锁住其 rq 且确认未运行。指针为 RCU 借用，不能越过保护域保存。
	 */
	struct numa_group __rcu		*numa_group;

	/*
	 * numa_faults is an array split into four regions:
	 * faults_memory, faults_cpu, faults_memory_buffer, faults_cpu_buffer
	 * in this precise order.
	 *
	 * faults_memory: Exponential decaying average of faults on a per-node
	 * basis. Scheduling placement decisions are made based on these
	 * counts. The values remain static for the duration of a PTE scan.
	 * faults_cpu: Track the nodes the process was running on when a NUMA
	 * hinting fault was incurred.
	 * faults_memory_buffer and faults_cpu_buffer: Record faults per node
	 * during the current scan window. When the scan completes, the counts
	 * in faults_memory and faults_cpu decay and these values are copied.
	 */
	/* 四段数组依次保存 memory/cpu 的稳定计数及本轮缓冲；扫描完成时衰减并合并。 */
	unsigned long			*numa_faults;
	unsigned long			total_numa_faults;

	/*
	 * numa_faults_locality tracks if faults recorded during the last
	 * scan window were remote/local or failed to migrate. The task scan
	 * period is adapted based on the locality of the faults with different
	 * weights depending on whether they were shared or private faults
	 */
	/* 上轮 fault 的 remote/local/迁移失败分类驱动下轮扫描周期自适应。 */
	unsigned long			numa_faults_locality[3];

	unsigned long			numa_pages_migrated;
#endif /* CONFIG_NUMA_BALANCING */

#ifdef CONFIG_SCHED_CACHE
	/* cache_work 评估 LLC 偏好；pref_llc_queued 记录本次入队是否命中。 */
	struct callback_head		cache_work;
	int				preferred_llc;
	/* 1: task was enqueued to its preferred LLC, 0 otherwise */
	/* pref_llc_queued 为 1 表示最近一次确实入队到 preferred_llc。 */
	int				pref_llc_queued;
#endif

	/* rseq 与 mm_cid 向用户态快速每 CPU/每 mm 并发标识协议提供 task 状态。 */
	struct rseq_data		rseq;
	struct sched_mm_cid		mm_cid;

	/* 批量解除映射时累积待刷新 CPU，离开批处理作用域时统一 TLB shootdown。 */
	struct tlbflush_unmap_batch	tlb_ubc;

	/* Cache last used pipe for splice(): */
	/* splice_pipe 和 task_frag 缓存可复用对象，退出时释放，减少热路径分配。 */
	struct pipe_inode_info		*splice_pipe;

	struct page_frag		task_frag;

#ifdef CONFIG_ARCH_HAS_LAZY_MMU_MODE
	struct lazy_mmu_state		lazy_mmu_state;
#endif

#ifdef CONFIG_TASK_DELAY_ACCT
	/* 可选的调度、块 I/O、swapin、回收等延迟记账对象。 */
	struct task_delay_info		*delays;
#endif

#ifdef CONFIG_FAULT_INJECTION
	/* 按 task 控制故障注入命中与第 N 次失败。 */
	int				make_it_fail;
	unsigned int			fail_nth;
#endif
	/*
	 * When (nr_dirtied >= nr_dirtied_pause), it's time to call
	 * balance_dirty_pages() for a dirty throttling pause:
	 */
	/* 脏页数达到阈值后进入 balance_dirty_pages；dirty_paused_when 是节流周期起点。 */
	int				nr_dirtied;
	int				nr_dirtied_pause;
	/* Start of a write-and-pause period: */
	/* dirty_paused_when：本轮写入并进入节流暂停周期的起点。 */
	unsigned long			dirty_paused_when;

#ifdef CONFIG_LATENCYTOP
	int				latency_record_count;
	struct latency_record		latency_record[LT_SAVECOUNT];
#endif
	/*
	 * Time slack values; these are used to round up poll() and
	 * select() etc timeout values. These are in nanoseconds.
	 */
	/* timer_slack_ns 允许合并当前 task 的超时；default 值供 exec/reset 恢复。 */
	u64				timer_slack_ns;
	u64				default_timer_slack_ns;

#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	unsigned int			kasan_depth;
#endif

#ifdef CONFIG_KCSAN
	struct kcsan_ctx		kcsan_ctx;
#ifdef CONFIG_TRACE_IRQFLAGS
	struct irqtrace_events		kcsan_save_irqtrace;
#endif
#ifdef CONFIG_KCSAN_WEAK_MEMORY
	int				kcsan_stack_depth;
#endif
#endif

#ifdef CONFIG_KMSAN
	struct kmsan_ctx		kmsan_ctx;
#endif

#if IS_ENABLED(CONFIG_KUNIT)
	struct kunit			*kunit_test;
#endif

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
	/* Index of current stored address in ret_stack: */
	/* 函数图 tracer 的返回栈、调度时间戳、溢出与暂停状态均按 task 隔离。 */
	int				curr_ret_stack;
	int				curr_ret_depth;

	/* Stack of return addresses for return function tracing: */
	/* ret_stack：函数图 tracer 保存的返回地址栈。 */
	unsigned long			*ret_stack;

	/* Timestamp for last schedule: */
	/* ftrace_timestamp/sleeptime：最近调度点及累计睡眠时间。 */
	unsigned long long		ftrace_timestamp;
	unsigned long long		ftrace_sleeptime;

	/*
	 * Number of functions that haven't been traced
	 * because of depth overrun:
	 */
	/* trace_overrun：因超过返回栈深度而未被跟踪的函数数量。 */
	atomic_t			trace_overrun;

	/* Pause tracing: */
	/* tracing_graph_pause：嵌套暂停函数图跟踪的原子计数。 */
	atomic_t			tracing_graph_pause;
#endif

#ifdef CONFIG_TRACING
	/* Bitmask and counter of trace recursion: */
	/* trace_recursion：各 tracing 上下文的递归位和计数。 */
	unsigned long			trace_recursion;
#endif /* CONFIG_TRACING */

#ifdef CONFIG_KCOV
	/* See kernel/kcov.c for more details. */
	/* KCOV 描述符和缓冲区由 task 在启用期间引用；remote 字段把异步上下文归因到调用者。 */

	/* Coverage collection mode enabled for this task (0 if disabled): */
	/* kcov_mode：0 表示关闭，否则选择 trace PC 或比较数值模式。 */
	unsigned int			kcov_mode;

	/* Size of the kcov_area: */
	/* kcov_size：覆盖缓冲可容纳的条目数。 */
	unsigned int			kcov_size;

	/* Buffer for coverage collection: */
	/* kcov_area：启用期间借用的覆盖输出缓冲。 */
	void				*kcov_area;

	/* KCOV descriptor wired with this task or NULL: */
	/* kcov：当前 task 绑定的 KCOV 描述符，可空。 */
	struct kcov			*kcov;

	/* KCOV descriptor for remote coverage collection from other tasks: */
	/* kcov_remote：接收其他异步 task 远程覆盖的描述符。 */
	struct kcov			*kcov_remote;

	/* KCOV common handle for remote coverage collection: */
	/* kcov_handle：远程覆盖双方匹配使用的公共句柄。 */
	u64				kcov_handle;

	/* KCOV sequence number: */
	/* kcov_sequence：区分重复远程覆盖会话的序列号。 */
	int				kcov_sequence;

	/* Collect coverage from softirq context: */
	/* kcov_softirq：允许把 softirq 覆盖归因到当前远程会话。 */
	unsigned int			kcov_softirq;
#endif

#ifdef CONFIG_MEMCG_V1
	struct mem_cgroup		*memcg_in_oom;
#endif

#ifdef CONFIG_MEMCG
	/* Number of pages to reclaim on returning to userland: */
	/* over_high 触发返回用户态前回收；active_memcg 是临时定向 charge 上下文。 */
	unsigned int			memcg_nr_pages_over_high;

	/* Used by memcontrol for targeted memcg charge: */
	/* active_memcg：定向 memcg charge 动态作用域中的临时目标。 */
	struct mem_cgroup		*active_memcg;

	/* Cache for current->cgroups->memcg->nodeinfo[nid]->objcg lookups: */
	/* objcg：缓存当前 memcg/node 对应的对象 cgroup 借用。 */
	struct obj_cgroup		*objcg;
#endif

#ifdef CONFIG_BLK_CGROUP
	struct gendisk			*throttle_disk;
#endif

#ifdef CONFIG_UPROBES
	/* uprobe_task 保存该线程断点命中、单步和 deferred_xol 状态。 */
	struct uprobe_task		*utask;
#endif
#if defined(CONFIG_BCACHE) || defined(CONFIG_BCACHE_MODULE)
	unsigned int			sequential_io;
	unsigned int			sequential_io_avg;
#endif
	/* 高端内存 local kmap 的嵌套索引/保存状态，随 task 调度保存。 */
	struct kmap_ctrl		kmap_ctrl;
#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
	unsigned long			task_state_change;
# ifdef CONFIG_PREEMPT_RT
	unsigned long			saved_state_change;
# endif
#endif
	/* task_struct 最终释放经 rcu 回调；rcu_users 是延迟释放协议的引用计数。 */
	struct rcu_head			rcu;
	refcount_t			rcu_users;
	int				pagefault_disabled;
#ifdef CONFIG_MMU
	/* OOM reaper 链接与超时 timer；入链/摘链由其专用锁和回调协议管理。 */
	struct task_struct		*oom_reaper_list;
	struct timer_list		oom_reaper_timer;
#endif
#ifdef CONFIG_VMAP_STACK
	struct vm_struct		*stack_vm_area;
#endif
#ifdef CONFIG_THREAD_INFO_IN_TASK
	/* A live task holds one reference: */
	/* 活 task 固有持有一份内核栈引用，get/put_task_stack 管理额外观察者。 */
	refcount_t			stack_refcount;
#endif
#ifdef CONFIG_LIVEPATCH
	int patch_state;
#endif
#ifdef CONFIG_SECURITY
	/* Used by LSM modules for access restriction: */
	/* security 是 LSM 私有 task blob，分配和释放与 task 生命周期绑定。 */
	void				*security;
#endif
#ifdef CONFIG_BPF_SYSCALL
	/* Used by BPF task local storage */
	/* BPF task-local storage 用 RCU 发布；bpf_ctx 是运行 BPF 程序时的动态上下文。 */
	struct bpf_local_storage __rcu	*bpf_storage;
	/* Used for BPF run context */
	/* bpf_ctx：BPF 程序运行期间压入的 per-task 动态上下文。 */
	struct bpf_run_ctx		*bpf_ctx;
#endif
	/* Used by BPF for per-TASK xdp storage */
	/* bpf_net_context 保存 per-task XDP/BPF 网络批处理状态。 */
	struct bpf_net_context		*bpf_net_context;

#ifdef CONFIG_KSTACK_ERASE
	unsigned long			lowest_stack;
#endif
#ifdef CONFIG_KSTACK_ERASE_METRICS
	unsigned long			prev_lowest_stack;
#endif

#ifdef CONFIG_X86_MCE
	void __user			*mce_vaddr;
	__u64				mce_kflags;
	u64				mce_addr;
	__u64				mce_ripv : 1,
					mce_whole_page : 1,
					__mce_reserved : 62;
	struct callback_head		mce_kill_me;
	int				mce_count;
#endif

#ifdef CONFIG_KRETPROBES
	struct llist_head               kretprobe_instances;
#endif
#ifdef CONFIG_RETHOOK
	struct llist_head               rethooks;
#endif

#ifdef CONFIG_ARCH_HAS_PARANOID_L1D_FLUSH
	/*
	 * If L1D flush is supported on mm context switch
	 * then we use this callback head to queue kill work
	 * to kill tasks that are not running on SMT disabled
	 * cores
	 */
	/* 支持切换时 L1D flush 的架构用该 task_work 终止不满足 SMT 隔离条件的 task。 */
	struct callback_head		l1d_flush_kill;
#endif

#ifdef CONFIG_RV
	/*
	 * Per-task RV monitor, fixed in CONFIG_RV_PER_TASK_MONITORS.
	 * If memory becomes a concern, we can think about a dynamic method.
	 */
	/* rv[] 是编译期固定数量的 per-task runtime-verification 监视器状态。 */
	union rv_task_monitor		rv[CONFIG_RV_PER_TASK_MONITORS];
#endif

#ifdef CONFIG_USER_EVENTS
	struct user_event_mm		*user_event_mm;
#endif

#ifdef CONFIG_UNWIND_USER
	struct unwind_task_info		unwind_info;
#endif

	/* CPU-specific state of this task: */
	/* thread 是体系结构上下文切换实际保存/恢复的寄存器及低层线程状态。 */
	struct thread_struct		thread;

	/*
	 * New fields for task_struct should be added above here, so that
	 * they are included in the randomized portion of task_struct.
	 */
	/* 新字段必须放在此标记之前，才能参与 randstruct 对 task_struct 后半布局的随机化。 */
	randomized_struct_fields_end
} __attribute__ ((aligned (64)));

#ifdef CONFIG_SCHED_PROXY_EXEC
/* 静态键让代理执行热路径在关闭时近似零成本；开启时调度器可替阻塞 owner 运行 donor。 */
DECLARE_STATIC_KEY_TRUE(__sched_proxy_exec);
static inline bool sched_proxy_exec(void)
{
	return static_branch_likely(&__sched_proxy_exec);
}
#else
static inline bool sched_proxy_exec(void)
{
	return false;
}
#endif

#define TASK_REPORT_IDLE	(TASK_REPORT + 1)
#define TASK_REPORT_MAX		(TASK_REPORT_IDLE << 1)

/* 将内部 state/exit_state 压缩为 /proc 等用户 ABI 的单一状态下标。 */
static inline unsigned int __task_state_index(unsigned int tsk_state,
					      unsigned int tsk_exit_state)
{
	unsigned int state = (tsk_state | tsk_exit_state) & TASK_REPORT;

	BUILD_BUG_ON_NOT_POWER_OF_2(TASK_REPORT_MAX);

	if ((tsk_state & TASK_IDLE) == TASK_IDLE)
		state = TASK_REPORT_IDLE;

	/*
	 * We're lying here, but rather than expose a completely new task state
	 * to userspace, we can make this appear as if the task has gone through
	 * a regular rt_mutex_lock() call.
	 * Report frozen tasks as uninterruptible.
	 */
	/* RT 锁等待和冻结是内核内部态，对用户态统一伪装成 D，避免扩展既有 ABI。 */
	if ((tsk_state & TASK_RTLOCK_WAIT) || (tsk_state & TASK_FROZEN))
		state = TASK_UNINTERRUPTIBLE;

	return fls(state);
}

static inline unsigned int task_state_index(struct task_struct *tsk)
{
	return __task_state_index(READ_ONCE(tsk->__state), tsk->exit_state);
}

static inline char task_index_to_char(unsigned int state)
{
	/* 顺序必须与 fls() 下标和 TASK_REPORT_MAX 保持同步。 */
	static const char state_char[] = "RSDTtXZPI";

	BUILD_BUG_ON(TASK_REPORT_MAX * 2 != 1 << (sizeof(state_char) - 1));

	return state_char[state];
}

static inline char task_state_to_char(struct task_struct *tsk)
{
	return task_index_to_char(task_state_index(tsk));
}

#ifdef CONFIG_ARCH_HAS_LAZY_MMU_MODE
/**
 * __task_lazy_mmu_mode_active() - Test the lazy MMU mode state for a task.
 * @tsk: The task to check.
 *
 * Test whether @tsk has its lazy MMU mode state set to active (i.e. enabled
 * and not paused).
 *
 * This function only considers the state saved in task_struct; to test whether
 * current actually is in lazy MMU mode, is_lazy_mmu_mode_active() should be
 * used instead.
 *
 * This function is intended for architectures that implement the lazy MMU
 * mode; it must not be called from generic code.
 */
/*
 * __task_lazy_mmu_mode_active() 只检查 @tsk 中保存的计数：enable_count>0 且未 pause 即 true。
 * 参数是调用期稳定借用，不取引用、不睡眠；它不判断中断上下文，只供实现 lazy MMU 的架构。
 */
static inline bool __task_lazy_mmu_mode_active(struct task_struct *tsk)
{
	/* 这里只检查 task 中保存的嵌套状态，不代表中断上下文真的可使用 lazy MMU。 */
	struct lazy_mmu_state *state = &tsk->lazy_mmu_state;

	return state->enable_count > 0 && state->pause_count == 0;
}

/**
 * is_lazy_mmu_mode_active() - Test whether we are currently in lazy MMU mode.
 *
 * Test whether the current context is in lazy MMU mode. This is true if both:
 * 1. We are not in interrupt context
 * 2. Lazy MMU mode is active for the current task
 *
 * This function is intended for architectures that implement the lazy MMU
 * mode; it must not be called from generic code.
 */
/*
 * is_lazy_mmu_mode_active() 查询 current 的真实动态状态；中断上下文直接 false，否则委托
 * 上述 helper。无参数、不睡眠、无副作用，只供实现 lazy MMU 的架构代码调用。
 */
static inline bool is_lazy_mmu_mode_active(void)
{
	/* 中断借用 current，但不继承进程的 lazy MMU 动态作用域。 */
	if (in_interrupt())
		return false;

	return __task_lazy_mmu_mode_active(current);
}
#endif

extern struct pid *cad_pid;

/*
 * Per process flags
 */
/*
 * PF_* 是 current 主写、其他 task 通常只读的非原子生命周期/执行属性；它们不等同于
 * 可被并发 bitops 修改的 atomic_flags。PF_KTHREAD、PF_EXITING、PF_MEMALLOC 等会改变
 * 多个子系统的行为，设置者必须遵守相应创建、退出或内存回收作用域。
 */
#define PF_VCPU			0x00000001	/* I'm a virtual CPU */
/* PF_VCPU：该 task 代表虚拟 CPU。 */
#define PF_IDLE			0x00000002	/* I am an IDLE thread */
/* PF_IDLE：每 CPU idle 线程。 */
#define PF_EXITING		0x00000004	/* Getting shut down */
/* PF_EXITING：已进入不可逆的退出拆除阶段。 */
#define PF_POSTCOREDUMP		0x00000008	/* Coredumps should ignore this task */
/* PF_POSTCOREDUMP：coredump 枚举应忽略该 task。 */
#define PF_IO_WORKER		0x00000010	/* Task is an IO worker */
/* PF_IO_WORKER：io_uring 等 I/O worker。 */
#define PF_WQ_WORKER		0x00000020	/* I'm a workqueue worker */
/* PF_WQ_WORKER：通用 workqueue worker。 */
#define PF_FORKNOEXEC		0x00000040	/* Forked but didn't exec */
/* PF_FORKNOEXEC：fork 后尚未成功 exec。 */
#define PF_MCE_PROCESS		0x00000080      /* Process policy on mce errors */
/* PF_MCE_PROCESS：采用进程级 MCE 错误策略。 */
#define PF_SUPERPRIV		0x00000100	/* Used super-user privileges */
/* PF_SUPERPRIV：曾使用超级用户权限。 */
#define PF_DUMPCORE		0x00000200	/* Dumped core */
/* PF_DUMPCORE：退出时已经生成 core dump。 */
#define PF_SIGNALED		0x00000400	/* Killed by a signal */
/* PF_SIGNALED：退出原因是信号。 */
#define PF_MEMALLOC		0x00000800	/* Allocating memory to free memory. See memalloc_noreclaim_save() */
/* PF_MEMALLOC：为释放内存而分配，可使用紧急储备；必须在受控动态作用域设置。 */
#define PF_NPROC_EXCEEDED	0x00001000	/* set_user() noticed that RLIMIT_NPROC was exceeded */
/* PF_NPROC_EXCEEDED：set_user 发现 RLIMIT_NPROC 已超限。 */
#define PF_USED_MATH		0x00002000	/* If unset the fpu must be initialized before use */
/* PF_USED_MATH：FPU 状态已初始化；未置位时首次使用必须初始化。 */
#define PF_USER_WORKER		0x00004000	/* Kernel thread cloned from userspace thread */
/* PF_USER_WORKER：从用户线程 clone 出的内核 worker。 */
#define PF_NOFREEZE		0x00008000	/* This thread should not be frozen */
/* PF_NOFREEZE：freezer 不应冻结该线程。 */
#define PF_KCOMPACTD		0x00010000	/* I am kcompactd */
/* PF_KCOMPACTD：内存规整守护线程。 */
#define PF_KSWAPD		0x00020000	/* I am kswapd */
/* PF_KSWAPD：页面回收守护线程。 */
#define PF_MEMALLOC_NOFS	0x00040000	/* All allocations inherit GFP_NOFS. See memalloc_nfs_save() */
/* PF_MEMALLOC_NOFS：后续分配继承 GFP_NOFS 限制。 */
#define PF_MEMALLOC_NOIO	0x00080000	/* All allocations inherit GFP_NOIO. See memalloc_noio_save() */
/* PF_MEMALLOC_NOIO：后续分配继承 GFP_NOIO 限制。 */
#define PF_LOCAL_THROTTLE	0x00100000	/* Throttle writes only against the bdi I write to,
						 * I am cleaning dirty pages from some other bdi. */
/* PF_LOCAL_THROTTLE：只按本 task 写入的 bdi 节流，避免代清其他 bdi 时被错误限制。 */
#define PF_KTHREAD		0x00200000	/* I am a kernel thread */
/* PF_KTHREAD：由内核线程框架管理，worker_private 按 kthread 控制块解释。 */
#define PF_RANDOMIZE		0x00400000	/* Randomize virtual address space */
/* PF_RANDOMIZE：exec 时启用地址空间随机化。 */
#define PF__HOLE__00800000	0x00800000
#define PF__HOLE__01000000	0x01000000
#define PF__HOLE__02000000	0x02000000
#define PF_NO_SETAFFINITY	0x04000000	/* Userland is not allowed to meddle with cpus_mask */
/* PF_NO_SETAFFINITY：禁止用户态修改该 task 的 CPU affinity。 */
#define PF_MCE_EARLY		0x08000000      /* Early kill for mce process policy */
/* PF_MCE_EARLY：MCE 进程策略选择提前终止。 */
#define PF_MEMALLOC_PIN		0x10000000	/* Allocations constrained to zones which allow long term pinning.
						 * See memalloc_pin_save() */
/* PF_MEMALLOC_PIN：分配限制在允许长期 pin 页面的 zone。 */
#define PF_BLOCK_TS		0x20000000	/* plug has ts that needs updating */
/* PF_BLOCK_TS：当前 block plug 的时间戳尚待更新。 */
#define PF__HOLE__40000000	0x40000000
#define PF_SUSPEND_TASK		0x80000000      /* This thread called freeze_processes() and should not be frozen */
/* PF_SUSPEND_TASK：发起系统冻结的线程自身不能被 freezer 冻结。 */

/*
 * Only the _current_ task can read/write to tsk->flags, but other
 * tasks can access tsk->flags in readonly mode for example
 * with tsk_used_math (like during threaded core dumping).
 * There is however an exception to this rule during ptrace
 * or during fork: the ptracer task is allowed to write to the
 * child->flags of its traced child (same goes for fork, the parent
 * can write to the child->flags), because we're guaranteed the
 * child is not running and in turn not changing child->flags
 * at the same time the parent does it.
 */
/*
 * 通常只有 current 修改 flags；ptrace/fork 例外成立是因为目标子 task 尚未运行。
 * 下列 FPU 宏因此不能任意用于一个正在别的 CPU 执行的 task。
 */
#define clear_stopped_child_used_math(child)	do { (child)->flags &= ~PF_USED_MATH; } while (0)
#define set_stopped_child_used_math(child)	do { (child)->flags |= PF_USED_MATH; } while (0)
#define clear_used_math()			clear_stopped_child_used_math(current)
#define set_used_math()				set_stopped_child_used_math(current)

#define conditional_stopped_child_used_math(condition, child) \
	do { (child)->flags &= ~PF_USED_MATH, (child)->flags |= (condition) ? PF_USED_MATH : 0; } while (0)

#define conditional_used_math(condition)	conditional_stopped_child_used_math(condition, current)

#define copy_to_stopped_child_used_math(child) \
	do { (child)->flags &= ~PF_USED_MATH, (child)->flags |= current->flags & PF_USED_MATH; } while (0)

/* NOTE: this will return 0 or PF_USED_MATH, it will never return 1 */
#define tsk_used_math(p)			((p)->flags & PF_USED_MATH)
#define used_math()				tsk_used_math(current)

/* is_percpu_thread() 检查 current 是否禁止改 affinity 且只允许一个 CPU；无参数、无副作用。 */
static __always_inline bool is_percpu_thread(void)
{
	/* 禁止用户改 affinity 且仅允许一个 CPU，是 percpu kthread 的组合判据。 */
	return (current->flags & PF_NO_SETAFFINITY) &&
		(current->nr_cpus_allowed  == 1);
}

static __always_inline bool is_user_task(struct task_struct *task)
{
	/* 有用户 mm 且不是内核/用户派生 worker 才按普通用户 task 处理。 */
	return task->mm && !(task->flags & (PF_KTHREAD | PF_USER_WORKER));
}

/* Per-process atomic flags. */
/* PFA_* 可由并发上下文用 bitops 修改，主要承载安全策略和 cpuset 分布提示。 */
#define PFA_NO_NEW_PRIVS		0	/* May not gain new privileges. */
/* 禁止 exec 等路径取得新权限。 */
#define PFA_SPREAD_PAGE			1	/* Spread page cache over cpuset */
/* 在 cpuset 允许节点间分散 page cache 分配。 */
#define PFA_SPREAD_SLAB			2	/* Spread some slab caches over cpuset */
/* 在 cpuset 允许节点间分散部分 slab 分配。 */
#define PFA_SPEC_SSB_DISABLE		3	/* Speculative Store Bypass disabled */
/* 禁用推测性存储绕过。 */
#define PFA_SPEC_SSB_FORCE_DISABLE	4	/* Speculative Store Bypass force disabled*/
/* 永久强制禁用推测性存储绕过。 */
#define PFA_SPEC_IB_DISABLE		5	/* Indirect branch speculation restricted */
/* 限制间接分支推测。 */
#define PFA_SPEC_IB_FORCE_DISABLE	6	/* Indirect branch speculation permanently restricted */
/* 永久限制间接分支推测。 */
#define PFA_SPEC_SSB_NOEXEC		7	/* Speculative Store Bypass clear on execve() */
/* execve 时清除普通 SSB 禁用状态。 */

#define TASK_PFA_TEST(name, func)					\
	static inline bool task_##func(struct task_struct *p)		\
	{ return test_bit(PFA_##name, &p->atomic_flags); }

#define TASK_PFA_SET(name, func)					\
	static inline void task_set_##func(struct task_struct *p)	\
	{ set_bit(PFA_##name, &p->atomic_flags); }

#define TASK_PFA_CLEAR(name, func)					\
	static inline void task_clear_##func(struct task_struct *p)	\
	{ clear_bit(PFA_##name, &p->atomic_flags); }

TASK_PFA_TEST(NO_NEW_PRIVS, no_new_privs)
TASK_PFA_SET(NO_NEW_PRIVS, no_new_privs)

TASK_PFA_TEST(SPREAD_PAGE, spread_page)
TASK_PFA_SET(SPREAD_PAGE, spread_page)
TASK_PFA_CLEAR(SPREAD_PAGE, spread_page)

TASK_PFA_TEST(SPREAD_SLAB, spread_slab)
TASK_PFA_SET(SPREAD_SLAB, spread_slab)
TASK_PFA_CLEAR(SPREAD_SLAB, spread_slab)

TASK_PFA_TEST(SPEC_SSB_DISABLE, spec_ssb_disable)
TASK_PFA_SET(SPEC_SSB_DISABLE, spec_ssb_disable)
TASK_PFA_CLEAR(SPEC_SSB_DISABLE, spec_ssb_disable)

TASK_PFA_TEST(SPEC_SSB_NOEXEC, spec_ssb_noexec)
TASK_PFA_SET(SPEC_SSB_NOEXEC, spec_ssb_noexec)
TASK_PFA_CLEAR(SPEC_SSB_NOEXEC, spec_ssb_noexec)

TASK_PFA_TEST(SPEC_SSB_FORCE_DISABLE, spec_ssb_force_disable)
TASK_PFA_SET(SPEC_SSB_FORCE_DISABLE, spec_ssb_force_disable)

TASK_PFA_TEST(SPEC_IB_DISABLE, spec_ib_disable)
TASK_PFA_SET(SPEC_IB_DISABLE, spec_ib_disable)
TASK_PFA_CLEAR(SPEC_IB_DISABLE, spec_ib_disable)

TASK_PFA_TEST(SPEC_IB_FORCE_DISABLE, spec_ib_force_disable)
TASK_PFA_SET(SPEC_IB_FORCE_DISABLE, spec_ib_force_disable)

/* current_restore_flags() 只把 @flags 掩码内的位恢复为 @orig_flags；仅 current 可写且无返回值。 */
static inline void
current_restore_flags(unsigned long orig_flags, unsigned long flags)
{
	/* 仅恢复调用者声明的位，适合 save/temporary override/restore 动态作用域。 */
	current->flags &= ~flags;
	current->flags |= orig_flags & flags;
}

/*
 * cpuset_cpumask_can_shrink() 只读比较当前/候选掩码，返回 cpuset 是否允许收缩；
 * task_can_attach() 校验 @p 能否迁入新 cgroup，返回 0 或负 errno；
 * dl_bw_alloc()/dl_bw_free() 以 @cpu 和定点 @dl_bw 在 deadline 带宽池中配对预留/归还。
 * 这些操作可能取得调度/cgroup 锁；调用者稳定参数对象，函数不接管其 ownership。
 */
extern int cpuset_cpumask_can_shrink(const struct cpumask *cur, const struct cpumask *trial);
extern int task_can_attach(struct task_struct *p);
extern int dl_bw_alloc(int cpu, u64 dl_bw);
extern void dl_bw_free(int cpu, u64 dl_bw);

/* set_cpus_allowed_force() - consider using set_cpus_allowed_ptr() instead */
/* force 版本绕过部分兼容性限制，普通调用者应使用会验证并迁移 task 的 ptr 版本。 */
extern void set_cpus_allowed_force(struct task_struct *p, const struct cpumask *new_mask);

/**
 * set_cpus_allowed_ptr - set CPU affinity mask of a task
 * @p: the task
 * @new_mask: CPU affinity mask
 *
 * Return: zero if successful, or a negative error code
 */
/*
 * set_cpus_allowed_ptr() 复制输入 @new_mask，在 rq/迁移协议下更新稳定 @p 并按需搬迁；
 * 可睡眠，成功返回 0，空交集、带宽或迁移失败返回负 errno，不接管 task/mask ownership。
 */
extern int set_cpus_allowed_ptr(struct task_struct *p, const struct cpumask *new_mask);
/*
 * set_cpus_allowed_ptr() 复制 @new_mask、在 rq 锁协议下更新 @p 并按需迁移，返回 0/负 errno；
 * dup_user_cpus_ptr() 为 @dst 在 @node 分配并复制 @src 的用户掩码，release 负责释放；
 * dl_task_check_affinity() 验证 deadline task 对 @mask 的带宽约束；force/relax 先临时
 * 强制兼容在线 CPU，再根据保存的 user_cpus_ptr 尝试恢复。掩码均为输入借用。
 */
extern int dup_user_cpus_ptr(struct task_struct *dst, struct task_struct *src, int node);
extern void release_user_cpus_ptr(struct task_struct *p);
extern int dl_task_check_affinity(struct task_struct *p, const struct cpumask *mask);
extern void force_compatible_cpus_allowed_ptr(struct task_struct *p);
extern void relax_compatible_cpus_allowed_ptr(struct task_struct *p);

/*
 * yield_to() 尝试把 current 的 CPU 让给稳定的 @p，@preempt 允许向远端 rq 发抢占请求；
 * 成功返回正值并可能 schedule，策略/状态不允许返回 0，无有效目标返回 -ESRCH。set_user_nice()
 * 在 rq 锁下更新 @p 的 -20..19 nice 并重排；task_prio() 返回用户可见动态 priority。
 * 三者不转移 task 引用，跨 task 调用者必须自行稳定其生命周期。
 */
extern int yield_to(struct task_struct *p, bool preempt);
extern void set_user_nice(struct task_struct *p, long nice);
extern int task_prio(const struct task_struct *p);

/**
 * task_nice - return the nice value of a given task.
 * @p: the task in question.
 *
 * Return: The nice value [ -20 ... 0 ... 19 ].
 */
/* task_nice() 无锁读取稳定 @p 的 static_prio 并转换为 -20..19；不睡眠、无副作用。 */
static inline int task_nice(const struct task_struct *p)
{
	/* static_prio 是内核编码，转换后才是用户可见的 -20..19 nice。 */
	return PRIO_TO_NICE((p)->static_prio);
}

/*
 * can_nice() 对稳定 @p 和目标 nice 做权限测试；task_curr() 查询其是否正运行；idle_cpu()
 * 查询 @cpu 的 rq 是否 idle。sched_setscheduler()/sched_setattr() 更新策略参数并返回 0/负
 * errno，nocheck 仅供已经完成权限检查的内核路径。sched_set_fifo*()/sched_set_normal()
 * 是内核快捷设置器，返回 void；idle_task() 返回指定 CPU 永久存在的 idle task 借用。
 */
extern int can_nice(const struct task_struct *p, const int nice);
extern int task_curr(const struct task_struct *p);
extern int idle_cpu(int cpu);
extern int sched_setscheduler(struct task_struct *, int, const struct sched_param *);
extern int sched_setscheduler_nocheck(struct task_struct *, int, const struct sched_param *);
extern void sched_set_fifo(struct task_struct *p);
extern void sched_set_fifo_low(struct task_struct *p);
extern void sched_set_fifo_secondary(struct task_struct *p);
extern void sched_set_normal(struct task_struct *p, int nice);
extern int sched_setattr(struct task_struct *, const struct sched_attr *);
extern int sched_setattr_nocheck(struct task_struct *, const struct sched_attr *);
/* idle_task() 返回 @cpu 永久存在的 idle task 借用；调用者不得 put 或修改其生命周期。 */
extern struct task_struct *idle_task(int cpu);

/**
 * is_idle_task - is the specified task an idle task?
 * @p: the task in question.
 *
 * Return: 1 if @p is an idle task. 0 otherwise.
 */
/* is_idle_task() 读取稳定 @p 的 PF_IDLE 快照；返回布尔值，不取引用、不睡眠。 */
static __always_inline bool is_idle_task(const struct task_struct *p)
{
	/* 依据 PF_IDLE 而非 PID/comm，适用于每 CPU idle task。 */
	return !!(p->flags & PF_IDLE);
}

/*
 * curr_task() 返回 @cpu 当前 task 的瞬时借用；ia64_set_curr_task() 是 IA64 专用发布入口；
 * yield() 在可调度进程上下文主动让出 CPU，返回时 current 再获运行资格，无返回值。
 */
extern struct task_struct *curr_task(int cpu);
extern void ia64_set_curr_task(int cpu, struct task_struct *p);

void yield(void);

union thread_union {
	/* 某些架构将 task/thread_info 与内核栈组合为 THREAD_SIZE 对齐的联合体。 */
	struct task_struct task;
#ifndef CONFIG_THREAD_INFO_IN_TASK
	struct thread_info thread_info;
#endif
	unsigned long stack[THREAD_SIZE/sizeof(long)];
};

#ifndef CONFIG_THREAD_INFO_IN_TASK
extern struct thread_info init_thread_info;
#endif

extern unsigned long init_stack[THREAD_SIZE / sizeof(unsigned long)];

#ifdef CONFIG_THREAD_INFO_IN_TASK
/* 配置决定 thread_info 内嵌 task_struct，还是位于内核栈底部。 */
# define task_thread_info(task)	(&(task)->thread_info)
#else
# define task_thread_info(task)	((struct thread_info *)(task)->stack)
#endif

/*
 * find a task by one of its numerical ids
 *
 * find_task_by_pid_ns():
 *      finds a task by its pid in the specified namespace
 * find_task_by_vpid():
 *      finds a task by its virtual pid
 *
 * see also find_vpid() etc in include/linux/pid.h
 */
/* PID 查找通常要求 RCU 读侧或 tasklist 相关锁；返回值默认不增加 task 引用。 */

extern struct task_struct *find_task_by_vpid(pid_t nr);
extern struct task_struct *find_task_by_pid_ns(pid_t nr, struct pid_namespace *ns);

/*
 * find a task by its virtual pid and get the task struct
 */
/* find_get 变体在成功时增加 task 引用，调用者必须 put_task_struct()。 */
extern struct task_struct *find_get_task_by_vpid(pid_t nr);

/*
 * wake_up_state() 只匹配 @state 睡眠位，wake_up_process() 匹配常规可唤醒态；二者返回
 * 是否使稳定的 @tsk 进入可运行状态。wake_up_new_task() 发布刚 fork 的新 task 并首次入队，
 * 返回 void。调用者不转移 task 引用；跨 CPU 唤醒由 try_to_wake_up 的屏障/rq 锁协议完成。
 */
extern int wake_up_state(struct task_struct *tsk, unsigned int state);
extern int wake_up_process(struct task_struct *tsk);
extern void wake_up_new_task(struct task_struct *tsk);

/* kick_process() 向正在远端 CPU 运行的稳定 @tsk 发重调度 IPI；不睡眠、无返回值。 */
extern void kick_process(struct task_struct *tsk);

/* 写 comm 必须走该入口，以保证固定长度、NUL 终止并触发相关 trace/通知语义。 */
extern void __set_task_comm(struct task_struct *tsk, const char *from, bool exec);
#define set_task_comm(tsk, from) ({			\
	BUILD_BUG_ON(sizeof(from) != TASK_COMM_LEN);	\
	__set_task_comm(tsk, from, false);		\
})

/*
 * - Why not use task_lock()?
 *   User space can randomly change their names anyway, so locking for readers
 *   doesn't make sense. For writers, locking is probably necessary, as a race
 *   condition could lead to long-term mixed results.
 *   The strscpy_pad() in __set_task_comm() can ensure that the task comm is
 *   always NUL-terminated and zero-padded. Therefore the race condition between
 *   reader and writer is not an issue.
 *
 * - BUILD_BUG_ON() can help prevent the buf from being truncated.
 *   Since the callers don't perform any return value checks, this safeguard is
 *   necessary.
 */
/*
 * 读侧允许与写侧竞争；当前 __set_task_comm() 用有界 memcpy 后 memset 尾部，效果是始终
 * NUL 终止并填零，可能读到混合名称但不会越界。原文提到的 strscpy_pad 与当前实现不符，
 * 这是实现手段变化，不影响其安全结论。数组参数需至少 TASK_COMM_LEN，BUILD_BUG_ON
 * 防止静默截断。
 */
#define get_task_comm(buf, tsk) ({			\
	BUILD_BUG_ON(sizeof(buf) < TASK_COMM_LEN);	\
	strscpy_pad(buf, (tsk)->comm);			\
	buf;						\
})

static __always_inline void scheduler_ipi(void)
{
	/*
	 * Fold TIF_NEED_RESCHED into the preempt_count; anybody setting
	 * TIF_NEED_RESCHED remotely (for the first time) will also send
	 * this IPI.
	 */
	/* 把远端设置的 TIF_NEED_RESCHED 折叠进本 CPU preempt_count，使退出路径看见抢占请求。 */
	preempt_fold_need_resched();
}

/*
 * wait_task_inactive() 等稳定 task 离开 CPU；@match_state 非零时还要求状态匹配，返回包含
 * nvcsw 快照的非零 token，状态改变则返回 0。可自旋/调度等待，不能持目标运行所需的锁。
 */
extern unsigned long wait_task_inactive(struct task_struct *, unsigned int match_state);

/*
 * Set thread flags in other task's structures.
 * See asm/thread_info.h for TIF_xxxx flags available:
 */
/*
 * set/clear/update_tsk_thread_flag() 对稳定 @tsk 的 @flag 原子置位、清位或按 @value 更新；
 * test_and_set/test_and_clear 返回修改前位值，test 只返回快照。它们都不睡眠、不取得 task
 * 引用；原子 bitops 仅保证该位更新，具体 TIF 位何时由目标 CPU消费仍取决于配对 IPI/屏障。
 */
static inline void set_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	set_ti_thread_flag(task_thread_info(tsk), flag);
}

/* clear_tsk_thread_flag() 原子清除稳定 @tsk 的 @flag；不睡眠、无返回值。 */
static inline void clear_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	clear_ti_thread_flag(task_thread_info(tsk), flag);
}

/* update_tsk_thread_flag() 按 @value 原子更新稳定 @tsk 的 @flag；不睡眠、无返回值。 */
static inline void update_tsk_thread_flag(struct task_struct *tsk, int flag,
					  bool value)
{
	update_ti_thread_flag(task_thread_info(tsk), flag, value);
}

/* test_and_set_tsk_thread_flag() 原子置位并返回旧值；参数仅在调用期间借用。 */
static inline int test_and_set_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	return test_and_set_ti_thread_flag(task_thread_info(tsk), flag);
}

/* test_and_clear_tsk_thread_flag() 原子清位并返回旧值；不取得 task 引用。 */
static inline int test_and_clear_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	return test_and_clear_ti_thread_flag(task_thread_info(tsk), flag);
}

/* test_tsk_thread_flag() 返回稳定 @tsk 的 @flag 瞬时值；不修改状态。 */
static inline int test_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	return test_ti_thread_flag(task_thread_info(tsk), flag);
}

static inline void set_tsk_need_resched(struct task_struct *tsk)
{
	/* 仅在 0->1 首次请求时记录 trace；真正远端 IPI 由调度器调用路径负责。 */
	if (tracepoint_enabled(sched_set_need_resched_tp) &&
	    !test_tsk_thread_flag(tsk, TIF_NEED_RESCHED))
		__trace_set_need_resched(tsk, TIF_NEED_RESCHED);
	set_tsk_thread_flag(tsk,TIF_NEED_RESCHED);
}

static inline void clear_tsk_need_resched(struct task_struct *tsk)
{
	/* 普通与 lazy 重调度位一起清除，避免残留较弱请求。 */
	atomic_long_andnot(_TIF_NEED_RESCHED | _TIF_NEED_RESCHED_LAZY,
			   (atomic_long_t *)&task_thread_info(tsk)->flags);
}

static inline int test_tsk_need_resched(struct task_struct *tsk)
{
	/* 返回目标普通 NEED_RESCHED 位的瞬时布尔值，不包含 lazy 位。 */
	return unlikely(test_tsk_thread_flag(tsk,TIF_NEED_RESCHED));
}

static inline void set_need_resched_current(void)
{
	/* 关中断保证 current 与本 CPU 抢占状态的两个标记一致更新。 */
	lockdep_assert_irqs_disabled();
	set_tsk_need_resched(current);
	set_preempt_need_resched();
}

/*
 * cond_resched() and cond_resched_lock(): latency reduction via
 * explicit rescheduling in places that are safe. The return
 * value indicates whether a reschedule was done in fact.
 * cond_resched_lock() will drop the spinlock before scheduling,
 */
/*
 * cond_resched 系列只在显式安全点降低长临界路径延迟；返回非零表示确实调度过。
 * lock 版本会暂时释放并重新获取锁，因此受保护数据必须在返回后重新验证。
 */
#if !defined(CONFIG_PREEMPTION) || defined(CONFIG_PREEMPT_DYNAMIC)
extern int __cond_resched(void);

#if defined(CONFIG_PREEMPT_DYNAMIC) && defined(CONFIG_HAVE_PREEMPT_DYNAMIC_CALL)

DECLARE_STATIC_CALL(cond_resched, __cond_resched);

static __always_inline int _cond_resched(void)
{
	return static_call_mod(cond_resched)();
}

#elif defined(CONFIG_PREEMPT_DYNAMIC) && defined(CONFIG_HAVE_PREEMPT_DYNAMIC_KEY)

/* dynamic_cond_resched() 由动态静态键选择当前抢占模型，返回是否发生调度。 */
extern int dynamic_cond_resched(void);

/* 该 _cond_resched() 包装动态键实现；无参数，返回值原样传递。 */
static __always_inline int _cond_resched(void)
{
	return dynamic_cond_resched();
}

#else /* !CONFIG_PREEMPTION */

/* 非抢占内核直接调用 __cond_resched() 慢路径，返回是否调度。 */
static inline int _cond_resched(void)
{
	return __cond_resched();
}

#endif /* PREEMPT_DYNAMIC && CONFIG_HAVE_PREEMPT_DYNAMIC_CALL */

#else /* CONFIG_PREEMPTION && !CONFIG_PREEMPT_DYNAMIC */

/* 固定抢占内核无需显式安全点调度，_cond_resched() 无副作用并返回 0。 */
static inline int _cond_resched(void)
{
	return 0;
}

#endif /* !CONFIG_PREEMPTION || CONFIG_PREEMPT_DYNAMIC */

#define cond_resched() ({			\
	__might_resched(__FILE__, __LINE__, 0);	\
	_cond_resched();			\
})

/*
 * 三个 __cond_resched_*() 都要求入口持有对应锁，必要时释放、schedule 再重新获取，返回
 * 是否调度过；read/write 版本分别保持共享/独占持锁后置条件。调用者返回后须重验数据。
 */
extern int __cond_resched_lock(spinlock_t *lock) __must_hold(lock);
extern int __cond_resched_rwlock_read(rwlock_t *lock) __must_hold_shared(lock);
extern int __cond_resched_rwlock_write(rwlock_t *lock) __must_hold(lock);

#define MIGHT_RESCHED_RCU_SHIFT		8
#define MIGHT_RESCHED_PREEMPT_MASK	((1U << MIGHT_RESCHED_RCU_SHIFT) - 1)

#ifndef CONFIG_PREEMPT_RT
/*
 * Non RT kernels have an elevated preempt count due to the held lock,
 * but are not allowed to be inside a RCU read side critical section
 */
/* 非 RT 自旋锁只贡献 preempt offset，不隐含 RCU 读侧深度。 */
# define PREEMPT_LOCK_RESCHED_OFFSETS	PREEMPT_LOCK_OFFSET
#else
/*
 * spin/rw_lock() on RT implies rcu_read_lock(). The might_sleep() check in
 * cond_resched*lock() has to take that into account because it checks for
 * preempt_count() and rcu_preempt_depth().
 */
/* PREEMPT_RT 的睡眠型 spin/rw lock 同时带 RCU 语义，检查偏移需扣除两者。 */
# define PREEMPT_LOCK_RESCHED_OFFSETS	\
	(PREEMPT_LOCK_OFFSET + (1U << MIGHT_RESCHED_RCU_SHIFT))
#endif

#define cond_resched_lock(lock) ({						\
	__might_resched(__FILE__, __LINE__, PREEMPT_LOCK_RESCHED_OFFSETS);	\
	__cond_resched_lock(lock);						\
})

#define cond_resched_rwlock_read(lock) ({					\
	__might_resched(__FILE__, __LINE__, PREEMPT_LOCK_RESCHED_OFFSETS);	\
	__cond_resched_rwlock_read(lock);					\
})

#define cond_resched_rwlock_write(lock) ({					\
	__might_resched(__FILE__, __LINE__, PREEMPT_LOCK_RESCHED_OFFSETS);	\
	__cond_resched_rwlock_write(lock);					\
})

#ifndef CONFIG_PREEMPT_RT

/*
 * __get_task_blocked_on() 在已持 @p->blocked_lock 时返回 blocked_on 借用；
 * __set_task_blocked_on() 只允许 current 登记非空 @m 且禁止覆盖另一把锁；
 * __clear_task_blocked_on() 校验可选 @m 后清除，clear_task_blocked_on() 自行加 irqsave 锁。
 * 都不改变 mutex ownership；PREEMPT_RT 的 rt_mutex 版本为空，因为等待关系由 RT 锁维护。
 */
static inline struct mutex *__get_task_blocked_on(struct task_struct *p)
{
	/* blocked_lock 是 blocked_on 关系的唯一串行化条件；返回值只是锁内借用。 */
	lockdep_assert_held_once(&p->blocked_lock);
	return p->blocked_on;
}

static inline void __set_task_blocked_on(struct task_struct *p, struct mutex *m)
{
	WARN_ON_ONCE(!m);
	/* The task should only be setting itself as blocked */
	WARN_ON_ONCE(p != current);
	/* Currently we serialize blocked_on under the task::blocked_lock */
	lockdep_assert_held_once(&p->blocked_lock);
	/*
	 * Check ensure we don't overwrite existing mutex value
	 * with a different mutex. Note, setting it to the same
	 * lock repeatedly is ok.
	 */
	/* 只允许 task 为自己登记，且禁止覆盖成另一把 mutex，以维持等待图一致。 */
	WARN_ON_ONCE(p->blocked_on && p->blocked_on != m);
	p->blocked_on = m;
}

static inline void __clear_task_blocked_on(struct task_struct *p, struct mutex *m)
{
	/* Currently we serialize blocked_on under the task::blocked_lock */
	lockdep_assert_held_once(&p->blocked_lock);
	/*
	 * There may be cases where we re-clear already cleared
	 * blocked_on relationships, but make sure we are not
	 * clearing the relationship with a different lock.
	 */
	/* 重复清除可接受，但传入 m 时必须与现有关系一致。 */
	WARN_ON_ONCE(m && p->blocked_on && p->blocked_on != m);
	p->blocked_on = NULL;
}

static inline void clear_task_blocked_on(struct task_struct *p, struct mutex *m)
{
	guard(raw_spinlock_irqsave)(&p->blocked_lock);
	__clear_task_blocked_on(p, m);
}
#else
static inline void __clear_task_blocked_on(struct task_struct *p, struct rt_mutex *m)
{
}

/* PREEMPT_RT 的公开 clear stub 同样无副作用；等待关系由 rt_mutex 核心维护。 */
static inline void clear_task_blocked_on(struct task_struct *p, struct rt_mutex *m)
{
}
#endif /* !CONFIG_PREEMPT_RT */

/* need_resched() 查询 current 是否有立即重调度请求；不睡眠、无副作用，返回瞬时布尔值。 */
static __always_inline bool need_resched(void)
{
	/* 当前 task 热路径查询，由底层 thread flag/preempt 状态提供所需可见性。 */
	return unlikely(tif_need_resched());
}

/*
 * Wrappers for p->thread_info->cpu access. No-op on UP.
 */
/* SMP 上 cpu 字段并发变化，READ_ONCE 只给一致单次快照；稳定性仍需 rq/pi 等锁。 */
#ifdef CONFIG_SMP

/* task_cpu() 取 @p 所属 CPU 快照；set_task_cpu() 只能在调度器规定的 rq 锁/迁移协议下发布。 */
static inline unsigned int task_cpu(const struct task_struct *p)
{
	return READ_ONCE(task_thread_info(p)->cpu);
}

extern void set_task_cpu(struct task_struct *p, unsigned int cpu);

#else

static inline unsigned int task_cpu(const struct task_struct *p)
{
	return 0;
}

/* UP 上唯一 CPU 恒为 0，set_task_cpu() 保留统一调用接口但无副作用。 */
static inline void set_task_cpu(struct task_struct *p, unsigned int cpu)
{
}

#endif /* CONFIG_SMP */

/* task_is_runnable() 读取稳定 @p 的 rq/delayed 状态快照；不取引用、不睡眠。 */
static inline bool task_is_runnable(struct task_struct *p)
{
	/* sched_delayed 表示逻辑留在 rq 但已延迟出队，不能当作真正 runnable。 */
	return p->on_rq && !p->se.sched_delayed;
}

/*
 * sched_task_on_rq() 直接返回 @p 是否处于普通 queued 状态的瞬时快照；get_wchan() 返回睡眠 task 的内核
 * 等待地址或 0；cpu_curr_snapshot() 返回 @cpu 当前 task 的无引用快照。调用者负责稳定 task。
 */
extern bool sched_task_on_rq(struct task_struct *p);
extern unsigned long get_wchan(struct task_struct *p);
extern struct task_struct *cpu_curr_snapshot(int cpu);

/*
 * In order to reduce various lock holder preemption latencies provide an
 * interface to see if a vCPU is currently running or not.
 *
 * This allows us to terminate optimistic spin loops and block, analogous to
 * the native optimistic spin heuristic of testing if the lock owner task is
 * running or not.
 */
/* 虚拟机可覆盖此钩子；检测 vCPU 被抢占可避免锁 owner 实际未运行时空转。 */
#ifndef vcpu_is_preempted
static inline bool vcpu_is_preempted(int cpu)
{
	return false;
}
#endif

/* set/getaffinity 是 PID 层内核入口：复制输入/填充输出 mask，成功返回 0，失败返回负 errno。 */
extern long sched_setaffinity(pid_t pid, const struct cpumask *new_mask);
extern long sched_getaffinity(pid_t pid, struct cpumask *mask);

#ifndef TASK_SIZE_OF
#define TASK_SIZE_OF(tsk)	TASK_SIZE
#endif

static inline bool owner_on_cpu(struct task_struct *owner)
{
	/*
	 * As lock holder preemption issue, we both skip spinning if
	 * task is not on cpu or its cpu is preempted
	 */
	/* on_cpu 只是快照启发式；同时排除承载 owner 的 vCPU 已被宿主抢占。 */
	return READ_ONCE(owner->on_cpu) && !vcpu_is_preempted(task_cpu(owner));
}

/* Returns effective CPU energy utilization, as seen by the scheduler */
/* 返回调度器纳入 capacity/frequency 等修正后的 CPU 能耗模型利用率。 */
unsigned long sched_cpu_util(int cpu);

#ifdef CONFIG_SCHED_CORE
/*
 * sched_core_fork()/free() 随 task 生命周期复制/释放 core cookie；share_pid() 按 @cmd、@pid、
 * @type 操作共享关系并经 @uaddr 交换用户结果，返回 0/负 errno；idle_cpu() 返回 core 视角
 * 的 idle 状态。关闭配置的空实现不取得资源，idle 查询退化为普通 idle_cpu()。
 */
extern void sched_core_free(struct task_struct *tsk);
extern void sched_core_fork(struct task_struct *p);
extern int sched_core_share_pid(unsigned int cmd, pid_t pid, enum pid_type type,
				unsigned long uaddr);
extern int sched_core_idle_cpu(int cpu);
#else
static inline void sched_core_free(struct task_struct *tsk) { }
static inline void sched_core_fork(struct task_struct *p) { }
static inline int sched_core_idle_cpu(int cpu) { return idle_cpu(cpu); }
#endif

/* sched_set_stop_task() 在调度器初始化/hotplug 协议下给 @cpu 安装稳定的 stop task 借用。 */
extern void sched_set_stop_task(int cpu, struct task_struct *stop);

#ifdef CONFIG_MEM_ALLOC_PROFILING
static __always_inline struct alloc_tag *alloc_tag_save(struct alloc_tag *tag)
{
	/* 交换 current 的分配归因标签，并把旧标签借给 restore 配对恢复。 */
	swap(current->alloc_tag, tag);
	return tag;
}

static __always_inline void alloc_tag_restore(struct alloc_tag *tag, struct alloc_tag *old)
{
	/* 校验动态作用域仍持 @tag 后恢复 @old；二者均为借用，不改变 alloc_tag 生命周期。 */
#ifdef CONFIG_MEM_ALLOC_PROFILING_DEBUG
	WARN(current->alloc_tag != tag, "current->alloc_tag was changed:\n");
#endif
	current->alloc_tag = old;
}
#else
#define alloc_tag_save(_tag)			NULL
#define alloc_tag_restore(_tag, _old)		do {} while (0)
#endif

/* Avoids recursive inclusion hell */
/* mm_cid 头文件依赖会形成环，故在此提供最小 exec/exit 生命周期接口。 */
#ifdef CONFIG_SCHED_MM_CID
/*
 * before_execve()/after_execve() 包围 mm 替换并迁移 CID 状态，exit() 做最终摘除；均操作稳定
 * @t 且不接管引用。task_mm_cid() 清除内部 ONCPU/TRANSIT 位后返回用户可用 CID 快照。
 */
void sched_mm_cid_before_execve(struct task_struct *t);
void sched_mm_cid_after_execve(struct task_struct *t);
void sched_mm_cid_exit(struct task_struct *t);
static __always_inline int task_mm_cid(struct task_struct *t)
{
	return t->mm_cid.cid & ~(MM_CID_ONCPU | MM_CID_TRANSIT);
}
#else
static inline void sched_mm_cid_before_execve(struct task_struct *t) { }
static inline void sched_mm_cid_after_execve(struct task_struct *t) { }
static inline void sched_mm_cid_exit(struct task_struct *t) { }
/* 关闭 mm_cid 时 task_mm_cid() 退化为 task_cpu()，返回功能正确的 per-CPU 标识。 */
static __always_inline int task_mm_cid(struct task_struct *t)
{
	/*
	 * Use the processor id as a fall-back when the mm cid feature is
	 * disabled. This provides functional per-cpu data structure accesses
	 * in user-space, althrough it won't provide the memory usage benefits.
	 */
	/* 关闭 mm_cid 时退化为 CPU id，保证功能正确但失去按 mm 压缩槽位的节省。 */
	return task_cpu(t);
}
#endif

#ifdef CONFIG_SCHED_CACHE

/* 每 CPU runtime/epoch 采样汇总为 LLC footprint 与 runnable 平均值。 */
struct sched_cache_time {
	u64 runtime;
	unsigned long epoch;
};

struct sched_cache_stat {
	struct sched_cache_time __percpu *pcpu_sched;
	raw_spinlock_t lock;
	unsigned long epoch;
	u64 nr_running_avg;
	unsigned long next_scan;
	unsigned long footprint;
	int cpu;
} ____cacheline_aligned_in_smp;

#else

struct sched_cache_stat { };

#endif

#ifndef MODULE
#ifndef COMPILE_OFFSETS

/* ___migrate_enable() 是最外层恢复 affinity 的慢路径，只由下方配对 helper 调用。 */
extern void ___migrate_enable(void);

struct rq;
DECLARE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);

/*
 * The "struct rq" is not available here, so we can't access the
 * "runqueues" with this_cpu_ptr(), as the compilation will fail in
 * this_cpu_ptr() -> raw_cpu_ptr() -> __verify_pcpu_ptr():
 *   typeof((ptr) + 0)
 *
 * So use arch_raw_cpu_ptr()/PERCPU_PTR() directly here.
 */
/* rq 尚是不完整类型，不能触发 this_cpu_ptr 的类型验证，只能用原始 percpu 地址加已知偏移。 */
#ifdef CONFIG_SMP
#define this_rq_raw() arch_raw_cpu_ptr(&runqueues)
#else
#define this_rq_raw() PERCPU_PTR(&runqueues)
#endif
#define this_rq_pinned() (*(unsigned int *)((void *)this_rq_raw() + RQ_nr_pinned))

static inline void __migrate_enable(void)
{
	struct task_struct *p = current;

#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Check both overflow from migrate_disable() and superfluous
	 * migrate_enable().
	 */
	/* DEBUG 下同时抓嵌套计数溢出和未配对 enable。 */
	if (WARN_ON_ONCE((s16)p->migration_disabled <= 0))
		return;
#endif

	if (p->migration_disabled > 1) {
		p->migration_disabled--;
		return;
	}

	/*
	 * Ensure stop_task runs either before or after this, and that
	 * __set_cpus_allowed_ptr(SCA_MIGRATE_ENABLE) doesn't schedule().
	 */
	/* 最外层 enable 在禁抢占下恢复常规 cpus_mask，和 stop task 严格排序。 */
	guard(preempt)();
	if (unlikely(p->cpus_ptr != &p->cpus_mask))
		___migrate_enable();
	/*
	 * Mustn't clear migration_disabled() until cpus_ptr points back at the
	 * regular cpus_mask, otherwise things that race (eg.
	 * select_fallback_rq) get confused.
	 */
	/* 先恢复 cpus_ptr，再以编译器屏障后清计数，避免竞态读者观察到矛盾状态。 */
	barrier();
	p->migration_disabled = 0;
	this_rq_pinned()--;
}

static inline void __migrate_disable(void)
{
	/* 嵌套只增加计数；最外层在禁抢占下增加本 rq pinned 数并固定 current。 */
	struct task_struct *p = current;

	if (p->migration_disabled) {
#ifdef CONFIG_DEBUG_PREEMPT
		/*
		 *Warn about overflow half-way through the range.
		 */
		WARN_ON_ONCE((s16)p->migration_disabled < 0);
#endif
		p->migration_disabled++;
		return;
	}

	guard(preempt)();
	this_rq_pinned()++;
	p->migration_disabled = 1;
}
#else /* !COMPILE_OFFSETS */
/* 生成结构偏移的特殊编译不实例化 rq 访问，两个内部迁移 helper 均为空。 */
static inline void __migrate_disable(void) { }
static inline void __migrate_enable(void) { }
#endif /* !COMPILE_OFFSETS */

/*
 * So that it is possible to not export the runqueues variable, define and
 * export migrate_enable/migrate_disable in kernel/sched/core.c too, and use
 * them for the modules. The macro "INSTANTIATE_EXPORTED_MIGRATE_DISABLE" will
 * be defined in kernel/sched/core.c.
 */
/* 内建代码内联实现；模块走导出符号，从而无需导出私有 per-CPU runqueues。 */
#ifndef INSTANTIATE_EXPORTED_MIGRATE_DISABLE
static __always_inline void migrate_disable(void)
{
	__migrate_disable();
}

static __always_inline void migrate_enable(void)
{
	__migrate_enable();
}
#else /* INSTANTIATE_EXPORTED_MIGRATE_DISABLE */
/* core.c 实例化时只声明导出实现，避免与本头文件 inline 定义冲突。 */
extern void migrate_disable(void);
extern void migrate_enable(void);
#endif /* INSTANTIATE_EXPORTED_MIGRATE_DISABLE */

#else /* MODULE */
/* 模块始终调用 core.c 导出的配对接口；disable/enable 必须严格嵌套。 */
extern void migrate_disable(void);
extern void migrate_enable(void);
#endif /* MODULE */

DEFINE_LOCK_GUARD_0(migrate, migrate_disable(), migrate_enable())

#endif
