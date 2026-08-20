// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  This file contains the interface functions for the various time related
 *  system calls: time, stime, gettimeofday, settimeofday, adjtime
 *
 * Modification history:
 *
 * 1993-09-02    Philip Gladstone
 *      Created file with time related functions from sched/core.c and adjtimex()
 * 1993-10-08    Torsten Duwe
 *      adjtime interface update and CMOS clock write code
 * 1995-08-13    Torsten Duwe
 *      kernel PLL updated to 1994-12-13 specs (rfc-1589)
 * 1999-01-16    Ulrich Windl
 *	Introduced error checking for many cases in adjtimex().
 *	Updated NTP code according to technical memorandum Jan '96
 *	"A Kernel Model for Precision Timekeeping" by Dave Mills
 *	Allow time_constant larger than MAXTC(6) for NTP v4 (MAXTC == 10)
 *	(Even though the technical memorandum forbids it)
 * 2004-07-14	 Christoph Lameter
 *	Added getnstimeofday to allow the posix timer functions to return
 *	with nanosecond accuracy
 */

/*
 * 本文件集中承接时间相关系统调用的 ABI 边界，并提供内核各子系统共用的时间单位换算。
 * 历史记录依次说明：这些入口最初从调度代码拆出，随后补入 adjtimex/CMOS、内核 PLL、
 * NTP 校验和纳秒精度支持。它们解释了为何旧 time/stime、32 位兼容结构和现代 64 位
 * timespec 会同时存在；许可证与作者名单保持原样，历史条目的技术含义在此完整补充。
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/timex.h>
#include <linux/capability.h>
#include <linux/timekeeper_internal.h>
#include <linux/errno.h>
#include <linux/syscalls.h>
#include <linux/security.h>
#include <linux/fs.h>
#include <linux/math64.h>
#include <linux/ptrace.h>

#include <linux/uaccess.h>
#include <linux/compat.h>
#include <asm/unistd.h>

#include <generated/timeconst.h>
#include "timekeeping.h"

/*
 * The timezone where the local system is located.  Used as a default by some
 * programs who obtain this value by using gettimeofday.
 */
/*
 * sys_tz 保存本机时区相对 UTC 的分钟偏移和夏令时类型；gettimeofday() 可把它复制给旧程序。
 * 它是长期存在的全局兼容状态，由 settimeofday 路径写入并由读取路径借用；本变量自身不携带
 * 引用或所有权。现代用户空间通常自行处理时区，内核保留它主要是 ABI 与启动期校时兼容需要。
 */
struct timezone sys_tz;

EXPORT_SYMBOL(sys_tz);

#ifdef __ARCH_WANT_SYS_TIME

/*
 * sys_time() can be implemented in user-level using
 * sys_gettimeofday().  Is this for backwards compatibility?  If so,
 * why not move it into the appropriate arch directory (for those
 * architectures that need it).
 */
/*
 * time() - 为仍选择 __ARCH_WANT_SYS_TIME 的体系结构提供旧秒级实时时钟系统调用。
 * 调用链为用户 ABI wrapper → ktime_get_real_seconds()；无锁快照由 timekeeping 层保证，
 * 本函数处于进程上下文且用户拷贝可能缺页，所以允许睡眠。
 * @tloc 是可空的用户输出指针，非 NULL 时接收截断到 __kernel_old_time_t 的 Unix 秒；仅借用，
 * 不转移所有权。局部 i 保存同一秒值。成功返回该秒值并强制按成功解释；写用户地址失败返回
 * -EFAULT，此时没有内核资源需要回滚。旧接口可在用户态用 gettimeofday() 等价实现，保留它是
 * 体系结构兼容选择；返回后由 syscall wrapper 回到用户态。
 */
SYSCALL_DEFINE1(time, __kernel_old_time_t __user *, tloc)
{
	__kernel_old_time_t i = (__kernel_old_time_t)ktime_get_real_seconds();

	/* 先取得一致的实时时钟秒快照；可选输出和直接返回必须使用同一个 i，避免跨秒不一致。 */
	if (tloc) {
		/* put_user() 校验并写用户地址；失败只影响输出，内核未取得任何长期资源。 */
		if (put_user(i,tloc))
			return -EFAULT;
	}
	/* 某些 ABI 会把负的时间值误判为 errno；该标记声明这里始终是成功返回值。 */
	force_successful_syscall_return();
	return i;
}

/*
 * sys_stime() can be implemented in user-level using
 * sys_settimeofday().  Is this for backwards compatibility?  If so,
 * why not move it into the appropriate arch directory (for those
 * architectures that need it).
 */

/*
 * stime() - 把旧 ABI 提供的整秒值转换为 timespec64 后设置系统实时时钟。
 * 调用链为用户 ABI wrapper → security_settime64() → do_settimeofday64()；它运行于可睡眠的
 * 进程上下文，不要求调用者持锁，timekeeping 核心自行串行化写入。
 * @tptr 是必需的用户输入指针，仅借用；其值按 Unix 秒读取，纳秒固定为 0。tv 是内核规范格式，
 * err 传递安全策略拒绝原因。用户读取失败返回 -EFAULT，LSM/权限检查失败原样返回错误码。
 * 当前源码忽略 do_settimeofday64() 返回值：可提交秒成功设时；越界或破坏 monotonic 基点的值
 * 若被核心以 -EINVAL 拒绝，本旧入口仍返回 0，这是必须按现状理解的遗留 ABI 行为。函数不持有引用；旧接口
 * 可由 settimeofday() 实现，保留范围受 __ARCH_WANT_SYS_TIME 控制。
 */

SYSCALL_DEFINE1(stime, __kernel_old_time_t __user *, tptr)
{
	struct timespec64 tv;
	int err;

	/* 阶段 1：把可能故障的用户访问隔离在授权和时间写入之前。 */
	if (get_user(tv.tv_sec, tptr))
		return -EFAULT;

	tv.tv_nsec = 0;

	/* 阶段 2：LSM/能力策略先审批完整目标时间；拒绝时 timekeeper 保持不变。 */
	err = security_settime64(&tv, NULL);
	if (err)
		return err;

	/* 阶段 3：核心 helper 尝试锁定并设时；本旧入口不检查其返回值，最终固定返回 0。 */
	do_settimeofday64(&tv);
	return 0;
}

#endif /* __ARCH_WANT_SYS_TIME */

#ifdef CONFIG_COMPAT_32BIT_TIME
#ifdef __ARCH_WANT_SYS_TIME32

/* old_time32_t is a 32 bit "long" and needs to get converted. */
/*
 * old_time32_t 是 32 位 long，因此必须显式截断/扩展；这也意味着其墙上时间表示受 2038 年范围
 * 限制。time32() 是 time() 的 32 位时间 ABI 镜像：@tloc 为可空、仅借用的用户输出指针，接收
 * Unix 秒；函数可睡眠，不要求外部锁。局部 i 固定同一次快照。成功返回 i，用户写失败返回
 * -EFAULT，无资源和 ownership 变化；仅在 CONFIG_COMPAT_32BIT_TIME 与架构请求该入口时构建。
 */
SYSCALL_DEFINE1(time32, old_time32_t __user *, tloc)
{
	old_time32_t i;

	/* 先读取 64 位实时时钟，再按旧 ABI 明确收窄；输出与返回共享这个快照。 */
	i = (old_time32_t)ktime_get_real_seconds();

	if (tloc) {
		if (put_user(i,tloc))
			return -EFAULT;
	}
	force_successful_syscall_return();
	return i;
}

/*
 * stime32() - 从 32 位旧时间 ABI 读取整秒并提交为系统实时时钟。
 * 它是 stime() 的兼容镜像，调用者为 compat syscall wrapper，后续依次进入安全策略与
 * do_settimeofday64()。@tptr 为必需、仅借用的用户输入，单位为秒；tv 承载扩展后的 64 位秒和
 * 零纳秒，err 保存策略错误。进程上下文可睡眠，不要求外部锁。用户/策略失败返回 -EFAULT 或
 * 策略 errno；当前源码忽略核心设时返回，所以核心拒绝越界/非法基点时仍返回 0，可提交值才实际设时。
 * 无引用转移，配置关闭时整个入口不存在。
 */
SYSCALL_DEFINE1(stime32, old_time32_t __user *, tptr)
{
	struct timespec64 tv;
	int err;

	/* 先完成用户读取和规范格式构造，避免在授权后再次触碰不可信输入。 */
	if (get_user(tv.tv_sec, tptr))
		return -EFAULT;

	tv.tv_nsec = 0;

	/* 安全检查成功才尝试跨越写时钟边界；核心 helper 负责同步，但其 errno 在此被旧 ABI 丢弃。 */
	err = security_settime64(&tv, NULL);
	if (err)
		return err;

	do_settimeofday64(&tv);
	return 0;
}

#endif /* __ARCH_WANT_SYS_TIME32 */
#endif

/*
 * gettimeofday() - 向旧 timeval/timezone ABI 导出当前实时时钟和兼容时区。
 * 用户 syscall wrapper 调用本函数；ktime_get_real_ts64() 提供一致快照，随后返回用户态。
 * @tv 为可空用户输出，接收秒和截断为微秒的纳秒；@tz 为可空用户输出，接收 sys_tz 快照；两者
 * 都只是借用地址。函数在可睡眠进程上下文运行，不要求外部锁；ts 仅在 tv 分支有效。任一用户写
 * 失败返回 -EFAULT，可能出现前一字段已写而后一字段失败的部分输出；成功返回 0。读取不修改
 * timekeeper、sys_tz 或所有权，likely/unlikely 仅提示现代调用通常要时间而不要旧时区。
 */
