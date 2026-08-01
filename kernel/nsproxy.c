// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 2006 IBM Corporation
 *
 *  Author: Serge Hallyn <serue@us.ibm.com>
 *
 *  Jun 2006 - namespaces support
 *             OpenVZ, SWsoft Inc.
 *             Pavel Emelianov <xemul@openvz.org>
 */

#include <linux/slab.h>
#include <linux/export.h>
#include <linux/nsproxy.h>
#include <linux/ns/ns_common_types.h>
#include <linux/init_task.h>
#include <linux/mnt_namespace.h>
#include <linux/utsname.h>
#include <linux/pid_namespace.h>
#include <net/net_namespace.h>
#include <linux/ipc_namespace.h>
#include <linux/time_namespace.h>
#include <linux/fs_struct.h>
#include <linux/proc_fs.h>
#include <linux/proc_ns.h>
#include <linux/file.h>
#include <linux/syscalls.h>
#include <linux/cgroup.h>
#include <linux/perf_event.h>
#include <linux/nstree.h>

static struct kmem_cache *nsproxy_cachep;

/*
 * 启动任务使用的永久 namespace 指针集合。count 的固定初始引用使该 nsproxy
 * 本身长期存活；uts_ns 槽位指向 init_uts_ns。后续任务若共享全部 namespace，
 * 只增加 nsproxy 引用；任一 namespace 被 clone/unshare/setns 替换时，外层
 * 创建新的 nsproxy 并为每个槽位分别取得引用。
 */
struct nsproxy init_nsproxy = {
	.count			= REFCOUNT_INIT(1),
	.uts_ns			= &init_uts_ns,
#if defined(CONFIG_POSIX_MQUEUE) || defined(CONFIG_SYSVIPC)
	.ipc_ns			= &init_ipc_ns,
#endif
	.mnt_ns			= NULL,
	.pid_ns_for_children	= &init_pid_ns,
#ifdef CONFIG_NET
	.net_ns			= &init_net,
#endif
#ifdef CONFIG_CGROUPS
	.cgroup_ns		= &init_cgroup_ns,
#endif
#ifdef CONFIG_TIME_NS
	.time_ns		= &init_time_ns,
	.time_ns_for_children	= &init_time_ns,
#endif
};

static inline struct nsproxy *create_nsproxy(void)
{
	struct nsproxy *nsproxy;

	nsproxy = kmem_cache_alloc(nsproxy_cachep, GFP_KERNEL);
	if (nsproxy)
		refcount_set(&nsproxy->count, 1);
	return nsproxy;
}

static inline void nsproxy_free(struct nsproxy *ns)
{
	put_mnt_ns(ns->mnt_ns);
	/*
	 * 释放 nsproxy 对 UTS 对象持有的普通引用，最后一份可触发 RCU 销毁。
	 */
	put_uts_ns(ns->uts_ns);
	put_ipc_ns(ns->ipc_ns);
	put_pid_ns(ns->pid_ns_for_children);
	put_time_ns(ns->time_ns);
	put_time_ns(ns->time_ns_for_children);
	put_cgroup_ns(ns->cgroup_ns);
	put_net(ns->net_ns);
	kmem_cache_free(nsproxy_cachep, ns);
}

void deactivate_nsproxy(struct nsproxy *ns)
{
	nsproxy_ns_active_put(ns);
	nsproxy_free(ns);
}

/*
 * Create new nsproxy and all of its the associated namespaces.
 * Return the newly created nsproxy.  Do not attach this to the task,
 * leave it to the caller to do proper locking and attach it to task.
 */
/*
 * 创建新 nsproxy 及其关联的全部 namespace。成功返回新 nsproxy，但不把它
 * 安装到任务；调用者负责采用正确锁协议完成发布。
 *
 * create_new_namespaces() - 按 flags 构造一个事务性的 namespace 集合。
 *
 * @flags:   clone/unshare 请求位；每个 copy_* helper 据此选择共享或克隆。
 * @tsk:     旧 namespace 来源任务，借用且其 nsproxy 在调用期间保持稳定。
 * @user_ns: 新建 namespace 的 owning user namespace，借用且不可为空。
 * @new_fs:  mount namespace 构造使用的 fs_struct，语义由 copy_mnt_ns() 负责。
 *
 * 函数在可睡眠进程上下文依次取得 mount、UTS、IPC、PID、cgroup、net 和 time
 * namespace 引用。成功返回 refcount=1、但尚未发布也尚未取得 active
 * 引用的 nsproxy；失败返回错误指针，并从失败点沿标签逆序 put 已成功的
 * 槽位。这个私有构造阶段保证任何任务都看不到半初始化集合。
 */
