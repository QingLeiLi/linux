// SPDX-License-Identifier: GPL-2.0
/*
 * pidfs 学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5 Codex（2026-07-28）。
 *
 * 职责边界：
 *   pidfs 是 pidfd 背后的内核伪文件系统。它把长期稳定的 struct pid
 *   包装成普通 struct file，使 poll、ioctl、fdinfo、文件句柄和 xattr
 *   可以复用 VFS 接口；它不负责 PID 分配、信号递送或任务回收本身。
 *
 * 主路径：
 *   alloc_pid()
 *     -> pidfs_prepare_pid() / pidfs_add_pid()：初始化并发布 64 位身份
 *     -> pidfs_alloc_file()：取得 pid 引用、创建或复用 stashed dentry
 *     -> poll/ioctl/fdinfo：观察任务与命名空间
 *   do_exit()/coredump
 *     -> pidfs_exit()/pidfs_coredump()：先写快照，再发布 attr_mask 位
 *     -> release_task()/free_pid()
 *     -> pidfs_remove_pid()/pidfs_free_pid()：停止按 inode 查找并最终释放
 *
 * 核心对象与所有权：
 *   struct pid 是身份与生命周期锚点；inode->i_private 持有一个 pid 引用。
 *   pid->stashed 让同一 pid 的所有 pidfd 复用同一 dentry/inode。
 *   pid->attr 在首次注册 pidfd 时分配，保存退出、coredump 与 xattr 快照，
 *   直到最后一个 struct pid 引用消失才释放。
 *
 * 并发模型：
 *   pid->wait_pidfd.lock 串行化“首次注册属性”与“任务退出”；attr_mask 的
 *   位发布配合 smp_wmb()/smp_rmb()，保证读者不会看到半写入快照。
 *   inode 表由 rhashtable + RCU 查找；stashed dentry helper 负责唯一化；
 *   inode 锁串行化 xattr 更新，旧 xattr 通过 RCU 延迟释放。
 *
 * 方案权衡：
 *   伪文件系统让 pidfd 获得独立 inode、exportfs 和 VFS 生态能力，代价是
 *   维护 dentry/inode/属性缓存及 32 位 inode 回绕协议。pidfd 稳定的是
 *   “这个 struct pid 身份”，不是保证对应 task 永远存活或字段不变化。
 */
#include <linux/anon_inodes.h>
#include <linux/exportfs.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/cgroup.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/pid.h>
#include <linux/pidfs.h>
#include <linux/sched/signal.h>
#include <linux/signal.h>
#include <linux/pid_namespace.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/proc_ns.h>
#include <linux/pseudo_fs.h>
#include <linux/ptrace.h>
#include <linux/seq_file.h>
#include <uapi/linux/pidfd.h>
#include <linux/ipc_namespace.h>
#include <linux/time_namespace.h>
#include <linux/utsname.h>
#include <net/net_namespace.h>
#include <linux/coredump.h>
#include <linux/rhashtable.h>
#include <linux/llist.h>
#include <linux/xattr.h>
#include <linux/cookie.h>

#include "internal.h"
#include "mount.h"

#define PIDFS_PID_DEAD ERR_PTR(-ESRCH)

/*
 * pidfs_attr_cachep：pidfd 延迟属性对象的专用 slab。仅在 init 后赋值，
 * __ro_after_init 防止运行期被改写；对象本身由 pidfs_free_pid() 回收。
 */
static struct kmem_cache *pidfs_attr_cachep __ro_after_init;

/*
 * pidfs_root_path：内核内部挂载的根路径。挂载在初始化后永久存在，
 * pidfs_get_root() 通过 path_get() 为调用者取得一份独立引用。
 */
static struct path pidfs_root_path = {};

/*
 * pidfs_xa_cache：所有 pidfs inode 的 simple_xattr 分配缓存。具体属性链
 * 仍属于各 pidfs_attr；缓存只提供统一的分配、查找与 RCU 回收机制。
 */
static struct simple_xattr_cache pidfs_xa_cache;

/*
 * pidfs_get_root() - 向调用者返回 pidfs 内核挂载根路径的持有引用。
 *
 * @path：非 NULL 输出参数；进入时无需包含有效路径，返回后持有 mnt/dentry
 * 引用，调用者必须 path_put()。函数只复制稳定的全局路径并增引用，
 * 不睡眠、不失败，也不改变全局根路径。
 */
void pidfs_get_root(struct path *path)
{
	/* 先复制成完整 path，再统一增持两部分引用，避免调用者借用全局存储。 */
	*path = pidfs_root_path;
	path_get(path);
}

/*
 * 这些位是 attr_mask 的发布标志，而不是用户 ABI。每一位为 1 都承诺
 * 对应字段组已完整写入；读侧必须先测试位，再通过读屏障读取字段。
 */
enum pidfs_attr_mask_bits {
	PIDFS_ATTR_BIT_EXIT	= 0,
	PIDFS_ATTR_BIT_COREDUMP	= 1,
};

/*
 * pidfs_anon_attr - 与 inode/xattr 无关、可在任务消失后继续读取的快照。
 *
 * attr_mask 发布各字段组；exit 组保存退出时所在默认 cgroup 与 wait
 * 状态码；coredump 组保存 dump 策略、触发信号和 si_code。字段在发布位
 * 置位前只由退出/coredump 路径写入，发布后视为只读。
 */
struct pidfs_anon_attr {
	unsigned long attr_mask;
	/* exit info：以下匿名字段组是任务退出时一次性冻结并共同发布的信息。 */
	struct /* exit info */ {
		/* 该匿名字段组保存任务退出时冻结并一次性发布的退出快照。 */
		__u64 cgroupid;
		__s32 exit_code;
	};
	__u32 coredump_mask;
	__u32 coredump_signal;
	__u32 coredump_code;
};

/*
 * pidfs_ino_ht：以 struct pid::ino 为键、pidfs_hash 为侵入节点的全局
 * 索引，供 exportfs 文件句柄恢复 pid。插入发生在 pid 创建阶段，删除
 * 先于 pid 最终释放；查找者处于 RCU 临界区并在返回前 get_pid()。
 */
static struct rhashtable pidfs_ino_ht;

/* 参数描述键、节点在 struct pid 内的偏移；自动收缩降低低负载内存成本。 */
static const struct rhashtable_params pidfs_ino_ht_params = {
	.key_offset		= offsetof(struct pid, ino),
	.key_len		= sizeof(u64),
	.head_offset		= offsetof(struct pid, pidfs_hash),
	.automatic_shrinking	= true,
};

/*
 * inode number handling
 *
 * On 64 bit nothing special happens. The 64bit number assigned
 * to struct pid is the inode number.
 *
 * On 32 bit the 64 bit number assigned to struct pid is split
 * into two 32 bit numbers. The lower 32 bits are used as the
 * inode number and the upper 32 bits are used as the inode
 * generation number.
 *
 * On 32 bit pidfs_ino() will return the lower 32 bit. When
 * pidfs_ino() returns zero a wrap around happened. When a
 * wraparound happens the 64 bit number will be incremented by 1
 * so inode numbering starts at 1 again.
 *
 * On 64 bit comparing two pidfds is as simple as comparing
 * inode numbers.
 *
 * When a wraparound happens on 32 bit multiple pidfds with the
 * same inode number are likely to exist (This isn't a problem
 * since before pidfs pidfds used the anonymous inode meaning
 * all pidfds had the same inode number.). Userspace can
 * reconstruct the 64 bit identifier by retrieving both the
 * inode number and the inode generation number to compare or
 * use file handles.
 */
/*
 *
 * 64 位平台可直接把分配给 struct pid 的 64 位编号用作 inode 号。
 * 32 位 VFS 的 i_ino 只能容纳低 32 位，因此高 32 位进入 i_generation；
 * 低半部回绕到 0 时跳过 0，使 0 始终保留为无效/未分配哨兵。
 *
 * 单看 32 位 i_ino 可能在回绕后重名，这不是对象身份混淆：用户态应把
 * i_ino 与 generation 合成 64 位身份，或者直接使用 exportfs file
 * handle。相比旧 anon_inode 让所有 pidfd 共用 inode，这仍提供了更强
 * 的可比较性。
 */
/*
 * pidfs_attr - pidfs 为一个 struct pid 延长保存的全部附加状态。
 *
 * xattrs 是受 inode 锁更新、可被 RCU 读者观察的属性链。匿名 union 在
 * 正常生命周期中保存 pidfs_anon_attr；只有 struct pid 已不可再被访问、
 * 且需要把带 xattr 的对象转交异步 worker 时，才复用为 pidfs_llist
 * 节点，因此两个视图不会同时使用。
 */
struct pidfs_attr {
	struct list_head xattrs;
	union {
		struct pidfs_anon_attr;
		struct llist_node pidfs_llist;
	};
};

#if BITS_PER_LONG == 32

/*
 * 32 位平台用全局锁保护 pidfs_ino_nr 的“跳过低半部 0 + 递增”事务。
 * 该路径只在 pid 创建时执行，简单自旋锁换取严格单调且无重复的 64 位号。
 */
DEFINE_SPINLOCK(pidfs_ino_lock);
static u64 pidfs_ino_nr = 1;

/*
 * pidfs_ino() - 提取 32 位 VFS inode 号。
 * @ino：已分配的 64 位 pidfs 身份；纯输入。
 * 返回低 32 位，不取得引用、无副作用。
 */
static inline unsigned long pidfs_ino(u64 ino)
{
	return lower_32_bits(ino);
}

/* On 32 bit the generation number are the upper 32 bits. */
/* 32 位平台的 generation number 是高 32 位，用于区分 i_ino 回绕代次。 */
/*
 * pidfs_gen() - 提取与 i_ino 配套的回绕代次。
 * @ino：64 位身份；返回高 32 位，无副作用。
 */
static inline u32 pidfs_gen(u64 ino)
{
	return upper_32_bits(ino);
}

/*
 * pidfs_alloc_ino() - 在 32 位平台分配非零低半部的唯一 64 位身份。
 *
 * 入参：无。持锁检查当前低 32 位是否为 0，必要时跳过该值，再领取并
 * 递增计数器。返回值由新 struct pid 持有直至销毁；函数不可睡眠。
 */
