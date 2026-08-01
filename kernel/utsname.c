// SPDX-License-Identifier: GPL-2.0-only
/*
 * UTS namespace 生命周期学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件负责 UTS namespace 对象的创建、共享、引用、销毁，以及把该对象
 * 接入 proc/nsfs 与 setns() 的通用 namespace 框架。主链路是：
 *
 *   clone()/unshare()
 *     -> create_new_namespaces()
 *     -> copy_utsname()
 *        -> 共享旧对象，或 clone_uts_ns() 创建隔离副本
 *     -> nsproxy 持有新对象
 *
 *   /proc/<pid>/ns/uts 或 setns()
 *     -> utsns_operations
 *     -> utsns_get()/utsns_put()/utsns_install()
 *
 *   最后一个普通引用 put_uts_ns()
 *     -> free_uts_ns()
 *     -> 从 namespace 树摘除并经 RCU 延迟释放
 *
 * UTS 数据本体是 struct uts_namespace::name，其中保存 sysname、nodename、
 * release、version、machine 和 domainname。sethostname()/setdomainname() 以及
 * uname() 系列系统调用位于 kernel/sys.c；它们与这里复制 name 的路径
 * 共同使用全局 uts_sem，保证读者不会观察到一次字符串更新的中间状态。
 * 本文件不实现这些系统调用，也不决定各字段的用户 ABI。
 *
 * 对象同时受三类约束：ns_common 的普通引用保证内存存活，active 引用决定
 * namespace 树中的用户可见性，ucounts 则限制一个 user namespace 层级能够
 * 创建的 UTS namespace 数量。struct uts_namespace::user_ns 是持有引用，决定
 * 管理该对象所需能力；ucounts 记录配额归属；嵌入的 ns_common 提供 inode、
 * 类型、操作表、树节点和 RCU 回收节点。
 *
 * 这种设计让多个任务可廉价共享同一个 UTS 身份，并在 CLONE_NEWUTS 时仅复制
 * 很小的 name 快照；代价是创建、setns、任务退出和 namespace fd 必须严格
 * 配对普通引用与 active 引用，名字读写还要经过一把跨 UTS namespace 的全局
 * rwsem。CONFIG_UTS_NS=n 时本文件不会进入构建，头文件中的桩函数只允许共享
 * init UTS namespace，并对 CLONE_NEWUTS 返回 -EINVAL。
 */
/*
 *  Copyright (C) 2004 IBM Corporation
 *
 *  Author: Serge Hallyn <serue@us.ibm.com>
 */

#include <linux/export.h>
#include <linux/uts.h>
#include <linux/utsname.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/user_namespace.h>
#include <linux/proc_ns.h>
#include <linux/nstree.h>
#include <linux/sched/task.h>

/*
 * 所有动态 struct uts_namespace 的专用 slab cache。uts_ns_init() 在
 * 启动阶段创建并写入该指针；__ro_after_init 使初始化完成后的指针只读，
 * 防止运行期被意外改写。cache 内对象仍然可以正常分配、修改和释放。
 */
static struct kmem_cache *uts_ns_cache __ro_after_init;

/*
 * inc_uts_namespaces() - 为一次 UTS namespace 创建预留分层用户配额。
 *
 * 调用位置：clone_uts_ns() 的第一阶段。@ns 是借用的 owning user namespace，
 * 不可为空；current_euid() 选择当前有效 UID 在该 user namespace 中对应的
 * ucounts 项。调用处于进程上下文，alloc_ucounts() 必要时可能分配并睡眠，
 * 入口不要求持锁。
 *
 * 成功返回一个由调用者持有的 ucounts 引用，并已把 UCOUNT_UTS_NAMESPACES
 * 沿 user namespace 祖先链计数；失败返回 NULL，表示某层达到上限或无法取得
 * ucounts。成功后必须交给新 UTS 对象，或在后续构造失败时由
 * dec_uts_namespaces() 回滚。
 */
