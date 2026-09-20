// SPDX-License-Identifier: GPL-2.0
/*
 * Device Memory Migration functionality.
 *
 * Originally written by Jérôme Glisse.
 */
/*
 * 本文件实现 CPU 普通内存与 ZONE_DEVICE 私有/一致性内存之间的迁移协议。
 * 核心边界是先以 migration entry 阻止 CPU 继续访问并稳定源 folio，再由驱动准备目标和复制数据，
 * 最后迁移 struct page 元数据、恢复 CPU 页表并释放两端临时引用；作者行属于元数据豁免项。
 */
/* 通用头文件提供导出接口、ZONE_DEVICE、迁移、页表遍历、MMU notifier、rmap 与 softleaf 编码。 */
#include <linux/export.h>
#include <linux/memremap.h>
#include <linux/migrate.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/mmu_notifier.h>
#include <linux/oom.h>
#include <linux/pagewalk.h>
#include <linux/rmap.h>
/* leafops/pgalloc 和架构 TLB helper 支撑非 present 条目编码及页表层级分配。 */
#include <linux/leafops.h>
#include <linux/pgalloc.h>
#include <asm/tlbflush.h>
#include "internal.h"

/*
 * migrate_vma_collect_skip() - 为不可迁移区间写入空的 src/dst 槽。
 * 业务背景：页表 walk 即使跳过某段也必须保持数组索引与虚拟页地址一一对应，供后续阶段线性扫描。
 * 入参：start/end 是页对齐半开地址区间；walk->private 借用 migrate_vma；不取得对象引用。
 * 出参/返回：每页把 dst/src 置零并推进 npages，恒返回 0；cpages 不增加。
 * 注意事项：调用者持 mmap 读锁，函数不睡眠、不访问页表内容；addr 仅在本区间有效。
 */
static int migrate_vma_collect_skip(unsigned long start,
				    unsigned long end,
				    struct mm_walk *walk)
{
	/* migrate 是调用者拥有的事务描述；addr 把虚拟页序号映射到数组槽位。 */
	struct migrate_vma *migrate = walk->private;
	unsigned long addr;

	for (addr = start; addr < end; addr += PAGE_SIZE) {
		/* 零值明确表示该地址不参与本轮迁移，但仍消费一个数组位置。 */
		migrate->dst[migrate->npages] = 0;
		migrate->src[migrate->npages++] = 0;
	}

	return 0;
}

/*
 * migrate_vma_collect_hole() - 把匿名 VMA 中的空洞登记为可由设备填充的迁移候选。
 * 业务背景：设备 fault 可直接为尚未建立 CPU 映射的匿名地址分配设备内存；非匿名空洞不能如此填充。
 * 入参：start/end 为 walk 发现的空洞；depth 未使用；walk 借用 VMA 和 migrate_vma。
 * 出参/返回：返回 0；候选槽置 MIGRATE，满足 THP 条件时首页另置 COMPOUND，更新 npages/cpages。
 * 注意事项：不分配页或改页表；数组容量由 migrate_vma_setup() 调用者保证，ownership 不变。
 */
static int migrate_vma_collect_hole(unsigned long start,
				    unsigned long end,
				    __always_unused int depth,
				    struct mm_walk *walk)
{
	/* migrate 接收输出数组和计数；addr 用于逐页登记普通粒度空洞。 */
	struct migrate_vma *migrate = walk->private;
	unsigned long addr;

	/* Only allow populating anonymous memory. */
	/* 只有匿名 VMA 能按匿名 fault 语义新建 backing；文件洞必须交回文件系统而不能由设备凭空填充。 */
	if (!vma_is_anonymous(walk->vma))
		return migrate_vma_collect_skip(start, end, walk);

	/* 整个洞覆盖完整 PMD 且选择 compound 时，用一个首页槽表达大页目标请求。 */
	if (thp_migration_supported() &&
		(migrate->flags & MIGRATE_VMA_SELECT_COMPOUND) &&
		(IS_ALIGNED(start, HPAGE_PMD_SIZE) &&
		 IS_ALIGNED(end, HPAGE_PMD_SIZE))) {
		migrate->src[migrate->npages] = MIGRATE_PFN_MIGRATE |
						MIGRATE_PFN_COMPOUND;
		migrate->dst[migrate->npages] = 0;
		migrate->npages++;
		migrate->cpages++;

		/*
		 * Collect the remaining entries as holes, in case we
		 * need to split later
		 */
		/*
		 * 首页以 COMPOUND 表示按 PMD 粒度申请目标；余下槽仍登记为空洞，若驱动只给出小页目标，
		 * 后续拆分/降级路径便能恢复逐页索引，而不会丢失整个地址区间。
		 */
		return migrate_vma_collect_skip(start + PAGE_SIZE, end, walk);
	}

	/* 不满足大页能力或对齐时，每个空洞页都是独立可迁移候选。 */
	for (addr = start; addr < end; addr += PAGE_SIZE) {
		migrate->src[migrate->npages] = MIGRATE_PFN_MIGRATE;
		migrate->dst[migrate->npages] = 0;
		migrate->npages++;
		migrate->cpages++;
	}

	return 0;
}

/**
 * migrate_vma_split_folio() - Helper function to split a THP folio
 * @folio: the folio to split
 * @fault_page: struct page associated with the fault if any
 *
 * Returns 0 on success
 */
/*
 * migrate_vma_split_folio() 把 THP 拆成 base folio，并确保返回时 fault_page 所属的新 folio 仍持锁。
 * @folio 是待拆分的源 folio；@fault_page 可空，非空时其原 folio 已由 fault 路径持锁。
 * 成功返回 0；失败返回 split_folio() errno，并恢复本函数额外取得的锁/引用。
 * 若拆分改变 fault_page 的 head，本函数把锁与引用从旧 head 转移到新 head；非 fault folio 的借用
 * 会临时升级为持有引用。函数可能睡眠，调用者进入时不能持页表锁。
 */
static int migrate_vma_split_folio(struct folio *folio,
				   struct page *fault_page)
{
	/* fault_folio 是入口锁 owner；new_fault_folio 在 split 后重新解析，避免继续使用旧 compound head。 */
	int ret;
	struct folio *fault_folio = fault_page ? page_folio(fault_page) : NULL;
	struct folio *new_fault_folio = NULL;

	if (folio != fault_folio) {
		/* fault 路径已锁自己的 folio；其它 folio 需本函数显式稳定生命周期并取得 folio 锁。 */
		folio_get(folio);
		folio_lock(folio);
	}

	ret = split_folio(folio);
	if (ret) {
		/* 拆分失败保持原 folio 结构；只撤销本函数为非 fault folio 取得的资源。 */
		if (folio != fault_folio) {
			folio_unlock(folio);
			folio_put(folio);
		}
		return ret;
	}

	new_fault_folio = fault_page ? page_folio(fault_page) : NULL;

	/*
	 * Ensure the lock is held on the correct
	 * folio after the split
	 */
	/* split 可能重建 compound head，必须基于 fault_page 重新找到应由 fault 路径继续持锁的 folio。 */
	if (!new_fault_folio) {
		/* 没有 fault owner，普通调用约定要求本 helper 释放入口取得的锁和引用。 */
		folio_unlock(folio);
		folio_put(folio);
	} else if (folio != new_fault_folio) {
		/* fault_page 落到新 head 时，先稳定并锁新 head，再释放旧 folio，避免中间出现无锁窗口。 */
		if (new_fault_folio != fault_folio) {
			folio_get(new_fault_folio);
			folio_lock(new_fault_folio);
		}
		folio_unlock(folio);
		folio_put(folio);
	}

	return 0;
}

/** migrate_vma_collect_huge_pmd - collect THP pages without splitting the
 * folio for device private pages.
 * @pmdp: pointer to pmd entry
 * @start: start address of the range for migration
 * @end: end address of the range for migration
 * @walk: mm_walk callback structure
 * @fault_folio: folio associated with the fault if any
 *
 * Collect the huge pmd entry at @pmdp for migration and set the
 * MIGRATE_PFN_COMPOUND flag in the migrate src entry to indicate that
 * migration will occur at HPAGE_PMD granularity
 */
/*
 * migrate_vma_collect_huge_pmd() 在 PMD 锁下辨认 THP 或 device-private softleaf，并优先整 folio 收集。
 * @pmdp 是 start 对应槽；@start/@end 为 PMD walk 区间；@walk 借用 mm/VMA/事务；@fault_folio 可空且
 * 代表外层已锁的 fault folio。成功整页收集或空洞/跳过返回 0；-EAGAIN 要求调用者重读 PMD；
 * -ENOENT 表示已拆分，应退回 PTE walk。函数为候选 folio 取得引用，必要时还取得锁；成功后这些资源
 * 由 unmap/finalize 阶段消费。页表锁只保护条目判定和安装 migration PMD，拆分前必须释放。
 */
static int migrate_vma_collect_huge_pmd(pmd_t *pmdp, unsigned long start,
					unsigned long end, struct mm_walk *walk,
					struct folio *fault_folio)
{
	/* mm/migrate 来自 walk；ptl 保护 PMD，write 保存源映射权限，ret 驱动整页失败后的降级。 */
	struct mm_struct *mm = walk->mm;
	struct folio *folio;
	struct migrate_vma *migrate = walk->private;
	spinlock_t *ptl;
	int ret;
	unsigned long write = 0;

	ptl = pmd_lock(mm, pmdp);
	if (pmd_none(*pmdp)) {
		/* 空 PMD 不持有 folio；释放锁后按匿名空洞规则登记候选。 */
		spin_unlock(ptl);
		return migrate_vma_collect_hole(start, end, -1, walk);
	}

	if (pmd_trans_huge(*pmdp)) {
		/* 系统 THP 只有显式选择 SYSTEM 时才属于本次事务。 */
		if (!(migrate->flags & MIGRATE_VMA_SELECT_SYSTEM)) {
			spin_unlock(ptl);
			return migrate_vma_collect_skip(start, end, walk);
		}

		folio = pmd_folio(*pmdp);
		if (is_huge_zero_folio(folio)) {
			/* huge zero folio 是共享只读优化，不迁移实体页，而按可填充空洞处理。 */
			spin_unlock(ptl);
			return migrate_vma_collect_hole(start, end, -1, walk);
		}
		if (pmd_write(*pmdp))
			write = MIGRATE_PFN_WRITE;
	} else if (!pmd_present(*pmdp)) {
		/* 非 present PMD 只接受 owner 匹配的 device-private softleaf，排除 swap 与其它设备。 */
		const softleaf_t entry = softleaf_from_pmd(*pmdp);

		/* softleaf_to_folio() 只在随后类型/owner 校验通过时成为可迁移对象。 */
		folio = softleaf_to_folio(entry);

		if (!softleaf_is_device_private(entry) ||
			!(migrate->flags & MIGRATE_VMA_SELECT_DEVICE_PRIVATE) ||
			(folio->pgmap->owner != migrate->pgmap_owner)) {
			spin_unlock(ptl);
			return migrate_vma_collect_skip(start, end, walk);
		}

		if (softleaf_is_device_private_write(entry))
			write = MIGRATE_PFN_WRITE;
	} else {
		/* 普通 present 非 THP PMD 指向 PTE 表；让调用者重试并转入 PTE walk。 */
		spin_unlock(ptl);
		return -EAGAIN;
	}

