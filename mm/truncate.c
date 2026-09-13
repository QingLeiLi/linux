// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/truncate.c - code for taking down pages from address_spaces
 *
 * Copyright (C) 2002, Linus Torvalds
 *
 * 10Sep2002	Andrew Morton
 *		Initial version.
 */

#include <linux/kernel.h>
#include <linux/backing-dev.h>
#include <linux/dax.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/export.h>
#include <linux/pagemap.h>
/* pagemap 提供 XArray/page-cache 基础操作；后续头补充 folio、rmap 和文件系统特例。 */
#include <linux/highmem.h>
#include <linux/folio_batch.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/shmem_fs.h>
#include <linux/rmap.h>
/* 公开 MM、page-cache、DAX、shmem 与 rmap 接口共同定义截断时的锁和生命周期契约。 */
/* 本文件统一把文件字节范围转换为 page-cache folio/XArray 的删除与失效操作。 */
/* 包含顺序不影响行为；所有修改只发生在下方明确持锁或引用稳定的路径。 */
/* 截断先保证页缓存与页表失效，再允许文件系统释放底层块以避免旧页回写。 */
/* 所有地址范围均保持调用者的 inode 锁约束，本文件不自行取得高层文件系统锁。 */
#include "internal.h"

/*
 * clear_shadow_entries() - 从 page-cache XArray 删除非驻留 shadow 条目
 * 业务背景：截断/失效后旧 refault 历史不能继续代表文件范围；shmem 与 DAX 各自管理。
 * 入参：mapping 借用，start/max 是闭区间 page index；返回无。注意事项：inode i_lock
 * 与 XArray IRQ 锁保护删除和 workingset 更新，调用路径可睡眠但锁内不可。
 */
static void clear_shadow_entries(struct address_space *mapping,
				 unsigned long start, unsigned long max)
{
	XA_STATE(xas, &mapping->i_pages, start);
	struct folio *folio;

	/* Handled by shmem itself, or for DAX we do nothing. */
	/* 这些 mapping 的 exceptional entry 生命周期不属于通用 page-cache 截断协议。 */
	if (shmem_mapping(mapping) || dax_mapping(mapping))
		return;

	/* 删除 shadow 同步更新 workingset 节点年龄，避免遗留 refault 统计。 */
	xas_set_update(&xas, workingset_update_node);

	/* i_lock 与 XArray 锁共同稳定 mapping shrinkability、节点统计及条目删除。 */
	spin_lock(&mapping->host->i_lock);
	xas_lock_irq(&xas);

	/* Clear all shadow entries from start to max */
	/* value 项不是 folio，NULL 删除只影响 shadow，真实 folio 留给截断主路径处理。 */
	xas_for_each(&xas, folio, max) {
		if (xa_is_value(folio))
			xas_store(&xas, NULL);
	}

	xas_unlock_irq(&xas);
	if (mapping_shrinkable(mapping))
		inode_lru_list_add(mapping->host);
	spin_unlock(&mapping->host->i_lock);
}

/*
 * Unconditionally remove exceptional entries. Usually called from truncate
 * path. Note that the folio_batch may be altered by this function by removing
 * exceptional entries similar to what folio_batch_remove_exceptionals() does.
 * Please note that indices[] has entries in ascending order as guaranteed by
 * either find_get_entries() or find_lock_entries().
 */
/*
 * truncate_folio_batch_exceptionals() - 从批量查找结果清除 exceptional XArray 项
 * 入参：mapping 借用，fbatch/indices 是按升序匹配的输入输出批次；返回无。
 * 注意事项：DAX 必须先由文件系统断布局；普通 mapping 在 i_lock+XA 锁下删 value，
 * 最后同步移除 batch 中的 exceptional 项，保证后续 folio 循环只看真实 folio。
 */
static void truncate_folio_batch_exceptionals(struct address_space *mapping,
				struct folio_batch *fbatch, pgoff_t *indices)
{
	XA_STATE(xas, &mapping->i_pages, indices[0]);
	int nr = folio_batch_count(fbatch);
	struct folio *folio;
	int i, j;

	/* Handled by shmem itself */
	if (shmem_mapping(mapping))
		return;

	/* 先定位首个 exceptional，完全普通的 batch 无需获取 XArray 锁。 */
	for (j = 0; j < nr; j++)
		if (xa_is_value(fbatch->folios[j]))
			break;

	if (j == nr)
		return;

	/* DAX value 可能对应设备映射，发现残留说明上层布局断开协议被违反。 */
	if (dax_mapping(mapping)) {
		/* 每个残留 DAX entry 都必须删除，否则主循环会反复遇到同一 index。 */
		for (i = j; i < nr; i++) {
			if (xa_is_value(fbatch->folios[i])) {
				/*
				 * File systems should already have called
				 * dax_break_layout_entry() to remove all DAX
				 * entries while holding a lock to prevent
				 * establishing new entries. Therefore we
				 * shouldn't find any here.
				 */
				WARN_ON_ONCE(1);

				/*
				 * Delete the mapping so truncate_pagecache()
				 * doesn't loop forever.
				 */
				dax_delete_mapping_entry(mapping, indices[i]);
			}
		}
		goto out;
	}

	/* 从第一个 value 起定位，避免对已确认都是 folio 的前缀重复加锁扫描。 */
	xas_set(&xas, indices[j]);
	/* update callback 与后续 value 删除绑定，确保非驻留历史的统计同步撤销。 */
	xas_set_update(&xas, workingset_update_node);

	spin_lock(&mapping->host->i_lock);
	xas_lock_irq(&xas);

/* 遍历终点为本 batch 最大 index，保证升序 indices 对应的 shadow 均被覆盖。 */
/* 此后每次 xas_store(NULL) 均在相同锁域内，避免并发 reader 看见半更新节点。 */
	/* 只删除 exceptional value；真实 folio 属于当前 batch 的 truncate cleanup 流程。 */
	xas_for_each(&xas, folio, indices[nr-1]) {
		/* value 删除后 XArray 仍由当前锁稳定，随后才解锁并允许 shrinker 观察。 */
		if (xa_is_value(folio))
			xas_store(&xas, NULL);
	}

