// SPDX-License-Identifier: GPL-2.0-only
/*
 * filemap/page cache 学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5 Codex（2026-07-29）。
 *
 * 本文件实现普通文件系统共享的页缓存核心：folio 在 address_space 的
 * XArray 中插入/查找/摘除，脏数据 writeback 与错误观察，folio bit
 * 等待队列，buffered read/write、readahead、splice、mmap fault 与
 * cache stat。具体磁盘布局和 I/O 提交由 mapping->a_ops 回调实现。
 *
 * 主要链路：
 *   read -> filemap_read -> 查 i_pages -> readahead/read_folio -> copy
 *   write -> generic_file_write_iter -> buffered/direct write -> writeback
 *   mmap fault -> filemap_fault -> 查/建/锁 folio -> 建立 PTE/PMD 映射
 *   truncate/invalidate -> 锁 folio -> 从 XArray 摘除 -> 取消统计 -> put
 *
 * 核心不变量：mapping->i_pages 是 index 到 folio/exceptional entry 的
 * 权威索引；在 XArray 中的每个基本页贡献一份 page-cache 引用与 nrpages
 * 统计。folio lock 串行化内容/归属转换，i_pages lock 保护索引更新，
 * invalidate_lock 与 i_mmap_rwsem 协调 fault、truncate 和 mmap。RCU
 * 查找只保证临界区生命周期，返回后若继续使用必须取得 folio 引用。
 *
 * 方案以 folio 批处理、XArray 无锁读和 readahead 获得吞吐与可扩展性；
 * 代价是必须在引用、锁、标签、统计和错误游标之间维护严格次序，直接 I/O
 * 还要显式处理页缓存一致性。
 */
/*
 *	linux/mm/filemap.c
 *
 * Copyright (C) 1994-1999  Linus Torvalds
 */

/*
 * This file handles the generic file mmap semantics used by
 * most "normal" filesystems (but you don't /have/ to use this:
 * the NFS filesystem used to do this differently, for example)
 */
/*
 * 本文件实现多数“普通”文件系统采用的通用 mmap/filemap
 * 语义，但不是强制接口；文件系统可以像早期 NFS 那样提供不同实现。
 */
#include <linux/export.h>
#include <linux/compiler.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/kernel_stat.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/leafops.h>
#include <linux/syscalls.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/error-injection.h>
#include <linux/hash.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/folio_batch.h>
#include <linux/security.h>
#include <linux/cpuset.h>
#include <linux/hugetlb.h>
#include <linux/memcontrol.h>
#include <linux/shmem_fs.h>
#include <linux/rmap.h>
#include <linux/delayacct.h>
#include <linux/psi.h>
#include <linux/ramfs.h>
#include <linux/page_idle.h>
#include <linux/migrate.h>
#include <linux/pipe_fs_i.h>
#include <linux/splice.h>
#include <linux/rcupdate_wait.h>
#include <linux/sched/mm.h>
#include <linux/sysctl.h>
#include <linux/pgalloc.h>

#include <asm/tlbflush.h>
#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/filemap.h>

/*
 * FIXME: remove all knowledge of the buffer layer from the core VM
 */
/*
 * 长期目标是让核心 VM 不感知 buffer_head 层；当前仍为
 * try_to_free_buffers 保留依赖，说明页缓存回收与旧块缓冲抽象尚未解耦。
 */
#include <linux/buffer_head.h> /* for try_to_free_buffers */
/* 仅为释放传统 buffer_head 私有状态引入该接口。 */

#include <asm/mman.h>

#include "swap.h"

/*
 * Shared mappings implemented 30.11.1994. It's not fully working yet,
 * though.
 *
 * Shared mappings now work. 15.8.1995  Bruno.
 *
 * finished 'unifying' the page and buffer cache and SMP-threaded the
 * page-cache, 21.05.1999, Ingo Molnar <mingo@redhat.com>
 *
 * SMP-threaded pagemap-LRU 1999, Andrea Arcangeli <andrea@suse.de>
 */
/*
 * 该历史块记录共享映射从 1994 年初版、1995 年可用，到 1999
 * 年统一 page/buffer cache 并完成 SMP 页缓存与 LRU 并发化的演进。
 */

/*
 * Lock ordering:
 *
 *  ->i_mmap_rwsem		(truncate_pagecache)
 *    ->private_lock		(__free_pte->block_dirty_folio)
 *      ->swap_lock		(exclusive_swap_page, others)
 *        ->i_pages lock
 *
 *  ->i_rwsem
 *    ->invalidate_lock		(acquired by fs in truncate path)
 *      ->i_mmap_rwsem		(truncate->unmap_mapping_range)
 *
 *  ->mmap_lock
 *    ->i_mmap_rwsem
 *      ->page_table_lock or pte_lock	(various, mainly in memory.c)
 *        ->i_pages lock	(arch-dependent flush_dcache_mmap_lock)
 *
 *  ->mmap_lock
 *    ->invalidate_lock		(filemap_fault)
 *      ->lock_page		(filemap_fault, access_process_vm)
 *
 *  ->i_rwsem			(generic_perform_write)
 *    ->mmap_lock		(fault_in_readable->do_page_fault)
 *
 *  bdi->wb.list_lock
 *    sb_lock			(fs/fs-writeback.c)
 *    ->i_pages lock		(__sync_single_inode)
 *
 *  ->i_mmap_rwsem
 *    ->anon_vma.lock		(vma_merge)
 *
 *  ->anon_vma.lock
 *    ->page_table_lock or pte_lock	(anon_vma_prepare and various)
 *
 *  ->page_table_lock or pte_lock
 *    ->swap_lock		(try_to_unmap_one)
 *    ->private_lock		(try_to_unmap_one)
 *    ->i_pages lock		(try_to_unmap_one)
 *    ->lruvec->lru_lock	(follow_page_mask->mark_page_accessed)
 *    ->lruvec->lru_lock	(check_pte_range->folio_isolate_lru)
 *    ->private_lock		(folio_remove_rmap_pte->set_page_dirty)
 *    ->i_pages lock		(folio_remove_rmap_pte->set_page_dirty)
 *    bdi.wb->list_lock		(folio_remove_rmap_pte->set_page_dirty)
 *    ->inode->i_lock		(folio_remove_rmap_pte->set_page_dirty)
 *    bdi.wb->list_lock		(zap_pte_range->set_page_dirty)
 *    ->inode->i_lock		(zap_pte_range->set_page_dirty)
 *    ->private_lock		(zap_pte_range->block_dirty_folio)
 */
/*
 * 上表是跨 VM/VFS 路径必须遵守的锁序。阅读本文件时重点是：
 * invalidate_lock 位于 folio lock 之前；页表锁可继续取得 i_pages/LRU/
 * private 等锁；write 路径可能在 i_rwsem 内因 fault 取得 mmap_lock。
 * 反转任一箭头会与 truncate、fault、writeback 或 unmap 路径形成死锁。
 */

/*
 * page_cache_delete() - 在已持 i_pages lock 下把一个锁定 folio 从索引摘除。
 *
 * @mapping：folio 当前所属 address_space；@folio：已锁定且仍在 i_pages
 * 中；@shadow：替换 entry，可为 NULL 或 workingset shadow。
 * 函数按 folio order 配置 XArray store，清除 marks 和 folio->mapping，
 * 并按基本页数扣减 nrpages。不会取消 LRU/VM 统计，也不 put 引用；
 * 调用者必须在前后分别完成 unaccount 和最终 free。
 */
static void page_cache_delete(struct address_space *mapping,
				   struct folio *folio, void *shadow)
{
	XA_STATE(xas, &mapping->i_pages, folio->index);
	long nr = 1;

	mapping_set_update(&xas, mapping);

	xas_set_order(&xas, folio->index, folio_order(folio));
	nr = folio_nr_pages(folio);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	/* xas_store 是并发查找者不再看到 folio 的摘除/替换发布点。 */
	xas_store(&xas, shadow);
	xas_init_marks(&xas);

	folio->mapping = NULL;
	/* Leave folio->index set: truncation lookup relies upon it */
	/* 保留 index，truncate 后续仍借它判断原文件偏移。 */
	mapping->nrpages -= nr;
}

/*
 * filemap_unaccount_folio() - 撤销 folio 作为页缓存成员贡献的 VM 统计。
 *
 * @mapping/@folio 均为借用；folio 仍保留 mapping 指针，便于读取 flags/
 * host。调用者已锁 folio 且保证不再映射；函数不从 XArray 摘除、不释放。
 * 按基本页数扣 NR_FILE_PAGES，并区分 shmem THP、file THP 与 kernel file。
 * 若普通可写回文件仍为 dirty，告警并修正 dirty 记账，防止统计泄漏。
 */
static void filemap_unaccount_folio(struct address_space *mapping,
		struct folio *folio)
{
	long nr;

	/* 摘除 page cache 前仍有 PTE 映射通常意味着 truncate/unmap 协议破坏。 */
	VM_BUG_ON_FOLIO(folio_mapped(folio), folio);
	if (!IS_ENABLED(CONFIG_DEBUG_VM) && unlikely(folio_mapped(folio))) {
		pr_alert("BUG: Bad page cache in process %s  pfn:%05lx\n",
			 current->comm, folio_pfn(folio));
		dump_page(&folio->page, "still mapped when deleted");
		dump_stack();
		add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);

		if (mapping_exiting(mapping) && !folio_test_large(folio)) {
			int mapcount = folio_mapcount(folio);

			if (folio_ref_count(folio) >= mapcount + 2) {
				/*
				 * All vmas have already been torn down, so it's
				 * a good bet that actually the page is unmapped
				 * and we'd rather not leak it: if we're wrong,
				 * another bad page check should catch it later.
				 */
				/*
				 * 所有 VMA 已拆除时更可能只是 mapcount 残留。
				 * 若引用数足以覆盖 mapcount 与页缓存/调用者引用，就强制
				 * 修复 mapcount/ref，宁可让后续 bad-page 再抓错也不泄漏。
				 */
				atomic_set(&folio->_mapcount, -1);
				folio_ref_sub(folio, mapcount);
			}
		}
	}

	/* hugetlb folios do not participate in page cache accounting. */
	/* hugetlb 使用独立统计体系，不能重复扣普通 page-cache 计数。 */
	if (folio_test_hugetlb(folio))
		return;

	nr = folio_nr_pages(folio);

	lruvec_stat_mod_folio(folio, NR_FILE_PAGES, -nr);
	if (folio_test_swapbacked(folio)) {
		lruvec_stat_mod_folio(folio, NR_SHMEM, -nr);
		/* shmem THP 与普通 file THP 使用不同 VM 统计项。 */
		if (folio_test_pmd_mappable(folio))
			lruvec_stat_mod_folio(folio, NR_SHMEM_THPS, -nr);
	} else if (folio_test_pmd_mappable(folio)) {
		lruvec_stat_mod_folio(folio, NR_FILE_THPS, -nr);
	}
	/* kernel file 另有 node 级统计，必须与普通 FILE_PAGES 同步扣减。 */
	if (test_bit(AS_KERNEL_FILE, &folio->mapping->flags))
		mod_node_page_state(folio_pgdat(folio),
				    NR_KERNEL_FILE_PAGES, -nr);

	/*
	 * At this point folio must be either written or cleaned by
	 * truncate.  Dirty folio here signals a bug and loss of
	 * unwritten data - on ordinary filesystems.
	 *
	 * But it's harmless on in-memory filesystems like tmpfs; and can
	 * occur when a driver which did get_user_pages() sets page dirty
	 * before putting it, while the inode is being finally evicted.
	 *
	 * Below fixes dirty accounting after removing the folio entirely
	 * but leaves the dirty flag set: it has no effect for truncated
	 * folio and anyway will be cleared before returning folio to
	 * buddy allocator.
	 */
	/*
	 * 正常情况下 folio 已写回或被 truncate 清理；普通磁盘文件
	 * 在此仍 dirty 意味着未写数据丢失。tmpfs 等内存文件系统或 GUP 驱动
	 * 在 inode 驱逐竞态中可能合法留下 dirty。这里仅修正 dirty accounting，
	 * 保留 flag；截断 folio 不再提交 I/O，归还 buddy 前 flag 仍会清除。
	 */
	if (WARN_ON_ONCE(folio_test_dirty(folio) &&
			 mapping_can_writeback(mapping)))
		/* 最终只修正脏页统计；dirty flag 留给 folio 释放路径清理。 */
		folio_account_cleaned(folio, inode_to_wb(mapping->host));
}

/*
 * Delete a page from the page cache and free it. Caller has to make
 * sure the page is locked and that nobody else uses it - or that usage
 * is safe.  The caller must hold the i_pages lock.
 */
/*
 * 调用者必须锁住 folio、独占或安全协调其他使用者，并持有
 * mapping->i_pages lock。本 helper 依次 trace、取消统计、从 XArray
 * 摘除；@shadow 可保留 workingset 历史。无直接返回，也不 put folio，
 * 因此离开时调用者引用仍有效。
 */
void __filemap_remove_folio(struct folio *folio, void *shadow)
{
	struct address_space *mapping = folio->mapping;

	trace_mm_filemap_delete_from_page_cache(folio);
	filemap_unaccount_folio(mapping, folio);
	page_cache_delete(mapping, folio, shadow);
}

/*
 * filemap_free_folio() - 执行文件系统释放钩子并归还全部 page-cache 引用。
 *
 * @mapping：摘除前的稳定映射，用于 a_ops；@folio 已从 XArray 摘除。
 * free_folio 回调可释放 private/buffer 状态，之后按 folio_nr_pages put
 * 页缓存对每个基本页持有的引用。可能触发最终释放，调用后不得再解引用。
 */
static void filemap_free_folio(const struct address_space *mapping,
		struct folio *folio)
{
	void (*free_folio)(struct folio *);

	free_folio = mapping->a_ops->free_folio;
	if (free_folio)
		free_folio(folio);

	folio_put_refs(folio, folio_nr_pages(folio));
}

/**
 * filemap_remove_folio - Remove folio from page cache.
 * @folio: The folio.
 *
 * This must be called only on folios that are locked and have been
 * verified to be in the page cache.  It will never put the folio into
 * the free list because the caller has a reference on the page.
 */
/*
 * 只接受已锁定并确认仍在 page cache 的 @folio。先在 inode
 * i_lock 与 i_pages xa_lock 下完成统计和索引摘除，再按 mapping shrinkable
 * 状态把 inode 放回 LRU，出锁后调用文件系统 free hook 并放掉 cache
 * 引用。调用者自己的 folio 引用确保本函数不会把对象直接送入 free list。
 * 无返回；锁序遵循文件头 i_lock -> i_pages。
 */
void filemap_remove_folio(struct folio *folio)
{
	struct address_space *mapping = folio->mapping;

	BUG_ON(!folio_test_locked(folio));
	/* 临界区同时维护 inode LRU 条件、mapping 统计与 XArray 成员关系。 */
	spin_lock(&mapping->host->i_lock);
	xa_lock_irq(&mapping->i_pages);
	__filemap_remove_folio(folio, NULL);
	xa_unlock_irq(&mapping->i_pages);
	if (mapping_shrinkable(mapping))
		inode_lru_list_add(mapping->host);
	spin_unlock(&mapping->host->i_lock);

	/* 可能调用文件系统并最终释放，必须在自旋锁外执行。 */
	filemap_free_folio(mapping, folio);
}

/*
 * page_cache_delete_batch - delete several folios from page cache
 * @mapping: the mapping to which folios belong
 * @fbatch: batch of folios to delete
 *
 * The function walks over mapping->i_pages and removes folios passed in
 * @fbatch from the mapping. The function expects @fbatch to be sorted
 * by page index and is optimised for it to be dense.
 * It tolerates holes in @fbatch (mapping entries at those indices are not
 * modified).
 *
 * The function expects the i_pages lock to be held.
 */
/*
 * 批量从 mapping->i_pages 删除 @fbatch 中已锁 folio。batch
 * 必须按 index 排序，稠密时 XArray 游标最有效；允许索引间出现洞。
 * exceptional value 或不属于 batch 的新页被跳过。由于目标 folio 已锁，
 * 它不应被别人移除；若看到更高 index，VM_BUG 暴露锁协议破坏。
 *
 * 本函数只清 mapping、XArray entry 和 nrpages，不取消统计/put；调用者
 * 已持 i_pages lock 并在前后完成这些阶段。
 */
static void page_cache_delete_batch(struct address_space *mapping,
			     struct folio_batch *fbatch)
{
	XA_STATE(xas, &mapping->i_pages, fbatch->folios[0]->index);
	long total_pages = 0;
	int i = 0;
	/* i 对应有序 batch 槽，total_pages 累计删除的基本页数量。 */
	struct folio *folio;

	mapping_set_update(&xas, mapping);
	xas_for_each(&xas, folio, ULONG_MAX) {
		if (i >= folio_batch_count(fbatch))
			break;

		/* A swap/dax/shadow entry got inserted? Skip it. */
		/* XArray value 是 swap/dax/shadow 元数据，不是可删 folio。 */
		if (xa_is_value(folio))
			continue;
		/*
		 * A page got inserted in our range? Skip it. We have our
		 * pages locked so they are protected from being removed.
		 * If we see a page whose index is higher than ours, it
		 * means our page has been removed, which shouldn't be
		 * possible because we're holding the PageLock.
		 */
		/*
		 * 范围内可并发插入非目标页，直接跳过；目标页因 PageLock
		 * 不应消失。游标越过目标 index 表示它被非法删除，立即触发调试检查。
		 */
		if (folio != fbatch->folios[i]) {
			VM_BUG_ON_FOLIO(folio->index >
					fbatch->folios[i]->index, folio);
			continue;
		}

		WARN_ON_ONCE(!folio_test_locked(folio));

		folio->mapping = NULL;
		/* Leave folio->index set: truncation lookup relies on it */
		/* 与单页删除相同，保留 index 给 truncate 后续定位。 */

		i++;
		xas_store(&xas, NULL);
		total_pages += folio_nr_pages(folio);
	}
	mapping->nrpages -= total_pages;
}

/*
 * delete_from_page_cache_batch() - 完整执行一批 folio 的取消统计、摘除和释放。
 *
 * @mapping：所有 batch folio 的共同 address_space；@fbatch：已锁、按 index
 * 排序且调用者持有引用的批次。空批次直接返回。持 i_lock+i_pages lock
 * 逐项 trace/unaccount 后批量删索引，必要时更新 inode LRU；出锁再调用
 * free hook/put cache 引用。无直接返回，batch 中调用者引用仍归调用者。
 */
void delete_from_page_cache_batch(struct address_space *mapping,
				  struct folio_batch *fbatch)
{
	int i;

	if (!folio_batch_count(fbatch))
		return;

	spin_lock(&mapping->host->i_lock);
	xa_lock_irq(&mapping->i_pages);
	/* 阶段 1：锁内逐项撤销统计，再以一个 XArray 游标批量摘除。 */
	for (i = 0; i < folio_batch_count(fbatch); i++) {
		struct folio *folio = fbatch->folios[i];

		trace_mm_filemap_delete_from_page_cache(folio);
		filemap_unaccount_folio(mapping, folio);
	}
	page_cache_delete_batch(mapping, fbatch);
	xa_unlock_irq(&mapping->i_pages);
	if (mapping_shrinkable(mapping))
		inode_lru_list_add(mapping->host);
	spin_unlock(&mapping->host->i_lock);

	/* free hook 和 put 可能复杂/触发释放，放到所有自旋锁之外。 */
	for (i = 0; i < folio_batch_count(fbatch); i++)
		filemap_free_folio(mapping, fbatch->folios[i]);
}

/*
 * filemap_check_errors() - 消费 mapping 级旧式 writeback 错误位。
 *
 * @mapping：借用 address_space。原子 test-and-clear 使本次观察者领取错误；
 * ENOSPC 优先检查，但若 EIO 同时存在最终返回 -EIO。返回 0/-ENOSPC/-EIO。
 * 新代码通常使用 errseq/file->f_wb_err 以给每个 file 独立观察游标。
 */
int filemap_check_errors(struct address_space *mapping)
{
	int ret = 0;
	/* Check for outstanding write errors */
	/* 读取并清除尚未报告的 mapping writeback 错误。 */
	if (test_bit(AS_ENOSPC, &mapping->flags) &&
	    test_and_clear_bit(AS_ENOSPC, &mapping->flags))
		ret = -ENOSPC;
	if (test_bit(AS_EIO, &mapping->flags) &&
	    test_and_clear_bit(AS_EIO, &mapping->flags))
		ret = -EIO;
	return ret;
}
EXPORT_SYMBOL(filemap_check_errors);

/*
 * filemap_check_and_keep_errors() - 读取但不消费 mapping writeback 错误。
 * @mapping 借用；EIO 优先于 ENOSPC，返回 0 或错误。适合 sync/fsfreeze
 * 等全局刷新者，避免它们替具体 file 吞掉用户应观察的错误。
 */
static int filemap_check_and_keep_errors(struct address_space *mapping)
{
	/* Check for outstanding write errors */
	/* 只测试、不清位，后续责任主体仍可观察同一错误。 */
	if (test_bit(AS_EIO, &mapping->flags))
		return -EIO;
	if (test_bit(AS_ENOSPC, &mapping->flags))
		return -ENOSPC;
	return 0;
}

/*
 * filemap_writeback() - 以给定同步策略启动 mapping 字节范围的 writeback。
 *
 * @mapping：借用；@start/@end：闭区间字节偏移；@sync_mode：等待策略；
 * @nr_to_write：可空输入输出页预算。无写回能力或无 DIRTY tag 快速返回 0。
 * 否则把 inode 附到 writeback_control，调用 do_writepages 分派 a_ops，
 * 再无条件 detach。成功时写回剩余预算；返回 0 或文件系统 errno。
 * WB_SYNC_ALL 可能等待 I/O，WB_SYNC_NONE 主要负责提交。
 */
static int filemap_writeback(struct address_space *mapping, loff_t start,
		loff_t end, enum writeback_sync_modes sync_mode,
		long *nr_to_write)
{
	struct writeback_control wbc = {
		.sync_mode	= sync_mode,
		/* 预算为空时视为无限，范围仍保持字节闭区间。 */
		.nr_to_write	= nr_to_write ? *nr_to_write : LONG_MAX,
		.range_start	= start,
		.range_end	= end,
	};
	int ret;

	/* 阶段 1：无写回能力或没有 DIRTY tag 时完全跳过上下文建立。 */
	if (!mapping_can_writeback(mapping) ||
	    !mapping_tagged(mapping, PAGECACHE_TAG_DIRTY))
		return 0;

	/* 阶段 2：把范围和预算绑定到 inode writeback 上下文后分派文件系统。 */
	/* attach/detach 建立 writeback 记账上下文，任何 do_writepages 结果都配对。 */
	wbc_attach_fdatawrite_inode(&wbc, mapping->host);
	ret = do_writepages(mapping, &wbc);
	wbc_detach_inode(&wbc);

	if (!ret && nr_to_write)
		*nr_to_write = wbc.nr_to_write;
	return ret;
}

/**
 * filemap_fdatawrite_range - start writeback on mapping dirty pages in range
 * @mapping:	address space structure to write
 * @start:	offset in bytes where the range starts
 * @end:	offset in bytes where the range ends (inclusive)
 *
 * Start writeback against all of a mapping's dirty pages that lie
 * within the byte offsets <start, end> inclusive.
 *
 * This is a data integrity operation that waits upon dirty or in writeback
 * pages.
 *
 * Return: %0 on success, negative error code otherwise.
 */
/*
 * 对 @mapping 的闭区间字节范围启动数据完整性 writeback。
 * 使用 WB_SYNC_ALL，要求相关 dirty/writeback folio 完成后才返回；因此
 * 可睡眠。返回 0 或底层 writepages errno，不自动消费 errseq 错误。
 */
int filemap_fdatawrite_range(struct address_space *mapping, loff_t start,
		loff_t end)
{
	return filemap_writeback(mapping, start, end, WB_SYNC_ALL, NULL);
}
EXPORT_SYMBOL(filemap_fdatawrite_range);

/*
 * filemap_fdatawrite() - 对整个 mapping 执行同步数据 writeback。
 * @mapping 借用；范围 0..LLONG_MAX，返回语义同 filemap_fdatawrite_range。
 */
int filemap_fdatawrite(struct address_space *mapping)
{
	return filemap_fdatawrite_range(mapping, 0, LLONG_MAX);
}
EXPORT_SYMBOL(filemap_fdatawrite);

/**
 * filemap_flush_range - start writeback on a range
 * @mapping:	target address_space
 * @start:	index to start writeback on
 * @end:	last (inclusive) index for writeback
 *
 * This is a non-integrity writeback helper, to start writing back folios
 * for the indicated range.
 *
 * Return: %0 on success, negative error code otherwise.
 */
/*
 * 对指定闭区间启动 WB_SYNC_NONE 非完整性 writeback，只负责
 * 尽量提交，不保证所有 dirty folio 已启动或完成。适合后台推进，不能
 * 用作 fsync 数据持久化保证。返回 0 或底层 errno。
 */
int filemap_flush_range(struct address_space *mapping, loff_t start,
				  loff_t end)
{
	return filemap_writeback(mapping, start, end, WB_SYNC_NONE, NULL);
}
EXPORT_SYMBOL_GPL(filemap_flush_range);

/**
 * filemap_flush - mostly a non-blocking flush
 * @mapping:	target address_space
 *
 * This is a mostly non-blocking flush.  Not suitable for data-integrity
 * purposes - I/O may not be started against all dirty pages.
 *
 * Return: %0 on success, negative error code otherwise.
 */
/*
 * 对整个 mapping 做“尽量非阻塞”的 WB_SYNC_NONE flush。
 * I/O 可能尚未覆盖全部脏页，不适用于数据完整性；返回 helper 结果。
 */
int filemap_flush(struct address_space *mapping)
{
	return filemap_flush_range(mapping, 0, LLONG_MAX);
}
EXPORT_SYMBOL(filemap_flush);

/*
 * Start writeback on @nr_to_write pages from @mapping.  No one but the existing
 * btrfs caller should be using this.  Talk to linux-mm if you think adding a
 * new caller is a good idea.
 */
/*
 * 从 @mapping 最多推进 @nr_to_write 页的异步 writeback，预算
 * 输入输出。该窄接口目前只为既有 btrfs 调用者保留；新增调用会扩大
 * 难以维护的局部预算语义，应先与 linux-mm 协调。
 */
int filemap_flush_nr(struct address_space *mapping, long *nr_to_write)
{
	return filemap_writeback(mapping, 0, LLONG_MAX, WB_SYNC_NONE,
			nr_to_write);
}
EXPORT_SYMBOL_FOR_MODULES(filemap_flush_nr, "btrfs");

/**
 * filemap_range_has_page - check if a page exists in range.
 * @mapping:           address space within which to check
 * @start_byte:        offset in bytes where the range starts
 * @end_byte:          offset in bytes where the range ends (inclusive)
 *
 * Find at least one page in the range supplied, usually used to check if
 * direct writing in this range will trigger a writeback.
 *
 * Return: %true if at least one page exists in the specified range,
 * %false otherwise.
 */
/*
 * 把字节闭区间换算为 page index，在 RCU 下查找至少一个真实
 * folio；shadow/exceptional value 不计。@mapping 借用，不持久化返回
 * folio，只返回 bool；反向范围 false。
 *
 * 该检查有意是瞬时提示而非强一致承诺，常用于 direct write 判断近期是否
 * 有 page cache、是否值得先 writeback/invalidate。
 */
bool filemap_range_has_page(struct address_space *mapping,
			   loff_t start_byte, loff_t end_byte)
{
	struct folio *folio;
	XA_STATE(xas, &mapping->i_pages, start_byte >> PAGE_SHIFT);
	pgoff_t max = end_byte >> PAGE_SHIFT;

	if (end_byte < start_byte)
		/* 反向范围没有任何 index，避免右移负值或回绕。 */
		return false;

	rcu_read_lock();
	for (;;) {
		folio = xas_find(&xas, max);
		if (xas_retry(&xas, folio))
			continue;
		/* Shadow entries don't count */
		/* value entry 只记回收历史/特殊映射，不代表缓存数据页。 */
		if (xa_is_value(folio))
			continue;
		/*
		 * We don't need to try to pin this page; we're about to
		 * release the RCU lock anyway.  It is enough to know that
		 * there was a page here recently.
		 */
		/*
		 * 无需 folio_try_get，因为马上退出 RCU 且只保留 bool。
		 * 结果只承诺“最近看到过”，返回后 folio 可立即被 truncate/reclaim。
		 */
		break;
	}
	rcu_read_unlock();

	return folio != NULL;
}
EXPORT_SYMBOL(filemap_range_has_page);

/*
 * __filemap_fdatawait_range() - 等待范围内所有带 WRITEBACK tag 的 folio。
 *
 * @mapping：借用；@start_byte/@end_byte：闭区间字节偏移。按 folio_batch
 * 取得持有引用的批次，逐项等待 writeback bit 清除，release 批次后
 * cond_resched，直至 tag 扫描为空。无直接返回、不读取/消费错误状态；
 * 可长时间睡眠，调用者随后选择 mapping 错误位或 file errseq 语义。
 */
static void __filemap_fdatawait_range(struct address_space *mapping,
				     loff_t start_byte, loff_t end_byte)
{
	pgoff_t index = start_byte >> PAGE_SHIFT;
	pgoff_t end = end_byte >> PAGE_SHIFT;
	struct folio_batch fbatch;
	unsigned nr_folios;

	folio_batch_init(&fbatch);

	/* 批处理既降低 XArray 查找/引用开销，也在批次间提供调度点。 */
	while (index <= end) {
		unsigned i;

		nr_folios = filemap_get_folios_tag(mapping, &index, end,
				PAGECACHE_TAG_WRITEBACK, &fbatch);

		if (!nr_folios)
			break;

		/* 非空批次逐项等待，完成后统一释放查找引用。 */
		for (i = 0; i < nr_folios; i++) {
			struct folio *folio = fbatch.folios[i];

			folio_wait_writeback(folio);
		}
		folio_batch_release(&fbatch);
		/* 批次间释放引用并让出 CPU，避免大范围等待独占执行。 */
		cond_resched();
	}
}

/**
 * filemap_fdatawait_range - wait for writeback to complete
 * @mapping:		address space structure to wait for
 * @start_byte:		offset in bytes where the range starts
 * @end_byte:		offset in bytes where the range ends (inclusive)
 *
 * Walk the list of under-writeback pages of the given address space
 * in the given range and wait for all of them.  Check error status of
 * the address space and return it.
 *
 * Since the error status of the address space is cleared by this function,
 * callers are responsible for checking the return value and handling and/or
 * reporting the error.
 *
 * Return: error status of the address space.
 */
/*
 * 等待范围内 writeback 完成，然后消费 mapping 级 AS_EIO/
 * AS_ENOSPC 错误位。@mapping 借用，范围为闭区间字节。返回 0/-EIO/
 * -ENOSPC；由于错误位被清除，调用者必须负责报告或处理，不能忽略。
 */
int filemap_fdatawait_range(struct address_space *mapping, loff_t start_byte,
			    loff_t end_byte)
{
	__filemap_fdatawait_range(mapping, start_byte, end_byte);
	return filemap_check_errors(mapping);
}
EXPORT_SYMBOL(filemap_fdatawait_range);

