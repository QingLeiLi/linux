// SPDX-License-Identifier: GPL-2.0+
/*
 * locktorture 是模块化锁压力测试器：模块参数选择一种 lock_torture_ops，统一的
 * writer/reader kthread 再经该操作表反复取锁、制造临界区延迟、校验互斥关系并
 * 统计吞吐/失败。测试覆盖自旋锁、mutex、ww_mutex、rtmutex、rwsem 和
 * percpu-rwsem；可叠加 CPU 热插拔、任务迁移、周期停顿、RT 提升与 RCU stall
 * 压力。这里的 busted 类型是故意失效的对照组，绝不能当作锁实现范例。
 */
/*
 * Module-based torture test facility for locking
 *
 * Copyright (C) IBM Corporation, 2014
 *
 * Authors: Paul E. McKenney <paulmck@linux.ibm.com>
 *          Davidlohr Bueso <dave@stgolabs.net>
 *	Based on kernel/rcu/torture.c.
 */

#define pr_fmt(fmt) fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/sched/rt.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include <linux/rtmutex.h>
#include <linux/atomic.h>
#include <linux/moduleparam.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/torture.h>
#include <linux/reboot.h>

MODULE_DESCRIPTION("torture test facility for locking");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Paul E. McKenney <paulmck@linux.ibm.com>");

torture_param(int, acq_writer_lim, 0, "Write_acquisition time limit (jiffies).");
torture_param(int, call_rcu_chains, 0, "Self-propagate call_rcu() chains during test (0=disable).");
torture_param(int, long_hold, 100, "Do occasional long hold of lock (ms), 0=disable");
torture_param(int, nested_locks, 0, "Number of nested locks (max = 8)");
torture_param(int, nreaders_stress, -1, "Number of read-locking stress-test threads");
torture_param(int, nwriters_stress, -1, "Number of write-locking stress-test threads");
torture_param(int, onoff_holdoff, 0, "Time after boot before CPU hotplugs (s)");
torture_param(int, onoff_interval, 0, "Time between CPU hotplugs (s), 0=disable");
torture_param(int, rt_boost, 2,
		   "Do periodic rt-boost. 0=Disable, 1=Only for rt_mutex, 2=For all lock types.");
torture_param(int, rt_boost_factor, 50, "A factor determining how often rt-boost happens.");
torture_param(int, shuffle_interval, 3, "Number of jiffies between shuffles, 0=disable");
torture_param(int, shutdown_secs, 0, "Shutdown time (j), <= zero to disable.");
torture_param(int, stat_interval, 60, "Number of seconds between stats printk()s");
torture_param(int, stutter, 5, "Number of jiffies to run/halt test, 0=disable");
torture_param(int, verbose, 1, "Enable verbose debugging printk()s");
torture_param(int, writer_fifo, 0, "Run writers at sched_set_fifo() priority");

/* 更高的嵌套深度容易超过 lockdep 的最大锁链长度并触发诊断。 */
/* Going much higher trips "BUG: MAX_LOCKDEP_CHAIN_HLOCKS too low!" errors */
#define MAX_NESTED_LOCKS 8

static char *torture_type = IS_ENABLED(CONFIG_PREEMPT_RT) ? "raw_spin_lock" : "spin_lock";
module_param(torture_type, charp, 0444);
MODULE_PARM_DESC(torture_type,
		 "Type of lock to torture (spin_lock, spin_lock_irq, mutex_lock, ...)");

/* 将 reader 线程绑定到用户指定的 CPU 集合。 */
static cpumask_var_t bind_readers; // Bind the readers to the specified set of CPUs.
/* 将 writer 线程绑定到用户指定的 CPU 集合。 */
static cpumask_var_t bind_writers; // Bind the writers to the specified set of CPUs.

/*
 * 解析 cpulist 模块参数；若未来出现更多调用者，可再下沉为通用帮助函数。
 * 成功分配并解析后返回 0。分配或语法失败时打印警告、把掩码回退为全部 CPU，
 * 同时保留 -ENOMEM/解析错误返回值，让模块初始化进入 unwind。
 */
// Parse a cpumask kernel parameter.  If there are more users later on,
// this might need to got to a more central location.
/* 解析并保存 CPU 集合；错误时回退为全 CPU 掩码并返回原错误码。 */
static int param_set_cpumask(const char *val, const struct kernel_param *kp)
{
	cpumask_var_t *cm_bind = kp->arg;
	int ret;
	char *s;

	if (!alloc_cpumask_var(cm_bind, GFP_KERNEL)) {
		s = "Out of memory";
		ret = -ENOMEM;
		goto out_err;
	}
	ret = cpulist_parse(val, *cm_bind);
	if (!ret)
		return ret;
	s = "Bad CPU range";
out_err:
	pr_warn("%s: %s, all CPUs set\n", kp->name, s);
	cpumask_setall(*cm_bind);
	return ret;
}

/* 把 kp 指向的 CPU 掩码按 cpulist 格式写入 sysfs 参数缓冲区并返回字符数。 */
// Output a cpumask kernel parameter.
/* 输出 CPU 集合的 cpulist 文本并返回写入长度。 */
static int param_get_cpumask(char *buffer, const struct kernel_param *kp)
{
	cpumask_var_t *cm_bind = kp->arg;

	return sprintf(buffer, "%*pbl", cpumask_pr_args(*cm_bind));
}

/* 仅当动态 cpumask 已成功分配且至少含一个 CPU 时返回 true。 */
static bool cpumask_nonempty(cpumask_var_t mask)
{
	return cpumask_available(mask) && !cpumask_empty(mask);
}

static const struct kernel_param_ops lt_bind_ops = {
	.set = param_set_cpumask,
	.get = param_get_cpumask,
};

module_param_cb(bind_readers, &lt_bind_ops, &bind_readers, 0444);
module_param_cb(bind_writers, &lt_bind_ops, &bind_writers, 0444);

long torture_sched_setaffinity(pid_t pid, const struct cpumask *in_mask, bool dowarn);

static struct task_struct *stats_task;
static struct task_struct **writer_tasks;
static struct task_struct **reader_tasks;

static bool lock_is_write_held;
static atomic_t lock_is_read_held;
static unsigned long last_lock_release;

struct lock_stress_stats {
	/* 发现互斥/读写排斥断言失败的次数。 */
	long n_lock_fail;
	/* 本线程完成的成功取锁次数。 */
	long n_lock_acquired;
};

/* 一个可自我续接的 RCU 回调链；crc_stop 以 release/acquire 协议终止续链。 */
struct call_rcu_chain {
	struct rcu_head crc_rh;
	bool crc_stop;
};
struct call_rcu_chain *call_rcu_chain_list;

/* 初始化失败也统一进入该清理入口。 */
/* Forward reference. */
static void lock_torture_cleanup(void);

/*
 * 操作向量把不同锁原语适配到统一测试循环。init/exit 管理类型专属资源；
 * nested_lock/unlock 生成锁链；write/read 三元组负责获取、临界区延迟和释放；
 * task_boost 改变调度优先级。flags 只保存 irqsave 测试的恢复值，name 用于选择。
 */
/*
 * Operations vector for selecting different types of tests.
 */
struct lock_torture_ops {
	void (*init)(void);
	void (*exit)(void);
	int (*nested_lock)(int tid, u32 lockset);
	int (*writelock)(int tid);
	void (*write_delay)(struct torture_random_state *trsp);
	void (*task_boost)(struct torture_random_state *trsp);
	void (*writeunlock)(int tid);
	void (*nested_unlock)(int tid, u32 lockset);
	int (*readlock)(int tid);
	void (*read_delay)(struct torture_random_state *trsp);
	void (*readunlock)(int tid);

	/* irq spinlock 适配器保存并在解锁时恢复的中断状态。 */
	unsigned long flags; /* for irq spinlocks */
	const char *name;
};

struct lock_torture_cxt {
	/* 参数归一化后的实际 writer/reader 线程数。 */
	int nrealwriters_stress;
	int nrealreaders_stress;
	/* 所选锁启用调试配置，以及类型 init 是否已经调用。 */
	bool debug_lock;
	bool init_called;
	/* 聚合断言/统计失败，cur_ops 指向当前唯一测试类型。 */
	atomic_t n_lock_torture_errors;
	struct lock_torture_ops *cur_ops;
	/* writer 统计数组。 */
	struct lock_stress_stats *lwsa; /* writer statistics */
	/* reader 统计数组。 */
	struct lock_stress_stats *lrsa; /* reader statistics */
};
static struct lock_torture_cxt cxt = { 0, 0, false, false,
				       ATOMIC_INIT(0),
				       NULL, NULL};
