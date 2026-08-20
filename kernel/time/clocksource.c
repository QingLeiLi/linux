// SPDX-License-Identifier: GPL-2.0+
/*
 * This file contains the functions which manage clocksource drivers.
 *
 * Copyright (C) 2004, 2005 IBM, John Stultz (johnstul@us.ibm.com)
 */
/*
 * 本文件管理 clocksource 驱动对象：计算 cycle 与纳秒之间的定点比例，按 rating 维护候选表，选择并
 * 通知 timekeeping 当前计时源，用独立连续计数器 watchdog 检测频率漂移和跨 CPU 偏斜，并为 suspend、
 * sysfs 覆盖和驱动注册/注销提供生命周期边界。版权行仅说明来源，不参与运行期协议。
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/clocksource.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/prandom.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/topology.h>

#include "tick-internal.h"
#include "timekeeping_internal.h"

/* 注册路径稍后定义该有序插入 helper；前置声明只解决源码顺序，不创建另一套实现或状态。 */
static void clocksource_enqueue(struct clocksource *cs);

/*
 * cycles_to_nsec_safe() - 把两个 counter 快照间的有效周期差安全换成纳秒。
 *
 * 【调用位置】watchdog 的频率比较和 suspend 计时使用；调用者先取得同一 clocksource 的 start/end，
 * 本函数完成回绕、反向跳变过滤和防乘法溢出的换算，结果随后参与阈值判断或作为睡眠时长返回。
 *
 * 【参数与 ownership】@cs 是已注册或由 suspend/watchdog 路径稳定持有的 clocksource 借用指针；
 * @start/@end 是该对象 read() 得到的起止原始周期值，单位为硬件 cycle。函数不保存指针、不改字段。
 * 调用者负责用 clocksource_mutex、watchdog_lock 或系统 suspend 的单 CPU 阶段保证对象生命周期；
 * 本函数自身无锁、不睡眠，可在关中断的采样窗口之后调用。
 *
 * 【阶段与返回】先以 mask 做模减，并由 max_raw_delta 把疑似反向跳变压成 0；若 delta 小于
 * max_cycles，直接执行 (delta*mult)>>shift 的快速定点换算，否则用 mul_u64_u32_shr() 避免中间乘积
 * 溢出。返回非负纳秒数；0 既可能表示无时间经过，也可能表示 delta 被反向运动门限拒绝，无 errno。
 * noinline 保持这个防溢出慢路径为独立调用边界，避免把较重算术复制进 watchdog 热循环。
 */
static noinline u64 cycles_to_nsec_safe(struct clocksource *cs, u64 start, u64 end)
{
	/* delta 同时完成窄位计数器回绕模减与 max_raw_delta 反向运动过滤。 */
	u64 delta = clocksource_delta(end, start, cs->mask, cs->max_raw_delta);

	/* 已知乘积安全时使用最短路径；边界及以上交给不会溢出 u64 中间值的 helper。 */
	if (likely(delta < cs->max_cycles))
		return clocksource_cyc2ns(delta, cs->mult, cs->shift);

	return mul_u64_u32_shr(delta, cs->mult, cs->shift);
}

/**
 * clocks_calc_mult_shift - calculate mult/shift factors for scaled math of clocks
 * @mult:	pointer to mult variable
 * @shift:	pointer to shift variable
 * @from:	frequency to convert from
 * @to:		frequency to convert to
 * @maxsec:	guaranteed runtime conversion range in seconds
 *
 * The function evaluates the shift/mult pair for the scaled math
 * operations of clocksources and clockevents.
 *
 * @to and @from are frequency values in HZ. For clock sources @to is
 * NSEC_PER_SEC == 1GHz and @from is the counter frequency. For clock
 * event @to is the counter frequency and @from is NSEC_PER_SEC.
 *
 * The @maxsec conversion range argument controls the time frame in
 * seconds which must be covered by the runtime conversion with the
 * calculated mult and shift factors. This guarantees that no 64bit
 * overflow happens when the input value of the conversion is
 * multiplied with the calculated mult factor. Larger ranges may
 * reduce the conversion accuracy by choosing smaller mult and shift
 * factors.
 */
/*
 * 为 clocksource/clockevent 的缩放算术求 mult/shift。运行期近似公式为 output=(input*mult)>>shift；
 * @to 与 @from 都是 Hz。clocksource 通常从硬件频率 @from 转到 1GHz 纳秒频率 @to；clockevent 则
 * 反向把纳秒频率转为设备频率。@maxsec 指定必须安全覆盖的秒数，范围越大可能被迫降低有效精度。
 *
 * clocks_calc_mult_shift() - 在 64 位乘积安全约束下选择精度最高的 32 位定点比例。
 *
 * 【调用位置】clocksource 注册/调频、clockevent 配置和 sched_clock 等消费者调用；返回后调用者把
 * 两个输出字段发布给各自转换路径。本函数只算比例，不注册对象也不切换当前时钟。
 *
 * 【参数与 ownership】@mult/@shift 是调用者持有的非 NULL 输出指针，成功时分别写入乘数和二进制
 * 右移位数；@from 是非零输入频率，@to 是目标频率，单位均为 Hz；@maxsec 是运行期单次 delta 必须
 * 安全覆盖的秒数。所有参数均不保存，无引用或 ownership 转移；from=0 会触发除零，调用者必须拒绝。
 *
 * 【上下文与返回】纯整数运算、无锁、不可睡眠，返回无直接值且无失败码。输出保证按 maxsec 约束
 * 选择可容纳的最高 shift；整数除法加入 from/2 做最近舍入，但结果仍是有限精度近似。
 *
 * 【变量地图】tmp 先估算范围所占高位，后承载候选乘数；sftacc 是乘数可安全使用的有效位数；sft
 * 从 32 向下搜索候选 shift。二者共同保证输入最大周期数乘 mult 不溢出 u64。
 */
void
clocks_calc_mult_shift(u32 *mult, u32 *shift, u32 from, u32 to, u32 maxsec)
{
	u64 tmp;
	u32 sft, sftacc= 32;

	/*
	 * Calculate the shift factor which is limiting the conversion
	 * range:
	 */
	/*
	 * 阶段 1：先估算 maxsec*from 的最大输入周期数需要多少个高 32 位；每右移一位就减少一位可供
	 * mult 使用的精度，得到不会让“最大 delta × mult”越过 64 位的 sftacc 上限。
	 */
	tmp = ((u64)maxsec * from) >> 32;
	while (tmp) {
		tmp >>=1;
		sftacc--;
	}

	/*
	 * Find the conversion shift/mult pair which has the best
	 * accuracy and fits the maxsec conversion range:
	 */
	/*
	 * 阶段 2：从最大 shift=32 递减。候选 mult=round((to<<shift)/from)；第一个能装入 sftacc 有效位
	 * 的组合精度最高且满足范围约束，因此立即停止搜索。
	 */
	for (sft = 32; sft > 0; sft--) {
		tmp = (u64) to << sft;
		tmp += from / 2;
		do_div(tmp, from);
		if ((tmp >> sftacc) == 0)
			break;
	}
	/* 阶段 3（提交）：同时写回同一候选对；调用者之后必须把二者作为一个转换参数组使用。 */
	*mult = tmp;
	*shift = sft;
}
EXPORT_SYMBOL_GPL(clocks_calc_mult_shift);

/*[Clocksource internal variables]---------
 * curr_clocksource:
 *	currently selected clocksource.
 * suspend_clocksource:
 *	used to calculate the suspend time.
 * clocksource_list:
 *	linked list with the registered clocksources
 * clocksource_mutex:
 *	protects manipulations to curr_clocksource and the clocksource_list
 * override_name:
 *	Name of the user-specified clocksource.
 */
/*
 * clocksource 内部全局状态如下：curr_clocksource 是已经由 timekeeping_notify() 接受的当前选择；
 * suspend_clocksource 是跨 suspend 仍运行的最高 rating 计数器；clocksource_list 按 rating 从高到低
 * 链接已注册对象；clocksource_mutex 串行化列表、当前/挂起选择、用户 override 和注册/注销。
 * override_name 保存启动参数或 sysfs 指定的名字，空串表示自动选择。finished_booting 在 fs_initcall
 * 阶段由 0 提交为 1，用于推迟早期频繁重选；suspend_start 是冻结后的单 CPU suspend/resume 协议中
 * 保存的原始周期起点。列表只借用驱动拥有的 clocksource，对象注销成功前不得释放其内存。
 */
static struct clocksource *curr_clocksource;
static struct clocksource *suspend_clocksource;
static LIST_HEAD(clocksource_list);
static DEFINE_MUTEX(clocksource_mutex);
static char override_name[CS_NAME_LEN];
static int finished_booting;
static u64 suspend_start;

#ifdef CONFIG_CLOCKSOURCE_WATCHDOG
static void clocksource_watchdog_work(struct work_struct *work);
static void clocksource_select(void);

static LIST_HEAD(watchdog_list);
static struct clocksource *watchdog;
static struct timer_list watchdog_timer;
static DECLARE_WORK(watchdog_work, clocksource_watchdog_work);
static DEFINE_SPINLOCK(watchdog_lock);
static int watchdog_running;
static atomic_t watchdog_reset_pending;
/*
 * watchdog_list 链接所有带 MUST_VERIFY 或待降级的候选；watchdog 指向当前参考计数器；timer 固定在启动
 * CPU 周期扫描，work 只负责转入可调用 stop_machine 的临时 kthread。watchdog_lock（关本地中断）保护
 * 这张表、参考选择、timer/running 状态及相关 cs watchdog flags；注册路径锁序固定为
 * clocksource_mutex → watchdog_lock。watchdog_reset_pending 用 atomic 跨越异常/恢复上下文，请求下一次
 * 全表扫描丢弃旧基线；它不是列表锁。所有指针均借用已注册对象，注销必须先从这些结构摘除。
 */

/* Watchdog interval: 0.5sec. */
/* watchdog 每半秒扫描一次；NS 版本用于比较时长而不是安排 jiffy timer。 */
#define WATCHDOG_INTERVAL		(HZ >> 1)
#define WATCHDOG_INTERVAL_NS		(WATCHDOG_INTERVAL * (NSEC_PER_SEC / HZ))

/* Maximum time between two reference watchdog readouts */
/* 两次包夹参考时钟读数最多允许相隔 50 微秒，超出说明采样窗口受 IRQ/虚拟化等扰动，必须重试。 */
#define WATCHDOG_READOUT_MAX_NS		(50U * NSEC_PER_USEC)

/*
 * Maximum time between two remote readouts for NUMA=n. On NUMA enabled systems
 * the timeout is calculated from the numa distance.
 */
/* 非 NUMA 或默认场景允许本地/远端交接最多 50 微秒；NUMA 构建会再按节点距离缩放。 */
#define WATCHDOG_DEFAULT_TIMEOUT_NS	(50U * NSEC_PER_USEC)

/*
 * Remote timeout NUMA distance multiplier. The local distance is 10. The
 * default remote distance is 20. ACPI tables provide more accurate numbers
 * which are guaranteed to be greater than the local distance.
 *
 * This results in a 5us base value, which is equivalent to the above !NUMA
 * default.
 */
/*
 * NUMA 距离的本地基准是 10、常见远端值是 20，ACPI 可提供保证大于本地距离的更精确值。把默认
 * 50 微秒除以 10 得到每距离单位 5 微秒；默认远端距离 20 会得到 100 微秒，非 NUMA 仍用上方 50 微秒。
 */
#define WATCHDOG_NUMA_MULTIPLIER_NS	((u64)(WATCHDOG_DEFAULT_TIMEOUT_NS / LOCAL_DISTANCE))

/* Limit the NUMA timeout in case the distance values are insanely big */
/* 对异常巨大的固件距离值封顶为 500 微秒，避免远端 CPU 不响应时忙等过久。 */
#define WATCHDOG_NUMA_MAX_TIMEOUT_NS	((u64)(500U * NSEC_PER_USEC))

/* Shift values to calculate the approximate $N ppm of a given delta. */
/* 右移 11/8 近似取 delta 的 1/2048（约 500ppm）或 1/256（约 4000ppm）作为频率偏差门限。 */
#define SHIFT_500PPM			11
#define SHIFT_4000PPM			8

/* Number of attempts to read the watchdog */
/* 包夹采样最多重试三次，避免一次过长读取窗口就误判被测 clocksource。 */
#define WATCHDOG_FREQ_RETRIES		3

/* Five reads local and remote for inter CPU skew detection */
/* seq 从 0 交接到 10，对本地和远端各形成五次读数，用于捕获跨 CPU 原始 counter 倒退。 */
#define WATCHDOG_REMOTE_MAX_SEQ		10

/*
 * clocksource_watchdog_lock() - 保存本 CPU IRQ 状态并取得 watchdog 自旋锁。
 * @flags 是调用者持有的非 NULL 输出指针，写入 irqsave 状态供配对 unlock 使用；不转移 ownership。
 * 可从中断/原子上下文调用且不睡眠。成功无返回值；持锁后可修改 watchdog 表、选择和运行状态，但
 * 必须保持临界区短小，且若同时需要 clocksource_mutex 必须先取得 mutex。
 */
static inline void clocksource_watchdog_lock(unsigned long *flags)
{
	spin_lock_irqsave(&watchdog_lock, *flags);
}

/*
 * clocksource_watchdog_unlock() - 释放 watchdog_lock 并恢复本 CPU 进入临界区前的 IRQ 状态。
 * @flags 是同一次 lock 写出的纯输入值，按值读取、不保存。无返回值、不可睡眠；错配 flags 或未持锁
 * 会破坏中断状态及列表互斥，因此它只作为上方 helper 的严格配对出口。
 */
static inline void clocksource_watchdog_unlock(unsigned long *flags)
{
	spin_unlock_irqrestore(&watchdog_lock, *flags);
}

/* work 回调稍后创建这个一次性线程；声明只建立函数类型，data 在当前实现中不使用。 */
static int clocksource_watchdog_kthread(void *data);

/*
 * clocksource_watchdog_work() - 从系统 workqueue 跳板创建一次性 kwatchdog 线程。
 *
 * 【调用位置】clocksource 被标记 unstable 或 newly HRES 时 schedule_work()；workqueue 调用本函数，
 * 新线程随后在 clocksource_mutex 下重排 rating 并通知 timekeeping 重选。
 *
 * 【参数与上下文】@work 是静态 watchdog_work 的借用指针，当前不使用、不释放。运行在可睡眠的
 * workqueue 进程上下文，入口不持 watchdog_lock；函数不直接修改 clocksource 状态。
 *
 * 【返回与失败】返回无直接值。必须另建 kthread，因为重选会经 timekeeping_notify() 使用
 * stop_machine()，直接从 workqueue 调用会与 CPU hotplug 锁形成反转。该工作内核生命周期通常只触发
 * 一两次，故不保留永久线程。kthread_run() 的错误指针被有意忽略；下次 watchdog 扫描仍会看到标志并
 * 再次调度，不需要本回调回滚资源。
 */
