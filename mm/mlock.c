// SPDX-License-Identifier: GPL-2.0
/*
 *	linux/mm/mlock.c
 *
 *  (C) Copyright 1995 Linus Torvalds
 *  (C) Copyright 2002 Christoph Hellwig
 */

#include <linux/capability.h>
#include <linux/mman.h>
#include <linux/mm.h>
/* 权限、VMA 标志、RLIMIT 与 folio 状态的公开定义由以上基础头提供。 */
#include <linux/sched/user.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/pagemap.h>
/* 页缓存、folio batch 与页表 walk 共同提供 mlock 从 VMA 策略到单 folio LRU 迁移的桥接。 */
#include <linux/folio_batch.h>
#include <linux/pagewalk.h>
#include <linux/mempolicy.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/export.h>
#include <linux/rmap.h>
#include <linux/mmzone.h>
#include <linux/hugetlb.h>
#include <linux/memcontrol.h>
/* mm_inline 与 secretmem 提供 LRU 辅助操作和不可解锁 VMA 的特殊策略。 */
#include <linux/mm_inline.h>
#include <linux/secretmem.h>

#include "internal.h"

/*
 * 每 CPU 批处理把多次 folio LRU 状态迁移合并：lock 禁止本 CPU 在批数组仍被
 * 修改时递归 drain；fbatch 持有每个已入队 folio 的额外引用，最终由 folios_put()
 * 成对归还。远端 drain 只允许目标 CPU 已下线，因而不需要取得其 local_lock。
 */
struct mlock_fbatch {
	/* 保护当前 CPU 的 fbatch 与其 folio 引用。 */
	local_lock_t lock;
	/* 编码了 mlock/新页/munlock 操作的延迟队列。 */
	struct folio_batch fbatch;
};

static DEFINE_PER_CPU(struct mlock_fbatch, mlock_fbatch) = {
	.lock = INIT_LOCAL_LOCK(lock),
};

/*
 * 业务背景：mlock 系统调用先用此权限门禁，避免无额度且无 CAP_IPC_LOCK 的任务
 * 绕过 RLIMIT_MEMLOCK 固定过多不可回收内存。
 * 入参：无。出参/返回：true 表示任务有非零锁页额度或具备特权，false 表示拒绝。
 * 注意事项：只回答资格，不预留额度也不锁页；并发额度变化仍由后续 mmap_lock
 * 路径和用户计数检查处理。
 */
bool can_do_mlock(void)
{
	if (rlimit(RLIMIT_MEMLOCK) != 0)
		return true;
	if (capable(CAP_IPC_LOCK))
		return true;
	return false;
}
EXPORT_SYMBOL(can_do_mlock);

/*
 * Mlocked folios are marked with the PG_mlocked flag for efficient testing
 * in vmscan and, possibly, the fault path; and to support semi-accurate
 * statistics.
 *
 * An mlocked folio [folio_test_mlocked(folio)] is unevictable.  As such, it
 * will be ostensibly placed on the LRU "unevictable" list (actually no such
 * list exists), rather than the [in]active lists. PG_unevictable is set to
 * indicate the unevictable state.
 */
/*
 * 原注释译注：mlocked folio 以 PG_mlocked 供 vmscan/fault 快速判断并统计；它
 * 不可回收，逻辑上放入 unevictable LRU（并无独立链表），PG_unevictable 标识该状态。
 * mlock_count 处理同一 folio 被多个锁定 VMA 覆盖的近似计数，LRU 锁保护链表和
 * active/unevictable 迁移，PG_lru 的暂时清除则把 folio 从并发 LRU 操作者手中摘除。
 */

/*
 * 业务背景：消费批中“已在/暂离 LRU”的 mlock 请求，把满足条件的 folio 移入
 * unevictable 状态；由 mlock_folio_batch() 分派。
 * 入参：folio 是批持有额外引用的 folio；lruvec 可为 NULL 或上一个持锁 lruvec。
 * 出参/返回：返回当前仍持 irq LRU 锁的 lruvec，调用者最后统一解锁；不归还引用。
 * 注意事项：folio_test_clear_lru() 是迁移所有权门槛；若并发 munlock 已清标志，
 * 必须按现状救回 stranded folio，不能假设批入队时的状态仍成立。
 */
static struct lruvec *__mlock_folio(struct folio *folio, struct lruvec *lruvec)
{
	/* There is nothing more we can do while it's off LRU */
	/* 原注释译注：folio 已不在 LRU 时无法再迁移；保留调用者持有的 lruvec。 */
	if (!folio_test_clear_lru(folio))
		return lruvec;

	lruvec = folio_lruvec_relock_irq(folio, lruvec);

	/* 阶段 1：重取与 folio 当前节点/内存 cgroup 匹配的 LRU 锁，再复核可回收性。 */
	if (unlikely(folio_evictable(folio))) {
		/*
		 * This is a little surprising, but quite possible: PG_mlocked
		 * must have got cleared already by another CPU.  Could this
		 * folio be unevictable?  I'm not sure, but move it now if so.
		 */
		/* 原注释译注：另一 CPU 可能刚清 PG_mlocked；若残留 unevictable 状态便立即救回。 */
		if (folio_test_unevictable(folio)) {
			lruvec_del_folio(lruvec, folio);
			folio_clear_unevictable(folio);
			lruvec_add_folio(lruvec, folio);

			__count_vm_events(UNEVICTABLE_PGRESCUED,
					  folio_nr_pages(folio));
		}
		goto out;
	}

	/* 阶段 2：已在 unevictable 链的嵌套 mlock 只累加近似计数，无需重复搬链。 */
	if (folio_test_unevictable(folio)) {
		if (folio_test_mlocked(folio))
			folio->mlock_count++;
		goto out;
	}

	/* 阶段 3：从原 LRU 摘除、清 active、设 unevictable 后重新入链，期间 LRU 锁稳定。 */
	lruvec_del_folio(lruvec, folio);
	folio_clear_active(folio);
	folio_set_unevictable(folio);
	folio->mlock_count = !!folio_test_mlocked(folio);
	lruvec_add_folio(lruvec, folio);
	__count_vm_events(UNEVICTABLE_PGCULLED, folio_nr_pages(folio));
out:
	folio_set_lru(folio);
	return lruvec;
}

/*
 * 业务背景：新分配 folio 尚未入 LRU，但已带 mlocked 标志时直接以 unevictable
 * 身份首次发布；这是 mlock_new_folio() 批路径的末端。
 * 入参：folio 由批持引用且必须不在 LRU；lruvec 与上一项复用。出参/返回：返回
 * 仍上锁的 lruvec，不释放 folio。注意事项：BUG 断言防止把已发布 folio 当新页
 * 重复插入；若并发变为 evictable，只按普通 LRU 发布。
 */
