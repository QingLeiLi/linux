/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 任务 namespace 集合与切换事务接口
 *
 * task_struct 不直接保存每类 namespace，而是通过 nsproxy 聚合一组持有
 * 引用。共享全部 namespace 的线程可共享同一 nsproxy；任一槽位需要隔离或
 * setns 替换时，先构造私有副本，全部验证成功后再原子切换 task->nsproxy。
 */
#ifndef _LINUX_NSPROXY_H
#define _LINUX_NSPROXY_H

#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <linux/sched.h>

struct mnt_namespace;
struct uts_namespace;
struct ipc_namespace;
struct pid_namespace;
struct cgroup_namespace;
struct fs_struct;

/*
 * A structure to contain pointers to all per-process
 * namespaces - fs (mount), uts, network, sysvipc, etc.
 *
 * The pid namespace is an exception -- it's accessed using
 * task_active_pid_ns.  The pid namespace here is the
 * namespace that children will use.
 *
 * 'count' is the number of tasks holding a reference.
 * The count for each namespace, then, will be the number
 * of nsproxies pointing to it, not the number of tasks.
 *
 * The nsproxy is shared by tasks which share all namespaces.
 * As soon as a single namespace is cloned or unshared, the
 * nsproxy is copied.
 */
/*
 * nsproxy 聚合每个进程的 mount、UTS、network、SysV IPC 等 namespace 指针。
 * PID 是例外：当前活动 PID namespace 通过 task_active_pid_ns() 查询，
 * 此处保存的是子进程将使用的 PID namespace。
 *
 * count 统计持有该 nsproxy 的任务/临时事务引用；各具体 namespace 的普通
 * 引用数统计指向它的 nsproxy 数，而不是共享这些 nsproxy 的任务总数。
 * 完全共享 namespace 集合的任务共用一个 nsproxy；只要 clone/unshare/setns
 * 改变任一槽位，就复制整个 nsproxy，以私有副本完成事务性替换。
 *
 * @count: nsproxy 自身引用；最后一次 put 归还整组 active 和普通引用。
 * @uts_ns: 当前 UTS namespace 持有引用，决定 uname/hostname 视图。
 * @ipc_ns/@mnt_ns/@net_ns/@cgroup_ns: 对应子系统 namespace 持有引用。
 * @pid_ns_for_children: 未来 child 使用的 PID namespace 持有引用。
 * @time_ns: 当前时间视图；@time_ns_for_children 是未来 child 的时间视图。
 */
struct nsproxy {
	refcount_t count;
	struct uts_namespace *uts_ns;
	struct ipc_namespace *ipc_ns;
	struct mnt_namespace *mnt_ns;
	struct pid_namespace *pid_ns_for_children;
	struct net 	     *net_ns;
	struct time_namespace *time_ns;
	struct time_namespace *time_ns_for_children;
	struct cgroup_namespace *cgroup_ns;
};
extern struct nsproxy init_nsproxy;

/*
 * A structure to encompass all bits needed to install
 * a partial or complete new set of namespaces.
 *
 * If a new user namespace is requested cred will
 * point to a modifiable set of credentials. If a pointer
 * to a modifiable set is needed nsset_cred() must be
 * used and tested.
 */
/*
 * nsset 汇集安装部分或全部新 namespace 所需的事务状态。
 *
 * 若请求新 user namespace，cred 指向可修改的准备中凭据；需要可写指针时
 * 必须调用并检查 nsset_cred()。否则 cred 借用 current_cred()。nsproxy 是
 * 尚未发布的私有副本；fs 仅在组合 mount 切换需要临时 fs_struct 时使用；
 * flags 记录本事务实际请求的 CLONE_NEW* 类型。失败由 put_nsset() 回滚，
 * 成功由 commit_nsset() 转移引用。
 */
struct nsset {
	unsigned flags;
	struct nsproxy *nsproxy;
	struct fs_struct *fs;
	const struct cred *cred;
};

