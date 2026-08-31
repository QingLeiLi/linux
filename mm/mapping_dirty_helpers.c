// SPDX-License-Identifier: GPL-2.0
#include <linux/pagewalk.h>
#include <linux/hugetlb.h>
#include <linux/bitops.h>
#include <linux/mmu_notifier.h>
#include <linux/mm_inline.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

/*
 * 这里为共享 file mapping 提供两种 PTE 级 dirty tracking 原语：先撤销可写位，
 * 让后续 CPU 写访问重新经过 page_mkwrite()/pfn_mkwrite()；或清除硬件 dirty 位
 * 并把已观察到的文件页偏移 OR 入调用者 bitmap。当前唯一用户 vmwgfx 以此在
 * 页表扫描与 fault 跟踪模式间切换。
 */

/**
 * struct wp_walk - Private struct for pagetable walk callbacks
 * @range: Range for mmu notifiers
 * @tlbflush_start: Address of first modified pte
 * @tlbflush_end: Address of last modified pte + 1
 * @total: Total number of modified ptes
 */
/*
 * wp_walk 是一次 walk 的共享账本。range 保存当前 VMA 上报给次级 MMU 的失效
 * 区间；tlbflush_start/end 是实际改写过的最小半开虚拟地址范围，初值反置为
 * [end,start) 表示尚无修改；total 统计真正从 writable/dirty 转换的 PTE 数。
 * 对象位于顶层调用栈，所有回调同步借用，不产生引用或跨 walk 保存。
 */
struct wp_walk {
	struct mmu_notifier_range range;
	unsigned long tlbflush_start;
	unsigned long tlbflush_end;
	unsigned long total;
};

/**
 * wp_pte - Write-protect a pte
 * @pte: Pointer to the pte
 * @addr: The start of protecting virtual address
 * @end: The end of protecting virtual address
 * @walk: pagetable walk callback argument
 *
 * The function write-protects a pte and records the range in
 * virtual address space of touched ptes for efficient range TLB flushes.
 */
/*
 * 原文所说的 write-protect 只撤销单个 PTE 的写权限；记录首末改写地址是为了
 * post_vma 只刷新真正发生变化的 TLB 子区间，而非整个映射。
 *
 * 业务背景：wp_shared_mapping_range() 的 PTE 回调，把共享可写映射转为只读，
 * 使下一次写入 fault 并进入文件/驱动 dirty 回调。
 * 入参：pte 是 walker 在 PTL 下借出的当前项；addr/end 是 PTE 区间，end 本地
 * 不使用；walk->vma/mm/private 在同步回调期间有效。
 * 出参/返回：已可写时原子式提交只读 PTE、累计 total/TLB 范围；否则不变；
 * 始终返回 0，不取得 page/VMA 引用。
 * 注意事项：ptep_modify_prot_start/commit 必须配对并保留非权限位；这里只改页表，
 * cache、TLB 与 MMU notifier 的开始/结束由 pre/post 回调成对处理。
 */
static int wp_pte(pte_t *pte, unsigned long addr, unsigned long end,
		  struct mm_walk *walk)
{
	struct wp_walk *wpwalk = walk->private;
	pte_t ptent = ptep_get(pte);

	/* 已经只读的 PTE 不算本轮修改，也无需扩大 TLB flush 区间。 */
	if (pte_write(ptent)) {
		pte_t old_pte = ptep_modify_prot_start(walk->vma, addr, pte);

		ptent = pte_wrprotect(old_pte);
		/* commit 是 PTE 对其他 CPU/硬件可见的状态转换点。 */
		ptep_modify_prot_commit(walk->vma, addr, pte, old_pte, ptent);
		wpwalk->total++;
		wpwalk->tlbflush_start = min(wpwalk->tlbflush_start, addr);
		wpwalk->tlbflush_end = max(wpwalk->tlbflush_end,
					   addr + PAGE_SIZE);
	}

	return 0;
}

/**
 * struct clean_walk - Private struct for the clean_record_pte function.
 * @base: struct wp_walk we derive from
 * @bitmap_pgoff: Address_space Page offset of the first bit in @bitmap
 * @bitmap: Bitmap with one bit for each page offset in the address_space range
 * covered.
 * @start: Address_space page offset of first modified pte relative
 * to @bitmap_pgoff
 * @end: Address_space page offset of last modified pte relative
 * to @bitmap_pgoff
 */