static inline u64 pidfs_alloc_ino(void)
{
	u64 ino;

	/* 检查与递增必须同属临界区，否则两个创建者可能领取相同编号。 */
	spin_lock(&pidfs_ino_lock);
	if (pidfs_ino(pidfs_ino_nr) == 0)
		pidfs_ino_nr++;
	ino = pidfs_ino_nr++;
	spin_unlock(&pidfs_ino_lock);
	return ino;
}

#else

/* On 64 bit simply return ino. */
/* 64 位平台的 VFS inode 足以容纳完整身份，因此无需拆分。 */
/*
 * pidfs_ino() - 返回完整 64 位 pidfs 身份作为 VFS inode 号。
 * @ino：纯输入；返回值不涉及引用或共享状态。
 */
static inline unsigned long pidfs_ino(u64 ino)
{
	return ino;
}

/* On 64 bit the generation number is 0. */
/* 64 位 i_ino 不会因截断而重名，所以 generation 固定为 0。 */
/*
 * pidfs_gen() - 给 64 位平台提供统一的 generation 接口。
 * @ino：未使用的身份参数；返回 0，无副作用。
 */
static inline u32 pidfs_gen(u64 ino)
{
	return 0;
}

DEFINE_COOKIE(pidfs_ino_cookie);

/*
 * pidfs_alloc_ino() - 从可扩展 cookie 生成器领取全局唯一的 64 位身份。
 *
 * gen_cookie_next() 使用 per-CPU/全局协调状态，调用期间禁用抢占以保证
 * 当前 CPU 的生成上下文稳定；返回必须从 1 开始。无失败返回。
 */
static u64 pidfs_alloc_ino(void)
{
	u64 ino;

	/* 禁止任务迁移，满足 cookie 生成器对当前 CPU 状态的配对要求。 */
	preempt_disable();
	ino = gen_cookie_next(&pidfs_ino_cookie);
	preempt_enable();

	VFS_WARN_ON_ONCE(ino < 1);
	return ino;
}

#endif

/*
 * pidfs_prepare_pid() - 初始化新 struct pid 内嵌的 pidfs 状态。
 *
 * @pid：尚未发布给 pidfs 查找者的新 pid，调用者独占；函数把 dentry、
 * 属性和 inode 身份置为“尚未创建”。无分配、无失败、不可单独发布对象。
 */
void pidfs_prepare_pid(struct pid *pid)
{
	pid->stashed = NULL;
	pid->attr = NULL;
	pid->ino = 0;
}

/*
 * pidfs_add_pid() - 分配身份并把新 pid 发布到 inode 哈希表。
 *
 * @pid：已执行 pidfs_prepare_pid()、仍由 pid 分配路径持有的对象。
 * 函数可能因 rhashtable 扩容而分配/失败；成功后 exportfs 可按 ino
 * 找到它，失败则把 ino 恢复为 0，由上层撤销 pid 创建。
 * 返回 0 或 rhashtable 的负 errno，不转移调用者已有的 pid 引用。
 */
int pidfs_add_pid(struct pid *pid)
{
	int ret;

	/* 先建立键值，再插入哈希；插入成功是外部可发现性的发布点。 */
	pid->ino = pidfs_alloc_ino();
	ret = rhashtable_insert_fast(&pidfs_ino_ht, &pid->pidfs_hash,
				     pidfs_ino_ht_params);
	if (unlikely(ret))
		pid->ino = 0;
	return ret;
}

/*
 * pidfs_remove_pid() - 在 struct pid 最终释放前停止新的 inode 查找。
 *
 * @pid：仍存活且由调用者持有的 pid。ino==0 表示创建期从未成功发布；
 * 否则从 rhashtable 摘除。已有 inode/pid 引用仍然有效，函数只阻止新
 * 的 file-handle 恢复，不负责释放 attr 或 pid。
 */
void pidfs_remove_pid(struct pid *pid)
{
	if (likely(pid->ino))
		rhashtable_remove_fast(&pidfs_ino_ht, &pid->pidfs_hash,
				       pidfs_ino_ht_params);
}

/*
 * pidfs_free_list：不能在当前释放上下文同步销毁 xattr 的 attr 对象队列。
 * llist_add() 无锁入队；只有空->非空的入队者调度一次 worker。
 */
static LLIST_HEAD(pidfs_free_list);

/*
 * pidfs_free_attr_work() - 批量释放延迟的属性链和 attr 容器。
 *
 * @work：静态 work 项，仅用于 workqueue 协议，本函数不从中取业务数据。
 * worker 先原子摘走整条 llist，使并发生产者可继续入队；随后逐项释放
 * xattr 和容器。进程上下文可睡眠，无返回值。
 */
static void pidfs_free_attr_work(struct work_struct *work)
{
	struct pidfs_attr *attr, *next;
	struct llist_node *head;

	/* 摘链建立本批次所有权：之后只有当前 worker 访问 head 中的对象。 */
	head = llist_del_all(&pidfs_free_list);
	llist_for_each_entry_safe(attr, next, head, pidfs_llist) {
		simple_xattrs_free(&pidfs_xa_cache, &attr->xattrs, NULL);
		kfree(attr);
	}
}

/* 唯一静态 work 项把 free_list 的批处理消费绑定到上述回调。 */
static DECLARE_WORK(pidfs_free_work, pidfs_free_attr_work);

/*
 * pidfs_free_pid() - 在 struct pid 最后阶段释放其 pidfs 附加状态。
 *
 * @pid：已从所有查找结构摘除、即将销毁的对象，调用者独占其最终状态。
 * stashed 必须已由 dentry prune 清空。attr 为 NULL 表示创建失败，错误
 * 指针表示从未创建 pidfd；正常 attr 若无 xattr 可直接释放，否则转交
 * worker，避免在当前上下文执行 xattr 的延迟回收流程。
 */
void pidfs_free_pid(struct pid *pid)
{
	struct pidfs_attr *attr = pid->attr;

	/*
	 * Any dentry must've been wiped from the pid by now.
	 * Otherwise there's a reference count bug.
	 */
	/*
	 * 此时任何 dentry 都必须已从 pid 上清除，否则说明 inode
	 * 持有的 pid 引用与 pid 销毁顺序矛盾，是引用计数错误。
	 */
	VFS_WARN_ON_ONCE(pid->stashed);

	/*
	 * This if an error occurred during e.g., task creation that
	 * causes us to never go through the exit path.
	 */
	/*
	 * 例如任务创建中途失败时不会经过正常 exit 路径，attr
	 * 可能从未分配；NULL 因而是合法的“无资源可释放”状态。
	 */
	if (unlikely(!attr))
		return;

	/* This never had a pidfd created. */
	/* 错误哨兵表示退出时确认从未创建 pidfd，也没有真实 attr 可释放。 */
	if (IS_ERR(attr))
		return;

	/*
	 * 无 xattr 时容器可立即释放；有 xattr 时复用 union 中 llist 节点
	 * 转交 worker。llist_add() 返回队列此前是否为空，以合并调度次数。
	 */
	if (likely(list_empty(&attr->xattrs)))
		kfree(attr);
	else if (llist_add(&attr->pidfs_llist, &pidfs_free_list))
		schedule_work(&pidfs_free_work);
}

#ifdef CONFIG_PROC_FS
/**
 * pidfd_show_fdinfo - print information about a pidfd
 * @m: proc fdinfo file
 * @f: file referencing a pidfd
 *
 * Pid:
 * This function will print the pid that a given pidfd refers to in the
 * pid namespace of the procfs instance.
 * If the pid namespace of the process is not a descendant of the pid
 * namespace of the procfs instance 0 will be shown as its pid. This is
 * similar to calling getppid() on a process whose parent is outside of
 * its pid namespace.
 *
 * NSpid:
 * If pid namespaces are supported then this function will also print
 * the pid of a given pidfd refers to for all descendant pid namespaces
 * starting from the current pid namespace of the instance, i.e. the
 * Pid field and the first entry in the NSpid field will be identical.
 * If the pid namespace of the process is not a descendant of the pid
 * namespace of the procfs instance 0 will be shown as its first NSpid
 * entry and no others will be shown.
 * Note that this differs from the Pid and NSpid fields in
 * /proc/<pid>/status where Pid and NSpid are always shown relative to
 * the  pid namespace of the procfs instance. The difference becomes
 * obvious when sending around a pidfd between pid namespaces from a
 * different branch of the tree, i.e. where no ancestral relation is
 * present between the pid namespaces:
 * - create two new pid namespaces ns1 and ns2 in the initial pid
 *   namespace (also take care to create new mount namespaces in the
 *   new pid namespace and mount procfs)
 * - create a process with a pidfd in ns1
 * - send pidfd from ns1 to ns2
 * - read /proc/self/fdinfo/<pidfd> and observe that both Pid and NSpid
 *   have exactly one entry, which is 0
 */
/*
 *
 * pidfd_show_fdinfo() 把 pidfd 指向的身份投影到“当前 procfs 实例”的
 * PID 命名空间。Pid 是该层编号；NSpid 从该层开始向 pid 所在的更深
 * 后代命名空间逐层列出编号。若两者不在祖先链上，则输出 Pid: 0，
 * NSpid 也只有一个 0，这与 getppid() 无法表示命名空间外父进程相同。
 *
 * 与 /proc/<pid>/status 的差别在跨分支传递 pidfd 时最明显：pidfd 的
 * struct pid 身份仍有效，但接收方 procfs 的命名空间不一定是其祖先，
 * 因而不存在可见数字。@m 是 seq_file 输出缓冲，@f 借用 pidfd file；
 * 函数不取得长期引用，调用者的 file 引用保证 inode/pid 存活。
 */