/* 以下定义提供各类锁的测试适配器。 */
/*
 * Definitions for lock torture testing.
 */

/* 故意不加锁且总报成功，用来验证测试器能否发现重复 writer；不可用于真实代码。 */
static int torture_lock_busted_write_lock(int tid __maybe_unused)
{
	/* 故意错误的对照实现，真实代码绝不能照用。 */
	return 0;  /* BUGGY, do not use in real life!!! */
}

/* busted 对照组的写临界区延迟：偶发长忙等制造重叠，并偶发主动让出处理器。 */
static void torture_lock_busted_write_delay(struct torture_random_state *trsp)
{
	/* 偶发长延迟用于强制形成大规模争用。 */
	/* We want a long delay occasionally to force massive contention.  */
	if (long_hold && !(torture_random(trsp) % (cxt.nrealwriters_stress * 2000 * long_hold)))
		mdelay(long_hold);
	if (!(torture_random(trsp) % (cxt.nrealwriters_stress * 20000)))
		/* 允许测试线程被抢占，扩大交错空间。 */
		torture_preempt_schedule();  /* Allow test to be preempted. */
}

/* busted 对照组故意不释放任何锁，因为对应获取也没有真正加锁。 */
static void torture_lock_busted_write_unlock(int tid __maybe_unused)
{
	  /* 故意错误的对照实现，真实代码绝不能照用。 */
	  /* BUGGY, do not use in real life!!! */
}

/*
 * 按随机频率在 SCHED_FIFO 与普通策略之间切换当前 writer，触发 rtmutex PI 链
 * 重算；trsp=NULL 是线程退出时的强制复位。rt_boost_factor 必须为正。
 */
static void __torture_rt_boost(struct torture_random_state *trsp)
{
	const unsigned int factor = rt_boost_factor;

	if (!rt_task(current)) {
		/*
		 * 平均每 rt_boost_factor 轮提升一次；随后取锁会让 rtmutex 按新优先级
		 * 更新等待队列并执行相应的优先级继承传播。
		 */
		/*
		 * Boost priority once every rt_boost_factor operations. When
		 * the task tries to take the lock, the rtmutex it will account
		 * for the new priority, and do any corresponding pi-dance.
		 */
		if (trsp && !(torture_random(trsp) %
			      (cxt.nrealwriters_stress * factor))) {
			sched_set_fifo(current);
		/* 常见的未命中路径不改变当前调度策略。 */
		} else /* common case, do nothing */
			return;
	} else {
		/*
		 * 提升后再维持约 10*factor 次操作再恢复；传入 NULL 时为停止 kthread
		 * 强制复位，不再依赖随机命中。
		 */
		/*
		 * The task will remain boosted for another 10 * rt_boost_factor
		 * operations, then restored back to its original prio, and so
		 * forth.
		 *
		 * When @trsp is nil, we want to force-reset the task for
		 * stopping the kthread.
		 */
		if (!trsp || !(torture_random(trsp) %
			       (cxt.nrealwriters_stress * factor * 2))) {
			sched_set_normal(current, 0);
		/* 常见的未命中路径继续保持当前 RT 提升。 */
		} else /* common case, do nothing */
			return;
	}
}

/* 仅当 rt_boost=2（所有锁类型）时执行通用 RT 优先级扰动。 */
static void torture_rt_boost(struct torture_random_state *trsp)
{
	if (rt_boost != 2)
		return;

	__torture_rt_boost(trsp);
}

static struct lock_torture_ops lock_busted_ops = {
	.writelock	= torture_lock_busted_write_lock,
	.write_delay	= torture_lock_busted_write_delay,
	.task_boost     = torture_rt_boost,
	.writeunlock	= torture_lock_busted_write_unlock,
	.readlock       = NULL,
	.read_delay     = NULL,
	.readunlock     = NULL,
	.name		= "lock_busted"
};

static DEFINE_SPINLOCK(torture_spinlock);

/* 获取普通 spinlock 写测试锁；tid 仅满足统一 ops 签名，成功固定返回 0。 */
static int torture_spin_lock_write_lock(int tid __maybe_unused)
__acquires(torture_spinlock)
{
	spin_lock(&torture_spinlock);
	return 0;
}

/* 在持有 spinlock 时以短忙等模拟常见临界区，并偶发长忙等制造强争用。 */
static void torture_spin_lock_write_delay(struct torture_random_state *trsp)
{
	const unsigned long shortdelay_us = 2;
	unsigned long j;

	/* 多数轮次只短延迟，少数轮次长延迟以形成大规模争用。 */
	/* We want a short delay mostly to emulate likely code, and
	 * we want a long delay occasionally to force massive contention.
	 */
	if (long_hold && !(torture_random(trsp) % (cxt.nrealwriters_stress * 2000 * long_hold))) {
		j = jiffies;
		mdelay(long_hold);
		pr_alert("%s: delay = %lu jiffies.\n", __func__, jiffies - j);
	}
	if (!(torture_random(trsp) % (cxt.nrealwriters_stress * 200 * shortdelay_us)))
		udelay(shortdelay_us);
	if (!(torture_random(trsp) % (cxt.nrealwriters_stress * 20000)))
		/* 扩大测试交错，允许当前测试线程被抢占。 */
		torture_preempt_schedule();  /* Allow test to be preempted. */
}

/* 释放普通 spinlock；与 write_lock 适配器严格成对。 */
static void torture_spin_lock_write_unlock(int tid __maybe_unused)
__releases(torture_spinlock)
{
	spin_unlock(&torture_spinlock);
}

static struct lock_torture_ops spin_lock_ops = {
	.writelock	= torture_spin_lock_write_lock,
	.write_delay	= torture_spin_lock_write_delay,
	.task_boost     = torture_rt_boost,
	.writeunlock	= torture_spin_lock_write_unlock,
	.readlock       = NULL,
	.read_delay     = NULL,
	.readunlock     = NULL,
	.name		= "spin_lock"
};

/* 关闭本地 IRQ 并获取 spinlock，把 flags 保存到当前 ops 供配对释放。 */
static int torture_spin_lock_write_lock_irq(int tid __maybe_unused)
__acquires(torture_spinlock)
{
	unsigned long flags;

	spin_lock_irqsave(&torture_spinlock, flags);
	cxt.cur_ops->flags = flags;
	return 0;
}

/* 释放 irq spinlock，并用获取时保存在 ops 中的 flags 恢复本地 IRQ 状态。 */
static void torture_lock_spin_write_unlock_irq(int tid __maybe_unused)
__releases(torture_spinlock)
{
	spin_unlock_irqrestore(&torture_spinlock, cxt.cur_ops->flags);
}

static struct lock_torture_ops spin_lock_irq_ops = {
	.writelock	= torture_spin_lock_write_lock_irq,
	.write_delay	= torture_spin_lock_write_delay,
	.task_boost     = torture_rt_boost,
	.writeunlock	= torture_lock_spin_write_unlock_irq,
	.readlock       = NULL,
	.read_delay     = NULL,
	.readunlock     = NULL,
	.name		= "spin_lock_irq"
};

static DEFINE_RAW_SPINLOCK(torture_raw_spinlock);

/* 获取不受 PREEMPT_RT 语义转换影响的 raw spinlock，成功固定返回 0。 */
static int torture_raw_spin_lock_write_lock(int tid __maybe_unused)
__acquires(torture_raw_spinlock)
{
	raw_spin_lock(&torture_raw_spinlock);
	return 0;
}

/* 释放 raw spinlock；临界区延迟复用普通 spinlock 压力模型。 */
static void torture_raw_spin_lock_write_unlock(int tid __maybe_unused)
__releases(torture_raw_spinlock)
{
	raw_spin_unlock(&torture_raw_spinlock);
}

static struct lock_torture_ops raw_spin_lock_ops = {
	.writelock	= torture_raw_spin_lock_write_lock,
	.write_delay	= torture_spin_lock_write_delay,
	.task_boost	= torture_rt_boost,
	.writeunlock	= torture_raw_spin_lock_write_unlock,
	.readlock	= NULL,
	.read_delay	= NULL,
	.readunlock	= NULL,
	.name		= "raw_spin_lock"
};

