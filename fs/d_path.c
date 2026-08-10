/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 路径字符串生成的核心实现。
 *
 * 学习主线：
 * 1. VFS 中的路径由 { vfsmount, dentry } 共同定位：dentry 描述文件系统内的
 *    父子关系，mount 则负责跨越挂载点；因此完整路径回溯不能只沿 d_parent。
 * 2. 本文件从调用者缓冲区的末尾向前写入各级名字。这样向父目录回溯时无需
 *    预先知道最终长度，也不需要在每增加一级目录后搬移已经生成的字符串。
 * 3. rename 和 mount 变化通过 RCU + 序列锁乐观读取；若序列号变化，就丢弃
 *    本轮缓冲区视图并重试，必要时退化为持有读锁的稳定遍历。
 * 4. 对外函数通常返回指向缓冲区中部的指针，而不保证返回原始 buf；调用者
 *    必须使用返回值。空间不足统一记录为负 len，最后转换为 -ENAMETOOLONG。
 *
 * 关键调用关系：
 *   d_path()/__d_path()/getcwd()
 *          -> prepend_path()
 *              -> __prepend_path()
 *                  -> prepend_name() -> prepend()
 *   dentry_path[_raw]() -> __dentry_path() -> prepend_name()
 *
 * 生命周期与所有权：所有输出内存均由调用者提供；本文件不把 dentry/path 的
 * 引用转交给调用者。RCU 区间内取得的 root、pwd 和名字指针都只是借用快照。
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5，2026-08-10）
 */
#include <linux/syscalls.h>
#include <linux/export.h>
#include <linux/uaccess.h>
#include <linux/fs_struct.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/prefetch.h>
#include "mount.h"
#include "internal.h"

/*
 * 反向构造字符串时的游标。
 *
 * buf 指向当前已生成字符串的首部，初始值是存储区末端后一字节；len 是 buf
 * 前方尚可使用的字节数。len 一旦变成负数，表示此前已经溢出，后续辅助函数
 * 只传播失败状态，不再把它恢复为正常值。
 */
struct prepend_buffer {
	char *buf;
	int len;
};
/* 在现有数组尾端创建一个反向写入游标；该宏不分配内存。 */
#define DECLARE_BUFFER(__name, __buf, __len) \
	struct prepend_buffer __name = {.buf = __buf + __len, .len = __len}

/*
 * 把内部游标转换为公共返回约定。
 * 成功时返回实际字符串首地址（可能位于原缓冲区中部），溢出时返回错误指针。
 */
static char *extract_string(struct prepend_buffer *p)
{
	if (likely(p->len >= 0))
		return p->buf;
	return ERR_PTR(-ENAMETOOLONG);
}

/*
 * 在当前字符串前插入一个字节。
 * 写入顺序必须先减少剩余空间、再前移 buf；没有空间时以 len == -1 粘住错误。
 */
static bool prepend_char(struct prepend_buffer *p, unsigned char c)
{
	if (likely(p->len > 0)) {
		p->len--;
		*--p->buf = c;
		return true;
	}
	p->len = -1;
	return false;
}

/*
 * The source of the prepend data can be an optimistic load
 * of a dentry name and length. And because we don't hold any
 * locks, the length and the pointer to the name may not be
 * in sync if a concurrent rename happens, and the kernel
 * copy might fault as a result.
 *
 * The end result will correct itself when we check the
 * rename sequence count, but we need to be able to handle
 * the fault gracefully.
 */
/*
 * RCU 路径遍历可能分别读到 rename 前后的 name 指针和长度。这里使用
 * copy_from_kernel_nofault()，避免这个短暂的不一致演变成内核故障。复制失败时
 * 填入占位字节只是为了保持缓冲区处于可控状态；调用者随后检查 rename_lock
 * 序列号并丢弃本轮结果，因此占位内容不会成为最终路径。
 */
static bool prepend_copy(void *dst, const void *src, int len)
{
	if (unlikely(copy_from_kernel_nofault(dst, src, len))) {
		memset(dst, 'x', len);
		return false;
	}
	return true;
}

