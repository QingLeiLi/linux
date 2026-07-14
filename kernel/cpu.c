// SPDX-License-Identifier: GPL-2.0
/*
 * CPU control.
 * (C) 2001, 2002, 2003, 2004 Rusty Russell
 *
 * 本文件实现 CPU 热插拔（hotplug）状态机，是 SMP 初始化和
 * 运行时 CPU 上下线的核心。
 *
 * 核心概念：CPU 热插拔状态机
 * ──────────────────────────────────────────
 * 每个 CPU 都有一个状态（enum cpuhp_state），从 OFFLINE 到 ONLINE
 * 经过约 30 个中间状态，每个状态对应一组 startup/teardown 回调。
 *
 * 上线路径（bringup）：OFFLINE → ... → ONLINE
 *   每经过一个状态，调用该状态注册的 startup 回调
 *
 * 下线路径（teardown）：ONLINE → ... → OFFLINE
 *   每经过一个状态，调用该状态注册的 teardown 回调
 *
 * 关键参与者：
 *   BP（Boot Processor）：boot CPU，CPU0，负责控制其他 CPU 的上下线
 *   AP（Application Processor）：其他 CPU，被 BP 控制
 *
 * 启动时调用链（来自 main.c 的 kernel_init_freeable）：
 *   smp_init()
 *     → bringup_nonboot_cpus()
 *       → cpu_up()
 *         → _cpu_up()
 *           → cpuhp_up_callbacks()（BP 侧状态机推进）
 *           → bringup_cpu()（唤醒 AP，等待 AP 完成低级初始化）
 *           → cpuhp_kick_ap()（AP 侧热插拔线程推进剩余状态）
 */
#include <linux/sched/mm.h>
#include <linux/proc_fs.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/sched/signal.h>
#include <linux/sched/hotplug.h>
#include <linux/sched/isolation.h>
#include <linux/sched/task.h>
#include <linux/sched/smt.h>
#include <linux/unistd.h>
#include <linux/cpu.h>
#include <linux/oom.h>
#include <linux/rcupdate.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/bug.h>
#include <linux/kthread.h>
#include <linux/stop_machine.h>
#include <linux/mutex.h>
#include <linux/gfp.h>
#include <linux/suspend.h>
#include <linux/lockdep.h>
#include <linux/tick.h>
#include <linux/irq.h>
#include <linux/nmi.h>
#include <linux/smpboot.h>
#include <linux/relay.h>
#include <linux/slab.h>
#include <linux/scs.h>
#include <linux/percpu-rwsem.h>
#include <linux/cpuset.h>
#include <linux/random.h>
#include <linux/cc_platform.h>
#include <linux/parser.h>

#include <trace/events/power.h>
#define CREATE_TRACE_POINTS
#include <trace/events/cpuhp.h>

#include "smpboot.h"

/**
 * struct cpuhp_cpu_state - 每个 CPU 的热插拔状态存储
 *
 * 每个 CPU 都有一个独立的此结构体实例（per-cpu 变量），
 * 记录该 CPU 当前在状态机中的位置和运行信息。
 *
 * @state:   CPU 当前所处的状态（已完成的最高状态）
 * @target:  CPU 正在向这个目标状态迁移
 * @fail:    用于测试：让指定状态的回调故意失败（-1 表示无）
 * @thread:  该 CPU 的热插拔线程（AP 侧状态机的执行者）
 * @should_run: 热插拔线程是否应该运行
 * @rollback:   当前是否正在执行回滚（上线失败时撤销已完成的步骤）
 * @single:  是否只执行单个回调（而非遍历整个状态范围）
 * @bringup: true=上线方向，false=下线方向
 * @node:    多实例状态：对单个实例执行 install/remove 时的节点
 * @last:    多实例回滚时记录已执行到哪个节点
 * @cb_state: 单个回调的状态（install/uninstall）
 * @result:  操作结果（AP 线程执行后写入，BP 读取）
 * @ap_sync_state: AP 与 BP 之间的同步状态（原子变量）
 * @done_up:   AP 上线完成时通知 BP 的完成量
 * @done_down: AP 下线完成时通知 BP 的完成量
 */
struct cpuhp_cpu_state {
	enum cpuhp_state	state;   /* 当前状态，状态机从这里继续推进 */
	enum cpuhp_state	target;  /* 目标状态，到达后停止 */
	enum cpuhp_state	fail;    /* 测试用：让这个状态故意失败 */
#ifdef CONFIG_SMP
	struct task_struct	*thread;    /* 此 CPU 的热插拔内核线程 */
	bool			should_run;     /* 通知线程开始执行 */
	bool			rollback;       /* 正在回滚，避免重复反转方向 */
	bool			single;         /* 只跑一个回调而非整个范围 */
	bool			bringup;        /* 方向：true=上线，false=下线 */
	struct hlist_node	*node;      /* 多实例：当前处理的节点 */
	struct hlist_node	*last;      /* 多实例回滚：记录进度 */
	enum cpuhp_state	cb_state;   /* 单回调模式下的当前状态 */
	int			result;             /* AP 执行结果，BP 来读 */
	atomic_t		ap_sync_state;  /* AP/BP 同步状态机 */
	struct completion	done_up;    /* AP 上线完成信号 */
	struct completion	done_down;  /* AP 下线完成信号 */
#endif
};

static DEFINE_PER_CPU(struct cpuhp_cpu_state, cpuhp_state) = {
	.fail = CPUHP_INVALID,
};

#ifdef CONFIG_SMP
cpumask_t cpus_booted_once_mask;
#endif

#if defined(CONFIG_LOCKDEP) && defined(CONFIG_SMP)
static struct lockdep_map cpuhp_state_up_map =
	STATIC_LOCKDEP_MAP_INIT("cpuhp_state-up", &cpuhp_state_up_map);
static struct lockdep_map cpuhp_state_down_map =
	STATIC_LOCKDEP_MAP_INIT("cpuhp_state-down", &cpuhp_state_down_map);


static inline void cpuhp_lock_acquire(bool bringup)
{
	lock_map_acquire(bringup ? &cpuhp_state_up_map : &cpuhp_state_down_map);
}

static inline void cpuhp_lock_release(bool bringup)
{
	lock_map_release(bringup ? &cpuhp_state_up_map : &cpuhp_state_down_map);
}
#else

static inline void cpuhp_lock_acquire(bool bringup) { }
static inline void cpuhp_lock_release(bool bringup) { }

#endif

/**
 * struct cpuhp_step - 热插拔状态机的一个步骤
 *
 * 状态机的每个状态（enum cpuhp_state 的每个值）对应一个 cpuhp_step。
 * 驱动/子系统通过 cpuhp_setup_state() 把自己的回调注册到某个状态。
 *
 * 类比：就像流水线上的一道工序，每个工序有"开工"和"收工"两个动作。
 *
 * @name:         步骤名称（调试和 /sys 接口使用）
 * @startup:      CPU 上线时的回调函数
 *   .single:     单实例回调（大多数驱动用这个）
 *   .multi:      多实例回调（同一状态有多个注册者，逐个调用）
 * @teardown:     CPU 下线时的回调函数（与 startup 对称）
 * @list:         多实例链表头（private，框架内部管理）
 * @cant_stop:    此步骤不可中断（如果在此步骤失败，无法回滚）
 * @multi_instance: 是否为多实例状态（允许多个驱动注册到同一状态）
 */
struct cpuhp_step {
	const char		*name;
	union {
		int		(*single)(unsigned int cpu);     /* 单实例回调 */
		int		(*multi)(unsigned int cpu,
					 struct hlist_node *node); /* 多实例回调 */
	} startup;
	union {
		int		(*single)(unsigned int cpu);
		int		(*multi)(unsigned int cpu,
					 struct hlist_node *node);
	} teardown;
	/* private: 框架内部使用，不对外 */
	struct hlist_head	list;   /* 多实例注册者链表 */
	/* public: */
	bool			cant_stop;        /* 此步骤不可停止/回滚 */
	bool			multi_instance;   /* 允许多实例注册 */
};

static DEFINE_MUTEX(cpuhp_state_mutex);
static struct cpuhp_step cpuhp_hp_states[];

static struct cpuhp_step *cpuhp_get_step(enum cpuhp_state state)
{
	return cpuhp_hp_states + state;
}

static bool cpuhp_step_empty(bool bringup, struct cpuhp_step *step)
{
	return bringup ? !step->startup.single : !step->teardown.single;
}

/**
 * cpuhp_invoke_callback - Invoke the callbacks for a given state
 * @cpu:	The cpu for which the callback should be invoked
 * @state:	The state to do callbacks for
 * @bringup:	True if the bringup callback should be invoked
 * @node:	For multi-instance, do a single entry callback for install/remove
 * @lastp:	For multi-instance rollback, remember how far we got
 *
 * Called from cpu hotplug and from the state register machinery.
 *
 * Return: %0 on success or a negative errno code
 */
static int cpuhp_invoke_callback(unsigned int cpu, enum cpuhp_state state,
				 bool bringup, struct hlist_node *node,
				 struct hlist_node **lastp)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	struct cpuhp_step *step = cpuhp_get_step(state);
	int (*cbm)(unsigned int cpu, struct hlist_node *node);
	int (*cb)(unsigned int cpu);
	int ret, cnt, rollback_ret;

	if (st->fail == state) {
		st->fail = CPUHP_INVALID;
		return -EAGAIN;
	}

	if (cpuhp_step_empty(bringup, step)) {
		WARN_ON_ONCE(1);
		return 0;
	}

	if (!step->multi_instance) {
		WARN_ON_ONCE(lastp && *lastp);
		cb = bringup ? step->startup.single : step->teardown.single;

		trace_cpuhp_enter(cpu, st->target, state, cb);
		ret = cb(cpu);
		trace_cpuhp_exit(cpu, st->state, state, ret);
		return ret;
	}
	cbm = bringup ? step->startup.multi : step->teardown.multi;

	/* Single invocation for instance add/remove */
	if (node) {
		WARN_ON_ONCE(lastp && *lastp);
		trace_cpuhp_multi_enter(cpu, st->target, state, cbm, node);
		ret = cbm(cpu, node);
		trace_cpuhp_exit(cpu, st->state, state, ret);
		return ret;
	}

	/* State transition. Invoke on all instances */
	cnt = 0;
	hlist_for_each(node, &step->list) {
		if (lastp && node == *lastp)
			break;

		trace_cpuhp_multi_enter(cpu, st->target, state, cbm, node);
		ret = cbm(cpu, node);
		trace_cpuhp_exit(cpu, st->state, state, ret);
		if (ret) {
			if (!lastp)
				goto err;

			*lastp = node;
			return ret;
		}
		cnt++;
	}
	if (lastp)
		*lastp = NULL;
	return 0;
err:
	/* Rollback the instances if one failed */
	cbm = !bringup ? step->startup.multi : step->teardown.multi;
	if (!cbm)
		return ret;

	hlist_for_each(node, &step->list) {
		if (!cnt--)
			break;

		trace_cpuhp_multi_enter(cpu, st->target, state, cbm, node);
		rollback_ret = cbm(cpu, node);
		trace_cpuhp_exit(cpu, st->state, state, rollback_ret);
		/*
		 * Rollback must not fail,
		 */
		WARN_ON_ONCE(rollback_ret);
	}
	return ret;
}

/*
 * The former STARTING/DYING states, ran with IRQs disabled and must not fail.
 */
static bool cpuhp_is_atomic_state(enum cpuhp_state state)
{
	return CPUHP_AP_IDLE_DEAD <= state && state < CPUHP_AP_ONLINE;
}

#ifdef CONFIG_SMP
static bool cpuhp_is_ap_state(enum cpuhp_state state)
{
	/*
	 * The extra check for CPUHP_TEARDOWN_CPU is only for documentation
	 * purposes as that state is handled explicitly in cpu_down.
	 */
	return state > CPUHP_BRINGUP_CPU && state != CPUHP_TEARDOWN_CPU;
}

static inline void wait_for_ap_thread(struct cpuhp_cpu_state *st, bool bringup)
{
	struct completion *done = bringup ? &st->done_up : &st->done_down;
	wait_for_completion(done);
}

static inline void complete_ap_thread(struct cpuhp_cpu_state *st, bool bringup)
{
	struct completion *done = bringup ? &st->done_up : &st->done_down;
	complete(done);
}

/*
 * AP 与 BP 之间的同步状态机。
 *
 * 用于解决一个问题：AP 在低级启动代码中（head.S / secondary_startup）
 * 无法使用 completion 等内核同步原语（太早了，内核数据结构还未初始化），
 * 所以用原子变量来握手。
 *
 * 上线时的状态转换：
 *   DEAD/KICKED → AP 接收到 IPI 开始启动
 *   KICKED → ALIVE：AP 完成低级初始化，通知 BP "我活了"
 *   ALIVE → SHOULD_ONLINE：BP 收到通知，回复 "你可以继续上线了"
 *   SHOULD_ONLINE → ONLINE：AP 完成上线
 *
 * 下线时的状态转换：
 *   ONLINE → SHOULD_DIE：BP 通知 AP "你可以下线了"
 *   SHOULD_DIE → DEAD：AP 完成下线，通知 BP "我死了"
 */
enum cpuhp_sync_state {
	SYNC_STATE_DEAD,          /* CPU 已完全停止 */
	SYNC_STATE_KICKED,        /* CPU 已收到 IPI，正在启动低级代码 */
	SYNC_STATE_SHOULD_DIE,    /* BP 通知 AP 可以执行下线了 */
	SYNC_STATE_ALIVE,         /* AP 完成低级初始化，等待 BP 放行 */
	SYNC_STATE_SHOULD_ONLINE, /* BP 放行，AP 可以继续上线 */
	SYNC_STATE_ONLINE,        /* AP 完全上线 */
};

#ifdef CONFIG_HOTPLUG_CORE_SYNC
/**
 * cpuhp_ap_update_sync_state - Update synchronization state during bringup/teardown
 * @state:	The synchronization state to set
 *
 * No synchronization point. Just update of the synchronization state, but implies
 * a full barrier so that the AP changes are visible before the control CPU proceeds.
 */
static inline void cpuhp_ap_update_sync_state(enum cpuhp_sync_state state)
{
	atomic_t *st = this_cpu_ptr(&cpuhp_state.ap_sync_state);

	(void)atomic_xchg(st, state);
}

void __weak arch_cpuhp_sync_state_poll(void) { cpu_relax(); }

static bool cpuhp_wait_for_sync_state(unsigned int cpu, enum cpuhp_sync_state state,
				      enum cpuhp_sync_state next_state)
{
	atomic_t *st = per_cpu_ptr(&cpuhp_state.ap_sync_state, cpu);
	ktime_t now, end, start = ktime_get();
	int sync;

	end = start + 10ULL * NSEC_PER_SEC;

	sync = atomic_read(st);
	while (1) {
		if (sync == state) {
			if (!atomic_try_cmpxchg(st, &sync, next_state))
				continue;
			return true;
		}

		now = ktime_get();
		if (now > end) {
			/* Timeout. Leave the state unchanged */
			return false;
		} else if (now - start < NSEC_PER_MSEC) {
			/* Poll for one millisecond */
			arch_cpuhp_sync_state_poll();
		} else {
			usleep_range(USEC_PER_MSEC, 2 * USEC_PER_MSEC);
		}
		sync = atomic_read(st);
	}
	return true;
}
#else  /* CONFIG_HOTPLUG_CORE_SYNC */
static inline void cpuhp_ap_update_sync_state(enum cpuhp_sync_state state) { }
#endif /* !CONFIG_HOTPLUG_CORE_SYNC */

#ifdef CONFIG_HOTPLUG_CORE_SYNC_DEAD
/**
 * cpuhp_ap_report_dead - Update synchronization state to DEAD
 *
 * No synchronization point. Just update of the synchronization state.
 */
void cpuhp_ap_report_dead(void)
{
	cpuhp_ap_update_sync_state(SYNC_STATE_DEAD);
}

void __weak arch_cpuhp_cleanup_dead_cpu(unsigned int cpu) { }

/*
 * Late CPU shutdown synchronization point. Cannot use cpuhp_state::done_down
 * because the AP cannot issue complete() at this stage.
 */
