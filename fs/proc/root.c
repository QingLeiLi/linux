// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/proc/root.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  proc root directory handling functions
 */
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/sched/stat.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/user_namespace.h>
#include <linux/fs_context.h>
#include <linux/mount.h>
#include <linux/pid_namespace.h>
#include <linux/fs_parser.h>
#include <linux/cred.h>
#include <linux/magic.h>
#include <linux/slab.h>

#include "internal.h"

struct proc_fs_context {
	struct pid_namespace	*pid_ns;
	unsigned int		mask;
	enum proc_hidepid	hidepid;
	int			gid;
	enum proc_pidonly	pidonly;
};

enum proc_param {
	Opt_gid,
	Opt_hidepid,
	Opt_subset,
	Opt_pidns,
};

static const struct fs_parameter_spec proc_fs_parameters[] = {
	fsparam_u32("gid",		Opt_gid),
	fsparam_string("hidepid",	Opt_hidepid),
	fsparam_string("subset",	Opt_subset),
	fsparam_file_or_string("pidns",	Opt_pidns),
	{}
};

static inline int valid_hidepid(unsigned int value)
{
	return (value == HIDEPID_OFF ||
		value == HIDEPID_NO_ACCESS ||
		value == HIDEPID_INVISIBLE ||
		value == HIDEPID_NOT_PTRACEABLE);
}

static int proc_parse_hidepid_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct proc_fs_context *ctx = fc->fs_private;
	struct fs_parameter_spec hidepid_u32_spec = fsparam_u32("hidepid", Opt_hidepid);
	struct fs_parse_result result;
	int base = (unsigned long)hidepid_u32_spec.data;

	if (param->type != fs_value_is_string)
		return invalf(fc, "proc: unexpected type of hidepid value\n");

	if (!kstrtouint(param->string, base, &result.uint_32)) {
		if (!valid_hidepid(result.uint_32))
			return invalf(fc, "proc: unknown value of hidepid - %s\n", param->string);
		ctx->hidepid = result.uint_32;
		return 0;
	}

	if (!strcmp(param->string, "off"))
		ctx->hidepid = HIDEPID_OFF;
	else if (!strcmp(param->string, "noaccess"))
		ctx->hidepid = HIDEPID_NO_ACCESS;
	else if (!strcmp(param->string, "invisible"))
		ctx->hidepid = HIDEPID_INVISIBLE;
	else if (!strcmp(param->string, "ptraceable"))
		ctx->hidepid = HIDEPID_NOT_PTRACEABLE;
	else
		return invalf(fc, "proc: unknown value of hidepid - %s\n", param->string);

	return 0;
}

static int proc_parse_subset_param(struct fs_context *fc, char *value)
{
	struct proc_fs_context *ctx = fc->fs_private;

	while (value) {
		char *ptr = strchr(value, ',');

		if (ptr != NULL)
			*ptr++ = '\0';

		if (*value != '\0') {
			if (!strcmp(value, "pid")) {
				ctx->pidonly = PROC_PIDONLY_ON;
			} else {
				return invalf(fc, "proc: unsupported subset option - %s\n", value);
			}
		}
		value = ptr;
	}

	return 0;
}

#ifdef CONFIG_PID_NS
static int proc_parse_pidns_param(struct fs_context *fc,
				  struct fs_parameter *param,
				  struct fs_parse_result *result)
{
	struct proc_fs_context *ctx = fc->fs_private;
	struct pid_namespace *target, *active = task_active_pid_ns(current);
	struct ns_common *ns;
	struct file *ns_filp __free(fput) = NULL;

	switch (param->type) {
	case fs_value_is_file:
		/* came through fsconfig, steal the file reference */
		ns_filp = no_free_ptr(param->file);
		break;
	case fs_value_is_string:
		ns_filp = filp_open(param->string, O_RDONLY, 0);
		break;
	default:
		WARN_ON_ONCE(true);
		break;
	}
	if (!ns_filp)
		ns_filp = ERR_PTR(-EBADF);
	if (IS_ERR(ns_filp)) {
		errorfc(fc, "could not get file from pidns argument");
		return PTR_ERR(ns_filp);
	}

	if (!proc_ns_file(ns_filp))
		return invalfc(fc, "pidns argument is not an nsfs file");
	ns = get_proc_ns(file_inode(ns_filp));
	if (ns->ns_type != CLONE_NEWPID)
		return invalfc(fc, "pidns argument is not a pidns file");
	target = container_of(ns, struct pid_namespace, ns);

	/*
	 * pidns= is shorthand for joining the pidns to get a fsopen fd, so the
	 * permission model should be the same as pidns_install().
	 */
	if (!ns_capable(target->user_ns, CAP_SYS_ADMIN)) {
		errorfc(fc, "insufficient permissions to set pidns");
		return -EPERM;
	}
	if (!pidns_is_ancestor(target, active))
		return invalfc(fc, "cannot set pidns to non-descendant pidns");