SYSCALL_DEFINE2(gettimeofday, struct __kernel_old_timeval __user *, tv,
		struct timezone __user *, tz)
{
	/* 阶段 1：仅在请求时间时采样；两个 put_user 不构成原子用户内存事务。 */
	if (likely(tv != NULL)) {
		struct timespec64 ts;

		ktime_get_real_ts64(&ts);
		if (put_user(ts.tv_sec, &tv->tv_sec) ||
		    put_user(ts.tv_nsec / 1000, &tv->tv_usec))
			return -EFAULT;
	}
	/* 阶段 2：旧时区是独立可选输出；即使 tv 已成功，时区拷贝仍可能使调用整体报错。 */
	if (unlikely(tz != NULL)) {
		if (copy_to_user(tz, &sys_tz, sizeof(sys_tz)))
			return -EFAULT;
	}
	return 0;
}

/*
 * In case for some reason the CMOS clock has not already been running
 * in UTC, but in some local time: The first time we set the timezone,
 * we will warp the clock so that it is ticking UTC time instead of
 * local time. Presumably, if someone is setting the timezone then we
 * are running in an environment where the programs understand about
 * timezones. This should be done at boot time in the /etc/rc script,
 * as soon as possible, so that the clock can be set right. Otherwise,
 * various programs will get confused when the clock gets warped.
 */
/*
 * 若固件 CMOS/RTC 历史上保存的是本地时间而非 UTC，首次只设置时区时，内核会按该偏移把墙钟
 * 扭转到 UTC。注释要求启动脚本尽早且只做一次，否则已经运行的程序会观察到时间跳变；现代系统
 * 应让 RTC 直接保存 UTC，从源头避免这项兼容行为。
 *
 * do_sys_settimeofday64() - 汇合 POSIX/旧 ABI 的设时与设时区请求并完成验证和提交。
 * 主要调用者是 settimeofday() 及 CLOCK_REALTIME 的 settime 实现；成功后分别由 timekeeping、
 * vDSO 和用户态观察新状态。@tv 为可空、借用的规范 timespec64 输入，表示绝对 Unix 时间；@tz
 * 为可空、借用的时区输入，分钟偏移限于正负 15 小时。函数在可睡眠进程上下文运行，不要求调用者
 * 持锁；核心设时 helper 自行同步，而 sys_tz/firsttime 是启动期兼容状态。
 * firsttime 表示尚未执行首次时区兼容处理，error 保存策略返回。无效时间/时区返回 -EINVAL，
 * 安全策略错误原样返回，do_settimeofday64() 的错误也原样返回；成功返回 0。时区分支会更新
 * sys_tz 和 vDSO，首次且未同时给 tv 时还会 warp 墙钟。若时间通过入口格式校验、却随后因
 * monotonic 基点约束被核心拒绝，时区已经发布且不会回滚；所有路径均无引用转移。
 */

int do_sys_settimeofday64(const struct timespec64 *tv, const struct timezone *tz)
{
	static int firsttime = 1;
	int error = 0;

	/* 阶段 1：在安全 hook 和任何全局写入前验证秒范围及 0 <= tv_nsec < 1 秒。 */
	if (tv && !timespec64_valid_settod(tv))
		return -EINVAL;

	/* 阶段 2：LSM 同时看到时间与时区意图；拒绝时二者都尚未提交。 */
	error = security_settime64(tv, tz);
	if (error)
		return error;

	if (tz) {
		/* Verify we're within the +-15 hrs range */
		/* 时区西偏分钟必须在正负 15 小时内，防止不合理偏移进入全局兼容状态。 */
		if (tz->tz_minuteswest > 15*60 || tz->tz_minuteswest < -15*60)
			return -EINVAL;

		/* 阶段 3：先发布 sys_tz，再同步 vDSO；旧 gettimeofday 读者随后可见新时区。 */
		sys_tz = *tz;
		update_vsyscall_tz();
		/* 首次纯时区请求才把假定为本地时间的启动墙钟校成 UTC；显式 tv 已给出正确绝对时间。 */
		if (firsttime) {
			firsttime = 0;
			if (!tv)
				timekeeping_warp_clock();
		}
	}
	/* 阶段 4：显式时间是最后的提交动作；NULL 表示调用仅更新时区。 */
	if (tv)
		return do_settimeofday64(tv);
	return 0;
}

/*
 * settimeofday() - 把旧 timeval/timezone 用户结构转换为内核格式并交给统一设时入口。
 * 用户 syscall wrapper 调用它，下一步是 do_sys_settimeofday64()。@tv、@tz 都是可空、仅借用的
 * 用户输入；tv_sec 为 Unix 秒，tv_usec 必须在 [0, 1000000) 后换成纳秒，tz 直接复制到栈。
 * new_ts/new_tz 仅在对应参数非空时有效。函数在可睡眠进程上下文运行，不持锁；用户读取失败返回
 * -EFAULT，微秒非法返回 -EINVAL，其余状态码由统一入口返回。转换阶段无全局副作用；统一入口
 * 成功后墙上时间和/或 sys_tz 已提交，无 ownership 转移，随后返回用户态。
 */
SYSCALL_DEFINE2(settimeofday, struct __kernel_old_timeval __user *, tv,
		struct timezone __user *, tz)
{
	struct timespec64 new_ts;
	struct timezone new_tz;

	/* 阶段 1：完整取入 timeval，先按微秒单位校验，再原地换算为纳秒。 */
	if (tv) {
		if (get_user(new_ts.tv_sec, &tv->tv_sec) ||
		    get_user(new_ts.tv_nsec, &tv->tv_usec))
			return -EFAULT;

		if (new_ts.tv_nsec >= USEC_PER_SEC || new_ts.tv_nsec < 0)
			return -EINVAL;

		new_ts.tv_nsec *= NSEC_PER_USEC;
	}
	/* 阶段 2：独立取入可选时区；到这里仍只修改栈对象。 */
	if (tz) {
		if (copy_from_user(&new_tz, tz, sizeof(*tz)))
			return -EFAULT;
	}

	/* 阶段 3：用原用户指针是否为空选择栈对象，统一执行授权、兼容 warp 与最终提交。 */
	return do_sys_settimeofday64(tv ? &new_ts : NULL, tz ? &new_tz : NULL);
}

#ifdef CONFIG_COMPAT
/*
 * compat_gettimeofday() - 为 32 位进程导出旧 32 位 timeval 与公共 timezone。
 * compat syscall wrapper 调用它；@tv/@tz 均为可空、仅借用的用户输出。ts 是 64 位实时时钟快照，
 * 秒写入 32 位 ABI 字段时按目标类型表示，纳秒截断成微秒。进程上下文可睡眠且不要求锁；任一
 * copy 返回 -EFAULT，并可能留下较早字段的部分输出；成功返回 0。函数只读取全局时间/时区，
 * 不修改对象或转移所有权；仅在 CONFIG_COMPAT 下存在。
 */
COMPAT_SYSCALL_DEFINE2(gettimeofday, struct old_timeval32 __user *, tv,
		       struct timezone __user *, tz)
{
	/* 先完成时间输出，再处理独立时区输出；两组用户写都不具备事务性。 */
	if (tv) {
		struct timespec64 ts;

		ktime_get_real_ts64(&ts);
		if (put_user(ts.tv_sec, &tv->tv_sec) ||
		    put_user(ts.tv_nsec / 1000, &tv->tv_usec))
			return -EFAULT;
	}
	if (tz) {
		if (copy_to_user(tz, &sys_tz, sizeof(sys_tz)))
			return -EFAULT;
	}

	return 0;
}

/*
 * compat_settimeofday() - 将 32 位旧 timeval 与 timezone 搬到栈上后调用统一设时入口。
 * @tv/@tz 是可空、仅借用的用户输入；new_ts 保存扩展后的秒和由微秒换算的纳秒，new_tz 保存
 * 时区副本。调用链、可睡眠上下文和无外部锁要求与原生 settimeofday() 相同。用户访问失败返回
 * -EFAULT，微秒越界返回 -EINVAL，统一入口还可能返回权限或时间校验错误；成功提交时间和/或
 * 时区并返回 0。转换失败发生在全局提交前，无资源回滚；仅在 CONFIG_COMPAT 下构建。
 */
COMPAT_SYSCALL_DEFINE2(settimeofday, struct old_timeval32 __user *, tv,
		       struct timezone __user *, tz)
{
	struct timespec64 new_ts;
	struct timezone new_tz;

	/* 先校验旧 ABI 的微秒原值，避免乘成纳秒后掩盖负值或一整秒的非法边界。 */
	if (tv) {
		if (get_user(new_ts.tv_sec, &tv->tv_sec) ||
		    get_user(new_ts.tv_nsec, &tv->tv_usec))
			return -EFAULT;

		if (new_ts.tv_nsec >= USEC_PER_SEC || new_ts.tv_nsec < 0)
			return -EINVAL;

		new_ts.tv_nsec *= NSEC_PER_USEC;
	}
	/* 时区仍使用公共结构布局；先完整复制，再把两个栈对象一起交给统一入口。 */
	if (tz) {
		if (copy_from_user(&new_tz, tz, sizeof(*tz)))
			return -EFAULT;
	}

	return do_sys_settimeofday64(tv ? &new_ts : NULL, tz ? &new_tz : NULL);
}
#endif

