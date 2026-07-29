// SPDX-License-Identifier: GPL-2.0-only
/*
 * Linux signal 子系统学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5 Codex（2026-07-29）。
 *
 * 本文件实现信号从“产生”到“选择、排队、唤醒、递送、停止/继续、ptrace
 * 交互和用户 ABI”的核心状态机。线程私有 pending 位于 task->pending，
 * 线程组共享 pending 位于 signal->shared_pending；sighand->siglock 串行化
 * handler、pending 队列、jobctl 与大部分 group 状态转换。
 *
 * 主链路：
 *   kill/tgkill/pidfd/timer/fault -> 权限检查 -> prepare/complete/send_signal
 *   -> pending bit + 可选 sigqueue -> TIF_SIGPENDING/唤醒目标
 *   -> get_signal/dequeue_signal -> ptrace/group-stop/default action/user handler
 *   -> 架构 setup_rt_frame，返回用户态；fatal action -> coredump/exit。
 *
 * 位图只表达“某 signal 至少 pending 一次”；标准信号可合并，实时信号按
 * sigqueue 保留多实例与 siginfo。task/cred/pid/ucounts 的引用、RCU 与
 * siglock 分别解决生命周期、读侧和状态一致性，不能相互替代。
 *
 * 设计收益是 POSIX 语义、线程组路由与低成本位图快路；代价是 handler、
 * mask、job-control、ptrace、timer 和退出可并发改变，所有重检与发布次序
 * 都是防止丢信号、重复递送、错误停止或 UAF 的协议组成部分。
 */
/*
 *  linux/kernel/signal.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  1997-11-02  Modified for POSIX.1b signals by Richard Henderson
 *
 *  2003-06-02  Jim Houston - Concurrent Computer Corp.
 *		Changes to use preallocated sigqueue structures
 *		to allow signals to be sent reliably.
 */
/*
 * 1997 年加入 POSIX.1b 实时信号；2003 年引入预分配 sigqueue，
 * 使 POSIX timer 等即使普通内存分配受限也能可靠保留待递送记录。
 */

#include <linux/slab.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/sched/mm.h>
#include <linux/sched/user.h>
#include <linux/sched/debug.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/cputime.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/tty.h>
#include <linux/binfmts.h>
#include <linux/coredump.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/ptrace.h>
#include <linux/signal.h>
#include <linux/signalfd.h>
#include <linux/ratelimit.h>
#include <linux/task_work.h>
#include <linux/capability.h>
#include <linux/freezer.h>
#include <linux/pid_namespace.h>
#include <linux/nsproxy.h>
#include <linux/user_namespace.h>
#include <linux/uprobes.h>
#include <linux/compat.h>
#include <linux/cn_proc.h>
#include <linux/compiler.h>
#include <linux/posix-timers.h>
#include <linux/cgroup.h>
#include <linux/audit.h>
#include <linux/sysctl.h>
#include <uapi/linux/pidfd.h>

#define CREATE_TRACE_POINTS
#include <trace/events/signal.h>

#include <asm/param.h>
#include <linux/uaccess.h>
#include <asm/unistd.h>
#include <asm/siginfo.h>
#include <asm/cacheflush.h>
#include <asm/syscall.h>	/* for syscall_get_* */
/* 引入架构系统调用取参接口，供 seccomp SIGSYS 信息构造使用。 */

#include "time/posix-timers.h"

/*
 * SLAB caches for signal bits.
 */
/* 下列 slab 专门分配携带 siginfo 的 struct sigqueue。 */

/*
 * sigqueue_cachep：signals_init 建立、运行期永久使用的队列记录 slab。
 * 单个记录由 pending 队列或 POSIX timer 预分配引用协议持有。
 */
static struct kmem_cache *sigqueue_cachep;

/* print_fatal_signals：只读居多的调试开关，也控制 RLIMIT 丢信号日志。 */
int print_fatal_signals __read_mostly;

/*
 * sig_handler() - 在 siglock 保护下取得任务对某信号的 handler 值。
 * @t 借用且 sighand 稳定；@sig 为 1.._NSIG。返回 SIG_DFL/SIG_IGN/
 * SIG_KTHREAD_KERNEL 或用户函数指针，不取得引用。
 */
static void __user *sig_handler(struct task_struct *t, int sig)
{
	return t->sighand->action[sig - 1].sa.sa_handler;
}

/*
 * sig_handler_ignored() - 判断 handler 是否显式或按默认动作忽略。
 * SIG_IGN 显式忽略；SIG_DFL 仅对 sig_kernel_ignore 集合忽略。纯判断。
 */
static inline bool sig_handler_ignored(void __user *handler, int sig)
{
	/* Is it explicitly or implicitly ignored? */
	/* 同时覆盖 SIG_IGN 与默认动作本来就是 ignore 的信号。 */
	return handler == SIG_IGN ||
	       (handler == SIG_DFL && sig_kernel_ignore(sig));
}

/*
 * sig_task_ignored() - 在已知 handler/任务类型下判断目标是否拒绝该信号。
 *
 * @t/@sig 借用输入；@force 只允许内核强制的 kernel-only 信号突破部分
 * UNKILLABLE/kthread 过滤。全局 init 不接收 SIGKILL/SIGSTOP；UNKILLABLE
 * 默认动作和只接内核信号的 kthread 也受保护。返回 bool，不改 pending。
 */
static bool sig_task_ignored(struct task_struct *t, int sig, bool force)
{
	void __user *handler;

	handler = sig_handler(t, sig);

	/* SIGKILL and SIGSTOP may not be sent to the global init */
	/* PID 1 是系统生存锚点，内核只允许其显式 handler 决定退出。 */
	if (unlikely(is_global_init(t) && sig_kernel_only(sig)))
		return true;

	if (unlikely(t->signal->flags & SIGNAL_UNKILLABLE) &&
	    handler == SIG_DFL && !(force && sig_kernel_only(sig)))
		return true;

	/* Only allow kernel generated signals to this kthread */
	/* SIG_KTHREAD_KERNEL 标记的 kthread handler 拒绝非强制普通来源。 */
	if (unlikely((t->flags & PF_KTHREAD) &&
		     (handler == SIG_KTHREAD_KERNEL) && !force))
		return true;

	return sig_handler_ignored(handler, sig);
}

/*
 * sig_ignored() - 结合 mask 与 ptrace 判断“现在发送是否可直接丢弃”。
 *
 * blocked 信号不能按当前 handler 丢弃，因为解阻塞前 disposition 可改变；
 * ptracer 需要观察即使通常忽略的信号（SIGKILL 例外）。其余委托
 * sig_task_ignored。调用者通常持 siglock；返回 bool。
 */
static bool sig_ignored(struct task_struct *t, int sig, bool force)
{
	/*
	 * Blocked signals are never ignored, since the
	 * signal handler may change by the time it is
	 * unblocked.
	 */
	/*
	 * pending 保存的是未来递送承诺；blocked 期间 handler 可由
	 * sigaction 改变，发送时提前丢弃会永久丢失本应递送的信号。
	 */
	if (sigismember(&t->blocked, sig) || sigismember(&t->real_blocked, sig))
		return false;

	/*
	 * Tracers may want to know about even ignored signal unless it
	 * is SIGKILL which can't be reported anyway but can be ignored
	 * by SIGNAL_UNKILLABLE task.
	 */
	/*
	 * ptrace 把 ignored signal 也作为可观察事件；SIGKILL 无法
	 * 报告/抑制，且 UNKILLABLE 目标可特殊忽略。
	 */
	if (t->ptrace && sig != SIGKILL)
		return false;

	return sig_task_ignored(t, sig, force);
}

/*
 * Re-calculate pending state from the set of locally pending
 * signals, globally pending signals, and blocked signals.
 */
/*
 * 计算一个 pending 位图中是否存在未被 @blocked 屏蔽的位。
 * 针对常见 _NSIG_WORDS=1/2/4 展开快路，其余循环。返回 bool，不修改集合。
 */
static inline bool has_pending_signals(sigset_t *signal, sigset_t *blocked)
{
	unsigned long ready;
	long i;

	switch (_NSIG_WORDS) {
	default:
		for (i = _NSIG_WORDS, ready = 0; --i >= 0 ;)
			ready |= signal->sig[i] &~ blocked->sig[i];
		break;

	case 4: ready  = signal->sig[3] &~ blocked->sig[3];
		/* 四字机器集合逐字做 pending-minus-blocked 并 OR 为存在性结论。 */
		ready |= signal->sig[2] &~ blocked->sig[2];
		ready |= signal->sig[1] &~ blocked->sig[1];
		ready |= signal->sig[0] &~ blocked->sig[0];
	/* 逐机器字计算 pending 与 blocked 的差集，尾字还必须屏蔽超出 _NSIG 的位。 */
		break;

	case 2: ready  = signal->sig[1] &~ blocked->sig[1];
		ready |= signal->sig[0] &~ blocked->sig[0];
		break;

	case 1: ready  = signal->sig[0] &~ blocked->sig[0];
	}
	return ready !=	0;
}

/*
 * PENDING() 把 sigpending 容器的位图与 blocked 集合交给公共检测器。
 * @p/@b 均为借用且只读；宏返回是否存在未屏蔽信号，无锁语义由调用者提供。
 */
#define PENDING(p,b) has_pending_signals(&(p)->signal, (b))

/*
 * recalc_sigpending_tsk() - 汇总线程/组 pending、jobctl 与 freezer，必要时置 TIF_SIGPENDING。
 *
 * @t 由调用者稳定，通常持其 siglock。任何可处理事件都置 thread flag 并
 * 返回 true；无事件也不在这里清位，因为远端线程或 syscall restart
 * 窗口中清除会丢唤醒/破坏 -ERESTART*。清位只允许 current 的知情调用者。
 */
static bool recalc_sigpending_tsk(struct task_struct *t)
{
	if ((t->jobctl & (JOBCTL_PENDING_MASK | JOBCTL_TRAP_FREEZE)) ||
	    PENDING(&t->pending, &t->blocked) ||
	    PENDING(&t->signal->shared_pending, &t->blocked) ||
	    cgroup_task_frozen(t)) {
		set_tsk_thread_flag(t, TIF_SIGPENDING);
		return true;
	}

	/*
	 * We must never clear the flag in another thread, or in current
	 * when it's possible the current syscall is returning -ERESTART*.
	 * So we don't clear it here, and only callers who know they should do.
	 */
	/*
	 * 远端清 TIF 可与目标刚入返回用户态竞态；current 若正携带
	 * restart errno，提前清也会跳过 signal/restart 处理，因此只报告 false。
	 */
	return false;
}

/*
 * recalc_sigpending() - 为 current 重算并在安全条件下清 TIF_SIGPENDING。
 * 只有没有 pending/jobctl/cgroup freeze 且 freezer 也不需要该位时清除。
 * 调用者通常持 current siglock；无返回。
 */
void recalc_sigpending(void)
{
	if (!recalc_sigpending_tsk(current) && !freezing(current)) {
		if (unlikely(test_thread_flag(TIF_SIGPENDING)))
			clear_thread_flag(TIF_SIGPENDING);
	}
}
EXPORT_SYMBOL(recalc_sigpending);

/*
 * calculate_sigpending() - fork 后强制 current 重新合并延迟事件。
 * 持 siglock+IRQ off，先置 TIF 再 recalc，关闭 fork 复制状态到事件发布间
 * 的丢唤醒窗口。无参数/返回。
 */
void calculate_sigpending(void)
{
	/* Have any signals or users of TIF_SIGPENDING been delayed
	 * until after fork?
	 */
	/* 检查 fork 完成前被延迟的信号或其他 TIF 使用者。 */
	spin_lock_irq(&current->sighand->siglock);
	set_tsk_thread_flag(current, TIF_SIGPENDING);
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);
}

/* Given the mask, find the first available signal that should be serviced. */
/* 在 pending-mask 中选择下一个应处理信号，同步故障优先。 */

#define SYNCHRONOUS_MASK \
	(sigmask(SIGSEGV) | sigmask(SIGBUS) | sigmask(SIGILL) | \
	 sigmask(SIGTRAP) | sigmask(SIGFPE) | sigmask(SIGSYS))

/*
 * next_signal() - 从 pending 位图选择最低编号的未屏蔽信号。
 *
 * 第一机器字含同步 fault 信号，若其中任何一个 pending，则只在同步集合
 * 内选最低位，使 SIGSEGV/BUS/ILL/TRAP/FPE/SYS 优先于普通异步信号。
 * 其余字按编号扫描。返回 1.._NSIG 或 0；不摘位、不访问 sigqueue。
 */
int next_signal(struct sigpending *pending, sigset_t *mask)
{
	unsigned long i, *s, *m, x;
	int sig = 0;

	s = pending->signal.sig;
	m = mask->sig;

	/*
	 * Handle the first word specially: it contains the
	 * synchronous signals that need to be dequeued first.
	 */
	/* 同步信号对应当前执行故障，延后可能让线程继续错误指令。 */
	x = *s &~ *m;
	if (x) {
		if (x & SYNCHRONOUS_MASK)
			x &= SYNCHRONOUS_MASK;
		sig = ffz(~x) + 1;
		return sig;
	}

	/* 首字没有候选时，再按机器字顺序寻找全局最低的未屏蔽信号。 */
	switch (_NSIG_WORDS) {
	default:
		for (i = 1; i < _NSIG_WORDS; ++i) {
			x = *++s &~ *++m;
			if (!x)
				continue;
			/* 首个非零机器字的最低 bit 就是全局最小可递送 signo。 */
			sig = ffz(~x) + i*_NSIG_BPW + 1;
			break;
		}
		break;

	case 2:
		x = s[1] &~ m[1];
		/* 两字集合的第二字直接计算候选，命中后换算为全局 signo。 */
		if (!x)
			break;
		sig = ffz(~x) + _NSIG_BPW + 1;
		break;

	case 1:
		/* Nothing to do */
		/* 信号集合仅有一个机器字，首字已在上方处理，无需继续扫描。 */
		break;
	}

	return sig;
}

/*
 * print_dropped_signal() - 限速记录因 RLIMIT_SIGPENDING 丢弃的信号。
 * print_fatal_signals 关闭时无开销；5 秒最多 10 条，防攻击者刷日志。
 * @sig 仅用于诊断；无返回。
 */
static inline void print_dropped_signal(int sig)
{
	static DEFINE_RATELIMIT_STATE(ratelimit_state, 5 * HZ, 10);

	if (!print_fatal_signals)
		return;

	if (!__ratelimit(&ratelimit_state))
		return;

	pr_info("%s/%d: reached RLIMIT_SIGPENDING, dropped signal %d\n",
				current->comm, current->pid, sig);
}

/**
 * task_set_jobctl_pending - set jobctl pending bits
 * @task: target task
 * @mask: pending bits to set
 *
 * Clear @mask from @task->jobctl.  @mask must be subset of
 * %JOBCTL_PENDING_MASK | %JOBCTL_STOP_CONSUME | %JOBCTL_STOP_SIGMASK |
 * %JOBCTL_TRAPPING.  If stop signo is being set, the existing signo is
 * cleared.  If @task is already being killed or exiting, this function
 * becomes noop.
 *
 * CONTEXT:
 * Must be called with @task->sighand->siglock held.
 *
 * RETURNS:
 * %true if @mask is set, %false if made noop because @task was dying.
 */
/*
 * 中文学习注释：
 * - @mask 同时编码“待处理事件”和 stop 信号编号；设置新 stop 编号前必须先
 *   清旧编号，否则两个编号按位或后会成为无效信号。
 * - JOBCTL_TRAPPING 只能依附于实际的 pending trap/stop，两个 BUG_ON()
 *   把这一状态机约束固定在入口。
 * - 已有致命信号或正在退出的任务不会再进入新的作业控制状态，返回 false
 *   让调用者不要等待一个永远不会发生的 trap。
 * 锁：调用者持有 task->sighand->siglock，故 jobctl 的复合更新不可被信号
 * 递送路径拆开观察。
 */
bool task_set_jobctl_pending(struct task_struct *task, unsigned long mask)
{
	BUG_ON(mask & ~(JOBCTL_PENDING_MASK | JOBCTL_STOP_CONSUME |
			JOBCTL_STOP_SIGMASK | JOBCTL_TRAPPING));
	BUG_ON((mask & JOBCTL_TRAPPING) && !(mask & JOBCTL_PENDING_MASK));

	if (unlikely(fatal_signal_pending(task) || (task->flags & PF_EXITING)))
		return false;

	/* 通过入口约束后，先替换 stop signo 槽，再原子合并其余 pending 状态位。 */
	if (mask & JOBCTL_STOP_SIGMASK)
		task->jobctl &= ~JOBCTL_STOP_SIGMASK;

	task->jobctl |= mask;
	return true;
}

/**
 * task_clear_jobctl_trapping - clear jobctl trapping bit
 * @task: target task
 *
 * If JOBCTL_TRAPPING is set, a ptracer is waiting for us to enter TRACED.
 * Clear it and wake up the ptracer.  Note that we don't need any further
 * locking.  @task->siglock guarantees that @task->parent points to the
 * ptracer.
 *
 * CONTEXT:
 * Must be called with @task->sighand->siglock held.
 */
/*
 * 中文学习注释：JOBCTL_TRAPPING 也是 ptracer 的等待条件。清位后先执行
 * smp_mb() 再 wake_up_bit()，保证等待者被唤醒时一定能看到清位结果；
 * siglock 还稳定了 task->parent，使唤醒对应的确是当前 tracer。
 */
void task_clear_jobctl_trapping(struct task_struct *task)
{
	if (unlikely(task->jobctl & JOBCTL_TRAPPING)) {
		task->jobctl &= ~JOBCTL_TRAPPING;
		smp_mb();	/* advised by wake_up_bit() */
		/*
		 * 该全屏障是 wake_up_bit() 等待协议建议的发布屏障，
		 * 确保等待者被唤醒后先看到 JOBCTL_TRAPPING 已清除。
		 */
		wake_up_bit(&task->jobctl, JOBCTL_TRAPPING_BIT);
	}
}

/**
 * task_clear_jobctl_pending - clear jobctl pending bits
 * @task: target task
 * @mask: pending bits to clear
 *
 * Clear @mask from @task->jobctl.  @mask must be subset of
 * %JOBCTL_PENDING_MASK.  If %JOBCTL_STOP_PENDING is being cleared, other
 * STOP bits are cleared together.
 *
 * If clearing of @mask leaves no stop or trap pending, this function calls
 * task_clear_jobctl_trapping().
 *
 * CONTEXT:
 * Must be called with @task->sighand->siglock held.
 */
/*
 * 中文学习注释：清 STOP_PENDING 时必须连同“一次性消费权”和“已出队”
 * 标记一起清理，避免下一轮 group-stop 继承旧状态。最后一个 pending 位
 * 消失时还要解除 TRAPPING 并唤醒 ptracer，完成状态机的收尾。
 */
void task_clear_jobctl_pending(struct task_struct *task, unsigned long mask)
{
	BUG_ON(mask & ~JOBCTL_PENDING_MASK);

	if (mask & JOBCTL_STOP_PENDING)
		mask |= JOBCTL_STOP_CONSUME | JOBCTL_STOP_DEQUEUED;

	task->jobctl &= ~mask;

	if (!(task->jobctl & JOBCTL_PENDING_MASK))
		task_clear_jobctl_trapping(task);
}

/**
 * task_participate_group_stop - participate in a group stop
 * @task: task participating in a group stop
 *
 * @task has %JOBCTL_STOP_PENDING set and is participating in a group stop.
 * Group stop states are cleared and the group stop count is consumed if
 * %JOBCTL_STOP_CONSUME was set.  If the consumption completes the group
 * stop, the appropriate `SIGNAL_*` flags are set.
 *
 * CONTEXT:
 * Must be called with @task->sighand->siglock held.
 *
 * RETURNS:
 * %true if group stop completion should be notified to the parent, %false
 * otherwise.
 */
/*
 * 中文学习注释：
 * - 每个真正“消费”STOP_PENDING 的线程将 group_stop_count 减一。
 * - 计数首次归零且尚未标记 STOPPED，说明本轮线程组停止刚刚完成；由这个
 *   最后参与者返回 true，调用者据此只向父进程发送一次完成通知。
 * - 没有 STOP_CONSUME 的任务只清本地状态，不碰共享计数。
 */
static bool task_participate_group_stop(struct task_struct *task)
{
	struct signal_struct *sig = task->signal;
	bool consume = task->jobctl & JOBCTL_STOP_CONSUME;

	WARN_ON_ONCE(!(task->jobctl & JOBCTL_STOP_PENDING));

	task_clear_jobctl_pending(task, JOBCTL_STOP_PENDING);

	if (!consume)
		return false;

	if (!WARN_ON_ONCE(sig->group_stop_count == 0))
		sig->group_stop_count--;

	/*
	 * Tell the caller to notify completion iff we are entering into a
	 * fresh group stop.  Read comment in do_signal_stop() for details.
	 */
	/*
	 * 仅当一轮新的组停止刚完成时要求调用者通知父进程；
	 * do_signal_stop() 解释了计数归零与 SIGNAL_STOP_STOPPED 的配合。
	 */
	if (!sig->group_stop_count && !(sig->flags & SIGNAL_STOP_STOPPED)) {
		signal_set_stop_flags(sig, SIGNAL_STOP_STOPPED);
		return true;
	}
	return false;
}

/*
 * task_join_group_stop() - 让新建线程加入 current 所在组的停止状态。
 * 已有未完成 stop 时增加 group_stop_count；若整组已停，则新线程也挂起，
 * 但无需再次消费完成计数。调用者在受保护的线程组创建路径中调用。
 */
void task_join_group_stop(struct task_struct *task)
{
	unsigned long mask = current->jobctl & JOBCTL_STOP_SIGMASK;
	struct signal_struct *sig = current->signal;

	if (sig->group_stop_count) {
		sig->group_stop_count++;
		mask |= JOBCTL_STOP_CONSUME;
	} else if (!(sig->flags & SIGNAL_STOP_STOPPED))
		return;

	/* Have the new thread join an on-going signal group stop */
	/* 新线程加入进行中的 group-stop，并成为新的计数参与者。 */
	task_set_jobctl_pending(task, mask | JOBCTL_STOP_PENDING);
}

/*
 * sig_get_ucounts() - 为一个新的排队信号预占用户级 RLIMIT_SIGPENDING 配额。
 *
 * task_ucounts() 依赖目标凭据，故在 RCU 读侧临界区取引用并增加计数。
 * inc_rlimit_get_ucounts() 在 0->1 时同时取得 ucounts 引用；失败或超过软
 * 限制时对称回滚。@override_rlimit 供内核必须递送的路径绕过软限制。
 * 成功返回持有计数的 ucounts，失败返回 NULL。
 */
static struct ucounts *sig_get_ucounts(struct task_struct *t, int sig,
				       int override_rlimit)
{
	struct ucounts *ucounts;
	long sigpending;

	/*
	 * Protect access to @t credentials. This can go away when all
	 * callers hold rcu read lock.
	 *
	 * NOTE! A pending signal will hold on to the user refcount,
	 * and we get/put the refcount only when the sigpending count
	 * changes from/to zero.
	 */
	/*
	 * RCU 当前用于稳定 @t 的凭据及其 ucounts 指针；若所有调用者
	 * 将来都已处于 RCU 读侧临界区，这层可删除。pending 信号在计数非零
	 * 期间保持 user 引用，只在 0→1 时取得、1→0 时释放，避免逐信号引用操作。
	 */
	rcu_read_lock();
	ucounts = task_ucounts(t);
	sigpending = inc_rlimit_get_ucounts(ucounts, UCOUNT_RLIMIT_SIGPENDING,
					    override_rlimit);
	rcu_read_unlock();
	if (!sigpending)
		return NULL;

	if (unlikely(!override_rlimit && sigpending > task_rlimit(t, RLIMIT_SIGPENDING))) {
	/* sigpending 配额的 ucounts 获取与计数增加必须成对，超限时撤销并返回 NULL。 */
		dec_rlimit_put_ucounts(ucounts, UCOUNT_RLIMIT_SIGPENDING);
		print_dropped_signal(sig);
		return NULL;
	}

	return ucounts;
}

/* __sigqueue_init()：初始化新节点，并把用户配额的释放责任交给该节点。 */
static void __sigqueue_init(struct sigqueue *q, struct ucounts *ucounts,
			    const unsigned int sigqueue_flags)
{
	/* 队列节点先初始化为孤立链表，所有权尚未交给任何 sigpending。 */
	INIT_LIST_HEAD(&q->list);
	q->flags = sigqueue_flags;
	q->ucounts = ucounts;
}

/*
 * allocate a new signal queue record
 * - this may be called without locks if and only if t == current, otherwise an
 *   appropriate lock must be held to stop the target task from exiting
 */
/*
 * 中文学习注释：分配分两阶段完成：先记账 RLIMIT_SIGPENDING，再从 slab
 * 取 sigqueue。第二阶段失败必须撤销第一阶段；成功后 q 同时拥有 ucounts
 * 计数责任，直到 __sigqueue_free() 或预分配定时器路径释放。
 */
static struct sigqueue *sigqueue_alloc(int sig, struct task_struct *t, gfp_t gfp_flags,
				       int override_rlimit)
{
	struct ucounts *ucounts = sig_get_ucounts(t, sig, override_rlimit);
	struct sigqueue *q;

	if (!ucounts)
		return NULL;

	q = kmem_cache_alloc(sigqueue_cachep, gfp_flags);
	/* slab 分配失败必须归还此前取得的 ucounts 配额引用。 */
	if (!q) {
		dec_rlimit_put_ucounts(ucounts, UCOUNT_RLIMIT_SIGPENDING);
		return NULL;
	}

	__sigqueue_init(q, ucounts, 0);
	return q;
}

static void __sigqueue_free(struct sigqueue *q)
{
	/*
	 * 预分配 POSIX timer 节点由 timer 引用计数管理，不能直接归还 slab；
	 * 普通节点则先归还用户 pending 配额，再释放对象。
	 */
	if (q->flags & SIGQUEUE_PREALLOC) {
		posixtimer_sigqueue_putref(q);
		return;
	}
	if (q->ucounts) {
		dec_rlimit_put_ucounts(q->ucounts, UCOUNT_RLIMIT_SIGPENDING);
		q->ucounts = NULL;
	}
	kmem_cache_free(sigqueue_cachep, q);
}

/*
 * 契约补充：@queue 是 siglock 下借用的私有或共享 pending 容器；本函数
 * 不睡眠。返回无；位图清零，节点和其配额引用按类型释放，所有权不外泄。
 */
void flush_sigqueue(struct sigpending *queue)
{
	struct sigqueue *q;

	/*
	 * 位图与链表是同一 pending 集合的两种索引，必须一起清空。逐节点使用
	 * __sigqueue_free()，确保 RLIMIT 记账和 timer 特例都得到对称处理。
	 */
	sigemptyset(&queue->signal);
	while (!list_empty(&queue->list)) {
		q = list_entry(queue->list.next, struct sigqueue , list);
		list_del_init(&q->list);
		__sigqueue_free(q);
	}
}

/*
 * Flush all pending signals for this kthread.
 */
/*
 * 中文学习注释：同时清线程私有 pending 与线程组 shared_pending；持
 * siglock 并关闭本地中断，阻止发送者并发挂入。先清 TIF_SIGPENDING，
 * 因为两个实际来源随后都会被清空。
 */
void flush_signals(struct task_struct *t)
{
	unsigned long flags;

	spin_lock_irqsave(&t->sighand->siglock, flags);
	clear_tsk_thread_flag(t, TIF_SIGPENDING);
	flush_sigqueue(&t->pending);
	flush_sigqueue(&t->signal->shared_pending);
	spin_unlock_irqrestore(&t->sighand->siglock, flags);
}
EXPORT_SYMBOL(flush_signals);

void ignore_signals(struct task_struct *t)
{
	int i;

	/* 内核线程常用：先把全部 disposition 改为忽略，再清掉既有 pending。 */
	for (i = 0; i < _NSIG; ++i)
		t->sighand->action[i].sa.sa_handler = SIG_IGN;

	flush_signals(t);
}

/*
 * Flush all handlers for a task.
 */
/* 为任务重置整张 signal action 表，通常服务于 exec/内核线程清理。 */

/*
 * 中文学习注释：exec 等路径用它重置用户安装的处理器。SIG_IGN 是否保留
 * 由 @force_default 决定；无论处理器值如何，flags、restorer 和 mask 都
 * 必须清零，不能把旧映像的用户地址或屏蔽集合带到新映像。
 * 调用者负责持有 sighand 所需的排他保护。
 */
void
flush_signal_handlers(struct task_struct *t, int force_default)
{
	int i;
	struct k_sigaction *ka = &t->sighand->action[0];
	for (i = _NSIG ; i != 0 ; i--) {
		if (force_default || ka->sa.sa_handler != SIG_IGN)
			ka->sa.sa_handler = SIG_DFL;
	/* 逐信号恢复默认 disposition，可选择保留 ignored，并清除用户 handler 专属 flags。 */
		ka->sa.sa_flags = 0;
#ifdef __ARCH_HAS_SA_RESTORER
		ka->sa.sa_restorer = NULL;
#endif
		sigemptyset(&ka->sa.sa_mask);
		ka++;
	}
}

bool unhandled_signal(struct task_struct *tsk, int sig)
{
	void __user *handler = tsk->sighand->action[sig-1].sa.sa_handler;
	/*
	 * “unhandled”用于判断内核默认动作是否可能生效：init 特殊处理；
	 * 用户 handler 明确接管则为 false；濒死任务忽略新增信号；被 ptrace
	 * 的任务把最终决定交给 tracer。
	 */
	if (is_global_init(tsk))
		return true;

	if (handler != SIG_IGN && handler != SIG_DFL)
		return false;

	/* If dying, we handle all new signals by ignoring them */
	/* 目标已有致命信号时，新信号不再改变最终结果，按已处理返回。 */
	if (fatal_signal_pending(tsk))
		return false;

	/* if ptraced, let the tracer determine */
	/* 被跟踪任务的默认动作由 tracer 审核，不能提前判为无人处理。 */
	return !tsk->ptrace;
}

static void collect_signal(int sig, struct sigpending *list, kernel_siginfo_t *info,
			   struct sigqueue **timer_sigq)
{
	struct sigqueue *q, *first = NULL;

	/* 先在队列中定位同号节点并判断是否仍有副本，再决定清位、摘链与释放方式。 */
	/*
	 * Collect the siginfo appropriate to this signal.  Check if
	 * there is another siginfo for the same signal.
	*/
	/*
	 * 链表可能有多个同号实时信号。找到第二个时跳到
	 * still_pending，表示本次只摘第一个、位图仍须保持；若只有一个则先
	 * 清位图。普通信号合并时通常只会存在一个记录。
	 */
	list_for_each_entry(q, &list->list, list) {
		if (q->info.si_signo == sig) {
			if (first)
				goto still_pending;
			first = q;
		}
	}

	sigdelset(&list->signal, sig);