/**
 * filemap_fdatawait_range_keep_errors - wait for writeback to complete
 * @mapping:		address space structure to wait for
 * @start_byte:		offset in bytes where the range starts
 * @end_byte:		offset in bytes where the range ends (inclusive)
 *
 * Walk the list of under-writeback pages of the given address space in the
 * given range and wait for all of them.  Unlike filemap_fdatawait_range(),
 * this function does not clear error status of the address space.
 *
 * Use this function if callers don't handle errors themselves.  Expected
 * call sites are system-wide / filesystem-wide data flushers: e.g. sync(2),
 * fsfreeze(8)
 */
/*
 * 等待范围内 writeback，但只读取、不清除 mapping 错误位。
 * 适用于 sync(2)、fsfreeze 等系统/文件系统级刷新者，它们不应替真正
 * 数据所有者消费错误。返回 0/-EIO/-ENOSPC。
 */
int filemap_fdatawait_range_keep_errors(struct address_space *mapping,
		loff_t start_byte, loff_t end_byte)
{
	__filemap_fdatawait_range(mapping, start_byte, end_byte);
	return filemap_check_and_keep_errors(mapping);
}
EXPORT_SYMBOL(filemap_fdatawait_range_keep_errors);

/**
 * file_fdatawait_range - wait for writeback to complete
 * @file:		file pointing to address space structure to wait for
 * @start_byte:		offset in bytes where the range starts
 * @end_byte:		offset in bytes where the range ends (inclusive)
 *
 * Walk the list of under-writeback pages of the address space that file
 * refers to, in the given range and wait for all of them.  Check error
 * status of the address space vs. the file->f_wb_err cursor and return it.
 *
 * Since the error status of the file is advanced by this function,
 * callers are responsible for checking the return value and handling and/or
 * reporting the error.
 *
 * Return: error status of the address space vs. the file->f_wb_err cursor.
 */
/*
 * 等待 @file 所属 mapping 的范围 writeback，然后通过
 * file_check_and_advance_wb_err() 比较并推进该 file 的 errseq 游标。
 * 每个 open file 因而能各自观察自上次检查以来的错误，不与其他 fd
 * 争抢 mapping 全局位。返回 0 或新 writeback errno；调用者必须处理。
 */
int file_fdatawait_range(struct file *file, loff_t start_byte, loff_t end_byte)
{
	struct address_space *mapping = file->f_mapping;

	__filemap_fdatawait_range(mapping, start_byte, end_byte);
	return file_check_and_advance_wb_err(file);
}
EXPORT_SYMBOL(file_fdatawait_range);

/**
 * filemap_fdatawait_keep_errors - wait for writeback without clearing errors
 * @mapping: address space structure to wait for
 *
 * Walk the list of under-writeback pages of the given address space
 * and wait for all of them.  Unlike filemap_fdatawait(), this function
 * does not clear error status of the address space.
 *
 * Use this function if callers don't handle errors themselves.  Expected
 * call sites are system-wide / filesystem-wide data flushers: e.g. sync(2),
 * fsfreeze(8)
 *
 * Return: error status of the address space.
 */
/*
 * 等待整个 mapping 的 writeback 并保留错误状态，供全局刷新
 * 路径使用。范围覆盖 0..LLONG_MAX；返回 0/-EIO/-ENOSPC。
 */
int filemap_fdatawait_keep_errors(struct address_space *mapping)
{
	__filemap_fdatawait_range(mapping, 0, LLONG_MAX);
	return filemap_check_and_keep_errors(mapping);
}
EXPORT_SYMBOL(filemap_fdatawait_keep_errors);

/* Returns true if writeback might be needed or already in progress. */
/* nrpages 非零表示 mapping 可能需要或正在 writeback；这是保守提示。 */
/*
 * mapping_needs_writeback() - 快速判断 mapping 是否含任何 page-cache 页。
 * 返回 bool，不扫描 DIRTY/WRITEBACK tag，因此 false 可排除工作，true
 * 不保证一定有脏页。无锁瞬时读取仅用于优化。
 */
static bool mapping_needs_writeback(struct address_space *mapping)
{
	return mapping->nrpages;
}

/*
 * filemap_range_has_writeback() - 查询字节范围内是否存在 writeback folio。
 *
 * @mapping 借用；范围闭区间，反向范围 false。在 RCU/XArray 中扫描，
 * 跳过 retry/value entry，看到带 folio writeback 状态即 true。
 * 返回是瞬时提示，不持有 folio 引用；并发完成/启动 I/O 可立即改变结果。
 */
bool filemap_range_has_writeback(struct address_space *mapping,
				 loff_t start_byte, loff_t end_byte)
{
	XA_STATE(xas, &mapping->i_pages, start_byte >> PAGE_SHIFT);
	pgoff_t max = end_byte >> PAGE_SHIFT;
	struct folio *folio;

	if (end_byte < start_byte)
		return false;

	rcu_read_lock();
	/* 扫描只保留 bool 结论；任何真实 folio 的脏、锁或写回状态都算命中。 */
	xas_for_each(&xas, folio, max) {
		if (xas_retry(&xas, folio))
			continue;
		if (xa_is_value(folio))
			continue;
		if (folio_test_dirty(folio) || folio_test_locked(folio) ||
				folio_test_writeback(folio))
			/* locked 也保守视为可能即将进入或完成 writeback。 */
			break;
	}
	rcu_read_unlock();
	return folio != NULL;
}
EXPORT_SYMBOL_GPL(filemap_range_has_writeback);

/**
 * filemap_write_and_wait_range - write out & wait on a file range
 * @mapping:	the address_space for the pages
 * @lstart:	offset in bytes where the range starts
 * @lend:	offset in bytes where the range ends (inclusive)
 *
 * Write out and wait upon file offsets lstart->lend, inclusive.
 *
 * Note that @lend is inclusive (describes the last byte to be written) so
 * that this function can be used to write to the very end-of-file (end = -1).
 *
 * Return: error status of the address space.
 */
/*
 * 把 mapping 的闭区间先执行同步 writeback，再等待已提交 I/O，
 * 最后消费旧式 mapping 错误位。@lend 为 inclusive，因此 -1 可表达 EOF。
 *
 * 即使 writepages 返回 ENOSPC，也可能已有部分页在飞行，仍须等待；EIO
 * 可能表示底层严重故障，避免继续等待潜在永不完成的 I/O。首个提交错误
 * 优先，若没有则返回等待后观察到的 -EIO/-ENOSPC。可睡眠。
 */
int filemap_write_and_wait_range(struct address_space *mapping,
				 loff_t lstart, loff_t lend)
{
	int err = 0, err2;

	if (lend < lstart)
		return 0;

	if (mapping_needs_writeback(mapping)) {
		err = filemap_fdatawrite_range(mapping, lstart, lend);
		/*
		 * Even if the above returned error, the pages may be
		 * written partially (e.g. -ENOSPC), so we wait for it.
		 * But the -EIO is special case, it may indicate the worst
		 * thing (e.g. bug) happened, so we avoid waiting for it.
		 */
		/*
		 * 提交失败不等于没有 I/O；ENOSPC 等部分成功仍要收拢。
		 * 唯独 -EIO 可能代表无法可靠完成的严重故障，直接跳过等待。
		 */
		if (err != -EIO)
			__filemap_fdatawait_range(mapping, lstart, lend);
	}
	err2 = filemap_check_errors(mapping);
	if (!err)
		err = err2;
	return err;
}
EXPORT_SYMBOL(filemap_write_and_wait_range);

/*
 * __filemap_set_wb_err() - 向 mapping 的 errseq 发布一次 writeback 错误。
 * @mapping 借用；@err 为负 errno。errseq_set 原子推进序列并保留错误，
 * 使每个 file 游标都能独立观察；返回序列只用于 trace。无直接返回。
 */
void __filemap_set_wb_err(struct address_space *mapping, int err)
{
	errseq_t eseq = errseq_set(&mapping->wb_err, err);

	trace_filemap_set_wb_err(mapping, eseq);
}
EXPORT_SYMBOL(__filemap_set_wb_err);

/**
 * file_check_and_advance_wb_err - report wb error (if any) that was previously
 * 				   and advance wb_err to current one
 * @file: struct file on which the error is being reported
 *
 * When userland calls fsync (or something like nfsd does the equivalent), we
 * want to report any writeback errors that occurred since the last fsync (or
 * since the file was opened if there haven't been any).
 *
 * Grab the wb_err from the mapping. If it matches what we have in the file,
 * then just quickly return 0. The file is all caught up.
 *
 * If it doesn't match, then take the mapping value, set the "seen" flag in
 * it and try to swap it into place. If it works, or another task beat us
 * to it with the new value, then update the f_wb_err and return the error
 * portion. The error at this point must be reported via proper channels
 * (a'la fsync, or NFS COMMIT operation, etc.).
 *
 * While we handle mapping->wb_err with atomic operations, the f_wb_err
 * value is protected by the f_lock since we must ensure that it reflects
 * the latest value swapped in for this file descriptor.
 *
 * Return: %0 on success, negative error code otherwise.
 */
/*
 * 比较 mapping->wb_err 与 @file->f_wb_err，报告该 file 自上次
 * 检查以来的新 writeback 错误，并把 file 游标推进到当前序列。
 *
 * 无变化走 READ_ONCE 无锁快路；有变化时持 file->f_lock 重新读取并调用
 * errseq_check_and_advance，串行化同一 file 的多个 fsync。mapping errseq
 * 自身用原子操作，file 锁只保护该 fd 游标。返回 0 或待通过 fsync/NFS
 * COMMIT 等正式渠道报告的 errno；同时清旧 AS_EIO/AS_ENOSPC 兼容位。
 */
int file_check_and_advance_wb_err(struct file *file)
{
	int err = 0;
	errseq_t old = READ_ONCE(file->f_wb_err);
	struct address_space *mapping = file->f_mapping;

	/* Locklessly handle the common case where nothing has changed */
	/* 绝大多数 fsync 没有新错误，避免争用 file->f_lock。 */
	if (errseq_check(&mapping->wb_err, old)) {
		/* Something changed, must use slow path */
		/* 慢路锁内重检，防止另一个线程已替同一 file 推进游标。 */
		spin_lock(&file->f_lock);
		old = file->f_wb_err;
		err = errseq_check_and_advance(&mapping->wb_err,
						&file->f_wb_err);
		trace_file_check_and_advance_wb_err(file, old);
		spin_unlock(&file->f_lock);
	}

	/*
	 * We're mostly using this function as a drop in replacement for
	 * filemap_check_errors. Clear AS_EIO/AS_ENOSPC to emulate the effect
	 * that the legacy code would have had on these flags.
	 */
	/*
	 * 此函数主要替代会消费旧 mapping 标志的
	 * filemap_check_errors，所以清除兼容位；真正的逐 file 可见性已由
	 * errseq 序列保存，不会因清位而丢失。
	 */
	clear_bit(AS_EIO, &mapping->flags);
	clear_bit(AS_ENOSPC, &mapping->flags);
	return err;
}
EXPORT_SYMBOL(file_check_and_advance_wb_err);

/**
 * file_write_and_wait_range - write out & wait on a file range
 * @file:	file pointing to address_space with pages
 * @lstart:	offset in bytes where the range starts
 * @lend:	offset in bytes where the range ends (inclusive)
 *
 * Write out and wait upon file offsets lstart->lend, inclusive.
 *
 * Note that @lend is inclusive (describes the last byte to be written) so
 * that this function can be used to write to the very end-of-file (end = -1).
 *
 * After writing out and waiting on the data, we check and advance the
 * f_wb_err cursor to the latest value, and return any errors detected there.
 *
 * Return: %0 on success, negative error code otherwise.
 */
/*
 * file 版本的 write-and-wait。提交/等待范围语义同 mapping
 * 版本，但最后使用 file 的 errseq 游标，让每个 fd 独立领取新错误。
 * @file 持有 mapping；@lstart/@lend 为闭区间字节。首个提交错误优先，
 * 否则返回 errseq 错误；可睡眠。
 */
int file_write_and_wait_range(struct file *file, loff_t lstart, loff_t lend)
{
	int err = 0, err2;
	struct address_space *mapping = file->f_mapping;

	if (lend < lstart)
		return 0;

	if (mapping_needs_writeback(mapping)) {
		err = filemap_fdatawrite_range(mapping, lstart, lend);
		/* See comment of filemap_write_and_wait() */
		/* 同 mapping 版本，非 EIO 的部分提交仍需等待完成。 */
		if (err != -EIO)
			__filemap_fdatawait_range(mapping, lstart, lend);
	}
	err2 = file_check_and_advance_wb_err(file);
	if (!err)
		err = err2;
	return err;
}
EXPORT_SYMBOL(file_write_and_wait_range);

/**
 * replace_page_cache_folio - replace a pagecache folio with a new one
 * @old:	folio to be replaced
 * @new:	folio to replace with
 *
 * This function replaces a folio in the pagecache with a new one.  On
 * success it acquires the pagecache reference for the new folio and
 * drops it for the old folio.  Both the old and new folios must be
 * locked.  This function does not add the new folio to the LRU, the
 * caller must do that.
 *
 * The remove + add is atomic.  This function cannot fail.
 */
/*
 * 在 page cache 中原子地以 @new 替换 @old。二者必须已锁；
 * old 在 mapping 中，new 尚无 mapping。成功为 new 取得 cache 引用并
 * 放掉 old 引用，迁移 memcg 与 NR_FILE_PAGES/NR_SHMEM 统计；XArray
 * lock 使并发查找者只看到 old 或 new，不会看到空洞。
 *
 * 函数不把 new 加 LRU，责任仍归调用者；old 的 free_folio hook 在出
 * XArray 锁后调用。无失败返回，调用后 old 可能释放，不得继续裸用。
 */
void replace_page_cache_folio(struct folio *old, struct folio *new)
{
	struct address_space *mapping = old->mapping;
	void (*free_folio)(struct folio *) = mapping->a_ops->free_folio;
	pgoff_t offset = old->index;
	XA_STATE(xas, &mapping->i_pages, offset);

	VM_BUG_ON_FOLIO(!folio_test_locked(old), old);
	VM_BUG_ON_FOLIO(!folio_test_locked(new), new);
	VM_BUG_ON_FOLIO(new->mapping, new);

	/* 阶段 1：发布前建立 new 的 page-cache 引用、mapping 与 index。 */
	folio_get(new);
	new->mapping = mapping;
	new->index = offset;

	/* memcg 归属必须随 cache 身份一起迁移，防止记账悬挂在 old。 */
	mem_cgroup_replace_folio(old, new);

	/* 阶段 2：单次 xas_store 是 old->new 的原子可见性切换。 */
	xas_lock_irq(&xas);
	xas_store(&xas, new);

	old->mapping = NULL;
	/* hugetlb pages do not participate in page cache accounting. */
	/* hugetlb 使用独立统计，普通 file/shmem 统计在同一锁域迁移。 */
	if (!folio_test_hugetlb(old))
		lruvec_stat_sub_folio(old, NR_FILE_PAGES);
	if (!folio_test_hugetlb(new))
		lruvec_stat_add_folio(new, NR_FILE_PAGES);
	if (folio_test_swapbacked(old))
		lruvec_stat_sub_folio(old, NR_SHMEM);
	if (folio_test_swapbacked(new))
		lruvec_stat_add_folio(new, NR_SHMEM);
	xas_unlock_irq(&xas);
	/* 阶段 3：可能复杂的文件系统清理和最终 put 必须在 xa 锁外。 */
	if (free_folio)
		free_folio(old);
	folio_put(old);
}
EXPORT_SYMBOL_GPL(replace_page_cache_folio);

/*
 * __filemap_add_folio() - 把已锁、未归属 folio 原子插入 mapping XArray。
 *
 * @mapping：目标 address_space；@folio：locked、非 swapbacked、order 不小于
 * mapping 最小 order 且 index 对齐；@index：基本页索引；@gfp 仅保留
 * reclaim 位供 XArray 节点分配；@shadowp 可空，返回被替换 exceptional
 * entry 的借用值。
 *
 * 先为每个基本页增加 cache 引用并设置 mapping/index；锁内检查冲突，
 * 真实 folio 冲突返回 -EEXIST，shadow/value 可被替换。若大 value entry
 * 覆盖更小 folio，逐级拆分后再 store。成功更新 nrpages/VM 统计并 trace；
 * XArray 缺内存在锁外按 gfp 重试。失败清 mapping、保留 index 并撤销
 * 全部 cache 引用。函数可因节点分配睡眠，支持错误注入。
 */
noinline int __filemap_add_folio(struct address_space *mapping,
		struct folio *folio, pgoff_t index, gfp_t gfp, void **shadowp)
{
	XA_STATE_ORDER(xas, &mapping->i_pages, index, folio_order(folio));
	bool huge;
	long nr;
	unsigned int forder = folio_order(folio);

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	/* 插入前必须满足 locked、非 swapbacked、无 mapping 的候选不变量。 */
	VM_BUG_ON_FOLIO(folio_test_swapbacked(folio), folio);
	VM_BUG_ON_FOLIO(folio_order(folio) < mapping_min_folio_order(mapping),
			folio);
	mapping_set_update(&xas, mapping);

	VM_BUG_ON_FOLIO(index & (folio_nr_pages(folio) - 1), folio);
	huge = folio_test_hugetlb(folio);
	nr = folio_nr_pages(folio);

	/*
	 * 阶段 1：只把 reclaim 能力传给 XArray；folio 自身已分配。提前加
	 * nr 个引用对应 mapping 对大 folio 中每个基本页的 cache 所有权。
	 */
	gfp &= GFP_RECLAIM_MASK;
	folio_ref_add(folio, nr);
	folio->mapping = mapping;
	folio->index = xas.xa_index;

	/* 阶段 2：锁内尝试插入，锁外按 xas_nomem 分配节点后重试。 */
	for (;;) {
		int order = -1;
		void *entry, *old = NULL;

		xas_lock_irq(&xas);
		xas_for_each_conflict(&xas, entry) {
			old = entry;
			if (!xa_is_value(entry)) {
				xas_set_err(&xas, -EEXIST);
				goto unlock;
			}
			/*
			 * If a larger entry exists,
			 * it will be the first and only entry iterated.
			 */
			/*
			 * 冲突范围中只能接受 XArray value；真实 folio 表示
			 * index 已占用。大 order value 覆盖整个范围时只会迭代一次。
			 */
			if (order == -1)
				order = xas_get_order(&xas);
		}

		if (old) {
			if (order > 0 && order > forder) {
				unsigned int split_order = max(forder,
						xas_try_split_min_order(order));

				/* How to handle large swap entries? */
				/*
				 * 大 exceptional entry 必须拆到不大于新 folio
				 * order 才能局部替换；shmem 的大 swap entry 尚无安全处理，
				 * 因此以 BUG 保证该路径不会静默破坏 swap 元数据。
				 */
				BUG_ON(shmem_mapping(mapping));

				while (order > forder) {
					xas_set_order(&xas, index, split_order);
					xas_try_split(&xas, old, order);
					if (xas_error(&xas))
						goto unlock;
					/* 本轮 split 成功后更新 order，继续向目标粒度收敛。 */
					order = split_order;
					split_order =
						max(xas_try_split_min_order(
							    split_order),
						    forder);
				}
				/* 拆分完成后重置游标，避免沿用旧节点状态。 */
				xas_reset(&xas);
			}
			if (shadowp)
				*shadowp = old;
			/* shadow 在 store 前带出，供调用者恢复 workingset 代际。 */
		}

		/* store 成功是 folio 对 RCU page-cache 查找者可见的发布点。 */
		xas_store(&xas, folio);
		if (xas_error(&xas))
			goto unlock;

		mapping->nrpages += nr;

		/* hugetlb pages do not participate in page cache accounting */
		/* 普通/THP 按基本页数记账；hugetlb 走独立体系。 */
		if (!huge) {
			lruvec_stat_mod_folio(folio, NR_FILE_PAGES, nr);
			if (folio_test_pmd_mappable(folio))
				lruvec_stat_mod_folio(folio,
						NR_FILE_THPS, nr);
		}

unlock:
		xas_unlock_irq(&xas);

		/* 节点分配失败时由 xas_nomem 释放锁并按 gfp 决定重试。 */
		if (!xas_nomem(&xas, gfp))
			break;
	}

	if (xas_error(&xas))
		goto error;

	trace_mm_filemap_add_to_page_cache(folio);
	return 0;
error:
	/*
	 * 回滚：folio 从未成功发布或插入已撤销，因此清 mapping 并放掉提前
	 * 增加的 nr 个 cache 引用；保留 index 供 truncate/诊断约定使用。
	 */
	folio->mapping = NULL;
	/* Leave folio->index set: truncation relies upon it */
	/* 失败也保留 index，调用者可沿既有清理协议识别目标偏移。 */
	folio_put_refs(folio, nr);
	return xas_error(&xas);
}
ALLOW_ERROR_INJECTION(__filemap_add_folio, ERRNO);

/*
 * filemap_add_folio() - 带 memcg/LRU/workingset 处理的公共 page-cache 插入。
 *
 * @mapping/@folio/@index/@gfp 语义同底层；folio 必须 locked 且由调用者
 * 持有。函数先完成 memcg charge，再调用 __filemap_add_folio；成功时
 * 处理 shadow refault、把 folio 加 LRU，并为 AS_KERNEL_FILE 更新节点
 * 统计。失败撤销 charge，返回 -ENOMEM/-EEXIST 等，folio ownership 不转移。
 */
int filemap_add_folio(struct address_space *mapping, struct folio *folio,
				pgoff_t index, gfp_t gfp)
{
	void *shadow = NULL;
	int ret;
	struct mem_cgroup *tmp;
	bool kernel_file = test_bit(AS_KERNEL_FILE, &mapping->flags);

	/*
	 * kernel file 页统一记到 root memcg，避免归属当前偶然触发者；普通文件
	 * 使用当前 memcg。set_active_memcg 返回旧值，charge 后必须恢复。
	 */
	if (kernel_file)
		tmp = set_active_memcg(root_mem_cgroup);
	ret = mem_cgroup_charge(folio, NULL, gfp);
	if (kernel_file)
		set_active_memcg(tmp);
	if (ret)
		return ret;

	/*
	 * 底层要求 locked folio。插入失败由本包装清锁并 uncharge；成功保持
	 * locked，让调用者在内容初始化完成后决定发布解锁时机。
	 */
	__folio_set_locked(folio);
	ret = __filemap_add_folio(mapping, folio, index, gfp, &shadow);
	if (unlikely(ret)) {
		mem_cgroup_uncharge(folio);
		__folio_clear_locked(folio);
	} else {
		/*
		 * The folio might have been evicted from cache only
		 * recently, in which case it should be activated like
		 * any other repeatedly accessed folio.
		 * The exception is folios getting rewritten; evicting other
		 * data from the working set, only to cache data that will
		 * get overwritten with something else, is a waste of memory.
		 */
		/*
		 * shadow 表示该偏移最近被回收，读 refault 应激活 folio
		 * 保护工作集；但 __GFP_WRITE 表示即将覆盖，激活它会挤出真正热数据。
		 */
		WARN_ON_ONCE(folio_test_active(folio));
		if (!(gfp & __GFP_WRITE) && shadow)
			workingset_refault(folio, shadow);
		folio_add_lru(folio);
		if (kernel_file)
			/* kernel-file 统计按 folio 基本页数记账，与插入结果一致。 */
			mod_node_page_state(folio_pgdat(folio),
					    NR_KERNEL_FILE_PAGES,
					    folio_nr_pages(folio));
	}
	return ret;
}
EXPORT_SYMBOL_GPL(filemap_add_folio);

#ifdef CONFIG_NUMA
/*
 * filemap_alloc_folio_noprof() - 按 mempolicy/cpuset 为 page cache 分配 folio。
 *
 * @gfp/@order：分配约束与 folio 阶；@policy 可空，非空时显式按策略分配。
 * 无 policy 且 cpuset 开启 spread 时轮转允许节点，并用 mems cookie 检测
 * 并发 cpuset 变化。返回持有引用 folio 或 NULL；可按 gfp 回收/睡眠。
 */
struct folio *filemap_alloc_folio_noprof(gfp_t gfp, unsigned int order,
		struct mempolicy *policy)
{
	int n;
	struct folio *folio;

	if (policy)
		return folio_alloc_mpol_noprof(gfp, order, policy,
				NO_INTERLEAVE_INDEX, numa_node_id());

	/* spread 只在 cpuset 策略要求时覆盖默认 NUMA placement。 */
	if (cpuset_do_page_mem_spread()) {
		unsigned int cpuset_mems_cookie;
		do {
			cpuset_mems_cookie = read_mems_allowed_begin();
			n = cpuset_mem_spread_node();
			folio = __folio_alloc_node_noprof(gfp, order, n);
		/*
		 * 仅分配失败且 mems_allowed 在期间改变时重试，既避免旧节点集合，
		 * 又避免稳定 OOM 条件下无界循环。
		 */
		} while (!folio && read_mems_allowed_retry(cpuset_mems_cookie));

		return folio;
	}
	return folio_alloc_noprof(gfp, order);
}
EXPORT_SYMBOL(filemap_alloc_folio_noprof);
#endif

/*
 * filemap_invalidate_lock_two - lock invalidate_lock for two mappings
 *
 * Lock exclusively invalidate_lock of any passed mapping that is not NULL.
 *
 * @mapping1: the first mapping to lock
 * @mapping2: the second mapping to lock
 */
/*
 * 以写模式锁住最多两个 mapping 的 invalidate_lock。NULL 忽略，
 * 相同对象只锁一次；按指针地址排序建立全局锁序，防止跨文件操作以相反
 * 参数次序形成 ABBA。第二把用 nested subclass 告知 lockdep。可睡眠。
 */
void filemap_invalidate_lock_two(struct address_space *mapping1,
				 struct address_space *mapping2)
{
	if (mapping1 > mapping2)
		swap(mapping1, mapping2);
	if (mapping1)
		down_write(&mapping1->invalidate_lock);
	if (mapping2 && mapping1 != mapping2)
		down_write_nested(&mapping2->invalidate_lock, 1);
}
EXPORT_SYMBOL(filemap_invalidate_lock_two);

/*
 * filemap_invalidate_unlock_two - unlock invalidate_lock for two mappings
 *
 * Unlock exclusive invalidate_lock of any passed mapping that is not NULL.
 *
 * @mapping1: the first mapping to unlock
 * @mapping2: the second mapping to unlock
 */
/*
 * 释放 lock_two 取得的写锁。NULL/相同指针仍只处理一次；
 * 调用者必须传入同一对象对。rwsem 解锁无需依赖反序。
 */
void filemap_invalidate_unlock_two(struct address_space *mapping1,
				   struct address_space *mapping2)
{
	if (mapping1)
		up_write(&mapping1->invalidate_lock);
	if (mapping2 && mapping1 != mapping2)
		up_write(&mapping2->invalidate_lock);
}
EXPORT_SYMBOL(filemap_invalidate_unlock_two);

/*
 * In order to wait for pages to become available there must be
 * waitqueues associated with pages. By using a hash table of
 * waitqueues where the bucket discipline is to maintain all
 * waiters on the same queue and wake all when any of the pages
 * become available, and for the woken contexts to check to be
 * sure the appropriate page became available, this saves space
 * at a cost of "thundering herd" phenomena during rare hash
 * collisions.
 */
/*
 * 为每个 folio 内嵌 waitqueue 会显著增大对象，因此按地址散列
 * 到 256 个共享队列。wait_page_key 唤醒时再筛真正 folio/bit；节省空间，
 * 代价是罕见哈希碰撞惊群。PG_waiters 是“可能有本 folio waiter”的提示。
 */
#define PAGE_WAIT_TABLE_BITS 8
#define PAGE_WAIT_TABLE_SIZE (1 << PAGE_WAIT_TABLE_BITS)
static wait_queue_head_t folio_wait_table[PAGE_WAIT_TABLE_SIZE] __cacheline_aligned;

/*
 * folio_waitqueue() - 由 folio 地址稳定选择共享等待队列。
 * @folio 仅用于哈希；返回永久静态表中的借用 queue，不取得 folio 引用。
 */
static wait_queue_head_t *folio_waitqueue(struct folio *folio)
{
	return &folio_wait_table[hash_ptr(folio, PAGE_WAIT_TABLE_BITS)];
}

/* How many times do we accept lock stealing from under a waiter? */
/*
 * 允许新来者在排队 waiter 前抢到 page lock 的次数；耗尽后
 * waiter 请求公平 handoff。值越小越公平，可能牺牲吞吐。
 */
static int sysctl_page_lock_unfairness = 5;
/* vm.page_lock_unfairness 的 sysctl 描述；最小值 0。 */
static const struct ctl_table filemap_sysctl_table[] = {
	{
		.procname	= "page_lock_unfairness",
		.data		= &sysctl_page_lock_unfairness,
		.maxlen		= sizeof(sysctl_page_lock_unfairness),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
	}
};

/*
 * pagecache_init() - 初始化 folio 等待表、writeback 子系统和 VM sysctl。
 * 启动期逐 bucket 初始化锁/链表，随后注册 writeback 与可调公平阈值。
 * 入参、直接返回和失败回滚：无。
 */
void __init pagecache_init(void)
{
	int i;

	for (i = 0; i < PAGE_WAIT_TABLE_SIZE; i++)
		init_waitqueue_head(&folio_wait_table[i]);

	page_writeback_init();
	register_sysctl_init("vm", filemap_sysctl_table);
}

/*
 * The page wait code treats the "wait->flags" somewhat unusually, because
 * we have multiple different kinds of waits, not just the usual "exclusive"
 * one.
 *
 * We have:
 *
 *  (a) no special bits set:
 *
 *	We're just waiting for the bit to be released, and when a waker
 *	calls the wakeup function, we set WQ_FLAG_WOKEN and wake it up,
 *	and remove it from the wait queue.
 *
 *	Simple and straightforward.
 *
 *  (b) WQ_FLAG_EXCLUSIVE:
 *
 *	The waiter is waiting to get the lock, and only one waiter should
 *	be woken up to avoid any thundering herd behavior. We'll set the
 *	WQ_FLAG_WOKEN bit, wake it up, and remove it from the wait queue.
 *
 *	This is the traditional exclusive wait.
 *
 *  (c) WQ_FLAG_EXCLUSIVE | WQ_FLAG_CUSTOM:
 *
 *	The waiter is waiting to get the bit, and additionally wants the
 *	lock to be transferred to it for fair lock behavior. If the lock
 *	cannot be taken, we stop walking the wait queue without waking
 *	the waiter.
 *
 *	This is the "fair lock handoff" case, and in addition to setting
 *	WQ_FLAG_WOKEN, we set WQ_FLAG_DONE to let the waiter easily see
 *	that it now has the lock.
 */
