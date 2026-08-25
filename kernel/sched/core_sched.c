// SPDX-License-Identifier: GPL-2.0-only

/*
 * A simple wrapper around refcount. An allocated sched_core_cookie's
 * address is used to compute the cookie of the task.
 */
#include "sched.h"

/*
 * cookie 对象本身只保存共享引用数；任务字段保存其地址作为不透明身份值。地址只用于
 * 相等比较或哈希后向用户态展示，用户态得到的 id 不能反解、持有或直接设置 cookie。
 */
struct sched_core_cookie {
	refcount_t refcnt;
};

/*
 * 分配一个唯一 cookie，初始引用归创建流程所有；成功后同时取得全局 core-scheduling
 * 用户引用，确保首个带 cookie 的任务写入前功能已经启用。分配失败返回 0 且无副作用。
 */
static unsigned long sched_core_alloc_cookie(void)
{
	struct sched_core_cookie *ck = kmalloc_obj(*ck);
	if (!ck)
		return 0;

	refcount_set(&ck->refcnt, 1);
	sched_core_get();

	return (unsigned long)ck;
}

/*
 * 释放调用方持有的一份 cookie 引用；0 表示“无隔离 cookie”并被忽略。最后一份引用
 * 释放对象并配对 sched_core_put()，后者负责安全地关闭全局 core scheduling。
 */
static void sched_core_put_cookie(unsigned long cookie)
{
	struct sched_core_cookie *ptr = (void *)cookie;

	if (ptr && refcount_dec_and_test(&ptr->refcnt)) {
		kfree(ptr);
		sched_core_put();
	}
}

/*
 * 为非零 cookie 增加共享引用并原样返回，便于赋值链组合；调用者必须已经通过任务锁、
 * task 引用或自有引用保证对象尚未到达最后一次 put。
 */
static unsigned long sched_core_get_cookie(unsigned long cookie)
{
	struct sched_core_cookie *ptr = (void *)cookie;

	if (ptr)
		refcount_inc(&ptr->refcnt);

	return cookie;
}

/*
 * sched_core_update_cookie - replace the cookie on a task
 * @p: the task to update
 * @cookie: the new cookie
 *
 * Effectively exchange the task cookie; caller is responsible for lifetimes on
 * both ends.
 *
 * Returns: the old cookie
 */
/*
 * 中文释义：在 task_rq_lock 同时稳定任务归属 rq 和 core 调度树后，用新 cookie 替换
 * 旧值；已入 core 红黑树的任务先摘除，仍在 rq 上且新 cookie 非零时再插入。正在 CPU
 * 上运行的任务被请求重调度，以免继续和新 cookie 不兼容的 SMT 兄弟并行。函数只交换
 * 值，不增减两端引用，返回旧 cookie 交由调用者释放。
 */
static unsigned long sched_core_update_cookie(struct task_struct *p,
					      unsigned long cookie)
{
	unsigned long old_cookie;
	struct rq_flags rf;
	struct rq *rq;

	rq = task_rq_lock(p, &rf);

	/*
	 * Since creating a cookie implies sched_core_get(), and we cannot set
	 * a cookie until after we've created it, similarly, we cannot destroy
	 * a cookie until after we've removed it, we must have core scheduling
	 * enabled here.
	 */
	/* 中文释义：cookie 的全局引用先于写入取得、后于移除释放，因此此临界区应始终启用。 */
	WARN_ON_ONCE((p->core_cookie || cookie) && !sched_core_enabled(rq));

	if (sched_core_enqueued(p))
		sched_core_dequeue(rq, p, DEQUEUE_SAVE);

	old_cookie = p->core_cookie;
	p->core_cookie = cookie;

	/*
	 * Consider the cases: !prev_cookie and !cookie.
	 */
	/* 中文释义：新值为 0 时任务不参与按 cookie 排序，无需重新插入 core 树。 */
	if (cookie && task_on_rq_queued(p))
		sched_core_enqueue(rq, p);

	/*
	 * If task is currently running, it may not be compatible anymore after
	 * the cookie change, so enter the scheduler on its CPU to schedule it
	 * away.
	 *
	 * Note that it is possible that as a result of this cookie change, the
	 * core has now entered/left forced idle state. Defer accounting to the
	 * next scheduling edge, rather than always forcing a reschedule here.
	 */
	/*
	 * 中文释义：仅运行中的目标必须立刻触发调度；forced-idle 统计允许等到下一调度边界
	 * 结算，避免每次非运行任务的 cookie 改动都制造额外 IPI。
	 */
	if (task_on_cpu(rq, p))
		resched_curr(rq);

	task_rq_unlock(rq, p, &rf);

	return old_cookie;
}

/*
 * 在 p->pi_lock 下取得任务当前 cookie 的一份独立引用；锁保证读取与 cookie 替换互斥，
 * 返回后即使任务改变 cookie，调用方持有的旧对象仍存活。0 仍表示未设置。
 */
static unsigned long sched_core_clone_cookie(struct task_struct *p)
{
	unsigned long cookie, flags;

	raw_spin_lock_irqsave(&p->pi_lock, flags);
	cookie = sched_core_get_cookie(p->core_cookie);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

	return cookie;
}

