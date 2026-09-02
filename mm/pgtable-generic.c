// SPDX-License-Identifier: GPL-2.0
/*
 *  mm/pgtable-generic.c
 *
 *  Generic pgtable methods declared in linux/pgtable.h
 *
 *  Copyright (C) 2010  Linus Torvalds
 */

#include <linux/pagemap.h>
#include <linux/hugetlb.h>
#include <linux/pgtable.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mm_inline.h>
#include <linux/iommu.h>
#include <linux/pgalloc.h>

#include <asm/tlb.h>

/*
 * 本文件只为体系结构没有定义 __HAVE_ARCH_* 覆盖的原语提供通用实现。
 * 修改页表项的调用者通常已持对应 PTL；通用 helper 负责保持“写表项后按需
 * TLB 失效”的顺序，体系结构实现可用更强的原子指令替换但不能削弱该契约。
 */
/*
 * If a p?d_bad entry is found while walking page tables, report
 * the error, before resetting entry to p?d_none.  Usually (but
 * very seldom) called out from the p?d_none_or_clear_bad macros.
 */
/* 中文翻译：页表遍历发现坏的 p?d 时先报告，再把它清成 none；该路径极少发生。 */

/*
 * pgd_clear_bad() - 诊断并清除一个非法 PGD 项。
 * @pgd 是调用者锁/页表生命周期内借用的可写项；无返回值。先用架构 ERROR
 * 打印旧值，再清为 none，清除是外界可观察的恢复点；函数不自行 flush TLB。
 */
void pgd_clear_bad(pgd_t *pgd)
{
	pgd_ERROR(*pgd);
	pgd_clear(pgd);
}

#ifndef __PAGETABLE_P4D_FOLDED
/*
 * p4d_clear_bad() - 非折叠 P4D 遍历发现坏项时诊断并恢复。
 * @p4d 是调用者页表锁/生命周期内借用的可写项；无返回值、不睡眠。ERROR 后
 * 清成 none，调用者可把该分支当作无下级表继续，折叠配置不生成此函数。
 */
void p4d_clear_bad(p4d_t *p4d)
{
	p4d_ERROR(*p4d);
	p4d_clear(p4d);
}
#endif

#ifndef __PAGETABLE_PUD_FOLDED
/*
 * pud_clear_bad() - 非折叠 PUD 遍历的坏项诊断与清除回退。
 * @pud 只借用且由上层页表稳定条件保护；无返回值、不自行 flush。报告旧值后
 * 发布 none，调用者随后按缺失下级表处理；折叠配置直接使用上层实现。
 */
void pud_clear_bad(pud_t *pud)
{
	pud_ERROR(*pud);
	pud_clear(pud);
}
#endif

/*
 * Note that the pmd variant below can't be stub'ed out just as for p4d/pud
 * above. pmd folding is special and typically pmd_* macros refer to upper
 * level even when folded
 */
/* 中文翻译：PMD 折叠具有特殊宏语义，故不能像 P4D/PUD 那样把实现整体裁掉。 */
/*
 * pmd_clear_bad() - 报告并清除非法 PMD，即使 PMD 折叠也始终提供。
 * @pmd 是借用可写项；无返回值。调用者负责 PTL/页表生命周期和后续遍历，函数
 * 不睡眠、不 flush；pmd_clear 发布 none 后本次 walker 不再进入错误下级表。
 */
void pmd_clear_bad(pmd_t *pmd)
{
	pmd_ERROR(*pmd);
	pmd_clear(pmd);
}

#ifndef __HAVE_ARCH_PTEP_SET_ACCESS_FLAGS
/*
 * Only sets the access flags (dirty, accessed), as well as write
 * permission. Furthermore, we know it always gets set to a "more
 * permissive" setting, which allows most architectures to optimize
 * this. We return whether the PTE actually changed, which in turn
 * instructs the caller to do things like update__mmu_cache.  This
 * used to be done in the caller, but sparc needs minor faults to
 * force that call on sun4c so we changed this macro slightly
 */
/*
 * 中文翻译：这里只把 PTE 变得更宽松（dirty/accessed/write）；返回是否真的
 * 改变，以便调用者决定 update_mmu_cache 等后续动作，某些架构即使小 fault
 * 也依赖这项通知。
 */
