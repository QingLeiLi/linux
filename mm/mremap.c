// SPDX-License-Identifier: GPL-2.0
/*
 *	mm/mremap.c
 *
 *	(C) Copyright 1996 Linus Torvalds
 *
 *	Address space accounting code	<alan@lxorguk.ukuu.org.uk>
 *	(C) Copyright 2002 Red Hat Inc, All Rights Reserved
 */
/*
 * 本文件实现 mremap() 的 VMA 改长、缩短、原地扩展与搬迁事务，并为 exec 栈搬迁复用页表移动器。
 * 核心难点是 mmap 写锁下协调 VMA 拆分/合并、页表锁、rmap、MMU notifier、内存计账和 userfaultfd。
 * 版权与作者行属于元数据；下文学习注释聚焦地址区间、资源所有权以及失败回滚边界。
 */

/* VMA、页表、hugetlb、KSM、swap、权限和文件接口组成 mremap 的主体状态机。 */
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/hugetlb.h>
#include <linux/shm.h>
#include <linux/ksm.h>
#include <linux/mman.h>
#include <linux/swap.h>
#include <linux/capability.h>
#include <linux/fs.h>
/* leafops/高端映射、安全检查、syscall、notifier 与 userfaultfd 支撑页表搬迁边界。 */
#include <linux/leafops.h>
#include <linux/highmem.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/mmu_notifier.h>
#include <linux/uaccess.h>
#include <linux/userfaultfd_k.h>
#include <linux/mempolicy.h>
#include <linux/pgalloc.h>

/* 架构 cache/TLB helper 保证旧虚拟地址不再残留可访问翻译。 */
#include <asm/cacheflush.h>
#include <asm/tlb.h>

#include "internal.h"

/* Classify the kind of remap operation being performed. */
/* remap 类型由对齐后的 old_len/new_len 决定，并驱动后续 no-op、缩短或扩展分支。 */
enum mremap_type {
	MREMAP_INVALID,		/* Initial state. */
	/* 尚未完成 VMA/长度检查的初始哨兵。 */
	MREMAP_NO_RESIZE,	/* old_len == new_len, if not moved, do nothing. */
	/* 长度相等；若又不要求搬迁则直接返回原地址。 */
	MREMAP_SHRINK,		/* old_len > new_len. */
	/* 释放源范围尾部。 */
	MREMAP_EXPAND,		/* old_len < new_len. */
	/* 原地延长或搬到更大空闲区。 */
};

/*
 * Describes a VMA mremap() operation and is threaded throughout it.
 *
 * Any of the fields may be mutated by the operation, however these values will
 * always accurately reflect the remap (for instance, we may adjust lengths and
 * delta to account for hugetlb alignment).
 */
/*
 * vma_remap_struct 是一次 mremap 的贯穿式事务对象。输入字段会因页对齐、hugetlb 粒度和批量 VMA
 * 搬迁而更新，但始终描述当前将执行/回滚的源、目标和长度；内部字段记录锁、计账、UFFD 与迭代器状态。
 */
struct vma_remap_struct {
	/* User-provided state. */
	/* 用户态输入；进入实际操作后可被规范化或按当前子 VMA 重写。 */
	unsigned long addr;	/* User-specified address from which we remap. */
	/* 当前源区间起点。 */
	unsigned long old_len;	/* Length of range being remapped. */
	/* 当前源区间长度。 */
	unsigned long new_len;	/* Desired new length of mapping. */
	/* 期望目标长度。 */
	const unsigned long flags; /* user-specified MREMAP_* flags. */
	/* 固定的 MREMAP_FIXED/MAYMOVE/DONTUNMAP 位。 */
	unsigned long new_addr;	/* Optionally, desired new address. */
	/* 用户指定或 get_unmapped_area() 选择的目标起点。 */

	/* uffd state. */
	/* UFFD remap 事件上下文及早期/普通 unmap 通知链，由 syscall 栈帧持有。 */
	struct vm_userfaultfd_ctx *uf;
	struct list_head *uf_unmap_early;
	struct list_head *uf_unmap;

	/* VMA state, determined in do_mremap(). */
	/* 当前源 VMA 借用指针；munmap/copy merge 后可能失效并被重新查找。 */
	struct vm_area_struct *vma;

	/* Internal state, determined in do_mremap(). */
	/* 内部派生状态只在对应阶段有效。 */
	unsigned long delta;		/* Absolute delta of old_len,new_len. */
	/* 新旧长度之差的绝对值。 */
	bool populate_expand;		/* mlock()'d expanded, must populate. */
	/* 锁定 VMA 扩展成功后需在解 mmap 锁后预填新增区。 */
	enum mremap_type remap_type;	/* expand, shrink, etc. */
	/* 当前 resize 分类。 */
	bool mmap_locked;		/* Is mm currently write-locked? */
	/* do_munmap 可主动放锁，因此显式记录最终是否仍需 unlock。 */
	unsigned long charged;		/* If VM_ACCOUNT, # pages to account. */
	/* 已通过 security_vm_enough_memory_mm() 预留、失败时需撤销的页数。 */
	bool vmi_needs_invalidate;	/* Is the VMA iterator invalidated? */
	/* copy/munmap 改树后通知批量遍历重置 maple iterator。 */
};

/*
 * get_old_pud() - 只读查找源地址现有的 PUD 表项。
 * @mm: mmap 写锁保护的地址空间；@addr: 源虚拟地址。
 * 返回：借用的非空 PUD 指针；任一上层缺失或坏项被清理时返回 NULL；不分配、不睡眠。
 */
static pud_t *get_old_pud(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	pgd = pgd_offset(mm, addr);
	/* 坏的顶层项由 helper 清除，并把该地址当作无源映射处理。 */
	if (pgd_none_or_clear_bad(pgd))
		return NULL;

	p4d = p4d_offset(pgd, addr);
	/* 折叠层 helper 仍返回可继续下钻的逻辑入口；缺失/坏项终止查找。 */
	if (p4d_none_or_clear_bad(p4d))
		return NULL;

	pud = pud_offset(p4d, addr);
	if (pud_none_or_clear_bad(pud))
		return NULL;

	return pud;
}

/*
 * get_old_pmd() - 只读查找源地址现有的 PMD 表项。
 * @mm/@addr: 同 get_old_pud()；返回借用的非空 PMD，路径缺失返回 NULL。
 * 上下文：调用者持 mmap 写锁，函数不取得页表锁，仅用于决定下一步搬迁层级。
 */
static pmd_t *get_old_pmd(struct mm_struct *mm, unsigned long addr)
{
	pud_t *pud;
	pmd_t *pmd;

	pud = get_old_pud(mm, addr);
	if (!pud)
		return NULL;

	/* get_old_pud 已验证上层，PMD none 表示本 extent 没有需要搬迁的下级映射。 */
	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return NULL;

	return pmd;
}

/*
 * alloc_new_pud() - 为目标地址补齐到 PUD 层的页表路径。
 * @mm: 目标地址空间；@addr: 目标虚拟地址。
 * 返回：借用的 PUD 指针，分配失败为 NULL；调用者持 mmap 写锁，分配可能失败。
 */
static pud_t *alloc_new_pud(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;

	pgd = pgd_offset(mm, addr);
	p4d = p4d_alloc(mm, pgd, addr);
	if (!p4d)
		return NULL;

	return pud_alloc(mm, p4d, addr);
}

/*
 * alloc_new_pmd() - 为目标地址补齐到 PMD 层的页表路径。
 * @mm/@addr: 目标地址空间与虚拟地址。
 * 返回：借用的普通 PMD 指针，分配失败为 NULL；新路径不得已是 THP PMD。
 */
static pmd_t *alloc_new_pmd(struct mm_struct *mm, unsigned long addr)
{
	pud_t *pud;
	pmd_t *pmd;

	pud = alloc_new_pud(mm, addr);
	if (!pud)
		return NULL;

	/* pmd_alloc 只补目标页表结构，不安装任何叶映射。 */
	pmd = pmd_alloc(mm, pud, addr);
	if (!pmd)
		return NULL;

	VM_BUG_ON(pmd_trans_huge(*pmd));

	return pmd;
}

/*
 * take_rmap_locks() - 按 file mapping 后 anon_vma 的固定顺序冻结反向映射观察。
 * @vma: 页表将被搬迁的源 VMA；返回无，存在的锁必须由 drop_rmap_locks() 配对释放。
 * 上下文：可睡眠，调用者已持 mmap 写锁。
 */
static void take_rmap_locks(struct vm_area_struct *vma)
{
	if (vma->vm_file)
		i_mmap_lock_write(vma->vm_file->f_mapping);
	if (vma->anon_vma)
		anon_vma_lock_write(vma->anon_vma);
}

/*
 * drop_rmap_locks() - 逆序释放 take_rmap_locks() 取得的 anon/file rmap 写锁。
 * @vma: 同一稳定源 VMA；返回无。
 */
static void drop_rmap_locks(struct vm_area_struct *vma)
{
	if (vma->anon_vma)
		anon_vma_unlock_write(vma->anon_vma);
	if (vma->vm_file)
		i_mmap_unlock_write(vma->vm_file->f_mapping);
}

/*
 * move_soft_dirty_pte() - 为被搬迁的 PTE/交换项设置 soft-dirty 可观测位。
 * @pte: 从旧地址取出的单个或批量首 PTE 值。
 * 返回：none 原样返回；架构支持时 present/swap 编码带 soft-dirty，ownership 不变。
 */
static pte_t move_soft_dirty_pte(pte_t pte)
{
	if (pte_none(pte))
		return pte;

	/*
	 * Set soft dirty bit so we can notice
	 * in userspace the ptes were moved.
	 */
	/* 搬迁本身应被用户态 soft-dirty 追踪视为映射变化，因此在新地址主动置位。 */
	if (pgtable_supports_soft_dirty()) {
		if (pte_present(pte))
			pte = pte_mksoft_dirty(pte);
		else
			pte = pte_swp_mksoft_dirty(pte);
	}

	return pte;
}

/*
 * mremap_folio_pte_batch() - 计算可按同一 large folio 属性批量搬迁的连续 PTE 数。
 * @vma/@addr/@ptep/@pte: 当前源映射；@max_nr: 本 extent 剩余上限。
 * 返回：至少 1，只有提示有收益且解析到 large folio 时才扩批；保持写权限边界，不取 folio 引用。
 */
static int mremap_folio_pte_batch(struct vm_area_struct *vma, unsigned long addr,
		pte_t *ptep, pte_t pte, int max_nr)
{
	struct folio *folio;

	if (max_nr == 1)
		return 1;

	/* Avoid expensive folio lookup if we stand no chance of benefit. */
	/* 页表提示只有单项可合并时，避免执行较昂贵的 folio/rmap 查询。 */
	if (pte_batch_hint(ptep, pte) == 1)
		return 1;

	folio = vm_normal_folio(vma, addr, pte);
	if (!folio || !folio_test_large(folio))
		return 1;

	return folio_pte_batch_flags(folio, NULL, ptep, &pte, max_nr, FPB_RESPECT_WRITE);
}

/*
 * move_ptes() - 在一个 PMD 范围内把源 PTE 批量转移到空目标页表。
 * @pmc: 持续推进的源/目标 VMA 与地址；@extent: 本轮字节数；@old_pmd/@new_pmd: 稳定层级入口。
 * 返回：成功为 0，临时无法映射 PTE 页返回 -EAGAIN 供上层重试。
 * 锁：mmap 写锁排除 VMA 并发；按需持 rmap 锁，同时持旧/新 PTL，离开前完成必要 TLB flush。
 */
