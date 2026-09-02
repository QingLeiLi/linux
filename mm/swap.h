/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_SWAP_H
#define _MM_SWAP_H

#include <linux/atomic.h> /* for atomic_long_t */
#include <linux/mm.h> /* for PAGE_SHIFT */
struct mempolicy;
struct swap_iocb;
struct swap_memcg_table;

/* 本私有头连接 swapfile、swap cache 与 I/O；page_cluster 是全局预读阶数。 */
extern int page_cluster;

#if defined(MAX_POSSIBLE_PHYSMEM_BITS)
#define SWAP_CACHE_PFN_BITS (MAX_POSSIBLE_PHYSMEM_BITS - PAGE_SHIFT)
#elif defined(MAX_PHYSMEM_BITS)
#define SWAP_CACHE_PFN_BITS (MAX_PHYSMEM_BITS - PAGE_SHIFT)
#else
#define SWAP_CACHE_PFN_BITS (BITS_PER_LONG - PAGE_SHIFT)
#endif

/* Swap table marker, 0x1 means shadow, 0x2 means PFN (SWP_TB_PFN_MARK) */
/* 中文翻译：swap table 低两位区分 shadow、PFN 与计数编码。 */
#define SWAP_CACHE_PFN_MARK_BITS	2
/* At least 2 bits are needed to distinguish SWP_TB_COUNT_MAX, 1 and 0 */
/* 中文翻译：计数至少两位，才能区分最大哨兵、1 和 0。 */
#define SWAP_COUNT_MIN_BITS		2
/* If there are enough bits besides PFN and marker, store zero flag inline */
/* 中文翻译：PFN/marker 外仍有空间时，把零页标志直接编码进 table entry。 */
#define SWAP_TABLE_HAS_ZEROFLAG		((BITS_PER_LONG - SWAP_CACHE_PFN_MARK_BITS - \
					  SWAP_CACHE_PFN_BITS) > SWAP_COUNT_MIN_BITS)

#ifdef CONFIG_THP_SWAP
/* THP swap 让 cluster 恰好容纳一个 PMD folio，并保留调用者请求的 order。 */
#define SWAPFILE_CLUSTER	HPAGE_PMD_NR
#define swap_entry_order(order)	(order)
#else
/* 普通配置使用 256 页 cluster，所有 entry 按 order-0 处理。 */
#define SWAPFILE_CLUSTER	256
#define swap_entry_order(order)	0
#endif

extern struct swap_info_struct *swap_info[];

/*
 * We use this to track usage of a cluster. A cluster is a block of swap disk
 * space with SWAPFILE_CLUSTER pages long and naturally aligns in disk. All
 * free clusters are organized into a list. We fetch an entry from the list to
 * get a free cluster.
 *
 * The flags field determines if a cluster is free. This is
 * protected by cluster lock.
 */
/*
 * 中文学习补充：swap_cluster_info 随 swap device 存活。lock 保护 count/flags/
 * order、对应 swap_map、扩展计数和链表状态；RCU table 供无锁查询，memcg/zero
 * 辅助表与 cluster 同生共死。list 只在持设备 cluster-list 锁时链接。
 */
struct swap_cluster_info {
	/* count 是已用槽数，flags/order 决定该 cluster 所在分配队列及 folio 粒度。 */
	spinlock_t lock;	/*
				 * Protect swap_cluster_info fields
				 * other than list, and swap_info_struct->swap_map
				 * elements corresponding to the swap cluster.
				 */
	u16 count;
	u8 flags;
	u8 order;
	/* table 发布后由 RCU 读者借用；大计数溢出到 extend_table。 */
	atomic_long_t __rcu *table;	/* Swap table entries, see mm/swap_table.h */
	unsigned int *extend_table;	/* For large swap count, protected by ci->lock */
	/* 中文翻译：table 保存 swap table entry（见 swap_table.h）；extend_table 在 ci 锁下保存大计数。 */
#ifdef CONFIG_MEMCG
	struct swap_memcg_table *memcg_table;	/* Swap table entries' cgroup record */
	/* 中文翻译：memcg_table 记录每个 swap table entry 所属的 cgroup。 */
#endif
#if !SWAP_TABLE_HAS_ZEROFLAG
	/* inline 编码放不下时，zero_bitmap 逐槽记录“内容全零”，受 ci->lock 保护。 */
	unsigned long *zero_bitmap;
#endif
	/* list 的具体所属队列由 flags 决定，只由设备 cluster-list 锁保护而非 ci->lock。 */
	struct list_head list;
};

/* All on-list cluster must have a non-zero flag. */
/* 中文翻译：进入任一设备链表的 cluster 必须拥有非零状态。 */
enum swap_cluster_flags {
	/* NONE：临时离链状态，由队列迁移路径设置，任何分配链均不得包含它。 */
	CLUSTER_FLAG_NONE = 0, /* For temporary off-list cluster */
	/* FREE：所有槽空闲，设备 free list 设置并由新分配者消费。 */
	CLUSTER_FLAG_FREE,
	/* NONFULL：已有同 order 分配但仍有完整连续空间，可继续同类分配。 */
	CLUSTER_FLAG_NONFULL,
	/* FRAG：只剩碎片空间，仍可由逐槽/较小 order 分配路径使用。 */
	CLUSTER_FLAG_FRAG,
	/* Clusters with flags above are allocatable */
	/* 中文翻译：到 FRAG 为止的上述状态都属于可分配状态。 */
	/* USABLE 是比较上界别名，由分配器判断 flags<=USABLE，不代表独立状态。 */
	CLUSTER_FLAG_USABLE = CLUSTER_FLAG_FRAG,
	/* FULL：没有可分配槽，由最后一次分配设置，释放槽后转回可用链。 */
	CLUSTER_FLAG_FULL,
	/* DISCARD：cluster 已离开分配链，等待底层 discard 完成后才能回到 FREE。 */
	CLUSTER_FLAG_DISCARD,
	/* MAX：枚举范围哨兵，仅供校验/数组界限，绝不写入运行 cluster。 */
	CLUSTER_FLAG_MAX,
};