	xas_unlock_irq(&xas);
	if (mapping_shrinkable(mapping))
		inode_lru_list_add(mapping->host);
	spin_unlock(&mapping->host->i_lock);
out:
	folio_batch_remove_exceptionals(fbatch);
}

/**
 * folio_invalidate - Invalidate part or all of a folio.
 * @folio: The folio which is affected.
 * @offset: start of the range to invalidate
 * @length: length of the range to invalidate
 *
 * folio_invalidate() is called when all or part of the folio has become
 * invalidated by a truncate operation.
 *
 * folio_invalidate() does not have to release all buffers, but it must
 * ensure that no dirty buffer is left outside @offset and that no I/O
 * is underway against any of the blocks which are outside the truncation
 * point.  Because the caller is about to free (and possibly reuse) those
 * blocks on-disk.
 */
/* 文件系统必须保证截断外的 dirty buffer 与 I/O 都停止，因为其磁盘块即将复用。 */
void folio_invalidate(struct folio *folio, size_t offset, size_t length)
{
	/* aops 从当前 mapping 取得，folio 锁由调用者保持以稳定其私有 buffer 状态。 */
	const struct address_space_operations *aops = folio->mapping->a_ops;

	/* 文件系统回调负责撤销 buffer/private 状态及范围外 I/O；没有回调则无额外资源。 */
	if (aops->invalidate_folio)
		aops->invalidate_folio(folio, offset, length);
}
EXPORT_SYMBOL_GPL(folio_invalidate);

/*
 * If truncate cannot remove the fs-private metadata from the page, the page
 * becomes orphaned.  It will be left on the LRU and may even be mapped into
 * user pagetables if we're racing with filemap_fault().
 *
 * We need to bail out if page->mapping is no longer equal to the original
 * mapping.  This happens a) when the VM reclaimed the page while we waited on
 * its lock, b) when a concurrent invalidate_mapping_pages got there first and
 * c) when tmpfs swizzles a page between a tmpfs inode and swapper_space.
 */
/*
 * truncate_cleanup_folio() - 使待删 folio 脱离映射、私有块状态和 dirty 账本
 * 入参：folio 已由调用者锁定；返回无。注意事项：先 unmap 防止用户页表继续引用，
 * invalidate 回调必须在真正释放磁盘块前完成，dirty 取消放最后应对文件系统重脏。
 */
static void truncate_cleanup_folio(struct folio *folio)
{
	/* 映射撤销先于 release，避免页表继续指向即将交还文件系统的块。 */
	if (folio_mapped(folio))
		unmap_mapping_folio(folio);

	if (folio_needs_release(folio))
		folio_invalidate(folio, 0, folio_size(folio));

	/*
	 * Some filesystems seem to re-dirty the page even after
	 * the VM has canceled the dirty bit (eg ext3 journaling).
	 * Hence dirty accounting check is placed after invalidation.
	 */
	/* invalidate 后再取消 dirty，覆盖 journal 等回调可能重新标脏的情况。 */
	folio_cancel_dirty(folio);
}

/* 仅当 folio 仍属于原 mapping 才移除；并发 reclaim/invalidate 换 mapping 时返回 -EIO。 */
int truncate_inode_folio(struct address_space *mapping, struct folio *folio)
{
	/* mapping 不匹配说明等待 folio 锁期间被 reclaim/并发 invalidate 夺走，不能删除。 */
	if (folio->mapping != mapping)
		return -EIO;

	truncate_cleanup_folio(folio);
	filemap_remove_folio(folio);
	return 0;
}

/* 分裂大 folio 失败时解除非 shmem PMD 映射，使后续以 PTE refault 保持 EOF 的 SIGBUS 语义。 */
static int folio_split_or_unmap(struct folio *folio, struct page *split_at,
				    unsigned long min_order)
{
	enum ttu_flags ttu_flags =
		TTU_SYNC |
		TTU_SPLIT_HUGE_PMD |
		TTU_IGNORE_MLOCK;
	int ret;

	/* split_at 是保留/丢弃边界；min_order 保留 mapping 对最小大 folio 的约束。 */
	ret = folio_split(folio, min_order, split_at, NULL);

	/*
	 * If the split fails, unmap the folio, so it will be refaulted
	 * with PTEs to respect SIGBUS semantics.
	 *
	 * Make an exception for shmem/tmpfs that for long time
	 * intentionally mapped with PMDs across i_size.
	 */
	if (ret && !shmem_mapping(folio->mapping)) {
		try_to_unmap(folio, ttu_flags);
		WARN_ON(folio_mapped(folio));
	}

	return ret;
}

/*
 * Handle partial folios.  The folio may be entirely within the
 * range if a split has raced with us.  If not, we zero the part of the
 * folio that's within the [start, end] range, and then split the folio if
 * it's large.  split_page_range() will discard pages which now lie beyond
 * i_size, and we rely on the caller to discard pages which lie within a
 * newly created hole.
 *
 * Returns false if splitting failed so the caller can avoid
 * discarding the entire folio which is stubbornly unsplit.
 */
/*
 * truncate_inode_partial_folio() - 处理截断区边界上未完全覆盖的 folio
 * 入参：folio 已锁定，start/end 为含端点文件字节范围；返回 false 表示脏大 folio
 * 不能安全拆除，调用者需保留它。注意事项：等待 writeback 后先零可保留区域，再释放
 * 文件系统私有状态；大 folio 分裂失败时以 unmap 回退，不能错误丢弃有效部分。
 */