/*
 * wait->flags 在这里承载三种协议。无特殊位只等 bit 清；
 * EXCLUSIVE 只唤醒一个锁竞争者；EXCLUSIVE|CUSTOM 要求 waker 先替 waiter
 * 原子取得 bit，成功以 DONE 表示公平交接。这样同一哈希队列可服务
 * PG_locked、PG_writeback 等多类等待，又能抑制锁竞争惊群。
 *
 * wake_page_function() - 过滤目标 folio/bit，并完成唤醒或锁交接。
 * @wait：嵌入 wait_page_queue 的项；@mode 为 task state；@sync 未使用；
 * @arg 为 wait_page_key。返回 0 继续扫描，1 表示唤醒 exclusive 后停止，
 * -1 表示 exclusive 暂不可取得也停止。调用时持 q->lock。
 */
static int wake_page_function(wait_queue_entry_t *wait, unsigned mode, int sync, void *arg)
{
	unsigned int flags;
	struct wait_page_key *key = arg;
	struct wait_page_queue *wait_page
		= container_of(wait, struct wait_page_queue, wait);

	if (!wake_page_match(wait_page, key))
		return 0;

	/*
	 * If it's a lock handoff wait, we get the bit for it, and
	 * stop walking (and do not wake it up) if we can't.
	 */
	/*
	 * exclusive 只有目标 bit 已清才可前进；CUSTOM 再
	 * test_and_set 替它领取 bit。失败停止扫描，避免越过队首破坏公平。
	 */
	flags = wait->flags;
	if (flags & WQ_FLAG_EXCLUSIVE) {
		if (test_bit(key->bit_nr, &key->folio->flags.f))
			return -1;
		if (flags & WQ_FLAG_CUSTOM) {
			if (test_and_set_bit(key->bit_nr, &key->folio->flags.f))
				return -1;
			flags |= WQ_FLAG_DONE;
		}
	}

	/*
	 * We are holding the wait-queue lock, but the waiter that
	 * is waiting for this will be checking the flags without
	 * any locking.
	 *
	 * So update the flags atomically, and wake up the waiter
	 * afterwards to avoid any races. This store-release pairs
	 * with the load-acquire in folio_wait_bit_common().
	 */
	/*
	 * waiter 无锁读 flags，release store 先发布 DONE/WOKEN，
	 * 再唤醒 task；与等待侧 acquire load 配对，防止“已醒但状态仍旧”。
	 */
	smp_store_release(&wait->flags, flags | WQ_FLAG_WOKEN);
	wake_up_state(wait->private, mode);

	/*
	 * Ok, we have successfully done what we're waiting for,
	 * and we can unconditionally remove the wait entry.
	 *
	 * Note that this pairs with the "finish_wait()" in the
	 * waiter, and has to be the absolute last thing we do.
	 * After this list_del_init(&wait->entry) the wait entry
	 * might be de-allocated and the process might even have
	 * exited.
	 */
	/*
	 * list_del_init_careful 必须是绝对最后一步；摘链与
	 * finish_wait 配对，此后栈上 wait 可能随任务退出立即失效。
	 */
	list_del_init_careful(&wait->entry);
	return (flags & WQ_FLAG_EXCLUSIVE) != 0;
}

/*
 * folio_wake_bit() - 唤醒等待指定 folio flag 的任务。
 * @folio 由清 bit 路径保证存活；@bit_nr 为目标 PG_*。在哈希 queue 锁下
 * 按 key 唤醒，并在确认无本 folio 匹配者时清 PG_waiters。无返回。
 */
static void folio_wake_bit(struct folio *folio, int bit_nr)
{
	wait_queue_head_t *q = folio_waitqueue(folio);
	struct wait_page_key key;
	unsigned long flags;

	key.folio = folio;
	key.bit_nr = bit_nr;
	key.page_match = 0;

	spin_lock_irqsave(&q->lock, flags);
	__wake_up_locked_key(q, TASK_NORMAL, &key);

	/*
	 * It's possible to miss clearing waiters here, when we woke our page
	 * waiters, but the hashed waitqueue has waiters for other pages on it.
	 * That's okay, it's a rare case. The next waker will clear it.
	 *
	 * Note that, depending on the page pool (buddy, hugetlb, ZONE_DEVICE,
	 * other), the flag may be cleared in the course of freeing the page;
	 * but that is not required for correctness.
	 */
	/*
	 * 共享 bucket 可能仍有其他 folio waiter，导致无法清本 folio
	 * 提示位；保留假阳性只多走慢路，下次 waker 会清。page pool 释放时
	 * 也可能顺便清位，但正确性不依赖该行为。
	 */
	if (!waitqueue_active(q) || !key.page_match)
		folio_clear_waiters(folio);

	spin_unlock_irqrestore(&q->lock, flags);
}

/*
 * A choice of three behaviors for folio_wait_bit_common():
 */
/* 公共等待状态机按引用与 bit ownership 需求选择三种模式。 */
enum behavior {
	EXCLUSIVE,	/* Hold ref to page and take the bit when woken, like
			 * __folio_lock() waiting on then setting PG_locked.
			 */
	/* 持有 folio 引用，醒来者原子取得目标位，典型为领取 PG_locked。 */
	SHARED,		/* Hold ref to page and check the bit when woken, like
			 * folio_wait_writeback() waiting on PG_writeback.
			 */
	/* 持有引用但只复查目标位，典型为等待 PG_writeback 被清除。 */
	DROP,		/* Drop ref to page before wait, no check when woken,
			 * like folio_put_wait_locked() on PG_locked.
			 */
	/* 睡前释放 folio 引用，醒来后不再访问对象，避免延长其生命周期。 */
};
/*
 * EXCLUSIVE：等待期间持引用，成功返回时已取得 bit。
 * SHARED：持引用，只等 bit 被清，不取得它。
 * DROP：睡眠前消费 folio 引用，醒后不得再检查/解引用 folio。
 */

/*
 * Attempt to check (or get) the folio flag, and mark us done
 * if successful.
 */
/*
 * 在 q->lock 下最后观察/取得 bit。exclusive 用 test_and_set
 * 原子领取，shared 只要求已清；同步成功设置 WOKEN|DONE，避免真正睡眠。
 */
static inline bool folio_trylock_flag(struct folio *folio, int bit_nr,
					struct wait_queue_entry *wait)
{
	if (wait->flags & WQ_FLAG_EXCLUSIVE) {
		if (test_and_set_bit(bit_nr, &folio->flags.f))
			return false;
	/* 非独占者只观察 bit；两种模式成功时都设置 DONE/WOKEN。 */
	} else if (test_bit(bit_nr, &folio->flags.f))
		return false;

	wait->flags |= WQ_FLAG_WOKEN | WQ_FLAG_DONE;
	return true;
}

/*
 * folio_wait_bit_common() - folio flag 等待、独占获取与放引用等待的统一状态机。
 *
 * @folio：EXCLUSIVE/SHARED 由调用者持引用；DROP 的引用在入睡前消费。
 * @bit_nr：目标 flag；@state：睡眠/信号策略；@behavior：上述模式。
 *
 * 在哈希 q 锁下先设置 PG_waiters、最后重检 bit，再同步完成或入队；通过
 * release/acquire flags 与 waker 通信。exclusive 多次被抢后启用公平
 * handoff。返回 0 或 -EINTR；DROP 返回后绝不可再解引用 folio。
 */
static inline int folio_wait_bit_common(struct folio *folio, int bit_nr,
		int state, enum behavior behavior)
{
	wait_queue_head_t *q = folio_waitqueue(folio);
	int unfairness = sysctl_page_lock_unfairness;
	struct wait_page_queue wait_page;
	wait_queue_entry_t *wait = &wait_page.wait;
	bool thrashing = false;
	/* pflags/in_thrashing 仅在 thrashing=true 时有效，并在出口成对恢复。 */
	unsigned long pflags;
	bool in_thrashing;

	/*
	 * 等待非 uptodate workingset folio 的锁通常表示 refault 抖动；
	 * 进入 delayacct/PSI memstall，所有出口严格配对退出。
	 */
	if (bit_nr == PG_locked &&
	    !folio_test_uptodate(folio) && folio_test_workingset(folio)) {
		delayacct_thrashing_start(&in_thrashing);
		psi_memstall_enter(&pflags);
		thrashing = true;
	}

	/* wait_page 关联目标 folio/bit，供 wake_page_function 精确匹配。 */
	init_wait(wait);
	wait->func = wake_page_function;
	wait_page.folio = folio;
	wait_page.bit_nr = bit_nr;

repeat:
	/* 重试会消耗 unfairness 预算，耗尽后添加 CUSTOM 请求直接交接。 */
	wait->flags = 0;
	if (behavior == EXCLUSIVE) {
		wait->flags = WQ_FLAG_EXCLUSIVE;
		if (--unfairness < 0)
			wait->flags |= WQ_FLAG_CUSTOM;
	}

	/*
	 * Do one last check whether we can get the
	 * page bit synchronously.
	 *
	 * Do the folio_set_waiters() marking before that
	 * to let any waker we _just_ missed know they
	 * need to wake us up (otherwise they'll never
	 * even go to the slow case that looks at the
	 * page queue), and add ourselves to the wait
	 * queue if we need to sleep.
	 *
	 * This part needs to be done under the queue
	 * lock to avoid races.
	 */
	/*
	 * 先置 waiters 再重检，关闭“bit 刚清而 waker 未看到 waiter
	 * 提示所以不唤醒”的窗口；置位、重检、入队同处 q 锁临界区。
	 */
	spin_lock_irq(&q->lock);
	/* 阶段 2：在同一 waitqueue 锁下设置 waiters 位并条件入队，避免丢唤醒。 */
	folio_set_waiters(folio);
	if (!folio_trylock_flag(folio, bit_nr, wait))
		__add_wait_queue_entry_tail(q, wait);
	spin_unlock_irq(&q->lock);

	/*
	 * From now on, all the logic will be based on
	 * the WQ_FLAG_WOKEN and WQ_FLAG_DONE flag, to
	 * see whether the page bit testing has already
	 * been done by the wake function.
	 *
	 * We can drop our reference to the folio.
	 */
	/*
	 * 之后只依赖栈上 flags；DROP 现在消费引用，waker 仍可按
	 * 地址 key 完成协议，但 waiter 不再读取 folio 内容。
	 */
	if (behavior == DROP)
		folio_put(folio);

	/*
	 * Note that until the "finish_wait()", or until
	 * we see the WQ_FLAG_WOKEN flag, we need to
	 * be very careful with the 'wait->flags', because
	 * we may race with a waker that sets them.
	 */
	/*
	 * finish_wait() 或观察到 WQ_FLAG_WOKEN 前，waker 可能并发
	 * 修改 wait->flags；读写必须遵循等待队列协议，不能把 flags 当本地状态。
	 */
	for (;;) {
		unsigned int flags;

		set_current_state(state);

		/* Loop until we've been woken or interrupted */
		/* acquire 与 waker release 配对；未醒则检查信号并 I/O 睡眠。 */
		flags = smp_load_acquire(&wait->flags);
		if (!(flags & WQ_FLAG_WOKEN)) {
			if (signal_pending_state(state, current))
				break;

			io_schedule();
			continue;
		}

		/* If we were non-exclusive, we're done */
		/* shared/drop 看到 WOKEN 即完成，不要求拥有目标 bit。 */
		if (behavior != EXCLUSIVE)
			break;

		/* If the waker got the lock for us, we're done */
		/* CUSTOM 的 DONE 证明 bit 已由 waker 代为设置。 */
		if (flags & WQ_FLAG_DONE)
			break;

		/*
		 * Otherwise, if we're getting the lock, we need to
		 * try to get it ourselves.
		 *
		 * And if that fails, we'll have to retry this all.
		 */
		/*
		 * 普通 exclusive 醒后自己 test_and_set；若又被新来者
		 * 抢走，重新排队并最终转为公平 handoff。
		 */
		if (unlikely(test_and_set_bit(bit_nr, folio_flags(folio, 0))))
			goto repeat;

		wait->flags |= WQ_FLAG_DONE;
		break;
	/* 退出等待循环后统一撤销队列关系，并结束可能开启的 workingset 抖动统计。 */
	}

	/*
	 * If a signal happened, this 'finish_wait()' may remove the last
	 * waiter from the wait-queues, but the folio waiters bit will remain
	 * set. That's ok. The next wakeup will take care of it, and trying
	 * to do it here would be difficult and prone to races.
	 */
	/*
	 * 信号退出可能留下 PG_waiters 假阳性。此处主动清位难以
	 * 排除并发 waker；留给下次 wake 只影响性能，不影响正确性。
	 */
	finish_wait(q, wait);

	if (thrashing) {
		delayacct_thrashing_end(&in_thrashing);
		psi_memstall_leave(&pflags);
	}

	/*
	 * NOTE! The wait->flags weren't stable until we've done the
	 * 'finish_wait()', and we could have exited the loop above due
	 * to a signal, and had a wakeup event happen after the signal
	 * test but before the 'finish_wait()'.
	 *
	 * So only after the finish_wait() can we reliably determine
	 * if we got woken up or not, so we can now figure out the final
	 * return value based on that state without races.
	 *
	 * Also note that WQ_FLAG_WOKEN is sufficient for a non-exclusive
	 * waiter, but an exclusive one requires WQ_FLAG_DONE.
	 */
	/*
	 * finish_wait 前 flags 仍可能被并发 waker 改写；摘链后才
	 * 能可靠定案。非独占 WOKEN 足够，独占必须 DONE 才证明取得 bit。
	 */
	if (behavior == EXCLUSIVE)
		return wait->flags & WQ_FLAG_DONE ? 0 : -EINTR;

	return wait->flags & WQ_FLAG_WOKEN ? 0 : -EINTR;
}

#ifdef CONFIG_MIGRATION
/**
 * softleaf_entry_wait_on_locked - Wait for a migration entry or
 * device_private entry to be removed.
 * @entry: migration or device_private swap entry.
 * @ptl: already locked ptl. This function will drop the lock.
 *
 * Wait for a migration entry referencing the given page, or device_private
 * entry referencing a dvice_private page to be unlocked. This is
 * equivalent to folio_put_wait_locked(folio, TASK_UNINTERRUPTIBLE) except
 * this can be called without taking a reference on the page. Instead this
 * should be called while holding the ptl for @entry referencing
 * the page.
 *
 * Returns after unlocking the ptl.
 *
 * This follows the same logic as folio_wait_bit_common() so see the comments
 * there.
 */
/*
 * 等待 migration/device-private entry 指向 folio 的 PG_locked
 * 清除，语义类似 DROP 模式，但调用者没有 folio 引用，而是已持 @ptl。
 *
 * migration 路径在 entry 存在期间持 folio 引用，删除 entry 必须取得同一
 * ptl；因此本函数可在 ptl 下安全建立 waiter，再释放 ptl，之后只依赖
 * wait flags。@entry 为 softleaf 编码，@ptl 进入时已锁、返回时必已解锁。
 * TASK_UNINTERRUPTIBLE 等待，无 errno 返回；CONFIG_MIGRATION 专用。
 */
void softleaf_entry_wait_on_locked(softleaf_t entry, spinlock_t *ptl)
	__releases(ptl)
{
	struct wait_page_queue wait_page;
	wait_queue_entry_t *wait = &wait_page.wait;
	bool thrashing = false;
	/* pflags/in_thrashing 仅在 thrashing=true 时有效，并在出口成对恢复。 */
	unsigned long pflags;
	bool in_thrashing;
	wait_queue_head_t *q;
	struct folio *folio = softleaf_to_folio(entry);

	q = folio_waitqueue(folio);
	/* 阶段 1：入队前记录 workingset 抖动状态，等待后再成对结束统计。 */
	if (!folio_test_uptodate(folio) && folio_test_workingset(folio)) {
		delayacct_thrashing_start(&in_thrashing);
		psi_memstall_enter(&pflags);
		thrashing = true;
	}

	init_wait(wait);
	wait->func = wake_page_function;
	wait_page.folio = folio;
	wait_page.bit_nr = PG_locked;
	wait->flags = 0;

	/* 阶段 2：队列锁内发布 waiters 位并尝试抢锁，防止检查与入队间丢唤醒。 */
	spin_lock_irq(&q->lock);
	folio_set_waiters(folio);
	if (!folio_trylock_flag(folio, PG_locked, wait))
		__add_wait_queue_entry_tail(q, wait);
	spin_unlock_irq(&q->lock);

	/*
	 * If a migration entry exists for the page the migration path must hold
	 * a valid reference to the page, and it must take the ptl to remove the
	 * migration entry. So the page is valid until the ptl is dropped.
	 * Similarly any path attempting to drop the last reference to a
	 * device-private page needs to grab the ptl to remove the device-private
	 * entry.
	 */
	/*
	 * entry 存在时迁移/device-private 路径负责持有效引用，并
	 * 必须先取 ptl 才能删 entry/放末引用。waiter 已入队后释放 ptl，
	 * 对方才可完成状态转换并唤醒，关闭无引用裸指针的 UAF 窗口。
	 */
	spin_unlock(ptl);

	for (;;) {
		unsigned int flags;

		set_current_state(TASK_UNINTERRUPTIBLE);

		/* Loop until we've been woken or interrupted */
		/* 不可中断状态通常不接受信号，循环结构与公共协议保持一致。 */
		flags = smp_load_acquire(&wait->flags);
		if (!(flags & WQ_FLAG_WOKEN)) {
			if (signal_pending_state(TASK_UNINTERRUPTIBLE, current))
				break;

			io_schedule();
			continue;
		}
		break;
	}

	/* waiter 已完成或被唤醒，finish_wait 负责从队列安全摘除。 */
	finish_wait(q, wait);

	if (thrashing) {
		delayacct_thrashing_end(&in_thrashing);
		psi_memstall_leave(&pflags);
	}
}
#endif

/*
 * folio_wait_bit() - 不可中断地等待 folio 指定位清除。
 * @folio：调用者持引用；@bit_nr 为 PG_*。SHARED 模式不取得 bit，
 * 无直接返回，可能长期 I/O 睡眠。
 */
void folio_wait_bit(struct folio *folio, int bit_nr)
{
	folio_wait_bit_common(folio, bit_nr, TASK_UNINTERRUPTIBLE, SHARED);
}
EXPORT_SYMBOL(folio_wait_bit);

/*
 * folio_wait_bit_killable() - 可被致命信号中断地等待指定位清除。
 * 持有 folio 引用，返回 0 或 -EINTR；SHARED 模式不转移 bit ownership。
 */
int folio_wait_bit_killable(struct folio *folio, int bit_nr)
{
	return folio_wait_bit_common(folio, bit_nr, TASK_KILLABLE, SHARED);
}
EXPORT_SYMBOL(folio_wait_bit_killable);

/**
 * folio_put_wait_locked - Drop a reference and wait for it to be unlocked
 * @folio: The folio to wait for.
 * @state: The sleep state (TASK_KILLABLE, TASK_UNINTERRUPTIBLE, etc).
 *
 * The caller should hold a reference on @folio.  They expect the page to
 * become unlocked relatively soon, but do not wish to hold up migration
 * (for example) by holding the reference while waiting for the folio to
 * come unlocked.  After this function returns, the caller should not
 * dereference @folio.
 *
 * Return: 0 if the folio was unlocked or -EINTR if interrupted by a signal.
 */
/*
 * 调用者持有 @folio 引用，但不想等待期间阻碍 migration；
 * DROP 模式在睡前消费该引用，等 PG_locked 清或信号。@state 决定中断
 * 策略。返回 0/-EINTR；无论结果如何，返回后都不得再解引用 folio。
 */
static int folio_put_wait_locked(struct folio *folio, int state)
{
	return folio_wait_bit_common(folio, PG_locked, state, DROP);
}

/**
 * folio_unlock - Unlock a locked folio.
 * @folio: The folio.
 *
 * Unlocks the folio and wakes up any thread sleeping on the page lock.
 *
 * Context: May be called from interrupt or process context.  May not be
 * called from NMI context.
 */
/*
 * 清除锁定 folio 的 PG_locked，并在 PG_waiters 提示存在时唤醒
 * 哈希队列。@folio 必须已锁；无返回。xor helper 原子清 bit 并同时测试
 * waiters，避免无 waiter 快路触碰 queue。可在 IRQ/进程上下文，不可 NMI。
 */
void folio_unlock(struct folio *folio)
{
	/* Bit 7 allows x86 to check the byte's sign bit */
	/*
	 * x86 优化依赖 PG_waiters 位于低字节 bit 7、PG_locked 也在
	 * 低字节；编译期断言防止 flags 布局变化静默破坏原子快路。
	 */
	BUILD_BUG_ON(PG_waiters != 7);
	BUILD_BUG_ON(PG_locked > 7);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	if (folio_xor_flags_has_waiters(folio, 1 << PG_locked))
		folio_wake_bit(folio, PG_locked);
}
EXPORT_SYMBOL(folio_unlock);

/**
 * folio_end_read - End read on a folio.
 * @folio: The folio.
 * @success: True if all reads completed successfully.
 *
 * When all reads against a folio have completed, filesystems should
 * call this function to let the pagecache know that no more reads
 * are outstanding.  This will unlock the folio and wake up any thread
 * sleeping on the lock.  The folio will also be marked uptodate if all
 * reads succeeded.
 *
 * Context: May be called from interrupt or process context.  May not be
 * called from NMI context.
 */
/*
 * 文件系统完成 @folio 的全部读取后调用。@success 为 true 时
 * 原子设置 PG_uptodate 并清 PG_locked；失败只清锁。若有 waiter，唤醒
 * 等锁线程。folio 必须 locked，成功前不得已 uptodate；无返回，可 IRQ。
 * 该原子状态发布保证 waiter 获锁后不会看到“已解锁但 uptodate 尚未设”。
 */
void folio_end_read(struct folio *folio, bool success)
{
	unsigned long mask = 1 << PG_locked;

	/* Must be in bottom byte for x86 to work */
	/* x86 合并 flags 操作要求 PG_uptodate 也位于低字节。 */
	BUILD_BUG_ON(PG_uptodate > 7);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(success && folio_test_uptodate(folio), folio);

	if (likely(success))
		mask |= 1 << PG_uptodate;
	if (folio_xor_flags_has_waiters(folio, mask))
		folio_wake_bit(folio, PG_locked);
}
EXPORT_SYMBOL(folio_end_read);

/**
 * folio_end_private_2 - Clear PG_private_2 and wake any waiters.
 * @folio: The folio.
 *
 * Clear the PG_private_2 bit on a folio and wake up any sleepers waiting for
 * it.  The folio reference held for PG_private_2 being set is released.
 *
 * This is, for example, used when a netfs folio is being written to a local
 * disk cache, thereby allowing writes to the cache for the same folio to be
 * serialised.
 */
/*
 * 清 PG_private_2，唤醒等待者，并放掉“设置该 bit 时额外持有”
 * 的 folio 引用。常用于 netfs 把 folio 写入本地缓存时串行化同 folio
 * 的 cache write。clear_bit_unlock 提供 release 语义，唤醒后 waiter 可见
 * 此前写入；调用后 folio 可能释放。
 */
void folio_end_private_2(struct folio *folio)
{
	VM_BUG_ON_FOLIO(!folio_test_private_2(folio), folio);
	clear_bit_unlock(PG_private_2, folio_flags(folio, 0));
	folio_wake_bit(folio, PG_private_2);
	folio_put(folio);
}
EXPORT_SYMBOL(folio_end_private_2);

/**
 * folio_wait_private_2 - Wait for PG_private_2 to be cleared on a folio.
 * @folio: The folio to wait on.
 *
 * Wait for PG_private_2 to be cleared on a folio.
 */
/*
 * 循环测试并共享等待 PG_private_2 清除。循环是必要的，因为
 * 哈希队列唤醒可能来自碰撞或 bit 被重新设置；@folio 由调用者持引用。
 * 无返回、不可中断。
 */
void folio_wait_private_2(struct folio *folio)
{
	while (folio_test_private_2(folio))
		folio_wait_bit(folio, PG_private_2);
}
EXPORT_SYMBOL(folio_wait_private_2);

/**
 * folio_wait_private_2_killable - Wait for PG_private_2 to be cleared on a folio.
 * @folio: The folio to wait on.
 *
 * Wait for PG_private_2 to be cleared on a folio or until a fatal signal is
 * received by the calling task.
 *
 * Return:
 * - 0 if successful.
 * - -EINTR if a fatal signal was encountered.
 */
/*
 * killable 版本循环等待 PG_private_2 清除，致命信号时返回
 * -EINTR；成功 0。每次醒来重检以容忍哈希碰撞/重新置位；folio 引用
 * 始终由调用者持有。
 */
int folio_wait_private_2_killable(struct folio *folio)
{
	int ret = 0;

	while (folio_test_private_2(folio)) {
		ret = folio_wait_bit_killable(folio, PG_private_2);
		if (ret < 0)
			break;
	}

	return ret;
}
EXPORT_SYMBOL(folio_wait_private_2_killable);

/*
 * filemap_end_dropbehind() - 在持 folio lock 时尝试兑现 DONTCACHE 回收意图。
 * @folio：locked；若仍 dirty/writeback，保留 dropbehind 等以后重试。
 * 否则原子领取并清标志，有 mapping 时 unmap/invalidate 全 folio。
 * 无返回；失败不强制回收，数据正确性优先于缓存提示。
 */
static void filemap_end_dropbehind(struct folio *folio)
{
	struct address_space *mapping = folio->mapping;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	if (folio_test_writeback(folio) || folio_test_dirty(folio))
		return;
	/* 只有仍带 dropbehind 标志的 clean folio 才执行尽力失效。 */
	if (!folio_test_clear_dropbehind(folio))
		return;
	if (mapping)
		folio_unmap_invalidate(mapping, folio, 0);
}

/*
 * If folio was marked as dropbehind, then pages should be dropped when writeback
 * completes. Do that now. If we fail, it's likely because of a big folio -
 * just reset dropbehind for that case and latter completions should invalidate.
 */
/*
 * 若 folio 标记 dropbehind，writeback 完成后应尽量从 cache
 * 丢弃。只能在进程上下文 trylock：中断上下文不做可能涉及 unmap 的工作，
 * 锁竞争也留给后续 completion。大 folio invalidate 失败时标志处理允许
 * 后续机会再试。无返回，属于性能/缓存语义，不影响已写数据正确性。
 */
void folio_end_dropbehind(struct folio *folio)
{
	if (!folio_test_dropbehind(folio))
		return;

	/*
	 * Hitting !in_task() should not happen off RWF_DONTCACHE writeback,
	 * but can happen if normal writeback just happens to find dirty folios
	 * that were created as part of uncached writeback, and that writeback
	 * would otherwise not need non-IRQ handling. Just skip the
	 * invalidation in that case.
	 */
	/*
	 * RWF_DONTCACHE 通常在 task 上下文完成，但普通 writeback
	 * 可能碰巧处理其脏 folio 并在 IRQ 完成；此时跳过 invalidate，避免把
	 * 原本无需进程上下文的完成路径强行升级。
	 */
	if (in_task() && folio_trylock(folio)) {
		filemap_end_dropbehind(folio);
		folio_unlock(folio);
	}
}
EXPORT_SYMBOL_GPL(folio_end_dropbehind);

/**
 * folio_end_writeback_no_dropbehind - End writeback against a folio.
 * @folio: The folio.
 *
 * The folio must actually be under writeback.
 * This call is intended for filesystems that need to defer dropbehind.
 *
 * Context: May be called from process or interrupt context.
 */
/*
 * 结束 PG_writeback 但暂不执行 dropbehind，供需要延后失效的
 * 文件系统使用。@folio 必须正在 writeback。reclaim 标志若存在则清除并
 * 把 folio 旋转到可回收位置；__folio_end_writeback 更新记账并清 bit，
 * 有 waiter 时唤醒。最后结算 reclaim writeback 统计。可 IRQ，无返回。
 */
void folio_end_writeback_no_dropbehind(struct folio *folio)
{
	VM_BUG_ON_FOLIO(!folio_test_writeback(folio), folio);

	/*
	 * folio_test_clear_reclaim() could be used here but it is an
	 * atomic operation and overkill in this particular case. Failing
	 * to shuffle a folio marked for immediate reclaim is too mild
	 * a gain to justify taking an atomic operation penalty at the
	 * end of every folio writeback.
	 */
	/*
	 * 这里普通读+清已足够；即便与竞争者错过一次立即 reclaim
	 * 旋转，收益也不足以让所有 writeback completion 支付原子操作成本。
	 */
	if (folio_test_reclaim(folio)) {
		folio_clear_reclaim(folio);
		folio_rotate_reclaimable(folio);
	}

	if (__folio_end_writeback(folio))
		folio_wake_bit(folio, PG_writeback);

	acct_reclaim_writeback(folio);
}
EXPORT_SYMBOL_GPL(folio_end_writeback_no_dropbehind);

/**
 * folio_end_writeback - End writeback against a folio.
 * @folio: The folio.
 *
 * The folio must actually be under writeback.
 *
 * Context: May be called from process or interrupt context.
 */
/*
 * 标准 writeback 完成入口，在清 PG_writeback 后继续兑现
 * dropbehind。writeback 自身不持 folio 引用，只靠 truncate 等待 bit；
 * 因此先临时 folio_get，防止清 bit/唤醒后最后引用释放并复用对象，再
 * 调 dropbehind，最后 put。可 IRQ/进程，无返回。
 */
void folio_end_writeback(struct folio *folio)
{
	VM_BUG_ON_FOLIO(!folio_test_writeback(folio), folio);

	/*
	 * Writeback does not hold a folio reference of its own, relying
	 * on truncation to wait for the clearing of PG_writeback.
	 * But here we must make sure that the folio is not freed and
	 * reused before the folio_wake_bit().
	 */
	/*
	 * 清 writeback 会让 truncate waiter 继续并可能放末引用，
	 * 临时引用覆盖 wake/dropbehind 这段窗口，防止 UAF/复用。
	 */
	folio_get(folio);
	folio_end_writeback_no_dropbehind(folio);
	folio_end_dropbehind(folio);
	folio_put(folio);
}
EXPORT_SYMBOL(folio_end_writeback);

/**
 * __folio_lock - Get a lock on the folio, assuming we need to sleep to get it.
 * @folio: The folio to lock
 */
/*
 * folio_lock 快路失败后的不可中断慢路。调用者持引用；
 * EXCLUSIVE 等待成功返回时已原子取得 PG_locked。无失败返回、可睡眠。
 */
void __folio_lock(struct folio *folio)
{
	folio_wait_bit_common(folio, PG_locked, TASK_UNINTERRUPTIBLE,
				EXCLUSIVE);
}
EXPORT_SYMBOL(__folio_lock);

/*
 * __folio_lock_killable() - 可被致命信号中断的 folio 独占锁慢路。
 * 返回 0 时持有 PG_locked，-EINTR 时未持锁；调用者引用不变。
 */
int __folio_lock_killable(struct folio *folio)
{
	return folio_wait_bit_common(folio, PG_locked, TASK_KILLABLE,
					EXCLUSIVE);
}
EXPORT_SYMBOL_GPL(__folio_lock_killable);

/*
 * __folio_lock_async() - 为异步 fault 排队 folio lock waiter。
 *
 * @folio：调用者持引用；@wait：调用者准备的 wait_page_queue，其回调/
 * private/flags 已配置。q 锁内先入队并置 waiters，再 trylock；若同步成功，
 * 仍在锁内可安全摘队并返回 0；否则回调尚未触发，返回 -EIOCBQUEUED，
 * ownership 留给异步完成路径。不会睡眠。
 */
