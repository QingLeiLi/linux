// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/swap_state.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *
 *  Rewritten to use page cache, (C) 1998 Stephen Tweedie
 */
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
/* 分配 flags、统计和地址空间定义：cache folio 的通用 VM 基础设施。 */
#include <linux/mempolicy.h>
#include <linux/swap.h>
#include <linux/leafops.h>
/* swap entry、NUMA policy 与 leaf PTE 表示：fault 到槽位的转换接口。 */
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/folio_batch.h>
/* init、page-cache 与批量引用操作：发布和回收阶段使用。 */
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/migrate.h>
/* block I/O 与迁移回调：换入提交和 cache folio 替换的边界。 */
#include <linux/vmalloc.h>
#include <linux/huge_mm.h>
#include <linux/shmem_fs.h>
/* 头文件按层提供：mm/folio 基础对象、swap 槽位与 I/O、NUMA/memcg 策略；
 * 本文件不定义用户 ABI，只实现这些已声明的内核内部状态转换。
 */
/* swap cache 同时连接通用 folio/LRU、块设备 I/O、NUMA 策略和 memcg；私有
 * swap_table/swap 头补充槽位编码与 cluster 锁接口，避免公开 ABI 泄露细节。
 */
#include "internal.h"
#include "swap_table.h"
#include "swap.h"

/*
 * swapper_space is a fiction, retained to simplify the path through
 * vmscan's shrink_folio_list.
 */
/*
 * 补充说明：swap cache 没有普通文件的 inode/address_space；这个虚拟
 * mapping 把换入 folio 纳入 page-cache/LRU 的通用回收路径。它只提供
 * dirty 与迁移回调，实际 swap 槽位的生命周期仍由 swap table/cluster 锁
 * 管理，不能把它当作可写回的文件映射。
 */
static const struct address_space_operations swap_aops = {
	.dirty_folio	= noop_dirty_folio,
#ifdef CONFIG_MIGRATION
	.migrate_folio	= migrate_folio,
#endif
};

struct address_space swap_space __read_mostly = {
	.a_ops = &swap_aops,
};

/*
 * 全局开关由 sysfs 写入、热路径用 READ_ONCE 读取；它只选择按 VMA
 * 虚拟地址预测还是按 swap 物理 cluster 预测，不改变换页数据正确性。
 */

static bool enable_vma_readahead __read_mostly = true;

/* 以下 packed 位字段把单个 atomic_long 作为 VMA 预读历史，更新时整体写回，
 * 不需额外锁；地址低页内位不会参与页对齐地址，故可承载窗口与命中计数。
 */
#define SWAP_RA_ORDER_CEILING	5

#define SWAP_RA_WIN_SHIFT	(PAGE_SHIFT / 2)
#define SWAP_RA_HITS_MASK	((1UL << SWAP_RA_WIN_SHIFT) - 1)
#define SWAP_RA_HITS_MAX	SWAP_RA_HITS_MASK
#define SWAP_RA_WIN_MASK	(~PAGE_MASK & ~SWAP_RA_HITS_MASK)

#define SWAP_RA_HITS(v)		((v) & SWAP_RA_HITS_MASK)
#define SWAP_RA_WIN(v)		(((v) & SWAP_RA_WIN_MASK) >> SWAP_RA_WIN_SHIFT)
#define SWAP_RA_ADDR(v)		((v) & PAGE_MASK)

/* SWAP_RA_VAL 生成可原子发布的地址/窗口/命中快照；宏续行必须保持连续，
 * 因此只在其外部解释，不能在反斜杠序列中插入学习注释。
 */
#define SWAP_RA_VAL(addr, win, hits)				\
	(((addr) & PAGE_MASK) |					\
	 (((win) << SWAP_RA_WIN_SHIFT) & SWAP_RA_WIN_MASK) |	\
	 ((hits) & SWAP_RA_HITS_MASK))

/* Initial readahead hits is 4 to start up with a small window */
#define GET_SWAP_RA_VAL(vma)					\
	(atomic_long_read(&(vma)->swap_readahead_info) ? : 4)

static atomic_t swapin_readahead_hits = ATOMIC_INIT(4);

/*
 * show_swap_cache_info() - 输出 swap cache 与设备容量快照。
 * 业务背景：OOM/内存诊断路径需要把缓存页数和可用槽位放在同一报告中；
 * 调用者通常是 show_mem()，随后继续输出其余 VM 统计。
 * 入参：无。出参/返回：无直接返回值；向内核日志写入三个瞬时计数。
 * 注意事项：这些统计并非同一把锁下的原子快照，只用于诊断，不能据此
 * 作资源分配决定，也不能在不可打印的上下文调用。
 */
void show_swap_cache_info(void)
{
	printk("%lu pages in swap cache\n", total_swapcache_pages());
	printk("Free swap  = %ldkB\n", K(get_nr_swap_pages()));
	printk("Total swap = %lukB\n", K(total_swap_pages));
}

/**
 * swap_cache_get_folio - Looks up a folio in the swap cache.
 * @entry: swap entry used for the lookup.
 *
 * A found folio will be returned unlocked and with its refcount increased.
 *
 * Context: Caller must ensure @entry is valid and protect the swap device
 * with reference count or locks.
 * Return: Returns the found folio on success, NULL otherwise. The caller
 * must lock and check if the folio still matches the swap entry before
 * use (e.g., folio_matches_swap_entry).
 */
/*
 * 业务背景：缺页和 swapoff 都要先按槽位查找已经在途或已完成的换入，
 * 避免为同一数据启动重复 I/O。入参：entry 是调用者已稳定设备引用或锁
 * 保护的有效 swap type+offset，仅借用。出参/返回：成功返回一个已加引用、
 * 未加 folio 锁的 folio；未命中返回 NULL，调用者随后可尝试分配或退出。
 * 注意事项：swap table 的可见性不等于 folio 生命周期；try_get 失败说明
 * 回收者正在最终释放，必须重查表，取得引用后仍须加锁验证 entry 配对。
 */
struct folio *swap_cache_get_folio(swp_entry_t entry)
{
	unsigned long swp_tb;
	struct folio *folio;

	/* 循环把“表中仍指向 folio、但引用恰好归零”的拆除竞态变成重新查找。 */
	for (;;) {
		/* table 项可能是 shadow/空；只有 PFN 编码才可转换并尝试持有引用。 */
		swp_tb = swap_table_get(__swap_entry_to_cluster(entry),
					swp_cluster_offset(entry));
		if (!swp_tb_is_folio(swp_tb))
			return NULL;
		/* folio_try_get 是生命周期取得点；失败后禁止使用裸 PFN 反解结果。 */
		folio = swp_tb_to_folio(swp_tb);
		if (likely(folio_try_get(folio)))
			return folio;
	}

	return NULL;
}

/**
 * swap_cache_has_folio - Check if a swap slot has cache.
 * @entry: swap entry indicating the slot.
 *
 * Context: Caller must ensure @entry is valid and protect the swap
 * device with reference count or locks.
 */
/*
 * 业务背景：只需判断槽位是否有 cache 时避免获取/释放 folio 引用。
 * 入参：entry 为已由调用者稳定的槽位借用值。出参/返回：true 仅表示表项
 * 当前编码为 folio，false 表示空、shadow 或设备条目；不转移任何所有权。
 * 注意事项：这是无引用的瞬时观察，不能据此解引用或保证后续仍命中；需要
 * 实体时必须改用 swap_cache_get_folio() 并完成锁下二次验证。
 */
bool swap_cache_has_folio(swp_entry_t entry)
{
	unsigned long swp_tb;

	swp_tb = swap_table_get(__swap_entry_to_cluster(entry),
				swp_cluster_offset(entry));
	return swp_tb_is_folio(swp_tb);
}

/**
 * swap_cache_get_shadow - Looks up a shadow in the swap cache.
 * @entry: swap entry used for the lookup.
 *
 * Context: Caller must ensure @entry is valid and protect the swap device
 * with reference count or locks.
 * Return: Returns either NULL or an XA_VALUE (shadow).
 */
/*
 * 业务背景：folio 被驱逐后保留 shadow，以便 workingset 根据 refault 距离
 * 判断是否应提升新换入页。入参：entry 是受设备生命周期保护的借用槽位。
 * 出参/返回：返回编码值或 NULL；它不是可解引用对象，也不附带引用。
 * 注意事项：cluster 锁外只能把该值作为一次查询结果；重新写入 swap table
 * 会使它过期，调用者不得跨越可能并发回收的边界保存它。
 */
