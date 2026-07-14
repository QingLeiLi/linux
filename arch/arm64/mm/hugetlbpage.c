// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/arm64/mm/hugetlbpage.c
 *
 * Copyright (C) 2013 Linaro Ltd.
 *
 * Based on arch/x86/mm/hugetlbpage.c.
 */

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/pagemap.h>
#include <linux/err.h>
#include <linux/sysctl.h>
#include <asm/mman.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>

/*
 * HugeTLB Support Matrix
 *
 * ---------------------------------------------------
 * | Page Size | CONT PTE |  PMD  | CONT PMD |  PUD  |
 * ---------------------------------------------------
 * |     4K    |   64K    |   2M  |    32M   |   1G  |
 * |    16K    |    2M    |  32M  |     1G   |       |
 * |    64K    |    2M    | 512M  |    16G   |       |
 * ---------------------------------------------------
 */

/*
 * Reserve CMA areas for the largest supported gigantic
 * huge page when requested. Any other smaller gigantic
 * huge pages could still be served from those areas.
 */

/*
 * arch_hugetlb_cma_order - 返回当前平台支持的最大 gigantic 巨页的阶数
 *
 * 被 hugetlb_cma_reserve() 调用，用于确定 CMA 区域需要预留多大的块。
 * 返回值是页阶数（order），即 gigantic 页大小 = PAGE_SIZE << order。
 *
 * arm64 支持的大页尺寸取决于基础页大小（PAGE_SIZE），见文件头的矩阵：
 *
 *   PAGE_SIZE = 4K：
 *     CONT PTE =  64K（order=4）   ← 普通大页，buddy 可分配
 *     PMD      =   2M（order=9）   ← 普通大页，buddy 可分配
 *     CONT PMD =  32M（order=13）  ← gigantic，order > MAX_PAGE_ORDER（10）
 *     PUD      =   1G（order=18）  ← gigantic，order > MAX_PAGE_ORDER（10）
 *
 *   PAGE_SIZE = 16K：
 *     CONT PMD =   1G（order=16）  ← gigantic
 *     （无 PUD 段映射支持）
 *
 *   PAGE_SIZE = 64K：
 *     CONT PMD =  16G（order=18）  ← gigantic
 *     （无 PUD 段映射支持）
 *
 * CMA 预留应覆盖"最大"的 gigantic 页，较小的 gigantic 页可以从同一
 * CMA 区域中分配（注释所说的 "any other smaller gigantic huge pages"）。
 *
 * 路径选择：
 *   pud_sect_supported()：仅在 PAGE_SIZE == 4K 时为 true，
 *     此时 PUD 段映射可用，最大 gigantic 页为 1G，
 *     order = PUD_SHIFT - PAGE_SHIFT = 30 - 12 = 18（1G / 4K = 2^18）。
 *
 *   否则（PAGE_SIZE = 16K 或 64K）：PUD 段映射不支持，
 *     最大 gigantic 页为 CONT PMD 大页，
 *     order = CONT_PMD_SHIFT - PAGE_SHIFT：
 *       16K 页：(14 + 6) - 14 = 16（1G  / 16K = 2^16）
 *       64K 页：(16 + 8) - 16 = 18（16G / 64K = 2^18）
 *
 * 返回 0 表示架构不支持（由 __weak 默认实现覆盖），此时 hugetlb_cma_reserve()
 * 会打印警告并跳过 CMA 预留。本文件的实现覆盖了该 __weak 默认值。
 */