static struct lruvec *__mlock_new_folio(struct folio *folio, struct lruvec *lruvec)
{
	VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);

	lruvec = folio_lruvec_relock_irq(folio, lruvec);

	/* As above, this is a little surprising, but possible */
	/* 原注释译注：与上例相同，该新页可能已变为 evictable；此时不设置 unevictable。 */
	if (unlikely(folio_evictable(folio)))
		goto out;

	folio_set_unevictable(folio);
	folio->mlock_count = !!folio_test_mlocked(folio);
	__count_vm_events(UNEVICTABLE_PGCULLED, folio_nr_pages(folio));
out:
	lruvec_add_folio(lruvec, folio);
	folio_set_lru(folio);
	return lruvec;
}

/*
 * 业务背景：消费批中的 munlock 请求；最后一个锁定者清 PG_mlocked、扣统计，并在
 * folio 重新可回收时把它从 unevictable 迁回普通 LRU。
 * 入参：folio 是批持有引用；lruvec 可复用且仅在成功摘 LRU 后上锁。出参/返回：
 * 返回仍持锁 lruvec；引用留给批尾 folios_put()。注意事项：mlock_count 允许
 * 低估，reclaim 会兜底修正；不能在清 PG_mlocked 前判断 folio_evictable()。
 */
static struct lruvec *__munlock_folio(struct folio *folio, struct lruvec *lruvec)
{
	int nr_pages = folio_nr_pages(folio);
	bool isolated = false;

	/* 阶段 1：只有取得 PG_lru 的迁移权才可安全触碰 LRU 链；否则直接尝试清 mlocked。 */
	if (!folio_test_clear_lru(folio))
		goto munlock;

	isolated = true;
	lruvec = folio_lruvec_relock_irq(folio, lruvec);

	if (folio_test_unevictable(folio)) {
		/* Then mlock_count is maintained, but might undercount */
		/* 原注释译注：此时维护 mlock_count，但它可能低估嵌套锁定次数。 */
		if (folio->mlock_count)
			folio->mlock_count--;
		if (folio->mlock_count)
			goto out;
	}
	/* else assume that was the last mlock: reclaim will fix it if not */
	/* 原注释译注：否则假设这是最后一个 mlock；若不准确由 reclaim 最终修正。 */

munlock:
	if (folio_test_clear_mlocked(folio)) {
		__zone_stat_mod_folio(folio, NR_MLOCK, -nr_pages);
		if (isolated || !folio_test_unevictable(folio))
			__count_vm_events(UNEVICTABLE_PGMUNLOCKED, nr_pages);
		else
			__count_vm_events(UNEVICTABLE_PGSTRANDED, nr_pages);
	}

	/* folio_evictable() has to be checked *after* clearing Mlocked */
	/* 原注释译注：必须清除 Mlocked 后才判断可回收性，否则状态谓词仍会返回不可回收。 */
	if (isolated && folio_test_unevictable(folio) && folio_evictable(folio)) {
		/* 最后一个锁撤销后，把曾摘除的 folio 从 unevictable 链救回普通可回收 LRU。 */
		lruvec_del_folio(lruvec, folio);
		folio_clear_unevictable(folio);
		lruvec_add_folio(lruvec, folio);
		__count_vm_events(UNEVICTABLE_PGRESCUED, nr_pages);
	}
out:
	if (isolated)
		folio_set_lru(folio);
	return lruvec;
}

/*
 * Flags held in the low bits of a struct folio pointer on the mlock_fbatch.
 */
/*
 * 原注释译注：批数组中的 folio 指针低位编码操作类型。folio 按对象对齐，低两位
 * 可临时借作 tag：LRU_FOLIO 表示现有 LRU 页锁定，NEW_FOLIO 表示新页锁定，零
 * 表示解锁；解码前必须去除 tag，不能把编码指针传给 folio API。
 */
#define LRU_FOLIO 0x1
#define NEW_FOLIO 0x2
/* 将“已在 LRU 的锁定”操作编码进对齐 folio 借用指针；不改变引用或对象状态。 */
static inline struct folio *mlock_lru(struct folio *folio)
{
	return (struct folio *)((unsigned long)folio + LRU_FOLIO);
}

/* 将“新页锁定”操作编码进指针低位；只可由批分派器在去 tag 后消费。 */
static inline struct folio *mlock_new(struct folio *folio)
{
	return (struct folio *)((unsigned long)folio + NEW_FOLIO);
}

/*
 * mlock_folio_batch() is derived from folio_batch_move_lru(): perhaps that can
 * make use of such folio pointer flags in future, but for now just keep it for
 * mlock.  We could use three separate folio batches instead, but one feels
 * better (munlocking a full folio batch does not need to drain mlocking folio
 * batches first).
 */
/*
 * 原注释译注：该批处理源自 folio_batch_move_lru()；未来可共用指针标志，目前
 * mlock 独用。一个批优于三个批，因为解锁整批不必先 drain 锁定批。
 * 业务背景：在 local_lock 下积累操作，出锁前统一取得/复用 lruvec 锁，降低每页
 * 锁开销。入参：fbatch 属于当前 CPU，含每项额外 folio 引用；出参：无，清空并
 * 归还所有批引用。注意事项：解码后必须回写真实指针；最后先解 lruvec 再 folios_put，
 * 防止释放路径在仍持 LRU 锁时递归。
 */
static void mlock_folio_batch(struct folio_batch *fbatch)
{
	struct lruvec *lruvec = NULL;
	unsigned long mlock;
	struct folio *folio;
	int i;

	/* 阶段 1：逐项解码 tag，分派到现有页锁定、新页锁定或解锁状态机。 */
	for (i = 0; i < folio_batch_count(fbatch); i++) {
		/* 先保存并剥离低位 tag，批数组恢复为真实指针，避免下一次 drain 重复解码。 */
		folio = fbatch->folios[i];
		mlock = (unsigned long)folio & (LRU_FOLIO | NEW_FOLIO);
		folio = (struct folio *)((unsigned long)folio - mlock);
		fbatch->folios[i] = folio;

		if (mlock & LRU_FOLIO)
			/* 现有 LRU 页需要先摘链再迁入 unevictable。 */
			lruvec = __mlock_folio(folio, lruvec);
		else if (mlock & NEW_FOLIO)
			lruvec = __mlock_new_folio(folio, lruvec);
		else
			lruvec = __munlock_folio(folio, lruvec);
	}

	/* 阶段 2：批内复用的 IRQ LRU 锁仅在所有链迁移完成后释放，再归还 folio 引用。 */
	if (lruvec)
		lruvec_unlock_irq(lruvec);
	folios_put(fbatch);
}

