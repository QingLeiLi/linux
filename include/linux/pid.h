/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PID_H
#define _LINUX_PID_H

#include <linux/pid_types.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/rhashtable-types.h>
#include <linux/sched.h>
#include <linux/wait.h>

/*
 * What is struct pid?
 *
 * A struct pid is the kernel's internal notion of a process identifier.
 * It refers to individual tasks, process groups, and sessions.  While
 * there are processes attached to it the struct pid lives in a hash
 * table, so it and then the processes that it refers to can be found
 * quickly from the numeric pid value.  The attached processes may be
 * quickly accessed by following pointers from struct pid.
 *
 * Storing pid_t values in the kernel and referring to them later has a
 * problem.  The process originally with that pid may have exited and the
 * pid allocator wrapped, and another process could have come along
 * and been assigned that pid.
 *
 * Referring to user space processes by holding a reference to struct
 * task_struct has a problem.  When the user space process exits
 * the now useless task_struct is still kept.  A task_struct plus a
 * stack consumes around 10K of low kernel memory.  More precisely
 * this is THREAD_SIZE + sizeof(struct task_struct).  By comparison
 * a struct pid is about 64 bytes.
 *
 * Holding a reference to struct pid solves both of these problems.
 * It is small so holding a reference does not consume a lot of
 * resources, and since a new struct pid is allocated when the numeric pid
 * value is reused (when pids wrap around) we don't mistakenly refer to new
 * processes.
 */
/*
 * ============================================================================
 * 【struct pid 设计说明】
 *
 * 【为什么需要 struct pid？】
 *
 * 问题1：直接使用 pid_t 数字有风险
 * - 进程退出后，PID 可能被回收并分配给新进程
 * - 内核保存的 pid_t 可能指向错误的进程（ABA 问题）
 * - 例如：保存 pid=1234，该进程退出，新进程也分配到 1234，导致误操作
 *
 * 问题2：引用 task_struct 太重
 * - task_struct 包含完整的进程状态（约 1.5KB）
 * - 加上内核栈（x86_64 通常 16KB），总共约 17KB
 * - 如果仅仅为了保持进程标识就占用这么多内存，太浪费
 * - 进程退出后，如果还有人引用 task_struct，内存无法释放
 *
 * 解决方案：struct pid
 * - 轻量级（约 64 字节），引用成本低
 * - 通过引用计数管理生命周期（refcount_t）
 * - PID 回收时分配新的 struct pid，避免 ABA 问题
 * - 支持 PID 命名空间（容器隔离）
 * - 通过哈希表快速查找（O(1) 时间复杂度）
 *
 * 【struct pid 的功能】
 * 1. 进程标识：唯一标识一个进程（PIDTYPE_PID）
 * 2. 进程组标识：标识一个进程组（PIDTYPE_PGID）
 * 3. 会话标识：标识一个会话（PIDTYPE_SID）
 * 4. 线程组标识：标识一个线程组（PIDTYPE_TGID）
 * 5. 命名空间支持：同一进程在不同命名空间有不同的 PID
 *
 * 【使用场景】
 * - 信号发送：kill(pid, signal) 需要查找目标进程
 * - waitpid 等待：父进程等待子进程退出
 * - /proc 文件系统：通过 PID 访问进程信息
 * - 容器管理：PID 命名空间隔离
 * - 进程监控：审计、跟踪系统调用
 * ============================================================================
 */


/*
 * struct upid is used to get the id of the struct pid, as it is
 * seen in particular namespace. Later the struct pid is found with
 * find_pid_ns() using the int nr and struct pid_namespace *ns.
 */
/*
 * 【结构体】upid - 单个命名空间中的 PID 表示（"unique pid"）
 *
 * 【设计背景】
 * Linux 支持 PID 命名空间（PID namespace），同一个进程在不同命名空间中
 * 有不同的 PID 值。例如：
 * - 在主机（init namespace）中看到的 PID 是 1234
 * - 在容器（container namespace）中看到的 PID 是 1
 *
 * struct upid 表示进程在某个特定命名空间中的 PID 值。
 *
 * 【字段说明】
 */