	/* 引用先于 trylock，保证退出 PMD 锁前后 folio head 不会被释放。 */
	folio_get(folio);
	if (folio != fault_folio && unlikely(!folio_trylock(folio))) {
		/* trylock 避免两个迁移事务互等 folio 锁；best-effort 失败只跳过该范围。 */
		spin_unlock(ptl);
		folio_put(folio);
		return migrate_vma_collect_skip(start, end, walk);
	}

	if (thp_migration_supported() &&
		(migrate->flags & MIGRATE_VMA_SELECT_COMPOUND) &&
		(IS_ALIGNED(start, HPAGE_PMD_SIZE) &&
		 IS_ALIGNED(end, HPAGE_PMD_SIZE))) {

		/* pvmw 复用当前已持 PMD 锁，避免 migration-entry helper 再次查找或锁错槽。 */
		struct page_vma_mapped_walk pvmw = {
			.ptl = ptl,
			.address = start,
			.pmd = pmdp,
			.vma = walk->vma,
		};

		unsigned long pfn = page_to_pfn(folio_page(folio, 0));

		/* 首页槽发布 PFN、权限、MIGRATE 与 COMPOUND；cpages 以复合候选个数计数。 */
		migrate->src[migrate->npages] = migrate_pfn(pfn) | write
						| MIGRATE_PFN_MIGRATE
						| MIGRATE_PFN_COMPOUND;
		migrate->dst[migrate->npages++] = 0;
		migrate->cpages++;
		ret = set_pmd_migration_entry(&pvmw, folio_page(folio, 0));
		if (ret) {
			/* PMD 替换失败必须撤回刚发布的数组槽与计数，再降级尝试拆分。 */
			migrate->npages--;
			migrate->cpages--;
			migrate->src[migrate->npages] = 0;
			migrate->dst[migrate->npages] = 0;
			goto fallback;
		}
		migrate_vma_collect_skip(start + PAGE_SIZE, end, walk);
		/* migration PMD 已发布，引用/锁留给后续迁移事务；只释放页表锁。 */
		spin_unlock(ptl);
		return 0;
	}

fallback:
	/* split_folio() 可能睡眠且会改 compound 结构，必须先退出 PMD 临界区。 */
	spin_unlock(ptl);
	if (!folio_test_large(folio))
		goto done;
	ret = split_folio(folio);
	if (fault_folio != folio)
		folio_unlock(folio);
	folio_put(folio);
	if (ret)
		/* 无法拆分时整段按不可迁移处理；上面取得的引用和非 fault 锁已撤销。 */
		return migrate_vma_collect_skip(start, end, walk);
	if (pmd_none(pmdp_get_lockless(pmdp)))
		/* 拆分期间 PMD 可能被清成空洞；重新按 hole 语义收集。 */
		return migrate_vma_collect_hole(start, end, -1, walk);

done:
	/* -ENOENT 告诉 collect_pmd() 不要结束 walk，而应重新进入 PTE 粒度路径。 */
	return -ENOENT;
}

/*
 * migrate_vma_collect_pmd() - 逐 PTE 收集候选并尽可能原地安装 migration entry。
 * 业务背景：pagewalk 的 PMD 回调先尝试整 THP，随后把普通页、device-private 条目或匿名空洞映射到
 * src/dst 数组；提前替换单映射 PTE 可避免稍后昂贵的 rmap walk。
 * 入参：pmdp/start/end/walk 均为借用；walk->private 指向输出事务，fault_page 可代表已锁页。
 * 出参/返回：通常 0；每个虚拟页消费一个数组槽，候选增加 cpages，并为实体 folio 留下锁/引用；
 * 拆分失败的余段被置零。注意事项：持 PTE 锁时只用 trylock，睡眠式拆分前退出 lazy MMU 和页表锁；
 * 修改 present PTE 后统一刷新 TLB，最终解除映射和锁。
 */
static int migrate_vma_collect_pmd(pmd_t *pmdp,
				   unsigned long start,
				   unsigned long end,
				   struct mm_walk *walk)
{
	/* addr/ptep 同步前进；unmapped 记录真正清除的 present PTE，决定是否需要 TLB flush。 */
	struct migrate_vma *migrate = walk->private;
	struct vm_area_struct *vma = walk->vma;
	struct mm_struct *mm = vma->vm_mm;
	unsigned long addr = start, unmapped = 0;
	spinlock_t *ptl;
	struct folio *fault_folio = migrate->fault_page ?
		page_folio(migrate->fault_page) : NULL;
	pte_t *ptep;

again:
	/* PMD 可能因并发拆分/折叠变化；特殊 PMD 先交给整页 helper，-EAGAIN 从同一层重新观察。 */
	if (pmd_trans_huge(*pmdp) || !pmd_present(*pmdp)) {
		int ret = migrate_vma_collect_huge_pmd(pmdp, start, end, walk, fault_folio);

		if (ret == -EAGAIN)
			goto again;
		if (ret == 0)
			return 0;
	}

	/* 映射并锁 start 对应 PTE；锁less 变化导致映射失败时回到 PMD 分类重新判定。 */
	ptep = pte_offset_map_lock(mm, pmdp, start, &ptl);
	if (!ptep)
		goto again;
	lazy_mmu_mode_enable();
	/* walk 可能从 PMD 中部开始，按页偏移把 ptep 校准到 addr。 */
	ptep += (addr - start) / PAGE_SIZE;

	for (; addr < end; addr += PAGE_SIZE, ptep++) {
		/* mpfn 是写入 src 的协议位图；page/folio 只在当前迭代有效，entry/pte 保存原条目语义。 */
		struct dev_pagemap *pgmap;
		unsigned long mpfn = 0, pfn;
		struct folio *folio;
		struct page *page;
		softleaf_t entry;
		pte_t pte;

		pte = ptep_get(ptep);

		if (pte_none(pte)) {
			/* 匿名空洞允许设备分配 backing；其它 VMA 保持零槽。 */
			if (vma_is_anonymous(vma)) {
				mpfn = MIGRATE_PFN_MIGRATE;
				migrate->cpages++;
			}
			goto next;
		}

		if (!pte_present(pte)) {
			/*
			 * Only care about unaddressable device page special
			 * page table entry. Other special swap entries are not
			 * migratable, and we ignore regular swapped page.
			 */
			/*
			 * 非 present 条目只处理 CPU 无法直接寻址的 device-private softleaf；普通 swap、hwpoison
			 * 等特殊条目不属于该协议，保持零槽并继续。
			 */
			entry = softleaf_from_pte(pte);
			if (!softleaf_is_device_private(entry))
				goto next;

			page = softleaf_to_page(entry);
			pgmap = page_pgmap(page);
			if (!(migrate->flags &
				MIGRATE_VMA_SELECT_DEVICE_PRIVATE) ||
			    pgmap->owner != migrate->pgmap_owner)
				goto next;

			/* device-private 大 folio 必须在页表锁外拆分，之后从 PMD 入口重新建立稳定视图。 */
			folio = page_folio(page);
			if (folio_test_large(folio)) {
				int ret;

				lazy_mmu_mode_disable();
				pte_unmap_unlock(ptep, ptl);
				ret = migrate_vma_split_folio(folio,
							  migrate->fault_page);

				if (ret) {
					/* 此前若已清 present PTE，离开回调前必须让 CPU 丢弃旧翻译。 */
					if (unmapped)
						flush_tlb_range(walk->vma, start, end);

					return migrate_vma_collect_skip(addr, end, walk);
				}

				goto again;
			}

			/* 保留原 softleaf 的可写语义，后续 migration entry 和目标映射据此恢复权限。 */
			mpfn = migrate_pfn(page_to_pfn(page)) |
					MIGRATE_PFN_MIGRATE;
			if (softleaf_is_device_private_write(entry))
				mpfn |= MIGRATE_PFN_WRITE;
		} else {
			/* present 零页在选择 SYSTEM 时按空洞候选处理，不对共享零页取得引用。 */
			pfn = pte_pfn(pte);
			if (is_zero_pfn(pfn) &&
			    (migrate->flags & MIGRATE_VMA_SELECT_SYSTEM)) {
				mpfn = MIGRATE_PFN_MIGRATE;
				migrate->cpages++;
				goto next;
			}
			page = vm_normal_page(migrate->vma, addr, pte);
			/* 系统页需 SELECT_SYSTEM；device-coherent 页还要求类型选择与 pgmap owner 同时匹配。 */
			if (page && !is_zone_device_page(page) &&
			    !(migrate->flags & MIGRATE_VMA_SELECT_SYSTEM)) {
				goto next;
			} else if (page && is_device_coherent_page(page)) {
				pgmap = page_pgmap(page);

				if (!(migrate->flags &
					MIGRATE_VMA_SELECT_DEVICE_COHERENT) ||
					pgmap->owner != migrate->pgmap_owner)
					goto next;
			}
			/* 页类型/owner 过滤完成后再解析 folio，避免为不支持的特殊映射建立 ownership。 */
			folio = page ? page_folio(page) : NULL;
			if (folio && folio_test_large(folio)) {
				/* 与 device-private 大 folio 相同，退出 PTE 临界区后拆分并从头重试。 */
				int ret;

				lazy_mmu_mode_disable();
				pte_unmap_unlock(ptep, ptl);
				ret = migrate_vma_split_folio(folio,
							  migrate->fault_page);

				if (ret) {
					if (unmapped)
						flush_tlb_range(walk->vma, start, end);

					return migrate_vma_collect_skip(addr, end, walk);
				}

				/* 拆分改变了 PMD/PTE 结构，旧 ptep/ptl 已失效，只能重新获取。 */
				goto again;
			}
			mpfn = migrate_pfn(pfn) | MIGRATE_PFN_MIGRATE;
			/* 源 PTE 的 write 权限成为数组协议位，驱动无需重新解析页表。 */
			mpfn |= pte_write(pte) ? MIGRATE_PFN_WRITE : 0;
		}

		if (!page || !page->mapping) {
			/* 非零 present 条目若无法归属 struct page/mapping，不能安全迁移或建立 rmap。 */
			mpfn = 0;
			goto next;
		}

		/*
		 * By getting a reference on the folio we pin it and that blocks
		 * any kind of migration. Side effect is that it "freezes" the
		 * pte.
		 *
		 * We drop this reference after isolating the folio from the lru
		 * for non device folio (device folio are not on the lru and thus
		 * can't be dropped from it).
		 */
		/*
		 * 额外 folio 引用冻结生命周期并阻止其它迁移；普通 folio 成功从 LRU 隔离后会放掉该引用，
		 * ZONE_DEVICE 不在 LRU，引用一直保留到 finalize。
		 */
		folio = page_folio(page);
		folio_get(folio);

		/*
		 * We rely on folio_trylock() to avoid deadlock between
		 * concurrent migrations where each is waiting on the others
		 * folio lock. If we can't immediately lock the folio we fail this
		 * migration as it is only best effort anyway.
		 *
		 * If we can lock the folio it's safe to set up a migration entry
		 * now. In the common case where the folio is mapped once in a
		 * single process setting up the migration entry now is an
		 * optimisation to avoid walking the rmap later with
		 * try_to_migrate().
		 */
		/*
		 * 只 trylock 防止并发迁移各持一页互等；锁成功后可在当前 PTE 下提前安装 migration entry，
		 * 单映射常见路径因此无需稍后再做反向映射遍历。fault_folio 的锁由外层已持有。
		 */
		if (fault_folio == folio || folio_trylock(folio)) {
			bool anon_exclusive;
			pte_t swp_pte;

			flush_cache_page(vma, addr, pte_pfn(pte));
			/* anon-exclusive 在撤 PTE 前尝试把独占 rmap 转为可迁移状态，失败则原样恢复并跳过。 */
			anon_exclusive = folio_test_anon(folio) &&
					  PageAnonExclusive(page);
			if (anon_exclusive) {
				pte = ptep_clear_flush(vma, addr, ptep);

				if (folio_try_share_anon_rmap_pte(folio, page)) {
					/* 转共享 rmap 失败时原样写回 PTE，并撤锁/引用/候选以保持独占语义。 */
					set_pte_at(mm, addr, ptep, pte);
					if (fault_folio != folio)
						folio_unlock(folio);
					folio_put(folio);
					/* 槽改为非候选后从 next 发布结果，原 PTE/独占 rmap 均保持不变。 */
					mpfn = 0;
					goto next;
				}
			} else {
				pte = ptep_get_and_clear(mm, addr, ptep);
			}

			migrate->cpages++;

			/* Set the dirty flag on the folio now the pte is gone. */
			/* PTE 已不可见后把 dirty 汇入 folio，保证换条目不会丢失写回责任。 */
			if (pte_dirty(pte))
				folio_mark_dirty(folio);

			/* Setup special migration page table entry */
			/* 根据原写权限和 anon-exclusive 构造对应 migration 类型，再复制 young/dirty 软件状态。 */
			if (mpfn & MIGRATE_PFN_WRITE)
				entry = make_writable_migration_entry(
							page_to_pfn(page));
			else if (anon_exclusive)
				entry = make_readable_exclusive_migration_entry(
							page_to_pfn(page));
			else
				entry = make_readable_migration_entry(
							page_to_pfn(page));
			if (pte_present(pte)) {
				/* present PTE 与原 softleaf 的 soft-dirty/UFFD-WP 访问器不同，分别保存同一用户语义。 */
				if (pte_young(pte))
					entry = make_migration_entry_young(entry);
				if (pte_dirty(pte))
					entry = make_migration_entry_dirty(entry);
			}
			/* 通用 migration entry 转为架构 PTE 后，再复制 soft-dirty 与 UFFD-WP。 */
			swp_pte = swp_entry_to_pte(entry);
			if (pte_present(pte)) {
				if (pte_soft_dirty(pte))
					swp_pte = pte_swp_mksoft_dirty(swp_pte);
				if (pte_uffd_wp(pte))
					swp_pte = pte_swp_mkuffd_wp(swp_pte);
			} else {
				/* 源本就是 softleaf 时必须使用 swap 版本访问器读取其软件标志。 */
				if (pte_swp_soft_dirty(pte))
					swp_pte = pte_swp_mksoft_dirty(swp_pte);
				if (pte_swp_uffd_wp(pte))
					swp_pte = pte_swp_mkuffd_wp(swp_pte);
			}
			set_pte_at(mm, addr, ptep, swp_pte);

			/*
			 * This is like regular unmap: we remove the rmap and
			 * drop the folio refcount. The folio won't be freed, as
			 * we took a reference just above.
			 */
			/*
			 * 像普通 unmap 一样撤 rmap 和映射引用；前面额外 folio_get() 保证对象继续存在并由迁移事务持有。
			 */
			folio_remove_rmap_pte(folio, page, vma);
			folio_put(folio);

			if (pte_present(pte))
				unmapped++;
		} else {
			/* 竞争拿不到锁时撤销临时引用并清候选位，绝不在页表锁内等待。 */
			folio_put(folio);
			mpfn = 0;
		}

next:
		/* 无论是否候选都发布对应数组槽，保证 npages 与虚拟地址推进严格一致。 */
		migrate->dst[migrate->npages] = 0;
		migrate->src[migrate->npages++] = mpfn;
	}

