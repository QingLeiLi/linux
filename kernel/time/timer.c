// SPDX-License-Identifier: GPL-2.0
/*
 *  Kernel internal timers
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  1997-01-28  Modified by Finn Arne Gangstad to make timers scale better.
 *
 *  1997-09-10  Updated NTP code according to technical memorandum Jan '96
 *              "A Kernel Model for Precision Timekeeping" by Dave Mills
 *  1998-12-24  Fixed a xtime SMP race (we need the xtime_lock rw spinlock to
 *              serialize accesses to xtime/lost_ticks).
 *                              Copyright (C) 1998  Andrea Arcangeli
 *  1999-03-10  Improved NTP compatibility by Ulrich Windl
 *  2002-05-31	Move sys_sysinfo here and make its locking sane, Robert Love
 *  2000-10-05  Implemented scalable SMP per-CPU timer handling.
 *                              Copyright (C) 2000, 2001, 2002  Ingo Molnar
 *              Designed by David S. Miller, Alexey Kuznetsov and Ingo Molnar
 */

#include <linux/kernel_stat.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/pid_namespace.h>
#include <linux/notifier.h>
#include <linux/thread_info.h>
#include <linux/time.h>
#include <linux/jiffies.h>
#include <linux/posix-timers.h>
#include <linux/cpu.h>
#include <linux/syscalls.h>
#include <linux/delay.h>
#include <linux/tick.h>
#include <linux/kallsyms.h>
#include <linux/irq_work.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/nohz.h>
#include <linux/sched/debug.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/random.h>
#include <linux/sysctl.h>

#include <linux/uaccess.h>
#include <asm/unistd.h>
#include <asm/div64.h>
#include <asm/timex.h>
#include <asm/io.h>

#include "tick-internal.h"
#include "timer_migration.h"

#define CREATE_TRACE_POINTS
#include <trace/events/timer.h>

/*
 * jiffies_64 是系统启动后 tick 数的全局 64 位时间轴，INITIAL_JIFFIES 刻意让
 * 低位较早发生一次环绕，以暴露错误的普通整数比较。写入由 tick/timekeeping
 * 路径负责，本文件只以 jiffies 视图做期限判断；缓存行对齐减少 SMP 伪共享。
 */
__visible u64 jiffies_64 __cacheline_aligned_in_smp = INITIAL_JIFFIES;

EXPORT_SYMBOL(jiffies_64);

/*
 * The timer wheel has LVL_DEPTH array levels. Each level provides an array of
 * LVL_SIZE buckets. Each level is driven by its own clock and therefore each
 * level has a different granularity.
 *
 * The level granularity is:		LVL_CLK_DIV ^ level
 * The level clock frequency is:	HZ / (LVL_CLK_DIV ^ level)
 *
 * The array level of a newly armed timer depends on the relative expiry
 * time. The farther the expiry time is away the higher the array level and
 * therefore the granularity becomes.
 *
 * Contrary to the original timer wheel implementation, which aims for 'exact'
 * expiry of the timers, this implementation removes the need for recascading
 * the timers into the lower array levels. The previous 'classic' timer wheel
 * implementation of the kernel already violated the 'exact' expiry by adding
 * slack to the expiry time to provide batched expiration. The granularity
 * levels provide implicit batching.
 *
 * This is an optimization of the original timer wheel implementation for the
 * majority of the timer wheel use cases: timeouts. The vast majority of
 * timeout timers (networking, disk I/O ...) are canceled before expiry. If
 * the timeout expires it indicates that normal operation is disturbed, so it
 * does not matter much whether the timeout comes with a slight delay.
 *
 * The only exception to this are networking timers with a small expiry
 * time. They rely on the granularity. Those fit into the first wheel level,
 * which has HZ granularity.
 *
 * We don't have cascading anymore. timers with a expiry time above the
 * capacity of the last wheel level are force expired at the maximum timeout
 * value of the last wheel level. From data sampling we know that the maximum
 * value observed is 5 days (network connection tracking), so this should not
 * be an issue.
 *
 * The currently chosen array constants values are a good compromise between
 * array size and granularity.
 *
 * This results in the following granularity and range levels:
 *
 * HZ 1000 steps
 * Level Offset  Granularity            Range
 *  0      0         1 ms                0 ms -         63 ms
 *  1     64         8 ms               64 ms -        511 ms
 *  2    128        64 ms              512 ms -       4095 ms (512ms - ~4s)
 *  3    192       512 ms             4096 ms -      32767 ms (~4s - ~32s)
 *  4    256      4096 ms (~4s)      32768 ms -     262143 ms (~32s - ~4m)
 *  5    320     32768 ms (~32s)    262144 ms -    2097151 ms (~4m - ~34m)
 *  6    384    262144 ms (~4m)    2097152 ms -   16777215 ms (~34m - ~4h)
 *  7    448   2097152 ms (~34m)  16777216 ms -  134217727 ms (~4h - ~1d)
 *  8    512  16777216 ms (~4h)  134217728 ms - 1073741822 ms (~1d - ~12d)
 *
 * HZ  300
 * Level Offset  Granularity            Range
 *  0	   0         3 ms                0 ms -        210 ms
 *  1	  64        26 ms              213 ms -       1703 ms (213ms - ~1s)
 *  2	 128       213 ms             1706 ms -      13650 ms (~1s - ~13s)
 *  3	 192      1706 ms (~1s)      13653 ms -     109223 ms (~13s - ~1m)
 *  4	 256     13653 ms (~13s)    109226 ms -     873810 ms (~1m - ~14m)
 *  5	 320    109226 ms (~1m)     873813 ms -    6990503 ms (~14m - ~1h)
 *  6	 384    873813 ms (~14m)   6990506 ms -   55924050 ms (~1h - ~15h)
 *  7	 448   6990506 ms (~1h)   55924053 ms -  447392423 ms (~15h - ~5d)
 *  8    512  55924053 ms (~15h) 447392426 ms - 3579139406 ms (~5d - ~41d)
 *
 * HZ  250
 * Level Offset  Granularity            Range
 *  0	   0         4 ms                0 ms -        255 ms
 *  1	  64        32 ms              256 ms -       2047 ms (256ms - ~2s)
 *  2	 128       256 ms             2048 ms -      16383 ms (~2s - ~16s)
 *  3	 192      2048 ms (~2s)      16384 ms -     131071 ms (~16s - ~2m)
 *  4	 256     16384 ms (~16s)    131072 ms -    1048575 ms (~2m - ~17m)
 *  5	 320    131072 ms (~2m)    1048576 ms -    8388607 ms (~17m - ~2h)
 *  6	 384   1048576 ms (~17m)   8388608 ms -   67108863 ms (~2h - ~18h)
 *  7	 448   8388608 ms (~2h)   67108864 ms -  536870911 ms (~18h - ~6d)
 *  8    512  67108864 ms (~18h) 536870912 ms - 4294967288 ms (~6d - ~49d)
 *
 * HZ  100
 * Level Offset  Granularity            Range
 *  0	   0         10 ms               0 ms -        630 ms
 *  1	  64         80 ms             640 ms -       5110 ms (640ms - ~5s)
 *  2	 128        640 ms            5120 ms -      40950 ms (~5s - ~40s)
 *  3	 192       5120 ms (~5s)     40960 ms -     327670 ms (~40s - ~5m)
 *  4	 256      40960 ms (~40s)   327680 ms -    2621430 ms (~5m - ~43m)
 *  5	 320     327680 ms (~5m)   2621440 ms -   20971510 ms (~43m - ~5h)
 *  6	 384    2621440 ms (~43m) 20971520 ms -  167772150 ms (~5h - ~1d)
 *  7	 448   20971520 ms (~5h) 167772160 ms - 1342177270 ms (~1d - ~15d)
 */
/*
 * 时间轮总览：
 *
 * timer_list 使用分层、分桶的时间轮保存低精度超时。第 0 层每个桶跨度一个
 * jiffy；越高层跨度越大、覆盖范围越远。新定时器只在入队时选择一次层和桶，
 * 到期前不再逐层级联，因此删除占绝大多数的 timeout 不会产生搬运成本。
 *
 * 这种设计承诺“不早于 timer->expires 执行”，却允许因桶粒度而稍晚执行。
 * 远期 timeout 本来就是故障兜底，适度延迟换来了更小的维护开销和自然批处理。
 * 超出最高层容量的期限被钳到时间轮上限；代码不能用本时间轮表达无限远事件。
 *
 * 物理数组按“层 0 的 64 个桶、层 1 的 64 个桶……”顺序排列。
 * pending_map 与 vectors[] 一一对应：位图用于快速寻找非空桶，链表保存桶内
 * timer。base->clk 是时间轮推进依据，而 jiffies 是当前墙上 tick；二者只有
 * 在持有 base->lock 的路径中按规则追赶，不能随意互换。
 */

/* Clock divisor for the next level */
/* 相邻层的粒度放大 2^3=8 倍；这些宏共同描述“层号 -> 粒度/偏移”的映射。 */
#define LVL_CLK_SHIFT	3
#define LVL_CLK_DIV	(1UL << LVL_CLK_SHIFT)
#define LVL_CLK_MASK	(LVL_CLK_DIV - 1)
#define LVL_SHIFT(n)	((n) * LVL_CLK_SHIFT)
#define LVL_GRAN(n)	(1UL << LVL_SHIFT(n))

/*
 * The time start value for each level to select the bucket at enqueue
 * time. We start from the last possible delta of the previous level
 * so that we can later add an extra LVL_GRAN(n) to n (see calc_index()).
 */
/*
 * 第 n 层从前一层最后能容纳的相对期限开始接管。calc_index() 会向上取整一个
 * 本层粒度，所以这里从前一层最后一个 delta 起算，既不留下空洞，也不让 timer
 * 因截断落入会提前触发的桶。
 */
#define LVL_START(n)	((LVL_SIZE - 1) << (((n) - 1) * LVL_CLK_SHIFT))

/* Size of each clock level */
/* 每层固定 64 桶，LVL_OFFS() 把层内下标换成扁平 vectors[] 下标。 */
#define LVL_BITS	6
#define LVL_SIZE	(1UL << LVL_BITS)
#define LVL_MASK	(LVL_SIZE - 1)
#define LVL_OFFS(n)	((n) * LVL_SIZE)

/* Level depth */
/* HZ 较大时增加一层，使不同 tick 频率下仍能覆盖足够长的 timeout。 */
#if HZ > 100
# define LVL_DEPTH	9
# else
# define LVL_DEPTH	8
#endif

/* The cutoff (max. capacity of the wheel) */
/* cutoff 是可表达 delta 的边界，max 是钳位后实际放入最高层的最大期限。 */
#define WHEEL_TIMEOUT_CUTOFF	(LVL_START(LVL_DEPTH))
#define WHEEL_TIMEOUT_MAX	(WHEEL_TIMEOUT_CUTOFF - LVL_GRAN(LVL_DEPTH - 1))

/*
 * The resulting wheel size. If NOHZ is configured we allocate two
 * wheels so we have a separate storage for the deferrable timers.
 */
/*
 * 扁平数组总桶数。NO_HZ_COMMON 下实际为每 CPU 三套 base：本地 pinned、
 * 可迁移 global、可延后 deferrable；它们共享算法但唤醒和迁移语义不同。
 */
#define WHEEL_SIZE	(LVL_SIZE * LVL_DEPTH)

#ifdef CONFIG_NO_HZ_COMMON
/*
 * If multiple bases need to be locked, use the base ordering for lock
 * nesting, i.e. lowest number first.
 */
/*
 * 同一 CPU 同时锁多套 base 时必须按 LOCAL -> GLOBAL -> DEF 的编号顺序，
 * 远端查询也遵守此嵌套顺序，从而避免 ABBA 死锁。
 */
# define NR_BASES	3
# define BASE_LOCAL	0
# define BASE_GLOBAL	1
# define BASE_DEF	2
#else
# define NR_BASES	1
# define BASE_LOCAL	0
# define BASE_GLOBAL	0
# define BASE_DEF	0
#endif

/**
 * struct timer_base - Per CPU timer base (number of base depends on config)
 * @lock:		Lock protecting the timer_base
 * @running_timer:	When expiring timers, the lock is dropped. To make
 *			sure not to race against deleting/modifying a
 *			currently running timer, the pointer is set to the
 *			timer, which expires at the moment. If no timer is
 *			running, the pointer is NULL.
 * @expiry_lock:	PREEMPT_RT only: Lock is taken in softirq around
 *			timer expiry callback execution and when trying to
 *			delete a running timer and it wasn't successful in
 *			the first glance. It prevents priority inversion
 *			when callback was preempted on a remote CPU and a
 *			caller tries to delete the running timer. It also
 *			prevents a life lock, when the task which tries to
 *			delete a timer preempted the softirq thread which
 *			is running the timer callback function.
 * @timer_waiters:	PREEMPT_RT only: Tells, if there is a waiter
 *			waiting for the end of the timer callback function
 *			execution.
 * @clk:		clock of the timer base; is updated before enqueue
 *			of a timer; during expiry, it is 1 offset ahead of
 *			jiffies to avoid endless requeuing to current
 *			jiffies
 * @next_expiry:	expiry value of the first timer; it is updated when
 *			finding the next timer and during enqueue; the
 *			value is not valid, when next_expiry_recalc is set
 * @cpu:		Number of CPU the timer base belongs to
 * @next_expiry_recalc: States, whether a recalculation of next_expiry is
 *			required. Value is set true, when a timer was
 *			deleted.
 * @is_idle:		Is set, when timer_base is idle. It is triggered by NOHZ
 *			code. This state is only used in standard
 *			base. Deferrable timers, which are enqueued remotely
 *			never wake up an idle CPU. So no matter of supporting it
 *			for this base.
 * @timers_pending:	Is set, when a timer is pending in the base. It is only
 *			reliable when next_expiry_recalc is not set.
 * @pending_map:	bitmap of the timer wheel; each bit reflects a
 *			bucket of the wheel. When a bit is set, at least a
 *			single timer is enqueued in the related bucket.
 * @vectors:		Array of lists; Each array member reflects a bucket
 *			of the timer wheel. The list contains all timers
 *			which are enqueued into a specific bucket.
 */
/*
 * struct timer_base 表示“一颗归某 CPU、某语义类别所有的时间轮”。
 *
 * 生命周期：为每个 possible CPU 静态分配，timers_init() 初始化；CPU 下线
 * 时其中 timer 迁到当前在线 CPU，而 base 对象本身不会释放。
 *
 * 同步：lock 串行化 vectors、pending_map、clk、next_expiry 以及 timer 的
 * base/桶归属。running_timer 在执行回调前发布，使删除者在回调期间仍能识别
 * timer；引用/RCU 在这里不能替代该锁。PREEMPT_RT 的 expiry_lock 额外解决
 * 可抢占 softirq 回调与同步删除者之间的优先级反转。
 *
 * 字段不变量：
 * - pending_map[idx]=1 表示 vectors[idx] 至少一个节点；
 * - next_expiry_recalc=false 时 next_expiry/timers_pending 才是可信缓存；
 * - clk 只前进不后退，到期执行时先加一，避免回调把 timer 永久重排到当前桶；
 * - cpu 与 timer->flags 中的 CPU 位共同确定 lock_timer_base() 应锁哪颗树。
 */
struct timer_base {
	raw_spinlock_t		lock;
	struct timer_list	*running_timer;
#ifdef CONFIG_PREEMPT_RT
	spinlock_t		expiry_lock;
	atomic_t		timer_waiters;
#endif
	unsigned long		clk;
	unsigned long		next_expiry;
	unsigned int		cpu;
	bool			next_expiry_recalc;
	bool			is_idle;
	bool			timers_pending;
	DECLARE_BITMAP(pending_map, WHEEL_SIZE);
	struct hlist_head	vectors[WHEEL_SIZE];
} ____cacheline_aligned;

static DEFINE_PER_CPU(struct timer_base, timer_bases[NR_BASES]);
/*
 * timer_bases 是静态的 per-CPU 所有权根。索引选择 timer 的 pinned/global/
 * deferrable 语义，CPU 位选择物理归属；timer 自身不保存 base 指针。
 */

#ifdef CONFIG_NO_HZ_COMMON

static DEFINE_STATIC_KEY_FALSE(timers_nohz_active);
static DEFINE_MUTEX(timer_keys_mutex);
/*
 * static key 让 NO_HZ 未启用时的热路径近似零开销；mutex 串行化两个 key 的
 * 切换，work item 则把可能修改跳转标签的操作移出调用者的敏感上下文。
 */

