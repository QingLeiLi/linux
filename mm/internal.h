/* SPDX-License-Identifier: GPL-2.0-or-later */
/* internal.h: mm/ internal definitions
 *
 * Copyright (C) 2004 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */
/* 本头文件汇集仅供 mm/ 子系统内部共享的定义；版权与作者信息保持如上。 */
#ifndef __MM_INTERNAL_H
#define __MM_INTERNAL_H

#include <linux/fs.h>
#include <linux/khugepaged.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
/* 依赖按页表通知、page cache、walk、rmap、swap 与 soft-leaf 语义分组引入。 */
#include <linux/mmu_notifier.h>
#include <linux/pagemap.h>
#include <linux/pagewalk.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/leafops.h>
#include <linux/tracepoint-defs.h>

/* Internal core VMA manipulation functions. */
/* VMA 核心变更接口集中在同目录 vma.h，此处作为 mm 内部聚合入口。 */
#include "vma.h"

/* folio_batch 的实体由公共头定义，这里只需声明供后续原型借用。 */
struct folio_batch;

/*
 * Maintains state across a page table move. The operation assumes both source
 * and destination VMAs already exist and are specified by the user.
 *
 * Partial moves are permitted, but the old and new ranges must both reside
 * within a VMA.
 *
 * mmap lock must be held in write and VMA write locks must be held on any VMA
 * that is visible.
 *
 * Use the PAGETABLE_MOVE() macro to initialise this struct.
 *
 * The old_addr and new_addr fields are updated as the page table move is
 * executed.
 *
 * NOTE: The page table move is affected by reading from [old_addr, old_end),
 * and old_addr may be updated for better page table alignment, so len_in
 * represents the length of the range being copied as specified by the user.
 */
/*
 * pagetable_move_control 描述一次已存在源/目标 VMA 间的页表搬迁游标。
 * 调用者持 mmap 写锁及可见 VMA 写锁；old/new 地址会推进，len_in 始终保存用户原始长度以换算短进度。
 */
struct pagetable_move_control {
	struct vm_area_struct *old; /* Source VMA. */
	/* old 借用源 VMA，事务期间由调用者保证存活。 */
	struct vm_area_struct *new; /* Destination VMA. */
	/* new 借用目标 VMA，目标范围在搬迁前应为空。 */
	unsigned long old_addr; /* Address from which the move begins. */
	/* old_addr 是可推进且可能向下重对齐的源游标。 */
	unsigned long old_end; /* Exclusive address at which old range ends. */
	/* old_end 是固定的源半开区间终点。 */
	unsigned long new_addr; /* Address to move page tables to. */
	/* new_addr 与 old_addr 同步推进，保持页间偏移。 */
	unsigned long len_in; /* Bytes to remap specified by user. */
	/* len_in 保留用户请求长度，不包含内部 realign 扩出的前缀。 */

	bool need_rmap_locks; /* Do rmap locks need to be taken? */
	/* need_rmap_locks 要求搬迁 PTE 时串行反向映射遍历。 */
	bool for_stack; /* Is this an early temp stack being moved? */
	/* for_stack 标识 exec 早期临时栈，失败收口依赖其专用生命周期。 */
};

/* PAGETABLE_MOVE() 以单次求值的参数构造栈上控制块，并计算固定 old_end。 */
#define PAGETABLE_MOVE(name, old_, new_, old_addr_, new_addr_, len_)	\
	struct pagetable_move_control name = {				\
		.old = old_,						\
		.new = new_,						\
		.old_addr = old_addr_,					\
		.old_end = (old_addr_) + (len_),			\
		.new_addr = new_addr_,					\
		.len_in = len_,						\
	}

/*
 * The set of flags that only affect watermark checking and reclaim
 * behaviour. This is used by the MM to obey the caller constraints
 * about IO, FS and watermark checking while ignoring placement
 * hints such as HIGHMEM usage.
 */
/* 该掩码只保留回收、水位及调用者 IO/FS 约束，刻意忽略 HIGHMEM 等放置提示。 */
#define GFP_RECLAIM_MASK (__GFP_RECLAIM|__GFP_HIGH|__GFP_IO|__GFP_FS|\
			__GFP_NOWARN|__GFP_RETRY_MAYFAIL|__GFP_NOFAIL|\
			__GFP_NORETRY|__GFP_MEMALLOC|__GFP_NOMEMALLOC|\
			__GFP_NOLOCKDEP)

/* The GFP flags allowed during early boot */
/* 启动早期尚不能回收或进入 IO/文件系统，因此从完整 GFP 位中去掉这些能力。 */
#define GFP_BOOT_MASK (__GFP_BITS_MASK & ~(__GFP_RECLAIM|__GFP_IO|__GFP_FS))

/* Control allocation cpuset and node placement constraints */
/* 这两位专门控制 cpuset 与指定节点的放置边界。 */
#define GFP_CONSTRAINT_MASK (__GFP_HARDWALL|__GFP_THISNODE)

/* Do not use these with a slab allocator */
/* slab 分配器不能接受 DMA32、HIGHMEM 或定义域外的 GFP 位。 */
#define GFP_SLAB_BUG_MASK (__GFP_DMA32|__GFP_HIGHMEM|~__GFP_BITS_MASK)

/*
 * Different from WARN_ON_ONCE(), no warning will be issued
 * when we specify __GFP_NOWARN.
 */
/* WARN_ON_ONCE_GFP() 仍返回条件真假，但 __GFP_NOWARN 会抑制一次性告警。 */
#define WARN_ON_ONCE_GFP(cond, gfp)	({				\
	static bool __section(".data..once") __warned;			\
	int __ret_warn_once = !!(cond);					\
									\
	if (unlikely(!(gfp & __GFP_NOWARN) && __ret_warn_once && !__warned)) { \
		__warned = true;					\
		WARN_ON(1);						\
	}								\
	unlikely(__ret_warn_once);					\
})

/* page_writeback_init() 在内存初始化期建立回写限流所需的全局状态。 */
void page_writeback_init(void);

/*
 * If a 16GB hugetlb folio were mapped by PTEs of all of its 4kB pages,
 * its nr_pages_mapped would be 0x400000: choose the ENTIRELY_MAPPED bit
 * above that range, instead of 2*(PMD_SIZE/PAGE_SIZE).  Hugetlb currently
 * leaves nr_pages_mapped at 0, but avoid surprise if it participates later.
 */
/* ENTIRELY_MAPPED 选在最大可表示逐页 mapcount 之上，低位掩码只保存部分映射页数。 */
#define ENTIRELY_MAPPED		0x800000
#define FOLIO_PAGES_MAPPED	(ENTIRELY_MAPPED - 1)

/*
 * Flags passed to __show_mem() and show_free_areas() to suppress output in
 * various contexts.
 */
/* SHOW_MEM_FILTER_NODES 让内存转储隐藏当前调用者不允许访问的节点。 */
#define SHOW_MEM_FILTER_NODES		(0x0001u)	/* disallowed nodes */

/*
 * How many individual pages have an elevated _mapcount.  Excludes
 * the folio's entire_mapcount.
 *
 * Don't use this function outside of debugging code.
 */
/*
 * folio_nr_pages_mapped() - 读取仅供调试的逐子页高 mapcount 数量。
 * 返回：关闭 PAGE_MAPCOUNT 时为 -1，否则屏蔽 entire_mapcount 标志后的计数；不取引用。
 */
static inline int folio_nr_pages_mapped(const struct folio *folio)
{
	if (IS_ENABLED(CONFIG_NO_PAGE_MAPCOUNT))
		return -1;
	return atomic_read(&folio->_nr_pages_mapped) & FOLIO_PAGES_MAPPED;
}

/*
 * Retrieve the first entry of a folio based on a provided entry within the
 * folio. We cannot rely on folio->swap as there is no guarantee that it has
 * been initialized. Used for calling arch_swap_restore()
 */
/*
 * folio_swap() - 从 folio 内任一 swap entry 向下对齐得到该 folio 的首 entry。
 * @entry: 已知属于 folio 的条目；@folio: 只借用以取得页数。返回值不依赖未必初始化的 folio->swap。
 */
static inline swp_entry_t folio_swap(swp_entry_t entry,
		const struct folio *folio)
{
	swp_entry_t swap = {
		.val = ALIGN_DOWN(entry.val, folio_nr_pages(folio)),
	};

	return swap;
}

/* folio_raw_mapping() 去掉 mapping 低位类型标志，返回未经类型解释的 address_space 指针。 */
static inline void *folio_raw_mapping(const struct folio *folio)
{
	unsigned long mapping = (unsigned long)folio->mapping;

	return (void *)(mapping & ~FOLIO_MAPPING_FLAGS);
}

/*
 * This is a file-backed mapping, and is about to be memory mapped - invoke its
 * mmap hook and safely handle error conditions. On error, VMA hooks will be
 * mutated.
 *
 * @file: File which backs the mapping.
 * @vma:  VMA which we are mapping.
 *
 * Returns: 0 if success, error otherwise.
 */
/*
 * mmap_file() - 调用文件 mmap 钩子并隔离失败后可能不一致的 VMA。
 * 返回 0 或文件 errno；失败把 vm_ops 换成空钩子，禁止后续 close 等回调触碰半初始化状态。
 */
static inline int mmap_file(struct file *file, struct vm_area_struct *vma)
{
	int err = vfs_mmap(file, vma);

	if (likely(!err))
		return 0;

	/*
	 * OK, we tried to call the file hook for mmap(), but an error
	 * arose. The mapping is in an inconsistent state and we must not invoke
	 * any further hooks on it.
	 */
	/* 文件钩子失败后 VMA 可能只完成部分初始化，必须切换到 dummy ops。 */
	vma->vm_ops = &vma_dummy_vm_ops;

	return err;
}

/*
 * If the VMA has a close hook then close it, and since closing it might leave
 * it in an inconsistent state which makes the use of any hooks suspect, clear
 * them down by installing dummy empty hooks.
 */
/*
 * vma_close() - 若存在 close 钩子则调用一次，并把 vm_ops 作废为 dummy。
 * 调用者负责 VMA 生命周期与所需锁；返回无，close 后不允许再调用该文件的一组 VMA 钩子。
 */
static inline void vma_close(struct vm_area_struct *vma)
{
	if (vma->vm_ops && vma->vm_ops->close) {
		vma->vm_ops->close(vma);

		/*
		 * The mapping is in an inconsistent state, and no further hooks
		 * may be invoked upon it.
		 */
		/* close 可能破坏私有状态，替换为空实现可阻止任何二次回调。 */
		vma->vm_ops = &vma_dummy_vm_ops;
	}
}

/* unmap_vmas is in mm/memory.c */
/* unmap_vmas() 的实现位于 memory.c；调用者提供已初始化的 TLB gather 与范围描述。 */
void unmap_vmas(struct mmu_gather *tlb, struct unmap_desc *unmap);

#ifdef CONFIG_MMU

/* get_anon_vma() 为共享 anon_vma 增加一份生命周期引用；不取得 rwsem。 */
static inline void get_anon_vma(struct anon_vma *anon_vma)
{
	atomic_inc(&anon_vma->refcount);
}

/* __put_anon_vma() 只供最后一份引用的慢速释放路径调用。 */
void __put_anon_vma(struct anon_vma *anon_vma);

/* put_anon_vma() 释放一份引用，并在原子减到零时销毁对象。 */
static inline void put_anon_vma(struct anon_vma *anon_vma)
{
	if (atomic_dec_and_test(&anon_vma->refcount))
		__put_anon_vma(anon_vma);
}

/* 以下六个包装器统一锁住 anon_vma 根节点 rwsem，使同一树共享同步域。 */
static inline void anon_vma_lock_write(struct anon_vma *anon_vma)
{
	down_write(&anon_vma->root->rwsem);
}

/* anon_vma_trylock_write() 非阻塞尝试根节点写锁，成功返回非零。 */
static inline int anon_vma_trylock_write(struct anon_vma *anon_vma)
{
	return down_write_trylock(&anon_vma->root->rwsem);
}

/* anon_vma_unlock_write() 释放与 lock/trylock 配对的根节点写锁。 */
static inline void anon_vma_unlock_write(struct anon_vma *anon_vma)
{
	up_write(&anon_vma->root->rwsem);
}

/* anon_vma_lock_read() 可睡眠取得根节点读锁。 */
static inline void anon_vma_lock_read(struct anon_vma *anon_vma)
{
	down_read(&anon_vma->root->rwsem);
}

/* anon_vma_trylock_read() 非阻塞尝试根节点读锁，成功返回非零。 */
static inline int anon_vma_trylock_read(struct anon_vma *anon_vma)
{
	return down_read_trylock(&anon_vma->root->rwsem);
}

/* anon_vma_unlock_read() 释放根节点读锁。 */
static inline void anon_vma_unlock_read(struct anon_vma *anon_vma)
{
	up_read(&anon_vma->root->rwsem);
}

/* folio_get_anon_vma() 尝试取得 folio 当前 anon_vma 的稳定引用，失败返回 NULL。 */
struct anon_vma *folio_get_anon_vma(const struct folio *folio);

