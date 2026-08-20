// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 ARM Ltd.
 *
 * Generic implementation of update_vsyscall and update_vsyscall_tz.
 *
 * Based on the x86 specific implementation.
 */
/*
 * 本文件把受 timekeeper 锁保护的内核时间基线复制到用户态可直接读取的 vDSO 数据页，并用序列计数器与
 * 架构同步钩子保证用户读者只看到完整一代。它发布 realtime/monotonic/boottime/raw/TAI、coarse 值、
 * clocksource 换算参数、时区和可选辅助时钟；不实现用户查询算法，用户侧在 include/vdso 中完成重试。
 */

#include <linux/hrtimer.h>
#include <linux/timekeeper_internal.h>
#include <vdso/datapage.h>
#include <vdso/helpers.h>
#include <vdso/vsyscall.h>

#include "timekeeping_internal.h"

/*
 * fill_clock_configuration() - 把一个 timekeeper 读基线的 cycle→ns 参数复制到 vDSO clock 槽。
 * @vc 是当前 vDSO 写序列内的可写槽借用指针；@base 是持 timekeeper 锁稳定的 tkr_mono/tkr_raw 借用指针。
 * 复制 cycle_last、mask、mult、shift；开启 OVERFLOW_PROTECT 时还发布底层 clocksource 的 max_cycles，供
 * 用户侧慢换算防 u64 乘积溢出。无返回值、不睡眠、不取得 clock 引用；调用者负责先使 vDSO seq 为奇数，
 * 并在字段/基线全部完成后结束发布。
 */
static inline void fill_clock_configuration(struct vdso_clock *vc, const struct tk_read_base *base)
{
	vc->cycle_last	= base->cycle_last;
#ifdef CONFIG_GENERIC_VDSO_OVERFLOW_PROTECT
	vc->max_cycles	= base->clock->max_cycles;
#endif
	vc->mask	= base->mask;
	vc->mult	= base->mult;
	vc->shift	= base->shift;
}

/*
 * update_vdso_time_data() - 填充主 vDSO 页的高分辨率 MONOTONIC/BOOTTIME/RAW/TAI 基线与换算配置。
 * @vdata 是已处于 vdso_write_begin/end 临界区的主数据页；@tk 是调用者持 timekeeper 写锁稳定的主
 * timekeeper 借用指针。vc 指向 clock_data 数组，vdso_ts 是各 clockid 基线游标，sec/nsec 为规范化临时量。
 *
 * 先为 HRES_COARSE 与 RAW 槽复制 cycle 配置。MONOTONIC=xtime+wall_to_monotonic；nsec 保持
 * tkr_mono.shift 左移后的固定点单位，并用循环把每个 1s 进位到 sec。BOOTTIME 从已规范化 monotonic
 * 复制再加 monotonic_to_boot 并再次进位。RAW 使用 raw_sec/tkr_raw.xtime_nsec，TAI 使用
 * xtime_sec+tai_offset 与 mono fixed-point nsec。无返回值或错误码，所有字段只在外层序列发布后可见。
 */
