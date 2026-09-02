// SPDX-License-Identifier: GPL-2.0
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/mmdebug.h>
#include <linux/mm_types.h>
#include <linux/mm_inline.h>
#include <linux/pagemap.h>
/* 这些依赖分别提供 RCU、跨 CPU 同步、swap/rmap 与架构页表释放原语。 */
#include <linux/rcupdate.h>
#include <linux/smp.h>
#include <linux/swap.h>
#include <linux/rmap.h>
#include <linux/pgalloc.h>
#include <linux/hugetlb.h>

#include <asm/tlb.h>

/*
 * 学习提示：mmu_gather 把“撤销映射、失效 TLB、释放物理页/页表”拆成有序阶段。
 * 先让旧翻译不可再被 CPU 使用，才允许回收旧映射背后的存储。
 */
#ifndef CONFIG_MMU_GATHER_NO_GATHER

/*
 * 业务背景：数据页编码批次写满后，gather 尝试复用或无等待扩充分配，以延后昂贵 TLB flush。
 * 入参：tlb 是当前线程独占的活跃 gather；已有批次/计数由其拥有。
 * 出参/返回：成功切换到空后继批次返回 true；受 delayed-rmap/上限/ENOMEM 限制返回 false。
 * 注意事项：GFP_NOWAIT 不睡眠；false 是要求上层立即 flush 的背压，不是 teardown 失败。
 */
static bool tlb_next_batch(struct mmu_gather *tlb)
{
	/* batch 在复用路径借用当前/后继，在分配路径暂持新页直到链入 tlb。 */
	struct mmu_gather_batch *batch;

	/* 延迟 rmap 尚未兑现时限制扩批，保证稍后的遍历不会漏掉旧批次。 */
	/* Limit batching if we have delayed rmaps pending */
	if (tlb->delayed_rmap && tlb->active != &tlb->local)
		return false;

	batch = tlb->active;
	/* 已分配的后继批次优先复用，避免 teardown 热路径反复申请内存。 */
	if (batch->next) {
		tlb->active = batch->next;
		return true;
	}

	if (tlb->batch_count == MAX_GATHER_BATCH_COUNT)
		return false;

	/* GFP_NOWAIT 失败不是致命错误；false 会让上层提前 flush 以保证进展。 */
	batch = (void *)__get_free_page(GFP_NOWAIT);
	if (!batch)
		return false;

	tlb->batch_count++;
	batch->next = NULL;
	batch->nr   = 0;
	batch->max  = MAX_GATHER_BATCH;

	/* 链入后同时推进 active，后续编码条目只能写入这个尾批次。 */
	tlb->active->next = batch;
	tlb->active = batch;

	return true;
}

#ifdef CONFIG_SMP
/*
 * 业务背景：TLB 已失效后，逐项兑现一个批次中为减少原子操作而延迟的 rmap 删除。
 * 入参：batch 是稳定借用编码流；vma 是被解除映射的借用区域。
 * 出参/返回：void；仅处理 DELAY_RMAP 项并更新 folio rmap，不释放编码页或 batch。
 * 注意事项：调用前必须完成对应 TLB flush；NR_PAGES_NEXT 槽必须与前项成对解析。
 */
static void tlb_flush_rmap_batch(struct mmu_gather_batch *batch, struct vm_area_struct *vma)
{
	/* pages 是 batch 内借用编码数组；i 是槽索引，遇到页数元数据时额外递增。 */
	struct encoded_page **pages = batch->encoded_pages;

	/* 编码流由“页指针”和可选的“连续页数”组成，消费时必须同步跳过后者。 */
	for (int i = 0; i < batch->nr; i++) {
		/* enc 是当前编码值，不持有额外 page 引用。 */
		struct encoded_page *enc = pages[i];

		if (encoded_page_flags(enc) & ENCODED_PAGE_BIT_DELAY_RMAP) {
			/* page 是编码起始页；nr_pages 是同 folio 连续基础页数。 */
			struct page *page = encoded_page_ptr(enc);
			unsigned int nr_pages = 1;

			/* NR_PAGES_NEXT 表示下一槽不是页，误按页解释会破坏引用计数。 */
			if (unlikely(encoded_page_flags(enc) &
				     ENCODED_PAGE_BIT_NR_PAGES_NEXT))
				nr_pages = encoded_nr_pages(pages[++i]);

			/* TLB 已失效后再减 rmap，避免并发 CPU 仍凭旧翻译访问已解绑页。 */
			folio_remove_rmap_ptes(page_folio(page), page, nr_pages,
					       vma);
		}
	}
}

/**
 * tlb_flush_rmaps - do pending rmap removals after we have flushed the TLB
 * @tlb: the current mmu_gather
 * @vma: The memory area from which the pages are being removed.
 *
 * Note that because of how tlb_next_batch() above works, we will
 * never start multiple new batches with pending delayed rmaps, so
 * we only need to walk through the current active batch and the
 * original local one.
 */