static struct ucounts *inc_uts_namespaces(struct user_namespace *ns)
{
	return inc_ucount(ns, current_euid(), UCOUNT_UTS_NAMESPACES);
}

/*
 * dec_uts_namespaces() - 归还 UTS namespace 配额及其 ucounts 引用。
 *
 * @ucounts 必须是 inc_uts_namespaces() 成功返回的持有引用，不可为空。
 * 调用者要么正在回滚尚未发布的对象，要么正在销毁最后一个普通引用
 * 已经消失的对象。
 * 函数无直接返回值；它沿祖先链递减 UCOUNT_UTS_NAMESPACES，最后 put ucounts。
 * 原子计数、短临界区和 RCU 延迟回收路径不睡眠；入口不要求持锁或
 * uts_sem，也不改变 UTS 名称内容。
 */
static void dec_uts_namespaces(struct ucounts *ucounts)
{
	dec_ucount(ucounts, UCOUNT_UTS_NAMESPACES);
}

/*
 * Clone a new ns copying an original utsname, setting refcount to 1
 * @old_ns: namespace to clone
 * Return ERR_PTR(-ENOMEM) on error (failure to allocate), new ns otherwise
 */
/*
 * 复制一个旧 UTS 名称来创建新 namespace，并把普通引用计数初始化为 1。
 * @old_ns 是要克隆的 namespace。
 * 原注释把失败概括为内存分配失败时返回 ERR_PTR(-ENOMEM)，成功返回
 * 新对象。
 * 当前实现还会在 UTS 配额耗尽时返回 ERR_PTR(-ENOSPC)，并原样传播
 * ns_common_init() 分配 namespace inode 等步骤产生的负 errno。
 *
 * clone_uts_ns() - 构造尚未安装到任何任务的独立 UTS namespace。
 *
 * 调用关系：copy_utsname() 在 CLONE_NEWUTS 路径调用本函数，成功结果随后被
 * create_new_namespaces() 放入临时 nsproxy；真正切换任务 namespace 发生在
 * 更外层的 switch_task_namespaces()。
 *
 * @user_ns: 新 UTS namespace 的 owning user namespace，借用输入且不可为空；
 *           它决定配额归属以及以后修改/安装该 UTS namespace 的能力边界。
 * @old_ns:  名称快照来源，借用输入且不可为空；copy_utsname() 已额外持有
 *           一个普通引用，因此本函数跨越可睡眠分配和 uts_sem 临界区时
 *           对象存活。
 *
 * 入口位于可睡眠的进程上下文，不持有 uts_sem。函数依次预留配额、分配
 * 并初始化 ns_common、在 uts_sem 读侧取得一致 name 快照、取得 user_ns
 * 引用，最后把完全初始化的对象加入 namespace 树。成功返回持有一个
 * 普通引用的新对象；失败返回错误指针，并逆序撤销已经取得的
 * slab 对象、inode 和配额，不发布半初始化对象，也不消耗 @old_ns 的引用。
 */
static struct uts_namespace *clone_uts_ns(struct user_namespace *user_ns,
					  struct uts_namespace *old_ns)
{
	/*
	 * 变量地图：
	 *   ns      正在构造的新对象；成功后其唯一普通引用转交调用者。
	 *   ucounts 已预留的分层 UTS 配额及持有引用，成功后转交 ns->ucounts。
	 *   err     当前失败阶段应返回的负 errno，各清理标签不改变它。
	 */
	struct uts_namespace *ns;
	struct ucounts *ucounts;
	int err;

	/*
	 * 配额必须先于 slab 分配：超限时无需创建任何 namespace 元数据。
	 * NULL 同时表示预留没有生效，因此可直接走不释放资源的 fail 出口。
	 */
	err = -ENOSPC;
	ucounts = inc_uts_namespaces(user_ns);
	if (!ucounts)
		goto fail;

	/*
	 * GFP_KERNEL 允许睡眠；zalloc 让尚未逐项初始化的指针和树状态
	 * 从零开始。
	 * 分配失败时只有 ucounts 需要归还。
	 */
	err = -ENOMEM;
	ns = kmem_cache_zalloc(uts_ns_cache, GFP_KERNEL);
	if (!ns)
		goto fail_dec;

