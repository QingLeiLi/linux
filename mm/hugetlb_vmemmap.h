// SPDX-License-Identifier: GPL-2.0
/*
 * HugeTLB Vmemmap Optimization (HVO)
 *
 * Copyright (c) 2020, ByteDance. All rights reserved.
 *
 *     Author: Muchun Song <songmuchun@bytedance.com>
 */
/*
 * HugeTLB vmemmap 优化（HVO）把一个 huge folio 大量、内容重复的尾 struct page
 * 映射到共享 backing page，并把原 backing pages 归还 buddy；HugeTLB 页要拆回
 * buddy 前必须先恢复这些元数据。本文档实现契约见 Documentation/mm/vmemmap_dedup.rst。
 */
#ifndef _LINUX_HUGETLB_VMEMMAP_H
#define _LINUX_HUGETLB_VMEMMAP_H
#include <linux/hugetlb.h>
#include <linux/io.h>
#include <linux/memblock.h>

/*
 * Reserve one vmemmap page, all vmemmap addresses are mapped to it. See
 * Documentation/mm/vmemmap_dedup.rst.
 */
/*
 * 每个优化后的 huge folio 仍保留一页独立 vmemmap，覆盖头页及必要的尾页描述；
 * 其余 vmemmap 虚拟地址可复用共享尾模板。RESERVE_PAGES 是该页能容纳的
 * struct page 数，二者决定可释放大小与恢复起点。
 */
#define HUGETLB_VMEMMAP_RESERVE_SIZE	PAGE_SIZE
#define HUGETLB_VMEMMAP_RESERVE_PAGES	(HUGETLB_VMEMMAP_RESERVE_SIZE / sizeof(struct page))

#ifdef CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP
/*
 * hugetlb_vmemmap_restore_folio() - 为单个 HVO folio 重新分配并映射完整 vmemmap。
 * 业务背景：HugeTLB 页离开池并准备交还 buddy 前调用，使所有尾 struct page 可独立写。
 * 入参：h/folio 为借用且非 NULL，folio 必须 frozen、属于 h；返回 0 或负 errno。
 * 返回/副作用：0 包括本来未优化和恢复成功；失败返回 vmemmap_remap_alloc() 的负 errno。
 * 成功清 optimized 标志，folio ownership 不变；可能分配/睡眠，须早于清 hugetlb 标志。
 */
int hugetlb_vmemmap_restore_folio(const struct hstate *h, struct folio *folio);
/*
 * hugetlb_vmemmap_restore_folios() - 批量恢复列表并分离已具备完整 vmemmap 的 folio。
 * 业务背景：批量释放 HugeTLB 页时合并 TLB flush，随后调用者只处理输出链表中的安全项。
 * 入参：h 借用；folio_list 为输入输出待处理链表；non_hvo_folios 为已初始化输出链表。
 * 返回恢复数量或首个负 errno；成功项移动到输出，错误项及未处理项留在输入列表。
 * 注意事项：批量延迟 TLB flush，列表 ownership 仍归调用者，可能分配并睡眠。
 */
long hugetlb_vmemmap_restore_folios(const struct hstate *h,
					struct list_head *folio_list,
					struct list_head *non_hvo_folios);
/*
 * hugetlb_vmemmap_optimize_folio() - 尽力优化单个 frozen HugeTLB folio 的 vmemmap。
 * 业务背景：alloc_fresh_hugetlb_folio() 在新页发布入池前调用，用元数据内存换取容量。
 * 入参：h/folio 均借用；无返回值，是否成功由 optimized 标志判断，失败保持可用原状。
 * 注意事项：可能分配/睡眠并释放 vmemmap backing pages，不转移 huge folio ownership。
 */
void hugetlb_vmemmap_optimize_folio(const struct hstate *h, struct folio *folio);
/*
 * hugetlb_vmemmap_optimize_folios() - 批量优化运行期新分配 folio 并合并 TLB flush。
 * 业务背景：prep_and_add_allocated_folios() 在取得 hugetlb_lock 和入池发布前调用。
 * 入参：h 为目标 hstate；folio_list 为借用输入链表；返回无，链表成员不移动。
 * 注意事项：逐项尽力而为，失败项保持未优化；可能睡眠，调用者之后再统一入池。
 */