void *swap_cache_get_shadow(swp_entry_t entry)
{
	unsigned long swp_tb;

	swp_tb = swap_table_get(__swap_entry_to_cluster(entry),
				swp_cluster_offset(entry));
	if (swp_tb_is_shadow(swp_tb))
		return swp_tb_to_shadow(swp_tb);
	return NULL;
}

/**
 * __swap_cache_add_check - Check if a range is suitable for adding a folio.
 * @ci: The locked swap cluster
 * @targ_entry: The target swap entry to check, will be rounded down by @nr
 * @nr: Number of slots to check, must be a power of 2
 * @shadowp: Returns the shadow value if one exists in the range
 * @memcg_id: Returns the memory cgroup id, NULL to ignore cgroup check
 *
 * Check if all slots covered by given range have a swap count >= 1.
 * Retrieves the shadow if there is one. If @memcg_id is not NULL, also
 * checks if all slots belong to the same cgroup and return the cgroup
 * private id.
 *
 * Context: Caller must lock the cluster.
 * Return: 0 if success, error code if failed.
 */
/*
 * 业务背景：大 folio 必须占用连续、同属性的一组槽位；本检查把“能否发布”
 * 与实际写表分开，供分配前后两次验证。入参：ci 为已持有锁的 cluster；
 * targ_entry/nr 指定向下对齐的 2^n 槽位范围；shadowp、memcg_id 是可空输出。
 * 出参/返回：0 时范围仍可加入并返回首槽 shadow/memcg；-EEXIST、-ENOENT
 * 或 -EBUSY 分别表示已有缓存、槽位失效或批量属性被并发路径改变。
 * 注意事项：ci->lock 同时串行 slot count、folio 指针、zero 位和 memcg ID；
 * 锁外分配会睡眠，所以分配完成后必须再次检查，第一次成功不是承诺。
 */
static int __swap_cache_add_check(struct swap_cluster_info *ci,
				  swp_entry_t targ_entry,
				  unsigned long nr, void **shadowp,
				  unsigned short *memcg_id)
{
	unsigned int ci_off, ci_end;
	unsigned long old_tb;
	bool is_zero;

	lockdep_assert_held(&ci->lock);

	/*
	 * If the target slot is not swapped out or already cached, return
	 * -ENOENT or -EEXIST. If the batch is not suitable, could be a
	 * race with concurrent free or cache add, return -EBUSY.
	 */
	/* 先检查首槽并采集可能复用的 shadow/memcg 元数据。 */
	if (unlikely(!ci->table))
		return -ENOENT;
	ci_off = swp_cluster_offset(targ_entry);
	old_tb = __swap_table_get(ci, ci_off);
	if (swp_tb_is_folio(old_tb))
		return -EEXIST;
	if (!__swp_tb_get_count(old_tb))
		return -ENOENT;
	/* shadow 与 memcg 是替换 cache 时要继承的 slot 元数据，不属于新 folio。 */
	if (shadowp && swp_tb_is_shadow(old_tb))
		*shadowp = swp_tb_to_shadow(old_tb);
	if (memcg_id)
		*memcg_id = __swap_cgroup_get(ci, ci_off);

	/* 单页无需验证批内一致性，首槽检查已给出完整结果。 */
	if (nr == 1)
		return 0;

	/* 大 folio 的每个子槽必须共享 count/zero/memcg 不变量，不能混拼。 */
	is_zero = __swap_table_test_zero(ci, ci_off);
	/* round_down 使任意命中子槽都扩展为完整高阶 folio 所需的对齐范围。 */
	ci_off = round_down(ci_off, nr);
	ci_end = ci_off + nr;
	/* 逐槽检查期间仍持 ci->lock，因此任一不一致都代表不能原子占用该范围。 */
	do {
		/* 任一子槽已缓存、未换出、zero 位或 memcg 不同都拒绝整个原子批次。 */
		old_tb = __swap_table_get(ci, ci_off);
		if (unlikely(swp_tb_is_folio(old_tb) ||
			     !__swp_tb_get_count(old_tb) ||
			     is_zero != __swap_table_test_zero(ci, ci_off) ||
			     (memcg_id && *memcg_id != __swap_cgroup_get(ci, ci_off))))
			return -EBUSY;
	} while (++ci_off < ci_end);

	/* 返回 0 后调用者才可在同一 cluster 锁域内把整段槽位发布给一个 folio。 */
	return 0;
}

/*
 * 业务背景：这是已验证范围的最小发布原语，供 add 包装和分配提交复用。
 * 入参：ci 持锁；folio 已锁且 swapbacked；entry 是连续槽位首项。
 * 出参/返回：无；逐槽发布 PFN、增加每页 cache 引用并写入 folio->swap。
 * 注意事项：不得睡眠；调用者负责范围验证与统计，发布后查找者可竞争取引用。
 */
static void __swap_cache_do_add_folio(struct swap_cluster_info *ci,
				      struct folio *folio, swp_entry_t entry)
{
	/* 这个内部函数只覆盖一个短直线发布阶段；三个 WARN 后才允许写 slot 表。 */
	/* ci_off 从 entry 首槽开始，ci_end 是开区间；循环不会越过 folio 覆盖范围。 */
	/* 仅由已验证的外层调用；这里不重查 count，避免锁内重复协议。 */
	/* ci、folio、entry 的关联已由 __swap_cache_add_check 与调用者锁条件建立。 */
	unsigned int ci_off = swp_cluster_offset(entry), ci_end;
	unsigned long nr_pages = folio_nr_pages(folio);
	unsigned long pfn = folio_pfn(folio);
	unsigned long old_tb;

	/* 此 helper 不分配也不睡眠；调用者持有的 cluster 锁覆盖整个 slot 写循环。 */
	/* 变量地图：ci_off/ci_end 遍历每个 base-page 槽；pfn 是同一 compound
	 * folio 的首 PFN；old_tb 的 flags 必须随新指针保留。
	 */
	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(folio_test_swapcache(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapbacked(folio), folio);

	/* 在持有 cluster 锁的发布窗口，把每个子槽都编码到同一 folio PFN。 */
	ci_end = ci_off + nr_pages;
	do {
		old_tb = __swap_table_get(ci, ci_off);
		VM_WARN_ON_ONCE(swp_tb_is_folio(old_tb));
		__swap_table_set(ci, ci_off, pfn_to_swp_tb(pfn, __swp_tb_get_flags(old_tb)));
	} while (++ci_off < ci_end);

	/* 每槽各持一引用；删除时必须以相同页数归还，避免大 folio 提前释放。 */
	folio_ref_add(folio, nr_pages);
	folio_set_swapcache(folio);
	folio->swap = entry;
}

/**
 * __swap_cache_add_folio - Add a folio to the swap cache and update stats.
 * @ci: The locked swap cluster.
 * @folio: The folio to be added.
 * @entry: The swap entry corresponding to the folio.
 *
 * Unconditionally add a folio to the swap cache. The caller must ensure
 * all slots are usable and have no conflicts. This assigns entry to
 * @folio->swap, increases folio refcount by the number of pages, and
 * updates swap cache stats.
 *
 * Context: Caller must ensure the folio is locked and lock the cluster
 * that holds the entries.
 */
/*
 * 业务背景：已准备好的 swap-in folio 在 I/O 前必须先作为唯一缓存对象发布。
 * 入参：ci 与 folio 均已锁定；entry 是该 folio 首槽且覆盖连续 nr_pages。
 * 出参/返回：无直接返回值；建立 swapcache 标志、slot→PFN 映射和统计。
 * 注意事项：这是发布点，调用者须先验证所有槽位；函数不分配、不睡眠，锁
 * 外的查找者只能在随后成功取得 folio 引用后使用它。
 */
void __swap_cache_add_folio(struct swap_cluster_info *ci,
			    struct folio *folio, swp_entry_t entry)
{
	/* nr_pages 是 folio order 的基页数，两个统计都必须使用同一计量单位。 */
	unsigned long nr_pages = folio_nr_pages(folio);