/*
 * 业务背景：SMP unmap 在 TLB shootdown 后集中清偿整个 gather 的延迟 rmap 债务。
 * 入参：tlb 是独占 gather；vma 是原映射区域借用指针，生命周期覆盖遍历。
 * 出参/返回：void；无债务快速返回，否则删除 local/active rmap 并清 delayed_rmap 发布完成。
 * 注意事项：必须位于 TLB flush 之后；扩批规则保证只需遍历至多两个含债务批次。
 */
void tlb_flush_rmaps(struct mmu_gather *tlb, struct vm_area_struct *vma)
{
	/* delayed_rmap 是整次 gather 的快速门；清零意味着本轮债务全部结清。 */
	if (!tlb->delayed_rmap)
		return;

	tlb_flush_rmap_batch(&tlb->local, vma);
	/* 扩批规则保证最多只需检查 local 与 active 两处含延迟 rmap 的记录。 */
	if (tlb->active != &tlb->local)
		tlb_flush_rmap_batch(tlb->active, vma);
	tlb->delayed_rmap = 0;
}
#endif

/*
 * We might end up freeing a lot of pages. Reschedule on a regular
 * basis to avoid soft lockups in configurations without full
 * preemption enabled. The magic number of 512 folios seems to work.
 */
#define MAX_NR_FOLIOS_PER_FREE		512

/*
 * 业务背景：TLB/rmap 安全点之后分段释放批次中的 folio 与 swap cache，避免长时间占用 CPU。
 * 入参：batch 是调用者拥有的非空编码批次，函数原地消费 nr/游标内容。
 * 出参/返回：void；释放全部编码页引用并把 batch->nr 减到零，容器仍由 gather 拥有。
 * 注意事项：可 cond_resched；poison/init-on-free 时按真实基础页数限流，编码对不得拆开。
 */
static void __tlb_batch_free_encoded_pages(struct mmu_gather_batch *batch)
{
	/* pages 是随消费推进的编码游标；nr 为本段槽数，nr_pages 为真实页成本。 */
	struct encoded_page **pages = batch->encoded_pages;
	unsigned int nr, nr_pages;

	/* 分段释放并主动让出 CPU，防止大范围 unmap 形成不可抢占的长尾。 */
	while (batch->nr) {
		/* 普通配置按编码槽计数；清毒/初始化配置必须按真实页数限流。 */
		if (!page_poisoning_enabled_static() && !want_init_on_free()) {
			nr = min(MAX_NR_FOLIOS_PER_FREE, batch->nr);

			/*
			 * Make sure we cover page + nr_pages, and don't leave
			 * nr_pages behind when capping the number of entries.
			 */
			/* 截断点不能把页条目与紧随的 nr_pages 元数据拆开。 */
			if (unlikely(encoded_page_flags(pages[nr - 1]) &
				     ENCODED_PAGE_BIT_NR_PAGES_NEXT))
				nr++;
		} else {
			/*
			 * With page poisoning and init_on_free, the time it
			 * takes to free memory grows proportionally with the
			 * actual memory size. Therefore, limit based on the
			 * actual memory size and not the number of involved
			 * folios.
			 */
			/* 大 folio 的释放成本随实际页数增长，因此累计展开后的页数。 */
			for (nr = 0, nr_pages = 0;
			     nr < batch->nr && nr_pages < MAX_NR_FOLIOS_PER_FREE;
			     nr++) {
				if (unlikely(encoded_page_flags(pages[nr]) &
					     ENCODED_PAGE_BIT_NR_PAGES_NEXT))
					nr_pages += encoded_nr_pages(pages[++nr]);
				else
					nr_pages++;
			}
		}

		/* 此处同时处理 swap cache；调用前的 TLB/rmap 阶段必须已经满足契约。 */
		free_pages_and_swap_cache(pages, nr);
		pages += nr;
		batch->nr -= nr;

		cond_resched();
	}
}

/*
 * 业务背景：一次 gather flush 要消费 local 及后继批次的所有数据页，再恢复可复用初态。
 * 入参：tlb 独占并已越过 TLB/rmap 安全点。
 * 出参/返回：void；清空各批次页条目并把 active 重置为 local，不释放额外批次容器。
 * 注意事项：遇到首个空批次即结束，依赖批次链按顺序填充的不变量。
 */
static void tlb_batch_pages_flush(struct mmu_gather *tlb)
{
	/* batch 是按链顺序借用的当前容器，函数不释放容器。 */
	struct mmu_gather_batch *batch;

	/* 空批次意味着后续也没有待释放条目；完成后 active 回到内嵌 local。 */
	for (batch = &tlb->local; batch && batch->nr; batch = batch->next)
		__tlb_batch_free_encoded_pages(batch);
	tlb->active = &tlb->local;
}

/*
 * 业务背景：finish 最后回收 gather 为扩容申请的批次页，内嵌 local 不释放。
 * 入参：tlb 独占且所有 batch->nr 已由 pages_flush 清零。
 * 出参/返回：void；释放 local.next 整链容器并置 NULL，tlb 不再拥有额外批次。
 * 注意事项：不得在仍含编码页时调用，否则会泄漏页引用/跳过 swap cache 清理。
 */
