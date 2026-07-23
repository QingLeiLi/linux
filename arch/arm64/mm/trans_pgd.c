// SPDX-License-Identifier: GPL-2.0
/*
 * arm64 kexec/hibernate 过渡页表构造学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 切换到新内核或恢复镜像时，当前内核页表本身可能即将被覆盖，不能继续
 * 依赖 swapper_pg_dir。调用者提供一个不会被覆盖的页分配器，本文件复制
 * 所需内核映射、为单个过渡代码页构造 TTBR0 identity map，并复制最小
 * EL2 vector stub。所有新页由调用者的 allocator 拥有；本文件不释放，
 * 任一 -ENOMEM 后调用者按其分配池整体回收。
 *
 * 复制时把 leaf 强制 valid+writable，是为了过渡阶段能覆盖/恢复内存；
 * 这张表生命周期极短且不作为正常内核安全边界。建立完成后，调用者负责
 * cache clean、TTBR/TCR 切换及 TLB 同步。
 */

/*
 * Transitional page tables for kexec and hibernate
 *
 * This file derived from: arch/arm64/kernel/hibernate.c
 *
 * Copyright (c) 2021, Microsoft Corporation.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 */

/*
 * Transitional tables are used during system transferring from one world to
 * another: such as during hibernate restore, and kexec reboots. During these
 * phases one cannot rely on page table not being overwritten. This is because
 * hibernate and kexec can overwrite the current page tables during transition.
 */
/* 过渡表必须放在目标覆盖范围之外，否则切换尚未完成页表就可能自毁。 */

#include <asm/trans_pgd.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <linux/suspend.h>
#include <linux/bug.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/kfence.h>

/*
 * 从 info 描述的调用者 allocator 取得一个清零页。trans_alloc_arg 原样
 * 传给回调；返回页虚拟地址或 NULL。所有权仍由 allocator/pool 管理，
 * 本文件只在构造期写入，失败不做逐页 free。
 */
static void *trans_alloc(struct trans_pgd_info *info)
{
	return info->trans_alloc_page(info->trans_alloc_arg);
}

/*
 * 复制 [start,end) 覆盖的 PTE leaf 到新 PMD 下。范围必须位于同一 PMD，
 * dst_pmdp 尚无 PTE table；成功分配并 populate 后逐页跳过 none 项，把
 * 现有 PTE 转为 valid+writable。返回 0/-ENOMEM，分配失败前不发布子表，
 * 复制中无其他可恢复错误。
 */
static int copy_pte(struct trans_pgd_info *info, pmd_t *dst_pmdp,
		    pmd_t *src_pmdp, unsigned long start, unsigned long end)
{
	pte_t *src_ptep;
	pte_t *dst_ptep;
	unsigned long addr = start;

	dst_ptep = trans_alloc(info);
	/* allocator 必须返回页表所需对齐且已清零页，否则未填槽会含垃圾映射。 */
	if (!dst_ptep)
		return -ENOMEM;
	pmd_populate_kernel(NULL, dst_pmdp, dst_ptep);
	dst_ptep = pte_offset_kernel(dst_pmdp, start);

	src_ptep = pte_offset_kernel(src_pmdp, start);
	do {
		pte_t pte = __ptep_get(src_ptep);

		if (pte_none(pte))
			/* 源洞保持目的洞；循环指针仍由 while 尾表达式推进。 */
			continue;
		__set_pte(dst_ptep, pte_mkvalid_k(pte_mkwrite_novma(pte)));
	} while (dst_ptep++, src_ptep++, addr += PAGE_SIZE, addr != end);

	return 0;
}

/*
 * 复制一个 PUD 范围内的 PMD。目的 PUD 首次使用时延迟分配 PMD table；
 * 对 table PMD 递归 copy_pte，对 block leaf 直接复制并放宽 valid/write。
 * next 由 pmd_addr_end 截到层级边界，返回 0 或任何子分配 -ENOMEM。
 */
static int copy_pmd(struct trans_pgd_info *info, pud_t *dst_pudp,
		    pud_t *src_pudp, unsigned long start, unsigned long end)
{
	pmd_t *src_pmdp;
	pmd_t *dst_pmdp;
	unsigned long next;
	unsigned long addr = start;

	if (pud_none(READ_ONCE(*dst_pudp))) {
		/* 同一目的 PUD 可能由相邻区间复用，只有 none 时创建下一级表。 */
		dst_pmdp = trans_alloc(info);
		if (!dst_pmdp)
			return -ENOMEM;
		pud_populate(NULL, dst_pudp, dst_pmdp);
	}
	dst_pmdp = pmd_offset(dst_pudp, start);

	src_pmdp = pmd_offset(src_pudp, start);
	do {
		pmd_t pmd = READ_ONCE(*src_pmdp);

		next = pmd_addr_end(addr, end);
		if (pmd_none(pmd))
			continue;
		if (pmd_table(pmd)) {
			if (copy_pte(info, dst_pmdp, src_pmdp, addr, next))
				return -ENOMEM;
		} else {
			set_pmd(dst_pmdp, pmd_mkvalid_k(pmd_mkwrite_novma(pmd)));
		}
	} while (dst_pmdp++, src_pmdp++, addr = next, addr != end);

	return 0;
}

