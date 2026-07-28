// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 任务凭据管理学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * struct cred 是进程身份与权限的不可变快照：UID/GID、capability、补充组、
 * keyring、user namespace、ucounts 和 LSM 私有标签共同决定“这个任务是谁”
 * 以及“它以什么权限访问别的对象”。task->real_cred 是外界观察该任务时使用
 * 的客观凭据，task->cred 是任务主动访问对象时使用的主观凭据；通常二者
 * 相同，内核服务可临时 override 仅替换主观指针。
 *
 * 生命周期主线：
 *   prepare_creds()/cred_alloc_blank()/prepare_kernel_cred()
 *       分配可修改、尚未发布的新快照并取得所引用子对象
 *   → 调用者只修改这个私有副本
 *   → commit_creds() 用 RCU 同时发布 real_cred/cred
 *   → get_cred()/put_cred() 管理长期引用
 *   → 最后引用归零后 __put_cred()
 *   → RCU 宽限期结束由 put_cred_rcu() 释放所有子对象与 slab
 *
 * fork 由 copy_creds() 决定共享父凭据还是复制；exec 由
 * prepare_exec_creds() 清理不应跨 exec 保留的 keyring 与保存身份。更新
 * 采用“复制后发布”而不原地修改已提交 cred，使无锁 RCU 读者只能看到完整
 * 的旧快照或完整的新快照。代价是更新时复制、引用和延迟回收成本。
 *
 * 并发不变量：引用计数只保证对象存活，不冻结字段；字段不可变来自“已提交
 * cred 不得修改”的协议。RCU 保护 task 凭据指针的读取窗口和最终释放时机，
 * 不等于取得可跨越临界区的长期引用。commit_creds() 只允许 current 更新
 * 自己，避免还需要一把跨任务写锁。
 */
/* Task credentials management - see Documentation/security/credentials.rst
 *
 * Copyright (C) 2008 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */
/*
 * 本文件管理任务凭据，完整接口契约参见
 * Documentation/security/credentials.rst。版权与作者信息按原文保留。
 */

#define pr_fmt(fmt) "CRED: " fmt

#include <linux/export.h>
#include <linux/cred.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/coredump.h>
#include <linux/key.h>
#include <linux/keyctl.h>
#include <linux/init_task.h>
#include <linux/security.h>
#include <linux/binfmts.h>
#include <linux/cn_proc.h>
#include <linux/uidgid.h>

#if 0
/*
 * 调试宏有两种编译期实现：把 #if 0 改为真时会打印当前 comm/PID；正常构建
 * 使用永不执行的 no_printk() 分支，既不产生运行时日志，又让编译器继续
 * 检查格式串和参数类型。宏不参与凭据同步或生命周期。
 */
