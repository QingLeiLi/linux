// SPDX-License-Identifier: GPL-2.0
/*
 *  mm/mprotect.c
 *
 *  (C) Copyright 1994 Linus Torvalds
 *  (C) Copyright 2002 Christoph Hellwig
 *
 *  Address space accounting code	<alan@lxorguk.ukuu.org.uk>
 *  (C) Copyright 2002 Red Hat Inc, All Rights Reserved
 */

/* 页表遍历、VMA 操作与异常映射的公共契约：本文件负责把用户权限落到这些层。 */
/* 这些头提供 VMA、页表、TLB、LSM 与 userfaultfd 的跨子系统接口。 */
#include <linux/pagewalk.h>
#include <linux/hugetlb.h>
#include <linux/shm.h>
#include <linux/mman.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/security.h>
#include <linux/mempolicy.h>
#include <linux/personality.h>
/* 下列接口补齐系统调用、交换、通知、性能统计和保护键各自的收尾责任。 */
#include <linux/syscalls.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mmu_notifier.h>
/* 交换、迁移、KSM 与性能统计让保护改变在非普通页和观察工具中保持一致。 */
#include <linux/migrate.h>
#include <linux/perf_event.h>
#include <linux/pkeys.h>
#include <linux/ksm.h>
#include <linux/uaccess.h>
#include <linux/mm_inline.h>
#include <linux/pgtable.h>
#include <linux/userfaultfd_k.h>
/* UAPI 定义用户可见 PROT_*；asm 头定义本架构实际的页表和 TLB 落地方式。 */
/* 用户 ABI 的 PROT 值须与本架构的缓存/TLB 操作配合，不能只改软件标志。 */
#include <uapi/linux/mman.h>
#include <asm/cacheflush.h>
/* 架构层定义修改页表后如何让本 CPU 和远端 CPU 丢弃陈旧翻译。 */
#include <asm/mmu_context.h>
#include <asm/tlbflush.h>
#include <asm/tlb.h>

#include "internal.h"

/*
 * maybe_change_pte_writable() - 判断保护转换能否直接把一个 PTE 设为可写。
 *
 * 业务背景：mprotect(PROT_WRITE) 改的是 VMA 的“允许写”规则；已经存在的
 * PTE 是否能跳过下一次写缺页而直接可写，还取决于 COW、soft-dirty 和
 * userfaultfd 的观察契约。
 * 入参：vma 给出映射类型和权限，pte 是正被页表锁保护的旧表项。
 * 返回：true 表示下面的快速路径可以考虑设写位；false 必须保留只读，
 * 让真正的写访问走缺页处理。
 * 注意事项：不能因为 VMA 有 VM_WRITE 就一律设写。那会绕过 soft-dirty
 * 记录或 userfaultfd 写保护，也会把 PROT_NONE 重新变成可读写。
 */
static bool maybe_change_pte_writable(struct vm_area_struct *vma, pte_t pte)
{
	/* VM_WRITE 是最基本前提；违反它说明调用路径的 VMA/PTE 语义已经不一致。 */
	if (WARN_ON_ONCE(!(vma->vm_flags & VM_WRITE)))
		return false;

	/* Don't touch entries that are not even readable. */
	if (pte_protnone(pte))
		return false;

	/* Do we need write faults for softdirty tracking? */
	if (pte_needs_soft_dirty_wp(vma, pte))
		return false;

	/* Do we need write faults for uffd-wp tracking? */
	if (userfaultfd_pte_wp(vma, pte))
		return false;

	return true;
}

/*
 * can_change_private_pte_writable() - 为 MAP_PRIVATE 保留 COW 的写保护边界。
 *
 * 业务背景：私有映射的首次写可能要复制共享页；只有已确认“匿名且独占”的
 * 页，缺页处理本来也会无需额外检查地映射为可写。
 * 入参：addr 用于把 PTE 解析为普通 page，vma/pte 必须仍对应同一页表项。
 * 返回：true 仅代表该页可安全走直接可写优化。
 * 注意事项：若把文件页、共享匿名页也放行，进程会直接改到本应 COW 的内容，
 * 从而破坏进程隔离；宁可返回 false 产生一次写缺页。
 */
static bool can_change_private_pte_writable(struct vm_area_struct *vma,
					    unsigned long addr, pte_t pte)
{
	struct page *page;

	if (!maybe_change_pte_writable(vma, pte))
		return false;

	/*
	 * Writable MAP_PRIVATE mapping: We can only special-case on
	 * exclusive anonymous pages, because we know that our
	 * write-fault handler similarly would map them writable without
	 * any additional checks while holding the PT lock.
	 */
	page = vm_normal_page(vma, addr, pte);
	return page && PageAnon(page) && PageAnonExclusive(page);
}

/*
 * can_change_shared_pte_writable() - 为 MAP_SHARED 判断能否省去写通知缺页。
 *
 * 业务背景：共享文件页第一次写有时必须触发 writenotify，让文件系统建立
 * 脏页/写回所需状态；脏 PTE 表示该通知已完成。
 * 入参：vma 描述共享映射，pte 为当前旧项。
 * 返回：true 时可直接设写，否则保留只读等待真实写访问。
 * 注意事项：忽略 dirty 判断会让文件系统错过写通知，后续脏页追踪或写回
 * 可能不完整；零页带 dirty 位也应当被视为异常状态而非正常捷径。
 */
static bool can_change_shared_pte_writable(struct vm_area_struct *vma,
					   pte_t pte)
{
	if (!maybe_change_pte_writable(vma, pte))
		return false;

	VM_WARN_ON_ONCE(is_zero_pfn(pte_pfn(pte)) && pte_dirty(pte));

	/*
	 * Writable MAP_SHARED mapping: "clean" might indicate that the FS still
	 * needs a real write-fault for writenotify
	 * (see vma_wants_writenotify()). If "dirty", the assumption is that the
	 * FS was already notified and we can simply mark the PTE writable
	 * just like the write-fault handler would do.
	 */
	return pte_dirty(pte);
}

/*
 * can_change_pte_writable() - 按共享/私有语义选择直接设写的判据。
 *
 * 业务背景：同一个 PROT_WRITE 对 MAP_PRIVATE 是 COW 问题，对 MAP_SHARED
 * 是文件写通知问题，因此不能共用一套“看见可写就设位”的规则。
 * 入参：vma、addr、pte 是当前页表项的上下文。
 * 返回：是否允许调用者将 PTE 写位打开。
 * 注意事项：这里只给优化作决定，不改变 PTE；提交 PTE 和刷新 TLB 仍由
 * 调用者统一完成，否则 CPU 可能继续使用旧的权限翻译。
 */
bool can_change_pte_writable(struct vm_area_struct *vma, unsigned long addr,
			     pte_t pte)
{
	if (!(vma->vm_flags & VM_SHARED))
		return can_change_private_pte_writable(vma, addr, pte);

	return can_change_shared_pte_writable(vma, pte);
}

/*
 * mprotect_folio_pte_batch() - 找出可一起修改保护位的一段连续 PTE。
 *
 * 业务背景：大 folio 常由一串相邻 PTE 映射；逐项锁后提交会增加页表和 TLB
 * 开销，folio 层已经知道哪些连续项可以安全合批。
 * 入参：folio/page-table 起点、旧 pte、最大边界和必须保持的 PTE 属性。
 * 返回：本轮可处理的 PTE 数；没有或不是大 folio 时固定为 1。
 * 注意事项：批次绝不能越过本轮 range 或混入属性不同的项，否则一次保护更新
 * 会错误覆盖相邻页；flags 让 folio helper 在 soft-dirty/写位边界停下。
 */
static int mprotect_folio_pte_batch(struct folio *folio, pte_t *ptep,
				    pte_t pte, int max_nr_ptes, fpb_t flags)
{
	/* No underlying folio, so cannot batch */
	if (!folio)
		return 1;

	if (!folio_test_large(folio))
		return 1;

	return folio_pte_batch_flags(folio, NULL, ptep, &pte, max_nr_ptes, flags);
}

/* Set nr_ptes number of ptes, starting from idx */
/*
 * prot_commit_flush_ptes() - 提交一批新保护 PTE，并在需要时失效旧 TLB。
 *
 * 业务背景：页表内存写完不表示各 CPU 已看见新权限；TLB 仍可能缓存旧翻译。
 * 入参：idx/nr_ptes 指出批内子区间，oldpte/ptent 是它的旧/新模板，tlb 记录
 * 可延迟合并的刷新工作。
 * 出参：无；PTE 被提交，且必要刷新被登记到 tlb。
 * 注意事项：先按 idx 同步地址、PTE 指针和 PFN 模板；少推进任何一个都会把
 * 别页的物理页号或权限写错。漏掉刷新会让用户态短暂继续按旧权限访问。
 */