/*
 * 业务背景：当前 CPU 在可能睡眠/切换上下文前清空本地延迟 LRU 迁移，供调度和
 * 内存管理边界调用。入参：无；出参/返回：无。注意事项：local_lock 防止本 CPU
 * 并发入队；批为空是无副作用快速路径，批函数负责引用归还。
 */
void mlock_drain_local(void)
{
	struct folio_batch *fbatch;

	local_lock(&mlock_fbatch.lock);
	fbatch = this_cpu_ptr(&mlock_fbatch.fbatch);
	if (folio_batch_count(fbatch))
		mlock_folio_batch(fbatch);
	local_unlock(&mlock_fbatch.lock);
}

/*
 * 业务背景：CPU 下线时由另一 CPU 清理其遗留批。入参：cpu 必须已 offline；
 * 出参/返回：无。注意事项：WARN_ON_ONCE 抓住在线 CPU 的不安全远端访问；离线
 * 保证该 CPU 不会并发修改 per-CPU fbatch，因此这里不能套用 local_lock。
 */
void mlock_drain_remote(int cpu)
{
	struct folio_batch *fbatch;

	WARN_ON_ONCE(cpu_online(cpu));
	fbatch = &per_cpu(mlock_fbatch.fbatch, cpu);
	if (folio_batch_count(fbatch))
		mlock_folio_batch(fbatch);
}

/* 返回指定 CPU 是否还有延迟 mlock 操作；调用者据此决定是否触发离线/切换 drain。 */
bool need_mlock_drain(int cpu)
{
	return folio_batch_count(&per_cpu(mlock_fbatch.fbatch, cpu));
}

/**
 * mlock_folio - mlock a folio already on (or temporarily off) LRU
 * @folio: folio to be mlocked.
 */
/*
 * 原注释译注：锁定一个已在（或暂离）LRU 的 folio。
 * 业务背景：rmap/fault 路径发现 VM_LOCKED 映射时调用它；先发布 PG_mlocked 和
 * 统计，再把带 LRU_FOLIO tag 的额外引用放入本 CPU 批，延迟完成 LRU 迁移。
 * 入参：folio 是调用者仍持有且可被标记的借用 folio。出参/返回：无。
 * 注意事项：local_lock 禁止同 CPU drain 与入队交错；若批满、folio 不能缓存或
 * LRU cache 被禁用，必须同步 drain，不能让带引用的批跨越该边界遗留。
 */
void mlock_folio(struct folio *folio)
{
	struct folio_batch *fbatch;

	local_lock(&mlock_fbatch.lock);
	fbatch = this_cpu_ptr(&mlock_fbatch.fbatch);

	/* 阶段 1：仅 PG_mlocked 从 0→1 的首个锁定者增加 zone/VM 统计。 */
	if (!folio_test_set_mlocked(folio)) {
		int nr_pages = folio_nr_pages(folio);

		zone_stat_mod_folio(folio, NR_MLOCK, nr_pages);
		__count_vm_events(UNEVICTABLE_PGMLOCKED, nr_pages);
	}

	/* 阶段 2：批持一份引用跨越 LRU 迁移；mlock_lru() 在指针低位携带操作类型。 */
	folio_get(folio);
	if (!folio_batch_add(fbatch, mlock_lru(folio)) ||
	    !folio_may_be_lru_cached(folio) || lru_cache_disabled())
		mlock_folio_batch(fbatch);
	local_unlock(&mlock_fbatch.lock);
}

/**
 * mlock_new_folio - mlock a newly allocated folio not yet on LRU
 * @folio: folio to be mlocked, either normal or a THP head.
 */
/*
 * 原注释译注：锁定一个尚未入 LRU 的新分配 folio，普通页或 THP head 均可。
 * 业务背景：新页首次入 LRU 前已属于锁定 VMA 时，应以 unevictable 身份发布。
 * 入参：folio 是调用方持有的正常 folio/THP head；出参/返回：无。
 * 注意事项：这里无条件置 PG_mlocked，因为新页不应已有该标志；批引用在 drain
 * 后归还，NEW_FOLIO tag 令分派器跳过“先摘旧 LRU”的路径。
 */
void mlock_new_folio(struct folio *folio)
{
	struct folio_batch *fbatch;
	int nr_pages = folio_nr_pages(folio);

	local_lock(&mlock_fbatch.lock);
	fbatch = this_cpu_ptr(&mlock_fbatch.fbatch);
	folio_set_mlocked(folio);

	zone_stat_mod_folio(folio, NR_MLOCK, nr_pages);
	__count_vm_events(UNEVICTABLE_PGMLOCKED, nr_pages);

	/* 阶段：发布标志和统计后持额外引用入队；必要时立即完成首次 LRU 发布。 */
	folio_get(folio);
	if (!folio_batch_add(fbatch, mlock_new(folio)) ||
	    !folio_may_be_lru_cached(folio) || lru_cache_disabled())
		mlock_folio_batch(fbatch);
	local_unlock(&mlock_fbatch.lock);
}

/**
 * munlock_folio - munlock a folio
 * @folio: folio to be munlocked, either normal or a THP head.
 */
/*
 * 原注释译注：解锁普通 folio 或 THP head。
 * 业务背景：VMA 失去 VM_LOCKED 时 rmap 路径调用它；实际清 PG_mlocked 被推迟到
 * __munlock_folio()，因为那里才能在 LRU 锁下判断多重 mlock_count。
 * 入参：folio 为调用者持有的借用对象；出参/返回：无。
 * 注意事项：不可在此提前 clear 标志，否则嵌套 VMA 的计数会被跳过；入批的额外
 * 引用由 drain 归还，缓存不可用时同步处理。
 */
void munlock_folio(struct folio *folio)
{
	struct folio_batch *fbatch;

	local_lock(&mlock_fbatch.lock);
	fbatch = this_cpu_ptr(&mlock_fbatch.fbatch);
	/*
	 * folio_test_clear_mlocked(folio) must be left to __munlock_folio(),
	 * which will check whether the folio is multiply mlocked.
	 */
	/* 原注释译注：必须把 clear 留给 __munlock_folio()，后者检查 folio 是否多重锁定。 */
	folio_get(folio);
	if (!folio_batch_add(fbatch, folio) ||
	    !folio_may_be_lru_cached(folio) || lru_cache_disabled())
		mlock_folio_batch(fbatch);
	local_unlock(&mlock_fbatch.lock);
}

