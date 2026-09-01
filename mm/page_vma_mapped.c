// SPDX-License-Identifier: GPL-2.0
/*
 * 本文件实现反向映射路径的“给定 PFN 范围与 VMA，逐个找出实际页表映射”迭代器。
 * 它沿 PGD/P4D/PUD/PMD/PTE 下降，同时处理 THP、HugeTLB、迁移项和设备私有内存；
 * 命中时把相应页表锁留给调用者，继续或结束时再由统一 helper 解锁并解除 PTE 映射。
 */
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/hugetlb.h>
#include <linux/swap.h>
#include <linux/leafops.h>
/* rmap 定义迭代状态与 flags，其余头提供 HugeTLB、softleaf 和页表层级判定。 */

#include "internal.h"

/*
 * not_found() - 统一完成一次无更多命中的迭代退出
 * 业务背景：walker 多个分支在确认当前 VMA 不再映射目标 PFN 后都需要相同清理。
 * 入参：pvmw 是输入输出迭代状态，可能持有 PTE 映射和 PMD/PTE/HugeTLB 锁。
 * 出参/返回：调用 page_vma_mapped_walk_done() 解除映射/锁并返回 false；不释放 VMA 或页。
 * 注意事项：只用于终止整个当前 VMA 搜索；调用后 pvmw 的 pte/ptl 值不可继续解引用。
 */
static inline bool not_found(struct page_vma_mapped_walk *pvmw)
{
	page_vma_mapped_walk_done(pvmw);
	return false;
}

/*
 * map_pte() - 为当前地址映射 PTE 页，并按 flags 选择立即或延迟取得 PTL
 *
 * 业务背景：page_vma_mapped_walk() 在确认 PMD 不是可直接返回的大页后调用这里；同步模式
 * 必须从一开始持锁，普通 rmap 快路径先 lockless 排除无关项，命中候选后再锁并复核 PMD。
 * 入参：pvmw 是输入输出 walker，vma/pmd/address/flags 已设置；pmdvalp 保存本轮 PMD
 * 快照并供锁后校验；ptlp 是输出参数，接收与所映射 PTE 页严格对应的锁指针。
 * 出参/返回：成功建立可用 PTE 映射返回 true，并在 pvmw->ptl 中留下已持锁 PTL；无 PTE、
 * 空/语义不符条目返回 false，此时 pvmw->pte 可能非 NULL 供上层继续扫描，ptlp 仍可用。
 * 注意事项：PVMW_MIGRATION 只接受迁移项；普通模式接受 present 与 device private/exclusive
 * softleaf。锁后若 PMD 已变化，先 unmap/unlock 再重试，防止 PTE 与锁来自不同页表。
 */