/*
PUD 段映射
arm64 使用多级页表将虚拟地址翻译为物理地址，4K 页时是 4 级：

虚拟地址（48位）
  [47:39] PGD index（9位）→ 指向 PUD 表
  [38:30] PUD index（9位）→ 指向 PMD 表
  [29:21] PMD index（9位）→ 指向 PTE 表
  [20:12] PTE index（9位）→ 指向物理页
  [11:0]  页内偏移（12位）

正常情况下，PUD 条目指向下一级 PMD 表，继续翻译。

---
PUD 段映射（PUD Section Mapping）

PUD 条目可以不指向 PMD 表，而是直接指向一块 1GB 的物理内存块，跳过 PMD 和 PTE 两级：

正常路径（4级翻译）：
  PGD → PUD → PMD → PTE → 4KB 物理页

PUD 段映射（2级翻译）：
  PGD → PUD ──────────────→ 1GB 物理块（直接命中）
              ↑ 条目的低位标记为"block entry"而非"table entry"

这块 1GB 的物理内存就是一个 gigantic 巨页（PUD_SIZE = 1GB）。

---
为什么只有 4K 页支持 PUD 段映射

arm64 的页表条目宽度固定 64 位，其中物理地址字段的范围由页大小决定：

- 4K 页：PUD 覆盖 [38:30] 共 9 位，PUD_SHIFT=30，1 << 30 = 1GB，物理地址可以对齐到 1GB 边界，硬件支持 block entry
	Level 0 (PGD)：只允许 table descriptor        ✗ 不能段映射
	Level 1 (PUD)：允许 block descriptor（1GB）   ✓ PUD 段映射
	Level 2 (PMD)：允许 block descriptor（2MB）   ✓ PMD 段映射
	Level 3 (PTE)：只允许 page descriptor         ✗ 不能段映射
- 16K 页：页表结构不同，PUD 级别不存在独立的段映射，硬件不支持
	Level 1 (PUD 等价级)：只允许 table descriptor ✗ 硬件不支持 block
	Level 2 (PMD 等价级)：允许 block descriptor   ✓
- 64K 页：同上，PUD 段映射硬件不支持
	Level 1 (PUD 等价级)：只允许 table descriptor ✗ 硬件不支持 block
	Level 2 (PMD 等价级)：允许 block descriptor   ✓

16K 和 64K 页时，CPU 的 MMU 在遇到 Level 1 条目时根本不检查 block 标志位，即使你把条目格式写成 block descriptor，硬件也会当成 fault 处理。

---
为什么 ARM 这样设计

不同颗粒度下每个页表级别覆盖的地址范围不同：

4K 页：Level 1 覆盖 1GB   → 1GB 的物理连续块在实际中存在，有意义
16K 页：Level 1 覆盖 64GB → 64GB 连续对齐的物理块几乎不存在，没必要支持
64K 页：Level 1 覆盖 4TB  → 完全不现实

覆盖范围太大，物理内存根本凑不出来，ARM 干脆在规范层面就不提供这个能力，简化了硬件实现。

---
所以 pud_sect_supported() 直接用 PAGE_SIZE == SZ_4K 判断，是因为这是 ARM 规范的硬性约束。

---
对 CMA 的影响

4K 页：最大 gigantic 页 = PUD_SIZE = 1GB，CMA 按 1GB 粒度预留
16K/64K：最大 gigantic 页 = CONT PMD，CMA 按 CONT PMD 大小预留
*/
/*
PUD_SHIFT/PAGE_SHIFT/CONT_PMD_SHIFT

这三个都是位移量（shift），表示虚拟/物理地址中某个字段从第几位开始。

---
PAGE_SHIFT — 页内偏移占多少位

PAGE_SIZE = 4KB = 2^12  →  PAGE_SHIFT = 12
PAGE_SIZE = 16KB = 2^14 →  PAGE_SHIFT = 14
PAGE_SIZE = 64KB = 2^16 →  PAGE_SHIFT = 16

地址的低 PAGE_SHIFT 位是页内偏移，高位是页号。1 << PAGE_SHIFT == PAGE_SIZE。

---
PUD_SHIFT — PUD 级别覆盖多少位

4K 页，4 级页表，每级页表索引占 9 位：

虚拟地址 48 位：
  [47:39]  PGD  (9位)  PGDIR_SHIFT = 39
  [38:30]  PUD  (9位)  PUD_SHIFT   = 30  ← 从第 30 位开始
  [29:21]  PMD  (9位)  PMD_SHIFT   = 21
  [20:12]  PTE  (9位)  PAGE_SHIFT  = 12
  [11:0]   页内偏移(12位)

PUD 索引从第 30 位开始，那么第 30 位以下的所有位（bit 29 ~ bit 0）都归一个 PUD 条目管辖
PUD_SHIFT = 30 意味着一个 PUD 条目覆盖 2^30 = 1GB 的地址空间。
PUD_SIZE = 1 << PUD_SHIFT = 1GB。

所以 PUD_SHIFT - PAGE_SHIFT = 30 - 12 = 18，即 1GB 页需要 2^18 个 4K 基础页，order = 18。

---
CONT_PMD_SHIFT — 连续 PMD 大页覆盖多少位

arm64 支持一种硬件优化：将相邻的多个 PMD 条目标记为"连续"（CONT 位），TLB 可以用一个条目缓存整批，减少 TLB 压力：

CONT_PMD_SHIFT = PMD_SHIFT + CONFIG_ARM64_CONT_PMD_SHIFT

CONFIG_ARM64_CONT_PMD_SHIFT 一般来自 Arm 的硬件规范规定的，相同架构是固定的
	4K 页：PMD 级别连续块 = 16 个 PMD 条目 × 2MB = 32MB
			CONFIG_ARM64_CONT_PMD_SHIFT = 4（2^4 = 16 个）

	16K 页：PMD 级别连续块 = 32 个 PMD 条目 × 32MB = 1GB
			CONFIG_ARM64_CONT_PMD_SHIFT = 5（2^5 = 32 个）

	64K 页：PMD 级别连续块 = 16 个 PMD 条目 × 512MB = ?
			实际上 64K 页没有 PMD 级别，对应级别叫 PUD

以 4K 页为例：
PMD_SHIFT = 21（单个 PMD 覆盖 2MB）
CONFIG_ARM64_CONT_PMD_SHIFT = 4（连续 2^4 = 16 个 PMD 条目）
CONT_PMD_SHIFT = 21 + 4 = 25 → CONT_PMD_SIZE = 2^25 = 32MB

CONT_PMD_SHIFT - PAGE_SHIFT = 25 - 12 = 13，即 32MB 页 order = 13。

---
三者关系总结（4K 页）

位地址：  47      39      30      21      12       0
          │  PGD  │  PUD  │  PMD  │  PTE  │  偏移  │
          └───────┘       └───────┘       └────────┘
PGDIR_SHIFT=39   PUD_SHIFT=30   PMD_SHIFT=21   PAGE_SHIFT=12

PUD 段映射直接从 PUD 跳到物理块：覆盖 [29:0] 共 30 位 → 1GB
CONT PMD 连续映射：覆盖 [24:0] 共 25 位 → 32MB

xSHIFT - PAGE_SHIFT 就是"该大页包含多少个基础页"的对数，即 buddy order。
*/
#ifdef CONFIG_CMA
unsigned int arch_hugetlb_cma_order(void)
{
	/* pud_sect_supported() 等价于 PAGE_SIZE == SZ_4K。
	 * 4K 页时 PUD 段映射可用，最大 gigantic 页为 1GiB（PUD_SIZE）。
	 * PUD_SHIFT = 30（4K 页，3 级页表），PAGE_SHIFT = 12，order = 18。 */
	if (pud_sect_supported())
		return PUD_SHIFT - PAGE_SHIFT;

	/* 16K/64K 页时无 PUD 段映射，最大 gigantic 页为 CONT PMD 大页。
	 * CONT_PMD_SHIFT = CONT_PMD_SHIFT + PMD_SHIFT，order = CONT_PMD_SHIFT - PAGE_SHIFT：
	 *   16K 页：CONT_PMD_SHIFT=20，PAGE_SHIFT=14，order=16（对应 1G 页）
	 *   64K 页：CONT_PMD_SHIFT=24，PAGE_SHIFT=16，order=18（对应 16G 页）*/
	return CONT_PMD_SHIFT - PAGE_SHIFT;
}
#endif /* CONFIG_CMA */