/* Operations which modify VMAs. */
/* vma_operation 告诉 anon_vma 克隆路径本次 VMA 结构变化的业务原因。 */
enum vma_operation {
	VMA_OP_SPLIT,
	VMA_OP_MERGE_UNFAULTED,
	VMA_OP_REMAP,
	VMA_OP_FORK,
};

/* anon_vma 接口组在持有相应 mmap/VMA 锁时复制、建立或解除反向映射链。 */
int anon_vma_clone(struct vm_area_struct *dst, struct vm_area_struct *src,
	enum vma_operation operation);
int anon_vma_fork(struct vm_area_struct *vma, struct vm_area_struct *pvma);
int  __anon_vma_prepare(struct vm_area_struct *vma);
void unlink_anon_vmas(struct vm_area_struct *vma);

/* anon_vma_prepare() 对已有 anon_vma 是无副作用快路，否则进入可能分配的慢路。 */
static inline int anon_vma_prepare(struct vm_area_struct *vma)
{
	if (likely(vma->anon_vma))
		return 0;

	return __anon_vma_prepare(vma);
}

/* Flags for folio_pte_batch(). */
/* fpb_t 是 PTE 批比较/合并策略的 sparse 位类型，避免与普通整数误混。 */
typedef int __bitwise fpb_t;

/* Compare PTEs respecting the dirty bit. */
/* 设置后 dirty 差异会截断批次；否则比较前清 dirty。 */
#define FPB_RESPECT_DIRTY		((__force fpb_t)BIT(0))

/* Compare PTEs respecting the soft-dirty bit. */
/* 设置后 soft-dirty 是批次相等条件的一部分。 */
#define FPB_RESPECT_SOFT_DIRTY		((__force fpb_t)BIT(1))

/* Compare PTEs respecting the writable bit. */
/* 设置后 writable 差异不可被忽略。 */
#define FPB_RESPECT_WRITE		((__force fpb_t)BIT(2))

/*
 * Merge PTE write bits: if any PTE in the batch is writable, modify the
 * PTE at @ptentp to be writable.
 */
/* 批内任一 PTE 可写时，把代表性副本合并为可写。 */
#define FPB_MERGE_WRITE			((__force fpb_t)BIT(3))

/*
 * Merge PTE young and dirty bits: if any PTE in the batch is young or dirty,
 * modify the PTE at @ptentp to be young or dirty, respectively.
 */
/* 批内任一 accessed/dirty 状态以 OR 方式合并到代表性 PTE 副本。 */
#define FPB_MERGE_YOUNG_DIRTY		((__force fpb_t)BIT(4))

/* __pte_batch_clear_ignored() 规范化一个 PTE 的可忽略位，并总清 young 供连续比较。 */
static inline pte_t __pte_batch_clear_ignored(pte_t pte, fpb_t flags)
{
	if (!(flags & FPB_RESPECT_DIRTY))
		pte = pte_mkclean(pte);
	if (likely(!(flags & FPB_RESPECT_SOFT_DIRTY)))
		pte = pte_clear_soft_dirty(pte);
	if (likely(!(flags & FPB_RESPECT_WRITE)))
		pte = pte_wrprotect(pte);
	return pte_mkold(pte);
}

/**
 * folio_pte_batch_flags - detect a PTE batch for a large folio
 * @folio: The large folio to detect a PTE batch for.
 * @vma: The VMA. Only relevant with FPB_MERGE_WRITE, otherwise can be NULL.
 * @ptep: Page table pointer for the first entry.
 * @ptentp: Pointer to a COPY of the first page table entry whose flags this
 *	    function updates based on @flags if appropriate.
 * @max_nr: The maximum number of table entries to consider.
 * @flags: Flags to modify the PTE batch semantics.
 *
 * Detect a PTE batch: consecutive (present) PTEs that map consecutive
 * pages of the same large folio in a single VMA and a single page table.
 *
 * All PTEs inside a PTE batch have the same PTE bits set, excluding the PFN,
 * the accessed bit, writable bit, dirty bit (unless FPB_RESPECT_DIRTY is set)
 * and soft-dirty bit (unless FPB_RESPECT_SOFT_DIRTY is set).
 *
 * @ptep must map any page of the folio. max_nr must be at least one and
 * must be limited by the caller so scanning cannot exceed a single VMA and
 * a single page table.
 *
 * Depending on the FPB_MERGE_* flags, the pte stored at @ptentp will
 * be updated: it's crucial that a pointer to a COPY of the first
 * page table entry, obtained through ptep_get(), is provided as @ptentp.
 *
 * This function will be inlined to optimize based on the input parameters;
 * consider using folio_pte_batch() instead if applicable.
 *
 * Return: the number of table entries in the batch.
 */
/*
 * folio_pte_batch_flags() - 在单 VMA/单页表范围内识别同一大 folio 的连续 PTE 批次。
 * @ptentp 必须指向首 PTE 的副本；返回不超过 max_nr 的条目数，并按 flags 合并 write/young/dirty。
 * 调用者负责页表稳定性；函数只读真实 PTE，唯一输出写入副本。
 */
static inline unsigned int folio_pte_batch_flags(struct folio *folio,
		struct vm_area_struct *vma, pte_t *ptep, pte_t *ptentp,
		unsigned int max_nr, fpb_t flags)
{
	bool any_writable = false, any_young = false, any_dirty = false;
	pte_t expected_pte, pte = *ptentp;
	unsigned int nr, cur_nr;

	VM_WARN_ON_FOLIO(!pte_present(pte), folio);
	VM_WARN_ON_FOLIO(!folio_test_large(folio) || max_nr < 1, folio);
	VM_WARN_ON_FOLIO(page_folio(pfn_to_page(pte_pfn(pte))) != folio, folio);
	/*
	 * Ensure this is a pointer to a copy not a pointer into a page table.
	 * If this is a stack value, it won't be a valid virtual address, but
	 * that's fine because it also cannot be pointing into the page table.
	 */
	/* 警告调用者若误传页表内地址；栈副本即使不是有效直接映射地址也符合契约。 */
	VM_WARN_ON(virt_addr_valid(ptentp) && PageTable(virt_to_page(ptentp)));

	/* Limit max_nr to the actual remaining PFNs in the folio we could batch. */
	/* 扫描上限还要裁到 folio 末尾，防止连续 PFN 比较越过对象。 */
	max_nr = min_t(unsigned long, max_nr,
		       folio_pfn(folio) + folio_nr_pages(folio) - pte_pfn(pte));

	nr = pte_batch_hint(ptep, pte);
	expected_pte = __pte_batch_clear_ignored(pte_advance_pfn(pte, nr), flags);
	ptep = ptep + nr;

	/* 从架构批提示后的下一项开始，逐批比较规范化 PTE 与期望连续 PFN。 */
	while (nr < max_nr) {
		pte = ptep_get(ptep);

		if (!pte_same(__pte_batch_clear_ignored(pte, flags), expected_pte))
			break;

		/* 比较时可忽略的访问属性仍按请求累积，供代表性 PTE 恢复最宽松状态。 */
		if (flags & FPB_MERGE_WRITE)
			any_writable |= pte_write(pte);
		if (flags & FPB_MERGE_YOUNG_DIRTY) {
			any_young |= pte_young(pte);
			any_dirty |= pte_dirty(pte);
		}

		cur_nr = pte_batch_hint(ptep, pte);
		/* hint 可一次跳过架构保证相同的连续槽，同时推进期望 PFN 和总数。 */
		expected_pte = pte_advance_pfn(expected_pte, cur_nr);
		ptep += cur_nr;
		nr += cur_nr;
	}

	/* 扫描阶段只累计 OR 状态，结束后统一合并到调用者持有的首项副本。 */
	if (any_writable)
		*ptentp = pte_mkwrite(*ptentp, vma);
	if (any_young)
		*ptentp = pte_mkyoung(*ptentp);
	if (any_dirty)
		*ptentp = pte_mkdirty(*ptentp);

	return min(nr, max_nr);
}

/* folio_pte_batch() 是采用默认比较策略的非内联入口。 */
unsigned int folio_pte_batch(struct folio *folio, pte_t *ptep, pte_t pte,
		unsigned int max_nr);

/**
 * pte_move_swp_offset - Move the swap entry offset field of a swap pte
 *	 forward or backward by delta
 * @pte: The initial pte state; must be a swap entry
 * @delta: The direction and the offset we are moving; forward if delta
 *	 is positive; backward if delta is negative
 *
 * Moves the swap offset, while maintaining all other fields, including
 * swap type, and any swp pte bits. The resulting pte is returned.
 */
/*
 * pte_move_swp_offset() - 保持 swap type 与软件 PTE 位，仅按有符号 delta 移动 offset。
 * @pte 必须是 swap entry；返回新 PTE，不修改页表槽位，调用者负责范围有效性。
 */
static inline pte_t pte_move_swp_offset(pte_t pte, long delta)
{
	const softleaf_t entry = softleaf_from_pte(pte);
	pte_t new = __swp_entry_to_pte(__swp_entry(swp_type(entry),
						   (swp_offset(entry) + delta)));

	/* offset 重建会丢软件位，按原 PTE 逐项恢复 soft-dirty、exclusive 与 UFFD-WP。 */
	if (pte_swp_soft_dirty(pte))
		new = pte_swp_mksoft_dirty(new);
	if (pte_swp_exclusive(pte))
		new = pte_swp_mkexclusive(new);
	if (pte_swp_uffd_wp(pte))
		new = pte_swp_mkuffd_wp(new);

	return new;
}


/**
 * pte_next_swp_offset - Increment the swap entry offset field of a swap pte.
 * @pte: The initial pte state; must be a swap entry.
 *
 * Increments the swap offset, while maintaining all other fields, including
 * swap type, and any swp pte bits. The resulting pte is returned.
 */
/* pte_next_swp_offset() 返回 offset 加一且完整保留 swap 软件位的新 PTE 值。 */
static inline pte_t pte_next_swp_offset(pte_t pte)
{
	return pte_move_swp_offset(pte, 1);
}

/**
 * swap_pte_batch - detect a PTE batch for a set of contiguous swap entries
 * @start_ptep: Page table pointer for the first entry.
 * @max_nr: The maximum number of table entries to consider.
 * @pte: Page table entry for the first entry.
 *
 * Detect a batch of contiguous swap entries: consecutive (non-present) PTEs
 * containing swap entries all with consecutive offsets and targeting the same
 * swap type, all with matching swp pte bits.
 *
 * max_nr must be at least one and must be limited by the caller so scanning
 * cannot exceed a single page table.
 *
 * Return: the number of table entries in the batch.
 */
/*
 * swap_pte_batch() - 统计同一类型、连续 offset 且软件位完全相同的 swap PTE 批次。
 * 扫描不超过调用者限定的单页表 max_nr；返回至少 1，不修改任何页表项。
 */
static inline int swap_pte_batch(pte_t *start_ptep, int max_nr, pte_t pte)
{
	pte_t expected_pte = pte_next_swp_offset(pte);
	const pte_t *end_ptep = start_ptep + max_nr;
	pte_t *ptep = start_ptep + 1;

	VM_WARN_ON(max_nr < 1);
	VM_WARN_ON(!softleaf_is_swap(softleaf_from_pte(pte)));

	/* 期望值每轮只推进一个 offset，pte_same 同时约束类型与全部软件位。 */
	while (ptep < end_ptep) {
		pte = ptep_get(ptep);

		if (!pte_same(pte, expected_pte))
			break;
		expected_pte = pte_next_swp_offset(expected_pte);
		ptep++;
	}

	/* 指针差即包含首项在内的批次数，首项契约保证结果至少为一。 */
	return ptep - start_ptep;
}
#endif /* CONFIG_MMU */

void __acct_reclaim_writeback(pg_data_t *pgdat, struct folio *folio,
						int nr_throttled);
/* acct_reclaim_writeback() 仅在本节点确有被回写限流者时记录一次回收写回事件。 */
static inline void acct_reclaim_writeback(struct folio *folio)
{
	pg_data_t *pgdat = folio_pgdat(folio);
	int nr_throttled = atomic_read(&pgdat->nr_writeback_throttled);

	if (nr_throttled)
		__acct_reclaim_writeback(pgdat, folio, nr_throttled);
}

/* wake_throttle_isolated() 在隔离限流队列有等待者时唤醒，空队列避免无谓锁操作。 */
static inline void wake_throttle_isolated(pg_data_t *pgdat)
{
	wait_queue_head_t *wqh;

	wqh = &pgdat->reclaim_wait[VMSCAN_THROTTLE_ISOLATED];
	if (waitqueue_active(wqh))
		wake_up(wqh);
}

vm_fault_t __vmf_anon_prepare(struct vm_fault *vmf);
/* vmf_anon_prepare() 转交匿名 fault 准备；返回 RETRY 时配套结束 VMA 读锁。 */
static inline vm_fault_t vmf_anon_prepare(struct vm_fault *vmf)
{
	vm_fault_t ret = __vmf_anon_prepare(vmf);

	if (unlikely(ret & VM_FAULT_RETRY))
		vma_end_read(vmf->vma);
	return ret;
}

