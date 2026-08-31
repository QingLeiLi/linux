/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Internal HugeTLB definitions.
 * (C) Nadia Yvette Chambers, April 2004
 */
/*
 * HugeTLB 子系统内部接口：本头文件连接池扩缩、NUMA 轮转、folio 入池/出池、
 * demote 及 sysfs/sysctl 初始化。它不是通用 MM ABI，调用者须遵守 hugetlb_lock、
 * frozen folio 与 hstate 计数契约；版权信息保留如上。
 */

#ifndef _LINUX_HUGETLB_INTERNAL_H
#define _LINUX_HUGETLB_INTERNAL_H

#include <linux/hugetlb.h>
#include <linux/hugetlb_cgroup.h>

/*
 * Check if the hstate represents gigantic pages but gigantic page
 * runtime support is not available. This is a common condition used to
 * skip operations that cannot be performed on gigantic pages when runtime
 * support is disabled.
 */
/*
 * 当 hstate 表示 gigantic page、但当前配置/架构不支持运行期分配时返回 true。
 * 池调整路径据此跳过不能由 buddy/contig allocator 完成的操作，避免把仅启动期
 * 可建立的 gigantic 页当成普通持久池页处理。
 */
/*
 * hstate_is_gigantic_no_runtime() - 判断 hstate 是否仅支持启动期 gigantic 页。
 * 入参：h 是调用期借用、非 NULL 的 hstate，只读且不取得引用。
 * 出参/返回：两个条件同时成立返回 true，否则 false；无状态副作用。
 * 注意事项：不加锁、不睡眠；结果描述能力而非当前池中是否已有 gigantic folio。
 */
static inline bool hstate_is_gigantic_no_runtime(struct hstate *h)
{
	/* 先判页阶类别，再查询架构与 contig allocator 组合给出的运行期能力。 */
	return hstate_is_gigantic(h) && !gigantic_page_runtime_supported();
}

/*
 * common helper functions for hstate_next_node_to_{alloc|free}.
 * We may have allocated or freed a huge page based on a different
 * nodes_allowed previously, so h->next_node_to_{alloc|free} might
 * be outside of *nodes_allowed.  Ensure that we use an allowed
 * node for alloc or free.
 */
/*
 * 分配/释放轮转游标可能由不同 mempolicy 的前一次操作留下，因而不一定仍在本次
 * nodes_allowed 内；以下 helper 先重新归一化游标，再保持环形公平轮转。
 */
/*
 * next_node_allowed() - 在允许掩码中取得 nid 之后的下一个节点。
 * 入参：nid 是当前节点号；nodes_allowed 是借用的非 NULL 输入掩码且必须非空。
 * 出参/返回：返回环绕后的允许 nid，不修改掩码或 ownership。
 * 注意事项：VM_BUG_ON 捕获空掩码等内部契约破坏；纯位图操作，不睡眠。
 */
static inline int next_node_allowed(int nid, nodemask_t *nodes_allowed)
{
	/* next_node_in() 在掩码末端回绕；返回 MAX_NUMNODES 说明“非空”契约被破坏。 */
	nid = next_node_in(nid, *nodes_allowed);
	VM_BUG_ON(nid >= MAX_NUMNODES);

	return nid;
}

/*
 * get_valid_node_allowed() - 把可能过期的轮转游标校正到允许节点。
 * 入参：nid 是候选节点；nodes_allowed 是借用、非空且只读的 NUMA 掩码。
 * 出参/返回：候选仍允许时原样返回，否则返回其后的首个允许节点；无副作用。
 * 注意事项：不验证 nid 之外的拓扑状态；实际分配仍可能因内存不足失败。
 */
static inline int get_valid_node_allowed(int nid, nodemask_t *nodes_allowed)
{
	/* 快速路径沿用仍有效游标；慢速路径只向前移动一次到掩码中的下一位。 */
	if (!node_isset(nid, *nodes_allowed))
		nid = next_node_allowed(nid, nodes_allowed);
	return nid;
}

/*
 * returns the previously saved node ["this node"] from which to
 * allocate a persistent huge page for the pool and advance the
 * next node from which to allocate, handling wrap at end of node
 * mask.
 */
/*
 * 返回本轮应尝试分配持久 HugeTLB 页的节点，并把保存游标推进到下一个允许节点；
 * 到掩码末尾时自动回绕，使重复扩池在允许节点间近似均衡。
 */
