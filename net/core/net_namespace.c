// SPDX-License-Identifier: GPL-2.0-only
/*
 * 网络命名空间核心实现学习导读
 *
 * 中文学习注释模型：OpenAI Codex GPT-5。
 *
 * 本文件负责 struct net 的全局生命周期编排：为每个网络子系统登记
 * pernet_operations，在新命名空间上按注册顺序初始化，在销毁或模块卸载时
 * 按相反顺序撤销；同时维护命名空间全局表、相对 nsid、rtnetlink 管理接口
 * 以及 /proc namespace 操作。协议栈各自的路由表、设备和 socket 状态并不在
 * 这里实现，而是由已注册的 pernet 回调创建和销毁。
 *
 * 创建主线：copy_net_ns() -> net_alloc() -> preinit_net() -> setup_net()
 *             -> ops_init() -> 发布到 net_namespace_list/ns_tree。
 * 销毁主线：put_net() 最后一个主动引用 -> __put_net() -> cleanup_net()
 *             -> 从索引摘除 -> ops_undo_list() -> RCU 回调排空
 *             -> net_passive_dec() -> 延迟释放 struct net。
 * 注册主线：register_pernet_subsys/device() 在 pernet_ops_rwsem 写锁下，
 *             既把新 ops 插入构造序列，也为所有既存 net 补做初始化。
 *
 * 核心所有权分成两层：net->ns.count 是“主动使用”引用，归零后命名空间进入
 * cleanup；net->passive 则让仍可能触及 struct net 外壳的异步/被动使用者延长
 * 存储期。net->gen 和各子系统私有数据遵循 RCU 发布，销毁必须等待宽限期和
 * 已排队 RCU callback。这样读侧成本低，但代价是回收分阶段且模块卸载必须
 * 显式等待屏障。
 *
 * 并发地图：pernet_ops_rwsem 串行化 ops 集合变化，并阻止 setup/cleanup 与
 * 注册卸载交错；net_rwsem 保护全局 net 表的结构修改；每个 net->nsid_lock
 * 保护该观察者的 netns_ids IDR；RCU 允许全局表、generic 指针和 nsid 查询
 * 低成本并发读取。锁只保护状态，引用和 RCU 才分别保证长期/临界区内存活。
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/workqueue.h>
#include <linux/rtnetlink.h>
#include <linux/cache.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/idr.h>
#include <linux/rculist.h>
#include <linux/nsproxy.h>
#include <linux/fs.h>
#include <linux/proc_ns.h>
#include <linux/file.h>
#include <linux/export.h>
#include <linux/user_namespace.h>
#include <linux/net_namespace.h>
#include <linux/sched/task.h>
#include <linux/uidgid.h>
#include <linux/proc_fs.h>
#include <linux/nstree.h>

#include <net/aligned_data.h>
#include <net/sock.h>
#include <net/netlink.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>

/*
 *	Our network namespace constructor/destructor lists
 */
/*
 * 这里保存网络命名空间构造/析构回调链。pernet_list 按初始化顺序排列；
 * first_device 把“普通子系统”与依赖它们的“网络设备类回调”分隔开，确保构造
 * 时 subsystem 在前、device 在后，而逆序销毁时 device 先退出。
 */

static LIST_HEAD(pernet_list);
static struct list_head *first_device = &pernet_list;

/*
 * 所有已经完成 setup_net() 并对查找者可见的 struct net。写侧持 net_rwsem，
 * 遍历者按具体接口使用该锁或 RCU；表节点本身不赋予引用，跨出保护区前仍需
 * get_net()/maybe_get_net() 稳定对象。
 */
LIST_HEAD(net_namespace_list);
EXPORT_SYMBOL_GPL(net_namespace_list);

/* Protects net_namespace_list. Nests iside rtnl_lock() */
/*
 * 保护 net_namespace_list；锁序规定它嵌套在 rtnl_lock() 内侧。换序可能使
 * cleanup 与同时持 RTNL、再查全局 net 表的路径形成 ABBA 死锁。
 */
DECLARE_RWSEM(net_rwsem);
EXPORT_SYMBOL_GPL(net_rwsem);

#ifdef CONFIG_KEYS
/* init_net 使用静态 key 域，初始引用由永久存在的初始命名空间持有。 */
static struct key_tag init_net_key_domain = { .usage = REFCOUNT_INIT(1) };
#endif

/* 初始网络命名空间是静态对象，不经 net_cachep 分配，生命周期覆盖整个内核。 */
struct net init_net;
EXPORT_SYMBOL(init_net);

/* setup_net(&init_net) 完成后置 true，供不支持多 netns 的注册分支判断时序。 */
static bool init_net_initialized;
/*
 * pernet_ops_rwsem: protects: pernet_list, net_generic_ids,
 * init_net_initialized and first_device pointer.
 * This is internal net namespace object. Please, don't use it
 * outside.
 */
/*
 * pernet_ops_rwsem 保护 pernet_list、net_generic_ids、init_net_initialized
 * 和 first_device。创建/清理持读锁，可彼此按工作流并行；注册/卸载持写锁，
 * 因而看到稳定的全局 net 集合并能原子地把一个 ops 应用于全部既存 net。
 * 这是 netns 内部锁，外部子系统不得借它建立自己的锁约定。
 */
DECLARE_RWSEM(pernet_ops_rwsem);

/*
 * net_generic 前部的长度/RCU 元数据占据若干指针槽；动态 ID 必须从其后的首个
 * 完整槽开始，避免把子系统私有指针覆盖到结构头。
 */
#define MIN_PERNET_OPS_ID	\
	((sizeof(struct net_generic) + sizeof(void *) - 1) / sizeof(void *))

#define INITIAL_NET_GEN_PTRS	13 /* +1 for len +2 for rcu_head */
/*
 * 初始 generic 数组容量是 13 个指针槽：布局还包含长度元数据和供 kfree_rcu()
 * 使用的 rcu_head。max_gen_ptrs 只增不减，记录已分配过的最高 pernet ID 上界。
 */

static unsigned int max_gen_ptrs = INITIAL_NET_GEN_PTRS;

/*
 * net_alloc_generic() - 按当前全局容量分配清零的 per-net 私有指针表。
 *
 * 调用者处于可睡眠的进程上下文，返回新表的独占所有权；失败返回 NULL。
 * READ_ONCE 与注册路径的 WRITE_ONCE 配对，避免无锁读取 max_gen_ptrs 时发生
 * 编译器撕裂/合并。读到旧的较小容量仍是安全快照，net_assign_generic()
 * 会在真正写入某个 id 时检查容量并重新扩展。
 */
static struct net_generic *net_alloc_generic(void)
{
	/* gen_ptrs 是本次分配的槽位数；generic_size 是含柔性数组的总字节数。 */
	unsigned int gen_ptrs = READ_ONCE(max_gen_ptrs);
	unsigned int generic_size;
	struct net_generic *ng;

	/* offsetof 在不解引用对象的情况下算出 ptr[gen_ptrs] 末端偏移。 */
	generic_size = offsetof(struct net_generic, ptr[gen_ptrs]);

	ng = kzalloc(generic_size, GFP_KERNEL);
	if (ng)
		ng->s.len = gen_ptrs;

	return ng;
}

/*
 * net_assign_generic() - 把子系统私有数据安装到 net->gen[id]。
 *
 * @net：正在初始化或补注册子系统的命名空间，借用指针；调用者持有
 * pernet_ops_rwsem，保证 id 分配和该 net 的构造序列稳定。
 * @id：由 net_generic_ids 分配的槽号，必须避开结构头保留槽。
 * @data：由 ops_init() 新分配的私有对象；成功后由该 net/ops 生命周期管理。
 *
 * 容量足够时原位写入尚未使用且以后不再改变的槽；容量不足时复制旧表、
 * 发布新表并 RCU 延迟释放旧表。成功返回 0，分配新表失败返回 -ENOMEM，
 * 此时 data 仍归调用者回滚。函数可睡眠，因为扩容使用 GFP_KERNEL。
 */
static int net_assign_generic(struct net *net, unsigned int id, void *data)
{
	/* old_ng 是锁保护下的借用快照；ng 在发布前由本函数独占。 */
	struct net_generic *ng, *old_ng;

	BUG_ON(id < MIN_PERNET_OPS_ID);

	old_ng = rcu_dereference_protected(net->gen,
					   lockdep_is_held(&pernet_ops_rwsem));
	if (old_ng->s.len > id) {
		/* generic.h 约定槽只从 NULL 写为最终值，读者因此无需逐槽同步。 */
		old_ng->ptr[id] = data;
		return 0;
	}

	ng = net_alloc_generic();
	if (!ng)
		return -ENOMEM;

	/*
	 * Some synchronisation notes:
	 *
	 * The net_generic explores the net->gen array inside rcu
	 * read section. Besides once set the net->gen->ptr[x]
	 * pointer never changes (see rules in netns/generic.h).
	 *
	 * That said, we simply duplicate this array and schedule
	 * the old copy for kfree after a grace period.
	 */
	/*
	 * RCU 读者会在读侧临界区遍历 net->gen，且已设置的 ptr[x] 永不替换。
	 * 因此扩容不原地 resize：复制所有业务槽，补入新槽后一次发布新数组，
	 * 旧数组等宽限期后释放；任一读者要么看到完整旧表，要么看到完整新表。
	 */

	memcpy(&ng->ptr[MIN_PERNET_OPS_ID], &old_ng->ptr[MIN_PERNET_OPS_ID],
	       (old_ng->s.len - MIN_PERNET_OPS_ID) * sizeof(void *));
	ng->ptr[id] = data;

	/* release 发布保证复制和新槽写入先于并发 rcu_dereference() 可见。 */
	rcu_assign_pointer(net->gen, ng);
	kfree_rcu(old_ng, s.rcu);
	return 0;
}

/*
 * ops_init() - 在一个 net 上执行单个 pernet_operations 的初始化事务。
 *
 * @ops：已登记或正在登记的回调描述，借用且在 pernet_ops_rwsem 下稳定。
 * @net：目标命名空间，借用；尚未对外发布，或正在写锁下为既存 net 补注册。
 *
 * 若 ops 声明 id/size，先分配清零私有区并发布到 generic 槽，再调用 init，
 * 使 init 回调能通过 net_generic() 取得自己的数据。成功返回 0，私有区所有权
 * 交给 ops 的退出流程；失败返回 -ENOMEM 或 init errno，并清空已发布槽后释放
 * 私有区。回调和 GFP_KERNEL 分配均可能睡眠。
 */
static int ops_init(const struct pernet_operations *ops, struct net *net)
{
	/* data 仅在本次事务成功后转交；err 驱动两级回滚出口。 */
	struct net_generic *ng;
	int err = -ENOMEM;
	void *data = NULL;

	if (ops->id) {
		/* id 与 size 必须成对，register_pernet_operations() 已验证该契约。 */
		data = kzalloc(ops->size, GFP_KERNEL);
		if (!data)
			goto out;

		err = net_assign_generic(net, *ops->id, data);
		if (err)
			goto cleanup;
	}
	err = 0;
	if (ops->init)
		/* generic 槽已经可用，子系统 init 可据此构造其余 per-net 状态。 */
		err = ops->init(net);
	if (!err)
		return 0;

	if (ops->id) {
		/* init 失败时先撤销可发现性，再释放 data，避免留下悬空槽。 */
		ng = rcu_dereference_protected(net->gen,
					       lockdep_is_held(&pernet_ops_rwsem));
		ng->ptr[*ops->id] = NULL;
	}

cleanup:
	/* 到此 data 尚未转移；NULL 也可安全交给 kfree。 */
	kfree(data);

out:
	return err;
}

