/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 通用 namespace 初始化与两级引用内联接口
 *
 * 普通 refcount 决定具体 namespace 结构的存储期；active atomic 记录任务、
 * namespace fd/bind mount 等活跃使用，并沿 owning user namespace 级联。
 * 初始 namespace 永久保持两种计数为 1，动态对象才真实增减。UTS 的
 * get_uts_ns()/put_uts_ns() 最终都落到这里的普通引用 helper。
 */
#ifndef _LINUX_NS_COMMON_H
#define _LINUX_NS_COMMON_H

#include <linux/ns/ns_common_types.h>
#include <linux/refcount.h>
#include <linux/vfsdebug.h>
#include <uapi/linux/sched.h>
#include <uapi/linux/nsfs.h>

/* 通用实现入口；类型宏负责把具体 namespace 指针及静态元数据传入。 */
bool is_current_namespace(struct ns_common *ns);
int __ns_common_init(struct ns_common *ns, u32 ns_type, const struct proc_ns_operations *ops, int inum);
void __ns_common_free(struct ns_common *ns);
struct ns_common *__must_check ns_owner(struct ns_common *ns);

/*
 * is_ns_init_inum() - 依据保留 inode 区间判断对象是否为初始 namespace。
 * @ns 是有效借用指针；inum 为 0 属于未初始化对象并触发警告。
 * 返回布尔值，不增加引用。unlikely 表明运行期绝大多数被检查对象
 * 是动态 namespace。
 */
static __always_inline bool is_ns_init_inum(const struct ns_common *ns)
{
	VFS_WARN_ON_ONCE(ns->inum == 0);
	return unlikely(in_range(ns->inum, MNT_NS_INIT_INO,
				 IPC_NS_INIT_INO - MNT_NS_INIT_INO + 1));
}

/*
 * is_ns_init_id() - 依据保留 ns_id 上界识别永久初始 namespace。
 * @ns 必须已分配非零 ID；动态 ID 大于 NS_LAST_INIT_ID。该判断控制普通与
 * active 引用是否真实增减，误分类会破坏永久对象或泄漏动态对象。
 */
static __always_inline bool is_ns_init_id(const struct ns_common *ns)
{
	VFS_WARN_ON_ONCE(ns->ns_id == 0);
	return ns->ns_id <= NS_LAST_INIT_ID;
}

/*
 * 静态初始化初始 namespace 的 common 部分：固定类型/ID/inode/操作表，
 * 普通与 active 引用均为 1，所有树链表头自指。红黑节点随后由启动期
 * ns_tree_add() 发布；该宏不能用于需要动态 inode/ID 的普通对象。
 */
#define NS_COMMON_INIT(nsname)										\
{													\
	.ns_type			= ns_common_type(&nsname),					\
	.ns_id				= ns_init_id(&nsname),						\
	.inum				= ns_init_inum(&nsname),					\
	.ops				= to_ns_operations(&nsname),					\
	.stashed			= NULL,								\
	.__ns_ref			= REFCOUNT_INIT(1),						\
	.__ns_ref_active		= ATOMIC_INIT(1),						\
	.ns_unified_node.ns_list_entry	= LIST_HEAD_INIT(nsname.ns.ns_unified_node.ns_list_entry),	\
	.ns_tree_node.ns_list_entry	= LIST_HEAD_INIT(nsname.ns.ns_tree_node.ns_list_entry),		\
	.ns_owner_node.ns_list_entry	= LIST_HEAD_INIT(nsname.ns.ns_owner_node.ns_list_entry),	\
	.ns_owner_root.ns_list_head	= LIST_HEAD_INIT(nsname.ns.ns_owner_root.ns_list_head),		\
}

/*
 * 从具体类型自动推导 ns_common、CLONE_NEW* 类型和 operations。初始对象传入
 * 固定 inode，动态对象传 0 让 __ns_common_init() 分配；返回 0 或负 errno。
 */
#define ns_common_init(__ns)                     \
	__ns_common_init(to_ns_common(__ns),     \
			 ns_common_type(__ns),   \
			 to_ns_operations(__ns), \
			 (((__ns) == ns_init_ns(__ns)) ? ns_init_inum(__ns) : 0))

/* 与 ns_common_init() 相同，但由特殊调用者显式指定 inode。 */
#define ns_common_init_inum(__ns, __inum)        \
	__ns_common_init(to_ns_common(__ns),     \
			 ns_common_type(__ns),   \
			 to_ns_operations(__ns), \
			 __inum)

/* 归还 common inode 资源，不释放具体对象内存或类型私有引用。 */
#define ns_common_free(__ns) __ns_common_free(to_ns_common((__ns)))

bool may_see_all_namespaces(void);

/* 原子读取 active 使用数，只提供瞬时值，不固定对象内容或后续状态。 */
static __always_inline __must_check int __ns_ref_active_read(const struct ns_common *ns)
{
	return atomic_read(&ns->__ns_ref_active);
}

/* 读取普通存储期引用数，仅供诊断/不变量检查，不能代替真正 get。 */
static __always_inline __must_check int __ns_ref_read(const struct ns_common *ns)
{
	return refcount_read(&ns->__ns_ref);
}

/*
 * __ns_ref_put() - 释放普通引用并报告动态对象是否应由类型析构。
 *
 * 初始对象保持固定计数并返回 false。动态对象 refcount 降为零时要求 active
 * 已为零，返回 true 让 put_uts_ns() 等调用 free_*；否则返回 false。
 */