static bool __hugetlb_valid_size(unsigned long size)
{
	switch (size) {
#ifndef __PAGETABLE_PMD_FOLDED
	case PUD_SIZE:
		return pud_sect_supported();
#endif
	case CONT_PMD_SIZE:
	case PMD_SIZE:
	case CONT_PTE_SIZE:
		return true;
	}

	return false;
}

#ifdef CONFIG_ARCH_ENABLE_HUGEPAGE_MIGRATION
bool arch_hugetlb_migration_supported(struct hstate *h)
{
	size_t pagesize = huge_page_size(h);

	if (!__hugetlb_valid_size(pagesize)) {
		pr_warn("%s: unrecognized huge page size 0x%lx\n",
			__func__, pagesize);
		return false;
	}
	return true;
}
#endif

static int find_num_contig(struct mm_struct *mm, unsigned long addr,
			   pte_t *ptep, size_t *pgsize)
{
	pgd_t *pgdp = pgd_offset(mm, addr);
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;

	*pgsize = PAGE_SIZE;
	p4dp = p4d_offset(pgdp, addr);
	pudp = pud_offset(p4dp, addr);
	pmdp = pmd_offset(pudp, addr);
	if ((pte_t *)pmdp == ptep) {
		*pgsize = PMD_SIZE;
		return CONT_PMDS;
	}
	return CONT_PTES;
}