static void timer_update_keys(struct work_struct *work);
/* 唯一静态 work 实例合并重复刷新请求；workqueue 不取得额外动态对象 ownership。 */
static DECLARE_WORK(timer_update_work, timer_update_keys);

#ifdef CONFIG_SMP
static unsigned int sysctl_timer_migration = 1;
/* 默认允许可迁移 timer 由活跃 CPU 代管；sysctl 在 mutex 下更新该策略源。 */

DEFINE_STATIC_KEY_FALSE(timers_migration_enabled);
/* 热路径读取的已发布版本，只有 timers_update_migration() 改变它。 */

/*
 * timers_update_migration() - 令迁移静态分支与“用户开关且 NO_HZ 已激活”一致。
 *
 * 入参/返回：无；由 sysctl 路径或延迟 work 在 timer_keys_mutex 下调用。
 * 可能睡眠（static key 更新会修改内核文本），副作用是切换热路径分支。
 */
static void timers_update_migration(void)
{
	if (sysctl_timer_migration && tick_nohz_is_active())
		static_branch_enable(&timers_migration_enabled);
	else
		static_branch_disable(&timers_migration_enabled);
}

#ifdef CONFIG_SYSCTL
/*
 * timer_migration_handler() - 读写 /proc/sys/kernel/timer_migration。
 *
 * @table/@buffer/@lenp/@ppos 原样借给通用 sysctl 整数处理器；@write 表示写入。
 * 返回 0 或通用处理器 errno。mutex 使数值更新和 static key 更新成为同一事务，
 * 避免观察到新配置却仍走旧分支。
 */
static int timer_migration_handler(const struct ctl_table *table, int write,
			    void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	mutex_lock(&timer_keys_mutex);
	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	if (!ret && write)
		timers_update_migration();
	mutex_unlock(&timer_keys_mutex);
	return ret;
}

static const struct ctl_table timer_sysctl[] = {
	/* 只接受 0/1；表项在 init 后长期只读，data 指向全局策略值。 */
	{
		.procname	= "timer_migration",
		.data		= &sysctl_timer_migration,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= timer_migration_handler,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
};

static int __init timer_sysctl_init(void)
{
	/* init 阶段注册表；无可回滚资源，注册接口负责其长期生命周期。 */
	register_sysctl("kernel", timer_sysctl);
	return 0;
}
device_initcall(timer_sysctl_init);
#endif /* CONFIG_SYSCTL */
#else /* CONFIG_SMP */
/* UP 配置没有跨 CPU 迁移，保留同名空入口使调用者无需条件编译。 */
static inline void timers_update_migration(void) { }
#endif /* !CONFIG_SMP */

/*
 * timer_update_keys() - 在进程上下文统一启用 NO_HZ 与 timer migration key。
 *
 * @work 仅用于 workqueue ABI，不转移所有权；无返回值。mutex 保证与 sysctl
 * 写串行。先按当前条件更新 migration，再发布 nohz_active，热路径不会在迁移
 * key 尚未同步时看到 NO_HZ 已就绪。
 */
static void timer_update_keys(struct work_struct *work)
{
	mutex_lock(&timer_keys_mutex);
	timers_update_migration();
	static_branch_enable(&timers_nohz_active);
	mutex_unlock(&timer_keys_mutex);
}

/*
 * timers_update_nohz() - 请求异步刷新 timer 的 NO_HZ 静态分支。
 *
 * 无入参、无返回；可从不适合直接 patch static key 的路径调用。重复调度会被
 * workqueue 合并，最终状态由 timer_update_keys() 收敛。
 */
void timers_update_nohz(void)
{
	schedule_work(&timer_update_work);
}

/* 热路径只查询已发布的 static key；不加锁、不睡眠。 */
static inline bool is_timers_nohz_active(void)
{
	return static_branch_unlikely(&timers_nohz_active);
}
#else
/* 未配置 NO_HZ 时查询恒为 false；无入参、无副作用。 */
static inline bool is_timers_nohz_active(void) { return false; }
#endif /* NO_HZ_COMMON */

/*
 * round_jiffies_common() - 把绝对 jiffy 期限聚合到近似整秒边界。
 *
 * @j 是绝对期限；@cpu 决定每 CPU 3-jiffy 错峰；@force_up 禁止向下取整。
 * 无锁、不可睡眠，返回仍在未来的绝对期限；若取整结果已经过期则保留原值。
 * 它只改变节能型 timer 的精度，不负责入队，也不提供并发序列化。
 */
static unsigned long round_jiffies_common(unsigned long j, int cpu,
		bool force_up)
{
	int rem;
	unsigned long original = j;

	/*
	 * We don't want all cpus firing their timers at once hitting the
	 * same lock or cachelines, so we skew each extra cpu with an extra
	 * 3 jiffies. This 3 jiffies came originally from the mm/ code which
	 * already did this.
	 * The skew is done by adding 3*cpunr, then round, then subtract this
	 * extra offset again.
	 */
	/*
	 * 先加偏移、取整、再减偏移，相当于每 CPU 使用不同的整秒相位；这样同 CPU
	 * 的宽松 timer 仍可批处理，又不会让所有 CPU 同时争抢共享锁和缓存行。
	 */
	j += cpu * 3;

	rem = j % HZ;

	/*
	 * If the target jiffy is just after a whole second (which can happen
	 * due to delays of the timer irq, long irq off times etc etc) then
	 * we should round down to the whole second, not up. Use 1/4th second
	 * as cutoff for this rounding as an extreme upper bound for this.
	 * But never round down if @force_up is set.
	 */
	/*
	 * 距整秒不足 1/4 秒时允许向下聚合，但 force_up 的 timeout 必须守住
	 * “绝不提前”契约。其余情况向上到下一秒。
	 */
	if (rem < HZ/4 && !force_up) /* round down */
		j = j - rem;
	else /* round up */
		j = j - rem + HZ;

	/* now that we have rounded, subtract the extra skew again */
	/* 恢复到调用者的时间坐标；错峰效果保留在最终期限的相位中。 */
	j -= cpu * 3;

	/*
	 * Make sure j is still in the future. Otherwise return the
	 * unmodified value.
	 */
	/* jiffies 可能在计算期间推进；绝不能把节能取整变成一个已经过期的期限。 */
	return time_is_after_jiffies(j) ? j : original;
}

/**
 * __round_jiffies_relative - function to round jiffies to a full second
 * @j: the time in (relative) jiffies that should be rounded
 * @cpu: the processor number on which the timeout will happen
 *
 * __round_jiffies_relative() rounds a time delta  in the future (in jiffies)
 * up or down to (approximately) full seconds. This is useful for timers
 * for which the exact time they fire does not matter too much, as long as
 * they fire approximately every X seconds.
 *
 * By rounding these timers to whole seconds, all such timers will fire
 * at the same time, rather than at various times spread out. The goal
 * of this is to have the CPU wake up less, which saves power.
 *
 * The exact rounding is skewed for each processor to avoid all
 * processors firing at the exact same time, which could lead
 * to lock contention or spurious cache line bouncing.
 *
 * The return value is the rounded version of the @j parameter.
 */
/*
 * 中文契约：把相对延迟 @j（jiffy）转换为绝对时间后按 @cpu 错峰取整，再转换
 * 回相对延迟。调用者仍拥有全部状态；无锁、不可睡眠。返回近似整秒的相对值，
 * 不保证严格向上取整，但保证计算出的绝对期限仍在未来。
 */
unsigned long __round_jiffies_relative(unsigned long j, int cpu)
{
	unsigned long j0 = jiffies;

	/* Use j0 because jiffies might change while we run */
	/* 同一份 j0 同时用于加、减，避免两次读取跨 tick 造成相对期限凭空变化。 */
	return round_jiffies_common(j + j0, cpu, false) - j0;
}
EXPORT_SYMBOL_GPL(__round_jiffies_relative);

/**
 * round_jiffies - function to round jiffies to a full second
 * @j: the time in (absolute) jiffies that should be rounded
 *
 * round_jiffies() rounds an absolute time in the future (in jiffies)
 * up or down to (approximately) full seconds. This is useful for timers
 * for which the exact time they fire does not matter too much, as long as
 * they fire approximately every X seconds.
 *
 * By rounding these timers to whole seconds, all such timers will fire
 * at the same time, rather than at various times spread out. The goal
 * of this is to have the CPU wake up less, which saves power.
 *
 * The return value is the rounded version of the @j parameter.
 */
/*
 * 中文契约：对绝对 @j 取整，CPU 使用当前处理器；返回仍在未来的绝对 jiffy。
 * 这是 wrapper，无状态副作用；可用于允许轻微提前/延后的周期性后台任务。
 */
unsigned long round_jiffies(unsigned long j)
{
	return round_jiffies_common(j, raw_smp_processor_id(), false);
}
EXPORT_SYMBOL_GPL(round_jiffies);

/**
 * round_jiffies_relative - function to round jiffies to a full second
 * @j: the time in (relative) jiffies that should be rounded
 *
 * round_jiffies_relative() rounds a time delta  in the future (in jiffies)
 * up or down to (approximately) full seconds. This is useful for timers
 * for which the exact time they fire does not matter too much, as long as
 * they fire approximately every X seconds.
 *
 * By rounding these timers to whole seconds, all such timers will fire
 * at the same time, rather than at various times spread out. The goal
 * of this is to have the CPU wake up less, which saves power.
 *
 * The return value is the rounded version of the @j parameter.
 */
/*
 * 中文契约：对当前 CPU 上的相对延迟 @j 取整；返回相对 jiffy。当前 CPU 只用于
 * 选择错峰相位，函数不固定 timer 的最终 CPU。
 */
unsigned long round_jiffies_relative(unsigned long j)
{
	return __round_jiffies_relative(j, raw_smp_processor_id());
}
EXPORT_SYMBOL_GPL(round_jiffies_relative);

/**
 * __round_jiffies_up_relative - function to round jiffies up to a full second
 * @j: the time in (relative) jiffies that should be rounded
 * @cpu: the processor number on which the timeout will happen
 *
 * This is the same as __round_jiffies_relative() except that it will never
 * round down.  This is useful for timeouts for which the exact time
 * of firing does not matter too much, as long as they don't fire too
 * early.
 */
/*
 * 中文契约：与 __round_jiffies_relative() 相同，但 @force_up=true，故不会
 * 因聚合而提前触发；适合“可以晚、不能早”的超时。返回相对 jiffy。
 */
unsigned long __round_jiffies_up_relative(unsigned long j, int cpu)
{
	unsigned long j0 = jiffies;

	/* Use j0 because jiffies might change while we run */
	/* 固定转换基准，确保返回值只反映取整而非并发 tick 的两次采样差。 */
	return round_jiffies_common(j + j0, cpu, true) - j0;
}
EXPORT_SYMBOL_GPL(__round_jiffies_up_relative);

/**
 * round_jiffies_up - function to round jiffies up to a full second
 * @j: the time in (absolute) jiffies that should be rounded
 *
 * This is the same as round_jiffies() except that it will never
 * round down.  This is useful for timeouts for which the exact time
 * of firing does not matter too much, as long as they don't fire too
 * early.
 */
/* 中文契约：当前 CPU 版本的绝对期限向上取整 wrapper；无副作用。 */
unsigned long round_jiffies_up(unsigned long j)
{
	return round_jiffies_common(j, raw_smp_processor_id(), true);
}
EXPORT_SYMBOL_GPL(round_jiffies_up);

/**
 * round_jiffies_up_relative - function to round jiffies up to a full second
 * @j: the time in (relative) jiffies that should be rounded
 *
 * This is the same as round_jiffies_relative() except that it will never
 * round down.  This is useful for timeouts for which the exact time
 * of firing does not matter too much, as long as they don't fire too
 * early.
 */
/* 中文契约：当前 CPU 版本的相对期限向上取整 wrapper；返回相对 jiffy。 */
unsigned long round_jiffies_up_relative(unsigned long j)
{
	return __round_jiffies_up_relative(j, raw_smp_processor_id());
}
EXPORT_SYMBOL_GPL(round_jiffies_up_relative);


/*
 * timer_get_idx()/timer_set_idx() - 从 timer->flags 读写扁平桶下标。
 *
 * 调用者必须持有 timer 所属 base->lock；set 仅替换 ARRAY 位，保留 CPU、
 * PINNED、DEFERRABLE、MIGRATING 等协议位。
 */
static inline unsigned int timer_get_idx(struct timer_list *timer)
{
	return (timer->flags & TIMER_ARRAYMASK) >> TIMER_ARRAYSHIFT;
}

static inline void timer_set_idx(struct timer_list *timer, unsigned int idx)
{
	timer->flags = (timer->flags & ~TIMER_ARRAYMASK) |
			idx << TIMER_ARRAYSHIFT;
}

/*
 * Helper function to calculate the array index for a given expiry
 * time.
 */
/*
 * calc_index() - 在确定层 @lvl 后计算桶下标和该桶的有效到期点。
 *
 * @expires 为绝对 jiffy；@bucket_expiry 是输出参数，写入向上对齐后的桶边界；
 * 返回 vectors[] 扁平下标。纯算术、无锁。向上加一格是“不提前执行”的关键，
 * 尤其消除 tick 边缘入队和高层右移截断带来的提前风险。
 */
static inline unsigned calc_index(unsigned long expires, unsigned lvl,
				  unsigned long *bucket_expiry)
{

	/*
	 * The timer wheel has to guarantee that a timer does not fire
	 * early. Early expiry can happen due to:
	 * - Timer is armed at the edge of a tick
	 * - Truncation of the expiry time in the outer wheel levels
	 *
	 * Round up with level granularity to prevent this.
	 */
	/* 先降到层时钟再 +1，最后还原为 jiffy；这是向上取整而不是普通截断。 */
	expires = (expires >> LVL_SHIFT(lvl)) + 1;
	*bucket_expiry = expires << LVL_SHIFT(lvl);
	return LVL_OFFS(lvl) + (expires & LVL_MASK);
}

/*
 * calc_wheel_index() - 按 expires-base->clk 的相对距离选择时间轮层。
 *
 * @expires/@clk 均为绝对 jiffy；@bucket_expiry 输出实际扫描该桶的边界。
 * 返回桶下标。调用者通常持有 base->lock，使 clk 与后续入队属于同一快照。
 * 已过期 timer 放入当前第 0 层桶；超大期限钳到最高层容量。
 */
static int calc_wheel_index(unsigned long expires, unsigned long clk,
			    unsigned long *bucket_expiry)
{
	unsigned long delta = expires - clk;
	unsigned int idx;

	if (delta < LVL_START(1)) {
		idx = calc_index(expires, 0, bucket_expiry);
	} else if (delta < LVL_START(2)) {
		idx = calc_index(expires, 1, bucket_expiry);
	} else if (delta < LVL_START(3)) {
		idx = calc_index(expires, 2, bucket_expiry);
	} else if (delta < LVL_START(4)) {
		idx = calc_index(expires, 3, bucket_expiry);
	} else if (delta < LVL_START(5)) {
		idx = calc_index(expires, 4, bucket_expiry);
	} else if (delta < LVL_START(6)) {
		idx = calc_index(expires, 5, bucket_expiry);
	} else if (delta < LVL_START(7)) {
		idx = calc_index(expires, 6, bucket_expiry);
	} else if (LVL_DEPTH > 8 && delta < LVL_START(8)) {
		idx = calc_index(expires, 7, bucket_expiry);
	} else if ((long) delta < 0) {
		idx = clk & LVL_MASK;
		*bucket_expiry = clk;
	} else {
		/*
		 * Force expire obscene large timeouts to expire at the
		 * capacity limit of the wheel.
		 */
		/*
		 * 极端期限若保留 unsigned 环绕值会破坏时间比较窗口；钳位使其在时间轮
		 * 能表示的最晚时刻触发，而不是永远丢失。
		 */
		if (delta >= WHEEL_TIMEOUT_CUTOFF)
			expires = clk + WHEEL_TIMEOUT_MAX;

		idx = calc_index(expires, LVL_DEPTH - 1, bucket_expiry);
	}
	return idx;
}

static void
trigger_dyntick_cpu(struct timer_base *base, struct timer_list *timer)
{
	/*
	 * trigger_dyntick_cpu() - 必要时唤醒因 NO_HZ 停 tick 的目标 CPU。
	 *
	 * @base 已由调用者持锁，@timer 已准备进入该 base，二者均为借用对象。
	 * 无返回值；仅 pinned/global timer 可能产生 IPI。锁使目标 CPU 在设置 idle
	 * 状态与远端入队之间不存在漏唤醒窗口。
	 */
	/*
	 * Deferrable timers do not prevent the CPU from entering dynticks and
	 * are not taken into account on the idle/nohz_full path. An IPI when a
	 * new deferrable timer is enqueued will wake up the remote CPU but
	 * nothing will be done with the deferrable timer base. Therefore skip
	 * the remote IPI for deferrable timers completely.
	 */
	/*
	 * deferrable timer 的契约就是“不能为了它唤醒 CPU”；即便发 IPI，idle 路径
	 * 也不会扫描 DEF base，因此跳过既省电也避免无效中断。
	 */
	if (!is_timers_nohz_active() || timer->flags & TIMER_DEFERRABLE)
		return;

	/*
	 * We might have to IPI the remote CPU if the base is idle and the
	 * timer is pinned. If it is a non pinned timer, it is only queued
	 * on the remote CPU, when timer was running during queueing. Then
	 * everything is handled by remote CPU anyway. If the other CPU is
	 * on the way to idle then it can't set base->is_idle as we hold
	 * the base lock:
	 */
	/*
	 * 非 pinned timer 正常应迁到当前 CPU；只有回调正在远端运行时才保留旧
	 * base，此时远端本就会完成处理。持锁观察 is_idle 与 idle 发布相互串行。
	 */
	if (base->is_idle) {
		WARN_ON_ONCE(!(timer->flags & TIMER_PINNED ||
			       tick_nohz_full_cpu(base->cpu)));
		wake_up_nohz_cpu(base->cpu);
	}
}

/*
 * Enqueue the timer into the hash bucket, mark it pending in
 * the bitmap, store the index in the timer flags then wake up
 * the target CPU if needed.
 */
/*
 * enqueue_timer() - 把 timer 发布到已选定的桶，并维护最早到期缓存。
 *
 * @base 必须加锁；@timer 为已激活、当前不在链表中的借用对象；@idx 是扁平桶
 * 下标；@bucket_expiry 是该桶被扫描的绝对 jiffy。无返回值。先链入、置位、
 * 记录 idx，随后才更新 next_expiry 并可能唤醒 CPU，保证观察者被唤醒时能够
 * 看到完整 timer。
 */
static void enqueue_timer(struct timer_base *base, struct timer_list *timer,
			  unsigned int idx, unsigned long bucket_expiry)
{

	hlist_add_head(&timer->entry, base->vectors + idx);
	__set_bit(idx, base->pending_map);
	timer_set_idx(timer, idx);

	trace_timer_start(timer, bucket_expiry);

	/*
	 * Check whether this is the new first expiring timer. The
	 * effective expiry time of the timer is required here
	 * (bucket_expiry) instead of timer->expires.
	 */
	/*
	 * next_expiry 比 timer->expires 粗，因为时间轮按桶执行。这里比较桶边界才与
	 * 扫描算法一致；WRITE_ONCE 与 run_local_timers() 的无锁 READ_ONCE 配对，
	 * 只保证无撕裂，完整不变量仍由 base->lock 保护。
	 */
	if (time_before(bucket_expiry, base->next_expiry)) {
		/*
		 * Set the next expiry time and kick the CPU so it
		 * can reevaluate the wheel:
		 */
		/* 新最早事件使旧缓存重新有效，并在 idle 时触发重编程/唤醒。 */
		WRITE_ONCE(base->next_expiry, bucket_expiry);
		base->timers_pending = true;
		base->next_expiry_recalc = false;
		trigger_dyntick_cpu(base, timer);
	}
}

/*
 * internal_add_timer() - 从 timer->expires 计算桶并完成入队。
 *
 * 调用者持有 @base->lock；@timer 的 flags 已指向该 base 且不在任何链表。
 * 无返回值，成功后 timer 由 base 的 vectors 持有 pending 关系。
 */
static void internal_add_timer(struct timer_base *base, struct timer_list *timer)
{
	unsigned long bucket_expiry;
	unsigned int idx;

	idx = calc_wheel_index(timer->expires, base->clk, &bucket_expiry);
	enqueue_timer(base, timer, idx, bucket_expiry);
}

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS

static const struct debug_obj_descr timer_debug_descr;

struct timer_hint {
	/*
	 * debugobjects 报错时，function 是 timer wrapper 回调；offset 指向其容器中
	 * 更有业务意义的真实 work 回调，二者只用于诊断，不参与 timer 生命周期。
	 */
	void	(*function)(struct timer_list *t);
	long	offset;
};

#define TIMER_HINT(fn, container, timr, hintfn)			\
	{							\
		.function = fn,					\
		.offset	  = offsetof(container, hintfn) -	\
			    offsetof(container, timr)		\
	}

static const struct timer_hint timer_hints[] = {
	/* 已知 wrapper 到真实 callback 的静态映射表，进程全生命周期只读。 */
	TIMER_HINT(delayed_work_timer_fn,
		   struct delayed_work, timer, work.func),
	TIMER_HINT(kthread_delayed_work_timer_fn,
		   struct kthread_delayed_work, timer, work.func),
};

/*
 * timer_debug_hint() - 为 debugobjects 返回最有诊断价值的函数地址。
 *
 * @addr 借用 timer 指针；返回 wrapper 容器内的真实 work 回调，未知类型则返回
 * timer->function。只读、不睡眠，返回值不携带引用。
 */
static void *timer_debug_hint(void *addr)
{
	struct timer_list *timer = addr;
	int i;

	for (i = 0; i < ARRAY_SIZE(timer_hints); i++) {
		if (timer_hints[i].function == timer->function) {
			void (**fn)(void) = addr + timer_hints[i].offset;

			return *fn;
		}
	}

	return timer->function;
}

/*
 * timer_is_static_object() - 识别由 DEFINE_TIMER 等静态初始化的 timer。
 *
 * 静态哨兵由 entry.next/pprev 组合编码；返回布尔值，不改变对象。
 */
static bool timer_is_static_object(void *addr)
{
	struct timer_list *timer = addr;

	return (timer->entry.pprev == NULL &&
		timer->entry.next == TIMER_ENTRY_STATIC);
}

/*
 * timer_fixup_init is called when:
 * - an active object is initialized
 */
/*
 * 中文说明：debugobjects 发现“仍 active 却重新初始化”时先同步删除，再把对象
 * 状态重置为 initialized。@state 是诊断状态；返回 true 表示修复已完成。
 * 该路径仅为错误恢复，不能成为正常同步手段。
 */
static bool timer_fixup_init(void *addr, enum debug_obj_state state)
{
	struct timer_list *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		timer_delete_sync(timer);
		debug_object_init(timer, &timer_debug_descr);
		return true;
	default:
		return false;
	}
}