/*
 * hstate_next_node_to_alloc() - 领取一个分配节点并推进调用者游标。
 * 入参：next_node 是非 NULL 输入输出游标指针；nodes_allowed 是借用的非空掩码。
 * 出参/返回：返回本轮 nid，并写回下一轮 nid；不分配页、不取得节点引用。
 * 注意事项：调用者负责串行化同一游标；函数不加锁、不睡眠；next_node 不可为空，
 * nodes_allowed 为空会触发 VM_BUG_ON，掩码为空会在下层范围断言中暴露。
 */
static inline int hstate_next_node_to_alloc(int *next_node,
					    nodemask_t *nodes_allowed)
{
	/* nid 是本次返回节点，*next_node 则在返回前发布为下一次轮转起点。 */
	int nid;

	VM_BUG_ON(!nodes_allowed);

	nid = get_valid_node_allowed(*next_node, nodes_allowed);
	*next_node = next_node_allowed(nid, nodes_allowed);

	return nid;
}

/*
 * helper for remove_pool_hugetlb_folio() - return the previously saved
 * node ["this node"] from which to free a huge page.  Advance the
 * next node id whether or not we find a free huge page to free so
 * that the next attempt to free addresses the next node.
 */
/*
 * remove_pool_hugetlb_folio() 用它领取本轮释放节点；无论该节点最终是否找到空闲
 * huge folio 都推进游标，避免一个空节点永久阻塞后续节点的缩池尝试。
 */
/*
 * hstate_next_node_to_free() - 领取释放节点并推进 hstate 的持久游标。
 * 入参：h 是非 NULL 输入输出 hstate；nodes_allowed 是借用的非空只读掩码。
 * 出参/返回：返回本轮允许 nid，并更新 h->next_nid_to_free；无直接页表/folio 副作用。
 * 注意事项：调用者通常持 hugetlb_lock 来保护共享游标；本 inline 自身不加锁、不睡眠。
 */
static inline int hstate_next_node_to_free(struct hstate *h, nodemask_t *nodes_allowed)
{
	/* nid 是本轮查找 freelist 的节点；共享游标的写回是唯一外部状态变化。 */
	int nid;

	VM_BUG_ON(!nodes_allowed);

	nid = get_valid_node_allowed(h->next_nid_to_free, nodes_allowed);
	h->next_nid_to_free = next_node_allowed(nid, nodes_allowed);

	return nid;
}

/*
 * 两个宏把允许节点权重快照为 nr_nodes，并且恰好迭代该次数；表达式中的 “|| 1”
 * 使 node==0 仍被视为 for 条件成功。参数会被多次求值，只能传稳定变量/指针，
 * 循环体可提前 break；alloc 版更新外部 *next_node，free 版更新 hs 的释放游标。
 */
#define for_each_node_mask_to_alloc(next_node, nr_nodes, node, mask)		\
	for (nr_nodes = nodes_weight(*mask);				\
		nr_nodes > 0 &&						\
		((node = hstate_next_node_to_alloc(next_node, mask)) || 1);	\
		nr_nodes--)

#define for_each_node_mask_to_free(hs, nr_nodes, node, mask)		\
	for (nr_nodes = nodes_weight(*mask);				\
		nr_nodes > 0 &&						\
		((node = hstate_next_node_to_free(hs, mask)) || 1);	\
		nr_nodes--)

/*
 * remove_hugetlb_folio() - 在持有 hugetlb_lock 时从池和计数中摘除 folio。
 * 业务背景：缩池、释放或 demote 路径先摘除空闲页，随后恢复 vmemmap 或拆分/归还 buddy。
 * 入参：h/folio 均为借用且非 NULL；adjust_surplus 指示同步扣减 surplus 计数。
 * 返回：无；folio 仍由调用者持有，后续负责恢复 vmemmap、释放或重新入池。
 * 注意事项：可能因 gigantic 无运行期支持而无操作；调用者必须持 hugetlb_lock。
 */
extern void remove_hugetlb_folio(struct hstate *h, struct folio *folio,
				 bool adjust_surplus);
/*
 * add_hugetlb_folio() - 把已准备 folio 加回指定 hstate 池并更新计数。
 * 业务背景：demote/restore 失败回滚把摘下的页重新发布，恢复池不变量后调用者继续轮转。
 * 入参：h/folio 为借用输入输出对象，folio 必须 frozen 且 HVO 标志已置；
 * adjust_surplus 决定是否增加 surplus 账目。
 * 返回：无；成功后池接管空闲 folio 的链表归属。注意事项：须遵守 hugetlb_lock 契约。
 */
extern void add_hugetlb_folio(struct hstate *h, struct folio *folio,
			      bool adjust_surplus);