#ifdef CONFIG_64BIT
/*
 * adjtimex() - 在原生 64 位 ABI 与内核 NTP 调整核心之间双向搬运 __kernel_timex。
 * 用户 syscall wrapper 调用本函数，do_adjtimex() 校验/读取/修改 NTP 状态并把查询结果写回 txc。
 * @txc_p 是必需的用户输入输出指针，仅在调用期间借用；txc 是完整内核副本，ret 是 NTP 状态码
 * 或负 errno。函数在可睡眠进程上下文运行，不要求外部锁，核心自行锁定 timekeeper。
 * 首次复制失败返回 -EFAULT 且无状态变化；核心可能已经提交调整后，若回写用户失败仍返回
 * -EFAULT，不能撤销该副作用；回写成功返回 do_adjtimex() 的值。仅 64 位构建使用此原生布局。
 */
SYSCALL_DEFINE1(adjtimex, struct __kernel_timex __user *, txc_p)
{
	struct __kernel_timex txc;		/* Local copy of parameter */
	/* txc 是参数的本地副本，隔离用户并发修改；核心同时把当前 NTP 状态填回其中。 */
	int ret;

	/* Copy the user data space into the kernel copy
	 * structure. But bear in mind that the structures
	 * may change
	 */
	/*
	 * 先把用户数据空间复制到内核副本；同时必须牢记 ABI 结构会演进，因此只能按当前固定布局
	 * 搬运，不能让核心直接解引用用户指针。
	 */
	if (copy_from_user(&txc, txc_p, sizeof(struct __kernel_timex)))
		return -EFAULT;
	/* 核心调用是状态提交边界；其返回后 txc 也包含需要回传的查询/统计字段。 */
	ret = do_adjtimex(&txc);
	/* 回写失败优先报告地址错误，即使核心调整已经生效；该副作用无法在此回滚。 */
	return copy_to_user(txc_p, &txc, sizeof(struct __kernel_timex)) ? -EFAULT : ret;
}
#endif

#ifdef CONFIG_COMPAT_32BIT_TIME
/*
 * get_old_timex32() - 把用户 old_timex32 请求扩展成内核 __kernel_timex。
 * adjtimex_time32() 调用它，成功后交给 do_adjtimex()。@txc 是必需的内核输出对象，由调用者拥有；
 * @utp 是必需、借用的用户输入。tx32 是一次性 ABI 快照。函数在可睡眠进程上下文运行且不持锁。
 * 先清零完整输出，使旧结构没有的字段保持确定的 0；用户复制失败返回 -EFAULT，此时 @txc 仍为
 * 全零，成功逐字段扩展控制量、误差、PPS 统计和时间并返回 0。它只转换布局，不验证 NTP 语义、
 * 不修改全局时钟，也不转移所有权；仅在 CONFIG_COMPAT_32BIT_TIME 下构建。
 */
int get_old_timex32(struct __kernel_timex *txc, const struct old_timex32 __user *utp)
{
	struct old_timex32 tx32;

	/* 阶段 1：清零新版结构全部字段，再一次性快照用户旧结构，避免 padding/新增字段泄漏。 */
	memset(txc, 0, sizeof(struct __kernel_timex));
	if (copy_from_user(&tx32, utp, sizeof(struct old_timex32)))
		return -EFAULT;

	/* 阶段 2：搬运请求模式、频率/误差控制和当前时间；整数赋值按目标字段自然扩展。 */
	txc->modes = tx32.modes;
	txc->offset = tx32.offset;
	txc->freq = tx32.freq;
	txc->maxerror = tx32.maxerror;
	txc->esterror = tx32.esterror;
	txc->status = tx32.status;
	txc->constant = tx32.constant;
	txc->precision = tx32.precision;
	txc->tolerance = tx32.tolerance;
	txc->time.tv_sec = tx32.time.tv_sec;
	txc->time.tv_usec = tx32.time.tv_usec;
	txc->tick = tx32.tick;
	/* 阶段 3：搬运 PPS 频率、抖动、稳定性及计数器；旧 ABI 没有的 tai 等字段保持 0。 */
	txc->ppsfreq = tx32.ppsfreq;
	txc->jitter = tx32.jitter;
	txc->shift = tx32.shift;
	txc->stabil = tx32.stabil;
	txc->jitcnt = tx32.jitcnt;
	txc->calcnt = tx32.calcnt;
	txc->errcnt = tx32.errcnt;
	txc->stbcnt = tx32.stbcnt;

	return 0;
}

/*
 * put_old_timex32() - 把 do_adjtimex() 产出的内核 timex 快照收窄并复制回旧 32 位 ABI。
 * adjtimex_time32() 调用它。@utp 是必需、借用的用户输出；@txc 是必需、借用且不会被修改的内核
 * 输入；tx32 是清零后的旧布局临时对象。函数可睡眠、不要求锁。所有可表示字段逐项复制，包括
 * 查询返回的 TAI 偏移；新版独有字段和 padding 保持 0。copy_to_user 失败返回 -EFAULT，用户区
 * 可能部分写入；成功返回 0。它不回滚此前 NTP 调整，不取得引用，仅在兼容时间 ABI 下构建。
 */
int put_old_timex32(struct old_timex32 __user *utp, const struct __kernel_timex *txc)
{
	struct old_timex32 tx32;

	/* 先清零整个对外结构，确保未显式赋值的字段和 padding 不暴露栈内容。 */
	memset(&tx32, 0, sizeof(struct old_timex32));
	tx32.modes = txc->modes;
	tx32.offset = txc->offset;
	tx32.freq = txc->freq;
	tx32.maxerror = txc->maxerror;
	tx32.esterror = txc->esterror;
	tx32.status = txc->status;
	tx32.constant = txc->constant;
	tx32.precision = txc->precision;
	tx32.tolerance = txc->tolerance;
	tx32.time.tv_sec = txc->time.tv_sec;
	tx32.time.tv_usec = txc->time.tv_usec;
	tx32.tick = txc->tick;
	/* 后半组是 PPS 观测统计与 TAI 偏移；赋值按旧字段宽度形成兼容表示。 */
	tx32.ppsfreq = txc->ppsfreq;
	tx32.jitter = txc->jitter;
	tx32.shift = txc->shift;
	tx32.stabil = txc->stabil;
	tx32.jitcnt = txc->jitcnt;
	tx32.calcnt = txc->calcnt;
	tx32.errcnt = txc->errcnt;
	tx32.stbcnt = txc->stbcnt;
	tx32.tai = txc->tai;
	if (copy_to_user(utp, &tx32, sizeof(struct old_timex32)))
		return -EFAULT;
	return 0;
}

/*
 * adjtimex_time32() - 编排旧 32 位 timex 的取入、NTP 核心处理和结果回写。
 * 32 位 syscall wrapper 调用它；get_old_timex32() → do_adjtimex() → put_old_timex32() 构成完整
 * 路径。@utp 是必需的借用用户输入输出；txc 是内核规范副本，err 保存搬运错误，ret 保存 NTP
 * 状态或错误。进程上下文可睡眠、不要求外部锁。取入失败返回 -EFAULT 且无核心副作用；核心调用
 * 后调整可能已提交，回写失败返回 -EFAULT 且不能回滚；全部成功返回 ret。无引用/所有权转移，
 * CONFIG_COMPAT_32BIT_TIME 关闭时入口不存在。
 */
SYSCALL_DEFINE1(adjtimex_time32, struct old_timex32 __user *, utp)
{
	struct __kernel_timex txc;
	int err, ret;

	/* 阶段 1：先稳定用户输入；失败时禁止进入会改变 NTP 状态的核心。 */
	err = get_old_timex32(&txc, utp);
	if (err)
		return err;

	/* 阶段 2：核心既可能查询也可能提交调整，并把最终状态更新在 txc 中。 */
	ret = do_adjtimex(&txc);

	/* 阶段 3：无论 ret 是状态还是核心 errno，仍按 ABI 尝试回传 txc；拷贝错误优先。 */
	err = put_old_timex32(utp, &txc);
	if (err)
		return err;

	return ret;
}
#endif

#if HZ > MSEC_PER_SEC || (MSEC_PER_SEC % HZ)
/**
 * jiffies_to_msecs - Convert jiffies to milliseconds
 * @j: jiffies value
 *
 * Return: milliseconds value
 */
/*
 * jiffies_to_msecs() - 在头文件无法常量化该 HZ 组合时，把内核 tick 数向上换算为毫秒。
 * 内核超时/统计调用者传入 @j（jiffy 个数，纯值无 ownership），返回覆盖该 tick 区间所需的
 * unsigned 毫秒。函数是纯算术、不可睡眠、无锁且无副作用。编译期按 HZ 与 1000 的整除关系选择
 * 整数除法、32 位定点乘移位或 NUM/DEN 比例；各路径都向上取整，非零短 tick 不会变成 0。
 * 返回类型溢出边界由调用者输入范围承担；配置可由头文件内联完成时本定义不构建。
 */
unsigned int jiffies_to_msecs(const unsigned long j)
{
	/* HZ 高且整除 1000 时，若干 jiffy 才是一毫秒，先加除数减一实现向上取整。 */
#if HZ > MSEC_PER_SEC && !(HZ % MSEC_PER_SEC)
	return (j + (HZ / MSEC_PER_SEC) - 1)/(HZ / MSEC_PER_SEC);
#else
	/* 其余配置使用 timeconst.bc 生成的定点常数；32/64 位分别避免中间值溢出。 */
# if BITS_PER_LONG == 32
	return (HZ_TO_MSEC_MUL32 * j + (1ULL << HZ_TO_MSEC_SHR32) - 1) >>
	       HZ_TO_MSEC_SHR32;
# else
	return DIV_ROUND_UP(j * HZ_TO_MSEC_NUM, HZ_TO_MSEC_DEN);
# endif
#endif
}
EXPORT_SYMBOL(jiffies_to_msecs);
#endif