static bool map_pte(struct page_vma_mapped_walk *pvmw, pmd_t *pmdvalp,
		    spinlock_t **ptlp)
{
	/* is_migration 固定本次搜索语义；ptent 是 lockless 候选快照。 */
	bool is_migration;
	pte_t ptent;

	if (pvmw->flags & PVMW_SYNC) {
		/* Use the stricter lookup */
		/* 同步调用者禁止竞态近似，直接映射并持 PTL；NULL 表示页表正在变化。 */
		pvmw->pte = pte_offset_map_lock(pvmw->vma->vm_mm, pvmw->pmd,
						pvmw->address, &pvmw->ptl);
		*ptlp = pvmw->ptl;
		return !!pvmw->pte;
	}

	is_migration = pvmw->flags & PVMW_MIGRATION;
again:
	/*
	 * It is important to return the ptl corresponding to pte,
	 * in case *pvmw->pmd changes underneath us; so we need to
	 * return it even when choosing not to lock, in case caller
	 * proceeds to loop over next ptes, and finds a match later.
	 * Though, in most cases, page lock already protects this.
	 */
	/*
	 * 即使暂不加锁，也必须返回与 pte 映射对应的 ptl；PMD 可能并发变化，而调用者会
	 * 在后续 PTE 才命中。多数 rmap 调用者另持 folio 锁，但不能以此替代锁对应关系。
	 */
	pvmw->pte = pte_offset_map_rw_nolock(pvmw->vma->vm_mm, pvmw->pmd,
					     pvmw->address, pmdvalp, ptlp);
	if (!pvmw->pte)
		return false;

	/* 先无锁读取以便快速排除空项和搜索模式不匹配的候选。 */
	ptent = ptep_get_lockless(pvmw->pte);

	if (pte_none(ptent)) {
		return false;
	} else if (pte_present(ptent)) {
		/* 迁移搜索不能把仍 present 的普通映射当作命中。 */
		if (is_migration)
			return false;
	} else if (!is_migration) {
		softleaf_t entry;

		/*
		 * Handle un-addressable ZONE_DEVICE memory.
		 *
		 * We get here when we are trying to unmap a private
		 * device page from the process address space. Such
		 * page is not CPU accessible and thus is mapped as
		 * a special swap entry, nonetheless it still does
		 * count as a valid regular mapping for the page
		 * (and is accounted as such in page maps count).
		 *
		 * So handle this special case as if it was a normal
		 * page mapping ie lock CPU page table and return true.
		 *
		 * For more details on device private memory see HMM
		 * (include/linux/hmm.h or mm/hmm.c).
		 */
		/*
		 * 不可由 CPU 寻址的 ZONE_DEVICE private/exclusive 页以特殊 swap 项编码，
		 * 但仍计入普通 mapcount。普通映射搜索须像 present PTE 一样锁住并验证它；
		 * 其他 swap/migration 项不代表当前普通映射。HMM 提供这类设备内存协议。
		 */
		entry = softleaf_from_pte(ptent);
		if (!softleaf_is_device_private(entry) &&
		    !softleaf_is_device_exclusive(entry))
			return false;
	}
	/* 候选语义正确后才取 PTL，缩短无关 PTE 的锁持有时间。 */
	spin_lock(*ptlp);
	/* 锁并不阻止上层 PMD 在加锁前已改变；复核失败必须丢弃旧 PTE 窗口并重来。 */
	if (unlikely(!pmd_same(*pmdvalp, pmdp_get_lockless(pvmw->pmd)))) {
		pte_unmap_unlock(pvmw->pte, *ptlp);
		goto again;
	}
	/* 发布已持有的 PTL，成功返回后由调用者检查/修改 PTE，再负责 done/restart。 */
	pvmw->ptl = *ptlp;

	return true;
}

/**
 * check_pte - check if [pvmw->pfn, @pvmw->pfn + @pvmw->nr_pages) is
 * mapped at the @pvmw->pte
 * @pvmw: page_vma_mapped_walk struct, includes a pair pte and pfn range
 * for checking
 * @pte_nr: the number of small pages described by @pvmw->pte.
 *
 * page_vma_mapped_walk() found a place where pfn range is *potentially*
 * mapped. check_pte() has to validate this.
 *
 * pvmw->pte may point to empty PTE, swap PTE or PTE pointing to
 * arbitrary page.
 *
 * If PVMW_MIGRATION flag is set, returns true if @pvmw->pte contains migration
 * entry that points to [pvmw->pfn, @pvmw->pfn + @pvmw->nr_pages)
 *
 * If PVMW_MIGRATION flag is not set, returns true if pvmw->pte points to
 * [pvmw->pfn, @pvmw->pfn + @pvmw->nr_pages)
 *
 * Otherwise, return false.
 *
 */
/*
 * 上述 helper 验证 walker 找到的候选 PTE 实际覆盖目标 `[pfn,pfn+nr_pages)`；PTE 可为空、
 * swap 或指向任意页。PVMW_MIGRATION 模式仅匹配迁移 softleaf，普通模式匹配 present 或
 * device private/exclusive 项；其余均 false。
 */
