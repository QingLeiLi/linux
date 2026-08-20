// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992 Darren Senn
 */

/* These are all the functions necessary to implement itimers */
/*
 * 本文件实现传统 `getitimer/setitimer/alarm`：ITIMER_REAL 用 signal_struct 内的 hrtimer 按墙钟流逝发送
 * SIGALRM，周期 timer 延迟到信号真正 dequeue 时重装；ITIMER_VIRTUAL/PROF 用线程组 CPU 时间绝对阈值
 * 发送信号。native/compat syscall 在 timespec64 内核表示与旧 timeval ABI 间转换，所有进程级状态由
 * `sighand->siglock` 串行，用户复制失败不会自动回滚已经提交的 timer 更改。
 */

#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/syscalls.h>
#include <linux/time.h>
#include <linux/sched/signal.h>
#include <linux/sched/cputime.h>
#include <linux/posix-timers.h>
#include <linux/hrtimer.h>
#include <trace/events/timer.h>
#include <linux/compat.h>

#include <linux/uaccess.h>

/**
 * itimer_get_remtime - get remaining time for the timer
 *
 * @timer: the timer to read
 *
 * Returns the delta between the expiry time and now, which can be
 * less than zero or 1usec for an pending expired timer
 */
/*
 * itimer_get_remtime() - 读取 ITIMER_REAL hrtimer 的剩余时间并转换为旧 ABI 可区分的非负值。
 * @timer 是调用者在 siglock 下稳定的借用指针。rem 先读取“到期-now”；若 timer 仍 active 而 rem<=0，
 * 返回 1us，表示已到期但 callback/信号尚未完成，避免被误读为已禁用；若已不 active 则返回 0。
 * active 检查与 remaining 读取有竞态，但到期后返回 0 仍合法。返回规范 timespec64，不修改/持有 timer。
 */
static struct timespec64 itimer_get_remtime(struct hrtimer *timer)
{
	ktime_t rem = __hrtimer_get_remaining(timer, true);

	/*
	 * Racy but safe: if the itimer expires after the above
	 * hrtimer_get_remtime() call but before this condition
	 * then we return 0 - which is correct.
	 */
	/* 两次读取间若 timer 到期，返回 0 表示已过期；仍 active 的非正剩余则抬到 1us 保留“pending”信息。 */
	if (hrtimer_active(timer)) {
		if (rem <= 0)
			rem = NSEC_PER_USEC;
	} else
		rem = 0;

	return ktime_to_timespec64(rem);
}

/*
 * get_cpu_itimer() - 查询当前线程组一个 VIRTUAL/PROF CPU itimer 的剩余 CPU 时间和周期。
 * @tsk 是所属任务借用指针；@clock_id 必须为 CPUCLOCK_VIRT/PROF；@value 是非空输出。siglock irq 区内
 * 读取绝对 expires/incr，并采样线程组各 CPU clock；已武装时用 expires-sample 得余量，若阈值已落后则
 * 返回 TICK_NSEC 表示即将触发。精确相等走减法得到 0，是当前边界。解锁后转 timespec64，无错误码。
 */
static void get_cpu_itimer(struct task_struct *tsk, unsigned int clock_id,
			   struct itimerspec64 *const value)
{
	u64 val, interval;
	struct cpu_itimer *it = &tsk->signal->it[clock_id];

	spin_lock_irq(&tsk->sighand->siglock);

	val = it->expires;
	interval = it->incr;
	if (val) {
		u64 t, samples[CPUCLOCK_MAX];

		thread_group_sample_cputime(tsk, samples);
		t = samples[clock_id];

		if (val < t)
			/* about to fire */
			/* CPU 用量已越过绝对阈值但信号尚待处理，以一个 tick 的非零余量表示“即将触发”。 */
			val = TICK_NSEC;
		else
			val -= t;
	}

	spin_unlock_irq(&tsk->sighand->siglock);

	value->it_value = ns_to_timespec64(val);
	value->it_interval = ns_to_timespec64(interval);
}