static void clocksource_watchdog_work(struct work_struct *work)
{
	/*
	 * We cannot directly run clocksource_watchdog_kthread() here, because
	 * clocksource_select() calls timekeeping_notify() which uses
	 * stop_machine(). One cannot use stop_machine() from a workqueue() due
	 * lock inversions wrt CPU hotplug.
	 *
	 * Also, we only ever run this work once or twice during the lifetime
	 * of the kernel, so there is no point in creating a more permanent
	 * kthread for this.
	 *
	 * If kthread_run fails the next watchdog scan over the
	 * watchdog_list will find the unstable clock again.
	 */
	/*
	 * 不能在 workqueue 里直接执行重选：timekeeping_notify() 可能调用 stop_machine()，会与 CPU hotplug
	 * 锁发生反转。该路径内核存续期通常只运行一两次，临时线程比永久线程更合适；创建失败时不丢失
	 * cs 上的 UNSTABLE/RESELECT 标志，后续扫描会再次发现并重试。
	 */
	kthread_run(clocksource_watchdog_kthread, NULL, "kwatchdog");
}

/*
 * clocksource_change_rating() - 在有序注册表中原地改变一个已注册 clocksource 的 rating。
 * @cs 是调用者稳定持有且当前位于 clocksource_list 的借用指针；@rating 是新的选择优先级。
 * 调用者必须持 clocksource_mutex，并在 watchdog 线程路径中同时持 watchdog_lock；函数不可睡眠。
 * 它先摘除旧节点、写 rating，再按降序重新插入。无直接返回值；对象始终保持已注册且 ownership 不变。
 */
static void clocksource_change_rating(struct clocksource *cs, int rating)
{
	list_del(&cs->list);
	cs->rating = rating;
	clocksource_enqueue(cs);
}

/*
 * __clocksource_unstable() - 在 watchdog_lock 下提交 clocksource 的不稳定状态并安排延迟降级。
 *
 * 【调用位置】外部 mark_unstable 路径和周期 watchdog 结果处理调用；调用前必须持 watchdog_lock，
 * @cs 为借用且生命周期由注册表/驱动保证。函数在原子上下文运行，不能睡眠；驱动 mark_unstable 回调
 * 也因此必须遵守不可睡眠约束。
 *
 * 【状态变化】清除 VALID_FOR_HRES/WATCHDOG，设置 UNSTABLE。未注册对象的 list 为空，直接把 rating
 * 置 0 后返回；已注册对象先通知驱动，再在启动完成后调度 work，由 kwatchdog 在 mutex 下真正重排
 * 列表并重选。这样 spinlock 临界区不执行 stop_machine，也不提前释放对象。
 *
 * 【返回】无直接返回值、无错误码。返回时不稳定标志已经外界可见，但当前 timekeeper 可能尚未切换；
 * 延迟 work 是提交后的异步后半部，启动早期则由 clocksource_done_booting() 统一处理。
 */
static void __clocksource_unstable(struct clocksource *cs)
{
	cs->flags &= ~(CLOCK_SOURCE_VALID_FOR_HRES | CLOCK_SOURCE_WATCHDOG);
	cs->flags |= CLOCK_SOURCE_UNSTABLE;

	/*
	 * If the clocksource is registered clocksource_watchdog_kthread() will
	 * re-rate and re-select.
	 */
	/* 已注册对象由 kwatchdog 重新降 rating 并选择；空 list 表示尚未发布，只需预先把 rating 归零。 */
	if (list_empty(&cs->list)) {
		cs->rating = 0;
		return;
	}

	if (cs->mark_unstable)
		/* 回调只通知驱动硬件/私有状态，不能在当前 watchdog 自旋锁上下文睡眠。 */
		cs->mark_unstable(cs);

	/* kick clocksource_watchdog_kthread() */
	/* 启动完成后用 work→临时线程跨越到可睡眠重选上下文；早期由 done_booting 补做。 */
	if (finished_booting)
		schedule_work(&watchdog_work);
}

/**
 * clocksource_mark_unstable - mark clocksource unstable via watchdog
 * @cs:		clocksource to be marked unstable
 *
 * This function is called by the x86 TSC code to mark clocksources as unstable;
 * it defers demotion and re-selection to a kthread.
 */
/*
 * x86 TSC 等驱动可调用本接口把 clocksource 标成不稳定；函数只在 irqsave watchdog_lock 内发布标志，
 * 把 rating 降级与 timekeeping 重选延后到 kthread，因而可安全来自异常或原子上下文。
 *
 * clocksource_mark_unstable() - 对外提供幂等的不稳定标记入口。
 *
 * 【参数】@cs 是驱动持有的非 NULL clocksource 借用指针；调用期间对象必须有效，函数不增加模块引用。
 * 入口无需持锁、不会睡眠；内部保存并恢复本地 IRQ 状态。
 *
 * 【阶段与返回】若尚未标记且对象已注册但不在 watchdog_list，先把 wd_list 加入待处理表，确保异步线程
 * 能找到它；随后由 __clocksource_unstable() 发布 flags、通知驱动并安排 work。重复调用直接返回。
 * 返回无直接值、无失败码；返回不保证当前 clocksource 已切换，只保证降级请求不会丢失。
 */
void clocksource_mark_unstable(struct clocksource *cs)
{
	unsigned long flags;

	spin_lock_irqsave(&watchdog_lock, flags);
	if (!(cs->flags & CLOCK_SOURCE_UNSTABLE)) {
		if (!list_empty(&cs->list) && list_empty(&cs->wd_list))
			list_add(&cs->wd_list, &watchdog_list);
		__clocksource_unstable(cs);
	}
	spin_unlock_irqrestore(&watchdog_lock, flags);
}

/*
 * clocksource_reset_watchdog() - 使 watchdog_list 中所有候选在下轮重新建立参考基线。
 * 入参：无。调用者必须持 watchdog_lock；遍历的 @cs 是列表借用指针。函数只清 WATCHDOG“已有基线”位，
 * 不清 UNSTABLE、不摘链、不睡眠。无返回值；下一次 freq check 会走 reset 而非比较旧时间戳。
 */
static inline void clocksource_reset_watchdog(void)
{
	struct clocksource *cs;

	list_for_each_entry(cs, &watchdog_list, wd_list)
		cs->flags &= ~CLOCK_SOURCE_WATCHDOG;
}

enum wd_result {
	WD_SUCCESS,
	WD_FREQ_NO_WATCHDOG,
	WD_FREQ_TIMEOUT,
	WD_FREQ_RESET,
	WD_FREQ_SKEWED,
	WD_CPU_TIMEOUT,
	WD_CPU_SKEWED,
};
/*
 * wd_result 把一次扫描的终态分成：成功；缺少参考钟、采样窗口超时、重置基线或频率偏斜；远端 CPU
 * 交接超时或跨 CPU counter 偏斜。WD_SUCCESS 必须为 0，因为 watchdog_set_result() 用 !result 实现
 * “只接受第一个错误”。RESET/TIMEOUT 只要求下轮重试，SKEWED 才会把 clocksource 标成不稳定。
 */

/*
 * 每个目标 CPU 的 watchdog_cpu_data 是控制 CPU 与该远端 CPU 的一次乒乓采样邮箱。
 * @csd 放在首字段并由整个 per-CPU 对象 32 字节对齐，供 async call-single 入队；@remote_inprogress 与
 * CSD lock 位共同阻止上次远端回调未结束时复用邮箱；@result 保存该轮首个终态；@cpu_ts[0/1] 分别
 * 保存控制/远端最近读值；@cs 是本轮借用的被测源。@seq 是双方交接令牌，独占 cache line 以避免与
 * 高频时间戳写产生伪共享；@timeout_ns 由控制 CPU 按 NUMA 距离写入。对象静态 per-CPU 存续，不释放。
 */
struct watchdog_cpu_data {
	/* Keep first as it is 32 byte aligned */
	/* csd 必须保持首字段，才能维持 call-single 数据要求的 32 字节对齐；不可为注释便利调整布局。 */
	call_single_data_t	csd;
	atomic_t		remote_inprogress;
	enum wd_result		result;
	u64			cpu_ts[2];
	struct clocksource	*cs;
	/* Ensure that the sequence is in a separate cache line */
	/* seq 单独占 cache line，减少两个 CPU 反复 atomic_inc 时与结果/时间戳之间的缓存行争用。 */
	atomic_t		seq ____cacheline_aligned;
	/* Set by the control CPU according to NUMA distance */
	/* 控制 CPU 在派发 CSD 前按目标节点距离设置总忙等时限，远端只读取。 */
	u64			timeout_ns;
};

/*
 * watchdog_data 是当前扫描 CPU 持有的汇总结果：@lock 只保护从 per-CPU 邮箱提交 result/cpu_ts 的短
 * 临界区；@result 决定后续打印、重试或降级；@wd_seq 是参考钟包夹窗口，@wd_delta/@cs_delta 是两钟
 * 纳秒增量；@cpu_ts 是跨 CPU 原始周期快照；@curr_cpu 记住轮转检查的上个目标。对象 cache-line 对齐
 * 以隔离高频 watchdog 状态。列表和这些字段的外层扫描仍由 watchdog_lock 管理，raw lock 不替代它。
 */
struct watchdog_data {
	raw_spinlock_t	lock;
	enum wd_result	result;

	u64		wd_seq;
	u64		wd_delta;
	u64		cs_delta;
	u64		cpu_ts[2];

	unsigned int	curr_cpu;
} ____cacheline_aligned_in_smp;

/* 前置声明供下方 CSD_INIT 保存回调地址；完整上下文/并发契约见后面的函数定义。 */
static void watchdog_check_skew_remote(void *unused);

static DEFINE_PER_CPU_ALIGNED(struct watchdog_cpu_data, watchdog_cpu_data) = {
	.csd	= CSD_INIT(watchdog_check_skew_remote, NULL),
};

static struct watchdog_data watchdog_data = {
	.lock	= __RAW_SPIN_LOCK_UNLOCKED(watchdog_data.lock),
};
/* per-CPU 邮箱预置同一个远端回调且静态存续；全局汇总对象仅显式初始化 raw lock，其余零值即 SUCCESS。 */

/*
 * watchdog_set_result() - 以“首错获胜”规则终止当前跨 CPU 交接。
 * @wd 是本轮 per-CPU 邮箱借用指针；@result 必须是非 WD_SUCCESS 的终态。可从关中断的本地或远端
 * 原子上下文调用，不睡眠。guard(raw_spinlock) 在作用域退出自动解锁 watchdog_data.lock。
 * 若尚无错误，先把 seq 设为终止哨兵 10 唤开等待方，再以 WRITE_ONCE 发布结果；已有错误则不覆盖。
 * 返回无直接值；对象/clocksource ownership 不变，后续控制 CPU 在同一 raw lock 下汇总结果。
 */
static inline void watchdog_set_result(struct watchdog_cpu_data *wd, enum wd_result result)
{
	guard(raw_spinlock)(&watchdog_data.lock);
	if (!wd->result) {
		atomic_set(&wd->seq, WATCHDOG_REMOTE_MAX_SEQ);
		WRITE_ONCE(wd->result, result);
	}
}

/* Wait for the sequence number to hand over control. */
/* 等待 seq 达到约定值，把 counter 读取权从另一 CPU 交回当前 CPU；序号而非 wall time 决定正常交接。 */
/*
 * watchdog_wait_seq() - 在原子上下文等待对端完成上一拍，并检测错误或总超时。
 *
 * 【参数】@wd 是共享 per-CPU 邮箱借用指针；@start 是整轮开始的 raw-fast 纳秒值；@seq 是当前期望的
 * 递增令牌（2..9）。调用者已关闭本地中断或运行在远端 call-single 上，不持睡眠锁；函数只忙等。
 *
 * 【变量与阶段】cnt 是降低时钟读取频率的自旋批次计数，每约 5001 次才读取 nsecs；先检查对端发布的
 * 非 SUCCESS 结果，再检查 nsecs-start 是否达到 NUMA timeout，超时通过 watchdog_set_result() 发布
 * WD_CPU_TIMEOUT；其余循环执行 cpu_relax()。原子 seq 提供交接同步，result 仍用 READ/WRITE_ONCE 防止
 * 编译器合并访问，首错序列化由 watchdog_data.lock 完成。
 *
 * 【返回】达到正常 seq 且未见终止哨兵时返回 true；对端错误、当前超时或 seq 被设为最大哨兵时返回
 * false。失败已写入共享结果或保留对端结果，调用者立即停止本侧采样，无资源回滚。
 */
static bool watchdog_wait_seq(struct watchdog_cpu_data *wd, u64 start, int seq)
{
	for(int cnt = 0; atomic_read(&wd->seq) < seq; cnt++) {
		/* Bail if the other side set an error result */
		/* 对端已发布首错时立即退出，避免继续等待一个不会到来的正常令牌。 */
		if (READ_ONCE(wd->result) != WD_SUCCESS)
			return false;

		/* Prevent endless loops if the other CPU does not react. */
		/* 分批读取 raw-fast 时钟，既限制不响应 CPU 的等待，又避免每次 cpu_relax 都付出读钟成本。 */
		if (cnt == 5000) {
			u64 nsecs = ktime_get_raw_fast_ns();

			if (nsecs - start >=wd->timeout_ns) {
				watchdog_set_result(wd, WD_CPU_TIMEOUT);
				return false;
			}
			cnt = 0;
		}
		cpu_relax();
	}
	return seq < WATCHDOG_REMOTE_MAX_SEQ;
}

/*
 * watchdog_check_skew() - 作为乒乓协议的一侧交替读取同一 clocksource，检测跨 CPU 原始周期倒退。
 *
 * 【调用位置】控制 CPU 以 index=0 在关中断区调用，async CSD 在远端以 index=1 调用；两侧共享 @wd，
 * 通过 atomic seq 交替五次。@wd/@wd->cs 都是本轮借用对象，调用期间 CSD 活动保护其生命周期。
 *
 * 【参数与变量】@index 只能为 0/1，决定 local/remote 两个槽位；prev 是对端上一拍，now 是本 CPU
 * 当前读值，delta 按 cs mask 模减，start 是整轮 raw-fast 超时基点。函数不睡眠、不分配。
 *
 * 【阶段与返回】先写首个本地时间戳并递增 seq 宣告到达；每轮等待对端令牌，随后先读本地 counter、
 * 再写本地槽并读取远端槽，以缩短非本地 cache coherency 延迟对采样的污染。若模减 delta 超过
 * max_raw_delta，说明时间顺序疑似倒退，发布 WD_CPU_SKEWED；否则递增 seq 交权。无直接返回值，正常
 * 完成或任一侧错误都由共享 result/seq 表达。
 */
