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
/*
 * 业务背景：调度与 tracing 在架构高精度时钟尚不可用时仍需一个不会递归插桩的启动期时间源。
 * 入参：无。
 * 出参/返回：返回自 INITIAL_JIFFIES 起的近似纳秒数，无输出参数、状态副作用或 ownership 变化。
 * 注意事项：精度只有一个 tick，基点不属于 ABI；架构强符号可覆盖，本函数在任意上下文均不睡眠。
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
/*
 * 业务背景：不稳定时钟过滤器需快速定位当前 CPU 的独立快照，避免跨 CPU cacheline 争用。
 * 入参：无。
 * 出参/返回：返回当前 CPU sched_clock_data 的非空借用指针，无输出参数或引用获取。
 * 注意事项：调用者必须已禁抢占/关中断以固定 CPU，并自行处理与 NMI/tick 的并发；函数不睡眠。
 */
static __always_inline struct sched_clock_data *this_scd(void)
{
	return this_cpu_ptr(&sched_clock_data);
}

/* 按编号取得任意 CPU 数据，远端字段必须由调用者用原子读改写协议访问。 */
/*
 * 业务背景：跨 CPU 时钟读取需要定位目标 CPU 的快照，再与本地时间线原子耦合。
 * 入参：cpu 是纯输入的有效 possible CPU 编号，范围 0..nr_cpu_ids-1。
 * 出参/返回：返回该 CPU sched_clock_data 的非空借用指针，不取得引用或改变 ownership。
 * 注意事项：不验证编号，调用者负责目标存活语义；远端 u64 必须按架构宽度使用原子访问，函数不睡眠。
 */
notrace static inline struct sched_clock_data *cpu_sdc(int cpu)
{
	return &per_cpu(sched_clock_data, cpu);
}

/* static key 查询底层原始时钟是否已被最终判定为跨 CPU 稳定。 */
/*
 * 业务背景：读时钟、tick 和 idle 路径需用同一低开销裁决选择全局直读或 per-CPU 过滤实现。
 * 入参：无。
 * 出参/返回：稳定 static key 启用返回非零，否则返回 0；无输出参数和副作用。
 * 注意事项：结果是瞬时配置状态而非锁；调用者不能据此推导其他字段的一致快照，函数不睡眠。
 */
notrace int sched_clock_stable(void)
{
	return static_branch_likely(&__sched_clock_stable);
}

/* 在调用者提供的关中断/固定 CPU 条件下，尽量相邻地记录 GTOD 与原始时钟样本。 */
/*
 * 业务背景：过滤器需要一对尽量同刻的 GTOD/raw 样本，才能用高分辨率增量约束漂移窗口。
 * 入参：scd 是不可空、借用且可写的当前 CPU 快照对象。
 * 出参/返回：无直接返回值；覆盖 tick_gtod/tick_raw，不修改 clock 或转移 ownership。
 * 注意事项：调用者须关本地中断并固定 CPU，两个读取仍非真正原子；notrace 路径不可睡眠。
 */
notrace static void __scd_stamp(struct sched_clock_data *scd)
{
	scd->tick_gtod = ktime_get_ns();
	scd->tick_raw = sched_clock();
}

/*
 * 完成不稳定到稳定的唯一精确切换：关本地中断读取配对样本，计算原始时钟偏移使
 * 切换前后连续，再启用稳定 static key 并允许 tick 停止 CLOCK_UNSTABLE 依赖。
 */
/*
 * 业务背景：驱动最终确认 TSC 可跨 CPU 信任后，需要无跳变地从 per-CPU 过滤切换到 raw 快路径。
 * 入参：无。
 * 出参/返回：无直接返回值；写全局 offset、启用稳定 static key 并清除 tick 不稳定依赖。
 * 注意事项：仅在 init/稳定性裁决路径且入口 IRQ 开启时调用；函数无条件重开 IRQ，static key 不可在 NMI 更新。
 */
