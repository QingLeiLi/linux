// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic pidhash and scalable, time-bounded PID allocator
 *
 * (C) 2002-2003 Nadia Yvette Chambers, IBM
 * (C) 2004 Nadia Yvette Chambers, Oracle
 * (C) 2002-2004 Ingo Molnar, Red Hat
 *
 * pid-structures are backing objects for tasks sharing a given ID to chain
 * against. There is very little to them aside from hashing them and
 * parking tasks using given ID's on a list.
 *
 * The hash is always changed with the tasklist_lock write-acquired,
 * and the hash is only accessed with the tasklist_lock at least
 * read-acquired, so there's no additional SMP locking needed here.
 *
 * We have a list of bitmap pages, which bitmaps represent the PID space.
 * Allocating and freeing PIDs is completely lockless. The worst-case
 * allocation scenario when all but one out of 1 million PIDs possible are
 * allocated already: the scanning of 32 list entries and at most PAGE_SIZE
 * bytes. The typical fastpath is a single successful setbit. Freeing is O(1).
 *
 * Pid namespaces:
 *    (C) 2007 Pavel Emelyanov <xemul@openvz.org>, OpenVZ, SWsoft Inc.
 *    (C) 2007 Sukadev Bhattiprolu <sukadev@us.ibm.com>, IBM
 *     Many thanks to Oleg Nesterov for comments and help
 *
 * 中文学习注释：
 * 本文件管理的是内核内部的 struct pid，而不只是 pid_t 数字。
 * pid_t 会复用；进程退出后，同一个数字可能很快分配给新进程。
 * 因此内核长期保存进程身份时，更适合保存带引用计数的 struct pid。
 *
 * 背景：
 * - 一个 task 在嵌套 PID namespace 中会有多层可见数字。
 * - struct pid->numbers[] 按 namespace 层级保存这些数字。
 * - struct pid->tasks[] 再按 PIDTYPE_PID/TGID/PGID/SID 反向挂 task。
 *
 * 优点：
 * - 比长期 pin task_struct 更省内存。
 * - 可以抵抗 PID 数字复用造成的误引用。
 * - 配合 RCU，读侧查找可以很轻。
 *
 * 代价和注意事项：
 * - 裸 pid_t 不是稳定句柄，跨时间保存有复用风险。
 * - pidmap_lock 管 idr 分配/删除，不管 task 链表。
 * - tasklist_lock 写锁管 task 和 pid 的绑定关系。
 * - RCU 读侧拿到的指针，离开临界区前要转成引用。
 */

#include <linux/mm.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/rculist.h>
#include <linux/memblock.h>
#include <linux/pid_namespace.h>
#include <linux/init_task.h>
#include <linux/syscalls.h>
#include <linux/proc_ns.h>
#include <linux/refcount.h>
#include <linux/anon_inodes.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/idr.h>
#include <linux/pidfs.h>
#include <net/sock.h>
#include <uapi/linux/pidfd.h>

struct pid init_struct_pid = {
	.count		= REFCOUNT_INIT(1),
	.tasks		= {
		{ .first = NULL },
		{ .first = NULL },
		{ .first = NULL },
	},
	.level		= 0,
	.numbers	= { {
		.nr		= 0,
		.ns		= &init_pid_ns,
	}, }
};

/*
 * pid_max_min/pid_max_max 是 pid_max sysctl 的边界。
 *
 * 背景：pid_max 太小会加快 PID 数字复用；太大则扩大分配器管理空间。
 * RESERVED_PIDS 以内的低号段保留给特殊/早期进程，普通回绕分配会避开。
 */
static int pid_max_min = RESERVED_PIDS + 1;
static int pid_max_max = PID_MAX_LIMIT;

/*
 * PID-map pages start out as NULL, they get allocated upon
 * first use and are never deallocated. This way a low pid_max
 * value does not cause lots of bitmaps to be allocated, but
 * the scheme scales to up to 4 million PIDs, runtime.
 *
 * 中文翻译：PID-map 页初始为 NULL，第一次使用时才分配，之后不释放。
 * 这样较小 pid_max 不会提前分配大量位图，又能在运行期扩展。
 *
 * 学习补充：当前分配核心是 namespace 内的 idr。
 * 这里要理解的思想仍然是“按需扩展、避免低 pid_max 浪费内存”。
 */
struct pid_namespace init_pid_ns = {
	.ns = NS_COMMON_INIT(init_pid_ns),
	.idr = IDR_INIT(init_pid_ns.idr),
	.pid_allocated = PIDNS_ADDING,
	.level = 0,
	.child_reaper = &init_task,
	.user_ns = &init_user_ns,
	.pid_max = PID_MAX_DEFAULT,
#if defined(CONFIG_SYSCTL) && defined(CONFIG_MEMFD_CREATE)
	.memfd_noexec_scope = MEMFD_NOEXEC_SCOPE_EXEC,
#endif
};
EXPORT_SYMBOL_GPL(init_pid_ns);

/*
 * pidmap_lock 保护每个 pid_namespace 的 idr 和 pid_allocated。
 *
 * 优点：PID 数字分配/释放被集中串行化，回滚逻辑更容易保证一致。
 * 代价：fork/exit 热路径会竞争这个锁，所以 alloc_pid() 尽量把可睡眠
 * 的内存准备放在拿锁前。
 *
 * 注意：这个锁不保护 pid->tasks[]。task 绑定关系由 tasklist_lock 和
 * RCU 维护。
 */
static  __cacheline_aligned_in_smp DEFINE_SPINLOCK(pidmap_lock);

/*
 * put_pid() - 释放调用者持有的一份 struct pid 引用。
 * @pid: 入参，可为 NULL；非 NULL 时消耗一份引用。
 *
 * 返回值：无。
 *
 * 生命周期：引用归零后释放 pidfs 状态、pid slab 对象和 pid namespace
 * 引用。它不负责从 idr 删除数字，也不负责从 task 链表摘除 task。
 */
void put_pid(struct pid *pid)
{
	struct pid_namespace *ns;

	if (!pid)
		return;

	ns = pid->numbers[pid->level].ns;
	if (refcount_dec_and_test(&pid->count)) {
		pidfs_free_pid(pid);
		kmem_cache_free(ns->pid_cachep, pid);
		put_pid_ns(ns);
	}
}
EXPORT_SYMBOL_GPL(put_pid);

/*
 * delayed_put_pid() - RCU 宽限期后真正 put_pid()。
 * @rhp: 入参，嵌入 struct pid 的 rcu_head。
 *
 * 背景：find_pid_ns()/pid_task() 的读者可能只持有 RCU 读锁。
 * free_pid() 从 idr 移除对象后，仍要等 RCU 读者离开才能释放内存。
 */
static void delayed_put_pid(struct rcu_head *rhp)
{
	struct pid *pid = container_of(rhp, struct pid, rcu);
	put_pid(pid);
}