/*
 * check_pte() - 在已持 PTL 下判断一个 PTE 覆盖范围是否与目标 PFN 范围相交
 * 业务背景：页表层级定位只能找到“可能映射”的槽，最终必须把 PTE/softleaf 解码为 PFN。
 * 入参：pvmw 借用输入输出状态，pte/ptl 有效且目标 pfn/nr_pages 已设置；pte_nr 是当前
 * 条目描述的连续基础页数，普通 PTE 为 1、HugeTLB 为 pages_per_huge_page()。
 * 出参/返回：两个闭区间有交集返回 true，否则 false；不改变 walker、页表或引用。
 * 注意事项：调用者持对应 PTL，故 ptep_get() 快照稳定。加法比较按先判断结束在目标前、
 * 再判断起点在目标后组织；输入页数必须非零并由合法页表粒度保证不溢出。
 */
static bool check_pte(struct page_vma_mapped_walk *pvmw, unsigned long pte_nr)
{
	/* pfn 接收条目起始 PFN；ptent 是锁保护下的当前值。 */
	unsigned long pfn;
	pte_t ptent = ptep_get(pvmw->pte);

	if (pvmw->flags & PVMW_MIGRATION) {
		/* 迁移模式拒绝普通 present/device 项，只从 migration entry 恢复原 PFN。 */
		const softleaf_t entry = softleaf_from_pte(ptent);

		if (!softleaf_is_migration(entry))
			return false;

		pfn = softleaf_to_pfn(entry);
	} else if (pte_present(ptent)) {
		/* 普通 present PTE 直接携带映射 PFN。 */
		pfn = pte_pfn(ptent);
	} else {
		/* 普通模式下唯一计作映射的非 present 项是 device private/exclusive。 */
		const softleaf_t entry = softleaf_from_pte(ptent);

		/* Handle un-addressable ZONE_DEVICE memory */
		/* 设备私有页不可由 CPU 寻址，却仍属于该 VMA 的有效 rmap/mapcount 映射。 */
		if (!softleaf_is_device_private(entry) &&
		    !softleaf_is_device_exclusive(entry))
			return false;

		pfn = softleaf_to_pfn(entry);
	}

	/* 条目末 PFN 严格小于目标起点，两个闭区间不相交。 */
	if ((pfn + pte_nr - 1) < pvmw->pfn)
		return false;
	if (pfn > (pvmw->pfn + pvmw->nr_pages - 1))
		return false;
	return true;
}

/* Returns true if the two ranges overlap.  Careful to not overflow. */
/* 两个 PFN 闭区间相交时返回 true；比较顺序避免把范围合并成易溢出的单个算式。 */
/*
 * check_pmd() - 判断一个 PMD 大页 PFN 区间是否覆盖目标 PFN 范围的任一页
 * 业务背景：THP present 与 PMD migration entry 都以 HPAGE_PMD_NR 为粒度匹配。
 * 入参：pfn 是 PMD 映射起始 PFN；pvmw 借用目标 pfn/nr_pages。
 * 出参/返回：两个闭区间重叠返回 true，否则 false；无状态、引用或锁变化。
 * 注意事项：调用者已持 PMD 锁；nr_pages 必须非零，合法 PFN 范围保证端点算术有效。
 */
static bool check_pmd(unsigned long pfn, struct page_vma_mapped_walk *pvmw)
{
	/* 先排除 PMD 末端位于目标前，再排除 PMD 起点位于目标后。 */
	if ((pfn + HPAGE_PMD_NR - 1) < pvmw->pfn)
		return false;
	if (pfn > pvmw->pfn + pvmw->nr_pages - 1)
		return false;
	return true;
}

/*
 * step_forward() - 跳到当前缺失页表层级覆盖区之后的下一对齐边界
 * 业务背景：PGD/P4D/PUD/PMD 不 present 时无需逐页扫描，walker 按层级 size 快进。
 * 入参：pvmw 是输入输出游标；size 是 2 的幂页表覆盖字节数。
 * 出参/返回：无直接返回；address 变为严格下一个 size 边界，溢出为 0 时改成 ULONG_MAX。
 * 注意事项：不触碰 PTE 映射/锁；调用者随后与 end 比较，ULONG_MAX 作为终止哨兵。
 */
