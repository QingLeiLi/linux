// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/kernel/exit.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/sched/autogroup.h>
#include <linux/sched/mm.h>
#include <linux/sched/stat.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/cputime.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/capability.h>
#include <linux/completion.h>
#include <linux/personality.h>
#include <linux/tty.h>
#include <linux/iocontext.h>
#include <linux/key.h>
#include <linux/cpu.h>
#include <linux/acct.h>
#include <linux/tsacct_kern.h>
#include <linux/file.h>
#include <linux/freezer.h>
#include <linux/binfmts.h>
#include <linux/nsproxy.h>
#include <linux/pid_namespace.h>
#include <linux/ptrace.h>
#include <linux/profile.h>
#include <linux/mount.h>
#include <linux/proc_fs.h>
#include <linux/kthread.h>
#include <linux/mempolicy.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/cgroup.h>
#include <linux/syscalls.h>
#include <linux/signal.h>
#include <linux/posix-timers.h>
#include <linux/cn_proc.h>
#include <linux/mutex.h>
#include <linux/futex.h>
#include <linux/pipe_fs_i.h>
#include <linux/audit.h> /* for audit_free() */
#include <linux/resource.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/blkdev.h>
#include <linux/task_work.h>
#include <linux/fs_struct.h>
#include <linux/init_task.h>
#include <linux/perf_event.h>
#include <trace/events/sched.h>
#include <linux/hw_breakpoint.h>
#include <linux/oom.h>
#include <linux/writeback.h>
#include <linux/shm.h>
#include <linux/kcov.h>
#include <linux/kmsan.h>
#include <linux/random.h>
#include <linux/rcuwait.h>
#include <linux/compat.h>
#include <linux/io_uring.h>
#include <linux/kprobes.h>
#include <linux/rethook.h>
#include <linux/sysfs.h>
#include <linux/user_events.h>
#include <linux/unwind_deferred.h>
#include <linux/uaccess.h>
#include <linux/pidfs.h>

#include <uapi/linux/wait.h>

#include <asm/unistd.h>
#include <asm/mmu_context.h>

#include "exit.h"

/*
 * 本文件学习主线：
 *
 *   do_exit()/do_group_exit()
 *        -> 关闭线程仍持有的内核资源，并把退出统计固定下来
 *        -> exit_notify() 建立 EXIT_ZOMBIE/EXIT_DEAD 状态并通知父进程
 *        -> 父进程经 wait*() 观察停止、继续或退出事件
 *        -> wait_task_zombie()/自动回收路径调用 release_task()
 *        -> 从 PID、线程组及全局任务链表摘除，最后经 RCU 释放 task_struct
 *
 * EXIT_ZOMBIE 表示退出信息仍需由父进程读取；EXIT_DEAD 表示已经取得唯一
 * 回收权。tasklist_lock 保护父子关系和任务链表，sighand->siglock 保护线程组
 * 信号状态，signal->stats_lock 保护跨线程累计统计。读这份代码时要持续区分
 * “当前线程停止执行”“父进程取得退出信息”和“task_struct 真正释放”三件事。
 */

/*
 * The default value should be high enough to not crash a system that randomly
 * crashes its kernel from time to time, but low enough to at least not permit
 * overflowing 32-bit refcounts or the ldsem writer count.
 *
 * 默认上限既要容忍偶发 oops，又必须在反复 oops 泄漏引用、最终绕回 32 位
 * 引用计数或 ldsem 写者计数之前触发 panic；它是一道利用难度加固边界。
 */
static unsigned int oops_limit = 10000;

#ifdef CONFIG_SYSCTL
static const struct ctl_table kern_exit_table[] = {
	{
		.procname       = "oops_limit",
		.data           = &oops_limit,
		.maxlen         = sizeof(oops_limit),
		.mode           = 0644,
		.proc_handler   = proc_douintvec,
	},
};

static __init int kernel_exit_sysctls_init(void)
{
	/* 注册 /proc/sys/kernel/oops_limit；失败不影响启动主流程。 */
	register_sysctl_init("kernel", kern_exit_table);
	return 0;
}
late_initcall(kernel_exit_sysctls_init);
#endif

static atomic_t oops_count = ATOMIC_INIT(0);

#ifdef CONFIG_SYSFS
static ssize_t oops_count_show(struct kobject *kobj, struct kobj_attribute *attr,
			       char *page)
{
	/* 原子读取保证并发 oops 更新时不会看到撕裂值。 */
	return sysfs_emit(page, "%d\n", atomic_read(&oops_count));
}

static struct kobj_attribute oops_count_attr = __ATTR_RO(oops_count);

static __init int kernel_exit_sysfs_init(void)
{
	/* 在 kernel_kobj 下暴露只读 oops_count，便于诊断累计故障次数。 */
	sysfs_add_file_to_group(kernel_kobj, &oops_count_attr.attr, NULL);
	return 0;
}
late_initcall(kernel_exit_sysfs_init);
#endif

/*
 * For things release_task() would like to do *after* tasklist_lock is released.
 * 保存必须推迟到 tasklist_lock 之外完成的 PID 引用；释放 PID 可能唤醒等待者，
 * 不应扩大任务链表写锁的临界区。
 */
struct release_task_post {
	struct pid *pids[PIDTYPE_MAX];
};

static void __unhash_process(struct release_task_post *post, struct task_struct *p,
			     bool group_dead)
{
	/*
	 * 调用者持有 tasklist_lock 写锁。每个线程都脱离 PID 与 thread_node；
	 * 只有线程组最后成员再脱离 TGID/PGID/SID 和进程级 tasks/sibling 链表。
	 */
	struct pid *pid = task_pid(p);

	nr_threads--;

	detach_pid(post->pids, p, PIDTYPE_PID);
	wake_up_all(&pid->wait_pidfd);

	if (group_dead) {
		detach_pid(post->pids, p, PIDTYPE_TGID);
		detach_pid(post->pids, p, PIDTYPE_PGID);
		detach_pid(post->pids, p, PIDTYPE_SID);

		list_del_rcu(&p->tasks);
		list_del_init(&p->sibling);
		__this_cpu_dec(process_counts);
	}
	list_del_rcu(&p->thread_node);
}

/*
 * This function expects the tasklist_lock write-locked.
 * 调用者还需依赖这里取得 sighand->siglock，串行化线程组信号和统计收尾。
 */
static void __exit_signal(struct release_task_post *post, struct task_struct *tsk)
{
	struct signal_struct *sig = tsk->signal;
	bool group_dead = thread_group_leader(tsk);
	struct sighand_struct *sighand;
	struct tty_struct *tty;
	u64 utime, stime;

	sighand = rcu_dereference_check(tsk->sighand,
					lockdep_tasklist_lock_is_held());
	spin_lock(&sighand->siglock);

#ifdef CONFIG_POSIX_TIMERS
	posix_cpu_timers_exit(tsk);
	if (group_dead)
		posix_cpu_timers_exit_group(tsk);
#endif

	if (group_dead) {
		/* 最后线程接管并清空控制终端引用，锁外再执行最终 put。 */
		tty = sig->tty;
		sig->tty = NULL;
	} else {
		/*
		 * If there is any task waiting for the group exit
		 * then notify it:
		 * 若 exec/组退出路径正等待其他线程消失，最后一个计数归零者唤醒它。
		 */
		if (sig->notify_count > 0 && !--sig->notify_count)
			wake_up_process(sig->group_exec_task);

		if (tsk == sig->curr_target)
			sig->curr_target = next_thread(tsk);
	}

	/*
	 * Accumulate here the counters for all threads as they die. We could
	 * skip the group leader because it is the last user of signal_struct,
	 * but we want to avoid the race with thread_group_cputime() which can
	 * see the empty ->thread_head list.
	 *
	 * 每个线程死亡时把自身 CPU、缺页、切换和 I/O 统计汇入共享 signal；
	 * 即使组长最后退出也照常累计，以免并发统计遍历看到空线程链表而漏算。
	 */
	task_cputime(tsk, &utime, &stime);
	write_seqlock(&sig->stats_lock);
	sig->utime += utime;
	sig->stime += stime;
	sig->gtime += task_gtime(tsk);
	sig->min_flt += tsk->min_flt;
	sig->maj_flt += tsk->maj_flt;
	sig->nvcsw += tsk->nvcsw;
	sig->nivcsw += tsk->nivcsw;
	sig->inblock += task_io_get_inblock(tsk);
	sig->oublock += task_io_get_oublock(tsk);
	task_io_accounting_add(&sig->ioac, &tsk->ioac);
	sig->sum_sched_runtime += tsk->se.sum_exec_runtime;
	sig->nr_threads--;
	__unhash_process(post, tsk, group_dead);
	write_sequnlock(&sig->stats_lock);

	/*
	 * Ensure that all preceeding state is visible. Pairs with
	 * the smp_acquire__after_ctrl_dep() in the sighand == NULL
	 * path of lock_task_sighand().
	 * release 写 NULL 既宣告信号处理结构不可再取得，也保证此前摘链和统计写入
	 * 对成功观察 NULL 的 acquire 读者可见。
	 */
	smp_store_release(&tsk->sighand, NULL);
	spin_unlock(&sighand->siglock);

	__cleanup_sighand(sighand);
	if (group_dead)
		tty_kref_put(tty);
}

static void delayed_put_task_struct(struct rcu_head *rhp)
{
	/* RCU 宽限期后清理仍可能按 task 指针索引的探针、性能及跟踪状态。 */
	struct task_struct *tsk = container_of(rhp, struct task_struct, rcu);

	kprobe_flush_task(tsk);
	rethook_flush_task(tsk);
	perf_event_delayed_put(tsk);
	trace_sched_process_free(tsk);
	put_task_struct(tsk);
}

void put_task_struct_rcu_user(struct task_struct *task)
{
	/* rcu_users 的最后持有者只排队回调，不能立即释放潜在 RCU 读者所见对象。 */
	if (refcount_dec_and_test(&task->rcu_users))
		call_rcu(&task->rcu, delayed_put_task_struct);
}

void __weak release_thread(struct task_struct *dead_task)
{
	/* 架构可覆盖此弱符号，释放体系结构专有的线程资源。 */
}