/*
 * free_pid() - 释放 PID 在所有 namespace 中占用的数字。
 * @pid: 入参，待释放的 struct pid。
 *
 * 返回值：无。
 *
 * 背景：一个 struct pid 可能同时在多层 PID namespace 中有数字。
 * 释放时必须逐层 idr_remove()，否则外层或内层仍能按旧数字查到它。
 *
 * 锁和并发：
 * - 不能持有 tasklist_lock 调用。
 * - pidmap_lock 保护 idr_remove() 和 pid_allocated。
 * - struct pid 的最终释放经 call_rcu() 延迟。
 *
 * 注意：pid 数字释放不等于 task_struct 释放。
 * task 退出、pid 链表摘除、pid 对象回收是不同层面的生命周期。
 */
void free_pid(struct pid *pid)
{
	int i;
	struct pid_namespace *active_ns;

	lockdep_assert_not_held(&tasklist_lock);

	active_ns = pid->numbers[pid->level].ns;
	ns_ref_active_put(active_ns);

	spin_lock(&pidmap_lock);
	for (i = 0; i <= pid->level; i++) {
		struct upid *upid = pid->numbers + i;
		struct pid_namespace *ns = upid->ns;
		switch (--ns->pid_allocated) {
		case 2:
		case 1:
			/* When all that is left in the pid namespace
			 * is the reaper wake up the reaper.  The reaper
			 * may be sleeping in zap_pid_ns_processes().
			 *
			 * 中文翻译：namespace 中只剩 reaper 时唤醒它。
			 *
			 * 背景：child_reaper 负责收尾孤儿进程和 namespace 退出。
			 * 最后几个 PID 释放时，reaper 可能正在等待其它任务
			 * 退出。
			 */
			wake_up_process(READ_ONCE(ns->child_reaper));
			break;
		case PIDNS_ADDING:
			/* Only possible if the 1st fork fails */
			/*
			 * 中文翻译：只有第一个 fork 失败时才可能发生。
			 *
			 * 注意：PIDNS_ADDING 表示 namespace 尚未完成 init 创建。
			 * 此时 child_reaper 不应已经发布。
			 */
			WARN_ON(READ_ONCE(ns->child_reaper));
			break;
		}

		/* 数字先从 idr 消失；对象内存稍后经 RCU 和引用计数释放。 */
		idr_remove(&ns->idr, upid->nr);
	}
	spin_unlock(&pidmap_lock);

	/* pidfs 视图跟随 PID 生命周期，不能在 idr 删除后继续暴露旧对象。 */
	pidfs_remove_pid(pid);
	call_rcu(&pid->rcu, delayed_put_pid);
}

/*
 * free_pids() - 批量释放一组按 PIDTYPE 收集的 PID。
 * @pids: 入参，数组元素为待释放 struct pid，可含 NULL。
 *
 * 返回值：无。
 *
 * 背景：fork 错误路径可能同时拿到 PID/TGID/PGID/SID。
 * 用数组集中释放，能让调用者把“解除绑定”和“释放 PID”分开处理。
 */
void free_pids(struct pid **pids)
{
	int tmp;

	/*
	 * This can batch pidmap_lock.
	 *
	 * 中文翻译：这里可以批量化 pidmap_lock。
	 *
	 * 现状：当前仍逐个 free_pid()。接口保留批量化空间。
	 */
	for (tmp = PIDTYPE_MAX; --tmp >= 0; )
		if (pids[tmp])
			free_pid(pids[tmp]);
}

/*
 * alloc_pid() - 为新任务分配 struct pid 和各层 namespace 数字。
 * @ns: 入参，新任务所在的最内层 PID namespace。
 * @arg_set_tid: 入参，可选的指定 PID 数组，从内层 namespace 向外排列。
 * @arg_set_tid_size: 入参，@arg_set_tid 的有效元素个数。
 *
 * 返回值：成功返回带 1 个引用的 struct pid；失败返回 ERR_PTR(-errno)。
 *
 * 背景：容器内 PID 和宿主 PID 可以不同。
 * 所以这里要从 @ns 一直向父 namespace 分配 upid。
 *
 * 优点：一个 struct pid 同时承载多层可见数字。
 * 这样查找、pidfd、proc 遍历都能以同一个稳定对象为中心。
 *
 * 注意事项：
 * - 指定 TID 是 checkpoint/restore 能力，必须做权限检查。
 * - idr 中先占 NULL，最后再替换成 pid，避免半初始化对象被查到。
 * - 任一层分配失败，都必须回滚已占用的外/内层数字。
 */
