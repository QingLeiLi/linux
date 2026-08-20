// SPDX-License-Identifier: GPL-2.0
/*
 * NTP state machine interfaces and logic.
 *
 * This code was mainly moved from kernel/timer.c and kernel/time.c
 * Please see those files for relevant copyright info and historical
 * changelogs.
 */
/*
 * 本文件实现每个 timekeeper 的 NTP 驯钟状态机：把用户 adjtimex 的相位、频率、tick 和状态请求折算为
 * 每 tick 的 fixed-point 长度，在整秒边界消化相位/adjtime、推进闰秒状态并监视 PPS；主 timekeeper
 * 还可约 11 分钟把已同步墙钟相位对齐到持久时钟/RTC。所有 NTP 状态由对应 timekeeping 写锁保护，
 * 普通读取接口也由持锁调用；PPS 与 CMOS 路径分别额外依赖配置、workqueue 和 hrtimer 生命周期协议。
 */
#include <linux/capability.h>
#include <linux/clocksource.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/timex.h>
#include <linux/time.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/rtc.h>
#include <linux/audit.h>
#include <linux/timekeeper_internal.h>

#include "ntp_internal.h"
#include "timekeeping_internal.h"

/**
 * struct ntp_data - Structure holding all NTP related state
 * @tick_usec:		USER_HZ period in microseconds
 * @tick_length:	Adjusted tick length
 * @tick_length_base:	Base value for @tick_length
 * @time_state:		State of the clock synchronization
 * @time_status:	Clock status bits
 * @time_offset:	Time adjustment in nanoseconds
 * @time_constant:	PLL time constant
 * @time_maxerror:	Maximum error in microseconds holding the NTP sync distance
 *			(NTP dispersion + delay / 2)
 * @time_esterror:	Estimated error in microseconds holding NTP dispersion
 * @time_freq:		Frequency offset scaled nsecs/secs
 * @time_reftime:	Time at last adjustment in seconds
 * @time_adjust:	Adjustment value
 * @ntp_tick_adj:	Constant boot-param configurable NTP tick adjustment (upscaled)
 * @ntp_next_leap_sec:	Second value of the next pending leapsecond, or TIME64_MAX if no leap
 *
 * @pps_valid:		PPS signal watchdog counter
 * @pps_tf:		PPS phase median filter
 * @pps_jitter:		PPS current jitter in nanoseconds
 * @pps_fbase:		PPS beginning of the last freq interval
 * @pps_shift:		PPS current interval duration in seconds (shift value)
 * @pps_intcnt:		PPS interval counter
 * @pps_freq:		PPS frequency offset in scaled ns/s
 * @pps_stabil:		PPS current stability in scaled ns/s
 * @pps_calcnt:		PPS monitor: calibration intervals
 * @pps_jitcnt:		PPS monitor: jitter limit exceeded
 * @pps_stbcnt:		PPS monitor: stability limit exceeded
 * @pps_errcnt:		PPS monitor: calibration errors
 *
 * Protected by the timekeeping locks.
 */
/*
 * `ntp_data` 是单个 timekeeper 的完整驯钟状态。tick_usec 是名义 USER_HZ tick 微秒数；tick_length/base
 * 是含频率、启动校正和相位分摊的 NTP_SCALE_SHIFT 定点 tick 长度。time_state/status 保存闰秒状态机与
 * STA_* 能力/错误位；time_offset、constant、maxerror、esterror、freq、reftime、adjust 分别保存待摊相位、
 * PLL 常数、误差界、估计误差、定点频偏、上次校正秒和 adjtime 余量；ntp_tick_adj 是启动参数定点校正，
 * ntp_next_leap_sec 是下一闰秒墙钟秒或 TIME64_MAX。PPS 字段保存信号 watchdog、三样本相位滤波、jitter、
 * raw 校频基点/区间、频偏/稳定度和四类监控计数。数组成员均由对应 timekeeper 锁串行，无独立引用计数。
 */
struct ntp_data {
	unsigned long		tick_usec;
	u64			tick_length;
	u64			tick_length_base;
	int			time_state;
	int			time_status;
	s64			time_offset;
	long			time_constant;
	long			time_maxerror;
	long			time_esterror;
	s64			time_freq;
	time64_t		time_reftime;
	long			time_adjust;
	s64			ntp_tick_adj;
	time64_t		ntp_next_leap_sec;
#ifdef CONFIG_NTP_PPS
	int			pps_valid;
	long			pps_tf[3];
	long			pps_jitter;
	struct timespec64	pps_fbase;
	int			pps_shift;
	int			pps_intcnt;
	s64			pps_freq;
	long			pps_stabil;
	long			pps_calcnt;
	long			pps_jitcnt;
	long			pps_stbcnt;
	long			pps_errcnt;
#endif
};

/* 每个 timekeeper 一槽；启动时先给单位/状态/误差/闰秒哨兵默认值，再由 ntp_init 完成定点频率初始化。 */
static struct ntp_data tk_ntp_data[TIMEKEEPERS_MAX] = {
	[ 0 ... TIMEKEEPERS_MAX - 1 ] = {
		.tick_usec		= USER_TICK_USEC,
		.time_state		= TIME_OK,
		.time_status		= STA_UNSYNC,
		.time_constant		= 2,
		.time_maxerror		= NTP_PHASE_LIMIT,
		.time_esterror		= NTP_PHASE_LIMIT,
		.ntp_next_leap_sec	= TIME64_MAX,
	},
};

/* 日界秒数供闰秒武装；adjtime 每秒最多消化 500us，其定点版本按 NTP interval 分摊；TAI offset 上限 100000。 */
#define SECS_PER_DAY		86400
#define MAX_TICKADJ		500LL		/* usecs */
#define MAX_TICKADJ_SCALED \
	(((MAX_TICKADJ * NSEC_PER_USEC) << NTP_SCALE_SHIFT) / NTP_INTERVAL_FREQ)
#define MAX_TAI_OFFSET		100000

#ifdef CONFIG_NTP_PPS

/*
 * The following variables are used when a pulse-per-second (PPS) signal
 * is available. They establish the engineering parameters of the clock
 * discipline loop when controlled by the PPS signal.
 */
/*
 * PPS 参数依次规定信号失效秒数、相位异常倍数、校频区间 shift 下/上限、连续好坏区间门限和最大频率
 * wander；全部值用于已持 core timekeeper 锁的状态机，不是用户 ABI。
 */
#define PPS_VALID	10	/* PPS signal watchdog max (s) */
#define PPS_POPCORN	4	/* popcorn spike threshold (shift) */
#define PPS_INTMIN	2	/* min freq interval (s) (shift) */
#define PPS_INTMAX	8	/* max freq interval (s) (shift) */
#define PPS_INTCOUNT	4	/* number of consecutive good intervals to
				   increase pps_shift or consecutive bad
				   intervals to decrease it */
#define PPS_MAXWANDER	100000	/* max PPS freq wander (ns/s) */

/*
 * PPS kernel consumer compensates the whole phase error immediately.
 * Otherwise, reduce the offset by a fixed factor times the time constant.
 */
/*
 * ntp_offset_chunk() - 计算下一个整秒应从剩余相位误差中消化的定点份额。
 * @ntpdata 是调用者已持对应 timekeeper 锁的状态借用指针；@offset 为 NTP_SCALE_SHIFT 定点相位余量。
 * PPS time discipline 且信号有效时返回全部 offset，实现立即相位校正；否则按 PLL shift+time_constant
 * 算术右移，仅返回渐进份额。无状态写入、无错误码、不睡眠，负 offset 保持符号。
 */
static inline s64 ntp_offset_chunk(struct ntp_data *ntpdata, s64 offset)
{
	if (ntpdata->time_status & STA_PPSTIME && ntpdata->time_status & STA_PPSSIGNAL)
		return offset;
	else
		return shift_right(offset, SHIFT_PLL + ntpdata->time_constant);
}

/*
 * pps_reset_freq_interval() - 把 PPS 校频观察窗口重置到最短 2^PPS_INTMIN 秒。
 * @ntpdata 是已持锁可写状态；同时把连续好坏计数清 0。无返回值、不清校频基点或频率，其调用者决定
 * 是否随后重建其他 PPS 状态；不可睡眠且不管理对象生命周期。
 */
static inline void pps_reset_freq_interval(struct ntp_data *ntpdata)
{
	/* The PPS calibration interval may end surprisingly early */
	/* PPS 校频窗口可能因异常信号提前结束，因此从最短窗口重新积累稳定证据。 */
	ntpdata->pps_shift = PPS_INTMIN;
	ntpdata->pps_intcnt = 0;
}

/**
 * pps_clear - Clears the PPS state variables
 * @ntpdata:	Pointer to ntp data
 */
/*
 * pps_clear() - 清除丢失/重置信号后的 PPS 滤波与校频运行状态。
 * @ntpdata 为已持对应 timekeeper 锁的可写状态。先重置窗口，再清三样本、raw 基点和 PPS 频偏；watchdog、
 * jitter/稳定度和累计监控计数不在此清零。无返回值、不睡眠，之后首个 pulse 只建立新频率基点。
 */
static inline void pps_clear(struct ntp_data *ntpdata)
{
	pps_reset_freq_interval(ntpdata);
	ntpdata->pps_tf[0] = 0;
	ntpdata->pps_tf[1] = 0;
	ntpdata->pps_tf[2] = 0;
	ntpdata->pps_fbase.tv_sec = ntpdata->pps_fbase.tv_nsec = 0;
	ntpdata->pps_freq = 0;
}

