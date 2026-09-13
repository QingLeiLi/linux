// SPDX-License-Identifier: GPL-2.0
/*
 * HugeTLB Vmemmap Optimization (HVO)
 *
 * Copyright (c) 2020, ByteDance. All rights reserved.
 *
 *     Author: Muchun Song <songmuchun@bytedance.com>
 *
 * See Documentation/mm/vmemmap_dedup.rst
 */
#define pr_fmt(fmt)	"HugeTLB: " fmt

#include <linux/pgtable.h>
#include <linux/moduleparam.h>
#include <linux/bootmem_info.h>
#include <linux/mmdebug.h>
#include <linux/pagewalk.h>
#include <linux/pgalloc.h>

#include <asm/tlbflush.h>
#include "hugetlb_vmemmap.h"
#include "internal.h"

/* HVO 的核心对象是 struct page 所在的 vmemmap 映射：保留可写 head，tail 复用只读模板并回收旧 backing 页。 */

/**
 * struct vmemmap_remap_walk - walk vmemmap page table
 *
 * @remap_pte:		called for each lowest-level entry (PTE).
 * @nr_walked:		the number of walked pte.
 * @vmemmap_head:	the page to be installed as first in the vmemmap range
 * @vmemmap_tail:	the page to be installed as non-first in the vmemmap range
 * @vmemmap_pages:	the list head of the vmemmap pages that can be freed
 *			or is mapped from.
 * @flags:		used to modify behavior in vmemmap page table walking
 *			operations.
 */
struct vmemmap_remap_walk {
	/* 回调决定本次 walk 是去重替换还是失败恢复；其余字段在整个 mmap-read 临界区内借用。 */
	void			(*remap_pte)(pte_t *pte, unsigned long addr,
					     struct vmemmap_remap_walk *walk);

	unsigned long		nr_walked;
	struct page		*vmemmap_head;
	struct page		*vmemmap_tail;
	struct list_head	*vmemmap_pages;


/* Skip the TLB flush when we split the PMD */
#define VMEMMAP_SPLIT_NO_TLB_FLUSH	BIT(0)
/* Skip the TLB flush when we remap the PTE */
#define VMEMMAP_REMAP_NO_TLB_FLUSH	BIT(1)
	unsigned long		flags;
};

/* 两个标志只延后 TLB 失效，不省略 PTE 内容的发布屏障；批量调用者必须在释放旧页前统一 flush。 */

/* 业务背景：把 vmemmap 的 leaf PMD 拆为 PTE，给 HVO 后续逐 struct page 重映射创造粒度。
 * 入参：pmd/head/start 定位当前 PMD；walk 提供 TLB 策略。出参/返回：0 或 -ENOMEM。
 * 注意事项：page_table_lock 发布 PTE→PMD，写屏障防新 PMD 先于内容可见；可选择延后 TLB 刷新。
 */
static int vmemmap_split_pmd(pmd_t *pmd, struct page *head, unsigned long start,
			     struct vmemmap_remap_walk *walk)
{
	pmd_t __pmd;
	int i;
	unsigned long addr = start;
	pte_t *pgtable;

	pgtable = pte_alloc_one_kernel(&init_mm);
	/* 先私下构造完整 PTE 页，分配失败时旧 PMD 未动，调用者可安全重试或跳过。 */
	if (!pgtable)
		return -ENOMEM;

	pmd_populate_kernel(&init_mm, &__pmd, pgtable);

	for (i = 0; i < PTRS_PER_PTE; i++, addr += PAGE_SIZE) {
		/* 临时 PMD 指向连续 backing pages；随后在锁内一次把它作为原 leaf 的替代。 */
		pte_t entry, *pte;
		pgprot_t pgprot = PAGE_KERNEL;

		entry = mk_pte(head + i, pgprot);
		pte = pte_offset_kernel(&__pmd, addr);
		set_pte_at(&init_mm, addr, pte, entry);
	}

	spin_lock(&init_mm.page_table_lock);
	/* 与并行页表修改竞争，锁内再次确认 leaf，防两个 walker 重复拆分同一 PMD。 */
	if (likely(pmd_leaf(*pmd))) {
		/*
		 * Higher order allocations from buddy allocator must be able to
		 * be treated as independent small pages (as they can be freed
		 * individually).
		 */
		if (!PageReserved(head))
			split_page(head, get_order(PMD_SIZE));

		/* Make pte visible before pmd. See comment in pmd_install(). */
		smp_wmb();
		pmd_populate_kernel(&init_mm, pmd, pgtable);
		if (!(walk->flags & VMEMMAP_SPLIT_NO_TLB_FLUSH))
			flush_tlb_kernel_range(start, start + PMD_SIZE);
	} else {
		/* 已由其他路径拆分时，私有 pgtable 从未发布，必须就地归还。 */
		pte_free_kernel(&init_mm, pgtable);
	}
	spin_unlock(&init_mm.page_table_lock);

	return 0;
}

/* 业务背景：page-table walker 的 PMD 回调，拒绝 self-hosted vmemmap 并按需拆分 leaf PMD。
 * 入参：pmd/addr 是 walker 当前条目，walk 私有区为重映射上下文。出参/返回：继续或错误。
 * 注意事项：持 init_mm.page_table_lock 读取 leaf；热插拔 self-hosted 页不能被 HVO 释放。
 */
static int vmemmap_pmd_entry(pmd_t *pmd, unsigned long addr,
			     unsigned long next, struct mm_walk *walk)
{
	int ret = 0;
	struct page *head;
	struct vmemmap_remap_walk *vmemmap_walk = walk->private;

	/* Only splitting, not remapping the vmemmap pages. */
	if (!vmemmap_walk->remap_pte)
		/* split-only 遍历跳过 PTE 回调，避免 NULL remap_pte 被调用。 */
		walk->action = ACTION_CONTINUE;

	spin_lock(&init_mm.page_table_lock);
	head = pmd_leaf(*pmd) ? pmd_page(*pmd) : NULL;
	/* 仅首个 walked PMD 做 self-hosted 检查：HugeTLB 与 hotplug 对齐保证该页可代表整个目标区间。 */
	/*
	 * Due to HugeTLB alignment requirements and the vmemmap
	 * pages being at the start of the hotplugged memory
	 * region in memory_hotplug.memmap_on_memory case. Checking
	 * the vmemmap page associated with the first vmemmap page
	 * if it is self-hosted is sufficient.
	 *
	 * [                  hotplugged memory                  ]
	 * [        section        ][...][        section        ]
	 * [ vmemmap ][              usable memory               ]
	 *   ^  | ^                        |
	 *   +--+ |                        |
	 *        +------------------------+
	 */
	if (IS_ENABLED(CONFIG_MEMORY_HOTPLUG) && unlikely(!vmemmap_walk->nr_walked)) {
		struct page *page = head ? head + pte_index(addr) :
				    pte_page(ptep_get(pte_offset_kernel(pmd, addr)));

		if (PageVmemmapSelfHosted(page))
			ret = -ENOTSUPP;
	}
	spin_unlock(&init_mm.page_table_lock);
	if (!head || ret)
		/* 非 leaf 无需再拆；self-hosted 拒绝是保护热插拔自身 vmemmap 的硬失败。 */
		return ret;

	return vmemmap_split_pmd(pmd, head, addr & PMD_MASK, vmemmap_walk);
}

