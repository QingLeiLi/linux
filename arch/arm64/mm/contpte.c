// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 contiguous-PTE 折叠、展开与并发读取学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * CONT_PTE 让一组连续、同属性 PTE 提示硬件把它们缓存为更大的 TLB 项，
 * 不改变基础页表层级。收益是减少 TLB 压力；代价是整组描述符必须保持
 * PFN 连续、属性一致，任何只改子集权限的操作都要先 unfold。软件仍以
 * folio 为 access/dirty 记账单位，所以可把整组 AF/dirty 逻辑 OR 聚合。
 *
 * 写路径通常持 PTL；lockless getter 不能原子读取整组，采用“读取全部并
 * 验证 CONT/PFN/prot 一致，否则 retry”的乐观快照。fold/unfold 遵守 BBM；
 * 支持 BBML2 no-abort 时可省中间 TLBI，但最终权限修改的 TLBI 仍是提交点。
 * 内核/EFI 映射不能容忍竞争 fault，因此动态 contiguous 只用于用户 mm。
 */
/*
 * Copyright (C) 2023 ARM Ltd.
 */

#include <linux/mm.h>
#include <linux/efi.h>
#include <linux/export.h>
#include <asm/tlbflush.h>

/* 判断 mm 是否允许动态 CONT_PTE；排除 EFI 特殊 mm 与 init_mm 内核页表。 */
static inline bool mm_is_user(struct mm_struct *mm)
{
	/*
	 * Don't attempt to apply the contig bit to kernel mappings, because
	 * dynamically adding/removing the contig bit can cause page faults.
	 * These racing faults are ok for user space, since they get serialized
	 * on the PTL. But kernel mappings can't tolerate faults.
	 */
	/* 用户 fault 会在 PTL 上等待转换完成；内核 fault 可能发生在不可恢复上下文。 */
	if (unlikely(mm_is_efi(mm)))
		return false;
	return mm != &init_mm;
}

/* 把任意组内 PTE 指针向下对齐到 CONT_PTES 个表项的组首。 */
static inline pte_t *contpte_align_down(pte_t *ptep)
{
	return PTR_ALIGN_DOWN(ptep, sizeof(*ptep) * CONT_PTES);
}

/*
 * 将操作范围扩展到它触及的完整 CONT block。start/end 是字节半开区间
 * 输入输出，ptep 对应原 start，nr 是连续 present PTE 数；若首/尾表项
 * 带 CONT，则调整地址及组首指针。调用者保证同 VMA、同页表、同大 folio。
 */
static inline pte_t *contpte_align_addr_ptep(unsigned long *start,
					     unsigned long *end, pte_t *ptep,
					     unsigned int nr)
{
	/*
	 * Note: caller must ensure these nr PTEs are consecutive (present)
	 * PTEs that map consecutive pages of the same large folio within a
	 * single VMA and a single page table.
	 */
	/* 这些前置条件使“扩到整组”仍不会跨所有权、锁或 folio 记账边界。 */
	if (pte_cont(__ptep_get(ptep + nr - 1)))
		*end = ALIGN(*end, CONT_PTE_SIZE);

	if (pte_cont(__ptep_get(ptep))) {
		*start = ALIGN_DOWN(*start, CONT_PTE_SIZE);
		ptep = contpte_align_down(ptep);
	}

	return ptep;
}

/*
 * 对只覆盖组一部分的 [ptep,ptep+nr) 先展开首尾 CONT block。整组覆盖无需
 * unfold；首尾可能是同一组，helper 自身按当前 PTE 状态安全处理。mm/PTL
 * 契约由上层 set/clear API 保证。
 */
static void contpte_try_unfold_partial(struct mm_struct *mm, unsigned long addr,
					pte_t *ptep, unsigned int nr)
{
	/*
	 * Unfold any partially covered contpte block at the beginning and end
	 * of the range.
	 */
	/*
	 * CONT hint 是整组契约；局部修改若不先拆组，会留下组内属性不一致的非法
	 * descriptor。只处理首尾，因为范围中间若覆盖 CONT 块必然是整组覆盖。
	 */

	if (ptep != contpte_align_down(ptep) || nr < CONT_PTES)
		contpte_try_unfold(mm, addr, ptep, __ptep_get(ptep));

	if (ptep + nr != contpte_align_down(ptep + nr)) {
		unsigned long last_addr = addr + PAGE_SIZE * (nr - 1);
		pte_t *last_ptep = ptep + nr - 1;

		contpte_try_unfold(mm, last_addr, last_ptep,
				   __ptep_get(last_ptep));
	}
}