	put_pid_ns(ctx->pid_ns);
	ctx->pid_ns = get_pid_ns(target);
	put_user_ns(fc->user_ns);
	fc->user_ns = get_user_ns(ctx->pid_ns->user_ns);
	return 0;
}
#endif /* CONFIG_PID_NS */

static int proc_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct proc_fs_context *ctx = fc->fs_private;
	struct fs_parse_result result;
	int opt, err;

	opt = fs_parse(fc, proc_fs_parameters, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_gid:
		ctx->gid = result.uint_32;
		break;

	case Opt_hidepid:
		err = proc_parse_hidepid_param(fc, param);
		if (err)
			return err;
		break;

	case Opt_subset:
		err = proc_parse_subset_param(fc, param->string);
		if (err)
			return err;
		break;

	case Opt_pidns:
#ifdef CONFIG_PID_NS
		/*
		 * We would have to RCU-protect every proc_pid_ns() or
		 * proc_sb_info() access if we allowed this to be reconfigured
		 * for an existing procfs instance. Luckily, procfs instances
		 * are cheap to create, and mount-beneath would let you
		 * atomically replace an instance even with overmounts.
		 */
		if (fc->purpose == FS_CONTEXT_FOR_RECONFIGURE) {
			errorfc(fc, "cannot reconfigure pidns for existing procfs");
			return -EBUSY;
		}
		err = proc_parse_pidns_param(fc, param, &result);
		if (err)
			return err;
		break;
#else
		errorfc(fc, "pidns mount flag not supported on this system");
		return -EOPNOTSUPP;
#endif

	default:
		return -EINVAL;
	}

	ctx->mask |= 1 << opt;
	return 0;
}

static int proc_apply_options(struct proc_fs_info *fs_info,
			       struct fs_context *fc,
			       struct user_namespace *user_ns)
{
	struct proc_fs_context *ctx = fc->fs_private;

	if ((ctx->mask & (1 << Opt_subset)) &&
	    fc->purpose == FS_CONTEXT_FOR_RECONFIGURE &&
	    ctx->pidonly != fs_info->pidonly)
		return invalf(fc, "proc: subset=pid cannot be changed\n");

	if (ctx->mask & (1 << Opt_gid))
		fs_info->pid_gid = make_kgid(user_ns, ctx->gid);
	if (ctx->mask & (1 << Opt_hidepid))
		fs_info->hide_pid = ctx->hidepid;
	if (ctx->mask & (1 << Opt_subset))
		fs_info->pidonly = ctx->pidonly;
	if (ctx->mask & (1 << Opt_pidns) &&
	    !WARN_ON_ONCE(fc->purpose == FS_CONTEXT_FOR_RECONFIGURE)) {
		put_pid_ns(fs_info->pid_ns);
		fs_info->pid_ns = get_pid_ns(ctx->pid_ns);
	}
	return 0;
}

static int proc_fill_super(struct super_block *s, struct fs_context *fc)
{
	struct proc_fs_context *ctx = fc->fs_private;
	struct inode *root_inode;
	struct proc_fs_info *fs_info;
	int ret;

	fs_info = kzalloc_obj(*fs_info);
	if (!fs_info)
		return -ENOMEM;

	fs_info->pid_ns = get_pid_ns(ctx->pid_ns);
	fs_info->mounter_cred = get_cred(fc->cred);
	ret = proc_apply_options(fs_info, fc, current_user_ns());
	if (ret)
		return ret;

	/* User space would break if executables or devices appear on proc */
	s->s_iflags |= SB_I_NOEXEC | SB_I_NODEV;
	s->s_flags |= SB_NODIRATIME | SB_NOSUID | SB_NOEXEC;
	s->s_blocksize = 1024;
	s->s_blocksize_bits = 10;
	s->s_magic = PROC_SUPER_MAGIC;
	s->s_op = &proc_sops;
	s->s_time_gran = 1;
	s->s_fs_info = fs_info;

	if (fs_info->pidonly == PROC_PIDONLY_ON)
		s->s_iflags |= SB_I_RESTRICTED_VARIANT;

	/*
	 * procfs isn't actually a stacking filesystem; however, there is
	 * too much magic going on inside it to permit stacking things on
	 * top of it
	 */
	s->s_stack_depth = FILESYSTEM_MAX_STACK_DEPTH;

	/* procfs dentries and inodes don't require IO to create */
	s->s_shrink->seeks = 0;

	pde_get(&proc_root);
	root_inode = proc_get_inode(s, &proc_root);
	if (!root_inode) {
		pr_err("proc_fill_super: get root inode failed\n");
		return -ENOMEM;
	}

	s->s_root = d_make_root(root_inode);
	if (!s->s_root) {
		pr_err("proc_fill_super: allocate dentry failed\n");
		return -ENOMEM;
	}

	ret = proc_setup_self(s);
	if (ret) {
		return ret;
	}
	return proc_setup_thread_self(s);
}