/*
 * 在路径前端插入长度明确的一段字节，不要求 str 以 NUL 结尾。
 *
 * 空间不足时仍复制源串尾部能容纳的部分，随后把 len 置为 -1。这样游标始终
 * 不越过缓冲区起点，同时最终由 extract_string() 统一报告路径过长。
 */
static bool prepend(struct prepend_buffer *p, const char *str, int namelen)
{
	// Already overflowed?
	if (p->len < 0)
		return false;

	// Will overflow?
	if (p->len < namelen) {
		// Fill as much as possible from the end of the name
		str += namelen - p->len;
		p->buf -= p->len;
		prepend_copy(p->buf, str, p->len);
		p->len = -1;
		return false;
	}

	// Fits fully
	p->len -= namelen;
	p->buf -= namelen;
	return prepend_copy(p->buf, str, namelen);
}

/**
 * prepend_name - prepend a pathname in front of current buffer pointer
 * @p: prepend buffer which contains buffer pointer and allocated length
 * @name: name string and length qstr structure
 *
 * With RCU path tracing, it may race with d_move(). Use READ_ONCE() to
 * make sure that either the old or the new name pointer and length are
 * fetched. However, there may be mismatch between length and pointer.
 * But since the length cannot be trusted, we need to copy the name very
 * carefully when doing the prepend_copy(). It also prepends "/" at
 * the beginning of the name. The sequence number check at the caller will
 * retry it again when a d_move() does happen. So any garbage in the buffer
 * due to mismatched pointer and length will be discarded.
 *
 * Load acquire is needed to make sure that we see the new name data even
 * if we might get the length wrong.
 */
/*
 * 插入一个目录项名字及其前导斜杠。
 *
 * d_name.name 的 acquire 读取与 dcache 发布新名字时的 release 写入配对，保证
 * 一旦看到新指针，也能看到新名字内容。name 和 len 仍可能来自不同版本；这由
 * 无故障复制及外层 rename_lock 序列号验证共同兜底。
 */
static bool prepend_name(struct prepend_buffer *p, const struct qstr *name)
{
	const char *dname = smp_load_acquire(&name->name); /* ^^^ */
	u32 dlen = READ_ONCE(name->len);

	return prepend(p, dname, dlen) && prepend_char(p, '/');
}

/*
 * 从给定 { dentry, mount } 向上回溯，直到 root 或某种边界，并把途经组件
 * 反向写入 p。调用者负责提供 RCU/序列锁保护以及在并发变化后重试。
 *
 * 返回值描述“为什么停止”，而不是缓冲区状态：
 * 0：到达调用者给定的 root（或因缓冲区溢出提前停止）；
 * 1：到达已挂载命名空间的绝对根；
 * 2：到达分离或尚未接入命名空间的 mount 根；
 * 3：遇到自指父目录，目标位于给定 root 之外或链条已经逃逸。
 * 缓冲区是否溢出由 p->len 独立记录，最终交给 extract_string() 判断。
 */
static int __prepend_path(const struct dentry *dentry, const struct mount *mnt,
			  const struct path *root, struct prepend_buffer *p)
{
	while (dentry != root->dentry || &mnt->mnt != root->mnt) {
		/* READ_ONCE 防止编译器把一次乐观快照拆成不可预期的重复读取。 */
		const struct dentry *parent = READ_ONCE(dentry->d_parent);

		if (dentry == mnt->mnt.mnt_root) {
			struct mount *m = READ_ONCE(mnt->mnt_parent);
			struct mnt_namespace *mnt_ns;

			/*
			 * 已到当前文件系统根；若存在父 mount，就切换到父 mount 的
			 * 挂载点继续回溯，而不是把当前 mount 根当作整条路径的根。
			 */
			if (likely(mnt != m)) {
				dentry = READ_ONCE(mnt->mnt_mountpoint);
				mnt = m;
				continue;
			}
			/* Global root */
			mnt_ns = READ_ONCE(mnt->mnt_ns);
			/* open-coded is_mounted() to use local mnt_ns */
			if (!IS_ERR_OR_NULL(mnt_ns) && !is_anon_ns(mnt_ns))
				return 1;	// absolute root
			else
				return 2;	// detached or not attached yet
		}

		if (unlikely(dentry == parent))
			/* Escaped? */
			return 3;

		/* 预取下一层父目录，同时把当前目录项的名字加入输出。 */
		prefetch(parent);
		if (!prepend_name(p, &dentry->d_name))
			break;
		dentry = parent;
	}
	return 0;
}