#define RESERVED_PIDS 300
/* 预留的 PID 数量
 * PID 0-299 保留给特殊用途：
 * - PID 0: swapper（调度器空闲进程）
 * - PID 1: init（用户空间第一个进程）
 * - PID 2: kthreadd（内核线程守护进程）
 * - PID 3-299: 早期启动的内核线程和关键进程
 *
 * 普通进程的 PID 从 300 开始分配。
 */

struct pidfs_attr;

struct upid {
	int nr;
	/* 进程在该命名空间中的数字 PID
	 * 范围：1 到 PID_MAX_LIMIT（通常是 2^22 = 4194304）
	 *
	 * 【为什么从 1 开始？】
	 * PID 0 是特殊的（swapper 进程），不分配给普通进程。
	 */

	struct pid_namespace *ns;
	/* 指向所属的 PID 命名空间
	 * - 不同的命名空间是层次结构（树形）
	 * - 父命名空间可以看到子命名空间的进程（但 PID 不同）
	 * - 子命名空间看不到父命名空间的进程
	 *
	 * 【命名空间层次示例】
	 *   init_pid_ns (PID 1234)
	 *        |
	 *   container_ns (PID 5)
	 *        |
	 *   nested_container_ns (PID 1)
	 *
	 * 同一个进程在三个命名空间中有三个不同的 PID。
	 */
};

/*
 * 【结构体】pid - 内核的进程标识符对象
 *
 * 这是 Linux PID 管理的核心数据结构，每个分配的 PID 都有一个对应的 struct pid。
 */
struct pid {
	refcount_t count;
	/* 引用计数
	 * - 每当有人持有对该 struct pid 的引用时，count++
	 * - 释放引用时，count--
	 * - 当 count 降为 0 时，struct pid 被释放（free_pid）
	 *
	 * 【谁会引用 struct pid？】
	 * - task_struct->thread_pid（进程自身）
	 * - task_struct->signal->pids[PIDTYPE_*]（进程组、会话）
	 * - 打开的 pidfd 文件描述符
	 * - /proc/<pid> 目录的 inode
	 * - 内核子系统（如审计、跟踪）
	 *
	 * 【引用计数的意义】
	 * 进程退出后，task_struct 可能被释放，但 struct pid 仍然存在，
	 * 直到所有引用者都释放引用。这保证了 PID 查找的安全性。
	 */

	unsigned int level;
	/* 命名空间层级深度
	 * - level = 0: 根命名空间（init_pid_ns）
	 * - level = 1: 第一层容器
	 * - level = 2: 嵌套容器
	 * - ...
	 *
	 * 【为什么需要 level？】
	 * 用于快速判断命名空间关系和遍历 numbers[] 数组。
	 * numbers[0] 是根命名空间的 PID，numbers[level] 是当前命名空间的 PID。
	 */

	spinlock_t lock;
	/* 自旋锁，保护 tasks[] 链表的并发访问
	 * - 添加/删除进程时需要持有该锁
	 * - 遍历 tasks[] 链表时需要持有该锁或使用 RCU
	 *
	 * 【为什么用自旋锁而不是互斥锁？】
	 * PID 操作在中断上下文中可能发生（如信号处理），
	 * 不能睡眠，只能用自旋锁。
	 */

	struct {
		u64 ino;
		/* pidfs（PID 文件系统）的 inode 号
		 * 用于 /proc/<pid> 目录
		 */

		struct rhash_head pidfs_hash;
		/* pidfs 的哈希表节点
		 * 用于快速查找 PID 对应的 inode
		 */

		struct dentry *stashed;
		/* pidfs 的 dentry 缓存
		 * 避免重复创建 /proc/<pid> 目录项
		 */

		struct pidfs_attr *attr;
		/* pidfs 的属性（如权限、所有者）
		 */
	};