static __always_inline void prot_commit_flush_ptes(struct vm_area_struct *vma,
		unsigned long addr, pte_t *ptep, pte_t oldpte, pte_t ptent,
		int nr_ptes, int idx, bool set_write, struct mmu_gather *tlb)
{
	/*
	 * Advance the position in the batch by idx; note that if idx > 0,
	 * then the nr_ptes passed here is <= batch size - idx.
	 */
	addr += idx * PAGE_SIZE;
	ptep += idx;
	oldpte = pte_advance_pfn(oldpte, idx);
	ptent = pte_advance_pfn(ptent, idx);

	if (set_write)
		/* 写位只能在前述 COW/写通知判定后补上，不能由基础 pgprot 直接决定。 */
		ptent = pte_mkwrite(ptent, vma);

	modify_prot_commit_ptes(vma, addr, ptep, oldpte, ptent, nr_ptes);
	if (pte_needs_flush(oldpte, ptent))
		tlb_flush_pte_range(tlb, addr, nr_ptes * PAGE_SIZE);
}

/*
 * page_anon_exclusive_sub_batch() - 在匿名大 folio 批中找同一独占状态的子段。
 *
 * 业务背景：folio 连续不等于每个 page 都被本进程独占；COW 资格是 page 级别的。
 * 入参：first_page 是同一 folio 的起页，start_idx/max_len 限定可扫描范围。
 * 返回：从 start_idx 起可用同一设写决定处理的连续 PTE 数。
 * 注意事项：调用者必须先保证 PTE 对应连续的同一匿名 folio。若不逐页切段，
 * 批量设写会把共享子页误当独占页，破坏私有映射隔离。
 */
static __always_inline int page_anon_exclusive_sub_batch(int start_idx, int max_len,
		struct page *first_page, bool expected_anon_exclusive)
{
	int idx;

	for (idx = start_idx + 1; idx < start_idx + max_len; ++idx) {
		/* 状态变化处立即截断，后续提交器才能给下一段单独选择写位。 */
		if (expected_anon_exclusive != PageAnonExclusive(first_page + idx))
			break;
	}
	return idx - start_idx;
}

/*
 * commit_anon_folio_batch() - 依每页独占性分段提交匿名 folio 的 PTE。
 *
 * 业务背景：这里保留“可直接写”优化，但把优化限制在确实独占的 page 子段。
 * 入参：first_page、ptep 与 addr 对齐批首；nr_ptes 是同一 folio 的连续项。
 * 出参：无；每段经统一提交器写入，并登记相应 TLB 刷新。
 * 注意事项：循环每次消费 len，不能只用首页状态覆盖整个 folio；否则一处共享
 * 子页会变可写。下面保留的原注释解释了为何此处不能只检查第一项。
 *
 * This function is a result of trying our very best to retain the
 * "avoid the write-fault handler" optimization. In can_change_pte_writable(),
 * if the vma is a private vma, and we cannot determine whether to change
 * the pte to writable just from the vma and the pte, we then need to look
 * at the actual page pointed to by the pte. Unfortunately, if we have a
 * batch of ptes pointing to consecutive pages of the same anon large folio,
 * the anon-exclusivity (or the negation) of the first page does not guarantee
 * the anon-exclusivity (or the negation) of the other pages corresponding to
 * the pte batch; hence in this case it is incorrect to decide to change or
 * not change the ptes to writable just by using information from the first
 * pte of the batch. Therefore, we must individually check all pages and
 * retrieve sub-batches.
 */
static __always_inline void commit_anon_folio_batch(struct vm_area_struct *vma,
		struct folio *folio, struct page *first_page, unsigned long addr, pte_t *ptep,
		pte_t oldpte, pte_t ptent, int nr_ptes, struct mmu_gather *tlb)
{
	bool expected_anon_exclusive;
	int sub_batch_idx = 0;
	int len;

	while (nr_ptes) {
		/* 本段的首 page 决定本段策略，扫描 helper 保证不会跨过状态变化点。 */
		expected_anon_exclusive = PageAnonExclusive(first_page + sub_batch_idx);
		len = page_anon_exclusive_sub_batch(sub_batch_idx, nr_ptes,
					first_page, expected_anon_exclusive);
		prot_commit_flush_ptes(vma, addr, ptep, oldpte, ptent, len,
				       sub_batch_idx, expected_anon_exclusive, tlb);
		sub_batch_idx += len;
		nr_ptes -= len;
	}
}

/*
 * set_write_prot_commit_flush_ptes() - 在提交前按映射语义决定哪些项真设写。
 *
 * 业务背景：合批的 PTE 可以共享保护模板，却不一定都可写；匿名大 folio 内的
 * 每个子页独占状态也可能不同。
 * 入参：folio/page 指向批次底页，oldpte/ptent 为保护转换前后模板。
 * 出参：无；通过下层 helper 提交并登记 TLB 刷新。
 * 注意事项：共享映射按写通知判定；私有匿名 folio 则再拆独占状态子批。若只
 * 根据首项设写，会让非独占子页绕过 COW，造成跨进程可见的数据修改。
 */
static __always_inline void set_write_prot_commit_flush_ptes(struct vm_area_struct *vma,
		struct folio *folio, struct page *page, unsigned long addr, pte_t *ptep,
		pte_t oldpte, pte_t ptent, int nr_ptes, struct mmu_gather *tlb)
{
	bool set_write;

	if (vma->vm_flags & VM_SHARED) {
		/* 共享页不走匿名独占性检查，而由文件写通知的 dirty 状态裁决。 */
		set_write = can_change_shared_pte_writable(vma, ptent);
		prot_commit_flush_ptes(vma, addr, ptep, oldpte, ptent, nr_ptes,
				       /* idx = */ 0, set_write, tlb);
		return;
	}

	/* 非匿名私有页也不应在此直设写，避免绕过文件页和 COW 的专属路径。 */
	set_write = maybe_change_pte_writable(vma, ptent) &&
		    (folio && folio_test_anon(folio));
	if (!set_write) {
		prot_commit_flush_ptes(vma, addr, ptep, oldpte, ptent, nr_ptes,
				       /* idx = */ 0, set_write, tlb);
		return;
	}
	commit_anon_folio_batch(vma, folio, page, addr, ptep, oldpte, ptent, nr_ptes, tlb);
}

/*
 * change_softleaf_pte() - 调整非 present 的迁移、设备私有和 marker 表项。
 *
 * 业务背景：PTE 不在内存中并不总是“空”；迁移条目保存未来页的访问语义，
 * userfaultfd marker 保存下一次缺页应通知用户态的约定。
 * 入参：pte/oldpte 是锁保护下的表项，cp_flags 描述写保护或解除写保护请求。
 * 返回：实际修改的表项数（0 或 1）。
 * 注意事项：迁移中保守去写位，避免在页尚未稳定时绕过 COW；poison/guard
 * marker 必须保留其故障语义。若把它们普通化，原本应 SIGBUS/SIGSEGV 的访问
 * 会以错误方式继续；解除 uffd marker 时清空，才会使下一次缺页不再陷入。
 */