static int move_ptes(struct pagetable_move_control *pmc,
		unsigned long extent, pmd_t *old_pmd, pmd_t *new_pmd)
{
	struct vm_area_struct *vma = pmc->old;
	bool need_clear_uffd_wp = vma_has_uffd_without_event_remap(vma);
	struct mm_struct *mm = vma->vm_mm;
	pte_t *old_ptep, *new_ptep;
	pte_t old_pte, pte;
	pmd_t dummy_pmdval;
	spinlock_t *old_ptl, *new_ptl;
	/* force_flush 记录是否清过 present PTE，决定释放 PTL 前是否必须失效旧 TLB。 */
	bool force_flush = false;
	unsigned long old_addr = pmc->old_addr;
	unsigned long new_addr = pmc->new_addr;
	unsigned long old_end = old_addr + extent;
	unsigned long len = old_end - old_addr;
	/* nr_ptes 是当前批次步长，max_nr_ptes 限制其不越过 extent。 */
	int max_nr_ptes;
	int nr_ptes;
	int err = 0;

	/*
	 * When need_rmap_locks is true, we take the i_mmap_rwsem and anon_vma
	 * locks to ensure that rmap will always observe either the old or the
	 * new ptes. This is the easiest way to avoid races with
	 * truncate_pagecache(), page migration, etc...
	 *
	 * When need_rmap_locks is false, we use other ways to avoid
	 * such races:
	 *
	 * - During exec() shift_arg_pages(), we use a specially tagged vma
	 *   which rmap call sites look for using vma_is_temporary_stack().
	 *
	 * - During mremap(), new_vma is often known to be placed after vma
	 *   in rmap traversal order. This ensures rmap will always observe
	 *   either the old pte, or the new pte, or both (the page table locks
	 *   serialize access to individual ptes, but only rmap traversal
	 *   order guarantees that we won't miss both the old and new ptes).
	 */
	/*
	 * need_rmap_locks 时以 i_mmap/anon_vma 锁保证 rmap 必见旧或新 PTE，排除 truncate 与 migration。
	 * exec 临时栈由特殊 VMA 标签保护；普通 mremap 常利用目标位于 rmap 遍历后方的顺序，再由 PTL
	 * 串行单项，使遍历至少看到一端，因而可省略全局 rmap 锁。
	 */
	if (pmc->need_rmap_locks)
		take_rmap_locks(vma);

	/*
	 * We don't have to worry about the ordering of src and dst
	 * pte locks because exclusive mmap_lock prevents deadlock.
	 */
	/* 独占 mmap_lock 保证没有另一搬迁者以相反顺序同时取得源/目标 PTL。 */
	old_ptep = pte_offset_map_lock(mm, old_pmd, old_addr, &old_ptl);
	if (!old_ptep) {
		err = -EAGAIN;
		goto out;
	}
	/*
	 * Now new_pte is none, so collapse_scan_file() path can not find
	 * this by traversing file->f_mapping, so there is no concurrency with
	 * retract_page_tables(). In addition, we already hold the exclusive
	 * mmap_lock, so this new_pte page is stable, so there is no need to get
	 * pmdval and do pmd_same() check.
	 */
	/*
	 * 目标 PTE 尚为空，file collapse 无法由 i_mmap 找到它；mmap 写锁又稳定目标页表页，
	 * 因此 nolock mapper 无需通过 pmdval/pmd_same() 防页表撤回。
	 */
	new_ptep = pte_offset_map_rw_nolock(mm, new_pmd, new_addr, &dummy_pmdval,
					   &new_ptl);
	if (!new_ptep) {
		pte_unmap_unlock(old_ptep, old_ptl);
		err = -EAGAIN;
		goto out;
	}
	if (new_ptl != old_ptl)
		spin_lock_nested(new_ptl, SINGLE_DEPTH_NESTING);
	/* 进入 lazy MMU 前先兑现旧的批量 flush，避免与本轮清项次序交叉。 */
	flush_tlb_batched_pending(vma->vm_mm);
	lazy_mmu_mode_enable();

	for (; old_addr < old_end; old_ptep += nr_ptes, old_addr += nr_ptes * PAGE_SIZE,
		new_ptep += nr_ptes, new_addr += nr_ptes * PAGE_SIZE) {
		/* 目标范围在 copy_vma/free_pgtables 后应完全为空，非空表示调用约束被破坏。 */
		VM_WARN_ON_ONCE(!pte_none(*new_ptep));

		nr_ptes = 1;
		max_nr_ptes = (old_end - old_addr) >> PAGE_SHIFT;
		old_pte = ptep_get(old_ptep);
		if (pte_none(old_pte))
			continue;

		/*
		 * If we are remapping a valid PTE, make sure
		 * to flush TLB before we drop the PTL for the
		 * PTE.
		 *
		 * NOTE! Both old and new PTL matter: the old one
		 * for racing with folio_mkclean(), the new one to
		 * make sure the physical page stays valid until
		 * the TLB entry for the old mapping has been
		 * flushed.
		 */
		/*
		 * present PTE 清除后必须在放任一 PTL 前 flush：旧锁与 folio_mkclean 竞争，新锁则保证物理页
		 * 在旧 TLB 项失效前仍由新映射稳定。large folio 可在权限一致边界内成批处理。
		 */
		if (pte_present(old_pte)) {
			nr_ptes = mremap_folio_pte_batch(vma, old_addr, old_ptep,
							 old_pte, max_nr_ptes);
			force_flush = true;
		}
		pte = get_and_clear_ptes(mm, old_addr, old_ptep, nr_ptes);
		/* 架构先修正地址相关 PTE 编码，再统一标记 soft-dirty。 */
		pte = move_pte(pte, old_addr, new_addr);
		pte = move_soft_dirty_pte(pte);

		if (need_clear_uffd_wp && pte_is_uffd_wp_marker(pte))
			/* 纯 marker 不代表物理映射；目标未继承 UFFD 时直接保持 none。 */
			pte_clear(mm, new_addr, new_ptep);
		else {
			if (need_clear_uffd_wp) {
				/* present 与 swap PTE 使用各自 helper 清 uffd-wp 编码。 */
				if (pte_present(pte))
					pte = pte_clear_uffd_wp(pte);
				else
					pte = pte_swp_clear_uffd_wp(pte);
			}
			set_ptes(mm, new_addr, new_ptep, pte, nr_ptes);
		}
	}

	lazy_mmu_mode_disable();
	/* 只要搬过 present PTE，就在放 PTL 前一次性失效完整旧 extent。 */
	if (force_flush)
		flush_tlb_range(vma, old_end - len, old_end);
	if (new_ptl != old_ptl)
		spin_unlock(new_ptl);
	/* 循环结束指针已越过最后批次，减一回到最后映射槽供 unmap helper 使用。 */
	pte_unmap(new_ptep - 1);
	pte_unmap_unlock(old_ptep - 1, old_ptl);
out:
	if (pmc->need_rmap_locks)
		drop_rmap_locks(vma);
	return err;
}

#ifndef arch_supports_page_table_move
#define arch_supports_page_table_move arch_supports_page_table_move
/*
 * arch_supports_page_table_move() - 缺少架构覆写时推导能否整层搬 PMD/PUD 页表。
 * 返回：任一对应配置启用为 true；编译期纯查询，无参数和副作用。
 */
static inline bool arch_supports_page_table_move(void)
{
	return IS_ENABLED(CONFIG_HAVE_MOVE_PMD) ||
		IS_ENABLED(CONFIG_HAVE_MOVE_PUD);
}
#endif

/*
 * uffd_supports_page_table_move() - 判断两端 UFFD 策略是否允许整层页表直接搬迁。
 * @pmc: 源/目标 VMA 事务；返回：两端都不会要求逐项清 uffd-wp 时为 true。
 * 原因：整层搬迁绕过 PTE 清位；失败回滚时 old/new 会互换，故必须同时检查两端。
 */
static inline bool uffd_supports_page_table_move(struct pagetable_move_control *pmc)
{
	/*
	 * If we are moving a VMA that has uffd-wp registered but with
	 * remap events disabled (new VMA will not be registered with uffd), we
	 * need to ensure that the uffd-wp state is cleared from all pgtables.
	 * This means recursing into lower page tables in move_page_tables().
	 *
	 * We might get called with VMAs reversed when recovering from a
	 * failed page table move. In that case, the
	 * "old"-but-actually-"originally new" VMA during recovery will not have
	 * a uffd context. Recursing into lower page tables during the original
	 * move but not during the recovery move will cause trouble, because we
	 * run into already-existing page tables. So check both VMAs.
	 */
	/*
	 * 源 VMA 注册 uffd-wp 但关闭 remap 事件时，新 VMA 不继承上下文，必须下钻逐项清除保护位。
	 * 回滚会交换两端身份；若正向下钻而回滚整层移动，会撞上已存在的下级页表，所以两端均须兼容。
	 */
	return !vma_has_uffd_without_event_remap(pmc->old) &&
	       !vma_has_uffd_without_event_remap(pmc->new);
}

#ifdef CONFIG_HAVE_MOVE_PMD
/*
 * move_normal_pmd() - 尝试把一个普通下级 PTE 页表整页从旧 PMD 接到新 PMD。
 * @pmc: 当前地址和 VMA；@old_pmd/@new_pmd: 源项及必须为空的目标项。
 * 返回：成功搬迁并 flush 为 true；能力/策略/目标冲突或源变成 leaf 时为 false，供调用者降级。
 * 锁：mmap 写锁在外层，函数嵌套取得旧/新 PMD PTL；不消费 pmc。
 */
static bool move_normal_pmd(struct pagetable_move_control *pmc,
			pmd_t *old_pmd, pmd_t *new_pmd)
{
	spinlock_t *old_ptl, *new_ptl;
	struct vm_area_struct *vma = pmc->old;
	struct mm_struct *mm = vma->vm_mm;
	bool res = false;
	pmd_t pmd;

	/* 架构整层能力和 UFFD 逐项清位需求任一不满足都必须下钻。 */
	if (!arch_supports_page_table_move())
		return false;
	if (!uffd_supports_page_table_move(pmc))
		return false;
	/*
	 * The destination pmd shouldn't be established, free_pgtables()
	 * should have released it.
	 *
	 * However, there's a case during execve() where we use mremap
	 * to move the initial stack, and in that case the target area
	 * may overlap the source area (always moving down).
	 *
	 * If everything is PMD-aligned, that works fine, as moving
	 * each pmd down will clear the source pmd. But if we first
	 * have a few 4kB-only pages that get moved down, and then
	 * hit the "now the rest is PMD-aligned, let's do everything
	 * one pmd at a time", we will still have the old (now empty
	 * of any 4kB pages, but still there) PMD in the page table
	 * tree.
	 *
	 * Warn on it once - because we really should try to figure
	 * out how to do this better - but then say "I won't move
	 * this pmd".
	 *
	 * One alternative might be to just unmap the target pmd at
	 * this point, and verify that it really is empty. We'll see.
	 */
	/*
	 * 正常目标 PMD 应由 free_pgtables() 清空；exec 向下搬栈可让源/目标重叠，开头若仅逐 4K 移动，
	 * 随后对齐处可能留下已空但仍存在的目标 PTE 页表。当前实现告警并降级，不冒险覆盖它。
	 */
	if (WARN_ON_ONCE(!pmd_none(*new_pmd)))
		return false;

	/*
	 * We don't have to worry about the ordering of src and dst
	 * ptlocks because exclusive mmap_lock prevents deadlock.
	 */
	/* mmap 写锁排除反向顺序的并发搬迁，因此不同 PTL 可按旧后新的嵌套层级取得。 */
	old_ptl = pmd_lock(mm, old_pmd);
	new_ptl = pmd_lockptr(mm, new_pmd);
	/* 两端可能共享同一 split page-table lock，只有不同时才嵌套取第二把。 */
	if (new_ptl != old_ptl)
		spin_lock_nested(new_ptl, SINGLE_DEPTH_NESTING);

	pmd = *old_pmd;

	/* Racing with collapse? */
	/* 若源 PMD 已被 collapse/leaf 转换，不再把它当普通 PTE 页表搬迁。 */
	if (unlikely(!pmd_present(pmd) || pmd_leaf(pmd)))
		goto out_unlock;
	/* Clear the pmd */
	/* 先从源断开页表页，再挂到已确认为空的目标，避免一张表同时由两端可达。 */
	pmd_clear(old_pmd);
	res = true;

	VM_BUG_ON(!pmd_none(*new_pmd));

	pmd_populate(mm, new_pmd, pmd_pgtable(pmd));
	/* 新端发布页表页后失效整个旧 PMD 范围，结束旧地址 CPU 可达性。 */
	flush_tlb_range(vma, pmc->old_addr, pmc->old_addr + PMD_SIZE);
out_unlock:
	if (new_ptl != old_ptl)
		spin_unlock(new_ptl);
	spin_unlock(old_ptl);

	return res;
}
#else
/* CONFIG_HAVE_MOVE_PMD 关闭时强制逐级下钻，不修改任何页表。 */
static inline bool move_normal_pmd(struct pagetable_move_control *pmc,
		pmd_t *old_pmd, pmd_t *new_pmd)
{
	return false;
}
#endif

#if CONFIG_PGTABLE_LEVELS > 2 && defined(CONFIG_HAVE_MOVE_PUD)
/*
 * move_normal_pud() - 尝试把一个普通下级 PMD 页表整页迁到空目标 PUD。
 * 参数/返回与 move_normal_pmd() 同义；成功清源、挂目标并 flush PUD 范围。
 * 调用者持 mmap 写锁，函数嵌套取得旧/新 PUD PTL；失败保持页表不变。
 */
static bool move_normal_pud(struct pagetable_move_control *pmc,
		pud_t *old_pud, pud_t *new_pud)
{
	spinlock_t *old_ptl, *new_ptl;
	struct vm_area_struct *vma = pmc->old;
	struct mm_struct *mm = vma->vm_mm;
	pud_t pud;

	if (!arch_supports_page_table_move())
		return false;
	/* UFFD 需要逐项状态转换时不能直接转接整个 PMD 页表。 */
	if (!uffd_supports_page_table_move(pmc))
		return false;
	/*
	 * The destination pud shouldn't be established, free_pgtables()
	 * should have released it.
	 */
	/* 整层搬迁只允许写入完全为空的目标 PUD，非空即告警并降级。 */
	if (WARN_ON_ONCE(!pud_none(*new_pud)))
		return false;

	/*
	 * We don't have to worry about the ordering of src and dst
	 * ptlocks because exclusive mmap_lock prevents deadlock.
	 */
	/* mmap 写锁排除其它 VMA 搬迁者，PTL 嵌套顺序不会形成 ABBA。 */
	old_ptl = pud_lock(mm, old_pud);
	new_ptl = pud_lockptr(mm, new_pud);
	if (new_ptl != old_ptl)
		spin_lock_nested(new_ptl, SINGLE_DEPTH_NESTING);

	/* Clear the pud */
	/* 从源摘除下级 PMD 页表页，再把同一页表 owner 转交目标 PUD。 */
	pud = *old_pud;
	pud_clear(old_pud);

	VM_BUG_ON(!pud_none(*new_pud));

	pud_populate(mm, new_pud, pud_pgtable(pud));
	/* 整层 owner 转移后刷新旧 PUD 虚拟范围。 */
	flush_tlb_range(vma, pmc->old_addr, pmc->old_addr + PUD_SIZE);
	if (new_ptl != old_ptl)
		spin_unlock(new_ptl);
	spin_unlock(old_ptl);

	return true;
}
#else
/* 页表层级不足或架构不支持 PUD 搬迁时返回 false，由上层降到 PMD/PTE。 */
static inline bool move_normal_pud(struct pagetable_move_control *pmc,
		pud_t *old_pud, pud_t *new_pud)
{
	return false;
}
#endif

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && defined(CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD)
/*
 * move_huge_pud() - 把一个 PUD THP leaf 原子移到空目标 PUD。
 * @pmc/@old_pud/@new_pud: 当前事务与两端项；成功返回 true，目标非空返回 false。
 * 锁：mmap 写锁在外层，函数同时持两端 PUD PTL；成功清源、设目标并 flush huge PUD TLB。
 */