static void cpuhp_bp_sync_dead(unsigned int cpu)
{
	atomic_t *st = per_cpu_ptr(&cpuhp_state.ap_sync_state, cpu);
	int sync = atomic_read(st);

	do {
		/* CPU can have reported dead already. Don't overwrite that! */
		if (sync == SYNC_STATE_DEAD)
			break;
	} while (!atomic_try_cmpxchg(st, &sync, SYNC_STATE_SHOULD_DIE));

	if (cpuhp_wait_for_sync_state(cpu, SYNC_STATE_DEAD, SYNC_STATE_DEAD)) {
		/* CPU reached dead state. Invoke the cleanup function */
		arch_cpuhp_cleanup_dead_cpu(cpu);
		return;
	}

	/* No further action possible. Emit message and give up. */
	pr_err("CPU%u failed to report dead state\n", cpu);
}
#else /* CONFIG_HOTPLUG_CORE_SYNC_DEAD */
static inline void cpuhp_bp_sync_dead(unsigned int cpu) { }
#endif /* !CONFIG_HOTPLUG_CORE_SYNC_DEAD */

#ifdef CONFIG_HOTPLUG_CORE_SYNC_FULL
/**
 * cpuhp_ap_sync_alive - Synchronize AP with the control CPU once it is alive
 *
 * Updates the AP synchronization state to SYNC_STATE_ALIVE and waits
 * for the BP to release it.
 */
void cpuhp_ap_sync_alive(void)
{
	atomic_t *st = this_cpu_ptr(&cpuhp_state.ap_sync_state);

	cpuhp_ap_update_sync_state(SYNC_STATE_ALIVE);

	/* Wait for the control CPU to release it. */
	while (atomic_read(st) != SYNC_STATE_SHOULD_ONLINE)
		cpu_relax();
}

static bool cpuhp_can_boot_ap(unsigned int cpu)
{
	atomic_t *st = per_cpu_ptr(&cpuhp_state.ap_sync_state, cpu);
	int sync = atomic_read(st);

again:
	switch (sync) {
	case SYNC_STATE_DEAD:
		/* CPU is properly dead */
		break;
	case SYNC_STATE_KICKED:
		/* CPU did not come up in previous attempt */
		break;
	case SYNC_STATE_ALIVE:
		/* CPU is stuck cpuhp_ap_sync_alive(). */
		break;
	default:
		/* CPU failed to report online or dead and is in limbo state. */
		return false;
	}

	/* Prepare for booting */
	if (!atomic_try_cmpxchg(st, &sync, SYNC_STATE_KICKED))
		goto again;

	return true;
}

void __weak arch_cpuhp_cleanup_kick_cpu(unsigned int cpu) { }

/*
 * Early CPU bringup synchronization point. Cannot use cpuhp_state::done_up
 * because the AP cannot issue complete() so early in the bringup.
 */
static int cpuhp_bp_sync_alive(unsigned int cpu)
{
	int ret = 0;

	if (!IS_ENABLED(CONFIG_HOTPLUG_CORE_SYNC_FULL))
		return 0;

	if (!cpuhp_wait_for_sync_state(cpu, SYNC_STATE_ALIVE, SYNC_STATE_SHOULD_ONLINE)) {
		pr_err("CPU%u failed to report alive state\n", cpu);
		ret = -EIO;
	}

	/* Let the architecture cleanup the kick alive mechanics. */
	arch_cpuhp_cleanup_kick_cpu(cpu);
	return ret;
}
#else /* CONFIG_HOTPLUG_CORE_SYNC_FULL */
static inline int cpuhp_bp_sync_alive(unsigned int cpu) { return 0; }
static inline bool cpuhp_can_boot_ap(unsigned int cpu) { return true; }
#endif /* !CONFIG_HOTPLUG_CORE_SYNC_FULL */

/*
 * cpu_add_remove_lock：序列化 cpu_online_mask 和 cpu_present_mask 的更新。
 *
 * cpu_online_mask：当前在线（可运行任务）的 CPU 集合
 * cpu_present_mask：存在（已识别）但不一定在线的 CPU 集合
 *
 * 这两个 mask 是内核调度和负载均衡的基础，必须在持锁时才能修改，
 * 否则并发的 CPU 上下线可能导致 mask 不一致。
 */
static DEFINE_MUTEX(cpu_add_remove_lock);

/* 系统当前是否处于任务冻结状态（休眠/挂起时置 true） */
bool cpuhp_tasks_frozen;
EXPORT_SYMBOL_GPL(cpuhp_tasks_frozen);

/*
 * cpu_maps_update_begin/done：更新 CPU mask 前后必须调用的锁对。
 * 所有修改 cpu_online_mask / cpu_present_mask 的代码都要用这对接口。
 */
void cpu_maps_update_begin(void)
{
	mutex_lock(&cpu_add_remove_lock);
}

void cpu_maps_update_done(void)
{
	mutex_unlock(&cpu_add_remove_lock);
}

/*
 * cpu_hotplug_disabled：计数器，非零时 cpu_up/cpu_down 返回 -EBUSY。
 * 用于系统挂起、关机等需要冻结 CPU 拓扑的场景。
 * 必须在 cpu_add_remove_lock 保护下修改。
 */
static int cpu_hotplug_disabled;

#ifdef CONFIG_HOTPLUG_CPU

DEFINE_STATIC_PERCPU_RWSEM(cpu_hotplug_lock);

static bool cpu_hotplug_offline_disabled __ro_after_init;

void cpus_read_lock(void)
{
	percpu_down_read(&cpu_hotplug_lock);
}
EXPORT_SYMBOL_GPL(cpus_read_lock);

int cpus_read_trylock(void)
{
	return percpu_down_read_trylock(&cpu_hotplug_lock);
}
EXPORT_SYMBOL_GPL(cpus_read_trylock);

void cpus_read_unlock(void)
{
	percpu_up_read(&cpu_hotplug_lock);
}
EXPORT_SYMBOL_GPL(cpus_read_unlock);

void cpus_write_lock(void)
{
	percpu_down_write(&cpu_hotplug_lock);
}

void cpus_write_unlock(void)
{
	percpu_up_write(&cpu_hotplug_lock);
}

void lockdep_assert_cpus_held(void)
{
	/*
	 * We can't have hotplug operations before userspace starts running,
	 * and some init codepaths will knowingly not take the hotplug lock.
	 * This is all valid, so mute lockdep until it makes sense to report
	 * unheld locks.
	 */
	if (system_state < SYSTEM_RUNNING)
		return;

	percpu_rwsem_assert_held(&cpu_hotplug_lock);
}
EXPORT_SYMBOL_GPL(lockdep_assert_cpus_held);

#ifdef CONFIG_LOCKDEP
int lockdep_is_cpus_held(void)
{
	return percpu_rwsem_is_held(&cpu_hotplug_lock);
}

int lockdep_is_cpus_write_held(void)
{
	return percpu_rwsem_is_write_held(&cpu_hotplug_lock);
}
#endif

static void lockdep_acquire_cpus_lock(void)
{
	rwsem_acquire(&cpu_hotplug_lock.dep_map, 0, 0, _THIS_IP_);
}

static void lockdep_release_cpus_lock(void)
{
	rwsem_release(&cpu_hotplug_lock.dep_map, _THIS_IP_);
}

/* Declare CPU offlining not supported */
void cpu_hotplug_disable_offlining(void)
{
	cpu_maps_update_begin();
	cpu_hotplug_offline_disabled = true;
	cpu_maps_update_done();
}

/*
 * Wait for currently running CPU hotplug operations to complete (if any) and
 * disable future CPU hotplug (from sysfs). The 'cpu_add_remove_lock' protects
 * the 'cpu_hotplug_disabled' flag. The same lock is also acquired by the
 * hotplug path before performing hotplug operations. So acquiring that lock
 * guarantees mutual exclusion from any currently running hotplug operations.
 */
void cpu_hotplug_disable(void)
{
	cpu_maps_update_begin();
	cpu_hotplug_disabled++;
	cpu_maps_update_done();
}
EXPORT_SYMBOL_GPL(cpu_hotplug_disable);

static void __cpu_hotplug_enable(void)
{
	if (WARN_ONCE(!cpu_hotplug_disabled, "Unbalanced cpu hotplug enable\n"))
		return;
	cpu_hotplug_disabled--;
}

void cpu_hotplug_enable(void)
{
	cpu_maps_update_begin();
	__cpu_hotplug_enable();
	cpu_maps_update_done();
}
EXPORT_SYMBOL_GPL(cpu_hotplug_enable);

#else

static void lockdep_acquire_cpus_lock(void)
{
}

static void lockdep_release_cpus_lock(void)
{
}

#endif	/* CONFIG_HOTPLUG_CPU */

/*
 * Architectures that need SMT-specific errata handling during SMT hotplug
 * should override this.
 */
void __weak arch_smt_update(void) { }

#ifdef CONFIG_HOTPLUG_SMT

enum cpuhp_smt_control cpu_smt_control __read_mostly = CPU_SMT_ENABLED;
static unsigned int cpu_smt_max_threads __ro_after_init;
unsigned int cpu_smt_num_threads __read_mostly = UINT_MAX;

void __init cpu_smt_disable(bool force)
{
	if (!cpu_smt_possible())
		return;

	if (force) {
		pr_info("SMT: Force disabled\n");
		cpu_smt_control = CPU_SMT_FORCE_DISABLED;
	} else {
		pr_info("SMT: disabled\n");
		cpu_smt_control = CPU_SMT_DISABLED;
	}
	cpu_smt_num_threads = 1;
}

/*
 * The decision whether SMT is supported can only be done after the full
 * CPU identification. Called from architecture code.
 */
void __init cpu_smt_set_num_threads(unsigned int num_threads,
				    unsigned int max_threads)
{
	WARN_ON(!num_threads || (num_threads > max_threads));

	if (max_threads == 1)
		cpu_smt_control = CPU_SMT_NOT_SUPPORTED;

	cpu_smt_max_threads = max_threads;

	/*
	 * If SMT has been disabled via the kernel command line or SMT is
	 * not supported, set cpu_smt_num_threads to 1 for consistency.
	 * If enabled, take the architecture requested number of threads
	 * to bring up into account.
	 */
	if (cpu_smt_control != CPU_SMT_ENABLED)
		cpu_smt_num_threads = 1;
	else if (num_threads < cpu_smt_num_threads)
		cpu_smt_num_threads = num_threads;
}

static int __init smt_cmdline_disable(char *str)
{
	cpu_smt_disable(str && !strcmp(str, "force"));
	return 0;
}
early_param("nosmt", smt_cmdline_disable);

/*
 * For Archicture supporting partial SMT states check if the thread is allowed.
 * Otherwise this has already been checked through cpu_smt_max_threads when
 * setting the SMT level.
 */
static inline bool cpu_smt_thread_allowed(unsigned int cpu)
{
#ifdef CONFIG_SMT_NUM_THREADS_DYNAMIC
	return topology_smt_thread_allowed(cpu);
#else
	return true;
#endif
}

static inline bool cpu_bootable(unsigned int cpu)
{
	if (cpu_smt_control == CPU_SMT_ENABLED && cpu_smt_thread_allowed(cpu))
		return true;

	/* All CPUs are bootable if controls are not configured */
	if (cpu_smt_control == CPU_SMT_NOT_IMPLEMENTED)
		return true;

	/* All CPUs are bootable if CPU is not SMT capable */
	if (cpu_smt_control == CPU_SMT_NOT_SUPPORTED)
		return true;

	if (topology_is_primary_thread(cpu))
		return true;

	/*
	 * On x86 it's required to boot all logical CPUs at least once so
	 * that the init code can get a chance to set CR4.MCE on each
	 * CPU. Otherwise, a broadcasted MCE observing CR4.MCE=0b on any
	 * core will shutdown the machine.
	 */
	return !cpumask_test_cpu(cpu, &cpus_booted_once_mask);
}

/* Returns true if SMT is supported and not forcefully (irreversibly) disabled */
bool cpu_smt_possible(void)
{
	return cpu_smt_control != CPU_SMT_FORCE_DISABLED &&
		cpu_smt_control != CPU_SMT_NOT_SUPPORTED;
}
EXPORT_SYMBOL_GPL(cpu_smt_possible);

#else
static inline bool cpu_bootable(unsigned int cpu) { return true; }
#endif

static inline enum cpuhp_state
cpuhp_set_state(int cpu, struct cpuhp_cpu_state *st, enum cpuhp_state target)
{
	enum cpuhp_state prev_state = st->state;
	bool bringup = st->state < target;

	st->rollback = false;
	st->last = NULL;

	st->target = target;
	st->single = false;
	st->bringup = bringup;
	if (cpu_dying(cpu) != !bringup)
		set_cpu_dying(cpu, !bringup);

	return prev_state;
}

static inline void
cpuhp_reset_state(int cpu, struct cpuhp_cpu_state *st,
		  enum cpuhp_state prev_state)
{
	bool bringup = !st->bringup;

	st->target = prev_state;

	/*
	 * Already rolling back. No need invert the bringup value or to change
	 * the current state.
	 */
	if (st->rollback)
		return;

	st->rollback = true;

	/*
	 * If we have st->last we need to undo partial multi_instance of this
	 * state first. Otherwise start undo at the previous state.
	 */
	if (!st->last) {
		if (st->bringup)
			st->state--;
		else
			st->state++;
	}

	st->bringup = bringup;
	if (cpu_dying(cpu) != !bringup)
		set_cpu_dying(cpu, !bringup);
}

/*
 * __cpuhp_kick_ap()：唤醒 AP 的热插拔线程，让它继续推进状态机。
 *
 * BP 侧调用，流程：
 *   1. 设置 should_run = true（通知 AP 线程有工作要做）
 *   2. 唤醒 AP 的热插拔线程
 *   3. 等待 AP 完成（阻塞在 done_up 或 done_down 完成量上）
 *
 * smp_mb()：全内存屏障。
 * 必须确保 result=0 和其他准备工作在 should_run=true 之前对 AP 可见，
 * 否则 AP 看到 should_run=true 时可能读到旧的 result 值。
 * 与 cpuhp_thread_fun() 开头的 mb() 配对使用（两侧各一个屏障）。
 */
static void __cpuhp_kick_ap(struct cpuhp_cpu_state *st)
{
	/* 已经在目标状态且不是单次调用，无需做任何事 */
	if (!st->single && st->state == st->target)
		return;

	st->result = 0;
	/*
	 * 确保上面的写操作对 AP 可见，然后再设置 should_run。
	 * 与 cpuhp_thread_fun() 中的 mb() 构成一对 acquire/release 屏障。
	 */
	smp_mb();
	st->should_run = true;
	wake_up_process(st->thread);          /* 唤醒 AP 的热插拔线程 */
	wait_for_ap_thread(st, st->bringup);  /* 阻塞等待 AP 完成 */
}

static int cpuhp_kick_ap(int cpu, struct cpuhp_cpu_state *st,
			 enum cpuhp_state target)
{
	enum cpuhp_state prev_state;
	int ret;

	prev_state = cpuhp_set_state(cpu, st, target);
	__cpuhp_kick_ap(st);
	if ((ret = st->result)) {
		cpuhp_reset_state(cpu, st, prev_state);
		__cpuhp_kick_ap(st);
	}

	return ret;
}

static int bringup_wait_for_ap_online(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);

	/* Wait for the CPU to reach CPUHP_AP_ONLINE_IDLE */
	wait_for_ap_thread(st, true);
	if (WARN_ON_ONCE((!cpu_online(cpu))))
		return -ECANCELED;

	/* Unpark the hotplug thread of the target cpu */
	kthread_unpark(st->thread);

	/*
	 * SMT soft disabling on X86 requires to bring the CPU out of the
	 * BIOS 'wait for SIPI' state in order to set the CR4.MCE bit.  The
	 * CPU marked itself as booted_once in notify_cpu_starting() so the
	 * cpu_bootable() check will now return false if this is not the
	 * primary sibling.
	 */
	if (!cpu_bootable(cpu))
		return -ECANCELED;
	return 0;
}

#ifdef CONFIG_HOTPLUG_SPLIT_STARTUP
static int cpuhp_kick_ap_alive(unsigned int cpu)
{
	if (!cpuhp_can_boot_ap(cpu))
		return -EAGAIN;

	return arch_cpuhp_kick_ap_alive(cpu, idle_thread_get(cpu));
}

