// SPDX-License-Identifier: GPL-2.0
/*
 * High memory handling common code and variables.
 *
 * (C) 1999 Andrea Arcangeli, SuSE GmbH, andrea@suse.de
 *          Gerhard Wichert, Siemens AG, Gerhard.Wichert@pdb.siemens.de
 *
 *
 * Redesigned the x86 32-bit VM architecture to deal with
 * 64-bit physical space. With current x86 CPUs this
 * means up to 64 Gigabytes physical RAM.
 *
 * Rewrote high memory support to move the page cache into
 * high memory. Implemented permanent (schedulable) kmaps
 * based on Linus' idea.
 *
 * Copyright (C) 1999 Ingo Molnar <mingo@redhat.com>
 */

#include <linux/mm.h>
/* mm.h 提供 struct page、folio 与 page table 基础操作；highmem API 的对象粒度始终是 page。 */
#include <linux/export.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/pagemap.h>
#include <linux/mempool.h>
#include <linux/init.h>
#include <linux/hash.h>
#include <linux/highmem.h>
#include <linux/kgdb.h>
#include <asm/tlbflush.h>
/* 架构 TLB/cache hook 决定 PTE 更新何时对 CPU 可见，通用层不自行假设一致性模型。 */
#include <linux/vmalloc.h>

/* highmem 仅在无法永久线性映射全部 RAM 的架构有意义；本文件同时提供配置桩与共享算法。 */

#ifdef CONFIG_KMAP_LOCAL
/* 每 CPU fixmap 槽以 CPU 基址加逻辑深度编号；迁移禁用保证映射期间索引不漂移。 */
static inline int kmap_local_calc_idx(int idx)
{
	/* 输入是 task 栈逻辑深度，输出是当前 CPU fixmap 的物理槽号。 */
	return idx + KM_MAX_IDX * smp_processor_id();
}

#ifndef arch_kmap_local_map_idx
/* 架构不覆写时，逻辑深度直接映射到本 CPU 的固定槽区。 */
#define arch_kmap_local_map_idx(idx, pfn)	kmap_local_calc_idx(idx)
#endif
#endif /* CONFIG_KMAP_LOCAL */

/*
 * Virtual_count is not a pure "count".
 *  0 means that it is not mapped, and has not been mapped
 *    since a TLB flush - it is usable.
 *  1 means that there are no users, but it has been mapped
 *    since the last TLB flush - so we can't use it.
 *  n means that there are (n-1) current users of it.
 */
#ifdef CONFIG_HIGHMEM

/* 永久 pkmap 区是共享、可睡眠的高端页窗口；计数 1 表示 PTE 尚留而无人持有。 */

/*
 * Architecture with aliasing data cache may define the following family of
 * helper functions in its asm/highmem.h to control cache color of virtual
 * addresses where physical memory pages are mapped by kmap.
 */
#ifndef get_pkmap_color

/*
 * Determine color of virtual address where the page should be mapped.
 */
static inline unsigned int get_pkmap_color(const struct page *page)
{
	/* 无 alias-cache 的默认架构只有一种 color，因此始终落到同一轮转序列。 */
	return 0;
}
#define get_pkmap_color get_pkmap_color

/*
 * Get next index for mapping inside PKMAP region for page with given color.
 */
static inline unsigned int get_next_pkmap_nr(unsigned int color)
{
	/* 该静态游标受 kmap_lock 保护；绕回时才批量回收 count==1 的旧映射。 */
	static unsigned int last_pkmap_nr;

	last_pkmap_nr = (last_pkmap_nr + 1) & LAST_PKMAP_MASK;
	return last_pkmap_nr;
}

/*
 * Determine if page index inside PKMAP region (pkmap_nr) of given color
 * has wrapped around PKMAP region end. When this happens an attempt to
 * flush all unused PKMAP slots is made.
 */
static inline int no_more_pkmaps(unsigned int pkmap_nr, unsigned int color)
{
	/* 默认色彩下 slot 0 标志完整扫描结束；有色架构可覆写此边界。 */
	return pkmap_nr == 0;
}

/*
 * Get the number of PKMAP entries of the given color. If no free slot is
 * found after checking that many entries, kmap will sleep waiting for
 * someone to call kunmap and free PKMAP slot.
 */
static inline int get_pkmap_entries_count(unsigned int color)
{
	/* 返回本色可搜索上限，耗尽后调用者睡眠等 kunmap_high 唤醒。 */
	return LAST_PKMAP;
}

/*
 * Get head of a wait queue for PKMAP entries of the given color.
 * Wait queues for different mapping colors should be independent to avoid
 * unnecessary wakeups caused by freeing of slots of other colors.
 */
static inline wait_queue_head_t *get_pkmap_wait_queue_head(unsigned int color)
{
	/* 默认共用队列；alias 架构可按 color 分队列以避免无关唤醒。 */
	static DECLARE_WAIT_QUEUE_HEAD(pkmap_map_wait);

	return &pkmap_map_wait;
}
#endif

unsigned long __nr_free_highpages(void)
{
	/* 只汇总 populated highmem zone 的 buddy 空闲页，不代表可立即映射的虚拟槽。 */
	unsigned long pages = 0;
	struct zone *zone;

	for_each_populated_zone(zone) {
		if (is_highmem(zone))
			pages += zone_page_state(zone, NR_FREE_PAGES);
	}

	return pages;
}