/*
 * do_getitimer() - 按传统 which 查询 current 线程组的墙钟或 CPU interval timer。
 * @which 接受 ITIMER_REAL/VIRTUAL/PROF；@value 是非空内核输出。REAL 在 siglock 下读取 hrtimer 和 incr；
 * CPU 两类委托 get_cpu_itimer。成功返回 0，未知 which 返回 -EINVAL；不接触用户内存或转移 task 引用。
 */
static int do_getitimer(int which, struct itimerspec64 *value)
{
	struct task_struct *tsk = current;

	switch (which) {
	case ITIMER_REAL:
		spin_lock_irq(&tsk->sighand->siglock);
		value->it_value = itimer_get_remtime(&tsk->signal->real_timer);
		value->it_interval =
			ktime_to_timespec64(tsk->signal->it_real_incr);
		spin_unlock_irq(&tsk->sighand->siglock);
		break;
	case ITIMER_VIRTUAL:
		get_cpu_itimer(tsk, CPUCLOCK_VIRT, value);
		break;
	case ITIMER_PROF:
		get_cpu_itimer(tsk, CPUCLOCK_PROF, value);
		break;
	default:
		return(-EINVAL);
	}
	return 0;
}

/*
 * put_itimerval() - 把内核 timespec64 interval/value 转成 native 旧 timeval ABI 并复制到用户。
 * @o 是非空用户输出；@i 是只读内核值。纳秒除 1000 向下截断到微秒，不做秒范围再验证；栈上 v 完整
 * 赋值后一次 copy_to_user。成功返回 0，任意复制失败返回 -EFAULT，timer 状态不受影响。
 */
static int put_itimerval(struct __kernel_old_itimerval __user *o,
			 const struct itimerspec64 *i)
{
	struct __kernel_old_itimerval v;

	v.it_interval.tv_sec = i->it_interval.tv_sec;
	v.it_interval.tv_usec = i->it_interval.tv_nsec / NSEC_PER_USEC;
	v.it_value.tv_sec = i->it_value.tv_sec;
	v.it_value.tv_usec = i->it_value.tv_nsec / NSEC_PER_USEC;
	return copy_to_user(o, &v, sizeof(struct __kernel_old_itimerval)) ? -EFAULT : 0;
}


/*
 * getitimer() - native 旧 ABI syscall 包装。
 * @which 选择 timer，@value 是用户输出。先在内核缓冲查询；仅成功才转换/复制。未知类型返回 -EINVAL，
 * 用户地址失败返回 -EFAULT，成功 0；查询快照即使随后 timer 变化也不重试。
 */
SYSCALL_DEFINE2(getitimer, int, which, struct __kernel_old_itimerval __user *, value)
{
	struct itimerspec64 get_buffer;
	int error = do_getitimer(which, &get_buffer);

	if (!error && put_itimerval(value, &get_buffer))
		error = -EFAULT;
	return error;
}

#if defined(CONFIG_COMPAT) || defined(CONFIG_ALPHA)
/* 32 位/Alpha 旧 timeval 成对组成 compat itimerval；仅用于 syscall 栈上转换，不保存内核 timer 状态。 */
struct old_itimerval32 {
	struct old_timeval32	it_interval;
	struct old_timeval32	it_value;
};

/*
 * put_old_itimerval32() - 把 timespec64 查询结果转成 32 位旧 timeval 并复制给 compat 用户。
 * @o 是用户输出；@i 是内核借用值。纳秒截断为微秒，秒赋给 old_timeval32 可能按 ABI 宽度截断；
 * copy 成功返回 0，失败 -EFAULT，不修改 timer。
 */
static int put_old_itimerval32(struct old_itimerval32 __user *o,
			       const struct itimerspec64 *i)
{
	struct old_itimerval32 v32;

	v32.it_interval.tv_sec = i->it_interval.tv_sec;
	v32.it_interval.tv_usec = i->it_interval.tv_nsec / NSEC_PER_USEC;
	v32.it_value.tv_sec = i->it_value.tv_sec;
	v32.it_value.tv_usec = i->it_value.tv_nsec / NSEC_PER_USEC;
	return copy_to_user(o, &v32, sizeof(struct old_itimerval32)) ? -EFAULT : 0;
}