static long change_softleaf_pte(struct vm_area_struct *vma,
	unsigned long addr, pte_t *pte, pte_t oldpte, unsigned long cp_flags)
{
	const bool uffd_wp = cp_flags & MM_CP_UFFD_WP;
	const bool uffd_wp_resolve = cp_flags & MM_CP_UFFD_WP_RESOLVE;
	softleaf_t entry = softleaf_from_pte(oldpte);
	pte_t newpte;

	if (softleaf_is_migration_write(entry)) {
		/* 迁移尚未落定物理页，先把未来映射限制为只读是最安全的保守选择。 */
		const struct folio *folio = softleaf_to_folio(entry);

		/*
		 * A protection check is difficult so
		 * just be safe and disable write
		 */
		if (folio_test_anon(folio))
			/* 匿名迁移项保留 exclusive 语义，后续迁移完成后 COW 判断才正确。 */
			entry = make_readable_exclusive_migration_entry(swp_offset(entry));
		else
			entry = make_readable_migration_entry(swp_offset(entry));
		newpte = swp_entry_to_pte(entry);
		if (pte_swp_soft_dirty(oldpte))
			newpte = pte_swp_mksoft_dirty(newpte);
	} else if (softleaf_is_device_private_write(entry)) {
		/* 设备私有页回迁/缺页路径另管一致性，不能假定 soft-dirty 可原样继承。 */
		/*
		 * We do not preserve soft-dirtiness. See
		 * copy_nonpresent_pte() for explanation.
		 */
		entry = make_readable_device_private_entry(swp_offset(entry));
		newpte = swp_entry_to_pte(entry);
		if (pte_swp_uffd_wp(oldpte))
			newpte = pte_swp_mkuffd_wp(newpte);
	} else if (softleaf_is_marker(entry)) {
		/* marker 是控制信息而非可访问页，保留或清除都直接改变下一次缺页路由。 */
		/*
		 * Ignore error swap entries unconditionally,
		 * because any access should sigbus/sigsegv
		 * anyway.
		 */
		if (softleaf_is_poison_marker(entry) ||
		    softleaf_is_guard_marker(entry))
			return 0;
		/*
		 * If this is uffd-wp pte marker and we'd like
		 * to unprotect it, drop it; the next page
		 * fault will trigger without uffd trapping.
		 */
		/* resolve 才有权删除 uffd marker；普通 mprotect 不得偷掉用户态观察点。 */
		if (uffd_wp_resolve) {
			pte_clear(vma->vm_mm, addr, pte);
			return 1;
		}
		return 0;
	} else {
		newpte = oldpte;
	}

	/* 对非 present 项也要维护 uffd 位，否则 present/迁移后用户态观察会断裂。 */
	if (uffd_wp)
		newpte = pte_swp_mkuffd_wp(newpte);
	else if (uffd_wp_resolve)
		newpte = pte_swp_clear_uffd_wp(newpte);

	/* 比较后再写，避免无变化的 PTE 产生不必要的硬件页表可见性操作。 */
	if (!pte_same(oldpte, newpte)) {
		set_pte_at(vma->vm_mm, addr, pte, newpte);
		return 1;
	}
	return 0;
}

/*
 * change_present_ptes() - 为已经驻留的页面生成并提交新的保护 PTE。
 *
 * 业务背景：VMA 已换成新策略后，已建立的硬件映射也必须同步，否则同一段
 * 地址会出现“新缺页按新权限、旧 TLB/PTE 按旧权限”的不一致。
 * 入参：页表锁由上层持有；newprot 是 VMA 算出的基础权限，cp_flags 附加
 * NUMA、uffd 写保护或可尝试直接设写等要求。
 * 出参：无；提交路径负责 TLB 刷新。
 * 注意事项：uffd 位要在新 PTE 上单独维护；当可写优化不安全时必须保留只读
 * 以触发 COW/writenotify，不能为了少一次缺页牺牲跟踪语义。
 */
static __always_inline void change_present_ptes(struct mmu_gather *tlb,
		struct vm_area_struct *vma, unsigned long addr, pte_t *ptep,
		int nr_ptes, unsigned long end, pgprot_t newprot,
		struct folio *folio, struct page *page, unsigned long cp_flags)
{
	const bool uffd_wp_resolve = cp_flags & MM_CP_UFFD_WP_RESOLVE;
	const bool uffd_wp = cp_flags & MM_CP_UFFD_WP;
	pte_t ptent, oldpte;

	oldpte = modify_prot_start_ptes(vma, addr, ptep, nr_ptes);
	/* 该 helper 取得架构要求的修改序列，不能用裸 set_pte 绕过并发协议。 */
	ptent = pte_modify(oldpte, newprot);

	if (uffd_wp)
		/* 写保护和 resolve 互斥；前者建立捕获点，后者撤销捕获点。 */
		ptent = pte_mkuffd_wp(ptent);
	else if (uffd_wp_resolve)
		ptent = pte_clear_uffd_wp(ptent);

	/*
	 * In some writable, shared mappings, we might want
	 * to catch actual write access -- see
	 * vma_wants_writenotify().
	 *
	 * In all writable, private mappings, we have to
	 * properly handle COW.
	 *
	 * In both cases, we can sometimes still change PTEs
	 * writable and avoid the write-fault handler, for
	 * example, if a PTE is already dirty and no other
	 * COW or special handling is required.
	 */
	/* 只在上层明确请求优化且基础保护尚未写时，才额外尝试设写。 */
	if ((cp_flags & MM_CP_TRY_CHANGE_WRITABLE) &&
	     !pte_write(ptent))
		set_write_prot_commit_flush_ptes(vma, folio, page,
			addr, ptep, oldpte, ptent, nr_ptes, tlb);
	else
		prot_commit_flush_ptes(vma, addr, ptep, oldpte, ptent,
			nr_ptes, /* idx = */ 0, /* set_write = */ false, tlb);
}

/*
 * change_pte_range() - 在一个 PMD 覆盖的区间内逐批同步叶级页表保护。
 *
 * 业务背景：这是普通页最终落到硬件 PTE 的位置，需同时照顾驻留页、空项、
 * swap/迁移等 softleaf 项以及 NUMA 采样的特殊 PROT_NONE。
 * 入参：pmd/addr/end 定位半开区间；tlb 归集刷新；cp_flags 指定附加语义。
 * 返回：实际改动页数，或 -EAGAIN 表示页表映射暂时拿不到、上层应重试。
 * 注意事项：PTE 锁覆盖读取、修改和提交；lazy MMU 模式把成批更新聚合。若
 * 不区分 present/none/softleaf，可能丢失 userfaultfd marker 或错误改写迁移项。
 */
static long change_pte_range(struct mmu_gather *tlb,
		struct vm_area_struct *vma, pmd_t *pmd, unsigned long addr,
		unsigned long end, pgprot_t newprot, unsigned long cp_flags)
{
	/* pages 只在实际改项后累加，供 NUMA/统计调用者区分扫描与生效。 */
	pte_t *pte, oldpte;
	spinlock_t *ptl;
	long pages = 0;
	bool is_private_single_threaded;
	bool prot_numa = cp_flags & MM_CP_PROT_NUMA;
	bool uffd_wp = cp_flags & MM_CP_UFFD_WP;
	int nr_ptes;

	/* 明确本次硬件叶级粒度，供 tlb gather 合并正确大小的失效范围。 */
	tlb_change_page_size(tlb, PAGE_SIZE);
	/* 映射并锁住叶表：之后的 oldpte 读取和写回必须在同一 PT 锁临界区。 */
	pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	if (!pte)
		return -EAGAIN;

	if (prot_numa)
		is_private_single_threaded = vma_is_single_threaded_private(vma);

	/* 先兑现此前延迟的刷新，避免把不同代次的页表改变混在一个批次。 */
	flush_tlb_batched_pending(vma->vm_mm);
	lazy_mmu_mode_enable();
	do {
		nr_ptes = 1;
		/* 每轮都从受锁 PTE 重新取值，不能用上一轮模板推测当前状态。 */
		oldpte = ptep_get(pte);
		if (pte_present(oldpte)) {
			const fpb_t flags = FPB_RESPECT_SOFT_DIRTY | FPB_RESPECT_WRITE;
			int max_nr_ptes = (end - addr) >> PAGE_SHIFT;
			struct folio *folio = NULL;
			struct page *page;

			/* Already in the desired state. */
			if (prot_numa && pte_protnone(oldpte))
				continue;

			/* 特殊映射没有普通 struct page，后续只能保守地不做 folio 优化。 */
			page = vm_normal_page(vma, addr, oldpte);
			if (page)
				folio = page_folio(page);

			/*
			 * Avoid trapping faults against the zero or KSM
			 * pages. See similar comment in change_huge_pmd.
			 */
			if (prot_numa &&
			    !folio_can_map_prot_numa(folio, vma,
						is_private_single_threaded)) {

				/* determine batch to skip */
				nr_ptes = mprotect_folio_pte_batch(folio,
					  pte, oldpte, max_nr_ptes, /* flags = */ 0);
				continue;
			}

			/* 批量边界同时尊重写位和 soft-dirty，避免把观测状态跨项复制。 */
			nr_ptes = mprotect_folio_pte_batch(folio, pte, oldpte, max_nr_ptes, flags);

			/*
			 * Optimize for the small-folio common case by
			 * special-casing it here. Compiler constant propagation
			 * plus copious amounts of __always_inline does wonders.
			 */
			if (likely(nr_ptes == 1)) {
				/* 小 folio 是常态，单项路径避免为批处理建立额外状态。 */
				change_present_ptes(tlb, vma, addr, pte, 1,
					end, newprot, folio, page, cp_flags);
			} else {
				/* 只有 helper 证明连续项兼容时才使用同一提交模板。 */
				change_present_ptes(tlb, vma, addr, pte,
					nr_ptes, end, newprot, folio, page,
					cp_flags);
			}

			pages += nr_ptes;
		} else if (pte_none(oldpte)) {
			/* 空 PTE 没有硬件权限可改，只有 uffd marker 需要留下未来缺页的意图。 */
			/*
			 * Nobody plays with any none ptes besides
			 * userfaultfd when applying the protections.
			 */
			if (likely(!uffd_wp))
				continue;

			if (userfaultfd_wp_use_markers(vma)) {
				/*
				 * For file-backed mem, we need to be able to
				 * wr-protect a none pte, because even if the
				 * pte is none, the page/swap cache could
				 * exist.  Doing that by install a marker.
				 */
				set_pte_at(vma->vm_mm, addr, pte,
					   make_pte_marker(PTE_MARKER_UFFD_WP));
				pages++;
			}
		} else  {
			pages += change_softleaf_pte(vma, addr, pte, oldpte, cp_flags);
		}
		/* nr_ptes 即使是跳过分支也会前进，防止大 folio 上重复扫描同一项。 */
	} while (pte += nr_ptes, addr += nr_ptes * PAGE_SIZE, addr != end);
	lazy_mmu_mode_disable();
	pte_unmap_unlock(pte - 1, ptl);

	return pages;
}

