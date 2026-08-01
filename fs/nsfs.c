// SPDX-License-Identifier: GPL-2.0
/*
 * namespace 文件系统学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * nsfs 是内核内部挂载的伪文件系统。它不提供可遍历的普通目录树，
 * 而是把
 * struct ns_common 所代表的 namespace 身份投影成匿名 inode/dentry，使
 * /proc/<pid>/ns/*、相关 ioctl、bind mount、文件句柄和内核调用者都能用
 * VFS path/file/fd 稳定持有同一个 namespace。
 *
 * 主路径：
 *   nsfs_init() -> kern_mount(nsfs)
 *   namespace 的 ops->get() -> path_from_stashed() -> nsfs_init_inode()
 *   -> dentry_open()/FD_ADD() -> ioctl/readlink/bind mount
 *   file/inode 最后释放 -> nsfs_evict() -> ops->put()
 *   name_to_handle_at/open_by_handle_at -> encode_fh()/fh_to_dentry()
 *
 * 核心对象与所有权：
 *   ns_common.stashed 原子缓存该 namespace 可复用的 dentry；inode->i_private
 *   保存 namespace 裸指针，而 inode 持有调用者传入的被动引用。inode 创建
 *   时还取得 active reference，使已无进程使用、但仍被其他内核对象钉住的
 *   namespace 所有权树可以复活；evict 时按相反顺序释放。
 *   nsfs_mnt/nsfs_root_path 是启动期建立并永久持有的内部挂载及其根路径。
 *
 * 并发模型：
 *   path_from_stashed() 用 cmpxchg/lockref 解决多个调用者同时为同一
 *   namespace 建 dentry 的竞争；stashed 只缓存身份，不替代 namespace
 *   引用。namespace tree 由 RCU 查找，离开 RCU 前必须通过
 *   ns_get_unless_inactive() 取得稳定被动引用。active reference 维护整棵
 *   user-namespace ownership 链是否活跃，但不等同于字段锁。
 *
 * 方案权衡：
 *   每个 namespace 最多复用一个匿名 dentry，避免建立可见目录层级并保持
 *   bind mount/file descriptor 的稳定 dev+ino 身份；代价是所有入口都必须
 *   严格遵守“传入引用无条件被消费”的 stashed-path 契约，并对文件
 *   句柄恢复施加额外可见性和类型检查。
 */
#include <linux/mount.h>
#include <linux/pseudo_fs.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/proc_ns.h>
#include <linux/magic.h>
#include <linux/ktime.h>
#include <linux/seq_file.h>
#include <linux/pid_namespace.h>
#include <linux/user_namespace.h>
#include <linux/nsfs.h>
#include <linux/uaccess.h>
#include <linux/mnt_namespace.h>
#include <linux/ipc_namespace.h>
#include <linux/time_namespace.h>
#include <linux/utsname.h>
#include <linux/exportfs.h>
#include <linux/nstree.h>
#include <net/net_namespace.h>

#include "mount.h"
#include "internal.h"

/*
 * nsfs_mnt 是 nsfs_init() 建立后永久持有的内部挂载。所有 namespace
 * 匿名 dentry 都属于这个 superblock，因此相同 namespace 的 dev+ino
 * 身份可跨 /proc 链接和 bind mount 比较。启动完成后只读。
 */
static struct vfsmount *nsfs_mnt;

/*
 * nsfs 根 path 的全局模板；本对象自身持有内核挂载的长期引用，
 * 调用者必须通过 nsfs_get_root() 再取得自己的 path 引用，不能直接
 * 借用后长期保存。
 */
static struct path nsfs_root_path = {};

/*
 * nsfs_get_root() - 向调用者返回一份持有引用的 nsfs 根路径。
 *
 * path 是非空输出参数；函数复制全局 mnt/dentry 后执行 path_get()，故成功
 * 后调用者必须 path_put()。无失败返回、无睡眠和私有锁要求；全局模板在
 * nsfs_init() 后不再改变。open_by_handle 等路径用它作为 nsfs 根锚点。
 */
void nsfs_get_root(struct path *path)
{
	*path = nsfs_root_path;
	path_get(path);
}

static long ns_ioctl(struct file *filp, unsigned int ioctl,
			unsigned long arg);
/*
 * 所有 namespace 文件共享同一操作表：本文件只提供 ioctl，compat 路径
 * 复用指针参数转换 helper。具体 namespace 类型差异由 inode->i_private
 * 中 ns_common 的 ops/ns_type 在 ns_ioctl() 内分派。
 */
