// SPDX-License-Identifier: GPL-2.0
/*
 * Virtual Memory Map support
 *
 * (C) 2007 sgi. Christoph Lameter.
 *
 * Virtual memory maps allow VM primitives pfn_to_page, page_to_pfn,
 * virt_to_page, page_address() to be implemented as a base offset
 * calculation without memory access.
 *
 * However, virtual mappings need a page table and TLBs. Many Linux
 * architectures already map their physical space using 1-1 mappings
 * via TLBs. For those arches the virtual memory map is essentially
 * for free if we use the same page size as the 1-1 mappings. In that
 * case the overhead consists of a few additional pages that are
 * allocated to create a view of memory for vmemmap.
 *
 * The architecture is expected to provide a vmemmap_populate() function
 * to instantiate the mapping.
 */
/* vmemmap 用虚拟连续的 struct page 数组换取 PFN/O(1) 地址转换；实际 backing 可来自普通页、启动期 memblock 或设备 altmap。 */
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/memblock.h>
/* MM/zone/memblock 提供 PFN、section 与启动分配的共同基础，决定 vmemmap 地址和 backing 的换算。 */
#include <linux/memremap.h>
#include <linux/highmem.h>
#include <linux/slab.h>
/* dev_pagemap/高端页/SLAB 覆盖设备自托管、地址访问和运行期后端三类路径。 */
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/pgalloc.h>
/* 页表建立可能让出 CPU；pgalloc 与 arch TLB 接口共同保证 kernel 页表项的正确发布。 */

#include <asm/dma.h>
#include <asm/tlbflush.h>
/* DMA 上限限制启动期 backing 位置；TLB 刷新接口由 HVO 写保护调用者配对。 */

#include "hugetlb_vmemmap.h"

/*
 * Flags for vmemmap_populate_range and friends.
 */
/* 这些 flag 只改变已存在 backing 页的引用处理，不改变页表项的虚拟地址布局。 */
/* Get a ref on the head page struct page, for ZONE_DEVICE compound pages */
/* 复用复合页尾部元数据页时，额外引用要与 init_mm 拆映射路径的 put_page_testzero 配对。 */
#define VMEMMAP_POPULATE_PAGEREF	0x0001

#include "internal.h"

/*
 * Allocate a block of memory to be used to back the virtual memory map
 * or to back the page tables that are used to create the mapping.
 * Uses the main allocators if they are available, else bootmem.
 */

static void * __ref __earlyonly_bootmem_alloc(int node,
				unsigned long size,
				unsigned long align,
				unsigned long goal)
{
	/* buddy/slab 尚不可用时从 memblock 取得 page-table 或 memmap backing；goal 约束 DMA 可达性。 */
	return memmap_alloc(size, align, goal, node, false);
}

void * __meminit vmemmap_alloc_block(unsigned long size, int node)
{
	/* 运行期优先按节点分配可回收普通页；失败仅告警并返回 NULL，由上层停止当前映射建立。 */
	/* If the main allocator is up use that, fallback to bootmem. */
	/* 启动期没有 buddy 时退回 bootmem；两条路径都返回内核虚拟地址而非 struct page。 */
	if (slab_is_available()) {
		gfp_t gfp_mask = GFP_KERNEL|__GFP_RETRY_MAYFAIL|__GFP_NOWARN;
		int order = get_order(size);
		static bool warned __meminitdata;
		struct page *page;

		page = alloc_pages_node(node, gfp_mask, order);
		/* 分配的 order 覆盖完整 size，调用者负责把该块初始化为页表或零化 metadata backing。 */
		/* 成功立即以线性映射地址交付；没有额外页引用转移，后续 vmemmap_free 按页表映射回收。 */
		if (page)
			return page_address(page);

		if (!warned) {
			warn_alloc(gfp_mask & ~__GFP_NOWARN, NULL,
				   "vmemmap alloc failure: order:%u", order);
			warned = true;
		}
		/* warned 只抑制重复日志，不改变失败语义；NULL 会沿调用栈传播到 section 初始化的回滚点。 */
		return NULL;
	} else
		return __earlyonly_bootmem_alloc(node, size, size,
				__pa(MAX_DMA_ADDRESS));
}

static void * __meminit altmap_alloc_block_buf(unsigned long size,
					       struct vmem_altmap *altmap);

/* need to make sure size is all the same during early stage */
void * __meminit vmemmap_alloc_block_buf(unsigned long size, int node,
					 struct vmem_altmap *altmap)
{
	/* altmap 存在时必须消耗设备预留 PFN，避免 vmemmap 自身占用系统 RAM；否则沿用普通后端。 */
	if (altmap)
		return altmap_alloc_block_buf(size, altmap);

	return vmemmap_alloc_block(size, node);
}

static unsigned long __meminit vmem_altmap_next_pfn(struct vmem_altmap *altmap)
{
	/* reserve 是驱动保留前缀，alloc 是已用 backing，align 是为页表块对齐留下的跳洞。 */
	return altmap->base_pfn + altmap->reserve + altmap->alloc
		+ altmap->align;
}

static unsigned long __meminit vmem_altmap_nr_free(struct vmem_altmap *altmap)
{
	/* free 是可供 vmemmap 消费的总额；已分配和对齐浪费都不能再次计入可用预算。 */
	unsigned long allocated = altmap->alloc + altmap->align;

	if (altmap->free > allocated)
		return altmap->free - allocated;
	return 0;
}

static void * __meminit altmap_alloc_block_buf(unsigned long size,
					       struct vmem_altmap *altmap)
{
	/* 从设备 altmap 线性领取页对齐块；失败不修改 alloc/align，供调用者以 -ENOMEM 原子退化。 */
	unsigned long pfn, nr_pfns, nr_align;

	if (size & ~PAGE_MASK) {
		/* 页表/backing 的基本分配单位是整页，非页倍数会破坏 PFN 与虚拟映射一一对应。 */
		pr_warn_once("%s: allocations must be multiple of PAGE_SIZE (%ld)\n",
				__func__, size);
		return NULL;
	}

