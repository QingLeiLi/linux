// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/page_io.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, 
 *  Asynchronous swapping added 30.12.95. Stephen Tweedie
 *  Removed race in async swapping. 14.4.1996. Bruno Haible
 *  Add swap of shared pages through the page cache. 20.2.1998. Stephen Tweedie
 *  Always use brw_page, life becomes simpler. 12 May 1998 Eric Biederman
 */

#include <linux/mm.h>
/* folio 状态机规定 writeback 与 unlock 的完成点；本文件的所有后端都遵守这一公开可见性顺序。 */
#include <linux/kernel_stat.h>
/* 统计、PSI、delayacct 分属不同观察口径；优化掉设备 I/O 也要保留正确的逻辑 swap 事件。 */
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/swapops.h>
/* swapops/swap table 定义 entry、cluster 与后端选择；bio/writeback 头文件提供最终 I/O 状态协议。 */
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/psi.h>
#include <linux/uio.h>
#include <linux/sched/task.h>
#include <linux/delayacct.h>
#include <linux/zswap.h>
#include "swap.h"
#include "swap_table.h"
/* 此层衔接 reclaim/swap table 与块/文件 I/O：folio 锁、writeback、bio/iocb 所有权不能混淆。 */

/*
 * 业务背景：块层完成异步 swap 写后在此把持久化结果交回 VM，供 end_io 包装调用。
 * 入参：bio 是提交后仍由块层持有的完成 bio，借用；其首 folio 是本次状态变更对象。
 * 出参/返回：无直接返回值；失败重脏并清 reclaim，随后结束该 folio 的 writeback。
 * 注意事项：只能在 I/O 已完成后调用；本函数不 bio_put，避免和包装层重复释放。
 */
static void __end_swap_bio_write(struct bio *bio)
{
	/* bio completion 的写侧收尾：错误把 folio 重新标脏，始终结束 writeback；不释放 bio。 */
	struct folio *folio = bio_first_folio_all(bio);
	/* 一个 swap bio 此处按提交路径只承载同一 folio，first_folio 是完成状态的归属对象。 */

	if (bio->bi_status) {
		/* I/O 失败时不能让 reclaim 把唯一脏数据当作已持久化，reclaim 位也需撤销。 */
		/*
		 * We failed to write the page out to swap-space.
		 * Re-dirty the page in order to avoid it being reclaimed.
		 * Also print a dire warning that things will go BAD (tm)
		 * very quickly.
		 *
		 * Also clear PG_reclaim to avoid folio_rotate_reclaimable()
		 */
		folio_mark_dirty(folio);
		pr_alert_ratelimited("Write-error on swap-device (%u:%u:%llu)\n",
				     MAJOR(bio_dev(bio)), MINOR(bio_dev(bio)),
				     (unsigned long long)bio->bi_iter.bi_sector);
		folio_clear_reclaim(folio);
	}
	folio_end_writeback(folio);
	/* 无论成败都唤醒等待 writeback 的观察者；脏位决定后续是否重试写出。 */
}

/*
 * 业务背景：这是异步块写的 end_io 入口，连接 block completion 与 VM writeback 状态机。
 * 入参：bio 为 block 层移交的拥有对象，结束时本函数负责归还。
 * 出参/返回：无直接返回值；完成 folio 状态收尾并释放 bio。
 * 注意事项：不得在调用后再访问 bio；同步路径改用不释放 bio 的内部 helper。
 */
static void end_swap_bio_write(struct bio *bio)
{
	/* async bio 的 end_io 包装在公共状态收尾后归还 bio 所有权。 */
	__end_swap_bio_write(bio);
	bio_put(bio);
}

/*
 * 业务背景：块设备 swapin 完成后把数据有效性发布给等待 folio 的 fault/readahead 路径。
 * 入参：bio 是已完成请求的借用指针，首 folio 在提交前已锁定。
 * 出参/返回：无直接返回值；成功置 uptodate，所有结果均解锁 folio。
 * 注意事项：不释放 bio；解锁是可见性边界，失败绝不能发布 uptodate。
 */
static void __end_swap_bio_read(struct bio *bio)
{
	/* 读侧只在完整成功时设 uptodate；解锁 folio 让 fault/readahead 调用者继续判断结果。 */
	struct folio *folio = bio_first_folio_all(bio);
	/* read completion 在 bio status 已稳定后决定是否发布 uptodate，随后无条件解锁。 */

	if (bio->bi_status) {
		/* 失败保持非 uptodate，解锁后上层会以 swap I/O 错误路径处理，不能伪造零页。 */
		pr_alert_ratelimited("Read-error on swap-device (%u:%u:%llu)\n",
				     MAJOR(bio_dev(bio)), MINOR(bio_dev(bio)),
				     (unsigned long long)bio->bi_iter.bi_sector);
	} else {
		folio_mark_uptodate(folio);
	}
	folio_unlock(folio);
}

/*
 * 业务背景：异步 swapin 的 block end_io 包装，将内部状态收尾与 bio 生命周期闭环。
 * 入参：bio 的所有权已由 block 层交给完成回调。
 * 出参/返回：无直接返回值；解锁目标 folio 后 bio_put。
 * 注意事项：同步读仅调用内部 helper，防止释放栈上 bio。
 */
static void end_swap_bio_read(struct bio *bio)
{
	/* 与写侧对称：状态处理不拥有 bio，end_io 包装才负责 bio_put。 */
	__end_swap_bio_read(bio);
	bio_put(bio);
}

/*
 * 业务背景：swapon 的通用文件后端把文件系统物理块映射登记为 swap extent。
 * 入参：sis 为待发布的 swap 描述符；swap_file 为已打开文件的借用引用；span 为物理跨度输出指针。
 * 出参/返回：成功返回 extent 数并写 sis->max/pages、*span；洞或 bmap 失败返回 -EINVAL/底层错误。
 * 注意事项：要求每个 PAGE_SIZE 范围物理连续且对齐；扫描可调度，失败前已加入的 extent 由上层清理。
 */