/* Stub timer callback for improperly used timers. */
/* 非法使用 timer 的替代回调：只告警，不尝试猜测或继续原业务。 */
static void stub_timer(struct timer_list *unused)
{
	WARN_ON(1);
}

/*
 * timer_fixup_activate is called when:
 * - an active object is activated
 * - an unknown non-static object is activated
 */
/*
 * 中文说明：激活未初始化对象时安装 stub 使后续执行可诊断；重复激活仅告警，
 * 不擅自删除现有 timer。返回值表示 debugobjects 是否完成了修复。
 */
static bool timer_fixup_activate(void *addr, enum debug_obj_state state)
{
	struct timer_list *timer = addr;

	switch (state) {
	case ODEBUG_STATE_NOTAVAILABLE:
		timer_setup(timer, stub_timer, 0);
		return true;

	case ODEBUG_STATE_ACTIVE:
		WARN_ON(1);
		fallthrough;
	default:
		return false;
	}
}

/*
 * timer_fixup_free is called when:
 * - an active object is freed
 */
/*
 * 中文说明：释放 active timer 是 UAF 风险，故先 timer_delete_sync() 等待回调
 * 结束，再把诊断对象标记 free。返回 true 表示危险状态已被收敛。
 */
static bool timer_fixup_free(void *addr, enum debug_obj_state state)
{
	struct timer_list *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		timer_delete_sync(timer);
		debug_object_free(timer, &timer_debug_descr);
		return true;
	default:
		return false;
	}
}

/*
 * timer_fixup_assert_init is called when:
 * - an untracked/uninit-ed object is found
 */
/* 中文说明：断言遇到未知对象时安装 stub 并初始化，避免带垃圾回调继续运行。 */
static bool timer_fixup_assert_init(void *addr, enum debug_obj_state state)
{
	struct timer_list *timer = addr;

	switch (state) {
	case ODEBUG_STATE_NOTAVAILABLE:
		timer_setup(timer, stub_timer, 0);
		return true;
	default:
		return false;
	}
}

static const struct debug_obj_descr timer_debug_descr = {
	/* timer 类型的 debugobjects 操作表；静态只读，由通用框架间接调用。 */
	.name			= "timer_list",
	.debug_hint		= timer_debug_hint,
	.is_static_object	= timer_is_static_object,
	.fixup_init		= timer_fixup_init,
	.fixup_activate		= timer_fixup_activate,
	.fixup_free		= timer_fixup_free,
	.fixup_assert_init	= timer_fixup_assert_init,
};

static inline void debug_timer_init(struct timer_list *timer)
{
	/* 向 debugobjects 发布“已初始化”；框架借用 timer，不取得业务所有权。 */
	debug_object_init(timer, &timer_debug_descr);
}

static inline void debug_timer_activate(struct timer_list *timer)
{
	/* 入队前把诊断状态转为 active，重复激活会由 fixup 报告。 */
	debug_object_activate(timer, &timer_debug_descr);
}

static inline void debug_timer_deactivate(struct timer_list *timer)
{
	/* 摘链时转回 inactive；这不负责实际链表删除。 */
	debug_object_deactivate(timer, &timer_debug_descr);
}

static inline void debug_timer_assert_init(struct timer_list *timer)
{
	/* 所有公开操作入口先验证 timer 已经过初始化。 */
	debug_object_assert_init(timer, &timer_debug_descr);
}

static void do_init_timer(struct timer_list *timer,
			  void (*func)(struct timer_list *),
			  unsigned int flags,
			  const char *name, struct lock_class_key *key);

/*
 * timer_init_key_on_stack() - 初始化栈上 timer，并登记特殊生命周期。
 *
 * 所有参数语义同 timer_init_key()；@timer 由调用者拥有，@func 借用为长期回调，
 * @name/@key 供 lockdep 使用。无返回值；离开栈作用域前必须
 * timer_destroy_on_stack()，且调用者仍须先确保 timer 已停止。
 */
void timer_init_key_on_stack(struct timer_list *timer,
			     void (*func)(struct timer_list *),
			     unsigned int flags,
			     const char *name, struct lock_class_key *key)
{
	debug_object_init_on_stack(timer, &timer_debug_descr);
	do_init_timer(timer, func, flags, name, key);
}
EXPORT_SYMBOL_GPL(timer_init_key_on_stack);

/*
 * timer_destroy_on_stack() - 撤销栈上 timer 的 debugobjects 登记。
 *
 * @timer 必须是不再 pending/running 的同一对象；无返回值，不代替同步删除。
 */
void timer_destroy_on_stack(struct timer_list *timer)
{
	debug_object_free(timer, &timer_debug_descr);
}
EXPORT_SYMBOL_GPL(timer_destroy_on_stack);

#else
/* release 配置下初始化诊断为空操作，timer 业务状态仍由 do_init_timer() 建立。 */
static inline void debug_timer_init(struct timer_list *timer) { }
/* release 配置下激活诊断为空操作，不改变实际入队协议。 */
static inline void debug_timer_activate(struct timer_list *timer) { }
/* release 配置下撤销诊断为空操作，实际链表仍由 detach_timer() 摘除。 */
static inline void debug_timer_deactivate(struct timer_list *timer) { }
/* release 配置下初始化断言为空操作，调用者仍必须遵守初始化契约。 */
static inline void debug_timer_assert_init(struct timer_list *timer) { }
#endif
/*
 * 未配置 DEBUG_OBJECTS_TIMERS 时四个空 wrapper 保留完全相同的调用契约，并由
 * 编译器消除；业务正确性不能依赖 debug 配置。
 */

/* debug_init() 同时发布 debugobjects 状态与 trace 初始化事件，不改变队列。 */
static inline void debug_init(struct timer_list *timer)
{
	debug_timer_init(timer);
	trace_timer_init(timer);
}

/* 摘链路径同步更新 debugobjects 状态并发出 cancel trace。 */
static inline void debug_deactivate(struct timer_list *timer)
{
	debug_timer_deactivate(timer);
	trace_timer_cancel(timer);
}

/* 操作入口的统一初始化断言；release 配置下可能为空。 */
static inline void debug_assert_init(struct timer_list *timer)
{
	debug_timer_assert_init(timer);
}

/*
 * do_init_timer() - 写入 timer_list 的最小可用初始状态。
 *
 * @timer 为调用者拥有的未活动对象；@func 是后续 softirq 回调且不可为垃圾值；
 * @flags 只接受 TIMER_INIT_FLAGS；@name/@key 初始化虚拟 lockdep map。
 * 无返回值。记录当前 CPU 作为初始 base 归属，但此时尚未 pending。
 */
static void do_init_timer(struct timer_list *timer,
			  void (*func)(struct timer_list *),
			  unsigned int flags,
			  const char *name, struct lock_class_key *key)
{
	timer->entry.pprev = NULL;
	timer->function = func;
	if (WARN_ON_ONCE(flags & ~TIMER_INIT_FLAGS))
		flags &= TIMER_INIT_FLAGS;
	timer->flags = flags | raw_smp_processor_id();
	lockdep_init_map(&timer->lockdep_map, name, key, 0);
}

/**
 * timer_init_key - initialize a timer
 * @timer: the timer to be initialized
 * @func: timer callback function
 * @flags: timer flags
 * @name: name of the timer
 * @key: lockdep class key of the fake lock used for tracking timer
 *       sync lock dependencies
 *
 * timer_init_key() must be done to a timer prior to calling *any* of the
 * other timer functions.
 */
/*
 * 中文契约：初始化普通（非栈特殊登记）timer。所有其他 timer API 之前必须调用。
 * timer 仍归调用者所有，函数只建立回调、flags、CPU 归属和 lockdep/debug 状态；
 * 不入队、不执行回调、不可用于覆盖一个仍 pending/running 的对象。
 */
void timer_init_key(struct timer_list *timer,
		    void (*func)(struct timer_list *), unsigned int flags,
		    const char *name, struct lock_class_key *key)
{
	debug_init(timer);
	do_init_timer(timer, func, flags, name, key);
}
EXPORT_SYMBOL(timer_init_key);

/*
 * detach_timer() - 在已锁定 base 中把 timer 从桶链表摘除。
 *
 * @clear_pending=true 时清空 pprev，使 timer_pending() 立即为假；到期执行和最终
 * 删除使用 true，迁移/重排中间态可用 false 保留“仍由 timer 子系统处理”的
 * 语义。调用者负责同步 pending_map 和 next_expiry 缓存。
 */
static inline void detach_timer(struct timer_list *timer, bool clear_pending)
{
	struct hlist_node *entry = &timer->entry;

	debug_deactivate(timer);

	__hlist_del(entry);
	if (clear_pending)
		entry->pprev = NULL;
	entry->next = LIST_POISON2;
}

/*
 * detach_if_pending() - 若 timer 仍 pending，则原子地维护桶元数据并摘链。
 *
 * @base 必须是根据 timer flags 锁定的正确 base；返回 1 表示摘除，0 表示本就
 * inactive。删除桶中最后节点时清位图并标记 next_expiry 需重算。
 */
static int detach_if_pending(struct timer_list *timer, struct timer_base *base,
			     bool clear_pending)
{
	unsigned idx = timer_get_idx(timer);

	if (!timer_pending(timer))
		return 0;

	if (hlist_is_singular_node(&timer->entry, base->vectors + idx)) {
		__clear_bit(idx, base->pending_map);
		base->next_expiry_recalc = true;
	}

	detach_timer(timer, clear_pending);
	return 1;
}

/*
 * get_timer_cpu_base() - 按 flags 类别和显式 @cpu 定位 per-CPU base。
 *
 * 返回借用指针，不加锁、不固定 CPU。PINNED 选 LOCAL，普通 timer 选 GLOBAL，
 * DEFERRABLE 在 NO_HZ 配置下优先选 DEF。
 */