unsigned long __totalhigh_pages(void)
{
	/* managed_pages 是 zone 管理容量，适合作为 highmem 总量统计而非 free 量。 */
	unsigned long pages = 0;
	struct zone *zone;

	for_each_populated_zone(zone) {
		if (is_highmem(zone))
			pages += zone_managed_pages(zone);
	}

	return pages;
}
EXPORT_SYMBOL(__totalhigh_pages);

static int pkmap_count[LAST_PKMAP];
/* pkmap_count/page_address/PTE 的三元状态全由 kmap_lock 串行维护。 */
static  __cacheline_aligned_in_smp DEFINE_SPINLOCK(kmap_lock);
/* 单独 cacheline 避免频繁 kmap/kunmap 与无关锁产生 false sharing。 */

pte_t *pkmap_page_table;
/* 永久映射 PTE 表由架构初始化；此文件只在锁保护下读写其中 PKMAP 范围。 */

/*
 * Most architectures have no use for kmap_high_get(), so let's abstract
 * the disabling of IRQ out of the locking in that case to save on a
 * potential useless overhead.
 */
#ifdef ARCH_NEEDS_KMAP_HIGH_GET
/* 需要 atomic 获取既有映射的架构把 IRQ 关闭并入 kmap_lock，保证 PTE 反查稳定。 */
#define lock_kmap()             spin_lock_irq(&kmap_lock)
#define unlock_kmap()           spin_unlock_irq(&kmap_lock)
#define lock_kmap_any(flags)    spin_lock_irqsave(&kmap_lock, flags)
#define unlock_kmap_any(flags)  spin_unlock_irqrestore(&kmap_lock, flags)
#else
/* 多数架构无需 IRQ 屏蔽；flags 参数在宏桩中显式消费以保持调用形式一致。 */
#define lock_kmap()             spin_lock(&kmap_lock)
#define unlock_kmap()           spin_unlock(&kmap_lock)
#define lock_kmap_any(flags)    \
		do { spin_lock(&kmap_lock); (void)(flags); } while (0)
#define unlock_kmap_any(flags)  \
		do { spin_unlock(&kmap_lock); (void)(flags); } while (0)
#endif

struct page *__kmap_to_page(void *vaddr)
{
	/* 调试/回退反查：先判永久 PKMAP，再扫描当前任务 local 栈，最后视作线性映射。 */
	unsigned long base = (unsigned long) vaddr & PAGE_MASK;
	struct kmap_ctrl *kctrl = &current->kmap_ctrl;
	/* kmap_ctrl 属于 task，local 映射允许调度但切换钩子会临时撤销本 CPU 的 PTE。 */
	unsigned long addr = (unsigned long)vaddr;
	int i;
	/* i 同时表示逻辑 local 深度；架构可按 PFN 重映射成不同硬件槽。 */

	/* kmap() mappings */
	/* PKMAP vaddr 可直接由槽号索引 PTE，但 WARN 表明该 helper 不应被一般路径滥用。 */
	if (WARN_ON_ONCE(addr >= PKMAP_ADDR(0) &&
			 addr < PKMAP_ADDR(LAST_PKMAP)))
		return pte_page(ptep_get(&pkmap_page_table[PKMAP_NR(addr)]));
	/* ptep_get 提供与并发 PTE 修改兼容的读取形式，但调用者仍应遵守 kmap 协议。 */

	/* kmap_local_page() mappings */
	/* local 映射只对 current 的 kmap_ctrl 有意义；遍历深度避免把其他任务槽误认。 */
	if (WARN_ON_ONCE(base >= __fix_to_virt(FIX_KMAP_END) &&
			 base < __fix_to_virt(FIX_KMAP_BEGIN))) {
		for (i = 0; i < kctrl->idx; i++) {
			/* pteval 快照是该任务当前深度的权威值，PTE 槽本身可能正被调度钩子处理。 */
			unsigned long base_addr;
			int idx;
			pte_t pteval = kctrl->pteval[i];

			idx = arch_kmap_local_map_idx(i, pte_pfn(pteval));
			/* 用 PFN 让彩色/非线性架构复现 map 时的实际虚拟槽。 */
			base_addr = __fix_to_virt(FIX_KMAP_BEGIN + idx);

			if (base_addr == base)
				return pte_page(pteval);
		}
	}

	return virt_to_page(vaddr);
}
/* 线性地址分支不需要锁，前两个分支的元数据访问则由各自调用协议保证。 */
EXPORT_SYMBOL(__kmap_to_page);

static void flush_all_zero_pkmaps(void)
{
	/* 锁内只撤销“count==1 的无人持有 PTE”；随后一次 TLB flush 才让槽可重用。 */
	int i;
	int need_flush = 0;

	flush_cache_kmaps();
	/* cache flush 先于 PTE 清除，避免 alias 数据 cache 保留已回收槽对应的旧页内容。 */

	for (i = 0; i < LAST_PKMAP; i++) {
		/* 全表扫描在绕回时发生，不在每次 kunmap 付出单槽 TLB flush 成本。 */
		struct page *page;
		pte_t ptent;

		/*
		 * zero means we don't have anything to do,
		 * >1 means that it is still in use. Only
		 * a count of 1 means that it is free but
		 * needs to be unmapped
		 */
		if (pkmap_count[i] != 1)
			/* 0 已空、>1 仍有持有者，二者都不能清 PTE。 */
			continue;
		pkmap_count[i] = 0;

		/* sanity check */
		ptent = ptep_get(&pkmap_page_table[i]);
		BUG_ON(pte_none(ptent));

		/*
		 * Don't need an atomic fetch-and-clear op here;
		 * no-one has the page mapped, and cannot get at
		 * its virtual address (and hence PTE) without first
		 * getting the kmap_lock (which is held here).
		 * So no dangers, even with speculative execution.
		 */
		page = pte_page(ptent);
		/* 先取 page 再清 PTE，之后解除 page_address 的哈希关联。 */
		pte_clear(&init_mm, PKMAP_ADDR(i), &pkmap_page_table[i]);

		set_page_address(page, NULL);
		/* page→vaddr 关联必须在 count 置零前撤销，避免新查找返回无 PTE 地址。 */
		need_flush = 1;
	}
	if (need_flush)
		/* PTE clear 对其他 CPU 不够，范围 TLB flush 是 count 从 1 到 0 的发布点。 */
		flush_tlb_kernel_range(PKMAP_ADDR(0), PKMAP_ADDR(LAST_PKMAP));
	/* flush 完成后所有清零 count 的 slot 才真正可供 map_new_virtual 再次选择。 */
}