static void watchdog_check_skew(struct watchdog_cpu_data *wd, int index)
{
	u64 prev, now, delta, start = ktime_get_raw_fast_ns();
	int local = index, remote = (index + 1) & 0x1;
	struct clocksource *cs = wd->cs;

	/* Set the local timestamp so that the first iteration works correctly */
	/* 预置本侧槽，使另一侧第一次取得的 prev 已是有效读数。 */
	wd->cpu_ts[local] = cs->read(cs);

	/* Signal arrival */
	/* 原子递增既宣告本侧到达，也把下一次读取权交给等待该序号的对端。 */
	atomic_inc(&wd->seq);

	for (int seq = local + 2; seq < WATCHDOG_REMOTE_MAX_SEQ; seq += 2) {
		if (!watchdog_wait_seq(wd, start, seq))
			return;

		/* Capture local timestamp before possible non-local coherency overhead */
		/* 先读硬件 counter，再触碰共享远端 cache line，减少一致性等待扩大测量窗口。 */
		now = cs->read(cs);

		/* Store local timestamp before reading remote to limit coherency stalls */
		/* 先发布本地槽，再取对端上一槽；下一次 atomic_inc 才正式把控制权交回。 */
		wd->cpu_ts[local] = now;

		prev = wd->cpu_ts[remote];
		delta = (now - prev) & cs->mask;

		if (delta > cs->max_raw_delta) {
			watchdog_set_result(wd, WD_CPU_SKEWED);
			return;
		}

		/* Hand over to the remote CPU */
		/* 本轮检查通过，递增令牌允许对端执行下一拍。 */
		atomic_inc(&wd->seq);
	}
}

/*
 * watchdog_check_skew_remote() - 在目标 CPU 的 async call-single 上执行乒乓协议远端半部。
 * @unused 是 CSD 回调占位参数，不读取。this_cpu_ptr 取得本 CPU 静态邮箱；remote_inprogress 在整个
 * 回调外形成活动标记，使控制 CPU 不会过早复用同一 CSD。函数运行在远端 IPI/原子上下文，不睡眠；
 * 返回无直接值，结果留在邮箱，由控制 CPU 汇总。增减严格配对，即使 skew helper 提前返回也会清零。
 */
static void watchdog_check_skew_remote(void *unused)
{
	struct watchdog_cpu_data *wd = this_cpu_ptr(&watchdog_cpu_data);

	atomic_inc(&wd->remote_inprogress);
	watchdog_check_skew(wd, 1);
	atomic_dec(&wd->remote_inprogress);
}

/*
 * wd_csd_locked() - 判断目标邮箱的 async call-single 节点是否仍被队列/回调持有。
 * @wd 为借用指针；READ_ONCE 只稳定读取 u_flags，返回 CSD_FLAG_LOCK 位的布尔值。无锁、不可睡眠、
 * 无副作用；它与 remote_inprogress 共同防止复用，但不延长任何对象生命周期。
 */
static inline bool wd_csd_locked(struct watchdog_cpu_data *wd)
{
	return READ_ONCE(wd->csd.node.u_flags) & CSD_FLAG_LOCK;
}

/*
 * This is only invoked for remote CPUs. See watchdog_check_cpu_skew().
 */
/* 只为真正的远端 CPU 计算等待预算；本 CPU 情况已在 watchdog_check_cpu_skew() 提前跳过。 */
/*
 * wd_get_remote_timeout() - 按控制 CPU 与目标 CPU 的 NUMA 距离计算乒乓采样总超时。
 * @remote_cpu 是在线远端 CPU 编号，纯输入；函数在 watchdog 原子路径调用，无锁需求、不睡眠。
 * 单节点系统返回默认 50 微秒；多节点取当前/远端 node id，以“5 微秒 × distance”缩放并封顶
 * 500 微秒。返回纳秒值，无失败码；拓扑数据只读，函数不保存 CPU/node 引用。
 */
static inline u64 wd_get_remote_timeout(unsigned int remote_cpu)
{
	unsigned int n1, n2;
	u64 ns;

	if (nr_node_ids == 1)
		return WATCHDOG_DEFAULT_TIMEOUT_NS;

	n1 = cpu_to_node(smp_processor_id());
	n2 = cpu_to_node(remote_cpu);
	ns = WATCHDOG_NUMA_MULTIPLIER_NS * node_distance(n1, n2);
	return min(ns, WATCHDOG_NUMA_MAX_TIMEOUT_NS);
}

/*
 * __watchdog_check_cpu_skew() - 初始化目标 CPU 邮箱、并发启动两侧采样并提交汇总结果。
 *
 * 【调用位置】watchdog timer 持 watchdog_lock，从轮转 wrapper 传入在线且非本地 @cpu；@cs 是当前被测
 * clocksource 借用指针，注册和 watchdog 表保证其生命周期。函数在 timer/原子上下文运行，不睡眠。
 *
 * 【变量与前置检查】wd 指向目标 CPU 静态邮箱。若上次 remote_inprogress 未归零或 CSD lock 位仍在，
 * 本轮不能覆写邮箱，直接把全局结果设为 WD_CPU_TIMEOUT。通过后重置 seq/result，发布 cs 与 NUMA
 * timeout，并把当前控制 CPU 号写入 cs->wd_cpu 供 watchdog 测试模块观测。
 *
 * 【执行与返回】smp_call_function_single_async() 发布远端 CSD；失败记 timeout。成功后 scoped_guard(irq)
 * 只在本地乒乓阶段关闭中断，作用域退出自动恢复；最后在 watchdog_data.raw lock + irq guard 下复制
 * 首错结果和两侧快照，和可能并发的 watchdog_set_result() 配对。无直接返回值、无资源 ownership
 * 转移；watchdog_data.result 是唯一对外结果。
 */
static void __watchdog_check_cpu_skew(struct clocksource *cs, unsigned int cpu)
{
	struct watchdog_cpu_data *wd;

	wd = per_cpu_ptr(&watchdog_cpu_data, cpu);
	/* 阶段 1：必须确认前一 CSD 已完全离队并退出，否则复用字段会与远端写并发。 */
	if (atomic_read(&wd->remote_inprogress) || wd_csd_locked(wd)) {
		watchdog_data.result = WD_CPU_TIMEOUT;
		return;
	}

	atomic_set(&wd->seq, 0);
	wd->result = WD_SUCCESS;
	wd->cs = cs;
	/* Store the current CPU ID for the watchdog test unit */
	/* 保存控制 CPU 编号仅供 WDTEST 验证调度位置，不参与生产环境偏斜判定。 */
	cs->wd_cpu = smp_processor_id();

	wd->timeout_ns = wd_get_remote_timeout(cpu);

	/* Kick the remote CPU into the watchdog function */
	/* 阶段 2（远端发布）：异步入队成功后，两 CPU 通过 seq 接管读钟权；失败不启动本地半部。 */
	if (WARN_ON_ONCE(smp_call_function_single_async(cpu, &wd->csd))) {
		watchdog_data.result = WD_CPU_TIMEOUT;
		return;
	}

	/* 阶段 3：本侧 index=0；自动 IRQ guard 限制本 CPU 调度/中断噪声并在语句结束恢复。 */
	scoped_guard(irq)
		watchdog_check_skew(wd, 0);

	/* 阶段 4（提交）：与两侧首错写锁配对，把邮箱终态复制到当前扫描的全局诊断快照。 */
	scoped_guard(raw_spinlock_irq, &watchdog_data.lock) {
		watchdog_data.result = wd->result;
		memcpy(watchdog_data.cpu_ts, wd->cpu_ts, sizeof(wd->cpu_ts));
	}
}

/*
 * watchdog_check_cpu_skew() - 每轮选择下一个在线远端 CPU 并决定是否执行跨 CPU 一致性检查。
 * @cs 是 watchdog_list 当前元素的借用指针；调用者持 watchdog_lock 且处于 timer 原子上下文。
 * curr_cpu 作为轮转游标，即使候选是本 CPU 也会更新；cpumask_next_wrap() 在 online mask 中循环。
 * 单 CPU/选中本 CPU 时无检查；普通 WDTEST 源为避免干扰测试机制而跳过，只有 WDTEST_PERCPU 允许进入。
 * 无直接返回值；实际结果由 __watchdog_check_cpu_skew() 写入 watchdog_data.result。
 */
static void watchdog_check_cpu_skew(struct clocksource *cs)
{
	unsigned int cpu = watchdog_data.curr_cpu;

	cpu = cpumask_next_wrap(cpu, cpu_online_mask);
	watchdog_data.curr_cpu = cpu;

	/* Skip the current CPU. Handles num_online_cpus() == 1 as well */
	/* 目标绕回本 CPU 也覆盖仅一个 online CPU 的情形；本地比较无法证明跨 CPU 同步。 */
	if (cpu == smp_processor_id())
		return;

	/* Don't interfere with the test mechanics */
	/* 非 per-CPU 的人工 watchdog 测试自己控制故障注入，额外远端读会改变其预期序列。 */
	if ((cs->flags & CLOCK_SOURCE_WDTEST) && !(cs->flags & CLOCK_SOURCE_WDTEST_PERCPU))
		return;

	__watchdog_check_cpu_skew(cs, cpu);
}

/*
 * watchdog_check_freq() - 用连续参考 clocksource 包夹采样被测源，验证相邻扫描的频率增量。
 *
 * 【调用位置】半秒 watchdog timer 对每个 watchdog_list 候选调用；调用者持 watchdog_lock、关中断语义
 * 由局部 guard 精确覆盖三次硬件读取。@cs 是被测已注册对象借用指针；@reset_pending 表示异常暂停或
 * resume 后必须丢弃旧基线。函数不可睡眠，read 回调也必须适用于原子上下文。
 *
 * 【返回与结果】只有本轮频率样本可信且偏差在门限内才返回 true，调用者随后做跨 CPU skew 检查。
 * false 的原因写入 watchdog_data.result：无参考、采样窗口三次超时、重置基线或频率偏斜；false 不都
 * 代表硬件坏，RESET/TIMEOUT 仅要求稍后重试。函数更新 cs->wd_last/cs_last 和 WATCHDOG 基线标志。
 *
 * 【变量地图】ppm_shift 在未校准源上给约 4000ppm 宽限，双方 CALIBRATED 时收紧至约 500ppm；
 * wd_ts0/wd_ts1 包夹 cs_ts；wd_last/cs_last 是上轮快照；wd_seq 是本轮参考读窗耗时；wd_delta/cs_delta
 * 是相邻扫描的纳秒增量；max_delta 用于转换范围和 ppm 容差。retries 最多三次过滤偶发长采样窗。
 */
static bool watchdog_check_freq(struct clocksource *cs, bool reset_pending)
{
	unsigned int ppm_shift = SHIFT_4000PPM;
	u64 wd_ts0, wd_ts1, cs_ts;

	watchdog_data.result = WD_SUCCESS;
	/* 阶段 1：没有连续参考源时无法比较频率；per-CPU 测试源则直接转去跨 CPU 专项检查。 */
	if (!watchdog) {
		watchdog_data.result = WD_FREQ_NO_WATCHDOG;
		return false;
	}

	if (cs->flags & CLOCK_SOURCE_WDTEST_PERCPU)
		return true;

	/*
	 * If both the clocksource and the watchdog claim they are
	 * calibrated use 500ppm limit. Uncalibrated clocksources need a
	 * larger allowance because thefirmware supplied frequencies can be
	 * way off.
	 */
	/*
	 * 被测源和参考源都声明已校准时使用约 500ppm；任一未校准就放宽到约 4000ppm，因为固件给出的
	 * 初始频率可能明显不准。原文中的 thefirmware 是既有拼写，语义仍按 firmware 频率解释。
	 */
	if (watchdog->flags & CLOCK_SOURCE_CALIBRATED && cs->flags & CLOCK_SOURCE_CALIBRATED)
		ppm_shift = SHIFT_500PPM;

	for (int retries = 0; retries < WATCHDOG_FREQ_RETRIES; retries++) {
		s64 wd_last, cs_last, wd_seq, wd_delta, cs_delta, max_delta;

		/* 阶段 2：关本地中断执行“参考前—被测—参考后”，以参考包夹宽度量化采样扰动。 */
		scoped_guard(irq) {
			wd_ts0 = watchdog->read(watchdog);
			cs_ts = cs->read(cs);
			wd_ts1 = watchdog->read(watchdog);
		}

		wd_last = cs->wd_last;
		cs_last = cs->cs_last;

		/* Validate the watchdog readout window */
		/* 包夹窗口超过 50 微秒时保存诊断值并重试，不更新两个源的历史基线。 */
		wd_seq = cycles_to_nsec_safe(watchdog, wd_ts0, wd_ts1);
		if (wd_seq > WATCHDOG_READOUT_MAX_NS) {
			/* Store for printout in case all retries fail */
			watchdog_data.wd_seq = wd_seq;
			continue;
		}

		/* Store for subsequent processing */
		/* 阶段 3：只有采样窗有效才提交本轮原始快照，供下次半秒扫描计算增量。 */
		cs->wd_last = wd_ts0;
		cs->cs_last = cs_ts;

		/* First round or reset pending? */
		/* 首轮或外部 reset 请求没有可比历史值，转 reset 出口只建立新基线。 */
		if (!(cs->flags & CLOCK_SOURCE_WATCHDOG) || reset_pending)
			goto reset;

		/* Calculate the nanosecond deltas from the last invocation */
		/* 阶段 4：分别把参考源和被测源从上轮到本轮的模周期差安全换算为纳秒。 */
		wd_delta = cycles_to_nsec_safe(watchdog, wd_last, wd_ts0);
		cs_delta = cycles_to_nsec_safe(cs, cs_last, cs_ts);

		watchdog_data.wd_delta = wd_delta;
		watchdog_data.cs_delta = cs_delta;

		/*
		 * Ensure that the deltas are within the readout limits of
		 * the clocksource and the watchdog. Long delays can cause
		 * clocksources to overflow.
		 */
		/*
		 * 任一增量超过任一计数器的 max_idle_ns，都可能已经多次回绕或使定点算术失真；该样本不能用于
		 * 判坏，清基线后下轮重来。
		 */
		max_delta = max(wd_delta, cs_delta);
		if (max_delta > cs->max_idle_ns || max_delta > watchdog->max_idle_ns)
			goto reset;

		/*
		 * Calculate and validate the skew against the allowed PPM
		 * value of the maximum delta plus the watchdog readout
		 * time.
		 */
		/*
		 * 阶段 5（判定）：允许误差由较大增量的 ppm 份额加本轮包夹窗口组成；后者覆盖两次参考读取之间
		 * 被测读取不可避免的时间。严格小于门限才成功，否则记录频率偏斜供结果处理降级。
		 */
		if (abs(wd_delta - cs_delta) < (max_delta >> ppm_shift) + wd_seq)
			return true;

		watchdog_data.result = WD_FREQ_SKEWED;
		return false;
	}

	watchdog_data.result = WD_FREQ_TIMEOUT;
	return false;

reset:
	/* RESET 出口发布“已有有效基线”位；本轮不做 skew 结论，下一轮才能比较相邻快照。 */
	cs->flags |= CLOCK_SOURCE_WATCHDOG;
	watchdog_data.result = WD_FREQ_RESET;
	return false;
}

/* Synchronization for sched clock */
/* watchdog 确认稳定时给 sched_clock 驱动一个同步点，帮助其校正与当前 timekeeping clock 的关系。 */
/*
 * clocksource_tick_stable() - 仅对当前 clocksource 调用可选 tick_stable 回调。
 * @cs 是 watchdog 当前候选借用指针；调用者持 watchdog_lock、处于 timer 原子上下文。若 cs 不是当前源
 * 或驱动未提供回调则无操作；否则回调必须不可睡眠。无返回值，副作用完全由驱动同步 sched_clock。
 */