static inline void update_vdso_time_data(struct vdso_time_data *vdata, struct timekeeper *tk)
{
	struct vdso_clock *vc = vdata->clock_data;
	struct vdso_timestamp *vdso_ts;
	u64 nsec, sec;

	fill_clock_configuration(&vc[CS_HRES_COARSE],	&tk->tkr_mono);
	fill_clock_configuration(&vc[CS_RAW],		&tk->tkr_raw);

	/* CLOCK_MONOTONIC */
	/* 墙钟基线加 wall_to_monotonic 得到不受 settimeofday 跳变影响的单调时间基线。 */
	vdso_ts		= &vc[CS_HRES_COARSE].basetime[CLOCK_MONOTONIC];
	vdso_ts->sec	= tk->xtime_sec + tk->wall_to_monotonic.tv_sec;

	nsec = tk->tkr_mono.xtime_nsec;
	nsec += ((u64)tk->wall_to_monotonic.tv_nsec << tk->tkr_mono.shift);
	while (nsec >= (((u64)NSEC_PER_SEC) << tk->tkr_mono.shift)) {
		nsec -= (((u64)NSEC_PER_SEC) << tk->tkr_mono.shift);
		vdso_ts->sec++;
	}
	vdso_ts->nsec	= nsec;

	/* Copy MONOTONIC time for BOOTTIME */
	/* 先复制已经规范化的 monotonic sec/fixed-point nsec，避免从 realtime 重复推导。 */
	sec	= vdso_ts->sec;
	/* Add the boot offset */
	/* monotonic_to_boot 累计 suspend 时间，使 BOOTTIME 在系统睡眠期间仍前进。 */
	sec	+= tk->monotonic_to_boot.tv_sec;
	nsec	+= (u64)tk->monotonic_to_boot.tv_nsec << tk->tkr_mono.shift;

	/* CLOCK_BOOTTIME */
	/* 加 offset 后再次按 fixed-point 1 秒进位，最终 nsec 保持与 mono shift 相同的单位。 */
	vdso_ts		= &vc[CS_HRES_COARSE].basetime[CLOCK_BOOTTIME];
	vdso_ts->sec	= sec;

	while (nsec >= (((u64)NSEC_PER_SEC) << tk->tkr_mono.shift)) {
		nsec -= (((u64)NSEC_PER_SEC) << tk->tkr_mono.shift);
		vdso_ts->sec++;
	}
	vdso_ts->nsec	= nsec;

	/* CLOCK_MONOTONIC_RAW */
	/* RAW 不应用 NTP 校正，使用独立 raw epoch 和 tkr_raw 比例。 */
	vdso_ts		= &vc[CS_RAW].basetime[CLOCK_MONOTONIC_RAW];
	vdso_ts->sec	= tk->raw_sec;
	vdso_ts->nsec	= tk->tkr_raw.xtime_nsec;

	/* CLOCK_TAI */
	/* TAI 与 realtime 相差当前 tai_offset 秒，共享 mono counter/固定点子秒基线。 */
	vdso_ts		= &vc[CS_HRES_COARSE].basetime[CLOCK_TAI];
	vdso_ts->sec	= tk->xtime_sec + (s64)tk->tai_offset;
	vdso_ts->nsec	= tk->tkr_mono.xtime_nsec;
}

/*
 * update_vsyscall() - 把主 timekeeper 的一代数据原子发布到 vDSO 用户页。
 *
 * 【参数与上下文】@tk 是 timekeeping 更新路径在写锁下持有的可写/稳定对象借用指针；vdata 指向架构映射
 * 的主 vDSO 页，vc/vdso_ts 为内部槽位借用指针。函数处于不可睡眠的时间更新临界区，不管理页生命周期。
 *
 * 【发布阶段】vdso_write_begin 先令 seq 无效。读取当前 mono clocksource 的 vdso_clock_mode，同时写入
 * HRES_COARSE/RAW；发布 REALTIME 高精度 fixed-point 基线、REALTIME_COARSE 和 MONOTONIC_COARSE 普通
 * 纳秒基线。coarse monotonic 用 __iter_div_u64_rem 规范化 sec/nsec。hrtimer_resolution 被 clock_getres
 * 无 seq 读取，故用 WRITE_ONCE 单独发布。mode=NONE 时用户高分辨率必须回退 syscall，跳过昂贵配置/基线
 * 更新但 coarse 数据仍有效；其他模式调用 update_vdso_time_data。
 *
 * 最后分别调用架构 clock 槽钩子，vdso_write_end 提交一致代，再以 arch sync 处理 cache/映射可见性。
 * 无返回值/错误传播；架构钩子必须不可睡眠。seq 结束前用户读者会重试，结束后整代同时生效。
 */
