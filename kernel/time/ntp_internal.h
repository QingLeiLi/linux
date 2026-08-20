/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_NTP_INTERNAL_H
#define _LINUX_NTP_INTERNAL_H

/*
 * 本头文件是 timekeeping 与 NTP 实现之间的目录内契约。除启动 init 外，按 tkid 访问的状态接口都要求
 * 调用者已持对应 timekeeper 写锁；声明只借用 ntp.c 的静态实例，不提供引用或独立生命周期管理。
 */
/*
 * ntp_init() - 启动期初始化所有 NTP 实例的定点 tick 状态和可选硬件时钟同步 timer。
 * 无入参/返回值；单线程调用一次，无失败传播，之后各实例由对应 timekeeper 锁保护。
 */
extern void ntp_init(void);
/*
 * ntp_clear() - 把指定 NTP 实例置为未同步并取消待处理相位、adjtime 和闰秒运行态。
 * @tkid 必须是有效 timekeeper id 且调用者持其写锁；无返回值、不做边界检查或引用管理。
 */
extern void ntp_clear(unsigned int tkid);
/* Returns how long ticks are at present, in ns / 2^NTP_SCALE_SHIFT. */
/* 返回当前 tick 长度；其数值是带 NTP_SCALE_SHIFT 个小数位的定点纳秒，使用前需按 timekeeping 比例缩放。 */
/*
 * ntp_tick_length() - 读取指定实例包含频率、相位分摊和 adjtime 份额的当前定点 tick 长度。
 * @tkid 必须有效且调用者持锁；返回 u64 定点值，无状态修改、同步或错误码。
 */
extern u64 ntp_tick_length(unsigned int tkid);
/*
 * ntp_get_next_leap() - 查询主实例已武装的插入闰秒 realtime 时刻。
 * @tkid 非 core 或无待插入闰秒时返回 KTIME_MAX；调用者持锁。删除闰秒不通过本接口报告。
 */
extern ktime_t ntp_get_next_leap(unsigned int tkid);
/*
 * second_overflow() - 在一个墙钟整秒边界推进指定实例的 NTP/闰秒/PPS/adjtime 状态。
 * @tkid 必须有效且已持锁；@secs 是新墙钟秒。返回 -1/0/+1 秒闰秒修正，供 timekeeping 调整 realtime、
 * monotonic offset 和 TAI；不返回 errno、不睡眠。
 */
extern int second_overflow(unsigned int tkid, time64_t secs);
/*
 * ntp_adjtimex() - 在持锁事务内应用已验证 NTP 请求并填充 timex/audit 返回状态。
 * @tkid 必须有效；@txc 是输入输出结构；@ts 是同事务时间快照；@time_tai 是可写 TAI offset；@ad 是
 * 审计结果。所有指针非空且仅借用；返回 TIME_* 状态而非 errno，权限/格式错误由 timekeeping 外层处理。
 */
extern int ntp_adjtimex(unsigned int tkid, struct __kernel_timex *txc, const struct timespec64 *ts,
			s32 *time_tai, struct audit_ntp_data *ad);
/*
 * __hardpps() - 用一次外部 PPS 的 realtime/raw 时间戳校正 core 相位与频率。
 * @phase_ts/@raw_ts 是规范、非空借用指针；调用者已持 core timekeeper irqsave 锁。无返回值，首 pulse
 * 可能只建立校频基点，坏 pulse 更新错误状态后拒绝本次校正。
 */
extern void __hardpps(const struct timespec64 *phase_ts, const struct timespec64 *raw_ts);

#if defined(CONFIG_GENERIC_CMOS_UPDATE) || defined(CONFIG_RTC_SYSTOHC)
/*
 * ntp_notify_cmos_timer() - NTP 状态或墙钟 offset 事务后驱动可选 RTC 同步 work/timer。
 * @offset_set 为 true 时先取消旧绝对 timer；同步状态下再排唯一 work。可调用 hrtimer_cancel，故要求
 * 可等待的进程路径；无返回值，静态 timer/work 生命周期由 ntp.c 管理。
 */
extern void ntp_notify_cmos_timer(bool offset_set);
#else
/*
 * ntp_notify_cmos_timer() - 无硬件时钟写回机制时的空通知桩。
 * @offset_set 不访问，无返回值/副作用，使 adjtimex 完成路径无需条件编译。
 */
static inline void ntp_notify_cmos_timer(bool offset_set) { }
#endif

#endif /* _LINUX_NTP_INTERNAL_H */