/**
 * prepend_path - Prepend path string to a buffer
 * @path: the dentry/vfsmount to report
 * @root: root vfsmnt/dentry
 * @p: prepend buffer which contains buffer pointer and allocated length
 *
 * The function will first try to write out the pathname without taking any
 * lock other than the RCU read lock to make sure that dentries won't go away.
 * It only checks the sequence number of the global rename_lock as any change
 * in the dentry's d_seq will be preceded by changes in the rename_lock
 * sequence number. If the sequence number had been changed, it will restart
 * the whole pathname back-tracing sequence again by taking the rename_lock.
 * In this case, there is no need to take the RCU read lock as the recursive
 * parent pointer references will keep the dentry chain alive as long as no
 * rename operation is performed.
 */
/*
 * 并发协议分成 mount 和 rename 两层：mount_lock 验证挂载树跳转，rename_lock
 * 验证 d_parent/d_name 链。read_seqbegin_or_lock() 首轮采用无锁序列读取；检测
 * 到冲突后把序号置为奇数并重试，从而转为持有对应读锁的稳定遍历。
 *
 * 每轮先复制 *p 到局部 b。失败轮次虽然可能在底层数组留下字节，但其游标
 * 不会提交；重试会从 *p 重新构造，只有两层序列号都验证成功后才提交新游标。
 */
static int prepend_path(const struct path *path,
			const struct path *root,
			struct prepend_buffer *p)
{
	unsigned seq, m_seq = 0;
	struct prepend_buffer b;
	int error;

	/* 外层 RCU 保护首次无锁读取 mount 链所借用的对象。 */
	rcu_read_lock();
restart_mnt:
	read_seqbegin_or_lock(&mount_lock, &m_seq);
	seq = 0;
	/* 内层 RCU 对应 dentry/name 的首次乐观遍历。 */
	rcu_read_lock();
restart:
	b = *p;
	read_seqbegin_or_lock(&rename_lock, &seq);
	error = __prepend_path(path->dentry, real_mount(path->mnt), root, &b);
	if (!(seq & 1))
		rcu_read_unlock();
	if (need_seqretry(&rename_lock, seq)) {
		/* rename 发生过：丢弃 b，持 rename_lock 读锁重新构造整条路径。 */
		seq = 1;
		goto restart;
	}
	done_seqretry(&rename_lock, seq);

	if (!(m_seq & 1))
		rcu_read_unlock();
	if (need_seqretry(&mount_lock, m_seq)) {
		/* mount 拓扑发生过：连跨挂载点的决策也必须从头重做。 */
		m_seq = 1;
		goto restart_mnt;
	}
	done_seqretry(&mount_lock, m_seq);

	/* 逃逸结果不能泄露已拼出的、相对于错误根的路径片段。 */
	if (unlikely(error == 3))
		b = *p;

	/* 输入本身就是根时循环没有写组件，规范化为单个 "/"。 */
	if (b.len == p->len)
		prepend_char(&b, '/');

	*p = b;
	return error;
}

/**
 * __d_path - return the path of a dentry
 * @path: the dentry/vfsmount to report
 * @root: root vfsmnt/dentry
 * @buf: buffer to return value in
 * @buflen: buffer length
 *
 * Convert a dentry into an ASCII path name.
 *
 * Returns a pointer into the buffer or an error code if the
 * path was too long.
 *
 * "buflen" should be positive.
 *
 * If the path is not reachable from the supplied root, return %NULL.
 */