/* 关闭本地 IRQ 并获取 raw spinlock，保存 flags 供唯一持有者配对恢复。 */
static int torture_raw_spin_lock_write_lock_irq(int tid __maybe_unused)
__acquires(torture_raw_spinlock)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&torture_raw_spinlock, flags);
	cxt.cur_ops->flags = flags;
	return 0;
}

/* 释放 raw irq spinlock，并恢复获取前的本地 IRQ 状态。 */
static void torture_raw_spin_lock_write_unlock_irq(int tid __maybe_unused)
__releases(torture_raw_spinlock)
{
	raw_spin_unlock_irqrestore(&torture_raw_spinlock, cxt.cur_ops->flags);
}

static struct lock_torture_ops raw_spin_lock_irq_ops = {
	.writelock	= torture_raw_spin_lock_write_lock_irq,
	.write_delay	= torture_spin_lock_write_delay,
	.task_boost	= torture_rt_boost,
	.writeunlock	= torture_raw_spin_lock_write_unlock_irq,
	.readlock	= NULL,
	.read_delay	= NULL,
	.readunlock	= NULL,
	.name		= "raw_spin_lock_irq"
};

#ifdef CONFIG_BPF_SYSCALL

#include <asm/rqspinlock.h>
static rqspinlock_t rqspinlock;

/* 在 BPF 配置下获取 rqspinlock 测试对象，成功固定返回 0。 */
static int torture_raw_res_spin_write_lock(int tid __maybe_unused)
{
	raw_res_spin_lock(&rqspinlock);
	return 0;
}

/* 释放 rqspinlock 测试对象，与 raw_res 获取适配器成对。 */
static void torture_raw_res_spin_write_unlock(int tid __maybe_unused)
{
	raw_res_spin_unlock(&rqspinlock);
}

static struct lock_torture_ops raw_res_spin_lock_ops = {
	.writelock	= torture_raw_res_spin_write_lock,
	.write_delay	= torture_spin_lock_write_delay,
	.task_boost     = torture_rt_boost,
	.writeunlock	= torture_raw_res_spin_write_unlock,
	.readlock       = NULL,
	.read_delay     = NULL,
	.readunlock     = NULL,
	.name		= "raw_res_spin_lock"
};

/* 关闭本地 IRQ 后获取 rqspinlock，并保存 flags 供解锁恢复。 */
static int torture_raw_res_spin_write_lock_irq(int tid __maybe_unused)
{
	unsigned long flags;

	raw_res_spin_lock_irqsave(&rqspinlock, flags);
	cxt.cur_ops->flags = flags;
	return 0;
}

/* 释放 irqsave rqspinlock 并恢复进入测试临界区前的 IRQ 状态。 */
static void torture_raw_res_spin_write_unlock_irq(int tid __maybe_unused)
{
	raw_res_spin_unlock_irqrestore(&rqspinlock, cxt.cur_ops->flags);
}

static struct lock_torture_ops raw_res_spin_lock_irq_ops = {
	.writelock	= torture_raw_res_spin_write_lock_irq,
	.write_delay	= torture_spin_lock_write_delay,
	.task_boost     = torture_rt_boost,
	.writeunlock	= torture_raw_res_spin_write_unlock_irq,
	.readlock       = NULL,
	.read_delay     = NULL,
	.readunlock     = NULL,
	.name		= "raw_res_spin_lock_irq"
};

#endif

static DEFINE_RWLOCK(torture_rwlock);

/* 获取 rwlock 的写侧独占锁，成功固定返回 0。 */
static int torture_rwlock_write_lock(int tid __maybe_unused)
__acquires(torture_rwlock)
{
	write_lock(&torture_rwlock);
	return 0;
}

/* 写持锁期间短忙等模拟常见路径，偶发 long_hold 忙等扩大 writer/reader 争用。 */
static void torture_rwlock_write_delay(struct torture_random_state *trsp)
{
	const unsigned long shortdelay_us = 2;

	/* 多数轮次短延迟模拟常见代码，少数轮次长延迟强制形成大规模争用。 */
	/* We want a short delay mostly to emulate likely code, and
	 * we want a long delay occasionally to force massive contention.
	 */
	if (long_hold && !(torture_random(trsp) % (cxt.nrealwriters_stress * 2000 * long_hold)))
		mdelay(long_hold);
	else
		udelay(shortdelay_us);
}

/* 释放 rwlock 写锁，结束独占临界区。 */
static void torture_rwlock_write_unlock(int tid __maybe_unused)
__releases(torture_rwlock)
{
	write_unlock(&torture_rwlock);
}

/* 获取 rwlock 读锁；多个 reader 可并行，成功固定返回 0。 */
static int torture_rwlock_read_lock(int tid __maybe_unused)
__acquires(torture_rwlock)
{
	read_lock(&torture_rwlock);
	return 0;
}

/* 读持锁期间通常短忙等，偶发 long_hold 忙等以扩大读写交错窗口。 */
static void torture_rwlock_read_delay(struct torture_random_state *trsp)
{
	const unsigned long shortdelay_us = 10;

	/* 多数轮次短延迟模拟常见代码，少数轮次长延迟强制形成大规模争用。 */
	/* We want a short delay mostly to emulate likely code, and
	 * we want a long delay occasionally to force massive contention.
	 */
	if (long_hold && !(torture_random(trsp) % (cxt.nrealreaders_stress * 2000 * long_hold)))
		mdelay(long_hold);
	else
		udelay(shortdelay_us);
}

/* 释放 rwlock 读锁，并允许等待的 writer 继续竞争。 */
static void torture_rwlock_read_unlock(int tid __maybe_unused)
__releases(torture_rwlock)
{
	read_unlock(&torture_rwlock);
}

static struct lock_torture_ops rw_lock_ops = {
	.writelock	= torture_rwlock_write_lock,
	.write_delay	= torture_rwlock_write_delay,
	.task_boost     = torture_rt_boost,
	.writeunlock	= torture_rwlock_write_unlock,
	.readlock       = torture_rwlock_read_lock,
	.read_delay     = torture_rwlock_read_delay,
	.readunlock     = torture_rwlock_read_unlock,
	.name		= "rw_lock"
};

/* 关闭本地 IRQ 并获取 rwlock 写锁，保存 flags 供配对解锁恢复。 */
static int torture_rwlock_write_lock_irq(int tid __maybe_unused)
__acquires(torture_rwlock)
{
	unsigned long flags;

	write_lock_irqsave(&torture_rwlock, flags);
	cxt.cur_ops->flags = flags;
	return 0;
}

/* 释放 irqsave rwlock 写锁，并恢复获取前的本地 IRQ 状态。 */
static void torture_rwlock_write_unlock_irq(int tid __maybe_unused)
__releases(torture_rwlock)
{
	write_unlock_irqrestore(&torture_rwlock, cxt.cur_ops->flags);
}

/* 关闭本地 IRQ 并获取 rwlock 读锁，保存 flags 供配对解锁恢复。 */
static int torture_rwlock_read_lock_irq(int tid __maybe_unused)
__acquires(torture_rwlock)
{
	unsigned long flags;

	read_lock_irqsave(&torture_rwlock, flags);
	cxt.cur_ops->flags = flags;
	return 0;
}

/* 释放 irqsave rwlock 读锁，并恢复获取前的本地 IRQ 状态。 */
static void torture_rwlock_read_unlock_irq(int tid __maybe_unused)
__releases(torture_rwlock)
{
	read_unlock_irqrestore(&torture_rwlock, cxt.cur_ops->flags);
}

static struct lock_torture_ops rw_lock_irq_ops = {
	.writelock	= torture_rwlock_write_lock_irq,
	.write_delay	= torture_rwlock_write_delay,
	.task_boost     = torture_rt_boost,
	.writeunlock	= torture_rwlock_write_unlock_irq,
	.readlock       = torture_rwlock_read_lock_irq,
	.read_delay     = torture_rwlock_read_delay,
	.readunlock     = torture_rwlock_read_unlock_irq,
	.name		= "rw_lock_irq"
};

static DEFINE_MUTEX(torture_mutex);
static struct mutex torture_nested_mutexes[MAX_NESTED_LOCKS];
static struct lock_class_key nested_mutex_keys[MAX_NESTED_LOCKS];