/*
 * Decrease pps_valid to indicate that another second has passed since the
 * last PPS signal. When it reaches 0, indicate that PPS signal is missing.
 */
/*
 * pps_dec_valid() - 每过一整秒递减 PPS 信号 watchdog，并在已超时后撤销有效性。
 * @ntpdata 为已持锁状态；正值只减 1，进入本次调用时已为 0 才清 PPSSIGNAL/JITTER/WANDER/ERROR 并调用
 * pps_clear，因此从 1 减到 0 的那一秒仍保留信号位到下一次调用。无返回值、不睡眠。
 */
static inline void pps_dec_valid(struct ntp_data *ntpdata)
{
	if (ntpdata->pps_valid > 0) {
		ntpdata->pps_valid--;
	} else {
		ntpdata->time_status &= ~(STA_PPSSIGNAL | STA_PPSJITTER |
					  STA_PPSWANDER | STA_PPSERROR);
		pps_clear(ntpdata);
	}
}

/*
 * pps_set_freq() - 用户显式设置基础 time_freq 后让 PPS 校频基线立即跟随。
 * @ntpdata 是已持锁可写状态；复制 NTP_SCALE_SHIFT 定点频偏，不更新 tick_length，外层随后统一重算。
 * 无返回值、不验证范围（调用者已钳位）、不睡眠。
 */
static inline void pps_set_freq(struct ntp_data *ntpdata)
{
	ntpdata->pps_freq = ntpdata->time_freq;
}

/*
 * is_error_status() - 把 NTP/PPS 状态位组合折叠为 adjtimex 的 TIME_ERROR 判定。
 * @status 按值输入；UNSYNC/CLOCKERR 总是错误。请求 PPS time/freq 却无信号、请求 PPS time 且 jitter
 * 超限、或请求 PPS freq 且 wander/calibration error 时也返回 true；否则 false。纯计算、无状态写入。
 */
static inline bool is_error_status(int status)
{
	return (status & (STA_UNSYNC|STA_CLOCKERR))
		/*
		 * PPS signal lost when either PPS time or PPS frequency
		 * synchronization requested
		 */
		/* 请求 PPS 相位或频率同步时若信号已丢失，整体同步状态必须报告错误。 */
		|| ((status & (STA_PPSFREQ|STA_PPSTIME))
			&& !(status & STA_PPSSIGNAL))
		/*
		 * PPS jitter exceeded when PPS time synchronization
		 * requested
		 */
		/* 只有启用 PPS 相位同步时，jitter 超限才使系统调用结果进入 TIME_ERROR。 */
		|| ((status & (STA_PPSTIME|STA_PPSJITTER))
			== (STA_PPSTIME|STA_PPSJITTER))
		/*
		 * PPS wander exceeded or calibration error when PPS
		 * frequency synchronization requested
		 */
		/* PPS 频率同步请求把 wander 或校准错误任一状态提升为 TIME_ERROR。 */
		|| ((status & STA_PPSFREQ)
			&& (status & (STA_PPSWANDER|STA_PPSERROR)));
}

/*
 * pps_fill_timex() - 把内部 PPS 频率、jitter、窗口和监控计数转换到用户 timex 输出字段。
 * @ntpdata 是调用者已持锁的只读状态；@txc 是有效可写输出借用指针。ppsfreq 从定点 ns/s 转 timex
 * scaled-ppm；jitter 在 STA_NANO 下保留 ns，否则转 us；其余字段直接复制。无错误码、不修改 NTP 状态。
 */
static inline void pps_fill_timex(struct ntp_data *ntpdata, struct __kernel_timex *txc)
{
	txc->ppsfreq	   = shift_right((ntpdata->pps_freq >> PPM_SCALE_INV_SHIFT) *
					 PPM_SCALE_INV, NTP_SCALE_SHIFT);
	txc->jitter	   = ntpdata->pps_jitter;
	if (!(ntpdata->time_status & STA_NANO))
		txc->jitter = ntpdata->pps_jitter / NSEC_PER_USEC;
	txc->shift	   = ntpdata->pps_shift;
	txc->stabil	   = ntpdata->pps_stabil;
	txc->jitcnt	   = ntpdata->pps_jitcnt;
	txc->calcnt	   = ntpdata->pps_calcnt;
	txc->errcnt	   = ntpdata->pps_errcnt;
	txc->stbcnt	   = ntpdata->pps_stbcnt;
}

#else /* !CONFIG_NTP_PPS */

/*
 * ntp_offset_chunk() - 无 PPS 构建下按 PLL 常数渐进消化相位误差。
 * @ntpdata 提供已持锁 time_constant，@offset 是有符号定点余量；返回算术右移后的本秒份额，无副作用。
 */
static inline s64 ntp_offset_chunk(struct ntp_data *ntpdata, s64 offset)
{
	return shift_right(offset, SHIFT_PLL + ntpdata->time_constant);
}

/*
 * pps_reset_freq_interval() - 无 PPS 构建下的空窗口重置桩。
 * @ntpdata 不求值其字段且不保存，函数无返回值/副作用，使共用状态路径无需条件编译。
 */
static inline void pps_reset_freq_interval(struct ntp_data *ntpdata) {}
/*
 * pps_clear() - 无 PPS 构建下的空状态清理桩。
 * @ntpdata 仅保持接口形状，无返回值、状态或生命周期变化。
 */
static inline void pps_clear(struct ntp_data *ntpdata) {}
/*
 * pps_dec_valid() - 无 PPS 构建下的空 watchdog 推进桩。
 * @ntpdata 不访问，无返回值；整秒状态机调用它不会生成任何 PPS 状态。
 */
static inline void pps_dec_valid(struct ntp_data *ntpdata) {}
/*
 * pps_set_freq() - 无 PPS 构建下的空频率同步桩。
 * @ntpdata 不访问，无返回值；基础 NTP frequency 仍由普通路径独立维护。
 */
static inline void pps_set_freq(struct ntp_data *ntpdata) {}

/*
 * is_error_status() - 无 PPS 构建下只检查通用 NTP 不同步/硬件错误位。
 * @status 按值输入；含 STA_UNSYNC 或 STA_CLOCKERR 返回 true，否则 false，不解释不存在的 PPS 位。
 */
static inline bool is_error_status(int status)
{
	return status & (STA_UNSYNC|STA_CLOCKERR);
}

/*
 * pps_fill_timex() - 无 PPS 构建下把所有 PPS 用户输出字段确定性清零。
 * @ntpdata 未使用；@txc 是有效可写 timex 借用指针。无返回值，防用户看到未初始化或陈旧 PPS 数据。
 */
static inline void pps_fill_timex(struct ntp_data *ntpdata, struct __kernel_timex *txc)
{
	/* PPS is not implemented, so these are zero */
	/* 当前内核未实现 PPS，因此明确把相关 ABI 输出全部置 0。 */
	txc->ppsfreq	   = 0;
	txc->jitter	   = 0;
	txc->shift	   = 0;
	txc->stabil	   = 0;
	txc->jitcnt	   = 0;
	txc->calcnt	   = 0;
	txc->errcnt	   = 0;
	txc->stbcnt	   = 0;
}

#endif /* CONFIG_NTP_PPS */

/*
 * Update tick_length and tick_length_base, based on tick_usec, ntp_tick_adj and
 * time_freq:
 */
/*
 * ntp_update_frequency() - 由名义 tick、启动校正和当前频偏重算基础/当前定点 tick 长度。
 * @ntpdata 是已持对应 timekeeper 锁的可写状态；second_length/new_base/tick_usec 均为 u64 定点计算临时量。
 * 先算每秒总长度并除以 NTP_INTERVAL_FREQ，再把 base 的差额立即加到当前 tick_length，避免等下个整秒
 * 才生效；最后替换 tick_length_base。无返回值；调用者已保证 tick/freq 范围，函数不报告算术错误。
 */
static void ntp_update_frequency(struct ntp_data *ntpdata)
{
	u64 second_length, new_base, tick_usec = (u64)ntpdata->tick_usec;

	second_length		 = (u64)(tick_usec * NSEC_PER_USEC * USER_HZ) << NTP_SCALE_SHIFT;

	second_length		+= ntpdata->ntp_tick_adj;
	second_length		+= ntpdata->time_freq;

	new_base		 = div_u64(second_length, NTP_INTERVAL_FREQ);

	/*
	 * Don't wait for the next second_overflow, apply the change to the
	 * tick length immediately:
	 */
	/* 不等待下次整秒边界：只把 base 的增量叠加到当前值，保留正在消化的相位/adjtime 份额。 */
	ntpdata->tick_length		+= new_base - ntpdata->tick_length_base;
	ntpdata->tick_length_base	 = new_base;
}

/*
 * ntp_update_offset_fll() - 按相位差/采样间隔计算 FLL 频率校正并维护 STA_MODE。
 * @ntpdata 是已持锁状态；@offset64 为已钳位纳秒相位误差；@secs 是距上次校正的有符号墙钟秒。
 * 每次先清 STA_MODE；间隔小于 MINSEC，或未请求 STA_FLL 且不超过 MAXSEC 时返回 0，表示只走 PLL。
 * 否则置 STA_MODE 并返回 NTP_SCALE_SHIFT 定点的 offset/secs FLL 项。无 errno，负/异常间隔走早退。
 */
static inline s64 ntp_update_offset_fll(struct ntp_data *ntpdata, s64 offset64, long secs)
{
	ntpdata->time_status &= ~STA_MODE;

	if (secs < MINSEC)
		return 0;

	if (!(ntpdata->time_status & STA_FLL) && (secs <= MAXSEC))
		return 0;

	ntpdata->time_status |= STA_MODE;

	return div64_long(offset64 << (NTP_SCALE_SHIFT - SHIFT_FLL), secs);
}