#ifdef CONFIG_SWAP
#include <linux/swapops.h> /* for swp_offset */
#include <linux/blk_types.h> /* for bio_end_io_t */

/*
 * 业务背景：cluster 分配器需把已验证 swap entry 映射为所在 cluster 的页槽下标。
 * 入参：entry 是有效 swap entry 值，调用者负责稳定对应设备。
 * 出参/返回：返回 0..SWAPFILE_CLUSTER-1 的页单位偏移；无引用或状态变化。
 * 注意事项：不校验 type/offset，也不阻止 swapoff，仅做取模运算。
 */
static inline unsigned int swp_cluster_offset(swp_entry_t entry)
{
	/* 返回 entry 在自然对齐 cluster 内的页槽下标，不取得设备引用。 */
	return swp_offset(entry) % SWAPFILE_CLUSTER;
}

/*
 * Callers of all helpers below must ensure the entry, type, or offset is
 * valid, and protect the swap device with reference count or locks.
 */
/* 中文翻译：以下裸转换要求 entry/type/offset 有效，并由引用或锁阻止 swapoff。 */
/*
 * 业务背景：已持设备引用/锁的 swap 内部路径需要从 type 快速取得全局设备描述。
 * 入参：type 是有效 swap_info[] 下标，调用者保证设备 users 引用非零。
 * 出参/返回：返回借用 si 指针；不增加 users 引用，非法生命周期仅 WARN。
 * 注意事项：READ_ONCE 只防编译器撕裂，不能替代 get_swap_device 或上层锁。
 */
static inline struct swap_info_struct *__swap_type_to_info(int type)
{
	/* READ_ONCE 只稳定指针值；si->users 才是生命周期保证。 */
	struct swap_info_struct *si;

	si = READ_ONCE(swap_info[type]); /* rcu_dereference() */
	VM_WARN_ON_ONCE(percpu_ref_is_zero(&si->users)); /* race with swapoff */
	return si;
}

/*
 * 业务背景：entry 消费路径需先解码 type，再复用裸 swap_info 快速查找。
 * 入参：entry 有效且其设备已由引用/锁稳定，纯值输入。
 * 出参/返回：返回借用 si，不增加引用、无失败编码或副作用。
 * 注意事项：继承 __swap_type_to_info 的 swapoff 约束，不能用于不可信 entry。
 */
static inline struct swap_info_struct *__swap_entry_to_info(swp_entry_t entry)
{
	/* 从 entry 解码 type 后复用设备查找，返回借用 si。 */
	return __swap_type_to_info(swp_type(entry));
}

/*
 * 业务背景：持稳定 si 的路径按页单位 offset 定位固定大小 cluster 元数据。
 * 入参：si 是 users 非零的借用设备；offset 是小于 roundup(si->max, cluster) 的页槽。
 * 出参/返回：返回 si->cluster_info 内借用指针；不加锁、不增加设备引用。
 * 注意事项：越界/失效只 WARN，调用者仍须在访问字段前取得 ci->lock。
 */
static inline struct swap_cluster_info *__swap_offset_to_cluster(
		struct swap_info_struct *si, pgoff_t offset)
{
	/* offset 按 cluster 大小除法定位；调用者必须持 si 引用。 */
	VM_WARN_ON_ONCE(percpu_ref_is_zero(&si->users)); /* race with swapoff */
	VM_WARN_ON_ONCE(offset >= roundup(si->max, SWAPFILE_CLUSTER));
	return &si->cluster_info[offset / SWAPFILE_CLUSTER];
}

/*
 * 业务背景：已稳定 swap entry 的调用者需要一步得到其 cluster 元数据。
 * 入参：entry 的 type/offset 有效且设备由外层引用或锁保护。
 * 出参/返回：返回未加锁的借用 ci；无引用、ownership 或状态变化。
 * 注意事项：仅组合两个裸转换，访问 table/count/flags 前仍须加 cluster 锁。
 */
static inline struct swap_cluster_info *__swap_entry_to_cluster(swp_entry_t entry)
{
	/* 组合设备与 offset 转换，返回的 cluster 不额外计引用。 */
	return __swap_offset_to_cluster(__swap_entry_to_info(entry),
					swp_offset(entry));
}

/*
 * 业务背景：swap_map/table 修改必须在对应 cluster 自旋锁下串行，本 helper 统一普通/关 IRQ 取锁。
 * 入参：si/offset 已由外层稳定且有效；irq 决定 spin_lock_irq 或普通 spin_lock。
 * 出参/返回：返回已加锁借用 ci，调用者获得与 irq 模式匹配的解锁责任。
 * 注意事项：只允许 task 上下文且不可睡眠；锁保护 ci 字段及该 cluster 的 swap_map/table。
 */
static __always_inline struct swap_cluster_info *__swap_cluster_lock(
		struct swap_info_struct *si, unsigned long offset, bool irq)
{
	/* @irq 选择普通或关中断自旋锁；返回时调用者拥有对应解锁责任。 */
	struct swap_cluster_info *ci = __swap_offset_to_cluster(si, offset);