static bool move_huge_pud(struct pagetable_move_control *pmc,
		pud_t *old_pud, pud_t *new_pud)
{
	spinlock_t *old_ptl, *new_ptl;
	struct vm_area_struct *vma = pmc->old;
	struct mm_struct *mm = vma->vm_mm;
	pud_t pud;

	/*
	 * The destination pud shouldn't be established, free_pgtables()
	 * should have released it.
	 */
	/* huge leaf 不能与目标既有页表合并，目标必须完全为空。 */
	if (WARN_ON_ONCE(!pud_none(*new_pud)))
		return false;

	/*
	 * We don't have to worry about the ordering of src and dst
	 * ptlocks because exclusive mmap_lock prevents deadlock.
	 */
	/* 独占 mmap_lock 保证同时取得两端 PTL 时不存在反向搬迁死锁。 */
	old_ptl = pud_lock(mm, old_pud);
	new_ptl = pud_lockptr(mm, new_pud);
	if (new_ptl != old_ptl)
		spin_lock_nested(new_ptl, SINGLE_DEPTH_NESTING);

	/* Clear the pud */
	/* 读取 leaf 后清源，保持物理大页只由新虚拟位置可达。 */
	pud = *old_pud;
	pud_clear(old_pud);

	VM_BUG_ON(!pud_none(*new_pud));

	/* Set the new pud */
	/* 把原 leaf 编码安装到新地址。 */
	/* mark soft_ditry when we add pud level soft dirty support */
	/* PUD soft-dirty 尚未支持；未来支持时应与 PTE 搬迁一样标记本次移动。 */
	set_pud_at(mm, pmc->new_addr, new_pud, pud);
	flush_pud_tlb_range(vma, pmc->old_addr, pmc->old_addr + HPAGE_PUD_SIZE);
	if (new_ptl != old_ptl)
		spin_unlock(new_ptl);
	spin_unlock(old_ptl);

	return true;
}
#else
/* 缺少 PUD THP 搬迁能力时，此分支不应被实际选择；告警并让上层降级/继续。 */
static bool move_huge_pud(struct pagetable_move_control *pmc,
		pud_t *old_pud, pud_t *new_pud)

{
	WARN_ON_ONCE(1);
	return false;

}
#endif

enum pgt_entry {
	/* 区分普通下级页表与 THP leaf，决定 extent 粒度及具体移动 helper。 */
	NORMAL_PMD,
	HPAGE_PMD,
	NORMAL_PUD,
	HPAGE_PUD,
};

/*
 * Returns an extent of the corresponding size for the pgt_entry specified if
 * valid. Else returns a smaller extent bounded by the end of the source and
 * destination pgt_entry.
 */
/*
 * get_extent() - 计算当前地址到指定 PMD/PUD 边界、源结束和目标边界三者中的最短距离。
 * @entry: 目标层级类型；@pmc: 当前 old/new 地址及 old_end。
 * 返回：本轮可安全处理的正字节数；即使地址加法溢出，减法和剩余范围裁剪仍给出有效 extent。
 */
static __always_inline unsigned long get_extent(enum pgt_entry entry,
						struct pagetable_move_control *pmc)
{
	unsigned long next, extent, mask, size;
	unsigned long old_addr = pmc->old_addr;
	unsigned long old_end = pmc->old_end;
	unsigned long new_addr = pmc->new_addr;

	/* 普通表与同层 huge leaf 使用相同边界 mask/size。 */
	switch (entry) {
	case HPAGE_PMD:
	case NORMAL_PMD:
		mask = PMD_MASK;
		size = PMD_SIZE;
		break;
	case HPAGE_PUD:
	case NORMAL_PUD:
		/* PUD 类型以 PUD_SIZE 为边界，不区分 leaf 与下级表。 */
		mask = PUD_MASK;
		size = PUD_SIZE;
		break;
	default:
		BUILD_BUG();
		break;
	}

	next = (old_addr + size) & mask;
	/* even if next overflowed, extent below will be ok */
	/* unsigned next 即使回绕，next-old_addr 仍表示到下一对齐边界的模运算距离。 */
	extent = next - old_addr;
	if (extent > old_end - old_addr)
		extent = old_end - old_addr;
	/* 再按目标下一边界裁剪，确保一次 helper 不跨两端任一页表项。 */
	next = (new_addr + size) & mask;
	if (extent > next - new_addr)
		extent = next - new_addr;
	return extent;
}

/*
 * Should move_pgt_entry() acquire the rmap locks? This is either expressed in
 * the PMC, or overridden in the case of normal, larger page tables.
 */
/*
 * should_take_rmap_locks() - 决定整层移动前是否必须冻结 rmap 遍历。
 * @pmc: 调用者默认策略；@entry: 将移动的层级类型。
 * 返回：普通 PMD/PUD 总为 true；huge leaf/PTE 路径沿用 pmc->need_rmap_locks。
 */
static bool should_take_rmap_locks(struct pagetable_move_control *pmc,
				   enum pgt_entry entry)
{
	switch (entry) {
	case NORMAL_PMD:
	case NORMAL_PUD:
		/* 普通整层表移动改变大量 rmap 可见 PTE，始终采用保守锁定。 */
		return true;
	default:
		return pmc->need_rmap_locks;
	}
}

/*
 * Attempts to speedup the move by moving entry at the level corresponding to
 * pgt_entry. Returns true if the move was successful, else false.
 */
/*
 * move_pgt_entry() - 分派一次 PMD/PUD 普通表或 huge leaf 的整层快速搬迁。
 * @pmc/@entry: 当前事务与类型；@old_entry/@new_entry: 对应层级的借用指针。
 * 返回：具体 helper 成功为 true，否则 false；按策略在分派外成对持 rmap 锁。
 */
static bool move_pgt_entry(struct pagetable_move_control *pmc,
			   enum pgt_entry entry, void *old_entry, void *new_entry)
{
	bool moved = false;
	bool need_rmap_locks = should_take_rmap_locks(pmc, entry);

	/* See comment in move_ptes() */
	/* 与 PTE 路径相同，rmap 锁使反向遍历不会同时错过旧、新位置。 */
	if (need_rmap_locks)
		take_rmap_locks(pmc->old);

	switch (entry) {
	case NORMAL_PMD:
		/* 普通 PMD/PUD helper 转移下级页表页；huge helper 转移 leaf。 */
		moved = move_normal_pmd(pmc, old_entry, new_entry);
		break;
	case NORMAL_PUD:
		moved = move_normal_pud(pmc, old_entry, new_entry);
		break;
	case HPAGE_PMD:
		/* 编译期无 THP 时短路为 false，调用者随后拆分/下钻。 */
		moved = IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) &&
			move_huge_pmd(pmc->old, pmc->old_addr, pmc->new_addr, old_entry,
				      new_entry);
		break;
	case HPAGE_PUD:
		/* PUD THP leaf 由本文件 helper 搬迁，能力关闭时表达式短路。 */
		moved = IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) &&
			move_huge_pud(pmc, old_entry, new_entry);
		break;

	default:
		WARN_ON_ONCE(1);
		break;
	}

	if (need_rmap_locks)
		/* 无论 helper 成败都在返回前配对释放 rmap 锁。 */
		drop_rmap_locks(pmc->old);

	return moved;
}

/*
 * A helper to check if aligning down is OK. The aligned address should fall
 * on *no mapping*. For the stack moving down, that's a special move within
 * the VMA that is created to span the source and destination of the move,
 * so we make an exception for it.
 */
/*
 * can_align_down() - 判断把源或目标起点向下对齐到更高页表边界是否不会覆盖其它映射。
 * @pmc: 含 exec 栈特例；@vma: 对应端 VMA；@addr_to_align: 当前起点；@mask: PMD/PUD mask。
 * 返回：普通路径仅 VMA 起点且前方空洞允许；栈内向下扩展可落在同一临时 VMA 内。
 */
static bool can_align_down(struct pagetable_move_control *pmc,
			   struct vm_area_struct *vma, unsigned long addr_to_align,
			   unsigned long mask)
{
	unsigned long addr_masked = addr_to_align & mask;

	/*
	 * If @addr_to_align of either source or destination is not the beginning
	 * of the corresponding VMA, we can't align down or we will destroy part
	 * of the current mapping.
	 */
	/* 普通 VMA 若从内部开始，向下对齐会把请求范围之前的有效映射一并搬走，必须拒绝。 */
	if (!pmc->for_stack && vma->vm_start != addr_to_align)
		return false;

	/* In the stack case we explicitly permit in-VMA alignment. */
	/* exec 临时栈 VMA 本就覆盖源与目标，向下落在其内部是专门允许的移动模型。 */
	if (pmc->for_stack && addr_masked >= vma->vm_start)
		return true;

	/*
	 * Make sure the realignment doesn't cause the address to fall on an
	 * existing mapping.
	 */
	/* 对齐新增的前缀必须完全是空洞，否则快速路径会破坏无关 VMA。 */
	return find_vma_intersection(vma->vm_mm, addr_masked, vma->vm_start) == NULL;
}

/*
 * Determine if are in fact able to realign for efficiency to a higher page
 * table boundary.
 */
/*
 * can_realign_addr() - 判断本次范围是否值得且能够把 old/new 同步向下对齐到页表边界。
 * @pmc: 页表移动游标；@pagetable_mask: PMD/PUD 对齐 mask。
 * 返回：范围跨过边界、两端偏移一致且新增前缀均安全时为 true；不修改 pmc。
 */
static bool can_realign_addr(struct pagetable_move_control *pmc,
			     unsigned long pagetable_mask)
{
	unsigned long align_mask = ~pagetable_mask;
	unsigned long old_align = pmc->old_addr & align_mask;
	unsigned long new_align = pmc->new_addr & align_mask;
	unsigned long pagetable_size = align_mask + 1;
	unsigned long old_align_next = pagetable_size - old_align;

	/*
	 * We don't want to have to go hunting for VMAs from the end of the old
	 * VMA to the next page table boundary, also we want to make sure the
	 * operation is worthwhile.
	 *
	 * So ensure that we only perform this realignment if the end of the
	 * range being copied reaches or crosses the page table boundary.
	 *
	 * boundary                        boundary
	 *    .<- old_align ->                .
	 *    .              |----------------.-----------|
	 *    .              |          vma   .           |
	 *    .              |----------------.-----------|
	 *    .              <----------------.----------->
	 *    .                          len_in
	 *    <------------------------------->
	 *    .         pagetable_size        .
	 *    .              <---------------->
	 *    .                old_align_next .
	 */
	/* 若输入长度连旧端下一边界都未触及，对齐只会增加工作量而没有整层搬迁收益。 */
	if (pmc->len_in < old_align_next)
		return false;

	/* Skip if the addresses are already aligned. */
	/* 已对齐无需调整。 */
	if (old_align == 0)
		return false;

	/* Only realign if the new and old addresses are mutually aligned. */
	/* 两端页内偏移必须相同，才能保持每个虚拟页的相对映射关系。 */
	if (old_align != new_align)
		return false;

	/* Ensure realignment doesn't cause overlap with existing mappings. */
	/* 分别验证源、目标新增前缀不覆盖请求范围外映射。 */
	if (!can_align_down(pmc, pmc->old, pmc->old_addr, pagetable_mask) ||
	    !can_align_down(pmc, pmc->new, pmc->new_addr, pagetable_mask))
		return false;

	return true;
}

/*
 * Opportunistically realign to specified boundary for faster copy.
 *
 * Consider an mremap() of a VMA with page table boundaries as below, and no
 * preceding VMAs from the lower page table boundary to the start of the VMA,
 * with the end of the range reaching or crossing the page table boundary.
 *
 *   boundary                        boundary
 *      .              |----------------.-----------|
 *      .              |          vma   .           |
 *      .              |----------------.-----------|
 *      .         pmc->old_addr         .      pmc->old_end
 *      .              <---------------------------->
 *      .                  move these page tables
 *
 * If we proceed with moving page tables in this scenario, we will have a lot of
 * work to do traversing old page tables and establishing new ones in the
 * destination across multiple lower level page tables.
 *
 * The idea here is simply to align pmc->old_addr, pmc->new_addr down to the
 * page table boundary, so we can simply copy a single page table entry for the
 * aligned portion of the VMA instead:
 *
 *   boundary                        boundary
 *      .              |----------------.-----------|
 *      .              |          vma   .           |
 *      .              |----------------.-----------|
 * pmc->old_addr                        .      pmc->old_end
 *      <------------------------------------------->
 *      .           move these page tables
 */
/*
 * try_realign_addr() - 尝试扩展移动起点，使中间部分可整层复制页表项。
 * @pmc: 原地更新的游标；@pagetable_mask: 目标边界。
 * 返回：无；不满足条件保持不变，成功只下调 old/new 起点而保持 old_end，因而扩大前缀不扩大尾端。
 */
static void try_realign_addr(struct pagetable_move_control *pmc,
			     unsigned long pagetable_mask)
{

	if (!can_realign_addr(pmc, pagetable_mask))
		return;

	/*
	 * Simply align to page table boundaries. Note that we do NOT update the
	 * pmc->old_end value, and since the move_page_tables() operation spans
	 * from [old_addr, old_end) (offsetting new_addr as it is performed),
	 * this simply changes the start of the copy, not the end.
	 */
	/* old_end 固定，两个起点同步下调，因此地址对应关系和原请求尾端均保持不变。 */
	pmc->old_addr &= pagetable_mask;
	pmc->new_addr &= pagetable_mask;
}