static void step_forward(struct page_vma_mapped_walk *pvmw, unsigned long size)
{
	/* 加 size 后向下掩码等价于向上取“下一”边界，而非停留在当前边界。 */
	pvmw->address = (pvmw->address + size) & ~(size - 1);
	if (!pvmw->address)
		pvmw->address = ULONG_MAX;
}

/**
 * page_vma_mapped_walk - check if @pvmw->pfn is mapped in @pvmw->vma at
 * @pvmw->address
 * @pvmw: pointer to struct page_vma_mapped_walk. page, vma, address and flags
 * must be set. pmd, pte and ptl must be NULL.
 *
 * Returns true if the page is mapped in the vma. @pvmw->pmd and @pvmw->pte point
 * to relevant page table entries. @pvmw->ptl is locked. @pvmw->address is
 * adjusted if needed (for PTE-mapped THPs).
 *
 * If @pvmw->pmd is set but @pvmw->pte is not, you have found PMD-mapped page
 * (usually THP). For PTE-mapped THP, you should run page_vma_mapped_walk() in
 * a loop to find all PTEs that map the THP.
 *
 * For HugeTLB pages, @pvmw->pte is set to the relevant page table entry
 * regardless of which page table level the page is mapped at. @pvmw->pmd is
 * NULL.
 *
 * Returns false if there are no more page table entries for the page in
 * the vma. @pvmw->ptl is unlocked and @pvmw->pte is unmapped.
 *
 * If you need to stop the walk before page_vma_mapped_walk() returned false,
 * use page_vma_mapped_walk_done(). It will do the housekeeping.
 */
/*
 * page_vma_mapped_walk() - 迭代寻找目标 PFN 范围在一个 VMA 中的实际页表映射
 *
 * 业务背景：try_to_unmap、migrate、mkclean、idle tracking 和 memory failure 等 rmap
 * 消费者先由 anon_vma/i_mmap 找到候选 VMA，再用本函数排除陈旧或不同 PFN 的页表项。
 * 入参：pvmw 是持久输入输出状态；首次调用须设置 pfn、非零 nr_pages、pgoff、vma、起始
 * address 与 flags，并令 pmd/pte/ptl 为 NULL。PVMW_SYNC 要求严格锁定查找，
 * PVMW_MIGRATION 改为寻找迁移项；VMA 和目标页均为借用对象。
 * 出参/返回：每次命中返回 true，并留下相关 PTL 已锁；PTE 命中令 pte 非 NULL，PMD
 * 大页命中令 pmd 非 NULL/pte NULL，HugeTLB 命中令 pte 非 NULL/pmd NULL。无更多映射
 * 返回 false 并完成解锁/unmap。PTE-mapped THP 的调用者应循环取得全部命中。
 * 注意事项：命中后调用者可在锁内检查/修改条目，再次调用会从下一项继续；提前停止必须
 * page_vma_mapped_walk_done()。HugeTLB 调用者持 i_mmap_rwsem，普通路径依赖 folio/VMA
 * 生命周期和逐级页表锁；函数不取得页引用，锁内不可睡眠。
 */
