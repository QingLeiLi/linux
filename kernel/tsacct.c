// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tsacct.c - System accounting over taskstats interface
 *
 * Copyright (C) Jay Lan,	<jlan@sgi.com>
 */

#include <linux/kernel.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/sched/cputime.h>
#include <linux/tsacct_kern.h>
#include <linux/acct.h>
#include <linux/jiffies.h>
#include <linux/mm.h>

/*
 * 本文件把 task_struct/mm 中的内部计数转换为 taskstats UAPI 字段。基础记账
 * bacct 始终可用；CONFIG_TASK_XACCT 增加内存时间积分及 I/O。读取活任务时是
 * 尽力快照，退出路径在资源销毁前调用可取得稳定终值；单位换算属于 ABI。
 */

/*
 * fill in basic accounting fields
 */
/*
 * bacct_add_tsk() - 把任务身份、时间和基础记账字段写入 taskstats 快照。
 *
 * @user_ns: UID/GID 的目标用户命名空间，借用且不可为 NULL。
 * @pid_ns: PID/TGID/PPID 的目标 PID 命名空间，借用且不可为 NULL。
 * @stats: 调用者拥有的输出缓冲区；
 *         函数只覆盖基础字段，不清零整个结构。
 * @tsk: 被采集任务，调用者用 task 引用或退出上下文保证其存活。
 *
 * 调用位置：kernel/taskstats.c::fill_stats() 在 delayacct 之后调用，返回后
 * 继续采集扩展记账和 exe 身份。
 * 入口不要求持本文件锁，函数不主动睡眠；
 * RCU 只保护读取 cred 的窗口。返回无直接值，所有引用均在函数内平衡。
 */
void bacct_add_tsk(struct user_namespace *user_ns,
		   struct pid_namespace *pid_ns,
		   struct taskstats *stats, struct task_struct *tsk)
{
	const struct cred *tcred;
	u64 utime, stime, utimescaled, stimescaled;
	u64 now_ns, delta;
	time64_t btime;

	BUILD_BUG_ON(TS_COMM_LEN < TASK_COMM_LEN);

	/* calculate task elapsed time in nsec */
	/* 先在单调时钟域计算持续时间，避免墙钟校准改变运行时长。 */
	now_ns = ktime_get_ns();
	/* store whole group time first */
	/* 组墙钟从 leader 创建时刻起算，单 task 墙钟从自身 start_time 起算。 */
	delta = now_ns - tsk->group_leader->start_time;
	/* Convert to micro seconds */
	/* taskstats 时间 ABI 使用微秒，两个差值在写出前统一换算。 */
	do_div(delta, NSEC_PER_USEC);
	stats->ac_tgetime = delta;
	delta = now_ns - tsk->start_time;
	do_div(delta, NSEC_PER_USEC);
	stats->ac_etime = delta;
	/* Convert to seconds for btime (note y2106 limit) */
	/*
	 * 用当前真实秒数减去任务持续秒数近似恢复启动墙钟时间。
	 * 旧 32 位字段
	 * 饱和到 U32_MAX 以应对 2106 边界，ac_btime64 保留完整 time64_t。
	 */
	btime = ktime_get_real_seconds() - div_u64(delta, USEC_PER_SEC);
	stats->ac_btime = clamp_t(time64_t, btime, 0, U32_MAX);
	stats->ac_btime64 = btime;

	/*
	 * 只有已经进入退出协议的任务才有最终 exit_code。随后从 task flags
	 * 派生 acct 标志，并按请求者命名空间映射各类身份标识。
	 */
	if (tsk->flags & PF_EXITING)
		stats->ac_exitcode = tsk->exit_code;
	if (thread_group_leader(tsk) && (tsk->flags & PF_FORKNOEXEC))
		stats->ac_flag |= AFORK;
	if (tsk->flags & PF_SUPERPRIV)
		stats->ac_flag |= ASU;
	if (tsk->flags & PF_DUMPCORE)
		stats->ac_flag |= ACORE;
	if (tsk->flags & PF_SIGNALED)
		stats->ac_flag |= AXSIG;
	stats->ac_nice	 = task_nice(tsk);
	stats->ac_sched	 = tsk->policy;
	stats->ac_pid	 = task_pid_nr_ns(tsk, pid_ns);
	stats->ac_tgid   = task_tgid_nr_ns(tsk, pid_ns);
	stats->ac_ppid	 = task_ppid_nr_ns(tsk, pid_ns);
	/* cred 指针只在 RCU 读侧窗口借用，转换后写入的是普通数值。 */
	rcu_read_lock();
	tcred = __task_cred(tsk);
	stats->ac_uid	 = from_kuid_munged(user_ns, tcred->uid);
	stats->ac_gid	 = from_kgid_munged(user_ns, tcred->gid);
	rcu_read_unlock();

	/*
	 * CPU 时间以纳秒维护，在 UAPI 中转换为微秒；
	 * scaled 版本保留容量校正。
	 */
	task_cputime(tsk, &utime, &stime);
	stats->ac_utime = div_u64(utime, NSEC_PER_USEC);
	stats->ac_stime = div_u64(stime, NSEC_PER_USEC);

	task_cputime_scaled(tsk, &utimescaled, &stimescaled);
	stats->ac_utimescaled = div_u64(utimescaled, NSEC_PER_USEC);
	stats->ac_stimescaled = div_u64(stimescaled, NSEC_PER_USEC);

	stats->ac_minflt = tsk->min_flt;
	stats->ac_majflt = tsk->maj_flt;

	/* strscpy_pad 清零未使用尾部，避免把旧栈/缓冲内容带入用户 ABI。 */
	strscpy_pad(stats->ac_comm, tsk->comm);
}


