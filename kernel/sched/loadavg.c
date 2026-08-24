// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/sched/loadavg.c
 *
 * This file contains the magic bits required to compute the global loadavg
 * figure. Its a silly number but people think its important. We go through
 * great pains to make it work on big machines and tickless kernels.
 */
/* 本文件以分布式增量和定点指数衰减计算全局 1/5/15 分钟 loadavg，并兼容 NO_HZ。 */
#include <linux/sched/nohz.h>
#include "sched.h"

/*
 * Global load-average calculations
 *
 * We take a distributed and async approach to calculating the global load-avg
 * in order to minimize overhead.
 *
 * The global load average is an exponentially decaying average of nr_running +
 * nr_uninterruptible.
 *
 * Once every LOAD_FREQ:
 *
 *   nr_active = 0;
 *   for_each_possible_cpu(cpu)
 *	nr_active += cpu_of(cpu)->nr_running + cpu_of(cpu)->nr_uninterruptible;
 *
 *   avenrun[n] = avenrun[0] * exp_n + nr_active * (1 - exp_n)
 *
 * Due to a number of reasons the above turns in the mess below:
 *
 *  - for_each_possible_cpu() is prohibitively expensive on machines with
 *    serious number of CPUs, therefore we need to take a distributed approach
 *    to calculating nr_active.
 *
 *        \Sum_i x_i(t) = \Sum_i x_i(t) - x_i(t_0) | x_i(t_0) := 0
 *                      = \Sum_i { \Sum_j=1 x_i(t_j) - x_i(t_j-1) }
 *
 *    So assuming nr_active := 0 when we start out -- true per definition, we
 *    can simply take per-CPU deltas and fold those into a global accumulate
 *    to obtain the same result. See calc_load_fold_active().
 *
 *    Furthermore, in order to avoid synchronizing all per-CPU delta folding
 *    across the machine, we assume 10 ticks is sufficient time for every
 *    CPU to have completed this task.
 *
 *    This places an upper-bound on the IRQ-off latency of the machine. Then
 *    again, being late doesn't loose the delta, just wrecks the sample.
 *
 *  - cpu_rq()->nr_uninterruptible isn't accurately tracked per-CPU because
 *    this would add another cross-CPU cache-line miss and atomic operation
 *    to the wakeup path. Instead we increment on whatever CPU the task ran
 *    when it went into uninterruptible state and decrement on whatever CPU
 *    did the wakeup. This means that only the sum of nr_uninterruptible over
 *    all CPUs yields the correct result.
 *
 *  This covers the NO_HZ=n code, for extra head-aches, see the comment below.
 */
/*
 * 全局负载是 nr_running+nr_uninterruptible 的指数移动平均。为避免每 5 秒扫描所有 CPU，
 * 每个 rq 只折叠相对上次样本的 delta 到全局原子量，并给所有 CPU 10 tick 完成；迟到不会
 * 丢 delta，但会污染本次样本。uninterruptible 在睡眠 CPU 加、唤醒 CPU 减，只有全局和正确。
 * 以下先描述普通 tick；NO_HZ 另用双缓冲 delta 保留停 tick CPU 的贡献。
 */

/* Variables and functions for calc_load */
/* 全局 active 增量累计、下个采样窗和三个 FSHIFT 定点 loadavg；读者接受近似值。 */
atomic_long_t calc_load_tasks;
unsigned long calc_load_update;
unsigned long avenrun[3];
EXPORT_SYMBOL(avenrun); /* should be removed */
/* 原注释：此导出历史上应移除；当前仍是 ABI，学习补注不改变它。 */

/**
 * get_avenrun - get the load average array
 * @loads:	pointer to destination load array
 * @offset:	offset to add
 * @shift:	shift count to shift the result left
 *
 * These values are estimates at best, so no need for locking.
 */
/*
 * 把三个近似 loadavg 复制到调用者 @loads，先加 @offset 再左移 @shift；不加锁是因为
 * 这些值本就为估计，三个元素可来自不同更新时刻。无返回值，调用者保证数组容量。
 */
void get_avenrun(unsigned long *loads, unsigned long offset, int shift)
{
	loads[0] = (avenrun[0] + offset) << shift;
	loads[1] = (avenrun[1] + offset) << shift;
	loads[2] = (avenrun[2] + offset) << shift;
}

/*
 * 在持有 @this_rq 锁的采样路径计算 active=nr_running-adjust+nr_uninterruptible，返回
 * 相对 rq->calc_load_active 的有符号 delta，并提交新基线；无变化返回 0。adjust 用于
 * 排除当前事件的已知偏差。无睡眠，局部 nr_uninterruptible 可负但全局和有效。
 */
