// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025 Christian Brauner <brauner@kernel.org> */
/*
 * 通用 namespace 元数据与两级引用协议
 *
 * 各具体 namespace 把 struct ns_common 嵌入自身对象。本文件负责初始化
 * inode/类型/操作表/树节点，释放 inode 号，并维护 active 引用沿 owning
 * user namespace 的级联。普通 __ns_ref 决定对象内存寿命；active 引用决定
 * 对象是否被任务、fd 或 bind mount 活跃使用及其树可见语义。UTS namespace
 * 通过 ns_common_init()/free 和 nsproxy active get/put 接入同一协议。
 */

#include <linux/ns_common.h>
#include <linux/nstree.h>
#include <linux/proc_ns.h>
#include <linux/user_namespace.h>
#include <linux/vfsdebug.h>

#ifdef CONFIG_DEBUG_VFS
/*
 * ns_debug() - 校验 namespace 类型与操作表是否匹配。
 *
 * @ns 是正在初始化、尚未发布的借用对象；@ops 是将写入 ns->ops 的静态
 * 操作表，可在特定未构建配置下为空。函数仅在 CONFIG_DEBUG_VFS 下存在，
 * 按 ns_type 检查对应 operations 地址，发现内部类型混配时发出一次性警告。
 * 返回：无直接返回值，不取得引用、不睡眠，也不修复错误对象。
 */
static void ns_debug(struct ns_common *ns, const struct proc_ns_operations *ops)
{
	switch (ns->ns_type) {
#ifdef CONFIG_CGROUPS
	case CLONE_NEWCGROUP:
		VFS_WARN_ON_ONCE(ops != &cgroupns_operations);
		break;
#endif
#ifdef CONFIG_IPC_NS
	case CLONE_NEWIPC:
		VFS_WARN_ON_ONCE(ops != &ipcns_operations);
		break;
#endif
	case CLONE_NEWNS:
		VFS_WARN_ON_ONCE(ops != &mntns_operations);
		break;
#ifdef CONFIG_NET_NS
	case CLONE_NEWNET:
		VFS_WARN_ON_ONCE(ops != &netns_operations);
		break;
#endif
#ifdef CONFIG_PID_NS
	case CLONE_NEWPID:
		VFS_WARN_ON_ONCE(ops != &pidns_operations);
		break;
#endif
#ifdef CONFIG_TIME_NS
	case CLONE_NEWTIME:
		VFS_WARN_ON_ONCE(ops != &timens_operations);
		break;
#endif
#ifdef CONFIG_USER_NS
	case CLONE_NEWUSER:
		VFS_WARN_ON_ONCE(ops != &userns_operations);
		break;
#endif
#ifdef CONFIG_UTS_NS
	case CLONE_NEWUTS:
		VFS_WARN_ON_ONCE(ops != &utsns_operations);
		break;
#endif
	}
}
#endif

/*
 * __ns_common_init() - 初始化具体 namespace 中嵌入的通用元数据。
 *
 * @ns:      尚未发布的借用输出对象，不可为空；调用者拥有外层存储。
 * @ns_type: CLONE_NEW* 类型位，决定 proc/nsfs 与 setns 的类型身份。
 * @ops:     该类型的静态操作表；不增加引用，必须覆盖对象整个生命周期。
 * @inum:    非零时使用调用者提供的固定 inode，零时动态分配 proc inode。
 *
 * 函数不初始化具体 namespace 字段，也不把对象加入 namespace 树。它建立
 * 普通引用 1、空 stashed dentry、未分配 ID、全部树节点和 owner 子树；
 * 动态 inode 分配可能失败。成功返回 0，调用者随后完成具体字段并
 * ns_tree_add；失败返回 proc_alloc_inum() 的负 errno，普通内存仍归
 * 调用者直接回收。
 */