static int cpuhp_bringup_ap(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	int ret;

	/*
	 * Some architectures have to walk the irq descriptors to
	 * setup the vector space for the cpu which comes online.
	 * Prevent irq alloc/free across the bringup.
	 */
	irq_lock_sparse();

	ret = cpuhp_bp_sync_alive(cpu);
	if (ret)
		goto out_unlock;

	ret = bringup_wait_for_ap_online(cpu);
	if (ret)
		goto out_unlock;

	irq_unlock_sparse();

	if (st->target <= CPUHP_AP_ONLINE_IDLE)
		return 0;

	return cpuhp_kick_ap(cpu, st, st->target);

out_unlock:
	irq_unlock_sparse();
	return ret;
}
#else
static int bringup_cpu(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	struct task_struct *idle = idle_thread_get(cpu);
	int ret;

	if (!cpuhp_can_boot_ap(cpu))
		return -EAGAIN;

	/*
	 * Some architectures have to walk the irq descriptors to
	 * setup the vector space for the cpu which comes online.
	 *
	 * Prevent irq alloc/free across the bringup by acquiring the
	 * sparse irq lock. Hold it until the upcoming CPU completes the
	 * startup in cpuhp_online_idle() which allows to avoid
	 * intermediate synchronization points in the architecture code.
	 */
	irq_lock_sparse();

	ret = __cpu_up(cpu, idle);
	if (ret)
		goto out_unlock;

	ret = cpuhp_bp_sync_alive(cpu);
	if (ret)
		goto out_unlock;

	ret = bringup_wait_for_ap_online(cpu);
	if (ret)
		goto out_unlock;

	irq_unlock_sparse();

	if (st->target <= CPUHP_AP_ONLINE_IDLE)
		return 0;

	return cpuhp_kick_ap(cpu, st, st->target);

out_unlock:
	irq_unlock_sparse();
	return ret;
}
#endif

static int finish_cpu(unsigned int cpu)
{
	struct task_struct *idle = idle_thread_get(cpu);
	struct mm_struct *mm = idle->active_mm;

	/*
	 * sched_force_init_mm() ensured the use of &init_mm,
	 * drop that refcount now that the CPU has stopped.
	 */
	WARN_ON(mm != &init_mm);
	idle->active_mm = NULL;
	mmdrop_lazy_tlb(mm);

	return 0;
}

/*
 * ══════════════════════════════════════════════════════════════
 * 热插拔状态机核心函数
 * ══════════════════════════════════════════════════════════════
 */

/*
 * cpuhp_next_state()：找到状态机下一个需要执行回调的状态。
 *
 * 空状态（没有注册回调）会被跳过，只返回有回调的状态。
 * st->state 会提前更新（"先修改再执行"），这样如果回调失败，
 * st->state 已经指向出错的那个状态，方便回滚时从此处开始。
 *
 * 返回 true 表示找到了需要执行的状态，state_to_run 被更新。
 * 返回 false 表示已到达 target，不需要再执行任何状态。
 */
static bool cpuhp_next_state(bool bringup,
			     enum cpuhp_state *state_to_run,
			     struct cpuhp_cpu_state *st,
			     enum cpuhp_state target)
{
	do {
		if (bringup) {
			if (st->state >= target)
				return false;

			*state_to_run = ++st->state;
		} else {
			if (st->state <= target)
				return false;

			*state_to_run = st->state--;
		}

		if (!cpuhp_step_empty(bringup, cpuhp_get_step(*state_to_run)))
			break;
	} while (true);

	return true;
}

/*
 * __cpuhp_invoke_callback_range()：在一个状态范围内依次执行所有回调。
 *
 * nofail=false（普通模式）：遇到失败立即停止，返回错误码。
 * nofail=true（强制模式）：即使失败也继续执行，记录错误但不停止。
 *   用于下线路径中 DYING 阶段：CPU 必须完成下线，不能半途而废。
 */
static int __cpuhp_invoke_callback_range(bool bringup,
					 unsigned int cpu,
					 struct cpuhp_cpu_state *st,
					 enum cpuhp_state target,
					 bool nofail)
{
	enum cpuhp_state state;
	int ret = 0;

	while (cpuhp_next_state(bringup, &state, st, target)) {
		int err;

		err = cpuhp_invoke_callback(cpu, state, bringup, NULL, NULL);
		if (!err)
			continue;

		if (nofail) {
			pr_warn("CPU %u %s state %s (%d) failed (%d)\n",
				cpu, bringup ? "UP" : "DOWN",
				cpuhp_get_step(st->state)->name,
				st->state, err);
			ret = -1;
		} else {
			ret = err;
			break;
		}
	}

	return ret;
}

static inline int cpuhp_invoke_callback_range(bool bringup,
					      unsigned int cpu,
					      struct cpuhp_cpu_state *st,
					      enum cpuhp_state target)
{
	return __cpuhp_invoke_callback_range(bringup, cpu, st, target, false);
}

static inline void cpuhp_invoke_callback_range_nofail(bool bringup,
						      unsigned int cpu,
						      struct cpuhp_cpu_state *st,
						      enum cpuhp_state target)
{
	__cpuhp_invoke_callback_range(bringup, cpu, st, target, true);
}

/*
 * can_rollback_cpu()：判断当前 CPU 是否可以执行回滚（撤销已完成的上线步骤）。
 *
 * CONFIG_HOTPLUG_CPU 未启用时，下线机制（takedown_cpu 等）不可用，
 * 无法把 CPU 降回 OFFLINE。此时只有还没真正启动 AP 的状态（<=CPUHP_BRINGUP_CPU）
 * 才能安全回滚（因为 AP 还没运行，直接放弃即可）。
 */
static inline bool can_rollback_cpu(struct cpuhp_cpu_state *st)
{
	if (IS_ENABLED(CONFIG_HOTPLUG_CPU))
		return true;
	/*
	 * 热插拔不支持时，AP 已经在运行的状态（>CPUHP_BRINGUP_CPU）无法回滚，
	 * 只能让 CPU 停留在当前状态。
	 */
	return st->state <= CPUHP_BRINGUP_CPU;
}

/*
 * cpuhp_up_callbacks()：执行 CPU 上线方向的回调，失败时自动回滚。
 *
 * 成功：CPU 状态推进到 target。
 * 失败：重置状态目标，执行 teardown 回调把 CPU 退回到 prev_state。
 */
static int cpuhp_up_callbacks(unsigned int cpu, struct cpuhp_cpu_state *st,
			      enum cpuhp_state target)
{
	enum cpuhp_state prev_state = st->state;
	int ret = 0;

	ret = cpuhp_invoke_callback_range(true, cpu, st, target);
	if (ret) {
		pr_debug("CPU UP failed (%d) CPU %u state %s (%d)\n",
			 ret, cpu, cpuhp_get_step(st->state)->name,
			 st->state);

		cpuhp_reset_state(cpu, st, prev_state);
		if (can_rollback_cpu(st))
			WARN_ON(cpuhp_invoke_callback_range(false, cpu, st,
							    prev_state));
	}
	return ret;
}

/*
 * ══════════════════════════════════════════════════════════════
 * AP 热插拔线程
 * ══════════════════════════════════════════════════════════════
 *
 * 每个 CPU 有一个专属的热插拔线程（"cpuhp/N"），运行在对应的 CPU 上。
 * AP 侧的状态推进（CPUHP_BRINGUP_CPU 以上的状态）由这个线程执行，
 * 因为这些回调必须在 AP 自身的 CPU 上运行。
 *
 * BP 通过 __cpuhp_kick_ap() 唤醒 AP 线程并等待其完成。
 */

/* cpuhp_should_run()：smpboot 框架用来判断热插拔线程是否有工作要做 */
static int cpuhp_should_run(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = this_cpu_ptr(&cpuhp_state);

	return st->should_run;
}

/*
 * cpuhp_thread_fun()：AP 热插拔线程的主函数，由 smpboot 框架调用。
 *
 * 在 AP 自己的 CPU 上执行，每次调用执行一个状态的回调。
 *
 * 三种工作模式：
 *   single：只执行 st->cb_state 对应的单个回调
 *           （用于运行时动态注册/注销状态时的回调）
 *   up：    ++st->state，执行新状态的 startup 回调
 *           重复直到 st->state == st->target
 *   down：  st->state--，执行当前状态的 teardown 回调
 *           重复直到 st->state == st->target
 *
 * 完成或出错时：清除 should_run，触发 done_up/done_down 完成量通知 BP。
 */
static void cpuhp_thread_fun(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = this_cpu_ptr(&cpuhp_state);
	bool bringup = st->bringup;
	enum cpuhp_state state;

	if (WARN_ON_ONCE(!st->should_run))
		return;

	/*
	 * ACQUIRE for the cpuhp_should_run() load of ->should_run. Ensures
	 * that if we see ->should_run we also see the rest of the state.
	 */
	smp_mb();

	/*
	 * The BP holds the hotplug lock, but we're now running on the AP,
	 * ensure that anybody asserting the lock is held, will actually find
	 * it so.
	 */
	lockdep_acquire_cpus_lock();
	cpuhp_lock_acquire(bringup);

	if (st->single) {
		state = st->cb_state;
		st->should_run = false;
	} else {
		st->should_run = cpuhp_next_state(bringup, &state, st, st->target);
		if (!st->should_run)
			goto end;
	}

	WARN_ON_ONCE(!cpuhp_is_ap_state(state));

	if (cpuhp_is_atomic_state(state)) {
		/*
		 * STARTING/DYING 阶段的回调必须在关中断下执行：
		 *   STARTING（上线）：CPU 刚上线，中断系统还未完全初始化
		 *   DYING（下线）：CPU 即将停止，必须确保不被中断打断
		 * 这两个阶段的回调绝对不允许失败。
		 */
		local_irq_disable();
		st->result = cpuhp_invoke_callback(cpu, state, bringup, st->node, &st->last);
		local_irq_enable();

		/* STARTING/DYING 回调失败是内核 bug，用 WARN_ON 标记 */
		WARN_ON_ONCE(st->result);
	} else {
		/* 普通状态：中断开启，允许睡眠等待 */
		st->result = cpuhp_invoke_callback(cpu, state, bringup, st->node, &st->last);
	}

	if (st->result) {
		/*
		 * If we fail on a rollback, we're up a creek without no
		 * paddle, no way forward, no way back. We loose, thanks for
		 * playing.
		 */
		WARN_ON_ONCE(st->rollback);
		st->should_run = false;
	}

end:
	cpuhp_lock_release(bringup);
	lockdep_release_cpus_lock();

	if (!st->should_run)
		complete_ap_thread(st, bringup);
}

/*
 * cpuhp_invoke_ap_callback()：在远端 CPU（AP）上执行单个状态的回调。
 *
 * 用于运行时动态注册状态时，需要对已在线的 CPU 补充执行 startup 回调。
 * 与 __cpuhp_kick_ap() 的区别：这里只执行单个特定状态的回调（single 模式），
 * 而不是推进到某个目标状态。
 *
 * 早期调用（热插拔线程还未就绪）时，直接在当前 CPU 上同步调用回调函数。
 */
static int
cpuhp_invoke_ap_callback(int cpu, enum cpuhp_state state, bool bringup,
			 struct hlist_node *node)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	int ret;

	if (!cpu_online(cpu))
		return 0;

	cpuhp_lock_acquire(false);
	cpuhp_lock_release(false);

	cpuhp_lock_acquire(true);
	cpuhp_lock_release(true);

	/*
	 * If we are up and running, use the hotplug thread. For early calls
	 * we invoke the thread function directly.
	 */
	if (!st->thread)
		return cpuhp_invoke_callback(cpu, state, bringup, node, NULL);

	st->rollback = false;
	st->last = NULL;

	st->node = node;
	st->bringup = bringup;
	st->cb_state = state;
	st->single = true;

	__cpuhp_kick_ap(st);

	/*
	 * If we failed and did a partial, do a rollback.
	 */
	if ((ret = st->result) && st->last) {
		st->rollback = true;
		st->bringup = !bringup;

		__cpuhp_kick_ap(st);
	}

	/*
	 * Clean up the leftovers so the next hotplug operation wont use stale
	 * data.
	 */
	st->node = st->last = NULL;
	return ret;
}

static int cpuhp_kick_ap_work(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	enum cpuhp_state prev_state = st->state;
	int ret;

	cpuhp_lock_acquire(false);
	cpuhp_lock_release(false);

	cpuhp_lock_acquire(true);
	cpuhp_lock_release(true);

	trace_cpuhp_enter(cpu, st->target, prev_state, cpuhp_kick_ap_work);
	ret = cpuhp_kick_ap(cpu, st, st->target);
	trace_cpuhp_exit(cpu, st->state, prev_state, ret);

	return ret;
}

/*
 * cpuhp_threads：热插拔线程的 smpboot 描述符。
 *
 * smpboot 框架根据这个描述符为每个 CPU 创建一个内核线程：
 *   .store：线程指针存放位置（per-cpu 的 cpuhp_state.thread）
 *   .thread_should_run：框架调用此函数判断是否需要运行
 *   .thread_fn：线程的主函数，每次被唤醒后调用一次
 *   .thread_comm："cpuhp/%u" → 线程名字如 "cpuhp/0", "cpuhp/1"
 *   .selfparking：线程自己管理 park/unpark（不由框架自动 park）
 */
static struct smp_hotplug_thread cpuhp_threads = {
	.store			= &cpuhp_state.thread,
	.thread_should_run	= cpuhp_should_run,
	.thread_fn		= cpuhp_thread_fun,
	.thread_comm		= "cpuhp/%u",
	.selfparking		= true,
};

/* cpuhp_init_state()：初始化所有 CPU 的热插拔完成量 */
static __init void cpuhp_init_state(void)
{
	struct cpuhp_cpu_state *st;
	int cpu;

	for_each_possible_cpu(cpu) {
		st = per_cpu_ptr(&cpuhp_state, cpu);
		init_completion(&st->done_up);
		init_completion(&st->done_down);
	}
}

/*
 * cpuhp_threads_init()：注册热插拔线程，由 start_kernel() 中的
 * smp_init() → ... 调用链触发，在 SMP 初始化早期执行。
 *
 * smpboot_register_percpu_thread()：为每个 possible CPU 创建热插拔线程，
 * 新创建的线程默认处于 parked（暂停）状态。
 * kthread_unpark()：解除 boot CPU 的热插拔线程的暂停，让它开始工作。
 */
void __init cpuhp_threads_init(void)
{
	cpuhp_init_state();
	BUG_ON(smpboot_register_percpu_thread(&cpuhp_threads));
	kthread_unpark(this_cpu_read(cpuhp_state.thread));
}

#ifdef CONFIG_HOTPLUG_CPU
#ifndef arch_clear_mm_cpumask_cpu
#define arch_clear_mm_cpumask_cpu(cpu, mm) cpumask_clear_cpu(cpu, mm_cpumask(mm))
#endif

/**
 * clear_tasks_mm_cpumask - Safely clear tasks' mm_cpumask for a CPU
 * @cpu: a CPU id
 *
 * This function walks all processes, finds a valid mm struct for each one and
 * then clears a corresponding bit in mm's cpumask.  While this all sounds
 * trivial, there are various non-obvious corner cases, which this function
 * tries to solve in a safe manner.
 *
 * Also note that the function uses a somewhat relaxed locking scheme, so it may
 * be called only for an already offlined CPU.
 */
void clear_tasks_mm_cpumask(int cpu)
{
	struct task_struct *p;

	/*
	 * This function is called after the cpu is taken down and marked
	 * offline, so its not like new tasks will ever get this cpu set in
	 * their mm mask. -- Peter Zijlstra
	 * Thus, we may use rcu_read_lock() here, instead of grabbing
	 * full-fledged tasklist_lock.
	 */
	WARN_ON(cpu_online(cpu));
	rcu_read_lock();
	for_each_process(p) {
		struct task_struct *t;

		/*
		 * Main thread might exit, but other threads may still have
		 * a valid mm. Find one.
		 */
		t = find_lock_task_mm(p);
		if (!t)
			continue;
		arch_clear_mm_cpumask_cpu(cpu, t->mm);
		task_unlock(t);
	}
	rcu_read_unlock();
}

/*
 * take_cpu_down()：在目标 CPU（AP）自身上执行下线操作。
 *
 * 通过 stop_machine() 调用，在目标 CPU 上运行，此时系统已停止（stop machine）。
 * 执行顺序：
 *   1. __cpu_disable()：通知架构层禁用此 CPU 的中断接收，迁移中断亲和性
 *   2. cpuhp_invoke_callback_range_nofail()：执行 DYING 阶段的 teardown 回调
 *   3. stop_machine_park()：暂停 stopper 线程
 *
 * 注意：DYING 回调绝对不允许失败（nofail），因为 CPU 必须完成下线流程。
 */