static void pidfd_show_fdinfo(struct seq_file *m, struct file *f)
{
	struct pid *pid = pidfd_pid(f);
	struct pid_namespace *ns;
	pid_t nr = -1;

	/*
	 * 只有 pid 仍关联活 task 时才做命名空间换算；已经退出/收割的 pidfd
	 * 仍可读 fdinfo，但以 -1 表明没有当前进程号。
	 */
	if (likely(pid_has_task(pid, PIDTYPE_PID))) {
		ns = proc_pid_ns(file_inode(m->file)->i_sb);
		nr = pid_nr_ns(pid, ns);
	}

	seq_put_decimal_ll(m, "Pid:\t", nr);

#ifdef CONFIG_PID_NS
	seq_put_decimal_ll(m, "\nNSpid:\t", nr);
	if (nr > 0) {
		int i;

		/* If nr is non-zero it means that 'pid' is valid and that
		 * ns, i.e. the pid namespace associated with the procfs
		 * instance, is in the pid namespace hierarchy of pid.
		 * Start at one below the already printed level.
		 */
		/*
		 * nr 非零证明 procfs 的 ns 位于 pid 的祖先链上。
		 * 当前层已经作为 Pid/NSpid 第一项输出，因此循环从下一层开始，
		 * 直接读取 struct upid 数组；file 引用保证 pid 存活。
		 */
		for (i = ns->level + 1; i <= pid->level; i++)
			seq_put_decimal_ll(m, "\t", pid->numbers[i].nr);
	}
#endif
	seq_putc(m, '\n');
}
#endif

/*
 * Poll support for process exit notification.
 */
/*
 * 为 pidfd 提供进程退出通知。poll_wait() 把调用者登记到
 * pid->wait_pidfd；退出路径唤醒后，本函数用 RCU 查 task 状态并返回
 * EPOLLIN/EPOLLRDNORM，彻底失去 task 时额外返回 EPOLLHUP。
 *
 * pidfd_poll() - 查询 pidfd 指向线程/线程组是否已达到可观察退出状态。
 *
 * @file：持有 pidfs inode 和 struct pid 引用的借用 file。
 * @pts：poll 核心提供的登记表，可为 NULL（仅查询不登记）。
 * 返回 poll 位图；无 errno。函数处于 poll 上下文，不睡眠；RCU 只保证
 * task 指针在临界区内不释放，exit_state 的语义由信号/退出协议保证。
 */
static __poll_t pidfd_poll(struct file *file, struct poll_table_struct *pts)
{
	struct pid *pid = pidfd_pid(file);
	struct task_struct *task;
	__poll_t poll_flags = 0;

	/* 必须先登记再检查条件，避免退出恰好发生在检查与入队之间而丢唤醒。 */
	poll_wait(file, &pid->wait_pidfd, pts);
	/*
	 * Don't wake waiters if the thread-group leader exited
	 * prematurely. They either get notified when the last subthread
	 * exits or not at all if one of the remaining subthreads execs
	 * and assumes the struct pid of the old thread-group leader.
	 */
	/*
	 * 线程组 leader 提前退出时不能立即唤醒；应等最后一个线程
	 * 退出，或由某个存活线程 exec 后接管旧 leader 的 struct pid。
	 * 否则用户态会把“leader task 退出”误当成“整个进程不可再运行”。
	 */
	guard(rcu)();
	task = pid_task(pid, PIDTYPE_PID);
	if (!task)
		poll_flags = EPOLLIN | EPOLLRDNORM | EPOLLHUP;
	else if (task->exit_state && !delay_group_leader(task))
		poll_flags = EPOLLIN | EPOLLRDNORM;

	return poll_flags;
}

/*
 * pid_in_current_pidns() - 判断 pid 是否能在当前任务的 PID 命名空间祖先链中表示。
 *
 * @pid：由 file/调用者保证存活的借用身份。若当前 ns 层级不深于 pid，
 * 且 pid 对应层的 namespace 指针相同，则返回 true；兄弟分支或更深的
 * 调用者返回 false。只读稳定的 namespace 关系，不增引用、不睡眠。
 */
static inline bool pid_in_current_pidns(const struct pid *pid)
{
	const struct pid_namespace *ns = task_active_pid_ns(current);

	if (ns->level <= pid->level)
		return pid->numbers[ns->level].ns == ns;

	return false;
}

/*
 * pidfs_coredump_mask() - 把内部 dumpable 状态翻译成 pidfd UAPI 位。
 *
 * @dumpable：任务在 exec/coredump 协议中的枚举快照。
 * 返回 USER、ROOT 或 SKIP 之一；未知值触发一次告警并返回 0。
 * 这是纯映射，无所有权与睡眠副作用。
 */
static __u32 pidfs_coredump_mask(enum task_dumpable dumpable)
{
	/*
	 * 这是固定枚举到 UAPI 位的机械映射表：每个已知分支立即返回，
	 * default 只负责把不可能状态变成可诊断的告警和安全的零结果。
	 */
	switch (dumpable) {
	case TASK_DUMPABLE_OWNER:
		return PIDFD_COREDUMP_USER;
	case TASK_DUMPABLE_ROOT:
		return PIDFD_COREDUMP_ROOT;
	/* OFF 与异常兜底都不授予可转储权限；前者仍有明确的 SKIP 标志。 */
	case TASK_DUMPABLE_OFF:
		return PIDFD_COREDUMP_SKIP;
	default:
		WARN_ON_ONCE(true);
	}

	return 0;
}

/* This must be updated whenever a new flag is added */
/*
 * 每新增一个可返回的 PIDFD_INFO_* 位都必须同步更新此集合。
 * 它既向用户态报告内核能力，也用于检查 kinfo.mask 是否意外泄露未知位。
 */
#define PIDFD_INFO_SUPPORTED (PIDFD_INFO_PID | \
			      PIDFD_INFO_CREDS | \
			      PIDFD_INFO_CGROUPID | \
			      PIDFD_INFO_EXIT | \
			      PIDFD_INFO_COREDUMP | \
			      PIDFD_INFO_SUPPORTED_MASK | \
			      PIDFD_INFO_COREDUMP_SIGNAL | \
			      PIDFD_INFO_COREDUMP_CODE)

/*
 * pidfd_info() - 为 PIDFD_GET_INFO 构造一致、可版本扩展的任务快照。
 *
 * @file：借用 pidfd file，保证 pid 存活。
 * @cmd：编码用户结构大小的 ioctl 命令；编号已由调用者验证。
 * @arg：用户态 struct pidfd_info 指针，必须非 NULL。
 *
 * 过程先校验 ABI 和 PID 命名空间，再读取可在 task 消失后保留的 exit/
 * coredump 快照；若 task 仍在，则持有 task 与 cred 引用，补齐凭据、
 * cgroup 与 PID。最后按用户声明的结构大小复制。
 *
 * 返回 0，或 -EINVAL/-EFAULT/-EREMOTE/-ESRCH/copy_to_user 错误。task 的
 * cleanup 属性在作用域结束时自动 put_task；cred 在使用后显式 put。
 * 本函数会访问用户内存并可能睡眠，输出只是尽力而为的时间点快照。
 */