	/* 低层 helper 发布映射；两项统计随后让 VM 回收准确看到此驻留页。 */
	__swap_cache_do_add_folio(ci, folio, entry);
	node_stat_mod_folio(folio, NR_FILE_PAGES, nr_pages);
	lruvec_stat_mod_folio(folio, NR_SWAPCACHE, nr_pages);
}

/*
 * 业务背景：作为 add 的逆操作，撤销 slot→folio 映射并按剩余 swap count 归还槽。
 * 入参：ci/folio 已锁，entry 为首槽，shadow 可空且仅保存 workingset 历史。
 * 出参/返回：无；清除 folio cache 状态，不扣 cache 引用，外层负责该配对。
 * 注意事项：folio 不可 writeback；锁保证并发 lookup 不会看到半段拆除。
 */
static void __swap_cache_do_del_folio(struct swap_cluster_info *ci,
				      struct folio *folio,
				      swp_entry_t entry, void *shadow)
{
	/* 删除内部 helper 与 add 对称：表、标志、空闲槽三类状态必须一致转移。 */
	/* shadow 可为 NULL，仍编码为空 shadow；它不是释放 folio 的替代引用。 */
	unsigned long old_tb;
	struct swap_info_struct *si;
	unsigned int ci_start, ci_off, ci_end;
	bool folio_swapped = false, need_free = false;
	unsigned long nr_pages = folio_nr_pages(folio);

	/* 变量地图：folio_swapped 表示至少一个子槽仍有使用者；need_free 表示
	 * 混合状态需逐槽归还。si 是设备/空闲 bitmap 的所有者。
	 */
	VM_WARN_ON_ONCE(__swap_entry_to_cluster(entry) != ci);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapcache(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(folio_test_writeback(folio), folio);

	/* 先把表项替换为 shadow，再清本地标志，阻止新查找者取得旧 folio。 */
	si = __swap_entry_to_info(entry);
	ci_start = swp_cluster_offset(entry);
	ci_end = ci_start + nr_pages;
	ci_off = ci_start;
	/* 每个旧 slot 都必须确实仍指向此 folio；否则调用者的锁/entry 协议已坏。 */
	do {
		/* count 决定 slot 还能否保留；shadow 只保留 refault 历史而不保数据。 */
		old_tb = __swap_table_get(ci, ci_off);
		WARN_ON_ONCE(!swp_tb_is_folio(old_tb) ||
			     swp_tb_to_folio(old_tb) != folio);
		if (__swp_tb_get_count(old_tb))
			folio_swapped = true;
		else
			need_free = true;
		/* If shadow is NULL, we set an empty shadow. */
		__swap_table_set(ci, ci_off, shadow_to_swp_tb(shadow,
				 __swp_tb_get_flags(old_tb)));
	} while (++ci_off < ci_end);

	/* 本地 swap 值与 PG_swapcache 一起清除，之后 folio 不再代表任何槽位。 */
	folio->swap.val = 0;
	folio_clear_swapcache(folio);

	/* 无剩余 swap count 时整段归还；部分仍被换出的子槽只能逐个归还。 */
	/* 分支汇合后不再持有任何对该 folio 的 slot→PFN 发布。 */
	if (!folio_swapped) {
		__swap_cluster_free_entries(si, ci, ci_start, nr_pages);
	} else if (need_free) {
		ci_off = ci_start;
		do {
			if (!__swp_tb_get_count(__swap_table_get(ci, ci_off)))
				__swap_cluster_free_entries(si, ci, ci_off, 1);
		} while (++ci_off < ci_end);
	}
}

/**
 * __swap_cache_del_folio - Removes a folio from the swap cache.
 * @ci: The locked swap cluster.
 * @folio: The folio.
 * @entry: The first swap entry that the folio corresponds to.
 * @shadow: shadow value to be filled in the swap cache.
 *
 * Removes a folio from the swap cache and fills a shadow in place.
 * This won't put the folio's refcount. The caller has to do that.
 *
 * Context: Caller must ensure the folio is locked and in the swap cache
 * using the index of @entry, and lock the cluster that holds the entries.
 */
/*
 * 业务背景：回收、换出完成或替换路径要撤销 slot→folio 发布，同时保留
 * workingset shadow。入参：ci/folio/entry 已锁定且 entry 为首槽；shadow
 * 是可空的编码统计信息。出参/返回：无；清除 cache 标记并调整 VM 统计，
 * 但故意不 put 引用，调用者仍控制 folio 生命周期。
 * 注意事项：folio 不得 writeback；否则删除会让未落盘的数据失去唯一 swap
 * 副本。cluster 锁与 folio 锁共同防止 slot 重用和并发状态切换。
 */
void __swap_cache_del_folio(struct swap_cluster_info *ci, struct folio *folio,
			    swp_entry_t entry, void *shadow)
{
	unsigned long nr_pages = folio_nr_pages(folio);

	/* 先撤销可见映射，随后同步 node/lruvec 统计；引用仍留给外层回收者。 */
	__swap_cache_do_del_folio(ci, folio, entry, shadow);
	node_stat_mod_folio(folio, NR_FILE_PAGES, -nr_pages);
	lruvec_stat_mod_folio(folio, NR_SWAPCACHE, -nr_pages);
}

/**
 * swap_cache_del_folio - Removes a folio from the swap cache.
 * @folio: The folio.
 *
 * Same as __swap_cache_del_folio, but handles lock and refcount. The
 * caller must ensure the folio is either clean or has a swap count
 * equal to zero, or it may cause data loss.
 *
 * Context: Caller must ensure the folio is locked and in the swap cache.
 */
/*
 * 业务背景：这是供通用回收路径调用的完整删除包装，统一取得正确 cluster 锁。
 * 入参：folio 为调用者持有、已锁且仍在 swapcache 的对象。出参/返回：无；
 * 删除映射后扣除每页 cache 引用，最后一个引用可能使 folio 进入释放。
 * 注意事项：调用者必须已保证 folio 干净或没有 swap 用户；否则先删 cache
 * 会丢失唯一数据副本。不能在未锁 folio 时读取 folio->swap。
 */
void swap_cache_del_folio(struct folio *folio)
{
	struct swap_cluster_info *ci;
	swp_entry_t entry = folio->swap;

	/* 从 folio->swap 反查所属 cluster；锁覆盖读取 entry 到完成删除的整个窗口。 */
	ci = swap_cluster_lock(__swap_entry_to_info(entry), swp_offset(entry));
	__swap_cache_del_folio(ci, folio, entry, NULL);
	swap_cluster_unlock(ci);

	/* 与 add 时每页 ref_add 配对；cluster 已解锁，之后无需再访问 slot 表。 */
	folio_ref_sub(folio, folio_nr_pages(folio));
}

/**
 * __swap_cache_replace_folio - Replace a folio in the swap cache.
 * @ci: The locked swap cluster.
 * @old: The old folio to be replaced.
 * @new: The new folio.
 *
 * Replace an existing folio in the swap cache with a new folio. The
 * caller is responsible for setting up the new folio's flag and swap
 * entries. Replacement will take the new folio's swap entry value as
 * the starting offset to override all slots covered by the new folio.
 *
 * Context: Caller must ensure both folios are locked, and lock the
 * cluster that holds the old folio to be replaced.
 */
/*
 * 业务背景：迁移、拆分/合并需要保持 swap 槽位连续可查，但把缓存实体换成
 * 新 folio。入参：ci、old、new 均已稳定；new->swap 指定目标首槽。
 * 出参/返回：无；所有覆盖槽改指向 new，引用和标志由调用者预先配对。
 * 注意事项：只替换表指针而不改变 slot count；DEBUG_VM 下额外检查部分替换
 * 没有覆盖仍归 old 的槽，避免大 folio 拆分时破坏映射。
 */
void __swap_cache_replace_folio(struct swap_cluster_info *ci,
				struct folio *old, struct folio *new)
{
	swp_entry_t entry = new->swap;
	unsigned long nr_pages = folio_nr_pages(new);
	unsigned int ci_off = swp_cluster_offset(entry);
	unsigned int ci_end = ci_off + nr_pages;
	unsigned long pfn = folio_pfn(new);
	unsigned long old_tb;

	/* old/new 都已是 cache folio；这里只改变查询可见的 PFN，不获取引用。 */
	VM_WARN_ON_ONCE(!folio_test_swapcache(old) || !folio_test_swapcache(new));
	VM_WARN_ON_ONCE(!folio_test_locked(old) || !folio_test_locked(new));
	VM_WARN_ON_ONCE(!entry.val);

	/* Swap cache still stores N entries instead of a high-order entry */
	/* swap table 以 base-page 粒度存储，即使 new 是高阶 folio 也逐槽覆写。 */
	do {
		old_tb = __swap_table_get(ci, ci_off);
		WARN_ON_ONCE(!swp_tb_is_folio(old_tb) || swp_tb_to_folio(old_tb) != old);
		__swap_table_set(ci, ci_off, pfn_to_swp_tb(pfn, __swp_tb_get_flags(old_tb)));
	} while (++ci_off < ci_end);

	/*
	 * If the old folio is partially replaced (e.g., splitting a large
	 * folio, the old folio is shrunk, and new split sub folios replace
	 * the shrunk part), ensure the new folio doesn't overlap it.
	 */
	/* 拆分时只覆写被新子 folio 接管的范围，剩余 old 槽仍必须保持指向 old。 */
	if (IS_ENABLED(CONFIG_DEBUG_VM) &&
	    /* 该检查仅调试配置执行，不参与正式迁移/拆分的返回语义。 */
	    folio_order(old) != folio_order(new)) {
		/* old 范围被缩短时逐项核验未接管的槽仍然可反解到 old。 */
		ci_off = swp_cluster_offset(old->swap);
		ci_end = ci_off + folio_nr_pages(old);
		while (ci_off++ < ci_end)
			/* WARN 只验证映射完整性，不在损坏时尝试修复或改变释放责任。 */
			WARN_ON_ONCE(swp_tb_to_folio(__swap_table_get(ci, ci_off)) != old);
	}
}

/*
 * Try to allocate a folio of given order in the swap cache.
 *
 * This helper resolves the potential races of swap allocation
 * and prepares a folio to be used for swap IO. May return following
 * value:
 *
 * -ENOMEM / -EBUSY: Order is too large or in conflict with sub slot,
 *                   caller should shrink the order and retry
 * -ENOENT / -EEXIST: Target swap entry is unavailable or cached, the caller
 *                    should abort or try to use the cached folio instead
 */
/*
 * 业务背景：把“锁下验证—锁外分配—锁下发布—memcg 回滚”封装为单次候选 order。
 * 入参：ci 是目标锁域；targ_entry/gfp/order 描述槽与分配；vmf/mpol/ilx 可选。
 * 出参/返回：成功返回锁定、已入 cache 的 folio；错误指针说明冲突、失效或 OOM。
 * 注意事项：可睡眠；第一次检查不能替代发布前重检，失败必须撤销 slot 引用。
 */
static struct folio *__swap_cache_alloc(struct swap_cluster_info *ci,
					swp_entry_t targ_entry, gfp_t gfp,
					unsigned int order, struct vm_fault *vmf,
					struct mempolicy *mpol, pgoff_t ilx)
{
	/* 分配流程的失败出口均在发布后撤销每槽引用，成功出口保留 allocation 引用。 */
	/* vmf 可空区分 fault 与非 fault 调用，vma 因而仅在 vmf 存在时可访问。 */
	int err;
	swp_entry_t entry;
	struct folio *folio;
	void *shadow = NULL;
	unsigned short memcg_id;
	unsigned long address, nr_pages = 1UL << order;
	struct vm_area_struct *vma = vmf ? vmf->vma : NULL;

	/* 变量地图：shadow 用于 refault 统计；memcg_id 必须对整批槽一致；folio
	 * 在成功返回时仍锁定，所有权由调用者取得并负责发起 swap I/O。
	 */
	VM_WARN_ON_ONCE(nr_pages > SWAPFILE_CLUSTER);
	entry.val = round_down(targ_entry.val, nr_pages);

	/* Check if the slot and range are available, skip allocation if not */
	/* 第一次锁下预检避免为明显冲突的槽位进行可能睡眠的页分配。 */
	spin_lock(&ci->lock);
	err = __swap_cache_add_check(ci, targ_entry, nr_pages, NULL, NULL);
	spin_unlock(&ci->lock);
	if (unlikely(err))
		return ERR_PTR(err);

	/*
	 * Limit THP gfp. The limitation is a no-op for typical
	 * GFP_HIGHUSER_MOVABLE but matters for shmem.
	 */
	if (order)
		gfp = thp_shmem_limit_gfp_mask(vma_thp_gfp_mask(vma), gfp);

	/* 无 fault 上下文时按显式策略/当前节点分配；否则 VMA helper 继承策略。 */
	if (mpol || !vmf) {
		folio = folio_alloc_mpol(gfp, order, mpol, ilx, numa_node_id());
	} else {
		address = round_down(vmf->address, PAGE_SIZE << order);
		folio = vma_alloc_folio(gfp, order, vmf->vma, address);
	}
	if (unlikely(!folio))
		return ERR_PTR(-ENOMEM);

	/* Double check the range is still not in conflict */
	/* 分配期间 slot 可能被 swapoff/其他 fault 改变，故重进锁做提交前验证。 */
	spin_lock(&ci->lock);
	err = __swap_cache_add_check(ci, targ_entry, nr_pages, &shadow, &memcg_id);
	if (unlikely(err)) {
		spin_unlock(&ci->lock);
		folio_put(folio);
		return ERR_PTR(err);
	}

	/* 先把 folio 标记为 locked/swapbacked 再发布 slot，查找者会等待 I/O 完成。 */
	__folio_set_locked(folio);
	__folio_set_swapbacked(folio);
	__swap_cache_do_add_folio(ci, folio, entry);
	spin_unlock(&ci->lock);

	/* 发布后 memcg charge 若失败，按相反顺序摘除映射、解锁并归还所有引用。 */
	if (mem_cgroup_swapin_charge_folio(folio, memcg_id,
					   vmf ? vmf->vma->vm_mm : NULL, gfp)) {
		spin_lock(&ci->lock);
		__swap_cache_do_del_folio(ci, folio, entry, shadow);
		spin_unlock(&ci->lock);
		folio_unlock(folio);
		/* nr_pages refs from swap cache, 1 from allocation */
		folio_put_refs(folio, nr_pages + 1);
		count_mthp_stat(order, MTHP_STAT_SWPIN_FALLBACK_CHARGE);
		return ERR_PTR(-ENOMEM);
	}

	/* 延迟 memcg 分配对多页 folio 也不可接受；走同一摘除回滚，保持表干净。 */
	if (order > 1 && folio_memcg_alloc_deferred(folio)) {
		spin_lock(&ci->lock);
		__swap_cache_do_del_folio(ci, folio, entry, shadow);
		spin_unlock(&ci->lock);
		folio_unlock(folio);
		/* nr_pages refs from swap cache, 1 from allocation */
		folio_put_refs(folio, nr_pages + 1);
		return ERR_PTR(-ENOMEM);
	}

	/* memsw uncharges swap when folio is added to swap cache */
	/* cache 引用现在承载驻留页，cgroup v1 的 swap 计费相应转为内存计费。 */
	memcg1_swapin(folio);
	if (shadow)
		workingset_refault(folio, shadow);

	node_stat_mod_folio(folio, NR_FILE_PAGES, nr_pages);
	lruvec_stat_mod_folio(folio, NR_SWAPCACHE, nr_pages);

	/* Caller will initiate read into locked new_folio */
	/* 加入 LRU 让完成后的 folio 可被正常回收；锁仍是 I/O 完成前的可见性门。 */
	folio_add_lru(folio);
	return folio;
}

/**
 * swap_cache_alloc_folio - Allocate folio for swapped out slot in swap cache.
 * @targ_entry: swap entry indicating the target slot
 * @gfp: memory allocation flags
 * @orders: allocation orders, must be non zero
 * @vmf: fault information
 * @mpol: NUMA memory allocation policy to be applied
 * @ilx: NUMA interleave index, for use only when MPOL_INTERLEAVE
 *
 * Allocate a folio in the swap cache for one swap slot, typically before
 * doing IO (e.g. swap in or zswap writeback). The swap slot indicated by
 * @targ_entry must have a non-zero swap count (swapped out).
 *
 * Context: Caller must protect the swap device with reference count or locks.
 * Return: Returns the folio if allocation succeeded and folio is in the swap
 * cache. Returns error code if failed due to race, OOM or invalid arguments.
 */
/*
 * 业务背景：swap-in/zswap 回写以候选 order 集合请求 cache folio；这里把大
 * folio 优化降级为较小 order，而不让一个冲突子槽阻断缺页。入参：targ_entry
 * 受设备引用保护；gfp/vmf/mpol/ilx 决定分配位置；orders 是非零 bitset。
 * 出参/返回：成功返回锁定且已发布的 folio；-EINVAL 为参数非法，-EEXIST
 * 可提示调用者重查，-ENOENT/-ENOMEM/-EBUSY 保留相应的设备/资源语义。
 * 注意事项：仅 -EBUSY/-ENOMEM 允许缩阶重试；其他错误若被吞掉会把 swapoff
 * 或已有 cache 误当内存压力。此函数可分配和 memcg charge，可能睡眠。
 */
struct folio *swap_cache_alloc_folio(swp_entry_t targ_entry, gfp_t gfp,
				     unsigned long orders, struct vm_fault *vmf,
				     struct mempolicy *mpol, pgoff_t ilx)
{
	int order, err;
	struct folio *ret;
	struct swap_cluster_info *ci;

	/* 先选 cluster 与最高候选 order；最低位失败时 next_order 只保留未尝试候选。 */
	ci = __swap_entry_to_cluster(targ_entry);
	order = highest_order(orders);

	/* orders must be non-zero, and must not exceed cluster size. */
	if (WARN_ON_ONCE(!orders || (1UL << order) > SWAPFILE_CLUSTER))
		return ERR_PTR(-EINVAL);

	/* 缩阶只针对资源/并发冲突；命中或设备无效必须原样交给调用者处理。 */
	do {
		/* ret 成功即为唯一 cache 实体；失败则由 errno 决定是否继续降低 order。 */
		ret = __swap_cache_alloc(ci, targ_entry, gfp, order,
					 vmf, mpol, ilx);
		if (!IS_ERR(ret))
			break;
		/* 只读取错误指针的 errno，绝不把它当 folio 解引用。 */
		err = PTR_ERR(ret);
		if (!order || (err && err != -EBUSY && err != -ENOMEM))
			break;
		count_mthp_stat(order, MTHP_STAT_SWPIN_FALLBACK);
		order = next_order(&orders, order);
	} while (orders);

	return ret;
}

/*
 * If we are the only user, then try to free up the swap cache.
 *
 * Its ok to check the swapcache flag without the folio lock
 * here because we are going to recheck again inside
 * folio_free_swap() _with_ the lock.
 * 					- Marcelo
 */
/*
 * 业务背景：最后一个映射用户释放页时尽量同时回收无用 swap cache。
 * 入参：folio 为调用者即将释放的一份引用。出参/返回：无；若能非阻塞取得
 * folio 锁，folio_free_swap() 会在锁下重检并删除无用 cache。
 * 注意事项：锁前的 flag/mapped 观察允许过期；trylock 失败宁可延后回收，
 * 不能等待，否则 page release 路径可能造成不必要的锁竞争或递归。
 */
void free_swap_cache(struct folio *folio)
{
	/* 映射存在即使只有一个用户也不能释放 swap 副本，避免随后 fault 失去数据源。 */
	/* trylock 成功后 folio_free_swap 会复查 mapped/swapcache，锁前判断只是筛选。 */
	if (folio_test_swapcache(folio) && !folio_mapped(folio) &&
	    folio_trylock(folio)) {
		folio_free_swap(folio);
		folio_unlock(folio);
	}
}

/*
 * Freeing a folio and also freeing any swap cache associated with
 * this folio if it is the last user.
 */
/*
 * 业务背景：批量回收的单 folio 包装，先尝试解除 swap cache 再放调用者引用。
 * 入参：folio 是一份待归还的持有引用。出参/返回：无；huge zero folio 是
 * 全局永生对象，不能 put，其他 folio 最后引用可进入释放。
 * 注意事项：free_swap_cache() 不保证成功，因并发映射或锁竞争可保留 cache；
 * 正确性依赖其后续回收者，而非此函数的即时效果。
 */
void free_folio_and_swap_cache(struct folio *folio)
{
	/* huge zero folio 没有普通引用生命周期；它是全局共享读零页。 */
	/* 普通 folio 的 put 发生在 cache 尝试之后，避免最后引用先消失。 */
	free_swap_cache(folio);
	if (!is_huge_zero_folio(folio))
		folio_put(folio);
}

/*
 * Passed an array of pages, drop them all from swapcache and then release
 * them.  They are removed from the LRU and freed if this is their last use.
 */
/*
 * 业务背景：I/O 完成或 fault 错误路径常持有 encoded_page 数组；按 folio batch
 * 放引用可减少 LRU 锁操作。入参：pages 为借用数组，nr 为元素数，NEXT 标记
 * 把下一元素解释为大 folio 页数。出参/返回：无；每个记录最终放回对应引用。
 * 注意事项：先逐 folio 尝试 free cache，再合并 put；NEXT 会消耗额外数组项，
 * 调用者必须保证编码完整，不能把普通指针数组误传入。
 */
void free_pages_and_swap_cache(struct encoded_page **pages, int nr)
{
	struct folio_batch folios;
	unsigned int refs[FOLIO_BATCH_SIZE];

	/* batch 内 refs 与 folio 索引同步；满批立即放，尾批在循环后统一清空。 */
	folio_batch_init(&folios);
	for (int i = 0; i < nr; i++) {
		struct folio *folio = page_folio(encoded_page_ptr(pages[i]));

		/* encoded NEXT 记录把大 folio 的全部引用数放在紧随元素中。 */
		free_swap_cache(folio);
		refs[folios.nr] = 1;
		if (unlikely(encoded_page_flags(pages[i]) &
			     ENCODED_PAGE_BIT_NR_PAGES_NEXT))
			refs[folios.nr] = encoded_nr_pages(pages[++i]);

		if (folio_batch_add(&folios, folio) == 0)
			folios_put_refs(&folios, refs);
	}
	/* 未满的一批也必须归还，否则调用者的页引用会泄漏。 */
	if (folios.nr)
		folios_put_refs(&folios, refs);
}

static inline bool swap_use_vma_readahead(void)
{
	/* swap rotation 改变槽位顺序，物理/VMA 历史都不可靠，故关闭 VMA 启发式。 */
	return READ_ONCE(enable_vma_readahead) && !atomic_read(&nr_rotate_swap);
}

/**
 * swap_update_readahead - Update the readahead statistics of VMA or globally.
 * @folio: the swap cache folio that just got hit.
 * @vma: the VMA that should be updated, could be NULL for global update.
 * @addr: the addr that triggered the swapin, ignored if @vma is NULL.
 */
/*
 * 业务背景：真正使用了预读 folio 时反馈命中，以便下次 fault 扩大或缩小窗口。
 * 入参：folio 是已命中的 cache folio；vma 可空，addr 是该 VMA 的 fault 地址。
 * 出参/返回：无；消费 PG_readahead，一次命中最多计一次并更新 VMA 或全局统计。
 * 注意事项：匿名 THP 尚无等价 readahead 标记，必须跳过以免把非预读页误算；
 * 原子读改写允许并发 fault 丢失部分启发式信息，但不影响页表或数据正确性。
 */
void swap_update_readahead(struct folio *folio, struct vm_area_struct *vma,
			   unsigned long addr)
{
	/* readahead 是被消费的页标记；vma_ra 固化本次选择，避免中途开关切换。 */
	bool readahead, vma_ra = swap_use_vma_readahead();

	/*
	 * At the moment, we don't support PG_readahead for anon THP
	 * so let's bail out rather than confusing the readahead stat.
	 */
	if (unlikely(folio_test_large(folio)))
		return;

	/* clear 是一次性消费：同一预读页被重复访问不能重复扩张窗口。 */
	readahead = folio_test_clear_readahead(folio);
	/* 标志清除与统计更新同次调用完成，避免两个并发消费者重复报告 hit。 */
	/* VMA 模式把命中写入所属 VMA；关闭/无 VMA 时退回全局计数。 */
	if (vma && vma_ra) {
		/* 将本次 fault 地址作为下一次相邻性判断的基准，同时保留命中饱和值。 */
		unsigned long ra_val;
		int win, hits;

		ra_val = GET_SWAP_RA_VAL(vma);
		win = SWAP_RA_WIN(ra_val);
		hits = SWAP_RA_HITS(ra_val);
		if (readahead)
			hits = min_t(int, hits + 1, SWAP_RA_HITS_MAX);
		atomic_long_set(&vma->swap_readahead_info,
				SWAP_RA_VAL(addr, win, hits));
	}

	/* VM event 用于观测；全局 atomic 是下一个 cluster 窗口的反馈输入。 */
	/* 没有 VMA 历史时，统计保存在全局 atomic，由 cluster 算法消费。 */
	if (readahead) {
		count_vm_event(SWAP_RA_HIT);
		if (!vma || !vma_ra)
			atomic_inc(&swapin_readahead_hits);
	}
}

/*
 * 业务背景：预读和单读共用的“命中或建 cache 并发 I/O”单槽原语。
 * 入参：entry 已有设备生命周期保护；mpol/ilx 仅决定新页；plug/readahead 可选。
 * 出参/返回：返回带引用 folio 或 NULL；新页 I/O 已提交且保持锁定。
 * 注意事项：-EEXIST 代表别人发布，必须重查；调用者处理返回 folio 的锁等待。
 */
static struct folio *swap_cache_read_folio(swp_entry_t entry, gfp_t gfp,
					   struct mempolicy *mpol, pgoff_t ilx,
					   struct swap_iocb **plug, bool readahead)
{
	/* plug 可空：预读批次传入聚合器，目标单读直接提交。 */
	struct folio *folio;

	/* 命中者复用已在途 I/O；-EEXIST 则表示竞争者刚完成发布，回到查找。 */
	/* 目标槽若被他人刚加入 cache，-EEXIST 后重查以复用对方的 I/O。 */
	do {
		folio = swap_cache_get_folio(entry);
		if (folio)
			return folio;
		folio = swap_cache_alloc_folio(entry, gfp, BIT(0), NULL, mpol, ilx);
	} while (PTR_ERR(folio) == -EEXIST);

	/* 内部错误不会传播给预读调用者；它只把此邻页当作未命中处理。 */
	if (IS_ERR_OR_NULL(folio))
		return NULL;

	/* 新 folio 保持锁定交给 I/O；完成路径才解锁，避免 fault 读到半页数据。 */
	swap_read_folio(folio, plug);
	/* 标记只加在非目标预取页，后续真正被消费时才记命中。 */
	if (readahead) {
		folio_set_readahead(folio);
		count_vm_event(SWAP_RA);
	}

	return folio;
}

/**
 * swapin_sync - swap-in one or multiple entries skipping readahead.
 * @entry: swap entry indicating the target slot
 * @gfp: memory allocation flags
 * @orders: allocation orders
 * @vmf: fault information
 * @mpol: NUMA memory allocation policy to be applied
 * @ilx: NUMA interleave index, for use only when MPOL_INTERLEAVE
 *
 * This allocates a folio suitable for given @orders, or returns the
 * existing folio in the swap cache for @entry. This initiates the IO, too,
 * if needed. @entry is rounded down if @orders allow large allocation.
 *
 * Context: Caller must ensure @entry is valid and pin the swap device with refcount.
 * Return: Returns the folio on success, error code if failed.
 */
/*
 * 业务背景：同步换入路径不做邻页预测，服务要求当前 entry 的缺页/直接消费。
 * 入参：entry 已 pin；orders 给允许的大页 order；vmf/mpol/ilx 选择 NUMA 放置。
 * 出参/返回：命中时返回加引用 folio；新建时已提交读 I/O；错误指针保留分配、
 * 设备失效或冲突原因。注意事项：-EEXIST 是发布竞态而非失败，必须重查；
 * 返回的新 folio 仍会等待 I/O 解锁，调用者遵循 folio 锁协议。
 */
struct folio *swapin_sync(swp_entry_t entry, gfp_t gfp, unsigned long orders,
			   struct vm_fault *vmf, struct mempolicy *mpol, pgoff_t ilx)
{
	struct folio *folio;

	/* 与异步 helper 相同的“查找—竞争发布—重查”协议，但保留 errno。 */
	do {
		folio = swap_cache_get_folio(entry);
		if (folio)
			return folio;
		folio = swap_cache_alloc_folio(entry, gfp, orders, vmf, mpol, ilx);
	} while (PTR_ERR(folio) == -EEXIST);

	/* 已存在的 folio 不重发 I/O；新建成功者才在此启动同步读取。 */
	if (IS_ERR(folio))
		return folio;

	swap_read_folio(folio, NULL);
	return folio;
}

/*
 * Locate a page of swap in physical memory, reserving swap cache space
 * and reading the disk if it is not already cached.
 * A failure return means that either the page allocation failed or that
 * the swap entry is no longer in use.
 */
/*
 * 业务背景：旧调用者在没有 vm_fault 的场景异步取得/发起单页 swap-in。
 * 入参：entry 只是槽位值，本函数先 get_swap_device() 取得稳定设备引用；
 * vma/addr 可为策略选择提供上下文，plug 可累计 block I/O 请求。
 * 出参/返回：返回带引用 folio 或 NULL；函数在返回前归还 device/mempolicy，
 * 因而调用者只能依赖 folio 自己的引用。注意事项：设备可能正 swapoff，
 * get 失败不是 OOM；不得在未持有 device 引用时直接访问其策略字段。
 */
struct folio *read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
		struct vm_area_struct *vma, unsigned long addr,
		struct swap_iocb **plug)
{
	struct swap_info_struct *si;
	struct mempolicy *mpol;
	pgoff_t ilx;
	struct folio *folio;