static struct nsproxy *create_new_namespaces(u64 flags,
	struct task_struct *tsk, struct user_namespace *user_ns,
	struct fs_struct *new_fs)
{
	struct nsproxy *new_nsp;
	int err;

	new_nsp = create_nsproxy();
	if (!new_nsp)
		return ERR_PTR(-ENOMEM);

	new_nsp->mnt_ns = copy_mnt_ns(flags, tsk->nsproxy->mnt_ns,
				      user_ns, new_fs);
	if (IS_ERR(new_nsp->mnt_ns)) {
		err = PTR_ERR(new_nsp->mnt_ns);
		goto out_ns;
	}

	/*
	 * UTS 阶段位于 mount 之后、IPC 之前。未请求 CLONE_NEWUTS 时取得旧对象
	 * 的新普通引用；请求时克隆一致的 name 快照并计入 owning user 配额。
	 */
	new_nsp->uts_ns = copy_utsname(flags, user_ns, tsk->nsproxy->uts_ns);
	if (IS_ERR(new_nsp->uts_ns)) {
		/* UTS 失败时只有 mount 槽位已成功，out_uts 从那里开始回滚。 */
		err = PTR_ERR(new_nsp->uts_ns);
		goto out_uts;
	}

	new_nsp->ipc_ns = copy_ipcs(flags, user_ns, tsk->nsproxy->ipc_ns);
	if (IS_ERR(new_nsp->ipc_ns)) {
		err = PTR_ERR(new_nsp->ipc_ns);
		goto out_ipc;
	}

	new_nsp->pid_ns_for_children =
		copy_pid_ns(flags, user_ns, tsk->nsproxy->pid_ns_for_children);
	if (IS_ERR(new_nsp->pid_ns_for_children)) {
		err = PTR_ERR(new_nsp->pid_ns_for_children);
		goto out_pid;
	}

	new_nsp->cgroup_ns = copy_cgroup_ns(flags, user_ns,
					    tsk->nsproxy->cgroup_ns);
	if (IS_ERR(new_nsp->cgroup_ns)) {
		err = PTR_ERR(new_nsp->cgroup_ns);
		goto out_cgroup;
	}

	new_nsp->net_ns = copy_net_ns(flags, user_ns, tsk->nsproxy->net_ns);
	if (IS_ERR(new_nsp->net_ns)) {
		err = PTR_ERR(new_nsp->net_ns);
		goto out_net;
	}

	new_nsp->time_ns_for_children = copy_time_ns(flags, user_ns,
					tsk->nsproxy->time_ns_for_children);
	if (IS_ERR(new_nsp->time_ns_for_children)) {
		err = PTR_ERR(new_nsp->time_ns_for_children);
		goto out_time;
	}
	new_nsp->time_ns = get_time_ns(tsk->nsproxy->time_ns);

	return new_nsp;

out_time:
	put_net(new_nsp->net_ns);
out_net:
	put_cgroup_ns(new_nsp->cgroup_ns);
out_cgroup:
	put_pid_ns(new_nsp->pid_ns_for_children);
out_pid:
	put_ipc_ns(new_nsp->ipc_ns);
out_ipc:
	/* IPC 之后的失败必须归还已经由 nsproxy 持有的 UTS 普通引用。 */
	put_uts_ns(new_nsp->uts_ns);
out_uts:
	put_mnt_ns(new_nsp->mnt_ns);
out_ns:
	kmem_cache_free(nsproxy_cachep, new_nsp);
	return ERR_PTR(err);
}

/*
 * called from clone.  This now handles copy for nsproxy and all
 * namespaces therein.
 */