/*
 * 把一个完整 CONT block 重绘为 contiguous 或 non-contiguous（由 pte 的
 * CONT 位决定）。先逐项 get-and-clear，并把任意子项 AF/dirty OR 到模板；
 * 必要时 flush，再用 __set_ptes 连续写回 PFN。addr/ptep 可指组内任意项，
 * 函数会对齐。PTL 必须持有，转换期间页表项短暂为 0。
 */
static void contpte_convert(struct mm_struct *mm, unsigned long addr,
			    pte_t *ptep, pte_t pte)
{
	struct vm_area_struct vma = TLB_FLUSH_VMA(mm, 0);
	unsigned long start_addr;
	pte_t *start_ptep;
	int i;

	start_ptep = ptep = contpte_align_down(ptep);
	start_addr = addr = ALIGN_DOWN(addr, CONT_PTE_SIZE);
	pte = pfn_pte(ALIGN_DOWN(pte_pfn(pte), CONT_PTES), pte_pgprot(pte));

	for (i = 0; i < CONT_PTES; i++, ptep++, addr += PAGE_SIZE) {
		pte_t ptent = __ptep_get_and_clear(mm, addr, ptep);

		if (pte_dirty(ptent))
			/* 硬件可能把 dirty/AF 更新到任意子描述符，逻辑状态必须聚合。 */
			pte = pte_mkdirty(pte);

		if (pte_young(ptent))
			pte = pte_mkyoung(pte);
	}

	/*
	 * On eliding the __tlb_flush_range() under BBML2+noabort:
	 *
	 * NOTE: Instead of using N=16 as the contiguous block length, we use
	 *       N=4 for clarity.
	 *
	 * NOTE: 'n' and 'c' are used to denote the "contiguous bit" being
	 *       unset and set, respectively.
	 *
	 * We worry about two cases where contiguous bit is used:
	 *  - When folding N smaller non-contiguous ptes as 1 contiguous block.
	 *  - When unfolding a contiguous block into N smaller non-contiguous ptes.
	 *
	 * Currently, the BBML0 folding case looks as follows:
	 *
	 *  0) Initial page-table layout:
	 *
	 *   +----+----+----+----+
	 *   |RO,n|RO,n|RO,n|RW,n| <--- last page being set as RO
	 *   +----+----+----+----+
	 *
	 *  1) Aggregate AF + dirty flags using __ptep_get_and_clear():
	 *
	 *   +----+----+----+----+
	 *   |  0 |  0 |  0 |  0 |
	 *   +----+----+----+----+
	 *
	 *  2) __flush_tlb_range():
	 *
	 *   |____ tlbi + dsb ____|
	 *
	 *  3) __set_ptes() to repaint contiguous block:
	 *
	 *   +----+----+----+----+
	 *   |RO,c|RO,c|RO,c|RO,c|
	 *   +----+----+----+----+
	 *
	 *  4) The kernel will eventually __flush_tlb() for changed page:
	 *
	 *                  |____| <--- tlbi + dsb
	 *
	 * As expected, the intermediate tlbi+dsb ensures that other PEs
	 * only ever see an invalid (0) entry, or the new contiguous TLB entry.
	 * The final tlbi+dsb will always throw away the newly installed
	 * contiguous TLB entry, which is a micro-optimisation opportunity,
	 * but does not affect correctness.
	 *
	 * In the BBML2 case, the change is avoiding the intermediate tlbi+dsb.
	 * This means a few things, but notably other PEs will still "see" any
	 * stale cached TLB entries. This could lead to a "contiguous bit
	 * misprogramming" issue until the final tlbi+dsb of the changed page,
	 * which would clear out both the stale (RW,n) entry and the new (RO,c)
	 * contiguous entry installed in its place.
	 *
	 * What this is saying, is the following:
	 *
	 *  +----+----+----+----+
	 *  |RO,n|RO,n|RO,n|RW,n| <--- old page tables, all non-contiguous
	 *  +----+----+----+----+
	 *
	 *  +----+----+----+----+
	 *  |RO,c|RO,c|RO,c|RO,c| <--- new page tables, all contiguous
	 *  +----+----+----+----+
	 *   /\
	 *   ||
	 *
	 *  If both the old single (RW,n) and new contiguous (RO,c) TLB entries
	 *  are present, and a write is made to this address, do we fault or
	 *  is the write permitted (via amalgamation)?
	 *
	 * The relevant Arm ARM DDI 0487L.a requirements are RNGLXZ and RJQQTC,
	 * and together state that when BBML1 or BBML2 are implemented, either
	 * a TLB conflict abort is raised (which we expressly forbid), or will
	 * "produce an OA, access permissions, and memory attributes that are
	 * consistent with any of the programmed translation table values".
	 *
	 * That is to say, will either raise a TLB conflict, or produce one of
	 * the cached TLB entries, but never amalgamate.
	 *
	 * Thus, as the page tables are only considered "consistent" after
	 * the final tlbi+dsb (which evicts both the single stale (RW,n) TLB
	 * entry as well as the new contiguous (RO,c) TLB entry), omitting the
	 * initial tlbi+dsb is correct.
	 *
	 * It is also important to note that at the end of the BBML2 folding
	 * case, we are still left with potentially all N TLB entries still
	 * cached (the N-1 non-contiguous ptes, and the single contiguous
	 * block). However, over time, natural TLB pressure will cause the
	 * non-contiguous pte TLB entries to be flushed, leaving only the
	 * contiguous block TLB entry. This means that omitting the tlbi+dsb is
	 * not only correct, but also keeps our eventual performance benefits.
	 *
	 * For the unfolding case, BBML0 looks as follows:
	 *
	 *  0) Initial page-table layout:
	 *
	 *   +----+----+----+----+
	 *   |RW,c|RW,c|RW,c|RW,c| <--- last page being set as RO
	 *   +----+----+----+----+
	 *
	 *  1) Aggregate AF + dirty flags using __ptep_get_and_clear():
	 *
	 *   +----+----+----+----+
	 *   |  0 |  0 |  0 |  0 |
	 *   +----+----+----+----+
	 *
	 *  2) __flush_tlb_range():
	 *
	 *   |____ tlbi + dsb ____|
	 *
	 *  3) __set_ptes() to repaint as non-contiguous:
	 *
	 *   +----+----+----+----+
	 *   |RW,n|RW,n|RW,n|RW,n|
	 *   +----+----+----+----+
	 *
	 *  4) Update changed page permissions:
	 *
	 *   +----+----+----+----+
	 *   |RW,n|RW,n|RW,n|RO,n| <--- last page permissions set
	 *   +----+----+----+----+
	 *
	 *  5) The kernel will eventually __flush_tlb() for changed page:
	 *
	 *                  |____| <--- tlbi + dsb
	 *
	 * For BBML2, we again remove the intermediate tlbi+dsb. Here, there
	 * are no issues, as the final tlbi+dsb covering the changed page is
	 * guaranteed to remove the original large contiguous (RW,c) TLB entry,
	 * as well as the intermediate (RW,n) TLB entry; the next access will
	 * install the new (RO,n) TLB entry and the page tables are only
	 * considered "consistent" after the final tlbi+dsb, so software must
	 * be prepared for this inconsistency prior to finishing the mm dance
	 * regardless.
	 */
	/*
	 * 上述论证的结论：BBML0/1 需中间 TLBI 防止错误 contiguous 组合；
	 * BBML2+noabort 保证不会混合旧/新描述符属性，可等最终调用者 TLBI。
	 * “省略”只优化中间同步，不放宽转换完成后必须 flush 的协议。
	 */

	if (!system_supports_bbml2_noabort())
		__flush_tlb_range(&vma, start_addr, addr, PAGE_SIZE, 3,
				  TLBF_NOWALKCACHE);

	__set_ptes(mm, start_addr, start_ptep, pte, CONT_PTES);
}

