/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2025 Christian Brauner <brauner@kernel.org> */
/*
 * namespace 全局树索引接口
 *
 * 每种 namespace 有独立 ns_id 树，同时所有类型还进入统一树，并可挂到
 * owning user namespace 的子树。UTS 对象在 clone_uts_ns() 完成构造后加入
 * uts_ns_tree，最后一个普通引用销毁时摘除。写侧由 ns_tree_lock/seqcount
 * 协调，读侧可在 RCU 下遍历，所以类型析构必须延迟释放对象内存。
 */
#ifndef _LINUX_NSTREE_H
#define _LINUX_NSTREE_H

#include <linux/ns/nstree_types.h>
#include <linux/nsproxy.h>
#include <linux/rbtree.h>
#include <linux/seqlock.h>
#include <linux/rculist.h>
#include <linux/cookie.h>
#include <uapi/linux/nsfs.h>

struct ns_common;

extern struct ns_tree_root cgroup_ns_tree;
extern struct ns_tree_root ipc_ns_tree;
extern struct ns_tree_root mnt_ns_tree;
extern struct ns_tree_root net_ns_tree;
extern struct ns_tree_root pid_ns_tree;
extern struct ns_tree_root time_ns_tree;
extern struct ns_tree_root user_ns_tree;
/* UTS namespace 按稳定 64 位 ns_id 组织的每类型根。 */
extern struct ns_tree_root uts_ns_tree;

/*
 * 节点/根初始化与底层增删接口。node_empty 只判断索引状态；node_add
 * 返回冲突节点，node_del 摘除已发布节点。这些 helper 不管理具体对象
 * 普通引用。
 */
void ns_tree_node_init(struct ns_tree_node *node);
void ns_tree_root_init(struct ns_tree_root *root);
bool ns_tree_node_empty(const struct ns_tree_node *node);
struct rb_node *ns_tree_node_add(struct ns_tree_node *node,
				  struct ns_tree_root *root,
				  int (*cmp)(struct rb_node *, const struct rb_node *));
void ns_tree_node_del(struct ns_tree_node *node, struct ns_tree_root *root);

/*
 * _Generic 在编译期按具体 namespace 指针类型选择对应全局树；传入 UTS
 * 指针时得到 &uts_ns_tree。它不计算运行期类型，也不增加对象引用。
 */
#define to_ns_tree(__ns)					\
	_Generic((__ns),					\
		struct cgroup_namespace *: &(cgroup_ns_tree),	\
		struct ipc_namespace *:    &(ipc_ns_tree),	\
		struct net *:              &(net_ns_tree),	\
		struct pid_namespace *:    &(pid_ns_tree),	\
		struct mnt_namespace *:    &(mnt_ns_tree),	\
		struct time_namespace *:   &(time_ns_tree),	\
		struct user_namespace *:   &(user_ns_tree),	\
		struct uts_namespace *:    &(uts_ns_tree))

/* 为具体对象生成 ns_id；初始 namespace 使用固定 ID，动态对象分配新 ID。 */
#define ns_tree_gen_id(__ns)                 \
	__ns_tree_gen_id(to_ns_common(__ns), \
			 (((__ns) == ns_init_ns(__ns)) ? ns_init_id(__ns) : 0))

u64 __ns_tree_gen_id(struct ns_common *ns, u64 id);
void __ns_tree_add_raw(struct ns_common *ns, struct ns_tree_root *ns_tree);
void __ns_tree_remove(struct ns_common *ns, struct ns_tree_root *ns_tree);
struct ns_common *ns_tree_lookup_rcu(u64 ns_id, int ns_type);
struct ns_common *__ns_tree_adjoined_rcu(struct ns_common *ns,
					 struct ns_tree_root *ns_tree,
					 bool previous);

/*
 * __ns_tree_add() - 先确定稳定 ns_id，再把完整对象发布到指定 namespace 树。
 *
 * @ns 与 @ns_tree 是调用者持有的借用对象；@id 非零表示初始固定 ID，零表示
 * 动态生成。函数无返回值；调用者必须保证对象尚未入树，且字段/操作表
 * 已完成。
 */
static inline void __ns_tree_add(struct ns_common *ns, struct ns_tree_root *ns_tree, u64 id)
{
	__ns_tree_gen_id(ns, id);
	__ns_tree_add_raw(ns, ns_tree);
}

/**
 * ns_tree_add_raw - Add a namespace to a namespace
 * @__ns: Namespace to add
 *
 * This function adds a namespace to the appropriate namespace tree
 * without assigning a id.
 */
/*
 * ns_tree_add_raw() 把 namespace 加入由其具体类型选择的树，但不分配 ID。
 * 调用者必须已设置非零 ns_id；该接口主要服务已有外部 ID 的
 * 特殊构造路径。
 */
#define ns_tree_add_raw(__ns) __ns_tree_add_raw(to_ns_common(__ns), to_ns_tree(__ns))

/**
 * ns_tree_add - Add a namespace to a namespace tree
 * @__ns: Namespace to add
 *
 * This function assigns a new id to the namespace and adds it to the
 * appropriate namespace tree and list.
 */
/*
 * ns_tree_add() 为 namespace 分配新 ID（初始对象使用固定 ID），然后把它加入
 * 对应每类型树、统一树、owner 树及相关链表。发布后 RCU 读者可以
 * 发现对象，所以所有可读字段和回收责任必须在调用前就绪。
 */
#define ns_tree_add(__ns)                                   \
	__ns_tree_add(to_ns_common(__ns), to_ns_tree(__ns), \
		      (((__ns) == ns_init_ns(__ns)) ? ns_init_id(__ns) : 0))

/**
 * ns_tree_remove - Remove a namespace from a namespace tree
 * @__ns: Namespace to remove
 *
 * This function removes a namespace from the appropriate namespace
 * tree and list.
 */
/*
 * ns_tree_remove() 从对应每类型树、统一树、owner 树和链表摘除 namespace。
 * 它阻止新遍历者发现对象，但不等待已经开始的 RCU 读者，因此类型
 * 析构仍须使用 kfree_rcu() 或等价宽限期机制完成最终释放。
 */
#define ns_tree_remove(__ns)  __ns_tree_remove(to_ns_common(__ns), to_ns_tree(__ns))

/*
 * 在 RCU 读侧按同类型树查找前后相邻对象；返回值是未增加普通引用的
 * 裸指针，离开 RCU 前若需长期使用，必须按 ns_common 协议转换为
 * 持有引用。
 */
#define ns_tree_adjoined_rcu(__ns, __previous) \
	__ns_tree_adjoined_rcu(to_ns_common(__ns), to_ns_tree(__ns), __previous)

/*
 * 仅检查对象是否已链接到每类型红黑树，不等同于 active 引用计数
 * 大于零。
 */
#define ns_tree_active(__ns) (!RB_EMPTY_NODE(&to_ns_common(__ns)->ns_tree_node.ns_node))

#endif /* _LINUX_NSTREE_H */