/*
 * compat_getitimer() - 32 位旧 ABI 查询包装。
 * @which/@value 语义同 native；内核统一用 itimerspec64 查询，再转 old_itimerval32。返回 0、-EINVAL 或
 * -EFAULT；状态快照不因用户复制失败而重取。
 */
COMPAT_SYSCALL_DEFINE2(getitimer, int, which,
		       struct old_itimerval32 __user *, value)
{
	struct itimerspec64 get_buffer;
	int error = do_getitimer(which, &get_buffer);

	if (!error && put_old_itimerval32(value, &get_buffer))
		error = -EFAULT;
	return error;
}
#endif

/*
 * Invoked from dequeue_signal() when SIG_ALRM is delivered.
 *
 * Restart the ITIMER_REAL timer if it is armed as periodic timer.  Doing
 * this in the signal delivery path instead of self rearming prevents a DoS
 * with small increments in the high reolution timer case and reduces timer
 * noise in general.
 */
/*
 * posixtimer_rearm_itimer() - SIGALRM 从 shared pending 真正 dequeue 时重装周期 ITIMER_REAL。
 * @tsk 是正在取信号的线程组成员，调用者持其 sighand->siglock；tmr 是 signal_struct 内嵌借用 hrtimer。
 * 仅 timer 未排队且 it_real_incr 非零时，从旧 expiry 向前跨过已错过周期到 now 之后并 restart。把重装延迟
 * 到信号递送会合并标准信号，防极小高分辨率周期在信号未处理时持续自重装造成 DoS/噪声。无返回值。
 */
void posixtimer_rearm_itimer(struct task_struct *tsk)
{
	struct hrtimer *tmr = &tsk->signal->real_timer;

	if (!hrtimer_is_queued(tmr) && tsk->signal->it_real_incr != 0) {
		hrtimer_forward_now(tmr, tsk->signal->it_real_incr);
		hrtimer_restart(tmr);
	}
}

/*
 * Interval timers are restarted in the signal delivery path.  See
 * posixtimer_rearm_itimer().
 */
/* 周期 ITIMER_REAL 不在到期 callback 自重装，而在 SIGALRM 实际递送路径按上面的协议恢复。 */
/*
 * it_real_fn() - ITIMER_REAL hrtimer 到期时向所属线程组发送一次 SIGALRM。
 * @timer 嵌入 signal_struct；sig/leader_pid 均由线程组对象生命周期稳定。回调在 hrtimer 原子上下文，先
 * trace，再以 SEND_SIG_PRIV 向 TGID pid 发信号；固定返回 HRTIMER_NORESTART，周期性由 dequeue 路径重装。
 * kill 结果不返回给 timer core，标准信号合并/无接收者均由 signal 子系统处理。
 */
enum hrtimer_restart it_real_fn(struct hrtimer *timer)
{
	struct signal_struct *sig =
		container_of(timer, struct signal_struct, real_timer);
	struct pid *leader_pid = sig->pids[PIDTYPE_TGID];

	trace_itimer_expire(ITIMER_REAL, leader_pid, 0);
	kill_pid_info(SIGALRM, SEND_SIG_PRIV, leader_pid);

	return HRTIMER_NORESTART;
}

/*
 * set_cpu_itimer() - 设置/关闭线程组 VIRTUAL 或 PROF CPU timer，并可返回旧剩余值。
 * @tsk 是目标线程组成员；@clock_id 必须为 VIRT/PROF；@value 是已验证相对 value/interval；@ovalue 可空。
 * nval/ninterval 是新 ns，oval/ointerval 是旧值；it 是 signal_struct 静态槽。siglock irq 区内，若旧/新任一
 * 已武装，正 nval 先加 TICK_NSEC 防采样/触发粒度导致过早到期，再由 set_process_cpu_timer 把新相对值
 * 转绝对 CPU 阈值、旧绝对值转剩余量并更新最早事件 cache。随后提交 expires/incr 和 trace。
 * 解锁后若请求 old 则转 timespec64。无返回值；0 value 关闭 timer，即使 interval 字段非零也不会自行武装。
 */
static void set_cpu_itimer(struct task_struct *tsk, unsigned int clock_id,
			   const struct itimerspec64 *const value,
			   struct itimerspec64 *const ovalue)
{
	u64 oval, nval, ointerval, ninterval;
	struct cpu_itimer *it = &tsk->signal->it[clock_id];