int __ns_common_init(struct ns_common *ns, u32 ns_type, const struct proc_ns_operations *ops, int inum)
{
	/* ret 只承载动态 inode 分配错误，固定 inode 路径保持 0。 */
	int ret = 0;

	/*
	 * 普通引用先建立为 1；其余指针、ID 和树节点仍处于未发布状态。
	 * 四个节点分别服务按类型树、统一树、owner 树和本对象
	 * 拥有的子树。
	 */
	refcount_set(&ns->__ns_ref, 1);
	ns->stashed = NULL;
	ns->ops = ops;
	ns->ns_id = 0;
	ns->ns_type = ns_type;
	ns_tree_node_init(&ns->ns_tree_node);
	ns_tree_node_init(&ns->ns_unified_node);
	ns_tree_node_init(&ns->ns_owner_node);
	ns_tree_root_init(&ns->ns_owner_root);

#ifdef CONFIG_DEBUG_VFS
	ns_debug(ns, ops);
#endif

	/* 初始 namespace 传固定 inode；动态对象从 proc inode 分配器取得编号。 */
	if (inum)
		ns->inum = inum;
	else
		ret = proc_alloc_inum(&ns->inum);
	if (ret)
		return ret;
	/*
	 * Tree ref starts at 0. It's incremented when namespace enters
	 * active use (installed in nsproxy) and decremented when all
	 * active uses are gone. Initial namespaces are always active.
	 */
	/*
	 * 树的 active 引用从 0 开始；namespace 安装进 nsproxy 等活跃使用时
	 * 增加，全部 active 使用消失时递减。初始 namespace 永久 active，
	 * 因而从 1 开始。这里的 active 计数不替代上面的普通存储期引用。
	 */
	if (is_ns_init_inum(ns))
		atomic_set(&ns->__ns_ref_active, 1);
	else
		atomic_set(&ns->__ns_ref_active, 0);
	return 0;
}

/*
 * __ns_common_free() - 归还 ns_common 初始化阶段取得的 proc inode 号。
 *
 * @ns 必须已从 namespace 树摘除且即将结束存储期；调用者仍持有外层对象
 * 的独占回收权。函数无直接返回值，不释放外层内存、不归还具体类型
 * 资源，也不等待 RCU；这些步骤由 free_uts_ns() 等类型析构函数负责。
 */
void __ns_common_free(struct ns_common *ns)
{
	proc_free_inum(ns->inum);
}

/*
 * ns_owner() - 取得 namespace 在 active 层级中的 owning user namespace。
 *
 * @ns 是普通引用仍有效的借用对象，不可为空。返回其 owner 中嵌入的
 * ns_common 借用指针；无操作表、没有 owner 或 owner 是永久 active 的
 * init_user_ns 时返回 NULL，表示级联遍历到此停止。函数不增加 owner 引用；
 * 调用者只能在 active 引用协议保证层级存活的范围内使用结果。
 */
struct ns_common *__must_check ns_owner(struct ns_common *ns)
{
	struct user_namespace *owner;

	if (unlikely(!ns->ops))
		return NULL;
	VFS_WARN_ON_ONCE(!ns->ops->owner);
	owner = ns->ops->owner(ns);
	VFS_WARN_ON_ONCE(!owner && ns != to_ns_common(&init_user_ns));
	if (!owner)
		return NULL;
	/* Skip init_user_ns as it's always active */
	/* init_user_ns 永久 active，无需继续增加或减少其 active 计数。 */
	if (owner == &init_user_ns)
		return NULL;
	return to_ns_common(owner);
}

/*
 * The active reference count works by having each namespace that gets
 * created take a single active reference on its owning user namespace.
 * That single reference is only released once the child namespace's
 * active count itself goes down.
 *
 * A regular namespace tree might look as follow:
 * Legend:
 * + : adding active reference
 * - : dropping active reference
 * x : always active (initial namespace)
 *
 *
 *                 net_ns          pid_ns
 *                       \        /
 *                        +      +
 *                        user_ns1 (2)
 *                            |
 *                 ipc_ns     |     uts_ns
 *                       \    |    /
 *                        +   +   +
 *                        user_ns2 (3)
 *                            |
 *            cgroup_ns       |       mnt_ns
 *                     \      |      /
 *                      x     x     x
 *                      init_user_ns (1)
 *
 * If both net_ns and pid_ns put their last active reference on
 * themselves it will cascade to user_ns1 dropping its own active
 * reference and dropping one active reference on user_ns2:
 *
 *                 net_ns          pid_ns
 *                       \        /
 *                        -      -
 *                        user_ns1 (0)
 *                            |
 *                 ipc_ns     |     uts_ns
 *                       \    |    /
 *                        +   -   +
 *                        user_ns2 (2)
 *                            |
 *            cgroup_ns       |       mnt_ns
 *                     \      |      /
 *                      x     x     x
 *                      init_user_ns (1)
 *
 * The iteration stops once we reach a namespace that still has active
 * references.
 */