void release_task(struct task_struct *p)
{
	/*
	 * 这里执行进程身份层面的最终回收，而非当前线程自杀；调用者可以是父进程。
	 * tasklist_lock 内只做必须原子可见的摘链和父进程通知，可能睡眠或唤醒的
	 * proc、命名空间、PID 及 task_struct 释放均放在锁外。
	 */
	struct release_task_post post;
	struct task_struct *leader;
	struct pid *thread_pid;
	int zap_leader;
repeat:
	memset(&post, 0, sizeof(post));

	/* don't need to get the RCU readlock here - the process is dead and
	 * can't be modifying its own credentials. */
	/* 目标已经死亡，不会再自行修改凭据；这里撤销其 RLIMIT_NPROC 记账。 */
	dec_rlimit_ucounts(task_ucounts(p), UCOUNT_RLIMIT_NPROC, 1);

	pidfs_exit(p);
	cgroup_task_release(p);

	/* Retrieve @thread_pid before __unhash_process() may set it to NULL. */
	/* 在摘除 PID 关联前保存引用，供锁外刷新对应 proc 项。 */
	thread_pid = task_pid(p);

	write_lock_irq(&tasklist_lock);
	ptrace_release_task(p);
	__exit_signal(&post, p);

	/*
	 * If we are the last non-leader member of the thread
	 * group, and the leader is zombie, then notify the
	 * group leader's parent process. (if it wants notification.)
	 * 最后一个非组长线程消失后，先前因仍有子线程而不能回收的僵尸组长现在
	 * 可以通知父进程；若父进程忽略 SIGCHLD，则当前回收者还需连带释放组长。
	 */
	zap_leader = 0;
	leader = p->group_leader;
	if (leader != p && thread_group_empty(leader)
			&& leader->exit_state == EXIT_ZOMBIE) {
	/* for pidfs_exit() and do_notify_parent() */
	/* 组退出码同时供 pidfs 状态和父进程通知使用。 */
		if (leader->signal->flags & SIGNAL_GROUP_EXIT)
			leader->exit_code = leader->signal->group_exit_code;
		/*
		 * If we were the last child thread and the leader has
		 * exited already, and the leader's parent ignores SIGCHLD,
		 * then we are the one who should release the leader.
		 * 当前线程是最后一个子线程且组长早已退出时，若父进程忽略 SIGCHLD，
		 * 就由当前回收路径负责把组长也释放掉。
		 */
		zap_leader = do_notify_parent(leader, leader->exit_signal);
		if (zap_leader)
			leader->exit_state = EXIT_DEAD;
	}

	write_unlock_irq(&tasklist_lock);
	/* @thread_pid can't go away until free_pids() below */
	/* post.pids 尚持有引用，所以此处使用 thread_pid 刷新 proc 是安全的。 */
	proc_flush_pid(thread_pid);
	exit_cred_namespaces(p);
	add_device_randomness(&p->se.sum_exec_runtime,
			      sizeof(p->se.sum_exec_runtime));
	free_pids(post.pids);
	release_thread(p);
	/*
	 * This task was already removed from the process/thread/pid lists
	 * and lock_task_sighand(p) can't succeed. Nobody else can touch
	 * ->pending or, if group dead, signal->shared_pending. We can call
	 * flush_sigqueue() lockless.
	 * 目标已从所有可发现链表摘除且无法再取得 sighand，因此待决信号队列
	 * 已无并发写者，可以无锁清空。
	 */
	flush_sigqueue(&p->pending);
	if (thread_group_leader(p))
		flush_sigqueue(&p->signal->shared_pending);

	put_task_struct_rcu_user(p);

	p = leader;
	if (unlikely(zap_leader))
		goto repeat;
}