	pfn = vmem_altmap_next_pfn(altmap);
	nr_pfns = size >> PAGE_SHIFT;
	nr_align = 1UL << find_first_bit(&nr_pfns, BITS_PER_LONG);
	nr_align = ALIGN(pfn, nr_align) - pfn;
	/* 对齐粒度取请求页数的低位 2 次幂，令大块映射的物理起点满足架构页表约束。 */
	if (nr_pfns + nr_align > vmem_altmap_nr_free(altmap))
		return NULL;

	altmap->alloc += nr_pfns;
	/* 先把账本推进再返回虚拟地址，后续失败不会把同一设备 PFN 重复借给另一个映射。 */
	altmap->align += nr_align;
	pfn += nr_align;

	pr_debug("%s: pfn: %#lx alloc: %ld align: %ld nr: %#lx\n",
			__func__, pfn, altmap->alloc, altmap->align, nr_pfns);
	return __va(__pfn_to_phys(pfn));
}

void __meminit vmemmap_verify(pte_t *pte, int node,
				unsigned long start, unsigned long end)
{
	/* 仅诊断 backing 页与目标 node 的距离；不改变映射，跨节点过远时输出一次性告警。 */
	unsigned long pfn = pte_pfn(ptep_get(pte));
	int actual_node = early_pfn_to_nid(pfn);

	if (node_distance(actual_node, node) > LOCAL_DISTANCE)
		pr_warn_once("[%lx-%lx] potential offnode page_structs\n",
			start, end - 1);
}

static pte_t * __meminit vmemmap_pte_populate(pmd_t *pmd, unsigned long addr, int node,
				       struct vmem_altmap *altmap,
				       unsigned long ptpfn, unsigned long flags)
{
	/* 确保 addr 的最低级 PTE 存在；新建时分配 backing，复用时按 flag 获取与拆映射配对的页引用。 */
	pte_t *pte = pte_offset_kernel(pmd, addr);
	if (pte_none(ptep_get(pte))) {
		/* 已存在 PTE 保持原映射，保证多段 populate 或共享尾页不会重复覆盖页表项。 */
		pte_t entry;
		void *p;

		if (ptpfn == (unsigned long)-1) {
			/* -1 是“为此 PTE 新取 backing”的内部哨兵；取得后转换为可直接编码的 PFN。 */
			p = vmemmap_alloc_block_buf(PAGE_SIZE, node, altmap);
			if (!p)
				return NULL;
			ptpfn = PHYS_PFN(__pa(p));
		} else {
			/*
			 * When a PTE/PMD entry is freed from the init_mm
			 * there's a free_pages() call to this page allocated
			 * above. Thus this get_page() is paired with the
			 * put_page_testzero() on the freeing path.
			 * This can only called by certain ZONE_DEVICE path,
			 * and through vmemmap_populate_compound_pages() when
			 * slab is available.
			 */
			if (flags & VMEMMAP_POPULATE_PAGEREF)
				/* ZONE_DEVICE 共享 tail metadata 时每个新增映射各持一引用，避免先拆者释放 backing。 */
				get_page(pfn_to_page(ptpfn));
		}
		entry = pfn_pte(ptpfn, PAGE_KERNEL);
		/* PTE 发布在 backing 就绪及引用取得之后；init_mm 是所有 vmemmap 地址的共同页表根。 */
		set_pte_at(&init_mm, addr, pte, entry);
	}
	return pte;
}

static void * __meminit vmemmap_alloc_block_zero(unsigned long size, int node)
{
	/* 页表页必须在链接前清零，避免旧条目被 walk 当作已有下级表或映射。 */
	void *p = vmemmap_alloc_block(size, node);

	if (!p)
		return NULL;
	memset(p, 0, size);

	return p;
}

static pmd_t * __meminit vmemmap_pmd_populate(pud_t *pud, unsigned long addr, int node)
{
	/* 逐级确保 PMD 表页存在；返回对应槽位，失败把 NULL 交给上层中止当前 address 的建立。 */
	pmd_t *pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd)) {
		/* 新 PMD 表先完成架构 PTE 初始化，再以 kernel mapping 形式挂到 init_mm。 */
		void *p = vmemmap_alloc_block_zero(PAGE_SIZE, node);
		if (!p)
			return NULL;
		kernel_pte_init(p);
		pmd_populate_kernel(&init_mm, pmd, p);
	}
	return pmd;
}

static pud_t * __meminit vmemmap_pud_populate(p4d_t *p4d, unsigned long addr, int node)
{
	/* PUD 层只负责承接新清零 PMD 表页；已有条目保持共享，支持分段重复调用。 */
	pud_t *pud = pud_offset(p4d, addr);
	if (pud_none(*pud)) {
		void *p = vmemmap_alloc_block_zero(PAGE_SIZE, node);
		if (!p)
			return NULL;
		pmd_init(p);
		pud_populate(&init_mm, pud, p);
		/* pmd_init 完成下级表的架构初态后才发布 PUD，读者不会见到未初始化的 PMD 内容。 */
	}
	return pud;
}

static p4d_t * __meminit vmemmap_p4d_populate(pgd_t *pgd, unsigned long addr, int node)
{
	/* P4D 层将清零的 PUD 表页接入 kernel page table，内存不足直接让父调用者退出。 */
	p4d_t *p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d)) {
		void *p = vmemmap_alloc_block_zero(PAGE_SIZE, node);
		if (!p)
			return NULL;
		pud_init(p);
		p4d_populate_kernel(addr, p4d, p);
		/* P4D 发布把地址相关属性交给架构 helper；pud_init 的清零语义先于该可见性点。 */
	}
	return p4d;
}

static pgd_t * __meminit vmemmap_pgd_populate(unsigned long addr, int node)
{
	/* 顶层 PGD 从内核共享页表取得；仅在空槽时分配下一层，避免覆盖既有 vmemmap 区间。 */
	pgd_t *pgd = pgd_offset_k(addr);
	if (pgd_none(*pgd)) {
		void *p = vmemmap_alloc_block_zero(PAGE_SIZE, node);
		if (!p)
			return NULL;
		pgd_populate_kernel(addr, pgd, p);
		/* 顶层发布后后续 range 可共享该页表分支；其失败前没有向 PGD 写入半初始化指针。 */
	}
	return pgd;
}