/*
 * ntp_update_offset() - 接收一次 adjtimex 相位样本，更新 PLL/FLL 频偏和待摊相位。
 * @ntpdata 为已持对应 timekeeper 锁的可写状态；@offset 按 STA_NANO 为 ns，否则为 us。未启用 STA_PLL
 * 直接忽略。微秒输入先钳到 ±1 秒防乘 1000 溢出，再统一钳到 ±MAXPHASE；real_secs/secs 建立采样间隔，
 * FREQHOLD 强制 secs=0。先叠加可选 FLL 项，再限制过长 PLL 采样的增益，最终把 time_freq 钳到
 * ±MAXFREQ_SCALED，并把完整相位误差换成每 NTP interval 的定点 time_offset。无返回值/错误传播。
 */
static void ntp_update_offset(struct ntp_data *ntpdata, long offset)
{
	s64 freq_adj, offset64;
	long secs, real_secs;

	if (!(ntpdata->time_status & STA_PLL))
		return;

	if (!(ntpdata->time_status & STA_NANO)) {
		/* Make sure the multiplication below won't overflow */
		/* 微秒输入先限到 ±1 秒，随后乘 NSEC_PER_USEC 才不会溢出 long。 */
		offset = clamp(offset, -USEC_PER_SEC, USEC_PER_SEC);
		offset *= NSEC_PER_USEC;
	}

	/* Scale the phase adjustment and clamp to the operating range. */
	/* 从此处起相位统一使用纳秒，并限制在驯钟环可稳定处理的 MAXPHASE 范围。 */
	offset = clamp(offset, -MAXPHASE, MAXPHASE);

	/*
	 * Select how the frequency is to be controlled
	 * and in which mode (PLL or FLL).
	 */
	/* 用本 timekeeper 当前墙钟秒与上次样本之差选择 PLL/FLL；FREQHOLD 只更新相位，不学习频率。 */
	real_secs = ktime_get_ntp_seconds(ntpdata - tk_ntp_data);
	secs = (long)(real_secs - ntpdata->time_reftime);
	if (unlikely(ntpdata->time_status & STA_FREQHOLD))
		secs = 0;

	ntpdata->time_reftime = real_secs;

	offset64    = offset;
	freq_adj    = ntp_update_offset_fll(ntpdata, offset64, secs);

	/*
	 * Clamp update interval to reduce PLL gain with low
	 * sampling rate (e.g. intermittent network connection)
	 * to avoid instability.
	 */
	/* 低采样率会令 PLL 增益过大，故只对过长正间隔钳位；负间隔保持当前实现的有符号影响。 */
	if (unlikely(secs > 1 << (SHIFT_PLL + 1 + ntpdata->time_constant)))
		secs = 1 << (SHIFT_PLL + 1 + ntpdata->time_constant);

	freq_adj    += (offset64 * secs) <<
			(NTP_SCALE_SHIFT - 2 * (SHIFT_PLL + 2 + ntpdata->time_constant));

	freq_adj    = min(freq_adj + ntpdata->time_freq, MAXFREQ_SCALED);

	ntpdata->time_freq   = max(freq_adj, -MAXFREQ_SCALED);

	ntpdata->time_offset = div_s64(offset64 << NTP_SCALE_SHIFT, NTP_INTERVAL_FREQ);
}

/*
 * __ntp_clear() - 把单个 NTP 实例重置为未同步且无待处理相位/adjtime/闰秒的状态。
 * @ntpdata 是调用者已持锁的可写槽。停止 adjtime，置 UNSYNC 和最大误差，按保留的 tick_usec/time_freq/
 * ntp_tick_adj 重算 base，再令当前 tick 等于 base、清 offset、闰秒设 TIME64_MAX，并重置 PPS 窗口/滤波/
 * 频率基点。
 * 不重置 time_state、time_freq、time_status 其他位或累计 PPS 统计；无返回值、不睡眠。
 */
static void __ntp_clear(struct ntp_data *ntpdata)
{
	/* Stop active adjtime() */
	/* 取消尚未逐秒消化的 adjtime 余量，但保留频率配置供重新同步时继续使用。 */
	ntpdata->time_adjust	= 0;
	ntpdata->time_status	|= STA_UNSYNC;
	ntpdata->time_maxerror	= NTP_PHASE_LIMIT;
	ntpdata->time_esterror	= NTP_PHASE_LIMIT;

	ntp_update_frequency(ntpdata);

	ntpdata->tick_length	= ntpdata->tick_length_base;
	ntpdata->time_offset	= 0;

	ntpdata->ntp_next_leap_sec = TIME64_MAX;
	/* Clear PPS state variables */
	/* 清 PPS 滤波/校频运行态；错误统计和 jitter 等诊断历史由 pps_clear 的契约决定是否保留。 */
	pps_clear(ntpdata);
}

/**
 * ntp_clear - Clears the NTP state variables
 * @tkid:	Timekeeper ID to be able to select proper ntp data array member
 */
/*
 * ntp_clear() - 按 timekeeper id 调用内部 NTP 重置。
 * @tkid 必须是 `tk_ntp_data[]` 有效索引，调用者已持对应 timekeeping 写锁；无返回值且不做边界检查。
 * 数组静态常驻，本函数不取得引用，只把清理语义委托给 `__ntp_clear()`。
 */
void ntp_clear(unsigned int tkid)
{
	__ntp_clear(&tk_ntp_data[tkid]);
}


/*
 * ntp_tick_length() - 返回指定 timekeeper 当前每 NTP interval 的定点 tick 长度。
 * @tkid 必须有效且调用者已持对应锁；返回包含基础频偏、相位分摊及 adjtime 当前份额的 u64 值。
 * 不做同步/边界检查、不修改状态，单位仍左移 NTP_SCALE_SHIFT，调用者不可当普通纳秒直接使用。
 */
u64 ntp_tick_length(unsigned int tkid)
{
	return tk_ntp_data[tkid].tick_length;
}

/**
 * ntp_get_next_leap - Returns the next leapsecond in CLOCK_REALTIME ktime_t
 * @tkid:	Timekeeper ID
 *
 * Returns: For @tkid == TIMEKEEPER_CORE this provides the time of the next
 *	    leap second against CLOCK_REALTIME in a ktime_t format if a
 *	    leap second is pending. KTIME_MAX otherwise.
 */
/*
 * ntp_get_next_leap() - 查询主 timekeeper 已武装的插入闰秒墙钟时刻。
 * @tkid 按值输入；非 TIMEKEEPER_CORE 固定返回 KTIME_MAX，因为辅助时钟不执行闰秒。core 仅在
 * time_state=TIME_INS 且 STA_INS 仍置位时把 ntp_next_leap_sec 转 ktime，否则返回无事件哨兵。
 * 调用者已持 timekeeper 锁，函数不改状态、无引用转移；删除闰秒也不通过该接口返回。
 */
ktime_t ntp_get_next_leap(unsigned int tkid)
{
	struct ntp_data *ntpdata = &tk_ntp_data[TIMEKEEPER_CORE];

	if (tkid != TIMEKEEPER_CORE)
		return KTIME_MAX;

	if ((ntpdata->time_state == TIME_INS) && (ntpdata->time_status & STA_INS))
		return ktime_set(ntpdata->ntp_next_leap_sec, 0);

	return KTIME_MAX;
}

/*
 * This routine handles the overflow of the microsecond field
 *
 * The tricky bits of code to handle the accurate clock support
 * were provided by Dave Mills (Mills@UDEL.EDU) of NTP fame.
 * They were originally developed for SUN and DEC kernels.
 * All the kudos should go to Dave for this stuff.
 *
 * Also handles leap second processing, and returns leap offset
 */
/*
 * second_overflow() - 在每个墙钟整秒边界推进 NTP、闰秒、PPS watchdog 和 adjtime 状态。
 * @tkid 必须是有效实例且调用者持对应 timekeeper 写锁；@secs 是刚推进到的墙钟秒。ntpdata 是静态槽，
 * delta 为本秒相位份额，leap 返回 -1 表示插入时把时钟退 1 秒、+1 表示删除时进 1 秒、0 表示无跳变，
 * rem 用于计算下一个 UTC 日界。函数不睡眠；执行闰秒时会在持锁路径打印 notice，可能增加临界区时延。
 *
 * 阶段 1 按 TIME_OK/INS/DEL/OOP/WAIT 状态机武装、取消或执行闰秒，只有 core 的外层语义会实际使用；
 * 阶段 2 每秒增加 maxerror，越界钳位并置 UNSYNC；阶段 3 从 base 重建 tick_length，按 PPS/PLL 策略
 * 消化 time_offset 并检查 PPS 失效；阶段 4 以每秒最多 MAX_TICKADJ us 消化 adjtime，最后返回 leap。
 */