int generic_swapfile_activate(struct swap_info_struct *sis,
				struct file *swap_file,
				sector_t *span)
{
	/* 文件 swap 激活把逻辑页连续映射转换为 swap extent；洞或页内不连续都会拒绝。 */
	struct address_space *mapping = swap_file->f_mapping;
	struct inode *inode = mapping->host;
	unsigned blocks_per_page;
	/* 所有计算在 inode block 单位与 PAGE_SIZE 单位间转换，错配会构造错误 swap offset。 */
	unsigned long page_no;
	unsigned blkbits;
	sector_t probe_block;
	sector_t last_block;
	sector_t lowest_block = -1;
	sector_t highest_block = 0;
	int nr_extents = 0;
	int ret;

	blkbits = inode->i_blkbits;
	/* swap 的最小逻辑单元是 PAGE_SIZE，因此磁盘块数必须整页覆盖。 */
	blocks_per_page = PAGE_SIZE >> blkbits;

	/*
	 * Map all the blocks into the extent tree.  This code doesn't try
	 * to be very smart.
	 */
	probe_block = 0;
	page_no = 0;
	last_block = i_size_read(inode) >> blkbits;
	/* 文件末尾不足一页的区域不纳入 swap；swap header 占第 0 个逻辑页。 */
	while ((probe_block + blocks_per_page) <= last_block &&
			page_no < sis->max) {
		unsigned block_in_page;
		sector_t first_block;

		cond_resched();
		/* 大 swapfile 的 bmap 扫描可很长，循环中主动让出 CPU。 */

		first_block = probe_block;
		ret = bmap(inode, &first_block);
		/* 文件系统须给出物理块且不能有 hole；swap 不能在运行期做普通文件缺页填洞。 */
		if (ret || !first_block)
			goto bad_bmap;

		/*
		 * It must be PAGE_SIZE aligned on-disk
		 */
		if (first_block & (blocks_per_page - 1)) {
			/* 页起点未对齐时向后探测，不能把一个 swap page 切成两个磁盘页片段。 */
			probe_block++;
			goto reprobe;
		}

		for (block_in_page = 1; block_in_page < blocks_per_page;
			/* 页内每个块都要匹配首块递增序列，否则这页不能作为单一 swap extent。 */
					block_in_page++) {
			sector_t block;

			block = probe_block + block_in_page;
			ret = bmap(inode, &block);
			if (ret || !block)
				goto bad_bmap;

			if (block != first_block + block_in_page) {
				/* 同一页内出现不连续块则从下一个块重新尝试找完整页 run。 */
				/* Discontiguity */
				probe_block++;
				goto reprobe;
			}
		}

		first_block >>= (PAGE_SHIFT - blkbits);
		/* 转为 swap page 单位后才写 extent；lowest/highest 用于报告物理跨度。 */
		if (page_no) {	/* exclude the header page */
			if (first_block < lowest_block)
				lowest_block = first_block;
			if (first_block > highest_block)
				highest_block = first_block;
		}

		/*
		 * We found a PAGE_SIZE-length, PAGE_SIZE-aligned run of blocks
		 */
		ret = add_swap_extent(sis, page_no, 1, first_block);
		/* extent 记录逻辑 swap page 到连续磁盘页的映射；header page 不计可用 swap。 */
		if (ret < 0)
			goto out;
		nr_extents += ret;
		page_no++;
		probe_block += blocks_per_page;
reprobe:
		continue;
	}
	ret = nr_extents;
	/* 循环结束可能因文件末尾、sis->max 或错误跳出；只有 bad_bmap 转换为 -EINVAL。 */
	*span = 1 + highest_block - lowest_block;
	/* span 包含 header 的相对偏移基准；空文件强制 page_no=1 以给出一致诊断。 */
	if (page_no == 0)
			/* 空 swapfile 仍保留 header 逻辑页，避免 max/pages 形成无效的零容量状态。 */
		page_no = 1;	/* force Empty message */
	/* header-only 情形随后仍写回 max/pages，使上层获得可诊断的一致空 swap 描述。 */
	sis->max = page_no;
	sis->pages = page_no - 1;
out:
	return ret;
bad_bmap:
	pr_err("swapon: swapfile has holes\n");
	ret = -EINVAL;
	goto out;
}

/* extent 构造结束后，后续零页检测与 swap-table 操作不再依赖文件 bmap 上下文。 */

/*
 * 业务背景：swapout 在真正写设备前识别全零 folio，以 zeromap 元数据替代无价值 I/O。
 * 入参：folio 是已锁定且内容稳定的 swap folio，借用；可含多个 base page。
 * 出参/返回：全零返回 true，发现任一非零字返回 false；不改变 folio 状态。
 * 注意事项：每个 local map 必须成对解除；调用者负责保证扫描不与写入并发。
 */
static bool is_folio_zero_filled(struct folio *folio)
{
	/* 检查只读 folio 数据，不更改 dirty/writeback；返回 true 才允许调用者转换为 zeromap 元数据。 */
	/* 此检查读取已锁 folio 的内容；调用者保证写入不会与 swapout 扫描并发破坏结果。 */
	/* 逐 base page local-map 检查全零，命中 zeromap 可跳过真实 swap 写 I/O。 */
	unsigned int pos, last_pos;
	unsigned long *data;
	unsigned int i;

	last_pos = PAGE_SIZE / sizeof(*data) - 1;
	/* 页内按 machine word 扫描比 byte 扫描快，先末字适配“前部清零、尾部有数据”的常见形态。 */
	for (i = 0; i < folio_nr_pages(folio); i++) {
			/* 扫描每个组成页；局部 kmap 只可在当前 CPU、当前不可抢占窗口中使用。 */
		/* local map 生命周期仅覆盖本次内存扫描；任一非零字即可提前释放并返回 false。 */
		data = kmap_local_folio(folio, i * PAGE_SIZE);
		/*
		 * Check last word first, incase the page is zero-filled at
		 * the start and has non-zero data at the end, which is common
		 * in real-world workloads.
		 */
		if (data[last_pos]) {
			/* 末字命中是常见快速失败路径；先 kunmap 保持 local 映射严格成对。 */
			/* 所有早退分支先解除 local map，避免 kmap 栈深度泄漏。 */
			kunmap_local(data);
			return false;
		}
		for (pos = 0; pos < last_pos; pos++) {
			/* 若末字为零才扫描余下字，发现任意非零字同样立即结束该 folio 的优化机会。 */
			if (data[pos]) {
				kunmap_local(data);
				return false;
			}
		}
		kunmap_local(data);
	}

	return true;
}

/* 全零检测完成后才允许调用 zeromap set；检测本身不持有 swap cluster 锁。 */

/*
 * 业务背景：把全零 swapout 的每个 entry 发布为零映射，供后续 swapin 本地填零。
 * 入参：folio 为已锁定的 swapcache folio，借用；其 entry 必须落在可锁定的 cluster 中。
 * 出参/返回：无直接返回值；写入 zeromap 位并累计 VM/objcg 逻辑写出统计。
 * 注意事项：folio lock 稳定 entry，cluster lock 原子保护位图；objcg 引用仅用于统计后归还。
 */