/*
 * active 引用的层级规则是：每个进入 active 状态的 namespace，都为自己的
 * owning user namespace 贡献一份 active 引用；只有子 namespace 自身的
 * active 计数归零时，才释放这份 owner 引用。
 *
 * 上图中“+”表示增加 active 引用，“-”表示减少，“x”表示初始 namespace
 * 永久 active。net_ns 与 pid_ns 都把引用贡献给 user_ns1；user_ns1 首次
 * active 时再向 user_ns2 贡献一份，而 ipc_ns 与 uts_ns 也分别贡献一份，
 * 所以 user_ns2 的计数为 3。若 net_ns、pid_ns 依次释放最后 active 引用，
 * user_ns1 最终从 1 降为 0，继而级联释放它对 user_ns2 的一份引用；遍历在
 * 遇到仍有其他 active 使用的 namespace 时停止。
 *
 * __ns_ref_active_put() - 释放一个 namespace active 使用并向 owner 级联。
 *
 * @ns 是普通引用仍有效的借用对象，不可为空。初始 namespace 无操作；动态
 * 对象先原子递减自身 active 计数，若仍非零立即返回。若从 1 降为 0，则沿
 * ns_owner() 逐层递减 owning user namespace，直到永久初始 owner、无 owner，
 * 或某层仍有其他 active 引用。函数无直接返回值、不释放普通引用且
 * 不睡眠；计数为负或 active 归零时普通引用已无效都属于内部协议错误，
 * 并触发警告。
 */
void __ns_ref_active_put(struct ns_common *ns)
{
	/* Initial namespaces are always active. */
	/* 初始 namespace 永久 active，既不递减自身，也不向 owner 级联。 */
	if (is_ns_init_id(ns))
		return;

	/* 非最后一个 active 使用只递减本层；负值表示发生了未配对 put。 */
	if (!atomic_dec_and_test(&ns->__ns_ref_active)) {
		VFS_WARN_ON_ONCE(__ns_ref_active_read(ns) < 0);
		return;
	}

	VFS_WARN_ON_ONCE(is_ns_init_id(ns));
	VFS_WARN_ON_ONCE(!__ns_ref_read(ns));

	/*
	 * 本层刚变为 inactive，开始归还它对 owner 的唯一 active 贡献。
	 * ns_owner() 返回 NULL 表示已到无需计数的 init_user_ns 或无 owner 类型。
	 */
	for (;;) {
		ns = ns_owner(ns);
		if (!ns)
			return;
		VFS_WARN_ON_ONCE(is_ns_init_id(ns));
		if (!atomic_dec_and_test(&ns->__ns_ref_active)) {
			VFS_WARN_ON_ONCE(__ns_ref_active_read(ns) < 0);
			return;
		}
	}
}

/*
 * The active reference count works by having each namespace that gets
 * created take a single active reference on its owning user namespace.
 * That single reference is only released once the child namespace's
 * active count itself goes down. This makes it possible to efficiently
 * resurrect a namespace tree:
 *
 * A regular namespace tree might look as follow:
 * Legend:
 * + : adding active reference
 * - : dropping active reference
 * x : always active (initial namespace)
 *
 *
 *                 net_ns          pid_ns
 *                       \        /
 *                        +      +
 *                        user_ns1 (2)
 *                            |
 *                 ipc_ns     |     uts_ns
 *                       \    |    /
 *                        +   +   +
 *                        user_ns2 (3)
 *                            |
 *            cgroup_ns       |       mnt_ns
 *                     \      |      /
 *                      x     x     x
 *                      init_user_ns (1)
 *
 * If both net_ns and pid_ns put their last active reference on
 * themselves it will cascade to user_ns1 dropping its own active
 * reference and dropping one active reference on user_ns2:
 *
 *                 net_ns          pid_ns
 *                       \        /
 *                        -      -
 *                        user_ns1 (0)
 *                            |
 *                 ipc_ns     |     uts_ns
 *                       \    |    /
 *                        +   -   +
 *                        user_ns2 (2)
 *                            |
 *            cgroup_ns       |       mnt_ns
 *                     \      |      /
 *                      x     x     x
 *                      init_user_ns (1)
 *
 * Assume the whole tree is dead but all namespaces are still active:
 *
 *                 net_ns          pid_ns
 *                       \        /
 *                        -      -
 *                        user_ns1 (0)
 *                            |
 *                 ipc_ns     |     uts_ns
 *                       \    |    /
 *                        -   -   -
 *                        user_ns2 (0)
 *                            |
 *            cgroup_ns       |       mnt_ns
 *                     \      |      /
 *                      x     x     x
 *                      init_user_ns (1)
 *
 * Now assume the net_ns gets resurrected (.e.g., via the SIOCGSKNS ioctl()):
 *
 *                 net_ns          pid_ns
 *                       \        /
 *                        +      -
 *                        user_ns1 (0)
 *                            |
 *                 ipc_ns     |     uts_ns
 *                       \    |    /
 *                        -   +   -
 *                        user_ns2 (0)
 *                            |
 *            cgroup_ns       |       mnt_ns
 *                     \      |      /
 *                      x     x     x
 *                      init_user_ns (1)
 *
 * If net_ns had a zero reference count and we bumped it we also need to
 * take another reference on its owning user namespace. Similarly, if
 * pid_ns had a zero reference count it also needs to take another
 * reference on its owning user namespace. So both net_ns and pid_ns
 * will each have their own reference on the owning user namespace.
 *
 * If the owning user namespace user_ns1 had a zero reference count then
 * it also needs to take another reference on its owning user namespace
 * and so on.
 */