#ifdef CONFIG_TASK_XACCT

#define KB 1024
#define MB (1024*KB)
#define KB_MASK (~(KB-1))
/*
 * fill in extended accounting fields
 */
/*
 * xacct_add_tsk() - 在 CONFIG_TASK_XACCT 下填充内存高水位和 I/O 字段。
 *
 * @stats: 调用者拥有的输出 taskstats，本函数只覆盖扩展记账字段。
 * @p: 被采集任务，借用指针；调用者保证 task 生命周期。
 *
 * 调用位置：fill_stats() 在基础记账后调用。积分计数按 ABI 单位换算；
 * get_task_mm() 成功会取得 mm 引用，读取高水位后由 mmput() 配对。函数
 * mmput 的最终释放路径可能睡眠，
 * 因此调用者必须位于进程上下文。
 * CONFIG_TASK_IO_ACCOUNTING 关闭时存储字节字段显式写零。
 */
void xacct_add_tsk(struct taskstats *stats, struct task_struct *p)
{
	struct mm_struct *mm;

	/* convert pages-nsec/1024 to Mbyte-usec, see __acct_update_integrals */
	/*
	 * acct_*_mem1 保存页面数与运行纳秒的积分；乘 PAGE_SIZE 后按历史 ABI
	 * 比例换算为内存-时间量，
	 * 转换公式必须与 __acct_update_integrals() 配对。
	 */
	stats->coremem = p->acct_rss_mem1 * PAGE_SIZE;
	do_div(stats->coremem, 1000 * KB);
	stats->virtmem = p->acct_vm_mem1 * PAGE_SIZE;
	do_div(stats->virtmem, 1000 * KB);
	mm = get_task_mm(p);
	if (mm) {
		/* adjust to KB unit */
		/*
		 * UAPI 高水位单位是 KB；
		 * mm 引用保证读取期间地址空间对象不释放。
		 */
		stats->hiwater_rss   = get_mm_hiwater_rss(mm) * PAGE_SIZE / KB;
		stats->hiwater_vm    = get_mm_hiwater_vm(mm)  * PAGE_SIZE / KB;
		mmput(mm);
	}
	stats->read_char	= p->ioac.rchar & KB_MASK;
	stats->write_char	= p->ioac.wchar & KB_MASK;
	stats->read_syscalls	= p->ioac.syscr & KB_MASK;
	stats->write_syscalls	= p->ioac.syscw & KB_MASK;
#ifdef CONFIG_TASK_IO_ACCOUNTING
	stats->read_bytes	= p->ioac.read_bytes & KB_MASK;
	stats->write_bytes	= p->ioac.write_bytes & KB_MASK;
	stats->cancelled_write_bytes = p->ioac.cancelled_write_bytes & KB_MASK;
#else
	stats->read_bytes	= 0;
	stats->write_bytes	= 0;
	stats->cancelled_write_bytes = 0;
#endif
}
#undef KB
#undef MB

static void __acct_update_integrals(struct task_struct *tsk,
				    u64 utime, u64 stime)
{
	/*
	 * 把本次新增 CPU 时间乘以当前 RSS/VM，形成内存-时间积分。内核线程或无 mm
	 * 任务无此含义；小于一 tick 的增量延后累计以降低热路径成本。
	 */
	u64 time, delta;

	if (unlikely(!tsk->mm || (tsk->flags & PF_KTHREAD)))
		return;

	time = stime + utime;
	delta = time - tsk->acct_timexpd;

	if (delta < TICK_NSEC)
		return;

	tsk->acct_timexpd = time;
	/*
	 * Divide by 1024 to avoid overflow, and to avoid division.
	 * The final unit reported to userspace is Mbyte-usecs,
	 * the rest of the math is done in xacct_add_tsk.
	 * 先右移 10 位降低乘法结果溢出风险，最终比例在 xacct_add_tsk() 补齐。
	 */
	tsk->acct_rss_mem1 += delta * get_mm_rss(tsk->mm) >> 10;
	tsk->acct_vm_mem1 += delta * READ_ONCE(tsk->mm->total_vm) >> 10;
}

/**
 * acct_update_integrals - update mm integral fields in task_struct
 * @tsk: task_struct for accounting
 *
 * 采样累计 CPU 时间后更新积分；关闭本地 IRQ 防止本 CPU 的记账
 * 更新与读改写交错。调用者不持有额外引用，本函数不睡眠。
 */
void acct_update_integrals(struct task_struct *tsk)
{
	u64 utime, stime;
	unsigned long flags;

	local_irq_save(flags);
	task_cputime(tsk, &utime, &stime);
	__acct_update_integrals(tsk, utime, stime);
	local_irq_restore(flags);
}

/**
 * acct_account_cputime - update mm integral after cputime update
 * @tsk: task_struct for accounting
 *
 * CPU 记账热路径已拥有最新 utime/stime，直接更新积分以避免重采样。
 */
void acct_account_cputime(struct task_struct *tsk)
{
	__acct_update_integrals(tsk, tsk->utime, tsk->stime);
}

/**
 * acct_clear_integrals - clear the mm integral fields in task_struct
 * @tsk: task_struct whose accounting fields are cleared
 *
 * exec 等建立新记账阶段时同时清除基准时间与两类积分，防止跨阶段
 * 重复累计；调用者负责与并发 CPU 记账串行化。
 */
void acct_clear_integrals(struct task_struct *tsk)
{
	tsk->acct_timexpd = 0;
	tsk->acct_rss_mem1 = 0;
	tsk->acct_vm_mem1 = 0;
}
#endif