/* 初始化每一把嵌套 mutex，并为各层提供独立 lockdep class key。 */
static void torture_mutex_init(void)
{
	int i;

	for (i = 0; i < MAX_NESTED_LOCKS; i++)
		__mutex_init(&torture_nested_mutexes[i], __func__,
			     &nested_mutex_keys[i]);
}

/* 按 lockset 低位从低到高获取所选嵌套 mutex；返回时这些锁仍由当前线程持有。 */
static int torture_mutex_nested_lock(int tid __maybe_unused,
				     u32 lockset)
{
	int i;

	for (i = 0; i < nested_locks; i++)
		if (lockset & (1 << i))
			mutex_lock(&torture_nested_mutexes[i]);
	return 0;
}

/* 获取主 torture mutex，成功固定返回 0。 */
static int torture_mutex_lock(int tid __maybe_unused)
__acquires(torture_mutex)
{
	mutex_lock(&torture_mutex);
	return 0;
}

/* mutex 持锁期间偶发长忙等和主动让出，制造可睡眠锁的激烈竞争。 */
static void torture_mutex_delay(struct torture_random_state *trsp)
{
	/* 偶发长延迟用于强制形成大规模争用。 */
	/* We want a long delay occasionally to force massive contention.  */
	if (long_hold && !(torture_random(trsp) % (cxt.nrealwriters_stress * 2000 * long_hold)))
		mdelay(long_hold * 5);
	if (!(torture_random(trsp) % (cxt.nrealwriters_stress * 20000)))
		/* 允许测试线程被抢占，扩大调度交错。 */
		torture_preempt_schedule();  /* Allow test to be preempted. */
}

/* 释放主 torture mutex。 */
static void torture_mutex_unlock(int tid __maybe_unused)
__releases(torture_mutex)
{
	mutex_unlock(&torture_mutex);
}

/* 按获取的逆序释放 lockset 选中的嵌套 mutex，维持正常锁层级。 */
static void torture_mutex_nested_unlock(int tid __maybe_unused,
					u32 lockset)
{
	int i;

	for (i = nested_locks - 1; i >= 0; i--)
		if (lockset & (1 << i))
			mutex_unlock(&torture_nested_mutexes[i]);
}

static struct lock_torture_ops mutex_lock_ops = {
	.init		= torture_mutex_init,
	.nested_lock	= torture_mutex_nested_lock,
	.writelock	= torture_mutex_lock,
	.write_delay	= torture_mutex_delay,
	.task_boost     = torture_rt_boost,
	.writeunlock	= torture_mutex_unlock,
	.nested_unlock	= torture_mutex_nested_unlock,
	.readlock       = NULL,
	.read_delay     = NULL,
	.readunlock     = NULL,
	.name		= "mutex_lock"
};

#include <linux/ww_mutex.h>
/*
 * 三把测试 ww_mutex 必须与 torture_ww_class 属于同一 ww class，避免 lockdep
 * 把伤口等待协议误判为普通锁序反转；使用 ww_mutex_init() 保证这一点。
 */
/*
 * The torture ww_mutexes should belong to the same lock class as
 * torture_ww_class to avoid lockdep problem. The ww_mutex_init()
 * function is called for initialization to ensure that.
 */
static DEFINE_WD_CLASS(torture_ww_class);
static struct ww_mutex torture_ww_mutex_0, torture_ww_mutex_1, torture_ww_mutex_2;
static struct ww_acquire_ctx *ww_acquire_ctxs;

/* 初始化三把同类 ww_mutex，并为每个 writer 分配独立 acquire context。 */
static void torture_ww_mutex_init(void)
{
	ww_mutex_init(&torture_ww_mutex_0, &torture_ww_class);
	ww_mutex_init(&torture_ww_mutex_1, &torture_ww_class);
	ww_mutex_init(&torture_ww_mutex_2, &torture_ww_class);

	ww_acquire_ctxs = kmalloc_objs(*ww_acquire_ctxs,
				       cxt.nrealwriters_stress);
	if (!ww_acquire_ctxs)
		VERBOSE_TOROUT_STRING("ww_acquire_ctx: Out of memory");
}

/* 释放 writer acquire-context 数组；三把静态 ww_mutex 无动态对象可释放。 */
static void torture_ww_mutex_exit(void)
{
	kfree(ww_acquire_ctxs);
}

/*
 * 用 tid 对应的 ww_acquire_ctx 获取三把锁。遇到 -EDEADLK 时逆序释放已经取得的
 * 锁，对冲突锁执行 slow 获取并移到列表前端，再继续重放剩余顺序；其他错误
 * 原样返回。成功返回 0，三把锁均保持持有，context 留给 unlock 完成。
 */
static int torture_ww_mutex_lock(int tid)
__acquires(torture_ww_mutex_0)
__acquires(torture_ww_mutex_1)
__acquires(torture_ww_mutex_2)
{
	LIST_HEAD(list);
	struct reorder_lock {
		struct list_head link;
		struct ww_mutex *lock;
	} locks[3], *ll, *ln;
	struct ww_acquire_ctx *ctx = &ww_acquire_ctxs[tid];

	locks[0].lock = &torture_ww_mutex_0;
	list_add(&locks[0].link, &list);

	locks[1].lock = &torture_ww_mutex_1;
	list_add(&locks[1].link, &list);

	locks[2].lock = &torture_ww_mutex_2;
	list_add(&locks[2].link, &list);

	ww_acquire_init(ctx, &torture_ww_class);

	list_for_each_entry(ll, &list, link) {
		int err;

		err = ww_mutex_lock(ll->lock, ctx);
		if (!err)
			continue;

		ln = ll;
		list_for_each_entry_continue_reverse(ln, &list, link)
			ww_mutex_unlock(ln->lock);

		if (err != -EDEADLK)
			return err;

		ww_mutex_lock_slow(ll->lock, ctx);
		list_move(&ll->link, &list);
	}

	return 0;
}

/* 释放三把 ww_mutex 并结束 tid 对应的 acquire context。 */
static void torture_ww_mutex_unlock(int tid)
__releases(torture_ww_mutex_0)
__releases(torture_ww_mutex_1)
__releases(torture_ww_mutex_2)
{
	struct ww_acquire_ctx *ctx = &ww_acquire_ctxs[tid];

	ww_mutex_unlock(&torture_ww_mutex_0);
	ww_mutex_unlock(&torture_ww_mutex_1);
	ww_mutex_unlock(&torture_ww_mutex_2);
	ww_acquire_fini(ctx);
}

static struct lock_torture_ops ww_mutex_lock_ops = {
	.init		= torture_ww_mutex_init,
	.exit		= torture_ww_mutex_exit,
	.writelock	= torture_ww_mutex_lock,
	.write_delay	= torture_mutex_delay,
	.task_boost     = torture_rt_boost,
	.writeunlock	= torture_ww_mutex_unlock,
	.readlock       = NULL,
	.read_delay     = NULL,
	.readunlock     = NULL,
	.name		= "ww_mutex_lock"
};

#ifdef CONFIG_RT_MUTEXES
static DEFINE_RT_MUTEX(torture_rtmutex);
static struct rt_mutex torture_nested_rtmutexes[MAX_NESTED_LOCKS];
static struct lock_class_key nested_rtmutex_keys[MAX_NESTED_LOCKS];

/* 初始化每层嵌套 rt_mutex，并赋予独立 lockdep class key。 */
static void torture_rtmutex_init(void)
{
	int i;

	for (i = 0; i < MAX_NESTED_LOCKS; i++)
		__rt_mutex_init(&torture_nested_rtmutexes[i], __func__,
				&nested_rtmutex_keys[i]);
}

/* 按 lockset 低位从低到高获取所选 rt_mutex，以生成可控 PI 锁链。 */
static int torture_rtmutex_nested_lock(int tid __maybe_unused,
				       u32 lockset)
{
	int i;

	for (i = 0; i < nested_locks; i++)
		if (lockset & (1 << i))
			rt_mutex_lock(&torture_nested_rtmutexes[i]);
	return 0;
}

/* 获取主 rt_mutex，成功固定返回 0；争用可触发优先级继承。 */
static int torture_rtmutex_lock(int tid __maybe_unused)
__acquires(torture_rtmutex)
{
	rt_mutex_lock(&torture_rtmutex);
	return 0;
}

