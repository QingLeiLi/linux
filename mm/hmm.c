// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2013 Red Hat Inc.
 *
 * Authors: Jérôme Glisse <jglisse@redhat.com>
 */
/*
 * Refer to include/linux/hmm.h for information about heterogeneous memory
 * management or HMM for short.
 */
#include <linux/pagewalk.h>
#include <linux/hmm.h>
#include <linux/hmm-dma.h>
#include <linux/init.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/slab.h>
#include <linux/sched.h>
/* 以下声明提供 NUMA 页表、leaf 编码、device memory、interval notifier 与 DMA/P2P 的跨子系统契约。 */
#include <linux/mmzone.h>
#include <linux/pagemap.h>
#include <linux/leafops.h>
#include <linux/hugetlb.h>
#include <linux/memremap.h>
#include <linux/sched/mm.h>
#include <linux/jump_label.h>
/* DMA 与 P2P 头紧随其后：HMM 映射不得把 CPU 页的 ownership 交给 DMA 子系统。 */
#include <linux/dma-mapping.h>
#include <linux/pci-p2pdma.h>
#include <linux/mmu_notifier.h>
#include <linux/memory_hotplug.h>

#include "internal.h"

/* 引入顺序覆盖 pagewalk、MMU interval notifier、页表 leaf 与 DMA/PCI P2P 后端；各接口共同定义本文件的同步边界。 */
/* 本文件把 CPU 页表的瞬时状态投影给异构设备；PFN 数组不是长期 pin，失效由 interval notifier 协议处理。 */

struct hmm_vma_walk {
	/* pagewalk 私有上下文：range 由调用者拥有，last 记录发生 fault/retry 后尚未产出的首地址。 */
	struct hmm_range	*range;
	unsigned long		last;
};

enum {
	/* fault 请求的内部汇总位：walk 可把多个 PFN 的需求合并为一次读/写 fault。 */
	HMM_NEED_FAULT = 1 << 0,
	HMM_NEED_WRITE_FAULT = 1 << 1,
	HMM_NEED_ALL_BITS = HMM_NEED_FAULT | HMM_NEED_WRITE_FAULT,
};

enum {
	/* 这些位由驱动在输入数组中保留，页表扫描只能更新 PFN 与 CPU 访问权限部分。 */
	/* These flags are carried from input-to-output */
	HMM_PFN_INOUT_FLAGS = HMM_PFN_DMA_MAPPED | HMM_PFN_P2PDMA |
			      HMM_PFN_P2PDMA_BUS,
};

static int hmm_pfns_fill(unsigned long addr, unsigned long end,
			 struct hmm_range *range, unsigned long cpu_flags)
{
	/* 业务背景：hole、不可访问 VMA 或无 fault 请求时批量写 PFN 输出槽。
	 * 入参：addr/end 为页对齐半开区间，range 借用且 hmm_pfns 足够大；cpu_flags 是输出状态。
	 * 出参/返回：保留 sticky DMA/P2P 位后填入状态，成功恒为 0。
	 * 注意事项：只改范围内槽位；索引基于 range->start，调用者持 mmap/read 与 interval 协议。
	 */
	unsigned long i = (addr - range->start) >> PAGE_SHIFT;

	for (; addr < end; addr += PAGE_SIZE, i++) {
		/* 每槽先清旧 CPU 输出但保留设备侧 sticky 位，防止一次重扫丢失已建立 DMA 映射。 */
		range->hmm_pfns[i] &= HMM_PFN_INOUT_FLAGS;
		range->hmm_pfns[i] |= cpu_flags;
	}
	return 0;
}

/*
 * hmm_vma_fault() - fault in a range lacking valid pmd or pte(s)
 * @addr: range virtual start address (inclusive)
 * @end: range virtual end address (exclusive)
 * @required_fault: HMM_NEED_* flags
 * @walk: mm_walk structure
 * Return: -EBUSY after page fault, or page fault error
 *
 * This function will be called whenever pmd_none() or pte_none() returns true,
 * or whenever there is no page directory covering the virtual address range.
 */
static int hmm_vma_fault(unsigned long addr, unsigned long end,
			 unsigned int required_fault, struct mm_walk *walk)
{
	/* 业务背景：walk 发现缺 PTE/PMD 时按驱动请求主动建立 CPU 映射，随后返回 EBUSY 让外层重扫。
	 * 入参：地址区间、读/写需求和 pagewalk 上下文；返回 -EPERM/-EFAULT/-EBUSY。
	 * 注意事项：REMOTE fault 可睡眠；last 在首次未完成地址发布，不能在仍持页表锁时调用。
	 */
	struct hmm_vma_walk *hmm_vma_walk = walk->private;
	struct vm_area_struct *vma = walk->vma;
	unsigned int fault_flags = FAULT_FLAG_REMOTE;

	WARN_ON_ONCE(!required_fault);
	hmm_vma_walk->last = addr;

	if (required_fault & HMM_NEED_WRITE_FAULT) {
		/* 写请求必须先通过 VMA 权限检查，不能靠 handle_mm_fault 越过只读映射。 */
		if (!(vma->vm_flags & VM_WRITE))
			return -EPERM;
		fault_flags |= FAULT_FLAG_WRITE;
	}

	for (; addr < end; addr += PAGE_SIZE)
		/* 成功 fault 后仍不在本轮读取 PTE，强制 -EBUSY 回到 interval 检查与重新 pagewalk。 */
		if (handle_mm_fault(vma, addr, fault_flags, NULL) &
		    VM_FAULT_ERROR)
			return -EFAULT;
	return -EBUSY;
}

