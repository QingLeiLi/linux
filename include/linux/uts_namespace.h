/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UTS namespace 对象与引用接口
 *
 * 本头文件定义 UTS namespace 的内存布局，以及 kernel/utsname.c 与
 * nsproxy/proc/nsfs 调用者共享的普通引用协议。name 是用户可观察的 UTS
 * 字符串快照；user_ns 决定能力边界；ucounts 记录创建配额；ns_common
 * 把对象接入通用 namespace inode、操作表、树索引与 RCU 生命周期。
 *
 * CONFIG_UTS_NS=y 时，任务可共享或克隆这些对象，最后一个普通引用负责
 * 触发销毁。关闭配置时不存在动态 UTS namespace：引用操作退化为空操作，
 * 未请求隔离时始终共享旧对象，请求 CLONE_NEWUTS 则返回 -EINVAL。
 */
#ifndef _LINUX_UTS_NAMESPACE_H
#define _LINUX_UTS_NAMESPACE_H

#include <linux/ns_common.h>
#include <uapi/linux/utsname.h>

struct user_namespace;
extern struct user_namespace init_user_ns;

/*
 * struct uts_namespace - 一组可被任务共享或隔离的 UTS 身份。
 *
 * @name: 用户通过 uname()/gethostname() 等接口观察的六个定长字符串；
 *        复制和运行期读写由全局 uts_sem 串行化。
 * @user_ns: owning user namespace 的持有引用，决定 CAP_SYS_ADMIN 检查边界；
 *           动态对象在创建时 get，在 free_uts_ns() 中 put。
 * @ucounts: 创建者有效 UID 对应的分层配额引用；仅动态对象使用，销毁时
 *           对称递减 UCOUNT_UTS_NAMESPACES。
 * @ns: 通用 namespace 元数据，普通引用保证本结构存活，active 引用控制
 *      用户可见性，并复用其中的 RCU 节点完成延迟释放。
 *
 * __randomize_layout 允许构建时随机排列字段，以增加针对固定偏移攻击的
 * 难度；代码必须通过字段名或 container_of() 访问，不能依赖手工计算的
 * 固定布局。
 */
struct uts_namespace {
	struct new_utsname name;
	struct user_namespace *user_ns;
	struct ucounts *ucounts;
	struct ns_common ns;
} __randomize_layout;

/*
 * 启动时静态构造、永久存活的初始 UTS namespace。它具有固定 namespace
 * ID/inode 和固定普通/active 引用，不参与动态 ucounts 计费。
 */
extern struct uts_namespace init_uts_ns;

#ifdef CONFIG_UTS_NS
/*
 * to_uts_ns() - 从嵌入的通用头恢复外层 UTS namespace。
 *
 * @ns 必须确实指向 struct uts_namespace::ns，借用且不可为空。返回外层对象
 * 的借用指针，不改变普通或 active 引用，也不获取 uts_sem、不会睡眠。
 */
static inline struct uts_namespace *to_uts_ns(struct ns_common *ns)
{
	return container_of(ns, struct uts_namespace, ns);
}

/*
 * get_uts_ns() - 为 UTS namespace 增加一份普通存储期引用。
 *
 * @ns 可为空，ns_ref_inc() 对 NULL 无操作；非空对象必须已有有效引用，
 * 不能用它复活普通引用已经归零的对象。无返回值，不锁定 name 字段，
 * 也不代表 active 使用；调用者必须以 put_uts_ns() 对称释放。
 */
static inline void get_uts_ns(struct uts_namespace *ns)
{
	ns_ref_inc(ns);
}

/*
 * 动态 UTS namespace 生命周期入口：copy_utsname() 为新 nsproxy 返回一份
 * 普通引用；free_uts_ns() 仅由最后一次 put 触发；uts_ns_init() 在启动期
 * 创建 slab cache 并登记 init_uts_ns。
 */
extern struct uts_namespace *copy_utsname(u64 flags,
	struct user_namespace *user_ns, struct uts_namespace *old_ns);
extern void free_uts_ns(struct uts_namespace *ns);

/*
 * put_uts_ns() - 释放一份普通 UTS namespace 引用。
 *
 * @ns 可为空；非空时必须对应先前持有的引用。普通引用降为零意味着对象
 * 没有存储期持有者，ns_common 协议还要求 active 引用已经为零；此时
 * 同步进入 free_uts_ns() 摘除索引并安排 RCU 释放。调用后不得再访问
 * 原借用指针。
 */
static inline void put_uts_ns(struct uts_namespace *ns)
{
	if (ns_ref_put(ns))
		free_uts_ns(ns);
}

void uts_ns_init(void);
#else
/*
 * 未启用 UTS namespace 时只有永久存活的初始对象，因此普通引用无需计数。
 * 两个桩函数均不读取 @ns、无副作用，也不会睡眠。
 */
static inline void get_uts_ns(struct uts_namespace *ns)
{
}

static inline void put_uts_ns(struct uts_namespace *ns)
{
}

/*
 * copy_utsname() - CONFIG_UTS_NS=n 下验证隔离请求并共享旧对象。
 *
 * @flags 只检查 CLONE_NEWUTS；@user_ns 在此配置下不使用；@old_ns 是永久
 * 初始对象的借用指针。请求新 UTS namespace 返回 ERR_PTR(-EINVAL)，否则
 * 原样返回 @old_ns。由于初始对象不会销毁，返回值无需新增普通引用。
 */
static inline struct uts_namespace *copy_utsname(u64 flags,
	struct user_namespace *user_ns, struct uts_namespace *old_ns)
{
	if (flags & CLONE_NEWUTS)
		return ERR_PTR(-EINVAL);

	return old_ns;
}

/*
 * 无动态 UTS 对象时不需要 slab cache 或 namespace 树登记步骤。
 * 入参：无；返回：无直接返回值；无副作用且不会睡眠。
 */
static inline void uts_ns_init(void)
{
}
#endif

#endif /* _LINUX_UTS_NAMESPACE_H */