static pte_t * __meminit vmemmap_populate_address(unsigned long addr, int node,
					      struct vmem_altmap *altmap,
					      unsigned long ptpfn,
					      unsigned long flags)
{
	/* 单地址事务按 PGD→P4D→PUD→PMD→PTE 下降；任一层失败即返回 NULL，已建祖先表可被后续调用复用。 */
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = vmemmap_pgd_populate(addr, node);
	/* 每层成功才可解引用下一层，避免启动期内存不足时产生部分层级的空指针 walk。 */
	if (!pgd)
		return NULL;
	p4d = vmemmap_p4d_populate(pgd, addr, node);
	if (!p4d)
		return NULL;
	/* PGD/P4D 成功只代表容器存在；继续下降时每层均保留独立的内存不足出口。 */
	pud = vmemmap_pud_populate(p4d, addr, node);
	if (!pud)
		return NULL;
	pmd = vmemmap_pmd_populate(pud, addr, node);
	if (!pmd)
		return NULL;
	/* 最低级 helper 才会分配或复用实际 vmemmap backing，并可按 PAGEREF 建立共享生命周期。 */
	pte = vmemmap_pte_populate(pmd, addr, node, altmap, ptpfn, flags);
	if (!pte)
		return NULL;
	vmemmap_verify(pte, node, addr, addr + PAGE_SIZE);
	/* 建立后做 NUMA 就近性诊断，不参与成功判定，也不会尝试迁移 backing。 */

	return pte;
}

static int __meminit vmemmap_populate_range(unsigned long start,
					    unsigned long end, int node,
					    struct vmem_altmap *altmap,
					    unsigned long ptpfn,
					    unsigned long flags)
{
	/* 按基础页遍历半开虚拟区间；返回 -ENOMEM 表示当前及后续地址未保证映射，调用者负责放弃 section。 */
	unsigned long addr = start;
	pte_t *pte;

	for (; addr < end; addr += PAGE_SIZE) {
		/* 同一 ptpfn 可让多个虚拟页映射到共享 metadata backing，flags 决定共享引用是否递增。 */
		pte = vmemmap_populate_address(addr, node, altmap,
					       ptpfn, flags);
		if (!pte)
			return -ENOMEM;
	}

	return 0;
}

int __meminit vmemmap_populate_basepages(unsigned long start, unsigned long end,
					 int node, struct vmem_altmap *altmap)
{
	/* 基页后端不传共享 PFN/引用 flag；每个 vmemmap 虚拟页独立获得一个 backing 页。 */
	return vmemmap_populate_range(start, end, node, altmap, -1, 0);
}

/*
 * Write protect the mirrored tail page structs for HVO. This will be
 * called from the hugetlb code when gathering and initializing the
 * memblock allocated gigantic pages. The write protect can't be
 * done earlier, since it can't be guaranteed that the reserved
 * page structures will not be written to during initialization,
 * even if CONFIG_DEFERRED_STRUCT_PAGE_INIT is enabled.
 *
 * The PTEs are known to exist, and nothing else should be touching
 * these pages. The caller is responsible for any TLB flushing.
 */
/* HVO 在 gigantic 页的 memmap 初始化完成后才锁住镜像 tail；过早写保护会阻断启动期 struct page 填充，TLB 同步归调用者。 */
void vmemmap_wrprotect_hvo(unsigned long addr, unsigned long end,
				    int node, unsigned long headsize)
{
	/* gigantic hugetlb 页初始化结束后保护镜像 tail struct page；调用者持有排他上下文并自行完成 TLB 刷新。 */
	unsigned long maddr;
	pte_t *pte;

	for (maddr = addr + headsize; maddr < end; maddr += PAGE_SIZE) {
		/* headsize 前的真实 metadata 保持可写，后段复用尾页只允许读取，防止初始化后再被误改。 */
		pte = virt_to_kpte(maddr);
		ptep_set_wrprotect(&init_mm, maddr, pte);
	}
}

#ifdef CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP
static __meminit struct page *vmemmap_get_tail(unsigned int order, struct zone *zone)
{
	/* 按 huge folio order 为每个 zone 懒建一个可复用 tail metadata 页；返回页引用由 zone 长期保存。 */
	struct page *p, *tail;
	unsigned int idx;
	int node = zone_to_nid(zone);

	if (WARN_ON_ONCE(order < VMEMMAP_TAIL_MIN_ORDER))
		return NULL;
	if (WARN_ON_ONCE(order > MAX_FOLIO_ORDER))
		return NULL;

	idx = order - VMEMMAP_TAIL_MIN_ORDER;
	/* 数组索引从最小可优化 order 归零，越界已由前述 WARN 防住。 */
	tail = zone->vmemmap_tails[idx];
	/* 已缓存 tail 不重新分配或初始化，确保同类型 gigantic folio 共享同一镜像页。 */
	if (tail)
		return tail;

	/*
	 * Only allocate the page, but do not initialize it.
	 *
	 * Any initialization done here will be overwritten by memmap_init().
	 *
	 * hugetlb_vmemmap_init() will take care of initialization after
	 * memmap_init().
	 */
	/* 该缓存页刻意保留为未初始化的 page backing：通用 memmap_init 先写通用字段，Hugetlb 随后按复用布局接管。 */

	p = vmemmap_alloc_block_zero(PAGE_SIZE, node);
	/* 此刻仅分配清零 backing；memmap_init 会覆盖 struct page 内容，后续 hugetlb hook 再完成专有初始化。 */
	if (!p)
		return NULL;

	tail = virt_to_page(p);
	zone->vmemmap_tails[idx] = tail;

	return tail;
}