/* Is the page table move operation done? */
/* pmc_done() 在旧游标到达半开区间 old_end 时结束；纯查询，不改变事务。 */
static bool pmc_done(struct pagetable_move_control *pmc)
{
	return pmc->old_addr >= pmc->old_end;
}

/* Advance to the next page table, offset by extent bytes. */
/* pmc_next() 同步推进 old/new 游标，保持两端偏移关系；extent 来自 get_extent()。 */
static void pmc_next(struct pagetable_move_control *pmc, unsigned long extent)
{
	pmc->old_addr += extent;
	pmc->new_addr += extent;
}

/*
 * Determine how many bytes in the specified input range have had their page
 * tables moved so far.
 */
/*
 * pmc_progress() - 把可能向下 realign 的内部游标换算回原始输入范围的已完成字节数。
 * @pmc: 移动后的游标；返回：不小于 0 的原请求进度，首个对齐块失败时钳为 0。
 */
static unsigned long pmc_progress(struct pagetable_move_control *pmc)
{
	unsigned long orig_old_addr = pmc->old_end - pmc->len_in;
	unsigned long old_addr = pmc->old_addr;

	/*
	 * Prevent negative return values when {old,new}_addr was realigned but
	 * we broke out of the loop in move_page_tables() for the first PMD
	 * itself.
	 */
	/* realign 后若首个 PMD 就失败，old_addr 仍可早于原起点，不能让 unsigned 减法回绕。 */
	return old_addr < orig_old_addr ? 0 : old_addr - orig_old_addr;
}

/*
 * move_page_tables() - 把 pmc 输入范围的页表映射从旧 VMA 搬到新 VMA，并返回实际进度。
 * @pmc: 调用者拥有的可变游标；old/new VMA 与地址、len_in 必须已初始化，目标范围为空。
 * 返回：完成的原始输入字节数；0 可表示无输入或首段失败，短值由调用者视为可回滚失败。
 * 上下文：持 mmap 写锁；发 MMU notifier，可能分配目标页表/调度，按 PUD→PMD→PTE 降级。
 */
unsigned long move_page_tables(struct pagetable_move_control *pmc)
{
	unsigned long extent;
	struct mmu_notifier_range range;
	pmd_t *old_pmd, *new_pmd;
	pud_t *old_pud, *new_pud;
	struct mm_struct *mm = pmc->old->vm_mm;

	if (!pmc->len_in)
		return 0;

	if (is_vm_hugetlb_page(pmc->old))
		/* hugetlb 使用 reservation-aware 专用搬迁器，不进入普通页表层级算法。 */
		return move_hugetlb_page_tables(pmc->old, pmc->new, pmc->old_addr,
						pmc->new_addr, pmc->len_in);

	/*
	 * If possible, realign addresses to PMD boundary for faster copy.
	 * Only realign if the mremap copying hits a PMD boundary.
	 */
	/* 只有复制范围跨 PMD 边界时才扩展前缀，以换取整 PMD 搬迁收益。 */
	try_realign_addr(pmc, PMD_MASK);

	flush_cache_range(pmc->old, pmc->old_addr, pmc->old_end);
	/* notifier start/end 包住所有页表可达性变化，设备页表观察到统一 UNMAP 区间。 */
	mmu_notifier_range_init(&range, MMU_NOTIFY_UNMAP, 0, mm,
				pmc->old_addr, pmc->old_end);
	mmu_notifier_invalidate_range_start(&range);

	for (; !pmc_done(pmc); pmc_next(pmc, extent)) {
		cond_resched();
		/*
		 * If extent is PUD-sized try to speed up the move by moving at the
		 * PUD level if possible.
		 */
		/* 每轮先按 PUD 边界裁剪；源无映射直接跳过，目标分配失败则报告短进度。 */
		extent = get_extent(NORMAL_PUD, pmc);

		old_pud = get_old_pud(mm, pmc->old_addr);
		if (!old_pud)
			continue;
		new_pud = alloc_new_pud(mm, pmc->new_addr);
		if (!new_pud)
			break;
		if (pud_trans_huge(*old_pud)) {
			if (extent == HPAGE_PUD_SIZE) {
				move_pgt_entry(pmc, HPAGE_PUD, old_pud, new_pud);
				/* We ignore and continue on error? */
				/* huge PUD helper 失败也继续本 extent；该分支保留现有行为，不能误称已降级复制。 */
				continue;
			}
		} else if (IS_ENABLED(CONFIG_HAVE_MOVE_PUD) && extent == PUD_SIZE) {
			/* 非 leaf 且恰好完整 PUD 时尝试整张下级 PMD 页表搬迁。 */
			if (move_pgt_entry(pmc, NORMAL_PUD, old_pud, new_pud))
				continue;
		}

		extent = get_extent(NORMAL_PMD, pmc);
		/* PUD 快路未完成时，把本轮重新裁成不跨 PMD 的范围。 */
		old_pmd = get_old_pmd(mm, pmc->old_addr);
		if (!old_pmd)
			continue;
		new_pmd = alloc_new_pmd(mm, pmc->new_addr);
		if (!new_pmd)
			break;
again:
		if (pmd_is_huge(*old_pmd)) {
			/* 完整 huge PMD 优先整 leaf 搬迁，否则先 split 后走普通/PTE 路径。 */
			if (extent == HPAGE_PMD_SIZE &&
			    move_pgt_entry(pmc, HPAGE_PMD, old_pmd, new_pmd))
				continue;
			split_huge_pmd(pmc->old, old_pmd, pmc->old_addr);
		} else if (IS_ENABLED(CONFIG_HAVE_MOVE_PMD) &&
			   extent == PMD_SIZE) {
			/*
			 * If the extent is PMD-sized, try to speed the move by
			 * moving at the PMD level if possible.
			 */
			/* 完整 PMD 范围优先转接整张 PTE 页表，失败再下钻。 */
			if (move_pgt_entry(pmc, NORMAL_PMD, old_pmd, new_pmd))
				continue;
		}
		if (pmd_none(*old_pmd))
			continue;
		/* 目标 PTE 页分配失败产生短进度；映射竞争返回 EAGAIN 则从 PMD 状态重新判定。 */
		if (pte_alloc(pmc->new->vm_mm, new_pmd))
			break;
		if (move_ptes(pmc, extent, old_pmd, new_pmd) < 0)
			/* PTE 映射临时失败时重新检查 PMD，兼容并发架构页表状态变化。 */
			goto again;
	}

	mmu_notifier_invalidate_range_end(&range);

	return pmc_progress(pmc);
}

/* Set vrm->delta to the difference in VMA size specified by user. */
/* vrm_set_delta() 计算规范化新旧长度的绝对差；只更新 vrm->delta，无失败。 */
static void vrm_set_delta(struct vma_remap_struct *vrm)
{
	vrm->delta = abs_diff(vrm->old_len, vrm->new_len);
}

/* Determine what kind of remap this is - shrink, expand or no resize at all. */
/* vrm_remap_type() 根据 delta 与长度方向返回 NO_RESIZE/SHRINK/EXPAND，不修改 vrm。 */
static enum mremap_type vrm_remap_type(struct vma_remap_struct *vrm)
{
	if (vrm->delta == 0)
		return MREMAP_NO_RESIZE;

	if (vrm->old_len > vrm->new_len)
		return MREMAP_SHRINK;

	return MREMAP_EXPAND;
}

/*
 * When moving a VMA to vrm->new_adr, does this result in the new and old VMAs
 * overlapping?
 */
/*
 * vrm_overlaps() - 判断源、目标半开区间是否有非空交集。
 * @vrm: 参数已完成溢出/范围检查的事务；返回相交为 true，仅做地址比较。
 */
static bool vrm_overlaps(struct vma_remap_struct *vrm)
{
	unsigned long start_old = vrm->addr;
	unsigned long start_new = vrm->new_addr;
	unsigned long end_old = vrm->addr + vrm->old_len;
	unsigned long end_new = vrm->new_addr + vrm->new_len;

	/*
	 * start_old    end_old
	 *     |-----------|
	 *     |           |
	 *     |-----------|
	 *             |-------------|
	 *             |             |
	 *             |-------------|
	 *         start_new      end_new
	 */
	/* 两个半开区间相交当且仅当各自终点都严格越过另一端起点。 */
	if (end_old > start_new && end_new > start_old)
		return true;

	return false;
}

/*
 * Will a new address definitely be assigned? This either if the user specifies
 * it via MREMAP_FIXED, or if MREMAP_DONTUNMAP is used, indicating we will
 * always determine a target address.
 */
/*
 * vrm_implies_new_addr() - 判断 flags 是否保证操作包含目标地址选择/搬迁。
 * 返回：FIXED 或 DONTUNMAP 任一设置为 true；纯查询。
 */
static bool vrm_implies_new_addr(struct vma_remap_struct *vrm)
{
	return vrm->flags & (MREMAP_FIXED | MREMAP_DONTUNMAP);
}

/*
 * Find an unmapped area for the requested vrm->new_addr.
 *
 * If MREMAP_FIXED then this is equivalent to a MAP_FIXED mmap() call. If only
 * MREMAP_DONTUNMAP is set, then this is equivalent to providing a hint to
 * mmap(), otherwise this is equivalent to mmap() specifying a NULL address.
 *
 * Returns 0 on success (with vrm->new_addr updated), or an error code upon
 * failure.
 */
/*
 * vrm_set_new_addr() - 按 FIXED、DONTUNMAP hint 或普通 NULL hint 选择目标空闲区。
 * @vrm: 已绑定源 VMA 的事务；成功更新 new_addr 并返回 0，失败返回 get_unmapped_area() errno。
 * pgoff 保留请求起点在源 VMA 内的文件偏移；不创建 VMA，只预选地址。
 */
static unsigned long vrm_set_new_addr(struct vma_remap_struct *vrm)
{
	struct vm_area_struct *vma = vrm->vma;
	unsigned long map_flags = 0;
	/* Page Offset _into_ the VMA. */
	/* 请求可从 VMA 中部开始，文件映射目标 pgoff 必须加上这段内部页偏移。 */
	pgoff_t internal_pgoff = (vrm->addr - vma->vm_start) >> PAGE_SHIFT;
	pgoff_t pgoff = vma->vm_pgoff + internal_pgoff;
	unsigned long new_addr = vrm_implies_new_addr(vrm) ? vrm->new_addr : 0;
	unsigned long res;

	/* FIXED 要求精确地址；共享 VMA 把 MAP_SHARED 语义传给文件地址选择器。 */
	if (vrm->flags & MREMAP_FIXED)
		map_flags |= MAP_FIXED;
	if (vma->vm_flags & VM_MAYSHARE)
		map_flags |= MAP_SHARED;

	res = get_unmapped_area(vma->vm_file, new_addr, vrm->new_len, pgoff,
				map_flags);
	/* helper 返回地址或编码 errno；成功地址在后续 move 前写回事务。 */
	if (IS_ERR_VALUE(res))
		return res;

	vrm->new_addr = res;
	return 0;
}

/*
 * Keep track of pages which have been added to the memory mapping. If the VMA
 * is accounted, also check to see if there is sufficient memory.
 *
 * Returns true on success, false if insufficient memory to charge.
 */
/*
 * vrm_calc_charge() - 为 VM_ACCOUNT 操作预留新增 commit，并记录可回滚页数。
 * @vrm: 已完成类型/长度检查的事务。
 * 返回：无需计账或预留成功为 true，commit limit 拒绝为 false；成功量写入 vrm->charged。
 */
static bool vrm_calc_charge(struct vma_remap_struct *vrm)
{
	unsigned long charged;

	if (!(vrm->vma->vm_flags & VM_ACCOUNT))
		return true;

	/*
	 * If we don't unmap the old mapping, then we account the entirety of
	 * the length of the new one. Otherwise it's just the delta in size.
	 */
	/* DONTUNMAP 保留旧映射，目标全长都是新增承诺；真正 move 只新增尺寸差额。 */
	if (vrm->flags & MREMAP_DONTUNMAP)
		charged = vrm->new_len >> PAGE_SHIFT;
	else
		charged = vrm->delta >> PAGE_SHIFT;


	/* This accounts 'charged' pages of memory. */
	/* security helper 成功即已计入 commit，后续失败必须由 vrm_uncharge() 撤销。 */
	if (security_vm_enough_memory_mm(current->mm, charged))
		return false;

	vrm->charged = charged;
	return true;
}

/*
 * an error has occurred so we will not be using vrm->charged memory. Unaccount
 * this memory if the VMA is accounted.
 */
/*
 * vrm_uncharge() - 放弃尚未转化为最终映射的 commit 预留。
 * @vrm: 可能已记录 charged 的事务；返回无，VM_ACCOUNT 时撤销并清零，便于幂等收口。
 */
static void vrm_uncharge(struct vma_remap_struct *vrm)
{
	if (!(vrm->vma->vm_flags & VM_ACCOUNT))
		return;

	vm_unacct_memory(vrm->charged);
	vrm->charged = 0;
}

/*
 * Update mm exec_vm, stack_vm, data_vm, and locked_vm fields as needed to
 * account for 'bytes' memory used, and if locked, indicate this in the VRM so
 * we can handle this correctly later.
 */
/*
 * vrm_stat_account() - 把成功新增的虚拟页计入 mm 分类统计及 locked_vm。
 * @vrm: 含最终 VMA flags；@bytes: 本次新增/复制目标字节数。
 * 返回：无；只更新统计，commit 预留已由 vrm_calc_charge() 完成。
 */
static void vrm_stat_account(struct vma_remap_struct *vrm,
			     unsigned long bytes)
{
	unsigned long pages = bytes >> PAGE_SHIFT;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma = vrm->vma;

	vm_stat_account(mm, vma->vm_flags, pages);
	if (vma->vm_flags & VM_LOCKED)
		mm->locked_vm += pages;
}