bool truncate_inode_partial_folio(struct folio *folio, loff_t start, loff_t end)
{
	loff_t pos = folio_pos(folio);
	size_t size = folio_size(folio);
	unsigned int offset, length;
	struct page *split_at, *split_at2;
	unsigned int min_order;

	if (pos < start)
		/* start 位于 folio 内时保留其前缀，否则从 folio 起点开始处理。 */
		offset = start - pos;
	else
		offset = 0;
	if (pos + size <= (u64)end)
		/* 整个尾部位于范围内时长度到 folio 末端，否则仅覆盖终点前的部分。 */
		length = size - offset;
	else
		length = end + 1 - pos - offset;

	/* 等待旧 I/O 完成，否则随后释放的块可能仍被设备写回。 */
	folio_wait_writeback(folio);
	if (length == size) {
		truncate_inode_folio(folio->mapping, folio);
		return true;
	}

	/*
	 * We may be zeroing pages we're about to discard, but it avoids
	 * doing a complex calculation here, and then doing the zeroing
	 * anyway if the page split fails.
	 */
	/* 不可访问 mapping 不允许直接清零；普通文件先抹去仍保留的边界字节。 */
	if (!mapping_inaccessible(folio->mapping))
		folio_zero_range(folio, offset, length);

	if (folio_needs_release(folio))
		folio_invalidate(folio, offset, length);
	if (!folio_test_large(folio))
		return true;

	min_order = mapping_min_folio_order(folio->mapping);
	split_at = folio_page(folio, PAGE_ALIGN_DOWN(offset) / PAGE_SIZE);
	/* 第一次拆分成功后可能得到第二个边界 folio，继续尝试减少 shmem 内存浪费。 */
	if (!folio_split_or_unmap(folio, split_at, min_order)) {
		/*
		 * try to split at offset + length to make sure folios within
		 * the range can be dropped, especially to avoid memory waste
		 * for shmem truncate
		 */
		struct folio *folio2;

		/* 若处理范围正好到 folio 末端，第二边界不存在，直接进入统一返回。 */
		if (offset + length == size)
			goto no_split;

		split_at2 = folio_page(folio,
				PAGE_ALIGN_DOWN(offset + length) / PAGE_SIZE);
		folio2 = page_folio(split_at2);

		/* 临时引用跨越 trylock，避免并发拆分/回收使 folio2 地址失效。 */
		if (!folio_try_get(folio2))
			goto no_split;

		if (!folio_test_large(folio2))
			goto out;

		if (!folio_trylock(folio2))
			goto out;

		/* make sure folio2 is large and does not change its mapping */
		if (folio_test_large(folio2) &&
		    folio2->mapping == folio->mapping)
			/* 重新验证 large 与 mapping，防止第一次拆分已改变第二对象身份。 */
			folio_split_or_unmap(folio2, split_at2, min_order);

		folio_unlock(folio2);
out:
		folio_put(folio2);
no_split:
		return true;
	}
	/* 未拆分的大 folio 若仍 dirty，删除会丢数据，向调用者报告保留失败。 */
	if (folio_test_dirty(folio))
		return false;
	truncate_inode_folio(folio->mapping, folio);
	return true;
}

/*
 * Used to get rid of pages on hardware memory corruption.
 */
/* 硬件错误只对普通文件数据页挖洞；目录等类型需文件系统额外审计，故拒绝。 */
int generic_error_remove_folio(struct address_space *mapping,
		struct folio *folio)
{
	if (!mapping)
		return -EINVAL;
	/*
	 * Only punch for normal data pages for now.
	 * Handling other types like directories would need more auditing.
	 */
	if (!S_ISREG(mapping->host->i_mode))
		return -EIO;
	return truncate_inode_folio(mapping, folio);
}
EXPORT_SYMBOL(generic_error_remove_folio);

/**
 * mapping_evict_folio() - Remove an unused folio from the page-cache.
 * @mapping: The mapping this folio belongs to.
 * @folio: The folio to remove.
 *
 * Safely remove one folio from the page cache.
 * It only drops clean, unused folios.
 *
 * Context: Folio must be locked.
 * Return: The number of pages successfully removed.
 */
/*
 * mapping_evict_folio() - 删除一个 clean、未映射、无额外引用的 page-cache folio
 * 入参：mapping/folio 借用且 folio 已锁；返回实际移除页数或 0。
 * 注意事项：refcount 阈值排除用户映射和并发持有者；release 回调失败不得强删私有状态。
 */
long mapping_evict_folio(struct address_space *mapping, struct folio *folio)
{
	/* The page may have been truncated before it was locked */
	if (!mapping)
		/* 已被截断/回收的 mapping 无需再次操作，返回 0 表示本轮未驱逐。 */
		return 0;
	if (folio_test_dirty(folio) || folio_test_writeback(folio))
		/* 脏或 writeback folio 不能由轻量 eviction 丢弃，须走截断或回写路径。 */
		return 0;
	/* The refcount will be elevated if any page in the folio is mapped */
	/* 每个 base page 的 cache 引用、可选 private 引用和当前锁定引用是允许的下限。 */
	if (folio_ref_count(folio) >
			folio_nr_pages(folio) + folio_has_private(folio) + 1)
		return 0;
	if (!filemap_release_folio(folio, 0))
		return 0;

	return remove_mapping(mapping, folio);
}

/**
 * truncate_inode_pages_range - truncate range of pages specified by start & end byte offsets
 * @mapping: mapping to truncate
 * @lstart: offset from which to truncate
 * @lend: offset to which to truncate (inclusive)
 *
 * Truncate the page cache, removing the pages that are between
 * specified offsets (and zeroing out partial pages
 * if lstart or lend + 1 is not page aligned).
 *
 * Truncate takes two passes - the first pass is nonblocking.  It will not
 * block on page locks and it will not block on writeback.  The second pass
 * will wait.  This is to prevent as much IO as possible in the affected region.
 * The first pass will remove most pages, so the search cost of the second pass
 * is low.
 *
 * We pass down the cache-hot hint to the page freeing code.  Even if the
 * mapping is large, it is probably the case that the final pages are the most
 * recently touched, and freeing happens in ascending file offset order.
 *
 * Note that since ->invalidate_folio() accepts range to invalidate
 * truncate_inode_pages_range is able to handle cases where lend + 1 is not
 * page aligned properly.
 */