struct pid *alloc_pid(struct pid_namespace *ns, pid_t *arg_set_tid,
		      size_t arg_set_tid_size)
{
	int set_tid[MAX_PID_NS_LEVEL + 1] = {};
	int pid_max[MAX_PID_NS_LEVEL + 1] = {};
	struct pid *pid;
	enum pid_type type;
	int i, nr;
	struct pid_namespace *tmp;
	struct upid *upid;
	int retval = -ENOMEM;
	bool retried_preload;

	/*
	 * arg_set_tid_size contains the size of the arg_set_tid array. Starting at
	 * the most nested currently active PID namespace it tells alloc_pid()
	 * which PID to set for a process in that most nested PID namespace
	 * up to arg_set_tid_size PID namespaces. It does not have to set the PID
	 * for a process in all nested PID namespaces but arg_set_tid_size must
	 * never be greater than the current ns->level + 1.
	 *
	 * 中文补充：数组按“内层到外层”的顺序解释。
	 * 用户态不能指定超过当前 namespace 层级数量的 PID。
	 */
	if (arg_set_tid_size > ns->level + 1)
		return ERR_PTR(-EINVAL);

	/*
	 * Prep before we take locks:
	 *
	 * 1. allocate and fill in pid struct
	 *
	 * 中文补充：GFP_KERNEL 分配可能睡眠，所以必须在 pidmap_lock 外完成。
	 * 这能缩短全局 PID 分配锁的持有时间。
	 */
	pid = kmem_cache_alloc(ns->pid_cachep, GFP_KERNEL);
	if (!pid)
		return ERR_PTR(retval);

	/* pid 持有 namespace 引用，保证 pid_cachep 等状态不会先释放。 */
	get_pid_ns(ns);
	pid->level = ns->level;
	refcount_set(&pid->count, 1);
	spin_lock_init(&pid->lock);
	for (type = 0; type < PIDTYPE_MAX; ++type)
		INIT_HLIST_HEAD(&pid->tasks[type]);
	/* pidfd 等待队列必须在 pid 对外可见前初始化。 */
	init_waitqueue_head(&pid->wait_pidfd);
	INIT_HLIST_HEAD(&pid->inodes);
	pidfs_prepare_pid(pid);

	/*
	 * 2. perm check checkpoint_restore_ns_capable()
	 *
	 * This stores found pid_max to make sure the used value is the same should
	 * later code need it.
	 *
	 * 中文补充：这里快照每层 pid_max。
	 * pid_max 是 sysctl，可运行期变化；分配过程要使用同一组边界。
	 */
	for (tmp = ns, i = ns->level; i >= 0; i--) {
		pid_max[ns->level - i] = READ_ONCE(tmp->pid_max);

		if (arg_set_tid_size) {
			int tid = set_tid[ns->level - i] = arg_set_tid[ns->level - i];

			/*
			 * 指定 PID 必须是普通正 PID，
			 * 且小于该 namespace 的 pid_max。
			 */
			retval = -EINVAL;
			if (tid < 1 || tid >= pid_max[ns->level - i])
				goto out_abort;
			/*
			 * 指定 PID 会改变可见数字布局，
			 * 只允许恢复/迁移类权限使用。
			 */
			retval = -EPERM;
			if (!checkpoint_restore_ns_capable(tmp->user_ns))
				goto out_abort;
			arg_set_tid_size--;
		}

		tmp = tmp->parent;
	}

	/*
	 * Prep is done, id allocation goes here:
	 *
	 * 中文补充：从这里开始进入真正的 idr 分配阶段。
	 * idr_preload() 在拿自旋锁前准备内存；锁内使用 GFP_ATOMIC。
	 */
	retried_preload = false;
	idr_preload(GFP_KERNEL);
	spin_lock(&pidmap_lock);
	/* For the case when the previous attempt to create init failed */
	/*
	 * 中文补充：新 PID namespace 的第一个进程必须是 PID 1。
	 * 如果上一次创建 init 失败，cursor 可能前进；这里重置后重新尝试。
	 */
	if (ns->pid_allocated == PIDNS_ADDING)
		idr_set_cursor(&ns->idr, 0);

	for (tmp = ns, i = ns->level; i >= 0;) {
		int tid = set_tid[ns->level - i];

		if (tid) {
			/* 指定 TID 时只分配 [tid, tid + 1) 这一格。 */
			nr = idr_alloc(&tmp->idr, NULL, tid,
				       tid + 1, GFP_ATOMIC);
			/*
			 * If ENOSPC is returned it means that the PID is
			 * alreay in use. Return EEXIST in that case.
			 *
			 * 中文补充：自动分配的 ENOSPC 是空间问题。
			 * 指定 TID 的 ENOSPC 是“目标数字已存在”，应报 EEXIST。
			 */
			if (nr == -ENOSPC)

				nr = -EEXIST;
		} else {
			int pid_min = 1;
			/*
			 * init really needs pid 1, but after reaching the
			 * maximum wrap back to RESERVED_PIDS
			 *
			 * 中文补充：初始化阶段需要 PID 1。
			 * 普通回绕分配则避开低号保留段，从 RESERVED_PIDS 开始。
			 */
			if (idr_get_cursor(&tmp->idr) > RESERVED_PIDS)
				pid_min = RESERVED_PIDS;

			/*
			 * Store a null pointer so find_pid_ns does not find
			 * a partially initialized PID (see below).
			 *
			 * 中文补充：NULL 表示“号已占住，但对象尚未发布”。
			 * 这避免查找路径拿到 numbers[] 尚未填完整的 struct pid。
			 */
			nr = idr_alloc_cyclic(&tmp->idr, NULL, pid_min,
					      pid_max[ns->level - i], GFP_ATOMIC);
			if (nr == -ENOSPC)
				nr = -EAGAIN;
		}

		if (unlikely(nr < 0)) {
			/*
			 * Preload more memory if idr_alloc{,cyclic} failed with -ENOMEM.
			 *
			 * The IDR API only allows us to preload memory for one call, while we may end
			 * up doing several under pidmap_lock with GFP_ATOMIC. The situation may be
			 * salvageable with GFP_KERNEL. But make sure to not loop indefinitely if preload
			 * did not help (the routine unfortunately returns void, so we have no idea
			 * if it got anywhere).
			 *
			 * The lock can be safely dropped and picked up as historically pid allocation
			 * for different namespaces was *not* atomic -- we try to hold on to it the
			 * entire time only for performance reasons.
			 *
			 * 中文补充：这里的重试只做一次。
			 * idr_preload() 不返回是否真正拿到内存，
			 * 无限制重试会卡住 fork。
			 * 短暂放开 pidmap_lock 可以接受；
			 * 跨 namespace 分配本来不对外
			 * 承诺整体原子性，长时间持锁主要是性能优化。
			 */
			if (nr == -ENOMEM && !retried_preload) {
				spin_unlock(&pidmap_lock);
				idr_preload_end();
				retried_preload = true;
				idr_preload(GFP_KERNEL);
				spin_lock(&pidmap_lock);
				continue;
			}
			retval = nr;
			goto out_free;
		}

		/* 当前 namespace 层分配成功，但还没有对 find_pid_ns() 发布。 */
		pid->numbers[i].nr = nr;
		pid->numbers[i].ns = tmp;
		i--;
		retried_preload = false;

		/*
		 * PID 1 (init) must be created first.
		 *
		 * 中文补充：没有 child_reaper 时，只允许创建 PID 1。
		 * 否则 namespace 还没有回收者，
		 * 后续孤儿进程和退出流程无法成立。
		 */
		if (!READ_ONCE(tmp->child_reaper) && nr != 1) {
			retval = -EINVAL;
			goto out_free;
		}

		tmp = tmp->parent;
	}

	/*
	 * ENOMEM is not the most obvious choice especially for the case
	 * where the child subreaper has already exited and the pid
	 * namespace denies the creation of any new processes. But ENOMEM
	 * is what we have exposed to userspace for a long time and it is
	 * documented behavior for pid namespaces. So we can't easily
	 * change it even if there were an error code better suited.
	 *
	 * This can't be done earlier because we need to preserve other
	 * error conditions.
	 *
	 * We need this even if copy_process() does the same check. If two
	 * or more tasks from parent namespace try to inject a child into a
	 * dead namespace, one of free_pid() calls from the copy_process()
	 * error path may try to wakeup the possibly freed ns->child_reaper.
	 *
	 * 中文补充：namespace 停止接收新 PID 时，历史 ABI 要返回 ENOMEM。
	 * 这不完全直观，但用户态已经依赖该行为。
	 */
	retval = -ENOMEM;
	if (unlikely(!(ns->pid_allocated & PIDNS_ADDING)))
		goto out_free;
	for (upid = pid->numbers + ns->level; upid >= pid->numbers; --upid) {
		/* Make the PID visible to find_pid_ns. */
		/*
		 * 中文补充：这是发布点。
		 * idr 槽位从 NULL 替换为完整 pid 后，RCU 查找者才能看到它。
		 */
		idr_replace(&upid->ns->idr, pid, upid->nr);
		upid->ns->pid_allocated++;
	}
	spin_unlock(&pidmap_lock);
	idr_preload_end();
	/* active 引用用于协调 namespace teardown 与 fork/exit 并发。 */
	ns_ref_active_get(ns);

	retval = pidfs_add_pid(pid);
	if (unlikely(retval)) {
		/* pid 已发布进 idr，pidfs 失败必须走 free_pid() 完整回滚。 */
		free_pid(pid);
		pid = ERR_PTR(-ENOMEM);
	}

	return pid;

out_free:
	/* 回滚已成功占用但尚未最终发布的 namespace 数字。 */
	while (++i <= ns->level) {
		upid = pid->numbers + i;
		idr_remove(&upid->ns->idr, upid->nr);
	}

	spin_unlock(&pidmap_lock);
	idr_preload_end();

out_abort:
	/* 拿 pidmap_lock 前失败，没有 idr 槽位需要回滚。 */
	put_pid_ns(ns);
	kmem_cache_free(ns->pid_cachep, pid);
	return ERR_PTR(retval);
}