long calc_load_fold_active(struct rq *this_rq, long adjust)
{
	long nr_active, delta = 0;

	nr_active = this_rq->nr_running - adjust;
	nr_active += (long)this_rq->nr_uninterruptible;

	if (nr_active != this_rq->calc_load_active) {
		delta = nr_active - this_rq->calc_load_active;
		this_rq->calc_load_active = nr_active;
	}

	return delta;
}

/**
 * fixed_power_int - compute: x^n, in O(log n) time
 *
 * @x:         base of the power
 * @frac_bits: fractional bits of @x
 * @n:         power to raise @x to.
 *
 * By exploiting the relation between the definition of the natural power
 * function: x^n := x*x*...*x (x multiplied by itself for n times), and
 * the binary encoding of numbers used by computers: n := \Sum n_i * 2^i,
 * (where: n_i \elem {0, 1}, the binary vector representing n),
 * we find: x^n := x^(\Sum n_i * 2^i) := \Prod x^(n_i * 2^i), which is
 * of course trivially computable in O(log_2 n), the length of our binary
 * vector.
 */
/*
 * 用平方求幂在 O(log n) 内计算 Q@frac_bits 定点 x^n。result 从 1.0 开始；每次乘法
 * 加半单位再右移实现四舍五入。参数纯值，n=0 返回 1<<frac_bits，无失败或副作用。
 */
static unsigned long
fixed_power_int(unsigned long x, unsigned int frac_bits, unsigned int n)
{
	unsigned long result = 1UL << frac_bits;

	if (n) {
		for (;;) {
			if (n & 1) {
				result *= x;
				result += 1UL << (frac_bits - 1);
				result >>= frac_bits;
			}
			n >>= 1;
			if (!n)
				break;
			x *= x;
			x += 1UL << (frac_bits - 1);
			x >>= frac_bits;
		}
	}

	return result;
}

/*
 * a1 = a0 * e + a * (1 - e)
 *
 * a2 = a1 * e + a * (1 - e)
 *    = (a0 * e + a * (1 - e)) * e + a * (1 - e)
 *    = a0 * e^2 + a * (1 - e) * (1 + e)
 *
 * a3 = a2 * e + a * (1 - e)
 *    = (a0 * e^2 + a * (1 - e) * (1 + e)) * e + a * (1 - e)
 *    = a0 * e^3 + a * (1 - e) * (1 + e + e^2)
 *
 *  ...
 *
 * an = a0 * e^n + a * (1 - e) * (1 + e + ... + e^n-1) [1]
 *    = a0 * e^n + a * (1 - e) * (1 - e^n)/(1 - e)
 *    = a0 * e^n + a * (1 - e^n)
 *
 * [1] application of the geometric series:
 *
 *              n         1 - x^(n+1)
 *     S_n := \Sum x^i = -------------
 *             i=0          1 - x
 */
/*
 * 上述等比级数把连续 n 次相同 active 的 EMA 合并为一次：旧 load 乘 exp^n，新 active
 * 乘 1-exp^n。用于 NO_HZ 跨多个采样周期追赶；输入/返回均为 FSHIFT 定点值。
 */
unsigned long
calc_load_n(unsigned long load, unsigned long exp,
	    unsigned long active, unsigned int n)
{
	return calc_load(load, fixed_power_int(exp, FSHIFT, n), active);
}

#ifdef CONFIG_NO_HZ_COMMON
/*
 * Handle NO_HZ for the global load-average.
 *
 * Since the above described distributed algorithm to compute the global
 * load-average relies on per-CPU sampling from the tick, it is affected by
 * NO_HZ.
 *
 * The basic idea is to fold the nr_active delta into a global NO_HZ-delta upon
 * entering NO_HZ state such that we can include this as an 'extra' CPU delta
 * when we read the global state.
 *
 * Obviously reality has to ruin such a delightfully simple scheme:
 *
 *  - When we go NO_HZ idle during the window, we can negate our sample
 *    contribution, causing under-accounting.
 *
 *    We avoid this by keeping two NO_HZ-delta counters and flipping them
 *    when the window starts, thus separating old and new NO_HZ load.
 *
 *    The only trick is the slight shift in index flip for read vs write.
 *
 *        0s            5s            10s           15s
 *          +10           +10           +10           +10
 *        |-|-----------|-|-----------|-|-----------|-|
 *    r:0 0 1           1 0           0 1           1 0
 *    w:0 1 1           0 0           1 1           0 0
 *
 *    This ensures we'll fold the old NO_HZ contribution in this window while
 *    accumulating the new one.
 *
 *  - When we wake up from NO_HZ during the window, we push up our
 *    contribution, since we effectively move our sample point to a known
 *    busy state.
 *
 *    This is solved by pushing the window forward, and thus skipping the
 *    sample, for this CPU (effectively using the NO_HZ-delta for this CPU which
 *    was in effect at the time the window opened). This also solves the issue
 *    of having to deal with a CPU having been in NO_HZ for multiple LOAD_FREQ
 *    intervals.
 *
 * When making the ILB scale, we should try to pull this in as well.
 */