static unsigned int hmm_pte_need_fault(const struct hmm_vma_walk *hmm_vma_walk,
				       unsigned long pfn_req_flags,
				       unsigned long cpu_flags)
{
	/* 合并单槽输入请求与 range 默认策略，判定现有 CPU flags 是否已满足有效/可写承诺。 */
	struct hmm_range *range = hmm_vma_walk->range;

	/*
	 * So we not only consider the individual per page request we also
	 * consider the default flags requested for the range. The API can
	 * be used 2 ways. The first one where the HMM user coalesces
	 * multiple page faults into one request and sets flags per pfn for
	 * those faults. The second one where the HMM user wants to pre-
	 * fault a range with specific flags. For the latter one it is a
	 * waste to have the user pre-fill the pfn arrays with a default
	 * flags value.
	 */
	pfn_req_flags &= range->pfn_flags_mask;
	/* mask 控制哪些逐槽输入位有效，default_flags 则覆盖整个 range 的公共请求。 */
	pfn_req_flags |= range->default_flags;

	/* We aren't ask to do anything ... */
	if (!(pfn_req_flags & HMM_PFN_REQ_FAULT))
		/* 纯查询不会建立映射：not-present 输出保持非 VALID，驱动据此自行跳过。 */
		return 0;

	/* Need to write fault ? */
	/* 写权限缺失时必须返回两个需求位，保证上层不会仅完成读 fault 后误判成功。 */
	if ((pfn_req_flags & HMM_PFN_REQ_WRITE) &&
	    !(cpu_flags & HMM_PFN_WRITE))
		return HMM_NEED_FAULT | HMM_NEED_WRITE_FAULT;

	/* If CPU page table is not valid then we need to fault */
	/* 有效位缺失独立于可写位；只要求读 fault 时不应引入无谓写权限升级。 */
	if (!(cpu_flags & HMM_PFN_VALID))
		return HMM_NEED_FAULT;
	return 0;
}

static unsigned int
hmm_range_need_fault(const struct hmm_vma_walk *hmm_vma_walk,
		     const unsigned long hmm_pfns[], unsigned long npages,
		     unsigned long cpu_flags)
{
	/* 批量 OR 所有槽的需求；同时需要有效和可写时可提前停止，供 huge 映射避免逐页无效工作。 */
	struct hmm_range *range = hmm_vma_walk->range;
	unsigned int required_fault = 0;
	unsigned long i;

	/*
	 * If the default flags do not request to fault pages, and the mask does
	 * not allow for individual pages to be faulted, then
	 * hmm_pte_need_fault() will always return 0.
	 */
	if (!((range->default_flags | range->pfn_flags_mask) &
	      HMM_PFN_REQ_FAULT))
		return 0;
	/* 快速路径证明没有任何槽允许/要求 fault，调用者输入数组无需逐项访问。 */

	for (i = 0; i < npages; ++i) {
		/* 请求位来自调用者输入数组；函数不修改它，实际输出留给对应页表层级。 */
		required_fault |= hmm_pte_need_fault(hmm_vma_walk, hmm_pfns[i],
						     cpu_flags);
		if (required_fault == HMM_NEED_ALL_BITS)
			return required_fault;
	}
	return required_fault;
}

static int hmm_vma_walk_hole(unsigned long addr, unsigned long end,
			     __always_unused int depth, struct mm_walk *walk)
{
	/* 业务背景：pagewalk 遇到未建页表或 VMA 空洞时，把无 fault 请求的槽标为错误/空，或触发 fault。
	 * 注意事项：walk->vma 为 NULL 代表没有 VMA，要求 fault 即是硬错误；有 VMA 的缺项返回 EBUSY 重扫。
	 */
	struct hmm_vma_walk *hmm_vma_walk = walk->private;
	struct hmm_range *range = hmm_vma_walk->range;
	unsigned int required_fault;
	unsigned long i, npages;
	unsigned long *hmm_pfns;

	i = (addr - range->start) >> PAGE_SHIFT;
	/* hole 的 base-page 数由 walk 提供的边界决定，不能假设它等于整个 VMA。 */
	npages = (end - addr) >> PAGE_SHIFT;
	hmm_pfns = &range->hmm_pfns[i];
	required_fault =
		hmm_range_need_fault(hmm_vma_walk, hmm_pfns, npages, 0);
	if (!walk->vma) {
		/* 地址没有 VMA 时没有可 fault 的后端；无请求才可返回 HMM_PFN_ERROR 而继续扫描。 */
		if (required_fault)
			return -EFAULT;
		return hmm_pfns_fill(addr, end, range, HMM_PFN_ERROR);
	}
	if (required_fault)
		/* 有 VMA 的空页表可以通过 fault 建立，返回 -EBUSY 保证不产生半新半旧 PFN 结果。 */
		return hmm_vma_fault(addr, end, required_fault, walk);
	return hmm_pfns_fill(addr, end, range, 0);
}