/*
 * __check_map_count_against_split() - 为接下来的 unmap/copy 最坏 VMA 拆分预留 map_count 余量。
 * @mm: 已持 mmap 写锁；@before_unmaps: true 时再计入前置 shrink/FIXED unmap 的两个临时 VMA。
 * 返回：整个事务最坏还不超过 sysctl_max_map_count 为 true；不改变 VMA 树。
 */
static bool __check_map_count_against_split(struct mm_struct *mm,
					    bool before_unmaps)
{
	const int sys_map_count = get_sysctl_max_map_count();
	int map_count = mm->map_count;

	mmap_assert_write_locked(mm);

	/*
	 * At the point of shrinking the VMA, if new_len < old_len, we unmap
	 * thusly in the worst case:
	 *
	 *              old_addr+old_len                    old_addr+old_len
	 * |---------------.----.---------|    |---------------|    |---------|
	 * |               .    .         | -> |      +1       | -1 |   +1    |
	 * |---------------.----.---------|    |---------------|    |---------|
	 *        old_addr+new_len                     old_addr+new_len
	 *
	 * At the point of removing the portion of an existing VMA to make space
	 * for the moved VMA if MREMAP_FIXED, we unmap thusly in the worst case:
	 *
	 *   new_addr   new_addr+new_len         new_addr   new_addr+new_len
	 * |----.---------------.---------|    |----|               |---------|
	 * |    .               .         | -> | +1 |      -1       |   +1    |
	 * |----.---------------.---------|    |----|               |---------|
	 *
	 * Therefore, before we consider the move anything, we have to account
	 * for 2 additional VMAs possibly being created upon these unmappings.
	 */
	/* shrink 或 FIXED 清目标都可能把一个 VMA 切成左右两段，净增最多 1；两步合计先预留 2。 */
	if (before_unmaps)
		map_count += 2;

	/*
	 * At the point of MOVING the VMA:
	 *
	 * We start by copying a VMA, which creates an additional VMA if no
	 * merge occurs, then if not MREMAP_DONTUNMAP, we unmap the source VMA.
	 * In the worst case we might then observe:
	 *
	 *   new_addr   new_addr+new_len         new_addr   new_addr+new_len
	 * |----|               |---------|    |----|---------------|---------|
	 * |    |               |         | -> |    |      +1       |         |
	 * |----|               |---------|    |----|---------------|---------|
	 *
	 *   old_addr   old_addr+old_len         old_addr   old_addr+old_len
	 * |----.---------------.---------|    |----|               |---------|
	 * |    .               .         | -> | +1 |      -1       |   +1    |
	 * |----.---------------.---------|    |----|               |---------|
	 *
	 * Therefore we must check to ensure we have headroom of 2 additional
	 * VMAs.
	 */
	/* copy 目标最坏新增 1，随后 unmap 源最坏净增 1，移动阶段还需固定保留两个槽位。 */
	return map_count + 2 <= sys_map_count;
}

/* Do we violate the map count limit if we split VMAs when moving the VMA? */
/* check_map_count_against_split() 检查移动阶段本身的两份余量；要求已持 mmap 写锁。 */
static bool check_map_count_against_split(void)
{
	return __check_map_count_against_split(current->mm,
					       /*before_unmaps=*/false);
	/* before_unmaps=false 表示这里只计移动阶段本身的拆分余量。 */
}

/* Do we violate the map count limit if we split VMAs prior to early unmaps? */
/* check_map_count_against_split_early() 连同前置 unmap 一起按最坏情况检查四份余量。 */
static bool check_map_count_against_split_early(void)
{
	return __check_map_count_against_split(current->mm,
					       /*before_unmaps=*/true);
	/* before_unmaps=true 还要计入 shrink/FIXED 清目标的前置拆分。 */
}

/*
 * Perform checks before attempting to write a VMA prior to it being
 * moved.
 */
/*
 * prep_move_vma() - 在复制 VMA/页表前验证 map_count、文件拆分回调并解除源范围 KSM 合并。
 * @vrm: 已绑定且稳定的源 VMA 事务。
 * 返回：通过为 0；余量不足 -ENOMEM，may_split/KSM 返回其 errno；失败未创建目标映射。
 * 上下文：持 mmap 写锁，可调用文件 vm_ops；dummy flags 保证 KSM 检查不永久改 VMA 策略。
 */
static unsigned long prep_move_vma(struct vma_remap_struct *vrm)
{
	unsigned long err = 0;
	struct vm_area_struct *vma = vrm->vma;
	unsigned long old_addr = vrm->addr;
	unsigned long old_len = vrm->old_len;
	vm_flags_t dummy = vma->vm_flags;

	/*
	 * We'd prefer to avoid failure later on in do_munmap: we copy a VMA,
	 * which may not merge, then (if MREMAP_DONTUNMAP is not set) unmap the
	 * source, which may split, causing a net increase of 2 mappings.
	 */
	/* 提前拒绝可避免 copy 已成功后才因源 munmap 需要拆分而无法收口。 */
	if (!check_map_count_against_split())
		return -ENOMEM;

	if (vma->vm_ops && vma->vm_ops->may_split) {
		if (vma->vm_start != old_addr)
			err = vma->vm_ops->may_split(vma, old_addr);
		if (!err && vma->vm_end != old_addr + old_len)
			err = vma->vm_ops->may_split(vma, old_addr + old_len);
		if (err)
			return err;
	}

	/*
	 * Advise KSM to break any KSM pages in the area to be moved:
	 * it would be confusing if they were to turn up at the new
	 * location, where they happen to coincide with different KSM
	 * pages recently unmapped.  But leave vma->vm_flags as it was,
	 * so KSM can come around to merge on vma and new_vma afterwards.
	 */
	/*
	 * 搬迁前先拆掉范围内 KSM 共享页，避免新地址恰与刚解除的其它 KSM 映射混淆；只用 dummy flags
	 * 请求 UNMERGEABLE，保留 VMA 原有可合并标志，使之后源/目标仍可被 KSM 再次扫描合并。
	 */
	err = ksm_madvise(vma, old_addr, old_addr + old_len,
			  MADV_UNMERGEABLE, &dummy);
	if (err)
		return err;

	return 0;
}

/*
 * Unmap source VMA for VMA move, turning it from a copy to a move, being
 * careful to ensure we do not underflow memory account while doing so if an
 * accountable move.
 *
 * This is best effort, if we fail to unmap then we simply try to correct
 * accounting and exit.
 */
/*
 * unmap_source_vma() - 删除一次成功 copy 后不再保留的源范围，完成“复制”到“移动”的提交。
 * @vrm: vma 指向当前应删除的一端；可能是正常源，也可能是 copy 失败后改写成的新目标。
 * 返回：无；总会使 vrm->vma 失效并标记 iterator，munmap OOM 时只修正 commit 计账后退出。
 * 计账：真正 move 临时清 VM_ACCOUNT 防重复 uncharge，DONTUNMAP 回滚删除新端时则允许正常 uncharge。
 */
static void unmap_source_vma(struct vma_remap_struct *vrm)
{
	struct mm_struct *mm = current->mm;
	unsigned long addr = vrm->addr;
	unsigned long len = vrm->old_len;
	struct vm_area_struct *vma = vrm->vma;
	VMA_ITERATOR(vmi, mm, addr);
	int err;
	unsigned long vm_start;
	unsigned long vm_end;
	/*
	 * It might seem odd that we check for MREMAP_DONTUNMAP here, given this
	 * function implies that we unmap the original VMA, which seems
	 * contradictory.
	 *
	 * However, this occurs when this operation was attempted and an error
	 * arose, in which case we _do_ wish to unmap the _new_ VMA, which means
	 * we actually _do_ want it be unaccounted.
	 */
	/*
	 * 名称虽是 unmap source，copy 失败时 vrm 已切到新 VMA，本函数实际删目标；此时 DONTUNMAP 原源
	 * 仍存在，必须让新端的 VM_ACCOUNT 正常撤账，所以 accountable_move 特意排除 DONTUNMAP。
	 */
	bool accountable_move = (vma->vm_flags & VM_ACCOUNT) &&
		!(vrm->flags & MREMAP_DONTUNMAP);

	/*
	 * So we perform a trick here to prevent incorrect accounting. Any merge
	 * or new VMA allocation performed in copy_vma() does not adjust
	 * accounting, it is expected that callers handle this.
	 *
	 * And indeed we already have, accounting appropriately in the case of
	 * both in vrm_charge().
	 *
	 * However, when we unmap the existing VMA (to effect the move), this
	 * code will, if the VMA has VM_ACCOUNT set, attempt to unaccount
	 * removed pages.
	 *
	 * To avoid this we temporarily clear this flag, reinstating on any
	 * portions of the original VMA that remain.
	 */
	/*
	 * copy_vma() 不自行调整 commit，调用者已预先计账；正常 move 删除源若保留 VM_ACCOUNT 会再次
	 * uncharge。故删除前临时清位，并在 munmap 可能留下的左右残段上恢复该 flag。
	 */
	if (accountable_move) {
		vm_flags_clear(vma, VM_ACCOUNT);
		/* We are about to split vma, so store the start/end. */
		/* munmap 后原 vma 指针失效，提前保存边界以判断左右残段。 */
		vm_start = vma->vm_start;
		vm_end = vma->vm_end;
	}

	err = do_vmi_munmap(&vmi, mm, addr, len, vrm->uf_unmap, /* unlock= */false);
	/* unlock=false：本函数后面还要在同一 mmap 写锁周期内修复残段属性。 */
	vrm->vma = NULL; /* Invalidated. */
	/* 旧 VMA 指针已经失效。 */
	/* VMA tree 已可能拆分/合并，旧指针与外部 iterator 都不可继续使用。 */
	vrm->vmi_needs_invalidate = true;
	if (err) {
		/* OOM: unable to split vma, just get accounts right */
		/* munmap 未完成却已临时阻止撤账，补回目标长度的 commit 使总账与现存映射一致。 */
		vm_acct_memory(len >> PAGE_SHIFT);
		return;
	}

	/*
	 * If we mremap() from a VMA like this:
	 *
	 *    addr  end
	 *     |     |
	 *     v     v
	 * |-------------|
	 * |             |
	 * |-------------|
	 *
	 * Having cleared VM_ACCOUNT from the whole VMA, after we unmap above
	 * we'll end up with:
	 *
	 *    addr  end
	 *     |     |
	 *     v     v
	 * |---|     |---|
	 * | A |     | B |
	 * |---|     |---|
	 *
	 * The VMI is still pointing at addr, so vma_prev() will give us A, and
	 * a subsequent or lone vma_next() will give as B.
	 *
	 * do_vmi_munmap() will have restored the VMI back to addr.
	 */
	/*
	 * 若删除的是 VMA 中段，临时清掉整段 VM_ACCOUNT 后会留下 A/B 两个残段。munmap 把 iterator
	 * 复位到 addr，因此 prev 对应左段、next 对应右段；分别在确实存在时恢复计账属性。
	 */
	if (accountable_move) {
		unsigned long end = addr + len;

		if (vm_start < addr) {
			struct vm_area_struct *prev = vma_prev(&vmi);

			vm_flags_set(prev, VM_ACCOUNT); /* Acquires VMA lock. */
			/* 修改 flags 同时取得相应 VMA 写锁，避免无锁发布属性。 */
		}

		if (vm_end > end) {
			struct vm_area_struct *next = vma_next(&vmi);

			vm_flags_set(next, VM_ACCOUNT); /* Acquires VMA lock. */
			/* 右残段同样恢复 VM_ACCOUNT，并由 helper 处理 VMA 锁。 */
		}
	}
}

/*
 * Copy vrm->vma over to vrm->new_addr possibly adjusting size as part of the
 * process. Additionally handle an error occurring on moving of page tables,
 * where we reset vrm state to cause unmapping of the new VMA.
 *
 * Outputs the newly installed VMA to new_vma_ptr. Returns 0 on success or an
 * error code.
 */
/*
 * copy_vma_and_data() - 在目标建立 VMA、搬页表并调用文件 mremap 回调，失败时把页表搬回。
 * @vrm: 已预留 commit 且源 VMA 稳定的事务；@new_vma_ptr: 始终输出新建 VMA或 NULL。
 * 返回：成功 0；建 VMA/短搬迁/文件回调失败返回 errno。若目标已建，失败会改写 vrm 使上层删除目标。
 * 上下文：持 mmap 写锁；页表搬迁可分配/调度，成功后准备 UFFD remap 事件。
 */
static int copy_vma_and_data(struct vma_remap_struct *vrm,
			     struct vm_area_struct **new_vma_ptr)
{
	unsigned long internal_offset = vrm->addr - vrm->vma->vm_start;
	unsigned long internal_pgoff = internal_offset >> PAGE_SHIFT;
	unsigned long new_pgoff = vrm->vma->vm_pgoff + internal_pgoff;
	unsigned long moved_len;
	struct vm_area_struct *vma = vrm->vma;
	struct vm_area_struct *new_vma;
	/* err 在目标已建立后也可能来自短页表搬迁或文件系统 mremap 回调。 */
	int err = 0;
	PAGETABLE_MOVE(pmc, NULL, NULL, vrm->addr, vrm->new_addr, vrm->old_len);

	new_vma = copy_vma(&vma, vrm->new_addr, vrm->new_len, new_pgoff,
			   &pmc.need_rmap_locks);
	/* copy_vma 只复制/合并 VMA 元数据并给出 rmap 锁需求，页表数据尚未搬迁。 */
	if (!new_vma) {
		vrm_uncharge(vrm);
		*new_vma_ptr = NULL;
		return -ENOMEM;
	}
	/* By merging, we may have invalidated any iterator in use. */
	/* copy_vma 可把源 VMA 合并/替换为新对象；指针变化说明外部 maple iterator 必须失效重建。 */
	if (vma != vrm->vma)
		vrm->vmi_needs_invalidate = true;