	/* Only flush the TLB if we actually modified any entries */
	/* 只有 present PTE 被替换才可能残留 CPU TLB；原 softleaf/空洞不需要刷新。 */
	if (unmapped)
		flush_tlb_range(walk->vma, start, end);

	lazy_mmu_mode_disable();
	/* ptep 已在循环后越过末项，减一传回原映射范围内的地址供 unmap helper 使用。 */
	pte_unmap_unlock(ptep - 1, ptl);

	return 0;
}

/*
 * migrate_vma_walk_ops 是只读的全局 pagewalk 分派表，生命周期覆盖整个内核运行期且无需锁保护。
 * PMD 回调负责实体页/特殊条目，pte_hole 负责匿名空洞；PGWALK_RDLOCK 声明调用时由 walker 持
 * mmap 读锁，从而稳定 VMA 边界。所有回调都把 walk->private 解释为当前 migrate_vma 事务。
 */
static const struct mm_walk_ops migrate_vma_walk_ops = {
	/* walk 在 mmap 读锁下把 PMD 与洞回调统一投影到 migrate_vma 数组。 */
	.pmd_entry		= migrate_vma_collect_pmd,
	.pte_hole		= migrate_vma_collect_hole,
	.walk_lock		= PGWALK_RDLOCK,
};

/*
 * migrate_vma_collect() - collect pages over a range of virtual addresses
 * @migrate: migrate struct containing all migration information
 *
 * This will walk the CPU page table. For each virtual address backed by a
 * valid page, it updates the src array and takes a reference on the page, in
 * order to pin the page until we lock it and unmap it.
 */
/*
 * migrate_vma_collect() 在 CPU 页表范围内收集候选；每个有效页取得引用，使其在后续锁定/撤映射前
 * 不会释放。MMU notifier 以 pgmap_owner 标识发起设备，让同一 owner 可跳过无意义的自失效。
 */
/*
 * 业务背景：这是 setup 的发现阶段，把虚拟地址投影为 src/dst 槽但不保证所有候选最终可迁移。
 * 入参：migrate 为调用者拥有的事务且 VMA/mmap_lock/数组均有效；出参/返回：无，填充 npages/cpages。
 * 注意事项：notifier start/end 包围整个 pagewalk；函数留下的页引用、锁和 migration entry 必须由
 * unmap/pages/finalize 链消费，任何驱动都不能在 setup 成功后跳过 finalize。
 */
static void migrate_vma_collect(struct migrate_vma *migrate)
{
	struct mmu_notifier_range range;

	/*
	 * Note that the pgmap_owner is passed to the mmu notifier callback so
	 * that the registered device driver can skip invalidating device
	 * private page mappings that won't be migrated.
	 */
	/* owner 传入 notifier，使设备驱动可识别自身不会迁移的 private 映射并避免重复失效。 */
	mmu_notifier_range_init_owner(&range, MMU_NOTIFY_MIGRATE, 0,
		migrate->vma->vm_mm, migrate->start, migrate->end,
		migrate->pgmap_owner);
	mmu_notifier_invalidate_range_start(&range);

	/* pagewalk 在 mmap 读锁语义下执行回调，逐页稳定引用并尽可能安装 migration entry。 */
	walk_page_range(migrate->vma->vm_mm, migrate->start, migrate->end,
			&migrate_vma_walk_ops, migrate);

	mmu_notifier_invalidate_range_end(&range);
	/* pagewalk 可能因 VMA 边界提前停止，实际 end 必须收缩到已写入数组的槽数。 */
	migrate->end = migrate->start + (migrate->npages << PAGE_SHIFT);
}

/*
 * migrate_vma_check_page() - check if page is pinned or not
 * @page: struct page to check
 *
 * Pinned pages cannot be migrated. This is the same test as in
 * folio_migrate_mapping(), except that here we allow migration of a
 * ZONE_DEVICE page.
 */
/*
 * 上述检查判断 folio 是否还有无法解释的外部 pin；逻辑与 folio_migrate_mapping() 相同，但允许
 * ZONE_DEVICE 页。@page 是借用的源页，@fault_page 可空且代表 fault 路径额外持有的一次引用。
 */
/*
 * migrate_vma_check_page() - 用 refcount 与 mapcount 的差值拒绝被额外 pin 的源页。
 * 出参/返回：true 表示除已知迁移引用、mapping/private 与映射引用外没有额外持有者；false 必须恢复。
 * 注意事项：调用者已锁 folio 并撤掉可撤映射；本函数不增减引用、不睡眠，ZONE_DEVICE 多扣其基准引用。
 */
static bool migrate_vma_check_page(struct page *page, struct page *fault_page)
{
	/* folio 是 page 所属 head；extra 汇总当前路径能够证明来源的非 mapcount 引用。 */
	struct folio *folio = page_folio(page);

	/*
	 * One extra ref because caller holds an extra reference, either from
	 * folio_isolate_lru() for a regular folio, or migrate_vma_collect() for
	 * a device folio.
	 */
	/* collect 或 LRU isolate 各贡献一次，fault_page 与当前页相同还多一份 fault owner 引用。 */
	int extra = 1 + (page == fault_page);

	/* Page from ZONE_DEVICE have one extra reference */
	/* ZONE_DEVICE 页的设备内存模型自带一份基准引用，不能误判成 DMA pin。 */
	if (folio_is_zone_device(folio))
		extra++;

	/* For file back page */
	/* 文件 folio 的 mapping 及 private 数据分别贡献可解释引用。 */
	if (folio_mapping(folio))
		extra += 1 + folio_has_private(folio);

	if ((folio_ref_count(folio) - extra) > folio_mapcount(folio))
		/* 超出映射数的剩余引用可能是 GUP/DMA pin，迁移会破坏持有者看到的物理页。 */
		return false;

	return true;
}

/*
 * Unmaps pages for migration. Returns number of source pfns marked as
 * migrating.
 */
/*
 * 为迁移隔离并撤销 src_pfns 指向的 folio，返回仍保留 MIGRATE 标志的候选数量。每个实体页在 collect
 * 后已持引用/锁；普通 folio 还要从 LRU 隔离，若仍有映射则用 rmap 安装 migration entry。无法隔离、
 * 仍被映射或疑似 pinned 的页会清 MIGRATE，并在第二遍恢复原 PTE、锁和引用。
 */
/*
 * migrate_device_unmap() - 把候选集合推进到内容稳定、可供驱动复制的状态。
 * 入参：src_pfns 是可修改协议数组；npages 为槽数；fault_page 可空并由外层持锁/引用。
 * 出参/返回：返回成功撤映射或空洞候选数；失败槽清 MIGRATE/最终置零，不消费数组 ownership。
 * 注意事项：可能 drain LRU、做 rmap walk 和 TLB 操作并睡眠；成功实体 folio 保持锁定到 finalize。
 */