/*
 * fork 初始化子任务尚未入树的 core_node，并复制 current 的 cookie 引用；后续失败清理
 * 或任务退出必须调用 sched_core_free() 配对，不在此处发布到运行队列。
 */
void sched_core_fork(struct task_struct *p)
{
	RB_CLEAR_NODE(&p->core_node);
	p->core_cookie = sched_core_clone_cookie(current);
}

/* 任务生命周期结束时释放其 cookie 所有权；调用者已确保任务不会再次入 core 调度树。 */
void sched_core_free(struct task_struct *p)
{
	sched_core_put_cookie(p->core_cookie);
}

/*
 * 为目标任务安装 cookie 的所有权转换封装：先为新值取引用，再原子交换，最后释放旧值。
 * 输入 cookie 只是借用引用，因此同一个 cookie 可安全批量设置给整个线程/进程组。
 */
static void __sched_core_set(struct task_struct *p, unsigned long cookie)
{
	cookie = sched_core_get_cookie(cookie);
	cookie = sched_core_update_cookie(p, cookie);
	sched_core_put_cookie(cookie);
}

/* Called from prctl interface: PR_SCHED_CORE */
/*
 * 处理 PR_SCHED_CORE 的查询、创建、推送和拉取。pid==0 选择 current；scope 映射到
 * PID/TGID/PGID。SMT 未启用返回 -ENODEV，参数错误返回 -EINVAL，目标消失返回
 * -ESRCH，ptrace real-credentials 检查失败返回 -EPERM，分配失败返回 -ENOMEM，用户
 * 地址写失败透传 put_user 错误。所有出口统一释放临时 cookie 和 task 引用。
 *
 * 组操作先在 tasklist_lock 下逐成员完成权限预检，全部通过后才逐个设置，避免权限
 * 失败造成半组更新；但设置阶段本身不是用户可见的全组原子事务，成员状态会依次改变。
 */
int sched_core_share_pid(unsigned int cmd, pid_t pid, enum pid_type type,
			 unsigned long uaddr)
{
	unsigned long cookie = 0, id = 0;
	struct task_struct *task, *p;
	struct pid *grp;
	int err = 0;

	if (!sched_smt_active())
		return -ENODEV;

	/* 编译期确认 UAPI scope 数值可直接、安全地解释为内核 pid_type。 */
	BUILD_BUG_ON(PR_SCHED_CORE_SCOPE_THREAD != PIDTYPE_PID);
	BUILD_BUG_ON(PR_SCHED_CORE_SCOPE_THREAD_GROUP != PIDTYPE_TGID);
	BUILD_BUG_ON(PR_SCHED_CORE_SCOPE_PROCESS_GROUP != PIDTYPE_PGID);

	if (type > PIDTYPE_PGID || cmd >= PR_SCHED_CORE_MAX || pid < 0 ||
	    (cmd != PR_SCHED_CORE_GET && uaddr))
		return -EINVAL;

	rcu_read_lock();
	/* RCU 只保护 PID 查找到 get_task_struct() 之间；取得引用后可退出 RCU 临界区。 */
	if (pid == 0) {
		task = current;
	} else {
		task = find_task_by_vpid(pid);
		if (!task) {
			rcu_read_unlock();
			return -ESRCH;
		}
	}
	/*
	 * task 此时仍只是 RCU 保护的借用指针；在退出读侧前取得 task_struct 引用，
	 * 后续权限检查和 cookie 更新才能跨越目标并发退出，最终由公共 out 配对 put。
	 */
	get_task_struct(task);
	rcu_read_unlock();

	/*
	 * Check if this process has the right to modify the specified
	 * process. Use the regular "ptrace_may_access()" checks.
	 */
	/* 中文释义：沿用 ptrace 的真实凭据读取权限，防止越权观察或改变其他任务的隔离域。 */
	if (!ptrace_may_access(task, PTRACE_MODE_READ_REALCREDS)) {
		err = -EPERM;
		goto out;
	}

	switch (cmd) {
	case PR_SCHED_CORE_GET:
		/* GET 仅支持单线程，且 u64 用户地址必须 8 字节对齐。 */
		if (type != PIDTYPE_PID || uaddr & 7) {
			err = -EINVAL;
			goto out;
		}
		cookie = sched_core_clone_cookie(task);
		if (cookie) {
			/* XXX improve ? */
			/* 原文保留：当前只把内核指针哈希成进程可比较的 id，接口可用性仍有改进空间。 */
			ptr_to_hashval((void *)cookie, &id);
		}
		err = put_user(id, (u64 __user *)uaddr);
		goto out;

	case PR_SCHED_CORE_CREATE:
		/* 新对象的临时引用随后被目标任务各自 get，统一 out 再释放创建者引用。 */
		cookie = sched_core_alloc_cookie();
		if (!cookie) {
			err = -ENOMEM;
			goto out;
		}
		break;

	case PR_SCHED_CORE_SHARE_TO:
		/* 把 current 的 cookie 推给目标或目标组；0 也会清除目标现有 cookie。 */
		cookie = sched_core_clone_cookie(current);
		break;

	case PR_SCHED_CORE_SHARE_FROM:
		/* 拉取只允许单线程源，接收者固定为 current，避免“从组读取”语义不确定。 */
		if (type != PIDTYPE_PID) {
			err = -EINVAL;
			goto out;
		}
		cookie = sched_core_clone_cookie(task);
		__sched_core_set(current, cookie);
		goto out;

	/* SHARE_FROM 成功已经更新 current 并退出；落到 default 的只能是未定义命令值。 */
	default:
		err = -EINVAL;
		goto out;
	}

	/*
	 * 能走到这里的只剩 CREATE 或 SHARE_TO，cookie 都是一份由本函数持有的临时引用；
	 * 单任务直接安装，组作用域则先做全员权限预检，再把同一对象逐个共享出去。
	 */
	if (type == PIDTYPE_PID) {
		/* 单任务路径在此完成；临时 cookie 引用仍由公共 out 释放。 */
		__sched_core_set(task, cookie);
		goto out;
	}

	read_lock(&tasklist_lock);
	grp = task_pid_type(task, type);

	/* 权限预检覆盖操作时仍属于该 pid 组的每个成员。 */
	do_each_pid_thread(grp, type, p) {
		if (!ptrace_may_access(p, PTRACE_MODE_READ_REALCREDS)) {
			err = -EPERM;
			goto out_tasklist;
		}
	} while_each_pid_thread(grp, type, p);

	do_each_pid_thread(grp, type, p) {
		/* 第二遍才逐任务持锁换 cookie，确保已知权限错误不会留下部分更新。 */
		__sched_core_set(p, cookie);
	} while_each_pid_thread(grp, type, p);
out_tasklist:
	read_unlock(&tasklist_lock);

out:
	sched_core_put_cookie(cookie);
	put_task_struct(task);
	return err;
}

