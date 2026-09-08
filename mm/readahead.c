// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/readahead.c - address_space-level file readahead.
 *
 * Copyright (C) 2002, Linus Torvalds
 *
 * 09Apr2002	Andrew Morton
 *		Initial version.
 */

/**
 * DOC: Readahead Overview
 *
 * Readahead is used to read content into the page cache before it is
 * explicitly requested by the application.  Readahead only ever
 * attempts to read folios that are not yet in the page cache.  If a
 * folio is present but not up-to-date, readahead will not try to read
 * it. In that case a simple ->read_folio() will be requested.
 *
 * Readahead is triggered when an application read request (whether a
 * system call or a page fault) finds that the requested folio is not in
 * the page cache, or that it is in the page cache and has the
 * readahead flag set.  This flag indicates that the folio was read
 * as part of a previous readahead request and now that it has been
 * accessed, it is time for the next readahead.
 *
 * Each readahead request is partly synchronous read, and partly async
 * readahead.  This is reflected in the struct file_ra_state which
 * contains ->size being the total number of pages, and ->async_size
 * which is the number of pages in the async section.  The readahead
 * flag will be set on the first folio in this async section to trigger
 * a subsequent readahead.  Once a series of sequential reads has been
 * established, there should be no need for a synchronous component and
 * all readahead request will be fully asynchronous.
 *
 * When either of the triggers causes a readahead, three numbers need
 * to be determined: the start of the region to read, the size of the
 * region, and the size of the async tail.
 *
 * The start of the region is simply the first page address at or after
 * the accessed address, which is not currently populated in the page
 * cache.  This is found with a simple search in the page cache.
 *
 * The size of the async tail is determined by subtracting the size that
 * was explicitly requested from the determined request size, unless
 * this would be less than zero - then zero is used.  NOTE THIS
 * CALCULATION IS WRONG WHEN THE START OF THE REGION IS NOT THE ACCESSED
 * PAGE.  ALSO THIS CALCULATION IS NOT USED CONSISTENTLY.
 *
 * The size of the region is normally determined from the size of the
 * previous readahead which loaded the preceding pages.  This may be
 * discovered from the struct file_ra_state for simple sequential reads,
 * or from examining the state of the page cache when multiple
 * sequential reads are interleaved.  Specifically: where the readahead
 * was triggered by the readahead flag, the size of the previous
 * readahead is assumed to be the number of pages from the triggering
 * page to the start of the new readahead.  In these cases, the size of
 * the previous readahead is scaled, often doubled, for the new
 * readahead, though see get_next_ra_size() for details.
 *
 * If the size of the previous read cannot be determined, the number of
 * preceding pages in the page cache is used to estimate the size of
 * a previous read.  This estimate could easily be misled by random
 * reads being coincidentally adjacent, so it is ignored unless it is
 * larger than the current request, and it is not scaled up, unless it
 * is at the start of file.
 *
 * In general readahead is accelerated at the start of the file, as
 * reads from there are often sequential.  There are other minor
 * adjustments to the readahead size in various special cases and these
 * are best discovered by reading the code.
 *
 * The above calculation, based on the previous readahead size,
 * determines the size of the readahead, to which any requested read
 * size may be added.
 *
 * Readahead requests are sent to the filesystem using the ->readahead()
 * address space operation, for which mpage_readahead() is a canonical
 * implementation.  ->readahead() should normally initiate reads on all
 * folios, but may fail to read any or all folios without causing an I/O
 * error.  The page cache reading code will issue a ->read_folio() request
 * for any folio which ->readahead() did not read, and only an error
 * from this will be final.
 *
 * ->readahead() will generally call readahead_folio() repeatedly to get
 * each folio from those prepared for readahead.  It may fail to read a
 * folio by:
 *
 * * not calling readahead_folio() sufficiently many times, effectively
 *   ignoring some folios, as might be appropriate if the path to
 *   storage is congested.
 *
 * * failing to actually submit a read request for a given folio,
 *   possibly due to insufficient resources, or
 *
 * * getting an error during subsequent processing of a request.
 *
 * In the last two cases, the folio should be unlocked by the filesystem
 * to indicate that the read attempt has failed.  In the first case the
 * folio will be unlocked by the VFS.
 *
 * Those folios not in the final ``async_size`` of the request should be
 * considered to be important and ->readahead() should not fail them due
 * to congestion or temporary resource unavailability, but should wait
 * for necessary resources (e.g.  memory or indexing information) to
 * become available.  Folios in the final ``async_size`` may be
 * considered less urgent and failure to read them is more acceptable.
 * In this case it is best to use filemap_remove_folio() to remove the
 * folios from the page cache as is automatically done for folios that
 * were not fetched with readahead_folio().  This will allow a
 * subsequent synchronous readahead request to try them again.  If they
 * are left in the page cache, then they will be read individually using
 * ->read_folio() which may be less efficient.
 */
/*
 * 学习总览：预读只为 page cache 中尚不存在的 folio 建立并提交 I/O；已存在但
 * 非 uptodate 的 folio 留给同步 ->read_folio。file_ra_state 的 start/size/
 * async_size 描述最近窗口，异步段首 folio 的 readahead 位触发下一窗口。
 * 窗口大小优先依据上一窗口或缓存中的连续历史估算，再受 BDI 上限约束。
 * ->readahead 可少取 folio：VFS 移除未领取者；已领取但提交失败者由文件系统
 * 解锁。同步重要段应等待资源，异步尾段可因拥塞放弃并让以后同步重试。
 */