#if (USEC_PER_SEC % HZ)
/**
 * jiffies_to_usecs - Convert jiffies to microseconds
 * @j: jiffies value
 *
 * Return: microseconds value
 */
/*
 * jiffies_to_usecs() - 对不能由头文件整除路径覆盖的 HZ，把 jiffy 个数换算为微秒。
 * @j 是纯输入 tick 数；返回 unsigned 微秒，供超时和 ABI 换算继续使用。函数是纯算术、无锁、
 * 不睡眠、无状态/ownership 副作用。BUILD_BUG_ON 限定 HZ 不得超过每秒微秒数，这是本函数与
 * usecs_to_jiffies() 算术成立的编译期前提。32 位使用生成的乘数/移位，64 位使用最简比例；结果
 * 按整数算术截断。USEC_PER_SEC 能整除 HZ 时使用头文件快速实现，本实体不参与构建。
 */
unsigned int jiffies_to_usecs(const unsigned long j)
{
	/*
	 * Hz usually doesn't go much further MSEC_PER_SEC.
	 * jiffies_to_usecs() and usecs_to_jiffies() depend on that.
	 */
	/*
	 * HZ 通常不会远高于每秒毫秒数；这两个正反换算共同依赖 HZ <= 每秒微秒数，故用编译期
	 * 断言阻止不受支持的配置，而不是在运行时产生错误结果。
	 */
	BUILD_BUG_ON(HZ > USEC_PER_SEC);

	/* 选择与机器字长匹配的生成常数，避免把同一公式强行用于易溢出的 32 位中间量。 */
#if BITS_PER_LONG == 32
	return (HZ_TO_USEC_MUL32 * j) >> HZ_TO_USEC_SHR32;
#else
	return (j * HZ_TO_USEC_NUM) / HZ_TO_USEC_DEN;
#endif
}
EXPORT_SYMBOL(jiffies_to_usecs);
#endif

/**
 * mktime64 - Converts date to seconds.
 * @year0: year to convert
 * @mon0: month to convert
 * @day: day to convert
 * @hour: hour to convert
 * @min: minute to convert
 * @sec: second to convert
 *
 * Converts Gregorian date to seconds since 1970-01-01 00:00:00.
 * Assumes input in normal date format, i.e. 1980-12-31 23:59:59
 * => year=1980, mon=12, day=31, hour=23, min=59, sec=59.
 *
 * [For the Julian calendar (which was used in Russia before 1917,
 * Britain & colonies before 1752, anywhere else before 1582,
 * and is still in use by some communities) leave out the
 * -year/100+year/400 terms, and add 10.]
 *
 * This algorithm was first published by Gauss (I think).
 *
 * A leap second can be indicated by calling this function with sec as
 * 60 (allowable under ISO 8601).  The leap second is treated the same
 * as the following second since they don't exist in UNIX time.
 *
 * An encoding of midnight at the end of the day as 24:00:00 - ie. midnight
 * tomorrow - (allowable under ISO 8601) is supported.
 *
 * Return: seconds since the epoch time for the given input date
 */
/*
 * mktime64() - 把公历日期按常数时间算法编码为 1970-01-01 00:00:00 起的 Unix 秒。
 * 日期/RTC 等调用者传入纯值：@year0 为完整年份，@mon0 为 1..12 月，@day 为日，@hour/@min/@sec
 * 为时分秒；常规示例是 1980-12-31 23:59:59。输入由调用者保证有效，本函数不查日历边界。
 * mon/year 是把一二月移到计算年末的工作副本。返回 time64_t 秒，无错误码、状态或 ownership
 * 副作用；纯算术、无锁、不可睡眠，可在任意上下文调用。
 *
 * 算法使用 Gregorian 的 year/4-year/100+year/400 闰年项；若计算历史 Julian 日历，应去掉
 * 世纪两项并加 10（俄罗斯 1917、英属地区 1752、其他许多地区 1582 前曾使用，部分社群仍用）。
 * 原注释认为算法最早由 Gauss 发表。ISO 8601 允许 @sec=60 表示闰秒，但 Unix 时间无独立闰秒，
 * 因而与下一秒编码相同；@hour=24 的日末午夜也被支持并自然落到次日 00:00:00。
 */
time64_t mktime64(const unsigned int year0, const unsigned int mon0,
		const unsigned int day, const unsigned int hour,
		const unsigned int min, const unsigned int sec)
{
	unsigned int mon = mon0, year = year0;

	/* 1..12 -> 11,12,1..10 */
	/* 月份映射为 3 月起算的 1..10、一二月 11..12，让闰日处于计算年的最后。 */
	if (0 >= (int) (mon -= 2)) {
		mon += 12;	/* Puts Feb last since it has leap day */
		/* 上行把含闰日的二月放到末尾；一二月属于前一个“计算年”，故 year 同步减一。 */
		year -= 1;
	}

	/*
	 * 先累计到目标日，再依次乘 24/60/60 加时分秒：加 hour 后已是小时，且 hour=24 自然跨日；
	 * 加 min 后得到分钟，最后加 sec 得到秒。719499 把公历累计日平移到 Unix epoch。
	 */
	return ((((time64_t)
		  (year/4 - year/100 + year/400 + 367*mon/12 + day) +
		  year*365 - 719499
	    )*24 + hour /* now have hours - midnight tomorrow handled here */
	  )*60 + min /* now have minutes */
	)*60 + sec; /* finally seconds */
}
EXPORT_SYMBOL(mktime64);

/*
 * ns_to_kernel_old_timeval() - 把有符号纳秒总量转换为旧 timeval 的秒/微秒表示。
 * 时间换算调用者传入 @nsec（相对某基点的总纳秒，纯值）；先由 ns_to_timespec64() 规范化负值，
 * 再把不足一秒部分截断到微秒。ts 是规范中间值，tv 是返回对象；返回完整结构，无错误码。
 * 函数纯算术、无锁、不可睡眠、无副作用或 ownership；低于一微秒的精度被有意丢弃。
 */
struct __kernel_old_timeval ns_to_kernel_old_timeval(s64 nsec)
{
	struct timespec64 ts = ns_to_timespec64(nsec);
	struct __kernel_old_timeval tv;

	tv.tv_sec = ts.tv_sec;
	tv.tv_usec = (suseconds_t)ts.tv_nsec / 1000;

	return tv;
}
EXPORT_SYMBOL(ns_to_kernel_old_timeval);

/**
 * set_normalized_timespec64 - set timespec sec and nsec parts and normalize
 *
 * @ts:		pointer to timespec variable to be set
 * @sec:	seconds to set
 * @nsec:	nanoseconds to set
 *
 * Set seconds and nanoseconds field of a timespec variable and
 * normalize to the timespec storage format
 *
 * Note: The tv_nsec part is always in the range of 0 <= tv_nsec < NSEC_PER_SEC.
 * For negative values only the tv_sec field is negative !
 */
/*
 * set_normalized_timespec64() - 写入并规范化 timespec64，使纳秒字段始终落在单秒范围。
 * 各时间算术 helper 调用它。@ts 是必需、由调用者拥有的纯输出对象；@sec 是起始秒，@nsec 是
 * 可超出一秒或为负的有符号纳秒。函数逐秒把溢出搬到 sec，最终保证
 * 0 <= tv_nsec < NSEC_PER_SEC；负时间只由 tv_sec 为负来表示。返回无直接值，唯一副作用是覆盖
 * @ts 两字段；无错误、引用或 ownership 转移。它是纯 CPU 运算、无锁、不可睡眠，但极大未规范
 * @nsec 会导致多次迭代，因此调用者通常传入接近一秒范围的值。
 */
void set_normalized_timespec64(struct timespec64 *ts, time64_t sec, s64 nsec)
{
	/* 阶段 1：每移走一个正向整秒，就同步增加 sec，保持 sec*1s+nsec 总量不变。 */
	while (nsec >= NSEC_PER_SEC) {
		/*
		 * The following asm() prevents the compiler from
		 * optimising this loop into a modulo operation. See
		 * also __iter_div_u64_rem() in include/linux/time.h
		 */
		/*
		 * 空 asm 声明 nsec 为读写操作数，阻止编译器把循环优化成可能引入昂贵除法的取模；
		 * include/linux/time.h 的 __iter_div_u64_rem() 使用同类技巧。
		 */
		asm("" : "+rm"(nsec));
		nsec -= NSEC_PER_SEC;
		++sec;
	}
	/* 阶段 2：负余数每次借一秒；同样保持总纳秒不变量，直到 nsec 非负。 */
	while (nsec < 0) {
		asm("" : "+rm"(nsec));
		nsec += NSEC_PER_SEC;
		--sec;
	}
	/* 阶段 3：只有完成两向规范化后才提交输出，调用者不会看到中间的不规范表示。 */
	ts->tv_sec = sec;
	ts->tv_nsec = nsec;
}
EXPORT_SYMBOL(set_normalized_timespec64);

/**
 * ns_to_timespec64 - Convert nanoseconds to timespec64
 * @nsec:       the nanoseconds value to be converted
 *
 * Return: the timespec64 representation of the nsec parameter.
 */
/*
 * ns_to_timespec64() - 把有符号总纳秒拆成规范化 timespec64。
 * @nsec 为纯输入总纳秒；ts 初始表示零，rem 保存除以每秒纳秒数的余数。返回值满足原总量且
 * 0 <= tv_nsec < NSEC_PER_SEC，无错误或副作用。函数纯算术、无锁、不可睡眠、无 ownership。
 * 正值走常见除余路径；零直接返回零；负值用 -nsec-1 技巧避开最小 s64 取反溢出，并把表示
 * 向更早的整秒取 floor，例如 -1ns 变为 {-1, 999999999}。
 */
