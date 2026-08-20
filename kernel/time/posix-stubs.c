// SPDX-License-Identifier: GPL-2.0
/*
 * Dummy stubs used when CONFIG_POSIX_TIMERS=n
 *
 * Created by:  Nicolas Pitre, July 2016
 * Copyright:   (C) 2016 Linaro Limited
 */
/*
 * CONFIG_POSIX_TIMERS=n 时，本文件与完整 posix-timers/posix-cpu-timers/posix-clock/itimer 实现互斥链接。
 * 它不是把全部入口统一退化为 ENOSYS，而是保留四组 clock syscall：只接受 REALTIME、MONOTONIC 与
 * BOOTTIME，提供墙钟设置、读时、分辨率和 hrtimer 睡眠；POSIX interval timer、CPU/dynamic clock、
 * adjtime 与传统 itimer 不在本对象实现。native 与可选 time32 路径共享同一内核 timespec64 语义，区别只在
 * 用户 ABI 转换和 restart_block 的剩余时间指针类型。文件没有持久对象；并发、时间序列与睡眠生命周期由
 * timekeeping、time namespace 和 hrtimer 层承担。
 */

#include <linux/linkage.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/syscalls.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/posix-timers.h>
#include <linux/time_namespace.h>
#include <linux/compat.h>

/*
 * We preserve minimal support for CLOCK_REALTIME and CLOCK_MONOTONIC
 * as it is easy to remain compatible with little code. CLOCK_BOOTTIME
 * is also included for convenience as at least systemd uses it.
 */
/*
 * 即使关闭完整 POSIX timer，也以少量代码保留 REALTIME、MONOTONIC 兼容，并因 systemd 等用户加入
 * BOOTTIME。其余固定、CPU 与 fd 动态 clock 均在各入口的白名单处返回 -EINVAL。
 */

/*
 * clock_settime() - 禁配构建的 native 墙钟设置入口。
 * 仅 CLOCK_REALTIME 合法，其他 id 先返回 -EINVAL；@tp 复制失败返回 -EFAULT。成功转换后交
 * do_sys_settimeofday64 执行范围、权限、timekeeping 提交与通知，本层不持锁也不回写用户内存。
 */
SYSCALL_DEFINE2(clock_settime, const clockid_t, which_clock,
		const struct __kernel_timespec __user *, tp)
{
	struct timespec64 new_tp;

	if (which_clock != CLOCK_REALTIME)
		return -EINVAL;
	if (get_timespec64(&new_tp, tp))
		return -EFAULT;

	return do_sys_settimeofday64(&new_tp, NULL);
}

/*
 * do_clock_gettime() - 把三个保留 clock 读取为调用者 time namespace 中的 timespec64。
 * REALTIME 直接使用命名空间共享的墙钟；MONOTONIC/BOOTTIME 先取 host 值再加 current namespace 对应
 * offset。@tp 是内核指针且仅在合法 id 时写入，成功返回 0，其他 id 返回 -EINVAL；读取一致性由 getter 保证。
 */