	/*
	 * Nothing modifies swap cache in an IRQ context. All access to
	 * swap cache is wrapped by swap_cache_* helpers, and swap cache
	 * writeback is handled outside of IRQs. Swapin or swapout never
	 * occurs in IRQ, and neither does in-place split or replace.
	 *
	 * Besides, modifying swap cache requires synchronization with
	 * swap_map, which was never IRQ safe.
	 */
	/* 中文翻译：swap cache 不在 IRQ 修改，且 swap_map 同步从来不支持 IRQ 上下文。 */
	VM_WARN_ON_ONCE(!in_task());
	VM_WARN_ON_ONCE(percpu_ref_is_zero(&si->users)); /* race with swapoff */
	/* 获取 ci->lock 后 table/swap_map/count/flags 在当前临界区稳定。 */
	if (irq)
		spin_lock_irq(&ci->lock);
	else
		spin_lock(&ci->lock);
	return ci;
}

/**
 * swap_cluster_lock - Lock and return the swap cluster of given offset.
 * @si: swap device the cluster belongs to.
 * @offset: the swap entry offset, pointing to a valid slot.
 *
 * Context: The caller must ensure the offset is in the valid range and
 * protect the swap device with reference count or locks.
 */
/* 中文补充：成功返回已加锁 cluster；@si/@offset 均只借用，无失败返回。 */
/*
 * 业务背景：公开普通锁入口供无需改变 IRQ 状态的 swap_map/table 临界区使用。
 * 入参：si 是已稳定设备，offset 是有效页槽；均借用且不转移 ownership。
 * 出参/返回：返回已持 spinlock 的 ci，无失败返回；调用者须 swap_cluster_unlock。
 * 注意事项：task 上下文、不可睡眠，offset 合法性和 swapoff 排斥由调用者保证。
 */
static inline struct swap_cluster_info *swap_cluster_lock(
		struct swap_info_struct *si, unsigned long offset)
{
	return __swap_cluster_lock(si, offset, false);
}

/*
 * 业务背景：swapcache folio 的锁已固定其 entry，可直接定位并锁住覆盖整 folio 的唯一 cluster。
 * 入参：folio 必须已锁且在 swap cache；irq 选择是否同时关闭本 CPU 中断。
 * 出参/返回：返回已加锁借用 ci；不改变 folio 引用，调用者承担配对解锁。
 * 注意事项：folio 锁须保持到取得 cluster 锁，避免 entry/slot 在转换途中变化。
 */
static inline struct swap_cluster_info *__swap_cluster_get_and_lock(
		const struct folio *folio, bool irq)
{
	/* folio 锁固定 swap entry 和 cluster 生命周期，随后按 @irq 取得 cluster 锁。 */
	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapcache(folio), folio);
	return __swap_cluster_lock(__swap_entry_to_info(folio->swap),
				   swp_offset(folio->swap), irq);
}

/*
 * swap_cluster_get_and_lock - Locks the cluster that holds a folio's entries.
 * @folio: The folio.
 *
 * This locks and returns the swap cluster that contains a folio's swap
 * entries. The swap entries of a folio are always in one single cluster.
 * The folio has to be locked so its swap entries won't change and the
 * cluster won't be freed.
 *
 * Context: Caller must ensure the folio is locked and in the swap cache.
 * Return: Pointer to the swap cluster.
 */
/* 中文翻译：folio 必须已锁且在 swap cache；返回其唯一 cluster 的已加锁指针。 */
/*
 * 业务背景：folio 级 swap 操作用其稳定 entry 取得普通 cluster 临界区。
 * 入参：folio 是已锁且在 swapcache 的借用对象，其全部 entries 位于同一 cluster。
 * 出参/返回：返回已普通加锁 ci；folio/entry ownership 不变，调用者负责普通 unlock。
 * 注意事项：不可睡眠，违反 folio 前置条件会 WARN 且可能得到失效 cluster。
 */
static inline struct swap_cluster_info *swap_cluster_get_and_lock(
		const struct folio *folio)
{
	return __swap_cluster_get_and_lock(folio, false);
}

/*
 * swap_cluster_get_and_lock_irq - Locks the cluster that holds a folio's entries.
 * @folio: The folio.
 *
 * Same as swap_cluster_get_and_lock but also disable IRQ.
 *
 * Context: Caller must ensure the folio is locked and in the swap cache.
 * Return: Pointer to the swap cluster.
 */
/* 中文翻译：与普通版本相同，但同时关闭本 CPU 中断，必须用 irq 版本解锁。 */
/*
 * 业务背景：可能与本 CPU 中断敏感锁序交互的 folio 路径需在持 cluster 锁时关闭 IRQ。
 * 入参：folio 前置条件与普通版本相同，借用且不转移引用。
 * 出参/返回：返回已锁 ci并关闭本 CPU IRQ；调用者须 swap_cluster_unlock_irq。
 * 注意事项：并非 irq-context 入口，底层仍 WARN 非 task 上下文；严禁与普通 unlock 混配。
 */
static inline struct swap_cluster_info *swap_cluster_get_and_lock_irq(
		const struct folio *folio)
{
	return __swap_cluster_get_and_lock(folio, true);
}

/*
 * 业务背景：结束普通 cluster 临界区，让 swap_map/table/计数更新对竞争者可见。
 * 入参：ci 是由非 irq lock 入口返回且当前调用者持锁的借用指针。
 * 出参/返回：void；释放 ci->lock，不改变设备/cluster ownership。
 * 注意事项：不得与 irq 版本混配，返回后受锁字段不再稳定。
 */
static inline void swap_cluster_unlock(struct swap_cluster_info *ci)
{
	/* 释放普通 cluster 临界区；@ci 仍由设备生命周期拥有。 */
	spin_unlock(&ci->lock);
}

/*
 * 业务背景：结束 irq cluster 临界区并重新允许本 CPU 中断。
 * 入参：ci 是 get_and_lock_irq 返回且当前调用者持锁的借用指针。
 * 出参/返回：void；释放锁并恢复中断使能状态，无 ownership 变化。
 * 注意事项：必须与 spin_lock_irq 路径配对，不能替代 irqsave/restore API。
 */