#ifdef CONFIG_SCHEDSTATS

/* REQUIRES: rq->core's clock recently updated. */
/*
 * 原文契约：rq->core 的时钟必须刚更新。函数把本段 forced-idle 时间按空闲 SMT 线程数
 * 与实际运行 cookied 任务数缩放，再计入每个非 idle core_pick/curr。调用者持 rq 锁；
 * 未开始计时或时钟未前进时快速返回，不分配对象也不改变任务选择。
 */
void __sched_core_account_forceidle(struct rq *rq)
{
	const struct cpumask *smt_mask = cpu_smt_mask(cpu_of(rq));
	u64 delta, now = rq_clock(rq->core);
	struct rq *rq_i;
	struct task_struct *p;
	int i;

	/*
	 * smt_mask 限定同一物理 core 的兄弟；delta 是本轮待分摊纳秒数。
	 * rq_i/p/i 只在后续遍历中借用每个兄弟 rq 及其实际选中任务，不取得任务引用。
	 */
	lockdep_assert_rq_held(rq);

	WARN_ON_ONCE(!rq->core->core_forceidle_count);

	if (rq->core->core_forceidle_start == 0)
		return;

	delta = now - rq->core->core_forceidle_start;
	if (unlikely((s64)delta <= 0))
		return;

	rq->core->core_forceidle_start = now;

	if (WARN_ON_ONCE(!rq->core->core_forceidle_occupation)) {
		/* can't be forced idle without a running task */
		/* 原文释义：没有任何运行任务就不可能由 cookie 不兼容造成 forced idle。 */
	} else if (rq->core->core_forceidle_count > 1 ||
		   rq->core->core_forceidle_occupation > 1) {
		/*
		 * For larger SMT configurations, we need to scale the charged
		 * forced idle amount since there can be more than one forced
		 * idle sibling and more than one running cookied task.
		 */
		/* 中文释义：多路 SMT 中按 forced-idle 数/占用数分摊，避免把同一时间段重复全额计费。 */
		delta *= rq->core->core_forceidle_count;
		delta = div_u64(delta, rq->core->core_forceidle_occupation);
	}

	for_each_cpu(i, smt_mask) {
		rq_i = cpu_rq(i);
		p = rq_i->core_pick ?: rq_i->curr;

		if (p == rq_i->idle)
			continue;

		/*
		 * Note: this will account forceidle to the current CPU, even
		 * if it comes from our SMT sibling.
		 */
		/* 中文释义：统计归到当前遍历 CPU 选中的非 idle 任务，即使强制空闲发生在其兄弟线程。 */
		__account_forceidle_time(p, delta);
	}
}

/*
 * 调度 tick 上仅在 core 正处于 forced-idle 时结算；非 core 主 rq 先更新共享 core rq
 * 时钟，再调用记账函数。CONFIG_SCHEDSTATS 关闭时这两个统计入口整体不存在。
 */
void __sched_core_tick(struct rq *rq)
{
	if (!rq->core->core_forceidle_count)
		return;

	if (rq != rq->core)
		update_rq_clock(rq->core);

	__sched_core_account_forceidle(rq);
}

#endif /* CONFIG_SCHEDSTATS */