void __kmap_flush_unused(void)
{
	/* 外部回收入口取得相同锁，避免与 kmap_high 重新引用旧 vaddr 竞争。 */
	lock_kmap();
	flush_all_zero_pkmaps();
	unlock_kmap();
}
/* 此导出入口常用于内存压力/回收，不能在持有会与 kmap_lock 反向依赖的锁时调用。 */

static inline unsigned long map_new_virtual(struct page *page)
{
	/* 调用时已持 kmap_lock；返回的 vaddr 同时发布 PTE、page_address 和占位计数 1。 */
	unsigned long vaddr;
	int count;
	unsigned int last_pkmap_nr;
	unsigned int color = get_pkmap_color(page);
	/* color 必须由 page 稳定属性决定；同页重映射到不同 color 会造成 cache alias。 */

start:
	/* 每次从睡眠回来均重新初始化可查数量，因为其他线程可能已改变整轮状态。 */
	count = get_pkmap_entries_count(color);
	/* Find an empty entry */
	for (;;) {
		last_pkmap_nr = get_next_pkmap_nr(color);
		if (no_more_pkmaps(last_pkmap_nr, color)) {
			flush_all_zero_pkmaps();
			count = get_pkmap_entries_count(color);
		}
		if (!pkmap_count[last_pkmap_nr])
			/* count 0 表示 TLB 已经在某个先前批次刷新，PTE 槽可安全复用。 */
			break;	/* Found a usable entry */
		if (--count)
			/* 仍有其他同色槽未检查，继续轮转而不是立即阻塞。 */
			continue;

		/*
		 * Sleep for somebody else to unmap their entries
		 */
		{
			/* 释放 spinlock 后不可保存任何 slot 假设；醒来须重新检查 page 是否已被映射。 */
			DECLARE_WAITQUEUE(wait, current);
			wait_queue_head_t *pkmap_map_wait =
				get_pkmap_wait_queue_head(color);

			__set_current_state(TASK_UNINTERRUPTIBLE);
			/* 不可中断等待保证资源不足不会把部分 kmap 语义暴露为 EINTR。 */
			add_wait_queue(pkmap_map_wait, &wait);
			/* 入队后才放锁，形成“检查无槽→登记等待者→睡眠”的无漏唤醒序列。 */
			unlock_kmap();
			schedule();
			remove_wait_queue(pkmap_map_wait, &wait);
			/* 被唤醒不等于获得槽，只表示需要重新竞争/扫描。 */
			lock_kmap();

			/* Somebody else might have mapped it while we slept */
			if (page_address(page))
				/* 睡眠期间其他调用者可能先建好相同页映射，直接共享即可。 */
				return (unsigned long)page_address(page);

			/* Re-start */
			goto start;
		}
	}
	vaddr = PKMAP_ADDR(last_pkmap_nr);
	/* set_pte_at 后 slot 至少留 count 1，page_address 使并发 kmap_high 能共用映射。 */
	set_pte_at(&init_mm, vaddr,
		   &(pkmap_page_table[last_pkmap_nr]), mk_pte(page, kmap_prot));

	pkmap_count[last_pkmap_nr] = 1;
	/* 顺序保证其他持锁路径看到 page_address 时，PTE 和基线计数已同时就绪。 */
	set_page_address(page, (void *)vaddr);

	return vaddr;
}

/**
 * kmap_high - map a highmem page into memory
 * @page: &struct page to map
 *
 * Returns the page's virtual memory address.
 *
 * We cannot call this from interrupts, as it may block.
 */
void *kmap_high(struct page *page)
{
	/* 获取一个永久映射引用；成功返回必须由 kunmap_high 配对，且该接口可睡眠。 */
	unsigned long vaddr;
	/* 只保存整数地址以便 PKMAP_NR；返回前转换为 void *，不产生额外页引用。 */

	/*
	 * For highmem pages, we can't trust "virtual" until
	 * after we have the lock.
	 */
	lock_kmap();
	/* 高端页没有 stable linear address，只有锁内 page_address 才能作为当前真相。 */
	vaddr = (unsigned long)page_address(page);
	/* page_address 的读取和 count 增加在同一锁内，防止 flush 清 PTE 的同时取得旧地址。 */
	if (!vaddr)
		vaddr = map_new_virtual(page);
	pkmap_count[PKMAP_NR(vaddr)]++;
	BUG_ON(pkmap_count[PKMAP_NR(vaddr)] < 2);
	/* 返回时 count>=2：一个缓存映射基线加当前调用者的可见引用。 */
	unlock_kmap();
	return (void *) vaddr;
}
EXPORT_SYMBOL(kmap_high);

