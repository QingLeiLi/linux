// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Andrei Vagin <avagin@openvz.org>
 * Author: Dmitry Safonov <dima@arista.com>
 */
/*
 * time namespace 不虚拟化 REALTIME，而为 MONOTONIC 与 BOOTTIME 保存相对 host 的 timespec64 offsets；读时加
 * offset，用户绝对期限入内核时减 offset。nsproxy 同时持当前 time_ns 和供未来 fork 的 time_ns_for_children，
 * 后者在任务首次加入时冻结，禁止再改 offset。对象由 ns ref、user_ns、ucounts、namespace tree/RCU 及可选
 * vDSO vvar page 共同管理；timens_offset_lock 串行 offsets writer 与 commit。
 */

#include <linux/time_namespace.h>
#include <linux/user_namespace.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/clocksource.h>
#include <linux/seq_file.h>
#include <linux/proc_ns.h>
#include <linux/export.h>
#include <linux/nstree.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/cleanup.h>

#include "namespace_internal.h"

/*
 * do_timens_ktime_to_host() - 把 namespace 绝对期限转成 host monotonic 坐标。
 * MONOTONIC 减 monotonic offset，BOOTTIME/ALARM 减 boottime offset；其他 clock 原样返回。用户期限小于正
 * offset 表示 host 上已到期，钳 0；减法结果越过 KTIME_MAX 则钳上限。@ns_offsets 只读且由冻结/锁协议稳定。
 */
ktime_t do_timens_ktime_to_host(clockid_t clockid, ktime_t tim,
				struct timens_offsets *ns_offsets)
{
	ktime_t offset;

	switch (clockid) {
	case CLOCK_MONOTONIC:
		offset = timespec64_to_ktime(ns_offsets->monotonic);
		break;
	case CLOCK_BOOTTIME:
	case CLOCK_BOOTTIME_ALARM:
		offset = timespec64_to_ktime(ns_offsets->boottime);
		break;
	default:
		return tim;
	}

	/*
	 * Check that @tim value is in [offset, KTIME_MAX + offset]
	 * and subtract offset.
	 */
	/* 有效 host 结果应落在 0..KTIME_MAX；用两端钳位避免负值或有符号减法越界。 */
	if (tim < offset) {
		/*
		 * User can specify @tim *absolute* value - if it's lesser than
		 * the time namespace's offset - it's already expired.
		 */
		/* namespace 绝对值早于其零点，在 host 视角已过期，以 0 促使 timer 立即处理。 */
		tim = 0;
	} else {
		tim = ktime_sub(tim, offset);
		if (unlikely(tim > KTIME_MAX))
			tim = KTIME_MAX;
	}

	return tim;
}
EXPORT_SYMBOL_GPL(do_timens_ktime_to_host);

/* inc_time_namespaces() 按 current euid 在 owner user_ns 计一份 UCOUNT_TIME_NAMESPACES；超限返回 NULL。 */
static struct ucounts *inc_time_namespaces(struct user_namespace *ns)
{
	return inc_ucount(ns, current_euid(), UCOUNT_TIME_NAMESPACES);
}

/* dec_time_namespaces() 归还 clone 成功前取得的 ucount；@ucounts 必须非空且恰好释放一次。 */
static void dec_time_namespaces(struct ucounts *ucounts)
{
	dec_ucount(ucounts, UCOUNT_TIME_NAMESPACES);
}

/**
 * clone_time_ns - Clone a time namespace
 * @user_ns:	User namespace which owns a new namespace.
 * @old_ns:	Namespace to clone
 *
 * Clone @old_ns and set the clone refcount to 1
 *
 * Return: The new namespace or ERR_PTR.
 */
/*
 * clone_time_ns() - 克隆 @old_ns offsets 并创建 refcount=1 的新 namespace。
 * 依次占 user quota、accounted zero object、可选 vvar page、ns_common id，再取得 owner user_ns 引用并加入
 * namespace tree。新 offsets 可修改（frozen=false）。失败按相反顺序回滚，分别返回 -ENOSPC/-ENOMEM 或 helper
 * errno；成功对象拥有 ucounts/user_ns/vvar/ns_common/tree，@old_ns 仅借用读取。
 */
static struct time_namespace *clone_time_ns(struct user_namespace *user_ns,
					  struct time_namespace *old_ns)
{
	struct time_namespace *ns;
	struct ucounts *ucounts;
	int err;

