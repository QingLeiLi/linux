// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/fs/file_table.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  Copyright (C) 1997 David S. Miller (davem@caip.rutgers.edu)
 */

#include <linux/string.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/filelock.h>
#include <linux/security.h>
#include <linux/cred.h>
#include <linux/eventpoll.h>
#include <linux/rcupdate.h>
#include <linux/mount.h>
#include <linux/capability.h>
#include <linux/cdev.h>
#include <linux/fsnotify.h>
#include <linux/sysctl.h>
#include <linux/percpu_counter.h>
#include <linux/percpu.h>
#include <linux/task_work.h>
#include <linux/swap.h>
#include <linux/kmemleak.h>

#include <linux/atomic.h>

#include <asm/runtime-const.h>

#include "internal.h"

/*
 * 本文件管理 struct file 的对象生命期，而不是进程文件描述符表本身：alloc_*
 * 建立引用为 1 的 file，fput 消费引用；最后一份引用经 task_work 或 delayed
 * work 进入 __fput()，依次拆除 epoll、锁、驱动、dentry、mount、凭据和 SLAB。
 * file cache 使用 SLAB_TYPESAFE_BY_RCU，复用对象必须先完整初始化、最后发布引用。
 */

/* sysctl tunables... */
/* files_stat 提供 file-max/file-nr 等全局容量视图。 */
static struct files_stat_struct files_stat = {
	.max_files = NR_FILE
};

/* SLAB cache for file structures */
/* 普通 file 与带 user_path/security 的 backing_file 分池，启动后缓存指针只读。 */
static struct kmem_cache *__filp_cache __ro_after_init;
#define filp_cache runtime_const_ptr(__filp_cache)
static struct kmem_cache *__bfilp_cache __ro_after_init;
#define bfilp_cache runtime_const_ptr(__bfilp_cache)

static struct percpu_counter nr_files __cacheline_aligned_in_smp;

/* Container for backing file with optional user path */
/* 内嵌 file 的扩展容器，额外保存面向用户的路径及 LSM 私有状态。 */
struct backing_file {
	struct file file;
	union {
		struct path user_path;
		freeptr_t bf_freeptr;
	};
#ifdef CONFIG_SECURITY
	void *security;
#endif
};

#define backing_file(f) container_of(f, struct backing_file, file)

const struct path *backing_file_user_path(const struct file *f)
{
	/* 返回容器内借用指针，不增加 path 引用。 */
	return &backing_file(f)->user_path;
}
EXPORT_SYMBOL_GPL(backing_file_user_path);

void backing_file_set_user_path(struct file *f, const struct path *path)
{
	/* 仅复制 path 值；调用者负责转移对应 dentry/mount 引用。 */
	backing_file(f)->user_path = *path;
}
EXPORT_SYMBOL_GPL(backing_file_set_user_path);

#ifdef CONFIG_SECURITY
void *backing_file_security(const struct file *f)
{
	/* 私有指针所有权由 security_backing_file_alloc/free 协议管理。 */
	return backing_file(f)->security;
}

void backing_file_set_security(struct file *f, void *security)
{
	/* 设置 LSM 私有状态，本辅助函数不释放旧值。 */
	backing_file(f)->security = security;
}
#endif /* CONFIG_SECURITY */

static inline void backing_file_free(struct backing_file *ff)
{
	/* 先通知 LSM，再放 user_path，最后释放包含内嵌 file 的容器。 */
	security_backing_file_free(&ff->file);
	path_put(&ff->user_path);
	kmem_cache_free(bfilp_cache, ff);
}

static inline void file_free(struct file *f)
{
	/* 通用尾部释放：撤销 LSM、全局数量和凭据，再按容器类型归还正确 SLAB。 */
	security_file_free(f);
	if (likely(!(f->f_mode & FMODE_NOACCOUNT)))
		percpu_counter_dec(&nr_files);
	put_cred(f->f_cred);
	if (unlikely(f->f_mode & FMODE_BACKING)) {
		backing_file_free(backing_file(f));
	} else {
		kmem_cache_free(filp_cache, f);
	}
}

/*
 * Return the total number of open files in the system
 * percpu 快速读允许小幅误差，并把负的瞬时聚合值截为零，适合容量快筛。
 */
static long get_nr_files(void)
{
	return percpu_counter_read_positive(&nr_files);
}

/*
 * Return the maximum number of open files in the system
 * 返回可由 sysctl 更新的系统级 struct file 软上限。
 */