static long pidfd_info(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct pidfd_info __user *uinfo = (struct pidfd_info __user *)arg;
	struct task_struct *task __free(put_task) = NULL;
	struct pid *pid = pidfd_pid(file);
	size_t usize = _IOC_SIZE(cmd);
	struct pidfd_info kinfo = {};
	/* 下列临时指针均不随输出转移所有权，具体稳定方式见后续阶段。 */
	struct user_namespace *user_ns;
	struct pidfs_attr *attr;
	const struct cred *c;
	__u64 mask;

	/*
	 * 变量地图：
	 *   uinfo/usize  用户缓冲及其 ABI 版本大小；
	 *   pid         file 持有的稳定身份；
	 *   attr        退出后仍存活的附加快照；
	 *   task        自动释放的可选 task 引用；
	 *   kinfo/mask  内核零初始化输出及用户请求位；
	 *   c/user_ns   临时 cred 引用和用于 UID/GID 映射的调用者 userns。
	 */
	BUILD_BUG_ON(sizeof(struct pidfd_info) != PIDFD_INFO_SIZE_VER3);

	/* 阶段 1：拒绝无法承载最早 ABI 的缓冲，并只读取固定位置的请求 mask。 */
	if (!uinfo)
		return -EINVAL;
	if (usize < PIDFD_INFO_SIZE_VER0)
		return -EINVAL; /* First version, no smaller struct possible */
	/* VER0 已是首版最小布局，不存在可安全接受的更短结构。 */

	if (copy_from_user(&mask, &uinfo->mask, sizeof(mask)))
		return -EFAULT;

	/*
	 * Restrict information retrieval to tasks within the caller's pid
	 * namespace hierarchy.
	 */
	/*
	 * 只允许查询调用者 PID 命名空间祖先链内可表示的任务。
	 * -EREMOTE 区分“身份存在但位于不可表示的命名空间分支”与 -ESRCH。
	 */
	if (!pid_in_current_pidns(pid))
		return -EREMOTE;

	/*
	 * 阶段 2：读取退出后快照。pidfd 已注册时 attr 是稳定对象，直到 pid
	 * 最终释放；READ_ONCE 防止编译器拆分/重复读取共享指针。
	 */
	attr = READ_ONCE(pid->attr);
	if (mask & PIDFD_INFO_EXIT) {
		if (test_bit(PIDFS_ATTR_BIT_EXIT, &attr->attr_mask)) {
			/*
			 * 发布位由 pidfs_exit() 在 smp_wmb() 后设置；读屏障确保
			 * 一旦看到位，就同时看到此前写完的 cgroupid/exit_code。
			 */
			smp_rmb();
			kinfo.mask |= PIDFD_INFO_EXIT;
#ifdef CONFIG_CGROUPS
			kinfo.cgroupid = attr->cgroupid;
			kinfo.mask |= PIDFD_INFO_CGROUPID;
#endif
			kinfo.exit_code = attr->exit_code;
		}
	}

	if (mask & PIDFD_INFO_COREDUMP) {
		if (test_bit(PIDFS_ATTR_BIT_COREDUMP, &attr->attr_mask)) {
			/* 与 pidfs_coredump() 的写屏障配对，拒绝半写 coredump 组。 */
			smp_rmb();
			kinfo.mask |= PIDFD_INFO_COREDUMP | PIDFD_INFO_COREDUMP_SIGNAL | PIDFD_INFO_COREDUMP_CODE;
			kinfo.coredump_mask = attr->coredump_mask;
			kinfo.coredump_signal = attr->coredump_signal;
			kinfo.coredump_code = attr->coredump_code;
		}
	}

	/*
	 * 阶段 3：尝试取得活 task 引用。身份可比 task 活得更久，NULL 不意味
	 * pidfd 无效，只意味实时凭据/PID 已不可读取。
	 */
	task = get_pid_task(pid, PIDTYPE_PID);
	if (!task) {
		/*
		 * If the task has already been reaped, only exit
		 * information is available
		 */
		/*
		 * task 已被收割后只能返回提前持久化的退出信息。
		 * 用户未请求 EXIT 时返回 -ESRCH，避免把全零实时字段当真。
		 */
		if (!(mask & PIDFD_INFO_EXIT))
			return -ESRCH;

		goto copy_out;
	}

	/* cred 通过独立引用稳定；task 引用本身并不会冻结其 cred 指针。 */
	c = get_task_cred(task);
	if (!c)
		return -ESRCH;

	if ((mask & PIDFD_INFO_COREDUMP) && !kinfo.coredump_mask) {
		/*
		 * 尚未发生实际 coredump 时仍报告当前 dumpable 策略，但不设置
		 * “触发信号/代码”位；这区分能力/策略与已经发生的事件。
		 */
		kinfo.coredump_mask = pidfs_coredump_mask(task_exec_state_get_dumpable(task));
		kinfo.mask |= PIDFD_INFO_COREDUMP;
		/* No coredump actually took place, so no coredump signal. */
		/* 未实际生成 core，因此没有可报告的 coredump 触发信号。 */
	}

	/* Unconditionally return identifiers and credentials, the rest only on request */
	/*
	 * 只要 task 仍存在，身份和凭据总是返回；其余字段由 mask
	 * 请求控制。UID/GID 被映射到调用者 user namespace，无法表示时使用
	 * munged 值，绝不直接泄露内核 kuid/kgid。
	 */

	user_ns = current_user_ns();
	/*
	 * 以下八项是 cred 布局到 pidfd_info 的逐字段机械投影；共同使用同一
	 * user_ns 做 munged 映射，最后一次性发布 CREDS 有效位。
	 */
	kinfo.ruid = from_kuid_munged(user_ns, c->uid);
	kinfo.rgid = from_kgid_munged(user_ns, c->gid);
	kinfo.euid = from_kuid_munged(user_ns, c->euid);
	kinfo.egid = from_kgid_munged(user_ns, c->egid);
	/* saved-id 与 fs-id 继续沿用同一 userns 映射，不能混用 init_user_ns。 */
	kinfo.suid = from_kuid_munged(user_ns, c->suid);
	kinfo.sgid = from_kgid_munged(user_ns, c->sgid);
	kinfo.fsuid = from_kuid_munged(user_ns, c->fsuid);
	kinfo.fsgid = from_kgid_munged(user_ns, c->fsgid);
	kinfo.mask |= PIDFD_INFO_CREDS;
	put_cred(c);

#ifdef CONFIG_CGROUPS
	if (!kinfo.cgroupid) {
		struct cgroup *cgrp;

		/*
		 * 退出快照未提供 cgroup 时，RCU 下读取任务当前默认 cgroup。
		 * cgroup_id 在临界区内提取为值，离开后不保留裸 cgrp 指针。
		 */
		rcu_read_lock();
		cgrp = task_dfl_cgroup(task);
		kinfo.cgroupid = cgroup_id(cgrp);
		kinfo.mask |= PIDFD_INFO_CGROUPID;
		rcu_read_unlock();
	}
#endif

	/*
	 * Copy pid/tgid last, to reduce the chances the information might be
	 * stale. Note that it is not possible to ensure it will be valid as the
	 * task might return as soon as the copy_to_user finishes, but that's ok
	 * and userspace expects that might happen and can act accordingly, so
	 * this is just best-effort. What we can do however is checking that all
	 * the fields are set correctly, or return ESRCH to avoid providing
	 * incomplete information. */
	/*
	 * 最后读取 pid/tgid/ppid 以缩短它们到 copy_to_user 的陈旧
	 * 窗口。无法保证复制完成时任务仍活着，这是 pidfd info 明示允许的
	 * 竞态；但若当前已映射为 0，则以 -ESRCH 拒绝不完整快照。
	 */

	kinfo.ppid = task_ppid_vnr(task);
	kinfo.tgid = task_tgid_vnr(task);
	kinfo.pid = task_pid_vnr(task);
	kinfo.mask |= PIDFD_INFO_PID;

	if (kinfo.pid == 0 || kinfo.tgid == 0)
		return -ESRCH;

copy_out:
	/*
	 * 阶段 4：无论 task 是否尚存，都可追加“支持位集合”；随后检查内核
	 * 没有生成集合外的 mask，并用 copy_struct_to_user 处理 ABI 伸缩。
	 */
	if (mask & PIDFD_INFO_SUPPORTED_MASK) {
		kinfo.mask |= PIDFD_INFO_SUPPORTED_MASK;
		kinfo.supported_mask = PIDFD_INFO_SUPPORTED;
	}

	/* Are there bits in the return mask not present in PIDFD_INFO_SUPPORTED? */
	/* 若返回位不在声明集合中，说明实现与 UAPI 能力表失配。 */
	WARN_ON_ONCE(~PIDFD_INFO_SUPPORTED & kinfo.mask);
	/*
	 * If userspace and the kernel have the same struct size it can just
	 * be copied. If userspace provides an older struct, only the bits that
	 * userspace knows about will be copied. If userspace provides a new
	 * struct, only the bits that the kernel knows about will be copied.
	 */
	/*
	 * 同版本整块复制；旧用户只接收其已知前缀；新用户只接收
	 * 旧内核已知部分。这使结构体可以尾部扩展而不破坏双向兼容。
	 */
	return copy_struct_to_user(uinfo, usize, &kinfo, sizeof(kinfo), NULL);
}

/*
 * pidfs_ioctl_valid() - 在接触任务状态前验证 pidfd ioctl 编码。
 *
 * @cmd：用户 ioctl 命令。固定命令按完整值匹配；可扩展 GET_INFO 还校验
 * direction/type/最小结构大小，以降低把普通 fd 误当 pidfd 的概率。
 * 返回布尔值，无状态变化、无睡眠。
 */
static bool pidfs_ioctl_valid(unsigned int cmd)
{
	/*
	 * 固定大小命令按完整 cmd 值构成机械白名单；这里不按 _IOC_NR()
	 * 放宽匹配，避免错误 direction/size 的编码被当成合法命令。
	 */
	switch (cmd) {
	case FS_IOC_GETVERSION:
	case PIDFD_GET_CGROUP_NAMESPACE:
	case PIDFD_GET_IPC_NAMESPACE:
	case PIDFD_GET_MNT_NAMESPACE:
	case PIDFD_GET_NET_NAMESPACE:
	case PIDFD_GET_PID_FOR_CHILDREN_NAMESPACE:
	/* 后半组仍是同一固定白名单，只按功能类别换行，不改变匹配语义。 */
	case PIDFD_GET_TIME_NAMESPACE:
	case PIDFD_GET_TIME_FOR_CHILDREN_NAMESPACE:
	case PIDFD_GET_UTS_NAMESPACE:
	case PIDFD_GET_USER_NAMESPACE:
	case PIDFD_GET_PID_NAMESPACE:
		return true;
	}

	/* Extensible ioctls require some more careful checks. */
	/* 可扩展 ioctl 的 size 位可变，不能仅用完整 cmd 常量相等判断。 */
	switch (_IOC_NR(cmd)) {
	case _IOC_NR(PIDFD_GET_INFO):
		/*
		 * Try to prevent performing a pidfd ioctl when someone
		 * erronously mistook the file descriptor for a pidfd.
		 * This is not perfect but will catch most cases.
		 */
		/*
		 * 额外验证无法形成密码学级类型保证，但能捕获大多数
		 * 错 fd 或错误结构大小，避免无意执行 pidfd 专用操作。
		 */
		return extensible_ioctl_valid(cmd, PIDFD_GET_INFO, PIDFD_INFO_SIZE_VER0);
	}

	return false;
}

/*
 * pidfd_ioctl() - 分派 pidfd 元数据与命名空间导出操作。
 *
 * @file：借用 pidfd file，其 inode 持有 struct pid。
 * @cmd：已编码操作与可能的用户结构大小。
 * @arg：GETVERSION/GET_INFO 使用的用户指针；命名空间命令必须为 0。
 *
 * 固定元数据走快捷路径；命名空间命令先持有 task，再在 task_lock 下
 * 稳定并增持 nsproxy，执行 ptrace FSCREDS 权限检查，然后为目标
 * namespace 取得引用并交给 open_namespace() 消费。
 *
 * 返回新 namespace fd、0，或精确 errno。task/nsproxy 的 cleanup
 * 属性负责所有提前返回；ns_common 引用无条件转移给 open_namespace。
 * 用户访问、权限检查和 fd 创建均可能睡眠。
 */