void truncate_inode_pages_range(struct address_space *mapping,
				loff_t lstart, uoff_t lend)
{
	/* 变量地图：start/end 管理完整页，fbatch/indices 管理临时引用，same_folio 消除边界重叠。 */
	pgoff_t		start;		/* inclusive */
	pgoff_t		end;		/* exclusive */
	struct folio_batch fbatch;
	pgoff_t		indices[FOLIO_BATCH_SIZE];
	pgoff_t		index;
	int		i;
	struct folio	*folio;
	bool		same_folio;

	/* 空 mapping 无需取得 XArray/folio 锁，快速返回避免无意义的截断扫描。 */
	if (mapping_empty(mapping))
		return;

	/*
	 * 'start' and 'end' always covers the range of pages to be fully
	 * truncated. Partial pages are covered with 'partial_start' at the
	 * start of the range and 'partial_end' at the end of the range.
	 * Note that 'end' is exclusive while 'lend' is inclusive.
	 */
	/* 完整删除区使用 page index 半开区间；边界部分由随后 partial folio 路径处理。 */
	start = (lstart + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (lend == -1)
		/*
		 * lend == -1 indicates end-of-file so we have to set 'end'
		 * to the highest possible pgoff_t and since the type is
		 * unsigned we're using -1.
		 */
		/* 无符号 pgoff_t 的全一值是最大可扫描 index，表示到 EOF 的无限上界。 */
		end = -1;
	else
		end = (lend + 1) >> PAGE_SHIFT;

	folio_batch_init(&fbatch);
	index = start;
	/* 索引随 find_lock_entries 前进；每批处理完释放引用再 cond_resched 防止长文件饿死 CPU。 */
	/* 第一遍只领取当前可锁 folio，尽量不等待锁/写回以缩短受影响范围的 I/O 干扰。 */
	while (index < end && find_lock_entries(mapping, &index, end - 1,
			&fbatch, indices)) {
		truncate_folio_batch_exceptionals(mapping, &fbatch, indices);
		/* cleanup 先撤映射/私有状态，随后批量从 page cache 摘除并解锁。 */
		for (i = 0; i < folio_batch_count(&fbatch); i++)
			truncate_cleanup_folio(fbatch.folios[i]);
		delete_from_page_cache_batch(mapping, &fbatch);
		for (i = 0; i < folio_batch_count(&fbatch); i++)
			folio_unlock(fbatch.folios[i]);
		folio_batch_release(&fbatch);
		cond_resched();
	}

	/* 边界 folio 可能相同，避免第二次处理后把同一对象再删/再解锁。 */
	same_folio = (lstart >> PAGE_SHIFT) == (lend >> PAGE_SHIFT);
	folio = __filemap_get_folio(mapping, lstart >> PAGE_SHIFT, FGP_LOCK, 0);
	/* 边界查询可能因不存在、竞争或错误返回 ERR；非错误 folio 均由本路径解锁并 put。 */
	if (!IS_ERR(folio)) {
		/* first boundary partial 失败时收缩完整页扫描的起点，保留无法安全删除的 folio。 */
		same_folio = lend < folio_next_pos(folio);
		/* 以实际 folio 尾位置重算 same_folio，large folio 可跨越多个 page index。 */
		if (!truncate_inode_partial_folio(folio, lstart, lend)) {
			start = folio_next_index(folio);
			if (same_folio)
				end = folio->index;
		}
		folio_unlock(folio);
		folio_put(folio);
		folio = NULL;
	}

	if (!same_folio) {
		/* 两端不同才处理终点，否则同一个 folio 的一次 partial 操作已覆盖全部范围。 */
		/* 另一端只有不同页时才单独锁定；partial helper 可收缩 end 以保护未拆大 folio。 */
		folio = __filemap_get_folio(mapping, lend >> PAGE_SHIFT,
						FGP_LOCK, 0);
		/* 第二端若不存在 cache folio，无需制造新页；范围主循环仍会清中间项。 */
		if (!IS_ERR(folio)) {
			if (!truncate_inode_partial_folio(folio, lstart, lend))
				end = folio->index;
			folio_unlock(folio);
		/* 锁、写回等待和 filemap 移除组成一个顺序单元，不能先释放 folio 引用。 */
			folio_put(folio);
		}
	}

/* 到此边界 folio 已处理；剩余 [start,end) 只含完整页，进入可等待的收敛扫描。 */
/* second pass 的循环即使遭遇并发删除也会重启，最终保证索引范围不残留可见 folio。 */
	/* 第二遍允许逐个等待，反复扫描直到范围内真实 folio 和 exceptional 项都消失。 */
	index = start;
	while (index < end) {
		cond_resched();
		/* 无条目但游标推进说明并发删除制造洞，重启确保起点前后无遗漏。 */
		if (!find_get_entries(mapping, &index, end - 1, &fbatch,
				indices)) {
			/* If all gone from start onwards, we're done */
			/* 游标未推进说明 start 之后已无项，推进过则回到 start 验证全范围。 */
			if (index == start)
				break;
			/* Otherwise restart to make sure all gone */
			index = start;
			continue;
		}

		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			struct folio *folio = fbatch.folios[i];

			/* We rely upon deletion not changing folio->index */

			if (xa_is_value(folio))
				/* exceptional 已由批后 helper 统一清理，不能当作可锁 folio。 */
				continue;

			folio_lock(folio);
			/* 锁后验证 index 仍位于 folio，防止并发 truncate/split 改变批次关联。 */
			/* writeback 完成后才调用 truncate_inode_folio，避免释放中的磁盘块仍被 I/O 使用。 */
			VM_BUG_ON_FOLIO(!folio_contains(folio, indices[i]), folio);
			folio_wait_writeback(folio);
			/* 该第二遍可阻塞，目的是保证第一遍跳过的 busy folio 最终也完成失效。 */
			truncate_inode_folio(mapping, folio);
			folio_unlock(folio);
		}
		truncate_folio_batch_exceptionals(mapping, &fbatch, indices);
		folio_batch_release(&fbatch);
	}
}
EXPORT_SYMBOL(truncate_inode_pages_range);