unsigned long get_max_files(void)
{
	return files_stat.max_files;
}
EXPORT_SYMBOL_GPL(get_max_files);

#if defined(CONFIG_SYSCTL) && defined(CONFIG_PROC_FS)

/*
 * Handle nr_files sysctl
 * 读取 file-nr 前用较昂贵的全 CPU 精确求和刷新兼容结构。
 */
static int proc_nr_files(const struct ctl_table *table, int write, void *buffer,
			 size_t *lenp, loff_t *ppos)
{
	files_stat.nr_files = percpu_counter_sum_positive(&nr_files);
	return proc_doulongvec_minmax(table, write, buffer, lenp, ppos);
}

static const struct ctl_table fs_stat_sysctls[] = {
	{
		.procname	= "file-nr",
		.data		= &files_stat,
		.maxlen		= sizeof(files_stat),
		.mode		= 0444,
		.proc_handler	= proc_nr_files,
	},
	{
		.procname	= "file-max",
		.data		= &files_stat.max_files,
		.maxlen		= sizeof(files_stat.max_files),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
		.extra1		= SYSCTL_LONG_ZERO,
		.extra2		= SYSCTL_LONG_MAX,
	},
	{
		.procname	= "nr_open",
		.data		= &sysctl_nr_open,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &sysctl_nr_open_min,
		.extra2		= &sysctl_nr_open_max,
	},
};

static int __init init_fs_stat_sysctls(void)
{
	/* 注册 fs/file-*；binfmt_misc 分支只预建可挂载的 sysctl 目录节点。 */
	register_sysctl_init("fs", fs_stat_sysctls);
	if (IS_ENABLED(CONFIG_BINFMT_MISC)) {
		struct ctl_table_header *hdr;

		hdr = register_sysctl_mount_point("fs/binfmt_misc");
		kmemleak_not_leak(hdr);
	}
	return 0;
}
fs_initcall(init_fs_stat_sysctls);
#endif

static int init_file(struct file *f, int flags, const struct cred *cred)
{
	/*
	 * 初始化尚未关联 path/f_op 的空 file。f_cred 固定打开时凭据；LSM 失败原样
	 * 回滚。对象可在 RCU 读者仍持旧地址时被复用，所以 f_ref 必须最后发布。
	 */
	int error;

	f->f_cred = get_cred(cred);
	error = security_file_alloc(f);
	if (unlikely(error)) {
		put_cred(f->f_cred);
		return error;
	}

	spin_lock_init(&f->f_lock);
	/*
	 * Note that f_pos_lock is only used for files raising
	 * FMODE_ATOMIC_POS and directories. Other files such as pipes
	 * don't need it and since f_pos_lock is in a union may reuse
	 * the space for other purposes. They are expected to initialize
	 * the respective member when opening the file.
	 * f_pos_lock 与其他字段共用 union；复用该空间的文件类型须在 open 时初始化。
	 */
	mutex_init(&f->f_pos_lock);
	memset(&f->__f_path, 0, sizeof(f->f_path));
	memset(&f->f_ra, 0, sizeof(f->f_ra));

	f->f_flags	= flags;
	f->f_mode	= OPEN_FMODE(flags);
	/*
	 * Disable permission and pre-content events for all files by default.
	 * They may be enabled later by fsnotify_open_perm_and_set_mode().
	 * 空对象尚未完成打开，默认禁止前置通知，避免过早产生 fsnotify 事件。
	 */
	file_set_fsnotify_mode(f, FMODE_NONOTIFY_PERM);

	f->f_op		= NULL;
	f->f_mapping	= NULL;
	f->private_data = NULL;
	f->f_inode	= NULL;
	f->f_owner	= NULL;
#ifdef CONFIG_EPOLL
	f->f_ep		= NULL;
#endif

	f->f_iocb_flags = 0;
	f->f_pos	= 0;
	f->f_wb_err	= 0;
	f->f_sb_err	= 0;

	/*
	 * We're SLAB_TYPESAFE_BY_RCU so initialize f_ref last. While
	 * fget-rcu pattern users need to be able to handle spurious
	 * refcount bumps we should reinitialize the reused file first.
	 * SLAB_TYPESAFE_BY_RCU 只保证内存不立即换类型；先重置可见字段，再发布引用 1。
	 */
	file_ref_init(&f->f_ref, 1);
	return 0;
}