	/* device 引用跨越策略选择和 I/O 提交，阻止 swapoff 在中间销毁元数据。 */
	si = get_swap_device(entry);
	if (!si)
		return NULL;

	/* policy 是可空临时引用；无 VMA 仍允许 helper 用默认节点分配。 */
	mpol = get_vma_policy(vma, addr, 0, &ilx);
	folio = swap_cache_read_folio(entry, gfp_mask, mpol, ilx, plug, false);
	mpol_cond_put(mpol);

	put_swap_device(si);
	return folio;
}

/*
 * 业务背景：把命中、相邻性和上次窗口压缩为下一次预读页数。
 * 入参：各值均是启发式快照；max_pages 为硬上限。出参/返回：返回至少一页。
 * 注意事项：不访问共享对象；结果影响性能而非数据正确性，允许并发历史近似。
 */
static unsigned int __swapin_nr_pages(unsigned long prev_offset,
				      unsigned long offset,
				      int hits,
				      int max_pages,
				      int prev_win)
{
	/* 返回页数始终不超过 max_pages，调用者因此可安全转换成 cluster 掩码。 */
	/* prev_offset/prev_win 是上次观测，hits 是已消费的预读页数，均只作预测。 */
	unsigned int pages, last_ra;

	/*
	 * This heuristic has been found to work well on both sequential and
	 * random loads, swapping to hard disk or to SSD: please don't ask
	 * what the "+ 2" means, it just happens to work well, that's all.
	 */
	/* 命中数是上一窗口的需求信号；+2 是历史调优常量，不是正确性边界。 */
	pages = hits + 2;
	/* 无命中时用相邻 offset 判断是否仍存在顺序访问，避免永远固定两页。 */
	if (pages == 2) {
		/*
		 * We can have no readahead hits to judge by: but must not get
		 * stuck here forever, so check for an adjacent offset instead
		 * (and don't even bother to check whether swap type is same).
		 */
		if (offset != prev_offset + 1 && offset != prev_offset - 1)
			/* 非相邻的零命中访问退化为单页，避免无根据的 I/O 放大。 */
			pages = 1;
	} else {
		/* 大于基础窗口时向上取 2 的幂，便于 cluster/掩码边界计算。 */
		unsigned int roundup = 4;
		while (roundup < pages)
			roundup <<= 1;
		pages = roundup;
	}

	/* 上限来自 sysctl/调用者，随后再限制收缩速度以避免窗口震荡。 */
	if (pages > max_pages)
		pages = max_pages;
	/* prev_win 的半值是平滑下界，防止偶发 miss 让预读立即收缩为单页。 */

	/* Don't shrink readahead too fast */
	last_ra = prev_win / 2;
	if (pages < last_ra)
		pages = last_ra;

	return pages;
}

/*
 * 业务背景：维护 cluster 预读的全局历史，供没有 VMA 预测的 fault 使用。
 * 入参：offset 是当前槽位偏移。出参/返回：返回经 page_cluster 限制的窗口页数。
 * 注意事项：静态状态仅 atomic/READ_ONCE 协调，允许不同 CPU 的反馈合并。
 */
static unsigned long swapin_nr_pages(unsigned long offset)
{
	/* 每次交换全局 hit 计数，因此并发 fault 只共享趋势而非逐次精确归因。 */
	static unsigned long prev_offset;
	unsigned int hits, pages, max_pages;
	static atomic_t last_readahead_pages;

	/* 静态 prev_offset/last_readahead_pages 是全局启发式；READ_ONCE/atomic
	 * 只保证并发更新不会撕裂，不要求每个 fault 都得到精确历史。
	 */
	max_pages = 1 << READ_ONCE(page_cluster);
	if (max_pages <= 1)
		return 1;

	/* xchg 消费上一轮反馈；零命中才推进相邻性基准 offset。 */
	hits = atomic_xchg(&swapin_readahead_hits, 0);
	pages = __swapin_nr_pages(READ_ONCE(prev_offset), offset, hits,
				  max_pages,
				  atomic_read(&last_readahead_pages));
	if (!hits)
		WRITE_ONCE(prev_offset, offset);
	atomic_set(&last_readahead_pages, pages);

	return pages;
}

/**
 * swap_cluster_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @mpol: NUMA memory allocation policy to be applied
 * @ilx: NUMA interleave index, for use only when MPOL_INTERLEAVE
 *
 * Returns the struct folio for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read an aligned block of
 * (1 << page_cluster) entries in the swap area. This method is chosen
 * because it doesn't cost us any seek time.  We also make sure to queue
 * the 'original' request together with the readahead ones...
 *
 * Note: it is intentional that the same NUMA policy and interleave index
 * are used for every page of the readahead: neighbouring pages on swap
 * are fairly likely to have been swapped out from the same node.
 */
/*
 * 业务背景：当 VMA 预测不可用时，按磁盘相邻槽位合并 I/O，利用顺序设备传输。
 * 入参：entry 是已 pin 的目标槽；gfp/mpol/ilx 选择每个缓存 folio 的位置。
 * 出参/返回：返回目标 entry 的带引用 folio 或 NULL；邻页仅预读并立即放引用。
 * 注意事项：blk plug 把多次提交聚合到一次队列操作；首槽是 swap header，
 * 不能读；si->max 是设备边界。预读失败不得影响目标页的最终单独读取。
 */
struct folio *swap_cluster_readahead(swp_entry_t entry, gfp_t gfp_mask,
				     struct mempolicy *mpol, pgoff_t ilx)
{
	struct folio *folio;
	unsigned long entry_offset = swp_offset(entry);
	/* entry_offset 保存目标槽；循环变量 offset 可移动而不得丢失目标位置。 */
	unsigned long offset = entry_offset;
	unsigned long start_offset, end_offset;
	unsigned long mask;
	struct swap_info_struct *si = __swap_entry_to_info(entry);
	struct blk_plug plug;
	struct swap_iocb *splug = NULL;
	swp_entry_t ra_entry;