void update_vsyscall(struct timekeeper *tk)
{
	struct vdso_time_data *vdata = vdso_k_time_data;
	struct vdso_clock *vc = vdata->clock_data;
	struct vdso_timestamp *vdso_ts;
	s32 clock_mode;
	u64 nsec;

	/* copy vsyscall data */
	/* seq 进入写代后，用户 vDSO 读者必须重试，直到下方 end 完成所有字段。 */
	vdso_write_begin(vdata);

	clock_mode = tk->tkr_mono.clock->vdso_clock_mode;
	vc[CS_HRES_COARSE].clock_mode	= clock_mode;
	vc[CS_RAW].clock_mode		= clock_mode;

	/* CLOCK_REALTIME also required for time() */
	/* time() 只取秒也依赖此 realtime 槽，不能因高分辨率路径变化而省略。 */
	vdso_ts		= &vc[CS_HRES_COARSE].basetime[CLOCK_REALTIME];
	vdso_ts->sec	= tk->xtime_sec;
	vdso_ts->nsec	= tk->tkr_mono.xtime_nsec;

	/* CLOCK_REALTIME_COARSE */
	/* coarse 子秒是已下调精度的普通纳秒，不带 tkr shift。 */
	vdso_ts		= &vc[CS_HRES_COARSE].basetime[CLOCK_REALTIME_COARSE];
	vdso_ts->sec	= tk->xtime_sec;
	vdso_ts->nsec	= tk->coarse_nsec;

	/* CLOCK_MONOTONIC_COARSE */
	/* realtime coarse 加 wall_to_monotonic，并把可能跨秒的 nsec 商/余数规范化。 */
	vdso_ts		= &vc[CS_HRES_COARSE].basetime[CLOCK_MONOTONIC_COARSE];
	vdso_ts->sec	= tk->xtime_sec + tk->wall_to_monotonic.tv_sec;
	nsec		= tk->coarse_nsec;
	nsec		= nsec + tk->wall_to_monotonic.tv_nsec;
	vdso_ts->sec	+= __iter_div_u64_rem(nsec, NSEC_PER_SEC, &vdso_ts->nsec);

	/*
	 * Read without the seqlock held by clock_getres().
	 */
	/* clock_getres 不参加 vDSO seq 重试，单字段必须以 WRITE_ONCE 避免撕裂/合并。 */
	WRITE_ONCE(vdata->hrtimer_res, hrtimer_resolution);

	/*
	 * If the current clocksource is not VDSO capable, then spare the
	 * update of the high resolution parts.
	 */
	/* NONE 明确要求用户高精度查询走 syscall；仍发布 realtime/coarse 和架构所需模式状态。 */
	if (clock_mode != VDSO_CLOCKMODE_NONE)
		update_vdso_time_data(vdata, tk);

	__arch_update_vdso_clock(&vc[CS_HRES_COARSE]);
	__arch_update_vdso_clock(&vc[CS_RAW]);

	vdso_write_end(vdata);

	__arch_sync_vdso_time_data(vdata);
}

/*
 * update_vsyscall_tz() - 把内核全局 sys_tz 两个标量同步到主 vDSO 页。
 * 入参：无；由设时区路径在其串行协议下调用。vdata 是静态映射页借用指针；分别复制 minuteswest/dsttime，
 * 再调用架构同步钩子保证用户映射可见。当前实现不包围 vdso seq，用户侧可能分别观察两个标量；每个字段
 * 是本机整型单次存储。无返回值、错误码或页 ownership 变化，钩子不可睡眠。
 */
void update_vsyscall_tz(void)
{
	struct vdso_time_data *vdata = vdso_k_time_data;

	vdata->tz_minuteswest = sys_tz.tz_minuteswest;
	vdata->tz_dsttime = sys_tz.tz_dsttime;

	__arch_sync_vdso_time_data(vdata);
}

#ifdef CONFIG_POSIX_AUX_CLOCKS
/*
 * vdso_time_update_aux() - 发布一个 POSIX 辅助 timekeeper 的 vDSO clock 槽。
 * @tk 是调用者持相应 timekeeper 更新锁稳定的辅助对象，id 必须位于 TIMEKEEPER_AUX_FIRST 起始范围；以
 * id-offset 索引 aux_clock_data。vc/vdso_ts 分别是该辅助槽和 VDSO_BASE_AUX 基线借用指针。底层模式来自
 * mono clocksource，但 tk->clock_valid=false 时强制 NONE，通知用户回退 syscall。
 *
 * 在该 clock 自有 write seq 内先发布 mode。可用时复制 cycle 配置，计算 xtime+monotonic_to_aux：先把
 * fixed-point xtime_nsec 右移为普通 ns，加 offset 后以除法规范化 sec/余数，再左移回 tkr shift 写入 vDSO
 * fixed-point nsec。无效模式保留旧基线但 mode 阻止用户使用。架构更新、clock seq end 和主数据页 sync
 * 完成发布。无返回值/错误传播，不睡眠或转移对象 ownership。
 */