#include <linux/blkdev.h>
#include <linux/kernel.h>
#include <linux/dax.h>
#include <linux/gfp.h>
#include <linux/export.h>
#include <linux/backing-dev.h>
/* pagemap 提供 page cache/XArray 操作；PSI 与 blk-cgroup 提供压力反馈。 */
#include <linux/task_io_accounting_ops.h>
#include <linux/pagemap.h>
#include <linux/psi.h>
#include <linux/syscalls.h>
/* file/fadvise 提供系统调用落点，sched/mm 提供 NOFS 上下文保存。 */
#include <linux/file.h>
#include <linux/mm_inline.h>
#include <linux/blk-cgroup.h>
#include <linux/fadvise.h>
#include <linux/sched/mm.h>

#define CREATE_TRACE_POINTS
#include <trace/events/readahead.h>

#include "internal.h"

/*
 * Initialise a struct file's readahead state.  Assumes that the caller has
 * memset *ra to zero.
 */
/*
 * 业务背景：打开文件时初始化其顺序读预测状态。
 * 入参：已清零的 ra 与 mapping；出参：写入设备预读上限和无前次位置。
 * 注意事项：不清其他字段，调用者必须先 memset，mapping->host 需有效。
 */
void
file_ra_state_init(struct file_ra_state *ra, struct address_space *mapping)
{
	ra->ra_pages = inode_to_bdi(mapping->host)->ra_pages;
	ra->prev_pos = -1;
}
EXPORT_SYMBOL_GPL(file_ra_state_init);

/**
 * read_pages() - Start IO for a contiguous range of allocated folios in the
 *                page cache.
 * @rac: Readahead control.
 *
 * When read_pages() returns, it is guaranteed that all of the folios will have
 * been processed or removed so that ``readahead_count(rac) == 0``. However,
 * that does not imply that ``readahead_index(rac)`` will be updated to point
 * to the end of the originally requested range because, for example, the
 * filesystem may expand the range upwards.
 */
/*
 * 译注：消费 rac 中已加入 cache 且锁定的连续 folio，返回时计数保证为 0；
 * 文件系统可扩展请求，最终 index 未必等于原末端。有 ->readahead 时未领取者
 * 由 VFS 移除/解锁；否则逐个调用 ->read_folio 交接 I/O 与解锁责任。
 */
static void read_pages(struct readahead_control *rac)
{
	const struct address_space_operations *aops = rac->mapping->a_ops;
	struct folio *folio;
	struct blk_plug plug;

	/* 空批次无需进入 PSI memstall，也无需建立块层 plug。 */
	if (!readahead_count(rac))
		return;

	if (unlikely(rac->_workingset))
		psi_memstall_enter(&rac->_pflags);
	blk_start_plug(&plug);

	if (aops->readahead) {
		/* 文件系统先领取想处理的 folio；剩余者仍锁定并由下面清理。 */
		aops->readahead(rac);
		/* Clean up the remaining folios. */
		/* 译注：临时加引用以安全移出 page cache，随后解锁并放掉本地引用。 */
		while ((folio = readahead_folio(rac)) != NULL) {
			folio_get(folio);
			filemap_remove_folio(folio);
			folio_unlock(folio);
			folio_put(folio);
		}
	} else {
		/* 无批量接口时 read_folio 接管每个锁定 folio 的 I/O/解锁责任。 */
		while ((folio = readahead_folio(rac)) != NULL)
			aops->read_folio(rac->file, folio);
	}

	blk_finish_plug(&plug);
	if (unlikely(rac->_workingset))
		psi_memstall_leave(&rac->_pflags);
	rac->_workingset = false;

	BUG_ON(readahead_count(rac));
}

/*
 * 业务背景：为预读窗口分配指定 order folio。
 * 入参：控制器、GFP、order；出参：新 folio 或 NULL。
 * 注意事项：dropbehind 只在发布前写标志，返回引用归调用者。
 */
static struct folio *ractl_alloc_folio(struct readahead_control *ractl,
				       gfp_t gfp_mask, unsigned int order)
{
	struct folio *folio;

	folio = filemap_alloc_folio(gfp_mask, order, NULL);
	if (folio && ractl->dropbehind)
		__folio_set_dropbehind(folio);

	return folio;
}

/**
 * page_cache_ra_unbounded - Start unchecked readahead.
 * @ractl: Readahead control.
 * @nr_to_read: The number of pages to read.
 * @lookahead_size: Where to start the next readahead.
 *
 * This function is for filesystems to call when they want to start
 * readahead beyond a file's stated i_size.  This is almost certainly
 * not the function you want to call.  Use page_cache_async_readahead()
 * or page_cache_sync_readahead() instead.
 *
 * Context: File is referenced by caller, and ractl->mapping->invalidate_lock
 * must be held by the caller at least in shared mode.  Mutexes may be held by
 * caller.  May sleep, but will not reenter filesystem to reclaim memory.
 */