/* 下列 memory/vmscan 内部入口分别处理 swap fault、LRU 状态、回写完成与页表释放。 */
vm_fault_t do_swap_page(struct vm_fault *vmf);
void folio_rotate_reclaimable(struct folio *folio);
bool __folio_end_writeback(struct folio *folio);
void deactivate_file_folio(struct folio *folio);
void folio_activate(struct folio *folio);

void free_pgtables(struct mmu_gather *tlb, struct unmap_desc *desc);

/* pmd_install() 在正确页表锁协议下把预分配 PTE 页发布到空 PMD。 */
void pmd_install(struct mm_struct *mm, pmd_t *pmd, pgtable_t *pte);

/**
 * sync_with_folio_pmd_zap - sync with concurrent zapping of a folio PMD
 * @mm: The mm_struct.
 * @pmdp: Pointer to the pmd that was found to be pmd_none().
 *
 * When we find a pmd_none() while unmapping a folio without holding the PTL,
 * zap_huge_pmd() may have cleared the PMD but not yet modified the folio to
 * indicate that it's unmapped. Skipping the PMD without synchronization could
 * make folio unmapping code assume that unmapping failed.
 *
 * Wait for concurrent zapping to complete by grabbing the PTL.
 */
/*
 * sync_with_folio_pmd_zap() - 对已观察为空的 PMD 取得并释放 PTL，与并发 huge PMD zap 同步。
 * 返回后 zap 对 folio 映射状态的更新已可见；不持锁返回，也不修改 PMD。
 */
static inline void sync_with_folio_pmd_zap(struct mm_struct *mm, pmd_t *pmdp)
{
	spinlock_t *ptl = pmd_lock(mm, pmdp);

	spin_unlock(ptl);
}

/* zap/unmap 接口组由调用者持 mmap/VMA 所需锁，并用 mmu_gather 批量延迟 TLB 释放。 */
struct zap_details;
void zap_vma_range_batched(struct mmu_gather *tlb,
		struct vm_area_struct *vma, unsigned long addr,
		unsigned long size, struct zap_details *details);
int zap_vma_for_reaping(struct vm_area_struct *vma);
int folio_unmap_invalidate(struct address_space *mapping, struct folio *folio,
			   gfp_t gfp);

/* page-cache 预读入口借用 mapping/file 状态，可能分配并发起文件 IO。 */
void page_cache_ra_order(struct readahead_control *, struct file_ra_state *);
void force_page_cache_ra(struct readahead_control *, unsigned long nr);
/* force_page_cache_readahead() 在栈上构造 ractl，从 index 强制预读至多 nr_to_read 页。 */
static inline void force_page_cache_readahead(struct address_space *mapping,
		struct file *file, pgoff_t index, unsigned long nr_to_read)
{
	DEFINE_READAHEAD(ractl, file, &file->f_ra, mapping, index);
	force_page_cache_ra(&ractl, nr_to_read);
}

/* page-cache 查询/截断接口会按各自契约取得 folio 引用或锁，并通过批次/索引数组返回结果。 */
unsigned find_lock_entries(struct address_space *mapping, pgoff_t *start,
		pgoff_t end, struct folio_batch *fbatch, pgoff_t *indices);
unsigned find_get_entries(struct address_space *mapping, pgoff_t *start,
		pgoff_t end, struct folio_batch *fbatch, pgoff_t *indices);
int truncate_inode_folio(struct address_space *mapping, struct folio *folio);
bool truncate_inode_partial_folio(struct folio *folio, loff_t start,
		loff_t end);
long mapping_evict_folio(struct address_space *mapping, struct folio *folio);
unsigned long mapping_try_invalidate(struct address_space *mapping,
		pgoff_t start, pgoff_t end, unsigned long *nr_failed);

/**
 * folio_evictable - Test whether a folio is evictable.
 * @folio: The folio to test.
 *
 * Test whether @folio is evictable -- i.e., should be placed on
 * active/inactive lists vs unevictable list.
 *
 * Reasons folio might not be evictable:
 * 1. folio's mapping marked unevictable
 * 2. One of the pages in the folio is part of an mlocked VMA
 */
/*
 * folio_evictable() - 判断 folio 应进入可回收 LRU 还是 unevictable LRU。
 * RCU 临界区稳定 mapping；mapping 标记不可回收或任一子页 mlocked 都返回 false，不改变 folio。
 */
static inline bool folio_evictable(struct folio *folio)
{
	bool ret;

	/* Prevent address_space of inode and swap cache from being freed */
	/* RCU 防止 inode 或 swap-cache 的 address_space 在判定期间释放。 */
	rcu_read_lock();
	ret = !mapping_unevictable(folio_mapping(folio)) &&
			!folio_test_mlocked(folio);
	rcu_read_unlock();
	return ret;
}

/*
 * Turn a non-refcounted page (->_refcount == 0) into refcounted with
 * a count of one.
 */
/* set_page_refcounted() 只把非 tail 且引用为零的 page 转为单引用，违约触发 VM_BUG。 */
static inline void set_page_refcounted(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	VM_BUG_ON_PAGE(page_ref_count(page), page);
	set_page_count(page, 1);
}

/* set_pages_refcounted() 按连续 PFN 对 nr_pages 个独立 page 执行零到一的引用初始化。 */
static inline void set_pages_refcounted(struct page *page, unsigned long nr_pages)
{
	unsigned long pfn = page_to_pfn(page);

	for (; nr_pages--; pfn++)
		set_page_refcounted(pfn_to_page(pfn));
}

/*
 * Return true if a folio needs ->release_folio() calling upon it.
 */
/* folio_needs_release() 在 folio 有 private 数据或 mapping 总要求回调时返回 true。 */
static inline bool folio_needs_release(struct folio *folio)
{
	struct address_space *mapping = folio_mapping(folio);

	return folio_has_private(folio) ||
		(mapping && mapping_release_always(mapping));
}

/* highest_memmap_pfn 记录存在 struct page 元数据的最高 PFN 上界。 */
extern unsigned long highest_memmap_pfn;

/*
 * Maximum number of reclaim retries without progress before the OOM
 * killer is consider the only way forward.
 */
/* 连续无进展达到 16 轮后，直接回收把 OOM killer 视为唯一前进方式。 */
#define MAX_RECLAIM_RETRIES 16

/*
 * in mm/vmscan.c:
 */
/* vmscan 内部接口负责 LRU 隔离/放回、回收限流和用户主动回收请求。 */
bool folio_isolate_lru(struct folio *folio);
void folio_putback_lru(struct folio *folio);
extern void reclaim_throttle(pg_data_t *pgdat, enum vmscan_throttle_state reason);
int user_proactive_reclaim(char *buf,
			   struct mem_cgroup *memcg, pg_data_t *pgdat);

/*
 * in mm/rmap.c:
 */
/* mm_find_pmd() 查询 address 所在 PMD，调用者负责 mm 页表生命周期与后续锁定。 */
pmd_t *mm_find_pmd(struct mm_struct *mm, unsigned long address);

/*
 * in mm/khugepaged.c
 */
/* set_recommended_min_free_kbytes() 按 THP/khugepaged 需求提高建议的最小空闲水位。 */
void set_recommended_min_free_kbytes(void);

/*
 * in mm/page_alloc.c
 */
/* K(x) 把以页为单位的值换算为 KiB；仅在 PAGE_SHIFT 至少为 10 时使用。 */
#define K(x) ((x) << (PAGE_SHIFT-10))

/* zone_names 与水位调节项由 page_alloc.c 拥有，这里只暴露 mm 内部配置入口。 */
extern char * const zone_names[MAX_NR_ZONES];

/* perform sanity checks on struct pages being allocated or freed */
/* 静态键启用后，在页分配/释放路径执行 struct page 一致性检查。 */
DECLARE_STATIC_KEY_MAYBE(CONFIG_DEBUG_VM, check_pages_enabled);

extern int min_free_kbytes;
extern int defrag_mode;

/* 水位接口重新计算各 zone 阈值并注册相关 sysctl，初始化期可带 __meminit。 */
void setup_per_zone_wmarks(void);
void calculate_min_free_kbytes(void);
int __meminit init_per_zone_wmark_min(void);
void page_alloc_sysctl_init(void);

/*
 * Structure for holding the mostly immutable allocation parameters passed
 * between functions involved in allocations, including the alloc_pages*
 * family of functions.
 *
 * nodemask, migratetype and highest_zoneidx are initialized only once in
 * __alloc_pages() and then never change.
 *
 * zonelist, preferred_zone and highest_zoneidx are set first in
 * __alloc_pages() for the fast path, and might be later changed
 * in __alloc_pages_slowpath(). All other functions pass the whole structure
 * by a const pointer.
 */
/*
 * alloc_context 汇总一次页分配的 zonelist、节点掩码、首选 zone、迁移类型和最高可用 zone。
 * 快路初始化后大多只读；慢路可调整 zonelist/首选 zone，但 placement 不变量保持有效。
 */
struct alloc_context {
	struct zonelist *zonelist;
	nodemask_t *nodemask;
	struct zoneref *preferred_zoneref;
	int migratetype;

	/*
	 * highest_zoneidx represents highest usable zone index of
	 * the allocation request. Due to the nature of the zone,
	 * memory on lower zone than the highest_zoneidx will be
	 * protected by lowmem_reserve[highest_zoneidx].
	 *
	 * highest_zoneidx is also used by reclaim/compaction to limit
	 * the target zone since higher zone than this index cannot be
	 * usable for this allocation request.
	 */
	/* 该上界同时决定 lowmem_reserve 保护和回收/压缩不得越过的最高 zone。 */
	enum zone_type highest_zoneidx;
	/* spread_dirty_pages 要求脏页在允许节点间扩散，避免单节点写回压力。 */
	bool spread_dirty_pages;
};

/*
 * This function returns the order of a free page in the buddy system. In
 * general, page_zone(page)->lock must be held by the caller to prevent the
 * page from being allocated in parallel and returning garbage as the order.
 * If a caller does not hold page_zone(page)->lock, it must guarantee that the
 * page cannot be allocated or merged in parallel. Alternatively, it must
 * handle invalid values gracefully, and use buddy_order_unsafe() below.
 */
/*
 * buddy_order() - 在 zone 锁或等价排他保证下读取空闲伙伴块 order。
 * 调用者先确认 PageBuddy；返回 page_private，不进行并发防护。
 */
static inline unsigned int buddy_order(struct page *page)
{
	/* PageBuddy() must be checked by the caller */
	/* 此处不重复检查，热路径由调用者合并 PageBuddy 与锁条件。 */
	return page_private(page);
}

/*
 * Like buddy_order(), but for callers who cannot afford to hold the zone lock.
 * PageBuddy() should be checked first by the caller to minimize race window,
 * and invalid values must be handled gracefully.
 *
 * READ_ONCE is used so that if the caller assigns the result into a local
 * variable and e.g. tests it for valid range before using, the compiler cannot
 * decide to remove the variable and inline the page_private(page) multiple
 * times, potentially observing different values in the tests and the actual
 * use of the result.
 */
/* buddy_order_unsafe() 用 READ_ONCE 取得可能竞争的 order；调用者必须校验范围并容忍失效值。 */
#define buddy_order_unsafe(page)	READ_ONCE(page_private(page))

/*
 * This function checks whether a page is free && is the buddy
 * we can coalesce a page and its buddy if
 * (a) the buddy is not in a hole (check before calling!) &&
 * (b) the buddy is in the buddy system &&
 * (c) a page and its buddy have the same order &&
 * (d) a page and its buddy are in the same zone.
 *
 * For recording whether a page is in the buddy system, we set PageBuddy.
 * Setting, clearing, and testing PageBuddy is serialized by zone->lock.
 *
 * For recording page's order, we use page_private(page).
 */
/*
 * page_is_buddy() - 在 zone 锁下验证 buddy 可与 page 按给定 order 合并。
 * guard page 或 PageBuddy、相同 order 与 zone 均必须满足；成功还断言 buddy 引用为零。
 */
static inline bool page_is_buddy(struct page *page, struct page *buddy,
				 unsigned int order)
{
	if (!page_is_guard(buddy) && !PageBuddy(buddy))
		return false;

	if (buddy_order(buddy) != order)
		return false;

	/*
	 * zone check is done late to avoid uselessly calculating
	 * zone/node ids for pages that could never merge.
	 */
	/* 先排除状态/order 不符者，再计算相对昂贵的 zone id。 */
	if (page_zone_id(page) != page_zone_id(buddy))
		return false;

	VM_BUG_ON_PAGE(page_count(buddy) != 0, buddy);

	return true;
}

/*
 * Locate the struct page for both the matching buddy in our
 * pair (buddy1) and the combined O(n+1) page they form (page).
 *
 * 1) Any buddy B1 will have an order O twin B2 which satisfies
 * the following equation:
 *     B2 = B1 ^ (1 << O)
 * For example, if the starting buddy (buddy2) is #8 its order
 * 1 buddy is #10:
 *     B2 = 8 ^ (1 << 1) = 8 ^ 2 = 10
 *
 * 2) Any buddy B will have an order O+1 parent P which
 * satisfies the following equation:
 *     P = B & ~(1 << O)
 *
 * Assumption: *_mem_map is contiguous at least up to MAX_PAGE_ORDER
 */
