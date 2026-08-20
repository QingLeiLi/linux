/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TIMEKEEPING_INTERNAL_H
#define _TIMEKEEPING_INTERNAL_H

#include <linux/clocksource.h>
#include <linux/spinlock.h>
#include <linux/time.h>

/*
 * timekeeping debug functions
 */
/* timekeeping 调试接口：按配置记录 multigrain floor 交换次数与 suspend 时长，关闭后调用点零开销退化。 */
#ifdef CONFIG_DEBUG_FS

/* 每 CPU 计数器由 timekeeping_debug.c 定义；本地递增避免在文件时间戳热路径争用全局 cacheline。 */
DECLARE_PER_CPU(unsigned long, timekeeping_mg_floor_swaps);

/*
 * timekeeping_inc_mg_floor_swaps() - 记录本 CPU 成功推进一次 multigrain timestamp floor。
 * 无入参/返回值；`this_cpu_inc` 直接更新执行该指令 CPU 的槽位，调用者无需另加锁或手工关闭抢占。
 * 计数仅供 debugfs 统计，允许并发读取形成近似快照，不参与 floor 正确性或对象生命周期。
 */
static inline void timekeeping_inc_mg_floor_swaps(void)
{
	this_cpu_inc(timekeeping_mg_floor_swaps);
}

/*
 * tk_debug_account_sleep_time() - 把一次有效 suspend 注入时长计入 debugfs 对数区间并输出延迟调试日志。
 * @t 是调用者拥有、函数期间有效的规范 timespec64 借用指针；无返回值。调用点在 timekeeper 写事务内，
 * 当前实现更新全局 bin 时不另加锁，统计是诊断信息而非精确同步状态；函数不保存 @t 或转移引用。
 */
extern void tk_debug_account_sleep_time(const struct timespec64 *t);

#else

/* DEBUG_FS 关闭时 suspend 记账完全丢弃实参且不求值，调用点无代码和副作用。 */
#define tk_debug_account_sleep_time(x)

/*
 * timekeeping_inc_mg_floor_swaps() - DEBUG_FS 关闭时的空统计桩。
 * 无入参/返回值且无副作用，使 multigrain 热路径无需条件编译；任何原调用上下文均可内联消除。
 */
static inline void timekeeping_inc_mg_floor_swaps(void)
{
}

#endif

/*
 * clocksource_delta() - 计算有限位 counter 的正向模差并过滤疑似反向运动。
 * @now/@last 是同一 clocksource 的新旧 cycle；@mask 描述有效 counter 位；@max_delta 是该源允许的最大
 * 正向原始差。ret 为 `(now-last)&mask`，自然覆盖一次回绕；若 ret 大于门限则返回 0，避免时间倒退被
 * 解释为巨大正跳。函数无锁、不睡眠、无错误码；0 同时表示零间隔和被拒绝，调用者负责对象稳定与采样序。
 */
static inline u64 clocksource_delta(u64 now, u64 last, u64 mask, u64 max_delta)
{
	u64 ret = (now - last) & mask;

	/*
	 * Prevent time going backwards by checking the result against
	 * @max_delta. If greater, return 0.
	 */
	/* 将模差与 @max_delta 比较以阻止时间倒退；超过门限时把可疑结果压成 0。 */
	return ret > max_delta ? 0 : ret;
}

/* Semi public for serialization of non timekeeper VDSO updates. */
/* 以下半公开接口让非 timekeeper 的架构 vDSO 更新复用 core 写锁，不对目录外提供通用锁服务。 */
/*
 * timekeeper_lock_irqsave() - 关闭本地中断并取得 core timekeeper raw spinlock。
 * 无入参；返回调用前 IRQ flags。不可睡眠、不可递归；返回值必须在同 CPU 原样交给配对 unlock，期间
 * 调用者可更新受 core 锁保护的架构 vDSO 状态，但不得调用会再次取得该锁的路径。
 */
unsigned long timekeeper_lock_irqsave(void);
/*
 * timekeeper_unlock_irqrestore() - 释放 core timekeeper 锁并恢复 begin 前的本地中断状态。
 * @flags 必须来自同一次 `timekeeper_lock_irqsave()`，按值消费且不可复用；无返回值。错配 CPU、顺序或
 * flags 会破坏互斥/IRQ 状态，函数不校验；释放后调用者不再拥有 timekeeper 写侧稳定性。
 */
void timekeeper_unlock_irqrestore(unsigned long flags);

/* NTP specific interface to access the current seconds value */
/* NTP 专用接口：在对应 timekeeper 锁已经持有时读取当前墙钟整秒，避免额外 seqcount。 */
/*
 * ktime_get_ntp_seconds() - 返回指定 timekeeper 当前 xtime_sec，供 NTP 参考时刻与 PPS 更新使用。
 * @id 必须是有效的 `timekeeper_data[]` 索引，调用者已持该实例写锁；返回有符号墙钟秒。函数不做边界
 * 检查、不取锁、不睡眠，锁外或无效 id 调用会产生竞态/越界；不持有或转移 timekeeper 生命周期。
 */
long ktime_get_ntp_seconds(unsigned int id);

#endif /* _TIMEKEEPING_INTERNAL_H */