static int __folio_lock_async(struct folio *folio, struct wait_page_queue *wait)
{
	struct wait_queue_head *q = folio_waitqueue(folio);
	int ret;

	wait->folio = folio;
	wait->bit_nr = PG_locked;

	spin_lock_irq(&q->lock);
	__add_wait_queue_entry_tail(q, &wait->wait);
	folio_set_waiters(folio);
	ret = !folio_trylock(folio);
	/*
	 * If we were successful now, we know we're still on the
	 * waitqueue as we're still under the lock. This means it's
	 * safe to remove and return success, we know the callback
	 * isn't going to trigger.
	 */
	/*
	 * q 锁排除并发 wake callback；同步得锁时可确定 entry 尚在，
	 * 摘除后返回。排队成功则保持 entry，后续 wake 负责完成。
	 */
	if (!ret)
		__remove_wait_queue(q, &wait->wait);
	else
		ret = -EIOCBQUEUED;
	spin_unlock_irq(&q->lock);
	return ret;
}

/*
 * Return values:
 * 0 - folio is locked.
 * non-zero - folio is not locked.
 *     mmap_lock or per-VMA lock has been released (mmap_read_unlock() or
 *     vma_end_read()), unless flags had both FAULT_FLAG_ALLOW_RETRY and
 *     FAULT_FLAG_RETRY_NOWAIT set, in which case the lock is still held.
 *
 * If neither ALLOW_RETRY nor KILLABLE are set, will always return 0
 * with the folio locked and the mmap_lock/per-VMA lock is left unperturbed.
 */
/*
 * 为 fault 尝试取得 @folio 锁，并按 vmf flags 决定是否释放
 * mmap_lock/per-VMA lock 后要求上层重试。
 *
 * 首次允许 retry 时，NOWAIT 直接返回 VM_FAULT_RETRY 但保留 fault lock；
 * 否则先 release_fault_lock，再 killable/不可中断等待，始终 RETRY。
 * 不允许 retry 时就在当前调用内取得锁；killable 中断会释放 fault lock
 * 并 RETRY。返回 0 表示 folio locked，非 0 表示未锁，锁释放例外严格
 * 如上。调用者始终持 folio 引用。
 */
vm_fault_t __folio_lock_or_retry(struct folio *folio, struct vm_fault *vmf)
{
	unsigned int flags = vmf->flags;

	if (fault_flag_allow_retry_first(flags)) {
		/*
		 * CAUTION! In this case, mmap_lock/per-VMA lock is not
		 * released even though returning VM_FAULT_RETRY.
		 */
		/*
		 * RETRY_NOWAIT 的契约是只请求上层尽快重试，不在这里
		 * 睡眠，也不释放 fault lock；这是非零返回的一项明确例外。
		 */
		if (flags & FAULT_FLAG_RETRY_NOWAIT)
			return VM_FAULT_RETRY;

		release_fault_lock(vmf);
		if (flags & FAULT_FLAG_KILLABLE)
			folio_wait_locked_killable(folio);
		else
			folio_wait_locked(folio);
		/* 等待只减少下次竞争；fault 语义仍要求上层完整重试。 */
		return VM_FAULT_RETRY;
	}
	if (flags & FAULT_FLAG_KILLABLE) {
		bool ret;

		/* 信号打断时释放 fault lock，以 RETRY 交回上层重走。 */
		ret = __folio_lock_killable(folio);
		if (ret) {
			release_fault_lock(vmf);
			return VM_FAULT_RETRY;
		}
	} else {
		/* 阻塞模式直接取得 folio lock，完成后由 fault 后续路径负责解锁。 */
		__folio_lock(folio);
	}

	return 0;
}

/**
 * page_cache_next_miss() - Find the next gap in the page cache.
 * @mapping: Mapping.
 * @index: Index.
 * @max_scan: Maximum range to search.
 *
 * Search the range [index, min(index + max_scan - 1, ULONG_MAX)] for the
 * gap with the lowest index.
 *
 * This function may be called under the rcu_read_lock.  However, this will
 * not atomically search a snapshot of the cache at a single point in time.
 * For example, if a gap is created at index 5, then subsequently a gap is
 * created at index 10, page_cache_next_miss covering both indices may
 * return 10 if called under the rcu_read_lock.
 *
 * Return: The index of the gap if found, otherwise an index outside the
 * range specified (in which case 'return - index >= max_scan' will be true).
 * In the rare case of index wrap-around, 0 will be returned.
 */
/*
 * 从 @index 向上最多扫描 @max_scan 个 XArray 位置，NULL 或
 * value entry 均视为没有真实 page-cache folio 的 gap。可在 RCU 下调用，
 * 但不是单时刻快照，并发创建多个 gap 时可能返回后创建者。
 *
 * 找到返回最低 gap index；未找到返回范围末后一位，使差值>=max_scan；
 * index 回绕返回 0。只扫描、不取 folio 引用。
 */
pgoff_t page_cache_next_miss(struct address_space *mapping,
			     pgoff_t index, unsigned long max_scan)
{
	XA_STATE(xas, &mapping->i_pages, index);

	while (max_scan--) {
		void *entry = xas_next(&xas);
		/* NULL/value 都是 hole；真实 folio 继续消耗扫描预算。 */
		if (!entry || xa_is_value(entry))
			return xas.xa_index;
		if (xas.xa_index == 0)
			return 0;
	}

	/* Return end of the range + 1 when no hole is found */
	/* 用范围外哨兵统一表达“扫描预算内没有 gap”。 */
	return xas.xa_index + 1;
}
EXPORT_SYMBOL(page_cache_next_miss);

/**
 * page_cache_prev_miss() - Find the previous gap in the page cache.
 * @mapping: Mapping.
 * @index: Index.
 * @max_scan: Maximum range to search.
 *
 * Search the range [max(index - max_scan + 1, 0), index] for the
 * gap with the highest index.
 *
 * This function may be called under the rcu_read_lock.  However, this will
 * not atomically search a snapshot of the cache at a single point in time.
 * For example, if a gap is created at index 10, then subsequently a gap is
 * created at index 5, page_cache_prev_miss() covering both indices may
 * return 5 if called under the rcu_read_lock.
 *
 * Return: The index of the gap if found, otherwise an index outside the
 * range specified (in which case 'index - return >= max_scan' will be true).
 * In the rare case of wrap-around, ULONG_MAX will be returned.
 */
/*
 * next_miss 的反向版本，从 @index 向下扫描，返回最高 gap。
 * RCU 下同样是弱一致遍历；未找到返回范围起点前一位，回绕返回 ULONG_MAX。
 * NULL/value 视为 gap，不取得任何引用。
 */
pgoff_t page_cache_prev_miss(struct address_space *mapping,
			     pgoff_t index, unsigned long max_scan)
{
	XA_STATE(xas, &mapping->i_pages, index);

	while (max_scan--) {
		void *entry = xas_prev(&xas);
		/* 反向扫描同样把 NULL/value 视为 hole，并防止索引下溢。 */
		if (!entry || xa_is_value(entry))
			return xas.xa_index;
		if (xas.xa_index == ULONG_MAX)
			return ULONG_MAX;
	}

	/* Return start of the range - 1 when no hole is found */
	/* 范围外哨兵让调用者通过 index-return>=max_scan 判断未命中。 */
	return xas.xa_index - 1;
}
EXPORT_SYMBOL(page_cache_prev_miss);

/*
 * Lockless page cache protocol:
 * On the lookup side:
 * 1. Load the folio from i_pages
 * 2. Increment the refcount if it's not zero
 * 3. If the folio is not found by xas_reload(), put the refcount and retry
 *
 * On the removal side:
 * A. Freeze the page (by zeroing the refcount if nobody else has a reference)
 * B. Remove the page from i_pages
 * C. Return the page to the page allocator
 *
 * This means that any page may have its reference count temporarily
 * increased by a speculative page cache (or GUP-fast) lookup as it can
 * be allocated by another user before the RCU grace period expires.
 * Because the refcount temporarily acquired here may end up being the
 * last refcount on the page, any page allocation must be freeable by
 * folio_put().
 */
/*
 * 无锁查找协议是“RCU load -> 非零增引用 -> xas_reload 验证仍
 * 是同 entry”，失败就 put/retry。删除侧先冻结 refcount，再摘 XArray，
 * 最后释放。RCU 宽限期内物理页可能已被重新分配，所以 speculative get
 * 可能临时落在新用途页上；所有分配得到的 folio 都必须能安全 folio_put，
 * 且 reload 验证阻止把已换身份的页当原 page-cache 命中返回。
 */

/*
 * filemap_get_entry - Get a page cache entry.
 * @mapping: the address_space to search
 * @index: The page cache index.
 *
 * Looks up the page cache entry at @mapping & @index.  If it is a folio,
 * it is returned with an increased refcount.  If it is a shadow entry
 * of a previously evicted folio, or a swap entry from shmem/tmpfs,
 * it is returned without further action.
 *
 * Return: The folio, swap or shadow entry, %NULL if nothing is found.
 */
/*
 * 在 @mapping/@index 查 entry。真实 folio 按上述无锁协议取得
 * 引用后返回，调用者必须 folio_put；shadow 或 shmem swap value 原样返回，
 * 不增引用；空返回 NULL。函数在内部 RCU 临界区重试，不锁 folio，
 * 因而返回 folio 内容/mapping 仍可并发改变。
 */
void *filemap_get_entry(struct address_space *mapping, pgoff_t index)
{
	XA_STATE(xas, &mapping->i_pages, index);
	struct folio *folio;

	rcu_read_lock();
repeat:
	xas_reset(&xas);
	folio = xas_load(&xas);
	if (xas_retry(&xas, folio))
		goto repeat;
	/*
	 * A shadow entry of a recently evicted page, or a swap entry from
	 * shmem/tmpfs.  Return it without attempting to raise page count.
	 */
	/*
	 * XArray value 不是 struct folio 指针，可能是 shadow/swap；
	 * 直接返回给能识别它的上层，绝不能 folio_try_get。
	 */
	if (!folio || xa_is_value(folio))
		goto out;

	/* refcount 已被冻结表示删除进行中，回到 XArray 重新查。 */
	if (!folio_try_get(folio))
		goto repeat;

	/*
	 * 增引用与摘除可并发；reload 必须仍是同 folio 才能把 speculative
	 * 引用升级为有效命中，否则 put 并重试。
	 */
	if (unlikely(folio != xas_reload(&xas))) {
		folio_put(folio);
		goto repeat;
	}
out:
	rcu_read_unlock();

	return folio;
}

/**
 * __filemap_get_folio_mpol - Find and get a reference to a folio.
 * @mapping: The address_space to search.
 * @index: The page index.
 * @fgp_flags: %FGP flags modify how the folio is returned.
 * @gfp: Memory allocation flags to use if %FGP_CREAT is specified.
 * @policy: NUMA memory allocation policy to follow.
 *
 * Looks up the page cache entry at @mapping & @index.
 *
 * If %FGP_LOCK or %FGP_CREAT are specified then the function may sleep even
 * if the %GFP flags specified for %FGP_CREAT are atomic.
 *
 * If this function returns a folio, it is returned with an increased refcount.
 *
 * Return: The found folio or an ERR_PTR() otherwise.
 */
/*
 * 查找 index 所在 folio，并按 @fgp_flags 选择加锁、创建、访问
 * 记账、稳定等待与 DONTCACHE。返回 folio 始终持一份引用；FGP_LOCK 成功
 * 时还持 folio lock，FGP_FOR_MMAP 新建页则按 mmap 契约解锁。
 *
 * value entry 当作无页。已有 folio 加锁后必须重验 mapping，处理 truncate
 * 竞态。创建时依据 mapping min/max order、对齐和 NUMA policy 分配，从
 * 大 order 失败逐级降级；插入 EEXIST 表示竞争者获胜，释放候选后重查。
 * 错误返回 -ENOENT/-EAGAIN/-ENOMEM 等 ERR_PTR。即便 gfp 原子，只要要求
 * LOCK/CREATE 仍可能因 folio lock 或 XArray 分配睡眠。
 */
struct folio *__filemap_get_folio_mpol(struct address_space *mapping,
		pgoff_t index, fgf_t fgp_flags, gfp_t gfp, struct mempolicy *policy)
{
	struct folio *folio;

repeat:
	/* 阶段 1：无锁取得引用；exceptional value 不作为普通 folio 返回。 */
	folio = filemap_get_entry(mapping, index);
	if (xa_is_value(folio))
		folio = NULL;
	if (!folio)
		goto no_page;

	if (fgp_flags & FGP_LOCK) {
		/*
		 * NOWAIT 只 trylock，失败放引用并返回 -EAGAIN；阻塞模式等待。
		 * 得锁后 mapping 重验把生命周期保证升级为“仍属于目标 cache”。
		 */
		if (fgp_flags & FGP_NOWAIT) {
			if (!folio_trylock(folio)) {
				folio_put(folio);
				return ERR_PTR(-EAGAIN);
			}
		} else {
			folio_lock(folio);
		}

		/* Has the page been truncated? */
		/* 等待锁期间 truncate 可摘除 folio；解锁/put 后重查。 */
		if (unlikely(folio->mapping != mapping)) {
			folio_unlock(folio);
			folio_put(folio);
			goto repeat;
		}
		VM_BUG_ON_FOLIO(!folio_contains(folio, index), folio);
	}

	if (fgp_flags & FGP_ACCESSED)
		/* 读取语义更新 referenced/active 工作集状态。 */
		folio_mark_accessed(folio);
	else if (fgp_flags & FGP_WRITE) {
		/* Clear idle flag for buffer write */
		/* buffered write 是实际访问，清 idle 防止监控误判冷页。 */
		if (folio_test_idle(folio))
			folio_clear_idle(folio);
	}

	if (fgp_flags & FGP_STABLE)
		/* 等待文件系统定义的 stable 条件，避免与 writeback 内容修改冲突。 */
		folio_wait_stable(folio);
no_page:
	if (!folio && (fgp_flags & FGP_CREAT)) {
		unsigned int min_order = mapping_min_folio_order(mapping);
		unsigned int order = max(min_order, FGF_GET_ORDER(fgp_flags));
		int err;
		/*
		 * 阶段 2：新 folio index 先按 mapping 最小 order 对齐；请求 order
		 * 被 min/max 限制，若目标自身不满足大 order 对齐则降到 __ffs。
		 */
		index = mapping_align_index(mapping, index);

		/*
		 * 根据业务约束调整分配：可写回写入加 __GFP_WRITE；NOFS 禁止递归
		 * 文件系统回收；NOWAIT 去掉阻塞 GFP_KERNEL 并改为 GFP_NOWAIT。
		 */
		if ((fgp_flags & FGP_WRITE) && mapping_can_writeback(mapping))
			gfp |= __GFP_WRITE;
		if (fgp_flags & FGP_NOFS)
			gfp &= ~__GFP_FS;
		if (fgp_flags & FGP_NOWAIT) {
			gfp &= ~GFP_KERNEL;
			gfp |= GFP_NOWAIT;
		}
		/*
		 * 创建后必须由锁或 mmap 专用发布协议保护初始化；缺失标志是调用
		 * 错误，告警并强制 FGP_LOCK，宁可保守串行化。
		 */
		if (WARN_ON_ONCE(!(fgp_flags & (FGP_LOCK | FGP_FOR_MMAP))))
			fgp_flags |= FGP_LOCK;

		if (order > mapping_max_folio_order(mapping))
			order = mapping_max_folio_order(mapping);
		/* If we're not aligned, allocate a smaller folio */
		/* 目标 index 不满足大 folio 对齐时降低 order，避免覆盖相邻索引。 */
		if (index & ((1UL << order) - 1))
			order = __ffs(index);

		/*
		 * 阶段 3：优先尝试大 folio；高于 min order 使用 NORETRY/NOWARN，
		 * 失败静默逐阶降级，min order 才按完整 gfp 语义尝试。
		 */
		do {
			gfp_t alloc_gfp = gfp;

			err = -ENOMEM;
			if (order > min_order)
				alloc_gfp |= __GFP_NORETRY | __GFP_NOWARN;
			folio = filemap_alloc_folio(alloc_gfp, order, policy);
			if (!folio)
				continue;

			/* Init accessed so avoid atomic mark_page_accessed later */
			/*
			 * 尚未发布时可非原子设置 referenced；DONTCACHE
			 * 预置 dropbehind，使写回/读完后尽快失效而不污染 cache。
			 */
			if (fgp_flags & FGP_ACCESSED)
				__folio_set_referenced(folio);
			if (fgp_flags & FGP_DONTCACHE)
				__folio_set_dropbehind(folio);

			/* 插入成功后 folio locked；失败候选仍归本函数并立即 put。 */
			err = filemap_add_folio(mapping, folio, index, gfp);
			if (!err)
				break;
			folio_put(folio);
			folio = NULL;
		} while (order-- > min_order);

		if (err == -EEXIST)
			/* 并发创建者先发布，释放候选后回到查找并取得它的引用。 */
			goto repeat;
		if (err) {
			/*
			 * When NOWAIT I/O fails to allocate folios this could
			 * be due to a nonblocking memory allocation and not
			 * because the system actually is out of memory.
			 * Return -EAGAIN so that there caller retries in a
			 * blocking fashion instead of propagating -ENOMEM
			 * to the application.
			 */
			/*
			 * NOWAIT 的 -ENOMEM 可能只是“禁止阻塞分配”而非真实
			 * OOM，改成 -EAGAIN 提示上层退回阻塞 I/O，避免误报应用。
			 */
			if ((fgp_flags & FGP_NOWAIT) && err == -ENOMEM)
				err = -EAGAIN;
			return ERR_PTR(err);
		}
		/*
		 * filemap_add_folio locks the page, and for mmap
		 * we expect an unlocked page.
		 */
		/*
		 * filemap_add_folio 固定以 locked 状态发布候选；mmap
		 * 调用链需要未锁页让 fault 后续按自己的锁序处理，因此在此解锁。
		 */
		if (folio && (fgp_flags & FGP_FOR_MMAP))
			folio_unlock(folio);
	}

	if (!folio)
		return ERR_PTR(-ENOENT);
	/* not an uncached lookup, clear uncached if set */
	/*
	 * 阶段 4：普通缓存 lookup 命中曾标 DONTCACHE 的 folio 时取消 dropbehind。
	 * 若它已 dirty 且可写回，必须同步扣 WB_DONTCACHE_DIRTY，保持 writeback
	 * 统计与标志一致；inode->wb 获取使用 unlocked cookie 协议。
	 */
	if (!(fgp_flags & FGP_DONTCACHE) && folio_test_clear_dropbehind(folio)) {
		if (folio_test_dirty(folio) &&
		    mapping_can_writeback(mapping)) {
			struct inode *inode = mapping->host;
			struct bdi_writeback *wb;
			struct wb_lock_cookie cookie = {};
			long nr = folio_nr_pages(folio);

			/* wb cookie 稳定 inode 当前 writeback 归属，修改后必须成对结束。 */
			wb = unlocked_inode_to_wb_begin(inode, &cookie);
			wb_stat_mod(wb, WB_DONTCACHE_DIRTY, -nr);
			unlocked_inode_to_wb_end(inode, &cookie);
		}
	}
	/* 返回前 dropbehind flag 与 WB_DONTCACHE_DIRTY 记账已经同步。 */
	return folio;
}
EXPORT_SYMBOL(__filemap_get_folio_mpol);

/*
 * find_get_entry() - 从当前 XArray 游标向前找 present/marked entry 并稳定引用。
 *
 * @xas：调用者已在 RCU 临界区的游标；@max：末 index；@mark：XA_PRESENT
 * 或具体 tag。value/NULL 原样返回；真实 folio 用 try_get + reload 验证，
 * 成功返回持有引用。删除/替换竞态时 reset 后重试。
 */
static inline struct folio *find_get_entry(struct xa_state *xas, pgoff_t max,
		xa_mark_t mark)
{
	struct folio *folio;

retry:
	if (mark == XA_PRESENT)
		folio = xas_find(xas, max);
	/* 指定 mark 时只替换查找原语，后续引用稳定协议相同。 */
	else
		folio = xas_find_marked(xas, max, mark);

	if (xas_retry(xas, folio))
		goto retry;
	/*
	 * A shadow entry of a recently evicted page, a swap
	 * entry from shmem/tmpfs or a DAX entry.  Return it
	 * without attempting to raise page count.
	 */
	/*
	 * shadow、shmem swap 或 DAX value 不是 folio；调用者决定
	 * 是否保留/跳过，不能增 page ref。
	 */
	if (!folio || xa_is_value(folio))
		return folio;

	if (!folio_try_get(folio))
		goto reset;

	if (unlikely(folio != xas_reload(xas))) {
		folio_put(folio);
		goto reset;
	}

	/* reload 成功证明引用仍对应当前槽，可以安全交给调用者。 */
	return folio;
reset:
	xas_reset(xas);
	goto retry;
}

/**
 * find_get_entries - gang pagecache lookup
 * @mapping:	The address_space to search
 * @start:	The starting page cache index
 * @end:	The final page index (inclusive).
 * @fbatch:	Where the resulting entries are placed.
 * @indices:	The cache indices corresponding to the entries in @entries
 *
 * find_get_entries() will search for and return a batch of entries in
 * the mapping.  The entries are placed in @fbatch.  find_get_entries()
 * takes a reference on any actual folios it returns.
 *
 * The entries have ascending indexes.  The indices may not be consecutive
 * due to not-present entries or large folios.
 *
 * Any shadow entries of evicted folios, or swap entries from
 * shmem/tmpfs, are included in the returned array.
 *
 * Return: The number of entries which were found.
 */
/*
 * 从 *@start 到 @end 批量收集全部 entry，包含 shadow/swap
 * value；真实 folio 带引用。@indices 与 fbatch 同槽记录实际 XArray
 * index，升序但可能因 hole/large entry 不连续。
 *
 * 返回数量，并把 *start 推到最后 entry 覆盖范围之后且按其 order 对齐；
 * 调用者必须释放 batch 中真实 folio，value 不拥有引用。
 */
unsigned find_get_entries(struct address_space *mapping, pgoff_t *start,
		pgoff_t end, struct folio_batch *fbatch, pgoff_t *indices)
{
	XA_STATE(xas, &mapping->i_pages, *start);
	struct folio *folio;

	rcu_read_lock();
	/* 第一阶段只在 RCU 下定位/稳定候选；引用验证失败会重置 XArray 游标。 */
	while ((folio = find_get_entry(&xas, end, XA_PRESENT)) != NULL) {
		indices[fbatch->nr] = xas.xa_index;
		if (!folio_batch_add(fbatch, folio))
			break;
	}

	if (folio_batch_count(fbatch)) {
		/* 第二阶段从 batch 尾项的 order 推导下一次不会重复的起点。 */
		unsigned long nr;
		int idx = folio_batch_count(fbatch) - 1;

		folio = fbatch->folios[idx];
		if (!xa_is_value(folio))
			nr = folio_nr_pages(folio);
		else
			nr = 1 << xa_get_order(&mapping->i_pages, indices[idx]);
		/*
		 * large folio/value 可覆盖多个 sibling index；下一起点越过整个
		 * entry，round_down 保持 order 边界。
		 */
		*start = round_down(indices[idx] + nr, nr);
	}
	rcu_read_unlock();

	/* 真实 folio 保持 locked+持引用；value entry 没有引用责任。 */
	return folio_batch_count(fbatch);
}

/**
 * find_lock_entries - Find a batch of pagecache entries.
 * @mapping:	The address_space to search.
 * @start:	The starting page cache index.
 * @end:	The final page index (inclusive).
 * @fbatch:	Where the resulting entries are placed.
 * @indices:	The cache indices of the entries in @fbatch.
 *
 * find_lock_entries() will return a batch of entries from @mapping.
 * Swap, shadow and DAX entries are included.  Folios are returned
 * locked and with an incremented refcount.  Folios which are locked
 * by somebody else or under writeback are skipped.  Folios which are
 * partially outside the range are not returned.
 *
 * The entries have ascending indexes.  The indices may not be consecutive
 * due to not-present entries, large folios, folios which could not be
 * locked or folios under writeback.
 *
 * Return: The number of entries which were found.
 */
/*
 * 批量返回完全落在 [*start,end] 的 entry。真实 folio 必须
 * trylock 成功、仍属于 mapping 且不在 writeback，返回时 locked+持引用；
 * 锁竞争/writeback/部分越界 folio 被跳过。value entry 原样返回。
 *
 * *start 每接纳一项即推进到其覆盖范围之后，确保 batch 满时可续扫。
 * 调用者对 folio 解锁并 put；value 无引用。整个扫描 RCU 弱一致。
 */
unsigned find_lock_entries(struct address_space *mapping, pgoff_t *start,
		pgoff_t end, struct folio_batch *fbatch, pgoff_t *indices)
{
	XA_STATE(xas, &mapping->i_pages, *start);
	struct folio *folio;

	rcu_read_lock();
	/* 扫描在 RCU 下同时处理真实 folio 与带 order 的 value entry。 */
	while ((folio = find_get_entry(&xas, end, XA_PRESENT))) {
		unsigned long base;
		unsigned long nr;

		if (!xa_is_value(folio)) {
			nr = folio_nr_pages(folio);
			base = folio->index;
			/* Omit large folio which begins before the start */
			/* truncate/invalidate 批次不能只处理大 folio 的一部分。 */
			if (base < *start)
				goto put;
			/* Omit large folio which extends beyond the end */
			/* 同理拒绝跨越右边界的大 folio。 */
			if (base + nr - 1 > end)
				goto put;
			if (!folio_trylock(folio))
				goto put;
			if (folio->mapping != mapping ||
			    folio_test_writeback(folio))
				goto unlock;
			/* 锁下归属与 writeback 重验通过，才接受为稳定候选。 */
			VM_BUG_ON_FOLIO(!folio_contains(folio, xas.xa_index),
					folio);
		} else {
			nr = 1 << xas_get_order(&xas);
			base = xas.xa_index & ~(nr - 1);
			/* Omit order>0 value which begins before the start */
			/* 大 value 也必须完整落入范围，否则不返回半个 entry。 */
			if (base < *start)
				continue;
			/* Omit order>0 value which extends beyond the end */
			/* value 已越右界，后续升序 entry 也无需继续。 */
			if (base + nr - 1 > end)
				break;
		}

		/* Update start now so that last update is correct on return */
		/* 先推进游标再加 batch，batch 恰好满时续扫位置仍正确。 */
		*start = base + nr;
		indices[fbatch->nr] = xas.xa_index;
		if (!folio_batch_add(fbatch, folio))
			break;
		continue;
unlock:
		/* unlock/put 标签分别撤销 folio 锁与查找引用，所有跳过路径在此配对。 */
		folio_unlock(folio);
put:
		folio_put(folio);
	}
	rcu_read_unlock();

	return folio_batch_count(fbatch);
}

/**
 * filemap_get_folios - Get a batch of folios
 * @mapping:	The address_space to search
 * @start:	The starting page index
 * @end:	The final page index (inclusive)
 * @fbatch:	The batch to fill.
 *
 * Search for and return a batch of folios in the mapping starting at
 * index @start and up to index @end (inclusive).  The folios are returned
 * in @fbatch with an elevated reference count.
 *
 * Return: The number of folios which were found.
 * We also update @start to index the next folio for the traversal.
 */
/*
 * XA_PRESENT 的通用批量包装。返回按 index 升序、各持引用的
 * folio，允许 hole；*@start 推到下一 folio 位置。具体实现复用 tag 扫描。
 *
 * @mapping 为借用索引；@start 是页索引单位的输入输出游标，
 * @end 为含端点上界；@fbatch 由调用者提供并接收持引用 folio。函数使用
 * XArray RCU 查找、不要求入口锁且不睡眠。返回批量数量；调用者必须逐项 put。
 */
unsigned filemap_get_folios(struct address_space *mapping, pgoff_t *start,
		pgoff_t end, struct folio_batch *fbatch)
{
	return filemap_get_folios_tag(mapping, start, end, XA_PRESENT, fbatch);
}
EXPORT_SYMBOL(filemap_get_folios);

/**
 * filemap_get_folios_contig - Get a batch of contiguous folios
 * @mapping:	The address_space to search
 * @start:	The starting page index
 * @end:	The final page index (inclusive)
 * @fbatch:	The batch to fill
 *
 * filemap_get_folios_contig() works exactly like filemap_get_folios(),
 * except the returned folios are guaranteed to be contiguous. This may
 * not return all contiguous folios if the batch gets filled up.
 *
 * Return: The number of folios found.
 * Also update @start to be positioned for traversal of the next folio.
 */
/*
 * 只返回从 *@start 起连续覆盖的真实 folio；遇到 NULL、
 * swap/shadow/DAX value 或落在 THP sibling 中间即停止。每个返回 folio
 * 持引用，batch 满也可能尚有更多连续页；*@start 指向最后 folio 之后。
 * RCU 弱一致，try_get/reload 防止删除复用竞态。
 */

unsigned filemap_get_folios_contig(struct address_space *mapping,
		pgoff_t *start, pgoff_t end, struct folio_batch *fbatch)
{
	XA_STATE(xas, &mapping->i_pages, *start);
	unsigned long nr;
	struct folio *folio;

	rcu_read_lock();

	/* 主循环要求接纳的 folio 从当前游标连续覆盖；任一洞立即收口。 */
	for (folio = xas_load(&xas); folio && xas.xa_index <= end;
			folio = xas_next(&xas)) {
		if (xas_retry(&xas, folio))
			continue;
		/*
		 * If the entry has been swapped out, we can stop looking.
		 * No current caller is looking for DAX entries.
		 */
		/*
		 * value 表示连续真实 page cache 已中断；当前调用者也
		 * 不请求 DAX entry，故立即停止。
		 */
		if (xa_is_value(folio))
			goto update_start;

		/* If we landed in the middle of a THP, continue at its end. */
		/*
		 * 起点落在大 folio sibling 而非 head，无法把“完整 folio
		 * 从起点连续”表达出来，停止并由已有 batch 决定下一位置。
		 */
		if (xa_is_sibling(folio))
			goto update_start;

		if (!folio_try_get(folio))
			goto retry;

		/* try_get 后必须 reload，防止引用稳定的是已经被替换的旧对象。 */
		if (unlikely(folio != xas_reload(&xas)))
			goto put_folio;

		if (!folio_batch_add(fbatch, folio)) {
			*start = folio_next_index(folio);
			goto out;
		}
		xas_advance(&xas, folio_next_index(folio) - 1);
		continue;
	/* reload 失败时先 put 候选引用，再 reset 游标重试逻辑位置。 */
put_folio:
		folio_put(folio);

retry:
		xas_reset(&xas);
	}

update_start:
	/* 收口阶段只根据最后一个已接纳 folio 推进游标，空 batch 保持原起点。 */
	nr = folio_batch_count(fbatch);

	if (nr) {
		folio = fbatch->folios[nr - 1];
		*start = folio_next_index(folio);
	}
out:
	rcu_read_unlock();
	return folio_batch_count(fbatch);
}
EXPORT_SYMBOL(filemap_get_folios_contig);