/*
 * 业务背景：PTE walk 遇到 large folio 时一次跨过同一 folio 覆盖的连续 PTE，避免
 * 对一个 folio 重复 mlock/munlock。入参：folio、当前 pte 借用；addr/end 为本次
 * walk 半开区间。出参/返回：返回至少 1 的 PTE 步长。注意事项：small folio 固定
 * 一页；large folio 的 batch 值还受当前 PTE 映射与范围限制。
 */
static inline unsigned int folio_mlock_step(struct folio *folio,
		pte_t *pte, unsigned long addr, unsigned long end)
{
	unsigned int count = (end - addr) >> PAGE_SHIFT;
	pte_t ptent = ptep_get(pte);

	if (!folio_test_large(folio))
		return 1;

	return folio_pte_batch(folio, pte, ptent, count);
}

/*
 * 业务背景：大 folio 与 VMA 范围不完全重合时，锁定整个 folio 会把范围外页错误
 * 变为不可回收；解锁则允许部分映射，给压力下拆分和范围外页回收留下机会。
 * 入参：folio/vma 是受 page-table walk 保护的借用对象；start/end 为原请求范围，
 * step 为本次连续 PTE 数。出参/返回：true 表示可执行对应 mlock/munlock。
 * 注意事项：VM_LOCKED 缺失代表解锁；KSM 不适用 folio_within_range，small folio
 * 天然安全；锁定 large folio 必须完整在范围且完整映射。
 */
static inline bool allow_mlock_munlock(struct folio *folio,
		struct vm_area_struct *vma, unsigned long start,
		unsigned long end, unsigned int step)
{
	/*
	 * For unlock, allow munlock large folio which is partially
	 * mapped to VMA. As it's possible that large folio is
	 * mlocked and VMA is split later.
	 *
	 * During memory pressure, such kind of large folio can
	 * be split. And the pages are not in VM_LOCKed VMA
	 * can be reclaimed.
	 */
	/* 原注释译注：部分映射的大 folio 可解锁；之后 VMA 分裂或内存压力拆页时，范围外页能回收。 */
	if (!(vma->vm_flags & VM_LOCKED))
		return true;

	/* folio_within_range() cannot take KSM, but any small folio is OK */
	/* 原注释译注：folio_within_range() 不处理 KSM，但任意 small folio 都可安全处理。 */
	if (!folio_test_large(folio))
		return true;

	/* folio not in range [start, end), skip mlock */
	/* 原注释译注：folio 不完全位于 [start,end) 时跳过 mlock，不能扩大用户请求。 */
	if (!folio_within_range(folio, vma, start, end))
		return false;

	/* folio is not fully mapped, skip mlock */
	/* 原注释译注：folio 未完整映射时跳过 mlock，避免锁定未覆盖的 tail 页。 */
	if (step != folio_nr_pages(folio))
		return false;

	return true;
}

/*
 * 业务背景：walk_page_range() 的 PMD 回调逐个 present 映射找到正常 folio，并按
 * VMA 新 flags 把它们批量 mlock 或 munlock。
 * 入参：pmd 是页表 walk 持有的 PMD；addr/end 是本次半开虚拟区间；walk 提供 vma
 * 与重试 action。出参/返回：0；缺页表时设置 ACTION_AGAIN 让上层重试。
 * 注意事项：THP PMD 和 PTE 路径都在 ptl 下读取；zone-device/zero PMD 不可按普通
 * folio 操作；每轮释放 ptl 后 cond_resched()，故不能把裸 PTE/folio 带出锁外。
 */
static int mlock_pte_range(pmd_t *pmd, unsigned long addr,
			   unsigned long end, struct mm_walk *walk)

{
	struct vm_area_struct *vma = walk->vma;
	spinlock_t *ptl;
	pte_t *start_pte, *pte;
	pte_t ptent;
	struct folio *folio;
	unsigned int step = 1;
	unsigned long start = addr;

	/* 阶段 1：优先处理被 PMD 锁稳定的 THP；空、零页和 ZONE_DEVICE 映射均跳过。 */
	ptl = pmd_trans_huge_lock(pmd, vma);
	if (ptl) {
		/* PMD huge 映射由这一把 ptl 覆盖：任一非普通项都无需进入 PTE 级 walk。 */
		if (!pmd_present(*pmd))
			goto out;
		if (is_huge_zero_pmd(*pmd))
			goto out;
		folio = pmd_folio(*pmd);
		if (folio_is_zone_device(folio))
			goto out;
		/* PMD 覆盖的正常 folio 依 VMA 当前锁位进入对应批路径。 */
		if (vma->vm_flags & VM_LOCKED)
			mlock_folio(folio);
		else
			munlock_folio(folio);
		goto out;
	}

	/* 阶段 2：非 THP PMD 映射并锁 PTE 表；暂时不可映射时请求 walk 框架重试。 */
	start_pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	if (!start_pte) {
		walk->action = ACTION_AGAIN;
		return 0;
	}

	/* 阶段 3：只消费 present 的普通 folio，large folio 按 step 跳过重复 PTE。 */
	for (pte = start_pte; addr != end; pte++, addr += PAGE_SIZE) {
		/* ptep_get() 在 ptl 下取得一致条目；non-present PTE 没有驻留 folio 可锁。 */
		ptent = ptep_get(pte);
		if (!pte_present(ptent))
			continue;
		folio = vm_normal_folio(vma, addr, ptent);
		if (!folio || folio_is_zone_device(folio))
			continue;

		/* 将连续 PTE 折叠成一个 folio 操作；范围/部分映射限制由下个 helper 判定。 */
		step = folio_mlock_step(folio, pte, addr, end);
		if (!allow_mlock_munlock(folio, vma, start, end, step))
			goto next_entry;

		if (vma->vm_flags & VM_LOCKED)
			mlock_folio(folio);
		else
			munlock_folio(folio);

	/* 跳过当前 large folio 剩余 PTE；步长已由 folio_pte_batch() 限制在 end 内。 */
next_entry:
		pte += step - 1;
		addr += (step - 1) << PAGE_SHIFT;
	}
	/* 阶段 4：解除 PTE 映射/锁，随后允许调度；返回前不保留页表借用指针。 */
	pte_unmap(start_pte);
out:
	spin_unlock(ptl);
	cond_resched();
	return 0;
}

/*
 * mlock_vma_pages_range() - mlock any pages already in the range,
 *                           or munlock all pages in the range.
 * @vma - vma containing range to be mlock()ed or munlock()ed
 * @start - start address in @vma of the range
 * @end - end of range in @vma
 * @new_vma_flags - the new set of flags for @vma.
 *
 * Called for mlock(), mlock2() and mlockall(), to set @vma VM_LOCKED;
 * called for munlock() and munlockall(), to clear VM_LOCKED from @vma.
 */