/*
 * clean_walk 把 wp_walk 嵌为首个成员，因此共享 pre/post 回调可只认识 base，
 * PTE 回调再用 container_of 恢复完整对象。bitmap_pgoff 是 bit 0 对应的文件
 * page offset；start/end 是相对 bitmap 的已置位最小半开范围。bitmap 由调用者
 * 持有，函数只置位不清零，可把多次扫描结果累积起来。
 */
struct clean_walk {
	struct wp_walk base;
	pgoff_t bitmap_pgoff;
	unsigned long *bitmap;
	pgoff_t start;
	pgoff_t end;
};

/* 从嵌入的 base 地址恢复 clean_walk；仅对确实属于该结构的指针有效。 */
#define to_clean_walk(_wpwalk) container_of(_wpwalk, struct clean_walk, base)

/**
 * clean_record_pte - Clean a pte and record its address space offset in a
 * bitmap
 * @pte: Pointer to the pte
 * @addr: The start of virtual address to be clean
 * @end: The end of virtual address to be clean
 * @walk: pagetable walk callback argument
 *
 * The function cleans a pte and records the range in
 * virtual address space of touched ptes for efficient TLB flushes.
 * It also records dirty ptes in a bitmap representing page offsets
 * in the address_space, as well as the first and last of the bits
 * touched.
 */
/*
 * 原文的 clean 是清 PTE dirty 位而非清理页面内容；每个被清的 dirty PTE 同时
 * 映射为 address_space 页偏移并记入 bitmap，从而不会悄悄丢掉已观察到的脏页。
 *
 * 业务背景：clean_record_shared_mapping_range() 用本回调收割硬件 dirty 位，
 * 支持 vmwgfx 选择性上传已被 CPU 写过的 buffer pages。
 * 入参：pte/addr/end/walk 由 walker 借用；bitmap 必须覆盖计算出的 pgoff。
 * 出参/返回：dirty 时提交 clean PTE、置 bitmap 位、扩大虚拟 TLB 范围和位图
 * start/end、递增 total；非 dirty 不变；始终返回 0。
 * 注意事项：只清 dirty、不撤销 write，因此 CPU 可立即再次置脏；PTL 串行化
 * 同一 PTE 的硬件状态转换，pre/post 回调负责 cache/TLB/notifier 协议。
 */
static int clean_record_pte(pte_t *pte, unsigned long addr,
			    unsigned long end, struct mm_walk *walk)
{
	struct wp_walk *wpwalk = walk->private;
	struct clean_walk *cwalk = to_clean_walk(wpwalk);
	pte_t ptent = ptep_get(pte);

	if (pte_dirty(ptent)) {
		/* 把 VMA 虚址换成文件页偏移，再减 bitmap 基准得到无符号 bit 索引。 */
		pgoff_t pgoff = ((addr - walk->vma->vm_start) >> PAGE_SHIFT) +
			walk->vma->vm_pgoff - cwalk->bitmap_pgoff;
		pte_t old_pte = ptep_modify_prot_start(walk->vma, addr, pte);

		ptent = pte_mkclean(old_pte);
		/* 先发布 clean PTE，再在当前同步 walk 内记录它原先为 dirty 的事实。 */
		ptep_modify_prot_commit(walk->vma, addr, pte, old_pte, ptent);

		wpwalk->total++;
		wpwalk->tlbflush_start = min(wpwalk->tlbflush_start, addr);
		wpwalk->tlbflush_end = max(wpwalk->tlbflush_end,
					   addr + PAGE_SIZE);

		__set_bit(pgoff, cwalk->bitmap);
		/* start/end 保持覆盖所有已置位结果的最小半开范围，便于调用者限界扫描。 */
		cwalk->start = min(cwalk->start, pgoff);
		cwalk->end = max(cwalk->end, pgoff + 1);
	}

	return 0;
}