/*
 * ops_pre_exit_list() - 对待销毁 net 批次运行一个 ops 的 pre_exit 阶段。
 * @ops 与 @net_exit_list 都是调用期间借用；列表中的 net 尚未释放。
 * pre_exit 用于先阻止新活动/摘除读侧入口，不返回错误，允许睡眠与否由回调
 * 契约决定。无回调时为空操作。
 */
static void ops_pre_exit_list(const struct pernet_operations *ops,
			      struct list_head *net_exit_list)
{
	struct net *net;

	if (ops->pre_exit) {
		list_for_each_entry(net, net_exit_list, exit_list)
			ops->pre_exit(net);
	}
}

/*
 * ops_exit_rtnl_list() - 在 RTNL 和每-net RTNL 锁协议下逆序执行 exit_rtnl。
 *
 * @ops_list：稳定的完整回调链；@ops：逆序退出起点；@net_exit_list：销毁批次。
 * 每个回调把待注销设备链接到共享 dev_kill_list，全部 net 扫描后再批量注销，
 * 避免在回调中重复获取 RTNL。返回：无；设备注销是可观察副作用。
 */
static void ops_exit_rtnl_list(const struct list_head *ops_list,
			       const struct pernet_operations *ops,
			       struct list_head *net_exit_list)
{
	/* saved_ops 固定每个 net 的相同逆序起点；dev_kill_list 汇总设备所有权。 */
	const struct pernet_operations *saved_ops = ops;
	LIST_HEAD(dev_kill_list);
	struct net *net;

	/* RTNL 外层串行化网络设备拓扑，__rtnl_net_lock 再保护目标 net 的局部状态。 */
	rtnl_lock();

	list_for_each_entry(net, net_exit_list, exit_list) {
		__rtnl_net_lock(net);

		ops = saved_ops;
		list_for_each_entry_continue_reverse(ops, ops_list, list) {
			if (ops->exit_rtnl)
				ops->exit_rtnl(net, &dev_kill_list);
		}

		__rtnl_net_unlock(net);
	}

	/* 回调只收集设备；真正 unregister 在所有 per-net 锁释放后统一完成。 */
	unregister_netdevice_many(&dev_kill_list);

	rtnl_unlock();
}

/*
 * ops_exit_list() - 对销毁批次执行普通 exit 或 batch exit。
 * @ops/@net_exit_list 均为借用；回调不能失败，完成后该子系统不得再使用对应
 * per-net 状态。逐 net 路径用 cond_resched() 防止大批销毁长期独占 CPU；
 * exit_batch 则让子系统一次观察整个批次。
 */
static void ops_exit_list(const struct pernet_operations *ops,
			  struct list_head *net_exit_list)
{
	if (ops->exit) {
		struct net *net;

		list_for_each_entry(net, net_exit_list, exit_list) {
			ops->exit(net);
			cond_resched();
		}
	}

	if (ops->exit_batch)
		ops->exit_batch(net_exit_list);
}

/*
 * ops_free_list() - 释放 ops_init() 为每个 net 分配的 generic 私有区。
 * 这里只释放槽指向的数据，不改槽值；net 已从查找入口摘除且退出回调、RCU
 * 宽限协议保证不再有合法读者。无 id 的 ops 没有框架代管内存。
 */
static void ops_free_list(const struct pernet_operations *ops,
			  struct list_head *net_exit_list)
{
	struct net *net;

	if (ops->id) {
		list_for_each_entry(net, net_exit_list, exit_list)
			kfree(net_generic(net, *ops->id));
	}
}

/*
 * ops_undo_list() - 按注册逆序对一批 net 完成 pernet 撤销协议。
 *
 * @ops_list：回调链；@ops：最后一个“不应保留”的位置，NULL 表示撤销整条链；
 * @net_exit_list：待处理 net；@expedite_rcu：销毁主线是否用加速宽限期。
 *
 * 阶段顺序固定为 pre_exit -> RCU 宽限期 -> exit_rtnl -> exit -> 私有区释放。
 * 这使先摘除入口的回调与既有 RCU 读者分离，再按依赖关系逆序拆解资源。
 * 返回：无；所有指定 ops 都已退出且其框架私有内存已释放。
 */
static void ops_undo_list(const struct list_head *ops_list,
			  const struct pernet_operations *ops,
			  struct list_head *net_exit_list,
			  bool expedite_rcu)
{
	/* saved_ops 保存逆序遍历的起点；hold_rtnl 避免无相关回调时获取全局重锁。 */
	const struct pernet_operations *saved_ops;
	bool hold_rtnl = false;

	if (!ops)
		ops = list_entry(ops_list, typeof(*ops), list);

	saved_ops = ops;

	list_for_each_entry_continue_reverse(ops, ops_list, list) {
		/* 第一遍只停止新入口，并预先判断后续是否存在 RTNL 退出阶段。 */
		hold_rtnl |= !!ops->exit_rtnl;
		ops_pre_exit_list(ops, net_exit_list);
	}

	/* Another CPU might be rcu-iterating the list, wait for it.
	 * This needs to be before calling the exit() notifiers, so the
	 * rcu_barrier() after ops_undo_list() isn't sufficient alone.
	 * Also the pre_exit() and exit() methods need this barrier.
	 */
	/*
	 * 另一 CPU 可能仍在 RCU 临界区遍历刚由 pre_exit 摘除的对象；必须先等它
	 * 离开，exit 才能拆资源。函数返回后的 rcu_barrier() 只等待 callback，
	 * 不能替代这里等待当前读者的 grace period，且 pre_exit/exit 都依赖它。
	 */
	if (expedite_rcu)
		synchronize_rcu_expedited();
	else
		synchronize_rcu();

	if (hold_rtnl)
		ops_exit_rtnl_list(ops_list, saved_ops, net_exit_list);

	/* 后三遍分别处理 RTNL 资源、普通/批量退出和框架私有内存，保持依赖逆序。 */
	ops = saved_ops;
	list_for_each_entry_continue_reverse(ops, ops_list, list)
		ops_exit_list(ops, net_exit_list);

	ops = saved_ops;
	list_for_each_entry_continue_reverse(ops, ops_list, list)
		ops_free_list(ops, net_exit_list);
}

/*
 * ops_undo_single() - 借助临时单元素链复用批量撤销协议。
 * @ops 在调用前属于正式链或刚被摘除；本函数临时重用其 list 节点，撤销完成
 * 后恢复为未链接状态。@net_exit_list 中只含曾成功初始化该 ops 的 net。
 */
static void ops_undo_single(struct pernet_operations *ops,
			    struct list_head *net_exit_list)
{
	LIST_HEAD(ops_list);

	list_add(&ops->list, &ops_list);
	ops_undo_list(&ops_list, NULL, net_exit_list, false);
	list_del(&ops->list);
}

/* should be called with nsid_lock held */
/*
 * 必须持观察者 @net 的 nsid_lock 调用，防止并发分配/删除同一个 IDR 槽。
 * @peer 是作为 IDR 值保存的借用指针；@reqid >= 0 要求精确槽位，否则从 0
 * 开始自动分配。GFP_ATOMIC 保证持自旋锁时不睡眠。成功返回非负 nsid，失败
 * 返回 IDR errno，映射所有权归 @net。
 */
static int alloc_netid(struct net *net, struct net *peer, int reqid)
{
	int min = 0, max = 0;

	if (reqid >= 0) {
		min = reqid;
		max = reqid + 1;
	}

	return idr_alloc(&net->netns_ids, peer, min, max, GFP_ATOMIC);
}

/* This function is used by idr_for_each(). If net is equal to peer, the
 * function returns the id so that idr_for_each() stops. Because we cannot
 * returns the id 0 (idr_for_each() will not stop), we return the magic value
 * NET_ID_ZERO (-1) for it.
 */
/*
 * 这是 idr_for_each() 的匹配回调：当条目值与目标 peer 相同便返回其 id，令
 * 遍历停止。由于回调返回 0 表示“继续”，真实 id 0 必须编码成 NET_ID_ZERO
 * (-1)，由调用者再还原；未匹配保持返回 0。
 */
#define NET_ID_ZERO -1
/* net_eq_idr() 的三个参数分别是当前槽号、槽值 net 和查找目标 peer，均为借用。 */
static int net_eq_idr(int id, void *net, void *peer)
{
	if (net_eq(net, peer))
		return id ? : NET_ID_ZERO;
	return 0;
}

/* Must be called from RCU-critical section or with nsid_lock held */
/*
 * __peernet2id() - 在 @net 的相对 IDR 中查找 @peer。
 * 调用者必须持 RCU 读锁或 nsid_lock：前者保证条目中的 peer 存储期，后者还
 * 串行化更新。成功返回包括 0 在内的 nsid；未分配返回
 * NETNSA_NSID_NOT_ASSIGNED。参数均为借用，本函数不取得引用且不睡眠。
 */
static int __peernet2id(const struct net *net, struct net *peer)
{
	int id = idr_for_each(&net->netns_ids, net_eq_idr, peer);

	/* Magic value for id 0. */
	/* idr_for_each 不能以 0 表示命中，因此把内部哨兵恢复为对外合法的 nsid 0。 */
	if (id == NET_ID_ZERO)
		return 0;
	if (id > 0)
		return id;

	return NETNSA_NSID_NOT_ASSIGNED;
}

/* nsid 更新路径先用此前置声明调用通知器，完整参数/所有权契约见后方定义。 */
static void rtnl_net_notifyid(struct net *net, int cmd, int id, u32 portid,
			      struct nlmsghdr *nlh, gfp_t gfp);
/* This function returns the id of a peer netns. If no id is assigned, one will
 * be allocated and returned.
 */
/*
 * peernet2id_alloc() - 查询或为 @net 观察到的 @peer 分配相对 nsid。
 *
 * @net 是观察者、@peer 是被命名者，均为借用；@gfp 控制通知 skb 的分配上下文。
 * 先在 nsid_lock 下查找，缺失时用 maybe_get_net() 跨越 peer 与 cleanup_net()
 * 的竞态，再在锁内插入。成功返回非负 id；观察者/peer 已死亡或分配失败统一
 * 返回 NETNSA_NSID_NOT_ASSIGNED。新映射成功后向 @net 的 RTNLGRP_NSID 发通知。
 */
int peernet2id_alloc(struct net *net, struct net *peer, gfp_t gfp)
{
	int id;

	/* check_net 拒绝已进入清理的观察者，避免向将销毁的 IDR 增加映射。 */
	if (!check_net(net))
		return NETNSA_NSID_NOT_ASSIGNED;

	spin_lock(&net->nsid_lock);
	id = __peernet2id(net, peer);
	if (id >= 0) {
		spin_unlock(&net->nsid_lock);
		return id;
	}

	/* When peer is obtained from RCU lists, we may race with
	 * its cleanup. Check whether it's alive, and this guarantees
	 * we never hash a peer back to net->netns_ids, after it has
	 * just been idr_remove()'d from there in cleanup_net().
	 */
	/*
	 * peer 可能只是从 RCU 表借来的并正被 cleanup 摘除。maybe_get_net() 只有
	 * 在主动引用尚未归零时成功；取得临时引用后，cleanup 不可能先 remove 又
	 * 被这里重新插回，从而禁止“复活”将死命名空间。
	 */
	if (!maybe_get_net(peer)) {
		spin_unlock(&net->nsid_lock);
		return NETNSA_NSID_NOT_ASSIGNED;
	}

	id = alloc_netid(net, peer, -1);
	spin_unlock(&net->nsid_lock);

	/* IDR 已持有裸指针映射；释放临时主动引用，映射存续由清理协议协调。 */
	put_net(peer);
	if (id < 0)
		return NETNSA_NSID_NOT_ASSIGNED;

	rtnl_net_notifyid(net, RTM_NEWNSID, id, 0, NULL, gfp);

	return id;
}
EXPORT_SYMBOL_GPL(peernet2id_alloc);