/*
 * 原注释译注：锁定范围内已有页或解锁范围内所有页；mlock/mlock2/mlockall 设置
 * VM_LOCKED，munlock/munlockall 清除它。
 * 业务背景：VMA flags 的提交和页级 LRU 状态必须协同；本函数在 mmap_write_lock
 * 下先用暂时 VM_IO 告知 rmap 竞争者，再遍历页表并最终清掉该哨兵。
 * 入参：vma/flags 是写锁保护的借用对象；start/end 是 VMA 内半开区间。出参：无。
 * 注意事项：walk 期间迁移或 reclaim 可调用 mlock_vma_folio()；VM_IO 防止重复
 * mlock_count，WRITE_ONCE flags 更新使 rmap walker 看见这一互斥组合。
 */
static void mlock_vma_pages_range(struct vm_area_struct *vma,
	unsigned long start, unsigned long end,
	vma_flags_t *new_vma_flags)
{
	static const struct mm_walk_ops mlock_walk_ops = {
		.pmd_entry = mlock_pte_range,
		.walk_lock = PGWALK_WRLOCK_VERIFY,
	};

	/*
	 * There is a slight chance that concurrent page migration,
	 * or page reclaim finding a page of this now-VM_LOCKED vma,
	 * will call mlock_vma_folio() and raise page's mlock_count:
	 * double counting, leaving the page unevictable indefinitely.
	 * Communicate this danger to mlock_vma_folio() with VM_IO,
	 * which is a VM_SPECIAL flag not allowed on VM_LOCKED vmas.
	 * mmap_lock is held in write mode here, so this weird
	 * combination should not be visible to other mmap_lock users;
	 * but WRITE_ONCE so rmap walkers must see VM_IO if VM_LOCKED.
	 */
	/* 原注释译注：迁移/reclaim 与本 VMA 遍历竞争会重复计数；临时 VM_IO 通知 rmap，写锁隔离 mmap 用户。 */
	if (vma_flags_test(new_vma_flags, VMA_LOCKED_BIT))
		vma_flags_set(new_vma_flags, VMA_IO_BIT);
	vma_start_write(vma);
	vma_flags_reset_once(vma, new_vma_flags);

	/* 阶段：先 drain 旧 LRU add，再 walk 并入队 mlock 操作，最后 drain 使状态已提交。 */
	lru_add_drain();
	walk_page_range(vma->vm_mm, start, end, &mlock_walk_ops, NULL);
	lru_add_drain();

	if (vma_flags_test(new_vma_flags, VMA_IO_BIT)) {
		vma_flags_clear(new_vma_flags, VMA_IO_BIT);
		vma_flags_reset_once(vma, new_vma_flags);
	}
}

/*
 * mlock_fixup  - handle mlock[all]/munlock[all] requests.
 *
 * Filters out "special" vmas -- VM_LOCKED never gets set for these, and
 * munlock is a no-op.  However, for some special vmas, we go ahead and
 * populate the ptes.
 *
 * For vmas that pass the filters, merge/split as appropriate.
 */
/*
 * 原注释译注：处理 mlock[all]/munlock[all]；特殊 VMA 永不设 VM_LOCKED，munlock
 * 对其无效，部分特殊 VMA 仍可 populate；普通 VMA 按需 merge/split。
 * 业务背景：单段范围可能切过 VMA，vma_modify_flags() 负责准备 split/merge，随后
 * 本函数更新 locked_vm 并让页表与新 flags 一致。
 * 入参：vmi/prev 是 Maple VMA 迭代状态；vma 是当前借用 VMA；start/end 是子范围；
 * newflags 是目标 legacy flags。出参/返回：0 或 vma_modify_flags() errno，*prev 更新。
 * 注意事项：mmap_write_lock 必须已持有；secretmem 不允许解锁；失败不进入页遍历，
 * 但仍回写 prev 以保持迭代器的后续结构有效。
 */
static int mlock_fixup(struct vma_iterator *vmi, struct vm_area_struct *vma,
	       struct vm_area_struct **prev, unsigned long start,
	       unsigned long end, vm_flags_t newflags)
{
	vma_flags_t new_vma_flags = legacy_to_vma_flags(newflags);
	const vma_flags_t old_vma_flags = vma->flags;
	struct mm_struct *mm = vma->vm_mm;
	int nr_pages;
	int ret = 0;

	if (vma_flags_same_pair(&old_vma_flags, &new_vma_flags) ||
	    /* 相同锁位、secretmem 或不支持 mlock 的 VMA 均保持原状态，不触碰 locked_vm。 */
	    vma_is_secretmem(vma) || !vma_supports_mlock(vma)) {
		/*
		 * Don't set VM_LOCKED or VM_LOCKONFAULT and don't count.
		 * For secretmem, don't allow the memory to be unlocked.
		 */
		/* 原注释译注：不为特殊 VMA 设置锁标志或计数；secretmem 禁止被解锁。 */
		goto out;
	}

	/* 阶段 1：必要时切分/合并 VMA，并取得代表目标子范围的新 VMA。 */
	vma = vma_modify_flags(vmi, *prev, vma, start, end, &new_vma_flags);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto out;
	}

	/*
	 * Keep track of amount of locked VM.
	 */
	/* 原注释译注：维护锁定虚拟内存页数；新锁加、解锁减、重复锁保持零增量。 */
	nr_pages = (end - start) >> PAGE_SHIFT;
	if (!vma_flags_test(&new_vma_flags, VMA_LOCKED_BIT))
		nr_pages = -nr_pages;
	else if (vma_flags_test(&old_vma_flags, VMA_LOCKED_BIT))
		nr_pages = 0;
	mm->locked_vm += nr_pages;

	/*
	 * vm_flags is protected by the mmap_lock held in write mode.
	 * It's okay if try_to_unmap_one unmaps a page just after we
	 * set VM_LOCKED, populate_vma_page_range will bring it back.
	 */
	/* 原注释译注：vm_flags 由写 mmap_lock 保护；刚设锁后即使 unmap，populate 仍会补回页。 */
	if (vma_flags_test(&new_vma_flags, VMA_LOCKED_BIT) &&
	    vma_flags_test(&old_vma_flags, VMA_LOCKED_BIT)) {
		/* No work to do, and mlocking twice would be wrong */
		/* 原注释译注：没有页级工作；再次 mlock 会错误地重复计数。 */
		vma_start_write(vma);
		vma->flags = new_vma_flags;
	} else {
		mlock_vma_pages_range(vma, start, end, &new_vma_flags);
	}
out:
	*prev = vma;
	return ret;
}