/**
 * filemap_get_folios_tag - Get a batch of folios matching @tag
 * @mapping:    The address_space to search
 * @start:      The starting page index
 * @end:        The final page index (inclusive)
 * @tag:        The tag index
 * @fbatch:     The batch to fill
 *
 * The first folio may start before @start; if it does, it will contain
 * @start.  The final folio may extend beyond @end; if it does, it will
 * contain @end.  The folios have ascending indices.  There may be gaps
 * between the folios if there are indices which have no folio in the
 * page cache.  If folios are added to or removed from the page cache
 * while this is running, they may or may not be found by this call.
 * Only returns folios that are tagged with @tag.
 *
 * Return: The number of folios found.
 * Also update @start to index the next folio for traversal.
 */
/*
 * 按 @tag（DIRTY/WRITEBACK/XA_PRESENT 等）批量取得 folio 引用。
 * 首尾大 folio 可跨范围边界但必须包含对应 start/end；并发增删下结果是
 * 弱一致快照。value 理论上不应带 tag，若 reclaim 竞态把已见 folio换成
 * shadow，则跳过。返回数量并推进 *start，end==-1 时防止加一溢出。
 */
unsigned filemap_get_folios_tag(struct address_space *mapping, pgoff_t *start,
			pgoff_t end, xa_mark_t tag, struct folio_batch *fbatch)
{
	XA_STATE(xas, &mapping->i_pages, *start);
	struct folio *folio;

	rcu_read_lock();
	while ((folio = find_get_entry(&xas, end, tag)) != NULL) {
		/*
		 * Shadow entries should never be tagged, but this iteration
		 * is lockless so there is a window for page reclaim to evict
		 * a page we saw tagged. Skip over it.
		 */
		/*
		 * 无锁扫描先看到 tag、后看到 reclaim shadow 的窗口是
		 * 合法竞态；value 没有 folio 引用，直接继续。
		 */
		if (xa_is_value(folio))
			continue;
		if (!folio_batch_add(fbatch, folio)) {
			*start = folio_next_index(folio);
			goto out;
		}
		/* batch 尚有容量时继续；锁竞争项按保守 dirty 语义接纳。 */
	}
	/*
	 * We come here when there is no page beyond @end. We take care to not
	 * overflow the index @start as it confuses some of the callers. This
	 * breaks the iteration when there is a page at index -1 but that is
	 * already broke anyway.
	 */
	/*
	 * 扫描耗尽时把起点置 end+1；若 end 为全 1，保持全 1
	 * 防止回绕到 0 让调用者重新扫描。
	 */
	if (end == (pgoff_t)-1)
		*start = (pgoff_t)-1;
	else
		*start = end + 1;
out:
	rcu_read_unlock();

	return folio_batch_count(fbatch);
}
EXPORT_SYMBOL(filemap_get_folios_tag);

/**
 * filemap_get_folios_dirty - Get a batch of dirty folios
 * @mapping:	The address_space to search
 * @start:	The starting folio index
 * @end:	The final folio index (inclusive)
 * @fbatch:	The batch to fill
 *
 * filemap_get_folios_dirty() works exactly like filemap_get_folios(), except
 * the returned folios are presumed to be dirty or undergoing writeback. Dirty
 * state is presumed because we don't block on folio lock nor want to miss
 * folios. Callers that need to can recheck state upon locking the folio.
 *
 * This may not return all dirty folios if the batch gets filled up.
 *
 * Return: The number of folios found.
 * Also update @start to be positioned for traversal of the next folio.
 */
/*
 * 批量筛选“可能 dirty 或 writeback”的 folio。为避免因锁竞争
 * 漏掉正在转态的页，trylock 失败者保守返回；trylock 成功才可靠排除同时
 * clean 且非 writeback 的页。返回 folio 均持引用，调用者锁定后可再验。
 * batch 满/扫描耗尽时推进 *start，end==-1 防溢出。
 */
unsigned filemap_get_folios_dirty(struct address_space *mapping, pgoff_t *start,
			pgoff_t end, struct folio_batch *fbatch)
{
	XA_STATE(xas, &mapping->i_pages, *start);
	struct folio *folio;

	rcu_read_lock();
	/* 对每个候选做无阻塞状态筛选；锁竞争者保守按“可能脏”保留。 */
	while ((folio = find_get_entry(&xas, end, XA_PRESENT)) != NULL) {
		if (xa_is_value(folio))
			continue;
		if (folio_trylock(folio)) {
			bool clean = !folio_test_dirty(folio) &&
				     !folio_test_writeback(folio);
			folio_unlock(folio);
			if (clean) {
				folio_put(folio);
				/* 可锁定且确认 clean 的项被准确排除，不进入输出 batch。 */
				continue;
			}
		}
		if (!folio_batch_add(fbatch, folio)) {
			*start = folio_next_index(folio);
			goto out;
		}
	}
	/*
	 * We come here when there is no folio beyond @end. We take care to not
	 * overflow the index @start as it confuses some of the callers. This
	 * breaks the iteration when there is a folio at index -1 but that is
	 * already broke anyway.
	 */
	/* 与 tag 扫描相同，避免 end+1 回绕破坏外层遍历。 */
	if (end == (pgoff_t)-1)
		*start = (pgoff_t)-1;
	else
		*start = end + 1;
out:
	rcu_read_unlock();

	return folio_batch_count(fbatch);
}

/*
 * CD/DVDs are error prone. When a medium error occurs, the driver may fail
 * a _large_ part of the i/o request. Imagine the worst scenario:
 *
 *      ---R__________________________________________B__________
 *         ^ reading here                             ^ bad block(assume 4k)
 *
 * read(R) => miss => readahead(R...B) => media error => frustrating retries
 * => failing the whole request => read(R) => read(R+1) =>
 * readahead(R+1...B+1) => bang => read(R+2) => read(R+3) =>
 * readahead(R+3...B+2) => bang => read(R+3) => read(R+4) =>
 * readahead(R+4...B+3) => bang => read(R+4) => read(R+5) => ......
 *
 * It is going insane. Fix it by quickly scaling down the readahead size.
 */
/*
 * CD/DVD 介质错误可能让包含坏块 B 的大 readahead 请求整体
 * 失败；顺序读每前进一步又发一个仍跨 B 的大请求，形成反复重试风暴。
 * 读取 EIO 后把 ra_pages 快速缩为四分之一，让窗口尽快收敛到坏块附近。
 *
 * shrink_readahead_size_eio() - 调低单 file readahead 窗口。
 * @ra 为 file 私有状态；无返回，最小值可自然降到 0。
 */
static void shrink_readahead_size_eio(struct file_ra_state *ra)
{
	ra->ra_pages /= 4;
}

/*
 * filemap_get_read_batch - Get a batch of folios for read
 *
 * Get a batch of folios which represent a contiguous range of bytes in
 * the file.  No exceptional entries will be returned.  If @index is in
 * the middle of a folio, the entire folio will be returned.  The last
 * folio in the batch may have the readahead flag set or the uptodate flag
 * clear so that the caller can take the appropriate action.
 */
/*
 * 从 @index 到 @max 收集一段连续真实 folio，每项持引用。
 * 起点落在大 folio 内时返回整个 folio；遇到 exceptional/sibling gap、
 * 非 uptodate 或 readahead 标志即停止，最后一项留给调用者触发 I/O/
 * 异步预读。RCU 下通过 try_get+reload 稳定，不修改外部位置。
 */
static void filemap_get_read_batch(struct address_space *mapping,
		pgoff_t index, pgoff_t max, struct folio_batch *fbatch)
{
	XA_STATE(xas, &mapping->i_pages, index);
	struct folio *folio;

	rcu_read_lock();
	/* 阶段 1：从精确起点向前，任何 value/sibling/范围边界都会终止连续批次。 */
	for (folio = xas_load(&xas); folio; folio = xas_next(&xas)) {
		if (xas_retry(&xas, folio))
			continue;
		if (xas.xa_index > max || xa_is_value(folio))
			break;
		if (xa_is_sibling(folio))
			break;
		if (!folio_try_get(folio))
			goto retry;

		/* 阶段 2：reload 验证引用仍对应当前槽，再按可读状态决定批次终点。 */
		if (unlikely(folio != xas_reload(&xas)))
			goto put_folio;

		if (!folio_batch_add(fbatch, folio))
			break;
		if (!folio_test_uptodate(folio))
			break;
		if (folio_test_readahead(folio))
			break;
		/* 完全有效且无 RA 标记的 folio 可跳到其末端继续连续扫描。 */
		xas_advance(&xas, folio_next_index(folio) - 1);
		continue;
put_folio:
		folio_put(folio);
retry:
		xas_reset(&xas);
	}
	/* batch 引用已稳定，可先退出 RCU 再由上层消费。 */
	rcu_read_unlock();
}

/*
 * filemap_read_folio() - 调文件系统 filler 启动读取并等待 folio 解锁完成。
 *
 * @file 可空（无 file 级 readahead 状态）；@filler 通常是 a_ops->read_folio，
 * 契约是接管 locked folio 的 I/O 并最终解锁；@folio 由调用者持引用。
 * workingset miss 计入 PSI memstall。返回 filler errno、-EINTR、0
 *（uptodate）或 -EIO；EIO 时缩小该 file readahead 窗口。
 */
static int filemap_read_folio(struct file *file, filler_t filler,
		struct folio *folio)
{
	bool workingset = folio_test_workingset(folio);
	unsigned long pflags;
	int error;

	/* Start the actual read. The read will unlock the page. */
	/* filler 成功仅表示 I/O 已提交，真正完成由 PG_locked 清除发布。 */
	if (unlikely(workingset))
		psi_memstall_enter(&pflags);
	error = filler(file, folio);
	if (unlikely(workingset))
		psi_memstall_leave(&pflags);
	if (error)
		return error;

	error = folio_wait_locked_killable(folio);
	/* I/O 完成由 unlock 发布；等待后以 uptodate 区分成功与介质错误。 */
	if (error)
		return error;
	if (folio_test_uptodate(folio))
		return 0;
	if (file)
		shrink_readahead_size_eio(&file->f_ra);
	return -EIO;
}

/*
 * filemap_range_uptodate() - 判断本次读取覆盖的 folio 子范围是否已有有效数据。
 *
 * @pos/@count 为文件字节范围；@folio 持引用；@need_uptodate 为 true 时
 * 要求整 folio uptodate（pipe 无法安全处理部分有效页）。若文件系统提供
 * is_partially_uptodate 且块大小小于 folio，换算为 folio 内偏移后查询。
 * 覆盖整 folio 时不能靠 partial helper，返回 false 触发完整读。
 */
static bool filemap_range_uptodate(struct address_space *mapping,
		loff_t pos, size_t count, struct folio *folio,
		bool need_uptodate)
{
	if (folio_test_uptodate(folio))
		return true;
	/* pipes can't handle partially uptodate pages */
	/* splice/pipe 可能暴露整页，不能只保证请求子区间有效。 */
	if (need_uptodate)
		return false;
	if (!mapping->a_ops->is_partially_uptodate)
		return false;
	if (mapping->host->i_blkbits >= folio_shift(folio))
		return false;

	if (folio_pos(folio) > pos) {
		/* 请求从 folio 之前开始时，裁掉前缀并把内部偏移归零。 */
		count -= folio_pos(folio) - pos;
		pos = 0;
	} else {
		pos -= folio_pos(folio);
	}

	if (pos == 0 && count >= folio_size(folio))
		return false;

	return mapping->a_ops->is_partially_uptodate(folio, pos, count);
}

/*
 * filemap_update_page() - 让已有 folio 对当前 buffered read 范围变为可读。
 *
 * @iocb：位置/IOCB_NOWAIT/NOIO/WAITQ 策略；@mapping：目标 cache；
 * @count：请求字节；@folio：batch 持有引用；@need_uptodate：是否要求整页。
 *
 * 先共享持 invalidate_lock，防止 truncate/hole punch 与填充块映射并发；
 * 再 trylock folio。同步等待路径先放 invalidate_lock，并用 DROP 等锁，
 * 返回 AOP_TRUNCATED_PAGE 要求上层重查；异步 WAITQ 排队返回 EIOCBQUEUED。
 * 得锁后重验 mapping，范围已有效则解锁成功；NOIO/NOWAIT 不发 I/O，
 * 否则 read_folio 并等待。返回 0、-EAGAIN、-EINTR、异步状态或重试哨兵。
 */
static int filemap_update_page(struct kiocb *iocb,
		struct address_space *mapping, size_t count,
		struct folio *folio, bool need_uptodate)
{
	int error;

	/* NOWAIT 连 invalidate rwsem 都只 trylock，不能因 truncate 阻塞。 */
	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (!filemap_invalidate_trylock_shared(mapping))
			return -EAGAIN;
	} else {
		filemap_invalidate_lock_shared(mapping);
	}

	if (!folio_trylock(folio)) {
		/* NOWAIT/NOIO 不等待；普通/WAITQ 分别走同步或异步等待协议。 */
		error = -EAGAIN;
		if (iocb->ki_flags & (IOCB_NOWAIT | IOCB_NOIO))
			goto unlock_mapping;
		if (!(iocb->ki_flags & IOCB_WAITQ)) {
			filemap_invalidate_unlock_shared(mapping);
			/*
			 * This is where we usually end up waiting for a
			 * previously submitted readahead to finish.
			 */
			/*
			 * 常见情形是异步 readahead 正持 folio lock。DROP
			 * 等待消费 batch 引用且先放 invalidate_lock，避免锁序死锁；
			 * 返回重试哨兵让上层重新取得最新 cache entry。
			 */
			folio_put_wait_locked(folio, TASK_KILLABLE);
			return AOP_TRUNCATED_PAGE;
		}
		error = __folio_lock_async(folio, iocb->ki_waitq);
		if (error)
			goto unlock_mapping;
	}

	/* 得锁后 mapping==NULL 表示等待期间被 truncate，必须丢引用并重查。 */
	error = AOP_TRUNCATED_PAGE;
	if (!folio->mapping)
		goto unlock;

	error = 0;
	if (filemap_range_uptodate(mapping, iocb->ki_pos, count, folio,
				   need_uptodate))
		goto unlock;

	/* 禁止 I/O 的模式只能报告需重试，不能调用 read_folio。 */
	error = -EAGAIN;
	if (iocb->ki_flags & (IOCB_NOIO | IOCB_NOWAIT | IOCB_WAITQ))
		goto unlock;

	error = filemap_read_folio(iocb->ki_filp, mapping->a_ops->read_folio,
			folio);
	goto unlock_mapping;
unlock:
	/* 未由 read_folio 接管解锁时，本地释放 folio lock。 */
	folio_unlock(folio);
unlock_mapping:
	/* 所有路径释放 invalidate shared；重试哨兵还消费 batch folio 引用。 */
	filemap_invalidate_unlock_shared(mapping);
	if (error == AOP_TRUNCATED_PAGE)
		folio_put(folio);
	return error;
}

/*
 * filemap_create_folio() - cache miss 时分配最小 order folio、插入并同步读入。
 *
 * @iocb 提供 file/位置/flags；@fbatch 输出一项持引用且 uptodate folio。
 * NOWAIT/WAITQ 不在此同步分配读 I/O，返回 -EAGAIN。持 invalidate shared
 * 覆盖插入到 read_folio 完成，防止 truncate 已逐出 cache、尚未释放磁盘
 * 块时重新实例化并读取旧块。EEXIST 转重试哨兵；失败解锁并 put 候选。
 */
static int filemap_create_folio(struct kiocb *iocb, struct folio_batch *fbatch)
{
	struct address_space *mapping = iocb->ki_filp->f_mapping;
	struct folio *folio;
	int error;
	unsigned int min_order = mapping_min_folio_order(mapping);
	pgoff_t index;

	if (iocb->ki_flags & (IOCB_NOWAIT | IOCB_WAITQ))
		return -EAGAIN;

	/* 阶段 1：阻塞语义允许后，按 mapping 最小 order 分配未发布候选。 */
	folio = filemap_alloc_folio(mapping_gfp_mask(mapping), min_order, NULL);
	if (!folio)
		return -ENOMEM;
	if (iocb->ki_flags & IOCB_DONTCACHE)
		__folio_set_dropbehind(folio);
	/* 阶段 2：候选已分配但尚未发布，后续任一错误都走统一 put。 */

	/*
	 * Protect against truncate / hole punch. Grabbing invalidate_lock
	 * here assures we cannot instantiate and bring uptodate new
	 * pagecache folios after evicting page cache during truncate
	 * and before actually freeing blocks.	Note that we could
	 * release invalidate_lock after inserting the folio into
	 * the page cache as the locked folio would then be enough to
	 * synchronize with hole punching. But there are code paths
	 * such as filemap_update_page() filling in partially uptodate
	 * pages or ->readahead() that need to hold invalidate_lock
	 * while mapping blocks for IO so let's hold the lock here as
	 * well to keep locking rules simple.
	 */
	/*
	 * 理论上插入后 locked folio 已能与 hole punch 同步，但
	 * partial-uptodate 和 readahead 也需在建立块映射/I/O 时持 invalidate
	 * lock；这里保持统一宽临界区，降低锁规则分叉。
	 */
	filemap_invalidate_lock_shared(mapping);
	index = (iocb->ki_pos >> (PAGE_SHIFT + min_order)) << min_order;
	error = filemap_add_folio(mapping, folio, index,
			mapping_gfp_constraint(mapping, GFP_KERNEL));
	if (error == -EEXIST)
		error = AOP_TRUNCATED_PAGE;
	if (error)
		goto error;

	/* 插入完成后由 read_folio 接管 locked folio，并等待 I/O 完整发布。 */
	error = filemap_read_folio(iocb->ki_filp, mapping->a_ops->read_folio,
					folio);
	if (error)
		/* filler 失败仍由 error 标签释放 invalidate lock 与候选引用。 */
		goto error;

	/* 成功路径退出 invalidate 临界区后，把候选引用转交 batch。 */
	/* read_folio 成功后 folio 已完成 I/O，可从候选状态转交输出 batch。 */
	/* 阶段 3：仅 uptodate folio 转移到输出 batch，然后释放 invalidate lock。 */
	filemap_invalidate_unlock_shared(mapping);
	folio_batch_add(fbatch, folio);
	return 0;
error:
	/* 插入/读取失败：释放 invalidate lock，再放本函数候选引用。 */
	filemap_invalidate_unlock_shared(mapping);
	folio_put(folio);
	return error;
}

/*
 * filemap_readahead() - 命中 readahead 标记 folio 时推进异步预读窗口。
 *
 * @iocb 决定 NOIO/DONTCACHE；@file/@mapping 借用；@folio 是触发点；
 * @last_index 为本次需求后一页。NOIO 返回 -EAGAIN；否则构造 ractl，
 * DONTCACHE 传播 dropbehind，并调用 page_cache_async_ra。返回 0。
 */
static int filemap_readahead(struct kiocb *iocb, struct file *file,
		struct address_space *mapping, struct folio *folio,
		pgoff_t last_index)
{
	DEFINE_READAHEAD(ractl, file, &file->f_ra, mapping, folio->index);

	if (iocb->ki_flags & IOCB_NOIO)
		return -EAGAIN;
	/* 异步 RA 只提交窗口，不等待任何 folio 完成。 */
	if (iocb->ki_flags & IOCB_DONTCACHE)
		ractl.dropbehind = 1;
	page_cache_async_ra(&ractl, folio, last_index - folio->index);
	return 0;
}

/*
 * filemap_get_pages() - 为一次 buffered read 准备一批连续、可复制 folio。
 *
 * @iocb：file/位置/flags；@count：需求字节；@fbatch 输出持引用批次；
 * @need_uptodate：是否允许部分有效。先无锁取连续 batch；空 cache 时触发
 * sync readahead，再空则创建单 folio。末 folio 带 readahead 标记时推进
 * async RA，非 uptodate 时只允许单项并调用 update_page。
 *
 * 成功 0；若末项准备失败但前面已有完整 folio，放末项后返回 0 形成部分
 * 成功；batch 只剩失败项时返回 errno/异步状态，truncate 哨兵重试。
 */
static int filemap_get_pages(struct kiocb *iocb, size_t count,
		struct folio_batch *fbatch, bool need_uptodate)
{
	struct file *filp = iocb->ki_filp;
	struct address_space *mapping = filp->f_mapping;
	pgoff_t index = iocb->ki_pos >> PAGE_SHIFT;
	pgoff_t last_index;
	struct folio *folio;
	unsigned int flags;
	int err = 0;

	/* "last_index" is the index of the folio beyond the end of the read */
	/*
	 * 按 mapping 最小 folio 字节对齐向上取“末后一页”，确保
	 * 大 folio mapping 的读窗口边界不切断最小分配单元。
	 */
	last_index = round_up(iocb->ki_pos + count,
			mapping_min_folio_nrbytes(mapping)) >> PAGE_SHIFT;
retry:
	/* truncate/锁等待重试前允许致命信号终止，避免无界不可杀循环。 */
	if (fatal_signal_pending(current))
		return -EINTR;

	filemap_get_read_batch(mapping, index, last_index - 1, fbatch);
	if (!folio_batch_count(fbatch)) {
		/*
		 * cache miss：NOIO 直接退；NOWAIT 用 memalloc_noio 包裹 sync RA，
		 * 防止分配回收递归发 I/O，随后再次查 batch。
		 */
		DEFINE_READAHEAD(ractl, filp, &filp->f_ra, mapping, index);

		if (iocb->ki_flags & IOCB_NOIO)
			return -EAGAIN;
		if (iocb->ki_flags & IOCB_NOWAIT)
			flags = memalloc_noio_save();
		if (iocb->ki_flags & IOCB_DONTCACHE)
			ractl.dropbehind = 1;
		/* 同步 RA 只提交窗口；随后从权威 XArray 重新收集 batch。 */
		page_cache_sync_ra(&ractl, last_index - index);
		if (iocb->ki_flags & IOCB_NOWAIT)
			memalloc_noio_restore(flags);
		filemap_get_read_batch(mapping, index, last_index - 1, fbatch);
	}
	if (!folio_batch_count(fbatch)) {
		/* 同步预读仍未命中时才分配首个 folio，NOWAIT 由 helper 拒绝。 */
		/* 预读仍未提供页，阻塞路径创建并同步填充一个 folio。 */
		err = filemap_create_folio(iocb, fbatch);
		if (err == AOP_TRUNCATED_PAGE)
			goto retry;
		return err;
	}

	folio = fbatch->folios[folio_batch_count(fbatch) - 1];
	if (folio_test_readahead(folio)) {
		/* batch 最后一项是异步窗口触发器，先扩下一窗口再交付当前数据。 */
		err = filemap_readahead(iocb, filp, mapping, folio, last_index);
		if (err)
			goto err;
	}
	if (!folio_test_uptodate(folio)) {
		/*
		 * 只有 batch 唯一项可在此等待/读入；若前面已有完整页，先返回它们，
		 * 把末项留到下一 read，保持部分成功低延迟。
		 */
		if (folio_batch_count(fbatch) > 1) {
			err = -EAGAIN;
			goto err;
		}
		err = filemap_update_page(iocb, mapping, count, folio,
					  need_uptodate);
		if (err)
			goto err;
	}

	/* batch 已可读，记录最终扫描范围后交付给 read/splice 上层。 */
	trace_mm_filemap_get_pages(mapping, index, last_index - 1);
	return 0;
err:
	/*
	 * 负 errno 表示失败项引用仍归本函数，先 put；若 batch 前缀仍非空，
	 * 对外成功交付前缀。仅无前缀时传播错误或重试。
	 */
	if (err < 0)
		folio_put(folio);
	if (likely(--fbatch->nr))
		return 0;
	if (err == AOP_TRUNCATED_PAGE)
		goto retry;
	return err;
}

/*
 * pos_same_folio() - 判断两个文件字节位置是否落在同一 folio 尺度区间。
 * 用于避免顺序小块读反复 mark_accessed；纯算术，无状态副作用。
 */
static inline bool pos_same_folio(loff_t pos1, loff_t pos2, struct folio *folio)
{
	unsigned int shift = folio_shift(folio);

	return (pos1 >> shift == pos2 >> shift);
}

/*
 * filemap_end_dropbehind_read() - 读完后尽力失效 DONTCACHE clean folio。
 * dirty/writeback 不能丢；trylock 失败不阻塞，留给后续路径。成功在锁内
 * 复用 filemap_end_dropbehind。@folio 由 batch 持引用，无返回。
 */
static void filemap_end_dropbehind_read(struct folio *folio)
{
	if (!folio_test_dropbehind(folio))
		return;
	if (folio_test_writeback(folio) || folio_test_dirty(folio))
		return;
	/* 读完成快路不等待 folio lock，竞争失败就留给后续回收。 */
	if (folio_trylock(folio)) {
		filemap_end_dropbehind(folio);
		folio_unlock(folio);
	}
}

/**
 * filemap_read - Read data from the page cache.
 * @iocb: The iocb to read.
 * @iter: Destination for the data.
 * @already_read: Number of bytes already read by the caller.
 *
 * Copies data from the page cache.  If the data is not currently present,
 * uses the readahead and read_folio address_space operations to fetch it.
 *
 * Return: Total number of bytes copied, including those already read by
 * the caller.  If an error happens before any bytes are copied, returns
 * a negative error number.
 */
/*
 * 通用 buffered read 主循环。@iocb 的 ki_pos/flags 为输入输出，
 * @iter 是用户/内核目标，@already_read 可包含前置 direct I/O 已读字节。
 *
 * 每轮 filemap_get_pages 准备连续 folio，待 uptodate 后重新读 i_size，
 * 避免把 EOF 后零填充暴露给用户；对可写 mmap folio 先 flush dcache，
 * 再逐 folio copy。更新 ki_pos、readahead prev_pos、atime，并在每批末
 * 兑现 dropbehind/put 引用。已有数据时错误按部分成功规则隐藏到下次。
 * 返回总字节数、EOF 0 或尚无进展时 errno；可能分配、I/O、睡眠。
 */
ssize_t filemap_read(struct kiocb *iocb, struct iov_iter *iter,
		ssize_t already_read)
{
	struct file *filp = iocb->ki_filp;
	struct file_ra_state *ra = &filp->f_ra;
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	/* fbatch 保存本轮引用；ra 记录跨批次顺序读历史。 */
	struct folio_batch fbatch;
	int i, error = 0;
	bool writably_mapped;
	loff_t isize, end_offset;
	loff_t last_pos = ra->prev_pos;

	/*
	 * 变量地图：isize/end_offset 是本批 EOF 快照与复制上界；last_pos 驱动
	 * readahead/访问记账；writably_mapped 是复制前一次性取得的 alias 提示。
	 */
	if (unlikely(iocb->ki_pos < 0))
		return -EINVAL;
	if (unlikely(iocb->ki_pos >= inode->i_sb->s_maxbytes))
		return 0;
	if (unlikely(!iov_iter_count(iter)))
		return 0;

	iov_iter_truncate(iter, inode->i_sb->s_maxbytes - iocb->ki_pos);
	folio_batch_init(&fbatch);

	do {
		cond_resched();

		/*
		 * If we've already successfully copied some data, then we
		 * can no longer safely return -EIOCBQUEUED. Hence mark
		 * an async read NOWAIT at that point.
		 */
		/*
		 * 异步 WAITQ 只有在零进展时才能返回 EIOCBQUEUED；已有
		 * 字节必须返回部分成功，因此后续改 NOWAIT，遇阻即结束本次。
		 */
		if ((iocb->ki_flags & IOCB_WAITQ) && already_read)
			iocb->ki_flags |= IOCB_NOWAIT;

		if (unlikely(iocb->ki_pos >= i_size_read(inode)))
			break;

		error = filemap_get_pages(iocb, iter->count, &fbatch, false);
		if (error < 0)
			break;

		/*
		 * i_size must be checked after we know the pages are Uptodate.
		 *
		 * Checking i_size after the check allows us to calculate
		 * the correct value for "nr", which means the zero-filled
		 * part of the page is not copied back to userspace (unless
		 * another truncate extends the file - this is desired though).
		 */
		/*
		 * 先确保页内容有效，再采样 i_size 计算精确字节，防止
		 * truncate 后把 folio 尾部零填充当文件数据；并发 extend 后多读
		 * 新合法数据是允许结果。
		 */
		isize = i_size_read(inode);
		if (unlikely(iocb->ki_pos >= isize))
			goto put_folios;
		end_offset = min_t(loff_t, isize, iocb->ki_pos + iter->count);

		/*
		 * Once we start copying data, we don't want to be touching any
		 * cachelines that might be contended:
		 */
		/* 复制热路径前一次性读取 mapping mmap 状态，避免每页争用 cacheline。 */
		writably_mapped = mapping_writably_mapped(mapping);

		/*
		 * When a read accesses the same folio several times, only
		 * mark it as accessed the first time.
		 */
		/*
		 * 同 folio 的分段 read 只在首次 mark_accessed，避免
		 * 高频原子/LRU 更新；后续 batch 中新 folio 各标一次。
		 */
		if (!pos_same_folio(iocb->ki_pos, last_pos - 1,
				    fbatch.folios[0]))
			folio_mark_accessed(fbatch.folios[0]);

		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			struct folio *folio = fbatch.folios[i];
			size_t fsize = folio_size(folio);
			size_t offset = iocb->ki_pos & (fsize - 1);
			size_t bytes = min_t(loff_t, end_offset - iocb->ki_pos,
					     fsize - offset);
			size_t copied;

			/* 本轮只暴露 folio 内、请求末端与 EOF 三者的交集。 */
			if (end_offset < folio_pos(folio))
				break;
			if (i > 0)
				folio_mark_accessed(folio);
			/*
			 * If users can be writing to this folio using arbitrary
			 * virtual addresses, take care of potential aliasing
			 * before reading the folio on the kernel side.
			 */
			/*
			 * 用户可通过任意虚拟别名写 folio 时，内核线性映射
			 * 读取前需处理非一致 cache 架构别名，否则可能复制陈旧数据。
			 */
			if (writably_mapped)
				flush_dcache_folio(folio);

			copied = copy_folio_to_iter(folio, offset, bytes, iter);

			already_read += copied;
			iocb->ki_pos += copied;
			last_pos = iocb->ki_pos;

			if (copied < bytes) {
				/* 用户目标短拷贝以 EFAULT 收口，但此前字节仍按部分成功返回。 */
				error = -EFAULT;
				break;
			}
		}
put_folios:
		/* 无论 EOF、fault 或成功，逐项兑现 dropbehind 并释放 batch 引用。 */
		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			struct folio *folio = fbatch.folios[i];

			filemap_end_dropbehind_read(folio);
			folio_put(folio);
		}
		folio_batch_init(&fbatch);
	} while (iov_iter_count(iter) && iocb->ki_pos < isize && !error);

	file_accessed(filp);
	/* 最终位置与 RA 历史同步；部分数据优先于本批后续 errno。 */
	ra->prev_pos = last_pos;
	return already_read ? already_read : error;
}
EXPORT_SYMBOL_GPL(filemap_read);

/*
 * kiocb_write_and_wait() - direct I/O 前按 iocb 策略处理重叠页缓存写回。
 * @count 与 ki_pos 形成闭区间。NOWAIT 只做保守 needs_writeback 查询，
 * 可能阻塞则 -EAGAIN；阻塞模式执行 write-and-wait。返回 0 或 errno。
 */
