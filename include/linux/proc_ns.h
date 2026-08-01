/* SPDX-License-Identifier: GPL-2.0 */
/*
 * procfs namespace bits
 */
/*
 * procfs namespace 接口定义。
 *
 * 本头文件规定具体 namespace 类型如何被 /proc/<pid>/ns、nsfs 与 setns
 * 通用代码访问。操作表把“从任务取得持有引用、释放引用、安装到
 * 事务性 nsset、查询 owning user namespace”抽象出来；UTS 类型由
 * utsns_operations 实现。这里还统一管理 namespace inode 编号接口。
 */
#ifndef _LINUX_PROC_NS_H
#define _LINUX_PROC_NS_H

#include <linux/nsfs.h>
#include <uapi/linux/nsfs.h>

struct pid_namespace;
struct nsset;
struct path;
struct task_struct;
struct inode;

/*
 * struct proc_ns_operations - 具体 namespace 接入 proc/nsfs 的回调契约。
 *
 * @name: proc namespace 项和类型显示名，例如 "uts"。
 * @real_ns_name: 别名条目对应的真实类型名；无别名的类型可为 NULL。
 * @get: 从存活 task 取得 namespace 普通引用；成功返回持有的 ns_common，
 *       任务已退出或无对象时返回 NULL。
 * @put: 释放 @get 或 nsfs 持有的一份普通引用，最后一次可触发类型销毁。
 * @install: 在 setns 验证阶段把目标对象装进私有 nsset；成功返回 0，失败
 *           返回负 errno，不能发布半完成切换。
 * @owner: 返回借用的 owning user namespace，供能力、层级和 ioctl 查询。
 * @get_parent: 取得具有公开父层级类型的 parent 引用；UTS 不提供此回调。
 *
 * 操作表是静态只读对象，必须覆盖其 namespace 对象的整个生命周期。
 */
struct proc_ns_operations {
	const char *name;
	const char *real_ns_name;
	struct ns_common *(*get)(struct task_struct *task);
	void (*put)(struct ns_common *ns);
	int (*install)(struct nsset *nsset, struct ns_common *ns);
	struct user_namespace *(*owner)(struct ns_common *ns);
	struct ns_common *(*get_parent)(struct ns_common *ns);
} __randomize_layout;

extern const struct proc_ns_operations netns_operations;
/* UTS 实现在 kernel/utsname.c，连接 /proc/<pid>/ns/uts 与 setns。 */
extern const struct proc_ns_operations utsns_operations;
extern const struct proc_ns_operations ipcns_operations;
extern const struct proc_ns_operations pidns_operations;
extern const struct proc_ns_operations pidns_for_children_operations;
extern const struct proc_ns_operations userns_operations;
extern const struct proc_ns_operations mntns_operations;
extern const struct proc_ns_operations cgroupns_operations;
extern const struct proc_ns_operations timens_operations;
extern const struct proc_ns_operations timens_for_children_operations;

/*
 * We always define these enumerators
 */
/*
 * 无论对应 namespace 配置是否启用，都定义这些初始 inode 枚举值。稳定的
 * 编译期常量让通用代码和 UAPI 布局无需为每种 CONFIG 组合改变编号表达。
 */
enum {
	PROC_IPC_INIT_INO	= IPC_NS_INIT_INO,
	PROC_UTS_INIT_INO	= UTS_NS_INIT_INO,
	PROC_USER_INIT_INO	= USER_NS_INIT_INO,
	PROC_PID_INIT_INO	= PID_NS_INIT_INO,
	PROC_CGROUP_INIT_INO	= CGROUP_NS_INIT_INO,
	PROC_TIME_INIT_INO	= TIME_NS_INIT_INO,
	PROC_NET_INIT_INO	= NET_NS_INIT_INO,
	PROC_MNT_INIT_INO	= MNT_NS_INIT_INO,
};

#ifdef CONFIG_PROC_FS

/*
 * 为动态 namespace 分配/归还 proc inode 编号。alloc 成功返回 0 并写入
 * @pino，失败返回负 errno；free 消费编号。编号标识 proc/nsfs 对象，
 * 不等同于 namespace tree 的 64 位 ns_id。
 */
extern int proc_alloc_inum(unsigned int *pino);
extern void proc_free_inum(unsigned int inum);

#else /* CONFIG_PROC_FS */

/*
 * 无 procfs 时仍给 ns_common 提供可编译桩：所有动态请求得到占位 inode 1，
 * 释放无操作。namespace 的普通引用、active 引用和 ns_id 协议仍独立存在。
 */
static inline int proc_alloc_inum(unsigned int *inum)
{
	*inum = 1;
	return 0;
}
static inline void proc_free_inum(unsigned int inum) {}

#endif /* CONFIG_PROC_FS */

/*
 * namespace proc inode 的 i_private 保存 struct ns_common 指针。调用者必须
 * 已通过 inode/nsfs 生命周期保证对象存活；该转换不增加任何引用。
 */
#define get_proc_ns(inode) ((struct ns_common *)(inode)->i_private)

#endif /* _LINUX_PROC_NS_H */