/* PUD 层递归复制，参数/所有权同 copy_pmd；block PUD 可直接形成过渡 leaf。 */
static int copy_pud(struct trans_pgd_info *info, p4d_t *dst_p4dp,
		    p4d_t *src_p4dp, unsigned long start,
		    unsigned long end)
{
	pud_t *dst_pudp;
	pud_t *src_pudp;
	unsigned long next;
	unsigned long addr = start;

	if (p4d_none(READ_ONCE(*dst_p4dp))) {
		dst_pudp = trans_alloc(info);
		if (!dst_pudp)
			return -ENOMEM;
		p4d_populate(NULL, dst_p4dp, dst_pudp);
	}
	dst_pudp = pud_offset(dst_p4dp, start);

	src_pudp = pud_offset(src_p4dp, start);
	do {
		pud_t pud = READ_ONCE(*src_pudp);

		next = pud_addr_end(addr, end);
		if (pud_none(pud))
			continue;
		if (pud_table(pud)) {
			if (copy_pmd(info, dst_pudp, src_pudp, addr, next))
				return -ENOMEM;
		} else {
			set_pud(dst_pudp, pud_mkvalid_k(pud_mkwrite_novma(pud)));
		}
	} while (dst_pudp++, src_pudp++, addr = next, addr != end);

	return 0;
}

/* P4D 层复制。折叠页表配置下通用 helper 仍保持同一接口，无需条件分支。 */
static int copy_p4d(struct trans_pgd_info *info, pgd_t *dst_pgdp,
		    pgd_t *src_pgdp, unsigned long start,
		    unsigned long end)
{
	p4d_t *dst_p4dp;
	p4d_t *src_p4dp;
	unsigned long next;
	unsigned long addr = start;

	if (pgd_none(READ_ONCE(*dst_pgdp))) {
		dst_p4dp = trans_alloc(info);
		if (!dst_p4dp)
			return -ENOMEM;
		pgd_populate(NULL, dst_pgdp, dst_p4dp);
	}

	dst_p4dp = p4d_offset(dst_pgdp, start);
	src_p4dp = p4d_offset(src_pgdp, start);
	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none(READ_ONCE(*src_p4dp)))
			continue;
		if (copy_pud(info, dst_p4dp, src_p4dp, addr, next))
			return -ENOMEM;
	} while (dst_p4dp++, src_p4dp++, addr = next, addr != end);

	return 0;
}

/*
 * 从内核 swapper 页表复制任意半开区间 [start,end) 到目的 PGD。start/end
 * 为内核虚拟字节地址且应页对齐；src_pgdp 由 pgd_offset_k 定位，目的用
 * pgd_offset_pgd 定位相同 VA。逐 PGD 递归，源 none 区间不建立目的表。
 */
static int copy_page_tables(struct trans_pgd_info *info, pgd_t *dst_pgdp,
			    unsigned long start, unsigned long end)
{
	unsigned long next;
	unsigned long addr = start;
	pgd_t *src_pgdp = pgd_offset_k(start);

	dst_pgdp = pgd_offset_pgd(dst_pgdp, start);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none(READ_ONCE(*src_pgdp)))
			continue;
		if (copy_p4d(info, dst_pgdp, src_pgdp, addr, next))
			return -ENOMEM;
	} while (dst_pgdp++, src_pgdp++, addr = next, addr != end);

	return 0;
}

/*
 * Create trans_pgd and copy linear map.
 * info:	contains allocator and its argument
 * dst_pgdp:	new page table that is created, and to which map is copied.
 * start:	Start of the interval (inclusive).
 * end:		End of the interval (exclusive).
 *
 * Returns 0 on success, and -ENOMEM on failure.
 */
/*
 * 创建顶级过渡 PGD 并复制指定线性映射区间。info 必须含可重复调用的页
 * allocator；dst_pgdp 是成功时写出的新 PGD，不可为 NULL；start 包含、
 * end 不包含。只有完整复制成功才发布 *dst_pgdp，失败保持出参原值，
 * 已分配页由调用者的池统一回收。
 */
int trans_pgd_create_copy(struct trans_pgd_info *info, pgd_t **dst_pgdp,
			  unsigned long start, unsigned long end)
{
	int rc;
	pgd_t *trans_pgd = trans_alloc(info);

	if (!trans_pgd) {
		pr_err("Failed to allocate memory for temporary page tables.\n");
		return -ENOMEM;
	}

	rc = copy_page_tables(info, trans_pgd, start, end);
	/* 发布出参是提交点，避免调用者误用部分构造的页表。 */
	if (!rc)
		*dst_pgdp = trans_pgd;

	return rc;
}