/*
 * 业务背景：把一个已对齐用户范围逐 VMA 转成目标 VM_LOCKED/VM_LOCKONFAULT flags，
 * 是 mlock/munlock 的 VMA 事务入口。
 * 入参：start 为页对齐起点，len 为页对齐长度，flags 仅含 VM_LOCKED_MASK 位。
 * 出参/返回：0 或 -EINVAL 溢出、-ENOMEM 缺 VMA/洞、下层修改错误。
 * 注意事项：调用者必须持 current->mm 的写 mmap_lock；VMA_ITERATOR 与 prev 配合
 * 处理 split/merge，遇洞不可部分成功后静默继续。
 */
static int apply_vma_lock_flags(unsigned long start, size_t len,
				vm_flags_t flags)
{
	unsigned long nstart, end, tmp;
	struct vm_area_struct *vma, *prev;
	VMA_ITERATOR(vmi, current->mm, start);

	VM_BUG_ON(offset_in_page(start));
	/* 入口不变量由 syscall 包装建立：未对齐或非页整长度是内部调用错误。 */
	VM_BUG_ON(len != PAGE_ALIGN(len));
	/* 加法回绕和零长度是无需遍历 VMA 的两个早期出口。 */
	end = start + len;
	if (end < start)
		return -EINVAL;
	if (end == start)
		return 0;
	vma = vma_iter_load(&vmi);
	/* 请求起点没有任何 VMA 时，整个锁定事务失败而非创建新映射。 */
	if (!vma)
		return -ENOMEM;

	prev = vma_prev(&vmi);
	/* 起点在 VMA 中部时把当前 VMA 作为 split 的前驱提示。 */
	if (start > vma->vm_start)
		prev = vma;

	nstart = start;
	tmp = vma->vm_start;
	/* 阶段：验证范围连续，裁剪首尾 VMA 子区间，逐段提交 flags 和页级状态。 */
	for_each_vma_range(vmi, vma, end) {
		int error;
		vm_flags_t newflags;

		/* 地址洞意味着范围不能完整锁定；已处理段不在这里单独回滚。 */
		if (vma->vm_start != tmp)
			return -ENOMEM;

		newflags = vma->vm_flags & ~VM_LOCKED_MASK;
		/* 只替换两种 mlock 策略位，保留 VMA 的权限、共享、特殊等其余属性。 */
		newflags |= flags;
		/* Here we know that  vma->vm_start <= nstart < vma->vm_end. */
		/* 原注释译注：此处已知 nstart 落在当前 VMA 内，tmp 可安全裁剪到请求终点。 */
		tmp = vma->vm_end;
		/* 末 VMA 只处理到用户请求终点，范围外 flags 和页状态保持不变。 */
		if (tmp > end)
			tmp = end;
		/* fixup 可能切分/合并 VMA；成功后必须从 iterator 的真实终点继续，而非旧 vm_end。 */
		error = mlock_fixup(&vmi, vma, &prev, nstart, tmp, newflags);
		if (error)
			return error;
		tmp = vma_iter_end(&vmi);
		nstart = tmp;
	}

	/* 循环结束仍未覆盖请求终点说明尾部存在 VMA 洞。 */
	if (tmp < end)
		return -ENOMEM;

	return 0;
}

/*
 * Go through vma areas and sum size of mlocked
 * vma pages, as return value.
 * Note deferred memory locking case(mlock2(,,MLOCK_ONFAULT)
 * is also counted.
 * Return value: previously mlocked page counts
 */
/*
 * 原注释译注：遍历 VMA 累加已 mlock 页数，MLOCK_ONFAULT 的延迟锁定亦计入；返回
 * 先前锁定页数。业务背景：额度检查前扣除重叠既有锁页，避免重复 mlock 被误拒绝。
 * 入参：mm 由调用者持写 mmap_lock；start/len 是字节范围。出参/返回：页数。
 * 注意事项：地址加法溢出时终点钳到 ULONG_MAX；只统计 VM_LOCKED，结果右移为页。
 */
static unsigned long count_mm_mlocked_page_nr(struct mm_struct *mm,
		unsigned long start, size_t len)
{
	struct vm_area_struct *vma;
	unsigned long count = 0;
	unsigned long end;
	VMA_ITERATOR(vmi, mm, start);

	/* Don't overflow past ULONG_MAX */
	/* 原注释译注：不得越过 ULONG_MAX；溢出请求按最大可表示终点遍历。 */
	if (unlikely(ULONG_MAX - len < start))
		/* 饱和终点仍可遍历最后一个 VMA，避免 unsigned 加法回绕到低地址。 */
		end = ULONG_MAX;
	else
		end = start + len;

	/* 逐 VMA 交叠扣除首段偏移、裁剪末段，最终把字节总数转换为页。 */
	for_each_vma_range(vmi, vma, end) {
		if (vma->vm_flags & VM_LOCKED) {
			if (start > vma->vm_start)
				count -= (start - vma->vm_start);
			/* 终点落在当前 VMA 时加入截断段并结束，之后 VMA 不属于请求范围。 */
			if (end < vma->vm_end) {
				count += end - vma->vm_start;
				break;
			}
			count += vma->vm_end - vma->vm_start;
		}
	}

	return count >> PAGE_SHIFT;
}

/*
 * convert get_user_pages() return value to posix mlock() error
 */
/*
 * 原注释译注：把 get_user_pages() 返回值转换为 POSIX mlock errno。
 * 业务背景：mlock 的用户 ABI 将访问故障归为 ENOMEM、分配不足归为 EAGAIN；这里
 * 仅重映射这两类错误。入参：retval 是 __mm_populate() 的负 errno；出参/返回：
 * 规范化后的 errno。注意事项：其它错误原样保留，调用者不得把部分成功误当 0。
 */
static int __mlock_posix_error_return(long retval)
{
	if (retval == -EFAULT)
		retval = -ENOMEM;
	else if (retval == -ENOMEM)
		retval = -EAGAIN;
	return retval;
}

/*
 * 业务背景：mlock/mlock2 共同的事务：规范化用户范围，检查权限/额度，写锁下更新
 * VMA flags 和 locked_vm，出锁后按要求 populate 实际页。
 * 入参：start/len 为用户字节范围；flags 是 VM_LOCKED 及可选 VM_LOCKONFAULT。
 * 出参/返回：0 或权限、地址、额度、中断、VMA 修改/填页错误；无所有权转移。
 * 注意事项：mmap_write_lock_killable() 后所有 VMA 计数读写受保护；先修改 flags
 * 再 populate 是提交边界，populate 失败按 POSIX 映射错误但不回滚已设锁定策略。
 */