void hugetlb_vmemmap_optimize_folios(struct hstate *h, struct list_head *folio_list);
/*
 * hugetlb_vmemmap_optimize_bootmem_folios() - 处理启动期 gigantic folio 与 pre-HVO 状态。
 * 业务背景：HugeTLB 启动初始化把 bootmem folio 转入正式池之前调用一次。
 * 入参：h/list 均借用；返回无，可能把重复 vmemmap backing pages 归还启动/buddy 路径。
 * 注意事项：仅用于启动转换，识别已 pre-HVO 项并补只读映射/bootmem 登记。
 */
void hugetlb_vmemmap_optimize_bootmem_folios(struct hstate *h, struct list_head *folio_list);
#ifdef CONFIG_SPARSEMEM_VMEMMAP_PREINIT
/*
 * hugetlb_vmemmap_init_early() - 在 sparse vmemmap 建立前标记 nid 上可 pre-HVO 的 section。
 * 业务背景：节点 vmemmap populate 前扫描 huge_boot_pages，为后续跳过重复 backing 做准备。
 * 入参：nid 为节点号；返回无；仅 __init 串行阶段使用，不取得 huge_boot_pages ownership。
 */
void hugetlb_vmemmap_init_early(int nid);
/*
 * hugetlb_vmemmap_init_late() - 页和 zone 就绪后完成 nid 上 pre-HVO 映射与元数据转换。
 * 业务背景：对应 early 阶段的收尾；完成后 bootmem folio 才能按 HVO 状态进入正式初始化。
 * 入参：nid 为节点号；返回无；仅 __init，必须晚于 early 标记和 sparsemem 初始化。
 */
void hugetlb_vmemmap_init_late(int nid);
#endif


/*
 * hugetlb_vmemmap_size() - 计算一个 HugeTLB folio 全部 struct page 元数据字节数。
 * 入参：h 为借用、非 NULL hstate；返回 pages_per_huge_page(h)*sizeof(struct page)。
 * 纯算术、无副作用且不睡眠；结果未扣除必须保留的一页。
 */
static inline unsigned int hugetlb_vmemmap_size(const struct hstate *h)
{
	/* 页数乘单个描述符大小；调用者再据此构造 vmemmap 半开地址区间。 */
	return pages_per_huge_page(h) * sizeof(struct page);
}

/*
 * Return how many vmemmap size associated with a HugeTLB page that can be
 * optimized and can be freed to the buddy allocator.
 */
/*
 * 返回一个 HugeTLB folio 可去重并归还 buddy 的 vmemmap 字节数。只有 struct page
 * 大小为 2 的幂时映射布局才满足算法假设；总元数据不超过保留页时也返回 0。
 */
/*
 * hugetlb_vmemmap_optimizable_size() - 计算当前 hstate 的 HVO 可释放容量。
 * 入参：h 是借用只读 hstate；返回非负字节数，0 表示布局不支持/无收益。
 * 无状态副作用、不睡眠；能力非零不代表运行期开关当前已启用或某 folio 已优化。
 */
static inline unsigned int hugetlb_vmemmap_optimizable_size(const struct hstate *h)
{
	/* size 是扣除首个保留页后的候选可释放字节数，负值通过最终分支归零。 */
	int size = hugetlb_vmemmap_size(h) - HUGETLB_VMEMMAP_RESERVE_SIZE;

	/* 非 2 次幂描述符无法让共享页映射按算法要求周期重复，直接走无能力快路径。 */
	if (!is_power_of_2(sizeof(struct page)))
		return 0;
	return size > 0 ? size : 0;
}
#else
/*
 * CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP=n：恢复单 folio 的中性桩。
 * 业务背景：HugeTLB 释放路径可无条件调用；下游直接按“已有完整 vmemmap”继续。
 * 入参：h/folio 是借用参数，仅为保持同一接口且函数体不读取。
 * 返回/副作用：返回 0 表示无需恢复，无映射、标志、ownership 变化，也不睡眠。
 * 注意事项：0 不表示执行过分配，而表示关闭配置下恢复天然满足。
 */