static unsigned long migrate_device_unmap(unsigned long *src_pfns,
					  unsigned long npages,
					  struct page *fault_page)
{
	/* restore 计数失败实体页；allow_drain 限制昂贵的全 CPU LRU drain 最多一次；unmapped 是成功数。 */
	struct folio *fault_folio = fault_page ?
		page_folio(fault_page) : NULL;
	unsigned long i, restore = 0;
	bool allow_drain = true;
	unsigned long unmapped = 0;

	/* 先把本 CPU pagevec 下放，提升随后 folio_isolate_lru() 找到页的概率。 */
	lru_add_drain();

	for (i = 0; i < npages; ) {
		/* page 可能为空洞；nr 让 compound folio 只由首页处理并跨过所有尾页槽。 */
		struct page *page = migrate_pfn_to_page(src_pfns[i]);
		struct folio *folio;
		unsigned int nr = 1;

		if (!page) {
			/* 无源页但带 MIGRATE 表示可填充空洞，已满足“无旧映射”条件。 */
			if (src_pfns[i] & MIGRATE_PFN_MIGRATE)
				unmapped++;
			goto next;
		}

		folio =	page_folio(page);
		nr = folio_nr_pages(folio);

		if (nr > 1)
			/* 即使收集阶段未标记，也按 folio 当前真实大小发布 COMPOUND。 */
			src_pfns[i] |= MIGRATE_PFN_COMPOUND;


		/* ZONE_DEVICE folios are not on LRU */
		/* 普通 folio 必须先隔离 LRU，防止 reclaim/writeback 与迁移同时改变其归属。 */
		if (!folio_is_zone_device(folio)) {
			if (!folio_test_lru(folio) && allow_drain) {
				/* Drain CPU's lru cache */
				/* 页可能仍滞留其它 CPU pagevec；全局 drain 只尝试一次以控制成本。 */
				lru_add_drain_all();
				allow_drain = false;
			}

			if (!folio_isolate_lru(folio)) {
				/* 隔离失败清候选，记入 restore；原 collect 引用/锁稍后第二遍统一归还。 */
				src_pfns[i] &= ~MIGRATE_PFN_MIGRATE;
				restore++;
				goto next;
			}

			/* Drop the reference we took in collect */
			/* LRU isolate 自己持有稳定引用，因此普通页可放掉 collect 阶段那一份。 */
			folio_put(folio);
		}

		if (folio_mapped(folio))
			/* 多重映射未在 collect 快路全部替换时，使用 rmap 为剩余 PTE 安装 migration entry。 */
			try_to_migrate(folio, 0);

		if (folio_mapped(folio) ||
		    !migrate_vma_check_page(page, fault_page)) {
			if (!folio_is_zone_device(folio)) {
				/* 失败的普通 folio 重新取得交给 putback 消费的引用并回到 LRU。 */
				folio_get(folio);
				folio_putback_lru(folio);
			}

			src_pfns[i] &= ~MIGRATE_PFN_MIGRATE;
			restore++;
			goto next;
		}

		unmapped++;
next:
		/* compound 首页一次处理整个 folio，尾页槽不重复隔离或释放。 */
		i += nr;
	}

	/* 第二遍只处理第一遍失败且需要恢复的实体页；成功页继续保持 migration 状态和锁。 */
	for (i = 0; i < npages && restore; i++) {
		struct page *page = migrate_pfn_to_page(src_pfns[i]);
		struct folio *folio;

		if (!page || (src_pfns[i] & MIGRATE_PFN_MIGRATE))
			continue;

		/* 把所有 migration PTE 恢复为原 folio，随后清数组槽并归还锁/collect 引用。 */
		folio = page_folio(page);
		remove_migration_ptes(folio, folio, 0);

		src_pfns[i] = 0;
		if (fault_folio != folio)
			/* fault_folio 的锁归 fault owner，不能由本事务解锁。 */
			folio_unlock(folio);
		folio_put(folio);
		restore--;
	}

	return unmapped;
}

/*
 * migrate_vma_unmap() - replace page mapping with special migration pte entry
 * @migrate: migrate struct containing all migration information
 *
 * Isolate pages from the LRU and replace mappings (CPU page table pte) with a
 * special migration pte entry and check if it has been pinned. Pinned pages are
 * restored because we cannot migrate them.
 *
 * This is the last step before we call the device driver callback to allocate
 * destination memory and copy contents of original page over to new page.
 */
/*
 * migrate_vma_unmap() 是 migrate_vma 版本的薄封装，把 unmap 返回的实际候选数写回 cpages。
 * @migrate 仍由驱动拥有，src/npages/fault_page 均借用；无直接返回值。成功后 cpages 页内容稳定且
 * 相关 folio 保持锁定，驱动可分配并复制目标；失败页已恢复。函数可能睡眠，之后必须调用 finalize。
 */
static void migrate_vma_unmap(struct migrate_vma *migrate)
{
	migrate->cpages = migrate_device_unmap(migrate->src, migrate->npages,
					migrate->fault_page);
}

/**
 * migrate_vma_setup() - prepare to migrate a range of memory
 * @args: contains the vma, start, and pfns arrays for the migration
 *
 * Returns: negative errno on failures, 0 when 0 or more pages were migrated
 * without an error.
 *
 * Prepare to migrate a range of memory virtual address range by collecting all
 * the pages backing each virtual address in the range, saving them inside the
 * src array.  Then lock those pages and unmap them. Once the pages are locked
 * and unmapped, check whether each page is pinned or not.  Pages that aren't
 * pinned have the MIGRATE_PFN_MIGRATE flag set (by this function) in the
 * corresponding src array entry.  Then restores any pages that are pinned, by
 * remapping and unlocking those pages.
 *
 * The caller should then allocate destination memory and copy source memory to
 * it for all those entries (ie with MIGRATE_PFN_VALID and MIGRATE_PFN_MIGRATE
 * flag set).  Once these are allocated and copied, the caller must update each
 * corresponding entry in the dst array with the pfn value of the destination
 * page and with MIGRATE_PFN_VALID. Destination pages must be locked via
 * lock_page().
 *
 * Note that the caller does not have to migrate all the pages that are marked
 * with MIGRATE_PFN_MIGRATE flag in src array unless this is a migration from
 * device memory to system memory.  If the caller cannot migrate a device page
 * back to system memory, then it must return VM_FAULT_SIGBUS, which has severe
 * consequences for the userspace process, so it must be avoided if at all
 * possible.
 *
 * For empty entries inside CPU page table (pte_none() or pmd_none() is true) we
 * do set MIGRATE_PFN_MIGRATE flag inside the corresponding source array thus
 * allowing the caller to allocate device memory for those unbacked virtual
 * addresses.  For this the caller simply has to allocate device memory and
 * properly set the destination entry like for regular migration.  Note that
 * this can still fail, and thus inside the device driver you must check if the
 * migration was successful for those entries after calling migrate_vma_pages(),
 * just like for regular migration.
 *
 * After that, the callers must call migrate_vma_pages() to go over each entry
 * in the src array that has the MIGRATE_PFN_VALID and MIGRATE_PFN_MIGRATE flag
 * set. If the corresponding entry in dst array has MIGRATE_PFN_VALID flag set,
 * then migrate_vma_pages() to migrate struct page information from the source
 * struct page to the destination struct page.  If it fails to migrate the
 * struct page information, then it clears the MIGRATE_PFN_MIGRATE flag in the
 * src array.
 *
 * At this point all successfully migrated pages have an entry in the src
 * array with MIGRATE_PFN_VALID and MIGRATE_PFN_MIGRATE flag set and the dst
 * array entry with MIGRATE_PFN_VALID flag set.
 *
 * Once migrate_vma_pages() returns the caller may inspect which pages were
 * successfully migrated, and which were not.  Successfully migrated pages will
 * have the MIGRATE_PFN_MIGRATE flag set for their src array entry.
 *
 * It is safe to update device page table after migrate_vma_pages() because
 * both destination and source page are still locked, and the mmap_lock is held
 * in read mode (hence no one can unmap the range being migrated).
 *
 * Once the caller is done cleaning up things and updating its page table (if it
 * chose to do so, this is not an obligation) it finally calls
 * migrate_vma_finalize() to update the CPU page table to point to new pages
 * for successfully migrated pages or otherwise restore the CPU page table to
 * point to the original source pages.
 */
/*
 * 上述 kernel-doc 定义三阶段驱动协议：setup 收集/锁定/撤映射，驱动为 src 中 VALID|MIGRATE 槽准备
 * 已锁目标并复制，pages 迁移 struct page 元数据，finalize 恢复 CPU 映射并释放锁/引用。匿名空洞也可
 * 被设备填充；device→system 的失败可能导致用户 fault SIGBUS，因此驱动应尽最大努力提供目标页。
 */
/*
 * migrate_vma_setup() - 校验事务范围并把可迁移页推进到驱动可复制状态。
 * 入参：args 为输入输出事务；VMA、页对齐范围、src/dst 容量、选择 flags/owner 由调用者提供；
 * fault_page 可空，非空时必须是已锁 device-private 页。数组和 VMA ownership 不转移。
 * 出参/返回：非法契约返回 -EINVAL 且不启动事务；成功返回 0，npages/cpages 与 src 标志说明逐槽结果。
 * 注意事项：调用者持 mmap 读锁并必须无条件以 pages/finalize 闭环成功 setup；本函数可能睡眠。
 */
int migrate_vma_setup(struct migrate_vma *args)
{
	/* nr_pages 使用原始半开范围计算；随后 start/end 向下页对齐并接受严格 VMA 边界校验。 */
	long nr_pages = (args->end - args->start) >> PAGE_SHIFT;

	args->start &= PAGE_MASK;
	args->end &= PAGE_MASK;
	if (!args->vma || is_vm_hugetlb_page(args->vma) ||
	    (args->vma->vm_flags & VM_SPECIAL) || vma_is_dax(args->vma))
		return -EINVAL;
	/* 范围必须非空且完整落入同一个 VMA，避免 pagewalk 越界或数组长度与地址数不符。 */
	if (nr_pages <= 0)
		return -EINVAL;
	if (args->start < args->vma->vm_start ||
	    args->start >= args->vma->vm_end)
		return -EINVAL;
	if (args->end <= args->vma->vm_start || args->end > args->vma->vm_end)
		return -EINVAL;
	if (!args->src || !args->dst)
		/* 两个数组分别承载源协议状态和驱动填入的目标 PFN，缺一无法闭环。 */
		return -EINVAL;
	if (args->fault_page && !is_device_private_page(args->fault_page))
		return -EINVAL;
	if (args->fault_page && !PageLocked(args->fault_page))
		return -EINVAL;

	/* setup 拥有本轮数组内容，先清 src 并重置计数；dst 由 collect 对访问到的槽逐项清零。 */
	memset(args->src, 0, sizeof(*args->src) * nr_pages);
	args->cpages = 0;
	args->npages = 0;

	migrate_vma_collect(args);

	/* 只有发现候选才执行昂贵的 LRU 隔离/rmap unmap；返回值成为稳定内容页数。 */
	if (args->cpages)
		migrate_vma_unmap(args);

	/*
	 * At this point pages are locked and unmapped, and thus they have
	 * stable content and can safely be copied to destination memory that
	 * is allocated by the drivers.
	 */
	/* 此刻带 MIGRATE 的实体源页已锁且 CPU 映射为 migration entry，驱动可安全读取稳定内容并复制。 */
	return 0;

}
/* 导出后驱动获得协议入口，但仍须遵守 pages/finalize 的配对责任。 */
EXPORT_SYMBOL(migrate_vma_setup);