/*
 * pgtable_split_needed() - 判断透明大页是否必须降为 PTE 粒度。
 *
 * 业务背景：uffd-wp marker 只存在于 PTE；文件 THP 若保持 PMD 大映射，就无法
 * 为其中一个尚未建立 PTE 的页面保存写保护意图。
 * 入参：vma 和保护控制位；返回 true 时上层先拆 THP。
 * 注意事项：只对非匿名映射触发这一限制；不拆会丢 marker，滥拆则损失 THP 的
 * TLB 效益并增加页表内存。
 */
static inline bool
pgtable_split_needed(struct vm_area_struct *vma, unsigned long cp_flags)
{
	/*
	 * pte markers only resides in pte level, if we need pte markers,
	 * we need to split.  For example, we cannot wr-protect a file thp
	 * (e.g. 2M shmem) because file thp is handled differently when
	 * split by erasing the pmd so far.
	 */
	return (cp_flags & MM_CP_UFFD_WP) && !vma_is_anonymous(vma);
}

/*
 * pgtable_populate_needed() - 判断是否要先创建缺失的叶级页表。
 *
 * 业务背景：普通 mprotect 不必为未触及地址建立页表；但要求 uffd marker 时，
 * 空洞也要有位置存 marker，供将来的缺页处理读取。
 * 入参：vma/cp_flags；返回是否分配下级表。
 * 注意事项：只在 marker 模式分配，若每次 mprotect 都补表，会把稀疏地址空间
 * 的元数据成本放大为实际覆盖范围。
 */
static inline bool
pgtable_populate_needed(struct vm_area_struct *vma, unsigned long cp_flags)
{
	/* If not within ioctl(UFFDIO_WRITEPROTECT), then don't bother */
	if (!(cp_flags & MM_CP_UFFD_WP))
		return false;

	/* Populate if the userfaultfd mode requires pte markers */
	return userfaultfd_wp_use_markers(vma);
}

/*
 * change_pmd_prepare() - 在 marker 需要落叶时保证 PMD 下已有 PTE 表。
 *
 * 注意事项：分配失败返回 -ENOMEM 交由系统调用收尾；不能写一个不存在的 PTE 表。
 *
 * Populate the pgtable underneath for whatever reason if requested.
 * When {pte|pmd|...}_alloc() failed we treat it the same way as pgtable
 * allocation failures during page faults by kicking OOM and returning
 * error.
 */
#define  change_pmd_prepare(vma, pmd, cp_flags)				\
	({								\
		long err = 0;						\
		if (unlikely(pgtable_populate_needed(vma, cp_flags))) {	\
			if (pte_alloc(vma->vm_mm, pmd))			\
				err = -ENOMEM;				\
		}							\
		err;							\
	})

/*
 * change_prepare() - 为 PUD/P4D/PGD 复用按需补下级表的分配规则。
 *
 * 注意事项：各级 allocator 的成功返回约定不同，因此不能误用 PTE 宏；误判会把
 * 有效指针当错误或在 NULL 上继续遍历。
 *
 * This is the general pud/p4d/pgd version of change_pmd_prepare(). We need to
 * have separate change_pmd_prepare() because pte_alloc() returns 0 on success,
 * while {pmd|pud|p4d}_alloc() returns the valid pointer on success.
 */
#define  change_prepare(vma, high, low, addr, cp_flags)			\
	  ({								\
		long err = 0;						\
		if (unlikely(pgtable_populate_needed(vma, cp_flags))) {	\
			low##_t *p = low##_alloc(vma->vm_mm, high, addr); \
			if (p == NULL)					\
				err = -ENOMEM;				\
		}							\
		err;							\
	})

/*
 * change_pmd_range() - 沿 PMD 槽位处理普通 PTE 或整块透明大页。
 *
 * 业务背景：一个 PMD 可能是下级 PTE 表，也可能直接映射 2 MiB THP；部分范围
 * 或需要 uffd PTE marker 时，整块保护无法准确表达，必须拆回叶级。
 * 入参：pud、addr/end 为本层边界，newprot/cp_flags 为目标语义。
 * 返回：改动页数，内存分配失败时返回负 errno。
 * 注意事项：大页完整且可直接处理时保留大页，避免拆页的成本；不能直接处理时
 * 先 split 再重读/准备页表。省略重试会在 split 改变页表形态后使用过期 PMD。
 */
static inline long change_pmd_range(struct mmu_gather *tlb,
		struct vm_area_struct *vma, pud_t *pud, unsigned long addr,
		unsigned long end, pgprot_t newprot, unsigned long cp_flags)
{
	pmd_t *pmd;
	unsigned long next;
	long pages = 0;
	unsigned long nr_huge_updates = 0;

	pmd = pmd_offset(pud, addr);
	/* pmd_addr_end 保证每轮绝不跨 PMD，跨层边界由外层继续处理。 */
	do {
		long ret;
		pmd_t _pmd;
again:
		next = pmd_addr_end(addr, end);

		ret = change_pmd_prepare(vma, pmd, cp_flags);
		/* marker 所需页表分配失败按缺页分配失败处理，不能假装保护已建立。 */
		if (ret) {
			pages = ret;
			break;
		}

		/* 没有下级映射且无需 marker 时没有 PTE 可同步，直接越过该槽。 */
		if (pmd_none(*pmd))
			goto next;

		/* 先无锁快照识别大页；大页 helper 会自行处理其必要的同步。 */
		_pmd = pmdp_get_lockless(pmd);
		if (pmd_is_huge(_pmd)) {
			/* 整块覆盖时优先保持 THP；这既减少 PTE 数也保留其性能特性。 */
			if ((next - addr != HPAGE_PMD_SIZE) ||
			    pgtable_split_needed(vma, cp_flags)) {
				__split_huge_pmd(vma, pmd, addr, false);
				/*
				 * For file-backed, the pmd could have been
				 * cleared; make sure pmd populated if
				 * necessary, then fall-through to pte level.
				 */
				ret = change_pmd_prepare(vma, pmd, cp_flags);
				if (ret) {
					pages = ret;
					break;
				}
			} else {
				/* huge helper 成功即消费整个 PMD；返回 0 表示需按新形态重试。 */
				ret = change_huge_pmd(tlb, vma, pmd,
						addr, newprot, cp_flags);
			/* 非零表示 helper 已处理或失败；零表示必须以普通 PTE 方式重试。 */
				if (ret) {
					if (ret == HPAGE_PMD_NR) {
						pages += HPAGE_PMD_NR;
						nr_huge_updates++;
					}

					/* huge pmd was handled */
					goto next;
				}
			}
			/* fall through, the trans huge pmd just split */
		}

		/* 不是可整块更新的大页后，才进入受 PTE 锁保护的普通页路径。 */
		ret = change_pte_range(tlb, vma, pmd, addr, next, newprot,
				       cp_flags);
		if (ret < 0)
			/* 分裂或并发页表变化后重新从当前 PMD 开始，不能继续使用旧层级判断。 */
			goto again;
		pages += ret;
next:
		/* 大地址空间遍历主动让出 CPU，避免长 mprotect 独占调度器。 */
		cond_resched();
	} while (pmd++, addr = next, addr != end);

	if (nr_huge_updates)
		/* 单独计数让 NUMA 策略知道有多少大页权限被改，而非把它们当普通页。 */
		count_vm_numa_events(NUMA_HUGE_PTE_UPDATES, nr_huge_updates);
	return pages;
}