	/* lists of tasks that use this pid */
	struct hlist_head tasks[PIDTYPE_MAX];
	/* 使用该 PID 的任务链表数组
	 *
	 * 【PIDTYPE_MAX 的含义】
	 * 一个 struct pid 可以表示多种类型的标识符：
	 * - tasks[PIDTYPE_PID]:  进程 ID（单个进程）
	 * - tasks[PIDTYPE_TGID]: 线程组 ID（多个线程共享）
	 * - tasks[PIDTYPE_PGID]: 进程组 ID（多个进程共享）
	 * - tasks[PIDTYPE_SID]:  会话 ID（多个进程组共享）
	 *
	 * 【为什么同一个 struct pid 可以有多个用途？】
	 * 为了节省内存。例如，单线程进程的 PID == TGID，
	 * 没必要分配两个 struct pid，共用一个即可。
	 *
	 * 【hlist_head 链表结构】
	 * 每个链表头指向使用该 PID 的所有任务（task_struct）。
	 * 任务通过 task_struct->pid_links[type] 链入对应的链表。
	 *
	 * 【示例】进程组
	 * 进程组中所有进程共享同一个 PIDTYPE_PGID 的 struct pid，
	 * tasks[PIDTYPE_PGID] 链表包含该组的所有进程。
	 */

	struct hlist_head inodes;
	/* 关联的 inode 链表
	 * /proc/<pid>/* 下的所有文件 inode 都链在这里
	 * 用于进程退出时批量删除 /proc 条目
	 */

	/* wait queue for pidfd notifications */
	wait_queue_head_t wait_pidfd;
	/* pidfd 等待队列
	 *
	 * 【pidfd 是什么？】
	 * pidfd 是 Linux 5.1 引入的特性，允许用户空间持有进程的文件描述符。
	 * 相比 PID 数字，pidfd 更安全（不会被回收复用）。
	 *
	 * 【等待队列的用途】
	 * - pidfd_poll(): 等待进程退出
	 * - pidfd_send_signal(): 发送信号
	 * - waitid(P_PIDFD): 等待子进程
	 *
	 * 【工作原理】
	 * 进程退出时（do_notify_pidfd），唤醒 wait_pidfd 上的等待者，
	 * 通知所有持有 pidfd 的进程。
	 */

	struct rcu_head rcu;
	/* RCU（Read-Copy-Update）回调
	 *
	 * 【为什么需要 RCU？】
	 * struct pid 在哈希表中，多个 CPU 可能并发查找（find_pid_ns）。
	 * 为了避免锁竞争，使用 RCU 实现无锁读取。
	 *
	 * 【RCU 的工作流程】
	 * 1. 读者（find_pid_ns）无需加锁，直接读取哈希表
	 * 2. 写者（free_pid）不直接删除，而是通过 call_rcu 延迟释放
	 * 3. RCU 等待所有读者完成当前的读操作（grace period）
	 * 4. 确认没有读者后，才真正释放内存
	 *
	 * 【优势】
	 * - 读操作完全无锁，性能极高（O(1)，无 cache bouncing）
	 * - 适合读多写少的场景（PID 查找频繁，PID 分配/释放相对少）
	 */

	struct upid numbers[];
	/* 灵活数组成员（Flexible Array Member）
	 *
	 * 【数组长度】
	 * numbers[] 的实际长度是 (level + 1)，在分配时动态确定。
	 *
	 * 【数组内容】
	 * numbers[0]: 根命名空间（init_pid_ns）中的 PID
	 * numbers[1]: 第一层容器命名空间中的 PID
	 * ...
	 * numbers[level]: 当前命名空间中的 PID
	 *
	 * 【示例】
	 * 假设进程在嵌套容器中创建（level = 2）：
	 * - numbers[0] = {.nr = 1234, .ns = &init_pid_ns}       // 主机 PID
	 * - numbers[1] = {.nr = 5,    .ns = &container_ns}       // 容器 PID
	 * - numbers[2] = {.nr = 1,    .ns = &nested_container_ns} // 嵌套容器 PID
	 *
	 * 从主机看，进程 PID 是 1234；从容器看，是 5；从嵌套容器看，是 1。
	 *
	 * 【为什么用灵活数组？】
	 * - 节省内存：只分配需要的空间（大多数进程 level = 0，只需一个 upid）
	 * - 提高局部性：upid 数据紧邻 struct pid，cache 友好
	 */
};

extern struct pid init_struct_pid;
/* 全局变量：init 进程（PID 1）的 struct pid
 * 这是系统启动时静态分配的第一个 PID 对象
 */

struct file;