static void tlb_batch_list_free(struct mmu_gather *tlb)
{
	/* batch/next 分别是待释放容器和预先保存的后继，避免 free 后解引用。 */
	struct mmu_gather_batch *batch, *next;

	/* 这里只释放批次容器；其中的页必须已由 tlb_batch_pages_flush() 处理。 */
	for (batch = tlb->local.next; batch; batch = next) {
		next = batch->next;
		free_pages((unsigned long)batch, 0);
	}
	tlb->local.next = NULL;
}

/*
 * 业务背景：页表拆除把待释放 folio 范围编码进当前批次，必要时把 rmap 删除推迟到 shootdown 后。
 * 入参：tlb 是有效 gather；page/nr_pages 为同一 folio 连续范围；delay_rmap 选择债务；page_size 为映射粒度。
 * 出参/返回：条目已记录；仍有容量返回 false，无法扩批返回 true 要求调用者 flush 后继续。
 * 注意事项：不在此释放页；多页编码占两槽且仅允许基础页粒度，tlb->end 必须已建立。
 */
static bool __tlb_remove_folio_pages_size(struct mmu_gather *tlb,
		/* page/nr_pages 描述同一 folio 内的连续范围。 */
		struct page *page, unsigned int nr_pages, bool delay_rmap,
		int page_size)
{
	/* flags 编码延迟 rmap/页数槽类型；batch 是当前写入目标借用指针。 */
	int flags = delay_rmap ? ENCODED_PAGE_BIT_DELAY_RMAP : 0;
	struct mmu_gather_batch *batch;

	/* 非零 end 证明调用者已建立有效 gather 范围，避免脱离 teardown 协议。 */
	VM_BUG_ON(!tlb->end);

#ifdef CONFIG_MMU_GATHER_PAGE_SIZE
	/* 同一轮批处理的粒度必须一致，多页编码当前只允许 PAGE_SIZE 粒度。 */
	VM_WARN_ON(tlb->page_size != page_size);
	VM_WARN_ON_ONCE(nr_pages != 1 && page_size != PAGE_SIZE);
	VM_WARN_ON_ONCE(page_folio(page) != page_folio(page + nr_pages - 1));
#endif

	/* delay_rmap 位随页指针编码，真正删除反向映射推迟到 TLB flush 之后。 */
	batch = tlb->active;
	/*
	 * Add the page and check if we are full. If so
	 * force a flush.
	 */
	if (likely(nr_pages == 1)) {
		batch->encoded_pages[batch->nr++] = encode_page(page, flags);
	} else {
		/* 多页范围占两个槽：起始页携带标志，下一槽携带页数。 */
		flags |= ENCODED_PAGE_BIT_NR_PAGES_NEXT;
		batch->encoded_pages[batch->nr++] = encode_page(page, flags);
		batch->encoded_pages[batch->nr++] = encode_nr_pages(nr_pages);
	}
	/*
	 * Make sure that we can always add another "page" + "nr_pages",
	 * requiring two entries instead of only a single one.
	 */
	/* 永远预留两个槽，保证下一次最大编码原子地写入。 */
	if (batch->nr >= batch->max - 1) {
		if (!tlb_next_batch(tlb))
			return true;
		/* 扩批成功后重新取得 active，旧指针不再是写入目标。 */
		batch = tlb->active;
	}
	VM_BUG_ON_PAGE(batch->nr > batch->max - 1, page);

	return false;
}

/*
 * 业务背景：普通基础页映射路径用统一 wrapper 批量记录同 folio 连续页。
 * 入参：tlb/page/nr_pages/delay_rmap 语义同内部实现，均由调用者稳定。
 * 出参/返回：true 仅表示批次背压需立即 flush，false 表示可继续收集；页已被记录。
 * 注意事项：不报告释放成功/失败，调用者不能因 false 提前释放 backing。
 */
bool __tlb_remove_folio_pages(struct mmu_gather *tlb, struct page *page,
		/* true 返回值是容量背压信号，而不是页释放失败。 */
		unsigned int nr_pages, bool delay_rmap)
{
	/* 普通 folio 路径固定以基础页粒度记录，返回 true 要求调用者立即 flush。 */
	return __tlb_remove_folio_pages_size(tlb, page, nr_pages, delay_rmap,
					     PAGE_SIZE);
}

/*
 * 业务背景：架构非默认映射粒度的单页拆除需把 page_size 纳入 gather 一致性校验。
 * 入参：tlb/page 稳定；page_size 是该映射字节粒度，nr 固定 1且不延迟 rmap。
 * 出参/返回：返回批次背压信号，页条目已记录；无 ownership 转移。
 * 注意事项：同一 gather 的 page_size 必须一致，否则 WARN 暴露架构调用错误。
 */
bool __tlb_remove_page_size(struct mmu_gather *tlb, struct page *page, int page_size)
{
	/* 架构指定粒度的单页入口不启用延迟 rmap。 */
	return __tlb_remove_folio_pages_size(tlb, page, 1, false, page_size);
}

#endif /* MMU_GATHER_NO_GATHER */

#ifdef CONFIG_MMU_GATHER_TABLE_FREE