/*
 * The page we want to idmap may be outside the range covered by VA_BITS that
 * can be built using the kernel's p?d_populate() helpers. As a one off, for a
 * single page, we build these page tables bottom up and just assume that will
 * need the maximum T0SZ.
 *
 * Returns 0 on success, and -ENOMEM on failure.
 * On success trans_ttbr0 contains page table with idmapped page, t0sz is set to
 * maximum T0SZ for this page.
 */
/*
 * 为单个过渡代码 page 自底向上构造 TTBR0 identity map。
 * page 是直接映射虚址，函数将其物理地址同时作为 VA/PA；trans_ttbr0 与
 * t0sz 为成功出参。由于物理页可能超出内核 VA_BITS，不能用常规 populate
 * 自顶向下，故根据最高物理位选择 48/52-bit 范围并逐层链接。成功返回 0，
 * 分配失败返回 -ENOMEM 且出参不发布；已分配页仍归调用者池。
 */
int trans_pgd_idmap_page(struct trans_pgd_info *info, phys_addr_t *trans_ttbr0,
			 unsigned long *t0sz, void *page)
{
	phys_addr_t dst_addr = virt_to_phys(page);
	/* dst_addr 最终按页对齐；pfn 形成最底层 ROX leaf，代码页不可写。 */
	unsigned long pfn = __phys_to_pfn(dst_addr);
	int max_msb = (dst_addr & GENMASK(52, 48)) ? 51 : 47;
	int bits_mapped = PAGE_SHIFT - 4;
	/* 每级表有 PAGE_SIZE/8 项，即每级索引贡献 PAGE_SHIFT-3 位；此处掩码边界按实现编码。 */
	unsigned long level_mask, prev_level_entry, *levels[4];
	int this_level, index, level_lsb, level_msb;

	dst_addr &= PAGE_MASK;
	prev_level_entry = pte_val(pfn_pte(pfn, PAGE_KERNEL_ROX));

	for (this_level = 3; this_level >= 0; this_level--) {
		/* levels[3] 先放 PTE leaf，随后每轮新页成为更高一级 table。 */
		levels[this_level] = trans_alloc(info);
		if (!levels[this_level])
			return -ENOMEM;

		level_lsb = ARM64_HW_PGTABLE_LEVEL_SHIFT(this_level);
		level_msb = min(level_lsb + bits_mapped, max_msb);
		level_mask = GENMASK_ULL(level_msb, level_lsb);

		index = (dst_addr & level_mask) >> level_lsb;
		*(levels[this_level] + index) = prev_level_entry;
		/* 每张表只填 identity address 对应一个槽，其余槽依赖 allocator 清零。 */

		pfn = virt_to_pfn(levels[this_level]);
		prev_level_entry = pte_val(pfn_pte(pfn,
						   __pgprot(PMD_TYPE_TABLE)));

		if (level_msb == max_msb)
			/* 已覆盖物理地址最高有效位，当前页就是最终 TTBR0 根。 */
			break;
	}

	*trans_ttbr0 = phys_to_ttbr(__pfn_to_phys(pfn));
	/* TTBR 保存根表物理地址；T0SZ=64-覆盖 VA 位数，由宏编码。 */
	*t0sz = TCR_T0SZ(max_msb + 1);

	return 0;
}

/*
 * Create a copy of the vector table so we can call HVC_SET_VECTORS or
 * HVC_SOFT_RESTART from contexts where the table may be overwritten.
 */
/*
 * 复制 2 KiB EL2 stub vector 到安全页。info 提供分配器，el2_vectors 是
 * 成功物理地址出参。memcpy 后同时 clean/invalidate 到 PoU（保证取指）和
 * PoC（保证 EL2/可能不同缓存观察点可见）；成功返回 0，分配失败 -ENOMEM。
 * stub 页由调用者池拥有，使用期必须保持不可覆盖。
 */
int trans_pgd_copy_el2_vectors(struct trans_pgd_info *info,
			       phys_addr_t *el2_vectors)
{
	void *hyp_stub = trans_alloc(info);

	if (!hyp_stub)
		return -ENOMEM;
	*el2_vectors = virt_to_phys(hyp_stub);
	/* 先发布物理地址变量但调用者只在返回 0 后使用，cache 同步是提交前置。 */
	memcpy(hyp_stub, &trans_pgd_stub_vectors, ARM64_VECTOR_TABLE_LEN);
	caches_clean_inval_pou((unsigned long)hyp_stub,
			       (unsigned long)hyp_stub +
			       ARM64_VECTOR_TABLE_LEN);
	dcache_clean_inval_poc((unsigned long)hyp_stub,
			       (unsigned long)hyp_stub +
			       ARM64_VECTOR_TABLE_LEN);

	return 0;
}