int second_overflow(unsigned int tkid, time64_t secs)
{
	struct ntp_data *ntpdata = &tk_ntp_data[tkid];
	s64 delta;
	int leap = 0;
	s32 rem;

	/*
	 * Leap second processing. If in leap-insert state at the end of the
	 * day, the system clock is set back one second; if in leap-delete
	 * state, the system clock is set ahead one second.
	 */
	/* 插入在日界重复一秒，删除在日界跳过一秒；WAIT 要等用户撤销 INS/DEL 位后才回到可重新武装状态。 */
	switch (ntpdata->time_state) {
	case TIME_OK:
		if (ntpdata->time_status & STA_INS) {
			ntpdata->time_state = TIME_INS;
			div_s64_rem(secs, SECS_PER_DAY, &rem);
			ntpdata->ntp_next_leap_sec = secs + SECS_PER_DAY - rem;
		} else if (ntpdata->time_status & STA_DEL) {
			ntpdata->time_state = TIME_DEL;
			div_s64_rem(secs + 1, SECS_PER_DAY, &rem);
			ntpdata->ntp_next_leap_sec = secs + SECS_PER_DAY - rem;
		}
		break;
	case TIME_INS:
		if (!(ntpdata->time_status & STA_INS)) {
			ntpdata->ntp_next_leap_sec = TIME64_MAX;
			ntpdata->time_state = TIME_OK;
		} else if (secs == ntpdata->ntp_next_leap_sec) {
			leap = -1;
			ntpdata->time_state = TIME_OOP;
			pr_notice("Clock: inserting leap second 23:59:60 UTC\n");
		}
		break;
	case TIME_DEL:
		if (!(ntpdata->time_status & STA_DEL)) {
			ntpdata->ntp_next_leap_sec = TIME64_MAX;
			ntpdata->time_state = TIME_OK;
		} else if (secs == ntpdata->ntp_next_leap_sec) {
			leap = 1;
			ntpdata->ntp_next_leap_sec = TIME64_MAX;
			ntpdata->time_state = TIME_WAIT;
			pr_notice("Clock: deleting leap second 23:59:59 UTC\n");
		}
		break;
	case TIME_OOP:
		ntpdata->ntp_next_leap_sec = TIME64_MAX;
		ntpdata->time_state = TIME_WAIT;
		break;
	case TIME_WAIT:
		if (!(ntpdata->time_status & (STA_INS | STA_DEL)))
			ntpdata->time_state = TIME_OK;
		break;
	}

	/* Bump the maxerror field */
	/* 未获新同步样本时误差界按最大频漂每秒增长；达到协议上限即明确标为未同步。 */
	ntpdata->time_maxerror += MAXFREQ / NSEC_PER_USEC;
	if (ntpdata->time_maxerror > NTP_PHASE_LIMIT) {
		ntpdata->time_maxerror = NTP_PHASE_LIMIT;
		ntpdata->time_status |= STA_UNSYNC;
	}

	/* Compute the phase adjustment for the next second */
	/* 每秒先恢复频率基线，再叠加本秒应消化的相位份额，剩余误差留给后续秒。 */
	ntpdata->tick_length	 = ntpdata->tick_length_base;

	delta			 = ntp_offset_chunk(ntpdata, ntpdata->time_offset);
	ntpdata->time_offset	-= delta;
	ntpdata->tick_length	+= delta;

	/* Check PPS signal */
	/* PPS watchdog 以整秒为时基；配置关闭时该调用内联为空。 */
	pps_dec_valid(ntpdata);

	if (!ntpdata->time_adjust)
		goto out;

	if (ntpdata->time_adjust > MAX_TICKADJ) {
		ntpdata->time_adjust -= MAX_TICKADJ;
		ntpdata->tick_length += MAX_TICKADJ_SCALED;
		goto out;
	}

	if (ntpdata->time_adjust < -MAX_TICKADJ) {
		ntpdata->time_adjust += MAX_TICKADJ;
		ntpdata->tick_length -= MAX_TICKADJ_SCALED;
		goto out;
	}

	ntpdata->tick_length += (s64)(ntpdata->time_adjust * NSEC_PER_USEC / NTP_INTERVAL_FREQ)
				<< NTP_SCALE_SHIFT;
	ntpdata->time_adjust = 0;

out:
	return leap;
}

#if defined(CONFIG_GENERIC_CMOS_UPDATE) || defined(CONFIG_RTC_SYSTOHC)
static void sync_hw_clock(struct work_struct *work);
/* 唯一 work 串行可睡眠 RTC I/O；静态 realtime hrtimer 只负责唤醒 work，11 分钟常量是正常同步周期。 */
static DECLARE_WORK(sync_work, sync_hw_clock);
static struct hrtimer sync_hrtimer;
#define SYNC_PERIOD_NS (11ULL * 60 * NSEC_PER_SEC)

/*
 * sync_timer_callback() - RTC 同步绝对 hrtimer 到期后把实际 I/O 排入可冻结节能 workqueue。
 * @timer 是静态 `sync_hrtimer` 借用指针，当前不读取；回调运行于 hrtimer/原子上下文，不能睡眠。
 * queue_work 合并已 pending 的同一 work；返回 HRTIMER_NORESTART，下一次到期由 work 按结果显式重武装。
 */
static enum hrtimer_restart sync_timer_callback(struct hrtimer *timer)
{
	queue_work(system_freezable_power_efficient_wq, &sync_work);

	return HRTIMER_NORESTART;
}

/*
 * sched_sync_hw_clock() - 按 RTC 写入相位安排下一次 CLOCK_REALTIME 绝对 hrtimer。
 * @offset_nsec 是 RTC set_time 到其下一秒跳变的纳秒偏移；@retry 表示上次未成功。exp 先取当前墙钟整秒，
 * retry 时定位约 2 秒后的写窗口，否则定位约 11 分钟后的周期窗口，再减 offset 对齐硬件秒沿。
 * 无返回值；启动静态 hrtimer 会替换其既有到期，调用者在 workqueue 可睡眠语境运行。
 */
static void sched_sync_hw_clock(unsigned long offset_nsec, bool retry)
{
	ktime_t exp = ktime_set(ktime_get_real_seconds(), 0);

	if (retry)
		exp = ktime_add_ns(exp, 2ULL * NSEC_PER_SEC - offset_nsec);
	else
		exp = ktime_add_ns(exp, SYNC_PERIOD_NS - offset_nsec);

	hrtimer_start(&sync_hrtimer, exp, HRTIMER_MODE_ABS);
}

/*
 * Check whether @now is correct versus the required time to update the RTC
 * and calculate the value which needs to be written to the RTC so that the
 * next seconds increment of the RTC after the write is aligned with the next
 * seconds increment of clock REALTIME.
 *
 * tsched     t1 write(t2.tv_sec - 1sec))	t2 RTC increments seconds
 *
 * t2.tv_nsec == 0
 * tsched = t2 - set_offset_nsec
 * newval = t2 - NSEC_PER_SEC
 *
 * ==> neval = tsched + set_offset_nsec - NSEC_PER_SEC
 *
 * As the execution of this code is not guaranteed to happen exactly at
 * tsched this allows it to happen within a fuzzy region:
 *
 *	abs(now - tsched) < FUZZ
 *
 * If @now is not inside the allowed window the function returns false.
 */
/*
 * rtc_tv_nsec_ok() - 判断当前执行时刻是否落在 RTC 写窗口，并生成应写入的整秒值。
 * @set_offset_nsec 是设备写入到下次硬件秒增量的偏移；@to_set 是非空输出；@now 是当前 realtime 借用
 * 快照。delay=(-1s,+offset)，先算候选写值；若其 nsec 距 0 小于 5 jiffies 的模糊窗，规范为整秒并
 * 返回 true，靠近上一秒末时还进位 sec；否则返回 false。失败仍会写候选 @to_set，调用者不得使用。
 */
static inline bool rtc_tv_nsec_ok(unsigned long set_offset_nsec,
				  struct timespec64 *to_set,
				  const struct timespec64 *now)
{
	/* Allowed error in tv_nsec, arbitrarily set to 5 jiffies in ns. */
	/* 允许误差经验性取 5 个 jiffy 的纳秒数，吸收 workqueue 调度和 RTC I/O 启动抖动。 */
	const unsigned long TIME_SET_NSEC_FUZZ = TICK_NSEC * 5;
	struct timespec64 delay = {.tv_sec = -1,
				   .tv_nsec = set_offset_nsec};

	*to_set = timespec64_add(*now, delay);

	if (to_set->tv_nsec < TIME_SET_NSEC_FUZZ) {
		to_set->tv_nsec = 0;
		return true;
	}

	if (to_set->tv_nsec > NSEC_PER_SEC - TIME_SET_NSEC_FUZZ) {
		to_set->tv_sec++;
		to_set->tv_nsec = 0;
		return true;
	}
	return false;
}

#ifdef CONFIG_GENERIC_CMOS_UPDATE
/*
 * update_persistent_clock64() - 架构可覆盖的 legacy 持久时钟写入弱实现。
 * @now64 按值传入已换算到硬件期望时区/相位的整秒时间；默认不访问硬件并返回 -ENODEV，驱动/架构同名
 * 强符号可替换它。调用于可睡眠 workqueue；成功应返回 0，其他 errno 触发上层重试而非 RTC class 回退。
 */
int __weak update_persistent_clock64(struct timespec64 now64)
{
	return -ENODEV;
}
#else
/*
 * update_persistent_clock64() - 未启用 legacy CMOS 更新时的不可用桩。
 * @now64 按值接收但不使用，固定返回 -ENODEV，通知上层继续尝试 RTC class；无副作用、不睡眠。
 */
static inline int update_persistent_clock64(struct timespec64 now64)
{
	return -ENODEV;
}
#endif