/*
 * disable_pid_allocation() - 禁止 namespace 继续分配新 PID。
 * @ns: 入参/出参，目标 PID namespace。
 *
 * 返回值：无。
 *
 * 背景：namespace 退出时清除 PIDNS_ADDING。
 * 后续 alloc_pid() 会按历史 ABI 失败为 -ENOMEM。
 */
void disable_pid_allocation(struct pid_namespace *ns)
{
	spin_lock(&pidmap_lock);
	ns->pid_allocated &= ~PIDNS_ADDING;
	spin_unlock(&pidmap_lock);
}

/*
 * find_pid_ns() - 在指定 namespace 中按数字查找 struct pid。
 * @nr: 入参，namespace 内的 PID 数字。
 * @ns: 入参，查找所在的 PID namespace。
 *
 * 返回值：找到返回 struct pid *，否则返回 NULL；不自动增加引用。
 *
 * 注意：调用者必须持有 tasklist_lock 或 rcu_read_lock()。
 * 若要把结果带出临界区，必须 get_pid()。
 */
struct pid *find_pid_ns(int nr, struct pid_namespace *ns)
{
	return idr_find(&ns->idr, nr);
}
EXPORT_SYMBOL_GPL(find_pid_ns);

/*
 * find_vpid() - 在 current 的 active PID namespace 中查找 PID。
 * @nr: 入参，当前 namespace 可见的 PID 数字。
 *
 * 返回值：同 find_pid_ns()，不带引用。
 */
struct pid *find_vpid(int nr)
{
	return find_pid_ns(nr, task_active_pid_ns(current));
}
EXPORT_SYMBOL_GPL(find_vpid);

/*
 * task_pid_ptr() - 返回 task 中某类 pid 指针槽位。
 * @task: 入参，目标 task。
 * @type: 入参，PIDTYPE_PID/TGID/PGID/SID。
 *
 * 返回值：指向 task 内部 pid 指针的地址。
 *
 * 背景：线程自己的 PID 在 task->thread_pid；
 * 线程组/进程组/会话这类共享身份在 signal->pids[]。
 */
static struct pid **task_pid_ptr(struct task_struct *task, enum pid_type type)
{
	return (type == PIDTYPE_PID) ?
		&task->thread_pid :
		&task->signal->pids[type];
}

/*
 * attach_pid() must be called with the tasklist_lock write-held.
 *
 * 中文补充：这是 PID 绑定关系的写侧规则。
 * pid->tasks[] 是 RCU 链表；写者用 tasklist_lock 串行化修改。
 */
void attach_pid(struct task_struct *task, enum pid_type type)
{
	struct pid *pid;

	lockdep_assert_held_write(&tasklist_lock);

	/* task 中的 pid 指针应已安装，这里只把 task 挂入 pid 的反向链表。 */
	pid = *task_pid_ptr(task, type);
	hlist_add_head_rcu(&task->pid_links[type], &pid->tasks[type]);
}

/*
 * __change_pid() - 替换或清空 task 的某类 pid 绑定。
 * @pids: 出参，收集可能需要释放的旧 pid。
 * @task: 入参/出参，目标 task。
 * @type: 入参，要修改的 PID 类型。
 * @new: 入参，新 pid；NULL 表示 detach。
 *
 * 返回值：无。
 *
 * 注意：旧 pid 只有在所有 PIDTYPE 链表都空时才可释放。
 * 一个 struct pid 可能同时服务 TGID/PGID/SID 等身份。
 */
static void __change_pid(struct pid **pids, struct task_struct *task,
			 enum pid_type type, struct pid *new)
{
	struct pid **pid_ptr, *pid;
	int tmp;

	lockdep_assert_held_write(&tasklist_lock);

	pid_ptr = task_pid_ptr(task, type);
	pid = *pid_ptr;

	/* 从旧 pid 的 RCU 链表摘除该 task。 */
	hlist_del_rcu(&task->pid_links[type]);
	*pid_ptr = new;

	/* 任一 PIDTYPE 仍有 task 使用该 pid，就不能释放。 */
	for (tmp = PIDTYPE_MAX; --tmp >= 0; )
		if (pid_has_task(pid, tmp))
			return;

	WARN_ON(pids[type]);
	pids[type] = pid;
}

/*
 * detach_pid() - 解除 task 的某类 pid 绑定。
 * @pids: 出参，收集可能需要释放的旧 pid。
 * @task: 入参/出参，目标 task。
 * @type: 入参，PID 类型。
 */
void detach_pid(struct pid **pids, struct task_struct *task, enum pid_type type)
{
	__change_pid(pids, task, type, NULL);
}

/*
 * change_pid() - 把 task 的某类 pid 切换为新 pid。
 * @pids: 出参，收集可能需要释放的旧 pid。
 * @task: 入参/出参，目标 task。
 * @type: 入参，PID 类型。
 * @pid: 入参，新 struct pid。
 */
void change_pid(struct pid **pids, struct task_struct *task, enum pid_type type,
		struct pid *pid)
{
	__change_pid(pids, task, type, pid);
	attach_pid(task, type);
}

/*
 * exchange_tids() - 交换两个 task 的线程 PID。
 * @left: 入参/出参，交换的一方。
 * @right: 入参/出参，交换的另一方。
 *
 * 背景：de_thread() 等路径会转移线程组 leader。
 * 必须同时交换 hlist、thread_pid 指针和 task->pid 缓存。
 */
void exchange_tids(struct task_struct *left, struct task_struct *right)
{
	struct pid *pid1 = left->thread_pid;
	struct pid *pid2 = right->thread_pid;
	struct hlist_head *head1 = &pid1->tasks[PIDTYPE_PID];
	struct hlist_head *head2 = &pid2->tasks[PIDTYPE_PID];

	lockdep_assert_held_write(&tasklist_lock);

	/* Swap the single entry tid lists */
	/* PIDTYPE_PID 链表通常单元素，交换链表头比删插更直接。 */
	hlists_swap_heads_rcu(head1, head2);

	/* Swap the per task_struct pid */
	/* rcu_assign_pointer() 发布新的 thread_pid 指针给 RCU 读者。 */
	rcu_assign_pointer(left->thread_pid, pid2);
	rcu_assign_pointer(right->thread_pid, pid1);

	/* Swap the cached value */
	/* task->pid 是快速缓存，必须和 thread_pid 对应数字保持一致。 */
	WRITE_ONCE(left->pid, pid_nr(pid2));
	WRITE_ONCE(right->pid, pid_nr(pid1));
}