/*
 * ============================================================================
 * 【pidfd 相关函数】- 进程文件描述符支持
 *
 * pidfd 是 Linux 5.1+ 引入的特性，允许用户空间通过文件描述符引用进程。
 * 相比传统的 pid_t 数字，pidfd 更安全（不受 PID 回收影响）。
 * ============================================================================
 */

struct pid *pidfd_pid(const struct file *file);
/* 从 pidfd 文件对象获取 struct pid
 * @file: pidfd 的文件对象（struct file *）
 *
 * 返回值：对应的 struct pid *，如果不是 pidfd 则返回 NULL
 *
 * 【使用场景】
 * 内核需要验证用户传入的文件描述符是否是有效的 pidfd。
 *
 * 【示例】
 * struct file *f = fget(fd);
 * struct pid *pid = pidfd_pid(f);
 * if (!pid) {
 *     // 不是 pidfd，返回错误
 * }
 */

struct pid *pidfd_get_pid(unsigned int fd, unsigned int *flags);
/* 从文件描述符获取 struct pid，并返回标志位
 * @fd:    用户空间的文件描述符
 * @flags: 输出参数，返回 pidfd 的标志位（如 O_NONBLOCK）
 *
 * 返回值：成功返回 struct pid *（引用计数已增加），失败返回 ERR_PTR(-errno)
 *
 * 【与 pidfd_pid 的区别】
 * - pidfd_pid: 输入是 struct file *，不增加引用计数
 * - pidfd_get_pid: 输入是 fd，会增加引用计数（调用者需要 put_pid）
 *
 * 【使用场景】
 * 系统调用（如 pidfd_send_signal）需要从用户传入的 fd 获取 pid。
 */

struct task_struct *pidfd_get_task(int pidfd, unsigned int *flags);
/* 从 pidfd 获取对应的进程（task_struct）
 * @pidfd: pidfd 文件描述符
 * @flags: 输出参数，返回标志位
 *
 * 返回值：成功返回 struct task_struct *（引用计数已增加），失败返回 ERR_PTR(-errno)
 *
 * 【注意事项】
 * - 返回的 task_struct 引用计数已增加，调用者需要 put_task_struct
 * - 如果进程已退出，返回 -ESRCH（No such process）
 *
 * 【使用场景】
 * pidfd_send_signal() 需要获取目标进程来发送信号。
 */

int pidfd_prepare(struct pid *pid, unsigned int flags, struct file **ret_file);
/* 为 struct pid 准备一个 pidfd 文件对象
 * @pid:      要创建 pidfd 的 struct pid
 * @flags:    文件标志位（O_CLOEXEC, O_NONBLOCK 等）
 * @ret_file: 输出参数，返回创建的文件对象
 *
 * 返回值：成功返回 0，失败返回负的错误码
 *
 * 【工作流程】
 * 1. 分配一个匿名文件对象（anon_inode）
 * 2. 设置文件操作（pidfd_fops）
 * 3. 将 struct pid 关联到文件的 private_data
 * 4. 增加 struct pid 的引用计数
 *
 * 【使用场景】
 * - clone3(CLONE_PIDFD): 创建进程时返回 pidfd
 * - pidfd_open(pid, flags): 为已有进程创建 pidfd
 */

void do_notify_pidfd(struct task_struct *task);
/* 通知所有持有该进程 pidfd 的等待者
 * @task: 退出的进程
 *
 * 无返回值
 *
 * 【调用时机】
 * 进程退出时（do_exit）调用，唤醒所有在 pidfd 上等待的进程。
 *
 * 【工作流程】
 * 1. 获取进程的 struct pid
 * 2. 唤醒 pid->wait_pidfd 等待队列
 * 3. 等待者可以通过 poll/epoll 得知进程已退出
 *
 * 【使用场景】
 * 用户空间可以用 poll(pidfd, POLLIN) 等待进程退出，
 * 比轮询 /proc/<pid> 更高效。
 */

/*
 * ============================================================================
 * 【引用计数管理函数】
 * ============================================================================
 */