static int do_clock_gettime(clockid_t which_clock, struct timespec64 *tp)
{
	switch (which_clock) {
	case CLOCK_REALTIME:
		ktime_get_real_ts64(tp);
		break;
	case CLOCK_MONOTONIC:
		ktime_get_ts64(tp);
		timens_add_monotonic(tp);
		break;
	case CLOCK_BOOTTIME:
		ktime_get_boottime_ts64(tp);
		timens_add_boottime(tp);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * clock_gettime() - native 用户 ABI 的保留 clock 读取包装。
 * 先调用 do_clock_gettime，非法 id 原样返回且不复制；成功后把完整 timespec64 写入 @tp，copy fault 返回
 * -EFAULT。局部快照已脱离 timekeeping 数据结构，用户复制期间不持时间锁。
 */
SYSCALL_DEFINE2(clock_gettime, const clockid_t, which_clock,
		struct __kernel_timespec __user *, tp)
{
	int ret;
	struct timespec64 kernel_tp;

	ret = do_clock_gettime(which_clock, &kernel_tp);
	if (ret)
		return ret;

	if (put_timespec64(&kernel_tp, tp))
		return -EFAULT;
	return 0;
}

/*
 * clock_getres() - 报告三个保留 clock 的软件定时分辨率。
 * 合法 id 均返回 `{0, hrtimer_resolution}`，其他 id 返回 -EINVAL；本 stub 无条件写 @tp，因此 NULL 或不可写
 * 指针返回 -EFAULT，这一点比完整 POSIX 实现允许 NULL 仅做能力探测更窄。报告值不等于底层 clocksource 精度。
 */
SYSCALL_DEFINE2(clock_getres, const clockid_t, which_clock, struct __kernel_timespec __user *, tp)
{
	struct timespec64 rtn_tp = {
		.tv_sec = 0,
		.tv_nsec = hrtimer_resolution,
	};

	switch (which_clock) {
	case CLOCK_REALTIME:
	case CLOCK_MONOTONIC:
	case CLOCK_BOOTTIME:
		if (put_timespec64(&rtn_tp, tp))
			return -EFAULT;
		return 0;
	default:
		return -EINVAL;
	}
}

/*
 * clock_nanosleep() - 三个保留 clock 的 native 高分辨率睡眠入口。
 * clock 白名单、@rqtp 复制与规范 timespec 校验失败分别返回 -EINVAL/-EFAULT/-EINVAL。绝对请求不定义剩余
 * 时间，清空 @rmtp；相对请求按其是否非空记录 TT_NATIVE 与用户指针。先将 restart fn 置 no-restart，随后
 * hrtimer_nanosleep 可在相对睡眠被信号中断时改写为绝对期限 restart；绝对 MONOTONIC/BOOTTIME 期限先从
 * current time namespace 转 host，REALTIME 转换保持原值。未知 flag 位被忽略，仅 TIMER_ABSTIME 有效。
 */
SYSCALL_DEFINE4(clock_nanosleep, const clockid_t, which_clock, int, flags,
		const struct __kernel_timespec __user *, rqtp,
		struct __kernel_timespec __user *, rmtp)
{
	struct timespec64 t;
	ktime_t texp;

	switch (which_clock) {
	case CLOCK_REALTIME:
	case CLOCK_MONOTONIC:
	case CLOCK_BOOTTIME:
		break;
	default:
		return -EINVAL;
	}

	if (get_timespec64(&t, rqtp))
		return -EFAULT;
	if (!timespec64_valid(&t))
		return -EINVAL;
	if (flags & TIMER_ABSTIME)
		rmtp = NULL;
	current->restart_block.fn = do_no_restart_syscall;
	current->restart_block.nanosleep.type = rmtp ? TT_NATIVE : TT_NONE;
	current->restart_block.nanosleep.rmtp = rmtp;
	texp = timespec64_to_ktime(t);
	if (flags & TIMER_ABSTIME)
		texp = timens_ktime_to_host(which_clock, texp);
	return hrtimer_nanosleep(texp, flags & TIMER_ABSTIME ?
				 HRTIMER_MODE_ABS : HRTIMER_MODE_REL,
				 which_clock);
}

#ifdef CONFIG_COMPAT_32BIT_TIME

/*
 * clock_settime32() - time32 ABI 的禁配墙钟设置入口。
 * 仅 CLOCK_REALTIME 合法；把 old_timespec32 扩展成 timespec64，复制失败 -EFAULT，其余权限、范围与提交错误
 * 由 do_sys_settimeofday64 原样返回。转换发生在局部对象中，不保留用户指针。
 */
SYSCALL_DEFINE2(clock_settime32, const clockid_t, which_clock,
		struct old_timespec32 __user *, tp)
{
	struct timespec64 new_tp;

	if (which_clock != CLOCK_REALTIME)
		return -EINVAL;
	if (get_old_timespec32(&new_tp, tp))
		return -EFAULT;

	return do_sys_settimeofday64(&new_tp, NULL);
}

/*
 * clock_gettime32() - 复用 do_clock_gettime 后窄化写回 old_timespec32。
 * 非白名单 id 原样返回 -EINVAL且不写 @tp；成功快照的兼容 ABI copy 失败返回 -EFAULT。若秒值超出 time32
 * 表示范围，具体窄化行为由 put_old_timespec32 的 ABI helper 定义，本层不另做截断判断。
 */
SYSCALL_DEFINE2(clock_gettime32, clockid_t, which_clock,
		struct old_timespec32 __user *, tp)
{
	int ret;
	struct timespec64 kernel_tp;

	ret = do_clock_gettime(which_clock, &kernel_tp);
	if (ret)
		return ret;

	if (put_old_timespec32(&kernel_tp, tp))
		return -EFAULT;
	return 0;
}

/*
 * clock_getres_time32() - time32 ABI 的保留 clock 分辨率查询。
 * 三个合法 id 均尝试把 `{0, hrtimer_resolution}` 写入 @tp，失败 -EFAULT；其他 id -EINVAL。与 native stub
 * 相同，它没有完整实现中的 `tp == NULL` 能力探测分支，NULL 会作为不可写用户地址处理。
 */
SYSCALL_DEFINE2(clock_getres_time32, clockid_t, which_clock,
		struct old_timespec32 __user *, tp)
{
	struct timespec64 rtn_tp = {
		.tv_sec = 0,
		.tv_nsec = hrtimer_resolution,
	};

	switch (which_clock) {
	case CLOCK_REALTIME:
	case CLOCK_MONOTONIC:
	case CLOCK_BOOTTIME:
		if (put_old_timespec32(&rtn_tp, tp))
			return -EFAULT;
		return 0;
	default:
		return -EINVAL;
	}
}

/*
 * clock_nanosleep_time32() - time32 版高分辨率睡眠与 restart 元数据包装。
 * clock/输入/flags/time namespace/返回语义与 native 入口相同；差异仅是 old_timespec32 输入转换，以及相对
 * 请求把剩余时间目标记为 TT_COMPAT/compat_rmtp，使 nanosleep_copyout 按 32 位布局写回。函数不长期持有
 * rqtp；rmtp 仅由当前任务 restart_block 保存，绝对请求将其清空。
 */
SYSCALL_DEFINE4(clock_nanosleep_time32, clockid_t, which_clock, int, flags,
		struct old_timespec32 __user *, rqtp,
		struct old_timespec32 __user *, rmtp)
{
	struct timespec64 t;
	ktime_t texp;

	switch (which_clock) {
	case CLOCK_REALTIME:
	case CLOCK_MONOTONIC:
	case CLOCK_BOOTTIME:
		break;
	default:
		return -EINVAL;
	}

	if (get_old_timespec32(&t, rqtp))
		return -EFAULT;
	if (!timespec64_valid(&t))
		return -EINVAL;
	if (flags & TIMER_ABSTIME)
		rmtp = NULL;
	current->restart_block.fn = do_no_restart_syscall;
	current->restart_block.nanosleep.type = rmtp ? TT_COMPAT : TT_NONE;
	current->restart_block.nanosleep.compat_rmtp = rmtp;
	texp = timespec64_to_ktime(t);
	if (flags & TIMER_ABSTIME)
		texp = timens_ktime_to_host(which_clock, texp);
	return hrtimer_nanosleep(texp, flags & TIMER_ABSTIME ?
				 HRTIMER_MODE_ABS : HRTIMER_MODE_REL,
				 which_clock);
}
#endif