static inline struct timer_base *get_timer_cpu_base(u32 tflags, u32 cpu)
{
	int index = tflags & TIMER_PINNED ? BASE_LOCAL : BASE_GLOBAL;

	/*
	 * If the timer is deferrable and NO_HZ_COMMON is set then we need
	 * to use the deferrable base.
	 */
	/* deferrable 的“不唤醒 idle CPU”语义必须由独立 base 扫描策略实现。 */
	if (IS_ENABLED(CONFIG_NO_HZ_COMMON) && (tflags & TIMER_DEFERRABLE))
		index = BASE_DEF;

	return per_cpu_ptr(&timer_bases[index], cpu);
}

/*
 * get_timer_this_cpu_base() - 当前 CPU 版本的 base 选择。
 *
 * 调用者须处于禁止迁移或持自旋锁的上下文；返回 this_cpu 借用指针。
 */
static inline struct timer_base *get_timer_this_cpu_base(u32 tflags)
{
	int index = tflags & TIMER_PINNED ? BASE_LOCAL : BASE_GLOBAL;

	/*
	 * If the timer is deferrable and NO_HZ_COMMON is set then we need
	 * to use the deferrable base.
	 */
	/*
	 * 当前 CPU 选择也必须把 deferrable timer 放入独立 DEF base，才能兑现
	 * “idle 时不因该 timer 唤醒”的契约。
	 */
	if (IS_ENABLED(CONFIG_NO_HZ_COMMON) && (tflags & TIMER_DEFERRABLE))
		index = BASE_DEF;

	return this_cpu_ptr(&timer_bases[index]);
}

/* get_timer_base() 从 flags 的 CPU 位恢复 timer 当前归属；返回未加锁借用指针。 */
static inline struct timer_base *get_timer_base(u32 tflags)
{
	return get_timer_cpu_base(tflags, tflags & TIMER_CPUMASK);
}

/*
 * __forward_timer_base() - 将 base 时钟安全前推到 @basej 或更早的 next_expiry。
 *
 * 调用者持有 base->lock；@basej 是绝对 jiffy。无返回值且绝不倒退 clk。
 * 若已有到期桶，停在 next_expiry 让执行路径先消费它；否则直接追到当前时间，
 * 减少新 timer 因陈旧 clk 被放入过粗层级。
 */
static inline void __forward_timer_base(struct timer_base *base,
					unsigned long basej)
{
	/*
	 * Check whether we can forward the base. We can only do that when
	 * @basej is past base->clk otherwise we might rewind base->clk.
	 */
	/* time_before_eq 使用 jiffy 环绕安全比较；禁止回拨破坏所有桶的相对距离。 */
	if (time_before_eq(basej, base->clk))
		return;

	/*
	 * If the next expiry value is > jiffies, then we fast forward to
	 * jiffies otherwise we forward to the next expiry value.
	 */
	/* next_expiry 已到期时只前推到它，不能越过尚未执行的桶。 */
	if (time_after(base->next_expiry, basej)) {
		base->clk = basej;
	} else {
		if (WARN_ON_ONCE(time_before(base->next_expiry, base->clk)))
			return;
		base->clk = base->next_expiry;
	}

}

/* forward_timer_base() 读取一次当前 jiffies 后调用锁内核心实现。 */
static inline void forward_timer_base(struct timer_base *base)
{
	__forward_timer_base(base, READ_ONCE(jiffies));
}

/*
 * We are using hashed locking: Holding per_cpu(timer_bases[x]).lock means
 * that all timers which are tied to this base are locked, and the base itself
 * is locked too.
 *
 * So __run_timers/migrate_timers can safely modify all timers which could
 * be found in the base->vectors array.
 *
 * When a timer is migrating then the TIMER_MIGRATING flag is set and we need
 * to wait until the migration is done.
 */
/*
 * 中文说明：timer 不含稳定 base 指针，flags 的类别/CPU 位相当于散列键。
 * 持有由该键选择的 base->lock 才能同时稳定 timer 归属和时间轮元数据。迁移者
 * 先设置 TIMER_MIGRATING，再换锁并发布新 CPU 位；查找者看到该位必须等待。
 */
/*
 * lock_timer_base() - 在并发迁移下锁住 timer 真正所属的 base。
 *
 * @timer 为借用对象；@flags 输出 irqsave 状态，调用者必须用对应 unlock 恢复。
 * 返回已加 raw spinlock 的 base，不睡眠。循环中的二次 flags 校验是乐观查找
 * 的提交点：若锁前归属改变，就解锁重试，绝不能拿错锁操作链表。
 */
static struct timer_base *lock_timer_base(struct timer_list *timer,
					  unsigned long *flags)
	__acquires(timer->base->lock)
{
	for (;;) {
		struct timer_base *base;
		u32 tf;

		/*
		 * We need to use READ_ONCE() here, otherwise the compiler
		 * might re-read @tf between the check for TIMER_MIGRATING
		 * and spin_lock().
		 */
		/* 单次 flags 快照把 MIGRATING 判断与 base 选择绑定到同一版本。 */
		tf = READ_ONCE(timer->flags);

		if (!(tf & TIMER_MIGRATING)) {
			base = get_timer_base(tf);
			raw_spin_lock_irqsave(&base->lock, *flags);
			if (timer->flags == tf)
				return base;
			raw_spin_unlock_irqrestore(&base->lock, *flags);
		}
		cpu_relax();
	}
}

#define MOD_TIMER_PENDING_ONLY		0x01
#define MOD_TIMER_REDUCE		0x02
#define MOD_TIMER_NOTPENDING		0x04
/*
 * __mod_timer 的 options：PENDING_ONLY 禁止激活 inactive；REDUCE 只允许期限
 * 提前；NOTPENDING 表示调用者已确认 inactive，可跳过常见 pending 优化。
 */

/*
 * __mod_timer() - timer 启动、重排与缩短操作的共同状态转换核心。
 *
 * @timer 已初始化且由调用者长期拥有；@expires 是绝对 jiffy；@options 选择公开
 * API 语义。返回 1 表示入口时 active，0 表示 inactive 或 shutdown 丢弃。
 * 函数用 base->lock 串行 pending、回调运行和 shutdown，可跨 CPU 换 base；
 * 不等待正在运行的回调，不转移 timer 对象所有权。
 *
 * 阶段：同桶快速路径 -> 锁定并复核 shutdown -> 必要时摘链 -> 选择当前 CPU
 * base -> 以 MIGRATING 协议换锁 -> 发布新 expires 并入队 -> 恢复 IRQ。
 */
static inline int
__mod_timer(struct timer_list *timer, unsigned long expires, unsigned int options)
{
	unsigned long clk = 0, flags, bucket_expiry;
	struct timer_base *base, *new_base;
	unsigned int idx = UINT_MAX;
	int ret = 0;

	debug_assert_init(timer);

	/*
	 * This is a common optimization triggered by the networking code - if
	 * the timer is re-modified to have the same timeout or ends up in the
	 * same array bucket then just return:
	 */
	/*
	 * 相同期限或相同桶无需摘链再入链；有效触发时刻由桶决定，因此同桶只更新
	 * 逻辑 expires 即可。代价是保留原桶的较粗粒度，但仍不会提前。
	 */
	if (!(options & MOD_TIMER_NOTPENDING) && timer_pending(timer)) {
		/*
		 * The downside of this optimization is that it can result in
		 * larger granularity than you would get from adding a new
		 * timer with this expiry.
		 */
		/* diff>0 表示新期限更早；REDUCE 遇到不更早的请求直接保持原状态。 */
		long diff = timer->expires - expires;

		if (!diff)
			return 1;
		if (options & MOD_TIMER_REDUCE && diff <= 0)
			return 1;

		/*
		 * We lock timer base and calculate the bucket index right
		 * here. If the timer ends up in the same bucket, then we
		 * just update the expiry time and avoid the whole
		 * dequeue/enqueue dance.
		 */
		/* 锁内计算确保 base->clk、旧 idx 和 shutdown 状态属于同一快照。 */
		base = lock_timer_base(timer, &flags);
		/*
		 * Has @timer been shutdown? This needs to be evaluated
		 * while holding base lock to prevent a race against the
		 * shutdown code.
		 */
		/* function==NULL 是永久 shutdown 哨兵；锁使它与并发 shutdown 线性化。 */
		if (!timer->function)
			goto out_unlock;

		forward_timer_base(base);

		if (timer_pending(timer) && (options & MOD_TIMER_REDUCE) &&
		    time_before_eq(timer->expires, expires)) {
			ret = 1;
			goto out_unlock;
		}

		clk = base->clk;
		idx = calc_wheel_index(expires, clk, &bucket_expiry);

		/*
		 * Retrieve and compare the array index of the pending
		 * timer. If it matches set the expiry to the new value so a
		 * subsequent call will exit in the expires check above.
		 */
		/* 同桶时 pending_map/链表均不变，只更新供观察和后续快速判断的 expires。 */
		if (idx == timer_get_idx(timer)) {
			if (!(options & MOD_TIMER_REDUCE))
				timer->expires = expires;
			else if (time_after(timer->expires, expires))
				timer->expires = expires;
			ret = 1;
			goto out_unlock;
		}
	} else {
		base = lock_timer_base(timer, &flags);
		/*
		 * Has @timer been shutdown? This needs to be evaluated
		 * while holding base lock to prevent a race against the
		 * shutdown code.
		 */
		/* inactive 路径同样必须在锁内复核，避免 shutdown 后被重新启动。 */
		if (!timer->function)
			goto out_unlock;

		forward_timer_base(base);
	}

	ret = detach_if_pending(timer, base, false);
	if (!ret && (options & MOD_TIMER_PENDING_ONLY))
		goto out_unlock;

	new_base = get_timer_this_cpu_base(timer->flags);

	if (base != new_base) {
		/*
		 * We are trying to schedule the timer on the new base.
		 * However we can't change timer's base while it is running,
		 * otherwise timer_delete_sync() can't detect that the timer's
		 * handler yet has not finished. This also guarantees that the
		 * timer is serialized wrt itself.
		 */
		/*
		 * 正在执行的回调必须保留旧 base：同步删除者通过 running_timer 判断完成，
		 * 若此时换 base 会让它锁新 base 而漏看旧 CPU 上仍运行的 callback。
		 */
		if (likely(base->running_timer != timer)) {
			/* See the comment in lock_timer_base() */
			/* MIGRATING 在两把锁之间封住无锁查找者，避免其使用半更新的 CPU 位。 */
			timer->flags |= TIMER_MIGRATING;

			raw_spin_unlock(&base->lock);
			base = new_base;
			raw_spin_lock(&base->lock);
			WRITE_ONCE(timer->flags,
				   (timer->flags & ~TIMER_BASEMASK) | base->cpu);
			forward_timer_base(base);
		}
	}

	debug_timer_activate(timer);

	timer->expires = expires;
	/*
	 * If 'idx' was calculated above and the base time did not advance
	 * between calculating 'idx' and possibly switching the base, only
	 * enqueue_timer() is required. Otherwise we need to (re)calculate
	 * the wheel index via internal_add_timer().
	 */
	/*
	 * 仅当计算 idx 所用 clk 仍等于目标 base->clk 才能复用结果；换 CPU 或时钟
	 * 前推后必须重算，否则 timer 可能落入错误层/桶。
	 */
	if (idx != UINT_MAX && clk == base->clk)
		enqueue_timer(base, timer, idx, bucket_expiry);
	else
		internal_add_timer(base, timer);

out_unlock:
	raw_spin_unlock_irqrestore(&base->lock, flags);

	return ret;
}

/**
 * mod_timer_pending - Modify a pending timer's timeout
 * @timer:	The pending timer to be modified
 * @expires:	New absolute timeout in jiffies
 *
 * mod_timer_pending() is the same for pending timers as mod_timer(), but
 * will not activate inactive timers.
 *
 * If @timer->function == NULL then the start operation is silently
 * discarded.
 *
 * Return:
 * * %0 - The timer was inactive and not modified or was in
 *	  shutdown state and the operation was discarded
 * * %1 - The timer was active and requeued to expire at @expires
 */
/*
 * 中文契约：仅修改已经 pending 的 @timer，@expires 为绝对 jiffy；inactive 或
 * shutdown 返回 0，active 返回 1。函数不等待 callback，调用者必须另外管理
 * timer 所在对象的生命周期。适合“若仍在等就续期，但不要重新启动”的协议。
 */
int mod_timer_pending(struct timer_list *timer, unsigned long expires)
{
	return __mod_timer(timer, expires, MOD_TIMER_PENDING_ONLY);
}
EXPORT_SYMBOL(mod_timer_pending);

/**
 * mod_timer - Modify a timer's timeout
 * @timer:	The timer to be modified
 * @expires:	New absolute timeout in jiffies
 *
 * mod_timer(timer, expires) is equivalent to:
 *
 *     timer_delete(timer); timer->expires = expires; add_timer(timer);
 *
 * mod_timer() is more efficient than the above open coded sequence. In
 * case that the timer is inactive, the timer_delete() part is a NOP. The
 * timer is in any case activated with the new expiry time @expires.
 *
 * Note that if there are multiple unserialized concurrent users of the
 * same timer, then mod_timer() is the only safe way to modify the timeout,
 * since add_timer() cannot modify an already running timer.
 *
 * If @timer->function == NULL then the start operation is silently
 * discarded. In this case the return value is 0 and meaningless.
 *
 * Return:
 * * %0 - The timer was inactive and started or was in shutdown
 *	  state and the operation was discarded
 * * %1 - The timer was active and requeued to expire at @expires or
 *	  the timer was active and not modified because @expires did
 *	  not change the effective expiry time
 */
/*
 * 中文契约：以单次加锁状态转换启动或重排 @timer。它比 delete+add 安全，因为
 * 并发使用者不会看到中间 inactive 窗口。返回值描述入口是否 active，不代表
 * callback 是否曾经/正在运行；function==NULL 的 shutdown 对象静默返回 0。
 */
int mod_timer(struct timer_list *timer, unsigned long expires)
{
	return __mod_timer(timer, expires, 0);
}
EXPORT_SYMBOL(mod_timer);

/**
 * timer_reduce - Modify a timer's timeout if it would reduce the timeout
 * @timer:	The timer to be modified
 * @expires:	New absolute timeout in jiffies
 *
 * timer_reduce() is very similar to mod_timer(), except that it will only
 * modify an enqueued timer if that would reduce the expiration time. If
 * @timer is not enqueued it starts the timer.
 *
 * If @timer->function == NULL then the start operation is silently
 * discarded.
 *
 * Return:
 * * %0 - The timer was inactive and started or was in shutdown
 *	  state and the operation was discarded
 * * %1 - The timer was active and requeued to expire at @expires or
 *	  the timer was active and not modified because @expires
 *	  did not change the effective expiry time such that the
 *	  timer would expire earlier than already scheduled
 */
/*
 * 中文契约：仅在 @expires 更早时重排 active timer；inactive timer 会启动。
 * 多个路径都只想“收紧 deadline”时可避免较晚请求覆盖较早请求。返回类别与
 * mod_timer() 相同，且不提供 callback 完成保证。
 */
int timer_reduce(struct timer_list *timer, unsigned long expires)
{
	return __mod_timer(timer, expires, MOD_TIMER_REDUCE);
}
EXPORT_SYMBOL(timer_reduce);

/**
 * add_timer - Start a timer
 * @timer:	The timer to be started
 *
 * Start @timer to expire at @timer->expires in the future. @timer->expires
 * is the absolute expiry time measured in 'jiffies'. When the timer expires
 * timer->function(timer) will be invoked from soft interrupt context.
 *
 * The @timer->expires and @timer->function fields must be set prior
 * to calling this function.
 *
 * If @timer->function == NULL then the start operation is silently
 * discarded.
 *
 * If @timer->expires is already in the past @timer will be queued to
 * expire at the next timer tick.
 *
 * This can only operate on an inactive timer. Attempts to invoke this on
 * an active timer are rejected with a warning.
 */
/*
 * 中文契约：启动一个已初始化且 inactive 的 timer，期限取 timer->expires，
 * callback 在 TIMER_SOFTIRQ 中执行。重复 add 是调用者 bug 并告警；已过期期限
 * 被排到下一次 tick；shutdown timer 的请求被丢弃。无直接返回值。
 */