int __meminit vmemmap_populate_hvo(unsigned long addr, unsigned long end,
				       unsigned int order, struct zone *zone,
				       unsigned long headsize)
{
	/* HVO 先为 headsize 建真实 vmemmap，再让余下 tail 虚拟页指向 zone 共享 tail backing，降低巨页元数据占用。 */
	unsigned long maddr;
	struct page *tail;
	pte_t *pte;
	int node = zone_to_nid(zone);

	tail = vmemmap_get_tail(order, zone);
	/* tail 获取失败不留下后段共享映射；上层以 -ENOMEM 放弃该优化/页初始化。 */
	if (!tail)
		return -ENOMEM;

	for (maddr = addr; maddr < addr + headsize; maddr += PAGE_SIZE) {
		/* 真实 head metadata 必须逐页独立映射，才能容纳 folio 头及必要的首批 tail 描述。 */
		pte = vmemmap_populate_address(maddr, node, NULL, -1, 0);
		if (!pte)
			return -ENOMEM;
	}

	/*
	 * Reuse the last page struct page mapped above for the rest.
	 */
	return vmemmap_populate_range(maddr, end, node, NULL,
	/* 剩余 PTE 均映射同一 tail PFN；共享引用规则由普通 range helper 处理。 */
				      page_to_pfn(tail), 0);
}
#endif

void __weak __meminit vmemmap_set_pmd(pmd_t *pmd, void *p, int node,
				      unsigned long addr, unsigned long next)
{
	/* 架构可覆盖 huge PMD 的属性/建表方式；默认实现要求把连续 PMD_SIZE backing 映射为 leaf。 */
	WARN_ON_ONCE(!pmd_set_huge(pmd, virt_to_phys(p), PAGE_KERNEL));
}

int __weak __meminit vmemmap_check_pmd(pmd_t *pmd, int node,
				       unsigned long addr, unsigned long next)
{
	/* 已有 PMD 若是 leaf 即验证并复用；若是下级 PTE 表，返回 0 让调用者继续基页 populate。 */
	if (!pmd_leaf(pmdp_get(pmd)))
		return 0;
	vmemmap_verify((pte_t *)pmd, node, addr, next);

	return 1;
}

int __meminit vmemmap_populate_hugepages(unsigned long start, unsigned long end,
					 int node, struct vmem_altmap *altmap)
{
	/* 优先把完整 PMD 范围映射为 huge leaf，altmap 存在时不得退回系统页，以免设备自托管账本失真。 */
	unsigned long addr;
	unsigned long next;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	for (addr = start; addr < end; addr = next) {
		/* next 截断到当前 PMD，故末段不足 PMD 时自然落入 basepages 后备路径。 */
		next = pmd_addr_end(addr, end);

		pgd = vmemmap_pgd_populate(addr, node);
		if (!pgd)
			return -ENOMEM;

		p4d = vmemmap_p4d_populate(pgd, addr, node);
		if (!p4d)
			return -ENOMEM;
		/* huge PMD 仍依赖上三级完整存在，任意建表失败不允许改用错误 node 的页表页。 */

		pud = vmemmap_pud_populate(p4d, addr, node);
		if (!pud)
			return -ENOMEM;

		pmd = pmd_offset(pud, addr);
		/* 上三级只保证页表容器存在；PMD 槽决定此区间能否直接安装 huge 映射。 */
		if (pmd_none(pmdp_get(pmd))) {
			void *p;

			p = vmemmap_alloc_block_buf(PMD_SIZE, node, altmap);
			/* 连续 PMD_SIZE backing 成功时由架构钩子发布 leaf，随后直接处理下一个 PMD。 */
			if (p) {
				vmemmap_set_pmd(pmd, p, node, addr, next);
				continue;
			} else if (altmap) {
				/*
				 * No fallback: In any case we care about, the
				 * altmap should be reasonably sized and aligned
				 * such that vmemmap_alloc_block_buf() will always
				 * succeed. For consistency with the PTE case,
				 * return an error here as failure could indicate
				 * a configuration issue with the size of the altmap.
				 */
			/* altmap 语义要求所有 backing 都来自设备预留范围；空间或对齐不足是配置错误，不能偷用普通内存掩盖。 */
				return -ENOMEM;
			}
		} else if (vmemmap_check_pmd(pmd, node, addr, next))
			/* 既有 huge leaf 无需重建；检查函数返回真才可跳过此 PMD。 */
			continue;
		/* 无 huge backing 或已有 PTE 表时逐页补齐，保证普通系统内存仍能启动。 */
		if (vmemmap_populate_basepages(addr, next, node, altmap))
			return -ENOMEM;
	}
	return 0;
}

#ifndef vmemmap_populate_compound_pages
/*
 * For compound pages bigger than section size (e.g. x86 1G compound
 * pages with 2M subsection size) fill the rest of sections as tail
 * pages.
 *
 * Note that memremap_pages() resets @nr_range value and will increment
 * it after each range successful onlining. Thus the value or @nr_range
 * at section memmap populate corresponds to the in-progress range
 * being onlined here.
 */
/* memremap 的 nr_range 随每段成功上线推进；本 helper 只据当前进行中 range 判断 compound 对齐和跨 section 复用。 */
static bool __meminit reuse_compound_section(unsigned long start_pfn,
					     struct dev_pagemap *pgmap)
{
	/* 设备复合 folio 跨 subsection 时，非复合对齐的后续 section 只需复用前一段尾页 metadata。 */
	unsigned long nr_pages = pgmap_vmemmap_nr(pgmap);
	unsigned long offset = start_pfn -
		PHYS_PFN(pgmap->ranges[pgmap->nr_range].start);

	return !IS_ALIGNED(offset, nr_pages) && nr_pages > PAGES_PER_SUBSECTION;
}

static pte_t * __meminit compound_section_tail_page(unsigned long addr)
{
	/* 假定 section 顺序 populate，向前取上一 section 最后一个 PTE，作为本 section 共享 tail backing 的来源。 */
	pte_t *pte;

	addr -= PAGE_SIZE;
	/* start 指向本 section 首虚拟页；减一页才是已建立 tail PTE，空指针表示顺序/页表前提被破坏。 */

	/*
	 * Assuming sections are populated sequentially, the previous section's
	 * page data can be reused.
	 */
	/* 该顺序前提将上一 section 最后一页作为本段 tail 的共享 backing；若页表不存在即返回失败而不猜测 PFN。 */
	pte = pte_offset_kernel(pmd_off_k(addr), addr);
	if (!pte)
		return NULL;

	return pte;
}

static int __meminit vmemmap_populate_compound_pages(unsigned long start_pfn,
						     unsigned long start,
						     unsigned long end, int node,
						     struct dev_pagemap *pgmap)
{
	/* 为 ZONE_DEVICE 复合 folio 建“head 页 + 首 tail 页 + 镜像剩余 tail 页”的 vmemmap；每段失败返回 -ENOMEM。 */
	unsigned long size, addr;
	pte_t *pte;
	int rc;

