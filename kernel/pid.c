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

/*
 * 原始英文总览的当前版本解读：struct pid 是共享同一类数字身份的 task 所挂靠的
 * 后端对象，早期实现把数字散列并用 bitmap page 管理可用空间；原文还强调分配
 * 近似无锁、最坏扫描 32 个表项和一页、释放 O(1)。当前文件已经改用每个
 * pid_namespace 的 IDR，并由 pidmap_lock 串行化分配、发布与删除，因此这些
 * bitmap/lockless/复杂度描述只保留为历史材料，不能当作当前实现事实。
 *
 * 当前并发边界分为三层：pidmap_lock 保护 namespace IDR 与 pid_allocated；
 * tasklist_lock 写侧保护 task 与 pid->tasks[] 的绑定变化；RCU 允许查找者在对象
 * 从索引/链表摘除后短暂继续访问内存。struct pid 引用计数只保证生命周期，不
 * 冻结链表内容。PID namespace 的 numbers[] 从最外层 0 到最内层 level 保存同一
 * 身份的多层数字，因而一个稳定对象可同时回答容器内外的可见 PID。
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

/*
 * init_struct_pid 是 boot task 使用的静态 PID 后端对象，编译期创建且永不走普通
 * alloc_pid() 分配。count 初始 1 提供永久基础引用；各 PIDTYPE 链表起初为空，
 * level=0 且 numbers[0]={nr=0, ns=&init_pid_ns} 表示 idle/init_task 的特殊 0 号
 * 身份。启动代码及 init_task 借用它，静态存储期决定最终生命周期；字段绑定
 * 发布后仍遵循 tasklist_lock/RCU 规则。
 */
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
/*
 * init_pid_ns 是根 PID namespace 的静态实例：ns 提供 namespacefs 身份，idr
 * 索引根空间 PID，pid_allocated 带 PIDNS_ADDING 状态位，level=0 表示没有父层，
 * child_reaper 指向 init_task，user_ns 决定权限映射，pid_max 是运行期分配上限。
 * CONFIG_SYSCTL && CONFIG_MEMFD_CREATE 时还保存本 namespace 的 memfd noexec
 * 策略。对象在启动期由 pid_idr_init()/register_pidns_sysctls() 补全，作为所有
 * 后代 PID namespace 的根长期存活；字段分别由 pidmap_lock、sysctl 生命周期及
 * 各自子系统协议保护，不能把整个结构视为由一把锁统一冻结。
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
/*
 * 补充说明：主要调用者是持有型查找、pidfd 和失败清理路径；它与 get_pid() 成对，
 * 位于“索引/关系已处理 → 最后一份身份引用释放”的生命周期末端。函数不要求
 * tasklist_lock/pidmap_lock，不能假设 @pid 字段静止；路径本身不睡眠，RCU 回调
 * 也会调用它。最后引用执行 pidfs 非阻塞清理、slab 回收和 put_pid_ns()。局部
 * @ns 是从最内层 upid 借用的 namespace 指针，在释放 pid slab 前有效；引用未
 * 归零时除计数外无可观察副作用，归零时 @pid 此后不可再使用。
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
/*
 * 补充说明：本函数只由 call_rcu() 在 RCU callback 上下文调用，不能睡眠。@rhp
 * 不可为 NULL，且 ownership 已从 free_pid() 转交 RCU；container_of() 取得的是
 * 嵌入对象，不增加引用。返回无直接值，调用 put_pid() 消耗 free_pid() 保留的
 * 基础引用；若这是最后一份，pid 内存与 namespace 引用在本回调中结束。
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
/*
 * 补充说明：调用链通常为 detach/change 收集最后绑定 → 释放 tasklist_lock →
 * free_pid()。@pid 是输入且本函数接管“从所有 namespace 数字索引撤销并最终
 * put”的责任；调用后调用者不得再依赖该基础引用。局部 @active_ns 是最内层
 * namespace，@i 逐层遍历，@upid/@ns 是锁内借用指针。函数不能持 tasklist_lock，
 * 自身不进行调度睡眠；返回无直接值。发布的逆过程是先在 pidmap_lock 下删除
 * 所有 IDR 槽并更新 pid_allocated，再移除 pidfs 表示，最后 call_rcu() 延迟释放。
 * wake_up_process() 只通知 namespace reaper，并不转移其 task 引用。
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
/*
 * 补充说明：@pids 是调用者持有的 PIDTYPE_MAX 元素输入数组，本函数不修改槽位，
 * 但对每个非 NULL 元素执行 free_pid()，因此消耗对应的释放责任；调用者返回后
 * 不得再次 free。入口不得持 tasklist_lock，函数不睡眠、无错误返回。@tmp 从高
 * PIDTYPE 向低类型扫描只是确定批次顺序，当前实现仍逐个取得 pidmap_lock。
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
/*
 * 补充说明：主要由 copy_process() 在 task 尚未可运行时调用；成功后调用者持有
 * 新 pid 的初始引用，并继续把它安装到 task/PIDTYPE 关系，失败时没有可见对象
 * 留给调用者。@ns 是借用输入且不可为 NULL，函数内部 get_pid_ns() 把其生命期
 * 延长到 pid 最终释放；@arg_set_tid 是可空、只读借用数组，长度以元素个数计，
 * 不转移 ownership。函数运行在可睡眠进程上下文：锁外 slab/preload/pidfs
 * 准备可分配内存，pidmap_lock 内只能用 GFP_ATOMIC。
 *
 * 返回 ERR_PTR(-EINVAL/-EPERM/-ENOMEM/-EAGAIN/-EEXIST 等当前分支错误) 或带一份
 * 引用的 struct pid。set_tid/pid_max 是按“最内层到最外层”排列的栈上快照；
 * @tmp/@upid 只在相应 namespace/数组生命期内借用；@i/@nr 标记当前层与分配号；
 * @retval 保存统一失败码；@retried_preload 防止 -ENOMEM 无限重试。发布边界是
 * idr_replace(NULL→pid)，其前错误只回滚预留槽，其后 pidfs 失败必须 free_pid()
 * 完成公开对象的摘除、RCU 延迟释放和 namespace active 引用配平。
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
	/*
	 * 原英文的完整约束：child reaper 已退出、namespace 拒绝新进程时，ENOMEM
	 * 并非最贴切错误，但它已成为文档化用户 ABI，不能轻易更换。检查不能提前，
	 * 否则会遮蔽前面更具体的参数/权限/分配错误。即使 copy_process() 也检查，
	 * 此处仍不可省：多个父 namespace task 并发向死亡 pidns 注入 child 时，其中
	 * 一个 copy_process() 错误回滚的 free_pid() 可能去唤醒已经释放的
	 * ns->child_reaper；在 pidmap_lock 下复核关闭状态可封住该竞态。
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
/*
 * 补充说明：PID namespace teardown 在阻止新进程创建时调用。@ns 是不可空的
 * 输入/输出借用对象，调用者保证其存活；本函数不取得引用、不睡眠、无返回。
 * pidmap_lock 与 alloc_pid()/free_pid() 的状态读写竞争，清除 PIDNS_ADDING 是
 * 不可逆的关闭发布点，但不会移除已经分配的 PID，后续由退出路径逐个回收。
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
/*
 * 补充说明：主要调用者是 find_vpid()、proc 遍历和按 namespace 的 task 查找。
 * @nr 是 @ns 层内的正 PID 数字，@ns 为不可空借用且调用期间必须存活。函数不
 * 睡眠、无副作用；IDR 读由外层 RCU/tasklist_lock 提供生命周期与关系保护。
 * NULL 同时表示数字不存在或分配器仍以 NULL 预留尚未发布的槽位。
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
/*
 * 补充说明：current 决定查找视图，task_active_pid_ns(current) 返回借用 namespace；
 * 调用者仍必须持 RCU/tasklist_lock，并在跨临界区使用前 get_pid()。函数不睡眠、
 * 不修改状态；找不到或尚未发布返回 NULL，成功后通常继续 pid_task()/get_pid()。
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
/*
 * 补充说明：这是内部字段分派器，主要供 attach/change/get_task_pid 等关系路径
 * 使用。@task 是不可空借用；@type 必须落在有效 enum pid_type 范围，函数不做
 * 边界检查。返回的是可写“指针槽地址”，不是持有的 struct pid 引用；其有效期
 * 受 task/signal 生命周期及 tasklist_lock/RCU 规则限制。函数不睡眠、无直接
 * 状态副作用，调用者随后才决定读取或发布新指针。
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
/*
 * 补充说明：copy_process()/关系切换路径在 task 的 pid 指针已设置后调用本函数。
 * @task 是输入/输出借用且不可空，@type 是有效 PIDTYPE；二者 ownership 不变。
 * 入口必须写持 tasklist_lock，不能睡眠。返回无直接值；hlist_add_head_rcu() 是
 * 关系发布点，使 RCU 读者可从 pid->tasks[type] 找到 task。局部 @pid 只是从 task
 * 槽读取的借用指针，调用者必须保证非 NULL及对象生命周期。
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
/*
 * 补充说明：@pids 是调用者提供的 PIDTYPE_MAX 输出数组，入口相应槽应为 NULL；
 * 当旧 pid 的所有类型链表都空时，本函数把释放责任写入 pids[type]，真正
 * free_pid() 必须在释放 tasklist_lock 后执行。@new 是可空借用的新 pid，本函数
 * 只写 task 槽，不把 task 挂入新链表（由 change_pid() 后续 attach）；因此这是
 * 一个内部两阶段关系变更。入口写持 tasklist_lock、不可睡眠、无直接返回。
 * @pid_ptr/@pid 是旧槽地址和旧对象借用，@tmp 扫描所有类型验证是否仍被使用。
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
/*
 * 补充说明：调用者写持 tasklist_lock，@pids 为输出数组，@task 为输入/输出借用，
 * @type 为有效类型。函数不睡眠、无直接返回；它从旧 RCU 链表摘除 task、把 task
 * 槽清为 NULL，并在旧 pid 完全无绑定时向 @pids 转移后续释放责任。返回后通常
 * 先完成其他关系修改并解锁，再 free_pids()。
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
/*
 * 补充说明：@pid 是不可空借用的新身份对象，其基础引用由更高层关系生命周期
 * 管理；本函数不单独 get_pid()。入口写持 tasklist_lock、不可睡眠、无返回。
 * 先 __change_pid() 摘除旧关系并写新槽，再 attach_pid() 发布新反向链表；锁使
 * RCU 读者只看到协议允许的过渡。旧 pid 若失去最后绑定，其释放责任写入 @pids。
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
/*
 * 补充说明：主要由 exec 的 de_thread() 在非 leader 接管线程组身份时调用。
 * @left/@right 是不可空的输入/输出借用 task，入口写持 tasklist_lock，二者的
 * thread_pid 必须有效且 PIDTYPE_PID 链表符合单 task 不变量。函数不睡眠、无
 * 返回、不改变引用计数；局部 pid1/pid2/head1/head2 都是锁内借用。链表头交换、
 * RCU 指针发布和数值缓存更新必须作为同一写锁事务，避免查找关系与 task->pid
 * 不一致；返回后调用者继续完成 leader/线程组的其余状态转换。
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
/*
 * 补充说明：这是 exec/任务替换场景中“新 task 接管旧 task 的共享身份”路径。
 * @old/@new 为不可空输入/输出借用 task，@type 必须是 TGID/PGID/SID 等非 PID
 * 类型。入口写持 tasklist_lock、不可睡眠、无返回；hlist_replace_rcu() 原地替换
 * 节点，保持链表位置和 struct pid 引用关系不变，比 detach+attach 少一次拆装。
 * 调用者负责事先让 new 的对应 pid 槽指向同一对象，并继续处理旧 task 生命周期。
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
/*
 * 补充说明：调用链通常是持 RCU/tasklist_lock 的查找者 → pid_task() → 可选的
 * get_task_struct()。@pid 可空且为借用，@type 必须有效。函数不睡眠、不修改
 * 链表；局部 @first 是 RCU 链节点借用，@result 只在外层保护范围内有效。返回
 * NULL 表示无对象/空链，成功不承诺具体成员在锁外继续存活，也不保证共享类型
 * 的“第一个”具有稳定顺序语义。
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
/*
 * 补充说明：英文入口要求是硬契约。@nr 是 @ns 中的 PID 数字，@ns 为不可空借用；
 * 调用者已持 rcu_read_lock()，函数用 lockdep 告警验证但不会替调用者加锁。它不
 * 睡眠、无副作用，返回 PIDTYPE_PID 链表中的借用 task 或 NULL；离开 RCU 前若
 * 需长期使用必须 get_task_struct()。通常由 find_task_by_vpid() 或 namespace
 * 定向查找继续消费。
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
/*
 * 补充说明：@vnr 是 current active PID namespace 中的数字，调用者必须已持 RCU
 * 读锁；函数不自行验证、不睡眠、不修改状态。返回借用 task 只在临界区内有效，
 * 下一步若要跨越 task 退出应改用 find_get_task_by_vpid() 或显式取引用。
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
/*
 * 补充说明：@nr 是 current namespace 中的输入 PID 数字。函数可在不持 RCU 的
 * 普通调用点使用，自行建立读侧临界区且不睡眠；成功在解锁前 get_task_struct()
 * 把借用指针转换为持有引用，失败返回 NULL。局部 @task 在 RCU 内先借用、成功后
 * 由返回值向调用者转移一份释放责任；数字复用只影响查找时刻，不会改写已取得
 * task 引用指向的对象。
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
/*
 * 补充说明：@task 是调用期间必须存活的不可空借用对象，@type 为有效 PIDTYPE。
 * 函数不睡眠，自行用 RCU 稳定 task 槽，并以 get_pid() 把可能为空的借用转换成
 * 持有引用。局部 @pid 成功返回后 ownership 交给调用者并须 put_pid()；NULL
 * 表示 task 当前没有该类身份。引用只 pin 身份后端，关系链和 task 生命周期仍
 * 可变化，调用者下一步通常用 pid_nr_ns() 或 pidfd 相关操作。
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
/*
 * 补充说明：@pid 是调用期间存活的可空借用（NULL 会得到 NULL），@type 为有效
 * PIDTYPE。函数不睡眠，自行持 RCU，先借用 pid_task() 结果再在临界区内增加
 * task 引用。成功把一份 task ownership 交给调用者；若链表空/目标已摘除返回
 * NULL。共享类型只选择链表第一个成员，不能把它当作枚举全部任务的接口。
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
/*
 * 补充说明：@nr 是 current active namespace 的瞬时 PID 数字。函数不睡眠，
 * 自行持 RCU 并在临界区内 get_pid()，成功把一份 struct pid 引用转移给调用者；
 * 查找不到/预留未发布返回 NULL。局部 @pid 解锁后仅因这份引用而存活，但它与
 * task 的绑定仍可消失；通常用于 pidfd_open、sysctl 转换等稳定身份入口。
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
/*
 * 补充说明：@pid/@ns 都是可空借用输入，调用者负责保证非空对象的生命周期；
 * 函数不加引用、不睡眠、无副作用。@ns 只能看到自身或后代 pid 的对应层：先按
 * level 定位 @upid，再验证 upid->ns 身份相同，防止同层不同 namespace 混淆。
 * 局部 @nr 初值 0 是不可见哨兵。返回数字可立即复用，跨时间身份应持 struct pid。
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
/*
 * 补充说明：@pid 是可空借用输入，current 提供 namespace 视图；调用者保证对象
 * 存活。函数不睡眠、无副作用，直接包装 pid_nr_ns()。返回值是瞬时 pid_t，常
 * 用于向当前进程展示/proc/sysctl，不附带引用或长期唯一性。
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
/*
 * 补充说明：@task 是不可空借用，@type 有效；@ns 可空，NULL 明确选择 current
 * active namespace，而不是 task 自己的视图。函数不睡眠，自行用 RCU 稳定 pid
 * 槽；局部 @nr 是返回快照。成功/不可见均不取得引用或改状态，返回后下一步
 * 通常仅做展示、比较或 ABI 输出，不能保存作永久句柄。
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
/*
 * 补充说明：@tsk 是调用期间存活的不可空借用 task；函数不睡眠、无副作用，
 * 通过 task_pid(tsk) 的线程 PID 后端取得最内层 namespace 借用指针，不增加
 * namespace 引用。调用者若跨越 task/pid 生命周期保存结果必须另取 namespace
 * 引用；current 的虚拟 PID 查找以此 helper 确定视图。
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
/*
 * 补充说明：英文定义的精确语义是返回第一个数字大于等于 @nr 的 pid；若 @nr
 * 本身存在则与 find_pid_ns() 相同。@nr 是按值输入，idr_get_next() 对其内部
 * 游标更新不会反馈调用者；@ns 是不可空借用。函数不睡眠、无副作用，返回的
 * struct pid 不带引用，只能在 RCU/tasklist_lock 保护下使用；主要供 /proc
 * 顺序枚举，NULL 表示从起点到当前 IDR 末尾都没有已发布对象。
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
/*
 * 补充说明：@fd 是当前进程 fdtable 的无符号描述符编号；@flags 是不可空输出
 * 指针，只有成功时写入底层 file->f_flags，失败保持未指定。CLASS(fd, f) 在
 * 作用域退出时自动 fdput()，局部 @pid 从 file 借用后通过 get_pid() 转为持有。
 * 函数可执行 fd 查找但不睡眠；返回 -EBADF 表示槽无 file，pidfd_pid() 还可返回
 * 非 pidfd 的错误指针。成功把一份 pid 引用转移给调用者，须 put_pid()。
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
/*
 * 补充说明：@pidfd 可为普通 fd、PIDFD_SELF_THREAD 或
 * PIDFD_SELF_THREAD_GROUP；@flags 是不可空输出，仅成功写入，self 特殊值写 0。
 * 函数不要求外层 RCU，当前 helper 不睡眠。局部 @f_flags 保存普通 pidfd file
 * flags，@type 决定线程/线程组语义，@pid 在各分支取得持有引用并在转换后统一
 * put。成功返回带引用 task，调用者负责 put_task_struct()；错误指针包括 fd/
 * pidfd 验证错误和目标已无 task 的 -ESRCH。
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
/*
 * 补充说明：@pid 是调用期间存活的借用身份对象，@flags 只允许 pidfd_prepare()
 * 支持的位；本函数不消费调用者引用。运行在可睡眠进程上下文，因为 prepare
 * 可分配 file/fd。局部 @pidfd 是预留描述符或负 errno，@pidfd_file 在成功时
 * 由 prepare 交给本函数；fd_install() 是不可失败的发布边界并把 file ownership
 * 转给 fdtable。prepare 失败返回负 errno 且没有待安装 file，成功返回 cloexec fd。
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
/*
 * 补充说明：系统调用处于可睡眠进程上下文。@pid 是 current namespace 中的正
 * PID 数字；@flags 仅允许 PIDFD_NONBLOCK/PIDFD_THREAD，未知位返回 -EINVAL。
 * 局部 @p 由 find_get_pid() 持有，保证 pidfd_create() 期间身份对象不释放，
 * 无论成功失败都 put_pid()；@fd 保存创建结果。返回新 cloexec fd，或
 * -EINVAL/-ESRCH 及 prepare 路径错误；成功后 fdtable 拥有 pidfd file。
 * 原英文还规定未设置 PIDFD_THREAD 时目标必须是线程组 leader，该验证由下游
 * pidfd_prepare()/pidfd 语义完成，不应误以为本 wrapper 的数字查找已完成全部检查。
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
/*
 * 补充说明：sysctl 核心通过 pid_table_root.lookup 调用本函数；@root 是借用输入，
 * 当前实现无需读取。函数不睡眠、无副作用，返回 current active pidns 内嵌 set
 * 的借用指针，不增加 namespace 引用；调用框架负责在访问期间稳定 current/ns。
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
/*
 * 补充说明：@set 是 sysctl 核心借用的候选集合，可为空与否由框架契约保证；函数
 * 只做地址身份比较，不睡眠、不取引用。true 表示候选正是 current active pidns
 * 的集合，false 阻止该 namespace 视图看到其他 pidns 的表项。
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
/*
 * 补充说明：sysctl inode 权限查询调用本回调。@head/@table 均为不可空只读借用，
 * 分别确定所属 pidns 和原始 mode。函数可执行凭据/组查询但不睡眠、不改表项；
 * 局部 @pidns 由 container_of(head->set) 借用，@mode 先复制再根据 user namespace
 * 中 CAP_SYS_ADMIN、映射 root uid、root gid 或 other 三类选择一组三位权限，最后
 * 复制到 u/g/o 位返回。无错误码、无 ownership 转移。
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
/*
 * 补充说明：@head 是不可空借用 sysctl header；@uid/@gid 是不可空输入/输出槽，
 * 仅当 pidns user namespace 的 0 号 ID 能映射到宿主 kuid/kgid 时覆盖，否则保留
 * 调用者默认值。函数不睡眠、无直接返回、不取得 namespace 引用。局部 pidns
 * 从内嵌 set 恢复，两个 ns_root_* 只是本次映射快照。
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
/*
 * pid_table_root 是 CONFIG_SYSCTL 下的静态策略对象，由 sysctl 核心长期借用且不
 * 动态释放；lookup 按 current 选择 set，permissions/set_ownership 把 userns
 * 语义投影到 proc sysctl inode。字段均为只读回调表，初始化后不需要额外锁。
 */
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
/*
 * 补充说明：@table 为只读借用表项；@write 选择方向；@buffer 是用户数据窗口；
 * @lenp/@ppos 为不可空输入/输出，分别携带剩余长度和文件偏移，ownership 均不变。
 * 函数在 sysctl 进程上下文中可睡眠/访问用户内存。局部 @tmp_table 是栈上表项
 * 副本，data 改指向 @tmp_pid，避免通用 handler 直接接触 struct pid；@new_pid
 * 写路径成功后持有引用，经 xchg 原子转移给全局 cad_pid，旧引用由 put_pid()
 * 释放。返回 0、proc_dointvec() 错误或数字找不到的 -ESRCH；读路径不改全局状态。
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

/*
 * pid_table 是每个 PID namespace sysctl 表的只读模板。首项 pid_max 用 init_pid_ns
 * 字段占位，注册其他 pidns 时 kmemdup() 后把 data 重定向到对应 pidns->pid_max；
 * extra1/extra2 借用全局上下限。CONFIG_PROC_SYSCTL 时第二项 cad_pid 通过自定义
 * handler 在 pid_t 与带引用 struct pid 之间转换。模板静态存活、不直接注销；
 * 每 namespace 副本的 ownership 由 register/unregister 成对管理。
 */
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
/*
 * 补充说明：@pidns 是不可空输入/输出借用对象，调用者保证 namespace 生命周期。
 * CONFIG_SYSCTL=y 时函数在可睡眠进程/创建上下文中分配表副本并注册；局部 @tbl
 * 成功后 ownership 转给 sysctl registration，并由 pidns->sysctls 间接保存；若
 * 实际注册失败，本函数 kfree 表副本并 retire set，kmemdup 失败则直接返回。
 * 成功还按 possible CPU 数抬高 pid_max。返回 0 或 -ENOMEM；CONFIG_SYSCTL=n 时
 * 参数无可观察修改且恒为 0。销毁时必须 unregister_pidns_sysctls()。
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
/*
 * 补充说明：@pidns 是已成功注册 sysctl 的不可空输入/输出借用对象。函数运行在
 * 可睡眠 teardown 上下文，无直接返回；CONFIG_SYSCTL=y 时局部 @tbl 从 header
 * 借用注册参数，先注销可见表、退休 set，最后释放副本，调用后 sysctls 指针不应
 * 再被使用。CONFIG_SYSCTL=n 时为空操作。调用者仍拥有 pidns 本体。
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
/*
 * 补充说明：入参无；由内核早期初始化在并发 fork 发生前调用，可执行 slab 创建，
 * 但失败由 SLAB_PANIC 终止启动而非返回 errno。函数无直接返回；编译期断言验证
 * PIDNS_ADDING 状态位不会与合法 pid_max 冲突，随后按 CPU 数确定默认/最小上限，
 * 初始化根 IDR，并创建只容纳一层 numbers[] 的根 pid cache。对 init_pid_ns、
 * pid_max_min 的写入是启动期一次性发布，后续分配/sysctl 路径开始读取。
 */