static __must_check int do_mlock(unsigned long start, size_t len, vm_flags_t flags)
{
	unsigned long locked;
	unsigned long lock_limit;
	int error = -ENOMEM;

	/* 阶段 1：去掉架构地址 tag，扩展到页边界，避免只锁半页而遗漏同页数据。 */
	start = untagged_addr(start);

	/* 阶段 2：先做资格门禁，再取得可中断写 mmap_lock。 */
	if (!can_do_mlock())
		return -EPERM;

	len = PAGE_ALIGN(len + (offset_in_page(start)));
	start &= PAGE_MASK;

	lock_limit = rlimit(RLIMIT_MEMLOCK);
	lock_limit >>= PAGE_SHIFT;
	locked = len >> PAGE_SHIFT;

	if (mmap_write_lock_killable(current->mm))
		return -EINTR;

	/* 阶段 3：以页为单位核算请求加现有锁页；重叠区域稍后扣回以避免双计。 */
	locked += current->mm->locked_vm;
	if ((locked > lock_limit) && (!capable(CAP_IPC_LOCK))) {
		/*
		 * It is possible that the regions requested intersect with
		 * previously mlocked areas, that part area in "mm->locked_vm"
		 * should not be counted to new mlock increment count. So check
		 * and adjust locked count if necessary.
		 */
		/* 原注释译注：请求可与既有 mlock 区重叠，需从新增额度中扣除该部分。 */
		locked -= count_mm_mlocked_page_nr(current->mm,
				start, len);
	}

	/* check against resource limits */
	/* 原注释译注：按资源上限决定是否提交 VMA flags；CAP_IPC_LOCK 可越过额度。 */
	if ((locked <= lock_limit) || capable(CAP_IPC_LOCK))
		error = apply_vma_lock_flags(start, len, flags);

	/* 阶段 4：VMA 事务结束即释放写锁；实际 fault/populate 不可在该锁下执行。 */
	mmap_write_unlock(current->mm);
	if (error)
		return error;

	/* 阶段 5：非 ONFAULT 也由此建立驻留页，失败转换为 POSIX 可见 errno。 */
	error = __mm_populate(start, len, 0);
	if (error)
		return __mlock_posix_error_return(error);
	return 0;
}

/*
 * 业务背景：mlock(2) 立即锁定并 populate 指定范围。入参：start 为用户地址，len
 * 为字节数；出参/返回：do_mlock() 的 errno。注意事项：系统调用 ABI 包装只负责
 * 参数进入，真正权限、范围、额度和页状态转换全部由 do_mlock() 完成。
 */
SYSCALL_DEFINE2(mlock, unsigned long, start, size_t, len)
{
	return do_mlock(start, len, VM_LOCKED);
}

/*
 * 业务背景：mlock2(2) 在普通锁定外允许 MLOCK_ONFAULT 延迟驻留。入参：start/len
 * 同 mlock，flags 仅允许 MLOCK_ONFAULT。出参/返回：-EINVAL 或 do_mlock() 结果。
 * 注意事项：ONFAULT 只改变 VMA 策略位，后续 fault 才锁页；未知标志必须拒绝，
 * 不能静默忽略而形成用户 ABI 误解。
 */
SYSCALL_DEFINE3(mlock2, unsigned long, start, size_t, len, int, flags)
{
	vm_flags_t vm_flags = VM_LOCKED;

	if (flags & ~MLOCK_ONFAULT)
		return -EINVAL;

	if (flags & MLOCK_ONFAULT)
		vm_flags |= VM_LOCKONFAULT;

	return do_mlock(start, len, vm_flags);
}

/*
 * 业务背景：munlock(2) 清除范围内锁定策略并驱动页级解锁。入参：start/len 是用户
 * 字节范围；出参/返回：-EINTR 或 apply_vma_lock_flags() 结果。
 * 注意事项：地址规范化/页对齐须与 mlock 对称；写 mmap_lock 覆盖整个 VMA 事务，
 * 解锁后页可回收，但 secretmem/特殊 VMA 的语义由 mlock_fixup() 保留。
 */
SYSCALL_DEFINE2(munlock, unsigned long, start, size_t, len)
{
	int ret;

	start = untagged_addr(start);

	/* 阶段：按页扩展请求，在可中断写锁中清空 VM_LOCKED_MASK 并交由 fixup 遍历。 */
	len = PAGE_ALIGN(len + (offset_in_page(start)));
	start &= PAGE_MASK;

	if (mmap_write_lock_killable(current->mm))
		return -EINTR;
	ret = apply_vma_lock_flags(start, len, 0);
	mmap_write_unlock(current->mm);

	return ret;
}

/*
 * Take the MCL_* flags passed into mlockall (or 0 if called from munlockall)
 * and translate into the appropriate modifications to mm->def_flags and/or the
 * flags for all current VMAs.
 *
 * There are a couple of subtleties with this.  If mlockall() is called multiple
 * times with different flags, the values do not necessarily stack.  If mlockall
 * is called once including the MCL_FUTURE flag and then a second time without
 * it, VM_LOCKED and VM_LOCKONFAULT will be cleared from mm->def_flags.
 */
/*
 * 原注释译注：把 mlockall 的 MCL_*（或 munlockall 的零）翻成 mm->def_flags 和
 * 当前全部 VMA 的 flags；多次调用的 flags 不叠加，后一次无 MCL_FUTURE 会清默认锁位。
 * 业务背景：MCL_FUTURE 影响未来 mmap，MCL_CURRENT 逐 VMA 修改现有映射。
 * 入参：flags 为已验证 MCL 位。出参/返回：始终 0，单 VMA 错误被忽略以符合 mlockall
 * 的尽力处理。注意事项：调用者已持写 mmap_lock；每轮 cond_resched() 不释放它。
 */