static inline struct pid *get_pid(struct pid *pid)
{
	if (pid)
		refcount_inc(&pid->count);
	return pid;
}
/* 增加 struct pid 的引用计数
 * @pid: 要增加引用的 struct pid（可以为 NULL）
 *
 * 返回值：返回 pid 本身（方便链式调用）
 *
 * 【使用场景】
 * 任何需要长期持有 struct pid 引用的地方：
 * - 保存到数据结构中
 * - 跨函数调用传递
 * - 异步操作（RCU 回调、工作队列）
 *
 * 【配对规则】
 * 每次 get_pid 必须配对一次 put_pid，否则内存泄漏。
 *
 * 【为什么允许 NULL？】
 * 简化调用代码，避免到处写 if (pid) get_pid(pid)。
 */

extern void put_pid(struct pid *pid);
/* 减少 struct pid 的引用计数，可能释放内存
 * @pid: 要释放引用的 struct pid（可以为 NULL）
 *
 * 无返回值
 *
 * 【工作流程】
 * 1. 如果 pid == NULL，直接返回（no-op）
 * 2. 原子递减 refcount
 * 3. 如果 refcount 降为 0：
 *    a. 从哈希表中移除
 *    b. 调用 call_rcu 延迟释放（等待 RCU grace period）
 *    c. RCU 回调中真正 kfree(pid)
 *
 * 【RCU 延迟释放的原因】
 * 可能有其他 CPU 正在无锁读取哈希表（find_pid_ns），
 * 必须等待它们完成当前的读操作，才能安全释放内存。
 */

/*
 * ============================================================================
 * 【PID 与任务的转换函数】
 * ============================================================================
 */

extern struct task_struct *pid_task(struct pid *pid, enum pid_type);
/* 从 struct pid 获取对应的任务（task_struct）
 * @pid:  要查询的 struct pid
 * @type: PID 类型（PIDTYPE_PID, PIDTYPE_TGID, PIDTYPE_PGID, PIDTYPE_SID）
 *
 * 返回值：找到返回 struct task_struct *，未找到返回 NULL
 *
 * 【工作原理】
 * 返回 pid->tasks[type] 链表的第一个任务。
 *
 * 【为什么需要 type 参数？】
 * 同一个 struct pid 可能被多种类型使用：
 * - PIDTYPE_PID:  返回该进程本身
 * - PIDTYPE_TGID: 返回线程组的组长（主线程）
 * - PIDTYPE_PGID: 返回进程组的组长
 * - PIDTYPE_SID:  返回会话的会话领导者
 *
 * 【注意事项】
 * - 返回的 task_struct 没有增加引用计数
 * - 调用者必须持有 rcu_read_lock() 或 tasklist_lock
 * - 如果需要长期持有，应该调用 get_task_struct()
 *
 * 【示例】
 * rcu_read_lock();
 * struct task_struct *task = pid_task(pid, PIDTYPE_PID);
 * if (task) {
 *     // 使用 task，但不能保存指针
 * }
 * rcu_read_unlock();
 */

static inline bool pid_has_task(struct pid *pid, enum pid_type type)
{
	return !hlist_empty(&pid->tasks[type]);
}
/* 检查 struct pid 是否有关联的任务
 * @pid:  要检查的 struct pid
 * @type: PID 类型
 *
 * 返回值：true=有任务，false=无任务
 *
 * 【使用场景】
 * 判断进程是否已退出：
 * - 进程退出时，会从 pid->tasks[] 链表中移除
 * - 如果链表为空，说明没有任务使用该 PID（进程已退出）
 *
 * 【与 pid_task 的区别】
 * - pid_has_task: 仅检查是否存在，不返回任务
 * - pid_task: 返回具体的任务
 *
 * 【性能优势】
 * 只检查链表头是否为空（O(1)），不需要遍历链表。
 */

extern struct task_struct *get_pid_task(struct pid *pid, enum pid_type);
/* 从 struct pid 获取任务，并增加引用计数
 * @pid:  要查询的 struct pid
 * @type: PID 类型
 *
 * 返回值：找到返回 struct task_struct *（引用计数已增加），未找到返回 NULL
 *
 * 【与 pid_task 的区别】
 * - pid_task:     不增加引用计数，需要持有锁
 * - get_pid_task: 增加引用计数，可以安全保存指针
 *
 * 【使用场景】
 * 需要长期持有 task_struct 引用的地方：
 * - 异步操作（工作队列、定时器回调）
 * - 保存到数据结构中
 * - 跨函数调用传递
 *
 * 【配对规则】
 * 调用者必须在使用完后调用 put_task_struct() 释放引用。
 */