static long pidfd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct task_struct *task __free(put_task) = NULL;
	struct nsproxy *nsp __free(put_nsproxy) = NULL;
	struct ns_common *ns_common = NULL;

	/*
	 * 变量地图：
	 *   task  自动 put 的目标 task 引用；
	 *   nsp   自动 put 的 nsproxy 快照，隔离并发 setns/exit；
	 *   ns_common 最终要转移给 nsfs 的具体 namespace 引用。
	 */
	if (!pidfs_ioctl_valid(cmd))
		return -ENOIOCTLCMD;

	/* 阶段 1：inode generation 是纯 VFS 元数据，不需要查 task。 */
	if (cmd == FS_IOC_GETVERSION) {
		if (!arg)
			return -EINVAL;

		__u32 __user *argp = (__u32 __user *)arg;
		return put_user(file_inode(file)->i_generation, argp);
	}

	/* Extensible IOCTL that does not open namespace FDs, take a shortcut */
	/* GET_INFO 自行处理版本化用户结构，也不创建 namespace fd。 */
	if (_IOC_NR(cmd) == _IOC_NR(PIDFD_GET_INFO))
		return pidfd_info(file, cmd, arg);

	/* 阶段 2：namespace 属于活 task；task 已收割后不能从 pid 身份恢复。 */
	task = get_pid_task(pidfd_pid(file), PIDTYPE_PID);
	if (!task)
		return -ESRCH;

	if (arg)
		return -EINVAL;

	/*
	 * task->nsproxy 可在退出/切换命名空间时改变。task_lock 下读取并
	 * get_nsproxy() 后，离锁仍可安全解引用各 namespace 指针。
	 */
	scoped_guard(task_lock, task) {
		nsp = task->nsproxy;
		if (nsp)
			get_nsproxy(nsp);
	}
	if (!nsp)
		return -ESRCH; /* just pretend it didn't exist */
	/* nsproxy 已消失时对外按目标不存在处理，避免暴露退出中间态。 */

	/*
	 * We're trying to open a file descriptor to the namespace so perform a
	 * filesystem cred ptrace check. Also, we mirror nsfs behavior.
	 */
	/*
	 * 创建 namespace fd 会暴露目标的命名空间对象，因此使用
	 * 文件系统凭据模式做 ptrace 访问检查，并与 nsfs 的权限语义一致。
	 */
	if (!ptrace_may_access(task, PTRACE_MODE_READ_FSCREDS))
		return -EACCES;

	switch (cmd) {
	/* Namespaces that hang of nsproxy. */
	/*
	 * 以下 namespace 由 nsproxy 聚合。配置关闭或引用已无法
	 * 获取时保持 ns_common==NULL，统一返回 -EOPNOTSUPP。
	 */
	case PIDFD_GET_CGROUP_NAMESPACE:
#ifdef CONFIG_CGROUPS
		if (!ns_ref_get(nsp->cgroup_ns))
			break;
		ns_common = to_ns_common(nsp->cgroup_ns);
#endif
		break;
	case PIDFD_GET_IPC_NAMESPACE:
#ifdef CONFIG_IPC_NS
		if (!ns_ref_get(nsp->ipc_ns))
			break;
		ns_common = to_ns_common(nsp->ipc_ns);
#endif
		break;
	/* mount namespace 总是存在，但仍须用 ns_ref_get() 防止并发退出竞态。 */
	case PIDFD_GET_MNT_NAMESPACE:
		if (!ns_ref_get(nsp->mnt_ns))
			break;
		ns_common = to_ns_common(nsp->mnt_ns);
		break;
	/*
	 * 这一段配置化 case 重复同一所有权模板：先 ns_ref_get()，成功后
	 * 才把具体对象转为 ns_common；配置关闭或加引用失败都落到统一错误。
	 */
	case PIDFD_GET_NET_NAMESPACE:
#ifdef CONFIG_NET_NS
		if (!ns_ref_get(nsp->net_ns))
			break;
		ns_common = to_ns_common(nsp->net_ns);
#endif
		break;
	case PIDFD_GET_PID_FOR_CHILDREN_NAMESPACE:
#ifdef CONFIG_PID_NS
		if (!ns_ref_get(nsp->pid_ns_for_children))
			break;
		ns_common = to_ns_common(nsp->pid_ns_for_children);
#endif
		break;
	/* time namespace 有 current/children 两个槽，二者必须分别取得引用。 */
	case PIDFD_GET_TIME_NAMESPACE:
#ifdef CONFIG_TIME_NS
		if (!ns_ref_get(nsp->time_ns))
			break;
		ns_common = to_ns_common(nsp->time_ns);
#endif
		break;
	case PIDFD_GET_TIME_FOR_CHILDREN_NAMESPACE:
#ifdef CONFIG_TIME_NS
		if (!ns_ref_get(nsp->time_ns_for_children))
			break;
		ns_common = to_ns_common(nsp->time_ns_for_children);
#endif
		break;
	/* nsproxy 聚合表的最后一项仍遵守“加引用成功后再发布”模板。 */
	case PIDFD_GET_UTS_NAMESPACE:
#ifdef CONFIG_UTS_NS
		if (!ns_ref_get(nsp->uts_ns))
			break;
		ns_common = to_ns_common(nsp->uts_ns);
#endif
		break;
	/* Namespaces that don't hang of nsproxy. */
	/*
	 * user namespace 属于 cred，active PID namespace 由 pid
	 * 关系推导，不在 nsproxy 中；二者在 RCU 临界区取得裸指针并立即
	 * ns_ref_get()，离开临界区后只使用持有引用。
	 */
	case PIDFD_GET_USER_NAMESPACE:
#ifdef CONFIG_USER_NS
		scoped_guard(rcu) {
			struct user_namespace *user_ns;

			user_ns = task_cred_xxx(task, user_ns);
			if (ns_ref_get(user_ns))
				ns_common = to_ns_common(user_ns);
		}
#endif
		break;
	/* active PID namespace 同样在 RCU 内取指针，并以独立引用带出临界区。 */
	case PIDFD_GET_PID_NAMESPACE:
#ifdef CONFIG_PID_NS
		scoped_guard(rcu) {
			struct pid_namespace *pid_ns;

			pid_ns = task_active_pid_ns(task);
			if (ns_ref_get(pid_ns))
				ns_common = to_ns_common(pid_ns);
		}
#endif
		break;
	/* 验证器理论上已排除未知 cmd；default 保留为防御性二次校验。 */
	default:
		return -ENOIOCTLCMD;
	}

	/* 阶段 4：只有已经持有引用的对象才能跨越 switch 交给 nsfs。 */
	if (!ns_common)
		return -EOPNOTSUPP;

	/* open_namespace() unconditionally consumes the reference */
	/*
	 * open_namespace() 无论成功失败都会消费 ns_common 引用，
	 * 所以此后本函数不得 put；这是一处明确的 ownership 转移边界。
	 */
	return open_namespace(ns_common);
}

/*
 * pidfs_file_release() - 带 PIDFD_AUTOKILL 的 file 关闭时杀死目标线程组。
 *
 * @inode：pidfs inode，i_private 借用 struct pid。
 * @file：正在释放的 file；f_flags 决定是否启用 AUTOKILL。
 * 无论是否发送信号都返回 0。RCU 临界区稳定 task 裸指针，信号以
 * SEND_SIG_PRIV 发给 PIDTYPE_TGID；当前明确排除内核线程和 user worker。
 */
static int pidfs_file_release(struct inode *inode, struct file *file)
{
	struct pid *pid = inode->i_private;
	struct task_struct *task;

	if (!(file->f_flags & PIDFD_AUTOKILL))
		return 0;

	/* 只在 RCU 保护范围内使用 pid_task() 返回的借用 task 指针。 */
	guard(rcu)();
	task = pid_task(pid, PIDTYPE_TGID);
	if (!task)
		return 0;

	/* Not available for kthreads or user workers for now. */
	/* 当前 AUTOKILL 契约不覆盖内核线程或 user worker，异常使用仅告警。 */
	if (WARN_ON_ONCE(task->flags & (PF_KTHREAD | PF_USER_WORKER)))
		return 0;
	do_send_sig_info(SIGKILL, SEND_SIG_PRIV, task, PIDTYPE_TGID);
	return 0;
}

/*
 * pidfs_file_operations：把 pidfd 接入关闭、退出通知、fdinfo 与 ioctl。
 * compat ioctl 复用指针参数转换，因为命令 ABI 本身已经版本化。
 */