/*
 * nsset_cred() - 仅在事务创建新 user namespace 时返回可修改凭据。
 * @set 是借用事务；匹配 CLONE_NEWUSER 返回借用的非 const cred，否则返回
 * NULL，防止调用者错误修改 current 的只读凭据。函数不增加引用、不睡眠。
 */
static inline struct cred *nsset_cred(struct nsset *set)
{
	if (set->flags & CLONE_NEWUSER)
		return (struct cred *)set->cred;

	return NULL;
}

/*
 * the namespaces access rules are:
 *
 *  1. only current task is allowed to change tsk->nsproxy pointer or
 *     any pointer on the nsproxy itself.  Current must hold the task_lock
 *     when changing tsk->nsproxy.
 *
 *  2. when accessing (i.e. reading) current task's namespaces - no
 *     precautions should be taken - just dereference the pointers
 *
 *  3. the access to other task namespaces is performed like this
 *     task_lock(task);
 *     nsproxy = task->nsproxy;
 *     if (nsproxy != NULL) {
 *             / *
 *               * work with the namespaces here
 *               * e.g. get the reference on one of them
 *               * /
 *     } / *
 *         * NULL task->nsproxy means that this task is
 *         * almost dead (zombie)
 *         * /
 *     task_unlock(task);
 *
 */
/*
 * namespace 访问规则如下：
 *
 * 1. 只有任务自己可以修改 tsk->nsproxy 或其中槽位；修改 task->nsproxy 时
 *    current 必须持有 task_lock。
 * 2. current 读取自己的 namespace 可直接解引用，不需要额外同步。
 * 3. 读取其他任务时必须持 task_lock，检查 nsproxy 非 NULL，并在锁内取得
 *    目标 namespace 引用；NULL 表示任务已接近死亡。解锁后只能使用已经
 *    转成持有引用的对象，不能继续使用裸 nsproxy 指针。
 *
 * utsns_get() 正是规则 3 的 UTS 实现，switch_task_namespaces() 是规则 1
 * 的统一写路径。
 */

/* namespace 创建、切换、退出和 active 引用的跨文件实现入口。 */
int copy_namespaces(u64 flags, struct task_struct *tsk);
void switch_cred_namespaces(const struct cred *old, const struct cred *new);
void exit_nsproxy_namespaces(struct task_struct *tsk);
void get_cred_namespaces(struct task_struct *tsk);
void exit_cred_namespaces(struct task_struct *tsk);
void switch_task_namespaces(struct task_struct *tsk, struct nsproxy *new);
int exec_task_namespaces(void);
void deactivate_nsproxy(struct nsproxy *ns);
int unshare_nsproxy_namespaces(unsigned long, struct nsproxy **,
	struct cred *, struct fs_struct *);
int __init nsproxy_cache_init(void);

/*
 * put_nsproxy() - 释放 nsproxy 自身引用。
 * @ns 必须非空且由调用者持有。最后一次 put 调用 deactivate_nsproxy()，
 * 先归还每个槽位的 active 引用，再释放全部普通引用及 nsproxy 内存。
 */
static inline void put_nsproxy(struct nsproxy *ns)
{
	if (refcount_dec_and_test(&ns->count))
		deactivate_nsproxy(ns);
}

/*
 * get_nsproxy() - 为已稳定存活的 nsproxy 增加引用。
 * @ns 不可为空；调用者必须已通过 task_lock 或现有引用排除并发归零。
 * 增加 nsproxy 引用不会重复增加每个具体 namespace 的 active/普通引用。
 */
static inline void get_nsproxy(struct nsproxy *ns)
{
	refcount_inc(&ns->count);
}

/* 让 cleanup class 在变量离开作用域时对非空 nsproxy 自动执行 put。 */
DEFINE_FREE(put_nsproxy, struct nsproxy *, if (_T) put_nsproxy(_T))

#endif