static void clocksource_tick_stable(struct clocksource *cs)
{
	if (cs == curr_clocksource && cs->tick_stable)
		cs->tick_stable(cs);
}

/* Conditionaly enable high resolution mode */
/* 当被测连续 clocksource 已由连续 watchdog 验证后，才有条件开放 high-resolution/oneshot 使用资格。 */
/*
 * clocksource_enable_highres() - 把一次成功 watchdog 验证转化为 HRES 可用标志和后续通知。
 *
 * 【参数与上下文】@cs 是已注册被测源借用指针；调用者持 watchdog_lock、不可睡眠。函数要求 cs 与
 * 当前 watchdog 都声明 CONTINUOUS，且 cs 尚未 VALID_FOR_HRES；否则快速返回。
 *
 * 【状态与返回】先发布 VALID_FOR_HRES。启动尚未完成时由 done_booting 后续统一选择；WDTEST 只改变
 * 测试对象标志、不影响生产选择。正常运行中，非当前源设置 RESELECT 并调度 kwatchdog，当前源则调用
 * tick_clock_notify() 让 tick 层看到能力升级。返回无直接值；标志提交不会因后续通知失败回滚。
 */
static void clocksource_enable_highres(struct clocksource *cs)
{
	if ((cs->flags & CLOCK_SOURCE_VALID_FOR_HRES) ||
	    !(cs->flags & CLOCK_SOURCE_IS_CONTINUOUS) ||
	    !watchdog || !(watchdog->flags & CLOCK_SOURCE_IS_CONTINUOUS))
		return;

	/* Mark it valid for high-res. */
	/* 参考与被测都连续且频率通过，本行是 HRES 能力对其他选择者可见的提交点。 */
	cs->flags |= CLOCK_SOURCE_VALID_FOR_HRES;

	/*
	 * Can't schedule work before finished_booting is
	 * true. clocksource_done_booting will take care of it.
	 */
	/* 启动早期不能调度重选 work；clocksource_done_booting() 会先清理 unstable 再统一 select。 */
	if (!finished_booting)
		return;

	if (cs->flags & CLOCK_SOURCE_WDTEST)
		return;

	/*
	 * If this is not the current clocksource let the watchdog thread
	 * reselect it. Due to the change to high res this clocksource
	 * might be preferred now. If it is the current clocksource let the
	 * tick code know about that change.
	 */
	/* 非当前源可能因新资格成为最佳源，交给线程重选；当前源无需切换，只通知 tick 重新评估模式。 */
	if (cs != curr_clocksource) {
		cs->flags |= CLOCK_SOURCE_RESELECT;
		schedule_work(&watchdog_work);
	} else {
		tick_clock_notify();
	}
}

static DEFINE_RATELIMIT_STATE(ratelimit_state, 5 * HZ, 2);
/* 频率采样超时日志每 5 秒最多放行 2 条，避免受干扰平台每半秒、每候选重复刷屏；状态静态存续。 */

/*
 * watchdog_print_freq_timeout() - 受速率限制打印参考 clocksource 包夹采样超时。
 * @cs 是结果处理统一签名传入的被测源借用指针，当前消息不使用它；调用者持 watchdog_lock、不可睡眠。
 * 令牌不足时无输出，否则读取稳定的 watchdog 名称和 wd_seq 打印 info。无返回值、不改变判定结果。
 */
static void watchdog_print_freq_timeout(struct clocksource *cs)
{
	if (!__ratelimit(&ratelimit_state))
		return;
	pr_info("Watchdog %s read timed out. Readout sequence took: %lluns\n",
		watchdog->name, watchdog_data.wd_seq);
}

/*
 * watchdog_print_freq_skew() - 打印导致被测 clocksource 将被降级的频率偏差证据。
 * @cs 为已注册借用指针；调用者持 watchdog_lock。函数输出被测名、参考名和两者纳秒区间，不限速、
 * 不睡眠且无返回值；它只报告，真正设置 UNSTABLE 由 watchdog_check_result() 随后完成。
 */
static void watchdog_print_freq_skew(struct clocksource *cs)
{
	pr_warn("Marking clocksource %s unstable due to frequency skew\n", cs->name);
	pr_warn("Watchdog    %20s interval: %16lluns\n", watchdog->name, watchdog_data.wd_delta);
	pr_warn("Clocksource %20s interval: %16lluns\n", cs->name, watchdog_data.cs_delta);
}

/*
 * watchdog_handle_remote_timeout() - 对跨 CPU 乒乓超时给出一次性诊断。
 * @cs 当前未使用但保持结果处理回调形状；调用者持 watchdog_lock。pr_info_once 使整个启动实例只打印
 * 首次目标 CPU 超时，后续周期仍会重试但不刷屏。无返回值，不把暂时不响应直接判成 clocksource 坏。
 */
static void watchdog_handle_remote_timeout(struct clocksource *cs)
{
	pr_info_once("Watchdog remote CPU %u read timed out\n", watchdog_data.curr_cpu);
}

/*
 * watchdog_print_remote_skew() - 打印跨 CPU 原始 counter 顺序反转的两侧证据。
 * @cs 是将被标记 unstable 的借用对象；cpu_ts[0] 属于当前控制 CPU，cpu_ts[1] 属于 curr_cpu。函数比较
 * 两值后总以“较小值 < 较大值”格式标明 CPU，便于定位不同步硬件；无锁变化和返回，调用者持
 * watchdog_lock 保持快照稳定，随后负责真正降级。
 */
static void watchdog_print_remote_skew(struct clocksource *cs)
{
	pr_warn("Marking clocksource %s unstable due to inter CPU skew\n", cs->name);
	if (watchdog_data.cpu_ts[0] < watchdog_data.cpu_ts[1]) {
		pr_warn("CPU%u %16llu < CPU%u %16llu (cycles)\n", smp_processor_id(),
			watchdog_data.cpu_ts[0], watchdog_data.curr_cpu, watchdog_data.cpu_ts[1]);
	} else {
		pr_warn("CPU%u %16llu < CPU%u %16llu (cycles)\n", watchdog_data.curr_cpu,
			watchdog_data.cpu_ts[1], smp_processor_id(), watchdog_data.cpu_ts[0]);
	}
}

/*
 * watchdog_check_result() - 把 freq/CPU 检查终态映射为能力升级、重试或不稳定降级。
 *
 * 【参数与上下文】@cs 是本轮 watchdog_list 候选借用指针；调用者持 watchdog_lock、处于 timer 原子
 * 上下文，不可睡眠。watchdog_data 保存紧邻本次检查的结果和诊断快照。
 *
 * 【分支】SUCCESS 给 sched_clock 同步点并尝试开放 HRES；FREQ_TIMEOUT 清 WATCHDOG 基线位使下轮重建；
 * NO_WATCHDOG/RESET 已完成所需状态，不再动作；CPU_TIMEOUT 只记录并下轮重试。两种 SKEWED 先打印
 * 证据后汇合到 __clocksource_unstable()，清 HRES/验证位并安排线程降级。
 *
 * 【返回】无直接值。暂态失败不会改变 rating；SKEWED 返回时 flags 已提交但 timekeeper 重选仍异步。
 */
static void watchdog_check_result(struct clocksource *cs)
{
	switch (watchdog_data.result) {
	case WD_SUCCESS:
		/* 稳定样本既可同步 sched_clock，也可能首次满足 HRES 资格。 */
		clocksource_tick_stable(cs);
		clocksource_enable_highres(cs);
		return;

	case WD_FREQ_TIMEOUT:
		watchdog_print_freq_timeout(cs);
		/* Try again later and invalidate the reference timestamps. */
		/* 采样窗受干扰不是硬件坏；清基线标志使下一轮只重新配对时间戳。 */
		cs->flags &= ~CLOCK_SOURCE_WATCHDOG;
		return;

	case WD_FREQ_NO_WATCHDOG:
	case WD_FREQ_RESET:
		/*
		 * Nothing to do when the reference timestamps were reset
		 * or no watchdog clocksource registered.
		 */
		/* reset 分支已写新快照，无参考源则没有可执行动作；都不能据此降级。 */
		return;

	case WD_FREQ_SKEWED:
		watchdog_print_freq_skew(cs);
		break;

	case WD_CPU_TIMEOUT:
		/* Remote check timed out. Try again next cycle. */
		/* CPU 暂时未响应可能是平台噪声；轮转游标下一周期继续，不污染频率稳定结论。 */
		watchdog_handle_remote_timeout(cs);
		return;

	case WD_CPU_SKEWED:
		watchdog_print_remote_skew(cs);
		break;
	}
	/* 只有频率或跨 CPU skew 两个确定性分支到达这里，统一提交 unstable 状态。 */
	__clocksource_unstable(cs);
}

/*
 * clocksource_watchdog() - 每半秒扫描全部待验证 clocksource 并重新安排下一轮 timer。
 *
 * 【调用位置】固定在 boot CPU 的 pinned timer 调用；@unused 是静态 watchdog_timer 借用指针，当前不
 * 使用。函数以 guard(spinlock) 自动持有/释放 watchdog_lock，运行在 softirq 原子上下文，所有 read、
 * tick_stable 和标记回调都不得睡眠。
 *
 * 【变量】@cs 是遍历中的列表借用对象；reset_pending 把 atomic 计数当前是否非零快照成 bool，本轮
 * 全表共用。计数可能大于一，每轮只 dec 一次，因此并发多个 reset 请求不会被一次扫描全部吞掉。
 *
 * 【阶段】若 stop 已把 running 清零则退出；对已 UNSTABLE 对象只确保 work 被调度；其余先做频率检查，
 * 仅成功时再做跨 CPU skew，最后按结果升级、重试或降级。全表完成后消费一个 reset 请求；若 timer
 * 未被并发 stop/start 重新挂起，以旧 expires 加 interval 的方式重排，减少回调耗时造成的周期漂移。
 *
 * 【返回】无直接返回值。timer/work 状态是可观察副作用；对象不在 softirq 中摘除或释放。
 */
static void clocksource_watchdog(struct timer_list *unused)
{
	struct clocksource *cs;
	bool reset_pending;

	/* 阶段 1：自动 guard 覆盖整次列表扫描和 timer 状态提交，所有提前 return 都会解锁。 */
	guard(spinlock)(&watchdog_lock);
	if (!watchdog_running)
		return;

	reset_pending = atomic_read(&watchdog_reset_pending);

	list_for_each_entry(cs, &watchdog_list, wd_list) {
		/* Clocksource already marked unstable? */
		/* 已提交 unstable 的对象不再读硬件；启动完成后只催促进程上下文完成降级/重选。 */
		if (cs->flags & CLOCK_SOURCE_UNSTABLE) {
			if (finished_booting)
				schedule_work(&watchdog_work);
			continue;
		}

		/* Compare against watchdog clocksource if available */
		/* 频率样本可信才允许进入更昂贵的双 CPU 原始周期交接。 */
		if (watchdog_check_freq(cs, reset_pending)) {
			/* Check for inter CPU skew */
			/* 频率一致仍不代表 per-CPU counter 同步，轮转选择一个远端 CPU 验证。 */
			watchdog_check_cpu_skew(cs);
		}

		watchdog_check_result(cs);
	}

	/* Clear after the full clocksource walk */
	/* reset 必须对同一轮所有候选生效，故只能在完整遍历后消费一次原子请求。 */
	if (reset_pending)
		atomic_dec(&watchdog_reset_pending);

	/* Could have been rearmed by a stop/start cycle */
	/* stop/start 可能已经安装新 timer；仅在当前未 pending 时按原截止点续期，避免重复挂载。 */
	if (!timer_pending(&watchdog_timer)) {
		watchdog_timer.expires += WATCHDOG_INTERVAL;
		add_timer_local(&watchdog_timer);
	}
}

/*
 * clocksource_start_watchdog() - 在第一项待验证源出现时启动 pinned 周期 timer。
 * 入参：无。调用者必须持 watchdog_lock、不可睡眠。已 running 或列表为空时幂等返回；否则初始化 timer
 * 回调/固定 CPU 属性，把首次截止设为当前 jiffies+半秒，挂到 boot CPU 后发布 running=1。无错误返回；
 * timer 持有的是静态对象，停止前 watchdog_list 中对象由注册生命周期保持有效。
 */
static inline void clocksource_start_watchdog(void)
{
	if (watchdog_running || list_empty(&watchdog_list))
		return;
	timer_setup(&watchdog_timer, clocksource_watchdog, TIMER_PINNED);
	watchdog_timer.expires = jiffies + WATCHDOG_INTERVAL;

	add_timer_on(&watchdog_timer, get_boot_cpu_id());
	watchdog_running = 1;
}

/*
 * clocksource_stop_watchdog() - 在最后一项待验证源摘除后停止周期 timer。
 * 入参：无。调用者持 watchdog_lock；未运行或列表仍非空都不得停止。满足条件时 timer_delete() 取消
 * pending 实例并清 running。无返回值；当前回调若已执行，会受同一 watchdog_lock 串行，函数不释放对象。
 */
static inline void clocksource_stop_watchdog(void)
{
	if (!watchdog_running || !list_empty(&watchdog_list))
		return;
	timer_delete(&watchdog_timer);
	watchdog_running = 0;
}

/*
 * clocksource_resume_watchdog() - 请求后续扫描丢弃 suspend/kgdb 前的参考时间戳。
 * 入参：无；可从 resume 或异常恢复路径调用，不取 watchdog_lock、不睡眠。atomic_inc 允许多个并发请求
 * 累积；timer 每轮消费一个并为所有候选走 reset。无返回值，若 watchdog 未构建则配置桩无操作。
 */
static void clocksource_resume_watchdog(void)
{
	atomic_inc(&watchdog_reset_pending);
}

/*
 * clocksource_enqueue_watchdog() - 初始化新注册对象的 watchdog 角色与验证状态。
 * @cs 是尚在注册提交过程中的借用指针；调用者同时持 clocksource_mutex 和 watchdog_lock，不睡眠。
 * 先初始化 wd_list。MUST_VERIFY 对象加入被测表并清已有基线位；其余对象是参考候选，若其 counter 连续
 * 可直接标 VALID_FOR_HRES。无返回值；本函数尚不选择全局 watchdog，也不启动 timer，后续 select 完成。
 */
static void clocksource_enqueue_watchdog(struct clocksource *cs)
{
	INIT_LIST_HEAD(&cs->wd_list);

	if (cs->flags & CLOCK_SOURCE_MUST_VERIFY) {
		/* cs is a clocksource to be watched. */
		/* 必须验证的源加入独立 wd_list；新对象没有与当前参考源配对的历史读数。 */
		list_add(&cs->wd_list, &watchdog_list);
		cs->flags &= ~CLOCK_SOURCE_WATCHDOG;
	} else {
		/* cs is a watchdog. */
		/* 无需被验证的源可竞争参考角色；连续性同时足以让它具备 HRES 使用资格。 */
		if (cs->flags & CLOCK_SOURCE_IS_CONTINUOUS)
			cs->flags |= CLOCK_SOURCE_VALID_FOR_HRES;
	}
}