	/* 有队列节点就转移其 payload/所有权；仅有位图时在下方合成 SI_USER。 */
	if (first) {
still_pending:
		list_del_init(&first->list);
		copy_siginfo(info, &first->info);

		/*
		 * posix-timer signals are preallocated and freed when the last
		 * reference count is dropped in posixtimer_deliver_signal() or
		 * immediately on timer deletion when the signal is not pending.
		 * Spare the extra round through __sigqueue_free() which is
		 * ignoring preallocated signals.
		 */
		/* 定时器节点的最终释放延后到递送/删除路径，本层只转交。 */
		if (unlikely((first->flags & SIGQUEUE_PREALLOC) && (info->si_code == SI_TIMER)))
			*timer_sigq = first;
		else
			__sigqueue_free(first);
	} else {
		/*
		 * Ok, it wasn't in the queue.  This must be
		 * a fast-pathed signal or we must have been
		 * out of queue space.  So zero out the info.
		 */
		/*
		 * 仅有位图而无节点来自快速路径或分配失败；仍必须递送
		 * 信号，但只能构造最保守的 SI_USER siginfo。
		 */
		clear_siginfo(info);
		info->si_signo = sig;
		info->si_errno = 0;
		info->si_code = SI_USER;
		info->si_pid = 0;
		info->si_uid = 0;
	}
}

static int __dequeue_signal(struct sigpending *pending, sigset_t *mask,
			    kernel_siginfo_t *info, struct sigqueue **timer_sigq)
{
	/* 先按优先级选信号，再原子地同步更新位图、链表和 siginfo。 */
	int sig = next_signal(pending, mask);

	if (sig)
		collect_signal(sig, pending, info, timer_sigq);
	return sig;
}

/*
 * Try to dequeue a signal. If a deliverable signal is found fill in the
 * caller provided siginfo and return the signal number. Otherwise return
 * 0.
 */
/*
 * 中文学习注释：
 * - 调用者持 current->sighand->siglock；先查线程私有队列，再查线程组
 *   共享队列，因此定向线程信号优先。
 * - @type 告知上层本次来自 PID 还是 TGID 队列；共享 SIGALRM 还会重装
 *   传统 itimer。
 * - 每次摘取后重算 TIF_SIGPENDING；若是 timer 预分配节点，离开 siglock
 *   后交给定时器递送协议，避免在锁内做其引用操作。
 */
int dequeue_signal(sigset_t *mask, kernel_siginfo_t *info, enum pid_type *type)
{
	struct task_struct *tsk = current;
	struct sigqueue *timer_sigq;
	int signr;

	lockdep_assert_held(&tsk->sighand->siglock);
	/* 线程私有 pending 优先；未命中才从 shared_pending 领取并重新选择共享接收者。 */

again:
	*type = PIDTYPE_PID;
	timer_sigq = NULL;
	signr = __dequeue_signal(&tsk->pending, mask, info, &timer_sigq);
	if (!signr) {
		/* 私有队列无可递送项时才转向线程组共享 pending。 */
		*type = PIDTYPE_TGID;
		signr = __dequeue_signal(&tsk->signal->shared_pending,
					 mask, info, &timer_sigq);

		if (unlikely(signr == SIGALRM))
			posixtimer_rearm_itimer(tsk);
	}

	recalc_sigpending();
	if (!signr)
		return 0;

	if (unlikely(sig_kernel_stop(signr))) {
		/*
		 * Set a marker that we have dequeued a stop signal.  Our
		 * caller might release the siglock and then the pending
		 * stop signal it is about to process is no longer in the
		 * pending bitmasks, but must still be cleared by a SIGCONT
		 * (and overruled by a SIGKILL).  So those cases clear this
		 * shared flag after we've set it.  Note that this flag may
		 * remain set after the signal we return is ignored or
		 * handled.  That doesn't matter because its only purpose
		 * is to alert stop-signal processing code when another
		 * processor has come along and cleared the flag.
		 */
		/*
		 * stop 信号出队后到真正执行 stop 之间存在“位图已无记录”
		 * 的窗口，用 JOBCTL_STOP_DEQUEUED 把这段隐含状态显式保存；并发
		 * SIGCONT/SIGKILL 因而仍能取消它。
		 */
		current->jobctl |= JOBCTL_STOP_DEQUEUED;
	}

	if (IS_ENABLED(CONFIG_POSIX_TIMERS) && unlikely(timer_sigq)) {
		if (!posixtimer_deliver_signal(info, timer_sigq))
			goto again;
	}

	return signr;
}
EXPORT_SYMBOL_GPL(dequeue_signal);

static int dequeue_synchronous_signal(kernel_siginfo_t *info)
{
	struct task_struct *tsk = current;
	struct sigpending *pending = &tsk->pending;
	struct sigqueue *q, *sync = NULL;

	/*
	 * Might a synchronous signal be in the queue?
	 */
	/* 只考察 current 私有队列：同步 fault 必然由当前线程自身产生。 */
	if (!((pending->signal.sig[0] & ~tsk->blocked.sig[0]) & SYNCHRONOUS_MASK))
		return 0;

	/*
	 * Return the first synchronous signal in the queue.
	 */
	/*
	 * 位图只说明“某信号存在”，链表和正 si_code 才能确认这是内核产生的
	 * 同步 fault，而不是用户伪造的同号异步信号。
	 */
	list_for_each_entry(q, &pending->list, list) {
		/* Synchronous signals have a positive si_code */
		/* 正 si_code 区分内核同步故障与用户伪造的同号异步信号。 */
		if ((q->info.si_code > SI_USER) &&
		    (sigmask(q->info.si_signo) & SYNCHRONOUS_MASK)) {
			sync = q;
			goto next;
		}
	}
	return 0;
next:
	/*
	 * Check if there is another siginfo for the same signal.
	 */
	/* 同号节点尚存则保留位图，否则同步清位并重算 TIF_SIGPENDING。 */
	list_for_each_entry_continue(q, &pending->list, list) {
		if (q->info.si_signo == sync->info.si_signo)
			goto still_pending;
	}

	sigdelset(&pending->signal, sync->info.si_signo);
	recalc_sigpending();
still_pending:
	/* 只从线程私有队列领取与当前同步 fault 对应的正 si_code 节点。 */
	list_del_init(&sync->list);
	copy_siginfo(info, &sync->info);
	__sigqueue_free(sync);
	return info->si_signo;
}

/*
 * Tell a process that it has a new active signal..
 *
 * NOTE! we rely on the previous spin_lock to
 * lock interrupts for us! We can only be called with
 * "siglock" held, and the local interrupt must
 * have been disabled when that got acquired!
 *
 * No need to set need_resched since signal event passing
 * goes through ->blocked
 */
/*
 * 中文学习注释：
 * - 先置 TIF_SIGPENDING 发布事件，再尝试从指定睡眠状态唤醒目标。
 * - 不能先读取 t->__state 决定是否唤醒：目标可能正在另一 CPU 上进入
 *   stopped；wake_up_state() 自带正确的状态同步。
 * - 若任务当前可运行，kick_process() 用 IPI 促使其尽快到达用户返回检查点。
 * 锁：必须持 siglock 且本地 IRQ 已关闭，以和 pending 队列发布形成整体。
 */
void signal_wake_up_state(struct task_struct *t, unsigned int state)
{
	lockdep_assert_held(&t->sighand->siglock);

	set_tsk_thread_flag(t, TIF_SIGPENDING);

	/*
	 * TASK_WAKEKILL also means wake it up in the stopped/traced/killable
	 * case. We don't check t->state here because there is a race with it
	 * executing another processor and just now entering stopped state.
	 * By using wake_up_state, we ensure the process will wake up and
	 * handle its death signal.
	 */
	/*
	 * TASK_WAKEKILL 还覆盖 stopped、traced 与 killable 睡眠。
	 * 这里不能先看 t->state，因为目标可能正在另一 CPU 上进入停止态；
	 * wake_up_state() 与状态转换同步，保证致命信号最终使其醒来处理退出。
	 */
	if (!wake_up_state(t, state | TASK_INTERRUPTIBLE))
		kick_process(t);
}

static inline void posixtimer_sig_ignore(struct task_struct *tsk, struct sigqueue *q);

static void sigqueue_free_ignored(struct task_struct *tsk, struct sigqueue *q)
{
	/* 被忽略的预分配 timer 信号须通知 timer 状态机，不能按普通节点释放。 */
	if (likely(!(q->flags & SIGQUEUE_PREALLOC) || q->info.si_code != SI_TIMER))
		__sigqueue_free(q);
	else
		posixtimer_sig_ignore(tsk, q);
}

/* Remove signals in mask from the pending set and queue. */
/*
 * 中文学习注释：在 siglock 下从位图和链表同步删除 @mask 指定的信号。
 * 先求交集可快速跳过无关队列；safe 迭代允许删除当前节点。该 helper 用于
 * SIGCONT 与 stop 信号互斥清理，也负责 timer 预分配节点的专用回收协议。
 */
static void flush_sigqueue_mask(struct task_struct *p, sigset_t *mask, struct sigpending *s)
{
	struct sigqueue *q, *n;
	sigset_t m;

	lockdep_assert_held(&p->sighand->siglock);

	sigandsets(&m, mask, &s->signal);
	if (sigisemptyset(&m))
		return;

	sigandnsets(&s->signal, &s->signal, mask);
	/* 位图先清除目标集合，再遍历链表摘除对应排队实例。 */
	list_for_each_entry_safe(q, n, &s->list, list) {
		if (sigismember(mask, q->info.si_signo)) {
			list_del_init(&q->list);
			sigqueue_free_ignored(p, q);
		}
	}
}

static inline int is_si_special(const struct kernel_siginfo *info)
{
	/* SEND_SIG_* 是小整数伪指针，只作内核调用约定，绝不能解引用。 */
	return info <= SEND_SIG_PRIV;
}

static inline bool si_fromuser(const struct kernel_siginfo *info)
{
	/* NOINFO 等价于用户 kill；PRIV/内核产生的正 si_code 不走用户权限语义。 */
	return info == SEND_SIG_NOINFO ||
		(!is_si_special(info) && SI_FROMUSER(info));
}

/*
 * called with RCU read lock from check_kill_permission()
 */
/*
 * 中文学习注释：传统 kill 权限允许发送者 real/effective uid 匹配目标
 * real/saved uid，或在目标 user namespace 中拥有 CAP_KILL。RCU 保证读取
 * t->cred 期间对象有效。
 */
static bool kill_ok_by_cred(struct task_struct *t)
{
	const struct cred *cred = current_cred();
	const struct cred *tcred = __task_cred(t);

	return uid_eq(cred->euid, tcred->suid) ||
	       uid_eq(cred->euid, tcred->uid) ||
	       uid_eq(cred->uid, tcred->suid) ||
	       uid_eq(cred->uid, tcred->uid) ||
	       ns_capable(tcred->user_ns, CAP_KILL);
}

/*
 * Bad permissions for sending the signal
 * - the caller must hold the RCU read lock
 */
/*
 * 中文学习注释：校验顺序是信号编号 -> 是否为用户来源 -> audit -> Unix
 * 凭据规则 -> LSM。同线程组无需传统 uid 检查，但仍经过 LSM；SIGCONT
 * 额外允许同 session 的作业控制关系。目标已从 pid 哈希摘除时 sid 为
 * NULL，这里不误报 EPERM，由更外层识别目标消失。
 */
static int check_kill_permission(int sig, struct kernel_siginfo *info,
				 struct task_struct *t)
{
	struct pid *sid;
	int error;

	if (!valid_signal(sig))
		return -EINVAL;

	if (!si_fromuser(info))
		return 0;

	error = audit_signal_info(sig, t); /* Let audit system see the signal */
	/* 权限判定前先把本次发送交给审计子系统观察和裁决。 */
	if (error)
		return error;

	if (!same_thread_group(current, t) &&
	    !kill_ok_by_cred(t)) {
		switch (sig) {
		case SIGCONT:
			sid = task_session(t);
			/*
			 * We don't return the error if sid == NULL. The
			 * task was unhashed, the caller must notice this.
			 */
			/*
			 * sid 为 NULL 表示目标已从哈希摘除；这里不返回权限
			 * 错误，外层必须把它识别为目标消失，而不是权限不足。
			 */
			if (!sid || sid == task_session(current))
				break;
			fallthrough;
		default:
			return -EPERM;
		}
	}

	return security_task_kill(t, info, sig, NULL);
}

/**
 * ptrace_trap_notify - schedule trap to notify ptracer
 * @t: tracee wanting to notify tracer
 *
 * This function schedules sticky ptrace trap which is cleared on the next
 * TRAP_STOP to notify ptracer of an event.  @t must have been seized by
 * ptracer.
 *
 * If @t is running, STOP trap will be taken.  If trapped for STOP and
 * ptracer is listening for events, tracee is woken up so that it can
 * re-trap for the new event.  If trapped otherwise, STOP trap will be
 * eventually taken without returning to userland after the existing traps
 * are finished by PTRACE_CONT.
 *
 * CONTEXT:
 * Must be called with @task->sighand->siglock held.
 */
/*
 * 中文学习注释：seize 模式不用传统 SIGSTOP，而把 sticky TRAP_NOTIFY
 * 合入 jobctl。正在 PTRACE_LISTEN 的 tracee 需要唤醒后重新陷入；其他
 * tracee 会在现有 trap 完成后、返回用户态前看到这个新 trap。
 */
static void ptrace_trap_notify(struct task_struct *t)
{
	WARN_ON_ONCE(!(t->ptrace & PT_SEIZED));
	lockdep_assert_held(&t->sighand->siglock);

	task_set_jobctl_pending(t, JOBCTL_TRAP_NOTIFY);
	ptrace_signal_wake_up(t, t->jobctl & JOBCTL_LISTENING);
}

/*
 * Handle magic process-wide effects of stop/continue signals. Unlike
 * the signal actions, these happen immediately at signal-generation
 * time regardless of blocking, ignoring, or handling.  This does the
 * actual continuing for SIGCONT, but not the actual stopping for stop
 * signals. The process stop is done as a signal action for SIG_DFL.
 *
 * Returns true if the signal should be actually delivered, otherwise
 * it should be dropped.
 */
/*
 * 中文学习注释：
 * - 这是“信号生成时”的线程组状态机，不是最终 disposition 执行点。
 * - stop 信号立即清除所有 SIGCONT；SIGCONT 反向清除所有 stop pending、
 *   解除 JOBCTL_STOP_PENDING 并唤醒各线程，所以即使 SIGCONT 被阻塞或
 *   忽略，其继续运行副作用也必须立刻发生。
 * - 若打断了一轮 group-stop，记录 CLD_STOPPED/CLD_CONTINUED 延迟通知，
 *   并清 group_stop_count，避免旧参与者继续消费。
 * - 已在 group exit 的进程只接受能终止 core dump 的 SIGKILL。
 * 返回值只回答该信号是否还需入队；所有操作均在 siglock 下。
 */
static bool prepare_signal(int sig, struct task_struct *p, bool force)
{
	struct signal_struct *signal = p->signal;
	struct task_struct *t;
	sigset_t flush;

	if (signal->flags & SIGNAL_GROUP_EXIT) {
		if (signal->core_state)
			return sig == SIGKILL;
		/*
		 * The process is in the middle of dying, drop the signal.
		 */
		/* 线程组已进入普通退出且非 core 阶段，新增信号直接丢弃。 */
		return false;
	} else if (sig_kernel_stop(sig)) {
		/*
		 * This is a stop signal.  Remove SIGCONT from all queues.
		 */
		/* 生成 stop 信号时立即清除全部 SIGCONT，维持二者互斥状态。 */
		siginitset(&flush, sigmask(SIGCONT));
		flush_sigqueue_mask(p, &flush, &signal->shared_pending);
		for_each_thread(p, t)
			flush_sigqueue_mask(p, &flush, &t->pending);
	} else if (sig == SIGCONT) {
		unsigned int why;
		/*
		 * Remove all stop signals from all queues, wake all threads.
		 */
		/* SIGCONT 反向清除全部 stop pending，并立即唤醒线程组成员。 */
		siginitset(&flush, SIG_KERNEL_STOP_MASK);
		flush_sigqueue_mask(p, &flush, &signal->shared_pending);
		for_each_thread(p, t) {
			flush_sigqueue_mask(p, &flush, &t->pending);
			task_clear_jobctl_pending(t, JOBCTL_STOP_PENDING);
			if (likely(!(t->ptrace & PT_SEIZED))) {
	/* stop/continue 在真正排队前立即清除相反 pending，并更新组停止/继续状态。 */
				t->jobctl &= ~JOBCTL_STOPPED;
				wake_up_state(t, __TASK_STOPPED);
			} else
				ptrace_trap_notify(t);
		}

		/*
		 * Notify the parent with CLD_CONTINUED if we were stopped.
		 *
		 * If we were in the middle of a group stop, we pretend it
		 * was already finished, and then continued. Since SIGCHLD
		 * doesn't queue we report only CLD_STOPPED, as if the next
		 * CLD_CONTINUED was dropped.
		 */
		/*
		 * SIGCHLD 非实时、同号会合并；若 stop 尚未完成，只能
		 * 对外呈现“先完成停止、随后继续中的后一条可能合并丢失”的语义。
		 */
		why = 0;
		if (signal->flags & SIGNAL_STOP_STOPPED)
			why |= SIGNAL_CLD_CONTINUED;
		else if (signal->group_stop_count)
			why |= SIGNAL_CLD_STOPPED;

		if (why) {
			/*
			 * The first thread which returns from do_signal_stop()
			 * will take ->siglock, notice SIGNAL_CLD_MASK, and
			 * notify its parent. See get_signal().
			 */
			/*
			 * 首个从 do_signal_stop() 返回并重新取得 siglock 的
			 * 线程会在 get_signal() 中消费 SIGNAL_CLD_MASK，统一通知父进程。
			 */
			signal_set_stop_flags(signal, why | SIGNAL_STOP_CONTINUED);
			signal->group_stop_count = 0;
			signal->group_exit_code = 0;
		}
	}

	return !sig_ignored(p, sig, force);
}

/*
 * Test if P wants to take SIG.  After we've checked all threads with this,
 * it's equivalent to finding no threads not blocking SIG.  Any threads not
 * blocking SIG were ruled out because they are not running and already
 * have pending signals.  Such threads will dequeue from the shared queue
 * as soon as they're available, so putting the signal on the shared queue
 * will be equivalent to sending it to one such thread.
 */
/*
 * 中文学习注释：用于线程组共享信号的接收者选择。屏蔽/退出/停止线程不宜
 * 唤醒；SIGKILL 无条件合格；正在 CPU 上运行或尚无 pending 的线程最可能
 * 尽快消费。返回 false 不表示信号丢弃，它仍留在 shared_pending。
 */
static inline bool wants_signal(int sig, struct task_struct *p)
{
	if (sigismember(&p->blocked, sig))
		return false;

	if (p->flags & PF_EXITING)
		return false;

	if (sig == SIGKILL)
	/* 接收者选择依次排除 blocked、退出和不合适线程，再用 pending/当前状态判断可唤醒性。 */
		return true;

	if (task_is_stopped_or_traced(p))
		return false;

	return task_curr(p) || !task_sigpending(p);
}

static void complete_signal(int sig, struct task_struct *p, enum pid_type type)
{
	struct signal_struct *signal = p->signal;
	struct task_struct *t;

	/*
	 * Now find a thread we can wake up to take the signal off the queue.
	 *
	 * Try the suggested task first (may or may not be the main thread).
	 */
	/*
	 * 选择顺序：发送时给出的线程 -> 从 curr_target 轮转扫描线程组。
	 * curr_target 提供近似公平性，避免共享信号总唤醒主线程。
	 */
	if (wants_signal(sig, p))
		t = p;
	else if ((type == PIDTYPE_PID) || thread_group_empty(p))
		/*
		 * There is just one thread and it does not need to be woken.
		 * It will dequeue unblocked signals before it runs again.
		 */
		/*
		 * 线程定向或单线程组没有替代接收者；任务下次运行前会
		 * 自行检查未屏蔽 pending，因此无需额外唤醒。
		 */
		return;
	else {
		/*
		 * Otherwise try to find a suitable thread.
		 */
		/* 共享信号从 curr_target 起轮转寻找能够立即消费的线程。 */
		t = signal->curr_target;
		while (!wants_signal(sig, t)) {
			t = next_thread(t);
			if (t == signal->curr_target)
				/*
				 * No thread needs to be woken.
				 * Any eligible threads will see
				 * the signal in the queue soon.
				 */
				/*
				 * 没有线程值得额外唤醒时，信号仍留在共享队列；
				 * 后续任何合格线程恢复运行都会看到它，并非丢失。
				 */
				return;
		}
		signal->curr_target = t;
	}

	/*
	 * Found a killable thread.  If the signal will be fatal,
	 * then start taking the whole group down immediately.
	 */
	/*
	 * 默认致命且未被 real_blocked/ptrace 改写时，非 core 信号直接进入
	 * SIGNAL_GROUP_EXIT，并向每个线程私有队列注入 SIGKILL。这样无需等
	 * 被选中的慢线程实际出队，整个组便开始收敛退出。core 信号须保留由
	 * 单一线程协调转储的路径，不能在这里提前广播。
	 */
	if (sig_fatal(p, sig) && !sigismember(&t->real_blocked, sig) &&
	    (sig == SIGKILL || !p->ptrace)) {
		/*
		 * This signal will be fatal to the whole group.
		 */
		/* 默认致命动作作用于整个线程组，而非仅当前候选线程。 */
		if (!sig_kernel_coredump(sig)) {
			/*
			 * Start a group exit and wake everybody up.
			 * This way we don't have other threads
			 * running and doing things after a slower
			 * thread has the fatal signal pending.
			 */
			/*
			 * 立即发布 group-exit 并唤醒所有成员，防止被选中的
			 * 慢线程尚未处理致命信号时，其他线程继续修改进程共享状态。
			 */
			signal->flags = SIGNAL_GROUP_EXIT;
			signal->group_exit_code = sig;
			signal->group_stop_count = 0;
			__for_each_thread(signal, t) {
				task_clear_jobctl_pending(t, JOBCTL_PENDING_MASK);
				sigaddset(&t->pending.signal, SIGKILL);
	/* 线程定向或共享信号选择接收者后，致命默认动作还需建立 group-exit 并唤醒全组。 */
				signal_wake_up(t, 1);
			}
			return;
		}
	}

	/*
	 * The signal is already in the shared-pending queue.
	 * Tell the chosen thread to wake up and dequeue it.
	 */
	/* 共享队列已经完成发布；此处只唤醒选中的消费者去摘取。 */
	signal_wake_up(t, sig == SIGKILL);
	return;
}

/* legacy_queue()：判断普通信号是否已有实例，供生成路径实施合并语义。 */
static inline bool legacy_queue(struct sigpending *signals, int sig)
{
	/* 非实时信号只保留一个 pending 实例；实时信号则允许逐个排队。 */
	return (sig < SIGRTMIN) && sigismember(&signals->signal, sig);
}

/*
 * __send_signal_locked() - 在 siglock 下完成信号生成的核心事务。
 *
 * 1. prepare_signal() 执行 stop/continue 的即时组副作用并判断是否忽略；
 * 2. 根据 PIDTYPE 选择线程私有或组共享 pending；
 * 3. 普通信号合并，其他信号尽量分配 sigqueue 保存完整 siginfo；
 * 4. 设置位图、通知 signalfd，并将多进程发送同步到 fork 延迟集合；
 * 5. complete_signal() 选择并唤醒消费者，最后发 tracepoint。
 *
 * 分配失败语义：显式排队的实时信号返回 -EAGAIN；kill 风格以及普通信号
 * 仍设置位图保证至少递送一次，只损失 siginfo。@force 可越过 disposition
 * 忽略判断，但不改变队列所有权规则。
 */
static int __send_signal_locked(int sig, struct kernel_siginfo *info,
				struct task_struct *t, enum pid_type type, bool force)
{
	struct sigpending *pending;
	struct sigqueue *q;
	int override_rlimit;
	int ret = 0, result;
	/* sigqueue 分配成功后按 NOINFO、PRIV 或完整 info 三种来源初始化唯一有效布局。 */

	lockdep_assert_held(&t->sighand->siglock);

	result = TRACE_SIGNAL_IGNORED;
	if (!prepare_signal(sig, t, force))
		goto ret;

	pending = (type != PIDTYPE_PID) ? &t->signal->shared_pending : &t->pending;
	/*
	 * Short-circuit ignored signals and support queuing
	 * exactly one non-rt signal, so that we can get more
	 * detailed information about the cause of the signal.
	 */
	/* 普通信号已有 pending 时合并，保留最早节点携带的原因。 */
	result = TRACE_SIGNAL_ALREADY_PENDING;
	if (legacy_queue(pending, sig))
		goto ret;

	result = TRACE_SIGNAL_DELIVERED;
	/*
	 * Skip useless siginfo allocation for SIGKILL and kernel threads.
	 */
	/*
	 * SIGKILL 不向 handler 暴露 payload，内核线程也不消费用户
	 * siginfo；直接置位可避免原子上下文中的无意义分配。
	 */
	if ((sig == SIGKILL) || (t->flags & PF_KTHREAD))
		goto out_set;

	/*
	 * Real-time signals must be queued if sent by sigqueue, or
	 * some other real-time mechanism.  It is implementation
	 * defined whether kill() does so.  We attempt to do so, on
	 * the principle of least surprise, but since kill is not
	 * allowed to fail with EAGAIN when low on memory we just
	 * make sure at least one signal gets delivered and don't
	 * pass on the info struct.
	 */
	/*
	 * 普通信号及特殊内核来源可越过用户配额，目标是保证控制类
	 * 信号可达；显式用户实时排队则必须遵守 POSIX 的 EAGAIN 契约。
	 */
	if (sig < SIGRTMIN)
		override_rlimit = (is_si_special(info) || info->si_code >= 0);
	else
		override_rlimit = 0;

	q = sigqueue_alloc(sig, t, GFP_ATOMIC, override_rlimit);

	if (q) {
		/* 节点自此由 pending->list 拥有；三种 info 约定在这里实体化。 */
		list_add_tail(&q->list, &pending->list);
		switch ((unsigned long) info) {
		case (unsigned long) SEND_SIG_NOINFO:
			clear_siginfo(&q->info);
			q->info.si_signo = sig;
			q->info.si_errno = 0;
			q->info.si_code = SI_USER;
			q->info.si_pid = task_tgid_nr_ns(current,
	/* sigqueue 分配成功后按 NOINFO、PRIV 或完整 info 三种来源初始化唯一有效布局。 */
							task_active_pid_ns(t));
			rcu_read_lock();
			q->info.si_uid =
				from_kuid_munged(task_cred_xxx(t, user_ns),
						 current_uid());
			rcu_read_unlock();
			break;
		/* PRIV 来源合成 SI_KERNEL，不暴露发送者 pid/uid。 */
		case (unsigned long) SEND_SIG_PRIV:
			clear_siginfo(&q->info);
			q->info.si_signo = sig;
			q->info.si_errno = 0;
			q->info.si_code = SI_KERNEL;
			q->info.si_pid = 0;
			q->info.si_uid = 0;
			break;
		/* 真实 kernel_siginfo 则完整复制其已选 tagged layout。 */
		default:
			copy_siginfo(&q->info, info);
			break;
		}
	} else if (!is_si_special(info) &&
		   sig >= SIGRTMIN && info->si_code != SI_USER) {
		/*
		 * Queue overflow, abort.  We may abort if the
		 * signal was rt and sent by user using something
		 * other than kill().
		 */
		/* 显式实时排队要求每个实例及其 payload 都不可静默丢失。 */
		result = TRACE_SIGNAL_OVERFLOW_FAIL;
		ret = -EAGAIN;
		goto ret;
	} else {
		/*
		 * This is a silent loss of information.  We still
		 * send the signal, but the *info bits are lost.
		 */
		/* 保留位图递送保证，但接收方只能得到合成的默认 siginfo。 */
		result = TRACE_SIGNAL_LOSE_INFO;
	}

out_set:
	/* 先通知观察者、再置 pending 位，随后选择接收线程并发布唤醒。 */
	signalfd_notify(t, sig);
	sigaddset(&pending->signal, sig);

	/* Let multiprocess signals appear after on-going forks */
	/*
	 * 向 PGID 等多进程目标发送期间并发 fork 的子进程，必须像在
	 * 发送开始时已存在一样看到信号；multiprocess 延迟集合封住这一窗口。
	 */
	if (type > PIDTYPE_TGID) {
		struct multiprocess_signals *delayed;
		hlist_for_each_entry(delayed, &t->signal->multiprocess, node) {
			sigset_t *signal = &delayed->signal;
			/* Can't queue both a stop and a continue signal */
			/* fork 延迟集合也必须维持 stop 与 SIGCONT 的互斥关系。 */
			if (sig == SIGCONT)
				sigdelsetmask(signal, SIG_KERNEL_STOP_MASK);
			else if (sig_kernel_stop(sig))
				sigdelset(signal, SIGCONT);
			sigaddset(signal, sig);
		}
	}

	/* 位图和 fork 延迟集合都发布完毕后，才选择消费者并记录生成结果。 */
	complete_signal(sig, t, type);
ret:
	trace_signal_generate(sig, info, t, type != PIDTYPE_PID, result);
	return ret;
}

static inline bool has_si_pid_and_uid(struct kernel_siginfo *info)
{
	/*
	 * siginfo 是 tagged union；只有 KILL/CHLD/RT 布局中的 pid/uid 字段可
	 * 做 namespace 翻译，其他布局相同偏移可能代表地址、fd 或 syscall。
	 */
	bool ret = false;
	switch (siginfo_layout(info->si_signo, info->si_code)) {
	case SIL_KILL:
	case SIL_CHLD:
	case SIL_RT:
		ret = true;
	/* 仅 KILL、CHLD、RT tagged layout 的重叠槽确实表示 pid/uid，可做 namespace 翻译。 */
		break;
	case SIL_TIMER:
	case SIL_POLL:
	case SIL_FAULT:
	case SIL_FAULT_TRAPNO:
		/* fault/POLL/SYS 的重叠槽不是身份字段，禁止 namespace 翻译。 */
	case SIL_FAULT_MCEERR:
	case SIL_FAULT_BNDERR:
	case SIL_FAULT_PKUERR:
	case SIL_FAULT_PERF_EVENT:
	case SIL_SYS:
		ret = false;
		break;
	}
	return ret;
}

/*
 * 契约补充：调用者必须持 @t->sighand->siglock；@info/@t 为借用输入，
 * @type 决定作用域。函数不睡眠；返回排队结果，并可能唤醒目标或启动组退出。
 */
int send_signal_locked(int sig, struct kernel_siginfo *info,
		       struct task_struct *t, enum pid_type type)
{
	/*
	 * 这一层处理 namespace 边界：祖先 pid namespace 发来的 SIGKILL/
	 * SIGSTOP 必须能穿透 namespace init 的 SIGNAL_UNKILLABLE；目标看不到
	 * 发送者 pid 时将 si_pid 置 0，并把 uid 转换到目标 user namespace。
	 * 完成元数据规范化后进入统一的 __send_signal_locked()。
	 */
	/* Should SIGKILL or SIGSTOP be received by a pid namespace init? */
	/* @force 决定 PID namespace init 是否必须接收不可忽略控制信号。 */
	bool force = false;