/* This function returns, if assigned, the id of a peer netns. */
/*
 * peernet2id() - 只读查询 @peer 在观察者 @net 中已有的相对 nsid。
 * 两个参数均为借用；RCU 读锁保护 IDR 条目和 peer 存储期，仅返回整数，不取得
 * 引用。返回非负 id 或 NETNSA_NSID_NOT_ASSIGNED，不会分配也不会睡眠。
 */
int peernet2id(const struct net *net, struct net *peer)
{
	int id;

	rcu_read_lock();
	id = __peernet2id(net, peer);
	rcu_read_unlock();

	return id;
}
EXPORT_SYMBOL(peernet2id);

/* This function returns true is the peer netns has an id assigned into the
 * current netns.
 */
/*
 * peernet_has_id() - peernet2id() 的布尔薄包装；true 表示已有非负映射，
 * false 表示未分配。参数仍是借用，生命周期要求与 peernet2id() 相同。
 */
bool peernet_has_id(const struct net *net, struct net *peer)
{
	return peernet2id(net, peer) >= 0;
}

/*
 * get_net_ns_by_id() - 由观察者 @net 的相对 nsid 取得 peer 的主动引用。
 * @id 必须非负；RCU 临界区内查 IDR，并用 maybe_get_net() 原子地拒绝已归零的
 * peer。成功返回持有引用，调用者必须 put_net()；不存在、负 id 或正在销毁
 * 返回 NULL。本函数不睡眠。
 */
struct net *get_net_ns_by_id(const struct net *net, int id)
{
	struct net *peer;

	if (id < 0)
		return NULL;

	rcu_read_lock();
	peer = idr_find(&net->netns_ids, id);
	if (peer)
		peer = maybe_get_net(peer);
	rcu_read_unlock();

	return peer;
}
EXPORT_SYMBOL_GPL(get_net_ns_by_id);

/*
 * preinit_net_sysctl() - 写入不依赖 pernet 回调的核心 socket/sysctl 默认值。
 * @net 是正在预初始化、尚未发布的借用对象；调用者拥有独占写权限。无返回值，
 * 不分配、不睡眠。这里只写默认字段，sysctl 表由协议 pernet init 后续注册。
 */
static __net_init void preinit_net_sysctl(struct net *net)
{
	net->core.sysctl_somaxconn = SOMAXCONN;
	/* Limits per socket sk_omem_alloc usage.
	 * TCP zerocopy regular usage needs 128 KB.
	 */
	/*
	 * sysctl_optmem_max 限制单 socket 记入 sk_omem_alloc 的附加内存；普通 TCP
	 * 零拷贝需要 128 KiB，因此默认值不能低于该常见工作集。
	 */
	net->core.sysctl_optmem_max = 128 * 1024;
	net->core.sysctl_txrehash = SOCK_TXREHASH_ENABLED;
	net->core.sysctl_tstamp_allow_data = 1;
	net->core.sysctl_txq_reselection = msecs_to_jiffies(1000);
}

/* init code that must occur even if setup_net() is not called. */
/*
 * preinit_net() - 建立 struct net 外壳的基础不变量，即使后续 setup 未调用或
 * 失败，清理路径也能安全释放它。
 *
 * @net：零初始化的新对象或静态 init_net，调用者独占；@user_ns：拥有者 user
 * namespace 的借用指针，引用由成功的 copy_net_ns() 另行取得。可能返回
 * ns_common_init() 的负 errno；成功返回 0，并初始化 passive 引用、追踪器、
 * nsid IDR/锁、核心链表和默认 sysctl。可睡眠；成功后仍未运行协议初始化，
 * 也尚未发布到全局 net 表。
 */
static __net_init int preinit_net(struct net *net, struct user_namespace *user_ns)
{
	/* ret 只承载 ns_common_init 的 errno；后续基础初始化没有失败出口。 */
	int ret;

	ret = ns_common_init(net);
	if (ret)
		return ret;

	/* passive=1 是外壳基础存储引用，主动引用耗尽后由清理路径最终消费。 */
	refcount_set(&net->passive, 1);
	ref_tracker_dir_init(&net->refcnt_tracker, 128, "net_refcnt");
	ref_tracker_dir_init(&net->notrefcnt_tracker, 128, "net_notrefcnt");

	/* 随机盐隔离不同 net 的哈希布局；dev_base_seq 从首个有效序号开始。 */
	net->hash_mix = get_random_u32();
	net->dev_base_seq = 1;
	net->user_ns = user_ns;

	/* nsid_lock 与 IDR 构成“观察者 net -> peer 相对编号”的局部索引。 */
	idr_init(&net->netns_ids);
	spin_lock_init(&net->nsid_lock);
	mutex_init(&net->ipv4.ra_mutex);

#ifdef CONFIG_DEBUG_NET_SMALL_RTNL
	/* 小粒度 RTNL 配置为每个 net 建独立互斥锁和 lockdep 比较规则。 */
	mutex_init(&net->rtnl_mutex);
	lock_set_cmp_fn(&net->rtnl_mutex, rtnl_net_lock_cmp_fn, NULL);
#endif

	/* 报文类型链表和默认参数必须在协议 pernet init 读取前就绪。 */
	INIT_LIST_HEAD(&net->ptype_all);
	INIT_LIST_HEAD(&net->ptype_specific);
	preinit_net_sysctl(net);
	return 0;
}

/*
 * setup_net runs the initializers for the network namespace object.
 */
/* setup_net 为网络命名空间对象运行所有已注册初始化器。 */
/*
 * setup_net() - 运行完整 pernet 构造链并把成功对象发布到全局索引。
 *
 * @net 已通过 preinit_net()；调用者持 pernet_ops_rwsem（动态创建持读锁，
 * init_net 启动持写锁），因此 pernet_list 稳定。函数可睡眠。
 * 成功返回 0：所有 ops 已按注册顺序初始化，net 已加入全局 RCU 表和 ns_tree；
 * 失败返回首个 init errno，仅逆序撤销此前成功的 ops 并排空其 RCU callback，
 * net 从未发布，仍由调用者释放。
 */
static __net_init int setup_net(struct net *net)
{
	/* Must be called with pernet_ops_rwsem held */
	/* 必须持上述锁；ops 标记当前/失败回调，net_exit_list 只在回滚时承载 net。 */
	const struct pernet_operations *ops;
	LIST_HEAD(net_exit_list);
	int error = 0;

	/* cookie 先生成，使任何子系统 init 都能使用稳定的跨接口标识。 */
	net->net_cookie = ns_tree_gen_id(net);

	/* 阶段 1：按依赖注册顺序初始化，ops 在失败时界定逆序回滚起点。 */
	list_for_each_entry(ops, &pernet_list, list) {
		error = ops_init(ops, net);
		if (error < 0)
			goto out_undo;
	}
	/* 阶段 2：全部成功后才发布，RCU 读者不会看到半初始化对象。 */
	down_write(&net_rwsem);
	list_add_tail_rcu(&net->list, &net_namespace_list);
	up_write(&net_rwsem);
	ns_tree_add_raw(net);
out:
	return error;

out_undo:
	/* Walk through the list backwards calling the exit functions
	 * for the pernet modules whose init functions did not fail.
	 */
	/*
	 * 失败 ops 已由 ops_init() 自行回滚；这里只把 net 放入临时批次，逆序退出
	 * 更早成功的模块。net 未进入全局表，因此失败路径仍保有独占清理权。
	 */
	list_add(&net->exit_list, &net_exit_list);
	ops_undo_list(&pernet_list, ops, &net_exit_list, false);
	/* 等完退出回调排队的 callback，调用者才可销毁 net 外壳。 */
	rcu_barrier();
	goto out;
}

#ifdef CONFIG_NET_NS
/*
 * inc_net_namespaces()/dec_net_namespaces() - 对 user namespace 与当前有效 euid
 * 的 UCOUNT_NET_NAMESPACES 配额成对记账。成功返回持有的 ucounts 指针，超限
 * 返回 NULL；dec 消费该记账引用，防止用户层级无限创建 netns。
 */
static struct ucounts *inc_net_namespaces(struct user_namespace *ns)
{
	return inc_ucount(ns, current_euid(), UCOUNT_NET_NAMESPACES);
}

static void dec_net_namespaces(struct ucounts *ucounts)
{
	dec_ucount(ucounts, UCOUNT_NET_NAMESPACES);
}

/* 动态 struct net 专用 slab；init_net 是静态对象，不从这里分配。 */
static struct kmem_cache *net_cachep __ro_after_init;
/* 单线程销毁队列串行化全局摘除和 nsid 清理。 */
static struct workqueue_struct *netns_wq;

/*
 * net_alloc() - 分配尚未 preinit 的动态 struct net、generic 表及可选 key 域。
 * 调用者处于可睡眠上下文；成功返回独占的零初始化 net，net->gen 已指向新表；
 * 失败返回 NULL 并逆序释放已取得资源。此时 ns_common、引用和 pernet 状态尚未
 * 建立，不能把返回对象交给普通 netns 查找者。
 */
static struct net *net_alloc(void)
{
	/* ng/net/key_domain 构成分配栈，失败标签严格逆序回收。 */
	struct net *net = NULL;
	struct net_generic *ng;

	ng = net_alloc_generic();
	if (!ng)
		goto out;

	net = kmem_cache_zalloc(net_cachep, GFP_KERNEL);
	if (!net)
		goto out_free;

#ifdef CONFIG_KEYS
	net->key_domain = kzalloc_obj(struct key_tag);
	if (!net->key_domain)
		goto out_free_2;
	refcount_set(&net->key_domain->usage, 1);
#endif

	/* 对象尚不可见；使用 RCU API 是为了从一开始满足 net->gen 的访问契约。 */
	rcu_assign_pointer(net->gen, ng);
out:
	return net;

#ifdef CONFIG_KEYS
out_free_2:
	/* key_domain 失败时已取得 slab net 与 ng，先退回 slab 对象。 */
	kmem_cache_free(net_cachep, net);
	net = NULL;
#endif
out_free:
	/* ng 尚未转入成功 net 的生命周期，由本函数直接释放。 */
	kfree(ng);
	goto out;
}

/* passive 归零对象先汇入无锁链，待下一次 cleanup 的 RCU 屏障后再 slab free。 */
static LLIST_HEAD(defer_free_list);

/*
 * net_complete_free() - 释放上一轮 defer_free_list 中的 struct net 外壳。
 * 调用者 cleanup_net() 已执行 rcu_barrier()；llist_del_all 原子取得快照，本轮
 * 后续新加入的对象留到下一次屏障。无参数、无返回，释放每个快照对象的 slab。
 */
static void net_complete_free(void)
{
	/* kill_list 是本次快照；net/next 仅在安全遍历快照期间有效。 */
	struct llist_node *kill_list;
	struct net *net, *next;

	/* Get the list of namespaces to free from last round. */
	/* 原子分代，避免把尚未跨越本轮完整屏障的新对象提前释放。 */
	kill_list = llist_del_all(&defer_free_list);

	llist_for_each_entry_safe(net, next, kill_list, defer_free_list)
		kmem_cache_free(net_cachep, net);

}

/*
 * net_passive_dec() - 释放一个 struct net 的被动存储引用。
 * @net 必须由当前 passive 引用保证存活。最后一个引用释放 generic 表和追踪器，
 * 但只把外壳加入 defer_free_list；额外一轮 rcu_barrier 后才真正 free，以覆盖
 * 晚到 callback。无直接返回，可在不能睡眠的引用释放路径执行。
 */