/*
 * change_pud_range() - 处理 PUD 级叶项或继续下钻，并通知二级 MMU。
 *
 * 业务背景：KVM 等 mmu notifier 使用者会缓存本进程页表；保护改变前后必须
 * 用 invalidate 区间包围修改，令它们撤销自己的旧权限映射。
 * 入参：p4d 与半开区间定位本层，tlb/newprot/cp_flags 传递下层工作。
 * 返回：已改页数或分配失败错误。
 * 注意事项：只有实际遇到有效 PUD 才开始 notifier，结束必须成对发送；遗漏
 * end 通知会让监听者永久处在失效窗口，遗漏 start 则可能继续访问旧权限。
 */
static inline long change_pud_range(struct mmu_gather *tlb,
		struct vm_area_struct *vma, p4d_t *p4d, unsigned long addr,
		unsigned long end, pgprot_t newprot, unsigned long cp_flags)
{
	struct mmu_notifier_range range;
	pud_t *pudp, pud;
	unsigned long next;
	long pages = 0, ret;

	range.start = 0;

	pudp = pud_offset(p4d, addr);
	/* notifier 覆盖实际改动的 PUD 子范围，而不是不必要地通知整个 mm。 */
	do {
again:
		next = pud_addr_end(addr, end);
		ret = change_prepare(vma, pudp, pmd, addr, cp_flags);
		/* 分配契约与 PMD 层一致：错误向上传递，禁止部分地继续下钻。 */
		if (ret) {
			pages = ret;
			break;
		}

		/* 读取 PUD 后先滤掉空槽，避免为没有映射的区间触发虚拟化失效。 */
		pud = pudp_get(pudp);
		if (pud_none(pud))
			/* 空 PUD 没有下级映射，也未触发 marker 分配时可直接进入下一槽。 */
			continue;

		if (!range.start) {
			/* 第一个有效项才开始通知，范围终点固定为本 VMA 子区间的 end。 */
			mmu_notifier_range_init(&range,
						MMU_NOTIFY_PROTECTION_VMA, 0,
						vma->vm_mm, addr, end);
			mmu_notifier_invalidate_range_start(&range);
		}

		/* PUD 叶项也是大页：部分覆盖或 marker 需求均要求先拆分。 */
		if (pud_leaf(pud)) {
			/* split 后跳回 again 重新读取 PUD，避免继续用拆分前的叶项快照。 */
			/* 完整 PUD 是唯一允许整块改写的形状，任何子范围都必须细化到下级。 */
			if ((next - addr != PUD_SIZE) ||
			    pgtable_split_needed(vma, cp_flags)) {
				__split_huge_pud(vma, pudp, addr);
				goto again;
			} else {
				/* PUD 巨页可整块改时交专用 helper，避免错误当成普通 PMD 表。 */
				ret = change_huge_pud(tlb, vma, pudp,
						      addr, newprot, cp_flags);
				if (ret == 0)
					goto again;
				/* huge pud was handled */
				if (ret == HPAGE_PUD_NR)
					pages += HPAGE_PUD_NR;
				continue;
			}
		}

		/* 非叶 PUD 继续下钻到 PMD；这里不能把 PUD 指针误交给 PTE 层。 */
		pages += change_pmd_range(tlb, vma, pudp, addr, next, newprot,
					  cp_flags);
		/* PUD 段结束后由循环推进到准确 next，避免巨页边界重复或遗漏。 */
	} while (pudp++, addr = next, addr != end);

	/* notifier 已开始则无论本层走大页还是下钻都必须关闭其失效区间。 */
	if (range.start)
		/* 与 invalidate_start 成对，通知二级 MMU 可以重新建立新权限翻译。 */
		mmu_notifier_invalidate_range_end(&range);

	return pages;
}

/*
 * change_p4d_range() - 遍历 P4D，分配必要下级表后交给 PUD 层。
 *
 * 业务背景：五级页表是同一保护操作的地址分片框架；折叠层在某些架构上虽
 * 形式存在，也必须走统一边界计算，才能不漏跨边界区间。
 * 入参：pgd、addr/end 和目标保护；返回已处理页数或负 errno。
 * 注意事项：遇到空/坏项跳过即可；只有 userfaultfd marker 要落到叶级时才会
 * 主动补表。盲目分配全部层级会让普通 mprotect 无谓消耗页表内存。
 */
static inline long change_p4d_range(struct mmu_gather *tlb,
		struct vm_area_struct *vma, pgd_t *pgd, unsigned long addr,
		unsigned long end, pgprot_t newprot, unsigned long cp_flags)
{
	p4d_t *p4d;
	unsigned long next;
	long pages = 0, ret;

	p4d = p4d_offset(pgd, addr);
	/* 这一层只负责地址分片；实际权限判断保持在可见叶项层。 */
	do {
		next = p4d_addr_end(addr, end);
		ret = change_prepare(vma, p4d, pud, addr, cp_flags);
		/* 仅 marker 模式会实际补表；普通路径空项保持稀疏。 */
		if (ret)
			return ret;
		/* 坏项按架构规则清理后跳过，继续向下会把损坏页表当成有效指针。 */
		if (p4d_none_or_clear_bad(p4d))
			continue;
		pages += change_pud_range(tlb, vma, p4d, addr, next, newprot,
					  cp_flags);
	} while (p4d++, addr = next, addr != end);

	/* pages 只统计真正下钻的范围，错误已由下层直接返回。 */
	return pages;
}

/*
 * change_protection_range() - 从 PGD 向下分派一个 VMA 子区间的页表更新。
 *
 * 业务背景：VMA 是软件权限声明，PGD 到 PTE 是硬件翻译树；本函数把二者连接
 * 起来，并用 mmu_gather 把跨层 TLB 回收/刷新保持在同一事务中。
 * 入参：addr/end 必须是非空半开区间，newprot 来自 VMA，cp_flags 为附加模式。
 * 返回：改动页数或负 errno。
 * 注意事项：tlb_start_vma/tlb_end_vma 必须包住全范围，供架构完成正确的缓存
 * 维护；少了任一端，旧权限可能留在 CPU 缓存而产生短暂越权或误拒绝。
 */
static long change_protection_range(struct mmu_gather *tlb,
		struct vm_area_struct *vma, unsigned long addr,
		unsigned long end, pgprot_t newprot, unsigned long cp_flags)
{
	/* mm 从 VMA 取得，确保页表根与本次 VMA 同属一个地址空间。 */
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd;
	unsigned long next;
	long pages = 0, ret;

	BUG_ON(addr >= end);
	pgd = pgd_offset(mm, addr);
	tlb_start_vma(tlb, vma);
	/* pgd_addr_end 的分段也保证不会让下层 helper 越过本级覆盖范围。 */
	do {
		next = pgd_addr_end(addr, end);
		ret = change_prepare(vma, pgd, p4d, addr, cp_flags);
		/* 顶层也遵循同一分配失败边界，不能带着 NULL 下级指针继续。 */
		if (ret) {
			pages = ret;
			break;
		}
		if (pgd_none_or_clear_bad(pgd))
			continue;
		pages += change_p4d_range(tlb, vma, pgd, addr, next, newprot,
					  cp_flags);
	} while (pgd++, addr = next, addr != end);

	/* VMA 范围结束标记与 start 配对，交给 tlb 层完成架构相关收尾。 */
	tlb_end_vma(tlb, vma);

	return pages;
}