/*
 * 尝试把含 ptep 的完整组 fold 为 CONT。调用者已检查 VA/PA 对齐和非 special；
 * 本函数再验证组落在单一 folio 内、每项 present、PFN 连续且除 AF/dirty
 * 外属性相同。任何条件不满足静默返回保持原映射；成功调用 convert，
 * 聚合状态并发布整组。只处理 user mm，需持 PTL。
 */
void __contpte_try_fold(struct mm_struct *mm, unsigned long addr,
			pte_t *ptep, pte_t pte)
{
	/*
	 * We have already checked that the virtual and pysical addresses are
	 * correctly aligned for a contpte mapping in contpte_try_fold() so the
	 * remaining checks are to ensure that the contpte range is fully
	 * covered by a single folio, and ensure that all the ptes are valid
	 * with contiguous PFNs and matching prots. We ignore the state of the
	 * access and dirty bits for the purpose of deciding if its a contiguous
	 * range; the folding process will generate a single contpte entry which
	 * has a single access and dirty bit. Those 2 bits are the logical OR of
	 * their respective bits in the constituent pte entries. In order to
	 * ensure the contpte range is covered by a single folio, we must
	 * recover the folio from the pfn, but special mappings don't have a
	 * folio backing them. Fortunately contpte_try_fold() already checked
	 * that the pte is not special - we never try to fold special mappings.
	 * Note we can't use vm_normal_page() for this since we don't have the
	 * vma.
	 */
	/* folio 边界验证防止把两个独立 folio 合成一个共享 AF/dirty 的硬件块。 */

	unsigned long folio_start, folio_end;
	unsigned long cont_start, cont_end;
	pte_t expected_pte, subpte;
	struct folio *folio;
	struct page *page;
	unsigned long pfn;
	pte_t *orig_ptep;
	pgprot_t prot;

	int i;

	if (!mm_is_user(mm))
		return;

	page = pte_page(pte);
	folio = page_folio(page);
	folio_start = addr - (page - &folio->page) * PAGE_SIZE;
	folio_end = folio_start + folio_nr_pages(folio) * PAGE_SIZE;
	cont_start = ALIGN_DOWN(addr, CONT_PTE_SIZE);
	cont_end = cont_start + CONT_PTE_SIZE;

	if (folio_start > cont_start || folio_end < cont_end)
		return;

	pfn = ALIGN_DOWN(pte_pfn(pte), CONT_PTES);
	prot = pte_pgprot(pte_mkold(pte_mkclean(pte)));
	/* 比较时清 AF/dirty，因为 convert 会对整组做 OR；其余属性必须逐项一致。 */
	expected_pte = pfn_pte(pfn, prot);
	orig_ptep = ptep;
	ptep = contpte_align_down(ptep);

	for (i = 0; i < CONT_PTES; i++) {
		subpte = pte_mkold(pte_mkclean(__ptep_get(ptep)));
		if (!pte_same(subpte, expected_pte))
			return;
		expected_pte = pte_advance_pfn(expected_pte, 1);
		ptep++;
	}

	pte = pte_mkcont(pte);
	contpte_convert(mm, addr, orig_ptep, pte);
}
EXPORT_SYMBOL_GPL(__contpte_try_fold);

