// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * taskstats 子系统学习导读
 *
 * 中文学习注释模型：OpenAI Codex GPT-5。
 *
 * 本文件把内核中的任务记账结果封装成 Generic Netlink 消息，提供两种
 * 使用方式：用户可以按 PID/TGID 主动查询仍然存在的任务，也可以按 CPU
 * 注册端口，在任务退出时被动接收最终快照。cgroupstats 复用同一个
 * Generic Netlink family，但其统计构造由 cgroup 子系统完成。
 *
 * 主调用链：
 *   用户查询 -> taskstats_user_cmd() -> cmd_attr_pid()/cmd_attr_tgid()
 *            -> fill_stats_for_*() -> send_reply()
 *   退出推送 -> do_exit() -> taskstats_exit() -> send_cpu_listeners()
 *   初始化   -> start_kernel() -> taskstats_init_early()
 *            -> late_initcall(taskstats_init)
 *
 * 核心生命周期有两条。每 PID 快照直接从持有引用的 task_struct 读取；
 * 每 TGID 快照则把已退出线程的数据逐步累计到 signal_struct::stats，查询
 * 时再合入仍存活线程，从而避免为每个线程永久保留完整历史。该缓存由
 * taskstats_cache 分配，随共享 signal_struct 最终销毁。
 *
 * 并发模型也分层：task 引用保证单任务对象在采集期间不释放；sighand
 * 的 siglock 串行化线程退出累计与 TGID 查询；每 CPU listener 链表由
 * 自己的 rwsem 保护。signal->stats 的首次发布另用 acquire/release，令
 * 无锁快速读取者不会观察到尚未清零完成的对象。
 *
 * 这种设计把退出事件按 CPU 分流，并只在多线程进程需要时分配 TGID
 * 累计对象，降低常驻开销；代价是 Netlink 在突发退出时可能丢包，TGID
 * 查询也只能形成受相应锁约束的近似时点快照，而不是全系统原子快照。
 * CONFIG_TASKSTATS 关闭时本文件不进入构建，taskstats_kern.h 提供空退出
 * 钩子；具体 delayacct/扩展记账配置关闭时，相应 helper 保持字段为零，
 * 但 Netlink 消息布局与版本规则不变。
 */
/*
 * taskstats.c - Export per-task statistics to userland
 *
 * Copyright (C) Shailabh Nagar, IBM Corp. 2006
 *           (C) Balbir Singh,   IBM Corp. 2006
 */

#include <linux/kernel.h>
#include <linux/taskstats_kern.h>
#include <linux/tsacct_kern.h>
#include <linux/acct.h>
#include <linux/delayacct.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/cgroupstats.h>
#include <linux/cgroup.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/pid_namespace.h>
#include <net/genetlink.h>
#include <linux/atomic.h>
#include <linux/sched/cputime.h>

/*
 * Maximum length of a cpumask that can be specified in
 * the TASKSTATS_CMD_ATTR_REGISTER/DEREGISTER_CPUMASK attribute
 */
/*
 * 用户用字符串形式的 CPU 列表选择退出事件来源。上限按最坏情况下每个
 * CPU 编号及分隔符预留空间，同时额外留出固定余量；超过此界限的属性在
 * 分配和解析之前即被拒绝，避免用户控制的超大临时分配。
 */
#define TASKSTATS_CPUMASK_MAXLEN	(100+6*NR_CPUS)

/*
 * taskstats_seqnum 为内核主动推送的消息提供每 CPU 序号；主动推送没有
 * genl_info 可继承请求序号，因此必须自行生成。它只要求各 CPU 的事件流
 * 内递增，不建立跨 CPU 的全局顺序。
 *
 * family_registered 是退出热路径的发布门槛：Generic Netlink family
 * 注册成功后才置位，此前的退出事件直接跳过。taskstats_cache 保存 TGID
 * 累计对象，早期初始化以 SLAB_PANIC 创建，因此后续可假定 cache 存在。
 * family 先前向声明，是因为消息构造 helper 位于最终描述符之前。
 */
static DEFINE_PER_CPU(__u32, taskstats_seqnum);
static int family_registered;
struct kmem_cache *taskstats_cache;

static struct genl_family family;

/*
 * 两张 policy 表定义用户属性的最外层类型边界。PID/TGID 是 32 位标识，
 * CPU mask 是 NUL 结尾字符串；更深的语义约束（命名空间、CPU 是否可能、
 * fd 是否指向 cgroup）留给各命令处理函数验证。
 */
static const struct nla_policy taskstats_cmd_get_policy[] = {
	[TASKSTATS_CMD_ATTR_PID]  = { .type = NLA_U32 },
	[TASKSTATS_CMD_ATTR_TGID] = { .type = NLA_U32 },
	[TASKSTATS_CMD_ATTR_REGISTER_CPUMASK] = { .type = NLA_STRING },
	[TASKSTATS_CMD_ATTR_DEREGISTER_CPUMASK] = { .type = NLA_STRING },};

static const struct nla_policy cgroupstats_cmd_get_policy[] = {
	[CGROUPSTATS_CMD_ATTR_FD] = { .type = NLA_U32 },
};

/*
 * listener 表示一个希望接收退出通知的 Generic Netlink 端口。
 * @list: 嵌入每 CPU listener_list 的侵入式链表节点。
 * @pid:  历史命名沿用 nl_pid，实际保存 Netlink port ID，而非进程 PID。
 * @valid: 单播返回 -ECONNREFUSED 时先在读锁下清零，稍后升级为写锁删除；
 *         这样发送遍历无需在中途改变链表结构。
 */
struct listener {
	struct list_head list;
	pid_t pid;
	char valid;
};

/*
 * 每个可能 CPU 拥有一个 listener_list。注册同一 mask 会把端口分别挂入
 * 各 CPU 的链表；任务退出时只查看当前 CPU 的实例，因此无需全局锁。
 * @sem 允许发送者并发遍历，但注册、注销和失效清理必须独占修改 @list。
 */
struct listener_list {
	struct rw_semaphore sem;
	struct list_head list;
};
static DEFINE_PER_CPU(struct listener_list, listener_array);

/*
 * actions 是 add_del_listener() 的操作协议。CPU_DONT_CARE 当前没有进入
 * 本文件控制流，保留为“不指定增删”的枚举值；真正调用只传 REGISTER
 * 或 DEREGISTER，不能把普通布尔值的含义与这些 ABI 内部动作混淆。
 */
enum actions {
	REGISTER,
	DEREGISTER,
	CPU_DONT_CARE
};

/*
 * prepare_reply() - 分配并建立一条尚未封口的 Generic Netlink 回复。
 *
 * @info: 主动查询时借用请求上下文，用于继承端口和序号；退出推送传
 *        NULL，函数改用当前 CPU 的内部序号。调用期间不接管该指针。
 * @cmd:  写入 Generic Netlink 头的 taskstats/cgroupstats 命令号。
 * @skbp: 成功时输出新 skb，所有权交给调用者；失败时不写出有效对象。
 * @size: 预估的属性载荷及对齐空间，不包含调用者尚未追加的其他分配。
 *
 * 调用位置：PID/TGID/cgroup 查询处理器以及 taskstats_exit() 都先在这里
 * 建立消息，返回后分别继续填属性并交给同步回复或退出广播路径。
 * 入口不持有本文件锁；GFP_KERNEL 分配可能睡眠。
 *
 * 本函数运行在可睡眠上下文，以 GFP_KERNEL 分配 skb。成功返回 0 时只
 * 建立消息头，调用者仍须追加属性并用 send_reply()/send_cpu_listeners()
 * 封口和发送；-ENOMEM 表示 skb 分配失败，-EINVAL 表示消息头无法放入，
 * 后一种情况下函数已释放 skb，不留下清理责任。
 */