/*
 * ptep_set_access_flags() - 在 fault PTL 内升级一个 PTE 的访问权限。
 * @vma/@ptep 借用，@address 为页地址，@entry 是新值，@dirty 供架构 ABI 使用；
 * 改变返回 1，否则 0。写入后立即修复伪 fault 的 TLB，调用者再更新 MMU cache。
 */
int ptep_set_access_flags(struct vm_area_struct *vma,
			  unsigned long address, pte_t *ptep,
			  pte_t entry, int dirty)
{
	/* 相同项是无写入、无 flush 的快速路径。 */
	int changed = !pte_same(ptep_get(ptep), entry);
	if (changed) {
		/* set_pte_at 发布更宽松权限；TLB helper 与该发布顺序不可交换。 */
		set_pte_at(vma->vm_mm, address, ptep, entry);
		flush_tlb_fix_spurious_fault(vma, address, ptep);
	}
	return changed;
}
#endif

#ifndef __HAVE_ARCH_PTEP_CLEAR_YOUNG_FLUSH
/*
 * ptep_clear_flush_young() - 原子测试并清除 PTE accessed 位。
 * @vma/@ptep 借用，@address 为目标页；原来 young 返回 true 并 flush 单页 TLB，
 * 否则 false 且不 flush。调用者持 PTL；清位供 reclaim/idle 观察下一轮新访问。
 */
bool ptep_clear_flush_young(struct vm_area_struct *vma,
		unsigned long address, pte_t *ptep)
{
	bool young;

	/* 只有硬件可能缓存旧 young 项时才需要失效，false 是快速路径。 */
	young = ptep_test_and_clear_young(vma, address, ptep);
	if (young)
		flush_tlb_page(vma, address);
	return young;
}
#endif

#ifndef __HAVE_ARCH_PTEP_CLEAR_FLUSH
/*
 * ptep_clear_flush() - 摘除 PTE，并在旧项可访问时同步失效 TLB。
 * @vma/@ptep 借用，@address 为页地址；返回旧 PTE 供调用者释放/计账。调用者持
 * PTL；get_and_clear 是页表发布点，flush 完成后旧物理映射才可安全回收。
 */
pte_t ptep_clear_flush(struct vm_area_struct *vma, unsigned long address,
		       pte_t *ptep)
{
	struct mm_struct *mm = (vma)->vm_mm;
	pte_t pte;
	/* 非 present/不可访问旧项没有 CPU TLB 映射，可跳过昂贵 shootdown。 */
	pte = ptep_get_and_clear(mm, address, ptep);
	if (pte_accessible(mm, pte))
		flush_tlb_page(vma, address);
	return pte;
}
#endif

#ifdef CONFIG_TRANSPARENT_HUGEPAGE

#ifndef __HAVE_ARCH_PMDP_SET_ACCESS_FLAGS
/*
 * pmdp_set_access_flags() - THP PMD 版权限升级与 TLB 同步。
 * @address 必须 PMD 对齐，@entry 只会更宽松，@dirty 为架构 ABI；改变返回 1，
 * 相同返回 0。调用者持 PMD 锁；发布新 PMD 后失效整个 HPAGE_PMD_SIZE 范围。
 */
int pmdp_set_access_flags(struct vm_area_struct *vma,
			  unsigned long address, pmd_t *pmdp,
			  pmd_t entry, int dirty)
{
	int changed = !pmd_same(*pmdp, entry);
	/* 对齐是 huge leaf 覆盖范围的前提，错误地址会 flush 错误区间。 */
	VM_BUG_ON(address & ~HPAGE_PMD_MASK);
	if (changed) {
		set_pmd_at(vma->vm_mm, address, pmdp, entry);
		flush_pmd_tlb_range(vma, address, address + HPAGE_PMD_SIZE);
	}
	return changed;
}
#endif