/*
 * 把已知 contiguous 的组展开为普通 PTE。非用户 mm 空操作；成功保留 PFN、
 * 权限及聚合 AF/dirty，只清 CONT 位。调用者已通过快速 wrapper 验证 pte_cont。
 */
void __contpte_try_unfold(struct mm_struct *mm, unsigned long addr,
			pte_t *ptep, pte_t pte)
{
	/*
	 * We have already checked that the ptes are contiguous in
	 * contpte_try_unfold(), so just check that the mm is user space.
	 */
	/* 前置 wrapper 已确认 CONT 状态；这里只保留用户地址空间条件，避免重复扫描。 */
	if (!mm_is_user(mm))
		return;

	pte = pte_mknoncont(pte);
	contpte_convert(mm, addr, ptep, pte);
}
EXPORT_SYMBOL_GPL(__contpte_try_unfold);

/*
 * 持 PTL 读取 contiguous PTE 的逻辑状态。orig_pte 是目标项快照；函数扫描
 * 整组并把任意 dirty/young OR 回结果。锁保证组不会同时 unfold/refold，
 * 因而无需一致性 retry；返回仍代表目标 PFN/属性，只聚合两个状态位。
 */
pte_t contpte_ptep_get(pte_t *ptep, pte_t orig_pte)
{
	/*
	 * Gather access/dirty bits, which may be populated in any of the ptes
	 * of the contig range. We are guaranteed to be holding the PTL, so any
	 * contiguous range cannot be unfolded or otherwise modified under our
	 * feet.
	 */
	/* 两段式扫描在发现第一个状态位后只寻找另一个，减少常见路径读取次数。 */

	pte_t pte;
	int i;

	ptep = contpte_align_down(ptep);

	for (i = 0; i < CONT_PTES; i++, ptep++) {
		pte = __ptep_get(ptep);

		if (pte_dirty(pte)) {
			orig_pte = pte_mkdirty(orig_pte);
			for (; i < CONT_PTES; i++, ptep++) {
				pte = __ptep_get(ptep);
				if (pte_young(pte)) {
					orig_pte = pte_mkyoung(orig_pte);
					break;
				}
			}
			break;
		}

		if (pte_young(pte)) {
			orig_pte = pte_mkyoung(orig_pte);
			i++;
			ptep++;
			for (; i < CONT_PTES; i++, ptep++) {
				pte = __ptep_get(ptep);
				if (pte_dirty(pte)) {
					orig_pte = pte_mkdirty(orig_pte);
					break;
				}
			}
			break;
		}
	}

	return orig_pte;
}
EXPORT_SYMBOL_GPL(contpte_ptep_get);