	vrm->vma = vma;
	pmc.old = vma;
	pmc.new = new_vma;

	/* 只有完整 old_len 搬迁才可调用 VMA 专用回调；短进度进入反向恢复。 */
	moved_len = move_page_tables(&pmc);
	if (moved_len < vrm->old_len)
		err = -ENOMEM;
	else if (vma->vm_ops && vma->vm_ops->mremap)
		err = vma->vm_ops->mremap(new_vma);

	if (unlikely(err)) {
		PAGETABLE_MOVE(pmc_revert, new_vma, vma, vrm->new_addr,
			       vrm->addr, moved_len);

		/*
		 * On error, move entries back from new area to old,
		 * which will succeed since page tables still there,
		 * and then proceed to unmap new area instead of old.
		 */
		/*
		 * 目标已建立后的任何错误都把已移动前缀反向搬回；旧页表页仍在，回滚应可完成。随后把 vrm
		 * 的“源”改成新 VMA/新范围，让 move_vma() 统一 unmap 它而保留原映射。
		 */
		pmc_revert.need_rmap_locks = true;
		/* 回滚不能依赖正向 VMA 遍历顺序，强制 rmap 锁确保任何遍历都不漏映射。 */
		move_page_tables(&pmc_revert);

		vrm->vma = new_vma;
		vrm->old_len = vrm->new_len;
		vrm->addr = vrm->new_addr;
	} else {
		/* 成功时记录 UFFD remap 关系，实际 complete 通知在 mmap 锁外发送。 */
		mremap_userfaultfd_prep(new_vma, vrm->uf);
	}

	fixup_hugetlb_reservations(vma);

	*new_vma_ptr = new_vma;
	return err;
}

/*
 * Perform final tasks for MADV_DONTUNMAP operation, clearing mlock() flag on
 * remaining VMA by convention (it cannot be mlock()'d any longer, as pages in
 * range are no longer mapped), and removing anon_vma_chain links from it if the
 * entire VMA was copied over.
 */
/*
 * dontunmap_complete() - 完成 DONTUNMAP 成功后的旧 VMA 清理而不解除其地址范围。
 * @vrm: 仍指向旧 VMA；@new_vma: 搬迁后的目标 VMA。
 * 返回：无；旧端清 VM_LOCKED，整 VMA 被搬空且两端对象不同时解除旧 anon_vma 链。
 */
static void dontunmap_complete(struct vma_remap_struct *vrm,
			       struct vm_area_struct *new_vma)
{
	unsigned long start = vrm->addr;
	unsigned long end = vrm->addr + vrm->old_len;
	unsigned long old_start = vrm->vma->vm_start;
	unsigned long old_end = vrm->vma->vm_end;

	/* We always clear VM_LOCKED[ONFAULT] on the old VMA. */
	/* 页表已从旧端移走，旧地址不能继续保持 mlock/ONFAULT 承诺。 */
	vm_flags_clear(vrm->vma, VM_LOCKED_MASK);

	/*
	 * anon_vma links of the old vma is no longer needed after its page
	 * table has been moved.
	 */
	/* 整个旧 VMA 的页表都被移走时，其 anon_vma rmap 链已无用途；部分搬迁仍须保留残余关系。 */
	if (new_vma != vrm->vma && start == old_start && end == old_end)
		unlink_anon_vmas(vrm->vma);

	/* Because we won't unmap we don't need to touch locked_vm. */
	/* DONTUNMAP 保留同样的 VMA 地址规模，locked_vm 总量无需像 munmap 那样扣减。 */
}

/*
 * move_vma() - 执行一次已选定目标的完整 VMA copy、页表搬迁和源端提交/回滚。
 * @vrm: 源/目标地址、长度、VMA 和 flags 已校验；返回目标地址或 errno 编码。
 * 资源：先检查拆分/KSM并预留 commit；目标建成后无论成功/失败都通过 unmap_source_vma() 收口。
 * 统计：短暂抬高 total_vm 时保存并恢复 hiwater；DONTUNMAP 成功走专用旧端清理。
 */
static unsigned long move_vma(struct vma_remap_struct *vrm)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *new_vma;
	unsigned long hiwater_vm;
	int err;

	err = prep_move_vma(vrm);
	if (err)
		return err;

	/*
	 * If accounted, determine the number of bytes the operation will
	 * charge.
	 */
	/* VM_ACCOUNT 事务在创建目标前一次性预留，后续 copy 失败由相应路径撤销或删除目标。 */
	if (!vrm_calc_charge(vrm))
		return -ENOMEM;

	/* We don't want racing faults. */
	/* VMA 写锁阻止 fault 在 flags/rmap/页表搬迁中观察到中间状态。 */
	vma_start_write(vrm->vma);

	/* Perform copy step. */
	/* copy helper 可能成功建目标后返回错误，此时 new_vma 非空并已把 vrm 改成应删除的一端。 */
	err = copy_vma_and_data(vrm, &new_vma);
	/*
	 * If we established the copied-to VMA, we attempt to recover from the
	 * error by setting the destination VMA to the source VMA and unmapping
	 * it below.
	 */
	/* 若目标尚未建立则直接返回；目标已建立的错误由 helper 改写 vrm，下面会删除该目标完成恢复。 */
	if (err && !new_vma)
		return err;

	/*
	 * If we failed to move page tables we still do total_vm increment
	 * since do_munmap() will decrement it by old_len == new_len.
	 *
	 * Since total_vm is about to be raised artificially high for a
	 * moment, we need to restore high watermark afterwards: if stats
	 * are taken meanwhile, total_vm and hiwater_vm appear too high.
	 * If this were a serious issue, we'd add a flag to do_munmap().
	 */
	/*
	 * 页表搬迁失败也会先为目标 new_len 增加 total_vm，随后删除等长目标再扣回。为免这段瞬时双计
	 * 污染历史峰值，保存 hiwater 并在收口后恢复；并发统计仍可能短暂看到偏高 current 值。
	 */
	hiwater_vm = mm->hiwater_vm;

	vrm_stat_account(vrm, vrm->new_len);
	if (unlikely(!err && (vrm->flags & MREMAP_DONTUNMAP)))
		dontunmap_complete(vrm, new_vma);
	else
		unmap_source_vma(vrm);

	mm->hiwater_vm = hiwater_vm;

	return err ? (unsigned long)err : vrm->new_addr;
}

/*
 * The user has requested that the VMA be shrunk (i.e., old_len > new_len), so
 * execute this, optionally dropping the mmap lock when we do so.
 *
 * In both cases this invalidates the VMA, however if we don't drop the lock,
 * then load the correct VMA into vrm->vma afterwards.
 */
/*
 * shrink_vma() - 解除源请求尾部 [addr+new_len, addr+old_len) 完成原地缩短。
 * @vrm: remap_type 必须为 SHRINK；@drop_lock: 是否允许 munmap 同时释放 mmap 写锁。
 * 返回：0 或 munmap/-EFAULT；总使旧 vma 指针失效，未放锁时重新 lookup 保持后续搬迁可用。
 */
static unsigned long shrink_vma(struct vma_remap_struct *vrm,
				bool drop_lock)
{
	struct mm_struct *mm = current->mm;
	unsigned long unmap_start = vrm->addr + vrm->new_len;
	unsigned long unmap_bytes = vrm->delta;
	unsigned long res;
	/* iterator 从待删除尾部起点开始，便于 munmap 后继续定位保留前缀。 */
	VMA_ITERATOR(vmi, mm, unmap_start);

	VM_BUG_ON(vrm->remap_type != MREMAP_SHRINK);

	res = do_vmi_munmap(&vmi, mm, unmap_start, unmap_bytes,
			    vrm->uf_unmap, drop_lock);
	vrm->vma = NULL; /* Invalidated. */
	/* munmap 后入口 VMA 指针已经失效。 */
	if (res)
		return res;

	/*
	 * If we've not dropped the lock, then we should reload the VMA to
	 * replace the invalidated VMA with the one that may have now been
	 * split.
	 */
	/* 固定目标的 shrink 后还要继续 move，故保持锁并重新取得可能因拆分而变化的源 VMA。 */
	if (drop_lock) {
		vrm->mmap_locked = false;
	} else {
		vrm->vma = vma_lookup(mm, vrm->addr);
		if (!vrm->vma)
			return -EFAULT;
	}

	return 0;
}

/*
 * mremap_to() - remap a vma to a new location.
 * Returns: The new address of the vma or an error.
 */
/*
 * mremap_to() - 把已绑定源 VMA 的事务搬到显式或必选的新地址。
 * @vrm: 已完成公共准备；FIXED 目标可能含旧映射，SHRINK 可先缩到目标长度。
 * 返回：最终新地址或 errno；持 mmap 写锁，调用期间可能清目标、缩源、检查扩张并移动 VMA。
 */
static unsigned long mremap_to(struct vma_remap_struct *vrm)
{
	struct mm_struct *mm = current->mm;
	unsigned long err;

	if (vrm->flags & MREMAP_FIXED) {
		/*
		 * In mremap_to().
		 * VMA is moved to dst address, and munmap dst first.
		 * do_munmap will check if dst is sealed.
		 */
		/* FIXED 与 MAP_FIXED 一样先解除整个目标区；do_munmap 同时执行 mseal 权限检查。 */
		err = do_munmap(mm, vrm->new_addr, vrm->new_len,
				vrm->uf_unmap_early);
		vrm->vma = NULL; /* Invalidated. */
		/* 目标解除可能拆分或合并同一源对象，旧 VMA 指针已经失效。 */
		vrm->vmi_needs_invalidate = true;
		if (err)
			return err;

		/*
		 * If we remap a portion of a VMA elsewhere in the same VMA,
		 * this can invalidate the old VMA. Reset.
		 */
		/* 目标若位于同一大 VMA 的另一部分，前置 munmap 可能拆掉原对象，必须按源地址重查。 */
		vrm->vma = vma_lookup(mm, vrm->addr);
		if (!vrm->vma)
			return -EFAULT;
	}

	if (vrm->remap_type == MREMAP_SHRINK) {
		err = shrink_vma(vrm, /* drop_lock= */false);
		/* drop_lock=false：缩短后仍需持 mmap 写锁继续搬迁。 */
		if (err)
			return err;

		/* Set up for the move now shrink has been executed. */
		/* 缩短已提交后，实际需搬的源长度就是 new_len，避免复制已释放尾部。 */
		vrm->old_len = vrm->new_len;
	}

	/* MREMAP_DONTUNMAP expands by old_len since old_len == new_len */
	/* DONTUNMAP 保留源且复制等长目标，虚拟页总量净增 old_len，需单独通过 may_expand_vm。 */
	if (vrm->flags & MREMAP_DONTUNMAP) {
		vma_flags_t vma_flags = vrm->vma->flags;
		unsigned long pages = vrm->old_len >> PAGE_SHIFT;

		/* 使用 flags 副本让 may_expand_vm 计算分类限额，不修改源 VMA。 */
		if (!may_expand_vm(mm, &vma_flags, pages))
			return -ENOMEM;
	}

	err = vrm_set_new_addr(vrm);
	if (err)
		return err;

	return move_vma(vrm);
}

/*
 * vma_expandable() - 验证 VMA 尾部追加 delta 字节在地址空间和文件映射策略上可行。
 * @vma: 待原地扩展 VMA；@delta: 已页对齐新增字节数。
 * 返回：无溢出、尾后无交叉且 get_unmapped_area(MAP_FIXED) 接受时为 1，否则 0；不修改 VMA。
 */
static int vma_expandable(struct vm_area_struct *vma, unsigned long delta)
{
	unsigned long end = vma->vm_end + delta;

	if (end < vma->vm_end) /* overflow */
		/* unsigned 尾地址回绕说明扩展越过地址上界。 */
		return 0;
	if (find_vma_intersection(vma->vm_mm, vma->vm_end, end))
		return 0;
	if (get_unmapped_area(NULL, vma->vm_start, end - vma->vm_start,
			      0, MAP_FIXED) & ~PAGE_MASK)
		return 0;
	return 1;
}

/* Determine whether we are actually able to execute an in-place expansion. */
/*
 * vrm_can_expand_in_place() - 判断请求是否恰从 VMA 内部起点延伸到尾部且尾后可扩。
 * 返回：源请求尾端等于 vm_end 且 vma_expandable(delta) 为真；纯检查。
 */
static bool vrm_can_expand_in_place(struct vma_remap_struct *vrm)
{
	/* Number of bytes from vrm->addr to end of VMA. */
	/* 只允许扩展请求所覆盖的 VMA 后缀，不能在中部插入虚拟地址。 */
	unsigned long suffix_bytes = vrm->vma->vm_end - vrm->addr;

	/* If end of range aligns to end of VMA, we can just expand in-place. */
	/* old_len 必须精确到 vm_end，否则新增区会与同一 VMA 未请求部分发生语义冲突。 */
	if (suffix_bytes != vrm->old_len)
		return false;

	/* Check whether this is feasible. */
	/* 再检查地址溢出、相邻 VMA 和文件自定义地址约束。 */
	if (!vma_expandable(vrm->vma, vrm->delta))
		return false;

	return true;
}

/*
 * We know we can expand the VMA in-place by delta pages, so do so.
 *
 * If we discover the VMA is locked, update mm_struct statistics accordingly and
 * indicate so to the caller.
 */
/*
 * expand_vma_in_place() - 为可原地扩展事务预留 commit、合并尾部 extension 并更新 mm 统计。
 * @vrm: 已由 vrm_can_expand_in_place() 验证的 EXPAND 事务。
 * 返回：成功 0；计账或 VMA merge 失败 -ENOMEM，失败会撤销本函数预留；成功更新 vrm->vma。
 */