extern struct pid *get_task_pid(struct task_struct *task, enum pid_type type);
/* 从任务获取 struct pid，并增加引用计数
 * @task: 要查询的任务
 * @type: PID 类型
 *
 * 返回值：成功返回 struct pid *（引用计数已增加），失败返回 NULL
 *
 * 【工作原理】
 * 根据 type 从 task 的不同字段获取 pid：
 * - PIDTYPE_PID:  task->thread_pid
 * - PIDTYPE_TGID: task->signal->pids[PIDTYPE_TGID]
 * - PIDTYPE_PGID: task->signal->pids[PIDTYPE_PGID]
 * - PIDTYPE_SID:  task->signal->pids[PIDTYPE_SID]
 *
 * 【使用场景】
 * 需要保存进程的 PID 引用（比保存 task_struct 更轻量）。
 *
 * 【配对规则】
 * 调用者必须在使用完后调用 put_pid() 释放引用。
 */

/*
 * these helpers must be called with the tasklist_lock write-held.
 */
extern void attach_pid(struct task_struct *task, enum pid_type);
void detach_pid(struct pid **pids, struct task_struct *task, enum pid_type);
void change_pid(struct pid **pids, struct task_struct *task, enum pid_type,
		struct pid *pid);
extern void exchange_tids(struct task_struct *task, struct task_struct *old);
extern void transfer_pid(struct task_struct *old, struct task_struct *new,
			 enum pid_type);

/*
 * look up a PID in the hash table. Must be called with the tasklist_lock
 * or rcu_read_lock() held.
 *
 * find_pid_ns() finds the pid in the namespace specified
 * find_vpid() finds the pid by its virtual id, i.e. in the current namespace
 *
 * see also find_task_by_vpid() set in include/linux/sched.h
 */
extern struct pid *find_pid_ns(int nr, struct pid_namespace *ns);
extern struct pid *find_vpid(int nr);

/*
 * Lookup a PID in the hash table, and return with it's count elevated.
 */
extern struct pid *find_get_pid(int nr);
extern struct pid *find_ge_pid(int nr, struct pid_namespace *);

extern struct pid *alloc_pid(struct pid_namespace *ns, pid_t *set_tid,
			     size_t set_tid_size);
extern void free_pid(struct pid *pid);
void free_pids(struct pid **pids);
extern void disable_pid_allocation(struct pid_namespace *ns);

/*
 * ns_of_pid() returns the pid namespace in which the specified pid was
 * allocated.
 *
 * NOTE:
 * 	ns_of_pid() is expected to be called for a process (task) that has
 * 	an attached 'struct pid' (see attach_pid(), detach_pid()) i.e @pid
 * 	is expected to be non-NULL. If @pid is NULL, caller should handle
 * 	the resulting NULL pid-ns.
 */
static inline struct pid_namespace *ns_of_pid(struct pid *pid)
{
	struct pid_namespace *ns = NULL;
	if (pid)
		ns = pid->numbers[pid->level].ns;
	return ns;
}

/*
 * is_child_reaper returns true if the pid is the init process
 * of the current namespace. As this one could be checked before
 * pid_ns->child_reaper is assigned in copy_process, we check
 * with the pid number.
 */
static inline bool is_child_reaper(struct pid *pid)
{
	return pid->numbers[pid->level].nr == 1;
}

/*
 * the helpers to get the pid's id seen from different namespaces
 *
 * pid_nr()    : global id, i.e. the id seen from the init namespace;
 * pid_vnr()   : virtual id, i.e. the id seen from the pid namespace of
 *               current.
 * pid_nr_ns() : id seen from the ns specified.
 *
 * see also task_xid_nr() etc in include/linux/sched.h
 */

static inline pid_t pid_nr(struct pid *pid)
{
	pid_t nr = 0;
	if (pid)
		nr = pid->numbers[0].nr;
	return nr;
}