static const struct file_operations pidfs_file_operations = {
	.release	= pidfs_file_release,
	.poll		= pidfd_poll,
#ifdef CONFIG_PROC_FS
	.show_fdinfo	= pidfd_show_fdinfo,
#endif
	.unlocked_ioctl	= pidfd_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/*
 * pidfd_pid() - 从经过类型验证的 pidfd file 取出借用 struct pid。
 *
 * @file：调用者持有引用的 file。只有 f_op 精确等于 pidfs 操作表才是
 * pidfd；否则返回 ERR_PTR(-EBADF)。成功指针不增 pid 引用，其寿命由
 * file->inode 持有的引用覆盖，调用者不得越过 file 生命周期保存裸指针。
 */
struct pid *pidfd_pid(const struct file *file)
{
	if (file->f_op != &pidfs_file_operations)
		return ERR_PTR(-EBADF);
	return file_inode(file)->i_private;
}

/*
 * We're called from release_task(). We know there's at least one
 * reference to struct pid being held that won't be released until the
 * task has been reaped which cannot happen until we're out of
 * release_task().
 *
 * If this struct pid has at least once been referred to by a pidfd then
 * pid->attr will be allocated. If not we mark the struct pid as dead so
 * anyone who is trying to register it with pidfs will fail to do so.
 * Otherwise we would hand out pidfs for reaped tasks without having
 * exit information available.
 *
 * Worst case is that we've filled in the info and the pid gets freed
 * right away in free_pid() when no one holds a pidfd anymore. Since
 * pidfs_exit() currently is placed after exit_task_work() we know that
 * it cannot be us aka the exiting task holding a pidfd to itself.
 */
/*
 *
 * 本函数由 release_task() 调用；至少还有一份 struct pid 引用要等任务
 * 被收割后才释放，因此写入期间 pid 不会消失。若 pid 曾被 pidfd 引用，
 * attr 已分配，退出信息写入其中；否则把 attr 设为 -ESRCH 哨兵，阻止
 * 退出后才注册一个缺失退出快照的 pidfd。
 *
 * 即便信息刚发布后 pid 立即在 free_pid() 中释放也安全：没有 pidfd
 * 就没有读者；若存在/正在取得 stashed dentry，则对应引用会延长 pid
 * 与 attr 寿命。当前调用点位于 exit_task_work() 之后，也排除了退出
 * 任务自己持有最后一个 self pidfd 的特殊释放环。
 *
 * pidfs_exit() - 冻结任务退出信息并发布给 PIDFD_GET_INFO。
 *
 * @tsk：正在 release_task 路径中的借用 task，task_pid(tsk) 仍稳定。
 * 可睡眠；无直接返回。成功后 attr 的 EXIT 位为 1，读者可读取完整快照；
 * 若从未创建 pidfd，则发布 PIDFS_PID_DEAD 哨兵。函数不释放 pid/attr。
 */
void pidfs_exit(struct task_struct *tsk)
{
	struct pid *pid = task_pid(tsk);
	struct pidfs_attr *attr;
#ifdef CONFIG_CGROUPS
	struct cgroup *cgrp;
#endif

	might_sleep();

	/* Synchronize with pidfs_register_pid(). */
	/*
	 * 与 pidfs_register_pid() 串行化，决定 NULL attr 最终由
	 * “注册真实对象”还是“退出写死哨兵”占有，避免退出后无快照 pidfd。
	 */
	scoped_guard(spinlock_irq, &pid->wait_pidfd.lock) {
		attr = pid->attr;
		if (!attr) {
			/*
			 * No one ever held a pidfd for this struct pid.
			 * Mark it as dead so no one can add a pidfs
			 * entry anymore. We're about to be reaped and
			 * so no exit information would be available.
			 */
			/*
			 * 从未有人持有 pidfd；任务即将收割，之后无法再
			 * 构造 exit 信息，因此写错误哨兵并永久拒绝迟到注册。
			 */
			pid->attr = PIDFS_PID_DEAD;
			return;
		}
	}

	/*
	 * If @pid->attr is set someone might still legitimately hold a
	 * pidfd to @pid or someone might concurrently still be getting
	 * a reference to an already stashed dentry from @pid->stashed.
	 * So defer cleaning @pid->attr until the last reference to @pid
	 * is put
	 */
	/*
	 * attr 非空意味着现有 pidfd 或并发 stashed dentry 获取者
	 * 仍可能读它，所以这里只填充，不清理；最终释放归最后一个 pid 引用。
	 */

#ifdef CONFIG_CGROUPS
	/*
	 * 阶段 2：在 RCU 下抓取退出时默认 cgroup 的稳定数值 ID，不把 cgrp
	 * 裸指针保存到 attr；随后记录 wait 语义使用的 exit_code。
	 */
	rcu_read_lock();
	cgrp = task_dfl_cgroup(tsk);
	attr->cgroupid = cgroup_id(cgrp);
	rcu_read_unlock();
#endif
	attr->exit_code = tsk->exit_code;

	/* Ensure that PIDFD_GET_INFO sees either all or nothing. */
	/*
	 * 先写数据，再用写屏障约束，最后原子置发布位。读侧看到
	 * EXIT 位后执行 smp_rmb()，因此只可能看到“未发布”或完整字段组。
	 */
	smp_wmb();
	set_bit(PIDFS_ATTR_BIT_EXIT, &attr->attr_mask);
}

#ifdef CONFIG_COREDUMP
/*
 * pidfs_coredump() - 在生成 core 时记录策略和触发原因并原子发布。
 *
 * @cprm：coredump 路径持有的只读参数，包含稳定 pid、dumpable 与 siginfo。
 * 调用前 pidfd attr 必须已注册且非 DEAD；无直接返回，不取得长期引用。
 * 成功后 COREDUMP 发布位承诺三个字段完整，PIDFD_GET_INFO 可在 task 消失
 * 后读取。函数处于 coredump 上下文，不在此分配内存。
 */
void pidfs_coredump(const struct coredump_params *cprm)
{
	struct pid *pid = cprm->pid;
	struct pidfs_attr *attr;

	attr = READ_ONCE(pid->attr);

	VFS_WARN_ON_ONCE(!attr);
	VFS_WARN_ON_ONCE(attr == PIDFS_PID_DEAD);

	/* Note how we were coredumped and that we coredumped. */
	/* 记录采用何种权限策略，并额外标记“实际已经生成过 core”。 */
	attr->coredump_mask = pidfs_coredump_mask(cprm->dumpable) |
			      PIDFD_COREDUMPED;
	/* If coredumping is set to skip we should never end up here. */
	/* SKIP 与进入真实 coredump 路径矛盾，出现说明上游状态机失配。 */
	VFS_WARN_ON_ONCE(attr->coredump_mask & PIDFD_COREDUMP_SKIP);
	/* Expose the signal number and code that caused the coredump. */
	/* 保存触发信号及 si_code；它们与策略共同组成不可拆分的发布组。 */
	attr->coredump_signal = cprm->siginfo->si_signo;
	attr->coredump_code = cprm->siginfo->si_code;
	/* 与 pidfd_info() 的 smp_rmb() 配对，禁止发布位越过字段写入。 */
	smp_wmb();
	set_bit(PIDFS_ATTR_BIT_COREDUMP, &attr->attr_mask);
}
#endif

/*
 * pidfs_mnt：初始化时创建、运行期只读的内核内部挂载；所有 stashed path
 * 都以它为根。挂载生命周期覆盖全部 pidfd，因此读者通常只增 path 引用。
 */
static struct vfsmount *pidfs_mnt __ro_after_init;

/*
 * The vfs falls back to simple_setattr() if i_op->setattr() isn't
 * implemented. Let's reject it completely until we have a clean
 * permission concept for pidfds.
 */
/*
 * 若文件系统未实现 setattr，VFS 会回退到 simple_setattr()，
 * 从而允许 chmod/chown 等通用修改。pidfd 尚无清晰权限模型，因此显式
 * 接入 anon_inode_setattr()，把允许范围收紧到匿名 inode 契约。
 *
 * pidfs_setattr() - 以匿名 inode 规则处理或拒绝 pidfs 属性修改。
 * 参数均借用自 VFS setattr 路径；返回 0 或 helper errno，副作用和锁
 * 要求完全由 anon_inode_setattr() 契约决定。
 */
static int pidfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			 struct iattr *attr)
{
	return anon_inode_setattr(idmap, dentry, attr);
}

/*
 * pidfs_getattr() - 用匿名 inode 语义填充 pidfd 的 stat 结果。
 *
 * @idmap/@path/@request_mask/@query_flags：VFS 借用输入；@stat 为输出。
 * 返回 0 或 helper errno，不改变 pidfs 生命周期。
 */
static int pidfs_getattr(struct mnt_idmap *idmap, const struct path *path,
			 struct kstat *stat, u32 request_mask,
			 unsigned int query_flags)
{
	return anon_inode_getattr(idmap, path, stat, request_mask, query_flags);
}

/*
 * pidfs_listxattr() - 枚举一个 pidfd inode 的扩展属性名。
 *
 * @dentry：借用且正被 VFS 稳定的 dentry；@buf 可空（只查询长度）；
 * @size 是字节容量。attr 与 inode/pid 同寿命，返回所需/写入字节数或
 * 负 errno；读取同步由 simple_xattr 的 RCU 协议提供。
 */
static ssize_t pidfs_listxattr(struct dentry *dentry, char *buf, size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct pid *pid = inode->i_private;

	return simple_xattr_list(inode, &pid->attr->xattrs, buf, size);
}

/* inode 操作表集中定义 stat、受限 setattr 与 xattr 枚举的分派边界。 */
static const struct inode_operations pidfs_inode_operations = {
	.getattr	= pidfs_getattr,
	.setattr	= pidfs_setattr,
	.listxattr	= pidfs_listxattr,
};

/*
 * pidfs_evict_inode() - 在 VFS 最终驱逐 inode 时释放其 struct pid 引用。
 *
 * @inode：已不可再被新查找获得的 inode；clear_inode() 先结束 VFS 状态，
 * 再 put_pid() 与 pidfs_init_inode() 的持有引用配对。无返回，可触发 pid
 * 的最终释放，因而顺序不可倒置。
 */
static void pidfs_evict_inode(struct inode *inode)
{
	struct pid *pid = inode->i_private;

	clear_inode(inode);
	put_pid(pid);
}

/*
 * superblock 操作表：不把 inode 写回磁盘，drop 时直接丢弃；evict 负责
 * pid 引用闭环，statfs 使用伪文件系统通用实现。
 */
static const struct super_operations pidfs_sops = {
	.drop_inode	= inode_just_drop,
	.evict_inode	= pidfs_evict_inode,
	.statfs		= simple_statfs,
};

/*
 * 'lsof' has knowledge of out historical anon_inode use, and expects
 * the pidfs dentry name to start with 'anon_inode'.
 */
/*
 * lsof 了解历史上 pidfd 使用 anon_inode 的表现，依赖名称以
 * "anon_inode" 开头。pidfs 虽已有独立 inode，仍保留该显示 ABI 兼容。
 *
 * pidfs_dname() - 动态格式化 pidfd dentry 的兼容显示名。
 * @dentry 仅为接口占位；返回 @buffer 内字符串起点或 dynamic_dname()
 * 的错误编码，不保存指针。
 */
static char *pidfs_dname(struct dentry *dentry, char *buffer, int buflen)
{
	return dynamic_dname(buffer, buflen, "anon_inode:[pidfd]");
}

/*
 * dentry 操作表：d_dname 提供兼容名称，d_prune 在 dentry 摘除时清空
 * pid->stashed，确保不会留下指向已释放 dentry 的缓存指针。
 */
const struct dentry_operations pidfs_dentry_operations = {
	.d_dname	= pidfs_dname,
	.d_prune	= stashed_dentry_prune,
};

/*
 * pidfs_encode_fh() - 把 pidfs inode 编码为可由 exportfs 恢复的 64 位句柄。
 *
 * @inode：借用，i_private 指向稳定 pid；@fh 为 u32 输出数组；
 * @max_len 输入容量/输出所需长度（两个 u32）；@parent 未使用，因为
 * pidfs 无目录层级。容量不足返回 FILEID_INVALID，否则写 pid->ino 并
 * 返回 FILEID_KERNFS。只复制数值，不取得 pid 引用。
 */
static int pidfs_encode_fh(struct inode *inode, u32 *fh, int *max_len,
			   struct inode *parent)
{
	const struct pid *pid = inode->i_private;

	/* 阶段 1：先按 exportfs 协议回报最低容量，绝不部分写入句柄。 */
	if (*max_len < 2) {
		*max_len = 2;
		return FILEID_INVALID;
	}

	/* 阶段 2：容量满足后写入完整 64 位 ino，并返回与解码端约定的类型。 */
	*max_len = 2;
	*(u64 *)fh = pid->ino;
	return FILEID_KERNFS;
}