	err = -ENOSPC;
	ucounts = inc_time_namespaces(user_ns);
	if (!ucounts)
		goto fail;

	err = -ENOMEM;
	ns = kzalloc_obj(*ns, GFP_KERNEL_ACCOUNT);
	if (!ns)
		goto fail_dec;

	err = timens_vdso_alloc_vvar_page(ns);
	if (err)
		goto fail_free;

	err = ns_common_init(ns);
	if (err)
		goto fail_free_page;

	ns->ucounts = ucounts;
	ns->user_ns = get_user_ns(user_ns);
	ns->offsets = old_ns->offsets;
	ns->frozen_offsets = false;
	ns_tree_add(ns);
	return ns;

fail_free_page:
	timens_vdso_free_vvar_page(ns);
fail_free:
	kfree(ns);
fail_dec:
	dec_time_namespaces(ucounts);
fail:
	return ERR_PTR(err);
}

/**
 * copy_time_ns - Create timens_for_children from @old_ns
 * @flags:	Cloning flags
 * @user_ns:	User namespace which owns a new namespace.
 * @old_ns:	Namespace to clone
 *
 * If CLONE_NEWTIME specified in @flags, creates a new timens_for_children;
 * adds a refcounter to @old_ns otherwise.
 *
 * Return: timens_for_children namespace or ERR_PTR.
 */
/*
 * copy_time_ns() - nsproxy clone 时建立新的 time_ns_for_children 引用。
 * 无 CLONE_NEWTIME 时给 @old_ns 增 ref 返回同对象；有标志时调用 clone，返回新对象或 ERR_PTR。@user_ns 只在
 * 新建路径成为 owner；本层不修改调用者 nsproxy。
 */
struct time_namespace *copy_time_ns(u64 flags,
	struct user_namespace *user_ns, struct time_namespace *old_ns)
{
	if (!(flags & CLONE_NEWTIME))
		return get_time_ns(old_ns);

	return clone_time_ns(user_ns, old_ns);
}

DEFINE_MUTEX(timens_offset_lock);
/* 全局 mutex 让多个 proc writer 与任务 commit/freeze 形成单一提交顺序；读已冻结 offsets 无需长期持锁。 */

/*
 * free_time_ns() - 最后一个 namespace ref 释放后的完整析构。
 * 先从 namespace tree 摘除，归还 ucount/user_ns/ns_common/vvar 资源；并发 nstree 读者可能仍持裸指针，最终用
 * kfree_rcu 延迟释放容器。@ns 不得是 init namespace，调用者已通过 ns_ref_put 确认最后引用。
 */
void free_time_ns(struct time_namespace *ns)
{
	ns_tree_remove(ns);
	dec_time_namespaces(ns->ucounts);
	put_user_ns(ns->user_ns);
	ns_common_free(ns);
	timens_vdso_free_vvar_page(ns);
	/* Concurrent nstree traversal depends on a grace period. */
	/* tree 摘除只阻止新发现者，已有 RCU遍历需宽限期后才能回收对象内存。 */
	kfree_rcu(ns, ns.ns_rcu);
}

/*
 * timens_get() - proc namespace `time` getter。
 * task_lock 下稳定 task->nsproxy；退出任务无 nsproxy 返回 NULL，否则给当前 time_ns 增 ref 并返回内嵌 ns_common。
 * guard 自动解 task_lock，返回引用由 timens_put 配对。
 */
static struct ns_common *timens_get(struct task_struct *task)
{
	struct time_namespace *ns;
	struct nsproxy *nsproxy;

	guard(task_lock)(task);
	nsproxy = task->nsproxy;
	if (!nsproxy)
		return NULL;

	ns = nsproxy->time_ns;
	get_time_ns(ns);
	return &ns->ns;
}

/*
 * timens_for_children_get() - proc `time_for_children` getter。
 * 与 timens_get 相同地在 task_lock 下取得 nsproxy，但选择未来子任务 namespace；成功引用交 proc 调用者持有。
 */
static struct ns_common *timens_for_children_get(struct task_struct *task)
{
	struct time_namespace *ns;
	struct nsproxy *nsproxy;

	guard(task_lock)(task);
	nsproxy = task->nsproxy;
	if (!nsproxy)
		return NULL;