/* 业务背景：PMD 已拆分后逐 PTE 分派为替换或恢复映射。
 * 入参：pte/addr 是当前 vmemmap 槽；walk 借用 remap 策略与页面链表。出参/返回：总是继续。
 * 注意事项：回调修改 PTE 的细节由 remap_pte 决定，nr_walked 用于失败回滚范围。
 */
static int vmemmap_pte_entry(pte_t *pte, unsigned long addr,
			     unsigned long next, struct mm_walk *walk)
{
	struct vmemmap_remap_walk *vmemmap_walk = walk->private;

	vmemmap_walk->remap_pte(pte, addr, vmemmap_walk);
	/* 回调完成后才递增，确保失败回滚范围精确等于已替换 PTE 数。 */
	vmemmap_walk->nr_walked++;

	return 0;
}

static const struct mm_walk_ops vmemmap_remap_ops = {
	.pmd_entry	= vmemmap_pmd_entry,
	.pte_entry	= vmemmap_pte_entry,
};

/* 业务背景：HVO 统一在 init_mm 中遍历并改写一段 vmemmap 虚拟地址。
 * 入参：start/end 为页对齐半开区间；walk 是借用的替换/恢复策略。出参/返回：0 或 walker 错误。
 * 注意事项：mmap_read_lock 保护页表遍历；PTE 更新后必须按 flags 决定 TLB 发布时机。
 */
static int vmemmap_remap_range(unsigned long start, unsigned long end,
			       struct vmemmap_remap_walk *walk)
{
	int ret;

	VM_BUG_ON(!PAGE_ALIGNED(start | end));
	/* vmemmap PTE 操作以整页为单位，未对齐会导致半个 struct-page backing 页被错误替换。 */

	mmap_read_lock(&init_mm);
	/* read 锁稳定 init_mm 的页表层级；具体 PTE 安装另由底层 page-table 锁序列化。 */
	ret = walk_kernel_page_table_range(start, end, &vmemmap_remap_ops,
				    NULL, walk);
	mmap_read_unlock(&init_mm);
	if (ret)
		return ret;

	if (walk->remap_pte && !(walk->flags & VMEMMAP_REMAP_NO_TLB_FLUSH))
		/* PTE 已可见后撤销旧翻译；否则 CPU 可能继续访问即将释放的旧 backing 页。 */
		flush_tlb_kernel_range(start, end);

	return 0;
}

/*
 * Free a vmemmap page. A vmemmap page can be allocated from the memblock
 * allocator or buddy allocator. If the PG_reserved flag is set, it means
 * that it allocated from the memblock allocator, just free it via the
 * free_bootmem_page(). Otherwise, use __free_page().
 */
/* 业务背景：归还因 vmemmap 去重而摘除的 backing 页，兼容早期 memblock 与 buddy 来源。
 * 入参：page 是从 remap 收集链表摘下的拥有页面。出参/返回：无；计数并归还页面。
 * 注意事项：PG_reserved 决定释放器，错误选择会破坏启动期分配器账本。
 */
static inline void free_vmemmap_page(struct page *page)
{
	if (PageReserved(page)) {
		/* memblock 页不在 buddy freelist，计数与释放器必须走 bootmem 分支。 */
		memmap_boot_pages_add(-1);
		free_bootmem_page(page);
	} else {
		memmap_pages_add(-1);
		__free_page(page);
	}
}

/* Free a list of the vmemmap pages */
/* 业务背景：批量完成 HVO 成功后的物理 vmemmap 页释放。
 * 入参：list 拥有待释放页的 lru 链。出参/返回：无；逐项转交 free_vmemmap_page。
 * 注意事项：只应在 TLB 刷新后调用，避免 CPU 仍通过旧映射访问已归还页。
 */
static void free_vmemmap_page_list(struct list_head *list)
{
	struct page *page, *next;

	list_for_each_entry_safe(page, next, list, lru)
		/* safe 迭代允许释放后 lru 节点失效，调用前列表 ownership 已完整转移给本函数。 */
		free_vmemmap_page(page);
}

/* 业务背景：优化时保留复制后的 head struct page、把所有 tail 映射收敛到只读共享 tail。
 * 入参：pte/addr 是原映射；walk 持有新 head/tail 与被摘除页链。出参/返回：无。
 * 注意事项：head 内容先复制再经 wmb+set_pte 发布；只读 tail 用于尽早捕获非法 tail 写。
 */
static void vmemmap_remap_pte(pte_t *pte, unsigned long addr,
			      struct vmemmap_remap_walk *walk)
{
	struct page *page = pte_page(ptep_get(pte));
	pte_t entry;

	/* Remapping the head page requires r/w */
	if (unlikely(walk->nr_walked == 0 && walk->vmemmap_head)) {
		/* 第一个 struct page 保存完整 head 元数据并保持可写，避免 HugeTLB head 字段丢失。 */
		VM_WARN_ON_ONCE(!PageHead((const struct page *)addr));

		list_del(&walk->vmemmap_head->lru);
		/* 该新 head 已被 PTE 消费，不能再被调用者当作待释放页。 */

		/*
		 * Makes sure that preceding stores to the page contents from
		 * vmemmap_remap_free() become visible before the set_pte_at()
		 * write.
		 */
		smp_wmb();

		entry = mk_pte(walk->vmemmap_head, PAGE_KERNEL);
	} else {
		/* 其余 tail 共享每 zone/order 模板，RO 映射将非法写变成尽早可见的 fault。 */
		VM_WARN_ON_ONCE(!PageTail((const struct page *)addr));

		/*
		 * Remap the tail pages as read-only to catch illegal write
		 * operation to the tail pages.
		 */
		entry = mk_pte(walk->vmemmap_tail, PAGE_KERNEL_RO);
	}