struct timespec64 ns_to_timespec64(s64 nsec)
{
	struct timespec64 ts = { 0, 0 };
	s32 rem;

	/* 正数可直接商作秒、余数作纳秒；likely 提示这是热路径但不改变语义。 */
	if (likely(nsec > 0)) {
		ts.tv_sec = div_u64_rem(nsec, NSEC_PER_SEC, &rem);
		ts.tv_nsec = rem;
	} else if (nsec < 0) {
		/*
		 * With negative times, tv_sec points to the earlier
		 * second, and tv_nsec counts the nanoseconds since
		 * then, so tv_nsec is always a positive number.
		 */
		/*
		 * 对负时间，tv_sec 指向更早的整秒，tv_nsec 表示从该秒起经过的纳秒，因此纳秒字段始终
		 * 非负；减一再取反同时正确处理边界和精确整秒。
		 */
		ts.tv_sec = -div_u64_rem(-nsec - 1, NSEC_PER_SEC, &rem) - 1;
		ts.tv_nsec = NSEC_PER_SEC - rem - 1;
	}

	return ts;
}
EXPORT_SYMBOL(ns_to_timespec64);

/**
 * __msecs_to_jiffies: - convert milliseconds to jiffies
 * @m:	time in milliseconds
 *
 * conversion is done as follows:
 *
 * - negative values mean 'infinite timeout' (MAX_JIFFY_OFFSET)
 *
 * - 'too large' values [that would result in larger than
 *   MAX_JIFFY_OFFSET values] mean 'infinite timeout' too.
 *
 * - all other values are converted to jiffies by either multiplying
 *   the input value by a factor or dividing it with a factor and
 *   handling any 32-bit overflows.
 *   for the details see _msecs_to_jiffies()
 *
 * msecs_to_jiffies() checks for the passed in value being a constant
 * via __builtin_constant_p() allowing gcc to eliminate most of the
 * code, __msecs_to_jiffies() is called if the value passed does not
 * allow constant folding and the actual conversion must be done at
 * runtime.
 * The _msecs_to_jiffies helpers are the HZ dependent conversion
 * routines found in include/linux/jiffies.h
 *
 * Return: jiffies value
 */
/*
 * __msecs_to_jiffies() - 为不能编译期折叠的毫秒输入执行运行时 jiffy 换算。
 * msecs_to_jiffies() 宏在 __builtin_constant_p() 失败时调用本函数；@m 是 unsigned 毫秒纯值。
 * 按历史 API 约定，把最高位被解释成负 int 的值和换算过大的值饱和为 MAX_JIFFY_OFFSET（无限
 * 超时），其余交给 HZ 相关 _msecs_to_jiffies()。返回 unsigned long jiffy，无错误码或副作用。
 * 函数纯算术、无锁、不可睡眠、无 ownership；生成细节位于 include/linux/jiffies.h。
 */
unsigned long __msecs_to_jiffies(const unsigned int m)
{
	/*
	 * Negative value, means infinite timeout:
	 */
	/* 历史调用者可能把负 int 强转为 unsigned 传入；检测符号位并把它解释为无限超时。 */
	if ((int)m < 0)
		return MAX_JIFFY_OFFSET;
	return _msecs_to_jiffies(m);
}
EXPORT_SYMBOL(__msecs_to_jiffies);

/**
 * __usecs_to_jiffies: - convert microseconds to jiffies
 * @u:	time in milliseconds
 *
 * Return: jiffies value
 */
/*
 * __usecs_to_jiffies() - 把运行时微秒输入换算为 jiffy，并对超长结果做饱和。
 * usecs_to_jiffies() 宏在不能常量折叠时调用；@u 是 unsigned 微秒纯值。原英文 @u 误写为
 * milliseconds，当前函数名、jiffies_to_usecs() 边界和 _usecs_to_jiffies() 调用共同证明单位应为
 * 微秒。若超过 MAX_JIFFY_OFFSET 可表示的微秒则返回该无限超时哨兵，否则按 HZ 向上换算。
 * 函数纯算术、无锁、不可睡眠，无错误码、全局副作用或 ownership；返回 unsigned long jiffy。
 */
unsigned long __usecs_to_jiffies(const unsigned int u)
{
	/* 先在微秒域比较上限，避免换算中间值溢出后绕回一个短超时。 */
	if (u > jiffies_to_usecs(MAX_JIFFY_OFFSET))
		return MAX_JIFFY_OFFSET;
	return _usecs_to_jiffies(u);
}
EXPORT_SYMBOL(__usecs_to_jiffies);

/**
 * timespec64_to_jiffies - convert a timespec64 value to jiffies
 * @value: pointer to &struct timespec64
 *
 * The TICK_NSEC - 1 rounds up the value to the next resolution.  Note
 * that a remainder subtract here would not do the right thing as the
 * resolution values don't fall on second boundaries.  I.e. the line:
 * nsec -= nsec % TICK_NSEC; is NOT a correct resolution rounding.
 * Note that due to the small error in the multiplier here, this
 * rounding is incorrect for sufficiently large values of tv_nsec, but
 * well formed timespecs should have tv_nsec < NSEC_PER_SEC, so we're
 * OK.
 *
 * Rather, we just shift the bits off the right.
 *
 * The >> (NSEC_JIFFIE_SC - SEC_JIFFIE_SC) converts the scaled nsec
 * value to a scaled second value.
 *
 * Return: jiffies value
 */
/*
 * timespec64_to_jiffies() - 把规范、非负的 timespec64 超时向上量化为 jiffy。
 * 定时器等待路径传入 @value（必需、借用、只读）；sec 是秒部分的 u64 工作量，nsec 在原纳秒上
 * 加 TICK_NSEC-1 以实现向上取整。函数返回 unsigned long jiffy；过大的秒饱和到
 * MAX_SEC_IN_JIFFIES 对应范围，无错误码。纯算术、无锁、不可睡眠，不改输入或 ownership。
 * 不能用 nsec -= nsec % TICK_NSEC：tick 网格可能跨秒，且定点乘数有微小误差；规范输入保证
 * tv_nsec < NSEC_PER_SEC，使当前移位舍弃低位的近似在预期范围内成立。
 */
unsigned long
timespec64_to_jiffies(const struct timespec64 *value)
{
	u64 sec = value->tv_sec;
	long nsec = value->tv_nsec + TICK_NSEC - 1;

	/* 阶段 1：先饱和秒，且清零纳秒，避免后续定点乘法超出设计范围。 */
	if (sec >= MAX_SEC_IN_JIFFIES){
		sec = MAX_SEC_IN_JIFFIES;
		nsec = 0;
	}
	/* 阶段 2：分别缩放秒/纳秒；中间右移把纳秒项对齐到秒项尺度，末次移位得到 jiffy。 */
	return ((sec * SEC_CONVERSION) +
		(((u64)nsec * NSEC_CONVERSION) >>
		 (NSEC_JIFFIE_SC - SEC_JIFFIE_SC))) >> SEC_JIFFIE_SC;

}
EXPORT_SYMBOL(timespec64_to_jiffies);

/**
 * jiffies_to_timespec64 - convert jiffies value to &struct timespec64
 * @jiffies: jiffies value
 * @value: pointer to &struct timespec64
 */
/*
 * jiffies_to_timespec64() - 把 jiffy 数转换为规范的秒/纳秒输出。
 * @jiffies 是纯输入 tick 数；@value 是必需、由调用者拥有的输出对象，调用后被完整覆盖。
 * timer/ABI 换算调用者使用它，下一步可把结果用于内核时间算术或复制给用户。函数将
 * jiffies*TICK_NSEC 一次除以每秒纳秒数，商写秒、rem 写不足一秒纳秒，返回无直接值。
 * 纯算术、无锁、不可睡眠，无失败、引用、全局副作用或 ownership 转移；乘法可表示范围遵循
 * unsigned long 与既有 jiffy API 的输入约束。
 */
void
jiffies_to_timespec64(const unsigned long jiffies, struct timespec64 *value)
{
	/*
	 * Convert jiffies to nanoseconds and separate with
	 * one divide.
	 */
	/* 一次 div_u64_rem 同时得到秒商和纳秒余数，天然保证 tv_nsec 小于一秒。 */
	u32 rem;
	value->tv_sec = div_u64_rem((u64)jiffies * TICK_NSEC,
				    NSEC_PER_SEC, &rem);
	value->tv_nsec = rem;
}
EXPORT_SYMBOL(jiffies_to_timespec64);

/*
 * Convert jiffies/jiffies_64 to clock_t and back.
 */
/*
 * 以下函数在内核 HZ tick 尺度与用户 ABI 的 USER_HZ/clock_t 尺度之间双向换算；二者可能不同，
 * 因而不能把 clock_t 当作原始 jiffies。各函数均为纯算术，不拥有对象或同步状态。
 */

/**
 * jiffies_to_clock_t - Convert jiffies to clock_t
 * @x: jiffies value
 *
 * Return: jiffies converted to clock_t (CLOCKS_PER_SEC)
 */
/*
 * jiffies_to_clock_t() - 把机器字长 jiffy 计数换算为用户时钟 tick。
 * proc/stat、times 等 ABI 调用者传入 @x（HZ 单位纯值），返回 clock_t（每秒 USER_HZ 个单位）。
 * 函数无锁、不可睡眠、无副作用/ownership。若 TICK_NSEC 能整除一个用户 tick，编译期按 HZ 与
 * USER_HZ 选择精确乘除；否则先转成纳秒尺度再除。整数除法向下截断，极大输入的乘法范围由调用者
 * 计数生命周期承担；返回后由 ABI 消费者输出给用户。
 */