#ifdef CONFIG_ARCH_ENABLE_THP_MIGRATION
/**
 * migrate_vma_insert_huge_pmd_page: Insert a huge folio into @migrate->vma->vm_mm
 * at @addr. folio is already allocated as a part of the migration process with
 * large page.
 *
 * @page needs to be initialized and setup after it's allocated. The code bits
 * here follow closely the code in __do_huge_pmd_anonymous_page(). This API does
 * not support THP zero pages.
 *
 * @migrate: migrate_vma arguments
 * @addr: address where the folio will be inserted
 * @page: page to be inserted at @addr
 * @src: src pfn which is being migrated
 * @pmdp: pointer to the pmd
 */
/*
 * migrate_vma_insert_huge_pmd_page() 把驱动已分配的目标 folio 作为匿名 PMD 大页发布到 CPU 页表；
 * 实现遵循 __do_huge_pmd_anonymous_page()，不支持 THP zero page。@migrate/@src 借用事务，@page 是
 * 已锁目标首页，@pmdp 是待发布槽。成功返回 0；真正的前置校验错误可返回负 errno；锁内竞争、
 * userfaultfd 或资源失败会清整组 src 的 MIGRATE 后返回 0，让 finalize 走恢复/失败协议。
 * memcg、rmap、LRU、页表页和 folio 引用只在发布路径提交；失败标签只释放已取得的临时页表/锁，
 * 目标 folio（包括已完成的 order/charge 状态）仍归驱动并在 finalize/驱动释放路径中收尾。
 */
static int migrate_vma_insert_huge_pmd_page(struct migrate_vma *migrate,
					 unsigned long addr,
					 struct page *page,
					 unsigned long *src,
					 pmd_t *pmdp)
{
	/* gfp 继承 VMA THP 策略；pgtable 为未来拆分预留，flush 标记替换 huge-zero PMD。 */
	struct vm_area_struct *vma = migrate->vma;
	gfp_t gfp = vma_thp_gfp_mask(vma);
	struct folio *folio = page_folio(page);
	int ret;
	/* csa_ret 表示 mm teardown 检查；ptl/pgtable/entry 分别承担发布锁、拆分页表和最终 PMD。 */
	vm_fault_t csa_ret;
	spinlock_t *ptl;
	pgtable_t pgtable;
	pmd_t entry;
	bool flush = false;
	unsigned long i;

	VM_WARN_ON_ONCE(!folio);

	/* 地址/VMA 必须允许 PMD order 匿名 THP，否则调用者应按 base page 处理。 */
	if (!thp_vma_suitable_order(vma, addr, HPAGE_PMD_ORDER))
		return -EINVAL;

	/* anon_vma 是随后建立匿名 rmap 的前提，失败时尚未改变目标 folio 形态。 */
	ret = anon_vma_prepare(vma);
	if (ret)
		return ret;

	/* 目标由驱动以一组页交付，此处正式建立 compound order 与可 rmap 属性。 */
	folio_set_order(folio, HPAGE_PMD_ORDER);
	folio_set_large_rmappable(folio);

	if (mem_cgroup_charge(folio, migrate->vma->vm_mm, gfp)) {
		/* memcg charge 是发布前可回滚边界；统计 fallback 并清源迁移标志。 */
		count_vm_event(THP_FAULT_FALLBACK);
		count_mthp_stat(HPAGE_PMD_ORDER, MTHP_STAT_ANON_FAULT_FALLBACK_CHARGE);
		ret = -ENOMEM;
		goto abort;
	}

	__folio_mark_uptodate(folio);

	/* 预分配 PTE 页供未来 split；必须在 PMD 锁外完成，因为分配可睡眠。 */
	pgtable = pte_alloc_one(vma->vm_mm);
	if (unlikely(!pgtable))
		goto abort;

	if (folio_is_device_private(folio)) {
		/* device-private CPU 映射使用非 present softleaf，写属性取自 VMA。 */
		swp_entry_t swp_entry;

		if (vma->vm_flags & VM_WRITE)
			swp_entry = make_writable_device_private_entry(
						page_to_pfn(page));
		else
			swp_entry = make_readable_device_private_entry(
						page_to_pfn(page));
		entry = swp_entry_to_pmd(swp_entry);
	} else {
		/* 仅普通系统页和 device-coherent 页可形成 present PMD；其它 ZONE_DEVICE 类型拒绝。 */
		if (folio_is_zone_device(folio) &&
		    !folio_is_device_coherent(folio)) {
			goto free_abort;
		}
		entry = folio_mk_pmd(folio, vma->vm_page_prot);
		if (vma->vm_flags & VM_WRITE)
			entry = pmd_mkwrite(pmd_mkdirty(entry), vma);
	}

	ptl = pmd_lock(vma->vm_mm, pmdp);
	/* 持锁后重新验证 mm 未进入不稳定 teardown，关闭与并发 exit/unmap 的窗口。 */
	csa_ret = check_stable_address_space(vma->vm_mm);
	if (csa_ret)
		goto unlock_abort;

	/*
	 * Check for userfaultfd but do not deliver the fault. Instead,
	 * just back off.
	 */
	/* 缺页由 userfaultfd 管理时不得绕过用户态协议，本轮迁移退让且不投递事件。 */
	if (userfaultfd_missing(vma))
		goto unlock_abort;

	if (is_huge_zero_pmd(*pmdp))
		/* 共享 huge zero 可被真实匿名页替换，但需失效旧映射；其它非空 PMD 均表示竞争。 */
		flush = true;
	else if (!pmd_none(*pmdp))
		goto unlock_abort;

	add_mm_counter(vma->vm_mm, MM_ANONPAGES, HPAGE_PMD_NR);
	/* 从此开始建立匿名 rmap/LRU/映射引用；仍在 PMD 锁下，尚未向 CPU 发布 entry。 */
	folio_add_new_anon_rmap(folio, vma, addr, RMAP_EXCLUSIVE);
	if (!folio_is_zone_device(folio))
		folio_add_lru_vma(folio, vma);
	folio_get(folio);

	if (flush) {
		/* 替换 huge-zero 时不需要 deposit 新 PTE 页，释放预分配并先失效旧 PMD/cache。 */
		pte_free(vma->vm_mm, pgtable);
		flush_cache_page(vma, addr, addr + HPAGE_PMD_SIZE);
		pmdp_invalidate(vma, addr, pmdp);
	} else {
		/* 空槽发布前 deposit PTE 页并增加 mm 页表计数，保证未来 split 有资源。 */
		pgtable_trans_huge_deposit(vma->vm_mm, pmdp, pgtable);
		mm_inc_nr_ptes(vma->vm_mm);
	}
	set_pmd_at(vma->vm_mm, addr, pmdp, entry);
	/* set_pmd_at() 是 CPU 可观察发布点；其前 uptodate/rmap/LRU 初始化必须全部完成。 */
	update_mmu_cache_pmd(vma, addr, pmdp);

	spin_unlock(ptl);

	/* 发布成功后记录 THP 与 memcg 事件；folio/pgtable ownership 已转入 mm。 */
	count_vm_event(THP_FAULT_ALLOC);
	count_mthp_stat(HPAGE_PMD_ORDER, MTHP_STAT_ANON_FAULT_ALLOC);
	count_memcg_event_mm(vma->vm_mm, THP_FAULT_ALLOC);

	return 0;

unlock_abort:
	/* 锁内失败尚未发布 entry，先解锁再释放预分配页表。 */
	spin_unlock(ptl);
free_abort:
	pte_free(vma->vm_mm, pgtable);
abort:
	/* 整个 PMD folio 作为一个事务失败，逐槽清 MIGRATE 让 finalize 不选目标。 */
	for (i = 0; i < HPAGE_PMD_NR; i++)
		src[i] &= ~MIGRATE_PFN_MIGRATE;
	return 0;
}

/*
 * migrate_vma_split_unmapped_folio() - 把已撤映射的复合源拆成 base folio 并展开 src 槽。
 * 业务背景：源是 THP 而驱动只提供小页目标时，元数据迁移必须降级为逐页协议。
 * 入参：migrate 借用事务；idx/addr 定位 compound 首页；folio 已锁且无普通映射。
 * 出参/返回：0 时清首页 COMPOUND 并为所有尾页复制 PFN+flags；失败返回 errno，数组保持复合语义。
 * 注意事项：额外 folio_get() 配对 split_huge_pmd_address(freeze=true) 消耗的引用；函数可能睡眠。
 */
static int migrate_vma_split_unmapped_folio(struct migrate_vma *migrate,
					    unsigned long idx, unsigned long addr,
					    struct folio *folio)
{
	/* flags 保留 PFN 低位协议标志，pfn 是 compound 首页，i 展开 HPAGE_PMD_NR 个 base 页。 */
	unsigned long i;
	unsigned long pfn;
	unsigned long flags;
	int ret = 0;

	/*
	 * take a reference, since split_huge_pmd_address() with freeze = true
	 * drops a reference at the end.
	 */
	/* freeze 拆 PMD 会放引用，先补一份确保随后 folio_split_unmapped() 仍可安全使用对象。 */
	folio_get(folio);
	split_huge_pmd_address(migrate->vma, addr, true);
	ret = folio_split_unmapped(folio, 0);
	if (ret)
		return ret;
	/* 拆分提交后首页不再是 compound；所有尾槽获得连续 PFN 与相同选择/权限标志。 */
	migrate->src[idx] &= ~MIGRATE_PFN_COMPOUND;
	flags = migrate->src[idx] & ((1UL << MIGRATE_PFN_SHIFT) - 1);
	pfn = migrate->src[idx] >> MIGRATE_PFN_SHIFT;
	for (i = 1; i < HPAGE_PMD_NR; i++)
		migrate->src[i+idx] = migrate_pfn(pfn + i) | flags;
	return ret;
}
#else /* !CONFIG_ARCH_ENABLE_THP_MIGRATION */
/*
 * 架构不支持 THP migration 时的大页插入空实现：主路径不会合法传入 COMPOUND 目标；返回 0、
 * 不读取参数、不发布映射，调用者依靠配置分支维持 base-page 协议。
 */
static int migrate_vma_insert_huge_pmd_page(struct migrate_vma *migrate,
					 unsigned long addr,
					 struct page *page,
					 unsigned long *src,
					 pmd_t *pmdp)
{
	return 0;
}