/* transfer_pid is an optimization of attach_pid(new), detach_pid(old) */
/*
 * 中文补充：这是共享 PID 身份转移的快速路径。
 * 禁止 PIDTYPE_PID，因为线程 PID 还涉及 task->pid 缓存和 thread_pid。
 */
void transfer_pid(struct task_struct *old, struct task_struct *new,
			   enum pid_type type)
{
	WARN_ON_ONCE(type == PIDTYPE_PID);
	lockdep_assert_held_write(&tasklist_lock);
	hlist_replace_rcu(&old->pid_links[type], &new->pid_links[type]);
}

/*
 * pid_task() - 从 pid 的某类 task 链表取第一个 task。
 * @pid: 入参，可为 NULL。
 * @type: 入参，PID 类型。
 *
 * 返回值：task_struct * 或 NULL；返回值不带引用。
 *
 * 注意：调用者必须在 RCU 或 tasklist_lock 保护下使用返回值。
 * TGID/PGID/SID 可能有多个 task，本函数只返回链表第一个。
 */
struct task_struct *pid_task(struct pid *pid, enum pid_type type)
{
	struct task_struct *result = NULL;
	if (pid) {
		struct hlist_node *first;
		/* 同时允许 RCU 读侧和 tasklist_lock 保护的读侧。 */
		first = rcu_dereference_check(hlist_first_rcu(&pid->tasks[type]),
					      lockdep_tasklist_lock_is_held());
		if (first)
			result = hlist_entry(first, struct task_struct, pid_links[(type)]);
	}
	return result;
}
EXPORT_SYMBOL(pid_task);

/*
 * Must be called under rcu_read_lock().
 *
 * 中文补充：返回 task 不带引用，只能在 RCU 临界区内安全使用。
 */
struct task_struct *find_task_by_pid_ns(pid_t nr, struct pid_namespace *ns)
{
	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "find_task_by_pid_ns() needs rcu_read_lock() protection");
	return pid_task(find_pid_ns(nr, ns), PIDTYPE_PID);
}

/*
 * find_task_by_vpid() - 按当前 namespace 可见 PID 查找 task。
 * @vnr: 入参，虚拟 PID。
 *
 * 返回值：task_struct * 或 NULL；不带引用。
 */
struct task_struct *find_task_by_vpid(pid_t vnr)
{
	return find_task_by_pid_ns(vnr, task_active_pid_ns(current));
}

/*
 * find_get_task_by_vpid() - 查找当前 namespace PID 并持有 task 引用。
 * @nr: 入参，虚拟 PID。
 *
 * 返回值：成功返回带引用的 task；找不到返回 NULL。
 * 调用者负责 put_task_struct()。
 */
struct task_struct *find_get_task_by_vpid(pid_t nr)
{
	struct task_struct *task;

	rcu_read_lock();
	task = find_task_by_vpid(nr);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	return task;
}

/*
 * get_task_pid() - 取得 task 某类 struct pid 引用。
 * @task: 入参，目标 task。
 * @type: 入参，PID 类型。
 *
 * 返回值：带引用的 struct pid * 或 NULL。调用者负责 put_pid()。
 *
 * 注意：这只 pin struct pid，不保证 task 仍存活。
 */
struct pid *get_task_pid(struct task_struct *task, enum pid_type type)
{
	struct pid *pid;
	rcu_read_lock();
	pid = get_pid(rcu_dereference(*task_pid_ptr(task, type)));
	rcu_read_unlock();
	return pid;
}
EXPORT_SYMBOL_GPL(get_task_pid);

/*
 * get_pid_task() - 从 struct pid 找 task，并持有 task 引用。
 * @pid: 入参，目标 pid。
 * @type: 入参，PID 类型。
 *
 * 返回值：带引用 task 或 NULL。调用者负责 put_task_struct()。
 */
struct task_struct *get_pid_task(struct pid *pid, enum pid_type type)
{
	struct task_struct *result;
	rcu_read_lock();
	result = pid_task(pid, type);
	if (result)
		get_task_struct(result);
	rcu_read_unlock();
	return result;
}
EXPORT_SYMBOL_GPL(get_pid_task);

/*
 * find_get_pid() - 按当前 namespace 数字查找并持有 pid 引用。
 * @nr: 入参，虚拟 PID。
 *
 * 返回值：带引用 struct pid 或 NULL。调用者负责 put_pid()。
 */
struct pid *find_get_pid(pid_t nr)
{
	struct pid *pid;

	rcu_read_lock();
	pid = get_pid(find_vpid(nr));
	rcu_read_unlock();

	return pid;
}
EXPORT_SYMBOL_GPL(find_get_pid);

/*
 * pid_nr_ns() - 返回 pid 在指定 namespace 中的可见数字。
 * @pid: 入参，可为 NULL。
 * @ns: 入参，可为 NULL。
 *
 * 返回值：可见则返回 pid_t；不可见或参数为空返回 0。
 *
 * 注意：0 表示无可见 PID，不是稳定进程身份。
 */
pid_t pid_nr_ns(struct pid *pid, struct pid_namespace *ns)
{
	struct upid *upid;
	pid_t nr = 0;

	if (pid && ns && ns->level <= pid->level) {
		upid = &pid->numbers[ns->level];
		if (upid->ns == ns)
			nr = upid->nr;
	}
	return nr;
}
EXPORT_SYMBOL_GPL(pid_nr_ns);

/*
 * pid_vnr() - 返回 pid 在 current active namespace 中的数字。
 * @pid: 入参，可为 NULL。
 *
 * 返回值：当前 namespace 可见 PID；不可见返回 0。
 */
pid_t pid_vnr(struct pid *pid)
{
	return pid_nr_ns(pid, task_active_pid_ns(current));
}
EXPORT_SYMBOL_GPL(pid_vnr);

/*
 * __task_pid_nr_ns() - 取 task 某类 PID 在 namespace 中的数字。
 * @task: 入参，目标 task。
 * @type: 入参，PID 类型。
 * @ns: 入参，NULL 表示 current active namespace。
 *
 * 返回值：数字快照；不可见返回 0。
 *
 * 注意：返回的是可复用数字，不是长期稳定引用。
 */
pid_t __task_pid_nr_ns(struct task_struct *task, enum pid_type type,
			struct pid_namespace *ns)
{
	pid_t nr = 0;

	rcu_read_lock();
	if (!ns)
		ns = task_active_pid_ns(current);
	nr = pid_nr_ns(rcu_dereference(*task_pid_ptr(task, type)), ns);
	rcu_read_unlock();