/* rt_mutex 持锁期间混合短/长忙等和主动让出，以放大 PI 与调度交错。 */
static void torture_rtmutex_delay(struct torture_random_state *trsp)
{
	const unsigned long shortdelay_us = 2;

	/* 多数轮次短延迟模拟常见临界区，少数轮次长延迟制造大规模争用。 */
	/*
	 * We want a short delay mostly to emulate likely code, and
	 * we want a long delay occasionally to force massive contention.
	 */
	if (long_hold && !(torture_random(trsp) % (cxt.nrealwriters_stress * 2000 * long_hold)))
		mdelay(long_hold);
	if (!(torture_random(trsp) %
	      (cxt.nrealwriters_stress * 200 * shortdelay_us)))
		udelay(shortdelay_us);
	if (!(torture_random(trsp) % (cxt.nrealwriters_stress * 20000)))
		/* 允许测试线程被抢占，扩大 PI 状态转换的覆盖。 */
		torture_preempt_schedule();  /* Allow test to be preempted. */
}

/* 释放主 rt_mutex 并触发必要的 PI 去提升。 */
static void torture_rtmutex_unlock(int tid __maybe_unused)
__releases(torture_rtmutex)
{
	rt_mutex_unlock(&torture_rtmutex);
}

/* rt_boost 非零时对 rt_mutex 测试启用优先级扰动，包括模式 1 的专属启用。 */
static void torture_rt_boost_rtmutex(struct torture_random_state *trsp)
{
	if (!rt_boost)
		return;

	__torture_rt_boost(trsp);
}

/* 按与获取相反的高位到低位顺序释放 lockset 选中的嵌套 rt_mutex。 */
static void torture_rtmutex_nested_unlock(int tid __maybe_unused,
					  u32 lockset)
{
	int i;

	for (i = nested_locks - 1; i >= 0; i--)
		if (lockset & (1 << i))
			rt_mutex_unlock(&torture_nested_rtmutexes[i]);
}

static struct lock_torture_ops rtmutex_lock_ops = {
	.init		= torture_rtmutex_init,
	.nested_lock	= torture_rtmutex_nested_lock,
	.writelock	= torture_rtmutex_lock,
	.write_delay	= torture_rtmutex_delay,
	.task_boost     = torture_rt_boost_rtmutex,
	.writeunlock	= torture_rtmutex_unlock,
	.nested_unlock	= torture_rtmutex_nested_unlock,
	.readlock       = NULL,
	.read_delay     = NULL,
	.readunlock     = NULL,
	.name		= "rtmutex_lock"
};
#endif

static DECLARE_RWSEM(torture_rwsem);
/* 获取 rwsem 写锁，成功固定返回 0 并保持独占持有。 */
static int torture_rwsem_down_write(int tid __maybe_unused)
__acquires(torture_rwsem)
{
	down_write(&torture_rwsem);
	return 0;
}

/* rwsem 写持锁期间偶发较长忙等和主动让出，扩大睡眠/唤醒争用。 */
static void torture_rwsem_write_delay(struct torture_random_state *trsp)
{
	/* 偶发长延迟用于强制形成大规模争用。 */
	/* We want a long delay occasionally to force massive contention.  */
	if (long_hold && !(torture_random(trsp) % (cxt.nrealwriters_stress * 2000 * long_hold)))
		mdelay(long_hold * 10);
	if (!(torture_random(trsp) % (cxt.nrealwriters_stress * 20000)))
		/* 允许测试线程被抢占，扩大调度交错。 */
		torture_preempt_schedule();  /* Allow test to be preempted. */
}

/* 释放 rwsem 写锁，并允许排队读者或写者继续。 */
static void torture_rwsem_up_write(int tid __maybe_unused)
__releases(torture_rwsem)
{
	up_write(&torture_rwsem);
}

/* 获取 rwsem 读锁，成功固定返回 0；多个 reader 可并发持有。 */
static int torture_rwsem_down_read(int tid __maybe_unused)
__acquires(torture_rwsem)
{
	down_read(&torture_rwsem);
	return 0;
}

/* rwsem 读持锁期间制造长短延迟并偶发主动调度，扩大读写交错。 */
static void torture_rwsem_read_delay(struct torture_random_state *trsp)
{
	/* 偶发长延迟用于强制形成大规模争用。 */
	/* We want a long delay occasionally to force massive contention.  */
	if (long_hold && !(torture_random(trsp) % (cxt.nrealreaders_stress * 2000 * long_hold)))
		mdelay(long_hold * 2);
	else
		mdelay(long_hold / 2);
	if (!(torture_random(trsp) % (cxt.nrealreaders_stress * 20000)))
		/* 允许测试线程被抢占，扩大调度交错。 */
		torture_preempt_schedule();  /* Allow test to be preempted. */
}

/* 释放 rwsem 读锁。 */
static void torture_rwsem_up_read(int tid __maybe_unused)
__releases(torture_rwsem)
{
	up_read(&torture_rwsem);
}

static struct lock_torture_ops rwsem_lock_ops = {
	.writelock	= torture_rwsem_down_write,
	.write_delay	= torture_rwsem_write_delay,
	.task_boost     = torture_rt_boost,
	.writeunlock	= torture_rwsem_up_write,
	.readlock       = torture_rwsem_down_read,
	.read_delay     = torture_rwsem_read_delay,
	.readunlock     = torture_rwsem_up_read,
	.name		= "rwsem_lock"
};

#include <linux/percpu-rwsem.h>
static struct percpu_rw_semaphore pcpu_rwsem;

/* 动态初始化 percpu-rwsem；测试环境把失败视为不可继续的 BUG。 */
static void torture_percpu_rwsem_init(void)
{
	BUG_ON(percpu_init_rwsem(&pcpu_rwsem));
}

/* 在所有测试线程停止后销毁 percpu-rwsem 及其每 CPU 计数。 */
static void torture_percpu_rwsem_exit(void)
{
	percpu_free_rwsem(&pcpu_rwsem);
}

/* 获取 percpu-rwsem 写锁，返回时已排空全部读者并独占。 */
static int torture_percpu_rwsem_down_write(int tid __maybe_unused)
__acquires(pcpu_rwsem)
{
	percpu_down_write(&pcpu_rwsem);
	return 0;
}

/* 释放 percpu-rwsem 写锁，并启动读快路径恢复过程。 */
static void torture_percpu_rwsem_up_write(int tid __maybe_unused)
__releases(pcpu_rwsem)
{
	percpu_up_write(&pcpu_rwsem);
}

/* 获取 percpu-rwsem 读锁；无 writer 时主要命中每 CPU 快路径。 */
static int torture_percpu_rwsem_down_read(int tid __maybe_unused)
__acquires(pcpu_rwsem)
{
	percpu_down_read(&pcpu_rwsem);
	return 0;
}

/* 释放 percpu-rwsem 读锁，并在慢路径下推动等待读者排空的 writer。 */
static void torture_percpu_rwsem_up_read(int tid __maybe_unused)
__releases(pcpu_rwsem)
{
	percpu_up_read(&pcpu_rwsem);
}

static struct lock_torture_ops percpu_rwsem_lock_ops = {
	.init		= torture_percpu_rwsem_init,
	.exit		= torture_percpu_rwsem_exit,
	.writelock	= torture_percpu_rwsem_down_write,
	.write_delay	= torture_rwsem_write_delay,
	.task_boost     = torture_rt_boost,
	.writeunlock	= torture_percpu_rwsem_up_write,
	.readlock       = torture_percpu_rwsem_down_read,
	.read_delay     = torture_rwsem_read_delay,
	.readunlock     = torture_percpu_rwsem_up_read,
	.name		= "percpu_rwsem_lock"
};

/*
 * writer 压力线程反复获取/释放选定写锁，并用共享标志检查重复写获取和读写重叠。
 */
/*
 * Lock torture writer kthread.  Repeatedly acquires and releases
 * the lock, checking for duplicate acquisitions.
 */
/*
 * arg 指向本线程统计槽，由指针差得到 tid。每轮可获取随机嵌套锁，偶发跳过主锁
 * 以形成互不相交的阻塞树；主锁成功后校验没有 writer/reader 重叠、记录获取耗时
 * 和次数、执行类型专属延迟再释放。停止时强制恢复普通调度策略并返回 0。
 */