static int proc_reconfigure(struct fs_context *fc)
{
	struct super_block *sb = fc->root->d_sb;
	struct proc_fs_info *fs_info = proc_sb_info(sb);

	sync_filesystem(sb);

	return proc_apply_options(fs_info, fc, current_user_ns());
}

static int proc_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, proc_fill_super);
}

static void proc_fs_context_free(struct fs_context *fc)
{
	struct proc_fs_context *ctx = fc->fs_private;

	put_pid_ns(ctx->pid_ns);
	kfree(ctx);
}

static const struct fs_context_operations proc_fs_context_ops = {
	.free		= proc_fs_context_free,
	.parse_param	= proc_parse_param,
	.get_tree	= proc_get_tree,
	.reconfigure	= proc_reconfigure,
};

static int proc_init_fs_context(struct fs_context *fc)
{
	struct proc_fs_context *ctx;

	ctx = kzalloc_obj(struct proc_fs_context);
	if (!ctx)
		return -ENOMEM;

	ctx->pid_ns = get_pid_ns(task_active_pid_ns(current));
	put_user_ns(fc->user_ns);
	fc->user_ns = get_user_ns(ctx->pid_ns->user_ns);
	fc->fs_private = ctx;
	fc->ops = &proc_fs_context_ops;
	return 0;
}

static void proc_kill_sb(struct super_block *sb)
{
	struct proc_fs_info *fs_info = proc_sb_info(sb);

	kill_anon_super(sb);
	if (fs_info) {
		put_pid_ns(fs_info->pid_ns);
		put_cred(fs_info->mounter_cred);
		kfree_rcu(fs_info, rcu);
	}
}

static struct file_system_type proc_fs_type = {
	.name			= "proc",
	.init_fs_context	= proc_init_fs_context,
	.parameters		= proc_fs_parameters,
	.kill_sb		= proc_kill_sb,
	.fs_flags		= FS_USERNS_MOUNT | FS_USERNS_MOUNT_RESTRICTED | FS_DISALLOW_NOTIFY_PERM,
};