	return nr;
}
EXPORT_SYMBOL(__task_pid_nr_ns);

/*
 * task_active_pid_ns() - 返回 task 所属的最内层 PID namespace。
 * @tsk: 入参，目标 task。
 *
 * 返回值：pid namespace；若 task 没有 pid，可能返回 NULL。
 */
struct pid_namespace *task_active_pid_ns(struct task_struct *tsk)
{
	return ns_of_pid(task_pid(tsk));
}
EXPORT_SYMBOL_GPL(task_active_pid_ns);

/*
 * Used by proc to find the first pid that is greater than or equal to nr.
 *
 * If there is a pid at nr this function is exactly the same as find_pid_ns.
 *
 * 中文补充：/proc 遍历需要“从 nr 开始找下一个 PID”。
 * 这和精确查找不同，idr_get_next() 提供游标式查找。
 * 返回值不带引用，读侧仍需 RCU 或 tasklist_lock。
 */
struct pid *find_ge_pid(int nr, struct pid_namespace *ns)
{
	return idr_get_next(&ns->idr, &nr);
}
EXPORT_SYMBOL_GPL(find_ge_pid);

/*
 * pidfd_get_pid() - 从 fd 解析 pidfd，并获取 struct pid 引用。
 * @fd: 入参，用户传入的文件描述符。
 * @flags: 出参，成功时写入 file flags。
 *
 * 返回值：成功返回带引用 struct pid；失败返回 ERR_PTR(-errno)。
 *
 * 背景：pidfd 的优点是稳定引用进程身份，避免 pid_t 复用问题。
 * 但打开 fd 时仍要验证它确实是 pidfd。
 */
struct pid *pidfd_get_pid(unsigned int fd, unsigned int *flags)
{
	CLASS(fd, f)(fd);
	struct pid *pid;

	if (fd_empty(f))
		return ERR_PTR(-EBADF);

	pid = pidfd_pid(fd_file(f));
	if (!IS_ERR(pid)) {
		get_pid(pid);
		*flags = fd_file(f)->f_flags;
	}
	return pid;
}

/**
 * pidfd_get_task() - Get the task associated with a pidfd
 *
 * @pidfd: pidfd for which to get the task
 * @flags: flags associated with this pidfd
 *
 * Return the task associated with @pidfd. The function takes a reference on
 * the returned task. The caller is responsible for releasing that reference.
 *
 * Return: On success, the task_struct associated with the pidfd.
 *	   On error, a negative errno number will be returned.
 *
 * 中文补充：成功返回带引用 task，调用者负责 put_task_struct()。
 * pidfd 稳定引用 struct pid，但目标 task 可能已经退出。
 * 所以这里还需要 get_pid_task()，失败时返回 -ESRCH。
 */
struct task_struct *pidfd_get_task(int pidfd, unsigned int *flags)
{
	unsigned int f_flags = 0;
	struct pid *pid;
	struct task_struct *task;
	enum pid_type type;

	switch (pidfd) {
	case  PIDFD_SELF_THREAD:
		/* 特殊值：当前线程本身，按 PIDTYPE_PID 解释。 */
		type = PIDTYPE_PID;
		pid = get_task_pid(current, type);
		break;
	case  PIDFD_SELF_THREAD_GROUP:
		/* 特殊值：当前线程组，按 PIDTYPE_TGID 解释。 */
		type = PIDTYPE_TGID;
		pid = get_task_pid(current, type);
		break;
	default:
		/* 普通 fd 必须是 pidfd；默认取线程组语义。 */
		pid = pidfd_get_pid(pidfd, &f_flags);
		if (IS_ERR(pid))
			return ERR_CAST(pid);
		type = PIDTYPE_TGID;
		break;
	}

	task = get_pid_task(pid, type);
	put_pid(pid);
	if (!task)
		return ERR_PTR(-ESRCH);

	*flags = f_flags;
	return task;
}

/**
 * pidfd_create() - Create a new pid file descriptor.
 *
 * @pid:   struct pid that the pidfd will reference
 * @flags: flags to pass
 *
 * This creates a new pid file descriptor with the O_CLOEXEC flag set.
 *
 * Note, that this function can only be called after the fd table has
 * been unshared to avoid leaking the pidfd to the new process.
 *
 * This symbol should not be explicitly exported to loadable modules.
 *
 * Return: On success, a cloexec pidfd is returned.
 *         On error, a negative errno number will be returned.
 *
 * 中文补充：pidfd_create() 分两步：pidfd_prepare() 分配 fd/file，
 * fd_install() 再发布到当前进程 fdtable。
 *
 * 注意：一旦 fd_install() 完成，用户态就能看到该 fd。
 * 因此调用者必须保证 fd table 已经 unshare，避免 fork 时泄漏给子进程。
 */
static int pidfd_create(struct pid *pid, unsigned int flags)
{
	int pidfd;
	struct file *pidfd_file;

	pidfd = pidfd_prepare(pid, flags, &pidfd_file);
	if (pidfd < 0)
		return pidfd;

	/* 发布点：fd 从这里开始对当前进程可见。 */
	fd_install(pidfd, pidfd_file);
	return pidfd;
}

/**
 * sys_pidfd_open() - Open new pid file descriptor.
 *
 * @pid:   pid for which to retrieve a pidfd
 * @flags: flags to pass
 *
 * This creates a new pid file descriptor with the O_CLOEXEC flag set for
 * the task identified by @pid. Without PIDFD_THREAD flag the target task
 * must be a thread-group leader.
 *
 * Return: On success, a cloexec pidfd is returned.
 *         On error, a negative errno number will be returned.
 *
 * 中文补充：pidfd_open() 把当前 namespace 中的 pid_t 转成稳定 fd。
 *
 * 优点：后续操作使用 fd，不再受 PID 数字复用影响。
 * 限制：打开瞬间仍按数字查找，目标当时必须存在。
 */
SYSCALL_DEFINE2(pidfd_open, pid_t, pid, unsigned int, flags)
{
	int fd;
	struct pid *p;

	/* 拒绝未知 flag，避免旧内核静默忽略未来语义。 */
	if (flags & ~(PIDFD_NONBLOCK | PIDFD_THREAD))
		return -EINVAL;

	/* pidfd_open 面向普通正 PID。 */
	if (pid <= 0)
		return -EINVAL;

	/* 查找并持有 struct pid，防止创建 pidfd 期间对象消失。 */
	p = find_get_pid(pid);
	if (!p)
		return -ESRCH;

	fd = pidfd_create(p, flags);

	put_pid(p);
	return fd;
}

#ifdef CONFIG_SYSCTL
/*
 * pid_table_root_lookup() - 按 current 选择可见 sysctl set。
 *
 * 返回值：current active PID namespace 的 ctl_table_set。
 */
static struct ctl_table_set *pid_table_root_lookup(struct ctl_table_root *root)
{
	return &task_active_pid_ns(current)->set;
}