int kiocb_write_and_wait(struct kiocb *iocb, size_t count)
{
	struct address_space *mapping = iocb->ki_filp->f_mapping;
	loff_t pos = iocb->ki_pos;
	loff_t end = pos + count - 1;

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (filemap_range_needs_writeback(mapping, pos, end))
			/* NOWAIT 只做瞬时探测，任何潜在等待都折为 -EAGAIN。 */
			return -EAGAIN;
		return 0;
	}

	return filemap_write_and_wait_range(mapping, pos, end);
}
EXPORT_SYMBOL_GPL(kiocb_write_and_wait);

/*
 * filemap_invalidate_pages() - direct write 前写回并失效重叠 page cache。
 *
 * @pos/@end 为闭区间；@nowait 为 true 时只要近期看到页就 -EAGAIN，
 * 避免 invalidate 阻塞。阻塞模式先 write-and-wait，再调用
 * invalidate_inode_pages2_range。返回 0 或写回/失效 errno。
 */
int filemap_invalidate_pages(struct address_space *mapping,
			     loff_t pos, loff_t end, bool nowait)
{
	int ret;

	if (nowait) {
		/* we could block if there are any pages in the range */
		/* 任何 cache 页都可能要求锁/等待，NOWAIT 保守拒绝。 */
		if (filemap_range_has_page(mapping, pos, end))
			return -EAGAIN;
	} else {
		ret = filemap_write_and_wait_range(mapping, pos, end);
		if (ret)
			return ret;
	}

	/*
	 * After a write we want buffered reads to be sure to go to disk to get
	 * the new data.  We invalidate clean cached page from the region we're
	 * about to write.  We do this *before* the write so that we can return
	 * without clobbering -EIOCBQUEUED from ->direct_IO().
	 */
	/*
	 * direct write 后 buffered read 必须重新从介质取新数据，
	 * 所以提前失效 clean cache。放在 direct_IO 前，异步提交返回的
	 * -EIOCBQUEUED 不会被事后 invalidate errno 覆盖。
	 */
	return invalidate_inode_pages2_range(mapping, pos >> PAGE_SHIFT,
					     end >> PAGE_SHIFT);
}

/*
 * kiocb_invalidate_pages() - 用 kiocb 位置/flags 包装 direct-write cache 失效。
 * @count 转闭区间；NOWAIT 传播。返回 filemap_invalidate_pages 结果。
 */
int kiocb_invalidate_pages(struct kiocb *iocb, size_t count)
{
	struct address_space *mapping = iocb->ki_filp->f_mapping;

	return filemap_invalidate_pages(mapping, iocb->ki_pos,
					iocb->ki_pos + count - 1,
					iocb->ki_flags & IOCB_NOWAIT);
}
EXPORT_SYMBOL_GPL(kiocb_invalidate_pages);

/**
 * generic_file_read_iter - generic filesystem read routine
 * @iocb:	kernel I/O control block
 * @iter:	destination for the data read
 *
 * This is the "read_iter()" routine for all filesystems
 * that can use the page cache directly.
 *
 * The IOCB_NOWAIT flag in iocb->ki_flags indicates that -EAGAIN shall
 * be returned when no data can be read without waiting for I/O requests
 * to complete; it doesn't prevent readahead.
 *
 * The IOCB_NOIO flag in iocb->ki_flags indicates that no new I/O
 * requests shall be made for the read or for readahead.  When no data
 * can be read, -EAGAIN shall be returned.  When readahead would be
 * triggered, a partial, possibly empty read shall be returned.
 *
 * Return:
 * * number of bytes copied, even for partial reads
 * * negative error code (or 0 if IOCB_NOIO) if nothing was read
 */
/*
 * 支持 page cache 的文件系统通用 read_iter。DIRECT 时先等待
 * 重叠 writeback，调用 a_ops->direct_IO，并按实际消费修正 iov_iter；
 * 异步 EIOCBQUEUED 保留 iterator ownership。短 DIO 若未到 EOF且非 DAX，
 * 余量回退 filemap_read；DAX 不支持 page-cache fallback。
 *
 * NOWAIT 允许 readahead 但无可立即读数据时 -EAGAIN；NOIO 禁止新 I/O，
 * 可返回部分或 0。返回总字节/errno，更新 ki_pos 与 atime。
 */
ssize_t
generic_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	size_t count = iov_iter_count(iter);
	ssize_t retval = 0;

	if (!count)
		return 0; /* skip atime */
	/* 零长度读不更新 atime，保持 POSIX 无访问语义。 */

	if (iocb->ki_flags & IOCB_DIRECT) {
		struct file *file = iocb->ki_filp;
		struct address_space *mapping = file->f_mapping;
		struct inode *inode = mapping->host;

		retval = kiocb_write_and_wait(iocb, count);
		if (retval < 0)
			return retval;
		file_accessed(file);

		/* direct_IO 可能同步返回字节/errno，或异步接管并返回 EIOCBQUEUED。 */
		retval = mapping->a_ops->direct_IO(iocb, iter);
		if (retval >= 0) {
			iocb->ki_pos += retval;
			count -= retval;
		}
		if (retval != -EIOCBQUEUED)
			iov_iter_revert(iter, count - iov_iter_count(iter));

		/*
		 * Btrfs can have a short DIO read if we encounter
		 * compressed extents, so if there was an error, or if
		 * we've already read everything we wanted to, or if
		 * there was a short read because we hit EOF, go ahead
		 * and return.  Otherwise fallthrough to buffered io for
		 * the rest of the read.  Buffered reads will not work for
		 * DAX files, so don't bother trying.
		 */
		/*
		 * btrfs 压缩 extent 可导致非 EOF 短 DIO，需要 buffered
		 * 补余量；真实错误、已满足、EOF 或 DAX 则直接返回。
		 */
		if (retval < 0 || !count || IS_DAX(inode))
			return retval;
		if (iocb->ki_pos >= i_size_read(inode))
			return retval;
	}

	return filemap_read(iocb, iter, retval);
}
EXPORT_SYMBOL(generic_file_read_iter);

/*
 * Splice subpages from a folio into a pipe.
 */
/*
 * 把 folio 从 @fpos 起最多 @size 字节拆成 PAGE_SIZE pipe_buffer。
 * 每个成功槽位持一份 folio 引用（由 pipe buffer release 归还），设置
 * page/offset/len 后推进 pipe head。遇 pipe 满停止，返回实际 splice
 * 字节；不复制数据，因而是零拷贝引用转移。
 */
size_t splice_folio_into_pipe(struct pipe_inode_info *pipe,
			      struct folio *folio, loff_t fpos, size_t size)
{
	struct page *page;
	size_t spliced = 0, offset = offset_in_folio(folio, fpos);

	page = folio_page(folio, offset / PAGE_SIZE);
	/* 文件偏移先归一化为 folio 内 page/offset，再拆 pipe 基本页片段。 */
	size = min(size, folio_size(folio) - offset);
	offset %= PAGE_SIZE;

	while (spliced < size && !pipe_is_full(pipe)) {
		struct pipe_buffer *buf = pipe_head_buf(pipe);
		size_t part = min_t(size_t, PAGE_SIZE - offset, size - spliced);

		/* 槽描述当前基本页内片段，首段之后 offset 恒归零。 */
		*buf = (struct pipe_buffer) {
			.ops	= &page_cache_pipe_buf_ops,
			.page	= page,
			.offset	= offset,
			.len	= part,
		};
		/* 每个 pipe 槽独立持有 folio 引用，允许原 batch 引用随后释放。 */
		folio_get(folio);
		pipe->head++;
		page++;
		spliced += part;
		offset = 0;
	}

	return spliced;
}

/**
 * filemap_splice_read -  Splice data from a file's pagecache into a pipe
 * @in: The file to read from
 * @ppos: Pointer to the file position to read from
 * @pipe: The pipe to splice into
 * @len: The amount to splice
 * @flags: The SPLICE_F_* flags
 *
 * This function gets folios from a file's pagecache and splices them into the
 * pipe.  Readahead will be called as necessary to fill more folios.  This may
 * be used for blockdevs also.
 *
 * Return: On success, the number of bytes read will be returned and *@ppos
 * will be updated if appropriate; 0 will be returned if there is no more data
 * to be read; -EAGAIN will be returned if the pipe had no space, and some
 * other negative error code will be returned on error.  A short read may occur
 * if the pipe has insufficient space, we reach the end of the data or we hit a
 * hole.
 */
/*
 * 从 @in page cache 把最多 @len 字节零拷贝挂入 @pipe，并更新
 * *@ppos。先按 pipe 空槽把请求截为可容纳页数；每轮用 filemap_get_pages
 * 且 need_uptodate=true（pipe 会暴露整页，不能部分有效），重读 i_size，
 * 处理 dcache alias，再为子页建立 pipe_buffer 引用。
 *
 * 返回实际字节优先；零进展时返回 EOF 0、pipe 无空间 -EAGAIN 或其他 errno。
 * pipe 满、EOF、hole 可短读。所有 batch 引用在 out 释放，pipe 自己持有
 * 已 splice 页的独立引用；更新 atime 和 readahead prev_pos。
 */
ssize_t filemap_splice_read(struct file *in, loff_t *ppos,
			    struct pipe_inode_info *pipe,
			    size_t len, unsigned int flags)
{
	struct folio_batch fbatch;
	struct kiocb iocb;
	size_t total_spliced = 0, used, npages;
	loff_t isize, end_offset;
	/* isize/end_offset 固定本轮 EOF 边界，writably_mapped 决定是否需要 dcache 同步。 */
	bool writably_mapped;
	int i, error = 0;

	if (unlikely(*ppos >= in->f_mapping->host->i_sb->s_maxbytes))
		return 0;

	init_sync_kiocb(&iocb, in);
	iocb.ki_pos = *ppos;

	/* Work out how much data we can actually add into the pipe */
	/* 每个 pipe slot 最多承载一页，先按空 slot 限制总长度。 */
	used = pipe_buf_usage(pipe);
	npages = max_t(ssize_t, pipe->max_usage - used, 0);
	len = min_t(size_t, len, npages * PAGE_SIZE);

	folio_batch_init(&fbatch);

	do {
		cond_resched();

		/* 每批重读 i_size，避免并发 truncate 后 splice 旧 EOF 外数据。 */
		if (*ppos >= i_size_read(in->f_mapping->host))
			break;

		iocb.ki_pos = *ppos;
		error = filemap_get_pages(&iocb, len, &fbatch, true);
		if (error < 0)
			break;

		/*
		 * i_size must be checked after we know the pages are Uptodate.
		 *
		 * Checking i_size after the check allows us to calculate
		 * the correct value for "nr", which means the zero-filled
		 * part of the page is not copied back to userspace (unless
		 * another truncate extends the file - this is desired though).
		 */
	/* 同 buffered read，页有效后再采样 EOF，避免暴露 truncate 尾零。 */
		isize = i_size_read(in->f_mapping->host);
		if (unlikely(*ppos >= isize))
			break;
		end_offset = min_t(loff_t, isize, *ppos + len);

		/*
		 * Once we start copying data, we don't want to be touching any
		 * cachelines that might be contended:
		 */
	/* 进入逐 folio 热循环前一次读取 writable mmap 提示。 */
		writably_mapped = mapping_writably_mapped(in->f_mapping);

		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			struct folio *folio = fbatch.folios[i];
			size_t n;

			if (folio_pos(folio) >= end_offset)
				goto out;
			folio_mark_accessed(folio);

			/*
			 * If users can be writing to this folio using arbitrary
			 * virtual addresses, take care of potential aliasing
			 * before reading the folio on the kernel side.
			 */
			/* 零拷贝给 pipe 前同样需解决用户可写别名的 cache 一致性。 */
			if (writably_mapped)
				flush_dcache_folio(folio);

			n = min_t(loff_t, len, isize - *ppos);
			n = splice_folio_into_pipe(pipe, folio, *ppos, n);
			if (!n)
				goto out;
			len -= n;
			total_spliced += n;
			*ppos += n;
			/* 文件位置、RA 历史与 pipe 已发布字节同步推进。 */
			in->f_ra.prev_pos = *ppos;
			if (pipe_is_full(pipe))
				goto out;
		}

		folio_batch_release(&fbatch);
	} while (len);

out:
	/* 收口：部分成功优先于后续错误，并统一更新 atime。 */
	/* batch 临时引用统一释放；pipe buffer 已各自 folio_get，不受此处影响。 */
	folio_batch_release(&fbatch);
	file_accessed(in);

	return total_spliced ? total_spliced : error;
}
EXPORT_SYMBOL(filemap_splice_read);

/*
 * folio_seek_hole_data() - 在一个 entry 覆盖范围内细分 SEEK_DATA/HOLE。
 *
 * value 或整 folio uptodate 视为数据：seek_data 返回 start，seek_hole
 * 返回 entry end；无 partial helper 的非 uptodate folio视为 hole。
 * 需要按块细查时暂停 xas、退出 RCU、锁 folio并重验 mapping，再逐
 * i_blocksize 调 is_partially_uptodate。返回找到位置或 entry end。
 */
static inline loff_t folio_seek_hole_data(struct xa_state *xas,
		struct address_space *mapping, struct folio *folio,
		loff_t start, loff_t end, bool seek_data)
{
	const struct address_space_operations *ops = mapping->a_ops;
	size_t offset, bsz = i_blocksize(mapping->host);

	if (xa_is_value(folio) || folio_test_uptodate(folio))
		return seek_data ? start : end;
	if (!ops->is_partially_uptodate)
		return seek_data ? end : start;

	/*
	 * folio_lock 可睡眠，不能持 RCU；xas_pause 保存续扫位置。得锁后
	 * mapping 重验防 truncate，出口恢复 RCU 供外层继续。
	 */
	xas_pause(xas);
	rcu_read_unlock();
	folio_lock(folio);
	if (unlikely(folio->mapping != mapping))
		goto unlock;

	offset = offset_in_folio(folio, start) & ~(bsz - 1);

	do {
		/* 逐块比较 partial-uptodate 结果与 seek 目标，首次相等即命中。 */
		if (ops->is_partially_uptodate(folio, offset, bsz) ==
							seek_data)
			break;
		start = (start + bsz) & ~((u64)bsz - 1);
		offset += bsz;
	} while (offset < folio_size(folio));
	/* 块级扫描结束后统一解锁，并重新进入外层要求的 RCU 临界区。 */
unlock:
	folio_unlock(folio);
	rcu_read_lock();
	return start;
}

/*
 * seek_folio_size() - 返回 entry 覆盖字节数。
 * value 由 XArray order 推算，真实 folio 用 folio_size；纯查询。
 */
static inline size_t seek_folio_size(struct xa_state *xas, struct folio *folio)
{
	if (xa_is_value(folio))
		return PAGE_SIZE << xas_get_order(xas);
	return folio_size(folio);
}

/**
 * mapping_seek_hole_data - Seek for SEEK_DATA / SEEK_HOLE in the page cache.
 * @mapping: Address space to search.
 * @start: First byte to consider.
 * @end: Limit of search (exclusive).
 * @whence: Either SEEK_HOLE or SEEK_DATA.
 *
 * If the page cache knows which blocks contain holes and which blocks
 * contain data, your filesystem can use this function to implement
 * SEEK_HOLE and SEEK_DATA.  This is useful for filesystems which are
 * entirely memory-based such as tmpfs, and filesystems which support
 * unwritten extents.
 *
 * Return: The requested offset on success, or -ENXIO if @whence specifies
 * SEEK_DATA and there is no data after @start.  There is an implicit hole
 * after @end - 1, so SEEK_HOLE returns @end if all the bytes between @start
 * and @end contain data.
 */
/*
 * 用 page cache 实现 SEEK_DATA/SEEK_HOLE，适合 tmpfs 或能通过
 * partial-uptodate 表示 unwritten extent 的文件系统。@end 为 exclusive。
 *
 * RCU 下按 entry 升序扫描；entry 前的 gap 是 hole，entry 内由
 * folio_seek_hole_data 按块细分。真实 folio 引用严格 put；large entry
 * 手工把 xas 跳到末后。DATA 未找到返回 -ENXIO；HOLE 可返回隐式 EOF
 * hole 的 @end。结果是并发弱一致快照。
 */
loff_t mapping_seek_hole_data(struct address_space *mapping, loff_t start,
		loff_t end, int whence)
{
	XA_STATE(xas, &mapping->i_pages, start >> PAGE_SHIFT);
	pgoff_t max = (end - 1) >> PAGE_SHIFT;
	bool seek_data = (whence == SEEK_DATA);
	struct folio *folio;

	if (end <= start)
		return -ENXIO;

	rcu_read_lock();
	/* 阶段 1：entry 前 gap 天然是 hole；DATA 查询则把 start 推到 entry。 */
	while ((folio = find_get_entry(&xas, max, XA_PRESENT))) {
		loff_t pos = (u64)xas.xa_index << PAGE_SHIFT;
		size_t seek_size;

		if (start < pos) {
			if (!seek_data)
				goto unlock;
			start = pos;
		}

		seek_size = seek_folio_size(&xas, folio);
		/* 阶段 2：在 entry 内细查，并把 large entry 游标跨到覆盖范围末后。 */
		pos = round_up((u64)pos + 1, seek_size);
		start = folio_seek_hole_data(&xas, mapping, folio, start, pos,
				seek_data);
		if (start < pos)
			goto unlock;
		if (start >= end)
			break;
		/* large entry 需越过 sibling 槽，避免重复检查同一 folio。 */
		if (seek_size > PAGE_SIZE)
			xas_set(&xas, pos >> PAGE_SHIFT);
		if (!xa_is_value(folio))
			folio_put(folio);
	}
	/* 自然耗尽时 DATA 未命中；HOLE 则保留隐式 EOF hole 位置。 */
	if (seek_data)
		start = -ENXIO;
unlock:
	/* 阶段 3：统一退出 RCU，并归还最后一个尚未在循环尾释放的真实 folio。 */
	rcu_read_unlock();
	if (folio && !xa_is_value(folio))
		folio_put(folio);
	if (start > end)
		return end;
	return start;
}

#ifdef CONFIG_MMU
#define MMAP_LOTSAMISS  (100)
/*
 * lock_folio_maybe_drop_mmap - lock the page, possibly dropping the mmap_lock
 * @vmf - the vm_fault for this fault.
 * @folio - the folio to lock.
 * @fpin - the pointer to the file we may pin (or is already pinned).
 *
 * This works similar to lock_folio_or_retry in that it can drop the
 * mmap_lock.  It differs in that it actually returns the folio locked
 * if it returns 1 and 0 if it couldn't lock the folio.  If we did have
 * to drop the mmap_lock then fpin will point to the pinned file and
 * needs to be fput()'ed at a later point.
 */
/*
 * fault 路径尝试锁 folio；快路返回 1 且 locked。竞争时
 * RETRY_NOWAIT 返回 0 但保留 fault lock；其余可通过
 * maybe_unlock_mmap_for_io 释放 mmap/per-VMA lock 并用 @fpin 持 file，
 * 再 killable/不可中断锁 folio。返回 0 表示上层必须 VM_FAULT_RETRY，
 * @fpin 非 NULL 由上层 fput。
 */
static int lock_folio_maybe_drop_mmap(struct vm_fault *vmf, struct folio *folio,
				     struct file **fpin)
{
	if (folio_trylock(folio))
		return 1;

	/*
	 * NOTE! This will make us return with VM_FAULT_RETRY, but with
	 * the fault lock still held. That's how FAULT_FLAG_RETRY_NOWAIT
	 * is supposed to work. We have way too many special cases..
	 */
	/*
	 * RETRY_NOWAIT 明确要求不睡眠也不释放 fault lock；虽然返回
	 * 上层 RETRY，这个特殊锁状态必须由 fault 核心按 flags 解释。
	 */
	if (vmf->flags & FAULT_FLAG_RETRY_NOWAIT)
		return 0;

	*fpin = maybe_unlock_mmap_for_io(vmf, *fpin);
	if (vmf->flags & FAULT_FLAG_KILLABLE) {
		if (__folio_lock_killable(folio)) {
			/*
			 * We didn't have the right flags to drop the
			 * fault lock, but all fault_handlers only check
			 * for fatal signals if we return VM_FAULT_RETRY,
			 * so we need to drop the fault lock here and
			 * return 0 if we don't have a fpin.
			 */
			/*
			 * killable 中断时 fault handler 只在 RETRY 检查致命
			 * 信号；若此前未能通过 fpin 释放锁，此处必须显式释放再返回。
			 */
			if (*fpin == NULL)
				release_fault_lock(vmf);
			return 0;
		}
	} else
		__folio_lock(folio);

	return 1;
}

/*
 * Synchronous readahead happens when we don't even find a page in the page
 * cache at all.  We don't want to perform IO under the mmap sem, so if we have
 * to drop the mmap sem we return the file that was pinned in order for us to do
 * that.  If we didn't pin a file then we return NULL.  The file that is
 * returned needs to be fput()'ed when we're done with it.
 */
/*
 * page-cache 完全 miss 的 mmap fault 同步预读策略。I/O 不应在
 * mmap_lock 下执行；需要释放时 maybe_unlock_mmap_for_io 返回持有引用
 * file，调用者最终 fput，否则返回 NULL。
 *
 * VM_RAND_READ 禁用普通预读；VM_SEQ_READ 整窗口前推；普通映射以
 * mmap_miss 抑制低命中预读并做 fault-around；VM_HUGEPAGE 强制按最大
 * 2MB order 预读；VM_EXEC 可用架构偏好 folio order，但限制在 VMA 内且
 * 不做 async 扩窗。返回值只表达 file 引用/锁已释放状态。
 */
static struct file *do_sync_mmap_readahead(struct vm_fault *vmf)
{
	struct file *file = vmf->vma->vm_file;
	struct file_ra_state *ra = &file->f_ra;
	struct address_space *mapping = file->f_mapping;
	DEFINE_READAHEAD(ractl, file, ra, mapping, vmf->pgoff);
	/* ractl 把 fault、file RA 状态与 mapping 绑定为一次预读决策。 */
	struct file *fpin = NULL;
	vm_flags_t vm_flags = vmf->vma->vm_flags;
	bool force_thp_readahead = false;
	unsigned int thp_order = 0;
	unsigned short mmap_miss;

	/* Use the readahead code, even if readahead is disabled */
	/* VM_HUGEPAGE 是显式用户策略，即使普通 ra_pages 为 0 也尝试。 */
	if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) && (vm_flags & VM_HUGEPAGE)) {
		/*
		 * Cap max THP order at 2MB: this is the common PMD-sized
		 * hugepage size, and it avoids memory pressure from very
		 * large forced readahead when mapping_max_folio_order() is
		 * high (for example, 128MB with 64K base pages on arm64).
		 */
		/*
		 * 强制 THP 上限 2MB，既匹配常见 PMD 映射收益，又避免
		 * arm64 64K 基页等配置按 mapping max order 一次拉入 128MB。
		 */
		if (mapping_large_folio_support(mapping)) {
			force_thp_readahead = true;
			thp_order = min_t(unsigned int,
					  mapping_max_folio_order(mapping),
					  get_order(SZ_2M));
		}
	}

	if (!force_thp_readahead) {
		/*
		 * If we don't want any read-ahead, don't bother.
		 * VM_EXEC case below is already intended for random access.
		 */
		/* 纯 RAND_READ 且非 EXEC 不做预读；EXEC 有独立布局策略。 */
		if ((vm_flags & (VM_RAND_READ | VM_EXEC)) == VM_RAND_READ)
			return fpin;

		if (!ra->ra_pages)
			return fpin;

		if (vm_flags & VM_SEQ_READ) {
			/* 顺序/强制预读提交前允许释放 mmap_lock，避免 I/O 长持锁。 */
			fpin = maybe_unlock_mmap_for_io(vmf, fpin);
			page_cache_sync_ra(&ractl, ra->ra_pages);
			return fpin;
		}
	}

	if (!(vm_flags & (VM_SEQ_READ | VM_EXEC))) {
		/* Avoid banging the cache line if not needed */
		/* 仅会使用 miss 自适应的映射才读写该共享 cacheline。 */
		mmap_miss = READ_ONCE(ra->mmap_miss);
		if (mmap_miss < MMAP_LOTSAMISS * 10)
			WRITE_ONCE(ra->mmap_miss, ++mmap_miss);

		/*
		 * Do we miss much more than hit in this file? If so,
		 * stop bothering with read-ahead. It will only hurt.
		 */
		/* miss 长期多于 hit 时预读只制造无用 I/O/回收，停止扩窗。 */
		if (mmap_miss > MMAP_LOTSAMISS)
			return fpin;
	}

	if (force_thp_readahead) {
		unsigned long folio_nr_pages = 1UL << thp_order;

		fpin = maybe_unlock_mmap_for_io(vmf, fpin);
		ractl._index &= ~(folio_nr_pages - 1);
		ra->size = folio_nr_pages;
		/*
		 * Fetch two folios so we get the chance to actually
		 * readahead, unless we've been told not to.
		 */
		/*
		 * 默认取两个大 folio，首个满足当前 fault，第二个形成
		 * 真正 readahead；RAND_READ 只取当前所需一个。
		 */
		if (!(vm_flags & VM_RAND_READ))
			ra->size *= 2;
		ra->async_size = folio_nr_pages;
		ra->order = thp_order;
		page_cache_ra_order(&ractl, ra);
		return fpin;
	}

	if (vm_flags & VM_EXEC) {
		/*
		 * Allow arch to request a preferred minimum folio order for
		 * executable memory. This can often be beneficial to
		 * performance if (e.g.) arm64 can contpte-map the folio.
		 * Executable memory rarely benefits from readahead, due to its
		 * random access nature, so set async_size to 0.
		 *
		 * Limit to the boundaries of the VMA to avoid reading in any
		 * pad that might exist between sections, which would be a waste
		 * of memory.
		 */
		/*
		 * 可执行映射常随机跳转，重点是架构可用 contpte 的 folio
		 * order 而非前瞻 I/O；窗口夹在 VMA 段内，避免读入节间 padding。
		 */
		struct vm_area_struct *vma = vmf->vma;
		unsigned long start = vma->vm_pgoff;
		unsigned long end = start + vma_pages(vma);
		unsigned long ra_end;

		ra->order = exec_folio_order();
		ra->start = round_down(vmf->pgoff, 1UL << ra->order);
		ra->start = max(ra->start, start);
		/* exec RA 对齐到目标 order，并同时裁剪文件/VMA 两侧边界。 */
		ra_end = round_up(ra->start + ra->ra_pages, 1UL << ra->order);
		ra_end = min(ra_end, end);
		ra->size = ra_end - ra->start;
		ra->async_size = 0;
	} else {
		/*
		 * mmap read-around
		 */
		/* 普通 fault 以当前位置为中心读 around，后四分之一作异步触发。 */
		ra->start = max_t(long, 0, vmf->pgoff - ra->ra_pages / 2);
		ra->size = ra->ra_pages;
		ra->async_size = ra->ra_pages / 4;
		ra->order = 0;
	}

	fpin = maybe_unlock_mmap_for_io(vmf, fpin);
	ractl._index = ra->start;
	page_cache_ra_order(&ractl, ra);
	return fpin;
}

/*
 * Asynchronous readahead happens when we find the page and PG_readahead,
 * so we want to possibly extend the readahead further.  We return the file that
 * was pinned if we have to drop the mmap_lock in order to do IO.
 */
/*
 * 命中 PG_readahead 触发点时推进异步 mmap 预读。RAND_READ 或
 * ra_pages=0 快退。普通、未锁 folio 命中会递减 mmap_miss，与 sync miss
 * 增量对称；锁定页多半是同一 fault 竞态，不能重复减。SEQ/EXEC 两侧都
 * 不维护 miss。需要 I/O 时可能释放 mmap lock 并返回持有 file。
 */
static struct file *do_async_mmap_readahead(struct vm_fault *vmf,
					    struct folio *folio)
{
	struct file *file = vmf->vma->vm_file;
	struct file_ra_state *ra = &file->f_ra;
	DEFINE_READAHEAD(ractl, file, ra, file->f_mapping, vmf->pgoff);
	struct file *fpin = NULL;
	unsigned short mmap_miss;

	/* If we don't want any read-ahead, don't bother */
	/* 策略明确禁用预读时直接返回，避免无收益的窗口计算和 I/O。 */
	if (vmf->vma->vm_flags & VM_RAND_READ || !ra->ra_pages)
		return fpin;

	/*
	 * If the folio is locked, we're likely racing against another fault.
	 * Don't touch the mmap_miss counter to avoid decreasing it multiple
	 * times for a single folio and break the balance with mmap_miss
	 * increase in do_sync_mmap_readahead().
	 *
	 * VM_SEQ_READ and VM_EXEC mappings skip the mmap_miss increment in
	 * do_sync_mmap_readahead(), so skip the decrement here as well to
	 * keep the counter symmetric.
	 */
	/*
	 * 同一 locked folio 的多个 fault 不能各算一次 hit；SEQ/EXEC
	 * 在同步侧未加 miss，这里也不减，保持计数器统计口径对称。
	 */
	if (likely(!folio_test_locked(folio)) &&
	    !(vmf->vma->vm_flags & (VM_SEQ_READ | VM_EXEC))) {
		mmap_miss = READ_ONCE(ra->mmap_miss);
		if (mmap_miss)
			WRITE_ONCE(ra->mmap_miss, --mmap_miss);
	}

	if (folio_test_readahead(folio)) {
		/* 真正提交异步 RA 前可释放 mmap_lock，降低 I/O 期间锁竞争。 */
		fpin = maybe_unlock_mmap_for_io(vmf, fpin);
		page_cache_async_ra(&ractl, folio, ra->ra_pages);
	}
	return fpin;
}

/*
 * filemap_fault_recheck_pte_none() - 为 mlocked COW 特例在页表锁下复核 PTE。
 *
 * 无锁观察 pte none 可能恰逢 NUMA/change_pte_range 的
 * read-clear-modify-write 临时窗口；若把它当真实缺页，会在 VM_LOCKED
 * 区域制造意外 major fault。仅 ORIG_PTE_VALID+VM_LOCKED 才做两级复核，
 * 先 lockless 降低锁频率，再持 ptl 确认。非 none 返回 NOPAGE。
 */