void add_timer(struct timer_list *timer)
{
	if (WARN_ON_ONCE(timer_pending(timer)))
		return;
	__mod_timer(timer, timer->expires, MOD_TIMER_NOTPENDING);
}
EXPORT_SYMBOL(add_timer);

/**
 * add_timer_local() - Start a timer on the local CPU
 * @timer:	The timer to be started
 *
 * Same as add_timer() except that the timer flag TIMER_PINNED is set.
 *
 * See add_timer() for further details.
 */
/*
 * 中文契约：与 add_timer() 相同，但设置 TIMER_PINNED，使本轮归当前 CPU 的
 * LOCAL base；这约束执行位置，不等于禁止 CPU hotplug 时的必要迁移。
 */
void add_timer_local(struct timer_list *timer)
{
	if (WARN_ON_ONCE(timer_pending(timer)))
		return;
	timer->flags |= TIMER_PINNED;
	__mod_timer(timer, timer->expires, MOD_TIMER_NOTPENDING);
}
EXPORT_SYMBOL(add_timer_local);

/**
 * add_timer_global() - Start a timer without TIMER_PINNED flag set
 * @timer:	The timer to be started
 *
 * Same as add_timer() except that the timer flag TIMER_PINNED is unset.
 *
 * See add_timer() for further details.
 */
/*
 * 中文契约：清除 TIMER_PINNED 后启动，使 timer 可放入 GLOBAL base 并参与
 * NO_HZ timer migration。用于显式撤销上轮 local/on-CPU 固定属性。
 */
void add_timer_global(struct timer_list *timer)
{
	if (WARN_ON_ONCE(timer_pending(timer)))
		return;
	timer->flags &= ~TIMER_PINNED;
	__mod_timer(timer, timer->expires, MOD_TIMER_NOTPENDING);
}
EXPORT_SYMBOL(add_timer_global);

/**
 * add_timer_on - Start a timer on a particular CPU
 * @timer:	The timer to be started
 * @cpu:	The CPU to start it on
 *
 * Same as add_timer() except that it starts the timer on the given CPU and
 * the TIMER_PINNED flag is set. When timer shouldn't be a pinned timer in
 * the next round, add_timer_global() should be used instead as it unsets
 * the TIMER_PINNED flag.
 *
 * See add_timer() for further details.
 */
/*
 * 中文契约：把 inactive @timer 固定到显式 @cpu 并启动。@cpu 必须是调用者已
 * 验证可用的 CPU 编号；无返回值。不等待旧 callback；若 timer 已 shutdown 则
 * 锁内静默退出。跨 base 时以 MIGRATING 协议保证查找者不会拿错锁。
 */
void add_timer_on(struct timer_list *timer, int cpu)
{
	struct timer_base *new_base, *base;
	unsigned long flags;

	debug_assert_init(timer);

	if (WARN_ON_ONCE(timer_pending(timer)))
		return;

	/* Make sure timer flags have TIMER_PINNED flag set */
	/* 先声明 pinned 语义，再据此选择 BASE_LOCAL；尚未入队所以可在锁外置位。 */
	timer->flags |= TIMER_PINNED;

	new_base = get_timer_cpu_base(timer->flags, cpu);

	/*
	 * If @timer was on a different CPU, it should be migrated with the
	 * old base locked to prevent other operations proceeding with the
	 * wrong base locked.  See lock_timer_base().
	 */
	/*
	 * 即使 timer inactive，flags 仍记录上次 base；必须先锁旧 base，再通过
	 * MIGRATING 切换，否则并发删除者可能按旧 flags 锁到错误对象。
	 */
	base = lock_timer_base(timer, &flags);
	/*
	 * Has @timer been shutdown? This needs to be evaluated while
	 * holding base lock to prevent a race against the shutdown code.
	 */
	/* shutdown 的 NULL function 与所有 rearm 在同一 base 锁下线性化。 */
	if (!timer->function)
		goto out_unlock;

	if (base != new_base) {
		timer->flags |= TIMER_MIGRATING;

		raw_spin_unlock(&base->lock);
		base = new_base;
		raw_spin_lock(&base->lock);
		WRITE_ONCE(timer->flags,
			   (timer->flags & ~TIMER_BASEMASK) | cpu);
	}
	forward_timer_base(base);

	debug_timer_activate(timer);
	internal_add_timer(base, timer);
out_unlock:
	raw_spin_unlock_irqrestore(&base->lock, flags);
}
EXPORT_SYMBOL_GPL(add_timer_on);

/**
 * __timer_delete - Internal function: Deactivate a timer
 * @timer:	The timer to be deactivated
 * @shutdown:	If true, this indicates that the timer is about to be
 *		shutdown permanently.
 *
 * If @shutdown is true then @timer->function is set to NULL under the
 * timer base lock which prevents further rearming of the time. In that
 * case any attempt to rearm @timer after this function returns will be
 * silently ignored.
 *
 * Return:
 * * %0 - The timer was not pending
 * * %1 - The timer was pending and deactivated
 */
/*
 * 中文契约：从时间轮摘除 @timer；@shutdown=true 还在同一把锁下把 function
 * 置 NULL，形成不可重启边界。返回 1 表示曾 pending，0 表示未 pending。
 * 不等待已经执行中的 callback，所以不会保证其关联对象可立即释放。
 */
static int __timer_delete(struct timer_list *timer, bool shutdown)
{
	struct timer_base *base;
	unsigned long flags;
	int ret = 0;

	debug_assert_init(timer);

	/*
	 * If @shutdown is set then the lock has to be taken whether the
	 * timer is pending or not to protect against a concurrent rearm
	 * which might hit between the lockless pending check and the lock
	 * acquisition. By taking the lock it is ensured that such a newly
	 * enqueued timer is dequeued and cannot end up with
	 * timer->function == NULL in the expiry code.
	 *
	 * If timer->function is currently executed, then this makes sure
	 * that the callback cannot requeue the timer.
	 */
	/*
	 * 普通 delete 可用无锁 pending 检查优化；shutdown 必须无条件拿锁，否则
	 * “检查 inactive -> 并发 rearm -> 写 NULL”会留下 pending 且无 callback
	 * 的 timer。锁也阻止正在运行的 callback 再次 rearm。
	 */
	if (timer_pending(timer) || shutdown) {
		base = lock_timer_base(timer, &flags);
		ret = detach_if_pending(timer, base, true);
		if (shutdown)
			timer->function = NULL;
		raw_spin_unlock_irqrestore(&base->lock, flags);
	}

	return ret;
}

/**
 * timer_delete - Deactivate a timer
 * @timer:	The timer to be deactivated
 *
 * The function only deactivates a pending timer, but contrary to
 * timer_delete_sync() it does not take into account whether the timer's
 * callback function is concurrently executed on a different CPU or not.
 * It neither prevents rearming of the timer.  If @timer can be rearmed
 * concurrently then the return value of this function is meaningless.
 *
 * Return:
 * * %0 - The timer was not pending
 * * %1 - The timer was pending and deactivated
 */
/*
 * 中文契约：只取消 pending，不等并发 callback，也不阻止随后 rearm。返回值
 * 仅在调用者已串行化所有 rearm 时有意义；因此它不能单独作为释放容器的屏障。
 */
int timer_delete(struct timer_list *timer)
{
	return __timer_delete(timer, false);
}
EXPORT_SYMBOL(timer_delete);

/**
 * timer_shutdown - Deactivate a timer and prevent rearming
 * @timer:	The timer to be deactivated
 *
 * The function does not wait for an eventually running timer callback on a
 * different CPU but it prevents rearming of the timer. Any attempt to arm
 * @timer after this function returns will be silently ignored.
 *
 * This function is useful for teardown code and should only be used when
 * timer_shutdown_sync() cannot be invoked due to locking or context constraints.
 *
 * Return:
 * * %0 - The timer was not pending
 * * %1 - The timer was pending
 */
/*
 * 中文契约：取消 pending 并永久封死 rearm，但不等待远端 callback。适用于因
 * 上下文/锁约束无法同步等待、且容器生命周期另有保障的 teardown；若要释放
 * 容器优先用 timer_shutdown_sync()。
 */
int timer_shutdown(struct timer_list *timer)
{
	return __timer_delete(timer, true);
}
EXPORT_SYMBOL_GPL(timer_shutdown);

/**
 * __try_to_del_timer_sync - Internal function: Try to deactivate a timer
 * @timer:	Timer to deactivate
 * @shutdown:	If true, this indicates that the timer is about to be
 *		shutdown permanently.
 *
 * If @shutdown is true then @timer->function is set to NULL under the
 * timer base lock which prevents further rearming of the timer. Any
 * attempt to rearm @timer after this function returns will be silently
 * ignored.
 *
 * This function cannot guarantee that the timer cannot be rearmed
 * right after dropping the base lock if @shutdown is false. That
 * needs to be prevented by the calling code if necessary.
 *
 * Return:
 * * %0  - The timer was not pending
 * * %1  - The timer was pending and deactivated
 * * %-1 - The timer callback function is running on a different CPU
 */
/*
 * 中文契约：在一次 base 锁临界区内尝试证明 timer 既不 pending 也不 running。
 * 返回 -1 表示 callback 正在执行，0/1 分别表示未 pending/成功摘除。shutdown
 * 只有在未 running 时才写 NULL；调用者负责遇到 -1 后等待并重试。
 */
static int __try_to_del_timer_sync(struct timer_list *timer, bool shutdown)
{
	struct timer_base *base;
	unsigned long flags;
	int ret = -1;

	debug_assert_init(timer);

	base = lock_timer_base(timer, &flags);

	if (base->running_timer != timer) {
		ret = detach_if_pending(timer, base, true);
		if (shutdown)
			timer->function = NULL;
	}

	raw_spin_unlock_irqrestore(&base->lock, flags);

	return ret;
}

/**
 * timer_delete_sync_try - Try to deactivate a timer
 * @timer:	Timer to deactivate
 *
 * This function tries to deactivate a timer. On success the timer is not
 * queued and the timer callback function is not running on any CPU.
 *
 * This function does not guarantee that the timer cannot be rearmed right
 * after dropping the base lock. That needs to be prevented by the calling
 * code if necessary.
 *
 * Return:
 * * %0  - The timer was not pending
 * * %1  - The timer was pending and deactivated
 * * %-1 - The timer callback function is running on a different CPU
 */
/*
 * 中文契约：非阻塞的同步删除尝试。成功的 0/1 同时保证返回瞬间 callback 未在
 * 任一 CPU 运行；-1 要求调用者稍后重试。它仍不能阻止返回后的并发 rearm。
 */
int timer_delete_sync_try(struct timer_list *timer)
{
	return __try_to_del_timer_sync(timer, false);
}
EXPORT_SYMBOL(timer_delete_sync_try);

#ifdef CONFIG_PREEMPT_RT
/*
 * expiry_lock helpers - PREEMPT_RT 下在 callback 整段外再套可睡眠自旋锁。
 *
 * init 仅启动期调用；lock/unlock 借用 @base。非 RT 为空操作。该锁不保护时间轮
 * 数据，那仍由 raw base->lock 负责；它只在可抢占 callback 与等待者间传递进度。
 */
static __init void timer_base_init_expiry_lock(struct timer_base *base)
{
	spin_lock_init(&base->expiry_lock);
}

static inline void timer_base_lock_expiry(struct timer_base *base)
{
	spin_lock(&base->expiry_lock);
}

static inline void timer_base_unlock_expiry(struct timer_base *base)
{
	spin_unlock(&base->expiry_lock);
}

/*
 * The counterpart to del_timer_wait_running().
 *
 * If there is a waiter for base->expiry_lock, then it was waiting for the
 * timer callback to finish. Drop expiry_lock and reacquire it. That allows
 * the waiter to acquire the lock and make progress.
 */
/*
 * 中文说明：若有同步删除者等待 expiry_lock，callback 完成后主动释放并重取两
 * 把锁，让高优先级 waiter 获得运行机会。函数返回时恢复“expiry_lock 后
 * base->lock”的原持锁状态，不把所有权交给调用者。
 */
static void timer_sync_wait_running(struct timer_base *base)
	__releases(&base->lock) __releases(&base->expiry_lock)
	__acquires(&base->expiry_lock) __acquires(&base->lock)
{
	if (atomic_read(&base->timer_waiters)) {
		raw_spin_unlock_irq(&base->lock);
		spin_unlock(&base->expiry_lock);
		spin_lock(&base->expiry_lock);
		raw_spin_lock_irq(&base->lock);
	}
}

/*
 * This function is called on PREEMPT_RT kernels when the fast path
 * deletion of a timer failed because the timer callback function was
 * running.
 *
 * This prevents priority inversion, if the softirq thread on a remote CPU
 * got preempted, and it prevents a life lock when the task which tries to
 * delete a timer preempted the softirq thread running the timer callback
 * function.
 */
/*
 * 中文说明：RT 上 softirq 是可调度线程，忙等可能让删除者反而饿死执行 callback
 * 的线程。非 IRQSAFE、非迁移 timer 通过 expiry_lock 睡眠等待一次回调临界区；
 * timer_waiters 通知 callback 端必须让锁。它可能睡眠。
 */
static void del_timer_wait_running(struct timer_list *timer)
{
	u32 tf;

	tf = READ_ONCE(timer->flags);
	if (!(tf & (TIMER_MIGRATING | TIMER_IRQSAFE))) {
		struct timer_base *base = get_timer_base(tf);

		/*
		 * Mark the base as contended and grab the expiry lock,
		 * which is held by the softirq across the timer
		 * callback. Drop the lock immediately so the softirq can
		 * expire the next timer. In theory the timer could already
		 * be running again, but that's more than unlikely and just
		 * causes another wait loop.
		 */
		/*
		 * 计数先于加锁，避免 callback 看不到 waiter；取得后立即释放，只把它
		 * 当作“先前 callback 已越过临界区”的完成栅栏，不长期持有。
		 */
		atomic_inc(&base->timer_waiters);
		spin_lock_bh(&base->expiry_lock);
		atomic_dec(&base->timer_waiters);
		spin_unlock_bh(&base->expiry_lock);
	}
}
#else
/* 非 RT 配置没有额外 expiry_lock；初始化为空操作。 */
static inline void timer_base_init_expiry_lock(struct timer_base *base) { }
/* 非 RT 配置只依赖 raw base->lock；加 expiry 锁为空操作。 */
static inline void timer_base_lock_expiry(struct timer_base *base) { }
/* 与上面的空加锁配对，无状态副作用。 */
static inline void timer_base_unlock_expiry(struct timer_base *base) { }
/* 非 RT callback 不需向可睡眠 waiter 主动让 expiry_lock。 */
static inline void timer_sync_wait_running(struct timer_base *base) { }
/* 非 RT 同步删除通过重试和 cpu_relax 等待，无可睡眠慢路径。 */
static inline void del_timer_wait_running(struct timer_list *timer) { }
#endif
/* 非 RT 内核 callback 不会被 RT 调度规则阻塞，空 helper 保留统一控制流。 */

/**
 * __timer_delete_sync - Internal function: Deactivate a timer and wait
 *			 for the handler to finish.
 * @timer:	The timer to be deactivated
 * @shutdown:	If true, @timer->function will be set to NULL under the
 *		timer base lock which prevents rearming of @timer
 *
 * If @shutdown is not set the timer can be rearmed later. If the timer can
 * be rearmed concurrently, i.e. after dropping the base lock then the
 * return value is meaningless.
 *
 * If @shutdown is set then @timer->function is set to NULL under timer
 * base lock which prevents rearming of the timer. Any attempt to rearm
 * a shutdown timer is silently ignored.
 *
 * If the timer should be reused after shutdown it has to be initialized
 * again.
 *
 * Return:
 * * %0	- The timer was not pending
 * * %1	- The timer was pending and deactivated
 */
/*
 * 中文契约：循环执行“锁内检查/摘除”，直至 callback 不再 running；shutdown
 * 还永久阻止 rearm。普通内核可能忙等，RT 非 IRQSAFE timer 的慢路径可睡眠。
 * 返回 0/1 表示入口最终观察到的 pending 状态；调用者必须阻止普通 delete_sync
 * 返回后的并发 rearm。
 */