/*
 * 生成相对于指定 root 的路径。buflen 必须为正；先放入终止 NUL，再向前添加
 * 组件。目标不在 root 下时返回 NULL，路径过长则返回 ERR_PTR(-ENAMETOOLONG)。
 */
char *__d_path(const struct path *path,
	       const struct path *root,
	       char *buf, int buflen)
{
	DECLARE_BUFFER(b, buf, buflen);

	prepend_char(&b, 0);
	if (unlikely(prepend_path(path, root, &b) > 0))
		return NULL;
	return extract_string(&b);
}

/*
 * 生成不受进程 chroot 根限制的绝对路径。
 * 空 root 让遍历一直走到 mount 拓扑边界；到达正常全局根（返回 1）可接受，
 * 分离 mount 或逃逸状态（返回值大于 1）则不能组成有效绝对路径，返回 -EINVAL。
 */
char *d_absolute_path(const struct path *path,
	       char *buf, int buflen)
{
	struct path root = {};
	DECLARE_BUFFER(b, buf, buflen);

	prepend_char(&b, 0);
	if (unlikely(prepend_path(path, &root, &b) > 1))
		return ERR_PTR(-EINVAL);
	return extract_string(&b);
}

/*
 * 在 RCU 读侧临界区中取得 fs->root 的一致快照。
 * fs->seq 同时保护 fs_struct 内路径字段的更新；这里不增加 path 引用，调用者
 * 只能在其现有生命周期保护（本文件中为 RCU）内使用这个借用结果。
 */
static void get_fs_root_rcu(struct fs_struct *fs, struct path *root)
{
	unsigned seq;

	do {
		seq = read_seqbegin(&fs->seq);
		*root = fs->root;
	} while (read_seqretry(&fs->seq, seq));
}

/**
 * d_path - return the path of a dentry
 * @path: path to report
 * @buf: buffer to return value in
 * @buflen: buffer length
 *
 * Convert a dentry into an ASCII path name. If the entry has been deleted
 * the string " (deleted)" is appended. Note that this is ambiguous.
 *
 * Returns a pointer into the buffer or an error code if the path was
 * too long. Note: Callers should use the returned pointer, not the passed
 * in buffer, to use the name! The implementation often starts at an offset
 * into the buffer, and may leave 0 bytes at the start.
 *
 * "buflen" should be positive.
 */
/*
 * 生成调用进程视角下的路径，是 /proc 等代码最常用的入口。
 *
 * 普通对象以 current->fs->root 为边界，因此会尊重 chroot。未链接对象在末尾
 * 带 " (deleted)"。某些从不进入哈希表的伪文件系统对象没有稳定的目录路径，
 * 则由其 d_dname 回调按需生成可读名称。
 *
 * 返回值可能指向 buf 中部，也可能是错误指针；buf 及 path 的所有权均不变。
 */
char *d_path(const struct path *path, char *buf, int buflen)
{
	DECLARE_BUFFER(b, buf, buflen);
	struct path root;

	/*
	 * We have various synthetic filesystems that never get mounted.  On
	 * these filesystems dentries are never used for lookup purposes, and
	 * thus don't need to be hashed.  They also don't need a name until a
	 * user wants to identify the object in /proc/pid/fd/.  The little hack
	 * below allows us to generate a name for these objects on demand:
	 *
	 * Some pseudo inodes are mountable.  When they are mounted
	 * path->dentry == path->mnt->mnt_root.  In that case don't call d_dname
	 * and instead have d_path return the mounted path.
	 */
	if (path->dentry->d_op && path->dentry->d_op->d_dname &&
	    (!IS_ROOT(path->dentry) || path->dentry != path->mnt->mnt_root))
		/* 回调直接接管格式和返回指针约定。 */
		return path->dentry->d_op->d_dname(path->dentry, buf, buflen);

	rcu_read_lock();
	/* root 只是 RCU 保护下的借用快照，不需要 path_get()/path_put()。 */
	get_fs_root_rcu(current->fs, &root);
	/* 后缀先写入尾部，随后 prepend_path() 再把目录组件插到它前面。 */
	if (unlikely(d_unlinked(path->dentry)))
		prepend(&b, " (deleted)", 11);
	else
		prepend_char(&b, 0);
	prepend_path(path, &root, &b);
	rcu_read_unlock();

	return extract_string(&b);
}
EXPORT_SYMBOL(d_path);