/* 验证一项仍是预期 CONT、PFN 与忽略 AF/dirty 后的原始权限。 */
static inline bool contpte_is_consistent(pte_t pte, unsigned long pfn,
					pgprot_t orig_prot)
{
	pgprot_t prot = pte_pgprot(pte_mkold(pte_mkclean(pte)));

	return pte_valid_cont(pte) && pte_pfn(pte) == pfn &&
			pgprot_val(prot) == pgprot_val(orig_prot);
}

/*
 * 无 PTL 获取自洽目标 PTE 快照。先读 orig_ptep；若非 valid CONT，单次读取
 * 已满足普通 API。若是 CONT，推导组首 PFN/prot，扫描每项并聚合 AF/dirty；
 * 任一项不匹配说明与写者竞争，goto retry 从目标项重新开始。返回时证明
 * 扫描期间观察到一套可对应同一合法组的值，但不阻止返回后立即变化。
 */
pte_t contpte_ptep_get_lockless(pte_t *orig_ptep)
{
	/*
	 * The ptep_get_lockless() API requires us to read and return *orig_ptep
	 * so that it is self-consistent, without the PTL held, so we may be
	 * racing with other threads modifying the pte. Usually a READ_ONCE()
	 * would suffice, but for the contpte case, we also need to gather the
	 * access and dirty bits from across all ptes in the contiguous block,
	 * and we can't read all of those neighbouring ptes atomically, so any
	 * contiguous range may be unfolded/modified/refolded under our feet.
	 * Therefore we ensure we read a _consistent_ contpte range by checking
	 * that all ptes in the range are valid and have CONT_PTE set, that all
	 * pfns are contiguous and that all pgprots are the same (ignoring
	 * access/dirty). If we find a pte that is not consistent, then we must
	 * be racing with an update so start again. If the target pte does not
	 * have CONT_PTE set then that is considered consistent on its own
	 * because it is not part of a contpte range.
	 */
	/* 不能只 READ_ONCE 目标项：硬件状态可能在兄弟项，且转换写者逐项更新。 */

	pgprot_t orig_prot;
	unsigned long pfn;
	pte_t orig_pte;
	pte_t *ptep;
	pte_t pte;
	int i;

retry:
	/* retry 是乐观并发协议，不获取 PTL；持续写竞争下允许多次重试。 */
	orig_pte = __ptep_get(orig_ptep);

	if (!pte_valid_cont(orig_pte))
		return orig_pte;

	orig_prot = pte_pgprot(pte_mkold(pte_mkclean(orig_pte)));
	ptep = contpte_align_down(orig_ptep);
	pfn = pte_pfn(orig_pte) - (orig_ptep - ptep);

	for (i = 0; i < CONT_PTES; i++, ptep++, pfn++) {
		pte = __ptep_get(ptep);

		if (!contpte_is_consistent(pte, pfn, orig_prot))
			goto retry;

		if (pte_dirty(pte)) {
			orig_pte = pte_mkdirty(orig_pte);
			for (; i < CONT_PTES; i++, ptep++, pfn++) {
				pte = __ptep_get(ptep);

				if (!contpte_is_consistent(pte, pfn, orig_prot))
					goto retry;

				if (pte_young(pte)) {
					orig_pte = pte_mkyoung(orig_pte);
					break;
				}
			}
			break;
		}

		if (pte_young(pte)) {
			orig_pte = pte_mkyoung(orig_pte);
			i++;
			ptep++;
			pfn++;
			for (; i < CONT_PTES; i++, ptep++, pfn++) {
				pte = __ptep_get(ptep);

				if (!contpte_is_consistent(pte, pfn, orig_prot))
					goto retry;

				if (pte_dirty(pte)) {
					orig_pte = pte_mkdirty(orig_pte);
					break;
				}
			}
			break;
		}
	}

	return orig_pte;
}
EXPORT_SYMBOL_GPL(contpte_ptep_get_lockless);