static int take_cpu_down(void *_param)
{
	struct cpuhp_cpu_state *st = this_cpu_ptr(&cpuhp_state);
	enum cpuhp_state target = max((int)st->target, CPUHP_AP_OFFLINE);
	int err, cpu = smp_processor_id();

	/* Ensure this CPU doesn't handle any more interrupts. */
	err = __cpu_disable();
	if (err < 0)
		return err;

	/*
	 * Must be called from CPUHP_TEARDOWN_CPU, which means, as we are going
	 * down, that the current state is CPUHP_TEARDOWN_CPU - 1.
	 */
	WARN_ON(st->state != (CPUHP_TEARDOWN_CPU - 1));

	/*
	 * Invoke the former CPU_DYING callbacks. DYING must not fail!
	 */
	cpuhp_invoke_callback_range_nofail(false, cpu, st, target);

	/* Park the stopper thread */
	stop_machine_park(cpu);
	return 0;
}

/*
 * takedown_cpu()：CPU 下线的核心步骤（在 CPUHP_TEARDOWN_CPU 状态执行）。
 *
 * 执行流程：
 *   1. 暂停热插拔线程（防止下线过程中被唤醒做其他事）
 *   2. 锁定 IRQ 稀疏矩阵（防止下线时中断亲和性被并发修改）
 *   3. 通过 stop_machine 在目标 CPU 上执行 take_cpu_down()
 *   4. 等待 AP 进入 IDLE_DEAD 状态（AP 确认自己即将停止）
 *   5. 调用 __cpu_die() 让硬件真正停止该 CPU
 *   6. 同步 DEAD 状态，迁移 RCU 回调
 *
 * stop_machine：全局暂停所有 CPU 的内核执行，在目标 CPU 上运行回调，
 * 然后恢复。用于需要安全地修改 per-cpu 状态的场景。
 */
static int takedown_cpu(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	int err;

	/* 暂停热插拔线程，下线过程中不需要它运行 */
	kthread_park(st->thread);

	/*
	 * Prevent irq alloc/free while the dying cpu reorganizes the
	 * interrupt affinities.
	 */
	irq_lock_sparse();

	err = stop_machine_cpuslocked(take_cpu_down, NULL, cpumask_of(cpu));
	if (err) {
		/* CPU refused to die */
		irq_unlock_sparse();
		/* Unpark the hotplug thread so we can rollback there */
		kthread_unpark(st->thread);
		return err;
	}
	BUG_ON(cpu_online(cpu));

	/*
	 * The teardown callback for CPUHP_AP_SCHED_STARTING will have removed
	 * all runnable tasks from the CPU, there's only the idle task left now
	 * that the migration thread is done doing the stop_machine thing.
	 *
	 * Wait for the stop thread to go away.
	 */
	wait_for_ap_thread(st, false);
	BUG_ON(st->state != CPUHP_AP_IDLE_DEAD);

	/* Interrupts are moved away from the dying cpu, reenable alloc/free */
	irq_unlock_sparse();

	hotplug_cpu__broadcast_tick_pull(cpu);
	/* This actually kills the CPU. */
	__cpu_die(cpu);

	cpuhp_bp_sync_dead(cpu);

	lockdep_cleanup_dead_cpu(cpu, idle_thread_get(cpu));

	/*
	 * Callbacks must be re-integrated right away to the RCU state machine.
	 * Otherwise an RCU callback could block a further teardown function
	 * waiting for its completion.
	 */
	rcutree_migrate_callbacks(cpu);

	return 0;
}

static void cpuhp_complete_idle_dead(void *arg)
{
	struct cpuhp_cpu_state *st = arg;

	complete_ap_thread(st, false);
}

/*
 * cpuhp_report_idle_dead()：AP 在停止前最后调用的函数，宣告自己已死。
 *
 * 调用时机：AP 即将进入最终的 idle 循环（即将停止执行），
 * 此时 AP 已经不能执行任何需要其他 CPU 配合的操作。
 *
 * 关键问题：不能在 rcutree_report_cpu_dead() 之后调用 complete()，
 * 因为 RCU 宣告死亡后，此 CPU 的 RCU 读者侧保护已经失效，
 * 而 complete() 内部会访问 RCU 保护的数据结构。
 *
 * 解决方案：把 complete() 委托给一个在线的 CPU 执行（smp_call_function_single）。
 * 这是 CPU 生命周期中的最后一次跨 CPU 通信。
 */
void cpuhp_report_idle_dead(void)
{
	struct cpuhp_cpu_state *st = this_cpu_ptr(&cpuhp_state);

	BUG_ON(st->state != CPUHP_AP_OFFLINE);
	/* 移交时间保持责任（时钟源必须在 CPU 死前交给其他 CPU） */
	tick_assert_timekeeping_handover();
	/* 通知 RCU 此 CPU 已死，RCU 从此不再等待此 CPU 的宽限期 */
	rcutree_report_cpu_dead();
	st->state = CPUHP_AP_IDLE_DEAD;
	/*
	 * rcutree_report_cpu_dead() 之后不能调用 complete()，
	 * 委托给第一个在线 CPU 代为完成通知。
	 */
	smp_call_function_single(cpumask_first(cpu_online_mask),
				 cpuhp_complete_idle_dead, st, 0);
}

static int cpuhp_down_callbacks(unsigned int cpu, struct cpuhp_cpu_state *st,
				enum cpuhp_state target)
{
	enum cpuhp_state prev_state = st->state;
	int ret = 0;

	ret = cpuhp_invoke_callback_range(false, cpu, st, target);
	if (ret) {
		pr_debug("CPU DOWN failed (%d) CPU %u state %s (%d)\n",
			 ret, cpu, cpuhp_get_step(st->state)->name,
			 st->state);

		cpuhp_reset_state(cpu, st, prev_state);

		if (st->state < prev_state)
			WARN_ON(cpuhp_invoke_callback_range(true, cpu, st,
							    prev_state));
	}

	return ret;
}

/*
 * _cpu_down()：CPU 下线的核心实现（对应 _cpu_up 的反向操作）。
 *
 * 下线流程分两段（与上线相反）：
 *   1. AP 的热插拔线程负责把 CPU 从 ONLINE 降到 CPUHP_TEARDOWN_CPU
 *   2. BP 负责执行 CPUHP_TEARDOWN_CPU 及以下的步骤（包括真正停止 CPU）
 *
 * 特殊保护：
 *   - 不允许最后一个 CPU 下线（系统至少需要一个 CPU 运行）
 *   - 必须保留至少一个 housekeeping CPU（保证调度域不为空）
 *
 * 调用前提：必须持有 cpu_add_remove_lock。
 */
static int __ref _cpu_down(unsigned int cpu, int tasks_frozen,
			   enum cpuhp_state target)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	int prev_state, ret = 0;

	if (num_online_cpus() == 1)
		return -EBUSY;

	if (!cpu_present(cpu))
		return -EINVAL;

	cpus_write_lock();

	/*
	 * Keep at least one housekeeping cpu onlined to avoid generating
	 * an empty sched_domain span.
	 */
	if (cpumask_any_and(cpu_online_mask,
			    housekeeping_cpumask(HK_TYPE_DOMAIN)) >= nr_cpu_ids) {
		ret = -EBUSY;
		goto out;
	}

	cpuhp_tasks_frozen = tasks_frozen;

	prev_state = cpuhp_set_state(cpu, st, target);
	/*
	 * If the current CPU state is in the range of the AP hotplug thread,
	 * then we need to kick the thread.
	 */
	if (st->state > CPUHP_TEARDOWN_CPU) {
		st->target = max((int)target, CPUHP_TEARDOWN_CPU);
		ret = cpuhp_kick_ap_work(cpu);
		/*
		 * The AP side has done the error rollback already. Just
		 * return the error code..
		 */
		if (ret)
			goto out;

		/*
		 * We might have stopped still in the range of the AP hotplug
		 * thread. Nothing to do anymore.
		 */
		if (st->state > CPUHP_TEARDOWN_CPU)
			goto out;

		st->target = target;
	}
	/*
	 * The AP brought itself down to CPUHP_TEARDOWN_CPU. So we need
	 * to do the further cleanups.
	 */
	ret = cpuhp_down_callbacks(cpu, st, target);
	if (ret && st->state < prev_state) {
		if (st->state == CPUHP_TEARDOWN_CPU) {
			cpuhp_reset_state(cpu, st, prev_state);
			__cpuhp_kick_ap(st);
		} else {
			WARN(1, "DEAD callback error for CPU%d", cpu);
		}
	}

out:
	cpus_write_unlock();
	arch_smt_update();
	return ret;
}

static int cpu_down_maps_locked(unsigned int cpu, enum cpuhp_state target)
{
	/*
	 * If the platform does not support hotplug, report it explicitly to
	 * differentiate it from a transient offlining failure.
	 */
	if (cpu_hotplug_offline_disabled)
		return -EOPNOTSUPP;
	if (cpu_hotplug_disabled)
		return -EBUSY;
	return _cpu_down(cpu, 0, target);
}

static int cpu_down(unsigned int cpu, enum cpuhp_state target)
{
	int err;

	cpu_maps_update_begin();
	err = cpu_down_maps_locked(cpu, target);
	cpu_maps_update_done();
	return err;
}

/**
 * cpu_device_down - Bring down a cpu device
 * @dev: Pointer to the cpu device to offline
 *
 * This function is meant to be used by device core cpu subsystem only.
 *
 * Other subsystems should use remove_cpu() instead.
 *
 * Return: %0 on success or a negative errno code
 */
int cpu_device_down(struct device *dev)
{
	return cpu_down(dev->id, CPUHP_OFFLINE);
}

int remove_cpu(unsigned int cpu)
{
	int ret;

	lock_device_hotplug();
	ret = device_offline(get_cpu_device(cpu));
	unlock_device_hotplug();

	return ret;
}
EXPORT_SYMBOL_GPL(remove_cpu);

void smp_shutdown_nonboot_cpus(unsigned int primary_cpu)
{
	unsigned int cpu;
	int error;

	cpu_maps_update_begin();

	/*
	 * Make certain the cpu I'm about to reboot on is online.
	 *
	 * This is inline to what migrate_to_reboot_cpu() already do.
	 */
	if (!cpu_online(primary_cpu))
		primary_cpu = cpumask_first(cpu_online_mask);

	for_each_online_cpu(cpu) {
		if (cpu == primary_cpu)
			continue;

		error = cpu_down_maps_locked(cpu, CPUHP_OFFLINE);
		if (error) {
			pr_err("Failed to offline CPU%d - error=%d",
				cpu, error);
			break;
		}
	}

	/*
	 * Ensure all but the reboot CPU are offline.
	 */
	BUG_ON(num_online_cpus() > 1);

	/*
	 * Make sure the CPUs won't be enabled by someone else after this
	 * point. Kexec will reboot to a new kernel shortly resetting
	 * everything along the way.
	 */
	cpu_hotplug_disabled++;

	cpu_maps_update_done();
}

#else
#define takedown_cpu		NULL
#endif /*CONFIG_HOTPLUG_CPU*/

/**
 * notify_cpu_starting(cpu) - Invoke the callbacks on the starting CPU
 * @cpu: cpu that just started
 *
 * It must be called by the arch code on the new cpu, before the new cpu
 * enables interrupts and before the "boot" cpu returns from __cpu_up().
 */
void notify_cpu_starting(unsigned int cpu)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	enum cpuhp_state target = min((int)st->target, CPUHP_AP_ONLINE);

	rcutree_report_cpu_starting(cpu);	/* Enables RCU usage on this CPU. */
	cpumask_set_cpu(cpu, &cpus_booted_once_mask);

	/*
	 * STARTING must not fail!
	 */
	cpuhp_invoke_callback_range_nofail(true, cpu, st, target);
}

/*
 * Called from the idle task. Wake up the controlling task which brings the
 * hotplug thread of the upcoming CPU up and then delegates the rest of the
 * online bringup to the hotplug thread.
 */
void cpuhp_online_idle(enum cpuhp_state state)
{
	struct cpuhp_cpu_state *st = this_cpu_ptr(&cpuhp_state);

	/* Happens for the boot cpu */
	if (state != CPUHP_AP_ONLINE_IDLE)
		return;

	cpuhp_ap_update_sync_state(SYNC_STATE_ONLINE);

	/*
	 * Unpark the stopper thread before we start the idle loop (and start
	 * scheduling); this ensures the stopper task is always available.
	 */
	stop_machine_unpark(smp_processor_id());

	st->state = CPUHP_AP_ONLINE_IDLE;
	complete_ap_thread(st, true);
}

/*
 * _cpu_up()：CPU 上线的核心实现。
 *
 * BP（boot CPU）侧执行，负责把目标 CPU 从当前状态推进到 target 状态。
 * 状态推进分两段：
 *   1. BP 负责推进到 CPUHP_BRINGUP_CPU（包含发送 IPI 唤醒 AP 的步骤）
 *   2. AP 的热插拔线程负责从 CPUHP_BRINGUP_CPU 推进到最终目标状态
 *
 * 调用前提：必须持有 cpu_add_remove_lock。
 */
static int _cpu_up(unsigned int cpu, int tasks_frozen, enum cpuhp_state target)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
	struct task_struct *idle;
	int ret = 0;

	/* 获取写锁，防止并发的 CPU 上下线操作 */
	cpus_write_lock();

	/* 检查 CPU 是否存在（在设备树或 ACPI 表中被识别） */
	if (!cpu_present(cpu)) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * 并发保护：如果另一个调用者已经把 CPU 上线到 target，
	 * 本次调用什么都不需要做。
	 */
	if (st->state >= target)
		goto out;

	if (st->state == CPUHP_OFFLINE) {
		/*
		 * 从完全离线状态开始上线，先准备 idle 线程。
		 * 每个 CPU 都有一个预分配的 idle 线程（PID 0 的每 CPU 副本），
		 * CPU 没有任务时就运行它（执行 WFI 指令节省电量）。
		 */
		idle = idle_thread_get(cpu);
		if (IS_ERR(idle)) {
			ret = PTR_ERR(idle);
			goto out;
		}

		/*
		 * 清理上次下线时留下的过期栈状态。
		 * CPU 下线后其 idle 线程的栈可能有脏数据，
		 * 上线前必须重置，否则 SCS（Shadow Call Stack）等安全机制会误判。
		 */
		scs_task_reset(idle);
		kasan_unpoison_task_stack(idle); /* 解毒 KASAN 对这块栈的标记 */
	}

	/* 记录当前是否在任务冻结状态（休眠路径会设置为 true） */
	cpuhp_tasks_frozen = tasks_frozen;

	/* 设置状态机目标，确定 bringup 方向 */
	cpuhp_set_state(cpu, st, target);

	/*
	 * 如果 CPU 已经到了 AP 热插拔线程负责的范围（>CPUHP_BRINGUP_CPU），
	 * 说明之前上线到一半被打断了，需要踢一下 AP 线程继续推进。
	 */
	if (st->state > CPUHP_BRINGUP_CPU) {
		ret = cpuhp_kick_ap_work(cpu);
		/* AP 侧已经完成了错误回滚，直接返回错误码 */
		if (ret)
			goto out;
	}

	/*
	 * BP 侧最多推进到 CPUHP_BRINGUP_CPU 状态（包含发 IPI 唤醒 AP）。
	 * 之后 AP 的热插拔线程接管，继续推进到最终目标状态。
	 * 这种 BP/AP 分工的原因：CPUHP_BRINGUP_CPU 之后的步骤
	 * 必须在 AP 自己的 CPU 上运行（如初始化本 CPU 的 per-cpu 数据）。
	 */
	target = min((int)target, CPUHP_BRINGUP_CPU);
	ret = cpuhp_up_callbacks(cpu, st, target);
out:
	cpus_write_unlock();
	/* 通知架构层 SMT（超线程）状态发生变化 */
	arch_smt_update();
	return ret;
}