/* 页表回收与数据页回收分批保存，因为二者需要的同步边界不同。 */
/*
 * 业务背景：页表页越过架构/RCU 同步点后，统一释放批次内所有已脱链 table 与批次容器。
 * 入参：batch ownership 交给函数，tables[0..nr) 均已不可被硬件/软件 walker 访问。
 * 出参/返回：void；逐表调用架构释放并最终 free 批次页。
 * 注意事项：本函数自身不做 TLB/RCU 同步，过早调用会造成 walker UAF。
 */
static void __tlb_remove_table_free(struct mmu_table_batch *batch)
{
	/* i 是 tables[] 零基索引，batch ownership 在循环后由 free_page 消费。 */
	int i;

	/* 批内页表都已脱链且越过同步点，随后再释放承载指针的批次页。 */
	for (i = 0; i < batch->nr; i++)
		__tlb_remove_table(batch->tables[i]);

	free_page((unsigned long)batch);
}

#ifdef CONFIG_MMU_GATHER_RCU_TABLE_FREE

/*
 * Semi RCU freeing of the page directories.
 * 学习提示：无锁软件页表遍历者以关中断充当读侧临界区，释放端必须等待它退出。
 * 硬件 TLB 同步未必广播 IPI，因此统一用 sched-RCU 延迟页表物理回收。
 *
 * This is needed by some architectures to implement software pagetable walkers.
 *
 * gup_fast() and other software pagetable walkers do a lockless page-table
 * walk and therefore needs some synchronization with the freeing of the page
 * directories. The chosen means to accomplish that is by disabling IRQs over
 * the walk.
 *
 * Architectures that use IPIs to flush TLBs will then automagically DTRT,
 * since we unlink the page, flush TLBs, free the page. Since the disabling of
 * IRQs delays the completion of the TLB flush we can never observe an already
 * freed page.
 *
 * Not all systems IPI every CPU for this purpose:
 *
 * - Some architectures have HW support for cross-CPU synchronisation of TLB
 *   flushes, so there's no IPI at all.
 *
 * - Paravirt guests can do this TLB flushing in the hypervisor, or coordinate
 *   with the hypervisor to defer flushing on preempted vCPUs.
 *
 * Such systems need to delay the freeing by some other means, this is that
 * means.
 *
 * What we do is batch the freed directory pages (tables) and RCU free them.
 * We use the sched RCU variant, as that guarantees that IRQ/preempt disabling
 * holds off grace periods.
 *
 * However, in order to batch these pages we need to allocate storage, this
 * allocation is deep inside the MM code and can thus easily fail on memory
 * pressure. To guarantee progress we fall back to single table freeing, see
 * the implementation of tlb_remove_table_one().
 *
 */
/*
 * 译注：某些架构的软件页表 walker（如 gup_fast）无锁遍历，并以关闭 IRQ 作为读侧临界区。
 * 若 TLB shootdown 通过 IPI，关 IRQ 会自然推迟 IPI 完成，使“脱链→flush→free”不会撞上旧 walker；
 * 但硬件跨 CPU 同步或由 hypervisor 协调的系统可能没有逐 CPU IPI，因此必须另行延迟物理释放。
 * 本实现把已释放目录页批量提交给 sched-RCU，因为它保证 IRQ/抢占关闭会阻止宽限期完成。
 * 深层 MM 路径申请批次存储可能在内存压力下失败，为保证进展则退化为单表同步释放路径。
 */

/*
 * 业务背景：同步回退通过向每个 CPU 递送空 IPI，等待既有 irq-disabled 软件 walker 越过临界区。
 * 入参：arg 未使用；由 smp_call_function 在远端中断上下文调用。
 * 出参/返回：void且不改内存；“回调已执行”本身就是同步事件。
 * 注意事项：不是 RCU 宽限期，不能据此宣称对象按 RCU 规则释放。
 */
static void tlb_remove_table_smp_sync(void *arg)
{
	/* 空回调的意义是让每个目标 CPU 实际接收并越过一次中断边界。 */
	/* Simply deliver the interrupt */
}

/*
 * 业务背景：无法批量 RCU 释放单表时，用同步 IPI 排空依赖 local_irq_disable 的旧 walker。
 * 入参：无。
 * 出参/返回：void；返回时调用前已在 irq-disabled 区的目标 CPU walker 已结束。
 * 注意事项：不是完整 RCU grace period；会打扰所有 CPU，仅用于保进展回退。
 */
void tlb_remove_table_sync_one(void)
{
	/*
	 * This isn't an RCU grace period and hence the page-tables cannot be
	 * assumed to be actually RCU-freed.
	 *
	 * It is however sufficient for software page-table walkers that rely on
	 * IRQ disabling.
	 */
	/* wait=1 保证返回时所有已关中断的既存遍历者都已完成。 */
	smp_call_function(tlb_remove_table_smp_sync, NULL, 1);
}

/*
 * 业务背景：sched-RCU 宽限期结束后把嵌入回调头还原为 table batch 并执行最终释放。
 * 入参：head ownership 属于先前 call_rcu 提交的 batch。
 * 出参/返回：void；消费并释放整个 batch 及其 tables。
 * 注意事项：RCU 回调上下文不可睡眠；container_of 依赖 rcu 字段布局。
 */