	if (reuse_compound_section(start_pfn, pgmap)) {
		/* 跨 section 的连续 folio 不重复建 head/tail，借前段尾页并为新增映射增加 backing 引用。 */
		pte = compound_section_tail_page(start);
		if (!pte)
			return -ENOMEM;

		/*
		 * Reuse the page that was populated in the prior iteration
		 * with just tail struct pages.
		 */
		return vmemmap_populate_range(start, end, node, NULL,
					      pte_pfn(ptep_get(pte)),
					      VMEMMAP_POPULATE_PAGEREF);
	}

	size = min(end - start, pgmap_vmemmap_nr(pgmap) * sizeof(struct page));
	/* 一段恰覆盖一个复合 folio 的 metadata 虚拟跨度，末段可小于该跨度。 */
	for (addr = start; addr < end; addr += size) {
		/* 每个 compound 单元先独立映射 head，再独立映射一个 tail 页，其余 tail 均复用后者。 */
		unsigned long next, last = addr + size;

		/* Populate the head page vmemmap page */
		pte = vmemmap_populate_address(addr, node, NULL, -1, 0);
		/* head PTE 不能共享，因为它的 struct page 记录该 compound folio 的唯一身份和状态。 */
		if (!pte)
			return -ENOMEM;

		/* Populate the tail pages vmemmap page */
		next = addr + PAGE_SIZE;
		/* 第一个 tail backing 既存放真实 tail 描述，也充当后续镜像 PTE 的共享来源。 */
		pte = vmemmap_populate_address(next, node, NULL, -1, 0);
		if (!pte)
			return -ENOMEM;
		/* 取得 tail PTE 后才可读取其 PFN；该 PFN 是下一步共享映射的唯一 backing 来源。 */

		/*
		 * Reuse the previous page for the rest of tail pages
		 * See layout diagram in Documentation/mm/vmemmap_dedup.rst
		 */
	/* 复用减少的是 tail struct page 的物理 backing，不改变每个虚拟 struct page 的地址连续性。 */
		next += PAGE_SIZE;
		/* 从第二个 tail 虚拟页开始共享；PAGEREF 使每个 PTE 生命周期都计入该 backing 页。 */
		rc = vmemmap_populate_range(next, last, node, NULL,
					    pte_pfn(ptep_get(pte)),
					    VMEMMAP_POPULATE_PAGEREF);
		if (rc)
			return -ENOMEM;
	}

	/* 每个 compound 单元均已建立 head 和共享 tail；循环结束表示整个半开区间可作为 section memmap 发布。 */
	return 0;
}

#endif

struct page * __meminit __populate_section_memmap(unsigned long pfn,
		unsigned long nr_pages, int nid, struct vmem_altmap *altmap,
		struct dev_pagemap *pgmap)
{
	/* 启动及热插拔共同入口：把 PFN 范围换算为 struct page 虚拟区间，选择复合优化或普通架构 populate。 */
	unsigned long start = (unsigned long) pfn_to_page(pfn);
	unsigned long end = start + nr_pages * sizeof(struct page);
	int r;

	if (WARN_ON_ONCE(!IS_ALIGNED(pfn, PAGES_PER_SUBSECTION) ||
		!IS_ALIGNED(nr_pages, PAGES_PER_SUBSECTION)))
		return NULL;
	/* subsection 对齐是 section usage bitmap 与 vmemmap 映射的共同粒度，违反时不得建立部分元数据。 */

	if (vmemmap_can_optimize(altmap, pgmap))
		/* 仅合格的设备 altmap/pgmap 几何可启用 compound 尾页复用；普通内存走架构的常规入口。 */
		r = vmemmap_populate_compound_pages(pfn, start, end, nid, pgmap);
	else
		r = vmemmap_populate(start, end, nid, altmap);

	if (r < 0)
		/* 下级已经保留必要的错误语义；NULL 让 sparse 初始化或 hotplug 上层执行其对应回退。 */
		return NULL;

	return pfn_to_page(pfn);
}

#ifdef CONFIG_SPARSEMEM_VMEMMAP_PREINIT
/*
 * This is called just before initializing sections for a NUMA node.
 * Any special initialization that needs to be done before the
 * generic initialization can be done from here. Sections that
 * are initialized in hooks called from here will be skipped by
 * the generic initialization.
 */
/* 早期 hook 的责任是把 Hugetlb 特殊 section 标记为已预初始化，通用 sparse 初始化据此不覆盖其 memmap/usage。 */
void __init sparse_vmemmap_init_nid_early(int nid)
{
	/* 通用 sparse 初始化之前把 Hugetlb 专用 vmemmap section 预建；标记后的 section 会被 generic loop 跳过。 */
	hugetlb_vmemmap_init_early(nid);
}

/*
 * This is called just before the initialization of page structures
 * through memmap_init. Zones are now initialized, so any work that
 * needs to be done that needs zone information can be done from
 * here.
 */
/* 晚期 hook 位于通用 page struct 初始化之后，因而允许依赖 zone 归属和 vmemmap tail 缓存。 */
void __init sparse_vmemmap_init_nid_late(int nid)
{
	/* memmap_init 后 zone 已可查询，Hugetlb 在此补齐依赖 zone 的 tail/backing 初始化。 */
	hugetlb_vmemmap_init_late(nid);
}
#endif

static void subsection_mask_set(unsigned long *map, unsigned long pfn,
		unsigned long nr_pages)
{
	/* 将 PFN 半开区间换成首尾 subsection 位并一次置位；调用者保证 nr_pages 非零且不跨自己的 bitmap 语义边界。 */
	int idx = subsection_map_index(pfn);
	int end = subsection_map_index(pfn + nr_pages - 1);

	bitmap_set(map, idx, end - idx + 1);
	/* 位图记录“本 section 哪些 subsection 有可用内存”，不是 page-online 状态本身。 */
}