static inline void swap_cluster_unlock_irq(struct swap_cluster_info *ci)
{
	/* 释放锁并恢复调用前中断状态，与 get_and_lock_irq 严格配对。 */
	spin_unlock_irq(&ci->lock);
}

/*
 * 业务背景：无锁 table 查询遇到扩展计数表缺失时，由实现重新取得设备引用并可睡眠分配。
 * 入参：entry 定位目标 cluster；gfp 约束分配上下文，均不转移 ownership。
 * 出参/返回：成功或设备已消失返回 0，分配失败负 errno；可能在 ci 锁下发布 extend_table。
 * 注意事项：函数内部 get/put swap device；返回 0 后调用者仍须重新查询，不能复用旧 table 观察。
 */
extern int swap_retry_table_alloc(swp_entry_t entry, gfp_t gfp);

/*
 * Below are the core routines for doing swap for a folio.
 * All helpers requires the folio to be locked, and a locked folio
 * in the swap cache pins the swap entries / slots allocated to the
 * folio, swap relies heavily on the swap cache and folio lock for
 * synchronization.
 *
 * folio_alloc_swap(): the entry point for a folio to be swapped
 * out. It allocates swap slots and pins the slots with swap cache.
 * The slots start with a swap count of zero. The slots are pinned
 * by swap cache reference which doesn't contribute to swap count.
 *
 * folio_dup_swap(): increases the swap count of a folio, usually
 * during it gets unmapped and a swap entry is installed to replace
 * it (e.g., swap entry in page table). A swap slot with swap
 * count == 0 can only be increased by this helper.
 *
 * folio_put_swap(): does the opposite thing of folio_dup_swap().
 */
/*
 * 中文学习补充：三项核心协议都要求 folio 锁。alloc 分配零计数槽并以 swap
 * cache 引用固定；dup 在页表安装 entry 时增加槽计数；put 做反向递减。swap
 * cache pin 不计入 swap count，最后一个页表引用消失不等于 folio 已离 cache。
 */
/* 背景：swapout 为已锁 folio 分配零计数连续槽并加入 cache；入参 folio 借用且已锁。 */
/* 返回：成功 0、失败负 errno；成功后 folio 获得 swap entry/cache pin；注意：调用者处理失败回收。 */
int folio_alloc_swap(struct folio *folio);
/* 背景：安装页表 swap entry 前增加已锁 folio 指定 subpage 的槽计数。 */
/* 入参/返回：folio/subpage 借用；成功 0、失败负 errno；成功必须由 folio_put_swap 配对。 */
int folio_dup_swap(struct folio *folio, struct page *subpage);
/* 背景：页表 entry 消失时减少已锁 folio 指定 subpage 的 swap count。 */
/* 入参/返回：folio/subpage 借用，void；可能释放最后槽计数但不等同移除 swap cache pin。 */
void folio_put_swap(struct folio *folio, struct page *subpage);

/* For internal use */
/* 中文翻译：内部批量释放 cluster 槽，调用者持 ci 锁并保证范围属于该 cluster。 */
/*
 * 背景：持 ci 锁的批量回收把 cluster 内连续槽归还设备分配器。
 * 入参：si/ci 借用且稳定，ci_off/nr_pages 为 cluster 内页单位范围。
 * 返回：void；更新 swap_map/table/count/队列，可能使 cluster 重新可分配。
 * 注意：调用者保证范围有效且无残留 cache/页表引用，锁责任不转移。
 */
extern void __swap_cluster_free_entries(struct swap_info_struct *si,
					struct swap_cluster_info *ci,
					unsigned int ci_off, unsigned int nr_pages);

/* linux/mm/page_io.c */
/* page_io 接口借用已锁 folio；plug 双指针允许累计异步 I/O 后统一提交。 */
/* 背景：启动期建立 swap I/O 辅助池；无入参；成功 0、失败负 errno；可睡眠且失败阻止后续 I/O。 */
int sio_pool_init(void);
struct swap_iocb;
/* 背景：对已锁 folio 发起 swapin，并可把 I/O 累积进 plug；folio/plug 借用，完成状态异步发布。 */
void swap_read_folio(struct folio *folio, struct swap_iocb **plug);
/* 背景：提交并结束非空读 plug；入参 ownership 交给实现，void；调用后禁止复用。 */
void __swap_read_unplug(struct swap_iocb *plug);
/*
 * 业务背景：swapin 批量读结束时仅在确有异步 plug 时提交，调用方无需重复判空。
 * 入参：plug 可为 NULL；非空对象的最终处理契约交给 __swap_read_unplug。
 * 出参/返回：void；NULL 无副作用，非空时提交/完成累积 I/O。
 * 注意事项：调用后不得再次使用同一 plug，具体可睡眠/完成语义由底层实现决定。
 */
static inline void swap_read_unplug(struct swap_iocb *plug)
{
	/* NULL 是无 I/O 快速路径，非空 ownership 交给底层 unplug 完成。 */
	if (unlikely(plug))
		__swap_read_unplug(plug);
}

/* 背景：提交/释放写 plug；sio ownership 交给实现，void；调用后禁止复用。 */
void swap_write_unplug(struct swap_iocb *sio);
/* 背景：按 writeback 策略尝试写出已锁 folio；plug 为输入输出，返回 0/负 errno并可能异步发 I/O。 */
int swap_writeout(struct folio *folio, struct swap_iocb **swap_plug);
/* 背景：writepage 核心对已锁 folio 建立 swap I/O；plug 输入输出，void；完成/解锁语义由实现负责。 */
void __swap_writepage(struct folio *folio, struct swap_iocb **swap_plug);