/*
 * wp_clean_pmd_entry - The pagewalk pmd callback.
 *
 * Dirty-tracking should take place on the PTE level, so
 * WARN() if encountering a dirty huge pmd.
 * Furthermore, never split huge pmds, since that currently
 * causes dirty info loss. The pagefault handler should do
 * that if needed.
 */
/*
 * 原文要求 dirty tracking 只在 PTE 层完成：这里若见到 writable/dirty 的 THP
 * PMD 会 WARN，因为本实现无法可靠收割其 dirty；同时绝不主动 split，避免拆分
 * 过程丢失 dirty 信息，必须等待 page fault 路径在需要时处理。
 *
 * 业务背景：为两种 walk 拦截 PMD 级透明大页，防止 walker 下钻 leaf。
 * 入参：pmd 为 lockless 读取的借用项；addr/end 未使用；walk 提供 action。
 * 出参/返回：THP（present 或 migration 形态）时可能 WARN 并设 CONTINUE，始终 0。
 * 注意事项：不修改/拆分 PMD，也不计入 total/bitmap；因此调用者得到的是 PTE
 * 级结果，不包含被跳过的 THP 映射。
 */
static int wp_clean_pmd_entry(pmd_t *pmd, unsigned long addr, unsigned long end,
			      struct mm_walk *walk)
{
	pmd_t pmdval = pmdp_get_lockless(pmd);

	/* Do not split a huge pmd, present or migrated */
	/* 无锁快照命中 THP 后只告警并跳过；页表 walker 自身负责安全的层级访问。 */
	if (pmd_trans_huge(pmdval)) {
		WARN_ON(pmd_write(pmdval) || pmd_dirty(pmdval));
		walk->action = ACTION_CONTINUE;
	}
	return 0;
}

/*
 * wp_clean_pud_entry - The pagewalk pud callback.
 *
 * Dirty-tracking should take place on the PTE level, so
 * WARN() if encountering a dirty huge puds.
 * Furthermore, never split huge puds, since that currently
 * causes dirty info loss. The pagefault handler should do
 * that if needed.
 */
/*
 * 原文与 PMD 路径相同：dirty tracking 不在透明大页 PUD 上执行，遇到 writable
 * 或 dirty leaf 只 WARN，不能为扫描而 split 并冒险丢 dirty 信息。
 *
 * 业务背景：在支持 PUD THP 的架构上阻止 walker 把 huge PUD 当成下级目录。
 * 入参：pud 为借用项；addr/end 未使用；walk 的 action 可写。
 * 出参/返回：huge PUD 时设置 CONTINUE，始终返回 0；关闭架构能力时为空操作。
 * 注意事项：不计数、不记录 bitmap；pudp_get() 仅在相应 CONFIG 分支编译。
 */
static int wp_clean_pud_entry(pud_t *pud, unsigned long addr, unsigned long end,
			      struct mm_walk *walk)
{
#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
	pud_t pudval = pudp_get(pud);

	/* Do not split a huge pud */
	/* leaf 只告警并跳过，下层 PTE dirty 状态不存在，不能伪造扫描结果。 */
	if (pud_trans_huge(pudval)) {
		WARN_ON(pud_write(pudval) || pud_dirty(pudval));
		walk->action = ACTION_CONTINUE;
	}
#endif
	return 0;
}

/*
 * wp_clean_pre_vma - The pagewalk pre_vma callback.
 *
 * The pre_vma callback performs the cache flush, stages the tlb flush
 * and calls the necessary mmu notifiers.
 */
/*
 * 业务背景：每进入一个适用 VMA 的实际 walk 前，建立“权限/dirty 位将变化”的
 * cache、次级 MMU 与 TLB 协议边界。
 * 入参：start/end 是该 VMA 中被裁剪的半开虚拟区间；walk 借用 mm/vma/wpwalk。
 * 出参/返回：初始化本 VMA 的 flush 哨兵与 notifier range，调用 invalidate_start、
 * 刷新 cache、递增 tlb_flush_pending，返回 0；不转移引用。
 * 注意事项：MMU_NOTIFY_PROTECTION_PAGE 要求 notifier 在 end 回调检查 CPU 页表；
 * start/end 必须与 post_vma 严格配对。函数可进入 notifier，调用时持 i_mmap 读锁。
 */