/*
 * 批量安装 nr>=2 个连续 PTE。set_ptes 契约保证目标初始均 not-present，
 * 因而不需 unfold/BBM。用户 mm 按 CONT_PTE_SIZE 边界切段：VA、末端和
 * 起始 PFN 都整组对齐才置 CONT，否则写普通 PTE；内核/EFI 直接走底层。
 * pte 提供首 PFN和统一 prot，函数逐段推进 PFN。
 */
void contpte_set_ptes(struct mm_struct *mm, unsigned long addr,
					pte_t *ptep, pte_t pte, unsigned int nr)
{
	unsigned long next;
	unsigned long end;
	unsigned long pfn;
	pgprot_t prot;

	/*
	 * The set_ptes() spec guarantees that when nr > 1, the initial state of
	 * all ptes is not-present. Therefore we never need to unfold or
	 * otherwise invalidate a range before we set the new ptes.
	 * contpte_set_ptes() should never be called for nr < 2.
	 */
	/* nr==1 是调用层选择错误，告警但后续代码仍能安全写一项 non-cont。 */
	VM_WARN_ON(nr == 1);

	if (!mm_is_user(mm))
		return __set_ptes(mm, addr, ptep, pte, nr);

	end = addr + (nr << PAGE_SHIFT);
	pfn = pte_pfn(pte);
	prot = pte_pgprot(pte);

	do {
		next = pte_cont_addr_end(addr, end);
		nr = (next - addr) >> PAGE_SHIFT;
		pte = pfn_pte(pfn, prot);

		if (((addr | next | (pfn << PAGE_SHIFT)) & ~CONT_PTE_MASK) == 0)
			pte = pte_mkcont(pte);
		else
			pte = pte_mknoncont(pte);

		__set_ptes(mm, addr, ptep, pte, nr);

		addr = next;
		ptep += nr;
		pfn += nr;

	} while (addr != end);
}
EXPORT_SYMBOL_GPL(contpte_set_ptes);

/* 清除 nr 项前展开任何首尾部分组，再委托 full-PTES helper；full 原样透传。 */
void contpte_clear_full_ptes(struct mm_struct *mm, unsigned long addr,
				pte_t *ptep, unsigned int nr, int full)
{
	contpte_try_unfold_partial(mm, addr, ptep, nr);
	__clear_full_ptes(mm, addr, ptep, nr, full);
}
EXPORT_SYMBOL_GPL(contpte_clear_full_ptes);

/* 与上函数相同但返回底层 get-and-clear 聚合结果，部分组同样先 unfold。 */
pte_t contpte_get_and_clear_full_ptes(struct mm_struct *mm,
				unsigned long addr, pte_t *ptep,
				unsigned int nr, int full)
{
	contpte_try_unfold_partial(mm, addr, ptep, nr);
	return __get_and_clear_full_ptes(mm, addr, ptep, nr, full);
}
EXPORT_SYMBOL_GPL(contpte_get_and_clear_full_ptes);

/*
 * 测试并清除连续 present PTE 的 young。vma 提供 mm/TLB 上下文；范围若
 * 触及 CONT block 则扩为整组，不 unfold，因为 core MM 按单一 folio
 * 记 access。返回是否任一项原先 young，不在此执行 TLB flush。
 */
bool contpte_test_and_clear_young_ptes(struct vm_area_struct *vma,
		unsigned long addr, pte_t *ptep, unsigned int nr)
{
	/*
	 * ptep_clear_flush_young() technically requires us to clear the access
	 * flag for a _single_ pte. However, the core-mm code actually tracks
	 * access/dirty per folio, not per page. And since we only create a
	 * contig range when the range is covered by a single folio, we can get
	 * away with clearing young for the whole contig range here, so we avoid
	 * having to unfold.
	 *
	 * The 'nr' means consecutive (present) PTEs that map consecutive pages
	 * of the same large folio in a single VMA and a single page table.
	 */
	/* folio/VMA 前置条件保证扩大范围不会清掉另一个对象的访问历史。 */

	unsigned long end = addr + nr * PAGE_SIZE;
	bool young = false;

	ptep = contpte_align_addr_ptep(&addr, &end, ptep, nr);
	for (; addr != end; ptep++, addr += PAGE_SIZE)
		young |= __ptep_test_and_clear_young(vma, addr, ptep);

	return young;
}
EXPORT_SYMBOL_GPL(contpte_test_and_clear_young_ptes);