int copy_namespaces(u64 flags, struct task_struct *tsk)
{
	struct nsproxy *old_ns = tsk->nsproxy;
	struct user_namespace *user_ns = task_cred_xxx(tsk, user_ns);
	struct nsproxy *new_ns;

	if (likely(!(flags & (CLONE_NS_ALL & ~CLONE_NEWUSER)))) {
		if ((flags & CLONE_VM) ||
		    likely(old_ns->time_ns_for_children == old_ns->time_ns)) {
			get_nsproxy(old_ns);
			return 0;
		}
	} else if (!ns_capable(user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	/*
	 * CLONE_NEWIPC must detach from the undolist: after switching
	 * to a new ipc namespace, the semaphore arrays from the old
	 * namespace are unreachable.  In clone parlance, CLONE_SYSVSEM
	 * means share undolist with parent, so we must forbid using
	 * it along with CLONE_NEWIPC.
	 */
	if ((flags & (CLONE_NEWIPC | CLONE_SYSVSEM)) ==
		(CLONE_NEWIPC | CLONE_SYSVSEM))
		return -EINVAL;

	new_ns = create_new_namespaces(flags, tsk, user_ns, tsk->fs);
	if (IS_ERR(new_ns))
		return  PTR_ERR(new_ns);

	if ((flags & CLONE_VM) == 0)
		timens_on_fork(new_ns, tsk);

	nsproxy_ns_active_get(new_ns);
	tsk->nsproxy = new_ns;
	return 0;
}

/*
 * Called from unshare. Unshare all the namespaces part of nsproxy.
 * On success, returns the new nsproxy.
 */
int unshare_nsproxy_namespaces(unsigned long unshare_flags,
	struct nsproxy **new_nsp, struct cred *new_cred, struct fs_struct *new_fs)
{
	struct user_namespace *user_ns;
	u64 flags = unshare_flags;
	int err = 0;

	if (!(flags & (CLONE_NS_ALL & ~CLONE_NEWUSER)))
		return 0;

	user_ns = new_cred ? new_cred->user_ns : current_user_ns();
	if (!ns_capable(user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	/*
	 * Convert the 32-bit UNSHARE_EMPTY_MNTNS (which aliases
	 * CLONE_PARENT_SETTID) to the unique 64-bit CLONE_EMPTY_MNTNS.
	 */
	if (flags & UNSHARE_EMPTY_MNTNS) {
		flags &= ~(u64)UNSHARE_EMPTY_MNTNS;
		flags |= CLONE_EMPTY_MNTNS;
	}

	*new_nsp = create_new_namespaces(flags, current, user_ns,
					 new_fs ? new_fs : current->fs);
	if (IS_ERR(*new_nsp)) {
		err = PTR_ERR(*new_nsp);
		goto out;
	}

out:
	return err;
}

/*
 * switch_task_namespaces() - 原子替换任务的整组 namespace 指针。
 *
 * @p:   目标任务，调用者保证 task_struct 存活；通常是 current。
 * @new: 要安装的 nsproxy 持有引用，允许为 NULL 表示任务退出并脱离全部
 *       namespace；成功后该引用由 @p 消费。
 *
 * 函数可睡眠。若 @new 非空，先为其每个 namespace 取得 active 引用，保证
 * 发布给任务时对象在 namespace 树语义上活跃；随后用 task_lock 与远端
 * utsns_get() 等读取者配对，原子交换 p->nsproxy。旧 nsproxy 在解锁后 put，
 * 避免可能的递归回收扩大 task_lock 临界区。返回：无直接返回值；完成后
 * @p 使用 @new，调用者不得再释放已转移的 nsproxy 引用。
 */
void switch_task_namespaces(struct task_struct *p, struct nsproxy *new)
{
	/* ns 暂存旧集合的持有引用，交换完成后在锁外释放。 */
	struct nsproxy *ns;

	might_sleep();

	/* active 引用必须先于任务指针发布，防止观察者看到 inactive 对象。 */
	if (new)
		nsproxy_ns_active_get(new);

	/* task_lock 使远端读取者只能看到完整旧指针或完整新指针。 */
	task_lock(p);
	ns = p->nsproxy;
	p->nsproxy = new;
	task_unlock(p);

	/* 最后一个 nsproxy 引用会对称归还整组 namespace 的 active/普通引用。 */
	if (ns)
		put_nsproxy(ns);
}

void exit_nsproxy_namespaces(struct task_struct *p)
{
	switch_task_namespaces(p, NULL);
}

void switch_cred_namespaces(const struct cred *old, const struct cred *new)
{
	ns_ref_active_get(new->user_ns);
	ns_ref_active_put(old->user_ns);
}

void get_cred_namespaces(struct task_struct *tsk)
{
	ns_ref_active_get(tsk->real_cred->user_ns);
}

void exit_cred_namespaces(struct task_struct *tsk)
{
	ns_ref_active_put(tsk->real_cred->user_ns);
}

int exec_task_namespaces(void)
{
	struct task_struct *tsk = current;
	struct nsproxy *new;

	if (tsk->nsproxy->time_ns_for_children == tsk->nsproxy->time_ns)
		return 0;

	new = create_new_namespaces(0, tsk, current_user_ns(), tsk->fs);
	if (IS_ERR(new))
		return PTR_ERR(new);

	timens_on_fork(new, tsk);
	switch_task_namespaces(tsk, new);
	return 0;
}

static int check_setns_flags(unsigned long flags)
{
	if (!flags || (flags & ~CLONE_NS_ALL))
		return -EINVAL;

#ifndef CONFIG_USER_NS
	if (flags & CLONE_NEWUSER)
		return -EINVAL;
#endif
#ifndef CONFIG_PID_NS
	if (flags & CLONE_NEWPID)
		return -EINVAL;
#endif
#ifndef CONFIG_UTS_NS
	if (flags & CLONE_NEWUTS)
		return -EINVAL;
#endif
#ifndef CONFIG_IPC_NS
	if (flags & CLONE_NEWIPC)
		return -EINVAL;
#endif
#ifndef CONFIG_CGROUPS
	if (flags & CLONE_NEWCGROUP)
		return -EINVAL;
#endif
#ifndef CONFIG_NET_NS
	if (flags & CLONE_NEWNET)
		return -EINVAL;
#endif
#ifndef CONFIG_TIME_NS
	if (flags & CLONE_NEWTIME)
		return -EINVAL;
#endif

	return 0;
}

static void put_nsset(struct nsset *nsset)
{
	unsigned flags = nsset->flags;

	if (flags & CLONE_NEWUSER)
		put_cred(nsset_cred(nsset));
	/*
	 * We only created a temporary copy if we attached to more than just
	 * the mount namespace.
	 */
	if (nsset->fs && (flags & CLONE_NEWNS) && (flags & ~CLONE_NEWNS))
		free_fs_struct(nsset->fs);
	if (nsset->nsproxy)
		nsproxy_free(nsset->nsproxy);
}

static int prepare_nsset(unsigned flags, struct nsset *nsset)
{
	struct task_struct *me = current;

	nsset->nsproxy = create_new_namespaces(0, me, current_user_ns(), me->fs);
	if (IS_ERR(nsset->nsproxy))
		return PTR_ERR(nsset->nsproxy);

	if (flags & CLONE_NEWUSER)
		nsset->cred = prepare_creds();
	else
		nsset->cred = current_cred();
	if (!nsset->cred)
		goto out;

	/* Only create a temporary copy of fs_struct if we really need to. */
	if (flags == CLONE_NEWNS) {
		nsset->fs = me->fs;
	} else if (flags & CLONE_NEWNS) {
		nsset->fs = copy_fs_struct(me->fs);
		if (!nsset->fs)
			goto out;
	}

	nsset->flags = flags;
	return 0;

out:
	put_nsset(nsset);
	return -ENOMEM;
}

static inline int validate_ns(struct nsset *nsset, struct ns_common *ns)
{
	return ns->ops->install(nsset, ns);
}

/*
 * This is the inverse operation to unshare().
 * Ordering is equivalent to the standard ordering used everywhere else
 * during unshare and process creation. The switch to the new set of
 * namespaces occurs at the point of no return after installation of
 * all requested namespaces was successful in commit_nsset().
 */
static int validate_nsset(struct nsset *nsset, struct pid *pid)
{
	int ret = 0;
	unsigned flags = nsset->flags;
	struct user_namespace *user_ns = NULL;
	struct pid_namespace *pid_ns = NULL;
	struct nsproxy *nsp;
	struct task_struct *tsk;

	/* Take a "snapshot" of the target task's namespaces. */
	rcu_read_lock();
	tsk = pid_task(pid, PIDTYPE_PID);
	if (!tsk) {
		rcu_read_unlock();
		return -ESRCH;
	}

	if (!ptrace_may_access(tsk, PTRACE_MODE_READ_REALCREDS)) {
		rcu_read_unlock();
		return -EPERM;
	}

	task_lock(tsk);
	nsp = tsk->nsproxy;
	if (nsp)
		get_nsproxy(nsp);
	task_unlock(tsk);
	if (!nsp) {
		rcu_read_unlock();
		return -ESRCH;
	}

#ifdef CONFIG_PID_NS
	if (flags & CLONE_NEWPID) {
		pid_ns = task_active_pid_ns(tsk);
		if (unlikely(!pid_ns)) {
			rcu_read_unlock();
			ret = -ESRCH;
			goto out;
		}
		get_pid_ns(pid_ns);
	}
#endif

#ifdef CONFIG_USER_NS
	if (flags & CLONE_NEWUSER)
		user_ns = get_user_ns(__task_cred(tsk)->user_ns);
#endif
	rcu_read_unlock();

	/*
	 * Install requested namespaces. The caller will have
	 * verified earlier that the requested namespaces are
	 * supported on this kernel. We don't report errors here
	 * if a namespace is requested that isn't supported.
	 */
	/*
	 * 安装所有请求的 namespace。调用者此前已确认本内核支持相应类型；
	 * 对未构建类型的请求不会在这里再次报告错误。每个 install 回调只
	 * 修改私有 nsset，任一失败都会放弃整项事务而不影响 current。
	 */
#ifdef CONFIG_USER_NS
	if (flags & CLONE_NEWUSER) {
		ret = validate_ns(nsset, &user_ns->ns);
		if (ret)
			goto out;
	}
#endif

	if (flags & CLONE_NEWNS) {
		ret = validate_ns(nsset, from_mnt_ns(nsp->mnt_ns));
		if (ret)
			goto out;
	}

#ifdef CONFIG_UTS_NS
	if (flags & CLONE_NEWUTS) {
		/*
		 * setns 请求 UTS 切换时，经 utsns_operations.install() 校验
		 * 目标 owner 与事务凭据能力，并只改写尚未发布的 nsset->nsproxy
		 * 槽位。
		 */
		ret = validate_ns(nsset, &nsp->uts_ns->ns);
		if (ret)
			goto out;
	}
#endif

#ifdef CONFIG_IPC_NS
	if (flags & CLONE_NEWIPC) {
		ret = validate_ns(nsset, &nsp->ipc_ns->ns);
		if (ret)
			goto out;
	}
#endif

#ifdef CONFIG_PID_NS
	if (flags & CLONE_NEWPID) {
		ret = validate_ns(nsset, &pid_ns->ns);
		if (ret)
			goto out;
	}
#endif

#ifdef CONFIG_CGROUPS
	if (flags & CLONE_NEWCGROUP) {
		ret = validate_ns(nsset, &nsp->cgroup_ns->ns);
		if (ret)
			goto out;
	}
#endif

#ifdef CONFIG_NET_NS
	if (flags & CLONE_NEWNET) {
		ret = validate_ns(nsset, &nsp->net_ns->ns);
		if (ret)
			goto out;
	}
#endif

#ifdef CONFIG_TIME_NS
	if (flags & CLONE_NEWTIME) {
		ret = validate_ns(nsset, &nsp->time_ns->ns);
		if (ret)
			goto out;
	}
#endif

out:
	if (pid_ns)
		put_pid_ns(pid_ns);
	if (nsp)
		put_nsproxy(nsp);
	put_user_ns(user_ns);

	return ret;
}

/*
 * This is the point of no return. There are just a few namespaces
 * that do some actual work here and it's sufficiently minimal that
 * a separate ns_common operation seems unnecessary for now.
 * Unshare is doing the same thing. If we'll end up needing to do
 * more in a given namespace or a helper here is ultimately not
 * exported anymore a simple commit handler for each namespace
 * should be added to ns_common.
 */
/*
 * 这里是 setns 事务的不可回滚点。目前只有少数 namespace 在提交时需要额外
 * 工作，内容足够少，因此没有为 ns_common 增加独立 commit 回调；unshare
 * 使用相同思路。若未来某类提交逻辑增长，或这里使用的 helper 不再
 * 导出，应为每类 namespace 增加明确的 commit handler。
 *
 * commit_nsset() - 一次性发布已经全部验证成功的 namespace 集合。
 *
 * @nsset 是 prepare/validate 阶段构造的事务对象，借用且不可为空。函数只在
 * 所有 install 回调成功后运行，可睡眠且无错误返回：依次提交可选凭据、
 * mount 根/工作目录、IPC/time 附带状态，最后通过 switch_task_namespaces()
 * 发布 nsproxy。成功后 nsproxy 所有权转给 current，并把槽位置 NULL，防止
 * put_nsset() 在清理事务外壳时重复释放。UTS 没有额外 commit 动作，因为
 * utsns_install() 已在私有 nsproxy 中完成引用替换。
 */
static void commit_nsset(struct nsset *nsset)
{
	unsigned flags = nsset->flags;
	struct task_struct *me = current;

#ifdef CONFIG_USER_NS
	if (flags & CLONE_NEWUSER) {
		/* transfer ownership */
		/*
		 * commit_creds() 消费准备好的凭据；清空指针避免事务清理
		 * 重复 put。
		 */
		commit_creds(nsset_cred(nsset));
		nsset->cred = NULL;
	}
#endif

	/* We only need to commit if we have used a temporary fs_struct. */
	/*
	 * 只有组合切换曾创建临时 fs_struct；纯 mount setns 直接使用 current->fs。
	 */
	if ((flags & CLONE_NEWNS) && (flags & ~CLONE_NEWNS)) {
		set_fs_root(me->fs, &nsset->fs->root);
		set_fs_pwd(me->fs, &nsset->fs->pwd);
	}

#ifdef CONFIG_IPC_NS
	if (flags & CLONE_NEWIPC)
		exit_sem(me);
#endif

#ifdef CONFIG_TIME_NS
	if (flags & CLONE_NEWTIME)
		timens_commit(me, nsset->nsproxy->time_ns);
#endif

	/* transfer ownership */
	/*
	 * 把 nsproxy 持有引用与全部槽位转移给 current；其中包含已验证的 UTS
	 * 引用。置 NULL 是事务所有权转移的显式标记。
	 */
	switch_task_namespaces(me, nsset->nsproxy);
	nsset->nsproxy = NULL;
}

SYSCALL_DEFINE2(setns, int, fd, int, flags)
{
	CLASS(fd, f)(fd);
	struct ns_common *ns = NULL;
	struct nsset nsset = {};
	int err = 0;

	if (fd_empty(f))
		return -EBADF;

	if (proc_ns_file(fd_file(f))) {
		ns = get_proc_ns(file_inode(fd_file(f)));
		if (flags && (ns->ns_type != flags))
			err = -EINVAL;
		flags = ns->ns_type;
	} else if (!IS_ERR(pidfd_pid(fd_file(f)))) {
		err = check_setns_flags(flags);
	} else {
		err = -EINVAL;
	}
	if (err)
		goto out;

	err = prepare_nsset(flags, &nsset);
	if (err)
		goto out;

	if (proc_ns_file(fd_file(f)))
		err = validate_ns(&nsset, ns);
	else
		err = validate_nsset(&nsset, pidfd_pid(fd_file(f)));
	if (!err) {
		commit_nsset(&nsset);
		perf_event_namespaces(current);
	}
	put_nsset(&nsset);
out:
	return err;
}

int __init nsproxy_cache_init(void)
{
	nsproxy_cachep = KMEM_CACHE(nsproxy, SLAB_PANIC|SLAB_ACCOUNT);
	return 0;
}