void net_passive_dec(struct net *net)
{
	/* 原子归零判定确保只有一个执行者承担最终存储回收。 */
	if (refcount_dec_and_test(&net->passive)) {
		kfree(rcu_access_pointer(net->gen));

		/* There should not be any trackers left there. */
		/* 主动清理已完成，此追踪目录不应再含活跃记录。 */
		ref_tracker_dir_exit(&net->notrefcnt_tracker);

		/* Wait for an extra rcu_barrier() before final free. */
		/* 延后一代，防止仍排队的 RCU callback 使用 net 外壳地址。 */
		llist_add(&net->defer_free_list, &defer_free_list);
	}
}

/*
 * net_drop_ns() - ns_common 销毁框架使用的可空 passive-put 适配器。
 * @ns 为借用指针；NULL 是无操作，非 NULL 时恢复外层 net 并消费一个 passive
 * 引用。返回：无。
 */
void net_drop_ns(struct ns_common *ns)
{
	if (ns)
		net_passive_dec(to_net_ns(ns));
}

/*
 * copy_net_ns() - 为 clone/unshare 共享旧 net 或构造新网络命名空间。
 *
 * @flags：clone 标志；无 CLONE_NEWNET 时只给 @old_net 增加主动引用。
 * @user_ns：新 net 的拥有者，借用；成功新建时取得一份引用。
 * @old_net：当前 net，借用；只在共享分支使用。
 *
 * 成功返回持有主动引用的 struct net；失败返回 ERR_PTR(-ENOSPC/-ENOMEM、信号
 * 中断或 pernet init errno)。新建可睡眠。setup/锁获取失败走 put_userns，撤销
 * 已挂接的 common、key、user_ns、passive 与配额资源；任何失败都不会把半初始
 * 对象发布到全局 net 表。
 */
struct net *copy_net_ns(u64 flags,
			struct user_namespace *user_ns, struct net *old_net)
{
	/* ucounts 是配额引用，net 是构造对象，rv 始终保存对外 errno。 */
	struct ucounts *ucounts;
	struct net *net;
	int rv;

	/* 快速路径共享 old_net；返回引用随后由新 nsproxy 持有。 */
	if (!(flags & CLONE_NEWNET))
		return get_net(old_net);

	/* 阶段 1：先占配额再分配，确保任何内存分配都不能绕过 namespace 限额。 */
	ucounts = inc_net_namespaces(user_ns);
	if (!ucounts)
		return ERR_PTR(-ENOSPC);

	net = net_alloc();
	if (!net) {
		rv = -ENOMEM;
		goto dec_ucounts;
	}

	/* 阶段 2：基础字段就绪后把配额与 user_ns 生命周期挂入 net。 */
	rv = preinit_net(net, user_ns);
	if (rv < 0)
		goto dec_ucounts;
	net->ucounts = ucounts;
	get_user_ns(user_ns);

	/*
	 * 阶段 3：读锁冻结 ops 序列；killable 获取允许创建者被信号打断。setup
	 * 成功才全局发布，失败仍由本地构造栈回滚。
	 */
	rv = down_read_killable(&pernet_ops_rwsem);
	if (rv < 0)
		goto put_userns;

	rv = setup_net(net);

	up_read(&pernet_ops_rwsem);

	if (rv < 0) {
put_userns:
		/* ns_common/user_ns/配额已经挂接，但 setup 没有成功发布。 */
		ns_common_free(net);
#ifdef CONFIG_KEYS
		key_remove_domain(net->key_domain);
#endif
		put_user_ns(user_ns);
		net_passive_dec(net);
dec_ucounts:
		/* 最外层回滚消费最先取得的 user namespace 配额。 */
		dec_net_namespaces(ucounts);
		return ERR_PTR(rv);
	}
	return net;
}

/**
 * net_ns_get_ownership - get sysfs ownership data for @net
 * @net: network namespace in question (can be NULL)
 * @uid: kernel user ID for sysfs objects
 * @gid: kernel group ID for sysfs objects
 *
 * Returns the uid/gid pair of root in the user namespace associated with the
 * given network namespace.
 */
/*
 * 取得 @net 所属 user namespace 中 uid/gid 0 映射后的 sysfs 所有者；@net 可为
 * NULL，此时输出全局 root。@uid/@gid 是调用者所有的输出槽。映射无效时保留
 * 槽中原值，供上层使用既定 fallback。无返回值，不改变 net、不取得引用。
 */
void net_ns_get_ownership(const struct net *net, kuid_t *uid, kgid_t *gid)
{
	if (net) {
		kuid_t ns_root_uid = make_kuid(net->user_ns, 0);
		kgid_t ns_root_gid = make_kgid(net->user_ns, 0);

		if (uid_valid(ns_root_uid))
			*uid = ns_root_uid;

		if (gid_valid(ns_root_gid))
			*gid = ns_root_gid;
	} else {
		*uid = GLOBAL_ROOT_UID;
		*gid = GLOBAL_ROOT_GID;
	}
}
EXPORT_SYMBOL_GPL(net_ns_get_ownership);

/*
 * unhash_nsid() - 从所有存活观察者中删除指向 dying peer 的相对 nsid。
 *
 * @last 是 cleanup 摘除批次后缓存的全局表尾哨兵，借用且仍存活。函数只由
 * 单线程 netns_wq 调用；逐观察者持 nsid_lock，删除条目后解锁发送
 * RTM_DELNSID，再重新加锁继续。返回：无。
 */
static void unhash_nsid(struct net *last)
{
	/* tmp 是观察者，peer 是当前 IDR 值；curr_id 固定删除槽，id 指向后继。 */
	struct net *tmp, *peer;

	/* This function is only called from cleanup_net() work,
	 * and this work is the only process, that may delete
	 * a net from net_namespace_list. So, when the below
	 * is executing, the list may only grow. Thus, we do not
	 * use for_each_net_rcu() or net_rwsem.
	 */
	/*
	 * cleanup worker 是唯一删除全局 net 表的执行者，本段期间表只会追加。因此
	 * 普通 for_each_net() 可扫描到缓存的 last 后停止；之后新建的 net 受
	 * peernet2id_alloc() 存活检查约束，不能重新引用本批 dying peer。
	 */
	for_each_net(tmp) {
		int id = 0;

		spin_lock(&tmp->nsid_lock);
		while ((peer = idr_get_next(&tmp->netns_ids, &id))) {
			int curr_id = id;

			id++;
			if (!peer->is_dying)
				continue;

			idr_remove(&tmp->netns_ids, curr_id);
			/* 通知会分配并可能睡眠，必须在释放自旋锁后发送。 */
			spin_unlock(&tmp->nsid_lock);
			rtnl_net_notifyid(tmp, RTM_DELNSID, curr_id, 0, NULL,
					  GFP_KERNEL);
			spin_lock(&tmp->nsid_lock);
		}
		spin_unlock(&tmp->nsid_lock);
		if (tmp == last)
			break;
	}
}

/* 最后一个主动引用归零的 net 汇入此无锁链，由专用 worker 批量接管。 */
static LLIST_HEAD(cleanup_list);

/* 非 NULL 时标识当前 cleanup worker task，供诊断和清理上下文识别。 */
struct task_struct *cleanup_net_task;

/*
 * cleanup_net() - 批量执行网络命名空间不可逆销毁。
 *
 * @work 是静态 net_cleanup_work 的借用指针，不代表单个 net。函数运行于专用
 * 单线程可睡眠工作队列；cleanup_list 中对象的主动引用均已归零，清理所有权
 * 由本 worker 独占。
 *
 * 阶段：快照 -> 从 ns_tree/全局表摘除并标 dying -> 删除外部 nsid -> 逆序
 * pernet exit -> 排空 RCU callback -> 释放 common/user/配额资源 -> 消费
 * passive 基础引用。无返回；完成后对象不可再取得主动引用，外壳可能因被动
 * 引用和延迟 RCU 机制继续存在一轮。
 */
static void cleanup_net(struct work_struct *work)
{
	/* net_kill_list 是本批无锁快照；net_exit_list 是有序的 pernet 退出视图。 */
	struct llist_node *net_kill_list;
	struct net *net, *tmp, *last;
	LIST_HEAD(net_exit_list);

	/* 无锁观察者只需看到明确 task 身份，WRITE_ONCE 防止编译器合并访问。 */
	WRITE_ONCE(cleanup_net_task, current);

	/* Atomically snapshot the list of namespaces to cleanup */
	/* 此后新归零对象留在 cleanup_list，由下一轮 work 处理。 */
	net_kill_list = llist_del_all(&cleanup_list);

	/* 冻结 pernet 回调集合，保证整批按同一构造链逆序退出。 */
	down_read(&pernet_ops_rwsem);

	/* Don't let anyone else find us. */
	/*
	 * 在 net_rwsem 写锁下从两种全局索引摘除，再标记 dying。list_del_rcu 让
	 * 已进入读侧临界区的遍历者完成，但新查找不会再发现这些 net。
	 */
	down_write(&net_rwsem);
	llist_for_each_entry(net, net_kill_list, cleanup_list) {
		ns_tree_remove(net);
		list_del_rcu(&net->list);
		net->is_dying = true;
	}
	/* Cache last net. After we unlock rtnl, no one new net
	 * added to net_namespace_list can assign nsid pointer
	 * to a net from net_kill_list (see peernet2id_alloc()).
	 * So, we skip them in unhash_nsid().
	 *
	 * Note, that unhash_nsid() does not delete nsid links
	 * between net_kill_list's nets, as they've already
	 * deleted from net_namespace_list. But, this would be
	 * useless anyway, as netns_ids are destroyed there.
	 */
	/*
	 * 缓存摘除后的表尾。释放锁后新 net 可以追加，但存活检查禁止它们为本批
	 * dying net 新建 nsid；批次内部映射也无需通知，因为各自 IDR 马上整体销毁。
	 */
	last = list_last_entry(&net_namespace_list, struct net, list);
	up_write(&net_rwsem);

	unhash_nsid(last);

	/* 把无锁快照转换为普通退出链，同时销毁每个 net 自己作为观察者的 IDR。 */
	llist_for_each_entry(net, net_kill_list, cleanup_list) {
		idr_destroy(&net->netns_ids);
		list_add_tail(&net->exit_list, &net_exit_list);
	}

	/* 加速宽限期降低销毁等待时间，但比普通 synchronize_rcu 成本更高。 */
	ops_undo_list(&pernet_list, NULL, &net_exit_list, true);

	up_read(&pernet_ops_rwsem);

	/* Ensure there are no outstanding rcu callbacks using this
	 * network namespace.
	 */
	/*
	 * 前面的 grace period 只等读者离开；rcu_barrier 还要等所有 CPU 已排队的
	 * callback 真正执行完，随后才能回收 callback 可能使用的 net 外壳。
	 */
	rcu_barrier();

	/* 先释放已跨过额外屏障的上一代对象，本代稍后才加入 defer_free_list。 */
	net_complete_free();

	/* Finally it is safe to free my network namespace structure */
	/*
	 * 释放通用元数据、配额、key 域和 user_ns 引用，最后消费 passive 基础引用。
	 * net_passive_dec 即使归零也只延迟入链，不在本循环直接 free。
	 */
	list_for_each_entry_safe(net, tmp, &net_exit_list, exit_list) {
		list_del_init(&net->exit_list);
		ns_common_free(net);
		dec_net_namespaces(net->ucounts);
#ifdef CONFIG_KEYS
		key_remove_domain(net->key_domain);
#endif
		put_user_ns(net->user_ns);
		net_passive_dec(net);
	}
	/* 清空身份是本批完成标记，与入口 WRITE_ONCE 构成可观察状态对。 */
	WRITE_ONCE(cleanup_net_task, NULL);
}

/**
 * net_ns_barrier - wait until concurrent net_cleanup_work is done
 *
 * cleanup_net runs from work queue and will first remove namespaces
 * from the global list, then run net exit functions.
 *
 * Call this in module exit path to make sure that all netns
 * ->exit ops have been invoked before the function is removed.
 */