/*
 * cpu_up()：CPU 上线的外部接口。
 *
 * 在调用 _cpu_up() 之前做一系列前置检查：
 *   1. CPU 是否在 possible 列表中（硬件存在且内核支持）
 *   2. CPU 所在的 NUMA 节点是否在线
 *   3. 热插拔是否被禁用（系统挂起等场景）
 *   4. CPU 是否允许启动（SMT 限制等）
 *
 * cpu_possible / cpu_present / cpu_online 三种 mask 的区别：
 *   possible：硬件存在，内核编译时支持（上限）
 *   present：硬件已被识别和注册（可以上线）
 *   online：当前正在运行（可以调度任务）
 */
static int cpu_up(unsigned int cpu, enum cpuhp_state target)
{
	int err = 0;

	/*
	 * cpu_possible 在启动时根据设备树/ACPI 确定，
	 * 不在 possible 列表中的 CPU 永远不能上线。
	 */
	if (!cpu_possible(cpu)) {
		pr_err("can't online cpu %d because it is not configured as may-hotadd at boot time\n",
		       cpu);
		return -EINVAL;
	}

	/*
	 * 确保 CPU 所在的 NUMA 节点已经上线。
	 * NUMA 节点上线涉及内存管理，必须在 CPU 上线前完成。
	 */
	err = try_online_node(cpu_to_node(cpu));
	if (err)
		return err;

	/* 持锁序列化，防止并发的 CPU 上下线操作 */
	cpu_maps_update_begin();

	/* 热插拔被全局禁用（如系统挂起过程中） */
	if (cpu_hotplug_disabled) {
		err = -EBUSY;
		goto out;
	}

	/*
	 * SMT 相关检查：如果关闭了超线程（nosmt 参数），
	 * 非主线程的 CPU 不允许上线。
	 */
	if (!cpu_bootable(cpu)) {
		err = -EPERM;
		goto out;
	}

	/* 执行实际的上线流程，tasks_frozen=0 表示非休眠路径 */
	err = _cpu_up(cpu, 0, target);
out:
	cpu_maps_update_done();
	return err;
}

/**
 * cpu_device_up - Bring up a cpu device
 * @dev: Pointer to the cpu device to online
 *
 * This function is meant to be used by device core cpu subsystem only.
 *
 * Other subsystems should use add_cpu() instead.
 *
 * Return: %0 on success or a negative errno code
 */
int cpu_device_up(struct device *dev)
{
	return cpu_up(dev->id, CPUHP_ONLINE);
}

int add_cpu(unsigned int cpu)
{
	int ret;

	lock_device_hotplug();
	ret = device_online(get_cpu_device(cpu));
	unlock_device_hotplug();

	return ret;
}
EXPORT_SYMBOL_GPL(add_cpu);

/**
 * bringup_hibernate_cpu - Bring up the CPU that we hibernated on
 * @sleep_cpu: The cpu we hibernated on and should be brought up.
 *
 * On some architectures like arm64, we can hibernate on any CPU, but on
 * wake up the CPU we hibernated on might be offline as a side effect of
 * using maxcpus= for example.
 *
 * Return: %0 on success or a negative errno code
 */
int bringup_hibernate_cpu(unsigned int sleep_cpu)
{
	int ret;

	if (!cpu_online(sleep_cpu)) {
		pr_info("Hibernated on a CPU that is offline! Bringing CPU up.\n");
		ret = cpu_up(sleep_cpu, CPUHP_ONLINE);
		if (ret) {
			pr_err("Failed to bring hibernate-CPU up!\n");
			return ret;
		}
	}
	return 0;
}

static void __init cpuhp_bringup_mask(const struct cpumask *mask, unsigned int ncpus,
				      enum cpuhp_state target)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask) {
		struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);

		if (cpu_up(cpu, target) && can_rollback_cpu(st)) {
			/*
			 * If this failed then cpu_up() might have only
			 * rolled back to CPUHP_BP_KICK_AP for the final
			 * online. Clean it up. NOOP if already rolled back.
			 */
			WARN_ON(cpuhp_invoke_callback_range(false, cpu, st, CPUHP_OFFLINE));
		}

		if (!--ncpus)
			break;
	}
}

#ifdef CONFIG_HOTPLUG_PARALLEL
static bool __cpuhp_parallel_bringup __ro_after_init = true;

static int __init parallel_bringup_parse_param(char *arg)
{
	return kstrtobool(arg, &__cpuhp_parallel_bringup);
}
early_param("cpuhp.parallel", parallel_bringup_parse_param);

#ifdef CONFIG_HOTPLUG_SMT
static inline bool cpuhp_smt_aware(void)
{
	return cpu_smt_max_threads > 1;
}

static inline const struct cpumask *cpuhp_get_primary_thread_mask(void)
{
	return cpu_primary_thread_mask;
}
#else
static inline bool cpuhp_smt_aware(void)
{
	return false;
}
static inline const struct cpumask *cpuhp_get_primary_thread_mask(void)
{
	return cpu_none_mask;
}
#endif

bool __weak arch_cpuhp_init_parallel_bringup(void)
{
	return true;
}

/*
 * On architectures which have enabled parallel bringup this invokes all BP
 * prepare states for each of the to be onlined APs first. The last state
 * sends the startup IPI to the APs. The APs proceed through the low level
 * bringup code in parallel and then wait for the control CPU to release
 * them one by one for the final onlining procedure.
 *
 * This avoids waiting for each AP to respond to the startup IPI in
 * CPUHP_BRINGUP_CPU.
 */
static bool __init cpuhp_bringup_cpus_parallel(unsigned int ncpus)
{
	const struct cpumask *mask = cpu_present_mask;

	if (__cpuhp_parallel_bringup)
		__cpuhp_parallel_bringup = arch_cpuhp_init_parallel_bringup();
	if (!__cpuhp_parallel_bringup)
		return false;

	if (cpuhp_smt_aware()) {
		const struct cpumask *pmask = cpuhp_get_primary_thread_mask();
		static struct cpumask tmp_mask __initdata;

		/*
		 * X86 requires to prevent that SMT siblings stopped while
		 * the primary thread does a microcode update for various
		 * reasons. Bring the primary threads up first.
		 */
		cpumask_and(&tmp_mask, mask, pmask);
		cpuhp_bringup_mask(&tmp_mask, ncpus, CPUHP_BP_KICK_AP);
		cpuhp_bringup_mask(&tmp_mask, ncpus, CPUHP_ONLINE);
		/* Account for the online CPUs */
		ncpus -= num_online_cpus();
		if (!ncpus)
			return true;
		/* Create the mask for secondary CPUs */
		cpumask_andnot(&tmp_mask, mask, pmask);
		mask = &tmp_mask;
	}

	/* Bring the not-yet started CPUs up */
	cpuhp_bringup_mask(mask, ncpus, CPUHP_BP_KICK_AP);
	cpuhp_bringup_mask(mask, ncpus, CPUHP_ONLINE);
	return true;
}
#else
static inline bool cpuhp_bringup_cpus_parallel(unsigned int ncpus) { return false; }
#endif /* CONFIG_HOTPLUG_PARALLEL */

/*
 * bringup_nonboot_cpus()：启动所有非 boot CPU，由 smp_init() 调用。
 *
 * max_cpus：允许上线的最大 CPU 数（由 maxcpus= 内核参数控制）。
 *
 * 两种策略：
 *   并行启动（CONFIG_HOTPLUG_PARALLEL）：
 *     先向所有 AP 发 IPI 唤醒信号，再逐个等待它们完成上线。
 *     减少了等待每个 AP 响应 IPI 的串行延迟，加快启动速度。
 *
 *   串行启动（默认）：
 *     逐个 CPU 执行 cpu_up()，等一个完成再启动下一个。
 *     简单可靠，是大多数场景的默认选择。
 */
void __init bringup_nonboot_cpus(unsigned int max_cpus)
{
	if (!max_cpus)
		return;

	/* 尝试并行启动优化（如果架构和配置支持） */
	if (cpuhp_bringup_cpus_parallel(max_cpus))
		return;

	/* 串行逐个启动所有 present 的 CPU，目标状态为完全 ONLINE */
	cpuhp_bringup_mask(cpu_present_mask, max_cpus, CPUHP_ONLINE);
}

/*
 * ══════════════════════════════════════════════════════════════
 * 休眠/挂起支持：冻结和解冻次级 CPU
 * ══════════════════════════════════════════════════════════════
 *
 * 系统进入休眠（suspend/hibernate）前必须关闭除主 CPU 以外的所有 CPU，
 * 唤醒后再把它们重新上线。
 *
 * 原因：休眠时硬件状态会发生变化，多核并发运行可能导致状态不一致。
 * 冻结后只剩一个 CPU 运行，可以安全地保存/恢复系统状态。
 */
#ifdef CONFIG_PM_SLEEP_SMP
/* 记录休眠前被关闭的 CPU 集合，唤醒时按此集合恢复 */
static cpumask_var_t frozen_cpus;

/*
 * freeze_secondary_cpus()：关闭除 primary 以外的所有在线 CPU。
 *
 * primary：休眠期间唯一保留运行的 CPU（通常选有时钟源的 CPU）。
 * 从高编号 CPU 开始关闭，避免与用户态的 CPU 热插拔操作竞争。
 * 如果有唤醒事件挂起（pm_wakeup_pending）则中止冻结。
 * 关闭完成后增加 cpu_hotplug_disabled 计数，防止其他代码意外上线 CPU。
 */
int freeze_secondary_cpus(int primary)
{
	int cpu, error = 0;

	cpu_maps_update_begin();
	if (primary == -1) {
		primary = cpumask_first(cpu_online_mask);
		if (!housekeeping_cpu(primary, HK_TYPE_TIMER))
			primary = housekeeping_any_cpu(HK_TYPE_TIMER);
	} else {
		if (!cpu_online(primary))
			primary = cpumask_first(cpu_online_mask);
	}

	/*
	 * We take down all of the non-boot CPUs in one shot to avoid races
	 * with the userspace trying to use the CPU hotplug at the same time
	 */
	cpumask_clear(frozen_cpus);

	pr_info("Disabling non-boot CPUs ...\n");
	for (cpu = nr_cpu_ids - 1; cpu >= 0; cpu--) {
		if (!cpu_online(cpu) || cpu == primary)
			continue;

		if (pm_wakeup_pending()) {
			pr_info("Wakeup pending. Abort CPU freeze\n");
			error = -EBUSY;
			break;
		}

		trace_suspend_resume(TPS("CPU_OFF"), cpu, true);
		error = _cpu_down(cpu, 1, CPUHP_OFFLINE);
		trace_suspend_resume(TPS("CPU_OFF"), cpu, false);
		if (!error)
			cpumask_set_cpu(cpu, frozen_cpus);
		else {
			pr_err("Error taking CPU%d down: %d\n", cpu, error);
			break;
		}
	}

	if (!error)
		BUG_ON(num_online_cpus() > 1);
	else
		pr_err("Non-boot CPUs are not disabled\n");

	/*
	 * Make sure the CPUs won't be enabled by someone else. We need to do
	 * this even in case of failure as all freeze_secondary_cpus() users are
	 * supposed to do thaw_secondary_cpus() on the failure path.
	 */
	cpu_hotplug_disabled++;

	cpu_maps_update_done();
	return error;
}

void __weak arch_thaw_secondary_cpus_begin(void)
{
}

void __weak arch_thaw_secondary_cpus_end(void)
{
}

/*
 * thaw_secondary_cpus()：系统唤醒后恢复之前冻结的所有 CPU。
 *
 * 先减少 cpu_hotplug_disabled 计数（允许热插拔），
 * 再逐个上线 frozen_cpus 中记录的 CPU。
 * 即使某个 CPU 上线失败也继续尝试其他 CPU（降级运行而不是 panic）。
 */
void thaw_secondary_cpus(void)
{
	int cpu, error;

	/* Allow everyone to use the CPU hotplug again */
	cpu_maps_update_begin();
	__cpu_hotplug_enable();
	if (cpumask_empty(frozen_cpus))
		goto out;

	pr_info("Enabling non-boot CPUs ...\n");

	arch_thaw_secondary_cpus_begin();

	for_each_cpu(cpu, frozen_cpus) {
		trace_suspend_resume(TPS("CPU_ON"), cpu, true);
		error = _cpu_up(cpu, 1, CPUHP_ONLINE);
		trace_suspend_resume(TPS("CPU_ON"), cpu, false);
		if (!error) {
			pr_info("CPU%d is up\n", cpu);
			continue;
		}
		pr_warn("Error taking CPU%d up: %d\n", cpu, error);
	}

	arch_thaw_secondary_cpus_end();

	cpumask_clear(frozen_cpus);
out:
	cpu_maps_update_done();
}

static int __init alloc_frozen_cpus(void)
{
	if (!alloc_cpumask_var(&frozen_cpus, GFP_KERNEL|__GFP_ZERO))
		return -ENOMEM;
	return 0;
}
core_initcall(alloc_frozen_cpus);

/*
 * When callbacks for CPU hotplug notifications are being executed, we must
 * ensure that the state of the system with respect to the tasks being frozen
 * or not, as reported by the notification, remains unchanged *throughout the
 * duration* of the execution of the callbacks.
 * Hence we need to prevent the freezer from racing with regular CPU hotplug.
 *
 * This synchronization is implemented by mutually excluding regular CPU
 * hotplug and Suspend/Hibernate call paths by hooking onto the Suspend/
 * Hibernate notifications.
 */
/*
 * cpu_hotplug_pm_callback()：电源管理通知回调。
 *
 * 挂起/休眠前禁用 CPU 热插拔，恢复后重新启用。
 * 目的：防止休眠过程中用户态触发 CPU 热插拔操作，
 * 与 freeze_secondary_cpus() 发生竞争。
 *
 * 优先级高于 x86 的 bsp_pm_callback，确保 CPU 热插拔先被禁用，
 * 再执行架构相关的休眠准备（依赖此顺序）。
 */