static void swap_zeromap_folio_set(struct folio *folio)
{
	/* set 与 clear 都遍历 folio 页数，确保大 folio 的每一个连续 swap entry 表意一致。 */
	/* zeromap 是 swap-table 元数据优化，不改 folio 数据；真正零化只在读回命中时发生。 */
	/* 锁住 swap cluster 后把 folio 所有 entry 标零；folio lock 保证 swapcache/entry 稳定。 */
	struct obj_cgroup *objcg = get_obj_cgroup_from_folio(folio);
	int nr_pages = folio_nr_pages(folio);
	struct swap_cluster_info *ci;
	swp_entry_t entry;
	unsigned int i;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapcache(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);

	ci = swap_cluster_get_and_lock(folio);
	/* 获取锁同时选择 folio 首 entry 所在 cluster；large folio 假定其 entry 在同一 cluster 内。 */
	/* cluster 锁把多个 entry 的 zero 位更新合并为一个原子观察区间。 */
	/* cluster 锁覆盖一组 swap entry 的 zeromap 位，folio lock 则稳定 entry 到 cluster 的映射。 */
	for (i = 0; i < folio_nr_pages(folio); i++) {
		/* 以 page_swap_entry 逐页定位，避免大 folio 错把首 entry 状态复制到整个 folio。 */
		/* large folio 跨多个连续 entry，必须逐 base page 设置而非只处理 folio 首 entry。 */
		entry = page_swap_entry(folio_page(folio, i));
		__swap_table_set_zero(ci, swp_cluster_offset(entry));
	}
	swap_cluster_unlock(ci);

	count_vm_events(SWPOUT_ZERO, nr_pages);
	/* 统计保持“逻辑写出”视图，即便 zeromap 实际节省了设备带宽。 */
	/* zero 写出同样记入 swapout 统计，避免 I/O 优化让内存行为统计失真。 */
	if (objcg) {
		count_objcg_events(objcg, SWPOUT_ZERO, nr_pages);
		obj_cgroup_put(objcg);
	}
}

/* objcg 统计引用结束后，zeromap 位仍保持由 swap cluster 锁发布的状态。 */

/*
 * 业务背景：真实非零写出前撤销旧 zeromap，防止复用 slot 的 swapin 读到伪造零页。
 * 入参：folio 为已锁定的 swapcache folio，借用。
 * 出参/返回：无直接返回值；清除其全部 base-page entry 的 zero 位。
 * 注意事项：与 set 使用相同 cluster lock；folio lock 与该锁分别稳定映射和位图更新。
 */
static void swap_zeromap_folio_clear(struct folio *folio)
{
	/* 非零写入前清旧标记，防止复用 entry 后 swapin 错把真实数据构造成零页。 */
	struct swap_cluster_info *ci;
	swp_entry_t entry;
	unsigned int i;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapcache(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);

	ci = swap_cluster_get_and_lock(folio);
	/* clear 使用同样粒度/锁，避免新非零数据与旧 zero 标记并发可见。 */
	for (i = 0; i < folio_nr_pages(folio); i++) {
		entry = page_swap_entry(folio_page(folio, i));
		__swap_table_clear_zero(ci, swp_cluster_offset(entry));
	}
	swap_cluster_unlock(ci);
}

/*
 * We may have stale swap cache pages in memory: notice
 * them here and get rid of the unnecessary final write.
 */
/*
 * 业务背景：reclaim 将脏 swapcache folio 写出的总调度点，选择 zeromap、zswap 或后端 I/O。
 * 入参：folio 是调用者锁定的待写对象；swap_plug 是可空的文件 I/O 聚合槽，输入输出且由调用者保存。
 * 出参/返回：返回 0/架构错误，或 AOP_WRITEPAGE_ACTIVATE；未提交路径解锁，已提交路径由 completion 解锁。
 * 注意事项：entry 复用、zeromap 与 memcg 策略都有并发边界；不得在交给后端后自行解锁。
 */
int swap_writeout(struct folio *folio, struct swap_iocb **swap_plug)
{
	/* reclaim 写出决策：已释放 entry→架构准备→zero/zswap→设备 I/O；本函数负责解锁多数快路径。 */
	int ret = 0;

	if (folio_free_swap(folio))
		/* 释放 entry 成功意味着其他路径已处理其存储责任，此处仅完成锁交接。 */
		/* 已从 swapcache 脱离时 entry 归还完成，写出请求成为陈旧回收工作。 */
		/* swapcache 中可能有已失效条目；无需 I/O，直接结束本次 folio 锁持有。 */
		goto out_unlock;

	/*
	 * Arch code may have to preserve more data than just the page
	 * contents, e.g. memory tags.
	 */
	ret = arch_prepare_to_swap(folio);
	/* 架构可能需保存 tag 等页内容之外的信息，失败必须重脏以免丢失可写数据。 */
	if (ret) {
		folio_mark_dirty(folio);
		goto out_unlock;
	}

	/*
	 * Use the swap table zero mark to avoid doing IO for zero-filled
	 * pages. The zero mark is protected by the cluster lock, which is
	 * acquired internally by swap_zeromap_folio_set/clear.
	 */
	if (is_folio_zero_filled(folio)) {
		/* 写全零不发 bio，swapin 将按 zeromap 直接填充，且仍需结束 folio 锁。 */
		/* zero 标记替代磁盘 I/O，但仍保留 swap entry 与统计，读回端会显式构造零数据。 */
		swap_zeromap_folio_set(folio);
		goto out_unlock;
	}

	/*
	 * Clear bits this folio occupies in the zeromap to prevent zero data
	 * being read in from any previous zero writes that occupied the same
	 * swap entries.
	 */
	swap_zeromap_folio_clear(folio);
	/* 清理旧零位先于 zswap/设备写，复用 swap entry 时读端不会返回过期零数据。 */
	/* 到真实写出前先撤销过去同 slot 的 zero 语义，保证读取永远以最新写出为准。 */

	if (zswap_store(folio)) {
		/* 压缩缓存接纳后，后端写入链路终止；对象统计独立标记 zswap 写出。 */
		/* zswap 接管成功后 folio 可结束写出；不再落盘，统计另计压缩写出。 */
		count_mthp_stat(folio_order(folio), MTHP_STAT_ZSWPOUT);
		goto out_unlock;
	}

	rcu_read_lock();
	/* zswap writeback 策略随 memcg 生命周期变化，RCU 包住读取避免解引用拆除中的 cgroup 状态。 */
	if (!mem_cgroup_zswap_writeback_enabled(folio_memcg(folio))) {
		/* cgroup 禁止 zswap writeback 时重新激活 folio，避免交换循环绕过租户策略。 */
		rcu_read_unlock();
		folio_mark_dirty(folio);
		return AOP_WRITEPAGE_ACTIVATE;
	}
	rcu_read_unlock();

	__swap_writepage(folio, swap_plug);
	return 0;
out_unlock:
	/* 未提交 bio/iocb 的路径由这里解除 folio 锁；已提交路径由相应 completion 解锁。 */
	folio_unlock(folio);
	return ret;
}