/* 架构无 THP migration 时不会拆未映射 THP；该空桩恒返回 0 且无锁、引用或数组副作用。 */
static int migrate_vma_split_unmapped_folio(struct migrate_vma *migrate,
					    unsigned long idx, unsigned long addr,
					    struct folio *folio)
{
	return 0;
}
#endif

/*
 * migrate_vma_nr_pages() - 返回当前 src 槽代表的协议页数。
 * 入参：src 是借用的数组槽；出参/返回：普通槽为 1，支持 THP migration 的 COMPOUND 为 HPAGE_PMD_NR。
 * 注意事项：不修改槽；无 THP migration 却出现 COMPOUND 属于调用者违约，WARN 后仍按 1 推进。
 */
static unsigned long migrate_vma_nr_pages(unsigned long *src)
{
	/* nr 默认单页；COMPOUND 只有配置支持时才扩展成 PMD 页数。 */
	unsigned long nr = 1;
#ifdef CONFIG_ARCH_ENABLE_THP_MIGRATION
	if (*src & MIGRATE_PFN_COMPOUND)
		nr = HPAGE_PMD_NR;
#else
	if (*src & MIGRATE_PFN_COMPOUND)
		VM_WARN_ON_ONCE(true);
#endif
	return nr;
}

/*
 * This code closely matches the code in:
 *   __handle_mm_fault()
 *     handle_pte_fault()
 *       do_anonymous_page()
 * to map in an anonymous zero page but the struct page will be a ZONE_DEVICE
 * private or coherent page.
 */
/*
 * 该实现仿照匿名缺页的 __handle_mm_fault()→handle_pte_fault()→do_anonymous_page()，但目标 struct page
 * 来自 device-private/coherent 内存。它只用于原地址为空洞、没有源 page 的迁移槽。
 */
/*
 * migrate_vma_insert_page() - 把单个驱动目标页发布为匿名 CPU 映射。
 * 入参：migrate/addr 定位 VMA；dst 指向驱动填入的目标协议槽；src 是可修改结果槽，均不转移数组所有权。
 * 出参/返回：无；成功把 src 规范化为 MIGRATE，失败清该位；目标 folio 的映射引用在成功时转入 mm。
 * 注意事项：页表层级分配和 memcg charge 可睡眠；PTE 锁内复核槽仍为空/零页并避让 userfaultfd，
 * set_pte_at() 是发布点，失败路径不负责解锁驱动持有的目标页。
 */
static void migrate_vma_insert_page(struct migrate_vma *migrate,
				    unsigned long addr,
				    unsigned long *dst,
				    unsigned long *src)
{
	/* page/folio 来自 dst；flush 表示替换共享零页；各级指针逐层定位待发布 PTE。 */
	struct page *page = migrate_pfn_to_page(*dst);
	struct folio *folio = page_folio(page);
	struct vm_area_struct *vma = migrate->vma;
	struct mm_struct *mm = vma->vm_mm;
	/* 锁与各级指针只在建表/发布阶段有效，orig_pte 保存锁内竞争复核结果。 */
	bool flush = false;
	spinlock_t *ptl;
	pte_t entry;
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	pte_t *ptep;
	pte_t orig_pte;

	/* Only allow populating anonymous memory */
	/* 与 collect_hole 对称，仅匿名 VMA 允许迁移协议直接创建 backing。 */
	if (!vma_is_anonymous(vma))
		goto abort;

	pgdp = pgd_offset(mm, addr);
	/* 逐级分配页表，任一级失败都在发布前清 src MIGRATE；已建目录由 mm 生命周期管理。 */
	p4dp = p4d_alloc(mm, pgdp, addr);
	if (!p4dp)
		goto abort;
	pudp = pud_alloc(mm, p4dp, addr);
	if (!pudp)
		goto abort;
	pmdp = pmd_alloc(mm, pudp, addr);
	if (!pmdp)
		goto abort;

	if (thp_migration_supported() && (*dst & MIGRATE_PFN_COMPOUND)) {
		/* compound 目标交给 PMD 发布 helper；其返回 0 也可能通过清 src 表示业务失败。 */
		int ret = migrate_vma_insert_huge_pmd_page(migrate, addr, page,
								src, pmdp);
		if (ret)
			goto abort;
		return;
	}

	if (!pmd_none(*pmdp)) {
		/* huge-zero 可拆为 PTE 后替换；真实 THP 或其它 leaf 属于竞争，不能覆盖。 */
		if (pmd_trans_huge(*pmdp)) {
			if (!is_huge_zero_pmd(*pmdp))
				goto abort;
			split_huge_pmd(vma, pmdp, addr);
		} else if (pmd_leaf(*pmdp))
			goto abort;
	}

	/* PTE 页、anon_vma 和 memcg charge 都必须在取得 PTE 自旋锁前准备完毕。 */
	if (pte_alloc(mm, pmdp))
		goto abort;
	if (unlikely(anon_vma_prepare(vma)))
		goto abort;
	if (mem_cgroup_charge(folio, vma->vm_mm, GFP_KERNEL))
		goto abort;

	/*
	 * The memory barrier inside __folio_mark_uptodate makes sure that
	 * preceding stores to the folio contents become visible before
	 * the set_pte_at() write.
	 */
	/* release 型 uptodate 屏障保证驱动此前写入目标内容先于 PTE 发布被其它 CPU 观察。 */
	__folio_mark_uptodate(folio);

	if (folio_is_device_private(folio)) {
		/* private 设备页用不可寻址 softleaf，权限仍来自 VMA。 */
		swp_entry_t swp_entry;

		if (vma->vm_flags & VM_WRITE)
			swp_entry = make_writable_device_private_entry(
						page_to_pfn(page));
		else
			swp_entry = make_readable_device_private_entry(
						page_to_pfn(page));
		entry = swp_entry_to_pte(swp_entry);
	} else {
		/* present ZONE_DEVICE 只允许 coherent 类型；普通页则用 mk_pte。 */
		if (folio_is_zone_device(folio) &&
		    !folio_is_device_coherent(folio)) {
			pr_warn_once("Unsupported ZONE_DEVICE page type.\n");
			goto abort;
		}
		entry = mk_pte(page, vma->vm_page_prot);
		if (vma->vm_flags & VM_WRITE)
			entry = pte_mkwrite(pte_mkdirty(entry), vma);
	}

	/* 目标条目构造完成后才取得 PTE 锁；锁内不再执行可睡眠分配。 */
	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	if (!ptep)
		goto abort;
	orig_pte = ptep_get(ptep);

	/* 锁内同时复核 mm 未 teardown，且目标槽仍是 none 或可替换的共享零页。 */
	if (check_stable_address_space(mm))
		goto unlock_abort;

	if (pte_present(orig_pte)) {
		unsigned long pfn = pte_pfn(orig_pte);

		if (!is_zero_pfn(pfn))
			goto unlock_abort;
		flush = true;
	} else if (!pte_none(orig_pte))
		/* swap、migration、UFFD 等非 present 非空条目均属于其它协议，不能覆盖。 */
		goto unlock_abort;

	/*
	 * Check for userfaultfd but do not deliver the fault. Instead,
	 * just back off.
	 */
	/* userfaultfd missing 拥有缺页决策权，本迁移只退让而不生成用户事件。 */
	if (userfaultfd_missing(vma))
		goto unlock_abort;

	inc_mm_counter(mm, MM_ANONPAGES);
	/* 在发布前建立计数、rmap、LRU 和映射引用，使读者见到 PTE 时对象已完整初始化。 */
	folio_add_new_anon_rmap(folio, vma, addr, RMAP_EXCLUSIVE);
	if (!folio_is_zone_device(folio))
		folio_add_lru_vma(folio, vma);
	folio_get(folio);

	if (flush) {
		/* 替换零页前清 cache/TLB，避免 CPU 继续使用共享只读翻译。 */
		flush_cache_page(vma, addr, pte_pfn(orig_pte));
		ptep_clear_flush(vma, addr, ptep);
	}
	set_pte_at(mm, addr, ptep, entry);
	/* PTE 写入是不可见→可见的发布边界，update_mmu_cache 完成架构收尾。 */
	update_mmu_cache(vma, addr, ptep);

	pte_unmap_unlock(ptep, ptl);
	*src = MIGRATE_PFN_MIGRATE;
	/* 无源页场景成功只需 MIGRATE 标志；没有可记录的源 PFN。 */
	return;

unlock_abort:
	pte_unmap_unlock(ptep, ptl);
abort:
	/* 所有失败统一清候选位，finalize 将释放未被采用的 dst，而不会安装它。 */
	*src &= ~MIGRATE_PFN_MIGRATE;
}

/*
 * __migrate_device_pages() - 将源 folio 的 mapping/flags 元数据提交给驱动准备的目标 folio。
 * 业务背景：数据复制由驱动负责；本阶段只决定每槽迁移是否可提交，并处理“空洞→新设备页”映射。
 * 入参：src/dst 为可修改协议数组，npages 为槽数；migrate 非空表示 VMA 协议，NULL 表示 PFN 范围协议。
 * 出参/返回：无；成功槽保留 src MIGRATE，失败槽清除它；不会解锁源/目标，统一留给 finalize。
 * 注意事项：可调用 folio_migrate_mapping() 并操作 notifier/页表；compound 源目标粒度不匹配时拆分或
 * 拒绝。notifier 只在首个空洞插页前启动，并覆盖到 migrate->end。
 */
static void __migrate_device_pages(unsigned long *src_pfns,
				unsigned long *dst_pfns, unsigned long npages,
				struct migrate_vma *migrate)
{
	/* range/notified 管理空洞插页的设备失效窗口；i/j 按 folio 粒度推进，addr 仅 VMA 协议有效。 */
	struct mmu_notifier_range range;
	unsigned long i, j;
	bool notified = false;
	unsigned long addr;