/* Find a struct pid based on the inode number. */
/* 根据 pidfs 的 64 位 inode 身份查找仍可导出的 struct pid。 */
/*
 * pidfs_ino_get_pid() - 在 RCU 哈希中查找并稳定一个未退出、当前可见的 pid。
 *
 * @ino：file handle 中的完整 64 位键。返回持有引用的 struct pid 或 NULL；
 * 调用者必须 put_pid()，或把引用转移给 stashed path/inode。
 *
 * RCU 保证查找期间对象内存不释放；attr 非空且非错误证明 pidfd 注册成立，
 * EXIT 位排除已退出身份，pid_vnr()!=0 排除调用者 PID 命名空间之外对象。
 * get_pid() 在退出 RCU 前把临时可见性转换成长期生命周期保证。
 */
static struct pid *pidfs_ino_get_pid(u64 ino)
{
	struct pid *pid;
	struct pidfs_attr *attr;

	/* 查找、验证与增引用必须全部位于同一 RCU 临界区。 */
	guard(rcu)();
	pid = rhashtable_lookup(&pidfs_ino_ht, &ino, pidfs_ino_ht_params);
	if (!pid)
		return NULL;
	attr = READ_ONCE(pid->attr);
	if (IS_ERR_OR_NULL(attr))
		return NULL;
	if (test_bit(PIDFS_ATTR_BIT_EXIT, &attr->attr_mask))
		return NULL;
	/* Within our pid namespace hierarchy? */
	/* 只允许当前 PID namespace 祖先链中有非零编号的身份。 */
	if (pid_vnr(pid) == 0)
		return NULL;
	return get_pid(pid);
}

/*
 * pidfs_fh_to_dentry() - 从 exportfs 文件句柄恢复或创建 pidfs dentry。
 *
 * @sb：pidfs superblock（本实现无需单独使用）；@fid/@fh_len/@fh_type
 * 描述用户句柄。成功返回持有引用的 dentry；无匹配返回 NULL；创建失败
 * 返回 ERR_PTR。pidfs_ino_get_pid() 的 pid 引用交给 path_from_stashed()：
 * 成功时 inode 接管，失败时 helper 按 stashed 协议清理。
 */
static struct dentry *pidfs_fh_to_dentry(struct super_block *sb,
					 struct fid *fid, int fh_len,
					 int fh_type)
{
	int ret;
	u64 pid_ino;
	struct path path;
	struct pid *pid;

	/* 阶段 1：只接受本实现编码的两个 u32、FILEID_KERNFS 格式。 */
	if (fh_len < 2)
		return NULL;

	switch (fh_type) {
	case FILEID_KERNFS:
		pid_ino = *(u64 *)fid;
		break;
	default:
		return NULL;
	}

	/* 阶段 2：把 RCU 查找转成持有 pid 引用，再唯一化 VFS path。 */
	pid = pidfs_ino_get_pid(pid_ino);
	if (!pid)
		return NULL;

	ret = path_from_stashed(&pid->stashed, pidfs_mnt, pid, &path);
	if (ret < 0)
		return ERR_PTR(ret);

	VFS_WARN_ON_ONCE(!pid->attr);

	/*
	 * 返回接口只需要 dentry 引用；path_from_stashed() 同时给出 mount
	 * 引用，因此在转交 dentry 前单独 mntput()，避免泄漏挂载引用。
	 */
	mntput(path.mnt);
	return path.dentry;
}

/*
 * Make sure that we reject any nonsensical flags that users pass via
 * open_by_handle_at(). Note that PIDFD_THREAD is defined as O_EXCL, and
 * PIDFD_NONBLOCK as O_NONBLOCK.
 */
/*
 * open_by_handle_at() 传入的无意义 flag 必须拒绝。
 * PIDFD_THREAD 复用 O_EXCL、PIDFD_NONBLOCK 复用 O_NONBLOCK，因此合法
 * 集合既含普通打开方式，也含这两个 pidfd ABI 位。
 */
#define VALID_FILE_HANDLE_OPEN_FLAGS \
	(O_RDONLY | O_WRONLY | O_RDWR | O_NONBLOCK | O_CLOEXEC | O_EXCL)

/*
 * pidfs_export_permission() - 校验 open_by_handle_at 的 flags。
 *
 * @ctx：exportfs 已建立的恢复上下文，本函数无需再检查；@oflags 为用户
 * 打开位。仅允许声明集合及 VFS 强加的 O_LARGEFILE；返回 0/-EINVAL。
 */
static int pidfs_export_permission(struct handle_to_path_ctx *ctx,
				   unsigned int oflags)
{
	if (oflags & ~(VALID_FILE_HANDLE_OPEN_FLAGS | O_LARGEFILE))
		return -EINVAL;

	/*
	 * pidfd_ino_get_pid() will verify that the struct pid is part
	 * of the caller's pid namespace hierarchy. No further
	 * permission checks are needed.
	 */
	/*
	 * pidfs_ino_get_pid() 已验证 pid 位于调用者可见的 PID
	 * 命名空间层级，所以此处无需重复基于任务的权限检查。
	 */
	return 0;
}

/*
 * pidfs_export_open() - 为已恢复 path 创建普通 pidfd file。
 *
 * @path：exportfs 持有的借用路径；@oflags 已经 permission 校验。
 * open_by_handle_at() 强制的 O_LARGEFILE 对 pidfd 无意义，清除后强制
 * O_RDWR，与常规 pidfd 创建语义一致。返回持有引用 file 或 ERR_PTR。
 */
static struct file *pidfs_export_open(const struct path *path, unsigned int oflags)
{
	/*
	 * Clear O_LARGEFILE as open_by_handle_at() forces it and raise
	 * O_RDWR as pidfds always are.
	 */
	/*
	 * 清除 open_by_handle_at() 自动添加的 O_LARGEFILE，并将
	 * 打开模式提升为 pidfd 固定使用的 O_RDWR。
	 */
	oflags &= ~O_LARGEFILE;
	return dentry_open(path, oflags | O_RDWR, current_cred());
}

/*
 * export 操作表把 64 位 pid 身份的编码、恢复、权限和最终 open 串成
 * open_by_handle_at() 协议；任一步失败都不会暴露半初始化 pidfd。
 */
static const struct export_operations pidfs_export_operations = {
	.encode_fh	= pidfs_encode_fh,
	.fh_to_dentry	= pidfs_fh_to_dentry,
	.open		= pidfs_export_open,
	.permission	= pidfs_export_permission,
};

/*
 * pidfs_init_inode() - 为 stashed dentry 初始化一个持有 pid 引用的 inode。
 *
 * @inode：新分配、尚未发布的 inode；@data 是已持有引用的 struct pid，
 * 成功后 ownership 转入 inode->i_private，最终由 pidfs_evict_inode()
 * put。设置私有/匿名标志、用户访问模式、操作表及 64/32 位身份拆分。
 * 返回 0，无失败路径；发布前所有字段均已完成。
 */
static int pidfs_init_inode(struct inode *inode, void *data)
{
	const struct pid *pid = data;

	inode->i_private = data;
	inode->i_flags |= S_PRIVATE | S_ANON_INODE;
	/* We allow to set xattrs. */
	/* pidfd 允许受 handler 约束的 xattr，因此清除匿名 inode 默认不可变位。 */
	inode->i_flags &= ~S_IMMUTABLE;
	inode->i_mode |= S_IRWXU;
	inode->i_op = &pidfs_inode_operations;
	inode->i_fop = &pidfs_file_operations;
	inode->i_ino = pidfs_ino(pid->ino);
	inode->i_generation = pidfs_gen(pid->ino);
	return 0;
}

/*
 * pidfs_put_data() - 回滚尚未被 inode 接管的 stashed data 引用。
 *
 * @data：path_from_stashed() 传入的持有 pid 引用。无返回；put_pid()
 * 与创建方 get_pid() 配对，可触发最终 pid 释放。
 */
static void pidfs_put_data(void *data)
{
	struct pid *pid = data;
	put_pid(pid);
}

/**
 * pidfs_register_pid_gfp - register a struct pid in pidfs with custom GFP
 * flags
 * @pid: pid to pin
 * @gfp: GFP flags for memory allocation
 *
 * Register a struct pid in pidfs with custom GFP flags.
 *
 * Return: On success zero, on error a negative error code is returned.
 */
/*
 * 使用调用者指定的 GFP 标志，把一个 struct pid 注册到 pidfs
 * 并固定其退出/xattr 属性容器。@pid 为借用指针，可为 NULL（视为无需
 * 注册并成功）；@gfp 决定分配是否可睡眠/允许回收。
 *
 * 快速路径读取现有 attr；需要分配时先在锁外准备 new_attr，再用
 * wait_pidfd.lock 与 pidfs_exit() 竞争最终发布。锁内必须重新检查：
 * 退出可能已写 DEAD，或另一注册者已安装对象。__free(kfree) 自动回滚
 * 未采用候选；no_free_ptr() 在获胜时把 ownership 转给 pid。
 *
 * 返回 0、-ENOMEM 或 -ESRCH。成功后 attr 与 pid 同寿命且 xattr 链已
 * 初始化；函数不额外持有 pid 引用。
 */
int pidfs_register_pid_gfp(struct pid *pid, gfp_t gfp)
{
	struct pidfs_attr *new_attr __free(kfree) = NULL;
	struct pidfs_attr *attr;

	might_sleep();

	if (!pid)
		return 0;

	/* 阶段 1：无锁快速观察稳定的终态；非 NULL 对象安装后不会被替换。 */
	attr = READ_ONCE(pid->attr);
	if (unlikely(attr == PIDFS_PID_DEAD))
		return PTR_ERR(PIDFS_PID_DEAD);
	if (attr)
		return 0;

	/* 阶段 2：锁外分配，避免在 spinlock/IRQ-disabled 区域睡眠。 */
	new_attr = kmem_cache_zalloc(pidfs_attr_cachep, gfp);
	if (!new_attr)
		return -ENOMEM;

	INIT_LIST_HEAD_RCU(&new_attr->xattrs);

	/* Synchronize with pidfs_exit(). */
	/*
	 * 锁内决定 attr 的唯一写者；必须重读，不能依据锁外快照，
	 * 否则可能覆盖退出哨兵或泄漏另一注册者刚安装的对象。
	 */
	guard(spinlock_irq)(&pid->wait_pidfd.lock);

	attr = pid->attr;
	if (unlikely(attr == PIDFS_PID_DEAD))
		return PTR_ERR(PIDFS_PID_DEAD);
	if (unlikely(attr))
		return 0;

	/* 发布成功：取消作用域自动 kfree，释放责任转移到 pidfs_free_pid()。 */
	pid->attr = no_free_ptr(new_attr);
	return 0;
}