/*
 * cleanup_net 先摘除全局可见性，再持 pernet_ops_rwsem 读锁调用 exit。模块退出
 * 通过取得同一锁的写侧等待并发 cleanup 完成，保证代码卸载前所有 ->exit 已
 * 返回。无参数、无返回；可睡眠，但它不保证 passive 外壳已经 slab free。
 */
void net_ns_barrier(void)
{
	down_write(&pernet_ops_rwsem);
	up_write(&pernet_ops_rwsem);
}
EXPORT_SYMBOL(net_ns_barrier);

/*
 * 全局静态 work 作为 cleanup_list 的调度令牌；无锁链可批量承载多个 net，
 * 因此无需为每个待销毁对象分配独立 work_struct。
 */
static DECLARE_WORK(net_cleanup_work, cleanup_net);

/*
 * __put_net() - 主动引用归零慢路径，把 net 交给进程上下文清理。
 * @net 已无主动引用，调用者不得继续使用或复活。函数关闭引用追踪目录后无锁
 * 入 cleanup_list；只有从空变非空的入队者提交静态 work，避免重复排队。
 * 无返回，pernet 退出和释放异步发生；本函数本身不睡眠。
 */
void __put_net(struct net *net)
{
	ref_tracker_dir_exit(&net->refcnt_tracker);
	/* Cleanup the network namespace in process context */
	/* cleanup 会获取睡眠锁并调用可睡眠回调，不能在任意最后-put 上下文中直做。 */
	if (llist_add(&net->cleanup_list, &cleanup_list))
		queue_work(netns_wq, &net_cleanup_work);
}
EXPORT_SYMBOL_GPL(__put_net);

/**
 * get_net_ns - increment the refcount of the network namespace
 * @ns: common namespace (net)
 *
 * Returns the net's common namespace or ERR_PTR() if ref is zero.
 */
/*
 * 增加 common namespace 外层 net 的主动引用。@ns 是借用指针；container_of
 * 只恢复地址，真正的非零原子获取由 maybe_get_net() 完成。成功返回持有引用
 * 的 &net->ns，调用者负责 put；引用已归零返回 ERR_PTR(-EINVAL)，拒绝复活。
 * 无副作用失败路径，不睡眠。
 */
struct ns_common *get_net_ns(struct ns_common *ns)
{
	struct net *net;

	net = maybe_get_net(container_of(ns, struct net, ns));
	if (net)
		return &net->ns;
	return ERR_PTR(-EINVAL);
}
EXPORT_SYMBOL_GPL(get_net_ns);

/*
 * get_net_ns_by_fd() - 从打开的 /proc/.../ns/net 文件取得 net 主动引用。
 * @fd 是当前进程描述符。CLASS(fd, f) 离开作用域时自动 fdput，包住全部早退。
 * 成功返回持有的 net，调用者 put_net；坏 fd 返回 -EBADF，非 proc namespace
 * 或非 net 类型返回 -EINVAL。可能因 fdget 访问当前文件表，但不转移 file 所有权。
 */
struct net *get_net_ns_by_fd(int fd)
{
	/* f 是带自动析构的临时 fd 引用，替代手写 fdget/fdput cleanup。 */
	CLASS(fd, f)(fd);

	if (fd_empty(f))
		return ERR_PTR(-EBADF);

	/* 先确认 proc namespace 文件，再用 ops 身份判别类型，避免错误 container_of。 */
	if (proc_ns_file(fd_file(f))) {
		struct ns_common *ns = get_proc_ns(file_inode(fd_file(f)));
		if (ns->ops == &netns_operations)
			return get_net(container_of(ns, struct net, ns));
	}

	return ERR_PTR(-EINVAL);
}
EXPORT_SYMBOL_GPL(get_net_ns_by_fd);
#endif

/*
 * get_net_ns_by_pid() - 取得指定可见 PID 当前 nsproxy 中的 net 主动引用。
 * @pid 按调用者 PID namespace 解释。RCU 稳定 task 查找，task_lock 串行化
 * nsproxy 替换/清空；成功返回持有的 net，调用者 put_net，task 不存在或正退出
 * 且无 nsproxy 返回 ERR_PTR(-ESRCH)。不持 task 引用跨出 RCU 临界区。
 */
struct net *get_net_ns_by_pid(pid_t pid)
{
	struct task_struct *tsk;
	struct net *net;

	/* Lookup the network namespace */
	/* 先放置默认错误，只有在锁内取得 net 引用后才覆盖它。 */
	net = ERR_PTR(-ESRCH);
	rcu_read_lock();
	tsk = find_task_by_vpid(pid);
	if (tsk) {
		/* nsproxy 是 task_lock 保护窗口内的借用指针，不能带到解锁后使用。 */
		struct nsproxy *nsproxy;
		task_lock(tsk);
		nsproxy = tsk->nsproxy;
		if (nsproxy)
			net = get_net(nsproxy->net_ns);
		task_unlock(tsk);
	}
	rcu_read_unlock();
	return net;
}
EXPORT_SYMBOL_GPL(get_net_ns_by_pid);

#ifdef CONFIG_NET_NS_REFCNT_TRACKER
/*
 * net_ns_net_debugfs() - 为一个 net 的主动/被动引用追踪目录建立可识别软链接。
 * @net 已完成 cookie/inum 初始化，借用且保持存活。链接名同时含 cookie 与 inode
 * 以减少复用混淆；无返回，debug 配置下可能创建 debugfs 项。
 */
static void net_ns_net_debugfs(struct net *net)
{
	ref_tracker_dir_symlink(&net->refcnt_tracker, "netns-%llx-%u-refcnt",
				net->net_cookie, net->ns.inum);
	ref_tracker_dir_symlink(&net->notrefcnt_tracker, "netns-%llx-%u-notrefcnt",
				net->net_cookie, net->ns.inum);
}

/*
 * init_net_debugfs() - late_initcall 阶段发布 init_net 的两个引用追踪目录。
 * 无参数；init_net 永久存活。成功固定返回 0，副作用是创建 debugfs 目录/链接。
 */
static int __init init_net_debugfs(void)
{
	ref_tracker_dir_debugfs(&init_net.refcnt_tracker);
	ref_tracker_dir_debugfs(&init_net.notrefcnt_tracker);
	net_ns_net_debugfs(&init_net);
	return 0;
}
late_initcall(init_net_debugfs);
#else
/* 引用追踪关闭时保留同签名空 stub，让通用 pernet init 无需条件编译。 */
static void net_ns_net_debugfs(struct net *net)
{
}
#endif

/*
 * net_ns_net_init() - 每个 net 创建时接入本文件自身的调试展示。
 * @net 为正在 setup 的借用对象；成功固定返回 0。追踪关闭时调用空 stub，
 * 因而该 pernet ops 在所有配置下都保持相同注册顺序。
 */
static __net_init int net_ns_net_init(struct net *net)
{
	net_ns_net_debugfs(net);
	return 0;
}

/* 本文件自己的 pernet 回调描述，仅负责调用 net_ns_net_init。 */
static struct pernet_operations __net_initdata net_ns_ops = {
	.init = net_ns_net_init,
};

/*
 * RTM_{NEW,GET}NSID 属性类型白名单：NSID/TARGET_NSID 是有符号相对编号，
 * PID/FD 是无符号句柄。策略只验证基础线格式，组合语义由各 handler 检查。
 */
static const struct nla_policy rtnl_net_policy[NETNSA_MAX + 1] = {
	[NETNSA_NONE]		= { .type = NLA_UNSPEC },
	[NETNSA_NSID]		= { .type = NLA_S32 },
	[NETNSA_PID]		= { .type = NLA_U32 },
	[NETNSA_FD]		= { .type = NLA_U32 },
	[NETNSA_TARGET_NSID]	= { .type = NLA_S32 },
};

/*
 * rtnl_net_newid() - 处理 RTM_NEWNSID，为请求 socket 所在 net 指定 peer 映射。
 *
 * @skb 提供请求 socket/portid，@nlh 是借用的 netlink 消息，@extack 输出精确
 * 错误属性和文字。请求必须含 NETNSA_NSID，并以 PID 或 FD 指定 peer。函数可
 * 睡眠；成功返回 0、安装 IDR 裸指针并广播 NEWNSID，失败返回解析/查找、
 * -EEXIST 或分配 errno。取得的 peer 主动引用在统一 out 释放。
 */
static int rtnl_net_newid(struct sk_buff *skb, struct nlmsghdr *nlh,
			  struct netlink_ext_ack *extack)
{
	/* net 是观察者借用指针；peer 是待获取引用；nla 记录出错的来源属性。 */
	struct net *net = sock_net(skb->sk);
	struct nlattr *tb[NETNSA_MAX + 1];
	struct nlattr *nla;
	struct net *peer;
	int nsid, err;

	/* 阶段 1：按策略解析并验证必需 nsid，extack 对应用户可诊断错误。 */
	err = nlmsg_parse_deprecated(nlh, sizeof(struct rtgenmsg), tb,
				     NETNSA_MAX, rtnl_net_policy, extack);
	if (err < 0)
		return err;
	if (!tb[NETNSA_NSID]) {
		NL_SET_ERR_MSG(extack, "nsid is missing");
		return -EINVAL;
	}
	nsid = nla_get_s32(tb[NETNSA_NSID]);

	/* 阶段 2：PID 优先于 FD，把外部句柄转换为持有引用的 peer。 */
	if (tb[NETNSA_PID]) {
		peer = get_net_ns_by_pid(nla_get_u32(tb[NETNSA_PID]));
		nla = tb[NETNSA_PID];
	} else if (tb[NETNSA_FD]) {
		peer = get_net_ns_by_fd(nla_get_u32(tb[NETNSA_FD]));
		nla = tb[NETNSA_FD];
	} else {
		NL_SET_ERR_MSG(extack, "Peer netns reference is missing");
		return -EINVAL;
	}
	if (IS_ERR(peer)) {
		NL_SET_BAD_ATTR(extack, nla);
		NL_SET_ERR_MSG(extack, "Peer netns reference is invalid");
		return PTR_ERR(peer);
	}

	/* 阶段 3：锁内原子完成“尚无映射”检查与指定槽分配，防止重复/抢占。 */
	spin_lock(&net->nsid_lock);
	if (__peernet2id(net, peer) >= 0) {
		spin_unlock(&net->nsid_lock);
		err = -EEXIST;
		NL_SET_BAD_ATTR(extack, nla);
		NL_SET_ERR_MSG(extack,
			       "Peer netns already has a nsid assigned");
		goto out;
	}

	err = alloc_netid(net, peer, nsid);
	spin_unlock(&net->nsid_lock);
	/* idr_alloc 返回实际槽；精确槽冲突的 -ENOSPC 对用户转换成 -EEXIST。 */
	if (err >= 0) {
		rtnl_net_notifyid(net, RTM_NEWNSID, err, NETLINK_CB(skb).portid,
				  nlh, GFP_KERNEL);
		err = 0;
	} else if (err == -ENOSPC && nsid >= 0) {
		err = -EEXIST;
		NL_SET_BAD_ATTR(extack, tb[NETNSA_NSID]);
		NL_SET_ERR_MSG(extack, "The specified nsid is already used");
	}
out:
	/* 所有 peer 获取成功后的出口都消费主动引用；IDR 仅保存受协议保护的裸指针。 */
	put_net(peer);
	return err;
}

/*
 * rtnl_net_get_size() - 计算一条 nsid 回复/通知 skb 所需的保守字节数。
 * 无参数、不睡眠；返回对齐 rtgenmsg 加两个可选 s32 属性的总空间。
 */