/*
 * 业务背景：各 swapout 后端在提交前统一记账，避免 backend 选择扭曲 VM、memcg 与 THP 统计。
 * 入参：folio 是待写的借用 folio，order 和页数决定统计粒度。
 * 出参/返回：无直接返回值；只增加统计，不改变 folio、引用或锁状态。
 * 注意事项：受 CONFIG_TRANSPARENT_HUGEPAGE 分流；它不是 I/O 完成确认，不能据此释放资源。
 */
static inline void count_swpout_vm_event(struct folio *folio)
{
	/* 事件在实际后端提交前记录，三种 backend 对统计的可见语义保持一致。 */
	/* 统计以 folio order/page 数兼容 THP、MTHP 与 memcg 口径，和实际 bio 提交点配对。 */
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (unlikely(folio_test_pmd_mappable(folio))) {
		/* THP 单独按一个巨大对象计数，普通 PSWPOUT 仍准确累计其基础页数量。 */
		/* PMD-mappable THP 另记一个对象事件，同时仍按实际 base page 数累计普通 swapout。 */
		count_memcg_folio_events(folio, THP_SWPOUT, 1);
		count_vm_event(THP_SWPOUT);
	}
#endif
	count_mthp_stat(folio_order(folio), MTHP_STAT_SWPOUT);
	count_memcg_folio_events(folio, PSWPOUT, folio_nr_pages(folio));
	count_vm_events(PSWPOUT, folio_nr_pages(folio));
}

/* 统计 helper 返回后，后端只需维护实际 I/O 完成状态，不再补记 swapout 指标。 */

#if defined(CONFIG_MEMCG) && defined(CONFIG_BLK_CGROUP)
/*
 * 业务背景：可选地把 folio 所属 memcg 的 IO cgroup 归属附到 swap bio，保留限速与记账语义。
 * 入参：bio 为尚未提交的借用请求；folio 为提供 memcg 的借用页。
 * 出参/返回：无直接返回值；成功关联 css，失败则保持根 cgroup 归属。
 * 注意事项：RCU 仅保护查找，css_tryget 把生命周期延长到关联完成；配置关闭时使用无副作用宏桩。
 */
static void bio_associate_blkg_from_page(struct bio *bio, struct folio *folio)
{
	/* 该可选钩子失败时退回未绑定 bio，不能让归属记账失败阻断数据正确性。 */
	/* 将内存 cgroup 的 I/O css 绑定 bio，使 swap I/O 也接受 blk-cgroup 限速/记账。 */
	struct cgroup_subsys_state *css;
	/* css 的 tryget 将 I/O 归属延长到 bio 绑定完成，不能只依赖短暂 RCU 读锁。 */
	struct mem_cgroup *memcg;

	if (!folio_memcg_charged(folio))
		/* 未 charged folio 不应凭空绑定 css，直接走根 I/O 归属。 */
		return;

	rcu_read_lock();
	/* css_tryget 取得跨 bio 提交的引用，离开 RCU 后仍可安全交给 block cgroup 绑定接口。 */
	memcg = folio_memcg(folio);
	css = cgroup_e_css(memcg->css.cgroup, &io_cgrp_subsys);
	if (!css || !css_tryget(css))
		css = NULL;
	rcu_read_unlock();

	bio_associate_blkg_from_css(bio, css);
	if (css)
		css_put(css);
}

/* 未编译 memcg+blkcg 时宏桩保持 bio 提交语义，仅省去归属记账。 */
#else
#define bio_associate_blkg_from_page(bio, folio)		do { } while (0)
#endif /* CONFIG_MEMCG && CONFIG_BLK_CGROUP */

struct swap_iocb {
	/* len 是整个 bvec 队列的字节长度，completion 以它判定短 I/O，不按单页判断。 */
	/* 文件型 swap 的聚合 I/O 容器：一组连续 bvec 共用 kiocb，完成时逐页结束 writeback/read。 */
	struct kiocb		iocb;
	struct bio_vec		bvecs[SWAP_CLUSTER_MAX];
	int			nr_bvecs;
	int			len;
};
static mempool_t *sio_pool;
/* file swap completion 可能在内存压力中运行，mempool 保证有 iocb 容器可用而不递归 reclaim。 */

/*
 * 业务背景：文件型 swap 的异步聚合请求必须预留容器，避免内存压力中分配反向触发 reclaim。
 * 入参：无。
 * 出参/返回：成功返回 0；所有竞争者均无法创建/观察到 pool 时返回 -ENOMEM。
 * 注意事项：cmpxchg 只发布一个 pool，落选者释放私有 pool；发布后指针不在本文件中替换。
 */
int sio_pool_init(void)
{
	/* 返回 -ENOMEM 代表没有任何已发布 pool；调用者须在启用文件 swap 前处理该初始化失败。 */
	/* 一次性发布全局 mempool；cmpxchg 输家销毁私建 pool，确保并发初始化不泄漏。 */
	if (!sio_pool) {
		/* 初始化并发下只允许一个 pool 获胜发布，其它临时 pool 立即销毁。 */
		/* 无锁快路径先读指针；cmpxchg 负责多个初始化竞争者的唯一发布。 */
		mempool_t *pool = mempool_create_kmalloc_pool(
			SWAP_CLUSTER_MAX, sizeof(struct swap_iocb));
		if (cmpxchg(&sio_pool, NULL, pool))
			/* 失败返回的旧值表明已有发布者，本地 pool 从未对任何请求可见。 */
			mempool_destroy(pool);
	}
	if (!sio_pool)
		return -ENOMEM;
	return 0;
}

/* pool 初始化完成后 sio_pool 永不替换；销毁由 swap 子系统的更高层生命周期负责。 */

/*
 * 业务背景：文件 swap 的 swap_rw 完成回调，把一批 bvec 的写结果逐 folio 回写到 writeback 状态机。
 * 入参：iocb 嵌入被完成路径持有的 swap_iocb；ret 是完成字节数或负错误。
 * 出参/返回：无直接返回值；短写重脏全部 folio，逐项 end_writeback 后归还 sio pool。
 * 注意事项：只有全长写才成功；本函数消费 sio，调用后不得保留其或 iocb 的指针。
 */