/**
 * truncate_inode_pages - truncate *all* the pages from an offset
 * @mapping: mapping to truncate
 * @lstart: offset from which to truncate
 *
 * Called under (and serialised by) inode->i_rwsem and
 * mapping->invalidate_lock.
 *
 * Note: When this function returns, there can be a page in the process of
 * deletion (inside __filemap_remove_folio()) in the specified range.  Thus
 * mapping->nrpages can be non-zero when this function returns even after
 * truncation of the whole mapping.
 */
void truncate_inode_pages(struct address_space *mapping, loff_t lstart)
{
	/* 此 wrapper 的成功不意味着 nrpages 立即为零，底层异步 remove 仍可能在收尾。 */
	/* 范围包装不新增锁；上层负责 inode i_rwsem 与 invalidate_lock 的串行化。 */
	/* -1 作为无符号终点覆盖到 EOF；实际锁序由调用者的 i_rwsem/invalidate_lock 保证。 */
	truncate_inode_pages_range(mapping, lstart, (loff_t)-1);
}
EXPORT_SYMBOL(truncate_inode_pages);

/**
 * truncate_inode_pages_final - truncate *all* pages before inode dies
 * @mapping: mapping to truncate
 *
 * Called under (and serialized by) inode->i_rwsem.
 *
 * Filesystems have to use this in the .evict_inode path to inform the
 * VM that this is the final truncate and the inode is going away.
 */
void truncate_inode_pages_final(struct address_space *mapping)
{
	/*
	 * Page reclaim can not participate in regular inode lifetime
	 * management (can't call iput()) and thus can race with the
	 * inode teardown.  Tell it when the address space is exiting,
	 * so that it does not install eviction information after the
	 * final truncate has begun.
	 */
	/* 先发布 AS_EXITING 阻止 reclaim 在 inode 销毁期间重新安装 eviction 信息。 */
	mapping_set_exiting(mapping);
	/* 发布后再观察空树；否则先检查为空与并发 reclaim 添加 eviction 信息之间会竞态。 */
	/* 与无锁 XArray 查找并发的修改者必须先经过一次锁循环，才能确认看见 AS_EXITING。 */

	if (!mapping_empty(mapping)) {
		/* 锁循环不修改树，只等待先前未见 AS_EXITING 的无锁修改者退出临界区。 */
		/*
		 * As truncation uses a lockless tree lookup, cycle
		 * the tree lock to make sure any ongoing tree
		 * modification that does not see AS_EXITING is
		 * completed before starting the final truncate.
		 */
		xa_lock_irq(&mapping->i_pages);
		xa_unlock_irq(&mapping->i_pages);
	}

	truncate_inode_pages(mapping, 0);
}
EXPORT_SYMBOL(truncate_inode_pages_final);

/**
 * mapping_try_invalidate - Invalidate all the evictable folios of one inode
 * @mapping: the address_space which holds the folios to invalidate
 * @start: the offset 'from' which to invalidate
 * @end: the offset 'to' which to invalidate (inclusive)
 * @nr_failed: How many folio invalidations failed
 *
 * This function is similar to invalidate_mapping_pages(), except that it
 * returns the number of folios which could not be evicted in @nr_failed.
 */
unsigned long mapping_try_invalidate(struct address_space *mapping,
		pgoff_t start, pgoff_t end, unsigned long *nr_failed)
{
	/* 变量地图：index 是扫描游标，count 是已失效索引数，nr_failed 是可选失败输出。 */
	pgoff_t indices[FOLIO_BATCH_SIZE];
	struct folio_batch fbatch;
	pgoff_t index = start;
	unsigned long ret;
	unsigned long count = 0;
	int i;

	/* 这是不等待 I/O 的提示性回收；nr_failed 将无法驱逐的远端/LRU folio 反馈调用者。 */
	folio_batch_init(&fbatch);
	while (find_lock_entries(mapping, &index, end, &fbatch, indices)) {
		/* 每批完成后解除所有 lock/ref，再让调度器运行以控制大 mapping 的停顿。 */
		/* xa_has_values 记录本批是否还需清 nonresident shadow；count 统计索引而非仅 folio。 */
		bool xa_has_values = false;
		int nr = folio_batch_count(&fbatch);

		for (i = 0; i < nr; i++) {
			struct folio *folio = fbatch.folios[i];

			/* We rely upon deletion not changing folio->index */

			if (xa_is_value(folio)) {
				xa_has_values = true;
				count++;
				continue;
			}

			/* eviction 失败不强制写回或阻塞，转为 deactivate 提高后续 reclaim 概率。 */
			/* mapping_evict 只允许 clean/unused，返回页数供 count 保持 folio 大小感知。 */
			ret = mapping_evict_folio(mapping, folio);
			/* ret=0 不代表错误，只表示此刻有 dirty/映射/引用阻止轻量失效。 */
			/* 释放 find_lock_entries 交付的锁后，folio 只由 batch 临时引用保持到本轮结束。 */
			folio_unlock(folio);
			/*
			 * Invalidation is a hint that the folio is no longer
			 * of interest and try to speed up its reclaim.
			 */
			if (!ret) {
				/* deactivate 只是回收提示，不改变 mapping 所有权或替代调用者的后续重试。 */
				deactivate_file_folio(folio);
				/* Likely in the lru cache of a remote CPU */
				if (nr_failed)
					(*nr_failed)++;
			}
			count += ret;
		}

		/* xa_has_values 为真时再删除 shadow，避免为纯 folio batch 额外取得 mapping 的两把锁。 */
		if (xa_has_values)
			clear_shadow_entries(mapping, indices[0], indices[nr-1]);

		folio_batch_remove_exceptionals(&fbatch);
		folio_batch_release(&fbatch);
		/* 让出 CPU 前本批没有任何锁或临时引用，下一轮可安全从更新后的 index 继续。 */
		cond_resched();
	}
	return count;
}