static int
cpu_hotplug_pm_callback(struct notifier_block *nb,
			unsigned long action, void *ptr)
{
	switch (action) {

	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
		cpu_hotplug_disable();
		break;

	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
		cpu_hotplug_enable();
		break;

	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}


static int __init cpu_hotplug_pm_sync_init(void)
{
	/*
	 * cpu_hotplug_pm_callback has higher priority than x86
	 * bsp_pm_callback which depends on cpu_hotplug_pm_callback
	 * to disable cpu hotplug to avoid cpu hotplug race.
	 */
	pm_notifier(cpu_hotplug_pm_callback, 0);
	return 0;
}
core_initcall(cpu_hotplug_pm_sync_init);

#endif /* CONFIG_PM_SLEEP_SMP */

int __boot_cpu_id;

#endif /* CONFIG_SMP */

/*
 * ══════════════════════════════════════════════════════════════
 * 热插拔状态表：所有内置状态的回调注册
 * ══════════════════════════════════════════════════════════════
 *
 * 这是整个状态机的"地图"，每个枚举值对应一个步骤（cpuhp_step）。
 *
 * 状态大致分三段：
 *   OFFLINE → CPUHP_BRINGUP_CPU：BP 侧执行（准备工作 + 发 IPI 唤醒 AP）
 *   CPUHP_AP_ONLINE_IDLE：AP 完成低级初始化，等待 BP 放行
 *   CPUHP_AP_ONLINE_IDLE → CPUHP_ONLINE：AP 热插拔线程执行（AP 自身初始化）
 *
 * 中间的动态区域（CPUHP_AP_ONLINE_DYN / CPUHP_BP_PREPARE_DYN）
 * 供驱动运行时通过 cpuhp_setup_state() 动态注册。
 *
 * 上线顺序（从上往下读）：
 *   OFFLINE → ... → BRINGUP_CPU → AP_ONLINE_IDLE → ... → ACTIVE → ONLINE
 * 下线顺序（从下往上读，teardown 回调）：
 *   ONLINE → ACTIVE → ... → AP_ONLINE_IDLE → TEARDOWN_CPU → ... → OFFLINE
 */
static struct cpuhp_step cpuhp_hp_states[] = {
	[CPUHP_OFFLINE] = {
		.name			= "offline",
		.startup.single		= NULL,
		.teardown.single	= NULL,
	},
#ifdef CONFIG_SMP
	[CPUHP_CREATE_THREADS]= {
		.name			= "threads:prepare",
		.startup.single		= smpboot_create_threads,
		.teardown.single	= NULL,
		.cant_stop		= true,
	},
	[CPUHP_RANDOM_PREPARE] = {
		.name			= "random:prepare",
		.startup.single		= random_prepare_cpu,
		.teardown.single	= NULL,
	},
	[CPUHP_WORKQUEUE_PREP] = {
		.name			= "workqueue:prepare",
		.startup.single		= workqueue_prepare_cpu,
		.teardown.single	= NULL,
	},
	[CPUHP_HRTIMERS_PREPARE] = {
		.name			= "hrtimers:prepare",
		.startup.single		= hrtimers_prepare_cpu,
		.teardown.single	= NULL,
	},
	[CPUHP_SMPCFD_PREPARE] = {
		.name			= "smpcfd:prepare",
		.startup.single		= smpcfd_prepare_cpu,
		.teardown.single	= smpcfd_dead_cpu,
	},
	[CPUHP_RELAY_PREPARE] = {
		.name			= "relay:prepare",
		.startup.single		= relay_prepare_cpu,
		.teardown.single	= NULL,
	},
	[CPUHP_RCUTREE_PREP] = {
		.name			= "RCU/tree:prepare",
		.startup.single		= rcutree_prepare_cpu,
		.teardown.single	= rcutree_dead_cpu,
	},
	/*
	 * On the tear-down path, timers_dead_cpu() must be invoked
	 * before blk_mq_queue_reinit_notify() from notify_dead(),
	 * otherwise a RCU stall occurs.
	 */
	[CPUHP_TIMERS_PREPARE] = {
		.name			= "timers:prepare",
		.startup.single		= timers_prepare_cpu,
		.teardown.single	= timers_dead_cpu,
	},

#ifdef CONFIG_HOTPLUG_SPLIT_STARTUP
	/*
	 * Kicks the AP alive. AP will wait in cpuhp_ap_sync_alive() until
	 * the next step will release it.
	 */
	[CPUHP_BP_KICK_AP] = {
		.name			= "cpu:kick_ap",
		.startup.single		= cpuhp_kick_ap_alive,
	},

	/*
	 * Waits for the AP to reach cpuhp_ap_sync_alive() and then
	 * releases it for the complete bringup.
	 */
	[CPUHP_BRINGUP_CPU] = {
		.name			= "cpu:bringup",
		.startup.single		= cpuhp_bringup_ap,
		.teardown.single	= finish_cpu,
		.cant_stop		= true,
	},
#else
	/*
	 * All-in-one CPU bringup state which includes the kick alive.
	 */
	[CPUHP_BRINGUP_CPU] = {
		.name			= "cpu:bringup",
		.startup.single		= bringup_cpu,
		.teardown.single	= finish_cpu,
		.cant_stop		= true,
	},
#endif
	/* Final state before CPU kills itself */
	[CPUHP_AP_IDLE_DEAD] = {
		.name			= "idle:dead",
	},
	/*
	 * Last state before CPU enters the idle loop to die. Transient state
	 * for synchronization.
	 */
	[CPUHP_AP_OFFLINE] = {
		.name			= "ap:offline",
		.cant_stop		= true,
	},
	/* First state is scheduler control. Interrupts are disabled */
	[CPUHP_AP_SCHED_STARTING] = {
		.name			= "sched:starting",
		.startup.single		= sched_cpu_starting,
		.teardown.single	= sched_cpu_dying,
	},
	[CPUHP_AP_RCUTREE_DYING] = {
		.name			= "RCU/tree:dying",
		.startup.single		= NULL,
		.teardown.single	= rcutree_dying_cpu,
	},
	[CPUHP_AP_SMPCFD_DYING] = {
		.name			= "smpcfd:dying",
		.startup.single		= NULL,
		.teardown.single	= smpcfd_dying_cpu,
	},
	[CPUHP_AP_HRTIMERS_DYING] = {
		.name			= "hrtimers:dying",
		.startup.single		= hrtimers_cpu_starting,
		.teardown.single	= hrtimers_cpu_dying,
	},
	[CPUHP_AP_TICK_DYING] = {
		.name			= "tick:dying",
		.startup.single		= NULL,
		.teardown.single	= tick_cpu_dying,
	},
	/* Entry state on starting. Interrupts enabled from here on. Transient
	 * state for synchronsization */
	[CPUHP_AP_ONLINE] = {
		.name			= "ap:online",
	},
	/*
	 * Handled on control processor until the plugged processor manages
	 * this itself.
	 */
	[CPUHP_TEARDOWN_CPU] = {
		.name			= "cpu:teardown",
		.startup.single		= NULL,
		.teardown.single	= takedown_cpu,
		.cant_stop		= true,
	},

	[CPUHP_AP_SCHED_WAIT_EMPTY] = {
		.name			= "sched:waitempty",
		.startup.single		= NULL,
		.teardown.single	= sched_cpu_wait_empty,
	},

	/* Handle smpboot threads park/unpark */
	[CPUHP_AP_SMPBOOT_THREADS] = {
		.name			= "smpboot/threads:online",
		.startup.single		= smpboot_unpark_threads,
		.teardown.single	= smpboot_park_threads,
	},
	[CPUHP_AP_IRQ_AFFINITY_ONLINE] = {
		.name			= "irq/affinity:online",
		.startup.single		= irq_affinity_online_cpu,
		.teardown.single	= NULL,
	},
	[CPUHP_AP_PERF_ONLINE] = {
		.name			= "perf:online",
		.startup.single		= perf_event_init_cpu,
		.teardown.single	= perf_event_exit_cpu,
	},
	[CPUHP_AP_WATCHDOG_ONLINE] = {
		.name			= "lockup_detector:online",
		.startup.single		= lockup_detector_online_cpu,
		.teardown.single	= lockup_detector_offline_cpu,
	},
	[CPUHP_AP_WORKQUEUE_ONLINE] = {
		.name			= "workqueue:online",
		.startup.single		= workqueue_online_cpu,
		.teardown.single	= workqueue_offline_cpu,
	},
	[CPUHP_AP_RANDOM_ONLINE] = {
		.name			= "random:online",
		.startup.single		= random_online_cpu,
		.teardown.single	= NULL,
	},
	[CPUHP_AP_RCUTREE_ONLINE] = {
		.name			= "RCU/tree:online",
		.startup.single		= rcutree_online_cpu,
		.teardown.single	= rcutree_offline_cpu,
	},
#endif
	/*
	 * The dynamically registered state space is here
	 */

#ifdef CONFIG_SMP
	/* Last state is scheduler control setting the cpu active */
	[CPUHP_AP_ACTIVE] = {
		.name			= "sched:active",
		.startup.single		= sched_cpu_activate,
		.teardown.single	= sched_cpu_deactivate,
	},
#endif

	/* CPU is fully up and running. */
	[CPUHP_ONLINE] = {
		.name			= "online",
		.startup.single		= NULL,
		.teardown.single	= NULL,
	},
};

/* Sanity check for callbacks */
static int cpuhp_cb_check(enum cpuhp_state state)
{
	if (state <= CPUHP_OFFLINE || state >= CPUHP_ONLINE)
		return -EINVAL;
	return 0;
}

/*
 * ══════════════════════════════════════════════════════════════
 * 状态注册/注销接口
 * ══════════════════════════════════════════════════════════════
 *
 * 驱动/子系统通过以下接口在运行时动态注册热插拔回调：
 *   cpuhp_setup_state()    → 注册，并对已在线 CPU 立即调用 startup
 *   cpuhp_remove_state()   → 注销，并对已在线 CPU 调用 teardown
 *   cpuhp_state_add_instance() → 多实例：添加一个实例
 *   cpuhp_state_remove_instance() → 多实例：移除一个实例
 *
 * 使用示例（驱动初始化时）：
 *   cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "mydrv:online",
 *                     mydrv_cpu_up, mydrv_cpu_down);
 */

/*
 * cpuhp_reserve_state()：在动态状态区间分配一个空闲的状态槽。
 * 空闲槽通过 name==NULL 识别。
 */
static int cpuhp_reserve_state(enum cpuhp_state state)
{
	enum cpuhp_state i, end;
	struct cpuhp_step *step;

	switch (state) {
	case CPUHP_AP_ONLINE_DYN:
		step = cpuhp_hp_states + CPUHP_AP_ONLINE_DYN;
		end = CPUHP_AP_ONLINE_DYN_END;
		break;
	case CPUHP_BP_PREPARE_DYN:
		step = cpuhp_hp_states + CPUHP_BP_PREPARE_DYN;
		end = CPUHP_BP_PREPARE_DYN_END;
		break;
	default:
		return -EINVAL;
	}

	for (i = state; i <= end; i++, step++) {
		if (!step->name)
			return i;
	}
	WARN(1, "No more dynamic states available for CPU hotplug\n");
	return -ENOSPC;
}

static int cpuhp_store_callbacks(enum cpuhp_state state, const char *name,
				 int (*startup)(unsigned int cpu),
				 int (*teardown)(unsigned int cpu),
				 bool multi_instance)
{
	/* (Un)Install the callbacks for further cpu hotplug operations */
	struct cpuhp_step *sp;
	int ret = 0;

	/*
	 * If name is NULL, then the state gets removed.
	 *
	 * CPUHP_AP_ONLINE_DYN and CPUHP_BP_PREPARE_DYN are handed out on
	 * the first allocation from these dynamic ranges, so the removal
	 * would trigger a new allocation and clear the wrong (already
	 * empty) state, leaving the callbacks of the to be cleared state
	 * dangling, which causes wreckage on the next hotplug operation.
	 */
	if (name && (state == CPUHP_AP_ONLINE_DYN ||
		     state == CPUHP_BP_PREPARE_DYN)) {
		ret = cpuhp_reserve_state(state);
		if (ret < 0)
			return ret;
		state = ret;
	}
	sp = cpuhp_get_step(state);
	if (name && sp->name)
		return -EBUSY;

	sp->startup.single = startup;
	sp->teardown.single = teardown;
	sp->name = name;
	sp->multi_instance = multi_instance;
	INIT_HLIST_HEAD(&sp->list);
	return ret;
}

static void *cpuhp_get_teardown_cb(enum cpuhp_state state)
{
	return cpuhp_get_step(state)->teardown.single;
}

/*
 * Call the startup/teardown function for a step either on the AP or
 * on the current CPU.
 */
static int cpuhp_issue_call(int cpu, enum cpuhp_state state, bool bringup,
			    struct hlist_node *node)
{
	struct cpuhp_step *sp = cpuhp_get_step(state);
	int ret;

	/*
	 * If there's nothing to do, we done.
	 * Relies on the union for multi_instance.
	 */
	if (cpuhp_step_empty(bringup, sp))
		return 0;
	/*
	 * The non AP bound callbacks can fail on bringup. On teardown
	 * e.g. module removal we crash for now.
	 */
#ifdef CONFIG_SMP
	if (cpuhp_is_ap_state(state))
		ret = cpuhp_invoke_ap_callback(cpu, state, bringup, node);
	else
		ret = cpuhp_invoke_callback(cpu, state, bringup, node, NULL);
#else
	if (cpuhp_is_atomic_state(state)) {
		guard(irqsave)();
		ret = cpuhp_invoke_callback(cpu, state, bringup, node, NULL);
		/* STARTING/DYING must not fail! */
		WARN_ON_ONCE(ret);
	} else {
		ret = cpuhp_invoke_callback(cpu, state, bringup, node, NULL);
	}
#endif
	BUG_ON(ret && !bringup);
	return ret;
}

/*
 * Called from __cpuhp_setup_state on a recoverable failure.
 *
 * Note: The teardown callbacks for rollback are not allowed to fail!
 */
static void cpuhp_rollback_install(int failedcpu, enum cpuhp_state state,
				   struct hlist_node *node)
{
	int cpu;

	/* Roll back the already executed steps on the other cpus */
	for_each_present_cpu(cpu) {
		struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
		int cpustate = st->state;

		if (cpu >= failedcpu)
			break;

		/* Did we invoke the startup call on that cpu ? */
		if (cpustate >= state)
			cpuhp_issue_call(cpu, state, false, node);
	}
}

int __cpuhp_state_add_instance_cpuslocked(enum cpuhp_state state,
					  struct hlist_node *node,
					  bool invoke)
{
	struct cpuhp_step *sp;
	int cpu;
	int ret;

	lockdep_assert_cpus_held();

	sp = cpuhp_get_step(state);
	if (sp->multi_instance == false)
		return -EINVAL;

	mutex_lock(&cpuhp_state_mutex);

	if (!invoke || !sp->startup.multi)
		goto add_node;

	/*
	 * Try to call the startup callback for each present cpu
	 * depending on the hotplug state of the cpu.
	 */
	for_each_present_cpu(cpu) {
		struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
		int cpustate = st->state;

		if (cpustate < state)
			continue;

		ret = cpuhp_issue_call(cpu, state, true, node);
		if (ret) {
			if (sp->teardown.multi)
				cpuhp_rollback_install(cpu, state, node);
			goto unlock;
		}
	}
add_node:
	ret = 0;
	hlist_add_head(node, &sp->list);
unlock:
	mutex_unlock(&cpuhp_state_mutex);
	return ret;
}

int __cpuhp_state_add_instance(enum cpuhp_state state, struct hlist_node *node,
			       bool invoke)
{
	int ret;

	cpus_read_lock();
	ret = __cpuhp_state_add_instance_cpuslocked(state, node, invoke);
	cpus_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(__cpuhp_state_add_instance);

/**
 * __cpuhp_setup_state_cpuslocked - Setup the callbacks for an hotplug machine state
 * @state:		The state to setup
 * @name:		Name of the step
 * @invoke:		If true, the startup function is invoked for cpus where
 *			cpu state >= @state
 * @startup:		startup callback function
 * @teardown:		teardown callback function
 * @multi_instance:	State is set up for multiple instances which get
 *			added afterwards.
 *
 * The caller needs to hold cpus read locked while calling this function.
 * Return:
 *   On success:
 *      Positive state number if @state is CPUHP_AP_ONLINE_DYN or CPUHP_BP_PREPARE_DYN;
 *      0 for all other states
 *   On failure: proper (negative) error code
 */
int __cpuhp_setup_state_cpuslocked(enum cpuhp_state state,
				   const char *name, bool invoke,
				   int (*startup)(unsigned int cpu),
				   int (*teardown)(unsigned int cpu),
				   bool multi_instance)
{
	int cpu, ret = 0;
	bool dynstate;

	lockdep_assert_cpus_held();

	if (cpuhp_cb_check(state) || !name)
		return -EINVAL;

	mutex_lock(&cpuhp_state_mutex);

	ret = cpuhp_store_callbacks(state, name, startup, teardown,
				    multi_instance);

	dynstate = state == CPUHP_AP_ONLINE_DYN || state == CPUHP_BP_PREPARE_DYN;
	if (ret > 0 && dynstate) {
		state = ret;
		ret = 0;
	}

	if (ret || !invoke || !startup)
		goto out;

	/*
	 * Try to call the startup callback for each present cpu
	 * depending on the hotplug state of the cpu.
	 */
	for_each_present_cpu(cpu) {
		struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
		int cpustate = st->state;

		if (cpustate < state)
			continue;

		ret = cpuhp_issue_call(cpu, state, true, NULL);
		if (ret) {
			if (teardown)
				cpuhp_rollback_install(cpu, state, NULL);
			cpuhp_store_callbacks(state, NULL, NULL, NULL, false);
			goto out;
		}
	}
out:
	mutex_unlock(&cpuhp_state_mutex);
	/*
	 * If the requested state is CPUHP_AP_ONLINE_DYN or CPUHP_BP_PREPARE_DYN,
	 * return the dynamically allocated state in case of success.
	 */
	if (!ret && dynstate)
		return state;
	return ret;
}
EXPORT_SYMBOL(__cpuhp_setup_state_cpuslocked);

int __cpuhp_setup_state(enum cpuhp_state state,
			const char *name, bool invoke,
			int (*startup)(unsigned int cpu),
			int (*teardown)(unsigned int cpu),
			bool multi_instance)
{
	int ret;

	cpus_read_lock();
	ret = __cpuhp_setup_state_cpuslocked(state, name, invoke, startup,
					     teardown, multi_instance);
	cpus_read_unlock();
	return ret;
}
EXPORT_SYMBOL(__cpuhp_setup_state);

int __cpuhp_state_remove_instance(enum cpuhp_state state,
				  struct hlist_node *node, bool invoke)
{
	struct cpuhp_step *sp = cpuhp_get_step(state);
	int cpu;

	BUG_ON(cpuhp_cb_check(state));

	if (!sp->multi_instance)
		return -EINVAL;

	cpus_read_lock();
	mutex_lock(&cpuhp_state_mutex);

	if (!invoke || !cpuhp_get_teardown_cb(state))
		goto remove;
	/*
	 * Call the teardown callback for each present cpu depending
	 * on the hotplug state of the cpu. This function is not
	 * allowed to fail currently!
	 */
	for_each_present_cpu(cpu) {
		struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
		int cpustate = st->state;

		if (cpustate >= state)
			cpuhp_issue_call(cpu, state, false, node);
	}

remove:
	hlist_del(node);
	mutex_unlock(&cpuhp_state_mutex);
	cpus_read_unlock();

	return 0;
}
EXPORT_SYMBOL_GPL(__cpuhp_state_remove_instance);

/**
 * __cpuhp_remove_state_cpuslocked - Remove the callbacks for an hotplug machine state
 * @state:	The state to remove
 * @invoke:	If true, the teardown function is invoked for cpus where
 *		cpu state >= @state
 *
 * The caller needs to hold cpus read locked while calling this function.
 * The teardown callback is currently not allowed to fail. Think
 * about module removal!
 */
void __cpuhp_remove_state_cpuslocked(enum cpuhp_state state, bool invoke)
{
	struct cpuhp_step *sp = cpuhp_get_step(state);
	int cpu;

	BUG_ON(cpuhp_cb_check(state));

	lockdep_assert_cpus_held();

	mutex_lock(&cpuhp_state_mutex);
	if (sp->multi_instance) {
		WARN(!hlist_empty(&sp->list),
		     "Error: Removing state %d which has instances left.\n",
		     state);
		goto remove;
	}

	if (!invoke || !cpuhp_get_teardown_cb(state))
		goto remove;

	/*
	 * Call the teardown callback for each present cpu depending
	 * on the hotplug state of the cpu. This function is not
	 * allowed to fail currently!
	 */
	for_each_present_cpu(cpu) {
		struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, cpu);
		int cpustate = st->state;

		if (cpustate >= state)
			cpuhp_issue_call(cpu, state, false, NULL);
	}
remove:
	cpuhp_store_callbacks(state, NULL, NULL, NULL, false);
	mutex_unlock(&cpuhp_state_mutex);
}
EXPORT_SYMBOL(__cpuhp_remove_state_cpuslocked);

void __cpuhp_remove_state(enum cpuhp_state state, bool invoke)
{
	cpus_read_lock();
	__cpuhp_remove_state_cpuslocked(state, invoke);
	cpus_read_unlock();
}
EXPORT_SYMBOL(__cpuhp_remove_state);

#ifdef CONFIG_HOTPLUG_SMT
static void cpuhp_offline_cpu_device(unsigned int cpu)
{
	struct device *dev = get_cpu_device(cpu);

	dev_set_offline(dev);
	/* Tell user space about the state change */
	kobject_uevent(&dev->kobj, KOBJ_OFFLINE);
}

static void cpuhp_online_cpu_device(unsigned int cpu)
{
	struct device *dev = get_cpu_device(cpu);

	dev_clear_offline(dev);
	/* Tell user space about the state change */
	kobject_uevent(&dev->kobj, KOBJ_ONLINE);
}

int cpuhp_smt_disable(enum cpuhp_smt_control ctrlval)
{
	int cpu, ret = 0;

	cpu_maps_update_begin();
	for_each_online_cpu(cpu) {
		if (topology_is_primary_thread(cpu))
			continue;
		/*
		 * Disable can be called with CPU_SMT_ENABLED when changing
		 * from a higher to lower number of SMT threads per core.
		 */
		if (ctrlval == CPU_SMT_ENABLED && cpu_smt_thread_allowed(cpu))
			continue;
		ret = cpu_down_maps_locked(cpu, CPUHP_OFFLINE);
		if (ret)
			break;
		/*
		 * As this needs to hold the cpu maps lock it's impossible
		 * to call device_offline() because that ends up calling
		 * cpu_down() which takes cpu maps lock. cpu maps lock
		 * needs to be held as this might race against in kernel
		 * abusers of the hotplug machinery (thermal management).
		 *
		 * So nothing would update device:offline state. That would
		 * leave the sysfs entry stale and prevent onlining after
		 * smt control has been changed to 'off' again. This is
		 * called under the sysfs hotplug lock, so it is properly
		 * serialized against the regular offline usage.
		 */
		cpuhp_offline_cpu_device(cpu);
	}
	if (!ret)
		cpu_smt_control = ctrlval;
	cpu_maps_update_done();
	return ret;
}

/* Check if the core a CPU belongs to is online */
#if !defined(topology_is_core_online)
static inline bool topology_is_core_online(unsigned int cpu)
{
	return true;
}
#endif

int cpuhp_smt_enable(void)
{
	int cpu, ret = 0;

	cpu_maps_update_begin();
	cpu_smt_control = CPU_SMT_ENABLED;
	for_each_present_cpu(cpu) {
		/* Skip online CPUs and CPUs on offline nodes */
		if (cpu_online(cpu) || !node_online(cpu_to_node(cpu)))
			continue;
		if (!cpu_smt_thread_allowed(cpu) || !topology_is_core_online(cpu))
			continue;
		ret = _cpu_up(cpu, 0, CPUHP_ONLINE);
		if (ret)
			break;
		/* See comment in cpuhp_smt_disable() */
		cpuhp_online_cpu_device(cpu);
	}
	cpu_maps_update_done();
	return ret;
}
#endif

/*
 * ══════════════════════════════════════════════════════════════
 * sysfs 接口：/sys/devices/system/cpu/cpuN/hotplug/
 * ══════════════════════════════════════════════════════════════
 *
 * 暴露给用户空间的 CPU 热插拔控制接口：
 *
 *   state  （只读）：CPU 当前所处的热插拔状态编号
 *   target （读写）：设置 CPU 的目标状态（触发上线/下线）
 *            写 "0" → 下线（CPUHP_OFFLINE）
 *            写 最大值 → 上线（CPUHP_ONLINE）
 *   fail   （读写）：测试用，设置让哪个状态的回调故意失败
 *            用于内核开发者测试错误路径
 *
 * 全局接口 /sys/devices/system/cpu/hotplug/states：
 *   列出所有已注册的热插拔状态名称（调试用）
 */
#if defined(CONFIG_SYSFS) && defined(CONFIG_HOTPLUG_CPU)
/* state：显示 CPU 当前热插拔状态编号 */
static ssize_t state_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, dev->id);

	return sprintf(buf, "%d\n", st->state);
}
static DEVICE_ATTR_RO(state);