static vm_fault_t filemap_fault_recheck_pte_none(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	vm_fault_t ret = 0;
	pte_t *ptep;

	/*
	 * We might have COW'ed a pagecache folio and might now have an mlocked
	 * anon folio mapped. The original pagecache folio is not mlocked and
	 * might have been evicted. During a read+clear/modify/write update of
	 * the PTE, such as done in do_numa_page()/change_pte_range(), we
	 * temporarily clear the PTE under PT lock and might detect it here as
	 * "none" when not holding the PT lock.
	 *
	 * Not rechecking the PTE under PT lock could result in an unexpected
	 * major fault in an mlock'ed region. Recheck only for this special
	 * scenario while holding the PT lock, to not degrade non-mlocked
	 * scenarios. Recheck the PTE without PT lock firstly, thereby reducing
	 * the number of times we hold PT lock.
	 */
	/*
	 * COW 后映射可能已变匿名 mlocked folio，原 page-cache folio
	 * 可被回收。只有此特例值得付页表锁成本；普通 fault 保持快路。
	 */
	if (!(vma->vm_flags & VM_LOCKED))
		return 0;

	if (!(vmf->flags & FAULT_FLAG_ORIG_PTE_VALID))
		return 0;

	ptep = pte_offset_map_ro_nolock(vma->vm_mm, vmf->pmd, vmf->address,
					&vmf->ptl);
	if (unlikely(!ptep))
		return VM_FAULT_NOPAGE;

	/* 阶段 2：先无锁快查；仍为 none 才持 ptl 排除临时清 PTE 窗口。 */
	if (unlikely(!pte_none(ptep_get_lockless(ptep)))) {
		ret = VM_FAULT_NOPAGE;
	} else {
		spin_lock(vmf->ptl);
		if (unlikely(!pte_none(ptep_get(ptep))))
			ret = VM_FAULT_NOPAGE;
		/* 持 ptl 的结果是最终判定，离锁后只保留 vm_fault_t 值。 */
		spin_unlock(vmf->ptl);
	}
	pte_unmap(ptep);
	return ret;
}

/**
 * filemap_fault - read in file data for page fault handling
 * @vmf:	struct vm_fault containing details of the fault
 *
 * filemap_fault() is invoked via the vma operations vector for a
 * mapped memory region to read in file data during a page fault.
 *
 * The goto's are kind of ugly, but this streamlines the normal case of having
 * it in the page cache, and handles the special cases reasonably without
 * having a lot of duplicated code.
 *
 * vma->vm_mm->mmap_lock must be held on entry.
 *
 * If our return value has VM_FAULT_RETRY set, it's because the mmap_lock
 * may be dropped before doing I/O or by lock_folio_maybe_drop_mmap().
 *
 * If our return value does not have VM_FAULT_RETRY set, the mmap_lock
 * has not been released.
 *
 * We never return with VM_FAULT_RETRY and a bit from VM_FAULT_ERROR set.
 *
 * Return: bitwise-OR of %VM_FAULT_ codes.
 */
/*
 * 文件 mmap 主 fault 状态机。先检查 EOF并查 cache：命中可触发
 * async RA；miss 计 major fault、sync RA，并在 invalidate shared 下创建。
 * 锁 folio 可能释放 mmap lock，此时必须返回 RETRY 让上层重找 VMA。
 *
 * 得锁后重验 mapping 与 i_size；非 uptodate 在持 invalidate lock 下同步
 * 重读一次。成功把 index 对应 subpage 放入 vmf->page，返回 LOCKED（folio
 * 引用/锁交给 fault 核心）；I/O/EOF 失败 SIGBUS，分配失败 OOM。
 * 所有 retry 路径释放 folio、invalidate lock、fpin，且 RETRY 不与 ERROR 位并存。
 */
vm_fault_t filemap_fault(struct vm_fault *vmf)
{
	int error;
	struct file *file = vmf->vma->vm_file;
	struct file *fpin = NULL;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	pgoff_t max_idx, index = vmf->pgoff;
	/* folio/fpin 分别追踪 cache 引用与可能临时 pin 的 file 引用。 */
	struct folio *folio;
	vm_fault_t ret = 0;
	bool mapping_locked = false;

	max_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	if (unlikely(index >= max_idx))
		return VM_FAULT_SIGBUS;

	trace_mm_filemap_fault(mapping, index);

	/*
	 * Do we have something in the page cache already?
	 */
	/* 无锁查找先区分 minor cache hit 与需要同步预读的 major miss。 */
	folio = filemap_get_folio(mapping, index);
	if (likely(!IS_ERR(folio))) {
		/*
		 * We found the page, so try async readahead before waiting for
		 * the lock.
		 */
		/* 在可能等待 folio lock 前先推进异步窗口，降低后续 fault 延迟。 */
		if (!(vmf->flags & FAULT_FLAG_TRIED))
			fpin = do_async_mmap_readahead(vmf, folio);
		if (unlikely(!folio_test_uptodate(folio))) {
			filemap_invalidate_lock_shared(mapping);
			mapping_locked = true;
		}
	} else {
		ret = filemap_fault_recheck_pte_none(vmf);
		if (unlikely(ret))
			return ret;

		/* No page in the page cache at all */
		/* 真正 cache miss 计入进程/mm 的 major fault 统计。 */
		count_vm_event(PGMAJFAULT);
		count_memcg_event_mm(vmf->vma->vm_mm, PGMAJFAULT);
		ret = VM_FAULT_MAJOR;
		fpin = do_sync_mmap_readahead(vmf);
retry_find:
		/*
		 * See comment in filemap_create_folio() why we need
		 * invalidate_lock
		 */
		/* 创建/填充块映射必须与 truncate/hole punch 共享锁协调。 */
		if (!mapping_locked) {
			filemap_invalidate_lock_shared(mapping);
			mapping_locked = true;
		}
		folio = __filemap_get_folio(mapping, index,
					  FGP_CREAT|FGP_FOR_MMAP,
					  vmf->gfp_mask);
		if (IS_ERR(folio)) {
			/* 创建失败且 mmap lock 已释放时只能走 fault 重试收口。 */
			if (fpin)
				goto out_retry;
			filemap_invalidate_unlock_shared(mapping);
			return VM_FAULT_OOM;
		}
	}

	if (!lock_folio_maybe_drop_mmap(vmf, folio, &fpin))
		goto out_retry;

	/* 阶段 3：得锁后重验 XArray 归属，排除等待期间的 truncate/replace。 */
	/* Did it get truncated? */
	/* 等待 folio lock 期间可被摘除；丢锁/引用后重新查或创建。 */
	if (unlikely(folio->mapping != mapping)) {
		folio_unlock(folio);
		folio_put(folio);
		goto retry_find;
	}
	VM_BUG_ON_FOLIO(!folio_contains(folio, index), folio);

	/*
	 * We have a locked folio in the page cache, now we need to check
	 * that it's up-to-date. If not, it is going to be due to an error,
	 * or because readahead was otherwise unable to retrieve it.
	 */
	/*
	 * 此时 folio 已在页缓存且上锁；若仍非 uptodate，原因只能是
	 * I/O 错误或预读未填充，必须转入同步读取/错误路径，不能直接建立映射。
	 */
	if (unlikely(!folio_test_uptodate(folio))) {
		/*
		 * If the invalidate lock is not held, the folio was in cache
		 * and uptodate and now it is not. Strange but possible since we
		 * didn't hold the page lock all the time. Let's drop
		 * everything, get the invalidate lock and try again.
		 */
		/*
		 * cache hit 初查 uptodate 后到得锁间状态可变化；若尚未持
		 * invalidate lock，不能直接发 I/O，先释放并沿统一 retry_find 重来。
		 */
		if (!mapping_locked) {
			folio_unlock(folio);
			folio_put(folio);
			goto retry_find;
		}

		/*
		 * OK, the folio is really not uptodate. This can be because the
		 * VMA has the VM_RAND_READ flag set, or because an error
		 * arose. Let's read it in directly.
		 */
		/* 已持 invalidate lock 后确认无效，进入一次同步 reread。 */
		goto page_not_uptodate;
	}

	/*
	 * We've made it this far and we had to drop our mmap_lock, now is the
	 * time to return to the upper layer and have it re-find the vma and
	 * redo the fault.
	 */
	/*
	 * fpin 非 NULL 证明 mmap lock 曾为 I/O 释放；即使 folio
	 * 已准备好也不能继续使用旧 VMA，必须 RETRY 让上层重新验证。
	 */
	if (fpin) {
		folio_unlock(folio);
		goto out_retry;
	}
	if (mapping_locked)
		filemap_invalidate_unlock_shared(mapping);

	/*
	 * Found the page and have a reference on it.
	 * We must recheck i_size under page lock.
	 */
	/* folio lock 下重读 EOF，防止 truncate 后映射越界页而漏 SIGBUS。 */
	max_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	if (unlikely(index >= max_idx)) {
		folio_unlock(folio);
		folio_put(folio);
		return VM_FAULT_SIGBUS;
	}

	vmf->page = folio_file_page(folio, index);
	return ret | VM_FAULT_LOCKED;

page_not_uptodate:
	/*
	 * Umm, take care of errors if the page isn't up-to-date.
	 * Try to re-read it _once_. We do this synchronously,
	 * because there really aren't any performance issues here
	 * and we need to check for errors.
	 */
	/*
	 * 异常路径只同步重读一次，便于得到确定 errno；read_folio
	 * 负责解锁。成功/被 truncate 都重新查，其他错误映射为 SIGBUS。
	 */
	fpin = maybe_unlock_mmap_for_io(vmf, fpin);
	error = filemap_read_folio(file, mapping->a_ops->read_folio, folio);
	if (fpin)
		goto out_retry;
	folio_put(folio);

	if (!error || error == AOP_TRUNCATED_PAGE)
		goto retry_find;
	filemap_invalidate_unlock_shared(mapping);

	return VM_FAULT_SIGBUS;

out_retry:
	/*
	 * We dropped the mmap_lock, we need to return to the fault handler to
	 * re-find the vma and come back and find our hopefully still populated
	 * page.
	 */
	/* 统一撤销本次临时引用/锁，并返回 RETRY；上层重新定位 VMA。 */
	if (!IS_ERR(folio))
		folio_put(folio);
	if (mapping_locked)
		filemap_invalidate_unlock_shared(mapping);
	if (fpin)
		fput(fpin);
	return ret | VM_FAULT_RETRY;
}
EXPORT_SYMBOL(filemap_fault);

/*
 * filemap_map_pmd() - 尝试把 PMD-mappable file folio 建成 huge PMD。
 * 已有 transhuge PMD 时释放当前 folio并返回 true。空 PMD 且 do_set_pmd
 * 成功时映射消费引用，本函数只解锁；失败则必要时安装预分配 PTE 页表，
 * 返回 false 交给 PTE fault-around。
 */
static bool filemap_map_pmd(struct vm_fault *vmf, struct folio *folio,
		pgoff_t start)
{
	struct mm_struct *mm = vmf->vma->vm_mm;

	/* Huge page is mapped? No need to proceed. */
	/* 并发 fault 已发布 huge PMD，本路径释放候选即可。 */
	if (pmd_trans_huge(*vmf->pmd)) {
		folio_unlock(folio);
		folio_put(folio);
		return true;
	}

	if (pmd_none(*vmf->pmd) && folio_test_pmd_mappable(folio)) {
		struct page *page = folio_file_page(folio, start);
		vm_fault_t ret = do_set_pmd(vmf, folio, page);
		if (!ret) {
			/* The page is mapped successfully, reference consumed. */
			/* 页表映射接管引用；这里只解锁，不能 folio_put。 */
			folio_unlock(folio);
			return true;
		}
	}

	if (pmd_none(*vmf->pmd) && vmf->prealloc_pte)
		pmd_install(mm, vmf->pmd, &vmf->prealloc_pte);

	return false;
}

/*
 * next_uptodate_folio() - 为 fault-around 找下一个可立即映射的 locked folio。
 * RCU 下跳过 value、锁竞争、非 uptodate、readahead 和已移动页；
 * try_get/reload/trylock 后重验 mapping 与 i_size。成功返回 locked+持引用，
 * 失败项完整解锁/put，EOF NULL；绝不等待或发 I/O。
 */
static struct folio *next_uptodate_folio(struct xa_state *xas,
		struct address_space *mapping, pgoff_t end_pgoff)
{
	struct folio *folio = xas_next_entry(xas, end_pgoff);
	unsigned long max_idx;

	do {
		/* 阶段 1：先跳过无需/不能映射的候选，再以 trylock 保证全程不等待。 */
		if (!folio)
			return NULL;
		if (xas_retry(xas, folio))
			continue;
		if (xa_is_value(folio))
			continue;
		if (!folio_try_get(folio))
			continue;
		if (folio_test_locked(folio))
			goto skip;
		/* Has the page moved or been split? */
		/* 锁等待期间 folio 可迁移或拆分，必须重验 mapping/index。 */
		if (unlikely(folio != xas_reload(xas)))
			goto skip;
		if (!folio_test_uptodate(folio) || folio_test_readahead(folio))
			goto skip;
		if (!folio_trylock(folio))
			goto skip;
		/* 阶段 2：得锁后重验归属、数据有效性和当前 EOF 上界。 */
		if (folio->mapping != mapping)
			goto unlock;
		if (!folio_test_uptodate(folio))
			goto unlock;
		max_idx = DIV_ROUND_UP(i_size_read(mapping->host), PAGE_SIZE);
		if (xas->xa_index >= max_idx)
			goto unlock;
		/* 成功返回的 folio 同时持引用和锁，ownership 交给映射 helper。 */
		return folio;
unlock:
		folio_unlock(folio);
skip:
		folio_put(folio);
	} while ((folio = xas_next_entry(xas, end_pgoff)) != NULL);

	/* 所有候选均不满足时返回 EOF，不残留锁或引用。 */
	return NULL;
}

/*
 * Map page range [start_page, start_page + nr_pages) of folio.
 * start_page is gotten from start by folio_page(folio, start)
 */
/*
 * 把 large folio 的 [start,start+nr_pages) 按连续空 PTE 段映射。
 * folio 完全位于文件/VMA/同一页表边界时尽量扩为整 folio。跳过 HWPoison
 * 与非 none PTE（含 marker）；首个 PTE 消费调用者引用，其余逐 PTE 加引用。
 * 若一个也没映射，folio lock 保证未被 truncate，可直接归还引用。
 */
static vm_fault_t filemap_map_folio_range(struct vm_fault *vmf,
			struct folio *folio, unsigned long start,
			unsigned long addr, unsigned int nr_pages,
			unsigned long *rss, pgoff_t file_end)
{
	struct address_space *mapping = folio->mapping;
	unsigned int ref_from_caller = 1;
	/* 首个 PTE 是否消费调用者引用由该计数追踪，防止重复增减。 */
	vm_fault_t ret = 0;
	struct page *page = folio_page(folio, start);
	unsigned int count = 0;
	pte_t *old_ptep = vmf->pte;
	unsigned long addr0;

	/*
	 * Map the large folio fully where possible:
	 *
	 *  - The folio is fully within size of the file or belong
	 *    to shmem/tmpfs;
	 *  - The folio doesn't cross VMA boundary;
	 *  - The folio doesn't cross page table boundary;
	 */
	/*
	 * 文件内容覆盖（shmem 例外）、VMA 覆盖和同一 PMD 页表三条件
	 * 都满足才扩成整 folio，避免越界映射和错误 SIGBUS 语义。
	 */
	addr0 = addr - start * PAGE_SIZE;
	if ((file_end >= folio_next_index(folio) || shmem_mapping(mapping)) &&
	    folio_within_vma(folio, vmf->vma) &&
	    (addr0 & PMD_MASK) == ((addr0 + folio_size(folio) - 1) & PMD_MASK)) {
		vmf->pte -= start;
		page -= start;
		addr = addr0;
		nr_pages = folio_nr_pages(folio);
	}

	do {
		/* 每轮先聚合一段连续空 PTE；poison/marker 会把该段切开。 */
		if (PageHWPoison(page + count))
			goto skip;

		/*
		 * NOTE: If there're PTE markers, we'll leave them to be
		 * handled in the specific fault path, and it'll prohibit the
		 * fault-around logic.
		 */
		/*
		 * PTE marker 含 userfaultfd 等专用语义，留给单页 fault
		 * 处理；批量 fault-around 遇到它必须停止，不能越过或覆盖 marker。
		 */
		if (!pte_none(ptep_get(&vmf->pte[count])))
			goto skip;

		count++;
		continue;
skip:
		if (count) {
			/* 提交此前连续段，并按实际 PTE 数精确转移 folio 引用。 */
			set_pte_range(vmf, folio, page, count, addr);
			*rss += count;
			folio_ref_add(folio, count - ref_from_caller);
			ref_from_caller = 0;
			if (in_range(vmf->address, addr, count * PAGE_SIZE))
				ret = VM_FAULT_NOPAGE;
		}

		count++;
		/* 提交/跳过一段后同步推进 page、PTE 与虚拟地址三个游标。 */
		page += count;
		vmf->pte += count;
		addr += count * PAGE_SIZE;
		count = 0;
	} while (--nr_pages > 0);

	if (count) {
		/* 循环结束仍有尾段时执行与 skip 路径相同的提交协议。 */
		set_pte_range(vmf, folio, page, count, addr);
		*rss += count;
		folio_ref_add(folio, count - ref_from_caller);
		ref_from_caller = 0;
		if (in_range(vmf->address, addr, count * PAGE_SIZE))
			ret = VM_FAULT_NOPAGE;
	}

	vmf->pte = old_ptep;
	if (ref_from_caller)
		/* Locked folios cannot get truncated. */
		/* 零 PTE 接管引用时直接减；锁保证 folio 尚未被摘除。 */
		folio_ref_dec(folio);

	return ret;
}

/*
 * filemap_map_order0_folio() - fault-around 映射单页 folio。
 * poison/非空 PTE 跳过并归还引用；空槽 set_pte_range 接管引用并增 rss。
 * 若该地址是原 fault，返回 NOPAGE；进入时 folio locked。
 */
static vm_fault_t filemap_map_order0_folio(struct vm_fault *vmf,
		struct folio *folio, unsigned long addr,
		unsigned long *rss)
{
	vm_fault_t ret = 0;
	struct page *page = &folio->page;

	if (PageHWPoison(page))
		goto out;

	/*
	 * NOTE: If there're PTE markers, we'll leave them to be
	 * handled in the specific fault path, and it'll prohibit
	 * the fault-around logic.
	 */
	/* 同上，marker 强制退回专用 fault 路径并禁止批量映射。 */
	if (!pte_none(ptep_get(vmf->pte)))
		goto out;

	if (vmf->address == addr)
		ret = VM_FAULT_NOPAGE;

	set_pte_range(vmf, folio, page, 1, addr);
	(*rss)++;
	return ret;

out:
	/* Locked folios cannot get truncated. */
	/* 未建映射，归还预先取得的 folio 引用。 */
	folio_ref_dec(folio);
	return ret;
}

/*
 * filemap_map_pages() - 在 fault 周围预映射多个已缓存、uptodate folio。
 *
 * 按 i_size 截 end，只挑无需 I/O 的 locked folio；先尝试 huge PMD，否则
 * 持 PTE lock 映射 order-0/large 范围。不跨 EOF（shmem PMD 历史例外），
 * 跳过 poison/marker/锁竞争。引用由映射或跳过路径逐项消费。
 * 返回 NOPAGE 表示原地址已覆盖，否则 0；无阻塞 I/O。
 */
vm_fault_t filemap_map_pages(struct vm_fault *vmf,
			     pgoff_t start_pgoff, pgoff_t end_pgoff)
{
	struct vm_area_struct *vma = vmf->vma;
	struct file *file = vma->vm_file;
	struct address_space *mapping = file->f_mapping;
	pgoff_t file_end, last_pgoff = start_pgoff;
	/* addr/PTE/last_pgoff 是同步游标，rss 统计本次新建映射数。 */
	unsigned long addr;
	XA_STATE(xas, &mapping->i_pages, start_pgoff);
	struct folio *folio;
	vm_fault_t ret = 0;
	unsigned long rss = 0;
	unsigned int nr_pages = 0, folio_type;

	/*
	 * Recalculate end_pgoff based on file_end before calling
	 * next_uptodate_folio() to avoid races with concurrent
	 * truncation.
	 */
	/* 先冻结本次 EOF 上界，避免并发 truncate 让扫描越界。 */
	file_end = DIV_ROUND_UP(i_size_read(mapping->host), PAGE_SIZE) - 1;
	end_pgoff = min(end_pgoff, file_end);

	rcu_read_lock();
	folio = next_uptodate_folio(&xas, mapping, end_pgoff);
	if (!folio)
		goto out;

	/*
	 * Do not allow to map with PMD across i_size to preserve
	 * SIGBUS semantics.
	 *
	 * Make an exception for shmem/tmpfs that for long time
	 * intentionally mapped with PMDs across i_size.
	 */
	/*
	 * 普通文件 huge PMD 不得跨 EOF，否则尾部访问应 SIGBUS；
	 * shmem/tmpfs 为兼容长期行为保留例外。
	 */
	if ((file_end >= folio_next_index(folio) || shmem_mapping(mapping)) &&
	    filemap_map_pmd(vmf, folio, start_pgoff)) {
		ret = VM_FAULT_NOPAGE;
		goto out;
	}

	addr = vma->vm_start + ((start_pgoff - vma->vm_pgoff) << PAGE_SHIFT);
	/* 阶段 2：huge PMD 未命中后锁住目标 PTE 页表，准备批量安装。 */
	vmf->pte = pte_offset_map_lock(vma->vm_mm, vmf->pmd, addr, &vmf->ptl);
	if (!vmf->pte) {
		folio_unlock(folio);
		folio_put(folio);
		goto out;
	}

	folio_type = mm_counter_file(folio);
	do {
		unsigned long end;
		vm_fault_t map_ret;

		/* xas 可跨 hole，PTE 与虚拟地址游标必须按索引差同步前移。 */
		addr += (xas.xa_index - last_pgoff) << PAGE_SHIFT;
		vmf->pte += xas.xa_index - last_pgoff;
		last_pgoff = xas.xa_index;
		end = folio_next_index(folio) - 1;
		nr_pages = min(end, end_pgoff) - xas.xa_index + 1;

		/* order-0 与 large folio 共享扫描框架，仅映射 helper 不同。 */
		if (!folio_test_large(folio)) {
			map_ret = filemap_map_order0_folio(vmf, folio, addr,
							   &rss);
		} else {
			unsigned long start = xas.xa_index - folio->index;

			map_ret = filemap_map_folio_range(vmf, folio, start,
							  addr, nr_pages, &rss,
							  file_end);
		}
		ret |= map_ret;
		/* map_ret 同时决定原 fault 是否覆盖，以及是否把本次命中计入反馈。 */

		/*
		 * If there are too many folios that are recently evicted
		 * in a file, they will probably continue to be evicted.
		 * In such situation, read-ahead is only a waste of IO.
		 * Don't decrease mmap_miss in this scenario to make sure
		 * we can stop read-ahead.
		 *
		 * VM_SEQ_READ and VM_EXEC mappings skip the mmap_miss
		 * increment in do_sync_mmap_readahead(), so skip the
		 * decrement here as well to keep the counter symmetric.
		 */
	/*
	 * 成功映射非 workingset 页才算稳定 hit 并递减 miss；
	 * 易回收页不减，促使低收益文件停止 RA。SEQ/EXEC 两侧保持对称。
	 */
		if ((map_ret & VM_FAULT_NOPAGE) &&
		    !(vmf->flags & FAULT_FLAG_TRIED) &&
		    !folio_test_workingset(folio) &&
		    !(vma->vm_flags & (VM_SEQ_READ | VM_EXEC))) {
			unsigned short mmap_miss;

			mmap_miss = READ_ONCE(file->f_ra.mmap_miss);
			if (mmap_miss)
				WRITE_ONCE(file->f_ra.mmap_miss,
					   mmap_miss - 1);
			/* 每个 folio 映射后用 mmap_miss 反馈调整后续预读收益判断。 */
		}

		folio_unlock(folio);
	} while ((folio = next_uptodate_folio(&xas, mapping, end_pgoff)) != NULL);
	/* 阶段 3：批量更新 rss、解 PTE 锁，再退出 RCU。 */
	add_mm_counter(vma->vm_mm, folio_type, rss);
	pte_unmap_unlock(vmf->pte, vmf->ptl);
	trace_mm_filemap_map_pages(mapping, start_pgoff, end_pgoff);
out:
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL(filemap_map_pages);

/*
 * filemap_page_mkwrite() - shared writable mmap 首次写前锁页并纳入 freeze 协议。
 * sb_start_pagefault 阻止文件系统冻结越过当前 fault；更新时间并锁 folio，
 * 重验 mapping 防 truncate。成功提前 mark dirty、等待 stable，返回
 * VM_FAULT_LOCKED 把锁交给 fault 核心；被摘除返回 NOPAGE。所有出口
 * sb_end_pagefault 配对。
 */
vm_fault_t filemap_page_mkwrite(struct vm_fault *vmf)
{
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	struct folio *folio = page_folio(vmf->page);
	vm_fault_t ret = VM_FAULT_LOCKED;

	sb_start_pagefault(mapping->host->i_sb);
	/* 阶段 1：冻结保护内更新时间并锁 folio，随后重验 mapping 归属。 */
	file_update_time(vmf->vma->vm_file);
	folio_lock(folio);
	if (folio->mapping != mapping) {
		folio_unlock(folio);
		ret = VM_FAULT_NOPAGE;
		goto out;
	}
	/*
	 * We mark the folio dirty already here so that when freeze is in
	 * progress, we are guaranteed that writeback during freezing will
	 * see the dirty folio and writeprotect it again.
	 */
	/*
	 * 先让 freeze writeback 看见 dirty 并重新 write-protect，
	 * 再允许用户写；次序反转会让冻结期间出现未被捕获的修改。
	 */
	folio_mark_dirty(folio);
	folio_wait_stable(folio);
out:
	sb_end_pagefault(mapping->host->i_sb);
	return ret;
}

/* 普通文件 mmap 操作表：缺页、fault-around 与 shared 写保护升级入口。 */
const struct vm_operations_struct generic_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= filemap_page_mkwrite,
};

/* This is used for a general mmap of a disk file */
/* 普通磁盘文件 mmap 初始化使用下列通用 vm_ops。 */

/*
 * generic_file_mmap() - 为支持 read_folio 的 file 安装通用 mmap 操作。
 * 无 read_folio 无法处理缺页，返回 -ENOEXEC；成功更新 atime、设置 vm_ops。
 */
int generic_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct address_space *mapping = file->f_mapping;

	if (!mapping->a_ops->read_folio)
		return -ENOEXEC;
	file_accessed(file);
	vma->vm_ops = &generic_file_vm_ops;
	return 0;
}

/*
 * generic_file_mmap_prepare() - 新 VMA 描述符 API 的等价准备入口。
 * 成功更新 atime 并把 generic_file_vm_ops 写入 desc；失败 -ENOEXEC。
 */
int generic_file_mmap_prepare(struct vm_area_desc *desc)
{
	struct file *file = desc->file;
	struct address_space *mapping = file->f_mapping;

	if (!mapping->a_ops->read_folio)
		return -ENOEXEC;
	file_accessed(file);
	desc->vm_ops = &generic_file_vm_ops;
	return 0;
}

/*
 * This is for filesystems which do not implement ->writepage.
 */
/*
 * 没有 writepage 的文件系统只能建立非 shared-maywrite 映射，
 * 否则 shared 脏页无法持久化；违规 -EINVAL，其余复用通用 mmap。
 */
int generic_file_readonly_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (vma_is_shared_maywrite(vma))
		return -EINVAL;
	return generic_file_mmap(file, vma);
}

/* readonly mmap 的 vm_area_desc API 等价版本，先拒绝 shared-maywrite。 */
int generic_file_readonly_mmap_prepare(struct vm_area_desc *desc)
{
	if (is_shared_maywrite(&desc->vma_flags))
		return -EINVAL;
	return generic_file_mmap_prepare(desc);
}
#else
/*
 * 无 MMU 配置没有页表 mmap：mkwrite 返回 SIGBUS，所有 mmap 入口 -ENOSYS。
 * 同名 stub 让通用文件系统无需在调用处散布条件编译。
 */
vm_fault_t filemap_page_mkwrite(struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}
int generic_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	return -ENOSYS;
}
int generic_file_mmap_prepare(struct vm_area_desc *desc)
{
	return -ENOSYS;
}
int generic_file_readonly_mmap(struct file *file, struct vm_area_struct *vma)
{
	return -ENOSYS;
}
int generic_file_readonly_mmap_prepare(struct vm_area_desc *desc)
{
	return -ENOSYS;
}
#endif /* CONFIG_MMU */
/* 以上按 CONFIG_MMU 选择完整实现或 stub，对外符号保持一致。 */

EXPORT_SYMBOL(filemap_page_mkwrite);
EXPORT_SYMBOL(generic_file_mmap);
EXPORT_SYMBOL(generic_file_mmap_prepare);
EXPORT_SYMBOL(generic_file_readonly_mmap);
EXPORT_SYMBOL(generic_file_readonly_mmap_prepare);

/*
 * do_read_cache_folio() - 查找或创建单个 cache folio，并保证返回时 uptodate。
 *
 * @mapping/@index 目标；@filler 可空则用 a_ops->read_folio；@file 透传；
 * @gfp 用于 folio/XArray 分配。调用者已持 invalidate_lock。
 *
 * miss 时分配最小 order、对齐并插入；EEXIST 说明竞争者获胜，重查。
 * hit 但未有效则 trylock；锁竞争用 DROP 等待后重查，得锁后重验 truncate/
 * uptodate。filler/read_folio 提交并等待。成功返回含 index、uptodate、
 * 持引用的 folio并 mark_accessed；失败 ERR_PTR，可能睡眠。
 */
static struct folio *do_read_cache_folio(struct address_space *mapping,
		pgoff_t index, filler_t filler, struct file *file, gfp_t gfp)
{
	struct folio *folio;
	int err;

	if (!filler)
		filler = mapping->a_ops->read_folio;
repeat:
	/* 阶段 1：先查 cache；miss 候选插入遇 EEXIST 时由竞争胜者替代。 */
	folio = filemap_get_folio(mapping, index);
	if (IS_ERR(folio)) {
		folio = filemap_alloc_folio(gfp, mapping_min_folio_order(mapping), NULL);
		if (!folio)
			return ERR_PTR(-ENOMEM);
		index = mapping_align_index(mapping, index);
	/* miss 候选按 mapping 最小 order 对齐，EEXIST 交给竞争胜者。 */
		err = filemap_add_folio(mapping, folio, index, gfp);
		if (unlikely(err)) {
			folio_put(folio);
			if (err == -EEXIST)
				goto repeat;
			/* Presumably ENOMEM for xarray node */
			/* 非 EEXIST 多半是 XArray 节点内存失败，原样上报。 */
			return ERR_PTR(err);
		}

		goto filler;
	}
	if (folio_test_uptodate(folio))
		goto out;

	/* 阶段 2：锁竞争用等待后重查，避免对可能已截断的旧对象继续操作。 */
	if (!folio_trylock(folio)) {
		folio_put_wait_locked(folio, TASK_UNINTERRUPTIBLE);
		goto repeat;
	}

	/* Folio was truncated from mapping */
	/* 等待锁期间被摘除，释放后从权威 XArray 重查。 */
	if (!folio->mapping) {
		folio_unlock(folio);
		folio_put(folio);
		goto repeat;
	}

	/* Someone else locked and filled the page in a very small window */
	/* trylock 前后另一读者可能已完成填充，避免重复 I/O。 */
	if (folio_test_uptodate(folio)) {
		folio_unlock(folio);
		goto out;
	}

filler:
	/* 阶段 3：filler 接管 locked folio 并以 unlock 发布完成状态。 */
	err = filemap_read_folio(file, filler, folio);
	if (err) {
		folio_put(folio);
		if (err == AOP_TRUNCATED_PAGE)
			goto repeat;
		return ERR_PTR(err);
	}

out:
	/* 统一成功出口记录访问热度，并返回持有引用的 uptodate folio。 */
	folio_mark_accessed(folio);
	return folio;
}