/*
 * 译注：不按 i_size 截断地预分配 nr_to_read 页，并在异步尾段首 folio 置
 * readahead 标志。调用者持 invalidate_lock 至少读锁；函数以 NOFS 分配，
 * 将锁定 folio 发布到 cache，最后统一提交 I/O，单项冲突形成批次边界。
 */
void page_cache_ra_unbounded(struct readahead_control *ractl,
		unsigned long nr_to_read, unsigned long lookahead_size)
{
	struct address_space *mapping = ractl->mapping;
	unsigned long index = readahead_index(ractl);
	gfp_t gfp_mask = readahead_gfp_mask(mapping);
	unsigned long mark = ULONG_MAX, i = 0;
	unsigned int min_nrpages = mapping_min_folio_nrpages(mapping);

	/*
	 * Partway through the readahead operation, we will have added
	 * locked pages to the page cache, but will not yet have submitted
	 * them for I/O.  Adding another page may need to allocate memory,
	 * which can trigger memory reclaim.  Telling the VM we're in
	 * the middle of a filesystem operation will cause it to not
	 * touch file-backed pages, preventing a deadlock.  Most (all?)
	 * filesystems already specify __GFP_NOFS in their mapping's
	 * gfp_mask, but let's be explicit here.
	 */
	/* 译注：锁定 folio 尚未提交时若回收重入文件系统会死锁，故显式 NOFS。 */
	unsigned int nofs = memalloc_nofs_save();

	lockdep_assert_held(&mapping->invalidate_lock);

	trace_page_cache_ra_unbounded(mapping->host, index, nr_to_read,
				      lookahead_size);
	index = mapping_align_index(mapping, index);

	/*
	 * As iterator `i` is aligned to min_nrpages, round_up the
	 * difference between nr_to_read and lookahead_size to mark the
	 * index that only has lookahead or "async_region" to set the
	 * readahead flag.
	 */
	/* 译注：按 mapping 最小 folio 粒度上取整异步起点，得到相对 mark。 */
	if (lookahead_size <= nr_to_read) {
		unsigned long ra_folio_index;

		ra_folio_index = round_up(readahead_index(ractl) +
					  nr_to_read - lookahead_size,
					  min_nrpages);
		mark = ra_folio_index - index;
	}
	nr_to_read += readahead_index(ractl) - index;
	ractl->_index = index;

	/*
	 * Preallocate as many pages as we will need.
	 */
	/* 译注：先尽量把整批 folio 加入 cache，再交给文件系统启动 I/O。 */
	while (i < nr_to_read) {
		struct folio *folio = xa_load(&mapping->i_pages, index + i);
		int ret;

		if (folio && !xa_is_value(folio)) {
			/*
			 * Page already present?  Kick off the current batch
			 * of contiguous pages before continuing with the
			 * next batch.  This page may be the one we would
			 * have intended to mark as Readahead, but we don't
			 * have a stable reference to this page, and it's
			 * not worth getting one just for that.
			 */
			/* 译注：已有 folio 打断连续批次；无稳定引用，不能只为置标志触碰它。 */
			read_pages(ractl);
			ractl->_index += min_nrpages;
			i = ractl->_index - index;
			continue;
		}

		folio = ractl_alloc_folio(ractl, gfp_mask,
					mapping_min_folio_order(mapping));
		if (!folio)
			break;

		ret = filemap_add_folio(mapping, folio, index + i, gfp_mask);
		/* add 成功后 cache 接管引用；失败仍由本函数 folio_put。 */
		if (ret < 0) {
			folio_put(folio);
			if (ret == -ENOMEM)
				/* 元数据内存不足时停止扩张，已建批次仍会提交。 */
				break;
			read_pages(ractl);
			ractl->_index += min_nrpages;
			i = ractl->_index - index;
			continue;
		}
		/* 新 folio 成功发布后设置异步标记并累计批次页数。 */
		if (i == mark)
			folio_set_readahead(folio);
		ractl->_workingset |= folio_test_workingset(folio);
		ractl->_nr_pages += min_nrpages;
		i += min_nrpages;
	}

	/*
	 * Now start the IO.  We ignore I/O errors - if the folio is not
	 * uptodate then the caller will launch read_folio again, and
	 * will then handle the error.
	 */
	/* 译注：预读 I/O 错误延迟到同步 read_folio 处理，当前只确保全部 folio 被消费。 */
	read_pages(ractl);
	memalloc_nofs_restore(nofs);
}
EXPORT_SYMBOL_GPL(page_cache_ra_unbounded);

/*
 * do_page_cache_ra() actually reads a chunk of disk.  It allocates
 * the pages first, then submits them for I/O. This avoids the very bad
 * behaviour which would occur if page allocations are causing VM writeback.
 * We really don't want to intermingle reads and writes like that.
 */
/*
 * 业务背景：按文件 i_size 截断窗口并在 invalidate 读锁内执行普通预读。
 * 入参：控制器、页数和异步尾长；出参：void，已建 folio 被提交或清理。
 * 注意事项：空文件/起点越 EOF 直接返回；EOF 窗口不设置下一次触发标记。
 */