static unsigned long expand_vma_in_place(struct vma_remap_struct *vrm)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma = vrm->vma;
	VMA_ITERATOR(vmi, mm, vma->vm_end);

	if (!vrm_calc_charge(vrm))
		return -ENOMEM;

	/*
	 * Function vma_merge_extend() is called on the
	 * extension we are adding to the already existing vma,
	 * vma_merge_extend() will merge this extension with the
	 * already existing vma (expand operation itself) and
	 * possibly also with the next vma if it becomes
	 * adjacent to the expanded vma and otherwise
	 * compatible.
	 */
	/*
	 * vma_merge_extend() 把新增尾段并入当前 VMA，并可能继续与后继兼容 VMA 合并；返回的新对象可能
	 * 不再等于入口 vma，因此必须写回 vrm。失败时尚未提交扩展，可安全撤销 commit。
	 */
	vma = vma_merge_extend(&vmi, vma, vrm->delta);
	if (!vma) {
		vrm_uncharge(vrm);
		return -ENOMEM;
	}
	vrm->vma = vma;

	vrm_stat_account(vrm, vrm->delta);

	return 0;
}

/*
 * align_hugetlb() - 把 hugetlb mremap 长度提升到 huge page 粒度并校验地址/保留约束。
 * @vrm: 已绑定 hugetlb VMA 的事务；返回可执行为 true，否则 false。
 * 副作用：即使后续失败也会更新 old_len/new_len；禁止扩展，因为 reservation 尚不能拆分处理。
 */
static bool align_hugetlb(struct vma_remap_struct *vrm)
{
	struct hstate *h __maybe_unused = hstate_vma(vrm->vma);

	vrm->old_len = ALIGN(vrm->old_len, huge_page_size(h));
	vrm->new_len = ALIGN(vrm->new_len, huge_page_size(h));

	/* addrs must be huge page aligned */
	/* 两端地址必须满足该 hstate 的大页掩码，不能用 base-page 粒度拆迁。 */
	if (vrm->addr & ~huge_page_mask(h))
		return false;
	if (vrm->new_addr & ~huge_page_mask(h))
		return false;

	/*
	 * Don't allow remap expansion, because the underlying hugetlb
	 * reservation is not yet capable to handle split reservation.
	 */
	/* 扩大可能要求拆分/延伸 hugetlb reservation map，当前实现无法正确维护，故只允许等长或缩短。 */
	if (vrm->new_len > vrm->old_len)
		return false;

	return true;
}

/*
 * We are mremap()'ing without specifying a fixed address to move to, but are
 * requesting that the VMA's size be increased.
 *
 * Try to do so in-place, if this fails, then move the VMA to a new location to
 * action the change.
 */
/*
 * expand_vma() - 为未指定固定目标的 EXPAND 事务优先原地扩展，必要时选择新址搬迁。
 * @vrm: 已校验事务；返回原/新地址或 errno。
 * 若原地不可行且未给 MAYMOVE 返回 -ENOMEM；选择新址只预定地址，最终由 move_vma() 提交。
 */
static unsigned long expand_vma(struct vma_remap_struct *vrm)
{
	unsigned long err;

	/*
	 * [addr, old_len) spans precisely to the end of the VMA, so try to
	 * expand it in-place.
	 */
	/* 源请求恰好覆盖 VMA 后缀且尾后可用时，原地扩展避免 VMA/页表复制。 */
	if (vrm_can_expand_in_place(vrm)) {
		err = expand_vma_in_place(vrm);
		if (err)
			return err;

		/* OK we're done! */
		/* 原地扩展成功，syscall 返回原起点。 */
		return vrm->addr;
	}

	/*
	 * We weren't able to just expand or shrink the area,
	 * we need to create a new one and move it.
	 */
	/* 尾后不可用或请求不抵 VMA 尾端，只能建立更大目标并搬迁。 */

	/* We're not allowed to move the VMA, so error out. */
	/* 缺少 MAYMOVE 时用户不允许改变地址，无法原地扩展即失败。 */
	if (!(vrm->flags & MREMAP_MAYMOVE))
		return -ENOMEM;

	/* Find a new location to move the VMA to. */
	/* 普通扩展以 NULL hint 让 get_unmapped_area() 选择容纳 new_len 的新范围。 */
	err = vrm_set_new_addr(vrm);
	if (err)
		return err;

	return move_vma(vrm);
}

/*
 * Attempt to resize the VMA in-place, if we cannot, then move the VMA to the
 * first available address to perform the operation.
 */
/*
 * mremap_at() - 执行不保证新地址的普通 resize 分派。
 * @vrm: remap_type 已计算；返回原地址、新地址或 errno。
 * NO_RESIZE 不动，SHRINK 原地 munmap 并允许放 mmap 锁，EXPAND 交给原地优先策略。
 */
static unsigned long mremap_at(struct vma_remap_struct *vrm)
{
	unsigned long res;

	switch (vrm->remap_type) {
	case MREMAP_INVALID:
		break;
	case MREMAP_NO_RESIZE:
		/* NO-OP CASE - resizing to the same size. */
		/* 等长且无需移动时保持所有 VMA/页表状态，直接返回原址。 */
		return vrm->addr;
	case MREMAP_SHRINK:
		/*
		 * SHRINK CASE. Can always be done in-place.
		 *
		 * Simply unmap the shrunken portion of the VMA. This does all
		 * the needed commit accounting, and we indicate that the mmap
		 * lock should be dropped.
		 */
		/* 缩短总能通过解除尾部完成；这是最终动作，允许 munmap 同时释放 mmap 写锁。 */
		res = shrink_vma(vrm, /* drop_lock= */true);
		/* drop_lock=true：缩短是最终动作，munmap 可直接交还 mmap 写锁。 */
		if (res)
			return res;

		return vrm->addr;
	case MREMAP_EXPAND:
		return expand_vma(vrm);
	}

	/* Should not be possible. */
	/* check_prep_vma() 必须把 INVALID 转成真实类型；到达此处说明状态机被破坏。 */
	WARN_ON_ONCE(1);
	return -EINVAL;
}

/*
 * Will this operation result in the VMA being expanded or moved and thus need
 * to map a new portion of virtual address space?
 */
/*
 * vrm_will_map_new() - 判断准备阶段是否需要执行跨 VMA 边界和扩张能力检查。
 * 返回：尺寸扩大，或 flags 保证建立目标地址时为 true；SHRINK/no-op 为 false。
 */
static bool vrm_will_map_new(struct vma_remap_struct *vrm)
{
	if (vrm->remap_type == MREMAP_EXPAND)
		return true;

	if (vrm_implies_new_addr(vrm))
		return true;

	return false;
}

/* Does this remap ONLY move mappings? */
/*
 * vrm_move_only() - 识别固定地址、等长的纯搬迁，可启用跨多个 VMA 的批处理。
 * 返回：同时满足 MREMAP_FIXED 与 old_len==new_len 为 true；不修改事务。
 */
static bool vrm_move_only(struct vma_remap_struct *vrm)
{
	if (!(vrm->flags & MREMAP_FIXED))
		return false;

	if (vrm->old_len != vrm->new_len)
		return false;

	return true;
}

/*
 * notify_uffd() - 在 mmap 锁释放后按阶段向 userfaultfd 完成 unmap 与 remap 事件。
 * @vrm: 保存两个 unmap 链和 remap 上下文；@failed: 整体 syscall 是否失败。
 * 返回：无；无论成功失败都完成 unmap，remap 则选择 fail/complete 分支并消费准备状态。
 */
static void notify_uffd(struct vma_remap_struct *vrm, bool failed)
{
	struct mm_struct *mm = current->mm;

	/* Regardless of success/failure, we always notify of any unmaps. */
	/* FIXED 的早期目标 unmap 与最终源/回滚目标 unmap 都已经发生，结果失败也必须发送完成通知。 */
	userfaultfd_unmap_complete(mm, vrm->uf_unmap_early);
	if (failed)
		mremap_userfaultfd_fail(vrm->uf);
	else
		mremap_userfaultfd_complete(vrm->uf, vrm->addr,
			vrm->new_addr, vrm->old_len);
	userfaultfd_unmap_complete(mm, vrm->uf_unmap);
}

/*
 * vma_multi_allowed() - 判断纯固定搬迁能否安全把当前 VMA 纳入多 VMA 批次。
 * @vma: 当前源 VMA；返回：UFFD 未 armed 且自定义 get_unmapped_area 可证明遵守 FIXED 时为 true。
 * 已知 shmem、hugetlb、THP helper 允许；未知文件回调可能重定位目标，故拒绝批处理。
 */
static bool vma_multi_allowed(struct vm_area_struct *vma)
{
	struct file *file = vma->vm_file;

	/*
	 * We can't support moving multiple uffd VMAs as notify requires
	 * mmap lock to be dropped.
	 */
	/* UFFD 通知要求放 mmap 锁，批次中途无法维持单一锁事务和 iterator，故拒绝。 */
	if (userfaultfd_armed(vma))
		return false;

	/*
	 * Custom get unmapped area might result in MREMAP_FIXED not
	 * being obeyed.
	 */
	/* 任意文件自定义地址选择器可能忽略 MAP_FIXED，导致目标间隙无法保持。 */
	if (!file || !file->f_op->get_unmapped_area)
		return true;
	/* Known good. */
	/* 以下内核内建实现已知严格遵循固定地址语义。 */
	if (vma_is_shmem(vma))
		return true;
	if (is_vm_hugetlb_page(vma))
		return true;
	if (file->f_op->get_unmapped_area == thp_get_unmapped_area)
		return true;

	return false;
}

/*
 * check_prep_vma() - 针对当前源 VMA 规范化 hugetlb/长度并验证本次 resize/move 可执行。
 * @vrm: addr/length/flags 已完成 syscall 级检查，vma 可为 lookup 结果。
 * 返回：0 或 -EFAULT/-EPERM/-EINVAL/-EAGAIN/-ENOMEM；成功写入 delta/type/new_addr/populate 状态。
 * 检查覆盖 seal、边界、pgoff 溢出、DONTUNMAP、锁页限额、VM_DONTEXPAND/PFNMAP 与虚拟页上限。
 */
static int check_prep_vma(struct vma_remap_struct *vrm)
{
	struct vm_area_struct *vma = vrm->vma;
	struct mm_struct *mm = current->mm;
	unsigned long addr = vrm->addr;
	unsigned long old_len, new_len, pgoff;

	if (!vma)
		return -EFAULT;

	/* If mseal()'d, mremap() is prohibited. */
	/* sealed VMA 禁止 resize/move，防止绕过 mseal 对布局变更的约束。 */
	if (vma_is_sealed(vma))
		return -EPERM;

	/* Align to hugetlb page size, if required. */
	/* hugetlb 长度先向上取整到其 hstate 粒度，后续 delta/type 都基于真实搬迁单位。 */
	if (is_vm_hugetlb_page(vma) && !align_hugetlb(vrm))
		return -EINVAL;

	vrm_set_delta(vrm);
	vrm->remap_type = vrm_remap_type(vrm);
	/* For convenience, we set new_addr even if VMA won't move. */
	/* 非强制搬迁先把目标设为源址，使后续原地成功和 populate 统一使用 new_addr。 */
	if (!vrm_implies_new_addr(vrm))
		vrm->new_addr = addr;

	/* Below only meaningful if we expand or move a VMA. */
	/* 单纯缩短/no-op 不建立新地址空间，可跳过以下扩张与跨 VMA 限制。 */
	if (!vrm_will_map_new(vrm))
		return 0;

	old_len = vrm->old_len;
	new_len = vrm->new_len;

	/*
	 * !old_len is a special case where an attempt is made to 'duplicate'
	 * a mapping.  This makes no sense for private mappings as it will
	 * instead create a fresh/new mapping unrelated to the original.  This
	 * is contrary to the basic idea of mremap which creates new mappings
	 * based on the original.  There are no known use cases for this
	 * behavior.  As a result, fail such attempts.
	 */
	/*
	 * old_len==0 历史上用于复制共享映射；私有映射这样做只会产生与原映射无关的新对象，违背
	 * mremap 派生语义且无已知用户，因此明确拒绝，只保留 SHARED/MAYSHARE 兼容入口。
	 */
	if (!old_len && !(vma->vm_flags & (VM_SHARED | VM_MAYSHARE))) {
		pr_warn_once("%s (%d): attempted to duplicate a private mapping with mremap.  This is not supported.\n",
			     current->comm, current->pid);
		return -EINVAL;
	}

	if ((vrm->flags & MREMAP_DONTUNMAP) &&
			(vma->vm_flags & (VM_DONTEXPAND | VM_PFNMAP)))
		return -EINVAL;

	/*
	 * We permit crossing of boundaries for the range being unmapped due to
	 * a shrink.
	 */
	/* shrink 可跨原请求尾端后的相邻 VMA 解除范围；实际源搬迁只取保留的 new_len 前缀。 */
	if (vrm->remap_type == MREMAP_SHRINK)
		old_len = new_len;

	/*
	 * We can't remap across the end of VMAs, as another VMA may be
	 * adjacent:
	 *
	 *       addr   vma->vm_end
	 *  |-----.----------|
	 *  |     .          |
	 *  |-----.----------|
	 *        .<--------->xxx>
	 *            old_len
	 *
	 * We also require that vma->vm_start <= addr < vma->vm_end.
	 */
	/* 除 shrink 特例外，实际复制范围不能越过当前 VMA 尾部，也要求 addr 确实落在 lookup VMA 内。 */
	if (old_len > vma->vm_end - addr)
		return -EFAULT;

	if (new_len == old_len)
		return 0;

	/* We are expanding and the VMA is mlock()'d so we need to populate. */
	/* 锁定映射扩展成功后必须 fault-in 新增页，动作延后到 mmap 锁释放之后。 */
	if (vma->vm_flags & VM_LOCKED)
		vrm->populate_expand = true;

	/* Need to be careful about a growing mapping */
	/* 文件页偏移加新页数不得回绕，否则目标映射无法用 pgoff_t 正确表达。 */
	pgoff = (addr - vma->vm_start) >> PAGE_SHIFT;
	pgoff += vma->vm_pgoff;
	if (pgoff + (new_len >> PAGE_SHIFT) < pgoff)
		return -EINVAL;

	if (vma->vm_flags & (VM_DONTEXPAND | VM_PFNMAP))
		return -EFAULT;

	/* mlock RLIMIT 与 mm 分类虚拟页限额分别给出 EAGAIN 和 ENOMEM。 */
	if (!mlock_future_ok(mm, vma->vm_flags & VM_LOCKED, vrm->delta))
		return -EAGAIN;

	if (!may_expand_vm(mm, &vma->flags, vrm->delta >> PAGE_SHIFT))
		return -ENOMEM;

	return 0;
}

