// SPDX-License-Identifier: GPL-2.0
/*
 *  Kernel timekeeping code and accessor functions. Based on code from
 *  timer.c, moved in commit 8524070b7982.
 */
/*
 * 补充说明：本文件把“不规则增长的硬件计数器”转换成内核可使用的多个时间域。
 * clocksource 只回答“当前周期数是多少”；timekeeper 负责保存上次采样、换算斜率、
 * 历史基点和各时间域偏移，并在 tick、设时、NTP、挂起恢复时提交新状态。
 *
 * 【宏观地图】
 *
 *   硬件 clocksource.read() -> cycles
 *          |  delta = (cycles - cycle_last) & mask
 *          |  ns = (delta * mult + xtime_nsec) >> shift
 *          v
 *   tk_read_base / timekeeper
 *          |-- CLOCK_MONOTONIC     base + 当前周期增量；不随 settimeofday 跳变
 *          |-- CLOCK_REALTIME      monotonic + offs_real；可被人工/NTP 调整
 *          |-- CLOCK_BOOTTIME      monotonic + offs_boot；包含 suspend 睡眠
 *          |-- CLOCK_TAI           monotonic + offs_tai；realtime 再加 TAI 偏移
 *          `-- CLOCK_MONOTONIC_RAW 原始 mult，不施加 NTP 频率校正
 *
 *   写侧：tkd->lock -> 修改 shadow_timekeeper -> write_seqcount_begin
 *         -> 更新 VDSO/fast timekeeper -> memcpy 发布正式 timekeeper
 *         -> write_seqcount_end
 *   读侧：read_seqcount_begin -> 复制基点/偏移并读取 clocksource
 *         -> read_seqcount_retry；若写侧穿插则整次重读
 *
 * 【贯穿例子】
 * 假设 clocksource 为 1 GHz，cycle_last=1000，本次读到 1300；mult/shift 把 300 cycles
 * 换成 300 ns，再加到已提交的 monotonic base。若用户随后把墙上时间向前拨 10 秒，
 * offs_real 增加 10 秒，因此 realtime 跳变；wall_to_monotonic/offs_real 同步反向调整，
 * monotonic 仍连续。系统 suspend 5 秒后恢复，会把 5 秒补进 realtime，同时反向调整
 * wall_to_monotonic 让 monotonic 不跳，并把同一 delta 加入 offs_boot，使 boottime
 * 包含睡眠时间。
 *
 * 【核心矛盾与方案代价】
 * 读时间是极热路径，不能每次拿全局锁；Linux 用 seqcount 提供一致快照，用 latch
 * 双缓冲满足 NMI/tracing，用 VDSO 把常用读取搬到用户态。代价是写侧必须同时维护
 * 多份派生状态并严格排序，而且 fast 接口为 NMI 安全接受极小的跨更新乱序风险。
 *
 * 建议阅读顺序：tk_setup_internals/timekeeping_cycles_to_ns -> 普通读取 API ->
 * timekeeping_update_from_shadow -> update_wall_time -> settimeofday/clocksource switch ->
 * suspend/resume -> adjtimex；辅助时钟属于最后的扩展分支。
 */
#include <linux/audit.h>
#include <linux/clocksource.h>
#include <linux/compiler.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/nmi.h>
#include <linux/pvclock_gtod.h>
#include <linux/random.h>
#include <linux/sched/clock.h>
#include <linux/sched/loadavg.h>
#include <linux/static_key.h>
#include <linux/stop_machine.h>
#include <linux/syscore_ops.h>
#include <linux/tick.h>
#include <linux/time.h>
#include <linux/timex.h>
#include <linux/timekeeper_internal.h>

#include <vdso/auxclock.h>

#include "tick-internal.h"
#include "timekeeping_internal.h"
#include "ntp_internal.h"

#define TK_CLEAR_NTP		(1 << 0)
#define TK_CLOCK_WAS_SET	(1 << 1)

#define TK_UPDATE_ALL		(TK_CLEAR_NTP | TK_CLOCK_WAS_SET)

enum timekeeping_adv_mode {
	/* Update timekeeper when a tick has passed */
	TK_ADV_TICK,

	/* Update timekeeper on a direct frequency change */
	TK_ADV_FREQ
};

/*
 * 补充说明：TK_ADV_TICK 允许“不足一个固定 interval”直接返回；TK_ADV_FREQ 用于
 * adjtimex 立即应用新频率，即使本次没有完整 tick 可累计也不能跳过斜率更新。
 */

/*
 * The most important data for readout fits into a single 64 byte
 * cache line.
 */
struct tk_data {
	seqcount_raw_spinlock_t	seq;
	struct timekeeper	timekeeper;
	struct timekeeper	shadow_timekeeper;
	raw_spinlock_t		lock;
} ____cacheline_aligned;

/*
 * 补充说明：每个 tk_data 有一份读者可见的 timekeeper 和一份只供写侧演算的 shadow。
 * raw spinlock 串行化 tick、settimeofday、clocksource 切换和 suspend；seqcount 不负责
 * 排斥写者，只让无锁读者发现写入穿插。seqcount_raw_spinlock_t 还把这把锁关联给
 * lockdep，帮助验证写序列确实处于正确锁保护下。
 *
 * shadow 的意义类似事务工作区：先在不可见副本完成周期推进、NTP/闰秒和派生基点，
 * 再在一个 seqcount 写区间内更新 VDSO/fast 副本并 memcpy 发布。这样读者不会看到
 * “秒已增加但 offset/VDSO 仍是旧值”的混合状态。
 *
 * 字段地图：
 *   seq               读者版本号；变化表示写事务穿插，并关联 lock 做 lockdep 验证。
 *   timekeeper        已提交、供无锁读者观察的正式状态。
 *   shadow_timekeeper 写者持 lock 时演算的候选状态，提交前对读者不可见。
 *   lock              该 tk_data 唯一写者锁；IRQ 也会推进时间，写侧通常 irqsave。
 */

/*
 * timekeeper_data 是所有软件 timekeeper 的固定槽位数组：索引 0 为系统 core，其余在
 * CONFIG_POSIX_AUX_CLOCKS 下作为 AUX。静态存储期保证对象永不释放，读者无需引用计数；
 * 每个槽自己的 lock/seq 管理内容并发。
 */
static struct tk_data timekeeper_data[TIMEKEEPERS_MAX];

/* The core timekeeper */
#define tk_core		(timekeeper_data[TIMEKEEPER_CORE])
/* tk_core 是系统主时钟槽位的左值别名，不创建对象，也不增加指针间接访问。 */

#ifdef CONFIG_POSIX_AUX_CLOCKS
/*
 * tk_get_aux_ts64 - 用内部 timekeeper ID 读取对应的 POSIX AUX 时钟。
 *
 * AUX（auxiliary，辅助时钟）不是 CLOCK_MONOTONIC 的别名，而是可选的独立软件
 * timekeeper：它复用 core raw clocksource 的硬件周期，却拥有自己的 offset、频率
 * 校正和有效状态，可供需要与外部设备建立独立时间域的用户空间使用。
 *
 * timekeeper 内部编号从 TIMEKEEPER_AUX_FIRST 开始，POSIX ABI 编号从 CLOCK_AUX
 * 开始；两者做相同距离的平移。返回 false 表示对应 AUX 尚未启用或读取时失效，
 * @ts 不能当作有效输出。
 */
static inline bool tk_get_aux_ts64(unsigned int tkid, struct timespec64 *ts)
{
	return ktime_get_aux_ts64(CLOCK_AUX + tkid - TIMEKEEPER_AUX_FIRST, ts);
}

/*
 * tk_is_aux - 判断一个 timekeeper 是否属于辅助时钟槽位。
 *
 * @tk->id 是 tkd_basic_setup() 在启动时写入的稳定内部身份，不是用户传入的 clockid。
 * TIMEKEEPER_CORE 位于 AUX 范围之外；TIMEKEEPER_AUX_FIRST..LAST 是闭区间，所以两端
 * 都使用包含比较。当前主要调用者 timekeeping_update_from_shadow() 据此选择发布目标：
 * core 更新通用 VDSO、pvclock 和 NMI fast timekeeper；AUX 只更新自己的 VDSO 数据。
 * 若误把 core 判成 AUX，会漏掉全局时间发布；若误把 AUX 判成 core，则会污染系统钟。
 */
static inline bool tk_is_aux(const struct timekeeper *tk)
{
	return tk->id >= TIMEKEEPER_AUX_FIRST && tk->id <= TIMEKEEPER_AUX_LAST;
}
/* 真正定义位于文件末尾；前向声明让前面的通用读取/cross-timestamp 路径可以分派。 */
static inline struct tk_data *aux_get_tk_data(clockid_t id);
#else
/* 配置关闭时这些 inline stub 会被编译消除，使 core 热路径无需散布条件编译。 */
/* 没有 AUX ABI 时任何内部 ID 都不可读取为辅助时钟，输出保持不可用。 */
static inline bool tk_get_aux_ts64(unsigned int tkid, struct timespec64 *ts)
{
	/* @tkid/@ts 在本配置中均不使用；false 明确要求调用者不得读取 @ts 原有内容。 */
	return false;
}

/*
 * CONFIG_POSIX_AUX_CLOCKS=n 时系统只有 core timekeeper，因此恒 false 正是配置语义。
 * 这使 timekeeping_update_from_shadow() 不可能进入 vdso_time_update_aux() 分支，编译器
 * 还能把整个 AUX 发布路径消除；它不是“尚未判断”，也不是运行时禁用状态。
 */
static inline bool tk_is_aux(const struct timekeeper *tk)
{
	/* @tk 只是为保持两种配置签名一致而保留的借用参数，本配置不解引用它。 */
	return false;
}
/* 无 AUX 槽位时所有 POSIX AUX clockid 都映射失败。 */
static inline struct tk_data *aux_get_tk_data(clockid_t id)
{
	/* @id 在无 AUX 配置下没有合法取值；NULL 表示不存在可借用的 tk_data 槽。 */
	return NULL;
}
#endif

static inline void tk_update_aux_offs(struct timekeeper *tk, ktime_t offs)
{
	/*
	 * @tk 是持锁写侧对象；@offs 是 AUX 相对其 raw-derived monotonic 基点的有符号纳秒
	 * 偏移。函数同步更新标量与 timespec 派生表示，后者供 VDSO 避免运行时 64 位除法。
	 */
	tk->offs_aux = offs;
	tk->monotonic_to_aux = ktime_to_timespec64(offs);
}

/* flag for if timekeeping is suspended */
int __read_mostly timekeeping_suspended;

/*
 * 补充说明：timekeeping_suspended 是挂起协议的全局状态，不是保护 timekeeper 的锁。
 * 它主要阻止不安全的 clocksource 读取并让 dummy clock 返回固定周期；状态切换仍在
 * timekeeper 锁和 syscore suspend/resume 排序下完成。
 */

/**
 * struct tk_fast - NMI safe timekeeper
 * @seq:	Sequence counter for protecting updates. The lowest bit
 *		is the index for the tk_read_base array
 * @base:	tk_read_base array. Access is indexed by the lowest bit of
 *		@seq.
 *
 * See @update_fast_timekeeper() below.
 */
struct tk_fast {
	seqcount_latch_t	seq;
	struct tk_read_base	base[2];
};

/*
 * 字段补充：seq 的最低位选择当前安全副本，高位用于发现更新；base[0]/base[1] 保存
 * 同一 readout base 的双缓冲。写者分阶段更新两份，NMI 读者总能选择完整的一份。
 */

/* Suspend-time cycles value for halted fast timekeeper. */
static u64 cycles_at_suspend;
/* cycles_at_suspend 保存 suspend 提交点的绝对 clocksource 周期，dummy read 恒返回它。 */

static u64 dummy_clock_read(struct clocksource *cs)
{
	/*
	 * @cs 是为满足 clocksource.read() 统一函数指针签名而传入的借用对象，本实现不读取
	 * 其字段。suspend 后真实 clocksource 可能停摆或不可访问，fast reader 必须只看到
	 * cycles_at_suspend；启动早期未 suspend 时以 local_clock() 纳秒值充当 1:1 周期。
	 */
	if (timekeeping_suspended)
		return cycles_at_suspend;
	return local_clock();
}

static struct clocksource dummy_clock = {
	.read = dummy_clock_read,
};
/* dummy_clock 只有 read 操作，用作早期 local_clock 适配和 suspend 期间冻结 fast reader。 */

/*
 * Boot time initialization which allows local_clock() to be utilized
 * during early boot when clocksources are not available. local_clock()
 * returns nanoseconds already so no conversion is required, hence mult=1
 * and shift=0. When the first proper clocksource is installed then
 * the fast time keepers are updated with the correct values.
 */
#define FAST_TK_INIT						\
	{							\
		.clock		= &dummy_clock,			\
		.mask		= CLOCKSOURCE_MASK(64),		\
		.mult		= 1,				\
		.shift		= 0,				\
	}

static struct tk_fast tk_fast_mono ____cacheline_aligned = {
	.seq     = SEQCNT_LATCH_ZERO(tk_fast_mono.seq),
	.base[0] = FAST_TK_INIT,
	.base[1] = FAST_TK_INIT,
};
/* tk_fast_mono 是 NMI-safe monotonic/realtime 的双缓冲发布对象，独占 cacheline。 */

static struct tk_fast tk_fast_raw  ____cacheline_aligned = {
	.seq     = SEQCNT_LATCH_ZERO(tk_fast_raw.seq),
	.base[0] = FAST_TK_INIT,
	.base[1] = FAST_TK_INIT,
};
/* tk_fast_raw 保存未经 NTP 调斜率的 raw 双缓冲，与 mono 分开避免 cache 相互污染。 */

#ifdef CONFIG_POSIX_AUX_CLOCKS
static __init void tk_aux_setup(void);
static void tk_aux_update_clocksource(void);
static void tk_aux_advance(void);
#else
/* aux 未编译时 setup/switch/tick 三个调用点都退化为空操作。 */
static inline void tk_aux_setup(void) { }
static inline void tk_aux_update_clocksource(void) { }
static inline void tk_aux_advance(void) { }
#endif

unsigned long timekeeper_lock_irqsave(void)
{
	/*
	 * 对外提供 core 写锁封装。irqsave 防止本 CPU 的 timer/IRQ 在持锁区重入同一写侧；
	 * 返回 flags 把“恢复到调用前 IRQ 状态”的责任交给 unlock 调用者。
	 */
	unsigned long flags;

	raw_spin_lock_irqsave(&tk_core.lock, flags);
	return flags;
}

void timekeeper_unlock_irqrestore(unsigned long flags)
{
	/* @flags 必须来自配对 lock 调用；它不是任意布尔值。 */
	raw_spin_unlock_irqrestore(&tk_core.lock, flags);
}

/*
 * Multigrain timestamps require tracking the latest fine-grained timestamp
 * that has been issued, and never returning a coarse-grained timestamp that is
 * earlier than that value.
 *
 * mg_floor represents the latest fine-grained time that has been handed out as
 * a file timestamp on the system. This is tracked as a monotonic ktime_t, and
 * converted to a realtime clock value on an as-needed basis.
 *
 * Maintaining mg_floor ensures the multigrain interfaces never issue a
 * timestamp earlier than one that has been previously issued.
 *
 * The exception to this rule is when there is a backward realtime clock jump. If
 * such an event occurs, a timestamp can appear to be earlier than a previous one.
 */
static __cacheline_aligned_in_smp atomic64_t mg_floor;
/* atomic64 允许并发文件时间发布者无锁抬高 floor；cacheline 隔离减少跨 CPU 伪共享。 */

static inline void tk_normalize_xtime(struct timekeeper *tk)
{
	/*
	 * @tk 是写侧持锁操作的 timekeeper；函数原地修改 mono/raw 的整秒和定点小数基点，
	 * 无返回值。xtime_nsec 使用“左移 shift 的纳秒”保存小数精度，跨秒后分别搬运到
	 * xtime_sec/raw_sec，从而恢复两个小数字段均小于一秒的不变量。
	 */
	while (tk->tkr_mono.xtime_nsec >= ((u64)NSEC_PER_SEC << tk->tkr_mono.shift)) {
		tk->tkr_mono.xtime_nsec -= (u64)NSEC_PER_SEC << tk->tkr_mono.shift;
		tk->xtime_sec++;
	}
	while (tk->tkr_raw.xtime_nsec >= ((u64)NSEC_PER_SEC << tk->tkr_raw.shift)) {
		tk->tkr_raw.xtime_nsec -= (u64)NSEC_PER_SEC << tk->tkr_raw.shift;
		tk->raw_sec++;
	}
}

static inline struct timespec64 tk_xtime(const struct timekeeper *tk)
{
	/* 这里只读取已累计基点，不额外读取 clocksource；调用者决定是否需要 fine 增量。 */
	/* @tk 是借用的稳定快照；ts 是按普通纳秒单位组装的返回值。 */
	struct timespec64 ts;

	ts.tv_sec = tk->xtime_sec;
	ts.tv_nsec = (long)(tk->tkr_mono.xtime_nsec >> tk->tkr_mono.shift);
	return ts;
}

static inline struct timespec64 tk_xtime_coarse(const struct timekeeper *tk)
{
	/* coarse_nsec 被维护为不后退的独立快照，读取快但精度只到最近一次更新。 */
	/* @tk 由调用者负责同步；ts 临时承载 coarse realtime 的秒/纳秒。 */
	struct timespec64 ts;

	ts.tv_sec = tk->xtime_sec;
	ts.tv_nsec = tk->coarse_nsec;
	return ts;
}

/*
 * Update the nanoseconds part for the coarse time keepers. They can't rely
 * on xtime_nsec because xtime_nsec could be adjusted by a small negative
 * amount when the multiplication factor of the clock is adjusted, which
 * could cause the coarse clocks to go slightly backwards. See
 * timekeeping_apply_adjustment(). Thus we keep a separate copy for the coarse
 * clockids which only is updated when the clock has been set or  we have
 * accumulated time.
 */
static inline void tk_update_coarse_nsecs(struct timekeeper *tk)
{
	/* @tk 是写侧对象；把 mono 定点小数截成普通纳秒写入 coarse_nsec，无返回值。 */
	tk->coarse_nsec = tk->tkr_mono.xtime_nsec >> tk->tkr_mono.shift;
}

static void tk_set_xtime(struct timekeeper *tk, const struct timespec64 *ts)
{
	/*
	 * @tk 是待原地更新的写侧对象，@ts 是借用的规范化 realtime 绝对值；函数不保留
	 * @ts。输入子秒是普通纳秒，写入内部定点格式时左移当前 clocksource 的 shift，
	 * 并同步 coarse 副本；调用者负责锁和随后发布，无显式返回值。
	 */
	tk->xtime_sec = ts->tv_sec;
	tk->tkr_mono.xtime_nsec = (u64)ts->tv_nsec << tk->tkr_mono.shift;
	tk_update_coarse_nsecs(tk);
}

static void tk_xtime_add(struct timekeeper *tk, const struct timespec64 *ts)
{
	/*
	 * @tk 是持锁写侧对象，@ts 是借用的增量而非绝对时间。函数把秒/纳秒增量加入
	 * realtime 基点，随后规范化进位并同步 coarse_nsec；它只修改 @tk，不负责发布。
	 */
	tk->xtime_sec += ts->tv_sec;
	tk->tkr_mono.xtime_nsec += (u64)ts->tv_nsec << tk->tkr_mono.shift;
	tk_normalize_xtime(tk);
	tk_update_coarse_nsecs(tk);
}

static void tk_set_wall_to_mono(struct timekeeper *tk, struct timespec64 wtm)
{
	/* @tk 是待更新对象；@wtm 是新 wall_to_monotonic；tmp 用于构造其规范化相反数。 */
	struct timespec64 tmp;

	/*
	 * Verify consistency of: offset_real = -wall_to_monotonic
	 * before modifying anything
	 */
	set_normalized_timespec64(&tmp, -tk->wall_to_monotonic.tv_sec,
					-tk->wall_to_monotonic.tv_nsec);
	WARN_ON_ONCE(tk->offs_real != timespec64_to_ktime(tmp));
	tk->wall_to_monotonic = wtm;
	set_normalized_timespec64(&tmp, -wtm.tv_sec, -wtm.tv_nsec);
	/* Paired with READ_ONCE() in ktime_mono_to_any() */
	/* 补充说明：offs_real = -wall_to_monotonic；二者必须作为同一逻辑更新维护。 */
	WRITE_ONCE(tk->offs_real, timespec64_to_ktime(tmp));
	WRITE_ONCE(tk->offs_tai, ktime_add(tk->offs_real, ktime_set(tk->tai_offset, 0)));
}

static inline void tk_update_sleep_time(struct timekeeper *tk, ktime_t delta)
{
	/* @tk 是持锁写侧对象；@delta 是本次 suspend 睡眠的纳秒时长，累加到 offs_boot。 */
	/* Paired with READ_ONCE() in ktime_mono_to_any() */
	/* 补充说明：只增加 boot offset，使 suspend 时间出现在 boottime 而不污染 monotonic。 */
	WRITE_ONCE(tk->offs_boot, ktime_add(tk->offs_boot, delta));
	/*
	 * Timespec representation for VDSO update to avoid 64bit division
	 * on every update.
	 */
	tk->monotonic_to_boot = ktime_to_timespec64(tk->offs_boot);
}