#ifdef ARCH_NEEDS_KMAP_HIGH_GET
/**
 * kmap_high_get - pin a highmem page into memory
 * @page: &struct page to pin
 *
 * Returns the page's current virtual memory address, or NULL if no mapping
 * exists.  If and only if a non null address is returned then a
 * matching call to kunmap_high() is necessary.
 *
 * This can be called from any context.
 */
void *kmap_high_get(const struct page *page)
{
	/* 原子上下文只借用既存永久映射；没有映射就返回 NULL，绝不创建或睡眠。 */
	unsigned long vaddr, flags;

	lock_kmap_any(flags);
	vaddr = (unsigned long)page_address(page);
	/* 仅增加已有引用，不会把 count 0/1 的地址重新变成可用映射。 */
	if (vaddr) {
		BUG_ON(pkmap_count[PKMAP_NR(vaddr)] < 1);
		pkmap_count[PKMAP_NR(vaddr)]++;
	}
	unlock_kmap_any(flags);
	return (void *) vaddr;
}
#endif

/**
 * kunmap_high - unmap a highmem page into memory
 * @page: &struct page to unmap
 *
 * If ARCH_NEEDS_KMAP_HIGH_GET is not defined then this may be called
 * only from user context.
 */
void kunmap_high(const struct page *page)
{
	/* 释放引用但保留 count==1 的缓存 PTE；真正拆 PTE 延后到下一轮 flush。 */
	unsigned long vaddr;
	unsigned long nr;
	unsigned long flags;
	/* flags 仅 ARCH_NEEDS_KMAP_HIGH_GET 生效；普通架构宏会安全忽略它。 */
	int need_wakeup;
	unsigned int color = get_pkmap_color(page);
	wait_queue_head_t *pkmap_map_wait;

	lock_kmap_any(flags);
	vaddr = (unsigned long)page_address(page);
	BUG_ON(!vaddr);
	nr = PKMAP_NR(vaddr);
	/* address→slot 转换只在永久窗口内有效，BUG_ON(!vaddr) 防止错误的配对释放。 */

	/*
	 * A count must never go down to zero
	 * without a TLB flush!
	 */
	need_wakeup = 0;
	switch (--pkmap_count[nr]) {
	/* 到 1 时最后用户刚离开，保留 PTE 供缓存命中或后续批量回收。 */
	case 0:
		BUG();
	case 1:
		/*
		 * Avoid an unnecessary wake_up() function call.
		 * The common case is pkmap_count[] == 1, but
		 * no waiters.
		 * The tasks queued in the wait-queue are guarded
		 * by both the lock in the wait-queue-head and by
		 * the kmap_lock.  As the kmap_lock is held here,
		 * no need for the wait-queue-head's lock.  Simply
		 * test if the queue is empty.
		 */
		pkmap_map_wait = get_pkmap_wait_queue_head(color);
		need_wakeup = waitqueue_active(pkmap_map_wait);
	}
	unlock_kmap_any(flags);

	/* do wake-up, if needed, race-free outside of the spin lock */
	if (need_wakeup)
		/* 唤醒放锁外，waitqueue 自身同步与 kmap_lock 的判定共同避免漏唤醒。 */
		wake_up(pkmap_map_wait);
}
EXPORT_SYMBOL(kunmap_high);

void zero_user_segments(struct page *page, unsigned start1, unsigned end1,
		unsigned start2, unsigned end2)
{
	/* 对 compound page 将两个半开区间逐页切分；同页时复用一次 local kmap。 */
	unsigned int i;
	/* start/end 在循环中被破坏性推进，函数结束 BUG_ON 校验所有剩余量已消费。 */

	BUG_ON(end1 > page_size(page) || end2 > page_size(page));
	/* 参数是相对 compound page 的半开区间，越界会直接损坏相邻页，故先硬失败。 */

	if (start1 >= end1)
		start1 = end1 = 0;
	if (start2 >= end2)
		start2 = end2 = 0;

	for (i = 0; i < compound_nr(page); i++) {
		/* 每轮把逻辑偏移消耗为本页偏移，两个区间可重叠但 memset 零化幂等。 */
		void *kaddr = NULL;
		/* kaddr 延迟到本页确实含有任一区间再建立，减少 local map 嵌套压力。 */

		if (start1 >= PAGE_SIZE) {
			/* 完整跨过该页时只平移区间，无需建立临时映射。 */
			start1 -= PAGE_SIZE;
			end1 -= PAGE_SIZE;
		} else {
			unsigned this_end = min_t(unsigned, end1, PAGE_SIZE);
			/* 将第一段截到当前页，避免一次 memset 跨越 local mapping 的单页边界。 */

			if (end1 > start1) {
				kaddr = kmap_local_page(page + i);
				memset(kaddr + start1, 0, this_end - start1);
			}
			end1 -= this_end;
			start1 = 0;
		}

		if (start2 >= PAGE_SIZE) {
			/* 第二段独立推进，因此两个区间可以位于 compound folio 的不同页。 */
			start2 -= PAGE_SIZE;
			end2 -= PAGE_SIZE;
		} else {
			unsigned this_end = min_t(unsigned, end2, PAGE_SIZE);
			/* 若第一段已映射本页，下面优先复用 kaddr，保证只有一个 kunmap_local。 */

			if (end2 > start2) {
				if (!kaddr)
					kaddr = kmap_local_page(page + i);
				memset(kaddr + start2, 0, this_end - start2);
			}
			end2 -= this_end;
			start2 = 0;
		}

		if (kaddr) {
			/* kunmap 前完成写入，随后 flush dcache 让可能 alias 的用户映射观察到零数据。 */
			kunmap_local(kaddr);
			flush_dcache_page(page + i);
			/* 对没有 alias 的架构该 hook 可为空；调用点仍统一保持可移植语义。 */
		}

		if (!end1 && !end2)
			/* 两段都耗尽即可提早退出，避免无意义地映射 compound 的后续页。 */
			break;
	}

	BUG_ON((start1 | start2 | end1 | end2) != 0);
}
EXPORT_SYMBOL(zero_user_segments);
#endif /* CONFIG_HIGHMEM */