	list_add(&page->lru, walk->vmemmap_pages);
	/* 被替换的旧 backing 页仍暂存，直到 TLB 不再缓存旧 PTE 才可交 buddy/memblock。 */
	set_pte_at(&init_mm, addr, pte, entry);
}

/* 业务背景：优化半途失败时恢复已替换 PTE 和每页 struct page 的原始内容。
 * 入参：pte/addr 定位槽；walk 提供可恢复链表与共享 tail。出参/返回：无。
 * 注意事项：只恢复仍指向共享 tail 的条目；copy 后 wmb 防新 PTE 先于页面内容可见。
 */
static void vmemmap_restore_pte(pte_t *pte, unsigned long addr,
				struct vmemmap_remap_walk *walk)
{
	struct page *src = pte_page(ptep_get(pte)), *dst;

	/*
	 * When rolling back vmemmap_remap_free(), keep the copied head page
	 * mapping and restore only PTEs currently pointing at the shared tail
	 * page.
	 */
	if (walk->vmemmap_tail && walk->vmemmap_tail != src)
		/* copied head 没有被 tail 替换，回滚时保留它，避免错误覆盖唯一 head 副本。 */
		return;

	VM_WARN_ON_ONCE(PageHead((const struct page *)addr));

	dst = list_first_entry(walk->vmemmap_pages, struct page, lru);
	/* 恢复链按 walk 顺序一一对应 PTE；每取一页立即从 ownership 链移除。 */
	list_del(&dst->lru);
	copy_page(page_to_virt(dst), page_to_virt(src));

	/*
	 * Makes sure that preceding stores to the page contents become visible
	 * before the set_pte_at() write.
	 */
	smp_wmb();
	set_pte_at(&init_mm, addr, pte, mk_pte(dst, PAGE_KERNEL));
}

/**
 * vmemmap_remap_split - split the vmemmap virtual address range [@start, @end)
 *                      backing PMDs of the directmap into PTEs
 * @start:     start address of the vmemmap virtual address range that we want
 *             to remap.
 * @end:       end address of the vmemmap virtual address range that we want to
 *             remap.
 * Return: %0 on success, negative error code otherwise.
 */
/* 业务背景：在实际去重前预拆 vmemmap PMD，避免修改阶段再混入页表结构转换。
 * 入参：start/end 为目标半开虚拟区间。出参/返回：0 或拆分时的 -ENOMEM。
 * 注意事项：TLB 刷新由批量调用者统一完成，失败不会直接进行 tail 重映射。
 */
static int vmemmap_remap_split(unsigned long start, unsigned long end)
{
	struct vmemmap_remap_walk walk = {
		/* 首轮 install 回调拥有 head/tail 与旧页收集链；所有字段在本同步 walk 内有效。 */
		.remap_pte	= NULL,
		.flags		= VMEMMAP_SPLIT_NO_TLB_FLUSH,
	};

	return vmemmap_remap_range(start, end, &walk);
}

/**
 * vmemmap_remap_free - remap the vmemmap virtual address range [@start, @end)
 *			to use @vmemmap_head/tail, then free vmemmap which
 *			the range are mapped to.
 * @start:	start address of the vmemmap virtual address range that we want
 *		to remap.
 * @end:	end address of the vmemmap virtual address range that we want to
 *		remap.
 * @vmemmap_head: the page to be installed as first in the vmemmap range
 * @vmemmap_tail: the page to be installed as non-first in the vmemmap range
 * @vmemmap_pages: list to deposit vmemmap pages to be freed.  It is callers
 *		responsibility to free pages.
 * @flags:	modifications to vmemmap_remap_walk flags
 *
 * Return: %0 on success, negative error code otherwise.
 */
/* 业务背景：把一段 vmemmap 替换为共享 head/tail，并在部分失败时立即回滚已走 PTE。
 * 入参：start/end、head/tail 和 pages 都由调用者借用；flags 控制批量 TLB。出参：0/错误。
 * 注意事项：失败回滚仅覆盖 nr_walked；旧映射页暂存 pages，最终释放责任仍在调用者。
 */
static int vmemmap_remap_free(unsigned long start, unsigned long end,
			      struct page *vmemmap_head,
			      struct page *vmemmap_tail,
			      struct list_head *vmemmap_pages,
			      unsigned long flags)
{
	int ret;
	/* 首轮 walk 的私有状态把新映射页与旧 backing 页收集链捆绑，成功后释放权仍归外层批处理者。 */
	struct vmemmap_remap_walk walk = {
		.remap_pte	= vmemmap_remap_pte,
		.vmemmap_head	= vmemmap_head,
		.vmemmap_tail	= vmemmap_tail,
		.vmemmap_pages	= vmemmap_pages,
		.flags		= flags,
	};

	ret = vmemmap_remap_range(start, end, &walk);
	/* 完整成功无需恢复；未走任何 PTE 的错误也没有旧页状态需要逆转。 */
	if (!ret || !walk.nr_walked)
		return ret;

	end = start + walk.nr_walked * PAGE_SIZE;
	/* 只回滚已经安装共享映射的前缀，不能覆盖 walker 尚未触及的原映射。 */

	/*
	 * vmemmap_pages contains pages from the previous vmemmap_remap_range()
	 * call which failed.  These are pages which were removed from
	 * the vmemmap. They will be restored in the following call.
	 */
	walk = (struct vmemmap_remap_walk) {
	/* 复用 walker 框架但换成 restore 回调；flags=0 强制回滚完成后立即刷新局部 TLB。 */
		.remap_pte	= vmemmap_restore_pte,
		.vmemmap_tail	= vmemmap_tail,
		.vmemmap_pages	= vmemmap_pages,
		.flags		= 0,
	};

	vmemmap_remap_range(start, end, &walk);

	return ret;
}

/* 业务背景：restore 预分配完整 vmemmap backing 页链，保证恢复不依赖被回收页面。
 * 入参：start/end 决定页数，list 接收所有权。出参：0 或 -ENOMEM；失败释放已获页。
 * 注意事项：MAYFAIL 避免无限回收；成功后 memmap 计数与链表必须同步。
 */