static int __timer_delete_sync(struct timer_list *timer, bool shutdown)
{
	int ret;

#ifdef CONFIG_LOCKDEP
	unsigned long flags;

	/*
	 * If lockdep gives a backtrace here, please reference
	 * the synchronization rules above.
	 */
	/*
	 * 虚拟 lock map 把删除者持有的真实锁与 callback 的锁链关联起来，提前报告
	 * “删除者等 callback、callback 等删除者的锁”这种死锁。
	 */
	local_irq_save(flags);
	lock_map_acquire(&timer->lockdep_map);
	lock_map_release(&timer->lockdep_map);
	local_irq_restore(flags);
#endif
	/*
	 * don't use it in hardirq context, because it
	 * could lead to deadlock.
	 */
	/* 非 IRQSAFE callback 可能依赖当前硬中断返回后才能推进，硬中断内等待会死锁。 */
	WARN_ON(in_hardirq() && !(timer->flags & TIMER_IRQSAFE));

	/*
	 * Must be able to sleep on PREEMPT_RT because of the slowpath in
	 * del_timer_wait_running().
	 */
	/* RT 慢路径会拿可睡眠 expiry_lock，因此要求抢占处于可用状态。 */
	if (IS_ENABLED(CONFIG_PREEMPT_RT) && !(timer->flags & TIMER_IRQSAFE))
		lockdep_assert_preemption_enabled();

	/* -1 只表示“此刻 running”；等待/relax 后必须重新锁定并复核全部状态。 */
	do {
		ret = __try_to_del_timer_sync(timer, shutdown);

		if (unlikely(ret < 0)) {
			del_timer_wait_running(timer);
			cpu_relax();
		}
	} while (ret < 0);

	return ret;
}

/**
 * timer_delete_sync - Deactivate a timer and wait for the handler to finish.
 * @timer:	The timer to be deactivated
 *
 * Synchronization rules: Callers must prevent restarting of the timer,
 * otherwise this function is meaningless. It must not be called from
 * interrupt contexts unless the timer is an irqsafe one. The caller must
 * not hold locks which would prevent completion of the timer's callback
 * function. The timer's handler must not call add_timer_on(). Upon exit
 * the timer is not queued and the handler is not running on any CPU.
 *
 * For !irqsafe timers, the caller must not hold locks that are held in
 * interrupt context. Even if the lock has nothing to do with the timer in
 * question.  Here's why::
 *
 *    CPU0                             CPU1
 *    ----                             ----
 *                                     <SOFTIRQ>
 *                                       call_timer_fn();
 *                                       base->running_timer = mytimer;
 *    spin_lock_irq(somelock);
 *                                     <IRQ>
 *                                        spin_lock(somelock);
 *    timer_delete_sync(mytimer);
 *    while (base->running_timer == mytimer);
 *
 * Now timer_delete_sync() will never return and never release somelock.
 * The interrupt on the other CPU is waiting to grab somelock but it has
 * interrupted the softirq that CPU0 is waiting to finish.
 *
 * This function cannot guarantee that the timer is not rearmed again by
 * some concurrent or preempting code, right after it dropped the base
 * lock. If there is the possibility of a concurrent rearm then the return
 * value of the function is meaningless.
 *
 * If such a guarantee is needed, e.g. for teardown situations then use
 * timer_shutdown_sync() instead.
 *
 * Return:
 * * %0	- The timer was not pending
 * * %1	- The timer was pending and deactivated
 */
/*
 * 中文契约：返回时 timer 不在队列且 callback 不在任一 CPU 运行。@timer 为借用
 * 对象，函数不释放它；0/1 表示是否摘除了 pending 实例。调用者必须先禁止
 * rearm，且不能持 callback 获取的锁。英文示例展示典型死锁：删除者持
 * somelock 等 softirq，而 softirq 被一个同样等 somelock 的 IRQ 打断。
 */
int timer_delete_sync(struct timer_list *timer)
{
	return __timer_delete_sync(timer, false);
}
EXPORT_SYMBOL(timer_delete_sync);

/**
 * timer_shutdown_sync - Shutdown a timer and prevent rearming
 * @timer: The timer to be shutdown
 *
 * When the function returns it is guaranteed that:
 *   - @timer is not queued
 *   - The callback function of @timer is not running
 *   - @timer cannot be enqueued again. Any attempt to rearm
 *     @timer is silently ignored.
 *
 * See timer_delete_sync() for synchronization rules.
 *
 * This function is useful for final teardown of an infrastructure where
 * the timer is subject to a circular dependency problem.
 *
 * A common pattern for this is a timer and a workqueue where the timer can
 * schedule work and work can arm the timer. On shutdown the workqueue must
 * be destroyed and the timer must be prevented from rearming. Unless the
 * code has conditionals like 'if (mything->in_shutdown)' to prevent that
 * there is no way to get this correct with timer_delete_sync().
 *
 * timer_shutdown_sync() is solving the problem. The correct ordering of
 * calls in this case is:
 *
 *	timer_shutdown_sync(&mything->timer);
 *	workqueue_destroy(&mything->workqueue);
 *
 * After this 'mything' can be safely freed.
 *
 * This obviously implies that the timer is not required to be functional
 * for the rest of the shutdown operation.
 *
 * Return:
 * * %0 - The timer was not pending
 * * %1 - The timer was pending
 */
/*
 * 中文契约：teardown 的最终屏障。返回时同时满足“不 pending、不 running、
 * 以后 rearm 静默失败”；0/1 只报告此前是否 pending。timer/work 相互重启时，
 * 必须先封死 timer，再销毁 workqueue，随后才可释放共同容器。
 */
int timer_shutdown_sync(struct timer_list *timer)
{
	return __timer_delete_sync(timer, true);
}
EXPORT_SYMBOL_GPL(timer_shutdown_sync);

/*
 * call_timer_fn() - 在 trace/lockdep/preempt 完整性护栏内调用 timer callback。
 *
 * @timer/@fn 均为借用值；@baseclk 只用于 trace，单位 jiffy。调用时 base->lock
 * 已释放，callback 可重排自身；执行上下文仍是 softirq，通常不可睡眠。无返回
 * 值，副作用完全由 callback 决定。函数最后检测并修复 callback 泄漏的
 * preempt_count，使单个坏 callback 不至于立即污染全部后续 timer。
 */
static void call_timer_fn(struct timer_list *timer,
			  void (*fn)(struct timer_list *),
			  unsigned long baseclk)
{
	int count = preempt_count();

#ifdef CONFIG_LOCKDEP
	/*
	 * It is permissible to free the timer from inside the
	 * function that is called from it, this we need to take into
	 * account for lockdep too. To avoid bogus "held lock freed"
	 * warnings as well as problems when looking into
	 * timer->lockdep_map, make a copy and use that here.
	 */
	/*
	 * callback 可以释放包含 timer 的对象，因此不能在返回后再解引用 timer 内的
	 * lockdep_map；先复制到栈上，把诊断对象生命周期与业务对象解耦。
	 */
	struct lockdep_map lockdep_map;

	lockdep_copy_map(&lockdep_map, &timer->lockdep_map);
#endif
	/*
	 * Couple the lock chain with the lock chain at
	 * timer_delete_sync() by acquiring the lock_map around the fn()
	 * call here and in timer_delete_sync().
	 */
	/* 与 delete_sync 的同一虚拟锁配对，让 lockdep 建立等待边而非提供真实互斥。 */
	lock_map_acquire(&lockdep_map);

	trace_timer_expire_entry(timer, baseclk);
	fn(timer);
	trace_timer_expire_exit(timer);

	lock_map_release(&lockdep_map);

	if (count != preempt_count()) {
		WARN_ONCE(1, "timer: %pS preempt leak: %08x -> %08x\n",
			  fn, count, preempt_count());
		/*
		 * Restore the preempt count. That gives us a decent
		 * chance to survive and extract information. If the
		 * callback kept a lock held, bad luck, but not worse
		 * than the BUG() we had.
		 */
		/*
		 * 恢复计数仅是故障遏制；若 callback 还泄漏真实锁，系统仍可能出错，
		 * WARN 中的函数地址和前后计数用于定位根因。
		 */
		preempt_count_set(count);
	}
}

/*
 * expire_timers() - 执行一个或多个已到期桶中的全部 callback。
 *
 * @base 入口/出口均持有 base->lock；@head 是 collect 阶段移出的私有临时链表。
 * 函数逐个设置 running_timer、摘链并释放锁调用 callback。timer 自摘链后重新
 * 归其拥有者管理，callback 可以重新入队；running_timer 则让同步删除者知道
 * 旧实例尚未完成。
 */
static void expire_timers(struct timer_base *base, struct hlist_head *head)
{
	/*
	 * This value is required only for tracing. base->clk was
	 * incremented directly before expire_timers was called. But expiry
	 * is related to the old base->clk value.
	 */
	/* collect 后 clk 已加一；trace 要报告实际被消费的旧桶时钟。 */
	unsigned long baseclk = base->clk - 1;

	while (!hlist_empty(head)) {
		struct timer_list *timer;
		void (*fn)(struct timer_list *);

		timer = hlist_entry(head->first, struct timer_list, entry);

		base->running_timer = timer;
		detach_timer(timer, true);

		fn = timer->function;

		if (WARN_ON_ONCE(!fn)) {
			/* Should never happen. Emphasis on should! */
			/*
			 * 中文说明：pending timer 理论上绝不应带 NULL callback；shutdown
			 * 与入队由同一锁串行。告警后清 running 标记，避免错误扩大。
			 */
			base->running_timer = NULL;
			continue;
		}

		if (timer->flags & TIMER_IRQSAFE) {
			/*
			 * IRQSAFE callback 在 IRQ 仍关闭时运行，只临时放 base 锁；普通
			 * callback 则同时开 IRQ，允许中断推进但仍处 softirq 上下文。
			 */
			raw_spin_unlock(&base->lock);
			call_timer_fn(timer, fn, baseclk);
			raw_spin_lock(&base->lock);
			base->running_timer = NULL;
		} else {
			raw_spin_unlock_irq(&base->lock);
			call_timer_fn(timer, fn, baseclk);
			raw_spin_lock_irq(&base->lock);
			base->running_timer = NULL;
			timer_sync_wait_running(base);
		}
	}
}

/*
 * collect_expired_timers() - 按当前 next_expiry 从各相关层收集到期桶。
 *
 * @base 已锁；@heads 是至少 LVL_DEPTH 项的输出数组，获得被移出链表的临时
 * ownership。返回非空层数。函数把 base->clk 对齐到 next_expiry，并清对应
 * pending 位；高层仅在低层时钟跨越 8 倍边界时需要同时检查。
 */
static int collect_expired_timers(struct timer_base *base,
				  struct hlist_head *heads)
{
	unsigned long clk = base->clk = base->next_expiry;
	struct hlist_head *vec;
	int i, levels = 0;
	unsigned int idx;

	for (i = 0; i < LVL_DEPTH; i++) {
		idx = (clk & LVL_MASK) + i * LVL_SIZE;

		if (__test_and_clear_bit(idx, base->pending_map)) {
			vec = base->vectors + idx;
			hlist_move_list(vec, heads++);
			levels++;
		}
		/* Is it time to look at the next level? */
		/* 低 3 位非零说明尚未跨越上层粒度边界，更高层此刻不可能到期。 */
		if (clk & LVL_CLK_MASK)
			break;
		/* Shift clock for the next level granularity */
		/* 换成上一层自己的时钟坐标后计算同一绝对时刻的桶。 */
		clk >>= LVL_CLK_SHIFT;
	}
	return levels;
}

/*
 * Find the next pending bucket of a level. Search from level start (@offset)
 * + @clk upwards and if nothing there, search from start of the level
 * (@offset) up to @offset + clk.
 */
/*
 * 中文契约：在一层 64 桶中从当前 @clk 环形向前找第一个置位桶。@offset 是
 * 层在 pending_map 的起点；返回相对当前位置的桶距离，-1 表示该层为空。
 * 调用者持有 base->lock，故两段位图扫描之间桶集合不会变化。
 */
static int next_pending_bucket(struct timer_base *base, unsigned offset,
			       unsigned clk)
{
	unsigned pos, start = offset + clk;
	unsigned end = offset + LVL_SIZE;

	pos = find_next_bit(base->pending_map, end, start);
	if (pos < end)
		return pos - start;

	pos = find_next_bit(base->pending_map, start, offset);
	return pos < start ? pos + LVL_SIZE - start : -1;
}

/*
 * Search the first expiring timer in the various clock levels. Caller must
 * hold base->lock.
 *
 * Store next expiry time in base->next_expiry.
 */
/*
 * 中文契约：在锁内重建 base->next_expiry 和 timers_pending 缓存。函数逐层把
 * “离本层当前位置多少桶”换算回绝对 jiffy，选最早值；无 timer 时使用
 * base->clk+TIMER_NEXT_MAX_DELTA 哨兵。完成后清 next_expiry_recalc。
 */
static void timer_recalc_next_expiry(struct timer_base *base)
{
	unsigned long clk, next, adj;
	unsigned lvl, offset = 0;

	next = base->clk + TIMER_NEXT_MAX_DELTA;
	clk = base->clk;
	for (lvl = 0; lvl < LVL_DEPTH; lvl++, offset += LVL_SIZE) {
		int pos = next_pending_bucket(base, offset, clk & LVL_MASK);
		unsigned long lvl_clk = clk & LVL_CLK_MASK;

		if (pos >= 0) {
			unsigned long tmp = clk + (unsigned long) pos;

			tmp <<= LVL_SHIFT(lvl);
			if (time_before(tmp, next))
				next = tmp;

			/*
			 * If the next expiration happens before we reach
			 * the next level, no need to check further.
			 */
			/* 本层事件早于下一次上层边界时，更高层不可能给出更早结果。 */
			if (pos <= ((LVL_CLK_DIV - lvl_clk) & LVL_CLK_MASK))
				break;
		}
		/*
		 * Clock for the next level. If the current level clock lower
		 * bits are zero, we look at the next level as is. If not we
		 * need to advance it by one because that's going to be the
		 * next expiring bucket in that level. base->clk is the next
		 * expiring jiffy. So in case of:
		 *
		 * LVL5 LVL4 LVL3 LVL2 LVL1 LVL0
		 *  0    0    0    0    0    0
		 *
		 * we have to look at all levels @index 0. With
		 *
		 * LVL5 LVL4 LVL3 LVL2 LVL1 LVL0
		 *  0    0    0    0    0    2
		 *
		 * LVL0 has the next expiring bucket @index 2. The upper
		 * levels have the next expiring bucket @index 1.
		 *
		 * In case that the propagation wraps the next level the same
		 * rules apply:
		 *
		 * LVL5 LVL4 LVL3 LVL2 LVL1 LVL0
		 *  0    0    0    0    F    2
		 *
		 * So after looking at LVL0 we get:
		 *
		 * LVL5 LVL4 LVL3 LVL2 LVL1
		 *  0    0    0    1    0
		 *
		 * So no propagation from LVL1 to LVL2 because that happened
		 * with the add already, but then we need to propagate further
		 * from LVL2 to LVL3.
		 *
		 * So the simple check whether the lower bits of the current
		 * level are 0 or not is sufficient for all cases.
		 */
		/*
		 * 中文推导：上层桶表示向上对齐后的区间。低位非零时，上层当前位置必须
		 * 进一；传播发生环绕时继续用同一规则。这样不用级联搬 timer，也能计算
		 * 下一次应扫描的高层桶。
		 */
		adj = lvl_clk ? 1 : 0;
		clk >>= LVL_CLK_SHIFT;
		clk += adj;
	}

	WRITE_ONCE(base->next_expiry, next);
	base->next_expiry_recalc = false;
	base->timers_pending = !(next == base->clk + TIMER_NEXT_MAX_DELTA);
}

#ifdef CONFIG_NO_HZ_COMMON
/*
 * Check, if the next hrtimer event is before the next timer wheel
 * event:
 */
/*
 * cmp_next_hrtimer_event() - 合并低精度时间轮与非高分辨率 hrtimer 的最早事件。
 *
 * @basem/@expires 单位纳秒；返回应编程的 CLOCK_MONOTONIC 时刻。若 hrtimer
 * 已过期则立即返回 basem；否则向上对齐 tick，避免 NO_HZ 反复停/启 tick。
 */