static const struct file_operations ns_file_operations = {
	.unlocked_ioctl = ns_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/*
 * ns_dname() - 动态格式化 namespace 匿名 dentry 的显示名。
 *
 * dentry/buffer 是 VFS 借用的输入及输出缓冲区，buflen 为字节容量。inode
 * 存活保证 i_private 中 ns_common 及 ops 有效。返回 buffer 内字符串起点
 * 或 dynamic_dname() 的错误编码；不分配内存、不改变引用。
 * 结果形如 "net:[4026531992]"，用于 /proc 链接和路径打印，不是目录名称。
 */
static char *ns_dname(struct dentry *dentry, char *buffer, int buflen)
{
	struct inode *inode = d_inode(dentry);
	struct ns_common *ns = inode->i_private;
	const struct proc_ns_operations *ns_ops = ns->ops;

	return dynamic_dname(buffer, buflen, "%s:[%llu]",
		ns_ops->name, inode->i_ino);
}

/*
 * d_dname 提供动态 namespace 名称；d_prune 在 dcache 摘除 dentry 时以
 * cmpxchg 清空 ns->stashed，避免缓存继续指向死亡 dentry。操作表全局只读。
 */
const struct dentry_operations ns_dentry_operations = {
	.d_dname	= ns_dname,
	.d_prune	= stashed_dentry_prune,
};

/*
 * nsfs_evict() - 在 nsfs inode 最终回收时闭合 namespace 引用生命周期。
 *
 * VFS super_operations.evict_inode 调用，inode 为独占回收对象。先减少
 * nsfs_init_inode() 取得的 active reference，使必要时沿 owning user_ns
 * 向上退活；clear_inode() 完成 VFS 清理；最后 ops->put() 归还 inode 持有
 * 的被动 namespace 引用。无返回值，调用后 i_private 不再可用。
 */
static void nsfs_evict(struct inode *inode)
{
	struct ns_common *ns = inode->i_private;

	__ns_ref_active_put(ns);
	clear_inode(inode);
	ns->ops->put(ns);
}

/*
 * ns_get_path_cb() - 通过回调取得 namespace 引用并物化为 nsfs path。
 *
 * path 是输出参数；ns_get_cb/private_data 描述如何取得目标。回调成功必须
 * 返回一份持有引用的 ns_common，path_from_stashed() 无论成功失败都会
 * 消费该引用：复用 dentry 时立即 put，新建时转交 inode，失败时也回滚。
 * 成功返回 0，path 持有 mnt+dentry 引用；调用者须 path_put()。回调返回
 * NULL 时返回 -ENOENT。路径分配可能睡眠。
 */
int ns_get_path_cb(struct path *path, ns_get_path_helper_t *ns_get_cb,
		     void *private_data)
{
	struct ns_common *ns;

	ns = ns_get_cb(private_data);
	if (!ns)
		return -ENOENT;

	return path_from_stashed(&ns->stashed, nsfs_mnt, ns, path);
}

/*
 * ns_get_path_task_args 把通用 void * 回调需要的两项借用输入打包：
 * ns_ops 决定 namespace 类型及 get/put 协议，task 是要查询的稳定任务。
 * 对象只活到同步 ns_get_path_cb() 返回，不发生引用转移。
 */
struct ns_get_path_task_args {
	const struct proc_ns_operations *ns_ops;
	struct task_struct *task;
};

/*
 * ns_get_path_task() - 将通用回调参数还原并调用类型专用 ops->get(task)。
 *
 * 返回 NULL 表示任务没有该 namespace；非 NULL 是持有引用的 ns_common，
 * 所有权立即交给 ns_get_path_cb()/path_from_stashed()。是否加锁和能否失败
 * 由具体 namespace 实现保证。
 */
static struct ns_common *ns_get_path_task(void *private_data)
{
	struct ns_get_path_task_args *args = private_data;

	return args->ns_ops->get(args->task);
}

/*
 * ns_get_path() - 获取某任务指定类型 namespace 的稳定 nsfs path。
 *
 * path 为输出；task/ns_ops 都是借用输入，调用者须保证 task 在同步调用期间
 * 存活。函数把专用 get 回调适配到 ns_get_path_cb()。返回 0 时调用者持有
 * path 引用，负 errno 时无输出引用；namespace 引用始终由下层消费。
 */
int ns_get_path(struct path *path, struct task_struct *task,
		  const struct proc_ns_operations *ns_ops)
{
	struct ns_get_path_task_args args = {
		.ns_ops	= ns_ops,
		.task	= task,
	};

	return ns_get_path_cb(path, ns_get_path_task, &args);
}

/*
 * open_namespace_file() - 把一份 namespace 引用转换为内核 struct file。
 *
 * ns 必须是持有引用的输入，并且无论成功失败都会被消费。临时 path 带
 * __free(path_put)，离开作用域自动释放；dentry_open() 成功后 file 自己
 * 持有 path，返回持有引用的 file，调用者必须 fput()。失败返回 ERR_PTR。
 */
struct file *open_namespace_file(struct ns_common *ns)
{
	struct path path __free(path_put) = {};
	int err;

	/* call first to consume reference */
	/*
	 * 必须首先调用它来消费引用；之后任何错误出口都不能再手工
	 * ns->ops->put(ns)，否则会 double put。
	 */
	err = path_from_stashed(&ns->stashed, nsfs_mnt, ns, &path);
	if (err < 0)
		return ERR_PTR(err);

	return dentry_open(&path, O_RDONLY, current_cred());
}

/**
 * open_namespace - open a namespace
 * @ns: the namespace to open
 *
 * This will consume a reference to @ns indendent of success or failure.
 *
 * Return: A file descriptor on success or a negative error code on failure.
 */
/*
 * 打开一个 namespace。@ns 是持有引用的输入；无论成功或失败，该引用都会
 * 被消费。成功时返回安装到当前进程 fdtable 的非负、带 O_CLOEXEC 的文件
 * 描述符；失败返回负 errno。临时 path 由 scope cleanup 释放，fd/file 的
 * 发布与失败回滚由 FD_ADD() 负责。
 */
int open_namespace(struct ns_common *ns)
{
	struct path path __free(path_put) = {};
	int err;

	/* call first to consume reference */
	/*
	 * 与 open_namespace_file() 相同，引用消费必须发生在所有后续
	 * 步骤之前。
	 */
	err = path_from_stashed(&ns->stashed, nsfs_mnt, ns, &path);
	if (err < 0)
		return err;

	return FD_ADD(O_CLOEXEC, dentry_open(&path, O_RDONLY, current_cred()));
}

/*
 * open_related_ns() - 由已有 namespace 打开其 owner/parent 等关联对象。
 *
 * ns 是借用输入；get_ns 回调成功返回持有引用的关联 ns，错误用 ERR_PTR
 * 表示。成功引用转交 open_namespace() 并被无条件消费，最终返回新 fd；
 * 回调错误直接转成负 errno。函数导出给需要统一关联 namespace fd ABI
 * 的内核模块。
 */
int open_related_ns(struct ns_common *ns,
		   struct ns_common *(*get_ns)(struct ns_common *ns))
{
	struct ns_common *relative;

	relative = get_ns(ns);
	if (IS_ERR(relative))
		return PTR_ERR(relative);

	return open_namespace(relative);
}
EXPORT_SYMBOL_GPL(open_related_ns);

/*
 * copy_ns_info_to_user() - 按用户声明的结构体大小复制 mount namespace 信息。
 *
 * mnt_ns 是借用且已被调用者稳定的输入；uinfo 是用户输出地址，可为 NULL
 * 但 copy_to_user 会以 -EFAULT 失败；usize 是 ioctl 编码中的用户 ABI
 * 字节数；kinfo 是调用者栈上的零初始化暂存区。
 *
 * size 取用户/内核结构体较小者，实现结构体尾部可扩展 ABI。nr_mounts 用
 * READ_ONCE() 避免编译器撕裂/重复读取，但不是一致性快照；减去 namespace
 * 内部根 mount 后向用户报告可见挂载数量。成功返回 0，复制失败 -EFAULT。
 */
static int copy_ns_info_to_user(const struct mnt_namespace *mnt_ns,
				struct mnt_ns_info __user *uinfo, size_t usize,
				struct mnt_ns_info *kinfo)
{
	/*
	 * If userspace and the kernel have the same struct size it can just
	 * be copied. If userspace provides an older struct, only the bits that
	 * userspace knows about will be copied. If userspace provides a new
	 * struct, only the bits that the kernel knows aobut will be copied and
	 * the size value will be set to the size the kernel knows about.
	 */
	/*
	 * 内核和用户结构体同长时完整复制；旧用户只得到认识的前缀；
	 * 新用户只得到当前内核认识的前缀，并由 size 告知实际版本。
	 * kinfo 预先清零，因而保留字段不会泄漏栈数据。
	 */
	kinfo->size		= min(usize, sizeof(*kinfo));
	kinfo->mnt_ns_id	= mnt_ns->ns.ns_id;
	kinfo->nr_mounts	= READ_ONCE(mnt_ns->nr_mounts);
	/* Subtract the root mount of the mount namespace. */
	/*
	 * 根 mount 是 namespace 实现细节，用户语义中的“挂载数”
	 * 不包含它。
	 */
	if (kinfo->nr_mounts)
		kinfo->nr_mounts--;

	if (copy_to_user(uinfo, kinfo, kinfo->size))
		return -EFAULT;

	return 0;
}

/*
 * nsfs_ioctl_valid() - 在解引用用户参数前验证 ioctl 编码和 ABI 尺寸。
 *
 * cmd 是纯输入；固定 ioctl 必须精确匹配完整命令。可扩展 ioctl 只按编号
 * 分派，再由 extensible_ioctl_valid() 校验方向、类型和最小版本尺寸，
 * 允许未来结构体尾部扩展。返回布尔值，无副作用、不会睡眠。
 */
static bool nsfs_ioctl_valid(unsigned int cmd)
{
	switch (cmd) {
	case NS_GET_USERNS:
	case NS_GET_PARENT:
	case NS_GET_NSTYPE:
	case NS_GET_OWNER_UID:
	case NS_GET_MNTNS_ID:
	case NS_GET_PID_FROM_PIDNS:
	case NS_GET_TGID_FROM_PIDNS:
	case NS_GET_PID_IN_PIDNS:
	case NS_GET_TGID_IN_PIDNS:
	case NS_GET_ID:
		return true;
	}

	/* Extensible ioctls require some extra handling. */
	/*
	 * 可扩展 ioctl 不能直接 switch 完整 cmd，因为 _IOC_SIZE 会随用户
	 * 结构体版本变化；先固定编号，再验证兼容前缀。
	 */
	switch (_IOC_NR(cmd)) {
	case _IOC_NR(NS_MNT_GET_INFO):
		return extensible_ioctl_valid(cmd, NS_MNT_GET_INFO, MNT_NS_INFO_SIZE_VER0);
	case _IOC_NR(NS_MNT_GET_NEXT):
		return extensible_ioctl_valid(cmd, NS_MNT_GET_NEXT, MNT_NS_INFO_SIZE_VER0);
	case _IOC_NR(NS_MNT_GET_PREV):
		return extensible_ioctl_valid(cmd, NS_MNT_GET_PREV, MNT_NS_INFO_SIZE_VER0);
	}

	return false;
}

/*
 * may_use_nsfs_ioctl() - 执行需要全局 namespace 可见性的额外授权。
 *
 * NEXT/PREV 会越过当前 namespace 枚举其他对象，只允许位于 init PID
 * namespace 且在其 user_ns 中有 CAP_SYS_ADMIN 的调用者。其他命令仍由
 * 各自类型/owner 检查约束。返回 true 表示可继续，不取得任何引用。
 */
static bool may_use_nsfs_ioctl(unsigned int cmd)
{
	switch (_IOC_NR(cmd)) {
	case _IOC_NR(NS_MNT_GET_NEXT):
		fallthrough;
	case _IOC_NR(NS_MNT_GET_PREV):
		return may_see_all_namespaces();
	}
	return true;
}

/*
 * ns_ioctl() - 实现 namespace fd 的查询、关联对象打开和 mount-ns 遍历 ABI。
 *
 * filp 是 VFS 保证存活的借用文件；ioctl 是已编码命令；arg 按命令解释为
 * 整数 ID 或用户指针。函数在进程上下文执行，可能分配、打开文件、复制
 * 用户内存并睡眠。inode 持有 namespace 引用，故从 i_private 取得的 ns
 * 在整个调用期间有效。
 *
 * 返回类别：类型/参数错误 -EINVAL，权限不足 -EPERM，查无任务 -ESRCH，
 * 用户地址错误 -EFAULT，未知命令 -ENOIOCTLCMD/-ENOTTY；查询命令返回
 * namespace 类型或 PID，打开命令返回新 fd，输出命令返回 0。只有
 * fd_publish() 是把预备文件描述符对当前进程公开的提交点，之前失败均由
 * cleanup class 回滚 file/path/namespace 引用。
 */
static long ns_ioctl(struct file *filp, unsigned int ioctl,
			unsigned long arg)
{
	/*
	 * 变量地图：
	 *   user_ns/pid_ns/mnt_ns  经类型校验后恢复的具体 namespace；
	 *   tsk                    仅在 RCU 临界区内借用的任务；
	 *   ns                     inode 持有的通用 namespace；
	 *   previous               mount-ns 遍历方向；
	 *   argp/uid               owner UID 的用户输出地址和映射后值；
	 *   ret                    errno、查询结果或待发布 fd。
	 */
	struct user_namespace *user_ns;
	struct pid_namespace *pid_ns;
	struct task_struct *tsk;
	struct ns_common *ns;
	struct mnt_namespace *mnt_ns;
	bool previous = false;
	uid_t __user *argp;
	uid_t uid;
	int ret;

	/* 第一层先验证编码，第二层先做跨 namespace 能力检查。 */
	if (!nsfs_ioctl_valid(ioctl))
		return -ENOIOCTLCMD;
	if (!may_use_nsfs_ioctl(ioctl))
		return -EPERM;

	ns = get_proc_ns(file_inode(filp));
	/*
	 * 固定尺寸命令直接按完整 ioctl 分派。打开 owner/parent 时 helper
	 * 返回持有引用并最终交给 fd；纯查询不改变当前 inode 的引用。
	 */
	switch (ioctl) {
	case NS_GET_USERNS:
		return open_related_ns(ns, ns_get_owner);
	case NS_GET_PARENT:
		/* 没有 get_parent 回调的 namespace 类型不具备层级 parent ABI。 */
		if (!ns->ops->get_parent)
			return -EINVAL;
		return open_related_ns(ns, ns->ops->get_parent);
	case NS_GET_NSTYPE:
		return ns->ns_type;
	case NS_GET_OWNER_UID:
		/* owner 字段只属于 user_namespace，先验证类型再 container_of。 */
		if (ns->ns_type != CLONE_NEWUSER)
			return -EINVAL;
		user_ns = container_of(ns, struct user_namespace, ns);
		argp = (uid_t __user *) arg;
		uid = from_kuid_munged(current_user_ns(), user_ns->owner);
		return put_user(uid, argp);
	case NS_GET_PID_FROM_PIDNS:
		fallthrough;
	case NS_GET_TGID_FROM_PIDNS:
		fallthrough;
	case NS_GET_PID_IN_PIDNS:
		fallthrough;
	case NS_GET_TGID_IN_PIDNS: {
		/*
		 * 四个命令构成两个坐标变换方向：
		 *   FROM_PIDNS：arg 是目标 pid_ns 中的编号，返回 current 视图编号；
		 *   IN_PIDNS：  arg 是 current 视图编号，返回目标 pid_ns 中的编号。
		 * PID/TGID 再决定线程 ID 或线程组 ID。
		 */
		if (ns->ns_type != CLONE_NEWPID)
			return -EINVAL;

		ret = -ESRCH;
		pid_ns = container_of(ns, struct pid_namespace, ns);

		/*
		 * guard(rcu) 使整个 task 查找和编号换算处于同一 RCU 读侧窗口。
		 * tsk 只是借用指针，没有 get_task_struct()，绝不能逃出
		 * 此作用域。
		 */
		guard(rcu)();

		if (ioctl == NS_GET_PID_IN_PIDNS ||
		    ioctl == NS_GET_TGID_IN_PIDNS)
			tsk = find_task_by_vpid(arg);
		else
			tsk = find_task_by_pid_ns(arg, pid_ns);
		if (!tsk)
			return ret;

		/*
		 * 找到 task 后按命令选择输出坐标系；0 表示在目标视图中
		 * 不可见。
		 */
		switch (ioctl) {
		case NS_GET_PID_FROM_PIDNS:
			ret = task_pid_vnr(tsk);
			break;
		case NS_GET_TGID_FROM_PIDNS:
			ret = task_tgid_vnr(tsk);
			break;
		case NS_GET_PID_IN_PIDNS:
			ret = task_pid_nr_ns(tsk, pid_ns);
			break;
		case NS_GET_TGID_IN_PIDNS:
			ret = task_tgid_nr_ns(tsk, pid_ns);
			break;
		default:
			ret = 0;
			break;
		}

		if (!ret)
			ret = -ESRCH;
		return ret;
	}
	case NS_GET_MNTNS_ID:
		/* 旧专用命令只接受 mount namespace，随后复用通用 NS_GET_ID。 */
		if (ns->ns_type != CLONE_NEWNS)
			return -EINVAL;
		fallthrough;
	case NS_GET_ID: {
		__u64 __user *idp;
		__u64 id;

		idp = (__u64 __user *)arg;
		id = ns->ns_id;
		return put_user(id, idp);
	}
	}

	/* extensible ioctls */
	/*
	 * 可扩展命令按编号分派，因为用户可编码比当前内核结构体更大的
	 * _IOC_SIZE；nsfs_ioctl_valid() 已验证其兼容前缀。
	 */
	switch (_IOC_NR(ioctl)) {
	case _IOC_NR(NS_MNT_GET_INFO): {
		/* kinfo 清零防止未来/保留字段泄漏，usize 来自 ioctl 编码。 */
		struct mnt_ns_info kinfo = {};
		struct mnt_ns_info __user *uinfo = (struct mnt_ns_info __user *)arg;
		size_t usize = _IOC_SIZE(ioctl);

		if (ns->ns_type != CLONE_NEWNS)
			return -EINVAL;

		if (!uinfo)
			return -EINVAL;

		if (usize < MNT_NS_INFO_SIZE_VER0)
			return -EINVAL;

		return copy_ns_info_to_user(to_mnt_ns(ns), uinfo, usize, &kinfo);
	}
	case _IOC_NR(NS_MNT_GET_PREV):
		/* PREV 与 NEXT 共用主体，只通过 previous 固定遍历方向。 */
		previous = true;
		fallthrough;
	case _IOC_NR(NS_MNT_GET_NEXT): {
		struct mnt_ns_info kinfo = {};
		struct mnt_ns_info __user *uinfo = (struct mnt_ns_info __user *)arg;
		struct path path __free(path_put) = {};
		size_t usize = _IOC_SIZE(ioctl);

		if (ns->ns_type != CLONE_NEWNS)
			return -EINVAL;

		if (usize < MNT_NS_INFO_SIZE_VER0)
			return -EINVAL;

		/*
		 * helper 在 namespace tree 中寻找相邻、调用者有权限管理的
		 * mount namespace，并返回持有 active/passive 语义所需的引用。
		 */
		mnt_ns = get_sequential_mnt_ns(to_mnt_ns(ns), previous);
		if (IS_ERR(mnt_ns))
			return PTR_ERR(mnt_ns);

		ns = to_ns_common(mnt_ns);
		/* Transfer ownership of @mnt_ns reference to @path. */
		/*
		 * 把 mnt_ns 引用所有权转给 stashed path：下层无条件消费，
		 * 成功后 inode/path 接力持有，失败时下层负责 put。
		 */
		ret = path_from_stashed(&ns->stashed, nsfs_mnt, ns, &path);
		if (ret)
			return ret;

		/*
		 * FD_PREPARE 先保留 fd 并打开 file，但尚未向 fdtable 发布；
		 * 任一后续用户复制失败都会由 scope cleanup 同时撤销二者。
		 */
		FD_PREPARE(fdf, O_CLOEXEC, dentry_open(&path, O_RDONLY, current_cred()));
		if (fdf.err)
			return fdf.err;
		/*
		 * If @uinfo is passed return all information about the
		 * mount namespace as well.
		 */
		/*
		 * 若用户传入 uinfo，则在发布 fd 前同时返回相邻 mount namespace
		 * 信息；复制失败时用户不会得到一个“成功但未知编号”的
		 * 隐藏 fd。
		 */
		ret = copy_ns_info_to_user(to_mnt_ns(ns), uinfo, usize, &kinfo);
		if (ret)
			return ret;
		/* 最后发布是不可回滚的外部可见提交点，返回新 fd。 */
		ret = fd_publish(fdf);
		break;
	}
	default:
		ret = -ENOTTY;
	}

	return ret;
}

/*
 * ns_get_name() - 取得任务 namespace 的 "type:[inum]" 文本身份。
 *
 * buf/size 是内核输出缓冲区及字节容量；task/ns_ops 为借用输入。ops->get()
 * 成功返回持有引用，格式化后必须 ops->put()。real_ns_name 可为某些代理
 * 视图覆盖默认 name。返回 snprintf 长度（可能大于 size，表示截断所需
 * 长度）或 -ENOENT；不把 namespace 引用交给调用者。
 */
int ns_get_name(char *buf, size_t size, struct task_struct *task,
			const struct proc_ns_operations *ns_ops)
{
	struct ns_common *ns;
	int res = -ENOENT;
	const char *name;
	ns = ns_ops->get(task);
	if (ns) {
		name = ns_ops->real_ns_name ? : ns_ops->name;
		res = snprintf(buf, size, "%s:[%u]", name, ns->inum);
		ns_ops->put(ns);
	}
	return res;
}

/*
 * proc_ns_file() - 判断 file 是否由 nsfs namespace 操作表创建。
 *
 * file 是借用且必须有效；通过 f_op 身份而非路径字符串判断，避免伪造
 * 名称。返回布尔值，无引用变化。procfs 的 pidns= 参数据此拒绝普通文件。
 */
bool proc_ns_file(const struct file *file)
{
	return file->f_op == &ns_file_operations;
}

/**
 * ns_match() - Returns true if current namespace matches dev/ino provided.
 * @ns: current namespace
 * @dev: dev_t from nsfs that will be matched against current nsfs
 * @ino: ino_t from nsfs that will be matched against current nsfs
 *
 * Return: true if dev and ino matches the current nsfs.
 */
/*
 * 判断当前 namespace 是否匹配给定 nsfs 设备号和 inode 号。@ns 为借用；
 * @dev/@ino 通常来自 stat。只有 namespace 的稳定 inum 和唯一 nsfs
 * superblock 的 s_dev 同时相等才返回 true，无引用或锁副作用。
 */
bool ns_match(const struct ns_common *ns, dev_t dev, ino_t ino)
{
	return (ns->inum == ino) && (nsfs_mnt->mnt_sb->s_dev == dev);
}


/*
 * nsfs_show_path() - 为 mountinfo/exportfs 等 seq_file 输出 namespace 路径。
 *
 * seq/dentry 均由 VFS 借用，inode 生命周期保证 ns/ops 有效。写入
 * "type:[ino]" 后返回 0；seq_file 自行记录溢出，不改变 namespace 引用。
 */
static int nsfs_show_path(struct seq_file *seq, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	const struct ns_common *ns = inode->i_private;
	const struct proc_ns_operations *ns_ops = ns->ops;

	seq_printf(seq, "%s:[%llu]", ns_ops->name, inode->i_ino);
	return 0;
}

/*
 * nsfs superblock 操作表：使用伪文件系统 statfs；evict_inode 闭合
 * namespace 引用；show_path 提供动态身份；inode_just_drop 禁止把无链接
 * 匿名 inode 当作普通 inode 回写/删除。表在内部挂载生命周期内只读。
 */
static const struct super_operations nsfs_ops = {
	.statfs = simple_statfs,
	.evict_inode = nsfs_evict,
	.show_path = nsfs_show_path,
	.drop_inode = inode_just_drop,
};

/*
 * nsfs_init_inode() - 把新匿名 inode 初始化为某个 namespace 的文件代理。
 *
 * inode 是尚未发布的输入输出对象；data 是 path_from_stashed() 已转交给
 * 文件系统的一份 ns_common 持有引用。函数设置 i_private、只读权限、
 * ioctl 操作表和稳定 inum，并取得 active reference。成功恒返回 0；
 * 后续若 dentry 分配失败，iput() 会进入 nsfs_evict() 完整回滚。
 */
static int nsfs_init_inode(struct inode *inode, void *data)
{
	struct ns_common *ns = data;

	inode->i_private = data;
	inode->i_mode |= S_IRUGO;
	inode->i_fop = &ns_file_operations;
	inode->i_ino = ns->inum;

	/*
	 * Bring the namespace subtree back to life if we have to. This
	 * can happen when e.g., all processes using a network namespace
	 * and all namespace files or namespace file bind-mounts have
	 * died but there are still sockets pinning it. The SIOCGSKNS
	 * ioctl on such a socket will resurrect the relevant namespace
	 * subtree.
	 */
	/*
	 * 当使用该 net namespace 的进程、nsfs 文件和 bind mount 都消失时，
	 * socket 仍可能以被动引用钉住对象。之后 SIOCGSKNS 再生成 inode 时，
	 * active_get 会把该 namespace 及必要的 owning user_ns 链重新激活。
	 * 这不新增字段锁；它只维护 ownership 树的活跃生命周期。
	 */
	__ns_ref_active_get(ns);
	return 0;
}

/*
 * nsfs_put_data() - 回滚尚未被 inode 接管的 namespace 引用。
 *
 * path_from_stashed() 复用已有 dentry 或在创建失败时调用。data 必须是一份
 * 持有引用的 ns_common；ops->put() 无条件消费它。无返回值。
 */
static void nsfs_put_data(void *data)
{
	struct ns_common *ns = data;
	ns->ops->put(ns);
}

/*
 * stashed helper 回调表定义 data 所有权边界：新 inode 接管时执行
 * nsfs_init_inode()，无需接管或失败时执行 nsfs_put_data()。该表通过
 * superblock->s_fs_info 传给通用 path_from_stashed()。
 */
static const struct stashed_operations nsfs_stashed_ops = {
	.init_inode = nsfs_init_inode,
	.put_data = nsfs_put_data,
};

/*
 * exportfs 的 max_len 单位是 u32 槽而非字节；两个宏把 UAPI 文件句柄的
 * 首版最小尺寸和当前尺寸转换为该单位，保持未来尾部扩展兼容。
 */
#define NSFS_FID_SIZE_U32_VER0 (NSFS_FILE_HANDLE_SIZE_VER0 / sizeof(u32))
#define NSFS_FID_SIZE_U32_LATEST (NSFS_FILE_HANDLE_SIZE_LATEST / sizeof(u32))

/*
 * nsfs_encode_fh() - 把 namespace inode 编码成可恢复的 exportfs 文件句柄。
 *
 * inode 为借用；fh 是 u32 输出数组；max_len 输入容量/输出所需槽数；
 * parent 若非 NULL 表示父句柄请求，nsfs 无目录层级故拒绝。容量不足或
 * parent 请求返回 FILEID_INVALID；否则写 ns_id/ns_type/inum 并返回
 * FILEID_NSFS。只复制稳定标识，不取得 namespace 引用，也不睡眠。
 */
static int nsfs_encode_fh(struct inode *inode, u32 *fh, int *max_len,
			  struct inode *parent)
{
	struct nsfs_file_handle *fid = (struct nsfs_file_handle *)fh;
	struct ns_common *ns = inode->i_private;
	int len = *max_len;

	if (parent)
		return FILEID_INVALID;

	/*
	 * 不足时回报当前完整需求；过大时收窄实际编码长度，避免未定义
	 * 尾部。
	 */
	if (len < NSFS_FID_SIZE_U32_VER0) {
		*max_len = NSFS_FID_SIZE_U32_LATEST;
		return FILEID_INVALID;
	} else if (len > NSFS_FID_SIZE_U32_LATEST) {
		*max_len = NSFS_FID_SIZE_U32_LATEST;
	}

	fid->ns_id	= ns->ns_id;
	fid->ns_type	= ns->ns_type;
	fid->ns_inum	= inode->i_ino;
	return FILEID_NSFS;
}

/*
 * is_current_namespace() - 按 ns_type 分派“current 是否位于该 namespace”。
 *
 * ns 是借用输入；具体 current_in_namespace() 比较 current/nsproxy 中相应
 * 对象，不取得长期引用。返回布尔值。各 case 受对应 CONFIG_* 控制；传入
 * 本构建不支持或未知类型会触发一次 VFS 警告并返回 false。
 */
bool is_current_namespace(struct ns_common *ns)
{
	switch (ns->ns_type) {
#ifdef CONFIG_CGROUPS
	case CLONE_NEWCGROUP:
		return current_in_namespace(to_cg_ns(ns));
#endif
#ifdef CONFIG_IPC_NS
	case CLONE_NEWIPC:
		return current_in_namespace(to_ipc_ns(ns));
#endif
	case CLONE_NEWNS:
		return current_in_namespace(to_mnt_ns(ns));
#ifdef CONFIG_NET_NS
	case CLONE_NEWNET:
		return current_in_namespace(to_net_ns(ns));
#endif
#ifdef CONFIG_PID_NS
	case CLONE_NEWPID:
		return current_in_namespace(to_pid_ns(ns));
#endif
#ifdef CONFIG_TIME_NS
	case CLONE_NEWTIME:
		return current_in_namespace(to_time_ns(ns));
#endif
#ifdef CONFIG_USER_NS
	case CLONE_NEWUSER:
		return current_in_namespace(to_user_ns(ns));
#endif
#ifdef CONFIG_UTS_NS
	case CLONE_NEWUTS:
		return current_in_namespace(to_uts_ns(ns));
#endif
	default:
		VFS_WARN_ON_ONCE(true);
		return false;
	}
}

/*
 * nsfs_fh_to_dentry() - 从 exportfs 文件句柄恢复可打开的 nsfs dentry。
 *
 * sb 是目标 nsfs superblock（本实现不直接读取）；fh/fh_len/fh_type 是
 * 不可信输入。成功返回持有引用的 dentry；合法但已不存在或不匹配返回
 * NULL；权限、类型或创建错误返回 ERR_PTR。函数可能分配并睡眠。
 *
 * 控制流：验证可扩展句柄 -> RCU tree 查找并取得被动引用 -> 判断 current
 * 可见性和特殊 PID namespace 状态 -> 将引用交给 stashed path -> 把
 * dentry 引用移交 exportfs。取得引用后的失败出口必须 put 或转交下层。
 */
static struct dentry *nsfs_fh_to_dentry(struct super_block *sb, struct fid *fh,
					int fh_len, int fh_type)
{
	/*
	 * path 自动 path_put；fid 是 fh 的布局视图；owning_ns 只借用并作为
	 * 跨 namespace 标志；ns 在 RCU 内先借用，随后转成持有引用。
	 */
	struct path path __free(path_put) = {};
	struct nsfs_file_handle *fid = (struct nsfs_file_handle *)fh;
	struct user_namespace *owning_ns = NULL;
	struct ns_common *ns;
	int ret;

	if (fh_len < NSFS_FID_SIZE_U32_VER0)
		return NULL;

	/* Check that any trailing bytes are zero. */
	/*
	 * 允许比当前结构更长的未来句柄，但不认识的尾部必须全零；
	 * 非零表示调用者依赖了本内核无法解释的新语义，不能静默忽略。
	 */
	if ((fh_len > NSFS_FID_SIZE_U32_LATEST) &&
	    memchr_inv((void *)fid + NSFS_FID_SIZE_U32_LATEST, 0,
		       fh_len - NSFS_FID_SIZE_U32_LATEST))
		return NULL;

	switch (fh_type) {
	case FILEID_NSFS:
		break;
	default:
		return NULL;
	}

	if (!fid->ns_id)
		return NULL;
	/* Either both are set or both are unset. */
	/*
	 * ns_type 与旧式 ns_inum 是一组可选加强校验：必须同时出现或同时
	 * 缺省，防止只校验半个旧身份而误匹配。
	 */
	if (!fid->ns_inum != !fid->ns_type)
		return NULL;

	/*
	 * tree 指针只在 RCU 读侧稳定。先按全局 ns_id 和可选 type 查找，
	 * 再核对冗余字段，防止损坏或伪造句柄指向其他对象。
	 */
	scoped_guard(rcu) {
		ns = ns_tree_lookup_rcu(fid->ns_id, fid->ns_type);
		if (!ns)
			return NULL;

		VFS_WARN_ON_ONCE(ns->ns_id != fid->ns_id);

		if (fid->ns_inum && (fid->ns_inum != ns->inum))
			return NULL;
		if (fid->ns_type && (fid->ns_type != ns->ns_type))
			return NULL;

		/*
		 * This is racy because we're not actually taking an
		 * active reference. IOW, it could happen that the
		 * namespace becomes inactive after this check.
		 * We don't care because nsfs_init_inode() will just
		 * resurrect the relevant namespace tree for us. If it
		 * has been active here we just allow it's resurrection.
		 * We could try to take an active reference here and
		 * then drop it again. But really, why bother.
		 */
		/*
		 * 此处只要求查找时仍 active，再取得被动引用；检查后
		 * active 可能归零。无需强行保持 active，因为稍后新建 inode 时
		 * nsfs_init_inode() 会复活 ownership 子树；若复用旧 dentry，
		 * 原 inode 已持有 active reference。被动引用保证对象内存存活。
		 */
		if (!ns_get_unless_inactive(ns))
			return NULL;
	}

	/*
	 * 类型 switch 恢复具体对象，并判断目标是否就是 current 的
	 * namespace。跨 namespace 时记录 owning_ns，稍后要求全局可见权限。
	 */
	switch (ns->ns_type) {
#ifdef CONFIG_CGROUPS
	case CLONE_NEWCGROUP:
		if (!current_in_namespace(to_cg_ns(ns)))
			owning_ns = to_cg_ns(ns)->user_ns;
		break;
#endif
#ifdef CONFIG_IPC_NS
	case CLONE_NEWIPC:
		if (!current_in_namespace(to_ipc_ns(ns)))
			owning_ns = to_ipc_ns(ns)->user_ns;
		break;
#endif
	case CLONE_NEWNS:
		if (!current_in_namespace(to_mnt_ns(ns)))
			owning_ns = to_mnt_ns(ns)->user_ns;
		break;
#ifdef CONFIG_NET_NS
	case CLONE_NEWNET:
		if (!current_in_namespace(to_net_ns(ns)))
			owning_ns = to_net_ns(ns)->user_ns;
		break;
#endif
#ifdef CONFIG_PID_NS
	case CLONE_NEWPID:
		if (!current_in_namespace(to_pid_ns(ns))) {
			owning_ns = to_pid_ns(ns)->user_ns;
		} else if (!READ_ONCE(to_pid_ns(ns)->child_reaper)) {
			/*
			 * current 虽指向该 pid_ns，但 child_reaper 已清空说明
			 * namespace 正在死亡，禁止由文件句柄复活。
			 */
			ns->ops->put(ns);
			return ERR_PTR(-EPERM);
		}
		break;
#endif
#ifdef CONFIG_TIME_NS
	case CLONE_NEWTIME:
		if (!current_in_namespace(to_time_ns(ns)))
			owning_ns = to_time_ns(ns)->user_ns;
		break;
#endif
#ifdef CONFIG_USER_NS
	case CLONE_NEWUSER:
		if (!current_in_namespace(to_user_ns(ns)))
			owning_ns = to_user_ns(ns);
		break;
#endif
#ifdef CONFIG_UTS_NS
	case CLONE_NEWUTS:
		if (!current_in_namespace(to_uts_ns(ns)))
			owning_ns = to_uts_ns(ns)->user_ns;
		break;
#endif
	default:
		/* tree 中出现本构建不支持的类型属于内部契约违例。 */
		return ERR_PTR(-EOPNOTSUPP);
	}

	/*
	 * 只有 init PID namespace 中具有全局 CAP_SYS_ADMIN 的任务才能从
	 * 文件句柄跨入其他 namespace；拒绝前归还已取得的被动引用。
	 */
	if (owning_ns && !may_see_all_namespaces()) {
		ns->ops->put(ns);
		return ERR_PTR(-EPERM);
	}

	/* path_from_stashed() unconditionally consumes the reference. */
	/*
	 * 从这里起 ns 引用归 path_from_stashed()：成功由 inode/已有 dentry
	 * 接力，失败由 helper 回滚，当前函数不得再次 put。
	 */
	ret = path_from_stashed(&ns->stashed, nsfs_mnt, ns, &path);
	if (ret)
		return ERR_PTR(ret);

	/*
	 * exportfs 只需要 dentry。no_free_ptr() 从自动 path cleanup 中取走
	 * dentry 所有权；path_put 随后只归还 mnt 引用，返回者负责 dput。
	 */
	return no_free_ptr(path.dentry);
}

/*
 * nsfs_export_permission() - exportfs 打开阶段的权限占位回调。
 *
 * ctx/oflags 均为借用输入。身份、状态和跨 namespace 权限已经在
 * fh_to_dentry() 检查，故恒返回 0；不取得引用，也不访问用户内存。
 */
static int nsfs_export_permission(struct handle_to_path_ctx *ctx,
				   unsigned int oflags)
{
	/* nsfs_fh_to_dentry() performs all permission checks. */
	/* 文件句柄恢复回调已经执行全部权限检查，此处不再重复。 */
	return 0;
}

/*
 * nsfs_export_open() - 打开已经过验证的 namespace path。
 *
 * path 为借用；oflags 是用户打开标志。空相对路径表示打开 path 本身。
 * 返回持有引用的 file 或 ERR_PTR，调用者负责 fput。
 */
static struct file *nsfs_export_open(const struct path *path, unsigned int oflags)
{
	return file_open_root(path, "", oflags, 0);
}

/*
 * exportfs 操作表闭合 namespace 文件句柄协议：编码稳定身份、解码并鉴权、
 * 打开恢复出的 path；permission 声明鉴权已在解码阶段完成。
 */
static const struct export_operations nsfs_export_operations = {
	.encode_fh	= nsfs_encode_fh,
	.fh_to_dentry	= nsfs_fh_to_dentry,
	.open		= nsfs_export_open,
	.permission	= nsfs_export_permission,
};

/*
 * nsfs_init_fs_context() - 配置 nsfs 的匿名 pseudo superblock。
 *
 * VFS/kern_mount() 传入新 fc。init_pseudo() 失败返回 -ENOMEM；成功后安装
 * super/export/dentry 操作表，并把 stashed data 回调表放入 s_fs_info。
 * DCACHE_DONTCACHE 允许无外部引用的匿名 dentry 及时回收，其 stashed
 * 指针由 d_prune 清除。返回 0 时 VFS 继续创建内部挂载。
 */
static int nsfs_init_fs_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx = init_pseudo(fc, NSFS_MAGIC);
	if (!ctx)
		return -ENOMEM;
	ctx->s_d_flags |= DCACHE_DONTCACHE;
	ctx->ops = &nsfs_ops;
	ctx->eops = &nsfs_export_operations;
	ctx->dops = &ns_dentry_operations;
	fc->s_fs_info = (void *)&nsfs_stashed_ops;
	return 0;
}