	/*
	 * ns_common_init() 建立普通引用计数 1、类型/操作表、树节点，
	 * 并为非初始 namespace 分配 proc/nsfs inode 号。此时对象尚未
	 * 加入全局树，失败仍可
	 * 私下释放；成功后 ns_common_free() 才有需要回收的 inode 资源。
	 */
	err = ns_common_init(ns);
	if (err)
		goto fail_free;

	/*
	 * 从这里起配额引用由 ns->ucounts 记录。uts_sem 的读侧与 kernel/sys.c
	 * 中 sethostname()/setdomainname() 的写侧配对，使六个字符串来自同一
	 * 完整快照，而不是在写者的 memcpy/memset 中途复制出混合内容。
	 * get_user_ns() 取得的持有引用由 free_uts_ns() 对称归还；uts_sem 不负责
	 * user_ns 或 ucounts 的生命周期。
	 */
	ns->ucounts = ucounts;
	down_read(&uts_sem);
	memcpy(&ns->name, &old_ns->name, sizeof(ns->name));
	ns->user_ns = get_user_ns(user_ns);
	up_read(&uts_sem);
	/*
	 * 所有字段与回收责任就绪后才分配稳定 namespace ID，并接入按类型、
	 * 统一以及 owner 组织的树。此时普通引用保证对象存活；active 引用
	 * 仍会在临时 nsproxy 真正安装给任务时由外层取得。
	 */
	ns_tree_add(ns);
	return ns;

fail_free:
	/* ns_common_init() 失败时没有可释放的 common 资源，直接归还 slab。 */
	kmem_cache_free(uts_ns_cache, ns);
fail_dec:
	/* slab 分配之后的任一失败都要撤销最先取得的分层配额。 */
	dec_uts_namespaces(ucounts);
fail:
	/*
	 * 所有已取得资源均已回滚；错误指针把具体失败阶段交给
	 * 外层处理。
	 */
	return ERR_PTR(err);
}

/*
 * Copy task tsk's utsname namespace, or clone it if flags
 * specifies CLONE_NEWUTS.  In latter case, changes to the
 * utsname of this process won't be seen by parent, and vice
 * versa.
 */
/*
 * 复制任务的 UTS namespace：未指定 CLONE_NEWUTS 时共享原对象；指定时
 * 克隆原 UTS 名称。后一种情况下，本进程之后对 utsname 的修改与父进程
 * 彼此隔离。原注释中的 tsk 是历史性表述；当前接口不接收 task_struct，
 * 而由调用者直接传入 flags、owning user namespace 和旧 UTS namespace。
 *
 * copy_utsname() - 为正在构造的 nsproxy 取得一个 UTS namespace 引用。
 *
 * 调用关系：create_new_namespaces() 在 clone()/unshare()/setns 准备临时
 * nsproxy 时调用；返回值随后写入 nsproxy->uts_ns，并在该 nsproxy 回滚或
 * 销毁时由 put_uts_ns() 释放。
 *
 * @flags:   clone/unshare namespace 标志位；本函数只解释 CLONE_NEWUTS，
 *           其他位不改变这里的选择。
 * @user_ns: 新对象的 owning user namespace，借用且不可为空；仅克隆分支使用。
 * @old_ns:  当前 nsproxy 中的 UTS namespace，借用且必须有效；BUG_ON 明确把
 *           NULL 视为内核内部不变量破坏，而不是可返回给用户的普通错误。
 *
 * 共享快路径不分配、不会因资源不足失败，返回指向 @old_ns 的一个新普通
 * 引用。克隆慢路径可能睡眠，成功返回新对象的唯一普通引用，失败返回
 * clone_uts_ns() 的错误指针。无论克隆成功还是失败，为稳定 @old_ns
 * 临时取得的引用都会在返回前归还，因此调用者只接收最终返回对象的
 * 一份引用。
 */