#ifdef CONFIG_ARCH_WANTS_CLOCKSOURCE_READ_INLINE
#include <asm/clock_inlined.h>

static DEFINE_STATIC_KEY_FALSE(clocksource_read_inlined);
/* 该 static key 是全局热路径开关：启用时架构可内联读当前源，换源期间由写侧切换。 */

/*
 * tk_clock_read - atomic clocksource read() helper
 *
 * This helper is necessary to use in the read paths because, while the
 * seqcount ensures we don't return a bad value while structures are updated,
 * it doesn't protect from potential crashes. There is the possibility that
 * the tkr's clocksource may change between the read reference, and the
 * clock reference passed to the read function.  This can cause crashes if
 * the wrong clocksource is passed to the wrong read function.
 * This isn't necessary to use when holding the tk_core.lock or doing
 * a read of the fast-timekeeper tkrs (which is protected by its own locking
 * and update logic).
 */
static __always_inline u64 tk_clock_read(const struct tk_read_base *tkr)
{
	/*
	 * READ_ONCE 只保证先取得一个完整 clock 指针。必须把同一快照同时用于选择 read
	 * 实现和传参；否则 clocksource 切换夹在两次读取之间，可能把 A 的回调传入 B。
	 * static key 让“不支持架构内联读取”的常态不承担普通条件分支成本。
	 */
	/* @tkr 是借用 read base；clock 是本次调用固定使用的 clocksource 指针快照。 */
	struct clocksource *clock = READ_ONCE(tkr->clock);

	if (static_branch_likely(&clocksource_read_inlined))
		return arch_inlined_clocksource_read(clock);

	return clock->read(clock);
}

static inline void clocksource_disable_inline_read(void)
{
	/* 关闭全局 static key，使后续热读改走 clock->read；无参数、无返回和所有权变化。 */
	static_branch_disable(&clocksource_read_inlined);
}

static inline void clocksource_enable_inline_read(void)
{
	/* 打开全局 static key，使支持架构走内联硬件读；调用者须确保当前源适合该实现。 */
	static_branch_enable(&clocksource_read_inlined);
}
#else
static __always_inline u64 tk_clock_read(const struct tk_read_base *tkr)
{
	/* 非内联配置仍先稳定函数表 owner，再通过该对象自己的 read 回调读取。 */
	/* @tkr 是借用 read base；clock 固定本次回调与参数属于同一 clocksource。 */
	struct clocksource *clock = READ_ONCE(tkr->clock);

	return clock->read(clock);
}

static inline void clocksource_disable_inline_read(void) { }
static inline void clocksource_enable_inline_read(void) { }
#endif

/**
 * tk_setup_internals - Set up internals to use clocksource clock.
 *
 * @tk:		The target timekeeper to setup.
 * @clock:		Pointer to clocksource.
 *
 * Calculates a fixed cycle/nsec interval for a given clocksource/adjustment
 * pair and interval request.
 *
 * Unless you're the timekeeping code, you should not be using this!
 */
static void tk_setup_internals(struct timekeeper *tk, struct clocksource *clock)
{
	/*
	 * 补充说明：调用者持有对应 tk_data 写锁，并已把旧时间推进到切换瞬间。本函数
	 * 重新建立“周期 <-> 定点纳秒”参数，但不自行向读者发布。
	 *
	 * 变量关系：clock->mult/shift 给出 ns ~= cycles*mult>>shift；cycle_interval 是一个
	 * NTP 更新周期对应的硬件 cycles；xtime_interval 是用当前 mult 换回的 shifted-ns；
	 * xtime_remainder 保存取整误差，后续 timekeeping_adjust 逐步偿还而不是丢失。
	 */
	/*
	 * 变量地图：
	 *   tk          调用者持锁的 shadow timekeeper，修改在随后提交前不可见。
	 *   clock       新 clocksource；函数借用，生命周期由切换路径的 module 引用保证。
	 *   old_clock   旧源快照，仅用于把 xtime_nsec 的定点 shift 无损换到新尺度。
	 *   tmp         ns/cycle 反算的可变工作值。
	 *   ntpinterval 一个 NTP 周期的 shifted-ns 精确预算。
	 *   interval    取整后一个 NTP 周期对应的硬件 cycles，最小为 1。
	 */
	u64 interval;
	u64 tmp, ntpinterval;
	struct clocksource *old_clock;

	++tk->cs_was_changed_seq;
	old_clock = tk->tkr_mono.clock;
	tk->tkr_mono.clock = clock;
	tk->tkr_mono.mask = clock->mask;
	tk->tkr_mono.cycle_last = tk_clock_read(&tk->tkr_mono);

	tk->tkr_raw.clock = clock;
	tk->tkr_raw.mask = clock->mask;
	tk->tkr_raw.cycle_last = tk->tkr_mono.cycle_last;

	/* Do the ns -> cycle conversion first, using original mult */
	/* 补充说明：加 mult/2 实现四舍五入；极低频时至少取 1 cycle，保证推进循环有进展。 */
	tmp = NTP_INTERVAL_LENGTH;
	tmp <<= clock->shift;
	ntpinterval = tmp;
	tmp += clock->mult/2;
	do_div(tmp, clock->mult);
	if (tmp == 0)
		tmp = 1;

	interval = (u64) tmp;
	tk->cycle_interval = interval;

	/* Go back from cycles -> shifted ns */
	tk->xtime_interval = interval * clock->mult;
	tk->xtime_remainder = ntpinterval - tk->xtime_interval;
	tk->raw_interval = interval * clock->mult;

	 /* if changing clocks, convert xtime_nsec shift units */
	/* 补充说明：xtime_nsec 是定点数；换 clocksource shift 时只变表示尺度，不改变时间值。 */
	if (old_clock) {
		int shift_change = clock->shift - old_clock->shift;
		if (shift_change < 0) {
			tk->tkr_mono.xtime_nsec >>= -shift_change;
			tk->tkr_raw.xtime_nsec >>= -shift_change;
		} else {
			tk->tkr_mono.xtime_nsec <<= shift_change;
			tk->tkr_raw.xtime_nsec <<= shift_change;
		}
	}

	tk->tkr_mono.shift = clock->shift;
	tk->tkr_raw.shift = clock->shift;

	tk->ntp_error = 0;
	tk->ntp_error_shift = NTP_SCALE_SHIFT - clock->shift;
	tk->ntp_tick = ntpinterval << tk->ntp_error_shift;

	/*
	 * The timekeeper keeps its own mult values for the currently
	 * active clocksource. These value will be adjusted via NTP
	 * to counteract clock drifting.
	 */
	tk->tkr_mono.mult = clock->mult;
	tk->tkr_raw.mult = clock->mult;
	tk->ntp_err_mult = 0;
	tk->skip_second_overflow = 0;

	tk->cs_id = clock->id;

	/* Coupled clockevent data */
	if (IS_ENABLED(CONFIG_GENERIC_CLOCKEVENTS_COUPLED) &&
	    clock->flags & CLOCK_SOURCE_HAS_COUPLED_CLOCK_EVENT) {
		/*
		 * Aim for an one hour maximum delta and use KHz to handle
		 * clocksources with a frequency above 4GHz correctly as
		 * the frequency argument of clocks_calc_mult_shift() is u32.
		 */
		clocks_calc_mult_shift(&tk->cs_ns_to_cyc_mult, &tk->cs_ns_to_cyc_shift,
				       NSEC_PER_MSEC, clock->freq_khz, 3600 * 1000);
		/*
		 * Initialize the conversion limit as the previous clocksource
		 * might have the same shift/mult pair so the quick check in
		 * tk_update_ns_to_cyc() fails to update it after a clocksource
		 * change leaving it effectivly zero.
		 */
		tk->cs_ns_to_cyc_maxns = div_u64(clock->mask, tk->cs_ns_to_cyc_mult);
	}
}

/* Timekeeper helper functions. */
static noinline u64 delta_to_ns_safe(const struct tk_read_base *tkr, u64 delta)
{
	/* 拆分乘加避免 delta*mult 在 u64 中间值溢出；仅异常大 delta 走慢路径。 */
	return mul_u64_u32_add_u64_shr(delta, tkr->mult, tkr->xtime_nsec, tkr->shift);
}

static __always_inline u64 timekeeping_cycles_to_ns(const struct tk_read_base *tkr, u64 cycles)
{
	/* Calculate the delta since the last update_wall_time() */
	/* @cycles 是绝对硬件读数；mask 定义回绕宽度；delta 是相对 cycle_last 的周期数。 */
	u64 mask = tkr->mask, delta = (cycles - tkr->cycle_last) & mask;

	/*
	 * This detects both negative motion and the case where the delta
	 * overflows the multiplication with tkr->mult.
	 */
	if (unlikely(delta > tkr->clock->max_cycles)) {
		/*
		 * Handle clocksource inconsistency between CPUs to prevent
		 * time from going backwards by checking for the MSB of the
		 * mask being set in the delta.
		 */
		/*
		 * 环形计数器的差值若落在 mask 的“后半圈”，更可能是另一 CPU 读到了稍旧周期，
		 * 而不是真经过了接近完整一圈；返回已累计基点可避免时间倒退/巨大跃迁。
		 */
		if (delta & ~(mask >> 1))
			return tkr->xtime_nsec >> tkr->shift;

		return delta_to_ns_safe(tkr, delta);
	}

	return ((delta * tkr->mult) + tkr->xtime_nsec) >> tkr->shift;
}

static __always_inline u64 timekeeping_get_ns(const struct tk_read_base *tkr)
{
	return timekeeping_cycles_to_ns(tkr, tk_clock_read(tkr));
}

/**
 * update_fast_timekeeper - Update the fast and NMI safe monotonic timekeeper.
 * @tkr: Timekeeping readout base from which we take the update
 * @tkf: Pointer to NMI safe timekeeper
 *
 * We want to use this from any context including NMI and tracing /
 * instrumenting the timekeeping code itself.
 *
 * Employ the latch technique; see @write_seqcount_latch.
 *
 * So if a NMI hits the update of base[0] then it will use base[1]
 * which is still consistent. In the worst case this can result is a
 * slightly wrong timestamp (a few nanoseconds). See
 * @ktime_get_mono_fast_ns.
 */
static void update_fast_timekeeper(const struct tk_read_base *tkr,
				   struct tk_fast *tkf)
{
	/*
	 * 补充说明：latch 的低位选择读副本。写者先把读者赶到 base[1] 再更新 base[0]，
	 * 然后赶回 base[0] 再更新 base[1]；任意 NMI 打断写者时至少有一份完整副本。
	 * memcpy 复制 clock 指针、cycle_last、mult/shift 和 base，缺一项都会混合两个时代。
	 */
	/* @tkr 是待发布源快照；@tkf 是目标 latch；base 指向其两个连续副本的首元素。 */
	struct tk_read_base *base = tkf->base;

	/* Force readers off to base[1] */
	write_seqcount_latch_begin(&tkf->seq);

	/* Update base[0] */
	memcpy(base, tkr, sizeof(*base));

	/* Force readers back to base[0] */
	write_seqcount_latch(&tkf->seq);

	/* Update base[1] */
	memcpy(base + 1, base, sizeof(*base));

	write_seqcount_latch_end(&tkf->seq);
}

static __always_inline u64 __ktime_get_fast_ns(struct tk_fast *tkf)
{
	/*
	 * seq&1 是数组索引而非“写入中”判断。若更新期间 latch 序号改变，retry 丢弃整个
	 * base+delta 结果。这里不取 tk_core.lock，因此能在 NMI/tracing 中使用。
	 */
	/* tkr 指向 seq 低位选中的副本；seq 是验证 token；now 累计 base 与增量纳秒。 */
	struct tk_read_base *tkr;
	unsigned int seq;
	u64 now;

	do {
		seq = read_seqcount_latch(&tkf->seq);
		tkr = tkf->base + (seq & 0x01);
		now = ktime_to_ns(tkr->base);
		now += timekeeping_get_ns(tkr);
	} while (read_seqcount_latch_retry(&tkf->seq, seq));

	return now;
}

/**
 * ktime_get_mono_fast_ns - Fast NMI safe access to clock monotonic
 *
 * This timestamp is not guaranteed to be monotonic across an update.
 * The timestamp is calculated by:
 *
 *	now = base_mono + clock_delta * slope
 *
 * So if the update lowers the slope, readers who are forced to the
 * not yet updated second array are still using the old steeper slope.
 *
 * tmono
 * ^
 * |    o  n
 * |   o n
 * |  u
 * | o
 * |o
 * |12345678---> reader order
 *
 * o = old slope
 * u = update
 * n = new slope
 *
 * So reader 6 will observe time going backwards versus reader 5.
 *
 * While other CPUs are likely to be able to observe that, the only way
 * for a CPU local observation is when an NMI hits in the middle of
 * the update. Timestamps taken from that NMI context might be ahead
 * of the following timestamps. Callers need to be aware of that and
 * deal with it.
 */
u64 notrace ktime_get_mono_fast_ns(void)
{
	/*
	 * 补充说明：fast 家族服务 NMI、trace 和递归观测，不能依赖普通 seqcount 写者最终
	 * 跑完，所以读 latch 双副本。mono 可能在调斜率更新边界出现极小倒退；raw 因斜率
	 * 固定没有该问题；boot/TAI 额外 data_race 读取 offset，可能短暂混合两个版本；
	 * real 把 base_real 与 delta 放在同一 latch 重试中。普通业务应优先使用严格 API。
	 */
	return __ktime_get_fast_ns(&tk_fast_mono);
}
EXPORT_SYMBOL_GPL(ktime_get_mono_fast_ns);

/**
 * ktime_get_raw_fast_ns - Fast NMI safe access to clock monotonic raw
 *
 * Contrary to ktime_get_mono_fast_ns() this is always correct because the
 * conversion factor is not affected by NTP/PTP correction.
 */
u64 notrace ktime_get_raw_fast_ns(void)
{
	return __ktime_get_fast_ns(&tk_fast_raw);
}
EXPORT_SYMBOL_GPL(ktime_get_raw_fast_ns);

/**
 * ktime_get_boot_fast_ns - NMI safe and fast access to boot clock.
 *
 * To keep it NMI safe since we're accessing from tracing, we're not using a
 * separate timekeeper with updates to monotonic clock and boot offset
 * protected with seqcounts. This has the following minor side effects:
 *
 * (1) Its possible that a timestamp be taken after the boot offset is updated
 * but before the timekeeper is updated. If this happens, the new boot offset
 * is added to the old timekeeping making the clock appear to update slightly
 * earlier:
 *    CPU 0                                        CPU 1
 *    timekeeping_inject_sleeptime64()
 *    __timekeeping_inject_sleeptime(tk, delta);
 *                                                 timestamp();
 *    timekeeping_update_staged(tkd, TK_CLEAR_NTP...);
 *
 * (2) On 32-bit systems, the 64-bit boot offset (tk->offs_boot) may be
 * partially updated.  Since the tk->offs_boot update is a rare event, this
 * should be a rare occurrence which postprocessing should be able to handle.
 *
 * The caveats vs. timestamp ordering as documented for ktime_get_mono_fast_ns()
 * apply as well.
 */
u64 notrace ktime_get_boot_fast_ns(void)
{
	/* mono 与 offs_boot 不是一个原子快照；data_race 明确接受更新边界的短暂混合。 */
	struct timekeeper *tk = &tk_core.timekeeper;

	return (ktime_get_mono_fast_ns() + ktime_to_ns(data_race(tk->offs_boot)));
}
EXPORT_SYMBOL_GPL(ktime_get_boot_fast_ns);

/**
 * ktime_get_tai_fast_ns - NMI safe and fast access to tai clock.
 *
 * The same limitations as described for ktime_get_boot_fast_ns() apply. The
 * mono time and the TAI offset are not read atomically which may yield wrong
 * readouts. However, an update of the TAI offset is an rare event e.g., caused
 * by settime or adjtimex with an offset. The user of this function has to deal
 * with the possibility of wrong timestamps in post processing.
 */
u64 notrace ktime_get_tai_fast_ns(void)
{
	/* TAI offset 极少变化，NMI 路径选择可后处理的偶发误差而不是不可用的全局锁。 */
	struct timekeeper *tk = &tk_core.timekeeper;

	return (ktime_get_mono_fast_ns() + ktime_to_ns(data_race(tk->offs_tai)));
}
EXPORT_SYMBOL_GPL(ktime_get_tai_fast_ns);

/**
 * ktime_get_real_fast_ns: - NMI safe and fast access to clock realtime.
 *
 * See ktime_get_mono_fast_ns() for documentation of the time stamp ordering.
 */
u64 ktime_get_real_fast_ns(void)
{
	/* 与 boot/TAI 不同，base_real 已嵌入 latch 副本，可与本次 delta 一起重试。 */
	/* tkf 是全局 mono latch；tkr 为选中副本；baser/delta 分别是 realtime 基点/增量。 */
	struct tk_fast *tkf = &tk_fast_mono;
	struct tk_read_base *tkr;
	u64 baser, delta;
	unsigned int seq;

	do {
		seq = raw_read_seqcount_latch(&tkf->seq);
		tkr = tkf->base + (seq & 0x01);
		baser = ktime_to_ns(tkr->base_real);
		delta = timekeeping_get_ns(tkr);
	} while (raw_read_seqcount_latch_retry(&tkf->seq, seq));

	return baser + delta;
}
EXPORT_SYMBOL_GPL(ktime_get_real_fast_ns);

/**
 * halt_fast_timekeeper - Prevent fast timekeeper from accessing clocksource.
 * @tk: Timekeeper to snapshot.
 *
 * It generally is unsafe to access the clocksource after timekeeping has been
 * suspended, so take a snapshot of the readout base of @tk and use it as the
 * fast timekeeper's readout base while suspended.  It will return the same
 * number of cycles every time until timekeeping is resumed at which time the
 * proper readout base for the fast timekeeper will be restored automatically.
 */
static void halt_fast_timekeeper(const struct timekeeper *tk)
{
	/*
	 * 把最后真实周期连同原 conversion/base 复制到静态 dummy base，再把 read 回调换成
	 * 固定 cycles_at_suspend。这样 fast/NMI reader 无需知道设备已 suspend，也不会调用
	 * 可能掉电的寄存器。resume 的正常 update 会重新发布真实 clocksource。
	 */
	/* tkr_dummy 是 suspend 期间长期存在的冻结副本；tkr 依次借用 mono 和 raw 源。 */
	static struct tk_read_base tkr_dummy;
	const struct tk_read_base *tkr = &tk->tkr_mono;

	memcpy(&tkr_dummy, tkr, sizeof(tkr_dummy));
	cycles_at_suspend = tk_clock_read(tkr);
	tkr_dummy.clock = &dummy_clock;
	tkr_dummy.base_real = tkr->base + tk->offs_real;
	update_fast_timekeeper(&tkr_dummy, &tk_fast_mono);

	tkr = &tk->tkr_raw;
	memcpy(&tkr_dummy, tkr, sizeof(tkr_dummy));
	tkr_dummy.clock = &dummy_clock;
	update_fast_timekeeper(&tkr_dummy, &tk_fast_raw);
}

static RAW_NOTIFIER_HEAD(pvclock_gtod_chain);
/* 链中元素由虚拟化时钟注册，通知参数是最新 core timekeeper 与“是否设时”标志。 */

static void update_pvclock_gtod(struct timekeeper *tk, bool was_set)
{
	/*
	 * @tk 是锁内借用的最新 core 状态，通知仅在回调期间有效；@was_set 区分墙钟跳变或
	 * 首次全量同步与普通推进。返回值被忽略，因为监听者不能否决已经形成的时间状态。
	 */
	raw_notifier_call_chain(&pvclock_gtod_chain, was_set, tk);
}

/**
 * pvclock_gtod_register_notifier - register a pvclock timedata update listener
 * @nb: Pointer to the notifier block to register
 */
int pvclock_gtod_register_notifier(struct notifier_block *nb)
{
	/* tk 是要立即推送给新监听者的 core 快照；ret 保存链表注册结果。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	int ret;

	/* guard 离开作用域自动 irqrestore/unlock；注册与首次快照处于同一锁区，避免漏更新。 */
	guard(raw_spinlock_irqsave)(&tk_core.lock);
	ret = raw_notifier_chain_register(&pvclock_gtod_chain, nb);
	update_pvclock_gtod(tk, true);

	return ret;
}
EXPORT_SYMBOL_GPL(pvclock_gtod_register_notifier);

/**
 * pvclock_gtod_unregister_notifier - unregister a pvclock
 * timedata update listener
 * @nb: Pointer to the notifier block to unregister
 */
int pvclock_gtod_unregister_notifier(struct notifier_block *nb)
{
	/* @nb 是调用者拥有的 notifier 节点；锁内摘链，返回 notifier 核心的注销状态码。 */
	guard(raw_spinlock_irqsave)(&tk_core.lock);
	return raw_notifier_chain_unregister(&pvclock_gtod_chain, nb);
}
EXPORT_SYMBOL_GPL(pvclock_gtod_unregister_notifier);

/*
 * tk_update_leap_state - helper to update the next_leap_ktime
 */