#ifdef CONFIG_RTC_SYSTOHC
/* Save NTP synchronized time to the RTC */
/* 已由 NTP 同步的系统墙钟将写入配置指定 RTC；设备写相位可能要求先反馈 offset 再重试。 */
/*
 * update_rtc() - 打开 CONFIG_RTC_SYSTOHC_DEVICE 并尝试把整秒墙钟写入 RTC class 设备。
 * @to_set 是已完成时区/相位计算的借用时间；@offset_nsec 是调用者持久保存的设备写相位输入输出。
 * 打不开、无 set_time op 返回 -ENODEV；缓存 offset 与设备不同时只更新输出并返回 -EAGAIN；相同时转换
 * 秒字段并返回 rtc_set_time errno/0。所有打开成功路径均关闭设备引用；函数可睡眠，不保存输入指针。
 */
static int update_rtc(struct timespec64 *to_set, unsigned long *offset_nsec)
{
	struct rtc_device *rtc;
	struct rtc_time tm;
	int err = -ENODEV;

	rtc = rtc_class_open(CONFIG_RTC_SYSTOHC_DEVICE);
	if (!rtc)
		return -ENODEV;

	if (!rtc->ops || !rtc->ops->set_time)
		goto out_close;

	/* First call might not have the correct offset */
	/* 首次打开或 RTC 更换后缓存 offset 可能不匹配：先学习设备相位，下一轮再重算正确写窗口。 */
	if (*offset_nsec == rtc->set_offset_nsec) {
		rtc_time64_to_tm(to_set->tv_sec, &tm);
		err = rtc_set_time(rtc, &tm);
	} else {
		/* Store the update offset and let the caller try again */
		/* 把设备真实 offset 返回给静态缓存并要求重试；旧 to_set 是按错误相位算出的，绝不能写入。 */
		*offset_nsec = rtc->set_offset_nsec;
		err = -EAGAIN;
	}
out_close:
	rtc_class_close(rtc);
	return err;
}
#else
/*
 * update_rtc() - 未启用 RTC_SYSTOHC 时的不可用桩。
 * @to_set/@offset_nsec 均不访问，固定返回 -ENODEV；上层据此停止本轮 11 分钟同步链。
 */
static inline int update_rtc(struct timespec64 *to_set, unsigned long *offset_nsec)
{
	return -ENODEV;
}
#endif

/**
 * ntp_synced - Tells whether the NTP status is not UNSYNC
 * Returns:	true if not UNSYNC, false otherwise
 */
/*
 * ntp_synced() - 对主 NTP 状态做一次轻量的 STA_UNSYNC 反向判断。
 * 无入参；位未置返回 true，置位返回 false。CMOS work/通知路径在 timekeeper 锁外读取且没有 seqcount，
 * 因而只用于容忍竞态的调度决策：并发变化最坏多排/少排一次 work，下一通知或 timer 会重新判断。
 */
static inline bool ntp_synced(void)
{
	return !(tk_ntp_data[TIMEKEEPER_CORE].time_status & STA_UNSYNC);
}

/*
 * If we have an externally synchronized Linux clock, then update RTC clock
 * accordingly every ~11 minutes. Generally RTCs can only store second
 * precision, but many RTCs will adjust the phase of their second tick to
 * match the moment of update. This infrastructure arranges to call to the RTC
 * set at the correct moment to phase synchronize the RTC second tick over
 * with the kernel clock.
 */
/*
 * sync_hw_clock() - 在 workqueue 中把已同步主墙钟于正确相位写入 legacy 持久钟或 RTC class。
 * @work 是静态 `sync_work` 借用指针，未使用；static offset_nsec 跨轮缓存设备 set offset，单个 work 实例
 * 保证本函数不并发执行。若 NTP 已失步或 timer 已被其他通知重武装则直接返回；否则取 realtime，检查
 * 写窗口，必要时按 local RTC 时区修正，先试 legacy、仅 -ENODEV 才试 RTC class。成功安排约 11 分钟后，
 * 时间窗错过/-EAGAIN/其他错误约 2 秒重试，两类设备都 -ENODEV 则停止，等待后续 NTP 通知。可睡眠。
 */
static void sync_hw_clock(struct work_struct *work)
{
	/*
	 * The default synchronization offset is 500ms for the deprecated
	 * update_persistent_clock64() under the assumption that it uses
	 * the infamous CMOS clock (MC146818).
	 */
	/* legacy MC146818 假定写入后半秒发生硬件秒沿；RTC class 首轮会用设备真实 offset 替换。 */
	static unsigned long offset_nsec = NSEC_PER_SEC / 2;
	struct timespec64 now, to_set;
	int res = -EAGAIN;

	/*
	 * Don't update if STA_UNSYNC is set and if ntp_notify_cmos_timer()
	 * managed to schedule the work between the timer firing and the
	 * work being able to rearm the timer. Wait for the timer to expire.
	 */
	/* 失步时禁止写硬件；若通知路径已排好 timer，本轮 work 不得用旧结果覆盖它的计划。 */
	if (!ntp_synced() || hrtimer_is_queued(&sync_hrtimer))
		return;

	ktime_get_real_ts64(&now);
	/* If @now is not in the allowed window, try again */
	/* work 调度未命中模糊窗时保留 -EAGAIN，rearm 选择短重试。 */
	if (!rtc_tv_nsec_ok(offset_nsec, &to_set, &now))
		goto rearm;

	/* Take timezone adjusted RTCs into account */
	/* 平台声明持久时钟保存本地时间时，把 UTC 墙钟减去 west 分钟，再交给后续任一硬件写入路径。 */
	if (persistent_clock_is_local)
		to_set.tv_sec -= (sys_tz.tz_minuteswest * 60);

	/* Try the legacy RTC first. */
	/* legacy 只以 -ENODEV 表示不存在；成功或实际硬件错误都直接按结果安排下一轮。 */
	res = update_persistent_clock64(to_set);
	if (res != -ENODEV)
		goto rearm;

	/* Try the RTC class */
	/* RTC class 也不存在时不再无意义重试；设备以后出现需靠新的 NTP 通知重新启动。 */
	res = update_rtc(&to_set, &offset_nsec);
	if (res == -ENODEV)
		return;
rearm:
	sched_sync_hw_clock(offset_nsec, res != 0);
}

/*
 * ntp_notify_cmos_timer() - NTP/设时事务后启动、取消或重新触发硬件时钟同步链。
 * @offset_set 表示 ADJ_SETOFFSET 造成墙钟跳变；为 true 时同步取消基于旧绝对墙钟的 timer（可等待回调）。
 * 若主钟当前同步且 timer 未排队，则把唯一 work 排入 freezable 节能队列；work 已运行时重复 queue 可能只
 * 产生一次无害空转。无返回值；调用者在可允许 hrtimer_cancel 的进程路径，静态 timer/work 常驻。
 * 当前 core 调用点传 `result.delta.tv_sec != 0`，纯子秒 ADJ_SETOFFSET 不走取消分支，这是现有策略边界。
 */
void ntp_notify_cmos_timer(bool offset_set)
{
	/*
	 * If the time jumped (using ADJ_SETOFFSET) cancels sync timer,
	 * which may have been running if the time was synchronized
	 * prior to the ADJ_SETOFFSET call.
	 */
	/* 绝对 CLOCK_REALTIME timer 会被墙钟跳变穿越，先取消旧计划，避免在错误相位写 RTC。 */
	if (offset_set)
		hrtimer_cancel(&sync_hrtimer);

	/*
	 * When the work is currently executed but has not yet the timer
	 * rearmed this queues the work immediately again. No big issue,
	 * just a pointless work scheduled.
	 */
	/* work 正处在“检查后、重武装前”时可能再排一次；共享单 work 保证不会并发执行。 */
	if (ntp_synced() && !hrtimer_is_queued(&sync_hrtimer))
		queue_work(system_freezable_power_efficient_wq, &sync_work);
}

/*
 * ntp_init_cmos_sync() - 初始化静态 RTC 同步 hrtimer 的回调、CLOCK_REALTIME 基准和绝对模式。
 * 无入参/返回值；启动期调用一次，不启动 timer，首次已同步通知才排 work。timer 静态常驻且无销毁路径。
 */
static void __init ntp_init_cmos_sync(void)
{
	hrtimer_setup(&sync_hrtimer, sync_timer_callback, CLOCK_REALTIME, HRTIMER_MODE_ABS);
}
#else /* CONFIG_GENERIC_CMOS_UPDATE) || defined(CONFIG_RTC_SYSTOHC) */
/*
 * ntp_init_cmos_sync() - 两种硬件时钟写回机制都关闭时的 init 空桩。
 * 无入参/返回值或副作用，令通用 ntp_init 无需条件编译。
 */
static inline void __init ntp_init_cmos_sync(void) { }
#endif /* !CONFIG_GENERIC_CMOS_UPDATE) || defined(CONFIG_RTC_SYSTOHC) */

/*
 * Propagate a new txc->status value into the NTP state:
 */
/*
 * process_adj_status() - 合并用户可写 STA 状态位并处理 PLL 开关边沿。
 * @ntpdata 是已持锁可写实例；@txc 是已验证、调用期间有效的只读 timex。关闭旧 PLL 时取消闰秒状态、
 * 标 UNSYNC 并重置 PPS 校频窗口；开启新 PLL 时把 reftime 锚到当前实例墙钟秒。最后保留内核只读
 * STA_RONLY 位，只从 txc 接受其余位。无返回值；不重算 tick length，外层 modes 处理统一完成。
 */