clock_t jiffies_to_clock_t(unsigned long x)
{
	/* 编译期分支消除通用除法；每个已选配置最终只保留一条换算路径。 */
#if (TICK_NSEC % (NSEC_PER_SEC / USER_HZ)) == 0
# if HZ < USER_HZ
	return x * (USER_HZ / HZ);
# else
	return x / (HZ / USER_HZ);
# endif
#else
	return div_u64((u64)x * TICK_NSEC, NSEC_PER_SEC / USER_HZ);
#endif
}
EXPORT_SYMBOL(jiffies_to_clock_t);

/**
 * clock_t_to_jiffies - Convert clock_t to jiffies
 * @x: clock_t value
 *
 * Return: clock_t value converted to jiffies
 */
/*
 * clock_t_to_jiffies() - 把用户 USER_HZ tick 转回内核 HZ jiffy，并显式防止机器字乘法溢出。
 * @x 是 unsigned long 用户 tick 纯值；返回 unsigned long jiffy。函数纯算术、无锁、不可睡眠、
 * 无状态/ownership。HZ 整除 USER_HZ 时先比较 ~0UL/(HZ/USER_HZ)，溢出将饱和为 ~0UL；其他
 * 配置用重排后的上限检查再以 u64 乘除，允许整数精度损失但抑制早期溢出。无错误码，调用者把
 * 饱和值视为超大期限/计数。
 */
unsigned long clock_t_to_jiffies(unsigned long x)
{
#if (HZ % USER_HZ)==0
	if (x >= ~0UL / (HZ / USER_HZ))
		return ~0UL;
	return x * (HZ / USER_HZ);
#else
	/* Don't worry about loss of precision here .. */
	/* 这里的上限比较允许少量整数精度损失，目标是先判断 x*HZ 是否会越过机器字可表示范围。 */
	if (x >= ~0UL / HZ * USER_HZ)
		return ~0UL;

	/* .. but do try to contain it here */
	/* 真正换算提升到 u64 再除 USER_HZ，把不可避免的截断和溢出限制在明确位置。 */
	return div_u64((u64)x * HZ, USER_HZ);
#endif
}
EXPORT_SYMBOL(clock_t_to_jiffies);

/**
 * jiffies_64_to_clock_t - Convert jiffies_64 to clock_t
 * @x: jiffies_64 value
 *
 * Return: jiffies_64 value converted to 64-bit "clock_t" (CLOCKS_PER_SEC)
 */
/*
 * jiffies_64_to_clock_t() - 把 64 位累计 jiffy 换算为 64 位用户 clock_t 尺度。
 * times()/trace clock 等长期累计路径传入 @x（HZ 单位纯值），返回每秒 USER_HZ 个单位的 u64。
 * 函数纯算术、无锁、不可睡眠、无副作用/ownership；notrace 禁止函数跟踪插桩，使 trace clock
 * 等调用它时不会递归进入跟踪基础设施。编译期优先选择精确整比乘除，HZ==USER_HZ 时值不变；
 * 通用路径经 TICK_NSEC 换算，整数截断。极大乘法虽可更晚溢出优化，但 64 位下已有数百年余量。
 */
notrace u64 jiffies_64_to_clock_t(u64 x)
{
#if (TICK_NSEC % (NSEC_PER_SEC / USER_HZ)) == 0
# if HZ < USER_HZ
	x = div_u64(x * USER_HZ, HZ);
# elif HZ > USER_HZ
	x = div_u64(x, HZ / USER_HZ);
# else
	/* Nothing to do */
	/* HZ 与 USER_HZ 相等，输入已经处于目标单位，无需运算。 */
# endif
#else
	/*
	 * There are better ways that don't overflow early,
	 * but even this doesn't overflow in hundreds of years
	 * in 64 bits, so..
	 */
	/* 存在更晚溢出的重排公式，但当前 64 位乘法也要数百年才溢出，保持简单路径即可。 */
	x = div_u64(x * TICK_NSEC, (NSEC_PER_SEC / USER_HZ));
#endif
	return x;
}
EXPORT_SYMBOL(jiffies_64_to_clock_t);

/**
 * nsec_to_clock_t - Convert nsec value to clock_t
 * @x: nsec value
 *
 * Return: nsec value converted to 64-bit "clock_t" (CLOCKS_PER_SEC)
 */
/*
 * nsec_to_clock_t() - 把 64 位 CPU/进程累计纳秒换算为 USER_HZ 用户 tick。
 * /proc、times、signal 统计调用者传入 @x（纳秒纯值），返回 u64 clock_t 尺度。函数纯算术、
 * 无锁、不可睡眠、无状态或 ownership。若每用户 tick 是整数纳秒则直接除；USER_HZ 为 512 倍数
 * 时先约掉 512 以延后乘法溢出；其余用乘 9 的近似分母。通用路径在 USER_HZ<=1024 时最大相对
 * 误差 5.7e-8（约每年 1.8 秒），约 64.99 年溢出，并对列出的常见频率精确。
 */
u64 nsec_to_clock_t(u64 x)
{
	/* 每个编译配置只保留一个分支：从精确且最不易溢出的公式逐级退化到通用近似。 */
#if (NSEC_PER_SEC % USER_HZ) == 0
	return div_u64(x, NSEC_PER_SEC / USER_HZ);
#elif (USER_HZ % 512) == 0
	return div_u64(x * USER_HZ / 512, NSEC_PER_SEC / 512);
#else
	/*
         * max relative error 5.7e-8 (1.8s per year) for USER_HZ <= 1024,
         * overflow after 64.99 years.
         * exact for HZ=60, 72, 90, 120, 144, 180, 300, 600, 900, ...
         */
		/*
		 * USER_HZ<=1024 时最大相对误差 5.7e-8（每年约 1.8 秒），约 64.99 年后乘法溢出；
		 * 对 60、72、90、120、144、180、300、600、900 等频率计算精确。
		 */
	return div_u64(x * 9, (9ull * NSEC_PER_SEC + (USER_HZ / 2)) / USER_HZ);
#endif
}

/**
 * jiffies64_to_nsecs - Convert jiffies64 to nanoseconds
 * @j: jiffies64 value
 *
 * Return: nanoseconds value
 */
/*
 * jiffies64_to_nsecs() - 把 64 位 jiffy 时长换算为纳秒。
 * @j 是 HZ 单位纯值，返回 u64 纳秒；调度/时间统计调用者随后在纳秒时间轴运算。函数纯算术、
 * 无锁、不可睡眠、无副作用/ownership。若每 jiffy 是整数纳秒则直接乘；否则使用 timeconst.bc
 * 生成并约分的 NUM/DEN 比例后整除。两路均向下截断，极大 @j 的乘法溢出由 u64 输入范围约束，
 * 不返回错误或饱和值。
 */
u64 jiffies64_to_nsecs(u64 j)
{
#if !(NSEC_PER_SEC % HZ)
	return (NSEC_PER_SEC / HZ) * j;
# else
	return div_u64(j * HZ_TO_NSEC_NUM, HZ_TO_NSEC_DEN);
#endif
}
EXPORT_SYMBOL(jiffies64_to_nsecs);

/**
 * jiffies64_to_msecs - Convert jiffies64 to milliseconds
 * @j: jiffies64 value
 *
 * Return: milliseconds value
 */
/*
 * jiffies64_to_msecs() - 把 64 位 jiffy 时长换算为毫秒。
 * @j 是 HZ 单位纯值，返回 u64 毫秒；函数纯算术、无锁、不可睡眠、无副作用/ownership。
 * HZ<=1000 且整除时，每 tick 有整数毫秒并直接乘；其他配置使用生成的最简比例后向下截断。
 * 与超时输入换算不同，本统计换算不使用 MAX_JIFFY_OFFSET，也不报告极大乘法溢出。
 */
u64 jiffies64_to_msecs(const u64 j)
{
#if HZ <= MSEC_PER_SEC && !(MSEC_PER_SEC % HZ)
	return (MSEC_PER_SEC / HZ) * j;
#else
	return div_u64(j * HZ_TO_MSEC_NUM, HZ_TO_MSEC_DEN);
#endif
}
EXPORT_SYMBOL(jiffies64_to_msecs);

/**
 * nsecs_to_jiffies64 - Convert nsecs in u64 to jiffies64
 *
 * @n:	nsecs in u64
 *
 * Unlike {m,u}secs_to_jiffies, type of input is not unsigned int but u64.
 * And this doesn't return MAX_JIFFY_OFFSET since this function is designed
 * for scheduler, not for use in device drivers to calculate timeout value.
 *
 * note:
 *   NSEC_PER_SEC = 10^9 = (5^9 * 2^9) = (1953125 * 512)
 *   ULLONG_MAX ns = 18446744073.709551615 secs = about 584 years
 *
 * Return: nsecs converted to jiffies64 value
 */
/*
 * nsecs_to_jiffies64() - 为调度器把 u64 纳秒时长换算为 64 位 jiffy 计数。
 * @n 是纯输入纳秒；与 unsigned int 的毫秒/微秒超时 API 不同，本函数面向调度累计值，故不把
 * 结果饱和为 MAX_JIFFY_OFFSET。NSEC_PER_SEC=10^9=5^9*2^9=1953125*512，u64 最大纳秒约
 * 18446744073.709551615 秒（584 年）；这些因数用于选择延后溢出的编译期公式。
 * 返回 u64 jiffy，纯算术、无锁、不可睡眠、无状态/ownership。整除路径最直接；HZ 为 512 倍数
 * 时先约分（HZ=1024 约 292 年溢出）；通用乘 9 公式偏向 HZ 为 3 倍数，约 64.99 年溢出并对
 * 60、72、90、120 等频率精确。各路径整数除法向下截断，无错误码。
 */