/*
 * change_protection() - 根据 VMA 类型选择普通页表或 HugeTLB 的保护更新。
 *
 * 业务背景：mprotect、soft-dirty、userfaultfd 和 NUMA 采样共用这条路径，
 * 但 HugeTLB 不使用普通的多级 PTE 遍历。
 * 入参：tlb 由上层生命周期管理；start/end 是已锁定 VMA 内的区间；cp_flags
 * 指定 NUMA、uffd 写保护等额外行为。
 * 返回：实际改动页数或下层错误。
 * 注意事项：NUMA 采样故意用 PAGE_NONE 触发后续访问缺页来收集热度；把它混为
 * 常规 mprotect 会把用户显式权限改坏。调用者之后仍须 finish tlb 才能完成刷新。
 */
long change_protection(struct mmu_gather *tlb,
		       struct vm_area_struct *vma, unsigned long start,
		       unsigned long end, unsigned long cp_flags)
{
	pgprot_t newprot = vma->vm_page_prot;
	long pages;

	BUG_ON((cp_flags & MM_CP_UFFD_WP_ALL) == MM_CP_UFFD_WP_ALL);

#ifdef CONFIG_NUMA_BALANCING
	/* NUMA 采样以 PAGE_NONE 故意制造可观测访问，基础 VMA 权限不在此丢失。 */
	/*
	 * Ordinary protection updates (mprotect, uffd-wp, softdirty tracking)
	 * are expected to reflect their requirements via VMA flags such that
	 * vma_set_page_prot() will adjust vma->vm_page_prot accordingly.
	 */
	if (cp_flags & MM_CP_PROT_NUMA)
		newprot = PAGE_NONE;
#else
	WARN_ON_ONCE(cp_flags & MM_CP_PROT_NUMA);
#endif

	/* HugeTLB 有独立页表格式，不能落入普通 PTE walker。 */
	if (is_vm_hugetlb_page(vma))
		/* 专用 helper 了解 HugeTLB reservation 和页尺寸，普通 walker 不具备。 */
		pages = hugetlb_change_protection(vma, start, end, newprot,
						  cp_flags);
	else
		pages = change_protection_range(tlb, vma, start, end, newprot,
						cp_flags);

	return pages;
}

/*
 * prot_none_pte_entry() - 检查普通 PFNMAP PTE 是否允许降为 PROT_NONE。
 *
 * 业务背景：PFNMAP/MIXEDMAP 可直接映射设备或特殊物理帧，架构可能禁止把某些
 * PFN 改成指定保护，必须在改变 VMA/页表前逐项拒绝。
 * 入参：walk->private 保存目标 pgprot；pte 是 walker 当前位置。
 * 返回：0 可继续，-EACCES 阻止整个 mprotect。
 * 注意事项：这是一项预检而非修改；若等到 VMA 已拆分、额度已记账才发现，
 * 回滚多个状态既复杂又易留下半更新。
 */
static int prot_none_pte_entry(pte_t *pte, unsigned long addr,
			       unsigned long next, struct mm_walk *walk)
{
	return pfn_modify_allowed(pte_pfn(ptep_get(pte)),
				  *(pgprot_t *)(walk->private)) ?
		0 : -EACCES;
}

/*
 * prot_none_hugetlb_entry() - 对 HugeTLB 项执行与普通 PTE 相同的 PFN 预检。
 *
 * 业务背景：HugeTLB walker 的回调签名不同，但设备 PFN 的架构限制不能因
 * 页尺寸变大而消失。
 * 入参：hmask/addr/next 描述巨页范围，目标保护仍从 walk->private 取得。
 * 返回：0 或 -EACCES。
 * 注意事项：必须与普通回调给出同一判定，否则同一设备映射会因页大小不同
 * 出现不一致的 mprotect 结果。
 */
static int prot_none_hugetlb_entry(pte_t *pte, unsigned long hmask,
				   unsigned long addr, unsigned long next,
				   struct mm_walk *walk)
{
	return pfn_modify_allowed(pte_pfn(ptep_get(pte)),
				  *(pgprot_t *)(walk->private)) ?
		0 : -EACCES;
}

/*
 * prot_none_test() - 告诉 pagewalk 不跳过任何候选区间。
 *
 * 业务背景：此 walker 的目的不是统计，而是找出所有可能受架构限制的 PFN。
 * 入参：addr/next 是 walker 建议的区间，walk 无需额外状态。
 * 返回：0 让 walker 继续进入下级项。
 * 注意事项：若错误跳过空洞旁的有效项，预检会漏掉禁止修改的 PFN，随后页表
 * 更新可能违反设备/架构约束。
 */
static int prot_none_test(unsigned long addr, unsigned long next,
			  struct mm_walk *walk)
{
	return 0;
}

static const struct mm_walk_ops prot_none_walk_ops = {
	/* 同一 walker 覆盖普通页和 HugeTLB，写锁模式防止预检时 PTE 变化。 */
	.pte_entry		= prot_none_pte_entry,
	.hugetlb_entry		= prot_none_hugetlb_entry,
	.test_walk		= prot_none_test,
	.walk_lock		= PGWALK_WRLOCK,
};

/*
 * mprotect_fixup() - 将一个 VMA 子区间正式切成新权限并同步已存在页表。
 *
 * 业务背景：用户一次 mprotect 常跨多个 VMA，也常只覆盖一个 VMA 的中段；
 * 因而必须先处理提交额度，再通过 vma_modify_flags() 按边界切分/合并，最后
 * 才把新策略下推到 PTE。
 * 入参：vmi/pprev 维护 VMA 遍历位置，tlb 由整次系统调用共享；start/end 是
 * 当前连续 VMA 内的半开区间，newflags 是已校验的目标 VMA 标志。
 * 返回：0 成功；-EPERM、-ENOMEM 或 VMA helper 的错误。成功后 *pprev 更新为
 * 实际生效（可能是新拆出的）VMA，调用者必须用它继续遍历。
 * 注意事项：调用者持有 mmap 写锁；它保护 flags/page_prot 和 VMA 拓扑。先做
 * PFN 预检和额度检查，避免半更新；失败路径只退回本函数新收取的 charged，
 * 漏退会永久耗尽 overcommit 额度，过早退则可能允许无保证的私有写入。
 */