/*
 * clocksource_select_watchdog() - 从注册表选择 rating 最高的连续、免验证参考源。
 *
 * 【参数与锁】@fallback 为 false 表示普通重评，为 true 表示必须尝试排除当前 watchdog（注销路径）。
 * 调用者持 clocksource_mutex，函数内部 irqsave watchdog_lock，形成固定 mutex→spin 顺序；@cs/@old_wd
 * 都是列表借用指针。锁内不可睡眠。
 *
 * 【阶段与返回】保存旧引用；fallback 先清 watchdog，再按 rating 顺序扫描，跳过 MUST_VERIFY、非连续和
 * 被要求替换的旧源。找不到替代时恢复 old_wd；选择变化则清全部被测源基线，最后按列表状态启动 timer。
 * 无直接返回值。fallback 后仍为旧源表示替换失败，clocksource_unbind() 会据此返回 -EBUSY。
 */
static void clocksource_select_watchdog(bool fallback)
{
	struct clocksource *cs, *old_wd;
	unsigned long flags;

	spin_lock_irqsave(&watchdog_lock, flags);
	/* save current watchdog */
	/* 阶段 1：保留旧源用于 fallback 排除、失败恢复和变化检测。 */
	old_wd = watchdog;
	if (fallback)
		watchdog = NULL;

	list_for_each_entry(cs, &clocksource_list, list) {
		/* cs is a clocksource to be watched. */
		/* MUST_VERIFY 对象不能证明自己或别人可靠，永不作为参考源。 */
		if (cs->flags & CLOCK_SOURCE_MUST_VERIFY)
			continue;

		/*
		 * If it's not continuous, don't put the fox in charge of
		 * the henhouse.
		 */
		/* 非连续 counter 在 idle/suspend 可停，用它监督别的源会把参考自身停顿误判为被测故障。 */
		if (!(cs->flags & CLOCK_SOURCE_IS_CONTINUOUS))
			continue;

		/* Skip current if we were requested for a fallback. */
		/* 注销旧 watchdog 时必须选不同对象；普通重评允许旧源继续胜出。 */
		if (fallback && cs == old_wd)
			continue;

		/* Pick the best watchdog. */
		/* 主表按 rating 降序，但仍显式比较，保持过滤后的最高候选。 */
		if (!watchdog || cs->rating > watchdog->rating)
			watchdog = cs;
	}
	/* If we failed to find a fallback restore the old one. */
	/* 没有替代者不在这里摘除旧源；恢复它让上层识别 -EBUSY 并保住监督能力。 */
	if (!watchdog)
		watchdog = old_wd;

	/* If we changed the watchdog we need to reset cycles. */
	/* 不同硬件参考源的 wd_last 不可比较，变化时全表清基线。 */
	if (watchdog != old_wd)
		clocksource_reset_watchdog();

	/* Check if the watchdog timer needs to be started. */
	/* 参考源选择完成后才启动；timer 是否需要运行还由待验证表是否为空决定。 */
	clocksource_start_watchdog();
	spin_unlock_irqrestore(&watchdog_lock, flags);
}

/*
 * clocksource_dequeue_watchdog() - 从 watchdog 角色中摘除一个即将注销的 clocksource。
 * @cs 为已注册对象借用指针；调用者持 clocksource_mutex 和 watchdog_lock。若它仍是全局 watchdog，
 * 上层替换协议尚未完成，本函数不处理；MUST_VERIFY 对象则 list_del_init()，最后一项离开时停止 timer。
 * 无直接返回值、不释放 cs；主注册 list 由 clocksource_unbind() 随后另行摘除。
 */
static void clocksource_dequeue_watchdog(struct clocksource *cs)
{
	if (cs != watchdog) {
		if (cs->flags & CLOCK_SOURCE_MUST_VERIFY) {
			/* cs is a watched clocksource. */
			/* 初始化为空节点便于注销幂等检查，也表明 timer 不再借用该对象。 */
			list_del_init(&cs->wd_list);
			/* Check if the watchdog timer needs to be stopped. */
			/* 只有摘除最后一个被测源时 stop helper 才真正取消 timer。 */
			clocksource_stop_watchdog();
		}
	}
}

/*
 * __clocksource_watchdog_kthread() - 在 mutex+watchdog_lock 下兑现异步降级/重选标志。
 *
 * 【调用位置】临时 kwatchdog wrapper 和启动完成路径调用；入口必须已持 clocksource_mutex，本函数再取
 * irqsave watchdog_lock。@cs/@tmp 用 safe 遍历，因为 unstable 对象会从 watchdog_list 摘除。
 *
 * 【阶段与返回】UNSTABLE 对象先离开被测表、rating 降为 0 并在主表重排；RESELECT 只清一次性标志。
 * 任一变化把 select 置 1。遍历后若表空停止 timer，解锁并返回 1 要求调用者执行 clocksource_select()；
 * 无变化返回 0。对象只摘链不释放，真正 timekeeping 切换在 spinlock 外完成。
 */
static int __clocksource_watchdog_kthread(void)
{
	struct clocksource *cs, *tmp;
	unsigned long flags;
	int select = 0;

	spin_lock_irqsave(&watchdog_lock, flags);
	list_for_each_entry_safe(cs, tmp, &watchdog_list, wd_list) {
		/* 阶段 1：确定不稳定的源不再接受 watchdog 扫描，并降到最低选择优先级。 */
		if (cs->flags & CLOCK_SOURCE_UNSTABLE) {
			list_del_init(&cs->wd_list);
			clocksource_change_rating(cs, 0);
			select = 1;
		}
		if (cs->flags & CLOCK_SOURCE_RESELECT) {
			/* HRES 能力变化只要求重新比较候选，不改变 rating 或 watchdog 成员关系。 */
			cs->flags &= ~CLOCK_SOURCE_RESELECT;
			select = 1;
		}
	}
	/* Check if the watchdog timer needs to be stopped. */
	/* 阶段 2：批量摘除完成后统一判断是否取消 timer，再把可能睡眠的 select 留给调用者。 */
	clocksource_stop_watchdog();
	spin_unlock_irqrestore(&watchdog_lock, flags);

	return select;
}

/*
 * clocksource_watchdog_kthread() - 在进程上下文串行执行 watchdog 后半部并按需切换当前源。
 * @data 是 kthread_run 传入的 NULL 占位，不读取、不释放。函数可睡眠，入口无锁；取得
 * clocksource_mutex 后处理标志，返回 1 时调用 select（可能经 timekeeping_notify/stop_machine），再
 * 解锁。固定返回 0 结束一次性线程；没有长期线程对象或调用者清理责任。
 */
static int clocksource_watchdog_kthread(void *data)
{
	mutex_lock(&clocksource_mutex);
	if (__clocksource_watchdog_kthread())
		clocksource_select();
	mutex_unlock(&clocksource_mutex);
	return 0;
}

/*
 * clocksource_is_watchdog() - 判断给定对象是否是当前参考源。
 * @cs 为借用指针，可为任意注册对象；调用者通过 clocksource_mutex 或 watchdog_lock 稳定全局指针。
 * 返回相等布尔值，无副作用、不睡眠、不取得引用。
 */
static bool clocksource_is_watchdog(struct clocksource *cs)
{
	return cs == watchdog;
}

#else /* CONFIG_CLOCKSOURCE_WATCHDOG */

/* 关闭 WATCHDOG 时没有监督表；连续 counter 按设计直接取得 HRES 资格，@cs 为注册期借用对象。 */
static void clocksource_enqueue_watchdog(struct clocksource *cs)
{
	if (cs->flags & CLOCK_SOURCE_IS_CONTINUOUS)
		cs->flags |= CLOCK_SOURCE_VALID_FOR_HRES;
}

/* 无 watchdog 可选；@fallback 被忽略，返回无值、无副作用。 */
static void clocksource_select_watchdog(bool fallback) { }
/* 无监督链可摘；@cs 被忽略，返回无值、无副作用。 */
static inline void clocksource_dequeue_watchdog(struct clocksource *cs) { }
/* 无历史 watchdog 基线可重置；入参：无，返回无值。 */
static inline void clocksource_resume_watchdog(void) { }
/* 无异步 flags 需要兑现；入参：无，固定返回 0 表示不需重选。 */
static inline int __clocksource_watchdog_kthread(void) { return 0; }
/* 配置关闭时任何 @cs 都不可能是 watchdog；借用指针不读取，固定 false。 */
static bool clocksource_is_watchdog(struct clocksource *cs) { return false; }
/* 外部不稳定通知在无 watchdog 构建中为空操作；@cs 借用，不改 rating/flags，无返回值。 */
void clocksource_mark_unstable(struct clocksource *cs) { }

/* 无 watchdog_lock 实体；@flags 不写入，调用点仍可共享同一源码结构。 */
static inline void clocksource_watchdog_lock(unsigned long *flags) { }
/* 与空 lock 配对；@flags 不读取，不改变 IRQ 状态。 */
static inline void clocksource_watchdog_unlock(unsigned long *flags) { }

#endif /* CONFIG_CLOCKSOURCE_WATCHDOG */

/*
 * clocksource_is_suspend() - 判断 @cs 是否为当前跨 suspend 计时源。
 * @cs 是 timekeeping 或注册表持有的借用指针；调用者位于 clocksource_mutex 保护的选择/注销路径，或
 * 已冻结并仅单 CPU 运行的 suspend/resume 阶段。返回指针相等布尔值，无副作用、不取得引用。
 */
static bool clocksource_is_suspend(struct clocksource *cs)
{
	return cs == suspend_clocksource;
}

/*
 * __clocksource_suspend_select() - 用单个候选增量更新最佳 non-stop suspend clocksource。
 * @cs 是已注册/注册中的对象借用指针；调用者持 clocksource_mutex。没有 SUSPEND_NONSTOP 就返回；具备
 * 该标志却同时提供 suspend/resume 回调会告警，因为回调若停掉它就无法测睡眠。随后按 rating 只在更优
 * 时替换全局借用指针。无返回值、不 enable 硬件、不改变对象 ownership。
 */
static void __clocksource_suspend_select(struct clocksource *cs)
{
	/*
	 * Skip the clocksource which will be stopped in suspend state.
	 */
	/* suspend 中会停止的 counter 无法提供入睡到恢复的连续 delta，不能成为候选。 */
	if (!(cs->flags & CLOCK_SOURCE_SUSPEND_NONSTOP))
		return;

	/*
	 * The nonstop clocksource can be selected as the suspend clocksource to
	 * calculate the suspend time, so it should not supply suspend/resume
	 * interfaces to suspend the nonstop clocksource when system suspends.
	 */
	/* NONSTOP 契约要求系统 suspend 时不由这些回调停/启该 counter；冲突只告警，仍按 flag 参与选择。 */
	if (cs->suspend || cs->resume) {
		pr_warn("Nonstop clocksource %s should not supply suspend/resume interfaces\n",
			cs->name);
	}

	/* Pick the best rating. */
	/* rating 更高才替换；相同 rating 保留先前候选，指针始终借用注册对象。 */
	if (!suspend_clocksource || cs->rating > suspend_clocksource->rating)
		suspend_clocksource = cs;
}

/**
 * clocksource_suspend_select - Select the best clocksource for suspend timing
 * @fallback:	if select a fallback clocksource
 */
/*
 * 从已注册对象中选择 rating 最高的 SUSPEND_NONSTOP 计数器。@fallback=false 用于普通扫描，true 用于
 * 注销当前 suspend 源时排除旧对象。调用者必须持 clocksource_mutex，函数可打印告警但不调用硬件。
 * old_suspend 是借用快照，@cs 是列表游标；fallback 找不到替代时允许 suspend_clocksource 保持 NULL，
 * 上层仍可注销旧源，后续睡眠时间会回退 persistent clock/RTC。无直接返回值或 ownership 转移。
 */
static void clocksource_suspend_select(bool fallback)
{
	struct clocksource *cs, *old_suspend;

	old_suspend = suspend_clocksource;
	/* fallback 先清全局选择，避免扫描失败时继续留下即将释放的旧指针。 */
	if (fallback)
		suspend_clocksource = NULL;

	list_for_each_entry(cs, &clocksource_list, list) {
		/* Skip current if we were requested for a fallback. */
		/* 注销场景不能把同一对象再次选回；普通扫描则允许它继续保持最佳。 */
		if (fallback && cs == old_suspend)
			continue;

		__clocksource_suspend_select(cs);
	}
}

/**
 * clocksource_start_suspend_timing - Start measuring the suspend timing
 * @cs:			current clocksource from timekeeping
 * @start_cycles:	current cycles from timekeeping
 *
 * This function will save the start cycle values of suspend timer to calculate
 * the suspend time when resuming system.
 *
 * This function is called late in the suspend process from timekeeping_suspend(),
 * that means processes are frozen, non-boot cpus and interrupts are disabled
 * now. It is therefore possible to start the suspend timer without taking the
 * clocksource mutex.
 */
/*
 * 保存系统进入 suspend 前的 counter 起点，供恢复时计算睡眠纳秒数。该接口由 timekeeping_suspend()
 * 在进程冻结、非 boot CPU 与中断关闭且持 timekeeper raw lock 的晚期阶段调用，因此无需
 * clocksource_mutex，驱动 enable/read 回调也不得睡眠。
 *
 * clocksource_start_suspend_timing() - 建立一次 suspend non-stop 计时基线。
 *
 * 【参数】@cs 是当前 timekeeping clocksource 借用指针；@start_cycles 是 timekeeping 已在 forward_now
 * 中读取并提交的该源 cycle_last。两者生命周期由冻结的 timekeeping/注册协议稳定，不取得新引用。
 *
 * 【分支与返回】没有 suspend 源时无操作；若它正是当前源，复用 start_cycles，避免对同一硬件重复读。
 * 若是独立源，先调用可选 enable；非零失败只告警一次并返回，不更新 suspend_start。成功后 read 并保存
 * 全局原始周期。无直接返回值和错误传播；enable 失败后恢复路径通常因无有效正 delta 回退其他来源。
 */
void clocksource_start_suspend_timing(struct clocksource *cs, u64 start_cycles)
{
	if (!suspend_clocksource)
		return;

	/*
	 * If current clocksource is the suspend timer, we should use the
	 * tkr_mono.cycle_last value as suspend_start to avoid same reading
	 * from suspend timer.
	 */
	/* 同一对象直接复用 timekeeper 的一致快照，使运行时间结算点与 suspend 起点完全相同。 */
	if (clocksource_is_suspend(cs)) {
		suspend_start = start_cycles;
		return;
	}

	/* 独立源可能平时为省电关闭；enable 失败时不能读取并建立新基线。 */
	if (suspend_clocksource->enable &&
	    suspend_clocksource->enable(suspend_clocksource)) {
		pr_warn_once("Failed to enable the non-suspend-able clocksource.\n");
		return;
	}

	/* 本行提交独立 non-stop counter 的起点，恢复阶段按同一对象读取终点。 */
	suspend_start = suspend_clocksource->read(suspend_clocksource);
}

/**
 * clocksource_stop_suspend_timing - Stop measuring the suspend timing
 * @cs:		current clocksource from timekeeping
 * @cycle_now:	current cycles from timekeeping
 *
 * This function will calculate the suspend time from suspend timer.
 *
 * Returns nanoseconds since suspend started, 0 if no usable suspend clocksource.
 *
 * This function is called early in the resume process from timekeeping_resume(),
 * that means there is only one cpu, no processes are running and the interrupts
 * are disabled. It is therefore possible to stop the suspend timer without
 * taking the clocksource mutex.
 */