	if (info == SEND_SIG_NOINFO) {
		/* Force if sent from an ancestor pid namespace */
		/* 祖先 namespace 的发送者在目标 namespace 中不可见，因此强制递送。 */
		force = !task_pid_nr_ns(current, task_active_pid_ns(t));
	} else if (info == SEND_SIG_PRIV) {
		/* Don't ignore kernel generated signals */
		/* 内核特权来源不受用户 disposition 的忽略规则阻断。 */
		force = true;
	} else if (has_si_pid_and_uid(info)) {
		/* SIGKILL and SIGSTOP is special or has ids */
		/* 只有含 pid/uid 的布局才可执行 user namespace 身份转换。 */
		struct user_namespace *t_user_ns;

		rcu_read_lock();
		t_user_ns = task_cred_xxx(t, user_ns);
		if (current_user_ns() != t_user_ns) {
			kuid_t uid = make_kuid(current_user_ns(), info->si_uid);
			info->si_uid = from_kuid_munged(t_user_ns, uid);
		}
		rcu_read_unlock();

		/* A kernel generated signal? */
		/* SI_KERNEL 表明来源可信，允许越过 namespace init 的保护。 */
		force = (info->si_code == SI_KERNEL);

		/* From an ancestor pid namespace? */
		/* 目标看不到祖先发送者时隐藏 si_pid，并强制控制信号可达。 */
		if (!task_pid_nr_ns(current, task_active_pid_ns(t))) {
			info->si_pid = 0;
			force = true;
		}
	}
	return __send_signal_locked(sig, info, t, type, force);
}

/* print_fatal_signal()：按调试配置输出 current 的可执行文件与寄存器现场。 */
static void print_fatal_signal(int signr)
{
	/*
	 * 调试开关开启时打印致命信号现场：对 exe_file 取得临时引用，架构允许
	 * 时安全探测用户指令字节，最后在禁止抢占期间打印寄存器。
	 */
	struct pt_regs *regs = task_pt_regs(current);
	struct file *exe_file;

	exe_file = get_task_exe_file(current);
	/* exe_file 引用只覆盖名称打印，随后立即 fput。 */
	if (exe_file) {
		pr_info("%pD: %s: potentially unexpected fatal signal %d.\n",
			exe_file, current->comm, signr);
		fput(exe_file);
	} else {
		pr_info("%s: potentially unexpected fatal signal %d.\n",
			current->comm, signr);
	/* 诊断输出按寄存器、指令字节和可选 VMA 信息分段，任何读取失败只缩减日志。 */
	}

#if defined(__i386__) && !defined(__arch_um__)
	pr_info("code at %08lx: ", regs->ip);
	{
		int i;
		for (i = 0; i < 16; i++) {
			/* 指令探测逐字节容错，首个不可访问地址只终止代码转储。 */
			unsigned char insn;

			if (get_user(insn, (unsigned char *)(regs->ip + i)))
				break;
			pr_cont("%02x ", insn);
		}
	}
	pr_cont("\n");
#endif
	/* show_regs 期间短暂禁止抢占，保持 current 的寄存器现场稳定。 */
	preempt_disable();
	show_regs(regs);
	preempt_enable();
}

/* setup_print_fatal_signals()：解析早期启动参数中的致命信号打印开关。 */
/*
 * 契约补充：早期启动阶段借用并解析可写字符串 @str；无并发与睡眠需求。
 * 返回 1 表示参数已消费；副作用是更新启动期 print_fatal_signals。
 */
static int __init setup_print_fatal_signals(char *str)
{
	/* 早期启动参数解析器，只更新诊断开关，不改变信号语义。 */
	get_option (&str, &print_fatal_signals);

	return 1;
}

__setup("print-fatal-signals=", setup_print_fatal_signals);

/*
 * 契约补充：@sig 是编号，@info/@p 为借用输入，@type 决定线程或组作用域。
 * 本函数获取目标 siglock；成功返回 0，目标退出返回 -ESRCH，输入所有权不变。
 */
int do_send_sig_info(int sig, struct kernel_siginfo *info, struct task_struct *p,
			enum pid_type type)
{
	/*
	 * lock_task_sighand() 同时解决目标退出和 sighand 替换竞态；锁不到即
	 * 视为目标已消失。成功时整个排队事务均在目标 siglock 内完成。
	 */
	unsigned long flags;
	int ret = -ESRCH;

	if (lock_task_sighand(p, &flags)) {
		ret = send_signal_locked(sig, info, p, type);
		unlock_task_sighand(p, &flags);
	}

	return ret;
}

enum sig_handler {
	HANDLER_CURRENT, /* If reachable use the current handler */
	/* 若当前 disposition 可达，则保留并使用现有用户处理器。 */
	HANDLER_SIG_DFL, /* Always use SIG_DFL handler semantics */
	/* 无条件改用默认动作，常用于递归故障后的致命收敛。 */
	HANDLER_EXIT,	 /* Only visible as the process exit code */
	/* 信号只作为不可改写的退出码对外可见，不再进入用户处理器。 */
};

/*
 * Force a signal that the process can't ignore: if necessary
 * we unblock the signal and change any SIG_IGN to SIG_DFL.
 *
 * Note: If we unblock the signal, we always reset it to SIG_DFL,
 * since we do not want to have a signal handler that was blocked
 * be invoked when user space had explicitly blocked it.
 *
 * We don't want to have recursive SIGSEGV's etc, for example,
 * that is why we also clear SIGNAL_UNKILLABLE.
 */
/*
 * 中文学习注释：
 * - fault/内核强制信号必须可达：若被阻塞、忽略或调用者要求默认动作，
 *   在 siglock 内改为 SIG_DFL 并解除阻塞。
 * - HANDLER_EXIT 还置 SA_IMMUTABLE，只把信号作为确定的退出原因；普通
 *   用户 sigaction 不能在窗口内重新改写。
 * - ptraced init 默认仍保持不可杀，避免调试行为意外改变 init 生存性；
 *   HANDLER_EXIT 是明确例外。
 * - 若同号已 pending 导致发送路径未再次置 TIF，末尾显式唤醒补足发布。
 */
static int
force_sig_info_to_task(struct kernel_siginfo *info, struct task_struct *t,
	enum sig_handler handler)
{
	unsigned long int flags;
	int ret, blocked, ignored;
	struct k_sigaction *action;
	int sig = info->si_signo;

	spin_lock_irqsave(&t->sighand->siglock, flags);
	action = &t->sighand->action[sig-1];
	ignored = action->sa.sa_handler == SIG_IGN;
	/* blocked 与 ignored 决定是否解除屏蔽、是否恢复默认 disposition。 */
	blocked = sigismember(&t->blocked, sig);
	if (blocked || ignored || (handler != HANDLER_CURRENT)) {
		action->sa.sa_handler = SIG_DFL;
		if (handler == HANDLER_EXIT)
			action->sa.sa_flags |= SA_IMMUTABLE;
		if (blocked)
			sigdelset(&t->blocked, sig);
	}
	/*
	 * Don't clear SIGNAL_UNKILLABLE for traced tasks, users won't expect
	 * debugging to leave init killable. But HANDLER_EXIT is always fatal.
	 */
	/*
	 * 普通调试不应让 init 因 ptrace 变成可杀；只有 HANDLER_EXIT
	 * 明确要求不可恢复退出，才允许清除 SIGNAL_UNKILLABLE。
	 */
	if (action->sa.sa_handler == SIG_DFL &&
	    (!t->ptrace || (handler == HANDLER_EXIT)))
		t->signal->flags &= ~SIGNAL_UNKILLABLE;
	ret = send_signal_locked(sig, info, t, PIDTYPE_PID);
	/* This can happen if the signal was already pending and blocked */
	/* 同号信号可能早已 pending 且被阻塞，发送合并后未重新置唤醒位。 */
	if (!task_sigpending(t))
		signal_wake_up(t, 0);
	spin_unlock_irqrestore(&t->sighand->siglock, flags);

	return ret;
}

/* force_sig_info()：强制 current 接收 siginfo 描述的信号。 */
int force_sig_info(struct kernel_siginfo *info)
{
	/* 强制递送给 current，但保留当前 handler（除非其忽略/阻塞）。 */
	return force_sig_info_to_task(info, current, HANDLER_CURRENT);
}

/*
 * Nuke all other threads in the group.
 */
/*
 * 中文学习注释：线程组致命退出/exec 收敛时清除所有 jobctl 停止状态，并
 * 向存活的其他线程私有 pending 直接加入 SIGKILL 后强制唤醒。返回需要
 * 等待退出的线程数；调用者已持线程组信号状态所需锁。
 */
int zap_other_threads(struct task_struct *p)
{
	struct task_struct *t;
	int count = 0;

	p->signal->group_stop_count = 0;
	task_clear_jobctl_pending(p, JOBCTL_PENDING_MASK);

	for_other_threads(p, t) {
		task_clear_jobctl_pending(t, JOBCTL_PENDING_MASK);
		count++;

		/* Don't bother with already dead threads */
		/* 已进入 exit_state 的线程无需再次注入 SIGKILL 或唤醒。 */
		if (t->exit_state)
			continue;
		sigaddset(&t->pending.signal, SIGKILL);
		signal_wake_up(t, 1);
	}

	return count;
}

struct sighand_struct *lock_task_sighand(struct task_struct *tsk,
					 unsigned long *flags)
{
	/*
	 * RCU 只保证 slab 对象内存暂不回收，不能保证 tsk->sighand 指针未被
	 * de_thread()/exit 替换。因此：RCU 取指针 -> 锁该对象 -> 回读指针；
	 * 不一致则解锁重试。SLAB_TYPESAFE_BY_RCU 与构造器保证复用期间 siglock
	 * 本身不会被重新初始化。看到 NULL 后的 acquire 屏障与退出路径 release
	 * 配对，使此前任务状态修改可见。
	 */
	struct sighand_struct *sighand;

	rcu_read_lock();
	for (;;) {
		sighand = rcu_dereference(tsk->sighand);
		if (unlikely(sighand == NULL)) {
			/*
			 * Pairs with the smp_store_release() in
			 * __exit_signal().  It ensures that all state
			 * modifications to the task preceeding the store are
			 * visible to the callers of lock_task_sighand().
			 */
			/*
			 * 该 acquire 与 __exit_signal() 把 sighand 置 NULL 的
			 * release 配对；看到 NULL 后，调用者也能看到退出路径此前的全部
			 * 状态写入，而不会观察到半完成退出。
			 */
			smp_acquire__after_ctrl_dep();
			break;
		}

		/*
		 * This sighand can be already freed and even reused, but
		 * we rely on SLAB_TYPESAFE_BY_RCU and sighand_ctor() which
		 * initializes ->siglock: this slab can't go away, it has
		 * the same object type, ->siglock can't be reinitialized.
		 *
		 * We need to ensure that tsk->sighand is still the same
		 * after we take the lock, we can race with de_thread() or
		 * __exit_signal(). In the latter case the next iteration
		 * must see ->sighand == NULL.
		 */
		/*
		 * SLAB_TYPESAFE_BY_RCU 只保证槽位仍是同类型对象，
		 * sighand_ctor() 又保证复用时不会重置正在获取的 siglock。锁住后仍
		 * 必须回读 tsk->sighand：de_thread() 可能换对象，退出可能置 NULL；
		 * 不一致就解锁重试，避免在已与目标脱钩的旧 sighand 上操作。
		 */
		spin_lock_irqsave(&sighand->siglock, *flags);
		if (likely(sighand == rcu_access_pointer(tsk->sighand)))
			break;
		spin_unlock_irqrestore(&sighand->siglock, *flags);
	}
	rcu_read_unlock();

	return sighand;
}

#ifdef CONFIG_LOCKDEP
/* lockdep_assert_task_sighand_held()：验证调用者持有目标当前 sighand 锁。 */
void lockdep_assert_task_sighand_held(struct task_struct *task)
{
	/* 仅供锁依赖验证；RCU 稳定 sighand 指针，再断言其 siglock 已持有。 */
	struct sighand_struct *sighand;

	rcu_read_lock();
	sighand = rcu_dereference(task->sighand);
	if (sighand)
		lockdep_assert_held(&sighand->siglock);
	else
		WARN_ON_ONCE(1);
	rcu_read_unlock();
}
#endif

/*
 * send signal info to all the members of a thread group or to the
 * individual thread if type == PIDTYPE_PID.
 */
/*
 * 中文学习注释：先在 RCU 下完成凭据/LSM 校验；sig==0 只做存在性与权限
 * 探测，不实际排队。真实发送再通过 do_send_sig_info() 获取目标 siglock。
 */
int group_send_sig_info(int sig, struct kernel_siginfo *info,
			struct task_struct *p, enum pid_type type)
{
	int ret;

	rcu_read_lock();
	ret = check_kill_permission(sig, info, p);
	/* 组信号在 RCU/siglock 下检查权限与目标存活，再调用 locked 发送事务。 */
	rcu_read_unlock();

	if (!ret && sig)
		ret = do_send_sig_info(sig, info, p, type);

	return ret;
}

/*
 * __kill_pgrp_info() sends a signal to a process group: this is what the tty
 * control characters do (^C, ^Z etc)
 * - the caller must hold at least a readlock on tasklist_lock
 */
/*
 * 中文学习注释：遍历 PGID 下每个线程组并分别做权限检查。只要一次成功，
 * 最终结果固定为 0；全部失败时返回最后一个错误，空组返回 -ESRCH。
 */
int __kill_pgrp_info(int sig, struct kernel_siginfo *info, struct pid *pgrp)
{
	struct task_struct *p = NULL;
	int ret = -ESRCH;

	do_each_pid_task(pgrp, PIDTYPE_PGID, p) {
		int err = group_send_sig_info(sig, info, p, PIDTYPE_PGID);
		/*
		 * If group_send_sig_info() succeeds at least once ret
		 * becomes 0 and after that the code below has no effect.
		 * Otherwise we return the last err or -ESRCH if this
		 * process group is empty.
		 */
		/*
		 * 进程组发送采用“至少一个成功即整体成功”；若没有成功，
		 * 返回最后一个成员错误，组内没有任务时保留初值 -ESRCH。
		 */
		if (ret)
			ret = err;
	} while_each_pid_task(pgrp, PIDTYPE_PGID, p);

	return ret;
}

static int kill_pid_info_type(int sig, struct kernel_siginfo *info,
				struct pid *pid, enum pid_type type)
{
	/*
	 * pid 对象稳定但 leader 可因 de_thread() 换人。若发送返回 -ESRCH，
	 * 循环重查：真正死亡会得到 NULL；仅 leader 迁移则找到新 leader。
	 */
	int error = -ESRCH;
	struct task_struct *p;

	for (;;) {
		rcu_read_lock();
		p = pid_task(pid, PIDTYPE_PID);
		if (p)
			error = group_send_sig_info(sig, info, p, type);
		rcu_read_unlock();
		if (likely(!p || error != -ESRCH))
			return error;
		/*
		 * The task was unhashed in between, try again.  If it
		 * is dead, pid_task() will return NULL, if we race with
		 * de_thread() it will find the new leader.
		 */
		/*
		 * 发送与 leader 换位竞态时重查同一 struct pid；真正死亡
		 * 会得到 NULL，de_thread() 则让下一轮找到新 leader。
		 */
	}
}

int kill_pid_info(int sig, struct kernel_siginfo *info, struct pid *pid)
{
	/* struct pid 定位线程组，按 TGID 语义发送。 */
	return kill_pid_info_type(sig, info, pid, PIDTYPE_TGID);
}

static int kill_proc_info(int sig, struct kernel_siginfo *info, pid_t pid)
{
	/* 数字 pid 在 current pid namespace 中解析，并由 RCU 稳定映射。 */
	int error;
	rcu_read_lock();
	error = kill_pid_info(sig, info, find_vpid(pid));
	rcu_read_unlock();
	return error;
}

static inline bool kill_as_cred_perm(const struct cred *cred,
				     struct task_struct *target)
{
	/* USB async 保存的旧凭据只做 uid 四向匹配；LSM 检查由调用者另做。 */
	const struct cred *pcred = __task_cred(target);

	return uid_eq(cred->euid, pcred->suid) ||
	       uid_eq(cred->euid, pcred->uid) ||
	       uid_eq(cred->uid, pcred->suid) ||
	       uid_eq(cred->uid, pcred->uid);
}

/*
 * The usb asyncio usage of siginfo is wrong.  The glibc support
 * for asyncio which uses SI_ASYNCIO assumes the layout is SIL_RT.
 * AKA after the generic fields:
 *	kernel_pid_t	si_pid;
 *	kernel_uid32_t	si_uid;
 *	sigval_t	si_value;
 *
 * Unfortunately when usb generates SI_ASYNCIO it assumes the layout
 * after the generic fields is:
 *	void __user 	*si_addr;
 *
 * This is a practical problem when there is a 64bit big endian kernel
 * and a 32bit userspace.  As the 32bit address will encoded in the low
 * 32bits of the pointer.  Those low 32bits will be stored at higher
 * address than appear in a 32 bit pointer.  So userspace will not
 * see the address it was expecting for it's completions.
 *
 * There is nothing in the encoding that can allow
 * copy_siginfo_to_user32 to detect this confusion of formats, so
 * handle this by requiring the caller of kill_pid_usb_asyncio to
 * notice when this situration takes place and to store the 32bit
 * pointer in sival_int, instead of sival_addr of the sigval_t addr
 * parameter.
 */
/*
 * 中文学习注释：这是历史 ABI 兼容路径。USB 把 SI_ASYNCIO 的 union 当地址
 * 布局，而 libc 按实时信号布局解释；尤其 64 位大端内核/32 位用户态会
 * 产生半字节序错位。内核无法从编码自省布局，只能要求调用者预先把 32 位
 * 地址放到 sival_int。发送时使用提交异步请求时保存的 @cred 做权限判断，
 * 再经过 LSM，并在目标 siglock 下按线程组信号入队。
 */
int kill_pid_usb_asyncio(int sig, int errno, sigval_t addr,
			 struct pid *pid, const struct cred *cred)
{
	struct kernel_siginfo info;
	struct task_struct *p;
	unsigned long flags;
	/* 先构造历史 SI_ASYNCIO payload，再在 RCU 下依次完成目标查找、凭据与 LSM 检查。 */
	int ret = -EINVAL;

	if (!valid_signal(sig))
		return ret;

	clear_siginfo(&info);
	info.si_signo = sig;
	/* 历史 USB ABI 把 sigval 原样塞入 si_pid 起始 union 槽。 */
	info.si_errno = errno;
	info.si_code = SI_ASYNCIO;
	*((sigval_t *)&info.si_pid) = addr;

	rcu_read_lock();
	p = pid_task(pid, PIDTYPE_PID);
	if (!p) {
		ret = -ESRCH;
		goto out_unlock;
	}
	/* 目标存在后按提交请求时保存的 cred 校验，再交 LSM 复核。 */
	if (!kill_as_cred_perm(cred, p)) {
		ret = -EPERM;
		goto out_unlock;
	}
	ret = security_task_kill(p, &info, sig, cred);
	if (ret)
		goto out_unlock;