/* Find an unused file structure and return a pointer to it.
 * Returns an error pointer if some error happened, e.g., we exceed the file
 * structures limit, run out of memory or operation is not permitted.
 *
 * Be very careful using this.  You are responsible for
 * getting write access to any mount that you might assign
 * to this filp, if it is opened for write.  If this is not
 * done, the mount's writer count will be wrong
 * and a warning at __fput() time.
 *
 * 普通分配计入 nr_files 并作两阶段上限检查：percpu 近似值用于快筛，真正拒绝
 * 前精确求和。CAP_SYS_ADMIN 可越过 file-max；内存或 LSM 失败返回 ERR_PTR。
 */
struct file *alloc_empty_file(int flags, const struct cred *cred)
{
	static long old_max;
	struct file *f;
	int error;

	/*
	 * Privileged users can go above max_files
	 * 管理员可为系统恢复等场景突破上限，但仍受内存分配约束。
	 */
	if (unlikely(get_nr_files() >= files_stat.max_files) &&
	    !capable(CAP_SYS_ADMIN)) {
		/*
		 * percpu_counters are inaccurate.  Do an expensive check before
		 * we go and fail.
		 * 拒绝前精确求和，避免 percpu 批量误差造成误报 ENFILE。
		 */
		if (percpu_counter_sum_positive(&nr_files) >= files_stat.max_files)
			goto over;
	}

	f = kmem_cache_alloc(filp_cache, GFP_KERNEL);
	if (unlikely(!f))
		return ERR_PTR(-ENOMEM);

	error = init_file(f, flags, cred);
	if (unlikely(error)) {
		kmem_cache_free(filp_cache, f);
		return ERR_PTR(error);
	}

	percpu_counter_inc(&nr_files);

	return f;

over:
	/* Ran out of filps - report that */
	/* 仅在新的历史高点打印，避免持续超限导致日志风暴。 */
	if (get_nr_files() > old_max) {
		pr_info("VFS: file-max limit %lu reached\n", get_max_files());
		old_max = get_nr_files();
	}
	return ERR_PTR(-ENFILE);
}

/*
 * Variant of alloc_empty_file() that doesn't check and modify nr_files.
 *
 * This is only for kernel internal use, and the allocate file must not be
 * installed into file tables or such.
 * 内核私有临时 file 设置 FMODE_NOACCOUNT，不得安装进用户 fdtable。
 */
struct file *alloc_empty_file_noaccount(int flags, const struct cred *cred)
{
	struct file *f;
	int error;

	f = kmem_cache_alloc(filp_cache, GFP_KERNEL);
	if (unlikely(!f))
		return ERR_PTR(-ENOMEM);

	error = init_file(f, flags, cred);
	if (unlikely(error)) {
		kmem_cache_free(filp_cache, f);
		return ERR_PTR(error);
	}

	f->f_mode |= FMODE_NOACCOUNT;

	return f;
}

static int init_backing_file(struct backing_file *ff,
			     const struct file *user_file)
{
	/* 先建立无路径、无安全私有数据的可回滚状态，再调用 LSM 分配。 */
	memset(&ff->user_path, 0, sizeof(ff->user_path));
	backing_file_set_security(&ff->file, NULL);
	return security_backing_file_alloc(&ff->file, user_file);
}

/*
 * Variant of alloc_empty_file() that allocates a backing_file container
 * and doesn't check and modify nr_files.
 *
 * This is only for kernel internal use, and the allocate file must not be
 * installed into file tables or such.
 * backing_file 供代理文件保存用户可见 path，不计 nr_files，也不得安装进 fdtable。
 */
struct file *alloc_empty_backing_file(int flags, const struct cred *cred,
				      const struct file *user_file)
{
	struct backing_file *ff;
	int error;

	ff = kmem_cache_alloc(bfilp_cache, GFP_KERNEL);
	if (unlikely(!ff))
		return ERR_PTR(-ENOMEM);

	error = init_file(&ff->file, flags, cred);
	if (unlikely(error)) {
		kmem_cache_free(bfilp_cache, ff);
		return ERR_PTR(error);
	}

	/* The f_mode flags must be set before fput(). */
	/* 失败会调用 fput，须先发布容器类型与 NOACCOUNT 标志以选择正确析构。 */
	ff->file.f_mode |= FMODE_BACKING | FMODE_NOACCOUNT;
	error = init_backing_file(ff, user_file);
	if (unlikely(error)) {
		fput(&ff->file);
		return ERR_PTR(error);
	}

	return &ff->file;
}
EXPORT_SYMBOL_GPL(alloc_empty_backing_file);