#ifndef __HAVE_ARCH_PMDP_CLEAR_YOUNG_FLUSH
/*
 * pmdp_clear_flush_young() - 为 reclaim/idle 清 THP PMD 的 accessed 位。
 * @vma/@pmdp 借用，@address 必须 PMD 对齐；调用者持 PMD 锁且不可睡眠。返回
 * 原 young 状态，仅 true 时 flush 整个 PMD 范围，返回后可观察下一轮新访问。
 */
bool pmdp_clear_flush_young(struct vm_area_struct *vma,
		unsigned long address, pmd_t *pmdp)
{
	bool young;

	/* 调用者持 PMD 锁；清位后 reclaim 才能区分此后发生的新访问。 */
	VM_BUG_ON(address & ~HPAGE_PMD_MASK);
	young = pmdp_test_and_clear_young(vma, address, pmdp);
	if (young)
		flush_pmd_tlb_range(vma, address, address + HPAGE_PMD_SIZE);
	return young;
}
#endif

#ifndef __HAVE_ARCH_PMDP_HUGE_CLEAR_FLUSH
/*
 * pmdp_huge_clear_flush() - 摘除一个 THP PMD leaf 并完成范围 TLB shootdown。
 * @vma/@pmdp 借用，@address PMD 对齐；返回旧 PMD。允许非 present 特殊项，但
 * present 项必须是 THP。调用者持锁，flush 后才可拆分/迁移旧 huge mapping。
 */
pmd_t pmdp_huge_clear_flush(struct vm_area_struct *vma, unsigned long address,
			    pmd_t *pmdp)
{
	pmd_t pmd;
	VM_BUG_ON(address & ~HPAGE_PMD_MASK);
	VM_BUG_ON(pmd_present(*pmdp) && !pmd_trans_huge(*pmdp));
	/* clear 是页表摘除点；随后范围失效构成释放旧映射前的硬屏障。 */
	pmd = pmdp_huge_get_and_clear(vma->vm_mm, address, pmdp);
	flush_pmd_tlb_range(vma, address, address + HPAGE_PMD_SIZE);
	return pmd;
}

#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
/*
 * pudp_huge_clear_flush() - 摘除架构支持的 transparent huge PUD leaf。
 * @vma/@pudp 借用，@address PUD 对齐；调用者持页表锁。返回旧 PUD 供拆分/释放，
 * clear 后同步 flush 整个 PUD 范围，完成前旧 backing 不得释放；无失败返回。
 */
pud_t pudp_huge_clear_flush(struct vm_area_struct *vma, unsigned long address,
			    pud_t *pudp)
{
	pud_t pud;

	/* 只接受对齐且确为 transparent huge 的 PUD，避免误清下一级页表指针。 */
	VM_BUG_ON(address & ~HPAGE_PUD_MASK);
	VM_BUG_ON(!pud_trans_huge(*pudp));
	pud = pudp_huge_get_and_clear(vma->vm_mm, address, pudp);
	flush_pud_tlb_range(vma, address, address + HPAGE_PUD_SIZE);
	return pud;
}
#endif
#endif

#ifndef __HAVE_ARCH_PGTABLE_DEPOSIT
/*
 * pgtable_trans_huge_deposit() - 把预分配 PTE 页表挂到 THP PMD 的备用 FIFO。
 * @mm/@pmdp 借用，@pgtable 的 ownership 转给 PMD 槽；无返回值。调用者必须持
 * pmd_lock，队列用 struct page::lru 串起，最后把新页设为 head，供 split 时取回。
 */
void pgtable_trans_huge_deposit(struct mm_struct *mm, pmd_t *pmdp,
				pgtable_t pgtable)
{
	assert_spin_locked(pmd_lockptr(mm, pmdp));

	/* FIFO */
	/* 中文翻译：新页放头部、withdraw 从最老的尾向前推进，整体表现为 FIFO。 */
	if (!pmd_huge_pte(mm, pmdp))
		INIT_LIST_HEAD(&pgtable->lru);
	else
		list_add(&pgtable->lru, &pmd_huge_pte(mm, pmdp)->lru);
	/* 更新 PMD 私有备用指针是 ownership 发布点，锁保护并发 deposit/withdraw。 */
	pmd_huge_pte(mm, pmdp) = pgtable;
}
#endif