static int rtnl_net_get_size(void)
{
	return NLMSG_ALIGN(sizeof(struct rtgenmsg))
	       + nla_total_size(sizeof(s32)) /* NETNSA_NSID */
	       /* 回复携带目标 net 眼中的 peer 编号。 */
	       + nla_total_size(sizeof(s32)) /* NETNSA_CURRENT_NSID */
	       /* 可选地再携带当前/引用 net 眼中的同一 peer 编号。 */
	       ;
}

/*
 * net_fill_args 描述一条 RTM_NEWNSID/DELNSID 消息的纯值快照：portid/seq/flags
 * 构成 netlink 信封，cmd 选择消息类型，nsid 是主视角编号；add_ref 为 true 时
 * ref_nsid 追加另一观察者视角的 NETNSA_CURRENT_NSID。结构不持有对象引用。
 */
struct net_fill_args {
	u32 portid;
	u32 seq;
	int flags;
	int cmd;
	int nsid;
	bool add_ref;
	int ref_nsid;
};

/*
 * rtnl_net_fill() - 向调用者拥有的 @skb 追加一条完整 nsid netlink 消息。
 * @args 是只在调用期借用的值快照。成功返回 0，skb 尾部提交消息；空间不足
 * 返回 -EMSGSIZE，并用 nlmsg_cancel 回滚本次尚未提交的追加，原有 skb 内容保留。
 * 不分配、不取得引用，适用于单答复、dump 和 multicast 通知共同复用。
 */
static int rtnl_net_fill(struct sk_buff *skb, struct net_fill_args *args)
{
	struct nlmsghdr *nlh;
	struct rtgenmsg *rth;

	/* 阶段 1：预留信封和 rtgenmsg；NULL 表示 skb 剩余 tailroom 不足。 */
	nlh = nlmsg_put(skb, args->portid, args->seq, args->cmd, sizeof(*rth),
			args->flags);
	if (!nlh)
		return -EMSGSIZE;

	rth = nlmsg_data(nlh);
	rth->rtgen_family = AF_UNSPEC;

	/* 阶段 2：主 nsid 必有，引用视角属性由 add_ref 显式控制。 */
	if (nla_put_s32(skb, NETNSA_NSID, args->nsid))
		goto nla_put_failure;

	if (args->add_ref &&
	    nla_put_s32(skb, NETNSA_CURRENT_NSID, args->ref_nsid))
		goto nla_put_failure;

	nlmsg_end(skb, nlh);
	return 0;

nla_put_failure:
	/* 撤销从 nlh 开始的半条消息，防止 dump 用户读到截断 TLV。 */
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

/*
 * rtnl_net_valid_getid_req() - 按 socket 严格模式解析并筛选 GETNSID 属性。
 * @skb/@nlh 为请求借用；@tb 是输出属性表；@extack 接收诊断。旧客户端未启用
 * strict check 时保留兼容解析；严格模式拒绝策略虽认识、但 GET 语义不支持的
 * 属性。成功返回 0 并填充 tb，失败返回 netlink 解析 errno。
 */
static int rtnl_net_valid_getid_req(struct sk_buff *skb,
				    const struct nlmsghdr *nlh,
				    struct nlattr **tb,
				    struct netlink_ext_ack *extack)
{
	int i, err;

	/* 兼容分支不额外枚举属性，避免改变旧 rtnetlink GET 请求行为。 */
	if (!netlink_strict_get_check(skb))
		return nlmsg_parse_deprecated(nlh, sizeof(struct rtgenmsg),
					      tb, NETNSA_MAX, rtnl_net_policy,
					      extack);

	err = nlmsg_parse_deprecated_strict(nlh, sizeof(struct rtgenmsg), tb,
					    NETNSA_MAX, rtnl_net_policy,
					    extack);
	if (err)
		return err;

	/* 严格分支逐槽拒绝此命令未定义的扩展，防止静默忽略用户错误。 */
	for (i = 0; i <= NETNSA_MAX; i++) {
		if (!tb[i])
			continue;

		switch (i) {
		case NETNSA_PID:
		case NETNSA_FD:
		case NETNSA_NSID:
		case NETNSA_TARGET_NSID:
			break;
		default:
			NL_SET_ERR_MSG(extack, "Unsupported attribute in peer netns getid request");
			return -EINVAL;
		}
	}

	return 0;
}

/*
 * rtnl_net_getid() - 回复 peer 在指定观察者中的 nsid。
 *
 * 请求 socket 的 net 是默认 target；peer 可由 PID、FD 或当前 net 的 NSID 指定，
 * NETNSA_TARGET_NSID 可把查询视角切到另一个有权限访问的 net。所有解析对象均
 * 为借用，peer/可选 target 转换后持主动引用。成功把自建 skb 单播给请求端并
 * 返回 rtnl_unicast 结果；失败返回解析、权限、查找、-ENOMEM/-EMSGSIZE。
 * 所有引用和未发送 skb 都在对应 cleanup 出口释放；函数可睡眠。
 */
static int rtnl_net_getid(struct sk_buff *skb, struct nlmsghdr *nlh,
			  struct netlink_ext_ack *extack)
{
	/* fillargs 固定回复信封；target 默认借用 net，仅显式切换时才持有引用。 */
	struct net *net = sock_net(skb->sk);
	struct nlattr *tb[NETNSA_MAX + 1];
	struct net_fill_args fillargs = {
		.portid = NETLINK_CB(skb).portid,
		.seq = nlh->nlmsg_seq,
		.cmd = RTM_NEWNSID,
	};
	struct net *peer, *target = net;
	struct nlattr *nla;
	struct sk_buff *msg;
	int err;

	/* 阶段 1：验证请求并把三种互斥 peer 标识转换成持有引用。 */
	err = rtnl_net_valid_getid_req(skb, nlh, tb, extack);
	if (err < 0)
		return err;
	if (tb[NETNSA_PID]) {
		peer = get_net_ns_by_pid(nla_get_u32(tb[NETNSA_PID]));
		nla = tb[NETNSA_PID];
	} else if (tb[NETNSA_FD]) {
		peer = get_net_ns_by_fd(nla_get_u32(tb[NETNSA_FD]));
		nla = tb[NETNSA_FD];
	} else if (tb[NETNSA_NSID]) {
		peer = get_net_ns_by_id(net, nla_get_s32(tb[NETNSA_NSID]));
		if (!peer)
			peer = ERR_PTR(-ENOENT);
		nla = tb[NETNSA_NSID];
	} else {
		NL_SET_ERR_MSG(extack, "Peer netns reference is missing");
		return -EINVAL;
	}

	if (IS_ERR(peer)) {
		NL_SET_BAD_ATTR(extack, nla);
		NL_SET_ERR_MSG(extack, "Peer netns reference is invalid");
		return PTR_ERR(peer);
	}

	/*
	 * 阶段 2：可选 target 改变“从谁的 IDR 看 peer”。权限 helper 成功返回持有
	 * 引用；CURRENT_NSID 仍记录原请求 net 对 peer 的编号，便于关联两个视角。
	 */
	if (tb[NETNSA_TARGET_NSID]) {
		int id = nla_get_s32(tb[NETNSA_TARGET_NSID]);

		target = rtnl_get_net_ns_capable(NETLINK_CB(skb).sk, id);
		if (IS_ERR(target)) {
			NL_SET_BAD_ATTR(extack, tb[NETNSA_TARGET_NSID]);
			NL_SET_ERR_MSG(extack,
				       "Target netns reference is invalid");
			err = PTR_ERR(target);
			goto out;
		}
		fillargs.add_ref = true;
		fillargs.ref_nsid = peernet2id(net, peer);
	}

	/* 阶段 3：构造独立回复 skb；fill 成功后所有权转给 rtnl_unicast。 */
	msg = nlmsg_new(rtnl_net_get_size(), GFP_KERNEL);
	if (!msg) {
		err = -ENOMEM;
		goto out;
	}

	fillargs.nsid = peernet2id(target, peer);
	err = rtnl_net_fill(msg, &fillargs);
	if (err < 0)
		goto err_out;

	err = rtnl_unicast(msg, net, NETLINK_CB(skb).portid);
	goto out;

err_out:
	/* fill 失败时 skb 尚未转移，必须本地释放。 */
	nlmsg_free(msg);
out:
	/* target 只有 add_ref=true 时是额外引用；peer 在所有成功查找分支都需 put。 */
	if (fillargs.add_ref)
		put_net(target);
	put_net(peer);
	return err;
}

/*
 * rtnl_net_dump_cb 是一次 dump 调用的栈上游标：tgt_net 是被遍历 IDR 的观察者，
 * ref_net 用于可选交叉视角，skb 是调用者输出缓冲，fillargs 是复用消息模板；
 * idx 为本轮逻辑序号，s_idx 是上轮保存的恢复位置。结构本身不自动管理引用。
 */
struct rtnl_net_dump_cb {
	struct net *tgt_net;
	struct net *ref_net;
	struct sk_buff *skb;
	struct net_fill_args fillargs;
	int idx;
	int s_idx;
};

/* Runs in RCU-critical section. */
/*
 * 在 RCU 临界区作为 idr_for_each 回调运行。@id/@peer 是当前条目借用值，@data
 * 指向本轮 dump 上下文。先跳过恢复点之前的条目；成功追加后递增逻辑游标。
 * 返回 0 继续，skb 满时返回 -EMSGSIZE 令遍历停止，下轮由 s_idx 续传。
 */
static int rtnl_net_dumpid_one(int id, void *peer, void *data)
{
	struct rtnl_net_dump_cb *net_cb = (struct rtnl_net_dump_cb *)data;
	int ret;

	/* idx 以条目数而非稀疏 ID 值分页，避免 IDR 空洞破坏恢复。 */
	if (net_cb->idx < net_cb->s_idx)
		goto cont;

	net_cb->fillargs.nsid = id;
	if (net_cb->fillargs.add_ref)
		net_cb->fillargs.ref_nsid = __peernet2id(net_cb->ref_net, peer);
	ret = rtnl_net_fill(net_cb->skb, &net_cb->fillargs);
	if (ret < 0)
		return ret;

cont:
	net_cb->idx++;
	return 0;
}

/*
 * rtnl_valid_dump_net_req() - 严格验证 dump 请求并解析可选目标观察者。
 * @nlh/@sk/@cb 为借用；@net_cb 是输入输出上下文。仅允许
 * NETNSA_TARGET_NSID，成功时取得目标 net 引用、把原 target 保存为 ref_net，
 * 并设置 add_ref；失败返回解析/权限 errno，extack 指向坏属性。
 */
static int rtnl_valid_dump_net_req(const struct nlmsghdr *nlh, struct sock *sk,
				   struct rtnl_net_dump_cb *net_cb,
				   struct netlink_callback *cb)
{
	struct netlink_ext_ack *extack = cb->extack;
	struct nlattr *tb[NETNSA_MAX + 1];
	int err, i;

	err = nlmsg_parse_deprecated_strict(nlh, sizeof(struct rtgenmsg), tb,
					    NETNSA_MAX, rtnl_net_policy,
					    extack);
	if (err < 0)
		return err;

	/* dump 没有单个 peer 条件，除 target 外的属性均是语义错误。 */
	for (i = 0; i <= NETNSA_MAX; i++) {
		if (!tb[i])
			continue;

		if (i == NETNSA_TARGET_NSID) {
			struct net *net;

			net = rtnl_get_net_ns_capable(sk, nla_get_s32(tb[i]));
			if (IS_ERR(net)) {
				NL_SET_BAD_ATTR(extack, tb[i]);
				NL_SET_ERR_MSG(extack,
					       "Invalid target network namespace id");
				return PTR_ERR(net);
			}
			net_cb->fillargs.add_ref = true;
			net_cb->ref_net = net_cb->tgt_net;
			net_cb->tgt_net = net;
		} else {
			NL_SET_BAD_ATTR(extack, tb[i]);
			NL_SET_ERR_MSG(extack,
				       "Unsupported attribute in dump request");
			return -EINVAL;
		}
	}

	return 0;
}

/*
 * rtnl_net_dumpid() - 分页枚举目标 net 的全部 peer nsid 映射。
 * @skb 是本轮输出缓冲，@cb 跨轮保存 args[0] 游标并携带请求。默认遍历请求
 * socket 的 net；严格请求可选择有权限的 target。RCU 保护 IDR 遍历，回调只
 * 使用借用 peer。返回验证 errno 或 0；skb 满不视为最终错误，而由 idx 续传。
 * 显式 target 引用在每轮结束释放。
 */
static int rtnl_net_dumpid(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct rtnl_net_dump_cb net_cb = {
		.tgt_net = sock_net(skb->sk),
		.skb = skb,
		.fillargs = {
			.portid = NETLINK_CB(cb->skb).portid,
			.seq = cb->nlh->nlmsg_seq,
			.flags = NLM_F_MULTI,
			.cmd = RTM_NEWNSID,
		},
		.idx = 0,
		.s_idx = cb->args[0],
	};
	int err = 0;

	/* 只有 strict dump 才解析扩展 target，旧请求保持默认当前 net 视角。 */
	if (cb->strict_check) {
		err = rtnl_valid_dump_net_req(cb->nlh, skb->sk, &net_cb, cb);
		if (err < 0)
			goto end;
	}

	/* IDR 值是 peer 裸指针，必须在 RCU 窗口内完成查找与消息取值。 */
	rcu_read_lock();
	idr_for_each(&net_cb.tgt_net->netns_ids, rtnl_net_dumpid_one, &net_cb);
	rcu_read_unlock();

	/* 无论 skb 是否装满，都保存已扫描条目数供下一轮跳过。 */
	cb->args[0] = net_cb.idx;
end:
	if (net_cb.fillargs.add_ref)
		put_net(net_cb.tgt_net);
	return err;
}

/*
 * rtnl_net_notifyid() - 向 @net 的 RTNLGRP_NSID 发送 NEW/DEL 映射通知。
 * @cmd/@id 是事件快照；@portid/@nlh 标记发起者以避免回送并关联序号，nlh 可空；
 * @gfp 由调用上下文选择分配约束。成功后 skb 所有权转给 rtnl_notify；分配或
 * 填充失败无返回值可上报，因此通过 rtnl_set_sk_err 通知组订阅者并释放 skb。
 */
static void rtnl_net_notifyid(struct net *net, int cmd, int id, u32 portid,
			      struct nlmsghdr *nlh, gfp_t gfp)
{
	struct net_fill_args fillargs = {
		.portid = portid,
		.seq = nlh ? nlh->nlmsg_seq : 0,
		.cmd = cmd,
		.nsid = id,
	};
	struct sk_buff *msg;
	int err = -ENOMEM;

	/* 构造与查询回复相同格式的独立 skb，通知路径不持有 peer 指针。 */
	msg = nlmsg_new(rtnl_net_get_size(), gfp);
	if (!msg)
		goto out;

	err = rtnl_net_fill(msg, &fillargs);
	if (err < 0)
		goto err_out;

	rtnl_notify(msg, net, portid, RTNLGRP_NSID, nlh, gfp);
	return;

err_out:
	/* fill 失败时消息未交付；释放后统一把 errno 写给 multicast 组。 */
	nlmsg_free(msg);
out:
	rtnl_set_sk_err(net, RTNLGRP_NSID, err);
}

#ifdef CONFIG_NET_NS
/*
 * netns_ipv4_struct_check() - 编译期验证 IPv4 热路径只读字段仍位于指定缓存组。
 * 无参数/返回，__init 后可丢弃。CACHELINE_ASSERT_GROUP_MEMBER 若结构布局漂移
 * 会让构建失败，从而阻止无意把 TX/RX 常读 sysctl 移出隔离 cacheline，避免
 * 冷字段写入造成额外共享失效；它不生成运行时检查。
 */
static void __init netns_ipv4_struct_check(void)
{
	/* TX readonly hotpath cache lines */
	/* 下列字段均由发送热路径频繁读取，必须留在 netns_ipv4_read_tx 组。 */
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_tx,
				      sysctl_tcp_early_retrans);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_tx,
				      sysctl_tcp_tso_win_divisor);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_tx,
				      sysctl_tcp_tso_rtt_log);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_tx,
				      sysctl_tcp_autocorking);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_tx,
				      sysctl_tcp_min_snd_mss);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_tx,
				      sysctl_tcp_notsent_lowat);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_tx,
				      sysctl_tcp_limit_output_bytes);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_tx,
				      sysctl_tcp_min_rtt_wlen);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_tx,
				      sysctl_tcp_wmem);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_tx,
				      sysctl_ip_fwd_use_pmtu);

	/* RX readonly hotpath cache line */
	/* 接收侧常读字段集中在独立组，减少与发送侧/可写字段的缓存行竞争。 */
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_rx,
				      sysctl_tcp_moderate_rcvbuf);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_rx,
				      sysctl_tcp_rcvbuf_low_rtt);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_rx,
				      sysctl_ip_early_demux);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_rx,
				      sysctl_tcp_early_demux);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_rx,
				      sysctl_tcp_l3mdev_accept);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_rx,
				      sysctl_tcp_reordering);
	CACHELINE_ASSERT_GROUP_MEMBER(struct netns_ipv4, netns_ipv4_read_rx,
				      sysctl_tcp_rmem);
}
#endif