	if (sig) {
		if (lock_task_sighand(p, &flags)) {
			ret = __send_signal_locked(sig, &info, p, PIDTYPE_TGID, false);
	/* 先构造历史 SI_ASYNCIO payload，再在 RCU 下依次完成目标查找、凭据与 LSM 检查。 */
			unlock_task_sighand(p, &flags);
		} else
			ret = -ESRCH;
	}
out_unlock:
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(kill_pid_usb_asyncio);

/*
 * kill_something_info() interprets pid in interesting ways just like kill(2).
 *
 * POSIX specifies that kill(-1,sig) is unspecified, but what we have
 * is probably wrong.  Should make it like BSD or SYSV.
 */
/*
 * 该函数按 kill(2) 解释 pid 的正数、0、负进程组和 -1 广播语义。
 * POSIX 对 kill(-1, sig) 未规定统一行为；上游注释明确承认当前 Linux 语义
 * 可能并非理想的 BSD 或 System V 选择，因此不能把它描述成跨系统保证。
 */

/*
 * 中文学习注释：实现 kill(2) 的 pid 多态：正数=指定进程，0=当前进程组，
 * 小于 -1=指定进程组，-1=广播。广播跳过 namespace init 与本线程组；
 * tasklist_lock 稳定全局遍历。INT_MIN 不能安全取负，直接按不存在处理。
 */
static int kill_something_info(int sig, struct kernel_siginfo *info, pid_t pid)
{
	int ret;

	if (pid > 0)
		return kill_proc_info(sig, info, pid);

	/* -INT_MIN is undefined.  Exclude this case to avoid a UBSAN warning */
	/* C 中对 INT_MIN 取负溢出；提前拒绝既避免 UB，也避免 UBSAN 报警。 */
	if (pid == INT_MIN)
		return -ESRCH;

	read_lock(&tasklist_lock);
	if (pid != -1) {
		ret = __kill_pgrp_info(sig, info,
				pid ? find_vpid(-pid) : task_pgrp(current));
	} else {
		int retval = 0, count = 0;
		struct task_struct * p;

		for_each_process(p) {
			/* 广播逐进程尝试，EPERM 不覆盖其他目标可能成功的汇总结果。 */
			if (task_pid_vnr(p) > 1 &&
					!same_thread_group(p, current)) {
				int err = group_send_sig_info(sig, info, p,
							      PIDTYPE_MAX);
				++count;
				if (err != -EPERM)
					retval = err;
			}
	/* tasklist_lock 内区分进程组与 -1 广播，并汇总存在性、权限和实际发送结果。 */
		}
		ret = count ? retval : -ESRCH;
	}
	read_unlock(&tasklist_lock);

	return ret;
}

/*
 * These are for backward compatibility with the rest of the kernel source.
 */
/* 以下入口保留给旧内核调用者，最终仍汇入统一 siginfo 发送核心。 */

/* 以下 wrapper 把旧内核调用约定归一化到 siginfo 核心发送路径。 */
/*
 * 契约补充：内核线程定向发送入口；@info/@p 均为借用且不转移所有权。
 * 无入口锁要求；返回 0、-EINVAL、-ESRCH 或排队错误。
 */
int send_sig_info(int sig, struct kernel_siginfo *info, struct task_struct *p)
{
	/*
	 * Make sure legacy kernel users don't send in bad values
	 * (normal paths check this in check_kill_permission).
	 */
	/*
	 * 旧内核调用者绕过用户权限入口，必须在此自行验证信号编号；
	 * 普通用户发送路径则由 check_kill_permission() 完成同一校验。
	 */
	if (!valid_signal(sig))
		return -EINVAL;

	return do_send_sig_info(sig, info, p, PIDTYPE_PID);
}
EXPORT_SYMBOL(send_sig_info);

#define __si_special(priv) \
	((priv) ? SEND_SIG_PRIV : SEND_SIG_NOINFO)

int
send_sig(int sig, struct task_struct *p, int priv)
{
	/* @priv 选择 SI_KERNEL 风格还是缺省用户 kill 风格的伪 siginfo。 */
	return send_sig_info(sig, __si_special(priv), p);
}
EXPORT_SYMBOL(send_sig);

void force_sig(int sig)
{
	/* 构造最小 SI_KERNEL 信息，并强制 current 接收。 */
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = 0;
	info.si_code = SI_KERNEL;
	info.si_pid = 0;
	info.si_uid = 0;
	force_sig_info(&info);
}
EXPORT_SYMBOL(force_sig);

/*
 * 契约补充：在 current 进程上下文强制 @sig 使用默认致命动作；无入口锁，
 * 不睡眠。返回无；副作用是在 siglock 下发布不可被现有 handler 阻止的信号。
 */
void force_fatal_sig(int sig)
{
	/* 强制采用 SIG_DFL；用于当前 handler/屏蔽状态不能再被信任的故障。 */
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = 0;
	info.si_code = SI_KERNEL;
	info.si_pid = 0;
	info.si_uid = 0;
	force_sig_info_to_task(&info, current, HANDLER_SIG_DFL);
}

/*
 * 契约补充：把 @sig 固化为 current 的退出原因；无入口锁且不睡眠。
 * 返回无；HANDLER_EXIT 同时使动作不可由用户 sigaction 再改写。
 */
void force_exit_sig(int sig)
{
	/* 进一步使用 HANDLER_EXIT 固化退出语义，用户态不能再安装 handler。 */
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = 0;
	info.si_code = SI_KERNEL;
	info.si_pid = 0;
	info.si_uid = 0;
	force_sig_info_to_task(&info, current, HANDLER_EXIT);
}

/*
 * When things go south during signal handling, we
 * will force a SIGSEGV. And if the signal that caused
 * the problem was already a SIGSEGV, we'll want to
 * make sure we don't even try to deliver the signal..
 */
/*
 * 构造信号帧时再次 SIGSEGV 说明处理路径自身失败；若原信号已经
 * 是 SIGSEGV，则必须改为默认致命动作，防止递归进入同一 handler。
 */
void force_sigsegv(int sig)
{
	if (sig == SIGSEGV)
		force_fatal_sig(SIGSEGV);
	else
		force_sig(SIGSEGV);
}

int force_sig_fault_to_task(int sig, int code, void __user *addr,
			    struct task_struct *t)
{
	/* 为同步 fault 构造带地址和正 si_code 的 siginfo，并强制目标处理。 */
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = 0;
	/* fault trapno wrapper 只组装 kernel_siginfo，实际权限、排队和唤醒交给发送核心。 */
	info.si_code  = code;
	info.si_addr  = addr;
	return force_sig_info_to_task(&info, t, HANDLER_CURRENT);
}

int force_sig_fault(int sig, int code, void __user *addr)
{
	/* current 版本的 fault 强制递送便捷入口。 */
	return force_sig_fault_to_task(sig, code, addr, current);
}

int send_sig_fault(int sig, int code, void __user *addr, struct task_struct *t)
{
	/* 非强制版本尊重目标的屏蔽/忽略 disposition。 */
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = 0;
	/* 普通 fault trapno wrapper 填充地址与 trap 编号后交给 send_sig_info。 */
	info.si_code  = code;
	info.si_addr  = addr;
	return send_sig_info(info.si_signo, &info, t);
}

/*
 * 契约补充：@code 限 MCEERR_AO/AR，@addr 是故障地址值，@lsb 是影响粒度
 * 指数；均为纯输入。无入口锁；返回 current 的强制发送结果。
 */
int force_sig_mceerr(int code, void __user *addr, short lsb)
{
	/* MCE SIGBUS 携带故障地址以及受影响地址粒度的 log2 值。 */
	struct kernel_siginfo info;

	WARN_ON((code != BUS_MCEERR_AO) && (code != BUS_MCEERR_AR));
	clear_siginfo(&info);
	info.si_signo = SIGBUS;
	info.si_errno = 0;
	info.si_code = code;
	info.si_addr = addr;
	info.si_addr_lsb = lsb;
	return force_sig_info(&info);
}

/*
 * 契约补充：向借用目标 @t 发送 MCE SIGBUS；地址只复制、不解引用。
 * 无入口锁；返回目标退出/分配/发送结果，@t 所有权不变。
 */
int send_sig_mceerr(int code, void __user *addr, short lsb, struct task_struct *t)
{
	/* 向指定任务发送可被正常 disposition 处理的 MCE SIGBUS。 */
	struct kernel_siginfo info;

	WARN_ON((code != BUS_MCEERR_AO) && (code != BUS_MCEERR_AR));
	clear_siginfo(&info);
	info.si_signo = SIGBUS;
	info.si_errno = 0;
	info.si_code = code;
	info.si_addr = addr;
	info.si_addr_lsb = lsb;
	return send_sig_info(info.si_signo, &info, t);
}
EXPORT_SYMBOL(send_sig_mceerr);

/*
 * 契约补充：三个用户指针分别是故障地址与合法边界，仅作为 payload。
 * 无入口锁且不访问其内存；返回 current 的强制 SIGSEGV 发送结果。
 */
int force_sig_bnderr(void __user *addr, void __user *lower, void __user *upper)
{
	/* MPX 边界错误使用 SIGSEGV/SEGV_BNDERR，并附合法区间上下界。 */
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = SIGSEGV;
	info.si_errno = 0;
	info.si_code  = SEGV_BNDERR;
	info.si_addr  = addr;
	info.si_lower = lower;
	info.si_upper = upper;
	return force_sig_info(&info);
}

#ifdef SEGV_PKUERR
/*
 * 契约补充：@addr 为故障地址值，@pkey 为保护键；均为纯输入。
 * 无入口锁；返回强制发送结果，仅在定义 SEGV_PKUERR 的配置构建。
 */
int force_sig_pkuerr(void __user *addr, u32 pkey)
{
	/* PKU 访问错误附带触发保护的 protection key。 */
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = SIGSEGV;
	info.si_errno = 0;
	info.si_code  = SEGV_PKUERR;
	info.si_addr  = addr;
	info.si_pkey  = pkey;
	return force_sig_info(&info);
}
#endif

/*
 * 契约补充：@addr/@type/@sig_data 构成 perf SIGTRAP payload；不取引用。
 * 无入口锁；返回排队结果，并依据 current blocked 状态标记异步递送。
 */
int send_sig_perf(void __user *addr, u32 type, u64 sig_data)
{
	/* perf 事件用 SIGTRAP 传递地址、事件类型和用户数据。 */
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo     = SIGTRAP;
	info.si_errno     = 0;
	info.si_code      = TRAP_PERF;
	info.si_addr      = addr;
	info.si_perf_data = sig_data;
	info.si_perf_type = type;

	/*
	 * Signals generated by perf events should not terminate the whole
	 * process if SIGTRAP is blocked, however, delivering the signal
	 * asynchronously is better than not delivering at all. But tell user
	 * space if the signal was asynchronous, so it can clearly be
	 * distinguished from normal synchronous ones.
	 */
	/*
	 * 被阻塞时不能按同步 SIGTRAP 的默认致命语义杀死线程组，
	 * 改标 ASYNC 后仍入队，让用户态能区分延迟的 perf 通知与真正 trap。
	 */
	info.si_perf_flags = sigismember(&current->blocked, info.si_signo) ?
				     TRAP_PERF_FLAG_ASYNC :
				     0;

	return send_sig_info(info.si_signo, &info, current);
}

/**
 * force_sig_seccomp - signals the task to allow in-process syscall emulation
 * @syscall: syscall number to send to userland
 * @reason: filter-supplied reason code to send to userland (via si_errno)
 * @force_coredump: true to trigger a coredump
 *
 * Forces a SIGSYS with a code of SYS_SECCOMP and related sigsys info.
 */
/*
 * 中文学习注释：把 syscall 号、调用地址、审计架构和 filter reason 封装为
 * SYS_SECCOMP。@force_coredump 决定是允许当前 SIGSYS handler 模拟调用，
 * 还是用不可改写的 HANDLER_EXIT 固定退出/core 语义。
 */
int force_sig_seccomp(int syscall, int reason, bool force_coredump)
{
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = SIGSYS;
	info.si_code = SYS_SECCOMP;
	info.si_call_addr = (void __user *)KSTK_EIP(current);
	/* seccomp 强制 SIGSYS 时完整填充 syscall、arch 与 call_addr fault payload。 */
	info.si_errno = reason;
	info.si_arch = syscall_get_arch(current);
	info.si_syscall = syscall;
	return force_sig_info_to_task(&info, current,
		force_coredump ? HANDLER_EXIT : HANDLER_CURRENT);
}

/* For the crazy architectures that include trap information in
 * the errno field, instead of an actual errno value.
 */
/* 兼容把硬件 trap 元数据塞进 si_errno 的历史架构 ABI。 */
int force_sig_ptrace_errno_trap(int errno, void __user *addr)
{
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = SIGTRAP;
	info.si_errno = errno;
	info.si_code  = TRAP_HWBKPT;
	info.si_addr  = addr;
	return force_sig_info(&info);
}

/* For the rare architectures that include trap information using
 * si_trapno.
 */
/* 强制版本携带架构 trap 编号并发给 current。 */
/*
 * 契约补充：四个参数均为构造 fault siginfo 的输入值，@addr 不解引用。
 * 无入口锁；返回 current 强制发送结果，仅供携带 trapno 的架构。
 */
int force_sig_fault_trapno(int sig, int code, void __user *addr, int trapno)
{
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = 0;
	/* fault trapno wrapper 只组装 kernel_siginfo，实际权限、排队和唤醒交给发送核心。 */
	info.si_code  = code;
	info.si_addr  = addr;
	info.si_trapno = trapno;
	return force_sig_info(&info);
}

/* For the rare architectures that include trap information using
 * si_trapno.
 */
/* 普通发送版本尊重指定任务的 disposition。 */
/*
 * 契约补充：向借用目标 @t 普通发送 trapno fault，尊重其 disposition。
 * 输入所有权不变；返回发送 errno，内部管理可能分配的队列节点。
 */
int send_sig_fault_trapno(int sig, int code, void __user *addr, int trapno,
			  struct task_struct *t)
{
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = 0;
	/* 强制 fault trapno wrapper 填充地址与 trap 编号后交给 force_sig_info。 */
	info.si_code  = code;
	info.si_addr  = addr;
	info.si_trapno = trapno;
	return send_sig_info(info.si_signo, &info, t);
}

/*
 * 契约补充：@info/@pgrp 为借用，函数以 tasklist 读锁稳定进程组遍历。
 * 返回至少一次成功的 0、空组 -ESRCH，或全部失败时的成员错误。
 */
static int kill_pgrp_info(int sig, struct kernel_siginfo *info, struct pid *pgrp)
{
	/* 用 tasklist 读锁封装要求锁已持有的 __kill_pgrp_info()。 */
	int ret;
	read_lock(&tasklist_lock);
	ret = __kill_pgrp_info(sig, info, pgrp);
	read_unlock(&tasklist_lock);
	return ret;
}

/*
 * 契约补充：旧式进程组 wrapper；@pid 为借用 PGID，@priv 选择来源语义。
 * 返回组发送结果，不接管 pid 引用，调用者负责其生命周期。
 */
int kill_pgrp(struct pid *pid, int sig, int priv)
{
	/* 旧式进程组发送 wrapper。 */
	return kill_pgrp_info(sig, __si_special(priv), pid);
}
EXPORT_SYMBOL(kill_pgrp);

/*
 * 契约补充：旧式线程组 wrapper；@pid 为借用稳定 PID，@priv 决定来源。
 * 返回发送结果；目标退出由内部 RCU/leader 重试处理。
 */
int kill_pid(struct pid *pid, int sig, int priv)
{
	/* 旧式 struct pid 线程组发送 wrapper。 */
	return kill_pid_info(sig, __si_special(priv), pid);
}
EXPORT_SYMBOL(kill_pid);

#ifdef CONFIG_POSIX_TIMERS
/*
 * These functions handle POSIX timer signals. POSIX timers use
 * preallocated sigqueue structs for sending signals.
 */
/*
 * 中文学习注释：POSIX timer 把 sigqueue 嵌在 k_itimer 中，生命周期由 timer
 * 引用计数而非 slab 临时分配管理。pending 链表、ignored_posix_timers
 * 链表和 timer 状态三者的迁移必须在 sighand 锁及 timer 引用协议下进行。
 */
static void __flush_itimer_signals(struct sigpending *pending)
{
	sigset_t signal, retain;
	struct sigqueue *q, *n;

	signal = pending->signal;
	sigemptyset(&retain);

	list_for_each_entry_safe(q, n, &pending->list, list) {
		int sig = q->info.si_signo;

		if (likely(q->info.si_code != SI_TIMER)) {
			sigaddset(&retain, sig);
	/* 在 siglock 下同步清位并摘出 SI_TIMER 节点，节点释放延后到锁外完成。 */
		} else {
			sigdelset(&signal, sig);
			list_del_init(&q->list);
			__sigqueue_free(q);
		}
	}

	sigorsets(&pending->signal, &signal, &retain);
}

/* flush_itimer_signals()：清除 current 私有及共享 pending 中的 timer 信号。 */
void flush_itimer_signals(void)
{
	/* 在 current siglock 下同时清理私有与共享队列中的 timer 实例。 */
	struct task_struct *tsk = current;

	guard(spinlock_irqsave)(&tsk->sighand->siglock);
	__flush_itimer_signals(&tsk->pending);
	__flush_itimer_signals(&tsk->signal->shared_pending);
}

/*
 * 契约补充：@q 是 k_itimer 所有的输出对象；函数为它预占用户 pending
 * 配额。成功 true 并把配额释放责任交给 q，失败 false 且 q 不可排队。
 */
bool posixtimer_init_sigqueue(struct sigqueue *q)
{
	/* 创建 timer 时预占一份用户 pending 配额并标记 PREALLOC。 */
	struct ucounts *ucounts = sig_get_ucounts(current, -1, 0);

	if (!ucounts)
		return false;
	clear_siginfo(&q->info);
	__sigqueue_init(q, ucounts, SIGQUEUE_PREALLOC);
	return true;
}

static void posixtimer_queue_sigqueue(struct sigqueue *q, struct task_struct *t, enum pid_type type)
{
	/* 已持 sighand 锁：把预分配节点接入相应队列、置位并选择接收者。 */
	struct sigpending *pending;
	int sig = q->info.si_signo;

	signalfd_notify(t, sig);
	pending = (type != PIDTYPE_PID) ? &t->signal->shared_pending : &t->pending;
	list_add_tail(&q->list, &pending->list);
	sigaddset(&pending->signal, sig);
	complete_signal(sig, t, type);
}

/*
 * This function is used by POSIX timers to deliver a timer signal.
 * Where type is PIDTYPE_PID (such as for timers with SIGEV_THREAD_ID
 * set), the signal must be delivered to the specific thread (queues
 * into t->pending).
 *
 * Where type is not PIDTYPE_PID, signals must be delivered to the
 * process. In this case, prefer to deliver to current if it is in
 * the same thread group as the target process and its sighand is
 * stable, which avoids unnecessarily waking up a potentially idle task.
 */
/*
 * 线程定向 timer 必须保持 PIDTYPE_PID；进程定向 timer 若 current
 * 属于目标组且仍存活，优先让 current 消费以避免无谓唤醒其他 CPU。
 */
static inline struct task_struct *posixtimer_get_target(struct k_itimer *tmr)
{
	struct task_struct *t = pid_task(tmr->it_pid, tmr->it_pid_type);

	if (t && tmr->it_pid_type != PIDTYPE_PID &&
	    same_thread_group(t, current) && !current->exit_state)
		t = current;
	return t;
}

/*
 * 契约补充：@tmr 为调用者持有引用的 timer；函数在 RCU 下找目标并取得
 * sighand 锁。返回无；可能排队、转入 ignored 链表或归还 signal 引用。
 */
void posixtimer_send_sigqueue(struct k_itimer *tmr)
{
	/*
	 * timer 到期递送主状态机：RCU 找目标并锁 sighand，发布本次 sequence
	 * 与 periodic 状态；被忽略的周期 timer 转移到 ignored 链表并持引用，
	 * oneshot 则丢引用；已 pending 时合并；恢复可递送时把 ignored 节点搬回
	 * pending。所有出口通过 tracepoint 记录结果并解锁。
	 */
	struct sigqueue *q = &tmr->sigq;
	int sig = q->info.si_signo;
	struct task_struct *t;
	unsigned long flags;
	int result;

	guard(rcu)();
	/* POSIX timer 在锁下校验有效期、处理重排队并更新 overrun，随后发送或释放节点。 */

	t = posixtimer_get_target(tmr);
	if (!t)
		return;

	if (!likely(lock_task_sighand(t, &flags)))
		return;

	/*
	 * Update @tmr::sigqueue_seq for posix timer signals with sighand
	 * locked to prevent a race against dequeue_signal().
	 */
	/*
	 * 在 sighand 锁内把本次 timer sequence 发布给 sigqueue，
	 * 与 dequeue_signal() 串行，防止出队者把旧到期实例误认成当前实例。
	 */
	tmr->it_sigqueue_seq = tmr->it_signal_seq;

	/*
	 * Set the signal delivery status under sighand lock, so that the
	 * ignored signal handling can distinguish between a periodic and a
	 * non-periodic timer.
	 */
	/*
	 * 同一把锁下快照 timer 是否周期重排，使忽略路径能区分
	 * “仍会再次到期”的周期 timer 与只应丢弃一次的 oneshot。
	 */
	tmr->it_sig_periodic = tmr->it_status == POSIX_TIMER_REQUEUE_PENDING;

	if (!prepare_signal(sig, t, false)) {
		result = TRACE_SIGNAL_IGNORED;

		if (!list_empty(&q->list)) {
			/*
			 * The signal was ignored and blocked. The timer
			 * expiry queued it because blocked signals are
			 * queued independent of the ignored state.
			 *
			 * The unblocking set SIGPENDING, but the signal
			 * was not yet dequeued from the pending list.
			 * So prepare_signal() sees unblocked and ignored,
			 * which ends up here. Leave it queued like a
			 * regular signal.
			 *
			 * The same happens when the task group is exiting
			 * and the signal is already queued.
			 * prepare_signal() treats SIGNAL_GROUP_EXIT as
			 * ignored independent of its queued state. This
			 * gets cleaned up in __exit_signal().
			 */
			/*
			 * 被屏蔽信号即使 disposition 为忽略也会排队；解除
			 * 屏蔽后 prepare_signal() 才发现忽略，但节点尚未出队，因此
			 * 必须像普通 pending 一样保留。线程组退出时已有节点也走同一
			 * 分支，最终由 __exit_signal() 清理，不能在此重复释放。
			 */
			goto out;
		}

		/* Periodic timers with SIG_IGN are queued on the ignored list */
		/* 周期 timer 被忽略后进入专用链表，保留重启所需的引用与状态。 */
		if (tmr->it_sig_periodic) {
			/*
			 * Already queued means the timer was rearmed after
			 * the previous expiry got it on the ignore list.
			 * Nothing to do for that case.
			 */
			/*
			 * 节点已在 ignored 链表说明上次到期后 timer 又被重装；
			 * 当前到期无需重复插入或增加引用。
			 */
			if (hlist_unhashed(&tmr->ignored_list)) {
				/*
				 * Take a signal reference and queue it on
				 * the ignored list.
				 */
				/* 首次进入 ignored 链表前取得 signal 引用，链表负责持有。 */
				posixtimer_sigqueue_getref(q);
				posixtimer_sig_ignore(t, q);
			}
		} else if (!hlist_unhashed(&tmr->ignored_list)) {
			/*
			 * Covers the case where a timer was periodic and
			 * then the signal was ignored. Later it was rearmed
			 * as oneshot timer. The previous signal is invalid
			 * now, and this oneshot signal has to be dropped.
			 * Remove it from the ignored list and drop the
			 * reference count as the signal is not longer
			 * queued.
			 */
			/*
			 * timer 从周期改为 oneshot 后，旧 ignored 实例已失效；
			 * 移出链表并归还其引用，本次被忽略的 oneshot 也直接丢弃。
			 */
			hlist_del_init(&tmr->ignored_list);
			posixtimer_putref(tmr);
		}
		goto out;
	}

	if (unlikely(!list_empty(&q->list))) {
		/* This holds a reference count already */
		/* 节点已 pending 时链表已经持有引用，本次只记录合并结果。 */
		result = TRACE_SIGNAL_ALREADY_PENDING;
		goto out;
	}

	/*
	 * If the signal is on the ignore list, it got blocked after it was
	 * ignored earlier. But nothing lifted the ignore. Move it back to
	 * the pending list to be consistent with the regular signal
	 * handling. This already holds a reference count.
	 *
	 * If it's not on the ignore list acquire a reference count.
	 */
	/*
	 * ignored 节点后来被屏蔽后应恢复为普通 pending；从 ignored
	 * 链表移出时沿用已有引用。全新节点则先取得引用，再交给 pending 链表。
	 */
	if (likely(hlist_unhashed(&tmr->ignored_list)))
		posixtimer_sigqueue_getref(q);
	else
		hlist_del_init(&tmr->ignored_list);

	posixtimer_queue_sigqueue(q, t, tmr->it_pid_type);
	result = TRACE_SIGNAL_DELIVERED;
out:
	trace_signal_generate(sig, &q->info, t, tmr->it_pid_type != PIDTYPE_PID, result);
	unlock_task_sighand(t, &flags);
}

/* posixtimer_sig_ignore()：把被忽略的周期 timer 节点转入 ignored 链表。 */
static inline void posixtimer_sig_ignore(struct task_struct *tsk, struct sigqueue *q)
{
	/* 仍有效的周期 timer 挂入线程组 ignored 链表，否则直接归还引用。 */
	struct k_itimer *tmr = container_of(q, struct k_itimer, sigq);

	/*
	 * If the timer is marked deleted already or the signal originates
	 * from a non-periodic timer, then just drop the reference
	 * count. Otherwise queue it on the ignored list.
	 */
	/*
	 * 已删除或非周期 timer 不会再产生后续到期，直接丢引用；
	 * 仍有效的周期 timer 才由 ignored 链表继续持有。
	 */
	if (posixtimer_valid(tmr) && tmr->it_sig_periodic)
		hlist_add_head(&tmr->ignored_list, &tsk->signal->ignored_posix_timers);
	else
		posixtimer_putref(tmr);
}

static void posixtimer_sig_unignore(struct task_struct *tsk, int sig)
{
	/*
	 * disposition 从 SIG_IGN 改回可递送时，筛选同号 timer 并搬回 pending。
	 * 因 sighand->siglock 与 tmr->it_lock 锁序不允许在此重装 timer，只恢复
	 * sigqueue，让正常 dequeue/重装路径继续；目标线程已退则丢引用。
	 */
	struct hlist_head *head = &tsk->signal->ignored_posix_timers;
	struct hlist_node *tmp;
	struct k_itimer *tmr;

	if (likely(hlist_empty(head)))
		return;

	/*
	 * Rearming a timer with sighand lock held is not possible due to
	 * lock ordering vs. tmr::it_lock. Just stick the sigqueue back and
	 * let the signal delivery path deal with it whether it needs to be
	 * rearmed or not. This cannot be decided here w/o dropping sighand
	 * lock and creating a loop retry horror show.
	 */
	/*
	 * sighand->siglock 与 tmr->it_lock 的既定锁序禁止在此直接
	 * 重装 timer。把节点放回 pending 可复用递送路径；若为判断重装而解锁
	 * 再重试，会引入 disposition 反复变化导致的复杂循环竞态。
	 */
	hlist_for_each_entry_safe(tmr, tmp , head, ignored_list) {
		struct task_struct *target;

		/*
		 * tmr::sigq.info.si_signo is immutable, so accessing it
		 * without holding tmr::it_lock is safe.
		 */
		/* si_signo 创建后不再改变，故筛选同号 timer 无需 it_lock。 */
		if (tmr->sigq.info.si_signo != sig)
			continue;

		hlist_del_init(&tmr->ignored_list);

		/* This should never happen and leaks a reference count */
		/* 节点同时在 pending 链表属不变量破坏；跳过会泄漏引用以避免双挂链。 */
		if (WARN_ON_ONCE(!list_empty(&tmr->sigq.list)))
			continue;

		/*
		 * Get the target for the signal. If target is a thread and
		 * has exited by now, drop the reference count.
		 */
		/*
		 * RCU 下重新解析目标；线程定向目标若已退出不能改投他人，
		 * 因而归还引用，进程定向目标则可由 helper 选择仍存活成员。
		 */
		guard(rcu)();
		target = posixtimer_get_target(tmr);
		if (target)
			posixtimer_queue_sigqueue(&tmr->sigq, target, tmr->it_pid_type);
		else
			posixtimer_putref(tmr);
	}
}
#else /* CONFIG_POSIX_TIMERS */
/* 关闭 POSIX timer 配置时保留空 stub，使通用信号代码无需条件分支。 */
static inline void posixtimer_sig_ignore(struct task_struct *tsk, struct sigqueue *q) { }
static inline void posixtimer_sig_unignore(struct task_struct *tsk, int sig) { }
#endif /* !CONFIG_POSIX_TIMERS */
/* 至此结束无 POSIX timer 的替代实现。 */

/* do_notify_pidfd()：任务退出后向其 pidfd poll 等待队列发布可读事件。 */
void do_notify_pidfd(struct task_struct *task)
{
	/* 任务成为 zombie/dead 后唤醒 pidfd poll 等待者，发布可读退出事件。 */
	struct pid *pid = task_pid(task);

	WARN_ON(task->exit_state == 0);

	__wake_up(&pid->wait_pidfd, TASK_NORMAL, 0,
			poll_to_key(EPOLLIN | EPOLLRDNORM));
}

/*
 * Let a parent know about the death of a child.
 * For a stopped/continued status change, use do_notify_parent_cldstop instead.
 *
 * Returns true if our parent ignored us and so we've switched to
 * self-reaping.
 */
/*
 * 中文学习注释：
 * - 先唤醒 pidfd；再构造父进程 namespace 视角下的 pid/uid、累计 CPU 时间
 *   与 CLD_EXITED/KILLED/DUMPED 状态。
 * - 非 SIGCHLD 的自定义 death signal 仅在父子未跨 exec 安全域时保留，
 *   否则降级为 SIGCHLD，防止新程序收到旧关系指定的任意信号。
 * - 在父 sighand 锁下检查 SIG_IGN/SA_NOCLDWAIT；需要 autoreap 时不留下
 *   zombie，但仍唤醒 wait4，使其及时得到 -ECHILD。
 * 返回 true 表示退出路径可自行回收。
 */
bool do_notify_parent(struct task_struct *tsk, int sig)
{
	struct kernel_siginfo info;
	unsigned long flags;
	struct sighand_struct *psig;
	bool autoreap = false;
	u64 utime, stime;

	if (WARN_ON_ONCE(!valid_signal(sig)))
		return false;

	/* do_notify_parent_cldstop should have been called instead.  */
	/* 停止/继续状态必须走 cldstop 专用入口，死亡通知不接受该状态。 */
	WARN_ON_ONCE(task_is_stopped_or_traced(tsk));

	WARN_ON_ONCE(!tsk->ptrace && !thread_group_empty(tsk));

	/* ptraced, or group-leader without sub-threads */
	/* 到此任务要么被跟踪，要么是已经没有其他线程的组长。 */
	do_notify_pidfd(tsk);

	if (sig != SIGCHLD) {
		/*
		 * This is only possible if parent == real_parent.
		 * Check if it has changed security domain.
		 */
		/*
		 * 自定义死亡信号只允许发给 real_parent；若父进程经过
		 * exec 改变安全域，则降级为 SIGCHLD，避免旧关系注入任意信号。
		 */
		if (tsk->parent_exec_id != READ_ONCE(tsk->parent->self_exec_id))
			sig = SIGCHLD;
	}

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = 0;
	/*
	 * We are under tasklist_lock here so our parent is tied to
	 * us and cannot change.
	 *
	 * task_active_pid_ns will always return the same pid namespace
	 * until a task passes through release_task.
	 *
	 * write_lock() currently calls preempt_disable() which is the
	 * same as rcu_read_lock(), but according to Oleg, this is not
	 * correct to rely on this
	 */
	/*
	 * tasklist_lock 稳定 parent 关系，任务释放前 active pid
	 * namespace 也不变；但不能依赖写锁“碰巧”禁用抢占来代替正式 RCU
	 * 契约，因此仍显式进入 RCU 读侧读取凭据与 namespace 相关字段。
	 */
	rcu_read_lock();
	info.si_pid = task_pid_nr_ns(tsk, task_active_pid_ns(tsk->parent));
	info.si_uid = from_kuid_munged(task_cred_xxx(tsk->parent, user_ns),
				       task_uid(tsk));
	rcu_read_unlock();

	task_cputime(tsk, &utime, &stime);
	/* 退出通知包含线程及已回收子线程累计的 CPU 时间。 */
	info.si_utime = nsec_to_clock_t(utime + tsk->signal->utime);
	info.si_stime = nsec_to_clock_t(stime + tsk->signal->stime);

	info.si_status = tsk->exit_code & 0x7f;
	if (tsk->exit_code & 0x80)
		info.si_code = CLD_DUMPED;
	else if (tsk->exit_code & 0x7f)
		info.si_code = CLD_KILLED;
	/* 退出原因先规范化为 CLD_*，再构造 SIGCHLD、检查父 disposition 并决定 autoreap。 */
	else {
		info.si_code = CLD_EXITED;
		info.si_status = tsk->exit_code >> 8;
	}

	psig = tsk->parent->sighand;
	/* 父 sighand 锁下同时判定 autoreap 与是否实际生成死亡信号。 */
	spin_lock_irqsave(&psig->siglock, flags);
	if (!tsk->ptrace && sig == SIGCHLD &&
	    (psig->action[SIGCHLD-1].sa.sa_handler == SIG_IGN ||
	     (psig->action[SIGCHLD-1].sa.sa_flags & SA_NOCLDWAIT))) {
		/*
		 * We are exiting and our parent doesn't care.  POSIX.1
		 * defines special semantics for setting SIGCHLD to SIG_IGN
		 * or setting the SA_NOCLDWAIT flag: we should be reaped
		 * automatically and not left for our parent's wait4 call.
		 * Rather than having the parent do it as a magic kind of
		 * signal handler, we just set this to tell do_exit that we
		 * can be cleaned up without becoming a zombie.  Note that
		 * we still call __wake_up_parent in this case, because a
		 * blocked sys_wait4 might now return -ECHILD.
		 *
		 * Whether we send SIGCHLD or not for SA_NOCLDWAIT
		 * is implementation-defined: we do (if you don't want
		 * it, just use SIG_IGN instead).
		 */
		/*
		 * 父进程忽略 SIGCHLD 或设置 SA_NOCLDWAIT 时，子进程可
		 * 自动回收而不留下 zombie；仍须唤醒 wait4，使等待者及时得到
		 * -ECHILD。SA_NOCLDWAIT 是否仍发送 SIGCHLD 由实现决定，Linux
		 * 选择发送；显式 SIG_IGN 才同时抑制该信号。
		 */
		autoreap = true;
		if (psig->action[SIGCHLD-1].sa.sa_handler == SIG_IGN)
			sig = 0;
	}
	if (!tsk->ptrace && tsk->signal->autoreap) {
		autoreap = true;
		sig = 0;
	}
	/*
	 * Send with __send_signal as si_pid and si_uid are in the
	 * parent's namespaces.
	 */
	/*
	 * si_pid/si_uid 已转换成父进程视角，不能再走会重复转换身份的
	 * 外层 wrapper，直接在父 sighand 锁下调用核心发送函数。
	 */
	if (sig)
		__send_signal_locked(sig, &info, tsk->parent, PIDTYPE_TGID, false);
	__wake_up_parent(tsk, tsk->parent);
	spin_unlock_irqrestore(&psig->siglock, flags);

	return autoreap;
}

/**
 * do_notify_parent_cldstop - notify parent of stopped/continued state change
 * @tsk: task reporting the state change
 * @for_ptracer: the notification is for ptracer
 * @why: CLD_{CONTINUED|STOPPED|TRAPPED} to report
 *
 * Notify @tsk's parent that the stopped/continued state has changed.  If
 * @for_ptracer is %false, @tsk's group leader notifies to its real parent.
 * If %true, @tsk reports to @tsk->parent which should be the ptracer.
 *
 * CONTEXT:
 * Must be called with tasklist_lock at least read locked.
 */
/*
 * 中文学习注释：ptrace stop 通知当前 tracer；普通 group-stop 则由组长身份
 * 通知 real_parent。siginfo 的 pid/uid 必须按接收父进程 namespace 转换。
 * SIGCHLD 被忽略或 SA_NOCLDSTOP 只抑制信号，不能抑制 wait4 唤醒。
 */
static void do_notify_parent_cldstop(struct task_struct *tsk,
				     bool for_ptracer, int why)
{
	struct kernel_siginfo info;
	unsigned long flags;
	struct task_struct *parent;
	struct sighand_struct *sighand;
	u64 utime, stime;

	if (for_ptracer) {
	/* 先选择父/ptracer 与通知原因，再在目标 sighand 锁下决定入队、唤醒和 SIGCHLD 忽略语义。 */
		parent = tsk->parent;
	} else {
		tsk = tsk->group_leader;
		parent = tsk->real_parent;
	}

	clear_siginfo(&info);
	info.si_signo = SIGCHLD;
	info.si_errno = 0;
	/*
	 * see comment in do_notify_parent() about the following 4 lines
	 */
	/* 以下 pid/uid 转换沿用死亡通知的 tasklist_lock+RCU 稳定协议。 */
	rcu_read_lock();
	info.si_pid = task_pid_nr_ns(tsk, task_active_pid_ns(parent));
	info.si_uid = from_kuid_munged(task_cred_xxx(parent, user_ns), task_uid(tsk));
	rcu_read_unlock();

	task_cputime(tsk, &utime, &stime);
	/* CPU 时间取任务快照后转换为用户 ABI clock tick 单位。 */
	info.si_utime = nsec_to_clock_t(utime);
	info.si_stime = nsec_to_clock_t(stime);

 	info.si_code = why;
 	switch (why) {
 	case CLD_CONTINUED:
 		info.si_status = SIGCONT;
 		break;
 	case CLD_STOPPED:
 		info.si_status = tsk->signal->group_exit_code & 0x7f;
	/* 先选择父/ptracer 与通知原因，再在目标 sighand 锁下决定入队、唤醒和 SIGCHLD 忽略语义。 */
 		break;
 	case CLD_TRAPPED:
 		info.si_status = tsk->exit_code & 0x7f;
 		break;
 	default:
 		BUG();
 	}

	sighand = parent->sighand;
	/* 父 sighand 锁同时保护 SIGCHLD disposition 与 pending 入队。 */
	spin_lock_irqsave(&sighand->siglock, flags);
	if (sighand->action[SIGCHLD-1].sa.sa_handler != SIG_IGN &&
	    !(sighand->action[SIGCHLD-1].sa.sa_flags & SA_NOCLDSTOP))
		send_signal_locked(SIGCHLD, &info, parent, PIDTYPE_TGID);
	/*
	 * Even if SIGCHLD is not generated, we must wake up wait4 calls.
	 */
	/* wait4 等待的是子状态而非信号本身，抑制 SIGCHLD 仍必须唤醒。 */
	__wake_up_parent(tsk, parent);
	spin_unlock_irqrestore(&sighand->siglock, flags);
}

/*
 * This must be called with current->sighand->siglock held.
 *
 * This should be the path for all ptrace stops.
 * We always set current->last_siginfo while stopped here.
 * That makes it a way to test a stopped process for
 * being ptrace-stopped vs being job-control-stopped.
 *
 * Returns the signal the ptracer requested the code resume
 * with.  If the code did not stop because the tracer is gone,
 * the stop signal remains unchanged unless clear_code.
 */
/*
 * 中文学习注释：ptrace_stop() 是所有 tracee 停止的共同提交点。
 *
 * 锁与状态顺序：
 * 1. 架构 hook 可能睡眠，必须在任何 stop 记账前临时释放 siglock；
 * 2. 置 TASK_TRACED/JOBCTL_TRACED 后用写屏障，再清 JOBCTL_TRAPPING，
 *    让 tracer 的 wait_on_bit -> do_wait 一定观察到已停止状态；
 * 3. 发布 last_siginfo/exit_code，分别通知 tracer 与真实父进程；
 * 4. 释放 siglock 后 schedule，恢复时重新持锁再清共享诊断字段。
 *
 * fatal signal 或 ptrace unlink 可取消睡眠；返回值是 tracer 要求恢复时注入
 * 的信号。函数声明显式标出释放并重新获取 siglock 的锁语义。
 */
static int ptrace_stop(int exit_code, int why, unsigned long message,
		       kernel_siginfo_t *info)
	__releases(&current->sighand->siglock)
	__acquires(&current->sighand->siglock)
{
	bool gstop_done = false;

	if (arch_ptrace_stop_needed()) {
		/*
		 * The arch code has something special to do before a
		 * ptrace stop.  This is allowed to block, e.g. for faults
		 * on user stack pages.  We can't keep the siglock while
		 * calling arch_ptrace_stop, so we must release it now.
		 * To preserve proper semantics, we must do this before
		 * any signal bookkeeping like checking group_stop_count.
		 */
		/*
		 * 架构 hook 可能为用户栈缺页等原因睡眠，故必须释放
		 * siglock；又必须早于 group-stop 记账，避免解锁窗口留下半提交状态。
		 */
		spin_unlock_irq(&current->sighand->siglock);
		arch_ptrace_stop();
		spin_lock_irq(&current->sighand->siglock);
	}

	/*
	 * After this point ptrace_signal_wake_up or signal_wake_up
	 * will clear TASK_TRACED if ptrace_unlink happens or a fatal
	 * signal comes in.  Handle previous ptrace_unlinks and fatal
	 * signals here to prevent ptrace_stop sleeping in schedule.
	 */
	/*
	 * 从这里起唤醒路径可清 TASK_TRACED；先处理已经发生的 detach
	 * 或致命信号，避免随后以无人能恢复的 traced 状态进入 schedule()。
	 */
	if (!current->ptrace || __fatal_signal_pending(current))
		return exit_code;

	set_special_state(TASK_TRACED);
	current->jobctl |= JOBCTL_TRACED;

	/*
	 * We're committing to trapping.  TRACED should be visible before
	 * TRAPPING is cleared; otherwise, the tracer might fail do_wait().
	 * Also, transition to TRACED and updates to ->jobctl should be
	 * atomic with respect to siglock and should be done after the arch
	 * hook as siglock is released and regrabbed across it.
	 *
	 *     TRACER				    TRACEE
	 *
	 *     ptrace_attach()
	 * [L]   wait_on_bit(JOBCTL_TRAPPING)	[S] set_special_state(TRACED)
	 *     do_wait()
	 *       set_current_state()                smp_wmb();
	 *       ptrace_do_wait()
	 *         wait_task_stopped()
	 *           task_stopped_code()
	 * [L]         task_is_traced()		[S] task_clear_jobctl_trapping();
	 */
	/*
	 * 这是 tracer/tracee 的发布配对。tracee 先发布 TASK_TRACED，
	 * 写屏障后再清 TRAPPING；tracer 从等待位醒来并进入 do_wait() 时因此
	 * 必能看到停止状态。siglock 还使 jobctl 更新对信号发送者保持原子。
	 */
	smp_wmb();

	current->ptrace_message = message;
	current->last_siginfo = info;
	current->exit_code = exit_code;

	/*
	 * If @why is CLD_STOPPED, we're trapping to participate in a group
	 * stop.  Do the bookkeeping.  Note that if SIGCONT was delievered
	 * across siglock relocks since INTERRUPT was scheduled, PENDING
	 * could be clear now.  We act as if SIGCONT is received after
	 * TASK_TRACED is entered - ignore it.
	 */
	/*
	 * CLD_STOPPED trap 同时消费 group-stop 计数。若解锁窗口收到
	 * SIGCONT 并清了 pending，则按“已进入 TASK_TRACED 后才继续”处理，
	 * 不再撤销本次已经提交的 trace stop。
	 */
	if (why == CLD_STOPPED && (current->jobctl & JOBCTL_STOP_PENDING))
		gstop_done = task_participate_group_stop(current);

	/* any trap clears pending STOP trap, STOP trap clears NOTIFY */
	/* 任意 trap 消费 STOP 请求；事件型 STOP 还一并消费 sticky NOTIFY。 */
	task_clear_jobctl_pending(current, JOBCTL_TRAP_STOP);
	if (info && info->si_code >> 8 == PTRACE_EVENT_STOP)
		task_clear_jobctl_pending(current, JOBCTL_TRAP_NOTIFY);

	/* entering a trap, clear TRAPPING */
	/* 停止状态和诊断字段已发布，现在可清等待位并唤醒 tracer。 */
	task_clear_jobctl_trapping(current);

	spin_unlock_irq(&current->sighand->siglock);
	read_lock(&tasklist_lock);
	/*
	 * Notify parents of the stop.
	 *
	 * While ptraced, there are two parents - the ptracer and
	 * the real_parent of the group_leader.  The ptracer should
	 * know about every stop while the real parent is only
	 * interested in the completion of group stop.  The states
	 * for the two don't interact with each other.  Notify
	 * separately unless they're gonna be duplicates.
	 */
	/*
	 * ptracer 观察每次 trace stop，真实父进程只观察线程组停止完成；
	 * 两套 wait 状态相互独立，因此分别通知，只有接收者相同时去重。
	 */
	if (current->ptrace)
		do_notify_parent_cldstop(current, true, why);
	if (gstop_done && (!current->ptrace || ptrace_reparented(current)))
		do_notify_parent_cldstop(current, false, why);

	/*
	 * The previous do_notify_parent_cldstop() invocation woke ptracer.
	 * One a PREEMPTION kernel this can result in preemption requirement
	 * which will be fulfilled after read_unlock() and the ptracer will be
	 * put on the CPU.
	 * The ptracer is in wait_task_inactive(, __TASK_TRACED) waiting for
	 * this task wait in schedule(). If this task gets preempted then it
	 * remains enqueued on the runqueue. The ptracer will observe this and
	 * then sleep for a delay of one HZ tick. In the meantime this task
	 * gets scheduled, enters schedule() and will wait for the ptracer.
	 *
	 * This preemption point is not bad from a correctness point of
	 * view but extends the runtime by one HZ tick time due to the
	 * ptracer's sleep.  The preempt-disable section ensures that there
	 * will be no preemption between unlock and schedule() and so
	 * improving the performance since the ptracer will observe that
	 * the tracee is scheduled out once it gets on the CPU.
	 *
	 * On PREEMPT_RT locking tasklist_lock does not disable preemption.
	 * Therefore the task can be preempted after do_notify_parent_cldstop()
	 * before unlocking tasklist_lock so there is no benefit in doing this.
	 *
	 * In fact disabling preemption is harmful on PREEMPT_RT because
	 * the spinlock_t in cgroup_enter_frozen() must not be acquired
	 * with preemption disabled due to the 'sleeping' spinlock
	 * substitution of RT.
	 */
	/*
	 * 非 RT 内核短暂禁抢占，使 tracee 从 tasklist 解锁直接进入
	 * schedule，避免 tracer 看到它仍在 runqueue 后多睡一个 tick。PREEMPT_RT
	 * 的 tasklist_lock 不提供同样效果，且 cgroup 冻结锁可能睡眠，禁抢占
	 * 反而违反 RT 锁约束，所以该优化必须按配置关闭。
	 */
	if (!IS_ENABLED(CONFIG_PREEMPT_RT))
		preempt_disable();
	read_unlock(&tasklist_lock);
	cgroup_enter_frozen();
	if (!IS_ENABLED(CONFIG_PREEMPT_RT))
		preempt_enable_no_resched();
	schedule();
	cgroup_leave_frozen(true);

	/*
	 * We are back.  Now reacquire the siglock before touching
	 * last_siginfo, so that we are sure to have synchronized with
	 * any signal-sending on another CPU that wants to examine it.
	 */
	/* 恢复后先重取 siglock，再清 last_siginfo，与并发发送者完成同步。 */
	spin_lock_irq(&current->sighand->siglock);
	exit_code = current->exit_code;
	current->last_siginfo = NULL;
	current->ptrace_message = 0;
	current->exit_code = 0;

	/* LISTENING can be set only during STOP traps, clear it */
	/* 离开 STOP trap 后 LISTENING/PTRACE_FROZEN 均不再有效，统一清除。 */
	current->jobctl &= ~(JOBCTL_LISTENING | JOBCTL_PTRACE_FROZEN);

	/*
	 * Queued signals ignored us while we were stopped for tracing.
	 * So check for any that we should take before resuming user mode.
	 * This sets TIF_SIGPENDING, but never clears it.
	 */
	/*
	 * trace 停止期间新信号只入队不使当前执行；恢复前重新汇总，
	 * 只允许置 TIF、不在远端语义不安全的上下文中清除它。
	 */
	recalc_sigpending_tsk(current);
	return exit_code;
}

/* ptrace_do_notify()：构造 trap 信息并让 current 进入统一 ptrace stop。 */
static int ptrace_do_notify(int signr, int exit_code, int why, unsigned long message)
{
	/* 构造来自 current 的 trap siginfo，并复用统一 ptrace_stop 状态机。 */
	kernel_siginfo_t info;

	clear_siginfo(&info);
	info.si_signo = signr;
	info.si_code = exit_code;
	info.si_pid = task_pid_vnr(current);
	info.si_uid = from_kuid_munged(current_user_ns(), current_uid());

	/* Let the debugger run.  */
	/* 把构造好的 trap 信息交给 ptrace_stop，睡眠等待调试器决定。 */
	return ptrace_stop(exit_code, why, message, &info);
}

int ptrace_notify(int exit_code, unsigned long message)
{
	/*
	 * 对外 ptrace 事件入口：先运行可能影响用户返回的 task_work，再在 siglock
	 * 下陷入 SIGTRAP。exit_code 只允许低字节 SIGTRAP 与高字节事件号。
	 */
	int signr;

	BUG_ON((exit_code & (0x7f | ~0xffff)) != SIGTRAP);
	if (unlikely(task_work_pending(current)))
		task_work_run();

	spin_lock_irq(&current->sighand->siglock);
	signr = ptrace_do_notify(SIGTRAP, exit_code, CLD_TRAPPED, message);
	spin_unlock_irq(&current->sighand->siglock);
	return signr;
}

/**
 * do_signal_stop - handle group stop for SIGSTOP and other stop signals
 * @signr: signr causing group stop if initiating
 *
 * If %JOBCTL_STOP_PENDING is not set yet, initiate group stop with @signr
 * and participate in it.  If already set, participate in the existing
 * group stop.  If participated in a group stop (and thus slept), %true is
 * returned with siglock released.
 *
 * If ptraced, this function doesn't handle stop itself.  Instead,
 * %JOBCTL_TRAP_STOP is scheduled and %false is returned with siglock
 * untouched.  The caller must ensure that INTERRUPT trap handling takes
 * places afterwards.
 *
 * CONTEXT:
 * Must be called with @current->sighand->siglock held, which is released
 * on %true return.
 *
 * RETURNS:
 * %false if group stop is already cancelled or ptrace trap is scheduled.
 * %true if participated in group stop.
 */
/*
 * 中文学习注释：
 * - 首个参与者把 STOP_PENDING/CONSUME 与 stop 编号广播到线程组，并以
 *   group_stop_count 统计尚未真正进入停止态的线程。
 * - 每个线程消费一次计数；最后一个设置 SIGNAL_STOP_STOPPED 并通知父进程。
 * - SIGCONT、group exit 或 exec 可在中途取消，JOBCTL_STOP_DEQUEUED 用于
 *   识别“信号已出队但尚未提交 stop”的窗口。
 * - 非 ptrace 任务置 TASK_STOPPED 后释放 siglock 睡眠；ptrace 任务改挂
 *   TRAP_STOP，由 tracer 状态机完成，不在这里直接睡。
 */
static bool do_signal_stop(int signr)
	__releases(&current->sighand->siglock)
{
	struct signal_struct *sig = current->signal;

	if (!(current->jobctl & JOBCTL_STOP_PENDING)) {
		unsigned long gstop = JOBCTL_STOP_PENDING | JOBCTL_STOP_CONSUME;
		struct task_struct *t;

		/* signr will be recorded in task->jobctl for retries */
		/* stop 编号写入 jobctl，若本轮被打断，后续重试仍知道原因。 */
		WARN_ON_ONCE(signr & ~JOBCTL_STOP_SIGMASK);

		if (!likely(current->jobctl & JOBCTL_STOP_DEQUEUED) ||
		    unlikely(sig->flags & SIGNAL_GROUP_EXIT) ||
		    unlikely(sig->group_exec_task))
			return false;
		/*
		 * There is no group stop already in progress.  We must
		 * initiate one now.
		 *
		 * While ptraced, a task may be resumed while group stop is
		 * still in effect and then receive a stop signal and
		 * initiate another group stop.  This deviates from the
		 * usual behavior as two consecutive stop signals can't
		 * cause two group stops when !ptraced.  That is why we
		 * also check !task_is_stopped(t) below.
		 *
		 * The condition can be distinguished by testing whether
		 * SIGNAL_STOP_STOPPED is already set.  Don't generate
		 * group_exit_code in such case.
		 *
		 * This is not necessary for SIGNAL_STOP_CONTINUED because
		 * an intervening stop signal is required to cause two
		 * continued events regardless of ptrace.
		 */
		/*
		 * 首次 stop 需广播并建立计数。ptrace 可在组仍停时单独
		 * 恢复某线程，使连续 stop 触发第二轮；已置 STOP_STOPPED 时不重写
		 * group_exit_code。continued 事件天然要求中间出现 stop，无需同样去重。
		 */
		if (!(sig->flags & SIGNAL_STOP_STOPPED))
			sig->group_exit_code = signr;

		sig->group_stop_count = 0;
		if (task_set_jobctl_pending(current, signr | gstop))
			sig->group_stop_count++;

		for_other_threads(current, t) {
			/*
			 * Setting state to TASK_STOPPED for a group
			 * stop is always done with the siglock held,
			 * so this check has no races.
			 */
			/* 所有 group-stop 状态写均持 siglock，因此此处检查稳定。 */
			if (!task_is_stopped(t) &&
			    task_set_jobctl_pending(t, signr | gstop)) {
				sig->group_stop_count++;
				if (likely(!(t->ptrace & PT_SEIZED)))
					signal_wake_up(t, 0);
				else
					ptrace_trap_notify(t);
	/* 组停止状态机分别处理首次发起、各线程到达、计数归零和父进程通知。 */
			}
		}
	}

	if (likely(!current->ptrace)) {
		int notify = 0;

		/*
		 * If there are no other threads in the group, or if there
		 * is a group stop in progress and we are the last to stop,
		 * report to the parent.
		 */
		/* 本线程若是最后一个参与者，负责发出唯一的组停止完成通知。 */
		if (task_participate_group_stop(current))
			notify = CLD_STOPPED;

		current->jobctl |= JOBCTL_STOPPED;
		set_special_state(TASK_STOPPED);
		spin_unlock_irq(&current->sighand->siglock);

		/*
		 * Notify the parent of the group stop completion.  Because
		 * we're not holding either the siglock or tasklist_lock
		 * here, ptracer may attach inbetween; however, this is for
		 * group stop and should always be delivered to the real
		 * parent of the group leader.  The new ptracer will get
		 * its notification when this task transitions into
		 * TASK_TRACED.
		 */
		/*
		 * 组停止属于真实父进程语义，即使解锁后新 tracer attach，
		 * 本通知仍发给组长 real_parent；新 tracer 会在 TASK_TRACED 转换时
		 * 得到自己的独立通知。
		 */
		if (notify) {
			read_lock(&tasklist_lock);
			do_notify_parent_cldstop(current, false, notify);
			read_unlock(&tasklist_lock);
		}

		/* Now we don't run again until woken by SIGCONT or SIGKILL */
		/* TASK_STOPPED 已发布，只有继续或致命信号能使任务恢复。 */
		cgroup_enter_frozen();
		schedule();
		return true;
	} else {
		/*
		 * While ptraced, group stop is handled by STOP trap.
		 * Schedule it and let the caller deal with it.
		 */
		/* 被跟踪任务改挂 TRAP_STOP，由 get_signal 的 ptrace 分支处理。 */
		task_set_jobctl_pending(current, JOBCTL_TRAP_STOP);
		return false;
	}
}

/**
 * do_jobctl_trap - take care of ptrace jobctl traps
 *
 * When PT_SEIZED, it's used for both group stop and explicit
 * SEIZE/INTERRUPT traps.  Both generate PTRACE_EVENT_STOP trap with
 * accompanying siginfo.  If stopped, lower eight bits of exit_code contain
 * the stop signal; otherwise, %SIGTRAP.
 *
 * When !PT_SEIZED, it's used only for group stop trap with stop signal
 * number as exit_code and no siginfo.
 *
 * CONTEXT:
 * Must be called with @current->sighand->siglock held, which may be
 * released and re-acquired before returning with intervening sleep.
 */
/*
 * PT_SEIZED 将 group-stop 与 PTRACE_INTERRUPT 都编码为
 * PTRACE_EVENT_STOP；若没有真实 stop 进行则用 SIGTRAP。旧 attach 模式
 * 只需要传统 stop signal，不附 siginfo。
 */
static void do_jobctl_trap(void)
{
	struct signal_struct *signal = current->signal;
	int signr = current->jobctl & JOBCTL_STOP_SIGMASK;

	if (current->ptrace & PT_SEIZED) {
		if (!signal->group_stop_count &&
		    !(signal->flags & SIGNAL_STOP_STOPPED))
			signr = SIGTRAP;
		WARN_ON_ONCE(!signr);
	/* ptrace 与组停止的通知对象不同，必须在重新取得 siglock 后复核 jobctl 状态。 */
		ptrace_do_notify(signr, signr | (PTRACE_EVENT_STOP << 8),
				 CLD_STOPPED, 0);
	} else {
		WARN_ON_ONCE(!signr);
		ptrace_stop(signr, CLD_STOPPED, 0, NULL);
	}
}

/**
 * do_freezer_trap - handle the freezer jobctl trap
 *
 * Puts the task into frozen state, if only the task is not about to quit.
 * In this case it drops JOBCTL_TRAP_FREEZE.
 *
 * CONTEXT:
 * Must be called with @current->sighand->siglock held,
 * which is always released before returning.
 */
/*
 * 中文学习注释：freezer trap 只在没有更高优先级 jobctl/fatal 事件时睡眠；
 * 否则解锁返回让主循环先处理它们。入睡前清 TIF_SIGPENDING，避免普通非
 * 致命信号令 schedule 立即返回；醒来运行 task_work，主循环随后重试。
 */
static void do_freezer_trap(void)
	__releases(&current->sighand->siglock)
{
	/*
	 * If there are other trap bits pending except JOBCTL_TRAP_FREEZE,
	 * let's make another loop to give it a chance to be handled.
	 * In any case, we'll return back.
	 */
	/*
	 * freezer 不能越过其他 jobctl trap；存在更高优先级事件时先
	 * 解锁返回主循环，让下一轮处理后再决定是否冻结。
	 */
	if ((current->jobctl & (JOBCTL_PENDING_MASK | JOBCTL_TRAP_FREEZE)) !=
	     JOBCTL_TRAP_FREEZE) {
		spin_unlock_irq(&current->sighand->siglock);
		return;
	}

	/*
	 * Now we're sure that there is no pending fatal signal and no
	 * pending traps. Clear TIF_SIGPENDING to not get out of schedule()
	 * immediately (if there is a non-fatal signal pending), and
	 * put the task into sleep.
	 */
	/*
	 * 确认无致命/trap 后才发布 FREEZABLE 睡眠，并清 TIF 避免
	 * 普通非致命 pending 令 schedule 立即返回；新事件仍可通过唤醒协议打断。
	 */
	__set_current_state(TASK_INTERRUPTIBLE|TASK_FREEZABLE);
	clear_thread_flag(TIF_SIGPENDING);
	spin_unlock_irq(&current->sighand->siglock);
	cgroup_enter_frozen();
	schedule();

	/*
	 * We could've been woken by task_work, run it to clear
	 * TIF_NOTIFY_SIGNAL. The caller will retry if necessary.
	 */
	/* task_work 也能唤醒冻结任务；立即执行并清通知位，外层再重试。 */
	clear_notify_signal();
	if (unlikely(task_work_pending(current)))
		task_work_run();
}

/* ptrace_signal()：让 tracer 审核已出队信号，并处理取消、改号或重新排队。 */
static int ptrace_signal(int signr, kernel_siginfo_t *info, enum pid_type type)
{
	/*
	 * 把已出队信号交给 tracer 决定取消、改号或继续。预先置
	 * STOP_DEQUEUED，覆盖 tracer 把任意信号改成 stop 的情况；若改号则
	 * 重建来自 tracer 的 SI_USER 信息。新信号被屏蔽或已有致命信号时重新
	 * 入原类型队列，返回 0 表示本轮不递送。
	 */
	/*
	 * We do not check sig_kernel_stop(signr) but set this marker
	 * unconditionally because we do not know whether debugger will
	 * change signr. This flag has no meaning unless we are going
	 * to stop after return from ptrace_stop(). In this case it will
	 * be checked in do_signal_stop(), we should only stop if it was
	 * not cleared by SIGCONT while we were sleeping. See also the
	 * comment in dequeue_signal().
	 */
	/*
	 * tracer 可把任意信号改成 stop，故无条件记录“已出队”窗口；
	 * 真正 stop 前 do_signal_stop() 会验证该位是否已被睡眠期间的 SIGCONT
	 * 清除，从而避免执行一个已经取消的停止请求。
	 */
	current->jobctl |= JOBCTL_STOP_DEQUEUED;
	signr = ptrace_stop(signr, CLD_TRAPPED, 0, info);

	/* We're back.  Did the debugger cancel the sig?  */
	/* 调试器以返回 0 丢弃信号，本轮不再递送。 */
	if (signr == 0)
		return signr;

	/*
	 * Update the siginfo structure if the signal has
	 * changed.  If the debugger wanted something
	 * specific in the siginfo structure then it should
	 * have updated *info via PTRACE_SETSIGINFO.
	 */
	/*
	 * 只改信号号时重建保守 SI_USER 信息；若调试器需要定制
	 * payload，应通过 PTRACE_SETSIGINFO 明确写入，内核不猜测旧 union 布局。
	 */
	if (signr != info->si_signo) {
		clear_siginfo(info);
		info->si_signo = signr;
		info->si_errno = 0;
		info->si_code = SI_USER;
		rcu_read_lock();
	/* ptracer 可改写或取消信号；恢复后必须重验 blocked、siginfo 来源和实际递送 signo。 */
		info->si_pid = task_pid_vnr(current->parent);
		info->si_uid = from_kuid_munged(current_user_ns(),
						task_uid(current->parent));
		rcu_read_unlock();
	}

	/* If the (new) signal is now blocked, requeue it.  */
	/* 改号后若被屏蔽或已有致命信号，按原队列类型重新入队。 */
	if (sigismember(&current->blocked, signr) ||
	    fatal_signal_pending(current)) {
		send_signal_locked(signr, info, current, type);
		signr = 0;
	}

	return signr;
}

/* hide_si_addr_tag_bits()：按架构规则清理向用户暴露的 fault 地址标签位。 */
static void hide_si_addr_tag_bits(struct ksignal *ksig)
{
	/*
	 * 除非 SA_EXPOSE_TAGBITS，向用户暴露 fault 地址前按架构规则去除地址
	 * tag。只处理 siginfo union 中确实含 si_addr 的布局，避免破坏其他字段。
	 */
	switch (siginfo_layout(ksig->sig, ksig->info.si_code)) {
	case SIL_FAULT:
	case SIL_FAULT_TRAPNO:
	case SIL_FAULT_MCEERR:
	case SIL_FAULT_BNDERR:
	case SIL_FAULT_PKUERR:
	case SIL_FAULT_PERF_EVENT:
		ksig->info.si_addr = arch_untagged_si_addr(
			ksig->info.si_addr, ksig->sig, ksig->info.si_code);
		break;
	/* 非 fault layout 不含可脱 tag 的地址成员，保持 payload 原样。 */
	case SIL_KILL:
	case SIL_TIMER:
	case SIL_POLL:
	case SIL_CHLD:
	case SIL_RT:
	case SIL_SYS:
		break;
	}
}

bool get_signal(struct ksignal *ksig)
{
	/*
	 * 用户返回前的信号总调度器。优先级：
	 * task_work/freezer -> 延迟父通知 -> group-exit/exec -> jobctl stop/trap
	 * -> 同步 fault -> 普通 pending -> ptrace -> disposition。
	 *
	 * 返回 true 表示架构层应构造用户 signal frame，ksig 已含 handler 与
	 * siginfo；返回 false 表示无可递送信号。默认致命动作在本函数内直接
	 * core dump/group exit，不返回用户态。主循环持 siglock 操作共享状态，
	 * 需要睡眠、tasklist_lock 或 core dump 时按注释明确释放并重新获取。
	 */
	struct sighand_struct *sighand = current->sighand;
	struct signal_struct *signal = current->signal;
	int signr;

	clear_notify_signal();
	if (unlikely(task_work_pending(current)))
		task_work_run();

	if (!task_sigpending(current))
		return false;

	if (unlikely(uprobe_deny_signal()))
		return false;

	/*
	 * Do this once, we can't return to user-mode if freezing() == T.
	 * do_signal_stop() and ptrace_stop() set TASK_STOPPED/TASK_TRACED
	 * and the freezer handles those states via TASK_FROZEN, thus they
	 * do not need another check after return.
	 */
	/*
	 * 返回用户态前必须兑现 freezer；stop/ptrace 路径会由 freezer
	 * 把其特殊状态转换为 TASK_FROZEN，因此从这些睡眠恢复后无需重复检查。
	 */
	try_to_freeze();

relock:
	spin_lock_irq(&sighand->siglock);

	/*
	 * Every stopped thread goes here after wakeup. Check to see if
	 * we should notify the parent, prepare_signal(SIGCONT) encodes
	 * the CLD_ si_code into SIGNAL_CLD_MASK bits.
	 */
	/*
	 * SIGCONT 生成路径把父通知原因编码进共享 flags；任一醒来的
	 * 线程在 siglock 下唯一消费这些位并负责通知。
	 */
	if (unlikely(signal->flags & SIGNAL_CLD_MASK)) {
		int why;

		if (signal->flags & SIGNAL_CLD_CONTINUED)
			why = CLD_CONTINUED;
		else
			why = CLD_STOPPED;

		signal->flags &= ~SIGNAL_CLD_MASK;

		spin_unlock_irq(&sighand->siglock);

		/*
		 * Notify the parent that we're continuing.  This event is
		 * always per-process and doesn't make whole lot of sense
		 * for ptracers, who shouldn't consume the state via
		 * wait(2) either, but, for backward compatibility, notify
		 * the ptracer of the group leader too unless it's gonna be
		 * a duplicate.
		 */
	/*
	 * continued 是进程级事件，理论上只给真实父进程；为兼容旧
	 * ptrace 行为，组长被重设父关系时也通知 tracer，并避免重复接收。
	 */
		read_lock(&tasklist_lock);
		do_notify_parent_cldstop(current, false, why);

		if (ptrace_reparented(current->group_leader))
			do_notify_parent_cldstop(current->group_leader,
						true, why);
		read_unlock(&tasklist_lock);
	/* 该阶段在 siglock 下领取候选并解释 disposition，默认 stop/fatal 路径再执行组级状态机。 */

		goto relock;
	}

	for (;;) {
		struct k_sigaction *ka;
		enum pid_type type;

		/* Has this task already been marked for death? */
		/* group exit 或 exec 收敛优先于所有普通信号，直接选择 SIGKILL。 */
		if ((signal->flags & SIGNAL_GROUP_EXIT) ||
		     signal->group_exec_task) {
			signr = SIGKILL;
			sigdelset(&current->pending.signal, SIGKILL);
			trace_signal_deliver(SIGKILL, SEND_SIG_NOINFO,
					     &sighand->action[SIGKILL-1]);
			recalc_sigpending();
			/*
			 * implies do_group_exit() or return to PF_USER_WORKER,
			 * no need to initialize ksig->info/etc.
			 */
			/*
			 * 该路径不会构造用户 signal frame；普通任务进入
			 * do_group_exit，PF_USER_WORKER 返回自清理，因此无需填 ksig。
			 */
			goto fatal;
		}

		if (unlikely(current->jobctl & JOBCTL_STOP_PENDING) &&
		    do_signal_stop(0))
			goto relock;

		if (unlikely(current->jobctl &
			     (JOBCTL_TRAP_MASK | JOBCTL_TRAP_FREEZE))) {
			if (current->jobctl & JOBCTL_TRAP_MASK) {
	/* 该阶段在 siglock 下领取候选并解释 disposition，默认 stop/fatal 路径再执行组级状态机。 */
				do_jobctl_trap();
				spin_unlock_irq(&sighand->siglock);
			} else if (current->jobctl & JOBCTL_TRAP_FREEZE)
				do_freezer_trap();

			goto relock;
		}

		/*
		 * If the task is leaving the frozen state, let's update
		 * cgroup counters and reset the frozen bit.
		 */
	/* 离开冻结态前先在解锁区更新 cgroup 记账，再回到信号选择。 */
		if (unlikely(cgroup_task_frozen(current))) {
			spin_unlock_irq(&sighand->siglock);
			cgroup_leave_frozen(false);
			goto relock;
		}

		/*
		 * Signals generated by the execution of an instruction
		 * need to be delivered before any other pending signals
		 * so that the instruction pointer in the signal stack
		 * frame points to the faulting instruction.
		 */
		/* 同步 fault 优先可保证用户帧 IP 仍对应真正故障指令。 */
		type = PIDTYPE_PID;
		signr = dequeue_synchronous_signal(&ksig->info);
		if (!signr)
			signr = dequeue_signal(&current->blocked, &ksig->info, &type);

		if (!signr)
			break; /* will return 0 */
			/* 没有可递送信号时退出循环，最终向架构层返回 false。 */

		if (unlikely(current->ptrace) && (signr != SIGKILL) &&
		    !(sighand->action[signr -1].sa.sa_flags & SA_IMMUTABLE)) {
			signr = ptrace_signal(signr, &ksig->info, type);
			if (!signr)
				continue;
		}

		ka = &sighand->action[signr-1];

		/* Trace actually delivered signals. */
		/* 只记录经过 ptrace 审核、即将执行 disposition 的真实递送。 */
		trace_signal_deliver(signr, &ksig->info, ka);

		if (ka->sa.sa_handler == SIG_IGN) /* Do nothing.  */
			/* 显式忽略不产生用户帧，继续选择下一信号。 */
			continue;
		if (ka->sa.sa_handler != SIG_DFL) {
			/* Run the handler.  */
			/* 复制 action 快照交给架构层构造用户 handler 帧。 */
			ksig->ka = *ka;

			if (ka->sa.sa_flags & SA_ONESHOT)
				ka->sa.sa_handler = SIG_DFL;

			break; /* will return non-zero "signr" value */
			/* 正信号号使本函数返回 true，通知架构层开始递送。 */
		}

		/*
		 * Now we are doing the default action for this signal.
		 */
		/* 默认动作分为忽略、保护 namespace init、停止、致命/core 四类。 */
		if (sig_kernel_ignore(signr)) /* Default is nothing. */
			/* 默认忽略类信号已完成处理，继续扫描其他 pending。 */
			continue;

		/*
		 * Global init gets no signals it doesn't want.
		 * Container-init gets no signals it doesn't want from same
		 * container.
		 *
		 * Note that if global/container-init sees a sig_kernel_only()
		 * signal here, the signal must have been generated internally
		 * or must have come from an ancestor namespace. In either
		 * case, the signal cannot be dropped.
		 */
		/*
		 * 全局/容器 init 默认免受同 namespace 的非内核专用信号；
		 * kernel-only 信号必来自内核或祖先 namespace，属于不可丢弃的控制语义。
		 */
		if (unlikely(signal->flags & SIGNAL_UNKILLABLE) &&
				!sig_kernel_only(signr))
			continue;

		if (sig_kernel_stop(signr)) {
			/*
			 * The default action is to stop all threads in
			 * the thread group.  The job control signals
			 * do nothing in an orphaned pgrp, but SIGSTOP
			 * always works.  Note that siglock needs to be
			 * dropped during the call to is_orphaned_pgrp()
			 * because of lock ordering with tasklist_lock.
			 * This allows an intervening SIGCONT to be posted.
			 * We need to check for that and bail out if necessary.
			 */
			/*
			 * 作业控制 stop 对孤儿进程组无效，SIGSTOP 例外。
			 * 检查孤儿组需按锁序暂放 siglock；窗口内 SIGCONT 可取消 stop，
			 * 因此重取锁后必须重新验证。
			 */
			if (signr != SIGSTOP) {
				spin_unlock_irq(&sighand->siglock);

				/* signals can be posted during this window */
				/* 解锁窗口允许并发发送，后续不得沿用旧 pending 判断。 */

				if (is_current_pgrp_orphaned())
					goto relock;

				spin_lock_irq(&sighand->siglock);
			}

			if (likely(do_signal_stop(signr))) {
				/* It released the siglock.  */
				/* true 表示已经睡眠并释放锁，回主循环重建状态。 */
				goto relock;
			}

			/*
			 * We didn't actually stop, due to a race
			 * with SIGCONT or something like that.
			 */
			/* 停止请求已被继续/退出竞态取消，本次信号视为消费完成。 */
			continue;
		}

	fatal:
		/* 致命慢路径离开 siglock；core dump 会协调并终止线程组其他成员。 */
		spin_unlock_irq(&sighand->siglock);
		if (unlikely(cgroup_task_frozen(current)))
			cgroup_leave_frozen(true);

		/*
		 * Anything else is fatal, maybe with a core dump.
		 */
		/* 其余默认动作均终止线程组，部分信号还先协调 core dump。 */
		current->flags |= PF_SIGNALED;

		if (sig_kernel_coredump(signr)) {
			if (print_fatal_signals)
				print_fatal_signal(signr);
			proc_coredump_connector(current);
			/*
			 * If it was able to dump core, this kills all
			 * other threads in the group and synchronizes with
			 * their demise.  If we lost the race with another
			 * thread getting here, it set group_exit_code
			 * first and our do_group_exit call below will use
			 * that value and ignore the one we pass it.
			 */
			/*
			 * core 协议会终止并等待其他线程；若另一线程先发布
			 * group_exit_code，后续退出必须沿用它，不能覆盖既定退出原因。
			 */
			vfs_coredump(&ksig->info);
		}

		/*
		 * PF_USER_WORKER threads will catch and exit on fatal signals
		 * themselves. They have cleanup that must be performed, so we
		 * cannot call do_exit() on their behalf. Note that ksig won't
		 * be properly initialized, PF_USER_WORKER's shouldn't use it.
		 */
		/*
		 * PF_USER_WORKER 有必须自行执行的清理，通用路径不能代为
		 * do_exit；此分支不会完整填写 ksig，worker 不得读取其中字段。
		 */
		if (current->flags & PF_USER_WORKER)
			goto out;

		/*
		 * Death signals, no core dump.
		 */
		/* 无需 core 的致命信号直接进入线程组退出。 */
		do_group_exit(signr);
		/* NOTREACHED */
		/* do_group_exit() 不返回，后续是控制流形式上的不可达区域。 */
	}
	spin_unlock_irq(&sighand->siglock);

	ksig->sig = signr;

	if (signr && !(ksig->ka.sa.sa_flags & SA_EXPOSE_TAGBITS))
		hide_si_addr_tag_bits(ksig);
out:
	return signr > 0;
}

/**
 * signal_delivered - called after signal delivery to update blocked signals
 * @ksig:		kernel signal struct
 * @stepping:		nonzero if debugger single-step or block-step in use
 *
 * This function should be called when a signal has successfully been
 * delivered. It updates the blocked signals accordingly (@ksig->ka.sa.sa_mask
 * is always blocked), and the signal itself is blocked unless %SA_NODEFER
 * is set in @ksig->ka.sa.sa_flags.  Tracing is notified.
 */
/*
 * 中文学习注释：架构成功把 frame 写到用户栈后才调用。旧 mask 已保存在
 * frame 中，故清 restore 标记；新 mask = 当前 mask | sa_mask，并按
 * SA_NODEFER 决定是否屏蔽当前信号。SS_AUTODISARM 在首次使用后关闭备用栈。
 */
static void signal_delivered(struct ksignal *ksig, int stepping)
{
	sigset_t blocked;

	/* A signal was successfully delivered, and the
	   saved sigmask was stored on the signal frame,
	   and will be restored by sigreturn.  So we can
	   simply clear the restore sigmask flag.  */
	/* 旧 mask 已保存到用户帧并由 sigreturn 恢复，此处清延迟恢复标志。 */
	clear_restore_sigmask();

	sigorsets(&blocked, &current->blocked, &ksig->ka.sa.sa_mask);
	if (!(ksig->ka.sa.sa_flags & SA_NODEFER))
		sigaddset(&blocked, ksig->sig);
	set_current_blocked(&blocked);
	if (current->sas_ss_flags & SS_AUTODISARM)
		sas_ss_reset(current);
	if (stepping)
		ptrace_notify(SIGTRAP, 0);
}

void signal_setup_done(int failed, struct ksignal *ksig, int stepping)
{
	/* frame 构造失败强制 SIGSEGV；成功才提交 blocked/altstack 状态变化。 */
	if (failed)
		force_sigsegv(ksig->sig);
	else
		signal_delivered(ksig, stepping);
}

/*
 * It could be that complete_signal() picked us to notify about the
 * group-wide signal. Other threads should be notified now to take
 * the shared signals in @which since we will not.
 */
/*
 * 中文学习注释：当前线程即将新增屏蔽集合时，原先被它“代表”唤醒的共享
 * 信号可能无人处理。扫描其他存活线程，按各自 blocked 集合逐步扣除可接管
 * 的信号，并在必要时唤醒；只处理 shared_pending 与 @which 的交集。
 */
static void retarget_shared_pending(struct task_struct *tsk, sigset_t *which)
{
	sigset_t retarget;
	struct task_struct *t;

	sigandsets(&retarget, &tsk->signal->shared_pending.signal, which);
	if (sigisemptyset(&retarget))
		return;

	/* 逐线程扣除其可接管集合；集合清空即可停止扫描，避免无效唤醒。 */
	for_other_threads(tsk, t) {
		if (t->flags & PF_EXITING)
			continue;

		if (!has_pending_signals(&retarget, &t->blocked))
			continue;
		/* Remove the signals this thread can handle. */
		/* 扣除该线程可接管的信号，剩余集合继续寻找消费者。 */
		sigandsets(&retarget, &retarget, &t->blocked);

		if (!task_sigpending(t))
			signal_wake_up(t, 0);

		if (sigisemptyset(&retarget))
			break;
	}
}

/* exit_signals()：将退出线程从共享信号与 group-stop 接收状态中安全撤出。 */
void exit_signals(struct task_struct *tsk)
{
	/*
	 * 线程退出前从线程组信号接收者集合撤出。先用 cgroup 的 threadgroup
	 * change 序列稳定成员关系，再在 siglock 下置 PF_EXITING；若共享 pending
	 * 原本指向本线程，retarget 给其他未屏蔽线程。退出线程仍需消费自己的
	 * group-stop 计数，必要时在解锁后通知真实父进程。
	 */
	int group_stop = 0;
	sigset_t unblocked;

	/*
	 * @tsk is about to have PF_EXITING set - lock out users which
	 * expect stable threadgroup.
	 */
	/* 先阻止依赖稳定线程组的 cgroup 操作，再发布 PF_EXITING。 */
	cgroup_threadgroup_change_begin(tsk);

	if (thread_group_empty(tsk) || (tsk->signal->flags & SIGNAL_GROUP_EXIT)) {
		tsk->flags |= PF_EXITING;
		cgroup_threadgroup_change_end(tsk);
		return;
	}

	spin_lock_irq(&tsk->sighand->siglock);
	/*
	 * From now this task is not visible for group-wide signals,
	 * see wants_signal(), do_signal_stop().
	 */
	/* PF_EXITING 后，本线程不再作为共享信号或组停止参与者。 */
	tsk->flags |= PF_EXITING;

	cgroup_threadgroup_change_end(tsk);

	if (!task_sigpending(tsk))
		goto out;

	unblocked = tsk->blocked;
	signotset(&unblocked);
	retarget_shared_pending(tsk, &unblocked);
	/* 退出线程在 siglock/tasklist 协议下退出信号接收、处理组停止计数并重定向共享 pending。 */

	if (unlikely(tsk->jobctl & JOBCTL_STOP_PENDING) &&
	    task_participate_group_stop(tsk))
		group_stop = CLD_STOPPED;
out:
	spin_unlock_irq(&tsk->sighand->siglock);

	/*
	 * If group stop has completed, deliver the notification.  This
	 * should always go to the real parent of the group leader.
	 */
	/* 退出线程若完成组停止计数，只通知组长的真实父进程。 */
	if (unlikely(group_stop)) {
		read_lock(&tasklist_lock);
		do_notify_parent_cldstop(tsk, false, group_stop);
		read_unlock(&tasklist_lock);
	}
}

/*
 * System call entry points.
 */
/*
 * 中文学习导览：下半文件把上述内核状态机封装为用户 ABI，并集中处理
 * usercopy、32 位兼容布局、不可屏蔽信号以及 pid/user namespace 转换。
 */

/**
 *  sys_restart_syscall - restart a system call
 */
/* 执行先前由中断系统调用写入 current->restart_block 的回调。 */
SYSCALL_DEFINE0(restart_syscall)
{
	struct restart_block *restart = &current->restart_block;
	return restart->fn(restart);
}

long do_no_restart_syscall(struct restart_block *param)
{
	/* 明确禁止重启的占位回调，统一返回 EINTR。 */
	return -EINTR;
}

static void __set_task_blocked(struct task_struct *tsk, const sigset_t *newset)
{
	/*
	 * 更新 mask 前先把“新近被屏蔽”的共享 pending 重新定向，否则发送路径
	 * 可能只唤醒本线程，而它换 mask 后再也不会消费。最后重算 TIF。
	 */
	if (task_sigpending(tsk) && !thread_group_empty(tsk)) {
		sigset_t newblocked;
		/* A set of now blocked but previously unblocked signals. */
		/* newblocked 是本次新增屏蔽项，供共享信号重新定向。 */
		sigandnsets(&newblocked, newset, &current->blocked);
		retarget_shared_pending(tsk, &newblocked);
	}
	tsk->blocked = *newset;
	recalc_sigpending();
}

/**
 * set_current_blocked - change current->blocked mask
 * @newset: new mask
 *
 * It is wrong to change ->blocked directly, this helper should be used
 * to ensure the process can't miss a shared signal we are going to block.
 */
/* 用户语义永远不能屏蔽 SIGKILL/SIGSTOP，再交给锁内 helper。 */
void set_current_blocked(sigset_t *newset)
{
	sigdelsetmask(newset, sigmask(SIGKILL) | sigmask(SIGSTOP));
	__set_current_blocked(newset);
}

void __set_current_blocked(const sigset_t *newset)
{
	/* 只有 current 写 blocked，可无锁快速比较；真实变更在 siglock 下提交。 */
	struct task_struct *tsk = current;

	/*
	 * In case the signal mask hasn't changed, there is nothing we need
	 * to do. The current->blocked shouldn't be modified by other task.
	 */
	/* blocked 只有 current 写，集合相同时可无锁快速返回。 */
	if (sigequalsets(&tsk->blocked, newset))
		return;

	spin_lock_irq(&tsk->sighand->siglock);
	__set_task_blocked(tsk, newset);
	spin_unlock_irq(&tsk->sighand->siglock);
}

/*
 * This is also useful for kernel threads that want to temporarily
 * (or permanently) block certain signals.
 *
 * NOTE! Unlike the user-mode sys_sigprocmask(), the kernel
 * interface happily blocks "unblockable" signals like SIGKILL
 * and friends.
 */
/*
 * 中文学习注释：内核 API 与用户 syscall 不同，刻意允许屏蔽 SIGKILL/
 * SIGSTOP；返回旧 mask 可支持成对临时修改。最终仍走共享信号重定向逻辑。
 */
int sigprocmask(int how, sigset_t *set, sigset_t *oldset)
{
	struct task_struct *tsk = current;
	sigset_t newset;

	/* Lockless, only current can change ->blocked, never from irq */
	/* current 独占写 blocked，且中断不修改，可安全无锁取旧值。 */
	if (oldset)
		*oldset = tsk->blocked;

	switch (how) {
	case SIG_BLOCK:
		sigorsets(&newset, &tsk->blocked, set);
		break;
	case SIG_UNBLOCK:
		sigandnsets(&newset, &tsk->blocked, set);
		break;
	case SIG_SETMASK:
		/* SETMASK 直接采用输入；前两分支分别完成集合并/差。 */
		newset = *set;
		break;
	default:
		return -EINVAL;
	}

	__set_current_blocked(&newset);
	return 0;
}
EXPORT_SYMBOL(sigprocmask);

/*
 * The api helps set app-provided sigmasks.
 *
 * This is useful for syscalls such as ppoll, pselect, io_pgetevents and
 * epoll_pwait where a new sigmask is passed from userland for the syscalls.
 *
 * Note that it does set_restore_sigmask() in advance, so it must be always
 * paired with restore_saved_sigmask_unless() before return from syscall.
 */
/*
 * 中文学习注释：pselect/ppoll 等原子“换 mask + 等待”接口的共同入口。
 * usercopy 成功后先保存旧 mask 并置 restore 标记；系统调用退出路径必须
 * 对称恢复，才能封住用户态先改 mask 再 sleep 的竞态窗口。
 */
int set_user_sigmask(const sigset_t __user *umask, size_t sigsetsize)
{
	sigset_t kmask;

	if (!umask)
		return 0;
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;
	if (copy_from_user(&kmask, umask, sizeof(sigset_t)))
		return -EFAULT;
	/* 完整导入后才替换 current mask，用户访存失败不会留下半更新状态。 */

	set_restore_sigmask();
	current->saved_sigmask = current->blocked;
	set_current_blocked(&kmask);

	return 0;
}

#ifdef CONFIG_COMPAT
int set_compat_user_sigmask(const compat_sigset_t __user *umask,
			    size_t sigsetsize)
{
	/* 32 位 ABI 仅转换集合表示，保存/恢复协议与原生路径完全相同。 */
	sigset_t kmask;

	if (!umask)
		return 0;
	if (sigsetsize != sizeof(compat_sigset_t))
		return -EINVAL;
	if (get_compat_sigset(&kmask, umask))
		return -EFAULT;

	/* 转换全部成功后才保存旧 mask 并提交 native 表示，失败路径不改 current。 */
	set_restore_sigmask();
	current->saved_sigmask = current->blocked;
	set_current_blocked(&kmask);

	return 0;
}
#endif

/**
 *  sys_rt_sigprocmask - change the list of currently blocked signals
 *  @how: whether to add, remove, or set signals
 *  @nset: stores pending signals
 *  @oset: previous value of signal mask if non-null
 *  @sigsetsize: size of sigset_t type
 */
/*
 * 先快照旧集合，再导入新集合、剔除不可屏蔽信号并按 how 更新；
 * 最后才复制旧值给用户。usercopy 失败只影响返回值，已提交的新 mask 不回滚，
 * 这是 syscall ABI 的既有语义。
 */
SYSCALL_DEFINE4(rt_sigprocmask, int, how, sigset_t __user *, nset,
		sigset_t __user *, oset, size_t, sigsetsize)
{
	sigset_t old_set, new_set;
	int error;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	/* compat sigprocmask 当前固定大小，未来仍可扩展其他布局。 */
	/* 当前固定大小校验不应永久排除未来的可变尺寸 ABI。 */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	old_set = current->blocked;

	if (nset) {
		if (copy_from_user(&new_set, nset, sizeof(sigset_t)))
			return -EFAULT;
		sigdelsetmask(&new_set, sigmask(SIGKILL)|sigmask(SIGSTOP));

		/* 新集合先剔除 SIGKILL/SIGSTOP，再按 how 一次提交。 */
		error = sigprocmask(how, &new_set, NULL);
		if (error)
			return error;
	}

	if (oset) {
		if (copy_to_user(oset, &old_set, sizeof(sigset_t)))
			return -EFAULT;
	}

	return 0;
}

#ifdef CONFIG_COMPAT
/*
 * compat rt_sigprocmask：把 32 位用户集合转换为内核集合后复用原生语义。
 * @nset/@oset 可空；返回参数/usercopy errno，current->blocked 是唯一副作用。
 */
COMPAT_SYSCALL_DEFINE4(rt_sigprocmask, int, how, compat_sigset_t __user *, nset,
		compat_sigset_t __user *, oset, compat_size_t, sigsetsize)
{
	sigset_t old_set = current->blocked;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	/* compat rt_sigprocmask 暂固定大小，但保留未来扩展余地。 */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (nset) {
		sigset_t new_set;
		int error;
		if (get_compat_sigset(&new_set, nset))
			return -EFAULT;
		sigdelsetmask(&new_set, sigmask(SIGKILL)|sigmask(SIGSTOP));

		/* compat 输入扩展完成后复用原生更新核心，旧集合最后按 32 位格式回写。 */
		error = sigprocmask(how, &new_set, NULL);
		if (error)
			return error;
	}
	return oset ? put_compat_sigset(oset, &old_set, sizeof(*oset)) : 0;
}
#endif

/* do_sigpending()：汇总 current 私有/共享且当前被屏蔽的 pending 信号。 */
/*
 * 契约补充：@set 为调用者提供的输出集合；函数短暂持 current siglock，
 * 不睡眠。返回无；输出仅包含私有/共享 pending 中当前被屏蔽的信号。
 */
static void do_sigpending(sigset_t *set)
{
	/*
	 * 在 siglock 下合并私有与共享 pending 位图；解锁后与 current->blocked
	 * 求交，只向用户报告“因被屏蔽而等待”的信号。
	 */
	spin_lock_irq(&current->sighand->siglock);
	sigorsets(set, &current->pending.signal,
		  &current->signal->shared_pending.signal);
	spin_unlock_irq(&current->sighand->siglock);

	/* Outside the lock because only this thread touches it.  */
	/* blocked 仅由 current 写，合并 pending 后可在锁外求交。 */
	sigandsets(set, &current->blocked, set);
}

/**
 *  sys_rt_sigpending - examine a pending signal that has been raised
 *			while blocked
 *  @uset: stores pending signals
 *  @sigsetsize: size of sigset_t type or larger
 */
/* 允许用户请求不超过内核集合大小的前缀，保留历史 ABI 弹性。 */
SYSCALL_DEFINE2(rt_sigpending, sigset_t __user *, uset, size_t, sigsetsize)
{
	sigset_t set;

	if (sigsetsize > sizeof(*uset))
		return -EINVAL;

	do_sigpending(&set);

	if (copy_to_user(uset, &set, sigsetsize))
		return -EFAULT;

	return 0;
}

#ifdef CONFIG_COMPAT
/*
 * compat rt_sigpending：@uset 为用户输出，@sigsetsize 是请求字节数。
 * 不睡眠于锁内；返回 0/-EINVAL/-EFAULT，不改变 pending 状态。
 */
COMPAT_SYSCALL_DEFINE2(rt_sigpending, compat_sigset_t __user *, uset,
		compat_size_t, sigsetsize)
{
	sigset_t set;

	if (sigsetsize > sizeof(*uset))
		return -EINVAL;

	do_sigpending(&set);

	return put_compat_sigset(uset, &set, sigsetsize);
}
#endif

static const struct {
	unsigned char limit, layout;
} sig_sicodes[] = {
	[SIGILL]  = { NSIGILL,  SIL_FAULT },
	[SIGFPE]  = { NSIGFPE,  SIL_FAULT },
	[SIGSEGV] = { NSIGSEGV, SIL_FAULT },
	[SIGBUS]  = { NSIGBUS,  SIL_FAULT },
	[SIGTRAP] = { NSIGTRAP, SIL_FAULT },
#if defined(SIGEMT)
	[SIGEMT]  = { NSIGEMT,  SIL_FAULT },
#endif
	[SIGCHLD] = { NSIGCHLD, SIL_CHLD },
	[SIGPOLL] = { NSIGPOLL, SIL_POLL },
	[SIGSYS]  = { NSIGSYS,  SIL_SYS },
};
/* 表驱动记录信号专属正 si_code 的上界及其 union 布局。 */

/*
 * 契约补充：@sig/@si_code 为纯值输入，无锁且不睡眠。
 * 返回 true 表示内核完整理解对应 union 布局，false 要求校验扩展区全零。
 */
static bool known_siginfo_layout(unsigned sig, int si_code)
{
	/*
	 * 判断内核是否理解该 signal/code 组合。未知扩展布局仍可接受，但随后
	 * 必须证明用户结构的 expansion 区全零，保证往返复制不丢信息。
	 */
	if (si_code == SI_KERNEL)
		return true;
	else if ((si_code > SI_USER)) {
		if (sig_specific_sicodes(sig)) {
			if (si_code <= sig_sicodes[sig].limit)
				return true;
		}
		/* 未落入信号专属表但属于 poll 范围时使用 POLL union。 */
		else if (si_code <= NSIGPOLL)
			return true;
	}
	else if (si_code >= SI_DETHREAD)
		return true;
	else if (si_code == SI_ASYNCNL)
		return true;
	return false;
}

enum siginfo_layout siginfo_layout(unsigned sig, int si_code)
{
	/*
	 * 把 signal+si_code 映射到 siginfo tagged union 分支。先取通用布局，
	 * 再处理 MCE、BNDERR、PKU、perf 与少数架构 trapno 特例；所有复制和
	 * namespace 字段处理都依赖这个结果，错误布局会把地址当 pid 等。
	 */
	enum siginfo_layout layout = SIL_KILL;
	if ((si_code > SI_USER) && (si_code < SI_KERNEL)) {
		if ((sig < ARRAY_SIZE(sig_sicodes)) &&
		    (si_code <= sig_sicodes[sig].limit)) {
			layout = sig_sicodes[sig].layout;
			/* Handle the exceptions */
			/* 以下 code 在基础 fault 布局上增加各自专属字段。 */
			if ((sig == SIGBUS) &&
			    (si_code >= BUS_MCEERR_AR) && (si_code <= BUS_MCEERR_AO))
				layout = SIL_FAULT_MCEERR;
			else if ((sig == SIGSEGV) && (si_code == SEGV_BNDERR))
				layout = SIL_FAULT_BNDERR;
#ifdef SEGV_PKUERR
			else if ((sig == SIGSEGV) && (si_code == SEGV_PKUERR))
				layout = SIL_FAULT_PKUERR;
#endif
			else if ((sig == SIGTRAP) && (si_code == TRAP_PERF))
				layout = SIL_FAULT_PERF_EVENT;
			else if (IS_ENABLED(CONFIG_SPARC) &&
	/* 正 si_code 走信号专属表及 fault 例外，非正 code 再映射 TIMER、POLL 或 RT 布局。 */
				 (sig == SIGILL) && (si_code == ILL_ILLTRP))
				layout = SIL_FAULT_TRAPNO;
			else if (IS_ENABLED(CONFIG_ALPHA) &&
				 ((sig == SIGFPE) ||
				  ((sig == SIGTRAP) && (si_code == TRAP_UNK))))
				layout = SIL_FAULT_TRAPNO;
		}
		else if (si_code <= NSIGPOLL)
			/* 未落入专属表但属于 poll code 时使用 POLL union。 */
			layout = SIL_POLL;
	} else {
		/* SI_TIMER/SI_SIGIO/负值分别选择 TIMER、POLL 与 RT 通用布局。 */
		if (si_code == SI_TIMER)
			layout = SIL_TIMER;
		else if (si_code == SI_SIGIO)
			layout = SIL_POLL;
		else if (si_code < 0)
			layout = SIL_RT;
	}
	return layout;
}

/* si_expansion()：定位用户 siginfo 公共内核前缀之后的 ABI 扩展区。 */
/*
 * 契约补充：@info 是不解引用的用户地址值；无锁、不睡眠。
 * 返回同一用户对象内核公共前缀之后的借用地址，不检查其可访问性。
 */
static inline char __user *si_expansion(const siginfo_t __user *info)
{
	/* 用户 siginfo 大于内核公共前缀，返回尾部 ABI 扩展区地址。 */
	return ((char __user *)info) + sizeof(struct kernel_siginfo);
}

/*
 * 契约补充：@from 为只读借用，@to 为用户输出缓冲；函数可能 user fault。
 * 成功返回 0 并写前缀、清零扩展区，失败 -EFAULT；输入所有权不变。
 */
int copy_siginfo_to_user(siginfo_t __user *to, const kernel_siginfo_t *from)
{
	/* 复制已知内核前缀并清零扩展区，避免泄露栈数据且保证确定性 ABI。 */
	char __user *expansion = si_expansion(to);
	if (copy_to_user(to, from , sizeof(struct kernel_siginfo)))
		return -EFAULT;
	if (clear_user(expansion, SI_EXPANSION_SIZE))
		return -EFAULT;
	return 0;
}

static int post_copy_siginfo_from_user(kernel_siginfo_t *info,
				       const siginfo_t __user *from)
{
	/*
	 * 未知 si_code 可能定义更大 payload；内核只在尾部全零时接受，这样之后
	 * copy 回用户仍能精确复现。非零未知数据返回 -E2BIG 而非静默截断。
	 */
	if (unlikely(!known_siginfo_layout(info->si_signo, info->si_code))) {
		char __user *expansion = si_expansion(from);
		char buf[SI_EXPANSION_SIZE];
		int i;
		/*
		 * An unknown si_code might need more than
		 * sizeof(struct kernel_siginfo) bytes.  Verify all of the
		 * extra bytes are 0.  This guarantees copy_siginfo_to_user
		 * will return this data to userspace exactly.
		 */
		/* 未知扩展只在尾部全零时接受，避免静默截断且保证往返一致。 */
		if (copy_from_user(&buf, expansion, SI_EXPANSION_SIZE))
			return -EFAULT;
		for (i = 0; i < SI_EXPANSION_SIZE; i++) {
			if (buf[i] != 0)
				return -E2BIG;
		}
	}
	return 0;
}

static int __copy_siginfo_from_user(int signo, kernel_siginfo_t *to,
				    const siginfo_t __user *from)
{
	/* 导入用户结构，但由 syscall 参数强制覆盖 signo，防字段自相矛盾。 */
	if (copy_from_user(to, from, sizeof(struct kernel_siginfo)))
		return -EFAULT;
	to->si_signo = signo;
	return post_copy_siginfo_from_user(to, from);
}

/*
 * 契约补充：@from 为用户输入，@to 为内核输出；可能因 usercopy 睡眠。
 * 成功 0 并完整初始化 @to，失败 -EFAULT/-E2BIG，任何对象所有权均不转移。
 */
int copy_siginfo_from_user(kernel_siginfo_t *to, const siginfo_t __user *from)
{
	/* 保留用户提供 signo 的通用导入版本，并执行 expansion 校验。 */
	if (copy_from_user(to, from, sizeof(struct kernel_siginfo)))
		return -EFAULT;
	return post_copy_siginfo_from_user(to, from);
}

#ifdef CONFIG_COMPAT
/**
 * copy_siginfo_to_external32 - copy a kernel siginfo into a compat user siginfo
 * @to: compat siginfo destination
 * @from: kernel siginfo source
 *
 * Note: This function does not work properly for the SIGCHLD on x32, but
 * fortunately it doesn't have to.  The only valid callers for this function are
 * copy_siginfo_to_user32, which is overriden for x32 and the coredump code.
 * The latter does not care because SIGCHLD will never cause a coredump.
 */
/*
 * 中文学习注释：32 位 siginfo 不能整体 memcpy，必须按 layout 逐字段转换
 * 指针宽度并先清零目标 union。x32 SIGCHLD 时间字段是已知例外，由真正的
 * 用户复制入口覆盖；core dump 不会以 SIGCHLD 触发，故这里可共用。
 */
void copy_siginfo_to_external32(struct compat_siginfo *to,
		const struct kernel_siginfo *from)
{
	memset(to, 0, sizeof(*to));

	to->si_signo = from->si_signo;
	to->si_errno = from->si_errno;
	to->si_code  = from->si_code;
	/*
	 * 阶段：按 tagged union 分支机械缩窄字段。各 case 不改变控制状态或
	 * ownership，只把该布局定义的有效成员复制到已清零目标；分组列举本身
	 * 即完整字段地图，因此中间连续赋值豁免逐段重复说明。
	 */
	switch(siginfo_layout(from->si_signo, from->si_code)) {
	case SIL_KILL:
		to->si_pid = from->si_pid;
		to->si_uid = from->si_uid;
		break;
	case SIL_TIMER:
		to->si_tid     = from->si_tid;
		to->si_overrun = from->si_overrun;
	/* 该段继续完成 tagged union 的 compat 缩窄，未选中的 union 字节保持清零。 */
		to->si_int     = from->si_int;
		break;
	case SIL_POLL:
		to->si_band = from->si_band;
		to->si_fd   = from->si_fd;
		break;
	/* fault 家族的共同地址先缩窄为 compat pointer，再复制各布局扩展。 */
	case SIL_FAULT:
		to->si_addr = ptr_to_compat(from->si_addr);
		break;
	case SIL_FAULT_TRAPNO:
		to->si_addr = ptr_to_compat(from->si_addr);
		to->si_trapno = from->si_trapno;
		break;
	case SIL_FAULT_MCEERR:
		to->si_addr = ptr_to_compat(from->si_addr);
	/* 该段继续完成 tagged union 的 compat 缩窄，未选中的 union 字节保持清零。 */
		to->si_addr_lsb = from->si_addr_lsb;
		break;
	case SIL_FAULT_BNDERR:
		to->si_addr = ptr_to_compat(from->si_addr);
		to->si_lower = ptr_to_compat(from->si_lower);
		to->si_upper = ptr_to_compat(from->si_upper);
		break;
	/* PKU/perf fault 分别携带保护键或 perf 元数据，不能按普通 fault 截断。 */
	case SIL_FAULT_PKUERR:
		to->si_addr = ptr_to_compat(from->si_addr);
		to->si_pkey = from->si_pkey;
		break;
	case SIL_FAULT_PERF_EVENT:
		to->si_addr = ptr_to_compat(from->si_addr);
		/* perf fault 的 data/type/flags 与地址共同构成有效 payload，必须成组缩窄。 */
		to->si_perf_data = from->si_perf_data;
		to->si_perf_type = from->si_perf_type;
		to->si_perf_flags = from->si_perf_flags;
		break;
	case SIL_CHLD:
		/* CHLD 的 pid/uid/status/CPU 时间按 compat 标量宽度逐项投影。 */
		to->si_pid = from->si_pid;
		to->si_uid = from->si_uid;
		to->si_status = from->si_status;
		to->si_utime = from->si_utime;
		to->si_stime = from->si_stime;
		break;
	case SIL_RT:
		/* RT 与 SYS 是最后两类：前者保留整数 payload，后者缩窄调用地址。 */
		to->si_pid = from->si_pid;
		to->si_uid = from->si_uid;
		to->si_int = from->si_int;
		break;
	case SIL_SYS:
		to->si_call_addr = ptr_to_compat(from->si_call_addr);
		/* SIGSYS 的调用地址、系统调用号和审计架构必须作为同一布局复制。 */
		to->si_syscall   = from->si_syscall;
		to->si_arch      = from->si_arch;
		break;
	}
}

/* __copy_siginfo_to_user32()：按 tagged layout 导出并写入 32 位 siginfo。 */
/*
 * 契约补充：@from 为只读内核信息，@to 为 compat 用户输出；先在栈上转换，
 * 再一次复制。成功 0，用户访存失败 -EFAULT；不转移输入所有权。
 */
int __copy_siginfo_to_user32(struct compat_siginfo __user *to,
			   const struct kernel_siginfo *from)
{
	/* 先在内核栈完成布局转换，再一次 usercopy，避免部分字段直接写用户态。 */
	struct compat_siginfo new;

	copy_siginfo_to_external32(&new, from);
	if (copy_to_user(to, &new, sizeof(struct compat_siginfo)))
		return -EFAULT;
	return 0;
}

/*
 * 契约补充：@from 是已稳定的 compat 内核栈副本，@to 为输出；
 * 无 usercopy、无锁且不睡眠。返回 0，并按 tagged layout 完整初始化 @to。
 */
static int post_copy_siginfo_from_user32(kernel_siginfo_t *to,
					 const struct compat_siginfo *from)
{
	/*
	 * 与导出路径对称，按 tagged layout 扩展 32 位指针/字段；目标先清零，
	 * 防止 union 未覆盖字节泄露。x32 的 SIGCHLD 时间布局需运行时分支。
	 */
	clear_siginfo(to);
	to->si_signo = from->si_signo;
	to->si_errno = from->si_errno;
	to->si_code  = from->si_code;
	/*
	 * 阶段：按布局机械扩展 compat 字段。每个 case 只初始化对应 union
	 * 成员，不取引用、不发布状态；连续赋值是表格式转换，豁免重复注释。
	 */
	switch(siginfo_layout(from->si_signo, from->si_code)) {
	case SIL_KILL:
		to->si_pid = from->si_pid;
		to->si_uid = from->si_uid;
		break;
	case SIL_TIMER:
		to->si_tid     = from->si_tid;
		to->si_overrun = from->si_overrun;
	/* 该段继续恢复 compat tagged union，x32 CHLD 时间字段按当前 syscall ABI 特判。 */
		to->si_int     = from->si_int;
		break;
	case SIL_POLL:
		to->si_band = from->si_band;
		to->si_fd   = from->si_fd;
		break;
	/* fault 家族把 compat 地址显式扩展为内核指针，再恢复各扩展字段。 */
	case SIL_FAULT:
		to->si_addr = compat_ptr(from->si_addr);
		break;
	case SIL_FAULT_TRAPNO:
		to->si_addr = compat_ptr(from->si_addr);
		to->si_trapno = from->si_trapno;
		break;
	case SIL_FAULT_MCEERR:
		to->si_addr = compat_ptr(from->si_addr);
	/* 该段继续恢复 compat tagged union，x32 CHLD 时间字段按当前 syscall ABI 特判。 */
		to->si_addr_lsb = from->si_addr_lsb;
		break;
	case SIL_FAULT_BNDERR:
		to->si_addr = compat_ptr(from->si_addr);
		to->si_lower = compat_ptr(from->si_lower);
		to->si_upper = compat_ptr(from->si_upper);
		break;
	/* PKU/perf fault 的专属字段必须与地址一起恢复，保持 tagged layout 完整。 */
	case SIL_FAULT_PKUERR:
		to->si_addr = compat_ptr(from->si_addr);
		to->si_pkey = from->si_pkey;
		break;
	case SIL_FAULT_PERF_EVENT:
		to->si_addr = compat_ptr(from->si_addr);
		/* perf fault 的 data/type/flags 与地址共同恢复为 native 有效 payload。 */
		to->si_perf_data = from->si_perf_data;
		to->si_perf_type = from->si_perf_type;
		to->si_perf_flags = from->si_perf_flags;
		break;
	case SIL_CHLD:
		/* CHLD 在 x32 syscall 下使用独立时间字段布局，其余 compat ABI 走通用槽。 */
		to->si_pid    = from->si_pid;
		to->si_uid    = from->si_uid;
		to->si_status = from->si_status;
#ifdef CONFIG_X86_X32_ABI
		if (in_x32_syscall()) {
			to->si_utime = from->_sifields._sigchld_x32._utime;
			to->si_stime = from->_sifields._sigchld_x32._stime;
		} else
#endif
		/* 非 x32 路径从通用 compat CHLD 槽恢复时间字段。 */
		{
			to->si_utime = from->si_utime;
			to->si_stime = from->si_stime;
		}
		break;
	case SIL_RT:
		/* RT/SYS 收尾分别恢复整数 payload 与 syscall 地址/编号/架构。 */
		to->si_pid = from->si_pid;
		to->si_uid = from->si_uid;
		to->si_int = from->si_int;
		break;
	case SIL_SYS:
		to->si_call_addr = compat_ptr(from->si_call_addr);
		to->si_syscall   = from->si_syscall;
		/* syscall 号与 arch 完成 SIGSYS payload 的最后两个标量字段。 */
		to->si_arch      = from->si_arch;
		break;
	}
	return 0;
}

/* __copy_siginfo_from_user32()：导入 compat siginfo 并强制采用 syscall signo。 */
/*
 * 契约补充：从 @ufrom 导入 compat 信息并用 @signo 覆盖用户信号号。
 * @to 为输出；成功 0，usercopy 失败 -EFAULT，所有权均不转移。
 */
static int __copy_siginfo_from_user32(int signo, struct kernel_siginfo *to,
				      const struct compat_siginfo __user *ufrom)
{
	/* syscall 指定信号号的 compat 导入版本。 */
	struct compat_siginfo from;

	if (copy_from_user(&from, ufrom, sizeof(struct compat_siginfo)))
		return -EFAULT;

	from.si_signo = signo;
	return post_copy_siginfo_from_user32(to, &from);
}

/*
 * 契约补充：从 compat 用户结构完整导入 @to，并保留其中 signo。
 * 可能因用户缺页睡眠；返回 0/-EFAULT，输出仅在成功路径可依赖。
 */
int copy_siginfo_from_user32(struct kernel_siginfo *to,
			     const struct compat_siginfo __user *ufrom)
{
	/* 完全采用 compat 用户结构中信号号的导入版本。 */
	struct compat_siginfo from;

	if (copy_from_user(&from, ufrom, sizeof(struct compat_siginfo)))
		return -EFAULT;

	return post_copy_siginfo_from_user32(to, &from);
}
#endif /* CONFIG_COMPAT */
/* 至此结束 32 位 siginfo 布局转换实现。 */

/**
 *  do_sigtimedwait - wait for queued signals specified in @which
 *  @which: queued signals to wait for
 *  @info: if non-null, the signal's siginfo is returned here
 *  @ts: upper bound on process time suspension
 */
/*
 * 中文学习注释：
 * - 将“要等待的集合”取反为 dequeue 所需的 blocked mask，并排除不可捕获
 *   信号；先在锁内尝试零等待出队。
 * - 无信号时把感兴趣集合临时从 blocked 中移除，同时把真实 mask 保存到
 *   real_blocked，使发送路径能唤醒本任务；定时/被唤醒后在锁内恢复并再
 *   出队一次，封住信号到达与睡眠之间的竞态。
 * - 返回正信号号、超时 -EAGAIN、其他唤醒 -EINTR。
 */
static int do_sigtimedwait(const sigset_t *which, kernel_siginfo_t *info,
		    const struct timespec64 *ts)
{
	ktime_t *to = NULL, timeout = KTIME_MAX;
	struct task_struct *tsk = current;
	sigset_t mask = *which;
	enum pid_type type;
	int sig, ret = 0;

	/*
	 * 阶段 1：校验可选超时并转换成 hrtimer 相对时间。
	 * @ts 为 NULL 表示无限等待；零时长保留为只轮询一次。
	 */
	if (ts) {
		if (!timespec64_valid(ts))
			return -EINVAL;
		timeout = timespec64_to_ktime(*ts);
		to = &timeout;
	}

	/*
	 * Invert the set of allowed signals to get those we want to block.
	 */
	/* dequeue 接口接收 blocked mask，因此把等待集合取反。 */
	sigdelsetmask(&mask, sigmask(SIGKILL) | sigmask(SIGSTOP));
	signotset(&mask);

	spin_lock_irq(&tsk->sighand->siglock);
	/* 阶段 2：在改变屏蔽集合前先尝试直接消费已有目标信号。 */
	sig = dequeue_signal(&mask, info, &type);
	if (!sig && timeout) {
		/*
		 * None ready, temporarily unblock those we're interested
		 * while we are sleeping in so that we'll be awakened when
		 * they arrive. Unblocking is always fine, we can avoid
		 * set_current_blocked().
		 */
		/* 临时解除目标信号以确保到达时唤醒；旧 mask 由 real_blocked 保存。 */
		tsk->real_blocked = tsk->blocked;
		sigandsets(&tsk->blocked, &tsk->blocked, &mask);
		recalc_sigpending();
		spin_unlock_irq(&tsk->sighand->siglock);

		__set_current_state(TASK_INTERRUPTIBLE|TASK_FREEZABLE);
		/* 阶段 3：锁外按相对超时睡眠，发送者通过 TIF/wakeup 结束等待。 */
		ret = schedule_hrtimeout_range(to, tsk->timer_slack_ns,
					       HRTIMER_MODE_REL);
		spin_lock_irq(&tsk->sighand->siglock);
		/* 阶段 4：锁内恢复真实 mask，再消费唤醒本次等待的信号。 */
		__set_task_blocked(tsk, &tsk->real_blocked);
		sigemptyset(&tsk->real_blocked);
		sig = dequeue_signal(&mask, info, &type);
	}
	spin_unlock_irq(&tsk->sighand->siglock);

	if (sig)
		return sig;
	/* 超时没有信号是 EAGAIN；被其他事件打断则按 EINTR 报告。 */
	return ret ? -EINTR : -EAGAIN;
}

/**
 *  sys_rt_sigtimedwait - synchronously wait for queued signals specified
 *			in @uthese
 *  @uthese: queued signals to wait for
 *  @uinfo: if non-null, the signal's siginfo is returned here
 *  @uts: upper bound on process time suspension
 *  @sigsetsize: size of sigset_t type
 */
/* 同步等待 @uthese；@uinfo 可输出详情，@uts 限定相对等待时间。 */
SYSCALL_DEFINE4(rt_sigtimedwait, const sigset_t __user *, uthese,
		siginfo_t __user *, uinfo,
		const struct __kernel_timespec __user *, uts,
		size_t, sigsetsize)
{
	sigset_t these;
	struct timespec64 ts;
	kernel_siginfo_t info;
	int ret;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	/* 当前 ABI 固定集合大小，但保留未来兼容其他尺寸的可能。 */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&these, uthese, sizeof(these)))
		return -EFAULT;

	if (uts) {
		if (get_timespec64(&ts, uts))
			return -EFAULT;
	}

	ret = do_sigtimedwait(&these, &info, uts ? &ts : NULL);

	if (ret > 0 && uinfo) {
		if (copy_siginfo_to_user(uinfo, &info))
			ret = -EFAULT;
	}

	return ret;
}