int
mprotect_fixup(struct vma_iterator *vmi, struct mmu_gather *tlb,
	       struct vm_area_struct *vma, struct vm_area_struct **pprev,
	       unsigned long start, unsigned long end, vm_flags_t newflags)
{
	/* charged 初始为零，使任何早期失败都可走统一 fail 而不误退旧额度。 */
	struct mm_struct *mm = vma->vm_mm;
	const vma_flags_t old_vma_flags = READ_ONCE(vma->flags);
	vma_flags_t new_vma_flags = legacy_to_vma_flags(newflags);
	long nrpages = (end - start) >> PAGE_SHIFT;
	unsigned int mm_cp_flags = 0;
	unsigned long charged = 0;
	int error;

	/* 密封 VMA 明确禁止后来降低/提升保护，先拒绝才能保持 seal 的承诺。 */
	/* vma_is_sealed 是不可变策略屏障；不需要也不允许尝试局部拆分规避它。 */
	if (vma_is_sealed(vma))
		/* seal 失败无需清理，因为这里尚未收取额度或改变 VMA。 */
		return -EPERM;

	/* 权限对未变时不切 VMA、不刷新 TLB；但要更新遍历前驱给调用者。 */
	/* 比较的是访问权限对；其他可保留属性不应单独触发一次无效页表遍历。 */
	if (vma_flags_same_pair(&old_vma_flags, &new_vma_flags)) {
		*pprev = vma;
		return 0;
	}

	/*
	 * Do PROT_NONE PFN permission checks here when we can still
	 * bail out without undoing a lot of state. This is a rather
	 * uncommon case, so doesn't need to be very optimized.
	 */
	/* 只有 PFN 直映射且目标无访问权时才需要昂贵逐页预检。 */
	if (arch_has_pfn_modify_check() &&
	    vma_flags_test_any(&old_vma_flags, VMA_PFNMAP_BIT,
			       VMA_MIXEDMAP_BIT) &&
	    !vma_flags_test_any_mask(&new_vma_flags, VMA_ACCESS_FLAGS)) {
		pgprot_t new_pgprot = vm_get_page_prot(newflags);

		error = walk_page_range(current->mm, start, end,
				&prot_none_walk_ops, &new_pgprot);
		if (error)
			/* walker 已发现禁止 PFN，直接返回保证 VMA 拓扑保持原样。 */
			return error;
	}

	/*
	 * If we make a private mapping writable we increase our commit;
	 * but (without finer accounting) cannot reduce our commit if we
	 * make it unwritable again except in the anonymous case where no
	 * anon_vma has yet to be assigned.
	 *
	 * hugetlb mapping were accounted for even if read-only so there is
	 * no need to account for them here.
	 */
	/* 私有区转可写意味着未来 COW 可能要占匿名页，先保留提交额度。 */
	if (vma_flags_test(&new_vma_flags, VMA_WRITE_BIT)) {
		/* Check space limits when area turns into data. */
		/* 先检查资源上限，再收费；反过来会在失败时增加不必要回滚。 */
		if (!may_expand_vm(mm, &new_vma_flags, nrpages) &&
		    may_expand_vm(mm, &old_vma_flags, nrpages))
			return -ENOMEM;
		/* 已写、共享、hugetlb 或 noreserve 各有独立记账规则，不能重复收费。 */
		if (!vma_flags_test_any(&old_vma_flags,
				VMA_ACCOUNT_BIT, VMA_WRITE_BIT, VMA_HUGETLB_BIT,
				VMA_SHARED_BIT, VMA_NORESERVE_BIT)) {
			/* charged 只记录本次新增承诺，失败时不触碰原先 VMA 的记账。 */
			charged = nrpages;
			if (security_vm_enough_memory_mm(mm, charged))
				return -ENOMEM;
			vma_flags_set(&new_vma_flags, VMA_ACCOUNT_BIT);
		}
	} else if (vma_flags_test(&old_vma_flags, VMA_ACCOUNT_BIT) &&
		   vma_is_anonymous(vma) && !vma->anon_vma) {
		vma_flags_clear(&new_vma_flags, VMA_ACCOUNT_BIT);
	}

	/* helper 可复用相邻 VMA 或切出三段；返回值才是后续应操作的那一段。 */
	/* 仅未绑定 anon_vma 的匿名区可安全取消记账，已有 COW 关系不能猜测可回收。 */
	vma = vma_modify_flags(vmi, *pprev, vma, start, end, &new_vma_flags);
	/* vma_modify_flags 可能因拆分分配失败；此时尚不能碰页表。 */
	if (IS_ERR(vma)) {
		error = PTR_ERR(vma);
		goto fail;
	}

	*pprev = vma;

	/* 此后 vma 指针已是 stable target，按写锁规则发布它的 flags 与 pgprot。 */
	/*
	 * vm_flags and vm_page_prot are protected by the mmap_lock
	 * held in write mode.
	 */
	/* 发布 flags 前标记 VMA 写序列，使 lockless 读者可检测并重试。 */
	vma_start_write(vma);
	vma_flags_reset_once(vma, &new_vma_flags);
	if (vma_wants_manual_pte_write_upgrade(vma))
		mm_cp_flags |= MM_CP_TRY_CHANGE_WRITABLE;
	vma_set_page_prot(vma);

	/* VMA 策略已发布后立即同步已有 PTE；尚未驻留的页以后按新 VMA 缺页。 */
	change_protection(tlb, vma, start, end, mm_cp_flags);

	/* 只有从“已记账”转为“无需记账”才归还额度，避免重复释放。 */
	if (vma_flags_test(&old_vma_flags, VMA_ACCOUNT_BIT) &&
	    !vma_flags_test(&new_vma_flags, VMA_ACCOUNT_BIT))
		vm_unacct_memory(nrpages);

	/*
	 * Private VM_LOCKED VMA becoming writable: trigger COW to avoid major
	 * fault on access.
	 */
	if (vma_flags_test(&new_vma_flags, VMA_WRITE_BIT) &&
	    vma_flags_test(&old_vma_flags, VMA_LOCKED_BIT) &&
	    !vma_flags_test_any(&old_vma_flags, VMA_WRITE_BIT, VMA_SHARED_BIT))
		/* 锁页语义要求此时准备可写私有副本，不能把延迟成本推到首个访问。 */
		populate_vma_page_range(vma, start, end, NULL);

	/* 统计按旧类减、新类加，不能只累计，否则 /proc 看到的映射类别会漂移。 */
	/* 锁定私有页转可写时预触发 COW，避免第一次用户访问承担重大缺页延迟。 */
	vm_stat_account(mm, vma_flags_to_legacy(old_vma_flags), -nrpages);
	newflags = vma_flags_to_legacy(new_vma_flags);
	vm_stat_account(mm, newflags, nrpages);
	perf_event_mmap(vma);
	return 0;

fail:
	vm_unacct_memory(charged);
	return error;
}

/*
 * do_mprotect_pkey() - 实现 mprotect/pkey_mprotect 的整段 VMA 事务。
 *
 * 业务背景：系统调用请求是一段用户虚拟地址，但内核必须逐个 VMA 校验可访问
 * 权限、LSM、文件专属回调和架构规则，再把每段安全地交给 mprotect_fixup。
 * 入参：start/len 是用户半开范围；prot 是 r/w/x 与增长方向；pkey 为保护键，
 * -1 表示传统 mprotect 不指定键。
 * 返回：0 成功；EINVAL（形状、pkey、架构参数）、ENOMEM（范围不完整或额度）、
 * EACCES（超过 VM_MAY* 或安全策略）、EINTR（等待写锁被信号打断）等。
 * 注意事项：取得 mmap 写锁后才允许 VMA 拆分和 flags 修改，并且同一个 tlb
 * 覆盖整次调用、最后统一 finish。若中途直接返回或不检查每段连续覆盖，前段
 * 已更新而后段遗漏会留下难以解释的部分 mprotect 结果。
 *
 * pkey==-1 when doing a legacy mprotect()
 */