static int alloc_vmemmap_page_list(unsigned long start, unsigned long end,
				   struct list_head *list)
{
	gfp_t gfp_mask = GFP_KERNEL | __GFP_RETRY_MAYFAIL;
	unsigned long nr_pages = (end - start) >> PAGE_SHIFT;
	int nid = page_to_nid((struct page *)start);
	struct page *page, *next;
	int i;

	for (i = 0; i < nr_pages; i++) {
		/* list 仅暂存成功分配页；还未调用 remap，因此失败清理不需要恢复任何 PTE。 */
		page = alloc_pages_node(nid, gfp_mask, 0);
		/* 分配失败前 list 中页尚未映射，逐项释放即可，不能错误递减成功路径的全局计数。 */
		if (!page)
			goto out;
		list_add(&page->lru, list);
	}
	memmap_pages_add(nr_pages);
	/* 至此整批替代页均已取得并纳入全局 vmemmap 账本，调用者才能安全开始逐 PTE restore。 */

	return 0;
out:
	list_for_each_entry_safe(page, next, list, lru)
		__free_page(page);
	return -ENOMEM;
}

/**
 * vmemmap_remap_alloc - remap the vmemmap virtual address range [@start, end)
 *			 to the page which is from the @vmemmap_pages
 *			 respectively.
 * @start:	start address of the vmemmap virtual address range that we want
 *		to remap.
 * @end:	end address of the vmemmap virtual address range that we want to
 *		remap.
 * @flags:	modifications to vmemmap_remap_walk flags
 *
 * Return: %0 on success, negative error code otherwise.
 */
/* 业务背景：HugeTLB 回 buddy 前重新分配并映射先前去重掉的 vmemmap 页面。
 * 入参：start/end 是恢复区间，flags 是 TLB 策略。出参：0 或 -ENOMEM/页表错误。
 * 注意事项：分配链所有权在恢复回调消费；失败时由 remap 协议保持可诊断状态。
 */
static int vmemmap_remap_alloc(unsigned long start, unsigned long end,
			       unsigned long flags)
{
	LIST_HEAD(vmemmap_pages);
	struct vmemmap_remap_walk walk = {
		.remap_pte	= vmemmap_restore_pte,
		.vmemmap_pages	= &vmemmap_pages,
		.flags		= flags,
	};

	if (alloc_vmemmap_page_list(start, end, &vmemmap_pages))
		/* 没有完整替代页链就绝不开始 restore，避免 HugeTLB 回收看到半恢复 vmemmap。 */
		return -ENOMEM;

	return vmemmap_remap_range(start, end, &walk);
}

static bool vmemmap_optimize_enabled = IS_ENABLED(CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP_DEFAULT_ON);
/* 业务背景：启动参数覆盖 HVO 默认开关。
 * 入参：buf 是启动命令行布尔字符串。出参：0 或解析错误，并更新全局开关。
 * 注意事项：仅 __init 期间运行；运行期读取用 READ_ONCE 与此写入配合。
 */
static int __init hugetlb_vmemmap_optimize_param(char *buf)
{
	return kstrtobool(buf, &vmemmap_optimize_enabled);
}
early_param("hugetlb_free_vmemmap", hugetlb_vmemmap_optimize_param);

/* 业务背景：释放已优化 HugeTLB folio 前恢复其独立 struct page 数组。
 * 入参：h/folio 为借用对象，flags 控制 TLB；folio 必须为零引用 HugeTLB 页。
 * 出参：0 或恢复错误；成功清 optimized 标志。注意：未优化页是无副作用快速路径。
 */
static int __hugetlb_vmemmap_restore_folio(const struct hstate *h,
					   struct folio *folio, unsigned long flags)
{
	int ret;
	unsigned long vmemmap_start, vmemmap_end;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_hugetlb(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(folio_ref_count(folio), folio);
	/* 零引用是把 vmemmap backing 改写为共享映射的前提，用户映射或并发释放者都不能仍持有 folio。 */

	if (!folio_test_hugetlb_vmemmap_optimized(folio))
		/* 未去重的 folio 已有独立 vmemmap，不做分配或 TLB 操作。 */
		return 0;

	vmemmap_start	= (unsigned long)&folio->page;
	vmemmap_end	= vmemmap_start + hugetlb_vmemmap_size(h);

	vmemmap_start	+= HUGETLB_VMEMMAP_RESERVE_SIZE;
	/* reserve 保留所有真实 head/subpage 状态，只有可镜像 tail 范围可被重新分配。 */

	/*
	 * The pages which the vmemmap virtual address range [@vmemmap_start,
	 * @vmemmap_end) are mapped to are freed to the buddy allocator.
	 * When a HugeTLB page is freed to the buddy allocator, previously
	 * discarded vmemmap pages must be allocated and remapping.
	 */
	ret = vmemmap_remap_alloc(vmemmap_start, vmemmap_end, flags);
	if (!ret)
		folio_clear_hugetlb_vmemmap_optimized(folio);

	return ret;
}

/**
 * hugetlb_vmemmap_restore_folio - restore previously optimized (by
 *				hugetlb_vmemmap_optimize_folio()) vmemmap pages which
 *				will be reallocated and remapped.
 * @h:		struct hstate.
 * @folio:     the folio whose vmemmap pages will be restored.
 *
 * Return: %0 if @folio's vmemmap pages have been reallocated and remapped,
 * negative error code otherwise.
 */
/* 业务背景：单 folio 的公开恢复包装，供 HugeTLB 生命周期在交还 buddy 前调用。
 * 入参：h 与零引用 folio 为借用。出参：0 或恢复错误；不延迟 TLB 刷新。
 * 注意事项：调用者必须满足内部 folio 状态断言，不能把仍使用的 HugeTLB 页交来。
 */
int hugetlb_vmemmap_restore_folio(const struct hstate *h, struct folio *folio)
{
	return __hugetlb_vmemmap_restore_folio(h, folio, 0);
}

/**
 * hugetlb_vmemmap_restore_folios - restore vmemmap for every folio on the list.
 * @h:			hstate.
 * @folio_list:		list of folios.
 * @non_hvo_folios:	Output list of folios for which vmemmap exists.
 *
 * Return: number of folios for which vmemmap was restored, or an error code
 *		if an error was encountered restoring vmemmap for a folio.
 *		Folios that have vmemmap are moved to the non_hvo_folios
 *		list.  Processing of entries stops when the first error is
 *		encountered. The folio that experienced the error and all
 *		non-processed folios will remain on folio_list.
 */
/* 业务背景：批量恢复列表中的 HVO folio，合并 TLB 刷新以降低释放路径开销。
 * 入参：h、folio_list 和 non_hvo_folios 都为调用者拥有的链表对象。出参：恢复数或首个错误。
 * 注意事项：成功/原本非 HVO 的项移动到输出链；失败项及后续项留原链，TLB 刷新在有恢复时发生。
 */
long hugetlb_vmemmap_restore_folios(const struct hstate *h,
					struct list_head *folio_list,
					struct list_head *non_hvo_folios)
{
	struct folio *folio, *t_folio;
	long restored = 0;
	long ret = 0;
	unsigned long flags = VMEMMAP_REMAP_NO_TLB_FLUSH;