/* __find_buddy_pfn() 用第 order 位异或得到伙伴 PFN；要求 mem_map 在最大伙伴阶内连续。 */
static inline unsigned long
__find_buddy_pfn(unsigned long page_pfn, unsigned int order)
{
	return page_pfn ^ (1 << order);
}

/*
 * Find the buddy of @page and validate it.
 * @page: The input page
 * @pfn: The pfn of the page, it saves a call to page_to_pfn() when the
 *       function is used in the performance-critical __free_one_page().
 * @order: The order of the page
 * @buddy_pfn: The output pointer to the buddy pfn, it also saves a call to
 *             page_to_pfn().
 *
 * The found buddy can be a non PageBuddy, out of @page's zone, or its order is
 * not the same as @page. The validation is necessary before use it.
 *
 * Return: the found buddy page or NULL if not found.
 */
/*
 * find_buddy_page_pfn() - 由 PFN 算出候选伙伴并用 page_is_buddy() 校验。
 * 可选输出 buddy_pfn；返回稳定性继承调用者的 zone 锁/排他保证，失败为 NULL。
 */
static inline struct page *find_buddy_page_pfn(struct page *page,
			unsigned long pfn, unsigned int order, unsigned long *buddy_pfn)
{
	unsigned long __buddy_pfn = __find_buddy_pfn(pfn, order);
	struct page *buddy;

	/* mem_map 连续性保证用 PFN 差在 page 指针上定位同阶伙伴。 */
	buddy = page + (__buddy_pfn - pfn);
	if (buddy_pfn)
		*buddy_pfn = __buddy_pfn;

	if (page_is_buddy(page, buddy, order))
		return buddy;
	return NULL;
}

/* pageblock PFN helper 验证非连续 zone 范围，连续 zone 则可直接取起始 page。 */
extern struct page *__pageblock_pfn_to_page(unsigned long start_pfn,
				unsigned long end_pfn, struct zone *zone);

/* pageblock_pfn_to_page() 返回可代表完整 pageblock 的首 page，跨空洞时可能为 NULL。 */
static inline struct page *pageblock_pfn_to_page(unsigned long start_pfn,
				unsigned long end_pfn, struct zone *zone)
{
	if (zone->contiguous)
		return pfn_to_page(start_pfn);

	return __pageblock_pfn_to_page(start_pfn, end_pfn, zone);
}

/* zone contiguous 标志的设置需完成 PFN 范围验证；清除是单向保守退化。 */
void set_zone_contiguous(struct zone *zone);
bool pfn_range_intersects_zones(int nid, unsigned long start_pfn,
			   unsigned long nr_pages);

/* clear_zone_contiguous() 让后续 pageblock 查询恢复逐范围验证。 */
static inline void clear_zone_contiguous(struct zone *zone)
{
	zone->contiguous = false;
}

/* 页分配内部隔离、归还和 memblock/core free 接口由调用者满足 zone/初始化阶段锁约束。 */
extern int __isolate_free_page(struct page *page, unsigned int order);
extern void __putback_isolated_page(struct page *page, unsigned int order,
				    int mt);
extern void memblock_free_pages(unsigned long pfn, unsigned int order);
extern void __free_pages_core(struct page *page, unsigned int order,
		enum meminit_context context);

/*
 * This will have no effect, other than possibly generating a warning, if the
 * caller passes in a non-large folio.
 */
/* folio_set_order() 初始化大 folio 的 order/页数；非大页或零 order 只告警并返回。 */
static inline void folio_set_order(struct folio *folio, unsigned int order)
{
	if (WARN_ON_ONCE(!order || !folio_test_large(folio)))
		return;
	VM_WARN_ON_ONCE(order > MAX_FOLIO_ORDER);

	folio->_flags_1 = (folio->_flags_1 & ~0xffUL) | order;
#ifdef NR_PAGES_IN_LARGE_FOLIO
	folio->_nr_pages = 1U << order;
#endif
}

/* __folio_unqueue_deferred_split() 是需取得 list_lru 锁的慢路；包装器先做无锁保守筛选。 */
bool __folio_unqueue_deferred_split(struct folio *folio);
/* folio_unqueue_deferred_split() 仅对可拆的大型可映射 folio 尝试摘除，返回是否成功。 */
static inline bool folio_unqueue_deferred_split(struct folio *folio)
{
	if (folio_order(folio) <= 1 || !folio_test_large_rmappable(folio))
		return false;

	/*
	 * At this point, there is no one trying to add the folio to
	 * deferred_list. If folio is not in deferred_list, it's safe
	 * to check without acquiring the list_lru lock.
	 */
	/* 此阶段无人再入队，data_race 读到空即可安全返回；非空交给带锁慢路复核。 */
	if (data_race(list_empty(&folio->_deferred_list)))
		return false;

	return __folio_unqueue_deferred_split(folio);
}

/* page_rmappable_folio() 把 page 视为 folio，并为大 folio 发布 large-rmappable 属性。 */
static inline struct folio *page_rmappable_folio(struct page *page)
{
	struct folio *folio = (struct folio *)page;

	if (folio && folio_test_large(folio))
		folio_set_large_rmappable(folio);
	return folio;
}

/* prep_compound_head() 初始化新 compound head 的 order、mapcount、pin 与 deferred-list 元数据。 */
static inline void prep_compound_head(struct page *page, unsigned int order)
{
	struct folio *folio = (struct folio *)page;

	folio_set_order(folio, order);
	atomic_set(&folio->_large_mapcount, -1);
	/* 可选逐页 mapcount 与 MM_ID 槽按“尚无映射”的哨兵值初始化。 */
	if (IS_ENABLED(CONFIG_PAGE_MAPCOUNT))
		atomic_set(&folio->_nr_pages_mapped, 0);
	if (IS_ENABLED(CONFIG_MM_ID)) {
		folio->_mm_ids = 0;
		folio->_mm_id_mapcount[0] = -1;
		folio->_mm_id_mapcount[1] = -1;
	}
	/* pincount/entire_mapcount 只在布局提供独立字段时初始化，随后建立延迟拆分链头。 */
	if (IS_ENABLED(CONFIG_64BIT) || order > 1) {
		atomic_set(&folio->_pincount, 0);
		atomic_set(&folio->_entire_mapcount, -1);
	}
	if (order > 1)
		INIT_LIST_HEAD(&folio->_deferred_list);
}

/* prep_compound_tail() 设置 tail 的 head 编码、TAIL_MAPPING 哨兵并清 private。 */
static inline void prep_compound_tail(struct page *tail,
		const struct page *head, unsigned int order)
{
	tail->mapping = TAIL_MAPPING;
	set_compound_head(tail, head, order);
	set_page_private(tail, 0);
}

/* init_compound_tail() 在 prep 基础上初始化 tail mapcount 与所属 NUMA node/zone。 */
static inline void init_compound_tail(struct page *tail,
		const struct page *head, unsigned int order, struct zone *zone)
{
	atomic_set(&tail->_mapcount, -1);
	set_page_node(tail, zone_to_nid(zone));
	set_page_zone(tail, zone_idx(zone));
	prep_compound_tail(tail, head, order);
}

/* 分配/释放 frozen pages 接口以冻结引用状态交给 CMA/迁移等内部调用者，必须成对归还。 */
void post_alloc_hook(struct page *page, unsigned int order, gfp_t gfp_flags);
extern bool free_pages_prepare(struct page *page, unsigned int order);

extern int user_min_free_kbytes;

/* noprof 原语由 alloc_hooks 包装以接入分配观测；释放接口接收仍冻结的引用状态。 */
struct page *__alloc_frozen_pages_noprof(gfp_t, unsigned int order, int nid,
		nodemask_t *);
#define __alloc_frozen_pages(...) \
	alloc_hooks(__alloc_frozen_pages_noprof(__VA_ARGS__))
void free_frozen_pages(struct page *page, unsigned int order);
void free_unref_folios(struct folio_batch *fbatch);

#ifdef CONFIG_NUMA
/* NUMA 实现自行选择允许节点；非 NUMA 分支在下方固定当前节点。 */
struct page *alloc_frozen_pages_noprof(gfp_t, unsigned int order);
#else
/* 非 NUMA 构建固定从当前伪节点分配，不传节点掩码。 */
static inline struct page *alloc_frozen_pages_noprof(gfp_t gfp, unsigned int order)
{
	return __alloc_frozen_pages_noprof(gfp, order, numa_node_id(), NULL);
}
#endif

#define alloc_frozen_pages(...) \
	alloc_hooks(alloc_frozen_pages_noprof(__VA_ARGS__))

/* nolock 变体供已满足外部同步的连续内存路径使用，必须用对应 free_nolock 归还。 */
struct page *alloc_frozen_pages_nolock_noprof(gfp_t gfp_flags, int nid, unsigned int order);
#define alloc_frozen_pages_nolock(...) \
	alloc_hooks(alloc_frozen_pages_nolock_noprof(__VA_ARGS__))
void free_frozen_pages_nolock(struct page *page, unsigned int order);

/* zone_pcp_* 在热插拔/初始化边界停用、重建或启用 per-CPU page cache。 */
extern void zone_pcp_reset(struct zone *zone);
extern void zone_pcp_disable(struct zone *zone);
extern void zone_pcp_enable(struct zone *zone);
extern void zone_pcp_init(struct zone *zone);

/* memmap_alloc() 从指定或可回退节点取得对齐的 memmap backing；失败返回 NULL。 */
extern void *memmap_alloc(phys_addr_t size, phys_addr_t align,
			  phys_addr_t min_addr,
			  int nid, bool exact_nid);

void memmap_init_range(unsigned long, int, unsigned long, unsigned long,
		unsigned long, enum meminit_context, struct vmem_altmap *, int,
		bool);

/*
 * mm/sparse.c
 */
/* sparse.c 提供 section 索引和 mem_map 编码的启动期初始化。 */
#ifdef CONFIG_SPARSEMEM
void sparse_init(void);
int sparse_index_init(unsigned long section_nr, int nid);

/*
 * sparse_init_one_section() - 发布一个 sparse section 的编码 mem_map、usage 与存在标志。
 * 调用者在启动/热插拔串行域内提供已分配元数据；函数不取得引用，越界编码触发警告。
 */
static inline void sparse_init_one_section(struct mem_section *ms,
		unsigned long pnum, struct page *mem_map,
		struct mem_section_usage *usage, unsigned long flags)
{
	unsigned long coded_mem_map;

	BUILD_BUG_ON(SECTION_MAP_LAST_BIT > PFN_SECTION_SHIFT);

	/*
	 * We encode the start PFN of the section into the mem_map such that
	 * page_to_pfn() on !CONFIG_SPARSEMEM_VMEMMAP can simply subtract it
	 * from the page pointer to obtain the PFN.
	 */
	/* 非 VMEMMAP 模式把 section 起始 PFN 偏差编码进指针低层表示，便于 page_to_pfn 做减法。 */
	coded_mem_map = (unsigned long)(mem_map - section_nr_to_pfn(pnum));
	VM_WARN_ON_ONCE(coded_mem_map & ~SECTION_MAP_MASK);

	ms->section_mem_map &= ~SECTION_MAP_MASK;
	ms->section_mem_map |= coded_mem_map;
	ms->section_mem_map |= flags | SECTION_HAS_MEM_MAP;
	ms->usage = usage;
}

/* __section_mark_present() 更新全局最高 present section，并在目标 section 设置 present 位。 */
static inline void __section_mark_present(struct mem_section *ms,
		unsigned long section_nr)
{
	if (section_nr > __highest_present_section_nr)
		__highest_present_section_nr = section_nr;

	ms->section_mem_map |= SECTION_MARKED_PRESENT;
}
#else
/* 非 SPARSEMEM 无 section 表需要初始化，空桩无副作用。 */
static inline void sparse_init(void) {}
#endif /* CONFIG_SPARSEMEM */

/*
 * mm/sparse-vmemmap.c
 */
/* sparse-vmemmap 子段位图只在 VMEMMAP 配置有实体实现。 */
#ifdef CONFIG_SPARSEMEM_VMEMMAP
void sparse_init_subsection_map(unsigned long pfn, unsigned long nr_pages);
#else
/* 非 VMEMMAP 构建没有 subsection map，参数仅为接口兼容。 */
static inline void sparse_init_subsection_map(unsigned long pfn,
		unsigned long nr_pages)
{
}
#endif /* CONFIG_SPARSEMEM_VMEMMAP */

#if defined CONFIG_COMPACTION || defined CONFIG_CMA

/*
 * in mm/compaction.c
 */
/* 以下控制块和范围隔离接口由 compaction/CMA 共享。 */
/*
 * compact_control is used to track pages being migrated and the free pages
 * they are being migrated to during memory compaction. The free_pfn starts
 * at the end of a zone and migrate_pfn begins at the start. Movable pages
 * are moved to the end of a zone during a compaction run and the run
 * completes when free_pfn <= migrate_pfn
 */