/*
 * pidfs_stash_dentry() - 在发布唯一 dentry 前确保 pidfs attr 已注册。
 *
 * @stashed：应精确指向 pid->stashed；@dentry 是尚未发布的候选。
 * 注册失败返回 ERR_PTR，候选由上层清理；成功交给 stash_dentry() 与
 * 并发候选竞争，返回唯一 canonical dentry。函数可能分配并睡眠。
 */
static struct dentry *pidfs_stash_dentry(struct dentry **stashed,
					 struct dentry *dentry)
{
	int ret;
	struct pid *pid = d_inode(dentry)->i_private;

	/* 阶段 1：验证调用者传入的缓存槽确实属于候选 inode 的 pid。 */
	VFS_WARN_ON_ONCE(stashed != &pid->stashed);

	/* 阶段 2：先建立退出快照等附属状态，失败时不发布候选 dentry。 */
	ret = pidfs_register_pid(pid);
	if (ret)
		return ERR_PTR(ret);

	/* 阶段 3：与并发候选竞争，返回已存在者或发布当前 canonical dentry。 */
	return stash_dentry(stashed, dentry);
}

/*
 * stashed 操作表定义候选 dentry 的注册、inode 初始化和失败引用回滚，
 * 供 path_from_stashed() 以统一协议复用同一 pid 的 VFS 对象。
 */
static const struct stashed_operations pidfs_stashed_ops = {
	.stash_dentry	= pidfs_stash_dentry,
	.init_inode	= pidfs_init_inode,
	.put_data	= pidfs_put_data,
};

/*
 * pidfs_xattr_get() - 从一个 pidfd 的 trusted xattr 链读取属性。
 *
 * @handler/@suffix 组成完整名称；@inode 持有 pid/attr；@value 可空用于
 * 查询长度，@size 为字节容量。返回长度或 errno；simple_xattr 负责 RCU
 * 读侧稳定，不转移 value 或 attr 所有权。
 */
static int pidfs_xattr_get(const struct xattr_handler *handler,
			   struct dentry *unused, struct inode *inode,
			   const char *suffix, void *value, size_t size)
{
	struct pid *pid = inode->i_private;
	const char *name = xattr_full_name(handler, suffix);

	return simple_xattr_get(&pidfs_xa_cache, &pid->attr->xattrs, name, value, size);
}

/*
 * pidfs_xattr_set() - 在 inode 锁下创建、替换或删除 trusted xattr。
 *
 * @idmap/@unused 为 VFS 接口占位；@value/@size/@flags 描述更新。
 * simple_xattr_set() 成功返回被替换的旧对象，当前函数把它交给 RCU 延迟
 * 释放，使无锁读者可完成；失败返回精确 errno。调用者必须持 inode 锁。
 */
static int pidfs_xattr_set(const struct xattr_handler *handler,
			   struct mnt_idmap *idmap, struct dentry *unused,
			   struct inode *inode, const char *suffix,
			   const void *value, size_t size, int flags)
{
	struct pid *pid = inode->i_private;
	const char *name = xattr_full_name(handler, suffix);
	struct simple_xattr *old_xattr;

	/* Ensure we're the only one to set @attr->xattrs. */
	/* inode 锁串行化写者；WARN 用于抓住破坏 simple_xattr 写协议的调用。 */
	WARN_ON_ONCE(!inode_is_locked(inode));

	/* 成功后新值已发布，旧值仅等待既有 RCU 读者退出。 */
	old_xattr = simple_xattr_set(&pidfs_xa_cache, &pid->attr->xattrs, name, value, size, flags);
	if (IS_ERR(old_xattr))
		return PTR_ERR(old_xattr);

	simple_xattr_free_rcu(old_xattr);
	return 0;
}

/* 目前仅开放 trusted.* 命名空间，权限过滤由 VFS/xattr 通用层完成。 */
static const struct xattr_handler pidfs_trusted_xattr_handler = {
	.prefix = XATTR_TRUSTED_PREFIX,
	.get	= pidfs_xattr_get,
	.set	= pidfs_xattr_set,
};

/* NULL 结尾的 handler 表供 VFS 逐项匹配 xattr 前缀。 */
static const struct xattr_handler *const pidfs_xattr_handlers[] = {
	&pidfs_trusted_xattr_handler,
	NULL
};

/*
 * pidfs_init_fs_context() - 配置 pidfs 伪文件系统的 superblock 构造参数。
 *
 * @fc：VFS 新建的 fs_context；成功后 s_fs_info 借用静态 stashed ops。
 * init_pseudo() 失败返回 -ENOMEM；成功设置不缓存 dentry、super/export/
 * dentry/xattr 操作表。尚未挂载，不发布全局 pidfs_mnt。
 */
static int pidfs_init_fs_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx;

	ctx = init_pseudo(fc, PID_FS_MAGIC);
	if (!ctx)
		return -ENOMEM;

	/*
	 * pid->stashed 自己控制唯一 dentry 生命周期，因此 dcache 不应额外
	 * 长期缓存；这样最后一个使用者离开后可及时 prune 并清空 stashed。
	 */
	ctx->s_d_flags |= DCACHE_DONTCACHE;
	ctx->ops = &pidfs_sops;
	ctx->eops = &pidfs_export_operations;
	ctx->dops = &pidfs_dentry_operations;
	ctx->xattr = pidfs_xattr_handlers;
	fc->s_fs_info = (void *)&pidfs_stashed_ops;
	return 0;
}

/* pidfs 类型只供内核挂载；卸载使用匿名 superblock 的通用销毁路径。 */
static struct file_system_type pidfs_type = {
	.name			= "pidfs",
	.init_fs_context	= pidfs_init_fs_context,
	.kill_sb		= kill_anon_super,
};

/*
 * pidfs_alloc_file() - 为稳定 struct pid 创建或复用 pidfd file。
 *
 * @pid：调用者借用的身份；函数通过 get_pid() 把一份引用交给 stashed
 * path/inode。@flags 是 UAPI 与内部 pidfd 位图。
 *
 * path_from_stashed() 返回持有 path，并确保 attr 已注册；随后清除只表示
 * 输入状态的 PIDFD_STALE、强制 O_RDWR，调用 dentry_open()。成功 file
 * 接管 inode/path 引用；局部 path 的 cleanup 始终 path_put。
 *
 * 返回持有引用的 file 或 ERR_PTR。创建可能分配/睡眠；失败路径不会泄漏
 * get_pid() 引用。THREAD/AUTOKILL 在 open 后显式恢复。
 */
struct file *pidfs_alloc_file(struct pid *pid, unsigned int flags)
{
	struct file *pidfd_file;
	struct path path __free(path_put) = {};
	int ret;

	/*
	 * Ensure that internal pidfd flags don't overlap with each
	 * other or with uapi pidfd flags.
	 */
	/*
	 * 编译期确认四个内部/UAPI 复用位彼此独立；若定义重叠，
	 * hweight 不再等于 4，阻止生成含歧义 flag ABI 的内核。
	 */
	BUILD_BUG_ON(hweight32(PIDFD_THREAD | PIDFD_NONBLOCK |
				PIDFD_STALE | PIDFD_AUTOKILL) != 4);

	/* 阶段 1：取得 pid 引用，并把它交给唯一化 path 的成功或回滚协议。 */
	ret = path_from_stashed(&pid->stashed, pidfs_mnt, get_pid(pid), &path);
	if (ret < 0)
		return ERR_PTR(ret);

	VFS_WARN_ON_ONCE(!pid->attr);

	/* 阶段 2：把 pidfd 语义位整理为 VFS open 可接受的 file flags。 */
	flags &= ~PIDFD_STALE;
	flags |= O_RDWR;
	pidfd_file = dentry_open(&path, flags, current_cred());
	/*
	 * Raise PIDFD_THREAD and PIDFD_AUTOKILL explicitly as
	 * do_dentry_open() strips O_EXCL and O_TRUNC.
	 */
	/*
	 * do_dentry_open() 会清除 O_EXCL/O_TRUNC；而 PIDFD_THREAD
	 * 与 PIDFD_AUTOKILL 复用这些位，所以成功后必须显式恢复到 f_flags。
	 */
	if (!IS_ERR(pidfd_file))
		pidfd_file->f_flags |= (flags & (PIDFD_THREAD | PIDFD_AUTOKILL));

	return pidfd_file;
}

/*
 * pidfs_init() - 启动期初始化全局索引、属性 slab 和永久内核挂载。
 *
 * 入参：无；__init 表示代码可在启动后释放。三个阶段均是 pidfd 基础
 * 设施，失败无法安全降级，因此 panic。成功后发布 pidfs_mnt/root_path，
 * 之后 pid 创建和 pidfd 分配均可使用；无正常返回值和回滚路径。
 */
void __init pidfs_init(void)
{
	/* 阶段 1：建立 ino->pid 的 RCU rhashtable。 */
	if (rhashtable_init(&pidfs_ino_ht, &pidfs_ino_ht_params))
		panic("Failed to initialize pidfs hashtable");

	/* 阶段 2：创建带内存记账、可回收且失败即 panic 的专用 slab。 */
	pidfs_attr_cachep = kmem_cache_create("pidfs_attr_cache", sizeof(struct pidfs_attr), 0,
					 (SLAB_HWCACHE_ALIGN | SLAB_RECLAIM_ACCOUNT |
					  SLAB_ACCOUNT | SLAB_PANIC), NULL);

	/* 阶段 3：内部挂载 pidfs；挂载成功是 VFS 路径可创建的前置条件。 */
	pidfs_mnt = kern_mount(&pidfs_type);
	if (IS_ERR(pidfs_mnt))
		panic("Failed to mount pidfs pseudo filesystem");

	/* 保存永久借用根路径；单个调用者通过 pidfs_get_root() 另取引用。 */
	pidfs_root_path.mnt = pidfs_mnt;
	pidfs_root_path.dentry = pidfs_mnt->mnt_root;
}