#ifdef CONFIG_KMAP_LOCAL

#include <asm/kmap_size.h>

/*
 * With DEBUG_KMAP_LOCAL the stack depth is doubled and every second
 * slot is unused which acts as a guard page
 */
#ifdef CONFIG_DEBUG_KMAP_LOCAL
/* debug 用步长 2 留出未映射 guard slot，错误的越界深度更容易被捕获。 */
# define KM_INCR	2
#else
/* 非调试模式每次压栈只消耗一个硬件 PTE 槽。 */
# define KM_INCR	1
#endif

static inline int kmap_local_idx_push(void)
{
	/* task 私有栈深度；hardirq 需关 IRQ 才允许嵌套，debug 配置用空槽作 guard。 */
	WARN_ON_ONCE(in_hardirq() && !irqs_disabled());
	/* hardirq 的映射需要 IRQ 禁用以免同 CPU 更深的中断重用 fixmap 栈。 */
	current->kmap_ctrl.idx += KM_INCR;
	BUG_ON(current->kmap_ctrl.idx >= KM_MAX_IDX);
	/* 返回的是刚占用槽，idx 本身已指向下一个可用位置。 */
	return current->kmap_ctrl.idx - 1;
}

static inline int kmap_local_idx(void)
{
	/* 返回最近一次 push 的逻辑槽，调用者必须遵守严格 LIFO unmap。 */
	return current->kmap_ctrl.idx - 1;
}

static inline void kmap_local_idx_pop(void)
{
	/* 下溢说明 unmap 次序/次数错误，BUG 防止后续 PTE 覆盖未知映射。 */
	current->kmap_ctrl.idx -= KM_INCR;
	BUG_ON(current->kmap_ctrl.idx < 0);
}

#ifndef arch_kmap_local_post_map
/* 三个 arch hook 包围 PTE install/remove，供需要额外 cache/TLB 操作的平台覆写。 */
# define arch_kmap_local_post_map(vaddr, pteval)	do { } while (0)
#endif

#ifndef arch_kmap_local_pre_unmap
/* pre-unmap 与 post-unmap 分开，使架构能在 PTE clear 前后分别布置维护操作。 */
# define arch_kmap_local_pre_unmap(vaddr)		do { } while (0)
#endif

#ifndef arch_kmap_local_post_unmap
# define arch_kmap_local_post_unmap(vaddr)		do { } while (0)
#endif

#ifndef arch_kmap_local_unmap_idx
/* 默认 unmap 槽与 map 槽同构；带地址颜色的架构可同时依据 idx 与 vaddr 覆写。 */
#define arch_kmap_local_unmap_idx(idx, vaddr)	kmap_local_calc_idx(idx)
#endif

#ifndef arch_kmap_local_high_get
/* 默认没有永久高端映射的轻量借用能力，统一返回 NULL 走 local PTE 建立。 */
static inline void *arch_kmap_local_high_get(const struct page *page)
{
	return NULL;
}
/* 空实现也让 __kmap_local_page_prot 的快路径无需增加架构条件判断。 */
#endif

#ifndef arch_kmap_local_set_pte
/* 默认直接写 PTE；特殊架构可在此统一插入本地 TLB/cache 同步。 */
#define arch_kmap_local_set_pte(mm, vaddr, ptep, ptev)	\
	set_pte_at(mm, vaddr, ptep, ptev)
#endif

/* Unmap a local mapping which was obtained by kmap_high_get() */
static inline bool kmap_high_unmap_local(unsigned long vaddr)
{
	/* local API 也可能接到 arch 高端永久映射；识别后转交 kunmap_high 释放引用。 */
#ifdef ARCH_NEEDS_KMAP_HIGH_GET
	if (vaddr >= PKMAP_ADDR(0) && vaddr < PKMAP_ADDR(LAST_PKMAP)) {
		/* 读取 PTE 所得 page 与当初 kmap_high_get 的页一致，随后由它递减永久映射引用。 */
		kunmap_high(pte_page(ptep_get(&pkmap_page_table[PKMAP_NR(vaddr)])));
		return true;
	}
#endif
	return false;
}

static pte_t *__kmap_pte;

static pte_t *kmap_get_pte(unsigned long vaddr, int idx)
{
	/* 线性 PTE 数组按负索引寻址；非线性架构必须覆写为 virt_to_kpte。 */
	if (IS_ENABLED(CONFIG_KMAP_LOCAL_NON_LINEAR_PTE_ARRAY))
		/* 非线性数组不可用负偏移假设，按虚拟地址由架构查实际 PTE。 */
		/*
		 * Set by the arch if __kmap_pte[-idx] does not produce
		 * the correct entry.
		 */
		return virt_to_kpte(vaddr);
	if (!__kmap_pte)
		/* 延迟缓存 fixmap 起点 PTE，初始化后该基址稳定，调用在已禁迁移区内。 */
		__kmap_pte = virt_to_kpte(__fix_to_virt(FIX_KMAP_BEGIN));
	return &__kmap_pte[-idx];
}