static int lock_torture_writer(void *arg)
{
	unsigned long j;
	unsigned long j1;
	u32 lockset_mask;
	struct lock_stress_stats *lwsp = arg;
	DEFINE_TORTURE_RANDOM(rand);
	bool skip_main_lock;
	int tid = lwsp - cxt.lwsa;

	VERBOSE_TOROUT_STRING("lock_torture_writer task started");
	if (!rt_task(current))
		set_user_nice(current, MAX_NICE);

	do {
		if ((torture_random(&rand) & 0xfffff) == 0)
			schedule_timeout_uninterruptible(1);

		lockset_mask = torture_random(&rand);
		/*
		 * 使用嵌套锁时偶尔跳过主锁，避免所有锁链都被中心锁串行化，从而形成
		 * 多棵互不相交的阻塞树和不同争用形态。
		 */
		/*
		 * When using nested_locks, we want to occasionally
		 * skip the main lock so we can avoid always serializing
		 * the lock chains on that central lock. By skipping the
		 * main lock occasionally, we can create different
		 * contention patterns (allowing for multiple disjoint
		 * blocked trees)
		 */
		skip_main_lock = (nested_locks &&
				 !(torture_random(&rand) % 100));

		cxt.cur_ops->task_boost(&rand);
		if (cxt.cur_ops->nested_lock)
			cxt.cur_ops->nested_lock(tid, lockset_mask);

		if (!skip_main_lock) {
			if (acq_writer_lim > 0)
				j = jiffies;
			cxt.cur_ops->writelock(tid);
			if (WARN_ON_ONCE(lock_is_write_held))
				lwsp->n_lock_fail++;
			lock_is_write_held = true;
			if (WARN_ON_ONCE(atomic_read(&lock_is_read_held)))
				/* 极少发生；一旦发生即证明读写排斥被破坏。 */
				lwsp->n_lock_fail++; /* rare, but... */
			if (acq_writer_lim > 0) {
				j1 = jiffies;
				WARN_ONCE(time_after(j1, j + acq_writer_lim),
					  "%s: Lock acquisition took %lu jiffies.\n",
					  __func__, j1 - j);
			}
			lwsp->n_lock_acquired++;

			cxt.cur_ops->write_delay(&rand);

			lock_is_write_held = false;
			WRITE_ONCE(last_lock_release, jiffies);
			cxt.cur_ops->writeunlock(tid);
		}
		if (cxt.cur_ops->nested_unlock)
			cxt.cur_ops->nested_unlock(tid, lockset_mask);

		stutter_wait("lock_torture_writer");
	} while (!torture_must_stop());

	/* 线程停止前把可能仍生效的 FIFO 提升恢复为普通优先级。 */
	cxt.cur_ops->task_boost(NULL); /* reset prio */
	torture_kthread_stopping("lock_torture_writer");
	return 0;
}

/*
 * reader 压力线程反复获取和释放读锁，并检查读临界区内是否出现 writer。
 */
/*
 * Lock torture reader kthread.  Repeatedly acquires and releases
 * the reader lock.
 */
/*
 * arg 指向本线程统计槽。每轮获取读锁后增加全局 reader 计数，检查 writer 标志，
 * 记录成功次数并执行读延迟，再按相反顺序递减计数和解锁。停止请求到来后退出，
 * 返回 0；仅为具有 readlock 操作的锁类型创建此线程。
 */
static int lock_torture_reader(void *arg)
{
	struct lock_stress_stats *lrsp = arg;
	int tid = lrsp - cxt.lrsa;
	DEFINE_TORTURE_RANDOM(rand);

	VERBOSE_TOROUT_STRING("lock_torture_reader task started");
	set_user_nice(current, MAX_NICE);

	do {
		if ((torture_random(&rand) & 0xfffff) == 0)
			schedule_timeout_uninterruptible(1);

		cxt.cur_ops->readlock(tid);
		atomic_inc(&lock_is_read_held);
		if (WARN_ON_ONCE(lock_is_write_held))
			/* 极少发生；一旦发生即证明读写排斥被破坏。 */
			lrsp->n_lock_fail++; /* rare, but... */

		lrsp->n_lock_acquired++;
		cxt.cur_ops->read_delay(&rand);
		atomic_dec(&lock_is_read_held);
		cxt.cur_ops->readunlock(tid);

		stutter_wait("lock_torture_reader");
	} while (!torture_must_stop());
	torture_kthread_stopping("lock_torture_reader");
	return 0;
}

/*
 * 在调用者提供的缓冲区中生成一条 locktorture 统计消息。
 */
/*
 * Create an lock-torture-statistics message in the specified buffer.
 */
/*
 * 汇总 writer 或 reader 的每线程获取次数、最大/最小值和失败位并写入 page。
 * 统计读取故意用 data_race() 接受近似快照；若发现失败则累加全局错误计数。
 * 调用者必须保证 page 足够大，本函数不返回写入长度。
 */
static void __torture_print_stats(char *page,
				  struct lock_stress_stats *statp, bool write)
{
	long cur;
	bool fail = false;
	int i, n_stress;
	/* 统计线程接受并发更新形成的近似快照，不用锁干扰被测负载。 */
	long max = 0, min = statp ? data_race(statp[0].n_lock_acquired) : 0;
	long long sum = 0;

	n_stress = write ? cxt.nrealwriters_stress : cxt.nrealreaders_stress;
	for (i = 0; i < n_stress; i++) {
		/* 两个字段都由对应压力线程并发更新，这里只做诊断性采样。 */
		if (data_race(statp[i].n_lock_fail))
			fail = true;
		/* 获取次数同样只要求近似快照，不与压力线程串行。 */
		cur = data_race(statp[i].n_lock_acquired);
		sum += cur;
		if (max < cur)
			max = cur;
		if (min > cur)
			min = cur;
	}
	page += sprintf(page,
			"%s:  Total: %lld  Max/Min: %ld/%ld %s  Fail: %d %s\n",
			write ? "Writes" : "Reads ",
			sum, max, min,
			!onoff_interval && max / 2 > min ? "???" : "",
			fail, fail ? "!!!" : "");
	if (fail)
		atomic_inc(&cxt.n_lock_torture_errors);
}

/*
 * 打印压力统计。调用者必须保证同一时刻只有一个实例：通常由模块单实例以及
 * stats kthread 的独占调用权保证；统计线程未运行时则只允许 init/cleanup 调用。
 */
/*
 * Print torture statistics.  Caller must ensure that there is only one
 * call to this function at a given time!!!  This is normally accomplished
 * by relying on the module system to only have one copy of the module
 * loaded, and then by giving the lock_torture_stats kthread full control
 * (or the init/cleanup functions when lock_torture_stats thread is not
 * running).
 */
/*
 * 分别为 writer 和可选 reader 分配临时缓冲、生成并打印统计后释放。任一分配
 * 失败只打印错误并返回，不改变测试线程；串行调用前提也避免共享统计输出交错。
 */
static void lock_torture_stats_print(void)
{
	int size = cxt.nrealwriters_stress * 200 + 8192;
	char *buf;

	if (cxt.cur_ops->readlock)
		size += cxt.nrealreaders_stress * 200 + 8192;

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf) {
		pr_err("lock_torture_stats_print: Out of memory, need: %d",
		       size);
		return;
	}

	__torture_print_stats(buf, cxt.lwsa, true);
	pr_alert("%s", buf);
	kfree(buf);

	if (cxt.cur_ops->readlock) {
		buf = kmalloc(size, GFP_KERNEL);
		if (!buf) {
			pr_err("lock_torture_stats_print: Out of memory, need: %d",
			       size);
			return;
		}

		__torture_print_stats(buf, cxt.lrsa, false);
		pr_alert("%s", buf);
		kfree(buf);
	}
}

/*
 * stat_interval 非零时周期打印统计。此线程不引用需要 fullstop 保护的易变状态，
 * 也不注册回调，因此无需额外处理 fullstop 阶段。
 */
/*
 * Periodically prints torture statistics, if periodic statistics printing
 * was specified via the stat_interval module parameter.
 *
 * No need to worry about fullstop here, since this one doesn't reference
 * volatile state or register callbacks.
 */
/* 每隔 stat_interval 秒打印一次并吸收关机请求，直到停止后返回 0。 */
static int lock_torture_stats(void *arg)
{
	VERBOSE_TOROUT_STRING("lock_torture_stats task started");
	do {
		schedule_timeout_interruptible(stat_interval * HZ);
		lock_torture_stats_print();
		torture_shutdown_absorb("lock_torture_stats");
	} while (!torture_must_stop());
	torture_kthread_stopping("lock_torture_stats");
	return 0;
}