/*
 * NO_HZ 停止 per-CPU tick，CPU 入 idle 前把 active delta 写入全局双缓冲。采样窗开始时
 * 翻转读写槽：本窗消费旧贡献，同时新 idle 贡献写另一槽，避免窗内入 idle 抵消样本。
 * 窗内唤醒则把该 CPU 的下一采样点向后推，继续使用开窗时已折叠的 NO_HZ delta。
 */
/* 两个原子槽保存旧/新 NO_HZ delta；idx 由全局采样者翻转。 */
static atomic_long_t calc_load_nohz[2];
static int calc_load_idx;

/*
 * 选择 NO_HZ writer 槽。先读 idx、rmb，再读 calc_load_update；若采样窗已开始则写下一
 * 槽。屏障与 calc_global_nohz 的“先更新时间、wmb、再翻 idx”配对，避免双翻转。
 */
static inline int calc_load_write_idx(void)
{
	int idx = calc_load_idx;

	/*
	 * See calc_global_nohz(), if we observe the new index, we also
	 * need to observe the new update time.
	 */
	/* 观察到新 idx 时也必须观察到对应的新采样时间。 */
	smp_rmb();

	/*
	 * If the folding window started, make sure we start writing in the
	 * next NO_HZ-delta.
	 */
	if (!time_before(jiffies, READ_ONCE(calc_load_update)))
		idx++;

	return idx & 1;
}

/* 返回当前 reader 槽；仅全局采样路径消费，无额外屏障。 */
static inline int calc_load_read_idx(void)
{
	return calc_load_idx & 1;
}

/* 在 rq 锁保护下折叠 @rq active delta，并原子加到当前 NO_HZ writer 槽。 */
static void calc_load_nohz_fold(struct rq *rq)
{
	long delta;

	delta = calc_load_fold_active(rq, 0);
	if (delta) {
		int idx = calc_load_write_idx();

		atomic_long_add(delta, &calc_load_nohz[idx]);
	}
}

/* 当前 CPU 进入 NO_HZ 前把尚未上报的 active delta 折叠；无参数/返回值。 */
void calc_load_nohz_start(void)
{
	/*
	 * We're going into NO_HZ mode, if there's any pending delta, fold it
	 * into the pending NO_HZ delta.
	 */
	/* 进入 NO_HZ 前提交 pending delta，之后即使无 tick 也不会丢失。 */
	calc_load_nohz_fold(this_rq());
}

/*
 * Keep track of the load for NOHZ_FULL, must be called between
 * calc_load_nohz_{start,stop}().
 */
/* NOHZ_FULL 远程维护期间折叠指定 @rq；调用者保证位于 start/stop 区间及 rq 稳定。 */
void calc_load_nohz_remote(struct rq *rq)
{
	calc_load_nohz_fold(rq);
}

/*
 * 当前 CPU 离开 NO_HZ 时同步本 rq 的下一采样时间。若仍早于窗口直接返回；若在窗口
 * 及 10 tick 宽限内唤醒，则跳过本 CPU 本周期 tick 样本并推进一周期，避免重复计入。
 */
void calc_load_nohz_stop(void)
{
	struct rq *this_rq = this_rq();

	/*
	 * If we're still before the pending sample window, we're done.
	 */
	/* 先复制全局窗口；尚未到达则无需修正。 */
	this_rq->calc_load_update = READ_ONCE(calc_load_update);
	if (time_before(jiffies, this_rq->calc_load_update))
		return;

	/*
	 * We woke inside or after the sample window, this means we're already
	 * accounted through the nohz accounting, so skip the entire deal and
	 * sync up for the next window.
	 */
	/* 窗内唤醒已由 NO_HZ delta 代表，推进本地窗口避免 tick 再折叠一次。 */
	if (time_before(jiffies, this_rq->calc_load_update + 10))
		this_rq->calc_load_update += LOAD_FREQ;
}

/* 原子读取并清空当前 reader 槽；空槽快速返回 0，xchg 保证每份 delta 只消费一次。 */
static long calc_load_nohz_read(void)
{
	int idx = calc_load_read_idx();
	long delta = 0;

	if (atomic_long_read(&calc_load_nohz[idx]))
		delta = atomic_long_xchg(&calc_load_nohz[idx], 0);

	return delta;
}