static inline int num_contig_ptes(unsigned long size, size_t *pgsize)
{
	int contig_ptes = 1;

	*pgsize = size;

	switch (size) {
	case CONT_PMD_SIZE:
		*pgsize = PMD_SIZE;
		contig_ptes = CONT_PMDS;
		break;
	case CONT_PTE_SIZE:
		*pgsize = PAGE_SIZE;
		contig_ptes = CONT_PTES;
		break;
	default:
		WARN_ON(!__hugetlb_valid_size(size));
	}

	return contig_ptes;
}

pte_t huge_ptep_get(struct mm_struct *mm, unsigned long addr, pte_t *ptep)
{
	int ncontig, i;
	size_t pgsize;
	pte_t orig_pte = __ptep_get(ptep);

	if (!pte_present(orig_pte) || !pte_cont(orig_pte))
		return orig_pte;

	ncontig = find_num_contig(mm, addr, ptep, &pgsize);
	for (i = 0; i < ncontig; i++, ptep++) {
		pte_t pte = __ptep_get(ptep);

		if (pte_dirty(pte))
			orig_pte = pte_mkdirty(orig_pte);

		if (pte_young(pte))
			orig_pte = pte_mkyoung(orig_pte);
	}
	return orig_pte;
}

/*
 * Changing some bits of contiguous entries requires us to follow a
 * Break-Before-Make approach, breaking the whole contiguous set
 * before we can change any entries. See ARM DDI 0487A.k_iss10775,
 * "Misprogramming of the Contiguous bit", page D4-1762.
 *
 * This helper performs the break step.
 */
static pte_t get_clear_contig(struct mm_struct *mm,
			     unsigned long addr,
			     pte_t *ptep,
			     unsigned long pgsize,
			     unsigned long ncontig)
{
	pte_t pte, tmp_pte;
	bool present;

	pte = __ptep_get_and_clear_anysz(mm, addr, ptep, pgsize);
	present = pte_present(pte);
	while (--ncontig) {
		ptep++;
		addr += pgsize;
		tmp_pte = __ptep_get_and_clear_anysz(mm, addr, ptep, pgsize);
		if (present) {
			if (pte_dirty(tmp_pte))
				pte = pte_mkdirty(pte);
			if (pte_young(tmp_pte))
				pte = pte_mkyoung(pte);
		}
	}
	return pte;
}

static pte_t get_clear_contig_flush(struct mm_struct *mm,
				    unsigned long addr,
				    pte_t *ptep,
				    unsigned long pgsize,
				    unsigned long ncontig)
{
	pte_t orig_pte = get_clear_contig(mm, addr, ptep, pgsize, ncontig);
	struct vm_area_struct vma = TLB_FLUSH_VMA(mm, 0);
	unsigned long end = addr + (pgsize * ncontig);

	__flush_hugetlb_tlb_range(&vma, addr, end, pgsize, TLBF_NOWALKCACHE);
	return orig_pte;
}