	list_for_each_entry_safe(folio, t_folio, folio_list, lru) {
		/* 处理顺序保持输入列表；遇到首错停止，让调用者可保留未处理后缀后续重试。 */
		if (folio_test_hugetlb_vmemmap_optimized(folio)) {
			ret = __hugetlb_vmemmap_restore_folio(h, folio, flags);
			if (ret)
				break;
			restored++;
		}

		/* Add non-optimized folios to output list */
		list_move(&folio->lru, non_hvo_folios);
		/* 输出链只含已有完整 vmemmap 的 folio，后续 buddy 释放不必再区分优化来源。 */
	}

	if (restored)
		flush_tlb_all();
	if (!ret)
		ret = restored;
	return ret;
}

/* Return true iff a HugeTLB whose vmemmap should and can be optimized. */
/* 业务背景：集中判断一个 HugeTLB folio 是否仍可进入 vmemmap 去重。
 * 入参：h/folio 为借用。出参：仅在未优化、全局启用且 hstate 支持时返回 true。
 * 注意事项：READ_ONCE 防止运行期开关读取撕裂；返回 true 不保证后续分配成功。
 */
static bool vmemmap_should_optimize_folio(const struct hstate *h, struct folio *folio)
{
	if (folio_test_hugetlb_vmemmap_optimized(folio))
		return false;

	if (!READ_ONCE(vmemmap_optimize_enabled))
		/* 启动参数关闭时所有 bootmem 页沿普通 sparsemem 路径初始化。 */
		return false;

	if (!hugetlb_vmemmap_optimizable(h))
		return false;

	return true;
}

/* 业务背景：每个 zone/order 懒建一个共享只读 tail struct page 模板，供 HVO 尾页映射复用。
 * 入参：order 指定 HugeTLB 阶数，zone 指定节点归属。出参：发布的 tail 或 NULL。
 * 注意事项：cmpxchg 只允许一个模板获胜；落选者释放私页再 acquire 式重新读取已发布模板。
 */
static struct page *vmemmap_get_tail(unsigned int order, struct zone *zone)
{
	const unsigned int idx = order - VMEMMAP_TAIL_MIN_ORDER;
	struct page *tail, *p;
	int node = zone_to_nid(zone);

	tail = READ_ONCE(zone->vmemmap_tails[idx]);
	/* 已发布 tail 永不替换，先读到者可直接借用；无须持锁但不能修改模板。 */
	if (likely(tail))
		return tail;

	tail = alloc_pages_node(node, GFP_KERNEL | __GFP_ZERO, 0);
	if (!tail)
		return NULL;

	p = page_to_virt(tail);
	/* 逐 struct page 初始化 compound tail 语义，shared RO 映射仍须呈现正确的 page 类型。 */
	for (int i = 0; i < PAGE_SIZE / sizeof(struct page); i++)
		init_compound_tail(p + i, NULL, order, zone);

	if (cmpxchg(&zone->vmemmap_tails[idx], NULL, tail)) {
		/* 竞争者已发布同类模板，本地页不应泄漏；重新读 winner 作为返回的稳定借用值。 */
		__free_page(tail);
		tail = READ_ONCE(zone->vmemmap_tails[idx]);
	}