static inline void process_adj_status(struct ntp_data *ntpdata, const struct __kernel_timex *txc)
{
	if ((ntpdata->time_status & STA_PLL) && !(txc->status & STA_PLL)) {
		ntpdata->time_state = TIME_OK;
		ntpdata->time_status = STA_UNSYNC;
		ntpdata->ntp_next_leap_sec = TIME64_MAX;
		/* Restart PPS frequency calibration */
		/* PLL 关闭后旧 PPS 观察窗口不再代表新一轮同步，恢复到最短窗口重新学习。 */
		pps_reset_freq_interval(ntpdata);
	}

	/*
	 * If we turn on PLL adjustments then reset the
	 * reference time to current time.
	 */
	/* PLL 关闭到开启的边沿以当前秒为频率采样基准，避免把关闭期间算进首个 interval。 */
	if (!(ntpdata->time_status & STA_PLL) && (txc->status & STA_PLL))
		ntpdata->time_reftime = ktime_get_ntp_seconds(ntpdata - tk_ntp_data);

	/* only set allowed bits */
	/* 内核生成的只读状态由旧值保留，用户只能替换其余可写位。 */
	ntpdata->time_status &= STA_RONLY;
	ntpdata->time_status |= txc->status & ~STA_RONLY;
}

/*
 * process_adjtimex_modes() - 按固定顺序把已验证 timex modes 写入一个 NTP 实例。
 * @ntpdata 是已持锁可写状态；@txc 是只读请求；@time_tai 是对应 timekeeper 的可写 TAI 秒偏移借用指针。
 * 依次处理 STATUS、NANO/MICRO、FREQUENCY、误差界、TIMECONST、TAI、OFFSET、TICK；频率钳到协议范围，
 * 误差钳到 0..NTP_PHASE_LIMIT，微秒模式 time_constant 兼容性加 4 后再钳位，非法 TAI 值静默不写。
 * 最后只要 tick/frequency/offset 任一改变就重算 tick_length。无返回值，权限/组合/范围基础验证在外层。
 */
static inline void process_adjtimex_modes(struct ntp_data *ntpdata, const struct __kernel_timex *txc,
					  s32 *time_tai)
{
	if (txc->modes & ADJ_STATUS)
		process_adj_status(ntpdata, txc);

	if (txc->modes & ADJ_NANO)
		ntpdata->time_status |= STA_NANO;

	if (txc->modes & ADJ_MICRO)
		ntpdata->time_status &= ~STA_NANO;

	if (txc->modes & ADJ_FREQUENCY) {
		/* timex freq 为 scaled ppm，转换到内部定点 ns/s 后双向钳位，并同步 PPS 初始频偏。 */
		ntpdata->time_freq = txc->freq * PPM_SCALE;
		ntpdata->time_freq = min(ntpdata->time_freq, MAXFREQ_SCALED);
		ntpdata->time_freq = max(ntpdata->time_freq, -MAXFREQ_SCALED);
		/* Update pps_freq */
		/* 同步 PPS 内部频偏起点，防下个校频窗口把用户这次显式改动误判为 wander。 */
		pps_set_freq(ntpdata);
	}

	if (txc->modes & ADJ_MAXERROR)
		ntpdata->time_maxerror = clamp(txc->maxerror, 0, NTP_PHASE_LIMIT);

	if (txc->modes & ADJ_ESTERROR)
		ntpdata->time_esterror = clamp(txc->esterror, 0, NTP_PHASE_LIMIT);

	if (txc->modes & ADJ_TIMECONST) {
		/* 旧微秒 ABI 的时间常数定义比纳秒模式偏 4，兼容转换后仍不得超过 MAXTC。 */
		ntpdata->time_constant = clamp(txc->constant, 0, MAXTC);
		if (!(ntpdata->time_status & STA_NANO))
			ntpdata->time_constant += 4;
		ntpdata->time_constant = clamp(ntpdata->time_constant, 0, MAXTC);
	}

	if (txc->modes & ADJ_TAI && txc->constant >= 0 && txc->constant <= MAX_TAI_OFFSET)
		/* ADJ_TAI 复用 constant 字段；越界请求在这里不写，前层按既有 ABI 不返回错误。 */
		*time_tai = txc->constant;

	if (txc->modes & ADJ_OFFSET)
		ntp_update_offset(ntpdata, txc->offset);

	if (txc->modes & ADJ_TICK)
		ntpdata->tick_usec = txc->tick;

	if (txc->modes & (ADJ_TICK|ADJ_FREQUENCY|ADJ_OFFSET))
		ntp_update_frequency(ntpdata);
}

/*
 * adjtimex() mainly allows reading (and writing, if superuser) of
 * kernel time-keeping variables. used by xntpd.
 */
/*
 * ntp_adjtimex() - 在持锁事务内应用/查询 NTP 参数并填满用户 timex 返回结构。
 * @tkid 必须有效；@txc 是已由 timekeeping 层验证且兼作输入/输出的非空结构；@ts 是同一事务当前墙钟
 * 快照；@time_tai 是对应实例可写 TAI offset；@ad 是非空审计累积对象。所有指针只在调用期间借用。
 *
 * ADJ_ADJTIME 分支独立于 ntp_adjtime：保存旧 time_adjust，可写请求替换新余量并重算频率/审计，随后
 * 无论读写都把旧余量返回到 txc->offset。普通分支先审计并应用 modes，再把内部定点相位转为 ns/us。
 * 最后根据通用/PPS 错误位选 TIME_ERROR，导出频率、误差、状态、精度、tick、TAI、PPS 和时间快照；
 * 若快照已跨待处理闰秒边界，临时调整 result/TAI/显示秒以保持 adjtimex ABI。返回 TIME_*，非 errno。
 */
int ntp_adjtimex(unsigned int tkid, struct __kernel_timex *txc, const struct timespec64 *ts,
		 s32 *time_tai, struct audit_ntp_data *ad)
{
	struct ntp_data *ntpdata = &tk_ntp_data[tkid];
	int result;

	if (txc->modes & ADJ_ADJTIME) {
		long save_adjust = ntpdata->time_adjust;

		if (!(txc->modes & ADJ_OFFSET_READONLY)) {
			/* adjtime() is independent from ntp_adjtime() */
			/* adjtime 的单次余量独立于 PLL/FLL 相位状态；新请求替换而非叠加旧余量。 */
			ntpdata->time_adjust = txc->offset;
			ntp_update_frequency(ntpdata);

			audit_ntp_set_old(ad, AUDIT_NTP_ADJUST,	save_adjust);
			audit_ntp_set_new(ad, AUDIT_NTP_ADJUST,	ntpdata->time_adjust);
		}
		txc->offset = save_adjust;
	} else {
		/* If there are input parameters, then process them: */
		/* modes=0 是纯查询；非零时先完整记录旧审计值，再应用并记录新值。 */
		if (txc->modes) {
			audit_ntp_set_old(ad, AUDIT_NTP_OFFSET,	ntpdata->time_offset);
			audit_ntp_set_old(ad, AUDIT_NTP_FREQ,	ntpdata->time_freq);
			audit_ntp_set_old(ad, AUDIT_NTP_STATUS,	ntpdata->time_status);
			audit_ntp_set_old(ad, AUDIT_NTP_TAI,	*time_tai);
			audit_ntp_set_old(ad, AUDIT_NTP_TICK,	ntpdata->tick_usec);

			process_adjtimex_modes(ntpdata, txc, time_tai);

			audit_ntp_set_new(ad, AUDIT_NTP_OFFSET,	ntpdata->time_offset);
			audit_ntp_set_new(ad, AUDIT_NTP_FREQ,	ntpdata->time_freq);
			audit_ntp_set_new(ad, AUDIT_NTP_STATUS,	ntpdata->time_status);
			audit_ntp_set_new(ad, AUDIT_NTP_TAI,	*time_tai);
			audit_ntp_set_new(ad, AUDIT_NTP_TICK,	ntpdata->tick_usec);
		}

		txc->offset = shift_right(ntpdata->time_offset * NTP_INTERVAL_FREQ, NTP_SCALE_SHIFT);
		if (!(ntpdata->time_status & STA_NANO))
			txc->offset = div_s64(txc->offset, NSEC_PER_USEC);
	}

	result = ntpdata->time_state;
	if (is_error_status(ntpdata->time_status))
		result = TIME_ERROR;

	txc->freq	   = shift_right((ntpdata->time_freq >> PPM_SCALE_INV_SHIFT) *
					 PPM_SCALE_INV, NTP_SCALE_SHIFT);
	txc->maxerror	   = ntpdata->time_maxerror;
	txc->esterror	   = ntpdata->time_esterror;
	txc->status	   = ntpdata->time_status;
	txc->constant	   = ntpdata->time_constant;
	txc->precision	   = 1;
	txc->tolerance	   = MAXFREQ_SCALED / PPM_SCALE;
	txc->tick	   = ntpdata->tick_usec;
	txc->tai	   = *time_tai;

	/* Fill PPS status fields */
	/* 配置 PPS 时导出真实监控量，未配置时 helper 确定性清零全部相关 ABI 字段。 */
	pps_fill_timex(ntpdata, txc);

	txc->time.tv_sec = ts->tv_sec;
	txc->time.tv_usec = ts->tv_nsec;
	if (!(ntpdata->time_status & STA_NANO))
		txc->time.tv_usec = ts->tv_nsec / NSEC_PER_USEC;

	/* Handle leapsec adjustments */
	/* timekeeping 尚未正式提交闰秒跳变时，按状态临时修正本次 adjtimex 观察，避免 ABI 提前/滞后一秒。 */
	if (unlikely(ts->tv_sec >= ntpdata->ntp_next_leap_sec)) {
		if ((ntpdata->time_state == TIME_INS) && (ntpdata->time_status & STA_INS)) {
			result = TIME_OOP;
			txc->tai++;
			txc->time.tv_sec--;
		}
		if ((ntpdata->time_state == TIME_DEL) && (ntpdata->time_status & STA_DEL)) {
			result = TIME_WAIT;
			txc->tai--;
			txc->time.tv_sec++;
		}
		if ((ntpdata->time_state == TIME_OOP) && (ts->tv_sec == ntpdata->ntp_next_leap_sec))
			result = TIME_WAIT;
	}

	return result;
}