pid_t pid_nr_ns(struct pid *pid, struct pid_namespace *ns);
pid_t pid_vnr(struct pid *pid);

#define do_each_pid_task(pid, type, task)				\
	do {								\
		if ((pid) != NULL)					\
			hlist_for_each_entry_rcu((task),		\
				&(pid)->tasks[type], pid_links[type]) {

			/*
			 * Both old and new leaders may be attached to
			 * the same pid in the middle of de_thread().
			 */
#define while_each_pid_task(pid, type, task)				\
				if (type == PIDTYPE_PID)		\
					break;				\
			}						\
	} while (0)

#define do_each_pid_thread(pid, type, task)				\
	do_each_pid_task(pid, type, task) {				\
		struct task_struct *tg___ = task;			\
		for_each_thread(tg___, task) {

#define while_each_pid_thread(pid, type, task)				\
		}							\
		task = tg___;						\
	} while_each_pid_task(pid, type, task)

static inline struct pid *task_pid(struct task_struct *task)
{
	return task->thread_pid;
}

/*
 * the helpers to get the task's different pids as they are seen
 * from various namespaces
 *
 * task_xid_nr()     : global id, i.e. the id seen from the init namespace;
 * task_xid_vnr()    : virtual id, i.e. the id seen from the pid namespace of
 *                     current.
 * task_xid_nr_ns()  : id seen from the ns specified;
 *
 * see also pid_nr() etc in include/linux/pid.h
 */
pid_t __task_pid_nr_ns(struct task_struct *task, enum pid_type type, struct pid_namespace *ns);

static inline pid_t task_pid_nr(struct task_struct *tsk)
{
	return tsk->pid;
}

static inline pid_t task_pid_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_PID, ns);
}

static inline pid_t task_pid_vnr(struct task_struct *tsk)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_PID, NULL);
}


static inline pid_t task_tgid_nr(struct task_struct *tsk)
{
	return tsk->tgid;
}

/**
 * pid_alive - check that a task structure is not stale
 * @p: Task structure to be checked.
 *
 * Test if a process is not yet dead (at most zombie state)
 * If pid_alive fails, then pointers within the task structure
 * can be stale and must not be dereferenced.
 *
 * Return: 1 if the process is alive. 0 otherwise.
 */
static inline int pid_alive(const struct task_struct *p)
{
	return p->thread_pid != NULL;
}

static inline pid_t task_pgrp_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_PGID, ns);
}

static inline pid_t task_pgrp_vnr(struct task_struct *tsk)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_PGID, NULL);
}


static inline pid_t task_session_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_SID, ns);
}

static inline pid_t task_session_vnr(struct task_struct *tsk)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_SID, NULL);
}

static inline pid_t task_tgid_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_TGID, ns);
}

static inline pid_t task_tgid_vnr(struct task_struct *tsk)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_TGID, NULL);
}

static inline pid_t task_ppid_nr_ns(const struct task_struct *tsk, struct pid_namespace *ns)
{
	pid_t pid = 0;

	rcu_read_lock();
	if (pid_alive(tsk))
		pid = task_tgid_nr_ns(rcu_dereference(tsk->real_parent), ns);
	rcu_read_unlock();

	return pid;
}

static inline pid_t task_ppid_vnr(const struct task_struct *tsk)
{
	return task_ppid_nr_ns(tsk, NULL);
}

static inline pid_t task_ppid_nr(const struct task_struct *tsk)
{
	return task_ppid_nr_ns(tsk, &init_pid_ns);
}

/* Obsolete, do not use: */
static inline pid_t task_pgrp_nr(struct task_struct *tsk)
{
	return task_pgrp_nr_ns(tsk, &init_pid_ns);
}

/**
 * is_global_init - check if a task structure is init. Since init
 * is free to have sub-threads we need to check tgid.
 * @tsk: Task structure to be checked.
 *
 * Check if a task structure is the first user space task the kernel created.
 *
 * Return: 1 if the task structure is init. 0 otherwise.
 */
static inline int is_global_init(struct task_struct *tsk)
{
	return task_tgid_nr(tsk) == 1;
}

#endif /* _LINUX_PID_H */