void __init pid_idr_init(void)
{
	/* Verify no one has done anything silly: */
	/* 英文说明：用编译期检查阻止不合理的常量布局进入可运行内核。 */
	/*
	 * 中文补充：PID_MAX_LIMIT 不能碰到 PIDNS_ADDING 哨兵范围。
	 * 否则普通计数和 namespace 状态位会混淆。
	 */
	BUILD_BUG_ON(PID_MAX_LIMIT >= PIDNS_ADDING);

	/* bump default and minimum pid_max based on number of cpus */
	/* 英文说明：依据 CPU 数量提高默认及最小 pid_max，降低并发系统数字回绕。 */
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
/*
 * 补充说明：无参数；由 subsys_initcall 在 sysctl 核心的 "kernel" 目录已建立后
 * 调用，允许注册过程分配内存。CONFIG_SYSCTL=y 时失败被 BUG_ON 视为启动期不可
 * 恢复错误；关闭时无副作用。返回恒为 0，成功后 init_pid_ns 持有注册表直到
 * 系统生命周期结束。subsys_initcall 宏把函数地址发布到对应 init section。
 */
static __init int pid_namespace_sysctl_init(void)
{
#ifdef CONFIG_SYSCTL
	/* "kernel" directory will have already been initialized. */
	/* 英文说明：父目录已由 sysctl 更早初始化，本函数只添加 PID 叶子项。 */
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
/*
 * 补充说明：@task 是已由调用者持有引用的不可空借用目标，@fd 是其 fdtable 中的
 * 非负编号。函数处于可睡眠进程上下文，down_read_killable() 可因信号返回错误；
 * exec_update_lock 读侧把 ptrace 权限检查、PF_EXITING 检查和 fget_task() 与 exec
 * 凭据/文件表更新串行化。局部 @file 成功时带一份引用并把 fput 责任转给调用者，
 * @ret 保存加锁错误。返回 ERR_PTR(-ERESTARTSYS/-EPERM/-ESRCH/-EBADF 等) 或 file。
 * 解锁后再次观察 PF_EXITING 只用于把“files 已释放”的空结果修正为 -ESRCH。
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
		/*
		 * 原英文列出退出竞态的三个时刻：exit_signals() 前和 exit_files() 取得
		 * task_lock 前仍可得到真实 fd；exit_files() 解锁后 files=NULL 且已设置
		 * PF_EXITING。第三种底层看似 EBADF，实质是 task 正在退出并已释放 files，
		 * 所以改报 ESRCH；只有非退出任务的空槽才保留 EBADF。
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
/*
 * 补充说明：@pid 是调用期间存活的借用身份，@fd 是目标 task 的描述符编号。
 * 函数在可睡眠进程上下文运行：先 get_pid_task(PIDTYPE_PID) 取得 task 引用，交给
 * __pidfd_fget() 取得 file 引用，再及时 put task；receive_fd() 可能分配当前
 * fd 并在成功时向 fdtable 发布同一 file，随后本地 fput() 配平。局部 @ret 保存
 * 新 fd/errno。返回 -ESRCH、权限/目标 fd 错误、分配错误或 cloexec 新 fd；成功
 * 不影响目标进程原 fd 和 file ownership。
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
/*
 * 补充说明：@pidfd 是 current fdtable 中的 pidfd，@fd 是目标进程 fd 编号，
 * @flags 当前保留且必须为 0。系统调用可睡眠。CLASS(fd, f) 对 pidfd file 建立
 * scope-bound 借用并在退出时自动 fdput；局部 @pid 由 pidfd file 借用，file
 * 引用保证调用 pidfd_getfd() 期间它有效，无需额外 get_pid。成功返回当前进程
 * 新 cloexec fd；失败返回 -EINVAL/-EBADF、非 pidfd 错误、-ESRCH/-EPERM 或 fd
 * 复制错误。英文强调目标进程本身不受复制影响，权限使用 ptrace real creds。
 */
SYSCALL_DEFINE3(pidfd_getfd, int, pidfd, int, fd,
		unsigned int, flags)
{
	struct pid *pid;

	/* flags is currently unused - make sure it's unset */
	/* 英文说明：flags 尚未使用，必须验证为 0，而不是静默忽略未知未来语义。 */
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