	/* 阶段 1：以动态窗口构造对齐范围；窗口为 1 时直接只读目标。 */
	mask = swapin_nr_pages(offset) - 1;
	if (!mask)
		goto skip;
	/* skip 保持目标页单读语义，不会因为禁用预读而把 fault 当作成功。 */

	/* Read a page_cluster sized and aligned cluster around offset. */
	/* 掩码窗口在槽位空间对齐；header 与设备尾端要在这里裁掉。 */
	start_offset = offset & ~mask;
	end_offset = offset | mask;
	if (!start_offset)	/* First page is swap header. */
		/* header 不承载用户页；从一开始才能避免把元数据提交给 block 读取。 */
		start_offset++;
	if (end_offset >= si->max)
		end_offset = si->max - 1;

	/* 阶段 2：逐槽查询/发布 cache 并聚合 I/O；返回的临时引用立即归还。 */
	blk_start_plug(&plug);
	/* 目标槽也进入同一 plug，保证预读不会排在真正缺页之后。 */
	for (offset = start_offset; offset <= end_offset ; offset++) {
		/* 每次迭代构造同 type 的槽位；folio 返回后立即 put 只保留 cache 参考。 */
		/* Ok, do the async read-ahead now */
		ra_entry = swp_entry(swp_type(entry), offset);
		folio = swap_cache_read_folio(ra_entry, gfp_mask, mpol, ilx,
					      &splug, offset != entry_offset);
		if (!folio)
			continue;
		folio_put(folio);
	}
	/* 阶段 3：提交 plug 中的请求并排空 per-cpu LRU，之后再取目标页。 */
	blk_finish_plug(&plug);
	swap_read_unplug(splug);
	lru_add_drain();	/* Push any new pages onto the LRU now */
skip:
	/* The page was likely read above, so no need for plugging here */
	return swap_cache_read_folio(entry, gfp_mask, mpol, ilx, NULL, false);
}

/*
 * 业务背景：根据一个 VMA 内的 fault 历史产生不跨 PMD/VMA 的虚拟地址预读窗。
 * 入参：vmf 的 VMA/地址受 mmap_lock 保护；start/end 是输出字节地址。
 * 出参/返回：返回窗口页数，1 表示不预读。注意事项：原子 packed 历史可近似。
 */
static int swap_vma_ra_win(struct vm_fault *vmf, unsigned long *start,
			   unsigned long *end)
{
	/* 输出 start/end 以字节地址表示，后续 PTE 遍历按 PAGE_SIZE 递增。 */
	struct vm_area_struct *vma = vmf->vma;
	unsigned long ra_val;
	unsigned long faddr, prev_faddr, left, right;
	unsigned int max_win, hits, prev_win, win;

	/* VMA 私有 packed 值同时保存上次地址、窗口和命中数。 */
	max_win = 1 << min(READ_ONCE(page_cluster), SWAP_RA_ORDER_CEILING);
	if (max_win == 1)
		return 1;
	/* page_cluster 被压到一页时不更新 VMA packed 历史，避免无收益写共享字段。 */

	/* 读取后立即发布新历史，后续 fault 即使并发也总能看到自洽 packed 值。 */
	faddr = vmf->address;
	ra_val = GET_SWAP_RA_VAL(vma);
	prev_faddr = SWAP_RA_ADDR(ra_val);
	prev_win = SWAP_RA_WIN(ra_val);
	hits = SWAP_RA_HITS(ra_val);
	win = __swapin_nr_pages(PFN_DOWN(prev_faddr), PFN_DOWN(faddr), hits,
				max_win, prev_win);
	atomic_long_set(&vma->swap_readahead_info, SWAP_RA_VAL(faddr, win, 0));
	if (win == 1)
		return 1;

	/* 相邻顺序访问把窗口偏向前进方向；随机访问则围绕 fault 居中。 */
	if (faddr == prev_faddr + PAGE_SIZE)
		left = faddr;
	else if (prev_faddr == faddr + PAGE_SIZE)
		left = faddr - (win << PAGE_SHIFT) + PAGE_SIZE;
	else
		left = faddr - (((win - 1) / 2) << PAGE_SHIFT);
	/* 最终限制在当前 VMA 与 fault 所在 PMD，避免跨锁保护/页表层级预读。 */
	right = left + (win << PAGE_SHIFT);
	if ((long)left < 0)
		left = 0;
	*start = max3(left, vma->vm_start, faddr & PMD_MASK);
	*end = min3(right, vma->vm_end, (faddr & PMD_MASK) + PMD_SIZE);

	return win;
}

/**
 * swap_vma_readahead - swap in pages in hope we need them soon
 * @targ_entry: swap entry of the targeted memory
 * @gfp_mask: memory allocation flags
 * @mpol: NUMA memory allocation policy to be applied
 * @targ_ilx: NUMA interleave index, for use only when MPOL_INTERLEAVE
 * @vmf: fault information
 *
 * Returns the struct folio for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read in a few pages whose
 * virtual addresses are around the fault address in the same vma.
 *
 * Caller must hold read mmap_lock if vmf->vma is not NULL.
 *
 */
/*
 * 业务背景：匿名页的空间局部性通常由虚拟地址而非换出时的物理槽位表达；
 * 本路径在同一 VMA、同一 PMD 内预取相邻 swap PTE。入参：targ_entry 是
 * 当前 fault 槽；vmf 提供仍受 mmap_lock read 保护的 VMA/PMD；mpol/ilx
 * 是当前页的 NUMA 策略。出参/返回：返回目标 folio，邻页只提交读取。
 * 注意事项：lockless PTE 读取后必须先确认 swap 类型；跨设备 entry 需要另取
 * device 引用，避免 swapoff 释放设备。PTE map 必须在所有退出分支 unmap。
 */
static struct folio *swap_vma_readahead(swp_entry_t targ_entry, gfp_t gfp_mask,
		struct mempolicy *mpol, pgoff_t targ_ilx, struct vm_fault *vmf)
{
	/* pte 只在当前页表页内有效；splug 由 swap_read_unplug 在提交后统一释放。 */
	struct blk_plug plug;
	struct swap_iocb *splug = NULL;
	struct folio *folio;
	pte_t *pte = NULL, pentry;
	int win;
	unsigned long start, end, addr;
	pgoff_t ilx = targ_ilx;