void vdso_time_update_aux(struct timekeeper *tk)
{
	struct vdso_time_data *vdata = vdso_k_time_data;
	struct vdso_timestamp *vdso_ts;
	struct vdso_clock *vc;
	s32 clock_mode;
	u64 nsec;

	vc = &vdata->aux_clock_data[tk->id - TIMEKEEPER_AUX_FIRST];
	vdso_ts = &vc->basetime[VDSO_BASE_AUX];
	clock_mode = tk->tkr_mono.clock->vdso_clock_mode;
	if (!tk->clock_valid)
		clock_mode = VDSO_CLOCKMODE_NONE;

	/* copy vsyscall data */
	/* 辅助槽有独立序列，只阻止读取本 vc 的用户读者，不必使整个主时间页失效。 */
	vdso_write_begin_clock(vc);

	vc->clock_mode = clock_mode;

	if (clock_mode != VDSO_CLOCKMODE_NONE) {
		fill_clock_configuration(vc, &tk->tkr_mono);

		vdso_ts->sec = tk->xtime_sec + tk->monotonic_to_aux.tv_sec;

		nsec = tk->tkr_mono.xtime_nsec >> tk->tkr_mono.shift;
		nsec += tk->monotonic_to_aux.tv_nsec;
		vdso_ts->sec += __iter_div_u64_rem(nsec, NSEC_PER_SEC, &nsec);
		nsec = nsec << tk->tkr_mono.shift;
		vdso_ts->nsec = nsec;
	}

	__arch_update_vdso_clock(vc);

	vdso_write_end_clock(vc);

	__arch_sync_vdso_time_data(vdata);
}
#endif

/**
 * vdso_update_begin - Start of a VDSO update section
 *
 * Allows architecture code to safely update the architecture specific VDSO
 * data. Disables interrupts, acquires timekeeper lock to serialize against
 * concurrent updates from timekeeping and invalidates the VDSO data
 * sequence counter to prevent concurrent readers from accessing
 * inconsistent data.
 *
 * Returns: Saved interrupt flags which need to be handed in to
 * vdso_update_end().
 */
/*
 * vdso_update_begin() - 为架构专用字段开启一个与 timekeeping 串行的 vDSO 写区间。
 * 入参：无；仅供架构代码在可关中断、不可睡眠的上下文调用。先 timekeeper_lock_irqsave 保存本 CPU IRQ
 * 状态并取得全局 timekeeper 锁，再 vdso_write_begin 使主 vdata seq 进入无效写代。返回 opaque flags，
 * 必须原样、同 CPU 传给 vdso_update_end；持锁期间架构可修改其 vDSO 数据，不能递归进入 timekeeping 写锁。
 */
unsigned long vdso_update_begin(void)
{
	struct vdso_time_data *vdata = vdso_k_time_data;
	unsigned long flags = timekeeper_lock_irqsave();

	vdso_write_begin(vdata);
	return flags;
}

/**
 * vdso_update_end - End of a VDSO update section
 * @flags:	Interrupt flags as returned from vdso_update_begin()
 *
 * Pairs with vdso_update_begin(). Marks vdso data consistent, invokes data
 * synchronization if the architecture requires it, drops timekeeper lock
 * and restores interrupt flags.
 */
/*
 * vdso_update_end() - 提交架构 vDSO 写代并释放 begin 取得的锁/IRQ 状态。
 * @flags 必须是同一次 begin 返回值，按值读取且不可复用。先 vdso_write_end 让用户读者看到一致字段，再调用
 * arch sync 完成 cache/别名映射可见性，最后 timekeeper_unlock_irqrestore 解锁并恢复原中断状态。无返回值；
 * 错配/遗漏会永久破坏 seq、锁或 IRQ 状态。函数不可睡眠，不改变数据页 ownership。
 */
void vdso_update_end(unsigned long flags)
{
	struct vdso_time_data *vdata = vdso_k_time_data;

	vdso_write_end(vdata);
	__arch_sync_vdso_time_data(vdata);
	timekeeper_unlock_irqrestore(flags);
}