static int do_mprotect_pkey(unsigned long start, size_t len,
		unsigned long prot, int pkey)
{
	/* reqprot 保存原请求，循环中临时增加 EXEC 后要恢复它再处理下一 VMA。 */
	unsigned long nstart, end, tmp, reqprot;
	struct vm_area_struct *vma, *prev;
	int error;
	const int grows = prot & (PROT_GROWSDOWN|PROT_GROWSUP);
	const bool rier = (current->personality & READ_IMPLIES_EXEC) &&
				(prot & PROT_READ);
	struct mmu_gather tlb;
	struct vma_iterator vmi;

	/* ARM 等架构的地址标签不能参与页表查找，否则同一地址会被误判未映射。 */
	start = untagged_addr(start);

	prot &= ~(PROT_GROWSDOWN|PROT_GROWSUP);
	/* 两个方向同时出现没有可定义的扩展端，必须在取锁前拒绝。 */
	if (grows == (PROT_GROWSDOWN|PROT_GROWSUP)) /* can't be both */
		return -EINVAL;

	/* mprotect 的硬件权限粒度是页；不对齐请求没有无歧义的首页语义。 */
	if (start & ~PAGE_MASK)
		return -EINVAL;
	/* 零长度是无副作用成功，不能因此取锁或要求地址已映射。 */
	if (!len)
		return 0;
	len = PAGE_ALIGN(len);
	/* 加法回绕会把巨大范围伪装成小范围，end 必须严格大于 start。 */
	end = start + len;
	if (end <= start)
		return -ENOMEM;
	if (!arch_validate_prot(prot, start))
		return -EINVAL;

	reqprot = prot;

	/* 可被信号打断地等写锁，避免用户请求在长期 VMA 修改后不可中断地阻塞。 */
	if (mmap_write_lock_killable(current->mm))
		return -EINTR;

	/*
	 * If userspace did not allocate the pkey, do not let
	 * them use it here.
	 */
	/* 获锁后所有失败分支经 out 解锁；从这里起禁止裸 return。 */
	error = -EINVAL;
	/* pkey 是按 mm 分配的能力，未分配编号不能被用户当作任意权限标签。 */
	if ((pkey != -1) && !mm_pkey_is_allocated(current->mm, pkey))
		goto out;

	/* iterator 在同一写锁下遍历；这使 VMA 之间的“连续覆盖”检查稳定可靠。 */
	vma_iter_init(&vmi, current->mm, start);
	vma = vma_find(&vmi, end);
	error = -ENOMEM;
	/* 起点之后没有 VMA 表示整个请求未映射，直接以 ENOMEM 告知用户。 */
	if (!vma)
		goto out;

	/* GROWS* 不是任意扩展地址范围，只能用于原本声明同方向增长的栈类 VMA。 */
	if (unlikely(grows & PROT_GROWSDOWN)) {
		/* grow-down 取 VMA 起点前，仍要求请求与这个 VMA 实际相交。 */
		if (vma->vm_start >= end)
			goto out;
		/* 向下增长将起点扩至 VMA 头，保护策略覆盖整个实际栈映射。 */
		start = vma->vm_start;
		error = -EINVAL;
		if (!(vma->vm_flags & VM_GROWSDOWN))
			goto out;
	} else {
		/* 非向下增长情形保留用户 start；向上增长会在下方特例延至 VMA 尾。 */
		/* 普通方向从 start 开始必须已有覆盖，不能跳到下一个 VMA。 */
		if (vma->vm_start > start)
			goto out;
		if (unlikely(grows & PROT_GROWSUP)) {
			/* 对应地向上增长扩到 VMA 尾，避免只改中间片段破坏增长语义。 */
			end = vma->vm_end;
			error = -EINVAL;
			if (!(vma->vm_flags & VM_GROWSUP))
				goto out;
		}
	}

	/* 中段切分需要真实前驱；迭代器前驱会随 fixup 更新而保持连续。 */
	prev = vma_prev(&vmi);
	if (start > vma->vm_start)
		prev = vma;

	/* 一个 gather 覆盖全部 VMA 段，减少跨段 TLB shootdown，并保证统一收尾。 */
	tlb_gather_mmu(&tlb, current->mm);
	nstart = start;
	tmp = vma->vm_start;
	for_each_vma_range(vmi, vma, end) {
		vm_flags_t mask_off_old_flags;
		vma_flags_t new_vma_flags;
		vm_flags_t newflags;
		int new_vma_pkey;

		/* 发现洞即拒绝：mprotect 必须全成或在首个未覆盖处失败，不能静默跳洞。 */
		if (vma->vm_start != tmp) {
			error = -ENOMEM;
			break;
		}

		/* Does the application expect PROT_READ to imply PROT_EXEC */
		/* personality 是兼容旧 ABI 的例外，且仍不得超过 VM_MAYEXEC 上限。 */
		if (rier && (vma->vm_flags & VM_MAYEXEC))
			prot |= PROT_EXEC;

		/*
		 * Each mprotect() call explicitly passes r/w/x permissions.
		 * If a permission is not passed to mprotect(), it must be
		 * cleared from the VMA.
		 */
		/* 显式请求的访问位必须覆盖旧值，其他映射属性才允许继承。 */
		mask_off_old_flags = VM_ACCESS_FLAGS | VM_FLAGS_CLEAR;

		/* 架构可替换默认键；随后只保留旧 VMA 中不属于访问权限的属性。 */
		new_vma_pkey = arch_override_mprotect_pkey(vma, prot, pkey);
		newflags = calc_vm_prot_bits(prot, new_vma_pkey);
		newflags |= (vma->vm_flags & ~mask_off_old_flags);
		new_vma_flags = legacy_to_vma_flags(newflags);

		/* newflags >> 4 shift VM_MAY% in place of VM_% */
		/* VM_MAY* 是 mmap 时授予的上限，mprotect 不能凭空追加读写执行权。 */
		if ((newflags & ~(newflags >> 4)) & VM_ACCESS_FLAGS) {
			error = -EACCES;
			break;
		}

		/* W^X 策略阻止同段可写可执行，减少把可改数据直接当代码执行的风险。 */
		if (map_deny_write_exec(&vma->flags, &new_vma_flags)) {
			error = -EACCES;
			break;
		}

		/* Allow architectures to sanity-check the new flags */
		/* 架构最终检查其 PTE 编码能力，通用层不能假定所有 r/w/x 组合可实现。 */
		if (!arch_validate_flags(newflags)) {
			error = -EINVAL;
			break;
		}

		/* LSM 在状态改变前裁决，例如阻止策略不允许的可执行文件映射。 */
		error = security_file_mprotect(vma, reqprot, prot);
		if (error)
			break;

		/* 当前 VMA 的尾与用户终点取较小者，保证 fixup 永不越过本段。 */
		tmp = vma->vm_end;
		if (tmp > end)
			tmp = end;

		/* 特殊映射可先拒绝或准备自己的后端状态，通用 VMA 更新不能越过它。 */
		if (vma->vm_ops && vma->vm_ops->mprotect) {
			error = vma->vm_ops->mprotect(vma, nstart, tmp, newflags);
			if (error)
				break;
		}

		/* 仅在所有前置裁决通过后才修改 VMA/PTE；失败不会继续碰后续段。 */
		error = mprotect_fixup(&vmi, &tlb, vma, &prev, nstart, tmp, newflags);
		if (error)
			break;

		/* fixup 可合并或拆分 VMA，必须从 iterator 取新尾，不能沿用旧 vm_end。 */
		tmp = vma_iter_end(&vmi);
		nstart = tmp;
		prot = reqprot;
	}
	/* 即使某段失败也要完成已登记刷新，否则先前成功段的旧 TLB 仍可被使用。 */
	tlb_finish_mmu(&tlb);

	/* 正常循环却未达到终点同样是地址洞，防止返回成功的部分覆盖。 */
	if (!error && tmp < end)
		error = -ENOMEM;

out:
	/* 所有 goto out 在释放写锁后才向用户返回，不能遗留阻塞其他 mmap 的锁。 */
	mmap_write_unlock(current->mm);
	return error;
}

SYSCALL_DEFINE3(mprotect, unsigned long, start, size_t, len,
		unsigned long, prot)
{
	/* 传统入口不指定 pkey，所有实质校验和修改集中在共同实现中。 */
	return do_mprotect_pkey(start, len, prot, -1);
}

#ifdef CONFIG_ARCH_HAS_PKEYS

SYSCALL_DEFINE4(pkey_mprotect, unsigned long, start, size_t, len,
		unsigned long, prot, int, pkey)
{
	/* 与 mprotect 共用 VMA/页表事务，只额外要求调用者拥有该保护键。 */
	return do_mprotect_pkey(start, len, prot, pkey);
}

SYSCALL_DEFINE2(pkey_alloc, unsigned long, flags, unsigned long, init_val)
{
	int pkey;
	int ret;

	/*
	 * 业务背景：pkey 是 mm 内有限的硬件/架构资源；先在 mmap 写锁下占号，再
	 * 写入当前线程的访问寄存器状态，保证并发 VMA 改键看不到半初始化键。
	 * 注意事项：寄存器设置失败必须归还刚占的号，否则可分配键会泄漏并很快
	 * 变成 ENOSPC；flags/init_val 的严格校验避免未知位被悄悄解释。
	 */
	/* No flags supported yet. */
	if (flags)
		return -EINVAL;
	/* check for unsupported init values */
	if (init_val & ~PKEY_ACCESS_MASK)
		return -EINVAL;

	mmap_write_lock(current->mm);
	pkey = mm_pkey_alloc(current->mm);
	/* 位图没有空键时以 ENOSPC 返回；不要把 -1 当成可用键继续配置。 */

	ret = -ENOSPC;
	if (pkey == -1)
		/* 此时未获得编号，直接收尾而不触碰架构访问寄存器。 */
		goto out;

	ret = arch_set_user_pkey_access(pkey, init_val);
	/* 失败回滚遵循“先撤资源再解锁”，让其他线程只见全成或全败的位图。 */
	/* 硬件访问寄存器设置失败时，位图占用必须在同一锁内撤销。 */
	if (ret) {
		/* 归还编号使下一次分配仍可使用它，避免一次寄存器失败永久吃掉槽位。 */
		mm_pkey_free(current->mm, pkey);
		goto out;
	}
	/* 将编号作为成功值交还用户；锁仍由 out 统一释放。 */
	ret = pkey;
out:
	mmap_write_unlock(current->mm);
	return ret;
}

SYSCALL_DEFINE1(pkey_free, int, pkey)
{
	int ret;

	mmap_write_lock(current->mm);
	/* 分配和释放共用这把锁，使一个编号不会在操作中被并发重新分配。 */
	/* free 只释放编号；调用者若仍保留映射，应由自身生命周期避免语义混淆。 */
	ret = mm_pkey_free(current->mm, pkey);
	mmap_write_unlock(current->mm);

	/*
	 * 业务背景：释放的是 mm 的键编号，不会逐 VMA 扫描并重写旧 pkey；现有
	 * 映射的语义由架构和后续重新分配规则约束。
	 * 注意事项：仍在写锁下操作位图，避免与 pkey_mprotect/alloc 并发复用同一号。
	 *
	 * We could provide warnings or errors if any VMA still
	 * has the pkey set here.
	 */
	return ret;
}

#endif /* CONFIG_ARCH_HAS_PKEYS */