static void do_page_cache_ra(struct readahead_control *ractl,
		unsigned long nr_to_read, unsigned long lookahead_size)
{
	struct address_space *mapping = ractl->mapping;
	unsigned long index = readahead_index(ractl);
	loff_t isize = i_size_read(mapping->host);
	pgoff_t end_index;	/* The last page we want to read */

	if (isize == 0)
		return;

	end_index = (isize - 1) >> PAGE_SHIFT;
	/* i_size 在无锁读取后可变化；这里只用快照限制当前尽力预读。 */
	if (index > end_index)
		return;
	/* Don't read past the page containing the last byte of the file */
	/* 译注：只允许覆盖含最后一个字节的页，不向文件末端之外预读。 */
	if (nr_to_read > end_index - index) {
		nr_to_read = end_index - index + 1;
		/* We've reached the end, so don't set a readahead marker. */
		/* 译注：到达 EOF 后没有下一窗口，清零 lookahead。 */
		lookahead_size = 0;
	}

	filemap_invalidate_lock_shared(mapping);
	page_cache_ra_unbounded(ractl, nr_to_read, lookahead_size);
	filemap_invalidate_unlock_shared(mapping);
}

/*
 * Chunk the readahead into 2 megabyte units, so that we don't pin too much
 * memory at once.
 */
/*
 * 业务背景：显式 WILLNEED/随机模式按请求强制预读，不运行顺序预测。
 * 入参：控制器和页数；出参：分块提交 I/O，返回 void。
 * 注意事项：无读取 aops 时跳过；总量受设备最佳 I/O 与 ra_pages 上限约束。
 */
void force_page_cache_ra(struct readahead_control *ractl,
		unsigned long nr_to_read)
{
	struct address_space *mapping = ractl->mapping;
	struct file_ra_state *ra = ractl->ra;
	struct backing_dev_info *bdi = inode_to_bdi(mapping->host);
	unsigned long max_pages;

	if (unlikely(!mapping->a_ops->read_folio && !mapping->a_ops->readahead))
		return;

	/*
	 * If the request exceeds the readahead window, allow the read to
	 * be up to the optimal hardware IO size
	 */
	/* 译注：大请求可扩大到设备最佳 I/O 页数，但不会无界增长。 */
	max_pages = max_t(unsigned long, bdi->io_pages, ra->ra_pages);
	nr_to_read = min_t(unsigned long, nr_to_read, max_pages);
	while (nr_to_read) {
		/* 每批最多 2MiB，避免大量锁定 folio 同时滞留在提交前阶段。 */
		unsigned long this_chunk = (2 * 1024 * 1024) / PAGE_SIZE;

		if (this_chunk > nr_to_read)
			this_chunk = nr_to_read;
		do_page_cache_ra(ractl, this_chunk, 0);

		nr_to_read -= this_chunk;
	}
}

/*
 * Set the initial window size, round to next power of 2 and square
 * for small size, x 4 for medium, and x 2 for large
 * for 128k (32 page) max ra
 * 1-2 page = 16k, 3-4 page 32k, 5-8 page = 64k, > 8 page = 128k initial
 */
/*
 * 业务背景：首次检测到顺序流时选择启动窗口。
 * 入参：本次请求页数与 max；出参：2 次幂对齐且不超过 max 的页数。
 * 注意事项：小请求乘四、中等乘二、大请求封顶，以尽早形成 I/O 流水线。
 */
static unsigned long get_init_ra_size(unsigned long size, unsigned long max)
{
	unsigned long newsize = roundup_pow_of_two(size);

	/* 阶梯式倍增在小请求上更激进，接近 max 时直接封顶。 */
	if (newsize <= max / 32)
		newsize = newsize * 4;
	else if (newsize <= max / 4)
		newsize = newsize * 2;
	else
		newsize = max;

	return newsize;
}

/*
 *  Get the previous window size, ramp it up, and
 *  return it as the new window size.
 */
/*
 * 业务背景：异步触发后按上一窗口平滑放大。
 * 入参：ra 最近 size 与 max；出参：小窗口四倍、中窗口两倍或 max。
 * 注意事项：只计算不写 ra，调用者负责更新 start/size/async_size。
 */
static unsigned long get_next_ra_size(struct file_ra_state *ra,
				      unsigned long max)
{
	unsigned long cur = ra->size;

	if (cur < max / 16)
		return 4 * cur;
	if (cur <= max / 2)
		return 2 * cur;
	return max;
}

/*
 * On-demand readahead design.
 *
 * The fields in struct file_ra_state represent the most-recently-executed
 * readahead attempt:
 *
 *                        |<----- async_size ---------|
 *     |------------------- size -------------------->|
 *     |==================#===========================|
 *     ^start             ^page marked with PG_readahead
 *
 * To overlap application thinking time and disk I/O time, we do
 * `readahead pipelining': Do not wait until the application consumed all
 * readahead pages and stalled on the missing page at readahead_index;
 * Instead, submit an asynchronous readahead I/O as soon as there are
 * only async_size pages left in the readahead window. Normally async_size
 * will be equal to size, for maximum pipelining.
 *
 * In interleaved sequential reads, concurrent streams on the same fd can
 * be invalidating each other's readahead state. So we flag the new readahead
 * page at (start+size-async_size) with PG_readahead, and use it as readahead
 * indicator. The flag won't be set on already cached pages, to avoid the
 * readahead-for-nothing fuss, saving pointless page cache lookups.
 *
 * prev_pos tracks the last visited byte in the _previous_ read request.
 * It should be maintained by the caller, and will be used for detecting
 * small random reads. Note that the readahead algorithm checks loosely
 * for sequential patterns. Hence interleaved reads might be served as
 * sequential ones.
 *
 * There is a special-case: if the first page which the application tries to
 * read happens to be the first page of the file, it is assumed that a linear
 * read is about to happen and the window is immediately set to the initial size
 * based on I/O request size and the max_readahead.
 *
 * The code ramps up the readahead size aggressively at first, but slow down as
 * it approaches max_readahead.
 */