static void sio_write_complete(struct kiocb *iocb, long ret)
{
	/* sio 从 iocb 反查，completion 独占其生命周期，函数返回后无调用者可继续使用它。 */
	/* 文件 swap 写完成：短写使所有 bvec 重脏/清 reclaim，随后逐页结束 writeback 并归还 pool。 */
	struct swap_iocb *sio = container_of(iocb, struct swap_iocb, iocb);
	struct page *page = sio->bvecs[0].bv_page;
	int p;

	if (ret != sio->len) {
		/* 文件 a_ops 的短完成与设备错误同等处理，整批页恢复重试资格。 */
		/* 写入长度严格匹配才算持久化成功；短写和负错统一让每页重新变脏。 */
		/* NFS 等文件 swap 可临时短写；保守重脏保证回收不会吞掉尚未落盘的数据。 */
		/*
		 * In the case of swap-over-nfs, this can be a
		 * temporary failure if the system has limited
		 * memory for allocating transmit buffers.
		 * Mark the page dirty and avoid
		 * folio_rotate_reclaimable but rate-limit the
		 * messages.
		 */
		pr_err_ratelimited("Write error %ld on dio swapfile (%llu)\n",
				   ret, swap_dev_pos(page_swap_entry(page)));
		for (p = 0; p < sio->nr_bvecs; p++) {
			page = sio->bvecs[p].bv_page;
			set_page_dirty(page);
			ClearPageReclaim(page);
		}
	}

	for (p = 0; p < sio->nr_bvecs; p++)
		/* start_writeback 在聚合前按 folio 调用，因此结束也必须逐 bvec 进行。 */
		/* 每个 folio 在聚合前独立 start_writeback，因此必须逐 bvec 完成配对。 */
		end_page_writeback(sio->bvecs[p].bv_page);

	mempool_free(sio, sio_pool);
}

/*
 * 业务背景：文件后端将连续 swap offset 的 folio 聚合为一次 swap_rw 写，降低 I/O 提交开销。
 * 入参：folio 是已锁的借用 swapcache 页；swap_plug 为可空输入输出队列槽，所有权留给调用者。
 * 出参/返回：无直接返回值；启动 writeback、解锁 folio，并提交或保存 sio。
 * 注意事项：仅同文件连续偏移可合并；GFP_NOIO 防递归 reclaim，completion 才能结束 writeback。
 */
static void swap_writepage_fs(struct folio *folio, struct swap_iocb **swap_plug)
{
	/* 文件 swap 只批量合并同一 file 且连续偏移的 folio；不连续时先 unplug 前一组。 */
	struct swap_iocb *sio = swap_plug ? *swap_plug : NULL;
	struct swap_info_struct *sis = __swap_entry_to_info(folio->swap);
	struct file *swap_file = sis->swap_file;
	loff_t pos = swap_dev_pos(folio->swap);

	count_swpout_vm_event(folio);
	folio_start_writeback(folio);
	/* writeback 先于 unlock 建立，确保 reclaim/等待者不把正在提交的页误作普通干净页。 */
	/* 在提交前置 writeback、解锁 folio；完成回调承担与该状态严格配对的 end。 */
	folio_unlock(folio);
	if (sio) {
		/* plug 不可跨文件或物理连续性边界，否则文件 a_ops 无法一次合法提交该 bvec 串。 */
		if (sio->iocb.ki_filp != swap_file ||
		    sio->iocb.ki_pos + sio->len != pos) {
			swap_write_unplug(sio);
			sio = NULL;
		}
	}
	if (!sio) {
		/* mempool/GFP_NOIO 避免 swapout 再触发文件系统回收死锁；新 iocb 从当前偏移开始。 */
		sio = mempool_alloc(sio_pool, GFP_NOIO);
		init_sync_kiocb(&sio->iocb, swap_file);
		sio->iocb.ki_complete = sio_write_complete;
		sio->iocb.ki_pos = pos;
		sio->nr_bvecs = 0;
		sio->len = 0;
	}
	bvec_set_folio(&sio->bvecs[sio->nr_bvecs], folio, folio_size(folio), 0);
	/* bvec 持有 folio 内容到异步完成；folio 锁已释放但 writeback 状态阻止错误回收。 */
	sio->len += folio_size(folio);
	sio->nr_bvecs += 1;
	if (sio->nr_bvecs == ARRAY_SIZE(sio->bvecs) || !swap_plug) {
		swap_write_unplug(sio);
		sio = NULL;
	}
	if (swap_plug)
		*swap_plug = sio;
}

/* write plug 为空代表本批已提交；非空时仅由调用者保存并在边界 unplug。 */

/*
 * 业务背景：同步块设备 swapout 在当前上下文等待 I/O，但复用异步路径的状态收尾语义。
 * 入参：folio 为已锁的借用页；sis 为选择好的借用 swap 后端描述符。
 * 出参/返回：无直接返回值；返回前 writeback 已结束、folio 已由共享 helper 处理。
 * 注意事项：bio 位于栈上，只能配 submit_bio_wait；不得把它交给异步 completion。
 */
static void swap_writepage_bdev_sync(struct folio *folio,
		struct swap_info_struct *sis)
{
	/* 栈对象的同步 bio 与异步 heap bio 不能互换，否则 completion 后会访问失效栈内存。 */
	/* 关联 blkg、统计、writeback 状态在同步/异步分支保持同一顺序。 */
	/* 块设备同步路径用栈 bio 等待完成，返回前手工调用公共 write completion。 */
	struct bio_vec bv;
	struct bio bio;

	bio_init(&bio, sis->bdev, &bv, 1, REQ_OP_WRITE | REQ_SWAP);
	/* 同步路径的栈 bio 在 submit_bio_wait 返回前有效，随后直接调用共享完成逻辑。 */
	/* 栈 bio 仅同步 submit_bio_wait 期间有效，不能用于 async 路径。 */
	bio.bi_iter.bi_sector = swap_folio_sector(folio);
	bio_add_folio_nofail(&bio, folio, folio_size(folio), 0);

	bio_associate_blkg_from_page(&bio, folio);
	count_swpout_vm_event(folio);

	folio_start_writeback(folio);
	folio_unlock(folio);

	submit_bio_wait(&bio);
	__end_swap_bio_write(&bio);
}

/* 同步提交返回时 writeback 已被结束；异步分支在 end_io 到达前仍维持该状态。 */

/*
 * 业务背景：普通块设备 swapout 创建异步 bio，把完成、错误恢复与释放交给 block completion。
 * 入参：folio 为已锁借用页；sis 为借用后端描述符。
 * 出参/返回：无直接返回值；提交后 folio 保持 writeback，bio 所有权转移给 block 层。
 * 注意事项：用 GFP_NOIO 防回收递归；提交后不可访问 bio，也不可提前解除 writeback。
 */
static void swap_writepage_bdev_async(struct folio *folio,
		struct swap_info_struct *sis)
{
	/* async end_io 获得 bio 后独占释放责任，提交函数返回时只能保留 folio 的状态机观察。 */
	/* end_swap_bio_write 最终负责 error dirty 和 folio_end_writeback，提交点不做同步等待。 */
	/* 异步块路径将 bio 所有权交给 block 层，end_io 负责 folio 状态与 bio_put。 */
	struct bio *bio;

	bio = bio_alloc(sis->bdev, 1, REQ_OP_WRITE | REQ_SWAP, GFP_NOIO);
	/* 异步路径把 bio 分配为 NOIO，规避 swapout 再触发 reclaim 的递归依赖。 */
	/* async bio 用 NOIO 分配避免写 swap 时再进入可能依赖 swap 的回收链。 */
	bio->bi_iter.bi_sector = swap_folio_sector(folio);
	bio->bi_end_io = end_swap_bio_write;
	bio_add_folio_nofail(bio, folio, folio_size(folio), 0);