void *__kmap_local_pfn_prot(unsigned long pfn, pgprot_t prot)
{
	/* 建立 task-local fixmap：迁移禁用固定 CPU，preempt disable 覆盖 PTE/栈状态更新。 */
	pte_t pteval, *kmap_pte;
	unsigned long vaddr;
	int idx;
	/* idx 是 task LIFO 深度经 arch 映射后的硬件索引，二者在非线性架构不同。 */

	/*
	 * Disable migration so resulting virtual address is stable
	 * across preemption.
	 */
	migrate_disable();
	/* 先禁迁移后才计算 CPU 槽；随后关抢占以原子更新任务栈与 PTE。 */
	preempt_disable();
	idx = arch_kmap_local_map_idx(kmap_local_idx_push(), pfn);
	vaddr = __fix_to_virt(FIX_KMAP_BEGIN + idx);
	kmap_pte = kmap_get_pte(vaddr, idx);
	BUG_ON(!pte_none(ptep_get(kmap_pte)));
	/* LIFO 栈约束保证目标槽空闲；非空意味着泄漏 unmap 或错误嵌套。 */
	pteval = pfn_pte(pfn, prot);
	arch_kmap_local_set_pte(&init_mm, vaddr, kmap_pte, pteval);
	/* 架构 hook 可加入必要屏障；post_map 后地址才能交给 C 调用者使用。 */
	arch_kmap_local_post_map(vaddr, pteval);
	current->kmap_ctrl.pteval[kmap_local_idx()] = pteval;
	/* pteval 是 sched-out 时的恢复来源；不能只依赖仍可能被清除的硬件 PTE。 */
	preempt_enable();

	return (void *)vaddr;
}
EXPORT_SYMBOL_GPL(__kmap_local_pfn_prot);

void *__kmap_local_page_prot(const struct page *page, pgprot_t prot)
{
	/* lowmem 常直接返回线性地址；强制 map debug 或 highmem 才消耗 local 槽。 */
	void *kmap;

	/*
	 * To broaden the usage of the actual kmap_local() machinery always map
	 * pages when debugging is enabled and the architecture has no problems
	 * with alias mappings.
	 */
	if (!IS_ENABLED(CONFIG_DEBUG_KMAP_LOCAL_FORCE_MAP) && !PageHighMem(page))
		/* 普通低端页可零成本取得线性地址，调用者仍需按 API 调用 kunmap_local。 */
		return page_address(page);

	/* Try kmap_high_get() if architecture has it enabled */
	kmap = arch_kmap_local_high_get(page);
	/* 架构若能借用 PKMAP，返回地址的 unmap 会在 kmap_high_unmap_local 中转回永久引用。 */
	if (kmap)
		/* 返回借用映射时不压 local idx，因而 unmap 必须走地址范围的特殊分支。 */
		return kmap;

	return __kmap_local_pfn_prot(page_to_pfn(page), prot);
}
EXPORT_SYMBOL(__kmap_local_page_prot);

void kunmap_local_indexed(const void *vaddr)
{
	/* 反向路径先识别非 fixmap 地址，再按 LIFO 槽清 PTE、清 task 快照并恢复调度/迁移。 */
	unsigned long addr = (unsigned long) vaddr & PAGE_MASK;
	pte_t *kmap_pte;
	int idx;

	if (addr < __fix_to_virt(FIX_KMAP_END) ||
		/* fixmap 范围方向与普通地址相反，两个比较共同排除真正 local 槽。 */
	    addr > __fix_to_virt(FIX_KMAP_BEGIN)) {
		if (IS_ENABLED(CONFIG_DEBUG_KMAP_LOCAL_FORCE_MAP)) {
			/* This _should_ never happen! See above. */
			WARN_ON_ONCE(1);
			return;
		}
		/*
		 * Handle mappings which were obtained by kmap_high_get()
		 * first as the virtual address of such mappings is below
		 * PAGE_OFFSET. Warn for all other addresses which are in
		 * the user space part of the virtual address space.
		 */
		if (!kmap_high_unmap_local(addr))
			/* 非用户且非 local 地址通常是低端线性地址，unmap 是合法 no-op。 */
			WARN_ON_ONCE(addr < PAGE_OFFSET);
		return;
	}

	preempt_disable();
	/* 保证 idx 与本 CPU PTE 匹配；unmap 完毕后依次恢复抢占与迁移。 */
	idx = arch_kmap_local_unmap_idx(kmap_local_idx(), addr);
	WARN_ON_ONCE(addr != __fix_to_virt(FIX_KMAP_BEGIN + idx));

	kmap_pte = kmap_get_pte(addr, idx);
	/* PTE 清除前执行 arch pre hook，某些平台需先处理别名 cache。 */
	arch_kmap_local_pre_unmap(addr);
	pte_clear(&init_mm, addr, kmap_pte);
	arch_kmap_local_post_unmap(addr);
	current->kmap_ctrl.pteval[kmap_local_idx()] = __pte(0);
	/* 清快照阻止 sched-in 重新装回已解除的映射，然后才 pop 深度。 */
	kmap_local_idx_pop();
	preempt_enable();
	migrate_enable();
}
EXPORT_SYMBOL(kunmap_local_indexed);

/*
 * Invoked before switch_to(). This is safe even when during or after
 * clearing the maps an interrupt which needs a kmap_local happens because
 * the task::kmap_ctrl.idx is not modified by the unmapping code so a
 * nested kmap_local will use the next unused index and restore the index
 * on unmap. The already cleared kmaps of the outgoing task are irrelevant
 * because the interrupt context does not know about them. The same applies
 * when scheduling back in for an interrupt which happens before the
 * restore is complete.
 */