/*
 * Helper function for dentry_operations.d_dname() members
 */
/*
 * 为 d_dname 回调提供 printf 风格的通用实现。
 * vsnprintf() 先在缓冲区起点格式化，确认结果同时满足 NAME_MAX 和 buflen 后，
 * 再把包含 NUL 的结果移到末端，以保持与 d_path() 其他返回路径一致的“返回
 * 缓冲区内部指针”约定。格式化被截断时不返回残缺名字，而报告 -ENAMETOOLONG。
 */
char *dynamic_dname(char *buffer, int buflen, const char *fmt, ...)
{
	va_list args;
	char *start;
	int sz;

	va_start(args, fmt);
	sz = vsnprintf(buffer, buflen, fmt, args) + 1;
	va_end(args);

	if (sz > NAME_MAX || sz > buflen)
		return ERR_PTR(-ENAMETOOLONG);

	/* Move the formatted d_name to the end of the buffer. */
	start = buffer + (buflen - sz);
	return memmove(start, buffer, sz);
}

/*
 * 为不会被 rename 的简单伪 dentry 生成 "/name (deleted)"。
 * 由于调用场景保证名字和父子关系不变化，可以直接读取 d_name 而无需 d_lock；
 * 输出仍沿用反向构造和错误指针约定。
 */
char *simple_dname(struct dentry *dentry, char *buffer, int buflen)
{
	DECLARE_BUFFER(b, buffer, buflen);
	/* these dentries are never renamed, so d_lock is not needed */
	prepend(&b, " (deleted)", 11);
	prepend(&b, dentry->d_name.name, dentry->d_name.len);
	prepend_char(&b, '/');
	return extract_string(&b);
}

/*
 * Write full pathname from the root of the filesystem into the buffer.
 */
/*
 * 只沿 d_parent 回溯到当前文件系统的 dentry 根，不跨越 mount。
 * 首轮在 RCU 下乐观读取；若 rename_lock 序列变化，则从原始游标 *p 重新开始，
 * 并由 read_seqbegin_or_lock() 切换到持锁遍历。成功轮次的局部 b 才用于返回。
 */
static char *__dentry_path(const struct dentry *d, struct prepend_buffer *p)
{
	const struct dentry *dentry;
	struct prepend_buffer b;
	int seq = 0;

	rcu_read_lock();
restart:
	/* 重试必须同时恢复起始 dentry 和缓冲区游标。 */
	dentry = d;
	b = *p;
	read_seqbegin_or_lock(&rename_lock, &seq);
	while (!IS_ROOT(dentry)) {
		const struct dentry *parent = dentry->d_parent;

		prefetch(parent);
		if (!prepend_name(&b, &dentry->d_name))
			break;
		dentry = parent;
	}
	if (!(seq & 1))
		rcu_read_unlock();
	if (need_seqretry(&rename_lock, seq)) {
		/* 丢弃并发 rename 期间产生的路径视图，下一轮持读锁重建。 */
		seq = 1;
		goto restart;
	}
	done_seqretry(&rename_lock, seq);
	/* 根 dentry 没有可插入的名字，仍应显示为 "/"。 */
	if (b.len == p->len)
		prepend_char(&b, '/');
	return extract_string(&b);
}

/*
 * 返回 dentry 在其所属文件系统内部的原始路径；不标记 unhashed/deleted 状态，
 * 也不根据 mount 或进程根改写路径。调用者必须使用返回的缓冲区内部指针。
 */
char *dentry_path_raw(const struct dentry *dentry, char *buf, int buflen)
{
	DECLARE_BUFFER(b, buf, buflen);

	prepend_char(&b, 0);
	return __dentry_path(dentry, &b);
}
EXPORT_SYMBOL(dentry_path_raw);