#ifndef __HAVE_ARCH_PGTABLE_WITHDRAW
/* no "address" argument so destroys page coloring of some arch */
/* 中文翻译：接口没有 address，某些架构无法保持基于地址的页表着色。 */
/*
 * pgtable_trans_huge_withdraw() - 从 THP PMD 的备用 FIFO 取回最老页表。
 * @mm/@pmdp 借用且 pmd_lock 必须已持有；返回的 pgtable ownership 交给调用者，
 * 队列不能为空。函数摘下返回页并推进 head，调用者通常用它拆分 THP 或迁移 PMD。
 */
pgtable_t pgtable_trans_huge_withdraw(struct mm_struct *mm, pmd_t *pmdp)
{
	pgtable_t pgtable;

	assert_spin_locked(pmd_lockptr(mm, pmdp));

	/* FIFO */
	/* 中文翻译：先保存当前返回项，再用其 lru 链中的下一项更新 PMD 私有指针。 */
	pgtable = pmd_huge_pte(mm, pmdp);
	pmd_huge_pte(mm, pmdp) = list_first_entry_or_null(&pgtable->lru,
							  struct page, lru);
	/* 非空时从链上摘除新 head 与旧返回项的连接；空队列无需 list_del。 */
	if (pmd_huge_pte(mm, pmdp))
		list_del(&pgtable->lru);
	return pgtable;
}
#endif

#ifndef __HAVE_ARCH_PMDP_INVALIDATE
/*
 * pmdp_invalidate() - 保留 PMD 内容但暂时清 present，并同步失效 huge TLB。
 * @vma/@pmdp 借用，@address 为对齐 huge 地址；返回替换前 PMD。调用者持锁，
 * establish 原子发布 invalid 版本，flush 后可安全修改访问/脏位再恢复 present。
 */
pmd_t pmdp_invalidate(struct vm_area_struct *vma, unsigned long address,
		     pmd_t *pmdp)
{
	/* 非 present 输入违反协议但仍尽力建立 invalid 值并返回旧项供恢复。 */
	VM_WARN_ON_ONCE(!pmd_present(*pmdp));
	pmd_t old = pmdp_establish(vma, address, pmdp, pmd_mkinvalid(*pmdp));
	flush_pmd_tlb_range(vma, address, address + HPAGE_PMD_SIZE);
	return old;
}
#endif

#ifndef __HAVE_ARCH_PMDP_INVALIDATE_AD
/*
 * pmdp_invalidate_ad() - 为只更新 access/dirty 的调用者临时 invalidate PMD。
 * @vma/@pmdp/@address 与 pmdp_invalidate 同契约，返回旧 PMD；通用实现没有更窄
 * 的 A/D 优化，故复用完整范围 flush。调用者持锁并负责稍后恢复有效项。
 */
pmd_t pmdp_invalidate_ad(struct vm_area_struct *vma, unsigned long address,
			 pmd_t *pmdp)
{
	VM_WARN_ON_ONCE(!pmd_present(*pmdp));
	return pmdp_invalidate(vma, address, pmdp);
}
#endif

#ifndef pmdp_collapse_flush
/*
 * pmdp_collapse_flush() - 为 khugepaged collapse 摘除普通 PTE 页表 PMD。
 * @address PMD 对齐，@pmdp 必须不是现成 THP；返回旧页表 PMD。调用者持锁，清除
 * 后必须 flush 普通 PTE 粒度的整个范围，再安装 huge PMD 并延迟释放旧 PTE 页。
 */
pmd_t pmdp_collapse_flush(struct vm_area_struct *vma, unsigned long address,
			  pmd_t *pmdp)
{
	/*
	 * pmd and hugepage pte format are same. So we could
	 * use the same function.
	 */
	/* 中文翻译：普通 PMD 与 huge PTE 编码相同，因而可复用 huge get-and-clear。 */
	pmd_t pmd;

	VM_BUG_ON(address & ~HPAGE_PMD_MASK);
	VM_BUG_ON(pmd_trans_huge(*pmdp));
	pmd = pmdp_huge_get_and_clear(vma->vm_mm, address, pmdp);

	/* collapse entails shooting down ptes not pmd */
	/* 中文翻译：collapse 淘汰的是一组 PTE 缓存项，必须用普通 range flush。 */
	flush_tlb_range(vma, address, address + HPAGE_PMD_SIZE);
	return pmd;
}
#endif