/*
 * Changing some bits of contiguous entries requires us to follow a
 * Break-Before-Make approach, breaking the whole contiguous set
 * before we can change any entries. See ARM DDI 0487A.k_iss10775,
 * "Misprogramming of the Contiguous bit", page D4-1762.
 *
 * This helper performs the break step for use cases where the
 * original pte is not needed.
 */
static void clear_flush(struct mm_struct *mm,
			     unsigned long addr,
			     pte_t *ptep,
			     unsigned long pgsize,
			     unsigned long ncontig)
{
	struct vm_area_struct vma = TLB_FLUSH_VMA(mm, 0);
	unsigned long i, saddr = addr;

	for (i = 0; i < ncontig; i++, addr += pgsize, ptep++)
		__ptep_get_and_clear_anysz(mm, addr, ptep, pgsize);

	if (mm == &init_mm)
		flush_tlb_kernel_range(saddr, addr);
	else
		__flush_hugetlb_tlb_range(&vma, saddr, addr, pgsize, TLBF_NOWALKCACHE);
}

void set_huge_pte_at(struct mm_struct *mm, unsigned long addr,
			    pte_t *ptep, pte_t pte, unsigned long sz)
{
	size_t pgsize;
	int i;
	int ncontig;

	ncontig = num_contig_ptes(sz, &pgsize);

	if (!pte_present(pte)) {
		for (i = 0; i < ncontig; i++, ptep++, addr += pgsize)
			__set_ptes_anysz(mm, addr, ptep, pte, 1, pgsize);
		return;
	}

	/* Only need to "break" if transitioning valid -> valid. */
	if (pte_cont(pte) && pte_valid(__ptep_get(ptep)))
		clear_flush(mm, addr, ptep, pgsize, ncontig);

	__set_ptes_anysz(mm, addr, ptep, pte, ncontig, pgsize);
}

pte_t *huge_pte_alloc(struct mm_struct *mm, struct vm_area_struct *vma,
		      unsigned long addr, unsigned long sz)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	pte_t *ptep = NULL;

	pgdp = pgd_offset(mm, addr);
	p4dp = p4d_alloc(mm, pgdp, addr);
	if (!p4dp)
		return NULL;

	pudp = pud_alloc(mm, p4dp, addr);
	if (!pudp)
		return NULL;

	if (sz == PUD_SIZE) {
		ptep = (pte_t *)pudp;
	} else if (sz == (CONT_PTE_SIZE)) {
		pmdp = pmd_alloc(mm, pudp, addr);
		if (!pmdp)
			return NULL;

		WARN_ON(addr & (sz - 1));
		ptep = pte_alloc_huge(mm, pmdp, addr);
	} else if (sz == PMD_SIZE) {
		if (want_pmd_share(vma, addr) && pud_none(READ_ONCE(*pudp)))
			ptep = huge_pmd_share(mm, vma, addr, pudp);
		else
			ptep = (pte_t *)pmd_alloc(mm, pudp, addr);
	} else if (sz == (CONT_PMD_SIZE)) {
		pmdp = pmd_alloc(mm, pudp, addr);
		WARN_ON(addr & (sz - 1));
		return (pte_t *)pmdp;
	}

	return ptep;
}

pte_t *huge_pte_offset(struct mm_struct *mm,
		       unsigned long addr, unsigned long sz)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;

	pgdp = pgd_offset(mm, addr);
	if (!pgd_present(READ_ONCE(*pgdp)))
		return NULL;

	p4dp = p4d_offset(pgdp, addr);
	if (!p4d_present(READ_ONCE(*p4dp)))
		return NULL;

	pudp = pud_offset(p4dp, addr);
	pud = READ_ONCE(*pudp);
	if (sz != PUD_SIZE && pud_none(pud))
		return NULL;
	/* hugepage or swap? */
	if (pud_leaf(pud) || !pud_present(pud))
		return (pte_t *)pudp;
	/* table; check the next level */

	if (sz == CONT_PMD_SIZE)
		addr &= CONT_PMD_MASK;

	pmdp = pmd_offset(pudp, addr);
	pmd = READ_ONCE(*pmdp);
	if (!(sz == PMD_SIZE || sz == CONT_PMD_SIZE) &&
	    pmd_none(pmd))
		return NULL;
	if (pmd_leaf(pmd) || !pmd_present(pmd))
		return (pte_t *)pmdp;

	if (sz == CONT_PTE_SIZE)
		return pte_offset_huge(pmdp, (addr & CONT_PTE_MASK));

	return NULL;
}