bool page_vma_mapped_walk(struct page_vma_mapped_walk *pvmw)
{
	/*
	 * vma/mm 为借用对象；end 是本 PFN 范围在 VMA 的搜索上界；ptl 暂存 PTE 锁；
	 * pteval/pmde 是当前层级快照；pgd/p4d/pud 是逐级页表游标。
	 */
	struct vm_area_struct *vma = pvmw->vma;
	struct mm_struct *mm = vma->vm_mm;
	unsigned long end;
	spinlock_t *ptl;
	pte_t pteval;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t pmde;

	/* The only possible pmd mapping has been handled on last iteration */
	/* 上次 true 已返回唯一 PMD 大页映射；再次调用必须统一解锁并结束，不能重复命中。 */
	if (pvmw->pmd && !pvmw->pte)
		return not_found(pvmw);

	if (unlikely(is_vm_hugetlb_page(vma))) {
		/* hstate/size 决定该 VMA 的 HugeTLB 层级与一个条目的基础页覆盖数。 */
		struct hstate *hstate = hstate_vma(vma);
		unsigned long size = huge_page_size(hstate);
		/* The only possible mapping was handled on last iteration */
		/* HugeTLB 候选地址只有一个相关 huge PTE；已返回过便结束本次 VMA 搜索。 */
		if (pvmw->pte)
			return not_found(pvmw);
		/*
		 * All callers that get here will already hold the
		 * i_mmap_rwsem.  Therefore, no additional locks need to be
		 * taken before calling hugetlb_walk().
		 */
		/*
		 * 所有进入者已持 i_mmap_rwsem，hugetlb page-table share/unshare 生命周期稳定，
		 * 因此查表前无需额外结构锁；找到条目后仍须 huge PTL 稳定其内容。
		 */
		pvmw->pte = hugetlb_walk(vma, pvmw->address, size);
		if (!pvmw->pte)
			return false;

		/* 锁住 huge PTE 后验证它覆盖目标 PFN；不匹配也通过 not_found() 解锁。 */
		pvmw->ptl = huge_pte_lock(hstate, mm, pvmw->pte);
		if (!check_pte(pvmw, pages_per_huge_page(hstate)))
			return not_found(pvmw);
		return true;
	}

	/* 普通 VMA 的 end 限定目标 folio 页索引在本 VMA 可能出现的最后位置。 */
	end = vma_address_end(pvmw);
	/* 上次返回 PTE 命中时保持映射/锁，本次直接从 next_pte 推进而不重走上层页表。 */
	if (pvmw->pte)
		goto next_pte;
restart:
	/* 从当前 address 逐级下降；缺失的高层项按其覆盖大小整体跳过。 */
	do {
		pgd = pgd_offset(mm, pvmw->address);
		if (!pgd_present(*pgd)) {
			/* 整个 PGD 区无映射，不可能包含目标 PFN。 */
			step_forward(pvmw, PGDIR_SIZE);
			continue;
		}
		p4d = p4d_offset(pgd, pvmw->address);
		if (!p4d_present(*p4d)) {
			/* 折叠层级下 present helper 保持相同语义；否则跳过整个 P4D。 */
			step_forward(pvmw, P4D_SIZE);
			continue;
		}
		pud = pud_offset(p4d, pvmw->address);
		if (!pud_present(*pud)) {
			/* 当前实现不在此返回 PUD 大页命中，缺项按 PUD_SIZE 快进。 */
			step_forward(pvmw, PUD_SIZE);
			continue;
		}

		pvmw->pmd = pmd_offset(pud, pvmw->address);
		/*
		 * Make sure the pmd value isn't cached in a register by the
		 * compiler and used as a stale value after we've observed a
		 * subsequent update.
		 */
		/* 用明确的 lockless accessor 每次重新取 PMD，禁止编译器跨后续观察复用陈旧寄存器值。 */
		pmde = pmdp_get_lockless(pvmw->pmd);

		if (pmd_trans_huge(pmde) || pmd_is_migration_entry(pmde)) {
			/* 候选 THP/migration PMD 必须加 PMD 锁后重读，随后才可按 PFN 返回。 */
			pvmw->ptl = pmd_lock(mm, pvmw->pmd);
			pmde = *pvmw->pmd;
			if (!pmd_present(pmde)) {
				/* 非 present 候选仅在配置支持且调用者请求 migration 时有意义。 */
				softleaf_t entry;

				if (!thp_migration_supported() ||
				    !(pvmw->flags & PVMW_MIGRATION))
					return not_found(pvmw);
				entry = softleaf_from_pmd(pmde);

				/* 必须既是 migration 类型又与目标 PFN 区间相交；否则终止并解锁。 */
				if (!softleaf_is_migration(entry) ||
				    !check_pmd(softleaf_to_pfn(entry), pvmw))
					return not_found(pvmw);
				return true;
			}
			if (likely(pmd_trans_huge(pmde))) {
				/* present THP 只服务普通映射搜索，且仍需验证其 HPAGE_PMD_NR PFN 范围。 */
				if (pvmw->flags & PVMW_MIGRATION)
					return not_found(pvmw);
				if (!check_pmd(pmd_pfn(pmde), pvmw))
					return not_found(pvmw);
				return true;
			}
			/* THP pmd was split under us: handle on pte level */
			/* 加锁期间 THP 已 split 成 PTE table；释放 PMD 锁后转入 PTE 慢路径。 */
			spin_unlock(pvmw->ptl);
			pvmw->ptl = NULL;
		} else if (!pmd_present(pmde)) {
			/* 普通非 present PMD 可能是 device-private 大页，或真正缺失/正在 zap。 */
			const softleaf_t entry = softleaf_from_pmd(pmde);

			if (softleaf_is_device_private(entry)) {
				/* device-private PMD 计作有效映射；持 PMD 锁返回给 rmap 调用者。 */
				pvmw->ptl = pmd_lock(mm, pvmw->pmd);
				return true;
			}

			/*
			 * PVMW_SYNC 的大 folio 查找可能与 PMD zap 竞态：仅当 VMA/目标大小适合
			 * PMD 映射时，同步等待 zap 完成，避免把瞬态空 PMD 当作最终无映射。
			 */
			if ((pvmw->flags & PVMW_SYNC) &&
			    thp_vma_suitable_order(vma, pvmw->address,
						   PMD_ORDER) &&
			    (pvmw->nr_pages >= HPAGE_PMD_NR))
				sync_with_folio_pmd_zap(mm, pvmw->pmd);

			/* 确认本 PMD 无可匹配映射后整体跳过，继续上层循环。 */
			step_forward(pvmw, PMD_SIZE);
			continue;
		}
		/*
		 * PMD 指向 PTE table：map_pte() 可能返回锁定候选；false 且 pte==NULL
		 * 表示页表映射窗口变化，需从上层 restart；false 但 pte 非 NULL 表示
		 * 当前项无关，可沿同一 PTE table 扫描后续非空项。
		 */
		if (!map_pte(pvmw, &pmde, &ptl)) {
			if (!pvmw->pte)
				goto restart;
			goto next_pte;
		}
this_pte:
		/* 已持 PTL 后做最终 PFN 交集验证；命中把锁和 pte 留给调用者。 */
		if (check_pte(pvmw, 1))
			return true;
next_pte:
		/*
		 * 上次命中或当前候选不匹配后逐页前进，跳过 none 项；在同一 PTE table
		 * 内复用映射与可选锁，跨 PMD 边界则清理并重新下降。
		 */
		do {
			pvmw->address += PAGE_SIZE;
			/* 到达目标 PFN 在 VMA 的可能终点，统一释放当前映射/锁。 */
			if (pvmw->address >= end)
				return not_found(pvmw);
			/* Did we cross page table boundary? */
			/* 跨过 PTE table：先解 PTL、unmap PTE 窗口，并记录结果 flag 后重走层级。 */
			if ((pvmw->address & (PMD_SIZE - PAGE_SIZE)) == 0) {
				if (pvmw->ptl) {
					spin_unlock(pvmw->ptl);
					pvmw->ptl = NULL;
				}
				pte_unmap(pvmw->pte);
				pvmw->pte = NULL;
				pvmw->flags |= PVMW_PGTABLE_CROSSED;
				goto restart;
			}
			/* 同表内指针前移；无锁快路径用 lockless getter，锁定路径用普通 getter。 */
			pvmw->pte++;
			if (!pvmw->ptl)
				pteval = ptep_get_lockless(pvmw->pte);
			else
				pteval = ptep_get(pvmw->pte);
		} while (pte_none(pteval));

		if (!pvmw->ptl) {
			/*
			 * lockless 扫描找到后续非空项后才取保存的对应 PTL；随后再次校验 PMD，
			 * 防止 split/zap 在扫描与加锁之间替换整张 PTE table。
			 */
			spin_lock(ptl);
			if (unlikely(!pmd_same(pmde, pmdp_get_lockless(pvmw->pmd)))) {
				pte_unmap_unlock(pvmw->pte, ptl);
				pvmw->pte = NULL;
				goto restart;
			}
			/* 校验成功后把锁发布到 walker 状态，供 check_pte() 与调用者共同使用。 */
			pvmw->ptl = ptl;
		}
		goto this_pte;
	} while (pvmw->address < end);

	/* 高层缺项跳跃令 address 达到 end 且未留下 PTE/PTL，直接报告没有更多映射。 */
	return false;
}