/* 清 young 后若有变化，flush 扩展后的完整 CONT 范围；返回原 young 聚合值。 */
bool contpte_clear_flush_young_ptes(struct vm_area_struct *vma,
		unsigned long addr, pte_t *ptep, unsigned int nr)
{
	bool young;

	young = contpte_test_and_clear_young_ptes(vma, addr, ptep, nr);

	if (young) {
		unsigned long end = addr + nr * PAGE_SIZE;

		contpte_align_addr_ptep(&addr, &end, ptep, nr);
		/*
		 * See comment in __ptep_clear_flush_young(); same rationale for
		 * eliding the trailing DSB applies here.
		 */
		/* NOSYNC 把尾部 DSB 留给上层批量序列；NOWALKCACHE 表示无需清 walk cache。 */
		__flush_tlb_range(vma, addr, end, PAGE_SIZE, 3,
				  TLBF_NOWALKCACHE | TLBF_NOSYNC);
	}

	return young;
}
EXPORT_SYMBOL_GPL(contpte_clear_flush_young_ptes);

/*
 * 写保护 nr 项。完整 CONT 组可原位逐项设 RO 并等 mmu_gather 最终 flush；
 * 部分组必须先 unfold，否则同一 contiguous 翻译内权限不一致会不可预测。
 */
void contpte_wrprotect_ptes(struct mm_struct *mm, unsigned long addr,
					pte_t *ptep, unsigned int nr)
{
	/*
	 * If wrprotecting an entire contig range, we can avoid unfolding. Just
	 * set wrprotect and wait for the later mmu_gather flush to invalidate
	 * the tlb. Until the flush, the page may or may not be wrprotected.
	 * After the flush, it is guaranteed wrprotected. If it's a partial
	 * range though, we must unfold, because we can't have a case where
	 * CONT_PTE is set but wrprotect applies to a subset of the PTEs; this
	 * would cause it to continue to be unpredictable after the flush.
	 */
	/* flush 前硬件可能暂见旧可写权限，调用者的 mmu_gather 协议必须容忍。 */

	contpte_try_unfold_partial(mm, addr, ptep, nr);
	__wrprotect_ptes(mm, addr, ptep, nr);
}
EXPORT_SYMBOL_GPL(contpte_wrprotect_ptes);

/*
 * 按 flags 清 young/dirty。范围可扩到完整 CONT block而不 unfold，因为
 * 架构允许独立更新这些状态且 core 按 folio 记账；vma/folio 前置条件同上。
 */
void contpte_clear_young_dirty_ptes(struct vm_area_struct *vma,
				    unsigned long addr, pte_t *ptep,
				    unsigned int nr, cydp_t flags)
{
	/*
	 * We can safely clear access/dirty without needing to unfold from
	 * the architectures perspective, even when contpte is set. If the
	 * range starts or ends midway through a contpte block, we can just
	 * expand to include the full contpte block. While this is not
	 * exactly what the core-mm asked for, it tracks access/dirty per
	 * folio, not per page. And since we only create a contpte block
	 * when it is covered by a single folio, we can get away with
	 * clearing access/dirty for the whole block.
	 */
	/* start/end 是扩展前后字节边界，最终页数由差值重新计算。 */
	unsigned long start = addr;
	unsigned long end = start + nr * PAGE_SIZE;

	ptep = contpte_align_addr_ptep(&start, &end, ptep, nr);
	__clear_young_dirty_ptes(vma, start, ptep, (end - start) / PAGE_SIZE, flags);
}
EXPORT_SYMBOL_GPL(contpte_clear_young_dirty_ptes);

/*
 * 检查整组每个子 PTE 的 AF/dirty/write 是否都与请求 entry 一致。PFN 本来
 * 就逐项不同，其他不由 set_access_flags 消费的属性不参与本 helper 比较。
 */
static bool contpte_all_subptes_match_access_flags(pte_t *ptep, pte_t entry)
{
	pte_t *cont_ptep = contpte_align_down(ptep);
	/*
	 * PFNs differ per sub-PTE. Match only bits consumed by
	 * __ptep_set_access_flags(): AF, DIRTY and write permission.
	 */
	/* raw per-PTE 比较避免聚合 getter 把“兄弟已更新”误当“目标已更新”。 */
	const pteval_t cmp_mask = PTE_RDONLY | PTE_AF | PTE_WRITE | PTE_DIRTY;
	pteval_t entry_cmp = pte_val(entry) & cmp_mask;
	int i;

	for (i = 0; i < CONT_PTES; i++) {
		pteval_t pte_cmp = pte_val(__ptep_get(cont_ptep + i)) & cmp_mask;

		if (pte_cmp != entry_cmp)
			return false;
	}

	return true;
}