#ifdef CONFIG_COMPAT_32BIT_TIME
SYSCALL_DEFINE4(rt_sigtimedwait_time32, const sigset_t __user *, uthese,
		siginfo_t __user *, uinfo,
		const struct old_timespec32 __user *, uts,
		size_t, sigsetsize)
{
	sigset_t these;
	struct timespec64 ts;
	kernel_siginfo_t info;
	int ret;

	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&these, uthese, sizeof(these)))
		return -EFAULT;

	if (uts) {
		if (get_old_timespec32(&ts, uts))
			return -EFAULT;
	}

	ret = do_sigtimedwait(&these, &info, uts ? &ts : NULL);

	if (ret > 0 && uinfo) {
		if (copy_siginfo_to_user(uinfo, &info))
			ret = -EFAULT;
	}

	return ret;
}
#endif

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE4(rt_sigtimedwait_time64, compat_sigset_t __user *, uthese,
		struct compat_siginfo __user *, uinfo,
		struct __kernel_timespec __user *, uts, compat_size_t, sigsetsize)
{
	sigset_t s;
	struct timespec64 t;
	kernel_siginfo_t info;
	long ret;

	/* compat 集合、64 位 timespec 与 siginfo 分别转换，成功等待后才回写 payload。 */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (get_compat_sigset(&s, uthese))
		return -EFAULT;