void __init sparse_init_subsection_map(unsigned long pfn, unsigned long nr_pages)
{
	/* 启动期把可能跨 section 的 PFN 区间拆分为每个 section 的 subsection 位图提交，供 pfn_valid 等查询使用。 */
	int end_sec_nr = pfn_to_section_nr(pfn + nr_pages - 1);
	unsigned long nr, start_sec_nr = pfn_to_section_nr(pfn);

	for (nr = start_sec_nr; nr <= end_sec_nr; nr++) {
		/* 每轮只消耗当前 section 剩余容量，随后推进 pfn/nr_pages，直至原请求完整覆盖。 */
		struct mem_section *ms;
		unsigned long pfns;

		pfns = min(nr_pages, PAGES_PER_SECTION
				- (pfn & ~PAGE_SECTION_MASK));
		/* pfns 取当前 section 尾部与剩余请求的较小值，确保跨 section 不会写出本 section 的 bitmap。 */
		ms = __nr_to_section(nr);
		/* usage 已由 section 初始化准备；这里仅发布 present 子区间，不分配或释放 vmemmap backing。 */
		subsection_mask_set(ms->usage->subsection_map, pfn, pfns);

		pr_debug("%s: sec: %lu pfns: %lu set(%d, %d)\n", __func__, nr,
				pfns, subsection_map_index(pfn),
				subsection_map_index(pfn + pfns - 1));

		pfn += pfns;
		nr_pages -= pfns;
	}
	/* 循环结束时剩余页数归零；每个跨越的 section 都已拥有与其部分范围对应的 subsection 位。 */
}

#ifdef CONFIG_MEMORY_HOTPLUG

/* Mark all memory sections within the pfn range as online */
void online_mem_sections(unsigned long start_pfn, unsigned long end_pfn)
{
	/* 热插拔完成 zone/页状态准备后批量置 ONLINE；输入按完整 section 对齐，位更新与调用者的更大热插拔锁配合。 */
	unsigned long pfn;

	for (pfn = start_pfn; pfn < end_pfn; pfn += PAGES_PER_SECTION) {
		/* 不检查 present/usage：上层只对已经建立 section 的完整范围调用，职责是发布 online 位。 */
		unsigned long section_nr = pfn_to_section_nr(pfn);
		struct mem_section *ms = __nr_to_section(section_nr);

		ms->section_mem_map |= SECTION_IS_ONLINE;
	}
}

/* Mark all memory sections within the pfn range as offline */
void offline_mem_sections(unsigned long start_pfn, unsigned long end_pfn)
{
	/* 与 online 配对清 ONLINE 位；实际 vmemmap/usage 拆除仍由 sparse_remove_section 的后续路径完成。 */
	unsigned long pfn;

	for (pfn = start_pfn; pfn < end_pfn; pfn += PAGES_PER_SECTION) {
		unsigned long section_nr = pfn_to_section_nr(pfn);
		struct mem_section *ms = __nr_to_section(section_nr);

		ms->section_mem_map &= ~SECTION_IS_ONLINE;
	}
}

static int __meminit section_nr_vmemmap_pages(unsigned long pfn, unsigned long nr_pages,
		struct vmem_altmap *altmap, struct dev_pagemap *pgmap)
{
	/* 计算这段 section 对全局 memmap_pages 计数的真实 backing 页数；复合优化的镜像 tail 不按虚拟页数计。 */
	const unsigned int order = pgmap ? pgmap->vmemmap_shift : 0;
	const unsigned long pages_per_compound = 1UL << order;

	VM_WARN_ON_ONCE(!IS_ALIGNED(pfn | nr_pages, PAGES_PER_SUBSECTION));
	VM_WARN_ON_ONCE(nr_pages > PAGES_PER_SECTION);

	if (!vmemmap_can_optimize(altmap, pgmap))
		/* 普通模式每个 struct page 都需要实际 backing，向上取整到基础页。 */
		return DIV_ROUND_UP(nr_pages * sizeof(struct page), PAGE_SIZE);

	if (order < PFN_SECTION_SHIFT) {
		/* 一个复合单元小于 section 时，每单元固定保留 VMEMMAP_RESERVE_NR 个真实 metadata 页。 */
		VM_WARN_ON_ONCE(!IS_ALIGNED(pfn | nr_pages, pages_per_compound));
		return VMEMMAP_RESERVE_NR * nr_pages / pages_per_compound;
	}

	VM_WARN_ON_ONCE(!IS_ALIGNED(pfn | nr_pages, PAGES_PER_SECTION));

	if (IS_ALIGNED(pfn, pages_per_compound))
		/* 复合单元不小于 section 时，只有单元首 section 承担真实 reserve，其余 section 全复用。 */
		return VMEMMAP_RESERVE_NR;

	return 0;
}

static struct page * __meminit populate_section_memmap(unsigned long pfn,
		unsigned long nr_pages, int nid, struct vmem_altmap *altmap,
		struct dev_pagemap *pgmap)
{
	/* 包装底层建映射并同步全局实际 backing 计数；计数更新即使 page 为 NULL 也遵循现有调用约定。 */
	struct page *page = __populate_section_memmap(pfn, nr_pages, nid, altmap,
						      pgmap);

	memmap_pages_add(section_nr_vmemmap_pages(pfn, nr_pages, altmap, pgmap));

	return page;
}

static void depopulate_section_memmap(unsigned long pfn, unsigned long nr_pages,
		struct vmem_altmap *altmap, struct dev_pagemap *pgmap)
{
	/* 热移除的逆路径：先扣除与该段相同的实际 backing 计数，再解除页表映射并按 altmap 规则归还页。 */
	unsigned long start = (unsigned long) pfn_to_page(pfn);
	unsigned long end = start + nr_pages * sizeof(struct page);

	memmap_pages_add(-section_nr_vmemmap_pages(pfn, nr_pages, altmap, pgmap));
	vmemmap_free(start, end, altmap);
}

static void free_map_bootmem(struct page *memmap)
{
	/* 启动期完整 section 的 memmap 由 bootmem/普通 vmemmap 建立；移除时以整 section 范围回收并扣启动计数。 */
	unsigned long start = (unsigned long)memmap;
	unsigned long end = (unsigned long)(memmap + PAGES_PER_SECTION);
	unsigned long pfn = page_to_pfn(memmap);

	memmap_boot_pages_add(-section_nr_vmemmap_pages(pfn, PAGES_PER_SECTION,
							NULL, NULL));
	vmemmap_free(start, end, NULL);
}