/* linux/mm/swap_state.c */
/* 全部 swap entries 共享 swap_space address_space，entry 参数只保持统一 ABI。 */
extern struct address_space swap_space __read_mostly;
/*
 * 业务背景：swap cache 统一借用全局 swap_space，调用者仍以 entry 保持常规 mapping 查询 ABI。
 * 入参：entry 当前实现不使用，调用者仍须保证后续槽操作的 entry 有效。
 * 出参/返回：返回模块生命周期内稳定的 swap_space 借用指针；无副作用。
 * 注意事项：不能由返回 mapping 推导设备引用，swapoff 稳定性仍由 entry 调用链负责。
 */
static inline struct address_space *swap_address_space(swp_entry_t entry)
{
	return &swap_space;
}

/*
 * Return the swap device position of the swap entry.
 */
/* 中文翻译：把页单位 swap offset 左移 PAGE_SHIFT，得到设备字节位置。 */
/*
 * 业务背景：块 I/O 构造需把页单位 swap offset 转换为设备字节位置。
 * 入参：entry 是已验证 swap entry，函数只读取其 offset。
 * 出参/返回：返回 offset<<PAGE_SHIFT 的 loff_t 字节偏移；无引用或状态变化。
 * 注意事项：不检查溢出/type/设备边界，调用者必须先完成 entry 验证。
 */
static inline loff_t swap_dev_pos(swp_entry_t entry)
{
	return ((loff_t)swp_offset(entry)) << PAGE_SHIFT;
}

/**
 * folio_matches_swap_entry - Check if a folio matches a given swap entry.
 * @folio: The folio.
 * @entry: The swap entry to check against.
 *
 * Context: The caller should have the folio locked to ensure it's stable
 * and nothing will move it in or out of the swap cache.
 * Return: true or false.
 */
/* 中文补充：folio 锁稳定 swapcache 标志和起始 entry；返回值不改变引用。 */
/*
 * 业务背景：cache 查删路径需判断任意 entry 是否落在给定 swapcache folio 的连续槽范围。
 * 入参：folio 应已锁；entry 是待比较值，二者均借用/纯值输入。
 * 出参/返回：同一对齐 folio 范围返回 true，否则 false；不改变引用或 cache 状态。
 * 注意事项：大 folio 起始 entry 必须按 nr_pages 对齐，非 swapcache 快速返回 false。
 */
static inline bool folio_matches_swap_entry(const struct folio *folio,
					    swp_entry_t entry)
{
	swp_entry_t folio_entry = folio->swap;
	long nr_pages = folio_nr_pages(folio);

	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	/* 非 swapcache 快速失败；大 folio entry 必须按页数对齐。 */
	if (!folio_test_swapcache(folio))
		return false;
	VM_WARN_ON_ONCE_FOLIO(!IS_ALIGNED(folio_entry.val, nr_pages), folio);
	return folio_entry.val == round_down(entry.val, nr_pages);
}

/*
 * All swap cache helpers below require the caller to ensure the swap entries
 * used are valid and stabilize the device by any of the following ways:
 * - Hold a reference by get_swap_device(): this ensures a single entry is
 *   valid and increases the swap device's refcount.
 * - Locking a folio in the swap cache: this ensures the folio's swap entries
 *   are valid and pinned, also implies reference to the device.
 * - Locking anything referencing the swap entry: e.g. PTL that protects
 *   swap entries in the page table, similar to locking swap cache folio.
 * - See the comment of get_swap_device() for more complex usage.
 */
/*
 * 中文学习补充：以下 cache helper 的 entry 必须由 get_swap_device、已锁
 * swapcache folio 或保护页表 entry 的 PTL 稳定；返回 folio/shadow 的具体引用
 * 语义由实现说明。锁内 __ 版本还要求调用者传入已锁定的所属 cluster。
 */
/* 背景：无引用探测 entry 槽是否保存 folio；entry 需由外层稳定；返回 bool，无 ownership 变化。 */
bool swap_cache_has_folio(swp_entry_t entry);
/* get_folio/get_shadow 查询同一 table 槽；调用者按实现约定释放所得引用。 */
/* 背景：取得 entry 对应 folio；入参需稳定；返回带引用 folio 或 NULL，调用者负责 folio_put。 */
struct folio *swap_cache_get_folio(swp_entry_t entry);
/* 背景：取得 entry 对应 workingset shadow；返回借用/编码指针或 NULL，不得当 folio 解引用。 */
void *swap_cache_get_shadow(swp_entry_t entry);
/* 背景：把已锁 folio 从 swap cache 删除；folio 借用，void；释放 cache pin 并更新 table。 */
void swap_cache_del_folio(struct folio *folio);
/*
 * 背景：fault/readahead 按 target entry 和 order 集合查找或分配 cache folio。
 * 入参：entry/gfp/orders/vmf/mpol/ilx 均借用或按值，mpol 不被消费。
 * 返回：成功为已在 cache 的 folio，失败为 ERR_PTR；可能分配并发布 cache 项。
 * 注意：调用者须稳定设备并按返回契约解锁/put，分配可睡眠。
 */
struct folio *swap_cache_alloc_folio(swp_entry_t target_entry, gfp_t gfp_mask,
				     unsigned long orders, struct vm_fault *vmf,
				     struct mempolicy *mpol, pgoff_t ilx);
/* Below helpers require the caller to lock and pass in the swap cluster. */
/* 中文翻译：以下 __ helper 要求调用者已锁定并传入目标 swap cluster。 */
/* 背景：持 ci 锁把已锁 folio/entry 发布到 cache；三者借用；void，新增 cache pin/table 项。 */
void __swap_cache_add_folio(struct swap_cluster_info *ci,
			    struct folio *folio, swp_entry_t entry);