struct uts_namespace *copy_utsname(u64 flags,
	struct user_namespace *user_ns, struct uts_namespace *old_ns)
{
	/* new_ns 仅在隔离分支有效，承载新对象或错误指针。 */
	struct uts_namespace *new_ns;

	/*
	 * nsproxy 的 UTS 槽位按设计不允许为空。先取得引用，使 old_ns 在后续
	 * 可能睡眠的克隆过程以及 uts_sem 读侧复制期间都不会被并发释放。
	 */
	BUG_ON(!old_ns);
	get_uts_ns(old_ns);

	/*
	 * 共享路径把刚取得的引用直接转交给新 nsproxy，名称内容
	 * 没有被复制。
	 */
	if (!(flags & CLONE_NEWUTS))
		return old_ns;

	/*
	 * 隔离路径得到独立 name 快照；错误指针同样暂存在 new_ns 中
	 * 向上传递。
	 */
	new_ns = clone_uts_ns(user_ns, old_ns);

	/*
	 * clone_uts_ns() 不消费来源引用，故克隆结果无论成败都在此
	 * 对称归还。
	 */
	put_uts_ns(old_ns);
	return new_ns;
}

/*
 * free_uts_ns() - 销毁最后一个普通引用已经归零的动态 UTS namespace。
 *
 * 调用关系：put_uts_ns() 的 ns_ref_put() 确认普通引用降为零后调用。此时
 * ns_common 协议已经要求 active 引用为零，对象不再被任务、namespace fd
 * 或 bind mount 活跃使用；初始 UTS namespace 的固定引用永不进入本函数。
 *
 * @ns 是待销毁对象的独占回收指针，不可为空；调用者已消费最后一个普通
 * 引用，本函数不再返回引用。入口不持有 uts_sem，因为名称已无合法
 * 写者；函数从树中摘除对象，归还创建配额和 owning user_ns 引用，
 * 释放 common inode 资源，最后等待 RCU 宽限期后释放 slab 内存。
 * 返回：无直接返回值；
 * 副作用是对象从 ID/owner 遍历结构消失并最终结束存储期。
 * 当前实现只使用 namespace tree 自身的短写侧临界区、引用归还和 RCU 回调，
 * 不睡眠；调用者不得持有仍需要访问 @ns 的外部状态。
 */
void free_uts_ns(struct uts_namespace *ns)
{
	/*
	 * 先在 namespace tree 写侧同步下摘除所有索引，阻止新的树查找者
	 * 取得它；已经开始的 RCU 遍历仍可能暂时持有裸指针，因此此处
	 * 不能立刻释放内存。
	 */
	ns_tree_remove(ns);
	/*
	 * 对象已不可重新发现，按创建时取得资源的反向顺序归还
	 * 分层配额、显式 owning user_ns 引用和 ns_common 分配的 inode 号。
	 */
	dec_uts_namespaces(ns->ucounts);
	put_user_ns(ns->user_ns);
	ns_common_free(ns);
	/* Concurrent nstree traversal depends on a grace period. */
	/*
	 * 并发 nstree 遍历依赖一个 RCU 宽限期：摘除只阻止新读者找到对象，
	 * 已经进入读侧临界区的遍历仍可能访问 ns 内存，所以把真正释放
	 * 延后到这些读者全部退出之后。ns.ns_rcu 使用嵌入 ns_common 的
	 * rcu_head，不另行分配。
	 */
	kfree_rcu(ns, ns.ns_rcu);
}