/*
 * 在 timekeeping_resume() 的早期单 CPU、进程未运行且中断关闭阶段结束 suspend 计时；调用者持
 * timekeeper raw lock，故无需 clocksource_mutex，所有 read/disable 回调也不得睡眠。
 *
 * clocksource_stop_suspend_timing() - 读取 non-stop 终点并返回可注入 timekeeping 的睡眠纳秒数。
 *
 * 【参数】@cs 是当前 timekeeping 源借用指针；@cycle_now 是恢复路径刚从该源读取的原始周期。若选择的
 * suspend 源相同就复用该快照，否则调用其 read。参数和全局对象均不转移 ownership。
 *
 * 【返回与副作用】无 suspend 源返回 0；仅当 now>suspend_start 时用安全换算返回正纳秒，否则返回 0，
 * 由 timekeeping 回退 persistent clock/RTC。独立 suspend 源若有 disable 回调，无论 delta 是否有效都在
 * 返回前关闭以省电；当前 timekeeping 源不能关闭。函数无 errno，0 同时表示不可用、未前进或回绕/异常。
 * now/nsec 分别是原始终点和缺省为 0 的纳秒结果。
 */
u64 clocksource_stop_suspend_timing(struct clocksource *cs, u64 cycle_now)
{
	u64 now, nsec = 0;

	if (!suspend_clocksource)
		return 0;

	/*
	 * If current clocksource is the suspend timer, we should use the
	 * tkr_mono.cycle_last value from timekeeping as current cycle to
	 * avoid same reading from suspend timer.
	 */
	/* 同一源复用 timekeeper 快照；独立源才执行额外硬件读取。 */
	if (clocksource_is_suspend(cs))
		now = cycle_now;
	else
		now = suspend_clocksource->read(suspend_clocksource);

	/* 当前实现只接受数值严格前进；相等、倒退或跨 mask 回绕都以 0 交给上层回退。 */
	if (now > suspend_start)
		nsec = cycles_to_nsec_safe(suspend_clocksource, suspend_start, now);

	/*
	 * Disable the suspend timer to save power if current clocksource is
	 * not the suspend timer.
	 */
	/* 独立源完成本次读后即可关闭；当前源仍承担恢复后的系统计时，绝不能在此 disable。 */
	if (!clocksource_is_suspend(cs) && suspend_clocksource->disable)
		suspend_clocksource->disable(suspend_clocksource);

	return nsec;
}

/**
 * clocksource_suspend - suspend the clocksource(s)
 */
/*
 * clocksource_suspend() - 在 timekeeping 已冻结后按注册表反向调用全部可选驱动 suspend 回调。
 * 入参：无。timekeeping_suspend() 在进程冻结的 syscore 晚期、退出 timekeeper raw lock 后调用；此时注册
 * 表不会并发变化，因此不取 clocksource_mutex。@cs 是遍历借用指针，回调不得注销对象或依赖普通用户
 * 进程推进。返回无直接值且驱动回调无 errno；低 rating 到高 rating 的反向顺序与 resume 正向顺序配对。
 */
void clocksource_suspend(void)
{
	struct clocksource *cs;

	list_for_each_entry_reverse(cs, &clocksource_list, list)
		if (cs->suspend)
			cs->suspend(cs);
}

/**
 * clocksource_resume - resume the clocksource(s)
 */
/*
 * clocksource_resume() - 在 timekeeping 恢复结算前按注册表正向恢复驱动并重置 watchdog 基线。
 * 入参：无。timekeeping_resume() 在早期 syscore/单 CPU 阶段调用，列表由冻结协议稳定，不取 mutex；
 * @cs 为借用指针。高 rating 到低 rating 调用可选 resume，与 suspend 的反向顺序成对。全部回调完成后
 * atomic 请求 watchdog 丢弃跨 suspend 的旧时间戳，避免把停机时间误判为漂移。无直接返回值/错误传播。
 */
void clocksource_resume(void)
{
	struct clocksource *cs;

	list_for_each_entry(cs, &clocksource_list, list)
		if (cs->resume)
			cs->resume(cs);

	clocksource_resume_watchdog();
}

/**
 * clocksource_touch_watchdog - Update watchdog
 *
 * Update the watchdog after exception contexts such as kgdb so as not
 * to incorrectly trip the watchdog. This might fail when the kernel
 * was stopped in code which holds watchdog_lock.
 */
/*
 * kgdb 等异常上下文长时间停止系统后调用本接口，请求 watchdog 在下一轮重建参考快照，避免把调试暂停
 * 当作 clocksource 频率异常。原说明警告若停机点正持 watchdog_lock，保护可能不完整；当前实现本身仅
 * atomic_inc，不尝试取得该锁，也没有可返回的失败码，这一限制表示无法回滚已在锁内进行到一半的扫描。
 *
 * clocksource_touch_watchdog() - 无锁累加一次 watchdog reset 请求。
 * 入参：无；可从异常恢复上下文调用，不睡眠。返回无直接值；请求由后续完整扫描消费，配置关闭时为空。
 */
void clocksource_touch_watchdog(void)
{
	clocksource_resume_watchdog();
}

/**
 * clocksource_max_adjustment- Returns max adjustment amount
 * @cs:         Pointer to clocksource
 *
 */
/*
 * 返回 clocksource 定点乘数允许的最大校时增减量。@cs 是注册/调频路径稳定持有的借用指针，只读取
 * mult；函数无锁、不可睡眠。以 64 位 ret 计算 floor(mult*11/100)，避免中间乘法溢出，再返回 u32。
 * 结果约等于 110000ppm（11%），供 NTP 调整和最大安全转换范围共同使用，无失败码或副作用。
 */
static u32 clocksource_max_adjustment(struct clocksource *cs)
{
	u64 ret;
	/*
	 * We won't try to correct for more than 11% adjustments (110,000 ppm),
	 */
	/* 内核不会尝试超过 11%（110000ppm）的频率校正；该上限也必须能与 mult 安全相加减。 */
	ret = (u64)cs->mult * 11;
	do_div(ret,100);
	return (u32)ret;
}

/**
 * clocks_calc_max_nsecs - Returns maximum nanoseconds that can be converted
 * @mult:	cycle to nanosecond multiplier
 * @shift:	cycle to nanosecond divisor (power of two)
 * @maxadj:	maximum adjustment value to mult (~11%)
 * @mask:	bitmask for two's complement subtraction of non 64 bit counters
 * @max_cyc:	maximum cycle value before potential overflow (does not include
 *		any safety margin)
 *
 * NOTE: This function includes a safety margin of 50%, in other words, we
 * return half the number of nanoseconds the hardware counter can technically
 * cover. This is done so that we can potentially detect problems caused by
 * delayed timers or bad hardware, which might result in time intervals that
 * are larger than what the math used can handle without overflows.
 */
/*
 * 计算在 mult 最坏校正与 counter mask 约束下，可安全延迟再次读取 clocksource 的最长纳秒数。技术上限
 * 最后减半作为 50% 安全裕量，使延迟 timer 或坏硬件产生的异常大 delta 仍能被发现，而非直接撞上溢出。
 *
 * clocks_calc_max_nsecs() - 同时求安全纳秒延期和无裕量最大 cycle 输入。
 *
 * 【参数】@mult 是 cycle→ns 乘数；@shift 是二进制除数位数；@maxadj 是 mult 最大约 11% 调整量；
 * @mask 是窄 counter 模减掩码；@max_cyc 可为 NULL，否则是调用者持有的输出指针，写入未减半的 cycle
 * 上限。全部按值/借用，不保存 ownership。调用者必须保证 mult+maxadj 非零且 mult>=maxadj。
 *
 * 【变量与返回】max_cycles 取 U64_MAX/(mult+maxadj) 与 mask 较小者，防最大正校正时乘法溢出；
 * max_nsecs 再用 mult-maxadj 换算，覆盖负校正下更短的真实时间范围，最后减半返回。纯整数运算、无锁、
 * 不睡眠、无 errno；可选 max_cyc 输出不包含 50% 时间裕量，调用者另按其用途限制 delta。
 */
u64 clocks_calc_max_nsecs(u32 mult, u32 shift, u32 maxadj, u64 mask, u64 *max_cyc)
{
	u64 max_nsecs, max_cycles;

	/*
	 * Calculate the maximum number of cycles that we can pass to the
	 * cyc2ns() function without overflowing a 64-bit result.
	 */
	/* 阶段 1：按最大正 mult 反推 u64 乘积仍可容纳的 cycle 数。 */
	max_cycles = ULLONG_MAX;
	do_div(max_cycles, mult+maxadj);

	/*
	 * The actual maximum number of cycles we can defer the clocksource is
	 * determined by the minimum of max_cycles and mask.
	 * Note: Here we subtract the maxadj to make sure we don't sleep for
	 * too long if there's a large negative adjustment.
	 */
	/* 阶段 2：硬件一圈的 mask 可能更小；用最小 mult 换成 ns，避免负调频时跨回绕窗口睡过头。 */
	max_cycles = min(max_cycles, mask);
	max_nsecs = clocksource_cyc2ns(max_cycles, mult - maxadj, shift);

	/* return the max_cycles value as well if requested */
	/* 可选输出保留纯算术/mask 上限，调用者可为安全慢乘路径判断 delta。 */
	if (max_cyc)
		*max_cyc = max_cycles;

	/* Return 50% of the actual maximum, so we can detect bad values */
	/* 阶段 3：对可延期时间折半，为迟到读数和异常硬件保留可检测空间。 */
	max_nsecs >>= 1;

	return max_nsecs;
}

/**
 * clocksource_update_max_deferment - Updates the clocksource max_idle_ns & max_cycles
 * @cs:         Pointer to clocksource to be updated
 *
 */
/*
 * clocksource_update_max_deferment() - 根据当前 mult/shift/maxadj/mask 刷新两个延期门限。
 * @cs 是注册/调频路径持有的可写借用对象；调用者尚未发布新比例或已负责外部串行，本函数自身无锁、
 * 不睡眠。先写 max_idle_ns 和 max_cycles，再把 max_raw_delta 设为 mask 的 1/2+1/4+1/8（0.875 圈）。
 * 返回无直接值；三个字段必须与同一组换算参数一起发布，否则 watchdog/NO_HZ 会使用不匹配边界。
 */
static inline void clocksource_update_max_deferment(struct clocksource *cs)
{
	cs->max_idle_ns = clocks_calc_max_nsecs(cs->mult, cs->shift,
						cs->maxadj, cs->mask,
						&cs->max_cycles);

	/*
	 * Threshold for detecting negative motion in clocksource_delta().
	 *
	 * Allow for 0.875 of the counter width so that overly long idle
	 * sleeps, which go slightly over mask/2, do not trigger the
	 * negative motion detection.
	 */
	/*
	 * max_raw_delta 是 clocksource_delta() 的反向运动门限。放宽到 counter 宽度的 0.875，而非简单半圈，
	 * 可让略超 mask/2 的长 idle 仍按正常前进处理；接近整圈的模差才被视为疑似倒退并压成 0。
	 */
	cs->max_raw_delta = (cs->mask >> 1) + (cs->mask >> 2) + (cs->mask >> 3);
}

/*
 * clocksource_find_best() - 从按 rating 降序的注册表返回第一个满足当前 tick 模式的候选。
 * @oneshot 表示 highres/NO_HZ oneshot 已生效，此时候选必须 VALID_FOR_HRES；@skipcur 用于注销当前源时
 * 排除它。调用者持 clocksource_mutex；返回注册表借用指针或 NULL（启动未完成、列表空或无合格对象），
 * 不增加模块引用、不 enable 硬件。WDTEST 对象永不进入生产选择。函数不自行睡眠或改状态。
 */
static struct clocksource *clocksource_find_best(bool oneshot, bool skipcur)
{
	struct clocksource *cs;

	if (!finished_booting || list_empty(&clocksource_list))
		return NULL;

	/*
	 * We pick the clocksource with the highest rating. If oneshot
	 * mode is active, we pick the highres valid clocksource with
	 * the best rating.
	 */
	/* 主表已按 rating 降序，过滤后遇到的首项就是最佳；oneshot 不能退到尚未验证 HRES 的源。 */
	list_for_each_entry(cs, &clocksource_list, list) {
		if (skipcur && cs == curr_clocksource)
			continue;
		if (oneshot && !(cs->flags & CLOCK_SOURCE_VALID_FOR_HRES))
			continue;
		if (cs->flags & CLOCK_SOURCE_WDTEST)
			continue;
		return cs;
	}
	return NULL;
}

/*
 * __clocksource_select() - 综合自动 rating、tick 约束和用户 override，向 timekeeping 提交最佳源。
 *
 * 【参数与锁】@skipcur 为 true 时排除当前源，用于注销 fallback。调用者必须持 clocksource_mutex；
 * best/cs 是注册表借用指针，oneshot 是 tick 当前模式快照。timekeeping_notify() 可能经 stop_machine
 * 睡眠并取得新源模块引用，所以本函数只能在进程上下文调用。
 *
 * 【阶段】先取得自动最佳；override_name 为空直接提交，否则查同名非 WDTEST 对象。oneshot 下，override
 * 若无 HRES 资格：已 UNSTABLE 则清除永久无效覆盖，未完成验证则保留名字并暂缓；其他情况覆盖 best。
 * 最后仅当 best 不同且 notify 返回 0（实际安装成功）才打印并更新 curr_clocksource。
 *
 * 【返回】无直接返回值。无候选或切换失败保持原选择；清 override 是不稳定用户请求的可观察副作用。
 * curr_clocksource 的提交晚于 timekeeper 真正切换，避免两层状态分裂。
 */
static void __clocksource_select(bool skipcur)
{
	bool oneshot = tick_oneshot_mode_active();
	struct clocksource *best, *cs;

	/* Find the best suitable clocksource */
	/* 阶段 1：自动策略先提供始终可用的 best，override 只在合法时替换它。 */
	best = clocksource_find_best(oneshot, skipcur);
	if (!best)
		return;

	if (!strlen(override_name))
		goto found;

	/* Check for the override clocksource. */
	/* 阶段 2：名字精确匹配；测试源和 fallback 要排除的当前源不能被用户强行选回。 */
	list_for_each_entry(cs, &clocksource_list, list) {
		if (skipcur && cs == curr_clocksource)
			continue;
		if (strcmp(cs->name, override_name) != 0)
			continue;
		if (cs->flags & CLOCK_SOURCE_WDTEST)
			continue;
		/*
		 * Check to make sure we don't switch to a non-highres
		 * capable clocksource if the tick code is in oneshot
		 * mode (highres or nohz)
		 */
		/* oneshot 已依赖可靠连续时基，切到非 HRES 源会破坏 highres/NO_HZ 定时语义。 */
		if (!(cs->flags & CLOCK_SOURCE_VALID_FOR_HRES) && oneshot) {
			/* Override clocksource cannot be used. */
			/* 已判 unstable 的源不可能通过后续验证，清名字回到长期自动选择。 */
			if (cs->flags & CLOCK_SOURCE_UNSTABLE) {
				pr_warn("Override clocksource %s is unstable and not HRT compatible - cannot switch while in HRT/NOHZ mode\n",
					cs->name);
				override_name[0] = 0;
			} else {
				/*
				 * The override cannot be currently verified.
				 * Deferring to let the watchdog check.
				 */
				/* 尚未验证不是永久失败；保留 override，等 watchdog 开放 HRES 后再由 RESELECT 兑现。 */
				pr_info("Override clocksource %s is not currently HRT compatible - deferring\n",
					cs->name);
			}
		} else
			/* Override clocksource can be used. */
			/* 同名候选满足当前模式，用户选择替换自动 best。 */
			best = cs;
		break;
	}

found:
	/* 阶段 3（提交）：timekeeping 先实际切换并管理模块/enable 生命周期，成功后才更新 core 镜像指针。 */
	if (curr_clocksource != best && !timekeeping_notify(best)) {
		pr_info("Switched to clocksource %s\n", best->name);
		curr_clocksource = best;
	}
}