static int prepare_reply(struct genl_info *info, u8 cmd, struct sk_buff **skbp,
				size_t size)
{
	struct sk_buff *skb;
	void *reply;

	/*
	 * If new attributes are added, please revisit this allocation
	 */
	/*
	 * 当前 size 由各调用者按既有属性精确估算。
	 * 将来若增加属性而未同步
	 * 扩大它，genlmsg_put/nla_put 会因尾部空间不足失败；这条英文注释
	 * 因而是在提醒维护者同时更新消息容量契约。
	 */
	skb = genlmsg_new(size, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	if (!info) {
		int seq = this_cpu_inc_return(taskstats_seqnum) - 1;

		/*
		 * 退出通知不是对某个请求的回复，
		 * 没有请求方的序号和 portid。
		 * 这里用内核端口 0 与每 CPU 序号构造异步消息；减一使该 CPU
		 * 第一条消息从序号 0 开始。
		 */
		reply = genlmsg_put(skb, 0, seq, &family, 0, cmd);
	} else
		/*
		 * 查询回复继承请求元数据，
		 * 使用户空间能把响应与请求配对。
		 */
		reply = genlmsg_put_reply(skb, info, &family, 0, cmd);
	if (reply == NULL) {
		nlmsg_free(skb);
		return -EINVAL;
	}

	*skbp = skb;
	return 0;
}

/*
 * Send taskstats data in @skb to listener with nl_pid @pid
 */
/*
 * 将 @skb 中已经填好的属性作为同步查询回复发回 @info 指定的端口。
 * @info 是查询 doit 借用的请求上下文，函数不保存也不释放它。
 * 调用者是 PID/TGID/cgroup 查询处理器；成功或失败返回后都直接结束
 * 当前命令，不再访问 skb。入口不持有本文件锁，运行在 Netlink doit
 * 的进程上下文。
 *
 * @skb 的所有权无论发送成功还是失败都交给 Generic Netlink 发送路径；
 * 调用者不得再次释放。返回 0 或底层单播 errno，函数本身不会睡眠等待
 * 用户读取。英文中的 nl_pid 是旧术语，本实现实际从 @info 使用 port ID。
 */
static int send_reply(struct sk_buff *skb, struct genl_info *info)
{
	struct genlmsghdr *genlhdr = nlmsg_data(nlmsg_hdr(skb));
	void *reply = genlmsg_data(genlhdr);

	/* genlmsg_end 根据起始头指针回填消息长度，之后 skb 才可发送。 */
	genlmsg_end(skb, reply);

	return genlmsg_reply(skb, info);
}

/*
 * Send taskstats data in @skb to listeners registered for @cpu's exit data
 */
/*
 * send_cpu_listeners() - 向某一 CPU 链表中的所有端口广播一份退出快照。
 *
 * @skb: 已填属性但尚未 genlmsg_end 的消息；函数取得并最终消费其所有权。
 * @listeners: 当前退出 CPU 对应的 per-CPU 链表，调用者只借用指针。
 *
 * 调用位置：仅由 taskstats_exit() 在退出通知消息填完后调用；返回后
 * do_exit() 继续后续资源拆除。入口不持有 listener rwsem，本函数自行
 * 取得读锁/写锁；skb_clone(GFP_KERNEL) 与 rwsem 路径允许睡眠。
 *
 * 遍历时持读锁，允许多个退出任务并行发送；每个接收者都必须获得独立
 * skb 所有权，所以除最后一个端口外预先 clone 下一份。若 clone 失败，
 * 本轮剩余 listener 收不到事件，这是无背压异步接口的允许退化。端口
 * 明确返回 -ECONNREFUSED 时先标无效，出读锁后再用写锁摘除并释放。
 * 函数无返回值，单个端口错误不会阻止退出路径继续，也不会重试。
 */
static void send_cpu_listeners(struct sk_buff *skb,
					struct listener_list *listeners)
{
	struct genlmsghdr *genlhdr = nlmsg_data(nlmsg_hdr(skb));
	struct listener *s, *tmp;
	struct sk_buff *skb_next, *skb_cur = skb;
	void *reply = genlmsg_data(genlhdr);
	int delcount = 0;

	/* 所有克隆都共享同一已经封口的消息内容。 */
	genlmsg_end(skb, reply);

	/*
	 * 读锁稳定链表节点的存储期。valid 在此锁域内写入，并由后续写锁
	 * 清理读取，因此不需要额外原子操作；注册/注销者也必须取写锁。
	 */
	down_read(&listeners->sem);
	list_for_each_entry(s, &listeners->list, list) {
		int rc;

		skb_next = NULL;
		if (!list_is_last(&s->list, &listeners->list)) {
			/*
			 * genlmsg_unicast 会消费 skb_cur。先克隆可保证发送当前
			 * 接收者后仍有一份交给下一个；
			 * 最后一个无需额外复制。
			 */
			skb_next = skb_clone(skb_cur, GFP_KERNEL);
			if (!skb_next)
				break;
		}
		rc = genlmsg_unicast(&init_net, skb_cur, s->pid);
		if (rc == -ECONNREFUSED) {
			/* 端口已不存在，延迟到退出读侧临界区后物理删除。 */
			s->valid = 0;
			delcount++;
		}
		skb_cur = skb_next;
	}
	up_read(&listeners->sem);

	if (skb_cur)
		/*
		 * clone 失败或链表为空时，
		 * 仍未被 unicast 消费的 skb 归这里释放。
		 */
		nlmsg_free(skb_cur);

	if (!delcount)
		return;

	/* Delete invalidated entries */
	/*
	 * 删除上一步判定为无接收者的节点。写锁既排斥发送遍历，也排斥
	 * 用户注册/注销，保证 list_del 后可立即 kfree 而无并发读者引用。
	 */
	down_write(&listeners->sem);
	list_for_each_entry_safe(s, tmp, &listeners->list, list) {
		if (!s->valid) {
			list_del(&s->list);
			kfree(s);
		}
	}
	up_write(&listeners->sem);
}

/*
 * exe_add_tsk() - 把任务最后一次 exec 对应文件的设备号和 inode 写入快照。
 *
 * @stats: 调用者拥有且已清零的输出快照，本函数只修改可执行文件字段。
 * @tsk:   借用的任务；调用者负责以 task 引用或退出上下文保证其存活。
 *
 * 调用位置：fill_stats() 的最后阶段；返回后快照即可交给 Netlink。
 * 入口不持有本文件锁；当前实现只用 task_lock、RCU 和 file 引用操作，
 * 不执行 GFP 分配或主动睡眠，但调用者本身位于查询/退出进程上下文。
 *
 * get_task_exe_file() 成功时返回带引用的 struct file，可跨越 mm 内部同步
 * 边界安全读取 inode，必须用 fput() 配对。内核线程或已无 exe_file 的
 * 任务返回 NULL，以两个零字段作为“没有可识别用户态映像”的 ABI 表示。
 */
static void exe_add_tsk(struct taskstats *stats, struct task_struct *tsk)
{
	/* No idea if I'm allowed to access that here, now. */
	/*
	 * 原注释表达了历史上的并发疑虑。
	 * 当前 helper 不直接解引用裸 exe_file，而是取得 file 引用；
	 * 引用保证随后的 inode 访问期间对象不会被释放。
	 */
	struct file *exe_file = get_task_exe_file(tsk);

	if (exe_file) {
		/* Following cp_new_stat64() in stat.c . */
		/*
		 * 设备号编码沿用 stat(2) 的 64 位编码约定，便于用户空间用
		 * device+inode 与文件系统对象匹配，而无需在内核中导出路径。
		 */
		stats->ac_exe_dev =
			huge_encode_dev(exe_file->f_inode->i_sb->s_dev);
		stats->ac_exe_inode = exe_file->f_inode->i_ino;
		fput(exe_file);
	} else {
		stats->ac_exe_dev = 0;
		stats->ac_exe_inode = 0;
	}
}

/*
 * fill_stats() - 为一个 task 构造完整、版本化的每 PID 统计快照。
 *
 * @user_ns/@pid_ns: 决定 UID/GID 与 PID 等身份字段的用户可见映射；均为
 *                   借用指针，调用期间不改变所有权。
 * @tsk:              被采集任务，调用者必须保证对象存活。
 * @stats:            调用者提供的输出缓冲区，函数先整体清零再完全填充。
 *
 * 调用位置：fill_stats_for_pid() 用它回答主动查询，taskstats_exit() 用它
 * 生成最终 PID 事件；返回后调用者继续封装或发送 skb。入口不持有本文件
 * 锁并处于进程上下文；当前各采集 helper 不主动睡眠，仍不能在字段采集
 * 中额外引入会与 task/signal 锁序冲突的阻塞操作。
 *
 * 该函数依次汇合 delayacct、基础进程记账、扩展 I/O/内存记账和 exe
 * 身份。它不取得全局快照锁，因此不同子系统字段可能来自稍有差异的
 * 时刻；退出路径中任务已不再执行，结果更接近最终值。返回无直接值，
 * 不转移任何引用，唯一副作用是覆盖 @stats。
 */
static void fill_stats(struct user_namespace *user_ns,
		       struct pid_namespace *pid_ns,
		       struct task_struct *tsk, struct taskstats *stats)
{
	memset(stats, 0, sizeof(*stats));
	/*
	 * Each accounting subsystem adds calls to its functions to
	 * fill in relevant parts of struct taskstsats as follows
	 *
	 *	per-task-foo(stats, tsk);
	 */
	/*
	 * 每个记账子系统只负责 struct taskstats 中自己的字段组；新增子系统
	 * 应在这里追加采集 helper，而不是让 Netlink 层理解其内部计数器。
	 */

	delayacct_add_tsk(stats, tsk);

	/* fill in basic acct fields */
	/*
	 * version 让用户空间按已知结构前缀解析；
	 * 上下文切换计数由调度器维护，
	 * bacct_add_tsk 再按传入命名空间转换身份并填运行时间等基础字段。
	 */
	stats->version = TASKSTATS_VERSION;
	stats->nvcsw = tsk->nvcsw;
	stats->nivcsw = tsk->nivcsw;
	bacct_add_tsk(user_ns, pid_ns, stats, tsk);

	/* fill in extended acct fields */
	/* 扩展记账补充内存高水位和 I/O 字节等字段。 */
	xacct_add_tsk(stats, tsk);

	/* add executable info */
	/*
	 * 最后取得 exe file 引用，
	 * 填入可用于用户空间关联二进制的稳定标识。
	 */
	exe_add_tsk(stats, tsk);
}

/*
 * fill_stats_for_pid() - 在调用者的 PID 命名空间中按虚拟 PID 查询任务。
 *
 * @pid: 用户请求的 vpid；0 或不存在的标识最终表现为 -ESRCH。
 * @stats: 成功时被完整覆盖的输出缓冲区，所有权始终属于调用者。
 *
 * 调用位置：cmd_attr_pid() 在预留 skb 载荷后调用；成功后立刻发送回复，
 * 失败则释放 skb。入口不持有本文件锁，处于可睡眠的 Netlink 进程上下文。
 *
 * find_get_task_by_vpid() 在查找成功时取得 task_struct 引用，使任务即使
 * 并发退出也不会在采集期间释放。身份字段按查询者当前 user/pid namespace
 * 映射。返回 0 或 -ESRCH；所有路径都平衡 task 引用。
 */
static int fill_stats_for_pid(pid_t pid, struct taskstats *stats)
{
	struct task_struct *tsk;

	tsk = find_get_task_by_vpid(pid);
	if (!tsk)
		return -ESRCH;
	/* 持有的 task 引用跨越整个多子系统采集阶段。 */
	fill_stats(current_user_ns(), task_active_pid_ns(current), tsk, stats);
	put_task_struct(tsk);
	return 0;
}

/*
 * tgid_stats_add_task() - 把一个线程的可累加字段并入 TGID 汇总对象。
 *
 * @stats: 线程组累计缓冲区；调用者必须通过 siglock 排除并发累计/复制。
 * @tsk:   借用的组内线程，调用者保证其存活并决定是否应计入。
 * @now_ns: 同一次查询/退出阶段统一采样的单调时间，单位纳秒。
 *
 * 调用位置：fill_stats_for_tgid() 合入活线程，fill_tgid_exit() 累计退出
 * 线程；返回后前者继续遍历，后者释放 siglock。两条入口都已持有
 * sighand->siglock 并关闭本地中断，本函数不得睡眠。
 *
 * 本函数累计 delayacct、存活时长、CPU 时间和上下文切换次数。它不填
 * PID、身份或 exe 等不可简单相加的字段，也不返回错误。相同线程只能在
 * 退出累计或“当前仍存活”查询中出现一次，否则 TGID 结果会重复计数。
 */
static void tgid_stats_add_task(struct taskstats *stats,
				struct task_struct *tsk, u64 now_ns)
{
	u64 delta, utime, stime;

	/*
	 * Each accounting subsystem calls its functions here to
	 * accumulate its per-task stats for tsk, into the per-tgid structure
	 *
	 *	per-task-foo(stats, tsk);
	 */
	/*
	 * 与 fill_stats() 相同，各子系统只累计适合求和的字段；TGID 汇总
	 * 不是任选一个线程的 taskstats，而是明确定义的可加字段集合。
	 */
	delayacct_add_tsk(stats, tsk);

	/* calculate task elapsed time in nsec */
	/*
	 * start_time 与 now_ns 同属单调时钟域，
	 * 差值不会受墙上时间校准影响。
	 */
	delta = now_ns - tsk->start_time;
	/* Convert to micro seconds */
	/* ABI 的 ac_etime 单位是微秒，do_div 就地完成 64 位单位换算。 */
	do_div(delta, NSEC_PER_USEC);
	stats->ac_etime += delta;

	task_cputime(tsk, &utime, &stime);
	stats->ac_utime += div_u64(utime, NSEC_PER_USEC);
	stats->ac_stime += div_u64(stime, NSEC_PER_USEC);

	stats->nvcsw += tsk->nvcsw;
	stats->nivcsw += tsk->nivcsw;
}

/*
 * fill_stats_for_tgid() - 生成“已退出线程累计 + 当前存活线程”的组快照。
 *
 * @tgid: 查询者活动 PID 命名空间中的线程组 ID。
 * @stats: 输出缓冲区；成功或失败都会写 version，只有成功时其余字段有效。
 *
 * 调用位置：cmd_attr_tgid() 在消息中预留统计载荷后调用；成功后发送，
 * 失败后丢弃整条回复。入口处于 Netlink 进程上下文且未持本文件锁；
 * 取得 siglock 后禁止睡眠，退出该锁域后才返回调用者。
 *
 * RCU 保证 find_task_by_vpid() 得到的裸 task 在查找窗口内不被释放；
 * lock_task_sighand() 在对象仍有效时取得 siglock，并稳定 signal/sighand
 * 以及线程链表的相关退出状态。先复制 signal->stats 中已退出线程历史，
 * 再遍历未退出线程补齐实时值。返回 0 或 -ESRCH，不取得长期引用。
 * 该锁可关闭本地中断，函数不能在持锁阶段睡眠。
 */
static int fill_stats_for_tgid(pid_t tgid, struct taskstats *stats)
{
	struct task_struct *tsk, *first;
	unsigned long flags;
	int rc = -ESRCH;
	u64 now_ns;

	/*
	 * Add additional stats from live tasks except zombie thread group
	 * leaders who are already counted with the dead tasks
	 */
	/*
	 * 已成为 zombie 的线程已经由 taskstats_exit() 累计进 signal->stats；
	 * 查询只补充 exit_state 为 0 的线程，
	 * 避免同一线程同时出现在历史区和
	 * 实时区。线程组 leader 也遵守这一规则，并无特殊重复计数资格。
	 */
	rcu_read_lock();
	first = find_task_by_vpid(tgid);

	if (!first || !lock_task_sighand(first, &flags))
		goto out;

	/*
	 * siglock 同时排斥 fill_tgid_exit() 对累计对象的写入。stats 尚未按
	 * 本次查询修改，因此先复制历史基线；
	 * 尚未分配表示没有退出线程历史。
	 */
	if (first->signal->stats)
		memcpy(stats, first->signal->stats, sizeof(*stats));
	else
		memset(stats, 0, sizeof(*stats));

	now_ns = ktime_get_ns();
	/*
	 * 整个组复用同一个 now_ns，保证各线程 elapsed time 的观察截止点
	 * 一致。exit_state 非零者已经进入退出协议，由历史累计路径负责。
	 */
	for_each_thread(first, tsk) {
		if (tsk->exit_state)
			continue;

		tgid_stats_add_task(stats, tsk, now_ns);
	}

	unlock_task_sighand(first, &flags);
	rc = 0;
out:
	rcu_read_unlock();

	stats->version = TASKSTATS_VERSION;
	/*
	 * Accounting subsystems can also add calls here to modify
	 * fields of taskstats.
	 */
	/*
	 * 这里是线程组级派生字段的扩展点。
	 * 即使查找失败也写 version，调用者
	 * 仍以 rc 判定载荷无效，不会把未完成快照发送给用户。
	 */
	return rc;
}

/*
 * fill_tgid_exit() - 在线程退出时把其最终可累加字段记入线程组历史。
 *
 * @tsk: 正在 do_exit() 中退出的当前任务；调用者保证 task、signal 和
 *       sighand 在此阶段仍存活，函数只借用它们。
 *
 * 调用位置：taskstats_exit() 在按需分配 TGID 对象后调用；返回后可能
 * 构造 PID/TGID 退出消息。入口未持 siglock，本函数自行 irqsave 加锁；
 * 锁内只做计时和数值累计，不得睡眠。
 *
 * siglock 与 fill_stats_for_tgid() 的复制/活线程遍历串行化，并避免多个
 * 线程同时更新同一组累计值。若 TGID 累计对象未分配，说明该组无需维护
 * 组统计，本次静默跳过。返回无直接值；
 * 成功的可观察副作用是 signal->stats
 * 增加该线程最终值，供后续 TGID 查询或最后一个线程退出事件使用。
 */
static void fill_tgid_exit(struct task_struct *tsk)
{
	unsigned long flags;
	u64 now_ns;

	/*
	 * irqsave 形式既保护累计对象，也与其他以 irq 关闭方式取得 siglock
	 * 的信号/退出路径保持锁上下文一致；
	 * 持锁期间只做不可睡眠的数值采集。
	 */
	spin_lock_irqsave(&tsk->sighand->siglock, flags);
	if (!tsk->signal->stats)
		goto ret;

	now_ns = ktime_get_ns();
	/* 每个退出线程只在 do_exit() 的这一位置累计一次。 */
	tgid_stats_add_task(tsk->signal->stats, tsk, now_ns);
ret:
	/* 无论是否存在累计对象，都恢复进入函数前的中断状态。 */
	spin_unlock_irqrestore(&tsk->sighand->siglock, flags);
	return;
}

/*
 * add_del_listener() - 在 CPU mask 覆盖的每 CPU 链表中注册或注销端口。
 *
 * @pid: Netlink port ID，命名因历史原因使用 pid；不持有 task/PID 引用。
 * @mask: 借用的 CPU 集，必须只包含 possible CPU，以便 per-CPU 存储存在。
 * @isadd: REGISTER 执行幂等注册；其他值走注销/失败回滚路径。
 *
 * 调用位置：注册/注销命令处理器在解析 mask 后调用；返回后释放临时
 * cpumask 并结束命令。入口不持有 listener 锁；GFP_KERNEL 与 rwsem
 * 都允许睡眠，因此必须从 Netlink 进程上下文调用。
 *
 * 只有初始 user namespace 与初始 PID namespace 中的调用者可建立退出
 * 事件订阅，防止容器用 init_net 的全局端口观察宿主任务。注册为每个
 * CPU 分别分配节点，并在写锁下去重；中途 -ENOMEM 时 cleanup 会撤销
 * 此 port 在整个 mask 中的节点，使调用者不会得到半注册状态。注销找不到
 * 节点也返回 0。函数可因 GFP_KERNEL 和 rwsem 睡眠。
 */
static int add_del_listener(pid_t pid, const struct cpumask *mask, int isadd)
{
	struct listener_list *listeners;
	struct listener *s, *tmp, *s2;
	unsigned int cpu;
	int ret = 0;

	/* possible mask 是 per_cpu(listener_array, cpu) 可安全索引的边界。 */
	if (!cpumask_subset(mask, cpu_possible_mask))
		return -EINVAL;

	/*
	 * family 虽声明 netnsok，但退出推送固定使用 init_net/init namespaces；
	 * 因而只有宿主初始命名空间可以注册，避免命名空间语义混杂。
	 */
	if (current_user_ns() != &init_user_ns)
		return -EINVAL;

	if (task_active_pid_ns(current) != &init_pid_ns)
		return -EINVAL;

	if (isadd == REGISTER) {
		/*
		 * 一个订阅在每个目标 CPU 上有独立节点，分配到对应 NUMA node
		 * 可改善退出热路径访问局部性；
		 * 节点尚未入链时归本循环所有。
		 */
		for_each_cpu(cpu, mask) {
			s = kmalloc_node(sizeof(struct listener),
					GFP_KERNEL, cpu_to_node(cpu));
			if (!s) {
				ret = -ENOMEM;
				goto cleanup;
			}
			s->pid = pid;
			s->valid = 1;

			listeners = &per_cpu(listener_array, cpu);
			down_write(&listeners->sem);
			/*
			 * 同一有效 port 对同一 CPU 的重复注册
			 * 是成功的幂等操作。
			 */
			list_for_each_entry(s2, &listeners->list, list) {
				if (s2->pid == pid && s2->valid)
					goto exists;
			}
			list_add(&s->list, &listeners->list);
			/* NULL 表示节点所有权已转给链表，出锁后不得释放。 */
			s = NULL;
exists:
			up_write(&listeners->sem);
			/*
			 * 重复项仍由局部变量持有；
			 * 新插入项传 NULL，kfree 为 no-op。
			 */
			kfree(s); /* nop if NULL */
		}
		return 0;
	}

	/* Deregister or cleanup */
	/*
	 * 显式注销与注册失败共用同一逆操作：逐 CPU 摘除该 port。注册失败
	 * 之前尚未处理的 CPU 本来就没有节点，遍历找不到即可安全跳过。
	 */
cleanup:
	for_each_cpu(cpu, mask) {
		listeners = &per_cpu(listener_array, cpu);
		down_write(&listeners->sem);
		list_for_each_entry_safe(s, tmp, &listeners->list, list) {
			if (s->pid == pid) {
				list_del(&s->list);
				kfree(s);
				break;
			}
		}
		up_write(&listeners->sem);
	}
	return ret;
}

/*
 * parse() - 把 Netlink 字符串属性解析为内核 cpumask。
 *
 * @na: 可为 NULL；NULL 返回 1，表示“没有此属性”而非解析错误。
 * @mask: 调用者已分配的输出 cpumask，成功时写入解析结果。
 *
 * 调用位置：注册和注销处理器用它准备 add_del_listener() 的输入；返回后
 * 临时字符串已释放，只有 @mask 保留结果。入口不持锁，kmalloc(GFP_KERNEL)
 * 可能睡眠，故运行在 Netlink 进程上下文。
 *
 * 属性长度先受固定上限约束，再复制到临时 NUL 字符串供 cpulist_parse()
 * 解析诸如 "0-3,8" 的列表。返回 0、cpulist_parse 的负 errno，或本函数
 * 的 1/-E2BIG/-EINVAL/-ENOMEM。临时缓冲区在所有已分配路径上释放。
 */
static int parse(struct nlattr *na, struct cpumask *mask)
{
	char *data;
	int len;
	int ret;

	if (na == NULL)
		return 1;
	/* 先验证用户控制长度，避免无界 GFP_KERNEL 分配。 */
	len = nla_len(na);
	if (len > TASKSTATS_CPUMASK_MAXLEN)
		return -E2BIG;
	if (len < 1)
		return -EINVAL;
	data = kmalloc(len, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	/*
	 * nla_strscpy 保证临时缓冲区终止，解析器不会越过属性边界；mask 的
	 * possible-CPU 语义由随后 add_del_listener() 再验证。
	 */
	nla_strscpy(data, na, len);
	ret = cpulist_parse(data, mask);
	kfree(data);
	return ret;
}

/*
 * mk_reply() - 在 skb 中建立 PID/TGID 聚合属性并预留 taskstats 载荷。
 *
 * @skb: prepare_reply() 创建、仍由调用者拥有且尚未封口的消息。
 * @type: TASKSTATS_TYPE_PID 或 TASKSTATS_TYPE_TGID，决定嵌套聚合类型。
 * @pid:  与载荷配对导出的 PID/TGID 数值。
 *
 * 调用位置：PID/TGID 查询处理器和 taskstats_exit() 在消息构造阶段调用；
 * 返回后调用者填充预留区，失败则释放整个 skb。入口不持本文件锁，
 * 只操作已分配 skb，不主动睡眠。
 *
 * 成功返回指向 skb 线性数据内的可写 struct taskstats 区域，是借用指针，
 * 仅在 skb 存活且未改变布局时有效；失败返回 NULL，并撤销已开始的嵌套
 * 属性，但不释放 skb。64bit reserve 带 padding 属性，确保结构内 64 位
 * 字段满足 Netlink ABI 对齐要求。
 */
static struct taskstats *mk_reply(struct sk_buff *skb, int type, u32 pid)
{
	struct nlattr *na, *ret;
	int aggr;

	aggr = (type == TASKSTATS_TYPE_PID)
			? TASKSTATS_TYPE_AGGR_PID
			: TASKSTATS_TYPE_AGGR_TGID;

	/* 外层 AGGR_PID/AGGR_TGID 把标识属性和统计载荷绑定为一个逻辑记录。 */
	na = nla_nest_start_noflag(skb, aggr);
	if (!na)
		goto err;

	if (nla_put(skb, type, sizeof(pid), &pid) < 0) {
		nla_nest_cancel(skb, na);
		goto err;
	}
	ret = nla_reserve_64bit(skb, TASKSTATS_TYPE_STATS,
				sizeof(struct taskstats), TASKSTATS_TYPE_NULL);
	if (!ret) {
		nla_nest_cancel(skb, na);
		goto err;
	}
	/*
	 * reserve 只扩展并对齐 skb，内容由 fill_stats* 或 memcpy 随后写入；
	 * 在填充完成前绝不能发送该消息。
	 */
	nla_nest_end(skb, na);

	return nla_data(ret);
err:
	/* 调用者仍拥有 skb，并负责统一 nlmsg_free。 */
	return NULL;
}

/*
 * cgroupstats_user_cmd() - 处理按 cgroup 目录 fd 查询状态计数的命令。
 *
 * @skb: Generic Netlink 核心传入的请求 skb，本函数不使用也不持有。
 * @info: 借用的解析上下文，必须含 CGROUPSTATS_CMD_ATTR_FD。
 *
 * 调用位置：taskstats_ops 将 CGROUPSTATS_CMD_GET 直接分派到这里；返回后
 * Generic Netlink 核心完成命令收尾。入口不持本文件锁，运行在可睡眠的
 * doit 进程上下文，回复 skb 和 cgroup 构造失败均在本函数内回滚。
 *
 * CLASS(fd, f) 把用户 fd 稳定为带自动清理的 fd 引用，离开作用域时释放；
 * 无效/空 fd 沿用接口历史语义返回 0 且不发回复。
 * 成功路径构造一条回复，
 * cgroupstats_build() 根据 fd 对应 dentry 汇总 cgroup 状态；返回底层 errno，
 * 所有失败路径释放 reply skb，成功发送后所有权交给 Netlink。
 */
static int cgroupstats_user_cmd(struct sk_buff *skb, struct genl_info *info)
{
	int rc = 0;
	struct sk_buff *rep_skb;
	struct cgroupstats *stats;
	struct nlattr *na;
	size_t size;
	u32 fd;

	na = info->attrs[CGROUPSTATS_CMD_ATTR_FD];
	if (!na)
		return -EINVAL;

	fd = nla_get_u32(info->attrs[CGROUPSTATS_CMD_ATTR_FD]);
	/*
	 * cleanup class 在本作用域结束时自动执行 fdput；fd_file(f) 仅在该
	 * 生命周期内是借用指针，不能保存到异步工作中。
	 */
	CLASS(fd, f)(fd);
	if (fd_empty(f))
		return 0;

	/* 只为单个 cgroupstats 属性预留含头部和对齐的完整空间。 */
	size = nla_total_size(sizeof(struct cgroupstats));

	rc = prepare_reply(info, CGROUPSTATS_CMD_NEW, &rep_skb,
				size);
	if (rc < 0)
		return rc;

	na = nla_reserve(rep_skb, CGROUPSTATS_TYPE_CGROUP_STATS,
				sizeof(struct cgroupstats));
	if (na == NULL) {
		nlmsg_free(rep_skb);
		return -EMSGSIZE;
	}

	stats = nla_data(na);
	/* 构造器按增量写字段，先清零可保证未支持计数稳定返回 0。 */
	memset(stats, 0, sizeof(*stats));

	/*
	 * dentry 由 fd 引用稳定；构造器验证它属于可统计的 cgroup，并填入
	 * running/sleeping 等状态计数。失败时载荷尚未发布，可直接丢弃 skb。
	 */
	rc = cgroupstats_build(stats, fd_file(f)->f_path.dentry);
	if (rc < 0) {
		nlmsg_free(rep_skb);
		return rc;
	}

	return send_reply(rep_skb, info);
}

/*
 * cmd_attr_register_cpumask() - 解析注册属性并原子式建立 CPU 订阅集合。
 *
 * @info: 借用的请求上下文；port ID 标识接收端，属性保存 CPU 列表。
 *
 * 调用位置：taskstats_user_cmd() 选中 REGISTER 属性后调用；返回后命令
 * 结束。入口未持 listener 锁，cpumask 分配和 add_del_listener() 可睡眠。
 *
 * cpumask_var_t 兼容大 NR_CPUS 配置下的动态 mask。成功返回 0；解析、
 * 权限或分配错误原样返回。无论注册结果如何，临时 mask 均在 out 释放，
 * 已转移到 listener 链表的节点则由注销/失效清理负责。
 */
static int cmd_attr_register_cpumask(struct genl_info *info)
{
	cpumask_var_t mask;
	int rc;

	/* mask 只是解析工作区，不发布给其他线程。 */
	if (!alloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;
	rc = parse(info->attrs[TASKSTATS_CMD_ATTR_REGISTER_CPUMASK], mask);
	if (rc < 0)
		goto out;
	rc = add_del_listener(info->snd_portid, mask, REGISTER);
out:
	/* add_del_listener 已复制 CPU 选择，不借用该 mask。 */
	free_cpumask_var(mask);
	return rc;
}

/*
 * cmd_attr_deregister_cpumask() - 从指定 CPU 的订阅表移除请求端口。
 *
 * @info: 借用的请求上下文，snd_portid 是要删除的 Netlink port ID。
 * 调用位置：taskstats_user_cmd() 选中 DEREGISTER 属性后调用；返回后命令
 * 结束。入口未持 listener 锁，临时分配和注销时取得的 rwsem 允许睡眠。
 *
 * 返回解析/校验错误或 0；不存在的注册项视为已达到目标。临时 cpumask
 * 的分配、解析和释放协议与注册函数完全相同。
 */
static int cmd_attr_deregister_cpumask(struct genl_info *info)
{
	cpumask_var_t mask;
	int rc;

	if (!alloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;
	rc = parse(info->attrs[TASKSTATS_CMD_ATTR_DEREGISTER_CPUMASK], mask);
	if (rc < 0)
		goto out;
	rc = add_del_listener(info->snd_portid, mask, DEREGISTER);
out:
	free_cpumask_var(mask);
	return rc;
}

/*
 * taskstats_packet_size() - 计算一条 PID 或 TGID 聚合记录所需 skb 空间。
 *
 * 入参：无。返回值包含 u32 标识属性、64 位对齐的 taskstats 属性以及
 * 空载荷嵌套属性头的总和。它是容量估算，
 * 不改变全局状态且不分配内存；
 * taskstats_exit() 在同时携带 PID 与 TGID 时将结果乘二。
 * 调用者是 PID/TGID 查询处理器和 taskstats_exit()；函数不要求持锁、
 * 不睡眠，返回后结果直接传给 prepare_reply()。
 */
static size_t taskstats_packet_size(void)
{
	size_t size;

	/* nla_total_size* 同时计入属性头和 NLA_ALIGN 填充。 */
	size = nla_total_size(sizeof(u32)) +
		nla_total_size_64bit(sizeof(struct taskstats)) +
		nla_total_size(0);

	return size;
}

/*
 * cmd_attr_pid() - 回答 TASKSTATS_CMD_ATTR_PID 单任务查询。
 *
 * @info: 借用的请求上下文；policy 已保证 PID 属性为 u32。
 *
 * 调用位置：taskstats_user_cmd() 选中 PID 属性后调用；成功发送或失败
 * 回滚后直接把状态返回 Generic Netlink。入口不持本文件锁，消息分配、
 * task 采集和发送均在可睡眠的 doit 进程上下文完成。
 *
 * 先按固定 ABI 估算并分配 skb，再建立 PID+STATS 嵌套记录，最后持有
 * task 引用采集数据。成功时 send_reply() 消费 skb；任何构造/查找失败
 * 都由 err 统一释放尚属本函数的 skb。返回 0 或 Netlink/分配/-ESRCH 等
 * errno，不留下部分回复。
 */
static int cmd_attr_pid(struct genl_info *info)
{
	struct taskstats *stats;
	struct sk_buff *rep_skb;
	size_t size;
	u32 pid;
	int rc;

	size = taskstats_packet_size();

	/* 在任何属性写入前取得完整容量，后续 mk_reply 失败可统一回滚。 */
	rc = prepare_reply(info, TASKSTATS_CMD_NEW, &rep_skb, size);
	if (rc < 0)
		return rc;

	rc = -EINVAL;
	/* stats 是 skb 内借用区域，发送或释放 skb 后立即失效。 */
	pid = nla_get_u32(info->attrs[TASKSTATS_CMD_ATTR_PID]);
	stats = mk_reply(rep_skb, TASKSTATS_TYPE_PID, pid);
	if (!stats)
		goto err;

	rc = fill_stats_for_pid(pid, stats);
	if (rc < 0)
		goto err;
	return send_reply(rep_skb, info);
err:
	/* 到达此处时 rep_skb 尚未交给 genlmsg_reply，仍由本函数释放。 */
	nlmsg_free(rep_skb);
	return rc;
}

/*
 * cmd_attr_tgid() - 回答 TASKSTATS_CMD_ATTR_TGID 线程组累计查询。
 *
 * @info: 借用的请求上下文；TGID 按调用者活动 PID 命名空间解释。
 *
 * 调用位置：taskstats_user_cmd() 选中 TGID 属性后调用；返回后结束命令。
 * 入口不持本文件锁，运行在可睡眠的 doit 进程上下文；组采集内部会短暂
 * 进入 RCU 读侧与 siglock 临界区，并在发送前全部退出。
 *
 * 消息所有权和错误回滚与 cmd_attr_pid() 相同；差别在于填充阶段取得
 * sighand siglock，复制已退出线程历史并合入仍存活线程。成功回复代表
 * 一个受该锁保护的组内快照，而不是与其他系统状态同步的全局快照。
 */
static int cmd_attr_tgid(struct genl_info *info)
{
	struct taskstats *stats;
	struct sk_buff *rep_skb;
	size_t size;
	u32 tgid;
	int rc;

	size = taskstats_packet_size();

	rc = prepare_reply(info, TASKSTATS_CMD_NEW, &rep_skb, size);
	if (rc < 0)
		return rc;

	rc = -EINVAL;
	/* 先建立 ABI 容器，再让组统计函数直接填充 skb 内预留区域。 */
	tgid = nla_get_u32(info->attrs[TASKSTATS_CMD_ATTR_TGID]);
	stats = mk_reply(rep_skb, TASKSTATS_TYPE_TGID, tgid);
	if (!stats)
		goto err;

	rc = fill_stats_for_tgid(tgid, stats);
	if (rc < 0)
		goto err;
	return send_reply(rep_skb, info);
err:
	/* mk_reply 或组查找失败时消息从未发布，安全直接释放。 */
	nlmsg_free(rep_skb);
	return rc;
}

/*
 * taskstats_user_cmd() - 分派 TASKSTATS_CMD_GET 的互斥属性动作。
 *
 * @skb: Generic Netlink 请求消息，本函数不直接读取或持有。
 * @info: 已按 taskstats policy 解析的属性集合，借用至子处理函数返回。
 *
 * 优先级固定为注册、注销、PID、TGID；若恶意请求同时携带多个属性，只
 * 执行第一个匹配动作，不组合多个副作用。没有支持属性返回 -EINVAL。
 * 各子函数可能睡眠，本入口由 Generic Netlink doit 进程上下文调用。
 */
static int taskstats_user_cmd(struct sk_buff *skb, struct genl_info *info)
{
	if (info->attrs[TASKSTATS_CMD_ATTR_REGISTER_CPUMASK])
		return cmd_attr_register_cpumask(info);
	else if (info->attrs[TASKSTATS_CMD_ATTR_DEREGISTER_CPUMASK])
		return cmd_attr_deregister_cpumask(info);
	else if (info->attrs[TASKSTATS_CMD_ATTR_PID])
		return cmd_attr_pid(info);
	else if (info->attrs[TASKSTATS_CMD_ATTR_TGID])
		return cmd_attr_tgid(info);
	else
		return -EINVAL;
}

/*
 * taskstats_tgid_alloc() - 按需为多线程组发布零初始化累计对象。
 *
 * @tsk: 正在退出的组内任务；借用其 signal/sighand，调用者保证二者存活。
 *
 * 调用位置：仅由 taskstats_exit() 在构造退出快照前调用；返回后若得到
 * 对象便立即累计当前线程。入口不持 siglock，GFP_KERNEL 候选分配可以
 * 睡眠；取得 siglock 后只发布指针，不执行可睡眠操作。
 *
 * 单线程组无需 TGID 历史，直接返回已有对象或 NULL。多线程组首次退出
 * 时在锁外用 GFP_KERNEL 分配，以免持 siglock 睡眠；多个线程可并发
 * 分配候选对象，锁内只有一个获胜并以 release 发布，其余候选出锁释放。
 * 成功返回 signal->stats 的借用指针，分配失败允许返回 NULL 并退化为
 * 只有 PID 退出统计，不阻塞任务退出，也不把错误传播给 do_exit()。
 */
static struct taskstats *taskstats_tgid_alloc(struct task_struct *tsk)
{
	struct signal_struct *sig = tsk->signal;
	struct taskstats *stats_new, *stats;

	/* Pairs with smp_store_release() below. */
	/*
	 * acquire 与首次发布的 release 配对：一旦看到非 NULL，便同时看到
	 * kmem_cache_zalloc 对整个对象完成的清零，不能读取半初始化统计。
	 */
	stats = smp_load_acquire(&sig->stats);
	/*
	 * 已有对象可直接复用；若线程组当前只有调用者，
	 * 退出事件的 PID 记录
	 * 已足够，避免为 TGID 再分配同内容对象。
	 */
	if (stats || thread_group_empty(tsk))
		return stats;

	/* No problem if kmem_cache_zalloc() fails */
	/*
	 * TGID 汇总是可选增强，内存紧张时保持 NULL 不影响退出正确性；只会
	 * 缺少该组的聚合统计与 TGID 退出载荷。
	 */
	stats_new = kmem_cache_zalloc(taskstats_cache, GFP_KERNEL);

	/*
	 * 锁内重新检查处理并发首次分配。
	 * 这里关闭本地中断并且绝不分配，
	 * 只决定哪个候选对象取得 signal->stats 的生命周期所有权。
	 */
	spin_lock_irq(&tsk->sighand->siglock);
	stats = sig->stats;
	if (!stats) {
		/*
		 * Pairs with smp_store_release() above and order the
		 * kmem_cache_zalloc().
		 */
		/*
		 * 英文中的 “above” 指本函数前面的 acquire 读取；release 还保证
		 * 清零发生在指针对无锁读取者可见之前。
		 * stats_new 即使为 NULL 也
		 * 可安全存入，表示本次内存分配退化。
		 */
		smp_store_release(&sig->stats, stats_new);
		stats = stats_new;
		stats_new = NULL;
	}
	spin_unlock_irq(&tsk->sighand->siglock);

	if (stats_new)
		/*
		 * 另一线程赢得发布竞赛，本候选从未可见，
		 * 仍由当前函数释放。
		 */
		kmem_cache_free(taskstats_cache, stats_new);

	return stats;
}

/* Send pid data out on exit */
/*
 * taskstats_exit() - 在 do_exit() 中累计并按当前 CPU 推送任务最终统计。
 *
 * @tsk: 正在退出的当前任务；调用点位于 exit_mm() 之前，因此 exe、mm
 *       相关记账对象仍可读取。函数借用 task/signal/sighand，不增加引用。
 * @group_dead: atomic_dec_and_test(signal->live) 的结果；非零表示这是线程组
 *              最后一个存活线程，需在记录上置 AGROUP 并可附带 TGID 汇总。
 *
 * 主要阶段是：确认 family 已发布；按需创建/累计 TGID 历史；检查当前 CPU
 * 是否有订阅者；构造每 PID 记录；若为组终结再追加 TGID 记录；
 * 广播消息。Netlink 分配或发送失败均被静默丢弃，
 * 不能阻塞或改变任务退出语义。函数无直接返回值；
 * 累计对象仍归 signal_struct，消息所有权最终交给发送路径。
 *
 * 本函数在退出进程上下文执行，可以在前半段 GFP_KERNEL 分配和取得 rwsem；
 * 但不会等待用户空间确认。raw_cpu_ptr 只在此处选择一个 per-CPU 实例；
 * 该实例是永久存储，即使随后发生调度，
 * 已取得的指针也不会失效，消息仍
 * 归入采样时选择的 CPU 事件流。
 */
void taskstats_exit(struct task_struct *tsk, int group_dead)
{
	int rc;
	struct listener_list *listeners;
	struct taskstats *stats;
	struct sk_buff *rep_skb;
	size_t size;
	int is_thread_group;

	/*
	 * late initcall 注册完成前没有可寻址的 family，
	 * 退出统计直接跳过。
	 */
	if (!family_registered)
		return;

	/*
	 * Size includes space for nested attributes
	 */
	/*
	 * 每条记录含嵌套 AGGR 属性、PID/TGID 标识和对齐后的 taskstats；
	 * 先估算容量，避免填充到一半才因 skb 尾部空间不足回滚。
	 */
	size = taskstats_packet_size();

	/*
	 * 返回非 NULL 表示该线程组已有或成功发布累计对象。
	 * 多线程组中的每个退出线程都会把自身最终值加入它；
	 * 单线程组不为等价 TGID 记录付费。
	 */
	is_thread_group = !!taskstats_tgid_alloc(tsk);
	if (is_thread_group) {
		/* PID + STATS + TGID + STATS */
		/*
		 * 最后线程可能发送两条聚合记录，
		 * 因此一次性把 skb 容量翻倍。
		 */
		size = 2 * size;
		/* fill the tsk->signal->stats structure */
		/*
		 * 在判断 group_dead 之前也必须累计：
		 * 非最后线程的历史要留给以后
		 * 的 TGID 查询和最终组退出消息。
		 */
		fill_tgid_exit(tsk);
	}

	/*
	 * 退出事件按此刻采样到的 CPU 分流。这里不取 rwsem 的 list_empty
	 * 只是节省构造消息的提示：
	 * 它可能与刚发生的注册竞争并漏掉这一条退出通知，
	 * 而无背压的事件接口本来就不保证无丢失。即使快速返回，
	 * TGID 历史仍已累计，主动查询不会缺少已退出线程的累计值。
	 */
	listeners = raw_cpu_ptr(&listener_array);
	if (list_empty(&listeners->list))
		return;

	rc = prepare_reply(NULL, TASKSTATS_CMD_NEW, &rep_skb, size);
	if (rc < 0)
		return;

	/*
	 * 异步退出 ABI 固定使用 init_pid_ns，使宿主 listener 得到全局 PID；
	 * 注册权限也被限制在初始命名空间，与此标识视图保持一致。
	 */
	stats = mk_reply(rep_skb, TASKSTATS_TYPE_PID,
			 task_pid_nr_ns(tsk, &init_pid_ns));
	if (!stats)
		goto err;

	/*
	 * 退出线程已停止正常执行，使用 init namespaces 填充最终每 PID 快照。
	 * fill_stats 会取得 exe file 引用，并在返回前释放。
	 */
	fill_stats(&init_user_ns, &init_pid_ns, tsk, stats);
	if (group_dead)
		/*
		 * AGROUP 告知用户这是传统“进程”的
		 * 最后一条线程退出记录。
		 */
		stats->ac_flag |= AGROUP;

	/*
	 * Doesn't matter if tsk is the leader or the last group member leaving
	 */
	/*
	 * TGID 累计归 signal_struct 而非 leader task 所有，因此最后退出者是否
	 * leader 并不影响完整性。只有确有累计对象且 group_dead 时才把 TGID
	 * 最终值附在本消息；其他退出只发送自己的 PID 记录。
	 */
	if (!is_thread_group || !group_dead)
		goto send;

	/* TGID 采用 init_pid_ns 视图，与上面的 PID 和 listener 权限模型一致。 */
	stats = mk_reply(rep_skb, TASKSTATS_TYPE_TGID,
			 task_tgid_nr_ns(tsk, &init_pid_ns));
	if (!stats)
		goto err;

	/*
	 * group_dead 后没有仍在运行的组员需要再合入；signal->stats 已由每个
	 * 线程退出依次累计，可直接复制为最终组快照并补 ABI 版本号。
	 */
	memcpy(stats, tsk->signal->stats, sizeof(*stats));
	stats->version = TASKSTATS_VERSION;

send:
	/*
	 * send_cpu_listeners 无论成功与否都消费 rep_skb，
	 * 退出路径不观察错误。
	 */
	send_cpu_listeners(rep_skb, listeners);
	return;
err:
	/* 属性预留失败时消息尚未发送，唯一待回滚资源是 rep_skb。 */
	nlmsg_free(rep_skb);
}

/*
 * taskstats_ops 把两个用户命令绑定到各自 policy 与 doit 回调。
 *
 * TASKSTATS_CMD_GET 支持 PID/TGID 查询和 CPU mask 订阅变更；
 * CGROUPSTATS_CMD_GET 复用 family 查询 cgroup fd。GENL_ADMIN_PERM 要求
 * 管理权限，两个 DONT_VALIDATE 标志保留较老 taskstats ABI 的非严格、
 * 非 dump 校验行为；回调仍自行完成语义验证。
 */
static const struct genl_ops taskstats_ops[] = {
	{
		.cmd		= TASKSTATS_CMD_GET,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit		= taskstats_user_cmd,
		.policy		= taskstats_cmd_get_policy,
		.maxattr	= ARRAY_SIZE(taskstats_cmd_get_policy) - 1,
		.flags		= GENL_ADMIN_PERM,
	},
	{
		.cmd		= CGROUPSTATS_CMD_GET,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit		= cgroupstats_user_cmd,
		.policy		= cgroupstats_cmd_get_policy,
		.maxattr	= ARRAY_SIZE(cgroupstats_cmd_get_policy) - 1,
	},
};

/*
 * family 是 taskstats Generic Netlink 协议的静态描述符。注册完成后标记
 * __ro_after_init，正常运行期不可再修改，既缩小误写面也让所有发送者
 * 共享稳定的 family ID/操作表。netnsok 允许命令经不同 netns 到达，
 * 但退出 listener 注册还会额外限制为初始 user/pid namespace。
 */
static struct genl_family family __ro_after_init = {
	.name		= TASKSTATS_GENL_NAME,
	.version	= TASKSTATS_GENL_VERSION,
	.module		= THIS_MODULE,
	.ops		= taskstats_ops,
	.n_ops		= ARRAY_SIZE(taskstats_ops),
	.resv_start_op	= CGROUPSTATS_CMD_GET + 1,
	.netnsok	= true,
};

/* Needed early in initialization */
/*
 * taskstats_init_early() - 在普通任务创建前准备 TGID slab 与每 CPU 链表。
 *
 * 入参：无。由 start_kernel() 的初始化主线调用，此时系统仍是单线程，
 * 无需与 listener/退出路径并发。KMEM_CACHE(..., SLAB_PANIC) 失败会直接
 * panic，因此函数无错误返回；成功后每个 possible CPU 都有空链表和
 * 已初始化 rwsem。返回无直接值，发布的 cache 持续整个内核运行期。
 */
void __init taskstats_init_early(void)
{
	unsigned int i;

	/*
	 * TGID 对象与 struct taskstats 同型，专用 cache 降低频繁线程组退出
	 * 的分配元数据开销；SLAB_PANIC 使后续路径无需处理 cache 不存在。
	 */
	taskstats_cache = KMEM_CACHE(taskstats, SLAB_PANIC);
	/*
	 * possible CPU 包括离线但未来可上线者，
	 * 预先初始化后热插拔无需补建。
	 */
	for_each_possible_cpu(i) {
		INIT_LIST_HEAD(&(per_cpu(listener_array, i).list));
		init_rwsem(&(per_cpu(listener_array, i).sem));
	}
}

/*
 * taskstats_init() - 向 Generic Netlink 核心注册用户可见 family。
 *
 * 入参：无。作为 late_initcall 在各记账机制完成初始化后运行，可以睡眠。
 * genl_register_family() 成功后 family ID 和 ops 对用户可见，随后置
 * family_registered 才允许退出路径构造异步消息。返回 0 或注册 errno；
 * 失败时接口保持关闭，但早期 cache/per-CPU 对象继续存在且无害。
 */
static int __init taskstats_init(void)
{
	int rc;

	/* 注册是用户与退出发送者观察 family 的发布边界。 */
	rc = genl_register_family(&family);
	if (rc)
		return rc;

	/*
	 * 只有注册完成才打开退出热路径门槛。
	 * family_registered 只在 initcall 中从 0 写成 1，运行期不再回退；
	 * 退出侧把它当作早期跳过提示，
	 * 真正可用性由先完成的 family 注册保证。
	 */
	family_registered = 1;
	pr_info("registered taskstats version %d\n", TASKSTATS_GENL_VERSION);
	return 0;
}

/*
 * late initcall ensures initialization of statistics collection
 * mechanisms precedes initialization of the taskstats interface
 */
/*
 * 先初始化 delayacct 等生产者，再开放 taskstats 消费接口，保证用户一旦
 * 能解析 family，就不会读到“接口已发布但底层统计尚未准备”的半初始化
 * 状态。late_initcall 只决定启动顺序，不表示运行期延迟执行每条请求。
 */
late_initcall(taskstats_init);