/*
 * init_new_hugetlb_folio() - 初始化一个尚未发布的新 HugeTLB folio 私有状态。
 * 业务背景：buddy/CMA/bootmem 得到 frozen folio 后调用，下一步通常进行 HVO 和池计账。
 * 入参：folio 为调用者独占的 frozen folio；返回无，folio ownership 不转移。
 * 注意事项：必须早于计数和 freelist 发布，不提供失败返回，也不负责底层页分配。
 */
extern void init_new_hugetlb_folio(struct folio *folio);
/*
 * prep_and_add_allocated_folios() - 批量优化 vmemmap 后把新 folio 计账并发布入池。
 * 业务背景：批量扩池完成物理分配后调用，把昂贵 HVO 放在锁外、发布集中在一次锁周期。
 * 入参：h 为目标 hstate；folio_list 为调用者拥有的 frozen folio 输入输出链表。
 * 返回：无；成功后各 folio 从输入链表转入 HugeTLB freelist，池取得空闲链表归属。
 * 注意事项：HVO 尽力失败不阻止入池；内部一次取得 hugetlb_lock，调用者入口不可持该锁。
 */
extern void prep_and_add_allocated_folios(struct hstate *h,
					  struct list_head *folio_list);
/*
 * demote_pool_huge_page() - 把源池空闲大页拆成较小 hstate 页。
 * 业务背景：sysfs demote 写路径持锁调用，成功后调用者按实际拆分数更新用户可见结果。
 * 入参：src 为输入输出池；nodes_allowed 为借用掩码；nr_to_demote 为目标 folio 数。
 * 返回：正数为实际完成数；无可用非毒化页返回 -EBUSY，demote_order 缺失返回 -EINVAL，
 * 也可透传拆分页的负错误。失败页会重新加入 src，成功页转入目标池且无法在此回滚。
 * 注意事项：入口和返回均持 hugetlb_lock，内部会暂时释放再取得；期间池状态可变化。
 */
extern long demote_pool_huge_page(struct hstate *src,
				  nodemask_t *nodes_allowed,
				  unsigned long nr_to_demote);
/*
 * __nr_hugepages_store_common() - 实现 sysfs 写入持久池目标数量的公共核心。
 * 业务背景：全局/节点 nr_hugepages 属性解析出 count 后调用，返回值直接成为 sysfs store 结果。
 * 入参：obey_mempolicy 选择 current 策略；h/nid 定位池；count 是目标页数；len 是写入长度。
 * 返回：成功返回 len；不支持运行期 gigantic 页为 -EINVAL，或透传扩缩池错误。
 * 注意事项：可能分配/释放并睡眠；部分池变化是否保留由 set_max_huge_pages() 契约决定。
 */
extern ssize_t __nr_hugepages_store_common(bool obey_mempolicy,
					   struct hstate *h, int nid,
					   unsigned long count, size_t len);

/*
 * hugetlb_sysfs_init() - 启动期发布 HugeTLB hstate/node sysfs 控制面。
 * 业务背景：hugetlb_init() 完成 hstate 建立后调用，后续用户空间由这些属性观察/调池。
 * 入参无；返回无，失败按实现记录且无错误可返给调用者。
 * 注意事项：仅 __init，可睡眠且只调用一次；发布后 sysfs core 持有相关对象。
 */
extern void hugetlb_sysfs_init(void) __init;

#ifdef CONFIG_SYSCTL
/*
 * hugetlb_sysctl_init() - 注册启用配置下的 HugeTLB sysctl 表。
 * 业务背景：启动期在 HugeTLB 基础状态就绪后调用，向用户空间发布 sysctl 控制面。
 * 入参无；返回无，注册失败只能由实现处理；初始化后 sysctl core 持有表项。
 * 注意事项：启动上下文可睡眠且仅应调用一次，不能作为运行期重复注册接口。
 */
extern void hugetlb_sysctl_init(void);
#else
/*
 * hugetlb_sysctl_init() - CONFIG_SYSCTL=n 时保持调用点无条件化的空桩。
 * 业务背景：hugetlb_init() 无需因配置拆成两套调用顺序；编译器会消除此函数。
 * 入参无；无返回值、无副作用且不睡眠，调用后未创建任何控制面。
 * 注意事项：与启用分支同名但不提供可观察注册结果，调用者不得假设 sysctl 存在。
 */
static inline void hugetlb_sysctl_init(void) { }
#endif

/* 结束 HugeTLB 内部声明的防重复包含范围。 */
#endif /* _LINUX_HUGETLB_INTERNAL_H */