/**
 * file_init_path - initialize a 'struct file' based on path
 *
 * @file: the file to set up
 * @path: the (dentry, vfsmount) pair for the new file
 * @fop: the 'struct file_operations' for the new file
 *
 * 安装调用者已持有引用的 path/fops，采样写回错误，设置能力位，
 * 最后以 FMODE_OPENED 声明 __fput() 应执行完整拆除。
 */
static void file_init_path(struct file *file, const struct path *path,
			   const struct file_operations *fop)
{
	file->__f_path = *path;
	file->f_inode = path->dentry->d_inode;
	file->f_mapping = path->dentry->d_inode->i_mapping;
	file->f_wb_err = filemap_sample_wb_err(file->f_mapping);
	file->f_sb_err = file_sample_sb_err(file);
	if (fop->llseek)
		file->f_mode |= FMODE_LSEEK;
	if ((file->f_mode & FMODE_READ) &&
	     likely(fop->read || fop->read_iter))
		file->f_mode |= FMODE_CAN_READ;
	if ((file->f_mode & FMODE_WRITE) &&
	     likely(fop->write || fop->write_iter))
		file->f_mode |= FMODE_CAN_WRITE;
	file->f_iocb_flags = iocb_flags(file);
	file->f_mode |= FMODE_OPENED;
	file->f_op = fop;
	if ((file->f_mode & (FMODE_READ | FMODE_WRITE)) == FMODE_READ)
		i_readcount_inc(path->dentry->d_inode);
}

/**
 * alloc_file - allocate and initialize a 'struct file'
 *
 * @path: the (dentry, vfsmount) pair for the new file
 * @flags: O_... flags with which the new file will be opened
 * @fop: the 'struct file_operations' for the new file
 *
 * 分配普通计数 file 并关联 path；path 引用由调用者准备并转移。
 */
static struct file *alloc_file(const struct path *path, int flags,
		const struct file_operations *fop)
{
	struct file *file;

	file = alloc_empty_file(flags, current_cred());
	if (!IS_ERR(file))
		file_init_path(file, path, fop);
	return file;
}

static inline int alloc_path_pseudo(const char *name, struct inode *inode,
				    struct vfsmount *mnt, struct path *path)
{
	/* 为匿名/伪文件创建 dentry+mount path；目录 inode 不适用此接口。 */
	if (WARN_ON_ONCE(S_ISDIR(inode->i_mode)))
		return -EINVAL;
	path->dentry = d_alloc_pseudo(mnt->mnt_sb, &QSTR(name));
	if (!path->dentry)
		return -ENOMEM;
	path->mnt = mntget(mnt);
	d_instantiate(path->dentry, inode);
	return 0;
}

struct file *alloc_file_pseudo(struct inode *inode, struct vfsmount *mnt,
			       const char *name, int flags,
			       const struct file_operations *fops)
{
	/* 创建计入 file-nr 的伪文件；任一步失败都平衡 path/inode 生命周期。 */
	int ret;
	struct path path;
	struct file *file;

	ret = alloc_path_pseudo(name, inode, mnt, &path);
	if (ret)
		return ERR_PTR(ret);

	file = alloc_file(&path, flags, fops);
	if (IS_ERR(file)) {
		ihold(inode);
		path_put(&path);
		return file;
	}
	/*
	 * Disable all fsnotify events for pseudo files by default.
	 * They may be enabled by caller with file_set_fsnotify_mode().
	 * 未计数伪文件同样默认关闭通知，调用者确有语义需求时再开启。
	 * 伪文件通常无用户路径语义，默认关闭通知，调用者可按需重新开启。
	 */
	file_set_fsnotify_mode(file, FMODE_NONOTIFY);
	return file;
}
EXPORT_SYMBOL(alloc_file_pseudo);

struct file *alloc_file_pseudo_noaccount(struct inode *inode,
					 struct vfsmount *mnt, const char *name,
					 int flags,
					 const struct file_operations *fops)
{
	/* 与 alloc_file_pseudo 相同，但使用未计入 nr_files 的内核私有 file。 */
	int ret;
	struct path path;
	struct file *file;

	ret = alloc_path_pseudo(name, inode, mnt, &path);
	if (ret)
		return ERR_PTR(ret);

	file = alloc_empty_file_noaccount(flags, current_cred());
	if (IS_ERR(file)) {
		ihold(inode);
		path_put(&path);
		return file;
	}
	file_init_path(file, &path, fops);
	/*
	 * Disable all fsnotify events for pseudo files by default.
	 * They may be enabled by caller with file_set_fsnotify_mode().
	 */
	file_set_fsnotify_mode(file, FMODE_NONOTIFY);
	return file;
}
EXPORT_SYMBOL_GPL(alloc_file_pseudo_noaccount);