static int wp_clean_pre_vma(unsigned long start, unsigned long end,
			    struct mm_walk *walk)
{
	struct wp_walk *wpwalk = walk->private;

	wpwalk->tlbflush_start = end;
	wpwalk->tlbflush_end = start;

	/* 先阻止/失效设备侧二级映射，再改 CPU PTE，避免设备继续使用旧权限。 */
	mmu_notifier_range_init(&wpwalk->range, MMU_NOTIFY_PROTECTION_PAGE, 0,
				walk->mm, start, end);
	mmu_notifier_invalidate_range_start(&wpwalk->range);
	flush_cache_range(walk->vma, start, end);

	/*
	 * We're not using tlb_gather_mmu() since typically
	 * only a small subrange of PTEs are affected, whereas
	 * tlb_gather_mmu() records the full range.
	 */
	/*
	 * 原文说明不使用 tlb_gather_mmu()：典型只改少量 PTE，完整 VMA 范围的 gather
	 * 会造成过度刷新；本对象已精确累计真正修改的首末地址。
	 */
	inc_tlb_flush_pending(walk->mm);

	return 0;
}

/*
 * wp_clean_post_vma - The pagewalk post_vma callback.
 *
 * The post_vma callback performs the tlb flush and calls necessary mmu
 * notifiers.
 */
/*
 * 业务背景：一个 VMA 的 PTE walk 完成后，先使旧 TLB 权限/dirty 状态失效，
 * 再结束次级 MMU invalidate 区间并撤销 pending 记账。
 * 入参：walk 借用当前 mm/vma 和由 pre_vma 初始化的 wpwalk。
 * 出参/返回：void；按嵌套状态刷新全 notifier range 或仅实际修改子区间，然后
 * 调 invalidate_end 并递减 tlb_flush_pending。
 * 注意事项：即使没有 PTE 被修改也必须调用 notifier end 和 dec；嵌套更新时
 * 扩大刷新范围，避免另一个 pending 修改未落入本地精确区间而残留旧 TLB。
 */
static void wp_clean_post_vma(struct mm_walk *walk)
{
	struct wp_walk *wpwalk = walk->private;

	/* nested>1 时保守刷完整区间；否则只刷本 walk 实际修改的非空半开子区间。 */
	if (mm_tlb_flush_nested(walk->mm))
		flush_tlb_range(walk->vma, wpwalk->range.start,
				wpwalk->range.end);
	else if (wpwalk->tlbflush_end > wpwalk->tlbflush_start)
		flush_tlb_range(walk->vma, wpwalk->tlbflush_start,
				wpwalk->tlbflush_end);

	mmu_notifier_invalidate_range_end(&wpwalk->range);
	dec_tlb_flush_pending(walk->mm);
}

/*
 * wp_clean_test_walk - The pagewalk test_walk callback.
 *
 * Won't perform dirty-tracking on COW, read-only or HUGETLB vmas.
 */
/*
 * 原文排除 COW、只读和 HugeTLB VMA：只有 VM_SHARED|VM_MAYWRITE 且非 HUGETLB
 * 的映射才属于共享可写 PTE dirty tracking，当前 vm_flags 用 READ_ONCE 取快照。
 *
 * 业务背景：pagewalk 在进入 VMA 前用本回调筛选可安全改写的共享文件映射。
 * 入参：start/end 是候选 VMA 范围但本地不使用；walk->vma 为借用对象。
 * 出参/返回：适用返回 0 继续，其他返回 1；walk_page_mapping() 的当前实现把
 * 正值作为非错误的提前终止，因此会结束本次 mapping walk，而非继续后续 VMA。
 * 本回调自身无状态或 ownership 修改。
 * 注意事项：外层只持 mapping->i_mmap_rwsem，不能假设可变 vm_flags 全程冻结；
 * READ_ONCE 防止撕裂/重复读取，但它不是 mmap_lock，也不提供更强一致性。
 */
static int wp_clean_test_walk(unsigned long start, unsigned long end,
			      struct mm_walk *walk)
{
	vm_flags_t vm_flags = READ_ONCE(walk->vma->vm_flags);