static inline void tk_update_leap_state(struct timekeeper *tk)
{
	/*
	 * @tk 是对应 NTP 实例的写侧对象；函数覆盖 next_leap_ktime。NTP 给出 realtime
	 * 闰秒时刻；有限值减 offs_real 转成不随 settimeofday 跳变的 monotonic 域；
	 * KTIME_MAX 保持“当前没有计划闰秒”的哨兵语义。
	 */
	tk->next_leap_ktime = ntp_get_next_leap(tk->id);
	if (tk->next_leap_ktime != KTIME_MAX)
		/* Convert to monotonic time */
		tk->next_leap_ktime = ktime_sub(tk->next_leap_ktime, tk->offs_real);
}

/*
 * Leap state update for both shadow and the real timekeeper
 * Separate to spare a full memcpy() of the timekeeper.
 */
static void tk_update_leap_state_all(struct tk_data *tkd)
{
	/*
	 * @tkd 是目标永久容器，调用者已持 tkd->lock。shadow 与正式副本只更新一个标量，
	 * seqcount 仍需覆盖两次写，防读者看到不同代；无返回值，结果直接提交到两副本。
	 */
	write_seqcount_begin(&tkd->seq);
	tk_update_leap_state(&tkd->shadow_timekeeper);
	tkd->timekeeper.next_leap_ktime = tkd->shadow_timekeeper.next_leap_ktime;
	write_seqcount_end(&tkd->seq);
}

/*
 * Update the ktime_t based scalar nsec members of the timekeeper
 */
static inline void tk_update_ktime_data(struct timekeeper *tk)
{
	/*
	 * 把 timespec 风格的 wall_to_monotonic 与 realtime 秒基点折叠成热读路径使用的
	 * ktime base。之后读取只需 base+本次 clocksource 增量，不必反复规范化 timespec。
	 */
	/* seconds 是折叠后的 monotonic 整秒；nsec 是 wall_to_mono 与 xtime 的亚秒进位工作值。 */
	u64 seconds;
	u32 nsec;

	/*
	 * The xtime based monotonic readout is:
	 *	nsec = (xtime_sec + wtm_sec) * 1e9 + wtm_nsec + now();
	 * The ktime based monotonic readout is:
	 *	nsec = base_mono + now();
	 * ==> base_mono = (xtime_sec + wtm_sec) * 1e9 + wtm_nsec
	 */
	seconds = (u64)(tk->xtime_sec + tk->wall_to_monotonic.tv_sec);
	nsec = (u32) tk->wall_to_monotonic.tv_nsec;
	tk->tkr_mono.base = ns_to_ktime(seconds * NSEC_PER_SEC + nsec);

	/*
	 * The sum of the nanoseconds portions of xtime and
	 * wall_to_monotonic can be greater/equal one second. Take
	 * this into account before updating tk->ktime_sec.
	 */
	nsec += (u32)(tk->tkr_mono.xtime_nsec >> tk->tkr_mono.shift);
	if (nsec >= NSEC_PER_SEC)
		seconds++;
	tk->ktime_sec = seconds;

	/* Update the monotonic raw base */
	tk->tkr_raw.base = ns_to_ktime(tk->raw_sec * NSEC_PER_SEC);
}

static inline void tk_update_ns_to_cyc(struct timekeeper *tks, struct timekeeper *tkc)
{
	/*
	 * coupled clockevent 需要把绝对纳秒 expiry 反算为同源 comparator cycles。只有 NTP
	 * 改变 core mono 的 mult/shift 时才重算倒数比例和安全上限；raw clocksource 参数
	 * 本身不变时快速返回。
	 */
	/* tkrs 是 shadow 新参数，tkrc 是已提交旧参数；shift 是构造倒数定点比例的总移位。 */
	struct tk_read_base *tkrs = &tks->tkr_mono;
	struct tk_read_base *tkrc = &tkc->tkr_mono;
	unsigned int shift;

	if (!IS_ENABLED(CONFIG_GENERIC_CLOCKEVENTS_COUPLED) ||
	    !(tkrs->clock->flags & CLOCK_SOURCE_HAS_COUPLED_CLOCK_EVENT))
		return;

	if (tkrs->mult == tkrc->mult && tkrs->shift == tkrc->shift)
		return;
	/*
	 * The conversion math is simple:
	 *
	 *      CS::MULT       (1 << NS_TO_CYC_SHIFT)
	 *   --------------- = ----------------------
	 *   (1 << CS:SHIFT)       NS_TO_CYC_MULT
	 *
	 * Ergo:
	 *
	 *   NS_TO_CYC_MULT = (1 << (CS::SHIFT + NS_TO_CYC_SHIFT)) / CS::MULT
	 *
	 * NS_TO_CYC_SHIFT has been set up in tk_setup_internals()
	 */
	shift = tkrs->shift + tks->cs_ns_to_cyc_shift;
	tks->cs_ns_to_cyc_mult = (u32)div_u64(1ULL << shift, tkrs->mult);
	tks->cs_ns_to_cyc_maxns = div_u64(tkrs->clock->mask, tks->cs_ns_to_cyc_mult);
}

/*
 * Restore the shadow timekeeper from the real timekeeper.
 */
static void timekeeping_restore_shadow(struct tk_data *tkd)
{
	/* 新一轮写事务必须从刚发布的正式状态开始，不能在上次临时演算残留上继续累积。 */
	lockdep_assert_held(&tkd->lock);
	memcpy(&tkd->shadow_timekeeper, &tkd->timekeeper, sizeof(tkd->timekeeper));
}

static void timekeeping_update_from_shadow(struct tk_data *tkd, unsigned int action)
{
	/*
	 * 这是 timekeeper 写事务的提交函数。入口：tkd->lock 已持有，shadow 包含完整候选
	 * 状态；出口：内核读副本、VDSO、pvclock、NMI fast 副本处于同一逻辑版本。
	 * seqcount 写区覆盖所有外部发布，避免用户先从新 VDSO 读时间、再从旧内核副本读到
	 * 更早时间。最后 memcpy 而非交换指针，是用一次冷写换取所有热读的少一次间接访问。
	 */
	/* @tkd 是持锁槽位；@action 是清 NTP/报告设时位图；tk 指向待提交 shadow。 */
	struct timekeeper *tk = &tkd->shadow_timekeeper;

	lockdep_assert_held(&tkd->lock);

	/*
	 * Block out readers before running the updates below because that
	 * updates VDSO and other time related infrastructure. Not blocking
	 * the readers might let a reader see time going backwards when
	 * reading from the VDSO after the VDSO update and then reading in
	 * the kernel from the timekeeper before that got updated.
	 */
	write_seqcount_begin(&tkd->seq);

	if (action & TK_CLEAR_NTP) {
		tk->ntp_error = 0;
		ntp_clear(tk->id);
	}

	tk_update_leap_state(tk);
	tk_update_ktime_data(tk);
	tk->tkr_mono.base_real = tk->tkr_mono.base + tk->offs_real;

	if (tk->id == TIMEKEEPER_CORE) {
		tk_update_ns_to_cyc(tk, &tkd->timekeeper);
		update_vsyscall(tk);
		update_pvclock_gtod(tk, action & TK_CLOCK_WAS_SET);

		update_fast_timekeeper(&tk->tkr_mono, &tk_fast_mono);
		update_fast_timekeeper(&tk->tkr_raw,  &tk_fast_raw);
	} else if (tk_is_aux(tk)) {
		vdso_time_update_aux(tk);
	}

	if (action & TK_CLOCK_WAS_SET)
		tk->clock_was_set_seq++;

	/*
	 * Update the real timekeeper.
	 *
	 * We could avoid this memcpy() by switching pointers, but that has
	 * the downside that the reader side does not longer benefit from
	 * the cacheline optimized data layout of the timekeeper and requires
	 * another indirection.
	 */
	memcpy(&tkd->timekeeper, tk, sizeof(*tk));
	write_seqcount_end(&tkd->seq);
}

/**
 * timekeeping_forward_now - update clock to the current time
 * @tk:		Pointer to the timekeeper to update
 *
 * Forward the current clock to update its state since the last call to
 * update_wall_time(). This is useful before significant clock changes,
 * as it avoids having to deal with this time offset explicitly.
 */
static void timekeeping_forward_now(struct timekeeper *tk)
{
	/*
	 * 在设时、调频或切换 clocksource 前先结算 cycle_last 到 now 的旧斜率时间，建立清晰
	 * 分界：旧周期按旧 mult 计价，新配置只作用于此后的周期。大 delta 分块不超过
	 * max_cycles，避免乘法溢出并复用安全换算上限。
	 */
	/* cycle_now 是本次绝对硬件读数；delta 是尚未结算、可能需分块消费的 cycles。 */
	u64 cycle_now, delta;

	cycle_now = tk_clock_read(&tk->tkr_mono);
	delta = clocksource_delta(cycle_now, tk->tkr_mono.cycle_last, tk->tkr_mono.mask,
				  tk->tkr_mono.clock->max_raw_delta);
	tk->tkr_mono.cycle_last = cycle_now;
	tk->tkr_raw.cycle_last  = cycle_now;

	while (delta > 0) {
		u64 max = tk->tkr_mono.clock->max_cycles;
		u64 incr = delta < max ? delta : max;

		tk->tkr_mono.xtime_nsec += incr * tk->tkr_mono.mult;
		tk->tkr_raw.xtime_nsec += incr * tk->tkr_raw.mult;
		tk_normalize_xtime(tk);
		delta -= incr;
	}
	tk_update_coarse_nsecs(tk);
}

/*
 * ktime_expiry_to_cycles - Convert a expiry time to clocksource cycles
 * @id:		Clocksource ID which is required for validity
 * @expires_ns:	Absolute CLOCK_MONOTONIC expiry time (nsecs) to be converted
 * @cycles:	Pointer to storage for corresponding absolute cycles value
 *
 * Convert a CLOCK_MONOTONIC based absolute expiry time to a cycles value
 * based on the correlated clocksource of the clockevent device by using
 * the base nanoseconds and cycles values of the last timekeeper update and
 * converting the delta between @expires_ns and base nanoseconds to cycles.
 *
 * This only works for clockevent devices which are using a less than or
 * equal comparator against the clocksource.
 *
 * Utilizing this avoids two clocksource reads for such devices, the
 * ktime_get() in clockevents_program_event() to calculate the delta expiry
 * value and the readout in the device::set_next_event() callback to
 * convert the delta back to a absolute comparator value.
 *
 * Returns: True if @id matches the current clocksource ID, false otherwise
 */
bool ktime_expiry_to_cycles(enum clocksource_ids id, ktime_t expires_ns, u64 *cycles)
{
	/*
	 * 补充说明：这是给“clockevent comparator 与 timekeeper 使用同一计数基准”的优化。
	 * 快速 data_race 检查只负责尽早拒绝，不承担正确性；seqcount 内再次核对 cs_id 并
	 * 复制 base/mult/shift 才形成有效快照。过期时间早于 base 时钳成 0，过远时钳到
	 * max_ns，避免负数转 u64 和乘法溢出。失败后调用者必须退回普通相对定时路径。
	 */
	/*
	 * 变量地图：tk/tkrm 指向 core 正式 mono base；base_ns/base_cycles 是同一提交点；
	 * delta_ns/delta_cycles 是目标相对基点的两种单位；max_ns 防溢出；mult/shift 是
	 * ns->cycles 定点比例；seq 验证上述字段同代；@cycles 是成功时才有意义的输出。
	 */
	struct timekeeper *tk = &tk_core.timekeeper;
	struct tk_read_base *tkrm = &tk->tkr_mono;
	ktime_t base_ns, delta_ns, max_ns;
	u64 base_cycles, delta_cycles;
	unsigned int seq;
	u32 mult, shift;

	/*
	 * Racy check to avoid the seqcount overhead when ID does not match. If
	 * the relevant clocksource is installed concurrently, then this will
	 * just delay the switch over to this mechanism until the next event is
	 * programmed. If the ID is not matching the clock events code will use
	 * the regular relative set_next_event() callback as before.
	 */
	if (data_race(tk->cs_id) != id)
		return false;

	do {
		seq = read_seqcount_begin(&tk_core.seq);

		if (tk->cs_id != id)
			return false;

		base_cycles = tkrm->cycle_last;
		base_ns = tkrm->base + (tkrm->xtime_nsec >> tkrm->shift);

		mult = tk->cs_ns_to_cyc_mult;
		shift = tk->cs_ns_to_cyc_shift;
		max_ns = tk->cs_ns_to_cyc_maxns;

	} while (read_seqcount_retry(&tk_core.seq, seq));

	/* Prevent negative deltas and multiplication overflows */
	delta_ns = min(expires_ns - base_ns, max_ns);
	delta_ns = max(delta_ns, 0);

	/* Convert to cycles */
	delta_cycles = ((u64)delta_ns * mult) >> shift;
	*cycles = base_cycles + delta_cycles;
	return true;
}

/**
 * ktime_get_real_ts64 - Returns the time of day in a timespec64.
 * @ts:		pointer to the timespec to be set
 *
 * Returns the time of day in a timespec64 (WARN if suspended).
 */