unsigned long hugetlb_mask_last_page(struct hstate *h)
{
	unsigned long hp_size = huge_page_size(h);

	switch (hp_size) {
#ifndef __PAGETABLE_PMD_FOLDED
	case PUD_SIZE:
		if (pud_sect_supported())
			return PGDIR_SIZE - PUD_SIZE;
		break;
#endif
	case CONT_PMD_SIZE:
		return PUD_SIZE - CONT_PMD_SIZE;
	case PMD_SIZE:
		return PUD_SIZE - PMD_SIZE;
	case CONT_PTE_SIZE:
		return PMD_SIZE - CONT_PTE_SIZE;
	default:
		break;
	}

	return 0UL;
}

pte_t arch_make_huge_pte(pte_t entry, unsigned int shift, vm_flags_t flags)
{
	size_t pagesize = 1UL << shift;

	switch (pagesize) {
#ifndef __PAGETABLE_PMD_FOLDED
	case PUD_SIZE:
		if (pud_sect_supported())
			return pud_pte(pud_mkhuge(pte_pud(entry)));
		break;
#endif
	case CONT_PMD_SIZE:
		return pmd_pte(pmd_mkhuge(pmd_mkcont(pte_pmd(entry))));
	case PMD_SIZE:
		return pmd_pte(pmd_mkhuge(pte_pmd(entry)));
	case CONT_PTE_SIZE:
		return pte_mkcont(entry);
	default:
		break;
	}
	pr_warn("%s: unrecognized huge page size 0x%lx\n",
		__func__, pagesize);
	return entry;
}

void huge_pte_clear(struct mm_struct *mm, unsigned long addr,
		    pte_t *ptep, unsigned long sz)
{
	int i, ncontig;
	size_t pgsize;

	ncontig = num_contig_ptes(sz, &pgsize);

	for (i = 0; i < ncontig; i++, addr += pgsize, ptep++)
		__pte_clear(mm, addr, ptep);
}

pte_t huge_ptep_get_and_clear(struct mm_struct *mm, unsigned long addr,
			      pte_t *ptep, unsigned long sz)
{
	int ncontig;
	size_t pgsize;

	ncontig = num_contig_ptes(sz, &pgsize);
	return get_clear_contig(mm, addr, ptep, pgsize, ncontig);
}

/*
 * huge_ptep_set_access_flags will update access flags (dirty, accesssed)
 * and write permission.
 *
 * For a contiguous huge pte range we need to check whether or not write
 * permission has to change only on the first pte in the set. Then for
 * all the contiguous ptes we need to check whether or not there is a
 * discrepancy between dirty or young.
 */
static int __cont_access_flags_changed(pte_t *ptep, pte_t pte, int ncontig)
{
	int i;

	if (pte_write(pte) != pte_write(__ptep_get(ptep)))
		return 1;

	for (i = 0; i < ncontig; i++) {
		pte_t orig_pte = __ptep_get(ptep + i);

		if (pte_dirty(pte) != pte_dirty(orig_pte))
			return 1;

		if (pte_young(pte) != pte_young(orig_pte))
			return 1;
	}

	return 0;
}