	/* 阶段 1：更新 VMA 历史并裁剪到 VMA/PMD，不能越界窥探相邻映射。 */
	win = swap_vma_ra_win(vmf, &start, &end);
	if (win == 1)
		goto skip;
	/* 窗口有效才调整 ilx；目标页的原始索引 targ_ilx 供最终单读保留。 */

	/* 向左扩展的每一页需要倒退 interleave index，保证 NUMA 条带保持一致。 */
	ilx = targ_ilx - PFN_DOWN(vmf->address - start);

	/* 阶段 2：逐 PTE 读取，仅对仍为 swap 的相邻地址建立 cache/I/O。 */
	blk_start_plug(&plug);
	for (addr = start; addr < end; ilx++, addr += PAGE_SIZE) {
		struct swap_info_struct *si = NULL;
		softleaf_t entry;

		/* pte 指针按页递增；首次或跨页表页时重新建立临时映射。 */
		if (!pte++) {
			/* pte_offset_map 失败通常表示页表层级不在当前映射，结束邻页扫描即可。 */
			pte = pte_offset_map(vmf->pmd, addr);
			if (!pte)
				break;
		}
		/* lockless 读取只取得单项快照；类型检查防止把已更新 PTE 误作 swap。 */
		pentry = ptep_get_lockless(pte);
		entry = softleaf_from_pte(pentry);

		/* 非 swap PTE 可能已被并发 fault 填充，跳过而非覆盖其现状。 */
		if (!softleaf_is_swap(entry))
			continue;
		pte_unmap(pte);
		pte = NULL;
		/*
		 * Readahead entry may come from a device that we are not
		 * holding a reference to, try to grab a reference, or skip.
		 */
		/* 目标设备已由 fault 路径 pin；其他类型必须临时 pin 后才可读取。 */
		if (swp_type(entry) != swp_type(targ_entry)) {
			/* get 成功后本次循环必须在 read helper 返回后立即 put，不能跨迭代借用。 */
			si = get_swap_device(entry);
			if (!si)
				continue;
		}
		/* helper 可能命中、分配或失败；预读失败只 continue，不改变 fault PTE。 */
		folio = swap_cache_read_folio(entry, gfp_mask, mpol, ilx,
					      &splug, addr != vmf->address);
		if (si)
			put_swap_device(si);
		if (!folio)
			continue;
		folio_put(folio);
	}
	/* 阶段 3：无论中途失败或跳过都解除 PTE 临时映射，并提交聚合 I/O。 */
	if (pte)
		pte_unmap(pte);
	/* unmap 在 finish plug 前完成，避免持有临时页表映射跨 block 层提交。 */
	blk_finish_plug(&plug);
	swap_read_unplug(splug);
	lru_add_drain();
skip:
	/* The folio was likely read above, so no need for plugging here */
	/* 即使窗口为空或中途失败，目标页仍走非 plug 单读，保证 fault 能前进。 */
	folio = swap_cache_read_folio(targ_entry, gfp_mask, mpol, targ_ilx,
				      NULL, false);
	return folio;
}

/**
 * swapin_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct folio for entry and addr, after queueing swapin.
 *
 * It's a main entry function for swap readahead. By the configuration,
 * it will read ahead blocks by cluster-based(ie, physical disk based)
 * or vma-based(ie, virtual address based on faulty address) readahead.
 */
/*
 * 业务背景：这是 page fault 到两种预读算法的统一入口，先绑定 fault VMA 的
 * NUMA policy，避免预读页落到与目标页不一致的节点。入参：entry/gfp 为目标
 * 读取，vmf 必须含有效 VMA/地址。出参/返回：返回目标带引用 folio 或 NULL。
 * 注意事项：policy 是临时引用，必须在两分支共同出口 put；选择开关变化只会
 * 改善/退化命中率，不会改变目标页仍被单独确保读取的正确性。
 */
struct folio *swapin_readahead(swp_entry_t entry, gfp_t gfp_mask,
				struct vm_fault *vmf)
{
	struct mempolicy *mpol;
	pgoff_t ilx;
	struct folio *folio;