/* 打印当前锁类型、调试状态、实际线程数、CPU 绑定及全部压力模块参数。 */
static inline void
lock_torture_print_module_parms(struct lock_torture_ops *cur_ops,
				const char *tag)
{
	static cpumask_t cpumask_all;
	cpumask_t *rcmp = cpumask_nonempty(bind_readers) ? bind_readers : &cpumask_all;
	cpumask_t *wcmp = cpumask_nonempty(bind_writers) ? bind_writers : &cpumask_all;

	cpumask_setall(&cpumask_all);
	pr_alert("%s" TORTURE_FLAG
		 "--- %s%s: acq_writer_lim=%d bind_readers=%*pbl bind_writers=%*pbl call_rcu_chains=%d long_hold=%d nested_locks=%d nreaders_stress=%d nwriters_stress=%d onoff_holdoff=%d onoff_interval=%d rt_boost=%d rt_boost_factor=%d shuffle_interval=%d shutdown_secs=%d stat_interval=%d stutter=%d verbose=%d writer_fifo=%d\n",
		 torture_type, tag, cxt.debug_lock ? " [debug]": "",
		 acq_writer_lim, cpumask_pr_args(rcmp), cpumask_pr_args(wcmp),
		 call_rcu_chains, long_hold, nested_locks, cxt.nrealreaders_stress,
		 cxt.nrealwriters_stress, onoff_holdoff, onoff_interval, rt_boost,
		 rt_boost_factor, shuffle_interval, shutdown_secs, stat_interval, stutter,
		 verbose, writer_fifo);
}

/*
 * 若请求 call_rcu_chains，则让每条回调链持续保持 RCU 宽限期在途，提高锁停顿
 * 演变为 RCU CPU stall 并产生诊断的概率。
 */
// If requested, maintain call_rcu() chains to keep a grace period always
// in flight.  These increase the probability of getting an RCU CPU stall
// warning and associated diagnostics when a locking primitive stalls.

/*
 * 一条 RCU 自续链的回调。acquire 读取 crc_stop；未停止时先发起一次轮询宽限期，
 * 再把自身重新交给 call_rcu()，形成持续链。cleanup 的 release 写入与之配对。
 */
static void call_rcu_chain_cb(struct rcu_head *rhp)
{
	struct call_rcu_chain *crcp = container_of(rhp, struct call_rcu_chain, crc_rh);

	/* 与 cleanup 的 release 停止写配对，看到 true 后不得再提交回调。 */
	if (!smp_load_acquire(&crcp->crc_stop)) {
		/* 先启动一个宽限期。 */
		(void)start_poll_synchronize_rcu(); // Start one grace period...
		/* 当前回调稍后再启动下一轮，保持链条自传播。 */
		call_rcu(&crcp->crc_rh, call_rcu_chain_cb); // ... and later start another.
	}
}

/*
 * 分配请求数量的链对象并各提交首个 call_rcu()。禁用时返回 0；分配失败返回
 * -ENOMEM；成功后所有对象由 call_rcu_chain_cleanup() 停止并释放。
 */
// Start the requested number of call_rcu() chains.
/* 启动请求数量的 RCU 自续链；成功返回 0，分配失败返回 -ENOMEM。 */
static int call_rcu_chain_init(void)
{
	int i;

	if (call_rcu_chains <= 0)
		return 0;
	call_rcu_chain_list = kzalloc_objs(*call_rcu_chain_list,
					   call_rcu_chains);
	if (!call_rcu_chain_list)
		return -ENOMEM;
	for (i = 0; i < call_rcu_chains; i++) {
		call_rcu_chain_list[i].crc_stop = false;
		call_rcu(&call_rcu_chain_list[i].crc_rh, call_rcu_chain_cb);
	}
	return 0;
}

/*
 * 以 release 为每条链设置停止位，rcu_barrier() 等待所有已排队回调完成且不再
 * 自续，然后释放数组并清空全局指针。未初始化时可重复调用且无操作。
 */
// Stop all of the call_rcu() chains.
/* 停止全部 RCU 自续链，等待在途回调结束后释放存储。 */
static void call_rcu_chain_cleanup(void)
{
	int i;

	if (!call_rcu_chain_list)
		return;
	for (i = 0; i < call_rcu_chains; i++)
		/* 发布停止请求，与回调的 acquire 读取配对。 */
		smp_store_release(&call_rcu_chain_list[i].crc_stop, true);
	rcu_barrier();
	kfree(call_rcu_chain_list);
	call_rcu_chain_list = NULL;
}

/*
 * 幂等清理整个测试实例。先用 torture_cleanup_begin() 仲裁唯一清理者；若已创建
 * 线程，依次停止 writer、reader、stats，在线程静止后打印最终统计和结果，随后
 * 释放统计数组、停止 RCU 链。即使初始化很早失败，也会在 end 标签执行已调用的
 * 类型专属 exit、释放动态 cpumask，并以 torture_cleanup_end() 完成框架状态转换。
 */
static void lock_torture_cleanup(void)
{
	int i;

	if (torture_cleanup_begin())
		return;

	/*
	 * 这里表示测试尚未真正运行的早期清理，例如模块参数非法；但类型 init
	 * 可能已经执行，所以仍须走 end 调用可选 exit 并完成框架级资源清理。
	 */
	/*
	 * Indicates early cleanup, meaning that the test has not run,
	 * such as when passing bogus args when loading the module.
	 * However cxt->cur_ops.init() may have been invoked, so beside
	 * perform the underlying torture-specific cleanups, cur_ops.exit()
	 * will be invoked if needed.
	 */
	if (!cxt.lwsa && !cxt.lrsa)
		goto end;

	if (writer_tasks) {
		for (i = 0; i < cxt.nrealwriters_stress; i++)
			torture_stop_kthread(lock_torture_writer, writer_tasks[i]);
		kfree(writer_tasks);
		writer_tasks = NULL;
	}

	if (reader_tasks) {
		for (i = 0; i < cxt.nrealreaders_stress; i++)
			torture_stop_kthread(lock_torture_reader,
					     reader_tasks[i]);
		kfree(reader_tasks);
		reader_tasks = NULL;
	}

	torture_stop_kthread(lock_torture_stats, stats_task);
	/* 必须先停统计线程，再由清理线程做最后一次无并发打印。 */
	lock_torture_stats_print();  /* -After- the stats thread is stopped! */

	if (atomic_read(&cxt.n_lock_torture_errors))
		lock_torture_print_module_parms(cxt.cur_ops,
						"End of test: FAILURE");
	else if (torture_onoff_failures())
		lock_torture_print_module_parms(cxt.cur_ops,
						"End of test: LOCK_HOTPLUG");
	else
		lock_torture_print_module_parms(cxt.cur_ops,
						"End of test: SUCCESS");

	kfree(cxt.lwsa);
	cxt.lwsa = NULL;
	kfree(cxt.lrsa);
	cxt.lrsa = NULL;

	call_rcu_chain_cleanup();

end:
	if (cxt.init_called) {
		if (cxt.cur_ops->exit)
			cxt.cur_ops->exit();
		cxt.init_called = false;
	}

	free_cpumask_var(bind_readers);
	free_cpumask_var(bind_writers);

	torture_cleanup_end();
}

/*
 * 模块初始化主状态机。选择 torture_type 对应 ops，校验线程配置并计算默认数量，
 * 调用类型 init、分配统计和任务数组、启动可选 RCU/热插拔/shuffle/shutdown/
 * stutter 子系统，最后交错创建 writer/reader 及统计线程。任一步失败都记录首个
 * 错误，经统一 unwind 结束 init 阶段并调用完整 cleanup；成功返回 0。
 */
