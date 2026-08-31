// SPDX-License-Identifier: GPL-2.0-only
/*
 * HugeTLB sysfs interfaces.
 * (C) Nadia Yvette Chambers, April 2004
 */
/*
 * 本文件实际注册 /proc/sys/vm 下的 HugeTLB sysctl；保留上游标题原文。它只操作
 * default_hstate，而按页大小/NUMA 节点展开的同类控制面位于 hugetlb_sysfs.c。
 */

#include <linux/sysctl.h>

#include "hugetlb_internal.h"

/* 非零时允许 gigantic HugeTLB 页从 ZONE_MOVABLE 分配；热移除可靠性由管理员权衡。 */
int movable_gigantic_pages;

#ifdef CONFIG_SYSCTL
/*
 * proc_hugetlb_doulongvec_minmax() - 让通用 unsigned-long handler 操作临时值。
 *
 * hugetlb 表项的 data 有意为 NULL，真实 hstate 字段还受 HugeTLB 自身锁和调整流程
 * 约束。这里复制只读 table 并把副本 data 指向调用者的 out，避免并发 sysctl 请求
 * 为改 data 而共享写原表。通用 handler 负责 read/write 文本解析、ppos/length 更新
 * 及 min/max 约束，返回 0 或负 errno；out 仅在其语义允许时更新。
 */
static int proc_hugetlb_doulongvec_minmax(const struct ctl_table *table, int write,
					  void *buffer, size_t *length,
					  loff_t *ppos, unsigned long *out)
{
	struct ctl_table dup_table;

	/*
	 * In order to avoid races with __do_proc_doulongvec_minmax(), we
	 * can duplicate the @table and alter the duplicate of it.
	 */
	/* 副本位于当前调用栈，data 重定向不会泄漏到其他读写者。 */
	dup_table = *table;
	dup_table.data = out;

	return proc_doulongvec_minmax(&dup_table, write, buffer, length, ppos);
}

/*
 * hugetlb_sysctl_handler_common() - 读写默认 hstate 的持久 HugeTLB 页目标数。
 *
 * obey_mempolicy 决定写入时是否把 current 的 NUMA 策略转为允许节点；其余 sysctl
 * 参数遵循 proc handler 契约。读操作以 max_huge_pages 快照格式化，不改变池；写
 * 操作先在临时 tmp 完成解析，再调用 __nr_hugepages_store_common() 调整池，后者可
 * 因 gigantic 页不支持运行期调整或分配/释放失败返回错误。无 HugeTLB 支持返回
 * -EOPNOTSUPP；成功读返回 0，成功写返回已消费长度。真正池计数和锁由 core helper
 * 管理，本函数不直接持 hugetlb_lock。
 */
static int hugetlb_sysctl_handler_common(bool obey_mempolicy,
			 const struct ctl_table *table, int write,
			 void *buffer, size_t *length, loff_t *ppos)
{
	/* sysctl 兼容接口只暴露启动时选定的默认页大小。 */
	struct hstate *h = &default_hstate;
	unsigned long tmp = h->max_huge_pages;
	int ret;

	if (!hugepages_supported())
		return -EOPNOTSUPP;

	/* 始终通过 tmp 读写，避免通用 handler 绕过 hstate 的专用提交路径。 */
	ret = proc_hugetlb_doulongvec_minmax(table, write, buffer, length, ppos,
					     &tmp);
	if (ret)
		goto out;

	/* *length 已由 proc handler 更新，成功提交时作为已消费字节数传给 core。 */
	if (write)
		ret = __nr_hugepages_store_common(obey_mempolicy, h,
						  NUMA_NO_NODE, tmp, *length);
out:
	return ret;
}

static int hugetlb_sysctl_handler(const struct ctl_table *table, int write,
			  void *buffer, size_t *length, loff_t *ppos)
{

	/* nr_hugepages 忽略写入任务的 mempolicy，使用所有含内存节点。 */
	return hugetlb_sysctl_handler_common(false, table, write,
							buffer, length, ppos);
}