/*
 * utsns_get() - 从指定任务取得其 UTS namespace 的持有引用。
 *
 * 调用关系：procfs 打开/readlink /proc/<pid>/ns/uts 等通用 namespace 路径
 * 通过 utsns_operations.get 调用；成功返回的 ns_common 交给 nsfs/procfs，
 * 最终必须由 utsns_put() 配对释放。
 *
 * @task: 要检查的任务，调用者保证 task_struct 本身存活；本函数只借用该
 *        指针。目标可能正在退出，此时 task->nsproxy 可以已经变为 NULL。
 *
 * task_lock() 与 switch_task_namespaces()/exit_nsproxy_namespaces() 修改目标
 * task->nsproxy 的路径配对。在锁内读取 UTS 指针并增加普通引用，才能在解锁
 * 后安全返回；只复制裸指针会让并发退出释放 nsproxy/UTS 对象而造成 UAF。
 * 函数不持 uts_sem、不读取名称，也不取得 active 引用。成功返回指向嵌入
 * ns_common 的持有引用，失败返回 NULL；无错误指针类别。task_lock 临界区内
 * 不能睡眠，本函数的读取和引用操作也都不睡眠。
 */
static struct ns_common *utsns_get(struct task_struct *task)
{
	/*
	 * ns 是最终要稳定的 UTS 对象；nsproxy 只在 task_lock 临界区内借用。
	 */
	struct uts_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	/*
	 * 锁住“读取 task->nsproxy、沿它找到 uts_ns、取得 uts_ns 引用”
	 * 这一整体。nsproxy 非 NULL 表示任务尚保留 namespace 集合；
	 * 其 UTS 槽位按不变量有效。
	 */
	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy) {
		ns = nsproxy->uts_ns;
		get_uts_ns(ns);
	}
	task_unlock(task);

	/*
	 * 返回嵌入成员地址不转移对象布局；持有关系仍由 UTS
	 * 普通引用表达。
	 */
	return ns ? &ns->ns : NULL;
}

/*
 * utsns_put() - 释放 utsns_get()/nsfs 为 UTS namespace 持有的普通引用。
 *
 * @ns 是 struct uts_namespace 中嵌入的 ns_common，必须来自 UTS 操作表且不可
 * 为空；to_uts_ns() 用 container_of 恢复外层对象。返回：无直接返回值。
 * 最后一份引用可能触发 free_uts_ns()，所以调用后不得再解引用 @ns；本函数
 * 不负责 active 引用，也不需要 uts_sem，当前释放链不睡眠。
 */
static void utsns_put(struct ns_common *ns)
{
	put_uts_ns(to_uts_ns(ns));
}

/*
 * utsns_install() - 在 setns() 的准备阶段把目标 UTS 对象装入临时 nsproxy。
 *
 * 调用关系：kernel/nsproxy.c 的 validate_nsset() 经 operations.install 调用。
 * 此时 nsset->nsproxy 是 prepare_nsset() 创建、尚未发布给 current 的私有副本；
 * 所有 namespace 的校验都成功后，commit_nsset() 才一次性切换 current。因此
 * 本函数失败不会让任务观察到一半新、一半旧的 namespace 集合。
 *
 * @nsset: setns 事务的临时集合，借用且不可为空；nsproxy 持有旧 uts_ns 引用，
 *         cred 表示本次事务将使用的当前或待提交凭据。
 * @new:   目标 UTS 对象中嵌入的 ns_common，借用且不可为空；调用者在
 *         整个 install 回调期间保证其存活，本函数成功时再为临时
 *         nsproxy 取得引用。
 *
 * 权限要求同时覆盖两个边界：调用者须在目标 UTS 的 owning user namespace
 * 中拥有 CAP_SYS_ADMIN，也须在 nsset 凭据所属 user namespace 中拥有该能力；
 * 后者在同一 setns 事务还切换 user namespace 时尤其重要。成功返回 0，并把
 * 临时 nsproxy 的引用从旧对象转移到 @new；权限不足返回 -EPERM，引用与槽位
 * 均保持不变。函数不取得 active 引用，真正提交 nsproxy 时外层统一处理；
 * 本回调不分配内存、不获取 uts_sem，也不睡眠。
 */
