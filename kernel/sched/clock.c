// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched_clock() for unstable CPU clocks
 *
 *  Copyright (C) 2008 Red Hat, Inc., Peter Zijlstra
 *
 *  Updates and enhancements:
 *    Copyright (C) 2008 Red Hat, Inc. Steven Rostedt <srostedt@redhat.com>
 *
 * Based on code by:
 *   Ingo Molnar <mingo@redhat.com>
 *   Guillaume Chazarain <guichaz@gmail.com>
 *
 *
 * What this file implements:
 *
 * cpu_clock(i) provides a fast (execution time) high resolution
 * clock with bounded drift between CPUs. The value of cpu_clock(i)
 * is monotonic for constant i. The timestamp returned is in nanoseconds.
 *
 * ######################### BIG FAT WARNING ##########################
 * # when comparing cpu_clock(i) to cpu_clock(j) for i != j, time can #
 * # go backwards !!                                                  #
 * ####################################################################
 *
 * There is no strict promise about the base, although it tends to start
 * at 0 on boot (but people really shouldn't rely on that).
 *
 * cpu_clock(i)       -- can be used from any context, including NMI.
 * local_clock()      -- is cpu_clock() on the current CPU.
 *
 * sched_clock_cpu(i)
 *
 * How it is implemented:
 *
 * The implementation either uses sched_clock() when
 * !CONFIG_HAVE_UNSTABLE_SCHED_CLOCK, which means in that case the
 * sched_clock() is assumed to provide these properties (mostly it means
 * the architecture provides a globally synchronized highres time source).
 *
 * Otherwise it tries to create a semi stable clock from a mixture of other
 * clocks, including:
 *
 *  - GTOD (clock monotonic)
 *  - sched_clock()
 *  - explicit idle events
 *
 * We use GTOD as base and use sched_clock() deltas to improve resolution. The
 * deltas are filtered to provide monotonicity and keeping it within an
 * expected window.
 *
 * Furthermore, explicit sleep and wakeup hooks allow us to account for time
 * that is otherwise invisible (TSC gets stopped).
 *
 */

/*
 * 本文件对外提供纳秒级调度时间：稳定架构直接使用全局 sched_clock；不稳定架构把
 * 每 CPU 原始时钟增量限制在 GTOD 窗口内，并在跨 CPU 读取时把两条时间线向较大值
 * 靠拢。它保证同一 CPU 的结果单调，但不保证任意两个 CPU 的瞬时值可直接排序；
 * idle、TSC 稳定性变化和 suspend 都通过专门入口修正或暂停采样。
 */

#include <linux/sched/clock.h>
#include "sched.h"

/*
 * sched_clock() —— 调度时钟的底层硬件读取接口，返回纳秒时间戳。
 *
 * 这是 __weak 默认实现：用 jiffies 折算出纳秒，精度仅 1/HZ（约 1~10ms）。
 * 各架构会用自己的高精度实现覆盖它：
 *   x86  → 读 TSC（Time Stamp Counter），精度 ~1ns
 *   ARM  → 读 arch timer 或 cycle counter，精度 ~1ns
 *
 * notrace：禁止 ftrace 插桩此函数，避免时钟读取本身触发 tracing 死递归。
 * __weak：允许架构用同名强符号覆盖此实现。
 */
notrace unsigned long long __weak sched_clock(void)
{
	/* (jiffies - INITIAL_JIFFIES) 得到自启动以来的 tick 数，
	 * 乘以每 tick 的纳秒数（NSEC_PER_SEC / HZ）折算为纳秒。
	 * 精度低但在硬件时钟就绪前作为保底实现。 */
	return (unsigned long long)(jiffies - INITIAL_JIFFIES)
					* (NSEC_PER_SEC / HZ);
}
EXPORT_SYMBOL_GPL(sched_clock);

/*
 * sched_clock_running：静态分支键，引用计数达到 2 时时钟完全就绪。
 *   - sched_clock_init()      将其从 0 增到 1（early，硬件时钟已就绪）
 *   - sched_clock_init_late() 将其从 1 增到 2（late，驱动可能修改稳定性后）
 * 读侧用 static_branch_likely() 检测，零开销快路径。
 */
static DEFINE_STATIC_KEY_FALSE(sched_clock_running);

#ifdef CONFIG_HAVE_UNSTABLE_SCHED_CLOCK
/*
 * We must start with !__sched_clock_stable because the unstable -> stable
 * transition is accurate, while the stable -> unstable transition is not.
 *
 * Similarly we start with __sched_clock_stable_early, thereby assuming we
 * will become stable, such that there's only a single 1 -> 0 transition.
 */
static DEFINE_STATIC_KEY_FALSE(__sched_clock_stable);
/* 早期先乐观假定稳定；驱动只需把它从 1 清到 0，避免来回切换造成裁决竞态。 */
static int __sched_clock_stable_early = 1;

/*
 * We want: ktime_get_ns() + __gtod_offset == sched_clock() + __sched_clock_offset
 */
__read_mostly u64 __sched_clock_offset;
static __read_mostly u64 __gtod_offset;

/*
 * 每 CPU 快照：tick_raw/tick_gtod 是同一 tick 附近的原始与 GTOD 样本，clock 是经
 * 单调和窗口过滤后的公开值。共享 cacheline 对齐减少不同 CPU 更新各自数据时的争用。
 */
struct sched_clock_data {
	u64			tick_raw;
	u64			tick_gtod;
	u64			clock;
};

static DEFINE_PER_CPU_SHARED_ALIGNED(struct sched_clock_data, sched_clock_data);

/* 仅在当前 CPU 已被固定的上下文中取得本 CPU 数据；返回借用指针，不提供额外同步。 */
static __always_inline struct sched_clock_data *this_scd(void)
{
	return this_cpu_ptr(&sched_clock_data);
}

/* 按编号取得任意 CPU 数据，远端字段必须由调用者用原子读改写协议访问。 */
notrace static inline struct sched_clock_data *cpu_sdc(int cpu)
{
	return &per_cpu(sched_clock_data, cpu);
}

/* static key 查询底层原始时钟是否已被最终判定为跨 CPU 稳定。 */
notrace int sched_clock_stable(void)
{
	return static_branch_likely(&__sched_clock_stable);
}

/* 在调用者提供的关中断/固定 CPU 条件下，尽量相邻地记录 GTOD 与原始时钟样本。 */
notrace static void __scd_stamp(struct sched_clock_data *scd)
{
	scd->tick_gtod = ktime_get_ns();
	scd->tick_raw = sched_clock();
}

/*
 * 完成不稳定到稳定的唯一精确切换：关本地中断读取配对样本，计算原始时钟偏移使
 * 切换前后连续，再启用稳定 static key 并允许 tick 停止 CLOCK_UNSTABLE 依赖。
 */
notrace static void __set_sched_clock_stable(void)
{
	struct sched_clock_data *scd;

	/*
	 * Since we're still unstable and the tick is already running, we have
	 * to disable IRQs in order to get a consistent scd->tick* reading.
	 */
	local_irq_disable();
	scd = this_scd();
	/*
	 * Attempt to make the (initial) unstable->stable transition continuous.
	 */
	__sched_clock_offset = (scd->tick_gtod + __gtod_offset) - (scd->tick_raw);
	local_irq_enable();

	printk(KERN_INFO "sched_clock: Marking stable (%lld, %lld)->(%lld, %lld)\n",
			scd->tick_gtod, __gtod_offset,
			scd->tick_raw,  __sched_clock_offset);

	static_branch_enable(&__sched_clock_stable);
	tick_dep_clear(TICK_DEP_BIT_CLOCK_UNSTABLE);
}

/*
 * If we ever get here, we're screwed, because we found out -- typically after
 * the fact -- that TSC wasn't good. This means all our clocksources (including
 * ktime) could have reported wrong values.
 *
 * What we do here is an attempt to fix up and continue sort of where we left
 * off in a coherent manner.
 *
 * The only way to fully avoid random clock jumps is to boot with:
 * "tsc=unstable".
 */
/*
 * 启动后才发现 TSC 不可靠时，既往时间可能已错误，无法完全修复。本工作项
 * 以当前 GTOD 重建一份统一快照并复制到所有 possible CPU，再关闭 irqtime 和稳定
 * static key；异步 work 上下文避免在报告不稳定的原子路径中直接修改 static key。
 */
notrace static void __sched_clock_work(struct work_struct *work)
{
	struct sched_clock_data *scd;
	int cpu;

	/* take a current timestamp and set 'now' */
	/* 固定当前 CPU，避免 this_scd() 的对象在两次采样与赋值之间改变。 */
	preempt_disable();
	scd = this_scd();
	__scd_stamp(scd);
	scd->clock = scd->tick_gtod + __gtod_offset;
	preempt_enable();

	/* clone to all CPUs */
	/* 此阶段以同一基线重新起步；后续每 CPU tick 再独立推进。 */
	for_each_possible_cpu(cpu)
		per_cpu(sched_clock_data, cpu) = *scd;

	printk(KERN_WARNING "TSC found unstable after boot, most likely due to broken BIOS. Use 'tsc=unstable'.\n");
	printk(KERN_INFO "sched_clock: Marking unstable (%lld, %lld)<-(%lld, %lld)\n",
			scd->tick_gtod, __gtod_offset,
			scd->tick_raw,  __sched_clock_offset);

	disable_sched_clock_irqtime();
	static_branch_disable(&__sched_clock_stable);
}

static DECLARE_WORK(sched_clock_work, __sched_clock_work);

/* 稳定键已启用时先阻止 CLOCK_UNSTABLE 下停 tick，再排队执行实际降级。 */
notrace static void __clear_sched_clock_stable(void)
{
	if (!sched_clock_stable())
		return;

	tick_dep_set(TICK_DEP_BIT_CLOCK_UNSTABLE);
	schedule_work(&sched_clock_work);
}

/*
 * 记录早期“不稳定”裁决，并用屏障与 late init 互锁：若 late init 已完成则立即请求
 * 降级，否则由 late init 观察 early=0 后保持不稳定路径，保证两边不会都漏做更新。
 */
notrace void clear_sched_clock_stable(void)
{
	__sched_clock_stable_early = 0;

	smp_mb(); /* matches sched_clock_init_late() */

	if (static_key_count(&sched_clock_running.key) == 2)
		__clear_sched_clock_stable();
}

/*
 * __sched_clock_gtod_offset() —— 计算 sched_clock() 与 ktime（GTOD）之间的偏移。
 *
 * 维持不变式：
 *   ktime_get_ns() + __gtod_offset  ==  sched_clock() + __sched_clock_offset
 *
 * 即让两条时间线在当前时刻对齐，后续才能用 sched_clock() 的增量来提升
 * ktime 的分辨率，同时又保持与墙上时间的可对比性。
 */
notrace static void __sched_clock_gtod_offset(void)
{
	/* 取本 CPU 的 per-CPU sched_clock_data，存储上次 tick 的快照 */
	struct sched_clock_data *scd = this_scd();

	/* 同时采样 tick_raw（sched_clock()读值）和 tick_gtod（ktime_get_ns()读值），
	 * 两次读取尽量靠近，缩小竞争窗口 */
	__scd_stamp(scd);

	/* 推导 __gtod_offset：
	 *   scd->tick_raw  + __sched_clock_offset  是 sched_clock 时间线当前值
	 *   scd->tick_gtod                          是 GTOD 时间线当前值
	 * 两者之差即为需要加在 GTOD 上才能等于 sched_clock 时间线的偏移量 */
	__gtod_offset = (scd->tick_raw + __sched_clock_offset) - scd->tick_gtod;
}

/*
 * sched_clock_init() —— 早期初始化（CONFIG_HAVE_UNSTABLE_SCHED_CLOCK 路径）。
 *
 * 在 late_time_init()（TSC/HPET 就绪）之后立即调用，完成两件事：
 *   1. 计算 __gtod_offset，让后续 sched_clock_tick() 从当前时刻无缝衔接；
 *   2. 将 sched_clock_running 引用计数 +1（0→1），激活 per-CPU 快路径。
 *
 * 此时仍是 UP（单处理器）阶段，TSC 即使有轻微误差也不会跨 CPU 漂移，
 * 所以直接采样是安全的。
 */
void __init sched_clock_init(void)
{
	/*
	 * Set __gtod_offset such that once we mark sched_clock_running,
	 * sched_clock_tick() continues where sched_clock() left off.
	 *
	 * Even if TSC is buggered, we're still UP at this point so it
	 * can't really be out of sync.
	 */
	/* 关中断保证 __scd_stamp() 里两次读取（tick_raw 和 tick_gtod）不被打断，
	 * 否则中断处理会污染采样值，导致 __gtod_offset 计算偏差 */
	local_irq_disable();
	__sched_clock_gtod_offset(); /* 计算并写入 __gtod_offset */
	local_irq_enable();

	/* 引用计数从 0 增到 1；sched_clock_cpu() 检测到 running>=1 后
	 * 才开始走 per-CPU sched_clock_data 快路径，而非直接返回裸 sched_clock() */
	static_branch_inc(&sched_clock_running);
}

/*
 * sched_clock_init_late() —— 晚期初始化，作为 late_initcall 运行。
 *
 * 必须在所有内建驱动初始化完成后执行，因为 acpi_processor、intel_idle
 * 等驱动可能在初始化时调用 mark_tsc_unstable() 修改时钟稳定性标志。
 * 只有等它们跑完，这里才能做最终裁决：时钟到底稳不稳定。
 */
static int __init sched_clock_init_late(void)
{
	/* 引用计数从 1 增到 2，表示时钟完全就绪（驱动已有机会修改稳定性）；
	 * sched_clock_cpu() 此后走完整的 per-CPU 稳定性判断逻辑 */
	static_branch_inc(&sched_clock_running);

	/*
	 * Ensure that it is impossible to not do a static_key update.
	 *
	 * Either {set,clear}_sched_clock_stable() must see sched_clock_running
	 * and do the update, or we must see their __sched_clock_stable_early
	 * and do the update, or both.
	 */
	/* 全内存屏障，与 set_sched_clock_stable() / clear_sched_clock_stable()
	 * 中的 smp_mb() 配对，防止编译器/CPU 重排导致双方都错过对方的写入，
	 * 从而漏掉一次 static_key 更新 */
	smp_mb(); /* matches {set,clear}_sched_clock_stable() */

	if (__sched_clock_stable_early)
		/* 驱动均未标记不稳定：将 __sched_clock_stable 静态分支设为 true，
		 * sched_clock_cpu() 走零开销快路径（直接 sched_clock() + offset） */
		__set_sched_clock_stable();
	else
		/* 有驱动标记了 TSC 不稳定：关闭 IRQ 时间统计（irqtime），
		 * 因为不稳定时钟会导致统计值异常跳变，不如不统计 */
		disable_sched_clock_irqtime();

	return 0;
}
/* late_initcall：在所有 device_initcall 之后、do_initcalls 最后一轮执行 */
late_initcall(sched_clock_init_late);

/*
 * min, max except they take wrapping into account
 */

/* 把 u64 差值解释为有符号距离，在模 2^64 时间线上选择较早值，正确处理自然回绕。 */

static __always_inline u64 wrap_min(u64 x, u64 y)
{
	return (s64)(x - y) < 0 ? x : y;
}

/* 与 wrap_min 对偶，在模 2^64 时间线上选择较晚值。 */
static __always_inline u64 wrap_max(u64 x, u64 y)
{
	return (s64)(x - y) > 0 ? x : y;
}

/*
 * update the percpu scd from the raw @now value
 *
 *  - filter out backward motion
 *  - use the GTOD tick value to create a window to filter crazy TSC values
 */
/*
 * 用当前 raw 增量推进单 CPU 过滤时钟：负增量钳为 0；结果下界是不倒退的旧 clock
 * 与 GTOD 基线较大者，上界是不超过一个 tick 的 GTOD 前沿或旧值。cmpxchg64 同时
 * 允许 NMI/普通上下文更新，失败便重新采样，成功返回该 CPU 新的单调值。
 */
static __always_inline u64 sched_clock_local(struct sched_clock_data *scd)
{
	u64 now, clock, old_clock, min_clock, max_clock, gtod;
	s64 delta;

again:
	now = sched_clock_noinstr();
	delta = now - scd->tick_raw;
	if (unlikely(delta < 0))
		delta = 0;

	old_clock = scd->clock;

	/*
	 * scd->clock = clamp(scd->tick_gtod + delta,
	 *		      max(scd->tick_gtod, scd->clock),
	 *		      scd->tick_gtod + TICK_NSEC);
	 */

	gtod = scd->tick_gtod + __gtod_offset;
	clock = gtod + delta;
	min_clock = wrap_max(gtod, old_clock);
	max_clock = wrap_max(old_clock, gtod + TICK_NSEC);

	clock = wrap_max(clock, min_clock);
	clock = wrap_min(clock, max_clock);

	if (!raw_try_cmpxchg64(&scd->clock, &old_clock, clock))
		goto again;

	return clock;
}

/*
 * noinstr 本地读取：稳定时直接返回 raw+offset；初始化前返回 raw；不稳定且已运行时
 * 通过 per-CPU 过滤器。调用者必须已经固定在当前 CPU，避免 this_scd() 指向变化。
 */
noinstr u64 local_clock_noinstr(void)
{
	u64 clock;

	if (static_branch_likely(&__sched_clock_stable))
		return sched_clock_noinstr() + __sched_clock_offset;

	if (!static_branch_likely(&sched_clock_running))
		return sched_clock_noinstr();

	clock = sched_clock_local(this_scd());

	return clock;
}

/* 用禁止抢占包住 noinstr 入口，为普通调用者提供当前 CPU 单调调度时钟。 */
u64 local_clock(void)
{
	u64 now;
	preempt_disable_notrace();
	now = local_clock_noinstr();
	preempt_enable_notrace();
	return now;
}
EXPORT_SYMBOL_GPL(local_clock);

/*
 * 读取远端 CPU 时先推进本地 clock，再比较本地与远端，使用原子 cmpxchg 把较小一侧
 * 向较大值靠拢并返回该值。这样一次跨 CPU 同步不会倒退任一参与者；32 位内核需用
 * cmpxchg64 防止 u64 撕裂，并在失败后重新执行本地读取以容纳 NMI 更新。
 */
static notrace u64 sched_clock_remote(struct sched_clock_data *scd)
{
	struct sched_clock_data *my_scd = this_scd();
	u64 this_clock, remote_clock;
	u64 *ptr, old_val, val;

#if BITS_PER_LONG != 64
again:
	/*
	 * Careful here: The local and the remote clock values need to
	 * be read out atomic as we need to compare the values and
	 * then update either the local or the remote side. So the
	 * cmpxchg64 below only protects one readout.
	 *
	 * We must reread via sched_clock_local() in the retry case on
	 * 32-bit kernels as an NMI could use sched_clock_local() via the
	 * tracer and hit between the readout of
	 * the low 32-bit and the high 32-bit portion.
	 */
	this_clock = sched_clock_local(my_scd);
	/*
	 * We must enforce atomic readout on 32-bit, otherwise the
	 * update on the remote CPU can hit in between the readout of
	 * the low 32-bit and the high 32-bit portion.
	 */
	remote_clock = cmpxchg64(&scd->clock, 0, 0);
#else
	/*
	 * On 64-bit kernels the read of [my]scd->clock is atomic versus the
	 * update, so we can avoid the above 32-bit dance.
	 */
	sched_clock_local(my_scd);
again:
	this_clock = my_scd->clock;
	remote_clock = scd->clock;
#endif

	/*
	 * Use the opportunity that we have both locks
	 * taken to couple the two clocks: we take the
	 * larger time as the latest time for both
	 * runqueues. (this creates monotonic movement)
	 */
	/* 借原子更新机会耦合两条时间线，把两者较大值作为共同的“最新”时间。 */
	if (likely((s64)(remote_clock - this_clock) < 0)) {
		ptr = &scd->clock;
		old_val = remote_clock;
		val = this_clock;
	} else {
		/*
		 * Should be rare, but possible:
		 */
		ptr = &my_scd->clock;
		old_val = this_clock;
		val = remote_clock;
	}

	if (!try_cmpxchg64(ptr, &old_val, val))
		goto again;

	return val;
}

/*
 * Similar to cpu_clock(), but requires local IRQs to be disabled.
 *
 * See cpu_clock().
 */
/*
 * 原文契约要求调用者关闭本地中断；函数内部再禁止抢占以固定 this CPU。稳定/未初始化
 * 路径直接读取，目标为远端时执行双时钟耦合，本地时只推进自己的过滤状态。
 */
notrace u64 sched_clock_cpu(int cpu)
{
	struct sched_clock_data *scd;
	u64 clock;

	if (sched_clock_stable())
		return sched_clock() + __sched_clock_offset;

	if (!static_branch_likely(&sched_clock_running))
		return sched_clock();

	preempt_disable_notrace();
	scd = cpu_sdc(cpu);

	if (cpu != smp_processor_id())
		clock = sched_clock_remote(scd);
	else
		clock = sched_clock_local(scd);
	preempt_enable_notrace();

	return clock;
}
EXPORT_SYMBOL_GPL(sched_clock_cpu);

/*
 * 每个调度 tick 在关中断条件下刷新本 CPU raw/GTOD 样本并推进过滤 clock；稳定时无需
 * per-CPU 校准，初始化前也不访问尚未建立的快照。
 */
notrace void sched_clock_tick(void)
{
	struct sched_clock_data *scd;

	if (sched_clock_stable())
		return;

	if (!static_branch_likely(&sched_clock_running))
		return;

	lockdep_assert_irqs_disabled();

	scd = this_scd();
	__scd_stamp(scd);
	sched_clock_local(scd);
}

/*
 * watchdog 在其锁内再次确认 TSC 稳定后更新 GTOD offset；若已经降级则不能再用坏
 * TSC 计算偏移。关中断保证两次底层采样不被本 CPU 的 tick 打断。
 */
notrace void sched_clock_tick_stable(void)
{
	if (!sched_clock_stable())
		return;

	/*
	 * Called under watchdog_lock.
	 *
	 * The watchdog just found this TSC to (still) be stable, so now is a
	 * good moment to update our __gtod_offset. Because once we find the
	 * TSC to be unstable, any computation will be computing crap.
	 */
	local_irq_disable();
	__sched_clock_gtod_offset();
	local_irq_enable();
}

/*
 * We are going deep-idle (IRQs are disabled):
 */
/* 进入深 idle 前在关中断状态推进一次本 CPU clock，保存停止计数器前的边界。 */
notrace void sched_clock_idle_sleep_event(void)
{
	sched_clock_cpu(smp_processor_id());
}
EXPORT_SYMBOL_GPL(sched_clock_idle_sleep_event);

/*
 * We just idled; resync with ktime.
 */
/*
 * idle 唤醒后，不稳定时钟通过 tick 与 ktime 重新对齐；timekeeping suspend
 * 期间 GTOD 不可作为有效基准而跳过。保存并恢复原中断状态，不假定调用点一定关中断。
 */
notrace void sched_clock_idle_wakeup_event(void)
{
	unsigned long flags;

	if (sched_clock_stable())
		return;

	if (unlikely(timekeeping_suspended))
		return;

	local_irq_save(flags);
	sched_clock_tick();
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(sched_clock_idle_wakeup_event);

#else /* !CONFIG_HAVE_UNSTABLE_SCHED_CLOCK: 架构提供全局同步的稳定高精度时钟 */

/*
 * sched_clock_init()（稳定时钟路径）。
 *
 * 当架构保证 sched_clock() 全局单调且跨 CPU 同步时（如 ARM64 的 arch timer），
 * 无需 per-CPU 漂移补偿机制，初始化更简单：
 *   1. 激活 sched_clock_running（引用计数 +1）；
 *   2. 调用 generic_sched_clock_init() 完成时钟注册和 wrap 定时器设置。
 */
void __init sched_clock_init(void)
{
	/* 标记时钟就绪，后续 sched_clock_cpu() 不再返回 0 */
	static_branch_inc(&sched_clock_running);
	/* generic_sched_clock_init() 内部会读取并更新 epoch，需关中断保证原子性 */
	local_irq_disable();
	generic_sched_clock_init(); /* 注册底层读函数、启动 wrap 防溢出 hrtimer */
	local_irq_enable();
}

/*
 * sched_clock_cpu()（稳定时钟路径）。
 *
 * 稳定时钟下所有 CPU 共享同一时间线，无需跨 CPU 补偿，
 * 直接返回 sched_clock() 即可。
 */
notrace u64 sched_clock_cpu(int cpu)
{
	/* 时钟未就绪（sched_clock_init 尚未执行）时返回 0，
	 * 避免调度器在极早期拿到无意义的时间戳 */
	if (!static_branch_likely(&sched_clock_running))
		return 0;

	/* 稳定时钟全局同步，直接读取即可，无需 per-CPU 漂移修正 */
	return sched_clock();
}

#endif /* !CONFIG_HAVE_UNSTABLE_SCHED_CLOCK */

/*
 * Running clock - returns the time that has elapsed while a guest has been
 * running.
 * On a guest this value should be local_clock minus the time the guest was
 * suspended by the hypervisor (for any reason).
 * On bare metal this function should return the same as local_clock.
 * Architectures and sub-architectures can override this.
 */
/*
 * running_clock 表示 guest 实际获准运行的时间，虚拟化实现应扣除 hypervisor
 * 挂起区间；裸机默认等同 local_clock。弱符号允许架构覆盖，函数不改变时钟状态。
 */
notrace u64 __weak running_clock(void)
{
	return local_clock();
}