static int apply_mlockall_flags(int flags)
{
	VMA_ITERATOR(vmi, current->mm, 0);
	struct vm_area_struct *vma, *prev = NULL;
	vm_flags_t to_add = 0;

	/* 阶段 1：先清未来默认值，再按 MCL_FUTURE 重建，防止历史调用残留位叠加。 */
	current->mm->def_flags &= ~VM_LOCKED_MASK;
	if (flags & MCL_FUTURE) {
		/* 未来 VMA 继承 LOCKED；ONFAULT 仅把实际驻留推迟到首次 fault。 */
		current->mm->def_flags |= VM_LOCKED;

		if (flags & MCL_ONFAULT)
			/* ONFAULT 是 FUTURE/CURRENT 的修饰位，不单独使任何 VMA 锁定。 */
			current->mm->def_flags |= VM_LOCKONFAULT;

		if (!(flags & MCL_CURRENT))
			goto out;
	}

	if (flags & MCL_CURRENT) {
		to_add |= VM_LOCKED;
		if (flags & MCL_ONFAULT)
			to_add |= VM_LOCKONFAULT;
	}

	/* 阶段 2：MCL_CURRENT 时遍历全部 VMA；单段失败不终止全局策略更新。 */
	for_each_vma(vmi, vma) {
		int error;
		vm_flags_t newflags;

		newflags = vma->vm_flags & ~VM_LOCKED_MASK;
		newflags |= to_add;

		error = mlock_fixup(&vmi, vma, &prev, vma->vm_start, vma->vm_end,
				    newflags);
		/* Ignore errors, but prev needs fixing up. */
		/* 原注释译注：忽略单段错误但仍修正 prev，避免后续 VMA 修改使用失效前驱。 */
		if (error)
			prev = vma;
		cond_resched();
	}
out:
	return 0;
}

/*
 * 业务背景：mlockall(2) 以 MCL_CURRENT/MCL_FUTURE 批量设置当前/未来映射的锁定
 * 策略。入参：flags 为三种允许位组合；出参/返回：-EINVAL、-EPERM、-EINTR、
 * -ENOMEM 或 0。注意事项：MCL_ONFAULT 不可单独出现；CURRENT 的总 VM 额度先
 * 检查，出锁后才 mm_populate，避免长 fault 阻塞 mmap writer。
 */
SYSCALL_DEFINE1(mlockall, int, flags)
{
	unsigned long lock_limit;
	int ret;

	/* 阶段 1：验证至少选 CURRENT/FUTURE，ONFAULT 只能作为它们的修饰。 */
	if (!flags || (flags & ~(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT)) ||
	    flags == MCL_ONFAULT)
		return -EINVAL;

	if (!can_do_mlock())
		return -EPERM;

	lock_limit = rlimit(RLIMIT_MEMLOCK);
	lock_limit >>= PAGE_SHIFT;

	if (mmap_write_lock_killable(current->mm))
		return -EINTR;

	/* 阶段 2：在写锁下核查 CURRENT 总页数并提交默认/现有 VMA flags。 */
	ret = -ENOMEM;
	if (!(flags & MCL_CURRENT) || (current->mm->total_vm <= lock_limit) ||
	    capable(CAP_IPC_LOCK))
		ret = apply_mlockall_flags(flags);
	mmap_write_unlock(current->mm);
	/* 阶段 3：当前映射成功提交后才统一 populate；FUTURE-only 不触碰既有页。 */
	if (!ret && (flags & MCL_CURRENT))
		mm_populate(0, TASK_SIZE);

	return ret;
}

/*
 * 业务背景：munlockall(2) 清除当前和未来映射的全部 mlock 策略。入参：无。
 * 出参/返回：-EINTR 或 0；实际逐 VMA 解锁由 apply_mlockall_flags(0) 完成。
 * 注意事项：必须持写 mmap_lock，保证 def_flags 和遍历 VMA 同一事务可见。
 */
SYSCALL_DEFINE0(munlockall)
{
	int ret;

	if (mmap_write_lock_killable(current->mm))
		return -EINTR;
	ret = apply_mlockall_flags(0);
	mmap_write_unlock(current->mm);
	return ret;
}

/*
 * Objects with different lifetime than processes (SHM_LOCK and SHM_HUGETLB
 * shm segments) get accounted against the user_struct instead.
 */
/*
 * 原注释译注：寿命不同于进程的 SHM_LOCK/SHM_HUGETLB 段改记到 user_struct。
 * 该全局锁串行化 user namespace 的 memlock ucounts 增减和引用取得，防止额度
 * 检查与 get_ucounts()/put_ucounts() 之间出现并发透支或释放后使用。
 */
static DEFINE_SPINLOCK(shmlock_user_lock);

/*
 * 业务背景：为 SysV SHM_LOCK/HUGETLB 段预留用户级 MEMLOCK 额度；对象不随单个
 * 进程退出，故不能使用 mm->locked_vm。
 * 入参：size 为字节数，ucounts 是调用者持有的用户计数对象。出参/返回：1 表示
 * 成功增加额度并额外持有 ucounts 引用，0 表示额度/引用失败且已回滚。
 * 注意事项：size 向上取页；spinlock 覆盖增量、上限和引用获取，CAP_IPC_LOCK 可
 * 越上限但不能越过 get_ucounts 失败；成功者必须调用 user_shm_unlock() 配对。
 */
int user_shm_lock(size_t size, struct ucounts *ucounts)
{
	unsigned long lock_limit, locked;
	long memlock;
	int allowed = 0;

	/* 阶段 1：把段大小转换为页额度，在锁内先记账再检查越限。 */
	locked = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	lock_limit = rlimit(RLIMIT_MEMLOCK);
	if (lock_limit != RLIM_INFINITY)
		lock_limit >>= PAGE_SHIFT;
	spin_lock(&shmlock_user_lock);
	memlock = inc_rlimit_ucounts(ucounts, UCOUNT_RLIMIT_MEMLOCK, locked);

	/* 阶段 2：无特权越限或计数饱和时立即撤销本次增量。 */
	if ((memlock == LONG_MAX || memlock > lock_limit) && !capable(CAP_IPC_LOCK)) {
		dec_rlimit_ucounts(ucounts, UCOUNT_RLIMIT_MEMLOCK, locked);
		goto out;
	}
	/* 阶段 3：只有成功取得独立 ucounts 引用才提交；否则同样回滚额度。 */
	if (!get_ucounts(ucounts)) {
		dec_rlimit_ucounts(ucounts, UCOUNT_RLIMIT_MEMLOCK, locked);
		allowed = 0;
		goto out;
	}
	allowed = 1;
out:
	spin_unlock(&shmlock_user_lock);
	return allowed;
}

/*
 * 业务背景：撤销 user_shm_lock() 成功时的用户级 SHM 锁页账目和额外引用。
 * 入参：size/ucounts 必须与成功 lock 配对；出参/返回：无。
 * 注意事项：先在 shmlock_user_lock 下按同一向上取整公式减账，再 put_ucounts；
 * 颠倒顺序会允许并发观察到已释放对象，重复调用会导致额度下溢。
 */
void user_shm_unlock(size_t size, struct ucounts *ucounts)
{
	spin_lock(&shmlock_user_lock);
	dec_rlimit_ucounts(ucounts, UCOUNT_RLIMIT_MEMLOCK, (size + PAGE_SIZE - 1) >> PAGE_SHIFT);
	spin_unlock(&shmlock_user_lock);
	put_ucounts(ucounts);
}