struct file *alloc_file_clone(struct file *base, int flags,
				const struct file_operations *fops)
{
	/* 克隆 path/mapping，但建立独立 file 状态；path_get 为新对象取得引用。 */
	struct file *f;

	f = alloc_file(&base->f_path, flags, fops);
	if (!IS_ERR(f)) {
		path_get(&f->f_path);
		f->f_mapping = base->f_mapping;
	}
	return f;
}

/* the real guts of fput() - releasing the last reference to file
 * 最后一份引用的真正析构，可睡眠。先拆 epoll/锁/驱动，再归还操作表、path、
 * mount 和 file 本体，避免资源释放后仍存在可达回调。
 */
static void __fput(struct file *file)
{
	struct dentry *dentry = file->f_path.dentry;
	struct vfsmount *mnt = file->f_path.mnt;
	struct inode *inode = file->f_inode;
	fmode_t mode = file->f_mode;

	if (unlikely(!(file->f_mode & FMODE_OPENED)))
		goto out;

	might_sleep();

	fsnotify_close(file);
	/*
	 * The function eventpoll_release() should be the first called
	 * in the file cleanup chain.
	 * epoll 先摘除所有观察关系，后续清理才不会留下悬空回调。
	 */
	eventpoll_release(file);
	locks_remove_file(file);

	security_file_release(file);
	if (unlikely(file->f_flags & FASYNC)) {
		if (file->f_op->fasync)
			file->f_op->fasync(-1, file, 0);
	}
	if (file->f_op->release)
		file->f_op->release(inode, file);
	if (unlikely(S_ISCHR(inode->i_mode) && inode->i_cdev != NULL &&
		     !(mode & FMODE_PATH))) {
		cdev_put(inode->i_cdev);
	}
	fops_put(file->f_op);
	file_f_owner_release(file);
	put_file_access(file);
	dput(dentry);
	if (unlikely(mode & FMODE_NEED_UNMOUNT))
		dissolve_on_fput(mnt);
	mntput(mnt);
out:
	file_free(file);
}

static LLIST_HEAD(delayed_fput_list);
static void delayed_fput(struct work_struct *unused)
{
	/* 原子取走整条无锁链表；并发新加入者会重新安排 delayed work。 */
	struct llist_node *node = llist_del_all(&delayed_fput_list);
	struct file *f, *t;

	llist_for_each_entry_safe(f, t, node, f_llist)
		__fput(f);
}

static void ____fput(struct callback_head *work)
{
	/* 从内嵌 task_work 节点还原 file，在安全的任务返回/退出点析构。 */
	__fput(container_of(work, struct file, f_task_work));
}

static DECLARE_DELAYED_WORK(delayed_fput_work, delayed_fput);

/*
 * If kernel thread really needs to have the final fput() it has done
 * to complete, call this.  The only user right now is the boot - we
 * *do* need to make sure our writes to binaries on initramfs has
 * not left us with opened struct file waiting for __fput() - execve()
 * won't work without that.  Please, don't add more callers without
 * very good reasons; in particular, never call that with locks
 * held and never call that from a thread that might need to do
 * some work on any kind of umount.
 * 这是全局冲刷屏障；持锁或参与卸载的线程调用可能与 __fput() 形成死锁。
 */
void flush_delayed_fput(void)
{
	delayed_fput(NULL);
	flush_delayed_work(&delayed_fput_work);
}
EXPORT_SYMBOL_GPL(flush_delayed_fput);

static void __fput_deferred(struct file *file)
{
	/*
	 * 用户任务优先使用自己的 task_work；中断、内核线程或已越过 task_work 清理点
	 * 的退出任务退化到全局 delayed work。未 OPENED 的普通空对象可直接释放。
	 */
	struct task_struct *task = current;

	if (unlikely(!(file->f_mode & (FMODE_BACKING | FMODE_OPENED)))) {
		file_free(file);
		return;
	}

	if (likely(!in_interrupt() && !(task->flags & PF_KTHREAD))) {
		init_task_work(&file->f_task_work, ____fput);
		if (!task_work_add(task, &file->f_task_work, TWA_RESUME))
			return;
		/*
		 * After this task has run exit_task_work(),
		 * task_work_add() will fail.  Fall through to delayed
		 * fput to avoid leaking *file.
		 * 添加失败说明退出清理点已过，必须转全局队列避免泄漏。
		 */
	}

	if (llist_add(&file->f_llist, &delayed_fput_list))
		schedule_delayed_work(&delayed_fput_work, 1);
}