/*
 * 更新 fault 路径的 access/dirty/write 权限。若整组 raw 位已匹配返回 0；
 * 仅 AF/dirty 变化可保持 CONT 并更新所有子项，最后按 dirty 参数整组 flush；
 * write 权限变化必须先 unfold，再只更新目标 PTE。返回 1 表示做了修改。
 * vma/PTL/目标 present 的前置条件由通用 ptep_set_access_flags 路径保证。
 */
int contpte_ptep_set_access_flags(struct vm_area_struct *vma,
					unsigned long addr, pte_t *ptep,
					pte_t entry, int dirty)
{
	unsigned long start_addr;
	pte_t orig_pte;
	int i;

	/*
	 * Check whether all sub-PTEs in the CONT block already match the
	 * requested access flags/write permission, using raw per-PTE values
	 * rather than the gathered ptep_get() view.
	 *
	 * __ptep_set_access_flags() can update AF, dirty and write
	 * permission, but only to make the mapping more permissive.
	 *
	 * ptep_get() gathers AF/dirty state across the whole CONT block,
	 * which is correct for a CPU with FEAT_HAFDBS. But page-table
	 * walkers that evaluate each descriptor individually (e.g. a CPU
	 * without DBM support, or an SMMU without HTTU, or with HA/HD
	 * disabled in CD.TCR) can keep faulting on the target sub-PTE if
	 * only a sibling has been updated. Gathering can therefore cause
	 * false no-ops when only a sibling has been updated:
	 *  - write faults: target still has PTE_RDONLY (needs PTE_RDONLY cleared)
	 *  - read faults:  target still lacks PTE_AF
	 *
	 * Per Arm ARM (DDI 0487) D8.7.1, any sub-PTE in a CONT range may
	 * become the effective cached translation, so all entries must have
	 * consistent attributes. Check the full CONT block before returning
	 * no-op, and when any sub-PTE mismatches, proceed to update the whole
	 * range.
	 */
	/* 关键兼容对象包括无 DBM CPU 与 SMMU walker，它们可能逐 descriptor 观察。 */
	if (contpte_all_subptes_match_access_flags(ptep, entry))
		return 0;

	/*
	 * Use raw target pte (not gathered) for write-bit unfold decision.
	 */
	/* 清 CONT 后只比较目标真实 write 位，不能用整组聚合状态替代权限。 */
	orig_pte = pte_mknoncont(__ptep_get(ptep));

	/*
	 * We can fix up access/dirty bits without having to unfold the contig
	 * range. But if the write bit is changing, we must unfold.
	 */
	/* AF/dirty 可按整组聚合更新而保持同构；write 权限变化涉及 BBM，必须先解组。 */
	if (pte_write(orig_pte) == pte_write(entry)) {
		/*
		 * For HW access management, we technically only need to update
		 * the flag on a single pte in the range. But for SW access
		 * management, we need to update all the ptes to prevent extra
		 * faults. Avoid per-page tlb flush in __ptep_set_access_flags()
		 * and instead flush the whole range at the end.
		 */
		/* dirty=0 的调用可依赖更外层 flush；dirty=1 在此完成整组失效。 */
		ptep = contpte_align_down(ptep);
		start_addr = addr = ALIGN_DOWN(addr, CONT_PTE_SIZE);

		/*
		 * We are not advancing entry because __ptep_set_access_flags()
		 * only consumes access flags from entry. And since we have checked
		 * for the whole contpte block and returned early, pte_same()
		 * within __ptep_set_access_flags() is likely false.
		 */
		/* entry PFN 不推进是安全的，因为底层 helper只读取 AF/dirty/write 位。 */
		for (i = 0; i < CONT_PTES; i++, ptep++, addr += PAGE_SIZE)
			__ptep_set_access_flags(vma, addr, ptep, entry, 0);

		if (dirty)
			__flush_tlb_range(vma, start_addr,
					  start_addr + CONT_PTE_SIZE,
					  PAGE_SIZE, 3,
					  TLBF_NOWALKCACHE | TLBF_NOBROADCAST);
	} else {
		__contpte_try_unfold(vma->vm_mm, addr, ptep, orig_pte);
		__ptep_set_access_flags(vma, addr, ptep, entry, dirty);
	}

	return 1;
}
EXPORT_SYMBOL_GPL(contpte_ptep_set_access_flags);