/*
 * Are the parameters passed to mremap() valid? If so return 0, otherwise return
 * error.
 */
/*
 * check_mremap_params() - 在查 VMA/加 mmap 锁前验证 syscall 级 flags、对齐、范围与组合关系。
 * @vrm: 用户参数事务，长度尚未必经过 VMA 特定 hugetlb 对齐。
 * 返回：有效为 0，否则 -EINVAL；不访问 VMA 树、不修改映射。
 */
static unsigned long check_mremap_params(struct vma_remap_struct *vrm)

{
	unsigned long addr = vrm->addr;
	unsigned long flags = vrm->flags;

	/* Ensure no unexpected flag values. */
	/* 只接受三个公开 MREMAP 位，未知位不能静默忽略。 */
	if (flags & ~(MREMAP_FIXED | MREMAP_MAYMOVE | MREMAP_DONTUNMAP))
		return -EINVAL;

	/* Start address must be page-aligned. */
	/* 源起点至少按 base page 对齐；hugetlb 更严格约束稍后检查。 */
	if (offset_in_page(addr))
		return -EINVAL;

	/*
	 * We allow a zero old-len as a special case
	 * for DOS-emu "duplicate shm area" thing. But
	 * a zero new-len is nonsensical.
	 */
	/* 保留 old_len==0 的共享映射兼容语义，但目标零长度没有有效映射含义。 */
	if (!vrm->new_len)
		return -EINVAL;

	/* Is the new length silly? */
	/* 单个映射不能请求超过用户地址空间上限。 */
	if (vrm->new_len > TASK_SIZE)
		return -EINVAL;

	/* Remainder of checks are for cases with specific new_addr. */
	/* 普通非搬迁 resize 不使用用户 new_addr，剩余目标检查可跳过。 */
	if (!vrm_implies_new_addr(vrm))
		return 0;

	/* Is the new address silly? */
	/* 采用减法形式同时排除目标尾部越过 TASK_SIZE 和加法溢出。 */
	if (vrm->new_addr > TASK_SIZE - vrm->new_len)
		return -EINVAL;

	/* The new address must be page-aligned. */
	/* 目标同样至少 base-page 对齐。 */
	if (offset_in_page(vrm->new_addr))
		return -EINVAL;

	/* A fixed address implies a move. */
	/* FIXED/DONTUNMAP 需要 MAYMOVE 授权地址改变。 */
	if (!(flags & MREMAP_MAYMOVE))
		return -EINVAL;

	/* MREMAP_DONTUNMAP does not allow resizing in the process. */
	/* 保留旧地址时只允许等长转移页表，避免一边保留一边改变范围的未定义计账。 */
	if (flags & MREMAP_DONTUNMAP && vrm->old_len != vrm->new_len)
		return -EINVAL;

	/* Target VMA must not overlap source VMA. */
	/* 显式目标与源重叠会破坏先清目标再搬源的事务顺序，统一拒绝。 */
	if (vrm_overlaps(vrm))
		return -EINVAL;

	return 0;
}

/*
 * remap_move() - 执行 FIXED 等长纯搬迁，可按源范围顺序跨多个 VMA 并保持原有间隙。
 * @vrm: flags/地址已检查且持 mmap 写锁；函数会为每个相交 VMA 重写当前子事务。
 * 返回：首个目标 VMA 地址或 errno；不允许源起点空洞，子 VMA 不兼容、失败或中间空洞按规则终止。
 * 迭代：VMA 树被 copy/munmap 改写后按 vmi_needs_invalidate 重建 iterator，锁始终保持。
 */
static unsigned long remap_move(struct vma_remap_struct *vrm)
{
	struct vm_area_struct *vma;
	unsigned long start = vrm->addr;
	unsigned long end = vrm->addr + vrm->old_len;
	unsigned long new_addr = vrm->new_addr;
	unsigned long target_addr = new_addr;
	unsigned long res = -EFAULT;
	unsigned long last_end;
	bool seen_vma = false;

	/* iterator 只遍历源半开区间，target_addr 累积上一目标片段尾端。 */
	VMA_ITERATOR(vmi, current->mm, start);

	/*
	 * When moving VMAs we allow for batched moves across multiple VMAs,
	 * with all VMAs in the input range [addr, addr + old_len) being moved
	 * (and split as necessary).
	 */
	/* 等长 FIXED move 的输入可覆盖多个 VMA；每个相交片段独立搬迁并在边界处按需拆分。 */
	for_each_vma_range(vmi, vma, end) {
		/* Account for start, end not aligned with VMA start, end. */
		/* 首尾 VMA 只取与用户半开区间的交集，内部 VMA 使用完整长度。 */
		unsigned long addr = max(vma->vm_start, start);
		unsigned long len = min(end, vma->vm_end) - addr;
		unsigned long offset, res_vma;
		bool multi_allowed;

		/* No gap permitted at the start of the range. */
		/* 源起点必须有映射；否则没有第一段可定义 syscall 返回地址。 */
		if (!seen_vma && start < vma->vm_start)
			return -EFAULT;

		/*
		 * To sensibly move multiple VMAs, accounting for the fact that
		 * get_unmapped_area() may align even MAP_FIXED moves, we simply
		 * attempt to move such that the gaps between source VMAs remain
		 * consistent in destination VMAs, e.g.:
		 *
		 *           X        Y                       X        Y
		 *         <--->     <->                    <--->     <->
		 * |-------|   |-----| |-----|      |-------|   |-----| |-----|
		 * |   A   |   |  B  | |  C  | ---> |   A'  |   |  B' | |  C' |
		 * |-------|   |-----| |-----|      |-------|   |-----| |-----|
		 *                               new_addr
		 *
		 * So we map B' at A'->vm_end + X, and C' at B'->vm_end + Y.
		 */
		/*
		 * 文件 get_unmapped_area 即使面对 FIXED 也可能对齐地址；批处理以第一段实际返回为基准，
		 * 后续把源 VMA 间隙 X/Y 原样加到上一目标尾部，使目标布局保持相对稀疏结构。
		 */
		offset = seen_vma ? vma->vm_start - last_end : 0;
		last_end = vma->vm_end;

		vrm->vma = vma;
		vrm->addr = addr;
		/* 当前目标由上一目标尾端加源间隙得到，子事务强制等长。 */
		vrm->new_addr = target_addr + offset;
		vrm->old_len = vrm->new_len = len;

		multi_allowed = vma_multi_allowed(vma);
		if (!multi_allowed) {
			/* This is not the first VMA, abort immediately. */
			/* 不兼容 VMA 只能作为唯一一段处理，出现在已搬段之后无法安全继续。 */
			if (seen_vma)
				return -EFAULT;
			/* This is the first, but there are more, abort. */
			/* 第一段不兼容且输入还跨到后续 VMA 时，也必须在产生副作用前拒绝。 */
			if (vma->vm_end < end)
				return -EFAULT;
		}

		res_vma = check_prep_vma(vrm);
		/* 每个子 VMA 都重新检查 seal/hugetlb/文件约束，再执行固定搬迁。 */
		if (!res_vma)
			res_vma = mremap_to(vrm);
		if (IS_ERR_VALUE(res_vma))
			return res_vma;

		if (!seen_vma) {
			/* syscall 返回首段实际目标；已知安全多 VMA 类型应严格服从原 new_addr。 */
			VM_WARN_ON_ONCE(multi_allowed && res_vma != new_addr);
			res = res_vma;
		}

		/* mmap lock is only dropped on shrink. */
		/* 本路径所有子事务等长纯 move，不应走会放锁的 shrink 分支。 */
		VM_WARN_ON_ONCE(!vrm->mmap_locked);
		/* This is a move, no expand should occur. */
		/* 等长子事务不得设置锁定扩展后的 populate 标记。 */
		VM_WARN_ON_ONCE(vrm->populate_expand);

		if (vrm->vmi_needs_invalidate) {
			vma_iter_invalidate(&vmi);
			vrm->vmi_needs_invalidate = false;
		}
		seen_vma = true;
		target_addr = res_vma + vrm->new_len;
	}

	return res;
}

/*
 * do_mremap() - mremap 内核主状态机：规范化长度、加锁、分派操作、解锁并完成 populate/UFFD。
 * @vrm: syscall 栈上事务及 UFFD 输出链；返回最终地址或 errno 编码。
 * 锁：killable 取得 mmap 写锁，shrink 可在内部释放并清 mmap_locked；所有通知在锁外完成。
 * 失败：参数/锁/余量/准备/执行任一步均由已到达阶段的 helper 收口资源。
 */
static unsigned long do_mremap(struct vma_remap_struct *vrm)
{
	struct mm_struct *mm = current->mm;
	unsigned long res;
	bool failed;

	vrm->old_len = PAGE_ALIGN(vrm->old_len);
	vrm->new_len = PAGE_ALIGN(vrm->new_len);
	/* syscall 长度先提升到 base-page 粒度；hugetlb 会在绑定 VMA 后再次提升到 huge 粒度。 */

	res = check_mremap_params(vrm);
	if (res)
		return res;

	if (mmap_write_lock_killable(mm))
		return -EINTR;
	/* 从此开始 vrm->mmap_locked 是唯一最终 unlock 判据，允许 shrink helper 主动清除。 */
	vrm->mmap_locked = true;

	if (!check_map_count_against_split_early()) {
		/* 在任何前置 munmap 产生副作用前，按最坏四个临时 VMA 检查余量。 */
		mmap_write_unlock(mm);
		return -ENOMEM;
	}

	if (vrm_move_only(vrm)) {
		res = remap_move(vrm);
	} else {
		vrm->vma = vma_lookup(current->mm, vrm->addr);
		res = check_prep_vma(vrm);
		if (res)
			goto out;

		/* Actually execute mremap. */
		/* 保证目标地址的 flags 走 mremap_to，否则由 resize 类型选择原地/no-op/搬迁。 */
		res = vrm_implies_new_addr(vrm) ? mremap_to(vrm) : mremap_at(vrm);
	}

out:
	failed = IS_ERR_VALUE(res);

	if (vrm->mmap_locked)
		mmap_write_unlock(mm);

	/* VMA mlock'd + was expanded, so populated expanded region. */
	/* mmap 锁外 fault-in 锁定映射新增尾部；只在整体成功且准备阶段标记时执行。 */
	if (!failed && vrm->populate_expand)
		mm_populate(vrm->new_addr + vrm->old_len, vrm->delta);

	notify_uffd(vrm, failed);
	return res;
}

/*
 * Expand (or shrink) an existing mapping, potentially moving it at the
 * same time (controlled by the MREMAP_MAYMOVE flag and available VM space)
 *
 * MREMAP_FIXED option added 5-Dec-1999 by Benjamin LaHaise
 * This option implies MREMAP_MAYMOVE.
 */
/*
 * sys_mremap() - 用户 ABI 入口，构造一次 vma_remap_struct 并交给 do_mremap()。
 * 参数：addr/old_len 为源，new_len 为目标大小，flags 控制移动语义，new_addr 仅相关 flags 使用。
 * 返回：成功地址或负 errno；UFFD 临时上下文/链表均在 syscall 栈上，do_mremap() 返回前完成通知。
 */
SYSCALL_DEFINE5(mremap, unsigned long, addr, unsigned long, old_len,
		unsigned long, new_len, unsigned long, flags,
		unsigned long, new_addr)
{
	struct vm_userfaultfd_ctx uf = NULL_VM_UFFD_CTX;
	LIST_HEAD(uf_unmap_early);
	LIST_HEAD(uf_unmap);
	/*
	 * There is a deliberate asymmetry here: we strip the pointer tag
	 * from the old address but leave the new address alone. This is
	 * for consistency with mmap(), where we prevent the creation of
	 * aliasing mappings in userspace by leaving the tag bits of the
	 * mapping address intact. A non-zero tag will cause the subsequent
	 * range checks to reject the address as invalid.
	 *
	 * See Documentation/arch/arm64/tagged-address-abi.rst for more
	 * information.
	 */
	/*
	 * 源地址允许 arm64 tagged pointer，查映射前去 tag；目标地址故意保留 tag，与 mmap 一致地让后续
	 * 范围检查拒绝可能制造别名的带 tag 映射。详见 tagged-address ABI 文档。
	 */
	struct vma_remap_struct vrm = {
		.addr = untagged_addr(addr),
		.old_len = old_len,
		.new_len = new_len,
		.flags = flags,
		.new_addr = new_addr,

		.uf = &uf,
		.uf_unmap_early = &uf_unmap_early,
		.uf_unmap = &uf_unmap,

		.remap_type = MREMAP_INVALID, /* We set later. */
		/* 绑定 VMA 并完成 hugetlb 长度规范化后再计算真实操作类型。 */
	};

	return do_mremap(&vrm);
}