static inline int hugetlb_vmemmap_restore_folio(const struct hstate *h, struct folio *folio)
{
	return 0;
}

/*
 * 禁用配置下批量 folio 从输入整体移动到 non_hvo_folios，因为它们本来就有完整
 * vmemmap。业务背景：批量释放者之后只消费输出表。入参 h 借用且不读取，两个链表
 * 均为非 NULL 输入输出；返回 0、无分配，splice 后输入为空、成员 ownership 不变。
 * 注意事项：调用者必须初始化输出表；list_splice_init() 改变的是链表归属而非 folio 引用。
 */
static inline long hugetlb_vmemmap_restore_folios(const struct hstate *h,
					struct list_head *folio_list,
					struct list_head *non_hvo_folios)
{
	list_splice_init(folio_list, non_hvo_folios);
	return 0;
}

/*
 * hugetlb_vmemmap_optimize_folio() - 禁用 HVO 时保留新 folio 发布前的统一调用点。
 * 入参 h/folio 均为借用且不读取；返回 void，无映射、标志、ownership 变化且不睡眠。
 * 注意事项：调用后 folio 保持完整 vmemmap，后续可直接入池。
 */
static inline void hugetlb_vmemmap_optimize_folio(const struct hstate *h, struct folio *folio)
{
}

/*
 * hugetlb_vmemmap_optimize_folios() - 禁用 HVO 时的运行期批量优化空桩。
 * 业务背景：入池代码保持统一阶段；h/list 为借用且不读取，返回 void。
 * 返回/副作用：不移动成员、不改引用或标志、不睡眠；调用者随后仍负责发布整表。
 */
static inline void hugetlb_vmemmap_optimize_folios(struct hstate *h, struct list_head *folio_list)
{
}

/*
 * hugetlb_vmemmap_optimize_bootmem_folios() - 禁用 HVO 时的 bootmem 转换空桩。
 * 入参 h/list 为借用启动期对象；返回 void，不移动成员、不修改启动页/HVO 状态。
 * 注意事项：只保留调用顺序，正式 HugeTLB 初始化仍由调用者继续完成。
 */
static inline void hugetlb_vmemmap_optimize_bootmem_folios(struct hstate *h,
						struct list_head *folio_list)
{
}

/*
 * hugetlb_vmemmap_init_early() - 禁用 HVO 时的节点 preinit 空桩。
 * 入参 nid 是节点号且函数体不读取；返回 void，无 section 标记、分配或睡眠。
 * 注意事项：后续 late 调用同样为空，不能据此推断节点执行过 pre-HVO。
 */
static inline void hugetlb_vmemmap_init_early(int nid)
{
}

/*
 * hugetlb_vmemmap_init_late() - 禁用 HVO 时的节点收尾空桩。
 * 入参 nid 是节点号且不读取；返回 void，不建立映射、不释放 bootmem 元数据。
 * 注意事项：调用者仍按普通 vmemmap 路径完成节点初始化。
 */
static inline void hugetlb_vmemmap_init_late(int nid)
{
}

/*
 * hugetlb_vmemmap_optimizable_size() - 禁用 HVO 时报告零可释放容量。
 * 入参 h 为借用且不读取；返回恒 0，无副作用、不睡眠、无 ownership 变化。
 * 注意事项：上层 optimizable() 因此稳定返回 false，避免进入任何 HVO 路径。
 */
static inline unsigned int hugetlb_vmemmap_optimizable_size(const struct hstate *h)
{
	return 0;
}
#endif /* CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP */

/*
 * hugetlb_vmemmap_optimizable() - 把可释放字节数转换为能力布尔值。
 * 入参：h 为借用只读 hstate；返回 true 表示布局有可去重空间，否则 false。
 * 纯查询、无副作用；配置关闭时经上方桩自然为 false。
 */
static inline bool hugetlb_vmemmap_optimizable(const struct hstate *h)
{
	return hugetlb_vmemmap_optimizable_size(h) != 0;
}

/* 结束 HVO 配置接口的防重复包含范围。 */
#endif /* _LINUX_HUGETLB_VMEMMAP_H */