/* 背景：持 ci 锁删除 folio/entry 并可留下 shadow；入参借用，void；转移/消费 shadow 依实现。 */
void __swap_cache_del_folio(struct swap_cluster_info *ci,
			    struct folio *folio, swp_entry_t entry, void *shadow);
/* 背景：持 ci 锁把 cache ownership 从 old 原子替换到 new；void；调用者保证两 folio 锁/引用。 */
void __swap_cache_replace_folio(struct swap_cluster_info *ci,
				struct folio *old, struct folio *new);

/* 背景：诊断路径输出全局 swap cache 统计；无入参/返回；只观察但会写日志。 */
void show_swap_cache_info(void);
/* clear 批量删除设备槽；read/readahead/sync 分别实现异步、簇预读和 fault 同步读。 */
/* 背景：swapoff/回收批量清除设备连续 cache 槽；si/entry 稳定，nr 页数；void并释放相关引用。 */
void swapcache_clear(struct swap_info_struct *si, swp_entry_t entry, int nr);
/* 背景：fault 异步取得 entry folio；参数均借用，plug 输入输出；返回 folio/NULL/错误并可能发 I/O。 */
struct folio *read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
		struct vm_area_struct *vma, unsigned long addr,
		struct swap_iocb **plug);
/* 背景：以 entry 所在 cluster 形成预读窗口；mpol 借用；返回目标 folio或NULL，可能批量发 I/O。 */
struct folio *swap_cluster_readahead(swp_entry_t entry, gfp_t flag,
		struct mempolicy *mpol, pgoff_t ilx);
/* fault-aware readahead 可用 vmf 决定窗口，sync 则按 orders 同步取得目标 folio。 */
/* 背景：fault-aware 选择预读算法；entry/vmf 稳定；返回目标 folio或NULL并可能更新窗口/发 I/O。 */
struct folio *swapin_readahead(swp_entry_t entry, gfp_t flag,
		struct vm_fault *vmf);
/* 背景：按允许 orders 同步取得目标 swapin folio；策略/fault 参数借用；返回 folio或失败结果。 */
struct folio *swapin_sync(swp_entry_t entry, gfp_t flag, unsigned long orders,
			   struct vm_fault *vmf, struct mempolicy *mpol, pgoff_t ilx);
/* 背景：swapin 命中后用 folio/VMA/地址反馈后续预读；参数借用；void且更新预读状态。 */
void swap_update_readahead(struct folio *folio, struct vm_area_struct *vma,
			   /* 命中后用 VMA/地址反馈更新后续预读窗口。 */
			   unsigned long addr);

/*
 * 业务背景：swapcache folio 的 I/O/策略路径需要读取所属 swap device 的 flags 快照。
 * 入参：folio 必须已锁且保持有效 swap entry，函数不取得 folio 引用。
 * 出参/返回：返回 si->flags 当前值；不改变 entry、设备引用或任何状态。
 * 注意事项：返回后 flags 可变化，folio 锁固定 entry 但设备生命周期仍依赖 swapcache pin。
 */
static inline unsigned int folio_swap_flags(struct folio *folio)
{
	/* folio 锁固定其 swap entry；返回设备 flags 快照，不延长 si 生命周期。 */
	return __swap_entry_to_info(folio->swap)->flags;
}

#else /* CONFIG_SWAP */
/* CONFIG_SWAP=n：所有查询返回中性值，分配/计数操作返回 -EINVAL 或空操作。 */
struct swap_iocb;
/*
 * 业务背景：关闭 swap 时保留 cluster-lock ABI，让通用调用者可由编译器消去分支。
 * 入参：si/offset/irq 均不使用、不取得引用。
 * 出参/返回：恒返回 NULL；不加锁、不改变 IRQ 或任何状态。
 * 注意事项：调用者不得解引用结果；该签名仅配置兼容。
 */
static inline struct swap_cluster_info *swap_cluster_lock(
	struct swap_info_struct *si, pgoff_t offset, bool irq)
{
	/* 无 swap device/cluster，忽略全部借用参数并返回 NULL。 */
	return NULL;
}

/*
 * 业务背景：无 swapcache 时提供 folio→cluster 普通锁操作的中性桩。
 * 入参：folio 是未使用借用指针，不要求锁定。
 * 出参/返回：恒返回 NULL；不加锁、不改变 folio。
 * 注意事项：只存在于 CONFIG_SWAP=n，调用者须容忍空结果。
 */
static inline struct swap_cluster_info *swap_cluster_get_and_lock(
		struct folio *folio)
{
	/* 无 cluster 可锁，返回 NULL 且不改变 folio。 */
	return NULL;
}

/*
 * 业务背景：无 swapcache 时保留关闭 IRQ 的 cluster-lock 调用形状。
 * 入参：folio 未使用且 ownership 不变。
 * 出参/返回：恒返回 NULL；尤其不会关闭本 CPU IRQ。
 * 注意事项：配对 irq unlock 也是空操作，不能据此建立同步。
 */
static inline struct swap_cluster_info *swap_cluster_get_and_lock_irq(
		struct folio *folio)
{
	/* CONFIG_SWAP=n 不改中断状态，返回 NULL。 */
	return NULL;
}

/*
 * 业务背景：与 CONFIG_SWAP=n 普通 lock 桩配对，允许无条件 cleanup。
 * 入参：ci 通常为 NULL，未使用。
 * 出参/返回：void 且无副作用。
 * 注意事项：不提供锁释放或内存排序保证。
 */
static inline void swap_cluster_unlock(struct swap_cluster_info *ci)
{
	/* 与返回 NULL 的 lock 桩配对，空操作。 */
}