/**
 * read_cache_folio - Read into page cache, fill it if needed.
 * @mapping: The address_space to read from.
 * @index: The index to read.
 * @filler: Function to perform the read, or NULL to use aops->read_folio().
 * @file: Passed to filler function, may be NULL if not required.
 *
 * Read one page into the page cache.  If it succeeds, the folio returned
 * will contain @index, but it may not be the first page of the folio.
 *
 * If the filler function returns an error, it will be returned to the
 * caller.
 *
 * Context: May sleep.  Expects mapping->invalidate_lock to be held.
 * Return: An uptodate folio on success, ERR_PTR() on failure.
 */
/*
 * 以 mapping 默认 gfp 包装单 folio cache read。@filler 可空，
 * @file 可空；成功返回持引用、含 @index 的 uptodate folio（index 可位于
 * 大 folio 中间），失败 ERR_PTR。期望 invalidate_lock 已持，可睡眠。
 */
struct folio *read_cache_folio(struct address_space *mapping, pgoff_t index,
		filler_t filler, struct file *file)
{
	return do_read_cache_folio(mapping, index, filler, file,
			mapping_gfp_mask(mapping));
}
EXPORT_SYMBOL(read_cache_folio);

/**
 * mapping_read_folio_gfp - Read into page cache, using specified allocation flags.
 * @mapping:	The address_space for the folio.
 * @index:	The index that the allocated folio will contain.
 * @gfp:	The page allocator flags to use if allocating.
 *
 * This is the same as "read_cache_folio(mapping, index, NULL, NULL)", but with
 * any new memory allocations done using the specified allocation flags.
 *
 * The most likely error from this function is EIO, but ENOMEM is
 * possible and so is EINTR.  If ->read_folio returns another error,
 * that will be returned to the caller.
 *
 * The function expects mapping->invalidate_lock to be already held.
 *
 * Return: Uptodate folio on success, ERR_PTR() on failure.
 */
/*
 * 与 read_cache_folio(mapping,index,NULL,NULL) 相同，但新分配
 * 使用调用者 @gfp。常见错误 -EIO，也可 -ENOMEM/-EINTR 或 a_ops 自定义
 * errno。调用者已持 invalidate_lock；成功 folio 持引用。
 */
struct folio *mapping_read_folio_gfp(struct address_space *mapping,
		pgoff_t index, gfp_t gfp)
{
	return do_read_cache_folio(mapping, index, NULL, NULL, gfp);
}
EXPORT_SYMBOL(mapping_read_folio_gfp);

/*
 * do_read_cache_page() - 兼容 page API 的 folio read 包装。
 * 调用 folio 核心后，错误指针通过 &folio->page 保持同一编码；成功返回
 * @index 对应 subpage，所持 folio 引用由 page 引用语义承接。
 */
static struct page *do_read_cache_page(struct address_space *mapping,
		pgoff_t index, filler_t *filler, struct file *file, gfp_t gfp)
{
	struct folio *folio;

	folio = do_read_cache_folio(mapping, index, filler, file, gfp);
	if (IS_ERR(folio))
		return &folio->page;
	return folio_file_page(folio, index);
}

/*
 * read_cache_page() - 用 mapping 默认 gfp 读取并返回 index 对应 struct page。
 * filler/file 语义同 read_cache_folio；成功持引用 page，失败 ERR_PTR。
 */
struct page *read_cache_page(struct address_space *mapping,
			pgoff_t index, filler_t *filler, struct file *file)
{
	return do_read_cache_page(mapping, index, filler, file,
			mapping_gfp_mask(mapping));
}
EXPORT_SYMBOL(read_cache_page);

/**
 * read_cache_page_gfp - read into page cache, using specified page allocation flags.
 * @mapping:	the page's address_space
 * @index:	the page index
 * @gfp:	the page allocator flags to use if allocating
 *
 * This is the same as "read_mapping_page(mapping, index, NULL)", but with
 * any new page allocations done using the specified allocation flags.
 *
 * If the page does not get brought uptodate, return -EIO.
 *
 * The function expects mapping->invalidate_lock to be already held.
 *
 * Return: up to date page on success, ERR_PTR() on failure.
 */
/*
 * read_cache_page 的自定义 GFP 版本；调用者已持 invalidate_lock。
 * 未能 bring uptodate 返回 -EIO，亦可返回分配/信号/a_ops 错误。
 */
struct page *read_cache_page_gfp(struct address_space *mapping,
				pgoff_t index,
				gfp_t gfp)
{
	return do_read_cache_page(mapping, index, NULL, NULL, gfp);
}
EXPORT_SYMBOL(read_cache_page_gfp);

/*
 * Warn about a page cache invalidation failure during a direct I/O write.
 */
/*
 * direct write 后仍无法失效重叠 page cache 意味 buffered read
 * 可能看到旧数据，属于潜在损坏。向 mapping errseq 发布 -EIO，并以每日
 * 限速打印路径/PID/comm，避免故障风暴刷屏。无直接返回。
 */
static void dio_warn_stale_pagecache(struct file *filp)
{
	static DEFINE_RATELIMIT_STATE(_rs, 86400 * HZ, DEFAULT_RATELIMIT_BURST);
	char pathname[128];
	char *path;

	errseq_set(&filp->f_mapping->wb_err, -EIO);
	/* 先持久发布错误；日志限速只影响告警频率，不影响 fsync 观察。 */
	if (__ratelimit(&_rs)) {
		path = file_path(filp, pathname, sizeof(pathname));
		if (IS_ERR(path))
			path = "(unknown)";
		pr_crit("Page cache invalidation failure on direct I/O.  Possible data corruption due to collision with buffered I/O!\n");
		pr_crit("File: %s PID: %d Comm: %.20s\n", path, current->pid,
			current->comm);
	}
}

/*
 * kiocb_invalidate_post_direct_write() - direct write 成功后再次失效 cache。
 * @count 为实际写入字节，区间从当前 ki_pos 开始。mapping 无页快退；
 * 失效失败调用严重告警/errseq。无返回，写本身已成功不能改其结果。
 */
void kiocb_invalidate_post_direct_write(struct kiocb *iocb, size_t count)
{
	struct address_space *mapping = iocb->ki_filp->f_mapping;

	if (mapping->nrpages &&
	    invalidate_inode_pages2_range(mapping,
			iocb->ki_pos >> PAGE_SHIFT,
			(iocb->ki_pos + count - 1) >> PAGE_SHIFT))
		dio_warn_stale_pagecache(iocb->ki_filp);
}

/*
 * generic_file_direct_write() - 通用 direct write 及页缓存一致性收尾。
 *
 * 写前 invalidate；-EBUSY 返回 0 请求上层 buffered fallback，其他错误
 * 上报。调用 a_ops->direct_IO 后，对同步正进展再次 invalidate、推进
 * i_size（非块设备）与 ki_pos。非 EIOCBQUEUED 时回退 iov_iter 中未被
 * 实际写入的预消费量；异步时 iterator ownership 已交完成路径。
 */
ssize_t
generic_file_direct_write(struct kiocb *iocb, struct iov_iter *from)
{
	struct address_space *mapping = iocb->ki_filp->f_mapping;
	size_t write_len = iov_iter_count(from);
	ssize_t written;

	/*
	 * If a page can not be invalidated, return 0 to fall back
	 * to buffered write.
	 */
	/* 无法失效的忙页不是硬失败，返回 0 让通用层改用一致 buffered write。 */
	written = kiocb_invalidate_pages(iocb, write_len);
	if (written) {
		if (written == -EBUSY)
			return 0;
		return written;
	}

	written = mapping->a_ops->direct_IO(iocb, from);

	/*
	 * Finally, try again to invalidate clean pages which might have been
	 * cached by non-direct readahead, or faulted in by get_user_pages()
	 * if the source of the write was an mmap'ed region of the file
	 * we're writing.  Either one is a pretty crazy thing to do,
	 * so we don't support it 100%.  If this invalidation
	 * fails, tough, the write still worked...
	 *
	 * Most of the time we do not need this since dio_complete() will do
	 * the invalidation for us. However there are some file systems that
	 * do not end up with dio_complete() being called, so let's not break
	 * them by removing it completely.
	 *
	 * Noticeable example is a blkdev_direct_IO().
	 *
	 * Skip invalidation for async writes or if mapping has no pages.
	 */
	/*
	 * 写中途非 direct readahead 或 GUP 自映射源可能重新把旧页
	 * 填入 cache，完成后再失效。多数 iomap dio_complete 已做，但 blkdev
	 * 等路径未必调用，保留兜底。失败不撤销已成功写，只发布潜在损坏 EIO。
	 */
	if (written > 0) {
		struct inode *inode = mapping->host;
		loff_t pos = iocb->ki_pos;

		kiocb_invalidate_post_direct_write(iocb, written);
		pos += written;
		write_len -= written;
		if (pos > i_size_read(inode) && !S_ISBLK(inode->i_mode)) {
			/* 仅普通文件扩展 i_size；块设备容量不由本次 DIO 改写。 */
			i_size_write(inode, pos);
			mark_inode_dirty(inode);
		}
		iocb->ki_pos = pos;
	}
	if (written != -EIOCBQUEUED)
		/* 同步 DIO 把未消费或回滚的尾部恢复到调用者 iterator。 */
		iov_iter_revert(from, write_len - iov_iter_count(from));
	return written;
}
EXPORT_SYMBOL(generic_file_direct_write);

/*
 * generic_perform_write() - 通用 buffered write 的 write_begin/copy/write_end 循环。
 *
 * @iocb 提供 file/位置；@i 是源 iterator。按 mapping 最大 folio 尺度分块，
 * balance dirty 速率后让 a_ops->write_begin 返回 locked folio/fsdata；
 * 使用 atomic copy 避免用户页 fault 在文件系统锁内递归死锁，再由
 * write_end 提交实际字节并解锁。短写回退 iterator，零进展时缩小 chunk
 * 或预 fault 用户内存保证前进。
 *
 * 返回部分写字节优先，否则最后 errno；成功推进 ki_pos。可能睡眠，
 * 调用者通常持 inode i_rwsem。
 */
ssize_t generic_perform_write(struct kiocb *iocb, struct iov_iter *i)
{
	struct file *file = iocb->ki_filp;
	loff_t pos = iocb->ki_pos;
	struct address_space *mapping = file->f_mapping;
	const struct address_space_operations *a_ops = mapping->a_ops;
	size_t chunk = mapping_max_folio_size(mapping);
	/* chunk 可动态折半，status/written 分离错误与已提交进展。 */
	long status = 0;
	ssize_t written = 0;

	do {
		struct folio *folio;
		size_t offset;		/* Offset into folio */
		/* 本轮写入相对 folio 起点的字节偏移。 */
		size_t bytes;		/* Bytes to write to folio */
		/* 本轮计划提交给 write_begin/write_end 的字节数。 */
		size_t copied;		/* Bytes copied from user */
		/* 实际从 iov_iter 复制成功的字节数，可小于 bytes。 */
		void *fsdata = NULL;

		bytes = iov_iter_count(i);
retry:
		offset = pos & (chunk - 1);
		bytes = min(chunk - offset, bytes);
		balance_dirty_pages_ratelimited(mapping);

		if (fatal_signal_pending(current)) {
			status = -EINTR;
			/* 未进入 write_begin，可安全终止；已有写入仍按部分成功返回。 */
			break;
		}

		status = a_ops->write_begin(iocb, mapping, pos, bytes,
						&folio, &fsdata);
		if (unlikely(status < 0))
			break;

		/* write_begin 返回 locked folio；从这里起 write_end 必须配对释放。 */
		offset = offset_in_folio(folio, pos);
		/* 文件系统可返回比预期更小的 folio，必须再次裁剪本轮长度。 */
		if (bytes > folio_size(folio) - offset)
			bytes = folio_size(folio) - offset;

		if (mapping_writably_mapped(mapping))
			flush_dcache_folio(folio);

		/*
		 * Faults here on mmap()s can recurse into arbitrary
		 * filesystem code. Lots of locks are held that can
		 * deadlock. Use an atomic copy to avoid deadlocking
		 * in page fault handling.
		 */
		/*
		 * 普通 copy_from_iter 可能 fault 并递归进入任意 fs，
		 * 此时 write_begin 已持多种锁；atomic copy 不处理 fault，失败量
		 * 由后面的预 fault/重试慢路解决。
		 */
		copied = copy_folio_from_iter_atomic(folio, offset, bytes, i);
		flush_dcache_folio(folio);

		status = a_ops->write_end(iocb, mapping, pos, bytes, copied,
						folio, fsdata);
		/* write_end 消费锁并报告已提交量；未提交的 iterator 字节立即回退。 */
		if (unlikely(status != copied)) {
			iov_iter_revert(i, copied - max(status, 0L));
			if (unlikely(status < 0))
				break;
		}
		cond_resched();

		if (unlikely(status == 0)) {
			/*
			 * A short copy made ->write_end() reject the
			 * thing entirely.  Might be memory poisoning
			 * halfway through, might be a race with munmap,
			 * might be severe memory pressure.
			 */
			/*
			 * write_end 完全拒绝短 copy 可能来自 poison、munmap
			 * 或压力。大 chunk 先折半；若复制过部分则按 copied 重试。
			 */
			if (chunk > PAGE_SIZE)
				chunk /= 2;
			if (copied) {
				bytes = copied;
				goto retry;
			}

			/*
			 * 'folio' is now unlocked and faults on it can be
			 * handled. Ensure forward progress by trying to
			 * fault it in now.
			 */
			/*
			 * write_end 已解锁 folio，现在可安全 fault-in 用户源；
			 * 若整个范围仍不可读，返回 EFAULT，否则下一轮 atomic copy 前进。
			 */
			if (fault_in_iov_iter_readable(i, bytes) == bytes) {
				status = -EFAULT;
				break;
			}
		} else {
			pos += status;
			written += status;
		}
	/* 循环必须以提交字节、缩小 chunk 或 errno 三者之一前进，防止零进展自旋。 */
	} while (iov_iter_count(i));

	if (!written)
		return status;
	iocb->ki_pos += written;
	return written;
}
EXPORT_SYMBOL(generic_perform_write);

/**
 * __generic_file_write_iter - write data to a file
 * @iocb:	IO state structure (file, offset, etc.)
 * @from:	iov_iter with data to write
 *
 * This function does all the work needed for actually writing data to a
 * file. It does all basic checks, removes SUID from the file, updates
 * modification times and calls proper subroutines depending on whether we
 * do direct IO or a standard buffered write.
 *
 * It expects i_rwsem to be grabbed unless we work on a block device or similar
 * object which does not need locking at all.
 *
 * This function does *not* take care of syncing data in case of O_SYNC write.
 * A caller has to handle it. This is mainly due to the fact that we want to
 * avoid syncing under i_rwsem.
 *
 * Return:
 * * number of bytes written, even for truncated writes
 * * negative error code if no data has been written at all
 */
/*
 * 真正执行一次写的内部入口。先移除 suid/sgid 等权限位并更新
 * mtime/ctime；DIRECT 走 direct write，短写且非 DAX 时把余量 buffered
 * fallback，并用 direct_write_fallback 合并返回/同步语义。普通写直接
 * generic_perform_write。调用者通常已持 i_rwsem，本函数不处理 O_SYNC，
 * 避免在 i_rwsem 内 fsync。返回部分字节优先或 errno。
 */
ssize_t __generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	ssize_t ret;

	ret = file_remove_privs(file);
	/* 阶段 1：先移除 setid/file capability，再更新时间，失败均禁止写。 */
	if (ret)
		return ret;

	ret = file_update_time(file);
	if (ret)
		return ret;

	/* 阶段 2：direct 先行；短写时保留进展并把剩余区间回退 buffered。 */
	if (iocb->ki_flags & IOCB_DIRECT) {
		ret = generic_file_direct_write(iocb, from);
		/*
		 * If the write stopped short of completing, fall back to
		 * buffered writes.  Some filesystems do this for writes to
		 * holes, for example.  For DAX files, a buffered write will
		 * not succeed (even if it did, DAX does not handle dirty
		 * page-cache pages correctly).
		 */
		/*
		 * holes 等可让 DIO 短写，普通文件余量可回退 buffered；
		 * DAX 没有正确的 dirty page-cache 语义，绝不能 fallback。
		 */
		if (ret < 0 || !iov_iter_count(from) || IS_DAX(inode))
			return ret;
		return direct_write_fallback(iocb, from, ret,
				generic_perform_write(iocb, from));
	}

	return generic_perform_write(iocb, from);
}
EXPORT_SYMBOL(__generic_file_write_iter);

/**
 * generic_file_write_iter - write data to a file
 * @iocb:	IO state structure
 * @from:	iov_iter with data to write
 *
 * This is a wrapper around __generic_file_write_iter() to be used by most
 * filesystems. It takes care of syncing the file in case of O_SYNC file
 * and acquires i_rwsem as needed.
 * Return:
 * * negative error code if no data has been written at all of
 *   vfs_fsync_range() failed for a synchronous write
 * * number of bytes written, even for truncated writes
 */
/*
 * 多数文件系统使用的完整 write_iter 包装。持 inode_lock 做
 * generic_write_checks（位置、限额、append 等）和内部写，出锁后再执行
 * generic_write_sync 处理 O_SYNC，避免 fsync 在 i_rwsem 内死锁/长持锁。
 * 返回字节数或检查/写/同步 errno。
 */
ssize_t generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	ssize_t ret;

	/* 阶段 1：inode 锁内完成通用检查与 direct/buffered 数据写入。 */
	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret > 0)
		ret = __generic_file_write_iter(iocb, from);
	inode_unlock(inode);

	/* 阶段 2：出 inode 锁后执行 O_SYNC 等等待，避免扩大锁临界区。 */
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}
EXPORT_SYMBOL(generic_file_write_iter);

/**
 * filemap_release_folio() - Release fs-specific metadata on a folio.
 * @folio: The folio which the kernel is trying to free.
 * @gfp: Memory allocation flags (and I/O mode).
 *
 * The address_space is trying to release any data attached to a folio
 * (presumably at folio->private).
 *
 * This will also be called if the private_2 flag is set on a page,
 * indicating that the folio has other metadata associated with it.
 *
 * The @gfp argument specifies whether I/O may be performed to release
 * this page (__GFP_IO), and whether the call may block
 * (__GFP_RECLAIM & __GFP_FS).
 *
 * Return: %true if the release was successful, otherwise %false.
 */
/*
 * 回收 locked @folio 时释放文件系统私有元数据/private_2。
 * 无需 release 直接 true；writeback 中不能拆元数据，false。文件系统有
 * release_folio 回调则分派，否则尝试释放 buffer_heads。@gfp 告知能否
 * I/O/阻塞；返回 true 证明可继续释放 folio，false 要保留重试。
 */
bool filemap_release_folio(struct folio *folio, gfp_t gfp)
{
	struct address_space * const mapping = folio->mapping;

	BUG_ON(!folio_test_locked(folio));
	if (!folio_needs_release(folio))
		return true;
	if (folio_test_writeback(folio))
		/* I/O 完成路径仍可能访问私有状态，不能在此释放。 */
		return false;

	if (mapping && mapping->a_ops->release_folio)
		return mapping->a_ops->release_folio(folio, gfp);
	return try_to_free_buffers(folio);
}
EXPORT_SYMBOL(filemap_release_folio);

/**
 * filemap_invalidate_inode - Invalidate/forcibly write back a range of an inode's pagecache
 * @inode: The inode to flush
 * @flush: Set to write back rather than simply invalidate.
 * @start: First byte to in range.
 * @end: Last byte in range (inclusive), or LLONG_MAX for everything from start
 *       onwards.
 *
 * Invalidate all the folios on an inode that contribute to the specified
 * range, possibly writing them back first.  Whilst the operation is
 * undertaken, the invalidate lock is held to prevent new folios from being
 * installed.
 */
/*
 * 在 @inode page cache 的闭区间字节范围失效所有 folio，@flush
 * 决定是否先 writeback。持 invalidate write lock 阻止新 folio 插入，
 * 先 unmap PTE，再可选提交写回，最后等待/强制 invalidate。空 mapping、
 * 空范围快退。返回并消费 mapping 旧式错误位；可睡眠。
 */
int filemap_invalidate_inode(struct inode *inode, bool flush,
			     loff_t start, loff_t end)
{
	struct address_space *mapping = inode->i_mapping;
	pgoff_t first = start >> PAGE_SHIFT;
	pgoff_t last = end >> PAGE_SHIFT;
	pgoff_t nr = end == LLONG_MAX ? ULONG_MAX : last - first + 1;

	if (!mapping || !mapping->nrpages || end < start)
		goto out;

	/* Prevent new folios from being added to the inode. */
	/* 写锁覆盖 unmap/writeback/invalidate 整事务，关闭重新实例化窗口。 */
	filemap_invalidate_lock(mapping);

	if (!mapping->nrpages)
		goto unlock;

	unmap_mapping_pages(mapping, first, nr, false);

	/* Write back the data if we're asked to. */
	/* flush=false 可丢 clean/允许丢弃的数据；true 先保存脏内容。 */
	if (flush)
		filemap_fdatawrite_range(mapping, start, end);

	/* Wait for writeback to complete on all folios and discard. */
	/* 最终等待在飞 I/O 并摘除范围 folio，完成生命周期闭环。 */
	invalidate_inode_pages2_range(mapping, start / PAGE_SIZE, end / PAGE_SIZE);

unlock:
	filemap_invalidate_unlock(mapping);
out:
	return filemap_check_errors(mapping);
}
EXPORT_SYMBOL_GPL(filemap_invalidate_inode);

#ifdef CONFIG_CACHESTAT_SYSCALL
/**
 * filemap_cachestat() - compute the page cache statistics of a mapping
 * @mapping:	The mapping to compute the statistics for.
 * @first_index:	The starting page cache index.
 * @last_index:	The final page index (inclusive).
 * @cs:	the cachestat struct to write the result to.
 *
 * This will query the page cache statistics of a mapping in the
 * page range of [first_index, last_index] (inclusive). The statistics
 * queried include: number of dirty pages, number of pages marked for
 * writeback, and the number of (recently) evicted pages.
 */
/*
 * 在 [first_index,last_index] 统计 cache/dirty/writeback、
 * evicted 与 recently_evicted 的基本页数，写入 @cs。先在 RCU 外刷新 memcg
 * 统计；RCU 下只从 XArray entry/order/marks 推导，绝不解引用未 pin folio，
 * 以保持系统调用轻量。large entry 跨边界只计覆盖部分。
 *
 * value 视为 evicted；shmem swap value 需在 RCU 下取得 swapcache shadow，
 * swapoff 会等待该读侧。扫描可 cond_resched_rcu，结果是允许陈旧的弱一致
 * 快照，无错误返回。
 */
static void filemap_cachestat(struct address_space *mapping,
		pgoff_t first_index, pgoff_t last_index, struct cachestat *cs)
{
	XA_STATE(xas, &mapping->i_pages, first_index);
	struct folio *folio;

	/* Flush stats (and potentially sleep) outside the RCU read section. */
	/* 可能睡眠的 memcg flush 必须在进入 RCU 读临界区之前完成。 */
	mem_cgroup_flush_stats_ratelimited(NULL);

	rcu_read_lock();
	xas_for_each(&xas, folio, last_index) {
		int order;
		unsigned long nr_pages;
		pgoff_t folio_first_index, folio_last_index;

		/*
		 * Don't deref the folio. It is not pinned, and might
		 * get freed (and reused) underneath us.
		 *
		 * We *could* pin it, but that would be expensive for
		 * what should be a fast and lightweight syscall.
		 *
		 * Instead, derive all information of interest from
		 * the rcu-protected xarray.
		 */
		/*
		 * folio 未 pin，随时可释放复用，绝不能读其 flags；
		 * 为低开销只读 RCU 保护的 XArray order/marks/value 元数据。
		 */

		if (xas_retry(&xas, folio))
			continue;

		order = xas_get_order(&xas);
		nr_pages = 1 << order;
		folio_first_index = round_down(xas.xa_index, 1 << order);
		folio_last_index = folio_first_index + nr_pages - 1;

		/* Folios might straddle the range boundaries, only count covered pages */
		/* 大 folio/value 跨查询边界时裁掉区间外基本页。 */
		if (folio_first_index < first_index)
			nr_pages -= first_index - folio_first_index;

		if (folio_last_index > last_index)
			nr_pages -= folio_last_index - last_index;

		if (xa_is_value(folio)) {
			/* page is evicted */
			/* value 表示真实 cache folio 已不在 XArray。 */
			void *shadow = (void *)folio;
			bool workingset; /* not used */
			/* 接口要求的输出位，本路径只查询 shadow 存在性而不消费该值。 */

			cs->nr_evicted += nr_pages;

#ifdef CONFIG_SWAP /* implies CONFIG_MMU */
/* swap 支持依赖 MMU；仅此配置下解析 shmem swap entry 的 shadow。 */
			if (shmem_mapping(mapping)) {
				/* shmem file - in swap cache */
				/* shmem value 是 swap entry，需追到 swapcache shadow 判断近期性。 */
				swp_entry_t swp = radix_to_swp_entry(folio);

				/* swapin error results in poisoned entry */
				/* swapin poison 不是正常 swap entry，无法查询 shadow。 */
				if (!softleaf_is_swap(swp))
					goto resched;

				/*
				 * Getting a swap entry from the shmem
				 * inode means we beat
				 * shmem_unuse(). rcu_read_lock()
				 * ensures swapoff waits for us before
				 * freeing the swapper space. However,
				 * we can race with swapping and
				 * invalidation, so there might not be
				 * a shadow in the swapcache (yet).
				 */
				/*
				 * 从 shmem inode 读到 swap entry 说明抢在
				 * shmem_unuse 前；RCU 让 swapoff 延迟释放 swapper space。
				 * 但 swap/invalidating 并发下 shadow 可尚未建立或已消失。
				 */
				shadow = swap_cache_get_shadow(swp);
				if (!shadow)
					goto resched;
			}
#endif
			if (workingset_test_recent(shadow, true, &workingset, false))
				cs->nr_recently_evicted += nr_pages;

			goto resched;
		}

		/* page is in cache */
		/* 非 value entry 按 XArray order 计入当前 cache 页数。 */
		cs->nr_cache += nr_pages;

		if (xas_get_mark(&xas, PAGECACHE_TAG_DIRTY))
			cs->nr_dirty += nr_pages;

		if (xas_get_mark(&xas, PAGECACHE_TAG_WRITEBACK))
			cs->nr_writeback += nr_pages;

resched:
		/* 大范围扫描仅暂停 XArray 游标，不丢失已经累计的统计。 */
		if (need_resched()) {
			xas_pause(&xas);
			cond_resched_rcu();
		}
	}
	rcu_read_unlock();
}

/*
 * See mincore: reveal pagecache information only for files
 * that the calling process has write access to, or could (if
 * tried) open for writing.
 */
/*
 * 与 mincore 相同，page-cache residency 可能成为侧信道，只向
 * 已写打开、文件 owner/capable，或实际具 MAY_WRITE 权限者公开。
 *
 * can_do_cachestat() - 执行上述权限判定；@f 借用，返回 bool，permission
 * 检查可能走文件系统/LSM。
 */
static inline bool can_do_cachestat(struct file *f)
{
	if (f->f_mode & FMODE_WRITE)
		return true;
	if (file_owner_or_capable(f))
		return true;
	return file_permission(f, MAY_WRITE) == 0;
}

/*
 * The cachestat(2) system call.
 *
 * cachestat() returns the page cache statistics of a file in the
 * bytes range specified by `off` and `len`: number of cached pages,
 * number of dirty pages, number of pages marked for writeback,
 * number of evicted pages, and number of recently evicted pages.
 *
 * An evicted page is a page that is previously in the page cache
 * but has been evicted since. A page is recently evicted if its last
 * eviction was recent enough that its reentry to the cache would
 * indicate that it is actively being used by the system, and that
 * there is memory pressure on the system.
 *
 * `off` and `len` must be non-negative integers. If `len` > 0,
 * the queried range is [`off`, `off` + `len`]. If `len` == 0,
 * we will query in the range from `off` to the end of the file.
 *
 * The `flags` argument is unused for now, but is included for future
 * extensibility. User should pass 0 (i.e no flag specified).
 *
 * Currently, hugetlbfs is not supported.
 *
 * Because the status of a page can change after cachestat() checks it
 * but before it returns to the application, the returned values may
 * contain stale information.
 *
 * return values:
 *  zero        - success
 *  -EFAULT     - cstat or cstat_range points to an illegal address
 *  -EINVAL     - invalid flags
 *  -EBADF      - invalid file descriptor
 *  -EOPNOTSUPP - file descriptor is of a hugetlbfs file
 */
/*
 * cachestat(2) 读取用户范围，校验 fd/hugetlb/权限/flags，把
 * 字节区间换成 page index 后调用 filemap_cachestat，再复制快照到用户。
 * len==0 表示 off 到末尾，len>0 的最后页由 off+len-1 计算。
 *
 * 统计含 cache、dirty、writeback、evicted、recently evicted；页面状态可
 * 在检查后变化，结果允许陈旧。fd CLASS 自动关闭引用。返回 0 或
 * -EFAULT/-EINVAL/-EBADF/-EPERM/-EOPNOTSUPP。
 */
SYSCALL_DEFINE4(cachestat, unsigned int, fd,
		struct cachestat_range __user *, cstat_range,
		struct cachestat __user *, cstat, unsigned int, flags)
{
	CLASS(fd, f)(fd);
	struct address_space *mapping;
	struct cachestat_range csr;
	struct cachestat cs;
	/* first/last_index 是页索引闭区间，由用户字节范围换算。 */
	pgoff_t first_index, last_index;

	if (fd_empty(f))
		return -EBADF;

	if (copy_from_user(&csr, cstat_range,
			sizeof(struct cachestat_range)))
		return -EFAULT;

	/* hugetlbfs is not supported */
	/* hugetlb 不参与普通 page-cache/XArray 统计，明确拒绝。 */
	if (is_file_hugepages(fd_file(f)))
		return -EOPNOTSUPP;

	if (!can_do_cachestat(fd_file(f)))
		return -EPERM;

	if (flags != 0)
		return -EINVAL;

	/* 校验通过后把用户字节范围换算为含端点页索引，再执行只读扫描。 */
	first_index = csr.off >> PAGE_SHIFT;
	last_index =
		csr.len == 0 ? ULONG_MAX : (csr.off + csr.len - 1) >> PAGE_SHIFT;
	memset(&cs, 0, sizeof(struct cachestat));
	mapping = fd_file(f)->f_mapping;
	/* 查询只生成内核快照；最后一步一次性复制完整固定结构。 */
	filemap_cachestat(mapping, first_index, last_index, &cs);

	if (copy_to_user(cstat, &cs, sizeof(struct cachestat)))
		return -EFAULT;

	return 0;
}
#endif /* CONFIG_CACHESTAT_SYSCALL */
/* 关闭 CONFIG_CACHESTAT_SYSCALL 时不编译统计 helper 与系统调用。 */