/**
 * clocksource_select - Select the best clocksource available
 *
 * Private function. Must hold clocksource_mutex when called.
 *
 * Select the clocksource with the best rating, or the clocksource,
 * which is selected by userspace override.
 */
/*
 * 选择当前可用的最佳 clocksource，或合法的用户 override。入参：无；调用者必须持
 * clocksource_mutex，可睡眠。它只是 __clocksource_select(false) 的普通包装，无直接返回值；切换失败
 * 保留旧源，真正对象引用和 enable/disable 由 timekeeping_notify() 管理。
 */
static void clocksource_select(void)
{
	__clocksource_select(false);
}

/*
 * clocksource_select_fallback() - 排除当前源后尝试安装一个替代 clocksource。
 * 入参：无；调用者持 clocksource_mutex，可睡眠。无直接返回值，调用者通过 curr_clocksource 是否仍为
 * 原对象判断替换成功；没有合格候选或 notify 失败都保持当前源，从而阻止其注销。
 */
static void clocksource_select_fallback(void)
{
	__clocksource_select(true);
}

/*
 * clocksource_done_booting - Called near the end of core bootup
 *
 * Hack to avoid lots of clocksource churn at boot time.
 * We use fs_initcall because we want this to start before
 * device_initcall but after subsys_initcall.
 */
/*
 * 启动末期启用正式选择。fs_initcall 位于 subsys_initcall 之后、device_initcall 之前，既让早期驱动先
 * 注册候选，又避免每注册一个就反复 stop_machine 切换。
 *
 * clocksource_done_booting() - 建立默认当前源、兑现早期 watchdog 标志并执行第一次正式选择。
 * 入参：无；在可睡眠 initcall 上下文取得 clocksource_mutex。先从架构默认源初始化 curr，发布
 * finished_booting=1，再处理早期 unstable/reselect，最后按完整策略 select。固定返回 0；列表对象仍由
 * 驱动持有，后续注册可以继续触发重选。
 */
static int __init clocksource_done_booting(void)
{
	mutex_lock(&clocksource_mutex);
	curr_clocksource = clocksource_default_clock();
	/* finished_booting 是允许 watchdog work/正常选择的发布点。 */
	finished_booting = 1;
	/*
	 * Run the watchdog first to eliminate unstable clock sources
	 */
	/* 先把启动期已标记不稳定的源降 rating，再选择，避免短暂安装已知坏源。 */
	__clocksource_watchdog_kthread();
	clocksource_select();
	mutex_unlock(&clocksource_mutex);
	return 0;
}
fs_initcall(clocksource_done_booting);

/*
 * Enqueue the clocksource sorted by rating
 */
/* 按 rating 降序插入新 clocksource；相同 rating 保持已有对象在前，提供稳定的平局顺序。 */
/*
 * clocksource_enqueue() - 把 @cs 的 list 节点加入主注册表正确位置。
 * @cs 是注册中或改 rating 的借用对象，入口节点必须未链接；调用者持 clocksource_mutex，并在降级路径
 * 同时持 watchdog_lock。entry 从表头哨兵开始，tmp 逐项借用；遇到首个更低 rating 停止，否则记住
 * 最后节点，list_add 在 entry 后提交。无返回值、不分配、不睡眠、不转移对象 ownership。
 */
static void clocksource_enqueue(struct clocksource *cs)
{
	struct list_head *entry = &clocksource_list;
	struct clocksource *tmp;

	list_for_each_entry(tmp, &clocksource_list, list) {
		/* Keep track of the place, where to insert */
		/* 保存最后一个 rating>=新对象的位置；首个更低对象前就是插入边界。 */
		if (tmp->rating < cs->rating)
			break;
		entry = &tmp->list;
	}
	list_add(&cs->list, entry);
}

/**
 * __clocksource_update_freq_scale - Used update clocksource with new freq
 * @cs:		clocksource to be registered
 * @scale:	Scale factor multiplied against freq to get clocksource hz
 * @freq:	clocksource frequency (cycles per second) divided by scale
 */
/*
 * __clocksource_update_freq_scale() - 由频率建立 clocksource 换算参数及所有派生安全边界。
 *
 * 【调用位置】注册入口在对象加入任何全局表之前调用。@cs 是驱动拥有、当前可写的借用对象；@scale 与
 * @freq 的乘积是真实 Hz，常见 scale=1/1000 对应 hz/khz helper。freq=0 仅允许 jiffies 等默认特殊源，
 * 表示驱动已自定义 mult/shift。函数无外部锁要求、不可睡眠，不保存指针。
 *
 * 【变量与阶段】sec 估算 counter 一圈秒数；普通源据此调用 clocks_calc_mult_shift，并发布实际
 * freq_khz。随后 maxadj 取 11%，若 mult±maxadj 会 u32 环绕，就同步右移 mult/shift 保持比例并重算；
 * 特殊 freq=0 源无法自动修正，只对正向溢出告警。最后更新 max_idle_ns/max_cycles/max_raw_delta 并打印。
 *
 * 【返回】无直接返回值/错误码。成功后 mult、shift、freq_khz、maxadj 与三个范围字段属于同一代参数；
 * 驱动必须提供非零 scale，普通源还需非零 freq，错误输入可能除零，函数不校验。
 */
static void __clocksource_update_freq_scale(struct clocksource *cs, u32 scale, u32 freq)
{
	u64 sec;

	/*
	 * Default clocksources are *special* and self-define their mult/shift.
	 * But, you're not special, so you should specify a freq value.
	 */
	/* 默认 clocksource 可预置比例；所有普通驱动必须给 freq，让 core 统一计算和约束精度。 */
	if (freq) {
		/*
		 * Calc the maximum number of seconds which we can run before
		 * wrapping around. For clocksources which have a mask > 32-bit
		 * we need to limit the max sleep time to have a good
		 * conversion precision. 10 minutes is still a reasonable
		 * amount. That results in a shift value of 24 for a
		 * clocksource with mask >= 40-bit and f >= 4GHz. That maps to
		 * ~ 0.06ppm granularity for NTP.
		 */
		/*
		 * 阶段 1：mask/(freq*scale) 是约一圈秒数。结果不足 1 秒按 1 处理；宽于 32 位且超过 10 分钟时
		 * 封顶 600 秒，换取更大的 shift 和约 0.06ppm NTP 粒度，而非为极长 idle 牺牲转换精度。
		 */
		sec = cs->mask;
		do_div(sec, freq);
		do_div(sec, scale);
		if (!sec)
			sec = 1;
		else if (sec > 600 && cs->mask > UINT_MAX)
			sec = 600;

		clocks_calc_mult_shift(&cs->mult, &cs->shift, freq,
				       NSEC_PER_SEC / scale, sec * scale);

		/* Update cs::freq_khz */
		/* 保存实际 freq*scale 的 kHz 供诊断/消费者使用，整数除法向下取整。 */
		cs->freq_khz = div_u64((u64)freq * scale, 1000);
	}

	/*
	 * Ensure clocksources that have large 'mult' values don't overflow
	 * when adjusted.
	 */
	/* 阶段 2：保证 11% NTP 调整在 u32 mult 两侧都不环绕；同步减 shift 近似保持换算比例。 */
	cs->maxadj = clocksource_max_adjustment(cs);
	while (freq && ((cs->mult + cs->maxadj < cs->mult)
		|| (cs->mult - cs->maxadj > cs->mult))) {
		cs->mult >>= 1;
		cs->shift--;
		cs->maxadj = clocksource_max_adjustment(cs);
	}

	/*
	 * Only warn for *special* clocksources that self-define
	 * their mult/shift values and don't specify a freq.
	 */
	/* freq=0 的特殊源不能由 core 重算比例；若自定义 mult 连正向 11% 都容不下，只能保留并告警。 */
	WARN_ONCE(cs->mult + cs->maxadj < cs->mult,
		"timekeeping: Clocksource %s might overflow on 11%% adjustment\n",
		cs->name);

	/* 阶段 3：从最终比例一次性重建 NO_HZ 延期、防溢出和反向运动门限。 */
	clocksource_update_max_deferment(cs);

	pr_info("%s: mask: 0x%llx max_cycles: 0x%llx, max_idle_ns: %lld ns\n",
		cs->name, cs->mask, cs->max_cycles, cs->max_idle_ns);
}

/**
 * __clocksource_register_scale - Used to install new clocksources
 * @cs:		clocksource to be registered
 * @scale:	Scale factor multiplied against freq to get clocksource hz
 * @freq:	clocksource frequency (cycles per second) divided by scale
 *
 * Returns -EBUSY if registration fails, zero otherwise.
 *
 * This *SHOULD NOT* be called directly! Please use the
 * clocksource_register_hz() or clocksource_register_khz helper functions.
 */
/*
 * 安装一个新 clocksource。调用者通常应使用 hz/khz 包装，避免手工传错 scale。原 kernel-doc 声称注册
 * 失败返回 -EBUSY，但当前实现没有任何负返回出口：所有输入异常都被告警并降级字段，最终固定返回 0。
 *
 * __clocksource_register_scale() - 完成字段规范化并把驱动对象发布到选择、watchdog 和 suspend 三套视图。
 *
 * 【参数与 ownership】@cs 是驱动创建并长期持有的非 NULL 对象；@scale/@freq 语义同更新 helper。
 * 注册表只借用 cs，不增加通用引用；成功后驱动在 clocksource_unregister() 成功前不得释放/复用对象。
 * 函数在可睡眠进程/初始化上下文调用，入口不持 core 锁。
 *
 * 【阶段】1. 架构校验 VDSO 等私有约束，并把非法 id、无 freq 的 coupled-event 标志、非法 vdso mode
 * 修正为安全通用值；2. 计算换算与范围字段；3. 取得 clocksource_mutex，再短暂取得 watchdog_lock，
 * 按固定锁序把对象发布到主表和监督表；4. 在 mutex 下重选当前源、参考源和 suspend 源。
 *
 * 【返回与副作用】当前版本固定返回 0。主表 list_add 是注册发布点；之后 timekeeping_notify 可能取得
 * 模块引用并 enable 新源。无 cleanup 标签，因为实现无可报告失败；输入告警后的字段修正不会回滚。
 */