/* arch define pte_free_defer in asm/pgalloc.h for its own implementation */
/* 中文翻译：架构可在 asm/pgalloc.h 自定义 pte_free_defer；以下仅为通用回退。 */
#ifndef pte_free_defer
/*
 * pte_free_now() - RCU grace period 后真正释放旧 PTE 页表。
 * @head 内嵌于 struct page，回调用 container_of 恢复 pgtable；无返回值。此时早先
 * 的 RCU lockless walker 已退出，ownership 从 RCU 队列转给 pte_free 并终结。
 */
static void pte_free_now(struct rcu_head *head)
{
	struct page *page;

	/* mm 未随 callback 保存且通用 pte_free 不使用它，显式传 NULL 说明契约。 */
	page = container_of(head, struct page, rcu_head);
	pte_free(NULL /* mm not passed and not used */, (pgtable_t)page);
}

/*
 * pte_free_defer() - 把已从页表摘除的 PTE 页延迟到 RCU 后释放。
 * @mm 在通用实现中未使用，@pgtable ownership 立即转给 RCU callback；无返回值。
 * 调用者在完成页表摘除/TLB 协议后调用，之后不得再访问该页表。
 */
void pte_free_defer(struct mm_struct *mm, pgtable_t pgtable)
{
	struct page *page;

	/* call_rcu 是不可回滚的移交点，保证并发 __pte_offset_map 读侧先结束。 */
	page = pgtable;
	call_rcu(&page->rcu_head, pte_free_now);
}
#endif /* pte_free_defer */
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#if defined(CONFIG_GUP_GET_PXX_LOW_HIGH) && \
	(defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RCU))
/*
 * See the comment above ptep_get_lockless() in include/linux/pgtable.h:
 * the barriers in pmdp_get_lockless() cannot guarantee that the value in
 * pmd_high actually belongs with the value in pmd_low; but holding interrupts
 * off blocks the TLB flush between present updates, which guarantees that a
 * successful __pte_offset_map() points to a page from matched halves.
 */
/*
 * 中文翻译：分裂 PMD 的高低半读取屏障不能证明两半同代；本地关中断阻止
 * present 更新之间的 TLB flush 插入，从而让成功映射只使用匹配的两半。
 */
/*
 * pmdp_get_lockless_start() - 为分裂 PMD lockless 读取建立本 CPU 临界区。
 * 无入参；保存并关闭本地中断，返回 flags 必须交给 end 恢复。不可睡眠；调用者
 * 紧接着读取 PMD，两者配对阻止 present 更新之间的 TLB flush 在本 CPU 插入。
 */
static unsigned long pmdp_get_lockless_start(void)
{
	unsigned long irqflags;

	local_irq_save(irqflags);
	return irqflags;
}

/*
 * pmdp_get_lockless_end() - 结束分裂 PMD lockless 读取窗口。
 * @irqflags 必须来自同 CPU 的 start；无返回值、不睡眠。恢复入口中断状态后，
 * 调用者可检查刚取得的同代 PMD 快照；错配会破坏中断状态。
 */
static void pmdp_get_lockless_end(unsigned long irqflags)
{
	local_irq_restore(irqflags);
}
#else
/*
 * 其他配置的 start/end 是成对零成本桩：start 无参数返回 0，end 借用并忽略
 * @irqflags、无返回值；均不睡眠且无副作用，使 __pte_offset_map 保持统一形状。
 */
static unsigned long pmdp_get_lockless_start(void) { return 0; }
static void pmdp_get_lockless_end(unsigned long irqflags) { }
#endif

/*
 * __pte_offset_map() - 在 RCU 下从 PMD 快照映射 @addr 对应的 PTE。
 * @pmd 借用，@addr 为虚拟地址，@pmdvalp 可空且成功/失败都输出本次 PMD 快照；
 * 成功返回已 kmap 的 PTE 并保持 rcu_read_lock，调用者必须 pte_unmap 配对；无
 * 普通页表、遇 THP 或坏 PMD 返回 NULL 并自行解 RCU。函数不持 PTL，只适合只读
 * 或由上层随后锁后复核；坏项会被清除，是失败路径的额外副作用。
 */