/*
 * compact_control 保存一次压缩运行的双向扫描游标、隔离页队列、分配约束与进度/竞争状态。
 * migrate_pfn 从低端向上，free_pfn 从高端向下；二者相遇即完成本 zone 扫描。
 */
struct compact_control {
	struct list_head freepages[NR_PAGE_ORDERS];	/* List of free pages to migrate to */
	/* freepages 按 order 保存作为迁移目标的已隔离空闲页。 */
	struct list_head migratepages;	/* List of pages being migrated */
	/* migratepages 保存待搬出的已隔离源页。 */
	unsigned int nr_freepages;	/* Number of isolated free pages */
	/* nr_freepages 按 base page 数量统计所有目标队列容量。 */
	unsigned int nr_migratepages;	/* Number of pages to migrate */
	/* nr_migratepages 是当前源批次的 base page 数。 */
	unsigned long free_pfn;		/* isolate_freepages search base */
	/* free_pfn 是高端空闲页扫描的下一起点。 */
	/*
	 * Acts as an in/out parameter to page isolation for migration.
	 * isolate_migratepages uses it as a search base.
	 * isolate_migratepages_block will update the value to the next pfn
	 * after the last isolated one.
	 */
	/* migrate_pfn 既是输入扫描起点，也是隔离后下一未扫描 PFN 输出。 */
	unsigned long migrate_pfn;
	unsigned long fast_start_pfn;	/* a pfn to start linear scan from */
	/* fast_start_pfn 记录快速空闲链搜索退化到线性扫描的位置。 */
	struct zone *zone;
	/* 两个累计扫描量用于判断压缩代价和是否继续。 */
	unsigned long total_migrate_scanned;
	unsigned long total_free_scanned;
	unsigned short fast_search_fail;/* failures to use free list searches */
	/* fast_search_fail 统计 freelist 快搜失败并驱动退避。 */
	short search_order;		/* order to start a fast search at */
	/* search_order 是快速目标页搜索的起始阶。 */
	const gfp_t gfp_mask;		/* gfp mask of a direct compactor */
	/* gfp_mask 继承直接分配者允许的回收与阻塞上下文。 */
	int order;			/* order a direct compactor needs */
	/* order 是最终要满足的连续分配阶。 */
	int migratetype;		/* migratetype of direct compactor */
	/* migratetype 决定目标 pageblock 的分配用途。 */
	const unsigned int alloc_flags;	/* alloc flags of a direct compactor */
	/* alloc_flags 保存水位、cpuset 与 reserve 访问策略。 */
	const int highest_zoneidx;	/* zone index of a direct compactor */
	/* highest_zoneidx 限定直接压缩可服务的最高 zone。 */
	enum migrate_mode mode;		/* Async or sync migration mode */
	/* mode 决定是否等待锁/回写及迁移同步程度。 */
	bool ignore_skip_hint;		/* Scan blocks even if marked skip */
	/* ignore_skip_hint 允许重新扫描已标记跳过的 pageblock。 */
	bool no_set_skip_hint;		/* Don't mark blocks for skipping */
	/* no_set_skip_hint 禁止本轮把失败块写入持久跳过提示。 */
	bool ignore_block_suitable;	/* Scan blocks considered unsuitable */
	/* ignore_block_suitable 强制检查通常被适配性筛选排除的块。 */
	bool direct_compaction;		/* False from kcompactd or /proc/... */
	/* direct_compaction 区分分配慢路与后台/用户触发。 */
	bool proactive_compaction;	/* kcompactd proactive compaction */
	/* proactive_compaction 标识后台主动压缩目标。 */
	bool whole_zone;		/* Whole zone should/has been scanned */
	/* whole_zone 表示本轮要求或已经覆盖完整 zone。 */
	bool contended;			/* Signal lock contention */
	/* contended 向上层报告因锁竞争提前退让。 */
	bool finish_pageblock;		/* Scan the remainder of a pageblock. Used
					 * when there are potentially transient
					 * isolation or migration failures to
					 * ensure forward progress.
					 */
	/* finish_pageblock 要求扫完当前块，以越过瞬时隔离/迁移失败并保证进展。 */
	bool alloc_contig;		/* alloc_contig_range allocation */
	/* alloc_contig 标识严格连续分配，不能接受普通压缩的局部成功。 */
};

/*
 * Used in direct compaction when a page should be taken from the freelists
 * immediately when one is created during the free path.
 */
/* capture_control 让直接压缩在 free path 新产生合适页时立即截获给当前 compact_control。 */
struct capture_control {
	struct compact_control *cc;
	struct page *page;
};

/* 两个 isolate_range 接口返回下一 PFN/状态，调用者拥有 compact_control 中已隔离队列。 */
unsigned long
isolate_freepages_range(struct compact_control *cc,
			unsigned long start_pfn, unsigned long end_pfn);
int
isolate_migratepages_range(struct compact_control *cc,
			   unsigned long low_pfn, unsigned long end_pfn);

/* Free whole pageblock and set its migration type to MIGRATE_CMA. */
/* init_cma_reserved_pageblock() 释放整块到 CMA freelist 并发布 MIGRATE_CMA 类型。 */
void init_cma_reserved_pageblock(struct page *page);

#endif /* CONFIG_COMPACTION || CONFIG_CMA */

struct cma;

#ifdef CONFIG_CMA
bool cma_validate_zones(struct cma *cma);
void *cma_reserve_early(struct cma *cma, unsigned long size);
void init_cma_pageblock(struct page *page);
#else
/* cma_validate_zones() 空桩恒 false，表示未启用 CMA 时任何区域都不能进入 CMA 生命周期。 */
static inline bool cma_validate_zones(struct cma *cma)
{
	return false;
}
/* cma_reserve_early() 空桩返回 NULL，明确表示没有取得早期保留区 owner。 */
static inline void *cma_reserve_early(struct cma *cma, unsigned long size)
{
	return NULL;
}
/* init_cma_pageblock() 空桩不改变 pageblock migratetype 或 buddy 状态。 */
static inline void init_cma_pageblock(struct page *page)
{
}
#endif

enum fallback_result {
	/* Found suitable migratetype, *mt_out is valid. */
	/* 找到可用迁移类型，调用者可读取 mt_out。 */
	FALLBACK_FOUND,
	/* No fallback found in requested order. */
	/* 请求 order 上没有任何候选 fallback。 */
	FALLBACK_EMPTY,
	/* Passed @claimable, but claiming whole block is a bad idea. */
	/* 虽允许 claim，但整块改类型会造成不合算的碎片化。 */
	FALLBACK_NOCLAIM,
};
/* find_suitable_fallback() 在指定 free_area 中选择迁移类型，并按结果决定 mt_out 是否有效。 */
enum fallback_result
find_suitable_fallback(struct free_area *area, unsigned int order,
		       int migratetype, bool claimable, int *mt_out);

/* free_area_empty() 只测试给定 order/migratetype 链表是否为空，调用者稳定 freelist。 */
static inline bool free_area_empty(struct free_area *area, int migratetype)
{
	return list_empty(&area->free_list[migratetype]);
}

/* mm/util.c */
/* folio_anon_vma() 仅查询当前 anon_vma；稳定引用需求应使用 folio_get_anon_vma()。 */
struct anon_vma *folio_anon_vma(const struct folio *folio);

#ifdef CONFIG_MMU
void unmap_mapping_folio(struct folio *folio);
extern long populate_vma_page_range(struct vm_area_struct *vma,
		unsigned long start, unsigned long end, int *locked);
extern long faultin_page_range(struct mm_struct *mm, unsigned long start,
		unsigned long end, bool write, int *locked);
/* populate/mlock 接口可能 fault-in 页并按 locked 输出 mmap 锁是否仍由调用者持有。 */
bool mlock_future_ok(const struct mm_struct *mm, bool is_vma_locked,
		unsigned long bytes);

/*
 * NOTE: This function can't tell whether the folio is "fully mapped" in the
 * range.
 * "fully mapped" means all the pages of folio is associated with the page
 * table of range while this function just check whether the folio range is
 * within the range [start, end). Function caller needs to do page table
 * check if it cares about the page table association.
 *
 * Typical usage (like mlock or madvise) is:
 * Caller knows at least 1 page of folio is associated with page table of VMA
 * and the range [start, end) is intersect with the VMA range. Caller wants
 * to know whether the folio is fully associated with the range. It calls
 * this function to check whether the folio is in the range first. Then checks
 * the page table to know whether the folio is fully mapped to the range.
 */
/*
 * folio_within_range() - 只判断 folio 的对象偏移范围能否完整落入 VMA 与请求范围交集。
 * 它不证明每个子页都有 PTE；调用者已知至少一页映射并在需要时继续检查页表。KSM folio 不适用。
 */
static inline bool
folio_within_range(struct folio *folio, struct vm_area_struct *vma,
		unsigned long start, unsigned long end)
{
	pgoff_t pgoff, addr;
	unsigned long vma_pglen = vma_pages(vma);

	/* KSM 没有普通对象 pgoff 语义；反向范围或空交集先行拒绝。 */
	VM_WARN_ON_FOLIO(folio_test_ksm(folio), folio);
	if (start > end)
		return false;

	/* 把用户范围裁剪到 VMA 半开区间，再做对象偏移与字节尺寸判断。 */
	if (start < vma->vm_start)
		start = vma->vm_start;

	if (end > vma->vm_end)
		end = vma->vm_end;

	pgoff = folio_pgoff(folio);

	/* if folio start address is not in vma range */
	/* 起始 pgoff 不属于 VMA 时无需继续做地址换算。 */
	if (!in_range(pgoff, vma->vm_pgoff, vma_pglen))
		return false;

	addr = vma->vm_start + ((pgoff - vma->vm_pgoff) << PAGE_SHIFT);

	return !(addr < start || end - addr < folio_size(folio));
}

/* folio_within_vma() 是以完整 VMA 边界调用 folio_within_range() 的便捷包装。 */
static inline bool
folio_within_vma(struct folio *folio, struct vm_area_struct *vma)
{
	return folio_within_range(folio, vma, vma->vm_start, vma->vm_end);
}

/*
 * mlock_vma_folio() and munlock_vma_folio():
 * should be called with vma's mmap_lock held for read or write,
 * under page table lock for the pte/pmd being added or removed.
 *
 * mlock is usually called at the end of folio_add_*_rmap_*(), munlock at
 * the end of folio_remove_rmap_*(); but new anon folios are managed by
 * folio_add_lru_vma() calling mlock_new_folio().
 */
/* mlock/munlock VMA 包装器要求 mmap 读或写锁与对应 PTE/PMD 锁，更新 folio 的 mlock 状态。 */
void mlock_folio(struct folio *folio);
/* mlock_vma_folio() 只为普通 VM_LOCKED VMA 计入，排除迁移重复计数与短暂 VM_SPECIAL 状态。 */
static inline void mlock_vma_folio(struct folio *folio,
				struct vm_area_struct *vma)
{
	/*
	 * The VM_SPECIAL check here serves two purposes.
	 * 1) VM_IO check prevents migration from double-counting during mlock.
	 * 2) Although mmap_region() and mlock_fixup() take care that VM_LOCKED
	 *    is never left set on a VM_SPECIAL vma, there is an interval while
	 *    file->f_op->mmap() is using vm_insert_page(s), when VM_LOCKED may
	 *    still be set while VM_SPECIAL bits are added: so ignore it then.
	 */
	/* VM_IO 防迁移期间双计；文件 mmap 添加 SPECIAL 位的窗口也必须忽略尚未清掉的 LOCKED。 */
	if (unlikely((vma->vm_flags & (VM_LOCKED|VM_SPECIAL)) == VM_LOCKED))
		mlock_folio(folio);
}

void munlock_folio(struct folio *folio);
/* munlock_vma_folio() 对任一 VM_LOCKED 解除映射都保守 munlock，错误由后续 reclaim 重新校正。 */
static inline void munlock_vma_folio(struct folio *folio,
					struct vm_area_struct *vma)
{
	/*
	 * munlock if the function is called. Ideally, we should only
	 * do munlock if any page of folio is unmapped from VMA and
	 * cause folio not fully mapped to VMA.
	 *
	 * But it's not easy to confirm that's the situation. So we
	 * always munlock the folio and page reclaim will correct it
	 * if it's wrong.
	 */
	/* 精确判断 folio 是否仍被 VMA 完整映射代价高，因此宁可多做一次可修正的 munlock。 */
	if (unlikely(vma->vm_flags & VM_LOCKED))
		munlock_folio(folio);
}

/* 新 folio mlock 与 per-CPU drain 接口把延迟状态合并回 folio/LRU。 */
void mlock_new_folio(struct folio *folio);
bool need_mlock_drain(int cpu);
void mlock_drain_local(void);
void mlock_drain_remote(int cpu);

extern pmd_t maybe_pmd_mkwrite(pmd_t pmd, struct vm_area_struct *vma);