	return tail;
}

/* 业务背景：真正把一个空闲 HugeTLB folio 的大量 tail vmemmap 页去重为 head+共享 tail。
 * 入参：h/folio 借用；vmemmap_pages 收集待释放页；flags 控制 TLB。出参：0 或 -ENOMEM/映射错误。
 * 注意事项：先置 optimized 保持延迟 TLB 时新旧 head 一致，失败必须清标志；folio 必须零引用。
 */
static int __hugetlb_vmemmap_optimize_folio(const struct hstate *h,
					    struct folio *folio,
					    struct list_head *vmemmap_pages,
					    unsigned long flags)
{
	unsigned long vmemmap_start, vmemmap_end;
	struct page *vmemmap_head, *vmemmap_tail;
	int nid, ret = 0;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_hugetlb(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(folio_ref_count(folio), folio);
	/* 这两个断言排除非 HugeTLB 或仍被引用的页；后续 PTE 替换不具备对并发使用者的同步保护。 */

	if (!vmemmap_should_optimize_folio(h, folio))
		/* 已优化、关闭开关或 hstate 不支持时保持原布局，优化仅是可选内存节省。 */
		return ret;

	nid = folio_nid(folio);
	vmemmap_tail = vmemmap_get_tail(h->order, folio_zone(folio));
	/* tail 模板按 zone 分配，防止 NUMA 内存元数据跨节点复用而破坏 locality/accounting。 */
	if (!vmemmap_tail)
		return -ENOMEM;

	/*
	 * Very Subtle
	 * If VMEMMAP_REMAP_NO_TLB_FLUSH is set, TLB flushing is not performed
	 * immediately after remapping.  As a result, subsequent accesses
	 * and modifications to struct pages associated with the hugetlb
	 * page could be to the OLD struct pages.  Set the vmemmap optimized
	 * flag here so that it is copied to the new head page.  This keeps
	 * the old and new struct pages in sync.
	 * If there is an error during optimization, we will immediately FLUSH
	 * the TLB and clear the flag below.
	 */
	folio_set_hugetlb_vmemmap_optimized(folio);
	/* 必须先标记再复制 head：延迟 TLB 下旧地址的观察者也能看到与新页一致的状态位。 */

	vmemmap_head = alloc_pages_node(nid, GFP_KERNEL, 0);
	/* 新 head 是将来唯一可写 struct page backing；失败直接走 out 撤销预先标记。 */
	if (!vmemmap_head) {
		ret = -ENOMEM;
		goto out;
	}

	copy_page(page_to_virt(vmemmap_head), folio);
	/* 复制包括 compound/head 元数据；随后 remap_pte 仅把第一个 PTE 指向此副本。 */
	list_add(&vmemmap_head->lru, vmemmap_pages);
	memmap_pages_add(1);

	vmemmap_start	= (unsigned long)&folio->page;
	vmemmap_end	= vmemmap_start + hugetlb_vmemmap_size(h);

	/*
	 * Remap the vmemmap virtual address range [@vmemmap_start, @vmemmap_end).
	 * Add pages previously mapping the range to vmemmap_pages list so that
	 * they can be freed by the caller.
	 */
	ret = vmemmap_remap_free(vmemmap_start, vmemmap_end,
	/* remap 成功后旧 backing 页进入 vmemmap_pages，批量调用者在 TLB 刷新后统一释放。 */
				 vmemmap_head, vmemmap_tail,
				 vmemmap_pages, flags);
out:
	if (ret)
		/* 任何映射/分配失败都使 folio 回到未优化语义，不能留下标志与实际 PTE 不匹配。 */
		folio_clear_hugetlb_vmemmap_optimized(folio);

	return ret;
}

/**
 * hugetlb_vmemmap_optimize_folio - optimize @folio's vmemmap pages.
 * @h:		struct hstate.
 * @folio:     the folio whose vmemmap pages will be optimized.
 *
 * This function only tries to optimize @folio's vmemmap pages and does not
 * guarantee that the optimization will succeed after it returns. The caller
 * can use folio_test_hugetlb_vmemmap_optimized(@folio) to detect if @folio's
 * vmemmap pages have been optimized.
 */
/* 业务背景：单 HugeTLB folio 的公开 HVO 包装，完成重映射后立即释放回收的 backing 页。
 * 入参：h 与空闲 folio 借用。出参/返回：无；是否实际优化由 folio 标志表示。
 * 注意事项：失败是允许的性能退化，不改变 HugeTLB 功能；释放只能在 remap 完成后进行。
 */
void hugetlb_vmemmap_optimize_folio(const struct hstate *h, struct folio *folio)
{
	LIST_HEAD(vmemmap_pages);

	__hugetlb_vmemmap_optimize_folio(h, folio, &vmemmap_pages, 0);
	/* 单页包装未延迟 TLB；helper 返回后旧页链已不可再被页表访问。 */
	free_vmemmap_page_list(&vmemmap_pages);
}

/* 业务背景：批量 HVO 第一阶段只拆目标 folio 的 vmemmap PMD，以便后面统一刷新 TLB 后去重。
 * 入参：h/folio 借用。出参：0、无需优化的 0 或 -ENOMEM。
 * 注意事项：不修改 optimized 标志；预拆失败可安全跳过后续去重。
 */
static int hugetlb_vmemmap_split_folio(const struct hstate *h, struct folio *folio)
{
	unsigned long vmemmap_start, vmemmap_end;

	if (!vmemmap_should_optimize_folio(h, folio))
		/* 与真正优化共用 eligibility，避免为不会去重的页无谓拆 PMD。 */
		return 0;

	vmemmap_start	= (unsigned long)&folio->page;
	vmemmap_end	= vmemmap_start + hugetlb_vmemmap_size(h);

	/*
	 * Split PMDs on the vmemmap virtual address range [@vmemmap_start,
	 * @vmemmap_end]
	 */
	return vmemmap_remap_split(vmemmap_start, vmemmap_end);
}

/* 业务背景：批量 HVO 编排拆 PMD、TLB 刷新、去重、低内存重试和最终物理页释放。
 * 入参：h 与 folio_list 借用；boot 区分预 HVO 的启动期页。出参/返回：无。
 * 注意事项：NO_TLB_FLUSH 期间新旧 struct page 并存，必须在释放链表前 flush_tlb_all。
 */
static void __hugetlb_vmemmap_optimize_folios(struct hstate *h,
					      struct list_head *folio_list,
					      bool boot)
{
	struct folio *folio;
	int nr_to_optimize;
	LIST_HEAD(vmemmap_pages);
	unsigned long flags = VMEMMAP_REMAP_NO_TLB_FLUSH;

	nr_to_optimize = 0;
	/* 阶段一只拆 PMD，收集可处理数后一次全局 flush，避免每 folio 的 shootdown 成本。 */
	list_for_each_entry(folio, folio_list, lru) {
		int ret;
		unsigned long spfn, epfn;

		if (boot && folio_test_hugetlb_vmemmap_optimized(folio)) {
			/* pre-HVO 已有共享布局，只补充保护/bootmem 登记，不能再次回收同一 backing 页。 */
			/*
			 * Already optimized by pre-HVO, just map the
			 * mirrored tail page structs RO.
			 */
			spfn = (unsigned long)&folio->page;
			epfn = spfn + pages_per_huge_page(h);
			vmemmap_wrprotect_hvo(spfn, epfn, folio_nid(folio),
					HUGETLB_VMEMMAP_RESERVE_SIZE);
			register_page_bootmem_memmap(pfn_to_section_nr(spfn),
					&folio->page,
					HUGETLB_VMEMMAP_RESERVE_SIZE);
			continue;
		}

		nr_to_optimize++;
		/* 只有尚未 HVO 的 folio 需要为后续 PTE remap 申请/拆分页表。 */

		ret = hugetlb_vmemmap_split_folio(h, folio);

		/*
		 * Splitting the PMD requires allocating a page, thus let's fail
		 * early once we encounter the first OOM. No point in retrying
		 * as it can be dynamically done on remap with the memory
		 * we get back from the vmemmap deduplication.
		 */
		if (ret == -ENOMEM)
			break;
	}

	if (!nr_to_optimize)
		/* 全部为 pre-HVO 时没有新 PTE 替换，也就没有待释放页链。 */
		/*
		 * All pre-HVO folios, nothing left to do. It's ok if
		 * there is a mix of pre-HVO and not yet HVO-ed folios
		 * here, as __hugetlb_vmemmap_optimize_folio() will
		 * skip any folios that already have the optimized flag
		 * set, see vmemmap_should_optimize_folio().
		 */
		goto out;

	flush_tlb_all();
	/* PMD→PTE 转换完成后先失效旧巨大翻译，后续才允许安装并延迟释放新旧 backing 页。 */

	list_for_each_entry(folio, folio_list, lru) {
		int ret;

		ret = __hugetlb_vmemmap_optimize_folio(h, folio, &vmemmap_pages, flags);
		/* 阶段二收集每页旧 backing；NO_TLB_FLUSH 让本批在最后一次 shootdown 前保持高吞吐。 */

		/*
		 * Pages to be freed may have been accumulated.  If we
		 * encounter an ENOMEM,  free what we have and try again.
		 * This can occur in the case that both splitting fails
		 * halfway and head page allocation also failed. In this
		 * case __hugetlb_vmemmap_optimize_folio() would free memory
		 * allowing more vmemmap remaps to occur.
		 */
		if (ret == -ENOMEM && !list_empty(&vmemmap_pages)) {
			/* 已回收候选页可先释放以缓解分配压力，再重试当前 folio；不无限重试其它错误。 */
			flush_tlb_all();
			free_vmemmap_page_list(&vmemmap_pages);
			INIT_LIST_HEAD(&vmemmap_pages);
			__hugetlb_vmemmap_optimize_folio(h, folio, &vmemmap_pages, flags);
		}
	}

out:
	flush_tlb_all();
	/* 最后一轮 TLB 刷新是释放 vmemmap_pages 的生命周期屏障，缺失会产生旧 PTE use-after-free。 */
	free_vmemmap_page_list(&vmemmap_pages);
}

/* 业务背景：运行期 HugeTLB 批量优化公开入口。
 * 入参：h 与调用者持有的 folio_list。出参/返回：无；每项可独立跳过或优化。
 * 注意事项：内部负责批量 TLB 和释放，调用者不得并行改变该列表。
 */
void hugetlb_vmemmap_optimize_folios(struct hstate *h, struct list_head *folio_list)
{
	__hugetlb_vmemmap_optimize_folios(h, folio_list, false);
}

/* 业务背景：启动期 bootmem HugeTLB 页的批量优化入口，兼容早期已预处理的映射。
 * 入参：h 与 boot folio_list 借用。出参/返回：无。
 * 注意事项：boot=true 允许只补 tail RO 映射，不可把启动期标志当作运行期新优化。
 */
void hugetlb_vmemmap_optimize_bootmem_folios(struct hstate *h, struct list_head *folio_list)
{
	__hugetlb_vmemmap_optimize_folios(h, folio_list, true);
}

#ifdef CONFIG_SPARSEMEM_VMEMMAP_PREINIT

/* Return true of a bootmem allocated HugeTLB page should be pre-HVO-ed */
/* 业务背景：判断 gigantic bootmem HugeTLB 页能否在 sparsemem 预初始化时采用 HVO 布局。
 * 入参：m 为借用的启动期 HugeTLB 描述符。出参：满足开关和 section/PMD 对齐时 true。
 * 注意事项：预 HVO 不支持 PMD 拆分；任一对齐失败必须回退普通 vmemmap 初始化。
 */
static bool vmemmap_should_optimize_bootmem_page(struct huge_bootmem_page *m)
{
	unsigned long section_size, psize, pmd_vmemmap_size;
	phys_addr_t paddr;

	if (!READ_ONCE(vmemmap_optimize_enabled))
		return false;

	if (!hugetlb_vmemmap_optimizable(m->hstate))
		return false;

	psize = huge_page_size(m->hstate);
	/* 物理地址和 hugepage 大小都必须同时满足 section 对齐，避免一个页跨越预初始化所有权边界。 */
	paddr = virt_to_phys(m);

	/*
	 * Pre-HVO only works if the bootmem huge page
	 * is aligned to the section size.
	 */
	section_size = (1UL << PA_SECTION_SHIFT);
	if (!IS_ALIGNED(paddr, section_size) ||
	    !IS_ALIGNED(psize, section_size))
		return false;

	/*
	 * The pre-HVO code does not deal with splitting PMDS,
	 * so the bootmem page must be aligned to the number
	 * of base pages that can be mapped with one vmemmap PMD.
	 */
	pmd_vmemmap_size = (PMD_SIZE / (sizeof(struct page))) << PAGE_SHIFT;
	/* HVO 预初始化不能临时分裂 PMD，因此至少以一个 vmemmap PMD 覆盖粒度对齐。 */
	if (!IS_ALIGNED(paddr, pmd_vmemmap_size) ||
	    !IS_ALIGNED(psize, pmd_vmemmap_size))
		return false;

	return true;
}

/*
 * Initialize memmap section for a gigantic page, HVO-style.
 */
/* 业务背景：稀疏内存早期为满足条件的 bootmem hugepage 建立 HVO section 初始布局。
 * 入参：nid 是当前节点。出参/返回：无；合格项标记 HUGE_BOOTMEM_HVO。
 * 注意事项：仅启动期运行，须在普通 section 初始化之前；全局开关关闭时无副作用返回。
 */
void __init hugetlb_vmemmap_init_early(int nid)
{
	unsigned long psize, paddr, section_size;
	unsigned long ns, i, pnum, pfn, nr_pages;
	struct huge_bootmem_page *m = NULL;
	void *map;

	if (!READ_ONCE(vmemmap_optimize_enabled))
		/* 早期入口无全局锁，开关只在启动序列设置，READ_ONCE 保证编译器不合并读取。 */
		return;

	section_size = (1UL << PA_SECTION_SHIFT);

	list_for_each_entry(m, &huge_boot_pages[nid], list) {
		/* 每个 boot hugepage 独立判定，无法 pre-HVO 的项留给普通 population。 */
		if (!vmemmap_should_optimize_bootmem_page(m))
			continue;

		nr_pages = pages_per_huge_page(m->hstate);
		/* map 指向该 hugepage 的第一个 struct page；pnum/ns 把物理范围切成 sparse section。 */
		psize = nr_pages << PAGE_SHIFT;
		paddr = virt_to_phys(m);
		pfn = PHYS_PFN(paddr);
		map = pfn_to_page(pfn);

		pnum = pfn_to_section_nr(pfn);
		ns = psize / section_size;

		for (i = 0; i < ns; i++) {
			/* 为每 section 安装 PREINIT 标记，后段据此知道它可安全采用 HVO backing。 */
			sparse_init_early_section(nid, map, pnum,
					SECTION_IS_VMEMMAP_PREINIT);
			map += section_map_size();
			pnum++;
		}

		m->flags |= HUGE_BOOTMEM_HVO;
		/* 标志将早期资格结果传给 init_late，避免重新推断对齐和 section 布局。 */
	}
}

/* 业务背景：启动期 HVO population 需把 PFN 归属到实际 zone 以选择共享 tail 模板。
 * 入参：nid/pfn 指定候选物理页。出参：覆盖该 PFN 的 zone 或 NULL。
 * 注意事项：跨 zone hugepage 不能使用此结果继续 HVO，调用者必须显式回退。
 */
static struct zone *pfn_to_zone(unsigned nid, unsigned long pfn)
{
	struct zone *zone;
	enum zone_type zone_type;

	for (zone_type = 0; zone_type < MAX_NR_ZONES; zone_type++) {
		/* node_zones 有空洞也要逐一测试 PFN 覆盖，zone 编号不是连续物理范围。 */
		zone = &NODE_DATA(nid)->node_zones[zone_type];
		if (zone_spans_pfn(zone, pfn))
			return zone;
	}

	return NULL;
}

/* 业务背景：启动期后段完成预 HVO 页的真实 vmemmap population，并处理跨 zone/分配失败回退。
 * 入参：nid 为要处理的 boot hugepage 节点。出参/返回：无；更新 bootmem memmap 账本。
 * 注意事项：仅 __init；HVO population 失败必须普通 populate，不能留下部分 vmemmap。
 */
void __init hugetlb_vmemmap_init_late(int nid)
{
	struct huge_bootmem_page *m, *tm;
	unsigned long phys, nr_pages, start, end;
	unsigned long pfn, nr_mmap;
	struct zone *zone = NULL;
	struct hstate *h;
	void *map;

	if (!READ_ONCE(vmemmap_optimize_enabled))
		/* 未启用时不处理 huge_boot_pages，正常 sparsemem 初始化继续拥有这些页。 */
		return;

	list_for_each_entry_safe(m, tm, &huge_boot_pages[nid], list) {
		/* safe 遍历允许跨 zone fallback 将当前 boot 页从待处理链表摘除。 */
		if (!(m->flags & HUGE_BOOTMEM_HVO))
			/* 只有 early 阶段确认的项可进入 HVO population，其他项不应误套 reserve 布局。 */
			continue;

		phys = virt_to_phys(m);
		h = m->hstate;
		pfn = PHYS_PFN(phys);
		nr_pages = pages_per_huge_page(h);
		map = pfn_to_page(pfn);
		start = (unsigned long)map;
		end = start + nr_pages * sizeof(struct page);

		if (!hugetlb_bootmem_page_zones_valid(nid, m)) {
			/* HVO 共享 tail 只能绑定单 zone；跨区巨大页必须撤销标记并普通 populate。 */
			/*
			 * Oops, the hugetlb page spans multiple zones.
			 * Remove it from the list, and populate it normally.
			 */
			list_del(&m->list);

			vmemmap_populate(start, end, nid, NULL);
			nr_mmap = end - start;
			memmap_boot_pages_add(DIV_ROUND_UP(nr_mmap, PAGE_SIZE));

			memblock_phys_free(phys, huge_page_size(h));
			continue;
		}

		if (!zone || !zone_spans_pfn(zone, pfn))
			/* 缓存 zone 仅在仍覆盖当前 PFN 时复用，避免列表中不同页误用上一个 zone。 */
			zone = pfn_to_zone(nid, pfn);
		if (WARN_ON_ONCE(!zone))
			continue;

		if (vmemmap_populate_hvo(start, end, huge_page_order(h), zone,
			/* HVO population 失败不传播为启动失败：普通 population 保证 vmemmap 完整可用。 */
					 HUGETLB_VMEMMAP_RESERVE_SIZE) < 0) {
			/* Fallback if HVO population fails */
			vmemmap_populate(start, end, nid, NULL);
			nr_mmap = end - start;
		} else {
			m->flags |= HUGE_BOOTMEM_ZONES_VALID;
			nr_mmap = HUGETLB_VMEMMAP_RESERVE_SIZE;
		}

		memmap_boot_pages_add(DIV_ROUND_UP(nr_mmap, PAGE_SIZE));
		/* 无论 HVO 或回退都按真正 backing 页数记账，供后续 bootmem 释放保持平衡。 */
	}
}
#endif

static const struct ctl_table hugetlb_vmemmap_sysctls[] = {
	/* sysctl 只暴露开关而不直接重映射现有页；实际效果由后续 HugeTLB 生命周期触发。 */
	{
		.procname	= "hugetlb_optimize_vmemmap",
		.data		= &vmemmap_optimize_enabled,
		.maxlen		= sizeof(vmemmap_optimize_enabled),
		.mode		= 0644,
		.proc_handler	= proc_dobool,
	},
};

/* 业务背景：late_init 重建共享 tail 的 compound 元数据，并仅在可优化 hstate 存在时注册 sysctl。
 * 入参：无。出参/返回：始终 0；完成全局 HVO 运行期发布。
 * 注意事项：BUILD_BUG_ON 固化 reserve 覆盖不变量；只在启动完成后运行一次。
 */
static int __init hugetlb_vmemmap_init(void)
{
	const struct hstate *h;
	struct zone *zone;

	/* HUGETLB_VMEMMAP_RESERVE_SIZE should cover all used struct pages */
	BUILD_BUG_ON(__NR_USED_SUBPAGE > HUGETLB_VMEMMAP_RESERVE_PAGES);
	/* reserve 若不足则 tail 镜像会覆盖仍需独立的 subpage 元数据，必须编译期拒绝。 */

	for_each_zone(zone) {
		/* 早期模板建立时 compound tail 可能尚未完整，late_init 在每 zone/order 重建该不变量。 */
		for (int i = 0; i < NR_VMEMMAP_TAILS; i++) {
			/* 一个 order 对应一个可共享 tail 模板；NULL 表示该阶从未被 HVO 使用。 */
			struct page *tail, *p;
			unsigned int order;

			tail = zone->vmemmap_tails[i];
			if (!tail)
				continue;

			order = i + VMEMMAP_TAIL_MIN_ORDER;
			/* 重新初始化防止启动期临时布局遗留的 compound 元数据与运行期 HVO 不一致。 */
			p = page_to_virt(tail);
			for (int j = 0; j < PAGE_SIZE / sizeof(struct page); j++)
				init_compound_tail(p + j, NULL, order, zone);
		}
	}

	for_each_hstate(h) {
		/* 没有可优化 hugepage 尺寸时不注册无效 sysctl，避免用户看到永远无作用的开关。 */
		if (hugetlb_vmemmap_optimizable(h)) {
			register_sysctl_init("vm", hugetlb_vmemmap_sysctls);
			break;
		}
	}
	return 0;
}
late_initcall(hugetlb_vmemmap_init);