	bio_associate_blkg_from_page(bio, folio);
	/* I/O cgroup 归属、统计与写回状态必须在 submit 前全部准备完毕。 */
	count_swpout_vm_event(folio);
	folio_start_writeback(folio);
	folio_unlock(folio);
	submit_bio(bio);
}

/* bio 提交后该函数不得访问 bio；completion 是唯一的释放和错误恢复所有者。 */

/*
 * 业务背景：这是 swapout 后端分派层，按稳定的 swap flags 选择文件、同步块或异步块实现。
 * 入参：folio 是已锁的 swapcache 借用页；swap_plug 是可空的文件写聚合输入输出槽。
 * 出参/返回：无直接返回值；被选后端接管 folio unlock/writeback 的配对。
 * 注意事项：flags 的 data_race 只读取运行期不改变的类别位；非 swapcache 输入是内核错误。
 */
void __swap_writepage(struct folio *folio, struct swap_iocb **swap_plug)
{
	/* 该 helper 假定 folio 仍是 swapcache；不同后端最终各自接管 folio unlock/writeback 配对。 */
	/* 依据 swap backend 选择文件聚合、同步 bdev 或异步 bdev；flags 的 racy 读取只测稳定类别位。 */
	struct swap_info_struct *sis = __swap_entry_to_info(folio->swap);

	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);
	/* swap entry 来自 folio->swap；非 swapcache folio 的 backend 查询是内核 bug。 */
	/*
	 * ->flags can be updated non-atomically,
	 * but that will never affect SWP_FS_OPS, so the data_race
	 * is safe.
	 */
	if (data_race(sis->flags & SWP_FS_OPS))
		/* 后端类型标志在运行中不会改变该语义，故允许 racy 判断以避开额外锁。 */
		swap_writepage_fs(folio, swap_plug);
	/*
	 * ->flags can be updated non-atomically,
	 * but that will never affect SWP_SYNCHRONOUS_IO, so the data_race
	 * is safe.
	 */
	else if (data_race(sis->flags & SWP_SYNCHRONOUS_IO))
		swap_writepage_bdev_sync(folio, sis);
	else
		swap_writepage_bdev_async(folio, sis);
}

/*
 * 业务背景：文件 swap 写在聚合边界把暂存 bvec 真正交给 address_space 的 swap_rw 回调。
 * 入参：sio 是待提交的聚合请求，调用后其所有权转给异步完成或本函数的同步完成路径。
 * 出参/返回：无直接返回值；立即完成时执行 write completion，排队时由文件系统稍后回调。
 * 注意事项：ITER_SOURCE 表示从 folio 读数据；不得重复 unplug 同一 sio。
 */
void swap_write_unplug(struct swap_iocb *sio)
{
	/* 把聚合 bvec 封装为 source iter 交给文件系统 swap_rw；同步返回需本地补做完成回调。 */
	struct iov_iter from;
	struct address_space *mapping = sio->iocb.ki_filp->f_mapping;
	int ret;

	iov_iter_bvec(&from, ITER_SOURCE, sio->bvecs, sio->nr_bvecs, sio->len);
	/* source 迭代器把 folio 内容输送到文件；queued 返回表示 completion 将由文件系统稍后调用。 */
	ret = mapping->a_ops->swap_rw(&sio->iocb, &from);
	if (ret != -EIOCBQUEUED)
		sio_write_complete(&sio->iocb, ret);
}

/*
 * 业务背景：文件 swap 读回的聚合完成点，将全长数据原子地逐 folio 发布给 fault 路径。
 * 入参：iocb 嵌入完成路径拥有的 swap_iocb；ret 为完成字节数或错误。
 * 出参/返回：无直接返回值；全长时置 uptodate 并解锁，失败仅解锁，最后归还 sio。
 * 注意事项：短读不是部分成功；本函数消费 sio，任何调用者均不得在返回后使用它。
 */
static void sio_read_complete(struct kiocb *iocb, long ret)
{
	/* 文件 swap 读完成：全长成功才逐 folio 标 uptodate+解锁；任一短读保持失败语义。 */
	struct swap_iocb *sio = container_of(iocb, struct swap_iocb, iocb);
	int p;

	if (ret == sio->len) {
		/* 严格全长读取才可以发布 uptodate；短读即使部分数据存在也不得解锁为成功。 */
		/* 每个 bvec 可是多页 folio，统计与状态更新均按 folio 而非单 base page。 */
		for (p = 0; p < sio->nr_bvecs; p++) {
			struct folio *folio = bvec_folio(&sio->bvecs[p]);

			count_mthp_stat(folio_order(folio), MTHP_STAT_SWPIN);
			count_memcg_folio_events(folio, PSWPIN, folio_nr_pages(folio));
			folio_mark_uptodate(folio);
			folio_unlock(folio);
		}
		count_vm_events(PSWPIN, sio->len >> PAGE_SHIFT);
	} else {
		/* 失败只解锁，不能设 uptodate；fault 观察者会把未填充 folio 转成 I/O 错误。 */
		for (p = 0; p < sio->nr_bvecs; p++) {
			struct folio *folio = bvec_folio(&sio->bvecs[p]);

			folio_unlock(folio);
		}
		pr_alert_ratelimited("Read-error on swap-device\n");
	}
	mempool_free(sio, sio_pool);
}

/* 文件读完成释放 sio 后，zeromap 仅查询元数据，不会复用该异步 I/O 容器。 */

/*
 * Return the count of contiguous swap entries that share the same
 * zeromap status as the starting entry. If is_zerop is not NULL,
 * it will return the zeromap status of the starting entry.
 *
 * Context: Caller must ensure the cluster containing the entries
 * that will be checked won't be freed.
 */
/*
 * 业务背景：large-folio swapin 先确认一段 entry 的 zeromap 状态一致，避免拼接真假数据。
 * 入参：entry 是起始 swap entry；max_nr 是不跨 cluster 的检查页数；is_zerop 是可空布尔输出。
 * 出参/返回：返回连续同状态 entry 数，并在非空 is_zerop 写入起始 zero 状态。
 * 注意事项：调用者必须确保 cluster 不会释放；RCU 只保护元数据读取，不替代跨 cluster 的锁协议。
 */