/**
 * vma_address - Find the virtual address a page range is mapped at
 * @vma: The vma which maps this object.
 * @pgoff: The page offset within its object.
 * @nr_pages: The number of pages to consider.
 *
 * If any page in this range is mapped by this VMA, return the first address
 * where any of these pages appear.  Otherwise, return -EFAULT.
 */
/*
 * vma_address() - 求对象页范围与 VMA 映射的首个相交虚拟地址。
 * 返回 VMA 内地址或编码 -EFAULT；使用溢出安全分支处理对象范围从 VMA pgoff 前方开始的情况。
 */
static inline unsigned long vma_address(const struct vm_area_struct *vma,
		pgoff_t pgoff, unsigned long nr_pages)
{
	unsigned long address;

	if (pgoff >= vma->vm_pgoff) {
		address = vma->vm_start +
			((pgoff - vma->vm_pgoff) << PAGE_SHIFT);
		/* Check for address beyond vma (or wrapped through 0?) */
		/* 同时拒绝越过 vm_end 与移位/加法回绕到 vm_start 之前。 */
		if (address < vma->vm_start || address >= vma->vm_end)
			address = -EFAULT;
	} else if (pgoff + nr_pages - 1 >= vma->vm_pgoff) {
		/* Test above avoids possibility of wrap to 0 on 32-bit */
		/* 前一分支保证这里的对象范围跨入 VMA；分支顺序避免 32 位加法回绕误命中。 */
		address = vma->vm_start;
	} else {
		address = -EFAULT;
	}
	return address;
}

/*
 * Then at what user virtual address will none of the range be found in vma?
 * Assumes that vma_address() already returned a good starting address.
 */
/* vma_address_end() 返回对象页范围在该 VMA 中最后相交字节之后的地址，并钳到 vm_end。 */
static inline unsigned long vma_address_end(struct page_vma_mapped_walk *pvmw)
{
	struct vm_area_struct *vma = pvmw->vma;
	pgoff_t pgoff;
	unsigned long address;

	/* Common case, plus ->pgoff is invalid for KSM */
	/* 单页快路也规避 KSM 无效 pgoff 的对象地址换算。 */
	if (pvmw->nr_pages == 1)
		return pvmw->address + PAGE_SIZE;

	pgoff = pvmw->pgoff + pvmw->nr_pages;
	address = vma->vm_start + ((pgoff - vma->vm_pgoff) << PAGE_SHIFT);
	/* Check for address beyond vma (or wrapped through 0?) */
	/* 多页尾地址若越界或回绕，则以 VMA 尾端作为停止位置。 */
	if (address < vma->vm_start || address > vma->vm_end)
		address = vma->vm_end;
	return address;
}

/*
 * maybe_unlock_mmap_for_io() - 首次可重试 fault 在允许等待时 pin 文件并释放 fault/mmap 锁。
 * 已有 fpin 原样返回；NOWAIT 或不可重试时保持锁并返回 NULL，调用者最终负责 fput 返回引用。
 */
static inline struct file *maybe_unlock_mmap_for_io(struct vm_fault *vmf,
						    struct file *fpin)
{
	int flags = vmf->flags;

	if (fpin)
		return fpin;

	/*
	 * FAULT_FLAG_RETRY_NOWAIT means we don't want to wait on page locks or
	 * anything, so we only pin the file and drop the mmap_lock if only
	 * FAULT_FLAG_ALLOW_RETRY is set, while this is the first attempt.
	 */
	/* 只有首轮 ALLOW_RETRY 且非 NOWAIT 才能为阻塞 IO 释放锁，文件引用跨越解锁窗口。 */
	if (fault_flag_allow_retry_first(flags) &&
	    !(flags & FAULT_FLAG_RETRY_NOWAIT)) {
		fpin = get_file(vmf->vma->vm_file);
		release_fault_lock(vmf);
	}
	return fpin;
}

/* vma_supports_mlock() 排除 special、droppable、DAX、hugetlb 和 gate VMA。 */
static inline bool vma_supports_mlock(const struct vm_area_struct *vma)
{
	if (vma_test_any_mask(vma, VMA_SPECIAL_FLAGS))
		return false;
	if (vma_test_single_mask(vma, VMA_DROPPABLE))
		return false;
	/* DAX/hugetlb 有独立锁页机制；gate VMA 也不属于可由用户 mlock 的普通映射。 */
	if (vma_is_dax(vma) || is_vm_hugetlb_page(vma))
		return false;
	return vma != get_gate_vma(current->mm);
}

#else /* !CONFIG_MMU */
/* unmap_mapping_folio() 空桩：无 MMU 没有可解除的用户页表映射。 */
static inline void unmap_mapping_folio(struct folio *folio) { }
/* mlock_new_folio() 空桩：无 MMU 不维护 folio 的用户映射锁页状态。 */
static inline void mlock_new_folio(struct folio *folio) { }
/* need_mlock_drain() 空桩恒 false，因为没有 per-CPU mlock 批次。 */
static inline bool need_mlock_drain(int cpu) { return false; }
/* mlock_drain_local() 空桩不访问本 CPU 状态。 */
static inline void mlock_drain_local(void) { }
/* mlock_drain_remote() 空桩不向目标 CPU 发起 drain。 */
static inline void mlock_drain_remote(int cpu) { }
/* vunmap_range_noflush() 空桩：无 MMU 没有 vmalloc 页表或 TLB 状态要解除。 */
static inline void vunmap_range_noflush(unsigned long start, unsigned long end)
{
}
#endif /* !CONFIG_MMU */

/* Memory initialisation debug and verification */
/* 以下开关控制延迟 struct page 初始化以及启动期内存布局诊断。 */
#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT
DECLARE_STATIC_KEY_TRUE(deferred_pages);

/* deferred_pages_enabled() 通过静态键查询是否仍有延迟初始化页，热路径近乎零开销。 */
static inline bool deferred_pages_enabled(void)
{
	return static_branch_unlikely(&deferred_pages);
}

bool __init deferred_grow_zone(struct zone *zone, unsigned int order);
#else
/* 配置关闭时不存在延迟页，恒返回 false。 */
static inline bool deferred_pages_enabled(void)
{
	return false;
}
#endif /* CONFIG_DEFERRED_STRUCT_PAGE_INIT */

/* init_deferred_page() 为指定节点 PFN 补做启动期跳过的 struct page 初始化。 */
void init_deferred_page(unsigned long pfn, int nid);

/* mminit_level 从只报告警告到验证与详细跟踪，值越高输出越细。 */
enum mminit_level {
	MMINIT_WARNING,
	MMINIT_VERIFY,
	MMINIT_TRACE
};

#ifdef CONFIG_DEBUG_MEMORY_INIT

/* mminit_loglevel 是启动参数控制的诊断阈值；宏只输出比阈值更紧迫的级别。 */
extern int mminit_loglevel;