	for (i = 0; i < npages; ) {
		/* page/newpage 分别由协议槽解码；mapping 是源 folio 当前归属，nr 表示本轮跨过的 base 页数。 */
		struct page *newpage = migrate_pfn_to_page(dst_pfns[i]);
		struct page *page = migrate_pfn_to_page(src_pfns[i]);
		struct address_space *mapping;
		struct folio *newfolio, *folio;
		int r, extra_cnt = 0;
		unsigned long nr = 1;

		if (!newpage) {
			/* 驱动没有提供目标就不能提交，清 MIGRATE 让 finalize 恢复源页。 */
			src_pfns[i] &= ~MIGRATE_PFN_MIGRATE;
			goto next;
		}

		if (!page) {
			/* 无源 page 表示匿名空洞；只有 VMA 事务能把目标发布到具体虚拟地址。 */
			unsigned long addr;

			if (!(src_pfns[i] & MIGRATE_PFN_MIGRATE))
				goto next;

			/*
			 * The only time there is no vma is when called from
			 * migrate_device_coherent_folio(). However this isn't
			 * called if the page could not be unmapped.
			 */
			/* PFN-range/coherent-folio 路径没有 VMA，但不会产生 page==NULL 的可迁移槽。 */
			VM_BUG_ON(!migrate);
			addr = migrate->start + i*PAGE_SIZE;
			if (!notified) {
				/* 延迟到确实要改页表时才启动 notifier，避免纯元数据迁移产生无效回调。 */
				notified = true;

				mmu_notifier_range_init_owner(&range,
					MMU_NOTIFY_MIGRATE, 0,
					migrate->vma->vm_mm, addr, migrate->end,
					migrate->pgmap_owner);
				mmu_notifier_invalidate_range_start(&range);
			}

			if ((src_pfns[i] & MIGRATE_PFN_COMPOUND) &&
				(!(dst_pfns[i] & MIGRATE_PFN_COMPOUND))) {
				nr = migrate_vma_nr_pages(&src_pfns[i]);
				/* compound 空洞但目标不是 compound 时降级成逐页插入，并清首页复合标志。 */
				src_pfns[i] &= ~MIGRATE_PFN_COMPOUND;
			} else {
				nr = 1;
			}

			for (j = 0; j < nr && i + j < npages; j++) {
				/* 每个展开槽先标候选，insert_page() 再以保留/清除 MIGRATE 返回实际结果。 */
				src_pfns[i+j] |= MIGRATE_PFN_MIGRATE;
				migrate_vma_insert_page(migrate,
					addr + j * PAGE_SIZE,
					&dst_pfns[i+j], &src_pfns[i+j]);
			}
			goto next;
		}

		newfolio = page_folio(newpage);
		folio = page_folio(page);
		mapping = folio_mapping(folio);

		/*
		 * If THP migration is enabled, check if both src and dst
		 * can migrate large pages
		 */
		/* THP 协议要求源/目标 compound 粒度匹配；仅源大时可在 VMA 事务中拆成 base folio。 */
		if (thp_migration_supported()) {
			if ((src_pfns[i] & MIGRATE_PFN_MIGRATE) &&
				(src_pfns[i] & MIGRATE_PFN_COMPOUND) &&
				!(dst_pfns[i] & MIGRATE_PFN_COMPOUND)) {

				if (!migrate) {
					/* 无 VMA 无法拆页表中的 PMD migration entry，整组拒绝。 */
					src_pfns[i] &= ~(MIGRATE_PFN_MIGRATE |
							 MIGRATE_PFN_COMPOUND);
					goto next;
				}
				nr = 1 << folio_order(folio);
				addr = migrate->start + i * PAGE_SIZE;
				if (migrate_vma_split_unmapped_folio(migrate, i, addr, folio)) {
					/* 拆分失败同时清 MIGRATE/COMPOUND，避免 finalize 误按大页提交。 */
					src_pfns[i] &= ~(MIGRATE_PFN_MIGRATE |
							 MIGRATE_PFN_COMPOUND);
					goto next;
				}
			} else if ((src_pfns[i] & MIGRATE_PFN_MIGRATE) &&
				(dst_pfns[i] & MIGRATE_PFN_COMPOUND) &&
				!(src_pfns[i] & MIGRATE_PFN_COMPOUND)) {
				src_pfns[i] &= ~MIGRATE_PFN_MIGRATE;
				/* 目标是 compound 而源不是，当前协议不支持把多个独立源合并成一个 folio。 */
			}
		}


		if (folio_is_device_private(newfolio) ||
		    folio_is_device_coherent(newfolio)) {
			if (mapping) {
				/*
				 * For now only support anonymous memory migrating to
				 * device private or coherent memory.
				 *
				 * Try to get rid of swap cache if possible.
				 */
				/*
				 * 迁往 device-private/coherent 暂只支持匿名页；若匿名 folio 仍在 swap cache，
				 * 先尝试脱离，否则 mapping 无法转移到设备页。
				 */
				if (!folio_test_anon(folio) ||
				    !folio_free_swap(folio)) {
					src_pfns[i] &= ~MIGRATE_PFN_MIGRATE;
					goto next;
				}
			}
		} else if (folio_is_zone_device(newfolio)) {
			/*
			 * Other types of ZONE_DEVICE page are not supported.
			 */
			/* 除 private/coherent 外的 ZONE_DEVICE 类型没有本协议定义的 CPU 映射/ownership 语义。 */
			src_pfns[i] &= ~MIGRATE_PFN_MIGRATE;
			goto next;
		}

		/* unmap 阶段已稳定内容；仍在 writeback 说明调用协议破坏，不能安全转移 mapping。 */
		BUG_ON(folio_test_writeback(folio));

		if (migrate && migrate->fault_page == page)
			/* fault owner 的额外引用要传给 folio_migrate_mapping()，避免被误判为外部 pin。 */
			extra_cnt = 1;
		for (j = 0; j < nr && i + j < npages; j++) {
			/* 按拆分后的粒度逐 folio 迁移 mapping；单槽失败不阻止同组其它槽尝试。 */
			folio = page_folio(migrate_pfn_to_page(src_pfns[i+j]));
			newfolio = page_folio(migrate_pfn_to_page(dst_pfns[i+j]));

			r = folio_migrate_mapping(mapping, newfolio, folio, extra_cnt);
			if (r)
				/* 清 MIGRATE 把该槽路由到 finalize 的源页恢复分支。 */
				src_pfns[i+j] &= ~MIGRATE_PFN_MIGRATE;
			else
				folio_migrate_flags(newfolio, folio);
		}
next:
		/* nr 可能是 THP 页数或拆分后 1；始终与当前协议表示匹配。 */
		i += nr;
	}

	if (notified)
		/* 与首次空洞插页前的 start 严格配对，发布全部 CPU 页表修改后再结束失效窗口。 */
		mmu_notifier_invalidate_range_end(&range);
}

/**
 * migrate_device_pages() - migrate meta-data from src page to dst page
 * @src_pfns: src_pfns returned from migrate_device_range()
 * @dst_pfns: array of pfns allocated by the driver to migrate memory to
 * @npages: number of pages in the range
 *
 * Equivalent to migrate_vma_pages(). This is called to migrate struct page
 * meta-data from source struct page to destination.
 */
/*
 * migrate_device_pages() 是无 VMA 的 PFN-range 包装；src 来自 migrate_device_range/pfns，dst 是驱动
 * 分配并锁定的目标数组，npages 为槽数。无返回值，逐槽成功由 src MIGRATE 保留表示；不解锁任何页，
 * 调用者复制数据后仍必须调用 migrate_device_finalize()。数组 ownership 不转移，函数可能睡眠。
 */
void migrate_device_pages(unsigned long *src_pfns, unsigned long *dst_pfns,
			unsigned long npages)
{
	__migrate_device_pages(src_pfns, dst_pfns, npages, NULL);
}
/* 导出给设备驱动的元数据提交阶段，不能替代 finalize。 */
EXPORT_SYMBOL(migrate_device_pages);

/**
 * migrate_vma_pages() - migrate meta-data from src page to dst page
 * @migrate: migrate struct containing all migration information
 *
 * This migrates struct page meta-data from source struct page to destination
 * struct page. This effectively finishes the migration from source page to the
 * destination page.
 */
/*
 * migrate_vma_pages() 是 VMA 事务包装，把 setup 填充的数组和范围上下文传入公共提交核心。
 * @migrate 仍由驱动拥有；无直接返回，成功槽继续带 MIGRATE，失败槽已清除。源/目标保持锁定，
 * mmap 读锁仍由调用者持有；下一步通常更新设备页表并调用 migrate_vma_finalize()。
 */
void migrate_vma_pages(struct migrate_vma *migrate)
{
	__migrate_device_pages(migrate->src, migrate->dst, migrate->npages, migrate);
}
/* 导出 VMA 协议的元数据提交阶段。 */
EXPORT_SYMBOL(migrate_vma_pages);

/*
 * __migrate_device_finalize() - 按 src MIGRATE 结果恢复 CPU 映射并释放事务锁/引用。
 * 业务背景：无论 pages 阶段成功与否都必须执行；成功用 dst 替换 migration entry，失败恢复 src。
 * 入参：src/dst 数组和槽数借用；fault_page 可空，非空时其 folio 锁归 fault owner。
 * 出参/返回：无；目标未采用时解锁/put，源经 remove_migration_ptes 后解锁/put，成功目标也释放驱动
 * 交给事务的临时锁/引用。注意事项：folio 加回 LRU 发生在 PTE 恢复前；同一 folio 的 tail 槽依协议处理。
 */
static void __migrate_device_finalize(unsigned long *src_pfns,
				      unsigned long *dst_pfns,
				      unsigned long npages,
				      struct page *fault_page)
{
	/* fault_folio 标记唯一不由本函数解锁的源；i 逐槽完成最终 ownership 交接。 */
	struct folio *fault_folio = fault_page ?
		page_folio(fault_page) : NULL;
	unsigned long i;

	for (i = 0; i < npages; i++) {
		/* src/dst 是每槽最终选择；newpage/page 可为空洞，folio 变量只保存 head。 */
		struct folio *dst = NULL, *src = NULL;
		struct page *newpage = migrate_pfn_to_page(dst_pfns[i]);
		struct page *page = migrate_pfn_to_page(src_pfns[i]);

		if (newpage)
			dst = page_folio(newpage);

		if (!page) {
			/* 空洞插页没有源 migration PTE owner；若目标未被采用，只需释放目标临时锁/引用。 */
			if (dst) {
				WARN_ON_ONCE(fault_folio == dst);
				folio_unlock(dst);
				folio_put(dst);
			}
			continue;
		}

		src = page_folio(page);

		if (!(src_pfns[i] & MIGRATE_PFN_MIGRATE) || !dst) {
			/* 提交失败或无目标：先丢弃 dst，再令 dst=src，使恢复 helper 指回原 folio。 */
			if (dst) {
				WARN_ON_ONCE(fault_folio == dst);
				folio_unlock(dst);
				folio_put(dst);
			}
			dst = src;
		}

		if (!folio_is_zone_device(dst))
			/* 普通目标在重新可访问前回到 LRU；ZONE_DEVICE 不参与 LRU。 */
			folio_add_lru(dst);
		remove_migration_ptes(src, dst, 0);
		/* 这是 CPU 映射恢复/替换的可观察提交点，所有 migration entry 重新指向最终 folio。 */
		if (fault_folio != src)
			folio_unlock(src);
		folio_put(src);

		if (dst != src) {
			/* 成功目标已由页表/rmap 持有，释放驱动交给迁移事务的临时锁和引用。 */
			WARN_ON_ONCE(fault_folio == dst);
			folio_unlock(dst);
			folio_put(dst);
		}
	}
}

/*
 * migrate_device_finalize() - complete page migration
 * @src_pfns: src_pfns returned from migrate_device_range()
 * @dst_pfns: array of pfns allocated by the driver to migrate memory to
 * @npages: number of pages in the range
 *
 * Completes migration of the page by removing special migration entries.
 * Drivers must ensure copying of page data is complete and visible to the CPU
 * before calling this.
 */