static ssize_t target_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, dev->id);
	struct cpuhp_step *sp;
	int target, ret;

	ret = kstrtoint(buf, 10, &target);
	if (ret)
		return ret;

#ifdef CONFIG_CPU_HOTPLUG_STATE_CONTROL
	if (target < CPUHP_OFFLINE || target > CPUHP_ONLINE)
		return -EINVAL;
#else
	if (target != CPUHP_OFFLINE && target != CPUHP_ONLINE)
		return -EINVAL;
#endif

	ret = lock_device_hotplug_sysfs();
	if (ret)
		return ret;

	mutex_lock(&cpuhp_state_mutex);
	sp = cpuhp_get_step(target);
	ret = !sp->name || sp->cant_stop ? -EINVAL : 0;
	mutex_unlock(&cpuhp_state_mutex);
	if (ret)
		goto out;

	if (st->state < target)
		ret = cpu_up(dev->id, target);
	else if (st->state > target)
		ret = cpu_down(dev->id, target);
	else if (WARN_ON(st->target != target))
		st->target = target;
out:
	unlock_device_hotplug();
	return ret ? ret : count;
}

static ssize_t target_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, dev->id);

	return sprintf(buf, "%d\n", st->target);
}
static DEVICE_ATTR_RW(target);

static ssize_t fail_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, dev->id);
	struct cpuhp_step *sp;
	int fail, ret;

	ret = kstrtoint(buf, 10, &fail);
	if (ret)
		return ret;

	if (fail == CPUHP_INVALID) {
		st->fail = fail;
		return count;
	}

	if (fail < CPUHP_OFFLINE || fail > CPUHP_ONLINE)
		return -EINVAL;

	/*
	 * Cannot fail STARTING/DYING callbacks.
	 */
	if (cpuhp_is_atomic_state(fail))
		return -EINVAL;

	/*
	 * DEAD callbacks cannot fail...
	 * ... neither can CPUHP_BRINGUP_CPU during hotunplug. The latter
	 * triggering STARTING callbacks, a failure in this state would
	 * hinder rollback.
	 */
	if (fail <= CPUHP_BRINGUP_CPU && st->state > CPUHP_BRINGUP_CPU)
		return -EINVAL;

	/*
	 * Cannot fail anything that doesn't have callbacks.
	 */
	mutex_lock(&cpuhp_state_mutex);
	sp = cpuhp_get_step(fail);
	if (!sp->startup.single && !sp->teardown.single)
		ret = -EINVAL;
	mutex_unlock(&cpuhp_state_mutex);
	if (ret)
		return ret;

	st->fail = fail;

	return count;
}

static ssize_t fail_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct cpuhp_cpu_state *st = per_cpu_ptr(&cpuhp_state, dev->id);

	return sprintf(buf, "%d\n", st->fail);
}

static DEVICE_ATTR_RW(fail);

static struct attribute *cpuhp_cpu_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_target.attr,
	&dev_attr_fail.attr,
	NULL
};

static const struct attribute_group cpuhp_cpu_attr_group = {
	.attrs = cpuhp_cpu_attrs,
	.name = "hotplug",
};

static ssize_t states_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t res = 0;
	int i;

	mutex_lock(&cpuhp_state_mutex);
	for (i = CPUHP_OFFLINE; i <= CPUHP_ONLINE; i++) {
		struct cpuhp_step *sp = cpuhp_get_step(i);

		if (sp->name)
			res += sysfs_emit_at(buf, res, "%3d: %s\n", i, sp->name);
	}
	mutex_unlock(&cpuhp_state_mutex);
	return res;
}
static DEVICE_ATTR_RO(states);

static struct attribute *cpuhp_cpu_root_attrs[] = {
	&dev_attr_states.attr,
	NULL
};

static const struct attribute_group cpuhp_cpu_root_attr_group = {
	.attrs = cpuhp_cpu_root_attrs,
	.name = "hotplug",
};

#ifdef CONFIG_HOTPLUG_SMT

static bool cpu_smt_num_threads_valid(unsigned int threads)
{
	if (IS_ENABLED(CONFIG_SMT_NUM_THREADS_DYNAMIC))
		return threads >= 1 && threads <= cpu_smt_max_threads;
	return threads == 1 || threads == cpu_smt_max_threads;
}

static ssize_t
__store_smt_control(struct device *dev, struct device_attribute *attr,
		    const char *buf, size_t count)
{
	int ctrlval, ret, num_threads, orig_threads;
	bool force_off;

	if (cpu_smt_control == CPU_SMT_FORCE_DISABLED)
		return -EPERM;

	if (cpu_smt_control == CPU_SMT_NOT_SUPPORTED)
		return -ENODEV;

	if (sysfs_streq(buf, "on")) {
		ctrlval = CPU_SMT_ENABLED;
		num_threads = cpu_smt_max_threads;
	} else if (sysfs_streq(buf, "off")) {
		ctrlval = CPU_SMT_DISABLED;
		num_threads = 1;
	} else if (sysfs_streq(buf, "forceoff")) {
		ctrlval = CPU_SMT_FORCE_DISABLED;
		num_threads = 1;
	} else if (kstrtoint(buf, 10, &num_threads) == 0) {
		if (num_threads == 1)
			ctrlval = CPU_SMT_DISABLED;
		else if (cpu_smt_num_threads_valid(num_threads))
			ctrlval = CPU_SMT_ENABLED;
		else
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	ret = lock_device_hotplug_sysfs();
	if (ret)
		return ret;

	orig_threads = cpu_smt_num_threads;
	cpu_smt_num_threads = num_threads;

	force_off = ctrlval != cpu_smt_control && ctrlval == CPU_SMT_FORCE_DISABLED;

	if (num_threads > orig_threads)
		ret = cpuhp_smt_enable();
	else if (num_threads < orig_threads || force_off)
		ret = cpuhp_smt_disable(ctrlval);

	unlock_device_hotplug();
	return ret ? ret : count;
}

#else /* !CONFIG_HOTPLUG_SMT */
static ssize_t
__store_smt_control(struct device *dev, struct device_attribute *attr,
		    const char *buf, size_t count)
{
	return -ENODEV;
}
#endif /* CONFIG_HOTPLUG_SMT */

static const char *smt_states[] = {
	[CPU_SMT_ENABLED]		= "on",
	[CPU_SMT_DISABLED]		= "off",
	[CPU_SMT_FORCE_DISABLED]	= "forceoff",
	[CPU_SMT_NOT_SUPPORTED]		= "notsupported",
	[CPU_SMT_NOT_IMPLEMENTED]	= "notimplemented",
};

static ssize_t control_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	const char *state = smt_states[cpu_smt_control];

#ifdef CONFIG_HOTPLUG_SMT
	/*
	 * If SMT is enabled but not all threads are enabled then show the
	 * number of threads. If all threads are enabled show "on". Otherwise
	 * show the state name.
	 */
	if (cpu_smt_control == CPU_SMT_ENABLED &&
	    cpu_smt_num_threads != cpu_smt_max_threads)
		return sysfs_emit(buf, "%d\n", cpu_smt_num_threads);
#endif

	return sysfs_emit(buf, "%s\n", state);
}

static ssize_t control_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	return __store_smt_control(dev, attr, buf, count);
}
static DEVICE_ATTR_RW(control);

static ssize_t active_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", sched_smt_active());
}
static DEVICE_ATTR_RO(active);

static struct attribute *cpuhp_smt_attrs[] = {
	&dev_attr_control.attr,
	&dev_attr_active.attr,
	NULL
};

static const struct attribute_group cpuhp_smt_attr_group = {
	.attrs = cpuhp_smt_attrs,
	.name = "smt",
};

static int __init cpu_smt_sysfs_init(void)
{
	struct device *dev_root;
	int ret = -ENODEV;

	dev_root = bus_get_dev_root(&cpu_subsys);
	if (dev_root) {
		ret = sysfs_create_group(&dev_root->kobj, &cpuhp_smt_attr_group);
		put_device(dev_root);
	}
	return ret;
}

static int __init cpuhp_sysfs_init(void)
{
	struct device *dev_root;
	int cpu, ret;

	ret = cpu_smt_sysfs_init();
	if (ret)
		return ret;

	dev_root = bus_get_dev_root(&cpu_subsys);
	if (dev_root) {
		ret = sysfs_create_group(&dev_root->kobj, &cpuhp_cpu_root_attr_group);
		put_device(dev_root);
		if (ret)
			return ret;
	}

	for_each_possible_cpu(cpu) {
		struct device *dev = get_cpu_device(cpu);

		if (!dev)
			continue;
		ret = sysfs_create_group(&dev->kobj, &cpuhp_cpu_attr_group);
		if (ret)
			return ret;
	}
	return 0;
}
device_initcall(cpuhp_sysfs_init);
#endif /* CONFIG_SYSFS && CONFIG_HOTPLUG_CPU */

/*
 * ══════════════════════════════════════════════════════════════
 * CPU mask 基础数据结构
 * ══════════════════════════════════════════════════════════════
 *
 * 内核用多个 cpumask 位图描述 CPU 的不同状态：
 *   possible：硬件支持且内核编译时配置的 CPU（上限）
 *   present：已被硬件识别和注册的 CPU
 *   online：当前在线（可以运行任务）的 CPU
 *   active：调度器认为活跃（加入调度域）的 CPU
 *   enabled：允许参与通用功能（如中断路由）的 CPU
 *   dying：正在下线过程中的 CPU
 *
 * 这些 mask 是调度器、中断路由、内存管理等子系统的基础。
 */

/*
 * cpu_bit_bitmap[]：一个"压缩"的特殊数据结构，
 * 表示所有 NR_CPUS 个"只有第 N 位为 1"的 bitmap 值。
 * cpumask_of(cpu) 用它来获取单个 CPU 的常量 mask 地址，
 * 避免每次都在栈上分配临时 mask。
 */