	mpol = get_vma_policy(vmf->vma, vmf->address, 0, &ilx);
	folio = swap_use_vma_readahead() ?
		swap_vma_readahead(entry, gfp_mask, mpol, ilx, vmf) :
		swap_cluster_readahead(entry, gfp_mask, mpol, ilx);
	/* 两分支均只借用 policy，结果 folio 的引用独立于该 policy 生命周期。 */
	mpol_cond_put(mpol);

	return folio;
}

#ifdef CONFIG_SYSFS
/*
 * vma_ra_enabled_show() - 读取 VMA 预读开关的 sysfs 文本表示。
 * 业务背景：管理员需要观察当前策略；入参：kobj/attr 仅为 sysfs 回调上下文，
 * buf 是内核提供的输出缓冲。出参/返回：返回写入 buf 的字节数。
 * 注意事项：读取端以 READ_ONCE 获取该开关，因此该快照不与并发 store 串行化。
 */
static ssize_t vma_ra_enabled_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	/* show 读取布尔快照并按 sysfs 文本 ABI 输出；kobj/attr 仅为回调签名。 */
	/* 输出带换行，用户空间可直接按标准 sysfs 单值文件读取。 */
	return sysfs_emit(buf, "%s\n", str_true_false(enable_vma_readahead));
}