static int __init lock_torture_init(void)
{
	int i, j;
	int firsterr = 0;
	static struct lock_torture_ops *torture_ops[] = {
		&lock_busted_ops,
		&spin_lock_ops, &spin_lock_irq_ops,
		&raw_spin_lock_ops, &raw_spin_lock_irq_ops,
#ifdef CONFIG_BPF_SYSCALL
		&raw_res_spin_lock_ops, &raw_res_spin_lock_irq_ops,
#endif
		&rw_lock_ops, &rw_lock_irq_ops,
		&mutex_lock_ops,
		&ww_mutex_lock_ops,
#ifdef CONFIG_RT_MUTEXES
		&rtmutex_lock_ops,
#endif
		&rwsem_lock_ops,
		&percpu_rwsem_lock_ops,
	};

	if (!torture_init_begin(torture_type, verbose))
		return -EBUSY;

	/* 解析参数、选择操作表，并公布测试器开始工作。 */
	/* Process args and tell the world that the torturer is on the job. */
	for (i = 0; i < ARRAY_SIZE(torture_ops); i++) {
		cxt.cur_ops = torture_ops[i];
		if (strcmp(torture_type, cxt.cur_ops->name) == 0)
			break;
	}
	if (i == ARRAY_SIZE(torture_ops)) {
		pr_alert("lock-torture: invalid torture type: \"%s\"\n",
			 torture_type);
		pr_alert("lock-torture types:");
		for (i = 0; i < ARRAY_SIZE(torture_ops); i++)
			pr_alert(" %s", torture_ops[i]->name);
		pr_alert("\n");
		firsterr = -EINVAL;
		goto unwind;
	}

	if (nwriters_stress == 0 &&
	    (!cxt.cur_ops->readlock || nreaders_stress == 0)) {
		pr_alert("lock-torture: must run at least one locking thread\n");
		firsterr = -EINVAL;
		goto unwind;
	}

	if (nwriters_stress >= 0)
		cxt.nrealwriters_stress = nwriters_stress;
	else
		cxt.nrealwriters_stress = 2 * num_online_cpus();

	if (cxt.cur_ops->init) {
		cxt.cur_ops->init();
		cxt.init_called = true;
	}

#ifdef CONFIG_DEBUG_MUTEXES
	if (str_has_prefix(torture_type, "mutex"))
		cxt.debug_lock = true;
#endif
#ifdef CONFIG_DEBUG_RT_MUTEXES
	if (str_has_prefix(torture_type, "rtmutex"))
		cxt.debug_lock = true;
#endif
#ifdef CONFIG_DEBUG_SPINLOCK
	if ((str_has_prefix(torture_type, "spin")) ||
	    (str_has_prefix(torture_type, "rw_lock")))
		cxt.debug_lock = true;
#endif

	/* 初始化每线程统计，确保每次模块运行都从独立的零计数开始。 */
	/* Initialize the statistics so that each run gets its own numbers. */
	if (nwriters_stress) {
		lock_is_write_held = false;
		cxt.lwsa = kmalloc_objs(*cxt.lwsa, cxt.nrealwriters_stress);
		if (cxt.lwsa == NULL) {
			VERBOSE_TOROUT_STRING("cxt.lwsa: Out of memory");
			firsterr = -ENOMEM;
			goto unwind;
		}

		for (i = 0; i < cxt.nrealwriters_stress; i++) {
			cxt.lwsa[i].n_lock_fail = 0;
			cxt.lwsa[i].n_lock_acquired = 0;
		}
	}

	if (cxt.cur_ops->readlock) {
		if (nreaders_stress >= 0)
			cxt.nrealreaders_stress = nreaders_stress;
		else {
			/*
			 * 默认均分 reader/writer；总线程数仍与仅 writer 锁类型的默认值相同。
			 */
			/*
			 * By default distribute evenly the number of
			 * readers and writers. We still run the same number
			 * of threads as the writer-only locks default.
			 */
			/* 负值表示用户未指定 writer 数量，可由默认均分规则调整。 */
			if (nwriters_stress < 0) /* user doesn't care */
				cxt.nrealwriters_stress = num_online_cpus();
			cxt.nrealreaders_stress = cxt.nrealwriters_stress;
		}

		if (nreaders_stress) {
			cxt.lrsa = kmalloc_objs(*cxt.lrsa,
						cxt.nrealreaders_stress);
			if (cxt.lrsa == NULL) {
				VERBOSE_TOROUT_STRING("cxt.lrsa: Out of memory");
				firsterr = -ENOMEM;
				kfree(cxt.lwsa);
				cxt.lwsa = NULL;
				goto unwind;
			}

			for (i = 0; i < cxt.nrealreaders_stress; i++) {
				cxt.lrsa[i].n_lock_fail = 0;
				cxt.lrsa[i].n_lock_acquired = 0;
			}
		}
	}

	firsterr = call_rcu_chain_init();
	if (torture_init_error(firsterr))
		goto unwind;

	lock_torture_print_module_parms(cxt.cur_ops, "Start of test");

	/* 初始化 CPU 热插拔、shuffle、自动关机和 stutter 等通用压力上下文。 */
	/* Prepare torture context. */
	if (onoff_interval > 0) {
		firsterr = torture_onoff_init(onoff_holdoff * HZ,
					      onoff_interval * HZ, NULL);
		if (torture_init_error(firsterr))
			goto unwind;
	}
	if (shuffle_interval > 0) {
		firsterr = torture_shuffle_init(shuffle_interval);
		if (torture_init_error(firsterr))
			goto unwind;
	}
	if (shutdown_secs > 0) {
		firsterr = torture_shutdown_init(shutdown_secs,
						 lock_torture_cleanup);
		if (torture_init_error(firsterr))
			goto unwind;
	}
	if (stutter > 0) {
		firsterr = torture_stutter_init(stutter, stutter);
		if (torture_init_error(firsterr))
			goto unwind;
	}

	if (nwriters_stress) {
		writer_tasks = kzalloc_objs(writer_tasks[0],
					    cxt.nrealwriters_stress);
		if (writer_tasks == NULL) {
			TOROUT_ERRSTRING("writer_tasks: Out of memory");
			firsterr = -ENOMEM;
			goto unwind;
		}
	}

	/* 把嵌套层数限制到 lockdep 和本地数组都支持的上限。 */
	/* cap nested_locks to MAX_NESTED_LOCKS */
	if (nested_locks > MAX_NESTED_LOCKS)
		nested_locks = MAX_NESTED_LOCKS;

	if (cxt.cur_ops->readlock) {
		reader_tasks = kzalloc_objs(reader_tasks[0],
					    cxt.nrealreaders_stress);
		if (reader_tasks == NULL) {
			TOROUT_ERRSTRING("reader_tasks: Out of memory");
			kfree(writer_tasks);
			writer_tasks = NULL;
			firsterr = -ENOMEM;
			goto unwind;
		}
	}

	/*
	 * 交错创建 writer 与 reader 并开始施压。writer 每轮先创建，因此有轻微先发
	 * 优势；未来如有特定需求，可把创建策略暴露为参数。
	 */
	/*
	 * Create the kthreads and start torturing (oh, those poor little locks).
	 *
	 * TODO: Note that we interleave writers with readers, giving writers a
	 * slight advantage, by creating its kthread first. This can be modified
	 * for very specific needs, or even let the user choose the policy, if
	 * ever wanted.
	 */
	for (i = 0, j = 0; i < cxt.nrealwriters_stress ||
		    j < cxt.nrealreaders_stress; i++, j++) {
		if (i >= cxt.nrealwriters_stress)
			goto create_reader;

		/* 创建 writer 线程。 */
		/* Create writer. */
		firsterr = torture_create_kthread_cb(lock_torture_writer, &cxt.lwsa[i],
						     writer_tasks[i],
						     writer_fifo ? sched_set_fifo : NULL);
		if (torture_init_error(firsterr))
			goto unwind;
		if (cpumask_nonempty(bind_writers))
			torture_sched_setaffinity(writer_tasks[i]->pid, bind_writers, true);

	create_reader:
		if (cxt.cur_ops->readlock == NULL || (j >= cxt.nrealreaders_stress))
			continue;
		/* 创建 reader 线程。 */
		/* Create reader. */
		firsterr = torture_create_kthread(lock_torture_reader, &cxt.lrsa[j],
						  reader_tasks[j]);
		if (torture_init_error(firsterr))
			goto unwind;
		if (cpumask_nonempty(bind_readers))
			torture_sched_setaffinity(reader_tasks[j]->pid, bind_readers, true);
	}
	if (stat_interval > 0) {
		firsterr = torture_create_kthread(lock_torture_stats, NULL,
						  stats_task);
		if (torture_init_error(firsterr))
			goto unwind;
	}
	torture_init_end();
	return 0;

unwind:
	torture_init_end();
	lock_torture_cleanup();
	if (shutdown_secs) {
		WARN_ON(!IS_MODULE(CONFIG_LOCK_TORTURE_TEST));
		kernel_power_off();
	}
	return firsterr;
}

module_init(lock_torture_init);
module_exit(lock_torture_cleanup);