	ns = nsproxy->time_ns_for_children;
	get_time_ns(ns);
	return &ns->ns;
}

/* timens_put() 把通用 ns_common 转回 time_namespace 并归还一份 ref，最后引用可能触发 RCU析构。 */
static void timens_put(struct ns_common *ns)
{
	put_time_ns(to_time_ns(ns));
}

/*
 * timens_install() - setns 同时替换 current 与 future-children 的 time namespace。
 * 多线程进程返回 -EUSERS；调用者必须在目标 owner user_ns 和当前凭据 user_ns 都具 CAP_SYS_ADMIN，否则 -EPERM。
 * 成功对目标各取两份 ref、分别归还 nsproxy 旧引用并发布两个指针，返回 0；相同对象也以 get-before-put 安全替换。
 */
static int timens_install(struct nsset *nsset, struct ns_common *new)
{
	struct nsproxy *nsproxy = nsset->nsproxy;
	struct time_namespace *ns = to_time_ns(new);

	if (!current_is_single_threaded())
		return -EUSERS;

	if (!ns_capable(ns->user_ns, CAP_SYS_ADMIN) ||
	    !ns_capable(nsset->cred->user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	get_time_ns(ns);
	put_time_ns(nsproxy->time_ns);
	nsproxy->time_ns = ns;

	get_time_ns(ns);
	put_time_ns(nsproxy->time_ns_for_children);
	nsproxy->time_ns_for_children = ns;
	return 0;
}

/*
 * timens_on_fork() - fork 完成 namespace 创建后让新任务加入 time_ns_for_children。
 * create_new_namespaces 已持 children ref；若 current/children 同对象无需动作。否则再给目标增一份 current ref，
 * 归还旧 time_ns 并发布，随后 timens_commit 冻结 offsets/更新任务 vDSO 视图。@tsk 是正在创建且未并发运行的任务。
 */
void timens_on_fork(struct nsproxy *nsproxy, struct task_struct *tsk)
{
	struct ns_common *nsc = &nsproxy->time_ns_for_children->ns;
	struct time_namespace *ns = to_time_ns(nsc);

	/* create_new_namespaces() already incremented the ref counter */
	/* nsproxy 的 children 槽已有引用；current 槽若切过去仍需自己额外取得一份。 */
	if (nsproxy->time_ns == nsproxy->time_ns_for_children)
		return;

	get_time_ns(ns);
	put_time_ns(nsproxy->time_ns);
	nsproxy->time_ns = ns;

	timens_commit(tsk, ns);
}

/* timens_owner() 返回 namespace 持有的 user_ns 借用指针，供 proc ns 权限/层级判断；不增 user_ns 引用。 */
static struct user_namespace *timens_owner(struct ns_common *ns)
{
	return to_time_ns(ns)->user_ns;
}

/* show_offset() 把 MONOTONIC/BOOTTIME/未知 id 映射名称并按固定列输出 signed seconds/nanoseconds；不加锁。 */
static void show_offset(struct seq_file *m, int clockid, struct timespec64 *ts)
{
	char *clock;

	switch (clockid) {
	case CLOCK_BOOTTIME:
		clock = "boottime";
		break;
	case CLOCK_MONOTONIC:
		clock = "monotonic";
		break;
	default:
		clock = "unknown";
		break;
	}
	seq_printf(m, "%-10s %10lld %9ld\n", clock, ts->tv_sec, ts->tv_nsec);
}

/*
 * proc_timens_show_offsets() - 输出任务 future-children namespace 的两项 offset。
 * getter 失败（任务退出）静默无输出；成功引用由 __free(time_ns) 自动 put。offset 在 freeze 前可能与 writer 并发，
 * 本接口不取 offset mutex，诊断输出不是事务快照；seq 错误由 seq_file 内部记录。
 */
void proc_timens_show_offsets(struct task_struct *p, struct seq_file *m)
{
	struct time_namespace *time_ns __free(time_ns) = NULL;
	struct ns_common *ns = timens_for_children_get(p);

	if (!ns)
		return;

	time_ns = to_time_ns(ns);

	show_offset(m, CLOCK_MONOTONIC, &time_ns->offsets.monotonic);
	show_offset(m, CLOCK_BOOTTIME, &time_ns->offsets.boottime);
}

/*
 * proc_timens_set_offset() - 校验并提交 `time_for_children` 的一批 MONOTONIC/BOOTTIME offsets。
 * 任务无 nsproxy -ESRCH；打开 proc 文件的凭据在目标 owner user_ns 缺 CAP_SYS_TIME 则 -EPERM。第一遍拒绝其他
 * clock (-EINVAL)、offset 秒越 ktime 范围或 host_now+offset 不在 [0, KTIME_SEC_MAX/2] (-ERANGE)，不改状态。
 * 随后持 timens_offset_lock 与 commit/freeze 串行；已冻结 -EACCES。通过后第二遍无失败点地逐项赋值，重复 clock
 * 后项覆盖前项，返回 0。目标 namespace 引用由 __free 自动 put，@offsets 为调用者已解析内核数组。
 */
int proc_timens_set_offset(struct file *file, struct task_struct *p,
			   struct proc_timens_offset *offsets, int noffsets)
{
	struct time_namespace *time_ns __free(time_ns) = NULL;
	struct ns_common *ns = timens_for_children_get(p);
	struct timespec64 tp;
	int i;

	if (!ns)
		return -ESRCH;

	time_ns = to_time_ns(ns);

	if (!file_ns_capable(file, time_ns->user_ns, CAP_SYS_TIME))
		return -EPERM;

	for (i = 0; i < noffsets; i++) {
		struct proc_timens_offset *off = &offsets[i];

		switch (off->clockid) {
		case CLOCK_MONOTONIC:
			ktime_get_ts64(&tp);
			break;
		case CLOCK_BOOTTIME:
			ktime_get_boottime_ts64(&tp);
			break;
		default:
			return -EINVAL;
		}

		if (off->val.tv_sec > KTIME_SEC_MAX ||
		    off->val.tv_sec < -KTIME_SEC_MAX)
			return -ERANGE;

		tp = timespec64_add(tp, off->val);
		/*
		 * KTIME_SEC_MAX is divided by 2 to be sure that KTIME_MAX is
		 * still unreachable.
		 */
		/* 将绝对上限留一半余量，确保后续 namespace/host 换算仍无法触及 ktime 饱和边界。 */
		if (tp.tv_sec < 0 || tp.tv_sec > KTIME_SEC_MAX / 2)
			return -ERANGE;
	}

	guard(mutex)(&timens_offset_lock);
	if (time_ns->frozen_offsets)
		return -EACCES;

	/* Don't report errors after this line */
	/* freeze 检查通过即进入提交段；switch 只可能处理已在第一遍验证的两个 id。 */
	for (i = 0; i < noffsets; i++) {
		struct proc_timens_offset *off = &offsets[i];
		struct timespec64 *offset = NULL;

		switch (off->clockid) {
		case CLOCK_MONOTONIC:
			offset = &time_ns->offsets.monotonic;
			break;
		case CLOCK_BOOTTIME:
			offset = &time_ns->offsets.boottime;
			break;
		}

		*offset = off->val;
	}

	return 0;
}

/*
 * 两张 proc_ns_operations 共享 put/install/owner；区别仅在 get：`time` 返回任务当前视图，
 * `time_for_children` 返回 fork 将继承的可配置视图，并用 real_ns_name 把类型归一为 time。
 */
const struct proc_ns_operations timens_operations = {
	.name		= "time",
	.get		= timens_get,
	.put		= timens_put,
	.install	= timens_install,
	.owner		= timens_owner,
};

const struct proc_ns_operations timens_for_children_operations = {
	.name		= "time_for_children",
	.real_ns_name	= "time",
	.get		= timens_for_children_get,
	.put		= timens_put,
	.install	= timens_install,
	.owner		= timens_owner,
};

struct time_namespace init_time_ns = {
	.ns		= NS_COMMON_INIT(init_time_ns),
	.user_ns	= &init_user_ns,
	.frozen_offsets	= true,
};
EXPORT_SYMBOL_GPL(init_time_ns);
/* init namespace 静态常驻、属于 init_user_ns，offset 零且预先 frozen；不走 ucounts/vvar/动态析构路径。 */

/* time_ns_init() 在 early init 把静态 init namespace 加入 namespace tree；无分配、无错误返回。 */
void __init time_ns_init(void)
{
	ns_tree_add(&init_time_ns);
}