static int utsns_install(struct nsset *nsset, struct ns_common *new)
{
	/*
	 * nsproxy 是未发布事务副本；ns 是从通用接口恢复出的目标 UTS 对象。
	 */
	struct nsproxy *nsproxy = nsset->nsproxy;
	struct uts_namespace *ns = to_uts_ns(new);

	/*
	 * 两项能力检查都必须先完成，避免失败路径需要回滚任何
	 * 引用或指针修改。
	 */
	if (!ns_capable(ns->user_ns, CAP_SYS_ADMIN) ||
	    !ns_capable(nsset->cred->user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	/*
	 * 先增新引用、再减旧引用，最后发布到私有槽位。若目标恰好就是
	 * 原对象，这一顺序仍能保证中间引用计数不归零；反过来会有
	 * 提前释放风险。
	 */
	get_uts_ns(ns);
	put_uts_ns(nsproxy->uts_ns);
	nsproxy->uts_ns = ns;
	return 0;
}

/*
 * utsns_owner() - 返回 UTS namespace 的能力与层级归属 user namespace。
 *
 * @ns 是仍由调用者持有的 UTS ns_common；返回值是借用的 user_ns 指针，
 * 本函数不增加引用。UTS 对象自身持有 user_ns 引用，所以只要 @ns 的普通
 * 引用有效，返回对象便保持存活。该回调供 namespace 树、NS_GET_USERNS
 * 等通用代码建立 owner 关系；无失败返回，也不读取 uts name、获取
 * uts_sem 或睡眠。
 */
static struct user_namespace *utsns_owner(struct ns_common *ns)
{
	return to_uts_ns(ns)->user_ns;
}

/*
 * UTS namespace 接入 procfs/nsfs/setns 的静态操作表。表本身贯穿内核生命期：
 * .name 决定 /proc/<pid>/ns/uts 的类型名；.get/.put 把任务中的借用指针转换为
 * 成对普通引用；.install 参与 setns 的校验事务；.owner 暴露 owning user_ns。
 * 未提供 .get_parent，因为 UTS namespace 之间没有由本接口公开的父子层级。
 */
const struct proc_ns_operations utsns_operations = {
	.name		= "uts",
	.get		= utsns_get,
	.put		= utsns_put,
	.install	= utsns_install,
	.owner		= utsns_owner,
};

/*
 * uts_ns_init() - 启动期创建 UTS namespace slab cache 并登记初始对象。
 *
 * 入参：无。调用位置：namespace/VFS 初始化阶段，仅执行一次；__init 使
 * 函数正文在启动完成后可回收。此时尚不存在运行期并发创建者，
 * 可以睡眠，也不持 uts_sem。返回：无直接返回值；SLAB_PANIC 把
 * cache 创建失败提升为启动 panic，因而成功返回时 uts_ns_cache 必然可用，
 * 且 init_uts_ns 已进入 namespace 树。
 *
 * init_uts_ns 的 name、固定 ns_common 引用/active 引用和 owning init_user_ns
 * 已由 init/version-timestamp.c 静态初始化。它永久存活，不走动态 ucounts
 * 计费或 free_uts_ns()；这里仅补齐动态分配设施和全局可发现性。
 */
void __init uts_ns_init(void)
{
	/*
	 * 为动态 UTS 对象建立等于结构体大小、无额外对齐要求的 cache。
	 * SLAB_ACCOUNT 允许内存 cgroup 记账；SLAB_PANIC 表示失败不返回 NULL。
	 * hardened usercopy 只允许整个 name 字段所在区间参与 slab 对象与用户态
	 * 之间的复制，结构中的 user_ns、ucounts、ns_common 指针和元数据不在
	 * 白名单内，从而把越界复制限制在 ABI 字符串快照之外。
	 */
	uts_ns_cache = kmem_cache_create_usercopy(
			"uts_namespace", sizeof(struct uts_namespace), 0,
			SLAB_PANIC|SLAB_ACCOUNT,
			offsetof(struct uts_namespace, name),
			sizeof_field(struct uts_namespace, name),
			NULL);
	/*
	 * 动态对象由 clone_uts_ns() 在构造完成后逐个加入；初始对象不会
	 * 经过该路径，因此在这里为其固定 ID 建立按类型、统一和 owner
	 * 树索引。
	 */
	ns_tree_add(&init_uts_ns);
}