/*
 * 译注：ra 记录最近已执行窗口；async 段首标志让应用尚未耗尽窗口时就提交
 * 下一批，从而重叠计算与 I/O。并发流可能互相覆盖 fd 状态，所以缓存中的
 * 标志与连续空洞历史可重建窗口。prev_pos 由调用者维护，仅用于宽松顺序判断。
 */

/*
 * 业务背景：分配一个可变 order folio 并加入预读控制器的 page cache 批次。
 * 入参：控制器、页索引、触发标记、order、GFP；出参：0 或负 errno。
 * 注意事项：add 失败时归还本地引用；成功后 cache 接管且 folio 仍锁定待 I/O。
 */
static inline int ra_alloc_folio(struct readahead_control *ractl, pgoff_t index,
		pgoff_t mark, unsigned int order, gfp_t gfp)
{
	int err;
	struct folio *folio = ractl_alloc_folio(ractl, gfp, order);

	if (!folio)
		return -ENOMEM;
	mark = round_down(mark, 1UL << order);
	if (index == mark)
		folio_set_readahead(folio);
	/* 标记在发布前设置，page-cache 查找者不会看见未完成的初态。 */
	err = filemap_add_folio(ractl->mapping, folio, index, gfp);
	/* 文件截断或并发插入可令 add 失败；未发布 folio 仍由当前路径释放。 */
	if (err) {
		folio_put(folio);
		return err;
	}

	ractl->_nr_pages += 1UL << order;
	ractl->_workingset |= folio_test_workingset(folio);
	return 0;
}

/*
 * 业务背景：优先用大 folio 覆盖 ra 窗口，并在失败/冲突时退回普通预读。
 * 入参：控制器及可被文件系统更新的 ra；出参：提交可建立的全部 I/O。
 * 注意事项：invalidate 读锁+NOFS 包住发布；order 会按对齐、EOF 和 mapping 能力降级。
 */
void page_cache_ra_order(struct readahead_control *ractl,
		struct file_ra_state *ra)
{
	struct address_space *mapping = ractl->mapping;
	pgoff_t start = readahead_index(ractl);
	pgoff_t index = start;
	/* start 保留回退基准，index 随成功加入的 folio 向前推进。 */
	unsigned int min_order = mapping_min_folio_order(mapping);
	pgoff_t limit = (i_size_read(mapping->host) - 1) >> PAGE_SHIFT;
	pgoff_t mark;
	unsigned int nofs;
	int err = 0;
	/* limit/mark 使用页索引；order 受 mapping 最小/最大阶共同约束。 */
	/* gfp 与 new_order 是此次尝试的本地快照，失败时可安全回退。 */
	gfp_t gfp = readahead_gfp_mask(mapping);
	unsigned int new_order = ra->order;

	trace_page_cache_ra_order(mapping->host, start, ra);
	if (!mapping_large_folio_support(mapping)) {
		/* mapping 不支持大 folio 时清零预测 order 并复用基础路径。 */
		ra->order = 0;
		goto fallback;
	}

	if (limit > index + ra->size - 1) {
		limit = index + ra->size - 1;
		mark = index + ra->size - ra->async_size;
	} else {
		/* We've reached the end, so don't set a readahead marker. */
		/* 译注：窗口触及 EOF，不再安排下一次异步触发。 */
		mark = ULONG_MAX;
	}

	new_order = min(mapping_max_folio_order(mapping), new_order);
	new_order = min_t(unsigned int, new_order, ilog2(ra->size));
	new_order = max(new_order, min_order);

	ra->order = new_order;

	/* See comment in page_cache_ra_unbounded() */
	/* 译注：同 unbounded 路径，避免持有未提交 folio 时回收重入文件系统。 */
	nofs = memalloc_nofs_save();
	filemap_invalidate_lock_shared(mapping);
	/*
	 * If the new_order is greater than min_order and index is
	 * already aligned to new_order, then this will be noop as index
	 * aligned to new_order should also be aligned to min_order.
	 */
	/* 译注：mapping 最小粒度对齐是下限；本来已按更大 order 对齐时保持不变。 */
	ractl->_index = mapping_align_index(mapping, index);
	index = readahead_index(ractl);

	while (index <= limit) {
		unsigned int order = new_order;

		/* Align with smaller pages if needed */
		/* 译注：起点不满足当前 order 时，降到其最低置位允许的对齐。 */
		if (index & ((1UL << order) - 1))
			order = __ffs(index);
		/* Don't allocate pages past EOF */
		/* 译注：末尾逐级降 order，确保 folio 不跨越 limit。 */
		while (order > min_order && index + (1UL << order) - 1 > limit)
			order--;
		err = ra_alloc_folio(ractl, index, mark, order, gfp);
		if (err)
			break;
		index += 1UL << order;
	}