#ifdef CONFIG_MEMORY_FAILURE
/**
 * page_mapped_in_vma - check whether a page is really mapped in a VMA
 * @page: the page to test
 * @vma: the VMA to test
 *
 * Return: The address the page is mapped at if the page is in the range
 * covered by the VMA and present in the page table.  If the page is
 * outside the VMA or not present, returns -EFAULT.
 * Only valid for normal file or anonymous VMAs.
 */
/*
 * 在 CONFIG_MEMORY_FAILURE 下，memory-failure 收集待 SIGBUS 的进程时用本 helper 验证
 * 一个 page 是否确实 present 于候选普通文件/匿名 VMA，并返回其用户虚拟地址；页超出
 * VMA 或页表无映射返回 -EFAULT。该接口不适用于 HugeTLB/特殊 VMA。
 */
/*
 * page_mapped_in_vma() - 同步验证单个 page 在给定普通 VMA 中的实际映射地址
 * 业务背景：hwpoison 的 rmap interval tree 只能给出候选 VMA；发送精确 SIGBUS 前需在
 * 页表锁下排除已经解除或映射到别页的条目。
 * 入参：page 是借用目标页；vma 是借用普通文件或匿名 VMA。调用者负责页/VMA 生命周期
 * 和相应 anon_vma/i_mmap 读侧保护。
 * 出参/返回：命中返回页在 VMA 的用户虚拟地址；页索引不落在 VMA 或同步 walker 无命中
 * 返回 `(unsigned long)-EFAULT`。不取得 page/VMA 引用，不改变页表。
 * 注意事项：PVMW_SYNC 强制严格 PTL 查找；命中后必须显式 done 解锁。函数可能在不同配置
 * 下不存在；返回类型为 unsigned long，调用者仍按 -EFAULT 哨兵比较。
 */
unsigned long page_mapped_in_vma(const struct page *page,
		struct vm_area_struct *vma)
{
	/* folio 用于计算大 folio 内 page 的 pgoff；pvmw 只搜索一个 PFN并启用同步模式。 */
	const struct folio *folio = page_folio(page);
	struct page_vma_mapped_walk pvmw = {
		.pfn = page_to_pfn(page),
		.nr_pages = 1,
		.vma = vma,
		.flags = PVMW_SYNC,
	};

	/* 先按 VMA 文件/匿名页索引换算唯一候选地址；不相交直接保留 -EFAULT。 */
	pvmw.address = vma_address(vma, page_pgoff(folio, page), 1);
	if (pvmw.address == -EFAULT)
		goto out;
	/* 同步 walker 若未找到实际条目，统一对外折叠为 -EFAULT。 */
	if (!page_vma_mapped_walk(&pvmw))
		return -EFAULT;
	/* 命中返回时 PTL 仍锁定，读取完地址后立即成对解除映射和锁。 */
	page_vma_mapped_walk_done(&pvmw);
out:
	/* address 要么是稳定计算出的命中 VA，要么是 vma_address() 的 -EFAULT 哨兵。 */
	return pvmw.address;
}
#endif