static int swap_zeromap_batch(swp_entry_t entry, int max_nr,
			      bool *is_zerop)
{
	/* is_zerop 是可选输出；返回值永远是从起点开始未跨状态边界的连续 entry 数。 */
	/* 只在一个 swap cluster 内观察，调用者因 folio lock/cluster 生命周期而可安全读取位图。 */
	/* 在单 cluster 中找连续相同 zero 状态，供 large folio 判断能否整体免 I/O。 */
	int i;
	bool is_zero;
	unsigned int ci_start = swp_cluster_offset(entry);
	struct swap_cluster_info *ci = __swap_entry_to_cluster(entry);

	VM_WARN_ON_ONCE(ci_start + max_nr > SWAPFILE_CLUSTER);
	/* 跨 cluster 会让连续零判定缺少第二把锁，因此 WARN 后仍由调用前提避免实际发生。 */
	/* 调用者必须保证不跨 cluster；否则单个 ci 锁/RCU 范围无法代表后续 entry。 */

	rcu_read_lock();
	/* zeromap 读侧在 RCU 下避免 cluster table 重组期间取到失效元数据。 */
	is_zero = __swap_table_test_zero(ci, ci_start);
	for (i = 1; i < max_nr; i++)
		if (is_zero != __swap_table_test_zero(ci, ci_start + i))
			break;
	rcu_read_unlock();
	if (is_zerop)
		*is_zerop = is_zero;

	return i;
}

/* 连续长度小于请求页数会被 large-folio 读路径视为不可安全处理的部分零映射。 */

/*
 * 业务背景：swapin 的最快路径依据 zeromap 在内存中构造全零 folio，避免访问 zswap 或设备。
 * 入参：folio 是已锁的借用 swap folio，entry 映射在本调用期间稳定。
 * 出参/返回：已处理或检测到不支持的部分零映射返回 true；未命中返回 false 且不改变 folio。
 * 注意事项：全零数据必须在 uptodate 前写完；部分 large-folio 零映射故意保留失败态供上层报 I/O 错。
 */
static bool swap_read_folio_zeromap(struct folio *folio)
{
	/* objcg 引用仅包住统计更新，零化/uptodate 不需要继续持有 memcg 对象。 */
	/* 部分 zeromap large folio 当前不能安全拼接真实 I/O，故留下失败态让 fault 上层报错。 */
	/* 返回 true 表示已处理（全零填充或检测到不支持的部分零映射），调用者不再发设备 I/O。 */
	int nr_pages = folio_nr_pages(folio);
	struct obj_cgroup *objcg;
	bool is_zeromap;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	/* 读入期间锁稳定 folio 的 swap entry，并使最终 uptodate/解锁成为单一发布点。 */

	/*
	 * Swapping in a large folio that is partially in the zeromap is not
	 * currently handled. Return true without marking the folio uptodate so
	 * that an IO error is emitted (e.g. do_swap_page() will sigbus).
	 * Folio lock stabilizes the cluster and map, so the check is safe.
	 */
	if (WARN_ON_ONCE(swap_zeromap_batch(folio->swap, nr_pages,
			 &is_zeromap) != nr_pages))
		return true;

	if (!is_zeromap)
		/* 未命中 zeromap 时不触碰数据和 uptodate，保留给 zswap 或设备填充。 */
		/* 没有 zero 标记时返回 false，继续 zswap/后端读取；不改变 folio 状态。 */
		return false;

	objcg = get_obj_cgroup_from_folio(folio);
	count_vm_events(SWPIN_ZERO, nr_pages);
	if (objcg) {
		count_objcg_events(objcg, SWPIN_ZERO, nr_pages);
		obj_cgroup_put(objcg);
	}

	folio_zero_range(folio, 0, folio_size(folio));
	/* 零化完整 folio 后再标 uptodate；持有 folio lock 确保 fault 无法观察到半填充数据。 */
	folio_mark_uptodate(folio);
	return true;
}

/* 成功零填充的 folio 仍由 swap_read_folio 解除锁，保持所有读入后端的交接统一。 */

/*
 * 业务背景：文件型 swapin 将相邻文件偏移的读请求聚合，最后由 swap_rw 填入这些 folio。
 * 入参：folio 是已锁借用页；plug 是可空的调用者拥有的聚合槽，输入输出。
 * 出参/返回：无直接返回值；将 folio 加入 sio，满载/无 plug 时提交，否则把队列写回 *plug。
 * 注意事项：只可合并同文件连续区间；完成回调才会置 uptodate 和解锁。
 */
static void swap_read_folio_fs(struct folio *folio, struct swap_iocb **plug)
{
	/* 文件读聚合与写侧相同：只合并同文件连续 offset，plug 为调用者持有的未提交队列。 */
	struct swap_info_struct *sis = __swap_entry_to_info(folio->swap);
	struct swap_iocb *sio = NULL;
	loff_t pos = swap_dev_pos(folio->swap);

	if (plug)
		sio = *plug;
	if (sio) {
		/* plug 只能累积连续 file offset；断裂时提交旧请求并重新初始化新队列。 */
		if (sio->iocb.ki_filp != sis->swap_file ||
		    sio->iocb.ki_pos + sio->len != pos) {
			swap_read_unplug(sio);
			sio = NULL;
		}
	}
	if (!sio) {
		/* read 可用 GFP_KERNEL；构造 destination iter 的真正提交在 unplug 时进行。 */
		sio = mempool_alloc(sio_pool, GFP_KERNEL);
		init_sync_kiocb(&sio->iocb, sis->swap_file);
		sio->iocb.ki_pos = pos;
		sio->iocb.ki_complete = sio_read_complete;
		sio->nr_bvecs = 0;
		sio->len = 0;
		/* 新 iocb 的位置和长度从当前 folio 重新起算，不能继承已 unplug 队列的尾状态。 */
	}
	bvec_set_folio(&sio->bvecs[sio->nr_bvecs], folio, folio_size(folio), 0);
	sio->len += folio_size(folio);
	/* bvec 追加后 len 表示目标迭代器总字节数，completion 用其严格区分全长与短读。 */
	sio->nr_bvecs += 1;
	if (sio->nr_bvecs == ARRAY_SIZE(sio->bvecs) || !plug) {
		swap_read_unplug(sio);
		sio = NULL;
	}
	if (plug)
		*plug = sio;
}

/* 队列边界已处理：接下来的同步或异步 bdev 路径不会共享文件型 sio。 */

/* 文件读 plug 的所有权保留给调用者；其需在批次边界调用 swap_read_unplug。 */

/*
 * 业务背景：同步块设备 swapin 等待物理读取完成，适用于标记为同步的 swap 后端。
 * 入参：folio 是已锁借用页；sis 为借用的块设备 swap 描述符。
 * 出参/返回：无直接返回值；共享 completion 发布数据或失败并解锁 folio。
 * 注意事项：等待期间持有 current 引用以满足 OOM retry 观察；栈 bio 仅在等待返回前有效。
 */