	read_pages(ractl);
	filemap_invalidate_unlock_shared(mapping);
	memalloc_nofs_restore(nofs);

	/*
	 * If there were already pages in the page cache, then we may have
	 * left some gaps.  Let the regular readahead code take care of this
	 * situation below.
	 */
	/* 译注：任何 add 冲突/失败都可能留下空洞，基础预读负责补齐剩余范围。 */
	if (!err)
		return;
fallback:
	/*
	 * ->readahead() may have updated readahead window size so we have to
	 * check there's still something to read.
	 */
	/* 译注：aops 可改写 ra 窗口，回退前重新按已推进页数计算剩余量。 */
	if (ra->size > index - start)
		do_page_cache_ra(ractl, ra->size - (index - start),
				 ra->async_size);
}

static unsigned long ractl_max_pages(struct readahead_control *ractl,
		unsigned long req_size)
{
	/*
	 * 业务背景：计算本请求允许的最大预读窗口；入参含 BDI 与请求页数。
	 * 出参：通常为 ra_pages，超大请求可放宽到 io_pages；不修改 ra 状态。
	 * 注意事项：只在请求和硬件最优 I/O 都大于当前上限时扩张。
	 */
	struct backing_dev_info *bdi = inode_to_bdi(ractl->mapping->host);
	unsigned long max_pages = ractl->ra->ra_pages;

	/*
	 * If the request exceeds the readahead window, allow the read to
	 * be up to the optimal hardware IO size
	 */
	/* 译注：超窗请求允许匹配设备最优 I/O，但仍不超过实际请求页数。 */
	if (req_size > max_pages && bdi->io_pages > max_pages)
		max_pages = min(req_size, bdi->io_pages);
	return max_pages;
}

/*
 * 业务背景：同步 cache miss 时建立或重建预读窗口。
 * 入参：控制器与当前明确请求页数；出参：更新 ra 并提交同步/异步批次。
 * 注意事项：禁用/拥塞/随机模式走强制预读；小随机读不污染顺序状态。
 */
void page_cache_sync_ra(struct readahead_control *ractl,
		unsigned long req_count)
{
	pgoff_t index = readahead_index(ractl);
	bool do_forced_ra = ractl->file && (ractl->file->f_mode & FMODE_RANDOM);
	struct file_ra_state *ra = ractl->ra;
	unsigned long max_pages, contig_count;
	pgoff_t prev_index, miss;

	trace_page_cache_sync_ra(ractl->mapping->host, index, ra, req_count);
	/*
	 * Even if readahead is disabled, issue this request as readahead
	 * as we'll need it to satisfy the requested range. The forced
	 * readahead will do the right thing and limit the read to just the
	 * requested range, which we'll set to 1 page for this case.
	 */
	/* 译注：即使禁用预读，当前缺页仍需读一页；拥塞时也限制为明确需求。 */
	if (!ra->ra_pages || blk_cgroup_congested()) {
		if (!ractl->file)
			return;
		req_count = 1;
		do_forced_ra = true;
	}

	/* be dumb */
	/* 译注：随机访问不做历史推断，只按请求范围直接预读。 */
	if (do_forced_ra) {
		force_page_cache_ra(ractl, req_count);
		return;
	}

	max_pages = ractl_max_pages(ractl, req_count);
	prev_index = (unsigned long long)ra->prev_pos >> PAGE_SHIFT;
	/*
	 * A start of file, oversized read, or sequential cache miss:
	 * trivial case: (index - prev_index) == 1
	 * unaligned reads: (index - prev_index) == 0
	 */
	/* 译注：文件起点、超大请求或与上次字节位置相邻时直接建立初始窗口。 */
	if (!index || req_count > max_pages || index - prev_index <= 1UL) {
		ra->start = index;
		ra->size = get_init_ra_size(req_count, max_pages);
		ra->async_size = ra->size > req_count ? ra->size - req_count :
							ra->size >> 1;
		goto readit;
	}

	/*
	 * Query the page cache and look for the traces(cached history pages)
	 * that a sequential stream would leave behind.
	 */
	/* 译注：在 RCU 下向前寻找 cache 空洞，用连续已缓存页估算上一轮长度。 */
	rcu_read_lock();
	miss = page_cache_prev_miss(ractl->mapping, index - 1, max_pages);
	rcu_read_unlock();
	contig_count = index - miss - 1;
	/*
	 * Standalone, small random read. Read as is, and do not pollute the
	 * readahead state.
	 */
	/* 译注：连续历史不长于本请求时视作巧合随机读，不更新 ra。 */
	if (contig_count <= req_count) {
		do_page_cache_ra(ractl, req_count, 0);
		return;
	}
	/*
	 * File cached from the beginning:
	 * it is a strong indication of long-run stream (or whole-file-read)
	 */
	/* 译注：从文件头至当前全缓存是强顺序信号，估计长度加倍但仍受上限。 */
	if (miss == ULONG_MAX)
		contig_count *= 2;
	ra->start = index;
	ra->size = min(contig_count + req_count, max_pages);
	/* 重建状态时只置一个异步触发页，下一次命中再扩大为完整流水线。 */
	ra->async_size = 1;
readit:
	ra->order = 0;
	ractl->_index = ra->start;
	page_cache_ra_order(ractl, ra);
}
EXPORT_SYMBOL_GPL(page_cache_sync_ra);