void __kmap_local_sched_out(void)
{
	/* 上下文切出时撤销当前 CPU 的 task-local PTE；idx 保留，使切回可按快照恢复。 */
	struct task_struct *tsk = current;
	pte_t *kmap_pte;
	int i;
	/* 该循环不能调度；switch 代码在相同 CPU 上清理 outgoing task 的映射。 */

	/* Clear kmaps */
	/* 不改变 idx：嵌套中断会从下一未用槽压栈，返回后原任务仍可恢复原深度。 */
	for (i = 0; i < tsk->kmap_ctrl.idx; i++) {
		/* idx 的每一项都是任务在进入调度前尚未配对的映射。 */
		pte_t pteval = tsk->kmap_ctrl.pteval[i];
		/* 每项保留原保护位；恢复不是重新按默认 prot 映射。 */
		unsigned long addr;
		int idx;

		/* With debug all even slots are unmapped and act as guard */
		if (IS_ENABLED(CONFIG_DEBUG_KMAP_LOCAL) && !(i & 0x01)) {
			/* debug guard 必须保持 PTE=0；否则说明 map/unmap 深度对齐已经丢失。 */
			/* guard 位置若有效会让恢复覆盖保护页，因此只检查而不写回。 */
			/* 偶数 guard 槽永远无 PTE；非零说明相邻栈布局已被破坏。 */
			WARN_ON_ONCE(pte_val(pteval) != 0);
			continue;
		}
		if (WARN_ON_ONCE(pte_none(pteval)))
			continue;
		/* 只有有效快照才允许重装；跳过项维持其在 task 栈中的位置供正确 LIFO 配对。 */

		/*
		 * This is a horrible hack for XTENSA to calculate the
		 * coloured PTE index. Uses the PFN encoded into the pteval
		 * and the map index calculation because the actual mapped
		 * virtual address is not stored in task::kmap_ctrl.
		 * For any sane architecture this is optimized out.
		 */
		idx = arch_kmap_local_map_idx(i, pte_pfn(pteval));
		/* 逻辑索引加保存的 PFN 足以重建架构颜色选择，不需要记录原虚拟地址。 */
		/* PTE install 后的 post_map hook 完成架构侧可见性，随后循环可恢复下一槽。 */
		/* 由保存的 PFN 重算彩色索引，适配无法从 i 唯一得到硬件槽的架构。 */

		addr = __fix_to_virt(FIX_KMAP_BEGIN + idx);
		kmap_pte = kmap_get_pte(addr, idx);
		/* set_pte_at 写入的保护属性来自原 pteval，防止 sched-in 把只读映射提升为默认权限。 */
		arch_kmap_local_pre_unmap(addr);
		pte_clear(&init_mm, addr, kmap_pte);
		arch_kmap_local_post_unmap(addr);
	}
}

void __kmap_local_sched_in(void)
{
	/* 切回同一任务后从 pteval 重新发布映射；不得假定 sched-out 前的 PTE 仍存在。 */
	struct task_struct *tsk = current;
	pte_t *kmap_pte;
	int i;
	/* sched-in 在迁移后运行也安全，因为 local 映射按当前 CPU 的 fixmap 重建。 */

	/* Restore kmaps */
	/* 恢复顺序按索引遍历；每个 PTE 只使用任务私有快照，绝不引用已失效 vaddr。 */
	for (i = 0; i < tsk->kmap_ctrl.idx; i++) {
		/* 对应 sched_out 的同一序列；guard 与空 PTE 仍保持跳过。 */
		pte_t pteval = tsk->kmap_ctrl.pteval[i];
		/* 保存 PTE 的 PFN/prot 是恢复输入；地址只在当前 CPU 的 fixmap 中重新计算。 */
		unsigned long addr;
		int idx;

		/* With debug all even slots are unmapped and act as guard */
		if (IS_ENABLED(CONFIG_DEBUG_KMAP_LOCAL) && !(i & 0x01)) {
			/* guard 有效即为栈损坏，故仅告警并跳过，不能把它当普通映射恢复。 */
			WARN_ON_ONCE(pte_val(pteval) != 0);
			continue;
		}
		if (WARN_ON_ONCE(pte_none(pteval)))
			continue;

		/* See comment in __kmap_local_sched_out() */
		idx = arch_kmap_local_map_idx(i, pte_pfn(pteval));
		/* 依据 PFN 重算槽号，支持上下文切换后在不同 CPU 的正确颜色映射。 */
		addr = __fix_to_virt(FIX_KMAP_BEGIN + idx);
		kmap_pte = kmap_get_pte(addr, idx);
		/* 取到目标 PTE 后才安装快照，post_map hook 负责补齐架构局部维护。 */
		set_pte_at(&init_mm, addr, kmap_pte, pteval);
		arch_kmap_local_post_map(addr, pteval);
	}
}

void kmap_local_fork(struct task_struct *tsk)
{
	/* 子任务不能继承父任务未配对的 local 映射；告警后清零避免跨任务 PTE 所有权。 */
	if (WARN_ON_ONCE(tsk->kmap_ctrl.idx))
		memset(&tsk->kmap_ctrl, 0, sizeof(tsk->kmap_ctrl));
}

#endif

#if defined(HASHED_PAGE_VIRTUAL)

/* 某些 highmem 架构不能把 virtual 直接塞进 page，改用按 page 指针哈希的外部关联表。 */

#define PA_HASH_ORDER	7

/*
 * Describes one page->virtual association
 */