	/* Skip non-applicable VMAs */
	/* 必须同时 shared、maywrite，且 HUGETLB 位为 0；私有映射不能污染共享账本。 */
	if ((vm_flags & (VM_SHARED | VM_MAYWRITE | VM_HUGETLB)) !=
	    (VM_SHARED | VM_MAYWRITE))
		return 1;

	return 0;
}

/*
 * clean 与 write-protect 两张操作表共享 VMA 筛选、THP 跳过、notifier/cache/TLB
 * 前后处理，仅 PTE 状态转换不同：前者 clear dirty 并记录 bitmap，后者 clear
 * writable。walk_page_mapping() 同步使用它们，不保存指针，也不创建页表。
 */
static const struct mm_walk_ops clean_walk_ops = {
	.pte_entry = clean_record_pte,
	.pmd_entry = wp_clean_pmd_entry,
	.pud_entry = wp_clean_pud_entry,
	.test_walk = wp_clean_test_walk,
	.pre_vma = wp_clean_pre_vma,
	.post_vma = wp_clean_post_vma
};

/* write-protect 表复用相同边界协议，但把 PTE 回调替换为撤销 writable 位。 */
static const struct mm_walk_ops wp_walk_ops = {
	.pte_entry = wp_pte,
	.pmd_entry = wp_clean_pmd_entry,
	.pud_entry = wp_clean_pud_entry,
	.test_walk = wp_clean_test_walk,
	.pre_vma = wp_clean_pre_vma,
	.post_vma = wp_clean_post_vma
};

/**
 * wp_shared_mapping_range - Write-protect all ptes in an address space range
 * @mapping: The address_space we want to write protect
 * @first_index: The first page offset in the range
 * @nr: Number of incremental page offsets to cover
 *
 * Note: This function currently skips transhuge page-table entries, since
 * it's intended for dirty-tracking on the PTE level. It will warn on
 * encountering transhuge write-enabled entries, though, and can easily be
 * extended to handle them as well.
 *
 * Return: The number of ptes actually write-protected. Note that
 * already write-protected ptes are not counted.
 */
/*
 * 原文边界：该接口当前跳过 transhuge 页表项，因为目标是 PTE 级 dirty tracking；
 * 遇到仍 writable 的 huge leaf 会 WARN。返回数只含本轮实际从 writable 变为
 * read-only 的 PTE，原本只读的不计。
 *
 * 业务背景：对一个 address_space 文件页区间的所有适用共享映射撤销 PTE 写权，
 * 迫使后续 writer 进入 mkwrite 路径。
 * 入参：mapping 是调用期间稳定的借用对象；first_index 是首个文件页偏移，nr
 * 是连续页偏移数，应大于 0 且加法不溢出。
 * 出参/返回：返回实际 write-protect 的 PTE 数；pagewalk 异常仅 WARN，不把错误码
 * 返回调用者。无 mapping/VMA 引用转移。
 * 注意事项：i_mmap 读锁稳定 interval tree 并允许多进程 VMA 遍历；每个 VMA
 * 内仍由 PTL、notifier、cache/TLB 协议保护。函数可进入 notifier/flush 路径。
 */
unsigned long wp_shared_mapping_range(struct address_space *mapping,
				      pgoff_t first_index, pgoff_t nr)
{
	struct wp_walk wpwalk = { .total = 0 };

	/* 同一文件可映射进多个 mm；i_mmap_rwsem 而非任一 mm 的 mmap_lock 稳定集合。 */
	i_mmap_lock_read(mapping);
	WARN_ON(walk_page_mapping(mapping, first_index, nr, &wp_walk_ops,
				  &wpwalk));
	i_mmap_unlock_read(mapping);

	return wpwalk.total;
}

/* 导出给 vmwgfx 等模块；不是用户 ABI。 */
EXPORT_SYMBOL_GPL(wp_shared_mapping_range);