/*
 * NO_HZ can leave us missing all per-CPU ticks calling
 * calc_load_fold_active(), but since a NO_HZ CPU folds its delta into
 * calc_load_nohz per calc_load_nohz_start(), all we need to do is fold
 * in the pending NO_HZ delta if our NO_HZ period crossed a load cycle boundary.
 *
 * Once we've updated the global active value, we need to apply the exponential
 * weights adjusted to the number of cycles missed.
 */
/*
 * 处理 NO_HZ 跨越多个 LOAD_FREQ 的追赶。超过窗口+10 tick 后计算遗漏周期 n，用当前
 * active 一次性推进三条 EMA，并更新 calc_load_update；随后 wmb 后翻槽，使 writer 看到
 * 新时间再看到新 idx。仅全局 timer 调用，无返回值。
 */
static void calc_global_nohz(void)
{
	unsigned long sample_window;
	long delta, active, n;

	sample_window = READ_ONCE(calc_load_update);
	if (!time_before(jiffies, sample_window + 10)) {
		/*
		 * Catch-up, fold however many we are behind still
		 */
		/* 计算至少一个仍欠缺的完整周期并批量衰减。 */
		delta = jiffies - sample_window - 10;
		n = 1 + (delta / LOAD_FREQ);

		active = atomic_long_read(&calc_load_tasks);
		active = active > 0 ? active * FIXED_1 : 0;

		avenrun[0] = calc_load_n(avenrun[0], EXP_1, active, n);
		avenrun[1] = calc_load_n(avenrun[1], EXP_5, active, n);
		avenrun[2] = calc_load_n(avenrun[2], EXP_15, active, n);

		WRITE_ONCE(calc_load_update, sample_window + n * LOAD_FREQ);
	}

	/*
	 * Flip the NO_HZ index...
	 *
	 * Make sure we first write the new time then flip the index, so that
	 * calc_load_write_idx() will see the new time when it reads the new
	 * index, this avoids a double flip messing things up.
	 */
	/* 先发布新时间，再翻 NO_HZ 槽；与 writer 的 rmb 配对。 */
	smp_wmb();
	calc_load_idx++;
}
#else /* !CONFIG_NO_HZ_COMMON: */

/* 无 NO_HZ 时没有额外 delta 或追赶工作。 */
static inline long calc_load_nohz_read(void) { return 0; }
static inline void calc_global_nohz(void) { }

#endif /* !CONFIG_NO_HZ_COMMON */

/*
 * calc_load - update the avenrun load estimates 10 ticks after the
 * CPUs have updated calc_load_tasks.
 *
 * Called from the global timer code.
 */
/*
 * 全局 timer 在窗口后 10 tick 调用：消费旧 NO_HZ delta，读取 active 原子和并夹到非负，
 * 更新三条定点 EMA并推进窗口，再让 calc_global_nohz 批量追赶遗漏周期。无锁近似统计；
 * 10 tick 宽限允许各 CPU tick 完成 delta 折叠。
 */
void calc_global_load(void)
{
	unsigned long sample_window;
	long active, delta;

	sample_window = READ_ONCE(calc_load_update);
	if (time_before(jiffies, sample_window + 10))
		return;

	/*
	 * Fold the 'old' NO_HZ-delta to include all NO_HZ CPUs.
	 */
	/* 消费旧槽并合入全局 active，使停 tick CPU 参与本次样本。 */
	delta = calc_load_nohz_read();
	if (delta)
		atomic_long_add(delta, &calc_load_tasks);

	active = atomic_long_read(&calc_load_tasks);
	active = active > 0 ? active * FIXED_1 : 0;

	avenrun[0] = calc_load(avenrun[0], EXP_1, active);
	avenrun[1] = calc_load(avenrun[1], EXP_5, active);
	avenrun[2] = calc_load(avenrun[2], EXP_15, active);

	WRITE_ONCE(calc_load_update, sample_window + LOAD_FREQ);

	/*
	 * In case we went to NO_HZ for multiple LOAD_FREQ intervals
	 * catch up in bulk.
	 */
	/* 长期 NO_HZ 可能跨多周期，使用合并公式追赶而非逐周期循环。 */
	calc_global_nohz();
}

/*
 * Called from sched_tick() to periodically update this CPU's
 * active count.
 */
/*
 * 每 CPU sched_tick 到达本 rq 采样点时折叠 active delta 到全局原子量，并推进本地
 * LOAD_FREQ。入口持 rq 锁；尚未到期快速返回。迟到不会丢 delta，但本次全局样本可能偏差。
 */
void calc_global_load_tick(struct rq *this_rq)
{
	long delta;

	if (time_before(jiffies, this_rq->calc_load_update))
		return;

	delta  = calc_load_fold_active(this_rq, 0);
	if (delta)
		atomic_long_add(delta, &calc_load_tasks);

	this_rq->calc_load_update += LOAD_FREQ;
}