/*
 * set_is_seen() - 判断 sysctl set 对 current 是否可见。
 * @set: 入参，待判断 set。
 *
 * 返回值：可见返回非 0，否则返回 0。
 */
static int set_is_seen(struct ctl_table_set *set)
{
	return &task_active_pid_ns(current)->set == set;
}

/*
 * pid_table_root_permissions() - 折算 PID namespace sysctl 权限。
 * @head: 入参，sysctl header。
 * @table: 入参，sysctl 表项。
 *
 * 返回值：对 current 生效的 mode。
 *
 * 背景：容器内 root 要按 pidns->user_ns 的 uid/gid 映射判断权限。
 */
static int pid_table_root_permissions(struct ctl_table_header *head,
				      const struct ctl_table *table)
{
	struct pid_namespace *pidns =
		container_of(head->set, struct pid_namespace, set);
	int mode = table->mode;

	if (ns_capable_noaudit(pidns->user_ns, CAP_SYS_ADMIN) ||
	    uid_eq(current_euid(), make_kuid(pidns->user_ns, 0)))
		mode = (mode & S_IRWXU) >> 6;
	else if (in_egroup_p(make_kgid(pidns->user_ns, 0)))
		mode = (mode & S_IRWXG) >> 3;
	else
		mode = mode & S_IROTH;
	/* 将选中的 3 位权限复制到 user/group/other 三组。 */
	return (mode << 6) | (mode << 3) | mode;
}

/*
 * pid_table_root_set_ownership() - 设置 sysctl 文件属主。
 * @head: 入参，sysctl header。
 * @uid: 出参，namespace root uid 有效时写入。
 * @gid: 出参，namespace root gid 有效时写入。
 */
static void pid_table_root_set_ownership(struct ctl_table_header *head,
					 kuid_t *uid, kgid_t *gid)
{
	struct pid_namespace *pidns =
		container_of(head->set, struct pid_namespace, set);
	kuid_t ns_root_uid;
	kgid_t ns_root_gid;

	ns_root_uid = make_kuid(pidns->user_ns, 0);
	if (uid_valid(ns_root_uid))
		*uid = ns_root_uid;

	ns_root_gid = make_kgid(pidns->user_ns, 0);
	if (gid_valid(ns_root_gid))
		*gid = ns_root_gid;
}

/* PID namespace sysctl 根，集中定义 lookup、权限和属主映射策略。 */
static struct ctl_table_root pid_table_root = {
	.lookup		= pid_table_root_lookup,
	.permissions	= pid_table_root_permissions,
	.set_ownership	= pid_table_root_set_ownership,
};

/*
 * proc_do_cad_pid() - cad_pid sysctl 的读写处理器。
 * @table: 入参，sysctl 表项。
 * @write: 入参，非 0 表示写。
 * @buffer: 入参/出参，用户缓冲区。
 * @lenp: 入参/出参，处理长度。
 * @ppos: 入参/出参，proc 文件偏移。
 *
 * 返回值：0 或负 errno。
 *
 * 背景：cad_pid 内部保存 struct pid，sysctl 展示 pid_t。
 * 读写时必须在二者之间转换，避免长期保存裸 PID。
 */
static int proc_do_cad_pid(const struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos)
{
	struct pid *new_pid;
	pid_t tmp_pid;
	int r;
	struct ctl_table tmp_table = *table;

	/* 读路径展示当前 namespace 可见数字。 */
	tmp_pid = pid_vnr(cad_pid);
	tmp_table.data = &tmp_pid;

	r = proc_dointvec(&tmp_table, write, buffer, lenp, ppos);
	if (r || !write)
		return r;

	/* 写路径把用户数字解析成带引用 struct pid。 */
	new_pid = find_get_pid(tmp_pid);
	if (!new_pid)
		return -ESRCH;

	/* 原子替换 cad_pid，并释放旧 pid 引用。 */
	put_pid(xchg(&cad_pid, new_pid));
	return 0;
}

static const struct ctl_table pid_table[] = {
	{
		/* pid_max 是每个 PID namespace 的自动分配上限。 */
		.procname	= "pid_max",
		.data		= &init_pid_ns.pid_max,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &pid_max_min,
		.extra2		= &pid_max_max,
	},
#ifdef CONFIG_PROC_SYSCTL
	{
		/* cad_pid 写入后会转为稳定 struct pid 引用保存。 */
		.procname	= "cad_pid",
		.maxlen		= sizeof(int),
		.mode		= 0600,
		.proc_handler	= proc_do_cad_pid,
	},
#endif
};
#endif

/*
 * register_pidns_sysctls() - 注册某个 PID namespace 的 sysctl 表。
 * @pidns: 入参/出参，目标 namespace。
 *
 * 返回值：0 或 -ENOMEM。
 *
 * 背景：每个 PID namespace 需要独立 pid_max。
 * 因此这里复制 pid_table，再把 data 指向 pidns->pid_max。
 */
int register_pidns_sysctls(struct pid_namespace *pidns)
{
#ifdef CONFIG_SYSCTL
	struct ctl_table *tbl;

	/* 将该 namespace 接入 pid_table_root 的可见性和权限模型。 */
	setup_sysctl_set(&pidns->set, &pid_table_root, set_is_seen);

	tbl = kmemdup(pid_table, sizeof(pid_table), GFP_KERNEL);
	if (!tbl)
		return -ENOMEM;
	/* pid_table[0] 是 pid_max；副本必须指向本 namespace 字段。 */
	tbl->data = &pidns->pid_max;
	/* 按 CPU 数量提高默认 pid_max，降低高并发系统的 PID 快速复用。 */
	pidns->pid_max = min(pid_max_max, max_t(int, pidns->pid_max,
			     PIDS_PER_CPU_DEFAULT * num_possible_cpus()));

	pidns->sysctls = __register_sysctl_table(&pidns->set, "kernel", tbl,
						 ARRAY_SIZE(pid_table));
	if (!pidns->sysctls) {
		/* 注册失败时撤销 set，并释放复制出的 sysctl 表。 */
		kfree(tbl);
		retire_sysctl_set(&pidns->set);
		return -ENOMEM;
	}
#endif
	return 0;
}

/*
 * unregister_pidns_sysctls() - 注销 PID namespace sysctl 表。
 * @pidns: 入参/出参，目标 namespace。
 *
 * 返回值：无。
 *
 * 注意：ctl_table 是 register 时 kmemdup() 的副本，必须释放。
 */
void unregister_pidns_sysctls(struct pid_namespace *pidns)
{
#ifdef CONFIG_SYSCTL
	const struct ctl_table *tbl;

	tbl = pidns->sysctls->ctl_table_arg;
	unregister_sysctl_table(pidns->sysctls);
	retire_sysctl_set(&pidns->set);
	kfree(tbl);
#endif
}

/*
 * pid_idr_init() - 初始化根 PID namespace 的 idr 和 pid cache。
 *
 * 返回值：无。只在启动期执行。
 */