#define mminit_dprintk(level, prefix, fmt, arg...) \
do { \
	if (level < mminit_loglevel) { \
		if (level <= MMINIT_WARNING) \
			pr_warn("mminit::" prefix " " fmt, ##arg);	\
		else \
			printk(KERN_DEBUG "mminit::" prefix " " fmt, ##arg); \
	} \
} while (0)

/* 两项验证分别检查 pageflags 位布局与 fallback zonelist 顺序。 */
extern void mminit_verify_pageflags_layout(void);
extern void mminit_verify_zonelist(void);
#else

/* mminit_dprintk() 空桩：关闭调试时不格式化也不输出启动期诊断。 */
static inline void mminit_dprintk(enum mminit_level level,
				const char *prefix, const char *fmt, ...)
{
}

/* mminit_verify_pageflags_layout() 空桩不检查 pageflags 位域布局。 */
static inline void mminit_verify_pageflags_layout(void)
{
}

/* mminit_verify_zonelist() 空桩不遍历 fallback zonelist。 */
static inline void mminit_verify_zonelist(void)
{
}
/* 四个返回常量区分未扫描、全量扫描、部分进展和满足请求。 */
#endif /* CONFIG_DEBUG_MEMORY_INIT */

#define NODE_RECLAIM_NOSCAN	-2
#define NODE_RECLAIM_FULL	-1
#define NODE_RECLAIM_SOME	0
#define NODE_RECLAIM_SUCCESS	1

#ifdef CONFIG_NUMA
/* NUMA 构建按 mode 在本地节点回收，并用 used_node_mask 选择下一最佳节点。 */
extern int node_reclaim_mode;

extern int node_reclaim(struct pglist_data *, gfp_t, unsigned int);
extern int find_next_best_node(int node, nodemask_t *used_node_mask);
#else
#define node_reclaim_mode 0

/* node_reclaim() 空桩返回 NOSCAN，表示非 NUMA 构建未尝试任何节点回收。 */
static inline int node_reclaim(struct pglist_data *pgdat, gfp_t mask,
				unsigned int order)
{
	return NODE_RECLAIM_NOSCAN;
}
/* find_next_best_node() 空桩返回 NUMA_NO_NODE，且不修改 used_node_mask。 */
static inline int find_next_best_node(int node, nodemask_t *used_node_mask)
{
	return NUMA_NO_NODE;
}
#endif

/* node_reclaim_enabled() 在回收、写回或 unmap 任一策略位打开时为 true。 */
static inline bool node_reclaim_enabled(void)
{
	/* Is any node_reclaim_mode bit set? */
	/* 只检查有执行语义的三个位，忽略其它可能的 mode 扩展。 */
	return node_reclaim_mode & (RECLAIM_ZONE|RECLAIM_WRITE|RECLAIM_UNMAP);
}

/*
 * mm/memory-failure.c
 */
/* memory-failure 接口隔离中毒页、筛选受害映射并维护 buddy 取出状态。 */
#ifdef CONFIG_MEMORY_FAILURE
int unmap_poisoned_folio(struct folio *folio, unsigned long pfn, bool must_kill);
void shake_folio(struct folio *folio);
typedef int hwpoison_filter_func_t(struct page *p);
void hwpoison_filter_register(hwpoison_filter_func_t *filter);
void hwpoison_filter_unregister(void);

/* HWPS 魔数标识已从 buddy 临时取出的中毒页，避免重复状态转换。 */
#define MAGIC_HWPOISON	0x48575053U	/* HWPS */
void SetPageHWPoisonTakenOff(struct page *page);
void ClearPageHWPoisonTakenOff(struct page *page);
bool take_page_off_buddy(struct page *page);
bool put_page_back_buddy(struct page *page);
struct task_struct *task_early_kill(struct task_struct *tsk, int force_early);
/* KSM 共享页需按映射地址为每个受影响任务构造待 kill 项。 */
void add_to_kill_ksm(struct task_struct *tsk, const struct page *p,
		     struct vm_area_struct *vma, struct list_head *to_kill,
		     unsigned long ksm_addr);
unsigned long page_mapped_in_vma(const struct page *page,
		struct vm_area_struct *vma);

#else
/* 未启用硬件内存故障恢复时无法解除中毒 folio，稳定返回 -EBUSY。 */
static inline int unmap_poisoned_folio(struct folio *folio, unsigned long pfn, bool must_kill)
{
	return -EBUSY;
}
#endif

/* 内部 mmap 与批量回收入口可能修改当前 mm，调用者必须检查地址/errno 返回。 */
extern unsigned long  __must_check vm_mmap_pgoff(struct file *, unsigned long,
        unsigned long, unsigned long,
        unsigned long, unsigned long);

extern void set_pageblock_order(void);
unsigned long reclaim_pages(struct list_head *folio_list);
unsigned int reclaim_clean_pages_from_list(struct zone *zone,
					    struct list_head *folio_list);
/* The ALLOC_WMARK bits are used as an index to zone->watermark */
/* ALLOC_WMARK 低位直接索引 zone 水位数组；NO_WATERMARKS 表示完全绕过水位检查。 */
#define ALLOC_WMARK_MIN		WMARK_MIN
#define ALLOC_WMARK_LOW		WMARK_LOW
#define ALLOC_WMARK_HIGH	WMARK_HIGH
#define ALLOC_NO_WATERMARKS	0x04 /* don't check watermarks at all */

/* Mask to get the watermark bits */
/* 掩码只提取 MIN/LOW/HIGH 三种正常水位索引。 */
#define ALLOC_WMARK_MASK	(ALLOC_NO_WATERMARKS-1)

/*
 * Only MMU archs have async oom victim reclaim - aka oom_reaper so we
 * cannot assume a reduced access to memory reserves is sufficient for
 * !MMU
 */
/* 只有 MMU 的 oom_reaper 能异步释放受害者地址空间；无 MMU 时 OOM 权限等同无水位检查。 */
#ifdef CONFIG_MMU
#define ALLOC_OOM		0x08
#else
#define ALLOC_OOM		ALLOC_NO_WATERMARKS
#endif

/* 下列 alloc_flags 分别授予水位折扣、cpuset/CMA 访问、抗碎片与唤醒/锁策略。 */
#define ALLOC_NON_BLOCK		 0x10 /* Caller cannot block. Allow access
				       * to 25% of the min watermark or
				       * 62.5% if __GFP_HIGH is set.
				       */
#define ALLOC_MIN_RESERVE	 0x20 /* __GFP_HIGH set. Allow access to 50%
				       * of the min watermark.
				       */
#define ALLOC_CPUSET		 0x40 /* check for correct cpuset */
#define ALLOC_CMA		 0x80 /* allow allocations from CMA areas */
#ifdef CONFIG_ZONE_DMA32
/* DMA32 存在时可要求避免不同迁移类型混用 pageblock；否则该位编译为零。 */
#define ALLOC_NOFRAGMENT	0x100 /* avoid mixing pageblock types */
#else
#define ALLOC_NOFRAGMENT	  0x0
#endif
#define ALLOC_HIGHATOMIC	0x200 /* Allows access to MIGRATE_HIGHATOMIC */
#define ALLOC_TRYLOCK		0x400 /* Only use spin_trylock in allocation path */
#define ALLOC_KSWAPD		0x800 /* allow waking of kswapd, __GFP_KSWAPD_RECLAIM set */

/* Flags that allow allocations below the min watermark. */
/* 四类 reserve 权限允许分配突破 min 水位，但各自来源与额度不同。 */
#define ALLOC_RESERVES (ALLOC_NON_BLOCK|ALLOC_MIN_RESERVE|ALLOC_HIGHATOMIC|ALLOC_OOM)

enum ttu_flags;
struct tlbflush_unmap_batch;


/*
 * only for MM internal work items which do not depend on
 * any allocations or locks which might depend on allocations
 */
/* mm_percpu_wq 仅承载不分配、也不取得可能反向依赖内存分配之锁的内部 work。 */
extern struct workqueue_struct *mm_percpu_wq;

#ifdef CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH
void try_to_unmap_flush(void);
void try_to_unmap_flush_dirty(void);
void flush_tlb_batched_pending(struct mm_struct *mm);
#else
/* try_to_unmap_flush() 空桩：架构没有待提交的 batched unmap TLB。 */
static inline void try_to_unmap_flush(void)
{
}
/* try_to_unmap_flush_dirty() 空桩：无需区分脏页触发的批量刷新。 */
static inline void try_to_unmap_flush_dirty(void)
{
}
/* flush_tlb_batched_pending() 空桩不读取 mm 的批处理序列。 */
static inline void flush_tlb_batched_pending(struct mm_struct *mm)
{
}
#endif /* CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH */

/* trace 名称表把 page/VMA/GFP 位转换为稳定的人类可读字符串。 */
extern const struct trace_print_flags pageflag_names[];
extern const struct trace_print_flags vmaflag_names[];
extern const struct trace_print_flags gfpflag_names[];

/* setup_zone_pageset() 初始化指定 zone 的 per-CPU pageset 与统计阈值。 */
void setup_zone_pageset(struct zone *zone);

/* migration_target_control 汇总迁移目标节点、允许节点、分配掩码与触发原因。 */
struct migration_target_control {
	int nid;		/* preferred node id */
	/* nid 是首选目标节点，nmask 可进一步限制允许集合。 */
	nodemask_t *nmask;
	gfp_t gfp_mask;
	enum migrate_reason reason;
};

/*
 * mm/filemap.c
 */
/* splice_folio_into_pipe() 把 folio 指定文件区间作为 pipe buffers 发布，返回实际字节数。 */
size_t splice_folio_into_pipe(struct pipe_inode_info *pipe,
			      struct folio *folio, loff_t fpos, size_t size);

/*
 * mm/vmalloc.c
 */
/* vmalloc 内部接口建立/解除内核虚拟范围映射；noflush 变体由调用者负责最终 TLB flush。 */
#ifdef CONFIG_MMU
void __init vmalloc_init(void);
int __must_check vmap_pages_range_noflush(unsigned long addr, unsigned long end,
	pgprot_t prot, struct page **pages, unsigned int page_shift, gfp_t gfp_mask);
unsigned int get_vm_area_page_order(struct vm_struct *vm);
#else
/* 无 MMU 无需 vmalloc 初始化，也不能建立任意 pages 的虚拟连续映射。 */
static inline void vmalloc_init(void)
{
}

/* 无 MMU 无法实现虚拟连续 pages remap，以 -EINVAL 明确拒绝而非假成功。 */
static inline
int __must_check vmap_pages_range_noflush(unsigned long addr, unsigned long end,
	pgprot_t prot, struct page **pages, unsigned int page_shift, gfp_t gfp_mask)
{
	return -EINVAL;
}
#endif

/* clear_vm_uninitialized_flag() 在 vmalloc 映射内容就绪后清除禁止暴露的初始化标志。 */
void clear_vm_uninitialized_flag(struct vm_struct *vm);

int __must_check __vmap_pages_range_noflush(unsigned long addr,
			       unsigned long end, pgprot_t prot,
			       struct page **pages, unsigned int page_shift);

void vunmap_range_noflush(unsigned long start, unsigned long end);

/* 双下划线入口跳过部分公共包装检查，仍由调用者负责 TLB flush 时序。 */
void __vunmap_range_noflush(unsigned long start, unsigned long end);

/* vma_is_single_threaded_private() 要求非共享且 mm 仅有一个 users 引用。 */
static inline bool vma_is_single_threaded_private(struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_SHARED)
		return false;

	return atomic_read(&vma->vm_mm->mm_users) == 1;
}

#ifdef CONFIG_NUMA_BALANCING
bool folio_can_map_prot_numa(struct folio *folio, struct vm_area_struct *vma,
		bool is_private_single_threaded);

#else
/* 未启用 NUMA balancing 时没有 PROT_NUMA 映射资格，恒返回 false。 */
static inline bool folio_can_map_prot_numa(struct folio *folio,
		struct vm_area_struct *vma, bool is_private_single_threaded)
{
	return false;
}
#endif

/* NUMA fault 检查输出迁移 flags/last_cpupid，并决定 folio 是否应迁往 fault 节点。 */
int numa_migrate_check(struct folio *folio, struct vm_fault *vmf,
		      unsigned long addr, int *flags, bool writable,
		      int *last_cpupid);

/* ZONE_DEVICE folio 释放与 coherent 迁移接口协调设备页生命周期。 */
void free_zone_device_folio(struct folio *folio);
int migrate_device_coherent_folio(struct folio *folio);

/* __get_vm_area_node() 在指定内核虚拟区间/节点预留 vm_struct，失败返回 NULL。 */
struct vm_struct *__get_vm_area_node(unsigned long size,
				     unsigned long align, unsigned long shift,
				     unsigned long vm_flags, unsigned long start,
				     unsigned long end, int node, gfp_t gfp_mask,
				     const void *caller);

/*
 * mm/gup.c
 */
/* try_grab_folio() 按 GUP/PIN flags 获取 refs 份引用或 pin，失败返回负 errno。 */
int __must_check try_grab_folio(struct folio *folio, int refs,
				unsigned int flags);

/*
 * mm/huge_memory.c
 */
/* touch_pud/pmd 在 fault 语义下更新 huge entry 的访问状态，write 决定是否请求写脏。 */
void touch_pud(struct vm_area_struct *vma, unsigned long addr,
	       pud_t *pud, bool write);
bool touch_pmd(struct vm_area_struct *vma, unsigned long addr,
	       pmd_t *pmd, bool write);

/*
 * Parses a string with mem suffixes into its order. Useful to parse kernel
 * parameters.
 */
/* get_order_from_str() 解析带内存后缀的 2 次幂大小，并验证 order 位属于 valid_orders。 */
static inline int get_order_from_str(const char *size_str,
				     unsigned long valid_orders)
{
	unsigned long size;
	char *endptr;
	int order;

	size = memparse(size_str, &endptr);

	/* 只有 2 次幂字节数才有唯一 order；解析尾缀合法性由参数调用场景约束。 */
	if (!is_power_of_2(size))
		return -EINVAL;
	order = get_order(size);
	if (BIT(order) & ~valid_orders)
		return -EINVAL;

	/* valid_orders 以 BIT(order) 表示白名单，越界值统一返回 -EINVAL。 */
	return order;
}

enum {
	/* mark page accessed */
	/* GUP 成功时把页标记为已访问。 */
	FOLL_TOUCH = 1 << 16,
	/* a retry, previous pass started an IO */
	/* 当前调用是已发起 IO 后的重试轮次。 */
	FOLL_TRIED = 1 << 17,
	/* we are working on non-current tsk/mm */
	/* 操作对象不是 current 的 task/mm。 */
	FOLL_REMOTE = 1 << 18,
	/* pages must be released via unpin_user_page */
	/* 获取的是 DMA pin，释放必须使用 unpin_user_page 系列。 */
	FOLL_PIN = 1 << 19,
	/* gup_fast: prevent fall-back to slow gup */
	/* fast GUP 失败时禁止回退到可能加锁/fault 的慢路。 */
	FOLL_FAST_ONLY = 1 << 20,
	/* allow unlocking the mmap lock */
	/* 允许 fault/retry 流程临时释放 mmap 锁。 */
	FOLL_UNLOCKABLE = 1 << 21,
	/* VMA lookup+checks compatible with MADV_POPULATE_(READ|WRITE) */
	/* 采用 MADV_POPULATE 所需的 VMA 查找与权限检查语义。 */
	FOLL_MADV_POPULATE = 1 << 22,
};

#define INTERNAL_GUP_FLAGS (FOLL_TOUCH | FOLL_TRIED | FOLL_REMOTE | FOLL_PIN | \
			    FOLL_FAST_ONLY | FOLL_UNLOCKABLE | \
			    FOLL_MADV_POPULATE)

/*
 * Indicates for which pages that are write-protected in the page table,
 * whether GUP has to trigger unsharing via FAULT_FLAG_UNSHARE such that the
 * GUP pin will remain consistent with the pages mapped into the page tables
 * of the MM.
 *
 * Temporary unmapping of PageAnonExclusive() pages or clearing of
 * PageAnonExclusive() has to protect against concurrent GUP:
 * * Ordinary GUP: Using the PT lock
 * * GUP-fast and fork(): mm->write_protect_seq
 * * GUP-fast and KSM or temporary unmapping (swap, migration): see
 *    folio_try_share_anon_rmap_*()
 *
 * Must be called with the (sub)page that's actually referenced via the
 * page table entry, which might not necessarily be the head page for a
 * PTE-mapped THP.
 *
 * If the vma is NULL, we're coming from the GUP-fast path and might have
 * to fallback to the slow path just to lookup the vma.
 */
/*
 * gup_must_unshare() - 判断只读 FOLL_PIN 是否必须先触发 COW/unshare 才能保持长期 pin 一致性。
 * 匿名页要求 PageAnonExclusive；文件页仅长期 pin 的私有可写映射需要拆 COW。fast 路缺 VMA 时保守回退。
 * 与 anon rmap 共享路径的屏障配对，调用者传实际 PTE 引用的子页而非任意 THP head。
 */
static inline bool gup_must_unshare(struct vm_area_struct *vma,
				    unsigned int flags, struct page *page)
{
	/*
	 * FOLL_WRITE is implicitly handled correctly as the page table entry
	 * has to be writable -- and if it references (part of) an anonymous
	 * folio, that part is required to be marked exclusive.
	 */
	/* FOLL_WRITE 已由可写 PTE/匿名 exclusive 不变量覆盖；这里只处理只读 PIN。 */
	if ((flags & (FOLL_WRITE | FOLL_PIN)) != FOLL_PIN)
		return false;
	/*
	 * Note: PageAnon(page) is stable until the page is actually getting
	 * freed.
	 */
	/* page 真正释放前 PageAnon 属性稳定，可在这里无额外锁分流。 */
	if (!PageAnon(page)) {
		/*
		 * We only care about R/O long-term pining: R/O short-term
		 * pinning does not have the semantics to observe successive
		 * changes through the process page tables.
		 */
		/* 短期只读 pin 不承诺观察后续 COW 变化，只有 LONGTERM 需要提前拆分。 */
		if (!(flags & FOLL_LONGTERM))
			return false;

		/* We really need the vma ... */
		/* fast 路没有 VMA 时无法判定文件映射是否私有可写，返回 true 迫使慢路查询。 */
		if (!vma)
			return true;

		/*
		 * ... because we only care about writable private ("COW")
		 * mappings where we have to break COW early.
		 */
		/* 仅 COW 映射会让页表后续替换被 pin 的只读文件页。 */
		return is_cow_mapping(vma->vm_flags);
	}

	/* Paired with a memory barrier in folio_try_share_anon_rmap_*(). */
	/* 读屏障与清除匿名 exclusive 的发布屏障配对，避免 pin 穿过共享转换。 */
	if (IS_ENABLED(CONFIG_HAVE_GUP_FAST))
		smp_rmb();

	/*
	 * Note that KSM pages cannot be exclusive, and consequently,
	 * cannot get pinned.
	 */
	/* KSM 页天然共享且不具 exclusive 位，因此该判定也会拒绝其 PIN。 */
	return !PageAnonExclusive(page);
}