static u64 cmp_next_hrtimer_event(u64 basem, u64 expires)
{
	u64 nextevt = ktime_to_ns(hrtimer_get_next_event());

	/*
	 * If high resolution timers are enabled
	 * hrtimer_get_next_event() returns KTIME_MAX.
	 */
	/* 高分辨率模式由独立硬件事件处理 hrtimer，KTIME_MAX 表示此处无需合并。 */
	if (expires <= nextevt)
		return expires;

	/*
	 * If the next timer is already expired, return the tick base
	 * time so the tick is fired immediately.
	 */
	/* 过期事件返回当前基准，要求调用者零延迟重编程。 */
	if (nextevt <= basem)
		return basem;

	/*
	 * Round up to the next jiffy. High resolution timers are
	 * off, so the hrtimers are expired in the tick and we need to
	 * make sure that this tick really expires the timer to avoid
	 * a ping pong of the nohz stop code.
	 *
	 * Use DIV_ROUND_UP_ULL to prevent gcc calling __divdi3
	 */
	/*
	 * 非高分辨率 hrtimer 只能随 tick 到期，向上取整保证这次 tick 真正覆盖期限；
	 * DIV_ROUND_UP_ULL 同时避免 32 位架构生成不可用的 64 位除法 helper。
	 */
	return DIV_ROUND_UP_ULL(nextevt, TICK_NSEC) * TICK_NSEC;
}

/*
 * next_timer_interrupt() - 返回一颗已锁 base 的下一时间轮事件。
 *
 * @basej 是当前 jiffy；必要时重算失效缓存。空 base 被规范化为远期哨兵，使多
 * base 比较可直接进行，也避免旧 next_expiry 到达时无意义地 raise softirq。
 */
static unsigned long next_timer_interrupt(struct timer_base *base,
					  unsigned long basej)
{
	if (base->next_expiry_recalc)
		timer_recalc_next_expiry(base);

	/*
	 * Move next_expiry for the empty base into the future to prevent an
	 * unnecessary raise of the timer softirq when the next_expiry value
	 * will be reached even if there is no timer pending.
	 *
	 * This update is also required to make timer_base::next_expiry values
	 * easy comparable to find out which base holds the first pending timer.
	 */
	/* timers_pending=false 时 next_expiry 只是缓存哨兵，不代表真实 timer。 */
	if (!base->timers_pending)
		WRITE_ONCE(base->next_expiry, basej + TIMER_NEXT_MAX_DELTA);

	return base->next_expiry;
}

/*
 * fetch_next_timer_interrupt() - 合并本 CPU LOCAL/GLOBAL base 的下一事件。
 *
 * 两颗 base 均已按顺序加锁；@tevt 输出纳秒级 local/global 期限，@basej 与
 * @basem 是同一时刻的 jiffy/monotonic 基准。返回最早 jiffy。global 字段只
 * 在 timer migration 层级需要代表可由其他 CPU 代管的事件。
 */
static unsigned long fetch_next_timer_interrupt(unsigned long basej, u64 basem,
						struct timer_base *base_local,
						struct timer_base *base_global,
						struct timer_events *tevt)
{
	unsigned long nextevt, nextevt_local, nextevt_global;
	bool local_first;

	nextevt_local = next_timer_interrupt(base_local, basej);
	nextevt_global = next_timer_interrupt(base_global, basej);

	local_first = time_before_eq(nextevt_local, nextevt_global);

	nextevt = local_first ? nextevt_local : nextevt_global;

	/*
	 * If the @nextevt is at max. one tick away, use @nextevt and store
	 * it in the local expiry value. The next global event is irrelevant in
	 * this case and can be left as KTIME_MAX.
	 */
	/*
	 * 一 tick 内无需复杂迁移：本 CPU 很快就会处理。若已经错过则钳到 basej，
	 * 避免 unsigned 差值变成巨大未来期限。
	 */
	if (time_before_eq(nextevt, basej + 1)) {
		/* If we missed a tick already, force 0 delta */
		/* 若期限早于采样基准，钳到 basej，避免无符号差值环绕成遥远未来。 */
		if (time_before(nextevt, basej))
			nextevt = basej;
		tevt->local = basem + (u64)(nextevt - basej) * TICK_NSEC;

		/*
		 * This is required for the remote check only but it doesn't
		 * hurt, when it is done for both call sites:
		 *
		 * * The remote callers will only take care of the global timers
		 *   as local timers will be handled by CPU itself. When not
		 *   updating tevt->global with the already missed first global
		 *   timer, it is possible that it will be missed completely.
		 *
		 * * The local callers will ignore the tevt->global anyway, when
		 *   nextevt is max. one tick away.
		 */
		/*
		 * 中文说明：远端调用者只代管 GLOBAL，故若错过的最早事件来自 GLOBAL，
		 * 必须同时写 global；本地调用者在此快速路径只看 local，不受影响。
		 */
		if (!local_first)
			tevt->global = tevt->local;
		return nextevt;
	}

	/*
	 * Update tevt.* values:
	 *
	 * If the local queue expires first, then the global event can be
	 * ignored. If the global queue is empty, nothing to do either.
	 */
	/* LOCAL 更早时 GLOBAL 无需上报迁移层级；先处理 LOCAL 后会重新计算。 */
	if (!local_first && base_global->timers_pending)
		tevt->global = basem + (u64)(nextevt_global - basej) * TICK_NSEC;

	if (base_local->timers_pending)
		tevt->local = basem + (u64)(nextevt_local - basej) * TICK_NSEC;

	return nextevt;
}

# ifdef CONFIG_SMP
/**
 * fetch_next_timer_interrupt_remote() - Store next timers into @tevt
 * @basej:	base time jiffies
 * @basem:	base time clock monotonic
 * @tevt:	Pointer to the storage for the expiry values
 * @cpu:	Remote CPU
 *
 * Stores the next pending local and global timer expiry values in the
 * struct pointed to by @tevt. If a queue is empty the corresponding
 * field is set to KTIME_MAX. If local event expires before global
 * event, global event is set to KTIME_MAX as well.
 *
 * Caller needs to make sure timer base locks are held (use
 * timer_lock_remote_bases() for this purpose).
 */
/*
 * 中文契约：读取远端 @cpu 的 LOCAL/GLOBAL 期限到 @tevt。调用者必须先用
 * timer_lock_remote_bases() 持两锁且关闭 IRQ；字段为空时保持 KTIME_MAX。
 * 函数不解锁、不取得 timer 引用。
 */
void fetch_next_timer_interrupt_remote(unsigned long basej, u64 basem,
				       struct timer_events *tevt,
				       unsigned int cpu)
{
	struct timer_base *base_local, *base_global;

	/* Preset local / global events */
	/* 先写空哨兵，后续只覆盖确实存在且需要上报的类别。 */
	tevt->local = tevt->global = KTIME_MAX;

	base_local = per_cpu_ptr(&timer_bases[BASE_LOCAL], cpu);
	base_global = per_cpu_ptr(&timer_bases[BASE_GLOBAL], cpu);

	lockdep_assert_held(&base_local->lock);
	lockdep_assert_held(&base_global->lock);

	fetch_next_timer_interrupt(basej, basem, base_local, base_global, tevt);
}

/**
 * timer_unlock_remote_bases - unlock timer bases of cpu
 * @cpu:	Remote CPU
 *
 * Unlocks the remote timer bases.
 */
/*
 * 中文契约：按加锁逆序释放 @cpu 的 GLOBAL、LOCAL base；IRQ 状态由外层管理。
 */
void timer_unlock_remote_bases(unsigned int cpu)
	__releases(timer_bases[BASE_LOCAL]->lock)
	__releases(timer_bases[BASE_GLOBAL]->lock)
{
	struct timer_base *base_local, *base_global;

	base_local = per_cpu_ptr(&timer_bases[BASE_LOCAL], cpu);
	base_global = per_cpu_ptr(&timer_bases[BASE_GLOBAL], cpu);

	raw_spin_unlock(&base_global->lock);
	raw_spin_unlock(&base_local->lock);
}

/**
 * timer_lock_remote_bases - lock timer bases of cpu
 * @cpu:	Remote CPU
 *
 * Locks the remote timer bases.
 */
/*
 * 中文契约：在 IRQ 已关闭条件下按固定顺序锁远端 @cpu 的 LOCAL、GLOBAL base。
 * 第二把使用 nested subclass 告诉 lockdep 这是规定的同类锁嵌套；调用者必须
 * 配对 timer_unlock_remote_bases()。
 */
void timer_lock_remote_bases(unsigned int cpu)
	__acquires(timer_bases[BASE_LOCAL]->lock)
	__acquires(timer_bases[BASE_GLOBAL]->lock)
{
	struct timer_base *base_local, *base_global;

	base_local = per_cpu_ptr(&timer_bases[BASE_LOCAL], cpu);
	base_global = per_cpu_ptr(&timer_bases[BASE_GLOBAL], cpu);

	lockdep_assert_irqs_disabled();

	raw_spin_lock(&base_local->lock);
	raw_spin_lock_nested(&base_global->lock, SINGLE_DEPTH_NESTING);
}

/**
 * timer_base_is_idle() - Return whether timer base is set idle
 *
 * Returns value of local timer base is_idle value.
 */
/* 中文契约：无锁读取当前 CPU LOCAL base 的 idle 标记；仅作状态提示，不是同步屏障。 */
bool timer_base_is_idle(void)
{
	return __this_cpu_read(timer_bases[BASE_LOCAL].is_idle);
}

static void __run_timer_base(struct timer_base *base);

/**
 * timer_expire_remote() - expire global timers of cpu
 * @cpu:	Remote CPU
 *
 * Expire timers of global base of remote CPU.
 */
/*
 * 中文契约：在当前 CPU 上驱动远端 @cpu 的 GLOBAL base 到期处理。只处理可迁移
 * timer，不触碰其 LOCAL/pinned base；内部自行加锁并执行 callback。
 */
void timer_expire_remote(unsigned int cpu)
{
	struct timer_base *base = per_cpu_ptr(&timer_bases[BASE_GLOBAL], cpu);

	__run_timer_base(base);
}

/*
 * timer_use_tmigr() - 让 timer migration 层级参与选择本 CPU 唤醒期限。
 *
 * @basej/@basem 是双时钟基准；@nextevt、@tick_stop_path、@tevt 为输入输出；
 * @timer_base_idle 选择 new_timer/deactivate/quick_check 协议。若本 CPU 是层级
 * 中最后的代理者，就把最早远端 global 事件提升为自己的 local 唤醒。
 */
static void timer_use_tmigr(unsigned long basej, u64 basem,
			    unsigned long *nextevt, bool *tick_stop_path,
			    bool timer_base_idle, struct timer_events *tevt)
{
	u64 next_tmigr;

	if (timer_base_idle)
		next_tmigr = tmigr_cpu_new_timer(tevt->global);
	else if (tick_stop_path)
		next_tmigr = tmigr_cpu_deactivate(tevt->global);
	else
		next_tmigr = tmigr_quick_check(tevt->global);

	/*
	 * If the CPU is the last going idle in timer migration hierarchy, make
	 * sure the CPU will wake up in time to handle remote timers.
	 * next_tmigr == KTIME_MAX if other CPUs are still active.
	 */
	/* KTIME_MAX 表示仍有其他 active CPU 可代理，本 CPU 无需为远端 timer 唤醒。 */
	if (next_tmigr < tevt->local) {
		u64 tmp;

		/* If we missed a tick already, force 0 delta */
		/* 已错过的纳秒期限钳到 basem，再换算为非负 jiffy delta。 */
		if (next_tmigr < basem)
			next_tmigr = basem;

		tmp = div_u64(next_tmigr - basem, TICK_NSEC);

		*nextevt = basej + (unsigned long)tmp;
		tevt->local = next_tmigr;
	}
}
# else
static void timer_use_tmigr(unsigned long basej, u64 basem,
			    unsigned long *nextevt, bool *tick_stop_path,
			    bool timer_base_idle, struct timer_events *tevt)
{
	/*
	 * Make sure first event is written into tevt->local to not miss a
	 * timer on !SMP systems.
	 */
	/* UP 无代理层级，global 与 local 都必须由唯一 CPU 自己处理。 */
	tevt->local = min_t(u64, tevt->local, tevt->global);
}
# endif /* CONFIG_SMP */

/*
 * __get_next_timer_interrupt() - NO_HZ 停 tick 前计算真实下一唤醒并可提交 idle。
 *
 * @basej/@basem 是同一采样点；@idle=NULL 仅查询，非 NULL 时既输入“tick 已停”
 * 状态又输出 base idle 状态。返回纳秒 monotonic 期限或 KTIME_MAX。函数同时锁
 * LOCAL/GLOBAL，合并迁移层级与 hrtimer，并在锁内发布 is_idle，封住入队漏唤醒。
 */
static inline u64 __get_next_timer_interrupt(unsigned long basej, u64 basem,
					     bool *idle)
{
	struct timer_events tevt = { .local = KTIME_MAX, .global = KTIME_MAX };
	struct timer_base *base_local, *base_global;
	unsigned long nextevt;
	bool idle_is_possible;

	/*
	 * When the CPU is offline, the tick is cancelled and nothing is supposed
	 * to try to stop it.
	 */
	/* offline CPU 不应进入停 tick 决策；告警后返回“无事件”避免继续操作 base。 */
	if (WARN_ON_ONCE(cpu_is_offline(smp_processor_id()))) {
		if (idle)
			*idle = true;
		return tevt.local;
	}

	base_local = this_cpu_ptr(&timer_bases[BASE_LOCAL]);
	base_global = this_cpu_ptr(&timer_bases[BASE_GLOBAL]);

	raw_spin_lock(&base_local->lock);
	raw_spin_lock_nested(&base_global->lock, SINGLE_DEPTH_NESTING);

	nextevt = fetch_next_timer_interrupt(basej, basem, base_local,
					     base_global, &tevt);

	/*
	 * If the next event is only one jiffy ahead there is no need to call
	 * timer migration hierarchy related functions. The value for the next
	 * global timer in @tevt struct equals then KTIME_MAX. This is also
	 * true, when the timer base is idle.
	 *
	 * The proper timer migration hierarchy function depends on the callsite
	 * and whether timer base is idle or not. @nextevt will be updated when
	 * this CPU needs to handle the first timer migration hierarchy
	 * event. See timer_use_tmigr() for detailed information.
	 */
	/*
	 * 一 tick 内不值得停 tick/修改迁移层级；更远时根据调用点和已有 idle 状态
	 * 选择 deactivate、new_timer 或 quick_check。
	 */
	idle_is_possible = time_after(nextevt, basej + 1);
	if (idle_is_possible)
		timer_use_tmigr(basej, basem, &nextevt, idle,
				base_local->is_idle, &tevt);

	/*
	 * We have a fresh next event. Check whether we can forward the
	 * base.
	 */
	/* 在仍持锁时前推两颗 base，减少下一次入队使用陈旧 clk 造成的粗粒度。 */
	__forward_timer_base(base_local, basej);
	__forward_timer_base(base_global, basej);

	/*
	 * Set base->is_idle only when caller is timer_base_try_to_set_idle()
	 */
	/* 纯查询 get_next_timer_interrupt() 不改变 idle 协议状态。 */
	if (idle) {
		/*
		 * Bases are idle if the next event is more than a tick
		 * away. Caution: @nextevt could have changed by enqueueing a
		 * global timer into timer migration hierarchy. Therefore a new
		 * check is required here.
		 *
		 * If the base is marked idle then any timer add operation must
		 * forward the base clk itself to keep granularity small. This
		 * idle logic is only maintained for the BASE_LOCAL and
		 * BASE_GLOBAL base, deferrable timers may still see large
		 * granularity skew (by design).
		 */
		/*
		 * 迁移层级可能把远端事件选为本地唤醒，故必须使用更新后的 nextevt 再判
		 * 一次。发布 idle 后，所有 add 路径负责前推 clk 并按需 IPI。
		 */
		if (!base_local->is_idle && time_after(nextevt, basej + 1)) {
			base_local->is_idle = true;
			/*
			 * Global timers queued locally while running in a task
			 * in nohz_full mode need a self-IPI to kick reprogramming
			 * in IRQ tail.
			 */
			/* nohz_full 的 GLOBAL 入队需要 IRQ tail 自 IPI 重编程，故同步标 idle。 */
			if (tick_nohz_full_cpu(base_local->cpu))
				base_global->is_idle = true;
			trace_timer_base_idle(true, base_local->cpu);
		}
		*idle = base_local->is_idle;

		/*
		 * When timer base is not set idle, undo the effect of
		 * tmigr_cpu_deactivate() to prevent inconsistent states - active
		 * timer base but inactive timer migration hierarchy.
		 *
		 * When timer base was already marked idle, nothing will be
		 * changed here.
		 */
		/*
		 * 若最终不能 idle，必须撤销此前 tmigr deactivate；否则会形成“base 活跃
		 * 但层级认为 inactive”的不一致，远端 timer 可能无人代理。
		 */
		if (!base_local->is_idle && idle_is_possible)
			tmigr_cpu_activate();
	}

	raw_spin_unlock(&base_global->lock);
	raw_spin_unlock(&base_local->lock);

	return cmp_next_hrtimer_event(basem, tevt.local);
}