static void tlb_remove_table_rcu(struct rcu_head *head)
{
	/* 宽限期后由嵌入的 rcu_head 反查批次，再统一释放页表与容器。 */
	__tlb_remove_table_free(container_of(head, struct mmu_table_batch, rcu));
}

/*
 * 业务背景：正常批量页表回收异步等待 sched-RCU，避免 teardown 同步宽限期延迟。
 * 入参：batch ownership 转给 RCU 回调，调用后调用者不得访问。
 * 出参/返回：void；仅排队，最终释放在 tlb_remove_table_rcu。
 * 注意事项：此前必须先脱链并完成需要的硬件页表缓存失效。
 */
static void tlb_remove_table_free(struct mmu_table_batch *batch)
{
	/* 回调方式把等待移出当前 teardown 路径，避免同步宽限期的延迟尖峰。 */
	call_rcu(&batch->rcu, tlb_remove_table_rcu);
}

/**
 * tlb_remove_table_sync_rcu - synchronize with software page-table walkers
 *
 * Like tlb_remove_table_sync_one() but uses RCU grace period instead of IPI
 * broadcast. Use in slow paths where sleeping is acceptable.
 *
 * Software/Lockless page-table walkers use local_irq_disable(), which is also
 * an RCU read-side critical section. synchronize_rcu() waits for all such
 * sections, providing the same guarantee as tlb_remove_table_sync_one() but
 * without disrupting all CPUs with IPIs.
 *
 * Do not use for freeing memory. Use RCU callbacks instead to avoid latency
 * spikes.
 */
/*
 * 业务背景：允许睡眠的非释放慢路径需要无 IPI 地等待 irq-disabled 软件页表 walker 退出。
 * 入参：无。
 * 出参/返回：void；返回时一个完整 RCU grace period 已结束。
 * 注意事项：不得用于常规释放内存，释放应排 RCU callback 以避免同步延迟尖峰。
 */
void tlb_remove_table_sync_rcu(void)
{
	/* 仅供允许睡眠的同步路径；常规释放应使用异步 RCU 回调。 */
	synchronize_rcu();
}

#else /* !CONFIG_MMU_GATHER_RCU_TABLE_FREE */

/*
 * 业务背景：架构不要求 RCU table free 时，批次已在 TLB 同步后可立即回收。
 * 入参：batch ownership 交给函数。
 * 出参/返回：void；同步释放全部 tables 与容器。
 * 注意事项：配置契约必须保证不存在无锁软件 walker，否则立即释放会 UAF。
 */
static void tlb_remove_table_free(struct mmu_table_batch *batch)
{
	/* 未配置 RCU 页表释放时，架构已提供可直接回收的同步保证。 */
	__tlb_remove_table_free(batch);
}

#endif /* CONFIG_MMU_GATHER_RCU_TABLE_FREE */

/*
 * If we want tlb_remove_table() to imply TLB invalidates.
 */
/*
 * 业务背景：部分架构硬件 walker 还缓存页表层级，table 物理页回收前需按架构要求额外失效。
 * 入参：tlb 是当前独占 gather，包含失效范围/层级信息。
 * 出参/返回：void；需要时执行 TLB-only flush，不释放 table 或数据页。
 * 注意事项：软件 walker 的 RCU 同步仍由后续 free 路径负责，二者不可互相替代。
 */
static inline void tlb_table_invalidate(struct mmu_gather *tlb)
{
	/* 某些架构还缓存页表层级；需求由架构钩子集中声明。 */
	if (tlb_needs_table_invalidate()) {
		/*
		 * Invalidate page-table caches used by hardware walkers. Then
		 * we still need to RCU-sched wait while freeing the pages
		 * because software walkers can still be in-flight.
		 */
		/* 译注：先失效硬件 walker 的页表缓存；软件 walker 仍可能在途，所以释放时还须 sched-RCU 等待。 */
		tlb_flush_mmu_tlbonly(tlb);
	}
}

#ifdef CONFIG_PT_RECLAIM
/*
 * 业务背景：PT_RECLAIM 把 rcu_head 嵌入 ptdesc，使单表回退无需另分配 batch。
 * 入参：head 是已越过宽限期的 pt_rcu_head，ownership 来自 call_rcu。
 * 出参/返回：void；反查 ptdesc 并最终释放 table。
 * 注意事项：RCU 回调上下文不可睡眠，table 必须已脱链并失效。
 */
static inline void __tlb_remove_table_one_rcu(struct rcu_head *head)
{
	/* ptdesc 由嵌入 rcu_head 反查，回调独占其最终释放权。 */
	struct ptdesc *ptdesc;

	/* PT_RECLAIM 将回调头嵌入 ptdesc，免去单表回退时另行分配批次。 */
	ptdesc = container_of(head, struct ptdesc, pt_rcu_head);
	__tlb_remove_table(ptdesc);
}

/*
 * 业务背景：批次页分配失败时仍用内嵌回调头异步回收单个 ptdesc 以保证进展。
 * 入参：table 是已脱链 ptdesc，ownership 转给 RCU。
 * 出参/返回：void；安排回调后立即返回。
 * 注意事项：调用后不得访问 table，失效顺序由上层先完成。
 */