/*
 * fput() - 释放调用者持有的一份 struct file 引用。
 *
 * @file: 必须是调用者持有引用的非 NULL file；调用后该引用被消费，调用者
 *        不得继续解引用它。
 *
 * 普通减引用不会改变其他持有者的使用；降到最后一份时也不在当前任意
 * 上下文直接执行完整 __fput()，而由 __fput_deferred() 选择 task_work 或
 * delayed work，避免在中断/不安全锁上下文中执行可能睡眠的文件系统释放。
 * 返回无直接值。taskstats 的 exe_add_tsk() 用它与 get_task_exe_file()
 * 配对，保证只在读取设备号和 inode 的窗口内延长 exe file 生命周期。
 */
void fput(struct file *file)
{
	if (unlikely(file_ref_put(&file->f_ref)))
		__fput_deferred(file);
}
EXPORT_SYMBOL(fput);

/*
 * synchronous analog of fput(); for kernel threads that might be needed
 * in some umount() (and thus can't use flush_delayed_fput() without
 * risking deadlocks), need to wait for completion of __fput() and know
 * for this specific struct file it won't involve anything that would
 * need them.  Use only if you really need it - at the very least,
 * don't blindly convert fput() by kernel thread to that.
 * 同步析构留在当前上下文；调用者须确认不会与该 file 的释放/卸载依赖互锁。
 */
void __fput_sync(struct file *file)
{
	if (file_ref_put(&file->f_ref))
		__fput(file);
}
EXPORT_SYMBOL(__fput_sync);

/*
 * Equivalent to __fput_sync(), but optimized for being called with the last
 * reference.
 *
 * See file_ref_put_close() for details.
 * 已知接近最后引用时使用 close 优化减引用协议，并同步完成析构。
 */
void fput_close_sync(struct file *file)
{
	if (likely(file_ref_put_close(&file->f_ref)))
		__fput(file);
}

/*
 * Equivalent to fput(), but optimized for being called with the last
 * reference.
 *
 * See file_ref_put_close() for details.
 * 已知接近最后引用但仍需上下文安全性时，优化减引用后走延迟析构。
 */
void fput_close(struct file *file)
{
	if (file_ref_put_close(&file->f_ref))
		__fput_deferred(file);
}

void __init files_init(void)
{
	/* 建立普通/后备 file 的 RCU 类型安全缓存及全局 percpu 计数器。 */
	struct kmem_cache_args args = {
		.use_freeptr_offset = true,
		.freeptr_offset = offsetof(struct file, f_freeptr),
	};

	__filp_cache = kmem_cache_create("filp", sizeof(struct file), &args,
				SLAB_HWCACHE_ALIGN | SLAB_PANIC |
				SLAB_ACCOUNT | SLAB_TYPESAFE_BY_RCU);
	runtime_const_init(ptr, __filp_cache);

	args.freeptr_offset = offsetof(struct backing_file, bf_freeptr);
	__bfilp_cache = kmem_cache_create("bfilp", sizeof(struct backing_file),
				&args, SLAB_HWCACHE_ALIGN | SLAB_PANIC |
				SLAB_ACCOUNT | SLAB_TYPESAFE_BY_RCU);
	runtime_const_init(ptr, __bfilp_cache);

	percpu_counter_init(&nr_files, 0, GFP_KERNEL);
}

/*
 * One file with associated inode and dcache is very roughly 1K. Per default
 * do not use more than 10% of our memory for files.
 * 按每个 file 连同 inode/dcache 约 1 KiB 估算，把默认上限控制在可用内存约
 * 10%，同时不低于 NR_FILE。
 */
void __init files_maxfiles_init(void)
{
	unsigned long n;
	unsigned long nr_pages = totalram_pages();
	unsigned long memreserve = (nr_pages - nr_free_pages()) * 3/2;

	memreserve = min(memreserve, nr_pages - 1);
	n = ((nr_pages - memreserve) * (PAGE_SIZE / 1024)) / 10;

	files_stat.max_files = max_t(unsigned long, n, NR_FILE);
}