pte_t *__pte_offset_map(pmd_t *pmd, unsigned long addr, pmd_t *pmdvalp)
{
	unsigned long irqflags;
	pmd_t pmdval;

	/* 第一阶段进入 RCU，保证被摘除的 PTE 页至少存活到 pte_unmap。 */
	rcu_read_lock();
	irqflags = pmdp_get_lockless_start();
	/* 分裂 PMD 配置在短暂关中断窗口读取自洽快照，随后立即恢复中断。 */
	pmdval = pmdp_get_lockless(pmd);
	pmdp_get_lockless_end(irqflags);

	/* 输出快照供 may-write 调用者锁后做 pmd_same 复核，ownership 不转移。 */
	if (pmdvalp)
		*pmdvalp = pmdval;
	/* none/non-present/THP 都没有普通 PTE 页，统一走 nomap 释放 RCU。 */
	if (unlikely(pmd_none(pmdval) || !pmd_present(pmdval)))
		goto nomap;
	if (unlikely(pmd_trans_huge(pmdval)))
		goto nomap;
	/* 坏项除返回失败外还被清成 none，阻止后续遍历继续使用非法指针。 */
	if (unlikely(pmd_bad(pmdval))) {
		pmd_clear_bad(pmd);
		goto nomap;
	}
	/* 成功把页表页映射成 PTE 指针；RCU ownership 留给调用者配对释放。 */
	return __pte_map(&pmdval, addr);
nomap:
	/* 所有失败均在本函数闭合 RCU，因此 NULL 调用者不得再执行 pte_unmap。 */
	rcu_read_unlock();
	return NULL;
}

/*
 * pte_offset_map_ro_nolock() - 返回只读 PTE 映射及其准确 PTL 指针但不加锁。
 * @mm/@pmd 借用，@addr 定位项，成功在 @ptlp 输出锁并返回 PTE，失败 NULL 且
 * @ptlp 不变。调用者只能读，即使后来拿锁也不能修改可能已脱离 mm 的旧页表；
 * 最终必须 pte_unmap 释放 RCU/kmap。
 */
pte_t *pte_offset_map_ro_nolock(struct mm_struct *mm, pmd_t *pmd,
				unsigned long addr, spinlock_t **ptlp)
{
	pmd_t pmdval;
	pte_t *pte;

	/* 用同一 PMD 快照计算锁地址，避免再次读取已变化的 *pmd。 */
	pte = __pte_offset_map(pmd, addr, &pmdval);
	if (likely(pte))
		*ptlp = pte_lockptr(mm, &pmdval);
	return pte;
}

/*
 * pte_offset_map_rw_nolock() - 为可能后续写入返回 PTE、PMD 快照和 PTL。
 * @pmdvalp 必须非空；成功输出快照/锁并保持 RCU，失败 NULL。调用者拿 @ptlp 后
 * 必须用 pmd_same/pte_same 验证页表仍连接，验证通过才可写，最后 pte_unmap。
 */
pte_t *pte_offset_map_rw_nolock(struct mm_struct *mm, pmd_t *pmd,
				unsigned long addr, pmd_t *pmdvalp,
				spinlock_t **ptlp)
{
	pte_t *pte;

	/* 非空输出是锁后稳定性复核所必需，WARN 捕获误用但仍沿原 ABI 执行。 */
	VM_WARN_ON_ONCE(!pmdvalp);
	pte = __pte_offset_map(pmd, addr, pmdvalp);
	if (likely(pte))
		*ptlp = pte_lockptr(mm, pmdvalp);
	return pte;
}