static inline void __tlb_remove_table_one(void *table)
{
	/* ptdesc 是 table 的类型化别名，ownership 随 call_rcu 转移。 */
	struct ptdesc *ptdesc;

	/* 单表内存紧张回退仍保持异步 RCU 生命周期。 */
	ptdesc = table;
	call_rcu(&ptdesc->pt_rcu_head, __tlb_remove_table_one_rcu);
}
#else
/*
 * 业务背景：无内嵌回调头时，单表内存压力回退同步等候 walker 后直接释放。
 * 入参：table 是已脱链对象，ownership 被消费。
 * 出参/返回：void；等待 RCU 后调用架构释放。
 * 注意事项：可睡眠且延迟较高，只在批次分配失败的保进展路径使用。
 */
static inline void __tlb_remove_table_one(void *table)
{
	/* 无内嵌回调头时只能同步等待软件遍历者，再直接释放。 */
	tlb_remove_table_sync_rcu();
	__tlb_remove_table(table);
}
#endif /* CONFIG_PT_RECLAIM */

/*
 * 业务背景：统一隐藏 PT_RECLAIM 配置差异，供 table batch 分配失败路径回收单表。
 * 入参：table ownership 转交给配置实现。
 * 出参/返回：void；返回后调用者不得访问 table。
 * 注意事项：调用前必须完成页表脱链与所需 invalidate。
 */
static void tlb_remove_table_one(void *table)
{
	/* 该包装统一批次分配失败后的保进展路径。 */
	__tlb_remove_table_one(table);
}

/*
 * 业务背景：页表批次达到容量或 gather 收口时，先失效 walker cache 再安排批次最终释放。
 * 入参：tlb 独占并可能拥有 *batch。
 * 出参/返回：void；非空 batch ownership 转给 free 路径并把 tlb->batch 清 NULL。
 * 注意事项：清指针是 ownership 交接点；空批次快速返回。
 */
static void tlb_table_flush(struct mmu_gather *tlb)
{
	/* batch 指向 tlb->batch ownership 槽，清 NULL 表示已交给释放路径。 */
	struct mmu_table_batch **batch = &tlb->batch;

	/* 顺序固定为页表缓存失效、安排释放、清空所有权指针。 */
	if (*batch) {
		tlb_table_invalidate(tlb);
		tlb_remove_table_free(*batch);
		*batch = NULL;
	}
}

/*
 * 业务背景：页表拆除路径把已脱链 table 收集成批，以摊薄 RCU/释放开销。
 * 入参：tlb 是独占 gather；table ownership 在调用时转入 batch/单表回退。
 * 出参/返回：void；成功入批或在 GFP_NOWAIT 失败时同步/异步单表回收。
 * 注意事项：调用者必须先解除父页表引用；批满会触发 invalidate 与 ownership 交接。
 */
void tlb_remove_table(struct mmu_gather *tlb, void *table)
{
	/* batch 是 ownership 槽地址，允许分配/flush 同步更新 tlb->batch。 */
	struct mmu_table_batch **batch = &tlb->batch;

	/* 批次页采用 NOWAIT；失败时同步单表回收，不能因内存压力停滞。 */
	if (*batch == NULL) {
		*batch = (struct mmu_table_batch *)__get_free_page(GFP_NOWAIT);
		if (*batch == NULL) {
			tlb_table_invalidate(tlb);
			tlb_remove_table_one(table);
			return;
		}
		(*batch)->nr = 0;
	}

	/* 达到固定容量即执行一次完整的 invalidate/free 交接。 */
	(*batch)->tables[(*batch)->nr++] = table;
	if ((*batch)->nr == MAX_TABLE_BATCH)
		tlb_table_flush(tlb);
}

/*
 * 业务背景：gather 初始化时建立“尚未拥有 table batch”的明确初态。
 * 入参：tlb 是未发布的新栈对象。
 * 出参/返回：void；把 batch 置 NULL，无分配。
 * 注意事项：已有 batch 上调用会丢失 ownership，仅可在 gather 起点使用。
 */
static inline void tlb_table_init(struct mmu_gather *tlb)
{
	/* NULL 同时表示尚无页表批次及当前 gather 不持有批次页。 */
	tlb->batch = NULL;
}

#else /* !CONFIG_MMU_GATHER_TABLE_FREE */

/*
 * 业务背景：架构无需延迟释放页表时保留统一 flush 调用点。
 * 入参：tlb 未使用；出参/返回：void、无副作用。
 * 注意事项：配置保证没有 batch ownership，不能据此提供同步。
 */
static inline void tlb_table_flush(struct mmu_gather *tlb) { }
/*
 * 业务背景：无 table batch 字段语义时保留 gather 初始化调用点。
 * 入参：tlb 未使用；出参/返回：void、无副作用。
 * 注意事项：仅配置桩，不初始化其他 gather 字段。
 */
static inline void tlb_table_init(struct mmu_gather *tlb) { }

#endif /* CONFIG_MMU_GATHER_TABLE_FREE */