static int clear_subsection_map(unsigned long pfn, unsigned long nr_pages)
{
	/* 撤销前验证请求位全部已置，避免重复 remove 把未激活 subsection 静默清零；成功后异或清除精确范围。 */
	DECLARE_BITMAP(map, SUBSECTIONS_PER_SECTION) = { 0 };
	DECLARE_BITMAP(tmp, SUBSECTIONS_PER_SECTION) = { 0 };
	struct mem_section *ms = __pfn_to_section(pfn);
	unsigned long *subsection_map = ms->usage
		? &ms->usage->subsection_map[0] : NULL;

	subsection_mask_set(map, pfn, nr_pages);
	if (subsection_map)
		/* 临时交集用于确认 map 是现有 usage 的子集，不能直接用 bitmap_and 覆盖共享状态。 */
		bitmap_and(tmp, map, subsection_map, SUBSECTIONS_PER_SECTION);

	if (WARN(!subsection_map || !bitmap_equal(tmp, map, SUBSECTIONS_PER_SECTION),
				"section already deactivated (%#lx + %ld)\n",
				pfn, nr_pages))
		return -EINVAL;
	/* 所有目标位确认存在后才修改 usage，失败保持 bitmap 和 section 生命周期不变。 */

	bitmap_xor(subsection_map, map, subsection_map, SUBSECTIONS_PER_SECTION);
	return 0;
}

static bool is_subsection_map_empty(struct mem_section *ms)
{
	/* 空位图意味着本 section 已无 present memory，可释放 usage 或把 section_mem_map 标为无效。 */
	return bitmap_empty(&ms->usage->subsection_map[0],
			    SUBSECTIONS_PER_SECTION);
}

static int fill_subsection_map(unsigned long pfn, unsigned long nr_pages)
{
	/* 激活时构造精确 subsection 掩码；空掩码和与现存位重叠分别表示无效范围与重复热添加。 */
	struct mem_section *ms = __pfn_to_section(pfn);
	DECLARE_BITMAP(map, SUBSECTIONS_PER_SECTION) = { 0 };
	unsigned long *subsection_map;
	int rc = 0;

	subsection_mask_set(map, pfn, nr_pages);

	subsection_map = &ms->usage->subsection_map[0];

	if (bitmap_empty(map, SUBSECTIONS_PER_SECTION))
		/* 这通常表示跨 section 调用者传入了无法映射到当前 section 的零长度范围。 */
		rc = -EINVAL;
	else if (bitmap_intersects(map, subsection_map, SUBSECTIONS_PER_SECTION))
		/* 重叠不能覆盖合并，否则失败调用者会失去“已存在”的可诊断错误。 */
		rc = -EEXIST;
	else
		bitmap_or(subsection_map, map, subsection_map,
				SUBSECTIONS_PER_SECTION);

	return rc;
}

/*
 * To deactivate a memory region, there are 3 cases to handle:
 *
 * 1. deactivation of a partial hot-added section:
 *      a) section was present at memory init.
 *      b) section was hot-added post memory init.
 * 2. deactivation of a complete hot-added section.
 * 3. deactivation of a complete section from memory init.
 *
 * For 1, when subsection_map does not empty we will not be freeing the
 * usage map, but still need to free the vmemmap range.
 */
/* 撤销按 subsection 是否清空、section 是否早期建立区分 usage 的保留/RCU 释放以及 vmemmap 的精确或整段回收。 */
static void section_deactivate(unsigned long pfn, unsigned long nr_pages,
		struct vmem_altmap *altmap, struct dev_pagemap *pgmap)
{
	/* 热移除核心：先撤 subsection 可见性，再按 early/动态来源解除 vmemmap，最后在空 section 时撤销 usage 与 section 描述。 */
	struct mem_section *ms = __pfn_to_section(pfn);
	bool section_is_early = early_section(ms);
	struct page *memmap = NULL;
	bool empty;

	if (clear_subsection_map(pfn, nr_pages))
		/* 位图不匹配时不继续释放 backing，防止重复请求破坏仍在线 subsection 的 metadata。 */
		return;

	empty = is_subsection_map_empty(ms);
	/* 仅最后一个 subsection 离开时才可使 valid_section 变假及释放共享 usage 页。 */
	if (empty) {
		/*
		 * Mark the section invalid so that valid_section()
		 * return false. This prevents code from dereferencing
		 * ms->usage array.
		 */
		/* 先禁止 valid_section 查询再处理 usage 生命周期，避免新读者在本 section 已空时取得即将释放的指针。 */
		ms->section_mem_map &= ~SECTION_HAS_MEM_MAP;
		/* 先撤 HAS_MEM_MAP 阻止并发查询继续解引用 usage；真正 usage 释放可经 RCU 延后。 */

		/*
		 * When removing an early section, the usage map is kept (as the
		 * usage maps of other sections fall into the same page). It
		 * will be re-used when re-adding the section - which is then no
		 * longer an early section. If the usage map is PageReserved, it
		 * was allocated during boot.
		 */
		if (!PageReserved(virt_to_page(ms->usage))) {
			/* 启动期 usage 可能与其它 section 共页且 PageReserved，不能在单 section 移除时释放。 */
			kfree_rcu(ms->usage, rcu);
			WRITE_ONCE(ms->usage, NULL);
		}
		memmap = pfn_to_page(SECTION_ALIGN_DOWN(pfn));
		/* early section 的 backing 固定覆盖整 section，需从 section 起点而非本次 subsection 起点回收。 */
	}

	/*
	 * The memmap of early sections is always fully populated. See
	 * section_activate() and pfn_valid() .
	 */
	/* early backing 从启动起覆盖完整 section，即使本次只删 subsection 也不能对中间范围做动态拆映射。 */
	if (!section_is_early)
		/* 动态 section 精确拆本次范围；它可能只是一个 section 的若干 subsection。 */
		depopulate_section_memmap(pfn, nr_pages, altmap, pgmap);
	else if (memmap)
		/* early section 只在所有 subsection 都空时按 bootmem 整段释放。 */
		free_map_bootmem(memmap);