/**
 * invalidate_mapping_pages - Invalidate all clean, unlocked cache of one inode
 * @mapping: the address_space which holds the cache to invalidate
 * @start: the offset 'from' which to invalidate
 * @end: the offset 'to' which to invalidate (inclusive)
 *
 * This function removes pages that are clean, unmapped and unlocked,
 * as well as shadow entries. It will not block on IO activity.
 *
 * If you want to remove all the pages of one inode, regardless of
 * their use and writeback state, use truncate_inode_pages().
 *
 * Return: The number of indices that had their contents invalidated
 */
unsigned long invalidate_mapping_pages(struct address_space *mapping,
		pgoff_t start, pgoff_t end)
{
	/* 公开轻量接口不需要失败计数，直接复用不阻塞核心实现。 */
	/* 调用者若需要精确失败原因应使用 mapping_try_invalidate 的 nr_failed 变体。 */
	return mapping_try_invalidate(mapping, start, end, NULL);
}
EXPORT_SYMBOL(invalidate_mapping_pages);

/* 若 dirty 且文件系统提供 launder 回调，先同步其私有写回/清理，否则保留原状态。 */
static int folio_launder(struct address_space *mapping, struct folio *folio)
{
	/* launder 回调是文件系统的可选提交点，返回 errno 阻止强失效继续破坏其状态。 */
	/* 该 helper 不持有新引用，调用者必须在回调后重新验证 mapping 归属。 */
	if (!folio_test_dirty(folio))
		return 0;
	if (folio->mapping != mapping || mapping->a_ops->launder_folio == NULL)
		return 0;
	return mapping->a_ops->launder_folio(folio);
}

/*
 * This is like mapping_evict_folio(), except it ignores the folio's
 * refcount.  We do this because invalidate_inode_pages2() needs stronger
 * invalidation guarantees, and cannot afford to leave folios behind because
 * shrink_folio_list() has a temp ref on them, or because they're transiently
 * sitting in the folio_add_lru() caches.
 */
int folio_unmap_invalidate(struct address_space *mapping, struct folio *folio,
			   gfp_t gfp)
{
	/* 忽略临时 refcount 以实现强失效，但仍严格保留 dirty、private 与 mapping 的安全检查。 */
	void (*free_folio)(struct folio *);
	int ret;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	/* 调用者持锁确保 map/release/private 回调观察的 folio 状态不会并发切换。 */

	/* 强失效先撤页表，再在 folio 锁下清理/释放，确保用户不能继续命中旧 cache。 */
	if (folio_mapped(folio))
		unmap_mapping_folio(folio);
	BUG_ON(folio_mapped(folio));

	/* launder 可写回或释放文件系统状态；失败时保持 folio 在 cache 供调用者重试。 */
	ret = folio_launder(mapping, folio);
	if (ret)
		return ret;
	if (folio->mapping != mapping)
		return -EBUSY;
	if (!filemap_release_folio(folio, gfp))
		return -EBUSY;

	/* 删除前同时锁 inode 与 XArray，避免并发插入/状态修改观察到半摘除 folio。 */
	spin_lock(&mapping->host->i_lock);
	xa_lock_irq(&mapping->i_pages);
	if (folio_test_dirty(folio))
		/* 发现再次变脏时跳 failed，不摘 XArray 条目也不调用 free_folio。 */
		goto failed;

	BUG_ON(folio_has_private(folio));
	__filemap_remove_folio(folio, NULL);
	xa_unlock_irq(&mapping->i_pages);
	if (mapping_shrinkable(mapping))
		inode_lru_list_add(mapping->host);
	free_folio = mapping->a_ops->free_folio;
	spin_unlock(&mapping->host->i_lock);

	if (free_folio)
		free_folio(folio);
	/* filemap 已摘除后由 folio_put_refs 归还所有 base page 引用，free 回调不得再持有它。 */
	folio_put_refs(folio, folio_nr_pages(folio));
	return 1;
/* failed 路径尚未摘 XArray 项，释放锁后返回 -EBUSY 保留 folio 的原 ownership。 */
failed:
	xa_unlock_irq(&mapping->i_pages);
	spin_unlock(&mapping->host->i_lock);
	return -EBUSY;
}

/**
 * invalidate_inode_pages2_range - remove range of pages from an address_space
 * @mapping: the address_space
 * @start: the page offset 'from' which to invalidate
 * @end: the page offset 'to' which to invalidate (inclusive)
 *
 * Any pages which are found to be mapped into pagetables are unmapped prior to
 * invalidation.
 *
 * Return: -EBUSY if any pages could not be invalidated.
 */