/**
 * clean_record_shared_mapping_range - Clean and record all ptes in an
 * address space range
 * @mapping: The address_space we want to clean
 * @first_index: The first page offset in the range
 * @nr: Number of incremental page offsets to cover
 * @bitmap_pgoff: The page offset of the first bit in @bitmap
 * @bitmap: Pointer to a bitmap of at least @nr bits. The bitmap needs to
 * cover the whole range @first_index..@first_index + @nr.
 * @start: Pointer to number of the first set bit in @bitmap.
 * is modified as new bits are set by the function.
 * @end: Pointer to the number of the last set bit in @bitmap.
 * none set. The value is modified as new bits are set by the function.
 *
 * When this function returns there is no guarantee that a CPU has
 * not already dirtied new ptes. However it will not clean any ptes not
 * reported in the bitmap. The guarantees are as follows:
 *
 * * All ptes dirty when the function starts executing will end up recorded
 *   in the bitmap.
 * * All ptes dirtied after that will either remain dirty, be recorded in the
 *   bitmap or both.
 *
 * If a caller needs to make sure all dirty ptes are picked up and none
 * additional are added, it first needs to write-protect the address-space
 * range and make sure new writers are blocked in page_mkwrite() or
 * pfn_mkwrite(). And then after a TLB flush following the write-protection
 * pick up all dirty bits.
 *
 * This function currently skips transhuge page-table entries, since
 * it's intended for dirty-tracking on the PTE level. It will warn on
 * encountering transhuge dirty entries, though, and can easily be extended
 * to handle them as well.
 *
 * Return: The number of dirty ptes actually cleaned.
 */
/*
 * 原文并发保证的核心是“不漏报被清位”：函数返回后 CPU 可能已再次置 dirty，
 * 但任何由本函数清掉的旧 dirty 都先写入 bitmap。开始时已 dirty 的 PTE 必被
 * 记录；执行期间新 dirty 则会保持 dirty、被记录，或两者兼有。若调用者要求
 * 封闭快照，必须先 write-protect 全区间、在 mkwrite 阻塞新 writer，完成 TLB
 * flush 后再调用本函数。THP 与 wp 接口相同，只 WARN 并跳过。
 *
 * 业务背景：扫描共享映射 PTE 的硬件 dirty 位，将结果累积到文件页 bitmap，
 * 同时清位以建立下一轮扫描起点。
 * 入参：mapping/first_index/nr 定义文件页范围；bitmap_pgoff 定义 bit 0 对应页；
 * bitmap 至少覆盖全范围且由调用者持有；start/end 是输入输出的相对 bit 半开
 * 边界，*start>=*end 表示此前无置位，所有指针均不可为 NULL。
 * 出参/返回：返回实际从 dirty 变 clean 的 PTE 数；OR 更新 bitmap，并把
 * *start/*end 扩为包含新旧置位的边界；pagewalk 错误只 WARN。
 * 注意事项：不清已有 bitmap 位，不阻止 PTE 再次变脏；调用者负责串行化同一
 * bitmap 的扫描者。bitmap 基准与范围必须保证计算出的无符号 pgoff 不越界。
 */
unsigned long clean_record_shared_mapping_range(struct address_space *mapping,
						pgoff_t first_index, pgoff_t nr,
						pgoff_t bitmap_pgoff,
						unsigned long *bitmap,
						pgoff_t *start,
						pgoff_t *end)
{
	/* start>=end 是空集合哨兵；否则继承调用者已有 dirty 子区间继续扩张。 */
	bool none_set = (*start >= *end);
	struct clean_walk cwalk = {
		.base = { .total = 0 },
		.bitmap_pgoff = bitmap_pgoff,
		.bitmap = bitmap,
		.start = none_set ? nr : *start,
		.end = none_set ? 0 : *end,
	};

	/* interval tree 读锁覆盖全部 VMA 回调；bitmap 本身的并发由上层对象锁负责。 */
	i_mmap_lock_read(mapping);
	WARN_ON(walk_page_mapping(mapping, first_index, nr, &clean_walk_ops,
				  &cwalk.base));
	i_mmap_unlock_read(mapping);

	*start = cwalk.start;
	*end = cwalk.end;

	return cwalk.base.total;
}

/* 导出给需要 PTE dirty bitmap 的 GPL 驱动；当前主要用户是 vmwgfx。 */
EXPORT_SYMBOL_GPL(clean_record_shared_mapping_range);