void ktime_get_real_ts64(struct timespec64 *ts)
{
	/*
	 * 典型 fine-grained seqcount 读取：在同一序列版本内取得 realtime 整秒基点和
	 * clocksource 增量，retry 后才在局部变量中规范化 timespec。seqcount 保护一致性
	 * 而非对象生命周期；全局 timekeeper 永久存在，所以无需引用计数。
	 */
	/* tk 是永久 core 对象；seq 验证快照；nsecs 是基点后增量；@ts 是调用者输出。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;
	u64 nsecs;

	WARN_ON(timekeeping_suspended);

	do {
		seq = read_seqcount_begin(&tk_core.seq);

		ts->tv_sec = tk->xtime_sec;
		nsecs = timekeeping_get_ns(&tk->tkr_mono);

	} while (read_seqcount_retry(&tk_core.seq, seq));

	ts->tv_nsec = 0;
	timespec64_add_ns(ts, nsecs);
}
EXPORT_SYMBOL(ktime_get_real_ts64);

ktime_t ktime_get(void)
{
	/* CLOCK_MONOTONIC = 已提交 base + cycle_last 之后按校正 mult 换算的当前增量。 */
	/* base 是已提交 monotonic 基点；nsecs 是当前增量；seq 保证二者来自同代 tkr。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;
	ktime_t base;
	u64 nsecs;

	WARN_ON(timekeeping_suspended);

	do {
		seq = read_seqcount_begin(&tk_core.seq);
		base = tk->tkr_mono.base;
		nsecs = timekeeping_get_ns(&tk->tkr_mono);

	} while (read_seqcount_retry(&tk_core.seq, seq));

	return ktime_add_ns(base, nsecs);
}
EXPORT_SYMBOL_GPL(ktime_get);

u32 ktime_get_resolution_ns(void)
{
	/* 每个硬件 cycle 对应的整数纳秒近似值；clocksource 切换时必须与 mult/shift 同读。 */
	/* nsecs 是返回分辨率，seq 防止读取到旧 mult 配新 shift。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;
	u32 nsecs;

	WARN_ON(timekeeping_suspended);

	do {
		seq = read_seqcount_begin(&tk_core.seq);
		nsecs = tk->tkr_mono.mult >> tk->tkr_mono.shift;
	} while (read_seqcount_retry(&tk_core.seq, seq));

	return nsecs;
}
EXPORT_SYMBOL_GPL(ktime_get_resolution_ns);

static const ktime_t *const offsets[TK_OFFS_MAX] = {
	[TK_OFFS_REAL]	= &tk_core.timekeeper.offs_real,
	[TK_OFFS_BOOT]	= &tk_core.timekeeper.offs_boot,
	[TK_OFFS_TAI]	= &tk_core.timekeeper.offs_tai,
};

/*
 * 补充说明：offsets 把“monotonic 基准 + 哪个稳定偏移”统一成一条读路径。索引必须是
 * 合法 tk_offsets；该内部 API 不做边界检查。REAL 可随设时改变，BOOT 只在恢复时
 * 累加睡眠，TAI 还包含 tai_offset，但三者都从同一 monotonic 快照派生。
 */

ktime_t ktime_get_with_offset(enum tk_offsets offs)
{
	/* tk 是 core；offset 由 @offs 选定；base/nsecs 是目标域基点和当前硬件增量；seq 验证。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	const ktime_t *offset = offsets[offs];
	unsigned int seq;
	ktime_t base;
	u64 nsecs;

	WARN_ON(timekeeping_suspended);

	do {
		seq = read_seqcount_begin(&tk_core.seq);
		base = ktime_add(tk->tkr_mono.base, *offset);
		nsecs = timekeeping_get_ns(&tk->tkr_mono);

	} while (read_seqcount_retry(&tk_core.seq, seq));

	return ktime_add_ns(base, nsecs);

}
EXPORT_SYMBOL_GPL(ktime_get_with_offset);

ktime_t ktime_get_coarse_with_offset(enum tk_offsets offs)
{
	/* coarse 省掉一次硬件 read，代价是只返回最近 update_wall_time 提交的 nsec。 */
	/* base 合入目标 offset；nsecs 取 coarse_nsec；seq 保证 offset/base/coarse 同代。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	const ktime_t *offset = offsets[offs];
	unsigned int seq;
	ktime_t base;
	u64 nsecs;

	WARN_ON(timekeeping_suspended);

	do {
		seq = read_seqcount_begin(&tk_core.seq);
		base = ktime_add(tk->tkr_mono.base, *offset);
		nsecs = tk->coarse_nsec;

	} while (read_seqcount_retry(&tk_core.seq, seq));

	return ktime_add_ns(base, nsecs);
}
EXPORT_SYMBOL_GPL(ktime_get_coarse_with_offset);

/**
 * ktime_mono_to_any() - convert monotonic time to any other time
 * @tmono:	time to convert.
 * @offs:	which offset to use
 */
ktime_t ktime_mono_to_any(ktime_t tmono, enum tk_offsets offs)
{
	/* @tmono 是输入 monotonic；offset 选择目标域；tconv 是 32 位 seqcount 路径的候选结果。 */
	const ktime_t *offset = offsets[offs];
	unsigned int seq;
	ktime_t tconv;

	if (IS_ENABLED(CONFIG_64BIT)) {
		/*
		 * Paired with WRITE_ONCE()s in tk_set_wall_to_mono() and
		 * tk_update_sleep_time().
		 */
		/* 64 位单次 load 不会撕裂，允许用稍旧/稍新的完整 offset 快照换取无循环读取。 */
		return ktime_add(tmono, READ_ONCE(*offset));
	}

	/* 32 位读取 64 位 ktime 可能撕裂，必须用 seqcount 检测写侧穿插。 */
	do {
		seq = read_seqcount_begin(&tk_core.seq);
		tconv = ktime_add(tmono, *offset);
	} while (read_seqcount_retry(&tk_core.seq, seq));

	return tconv;
}
EXPORT_SYMBOL_GPL(ktime_mono_to_any);

/**
 * ktime_get_raw - Returns the raw monotonic time in ktime_t format
 */
ktime_t ktime_get_raw(void)
{
	/* raw 使用未被 NTP 改斜率的 tkr_raw，适合测量硬件经过时间而非民用时钟。 */
	/* base 是 raw 已提交基点；nsecs 是 raw 周期增量；seq 验证同一 read base 版本。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;
	ktime_t base;
	u64 nsecs;

	do {
		seq = read_seqcount_begin(&tk_core.seq);
		base = tk->tkr_raw.base;
		nsecs = timekeeping_get_ns(&tk->tkr_raw);

	} while (read_seqcount_retry(&tk_core.seq, seq));

	return ktime_add_ns(base, nsecs);
}
EXPORT_SYMBOL_GPL(ktime_get_raw);

/**
 * ktime_get_ts64 - get the monotonic clock in timespec64 format
 * @ts:		pointer to timespec variable
 *
 * The function calculates the monotonic clock from the realtime
 * clock and the wall_to_monotonic offset and stores the result
 * in normalized timespec64 format in the variable pointed to by @ts.
 */
void ktime_get_ts64(struct timespec64 *ts)
{
	/*
	 * 旧式表示从 realtime xtime 加 wall_to_monotonic 得到 monotonic；两部分必须处于
	 * 同一 seqcount 版本，否则一次 settimeofday 会令 monotonic 跟着墙钟跳变。
	 */
	/* tomono 缓存 wall_to_monotonic；nsec 是 realtime 当前增量；@ts 承载最终 mono 输出。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	struct timespec64 tomono;
	unsigned int seq;
	u64 nsec;

	WARN_ON(timekeeping_suspended);

	do {
		seq = read_seqcount_begin(&tk_core.seq);
		ts->tv_sec = tk->xtime_sec;
		nsec = timekeeping_get_ns(&tk->tkr_mono);
		tomono = tk->wall_to_monotonic;

	} while (read_seqcount_retry(&tk_core.seq, seq));

	ts->tv_sec += tomono.tv_sec;
	ts->tv_nsec = 0;
	timespec64_add_ns(ts, nsec + tomono.tv_nsec);
}
EXPORT_SYMBOL_GPL(ktime_get_ts64);

/**
 * ktime_get_seconds - Get the seconds portion of CLOCK_MONOTONIC
 *
 * Returns the seconds portion of CLOCK_MONOTONIC with a single non
 * serialized read. tk->ktime_sec is of type 'unsigned long' so this
 * works on both 32 and 64 bit systems. On 32 bit systems the readout
 * covers ~136 years of uptime which should be enough to prevent
 * premature wrap arounds.
 */
time64_t ktime_get_seconds(void)
{
	/* 秒级缓存故意放弃亚秒精度，换取一次本机字长 load；适合超时统计而非事件排序。 */
	struct timekeeper *tk = &tk_core.timekeeper;

	WARN_ON(timekeeping_suspended);
	return tk->ktime_sec;
}
EXPORT_SYMBOL_GPL(ktime_get_seconds);

/**
 * ktime_get_real_seconds - Get the seconds portion of CLOCK_REALTIME
 *
 * Returns the wall clock seconds since 1970.
 *
 * For 64bit systems the fast access to tk->xtime_sec is preserved. On
 * 32bit systems the access must be protected with the sequence
 * counter to provide "atomic" access to the 64bit tk->xtime_sec
 * value.
 */
time64_t ktime_get_real_seconds(void)
{
	/* 64 位可单次读 xtime_sec；32 位会撕裂同一 64 位字段，故用 seqcount 重试。 */
	/* seconds 是 32 位重试路径的局部完整副本；seq 只在该配置路径使用。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	time64_t seconds;
	unsigned int seq;

	if (IS_ENABLED(CONFIG_64BIT))
		return tk->xtime_sec;

	do {
		seq = read_seqcount_begin(&tk_core.seq);
		seconds = tk->xtime_sec;

	} while (read_seqcount_retry(&tk_core.seq, seq));

	return seconds;
}
EXPORT_SYMBOL_GPL(ktime_get_real_seconds);

/**
 * __ktime_get_real_seconds - Unprotected access to CLOCK_REALTIME seconds
 *
 * The same as ktime_get_real_seconds() but without the sequence counter
 * protection. This function is used in restricted contexts like the x86 MCE
 * handler and in KGDB. It's unprotected on 32-bit vs. concurrent half
 * completed modification and only to be used for such critical contexts.
 *
 * Returns: Racy snapshot of the CLOCK_REALTIME seconds value
 */
noinstr time64_t __ktime_get_real_seconds(void)
{
	/* MCE/KGDB 等受限上下文宁可接受竞态快照，也不能调用可能被插桩/重试的普通路径。 */
	struct timekeeper *tk = &tk_core.timekeeper;

	return tk->xtime_sec;
}

static inline u64 tk_clock_read_snapshot(const struct tk_read_base *tkr,
					 struct clocksource_hw_snapshot *chs)
{
	/* read_snapshot 可同时采集关联硬件 counter；没有该能力时只返回普通 clocksource 值。 */
	/* clock 是本次固定源；@chs 是可选关联硬件快照输出，由具体 clocksource 填写。 */
	struct clocksource *clock = READ_ONCE(tkr->clock);

	if (unlikely(clock->read_snapshot))
		return clock->read_snapshot(clock, chs);

	return clock->read(clock);
}


/**
 * ktime_get_snapshot_id -  Simultaneously snapshot a given clock ID with
 *			    CLOCK_MONOTONIC_RAW and the underlying
 *			    clocksource counter value.
 * @clock_id:		The clock ID to snapshot
 * @systime_snapshot:	Pointer to struct receiving the system time snapshot
 */
void ktime_get_snapshot_id(clockid_t clock_id, struct system_time_snapshot *systime_snapshot)
{
	/*
	 * 补充说明：一次 clocksource 读取同时投影到目标系统时间和 MONOTONIC_RAW，供 PTP
	 * 等调用者建立“同一硬件瞬间”的相关性。输出先置 invalid，只有地址域、aux 有效性、
	 * clocksource snapshot 和全部基点通过同一 seqcount 版本后才置 true。
	 * cs_was_changed_seq/clock_was_set_seq 是给历史插值检测不连续的版本戳，不是时间值。
	 */
	/*
	 * 变量地图：tkd/tk 是所选时钟槽及正式状态；offs 指向目标域 offset，offs_zero
	 * 表示 mono/raw 无额外偏移；base_sys/base_raw 是同一提交点两种基点；now 是唯一
	 * clocksource 读数；nsec_sys/nsec_raw 是其两种斜率投影；seq 验证全组；输出先 invalid。
	 */
	ktime_t base_raw, base_sys, offs_sys, *offs, offs_zero = 0;
	u64 nsec_raw, nsec_sys, now;
	struct timekeeper *tk;
	struct tk_data *tkd;
	unsigned int seq;

	/* Invalidate the snapshot for all failure cases */
	systime_snapshot->valid = false;

	if (WARN_ON_ONCE(timekeeping_suspended))
		return;

	switch (clock_id) {
	case CLOCK_REALTIME:
		tkd = &tk_core;
		offs = &tk_core.timekeeper.offs_real;
		break;
	/* Map RAW to MONOTONIC so the loop below is trivial */
	case CLOCK_MONOTONIC_RAW:
	case CLOCK_MONOTONIC:
		tkd = &tk_core;
		offs = &offs_zero;
		break;
	case CLOCK_BOOTTIME:
		tkd = &tk_core;
		offs = &tk_core.timekeeper.offs_boot;
		break;
	case CLOCK_AUX ... CLOCK_AUX_LAST:
		tkd = aux_get_tk_data(clock_id);
		if (!tkd)
			return;
		offs = &tkd->timekeeper.offs_aux;
		break;
	default:
		WARN_ON_ONCE(1);
		return;
	}

	tk = &tkd->timekeeper;

	do {
		struct clocksource_hw_snapshot chs = { };

		seq = read_seqcount_begin(&tkd->seq);

		/* Aux clocks can be invalid */
		if (!tk->clock_valid)
			return;

		now = tk_clock_read_snapshot(&tk->tkr_mono, &chs);
		systime_snapshot->cs_id = tk->tkr_mono.clock->id;

		systime_snapshot->hw_cycles = chs.hw_cycles;
		systime_snapshot->hw_csid = chs.hw_csid;

		systime_snapshot->cs_was_changed_seq = tk->cs_was_changed_seq;
		systime_snapshot->clock_was_set_seq = tk->clock_was_set_seq;

		base_sys = tk->tkr_mono.base;
		offs_sys = *offs;
		base_raw = tk->tkr_raw.base;

		nsec_sys = timekeeping_cycles_to_ns(&tk->tkr_mono, now);
		nsec_raw = timekeeping_cycles_to_ns(&tk->tkr_raw, now);
	} while (read_seqcount_retry(&tkd->seq, seq));

	systime_snapshot->cycles = now;
	systime_snapshot->systime = ktime_add_ns(base_sys, offs_sys + nsec_sys);
	systime_snapshot->monoraw = ktime_add_ns(base_raw, nsec_raw);

	/*
	 * Special case for PTP. Just transfer the raw time into sys,
	 * so the call sites can consistently use snap::systime.
	 */
	if (clock_id == CLOCK_MONOTONIC_RAW)
		systime_snapshot->systime = systime_snapshot->monoraw;
	/* Tell the consumer that this snapshot is valid */
	systime_snapshot->valid = true;
}
EXPORT_SYMBOL_GPL(ktime_get_snapshot_id);

/* Scale base by mult/div checking for overflow */
static int scale64_check_overflow(u64 mult, u64 div, u64 *base)
{
	/*
	 * 把 base 拆成 quotient/remainder 后分别乘 numerator，避免先做 base*mult 溢出。
	 * fls64 预检位宽；失败不修改 *base，成功才提交缩放结果。
	 */
	/* @base 是输入输出被缩放量；tmp/rem 分别保存除法商和余数；@mult/@div 是比例。 */
	u64 tmp, rem;

	tmp = div64_u64_rem(*base, div, &rem);

	if (((int)sizeof(u64)*8 - fls64(mult) < fls64(tmp)) ||
	    ((int)sizeof(u64)*8 - fls64(mult) < fls64(rem)))
		return -EOVERFLOW;
	tmp *= mult;

	rem = div64_u64(rem * mult, div);
	*base = tmp + rem;
	return 0;
}

/**
 * adjust_historical_crosststamp - adjust crosstimestamp previous to current interval
 * @history:			Snapshot representing start of history
 * @partial_history_cycles:	Cycle offset into history (fractional part)
 * @total_history_cycles:	Total history length in cycles
 * @discontinuity:		True indicates clock was set on history period
 * @ts:				Cross timestamp that should be adjusted using
 *	partial/total ratio
 *
 * Helper function used by get_device_system_crosststamp() to correct the
 * crosstimestamp corresponding to the start of the current interval to the
 * system counter value (timestamp point) provided by the driver. The
 * total_history_* quantities are the total history starting at the provided
 * reference point and ending at the start of the current interval. The cycle
 * count between the driver timestamp point and the start of the current
 * interval is partial_history_cycles.
 */
static int adjust_historical_crosststamp(struct system_time_snapshot *history,
					 u64 partial_history_cycles,
					 u64 total_history_cycles,
					 bool discontinuity,
					 struct system_device_crosststamp *ts)
{
	/*
	 * 补充说明：设备给出的系统 counter 可能早于当前 timekeeper interval，只能借历史
	 * snapshot 按 cycle 比例插值。选择离区间起点或终点更近的一侧可缩小乘法操作数；
	 * 若期间墙钟被设置，不能直接按 realtime 端点线性插值，改用 raw 修正再乘当前
	 * mono/raw 斜率比。返回错误表示历史跨 clocksource 或算术范围不足，结果不可用。
	 */
	/*
	 * tk 提供 mono/raw 斜率；corr_raw/corr_sys 是两个时间域的插值修正纳秒；
	 * interp_forward 选择从 history 向前或从当前端点向后；ret 传播溢出错误；@ts 是输出。
	 */
	struct timekeeper *tk = &tk_core.timekeeper;
	u64 corr_raw, corr_sys;
	bool interp_forward;
	int ret;

	if (total_history_cycles == 0 || partial_history_cycles == 0)
		return 0;

	/* Interpolate shortest distance from beginning or end of history */
	interp_forward = partial_history_cycles > total_history_cycles / 2;
	partial_history_cycles = interp_forward ?
		total_history_cycles - partial_history_cycles :
		partial_history_cycles;

	/*
	 * Scale the monotonic raw time delta by:
	 *	partial_history_cycles / total_history_cycles
	 */
	corr_raw = (u64)ktime_to_ns(ktime_sub(ts->sys_monoraw, history->monoraw));
	ret = scale64_check_overflow(partial_history_cycles,
				     total_history_cycles, &corr_raw);
	if (ret)
		return ret;

	/*
	 * If there is a discontinuity in the history, scale monotonic raw
	 * correction by:
	 *	mult(sys)/mult(raw) yielding the system time correction
	 *
	 * Otherwise, calculate the system time correction similar to monotonic
	 * raw calculation
	 */
	if (discontinuity) {
		corr_sys = mul_u64_u32_div(corr_raw, tk->tkr_mono.mult, tk->tkr_raw.mult);
	} else {
		corr_sys = (u64)ktime_to_ns(ktime_sub(ts->sys_systime, history->systime));
		ret = scale64_check_overflow(partial_history_cycles, total_history_cycles,
					     &corr_sys);
		if (ret)
			return ret;
	}

	/* Fixup monotonic raw and system time time values */
	if (interp_forward) {
		ts->sys_monoraw = ktime_add_ns(history->monoraw, corr_raw);
		ts->sys_systime = ktime_add_ns(history->systime, corr_sys);
	} else {
		ts->sys_monoraw = ktime_sub_ns(ts->sys_monoraw, corr_raw);
		ts->sys_systime = ktime_sub_ns(ts->sys_systime, corr_sys);
	}

	return 0;
}

/*
 * timestamp_in_interval - true if ts is chronologically in [start, end]
 *
 * True if ts occurs chronologically at or after start, and before or at end.
 */
static bool timestamp_in_interval(u64 start, u64 end, u64 ts)
{
	/*
	 * @start/@end 是同一周期域的闭区间边界，@ts 是待判定周期值；true 表示含端点命中。
	 * 第二个分支处理环形计数器跨 mask 回绕时 start > end 的区间。
	 */
	if (ts >= start && ts <= end)
		return true;
	if (start > end && (ts >= start || ts <= end))
		return true;
	return false;
}

static bool convert_clock(u64 *val, u32 numerator, u32 denominator)
{
	/* quotient/remainder 分解避免 *val*numerator 的中间溢出；零比例没有可逆含义。 */
	/* res/rem 是 @val/denominator 的商路径和余数路径，最终合并写回 @val。 */
	u64 rem, res;

	if (!numerator || !denominator)
		return false;

	res = div64_u64_rem(*val, denominator, &rem) * numerator;
	*val = res + div_u64(rem * numerator, denominator);
	return true;
}

static bool convert_base_to_cs(struct system_counterval_t *scv)
{
	/*
	 * 驱动可报告当前 clocksource 或其声明的 base counter。READ_ONCE 稳定 base 指针，
	 * 随后按比例和 offset 折算到 timekeeper clocksource 域，并更新 cs_id 表示 ownership
	 * 已变；clocksource 并发切换最终仍由外层 seqcount retry 兜底。
	 */
	/* cs 是当前源快照；base 是其关联底层源；num/den 是 base->cs 的换算比例。 */
	struct clocksource *cs = tk_core.timekeeper.tkr_mono.clock;
	struct clocksource_base *base;
	u32 num, den;

	/* The timestamp was taken from the time keeper clock source */
	if (cs->id == scv->cs_id)
		return true;

	/*
	 * Check whether cs_id matches the base clock. Prevent the compiler from
	 * re-evaluating @base as the clocksource might change concurrently.
	 */
	base = READ_ONCE(cs->base);
	if (!base || base->id != scv->cs_id)
		return false;

	num = scv->use_nsecs ? cs->freq_khz : base->numerator;
	den = scv->use_nsecs ? USEC_PER_SEC : base->denominator;

	if (!convert_clock(&scv->cycles, num, den))
		return false;

	scv->cycles += base->offset;
	/* Set the clocksource ID as scv::cycles is now clocksource based */
	scv->cs_id = cs->id;
	return true;
}

static bool convert_cs_to_base(u64 *cycles, enum clocksource_ids base_id)
{
	/* 当前 clocksource absolute cycles 先减 base offset，再按声明比例还原到 base 域。 */
	struct clocksource *cs = tk_core.timekeeper.tkr_mono.clock;
	struct clocksource_base *base;

	/*
	 * Check whether base_id matches the base clock. Prevent the compiler from
	 * re-evaluating @base as the clocksource might change concurrently.
	 */
	base = READ_ONCE(cs->base);
	if (!base || base->id != base_id)
		return false;

	*cycles -= base->offset;
	if (!convert_clock(cycles, base->denominator, base->numerator))
		return false;
	return true;
}

static bool convert_ns_to_cs(u64 *delta)
{
	/* 反解 ns=(cycles*mult+xtime_nsec)>>shift；左移前先检查位宽，拒绝不可表示的未来值。 */
	struct tk_read_base *tkr = &tk_core.timekeeper.tkr_mono;

	if (BITS_TO_BYTES(fls64(*delta) + tkr->shift) >= sizeof(*delta))
		return false;

	*delta = div_u64((*delta << tkr->shift) - tkr->xtime_nsec, tkr->mult);
	return true;
}

/**
 * ktime_real_to_base_clock() - Convert CLOCK_REALTIME timestamp to a base clock timestamp
 * @treal:	CLOCK_REALTIME timestamp to convert
 * @base_id:	base clocksource id
 * @cycles:	pointer to store the converted base clock timestamp
 *
 * Converts a supplied, future realtime clock value to the corresponding base clock value.
 *
 * Return:  true if the conversion is successful, false otherwise.
 */
bool ktime_real_to_base_clock(ktime_t treal, enum clocksource_ids base_id, u64 *cycles)
{
	/*
	 * 补充说明：只接受不早于当前 base_real 的未来 realtime；先反解成当前 clocksource
	 * absolute cycles，再转换到设备要求的 base。任一步失败或写侧更新穿插都不发布
	 * 成功，调用者应退回普通时间换算/编程路径。
	 */
	/* @treal 是未来 realtime；delta 先转当前源 cycles；@cycles 再写成 @base_id 域；seq 验证。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;
	u64 delta;

	do {
		seq = read_seqcount_begin(&tk_core.seq);
		if ((u64)treal < tk->tkr_mono.base_real)
			return false;
		delta = (u64)treal - tk->tkr_mono.base_real;
		if (!convert_ns_to_cs(&delta))
			return false;
		*cycles = tk->tkr_mono.cycle_last + delta;
		if (!convert_cs_to_base(cycles, base_id))
			return false;
	} while (read_seqcount_retry(&tk_core.seq, seq));

	return true;
}
EXPORT_SYMBOL_GPL(ktime_real_to_base_clock);

/**
 * get_device_system_crosststamp - Synchronously capture system/device timestamp
 * @get_time_fn:	Callback to get simultaneous device time and system counter
 *			from the device driver
 * @ctx:		Context passed to get_time_fn()
 * @history_begin:	Historical reference point used to interpolate system time when
 *			the counter value provided by the driver is before the current interval
 * @xtstamp:		Receives simultaneously captured system and device time
 *
 * Reads a timestamp from a device and correlates it to system time
 */
int get_device_system_crosststamp(int (*get_time_fn)
				  (ktime_t *device_time,
				   struct system_counterval_t *sys_counterval,
				   void *ctx),
				  void *ctx,
				  struct system_time_snapshot *history_begin,
				  struct system_device_crosststamp *xtstamp)
{
	/*
	 * 补充说明：驱动回调必须近同步地返回 device_time 和 system counter，而不是两个
	 * 任意时刻的独立读取。回调运行在 seqcount 读循环内但没有持 timekeeper 写锁；
	 * 若写侧穿插，整个循环（包括回调）可能重做，因此回调必须容忍重复调用和重试。
	 *
	 * 若 counter 位于 [cycle_last, now]，可直接用当前 base/mult 投影；若更早，则要求
	 * history_begin 与当前使用同一 clocksource，并通过版本戳判断墙钟是否跳变后插值。
	 * 返回 0 承诺 device/sys_systime/sys_monoraw 对应同一捕获点；错误时输出不可采用。
	 */
	/*
	 * 变量地图：syscnt_cycles 保留驱动原始 counter；cycles 可改成当前 interval 起点用于
	 * 插值；now/interval_start 界定本轮有效区间；tkd/tk/offs 选择目标时间域；base_* 与
	 * nsec_* 构造 system/raw 输出；两个 *_seq 检测历史不连续；do_interp 决定慢路径；
	 * ret 传播驱动或算术错误。@ctx 借给回调，@xtstamp 是调用者输出对象。
	 */
	u64 syscnt_cycles, cycles, now, interval_start;
	unsigned int seq, clock_was_set_seq = 0;
	ktime_t base_sys, base_raw, *offs;
	u64 nsec_sys, nsec_raw;
	u8 cs_was_changed_seq;
	bool do_interp;
	struct timekeeper *tk;
	struct tk_data *tkd;
	int ret;

	switch (xtstamp->clock_id) {
	case CLOCK_REALTIME:
		tkd = &tk_core;
		offs = &tk_core.timekeeper.offs_real;
		break;
	case CLOCK_AUX ... CLOCK_AUX_LAST:
		tkd = aux_get_tk_data(xtstamp->clock_id);
		if (!tkd)
			return -ENODEV;
		offs = &tkd->timekeeper.offs_aux;
		break;
	default:
		WARN_ON_ONCE(1);
		return -ENODEV;
	}

	tk = &tkd->timekeeper;

	do {
		seq = read_seqcount_begin(&tkd->seq);
		/*
		 * Try to synchronously capture device time and a system
		 * counter value calling back into the device driver
		 */
		ret = get_time_fn(&xtstamp->device, &xtstamp->sys_counter, ctx);
		if (ret)
			return ret;

		/*
		 * Verify that the clocksource ID associated with the captured
		 * system counter value is the same as for the currently
		 * installed timekeeper clocksource and convert to it.
		 */
		if (xtstamp->sys_counter.cs_id == CSID_GENERIC ||
		    !convert_base_to_cs(&xtstamp->sys_counter))
			return -ENODEV;

		cycles = syscnt_cycles = xtstamp->sys_counter.cycles;

		/*
		 * Check whether the system counter value provided by the
		 * device driver is on the current timekeeping interval.
		 */
		now = tk_clock_read(&tk->tkr_mono);
		interval_start = tk->tkr_mono.cycle_last;
		if (!timestamp_in_interval(interval_start, now, cycles)) {
			clock_was_set_seq = tk->clock_was_set_seq;
			cs_was_changed_seq = tk->cs_was_changed_seq;
			cycles = interval_start;
			do_interp = true;
		} else {
			do_interp = false;
		}

		base_sys = ktime_add(tk->tkr_mono.base, *offs);
		base_raw = tk->tkr_raw.base;

		nsec_sys = timekeeping_cycles_to_ns(&tk->tkr_mono, cycles);
		nsec_raw = timekeeping_cycles_to_ns(&tk->tkr_raw, cycles);
	} while (read_seqcount_retry(&tkd->seq, seq));

	xtstamp->sys_systime = ktime_add_ns(base_sys, nsec_sys);
	xtstamp->sys_monoraw = ktime_add_ns(base_raw, nsec_raw);

	/*
	 * Interpolate if necessary, adjusting back from the start of the
	 * current interval
	 */
	if (do_interp) {
		/* partial 是驱动点到当前起点距离，total 是 history 到当前起点距离；discontinuity
		 * 表示两端之间发生过墙钟设置，决定 system 修正使用哪种斜率。
		 */
		u64 partial_history_cycles, total_history_cycles;
		bool discontinuity;

		/*
		 * Check that the counter value is not before the provided
		 * history reference and that the history doesn't cross a
		 * clocksource change
		 */
		if (!history_begin ||
		    !timestamp_in_interval(history_begin->cycles, cycles, syscnt_cycles) ||
		    history_begin->cs_was_changed_seq != cs_was_changed_seq)
			return -EINVAL;

		partial_history_cycles = cycles - syscnt_cycles;
		total_history_cycles = cycles - history_begin->cycles;
		discontinuity = history_begin->clock_was_set_seq != clock_was_set_seq;

		ret = adjust_historical_crosststamp(history_begin, partial_history_cycles,
						    total_history_cycles, discontinuity, xtstamp);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(get_device_system_crosststamp);

/**
 * timekeeping_clocksource_has_base - Check whether the current clocksource
 *				      is based on given a base clock
 * @id:		base clocksource ID
 *
 * Note:	The return value is a snapshot which can become invalid right
 *		after the function returns.
 *
 * Return:	true if the timekeeper clocksource has a base clock with @id,
 *		false otherwise
 */
bool timekeeping_clocksource_has_base(enum clocksource_ids id)
{
	/*
	 * 这里只提供瞬时能力探测，不试图把结果稳定到调用者使用时刻。READ_ONCE 防编译器
	 * 重取 base 指针造成同一表达式混合；需要强一致性的调用者必须使用更高层锁协议。
	 */
	/*
	 * This is a snapshot, so no point in using the sequence
	 * count. Just prevent the compiler from re-evaluating @base as the
	 * clocksource might change concurrently.
	 */
	struct clocksource_base *base = READ_ONCE(tk_core.timekeeper.tkr_mono.clock->base);

	return base ? base->id == id : false;
}
EXPORT_SYMBOL_GPL(timekeeping_clocksource_has_base);

/**
 * do_settimeofday64 - Sets the time of day.
 * @ts:     pointer to the timespec64 variable containing the new time
 *
 * Sets the time of day to the new time and update NTP and notify hrtimers
 */
int do_settimeofday64(const struct timespec64 *ts)
{
	/*
	 * 补充说明：这是“绝对设墙钟”事务。先在锁内 forward_now 把旧 clocksource 斜率
	 * 结算到调用瞬间，再计算 new-old 的 ts_delta；xtime 增加 delta 的同时让
	 * wall_to_monotonic 减去同一 delta，因此 CLOCK_MONOTONIC 保持连续。
	 *
	 * 若新组合会使 monotonic 基点非法，restore_shadow 撤销尚未发布的演算。提交后才
	 * 在锁外通知 hrtimer/timerfd、audit 和随机池，避免回调在 timekeeper 锁内重入。
	 */
	/* @ts 是借用的新绝对墙钟；xt 是 forward 后旧墙钟；ts_delta=new-old，驱动两类 offset。 */
	struct timespec64 ts_delta, xt;

	if (!timespec64_valid_settod(ts))
		return -EINVAL;

	scoped_guard (raw_spinlock_irqsave, &tk_core.lock) {
		struct timekeeper *tks = &tk_core.shadow_timekeeper;

		timekeeping_forward_now(tks);

		xt = tk_xtime(tks);
		ts_delta = timespec64_sub(*ts, xt);

		if (timespec64_compare(&tks->wall_to_monotonic, &ts_delta) > 0) {
			timekeeping_restore_shadow(&tk_core);
			return -EINVAL;
		}

		tk_set_wall_to_mono(tks, timespec64_sub(tks->wall_to_monotonic, ts_delta));
		tk_set_xtime(tks, ts);
		timekeeping_update_from_shadow(&tk_core, TK_UPDATE_ALL);
	}

	/* Signal hrtimers about time change */
	clock_was_set(CLOCK_SET_WALL);

	audit_tk_injoffset(ts_delta);
	add_device_randomness(ts, sizeof(*ts));
	return 0;
}
EXPORT_SYMBOL(do_settimeofday64);

static inline bool timekeeper_is_core_tk(struct timekeeper *tk)
{
	/* aux 配置关闭时唯一 timekeeper 天然是 core；开启时用稳定 id 区分不同设时语义。 */
	return !IS_ENABLED(CONFIG_POSIX_AUX_CLOCKS) || tk->id == TIMEKEEPER_CORE;
}

/**
 * __timekeeping_inject_offset - Adds or subtracts from the current time.
 * @tkd:	Pointer to the timekeeper to modify
 * @ts:		Pointer to the timespec variable containing the offset
 *
 * Adds or subtracts an offset value from the current time.
 */
static int __timekeeping_inject_offset(struct tk_data *tkd, const struct timespec64 *ts)
{
	/*
	 * 补充说明：调用者已经持有 tkd->lock，@ts 是有符号相对偏移且 tv_nsec 必须规范化。
	 * core 与 settimeofday 一样反向调整 wall_to_monotonic；aux 只改变 offs_aux，并拒绝
	 * 结果落到负时间。错误发生在提交前，用正式副本覆盖 shadow 完成回滚。
	 */
	/* tkd/tks 是持锁槽位及 shadow；@ts 是借用相对量；tmp 验证 core 新 realtime。 */
	struct timekeeper *tks = &tkd->shadow_timekeeper;
	struct timespec64 tmp;

	if (ts->tv_nsec < 0 || ts->tv_nsec >= NSEC_PER_SEC)
		return -EINVAL;

	timekeeping_forward_now(tks);

	if (timekeeper_is_core_tk(tks)) {
		/* Make sure the proposed value is valid */
		tmp = timespec64_add(tk_xtime(tks), *ts);
		if (timespec64_compare(&tks->wall_to_monotonic, ts) > 0 ||
		    !timespec64_valid_settod(&tmp)) {
			timekeeping_restore_shadow(tkd);
			return -EINVAL;
		}

		tk_xtime_add(tks, ts);
		tk_set_wall_to_mono(tks, timespec64_sub(tks->wall_to_monotonic, *ts));
	} else {
		/* tkr_mono 是 aux 基准；now 是当前无 offset 时间；offs 是注入后的新 aux offset。 */
		struct tk_read_base *tkr_mono = &tks->tkr_mono;
		ktime_t now, offs;

		/* Get the current time */
		now = ktime_add_ns(tkr_mono->base, timekeeping_get_ns(tkr_mono));
		/* Add the relative offset change */
		offs = ktime_add(tks->offs_aux, timespec64_to_ktime(*ts));

		/* Prevent that the resulting time becomes negative */
		if (ktime_add(now, offs) < 0) {
			timekeeping_restore_shadow(tkd);
			return -EINVAL;
		}
		tk_update_aux_offs(tks, offs);
	}

	timekeeping_update_from_shadow(tkd, TK_UPDATE_ALL);
	return 0;
}

static int timekeeping_inject_offset(const struct timespec64 *ts)
{
	/* core 包装负责锁和锁外 clock_was_set；内部 helper 也被 aux 路径复用。 */
	/* ret 保存锁内 helper 结果，决定锁外是否广播；@ts 全程只借用。 */
	int ret;

	scoped_guard (raw_spinlock_irqsave, &tk_core.lock)
		ret = __timekeeping_inject_offset(&tk_core, ts);

	/* Signal hrtimers about time change */
	if (!ret)
		clock_was_set(CLOCK_SET_WALL);
	return ret;
}

/*
 * Indicates if there is an offset between the system clock and the hardware
 * clock/persistent clock/rtc.
 */
int persistent_clock_is_local;

/*
 * Adjust the time obtained from the CMOS to be UTC time instead of
 * local time.
 *
 * This is ugly, but preferable to the alternatives.  Otherwise we
 * would either need to write a program to do it in /etc/rc (and risk
 * confusion if the program gets run more than once; it would also be
 * hard to make the program warp the clock precisely n hours)  or
 * compile in the timezone information into the kernel.  Bad, bad....
 *
 *						- TYT, 1992-01-01
 *
 * The best thing to do is to keep the CMOS clock in universal time (UTC)
 * as real UNIX machines always do it. This avoids all headaches about
 * daylight saving times and warping kernel clocks.
 */
void timekeeping_warp_clock(void)
{
	/*
	 * 历史兼容：RTC 若按 local time 保存，用 sys_tz 把启动墙钟扭成 UTC。现代系统应让
	 * RTC 直接存 UTC；否则 DST/时区策略进入内核并容易重复校正。
	 */
	if (sys_tz.tz_minuteswest != 0) {
		/* adjust 是把“UTC 落后本地时间多少分钟”换成规范化秒偏移的栈对象。 */
		struct timespec64 adjust;

		persistent_clock_is_local = 1;
		adjust.tv_sec = sys_tz.tz_minuteswest * 60;
		adjust.tv_nsec = 0;
		timekeeping_inject_offset(&adjust);
	}
}

/*
 * __timekeeping_set_tai_offset - Sets the TAI offset from UTC and monotonic
 */
static void __timekeeping_set_tai_offset(struct timekeeper *tk, s32 tai_offset)
{
	/* TAI = monotonic + offs_real + tai_offset；必须同时缓存标量 offset 供热读。 */
	tk->tai_offset = tai_offset;
	tk->offs_tai = ktime_add(tk->offs_real, ktime_set(tai_offset, 0));
}

/*
 * change_clocksource - Swaps clocksources if a new one is available
 *
 * Accumulates current time interval and initializes new clocksource
 */
static int change_clocksource(void *data)
{
	/*
	 * 补充说明：由 stop_machine 在所有 CPU 不会并发执行旧 clocksource 内联读的环境中
	 * 调用。先取得新 clocksource 模块引用并 enable；锁内 forward 旧时间、替换换算参数
	 * 并原子发布；成功后才 disable/put 旧源。任何前置失败都保留旧源且返回 0，最终
	 * 是否切换由 timekeeping_notify 比较实际指针判断。
	 */
	/* new 借用 stop_machine 的 @data，引用在本函数取得；old 成功提交后负责 disable/put。 */
	struct clocksource *new = data, *old = NULL;

	/*
	 * If the clocksource is in a module, get a module reference.
	 * Succeeds for built-in code (owner == NULL) as well. Abort if the
	 * reference can't be acquired.
	 */
	if (!try_module_get(new->owner))
		return 0;

	/* Abort if the device can't be enabled */
	if (new->enable && new->enable(new) != 0) {
		module_put(new->owner);
		return 0;
	}

	scoped_guard (raw_spinlock_irqsave, &tk_core.lock) {
		struct timekeeper *tks = &tk_core.shadow_timekeeper;

		timekeeping_forward_now(tks);
		old = tks->tkr_mono.clock;
		tk_setup_internals(tks, new);
		timekeeping_update_from_shadow(&tk_core, TK_UPDATE_ALL);
	}

	tk_aux_update_clocksource();

	if (old) {
		if (old->disable)
			old->disable(old);
		module_put(old->owner);
	}

	return 0;
}

/**
 * timekeeping_notify - Install a new clock source
 * @clock:		pointer to the clock source
 *
 * This function is called from clocksource.c after a new, better clock
 * source has been registered. The caller holds the clocksource_mutex.
 */
int timekeeping_notify(struct clocksource *clock)
{
	/*
	 * 补充说明：clocksource_mutex 只串行化候选选择；实际 timekeeper 与各 CPU 的代码
	 * patch/读取还需 stop_machine。切换前关闭 static-key 内联路径，避免 CPU 在替换
	 * 回调期间执行绑定旧源的架构指令；确认新源真正安装且支持后再重新开启。
	 */
	/* @clock 是 clocksource core 选出的候选；tk 用于切换前后确认实际 active 指针。 */
	struct timekeeper *tk = &tk_core.timekeeper;

	if (tk->tkr_mono.clock == clock)
		return 0;

	/* Disable inlined reads accross the clocksource switch */
	clocksource_disable_inline_read();

	stop_machine(change_clocksource, clock, NULL);

	/*
	 * If the clocksource has been selected and supports inlined reads
	 * enable the branch.
	 */
	if (tk->tkr_mono.clock == clock && clock->flags & CLOCK_SOURCE_CAN_INLINE_READ)
		clocksource_enable_inline_read();

	tick_clock_notify();
	return tk->tkr_mono.clock == clock ? 0 : -1;
}

/**
 * ktime_get_raw_ts64 - Returns the raw monotonic time in a timespec
 * @ts:		pointer to the timespec64 to be set
 *
 * Returns the raw monotonic time (completely un-modified by ntp)
 */
void ktime_get_raw_ts64(struct timespec64 *ts)
{
	/* raw_sec 与 raw cycle 增量必须同代读取；结果完全不应用 NTP/设时 offset。 */
	/* nsecs 是 raw_sec 基点后的当前增量；seq 验证；@ts 是调用者输出。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;
	u64 nsecs;

	do {
		seq = read_seqcount_begin(&tk_core.seq);
		ts->tv_sec = tk->raw_sec;
		nsecs = timekeeping_get_ns(&tk->tkr_raw);

	} while (read_seqcount_retry(&tk_core.seq, seq));

	ts->tv_nsec = 0;
	timespec64_add_ns(ts, nsecs);
}
EXPORT_SYMBOL(ktime_get_raw_ts64);

/**
 * ktime_get_clock_ts64 - Returns time of a clock in a timespec
 * @id:		POSIX clock ID of the clock to read
 * @ts:		Pointer to the timespec64 to be set
 *
 * The timestamp is invalidated (@ts->sec is set to -1) if the
 * clock @id is not available.
 */
void ktime_get_clock_ts64(clockid_t id, struct timespec64 *ts)
{
	/* 通用分派先把输出置 invalid；不支持/禁用 aux 时调用者不会误用旧栈内容。 */
	/* Invalidate time stamp */
	ts->tv_sec = -1;
	ts->tv_nsec = 0;

	switch (id) {
	case CLOCK_REALTIME:
		ktime_get_real_ts64(ts);
		return;
	case CLOCK_MONOTONIC:
		ktime_get_ts64(ts);
		return;
	case CLOCK_MONOTONIC_RAW:
		ktime_get_raw_ts64(ts);
		return;
	case CLOCK_AUX ... CLOCK_AUX_LAST:
		if (IS_ENABLED(CONFIG_POSIX_AUX_CLOCKS))
			ktime_get_aux_ts64(id, ts);
		return;
	default:
		WARN_ON_ONCE(1);
	}
}
EXPORT_SYMBOL_GPL(ktime_get_clock_ts64);

/**
 * timekeeping_valid_for_hres - Check if timekeeping is suitable for hres
 */
int timekeeping_valid_for_hres(void)
{
	/* clocksource flags 与指针在切换时一起变化，seqcount 保证能力判断属于当前源。 */
	/* ret 保存 CLOCK_SOURCE_VALID_FOR_HRES 位快照；非零即 true 语义。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;
	int ret;

	do {
		seq = read_seqcount_begin(&tk_core.seq);

		ret = tk->tkr_mono.clock->flags & CLOCK_SOURCE_VALID_FOR_HRES;

	} while (read_seqcount_retry(&tk_core.seq, seq));

	return ret;
}

/**
 * timekeeping_max_deferment - Returns max time the clocksource can be deferred
 */
u64 timekeeping_max_deferment(void)
{
	/* max_idle_ns 限定 NO_HZ 最久多久必须再读源，防计数器绕回后无法辨认真实 delta。 */
	/* ret 是纳秒单位的返回快照；seq 保证它与当前 clocksource 对应。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;
	u64 ret;

	do {
		seq = read_seqcount_begin(&tk_core.seq);

		ret = tk->tkr_mono.clock->max_idle_ns;

	} while (read_seqcount_retry(&tk_core.seq, seq));

	return ret;
}

/**
 * read_persistent_clock64 -  Return time from the persistent clock.
 * @ts: Pointer to the storage for the readout value
 *
 * Weak dummy function for arches that do not yet support it.
 * Reads the time from the battery backed persistent clock.
 * Returns a timespec with tv_sec=0 and tv_nsec=0 if unsupported.
 *
 *  XXX - Do be sure to remove it once all arches implement it.
 */
void __weak read_persistent_clock64(struct timespec64 *ts)
{
	/* weak 默认 0 表示“没有可靠持久时钟”；架构强实现可由电池 RTC/固件提供 UTC。 */
	ts->tv_sec = 0;
	ts->tv_nsec = 0;
}

/**
 * read_persistent_wall_and_boot_offset - Read persistent clock, and also offset
 *                                        from the boot.
 * @wall_time:	  current time as returned by persistent clock
 * @boot_offset:  offset that is defined as wall_time - boot_time
 *
 * Weak dummy function for arches that do not yet support it.
 *
 * The default function calculates offset based on the current value of
 * local_clock(). This way architectures that support sched_clock() but don't
 * support dedicated boot time clock will provide the best estimate of the
 * boot time.
 */
void __weak __init
read_persistent_wall_and_boot_offset(struct timespec64 *wall_time,
				     struct timespec64 *boot_offset)
{
	/* local_clock 只能近似开机以来经过时间；架构有跨重启/跨 suspend 来源时应覆盖。 */
	read_persistent_clock64(wall_time);
	*boot_offset = ns_to_timespec64(local_clock());
}

static __init void tkd_basic_setup(struct tk_data *tkd, enum timekeeper_ids tk_id, bool valid)
{
	/* seqcount 绑定刚初始化的 raw lock；正式和 shadow 必须从同一身份/有效位起步。 */
	/* @tkd 是待初始化槽；@tk_id 是稳定身份；@valid 决定该槽能否立即被读者使用。 */
	raw_spin_lock_init(&tkd->lock);
	seqcount_raw_spinlock_init(&tkd->seq, &tkd->lock);
	tkd->timekeeper.id = tkd->shadow_timekeeper.id = tk_id;
	tkd->timekeeper.clock_valid = tkd->shadow_timekeeper.clock_valid = valid;
}

/*
 * Flag reflecting whether timekeeping_resume() has injected sleeptime.
 *
 * The flag starts of false and is only set when a suspend reaches
 * timekeeping_suspend(), timekeeping_resume() sets it to false when the
 * timekeeper clocksource is not stopping across suspend and has been
 * used to update sleep time. If the timekeeper clocksource has stopped
 * then the flag stays true and is used by the RTC resume code to decide
 * whether sleeptime must be injected and if so the flag gets false then.
 *
 * If a suspend fails before reaching timekeeping_resume() then the flag
 * stays false and prevents erroneous sleeptime injection.
 */
static bool suspend_timing_needed;

/* true 表示 timekeeping 尚未用 non-stop/persistent 来源补睡眠，RTC resume 仍需兜底。 */

/* Flag for if there is a persistent clock on this platform */
static bool persistent_clock_exists;
/* 一旦探测到非零持久时钟便保持 true，供后续 suspend 选择无需 IRQ 的睡眠时间来源。 */

/*
 * timekeeping_init - Initializes the clocksource and common timekeeping values
 */
void __init timekeeping_init(void)
{
	/*
	 * 补充说明：启动时还没有可靠 realtime，优先从 persistent clock 取墙钟，并用
	 * boot_offset 建立 wall_to_monotonic，使 monotonic 从启动时刻附近开始且不为负。
	 * 无效/缺失 persistent clock 回退 Unix epoch，但 raw/monotonic 仍从 0 正常推进。
	 *
	 * 选择默认 clocksource 并 enable 后，在 core 锁下初始化 NTP、conversion、xtime、
	 * raw 和 offset，最后一次提交同步 VDSO/fast timekeeper。函数返回后普通时间读取
	 * 才拥有完整基点；初始化失败没有可恢复 errno 路径。
	 */
	/*
	 * 变量地图：wall_time 是持久墙钟；boot_offset 是固件估计的开机后经过时间；
	 * wall_to_mono=boot_offset-wall_time；tks 是 core shadow；clock 是默认硬件源。
	 */
	struct timespec64 wall_time, boot_offset, wall_to_mono;
	struct timekeeper *tks = &tk_core.shadow_timekeeper;
	struct clocksource *clock;

	tkd_basic_setup(&tk_core, TIMEKEEPER_CORE, true);
	tk_aux_setup();

	read_persistent_wall_and_boot_offset(&wall_time, &boot_offset);
	if (timespec64_valid_settod(&wall_time) &&
	    timespec64_to_ns(&wall_time) > 0) {
		persistent_clock_exists = true;
	} else if (timespec64_to_ns(&wall_time) != 0) {
		pr_warn("Persistent clock returned invalid value");
		wall_time = (struct timespec64){0};
	}

	if (timespec64_compare(&wall_time, &boot_offset) < 0)
		boot_offset = (struct timespec64){0};

	/*
	 * We want set wall_to_mono, so the following is true:
	 * wall time + wall_to_mono = boot time
	 */
	wall_to_mono = timespec64_sub(boot_offset, wall_time);

	clock = clocksource_default_clock();
	if (clock->enable)
		clock->enable(clock);

	guard(raw_spinlock_irqsave)(&tk_core.lock);

	ntp_init();

	tk_setup_internals(tks, clock);

	tk_set_xtime(tks, &wall_time);
	tks->raw_sec = 0;

	tk_set_wall_to_mono(tks, wall_to_mono);

	timekeeping_update_from_shadow(&tk_core, TK_CLOCK_WAS_SET);
}

/* time in seconds when suspend began for persistent clock */
static struct timespec64 timekeeping_suspend_time;
/* 保存 suspend 入口的 persistent wall time；resume 与新读数相减得到候选睡眠 delta。 */

/**
 * __timekeeping_inject_sleeptime - Internal function to add sleep interval
 * @tk:		Pointer to the timekeeper to be updated
 * @delta:	Pointer to the delta value in timespec64 format
 *
 * Takes a timespec offset measuring a suspend interval and properly
 * adds the sleep offset to the timekeeping variables.
 */
static void __timekeeping_inject_sleeptime(struct timekeeper *tk,
					   const struct timespec64 *delta)
{
	/*
	 * suspend 期间 realtime 与 boottime 应继续前进，而 monotonic 应停住：xtime 加 delta，
	 * wall_to_monotonic 减 delta 抵消 monotonic，offs_boot 加 delta 恢复 boottime。
	 */
	if (!timespec64_valid_strict(delta)) {
		printk_deferred(KERN_WARNING
				"__timekeeping_inject_sleeptime: Invalid "
				"sleep delta value!\n");
		return;
	}
	tk_xtime_add(tk, delta);
	tk_set_wall_to_mono(tk, timespec64_sub(tk->wall_to_monotonic, *delta));
	tk_update_sleep_time(tk, timespec64_to_ktime(*delta));
	tk_debug_account_sleep_time(delta);
}

#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_RTC_HCTOSYS_DEVICE)
/*
 * We have three kinds of time sources to use for sleep time
 * injection, the preference order is:
 * 1) non-stop clocksource
 * 2) persistent clock (ie: RTC accessible when irqs are off)
 * 3) RTC
 *
 * 1) and 2) are used by timekeeping, 3) by RTC subsystem.
 * If system has neither 1) nor 2), 3) will be used finally.
 *
 *
 * If timekeeping has injected sleeptime via either 1) or 2),
 * 3) becomes needless, so in this case we don't need to call
 * rtc_resume(), and this is what timekeeping_rtc_skipresume()
 * means.
 */
bool timekeeping_rtc_skipresume(void)
{
	/* timekeeping 已用更优来源注入睡眠时返回 true，防 RTC core 再注入一次。 */
	return !suspend_timing_needed;
}

/*
 * 1) can be determined whether to use or not only when doing
 * timekeeping_resume() which is invoked after rtc_suspend(),
 * so we can't skip rtc_suspend() surely if system has 1).
 *
 * But if system has 2), 2) will definitely be used, so in this
 * case we don't need to call rtc_suspend(), and this is what
 * timekeeping_rtc_skipsuspend() means.
 */
bool timekeeping_rtc_skipsuspend(void)
{
	/* persistent clock 可在 IRQ 关闭阶段读取时，无需 RTC 子系统另存一份 suspend 基点。 */
	return persistent_clock_exists;
}

/**
 * timekeeping_inject_sleeptime64 - Adds suspend interval to timeekeeping values
 * @delta: pointer to a timespec64 delta value
 *
 * This hook is for architectures that cannot support read_persistent_clock64
 * because their RTC/persistent clock is only accessible when irqs are enabled.
 * and also don't have an effective nonstop clocksource.
 *
 * This function should only be called by rtc_resume(), and allows
 * a suspend offset to be injected into the timekeeping values.
 */
void timekeeping_inject_sleeptime64(const struct timespec64 *delta)
{
	/* RTC fallback 在 IRQ 已可用时调用；成功注入后清 needed，防后续来源重复计算睡眠。 */
	scoped_guard(raw_spinlock_irqsave, &tk_core.lock) {
		struct timekeeper *tks = &tk_core.shadow_timekeeper;

		suspend_timing_needed = false;
		timekeeping_forward_now(tks);
		__timekeeping_inject_sleeptime(tks, delta);
		timekeeping_update_from_shadow(&tk_core, TK_UPDATE_ALL);
	}

	/* Signal hrtimers about time change */
	clock_was_set(CLOCK_SET_WALL | CLOCK_SET_BOOT);
}
#endif

/**
 * timekeeping_resume - Resumes the generic timekeeping subsystem.
 */
void timekeeping_resume(void)
{
	/*
	 * 补充说明：恢复按精度/可靠性优先选择 non-stop clocksource、persistent clock，最后
	 * 留给 RTC core。锁内只接受正 delta，注入一次后重置 cycle_last，避免恢复后把
	 * suspend 周期再次当运行时间累计。清 suspended 并发布真实 fast base 后才恢复
	 * tick/timerfd；timerfd 把 resume 当作 wall/boot clock 发生跳变处理。
	 */
	/*
	 * 变量地图：tks/clock 是恢复中的 shadow 与 active 源；ts_new 是恢复后 persistent
	 * 读数，ts_delta 是选中的睡眠量；cycle_now/nsec 是 non-stop 源读数及推导纳秒；
	 * inject_sleeptime 标记是否已有可靠来源；flags 保存 IRQ 状态供配对恢复。
	 */
	struct timekeeper *tks = &tk_core.shadow_timekeeper;
	struct clocksource *clock = tks->tkr_mono.clock;
	struct timespec64 ts_new, ts_delta;
	bool inject_sleeptime = false;
	u64 cycle_now, nsec;
	unsigned long flags;

	read_persistent_clock64(&ts_new);

	clockevents_resume();
	clocksource_resume();

	raw_spin_lock_irqsave(&tk_core.lock, flags);

	/*
	 * After system resumes, we need to calculate the suspended time and
	 * compensate it for the OS time. There are 3 sources that could be
	 * used: Nonstop clocksource during suspend, persistent clock and rtc
	 * device.
	 *
	 * One specific platform may have 1 or 2 or all of them, and the
	 * preference will be:
	 *	suspend-nonstop clocksource -> persistent clock -> rtc
	 * The less preferred source will only be tried if there is no better
	 * usable source. The rtc part is handled separately in rtc core code.
	 */
	cycle_now = tk_clock_read(&tks->tkr_mono);
	nsec = clocksource_stop_suspend_timing(clock, cycle_now);
	if (nsec > 0) {
		ts_delta = ns_to_timespec64(nsec);
		inject_sleeptime = true;
	} else if (timespec64_compare(&ts_new, &timekeeping_suspend_time) > 0) {
		ts_delta = timespec64_sub(ts_new, timekeeping_suspend_time);
		inject_sleeptime = true;
	}

	if (inject_sleeptime) {
		suspend_timing_needed = false;
		__timekeeping_inject_sleeptime(tks, &ts_delta);
	}

	/* Re-base the last cycle value */
	tks->tkr_mono.cycle_last = cycle_now;
	tks->tkr_raw.cycle_last  = cycle_now;

	tks->ntp_error = 0;
	timekeeping_suspended = 0;
	timekeeping_update_from_shadow(&tk_core, TK_CLOCK_WAS_SET);
	raw_spin_unlock_irqrestore(&tk_core.lock, flags);

	touch_softlockup_watchdog();

	/* Resume the clockevent device(s) and hrtimers */
	tick_resume();
	/* Notify timerfd as resume is equivalent to clock_was_set() */
	timerfd_resume();
}

static void timekeeping_syscore_resume(void *data)
{
	/* syscore 回调签名适配层；@data 未使用，实际状态全部属于全局 core timekeeper。 */
	timekeeping_resume();
}

int timekeeping_suspend(void)
{
	/*
	 * 补充说明：syscore suspend 的晚期入口。先采 persistent clock，再在 core 锁内把
	 * 当前运行时间结算并置 suspended；保存 cycle_last 给 non-stop 源测睡眠，发布冻结
	 * 状态并把 NMI fast reader 切到 dummy clock。出锁后按 tick -> clocksource ->
	 * clockevent 顺序停设备。
	 *
	 * old_delta 记录系统墙钟与 persistent clock 的长期差；用相邻 suspend 的差值补偿
	 * 秒级读取取整误差，但若差值突变 >=2 秒则视为人工/NTP 校时，重新建立基准。
	 */
	/*
	 * 变量地图：tks 是持锁 shadow；delta 是 system-persistent 差，old_delta 跨电源周期
	 * 保存上次差值，delta_delta 是漂移变化；curr_clock/cycle_now 建立 non-stop 测量
	 * 起点；flags 保存 IRQ；timekeeping_suspend_time 是本次持久时钟基点。
	 */
	struct timekeeper *tks = &tk_core.shadow_timekeeper;
	struct timespec64 delta, delta_delta;
	static struct timespec64 old_delta;
	struct clocksource *curr_clock;
	unsigned long flags;
	u64 cycle_now;

	read_persistent_clock64(&timekeeping_suspend_time);

	/*
	 * On some systems the persistent_clock can not be detected at
	 * timekeeping_init by its return value, so if we see a valid
	 * value returned, update the persistent_clock_exists flag.
	 */
	if (timekeeping_suspend_time.tv_sec || timekeeping_suspend_time.tv_nsec)
		persistent_clock_exists = true;

	suspend_timing_needed = true;

	raw_spin_lock_irqsave(&tk_core.lock, flags);
	timekeeping_forward_now(tks);
	timekeeping_suspended = 1;

	/*
	 * Since we've called forward_now, cycle_last stores the value
	 * just read from the current clocksource. Save this to potentially
	 * use in suspend timing.
	 */
	curr_clock = tks->tkr_mono.clock;
	cycle_now = tks->tkr_mono.cycle_last;
	clocksource_start_suspend_timing(curr_clock, cycle_now);

	if (persistent_clock_exists) {
		/*
		 * To avoid drift caused by repeated suspend/resumes,
		 * which each can add ~1 second drift error,
		 * try to compensate so the difference in system time
		 * and persistent_clock time stays close to constant.
		 */
		delta = timespec64_sub(tk_xtime(tks), timekeeping_suspend_time);
		delta_delta = timespec64_sub(delta, old_delta);
		if (abs(delta_delta.tv_sec) >= 2) {
			/*
			 * if delta_delta is too large, assume time correction
			 * has occurred and set old_delta to the current delta.
			 */
			old_delta = delta;
		} else {
			/* Otherwise try to adjust old_system to compensate */
			timekeeping_suspend_time =
				timespec64_add(timekeeping_suspend_time, delta_delta);
		}
	}

	timekeeping_update_from_shadow(&tk_core, 0);
	halt_fast_timekeeper(tks);
	raw_spin_unlock_irqrestore(&tk_core.lock, flags);

	tick_suspend();
	clocksource_suspend();
	clockevents_suspend();

	return 0;
}

static int timekeeping_syscore_suspend(void *data)
{
	/* 把 timekeeping_suspend 的错误码直接交给 syscore，失败可中止系统 suspend。 */
	return timekeeping_suspend();
}

/* sysfs resume/suspend bits for timekeeping */
static const struct syscore_ops timekeeping_syscore_ops = {
	.resume		= timekeeping_syscore_resume,
	.suspend	= timekeeping_syscore_suspend,
};
/* ops 表描述 timekeeping 在 syscore 阶段的 suspend/resume 两个回调入口。 */

static struct syscore timekeeping_syscore = {
	.ops = &timekeeping_syscore_ops,
};
/* 注册对象把上述操作表挂入全局 syscore 顺序；静态生命周期覆盖所有电源周期。 */

static int __init timekeeping_init_ops(void)
{
	/* 注册为 syscore，确保设备 suspend 的极晚/恢复的极早阶段仍能维护时间基准。 */
	register_syscore(&timekeeping_syscore);
	return 0;
}
device_initcall(timekeeping_init_ops);

/*
 * Apply a multiplier adjustment to the timekeeper
 */
static __always_inline void timekeeping_apply_adjustment(struct timekeeper *tk,
							 s64 offset,
							 s32 mult_adj)
{
	/*
	 * 补充说明：改变 mult 会改变“尚未累计 offset cycles”的估值。为保证调整瞬间时间
	 * 连续，mult 每增 1，就从 xtime_nsec 减去 offset；同时修正下个固定 interval 的
	 * shifted-ns 预算。这里调斜率而不直接跳墙钟，是 NTP 平滑校频的核心。
	 */
	/* @offset 是未累计 cycles；@mult_adj 是斜率量化步数；interval 是相应 shifted-ns 修正。 */
	s64 interval = tk->cycle_interval;

	if (mult_adj == 0) {
		return;
	} else if (mult_adj == -1) {
		interval = -interval;
		offset = -offset;
	} else if (mult_adj != 1) {
		interval *= mult_adj;
		offset *= mult_adj;
	}

	/*
	 * So the following can be confusing.
	 *
	 * To keep things simple, lets assume mult_adj == 1 for now.
	 *
	 * When mult_adj != 1, remember that the interval and offset values
	 * have been appropriately scaled so the math is the same.
	 *
	 * The basic idea here is that we're increasing the multiplier
	 * by one, this causes the xtime_interval to be incremented by
	 * one cycle_interval. This is because:
	 *	xtime_interval = cycle_interval * mult
	 * So if mult is being incremented by one:
	 *	xtime_interval = cycle_interval * (mult + 1)
	 * Its the same as:
	 *	xtime_interval = (cycle_interval * mult) + cycle_interval
	 * Which can be shortened to:
	 *	xtime_interval += cycle_interval
	 *
	 * So offset stores the non-accumulated cycles. Thus the current
	 * time (in shifted nanoseconds) is:
	 *	now = (offset * adj) + xtime_nsec
	 * Now, even though we're adjusting the clock frequency, we have
	 * to keep time consistent. In other words, we can't jump back
	 * in time, and we also want to avoid jumping forward in time.
	 *
	 * So given the same offset value, we need the time to be the same
	 * both before and after the freq adjustment.
	 *	now = (offset * adj_1) + xtime_nsec_1
	 *	now = (offset * adj_2) + xtime_nsec_2
	 * So:
	 *	(offset * adj_1) + xtime_nsec_1 =
	 *		(offset * adj_2) + xtime_nsec_2
	 * And we know:
	 *	adj_2 = adj_1 + 1
	 * So:
	 *	(offset * adj_1) + xtime_nsec_1 =
	 *		(offset * (adj_1+1)) + xtime_nsec_2
	 *	(offset * adj_1) + xtime_nsec_1 =
	 *		(offset * adj_1) + offset + xtime_nsec_2
	 * Canceling the sides:
	 *	xtime_nsec_1 = offset + xtime_nsec_2
	 * Which gives us:
	 *	xtime_nsec_2 = xtime_nsec_1 - offset
	 * Which simplifies to:
	 *	xtime_nsec -= offset
	 */
	if ((mult_adj > 0) && (tk->tkr_mono.mult + mult_adj < mult_adj)) {
		/* NTP adjustment caused clocksource mult overflow */
		WARN_ON_ONCE(1);
		return;
	}

	tk->tkr_mono.mult += mult_adj;
	tk->xtime_interval += interval;
	tk->tkr_mono.xtime_nsec -= offset;
}

/*
 * Adjust the timekeeper's multiplier to the correct frequency
 * and also to reduce the accumulated error value.
 */
static void timekeeping_adjust(struct timekeeper *tk, s64 offset)
{
	/*
	 * 根据 NTP 给出的目标 tick 长度重算理想 mult，并用 ntp_error 的符号决定是否再加
	 * 一个最小量化单位偿还余差。maxadj 告警表示校正已超出 clocksource 声明安全范围；
	 * xtime_nsec 被连续性补偿减成负数时借前一秒，并令 next second_overflow 跳过一次，
	 * 避免闰秒/NTP 秒级状态被重复推进。
	 */
	/* ntp_tl 是 NTP 目标 tick 长度；mult 是由它和累计 error 推出的新 mono 斜率。 */
	u64 ntp_tl = ntp_tick_length(tk->id);
	u32 mult;

	/*
	 * Determine the multiplier from the current NTP tick length.
	 * Avoid expensive division when the tick length doesn't change.
	 */
	if (likely(tk->ntp_tick == ntp_tl)) {
		mult = tk->tkr_mono.mult - tk->ntp_err_mult;
	} else {
		tk->ntp_tick = ntp_tl;
		mult = div64_u64((tk->ntp_tick >> tk->ntp_error_shift) -
				 tk->xtime_remainder, tk->cycle_interval);
	}

	/*
	 * If the clock is behind the NTP time, increase the multiplier by 1
	 * to catch up with it. If it's ahead and there was a remainder in the
	 * tick division, the clock will slow down. Otherwise it will stay
	 * ahead until the tick length changes to a non-divisible value.
	 */
	tk->ntp_err_mult = tk->ntp_error > 0 ? 1 : 0;
	mult += tk->ntp_err_mult;

	timekeeping_apply_adjustment(tk, offset, mult - tk->tkr_mono.mult);

	if (unlikely(tk->tkr_mono.clock->maxadj &&
		(abs(tk->tkr_mono.mult - tk->tkr_mono.clock->mult)
			> tk->tkr_mono.clock->maxadj))) {
		printk_once(KERN_WARNING
			"Adjusting %s more than 11%% (%ld vs %ld)\n",
			tk->tkr_mono.clock->name, (long)tk->tkr_mono.mult,
			(long)tk->tkr_mono.clock->mult + tk->tkr_mono.clock->maxadj);
	}

	/*
	 * It may be possible that when we entered this function, xtime_nsec
	 * was very small.  Further, if we're slightly speeding the clocksource
	 * in the code above, its possible the required corrective factor to
	 * xtime_nsec could cause it to underflow.
	 *
	 * Now, since we have already accumulated the second and the NTP
	 * subsystem has been notified via second_overflow(), we need to skip
	 * the next update.
	 */
	if (unlikely((s64)tk->tkr_mono.xtime_nsec < 0)) {
		tk->tkr_mono.xtime_nsec += (u64)NSEC_PER_SEC <<
							tk->tkr_mono.shift;
		tk->xtime_sec--;
		tk->skip_second_overflow = 1;
	}
}

/*
 * accumulate_nsecs_to_secs - Accumulates nsecs into secs
 *
 * Helper function that accumulates the nsecs greater than a second
 * from the xtime_nsec field to the xtime_secs field.
 * It also calls into the NTP code to handle leapsecond processing.
 */
static inline unsigned int accumulate_nsecs_to_secs(struct timekeeper *tk)
{
	/*
	 * 补充说明：把 shifted-ns 的整秒进位到 xtime_sec，并在每个真实跨秒点调用 NTP
	 * second_overflow 处理状态机/闰秒。闰秒改变 realtime 秒数时反向修正 mono offset，
	 * 所以 monotonic 不跳；TAI offset 同步改变以维持其连续原子时间语义。
	 */
	/* nsecps 是内部定点格式的一秒；clock_set 累计闰秒导致的发布动作位。 */
	u64 nsecps = (u64)NSEC_PER_SEC << tk->tkr_mono.shift;
	unsigned int clock_set = 0;

	while (tk->tkr_mono.xtime_nsec >= nsecps) {
		/* leap 是 second_overflow 返回的 -1/0/+1 秒修正。 */
		int leap;

		tk->tkr_mono.xtime_nsec -= nsecps;
		tk->xtime_sec++;

		/*
		 * Skip NTP update if this second was accumulated before,
		 * i.e. xtime_nsec underflowed in timekeeping_adjust()
		 */
		if (unlikely(tk->skip_second_overflow)) {
			tk->skip_second_overflow = 0;
			continue;
		}

		/* Figure out if its a leap sec and apply if needed */
		leap = second_overflow(tk->id, tk->xtime_sec);
		if (unlikely(leap)) {
			struct timespec64 ts;

			tk->xtime_sec += leap;

			ts.tv_sec = leap;
			ts.tv_nsec = 0;
			tk_set_wall_to_mono(tk,
				timespec64_sub(tk->wall_to_monotonic, ts));

			__timekeeping_set_tai_offset(tk, tk->tai_offset - leap);

			clock_set = TK_CLOCK_WAS_SET;
		}
	}
	return clock_set;
}

/*
 * logarithmic_accumulation - shifted accumulation of cycles
 *
 * This functions accumulates a shifted interval of cycles into
 * a shifted interval nanoseconds. Allows for O(log) accumulation
 * loop.
 *
 * Returns the unconsumed cycles.
 */
static u64 logarithmic_accumulation(struct timekeeper *tk, u64 offset,
				    u32 shift, unsigned int *clock_set)
{
	/*
	 * 补充说明：NO_HZ 后可能一次积压成千上万个 interval。@shift 表示一次吞掉
	 * cycle_interval*2^shift，像二进制分解一样把 O(n) tick 循环降为 O(log n)。mono、
	 * raw、cycle_last 和 NTP error 必须消费同一块 cycles，否则时间域会逐步失配。
	 */
	/* interval 是本轮消费 cycles；snsec_per_sec 是 raw 定点格式一秒；offset 是剩余输入输出。 */
	u64 interval = tk->cycle_interval << shift;
	u64 snsec_per_sec;

	/* If the offset is smaller than a shifted interval, do nothing */
	if (offset < interval)
		return offset;

	/* Accumulate one shifted interval */
	offset -= interval;
	tk->tkr_mono.cycle_last += interval;
	tk->tkr_raw.cycle_last  += interval;

	tk->tkr_mono.xtime_nsec += tk->xtime_interval << shift;
	*clock_set |= accumulate_nsecs_to_secs(tk);

	/* Accumulate raw time */
	tk->tkr_raw.xtime_nsec += tk->raw_interval << shift;
	snsec_per_sec = (u64)NSEC_PER_SEC << tk->tkr_raw.shift;
	while (tk->tkr_raw.xtime_nsec >= snsec_per_sec) {
		tk->tkr_raw.xtime_nsec -= snsec_per_sec;
		tk->raw_sec++;
	}

	/* Accumulate error between NTP and clock interval */
	tk->ntp_error += tk->ntp_tick << shift;
	tk->ntp_error -= (tk->xtime_interval + tk->xtime_remainder) <<
						(tk->ntp_error_shift + shift);

	return offset;
}

/*
 * timekeeping_advance - Updates the timekeeper to the current time and
 * current NTP tick length
 */
static bool __timekeeping_advance(struct tk_data *tkd, enum timekeeping_adv_mode mode)
{
	/*
	 * 补充说明：入口持有 tkd->lock，shadow 已从上次提交继续维护。TK_ADV_TICK 在不足
	 * 一个 interval 时可直接跳过；TK_ADV_FREQ 即使没有足够 cycles 也要立即重算 mult。
	 * 流程是：读取 offset -> 对数累计完整 intervals -> 用残余 offset 调整 NTP 斜率 ->
	 * 规范化秒/coarse -> 原子提交。返回 true 仅表示闰秒等需要 clock_was_set 通知，
	 * 不表示“时间是否推进”。
	 */
	/*
	 * 变量地图：tk 是演算 shadow，real_tk 只供 tick 快速阈值；offset 是未消费 cycles，
	 * orig_offset 用来判断是否真正累计；shift/maxshift 控制二进制块大小；clock_set 是
	 * 闰秒等动作位；@mode 区分 tick 与强制调频。
	 */
	struct timekeeper *tk = &tkd->shadow_timekeeper;
	struct timekeeper *real_tk = &tkd->timekeeper;
	unsigned int clock_set = 0;
	int shift = 0, maxshift;
	u64 offset, orig_offset;

	/* Make sure we're fully resumed: */
	if (unlikely(timekeeping_suspended))
		return false;

	offset = clocksource_delta(tk_clock_read(&tk->tkr_mono),
				   tk->tkr_mono.cycle_last, tk->tkr_mono.mask,
				   tk->tkr_mono.clock->max_raw_delta);
	orig_offset = offset;
	/* Check if there's really nothing to do */
	if (offset < real_tk->cycle_interval && mode == TK_ADV_TICK)
		return false;

	/*
	 * With NO_HZ we may have to accumulate many cycle_intervals
	 * (think "ticks") worth of time at once. To do this efficiently,
	 * we calculate the largest doubling multiple of cycle_intervals
	 * that is smaller than the offset.  We then accumulate that
	 * chunk in one go, and then try to consume the next smaller
	 * doubled multiple.
	 */
	shift = ilog2(offset) - ilog2(tk->cycle_interval);
	shift = max(0, shift);
	/* Bound shift to one less than what overflows tick_length */
	maxshift = (64 - (ilog2(ntp_tick_length(tk->id)) + 1)) - 1;
	shift = min(shift, maxshift);
	while (offset >= tk->cycle_interval) {
		offset = logarithmic_accumulation(tk, offset, shift, &clock_set);
		if (offset < tk->cycle_interval<<shift)
			shift--;
	}

	/* Adjust the multiplier to correct NTP error */
	timekeeping_adjust(tk, offset);

	/*
	 * Finally, make sure that after the rounding
	 * xtime_nsec isn't larger than NSEC_PER_SEC
	 */
	clock_set |= accumulate_nsecs_to_secs(tk);

	/*
	 * To avoid inconsistencies caused adjtimex TK_ADV_FREQ calls
	 * making small negative adjustments to the base xtime_nsec
	 * value, only update the coarse clocks if we accumulated time
	 */
	if (orig_offset != offset)
		tk_update_coarse_nsecs(tk);

	timekeeping_update_from_shadow(tkd, clock_set);

	return !!clock_set;
}

static bool timekeeping_advance(enum timekeeping_adv_mode mode)
{
	/* guard 自动 irqsave/解锁；bool 结果直接传给 tick 入口决定是否广播 clock-set。 */
	guard(raw_spinlock_irqsave)(&tk_core.lock);
	return __timekeeping_advance(&tk_core, mode);
}

/**
 * update_wall_time - Uses the current clocksource to increment the wall time
 *
 * It also updates the enabled auxiliary clock timekeepers
 */
void update_wall_time(void)
{
	/* tick 主入口：core 写事务后再推进已启用 aux；通知延后，避免在 IRQ 热路径同步广播。 */
	if (timekeeping_advance(TK_ADV_TICK))
		clock_was_set_delayed();
	tk_aux_advance();
}

/**
 * getboottime64 - Return the real time of system boot.
 * @ts:		pointer to the timespec64 to be set
 *
 * Returns the wall-time of boot in a timespec64.
 *
 * This is based on the wall_to_monotonic offset and the total suspend
 * time. Calls to settimeofday will affect the value returned (which
 * basically means that however wrong your real time clock is at boot time,
 * you get the right time here).
 */
void getboottime64(struct timespec64 *ts)
{
	/* boot 的 realtime 时刻 = offs_real - offs_boot；settimeofday 会重估它，suspend 不会。 */
	/* t 是该差值的 ktime 临时量，随后转换到调用者 @ts。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	ktime_t t = ktime_sub(tk->offs_real, tk->offs_boot);

	*ts = ktime_to_timespec64(t);
}
EXPORT_SYMBOL_GPL(getboottime64);

void ktime_get_coarse_real_ts64(struct timespec64 *ts)
{
	/* tk 是正式 core；seq 验证 coarse 秒/纳秒成对；@ts 是输出。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;

	do {
		seq = read_seqcount_begin(&tk_core.seq);

		*ts = tk_xtime_coarse(tk);
	} while (read_seqcount_retry(&tk_core.seq, seq));
}
EXPORT_SYMBOL(ktime_get_coarse_real_ts64);

/**
 * ktime_get_coarse_real_ts64_mg - return latter of coarse grained time or floor
 * @ts:		timespec64 to be filled
 *
 * Fetch the global mg_floor value, convert it to realtime and compare it
 * to the current coarse-grained time. Fill @ts with whichever is
 * latest. Note that this is a filesystem-specific interface and should be
 * avoided outside of that context.
 */
void ktime_get_coarse_real_ts64_mg(struct timespec64 *ts)
{
	/*
	 * multigrain 文件时间不能比本机此前发出的 fine timestamp 更早。mg_floor 保存在
	 * monotonic 域，读取时用同一 seqcount 快照的 offs_real 转 realtime，再与 coarse
	 * 取较晚者；墙钟向后拨是文档明确允许的例外。
	 */
	/* floor 是 mono 原子下限；offset 转 realtime；coarse/f_real 是两个候选；seq 验证 offset。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	u64 floor = atomic64_read(&mg_floor);
	ktime_t f_real, offset, coarse;
	unsigned int seq;

	do {
		seq = read_seqcount_begin(&tk_core.seq);
		*ts = tk_xtime_coarse(tk);
		offset = tk_core.timekeeper.offs_real;
	} while (read_seqcount_retry(&tk_core.seq, seq));

	coarse = timespec64_to_ktime(*ts);
	f_real = ktime_add(floor, offset);
	if (ktime_after(f_real, coarse))
		*ts = ktime_to_timespec64(f_real);
}

/**
 * ktime_get_real_ts64_mg - attempt to update floor value and return result
 * @ts:		pointer to the timespec to be set
 *
 * Get a monotonic fine-grained time value and attempt to swap it into
 * mg_floor. If that succeeds then accept the new floor value. If it fails
 * then another task raced in during the interim time and updated the
 * floor.  Since any update to the floor must be later than the previous
 * floor, either outcome is acceptable.
 *
 * Typically this will be called after calling ktime_get_coarse_real_ts64_mg(),
 * and determining that the resulting coarse-grained timestamp did not effect
 * a change in ctime. Any more recent floor value would effect a change to
 * ctime, so there is no need to retry the atomic64_try_cmpxchg() on failure.
 *
 * @ts will be filled with the latest floor value, regardless of the outcome of
 * the cmpxchg. Note that this is a filesystem specific interface and should be
 * avoided outside of that context.
 */
void ktime_get_real_ts64_mg(struct timespec64 *ts)
{
	/*
	 * fine reader尝试把本次 mono 写成全局 floor。cmpxchg 失败会把 @old 更新成胜者的
	 * 新值；所有合法写入都递增 floor，因此无需循环，采用胜者值同样足以推动 ctime。
	 */
	/* old 是 cmpxchg 的期望/失败输出；mono 是本次 fine 值；offset 用于把胜者转 realtime。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	ktime_t old = atomic64_read(&mg_floor);
	ktime_t offset, mono;
	unsigned int seq;
	u64 nsecs;

	do {
		seq = read_seqcount_begin(&tk_core.seq);

		ts->tv_sec = tk->xtime_sec;
		mono = tk->tkr_mono.base;
		nsecs = timekeeping_get_ns(&tk->tkr_mono);
		offset = tk_core.timekeeper.offs_real;
	} while (read_seqcount_retry(&tk_core.seq, seq));

	mono = ktime_add_ns(mono, nsecs);

	/*
	 * Attempt to update the floor with the new time value. As any
	 * update must be later then the existing floor, and would effect
	 * a change to ctime from the perspective of the current task,
	 * accept the resulting floor value regardless of the outcome of
	 * the swap.
	 */
	if (atomic64_try_cmpxchg(&mg_floor, &old, mono)) {
		ts->tv_nsec = 0;
		timespec64_add_ns(ts, nsecs);
		timekeeping_inc_mg_floor_swaps();
	} else {
		/*
		 * Another task changed mg_floor since "old" was fetched.
		 * "old" has been updated with the latest value of "mg_floor".
		 * That value is newer than the previous floor value, which
		 * is enough to effect a change to ctime. Accept it.
		 */
		*ts = ktime_to_timespec64(ktime_add(old, offset));
	}
}

void ktime_get_coarse_ts64(struct timespec64 *ts)
{
	/* coarse realtime + wall_to_monotonic 得到 coarse monotonic；seqcount 防设时混合。 */
	/* now 是 coarse realtime，mono 是 wall_to_monotonic，二者规范化相加写入 @ts。 */
	struct timekeeper *tk = &tk_core.timekeeper;
	struct timespec64 now, mono;
	unsigned int seq;

	do {
		seq = read_seqcount_begin(&tk_core.seq);

		now = tk_xtime_coarse(tk);
		mono = tk->wall_to_monotonic;
	} while (read_seqcount_retry(&tk_core.seq, seq));

	set_normalized_timespec64(ts, now.tv_sec + mono.tv_sec,
				  now.tv_nsec + mono.tv_nsec);
}
EXPORT_SYMBOL(ktime_get_coarse_ts64);

/*
 * Must hold jiffies_lock
 */
void do_timer(unsigned long ticks)
{
	/* jiffies_lock 由调用者持有；这里推进全局 tick 计数并触发基于它的负载采样。 */
	jiffies_64 += ticks;
	calc_global_load();
}

/**
 * ktime_get_update_offsets_now - hrtimer helper
 * @cwsseq:	pointer to check and store the clock was set sequence number
 * @offs_real:	pointer to storage for monotonic -> realtime offset
 * @offs_boot:	pointer to storage for monotonic -> boottime offset
 * @offs_tai:	pointer to storage for monotonic -> clock tai offset
 *
 * Returns current monotonic time and updates the offsets if the
 * sequence number in @cwsseq and timekeeper.clock_was_set_seq are
 * different.
 *
 * Called from hrtimer_interrupt() or retrigger_next_event()
 */
ktime_t ktime_get_update_offsets_now(unsigned int *cwsseq, ktime_t *offs_real,
				     ktime_t *offs_boot, ktime_t *offs_tai)
{
	/*
	 * 补充说明：hrtimer 缓存三种 offset，并用 clock_was_set_seq 判断何时刷新，避免每次
	 * 中断复制。当前 monotonic 与 offset 必须来自同一 seqcount 版本。若已跨入待插入
	 * 闰秒但 timekeeper 尚未正式推进，临时把 realtime offset 减 1 秒，使到期重编程
	 * 采用即将生效的墙钟关系。
	 */
	/*
	 * tk 是 core；seq 验证；base/nsecs 构造当前 mono；@cwsseq 是调用者缓存版本；三个
	 * offs_* 是仅在版本变化时刷新的输出缓存，均由调用者提供有效存储。
	 */
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;
	ktime_t base;
	u64 nsecs;

	do {
		seq = read_seqcount_begin(&tk_core.seq);

		base = tk->tkr_mono.base;
		nsecs = timekeeping_get_ns(&tk->tkr_mono);
		base = ktime_add_ns(base, nsecs);

		if (*cwsseq != tk->clock_was_set_seq) {
			*cwsseq = tk->clock_was_set_seq;
			*offs_real = tk->offs_real;
			*offs_boot = tk->offs_boot;
			*offs_tai = tk->offs_tai;
		}

		/* Handle leapsecond insertion adjustments */
		if (unlikely(base >= tk->next_leap_ktime))
			*offs_real = ktime_sub(tk->offs_real, ktime_set(1, 0));

	} while (read_seqcount_retry(&tk_core.seq, seq));

	return base;
}

/*
 * timekeeping_validate_timex - Ensures the timex is ok for use in do_adjtimex
 */
static int timekeeping_validate_timex(const struct __kernel_timex *txc, bool aux_clock)
{
	/*
	 * 在关 IRQ/取写锁前完成纯输入验证和权限检查，缩短全局时间锁临界区。ADJ_ADJTIME
	 * singleshot 不能与普通模式混用；ADJ_SETOFFSET 的子秒字段必须规范化；频率乘法先
	 * 做边界检查。aux 没有闰秒/TAI/PPS 语义，因此显式拒绝而不是静默忽略。
	 */
	if (txc->modes & ADJ_ADJTIME) {
		/* singleshot must not be used with any other mode bits */
		if (!(txc->modes & ADJ_OFFSET_SINGLESHOT))
			return -EINVAL;
		if (!(txc->modes & ADJ_OFFSET_READONLY) &&
		    !capable(CAP_SYS_TIME))
			return -EPERM;
	} else {
		/* In order to modify anything, you gotta be super-user! */
		if (txc->modes && !capable(CAP_SYS_TIME))
			return -EPERM;
		/*
		 * if the quartz is off by more than 10% then
		 * something is VERY wrong!
		 */
		if (txc->modes & ADJ_TICK &&
		    (txc->tick <  900000/USER_HZ ||
		     txc->tick > 1100000/USER_HZ))
			return -EINVAL;
	}

	if (txc->modes & ADJ_SETOFFSET) {
		/* In order to inject time, you gotta be super-user! */
		if (!capable(CAP_SYS_TIME))
			return -EPERM;

		/*
		 * Validate if a timespec/timeval used to inject a time
		 * offset is valid.  Offsets can be positive or negative, so
		 * we don't check tv_sec. The value of the timeval/timespec
		 * is the sum of its fields,but *NOTE*:
		 * The field tv_usec/tv_nsec must always be non-negative and
		 * we can't have more nanoseconds/microseconds than a second.
		 */
		if (txc->time.tv_usec < 0)
			return -EINVAL;

		if (txc->modes & ADJ_NANO) {
			if (txc->time.tv_usec >= NSEC_PER_SEC)
				return -EINVAL;
		} else {
			if (txc->time.tv_usec >= USEC_PER_SEC)
				return -EINVAL;
		}
	}

	/*
	 * Check for potential multiplication overflows that can
	 * only happen on 64-bit systems:
	 */
	if ((txc->modes & ADJ_FREQUENCY) && (BITS_PER_LONG == 64)) {
		if (LLONG_MIN / PPM_SCALE > txc->freq)
			return -EINVAL;
		if (LLONG_MAX / PPM_SCALE < txc->freq)
			return -EINVAL;
	}

	if (aux_clock) {
		/* Auxiliary clocks are similar to TAI and do not have leap seconds */
		if (txc->modes & ADJ_STATUS &&
		    txc->status & (STA_INS | STA_DEL))
			return -EINVAL;

		/* No TAI offset setting */
		if (txc->modes & ADJ_TAI)
			return -EINVAL;

		/* No PPS support either */
		if (txc->modes & ADJ_STATUS &&
		    txc->status & (STA_PPSFREQ | STA_PPSTIME))
			return -EINVAL;
	}

	return 0;
}

/**
 * random_get_entropy_fallback - Returns the raw clock source value,
 * used by random.c for platforms with no valid random_get_entropy().
 */
unsigned long random_get_entropy_fallback(void)
{
	/*
	 * 仅向随机池提供不可预测性候选，不是规范时间读取。suspend 或源未就绪返回 0，避免
	 * 访问掉电设备；READ_ONCE 稳定 owner，但此 fallback 本身不承诺跨切换一致快照。
	 */
	/* tkr/clock 是无锁借用快照；返回值是原始 cycles 截成 unsigned long，不是纳秒。 */
	struct tk_read_base *tkr = &tk_core.timekeeper.tkr_mono;
	struct clocksource *clock = READ_ONCE(tkr->clock);

	if (unlikely(timekeeping_suspended || !clock))
		return 0;
	return clock->read(clock);
}
EXPORT_SYMBOL_GPL(random_get_entropy_fallback);

struct adjtimex_result {
	struct audit_ntp_data	ad;
	struct timespec64	delta;
	bool			clock_set;
};

/*
 * adjtimex_result 是锁内核心向锁外包装层传递的结果包：
 *   ad        NTP 参数变化的审计快照；
 *   delta     ADJ_SETOFFSET 实际注入量，亦供 CMOS timer 策略判断；
 *   clock_set 表示 offset/TAI/频率推进改变了时钟关系，需要广播 clock_was_set。
 * 它由调用者栈上创建，无独立生命周期或引用。
 */

static int __do_adjtimex(struct tk_data *tkd, struct __kernel_timex *txc,
			 struct adjtimex_result *result)
{
	/*
	 * adjtimex 核心事务：锁前验证并采样当前时间，锁内先可选注入 offset，再让 NTP
	 * 子系统更新频率/状态/TAI。TAI 改变需完整提交；否则只同步 leap state。直接设置
	 * frequency/tick 时立即 TK_ADV_FREQ，使新 mult 不必等下一 tick。
	 * result 把 audit/通知所需信息带出锁，避免这些外部路径在 raw spinlock 下执行。
	 */
	/*
	 * 变量地图：tks 是目标 shadow；aux_clock 选择验证规则；ts 是送入 NTP 的当前时间；
	 * orig_tai/tai 比较 TAI 是否改变；ret 传播验证/NTP/注入结果；@result 是锁外输出包。
	 */
	struct timekeeper *tks = &tkd->shadow_timekeeper;
	bool aux_clock = !timekeeper_is_core_tk(tks);
	struct timespec64 ts;
	s32 orig_tai, tai;
	int ret;

	/* Validate the data before disabling interrupts */
	ret = timekeeping_validate_timex(txc, aux_clock);
	if (ret)
		return ret;
	add_device_randomness(txc, sizeof(*txc));

	if (!aux_clock)
		ktime_get_real_ts64(&ts);
	else
		tk_get_aux_ts64(tkd->timekeeper.id, &ts);

	add_device_randomness(&ts, sizeof(ts));

	guard(raw_spinlock_irqsave)(&tkd->lock);

	if (!tks->clock_valid)
		return -ENODEV;

	if (txc->modes & ADJ_SETOFFSET) {
		result->delta.tv_sec  = txc->time.tv_sec;
		result->delta.tv_nsec = txc->time.tv_usec;
		if (!(txc->modes & ADJ_NANO))
			result->delta.tv_nsec *= 1000;
		ret = __timekeeping_inject_offset(tkd, &result->delta);
		if (ret)
			return ret;
		result->clock_set = true;
	}

	orig_tai = tai = tks->tai_offset;
	ret = ntp_adjtimex(tks->id, txc, &ts, &tai, &result->ad);

	if (tai != orig_tai) {
		__timekeeping_set_tai_offset(tks, tai);
		timekeeping_update_from_shadow(tkd, TK_CLOCK_WAS_SET);
		result->clock_set = true;
	} else {
		tk_update_leap_state_all(tkd);
	}

	/* Update the multiplier immediately if frequency was set directly */
	if (txc->modes & (ADJ_FREQUENCY | ADJ_TICK))
		result->clock_set |= __timekeeping_advance(tkd, TK_ADV_FREQ);

	return ret;
}

/**
 * do_adjtimex() - Accessor function to NTP __do_adjtimex function
 * @txc:	Pointer to kernel_timex structure containing NTP parameters
 */
int do_adjtimex(struct __kernel_timex *txc)
{
	/* core 包装在成功后完成 audit、hrtimer/timerfd 通知和 CMOS timer 策略更新。 */
	/* result 零初始化，表示默认无 offset/通知；ret 是最终 syscall 风格状态码。 */
	struct adjtimex_result result = { };
	int ret;

	ret = __do_adjtimex(&tk_core, txc, &result);
	if (ret < 0)
		return ret;

	if (txc->modes & ADJ_SETOFFSET)
		audit_tk_injoffset(result.delta);

	audit_ntp_log(&result.ad);

	if (result.clock_set)
		clock_was_set(CLOCK_SET_WALL);

	ntp_notify_cmos_timer(result.delta.tv_sec != 0);

	return ret;
}

/*
 * Invoked from NTP with the time keeper lock held, so lockless access is
 * fine.
 */
long ktime_get_ntp_seconds(unsigned int id)
{
	/* 调用契约已持对应 timekeeper 锁，故不再套 seqcount；@id 必须是有效 timekeeper。 */
	return timekeeper_data[id].timekeeper.xtime_sec;
}

#ifdef CONFIG_NTP_PPS
/**
 * hardpps() - Accessor function to NTP __hardpps function
 * @phase_ts:	Pointer to timespec64 structure representing phase timestamp
 * @raw_ts:	Pointer to timespec64 structure representing raw timestamp
 */
void hardpps(const struct timespec64 *phase_ts, const struct timespec64 *raw_ts)
{
	/* PPS discipline 与普通 NTP 更新共享状态，必须使用同一 core timekeeper 写锁。 */
	guard(raw_spinlock_irqsave)(&tk_core.lock);
	__hardpps(phase_ts, raw_ts);
}
EXPORT_SYMBOL(hardpps);
#endif /* CONFIG_NTP_PPS */

#ifdef CONFIG_POSIX_AUX_CLOCKS
#include "posix-timers.h"

/*
 * Bitmap for the activated auxiliary timekeepers to allow lockless quick
 * checks in the hot paths without touching extra cache lines. If set, then
 * the state of the corresponding timekeeper has to be re-checked under
 * timekeeper::lock.
 */
static unsigned long aux_timekeepers;

/*
 * 补充说明：bitmap 只是热路径提示，不单独证明某个 aux clock 有效。enable/disable 先
 * 在各自锁内发布完整 timekeeper，再在 mutex 下设置/清除 bit；读取 bitmap 后仍要取
 * aux 锁并复查 clock_valid，因此并发看到旧 bit 最多造成一次多余检查。
 */

/*
 * clockid_to_tkid - 把用户可见 CLOCK_AUX+n 映射到 timekeeper_data 的 AUX 槽位。
 *
 * 这里只做算术平移，不验证范围；调用者必须先通过 clockid_aux_valid()。把验证留在
 * aux_get_tk_data() 可使所有外部入口共享一处边界检查，内部已验证路径保持轻量。
 */
static inline unsigned int clockid_to_tkid(unsigned int id)
{
	return TIMEKEEPER_AUX_FIRST + id - CLOCK_AUX;
}

static inline struct tk_data *aux_get_tk_data(clockid_t id)
{
	/*
	 * @id 是用户可见的 POSIX clockid，按值传入且不转移所有权；返回值是全局
	 * timekeeper_data[] 中借用的永久槽位，不需要 put/free。无效 ID 返回 NULL，
	 * 使后续入口不会用未经验证的下标构造越界指针。
	 */
	if (!clockid_aux_valid(id))
		return NULL;
	return &timekeeper_data[clockid_to_tkid(id)];
}

/* Invoked from timekeeping after a clocksource change */
static void tk_aux_update_clocksource(void)
{
	/*
	 * core 换源后，所有 active aux 必须结算旧源到当前点，再改用 core raw clocksource。
	 * bitmap 快照允许 enable/disable 并发；每个 aux 锁下复查 valid，保证不复活已禁用项。
	 */
	/*
	 * 变量地图：
	 *   active  aux_timekeepers 的一次无锁位图快照；每一位对应 CLOCK_AUX+n，
	 *           只决定是否值得进入该槽检查，不保证槽仍有效。
	 *   id      active 中当前置位的零基位号，也是 AUX 槽相对首槽的下标。
	 *   tkd     当前 aux 的永久容器，持有 seqcount、写锁和正式/影子副本。
	 *   tks     tkd 的影子 timekeeper；只在 tkd->lock 保护下修改，提交后才供读者看见。
	 */
	unsigned long active = READ_ONCE(aux_timekeepers);
	unsigned int id;

	for_each_set_bit(id, &active, BITS_PER_LONG) {
		struct tk_data *tkd = &timekeeper_data[id + TIMEKEEPER_AUX_FIRST];
		struct timekeeper *tks = &tkd->shadow_timekeeper;

		guard(raw_spinlock_irqsave)(&tkd->lock);
		if (!tks->clock_valid)
			continue;

		timekeeping_forward_now(tks);
		tk_setup_internals(tks, tk_core.timekeeper.tkr_raw.clock);
		timekeeping_update_from_shadow(tkd, TK_UPDATE_ALL);
	}
}

static void tk_aux_advance(void)
{
	/* 每次 core tick 只遍历 bitmap 中 active 项；各 aux 有独立锁，互不污染时间状态。 */
	/*
	 * 变量地图：active 是本次 tick 使用的启用位图快照；id 是其中的相对 AUX 编号；
	 * aux_tkd 是对应的永久容器借用指针。bitmap 可在遍历中变旧，所以真正推进前仍在
	 * aux_tkd->lock 下复查 shadow_timekeeper.clock_valid。
	 */
	unsigned long active = READ_ONCE(aux_timekeepers);
	unsigned int id;

	/* Lockless quick check to avoid extra cache lines */
	for_each_set_bit(id, &active, BITS_PER_LONG) {
		struct tk_data *aux_tkd = &timekeeper_data[id + TIMEKEEPER_AUX_FIRST];

		guard(raw_spinlock)(&aux_tkd->lock);
		if (aux_tkd->shadow_timekeeper.clock_valid)
			__timekeeping_advance(aux_tkd, TK_ADV_TICK);
	}
}

/**
 * ktime_get_aux - Get time for a AUX clock
 * @id:	ID of the clock to read (CLOCK_AUX...)
 * @kt:	Pointer to ktime_t to store the time stamp
 *
 * Returns: True if the timestamp is valid, false otherwise
 */
bool ktime_get_aux(clockid_t id, ktime_t *kt)
{
	/*
	 * aux 读取与 core 相同：seqcount 内取 base+offs_aux 和当前 cycles 增量；无效 ID 或
	 * disable 并发使 clock_valid 为 false 时不写输出并返回 false。
	 */
	/*
	 * 变量地图：
	 *   @id     要读取的 CLOCK_AUX+n；必须落在配置支持的 AUX 范围。
	 *   @kt     调用者拥有的输出地址；仅 true 返回时写入有效 ktime_t 纳秒时间戳。
	 *   aux_tkd 全局 AUX 槽的借用指针；NULL 表示 @id 非法。
	 *   aux_tk  正式发布副本的借用指针；字段由 aux_tkd->seq 保护一致读取。
	 *   seq     本轮 seqcount 版本；奇数或重试表示写者在本轮读取期间提交过更新。
	 *   base    已含 offs_aux 的已累计纳秒基点；nsecs 是从 cycle_last 到当前周期的增量。
	 */
	struct tk_data *aux_tkd = aux_get_tk_data(id);
	struct timekeeper *aux_tk;
	unsigned int seq;
	ktime_t base;
	u64 nsecs;

	WARN_ON(timekeeping_suspended);

	if (!aux_tkd)
		return false;

	aux_tk = &aux_tkd->timekeeper;
	do {
		seq = read_seqcount_begin(&aux_tkd->seq);
		if (!aux_tk->clock_valid)
			return false;

		base = ktime_add(aux_tk->tkr_mono.base, aux_tk->offs_aux);
		nsecs = timekeeping_get_ns(&aux_tk->tkr_mono);
	} while (read_seqcount_retry(&aux_tkd->seq, seq));

	*kt = ktime_add_ns(base, nsecs);
	return true;
}
EXPORT_SYMBOL_GPL(ktime_get_aux);

/**
 * ktime_get_aux_ts64 - Get time for a AUX clock
 * @id:	ID of the clock to read (CLOCK_AUX...)
 * @ts:	Pointer to timespec64 to store the time stamp
 *
 * Returns: True if the timestamp is valid, false otherwise
 */
bool ktime_get_aux_ts64(clockid_t id, struct timespec64 *ts)
{
	/*
	 * @id 与 ktime_get_aux() 相同；@ts 是调用者拥有的输出对象，只在 true 返回时写入。
	 * now 是栈上的中间 ktime_t，先承接经 seqcount 验证的纳秒值，再做无状态格式转换；
	 * 这样底层读取失败时不会把半成品写入 @ts。
	 */
	ktime_t now;

	if (!ktime_get_aux(id, &now))
		return false;
	*ts = ktime_to_timespec64(now);
	return true;
}
EXPORT_SYMBOL_GPL(ktime_get_aux_ts64);

static int aux_get_res(clockid_t id, struct timespec64 *tp)
{
	/*
	 * @id 只用于验证请求的 AUX 槽存在；@tp 是 POSIX 层提供的输出对象，成功时写入
	 * 秒/纳秒形式的全局 AUX 分辨率。该接口返回 0 或 -ENODEV，不取得任何引用。
	 */
	if (!clockid_aux_valid(id))
		return -ENODEV;

	tp->tv_sec = aux_clock_resolution_ns() / NSEC_PER_SEC;
	tp->tv_nsec = aux_clock_resolution_ns() % NSEC_PER_SEC;
	return 0;
}

static int aux_get_timespec(clockid_t id, struct timespec64 *tp)
{
	/*
	 * @id 指定 AUX 时钟，@tp 是调用者输出；本包装不拥有二者，只把内部 bool
	 * 有效性契约转换成 POSIX k_clock 所需的 0/-ENODEV。
	 */
	return ktime_get_aux_ts64(id, tp) ? 0 : -ENODEV;
}

static int aux_clock_set(const clockid_t id, const struct timespec64 *tnew)
{
	/*
	 * aux 没有 realtime/monotonic 双时间域，只把目标绝对值编码成 offs_aux：先 forward
	 * 当前 raw-derived base，计算 now，再令 offs_aux=tnew-now。这样设时不需要伪造
	 * xtime/wall_to_monotonic，频率斜率仍可由 clock_adjtime 独立校正。
	 */
	/*
	 * 变量地图：
	 *   @id      目标 CLOCK_AUX+n，不转移所有权。
	 *   @tnew    借用的目标绝对时间；必须是规范化且可用于 settimeofday 的 timespec64。
	 *   aux_tkd  目标永久槽；NULL 表示 ID 无效。
	 *   aux_tks  写侧影子副本，只能在 aux_tkd->lock 下修改。
	 *   nsecs    从最新 cycle_last 换算出的纳秒增量；tnow 是设置前 AUX 当前绝对值。
	 */
	struct tk_data *aux_tkd = aux_get_tk_data(id);
	struct timekeeper *aux_tks;
	ktime_t tnow, nsecs;

	if (!timespec64_valid_settod(tnew))
		return -EINVAL;
	if (!aux_tkd)
		return -ENODEV;

	aux_tks = &aux_tkd->shadow_timekeeper;

	guard(raw_spinlock_irq)(&aux_tkd->lock);
	if (!aux_tks->clock_valid)
		return -ENODEV;

	/* Forward the timekeeper base time */
	timekeeping_forward_now(aux_tks);
	/*
	 * Get the updated base time. tkr_mono.base has not been
	 * updated yet, so do that first. That makes the update
	 * in timekeeping_update_from_shadow() redundant, but
	 * that's harmless. After that @tnow can be calculated
	 * by using tkr_mono::cycle_last, which has been set
	 * by timekeeping_forward_now().
	 */
	tk_update_ktime_data(aux_tks);
	nsecs = timekeeping_cycles_to_ns(&aux_tks->tkr_mono, aux_tks->tkr_mono.cycle_last);
	tnow = ktime_add(aux_tks->tkr_mono.base, nsecs);

	/*
	 * Calculate the new AUX offset as delta to @tnow ("monotonic").
	 * That avoids all the tk::xtime back and forth conversions as
	 * xtime ("realtime") is not applicable for auxiliary clocks and
	 * kept in sync with "monotonic".
	 */
	tk_update_aux_offs(aux_tks, ktime_sub(timespec64_to_ktime(*tnew), tnow));

	timekeeping_update_from_shadow(aux_tkd, TK_UPDATE_ALL);
	return 0;
}

static int aux_clock_adj(const clockid_t id, struct __kernel_timex *txc)
{
	/*
	 * @id 选择 AUX 槽；@txc 是 POSIX 层借入的调频请求，同时作为查询结果输出对象。
	 * aux_tkd 是对应永久槽的借用指针；result 是栈上锁内/锁外结果包，但 AUX 当前没有
	 * hrtimer、RTC 或 audit 消费者，因此调用结束后直接丢弃。验证层会拒绝 AUX 不具备
	 * 的 TAI、闰秒与 PPS 模式。
	 */
	struct tk_data *aux_tkd = aux_get_tk_data(id);
	struct adjtimex_result result = { };

	if (!aux_tkd)
		return -ENODEV;

	/*
	 * @result is ignored for now as there are neither hrtimers nor a
	 * RTC related to auxiliary clocks for now.
	 */
	return __do_adjtimex(aux_tkd, txc, &result);
}

const struct k_clock clock_aux = {
	.clock_getres		= aux_get_res,
	.clock_get_timespec	= aux_get_timespec,
	.clock_set		= aux_clock_set,
	.clock_adj		= aux_clock_adj,
};

/*
 * clock_aux 是 POSIX clock 层为所有 CLOCK_AUX+n 共享的只读操作表：getres 查询统一
 * 分辨率，get_timespec 读取，clock_set 改绝对偏移，clock_adj 调频。具体 AUX 实例不靠
 * 不同操作表区分，而由每次回调收到的 clockid 映射到 timekeeper_data[] 槽；该静态对象
 * 生命周期覆盖整个内核运行期，没有引用计数和销毁路径。
 */

static void aux_clock_enable(clockid_t id)
{
	/*
	 * 锁顺序固定为 core -> aux：先冻结 core clocksource，再嵌套取得 aux 锁，避免换源
	 * 时建立在过期指针上。清除旧注册残留后以 core raw 的未经 NTP 校正斜率启动；用户
	 * 负责通过 clock_adjtime 校准 aux 与外部设备的真实频差。
	 */
	/*
	 * 变量地图：@id 已由 sysfs 路径验证为 CLOCK_AUX+n；tkr_raw 借用 core 正式副本的
	 * raw 换算参数，须由 tk_core.lock 保证换源期间稳定；aux_tkd 是目标永久槽；aux_tks
	 * 是待重建的影子副本。后两者不取得引用，因 timekeeper_data[] 永不释放。
	 */
	struct tk_read_base *tkr_raw = &tk_core.timekeeper.tkr_raw;
	struct tk_data *aux_tkd = aux_get_tk_data(id);
	struct timekeeper *aux_tks = &aux_tkd->shadow_timekeeper;

	/* Prevent the core timekeeper from changing. */
	guard(raw_spinlock_irq)(&tk_core.lock);

	/*
	 * Setup the auxiliary clock assuming that the raw core timekeeper
	 * clock frequency conversion is close enough. Userspace has to
	 * adjust for the deviation via clock_adjtime(2).
	 */
	guard(raw_spinlock_nested)(&aux_tkd->lock);

	/* Remove leftovers of a previous registration */
	memset(aux_tks, 0, sizeof(*aux_tks));
	/* Restore the timekeeper id */
	aux_tks->id = aux_tkd->timekeeper.id;
	/* Setup the timekeeper based on the current system clocksource */
	tk_setup_internals(aux_tks, tkr_raw->clock);

	/* Mark it valid and set it live */
	aux_tks->clock_valid = true;
	timekeeping_update_from_shadow(aux_tkd, TK_UPDATE_ALL);
}

static void aux_clock_disable(clockid_t id)
{
	/*
	 * @id 是已启用的 CLOCK_AUX+n；aux_tkd 是其永久槽借用指针。先在 seqcount 提交中
	 * 发布 invalid，调用者随后清 bitmap，读者观察任一顺序都只能读到旧有效时间或失败。
	 */
	struct tk_data *aux_tkd = aux_get_tk_data(id);

	guard(raw_spinlock_irq)(&aux_tkd->lock);
	aux_tkd->shadow_timekeeper.clock_valid = false;
	timekeeping_update_from_shadow(aux_tkd, TK_UPDATE_ALL);
}

static DEFINE_MUTEX(aux_clock_mutex);
/* aux_clock_mutex 串行化 sysfs enable 状态机及 aux_timekeepers bitmap，不保护时间读数。 */

static ssize_t aux_clock_enable_store(struct kobject *kobj, struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	/*
	 * sysfs 目录名由本文件固定创建为单字符 0..7，因此位与解析成立；CAP_SYS_TIME
	 * 限制时间域创建。mutex 串行化 bitmap 与 enable/disable 状态机，重复写幂等返回。
	 */
	/*
	 * 变量/参数地图：@kobj 是当前数字子目录的借用对象；@attr 指向共享 enable 属性，
	 * 本实现无需读取它；@buf/@count 是 sysfs 借入的用户文本及字节数。id 是由目录名
	 * 解出的零基 AUX 位号；enable 是 kstrtobool 规范化后的目标状态。返回 @count 表示
	 * 整个输入被消费，负 errno 表示权限、格式或设备错误。
	 */
	/* Lazy atoi() as name is "0..7" */
	int id = kobj->name[0] & 0x7;
	bool enable;

	if (!capable(CAP_SYS_TIME))
		return -EPERM;

	if (kstrtobool(buf, &enable) < 0)
		return -EINVAL;

	guard(mutex)(&aux_clock_mutex);
	if (enable == test_bit(id, &aux_timekeepers))
		return count;

	if (enable) {
		aux_clock_enable(CLOCK_AUX + id);
		set_bit(id, &aux_timekeepers);
	} else {
		aux_clock_disable(CLOCK_AUX + id);
		clear_bit(id, &aux_timekeepers);
	}
	return count;
}

static ssize_t aux_clock_enable_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	/*
	 * @kobj 提供数字目录名；@attr 是未使用的共享属性描述；@buf 由 sysfs 拥有并保证
	 * 容量。active 是控制位图的一次快照，id 是目录对应位号；返回值是写入 @buf 的
	 * 字节数。展示控制平面状态，不承诺与正在进行的 enable 在同一瞬间同步。
	 */
	unsigned long active = READ_ONCE(aux_timekeepers);
	/* Lazy atoi() as name is "0..7" */
	int id = kobj->name[0] & 0x7;

	return sysfs_emit(buf, "%d\n", test_bit(id, &active));
}

static struct kobj_attribute aux_clock_enable_attr = __ATTR_RW(aux_clock_enable);

/*
 * aux_clock_enable_attr 把同名 show/store 回调包装成一个读写 sysfs 属性；attrs 以 NULL
 * 结尾供 sysfs 通用代码遍历；attr_group 把该数组作为每个 AUX 数字目录的共享布局。
 * 三者均是只在初始化时发布的静态元数据，目录销毁不会释放这些静态对象。
 */

static struct attribute *aux_clock_enable_attrs[] = {
	&aux_clock_enable_attr.attr,
	NULL
};

static const struct attribute_group aux_clock_enable_attr_group = {
	.attrs = aux_clock_enable_attrs,
};

static int __init tk_aux_sysfs_init(void)
{
	/*
	 * 建立 /sys/kernel/time/aux_clocks/{0..N}/enable。任一步失败由父 kobject_put 递归
	 * 撤销已经建立的子树；late_initcall 时 kobject/sysfs 已可用。
	 */
	/*
	 * 变量地图：tko 是 /sys/kernel/time 的持有引用，auxo 是其 aux_clocks 子目录引用；
	 * ret 保存首个失败 errno，初值 -ENOMEM 覆盖 kobject 创建返回 NULL 的接口约定。
	 * 循环中的 i 是零基 AUX 编号；id 是仅含一位数字和 NUL 的目录名；clk 是当前数字
	 * 子目录的创建引用。成功后 sysfs/kobject 层持有树关系；失败由 err_clean 从父层
	 * put，递归撤销此前已经发布的子树。
	 */
	struct kobject *auxo, *tko = kobject_create_and_add("time", kernel_kobj);
	int ret = -ENOMEM;

	if (!tko)
		return ret;

	auxo = kobject_create_and_add("aux_clocks", tko);
	if (!auxo)
		goto err_clean;

	for (int i = 0; i < MAX_AUX_CLOCKS; i++) {
		char id[2] = { [0] = '0' + i, };
		struct kobject *clk = kobject_create_and_add(id, auxo);

		if (!clk) {
			ret = -ENOMEM;
			goto err_clean;
		}

		ret = sysfs_create_group(clk, &aux_clock_enable_attr_group);
		if (ret)
			goto err_clean;
	}
	return 0;

err_clean:
	kobject_put(auxo);
	kobject_put(tko);
	return ret;
}
late_initcall(tk_aux_sysfs_init);

static __init void tk_aux_setup(void)
{
	/*
	 * 启动期只建立锁、seqcount 和稳定 ID；循环变量 i 是 timekeeper_data[] 的绝对槽号，
	 * 覆盖 [TIMEKEEPER_AUX_FIRST, TIMEKEEPER_AUX_LAST]。AUX 默认 invalid，之后按 sysfs
	 * 请求再绑定时钟源；函数无返回值，也不发布可读的 AUX 时间。
	 */
	for (int i = TIMEKEEPER_AUX_FIRST; i <= TIMEKEEPER_AUX_LAST; i++)
		tkd_basic_setup(&timekeeper_data[i], i, false);
}
#endif /* CONFIG_POSIX_AUX_CLOCKS */