/*
 * 启动期 rtnetlink 分发表：NEWNSID、GETNSID 处理器声明为 unlocked，表示 rtnl
 * 核心不替它们持全局 RTNL；本文件依靠 nsid_lock、引用和 RCU 自行同步。
 * 表注册后由 rtnetlink 复制/保存，__initconst 原始存储可在启动后回收。
 */
static const struct rtnl_msg_handler net_ns_rtnl_msg_handlers[] __initconst = {
	{.msgtype = RTM_NEWNSID, .doit = rtnl_net_newid,
	 .flags = RTNL_FLAG_DOIT_UNLOCKED},
	{.msgtype = RTM_GETNSID, .doit = rtnl_net_getid,
	 .dumpit = rtnl_net_dumpid,
	 .flags = RTNL_FLAG_DOIT_UNLOCKED | RTNL_FLAG_DUMP_UNLOCKED},
};

/*
 * net_ns_init() - 启动期构造初始网络命名空间并启用全局 netns 基础设施。
 *
 * 无参数、无返回；失败均 panic，因为没有 init_net 网络栈内核无法继续。
 * CONFIG_NET_NS 下先验证布局、创建 slab 和单线程清理队列；随后为静态 init_net
 * 分配 generic 表、preinit、在 pernet_ops_rwsem 写锁下 setup 并标记完成；
 * 最后注册本文件调试 pernet ops 与 rtnetlink handlers。启动上下文可睡眠。
 */
void __init net_ns_init(void)
{
	/* ng 最终由 init_net.gen 持有；init_net 自身不从 net_cachep 分配。 */
	struct net_generic *ng;

#ifdef CONFIG_NET_NS
	/* 动态 net 的 slab 对齐到缓存行；SLAB_ACCOUNT 纳入 memcg，失败直接 panic。 */
	netns_ipv4_struct_check();
	net_cachep = kmem_cache_create("net_namespace", sizeof(struct net),
					SMP_CACHE_BYTES,
					SLAB_PANIC|SLAB_ACCOUNT, NULL);

	/* Create workqueue for cleanup */
	/* 创建专用单线程清理队列，使全局摘除与 unhash_nsid 天然串行。 */
	netns_wq = create_singlethread_workqueue("netns");
	if (!netns_wq)
		panic("Could not create netns workq");
#endif

	/* 初始 net 也使用同一 RCU generic 访问协议，只是外壳为静态存储。 */
	ng = net_alloc_generic();
	if (!ng)
		panic("Could not allocate generic netns");

	rcu_assign_pointer(init_net.gen, ng);

#ifdef CONFIG_KEYS
	init_net.key_domain = &init_net_key_domain;
#endif
	/*
	 * This currently cannot fail as the initial network namespace
	 * has a static inode number.
	 */
	/*
	 * 当前 preinit 不会失败，因为初始 net 使用静态 inode 号；仍保留错误检查，
	 * 使未来 ns_common_init 契约变化时不会带着半初始化 init_net 启动。
	 */
	if (preinit_net(&init_net, &init_user_ns))
		panic("Could not preinitialize the initial network namespace");

	/* 写锁覆盖 setup 和完成标志发布，早期注册者不会观察到中间状态。 */
	down_write(&pernet_ops_rwsem);
	if (setup_net(&init_net))
		panic("Could not setup the initial network namespace");

	init_net_initialized = true;
	up_write(&pernet_ops_rwsem);

	/* 此时通用注册 API 会立刻为 init_net 运行 net_ns_net_init。 */
	if (register_pernet_subsys(&net_ns_ops))
		panic("Could not register network namespace subsystems");

	rtnl_register_many(net_ns_rtnl_msg_handlers);
}

#ifdef CONFIG_NET_NS
/*
 * __register_pernet_operations() - 多 netns 配置下把新 ops 应用于全部既存 net。
 * @list 是 subsystem/device 插入位置，@ops 由调用者长期持有；入口持
 * pernet_ops_rwsem 写锁，故全局 net 集合和 setup/cleanup 都被冻结。成功返回
 * 0：ops 已入链且每个 net 初始化完成；失败返回首个 errno，摘链并只对之前
 * 成功的 net 逆序退出。函数可睡眠。
 */
static int __register_pernet_operations(struct list_head *list,
					struct pernet_operations *ops)
{
	LIST_HEAD(net_exit_list);
	struct net *net;
	int error;

	/* 先入链让初始化顺序成为正式顺序；失败时 out_undo 再撤销可见性。 */
	list_add_tail(&ops->list, list);
	if (ops->init || ops->id) {
		/* We held write locked pernet_ops_rwsem, and parallel
		 * setup_net() and cleanup_net() are not possible.
		 */
		/*
		 * 写锁排除并发 setup/cleanup，所以 for_each_net 得到稳定集合；每成功一个
		 * 就把它加入回滚链，失败点自身由 ops_init 内部回滚。
		 */
		for_each_net(net) {
			error = ops_init(ops, net);
			if (error)
				goto out_undo;
			list_add_tail(&net->exit_list, &net_exit_list);
		}
	}
	return 0;

out_undo:
	/* If I have an error cleanup all namespaces I initialized */
	/* 初始化部分成功：先从未来构造链摘除，再对成功子集运行完整退出协议。 */
	list_del(&ops->list);
	ops_undo_single(ops, &net_exit_list);
	return error;
}

/*
 * __unregister_pernet_operations() - 多 netns 配置下从所有既存 net 撤销 ops。
 * 入口持 pernet_ops_rwsem 写锁，@ops 为调用期间借用。先快照所有 net 到退出链，
 * 再从构造链摘除 ops，最后统一 pre_exit/RCU/exit/free。无返回，函数可睡眠。
 */
static void __unregister_pernet_operations(struct pernet_operations *ops)
{
	LIST_HEAD(net_exit_list);
	struct net *net;

	/* See comment in __register_pernet_operations() */
	/* 同注册路径：写锁使普通全局遍历稳定，无需 RCU 或 net_rwsem。 */
	for_each_net(net)
		list_add_tail(&net->exit_list, &net_exit_list);

	list_del(&ops->list);
	ops_undo_single(ops, &net_exit_list);
}

#else

/*
 * 不支持多 netns 时只有静态 init_net。其 setup 前注册的 ops 仅入链，等待
 * net_ns_init() 一次性按序执行；setup 后注册则立即只初始化 init_net。
 */