/* cpu_bit_bitmap[0] 是空的，作为越界保护（可以向前回退到它） */
#define MASK_DECLARE_1(x)	[x+1][0] = (1UL << (x))
#define MASK_DECLARE_2(x)	MASK_DECLARE_1(x), MASK_DECLARE_1(x+1)
#define MASK_DECLARE_4(x)	MASK_DECLARE_2(x), MASK_DECLARE_2(x+2)
#define MASK_DECLARE_8(x)	MASK_DECLARE_4(x), MASK_DECLARE_4(x+4)

const unsigned long cpu_bit_bitmap[BITS_PER_LONG+1][BITS_TO_LONGS(NR_CPUS)] = {

	MASK_DECLARE_8(0),	MASK_DECLARE_8(8),
	MASK_DECLARE_8(16),	MASK_DECLARE_8(24),
#if BITS_PER_LONG > 32
	MASK_DECLARE_8(32),	MASK_DECLARE_8(40),
	MASK_DECLARE_8(48),	MASK_DECLARE_8(56),
#endif
};
EXPORT_SYMBOL_GPL(cpu_bit_bitmap);

const DECLARE_BITMAP(cpu_all_bits, NR_CPUS) = CPU_BITS_ALL;
EXPORT_SYMBOL(cpu_all_bits);

#ifdef CONFIG_INIT_ALL_POSSIBLE
struct cpumask __cpu_possible_mask __ro_after_init
	= {CPU_BITS_ALL};
unsigned int __num_possible_cpus __ro_after_init = NR_CPUS;
#else
struct cpumask __cpu_possible_mask __ro_after_init;
unsigned int __num_possible_cpus __ro_after_init;
#endif
EXPORT_SYMBOL(__cpu_possible_mask);
EXPORT_SYMBOL(__num_possible_cpus);

struct cpumask __cpu_online_mask __read_mostly;
EXPORT_SYMBOL(__cpu_online_mask);

struct cpumask __cpu_enabled_mask __read_mostly;
EXPORT_SYMBOL(__cpu_enabled_mask);

struct cpumask __cpu_present_mask __read_mostly;
EXPORT_SYMBOL(__cpu_present_mask);

struct cpumask __cpu_active_mask __read_mostly;
EXPORT_SYMBOL(__cpu_active_mask);

struct cpumask __cpu_dying_mask __read_mostly;
EXPORT_SYMBOL(__cpu_dying_mask);

atomic_t __num_online_cpus __read_mostly;
EXPORT_SYMBOL(__num_online_cpus);

void init_cpu_present(const struct cpumask *src)
{
	cpumask_copy(&__cpu_present_mask, src);
}

void init_cpu_possible(const struct cpumask *src)
{
	cpumask_copy(&__cpu_possible_mask, src);
	__num_possible_cpus = cpumask_weight(&__cpu_possible_mask);
}

/*
 * set_cpu_online()：更新 cpu_online_mask 并维护在线 CPU 计数。
 *
 * 使用 atomic_inc/dec 的原因：
 * reboot 和 kexec 代码会在 IPI/NMI 广播中调用此函数关闭 CPU，
 * 此时没有正常的热插拔序列化保护，必须用原子操作保证计数安全。
 *
 * 注意：__num_online_cpus 是 atomic_t 不代表读者是线程安全的，
 * 未通过热插拔锁序列化的读者仍可能看到不一致的状态。
 */
void set_cpu_online(unsigned int cpu, bool online)
{
	/*
	 * 需要 atomic_inc/dec() 来处理 reboot/kexec 代码对此函数的特殊调用，
	 * 它们在 IPI/NMI 广播中关闭 CPU 时调用此函数，没有锁保护。
	 * 正常热插拔路径是有锁序列化的。
	 */
	if (online) {
		if (!cpumask_test_and_set_cpu(cpu, &__cpu_online_mask))
			atomic_inc(&__num_online_cpus);
	} else {
		if (cpumask_test_and_clear_cpu(cpu, &__cpu_online_mask))
			atomic_dec(&__num_online_cpus);
	}
}

/*
 * This should be marked __init, but there is a boatload of call sites
 * which need to be fixed up to do so. Sigh...
 */
void set_cpu_possible(unsigned int cpu, bool possible)
{
	if (possible) {
		if (!cpumask_test_and_set_cpu(cpu, &__cpu_possible_mask))
			__num_possible_cpus++;
	} else {
		if (cpumask_test_and_clear_cpu(cpu, &__cpu_possible_mask))
			__num_possible_cpus--;
	}
}

/*
 * boot_cpu_init()：把 boot CPU 的所有状态标记为已激活。
 *
 * 在 start_kernel() 最早期调用，此时只有 CPU0 在运行。
 * 把 CPU0 同时加入 possible/present/online/active 四个 mask，
 * 使内核的各子系统从一开始就能正常看到 boot CPU。
 *
 * 这是所有 CPU mask 操作的起点：boot CPU 先进入，
 * 其他 CPU 在 smp_init() 后陆续加入 online/active mask。
 */
/*
	四个状态来自 Linux CPU mask 体系，每个 mask 回答一个不同问题，缺一不可：

	---
	四个 mask 的语义

	┌──────────┬──────────────────────────────┬─────────────────────────────────────────────────────────────────────────────────────┐
	│   mask   │             问题             │                                        含义                                         │
	├──────────┼──────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────────┤
	│ possible │ 这个 CPU                     │ 内核为其分配 per-cpu 变量、数据结构。不在此 mask 中的 CPU                                  │
	│          │ 编号将来可能存在吗？            │ 编号，内核连内存都不会分配                                                               │
	├──────────┼──────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────────┤
	│ present  │ 这个 CPU 硬件上存在吗？         │ 硬件已插入/已被固件报告。ACPI/设备树解析后设置，热插拔时动态变化                              │
	├──────────┼──────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────────┤
	│ online   │ 这个 CPU 正在运行调度器吗？      │ 调度器将任务分配到此 CPU，可以处理中断。cpu_online_mask 是调度的核心依据                     │
	├──────────┼──────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────────┤
	│ active   │ 这个 CPU                     │ 负载均衡时的迁移目标集合。CPU 下线前先从 active 移除，让任务提前迁走，再退出 online            │
	│          │ 可以接受任务迁移吗？            │                                                                                     │
	└──────────┴──────────────────────────────┴─────────────────────────────────────────────────────────────────────────────────────┘

	---
	为什么 boot CPU 要同时设置四个

	这四个状态在正常 CPU 上线流程中是依次经过的：

	possible（内核初始化时） → present（硬件发现时） → online（CPU 启动后） → active（调度就绪后）

	但 boot CPU 是内核本身跑起来的那个 CPU，它跳过了所有上线流程，直接就在运行了。
	如果不手动补齐四个状态，各子系统会认为它不存在：

	- 不设 possible：per-cpu 变量未分配，访问直接崩溃
	- 不设 present：固件/ACPI 层认为该 CPU 不存在
	- 不设 online：调度器不向它分配任务，中断无处投递
	- 不设 active：负载均衡跳过它，任务永远无法迁移到 boot CPU

	所以 boot_cpu_init() 本质上是代替正常上线流程，一次性补齐所有状态，让内核其余部分从启动第一刻起就能正确看到 boot CPU。

*/
void __init boot_cpu_init(void)
{
	int cpu = smp_processor_id(); /* 获取当前（boot）CPU 的编号 */

	/* 把 boot CPU 标记为 present/online/active/possible */
	set_cpu_online(cpu, true);
	set_cpu_active(cpu, true);
	set_cpu_present(cpu, true);
	set_cpu_possible(cpu, true);

#ifdef CONFIG_SMP
	__boot_cpu_id = cpu; /* 记录 boot CPU ID，供其他代码查询 */
#endif
}

/*
 * boot_cpu_hotplug_init()：初始化 boot CPU 的热插拔状态。
 *
 * 必须在 per_cpu 区域建立之后调用（setup_per_cpu_areas() 之后）。
 * 把 boot CPU 的热插拔状态设置为完全 ONLINE，
 * 并把它加入 cpus_booted_once_mask（记录曾经启动过的 CPU，
 * 用于 x86 的 SMT 处理：CR4.MCE 位必须在每个 CPU 启动时设置一次）。
 */
/*
	boot_cpu_hotplug_init 只是补记状态

	boot CPU 启动时跳过了整个状态机流程（它是第一个 CPU，状态机框架还没建好），直接就在运行了。

	这里做的事是：

	"boot CPU 已经在 ONLINE 状态了，我把这个事实记录下来"

	相当于给一个已经开着的灯补贴一张"已开"的标签，而不是按了开关。
*/
void __init boot_cpu_hotplug_init(void)
{
#ifdef CONFIG_SMP
	/* 记录 boot CPU 已经启动过（x86 SMT 处理需要） */
	cpumask_set_cpu(smp_processor_id(), &cpus_booted_once_mask);
	/* boot CPU 的同步状态设为 ONLINE（已完全上线） */
	atomic_set(this_cpu_ptr(&cpuhp_state.ap_sync_state), SYNC_STATE_ONLINE);
#endif
	/* boot CPU 的状态和目标都设为 CPUHP_ONLINE（最高状态,CPU 完全上线） */
	this_cpu_write(cpuhp_state.state, CPUHP_ONLINE);
	this_cpu_write(cpuhp_state.target, CPUHP_ONLINE);
}

/*
 * ══════════════════════════════════════════════════════════════
 * CPU 安全漏洞缓解措施（mitigations）
 * ══════════════════════════════════════════════════════════════
 *
 * 应对 Spectre/Meltdown/MDS 等推测执行漏洞的内核层缓解机制。
 * 通过 mitigations= 内核命令行参数控制：
 *   mitigations=off       → 关闭所有缓解（性能最佳，安全性最低）
 *   mitigations=auto      → 启用所有缓解（默认）
 *   mitigations=auto,nosmt → 启用缓解并禁用 SMT（超线程）
 *
 * 攻击向量分类（attack_vectors[]）：
 *   USER_KERNEL：用户态攻击内核（Meltdown）
 *   USER_USER：  用户进程之间的攻击（Spectre v1/v2）
 *   GUEST_HOST： 虚拟机攻击宿主机（KVM 相关）
 *   GUEST_GUEST：虚拟机之间的攻击（KVM 相关）
 *   CROSS_THREAD：同一物理核心的超线程之间的攻击（MDS/TAA）
 *
 * 跨线程攻击（CROSS_THREAD）默认不完全缓解，因为需要禁用 SMT，
 * 对性能影响很大（通常需要减少约 30% 的计算能力）。
 */
#ifdef CONFIG_CPU_MITIGATIONS
/* 各攻击向量的缓解开关，__ro_after_init 确保启动后不可修改 */
static bool attack_vectors[NR_CPU_ATTACK_VECTORS] __ro_after_init = {
	[CPU_MITIGATE_USER_KERNEL] = true,
	[CPU_MITIGATE_USER_USER] = true,
	[CPU_MITIGATE_GUEST_HOST] = IS_ENABLED(CONFIG_KVM),
	[CPU_MITIGATE_GUEST_GUEST] = IS_ENABLED(CONFIG_KVM),
};

bool cpu_attack_vector_mitigated(enum cpu_attack_vectors v)
{
	if (v < NR_CPU_ATTACK_VECTORS)
		return attack_vectors[v];

	WARN_ONCE(1, "Invalid attack vector %d\n", v);
	return false;
}

/*
 * There are 3 global options, 'off', 'auto', 'auto,nosmt'. These may optionally
 * be combined with attack-vector disables which follow them.
 *
 * Examples:
 *   mitigations=auto,no_user_kernel,no_user_user,no_cross_thread
 *   mitigations=auto,nosmt,no_guest_host,no_guest_guest
 *
 * mitigations=off is equivalent to disabling all attack vectors.
 */
enum cpu_mitigations {
	CPU_MITIGATIONS_OFF,
	CPU_MITIGATIONS_AUTO,
	CPU_MITIGATIONS_AUTO_NOSMT,
};

enum {
	NO_USER_KERNEL,
	NO_USER_USER,
	NO_GUEST_HOST,
	NO_GUEST_GUEST,
	NO_CROSS_THREAD,
	NR_VECTOR_PARAMS,
};

enum smt_mitigations smt_mitigations __ro_after_init = SMT_MITIGATIONS_AUTO;
static enum cpu_mitigations cpu_mitigations __ro_after_init = CPU_MITIGATIONS_AUTO;

static const match_table_t global_mitigations = {
	{ CPU_MITIGATIONS_AUTO_NOSMT,	"auto,nosmt"},
	{ CPU_MITIGATIONS_AUTO,		"auto"},
	{ CPU_MITIGATIONS_OFF,		"off"},
};

static const match_table_t vector_mitigations = {
	{ NO_USER_KERNEL,	"no_user_kernel"},
	{ NO_USER_USER,		"no_user_user"},
	{ NO_GUEST_HOST,	"no_guest_host"},
	{ NO_GUEST_GUEST,	"no_guest_guest"},
	{ NO_CROSS_THREAD,	"no_cross_thread"},
	{ NR_VECTOR_PARAMS,	NULL},
};

static int __init mitigations_parse_global_opt(char *arg)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(global_mitigations); i++) {
		const char *pattern = global_mitigations[i].pattern;

		if (!strncmp(arg, pattern, strlen(pattern))) {
			cpu_mitigations = global_mitigations[i].token;
			return strlen(pattern);
		}
	}

	return 0;
}

/*
 * mitigations_parse_cmdline()：解析 mitigations= 内核命令行参数。
 *
 * 解析格式：mitigations=<全局选项>[,<攻击向量禁用>...]
 * 例如：mitigations=auto,no_user_kernel,no_guest_host
 *
 * 全局选项处理完后，如果有逗号分隔的额外选项，
 * 依次禁用对应的攻击向量缓解。
 */
static int __init mitigations_parse_cmdline(char *arg)
{
	char *s, *p;
	int len;

	len = mitigations_parse_global_opt(arg);

	if (cpu_mitigations_off()) {
		/* mitigations=off：清空所有攻击向量缓解，同时禁用 SMT 缓解 */
		memset(attack_vectors, 0, sizeof(attack_vectors));
		smt_mitigations = SMT_MITIGATIONS_OFF;
	} else if (cpu_mitigations_auto_nosmt()) {
		/* mitigations=auto,nosmt：启用 SMT 缓解（可能会禁用超线程） */
		smt_mitigations = SMT_MITIGATIONS_ON;
	}

	p = arg + len;

	if (!*p)
		return 0;

	/* Attack vector controls may come after the ',' */
	if (*p++ != ',' || !IS_ENABLED(CONFIG_ARCH_HAS_CPU_ATTACK_VECTORS)) {
		pr_crit("Unsupported mitigations=%s, system may still be vulnerable\n",	arg);
		return 0;
	}

	while ((s = strsep(&p, ",")) != NULL) {
		switch (match_token(s, vector_mitigations, NULL)) {
		case NO_USER_KERNEL:
			attack_vectors[CPU_MITIGATE_USER_KERNEL] = false;
			break;
		case NO_USER_USER:
			attack_vectors[CPU_MITIGATE_USER_USER] = false;
			break;
		case NO_GUEST_HOST:
			attack_vectors[CPU_MITIGATE_GUEST_HOST] = false;
			break;
		case NO_GUEST_GUEST:
			attack_vectors[CPU_MITIGATE_GUEST_GUEST] = false;
			break;
		case NO_CROSS_THREAD:
			smt_mitigations = SMT_MITIGATIONS_OFF;
			break;
		default:
			pr_crit("Unsupported mitigations options %s\n",	s);
			return 0;
		}
	}

	return 0;
}

/* mitigations=off */
bool cpu_mitigations_off(void)
{
	return cpu_mitigations == CPU_MITIGATIONS_OFF;
}
EXPORT_SYMBOL_GPL(cpu_mitigations_off);

/* mitigations=auto,nosmt */
bool cpu_mitigations_auto_nosmt(void)
{
	return cpu_mitigations == CPU_MITIGATIONS_AUTO_NOSMT;
}
EXPORT_SYMBOL_GPL(cpu_mitigations_auto_nosmt);
#else
static int __init mitigations_parse_cmdline(char *arg)
{
	pr_crit("Kernel compiled without mitigations, ignoring 'mitigations'; system may still be vulnerable\n");
	return 0;
}
#endif
early_param("mitigations", mitigations_parse_cmdline);