/*
 * pte_offset_map_lock(mm, pmd, addr, ptlp) is usually called with the pmd
 * pointer for addr, reached by walking down the mm's pgd, p4d, pud for addr:
 * either while holding mmap_lock or vma lock for read or for write; or in
 * truncate or rmap context, while holding file's i_mmap_lock or anon_vma lock
 * for read (or for write). In a few cases, it may be used with pmd pointing to
 * a pmd_t already copied to or constructed on the stack.
 *
 * When successful, it returns the pte pointer for addr, with its page table
 * kmapped if necessary (when CONFIG_HIGHPTE), and locked against concurrent
 * modification by software, with a pointer to that spinlock in ptlp (in some
 * configs mm->page_table_lock, in SPLIT_PTLOCK configs a spinlock in table's
 * struct page).  pte_unmap_unlock(pte, ptl) to unlock and unmap afterwards.
 *
 * But it is unsuccessful, returning NULL with *ptlp unchanged, if there is no
 * page table at *pmd: if, for example, the page table has just been removed,
 * or replaced by the huge pmd of a THP.  (When successful, *pmd is rechecked
 * after acquiring the ptlock, and retried internally if it changed: so that a
 * page table can be safely removed or replaced by THP while holding its lock.)
 *
 * pte_offset_map(pmd, addr), and its internal helper __pte_offset_map() above,
 * just returns the pte pointer for addr, its page table kmapped if necessary;
 * or NULL if there is no page table at *pmd.  It does not attempt to lock the
 * page table, so cannot normally be used when the page table is to be updated,
 * or when entries read must be stable.  But it does take rcu_read_lock(): so
 * that even when page table is racily removed, it remains a valid though empty
 * and disconnected table.  Until pte_unmap(pte) unmaps and rcu_read_unlock()s
 * afterwards.
 *
 * pte_offset_map_ro_nolock(mm, pmd, addr, ptlp), above, is like pte_offset_map();
 * but when successful, it also outputs a pointer to the spinlock in ptlp - as
 * pte_offset_map_lock() does, but in this case without locking it.  This helps
 * the caller to avoid a later pte_lockptr(mm, *pmd), which might by that time
 * act on a changed *pmd: pte_offset_map_ro_nolock() provides the correct spinlock
 * pointer for the page table that it returns. Even after grabbing the spinlock,
 * we might be looking either at a page table that is still mapped or one that
 * was unmapped and is about to get freed. But for R/O access this is sufficient.
 * So it is only applicable for read-only cases where any modification operations
 * to the page table are not allowed even if the corresponding spinlock is held
 * afterwards.
 *
 * pte_offset_map_rw_nolock(mm, pmd, addr, pmdvalp, ptlp), above, is like
 * pte_offset_map_ro_nolock(); but when successful, it also outputs the pdmval.
 * It is applicable for may-write cases where any modification operations to the
 * page table may happen after the corresponding spinlock is held afterwards.
 * But the users should make sure the page table is stable like checking pte_same()
 * or checking pmd_same() by using the output pmdval before performing the write
 * operations.
 *
 * Note: "RO" / "RW" expresses the intended semantics, not that the *kmap* will
 * be read-only/read-write protected.
 *
 * Note that free_pgtables(), used after unmapping detached vmas, or when
 * exiting the whole mm, does not take page table lock before freeing a page
 * table, and may not use RCU at all: "outsiders" like khugepaged should avoid
 * pte_offset_map() and co once the vma is detached from mm or mm_users is zero.
 */
/*
 * 中文学习补充：上述契约可归纳为三层。lock 版本在 mmap/vma/i_mmap/
 * anon_vma 等外层稳定条件下映射并获取正确 PTL，锁后复核 PMD，变化则完整
 * unmap/unlock 后重试；普通 map 只用 RCU 保活，不能稳定内容；RO/RW nolock
 * 都返回该快照对应的锁，RW 还必须凭输出 PMD 做锁后同代验证。成功路径均由
 * pte_unmap[_unlock] 配对。VMA 已 detach 或 mm_users 为零时，free_pgtables
 * 可能不经 PTL/RCU，外部 walker 因而不得再进入这些接口。
 */
/*
 * pte_offset_map_lock() - 映射 PTE、取得 PTL，并验证 PMD 快照仍连接。
 * @mm/@pmd 借用，@addr 定位项，成功返回 PTE 且在 @ptlp 输出已持锁；失败返回
 * NULL。不可睡眠。调用者必须 pte_unmap_unlock；若锁前 PMD 改变，本函数释放
 * 当前映射/锁并从 again 重试，避免锁住已被 THP 替换的旧页表。
 */