/*
 * 业务背景：TLB-only 阶段完成后集中兑现页表批次和数据页批次的最终释放。
 * 入参：tlb 独占且旧翻译已不可用。
 * 出参/返回：void；清空 table/data page 债务，批次容器留到 finish。
 * 注意事项：不得在 shootdown 前调用，否则 CPU 可能访问已释放 backing。
 */
static void tlb_flush_mmu_free(struct mmu_gather *tlb)
{
	/* TLB-only 阶段由调用者先完成；这里才兑现页表与数据页的释放。 */
	tlb_table_flush(tlb);
#ifndef CONFIG_MMU_GATHER_NO_GATHER
	tlb_batch_pages_flush(tlb);
#endif
}

/*
 * 业务背景：对外 flush 原子地执行“翻译失效→对象释放”安全序列。
 * 入参：tlb 是活跃 gather，范围/批次由调用者维护。
 * 出参/返回：void；兑现当前所有页/页表债务但 gather 可继续收集。
 * 注意事项：不结束 pending 计数或释放批次容器，最终仍须 tlb_finish_mmu。
 */
void tlb_flush_mmu(struct mmu_gather *tlb)
{
	/* 核心安全顺序：先失效处理器翻译，再释放其曾指向的对象。 */
	tlb_flush_mmu_tlbonly(tlb);
	tlb_flush_mmu_free(tlb);
}

/*
 * 业务背景：所有 gather 入口共享统一字段初始化和 mm pending-flush 发布协议。
 * 入参：tlb 是调用者栈对象；mm 借用且生命周期覆盖 gather；fullmm 选择整空间语义。
 * 出参/返回：void；初始化全部批次/范围字段并递增 mm 的 TLB pending 计数。
 * 注意事项：同一 tlb 只能初始化一次，必须由 finish 配对递减并释放资源。
 */
static void __tlb_gather_mmu(struct mmu_gather *tlb, struct mm_struct *mm,
			     bool fullmm)
{
	/* mm/fullmm 确定失效语义，其余字段建立本轮 gather 的唯一初态。 */
	tlb->mm = mm;
	tlb->fullmm = fullmm;

#ifndef CONFIG_MMU_GATHER_NO_GATHER
	/* local 是免分配的首批次，额外批次通过 next 串联。 */
	tlb->need_flush_all = 0;
	tlb->local.next = NULL;
	tlb->local.nr   = 0;
	tlb->local.max  = ARRAY_SIZE(tlb->__pages);
	tlb->active     = &tlb->local;
	tlb->batch_count = 0;
#endif
	/* 所有延迟债务和页表批次都从空状态开始。 */
	tlb->delayed_rmap = 0;

	tlb_table_init(tlb);
#ifdef CONFIG_MMU_GATHER_PAGE_SIZE
	tlb->page_size = 0;
#endif
	tlb->vma_pfn = 0;

	/* reset_range 建立架构范围哨兵；pending 计数覆盖整个 gather 生命周期。 */
	tlb->fully_unshared_tables = 0;
	__tlb_reset_range(tlb);
	inc_tlb_flush_pending(tlb->mm);
}

/**
 * tlb_gather_mmu - initialize an mmu_gather structure for page-table tear-down
 * @tlb: the mmu_gather structure to initialize
 * @mm: the mm_struct of the target address space
 *
 * Called to initialize an (on-stack) mmu_gather structure for page-table
 * tear-down from @mm.
 */
/*
 * 业务背景：部分地址空间 teardown 在栈上开启一次可分批、可多次 flush 的 gather 会话。
 * 入参：tlb 是未初始化输出对象；mm 是调用期间稳定借用的目标地址空间。
 * 出参/返回：void；建立 fullmm=false 会话并发布 pending 计数，无 ownership 转移。
 * 注意事项：调用者随后设置范围/收集页，所有出口必须 tlb_finish_mmu 配对。
 */
void tlb_gather_mmu(struct mmu_gather *tlb, struct mm_struct *mm)
{
	/* 普通入口可能只拆地址空间的一段，因此 fullmm=false。 */
	__tlb_gather_mmu(tlb, mm, false);
}

/**
 * tlb_gather_mmu_fullmm - initialize an mmu_gather structure for page-table tear-down
 * @tlb: the mmu_gather structure to initialize
 * @mm: the mm_struct of the target address space
 *
 * In this case, @mm is without users and we're going to destroy the
 * full address space (exit/execve).
 *
 * Called to initialize an (on-stack) mmu_gather structure for page-table
 * tear-down from @mm.
 */
/*
 * 业务背景：exit/exec 在 mm 已无用户时可用整地址空间语义减少范围 shootdown 开销。
 * 入参：tlb 是未初始化输出对象；mm 借用且调用者保证无用户。
 * 出参/返回：void；建立 fullmm=true 会话并递增 pending。
 * 注意事项：该前置条件比普通入口强，误用于仍活动 mm 会遗漏必要的精细同步；须 finish 配对。
 */
void tlb_gather_mmu_fullmm(struct mmu_gather *tlb, struct mm_struct *mm)
{
	/* exit/exec 已无用户，可让架构选择整地址空间失效优化。 */
	__tlb_gather_mmu(tlb, mm, true);
}