static int __register_pernet_operations(struct list_head *list,
					struct pernet_operations *ops)
{
	if (!init_net_initialized) {
		list_add_tail(&ops->list, list);
		return 0;
	}

	return ops_init(ops, &init_net);
}

/*
 * 单 net 配置下，init_net setup 前卸载只需摘链；之后则为 init_net 构造单元素
 * 退出批次并运行完整撤销。入口仍持 pernet_ops_rwsem 写锁，无返回。
 */
static void __unregister_pernet_operations(struct pernet_operations *ops)
{
	if (!init_net_initialized) {
		list_del(&ops->list);
	} else {
		LIST_HEAD(net_exit_list);

		list_add(&init_net.exit_list, &net_exit_list);
		ops_undo_single(ops, &net_exit_list);
	}
}

#endif /* CONFIG_NET_NS */

/* 为申请 generic 私有区的 pernet ops 分配全局唯一、可回收的槽号。 */
static DEFINE_IDA(net_generic_ids);

/*
 * register_pernet_operations() - 分配可选 generic ID 并执行配置相关注册事务。
 * @list/@ops 均由上层在 pernet_ops_rwsem 写锁下传入。id 与 size 必须同时有或
 * 同时无；成功返回 0，ops 已作用于全部既存 net。失败返回 -EINVAL、IDA 或
 * init errno，等待回滚 RCU callback 后归还 ID，调用者可安全释放 ops 代码。
 */
static int register_pernet_operations(struct list_head *list,
				      struct pernet_operations *ops)
{
	int error;

	/* 框架只有同时知道输出 id 槽和每-net 大小时才能代管私有数据。 */
	if (WARN_ON(!!ops->id ^ !!ops->size))
		return -EINVAL;

	if (ops->id) {
		error = ida_alloc_min(&net_generic_ids, MIN_PERNET_OPS_ID,
				GFP_KERNEL);
		if (error < 0)
			return error;
		/* 写回的 ID 是子系统随后调用 net_generic(net, id) 的稳定索引。 */
		*ops->id = error;
		/* This does not require READ_ONCE as writers already hold
		 * pernet_ops_rwsem. But WRITE_ONCE is needed to protect
		 * net_alloc_generic.
		 */
		/*
		 * 写者都受 rwsem 串行化，所以这里自身读取无需 READ_ONCE；但分配新 net 的
		 * net_alloc_generic() 无锁读取该上界，必须用 WRITE_ONCE 发布完整新值。
		 */
		WRITE_ONCE(max_gen_ptrs, max(max_gen_ptrs, *ops->id + 1));
	}
	error = __register_pernet_operations(list, ops);
	if (error) {
		/* 退出回调可能排队使用 ops 私有数据的 RCU callback，归还 ID 前必须排空。 */
		rcu_barrier();
		if (ops->id)
			ida_free(&net_generic_ids, *ops->id);
	}

	return error;
}

/*
 * unregister_pernet_operations() - 从所有 net 撤销 ops 并回收 generic ID。
 * 调用者持写锁。__unregister 已停止入口并运行退出；rcu_barrier 确保所有回调
 * 不再引用模块/槽数据后才 ida_free，防止新注册者过早复用同一 id。无返回。
 */
static void unregister_pernet_operations(struct pernet_operations *ops)
{
	__unregister_pernet_operations(ops);
	rcu_barrier();
	if (ops->id)
		ida_free(&net_generic_ids, *ops->id);
}

/**
 *      register_pernet_subsys - register a network namespace subsystem
 *	@ops:  pernet operations structure for the subsystem
 *
 *	Register a subsystem which has init and exit functions
 *	that are called when network namespaces are created and
 *	destroyed respectively.
 *
 *	When registered all network namespace init functions are
 *	called for every existing network namespace.  Allowing kernel
 *	modules to have a race free view of the set of network namespaces.
 *
 *	When a new network namespace is created all of the init
 *	methods are called in the order in which they were registered.
 *
 *	When a network namespace is destroyed all of the exit methods
 *	are called in the reverse of the order with which they were
 *	registered.
 */
/*
 * register_pernet_subsys() - 注册网络命名空间普通子系统。
 *
 * @ops 是调用者长期保存的回调结构；注册成功到卸载前不得修改/释放。注册时会
 * 为每个既存 net 调 init，使模块得到无竞态的完整 net 集合视图；以后新 net
 * 按注册顺序 init，销毁按逆序 exit。subsystem 插在 first_device 之前，保证
 * 设备类回调依赖的基础设施先构造、后销毁。成功返回 0，失败返回校验/分配或
 * init errno 且已完整回滚。写锁覆盖整个事务，函数可睡眠。
 */
int register_pernet_subsys(struct pernet_operations *ops)
{
	int error;
	down_write(&pernet_ops_rwsem);
	/* first_device 是设备段首节点，把 subsystem 追加到它前面的逻辑子链。 */
	error =  register_pernet_operations(first_device, ops);
	up_write(&pernet_ops_rwsem);
	return error;
}
EXPORT_SYMBOL_GPL(register_pernet_subsys);

/**
 *      unregister_pernet_subsys - unregister a network namespace subsystem
 *	@ops: pernet operations structure to manipulate
 *
 *	Remove the pernet operations structure from the list to be
 *	used when network namespaces are created or destroyed.  In
 *	addition run the exit method for all existing network
 *	namespaces.
 */
/*
 * unregister_pernet_subsys() - 摘除普通子系统并对所有既存 net 运行 exit。
 * @ops 必须是成功注册且尚未卸载的同一对象。写锁阻止新 net 创建、cleanup 和
 * 其他注册变化；内部 RCU barrier 返回后模块可释放回调代码与 ops。无返回，
 * 可睡眠，调用者必须先阻止自身产生新的外部入口。
 */
void unregister_pernet_subsys(struct pernet_operations *ops)
{
	down_write(&pernet_ops_rwsem);
	unregister_pernet_operations(ops);
	up_write(&pernet_ops_rwsem);
}
EXPORT_SYMBOL_GPL(unregister_pernet_subsys);

/**
 *      register_pernet_device - register a network namespace device
 *	@ops:  pernet operations structure for the subsystem
 *
 *	Register a device which has init and exit functions
 *	that are called when network namespaces are created and
 *	destroyed respectively.
 *
 *	When registered all network namespace init functions are
 *	called for every existing network namespace.  Allowing kernel
 *	modules to have a race free view of the set of network namespaces.
 *
 *	When a new network namespace is created all of the init
 *	methods are called in the order in which they were registered.
 *
 *	When a network namespace is destroyed all of the exit methods
 *	are called in the reverse of the order with which they were
 *	registered.
 */
/*
 * register_pernet_device() - 注册依赖普通子系统的 per-net 设备阶段回调。
 *
 * @ops 生命周期和返回约定同 register_pernet_subsys。设备 ops 追加到完整链尾；
 * 首个设备注册时更新 first_device 分界。于是创建时所有 subsystem init 在所有
 * device init 之前，销毁逆序时设备先退出，避免基础设施先被拆掉。
 */
int register_pernet_device(struct pernet_operations *ops)
{
	int error;
	down_write(&pernet_ops_rwsem);
	error = register_pernet_operations(&pernet_list, ops);
	/* 仅首个设备建立分界；后续设备继续追加而不移动段首。 */
	if (!error && (first_device == &pernet_list))
		first_device = &ops->list;
	up_write(&pernet_ops_rwsem);
	return error;
}
EXPORT_SYMBOL_GPL(register_pernet_device);

/**
 *      unregister_pernet_device - unregister a network namespace netdevice
 *	@ops: pernet operations structure to manipulate
 *
 *	Remove the pernet operations structure from the list to be
 *	used when network namespaces are created or destroyed.  In
 *	addition run the exit method for all existing network
 *	namespaces.
 */
/*
 * unregister_pernet_device() - 撤销设备阶段回调并维护 subsystem/device 分界。
 * @ops 必须已注册。若删除当前首设备，先把 first_device 前移到下一节点；随后
 * 对全部 net 逆序退出并等待 RCU callback。无返回，写锁内可睡眠；返回后 ops
 * 和模块代码不再被 netns 框架调用。
 */
void unregister_pernet_device(struct pernet_operations *ops)
{
	down_write(&pernet_ops_rwsem);
	if (&ops->list == first_device)
		first_device = first_device->next;
	unregister_pernet_operations(ops);
	up_write(&pernet_ops_rwsem);
}
EXPORT_SYMBOL_GPL(unregister_pernet_device);

#ifdef CONFIG_NET_NS
/*
 * netns_get() - proc namespace 框架从 @task 取得其当前 net 的 common 引用。
 * @task 由调用者保证存活；task_lock 保护 nsproxy 指针，锁内 get_net 把借用
 * net 转成主动引用。成功返回持有的 &net->ns，nsproxy 已被退出路径清空则
 * 返回 NULL。函数不把 task/nsproxy 指针带出锁保护区。
 */
static struct ns_common *netns_get(struct task_struct *task)
{
	struct net *net = NULL;
	struct nsproxy *nsproxy;

	/* nsproxy 可被 exit/switch 并发替换，必须在 task_lock 内读取并增 net 引用。 */
	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy)
		net = get_net(nsproxy->net_ns);
	task_unlock(task);

	return net ? &net->ns : NULL;
}

/*
 * netns_put() - proc namespace 框架的引用释放回调。
 * @ns 必须来自 netns_get/get_net_ns 的持有引用；转换回 net 后消费该引用。
 * 无返回，最后一个 put 可能异步触发 cleanup，但本函数不直接睡眠清理。
 */
static void netns_put(struct ns_common *ns)
{
	put_net(to_net_ns(ns));
}

/*
 * netns_install() - 为 setns() 把准备中的 nsproxy 切换到目标网络命名空间。
 *
 * @nsset 提供待修改 nsproxy 和调用凭据，调用者拥有该准备对象；@ns 是目标 net
 * 的借用 common 指针。调用者必须同时在目标 net 的 owning user_ns 和自己凭据
 * user_ns 中具备 CAP_SYS_ADMIN，防止跨层级借 setns 获得网络控制能力。
 * 成功返回 0：旧 net 引用已 put、nsproxy 持有目标的新引用；失败 -EPERM 且
 * nsproxy/引用不变。无部分提交。
 */
static int netns_install(struct nsset *nsset, struct ns_common *ns)
{
	struct nsproxy *nsproxy = nsset->nsproxy;
	struct net *net = to_net_ns(ns);

	/* 两个能力检查都必须先完成，保证引用交换是不可失败的单一提交阶段。 */
	if (!ns_capable(net->user_ns, CAP_SYS_ADMIN) ||
	    !ns_capable(nsset->cred->user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	/* nsproxy 对 net 始终恰有一个主动引用：先放旧值，再取得并安装新值。 */
	put_net(nsproxy->net_ns);
	nsproxy->net_ns = get_net(net);
	return 0;
}

/*
 * netns_owner() - 返回网络命名空间的 owning user namespace 借用指针。
 * @ns 在调用期间存活；无引用转移、不睡眠。proc/ns 权限框架用它解释能力边界。
 */
static struct user_namespace *netns_owner(struct ns_common *ns)
{
	return to_net_ns(ns)->user_ns;
}

/*
 * 网络命名空间向 procfs/setns 框架发布的操作表。全局常量生命周期永久；name
 * 决定 /proc/.../ns/net 类型名，get/put 成对管理主动引用，install 执行权限
 * 检查和 nsproxy 交换，owner 暴露 user_ns 能力域。
 */
const struct proc_ns_operations netns_operations = {
	.name		= "net",
	.get		= netns_get,
	.put		= netns_put,
	.install	= netns_install,
	.owner		= netns_owner,
};
#endif
