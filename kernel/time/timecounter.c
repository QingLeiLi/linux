// SPDX-License-Identifier: GPL-2.0+
/*
 * Based on clocksource code. See commit 74d23cc704d1
 */
/*
 * 本实现由 clocksource 代码演化而来（历史依据为上方提交）。cyclecounter 只提供无状态硬件周期读数，
 * timecounter 在其上保存最近周期、累计纳秒和小数余量，把会回绕的设备计数扩展为长寿命时间轴。
 * 具体设备和调用者负责硬件初始化、读取回调的上下文约束以及对同一 timecounter 的串行化。
 */
#include <linux/export.h>
#include <linux/timecounter.h>

/*
 * timecounter_init() - 把 cyclecounter 绑定到一个纳秒累计器并建立初始基点。
 *
 * 【调用位置】PTP/NIC/CAN 等驱动在硬件时钟就绪后调用，后续用 timecounter_read() 推进时间，或用
 * timecounter_cyc2time() 把捕获的硬件时间戳映射到同一 epoch。
 * 【参数】@tc 是非 NULL 输出对象，调用者拥有并负责串行化；@cc 是已就绪的借用对象，必须至少活到
 * tc 停用且 read/mask/mult/shift 保持有效；@start_tstamp 是任意 epoch 下的初始纳秒值。
 * 【上下文】不自行加锁；会调用 cc->read()，能否在中断/NMI 使用由具体回调决定。函数自身不分配、
 * 不睡眠、不可失败。返回无直接值；副作用是覆盖 tc 全部状态，旧累计值不可恢复。
 */
void timecounter_init(struct timecounter *tc,
		      struct cyclecounter *cc,
		      u64 start_tstamp)
{
	/* 阶段 1：先记录借用的转换描述，再采样硬件，令 cycle_last 与 start_tstamp 近似对应同一时刻。 */
	tc->cc = cc;
	tc->cycle_last = cc->read(cc);
	tc->nsec = start_tstamp;
	/*
	 * shift 个低位保存乘法后不足 1ns 的定点余量；mask 要求 shift < 64。初始 frac 清零，
	 * 后续每次转换把余量带到下一次，避免大量小 delta 因逐次截断而持续丢失时间。
	 */
	tc->mask = (1ULL << cc->shift) - 1;
	tc->frac = 0;
}
EXPORT_SYMBOL_GPL(timecounter_init);

/**
 * timecounter_read_delta - get nanoseconds since last call of this function
 * @tc:         Pointer to time counter
 *
 * When the underlying cycle counter runs over, this will be handled
 * correctly as long as it does not run over more than once between
 * calls.
 *
 * The first call to this function for a new time counter initializes
 * the time tracking and returns an undefined result.
 */
/*
 * 取得自上次调用以来的纳秒增量。底层周期计数器回绕一次仍可借助 cc->mask 的模减法正确恢复；
 * 两次读取之间若回绕超过一次，高位圈数已不可观察，会永久少算时间。
 *
 * 修正说明：上方“首次调用初始化且结果未定义”描述的是未初始化对象，不是当前 API 的合法流程。
 * 当前实现不会在此初始化；调用者必须先执行 timecounter_init()，其第一次合法 read 已有确定结果。
 */
/*
 * timecounter_read_delta() - 读取、换算并提交自上次采样以来的纳秒增量。
 *
 * @tc 是已初始化的非 NULL 输入输出对象，调用者必须独占其 cycle_last/frac。返回 u64 纳秒增量；
 * 同时更新 cycle_last 和 frac，但不更新 tc->nsec。无显式失败返回，回绕过多属于不可检测的精度丢失。
 */
static u64 timecounter_read_delta(struct timecounter *tc)
{
	u64 cycle_now, cycle_delta;
	u64 ns_offset;

	/* read cycle counter: */
	/* 读取当前硬件周期；回调可能含 MMIO 或设备专属同步，本通用层不额外持锁。 */
	cycle_now = tc->cc->read(tc->cc);

	/* calculate the delta since the last timecounter_read_delta(): */
	/* 无符号相减后按硬件位宽 mask，单次自然回绕与普通前进得到同一个正向周期差。 */
	cycle_delta = (cycle_now - tc->cycle_last) & tc->cc->mask;

	/* convert to nanoseconds: */
	/* mult/shift 完成定点换算，tc->frac 输入旧余量并输出新余量，使跨调用的舍入误差守恒。 */
	ns_offset = cyclecounter_cyc2ns(tc->cc, cycle_delta,
					tc->mask, &tc->frac);

	/* update time stamp of timecounter_read_delta() call: */
	/* 提交采样基点；从此下次 read 只累计 cycle_now 之后的周期，顺序不能早于换算。 */
	tc->cycle_last = cycle_now;

	return ns_offset;
}

/*
 * timecounter_read() - 推进并返回 timecounter 当前 epoch 下的绝对纳秒值。
 *
 * @tc 是由 timecounter_init() 初始化的非 NULL 输入输出对象；调用者必须与 read/adjtime/reinit 串行化。
 * 返回 start_tstamp 加所有可观察周期增量的 u64 纳秒值。函数更新 cycle_last、frac、nsec，不分配、
 * 无显式失败；cc->read() 的上下文限制和“采样间隔短于一次完整回绕”前提继续适用。
 */
u64 timecounter_read(struct timecounter *tc)
{
	u64 nsec;

	/* increment time by nanoseconds since last call */
	/* 先取得并提交增量，再加到累计 epoch；tc->nsec 的写入是本次 read 留给后续调用的状态提交点。 */
	nsec = timecounter_read_delta(tc);
	nsec += tc->nsec;
	tc->nsec = nsec;

	return nsec;
}
EXPORT_SYMBOL_GPL(timecounter_read);

/* 文件边界：任意周期时间戳映射由头文件内联 timecounter_cyc2time() 提供，本文件不重复实现。 */