/*
 * 与 active put 相同，每个 active namespace 为 owning user namespace 保留
 * 一份 active 引用；该引用直到子对象 active 归零才释放。这个层级规则还
 * 允许高效复活整棵 inactive namespace 树。
 *
 * 图中“+”“-”“x”分别表示增加、减少和永久 active。正常树里 net_ns 与
 * pid_ns 使 user_ns1 active，user_ns1、ipc_ns、uts_ns 共同使 user_ns2
 * active。子对象退出可把 user_ns1 及其对 user_ns2 的贡献级联降为零；整棵
 * 动态树即使 inactive，普通引用仍可能让对象内存存在。若随后 SIOCGSKNS
 * 等路径复活 net_ns，从 0 增至 1 的 net_ns 必须重新激活 user_ns1；若
 * user_ns1 也从 0 复活，还要继续激活 user_ns2，直到遇到原本已 active 的
 * owner。pid_ns 日后独立复活时也会持有自己对 owner 的那一份贡献。
 *
 * __ns_ref_active_get() - 增加 active 使用，必要时向 owner 层级级联复活。
 *
 * @ns 是普通引用仍有效的借用对象，不可为空。初始 namespace 无操作；动态
 * 对象用 atomic_fetch_add() 返回增加前的计数。旧值非零表示对象本来 active，
 * 无需改变 owner；旧值为零表示发生 inactive -> active 转换，必须沿
 * ns_owner() 对 owner 执行相同过程。函数无返回值、不增加普通引用且
 * 不睡眠；负旧值属于计数协议错误。
 */
void __ns_ref_active_get(struct ns_common *ns)
{
	int prev;

	/* Initial namespaces are always active. */
	/* 初始对象由固定 active 引用钉住，不参与动态计数或 owner 级联。 */
	if (is_ns_init_id(ns))
		return;

	/* If we didn't resurrect the namespace we're done. */
	/* 旧值非零说明只是新增并列使用者，没有 0 -> 1 的复活边界。 */
	prev = atomic_fetch_add(1, &ns->__ns_ref_active);
	VFS_WARN_ON_ONCE(prev < 0);
	if (likely(prev))
		return;

	/*
	 * We did resurrect it. Walk the ownership hierarchy upwards
	 * until we found an owning user namespace that is active.
	 */
	/*
	 * 当前层确实从 inactive 复活；逐级为 owner 补回 active 贡献，直到
	 * 找到原本已 active 的 owner 或永久初始层级。
	 */
	for (;;) {
		ns = ns_owner(ns);
		if (!ns)
			return;

		VFS_WARN_ON_ONCE(is_ns_init_id(ns));
		prev = atomic_fetch_add(1, &ns->__ns_ref_active);
		VFS_WARN_ON_ONCE(prev < 0);
		if (likely(prev))
			return;
	}
}

/*
 * may_see_all_namespaces() - 判断 current 是否拥有全局 namespace 可见权限。
 *
 * 入参：无。只有 current 位于初始 PID namespace，并在其 owning user
 * namespace 中拥有 CAP_SYS_ADMIN 时返回 true；否则返回 false。能力检查
 * 使用 noaudit 变体，适合列表过滤等探测路径，不产生审计记录。
 * 函数不取得 namespace 引用；current 的活动 PID namespace 在调用期间
 * 可直接借用。
 */
bool may_see_all_namespaces(void)
{
	return (task_active_pid_ns(current) == &init_pid_ns) &&
	       ns_capable_noaudit(init_pid_ns.user_ns, CAP_SYS_ADMIN);
}