#ifdef	CONFIG_NTP_PPS

/*
 * struct pps_normtime is basically a struct timespec, but it is
 * semantically different (and it is the reason why it was invented):
 * pps_normtime.nsec has a range of ( -NSEC_PER_SEC / 2, NSEC_PER_SEC / 2 ]
 * while timespec.tv_nsec has a range of [0, NSEC_PER_SEC)
 */
/* PPS 规范时间以最近秒沿为中心：sec 携带整数区间，nsec 在 (-0.5s, +0.5s]，便于直接表示相位误差。 */
struct pps_normtime {
	s64		sec;	/* seconds */
	long		nsec;	/* nanoseconds */
};

/*
 * Normalize the timestamp so that nsec is in the
 * [ -NSEC_PER_SEC / 2, NSEC_PER_SEC / 2 ] interval
 */
/*
 * pps_normalize_ts() - 把标准 timespec64 转成以最近秒沿为中心的 PPS 表示。
 * @ts 按值输入且应规范为 0<=tv_nsec<1s；norm 是返回副本。nsec 大于半秒时减 1 秒并令 sec 加 1，
 * 其余保持不变，返回范围为 (-0.5s,+0.5s]。纯计算、无错误码；非规范输入不在契约内。
 */
static inline struct pps_normtime pps_normalize_ts(struct timespec64 ts)
{
	struct pps_normtime norm = {
		.sec = ts.tv_sec,
		.nsec = ts.tv_nsec
	};

	if (norm.nsec > (NSEC_PER_SEC >> 1)) {
		norm.nsec -= NSEC_PER_SEC;
		norm.sec++;
	}

	return norm;
}

/* Get current phase correction and jitter */
/* 取得当前相位校正和 jitter；现实现尚非三样本中值滤波，而是返回最新样本并比较前一样本。 */
/*
 * pps_phase_filter_get() - 读取 PPS 相位滤波当前输出并计算相邻两样本绝对差。
 * @ntpdata 是已持 core 锁状态；@jitter 是非空输出，写 `abs(tf[0]-tf[1])`；返回 tf[0] 最新校正值。
 * 不修改滤波器、无错误码；原 TODO 表明未来可替换真正滤波算法，调用者不能假定当前值是三点中值。
 */
static inline long pps_phase_filter_get(struct ntp_data *ntpdata, long *jitter)
{
	*jitter = ntpdata->pps_tf[0] - ntpdata->pps_tf[1];
	if (*jitter < 0)
		*jitter = -*jitter;

	/* TODO: test various filters */
	/* 待办：比较不同滤波器；当前仍直接返回最新相位样本。 */
	return ntpdata->pps_tf[0];
}

/* Add the sample to the phase filter */
/* 把新相位样本压入三槽历史；当前读取算法只使用最近两槽，但保留第三槽供未来滤波器。 */
/*
 * pps_phase_filter_add() - 以前移方式把一个 PPS 相位校正样本加入固定三槽窗口。
 * @ntpdata 为已持锁可写状态；@err 是纳秒校正值。tf[2]=旧 tf[1]、tf[1]=旧 tf[0]、tf[0]=err；
 * 无返回值，不计算输出或清历史，启动时的 0 槽也会参与最初 jitter 比较。
 */
static inline void pps_phase_filter_add(struct ntp_data *ntpdata, long err)
{
	ntpdata->pps_tf[2] = ntpdata->pps_tf[1];
	ntpdata->pps_tf[1] = ntpdata->pps_tf[0];
	ntpdata->pps_tf[0] = err;
}

/*
 * Decrease frequency calibration interval length. It is halved after four
 * consecutive unstable intervals.
 */
/*
 * pps_dec_freq_interval() - 累积不稳定校频区间，并在连续四次后把观察窗口减半。
 * @ntpdata 是已持锁状态。pps_intcnt 先减并在 -PPS_INTCOUNT 饱和；若 pps_shift 仍高于下限则减 1 并把
 * 计数归零，否则保持负饱和值。无返回值，不改变 raw 基点，调用者负责为下一窗口重建基点。
 */
static inline void pps_dec_freq_interval(struct ntp_data *ntpdata)
{
	if (--ntpdata->pps_intcnt <= -PPS_INTCOUNT) {
		ntpdata->pps_intcnt = -PPS_INTCOUNT;
		if (ntpdata->pps_shift > PPS_INTMIN) {
			ntpdata->pps_shift--;
			ntpdata->pps_intcnt = 0;
		}
	}
}

/*
 * Increase frequency calibration interval length. It is doubled after
 * four consecutive stable intervals.
 */
/*
 * pps_inc_freq_interval() - 累积稳定校频区间，并在连续四次后把观察窗口加倍。
 * @ntpdata 是已持锁状态。pps_intcnt 先加并在 PPS_INTCOUNT 饱和；若 shift 未达上限则加 1 并清计数，
 * 已达上限则保持正饱和值。无返回值；改变只影响后续完成窗口的门限。
 */
static inline void pps_inc_freq_interval(struct ntp_data *ntpdata)
{
	if (++ntpdata->pps_intcnt >= PPS_INTCOUNT) {
		ntpdata->pps_intcnt = PPS_INTCOUNT;
		if (ntpdata->pps_shift < PPS_INTMAX) {
			ntpdata->pps_shift++;
			ntpdata->pps_intcnt = 0;
		}
	}
}

/*
 * Update clock frequency based on MONOTONIC_RAW clock PPS signal
 * timestamps
 *
 * At the end of the calibration interval the difference between the
 * first and last MONOTONIC_RAW clock timestamps divided by the length
 * of the interval becomes the frequency update. If the interval was
 * too long, the data are discarded.
 * Returns the difference between old and new frequency values.
 */
/*
 * hardpps_update_freq() - 用一个完成的 MONOTONIC_RAW PPS 窗口估算振荡器频偏与稳定度。
 * @ntpdata 是已持 core timekeeper 锁的可写状态；@freq_norm 是从旧 pps_fbase 到当前 pulse 的中心化时差，
 * sec 已由调用者保证为正。窗口超过 2^(pps_shift+1) 秒时置 PPSERROR、计数、缩短窗口并返回 0。
 *
 * 正常路径以 `-nsec/sec` 算定点 pps_freq，delta 是新旧频偏差的 ns/s；即使 wander 超阈值也保留新
 * pps_freq、置 PPSWANDER 并缩窗，否则扩窗。随后以绝对 delta 更新稳定度 EWMA；启用 PPSFREQ 且未
 * FREQHOLD 时把 pps_freq 写入系统 time_freq 并重算 tick。返回 delta，无 errno，延迟日志不可睡眠。
 */
static long hardpps_update_freq(struct ntp_data *ntpdata, struct pps_normtime freq_norm)
{
	long delta, delta_mod;
	s64 ftemp;

	/* Check if the frequency interval was too long */
	/* 过长窗口可能跨越丢 pulse/不连续区间，拒绝本次校准并缩短下一观察窗口。 */
	if (freq_norm.sec > (2 << ntpdata->pps_shift)) {
		ntpdata->time_status |= STA_PPSERROR;
		ntpdata->pps_errcnt++;
		pps_dec_freq_interval(ntpdata);
		printk_deferred(KERN_ERR "hardpps: PPSERROR: interval too long - %lld s\n",
				freq_norm.sec);
		return 0;
	}

	/*
	 * Here the raw frequency offset and wander (stability) is
	 * calculated. If the wander is less than the wander threshold the
	 * interval is increased; otherwise it is decreased.
	 */
	/* raw clock 不受 NTP 驯速反向影响；相对理想整数秒沿的负 nsec/秒即本机频偏估计。 */
	ftemp = div_s64(((s64)(-freq_norm.nsec)) << NTP_SCALE_SHIFT,
			freq_norm.sec);
	delta = shift_right(ftemp - ntpdata->pps_freq, NTP_SCALE_SHIFT);
	ntpdata->pps_freq = ftemp;
	if (delta > PPS_MAXWANDER || delta < -PPS_MAXWANDER) {
		printk_deferred(KERN_WARNING "hardpps: PPSWANDER: change=%ld\n", delta);
		ntpdata->time_status |= STA_PPSWANDER;
		ntpdata->pps_stbcnt++;
		pps_dec_freq_interval(ntpdata);
	} else {
		/* Good sample */
		/* 频偏变化在 wander 门限内，连续稳定样本将逐步扩大窗口以降低测量噪声。 */
		pps_inc_freq_interval(ntpdata);
	}

	/*
	 * The stability metric is calculated as the average of recent
	 * frequency changes, but is used only for performance monitoring
	 */
	/* 稳定度只做监控：对绝对频偏变化应用 1/2^PPS_INTMIN 的指数平滑。 */
	delta_mod = delta;
	if (delta_mod < 0)
		delta_mod = -delta_mod;
	ntpdata->pps_stabil += (div_s64(((s64)delta_mod) << (NTP_SCALE_SHIFT - SHIFT_USEC),
				     NSEC_PER_USEC) - ntpdata->pps_stabil) >> PPS_INTMIN;

	/* If enabled, the system clock frequency is updated */
	/* PPSFREQ 使该估计接管系统频率；FREQHOLD 则只更新监控状态，不改变驯钟速度。 */
	if ((ntpdata->time_status & STA_PPSFREQ) && !(ntpdata->time_status & STA_FREQHOLD)) {
		ntpdata->time_freq = ntpdata->pps_freq;
		ntp_update_frequency(ntpdata);
	}

	return delta;
}