int __clocksource_register_scale(struct clocksource *cs, u32 scale, u32 freq)
{
	unsigned long flags;

	/* 阶段 1：架构可收紧通用字段契约，例如 x86 会拒绝非 64 位 mask 的 VDSO clock mode。 */
	clocksource_arch_init(cs);

	if (WARN_ON_ONCE((unsigned int)cs->id >= CSID_MAX))
		/* snapshot 消费者不能识别越界 id，回退通用身份而非拒绝整个时钟。 */
		cs->id = CSID_GENERIC;

	if (WARN_ON_ONCE(!freq && cs->flags & CLOCK_SOURCE_HAS_COUPLED_CLOCK_EVENT))
		/* coupled event 计算依赖已知频率；特殊自定义比例源不能承诺该能力。 */
		cs->flags &= ~CLOCK_SOURCE_HAS_COUPLED_CLOCK_EVENT;

	if (cs->vdso_clock_mode < 0 ||
	    cs->vdso_clock_mode >= VDSO_CLOCKMODE_MAX) {
		pr_warn("clocksource %s registered with invalid VDSO mode %d. Disabling VDSO support.\n",
			cs->name, cs->vdso_clock_mode);
		cs->vdso_clock_mode = VDSO_CLOCKMODE_NONE;
	}

	/* Initialize mult/shift and max_idle_ns */
	/* 阶段 2：对象尚未发布，所有换算字段可无锁成组更新。 */
	__clocksource_update_freq_scale(cs, scale, freq);

	/* Add clocksource to the clocksource list */
	/* 阶段 3：mutex 串行化注册/注销/选择；内层 watchdog_lock 保护监督链和 flags。 */
	mutex_lock(&clocksource_mutex);

	clocksource_watchdog_lock(&flags);
	clocksource_enqueue(cs);
	clocksource_enqueue_watchdog(cs);
	clocksource_watchdog_unlock(&flags);

	/* 阶段 4：主源切换可睡眠，必须已离开 spinlock；三个视图都仍由 mutex 稳定。 */
	clocksource_select();
	clocksource_select_watchdog(false);
	__clocksource_suspend_select(cs);
	mutex_unlock(&clocksource_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(__clocksource_register_scale);

/*
 * __devm_clocksource_unregister() - 把 devres 清理回调适配为普通 clocksource 注销。
 * @data 是注册时保存的 cs 借用指针，设备资源层只保存地址、不取得额外 clocksource 引用。运行在可睡眠
 * devres 释放/回滚上下文，入口无 core 锁。函数无返回值并忽略 unregister 的 -EBUSY；驱动必须保证设备
 * 资源释放时已有替代主源/watchdog，否则对象仍注册却可能随设备内存释放，违反生命周期契约。
 */
static void __devm_clocksource_unregister(void *data)
{
	struct clocksource *cs = data;

	clocksource_unregister(cs);
}

/*
 * __devm_clocksource_register_scale() - 注册 clocksource 并把对应注销动作绑定到设备资源生命周期。
 *
 * 【参数】@dev 是 devres 所属设备借用指针；@cs 是驱动对象借用指针；@scale/@freq 同普通注册入口。
 * 成功后 devres 持有“将来以 cs 调用清理函数”的责任，cs 内存仍由驱动/设备资源管理。
 *
 * 【阶段与返回】先调用普通注册；若其返回负值直接透传且不登记 action。随后
 * devm_add_action_or_reset() 分配 action：成功返回 0；失败时 helper 立即调用注销回滚并返回 -ENOMEM 等
 * errno。可睡眠。当前普通注册固定 0，但保留分支兼容未来错误契约。
 */
int __devm_clocksource_register_scale(struct device *dev, struct clocksource *cs,
				      u32 scale, u32 freq)
{
	int ret;

	ret = __clocksource_register_scale(cs, scale, freq);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, __devm_clocksource_unregister, cs);
}
EXPORT_SYMBOL_GPL(__devm_clocksource_register_scale);

/*
 * Unbind clocksource @cs. Called with clocksource_mutex held
 */
/* 从 core 的所有角色中解绑 @cs；调用者已持 clocksource_mutex，@cs 是仍在主表的借用对象。 */
/*
 * clocksource_unbind() - 先消除全局指针引用，再从 watchdog/main list 摘除对象。
 *
 * 【阶段】若 cs 是 watchdog，必须先选择不同连续参考源；仍指向 cs 则返回 -EBUSY。若 cs 是当前
 * timekeeping 源，再排除它调用 fallback，实际切换失败同样 -EBUSY。若它是 suspend 源则尝试替代，允许
 * 没有替代而清为 NULL。最后在 watchdog_lock 下摘监督链和主链，list_del_init 标记未注册。
 *
 * 【返回与 ownership】成功返回 0，core 不再借用 cs，调用者之后可释放；失败返回 -EBUSY，cs 仍在主表
 * 且不可释放。失败前 watchdog 可能已成功换成别的对象，这是合法的部分状态变化，不需要回滚旧参考源。
 * 函数可因 fallback timekeeping 切换睡眠；锁序始终 mutex→watchdog spin。
 */
static int clocksource_unbind(struct clocksource *cs)
{
	unsigned long flags;

	if (clocksource_is_watchdog(cs)) {
		/* Select and try to install a replacement watchdog. */
		/* 阶段 1：参考指针必须先离开 cs，否则摘链会留下 timer 使用的悬空引用。 */
		clocksource_select_watchdog(true);
		if (clocksource_is_watchdog(cs))
			return -EBUSY;
	}

	if (cs == curr_clocksource) {
		/* Select and try to install a replacement clock source */
		/* 阶段 2：timekeeping_notify 真正安装替代源后 curr 才变化；失败则禁止解绑。 */
		clocksource_select_fallback();
		if (curr_clocksource == cs)
			return -EBUSY;
	}

	if (clocksource_is_suspend(cs)) {
		/*
		 * Select and try to install a replacement suspend clocksource.
		 * If no replacement suspend clocksource, we will just let the
		 * clocksource go and have no suspend clocksource.
		 */
		/* suspend 计时是可选增强，无替代时清为 NULL 并由 persistent clock/RTC 兜底，不阻止注销。 */
		clocksource_suspend_select(true);
	}

	/* 阶段 4（摘除提交）：所有全局角色已安全迁移，spinlock 下同时断开监督链和主注册链。 */
	clocksource_watchdog_lock(&flags);
	clocksource_dequeue_watchdog(cs);
	list_del_init(&cs->list);
	clocksource_watchdog_unlock(&flags);

	return 0;
}

/**
 * clocksource_unregister - remove a registered clocksource
 * @cs:	clocksource to be unregistered
 */
/*
 * clocksource_unregister() - 对外串行注销一个已注册 clocksource。
 * @cs 是驱动持有的非 NULL 对象；函数不取得/释放驱动内存 ownership。运行在可睡眠上下文，入口无锁，
 * 内部取得 clocksource_mutex。list 已空时幂等返回 0；仍注册时调用 unbind，返回 0 或因当前主源/唯一
 * watchdog 无法替换而返回 -EBUSY。只有返回 0 后驱动才可 disable（若自身需要）并释放对象存储。
 */
int clocksource_unregister(struct clocksource *cs)
{
	int ret = 0;

	mutex_lock(&clocksource_mutex);
	if (!list_empty(&cs->list))
		ret = clocksource_unbind(cs);
	mutex_unlock(&clocksource_mutex);
	return ret;
}
EXPORT_SYMBOL(clocksource_unregister);

#ifdef CONFIG_SYSFS
/**
 * current_clocksource_show - sysfs interface for current clocksource
 * @dev:	unused
 * @attr:	unused
 * @buf:	char buffer to be filled with clocksource list
 *
 * Provides sysfs interface for listing current clocksource.
 */
/*
 * 把当前 clocksource 名称输出到 sysfs。@dev/@attr 是 device attribute 回调占位借用指针，当前不用；
 * @buf 是 sysfs 提供的 PAGE_SIZE 输出缓冲，调用者持有。函数在可睡眠 sysfs 读上下文取得
 * clocksource_mutex，使 curr_clocksource 在取 name 期间不会被切换/注销；启动顺序保证设备发布时该指针
 * 已有效。返回写入字节数（含换行），无错误出口、引用转移或长期副作用；count 是本次输出长度。
 */
static ssize_t current_clocksource_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	ssize_t count = 0;

	mutex_lock(&clocksource_mutex);
	count = sysfs_emit(buf, "%s\n", curr_clocksource->name);
	mutex_unlock(&clocksource_mutex);

	return count;
}

/*
 * sysfs_get_uname() - 把未以 NUL 结尾的 sysfs 写缓冲规范化为 clocksource 名称。
 * @buf 是长度为 @cnt 的纯输入内核缓冲；@dst 是调用者提供、容量至少 CS_NAME_LEN 的输出缓冲。两者只
 * 借用且不可重叠依赖。cnt 必须为 1..CS_NAME_LEN-1；可选的最后一个换行被剥离，剩余字节原样复制并
 * 追加 NUL。成功返回原始 cnt（使 store 符合“消费全部输入”约定），空/过长返回 -EINVAL 且 dst 内容
 * 不保证不变。纯内存操作、无锁、不睡眠；ret 保存剥离换行前的成功返回长度。
 */
ssize_t sysfs_get_uname(const char *buf, char *dst, size_t cnt)
{
	size_t ret = cnt;

	/* strings from sysfs write are not 0 terminated! */
	/* sysfs 只给显式长度，不能直接 strlen/strcmp；还要为 dst 的终止 NUL 留一字节。 */
	if (!cnt || cnt >= CS_NAME_LEN)
		return -EINVAL;

	/* strip of \n: */
	/* echo 通常附带一个末尾换行；只剥离最后一个，不修剪其他空白。 */
	if (buf[cnt-1] == '\n')
		cnt--;
	if (cnt > 0)
		memcpy(dst, buf, cnt);
	dst[cnt] = 0;
	return ret;
}

/**
 * current_clocksource_store - interface for manually overriding clocksource
 * @dev:	unused
 * @attr:	unused
 * @buf:	name of override clocksource
 * @count:	length of buffer
 *
 * Takes input from sysfs interface for manually overriding the default
 * clocksource selection.
 */
/*
 * 处理 current_clocksource sysfs 写入，保存用户 override 并立即尝试重选。@dev/@attr 未使用；@buf 是
 * 长度 @count 的 sysfs 输入借用缓冲。函数在可睡眠进程上下文取得 clocksource_mutex，直接把规范化名字
 * 写入全局 override_name，使解析与 select 属于同一串行临界区。解析失败返回 -EINVAL且不重选；成功
 * 返回原始 count，即使名字不存在、暂不能 HRES 或底层切换失败也仍算写入成功：名字会保留供后续注册/
 * watchdog 重选使用。ret 保存解析返回值，无额外资源 ownership。
 */
static ssize_t current_clocksource_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	ssize_t ret;

	mutex_lock(&clocksource_mutex);

	/* 提交 override 字符串后立即在同一 mutex 代内评估，避免注册/注销看到半个名字。 */
	ret = sysfs_get_uname(buf, override_name, count);
	if (ret >= 0)
		clocksource_select();

	mutex_unlock(&clocksource_mutex);

	return ret;
}
/* 生成 current_clocksource 的 show/store 属性对象；对象静态存续并由下方 attribute group 借用。 */
static DEVICE_ATTR_RW(current_clocksource);

/**
 * unbind_clocksource_store - interface for manually unbinding clocksource
 * @dev:	unused
 * @attr:	unused
 * @buf:	unused
 * @count:	length of buffer
 *
 * Takes input from sysfs interface for manually unbinding a clocksource.
 */
/*
 * 按 sysfs 输入名称注销一个 clocksource。原 kernel-doc 把 @buf 写成 unused，但当前实现实际解析它；
 * @dev/@attr 才未使用。@buf/@count 为借用输入，name 是 NUL 结尾的栈上候选名，cs 为 mutex 下列表游标，
 * ret 依次表示解析结果、缺省 -ENODEV 或 unbind 结果。
 *
 * 解析在锁外完成，失败直接 -EINVAL；随后在可睡眠 clocksource_mutex 下精确匹配首个名字并调用
 * clocksource_unbind()。找不到返回 -ENODEV；对象仍是当前源/唯一 watchdog 时返回 -EBUSY；成功返回
 * 原始 count。成功摘链后 core 不再持有对象，但 sysfs 本身不释放驱动内存或模块引用。
 */
static ssize_t unbind_clocksource_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct clocksource *cs;
	char name[CS_NAME_LEN];
	ssize_t ret;

	ret = sysfs_get_uname(buf, name, count);
	if (ret < 0)
		return ret;

	/* 阶段 2：以 -ENODEV 为“遍历结束仍未命中”的哨兵，锁内稳定名字和对象生命周期。 */
	ret = -ENODEV;
	mutex_lock(&clocksource_mutex);
	list_for_each_entry(cs, &clocksource_list, list) {
		if (strcmp(cs->name, name))
			continue;
		ret = clocksource_unbind(cs);
		break;
	}
	mutex_unlock(&clocksource_mutex);

	return ret ? ret : count;
}
/* 生成只写 unbind_clocksource 属性；其 store 的成功返回仍是用户输入 count。 */
static DEVICE_ATTR_WO(unbind_clocksource);

/**
 * available_clocksource_show - sysfs interface for listing clocksource
 * @dev:	unused
 * @attr:	unused
 * @buf:	char buffer to be filled with clocksource list
 *
 * Provides sysfs interface for listing registered clocksources
 */
/*
 * 列出当前 tick 模式下用户可选择的已注册 clocksources。@dev/@attr 未使用；@buf 是 sysfs PAGE_SIZE 输出
 * 缓冲借用指针。函数在 clocksource_mutex 下按 rating 顺序遍历 src；oneshot/highres/NO_HZ 活跃时隐藏
 * 非 VALID_FOR_HRES 源，避免向用户展示当前不能安全切换的选项。每个名称后加空格，解锁后追加换行。
 * 返回格式化长度，无 errno、引用或状态修改；count 跟踪已使用/理论长度，snprintf 剩余量被钳到非负。
 */
static ssize_t available_clocksource_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct clocksource *src;
	ssize_t count = 0;

	mutex_lock(&clocksource_mutex);
	list_for_each_entry(src, &clocksource_list, list) {
		/*
		 * Don't show non-HRES clocksource if the tick code is
		 * in one shot mode (highres=on or nohz=on)
		 */
		/* oneshot 已依赖连续高精度时基，只展示通过 watchdog/固有资格验证的 HRES 源。 */
		if (!tick_oneshot_mode_active() ||
		    (src->flags & CLOCK_SOURCE_VALID_FOR_HRES))
			count += snprintf(buf + count,
				  max((ssize_t)PAGE_SIZE - count, (ssize_t)0),
				  "%s ", src->name);
	}
	mutex_unlock(&clocksource_mutex);

	count += snprintf(buf + count,
			  max((ssize_t)PAGE_SIZE - count, (ssize_t)0), "\n");

	return count;
}
/* 生成只读 available_clocksource 属性，静态存续并加入统一属性组。 */
static DEVICE_ATTR_RO(available_clocksource);

/*
 * clocksource_attrs 是以 NULL 结尾的静态属性指针表，依次发布当前选择、手工解绑和可用列表；
 * ATTRIBUTE_GROUPS 生成 clocksource_groups，供 device_clocksource.groups 借用，均无运行期释放责任。
 */
static struct attribute *clocksource_attrs[] = {
	&dev_attr_current_clocksource.attr,
	&dev_attr_unbind_clocksource.attr,
	&dev_attr_available_clocksource.attr,
	NULL
};
ATTRIBUTE_GROUPS(clocksource);

/* 静态 bus_type 为 sysfs 创建 clocksource 子系统/设备命名空间；注册后由 driver core 管理可见性。 */
static const struct bus_type clocksource_subsys = {
	.name = "clocksource",
	.dev_name = "clocksource",
};

/* id=0 的静态设备挂到 clocksource_subsys，并在注册时发布上述属性组；对象内存常驻，不动态分配。 */
static struct device device_clocksource = {
	.id	= 0,
	.bus	= &clocksource_subsys,
	.groups	= clocksource_groups,
};

/*
 * init_clocksource_sysfs() - 在 device_initcall 阶段发布 clocksource 总线、设备和属性。
 * 入参：无；运行在可睡眠初始化上下文。先 subsys_system_register()，成功后 device_register()；返回 0
 * 或首个 errno。若设备注册失败，当前函数不注销已成功的 subsystem，留下的部分注册由启动错误状态
 * 体现；本文件没有重试/cleanup 标签。error 是两阶段共享的返回状态。
 */
static int __init init_clocksource_sysfs(void)
{
	int error = subsys_system_register(&clocksource_subsys, NULL);

	if (!error)
		error = device_register(&device_clocksource);

	return error;
}

/* sysfs 晚于核心 fs_initcall 选择初始化，用户看到设备时 curr_clocksource 已建立。 */
device_initcall(init_clocksource_sysfs);
#endif /* CONFIG_SYSFS */

/**
 * boot_override_clocksource - boot clock override
 * @str:	override name
 *
 * Takes a clocksource= boot argument and uses it
 * as the clocksource override name.
 */
/*
 * 解析 clocksource=<name> 启动参数。@str 是 early boot 命令行存储中的借用字符串，可为 NULL；函数在
 * __setup 解析上下文取得 clocksource_mutex，把非 NULL 名称以 strscpy 截断/NUL 终止写入 override_name。
 * 固定返回 1 表示参数已消费，无失败码；此时可能尚无同名源，正式选择由 done_booting 或后续注册完成。
 * __init 代码启动后可回收，override_name 静态常驻。
 */
static int __init boot_override_clocksource(char* str)
{
	mutex_lock(&clocksource_mutex);
	if (str)
		strscpy(override_name, str);
	mutex_unlock(&clocksource_mutex);
	return 1;
}

/* 把 clocksource= 前缀注册到启动参数解析表，匹配后调用上方消费者。 */
__setup("clocksource=", boot_override_clocksource);

/**
 * boot_override_clock - Compatibility layer for deprecated boot option
 * @str:	override name
 *
 * DEPRECATED! Takes a clock= boot argument and uses it
 * as the clocksource override name
 */
/*
 * 兼容已废弃的 clock=<name> 参数。@str 是必须非 NULL 的启动字符串借用指针；pmtmr 旧名专门映射为
 * acpi_pm，其余名称原样转交新入口，两个分支都打印弃用警告。返回新入口的 1，表示已消费；函数不直接
 * 选择硬件，锁与截断由 boot_override_clocksource() 负责，代码在启动后可回收。
 */
static int __init boot_override_clock(char* str)
{
	if (!strcmp(str, "pmtmr")) {
		pr_warn("clock=pmtmr is deprecated - use clocksource=acpi_pm\n");
		return boot_override_clocksource("acpi_pm");
	}
	pr_warn("clock= boot option is deprecated - use clocksource=xyz\n");
	return boot_override_clocksource(str);
}

/* 注册兼容 clock= 前缀；仅保留旧命令行可用性，新配置应使用 clocksource=。 */
__setup("clock=", boot_override_clock);