/*
 * dentry_path_raw() 的带状态版本：未链接 dentry 使用 "//deleted" 作为尾部标记。
 * 长度 10 包含字符串终止 NUL；正常分支则显式先写一个 NUL。
 */
char *dentry_path(const struct dentry *dentry, char *buf, int buflen)
{
	DECLARE_BUFFER(b, buf, buflen);

	if (unlikely(d_unlinked(dentry)))
		prepend(&b, "//deleted", 10);
	else
		prepend_char(&b, 0);
	return __dentry_path(dentry, &b);
}

/*
 * 一次性取得 current fs_struct 的 root 与 pwd 一致快照，避免分别读取时夹入
 * chroot/chdir 更新而得到不属于同一时刻的路径对。结果不增引用，仅供调用者
 * 在 RCU 临界区内借用。
 */
static void get_fs_root_and_pwd_rcu(struct fs_struct *fs, struct path *root,
				    struct path *pwd)
{
	unsigned seq;

	do {
		seq = read_seqbegin(&fs->seq);
		*root = fs->root;
		*pwd = fs->pwd;
	} while (read_seqretry(&fs->seq, seq));
}

/*
 * NOTE! The user-level library version returns a
 * character pointer. The kernel system call just
 * returns the length of the buffer filled (which
 * includes the ending '\0' character), or a negative
 * error value. So libc would do something like
 *
 *	char *getcwd(char * buf, size_t size)
 *	{
 *		int retval;
 *
 *		retval = sys_getcwd(buf, size);
 *		if (retval >= 0)
 *			return buf;
 *		errno = -retval;
 *		return NULL;
 *	}
 */
/*
 * getcwd(2) 的内核实现。
 *
 * 流程：从 name cache 取得 PATH_MAX 临时页；在 RCU 下同时快照进程 root/pwd；
 * 拒绝已删除的工作目录；从页尾反向构造路径；退出 RCU 后检查长度并复制给
 * 用户。临时页始终在统一出口由 __putname() 归还。
 *
 * 与 libc 包装不同，系统调用成功时返回写入字节数且包含末尾 NUL。若 pwd 已
 * 越出进程 root（例如改变进程根后 cwd 仍处在旧目录树），仍返回路径，但在
 * 前面增加 "(unreachable)"，让用户空间能够识别这一安全边界。
 */
SYSCALL_DEFINE2(getcwd, char __user *, buf, unsigned long, size)
{
	int error;
	struct path pwd, root;
	char *page = __getname();

	if (!page)
		return -ENOMEM;

	/* root 和 pwd 都是无引用的借用快照，整个路径回溯完成前保持 RCU。 */
	rcu_read_lock();
	get_fs_root_and_pwd_rcu(current->fs, &root, &pwd);

	if (unlikely(d_unlinked(pwd.dentry))) {
		/* 已删除工作目录不再有可报告的当前路径。 */
		rcu_read_unlock();
		error = -ENOENT;
	} else {
		unsigned len;
		DECLARE_BUFFER(b, page, PATH_MAX);

		/* 先放终止符；后续目录组件和不可达标记都在它前方插入。 */
		prepend_char(&b, 0);
		if (unlikely(prepend_path(&pwd, &root, &b) > 0))
			prepend(&b, "(unreachable)", 13);
		rcu_read_unlock();

		/* len < 0 会使该差值大于 PATH_MAX，从而统一映射为路径过长。 */
		len = PATH_MAX - b.len;
		if (unlikely(len > PATH_MAX))
			error = -ENAMETOOLONG;
		else if (unlikely(len > size))
			/* 用户缓冲区存在但容量不足，与内部 PATH_MAX 溢出区分。 */
			error = -ERANGE;
		else if (copy_to_user(buf, b.buf, len))
			/* 用户地址不可写或复制中发生 fault。 */
			error = -EFAULT;
		else
			error = len;
	}
	__putname(page);
	return error;
}