#ifdef CONFIG_NUMA
static int hugetlb_mempolicy_sysctl_handler(const struct ctl_table *table, int write,
			  void *buffer, size_t *length, loff_t *ppos)
{
	/* NUMA 专用入口只改变节点选择策略，其解析和默认 hstate 语义完全共用。 */
	return hugetlb_sysctl_handler_common(true, table, write,
							buffer, length, ppos);
}
#endif /* CONFIG_NUMA */

/*
 * hugetlb_overcommit_handler() - 读写默认 hstate 可动态超额分配的页数上限。
 *
 * 读路径从 nr_overcommit_huge_pages 取快照；写路径先拒绝不能运行期分配的 gigantic
 * hstate，再解析到 tmp，最后在 hugetlb_lock+关本地中断区间内一次发布。该值不是
 * 立即分配量，而是 persistent pool 之外允许形成的 surplus 页上限。解析/位置状态
 * 由通用 proc handler 处理；不支持 HugeTLB 返回 -EOPNOTSUPP，非法 gigantic 写返回
 * -EINVAL，失败均不改字段。
 */
static int hugetlb_overcommit_handler(const struct ctl_table *table, int write,
		void *buffer, size_t *length, loff_t *ppos)
{
	struct hstate *h = &default_hstate;
	unsigned long tmp;
	int ret;

	if (!hugepages_supported())
		return -EOPNOTSUPP;

	/* 临时副本使错误写入无法部分污染共享上限，也为无锁读提供单值快照。 */
	tmp = h->nr_overcommit_huge_pages;

	if (write && hstate_is_gigantic_no_runtime(h))
		return -EINVAL;

	ret = proc_hugetlb_doulongvec_minmax(table, write, buffer, length, ppos,
					     &tmp);
	if (ret)
		goto out;

	if (write) {
		/* 分配热路径在同一锁下比较 surplus 计数与该上限，故发布必须受锁保护。 */
		spin_lock_irq(&hugetlb_lock);
		h->nr_overcommit_huge_pages = tmp;
		spin_unlock_irq(&hugetlb_lock);
	}
out:
	return ret;
}

/*
 * 所有节点位于 /proc/sys/vm 且 mode 0644：普通用户可读、特权写。data=NULL 的
 * HugeTLB 计数使用专用 handler 间接访问 hstate；简单标量直接交给 proc_dointvec。
 */
static const struct ctl_table hugetlb_table[] = {
	{
		/* 默认页大小的持久池目标；忽略 current mempolicy。 */
		.procname	= "nr_hugepages",
		.data		= NULL,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= hugetlb_sysctl_handler,
	},
#ifdef CONFIG_NUMA
	{
		/* 同一目标数接口，但调整只分布在写入任务 mempolicy 允许的节点。 */
		.procname       = "nr_hugepages_mempolicy",
		.data           = NULL,
		.maxlen         = sizeof(unsigned long),
		.mode           = 0644,
		.proc_handler   = &hugetlb_mempolicy_sysctl_handler,
	},
#endif
	{
		/* 允许创建 SysV SHM HugeTLB 段而无需逐进程 capability 的组 ID。 */
		.procname	= "hugetlb_shm_group",
		.data		= &sysctl_hugetlb_shm_group,
		.maxlen		= sizeof(gid_t),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		/* persistent 池之外可按需创建的 surplus HugeTLB 页数量上限。 */
		.procname	= "nr_overcommit_hugepages",
		.data		= NULL,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= hugetlb_overcommit_handler,
	},
#ifdef CONFIG_ARCH_ENABLE_HUGEPAGE_MIGRATION
	{
		/* 仅支持 gigantic 页迁移的架构才暴露 ZONE_MOVABLE 选择开关。 */
		.procname	= "movable_gigantic_pages",
		.data		= &movable_gigantic_pages,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
#endif
};

void __init hugetlb_sysctl_init(void)
{
	/* init 注册失败由 sysctl 初始化框架处理；表和路径均为静态生命周期。 */
	register_sysctl_init("vm", hugetlb_table);
}
#endif /* CONFIG_SYSCTL */