static inline unsigned long hmm_pfn_flags_order(unsigned long order)
{
	/* 将 CPU large mapping 的页阶编码到 HMM 输出高位，低位仍保存 PFN。 */
	return order << HMM_PFN_ORDER_SHIFT;
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static inline unsigned long pmd_to_hmm_pfn_flags(struct hmm_range *range,
						 pmd_t pmd)
{
	/* THP PMD 的单个权限快照适用于覆盖范围内每个 base-page PFN；protnone 不可报告为有效。 */
	if (pmd_protnone(pmd))
		return 0;
	return (pmd_write(pmd) ? (HMM_PFN_VALID | HMM_PFN_WRITE) :
				 HMM_PFN_VALID) |
	       hmm_pfn_flags_order(PMD_SHIFT - PAGE_SHIFT);
}

static int hmm_vma_handle_pmd(struct mm_walk *walk, unsigned long addr,
			      unsigned long end, unsigned long hmm_pfns[],
			      pmd_t pmd)
{
	/* 业务背景：把稳定的 THP PMD 展开写为连续 PFN 槽，避免再下钻 PTE。
	 * 注意事项：先检查整个 huge 范围的 fault 请求；有需求时不能部分填表，转由 fault→重扫闭环。
	 */
	struct hmm_vma_walk *hmm_vma_walk = walk->private;
	struct hmm_range *range = hmm_vma_walk->range;
	unsigned long pfn, npages, i;
	unsigned int required_fault;
	unsigned long cpu_flags;

	npages = (end - addr) >> PAGE_SHIFT;
	cpu_flags = pmd_to_hmm_pfn_flags(range, pmd);
	/* PMD leaf 可在 range 两端裁剪，后续 PFN 必须从 addr 的 PMD 内偏移而非首页计算。 */
	required_fault =
		hmm_range_need_fault(hmm_vma_walk, hmm_pfns, npages, cpu_flags);
	if (required_fault)
	/* THP 大页的需求由整个 range 汇总；返回后外层仍需以 EBUSY 重新获得一致页表快照。 */
		return hmm_vma_fault(addr, end, required_fault, walk);

	pfn = pmd_pfn(pmd) + ((addr & ~PMD_MASK) >> PAGE_SHIFT);
	/* 连续 base PFN 加相同 PMD order/权限，驱动可选择按大页批处理。 */
	for (i = 0; addr < end; addr += PAGE_SIZE, i++, pfn++) {
		hmm_pfns[i] &= HMM_PFN_INOUT_FLAGS;
		hmm_pfns[i] |= pfn | cpu_flags;
	}
	return 0;
}
#else /* CONFIG_TRANSPARENT_HUGEPAGE */
/* 配置关闭时只保留外部声明；调用点由 pmd_trans_huge() 条件保证不会执行该实现。 */
/* stub to allow the code below to compile */
int hmm_vma_handle_pmd(struct mm_walk *walk, unsigned long addr,
		unsigned long end, unsigned long hmm_pfns[], pmd_t pmd);
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

static inline unsigned long pte_to_hmm_pfn_flags(struct hmm_range *range,
						 pte_t pte)
{
	/* 普通 PTE 仅导出 CPU 可读/可写权限；none、not-present 与 protnone 都不能作为有效 PFN。 */
	if (pte_none(pte) || !pte_present(pte) || pte_protnone(pte))
		return 0;
	return pte_write(pte) ? (HMM_PFN_VALID | HMM_PFN_WRITE) : HMM_PFN_VALID;
}

static int hmm_vma_handle_pte(struct mm_walk *walk, unsigned long addr,
			      unsigned long end, pmd_t *pmdp, pte_t *ptep,
			      unsigned long *hmm_pfn)
{
	/* 业务背景：将单个普通、swap、device-private 或 migration PTE 转为驱动可消费 HMM 槽。
	 * 入参：ptep 为当前映射借用，hmm_pfn 是输入输出槽；返回 0、-EBUSY 或硬错误。
	 * 注意事项：所有离开 fault 路径先 pte_unmap；设备私有页仅同 owner 可直接返回 PFN。
	 */
	struct hmm_vma_walk *hmm_vma_walk = walk->private;
	struct hmm_range *range = hmm_vma_walk->range;
	unsigned int required_fault;
	unsigned long cpu_flags;
	pte_t pte = ptep_get(ptep);
	uint64_t pfn_req_flags = *hmm_pfn;
	uint64_t new_pfn_flags = 0;

	/*
	 * Any other marker than a UFFD WP marker will result in a fault error
	 * that will be correctly handled, so we need only check for UFFD WP
	 * here.
	 */
	if (pte_none(pte) || pte_is_uffd_wp_marker(pte)) {
		/* UFFD write-protect marker 没有可直接导出的 CPU PFN；按请求 fault 或留空。 */
		required_fault =
			hmm_pte_need_fault(hmm_vma_walk, pfn_req_flags, 0);
		if (required_fault)
			goto fault;
		goto out;
	}

	if (!pte_present(pte)) {
		/* not-present PTE 的 softleaf 类型决定是可等待迁移、可 fault swap，还是硬错误。 */
		const softleaf_t entry = softleaf_from_pte(pte);

		/*
		 * Don't fault in device private pages owned by the caller,
		 * just report the PFN.
		 */
		if (softleaf_is_device_private(entry) &&
		    page_pgmap(softleaf_to_page(entry))->owner ==
		    range->dev_private_owner) {
		/* 仅 owner 相同才能直接报告 device-private PFN；跨设备 owner 必须回到 CPU fault 路径。 */
			cpu_flags = HMM_PFN_VALID;
			if (softleaf_is_device_private_write(entry))
				cpu_flags |= HMM_PFN_WRITE;
			new_pfn_flags = softleaf_to_pfn(entry) | cpu_flags;
			goto out;
		}

		required_fault =
			hmm_pte_need_fault(hmm_vma_walk, pfn_req_flags, 0);
		if (!required_fault)
		/* 无请求时保留零输出，让设备知道此槽尚不可用而不是强行迁回页面。 */
			goto out;

		if (softleaf_is_swap(entry))
		/* swap entry 可通过普通 fault 重新变为 present PTE。 */
			goto fault;

		if (softleaf_is_device_private(entry))
		/* 非本 owner 的 private 页必须 fault，不能泄漏另一设备的本地内存 PFN。 */
			goto fault;

		if (softleaf_is_device_exclusive(entry))
		/* exclusive 页同样需要 fault 协议协调 CPU/device 的独占转换。 */
			goto fault;

		if (softleaf_is_migration(entry)) {
		/* 迁移中 PFN 不稳定：等待完成后用 -EBUSY 强制完整重读。 */
			pte_unmap(ptep);
			hmm_vma_walk->last = addr;
			migration_entry_wait(walk->mm, pmdp, addr);
			return -EBUSY;
		}

		/* Report error for everything else */
	/* 非法 softleaf 没有可恢复语义；ptep 已 unmap 后向上层给硬错误。 */
		pte_unmap(ptep);
		return -EFAULT;
	}

	cpu_flags = pte_to_hmm_pfn_flags(range, pte);
	required_fault =
		hmm_pte_need_fault(hmm_vma_walk, pfn_req_flags, cpu_flags);
	if (required_fault)
		goto fault;

	/*
	 * Since each architecture defines a struct page for the zero page, just
	 * fall through and treat it like a normal page.
	 */
	if (!vm_normal_page(walk->vma, addr, pte) &&
	    !is_zero_pfn(pte_pfn(pte))) {
		/* 非 normal 的非零 PTE 没有通用 page lifetime，不能假装成可 DMA 的普通 PFN。 */
		if (hmm_pte_need_fault(hmm_vma_walk, pfn_req_flags, 0)) {
			pte_unmap(ptep);
			return -EFAULT;
		}
		new_pfn_flags = HMM_PFN_ERROR;
		/* 纯查询仍给明确 ERROR；调用者可继续处理其它槽而不会误访问此地址。 */
		goto out;
	}

	new_pfn_flags = pte_pfn(pte) | cpu_flags;
out:
	/* 所有正常输出路径统一保留输入 sticky 位；DMA map 重扫后才可安全复用。 */
	*hmm_pfn = (*hmm_pfn & HMM_PFN_INOUT_FLAGS) | new_pfn_flags;
	return 0;

fault:
	/* fault 路径不得带着 pte 映射进入可能睡眠的 handle_mm_fault。 */
	pte_unmap(ptep);
	/* Fault any virtual address we were asked to fault */
	return hmm_vma_fault(addr, end, required_fault, walk);
}

#ifdef CONFIG_ARCH_ENABLE_THP_MIGRATION
static int hmm_vma_handle_absent_pmd(struct mm_walk *walk, unsigned long start,
				     unsigned long end, unsigned long *hmm_pfns,
				     pmd_t pmd)
{
	/* THP migration 配置下，device-private PMD 属于本驱动时完整展开；否则只能 fault 或报告错误。 */
	struct hmm_vma_walk *hmm_vma_walk = walk->private;
	struct hmm_range *range = hmm_vma_walk->range;
	unsigned long npages = (end - start) >> PAGE_SHIFT;
	const softleaf_t entry = softleaf_from_pmd(pmd);
	unsigned long addr = start;
	unsigned int required_fault;

	/* entry 是 PMD software leaf 快照；仅同一 dev_private_owner 可绕过迁回 CPU 的 fault。 */
	if (softleaf_is_device_private(entry) &&
	    softleaf_to_folio(entry)->pgmap->owner ==
	    range->dev_private_owner) {
		unsigned long cpu_flags = HMM_PFN_VALID |
			hmm_pfn_flags_order(PMD_SHIFT - PAGE_SHIFT);
		unsigned long pfn = softleaf_to_pfn(entry);
		unsigned long i;

		if (softleaf_is_device_private_write(entry))
			cpu_flags |= HMM_PFN_WRITE;

		/*
		 * Fully populate the PFN list though subsequent PFNs could be
		 * inferred, because drivers which are not yet aware of large
		 * folios probably do not support sparsely populated PFN lists.
		 */
		for (i = 0; addr < end; addr += PAGE_SIZE, i++, pfn++) {
			/* 即使 PMD 大页也逐 base page 物化输出，以兼容旧驱动的非稀疏数组假设。 */
			hmm_pfns[i] &= HMM_PFN_INOUT_FLAGS;
			hmm_pfns[i] |= pfn | cpu_flags;
		}

		return 0;
	}

	required_fault = hmm_range_need_fault(hmm_vma_walk, hmm_pfns,
					      npages, 0);
	if (required_fault) {
		/* 非本 owner 的 absent PMD 只有 device-private 才能 fault 恢复，其余情形硬失败。 */
		if (softleaf_is_device_private(entry))
			return hmm_vma_fault(addr, end, required_fault, walk);
		else
			return -EFAULT;
	}

	return hmm_pfns_fill(start, end, range, HMM_PFN_ERROR);
}
#else
/* 不支持 THP migration 的构建没有 device-private PMD 迁移等待路径，保守地只报告错误。 */
static int hmm_vma_handle_absent_pmd(struct mm_walk *walk, unsigned long start,
				     unsigned long end, unsigned long *hmm_pfns,
				     pmd_t pmd)
{
	struct hmm_vma_walk *hmm_vma_walk = walk->private;
	struct hmm_range *range = hmm_vma_walk->range;
	unsigned long npages = (end - start) >> PAGE_SHIFT;

	if (hmm_range_need_fault(hmm_vma_walk, hmm_pfns, npages, 0))
		/* 未启用 THP migration 时没有安全的等待/重试协议，缺项请求直接失败。 */
		return -EFAULT;
	return hmm_pfns_fill(start, end, range, HMM_PFN_ERROR);
}
#endif  /* CONFIG_ARCH_ENABLE_THP_MIGRATION */

static int hmm_vma_walk_pmd(pmd_t *pmdp,
			    unsigned long start,
			    unsigned long end,
			    struct mm_walk *walk)
{
	/* 业务背景：pagewalk 的 PMD 回调依次处理 hole、migration、device/THP、bad PMD 和普通 PTE。
	 * 注意事项：无锁 PMD 快照可能与 split 竞争，重读失败回到 again；PTE 映射必须由本函数配对 unmap。
	 */
	struct hmm_vma_walk *hmm_vma_walk = walk->private;
	struct hmm_range *range = hmm_vma_walk->range;
	unsigned long *hmm_pfns =
		&range->hmm_pfns[(start - range->start) >> PAGE_SHIFT];
	unsigned long npages = (end - start) >> PAGE_SHIFT;
	unsigned long addr = start;
	pte_t *ptep;
	pmd_t pmd;

again:
	pmd = pmdp_get_lockless(pmdp);
	/* 无锁 PMD 快照可能被 split 改写；后续巨大页分支会再次读取确认。 */
	if (pmd_none(pmd))
		/* 缺 PMD 与 PTE hole 使用同一输出/fault 协议，避免层级差异泄漏给驱动。 */
		return hmm_vma_walk_hole(start, end, -1, walk);

	if (thp_migration_supported() && pmd_is_migration_entry(pmd)) {
		/* 等待 PMD migration 前只发布 last，绝不把迁移中的 huge PFN 写进结果数组。 */
		if (hmm_range_need_fault(hmm_vma_walk, hmm_pfns, npages, 0)) {
			hmm_vma_walk->last = addr;
			pmd_migration_entry_wait(walk->mm, pmdp);
			return -EBUSY;
		}
		return hmm_pfns_fill(start, end, range, 0);
	}

	if (!pmd_present(pmd))
		/* 其它 non-present PMD 交配置 helper 解释，主 walk 不猜测软件叶类型。 */
		return hmm_vma_handle_absent_pmd(walk, start, end, hmm_pfns,
						 pmd);

	if (pmd_trans_huge(pmd)) {
		/* THP leaf 不下钻普通 PTE；重读失败则重新分类，保证 PFN/order 同一快照。 */
		/*
		 * No need to take pmd_lock here, even if some other thread
		 * is splitting the huge pmd we will get that event through
		 * mmu_notifier callback.
		 *
		 * So just read pmd value and check again it's a transparent
		 * huge or device mapping one and compute corresponding pfn
		 * values.
		 */
		pmd = pmdp_get_lockless(pmdp);
		if (!pmd_trans_huge(pmd))
			goto again;

		return hmm_vma_handle_pmd(walk, addr, end, hmm_pfns, pmd);
	}

	/*
	 * We have handled all the valid cases above ie either none, migration,
	 * huge or transparent huge. At this point either it is a valid pmd
	 * entry pointing to pte directory or it is a bad pmd that will not
	 * recover.
	 */
	if (pmd_bad(pmd)) {
		/* bad PMD 不能由用户 fault 修复；无请求时将该段标为 ERROR 并继续扫描。 */
		if (hmm_range_need_fault(hmm_vma_walk, hmm_pfns, npages, 0))
			return -EFAULT;
		return hmm_pfns_fill(start, end, range, HMM_PFN_ERROR);
	}

	ptep = pte_offset_map(pmdp, addr);
	/* 临时 PTE 映射由末尾 unmap 配对，循环内错误返回已由子 helper 先行解除。 */
	if (!ptep)
		/* 页表页可能在无锁 PMD 读取后被撤销；重新从 PMD 分类而非使用失效 PTE 指针。 */
		goto again;
	for (; addr < end; addr += PAGE_SIZE, ptep++, hmm_pfns++) {
		/* 每次处理一个 base PTE；hmm_pfns 指针与虚拟地址同步前进。 */
		int r;

		r = hmm_vma_handle_pte(walk, addr, end, pmdp, ptep, hmm_pfns);
		if (r) {
			/* hmm_vma_handle_pte() did pte_unmap() */
			return r;
		}
	}
	pte_unmap(ptep - 1);
	/* 成功走完全段后，最后一个临时 PTE 映射在此统一归还。 */
	return 0;
}

#if defined(CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD)
static inline unsigned long pud_to_hmm_pfn_flags(struct hmm_range *range,
						 pud_t pud)
{
	/* PUD leaf 与 PMD 相同地编码权限和 mapping order；非 present 不向驱动伪造 PFN。 */
	if (!pud_present(pud))
		return 0;
	return (pud_write(pud) ? (HMM_PFN_VALID | HMM_PFN_WRITE) :
				 HMM_PFN_VALID) |
	       hmm_pfn_flags_order(PUD_SHIFT - PAGE_SHIFT);
}

static int hmm_vma_walk_pud(pud_t *pudp, unsigned long start, unsigned long end,
		struct mm_walk *walk)
{
	/* 业务背景：架构支持时处理 PUD huge leaf，否则要求 pagewalk 继续下钻。
	 * 注意事项：pud_trans_huge_lock 成功后必须所有出口 spin_unlock；fault 前释放锁以避免递归死锁。
	 */
	struct hmm_vma_walk *hmm_vma_walk = walk->private;
	struct hmm_range *range = hmm_vma_walk->range;
	unsigned long addr = start;
	pud_t pud;
	spinlock_t *ptl = pud_trans_huge_lock(pudp, walk->vma);

	if (!ptl)
		/* 非 PUD-huge 映射交给下一层；观察 HMM 不应主动请求 split。 */
		return 0;

	/* Normally we don't want to split the huge page */
	walk->action = ACTION_CONTINUE;
	/* 持锁检查期间禁止 pagewalk 自动拆 huge 映射，避免为了读取而改变用户页表。 */

	pud = pudp_get(pudp);
	if (!pud_present(pud)) {
		/* 锁下复查 absent 后先放自旋锁，再进入可能 fault 的 hole 处理。 */
		spin_unlock(ptl);
		return hmm_vma_walk_hole(start, end, -1, walk);
	}
	/* 此后 PUD present，但可能是 leaf 或下级表；两者分别决定填 PFN 或 ACTION_SUBTREE。 */

	if (pud_leaf(pud)) {
		/* PUD leaf 覆盖连续 PFN；range 可能只覆盖其中一段，索引必须从 start 计算。 */
		unsigned long i, npages, pfn;
		unsigned int required_fault;
		unsigned long *hmm_pfns;
		unsigned long cpu_flags;

		i = (addr - range->start) >> PAGE_SHIFT;
		npages = (end - addr) >> PAGE_SHIFT;
		hmm_pfns = &range->hmm_pfns[i];

		cpu_flags = pud_to_hmm_pfn_flags(range, pud);
		/* 权限和 order 取自锁内 PUD 快照；不在解锁后重新读 entry。 */
		required_fault = hmm_range_need_fault(hmm_vma_walk, hmm_pfns,
						      npages, cpu_flags);
		if (required_fault) {
			/* fault 前已经不再使用 ptl；VMA 锁的释放/重取由 HugeTLB 协议配对。 */
			spin_unlock(ptl);
			return hmm_vma_fault(addr, end, required_fault, walk);
		}

		pfn = pud_pfn(pud) + ((addr & ~PUD_MASK) >> PAGE_SHIFT);
		/* 起点偏移保持子范围 PFN 正确，循环将结果展开成 HMM 固定的 base-page ABI。 */
		for (i = 0; i < npages; ++i, ++pfn) {
			hmm_pfns[i] &= HMM_PFN_INOUT_FLAGS;
			hmm_pfns[i] |= pfn | cpu_flags;
		}
		goto out_unlock;
	}

	/* Ask for the PUD to be split */
	/* 非 leaf PUD 不强制拆分，只让 pagewalk 下降；HMM 读取不能改变映射形态。 */
	walk->action = ACTION_SUBTREE;

out_unlock:
	spin_unlock(ptl);
	return 0;
}
#else
#define hmm_vma_walk_pud	NULL
#endif

#ifdef CONFIG_HUGETLB_PAGE
/* HugeTLB 使用独立 hstate/PTE 锁；关闭配置时回调表置空，让 pagewalk 不注册该层处理。 */
static int hmm_vma_walk_hugetlb_entry(pte_t *pte, unsigned long hmask,
				      unsigned long start, unsigned long end,
				      struct mm_walk *walk)
{
	/* 业务背景：HugeTLB 回调在 huge PTE 锁下写连续 HMM PFN，并携带该 hstate 的 order。
	 * 注意事项：要求 fault 时先放 PTE 与 VMA read lock；fault 可能重新取得 VMA 锁，返回后才恢复读锁。
	 */
	unsigned long addr = start, i, pfn;
	struct hmm_vma_walk *hmm_vma_walk = walk->private;
	struct hmm_range *range = hmm_vma_walk->range;
	struct vm_area_struct *vma = walk->vma;
	unsigned int required_fault;
	unsigned long pfn_req_flags;
	unsigned long cpu_flags;
	spinlock_t *ptl;
	pte_t entry;

	ptl = huge_pte_lock(hstate_vma(vma), walk->mm, pte);
	/* HugeTLB 的 hstate 决定锁和页阶；普通 PTE 锁不保证此 entry 的稳定性。 */
	entry = huge_ptep_get(walk->mm, addr, pte);

	i = (start - range->start) >> PAGE_SHIFT;
	pfn_req_flags = range->hmm_pfns[i];
	cpu_flags = pte_to_hmm_pfn_flags(range, entry) |
		    hmm_pfn_flags_order(huge_page_order(hstate_vma(vma)));
	required_fault =
		hmm_pte_need_fault(hmm_vma_walk, pfn_req_flags, cpu_flags);
	if (required_fault) {
		/* 必须先解除 ptl 和 VMA read lock，再调用可重入 VMA 锁的 fault helper。 */
		int ret;

		spin_unlock(ptl);
		hugetlb_vma_unlock_read(vma);
		/*
		 * Avoid deadlock: drop the vma lock before calling
		 * hmm_vma_fault(), which will itself potentially take and
		 * drop the vma lock. This is also correct from a
		 * protection point of view, because there is no further
		 * use here of either pte or ptl after dropping the vma
		 * lock.
		 */
		ret = hmm_vma_fault(addr, end, required_fault, walk);
		hugetlb_vma_lock_read(vma);
		return ret;
	}

	pfn = pte_pfn(entry) + ((start & ~hmask) >> PAGE_SHIFT);
	/* hmask 保证起点在 huge 映射中的 PFN 偏移正确，不会错误从 huge 首页报告。 */
	for (; addr < end; addr += PAGE_SIZE, i++, pfn++) {
		/* HugeTLB 也按 base-page 填充，保持 hmm_pfns ABI 对所有映射层级一致。 */
		range->hmm_pfns[i] &= HMM_PFN_INOUT_FLAGS;
		range->hmm_pfns[i] |= pfn | cpu_flags;
	}

	spin_unlock(ptl);
	/* PFN/权限均在同一 huge PTE 锁快照下生成，解锁后结果只能经 notifier 继续使用。 */
	return 0;
}
#else
#define hmm_vma_walk_hugetlb_entry NULL
#endif /* CONFIG_HUGETLB_PAGE */

static int hmm_vma_walk_test(unsigned long start, unsigned long end,
			     struct mm_walk *walk)
{
	/* test_walk 在进入 VMA 前排除 I/O、PFNMAP 与不可读映射；若调用者要求 fault 必须失败，不能越权。 */
	struct hmm_vma_walk *hmm_vma_walk = walk->private;
	struct hmm_range *range = hmm_vma_walk->range;
	struct vm_area_struct *vma = walk->vma;

	if (!(vma->vm_flags & (VM_IO | VM_PFNMAP)) &&
		/* 普通可读 VMA 允许后续 level 回调进入；其余范围不具备 HMM 所需 struct page。 */
	    vma->vm_flags & VM_READ)
		return 0;

	/*
	 * vma ranges that don't have struct page backing them or map I/O
	 * devices directly cannot be handled by hmm_range_fault().
	 *
	 * If the vma does not allow read access, then assume that it does not
	 * allow write access either. HMM does not support architectures that
	 * allow write without read.
	 *
	 * If a fault is requested for an unsupported range then it is a hard
	 * failure.
	 */
	if (hmm_range_need_fault(hmm_vma_walk,
				 range->hmm_pfns +
					 ((start - range->start) >> PAGE_SHIFT),
				 (end - start) >> PAGE_SHIFT, 0))
		/* 对不支持 VMA 请求 fault 是权限/对象错误，不可把它弱化成 ERROR 部分成功。 */
		return -EFAULT;

	hmm_pfns_fill(start, end, range, HMM_PFN_ERROR);
	/* 纯查询的特殊 VMA 统一显式填 ERROR，并以 1 指示 pagewalk 跳过但继续下一 VMA。 */

	/* Skip this vma and continue processing the next vma. */
	return 1;
}

static const struct mm_walk_ops hmm_walk_ops = {
	/* 回调表把各级 leaf/hole 处理接入 walk_page_range；PGWALK_RDLOCK 规定 walk 持 mmap 读锁。 */
	.pud_entry	= hmm_vma_walk_pud,
	.pmd_entry	= hmm_vma_walk_pmd,
	.pte_hole	= hmm_vma_walk_hole,
	.hugetlb_entry	= hmm_vma_walk_hugetlb_entry,
	.test_walk	= hmm_vma_walk_test,
	.walk_lock	= PGWALK_RDLOCK,
};

/**
 * hmm_range_fault - try to fault some address in a virtual address range
 * @range:	argument structure
 *
 * Returns 0 on success or one of the following error codes:
 *
 * -EINVAL:	Invalid arguments or mm or virtual address is in an invalid vma
 *		(e.g., device file vma).
 * -ENOMEM:	Out of memory.
 * -EPERM:	Invalid permission (e.g., asking for write and range is read
 *		only).
 * -EBUSY:	The range has been invalidated and the caller needs to wait for
 *		the invalidation to finish.
 * -EFAULT:     A page was requested to be valid and could not be made valid
 *              ie it has no backing VMA or it is illegal to access
 *
 * This is similar to get_user_pages(), except that it can read the page tables
 * without mutating them (ie causing faults).
 */
int hmm_range_fault(struct hmm_range *range)
{
	/* 业务背景：驱动在 interval notifier 的序列快照内取得 CPU 页表 PFN/权限，并按需 fault。
	 * 入参：range 含 notifier、半开 VA 和输入输出 PFN 数组；返回 0 或 -EBUSY/-EFAULT/-EPERM。
	 * 注意事项：调用者已持 mmap lock；-EBUSY 不可把部分数组当完整结果，须重新 read_begin 后重试。
	 */
	struct hmm_vma_walk hmm_vma_walk = {
		.range = range,
		.last = range->start,
	};
	struct mm_struct *mm = range->notifier->mm;
	int ret;

	mmap_assert_locked(mm);
	/* 每轮仅从 last 继续，前缀槽已是输出；fault 回来的 EBUSY 绝不覆盖它们的 sticky DMA 标记。 */

	do {
		/* If range is no longer valid force retry. */
		if (mmu_interval_check_retry(range->notifier,
					     range->notifier_seq))
			/* notifier 序列变化意味着此前 PFN 已可能失效，调用者必须重新 begin/read。 */
			return -EBUSY;
		ret = walk_page_range(mm, hmm_vma_walk.last, range->end,
		/* walk 仅处理未完成后缀，last 由 fault/migration helper 在返回 EBUSY 前记录。 */
				      &hmm_walk_ops, &hmm_vma_walk);
		/*
		 * When -EBUSY is returned the loop restarts with
		 * hmm_vma_walk.last set to an address that has not been stored
		 * in pfns. All entries < last in the pfn array are set to their
		 * output, and all >= are still at their input values.
		 */
	} while (ret == -EBUSY);
	return ret;
}
EXPORT_SYMBOL(hmm_range_fault);

/**
 * hmm_dma_map_alloc - Allocate HMM map structure
 * @dev: device to allocate structure for
 * @map: HMM map to allocate
 * @nr_entries: number of entries in the map
 * @dma_entry_size: size of the DMA entry in the map
 *
 * Allocate the HMM map structure and all the lists it contains.
 * Return 0 on success, -ENOMEM on failure.
 */
int hmm_dma_map_alloc(struct device *dev, struct hmm_dma_map *map,
		      size_t nr_entries, size_t dma_entry_size)
{
	/* 业务背景：为 HMM PFN 数组准备 DMA 地址或 IOVA 状态，供驱动逐槽 map。
	 * 入参：dev/map 借用，nr_entries 和 dma_entry_size 定义数组与 DMA 粒度；返回 0、-ENOMEM、-EOPNOTSUPP。
	 * 注意事项：HMM 不转移页 ownership，不能使用会产生 SWIOTLB bounce 的设备；成功后必须 map_free 配对。
	 */
	bool dma_need_sync = false;
	bool use_iova;

	WARN_ON_ONCE(!(nr_entries * PAGE_SIZE / dma_entry_size));
	/* 这个告警保护 entry 尺寸与页数换算；错误输入不允许静默构造零长度 IOVA。 */

	/*
	 * The HMM API violates our normal DMA buffer ownership rules and can't
	 * transfer buffer ownership.  The dma_addressing_limited() check is a
	 * best approximation to ensure no swiotlb buffering happens.
	 */
#ifdef CONFIG_DMA_NEED_SYNC
	dma_need_sync = !dev_dma_skip_sync(dev);
#endif /* CONFIG_DMA_NEED_SYNC */
	if (dma_need_sync || dma_addressing_limited(dev))
		/* 同步或地址受限意味着 DMA API 可能复制缓冲，违背 HMM 直接访问原 PFN 的不变量。 */
		return -EOPNOTSUPP;

	map->dma_entry_size = dma_entry_size;
	/* entry_size 是 idx→DMA offset 的唯一换算单位，map 生命周期中不得变更。 */
	map->pfn_list = kvcalloc(nr_entries, sizeof(*map->pfn_list),
				 GFP_KERNEL | __GFP_NOWARN);
	if (!map->pfn_list)
		/* PFN 表是后续所有映射的前提，失败时尚无其它 ownership 需要回滚。 */
		return -ENOMEM;

	use_iova = dma_iova_try_alloc(dev, &map->state, 0,
	/* IOVA 成功时 state 持有整段地址空间，逐 PFN 的 link/unlink 后续才建立物理关联。 */
			nr_entries * PAGE_SIZE);
	if (!use_iova && dma_need_unmap(dev)) {
		/* 无 IOVA 且映射有状态时保存每槽 dma_addr，unmap 才能对称调用 DMA API。 */
		map->dma_list = kvzalloc_objs(*map->dma_list, nr_entries,
					      GFP_KERNEL | __GFP_NOWARN);
		if (!map->dma_list)
			goto err_dma;
	}
	return 0;

err_dma:
	/* 仅 pfn_list 已成功分配；释放后 map 不可传给 map_pfn，调用者收到失败。 */
	kvfree(map->pfn_list);
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(hmm_dma_map_alloc);

/**
 * hmm_dma_map_free - iFree HMM map structure
 * @dev: device to free structure from
 * @map: HMM map containing the various lists and state
 *
 * Free the HMM map structure and all the lists it contains.
 */
void hmm_dma_map_free(struct device *dev, struct hmm_dma_map *map)
{
	/* 释放 map 容器资源，不替驱动逐槽 unmap；调用者须先撤销仍有效的 DMA 映射。 */
	if (dma_use_iova(&map->state))
		dma_iova_free(dev, &map->state);
	kvfree(map->pfn_list);
	kvfree(map->dma_list);
}
EXPORT_SYMBOL_GPL(hmm_dma_map_free);

/**
 * hmm_dma_map_pfn - Map a physical HMM page to DMA address
 * @dev: Device to map the page for
 * @map: HMM map
 * @idx: Index into the PFN and dma address arrays
 * @p2pdma_state: PCI P2P state.
 *
 * dma_alloc_iova() allocates IOVA based on the size specified by their use in
 * iova->size. Call this function after IOVA allocation to link whole @page
 * to get the DMA address. Note that very first call to this function
 * will have @offset set to 0 in the IOVA space allocated from
 * dma_alloc_iova(). For subsequent calls to this function on same @iova,
 * @offset needs to be advanced by the caller with the size of previous
 * page that was linked + DMA address returned for the previous page that was
 * linked by this function.
 */
dma_addr_t hmm_dma_map_pfn(struct device *dev, struct hmm_dma_map *map,
			   size_t idx,
			   struct pci_p2pdma_map_state *p2pdma_state)
{
	/* 业务背景：将一个已有效 HMM PFN 链接成设备可访问 DMA 地址，并在槽中发布 DMA_MAPPED/P2P 状态。
	 * 注意事项：idx 由调用者校验范围；IOVA link 失败后必须 unlink，普通 DMA 映射失败不设置 DMA_MAPPED。
	 */
	struct dma_iova_state *state = &map->state;
	dma_addr_t *dma_addrs = map->dma_list;
	unsigned long *pfns = map->pfn_list;
	struct page *page = hmm_pfn_to_page(pfns[idx]);
	/* 调用者只应传入 HMM_PFN_VALID 槽；否则 pfn_to_page/物理地址转换没有生命周期保证。 */
	phys_addr_t paddr = hmm_pfn_to_phys(pfns[idx]);
	size_t offset = idx * map->dma_entry_size;
	unsigned long attrs = DMA_ATTR_REQUIRE_COHERENT;
	dma_addr_t dma_addr;
	int ret;

	if ((pfns[idx] & HMM_PFN_DMA_MAPPED) &&
	    !(pfns[idx] & HMM_PFN_P2PDMA_BUS)) {
		/*
		 * We are in this flow when there is a need to resync flags,
		 * for example when page was already linked in prefetch call
		 * with READ flag and now we need to add WRITE flag
		 *
		 * This page was already programmed to HW and we don't want/need
		 * to unlink and link it again just to resync flags.
		 */
		if (dma_use_iova(state))
			/* IOVA 已有映射只需返回确定 offset 地址，不重复 DMA link 以避免设备侧闪断。 */
			/* IOVA 地址由固定 base+offset 决定，已有 link 可直接复用来提升权限同步。 */
			return state->addr + offset;

		/*
		 * Without dma_need_unmap, the dma_addrs array is NULL, thus we
		 * need to regenerate the address below even if there already
		 * was a mapping. But !dma_need_unmap implies that the
		 * mapping stateless, so this is fine.
		 */
		if (dma_need_unmap(dev))
			/* 有状态 DMA 的已映射地址保存在 dma_list；无状态设备可安全重新计算 map 地址。 */
			return dma_addrs[idx];

		/* Continue to remapping */
	}

	switch (pci_p2pdma_state(p2pdma_state, dev, page)) {
	case PCI_P2PDMA_MAP_NONE:
		/* 普通 RAM 走默认 coherent DMA 属性，既不设置 P2P 标记也不改变错误恢复。 */
		break;
	case PCI_P2PDMA_MAP_THRU_HOST_BRIDGE:
		/* 经 host bridge 的 P2P 仍走 DMA 映射，但须标记 MMIO 属性和 P2PDMA 来源。 */
		attrs |= DMA_ATTR_MMIO;
		pfns[idx] |= HMM_PFN_P2PDMA;
		break;
	case PCI_P2PDMA_MAP_BUS_ADDR:
		/* 总线地址由 P2P helper 直接给出，没有 DMA API map/unmap 配对。 */
		pfns[idx] |= HMM_PFN_P2PDMA_BUS | HMM_PFN_DMA_MAPPED;
		return pci_p2pdma_bus_addr_map(p2pdma_state->mem, paddr);
	default:
		/* 未知 P2P 路径不能猜测地址类型，直接拒绝且不发布 DMA_MAPPED。 */
		return DMA_MAPPING_ERROR;
	}

	if (dma_use_iova(state)) {
		/* IOVA 路径先 link physical span，再 sync；sync 失败时撤 link 保持 state 可复用。 */
		ret = dma_iova_link(dev, state, paddr, offset,
				    map->dma_entry_size, DMA_BIDIRECTIONAL,
				    attrs);
		if (ret)
			/* link 失败时尚未建立 IOVA 物理关联，统一进入 error 清理本次 P2P 临时位。 */
			goto error;

		ret = dma_iova_sync(dev, state, offset, map->dma_entry_size);
		if (ret) {
			/* sync 失败需立即 unlink；否则 IOVA state 会保留无人引用的物理 span。 */
			dma_iova_unlink(dev, state, offset, map->dma_entry_size,
					DMA_BIDIRECTIONAL, attrs);
			goto error;
		}

		dma_addr = state->addr + offset;
	} else {
		/* 无 IOVA 路径由 DMA API 返回地址；有状态设备把结果保存给 unmap。 */
		if (WARN_ON_ONCE(dma_need_unmap(dev) && !dma_addrs))
			goto error;

		dma_addr = dma_map_phys(dev, paddr, map->dma_entry_size,
					DMA_BIDIRECTIONAL, attrs);
		if (dma_mapping_error(dev, dma_addr))
			goto error;

		if (dma_need_unmap(dev))
			dma_addrs[idx] = dma_addr;
	}
	pfns[idx] |= HMM_PFN_DMA_MAPPED;
	/* 此位是映射真正对后续调用可见的发布点，必须晚于 link/sync 或 dma_map_phys 全部成功。 */
	return dma_addr;
error:
	/* 错误只清本次可能设置的 P2P 位，DMA_MAPPED 从未成功发布，因此调用者可安全重试。 */
	pfns[idx] &= ~HMM_PFN_P2PDMA;
	return DMA_MAPPING_ERROR;

}
EXPORT_SYMBOL_GPL(hmm_dma_map_pfn);

/**
 * hmm_dma_unmap_pfn - Unmap a physical HMM page from DMA address
 * @dev: Device to unmap the page from
 * @map: HMM map
 * @idx: Index of the PFN to unmap
 *
 * Returns true if the PFN was mapped and has been unmapped, false otherwise.
 */
bool hmm_dma_unmap_pfn(struct device *dev, struct hmm_dma_map *map, size_t idx)
{
	/* 业务背景：撤销一个已映射且仍有效的 PFN 的 IOVA/DMA/P2P 可见性。
	 * 出参/返回：true 表示执行或确认了撤销；false 表示槽未处于可撤销状态。
	 * 注意事项：调用者串行化同一槽 map/unmap；最后统一清三种 sticky 映射位。
	 */
	const unsigned long valid_dma = HMM_PFN_VALID | HMM_PFN_DMA_MAPPED;
	struct dma_iova_state *state = &map->state;
	dma_addr_t *dma_addrs = map->dma_list;
	unsigned long *pfns = map->pfn_list;
	unsigned long attrs = DMA_ATTR_REQUIRE_COHERENT;

	if ((pfns[idx] & valid_dma) != valid_dma)
		/* 无效页或从未成功 map 的槽绝不可调用 DMA unmap，避免对垃圾地址反向操作。 */
		return false;

	if (pfns[idx] & HMM_PFN_P2PDMA)
		/* 经 host bridge 的 P2P 必须按 MMIO 属性 unmap，和 map_pfn 的属性设置配对。 */
		attrs |= DMA_ATTR_MMIO;

	if (pfns[idx] & HMM_PFN_P2PDMA_BUS)
		/* bus-address P2P 没有 DMA 映射资源；仅清标记使下一次查询重新决策路径。 */
		; /* no need to unmap bus address P2P mappings */
	else if (dma_use_iova(state))
		/* IOVA 撤销只 unlink 当前 idx span，整个 IOVA 地址空间仍由 map_free 最终释放。 */
		dma_iova_unlink(dev, state, idx * map->dma_entry_size,
				map->dma_entry_size, DMA_BIDIRECTIONAL, attrs);
	else if (dma_need_unmap(dev))
		dma_unmap_phys(dev, dma_addrs[idx], map->dma_entry_size,
			       DMA_BIDIRECTIONAL, attrs);

	pfns[idx] &=
	/* 清三种 sticky 位是 unmap 的发布点；后续 map 必须重新执行 P2P 与 DMA 决策。 */
		~(HMM_PFN_DMA_MAPPED | HMM_PFN_P2PDMA | HMM_PFN_P2PDMA_BUS);
	return true;
}
EXPORT_SYMBOL_GPL(hmm_dma_unmap_pfn);
