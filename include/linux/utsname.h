/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 当前任务 UTS 名称访问与变更通知接口
 *
 * 本头文件把 current->nsproxy->uts_ns 中的 name 快照暴露给 uname、hostname
 * 与 sysctl 实现。对象存储期由 nsproxy/UTS 普通引用保证；字符串内容的
 * 一致快照由 uts_sem 保证。这两个协议不可互换：持有引用不阻止字段
 * 变化，持有 uts_sem 也不能替代 namespace 对象引用。
 */
#ifndef _LINUX_UTSNAME_H
#define _LINUX_UTSNAME_H


#include <linux/sched.h>
#include <linux/nsproxy.h>
#include <linux/ns_common.h>
#include <linux/err.h>
#include <linux/uts_namespace.h>

/*
 * enum uts_proc - /proc/sys/kernel 中可被 UTS 写路径通知的字段类别。
 *
 * ARCH、OSTYPE、OSRELEASE、VERSION 对应体系结构与内核构建身份；HOSTNAME、
 * DOMAINNAME 对应每个 UTS namespace 可修改的节点名和 NIS 域名。枚举值是
 * 内核内部通知索引，不是稳定的用户 ABI 数字。
 */
enum uts_proc {
	UTS_PROC_ARCH,
	UTS_PROC_OSTYPE,
	UTS_PROC_OSRELEASE,
	UTS_PROC_VERSION,
	UTS_PROC_HOSTNAME,
	UTS_PROC_DOMAINNAME,
};

#ifdef CONFIG_PROC_SYSCTL
/*
 * 通知 proc/sys 观察者 @proc 对应的 UTS 项已经变化。调用者通常仍持有
 * uts_sem 写锁，以便通知与刚提交的字符串状态保持顺序；实现不转移
 * 引用。
 */
extern void uts_proc_notify(enum uts_proc proc);
#else
/*
 * 未构建 proc sysctl 时没有观察者需要唤醒。@proc 仍保留类型检查，但桩函数
 * 无副作用、无返回值且不会睡眠，使 sethostname()/setdomainname() 无需条件
 * 编译其核心更新路径。
 */
static inline void uts_proc_notify(enum uts_proc proc)
{
}
#endif

/*
 * utsname() - 返回 current 所在 UTS namespace 的 name 字段。
 *
 * 入参：无。namespace 访问规则保证只有 current 自己切换其 nsproxy，因此
 * 当前任务读取时可直接解引用，无需 task_lock；返回值是借用指针，不增加
 * UTS 引用。调用者若读取或修改字符串，必须按操作类型持有 uts_sem 的读锁
 * 或写锁。返回值不可跨越可能切换 current namespace 的边界长期保存。
 */
static inline struct new_utsname *utsname(void)
{
	return &current->nsproxy->uts_ns->name;
}

/*
 * init_utsname() - 返回永久初始 UTS namespace 的 name 字段。
 *
 * 入参：无；返回借用指针且不增加引用。init_uts_ns 不会销毁，但运行期
 * 仍可能通过初始 namespace 中的 hostname/domainname 写接口修改相应字段，
 * 因此需要一致快照的调用者仍须遵循 uts_sem，而不能把“永久存活”误作
 * “内容只读”。
 */
static inline struct new_utsname *init_utsname(void)
{
	return &init_uts_ns.name;
}

/*
 * 全局 UTS 名称读写信号量。uname/gethostname、克隆 name 快照等路径持读锁；
 * sethostname/setdomainname 与 UTS sysctl 写路径持写锁。它保护字符串内容和
 * 更新原子性，不负责 uts_namespace、nsproxy 或 user_namespace 的存储期。
 */
extern struct rw_semaphore uts_sem;

#endif /* _LINUX_UTSNAME_H */