/*
 * migrate_device_finalize() 是 PFN-range 对外收尾接口。驱动必须在调用前完成并向 CPU 发布数据复制；
 * src/dst/npages 与 pages 阶段完全相同。无返回值，调用后迁移条目已移除、临时页锁和引用已消费，
 * 数组仍归调用者但其中 PFN/标志只适合作结果记录，不能再据此重复 finalize。
 */
void migrate_device_finalize(unsigned long *src_pfns,
			     unsigned long *dst_pfns, unsigned long npages)
{
	return __migrate_device_finalize(src_pfns, dst_pfns, npages, NULL);
}
/* 导出无 VMA 迁移的强制收尾阶段。 */
EXPORT_SYMBOL(migrate_device_finalize);

/**
 * migrate_vma_finalize() - restore CPU page table entry
 * @migrate: migrate struct containing all migration information
 *
 * This replaces the special migration pte entry with either a mapping to the
 * new page if migration was successful for that page, or to the original page
 * otherwise.
 *
 * This also unlocks the pages and puts them back on the lru, or drops the extra
 * refcount, for device pages.
 */
/*
 * migrate_vma_finalize() 是 VMA 协议的强制收尾：成功槽映射目标，失败槽恢复源，并释放普通 LRU/设备页
 * 的锁和额外引用。@migrate 借用且必须与 setup/pages 为同一事务；无返回值。fault_page 的外层锁不在
 * 本函数释放。调用后驱动可释放事务数组并退出 mmap 读锁，禁止再次访问已转移的临时页 ownership。
 */
void migrate_vma_finalize(struct migrate_vma *migrate)
{
	__migrate_device_finalize(migrate->src, migrate->dst, migrate->npages,
				  migrate->fault_page);
}
/* 导出 VMA 迁移的最终闭环接口。 */
EXPORT_SYMBOL(migrate_vma_finalize);

/*
 * migrate_device_pfn_lock() - 为 PFN-range 迁移取得 folio 引用并尝试锁定。
 * 业务背景：没有 VMA/pagewalk 可代为稳定源页，range/pfns 入口必须直接把 PFN 升级为事务 ownership。
 * 入参：pfn 为调用者提供的源 PFN；出参/返回：成功返回编码 PFN|MIGRATE，失败返回 0。
 * 注意事项：folio_get_nontail_page() 规范化并取得 head 引用；只 trylock 避免死锁，失败立即 put；
 * 成功的锁/引用由 migrate_device_unmap/finalize 链消费。
 */
static unsigned long migrate_device_pfn_lock(unsigned long pfn)
{
	/* folio 只在成功返回时转移给迁移事务；任何失败都不留下引用。 */
	struct folio *folio;

	folio = folio_get_nontail_page(pfn_to_page(pfn));
	if (!folio)
		return 0;

	if (!folio_trylock(folio)) {
		/* PFN-range 迁移是 best-effort，不等待可能由其它迁移持有的锁。 */
		folio_put(folio);
		return 0;
	}

	return migrate_pfn(pfn) | MIGRATE_PFN_MIGRATE;
}

/**
 * migrate_device_range() - migrate device private pfns to normal memory.
 * @src_pfns: array large enough to hold migrating source device private pfns.
 * @start: starting pfn in the range to migrate.
 * @npages: number of pages to migrate.
 *
 * migrate_vma_setup() is similar in concept to migrate_vma_setup() except that
 * instead of looking up pages based on virtual address mappings a range of
 * device pfns that should be migrated to system memory is used instead.
 *
 * This is useful when a driver needs to free device memory but doesn't know the
 * virtual mappings of every page that may be in device memory. For example this
 * is often the case when a driver is being unloaded or unbound from a device.
 *
 * Like migrate_vma_setup() this function will take a reference and lock any
 * migrating pages that aren't free before unmapping them. Drivers may then
 * allocate destination pages and start copying data from the device to CPU
 * memory before calling migrate_device_pages().
 */
/*
 * 上述接口供驱动在卸载/解绑等不知道全部虚拟映射的场景，把一段连续 device PFN 拉回系统内存。
 * 它逐 folio 取得引用/锁并标记 compound 首页，再由 migrate_device_unmap() 撤所有反向映射。
 */
/*
 * migrate_device_range() - 建立连续 PFN 范围的无 VMA 迁移事务。
 * 入参：src_pfns 是至少 npages 槽的输出数组；start/npages 描述连续 PFN 半开范围，数组归调用者。
 * 出参/返回：当前恒返回 0；每个成功候选写 PFN|MIGRATE，失败为 0，compound tail 置零。
 * 注意事项：可能睡眠；成功页保持锁/引用，调用者必须准备 dst、复制并调用 pages/finalize；函数按
 * folio 粒度跨尾页，要求输入范围和数组足以容纳遇到的完整 folio。
 */
int migrate_device_range(unsigned long *src_pfns, unsigned long start,
			unsigned long npages)
{
	/* pfn/i 同步遍历物理范围与数组；j 展开 compound tail，nr 是当前 folio 页数。 */
	unsigned long i, j, pfn;

	for (pfn = start, i = 0; i < npages; pfn++, i++) {
		struct page *page = pfn_to_page(pfn);
		struct folio *folio = page_folio(page);
		unsigned int nr = 1;

		src_pfns[i] = migrate_device_pfn_lock(pfn);
		/* 即使锁失败也用真实 folio 大小跨过 tails，避免对同一 head 重复加锁。 */
		nr = folio_nr_pages(folio);
		if (nr > 1) {
			/* 只有首页承载 PFN/协议位，tail 槽清零并由 COMPOUND 指示整体粒度。 */
			src_pfns[i] |= MIGRATE_PFN_COMPOUND;
			for (j = 1; j < nr; j++)
				src_pfns[i+j] = 0;
			i += j - 1;
			pfn += j - 1;
		}
	}

	/* 隔离 LRU/撤 rmap/拒绝 pin；返回计数在 PFN API 中不需要，逐槽标志即结果。 */
	migrate_device_unmap(src_pfns, npages, NULL);

	return 0;
}
/* 导出连续物理范围事务入口。 */
EXPORT_SYMBOL(migrate_device_range);

/**
 * migrate_device_pfns() - migrate device private pfns to normal memory.
 * @src_pfns: pre-populated array of source device private pfns to migrate.
 * @npages: number of pages to migrate.
 *
 * Similar to migrate_device_range() but supports non-contiguous pre-populated
 * array of device pages to migrate.
 */
/*
 * migrate_device_pfns() 与 range 入口相同，但 src_pfns 入参预装非连续 PFN；函数原地把每个首页替换为
 * 协议编码，compound tail 清零。成功/失败和后续 pages/finalize 契约相同。
 */
/*
 * 入参：src_pfns 为输入输出数组，npages 为槽数；出参/返回：恒 0，实际候选由 MIGRATE 位表达。
 * 注意事项：会覆盖原 PFN 数组、可能睡眠；每个成功 folio 的锁/引用转给事务，必须最终 finalize。
 */
int migrate_device_pfns(unsigned long *src_pfns, unsigned long npages)
{
	/* i 遍历用户给出的首页 PFN，j/nr 用于识别并清空同一 compound folio 的尾槽。 */
	unsigned long i, j;

	for (i = 0; i < npages; i++) {
		struct page *page = pfn_to_page(src_pfns[i]);
		struct folio *folio = page_folio(page);
		unsigned int nr = 1;

		src_pfns[i] = migrate_device_pfn_lock(src_pfns[i]);
		/* 原值在调用 lock 前已用于找 folio；返回编码覆盖该槽并成为后续唯一权威。 */
		nr = folio_nr_pages(folio);
		if (nr > 1) {
			/* 非连续数组仍要求 compound folio 的 tails 紧随首页，以便统一页数推进。 */
			src_pfns[i] |= MIGRATE_PFN_COMPOUND;
			for (j = 1; j < nr; j++)
				src_pfns[i+j] = 0;
			i += j - 1;
		}
	}

	/* 与连续 range 共用撤映射/pin 检查，结果完全写回 src 标志。 */
	migrate_device_unmap(src_pfns, npages, NULL);

	return 0;
}
/* 导出非连续 PFN 数组事务入口。 */
EXPORT_SYMBOL(migrate_device_pfns);

/*
 * Migrate a device coherent folio back to normal memory. The caller should have
 * a reference on folio which will be copied to the new folio if migration is
 * successful or dropped on failure.
 */
/*
 * 把单个 device-coherent order-0 folio 拉回普通内存。调用者持有的源引用在成功时转移到新 folio，
 * 失败时被迁移协议消费；因此无论返回值如何都不能按原 ownership 再 put 源引用。
 */
/*
 * migrate_device_coherent_folio() - 完成单页 coherent→system 的分配、复制与收尾。
 * 入参：folio 为调用者持引用的 order-0 coherent folio，函数取得其锁；出参/返回：成功 0，无法撤映射、
 * 分配或提交返回 -EBUSY。注意事项：无 VMA，直接使用 PFN 协议；目标在分配后上锁并交给 pages/finalize，
 * folio_copy() 只在元数据迁移成功后执行，finalize 始终配对释放临时 ownership。
 */
int migrate_device_coherent_folio(struct folio *folio)
{
	/* src_pfn/dst_pfn 是单槽协议数组；dfolio 是可能成为最终系统页的目标 owner。 */
	unsigned long src_pfn, dst_pfn = 0;
	struct folio *dfolio;

	WARN_ON_ONCE(folio_test_large(folio));

	/* 锁定源并建立候选；调用者那份引用成为迁移期间稳定源对象的一部分。 */
	folio_lock(folio);
	src_pfn = migrate_pfn(folio_pfn(folio)) | MIGRATE_PFN_MIGRATE;

	/*
	 * We don't have a VMA and don't need to walk the page tables to find
	 * the source folio. So call migrate_vma_unmap() directly to unmap the
	 * folio as migrate_vma_setup() will fail if args.vma == NULL.
	 */
	/* 本路径已直接知道源 folio 且没有 VMA，跳过 collect，直接撤全部 rmap 并检查 pin。 */
	migrate_device_unmap(&src_pfn, 1, NULL);
	if (!(src_pfn & MIGRATE_PFN_MIGRATE))
		/* unmap 失败已恢复映射并消费相应锁/引用，不能继续分配目标。 */
		return -EBUSY;

	dfolio = folio_alloc(GFP_USER | __GFP_NOWARN, 0);
	if (dfolio) {
		/* 目标锁从这里保持到 finalize；分配失败以 dst=0 让 pages 清 MIGRATE。 */
		folio_lock(dfolio);
		dst_pfn = migrate_pfn(folio_pfn(dfolio));
	}

	migrate_device_pages(&src_pfn, &dst_pfn, 1);
	if (src_pfn & MIGRATE_PFN_MIGRATE)
		/* mapping/flags 已成功转到目标后复制稳定数据，随后才恢复 CPU PTE。 */
		folio_copy(dfolio, folio);
	migrate_device_finalize(&src_pfn, &dst_pfn, 1);

	/* finalize 后数组标志仍记录最终结果，但页锁/临时引用均已消费。 */
	if (src_pfn & MIGRATE_PFN_MIGRATE)
		return 0;
	return -EBUSY;
}