/*
 * 业务背景：与 CONFIG_SWAP=n irq lock 桩配对，统一调用者退出路径。
 * 入参：ci 未使用，不取得 ownership。
 * 出参/返回：void；不解锁且不改变 IRQ 状态。
 * 注意事项：不能用于释放其他路径实际取得的锁。
 */
static inline void swap_cluster_unlock_irq(struct swap_cluster_info *ci)
{
	/* 未关闭中断，因此也无需恢复。 */
}

/*
 * 业务背景：关闭 swap 后不存在从 entry 到设备的映射，保留查询 ABI。
 * 入参：entry 纯值输入且不解析。
 * 出参/返回：恒返回 NULL；不取得设备引用。
 * 注意事项：调用者不得把 NULL 当作受保护 swap_info。
 */
static inline struct swap_info_struct *__swap_entry_to_info(swp_entry_t entry)
{
	/* 无已注册 swap_info，稳定返回 NULL。 */
	return NULL;
}

/*
 * 业务背景：回收路径在无 swap 后端时必须明确得知 folio 无法分配 swap 槽。
 * 入参：folio 是借用对象，不要求锁且不修改。
 * 出参/返回：恒返回 -EINVAL；不分配 entry、不建立 cache pin。
 * 注意事项：调用者须保留 folio 或选择非 swap 回收路径。
 */
static inline int folio_alloc_swap(struct folio *folio)
{
	/* 明确拒绝 swapout，调用者必须保留 folio 或选择其他回收路径。 */
	return -EINVAL;
}

/*
 * 业务背景：无 swap slot 时页表替换路径不能增加 folio 的 swap count。
 * 入参：folio/page 均借用且不读取。
 * 出参/返回：恒返回 -EINVAL；无计数或 ownership 变化。
 * 注意事项：调用者不得据此安装 swap PTE。
 */
static inline int folio_dup_swap(struct folio *folio, struct page *page)
{
	/* 无槽可增引用，返回 -EINVAL 且不改变 folio/page。 */
	return -EINVAL;
}

/*
 * 业务背景：通用释放路径需要与 dup ABI 配对，即使配置下从未取得 swap count。
 * 入参：folio/page 均未使用、ownership 不变。
 * 出参/返回：void 且无副作用。
 * 注意事项：本桩不释放 folio 引用或页表状态。
 */
static inline void folio_put_swap(struct folio *folio, struct page *page)
{
	/* 无槽计数可释放，保持 void ABI 的无副作用桩。 */
}

/*
 * 业务背景：无 swap 后端时保留读 folio 入口供配置无关调用代码编译。
 * 入参：folio/plug 均借用且不使用，plug ownership 不变。
 * 出参/返回：void；不发 I/O、不解锁或更新 folio。
 * 注意事项：调用者不能期待 uptodate/error/completion 状态变化。
 */
static inline void swap_read_folio(struct folio *folio, struct swap_iocb **plug)
{
	/* 无读 I/O；folio 状态和 plug ownership 均保持不变。 */
}

/*
 * 业务背景：无 swap 写 I/O 时让统一 cleanup 可无条件 unplug。
 * 入参：sio 未使用，不消费指针。
 * 出参/返回：void；不提交 I/O 或释放资源。
 * 注意事项：只能配对本配置下不会创建的 plug。
 */
static inline void swap_write_unplug(struct swap_iocb *sio)
{
	/* 无写 plug 可提交，空操作。 */
}

/*
 * 业务背景：关闭 swap 后不存在全局 swap cache mapping。
 * 入参：entry 未使用，纯值输入。
 * 出参/返回：恒返回 NULL；无引用或状态变化。
 * 注意事项：调用者须在访问 mapping 前处理 NULL。
 */
static inline struct address_space *swap_address_space(swp_entry_t entry)
{
	/* 未配置 swap_space，返回 NULL 阻止 cache 操作。 */
	return NULL;
}

/*
 * 业务背景：无 swapcache 配置下任何 folio 都不可能匹配 swap entry。
 * 入参：folio/entry 均只借用或按值传入，不读取。
 * 出参/返回：恒 false；无副作用。
 * 注意事项：不检查 folio 锁，因为配置不可能建立该状态。
 */
static inline bool folio_matches_swap_entry(const struct folio *folio, swp_entry_t entry)
{
	/* 任何 folio 都不可能属于 swap cache。 */
	return false;
}

/*
 * 业务背景：诊断调用在无 swap cache 时仍可无条件执行。
 * 入参：无。
 * 出参/返回：void；不输出任何统计。
 * 注意事项：静默空操作不代表运行时存在零容量设备。
 */
static inline void show_swap_cache_info(void)
{
	/* 无 cache 统计可输出。 */
}

/*
 * 业务背景：无 swapin 后端时 cluster readahead 必须返回无结果。
 * 入参：entry/gfp/mpol/ilx 均不使用，mpol ownership 不变。
 * 出参/返回：恒 NULL；不分配 folio、不发 I/O。
 * 注意事项：调用者须走缺页失败或其他恢复路径。
 */
static inline struct folio *swap_cluster_readahead(swp_entry_t entry,
			gfp_t gfp_mask, struct mempolicy *mpol, pgoff_t ilx)
{
	/* swapin 不可用，返回 NULL 且不消费 mempolicy。 */
	return NULL;
}

/*
 * 业务背景：关闭 swap 时 fault-aware readahead 保留中性查询 ABI。
 * 入参：swp/gfp/vmf 均不使用、不取得引用。
 * 出参/返回：恒 NULL；无 I/O 或 fault 状态变化。
 * 注意事项：返回 NULL 是配置退化，不是异步 I/O 正在进行。
 */