/**
 * get_next_timer_interrupt() - return the time (clock mono) of the next timer
 * @basej:	base time jiffies
 * @basem:	base time clock monotonic
 *
 * Returns the tick aligned clock monotonic time of the next pending timer or
 * KTIME_MAX if no timer is pending. If timer of global base was queued into
 * timer migration hierarchy, first global timer is not taken into account. If
 * it was the last CPU of timer migration hierarchy going idle, first global
 * event is taken into account.
 */
/*
 * 中文契约：只查询下一次 timer 唤醒，不提交 base idle。@basej（jiffy）与
 * @basem（纳秒）必须对应同一时刻。返回 tick 对齐的 monotonic 期限；无事件
 * 返回 KTIME_MAX。已交给迁移层级代理的 global timer 通常不计入本 CPU。
 */
u64 get_next_timer_interrupt(unsigned long basej, u64 basem)
{
	return __get_next_timer_interrupt(basej, basem, NULL);
}

/**
 * timer_base_try_to_set_idle() - Try to set the idle state of the timer bases
 * @basej:	base time jiffies
 * @basem:	base time clock monotonic
 * @idle:	pointer to store the value of timer_base->is_idle on return;
 *		*idle contains the information whether tick was already stopped
 *
 * Returns the tick aligned clock monotonic time of the next pending timer or
 * KTIME_MAX if no timer is pending. When tick was already stopped KTIME_MAX is
 * returned as well.
 */
/*
 * 中文契约：NO_HZ 停 tick 路径的“计算并提交”版本。@idle 是输入输出：入口 true
 * 表示 tick 已停，直接返回 KTIME_MAX；否则锁内尝试发布 base->is_idle 并写回。
 * 返回下一 monotonic 期限。调用者继续负责实际时钟事件编程。
 */
u64 timer_base_try_to_set_idle(unsigned long basej, u64 basem, bool *idle)
{
	if (*idle)
		return KTIME_MAX;

	return __get_next_timer_interrupt(basej, basem, idle);
}

/**
 * timer_clear_idle - Clear the idle state of the timer base
 *
 * Called with interrupts disabled
 */
/*
 * 中文契约：CPU 退出 idle 时清本 CPU base 标记并重新激活迁移层级。必须在 IRQ
 * 关闭、不可迁移的本地上下文调用；无返回值。无锁写允许一次多余 IPI，换取
 * idle 热路径不拿 raw spinlock。
 */
void timer_clear_idle(void)
{
	int this_cpu = smp_processor_id();
	/*
	 * We do this unlocked. The worst outcome is a remote pinned timer
	 * enqueue sending a pointless IPI, but taking the lock would just
	 * make the window for sending the IPI a few instructions smaller
	 * for the cost of taking the lock in the exit from idle
	 * path. Required for BASE_LOCAL only.
	 */
	/*
	 * 与远端 pinned 入队竞态时，远端最多基于旧 true 多发一个 IPI；绝不会漏掉
	 * timer。拿锁只能略缩窗口，却会让每次 idle exit 付出更高固定成本。
	 */
	__this_cpu_write(timer_bases[BASE_LOCAL].is_idle, false);
	if (tick_nohz_full_cpu(this_cpu))
		__this_cpu_write(timer_bases[BASE_GLOBAL].is_idle, false);
	trace_timer_base_idle(false, this_cpu);

	/* Activate without holding the timer_base->lock */
	/* 层级实现自带同步；此处不能把 timer base 锁顺序扩散到迁移层级内部。 */
	tmigr_cpu_activate();
}
#endif

/**
 * __run_timers - run all expired timers (if any) on this CPU.
 * @base: the timer vector to be processed.
 */
/*
 * 中文契约：在已持 @base->lock 时反复收集并执行所有已到 next_expiry 的桶。
 * @base 为借用对象；无返回值。expire_timers() 会临时释放锁执行 callback，故每
 * 轮重新检查 jiffies/next_expiry。running_timer 非 NULL 表示同 base 已有执行者，
 * 当前调用直接退出以避免并行执行同一时间轮。
 */
static inline void __run_timers(struct timer_base *base)
{
	struct hlist_head heads[LVL_DEPTH];
	int levels;

	lockdep_assert_held(&base->lock);

	if (base->running_timer)
		return;

	while (time_after_eq(jiffies, base->clk) &&
	       time_after_eq(jiffies, base->next_expiry)) {
		levels = collect_expired_timers(base, heads);
		/*
		 * The two possible reasons for not finding any expired
		 * timer at this clk are that all matching timers have been
		 * dequeued or no timer has been queued since
		 * base::next_expiry was set to base::clk +
		 * TIMER_NEXT_MAX_DELTA.
		 */
	/*
	 * 空收集只有两种合法来源：匹配 timer 已被删除并标记重算，或 base 原本为空
	 * 且 next_expiry 是远期哨兵。其余情况说明位图/缓存不变量破坏。
	 */
		WARN_ON_ONCE(!levels && !base->next_expiry_recalc
			     && base->timers_pending);
		/*
		 * While executing timers, base->clk is set 1 offset ahead of
		 * jiffies to avoid endless requeuing to current jiffies.
		 */
	/*
	 * 先把 clk 推到下一 jiffy，再重算和调用 callback。回调若以“当前 jiffies”
	 * 重排自身，就会落到后续桶而不是再次进入正在消费的桶形成活锁。
	 */
		base->clk++;
		timer_recalc_next_expiry(base);

		while (levels--)
			expire_timers(base, heads + levels);
	}
}

/*
 * __run_timer_base() - 对一颗任意本地或远端 base 做快速到期检查并执行。
 *
 * 无锁 READ_ONCE 只用于早退；命中后用 expiry_lock（RT）和 base raw lock 复核
 * 完整状态。无返回值，可能执行任意 timer callback。
 */
static void __run_timer_base(struct timer_base *base)
{
	/* Can race against a remote CPU updating next_expiry under the lock */
	/*
	 * 中文说明：远端写由 WRITE_ONCE 发布；读到旧值最多导致多拿一次锁，读到尚未
	 * 到期值则后续 tick/唤醒会再检查，不用在每个 tick 强制锁住 base。
	 */
	if (time_before(jiffies, READ_ONCE(base->next_expiry)))
		return;

	timer_base_lock_expiry(base);
	raw_spin_lock_irq(&base->lock);
	__run_timers(base);
	raw_spin_unlock_irq(&base->lock);
	timer_base_unlock_expiry(base);
}

/*
 * run_timer_base() - 获取当前 CPU 指定类别 base 并驱动到期；无返回值。
 */
static void run_timer_base(int index)
{
	struct timer_base *base = this_cpu_ptr(&timer_bases[index]);

	__run_timer_base(base);
}

/*
 * This function runs timers and the timer-tq in bottom half context.
 */
/*
 * 中文说明：TIMER_SOFTIRQ 的总入口。依次处理 LOCAL、GLOBAL、DEF，随后让 timer
 * migration 执行本 CPU 代理的远端 global timer。softirq 上下文不可睡眠；
 * callback 的 IRQ 开关策略由 TIMER_IRQSAFE 决定。
 */
static __latent_entropy void run_timer_softirq(void)
{
	run_timer_base(BASE_LOCAL);
	if (IS_ENABLED(CONFIG_NO_HZ_COMMON)) {
		run_timer_base(BASE_GLOBAL);
		run_timer_base(BASE_DEF);

		if (is_timers_nohz_active())
			tmigr_handle_remote();
	}
}

/*
 * Called by the local, per-CPU timer interrupt on SMP.
 */
/*
 * 中文说明：本地 tick 硬中断的 timer 入口。先运行 hrtimer 队列，再无锁检查各
 * 低精度 base 是否需要 raise TIMER_SOFTIRQ。这里不直接执行普通 callback，
 * 将较长工作推迟到底半部以缩短硬中断临界段。
 */
static void run_local_timers(void)
{
	struct timer_base *base = this_cpu_ptr(&timer_bases[BASE_LOCAL]);

	hrtimer_run_queues();

	for (int i = 0; i < NR_BASES; i++, base++) {
		/*
		 * Raise the softirq only if required.
		 *
		 * timer_base::next_expiry can be written by a remote CPU while
		 * holding the lock. If this write happens at the same time than
		 * the lockless local read, sanity checker could complain about
		 * data corruption.
		 *
		 * There are two possible situations where
		 * timer_base::next_expiry is written by a remote CPU:
		 *
		 * 1. Remote CPU expires global timers of this CPU and updates
		 * timer_base::next_expiry of BASE_GLOBAL afterwards in
		 * next_timer_interrupt() or timer_recalc_next_expiry(). The
		 * worst outcome is a superfluous raise of the timer softirq
		 * when the not yet updated value is read.
		 *
		 * 2. A new first pinned timer is enqueued by a remote CPU
		 * and therefore timer_base::next_expiry of BASE_LOCAL is
		 * updated. When this update is missed, this isn't a
		 * problem, as an IPI is executed nevertheless when the CPU
		 * was idle before. When the CPU wasn't idle but the update
		 * is missed, then the timer would expire one jiffy late -
		 * bad luck.
		 *
		 * Those unlikely corner cases where the worst outcome is only a
		 * one jiffy delay or a superfluous raise of the softirq are
		 * not that expensive as doing the check always while holding
		 * the lock.
		 *
		 * Possible remote writers are using WRITE_ONCE(). Local reader
		 * uses therefore READ_ONCE().
		 */
		/*
		 * 中文并发说明：next_expiry 允许远端在锁下更新，而本地 tick 为性能无锁
		 * 读取。READ_ONCE/WRITE_ONCE 防撕裂但不是一致性快照；竞态最坏是多 raise
		 * 一次或 pinned timer 晚一个 jiffy，idle 情况另有 IPI 防止真正漏唤醒。
		 */
		if (time_after_eq(jiffies, READ_ONCE(base->next_expiry)) ||
		    (i == BASE_DEF && tmigr_requires_handle_remote())) {
			raise_timer_softirq(TIMER_SOFTIRQ);
			return;
		}
	}
}

/*
 * Called from the timer interrupt handler to charge one tick to the current
 * process.  user_tick is 1 if the tick is user time, 0 for system.
 */
/*
 * 中文契约：一次调度 tick 的总编排入口。@user_tick 表示 tick 落在用户态(1)
 * 或内核态(0)；无返回值。硬中断上下文中依次记账、触发 timer、推进 RCU/irq
 * work/调度器和 POSIX CPU timer。顺序确保当前 tick 的 CPU 时间先入账，再让
 * 调度与 CPU timer 基于更新后的统计做决定。
 */
void update_process_times(int user_tick)
{
	struct task_struct *p = current;

	/* Note: this timer irq context must be accounted for as well. */
	/* 当前 hardirq 消耗也归本 tick 的被中断任务，不能只推进时钟而漏记 CPU 时间。 */
	account_process_tick(p, user_tick);
	run_local_timers();
	rcu_sched_clock_irq(user_tick);
#ifdef CONFIG_IRQ_WORK
	if (in_hardirq())
		irq_work_tick();
#endif
	sched_tick();
	if (IS_ENABLED(CONFIG_POSIX_TIMERS))
		run_posix_cpu_timers();
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * migrate_timer_list() - 把一个旧桶的全部 timer 迁入在线 CPU 的 @new_base。
 *
 * 新旧 base 均由调用者持锁；@head 属于下线 CPU。逐项摘链但保持 pending 中间
 * 语义，改 CPU 位后按新 base->clk 重新选桶。无返回值，callback/对象所有权不变。
 */
static void migrate_timer_list(struct timer_base *new_base, struct hlist_head *head)
{
	struct timer_list *timer;
	int cpu = new_base->cpu;

	while (!hlist_empty(head)) {
		timer = hlist_entry(head->first, struct timer_list, entry);
		detach_timer(timer, false);
		timer->flags = (timer->flags & ~TIMER_BASEMASK) | cpu;
		internal_add_timer(new_base, timer);
	}
}

/*
 * timers_prepare_cpu() - 为即将上线的 @cpu 重置所有 timer base 运行时缓存。
 *
 * hotplug 状态机串行调用，base 尚无并发使用者；返回 0。以当前 jiffies 建立
 * clk，空队列使用远期 next_expiry，并清 pending/idle/recalc 状态。
 */
int timers_prepare_cpu(unsigned int cpu)
{
	struct timer_base *base;
	int b;

	for (b = 0; b < NR_BASES; b++) {
		base = per_cpu_ptr(&timer_bases[b], cpu);
		base->clk = jiffies;
		base->next_expiry = base->clk + TIMER_NEXT_MAX_DELTA;
		base->next_expiry_recalc = false;
		base->timers_pending = false;
		base->is_idle = false;
	}
	return 0;
}

/*
 * timers_dead_cpu() - 将下线 @cpu 的全部时间轮迁到当前在线 CPU。
 *
 * hotplug 全局串行保证不会有第三方同时做双 base 操作；返回 0。每个类别分别
 * 固定当前 CPU per-CPU 指针、按“新后旧”的已知嵌套加锁、前推新时钟、重排全部
 * 桶，最后 put_cpu_ptr() 恢复迁移状态。
 */
int timers_dead_cpu(unsigned int cpu)
{
	struct timer_base *old_base;
	struct timer_base *new_base;
	int b, i;

	for (b = 0; b < NR_BASES; b++) {
		old_base = per_cpu_ptr(&timer_bases[b], cpu);
		new_base = get_cpu_ptr(&timer_bases[b]);
		/*
		 * The caller is globally serialized and nobody else
		 * takes two locks at once, deadlock is not possible.
		 */
	/*
	 * 中文说明：CPU hotplug 锁排除了另一迁移者；普通 timer 路径一次只持一颗
	 * base 锁，故此处特定双锁顺序不会与其形成 ABBA。
	 */
		raw_spin_lock_irq(&new_base->lock);
		raw_spin_lock_nested(&old_base->lock, SINGLE_DEPTH_NESTING);

		/*
		 * The current CPUs base clock might be stale. Update it
		 * before moving the timers over.
		 */
	/* 迁入前前推目标 clk，否则 timer 会按陈旧 delta 落入过高层并额外延迟。 */
		forward_timer_base(new_base);

		WARN_ON_ONCE(old_base->running_timer);
		old_base->running_timer = NULL;

		for (i = 0; i < WHEEL_SIZE; i++)
			migrate_timer_list(new_base, old_base->vectors + i);

		raw_spin_unlock(&old_base->lock);
		raw_spin_unlock_irq(&new_base->lock);
		put_cpu_ptr(&timer_bases);
	}
	return 0;
}

#endif /* CONFIG_HOTPLUG_CPU */

/*
 * init_timer_cpu() - 初始化一个 possible CPU 的所有静态 timer base。
 *
 * @cpu 是合法 possible CPU；仅启动期调用，无并发、不可失败。写 CPU 身份，
 * 初始化 raw/RT 锁和时钟缓存；BSS 已保证链表、位图和布尔字段为零。
 */
static void __init init_timer_cpu(int cpu)
{
	struct timer_base *base;
	int i;

	for (i = 0; i < NR_BASES; i++) {
		base = per_cpu_ptr(&timer_bases[i], cpu);
		base->cpu = cpu;
		raw_spin_lock_init(&base->lock);
		base->clk = jiffies;
		base->next_expiry = base->clk + TIMER_NEXT_MAX_DELTA;
		timer_base_init_expiry_lock(base);
	}
}

/*
 * init_timer_cpus() - 遍历所有 possible CPU 完成 timer base 早期初始化。
 *
 * 无入参、无返回；possible 而非 online 确保后续 CPU 上线前锁对象已经可用。
 */
static void __init init_timer_cpus(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		init_timer_cpu(cpu);
}

/*
 * timers_init() - timer 子系统启动入口。
 *
 * 无入参、无返回，仅 init 阶段调用。先准备 per-CPU base，再初始化 POSIX CPU
 * timer work，最后注册 TIMER_SOFTIRQ；注册后 tick 才能把到期工作交给
 * run_timer_softirq()。
 */
void __init timers_init(void)
{
	init_timer_cpus();
	posix_cputimers_init_work();
	open_softirq(TIMER_SOFTIRQ, run_timer_softirq);
}