u64 nsecs_to_jiffies64(u64 n)
{
#if (NSEC_PER_SEC % HZ) == 0
	/* Common case, HZ = 100, 128, 200, 250, 256, 500, 512, 1000 etc. */
	/* 常见 HZ（100、128、200、250、256、500、512、1000 等）可直接除以每 tick 纳秒数。 */
	return div_u64(n, NSEC_PER_SEC / HZ);
#elif (HZ % 512) == 0
	/* overflow after 292 years if HZ = 1024 */
	/* 先约掉 512；HZ=1024 时中间乘法约 292 年后才溢出。 */
	return div_u64(n * HZ / 512, NSEC_PER_SEC / 512);
#else
	/*
	 * Generic case - optimized for cases where HZ is a multiple of 3.
	 * overflow after 64.99 years, exact for HZ = 60, 72, 90, 120 etc.
	 */
	/* 通用公式针对 HZ 为 3 倍数优化，约 64.99 年后溢出，对 60、72、90、120 等频率精确。 */
	return div_u64(n * 9, (9ull * NSEC_PER_SEC + HZ / 2) / HZ);
#endif
}
EXPORT_SYMBOL(nsecs_to_jiffies64);

/**
 * nsecs_to_jiffies - Convert nsecs in u64 to jiffies
 *
 * @n:	nsecs in u64
 *
 * Unlike {m,u}secs_to_jiffies, type of input is not unsigned int but u64.
 * And this doesn't return MAX_JIFFY_OFFSET since this function is designed
 * for scheduler, not for use in device drivers to calculate timeout value.
 *
 * note:
 *   NSEC_PER_SEC = 10^9 = (5^9 * 2^9) = (1953125 * 512)
 *   ULLONG_MAX ns = 18446744073.709551615 secs = about 584 years
 *
 * Return: nsecs converted to jiffies value
 */
/*
 * nsecs_to_jiffies() - 将调度器纳秒值换算为本机字长 jiffy。
 * @n 为 u64 纳秒纯值；本函数是 nsecs_to_jiffies64() 的薄包装，先沿同一 HZ 算法得到 u64，
 * 再显式收窄为 unsigned long。返回不采用 MAX_JIFFY_OFFSET 饱和，因为它面向调度内部而非驱动
 * 超时；在 32 位机器上过大结果会按 C 转换截断，调用者必须提供可表示范围。纯算术、无锁、
 * 不睡眠、无错误码、状态或 ownership 副作用，GPL 内核调用者消费返回值。
 */
unsigned long nsecs_to_jiffies(u64 n)
{
	/* 复用 64 位公式保持各 HZ 配置一致；这里只承担最终 ABI/机器字宽收窄。 */
	return (unsigned long)nsecs_to_jiffies64(n);
}
EXPORT_SYMBOL_GPL(nsecs_to_jiffies);

/**
 * timespec64_add_safe - Add two timespec64 values and do a safety check
 * for overflow.
 * @lhs: first (left) timespec64 to add
 * @rhs: second (right) timespec64 to add
 *
 * It's assumed that both values are valid (>= 0).
 * And, each timespec64 is in normalized form.
 *
 * Return: sum of @lhs + @rhs
 */
/*
 * timespec64_add_safe() - 相加两个非负规范 timespec64，并在秒溢出时返回最大时间哨兵。
 * 时间范围计算者按值传入 @lhs/@rhs；二者须满足 >=0 且 0<=tv_nsec<1 秒。res 是返回对象。
 * 先用 timeu64_t 秒加法和 set_normalized_timespec64() 合并纳秒进位，再检查规范化结果是否小于
 * 任一加数；该回绕关系表示溢出，结果饱和为 {TIME64_MAX,0}。返回完整 timespec64，无错误码。
 * 函数纯算术、无锁、不可睡眠，不修改输入、全局状态或 ownership；unlikely 标记溢出是冷路径。
 */
struct timespec64 timespec64_add_safe(const struct timespec64 lhs,
				const struct timespec64 rhs)
{
	struct timespec64 res;

	/* 阶段 1：用无符号秒加法保存模 2^64 结果，并让 helper 处理纳秒跨秒进位。 */
	set_normalized_timespec64(&res, (timeu64_t) lhs.tv_sec + rhs.tv_sec,
			lhs.tv_nsec + rhs.tv_nsec);

	/* 阶段 2：非负加法应不小于任一操作数；违反即发生回绕，改为明确的最大值。 */
	if (unlikely(res.tv_sec < lhs.tv_sec || res.tv_sec < rhs.tv_sec)) {
		res.tv_sec = TIME64_MAX;
		res.tv_nsec = 0;
	}

	return res;
}
EXPORT_SYMBOL_GPL(timespec64_add_safe);

/**
 * get_timespec64 - get user's time value into kernel space
 * @ts: destination &struct timespec64
 * @uts: user's time value as &struct __kernel_timespec
 *
 * Handles compat or 32-bit modes.
 *
 * Return: 0 on success or negative errno on error
 */
/*
 * get_timespec64() - 从现代固定宽度用户 timespec 取得内核 timespec64。
 * 各 time64 syscall 入口调用它。@ts 是必需、由调用者拥有的内核输出；@uts 是必需、借用的用户
 * 输入。kts 是完整用户快照，ret 是 copy_from_user 未复制字节数。函数在可睡眠进程上下文使用，
 * 不要求锁。复制失败返回 -EFAULT 且不写 @ts；成功覆盖秒/纳秒并返回 0，但只负责 ABI 搬运，
 * 不校验 timespec 取值。兼容 syscall 会清掉 tv_nsec 高 32 位 padding；32 位赋值再丢弃 padding。
 * 无引用、全局副作用或 ownership 转移，调用者随后负责语义校验。
 */
int get_timespec64(struct timespec64 *ts,
		   const struct __kernel_timespec __user *uts)
{
	struct __kernel_timespec kts;
	int ret;

	/* 阶段 1：一次性稳定用户结构；失败前保持调用者输出对象不变。 */
	ret = copy_from_user(&kts, uts, sizeof(kts));
	if (ret)
		return -EFAULT;

	/* 阶段 2：秒字段本身固定为 64 位，可直接提交到内核输出。 */
	ts->tv_sec = kts.tv_sec;

	/* Zero out the padding in compat mode */
	/* compat 布局把高 32 位当 padding，先清零，避免它被误当作纳秒数值。 */
	if (in_compat_syscall())
		kts.tv_nsec &= 0xFFFFFFFFUL;

	/* In 32-bit mode, this drops the padding */
	/* 32 位内核赋给较窄的 tv_nsec 字段时自然丢弃 padding；64 位则保留已规范化的低位值。 */
	ts->tv_nsec = kts.tv_nsec;

	return 0;
}
EXPORT_SYMBOL_GPL(get_timespec64);

/**
 * put_timespec64 - convert timespec64 value to __kernel_timespec format and
 * 		    copy the latter to userspace
 * @ts: input &struct timespec64
 * @uts: user's &struct __kernel_timespec
 *
 * Return: 0 on success or negative errno on error
 */
/*
 * put_timespec64() - 构造现代固定宽度用户 timespec 并复制到用户空间。
 * time64 syscall 输出路径调用它。@ts 是必需、借用、只读的内核输入；@uts 是必需、借用的用户
 * 输出。kts 通过指定初始化器完整承载秒/纳秒，未列出的 padding 自动为 0。函数在可睡眠进程
 * 上下文运行且不要求锁。copy_to_user 全成功返回 0，否则返回 -EFAULT，用户内存可能部分写入；
 * 不修改 @ts、全局状态或 ownership。它只处理布局，调用前由生产者保证 timespec 语义有效。
 */
int put_timespec64(const struct timespec64 *ts,
		   struct __kernel_timespec __user *uts)
{
	struct __kernel_timespec kts = {
		.tv_sec = ts->tv_sec,
		.tv_nsec = ts->tv_nsec
	};

	/* 单次复制仍可能部分完成；三目运算把未复制字节数统一映射为 syscall errno。 */
	return copy_to_user(uts, &kts, sizeof(kts)) ? -EFAULT : 0;
}
EXPORT_SYMBOL_GPL(put_timespec64);

/*
 * __get_old_timespec32() - 把严格 old_timespec32 用户布局扩展到 timespec64。
 * get_old_timespec32() 与旧 itimerspec helper 调用它。@ts64 是必需内核输出，@cts 是必需、借用的
 * 用户输入；ts 是稳定旧结构快照，ret 是拷贝结果。函数可睡眠、不持锁。用户复制失败返回
 * -EFAULT 且不写输出；成功按有符号 32 位语义扩展秒/纳秒并返回 0，不做范围规范化。
 * 无全局副作用、引用或 ownership，调用者继续完成 ABI 选择或定时器语义校验。
 */
static int __get_old_timespec32(struct timespec64 *ts64,
				   const struct old_timespec32 __user *cts)
{
	struct old_timespec32 ts;
	int ret;

	/* 先完成全部用户读取，再成组覆盖输出，避免 fault 留下半转换的 timespec64。 */
	ret = copy_from_user(&ts, cts, sizeof(ts));
	if (ret)
		return -EFAULT;

	ts64->tv_sec = ts.tv_sec;
	ts64->tv_nsec = ts.tv_nsec;

	return 0;
}

/*
 * __put_old_timespec32() - 把内核 timespec64 收窄为严格 old_timespec32 并复制给用户。
 * put_old_timespec32() 与旧 itimerspec 输出 helper 调用它。@ts64 是必需、借用、只读的内核输入；
 * @cts 是必需、借用的用户输出；ts 是按旧字段宽度构造的栈对象，因此超出 32 位秒范围会按 C
 * 赋值收窄。函数可睡眠、不持锁。复制成功返回 0，失败返回 -EFAULT 且用户区可能部分写入；
 * 不修改输入、全局状态或 ownership，也不负责 Y2038 范围校验。
 */