static void swap_read_folio_bdev_sync(struct folio *folio,
		struct swap_info_struct *sis)
{
	/* 读统计在提交前记账，completion 无论成功失败都解除 folio 锁并使 fault 路径继续。 */
	/* submit 完成后公共 read completion 设状态并解锁，task 引用随后归还。 */
	/* 同步 bdev 读保持 current 引用，防 OOM/fault retry 观察路径在 I/O 等待中误判任务消失。 */
	struct bio_vec bv;
	struct bio bio;

	bio_init(&bio, sis->bdev, &bv, 1, REQ_OP_READ);
	/* 同步 swapin 由当前任务等待；额外 task ref 覆盖等待期间 OOM 相关检查。 */
	bio.bi_iter.bi_sector = swap_folio_sector(folio);
	bio_add_folio_nofail(&bio, folio, folio_size(folio), 0);
	/*
	 * Keep this task valid during swap readpage because the oom killer may
	 * attempt to access it in the page fault retry time check.
	 */
	get_task_struct(current);
	/* OOM retry 代码可能从外部观察 current；引用确保同步 I/O 等待期间对象不会过早消失。 */
	/* task 引用跨 submit_bio_wait，结束后严格 put；folio 锁由 __end_swap_bio_read 释放。 */
	count_mthp_stat(folio_order(folio), MTHP_STAT_SWPIN);
	count_memcg_folio_events(folio, PSWPIN, folio_nr_pages(folio));
	count_vm_events(PSWPIN, folio_nr_pages(folio));
	submit_bio_wait(&bio);
	__end_swap_bio_read(&bio);
	put_task_struct(current);
}

/*
 * 业务背景：普通块设备 swapin 非阻塞提交 bio，让 end_io 在数据实际可见时解除 folio 锁。
 * 入参：folio 是已锁借用页；sis 为借用的后端描述符。
 * 出参/返回：无直接返回值；bio 所有权在 submit 后转移，completion 置状态、解锁并释放 bio。
 * 注意事项：提交点前必须填完 sector、bvec 与统计；调用者不能提前使用或解锁 folio。
 */
static void swap_read_folio_bdev_async(struct folio *folio,
		struct swap_info_struct *sis)
{
	/* 异步读 bio 回调统一标页、解锁和 bio_put，提交者不得继续触碰该 bio。 */
	struct bio *bio;

	bio = bio_alloc(sis->bdev, 1, REQ_OP_READ, GFP_KERNEL);
	/* 异步 read 的 bio 所有权在 submit 后移交 block 层，end_io 最终释放。 */
	bio->bi_iter.bi_sector = swap_folio_sector(folio);
	bio->bi_end_io = end_swap_bio_read;
	bio_add_folio_nofail(bio, folio, folio_size(folio), 0);
	count_mthp_stat(folio_order(folio), MTHP_STAT_SWPIN);
	count_memcg_folio_events(folio, PSWPIN, folio_nr_pages(folio));
	count_vm_events(PSWPIN, folio_nr_pages(folio));
	submit_bio(bio);
}

/* async 读没有本地解锁出口，folio 仅在 end_swap_bio_read 处理完状态后可被 fault 使用。 */

/*
 * 业务背景：缺页/readahead 的 swapin 总入口，依次尝试 zeromap、zswap 和慢速文件/块设备后端。
 * 入参：folio 是已锁且未 uptodate 的借用页；plug 是可空的文件读聚合输入输出槽。
 * 出参/返回：无直接返回值；后端或快路径负责 folio 状态，函数始终结束本次 PSI/delay 计时。
 * 注意事项：同步后端可例外不在 swapcache；workingset 计时必须严格成对，慢设备读取前保护 zswap。
 */
void swap_read_folio(struct folio *folio, struct swap_iocb **plug)
{
	/* swapin 编排：PSI/delay 计时→zeromap→zswap→慢设备，所有出口最终关闭计时范围。 */
	struct swap_info_struct *sis = __swap_entry_to_info(folio->swap);
	bool synchronous = sis->flags & SWP_SYNCHRONOUS_IO;
	bool workingset = folio_test_workingset(folio);
	unsigned long pflags;
	bool in_thrashing;

	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio) && !synchronous, folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	/* 调用契约要求锁定且未 uptodate；同步特殊 backend 允许不在 swapcache。 */
	VM_BUG_ON_FOLIO(folio_test_uptodate(folio), folio);

	/*
	 * Count submission time as memory stall and delay. When the device
	 * is congested, or the submitting cgroup IO-throttled, submission
	 * can be a significant part of overall IO time.
	 */
	if (workingset) {
		/* workingset swapin 计为 thrashing/memstall，覆盖提交和等待前的关键路径。 */
		delayacct_thrashing_start(&in_thrashing);
		psi_memstall_enter(&pflags);
	}
	delayacct_swapin_start();

	if (swap_read_folio_zeromap(folio)) {
		/* zeromap 成功时本地填零后立即解锁；部分零异常则保留非 uptodate 供错误处理。 */
		folio_unlock(folio);
		goto finish;
	}

	if (zswap_load(folio) != -ENOENT)
		/* zswap 命中或报告错误均已处理其 folio 状态，只有 ENOENT 才需后端设备读取。 */
		goto finish;

	/* We have to read from slower devices. Increase zswap protection. */
	zswap_folio_swapin(folio);
	/* 去慢设备前增加 zswap 保护，防正在被读取的数据被并发 writeback/回收。 */

	if (data_race(sis->flags & SWP_FS_OPS)) {
		swap_read_folio_fs(folio, plug);
	} else if (synchronous) {
		swap_read_folio_bdev_sync(folio, sis);
	} else {
		swap_read_folio_bdev_async(folio, sis);
	}

finish:
	/* 无论哪种后端，成对退出 PSI/delay accounting，防止 task 状态泄漏到后续 CPU 时间。 */
	if (workingset) {
		delayacct_thrashing_end(&in_thrashing);
		psi_memstall_leave(&pflags);
	}
	delayacct_swapin_end();
}

/*
 * 业务背景：文件 swapin 在聚合边界将 destination bvec 提交给 a_ops->swap_rw。
 * 入参：sio 是待提交聚合请求；调用后其所有权由 queued completion 或同步完成路径消费。
 * 出参/返回：无直接返回值；立即结果直接调用 read completion，排队结果由文件系统回调。
 * 注意事项：ITER_DEST 指明数据写入 folio；同一 sio 只能提交一次。
 */
void __swap_read_unplug(struct swap_iocb *sio)
{
	/* 与写 unplug 对称：destination iter 表示设备数据写入 folio，立即完成时本地调用 read complete。 */
	struct iov_iter from;
	struct address_space *mapping = sio->iocb.ki_filp->f_mapping;
	int ret;

	iov_iter_bvec(&from, ITER_DEST, sio->bvecs, sio->nr_bvecs, sio->len);
	ret = mapping->a_ops->swap_rw(&sio->iocb, &from);
	if (ret != -EIOCBQUEUED)
		sio_read_complete(&sio->iocb, ret);
}