pte_t *pte_offset_map_lock(struct mm_struct *mm, pmd_t *pmd,
			   unsigned long addr, spinlock_t **ptlp)
{
	spinlock_t *ptl;
	pmd_t pmdval;
	pte_t *pte;
again:
	/* 第一阶段在 RCU 下映射；NULL 已由 helper 解锁 RCU，可直接返回。 */
	pte = __pte_offset_map(pmd, addr, &pmdval);
	if (unlikely(!pte))
		return pte;
	/* 锁地址必须从快照计算；拿锁后再次读取 live PMD 判断是否同代。 */
	ptl = pte_lockptr(mm, &pmdval);
	spin_lock(ptl);
	/* 相同即同时发布 PTE 和已持 PTL；调用者从此拥有配对解锁责任。 */
	if (likely(pmd_same(pmdval, pmdp_get_lockless(pmd)))) {
		*ptlp = ptl;
		return pte;
	}
	/* 竞态失败路径逆序释放 PTL、kmap、RCU，然后重新取得新一代页表。 */
	pte_unmap_unlock(pte, ptl);
	goto again;
}

#ifdef CONFIG_ASYNC_KERNEL_PGTABLE_FREE
static void kernel_pgtable_work_func(struct work_struct *work);

static struct {
	struct list_head list;
	/* protect above ptdesc lists */
	/* 中文翻译：lock 只保护上面的待释放 ptdesc 链表，不覆盖 IOMMU flush。 */
	spinlock_t lock;
	struct work_struct work;
} kernel_pgtable_work = {
	/* 全局队列和 work 随内核存活；work 合并并批量处理多个异步释放请求。 */
	.list = LIST_HEAD_INIT(kernel_pgtable_work.list),
	.lock = __SPIN_LOCK_UNLOCKED(kernel_pgtable_work.lock),
	.work = __WORK_INITIALIZER(kernel_pgtable_work.work, kernel_pgtable_work_func),
};

/*
 * kernel_pgtable_work_func() - 批量完成内核页表的 IOMMU 失效与最终释放。
 * @work 是静态 work 的借用指针；无返回值，可在 workqueue 进程上下文睡眠。
 * 锁内把全局队列原子 splice 到私有链，锁外先全范围通知 SVA，再逐项释放；
 * 新请求可并发加入下一批，不会与当前私有 page_list 混用。
 */
static void kernel_pgtable_work_func(struct work_struct *work)
{
	struct ptdesc *pt, *next;
	LIST_HEAD(page_list);

	/* 阶段一短锁摘取整批，list_splice_tail_init 同时把共享队列重置为空。 */
	spin_lock(&kernel_pgtable_work.lock);
	list_splice_tail_init(&kernel_pgtable_work.list, &page_list);
	spin_unlock(&kernel_pgtable_work.lock);

	/* 阶段二在释放前让设备侧 SVA 丢弃所有内核虚拟地址翻译。 */
	iommu_sva_invalidate_kva_range(PAGE_OFFSET, TLB_FLUSH_ALL);
	list_for_each_entry_safe(pt, next, &page_list, pt_list)
		__pagetable_free(pt);
}

/*
 * pagetable_free_kernel() - 异步移交一个已摘除的内核页表页。
 * @pt ownership 转给全局 work 队列；无返回值，不可在调用后访问。锁保护并发
 * 生产者，schedule_work 可安全合并重复调度；worker 在 IOMMU flush 后最终释放。
 */
void pagetable_free_kernel(struct ptdesc *pt)
{
	/* 入队是 ownership 发布点，pt_list 在此之前必须未链接到其他队列。 */
	spin_lock(&kernel_pgtable_work.lock);
	list_add(&pt->pt_list, &kernel_pgtable_work.list);
	spin_unlock(&kernel_pgtable_work.lock);

	/* 调度不等待释放完成；已在运行时返回 false 也会由当前/下一轮看到队列。 */
	schedule_work(&kernel_pgtable_work.work);
}
#endif