	if (uts) {
		if (get_timespec64(&t, uts))
			return -EFAULT;
	}

	ret = do_sigtimedwait(&s, &info, uts ? &t : NULL);

	if (ret > 0 && uinfo) {
		if (copy_siginfo_to_user32(uinfo, &info))
			ret = -EFAULT;
	}

	return ret;
}

#ifdef CONFIG_COMPAT_32BIT_TIME
COMPAT_SYSCALL_DEFINE4(rt_sigtimedwait_time32, compat_sigset_t __user *, uthese,
		struct compat_siginfo __user *, uinfo,
		struct old_timespec32 __user *, uts, compat_size_t, sigsetsize)
{
	sigset_t s;
	struct timespec64 t;
	kernel_siginfo_t info;
	long ret;

	/* time32 入口把旧 timespec 扩展为 timespec64，再复用同一同步等待核心。 */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (get_compat_sigset(&s, uthese))
		return -EFAULT;

	if (uts) {
		if (get_old_timespec32(&t, uts))
			return -EFAULT;
	}

	ret = do_sigtimedwait(&s, &info, uts ? &t : NULL);

	if (ret > 0 && uinfo) {
		if (copy_siginfo_to_user32(uinfo, &info))
			ret = -EFAULT;
	}

	return ret;
}
#endif
#endif