/* Correct REALTIME clock phase error against PPS signal */
/* 用 PPS 信号修正 REALTIME 相位；异常样本仍参与 jitter EWMA，但不会写 time_offset。 */
/*
 * hardpps_update_phase() - 过滤一个 PPS 相位误差，按 jitter 门限决定是否校正系统相位。
 * @ntpdata 是已持 core 锁状态；@error 是中心化 REALTIME 时间戳相对秒沿的纳秒误差。correction 取负后
 * 推入样本窗口，jitter 是最近两校正差；超过旧 jitter<<PPS_POPCORN 时置错误并计数。否则若 PPSTIME
 * 开启，把 correction 转为定点 time_offset 并取消并行 adjtime。最后无论接受与否都更新 jitter EWMA。
 * 无返回值；未启用 PPSTIME 时仍维护信号质量统计，不改变相位。
 */
static void hardpps_update_phase(struct ntp_data *ntpdata, long error)
{
	long correction = -error;
	long jitter;

	/* Add the sample to the median filter */
	/* 源注释称 median filter，但当前 helper 实际输出最新样本；该差异由 helper 契约明确。 */
	pps_phase_filter_add(ntpdata, correction);
	correction = pps_phase_filter_get(ntpdata, &jitter);

	/*
	 * Nominal jitter is due to PPS signal noise. If it exceeds the
	 * threshold, the sample is discarded; otherwise, if so enabled,
	 * the time offset is updated.
	 */
	/* popcorn 门限相对历史 jitter 放大 16 倍；超限丢相位校正但保留错误状态和统计。 */
	if (jitter > (ntpdata->pps_jitter << PPS_POPCORN)) {
		printk_deferred(KERN_WARNING "hardpps: PPSJITTER: jitter=%ld, limit=%ld\n",
				jitter, (ntpdata->pps_jitter << PPS_POPCORN));
		ntpdata->time_status |= STA_PPSJITTER;
		ntpdata->pps_jitcnt++;
	} else if (ntpdata->time_status & STA_PPSTIME) {
		/* Correct the time using the phase offset */
		/* PPS 相位校正写入与普通 NTP 相同的定点 offset，由后续 second_overflow 消化。 */
		ntpdata->time_offset = div_s64(((s64)correction) << NTP_SCALE_SHIFT,
					       NTP_INTERVAL_FREQ);
		/* Cancel running adjtime() */
		/* 避免单次 adjtime 与 PPS 相位控制同时向 tick_length 叠加两个相位源。 */
		ntpdata->time_adjust = 0;
	}
	/* Update jitter */
	/* 用固定 1/2^PPS_INTMIN 增益平滑 jitter；异常样本也会抬高未来门限。 */
	ntpdata->pps_jitter += (jitter - ntpdata->pps_jitter) >> PPS_INTMIN;
}

/*
 * __hardpps() - discipline CPU clock oscillator to external PPS signal
 *
 * This routine is called at each PPS signal arrival in order to
 * discipline the CPU clock oscillator to the PPS signal. It takes two
 * parameters: REALTIME and MONOTONIC_RAW clock timestamps. The former
 * is used to correct clock phase error and the latter is used to
 * correct the frequency.
 *
 * This code is based on David Mills's reference nanokernel
 * implementation. It was mostly rewritten but keeps the same idea.
 */
/*
 * __hardpps() - 在每个外部 PPS 到达时用 REALTIME 校相、MONOTONIC_RAW 校频。
 * @phase_ts/@raw_ts 是调用期间有效的非空规范 timespec64 借用指针；外层 `hardpps()` 已持 core
 * timekeeper irqsave 锁，因此本函数不再同步。pts_norm/freq_norm 分别是相位和距校频基点的中心化时间。
 *
 * 每个 pulse 先清瞬时 PPS 错误、置 PPSSIGNAL 并把 watchdog 续到 10 秒。首次只保存 raw 基点返回；
 * 后续用 raw 差验证平均每秒误差未超 ±MAXFREQ，坏 pulse 置 JITTER、重建基点并连相位也拒绝。有效窗口
 * 达 2^pps_shift 秒时先重建基点、计数并更新频率；每个有效 pulse 都更新相位。无返回值，延迟日志不睡眠，
 * 指针不保存；pps_fbase.tv_sec==0 是未初始化哨兵，启动最初不足 1 秒的 raw 基点可能延后一次建窗。
 */
void __hardpps(const struct timespec64 *phase_ts, const struct timespec64 *raw_ts)
{
	struct ntp_data *ntpdata = &tk_ntp_data[TIMEKEEPER_CORE];
	struct pps_normtime pts_norm, freq_norm;

	pts_norm = pps_normalize_ts(*phase_ts);

	/* Clear the error bits, they will be set again if needed */
	/* JITTER/WANDER/ERROR 是本 pulse/窗口的瞬时质量位，每次先清，后续检查可重新置位。 */
	ntpdata->time_status &= ~(STA_PPSJITTER | STA_PPSWANDER | STA_PPSERROR);

	/* indicate signal presence */
	/* 收到 pulse 即续 watchdog；即便后续样本质量失败，PPSSIGNAL 仍表示物理信号存在。 */
	ntpdata->time_status |= STA_PPSSIGNAL;
	ntpdata->pps_valid = PPS_VALID;

	/*
	 * When called for the first time, just start the frequency
	 * interval
	 */
	/* 无 raw 基点无法算振荡器频偏，首个 pulse 只锚定窗口且暂不进行相位更新。 */
	if (unlikely(ntpdata->pps_fbase.tv_sec == 0)) {
		ntpdata->pps_fbase = *raw_ts;
		return;
	}

	/* Ok, now we have a base for frequency calculation */
	/* raw 差不含系统 NTP 校正反馈，适合测量本机振荡器自身漂移。 */
	freq_norm = pps_normalize_ts(timespec64_sub(*raw_ts, ntpdata->pps_fbase));

	/*
	 * Check that the signal is in the range
	 * [1s - MAXFREQ us, 1s + MAXFREQ us], otherwise reject it
	 */
	/* 校频累计窗口的中心化 nsec 必须随秒数保持在 MAXFREQ 允许的线性误差锥内。 */
	if ((freq_norm.sec == 0) || (freq_norm.nsec > MAXFREQ * freq_norm.sec) ||
	    (freq_norm.nsec < -MAXFREQ * freq_norm.sec)) {
		ntpdata->time_status |= STA_PPSJITTER;
		/* Restart the frequency calibration interval */
		/* 坏 pulse 不能与旧基点继续累计，立即以当前 raw 时刻重启窗口。 */
		ntpdata->pps_fbase = *raw_ts;
		printk_deferred(KERN_ERR "hardpps: PPSJITTER: bad pulse\n");
		return;
	}

	/* Signal is ok. Check if the current frequency interval is finished */
	/* 到达自适应窗口长度才更新频率；未到时仍用本 pulse 做相位质量/校正。 */
	if (freq_norm.sec >= (1 << ntpdata->pps_shift)) {
		ntpdata->pps_calcnt++;
		/* Restart the frequency calibration interval */
		/* 先移动基点，保证即使校频 helper 判过长失败，下一窗口也从当前 pulse 开始。 */
		ntpdata->pps_fbase = *raw_ts;
		hardpps_update_freq(ntpdata, freq_norm);
	}

	hardpps_update_phase(ntpdata, pts_norm.nsec);

}
#endif	/* CONFIG_NTP_PPS */

/*
 * ntp_tick_adj_setup() - 解析早期启动参数 `ntp_tick_adj=` 并存为 core 定点每秒校正。
 * @str 是 setup 框架提供的 NUL 结尾借用字符串；rc 为 base=0 的 s64 解析结果。失败原样返回负 errno；
 * 成功把值左移 NTP_SCALE_SHIFT 后返回 1 表示参数已处理。无显式范围/移位溢出检查，错误极值是现有边界；
 * 仅启动早期单线程写 core 槽，之后 ntp_init 重算 tick base。
 */
static int __init ntp_tick_adj_setup(char *str)
{
	int rc = kstrtos64(str, 0, &tk_ntp_data[TIMEKEEPER_CORE].ntp_tick_adj);
	if (rc)
		return rc;

	tk_ntp_data[TIMEKEEPER_CORE].ntp_tick_adj <<= NTP_SCALE_SHIFT;
	return 1;
}
/* 注册早期命令行处理器；参数只影响主 timekeeper，不传播到辅助实例。 */
__setup("ntp_tick_adj=", ntp_tick_adj_setup);

/*
 * ntp_init() - 初始化所有 NTP 实例的定点 tick/未同步状态并建立可选 RTC 同步 timer。
 * 无入参/返回值；id 遍历 0..TIMEKEEPERS_MAX-1，调用 `__ntp_clear()` 使用静态默认字段计算 base，再只对
 * 已编译的硬件写回机制初始化 hrtimer。启动期单线程执行，无失败传播；静态状态自此由各 timekeeper 锁保护。
 */
void __init ntp_init(void)
{
	for (int id = 0; id < TIMEKEEPERS_MAX; id++)
		__ntp_clear(tk_ntp_data + id);
	ntp_init_cmos_sync();
}