/*
 * 业务背景：命中 readahead 标记 folio 时把流水线窗口向前推进。
 * 入参：控制器、触发 folio、明确需求页数；出参：更新 ra 并提交下一批。
 * 注意事项：writeback 共用标志位、设备拥塞或预读禁用时只清/忽略触发。
 */
void page_cache_async_ra(struct readahead_control *ractl,
		struct folio *folio, unsigned long req_count)
{
	unsigned long max_pages;
	struct file_ra_state *ra = ractl->ra;
	pgoff_t index = readahead_index(ractl);
	pgoff_t expected, start, end, aligned_end, align;

	/* no readahead */
	/* 译注：设备给出的预读上限为零时不产生任何额外 I/O。 */
	if (!ra->ra_pages)
		return;

	/*
	 * Same bit is used for PG_readahead and PG_reclaim.
	 */
	/* 译注：writeback folio 上同一位表示 reclaim，不能误作预读触发。 */
	if (folio_test_writeback(folio))
		return;

	trace_page_cache_async_ra(ractl->mapping->host, index, ra, req_count);
	folio_clear_readahead(folio);

	if (blk_cgroup_congested())
		return;

	max_pages = ractl_max_pages(ractl, req_count);
	/*
	 * It's the expected callback index, assume sequential access.
	 * Ramp up sizes, and push forward the readahead window.
	 */
	/* 译注：命中预测触发位置说明顺序流有效，窗口从旧末端继续并放大。 */
	expected = round_down(ra->start + ra->size - ra->async_size,
			folio_nr_pages(folio));
	if (index == expected) {
		ra->start += ra->size;
		/*
		 * In the case of MADV_HUGEPAGE, the actual size might exceed
		 * the readahead window.
		 */
		/* 译注：MADV_HUGEPAGE 可令实际 folio 大于窗口，先保留该实际大小。 */
		ra->size = max(ra->size, get_next_ra_size(ra, max_pages));
		goto readit;
	}

	/*
	 * Hit a marked folio without valid readahead state.
	 * E.g. interleaved reads.
	 * Query the pagecache for async_size, which normally equals to
	 * readahead size. Ramp it up and use it as the new readahead size.
	 */
	/* 译注：状态失配时向后找下一个 cache 空洞，以旧异步段长度重建流。 */
	rcu_read_lock();
	start = page_cache_next_miss(ractl->mapping, index + 1, max_pages);
	rcu_read_unlock();

	if (!start || start - index > max_pages)
		return;

	ra->start = start;
	ra->size = start - index;	/* old async_size */
	ra->size += req_count;
	ra->size = get_next_ra_size(ra, max_pages);
readit:
	/* 连续命中提高 order 倾向；窗口末端按当前可行对齐向下收缩。 */
	ra->order += 2;
	align = 1UL << min(ra->order, ffs(max_pages) - 1);
	end = ra->start + ra->size;
	aligned_end = round_down(end, align);
	/* 仅在对齐后仍非空时收缩，避免窗口退到 start 之前。 */
	if (aligned_end > ra->start)
		ra->size -= end - aligned_end;
	ra->async_size = ra->size;
	ractl->_index = ra->start;
	page_cache_ra_order(ractl, ra);
}
EXPORT_SYMBOL_GPL(page_cache_async_ra);

/*
 * 业务背景：实现 readahead(2) 的文件验证并转为 WILLNEED 建议。
 * 入参：fd、字节 offset/count；出参：成功字节建议结果或 -EBADF/-EINVAL 等。
 * 注意事项：仅可读的常规文件/块设备有效，匿名文件和无 mapping/aops 拒绝。
 */
ssize_t ksys_readahead(int fd, loff_t offset, size_t count)
{
	struct file *file;
	const struct inode *inode;

	CLASS(fd, f)(fd);
	if (fd_empty(f))
		return -EBADF;

	file = fd_file(f);
	if (!(file->f_mode & FMODE_READ))
		return -EBADF;

	/*
	 * The readahead() syscall is intended to run only on files
	 * that can execute readahead. If readahead is not possible
	 * on this file, then we must return -EINVAL.
	 */
	/* 译注：系统调用契约要求目标具备预读能力，否则必须明确返回 -EINVAL。 */
	if (!file->f_mapping)
		return -EINVAL;
	if (!file->f_mapping->a_ops)
		return -EINVAL;

	/* VFS readahead ABI 只接受具名常规文件和块设备。 */
	inode = file_inode(file);
	if (!S_ISREG(inode->i_mode) && !S_ISBLK(inode->i_mode))
		return -EINVAL;
	if (IS_ANON_FILE(inode))
		return -EINVAL;

	return vfs_fadvise(fd_file(f), offset, count, POSIX_FADV_WILLNEED);
}

SYSCALL_DEFINE3(readahead, int, fd, loff_t, offset, size_t, count)
{
	/* 原生 ABI 只负责取参，文件引用和所有验证统一由 ksys_readahead 完成。 */
	return ksys_readahead(fd, offset, count);
}