	nval = timespec64_to_ns(&value->it_value);
	ninterval = timespec64_to_ns(&value->it_interval);

	spin_lock_irq(&tsk->sighand->siglock);

	oval = it->expires;
	ointerval = it->incr;
	if (oval || nval) {
		if (nval > 0)
			nval += TICK_NSEC;
		set_process_cpu_timer(tsk, clock_id, &nval, &oval);
	}
	it->expires = nval;
	it->incr = ninterval;
	trace_itimer_state(clock_id == CPUCLOCK_VIRT ?
			   ITIMER_VIRTUAL : ITIMER_PROF, value, nval);

	spin_unlock_irq(&tsk->sighand->siglock);

	if (ovalue) {
		ovalue->it_value = ns_to_timespec64(oval);
		ovalue->it_interval = ns_to_timespec64(ointerval);
	}
}

/*
 * Returns true if the timeval is in canonical form
 */
/* timeval 合法要求秒非负且 usec 经 unsigned 检查小于 1 秒；负 usec 转大 unsigned 后同样被拒绝。 */
#define timeval_valid(t) \
	(((t)->tv_sec >= 0) && (((unsigned long) (t)->tv_usec) < USEC_PER_SEC))

/*
 * do_setitimer() - 在内核 timespec64 表示上原子替换 current 线程组的一个传统 itimer。
 * @which 接受 REAL/VIRTUAL/PROF；@value 非空且已规范；@ovalue 可空，非空时返回提交前旧状态。
 *
 * REAL 路径持 siglock 读取 old，并 try_cancel 旧 hrtimer；若 callback 正运行，为避免与信号路径锁互等，
 * 先解 siglock、等待 callback 完成再从头重取 old/重试。新 value 非零时保存 interval 并以 REL 启动；
 * value=0 同时清 interval。CPU 两类委托 set_cpu_itimer。成功 0，未知 which -EINVAL；不访问用户内存。
 */
static int do_setitimer(int which, struct itimerspec64 *value,
			struct itimerspec64 *ovalue)
{
	struct task_struct *tsk = current;
	struct hrtimer *timer;
	ktime_t expires;