#define kdebug(FMT, ...)						\
	printk("[%-5.5s%5u] " FMT "\n",					\
	       current->comm, current->pid, ##__VA_ARGS__)
#else
#define kdebug(FMT, ...)						\
do {									\
	if (0)								\
		no_printk("[%-5.5s%5u] " FMT "\n",			\
			  current->comm, current->pid, ##__VA_ARGS__);	\
} while (0)
#endif

/*
 * cred_jar 是所有通用 struct cred 对象的专用 slab cache。cred_init() 在
 * 启动期创建，之后分配路径读取、最终 RCU 回调归还；SLAB_PANIC 保证初始化
 * 成功或直接终止启动，所以正常路径无需处理 cache 本身为 NULL。
 */
static struct kmem_cache *cred_jar;

/*
 * The RCU callback to actually dispose of a set of credentials
 */
/* RCU 回调负责真正销毁一组凭据；到达这里时旧的 RCU 读者均已离开。 */
/*
 * put_cred_rcu() - 在安全的回收时刻拆除 cred 及其持有的所有子引用。
 *
 * 调用关系：__put_cred() 在 usage 归零后直接调用或交给 call_rcu()；普通
 * 已发布凭据走后者，保证仍持裸 RCU 指针的读者不会遭遇 UAF。
 * @rcu: 嵌入 struct cred 的回收节点，借用给回调；container_of() 恢复
 *       所属 cred。回调取得该对象的最终销毁责任。
 * 上下文：RCU callback 上下文，不能睡眠；无外部锁。对象已不可再获得新
 * 长期引用，usage 必须为 0。
 * 返回：无直接返回值。释放 LSM、keyring、group_info、user、ucounts、
 * user_ns 引用，最后归还 slab；这些 ownership 全部在此闭环。
 */
static void put_cred_rcu(struct rcu_head *rcu)
{
	/*
	 * rcu 与 non_rcu 共用 union；当前已经进入回收阶段，按 rcu_head 成员
	 * 反推出对象地址是合法的，cred 只在本函数末尾才真正失效。
	 */
	struct cred *cred = container_of(rcu, struct cred, rcu);

	kdebug("put_cred_rcu(%p)", cred);

	/*
	 * 引用归零是进入回调的硬前置条件。若非零，说明有人在排队回收后又取得
	 * 引用，生命周期协议已被破坏；继续释放会制造 UAF，因此直接 panic。
	 */
	if (atomic_long_read(&cred->usage) != 0)
		panic("CRED: put_cred_rcu() sees %p with usage %ld\n",
		      cred, atomic_long_read(&cred->usage));

	/*
	 * 按对象持有关系逐项释放。LSM 先拆除自己的 cred 私有状态；四类 key
	 * 指针、补充组、user、ucounts 和 user_ns 都是构造时取得的持有引用，
	 * put helper 均能处理对应的空值条件（代码对可选对象仍显式判断）。
	 */
	security_cred_free(cred);
	key_put(cred->session_keyring);
	key_put(cred->process_keyring);
	key_put(cred->thread_keyring);
	key_put(cred->request_key_auth);
	if (cred->group_info)
		put_group_info(cred->group_info);
	free_uid(cred->user);
	if (cred->ucounts)
		put_ucounts(cred->ucounts);
	put_user_ns(cred->user_ns);
	/* 所有子对象均已释放，最后才能把 cred 本体归还专用 slab。 */
	kmem_cache_free(cred_jar, cred);
}

/**
 * __put_cred - Destroy a set of credentials
 * @cred: The record to release
 *
 * Destroy a set of credentials on which no references remain.
 */
/*
 * 销毁已经没有任何引用的一组凭据。@cred 是待释放记录；调用者把最后一个
 * usage 引用交给本函数，函数可能立即销毁未发布对象，或为已发布对象安排
 * RCU 延迟销毁。
 */
/*
 * __put_cred() - 在 cred 最后一个长期引用释放时启动最终回收。
 *
 * @cred: 非 NULL、usage 已原子减到 0 的输入对象；所有权已由最后一个
 *        put_cred() 转移到本函数，调用后不得再访问。
 * 上下文：可从多种 put 路径进入；本函数本身不睡眠。普通已发布对象必须
 * 经过 RCU 宽限期；仅从未暴露给 RCU 读者的 non_rcu 对象可同步回收。
 * 返回：无直接返回值。协议错误触发 BUG；成功则立即完成或排队最终释放。
 */
void __put_cred(struct cred *cred)
{
	kdebug("__put_cred(%p{%ld})", cred,
	       atomic_long_read(&cred->usage));

	/*
	 * 三个断言共同验证回收边界：引用必须恰好为零，且 current 的主观或
	 * 客观指针绝不能仍指向该对象。否则排队回收后当前任务仍可访问悬空 cred。
	 */
	BUG_ON(atomic_long_read(&cred->usage) != 0);
	BUG_ON(cred == current->cred);
	BUG_ON(cred == current->real_cred);

	/*
	 * non_rcu 表示对象从未进入需要 RCU 保护的可发现状态，可同步执行统一
	 * 析构函数；get_cred*() 会把它清零。普通已提交对象通过 call_rcu()
	 * 延迟到所有既有读侧临界区结束，引用计数归零本身不能替代这个宽限期。
	 */
	if (cred->non_rcu)
		put_cred_rcu(&cred->rcu);
	else
		call_rcu(&cred->rcu, put_cred_rcu);
}
EXPORT_SYMBOL(__put_cred);

/*
 * Clean up a task's credentials when it exits
 */
/* 任务退出时清理其凭据。此后 task 不再拥有主观或客观 cred 引用。 */
/*
 * exit_creds() - 从退出中的 task 摘除并释放两类凭据引用。
 *
 * @tsk: 正在 teardown 的 task，调用者保证其生命周期稳定且不会再执行权限
 *       检查；本函数借用 task 指针，不释放 task 本体。
 * 上下文：退出清理路径，无需本地 cred 写锁，不睡眠；外部退出协议阻止并发
 * 再次安装凭据。返回无直接值。
 * 副作用：先把 real_cred/cred 清为 NULL，再各释放一个引用；启用请求密钥
 * 缓存时也释放 cached_requested_key。最后引用可能触发 RCU 延迟回收。
 */
void exit_creds(struct task_struct *tsk)
{
	/*
	 * real_cred/cred 是从 task 借出的两个快照指针。即使地址相同，也代表
	 * task 分别持有的两个逻辑引用，必须合计释放两次。
	 */
	struct cred *real_cred, *cred;

	kdebug("exit_creds(%u,%p,%p,{%ld})", tsk->pid, tsk->real_cred, tsk->cred,
	       atomic_long_read(&tsk->cred->usage));

	/*
	 * 阶段 1：先从 task 摘除两个公开指针，再做 put。强制转换仅去掉用于
	 * 防止意外修改的 const；这里不会修改 cred 字段，只是释放引用。
	 */
	real_cred = (struct cred *) tsk->real_cred;
	tsk->real_cred = NULL;

	cred = (struct cred *) tsk->cred;
	tsk->cred = NULL;

	/*
	 * 同一对象时一次原子减 2，避免两次操作之间触发回收；不同对象则各自
	 * 归还。无论哪条分支，task 持有的客观/主观引用都精确闭合。
	 */
	if (real_cred == cred) {
		put_cred_many(cred, 2);
	} else {
		put_cred(real_cred);
		put_cred(cred);
	}

#ifdef CONFIG_KEYS_REQUEST_CACHE
	/*
	 * 请求密钥缓存属于 task 而不是 struct cred；只有该配置启用时存在。
	 * 先 put 再清指针，退出协议保证没有并发使用者。
	 */
	key_put(tsk->cached_requested_key);
	tsk->cached_requested_key = NULL;
#endif
}

/**
 * get_task_cred - Get another task's objective credentials
 * @task: The task to query
 *
 * Get the objective credentials of a task, pinning them so that they can't go
 * away.  Accessing a task's credentials directly is not permitted.
 *
 * The caller must also make sure task doesn't get deleted, either by holding a
 * ref on task or by holding tasklist_lock to prevent it from being unlinked.
 */
/*
 * 获取另一个任务的客观凭据并固定其生命周期。禁止直接读取 task->real_cred
 * 后在 RCU 区外使用，因为并发 commit/exit 可替换指针并回收旧对象。
 *
 * @task: 被查询任务的借用指针；调用者还必须持 task 引用，或持有能阻止其
 *        从任务表摘除的 tasklist_lock。该约束保护 task 本体，RCU 只保护
 *        cred 指针及其对象。
 * 返回：持有一个 usage 引用的 const cred，调用者必须 put_cred()；本函数
 * 不返回 NULL。可在不能睡眠的上下文使用，但重试次数取决于并发替换。
 */
const struct cred *get_task_cred(struct task_struct *task)
{
	/* cred 是 RCU 临界区内的借用候选，成功增引用后才可跨越临界区。 */
	const struct cred *cred;

	/*
	 * 阶段 1：RCU 保证读到的候选内存暂不释放；它不保证 usage 仍非零，
	 * 所以不能先退出读锁再使用普通 get_cred()。
	 */
	rcu_read_lock();

	/*
	 * __task_cred() 以 rcu_dereference() 读取已发布指针；
	 * get_cred_rcu() 仅在 usage 非零时原子加一。若恰逢旧 cred 已归零，
	 * 加引用失败便重新读取 task 的新指针，避免“复活”待回收对象。
	 */
	do {
		cred = __task_cred((task));
		BUG_ON(!cred);
	} while (!get_cred_rcu(cred));

	/* 成功引用已独立保证生命周期，退出 RCU 后把持有引用交给调用者。 */
	rcu_read_unlock();
	return cred;
}
EXPORT_SYMBOL(get_task_cred);

/*
 * Allocate blank credentials, such that the credentials can be filled in at a
 * later date without risk of ENOMEM.
 */
/*
 * 分配一份空白凭据，供调用者稍后填充；预先分配本体和 LSM 私有数据，可在
 * 后续不能容忍 ENOMEM 的阶段使用。返回对象尚未发布，字段仍需调用者初始化。
 */
/*
 * cred_alloc_blank() - 构造只含基础生命周期状态的空白 cred。
 *
 * 入参：无。进程上下文、可睡眠，GFP_KERNEL/GFP_KERNEL_ACCOUNT 可能触发
 * reclaim；不持 cred 锁。
 * 返回：成功时返回 usage=1 的可修改对象，调用者持有唯一引用并最终选择
 * commit/专用安装或 abort_creds()；slab 或 LSM 分配失败返回 NULL，函数已
 * 回滚本次对象，不遗留引用。
 */
struct cred *cred_alloc_blank(void)
{
	/* new 在成功出口是调用者拥有的未发布对象；error 路径负责销毁。 */
	struct cred *new;

	/* zalloc 让可选指针和标志从安全的零状态开始，再建立 usage 初始引用。 */
	new = kmem_cache_zalloc(cred_jar, GFP_KERNEL);
	if (!new)
		return NULL;

	/*
	 * LSM hook 可为多个安全模块分配组合安全 blob。失败时 usage 已为 1，
	 * 因而可用统一 abort/put 路径释放已完成的部分初始化。
	 */
	atomic_long_set(&new->usage, 1);
	if (security_cred_alloc_blank(new, GFP_KERNEL_ACCOUNT) < 0)
		goto error;

	return new;

error:
	/* 尚未发布，无外部观察者；abort_creds() 消费唯一引用并完整析构。 */
	abort_creds(new);
	return NULL;
}

/**
 * prepare_creds - Prepare a new set of credentials for modification
 *
 * Prepare a new set of task credentials for modification.  A task's creds
 * shouldn't generally be modified directly, therefore this function is used to
 * prepare a new copy, which the caller then modifies and then commits by
 * calling commit_creds().
 *
 * Preparation involves making a copy of the objective creds for modification.
 *
 * Returns a pointer to the new creds-to-be if successful, NULL otherwise.
 *
 * Call commit_creds() or abort_creds() to clean up.
 */
/*
 * 为修改准备一份当前任务客观凭据的私有副本。已提交 task cred 通常不能
 * 原地修改；调用者应只改新副本，最后用 commit_creds() 原子发布，或用
 * abort_creds() 放弃。成功返回待提交指针，失败返回 NULL。
 */
/*
 * prepare_creds() - 复制 current 的 cred 并取得所有子对象引用。
 *
 * 入参：无。调用者是 current，task->cred 在本任务上下文中稳定；进程上下文
 * 可睡眠，slab、ucounts 或 LSM 分配可能失败。
 * 返回：成功时 usage=1 的可修改新 cred，唯一引用归调用者；NULL 表示内存、
 * ucounts 或 LSM 准备失败，所有已取得引用均已回滚。成功后必须且只能选择
 * commit_creds()（消费引用）或 abort_creds()。
 */
struct cred *prepare_creds(void)
{
	/*
	 * 变量地图：task 是 current 的稳定借用指针；old 是已提交、只读的源
	 * 快照，不额外增引用；new 是本函数分配并逐步取得子引用的私有副本。
	 */
	struct task_struct *task = current;
	const struct cred *old;
	struct cred *new;

	/* 阶段 1：先取得 cred 本体；失败前尚未产生任何需要回滚的资源。 */
	new = kmem_cache_alloc(cred_jar, GFP_KERNEL);
	if (!new)
		return NULL;

	kdebug("prepare_creds() alloc %p", new);

	/*
	 * 阶段 2：按值复制身份字段和子对象指针，但 memcpy 只复制地址，不增加
	 * ownership。随后必须重置回收状态/usage，并逐项为共享子对象取得引用。
	 */
	old = task->cred;
	memcpy(new, old, sizeof(struct cred));

	/*
	 * 新对象尚未发布，non_rcu 清零使其一旦经常规引用路径使用就采用 RCU
	 * 回收；usage=1 是调用者最终将消费的构造引用。
	 */
	new->non_rcu = 0;
	atomic_long_set(&new->usage, 1);
	get_group_info(new->group_info);
	get_uid(new->user);
	get_user_ns(new->user_ns);

#ifdef CONFIG_KEYS
	/* 启用 key 子系统时，复制的四个 key 指针各取得一份长期引用。 */
	key_get(new->session_keyring);
	key_get(new->process_keyring);
	key_get(new->thread_keyring);
	key_get(new->request_key_auth);
#endif

#ifdef CONFIG_SECURITY
	/*
	 * 不能复制 old->security 的裸地址：LSM blob 具有独立生命周期。
	 * 先清空，稍后的 security_prepare_creds() 为 new 创建/复制安全状态。
	 */
	new->security = NULL;
#endif

	/*
	 * ucounts 与 user namespace/uid 组合绑定；get_ucounts() 可能在引用已死
	 * 时失败。成功后 new 独立持有引用，失败则统一撤销此前全部子引用。
	 */
	new->ucounts = get_ucounts(new->ucounts);
	if (!new->ucounts)
		goto error;

	/* LSM 最后看到已完成通用字段的 new/old；失败仍由 error 统一回滚。 */
	if (security_prepare_creds(new, old, GFP_KERNEL_ACCOUNT) < 0)
		goto error;

	return new;

error:
	/*
	 * 到达这里时 new 本体以及若干/全部共享子对象引用已经取得，但从未发布；
	 * abort_creds() 消费 usage=1，并由统一析构按字段逆向释放。
	 */
	abort_creds(new);
	return NULL;
}
EXPORT_SYMBOL(prepare_creds);

/*
 * Prepare credentials for current to perform an execve()
 * - The caller must hold ->cred_guard_mutex
 */
/*
 * 为 current 的 execve() 准备凭据；调用者必须持 current->signal 的
 * cred_guard_mutex，序列化 exec、ptrace 与凭据相关操作，保证准备期间的
 * current cred 基线不会被相冲突路径改变。
 */
/*
 * prepare_exec_creds() - 构造符合 exec 边界语义的待提交凭据。
 *
 * 入参：无。主要由 exec 的 bprm 准备路径调用；必须持 cred_guard_mutex，
 * 进程上下文可睡眠。
 * 返回：成功时返回 usage=1 的私有可修改 cred，由 linux_binprm 暂时持有，
 * 后续 exec 安全检查继续调整并在不可回滚点提交；失败返回 NULL，内部已
 * 清理。尚无 task 指针被发布。
 */
struct cred *prepare_exec_creds(void)
{
	/* new 是 prepare_creds() 返回的完整副本，失败时没有 ownership。 */
	struct cred *new;

	/* 阶段 1：复用通用复制与子引用获取，ENOMEM/LSM 失败直接向上返回。 */
	new = prepare_creds();
	if (!new)
		return new;

#ifdef CONFIG_KEYS
	/* newly exec'd tasks don't get a thread keyring */
	/*
	 * 新执行映像不继承原线程 keyring：释放复制时取得的引用并清空指针，
	 * 后续需要时可按新身份重新创建，避免旧线程私有授权跨 exec 泄漏。
	 */
	key_put(new->thread_keyring);
	new->thread_keyring = NULL;

	/* inherit the session keyring; new process keyring */
	/*
	 * session keyring 保留继承；process keyring 则释放并置空，使新程序拥有
	 * 新的进程级 keyring 生命周期，而不是沿用旧映像的私有 keyring。
	 */
	key_put(new->process_keyring);
	new->process_keyring = NULL;
#endif

	/*
	 * 阶段 2：exec 后 saved/fs 身份以当前 effective 身份为新基线。
	 * 这里只修改未发布副本；后续二进制格式和 LSM 仍可继续调整 euid/egid。
	 */
	new->suid = new->fsuid = new->euid;
	new->sgid = new->fsgid = new->egid;

	return new;
}

/*
 * Copy credentials for the new process created by fork()
 *
 * We share if we can, but under some circumstances we have to generate a new
 * set.
 *
 * The new process gets the current process's subjective credentials as its
 * objective and subjective credentials
 */
/*
 * 为 fork 创建的新任务复制凭据。能够安全共享时直接增加父凭据引用；线程
 * keyring 或新 user namespace 等条件要求独立快照时才复制。子任务把当前
 * 进程的主观凭据同时作为自己的客观和主观凭据。
 */
/*
 * copy_creds() - 在 child 尚未发布前建立其 cred/real_cred 与相关计数。
 *
 * @p: 正在 copy_process() 中构造的子 task，借用且尚不可运行；入口时
 *     p->cred 是从 task_struct 复制来的父 current 主观凭据裸指针。
 * @clone_flags: clone 策略位图；CLONE_THREAD 允许线程共享 cred，
 *               CLONE_NEWUSER 要求创建新 user namespace。
 * 上下文：fork 构造期、进程上下文，可睡眠；失败时 child 尚未发布。
 * 返回：0 表示 p 已持有客观/主观各一份 cred 引用，并增加对应 NPROC 与
 * namespace active 引用；-ENOMEM 或 create_user_ns()/ucounts 错误表示
 * 未建立可用 child cred，本函数已释放自己的新 cred，copy_process() 继续
 * 按既有 cleanup 栈撤销更早资源。
 */
int copy_creds(struct task_struct *p, u64 clone_flags)
{
	/*
	 * new 是需要独立快照时的构造对象；ret 保存可向 fork 调用链传播的
	 * errno。共享快速路径不使用二者。
	 */
	struct cred *new;
	int ret;

#ifdef CONFIG_KEYS_REQUEST_CACHE
	/* 请求密钥缓存是每 task 临时状态，子任务绝不能继承父任务的缓存引用。 */
	p->cached_requested_key = NULL;
#endif

	/*
	 * 阶段 1，线程共享快速路径：只有 CLONE_THREAD 且父主观凭据没有线程
	 * 私有 keyring 时才能共享。若存在 thread_keyring，新线程需要独立 cred
	 * 才能建立自己的线程级密钥语义。
	 */
	if (
#ifdef CONFIG_KEYS
		!p->cred->thread_keyring &&
#endif
		clone_flags & CLONE_THREAD
	    ) {
		/*
		 * p->cred 已是复制 task_struct 时带来的同一裸指针。一次原子加 2
		 * 同时为 p->real_cred 与 p->cred 建立两个长期引用，并把返回指针
		 * 装入 real_cred；主观指针仍保持同一地址。
		 */
		p->real_cred = get_cred_many(p->cred, 2);
		kdebug("share_creds(%p{%ld})",
		       p->cred, atomic_long_read(&p->cred->usage));
		/*
		 * cred 引用之外，task 生命周期还要分别计入该 user/namespace 的
		 * RLIMIT_NPROC 使用量和 user namespace active 引用；退出清理有
		 * 对称的递减路径。
		 */
		inc_rlimit_ucounts(task_ucounts(p), UCOUNT_RLIMIT_NPROC, 1);
		get_cred_namespaces(p);
		return 0;
	}

	/* 阶段 2，复制慢路径：取得 current cred 的完整私有副本。 */
	new = prepare_creds();
	if (!new)
		return -ENOMEM;

	/*
	 * CLONE_NEWUSER 先让 new 切换到新 user namespace，再重绑 ucounts。
	 * 两步都发生在发布前；任何失败都可直接释放 new，不会让 child 暴露
	 * 半完成身份。
	 */
	if (clone_flags & CLONE_NEWUSER) {
		ret = create_user_ns(new);
		if (ret < 0)
			goto error_put;
		ret = set_cred_ucounts(new);
		if (ret < 0)
			goto error_put;
	}

#ifdef CONFIG_KEYS
	/* new threads get their own thread keyrings if their parent already
	 * had one */
	/*
	 * 若父凭据已有线程 keyring，复制时暂时取得了它的引用。无论创建线程
	 * 还是进程都先丢弃这份继承；CLONE_THREAD 再为新线程安装独立 keyring，
	 * 非线程子进程保持 NULL。
	 */
	if (new->thread_keyring) {
		key_put(new->thread_keyring);
		new->thread_keyring = NULL;
		if (clone_flags & CLONE_THREAD)
			install_thread_keyring_to_cred(new);
	}

	/* The process keyring is only shared between the threads in a process;
	 * anything outside of those threads doesn't inherit.
	 */
	/*
	 * process keyring 只在同一进程的线程之间共享；非 CLONE_THREAD 子进程
	 * 必须释放复制来的引用并置空，防止进程私有授权跨进程继承。
	 */
	if (!(clone_flags & CLONE_THREAD)) {
		key_put(new->process_keyring);
		new->process_keyring = NULL;
	}
#endif

	/*
	 * 阶段 3，提交到尚不可运行的 child。prepare_creds() 的构造引用直接
	 * 成为 p->cred 所需的一份，get_cred(new) 再增加一份供 real_cred；
	 * 此后两指针地址相同但各拥有一个逻辑引用。
	 */
	p->cred = p->real_cred = get_cred(new);
	/* 与共享路径相同，建立 task 对用户计数和命名空间的生命周期订阅。 */
	inc_rlimit_ucounts(task_ucounts(p), UCOUNT_RLIMIT_NPROC, 1);
	get_cred_namespaces(p);

	return 0;

error_put:
	/*
	 * new 从未挂到 p，唯一构造引用仍归本函数；put_cred() 完整释放新 user
	 * namespace/ucounts/LSM 等已经建立的子状态，并保留原始 ret errno。
	 */
	put_cred(new);
	return ret;
}

/*
 * cred_cap_issubset() - 判断 subset 的 permitted capability 是否不比
 * set 更强，用于 commit_creds() 判断一次身份变化是否可能提升权限。
 *
 * @set: 比较基准的只读借用 cred；@subset: 候选只读借用 cred。调用期间
 * 两者均由 current 持有而稳定，不取得额外引用，不睡眠、无副作用。
 * 返回 true 表示相同 user_ns 内集合包含，或跨 namespace 的祖先/owner
 * 规则认为 set 支配 subset；false 表示不能证明是权限子集。
 */
static bool cred_cap_issubset(const struct cred *set, const struct cred *subset)
{
	/* 两个局部 namespace 指针均借自相应 cred，只在函数调用期间有效。 */
	const struct user_namespace *set_ns = set->user_ns;
	const struct user_namespace *subset_ns = subset->user_ns;

	/* If the two credentials are in the same user namespace see if
	 * the capabilities of subset are a subset of set.
	 */
	/*
	 * 同一 user namespace 可直接比较 permitted capability 位图：
	 * subset 每一位都必须存在于 set 中。
	 */
	if (set_ns == subset_ns)
		return cap_issubset(subset->cap_permitted, set->cap_permitted);

	/* The credentials are in a different user namespaces
	 * therefore one is a subset of the other only if a set is an
	 * ancestor of subset and set->euid is owner of subset or one
	 * of subsets ancestors.
	 */
	/*
	 * 不同 user namespace 的 capability 位语义相对各自 namespace。只有
	 * set_ns 是 subset_ns 的祖先，且 set->euid 是下一级 namespace owner，
	 * 才能沿层级证明支配关系；循环逐级向 init_user_ns 回溯，不持有新引用，
	 * 因为 cred 对 user_ns 的引用保证整条父链在本次比较期间存在。
	 */
	for (;subset_ns != &init_user_ns; subset_ns = subset_ns->parent) {
		if ((set_ns == subset_ns->parent)  &&
		    uid_eq(subset_ns->owner, set->euid))
			return true;
	}

	return false;
}

/**
 * commit_creds - Install new credentials upon the current task
 * @new: The credentials to be assigned
 *
 * Install a new set of credentials to the current task, using RCU to replace
 * the old set.  Both the objective and the subjective credentials pointers are
 * updated.  This function may not be called if the subjective credentials are
 * in an overridden state.
 *
 * This function eats the caller's reference to the new credentials.
 *
 * Always returns 0 thus allowing this function to be tail-called at the end
 * of, say, sys_setgid().
 */
/*
 * 把新凭据安装到 current。使用 RCU 同时替换客观与主观指针；主观凭据不能
 * 正处于 override 状态。函数消费调用者对 @new 的引用，并恒返回 0，便于
 * set*id 系统调用在末尾直接 return commit_creds(new)。
 */
/*
 * commit_creds() - 完成不可回滚的凭据发布及关联计数切换。
 *
 * @new: usage 至少为 1、已完成所有字段/LSM 初始化的可修改构造对象；非空。
 *       成功调用后 ownership 被本函数消费，调用者不得再 abort 或修改。
 * 上下文：只允许 current 更新自己，task->cred 必须等于 real_cred；进程
 * 上下文，不持 cred 全局锁。通知 helper 可有外部副作用。
 * 返回：恒为 0。副作用包括 dumpability/pdeath_signal、keyring 身份、
 * NPROC 与 namespace active 计数、两个 RCU 指针、proc connector 通知；
 * 最后释放 old 的客观/主观两个引用。
 */
int commit_creds(struct cred *new)
{
	/*
	 * task 是 current 的稳定借用指针；old 由 task->real_cred 的两个现存
	 * 逻辑引用保护。发布完成前后都无需额外 get。
	 */
	struct task_struct *task = current;
	const struct cred *old = task->real_cred;

	kdebug("commit_creds(%p{%ld})", new,
	       atomic_long_read(&new->usage));

	/*
	 * 禁止在 override_creds() 期间提交：此时 task->cred 指向临时主观身份，
	 * 同时替换会丢失恢复链。new 必须仍有调用者构造引用。
	 */
	BUG_ON(task->cred != old);
	BUG_ON(atomic_long_read(&new->usage) < 1);

	get_cred(new); /* we will require a ref for the subj creds too */
	/*
	 * new 原有引用将成为 real_cred 引用；这里再取一份给 subjective cred。
	 * 从此本函数持有发布所需的两份引用，即使后续不再访问调用者 ownership。
	 */

	/* dumpability changes */
	/*
	 * 若有效/文件系统身份改变，或 new capability 不能证明是 old 的子集，
	 * 先把任务视为不可按普通规则转储并清除父死亡信号，避免权限边界变化后
	 * 仍保留可能泄露内存或触发信号的旧安全属性。
	 */
	if (!uid_eq(old->euid, new->euid) ||
	    !gid_eq(old->egid, new->egid) ||
	    !uid_eq(old->fsuid, new->fsuid) ||
	    !gid_eq(old->fsgid, new->fsgid) ||
	    !cred_cap_issubset(old, new)) {
		/* mm-less tasks share init_task's exec_state */
		/*
		 * 没有 mm 的内核线程共享 init_task exec_state，不能修改公共
		 * dumpability；有 mm 的用户任务才更新自己的执行状态。
		 */
		if (task->mm)
			task_exec_state_set_dumpable(suid_dumpable);
		task->pdeath_signal = 0;
		/*
		 * If a task drops privileges and becomes nondumpable,
		 * the dumpability change must become visible before
		 * the credential change; otherwise, a __ptrace_may_access()
		 * racing with this change may be able to attach to a task it
		 * shouldn't be able to attach to (as if the task had dropped
		 * privileges without becoming nondumpable).
		 * Pairs with a read barrier in __ptrace_may_access().
		 */
		/*
		 * 当任务降权并变为不可转储时，dumpability 必须先于新 cred 对其他
		 * CPU 可见；否则并发 __ptrace_may_access() 可能看到“已降权的新
		 * cred + 尚可转储的旧状态”并错误附加。smp_wmb() 与该读路径的
		 * read barrier 配对，建立发布顺序；它不延长对象生命周期。
		 */
		smp_wmb();
	}

	/* alter the thread keyring */
	/*
	 * 文件系统 UID/GID 变化会影响 key 权限判断；在发布新 cred 前通知 key
	 * 子系统调整 current 的线程 keyring 相关状态。
	 */
	if (!uid_eq(new->fsuid, old->fsuid))
		key_fsuid_changed(new);
	if (!gid_eq(new->fsgid, old->fsgid))
		key_fsgid_changed(new);

	/* do it
	 * RLIMIT_NPROC limits on user->processes have already been checked
	 * in set_user().
	 */
	/*
	 * 正式提交。set_user() 已检查 RLIMIT_NPROC；若真实 user 或 user_ns
	 * 改变，必须先给 new->ucounts 加一，再发布指针，避免并发观察新身份时
	 * 计数尚未建立。
	 */
	if (new->user != old->user || new->user_ns != old->user_ns)
		inc_rlimit_ucounts(new->ucounts, UCOUNT_RLIMIT_NPROC, 1);

	/*
	 * 两次 rcu_assign_pointer() 是对外可见的发布边界：初始化 new 的普通
	 * 写入先行于 RCU 读者取得指针。先 real_cred 后 cred 的短暂窗口只会被
	 * 遵守各自接口的读者看到；current 自身不并发执行另一提交。
	 */
	rcu_assign_pointer(task->real_cred, new);
	rcu_assign_pointer(task->cred, new);
	/*
	 * 发布后再撤销 old 的 NPROC 计数，使切换期间宁可短暂双计也不出现漏计。
	 * user_ns 改变时同步切换 namespace active 引用，保持命名空间存活。
	 */
	if (new->user != old->user || new->user_ns != old->user_ns)
		dec_rlimit_ucounts(old->ucounts, UCOUNT_RLIMIT_NPROC, 1);
	if (new->user_ns != old->user_ns)
		switch_cred_namespaces(old, new);

	/* send notifications */
	/*
	 * 只有 UID 四元组实际变化才向 proc connector 发送 UID 事件；通知发生在
	 * 发布后，因此观察者重新查询 task 时能看到 new。
	 */
	if (!uid_eq(new->uid,   old->uid)  ||
	    !uid_eq(new->euid,  old->euid) ||
	    !uid_eq(new->suid,  old->suid) ||
	    !uid_eq(new->fsuid, old->fsuid))
		proc_id_connector(task, PROC_EVENT_UID);

	/* GID 四元组采用相同的变更检测和发布后通知规则。 */
	if (!gid_eq(new->gid,   old->gid)  ||
	    !gid_eq(new->egid,  old->egid) ||
	    !gid_eq(new->sgid,  old->sgid) ||
	    !gid_eq(new->fsgid, old->fsgid))
		proc_id_connector(task, PROC_EVENT_GID);

	/* release the old obj and subj refs both */
	/*
	 * old 不再被 task 两个指针持有，统一释放两份逻辑引用。并发 RCU 读者
	 * 即使使 usage 最终归零，也由 __put_cred()/call_rcu() 保证宽限期后释放。
	 */
	put_cred_many(old, 2);
	return 0;
}
EXPORT_SYMBOL(commit_creds);

/**
 * abort_creds - Discard a set of credentials and unlock the current task
 * @new: The credentials that were going to be applied
 *
 * Discard a set of credentials that were under construction and unlock the
 * current task.
 */
/*
 * 丢弃仍在构造中的凭据并“解锁当前任务”。当前实现本身并不操作 mutex；
 * 英文描述保留历史接口措辞，实际锁若存在由调用者作用域管理。本函数只消费
 * @new 的一份引用。
 */
/*
 * abort_creds() - 放弃未提交 cred，闭合 prepare_*() 的失败/取消路径。
 *
 * @new: 非 NULL、usage 至少为 1 的持有引用；调用后 ownership 被消费。
 * 返回：无直接返回值。对象未发布时通常可直接回收；若曾取得普通引用，
 * put 协议会选择 RCU 回收。无失败返回，不修改 current 的已提交凭据。
 */
void abort_creds(struct cred *new)
{
	kdebug("abort_creds(%p{%ld})", new,
	       atomic_long_read(&new->usage));

	/* usage 异常表示调用者重复 abort 或传入了不受引用保护的对象。 */
	BUG_ON(atomic_long_read(&new->usage) < 1);
	put_cred(new);
}
EXPORT_SYMBOL(abort_creds);

/**
 * cred_fscmp - Compare two credentials with respect to filesystem access.
 * @a: The first credential
 * @b: The second credential
 *
 * cred_cmp() will return zero if both credentials have the same
 * fsuid, fsgid, and supplementary groups.  That is, if they will both
 * provide the same access to files based on mode/uid/gid.
 * If the credentials are different, then either -1 or 1 will
 * be returned depending on whether @a comes before or after @b
 * respectively in an arbitrary, but stable, ordering of credentials.
 *
 * Return: -1, 0, or 1 depending on comparison
 */
/*
 * 修正说明：上方正文写作 cred_cmp()，当前文件实际函数名是 cred_fscmp()；
 * 这里描述的返回契约适用于下方 cred_fscmp()，并没有另一个本地 cred_cmp()
 * 实现。原英文仍按上游内容保留。
 */
/*
 * 按文件访问相关身份比较两份凭据：fsuid、fsgid 与补充组全部相同才返回 0，
 * 表示基于 mode/uid/gid 的传统 DAC 判断会得到同样主体身份；该比较不包含
 * capability、LSM 标签或 keyring，不能据此断言所有安全决策等价。
 *
 * @a: 第一份只读借用 cred；@b: 第二份只读借用 cred。调用者保证二者存活，
 * 本函数不增引用、不修改对象、不睡眠。
 * 返回：相等为 0；否则按稳定但无业务大小含义的字典序返回 -1 或 1，便于
 * 缓存/树结构排序。同一组列表必须已按内核 group_info 约定规范化排序。
 */
int cred_fscmp(const struct cred *a, const struct cred *b)
{
	/*
	 * ga/gb 是从 cred 借出的补充组列表；g 是逐项比较下标。cred 引用保证
	 * group_info 指针在调用期间有效，已提交对象不可变保证内容稳定。
	 */
	struct group_info *ga, *gb;
	int g;

	/* 阶段 1：同一 cred 指针必然完全等价，是最便宜的快速路径。 */
	if (a == b)
		return 0;
	/* 先按 fsuid，再按 fsgid 比较；任一不同即可确定稳定顺序。 */
	if (uid_lt(a->fsuid, b->fsuid))
		return -1;
	if (uid_gt(a->fsuid, b->fsuid))
		return 1;

	if (gid_lt(a->fsgid, b->fsgid))
		return -1;
	if (gid_gt(a->fsgid, b->fsgid))
		return 1;

	/*
	 * 阶段 2：比较补充组对象。共享同一 group_info 可直接相等；NULL 被定义
	 * 为排在非 NULL 前。这里区分 NULL 与空列表，遵循对象表示的稳定排序。
	 */
	ga = a->group_info;
	gb = b->group_info;
	if (ga == gb)
		return 0;
	if (ga == NULL)
		return -1;
	if (gb == NULL)
		return 1;
	/* 组数量不同先按长度排序，数量相同才需要逐个 kgid 比较。 */
	if (ga->ngroups < gb->ngroups)
		return -1;
	if (ga->ngroups > gb->ngroups)
		return 1;

	/*
	 * 阶段 3：按规范化组数组逐项字典序比较。循环完成表示每个 kgid 都相等，
	 * 两份凭据对传统文件 UID/GID/补充组检查等价。
	 */
	for (g = 0; g < ga->ngroups; g++) {
		if (gid_lt(ga->gid[g], gb->gid[g]))
			return -1;
		if (gid_gt(ga->gid[g], gb->gid[g]))
			return 1;
	}
	return 0;
}
EXPORT_SYMBOL(cred_fscmp);

/*
 * set_cred_ucounts() - 让待提交 cred 的 ucounts 匹配其 user_ns 与 uid。
 *
 * @new: 调用者独占、尚未发布的可修改 cred；必须已有非 NULL old ucounts，
 *       且 new->user_ns/new->uid 已设置为最终候选值。本函数借用 new 指针，
 *       成功时在其内部转移 ucounts 持有引用。
 * 上下文：进程上下文，alloc_ucounts() 可能加锁和分配，因而可能睡眠。
 * 返回：0 表示原绑定已匹配或成功换绑；-EAGAIN 表示无法取得目标 ucounts，
 * 此时 new->ucounts 仍保持旧引用不变，调用者可安全 abort。
 */
int set_cred_ucounts(struct cred *new)
{
	/*
	 * old_ucounts 是 new 当前持有的引用；new_ucounts 仅在分配成功后成为新
	 * 持有引用。先获取新对象再释放旧对象，保证失败路径不破坏可析构状态。
	 */
	struct ucounts *new_ucounts, *old_ucounts = new->ucounts;

	/*
	 * This optimization is needed because alloc_ucounts() uses locks
	 * for table lookups.
	 */
	/*
	 * 若 namespace 和 uid 都相同，已有 ucounts 正是目标对象。该快速路径
	 * 避免 alloc_ucounts() 的哈希表查找锁，且不改变任何引用计数。
	 */
	if (old_ucounts->ns == new->user_ns && uid_eq(old_ucounts->uid, new->uid))
		return 0;

	/*
	 * 分配/查找目标 (user_ns, uid) 计数对象并取得引用；失败时用 -EAGAIN
	 * 表示当前无法建立计数订阅，而不是把半完成绑定发布出去。
	 */
	if (!(new_ucounts = alloc_ucounts(new->user_ns, new->uid)))
		return -EAGAIN;

	/*
	 * 成功后先把持有的新引用装入 cred，再归还旧引用；从这一行起 abort 或
	 * put_cred_rcu() 都会释放正确的新对象，不存在无 ucounts 的窗口。
	 */
	new->ucounts = new_ucounts;
	put_ucounts(old_ucounts);

	return 0;
}

/*
 * initialise the credentials stuff
 */
/* 初始化凭据子系统的基础分配设施；在任何动态 cred 分配之前执行。 */
/*
 * cred_init() - 启动期创建 struct cred 专用 slab cache。
 *
 * 入参：无；__init 阶段单次调用，不持锁。返回无直接值。
 * KMEM_CACHE 根据 struct cred 大小/对齐创建 cred_jar；HWCACHE_ALIGN 减少
 * 热字段跨 cache line，SLAB_ACCOUNT 纳入 memcg 记账，SLAB_PANIC 使失败
 * 直接终止启动，因此后续分配路径可假定 cache 始终有效。
 */
void __init cred_init(void)
{
	/* allocate a slab in which we can store credentials */
	/*
	 * 为凭据对象分配 slab cache。宏从类型名推导对象大小和对齐；这里只创建
	 * cache，不预先创建某个 task 的 cred。
	 */
	cred_jar = KMEM_CACHE(cred,
			      SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_ACCOUNT);
}

/**
 * prepare_kernel_cred - Prepare a set of credentials for a kernel service
 * @daemon: A userspace daemon to be used as a reference
 *
 * Prepare a set of credentials for a kernel service.  This can then be used to
 * override a task's own credentials so that work can be done on behalf of that
 * task that requires a different subjective context.
 *
 * @daemon is used to provide a base cred, with the security data derived from
 * that; if this is "&init_task", they'll be set to 0, no groups, full
 * capabilities, and no keys.
 *
 * The caller may change these controls afterwards if desired.
 *
 * Returns the new credentials or NULL if out of memory.
 */
/*
 * 为内核服务准备一份凭据，可用于临时覆盖任务主观身份，以便代表另一主体
 * 工作。@daemon 提供基础身份和 LSM 派生来源；若是 &init_task，基线具有
 * UID/GID 0、无补充组、完整 capability 且无 key。调用者之后仍可修改。
 * 成功返回新 cred，内存/引用/LSM 准备失败返回 NULL。
 */
/*
 * prepare_kernel_cred() - 从指定 daemon 客观凭据复制内核服务身份。
 *
 * @daemon: 非 NULL 的 task 借用指针；调用期间本体必须存活。函数通过
 *          get_task_cred() 单独固定其 real_cred，因此不借用裸 cred 出域。
 * 上下文：进程上下文，可睡眠；无 cred 写锁。
 * 返回：usage=1、未发布且可修改的新 cred，ownership 归调用者，通常交给
 * override_creds()/scoped_with_creds() 并最终 put；任何失败返回 NULL，
 * 本函数释放 old 和已构造 new 的全部引用。
 */
struct cred *prepare_kernel_cred(struct task_struct *daemon)
{
	/* old 是从 daemon 取得的持有引用；new 是待返回或回滚的私有对象。 */
	const struct cred *old;
	struct cred *new;

	/* NULL daemon 是调用契约错误，告警一次并在分配前失败。 */
	if (WARN_ON_ONCE(!daemon))
		return NULL;

	/* 阶段 1：先分配本体，再固定 daemon 客观 cred，避免并发提交导致悬空。 */
	new = kmem_cache_alloc(cred_jar, GFP_KERNEL);
	if (!new)
		return NULL;

	kdebug("prepare_kernel_cred() alloc %p", new);

	old = get_task_cred(daemon);

	/*
	 * 阶段 2：结构体赋值复制所有标量和子对象指针；随后重建本对象自己的
	 * usage/RCU 状态并为共享 user、namespace、groups 逐项取得引用。
	 */
	*new = *old;
	new->non_rcu = 0;
	atomic_long_set(&new->usage, 1);
	get_uid(new->user);
	get_user_ns(new->user_ns);
	get_group_info(new->group_info);

#ifdef CONFIG_KEYS
	/*
	 * 内核服务凭据不继承 daemon 的任何 keyring 或 request_key 授权。
	 * jit 策略回到线程 keyring 默认值，避免把用户任务密钥权限带入服务。
	 */
	new->session_keyring = NULL;
	new->process_keyring = NULL;
	new->thread_keyring = NULL;
	new->request_key_auth = NULL;
	new->jit_keyring = KEY_REQKEY_DEFL_THREAD_KEYRING;
#endif

#ifdef CONFIG_SECURITY
	/* LSM blob 不可浅复制；清空后由 security_prepare_creds() 独立构造。 */
	new->security = NULL;
#endif
	/*
	 * 取得 ucounts 引用后，new 的通用子对象 ownership 才完整；失败仍保留
	 * 可由 put_cred() 统一析构的状态。
	 */
	new->ucounts = get_ucounts(new->ucounts);
	if (!new->ucounts)
		goto error;

	/* 让 LSM 根据 daemon 的 old 标签生成适用于内核服务的新安全状态。 */
	if (security_prepare_creds(new, old, GFP_KERNEL_ACCOUNT) < 0)
		goto error;

	/* 成功：归还临时 old 引用，把 new 的唯一构造引用交给调用者。 */
	put_cred(old);
	return new;

error:
	/*
	 * 失败时 new 从未发布；先消费其构造引用并释放已取得子对象，再归还
	 * get_task_cred() 的 old 引用。两个对象地址不同，ownership 清晰独立。
	 */
	put_cred(new);
	put_cred(old);
	return NULL;
}
EXPORT_SYMBOL(prepare_kernel_cred);

/**
 * set_security_override - Set the security ID in a set of credentials
 * @new: The credentials to alter
 * @secid: The LSM security ID to set
 *
 * Set the LSM security ID in a set of credentials so that the subjective
 * security is overridden when an alternative set of credentials is used.
 */
/*
 * 在一组待使用的凭据中设置 LSM security ID，使该凭据被临时作为主观身份
 * 时，LSM 以指定安全主体执行检查。
 */
/*
 * set_security_override() - 请求 LSM 把 new 设置为 secid 所代表的主体。
 *
 * @new: 调用者独占、尚未提交的可修改 cred；借用且 ownership 不变。
 * @secid: LSM 安全标识值，由调用者从安全子系统语境取得，不是 UID。
 * 返回：security_kernel_act_as() 的 0 或负 errno；成功副作用位于 new 的
 * LSM blob，失败时调用者仍负责 abort/put。函数是否睡眠取决于 LSM hook，
 * 应在可睡眠的凭据准备上下文调用。
 */
int set_security_override(struct cred *new, u32 secid)
{
	/* 通用 cred 层不解释 secid，直接把“以此主体行动”的策略交给 LSM。 */
	return security_kernel_act_as(new, secid);
}
EXPORT_SYMBOL(set_security_override);

/**
 * set_create_files_as - Set the LSM file create context in a set of credentials
 * @new: The credentials to alter
 * @inode: The inode to take the context from
 *
 * Change the LSM file creation context in a set of credentials to be the same
 * as the object context of the specified inode, so that the new inodes have
 * the same MAC context as that inode.
 */
/*
 * 把新文件创建身份设置为指定 inode：传统 DAC 部分采用 inode 的所有者
 * fsuid/fsgid，MAC 部分由 LSM 根据该 inode 的对象标签建立创建上下文，
 * 从而让后续新 inode 具有相同的 MAC context。
 */
/*
 * set_create_files_as() - 为内核代表现有目录/对象创建文件准备主观凭据。
 *
 * @new: 调用者独占、尚未提交的可修改 cred；成功或失败后 ownership 不变。
 * @inode: 只读借用的来源 inode；调用者保证其生命周期，函数不取得引用。
 * 返回：来源 owner id 无效时 -EINVAL，且 new 尚未改动；否则先更新
 * fsuid/fsgid，再返回 security_kernel_create_files_as() 的 0/负 errno。
 * 注意 LSM 失败时 DAC 两字段已改变，调用者通常应整体 abort new，而不能
 * 假定失败完全无副作用。
 */
int set_create_files_as(struct cred *new, struct inode *inode)
{
	/*
	 * 先验证 inode owner 在当前内核 id 表示中有效，避免把无效 kuid/kgid
	 * 写入待使用凭据。此检查早于任何字段修改，形成干净的 -EINVAL 出口。
	 */
	if (!uid_valid(inode->i_uid) || !gid_valid(inode->i_gid))
		return -EINVAL;
	/*
	 * DAC 身份先写入 new，再让 LSM 根据 inode 建立 MAC 创建标签；new 仍是
	 * 私有对象，不会被并发权限检查观察到半完成状态。
	 */
	new->fsuid = inode->i_uid;
	new->fsgid = inode->i_gid;
	return security_kernel_create_files_as(new, inode);
}
EXPORT_SYMBOL(set_create_files_as);