static inline struct folio *swapin_readahead(swp_entry_t swp, gfp_t gfp_mask,
			struct vm_fault *vmf)
{
	/* fault 预读不可用，返回 NULL。 */
	return NULL;
}

/*
 * 业务背景：关闭 swap 后同步按 orders 读取目标 folio 不可实现。
 * 入参：entry/flag/orders/vmf/mpol/ilx 均不使用，ownership 不变。
 * 出参/返回：恒 NULL；不分配、不锁 folio、不发 I/O。
 * 注意事项：调用者不得将 NULL 解释为已成功实例化。
 */
static inline struct folio *swapin_sync(
	swp_entry_t entry, gfp_t flag, unsigned long orders,
	struct vm_fault *vmf, struct mempolicy *mpol, pgoff_t ilx)
{
	/* 同步 swapin 不可用，返回 NULL 且不消费策略对象。 */
	return NULL;
}

/*
 * 业务背景：无 swapin 历史时统一 fault 路径仍可调用反馈接口。
 * 入参：folio/vma/addr 均借用或按值传入且不使用。
 * 出参/返回：void；不更新窗口、引用或 VMA 状态。
 * 注意事项：不提供任何预读学习效果。
 */
static inline void swap_update_readahead(struct folio *folio,
		struct vm_area_struct *vma, unsigned long addr)
{
	/* 无 swapin 历史需要反馈，空操作。 */
}

/*
 * 业务背景：无 swap 后端时 writeout 入口不产生块 I/O，但保持调用 ABI。
 * 入参：folio/swap_plug 均借用且不修改。
 * 出参/返回：返回 0；不写出、不解锁 folio、不创建 plug。
 * 注意事项：0 仅表示桩无 I/O 错误，不证明数据已持久化。
 */
static inline int swap_writeout(struct folio *folio,
		struct swap_iocb **swap_plug)
{
	/* 无 swap 后端时不发 I/O，0 表示该桩没有写出错误可报告。 */
	return 0;
}

/*
 * 业务背景：无 swap table 时因缺页元数据而重试分配必然不可用。
 * 入参：entry/gfp 均不使用。
 * 出参/返回：恒 -EINVAL；不分配 table 或改变 entry。
 * 注意事项：调用者应终止 swap 操作而非循环重试。
 */
static inline int swap_retry_table_alloc(swp_entry_t entry, gfp_t gfp)
{
	/* 不存在 table，重试分配永远以 -EINVAL 结束。 */
	return -EINVAL;
}

/*
 * 业务背景：关闭 swap 时 cache 命中查询有确定的中性答案。
 * 入参：entry 不解析。
 * 出参/返回：恒 false；无引用或状态变化。
 * 注意事项：不验证 entry 合法性。
 */
static inline bool swap_cache_has_folio(swp_entry_t entry)
{
	/* 无 cache，任何 entry 都不命中。 */
	return false;
}

/*
 * 业务背景：无 swap cache 时无法按 entry 取得 folio 引用。
 * 入参：entry 不解析。
 * 出参/返回：恒 NULL；不增加 folio 引用。
 * 注意事项：调用者无需 put，但必须处理缺失结果。
 */
static inline struct folio *swap_cache_get_folio(swp_entry_t entry)
{
	/* 无 folio 引用可返回。 */
	return NULL;
}

/*
 * 业务背景：无 swap cache 时也不存在被驱逐条目的 workingset shadow。
 * 入参：entry 不解析。
 * 出参/返回：恒 NULL；不取得 shadow ownership。
 * 注意事项：NULL 是配置中性结果。
 */
static inline void *swap_cache_get_shadow(swp_entry_t entry)
{
	/* 无 workingset shadow 可返回。 */
	return NULL;
}

/*
 * 业务背景：统一 teardown 可在无 swapcache 配置调用删除入口。
 * 入参：folio 借用且不读取。
 * 出参/返回：void；不删 mapping、不释放引用。
 * 注意事项：调用者不能依赖本桩清理其他 folio 状态。
 */
static inline void swap_cache_del_folio(struct folio *folio)
{
	/* folio 不可能在 swap cache，删除为空操作。 */
}

/*
 * 业务背景：无 cache 时保留“已持 cluster 锁”删除 helper 的配置 ABI。
 * 入参：ci/folio/entry/shadow 均不使用，shadow ownership 不被消费。
 * 出参/返回：void；无 mapping、计数或引用变化。
 * 注意事项：调用者若实际拥有 shadow，仍须自行按来源释放。
 */
static inline void __swap_cache_del_folio(struct swap_cluster_info *ci,
		struct folio *folio, swp_entry_t entry, void *shadow)
{
	/* 锁内删除桩不消费 shadow，也不改变 folio。 */
}

/*
 * 业务背景：无 cache 时不存在锁内 old→new folio 替换状态。
 * 入参：ci/old/new 均借用且不读取。
 * 出参/返回：void；不转移引用、entry 或 mapping ownership。
 * 注意事项：不能用于完成其他 address_space 的替换。
 */
static inline void __swap_cache_replace_folio(struct swap_cluster_info *ci,
		struct folio *old, struct folio *new)
{
	/* 无 cache，不改变 old/new ownership。 */
}

/*
 * 业务背景：无 swap device 时策略调用需要一个中性 flags 值。
 * 入参：folio 未使用，不要求锁定。
 * 出参/返回：恒返回 0；无引用或状态变化。
 * 注意事项：0 表示配置缺失，不是某真实设备 flags 快照。
 */
static inline unsigned int folio_swap_flags(struct folio *folio)
{
	/* 无设备 flags，返回中性 0。 */
	return 0;
}

#endif /* CONFIG_SWAP */
#endif /* _MM_SWAP_H */