#if defined(CONFIG_COMPAT) && defined(__ARCH_WANT_COMPAT_READAHEAD)
COMPAT_SYSCALL_DEFINE4(readahead, int, fd, compat_arg_u64_dual(offset), size_t, count)
{
	/* 兼容 ABI 合并拆分的 64 位 offset 后复用同一语义实现。 */
	return ksys_readahead(fd, compat_arg_u64_glue(offset), count);
}
#endif

/**
 * readahead_expand - Expand a readahead request
 * @ractl: The request to be expanded
 * @new_start: The revised start
 * @new_len: The revised size of the request
 *
 * Attempt to expand a readahead request outwards from the current size to the
 * specified size by inserting locked pages before and after the current window
 * to increase the size to the new window.  This may involve the insertion of
 * THPs, in which case the window may get expanded even beyond what was
 * requested.
 *
 * The algorithm will stop if it encounters a conflicting page already in the
 * pagecache and leave a smaller expansion than requested.
 *
 * The caller must check for this by examining the revised @ractl object for a
 * different expansion than was requested.
 */
/*
 * 译注：尝试在当前窗口前后插入锁定 folio 扩展至 new_start/new_len；THP 可令
 * 实际范围超出请求，已有 cache 冲突或分配失败则保留部分扩展并返回 void。
 * 调用者必须读回 ractl 判断结果；成功加入的 folio 留给当前 aops 继续处理。
 */
void readahead_expand(struct readahead_control *ractl,
		      loff_t new_start, size_t new_len)
{
	struct address_space *mapping = ractl->mapping;
	struct file_ra_state *ra = ractl->ra;
	pgoff_t new_index, new_nr_pages;
	gfp_t gfp_mask = readahead_gfp_mask(mapping);
	unsigned long min_nrpages = mapping_min_folio_nrpages(mapping);
	unsigned int min_order = mapping_min_folio_order(mapping);

	new_index = new_start / PAGE_SIZE;
	/*
	 * Readahead code should have aligned the ractl->_index to
	 * min_nrpages before calling readahead aops.
	 */
	/* 译注：进入文件系统 aops 前，核心代码已按 mapping 最小 folio 粒度对齐。 */
	VM_BUG_ON(!IS_ALIGNED(ractl->_index, min_nrpages));

	/* Expand the leading edge downwards */
	/* 译注：前沿向低地址扩展，遇已有 folio 或分配/add 失败立即保留当前进度。 */
	while (ractl->_index > new_index) {
		unsigned long index = ractl->_index - 1;
		struct folio *folio = xa_load(&mapping->i_pages, index);

		if (folio && !xa_is_value(folio))
			/* cache 中真实 folio 是不可跨越的冲突边界。 */
			return; /* Folio apparently present */

		folio = ractl_alloc_folio(ractl, gfp_mask, min_order);
		if (!folio)
			return;

		index = mapping_align_index(mapping, index);
		if (filemap_add_folio(mapping, folio, index, gfp_mask) < 0) {
			folio_put(folio);
			return;
		}
		/* 首次纳入 workingset folio 时进入 PSI stall，交由 read_pages 配对退出。 */
		if (unlikely(folio_test_workingset(folio)) &&
				!ractl->_workingset) {
			ractl->_workingset = true;
			psi_memstall_enter(&ractl->_pflags);
		}
		ractl->_nr_pages += min_nrpages;
		/* 向前扩展会移动整个控制窗口起点，而不是改变 ra 的预测参数。 */
		ractl->_index = folio->index;
	}

	new_len += new_start - readahead_pos(ractl);
	/* 前沿实际扩张可能按大 folio 越界，按读回位置重算剩余总页数。 */
	new_nr_pages = DIV_ROUND_UP(new_len, PAGE_SIZE);

	/* Expand the trailing edge upwards */
	/* 译注：后沿再向高地址扩展；同样不覆盖现有 folio。 */
	while (ractl->_nr_pages < new_nr_pages) {
		unsigned long index = ractl->_index + ractl->_nr_pages;
		struct folio *folio = xa_load(&mapping->i_pages, index);

		if (folio && !xa_is_value(folio))
			return; /* Folio apparently present */

		/* 每轮只分配 mapping 最小阶，避免尾部扩张越过已有页粒度。 */
		folio = ractl_alloc_folio(ractl, gfp_mask, min_order);
		if (!folio)
			return;

		index = mapping_align_index(mapping, index);
		if (filemap_add_folio(mapping, folio, index, gfp_mask) < 0) {
			folio_put(folio);
			return;
		}
		/* add 成功后该 folio 保持锁定，由当前 readahead aops 后续领取。 */
		/* 与前沿相同，workingset 状态只在首次观察时进入 PSI。 */
		if (unlikely(folio_test_workingset(folio)) &&
				!ractl->_workingset) {
			ractl->_workingset = true;
			psi_memstall_enter(&ractl->_pflags);
		}
		ractl->_nr_pages += min_nrpages;
		if (ra) {
			/* 后沿扩展同步增加总窗口和异步尾长，保持触发点位置不变。 */
			ra->size += min_nrpages;
			ra->async_size += min_nrpages;
		}
	}
}
EXPORT_SYMBOL(readahead_expand);