/* memblock 入口查询镜像内存并在启动末期把剩余内存释放给 buddy。 */
extern bool mirrored_kernelcore;
bool memblock_has_mirror(void);
void memblock_free_all(void);

/* vma_set_range() 原子语义地成组写入 VMA 虚拟范围与对象页偏移；调用者持 VMA 写锁。 */
static __always_inline void vma_set_range(struct vm_area_struct *vma,
					  unsigned long start, unsigned long end,
					  pgoff_t pgoff)
{
	vma->vm_start = start;
	vma->vm_end = end;
	vma->vm_pgoff = pgoff;
}

/* vma_soft_dirty_enabled() 先检查页表能力，再按反向 VM_SOFTDIRTY 标志解释追踪是否开启。 */
static inline bool vma_soft_dirty_enabled(struct vm_area_struct *vma)
{
	/*
	 * NOTE: we must check this before VM_SOFTDIRTY on soft-dirty
	 * enablements, because when without soft-dirty being compiled in,
	 * VM_SOFTDIRTY is defined as 0x0, then !(vm_flags & VM_SOFTDIRTY)
	 * will be constantly true.
	 */
	/* 配置关闭时 VM_SOFTDIRTY 为零，必须先以架构能力短路，避免把所有 VMA 误判为启用。 */
	if (!pgtable_supports_soft_dirty())
		return false;

	/*
	 * Soft-dirty is kind of special: its tracking is enabled when the
	 * vma flags not set.
	 */
	/* 清 VM_SOFTDIRTY 表示下一轮写入需要以只读 fault 记录新的 soft-dirty。 */
	return !(vma->vm_flags & VM_SOFTDIRTY);
}

/* pmd_needs_soft_dirty_wp() 在追踪开启且 entry 尚未 dirty 时要求写保护。 */
static inline bool pmd_needs_soft_dirty_wp(struct vm_area_struct *vma, pmd_t pmd)
{
	return vma_soft_dirty_enabled(vma) && !pmd_soft_dirty(pmd);
}

/* pte_needs_soft_dirty_wp() 是 base-page PTE 对应判定，不修改 entry。 */
static inline bool pte_needs_soft_dirty_wp(struct vm_area_struct *vma, pte_t pte)
{
	return vma_soft_dirty_enabled(vma) && !pte_soft_dirty(pte);
}

/* 两个 meminit helper 建立单个 struct page 的 zone/node/PFN 基础元数据。 */
void __meminit __init_single_page(struct page *page, unsigned long pfn,
				unsigned long zone, int nid);
void __meminit __init_page_from_nid(unsigned long pfn, int nid);

/* shrinker related functions */
/* shrink_slab() 按节点、memcg 和回收优先级调用已注册 shrinker，返回扫描进展。 */
unsigned long shrink_slab(gfp_t gfp_mask, int nid, struct mem_cgroup *memcg,
			  int priority);

/* shmem 内部接口负责 page-cache 插入与 inode 已分配/swap 块计账。 */
int shmem_add_to_page_cache(struct folio *folio,
			    struct address_space *mapping,
			    pgoff_t index, void *expected, gfp_t gfp);
int shmem_inode_acct_blocks(struct inode *inode, long pages);
bool shmem_recalc_inode(struct inode *inode, long alloced, long swapped);

#ifdef CONFIG_SHRINKER_DEBUG
/* 调试开启时为 shrinker 分配稳定名称；失败返回 -ENOMEM，free 后清空 owner 字段。 */
static inline __printf(2, 0) int shrinker_debugfs_name_alloc(
			struct shrinker *shrinker, const char *fmt, va_list ap)
{
	shrinker->name = kvasprintf_const(GFP_KERNEL, fmt, ap);

	return shrinker->name ? 0 : -ENOMEM;
}

/* shrinker_debugfs_name_free() 释放 const/动态统一名称并将指针置 NULL。 */
static inline void shrinker_debugfs_name_free(struct shrinker *shrinker)
{
	kfree_const(shrinker->name);
	shrinker->name = NULL;
}

/* 实体 debugfs 注册接口拥有目录项与 id，detach 后由 remove 完成销毁。 */
extern int shrinker_debugfs_add(struct shrinker *shrinker);
extern struct dentry *shrinker_debugfs_detach(struct shrinker *shrinker,
					      int *debugfs_id);
extern void shrinker_debugfs_remove(struct dentry *debugfs_entry,
				    int debugfs_id);
#else /* CONFIG_SHRINKER_DEBUG */
/* shrinker_debugfs_add() 空桩返回成功，不创建目录项也不取得 owner。 */
static inline int shrinker_debugfs_add(struct shrinker *shrinker)
{
	return 0;
}
/* shrinker_debugfs_name_alloc() 空桩不写 name，返回 0 让公共注册流程继续。 */
static inline int shrinker_debugfs_name_alloc(struct shrinker *shrinker,
					      const char *fmt, va_list ap)
{
	return 0;
}
/* shrinker_debugfs_name_free() 空桩不读取或释放未分配的 name。 */
static inline void shrinker_debugfs_name_free(struct shrinker *shrinker)
{
}
/* shrinker_debugfs_detach() 写回无效 id 并返回 NULL，明确不存在目录项 owner。 */
static inline struct dentry *shrinker_debugfs_detach(struct shrinker *shrinker,
						     int *debugfs_id)
{
	*debugfs_id = -1;
	return NULL;
}
/* shrinker_debugfs_remove() 空桩不消费不存在的 dentry 或 id。 */
static inline void shrinker_debugfs_remove(struct dentry *debugfs_entry,
					   int debugfs_id)
{
}
#endif /* CONFIG_SHRINKER_DEBUG */

/* Only track the nodes of mappings with shadow entries */
/* workingset 只把含 shadow entry 的非 DAX、非 shmem xarray 节点挂入全局 shadow LRU。 */
void workingset_update_node(struct xa_node *node);
extern struct list_lru shadow_nodes;
#define mapping_set_update(xas, mapping) do {			\
	if (!dax_mapping(mapping) && !shmem_mapping(mapping)) {	\
		xas_set_update(xas, workingset_update_node);	\
		xas_set_lru(xas, &shadow_nodes);		\
	}							\
} while (0)

/* mremap.c */
/* move_page_tables() 消费并推进页表搬迁控制块，返回用户原始范围内实际完成字节数。 */
unsigned long move_page_tables(struct pagetable_move_control *pmc);

#ifdef CONFIG_UNACCEPTED_MEMORY
void accept_page(struct page *page);
#else /* CONFIG_UNACCEPTED_MEMORY */
/* 平台没有 unaccepted memory 时所有页天然可用，accept_page() 为空操作。 */
static inline void accept_page(struct page *page)
{
}
#endif /* CONFIG_UNACCEPTED_MEMORY */

/* pagewalk.c */
/* unsafe pagewalk 入口要求调用者自行稳定 mm/VMA/页表；debug 入口可从指定 PGD 开始。 */
int walk_page_range_mm_unsafe(struct mm_struct *mm, unsigned long start,
		unsigned long end, const struct mm_walk_ops *ops,
		void *private);
int walk_page_range_vma_unsafe(struct vm_area_struct *vma, unsigned long start,
		unsigned long end, const struct mm_walk_ops *ops,
		void *private);
int walk_page_range_debug(struct mm_struct *mm, unsigned long start,
			  unsigned long end, const struct mm_walk_ops *ops,
			  pgd_t *pgd, void *private);

/* fork/exec 复制接口转移 exe_file 引用并复制旧 mm 的 VMA/页表布局。 */
void dup_mm_exe_file(struct mm_struct *mm, struct mm_struct *oldmm);
int dup_mmap(struct mm_struct *mm, struct mm_struct *oldmm);

/* mmap action prepare/complete 把 PFN remap 或简单 IO remap 纳入统一 VMA 提交流程。 */
int remap_pfn_range_prepare(struct vm_area_desc *desc);
int remap_pfn_range_complete(struct vm_area_struct *vma,
			     struct mmap_action *action);
int simple_ioremap_prepare(struct vm_area_desc *desc);

/*
 * io_remap_pfn_range_prepare() - 架构变换 IO PFN、使用解密 pgprot，再准备普通 PFN remap。
 * 成功把 action type 设为 MMAP_REMAP_PFN；失败保留 helper 返回 errno，不执行实际 remap。
 */
static inline int io_remap_pfn_range_prepare(struct vm_area_desc *desc)
{
	struct mmap_action *action = &desc->action;
	const unsigned long orig_pfn = action->remap.start_pfn;
	const pgprot_t orig_pgprot = action->remap.pgprot;
	const unsigned long size = action->remap.size;
	const unsigned long pfn = io_remap_pfn_range_pfn(orig_pfn, size);
	int err;

	/* 在调用通用 prepare 前暂存架构规范化 PFN 与解密后的页保护属性。 */
	action->remap.start_pfn = pfn;
	action->remap.pgprot = pgprot_decrypted(orig_pgprot);
	err = remap_pfn_range_prepare(desc);
	if (err)
		return err;

	/* Remap does the actual work. */
	/* prepare 只验证并记录动作，真正页表建立由后续 mmap action 提交阶段完成。 */
	action->type = MMAP_REMAP_PFN;
	return 0;
}

/*
 * When we succeed an mmap action or just before we unmap a VMA on error, we
 * need to ensure any rmap lock held is released. On unmap it's required to
 * avoid a deadlock.
 */
/* maybe_rmap_unlock_action() 在 action 成功或错误 unmap 前释放暂持的 i_mmap 写锁并清 owner 标志。 */
static inline void maybe_rmap_unlock_action(struct vm_area_struct *vma,
		struct mmap_action *action)
{
	struct file *file;

	if (!action->hide_from_rmap_until_complete)
		return;

	/* 该锁只可能属于文件映射；释放后清标志把 ownership 转回普通 action 状态。 */
	VM_WARN_ON_ONCE(vma_is_anonymous(vma));
	file = vma->vm_file;
	i_mmap_unlock_write(file->f_mapping);
	action->hide_from_rmap_until_complete = false;
}

#ifdef CONFIG_MMU_NOTIFIER
/* clear_flush_young_ptes_notify() 同时清 CPU PTE accessed 位并通知二级 MMU，返回任一路径是否年轻。 */
static inline bool clear_flush_young_ptes_notify(struct vm_area_struct *vma,
		unsigned long addr, pte_t *ptep, unsigned int nr)
{
	bool young;

	young = clear_flush_young_ptes(vma, addr, ptep, nr);
	young |= mmu_notifier_clear_flush_young(vma->vm_mm, addr,
						addr + nr * PAGE_SIZE);
	return young;
}

/* pmdp_clear_flush_young_notify() 是 PMD 粒度且含 TLB flush 的同类组合操作。 */
static inline bool pmdp_clear_flush_young_notify(struct vm_area_struct *vma,
		unsigned long addr, pmd_t *pmdp)
{
	bool young;

	young = pmdp_clear_flush_young(vma, addr, pmdp);
	young |= mmu_notifier_clear_flush_young(vma->vm_mm, addr, addr + PMD_SIZE);
	return young;
}

/* test_and_clear_young_ptes_notify() 清 PTE young 而不要求本地 flush，并合并 notifier 结果。 */
static inline bool test_and_clear_young_ptes_notify(struct vm_area_struct *vma,
		unsigned long addr, pte_t *ptep, unsigned int nr)
{
	bool young;

	young = test_and_clear_young_ptes(vma, addr, ptep, nr);
	young |= mmu_notifier_clear_young(vma->vm_mm, addr, addr + nr * PAGE_SIZE);
	return young;
}

/* pmdp_test_and_clear_young_notify() 对单个 PMD 执行 test-clear 并覆盖完整 PMD 通知范围。 */
static inline bool pmdp_test_and_clear_young_notify(struct vm_area_struct *vma,
		unsigned long addr, pmd_t *pmdp)
{
	bool young;

	young = pmdp_test_and_clear_young(vma, addr, pmdp);
	young |= mmu_notifier_clear_young(vma->vm_mm, addr, addr + PMD_SIZE);
	return young;
}

#else /* CONFIG_MMU_NOTIFIER */

/* 无 MMU notifier 时直接别名到底层页表 helper，不增加额外分支或通知。 */
#define clear_flush_young_ptes_notify	clear_flush_young_ptes
#define pmdp_clear_flush_young_notify	pmdp_clear_flush_young
#define test_and_clear_young_ptes_notify	test_and_clear_young_ptes
#define pmdp_test_and_clear_young_notify	pmdp_test_and_clear_young

#endif /* CONFIG_MMU_NOTIFIER */

extern int sysctl_max_map_count;
/* get_sysctl_max_map_count() 用 READ_ONCE 获取并发 sysctl 更新下的一致标量快照。 */
static inline int get_sysctl_max_map_count(void)
{
	return READ_ONCE(sysctl_max_map_count);
}

/* may_expand_vm() 按 VMA 类型和 mm 虚拟页统计判断增加 npages 是否越过资源上限。 */
bool may_expand_vm(struct mm_struct *mm, const vma_flags_t *vma_flags,
		   unsigned long npages);

#endif	/* __MM_INTERNAL_H */