int rcuwait_wake_up(struct rcuwait *w)
{
	/* RCU 保护等待者指针的生命期；返回值沿用 wake_up_process() 的语义。 */
	int ret = 0;
	struct task_struct *task;

	rcu_read_lock();

	/*
	 * Order condition vs @task, such that everything prior to the load
	 * of @task is visible. This is the condition as to why the user called
	 * rcuwait_wake() in the first place. Pairs with set_current_state()
	 * barrier (A) in rcuwait_wait_event().
	 *
	 *    WAIT                WAKE
	 *    [S] tsk = current	  [S] cond = true
	 *        MB (A)	      MB (B)
	 *    [L] cond		  [L] tsk
	 *
	 * 该全屏障防止唤醒者先读到旧的 task=NULL：若等待者没看到条件成立，
	 * 唤醒者就必须看到等待者已经发布的 task 指针，避免丢失唤醒。
	 */
	smp_mb(); /* (B) */

	task = rcu_dereference(w->task);
	if (task)
		ret = wake_up_process(task);
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(rcuwait_wake_up);

/*
 * Determine if a process group is "orphaned", according to the POSIX
 * definition in 2.2.2.52.  Orphaned process groups are not to be affected
 * by terminal-generated stop signals.  Newly orphaned process groups are
 * to receive a SIGHUP and a SIGCONT.
 *
 * "I ask you, have you ever known what it is to be an orphan?"
 *
 * POSIX 孤儿进程组：组内不存在“同一会话、但位于组外”的父进程连接。
 * ignored_task 用于模拟某任务退出后的拓扑，已退出且无其他线程者不再算连接。
 */
static int will_become_orphaned_pgrp(struct pid *pgrp,
					struct task_struct *ignored_task)
{
	struct task_struct *p;

	do_each_pid_task(pgrp, PIDTYPE_PGID, p) {
		if ((p == ignored_task) ||
		    (p->exit_state && thread_group_empty(p)) ||
		    is_global_init(p->real_parent))
			continue;

		if (task_pgrp(p->real_parent) != pgrp &&
		    task_session(p->real_parent) == task_session(p))
			return 0;
	} while_each_pid_task(pgrp, PIDTYPE_PGID, p);

	return 1;
}

int is_current_pgrp_orphaned(void)
{
	/* tasklist_lock 读锁固定父子关系、会话和进程组成员关系。 */
	int retval;

	read_lock(&tasklist_lock);
	retval = will_become_orphaned_pgrp(task_pgrp(current), NULL);
	read_unlock(&tasklist_lock);

	return retval;
}

static bool has_stopped_jobs(struct pid *pgrp)
{
	/* 调用路径已持有 tasklist_lock；任一成员处于组停止状态即满足 POSIX 条件。 */
	struct task_struct *p;

	do_each_pid_task(pgrp, PIDTYPE_PGID, p) {
		if (p->signal->flags & SIGNAL_STOP_STOPPED)
			return true;
	} while_each_pid_task(pgrp, PIDTYPE_PGID, p);

	return false;
}

/*
 * Check to see if any process groups have become orphaned as
 * a result of our exiting, and if they have any stopped jobs,
 * send them a SIGHUP and then a SIGCONT. (POSIX 3.2.2.2)
 * 退出或重新收养导致进程组刚成为孤儿，且仍含停止作业时，先通知挂断，
 * 再继续执行，使作业有机会处理 SIGHUP 并完成退出。
 */
static void
kill_orphaned_pgrp(struct task_struct *tsk, struct task_struct *parent)
{
	struct pid *pgrp = task_pgrp(tsk);
	struct task_struct *ignored_task = tsk;

	if (!parent)
		/* exit: our father is in a different pgrp than
		 * we are and we were the only connection outside.
		 */
		/* 退出场景：父进程是本组连接到外部的唯一纽带。 */
		parent = tsk->real_parent;
	else
		/* reparent: our child is in a different pgrp than
		 * we are, and it was the only connection outside.
		 */
		/* 重新收养场景：被移动的孩子曾是其组连接到外部的唯一纽带。 */
		ignored_task = NULL;

	if (task_pgrp(parent) != pgrp &&
	    task_session(parent) == task_session(tsk) &&
	    will_become_orphaned_pgrp(pgrp, ignored_task) &&
	    has_stopped_jobs(pgrp)) {
		__kill_pgrp_info(SIGHUP, SEND_SIG_PRIV, pgrp);
		__kill_pgrp_info(SIGCONT, SEND_SIG_PRIV, pgrp);
	}
}

static void coredump_task_exit(struct task_struct *tsk,
			       struct core_state *core_state)
{
	/*
	 * core dumper 必须等同组相关线程到达稳定点。PF_SIGNALED 线程把栈上的
	 * core_thread 节点原子挂入 dumper 链表；随后睡眠，直到 coredump_finish()
	 * 清空 self.task。xchg 和 atomic_dec 的屏障保证 dumper 看见完整节点。
	 */
	struct core_thread self;

	self.task = tsk;
	if (self.task->flags & PF_SIGNALED)
		self.next = xchg(&core_state->dumper.next, &self);
	else
		self.task = NULL;
	/*
	 * Implies mb(), the result of xchg() must be visible
	 * to core_state->dumper.
	 * xchg 隐含全屏障，确保随后递减线程数前 dumper 已能看到刚挂入的节点。
	 */
	if (atomic_dec_and_test(&core_state->nr_threads))
		complete(&core_state->startup);

	for (;;) {
		set_current_state(TASK_IDLE|TASK_FREEZABLE);
		if (!self.task) /* see coredump_finish() */
			/* coredump_finish() 清空该指针，发布允许退出的条件。 */
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
}

#ifdef CONFIG_MEMCG
/* drops tasklist_lock if succeeds */
/* 成功时故意释放 tasklist_lock，把“找到候选者”和“交接 owner”合成一次验证。 */
static bool __try_to_set_owner(struct task_struct *tsk, struct mm_struct *mm)
{
	bool ret = false;

	task_lock(tsk);
	if (likely(tsk->mm == mm)) {
		/* tsk can't pass exit_mm/exec_mmap and exit */
		/* task_lock 下确认仍持有该 mm，它就不能同时越过 exit_mm/exec_mmap。 */
		read_unlock(&tasklist_lock);
		WRITE_ONCE(mm->owner, tsk);
		lru_gen_migrate_mm(mm);
		ret = true;
	}
	task_unlock(tsk);
	return ret;
}

static bool try_to_set_owner(struct task_struct *g, struct mm_struct *mm)
{
	/* 在线程组内寻找仍实际引用该 mm 的线程；遇到其他非空 mm 即可停止扫描。 */
	struct task_struct *t;

	for_each_thread(g, t) {
		struct mm_struct *t_mm = READ_ONCE(t->mm);
		if (t_mm == mm) {
			if (__try_to_set_owner(t, mm))
				return true;
		} else if (t_mm)
			break;
	}

	return false;
}

/*
 * A task is exiting.   If it owned this mm, find a new owner for the mm.
 * mm->owner 供内存控制组等记账使用，不持有 task_struct 引用，故原 owner
 * 退出/exec 时必须转交给仍共享该 mm 的任务，找不到则明确置 NULL。
 */
void mm_update_next_owner(struct mm_struct *mm)
{
	struct task_struct *g, *p = current;

	/*
	 * If the exiting or execing task is not the owner, it's
	 * someone else's problem.
	 * 只有当前 owner 才负责交接，非 owner 退出不应覆盖他人的所有权记录。
	 */
	if (mm->owner != p)
		return;
	/*
	 * The current owner is exiting/execing and there are no other
	 * candidates.  Do not leave the mm pointing to a possibly
	 * freed task structure.
	 * mm_users 只剩当前使用者时没有可交接对象，先清空悬空风险再返回。
	 */
	if (atomic_read(&mm->mm_users) <= 1) {
		WRITE_ONCE(mm->owner, NULL);
		return;
	}

	read_lock(&tasklist_lock);
	/*
	 * Search in the children
	 * 优先子进程、再兄弟、最后全局扫描，以常见共享关系降低查找开销。
	 */
	list_for_each_entry(g, &p->children, sibling) {
		if (try_to_set_owner(g, mm))
			goto ret;
	}
	/*
	 * Search in the siblings
	 * 子进程未命中后扫描同一父进程的兄弟任务。
	 */
	list_for_each_entry(g, &p->real_parent->children, sibling) {
		if (try_to_set_owner(g, mm))
			goto ret;
	}
	/*
	 * Search through everything else, we should not get here often.
	 * 最慢路径遍历全局进程，仅处理共享者不在常见亲缘关系中的少数情况。
	 */
	for_each_process(g) {
		if (atomic_read(&mm->mm_users) <= 1)
			break;
		if (g->flags & PF_KTHREAD)
			continue;
		if (try_to_set_owner(g, mm))
			goto ret;
	}
	read_unlock(&tasklist_lock);
	/*
	 * We found no owner yet mm_users > 1: this implies that we are
	 * most likely racing with swapoff (try_to_unuse()) or /proc or
	 * ptrace or page migration (get_task_mm()).  Mark owner as NULL.
	 * mm_users 的额外引用也可能来自短期内核用户而非任务，不能据此强行指定
	 * owner；NULL 是允许且安全的退化状态。
	 */
	WRITE_ONCE(mm->owner, NULL);
 ret:
	return;

}
#endif /* CONFIG_MEMCG */

#if defined(CONFIG_SCHED_CACHE) && defined(CONFIG_NUMA_BALANCING)
/*
 * Subtract the memory footprint of the current task from
 * mm.
 * 任务退出时从 mm 的调度缓存估计中扣除其 NUMA fault footprint。
 */
static void exit_mm_sched_cache(struct mm_struct *mm)
{
	unsigned long fp, sub;

	if (!current->total_numa_faults)
		return;
	/*
	 * No lock protection due to performance considerations.
	 * Make sure mm->sc_stat.footprint does not become
	 * negative.
	 * 该统计允许近似竞争以避免热路径加锁，并以 min() 保证无符号值不下溢。
	 */
	fp = READ_ONCE(mm->sc_stat.footprint);
	sub = min(fp, current->total_numa_faults);
	WRITE_ONCE(mm->sc_stat.footprint, fp - sub);
}
#else
static inline void exit_mm_sched_cache(struct mm_struct *mm)
{
}
#endif /* CONFIG_SCHED_CACHE CONFIG_NUMA_BALANCING */

/*
 * Turn us into a lazy TLB process if we
 * aren't already..
 * 清除 current->mm 后任务不再拥有用户地址空间，但 active_mm 可作为 lazy-TLB
 * 上下文暂留；mmgrab_lazy_tlb() 保证这段过渡期的 mm 生命周期。
 */
static void exit_mm(void)
{
	/* exit_mm_release 先处理 vfork/核心转储等依赖；内核线程式任务可能本就无 mm。 */
	struct mm_struct *mm = current->mm;

	exit_mm_release(current, mm);
	if (!mm)
		return;

	exit_mm_sched_cache(mm);

	mmap_read_lock(mm);
	mmgrab_lazy_tlb(mm);
	BUG_ON(mm != current->active_mm);
	/* more a memory barrier than a real lock */
	/* task_lock 主要提供与远端观察 current->mm 的顺序关系，而非保护复杂数据。 */
	task_lock(current);
	/*
	 * When a thread stops operating on an address space, the loop
	 * in membarrier_private_expedited() may not observe that
	 * tsk->mm, and the loop in membarrier_global_expedited() may
	 * not observe a MEMBARRIER_STATE_GLOBAL_EXPEDITED
	 * rq->membarrier_state, so those would not issue an IPI.
	 * Membarrier requires a memory barrier after accessing
	 * user-space memory, before clearing tsk->mm or the
	 * rq->membarrier_state.
	 * 在清空 mm/运行队列 membarrier 状态前完成一次全序屏障，保证此前用户内存
	 * 访问先完成；否则 expedited membarrier 可能既看不到 mm 又漏发 IPI。
	 */
	smp_mb__after_spinlock();
	local_irq_disable();
	current->mm = NULL;
	membarrier_update_current_mm(NULL);
	enter_lazy_tlb(mm, current);
	local_irq_enable();
	task_unlock(current);
	mmap_read_unlock(mm);
	mm_update_next_owner(mm);
	mmput(mm);
	if (test_thread_flag(TIF_MEMDIE))
		exit_oom_victim();
}

static struct task_struct *find_alive_thread(struct task_struct *p)
{
	/* 返回同组中尚未进入 PF_EXITING 的线程；调用者持有 tasklist_lock。 */
	struct task_struct *t;

	for_each_thread(p, t) {
		if (!(t->flags & PF_EXITING))
			return t;
	}
	return NULL;
}

static struct task_struct *find_child_reaper(struct task_struct *father,
						struct list_head *dead)
	__releases(&tasklist_lock)
	__acquires(&tasklist_lock)
{
	/*
	 * 若退出者不是 PID 命名空间 reaper，直接使用现任 reaper；若它正是 reaper，
	 * 优先把职责交给同组活线程。整个组都退出时必须锁外释放 dead 链表并终止
	 * 命名空间内进程，随后重新取得 tasklist_lock。
	 */
	struct pid_namespace *pid_ns = task_active_pid_ns(father);
	struct task_struct *reaper = pid_ns->child_reaper;
	struct task_struct *p, *n;

	if (likely(reaper != father))
		return reaper;

	reaper = find_alive_thread(father);
	if (reaper) {
		ASSERT_EXCLUSIVE_WRITER(pid_ns->child_reaper);
		WRITE_ONCE(pid_ns->child_reaper, reaper);
		return reaper;
	}

	write_unlock_irq(&tasklist_lock);

	list_for_each_entry_safe(p, n, dead, ptrace_entry) {
		list_del_init(&p->ptrace_entry);
		release_task(p);
	}

	zap_pid_ns_processes(pid_ns);
	write_lock_irq(&tasklist_lock);

	return father;
}

/*
 * When we die, we re-parent all our children, and try to:
 * 1. give them to another thread in our thread group, if such a member exists
 * 2. give it to the first ancestor process which prctl'd itself as a
 *    child_subreaper for its children (like a service manager)
 * 3. give it to the init process (PID 1) in our pid namespace
 * 重新收养顺序是同线程组、同 PID 命名空间内最近的 child_subreaper、命名空间
 * init。pid level 检查防止 setns()+fork() 形成的祖先链跨越命名空间边界。
 */
static struct task_struct *find_new_reaper(struct task_struct *father,
					   struct task_struct *child_reaper)
{
	struct task_struct *thread, *reaper;

	thread = find_alive_thread(father);
	if (thread)
		return thread;

	if (father->signal->has_child_subreaper) {
		unsigned int ns_level = task_pid(father)->level;
		/*
		 * Find the first ->is_child_subreaper ancestor in our pid_ns.
		 * We can't check reaper != child_reaper to ensure we do not
		 * cross the namespaces, the exiting parent could be injected
		 * by setns() + fork().
		 * We check pid->level, this is slightly more efficient than
		 * task_active_pid_ns(reaper) != task_active_pid_ns(father).
		 * 沿 real_parent 找同 PID 层级内最近的 child_subreaper；不能只比较
		 * child_reaper，因为 setns()+fork() 可构造跨命名空间的祖先链。
		 */
		for (reaper = father->real_parent;
		     task_pid(reaper)->level == ns_level;
		     reaper = reaper->real_parent) {
			if (reaper == &init_task)
				break;
			if (!reaper->signal->is_child_subreaper)
				continue;
			thread = find_alive_thread(reaper);
			if (thread)
				return thread;
		}
	}

	return child_reaper;
}

/*
* Any that need to be release_task'd are put on the @dead list.
 * 需要最终 release_task() 的子进程只加入 @dead，待释放 tasklist_lock 后处理。
 */
static void reparent_leader(struct task_struct *father, struct task_struct *p,
				struct list_head *dead)
{
	/* 新父进程只按 SIGCHLD 接收通知，防止沿用 clone 的特殊退出信号伤害 init。 */
	if (unlikely(p->exit_state == EXIT_DEAD))
		return;

	/* We don't want people slaying init. */
	/* 强制改为 SIGCHLD，避免特殊 clone 退出信号导致新 reaper（尤其 init）被杀。 */
	p->exit_signal = SIGCHLD;

	/* If it has exited notify the new parent about this child's death. */
	/* 已成僵尸且无线程/ptrace 时立即通知新父；无需保留则加入 dead 链表。 */
	if (!p->ptrace &&
	    p->exit_state == EXIT_ZOMBIE && thread_group_empty(p)) {
		if (do_notify_parent(p, p->exit_signal)) {
			p->exit_state = EXIT_DEAD;
			list_add(&p->ptrace_entry, dead);
		}
	}

	kill_orphaned_pgrp(p, father);
}

/*
 * Make init inherit all the child processes
 * 名称沿用历史；实际继承者也可能是同组活线程或 child_subreaper。
 */
static void forget_original_parent(struct task_struct *father,
					struct list_head *dead)
{
	struct task_struct *p, *t, *reaper;

	if (unlikely(!list_empty(&father->ptraced)))
		exit_ptrace(father, dead);

	/* Can drop and reacquire tasklist_lock */
	/* find_child_reaper() 在 PID 命名空间退出路径可能临时放锁，返回时已重新持有。 */
	reaper = find_child_reaper(father, dead);
	if (list_empty(&father->children))
		return;

	reaper = find_new_reaper(father, reaper);
	list_for_each_entry(p, &father->children, sibling) {
		for_each_thread(p, t) {
			RCU_INIT_POINTER(t->real_parent, reaper);
			BUG_ON((!t->ptrace) != (rcu_access_pointer(t->parent) == father));
			if (likely(!t->ptrace))
				t->parent = t->real_parent;
			if (t->pdeath_signal)
				group_send_sig_info(t->pdeath_signal,
						    SEND_SIG_NOINFO, t,
						    PIDTYPE_TGID);
		}
		/*
		 * If this is a threaded reparent there is no need to
		 * notify anyone anything has happened.
		 * 同线程组内换父只改变具体 task 指针，进程语义上的父线程组未变。
		 */
		if (!same_thread_group(reaper, father))
			reparent_leader(father, p, dead);
	}
	list_splice_tail_init(&father->children, &reaper->children);
}

/*
 * Send signals to all our closest relatives so that they know
 * to properly mourn us..
 * 在 tasklist_lock 下完成重新收养、状态发布和父进程通知；需要实际释放的任务
 * 暂存 dead 链表，锁外调用 release_task()，避免锁层级和长临界区。
 */
static void exit_notify(struct task_struct *tsk, int group_dead)
{
	bool autoreap;
	struct task_struct *p, *n;
	LIST_HEAD(dead);

	write_lock_irq(&tasklist_lock);
	forget_original_parent(tsk, &dead);

	if (group_dead)
		kill_orphaned_pgrp(tsk->group_leader, NULL);

	tsk->exit_state = EXIT_ZOMBIE;

	if (unlikely(tsk->ptrace)) {
		int sig = thread_group_empty(tsk) && !ptrace_reparented(tsk)
			  ? tsk->exit_signal : SIGCHLD;
		autoreap = do_notify_parent(tsk, sig);
	} else if (thread_group_leader(tsk)) {
		autoreap = thread_group_empty(tsk) &&
			   do_notify_parent(tsk, tsk->exit_signal);
	} else {
		autoreap = true;
		/* untraced sub-thread */
		/* 非组长且未跟踪的线程不形成父进程可等待的独立僵尸，只通知 pidfd。 */
		do_notify_pidfd(tsk);
	}

	if (autoreap) {
		/* 父进程不需要保留退出信息时，直接取得 EXIT_DEAD 状态并延后释放。 */
		tsk->exit_state = EXIT_DEAD;
		list_add(&tsk->ptrace_entry, &dead);
	}

	/* mt-exec, de_thread() is waiting for group leader */
	/* 多线程 exec 的 de_thread() 用负 notify_count 表示正等待旧组长退出。 */
	if (unlikely(tsk->signal->notify_count < 0))
		wake_up_process(tsk->signal->group_exec_task);
	write_unlock_irq(&tasklist_lock);

	list_for_each_entry_safe(p, n, &dead, ptrace_entry) {
		list_del_init(&p->ptrace_entry);
		release_task(p);
	}
}

#ifdef CONFIG_DEBUG_STACK_USAGE
#ifdef CONFIG_STACK_GROWSUP
unsigned long stack_not_used(struct task_struct *p)
{
	/* 向低地址增长的栈从末端向回扫描；越过 canary 后首个非零值界定用量。 */
	unsigned long *n = end_of_stack(p);

	do {	/* Skip over canary */
		n--;
	} while (!*n);

	return (unsigned long)end_of_stack(p) - (unsigned long)n;
}
#else /* !CONFIG_STACK_GROWSUP */
unsigned long stack_not_used(struct task_struct *p)
{
	/* 向高地址增长时反向选择扫描方向，返回从 end_of_stack 起的空闲字节数。 */
	unsigned long *n = end_of_stack(p);

	do {	/* Skip over canary */
		n++;
	} while (!*n);

	return (unsigned long)n - (unsigned long)end_of_stack(p);
}
#endif /* CONFIG_STACK_GROWSUP */

/* Count the maximum pages reached in kernel stacks */
/* 按使用深度分桶累计 VM 事件，便于评估 THREAD_SIZE 是否留有足够余量。 */
static inline void kstack_histogram(unsigned long used_stack)
{
#ifdef CONFIG_VM_EVENT_COUNTERS
	if (used_stack <= 1024)
		count_vm_event(KSTACK_1K);
#if THREAD_SIZE > 1024
	else if (used_stack <= 2048)
		count_vm_event(KSTACK_2K);
#endif
#if THREAD_SIZE > 2048
	else if (used_stack <= 4096)
		count_vm_event(KSTACK_4K);
#endif
#if THREAD_SIZE > 4096
	else if (used_stack <= 8192)
		count_vm_event(KSTACK_8K);
#endif
#if THREAD_SIZE > 8192
	else if (used_stack <= 16384)
		count_vm_event(KSTACK_16K);
#endif
#if THREAD_SIZE > 16384
	else if (used_stack <= 32768)
		count_vm_event(KSTACK_32K);
#endif
#if THREAD_SIZE > 32768
	else if (used_stack <= 65536)
		count_vm_event(KSTACK_64K);
#endif
#if THREAD_SIZE > 65536
	else
		count_vm_event(KSTACK_REST);
#endif
#endif /* CONFIG_VM_EVENT_COUNTERS */
}

static void check_stack_usage(void)
{
	/* 全局最低剩余栈只在刷新纪录时加锁；无竞争的常见路径只读后直接返回。 */
	static DEFINE_SPINLOCK(low_water_lock);
	static int lowest_to_date = THREAD_SIZE;
	unsigned long free;

	free = stack_not_used(current);
	kstack_histogram(THREAD_SIZE - free);

	if (free >= lowest_to_date)
		return;

	spin_lock(&low_water_lock);
	if (free < lowest_to_date) {
		pr_info("%s (%d) used greatest stack depth: %lu bytes left\n",
			current->comm, task_pid_nr(current), free);
		lowest_to_date = free;
	}
	spin_unlock(&low_water_lock);
}
#else /* !CONFIG_DEBUG_STACK_USAGE */
static inline void check_stack_usage(void) {}
#endif /* CONFIG_DEBUG_STACK_USAGE */

static void synchronize_group_exit(struct task_struct *tsk, long code)
{
	/*
	 * quick_threads 在 siglock 下选出第一个确定组退出结果的线程；同一把锁还把
	 * PF_POSTCOREDUMP 与 core_state 建立原子快照，防止 core dumper 漏等线程。
	 */
	struct sighand_struct *sighand = tsk->sighand;
	struct signal_struct *signal = tsk->signal;
	struct core_state *core_state;

	spin_lock_irq(&sighand->siglock);
	signal->quick_threads--;
	if ((signal->quick_threads == 0) &&
	    !(signal->flags & SIGNAL_GROUP_EXIT)) {
		signal->flags = SIGNAL_GROUP_EXIT;
		signal->group_exit_code = code;
		signal->group_stop_count = 0;
	}
	/*
	 * Serialize with any possible pending coredump.
	 * We must hold siglock around checking core_state
	 * and setting PF_POSTCOREDUMP.  The core-inducing thread
	 * will increment ->nr_threads for each thread in the
	 * group without PF_POSTCOREDUMP set.
	 * siglock 把 core_state 检查与 PF_POSTCOREDUMP 发布串行化，保证 dumper 统计
	 * 线程时不会漏掉正越过退出边界的任务。
	 */
	tsk->flags |= PF_POSTCOREDUMP;
	core_state = signal->core_state;
	spin_unlock_irq(&sighand->siglock);

	if (unlikely(core_state))
		coredump_task_exit(tsk, core_state);
}

void __noreturn do_exit(long code)
{
	/*
	 * 退出主路径只允许当前任务调用且永不返回。顺序要点是：先禁止新工作并固定
	 * 统计，再销毁 mm/files/fs/命名空间等资源，最后发布死亡状态、退出 RCU
	 * 可见范围并交给调度器。任何可选记账失败都不能中断这条不可逆清理链。
	 */
	struct task_struct *tsk = current;
	struct kthread *kthread;
	int group_dead;

	WARN_ON(irqs_disabled());
	WARN_ON(tsk->plug);

	kthread = tsk_is_kthread(tsk);
	if (unlikely(kthread))
		kthread_do_exit(kthread, code);

	kcov_task_exit(tsk);
	kmsan_task_exit(tsk);

	synchronize_group_exit(tsk, code);
	ptrace_event(PTRACE_EVENT_EXIT, code);
	user_events_exit(tsk);

	io_uring_files_cancel();
	sched_mm_cid_exit(tsk);
	exit_signals(tsk);  /* sets PF_EXITING */
	/* PF_EXITING 是禁止其他子系统再向该任务附加长期工作的发布点。 */

	seccomp_filter_release(tsk);

	acct_update_integrals(tsk);
	group_dead = atomic_dec_and_test(&tsk->signal->live);
	if (group_dead) {
		/*
		 * If the last thread of global init has exited, panic
		 * immediately to get a useable coredump.
		 * 全局 init 是系统回收链根节点，最后线程退出后无法继续维持用户空间，
		 * 立即 panic 还能保留较完整的故障现场。
		 */
		if (unlikely(is_global_init(tsk)))
			panic("Attempted to kill init! exitcode=0x%08x\n",
				tsk->signal->group_exit_code ?: (int)code);

#ifdef CONFIG_POSIX_TIMERS
		hrtimer_cancel(&tsk->signal->real_timer);
		exit_itimers(tsk);
#endif
		if (tsk->mm)
			setmax_mm_hiwater_rss(&tsk->signal->maxrss, tsk->mm);
	}
	acct_collect(code, group_dead);
	if (group_dead)
		tty_audit_exit();
	audit_free(tsk);

	tsk->exit_code = code;
	/*
	 * taskstats 必须位于 exit_mm()/exit_files() 之前：它还要读取最终记账、
	 * exe file、PID/TGID 和 signal->stats。group_dead 表明本任务是否把
	 * signal->live 减到零；最后线程可同时产生 PID 记录和完整 TGID 记录。
	 * Netlink 分配或投递失败只丢统计事件，不得阻止 do_exit() 继续释放。
	 */
	taskstats_exit(tsk, group_dead);
	trace_sched_process_exit(tsk, group_dead);

	/*
	 * Since sampling can touch ->mm, make sure to stop everything before we
	 * tear it down.
	 *
	 * Also flushes inherited counters to the parent - before the parent
	 * gets woken up by child-exit notifications.
	 * perf 采样可能访问 mm，必须在 exit_mm 前停止；同时先把继承统计汇入父级，
	 * 保证父进程被 SIGCHLD 唤醒后读到的是最终值。
	 */
	perf_event_exit_task(tsk);
	/*
	 * PF_EXITING (above) ensures unwind_deferred_request() will no
	 * longer add new unwinds. While exit_mm() (below) will destroy the
	 * abaility to do unwinds. So flush any pending unwinds here.
	 * PF_EXITING 阻止新增延迟回溯，随后清空现有请求，才能安全销毁其地址空间。
	 */
	unwind_deferred_task_exit(tsk);

	exit_mm();

	if (group_dead)
		acct_process();

	exit_sem(tsk);
	exit_shm(tsk);
	exit_files(tsk);
	exit_fs(tsk);
	if (group_dead)
		disassociate_ctty(1);
	exit_nsproxy_namespaces(tsk);
	exit_task_work(tsk);
	exit_thread(tsk);

	sched_autogroup_exit_task(tsk);
	cgroup_task_exit(tsk);

	/*
	 * FIXME: do that only when needed, using sched_exit tracepoint
	 * 当前无条件清除 ptrace 硬件断点，未来可由调度退出跟踪点按需触发。
	 */
	flush_ptrace_hw_breakpoint(tsk);

	exit_tasks_rcu_start();
	exit_notify(tsk, group_dead);
	proc_exit_connector(tsk);
	mpol_put_task_policy(tsk);
#ifdef CONFIG_FUTEX
	if (unlikely(current->futex.pi_state_cache))
		kfree(current->futex.pi_state_cache);
#endif
	/*
	 * Make sure we are holding no locks:
	 * 进入最终不可调度返回阶段前，用 lockdep 检查是否把锁泄漏到死亡任务中。
	 */
	debug_check_no_locks_held();

	if (tsk->io_context)
		exit_io_context(tsk);

	if (tsk->splice_pipe)
		/* splice_pipe 为任务私有的缓存管道，退出时归还其中页和管道对象。 */
		free_pipe_info(tsk->splice_pipe);

	if (tsk->task_frag.page)
		/* 网络发送路径可能留下 task_frag 页引用，必须在 task_struct 消失前释放。 */
		put_page(tsk->task_frag.page);

	exit_task_stack_account(tsk);

	check_stack_usage();
	preempt_disable();
	/* 最终 RCU 退出和 do_task_dead() 要求不再被普通抢占路径迁移或返回。 */
	if (tsk->nr_dirtied)
		__this_cpu_add(dirty_throttle_leaks, tsk->nr_dirtied);
	exit_rcu();
	exit_tasks_rcu_finish();

	lockdep_free_task(tsk);
	do_task_dead();
}
EXPORT_SYMBOL(do_exit);

void __noreturn make_task_dead(int signr)
{
	/*
	 * Take the task off the cpu after something catastrophic has
	 * happened.
	 *
	 * We can get here from a kernel oops, sometimes with preemption off.
	 * Start by checking for critical errors.
	 * Then fix up important state like USER_DS and preemption.
	 * Then do everything else.
	 *
	 * 灾难退出可能从 oops 且 IRQ/抢占状态损坏的上下文进入，因此先排除中断
	 * 处理程序和 idle 任务，再尽量修复 IRQ、preempt_count，最后才走 do_exit()。
	 */
	struct task_struct *tsk = current;
	unsigned int limit;

	if (unlikely(in_interrupt()))
		panic("Aiee, killing interrupt handler!");
	if (unlikely(!tsk->pid))
		panic("Attempted to kill the idle task!");

	if (unlikely(irqs_disabled())) {
		pr_info("note: %s[%d] exited with irqs disabled\n",
			current->comm, task_pid_nr(current));
		local_irq_enable();
	}
	if (unlikely(in_atomic())) {
		pr_info("note: %s[%d] exited with preempt_count %d\n",
			current->comm, task_pid_nr(current),
			preempt_count());
		preempt_count_set(PREEMPT_ENABLED);
	}

	/*
	 * Every time the system oopses, if the oops happens while a reference
	 * to an object was held, the reference leaks.
	 * If the oops doesn't also leak memory, repeated oopsing can cause
	 * reference counters to wrap around (if they're not using refcount_t).
	 * This means that repeated oopsing can make unexploitable-looking bugs
	 * exploitable through repeated oopsing.
	 * To make sure this can't happen, place an upper bound on how often the
	 * kernel may oops without panic().
	 *
	 * oops 往往中断正常清理并泄漏引用；反复触发可能使非 refcount_t 计数回绕，
	 * 将原本不可利用的缺陷变成 UAF。达到上限后 panic 阻断这类累积攻击。
	 */
	limit = READ_ONCE(oops_limit);
	if (atomic_inc_return(&oops_count) >= limit && limit)
		panic("Oopsed too often (kernel.oops_limit is %d)", limit);

	/*
	 * We're taking recursive faults here in make_task_dead. Safest is to just
	 * leave this task alone and wait for reboot.
	 * PF_EXITING 表示 do_exit 自身再次故障；继续重复清理可能双重释放，因此只
	 * 修复 futex 状态、固定为 DEAD，并永久离开调度。
	 */
	if (unlikely(tsk->flags & PF_EXITING)) {
		pr_alert("Fixing recursive fault but reboot is needed!\n");
		futex_exit_recursive(tsk);
		tsk->exit_state = EXIT_DEAD;
		refcount_inc(&tsk->rcu_users);
		preempt_disable();
		do_task_dead();
	}

	do_exit(signr);
}

SYSCALL_DEFINE1(exit, int, error_code)
{
	/* wait 状态使用高 8 位保存正常退出码，低位预留给终止信号/core 标记。 */
	do_exit((error_code&0xff)<<8);
}

/*
 * Take down every thread in the group.  This is called by fatal signals
 * as well as by sys_exit_group (below).
 * 组退出码在 siglock 下只由首个线程确定，随后 zap_other_threads() 终止同组
 * 其他线程；后来者复用已发布的 group_exit_code。
 */
void __noreturn
do_group_exit(int exit_code)
{
	struct signal_struct *sig = current->signal;

	if (sig->flags & SIGNAL_GROUP_EXIT)
		exit_code = sig->group_exit_code;
	else if (sig->group_exec_task)
		exit_code = 0;
	else {
		struct sighand_struct *const sighand = current->sighand;

		spin_lock_irq(&sighand->siglock);
		if (sig->flags & SIGNAL_GROUP_EXIT)
			/* Another thread got here before we took the lock.  */
			/* 加锁前检查只是快路径；锁内必须重查以处理并发首发者。 */
			exit_code = sig->group_exit_code;
		else if (sig->group_exec_task)
			exit_code = 0;
		else {
			sig->group_exit_code = exit_code;
			sig->flags = SIGNAL_GROUP_EXIT;
			zap_other_threads(current);
		}
		spin_unlock_irq(&sighand->siglock);
	}

	do_exit(exit_code);
	/* NOTREACHED */
}

/*
 * this kills every thread in the thread group. Note that any externally
 * wait4()-ing process will get the correct exit code - even if this
 * thread is not the thread group leader.
 * exit_group 的可见退出状态属于整个线程组，因此无论哪个线程发起，外部
 * wait4() 最终取得的都是 group_exit_code。
 */
SYSCALL_DEFINE1(exit_group, int, error_code)
{
	do_group_exit((error_code & 0xff) << 8);
	/* NOTREACHED */
	return 0;
}

static int eligible_pid(struct wait_opts *wo, struct task_struct *p)
{
	/* PIDTYPE_MAX 表示 P_ALL；其余类型比较 PID/TGID/PGID 对应的 struct pid。 */
	return	wo->wo_type == PIDTYPE_MAX ||
		task_pid_type(p, wo->wo_type) == wo->wo_pid;
}

static int
eligible_child(struct wait_opts *wo, bool ptrace, struct task_struct *p)
{
	/* 先筛 PID 范围，再应用 Linux 对 clone 子进程和 ptrace 子进程的等待规则。 */
	if (!eligible_pid(wo, p))
		return 0;

	/*
	 * Wait for all children (clone and not) if __WALL is set or
	 * if it is traced by us.
	 * __WALL 或作为 ptracer 等待时，不区分普通 SIGCHLD 子进程与 clone 子进程。
	 */
	if (ptrace || (wo->wo_flags & __WALL))
		return 1;

	/*
	 * Otherwise, wait for clone children *only* if __WCLONE is set;
	 * otherwise, wait for non-clone children *only*.
	 *
	 * Note: a "clone" child here is one that reports to its parent
	 * using a signal other than SIGCHLD, or a non-leader thread which
	 * we can only see if it is traced by us.
	 * “clone child”在此按退出信号分类；异或表达式使 __WCLONE 只选择非
	 * SIGCHLD 子进程，未设置时只选择普通子进程。
	 */
	if ((p->exit_signal != SIGCHLD) ^ !!(wo->wo_flags & __WCLONE))
		return 0;

	return 1;
}

/*
 * Handle sys_wait4 work for one task in state EXIT_ZOMBIE.  We hold
 * read_lock(&tasklist_lock) on entry.  If we return zero, we still hold
 * the lock and this task is uninteresting.  If we return nonzero, we have
 * released the lock and the system call should return.
 * 入参 p 已是 EXIT_ZOMBIE。返回 0 时调用者仍持有 tasklist_lock 读锁；返回
 * PID 时本函数已经放锁并完成一次退出事件消费，这是 wait 扫描的核心契约。
 */
static int wait_task_zombie(struct wait_opts *wo, struct task_struct *p)
{
	int state, status;
	pid_t pid = task_pid_vnr(p);
	uid_t uid = from_kuid_munged(current_user_ns(), task_uid(p));
	struct waitid_info *infop;

	if (!likely(wo->wo_flags & WEXITED))
		return 0;

	if (unlikely(wo->wo_flags & WNOWAIT)) {
		/* 只窥视退出信息，不取得回收权；临时 task 引用允许锁外读取 rusage。 */
		status = (p->signal->flags & SIGNAL_GROUP_EXIT)
			? p->signal->group_exit_code : p->exit_code;
		get_task_struct(p);
		read_unlock(&tasklist_lock);
		sched_annotate_sleep();
		if (wo->wo_rusage)
			getrusage(p, RUSAGE_BOTH, wo->wo_rusage);
		put_task_struct(p);
		goto out_info;
	}
	/*
	 * Move the task's state to DEAD/TRACE, only one thread can do this.
	 * cmpxchg 同时仲裁多个 waiter：只有一方能消费。被重新指定父进程的 ptrace
	 * 组长先进入 EXIT_TRACE，稍后再把死亡级联给真实父进程。
	 */
	state = (ptrace_reparented(p) && thread_group_leader(p)) ?
		EXIT_TRACE : EXIT_DEAD;
	if (cmpxchg(&p->exit_state, EXIT_ZOMBIE, state) != EXIT_ZOMBIE)
		return 0;
	/*
	 * We own this thread, nobody else can reap it.
	 * 状态转换成功即取得唯一回收权，可释放 tasklist_lock 做可能睡眠的统计。
	 */
	read_unlock(&tasklist_lock);
	sched_annotate_sleep();

	/*
	 * Check thread_group_leader() to exclude the traced sub-threads.
	 * 只有完整线程组的自然回收才累计子进程统计；单独跟踪的非组长线程不代表
	 * 一个可计入 children 的进程。
	 */
	if (state == EXIT_DEAD && thread_group_leader(p)) {
		struct signal_struct *sig = p->signal;
		struct signal_struct *psig = current->signal;
		unsigned long maxrss;
		u64 tgutime, tgstime;

		/*
		 * The resource counters for the group leader are in its
		 * own task_struct.  Those for dead threads in the group
		 * are in its signal_struct, as are those for the child
		 * processes it has previously reaped.  All these
		 * accumulate in the parent's signal_struct c* fields.
		 *
		 * We don't bother to take a lock here to protect these
		 * p->signal fields because the whole thread group is dead
		 * and nobody can change them.
		 *
		 * psig->stats_lock also protects us from our sub-threads
		 * which can reap other children at the same time.
		 *
		 * We use thread_group_cputime_adjusted() to get times for
		 * the thread group, which consolidates times for all threads
		 * in the group including the group leader.
		 *
		 * 已死线程累计值位于 signal，组长自身仍在 task_struct，历史子孙统计在
		 * c* 字段；三部分合并进当前父进程。目标组已全死，无需锁其统计，但父
		 * 进程的多个 waiter 可并发回收，故用 psig->stats_lock 串行累计。
		 */
		thread_group_cputime_adjusted(p, &tgutime, &tgstime);
		write_seqlock_irq(&psig->stats_lock);
		psig->cutime += tgutime + sig->cutime;
		psig->cstime += tgstime + sig->cstime;
		psig->cgtime += task_gtime(p) + sig->gtime + sig->cgtime;
		psig->cmin_flt +=
			p->min_flt + sig->min_flt + sig->cmin_flt;
		psig->cmaj_flt +=
			p->maj_flt + sig->maj_flt + sig->cmaj_flt;
		psig->cnvcsw +=
			p->nvcsw + sig->nvcsw + sig->cnvcsw;
		psig->cnivcsw +=
			p->nivcsw + sig->nivcsw + sig->cnivcsw;
		psig->cinblock +=
			task_io_get_inblock(p) +
			sig->inblock + sig->cinblock;
		psig->coublock +=
			task_io_get_oublock(p) +
			sig->oublock + sig->coublock;
		maxrss = max(sig->maxrss, sig->cmaxrss);
		if (psig->cmaxrss < maxrss)
			psig->cmaxrss = maxrss;
		task_io_accounting_add(&psig->ioac, &p->ioac);
		task_io_accounting_add(&psig->ioac, &sig->ioac);
		write_sequnlock_irq(&psig->stats_lock);
	}

	if (wo->wo_rusage)
		getrusage(p, RUSAGE_BOTH, wo->wo_rusage);
	status = (p->signal->flags & SIGNAL_GROUP_EXIT)
		? p->signal->group_exit_code : p->exit_code;
	wo->wo_stat = status;

	if (state == EXIT_TRACE) {
		/* ptracer 消费后解除跟踪；真实父进程要等待则恢复 ZOMBIE，否则转 DEAD。 */
		write_lock_irq(&tasklist_lock);
		/* We dropped tasklist, ptracer could die and untrace */
		/* 放锁期间 ptracer 可能死亡并解除跟踪，故重新持写锁后先规范化关系。 */
		ptrace_unlink(p);

		/* If parent wants a zombie, don't release it now */
		/* 真实父进程需要退出信息时恢复 ZOMBIE，否则才允许 DEAD 回收。 */
		state = EXIT_ZOMBIE;
		if (do_notify_parent(p, p->exit_signal))
			state = EXIT_DEAD;
		p->exit_state = state;
		write_unlock_irq(&tasklist_lock);
	}
	if (state == EXIT_DEAD)
		release_task(p);

out_info:
	/* waitid 使用结构化 cause/status；传统 wait4 通过 wo_stat 返回编码状态。 */
	infop = wo->wo_info;
	if (infop) {
		if ((status & 0x7f) == 0) {
			infop->cause = CLD_EXITED;
			infop->status = status >> 8;
		} else {
			infop->cause = (status & 0x80) ? CLD_DUMPED : CLD_KILLED;
			infop->status = status & 0x7f;
		}
		infop->pid = pid;
		infop->uid = uid;
	}

	return pid;
}

static int *task_stopped_code(struct task_struct *p, bool ptrace)
{
	/* ptrace 停止是每线程 exit_code；作业控制停止是线程组共享 group_exit_code。 */
	if (ptrace) {
		if (task_is_traced(p) && !(p->jobctl & JOBCTL_LISTENING))
			return &p->exit_code;
	} else {
		if (p->signal->flags & SIGNAL_STOP_STOPPED)
			return &p->signal->group_exit_code;
	}
	return NULL;
}

/**
 * wait_task_stopped - Wait for %TASK_STOPPED or %TASK_TRACED
 * @wo: wait options
 * @ptrace: is the wait for ptrace
 * @p: task to wait for
 *
 * Handle sys_wait4() work for %p in state %TASK_STOPPED or %TASK_TRACED.
 *
 * CONTEXT:
 * read_lock(&tasklist_lock), which is released if return value is
 * non-zero.  Also, grabs and releases @p->sighand->siglock.
 *
 * RETURNS:
 * 0 if wait condition didn't exist and search for other wait conditions
 * should continue.  Non-zero return, -errno on failure and @p's pid on
 * success, implies that tasklist_lock is released and wait condition
 * search should terminate.
 *
 * 处理 TASK_STOPPED/TASK_TRACED。入口持有 tasklist_lock 读锁并
 * 短暂取得目标 siglock；0 表示继续扫描且仍持锁，非 0 表示已放锁并终止扫描。
 */
static int wait_task_stopped(struct wait_opts *wo,
				int ptrace, struct task_struct *p)
{
	struct waitid_info *infop;
	int exit_code, *p_code, why;
	uid_t uid = 0; /* unneeded, required by compiler */
	/* 某些分支不读取 uid，但编译器的数据流要求先初始化。 */
	pid_t pid;

	/*
	 * Traditionally we see ptrace'd stopped tasks regardless of options.
	 * ptrace 等待历史上不要求 WUNTRACED；自然父进程只有显式请求才看停止事件。
	 */
	if (!ptrace && !(wo->wo_flags & WUNTRACED))
		return 0;

	if (!task_stopped_code(p, ptrace))
		return 0;

	exit_code = 0;
	spin_lock_irq(&p->sighand->siglock);

	p_code = task_stopped_code(p, ptrace);
	/* 首次无锁检查仅作快筛，取得 siglock 后必须重查并原子消费停止码。 */
	if (unlikely(!p_code))
		goto unlock_sig;

	exit_code = *p_code;
	if (!exit_code)
		goto unlock_sig;

	if (!unlikely(wo->wo_flags & WNOWAIT))
		*p_code = 0;

	uid = from_kuid_munged(current_user_ns(), task_uid(p));
unlock_sig:
	spin_unlock_irq(&p->sighand->siglock);
	if (!exit_code)
		return 0;

	/*
	 * Now we are pretty sure this task is interesting.
	 * Make sure it doesn't get reaped out from under us while we
	 * give up the lock and then examine it below.  We don't want to
	 * keep holding onto the tasklist_lock while we call getrusage and
	 * possibly take page faults for user memory.
	 * 临时增加 task 引用后即可放 tasklist_lock；getrusage 及用户复制可能睡眠，
	 * 不能在全局任务链表读锁内执行。
	 */
	get_task_struct(p);
	pid = task_pid_vnr(p);
	why = ptrace ? CLD_TRAPPED : CLD_STOPPED;
	read_unlock(&tasklist_lock);
	sched_annotate_sleep();
	if (wo->wo_rusage)
		getrusage(p, RUSAGE_BOTH, wo->wo_rusage);
	put_task_struct(p);

	if (likely(!(wo->wo_flags & WNOWAIT)))
		wo->wo_stat = (exit_code << 8) | 0x7f;

	infop = wo->wo_info;
	if (infop) {
		infop->cause = why;
		infop->status = exit_code;
		infop->pid = pid;
		infop->uid = uid;
	}
	return pid;
}

/*
 * Handle do_wait work for one task in a live, non-stopped state.
 * read_lock(&tasklist_lock) on entry.  If we return zero, we still hold
 * the lock and this task is uninteresting.  If we return nonzero, we have
 * released the lock and the system call should return.
 * continued 是线程组共享的一次性事件；WNOWAIT 保留标志，普通等待则在
 * siglock 下清除 SIGNAL_STOP_CONTINUED，避免多个 waiter 重复消费。
 */
static int wait_task_continued(struct wait_opts *wo, struct task_struct *p)
{
	struct waitid_info *infop;
	pid_t pid;
	uid_t uid;

	if (!unlikely(wo->wo_flags & WCONTINUED))
		return 0;

	if (!(p->signal->flags & SIGNAL_STOP_CONTINUED))
		return 0;

	spin_lock_irq(&p->sighand->siglock);
	/* Re-check with the lock held.  */
	/* 与并发继续动作及其他 waiter 竞争，必须在信号锁内重查共享标志。 */
	if (!(p->signal->flags & SIGNAL_STOP_CONTINUED)) {
		spin_unlock_irq(&p->sighand->siglock);
		return 0;
	}
	if (!unlikely(wo->wo_flags & WNOWAIT))
		p->signal->flags &= ~SIGNAL_STOP_CONTINUED;
	uid = from_kuid_munged(current_user_ns(), task_uid(p));
	spin_unlock_irq(&p->sighand->siglock);

	pid = task_pid_vnr(p);
	get_task_struct(p);
	read_unlock(&tasklist_lock);
	sched_annotate_sleep();
	if (wo->wo_rusage)
		getrusage(p, RUSAGE_BOTH, wo->wo_rusage);
	put_task_struct(p);

	infop = wo->wo_info;
	if (!infop) {
		wo->wo_stat = 0xffff;
	} else {
		infop->cause = CLD_CONTINUED;
		infop->pid = pid;
		infop->uid = uid;
		infop->status = SIGCONT;
	}
	return pid;
}

/*
 * Consider @p for a wait by @parent.
 *
 * -ECHILD should be in ->notask_error before the first call.
 * Returns nonzero for a final return, when we have unlocked tasklist_lock.
 * Returns zero if the search for a child should continue;
 * then ->notask_error is 0 if @p is an eligible child,
 * or still -ECHILD.
 * 本函数统一考虑 zombie、停止和继续事件，并维护 notask_error：它区分
 * “根本没有符合身份的孩子”和“有孩子但当前尚无可报告事件”。
 */
static int wait_consider_task(struct wait_opts *wo, int ptrace,
				struct task_struct *p)
{
	/*
	 * We can race with wait_task_zombie() from another thread.
	 * Ensure that EXIT_ZOMBIE -> EXIT_DEAD/EXIT_TRACE transition
	 * can't confuse the checks below.
	 * READ_ONCE 固定一次状态快照；并发回收只允许 ZOMBIE 单向转 DEAD/TRACE，
	 * 后续分支据此作保守处理。
	 */
	int exit_state = READ_ONCE(p->exit_state);
	int ret;

	if (unlikely(exit_state == EXIT_DEAD))
		return 0;

	ret = eligible_child(wo, ptrace, p);
	if (!ret)
		return ret;

	if (unlikely(exit_state == EXIT_TRACE)) {
		/*
		 * ptrace == 0 means we are the natural parent. In this case
		 * we should clear notask_error, debugger will notify us.
		 * 自然父进程暂不能消费 TRACE，但未来 ptracer 会级联通知，所以仍有合格
		 * 孩子，不能错误返回 ECHILD。
		 */
		if (likely(!ptrace))
			wo->notask_error = 0;
		return 0;
	}

	if (likely(!ptrace) && unlikely(p->ptrace)) {
		/*
		 * If it is traced by its real parent's group, just pretend
		 * the caller is ptrace_do_wait() and reap this child if it
		 * is zombie.
		 *
		 * This also hides group stop state from real parent; otherwise
		 * a single stop can be reported twice as group and ptrace stop.
		 * If a ptracer wants to distinguish these two events for its
		 * own children it should create a separate process which takes
		 * the role of real parent.
		 * 跟踪者若位于真实父进程线程组内，就把自然父等待按 ptrace 等待处理，
		 * 避免同一次停止既作为 group-stop 又作为 ptrace-stop 报告两次。
		 */
		if (!ptrace_reparented(p))
			ptrace = 1;
	}

	/* slay zombie? */
	/* 是否消费僵尸：组长仍有子线程时必须延迟，避免提前销毁共享 signal。 */
	if (exit_state == EXIT_ZOMBIE) {
		/* we don't reap group leaders with subthreads */
		/* delay_group_leader() 为真时先等待其他线程完成 release_task。 */
		if (!delay_group_leader(p)) {
			/*
			 * A zombie ptracee is only visible to its ptracer.
			 * Notification and reaping will be cascaded to the
			 * real parent when the ptracer detaches.
			 * ptracee 僵尸只对 ptracer 可见；解除跟踪时再向真实父进程级联。
			 */
			if (unlikely(ptrace) || likely(!p->ptrace))
				return wait_task_zombie(wo, p);
		}

		/*
		 * Allow access to stopped/continued state via zombie by
		 * falling through.  Clearing of notask_error is complex.
		 *
		 * When !@ptrace:
		 *
		 * If WEXITED is set, notask_error should naturally be
		 * cleared.  If not, subset of WSTOPPED|WCONTINUED is set,
		 * so, if there are live subthreads, there are events to
		 * wait for.  If all subthreads are dead, it's still safe
		 * to clear - this function will be called again in finite
		 * amount time once all the subthreads are released and
		 * will then return without clearing.
		 *
		 * When @ptrace:
		 *
		 * Stopped state is per-task and thus can't change once the
		 * target task dies.  Only continued and exited can happen.
		 * Clear notask_error if WCONTINUED | WEXITED.
		 * 即使暂不能回收，也要判断未来是否可能出现所请求事件，以决定 do_wait()
		 * 应睡眠等待还是立即返回 ECHILD。
		 */
		if (likely(!ptrace) || (wo->wo_flags & (WCONTINUED | WEXITED)))
			wo->notask_error = 0;
	} else {
		/*
		 * @p is alive and it's gonna stop, continue or exit, so
		 * there always is something to wait for.
		 * 活任务将来总可能停止、继续或退出，因此存在合格孩子时清除 ECHILD。
		 */
		wo->notask_error = 0;
	}

	/*
	 * Wait for stopped.  Depending on @ptrace, different stopped state
	 * is used and the two don't interact with each other.
	 * ptrace-stop 与作业控制 stop 使用不同状态槽，可以独立消费。
	 */
	ret = wait_task_stopped(wo, ptrace, p);
	if (ret)
		return ret;

	/*
	 * Wait for continued.  There's only one continued state and the
	 * ptracer can consume it which can confuse the real parent.  Don't
	 * use WCONTINUED from ptracer.  You don't need or want it.
	 * continue 标志只有一份；禁止 ptracer 消费，避免真实父进程永久错过事件。
	 */
	return wait_task_continued(wo, p);
}

/*
 * Do the work of do_wait() for one thread in the group, @tsk.
 *
 * -ECHILD should be in ->notask_error before the first call.
 * Returns nonzero for a final return, when we have unlocked tasklist_lock.
 * Returns zero if the search for a child should continue; then
 * ->notask_error is 0 if there were any eligible children,
 * or still -ECHILD.
 * 遍历某线程名下的自然 children；找到终结事件时下层已经释放 tasklist_lock。
 */
static int do_wait_thread(struct wait_opts *wo, struct task_struct *tsk)
{
	struct task_struct *p;

	list_for_each_entry(p, &tsk->children, sibling) {
		int ret = wait_consider_task(wo, 0, p);

		if (ret)
			return ret;
	}

	return 0;
}

static int ptrace_do_wait(struct wait_opts *wo, struct task_struct *tsk)
{
	/* ptraced 链表与自然 children 分开扫描，ptrace=1 令事件采用跟踪语义。 */
	struct task_struct *p;

	list_for_each_entry(p, &tsk->ptraced, ptrace_entry) {
		int ret = wait_consider_task(wo, 1, p);

		if (ret)
			return ret;
	}

	return 0;
}

bool pid_child_should_wake(struct wait_opts *wo, struct task_struct *p)
{
	/* 先按 PID 选择器过滤；__WNOTHREAD 还要求事件属于当前线程自己的孩子。 */
	if (!eligible_pid(wo, p))
		return false;

	if ((wo->wo_flags & __WNOTHREAD) && wo->child_wait.private != p->parent)
		return false;

	return true;
}

static int child_wait_callback(wait_queue_entry_t *wait, unsigned mode,
				int sync, void *key)
{
	/* key 是产生子事件的 task；只有可能匹配本次 wait 的事件才唤醒 waiter。 */
	struct wait_opts *wo = container_of(wait, struct wait_opts,
						child_wait);
	struct task_struct *p = key;

	if (pid_child_should_wake(wo, p))
		return default_wake_function(wait, mode, sync, key);

	return 0;
}

void __wake_up_parent(struct task_struct *p, struct task_struct *parent)
{
	/* 以子任务 p 为 key 同步唤醒，过滤回调可减少父线程组内无关惊群。 */
	__wake_up_sync_key(&parent->signal->wait_chldexit,
			   TASK_INTERRUPTIBLE, p);
}

static bool is_effectively_child(struct wait_opts *wo, bool ptrace,
				 struct task_struct *target)
{
	/* 自然等待看 real_parent，ptrace 看 parent；默认允许同线程组代为等待。 */
	struct task_struct *parent =
		!ptrace ? target->real_parent : target->parent;

	return current == parent || (!(wo->wo_flags & __WNOTHREAD) &&
				     same_thread_group(current, parent));
}

/*
 * Optimization for waiting on PIDTYPE_PID. No need to iterate through child
 * and tracee lists to find the target task.
 * P_PID 可从 pid 哈希直达：先按 TGID 检查自然子进程，再按 PID 检查被跟踪的
 * 具体线程；事件消费仍统一交给 wait_consider_task()。
 */
static int do_wait_pid(struct wait_opts *wo)
{
	bool ptrace;
	struct task_struct *target;
	int retval;

	ptrace = false;
	target = pid_task(wo->wo_pid, PIDTYPE_TGID);
	if (target && is_effectively_child(wo, ptrace, target)) {
		retval = wait_consider_task(wo, ptrace, target);
		if (retval)
			return retval;
	}

	ptrace = true;
	target = pid_task(wo->wo_pid, PIDTYPE_PID);
	if (target && target->ptrace &&
	    is_effectively_child(wo, ptrace, target)) {
		retval = wait_consider_task(wo, ptrace, target);
		if (retval)
			return retval;
	}

	return 0;
}

long __do_wait(struct wait_opts *wo)
{
	/*
	 * 执行一次不睡眠扫描。返回 PID/错误；有合格孩子但暂时无事件时返回
	 * -ERESTARTSYS，交由外层 wait 队列进行可中断睡眠。
	 */
	long retval;

	/*
	 * If there is nothing that can match our criteria, just get out.
	 * We will clear ->notask_error to zero if we see any child that
	 * might later match our criteria, even if we are not able to reap
	 * it yet.
	 * notask_error 初始为 ECHILD，任何未来可能匹配的孩子都会将其清零，从而
	 * 区分“等待集合为空”和“事件尚未发生”。
	 */
	wo->notask_error = -ECHILD;
	if ((wo->wo_type < PIDTYPE_MAX) &&
	   (!wo->wo_pid || !pid_has_task(wo->wo_pid, wo->wo_type)))
		goto notask;

	read_lock(&tasklist_lock);

	if (wo->wo_type == PIDTYPE_PID) {
		retval = do_wait_pid(wo);
		if (retval)
			return retval;
	} else {
		struct task_struct *tsk = current;

		do {
			retval = do_wait_thread(wo, tsk);
			if (retval)
				return retval;

			retval = ptrace_do_wait(wo, tsk);
			if (retval)
				return retval;

			if (wo->wo_flags & __WNOTHREAD)
				break;
		} while_each_thread(current, tsk);
	}
	read_unlock(&tasklist_lock);

notask:
	retval = wo->notask_error;
	if (!retval && !(wo->wo_flags & WNOHANG))
		return -ERESTARTSYS;

	return retval;
}

static long do_wait(struct wait_opts *wo)
{
	/*
	 * 先把过滤等待项挂入 wait_chldexit 再扫描，避免检查与睡眠之间丢失事件。
	 * -ERESTARTSYS 且无待处理信号时调度睡眠，被唤醒后重新完整扫描。
	 */
	int retval;

	trace_sched_process_wait(wo->wo_pid);

	init_waitqueue_func_entry(&wo->child_wait, child_wait_callback);
	wo->child_wait.private = current;
	add_wait_queue(&current->signal->wait_chldexit, &wo->child_wait);

	do {
		set_current_state(TASK_INTERRUPTIBLE);
		retval = __do_wait(wo);
		if (retval != -ERESTARTSYS)
			break;
		if (signal_pending(current))
			break;
		schedule();
	} while (1);

	__set_current_state(TASK_RUNNING);
	remove_wait_queue(&current->signal->wait_chldexit, &wo->child_wait);
	return retval;
}

int kernel_waitid_prepare(struct wait_opts *wo, int which, pid_t upid,
			  struct waitid_info *infop, int options,
			  struct rusage *ru)
{
	/*
	 * 校验选项，并把 P_ALL/P_PID/P_PGID/P_PIDFD 统一转换成 pid_type 与 struct
	 * pid 引用。pidfd 的 O_NONBLOCK 映射为内部 WNOHANG，由上层再转换为 EAGAIN。
	 */
	unsigned int f_flags = 0;
	struct pid *pid = NULL;
	enum pid_type type;

	if (options & ~(WNOHANG|WNOWAIT|WEXITED|WSTOPPED|WCONTINUED|
			__WNOTHREAD|__WCLONE|__WALL))
		return -EINVAL;
	if (!(options & (WEXITED|WSTOPPED|WCONTINUED)))
		return -EINVAL;

	switch (which) {
	case P_ALL:
		type = PIDTYPE_MAX;
		break;
	case P_PID:
		type = PIDTYPE_PID;
		if (upid <= 0)
			return -EINVAL;

		pid = find_get_pid(upid);
		break;
	case P_PGID:
		type = PIDTYPE_PGID;
		if (upid < 0)
			return -EINVAL;

		if (upid)
			pid = find_get_pid(upid);
		else
			pid = get_task_pid(current, PIDTYPE_PGID);
		break;
	case P_PIDFD:
		type = PIDTYPE_PID;
		if (upid < 0)
			return -EINVAL;

		pid = pidfd_get_pid(upid, &f_flags);
		if (IS_ERR(pid))
			return PTR_ERR(pid);

		break;
	default:
		return -EINVAL;
	}

	wo->wo_type	= type;
	wo->wo_pid	= pid;
	wo->wo_flags	= options;
	wo->wo_info	= infop;
	wo->wo_rusage	= ru;
	if (f_flags & O_NONBLOCK)
		wo->wo_flags |= WNOHANG;

	return 0;
}

static long kernel_waitid(int which, pid_t upid, struct waitid_info *infop,
			  int options, struct rusage *ru)
{
	/* 内核公共封装负责参数准备、执行等待和释放 struct pid 引用。 */
	struct wait_opts wo;
	long ret;

	ret = kernel_waitid_prepare(&wo, which, upid, infop, options, ru);
	if (ret)
		return ret;

	ret = do_wait(&wo);
	if (!ret && !(options & WNOHANG) && (wo.wo_flags & WNOHANG))
		ret = -EAGAIN;

	put_pid(wo.wo_pid);
	return ret;
}

SYSCALL_DEFINE5(waitid, int, which, pid_t, upid, struct siginfo __user *,
		infop, int, options, struct rusage __user *, ru)
{
	/*
	 * 先在内核 waitid_info 中形成结果，有事件时再转为 SIGCHLD siginfo；
	 * user_write_access_begin 与 unsafe_put_user 组成受保护的用户写区间。
	 */
	struct rusage r;
	struct waitid_info info = {.status = 0};
	long err = kernel_waitid(which, upid, &info, options, ru ? &r : NULL);
	int signo = 0;

	if (err > 0) {
		signo = SIGCHLD;
		err = 0;
		if (ru && copy_to_user(ru, &r, sizeof(struct rusage)))
			return -EFAULT;
	}
	if (!infop)
		return err;

	if (!user_write_access_begin(infop, sizeof(*infop)))
		return -EFAULT;

	unsafe_put_user(signo, &infop->si_signo, Efault);
	unsafe_put_user(0, &infop->si_errno, Efault);
	unsafe_put_user(info.cause, &infop->si_code, Efault);
	unsafe_put_user(info.pid, &infop->si_pid, Efault);
	unsafe_put_user(info.uid, &infop->si_uid, Efault);
	unsafe_put_user(info.status, &infop->si_status, Efault);
	user_write_access_end();
	return err;
Efault:
	user_write_access_end();
	return -EFAULT;
}

long kernel_wait4(pid_t upid, int __user *stat_addr, int options,
		  struct rusage *ru)
{
	/*
	 * wait4 的 pid 编码：-1 任意子进程，负值指定进程组，0 当前进程组，正值
	 * 指定 PID。它总隐含 WEXITED，并可选返回资源用量。
	 */
	struct wait_opts wo;
	struct pid *pid = NULL;
	enum pid_type type;
	long ret;

	if (options & ~(WNOHANG|WUNTRACED|WCONTINUED|
			__WNOTHREAD|__WCLONE|__WALL))
		return -EINVAL;

	/* -INT_MIN is not defined */
	/* 对 INT_MIN 取负在 C 中未定义，解析负进程组号前必须单独拒绝。 */
	if (upid == INT_MIN)
		return -ESRCH;

	if (upid == -1)
		type = PIDTYPE_MAX;
	else if (upid < 0) {
		type = PIDTYPE_PGID;
		pid = find_get_pid(-upid);
	} else if (upid == 0) {
		type = PIDTYPE_PGID;
		pid = get_task_pid(current, PIDTYPE_PGID);
	} else /* upid > 0 */ {
		/* 正数精确选择一个 PID。 */
		type = PIDTYPE_PID;
		pid = find_get_pid(upid);
	}

	wo.wo_type	= type;
	wo.wo_pid	= pid;
	wo.wo_flags	= options | WEXITED;
	wo.wo_info	= NULL;
	wo.wo_stat	= 0;
	wo.wo_rusage	= ru;
	ret = do_wait(&wo);
	put_pid(pid);
	if (ret > 0 && stat_addr && put_user(wo.wo_stat, stat_addr))
		ret = -EFAULT;

	return ret;
}

int kernel_wait(pid_t pid, int *stat)
{
	/* 内核调用者使用的精简 P_PID + WEXITED 接口。 */
	struct wait_opts wo = {
		.wo_type	= PIDTYPE_PID,
		.wo_pid		= find_get_pid(pid),
		.wo_flags	= WEXITED,
	};
	int ret;

	ret = do_wait(&wo);
	if (ret > 0 && wo.wo_stat)
		*stat = wo.wo_stat;
	put_pid(wo.wo_pid);
	return ret;
}

SYSCALL_DEFINE4(wait4, pid_t, upid, int __user *, stat_addr,
		int, options, struct rusage __user *, ru)
{
	/* rusage 先写入内核临时对象，确认取得事件后再复制给用户。 */
	struct rusage r;
	long err = kernel_wait4(upid, stat_addr, options, ru ? &r : NULL);

	if (err > 0) {
		if (ru && copy_to_user(ru, &r, sizeof(struct rusage)))
			return -EFAULT;
	}
	return err;
}

#ifdef __ARCH_WANT_SYS_WAITPID

/*
 * sys_waitpid() remains for compatibility. waitpid() should be
 * implemented by calling sys_wait4() from libc.a.
 * 内核保留旧系统调用号用于 ABI 兼容；现代 libc 通常以 wait4 实现 waitpid。
 */
SYSCALL_DEFINE3(waitpid, pid_t, pid, int __user *, stat_addr, int, options)
{
	return kernel_wait4(pid, stat_addr, options, NULL);
}

#endif

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE4(wait4,
	compat_pid_t, pid,
	compat_uint_t __user *, stat_addr,
	int, options,
	struct compat_rusage __user *, ru)
{
	/* compat 层复用本机等待逻辑，仅转换 32 位用户 ABI 的 rusage/stat。 */
	struct rusage r;
	long err = kernel_wait4(pid, stat_addr, options, ru ? &r : NULL);
	if (err > 0) {
		if (ru && put_compat_rusage(&r, ru))
			return -EFAULT;
	}
	return err;
}

COMPAT_SYSCALL_DEFINE5(waitid,
		int, which, compat_pid_t, pid,
		struct compat_siginfo __user *, infop, int, options,
		struct compat_rusage __user *, uru)
{
	/* 事件选择与本机 waitid 相同，差异仅在 compat_siginfo/rusage 布局。 */
	struct rusage ru;
	struct waitid_info info = {.status = 0};
	long err = kernel_waitid(which, pid, &info, options, uru ? &ru : NULL);
	int signo = 0;
	if (err > 0) {
		signo = SIGCHLD;
		err = 0;
		if (uru) {
			/* kernel_waitid() overwrites everything in ru */
			/* ru 已由内核辅助函数完整初始化，可按目标时间 ABI 整体转换。 */
			if (COMPAT_USE_64BIT_TIME)
				err = copy_to_user(uru, &ru, sizeof(ru));
			else
				err = put_compat_rusage(&ru, uru);
			if (err)
				return -EFAULT;
		}
	}

	if (!infop)
		return err;

	if (!user_write_access_begin(infop, sizeof(*infop)))
		return -EFAULT;

	unsafe_put_user(signo, &infop->si_signo, Efault);
	unsafe_put_user(0, &infop->si_errno, Efault);
	unsafe_put_user(info.cause, &infop->si_code, Efault);
	unsafe_put_user(info.pid, &infop->si_pid, Efault);
	unsafe_put_user(info.uid, &infop->si_uid, Efault);
	unsafe_put_user(info.status, &infop->si_status, Efault);
	user_write_access_end();
	return err;
Efault:
	user_write_access_end();
	return -EFAULT;
}
#endif

/*
 * This needs to be __function_aligned as GCC implicitly makes any
 * implementation of abort() cold and drops alignment specified by
 * -falign-functions=N.
 *
 * See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=88345#c11
 * GCC 将 abort() 隐式视为 cold，可能丢弃普通函数对齐选项，故显式要求函数
 * 对齐；弱符号允许架构或运行时提供替代实现。
 */
__weak __function_aligned void abort(void)
{
	BUG();

	/* if that doesn't kill us, halt */
	/* BUG() 理论上不返回；若异常路径继续，panic 保证不带损坏状态运行。 */
	panic("Oops failed to kill thread");
}
EXPORT_SYMBOL(abort);