/* prepare_kill_siginfo()：用 current 可见身份构造 kill/tkill 来源信息。 */
/*
 * 契约补充：@sig/@type 为输入，@info 为完整输出；无锁且不睡眠。
 * 返回无；用 current 在当前 namespace 可见的身份构造可信来源信息。
 */
static void prepare_kill_siginfo(int sig, struct kernel_siginfo *info,
				 enum pid_type type)
{
	/* 按线程定向与否选择 SI_TKILL/SI_USER，并填 current 的可见 tgid/uid。 */
	clear_siginfo(info);
	info->si_signo = sig;
	info->si_errno = 0;
	info->si_code = (type == PIDTYPE_PID) ? SI_TKILL : SI_USER;
	info->si_pid = task_tgid_vnr(current);
	info->si_uid = from_kuid_munged(current_user_ns(), current_uid());
}

/**
 *  sys_kill - send a signal to a process
 *  @pid: the PID of the process
 *  @sig: signal to be sent
 */
/* 构造用户来源 siginfo 后由 pid 多态 helper 实现 kill(2)。 */
SYSCALL_DEFINE2(kill, pid_t, pid, int, sig)
{
	struct kernel_siginfo info;

	prepare_kill_siginfo(sig, &info, PIDTYPE_TGID);

	return kill_something_info(sig, &info, pid);
}

/*
 * Verify that the signaler and signalee either are in the same pid namespace
 * or that the signaler's pid namespace is an ancestor of the signalee's pid
 * namespace.
 */
/*
 * pidfd 不因持有 fd 就自动跨越无关 pid namespace；发送者只能
 * 访问同 namespace 或其后代 namespace 中的目标。
 */
static bool access_pidfd_pidns(struct pid *pid)
{
	struct pid_namespace *active = task_active_pid_ns(current);
	struct pid_namespace *p = ns_of_pid(pid);

	for (;;) {
		if (!p)
			return false;
	/* pidfd 目标只能在调用者可表示的 pid namespace 层级访问，否则返回 -EPERM。 */
		if (p == active)
			break;
		p = p->parent;
	}

	return true;
}

/*
 * 契约补充：@info 是当前 syscall ABI 的用户输入，@kinfo 为内核输出。
 * 根据 in_compat_syscall() 分派；返回转换 errno，不转移任何所有权。
 */
static int copy_siginfo_from_user_any(kernel_siginfo_t *kinfo,
		siginfo_t __user *info)
{
	/* pidfd syscall 共用入口，按当前 syscall ABI 选择原生或 compat 转换。 */
#ifdef CONFIG_COMPAT
	/*
	 * Avoid hooking up compat syscalls and instead handle necessary
	 * conversions here. Note, this is a stop-gap measure and should not be
	 * considered a generic solution.
	 */
	/* pidfd 暂在同一入口转换 compat 数据；这是局部过渡方案。 */
	if (in_compat_syscall())
		return copy_siginfo_from_user32(
			kinfo, (struct compat_siginfo __user *)info);
#endif
	return copy_siginfo_from_user(kinfo, info);
}

/*
 * 契约补充：@file 是 fd guard 保持有效的借用文件；无锁、不睡眠。
 * 返回借用的 struct pid 或错误指针，引用生命周期仍由 file/调用者保护。
 */
static struct pid *pidfd_to_pid(const struct file *file)
{
	/* 同时接受现代 pidfd 与旧 tgid pidfd 文件类型，统一返回 struct pid。 */
	struct pid *pid;

	pid = pidfd_pid(file);
	if (!IS_ERR(pid))
		return pid;

	return tgid_pidfd_to_pid(file);
}

#define PIDFD_SEND_SIGNAL_FLAGS                            \
	(PIDFD_SIGNAL_THREAD | PIDFD_SIGNAL_THREAD_GROUP | \
	 PIDFD_SIGNAL_PROCESS_GROUP)

/*
 * 契约补充：@pid 为稳定借用目标，@sig/@type/@flags 决定信号与 scope，
 * @info 可空且为用户输入。函数可能 usercopy/取锁；返回权限、参数或发送结果。
 */
static int do_pidfd_send_signal(struct pid *pid, int sig, enum pid_type type,
				siginfo_t __user *info, unsigned int flags)
{
	/*
	 * flags 可覆盖 fd 推断的线程/线程组/进程组作用域。用户 siginfo 的 signo
	 * 必须与参数一致；除给自身（且非 PGID）外，不允许伪造非负内核来源或
	 * SI_TKILL。无 info 时构造可信的 current 身份，再进入相应 kill helper。
	 */
	kernel_siginfo_t kinfo;

	switch (flags) {
	case PIDFD_SIGNAL_THREAD:
		type = PIDTYPE_PID;
		break;
	case PIDFD_SIGNAL_THREAD_GROUP:
		type = PIDTYPE_TGID;
		break;
	case PIDFD_SIGNAL_PROCESS_GROUP:
		type = PIDTYPE_PGID;
		/* 目标类型确定后，后续 siginfo 与 namespace 校验保持统一。 */
		break;
	}

	if (info) {
		int ret;

		ret = copy_siginfo_from_user_any(&kinfo, info);
		if (unlikely(ret))
			return ret;

		if (unlikely(sig != kinfo.si_signo))
			return -EINVAL;

		/* Only allow sending arbitrary signals to yourself. */
		/* 只有给自身且非进程组作用域时可携带任意用户来源码。 */
		if ((task_pid(current) != pid || type > PIDTYPE_TGID) &&
		    (kinfo.si_code >= 0 || kinfo.si_code == SI_TKILL))
			return -EPERM;
	} else {
		prepare_kill_siginfo(sig, &kinfo, type);
	}

	if (type == PIDTYPE_PGID)
		return kill_pgrp_info(sig, &kinfo, pid);

	return kill_pid_info_type(sig, &kinfo, pid, type);
}

/**
 * sys_pidfd_send_signal - Signal a process through a pidfd
 * @pidfd:  file descriptor of the process
 * @sig:    signal to send
 * @info:   signal info
 * @flags:  future flags
 *
 * Send the signal to the thread group or to the individual thread depending
 * on PIDFD_THREAD.
 * In the future extension to @flags may be used to override the default scope
 * of @pidfd.
 *
 * Return: 0 on success, negative errno on failure
 */
/*
 * 中文学习注释：先验证 flags 互斥，再处理 SELF_THREAD/SELF_THREAD_GROUP
 * 魔数或从 fd 解析 pid。普通 pidfd 还检查 pid namespace 可达性，并由
 * PIDFD_THREAD 标志推断默认 scope；fd 的 guard 在发送完成前保持文件有效。
 */
SYSCALL_DEFINE4(pidfd_send_signal, int, pidfd, int, sig,
		siginfo_t __user *, info, unsigned int, flags)
{
	struct pid *pid;
	enum pid_type type;
	int ret;

	/* Enforce flags be set to 0 until we add an extension. */
	/* 拒绝未知位，防止旧内核误接受未来语义。 */
	if (flags & ~PIDFD_SEND_SIGNAL_FLAGS)
		return -EINVAL;

	/* Ensure that only a single signal scope determining flag is set. */
	/* 线程、线程组和进程组作用域互斥。 */
	if (hweight32(flags & PIDFD_SEND_SIGNAL_FLAGS) > 1)
		return -EINVAL;

	switch (pidfd) {
	case PIDFD_SELF_THREAD:
		pid = get_task_pid(current, PIDTYPE_PID);
		type = PIDTYPE_PID;
		break;
	case PIDFD_SELF_THREAD_GROUP:
		pid = get_task_pid(current, PIDTYPE_TGID);
		type = PIDTYPE_TGID;
		break;
	default: {
		CLASS(fd, f)(pidfd);
		if (fd_empty(f))
			return -EBADF;

		/* Is this a pidfd? */
		/* 解析标准 pidfd 或兼容 tgid pidfd，其他文件类型报错。 */
		pid = pidfd_to_pid(fd_file(f));
		if (IS_ERR(pid))
			return PTR_ERR(pid);

		if (!access_pidfd_pidns(pid))
			return -EINVAL;

		/* Infer scope from the type of pidfd. */
		/* 未显式覆盖时由 PIDFD_THREAD 推断默认作用域。 */
		if (fd_file(f)->f_flags & PIDFD_THREAD)
			type = PIDTYPE_PID;
		else
			type = PIDTYPE_TGID;

		return do_pidfd_send_signal(pid, sig, type, info, flags);
	}
	}

	ret = do_pidfd_send_signal(pid, sig, type, info, flags);
	put_pid(pid);

	return ret;
}

/* do_send_specific()：以 tid 定向发送，并可用 tgid 防止 PID 复用误投。 */
static int
do_send_specific(pid_t tgid, pid_t pid, int sig, struct kernel_siginfo *info)
{
	/*
	 * RCU 下按 vpid 找线程，并可用 tgid 二次校验抵御 pid 复用。sig==0 仅
	 * 探测权限/存在性；真正发送若恰与退出竞态而锁不到 sighand，按“目标
	 * 已在收到后退出”视为成功，符合线程私有信号不可再观察的语义。
	 */
	struct task_struct *p;
	int error = -ESRCH;

	rcu_read_lock();
	p = find_task_by_vpid(pid);
	if (p && (tgid <= 0 || task_tgid_vnr(p) == tgid)) {
		error = check_kill_permission(sig, info, p);
		/*
		 * The null signal is a permissions and process existence
		 * probe.  No signal is actually delivered.
		 */
		/* sig==0 只验证目标存在与权限，不排队也不唤醒。 */
		if (!error && sig) {
			error = do_send_sig_info(sig, info, p, PIDTYPE_PID);
			/*
			 * If lock_task_sighand() failed we pretend the task
			 * dies after receiving the signal. The window is tiny,
			 * and the signal is private anyway.
			 */
			/* 锁失败表示目标正在退出；线程私有信号已不可观察，按成功处理。 */
			if (unlikely(error == -ESRCH))
				error = 0;
		}
	}
	rcu_read_unlock();

	return error;
}

/* do_tkill()：tkill/tgkill 共用的 SI_TKILL 线程发送入口。 */
static int do_tkill(pid_t tgid, pid_t pid, int sig)
{
	/* 为 tkill/tgkill 构造 SI_TKILL 信息并走线程定向发送。 */
	struct kernel_siginfo info;

	prepare_kill_siginfo(sig, &info, PIDTYPE_PID);

	return do_send_specific(tgid, pid, sig, &info);
}

/**
 *  sys_tgkill - send signal to one specific thread
 *  @tgid: the thread group ID of the thread
 *  @pid: the PID of the thread
 *  @sig: signal to be sent
 *
 *  This syscall also checks the @tgid and returns -ESRCH even if the PID
 *  exists but it's not belonging to the target process anymore. This
 *  method solves the problem of threads exiting and PIDs getting reused.
 */
/* tgid+tid 双重身份校验是相对旧 tkill 的关键防复用保证。 */
SYSCALL_DEFINE3(tgkill, pid_t, tgid, pid_t, pid, int, sig)
{
	/* This is only valid for single tasks */
	/* 线程定向接口要求正 tid/tgid，不接受进程组多态。 */
	if (pid <= 0 || tgid <= 0)
		return -EINVAL;

	return do_tkill(tgid, pid, sig);
}

/**
 *  sys_tkill - send signal to one specific task
 *  @pid: the PID of the task
 *  @sig: signal to be sent
 *
 *  Send a signal to only one task, even if it's a CLONE_THREAD task.
 */
/* 旧 ABI 只给 tid，无法像 tgkill 一样验证线程组归属。 */
SYSCALL_DEFINE2(tkill, pid_t, pid, int, sig)
{
	/* This is only valid for single tasks */
	/* 旧 tkill 同样只接受正 tid。 */
	if (pid <= 0)
		return -EINVAL;

	return do_tkill(0, pid, sig);
}

static int do_rt_sigqueueinfo(pid_t pid, int sig, kernel_siginfo_t *info)
{
	/*
	 * 用户只能为自己使用非负/内核保留来源码，不能冒充 kernel 或另一任务
	 * 的 kill/tgkill；POSIX rt_sigqueueinfo 仅支持正 pid，不支持进程组。
	 */
	/* Not even root can pretend to send signals from the kernel.
	 * Nor can they impersonate a kill()/tgkill(), which adds source info.
	 */
	/* 即使特权用户也不能伪造内核来源或由内核填写身份的 kill/tgkill。 */
	if ((info->si_code >= 0 || info->si_code == SI_TKILL) &&
	    (task_pid_vnr(current) != pid))
		return -EPERM;

	/* POSIX.1b doesn't mention process groups.  */
	/* 该 POSIX 实时排队接口仅支持单个进程，不扩展 pid 负值语义。 */
	return kill_proc_info(sig, info, pid);
}

/**
 *  sys_rt_sigqueueinfo - send signal information to a signal
 *  @pid: the PID of the thread
 *  @sig: signal to be sent
 *  @uinfo: signal info to be sent
 */