notrace static void __set_sched_clock_stable(void)
{
	struct sched_clock_data *scd;

	/* 先冻结本 CPU tick 更新，读取同一代 GTOD/raw 基线并求连续切换偏移。 */
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

	/* offset 已发布后再记录裁决并启用快路径，读者不会看到尚未校准的稳定状态。 */
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
/*
 * 业务背景：晚发现不稳定时钟后必须在进程上下文重建各 CPU 基线并退出依赖 TSC 的快路径。
 * 入参：work 是工作队列传入的 sched_clock_work 借用指针，本实现不读取其内容且不可为空。
 * 出参/返回：无直接返回值；重写所有 possible CPU 快照、关闭 irqtime 和稳定 key，不转移 ownership。
 * 注意事项：可在工作队列上下文运行但本身不主动睡眠；复制期间只能恢复一致起点，不能修复历史跳变。
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
/*
 * 业务背景：原子稳定性报告路径不能直接修改 static key，需先保持 tick 活跃再把降级交给 workqueue。
 * 入参：无。
 * 出参/返回：无直接返回值；已不稳定时无副作用，否则设置 tick 依赖并排队一次降级工作。
 * 注意事项：schedule_work 只保证工作已排队/在途，不等待完成；函数不可睡眠且可被重复报告调用。
 */
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
/*
 * 业务背景：时钟驱动可在 late init 前后任意时刻判坏 TSC，本入口保证最终至少一方执行 static-key 降级。
 * 入参：无。
 * 出参/返回：无直接返回值；清 early 稳定标记，late init 已完成时还设置 tick 依赖并排队工作。
 * 注意事项：smp_mb 与 late init 配对，不能改成普通顺序写；异步返回不表示降级工作已完成。
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
/*
 * 业务背景：raw sched_clock 与 GTOD 基点不同，过滤器必须先求偏移才能连续拼接高分辨率增量。
 * 入参：无。
 * 出参/返回：无直接返回值；更新当前 CPU tick 样本和全局 __gtod_offset，不取得或释放对象。
 * 注意事项：调用者必须固定 CPU并关本地中断以缩小采样间隔；两次硬件读取仍可能有微小测量误差。
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
/*
 * 业务背景：调度时钟从启动期 raw 读切到 per-CPU 过滤前，需要建立 GTOD 偏移并发布“已早期就绪”。
 * 入参：无。
 * 出参/返回：无直接返回值；计算 offset 并把 sched_clock_running 引用从 0 增至 1。
 * 注意事项：仅启动期 UP 上下文调用且不睡眠；static key 计数与 late init 的第二次递增必须配对。
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
/*
 * 业务背景：所有内建驱动完成稳定性报告后，late init 才能最终选择全局稳定或 per-CPU 过滤路径。
 * 入参：无。
 * 出参/返回：总返回 0 供 initcall 框架继续；递增 running key，并启用稳定路径或关闭 irqtime。
 * 注意事项：只在 late_initcall 执行一次；全屏障与早期 set/clear 报告配对，函数不可重复调用。
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
/*
 * 业务背景：时钟值可自然回绕，普通无符号 min 无法表达回绕附近的先后顺序。
 * 入参：x、y 是同一 u64 模时间线上的纯输入时间戳，二者距离必须小于 2^63。
 * 出参/返回：返回按有符号环形距离判断的较早值，无输出参数或副作用。
 * 注意事项：距离达到或超过半个 u64 空间时顺序无定义；仅做算术且不睡眠。
 */

static __always_inline u64 wrap_min(u64 x, u64 y)
{
	return (s64)(x - y) < 0 ? x : y;
}

/* 与 wrap_min 对偶，在模 2^64 时间线上选择较晚值。 */
/*
 * 业务背景：过滤窗口还需在回绕时间线上选择较晚下界，保证公开时钟不会倒退。
 * 入参：x、y 是同一 u64 模时间线上的纯输入时间戳，二者距离必须小于 2^63。
 * 出参/返回：返回按有符号环形距离判断的较晚值，无输出参数、ownership 变化或副作用。
 * 注意事项：半空间之外不能可靠排序；本 helper 不提供并发同步且不睡眠。
 */
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
/*
 * 业务背景：不稳定 raw 时钟仍要为同一 CPU 提供高分辨率单调值，并把漂移限制在 GTOD 的一个 tick 窗口。
 * 入参：scd 是不可空、借用且可写的目标 CPU 快照；调用者保证对象永久存在且时间基线已初始化。
 * 出参/返回：返回成功原子发布的新纳秒 clock，并可能更新 scd->clock；不转移 ownership。
 * 注意事项：可与 NMI/tick 并发，必须保留 cmpxchg 重试；调用者固定相应 CPU/远端协议，函数不睡眠。
 */
static __always_inline u64 sched_clock_local(struct sched_clock_data *scd)
{
	u64 now, clock, old_clock, min_clock, max_clock, gtod;
	s64 delta;

again:
	/* 每次重试重新读取 raw，负 delta 视作不稳定计数器倒退而不允许公开 clock 回退。 */
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

	/* 用 GTOD 基线构造候选值及单调下界、一个 tick 前沿上界。 */
	gtod = scd->tick_gtod + __gtod_offset;
	clock = gtod + delta;
	min_clock = wrap_max(gtod, old_clock);
	max_clock = wrap_max(old_clock, gtod + TICK_NSEC);

	clock = wrap_max(clock, min_clock);
	clock = wrap_min(clock, max_clock);

	/* NMI 或 tick 抢先发布时 old_clock 被刷新，携新基线回到 again 重新裁剪。 */
	if (!raw_try_cmpxchg64(&scd->clock, &old_clock, clock))
		goto again;

	return clock;
}

/*
 * noinstr 本地读取：稳定时直接返回 raw+offset；初始化前返回 raw；不稳定且已运行时
 * 通过 per-CPU 过滤器。调用者必须已经固定在当前 CPU，避免 this_scd() 指向变化。
 */
/*
 * 业务背景：NMI/tracing 等不可插桩路径仍需读取当前 CPU 时钟，并依据初始化/稳定状态选择安全实现。
 * 入参：无。
 * 出参/返回：返回当前 CPU 纳秒调度时钟；不稳定路径可能原子推进本 CPU scd->clock，无 ownership 变化。
 * 注意事项：调用者必须固定 CPU；noinstr 禁止引入可插桩或睡眠调用，跨 CPU 可比性仍有限。
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
/*
 * 业务背景：普通内核代码需要无需自行固定 CPU 的本地调度时间读取封装。
 * 入参：无。
 * 出参/返回：返回当前 CPU 纳秒调度时钟，无输出参数或 ownership 变化；读取可能推进 per-CPU 过滤值。
 * 注意事项：短暂禁止抢占但不关中断，可在任意非睡眠读取上下文使用；不同 CPU 的值仍可能倒序。
 */
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
/*
 * 业务背景：读取另一 CPU 时要临时耦合两条各自单调的时间线，降低迁移/跨 CPU 事件出现倒退的概率。
 * 入参：scd 是不可空、借用且可写的远端 CPU 快照；本地 CPU 由已固定的调用上下文隐式提供。
 * 出参/返回：返回本地/远端较大纳秒值，并原子抬高较小一侧的 clock；对象 ownership 不变。
 * 注意事项：调用者关本地中断并固定 CPU；32 位必须原子读 u64，cmpxchg 失败需完整重试，函数不睡眠。
 */
static notrace u64 sched_clock_remote(struct sched_clock_data *scd)
{
	struct sched_clock_data *my_scd = this_scd();
	u64 this_clock, remote_clock;
	u64 *ptr, old_val, val;

#if BITS_PER_LONG != 64
again:
	/* 32 位必须分别以原子 helper 取得本地推进值和不撕裂的远端快照。 */
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
	/* 64 位自然原子读取允许先推进本地，再在重试标签仅重读两侧 clock。 */
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

	/* 只抬高较小一侧；失败说明并发更新已改变它，重新比较两条时间线。 */
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
/*
 * 业务背景：调度器和 tracing 需要按 CPU 编号读取单调时钟，并在不稳定架构上处理远端漂移。
 * 入参：cpu 是纯输入的有效 possible CPU 编号，范围 0..nr_cpu_ids-1。
 * 出参/返回：返回目标 CPU 的纳秒调度时钟；不稳定远端路径可能原子抬高本地或远端 clock。
 * 注意事项：调用者必须已关本地中断；函数不验证 cpu，结果不承诺任意两次独立跨 CPU 读取可全序。
 */
notrace u64 sched_clock_cpu(int cpu)
{
	struct sched_clock_data *scd;
	u64 clock;

	if (sched_clock_stable())
		return sched_clock() + __sched_clock_offset;

	if (!static_branch_likely(&sched_clock_running))
		return sched_clock();

	/* 完整过滤路径固定本 CPU，随后按目标是否远端选择单边推进或双边耦合。 */
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
/*
 * 业务背景：不稳定时钟依赖周期 tick 刷新 GTOD 窗口，才能持续限制 raw 漂移并维持本 CPU 单调性。
 * 入参：无。
 * 出参/返回：无直接返回值；需要时覆盖本 CPU tick 样本并推进 clock，稳定/未初始化时无副作用。
 * 注意事项：调用者必须关本地中断并固定 CPU；函数可从调度 tick 调用且不可睡眠。
 */
notrace void sched_clock_tick(void)
{
	struct sched_clock_data *scd;

	if (sched_clock_stable())
		return;

	if (!static_branch_likely(&sched_clock_running))
		return;

	/* 只有不稳定且已就绪时才要求 IRQ 契约并刷新本 CPU 三个时钟字段。 */
	lockdep_assert_irqs_disabled();

	scd = this_scd();
	__scd_stamp(scd);
	sched_clock_local(scd);
}

/*
 * watchdog 在其锁内再次确认 TSC 稳定后更新 GTOD offset；若已经降级则不能再用坏
 * TSC 计算偏移。关中断保证两次底层采样不被本 CPU 的 tick 打断。
 */
/*
 * 业务背景：clocksource watchdog 周期确认 TSC 仍可靠时，应重新校准 GTOD 偏移以限制长期基点漂移。
 * 入参：无。
 * 出参/返回：无直接返回值；稳定时刷新当前 CPU 样本和全局 GTOD offset，不稳定时无副作用。
 * 注意事项：调用者持 watchdog_lock；本函数局部关中断且不可睡眠，判坏后禁止再用 TSC 计算偏移。
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
/*
 * 业务背景：深 idle 可能停止底层计数器，进入前必须封存最后一个可见时刻供唤醒后与 ktime 重同步。
 * 入参：无。
 * 出参/返回：无直接返回值；读取并可能推进当前 CPU 过滤 clock，无输出参数或 ownership 变化。
 * 注意事项：调用者必须已关本地中断；只建立睡眠前边界，唤醒后的缺失时间由配对事件补齐。
 */
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
/*
 * 业务背景：底层计数器停走的 CPU 从深 idle 返回后，要用仍有效的 GTOD 重建过滤窗口避免时间停滞。
 * 入参：无。
 * 出参/返回：无直接返回值；不稳定且 timekeeping 活跃时刷新本 CPU 样本/clock，其余路径无副作用。
 * 注意事项：timekeeping suspend 时必须跳过；函数保存恢复原 IRQ 状态，不睡眠且不保证补回历史精度。
 */
notrace void sched_clock_idle_wakeup_event(void)
{
	unsigned long flags;

	if (sched_clock_stable())
		return;

	if (unlikely(timekeeping_suspended))
		return;

	/* 保存而非强假定 IRQ 状态，使 idle 调用点在不同体系结构上都能安全复用 tick 刷新。 */
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
/*
 * 业务背景：架构已保证全局同步时无需 per-CPU 漂移过滤，只需发布就绪并初始化通用回绕维护。
 * 入参：无。
 * 出参/返回：无直接返回值；递增 running static key 并初始化 generic sched_clock。
 * 注意事项：仅 !CONFIG_HAVE_UNSTABLE_SCHED_CLOCK 的启动期调用一次；初始化时局部关中断且不睡眠。
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
/*
 * 业务背景：全局同步架构无需按目标 CPU 校正，保留统一 API 让调用者不必区分配置。
 * 入参：cpu 是纯输入的 possible CPU 编号，但稳定实现不读取它且不取得任何引用。
 * 出参/返回：未初始化返回 0，已初始化返回全局 sched_clock 纳秒值；无状态副作用或输出参数。
 * 注意事项：调用者仍应传有效 CPU 并遵守公共接口的本地 IRQ 关闭契约；0 表示启动期而非错误码。
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
/*
 * 业务背景：guest 性能统计要排除被 hypervisor 暂停的墙上时间，裸机则可直接复用本地调度时钟。
 * 入参：无。
 * 出参/返回：默认返回 local_clock 的纳秒值；无输出参数或 ownership 变化，架构可用强符号覆盖。
 * 注意事项：默认实现未扣 guest steal/suspend 时间且不稳定路径可能推进 per-CPU 过滤值；基点不保证为 0。
 */
notrace u64 __weak running_clock(void)
{
	return local_clock();
}