	if (empty)
		/* 最终清零移除 nid/present 等残留编码，后续 sparse_add_section 可重新初始化该 slot。 */
		ms->section_mem_map = (unsigned long)NULL;
}

static struct page * __meminit section_activate(int nid, unsigned long pfn,
		unsigned long nr_pages, struct vmem_altmap *altmap,
		struct dev_pagemap *pgmap)
{
	/* 热添加镜像撤销路径：先确保 usage 存在并置 subsection，再建立 vmemmap；后段失败调用 deactivate 回滚前段状态。 */
	struct mem_section *ms = __pfn_to_section(pfn);
	struct mem_section_usage *usage = NULL;
	struct page *memmap;
	int rc;

	if (!ms->usage) {
		/* usage 是 per-section bitmap/RCU 状态，先分配但暂不对外标记 present。 */
		usage = kzalloc(mem_section_usage_size(), GFP_KERNEL);
		if (!usage)
			return ERR_PTR(-ENOMEM);
		ms->usage = usage;
		/* 此赋值在 fill 前仅供本调用使用；fill 失败时立即置回 NULL 并释放新页。 */
	}

	rc = fill_subsection_map(pfn, nr_pages);
	/* 位图先提交使 pfn_valid 查询与随后 memmap 映射在成功路径保持同一激活范围。 */
	if (rc) {
		if (usage)
			ms->usage = NULL;
		kfree(usage);
		return ERR_PTR(rc);
	}

	/*
	 * The early init code does not consider partially populated
	 * initial sections, it simply assumes that memory will never be
	 * referenced.  If we hot-add memory into such a section then we
	 * do not need to populate the memmap and can simply reuse what
	 * is already there.
	 */
	/* 初始 section 的 backing 永久覆盖全段，因此 partial hot-add 只补 subsection bitmap，复用现有 struct page 数组。 */
	if (nr_pages < PAGES_PER_SECTION && early_section(ms))
		/* 启动期已有完整 memmap 的 section 热补 subsection 只更新位图，不得重复分配或计数 backing。 */
		return pfn_to_page(pfn);

	memmap = populate_section_memmap(pfn, nr_pages, nid, altmap, pgmap);
	/* 动态/整段 early 情况在此真正取得映射首页；NULL 后由 deactivate 还原 bitmap、usage 与计数。 */
	if (!memmap) {
		section_deactivate(pfn, nr_pages, altmap, pgmap);
		return ERR_PTR(-ENOMEM);
	}

	return memmap;
}

/**
 * sparse_add_section - add a memory section, or populate an existing one
 * @nid: The node to add section on
 * @start_pfn: start pfn of the memory range
 * @nr_pages: number of pfns to add in the section
 * @altmap: alternate pfns to allocate the memmap backing store
 * @pgmap: alternate compound page geometry for devmap mappings
 *
 * This is only intended for hotplug.
 *
 * Note that only VMEMMAP supports sub-section aligned hotplug,
 * the proper alignment and size are gated by check_pfn_span().
 *
 *
 * Return:
 * * 0		- On success.
 * * -EEXIST	- Section has been present.
 * * -ENOMEM	- Out of memory.
 */
/* 此接口只服务热插拔；调用者已通过 check_pfn_span 保证 subsection 对齐，成功后 section 才对 PFN 查询正式 present。 */
int __meminit sparse_add_section(int nid, unsigned long start_pfn,
		unsigned long nr_pages, struct vmem_altmap *altmap,
		struct dev_pagemap *pgmap)
{
	/* hotplug 公开入口：索引先就绪，激活 vmemmap 后才 poison/标 present/初始化 section，任一早退不得发布半成品。 */
	unsigned long section_nr = pfn_to_section_nr(start_pfn);
	struct mem_section *ms;
	struct page *memmap;
	int ret;

	ret = sparse_index_init(section_nr, nid);
	/* 索引分配失败时尚未接触 usage 或页表；错误可直接上返热插拔编排器。 */
	if (ret < 0)
		return ret;

	memmap = section_activate(nid, start_pfn, nr_pages, altmap, pgmap);
	/* section_activate 已把内存不足回滚；ERR_PTR 保留 -EEXIST 等可区分失败原因。 */
	if (IS_ERR(memmap))
		return PTR_ERR(memmap);

	/*
	 * Poison uninitialized struct pages in order to catch invalid flags
	 * combinations.
	 */
	page_init_poison(memmap, sizeof(struct page) * nr_pages);
	/* 映射可访问但尚未 publish 时毒化未初始化 struct page，帮助后续初始化发现非法 flags 组合。 */

	ms = __nr_to_section(section_nr);
	__section_mark_present(ms, section_nr);
	/* PRESENT 发布在 memmap/usage 就绪之后，读者由此开始允许把 PFN 转换为 struct page。 */

	/* Align memmap to section boundary in the subsection case */
	if (section_nr_to_pfn(section_nr) != start_pfn)
		/* subsection 添加仍需以 section 基址交给 sparse_init_one_section，后者维护 section-level mem_map 编码。 */
		memmap = pfn_to_page(section_nr_to_pfn(section_nr));
	sparse_init_one_section(ms, section_nr, memmap, ms->usage, 0);
	/* 最终初始化写入 section map 与 usage 指针；返回成功后热插拔上层才会继续 online 页和 zone。 */

	return 0;
}

void sparse_remove_section(unsigned long pfn, unsigned long nr_pages,
		struct vmem_altmap *altmap, struct dev_pagemap *pgmap)
{
	/* hotplug 删除入口只接受已 valid 的 section；实际 online 位清除由 memory_hotplug 的更高层时序负责。 */
	struct mem_section *ms = __pfn_to_section(pfn);

	if (WARN_ON_ONCE(!valid_section(ms)))
		/* 防御重复/乱序移除；拒绝后保留现有 usage、vmemmap 和计数。 */
		return;

	section_deactivate(pfn, nr_pages, altmap, pgmap);
	/* deactivate 依据 early 与 subsection 空状态选择精确拆映射或整段 bootmem 回收。 */
}
#endif /* CONFIG_MEMORY_HOTPLUG */