struct page_address_map {
	/* hash 链元素与 PKMAP slot 一一对应，virtual 仅在该 slot 的 PTE 有效期间有效。 */
	struct page *page;
	void *virtual;
	struct list_head list;
};

static struct page_address_map page_address_maps[LAST_PKMAP];
/* 数组容量与 PKMAP 槽一致，因此一个永久映射 slot 始终有唯一 map 记录可借用。 */

/*
 * Hash table bucket
 */
static struct page_address_slot {
	struct list_head lh;			/* List of page_address_maps */
	spinlock_t lock;			/* Protect this bucket's list */
} ____cacheline_aligned_in_smp page_address_htable[1<<PA_HASH_ORDER];
/* 桶锁粒度允许不同 highmem page 的查找/发布并行，链长由 hash order 控制。 */

static struct page_address_slot *page_slot(const struct page *page)
{
	/* page 指针 hash 分桶；桶锁保护同桶 page→virtual 关联而非全局 kmap 状态。 */
	return &page_address_htable[hash_ptr(page, PA_HASH_ORDER)];
}
/* 仅返回桶地址，不加锁；调用者 page_address/set_page_address 再取得该桶锁。 */

/**
 * page_address - get the mapped virtual address of a page
 * @page: &struct page to get the virtual address of
 *
 * Returns the page's virtual address.
 */
void *page_address(const struct page *page)
{
	/* lowmem 直接线性映射；highmem 在哈希桶锁下查找，找不到表示当前未永久映射。 */
	unsigned long flags;
	void *ret;
	struct page_address_slot *pas;
	/* slot 不持有 page 引用；调用者必须按正常 page 生命周期保证 page 未释放。 */

	if (!PageHighMem(page))
		/* lowmem 的 direct map 永远存在，不应进入 highmem 哈希表。 */
		return lowmem_page_address(page);

	pas = page_slot(page);
	ret = NULL;
	spin_lock_irqsave(&pas->lock, flags);
	/* 链遍历与 set_page_address 的 add/remove 同锁，ret 离锁后只作地址值返回。 */
	if (!list_empty(&pas->lh)) {
		/* 空桶常见时快速跳过遍历；page 指针比较避免 hash 碰撞误命中。 */
		struct page_address_map *pam;

		list_for_each_entry(pam, &pas->lh, list) {
			if (pam->page == page) {
				/* 同一 page 在同一时刻只应有一个永久 slot 关联，找到即停止。 */
				ret = pam->virtual;
				break;
			}
		}
	}

	spin_unlock_irqrestore(&pas->lock, flags);
	/* 返回 NULL 只表示当前没有永久映射，并不影响 page 本身的引用或物理有效性。 */
	return ret;
}
EXPORT_SYMBOL(page_address);

/**
 * set_page_address - set a page's virtual address
 * @page: &struct page to set
 * @virtual: virtual address to use
 */
void set_page_address(struct page *page, void *virtual)
{
	/* 仅 PKMAP 发布/撤销调用；Add 先填私有 map 再挂链，Remove 在桶锁内摘链。 */
	unsigned long flags;
	struct page_address_slot *pas;
	struct page_address_map *pam;
	/* pam 是静态 slot 存储，不可在锁外长期保存其地址，因为下一次映射会覆盖内容。 */

	BUG_ON(!PageHighMem(page));
	/* 永久高端映射的发布顺序为 PTE→map 字段→链表；撤销时由 flush 路径反向执行。 */
	/* 若传 lowmem 会让 direct-map 与哈希两种真相并存，直接拒绝这种调用者错误。 */

	pas = page_slot(page);
	/* 同 page 的 add/remove 落到同一 hash bucket，保证链表修改无需全局锁。 */
	if (virtual) {		/* Add */
		/* PKMAP_NR 绑定 map 记录到实际 slot，调用方已完成 PTE install 才发布关联。 */
		pam = &page_address_maps[PKMAP_NR((unsigned long)virtual)];
		pam->page = page;
		pam->virtual = virtual;
		/* 字段填充在加桶锁前完成；锁的 acquire/release 使读者不会见到半初始化 pam。 */

		spin_lock_irqsave(&pas->lock, flags);
		/* 发布后 page_address 才可能命中；虚拟地址撤销路径反向在同锁内删除。 */
		list_add_tail(&pam->list, &pas->lh);
		spin_unlock_irqrestore(&pas->lock, flags);
	} else {		/* Remove */
		/* 移除只摘链；静态 map 记录由下次同 slot 映射覆盖，无单独 free。 */
		spin_lock_irqsave(&pas->lock, flags);
		/* 若找不到 page 表明 API 配对错误或先前已撤销，代码保持链表不变。 */
		list_for_each_entry(pam, &pas->lh, list) {
			if (pam->page == page) {
				/* 删除后不清字段无害，因为链外记录不可被 page_address 遍历到。 */
				list_del(&pam->list);
				break;
			}
		}
		spin_unlock_irqrestore(&pas->lock, flags);
	}
}

void __init page_address_init(void)
{
	/* 启动期初始化每个桶的链表与自旋锁，之后查找/更新可在 IRQ 上下文安全执行。 */
	int i;

	for (i = 0; i < ARRAY_SIZE(page_address_htable); i++) {
		/* 每桶独立初始化，启动完成后从不整体重置，避免运行期读者看到空链头。 */
		INIT_LIST_HEAD(&page_address_htable[i].lh);
		spin_lock_init(&page_address_htable[i].lock);
	}
}

#endif	/* defined(HASHED_PAGE_VIRTUAL) */