void __init proc_root_init(void)
{
	/*
	 * 创建 procfs 专用的三个 slab 缓存（fs/proc/inode.c）：
	 *   proc_inode_cachep    — struct proc_inode（VFS inode + proc 私有字段合体），
	 *                          每个 /proc 文件背后都有一个，SLAB_RECLAIM_ACCOUNT
	 *                          使其可被 shrinker 在内存压力下回收；
	 *   pde_opener_cache     — struct pde_opener，跟踪 /proc 文件的打开实例，
	 *                          用于在 proc_dir_entry 被移除时等待所有 reader 退出；
	 *   proc_dir_entry_cache — struct proc_dir_entry（PDE），描述一个 /proc 节点
	 *                          的元数据（名称、权限、ops 指针等），
	 *                          kmem_cache_create_usercopy 标记 inline_name 字段
	 *                          可安全复制到用户空间（HARDENED_USERCOPY 合规）。
	 */
	proc_init_kmemcache();

	/*
	 * 预计算 /proc/<pid>/ 和 /proc/<pid>/task/<tid>/ 目录的 nlink 数量。
	 * nlink 等于子目录数 + 2（"." 和 ".."），而子目录数由
	 * tgid_base_stuff / tid_base_stuff 数组中类型为目录的条目数决定。
	 * 预计算结果存入 nlink_tgid / nlink_tid，避免每次 stat() 重新遍历数组。
	 */
	set_proc_pid_nlink();

	/*
	 * 为 /proc/self 预分配 inode 编号（self_inum）。
	 * /proc/self 是指向当前进程 /proc/<pid> 的符号链接；
	 * inode 编号固定后，readlink("/proc/self") 才能稳定返回目标路径。
	 */
	proc_self_init();

	/*
	 * 为 /proc/thread-self 预分配 inode 编号（thread_self_inum）。
	 * /proc/thread-self 指向当前线程的 /proc/<pid>/task/<tid>，
	 * 多线程程序通过它读取本线程的 stat/status 而无需知道自己的 tid。
	 */
	proc_thread_self_init();

	/*
	 * 创建 /proc/mounts → self/mounts 的符号链接。
	 * 用户态工具（mount(8)、df(1) 等）通过 /proc/mounts 读取
	 * 当前挂载表，实际内容由 /proc/<pid>/mounts 的 seq_file 生成。
	 */
	proc_symlink("mounts", NULL, "self/mounts");

	/*
	 * 初始化 /proc/net 目录框架。
	 * /proc/net 是每网络命名空间独立的目录，内容随 net namespace 切换。
	 * 这里只建立框架和 pernet_operations 注册机制；具体条目（如
	 * /proc/net/dev、/proc/net/tcp）由各网络子系统后续注册。
	 */
	proc_net_init();

	/* 建立 /proc/fs 目录：各文件系统注册自己的调试/统计节点的挂载点 */
	proc_mkdir("fs", NULL);
	/* 建立 /proc/driver 目录：驱动程序注册自己的状态节点 */
	proc_mkdir("driver", NULL);
	/* 为 nfsd 文件系统预留挂载点 /proc/fs/nfsd，nfsd 模块加载时在此挂载 */
	proc_create_mount_point("fs/nfsd");
#if defined(CONFIG_SUN_OPENPROMFS) || defined(CONFIG_SUN_OPENPROMFS_MODULE)
	/* SPARC OpenPROM 文件系统的挂载点，ARM64 上此分支不编译 */
	proc_create_mount_point("openprom");
#endif

	/*
	 * 初始化 /proc/tty 目录，注册 TTY 驱动信息节点：
	 *   /proc/tty/drivers  — 已注册的 TTY 驱动列表；
	 *   /proc/tty/ldiscs   — 已注册的线路规程（line discipline）列表。
	 * ARM64 的串口控制台（如 ttyAMA0）依赖这里的注册信息。
	 */
	proc_tty_init();

	/* 建立 /proc/bus 目录：USB、PCI 等总线子系统的设备枚举节点 */
	proc_mkdir("bus", NULL);

	/*
	 * 初始化 /proc/sys 目录并调用 sysctl_init_bases()。
	 * sysctl 是内核参数的运行时读写接口：
	 *   /proc/sys/kernel/  — 进程、调度、崩溃等核心参数；
	 *   /proc/sys/vm/      — 内存管理参数（swappiness、dirty_ratio 等）；
	 *   /proc/sys/net/     — 网络栈参数（tcp_congestion_control 等）。
	 * 此后 sysctl_register_table() 可用，驱动可注册自己的 sysctl 节点。
	 */
	proc_sys_init();

	/*
	 * 最后一步：将 proc_fs_type 注册到 VFS 文件系统表。
	 * 只有注册后，"mount -t proc proc /proc" 才能找到这个文件系统类型。
	 * 故意放在最后：确保所有内部数据结构（缓存、目录、inode 编号）
	 * 完全就绪后，外部才能访问 /proc，避免竞争窗口。
	 */
	register_filesystem(&proc_fs_type);
}

static int proc_root_getattr(struct mnt_idmap *idmap,
			     const struct path *path, struct kstat *stat,
			     u32 request_mask, unsigned int query_flags)
{
	generic_fillattr(&nop_mnt_idmap, request_mask, d_inode(path->dentry),
			 stat);
	stat->nlink = proc_root.nlink + nr_processes();
	return 0;
}

static struct dentry *proc_root_lookup(struct inode * dir, struct dentry * dentry, unsigned int flags)
{
	if (!proc_pid_lookup(dentry, flags))
		return NULL;

	return proc_lookup(dir, dentry, flags);
}

static int proc_root_readdir(struct file *file, struct dir_context *ctx)
{
	if (ctx->pos < FIRST_PROCESS_ENTRY) {
		int error = proc_readdir(file, ctx);
		if (unlikely(error <= 0))
			return error;
		ctx->pos = FIRST_PROCESS_ENTRY;
	}

	return proc_pid_readdir(file, ctx);
}

/*
 * The root /proc directory is special, as it has the
 * <pid> directories. Thus we don't use the generic
 * directory handling functions for that..
 */
static const struct file_operations proc_root_operations = {
	.read		 = generic_read_dir,
	.iterate_shared	 = proc_root_readdir,
	.llseek		= generic_file_llseek,
};

/*
 * proc root can do almost nothing..
 */
static const struct inode_operations proc_root_inode_operations = {
	.lookup		= proc_root_lookup,
	.getattr	= proc_root_getattr,
};

/*
 * This is the root "inode" in the /proc tree..
 */
struct proc_dir_entry proc_root = {
	.low_ino	= PROCFS_ROOT_INO,
	.namelen	= 5,
	.mode		= S_IFDIR | S_IRUGO | S_IXUGO,
	.nlink		= 2,
	.refcnt		= REFCOUNT_INIT(1),
	.proc_iops	= &proc_root_inode_operations,
	.proc_dir_ops	= &proc_root_operations,
	.parent		= &proc_root,
	.subdir		= RB_ROOT,
	.name		= "/proc",
};