static __always_inline __must_check bool __ns_ref_put(struct ns_common *ns)
{
	if (is_ns_init_id(ns)) {
		VFS_WARN_ON_ONCE(__ns_ref_read(ns) != 1);
		VFS_WARN_ON_ONCE(__ns_ref_active_read(ns) != 1);
		return false;
	}
	if (refcount_dec_and_test(&ns->__ns_ref)) {
		VFS_WARN_ON_ONCE(__ns_ref_active_read(ns));
		return true;
	}
	return false;
}

/*
 * __ns_ref_get() - 仅在普通引用尚未归零时安全取得一份引用。
 *
 * 初始对象无需增加固定计数而直接成功；动态对象使用
 * refcount_inc_not_zero()，适合 RCU 查找者避免复活已经进入销毁的对象。
 * 失败返回 false，并在仍有 active 引用时警告生命周期协议错误。
 */
static __always_inline __must_check bool __ns_ref_get(struct ns_common *ns)
{
	if (is_ns_init_id(ns)) {
		VFS_WARN_ON_ONCE(__ns_ref_read(ns) != 1);
		VFS_WARN_ON_ONCE(__ns_ref_active_read(ns) != 1);
		return true;
	}
	if (refcount_inc_not_zero(&ns->__ns_ref))
		return true;
	VFS_WARN_ON_ONCE(__ns_ref_active_read(ns));
	return false;
}

/*
 * __ns_ref_inc() - 为已由调用者稳定持有的动态对象无条件增加普通引用。
 * 初始对象只检查固定计数；调用者若只有可能过期的 RCU 裸指针，
 * 必须改用 __ns_ref_get()，否则与最后一次 put 竞争会触发 refcount
 * 错误。
 */
static __always_inline void __ns_ref_inc(struct ns_common *ns)
{
	if (is_ns_init_id(ns)) {
		VFS_WARN_ON_ONCE(__ns_ref_read(ns) != 1);
		VFS_WARN_ON_ONCE(__ns_ref_active_read(ns) != 1);
		return;
	}
	refcount_inc(&ns->__ns_ref);
}

/*
 * 最后一次普通 put 与取得 @ns_lock 合并成原子协议，供需要在锁内
 * 完成摘除的 namespace 类型使用。初始对象永不降零；返回 true
 * 表示锁已由调用者持有。
 */
static __always_inline __must_check bool __ns_ref_dec_and_lock(struct ns_common *ns,
							       spinlock_t *ns_lock)
{
	if (is_ns_init_id(ns)) {
		VFS_WARN_ON_ONCE(__ns_ref_read(ns) != 1);
		VFS_WARN_ON_ONCE(__ns_ref_active_read(ns) != 1);
		return false;
	}
	return refcount_dec_and_lock(&ns->__ns_ref, ns_lock);
}

/*
 * 面向具体 namespace 指针的空值安全普通引用包装。inc 要求已有稳定引用；
 * get 可从非零计数条件获取；put 返回是否应析构；put_and_lock 合并末次释放
 * 与自旋锁获取。所有权规则与底层 helper 相同。
 */
#define ns_ref_read(__ns) __ns_ref_read(to_ns_common((__ns)))
#define ns_ref_inc(__ns) \
	do { if (__ns) __ns_ref_inc(to_ns_common((__ns))); } while (0)
#define ns_ref_get(__ns) \
	((__ns) ? __ns_ref_get(to_ns_common((__ns))) : false)
#define ns_ref_put(__ns) \
	((__ns) ? __ns_ref_put(to_ns_common((__ns))) : false)
#define ns_ref_put_and_lock(__ns, __ns_lock) \
	((__ns) ? __ns_ref_dec_and_lock(to_ns_common((__ns)), __ns_lock) : false)

/* NULL namespace 的 active 计数按 0 处理。 */
#define ns_ref_active_read(__ns) \
	((__ns) ? __ns_ref_active_read(to_ns_common(__ns)) : 0)

void __ns_ref_active_put(struct ns_common *ns);

/* 空值安全 active put；动态对象归零时可沿 owning user_ns 向上级联。 */
#define ns_ref_active_put(__ns) \
	do { if (__ns) __ns_ref_active_put(to_ns_common(__ns)); } while (0)

/*
 * ns_get_unless_inactive() - 仅为当前仍 active 的对象取得普通引用。
 *
 * @ns 通常来自受 RCU 保护的树查找，不可为空。active 为 0 时返回 NULL；
 * 否则再用 inc-not-zero 获取普通引用，成功返回持有的同一指针。
 * active 引用只用于准入判断，返回后的存储期由新普通引用保证，
 * 字段一致性仍需其他锁。
 */
static __always_inline struct ns_common *__must_check ns_get_unless_inactive(struct ns_common *ns)
{
	if (!__ns_ref_active_read(ns)) {
		VFS_WARN_ON_ONCE(is_ns_init_id(ns));
		return NULL;
	}
	if (!__ns_ref_get(ns))
		return NULL;
	return ns;
}

void __ns_ref_active_get(struct ns_common *ns);

/* 空值安全 active get；0 -> 1 时可沿 owning user_ns 向上级联复活。 */
#define ns_ref_active_get(__ns) \
	do { if (__ns) __ns_ref_active_get(to_ns_common(__ns)); } while (0)

#endif