/*
 * vma_ra_enabled_store() - 解析并提交管理员写入的 VMA 预读开关。
 * 业务背景：在实际 fault 路径外切换预测策略；入参：buf/count 是 sysfs 写入，
 * kobj/attr 仅为回调上下文。出参/返回：成功返回 count，格式非法返回 errno。
 * 注意事项：成功仅影响后续读取选择；已经提交的 I/O 与现有 cache 不回滚。
 */
static ssize_t vma_ra_enabled_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	/* ret 是解析 errno；count 仅在状态已接受时返回给 sysfs 核心。 */
	/* kstrtobool 失败不改变旧值；成功返回原写入长度是 sysfs 的提交约定。 */
	ssize_t ret;

	ret = kstrtobool(buf, &enable_vma_readahead);
	if (ret)
		return ret;
	/* 成功后普通 bool 写配合读取端 READ_ONCE；该开关无需等待正在 fault 的线程。 */

	/* 成功返回 count 是 sysfs “全部字节已消费”的接口承诺。 */
	return count;
}
static struct kobj_attribute vma_ra_enabled_attr = __ATTR_RW(vma_ra_enabled);

/* 属性数组以 NULL 结尾；group 不拥有 attribute，本静态对象贯穿内核生命周期。 */
static struct attribute *swap_attrs[] = {
	&vma_ra_enabled_attr.attr,
	NULL,
};

static const struct attribute_group swap_attr_group = {
	/* 唯一 group 把 vma_ra_enabled 文件发布在 /sys/kernel/mm/swap 下。 */
	.attrs = swap_attrs,
};

/*
 * swap_init() - 在启用 SYSFS 时发布 swap 策略控制节点。
 * 业务背景：把预读策略纳入 mm 的统一 sysfs 树；入参：无。
 * 出参/返回：0 表示 group 已发布，-ENOMEM 或 sysfs errno 表示未完成发布。
 * 注意事项：仅初始化期运行；失败路径只释放本函数创建的 kobject，不能触碰 cache。
 */
static int __init swap_init(void)
{
	/* 此 initcall 仅在 CONFIG_SYSFS 下编译；无 sysfs 时 cache 机制仍完整可用。 */
	/* swap_kobj 在 group 成功后由 sysfs 层级持有；本函数不保存私有裸指针。 */
	int err;
	struct kobject *swap_kobj;

	/* 初始化阶段创建 /sys/kernel/mm/swap；失败时未发布对象，无需额外回滚。 */
	swap_kobj = kobject_create_and_add("swap", mm_kobj);
	if (!swap_kobj) {
		pr_err("failed to create swap kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(swap_kobj, &swap_attr_group);
	if (err) {
		/* 唯一失败 cleanup：group 未发布，put 即可回收 kobject 及其名称。 */
		pr_err("failed to register swap group\n");
		goto delete_obj;
	}
	/* Swap cache writeback is LRU based, no tags for it */
	mapping_set_no_writeback_tags(&swap_space);
	return 0;

delete_obj:
	/* group 注册失败后 kobject_put 触发层级回收，不能遗留无属性的目录。 */
	kobject_put(swap_kobj);
	return err;
}
subsys_initcall(swap_init);
#endif