/*
 * nsfs 类型只供内核 kern_mount() 建立单一匿名实例。init 回调配置
 * pseudo fs，kill_anon_super 负责理论卸载；正常运行中由 nsfs_mnt 永久
 * 钉住，不提供可遍历的用户挂载入口。
 */
static struct file_system_type nsfs = {
	.name = "nsfs",
	.init_fs_context = nsfs_init_fs_context,
	.kill_sb = kill_anon_super,
};

/*
 * nsfs_init() - 启动期创建全局 nsfs 内部挂载并发布根 path。
 *
 * 入参：无。返回：无直接返回值。函数位于 __init section，可睡眠且尚无
 * 并发调用者。kern_mount() 失败会使 namespace fd ABI 无法工作，故 panic。
 * 成功后清除 SB_NOUSER，让用户可通过 namespace fd/export handle 使用
 * inode，并初始化之后只读的全局根模板。
 */
void __init nsfs_init(void)
{
	nsfs_mnt = kern_mount(&nsfs);
	if (IS_ERR(nsfs_mnt))
		panic("can't set nsfs up\n");
	nsfs_mnt->mnt_sb->s_flags &= ~SB_NOUSER;
	nsfs_root_path.mnt = nsfs_mnt;
	nsfs_root_path.dentry = nsfs_mnt->mnt_root;
}