/**
 * tlb_gather_mmu_vma - initialize an mmu_gather structure for operating on a
 *			single VMA
 * @tlb: the mmu_gather structure to initialize
 * @vma: the vm_area_struct
 *
 * Called to initialize an (on-stack) mmu_gather structure for operating on
 * a single VMA. In contrast to tlb_gather_mmu(), calling this function will
 * not require another call to tlb_start_vma(). In contrast to tlb_start_vma(),
 * this function will *not* call flush_cache_range().
 *
 * For hugetlb VMAs, this function will also initialize the mmu_gather
 * page_size accordingly, not requiring a separate call to
 * tlb_change_page_size().
 *
 */
/*
 * 业务背景：只操作一个 VMA 的调用者需要同时初始化 gather 的 mm、VMA flags 和 hugepage 粒度。
 * 入参：tlb 是未初始化输出对象；vma 借用且生命周期覆盖整个操作。
 * 出参/返回：void；建立普通 gather 并更新 VMA 属性，hugetlb 时设置 page_size。
 * 注意事项：不会调用 flush_cache_range，也无需再调 tlb_start_vma；最终仍须 finish。
 */
void tlb_gather_mmu_vma(struct mmu_gather *tlb, struct vm_area_struct *vma)
{
	/* 单 VMA 入口补入 VMA 属性，但刻意不替调用者刷新 cache range。 */
	tlb_gather_mmu(tlb, vma->vm_mm);
	tlb_update_vma_flags(tlb, vma);
	/* hugetlb 条目等大，可在初始化时一次性固定 gather 页粒度。 */
	if (is_vm_hugetlb_page(vma))
		/* All entries have the same size. */
		tlb_change_page_size(tlb, huge_page_size(hstate_vma(vma)));
}

/**
 * tlb_finish_mmu - finish an mmu_gather structure
 * @tlb: the mmu_gather structure to finish
 *
 * Called at the end of the shootdown operation to free up any resources that
 * were required.
 */
/*
 * 业务背景：任何 gather 会话的唯一终点，强制 flush 剩余债务、回收容器并撤销 pending 发布。
 * 入参：tlb 是已初始化且由当前 teardown 独占的会话对象。
 * 出参/返回：void；返回后无待释放页/页表/batch，mm pending 计数已递减。
 * 注意事项：并行 PTE batching 时升级 fullmm；不得重复 finish，亦不得在返回后继续收集。
 */
void tlb_finish_mmu(struct mmu_gather *tlb)
{
	/* finish 是强制收口点：任何剩余批次和 pending 标记都不能越过返回。 */
	/*
	 * We expect an earlier huge_pmd_unshare_flush() call to sort this out,
	 * due to complicated locking requirements with page table unsharing.
	 */
	/* 译注：页表 unshare 锁序复杂，预期更早的 huge_pmd_unshare_flush() 已处理 fully-unshared 状态。 */
	VM_WARN_ON_ONCE(tlb->fully_unshared_tables);

	/*
	 * If there are parallel threads are doing PTE changes on same range
	 * under non-exclusive lock (e.g., mmap_lock read-side) but defer TLB
	 * flush by batching, one thread may end up seeing inconsistent PTEs
	 * and result in having stale TLB entries.  So flush TLB forcefully
	 * if we detect parallel PTE batching threads.
	 *
	 * However, some syscalls, e.g. munmap(), may free page tables, this
	 * needs force flush everything in the given range. Otherwise this
	 * may result in having stale TLB entries for some architectures,
	 * e.g. aarch64, that could specify flush what level TLB.
	 */
	/*
	 * 译注：多个线程在非独占锁下并行改同一范围 PTE 并各自批量延迟 flush 时，可能互见不一致 PTE
	 * 并留下旧 TLB；而 munmap 等还会释放页表，某些架构又能指定 TLB 层级，因此检测到嵌套批处理
	 * 时必须强制刷新给定范围的全部层级。
	 */
	/* 并行批处理嵌套时退化为 fullmm，优先保证跨 CPU/层级的一致性。 */
	if (mm_tlb_flush_nested(tlb->mm)) {
		/*
		 * The aarch64 yields better performance with fullmm by
		 * avoiding multiple CPUs spamming TLBI messages at the
		 * same time.
		 *
		 * On x86 non-fullmm doesn't yield significant difference
		 * against fullmm.
		 */
		/* 译注：aarch64 用 fullmm 可避免多 CPU 同时发送大量 TLBI；x86 上 fullmm 与范围模式差异不显著。 */
		/* 重置范围并标记释放过页表，迫使架构执行足够强的失效。 */
		tlb->fullmm = 1;
		__tlb_reset_range(tlb);
		tlb->freed_tables = 1;
	}

	/* 最后一轮 flush 兑现所有尚未释放的数据页和页表页。 */
	tlb_flush_mmu(tlb);

#ifndef CONFIG_MMU_GATHER_NO_GATHER
	tlb_batch_list_free(tlb);
#endif
	/* 与 gather 初始化的 inc 配对，向并发观察者发布 teardown 已结束。 */
	dec_tlb_flush_pending(tlb->mm);
}