void __init pid_idr_init(void)
{
	/* Verify no one has done anything silly: */
	/*
	 * 中文补充：PID_MAX_LIMIT 不能碰到 PIDNS_ADDING 哨兵范围。
	 * 否则普通计数和 namespace 状态位会混淆。
	 */
	BUILD_BUG_ON(PID_MAX_LIMIT >= PIDNS_ADDING);

	/* bump default and minimum pid_max based on number of cpus */
	/*
	 * 中文补充：CPU 越多，fork/exit 并发越高。
	 * 提高 pid_max 可以降低 PID 数字快速回绕概率。
	 */
	init_pid_ns.pid_max = min(pid_max_max, max_t(int, init_pid_ns.pid_max,
				  PIDS_PER_CPU_DEFAULT * num_possible_cpus()));
	pid_max_min = max_t(int, pid_max_min,
				PIDS_PER_CPU_MIN * num_possible_cpus());
	pr_info("pid_max: default: %u minimum: %u\n", init_pid_ns.pid_max, pid_max_min);

	/* 初始化根 namespace 的数字分配 idr。 */
	idr_init(&init_pid_ns.idr);

	/* init_pid_ns level 为 0，所以 struct pid 只需要 1 个 numbers[] 元素。 */
	init_pid_ns.pid_cachep = kmem_cache_create("pid",
			struct_size_t(struct pid, numbers, 1),
			__alignof__(struct pid),
			SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_ACCOUNT,
			NULL);
}

/*
 * pid_namespace_sysctl_init() - 为 init_pid_ns 注册 sysctl。
 *
 * 返回值：0。
 */
static __init int pid_namespace_sysctl_init(void)
{
#ifdef CONFIG_SYSCTL
	/* "kernel" directory will have already been initialized. */
	/* 中文补充：这里只注册 kernel/ 下的 PID 相关叶子表项。 */
	BUG_ON(register_pidns_sysctls(&init_pid_ns));
#endif
	return 0;
}
subsys_initcall(pid_namespace_sysctl_init);

/*
 * __pidfd_fget() - 权限检查后，从目标 task 获取一个 file 引用。
 * @task: 入参，目标 task。
 * @fd: 入参，目标 task 文件表中的 fd。
 *
 * 返回值：成功返回带引用 file；失败返回 ERR_PTR(-errno)。
 *
 * 背景：pidfd_getfd() 允许有权限的进程复制另一个进程的 fd。
 * 这里复用 ptrace 权限模型，并用 exec_update_lock 避免 exec 并发。
 *
 * 注意：目标任务退出时，files 可能已经释放。
 * 某些 EBADF 会被修正为 ESRCH，以表达“目标任务正在退出”。
 */
static struct file *__pidfd_fget(struct task_struct *task, int fd)
{
	struct file *file;
	int ret;

	ret = down_read_killable(&task->signal->exec_update_lock);
	if (ret)
		return ERR_PTR(ret);

	/* 权限检查和取 fd 放在 exec_update_lock 下，避免 exec 凭据竞态。 */
	if (!ptrace_may_access(task, PTRACE_MODE_ATTACH_REALCREDS))
		file = ERR_PTR(-EPERM);
	else if (task->flags & PF_EXITING)
		file = ERR_PTR(-ESRCH);
	else
		/* 成功时返回带引用 file，调用者之后必须 fput()。 */
		file = fget_task(task, fd);

	up_read(&task->signal->exec_update_lock);

	if (!file) {
		/*
		 * It is possible that the target thread is exiting; it can be
		 * either:
		 * 1. before exit_signals(), which gives a real fd
		 * 2. before exit_files() takes the task_lock() gives a real fd
		 * 3. after exit_files() releases task_lock(), ->files is NULL;
		 *    this has PF_EXITING, since it was set in exit_signals(),
		 *    __pidfd_fget() returns EBADF.
		 * In case 3 we get EBADF, but that really means ESRCH, since
		 * the task is currently exiting and has freed its files
		 * struct, so we fix it up.
		 *
		 * 中文补充：如果 files 已经释放，
		 * 用户更需要知道目标任务消失，
		 * 而不是误以为只是目标 fd 编号不存在。
		 */
		if (task->flags & PF_EXITING)
			file = ERR_PTR(-ESRCH);
		else
			file = ERR_PTR(-EBADF);
	}

	return file;
}

/*
 * pidfd_getfd() - 通过 struct pid 复制目标 task 的 fd。
 * @pid: 入参，pidfd 关联的 struct pid。
 * @fd: 入参，目标 fd 编号。
 *
 * 返回值：成功返回当前进程中新安装的 cloexec fd；失败返回负 errno。
 */
static int pidfd_getfd(struct pid *pid, int fd)
{
	struct task_struct *task;
	struct file *file;
	int ret;

	task = get_pid_task(pid, PIDTYPE_PID);
	if (!task)
		return -ESRCH;

	/* 先 pin 住目标 task，再取目标 file。 */
	file = __pidfd_fget(task, fd);
	put_task_struct(task);
	if (IS_ERR(file))
		return PTR_ERR(file);

	/* receive_fd() 把 file 安装到当前进程，O_CLOEXEC 避免 exec 泄漏。 */
	ret = receive_fd(file, NULL, O_CLOEXEC);
	fput(file);

	return ret;
}

/**
 * sys_pidfd_getfd() - Get a file descriptor from another process
 *
 * @pidfd:	the pidfd file descriptor of the process
 * @fd:		the file descriptor number to get
 * @flags:	flags on how to get the fd (reserved)
 *
 * This syscall gets a copy of a file descriptor from another process
 * based on the pidfd, and file descriptor number. It requires that
 * the calling process has the ability to ptrace the process represented
 * by the pidfd. The process which is having its file descriptor copied
 * is otherwise unaffected.
 *
 * Return: On success, a cloexec file descriptor is returned.
 *         On error, a negative errno number will be returned.
 *
 * 中文补充：从 pidfd 指向的进程复制一个 fd 到当前进程。
 *
 * 优点：目标由 pidfd 稳定定位，不受 pid_t 复用影响。
 * 风险：能力很强，所以必须经过 pidfd 验证和 ptrace 权限检查。
 */
SYSCALL_DEFINE3(pidfd_getfd, int, pidfd, int, fd,
		unsigned int, flags)
{
	struct pid *pid;

	/* flags is currently unused - make sure it's unset */
	/* 中文补充：保留 flags 必须为 0，防止旧内核忽略未来语义。 */
	if (flags)
		return -EINVAL;

	/* 先把用户 fd 转成 file；不是打开 fd 则返回 EBADF。 */
	CLASS(fd, f)(pidfd);
	if (fd_empty(f))
		return -EBADF;

	/* 验证 file 是 pidfd，并取出它关联的 struct pid。 */
	pid = pidfd_pid(fd_file(f));
	if (IS_ERR(pid))
		return PTR_ERR(pid);

	return pidfd_getfd(pid, fd);
}