int invalidate_inode_pages2_range(struct address_space *mapping,
				  pgoff_t start, pgoff_t end)
{
	/* 变量地图：ret 合并失败，ret2 是当前 folio 结果，did_range_unmap 记录一次性 zap。 */
	pgoff_t indices[FOLIO_BATCH_SIZE];
	struct folio_batch fbatch;
	pgoff_t index;
	int i;
	int ret = 0;
	int ret2 = 0;
	int did_range_unmap = 0;

	/* 强失效也保留空 mapping 快速路径，避免无意义的批量与锁初始化。 */
	if (mapping_empty(mapping))
		return 0;

	folio_batch_init(&fbatch);
	index = start;
	/* get 路径允许逐项锁和等待；批次引用保证扫描期间 folio 内存不释放。 */
	while (find_get_entries(mapping, &index, end, &fbatch, indices)) {
		bool xa_has_values = false;
		int nr = folio_batch_count(&fbatch);

		for (i = 0; i < nr; i++) {
			struct folio *folio = fbatch.folios[i];

			/* We rely upon deletion not changing folio->index */

			/* DAX exceptional 项需同步断开设备布局；普通 value 在批后统一清 shadow。 */
			if (xa_is_value(folio)) {
			/* DAX entry 的同步失效由设备布局层确认，普通 exceptional 留到 batch 收尾。 */
				xa_has_values = true;
				if (dax_mapping(mapping) &&
				    !dax_invalidate_mapping_entry_sync(mapping, indices[i]))
					ret = -EBUSY;
				continue;
			}

			/* 首个 mapped folio 触发范围 unmap，避免逐 folio zap 的高额 TLB 成本。 */
			if (!did_range_unmap && folio_mapped(folio)) {
			/* did_range_unmap 防止每个 mapped folio 触发一次重叠的大范围 TLB shootdown。 */
				/*
				 * If folio is mapped, before taking its lock,
				 * zap the rest of the file in one hit.
				 */
				unmap_mapping_pages(mapping, indices[i],
						(1 + end - indices[i]), false);
				did_range_unmap = 1;
			}

			folio_lock(folio);
		/* 锁后 mapping 可改变，必须重新验证再等待写回并执行强失效。 */
			if (unlikely(folio->mapping != mapping)) {
				/* 竞争摘除的 folio 只解锁，不能调用强失效或影响本 mapping 的 ret。 */
				folio_unlock(folio);
				continue;
			}
			VM_BUG_ON_FOLIO(!folio_contains(folio, indices[i]), folio);
			folio_wait_writeback(folio);
			/* 写回结束后 folio_unmap_invalidate 可能返回 -EBUSY；该结果汇入 ret 但不终止批次。 */
			ret2 = folio_unmap_invalidate(mapping, folio, GFP_KERNEL);
			if (ret2 < 0)
				ret = ret2;
			folio_unlock(folio);
		}

		/* 本批完成后才处理 shadow，确保 indices 仍准确描述本次 get 的 exceptional 项范围。 */
		/* ret 即使记录 -EBUSY 仍继续扫描，调用者得到尽可能大的已失效范围和最终失败状态。 */
		if (xa_has_values)
			clear_shadow_entries(mapping, indices[0], indices[nr-1]);
		/* batch exceptional 和引用均在此释放，之后 cond_resched 不携带上一批状态。 */

		folio_batch_remove_exceptionals(&fbatch);
		folio_batch_release(&fbatch);
		/* 强失效同样在批间让出 CPU，避免同步等待大量 writeback 时长期占用执行上下文。 */
		cond_resched();
	}
	/*
	 * For DAX we invalidate page tables after invalidating page cache.  We
	 * could invalidate page tables while invalidating each entry however
	 * that would be expensive. And doing range unmapping before doesn't
	 * work as we have no cheap way to find whether page cache entry didn't
	 * get remapped later.
	 */
	/* DAX 在 cache 条目处理后统一 unmap，防止新映射在前置 unmap 后再次建立。 */
	if (dax_mapping(mapping)) {
		unmap_mapping_pages(mapping, start, end - start + 1, false);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(invalidate_inode_pages2_range);

/**
 * invalidate_inode_pages2 - remove all pages from an address_space
 * @mapping: the address_space
 *
 * Any pages which are found to be mapped into pagetables are unmapped prior to
 * invalidation.
 *
 * Return: -EBUSY if any pages could not be invalidated.
 */
int invalidate_inode_pages2(struct address_space *mapping)
{
	/* 无符号 -1 覆盖全部索引；返回任一无法强失效 folio 的 -EBUSY。 */
	return invalidate_inode_pages2_range(mapping, 0, -1);
}
EXPORT_SYMBOL_GPL(invalidate_inode_pages2);

/**
 * truncate_pagecache - unmap and remove pagecache that has been truncated
 * @inode: inode
 * @newsize: new file size
 *
 * inode's new i_size must already be written before truncate_pagecache
 * is called.
 *
 * This function should typically be called before the filesystem
 * releases resources associated with the freed range (eg. deallocates
 * blocks). This way, pagecache will always stay logically coherent
 * with on-disk format, and the filesystem would not have to deal with
 * situations such as writepage being called for a page that has already
 * had its underlying blocks deallocated.
 */
void truncate_pagecache(struct inode *inode, loff_t newsize)
{
	struct address_space *mapping = inode->i_mapping;
	loff_t holebegin = round_up(newsize, PAGE_SIZE);

	/*
	 * unmap_mapping_range is called twice, first simply for
	 * efficiency so that truncate_inode_pages does fewer
	 * single-page unmaps.  However after this first call, and
	 * before truncate_inode_pages finishes, it is possible for
	 * private pages to be COWed, which remain after
	 * truncate_inode_pages finishes, hence the second
	 * unmap_mapping_range call must be made for correctness.
	 */
	/* 第一次预先批量 zap，第二次覆盖截断期间新 COW 私页，二者缺一会留下旧映射。 */
	unmap_mapping_range(mapping, holebegin, 0, 1);
	truncate_inode_pages(mapping, newsize);
	unmap_mapping_range(mapping, holebegin, 0, 1);
}
EXPORT_SYMBOL(truncate_pagecache);

/**
 * truncate_setsize - update inode and pagecache for a new file size
 * @inode: inode
 * @newsize: new file size
 *
 * truncate_setsize updates i_size and performs pagecache truncation (if
 * necessary) to @newsize. It will be typically be called from the filesystem's
 * setattr function when ATTR_SIZE is passed in.
 *
 * Must be called with a lock serializing truncates and writes (generally
 * i_rwsem but e.g. xfs uses a different lock) and before all filesystem
 * specific block truncation has been performed.
 */
void truncate_setsize(struct inode *inode, loff_t newsize)
{
	loff_t oldsize = inode->i_size;
	/* i_rwsem 或等价锁由调用者持有，oldsize/newsize 的比较在该锁下稳定。 */
	/* 扩展先补 pagecache EOF 边界，缩小则由 truncate_pagecache 删除超出新大小的 cache。 */

	/* 先发布新 i_size，再处理 pagecache；缺页在 folio 解锁后必须看到正确 EOF。 */
	i_size_write(inode, newsize);
	if (newsize > oldsize)
		pagecache_isize_extended(inode, oldsize, newsize);
	truncate_pagecache(inode, newsize);
}
EXPORT_SYMBOL(truncate_setsize);

/**
 * pagecache_isize_extended - update pagecache after extension of i_size
 * @inode:	inode for which i_size was extended
 * @from:	original inode size
 * @to:		new inode size
 *
 * Handle extension of inode size either caused by extending truncate or
 * by write starting after current i_size.  We mark the page straddling
 * current i_size RO so that page_mkwrite() is called on the first
 * write access to the page.  The filesystem will update its per-block
 * information before user writes to the page via mmap after the i_size
 * has been changed.
 *
 * The function must be called after i_size is updated so that page fault
 * coming after we unlock the folio will already see the new i_size.
 * The function must be called while we still hold i_rwsem - this not only
 * makes sure i_size is stable but also that userspace cannot observe new
 * i_size value before we are prepared to store mmap writes at new inode size.
 */
void pagecache_isize_extended(struct inode *inode, loff_t from, loff_t to)
{
	int bsize = i_blocksize(inode);
	/* rounded_from 将旧 EOF 对齐到文件系统块边界，决定是否存在需保护的同页 hole。 */
	loff_t rounded_from;
	struct folio *folio;

	WARN_ON(to > inode->i_size);
	/* 调用顺序违例只告警；后续仍按当前 i_size 的可见范围保守处理。 */

	/* 块已至少一页或无实际扩展时不存在需要写保护/清零的跨 EOF 页。 */
	if (from >= to || bsize >= PAGE_SIZE)
		return;
	/* Page straddling @from will not have any hole block created? */
	rounded_from = round_up(from, bsize);
	if (to <= rounded_from || !(rounded_from & (PAGE_SIZE - 1)))
		return;

	/* 锁住跨旧 EOF 的 folio，防止 mmap 写在文件系统补块信息前绕过 page_mkwrite。 */
	folio = filemap_lock_folio(inode->i_mapping, from / PAGE_SIZE);
	/* Folio not cached? Nothing to do */
	if (IS_ERR(folio))
		return;
	/*
	 * See folio_clear_dirty_for_io() for details why folio_mark_dirty()
	 * is needed.
	 */
	if (folio_mkclean(folio))
		folio_mark_dirty(folio);

	/*
	 * The post-eof range of the folio must be zeroed before it is exposed
	 * to the file. Writeback normally does this, but since i_size has been
	 * increased we handle it here.
	 */
	/* dirty folio 的 post-EOF 字节可能含旧数据，扩展前必须归零以防信息泄露。 */
	if (folio_test_dirty(folio)) {
		/* offset/end 均相对 folio 起点，min 防止扩展终点跨入下一个 folio。 */
		unsigned int offset, end;

		offset = from - folio_pos(folio);
		end = min_t(unsigned int, to - folio_pos(folio),
			    folio_size(folio));
		folio_zero_segment(folio, offset, end);
	}

	/* 清零完成后才解锁/put；mmap fault 此后看到扩展 i_size 时不会读取旧页内数据。 */
	folio_unlock(folio);
	folio_put(folio);
}
EXPORT_SYMBOL(pagecache_isize_extended);

/**
 * truncate_pagecache_range - unmap and remove pagecache that is hole-punched
 * @inode: inode
 * @lstart: offset of beginning of hole
 * @lend: offset of last byte of hole
 *
 * This function should typically be called before the filesystem
 * releases resources associated with the freed range (eg. deallocates
 * blocks). This way, pagecache will always stay logically coherent
 * with on-disk format, and the filesystem would not have to deal with
 * situations such as writepage being called for a page that has already
 * had its underlying blocks deallocated.
 */
void truncate_pagecache_range(struct inode *inode, loff_t lstart, loff_t lend)
{
	/* 字节范围为闭区间；unmap 仅针对中间完整页，实际边界清理委托 truncate_inode_pages_range。 */
	/* 调用者应已序列化文件大小和块释放；本函数不修改 inode->i_size。 */
	struct address_space *mapping = inode->i_mapping;
	loff_t unmap_start = round_up(lstart, PAGE_SIZE);
	loff_t unmap_end = round_down(1 + lend, PAGE_SIZE) - 1;
	/* 两个对齐值只服务 unmap；原 lstart/lend 不丢失，随后传给可处理 partial folio 的截断器。 */
	/* lend 为 -1 时算术在 u64 比较下保留 EOF 特例，不把它当普通小于 start 的洞。 */
	/*
	 * This rounding is currently just for example: unmap_mapping_range
	 * expands its hole outwards, whereas we want it to contract the hole
	 * inwards.  However, existing callers of truncate_pagecache_range are
	 * doing their own page rounding first.  Note that unmap_mapping_range
	 * allows holelen 0 for all, and we allow lend -1 for end of file.
	 */

	/*
	 * Unlike in truncate_pagecache, unmap_mapping_range is called only
	 * once (before truncating pagecache), and without "even_cows" flag:
	 * hole-punching should not remove private COWed pages from the hole.
	 */
	/* hole-punch 只 unmap 完整页内部，保留边界页和私有 COW 页的文件语义。 */
	/* 即使没有完整页可 unmap，仍调用范围截断以清理边界 folio 的有效数据和 private 状态。 */
	if ((u64)unmap_end > (u64)unmap_start)
		unmap_mapping_range(mapping, unmap_start,
				    1 + unmap_end - unmap_start, 0);
	truncate_inode_pages_range(mapping, lstart, lend);
}
EXPORT_SYMBOL(truncate_pagecache_range);