int huge_ptep_set_access_flags(struct vm_area_struct *vma,
			       unsigned long addr, pte_t *ptep,
			       pte_t pte, int dirty)
{
	int ncontig;
	size_t pgsize = 0;
	struct mm_struct *mm = vma->vm_mm;
	pte_t orig_pte;

	VM_WARN_ON(!pte_present(pte));
	ncontig = num_contig_ptes(huge_page_size(hstate_vma(vma)), &pgsize);

	if (!pte_cont(pte))
		return __ptep_set_access_flags_anysz(vma, addr, ptep, pte,
						     dirty, pgsize);

	if (!__cont_access_flags_changed(ptep, pte, ncontig))
		return 0;

	orig_pte = get_clear_contig_flush(mm, addr, ptep, pgsize, ncontig);
	VM_WARN_ON(!pte_present(orig_pte));

	/* Make sure we don't lose the dirty or young state */
	if (pte_dirty(orig_pte))
		pte = pte_mkdirty(pte);

	if (pte_young(orig_pte))
		pte = pte_mkyoung(pte);

	__set_ptes_anysz(mm, addr, ptep, pte, ncontig, pgsize);
	return 1;
}

void huge_ptep_set_wrprotect(struct mm_struct *mm,
			     unsigned long addr, pte_t *ptep)
{
	int ncontig;
	size_t pgsize;
	pte_t pte;

	pte = __ptep_get(ptep);
	VM_WARN_ON(!pte_present(pte));

	if (!pte_cont(pte)) {
		__ptep_set_wrprotect(mm, addr, ptep);
		return;
	}

	ncontig = find_num_contig(mm, addr, ptep, &pgsize);

	pte = get_clear_contig_flush(mm, addr, ptep, pgsize, ncontig);
	pte = pte_wrprotect(pte);

	__set_ptes_anysz(mm, addr, ptep, pte, ncontig, pgsize);
}

pte_t huge_ptep_clear_flush(struct vm_area_struct *vma,
			    unsigned long addr, pte_t *ptep)
{
	struct mm_struct *mm = vma->vm_mm;
	size_t pgsize;
	int ncontig;

	ncontig = num_contig_ptes(huge_page_size(hstate_vma(vma)), &pgsize);
	return get_clear_contig_flush(mm, addr, ptep, pgsize, ncontig);
}

static int __init hugetlbpage_init(void)
{
	/*
	 * HugeTLB pages are supported on maximum four page table
	 * levels (PUD, CONT PMD, PMD, CONT PTE) for a given base
	 * page size, corresponding to hugetlb_add_hstate() calls
	 * here.
	 *
	 * HUGE_MAX_HSTATE should at least match maximum supported
	 * HugeTLB page sizes on the platform. Any new addition to
	 * supported HugeTLB page sizes will also require changing
	 * HUGE_MAX_HSTATE as well.
	 */
	BUILD_BUG_ON(HUGE_MAX_HSTATE < 4);
	if (pud_sect_supported())
		hugetlb_add_hstate(PUD_SHIFT - PAGE_SHIFT);

	hugetlb_add_hstate(CONT_PMD_SHIFT - PAGE_SHIFT);
	hugetlb_add_hstate(PMD_SHIFT - PAGE_SHIFT);
	hugetlb_add_hstate(CONT_PTE_SHIFT - PAGE_SHIFT);

	return 0;
}
arch_initcall(hugetlbpage_init);

bool __init arch_hugetlb_valid_size(unsigned long size)
{
	return __hugetlb_valid_size(size);
}

pte_t huge_ptep_modify_prot_start(struct vm_area_struct *vma, unsigned long addr, pte_t *ptep)
{
	unsigned long psize = huge_page_size(hstate_vma(vma));

	if (alternative_has_cap_unlikely(ARM64_WORKAROUND_2645198)) {
		/*
		 * Break-before-make (BBM) is required for all user space mappings
		 * when the permission changes from executable to non-executable
		 * in cases where cpu is affected with errata #2645198.
		 */
		if (pte_user_exec(__ptep_get(ptep)))
			return huge_ptep_clear_flush(vma, addr, ptep);
	}
	return huge_ptep_get_and_clear(vma->vm_mm, addr, ptep, psize);
}

void huge_ptep_modify_prot_commit(struct vm_area_struct *vma, unsigned long addr, pte_t *ptep,
				  pte_t old_pte, pte_t pte)
{
	unsigned long psize = huge_page_size(hstate_vma(vma));

	set_huge_pte_at(vma->vm_mm, addr, ptep, pte, psize);
}