/* 覆盖用户结构中的 signo，校验来源后按进程信号排队完整 payload。 */
SYSCALL_DEFINE3(rt_sigqueueinfo, pid_t, pid, int, sig,
		siginfo_t __user *, uinfo)
{
	kernel_siginfo_t info;
	int ret = __copy_siginfo_from_user(sig, &info, uinfo);
	if (unlikely(ret))
		return ret;
	return do_rt_sigqueueinfo(pid, sig, &info);
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE3(rt_sigqueueinfo,
			compat_pid_t, pid,
			int, sig,
			struct compat_siginfo __user *, uinfo)
{
	kernel_siginfo_t info;
	/* compat rt_sigqueueinfo 导入 32 位 siginfo 后复用 native 校验与发送核心。 */
	int ret = __copy_siginfo_from_user32(sig, &info, uinfo);
	if (unlikely(ret))
		return ret;
	return do_rt_sigqueueinfo(pid, sig, &info);
}
#endif

/* do_rt_tgsigqueueinfo()：携带实时 payload 的 tgid+tid 定向发送核心。 */
/*
 * 契约补充：@tgid/@pid 必须为正并共同唯一定位线程，@info 为借用 payload。
 * 无入口锁；返回 -EINVAL/-EPERM/-ESRCH 或发送结果，不接管 @info。
 */
static int do_rt_tgsigqueueinfo(pid_t tgid, pid_t pid, int sig, kernel_siginfo_t *info)
{
	/* rt payload 的线程定向版本，同时要求合法 tgid/tid 和防伪来源检查。 */
	/* This is only valid for single tasks */
	/* tgsigqueueinfo 必须由正 tgid+tid 唯一定位线程。 */
	if (pid <= 0 || tgid <= 0)
		return -EINVAL;

	/* Not even root can pretend to send signals from the kernel.
	 * Nor can they impersonate a kill()/tgkill(), which adds source info.
	 */
	/* 实时 payload 也不得伪造 SI_KERNEL 或 kill/tgkill 来源身份。 */
	if ((info->si_code >= 0 || info->si_code == SI_TKILL) &&
	    (task_pid_vnr(current) != pid))
		return -EPERM;

	return do_send_specific(tgid, pid, sig, info);
}

SYSCALL_DEFINE4(rt_tgsigqueueinfo, pid_t, tgid, pid_t, pid, int, sig,
		siginfo_t __user *, uinfo)
{
	kernel_siginfo_t info;
	int ret = __copy_siginfo_from_user(sig, &info, uinfo);
	if (unlikely(ret))
		return ret;
	return do_rt_tgsigqueueinfo(tgid, pid, sig, &info);
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE4(rt_tgsigqueueinfo,
			compat_pid_t, tgid,
			compat_pid_t, pid,
			int, sig,
			struct compat_siginfo __user *, uinfo)
{
	kernel_siginfo_t info;
	int ret = __copy_siginfo_from_user32(sig, &info, uinfo);
	if (unlikely(ret))
		return ret;
	return do_rt_tgsigqueueinfo(tgid, pid, sig, &info);
}
#endif

/*
 * For kthreads only, must not be used if cloned with CLONE_SIGHAND
 */
/*
 * 中文学习注释：内核线程可直接安装简单 handler；若改为 SIG_IGN，必须在
 * siglock 下同步清除私有/共享同号 pending 并重算 TIF。共享 sighand 的
 * 线程不能用此 API，因为它会无协商改变整个组 disposition。
 */
void kernel_sigaction(int sig, __sighandler_t action)
{
	spin_lock_irq(&current->sighand->siglock);
	current->sighand->action[sig - 1].sa.sa_handler = action;
	if (action == SIG_IGN) {
		sigset_t mask;

		sigemptyset(&mask);
		sigaddset(&mask, sig);
	/* 内核安装 handler 只允许合法信号和受支持 flags，并在 siglock 下替换 action。 */

		flush_sigqueue_mask(current, &mask, &current->signal->shared_pending);
		flush_sigqueue_mask(current, &mask, &current->pending);
		recalc_sigpending();
	}
	spin_unlock_irq(&current->sighand->siglock);
}
EXPORT_SYMBOL(kernel_sigaction);

void __weak sigaction_compat_abi(struct k_sigaction *act,
		struct k_sigaction *oact)
{
	/* 架构可覆盖，用于修正其特殊 sigaction ABI；通用实现无需处理。 */
}

/*
 * do_sigaction() - 在 current 线程组上查询或安装 disposition。
 *
 * 拒绝无效/内核专用信号及 SA_IMMUTABLE 动作；只向用户暴露 UAPI flags，
 * 并从 handler mask 剔除 SIGKILL/SIGSTOP。新动作变为忽略时按 POSIX 从
 * 私有和共享队列删除所有同号实例；从忽略恢复时把 POSIX timer ignored
 * 节点重新纳入 pending。整个动作表与队列迁移在 siglock 下原子完成。
 */
int do_sigaction(int sig, struct k_sigaction *act, struct k_sigaction *oact)
{
	struct task_struct *p = current, *t;
	struct k_sigaction *k;
	sigset_t mask;

	if (!valid_signal(sig) || sig < 1 || (act && sig_kernel_only(sig)))
		return -EINVAL;

	k = &p->sighand->action[sig-1];
	/* action 槽只在 siglock 下读取/替换，旧值与 pending 清理属于同一事务。 */

	spin_lock_irq(&p->sighand->siglock);
	if (k->sa.sa_flags & SA_IMMUTABLE) {
		spin_unlock_irq(&p->sighand->siglock);
		return -EINVAL;
	}
	if (oact)
		*oact = *k;

	/*
	 * Make sure that we never accidentally claim to support SA_UNSUPPORTED,
	 * e.g. by having an architecture use the bit in their uapi.
	 */
	/* 编译期禁止架构把探测位纳入 UAPI，避免误报 flag 支持。 */
	BUILD_BUG_ON(UAPI_SA_FLAGS & SA_UNSUPPORTED);

	/*
	 * Clear unknown flag bits in order to allow userspace to detect missing
	 * support for flag bits and to allow the kernel to use non-uapi bits
	 * internally.
	 */
	/* 过滤未知位既支持用户探测，也保留内核私有 flag 空间。 */
	if (act)
		act->sa.sa_flags &= UAPI_SA_FLAGS;
	if (oact)
		oact->sa.sa_flags &= UAPI_SA_FLAGS;

	sigaction_compat_abi(act, oact);

	if (act) {
		bool was_ignored = k->sa.sa_handler == SIG_IGN;

		sigdelsetmask(&act->sa.sa_mask,
			      sigmask(SIGKILL) | sigmask(SIGSTOP));
		*k = *act;
		/*
		 * POSIX 3.3.1.3:
		 *  "Setting a signal action to SIG_IGN for a signal that is
		 *   pending shall cause the pending signal to be discarded,
		 *   whether or not it is blocked."
		 *
		 *  "Setting a signal action to SIG_DFL for a signal that is
		 *   pending and whose default action is to ignore the signal
		 *   (for example, SIGCHLD), shall cause the pending signal to
		 *   be discarded, whether or not it is blocked"
		 */
		/*
		 * POSIX 要求动作改为 SIG_IGN，或改为默认且默认动作本就
		 * 忽略时，无论是否屏蔽都丢弃既有 pending，因此必须清所有线程队列。
		 */
		if (sig_handler_ignored(sig_handler(p, sig), sig)) {
			sigemptyset(&mask);
			sigaddset(&mask, sig);
			flush_sigqueue_mask(p, &mask, &p->signal->shared_pending);
			for_each_thread(p, t)
				flush_sigqueue_mask(p, &mask, &t->pending);
		} else if (was_ignored) {
	/* 锁下读取旧 action、校验并安装新 action，同时按忽略语义清理 pending 和 stop 状态。 */
			posixtimer_sig_unignore(p, sig);
		}
	}

	spin_unlock_irq(&p->sighand->siglock);
	return 0;
}

#ifdef CONFIG_DYNAMIC_SIGFRAME
/* sigaltstack_lock()：动态信号帧配置下串行化备用栈状态更新。 */
static inline void sigaltstack_lock(void)
	__acquires(&current->sighand->siglock)
{
	/* 动态 signal frame 架构允许并发状态依赖，需用 siglock 串行化 altstack。 */
	spin_lock_irq(&current->sighand->siglock);
}

static inline void sigaltstack_unlock(void)
	__releases(&current->sighand->siglock)
{
	spin_unlock_irq(&current->sighand->siglock);
}
#else
static inline void sigaltstack_lock(void) { }
static inline void sigaltstack_unlock(void) { }
#endif

/*
 * 契约补充：@ss/@oss 分别为可空输入/输出内核副本，@sp 是当前用户栈地址，
 * @min_ss_size 为字节下限。可能取得 siglock；返回 0 或 -EPERM/-EINVAL/-ENOMEM。
 */
static int
do_sigaltstack (const stack_t *ss, stack_t *oss, unsigned long sp,
		size_t min_ss_size)
{
	/*
	 * sigaltstack 核心：可先返回当前配置；正在备用栈上时禁止修改；严格校验
	 * mode 与最小/架构允许尺寸。SS_DISABLE 归一化为 NULL+0。仅真实变化才
	 * 进入可选 siglock，成功时一次提交 sp/size/flags，失败保持旧配置。
	 */
	struct task_struct *t = current;
	int ret = 0;

	if (oss) {
		memset(oss, 0, sizeof(stack_t));
		oss->ss_sp = (void __user *) t->sas_ss_sp;
		oss->ss_size = t->sas_ss_size;
		/* 输出 flags 由当前 SP 的 on-stack 状态与持久配置位共同组成。 */
		oss->ss_flags = sas_ss_flags(sp) |
			(current->sas_ss_flags & SS_FLAG_BITS);
	}

	if (ss) {
		void __user *ss_sp = ss->ss_sp;
	/* 校验 flags、栈区间和当前 on-stack 状态后，才原子更新 sas_ss_* 线程状态。 */
		size_t ss_size = ss->ss_size;
		unsigned ss_flags = ss->ss_flags;
		int ss_mode;

		if (unlikely(on_sig_stack(sp)))
			return -EPERM;

		ss_mode = ss_flags & ~SS_FLAG_BITS;
		if (unlikely(ss_mode != SS_DISABLE && ss_mode != SS_ONSTACK &&
				ss_mode != 0))
			return -EINVAL;

		/*
		 * Return before taking any locks if no actual
		 * sigaltstack changes were requested.
		 */
	/* 三元组完全相同则无状态变化，可在取得可选 siglock 前快速返回。 */
		if (t->sas_ss_sp == (unsigned long)ss_sp &&
		    t->sas_ss_size == ss_size &&
		    t->sas_ss_flags == ss_flags)
			return 0;

		sigaltstack_lock();
		if (ss_mode == SS_DISABLE) {
			ss_size = 0;
	/* 校验 flags、栈区间和当前 on-stack 状态后，才原子更新 sas_ss_* 线程状态。 */
			ss_sp = NULL;
		} else {
			if (unlikely(ss_size < min_ss_size))
				ret = -ENOMEM;
			if (!sigaltstack_size_valid(ss_size))
				/* 启用栈必须满足架构最小值与上限，失败不改 current 状态。 */
				ret = -ENOMEM;
		}
		if (!ret) {
			t->sas_ss_sp = (unsigned long) ss_sp;
			t->sas_ss_size = ss_size;
			t->sas_ss_flags = ss_flags;
			/* 三元组只在全部校验通过且锁内重验无变化后一起发布。 */
		}
		sigaltstack_unlock();
	}
	return ret;
}

SYSCALL_DEFINE2(sigaltstack,const stack_t __user *,uss, stack_t __user *,uoss)
{
	/* 原生 ABI wrapper：先完整导入，再调用核心，最后导出旧配置。 */
	stack_t new, old;
	int err;
	if (uss && copy_from_user(&new, uss, sizeof(stack_t)))
		return -EFAULT;
	err = do_sigaltstack(uss ? &new : NULL, uoss ? &old : NULL,
			      current_user_stack_pointer(),
			      MINSIGSTKSZ);
	if (!err && uoss && copy_to_user(uoss, &old, sizeof(stack_t)))
		err = -EFAULT;
	return err;
}

/*
 * 契约补充：@uss 是 sigreturn 帧中的用户输入；可能 user fault。
 * 返回仅传播 -EFAULT，其余配置错误按历史 ABI 吞掉；current 栈状态可能更新。
 */
int restore_altstack(const stack_t __user *uss)
{
	/*
	 * sigreturn 恢复用户帧保存的 altstack；历史 ABI 除 usercopy EFAULT 外
	 * 吞掉配置校验错误，避免破坏旧程序的信号返回。
	 */
	stack_t new;
	if (copy_from_user(&new, uss, sizeof(stack_t)))
		return -EFAULT;
	(void)do_sigaltstack(&new, NULL, current_user_stack_pointer(),
			     MINSIGSTKSZ);
	/* squash all but EFAULT for now */
	/* 历史 sigreturn ABI 仅向外保留用户访存错误，吞掉配置错误。 */
	return 0;
}

int __save_altstack(stack_t __user *uss, unsigned long sp)
{
	/* 架构构造 signal frame 时把 current 的备用栈三元组写给用户。 */
	struct task_struct *t = current;
	int err = __put_user((void __user *)t->sas_ss_sp, &uss->ss_sp) |
		__put_user(t->sas_ss_flags, &uss->ss_flags) |
		__put_user(t->sas_ss_size, &uss->ss_size);
	return err;
}

#ifdef CONFIG_COMPAT
static int do_compat_sigaltstack(const compat_stack_t __user *uss_ptr,
				 compat_stack_t __user *uoss_ptr)
{
	/* 32 位 wrapper 转换指针宽度，并采用 COMPAT_MINSIGSTKSZ。 */
	stack_t uss, uoss;
	int ret;

	if (uss_ptr) {
		compat_stack_t uss32;
		/* 用户输入先整体复制到内核栈，再逐字段扩展指针和宽度。 */
		if (copy_from_user(&uss32, uss_ptr, sizeof(compat_stack_t)))
			return -EFAULT;
		uss.ss_sp = compat_ptr(uss32.ss_sp);
		uss.ss_flags = uss32.ss_flags;
		uss.ss_size = uss32.ss_size;
	}
	/* compat 栈字段先扩展为 native 结构，调用核心后再把旧栈状态缩窄回用户 ABI。 */
	ret = do_sigaltstack(uss_ptr ? &uss : NULL, &uoss,
			     compat_user_stack_pointer(),
			     COMPAT_MINSIGSTKSZ);
	if (ret >= 0 && uoss_ptr)  {
		compat_stack_t old;
		/* 仅核心成功时导出旧栈，目标先清零避免 compat padding 泄露。 */
		memset(&old, 0, sizeof(old));
		old.ss_sp = ptr_to_compat(uoss.ss_sp);
		old.ss_flags = uoss.ss_flags;
		old.ss_size = uoss.ss_size;
		if (copy_to_user(uoss_ptr, &old, sizeof(compat_stack_t)))
			ret = -EFAULT;
	}
	return ret;
}

COMPAT_SYSCALL_DEFINE2(sigaltstack,
			const compat_stack_t __user *, uss_ptr,
			compat_stack_t __user *, uoss_ptr)
{
	return do_compat_sigaltstack(uss_ptr, uoss_ptr);
}

/* compat_restore_altstack()：从 32 位 signal frame 恢复备用栈状态。 */
/*
 * 契约补充：@uss 是 32 位 sigreturn 帧输入；可能 user fault。
 * 返回仅保留 -EFAULT，成功时更新 current 备用栈且不持有用户地址。
 */
int compat_restore_altstack(const compat_stack_t __user *uss)
{
	/* compat sigreturn 同样只保留 -EFAULT，其余恢复错误按历史语义吞掉。 */
	int err = do_compat_sigaltstack(uss, NULL);
	/* squash all but -EFAULT for now */
	/* compat 恢复同样只传播 -EFAULT，保持旧 ABI。 */
	return err == -EFAULT ? err : 0;
}

int __compat_save_altstack(compat_stack_t __user *uss, unsigned long sp)
{
	/* 将备用栈配置缩窄为 compat 指针/尺寸并写入 32 位 signal frame。 */
	int err;
	struct task_struct *t = current;
	err = __put_user(ptr_to_compat((void __user *)t->sas_ss_sp),
			 &uss->ss_sp) |
		__put_user(t->sas_ss_flags, &uss->ss_flags) |
		__put_user(t->sas_ss_size, &uss->ss_size);
	return err;
}
#endif

#ifdef __ARCH_WANT_SYS_SIGPENDING

/**
 *  sys_sigpending - examine pending signals
 *  @uset: where mask of pending signal is returned
 */
/* 旧 ABI 只复制 old_sigset_t 可表示的低位信号集合。 */
SYSCALL_DEFINE1(sigpending, old_sigset_t __user *, uset)
{
	sigset_t set;

	if (sizeof(old_sigset_t) > sizeof(*uset))
		return -EINVAL;

	do_sigpending(&set);

	if (copy_to_user(uset, &set, sizeof(old_sigset_t)))
		return -EFAULT;

	return 0;
}

#ifdef CONFIG_COMPAT
/*
 * compat old sigpending：把 current 被屏蔽的 pending 低字写入 @set32。
 * 无状态副作用；成功 0，用户写失败返回 -EFAULT。
 */
COMPAT_SYSCALL_DEFINE1(sigpending, compat_old_sigset_t __user *, set32)
{
	sigset_t set;

	do_sigpending(&set);

	return put_user(set.sig[0], set32);
}
#endif

#endif

#ifdef __ARCH_WANT_SYS_SIGPROCMASK
/**
 *  sys_sigprocmask - examine and change blocked signals
 *  @how: whether to add, remove, or set signals
 *  @nset: signals to add or remove (if non-null)
 *  @oset: previous value of signal mask if non-null
 *
 * Some platforms have their own version with special arguments;
 * others support only sys_rt_sigprocmask.
 */
/* 旧单字 mask ABI；架构可能自定义参数，其他架构只提供实时版本。 */

/* 旧单字 mask ABI，仅更新 blocked 的第一机器字，其他字保留。 */
SYSCALL_DEFINE3(sigprocmask, int, how, old_sigset_t __user *, nset,
		old_sigset_t __user *, oset)
{
	old_sigset_t old_set, new_set;
	sigset_t new_blocked;

	old_set = current->blocked.sig[0];

	if (nset) {
		if (copy_from_user(&new_set, nset, sizeof(*nset)))
			return -EFAULT;

		new_blocked = current->blocked;

		switch (how) {
		case SIG_BLOCK:
			sigaddsetmask(&new_blocked, new_set);
			break;
		case SIG_UNBLOCK:
			sigdelsetmask(&new_blocked, new_set);
			break;
		case SIG_SETMASK:
			new_blocked.sig[0] = new_set;
			break;
		default:
			return -EINVAL;
		}

		set_current_blocked(&new_blocked);
	}

	if (oset) {
		if (copy_to_user(oset, &old_set, sizeof(*oset)))
			return -EFAULT;
	}

	return 0;
}
#endif /* __ARCH_WANT_SYS_SIGPROCMASK */
/* 结束架构请求旧 sigprocmask syscall 时的兼容实现。 */

#ifndef CONFIG_ODD_RT_SIGACTION
/**
 *  sys_rt_sigaction - alter an action taken by a process
 *  @sig: signal to be sent
 *  @act: new sigaction
 *  @oact: used to save the previous sigaction
 *  @sigsetsize: size of sigset_t type
 */
/* 安装/查询 @sig 动作；@act/@oact 可空，集合大小当前必须精确匹配。 */
SYSCALL_DEFINE4(rt_sigaction, int, sig,
		const struct sigaction __user *, act,
		struct sigaction __user *, oact,
		size_t, sigsetsize)
{
	struct k_sigaction new_sa, old_sa;
	int ret;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	/* 原生 rt_sigaction 当前固定集合大小，但不应阻断未来扩展。 */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (act && copy_from_user(&new_sa.sa, act, sizeof(new_sa.sa)))
		return -EFAULT;

	ret = do_sigaction(sig, act ? &new_sa : NULL, oact ? &old_sa : NULL);
	if (ret)
		return ret;

	if (oact && copy_to_user(oact, &old_sa.sa, sizeof(old_sa.sa)))
		return -EFAULT;

	return 0;
}
#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE4(rt_sigaction, int, sig,
		const struct compat_sigaction __user *, act,
		struct compat_sigaction __user *, oact,
		compat_size_t, sigsetsize)
{
	struct k_sigaction new_ka, old_ka;
#ifdef __ARCH_HAS_SA_RESTORER
	compat_uptr_t restorer;
#endif
	int ret;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	/* compat 版本也暂时要求固定大小。 */
	if (sigsetsize != sizeof(compat_sigset_t))
		return -EINVAL;

	if (act) {
		compat_uptr_t handler;
		ret = get_user(handler, &act->sa_handler);
		new_ka.sa.sa_handler = compat_ptr(handler);
#ifdef __ARCH_HAS_SA_RESTORER
		ret |= get_user(restorer, &act->sa_restorer);
		new_ka.sa.sa_restorer = compat_ptr(restorer);
#endif
		ret |= get_compat_sigset(&new_ka.sa.sa_mask, &act->sa_mask);
		ret |= get_user(new_ka.sa.sa_flags, &act->sa_flags);
		if (ret)
			return -EFAULT;
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);
	if (!ret && oact) {
		ret = put_user(ptr_to_compat(old_ka.sa.sa_handler), 
			       &oact->sa_handler);
		ret |= put_compat_sigset(&oact->sa_mask, &old_ka.sa.sa_mask,
					 sizeof(oact->sa_mask));
		ret |= put_user(old_ka.sa.sa_flags, &oact->sa_flags);
#ifdef __ARCH_HAS_SA_RESTORER
		ret |= put_user(ptr_to_compat(old_ka.sa.sa_restorer),
				&oact->sa_restorer);
#endif
	}
	return ret;
}
#endif
#endif /* !CONFIG_ODD_RT_SIGACTION */
/* 特殊架构可自行提供不同 rt_sigaction ABI，通用实现至此结束。 */

#ifdef CONFIG_OLD_SIGACTION
SYSCALL_DEFINE3(sigaction, int, sig,
		const struct old_sigaction __user *, act,
	        struct old_sigaction __user *, oact)
{
	struct k_sigaction new_ka, old_ka;
	int ret;

	if (act) {
		old_sigset_t mask;
		if (!access_ok(act, sizeof(*act)) ||
		    __get_user(new_ka.sa.sa_handler, &act->sa_handler) ||
		    __get_user(new_ka.sa.sa_restorer, &act->sa_restorer) ||
		    __get_user(new_ka.sa.sa_flags, &act->sa_flags) ||
		    __get_user(mask, &act->sa_mask))
			return -EFAULT;
#ifdef __ARCH_HAS_KA_RESTORER
		new_ka.ka_restorer = NULL;
#endif
		siginitset(&new_ka.sa.sa_mask, mask);
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);

	if (!ret && oact) {
		if (!access_ok(oact, sizeof(*oact)) ||
		    __put_user(old_ka.sa.sa_handler, &oact->sa_handler) ||
		    __put_user(old_ka.sa.sa_restorer, &oact->sa_restorer) ||
		    __put_user(old_ka.sa.sa_flags, &oact->sa_flags) ||
		    __put_user(old_ka.sa.sa_mask.sig[0], &oact->sa_mask))
			return -EFAULT;
	}

	return ret;
}
#endif
#ifdef CONFIG_COMPAT_OLD_SIGACTION
COMPAT_SYSCALL_DEFINE3(sigaction, int, sig,
		const struct compat_old_sigaction __user *, act,
	        struct compat_old_sigaction __user *, oact)
{
	struct k_sigaction new_ka, old_ka;
	int ret;
	compat_old_sigset_t mask;
	compat_uptr_t handler, restorer;

	if (act) {
		if (!access_ok(act, sizeof(*act)) ||
		    __get_user(handler, &act->sa_handler) ||
		    __get_user(restorer, &act->sa_restorer) ||
		    __get_user(new_ka.sa.sa_flags, &act->sa_flags) ||
		    __get_user(mask, &act->sa_mask))
			return -EFAULT;

#ifdef __ARCH_HAS_KA_RESTORER
		new_ka.ka_restorer = NULL;
#endif
		new_ka.sa.sa_handler = compat_ptr(handler);
		new_ka.sa.sa_restorer = compat_ptr(restorer);
		siginitset(&new_ka.sa.sa_mask, mask);
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);

	if (!ret && oact) {
		if (!access_ok(oact, sizeof(*oact)) ||
		    __put_user(ptr_to_compat(old_ka.sa.sa_handler),
			       &oact->sa_handler) ||
		    __put_user(ptr_to_compat(old_ka.sa.sa_restorer),
			       &oact->sa_restorer) ||
		    __put_user(old_ka.sa.sa_flags, &oact->sa_flags) ||
		    __put_user(old_ka.sa.sa_mask.sig[0], &oact->sa_mask))
			return -EFAULT;
	}
	return ret;
}
#endif

#ifdef CONFIG_SGETMASK_SYSCALL

/*
 * For backwards compatibility.  Functionality superseded by sigprocmask.
 */
/* 最旧 sgetmask/ssetmask 只处理低字，内部仍走安全 blocked helper。 */
SYSCALL_DEFINE0(sgetmask)
{
	/* SMP safe */
	/* blocked 仅由 current 写，因此读取低字无需跨 CPU 锁。 */
	return current->blocked.sig[0];
}

SYSCALL_DEFINE1(ssetmask, int, newmask)
{
	int old = current->blocked.sig[0];
	sigset_t newset;

	siginitset(&newset, newmask);
	set_current_blocked(&newset);

	return old;
}
#endif /* CONFIG_SGETMASK_SYSCALL */
/* 结束最旧 sgetmask/ssetmask 兼容接口。 */

#ifdef __ARCH_WANT_SYS_SIGNAL
/*
 * For backwards compatibility.  Functionality superseded by sigaction.
 */
/* 旧 signal() 等价于一次性、递送期间不自动屏蔽自身的 handler。 */
SYSCALL_DEFINE2(signal, int, sig, __sighandler_t, handler)
{
	struct k_sigaction new_sa, old_sa;
	int ret;

	new_sa.sa.sa_handler = handler;
	new_sa.sa.sa_flags = SA_ONESHOT | SA_NOMASK;
	sigemptyset(&new_sa.sa.sa_mask);

	ret = do_sigaction(sig, &new_sa, &old_sa);

	return ret ? ret : (unsigned long)old_sa.sa.sa_handler;
}
#endif /* __ARCH_WANT_SYS_SIGNAL */
/* 结束架构请求旧 signal() syscall 时的实现。 */

#ifdef __ARCH_WANT_SYS_PAUSE

SYSCALL_DEFINE0(pause)
{
	/* 可中断睡眠直到任一信号 pending；返回码让无 handler 时不自动重启。 */
	while (!signal_pending(current)) {
		__set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}
	return -ERESTARTNOHAND;
}

#endif

/*
 * 契约补充：@set 为借用临时屏蔽集合；进程上下文调用并可睡眠。
 * 函数保存旧 mask，睡至信号后返回 -ERESTARTNOHAND，并由返回路径恢复旧值。
 */
static int sigsuspend(sigset_t *set)
{
	/*
	 * 原子保存旧 mask、安装临时 mask 并睡到信号到达。实际恢复延迟到信号
	 * 返回路径，通过 restore_sigmask 标记完成，避免恢复与递送之间的窗口。
	 */
	current->saved_sigmask = current->blocked;
	set_current_blocked(set);

	while (!signal_pending(current)) {
		__set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}
	set_restore_sigmask();
	return -ERESTARTNOHAND;
}

/**
 *  sys_rt_sigsuspend - replace the signal mask for a value with the
 *	@unewset value until a signal is received
 *  @unewset: new signal mask value
 *  @sigsetsize: size of sigset_t type
 */
/* 原生 wrapper 导入完整集合，核心 helper 会剔除不可屏蔽信号。 */
SYSCALL_DEFINE2(rt_sigsuspend, sigset_t __user *, unewset, size_t, sigsetsize)
{
	sigset_t newset;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	/* rt_sigsuspend 当前固定集合大小，保留未来 ABI 扩展空间。 */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&newset, unewset, sizeof(newset)))
		return -EFAULT;
	return sigsuspend(&newset);
}
 
#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE2(rt_sigsuspend, compat_sigset_t __user *, unewset, compat_size_t, sigsetsize)
{
	sigset_t newset;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	/* compat rt_sigsuspend 同样暂只接受固定大小。 */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (get_compat_sigset(&newset, unewset))
		return -EFAULT;
	return sigsuspend(&newset);
}
#endif

#ifdef CONFIG_OLD_SIGSUSPEND
SYSCALL_DEFINE1(sigsuspend, old_sigset_t, mask)
{
	sigset_t blocked;
	siginitset(&blocked, mask);
	return sigsuspend(&blocked);
}
#endif
#ifdef CONFIG_OLD_SIGSUSPEND3
SYSCALL_DEFINE3(sigsuspend, int, unused1, int, unused2, old_sigset_t, mask)
{
	sigset_t blocked;
	siginitset(&blocked, mask);
	return sigsuspend(&blocked);
}
#endif

/* arch_vma_name()：供架构覆盖的特殊 VMA 命名钩子，通用实现返回 NULL。 */
/*
 * 契约补充：@vma 为借用且调用期间有效；默认实现无锁、不睡眠。
 * 返回 NULL 表示无架构专名；架构覆盖时返回字符串所有权仍归架构。
 */
__weak const char *arch_vma_name(struct vm_area_struct *vma)
{
	/* 架构可为特殊 VMA 提供 core/proc 显示名称；通用默认无名称。 */
	return NULL;
}

/*
 * 契约补充：无入参、无运行时返回或副作用；全部检查在编译期求值。
 * 任一用户/内核 ABI 大小或偏移不一致都会使构建失败。
 */
static inline void siginfo_buildtime_checks(void)
{
	/*
	 * 编译期 ABI 防线：确保用户/内核 siginfo 总大小及所有共享字段偏移一致，
	 * 并固定 USB async 依赖的 pid/uid 与 addr 重叠关系；任何布局漂移直接
	 * BUILD_BUG，而不是在运行时误解释 union。
	 */
	BUILD_BUG_ON(sizeof(struct siginfo) != SI_MAX_SIZE);

	/* Verify the offsets in the two siginfos match */
	/* 以下编译期断言保证用户/内核 tagged union 的共享字段同偏移。 */
#define CHECK_OFFSET(field) \
	BUILD_BUG_ON(offsetof(siginfo_t, field) != offsetof(kernel_siginfo_t, field))

	/* kill */
	/* 校验 kill 布局的发送者 pid/uid。 */
	CHECK_OFFSET(si_pid);
	CHECK_OFFSET(si_uid);

	/* timer */
	/* 校验 timer id、overrun 与 payload。 */
	CHECK_OFFSET(si_tid);
	CHECK_OFFSET(si_overrun);
	CHECK_OFFSET(si_value);

	/* rt */
	/* 校验实时信号的身份与 payload。 */
	CHECK_OFFSET(si_pid);
	CHECK_OFFSET(si_uid);
	CHECK_OFFSET(si_value);

	/* sigchld */
	/* 校验子进程身份、状态及用户/系统时间。 */
	CHECK_OFFSET(si_pid);
	CHECK_OFFSET(si_uid);
	CHECK_OFFSET(si_status);
	CHECK_OFFSET(si_utime);
	CHECK_OFFSET(si_stime);

	/* sigfault */
	/* 校验 fault 地址及各扩展错误字段。 */
	CHECK_OFFSET(si_addr);
	CHECK_OFFSET(si_trapno);
	CHECK_OFFSET(si_addr_lsb);
	CHECK_OFFSET(si_lower);
	CHECK_OFFSET(si_upper);
	CHECK_OFFSET(si_pkey);
	CHECK_OFFSET(si_perf_data);
	CHECK_OFFSET(si_perf_type);
	CHECK_OFFSET(si_perf_flags);

	/* sigpoll */
	/* 校验异步 I/O band 与 fd。 */
	CHECK_OFFSET(si_band);
	CHECK_OFFSET(si_fd);

	/* sigsys */
	/* 校验 SIGSYS 调用地址、系统调用号与审计架构。 */
	CHECK_OFFSET(si_call_addr);
	CHECK_OFFSET(si_syscall);
	CHECK_OFFSET(si_arch);
#undef CHECK_OFFSET

	/* usb asyncio */
	/* 校验 USB 历史 ABI 依赖的 pid/uid 与地址 union 重叠。 */
	BUILD_BUG_ON(offsetof(struct siginfo, si_pid) !=
		     offsetof(struct siginfo, si_addr));
	if (sizeof(int) == sizeof(void __user *)) {
		BUILD_BUG_ON(sizeof_field(struct siginfo, si_pid) !=
			     sizeof(void __user *));
	} else {
		BUILD_BUG_ON((sizeof_field(struct siginfo, si_pid) +
			      sizeof_field(struct siginfo, si_uid)) !=
			     sizeof(void __user *));
		BUILD_BUG_ON(offsetofend(struct siginfo, si_pid) !=
	/* 这些 BUILD_BUG_ON 验证内核/用户/compat siginfo 的大小、对齐和 union 偏移 ABI 不变量。 */
			     offsetof(struct siginfo, si_uid));
	}
#ifdef CONFIG_COMPAT
	BUILD_BUG_ON(offsetof(struct compat_siginfo, si_pid) !=
		     offsetof(struct compat_siginfo, si_addr));
	BUILD_BUG_ON(sizeof_field(struct compat_siginfo, si_pid) !=
		     sizeof(compat_uptr_t));
	BUILD_BUG_ON(sizeof_field(struct compat_siginfo, si_pid) !=
		     sizeof_field(struct siginfo, si_pid));
#endif
}

#if defined(CONFIG_SYSCTL)
static const struct ctl_table signal_debug_table[] = {
#ifdef CONFIG_SYSCTL_EXCEPTION_TRACE
	{
		.procname	= "exception-trace",
		.data		= &show_unhandled_signals,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec
	},
#endif
};

static const struct ctl_table signal_table[] = {
	{
		.procname	= "print-fatal-signals",
		.data		= &print_fatal_signals,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
};

/* init_signal_sysctls()：注册信号异常诊断与致命信号打印 sysctl。 */
/*
 * 契约补充：早期 init 上下文、无入参；注册两组长期 sysctl 表。
 * 返回固定 0；注册结果由 sysctl 核心持有，函数退出后无需回滚。
 */
static int __init init_signal_sysctls(void)
{
	/* 分别注册 debug/exception-trace 与 kernel/print-fatal-signals 调试开关。 */
	register_sysctl_init("debug", signal_debug_table);
	register_sysctl_init("kernel", signal_table);
	return 0;
}
early_initcall(init_signal_sysctls);
#endif /* CONFIG_SYSCTL */
/* 结束可运行时调整的信号诊断 sysctl。 */

void __init signals_init(void)
{
	/* 启动时先验证 ABI，再创建带 memcg 记账且分配失败即 panic 的队列 slab。 */
	siginfo_buildtime_checks();

	sigqueue_cachep = KMEM_CACHE(sigqueue, SLAB_PANIC | SLAB_ACCOUNT);
}

#ifdef CONFIG_KGDB_KDB
#include <linux/kdb.h>
/*
 * kdb_send_sig - Allows kdb to send signals without exposing
 * signal internals.  This function checks if the required locks are
 * available before calling the main signal code, to avoid kdb
 * deadlocks.
 */
/*
 * 中文学习注释：KDB 可能在任意锁上下文进入，绝不能阻塞等待 siglock，
 * 因而只 trylock。首次对非 RUNNING 任务发送会拒绝并警告 runqueue 死锁
 * 风险；用户重复命令视为显式承担风险。成功后用内核特权来源直接入队。
 */
void kdb_send_sig(struct task_struct *t, int sig)
{
	static struct task_struct *kdb_prev_t;
	int new_t, ret;
	if (!spin_trylock(&t->sighand->siglock)) {
		kdb_printf("Can't do kill command now.\n"
			   "The sigmask lock is held somewhere else in "
			   "kernel, try again later\n");
		return;
	}
	/* trylock 成功后记录是否首次选择该任务，用于要求二次确认死锁风险。 */
	new_t = kdb_prev_t != t;
	kdb_prev_t = t;
	if (!task_is_running(t) && new_t) {
		spin_unlock(&t->sighand->siglock);
		kdb_printf("Process is not RUNNING, sending a signal from "
			   "kdb risks deadlock\n"
			   "on the run queue locks. "
	/* KDB 只用 trylock；任务状态风险确认后才调用 locked 发送核心，并在日志前释放锁。 */
			   "The signal has _not_ been sent.\n"
			   "Reissue the kill command if you want to risk "
			   "the deadlock.\n");
		return;
	}
	/* 二次确认通过才以 PRIV 来源发送，并在打印结果前释放 siglock。 */
	ret = send_signal_locked(sig, SEND_SIG_PRIV, t, PIDTYPE_PID);
	spin_unlock(&t->sighand->siglock);
	if (ret)
		kdb_printf("Fail to deliver Signal %d to process %d.\n",
			   sig, t->pid);
	else
		kdb_printf("Signal %d is sent to process %d.\n", sig, t->pid);
}
#endif	/* CONFIG_KGDB_KDB */
/* 结束仅在 KGDB/KDB 配置下构建的调试器信号入口。 */