	switch (which) {
	case ITIMER_REAL:
again:
		spin_lock_irq(&tsk->sighand->siglock);
		timer = &tsk->signal->real_timer;
		if (ovalue) {
			ovalue->it_value = itimer_get_remtime(timer);
			ovalue->it_interval
				= ktime_to_timespec64(tsk->signal->it_real_incr);
		}
		/* We are sharing ->siglock with it_real_fn() */
		/* callback 发信号会进入共享 siglock 协议；try_cancel 报运行中时必须解锁等待，随后完整重试。 */
		if (hrtimer_try_to_cancel(timer) < 0) {
			spin_unlock_irq(&tsk->sighand->siglock);
			hrtimer_cancel_wait_running(timer);
			goto again;
		}
		expires = timespec64_to_ktime(value->it_value);
		if (expires != 0) {
			tsk->signal->it_real_incr =
				timespec64_to_ktime(value->it_interval);
			hrtimer_start(timer, expires, HRTIMER_MODE_REL);
		} else
			tsk->signal->it_real_incr = 0;

		trace_itimer_state(ITIMER_REAL, value, 0);
		spin_unlock_irq(&tsk->sighand->siglock);
		break;
	case ITIMER_VIRTUAL:
		set_cpu_itimer(tsk, CPUCLOCK_VIRT, value, ovalue);
		break;
	case ITIMER_PROF:
		set_cpu_itimer(tsk, CPUCLOCK_PROF, value, ovalue);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

#ifdef CONFIG_SECURITY_SELINUX
/*
 * clear_itimer() - SELinux 转换需要时关闭 current 的三类传统 interval timer。
 * 无入参/返回值；零值 v 依次传 which=0..2（REAL/VIRTUAL/PROF），不请求旧值。do_setitimer 对这些固定 id
 * 不失败；每类内部自行取得 siglock，函数可等待正在运行的 REAL callback，只能在可睡眠进程上下文调用。
 */
void clear_itimer(void)
{
	struct itimerspec64 v = {};
	int i;

	for (i = 0; i < 3; i++)
		do_setitimer(i, &v, NULL);
}
#endif

#ifdef __ARCH_WANT_SYS_ALARM

/**
 * alarm_setitimer - set alarm in seconds
 *
 * @seconds:	number of seconds until alarm
 *		0 disables the alarm
 *
 * Returns the remaining time in seconds of a pending timer or 0 when
 * the timer is not active.
 *
 * On 32 bit machines the seconds value is limited to (INT_MAX/2) to avoid
 * negative timeval settings which would cause immediate expiry.
 */
/*
 * alarm_setitimer() - 用一次性 ITIMER_REAL 实现秒级 alarm，并返回被替换 alarm 的近似剩余秒。
 * @seconds=0 关闭；32 位 long 构建把大于 INT_MAX 的请求钳到 INT_MAX（原 kernel-doc 写 INT_MAX/2，与当前
 * 代码不一致）。构造零 interval 的 timespec64 后调用 do_setitimer，并取得 old；该内部调用对固定合法输入
 * 不失败。旧值不足 1 秒但非零时强制返回 1，其他值在 nsec>=0.5s 时向上取整，否则截断。返回 unsigned，
 * 若旧 timer 是其他 setitimer 路径设置的超大秒值，窄化仍按 C 转换；函数可等待旧 callback 完成。
 */
static unsigned int alarm_setitimer(unsigned int seconds)
{
	struct itimerspec64 it_new, it_old;

#if BITS_PER_LONG < 64
	if (seconds > INT_MAX)
		seconds = INT_MAX;
#endif
	it_new.it_value.tv_sec = seconds;
	it_new.it_value.tv_nsec = 0;
	it_new.it_interval.tv_sec = it_new.it_interval.tv_nsec = 0;

	do_setitimer(ITIMER_REAL, &it_new, &it_old);

	/*
	 * We can't return 0 if we have an alarm pending ...  And we'd
	 * better return too much than too little anyway
	 */
	/* pending 的亚秒 alarm 不能与“无 alarm”的 0 混淆；较大余量则以半秒为界做近似取整。 */
	if ((!it_old.it_value.tv_sec && it_old.it_value.tv_nsec) ||
	      it_old.it_value.tv_nsec >= (NSEC_PER_SEC / 2))
		it_old.it_value.tv_sec++;

	return it_old.it_value.tv_sec;
}

/*
 * For backwards compatibility?  This can be done in libc so Alpha
 * and all newer ports shouldn't need it.
 */
/* alarm syscall 是历史兼容薄包装；现代用户态也可直接用 setitimer 实现相同能力。 */
/*
 * alarm() - 把用户秒数交给 alarm_setitimer 并返回旧 alarm 剩余秒。
 * @seconds 按值输入；返回语义和 32 位钳位完全继承 helper，无额外用户复制或错误码。
 */
SYSCALL_DEFINE1(alarm, unsigned int, seconds)
{
	return alarm_setitimer(seconds);
}

#endif

/*
 * get_itimerval() - 从 native 旧 itimerval 用户结构复制、验证并转换成内核 timespec64。
 * @o 是非空内核输出；@i 是用户输入。先整体 copy，失败 -EFAULT；value/interval 任一秒为负或 usec 不在
 * [0,1s) 返回 -EINVAL；成功把微秒乘 1000 成纳秒并返回 0。失败不部分写 @o，也不改 timer。
 */
static int get_itimerval(struct itimerspec64 *o, const struct __kernel_old_itimerval __user *i)
{
	struct __kernel_old_itimerval v;

	if (copy_from_user(&v, i, sizeof(struct __kernel_old_itimerval)))
		return -EFAULT;

	/* Validate the timevals in value. */
	/* value 和 interval 都必须是规范非负 timeval，禁用 timer 用全 0 而不是负数。 */
	if (!timeval_valid(&v.it_value) ||
	    !timeval_valid(&v.it_interval))
		return -EINVAL;

	o->it_interval.tv_sec = v.it_interval.tv_sec;
	o->it_interval.tv_nsec = v.it_interval.tv_usec * NSEC_PER_USEC;
	o->it_value.tv_sec = v.it_value.tv_sec;
	o->it_value.tv_nsec = v.it_value.tv_usec * NSEC_PER_USEC;
	return 0;
}

/*
 * setitimer() - native 旧 ABI 设置/查询 current 线程组三类 itimer。
 * @which 选择类型；@value 可空用户输入；@ovalue 可空用户输出。非空 value 先 copy/验证；NULL 是遗留
 * misfeature，按全 0 处理即关闭 timer，并只 printk_once 告警。随后 do_setitimer 原子替换；若请求 old，
 * 成功提交后再转回用户。返回 -EFAULT/-EINVAL 或 0；old copy 失败时新 timer 已生效且不回滚。
 */
SYSCALL_DEFINE3(setitimer, int, which, struct __kernel_old_itimerval __user *, value,
		struct __kernel_old_itimerval __user *, ovalue)
{
	struct itimerspec64 set_buffer, get_buffer;
	int error;

	if (value) {
		error = get_itimerval(&set_buffer, value);
		if (error)
			return error;
	} else {
		memset(&set_buffer, 0, sizeof(set_buffer));
		printk_once(KERN_WARNING "%s calls setitimer() with new_value NULL pointer."
			    " Misfeature support will be removed\n",
			    current->comm);
	}

	error = do_setitimer(which, &set_buffer, ovalue ? &get_buffer : NULL);
	if (error || !ovalue)
		return error;

	if (put_itimerval(ovalue, &get_buffer))
		return -EFAULT;
	return 0;
}

#if defined(CONFIG_COMPAT) || defined(CONFIG_ALPHA)
/*
 * get_old_itimerval32() - 从 32 位旧 itimerval 用户结构复制、验证并转 timespec64。
 * @o 是内核输出；@i 是 compat 用户输入。copy 失败 -EFAULT，任一 timeval 非规范 -EINVAL；成功把 32 位
 * 秒提升到 time64、微秒乘 1000 并返回 0。验证完成前不写 @o，timer 状态不变。
 */
static int get_old_itimerval32(struct itimerspec64 *o, const struct old_itimerval32 __user *i)
{
	struct old_itimerval32 v32;

	if (copy_from_user(&v32, i, sizeof(struct old_itimerval32)))
		return -EFAULT;

	/* Validate the timevals in value.  */
	/* compat value/interval 与 native 使用同一非负秒和 0<=usec<1s 规则。 */
	if (!timeval_valid(&v32.it_value) ||
	    !timeval_valid(&v32.it_interval))
		return -EINVAL;

	o->it_interval.tv_sec = v32.it_interval.tv_sec;
	o->it_interval.tv_nsec = v32.it_interval.tv_usec * NSEC_PER_USEC;
	o->it_value.tv_sec = v32.it_value.tv_sec;
	o->it_value.tv_nsec = v32.it_value.tv_usec * NSEC_PER_USEC;
	return 0;
}

/*
 * compat_setitimer() - 32 位旧 ABI 的 setitimer 包装。
 * @which、可空 @value/@ovalue 语义同 native；使用 old_itimerval32 转换。NULL value 同样按关闭处理并
 * printk_once；do_setitimer 成功后 old copy 若失败返回 -EFAULT 但不回滚新状态。其他返回 0/-EINVAL/-EFAULT。
 */
COMPAT_SYSCALL_DEFINE3(setitimer, int, which,
		       struct old_itimerval32 __user *, value,
		       struct old_itimerval32 __user *, ovalue)
{
	struct itimerspec64 set_buffer, get_buffer;
	int error;

	if (value) {
		error = get_old_itimerval32(&set_buffer, value);
		if (error)
			return error;
	} else {
		memset(&set_buffer, 0, sizeof(set_buffer));
		printk_once(KERN_WARNING "%s calls setitimer() with new_value NULL pointer."
			    " Misfeature support will be removed\n",
			    current->comm);
	}

	error = do_setitimer(which, &set_buffer, ovalue ? &get_buffer : NULL);
	if (error || !ovalue)
		return error;
	if (put_old_itimerval32(ovalue, &get_buffer))
		return -EFAULT;
	return 0;
}
#endif