/*
 * nsproxy_ns_active_get() - 为任务 namespace 集合取得全部 active 引用。
 *
 * ns 是借用且必须有效的 nsproxy。函数逐项增加 mount/UTS/IPC/child-PID/
 * cgroup/net/time namespace 的 active count；同一对象若出现在两个字段，
 * 仍需按字段对称计数。无返回值，必须与 nsproxy_ns_active_put() 配对。
 * active_get 可能沿 owning user_ns 链复活祖先，但不锁定可变字段。
 */
void nsproxy_ns_active_get(struct nsproxy *ns)
{
	ns_ref_active_get(ns->mnt_ns);
	ns_ref_active_get(ns->uts_ns);
	ns_ref_active_get(ns->ipc_ns);
	ns_ref_active_get(ns->pid_ns_for_children);
	ns_ref_active_get(ns->cgroup_ns);
	ns_ref_active_get(ns->net_ns);
	ns_ref_active_get(ns->time_ns);
	ns_ref_active_get(ns->time_ns_for_children);
}

/*
 * nsproxy_ns_active_put() - 对称归还 nsproxy_ns_active_get() 的全部引用。
 *
 * ns 仍为借用输入；某项计数降为零时可能沿 owning user_ns 链继续退活。
 * 无返回值。active 引用只表达 ownership 树活跃性，对象存储期仍由各类型
 * 的被动引用协议管理。
 */
void nsproxy_ns_active_put(struct nsproxy *ns)
{
	ns_ref_active_put(ns->mnt_ns);
	ns_ref_active_put(ns->uts_ns);
	ns_ref_active_put(ns->ipc_ns);
	ns_ref_active_put(ns->pid_ns_for_children);
	ns_ref_active_put(ns->cgroup_ns);
	ns_ref_active_put(ns->net_ns);
	ns_ref_active_put(ns->time_ns);
	ns_ref_active_put(ns->time_ns_for_children);
}