static int __put_old_timespec32(const struct timespec64 *ts64,
				   struct old_timespec32 __user *cts)
{
	struct old_timespec32 ts = {
		.tv_sec = ts64->tv_sec,
		.tv_nsec = ts64->tv_nsec
	};
	/* 把一次旧布局复制的未完成字节统一映射为 -EFAULT；此前没有可回滚的内核副作用。 */
	return copy_to_user(cts, &ts, sizeof(ts)) ? -EFAULT : 0;
}

/**
 * get_old_timespec32 - get user's old-format time value into kernel space
 * @ts: destination &struct timespec64
 * @uts: user's old-format time value (&struct old_timespec32)
 *
 * Handles X86_X32_ABI compatibility conversion.
 *
 * Return: 0 on success or negative errno on error
 */
/*
 * get_old_timespec32() - 根据当前 compat ABI 选择 64 位直拷或真正的 32 位 timespec 扩展。
 * 旧时间 syscall 入口调用它。@ts 是必需、由调用者拥有的内核输出；@uts 是无类型的必需、借用
 * 用户输入，其真实布局由 COMPAT_USE_64BIT_TIME 判定。x86 x32 虽走 compat syscall，却使用
 * 64 位时间布局，故直接复制 timespec64；普通 32 位 ABI 调 __get_old_timespec32()。
 * 函数可睡眠、不持锁；成功返回 0 并覆盖输出，用户 fault 返回 -EFAULT 且无全局副作用、引用或
 * ownership。它只选择布局，不校验数值，调用者随后处理定时语义。
 */
int get_old_timespec32(struct timespec64 *ts, const void __user *uts)
{
	/* 运行期 ABI 分派是 x32 与传统 compat 共用 syscall 实现的关键，不能仅按内核位数判断。 */
	if (COMPAT_USE_64BIT_TIME)
		return copy_from_user(ts, uts, sizeof(*ts)) ? -EFAULT : 0;
	else
		return __get_old_timespec32(ts, uts);
}
EXPORT_SYMBOL_GPL(get_old_timespec32);

/**
 * put_old_timespec32 - convert timespec64 value to &struct old_timespec32 and
 * 			copy the latter to userspace
 * @ts: input &struct timespec64
 * @uts: user's &struct old_timespec32
 *
 * Handles X86_X32_ABI compatibility conversion.
 *
 * Return: 0 on success or negative errno on error
 */
/*
 * put_old_timespec32() - 按当前 compat ABI 把 timespec64 输出为 64 位直布局或 old_timespec32。
 * 旧时间 syscall 返回路径调用它。@ts 是必需、借用、只读的内核输入；@uts 是必需、借用且布局
 * 由 COMPAT_USE_64BIT_TIME 决定的用户输出。x86 x32 直接复制完整 64 位结构，传统 32 位 ABI
 * 由 __put_old_timespec32() 收窄。函数可睡眠、不持锁；成功返回 0，fault 返回 -EFAULT 且用户
 * 区可能部分写入。无全局状态/ownership，旧布局超范围由 ABI 收窄语义承担。
 */
int put_old_timespec32(const struct timespec64 *ts, void __user *uts)
{
	/* 与取入函数使用同一运行期条件，保证同一次 syscall 的输入输出布局对称。 */
	if (COMPAT_USE_64BIT_TIME)
		return copy_to_user(uts, ts, sizeof(*ts)) ? -EFAULT : 0;
	else
		return __put_old_timespec32(ts, uts);
}
EXPORT_SYMBOL_GPL(put_old_timespec32);

/**
 * get_itimerspec64 - get user's &struct __kernel_itimerspec into kernel space
 * @it: destination &struct itimerspec64
 * @uit: user's &struct __kernel_itimerspec
 *
 * Return: 0 on success or negative errno on error
 */
/*
 * get_itimerspec64() - 依次从用户 __kernel_itimerspec 取入周期和首次到期时间。
 * POSIX timer/time64 syscall 调用它。@it 是必需、由调用者拥有的内核输出，包含 it_interval 周期
 * 和 it_value 当前/首次到期；@uit 是必需、借用的用户输入。ret 传递 get_timespec64() 结果。
 * 函数在可睡眠进程上下文运行、不持锁。第一项失败立即返回 errno 且不继续；第二项失败时周期
 * 已写入 @it、到期字段不保证更新，因此调用者必须把非零返回视为整个对象无效。成功返回 0；
 * 只做 ABI 搬运，不校验字段、修改定时器或转移 ownership，后续由具体 timer 入口处理。
 */
int get_itimerspec64(struct itimerspec64 *it,
			const struct __kernel_itimerspec __user *uit)
{
	int ret;

	/* 阶段 1：先取周期；失败时不再触碰用户对象的第二个成员。 */
	ret = get_timespec64(&it->it_interval, &uit->it_interval);
	if (ret)
		return ret;

	/* 阶段 2：再取到期值；两个成员复制不是事务，错误由调用者通过 ret 统一判废。 */
	ret = get_timespec64(&it->it_value, &uit->it_value);

	return ret;
}
EXPORT_SYMBOL_GPL(get_itimerspec64);

/**
 * put_itimerspec64 - convert &struct itimerspec64 to __kernel_itimerspec format
 * 		      and copy the latter to userspace
 * @it: input &struct itimerspec64
 * @uit: user's &struct __kernel_itimerspec
 *
 * Return: 0 on success or negative errno on error
 */
/*
 * put_itimerspec64() - 依次把内核定时器周期和到期值复制为用户 __kernel_itimerspec。
 * POSIX timer/time64 syscall 输出路径调用它。@it 是必需、借用、只读的内核输入；@uit 是必需、
 * 借用的用户输出；ret 传递 put_timespec64() 结果。函数可睡眠、不持锁。周期输出失败立即返回；
 * 到期输出失败时用户区已收到周期，因此输出不是事务性的。成功返回 0，失败返回 -EFAULT；
 * 内核定时器状态不会因用户复制失败回滚或改变，也无引用/ownership 转移，随后 syscall 返回用户态。
 */
int put_itimerspec64(const struct itimerspec64 *it,
			struct __kernel_itimerspec __user *uit)
{
	int ret;

	/* 阶段 1：先发布周期字段；失败则避免继续写第二个用户区成员。 */
	ret = put_timespec64(&it->it_interval, &uit->it_interval);
	if (ret)
		return ret;

	/* 阶段 2：再发布到期值；若 fault，调用者只得到 -EFAULT，已写周期不能撤销。 */
	ret = put_timespec64(&it->it_value, &uit->it_value);

	return ret;
}
EXPORT_SYMBOL_GPL(put_itimerspec64);

/**
 * get_old_itimerspec32 - get user's &struct old_itimerspec32 into kernel space
 * @its: destination &struct itimerspec64
 * @uits: user's &struct old_itimerspec32
 *
 * Return: 0 on success or negative errno on error
 */
/*
 * get_old_itimerspec32() - 从严格 32 位旧 itimerspec 取入周期与到期值。
 * time32 POSIX timer 入口调用它。@its 是必需、由调用者拥有的内核输出；@uits 是必需、借用的
 * old_itimerspec32 用户输入。两个成员都由 __get_old_timespec32() 扩展，逻辑或采用短路语义：
 * 周期失败时不读取到期值；到期失败时周期已经写入。任一 fault 返回 -EFAULT，调用者必须判废
 * 整个输出；成功返回 0。函数可睡眠、不持锁，只搬运 ABI，不校验、修改 timer 或转移 ownership。
 */
int get_old_itimerspec32(struct itimerspec64 *its,
			const struct old_itimerspec32 __user *uits)
{

	/* 短路组合保留第一个错误即停止；两个用户结构读取不具备原子快照保证。 */
	if (__get_old_timespec32(&its->it_interval, &uits->it_interval) ||
	    __get_old_timespec32(&its->it_value, &uits->it_value))
		return -EFAULT;
	return 0;
}
EXPORT_SYMBOL_GPL(get_old_itimerspec32);

/**
 * put_old_itimerspec32 - convert &struct itimerspec64 to &struct
 *			  old_itimerspec32 and copy the latter to userspace
 * @its: input &struct itimerspec64
 * @uits: user's &struct old_itimerspec32
 *
 * Return: 0 on success or negative errno on error
 */
/*
 * put_old_itimerspec32() - 把内核 itimerspec64 的两个成员收窄并输出为旧 32 位布局。
 * time32 POSIX timer 返回路径调用它。@its 是必需、借用、只读的内核输入；@uits 是必需、借用的
 * 用户输出。逻辑或使周期复制失败时跳过到期复制；第二次失败时周期已在用户区，不能撤销。
 * 任一 fault 返回 -EFAULT，全部成功返回 0。函数可睡眠、不持锁；它不改变内核 timer、引用或
 * ownership，超出 32 位秒范围按旧 ABI 收窄，调用者随后直接结束 syscall。
 */
int put_old_itimerspec32(const struct itimerspec64 *its,
			struct old_itimerspec32 __user *uits)
{
	/* 两次复制按字段顺序短路执行；返回成功只表示用户拷贝完成，不代表 timer 状态发生变化。 */
	if (__put_old_timespec32(&its->it_interval, &uits->it_interval) ||
	    __put_old_timespec32(&its->it_value, &uits->it_value))
		return -EFAULT;
	return 0;
}
EXPORT_SYMBOL_GPL(put_old_itimerspec32);
