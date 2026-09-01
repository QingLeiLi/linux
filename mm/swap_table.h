/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_SWAP_TABLE_H
#define _MM_SWAP_TABLE_H
/*
 * 本私有头定义每个 swap cluster 的原子槽位表编码及访问 helper。一个 unsigned long
 * 同时表达 free、working-set shadow、swap-cache folio PFN、bad slot、引用 count 和可选
 * zero-page flag；cluster lock 负责复合更新，RCU 允许无锁读者跨表摘除生命周期读取。
 */

#include <linux/rcupdate.h>
#include <linux/atomic.h>
#include "swap.h"

/* A typical flat array in each cluster as swap table */
/* 每个 cluster 用固定 SWAPFILE_CLUSTER 个原子 long 与 swap slot 一一对应。 */
struct swap_table {
	/* entries[i] 的位布局由下方 SWP_TB_* 定义；原子性支持 RCU 读侧单槽快照。 */
	atomic_long_t entries[SWAPFILE_CLUSTER];
};

/* For storing memcg private id */
/* memcg 私有 ID 与同 cluster 的 slot 同下标存放，仅在 CONFIG_MEMCG 时分配和使用。 */
struct swap_memcg_table {
	/* 0 表示无归属；非零 ID 由 memcg swap 记账路径解释。 */
	unsigned short id[SWAPFILE_CLUSTER];
};

/* 若整张表恰好一页，分配/RCU 释放可直接使用 folio；否则走专用 slab cache。 */
#define SWP_TABLE_USE_PAGE (sizeof(struct swap_table) == PAGE_SIZE)

/*
 * A swap table entry represents the status of a swap slot on a swap
 * (physical or virtual) device. The swap table in each cluster is a
 * 1:1 map of the swap slots in this cluster.
 *
 * Swap table entry type and bits layouts:
 *
 * NULL:     |---------------- 0 ---------------| - Free slot
 * Shadow:   |SWAP_COUNT|Z|---- SHADOW_VAL ---|1| - Swapped out slot
 * PFN:      |SWAP_COUNT|Z|------ PFN -------|10| - Cached slot
 * Pointer:  |----------- Pointer ----------|100| - (Unused)
 * Bad:      |------------- 1 -------------|1000| - Bad slot
 *
 * COUNT is `SWP_TB_COUNT_BITS` long, Z is the `SWP_TB_ZERO_FLAG` bit,
 * and together they form the `SWP_TB_FLAGS_BITS` wide flags field.
 * Each entry is an atomic long.
 *
 * Usages:
 *
 * - NULL: Swap slot is unused, could be allocated.
 *
 * - Shadow: Swap slot is used and not cached (usually swapped out). It reuses
 *   the XA_VALUE format to be compatible with working set shadows. SHADOW_VAL
 *   part might be all 0 if the working shadow info is absent. In such a case,
 *   we still want to keep the shadow format as a placeholder.
 *
 *   Memcg ID is embedded in SHADOW_VAL.
 *
 * - PFN: Swap slot is in use, and cached. Memcg info is recorded on the page
 *   struct.
 *
 * - Pointer: Unused yet. `0b100` is reserved for potential pointer usage
 *   because only the lower three bits can be used as a marker for 8 bytes
 *   aligned pointers.
 *
 * - Bad: Swap slot is reserved, protects swap header or holes on swap devices.
 */
/*
 * 一个 entry 是 cluster 内物理/虚拟 swap slot 的 1:1 状态字：全零可分配；低位 1 的
 * XA_VALUE shadow 表示已换出且未缓存，并在 payload 内携带 workingset/memcg 信息；低位
 * 10 的 PFN 表示 slot 已有 swap-cache folio；100 保留给未来 8 字节对齐指针；高位全 1、
 * 低位 1000 的 BAD 保护 swap header 或设备洞。最高 flags 域由 count 与可选 zero bit
 * 组成，每个状态字均以 atomic_long_t 发布。
 */

/* NULL Entry, all 0 */
/* 全零同时表示类型 NULL 和 count=0，是唯一可重新分配的空槽。 */
#define SWP_TB_NULL		0UL

/* Swapped out: shadow */
/* XA_VALUE 的低位 1 作为 shadow 标记；即使无 workingset payload 也保留该占位格式。 */
#define SWP_TB_SHADOW_MARK	0b1UL

/* Cached: PFN */
/* PFN 编码占物理 PFN 位加 2 个 marker 位，低位 10 与 shadow/null/bad 区分。 */
#define SWP_TB_PFN_BITS		(SWAP_CACHE_PFN_BITS + SWAP_CACHE_PFN_MARK_BITS)
#define SWP_TB_PFN_MARK		0b10UL
#define SWP_TB_PFN_MARK_MASK	(BIT(SWAP_CACHE_PFN_MARK_BITS) - 1)

/* Flags: For PFN or shadow, contains SWAP_COUNT, width changes */
/*
 * flags 位于 unsigned long 最高端，最多 5 位且必须为 PFN 留足空间；若架构余量允许，
 * 最低 flags 位作为 zero flag，其余 count 至少能表达 0、普通引用和 overflow 哨兵。
 */
#define SWP_TB_FLAGS_BITS	min(5, BITS_PER_LONG - SWP_TB_PFN_BITS)
#define SWP_TB_COUNT_BITS	(SWP_TB_FLAGS_BITS - SWAP_TABLE_HAS_ZEROFLAG)
#define SWP_TB_FLAGS_MASK	(~((~0UL) >> SWP_TB_FLAGS_BITS))
#define SWP_TB_COUNT_MASK      (~((~0UL) >> SWP_TB_COUNT_BITS))
#define SWP_TB_FLAGS_SHIFT     (BITS_PER_LONG - SWP_TB_FLAGS_BITS)
#define SWP_TB_COUNT_SHIFT     (BITS_PER_LONG - SWP_TB_COUNT_BITS)
#define SWP_TB_COUNT_MAX       ((1 << SWP_TB_COUNT_BITS) - 1)
/* The first flag is zero bit (SWAP_TABLE_HAS_ZEROFLAG) */
/* inline zero flag 占 flags 域最低位；不支持时同一语义存入 cluster 的 zero_bitmap。 */
#define SWP_TB_ZERO_FLAG	BIT(BITS_PER_LONG - SWP_TB_FLAGS_BITS)

/* Bad slot: ends with 0b1000 and rests of bits are all 1 */
/* BAD 不可计数且不携带 PFN/shadow；该常量避免保留区被正常分配。 */
#define SWP_TB_BAD		((~0UL) << 3)

/* Macro for shadow offset calculation */
/* workingset shadow payload 必须为最高 flags 域让位，复用此位数作为编码位移契约。 */
#define SWAP_COUNT_SHIFT	SWP_TB_FLAGS_BITS

/*
 * Helpers for casting one type of info into a swap table entry.
 */
/* 下组 helper 只构造位模式，不发布到表；调用者随后须在正确锁/原子边界写入。 */
/*
 * null_to_swp_tb() - 构造空槽状态字
 * 入参：无。出参/返回：返回全零 SWP_TB_NULL，无副作用。
 * 注意事项：编译期确认 unsigned long 与 atomic_long_t 等宽，保证原子槽可完整承载编码。
 */
static inline unsigned long null_to_swp_tb(void)
{
	BUILD_BUG_ON(sizeof(unsigned long) != sizeof(atomic_long_t));
	return 0;
}

/*
 * __count_to_swp_tb() - 把 swap 引用计数编码到状态字最高 count 域
 * 入参：count 范围 0..SWP_TB_COUNT_MAX，纯输入。
 * 出参/返回：返回仅含 count 位的掩码值；不保留类型/payload。
 * 注意事项：编译期保证至少三态，运行期越界仅警告；调用者通常与旧字其余位 OR/替换。
 */
static inline unsigned long __count_to_swp_tb(unsigned char count)
{
	/*
	 * At least three values are needed to distinguish free (0),
	 * used (count > 0 && count < SWP_TB_COUNT_MAX), and
	 * overflow (count == SWP_TB_COUNT_MAX).
	 */
	/* 至少需要 free=0、正常 1..max-1 和 overflow=max 三种可区分状态。 */
	BUILD_BUG_ON(SWP_TB_COUNT_BITS < SWAP_COUNT_MIN_BITS);
	VM_WARN_ON(count > SWP_TB_COUNT_MAX);
	return ((unsigned long)count) << SWP_TB_COUNT_SHIFT;
}

/*
 * __flags_to_swp_tb() - 把 count+zero 的低位 flags 值移到状态字最高域
 * 入参：flags 只能使用 SWP_TB_FLAGS_BITS 个低位。
 * 出参/返回：返回移位后的 flags 位模式，无副作用。
 * 注意事项：超宽输入仅警告；调用者负责避免与 PFN/shadow payload 冲突。
 */
static inline unsigned long __flags_to_swp_tb(unsigned char flags)
{
	BUILD_BUG_ON(SWP_TB_FLAGS_BITS > BITS_PER_BYTE);
	VM_WARN_ON(flags >> SWP_TB_FLAGS_BITS);
	return ((unsigned long)flags) << SWP_TB_FLAGS_SHIFT;
}

/*
 * pfn_to_swp_tb() - 编码一个 swap-cache PFN 与 flags
 * 业务背景：swap cache 加入/迁移时用 PFN 替换 shadow，同时保留 count/zero flags。
 * 入参：pfn 是可由 pfn_folio() 反解的起始 PFN；flags 是未移位 count/zero 位。
 * 出参/返回：返回 PFN marker、PFN payload 和 flags 合成的状态字，无引用变化。
 * 注意事项：编译期验证位宽，运行期警告 PFN 侵入 flags；不验证 PFN 当前 folio 生命周期。
 */
static inline unsigned long pfn_to_swp_tb(unsigned long pfn, unsigned char flags)
{
	/* swp_tb 先承载 PFN 与低位 marker，最后再合并不重叠的高 flags。 */
	unsigned long swp_tb;

	BUILD_BUG_ON(sizeof(unsigned long) != sizeof(void *));
	BUILD_BUG_ON(SWAP_CACHE_PFN_BITS >
		     (BITS_PER_LONG - SWAP_CACHE_PFN_MARK_BITS - SWP_TB_FLAGS_BITS));

	swp_tb = (pfn << SWAP_CACHE_PFN_MARK_BITS) | SWP_TB_PFN_MARK;
	VM_WARN_ON_ONCE(swp_tb & SWP_TB_FLAGS_MASK);

	return swp_tb | __flags_to_swp_tb(flags);
}

/*
 * folio_to_swp_tb() - 以 folio 首 PFN 构造缓存状态字
 * 入参：folio 是调用者稳定的借用 folio；flags 同 pfn_to_swp_tb。
 * 出参/返回：返回编码值，不增 folio 引用；发布后生命周期由 swap-cache 协议另行持有。
 * 注意事项：大 folio 的每个 slot 通常由调用者分别编码相应 PFN，本 wrapper 只取首 PFN。
 */
static inline unsigned long folio_to_swp_tb(struct folio *folio, unsigned char flags)
{
	return pfn_to_swp_tb(folio_pfn(folio), flags);
}

/*
 * shadow_to_swp_tb() - 合成 workingset shadow/占位值与 flags
 * 入参：shadow 可为 NULL 或 xa_is_value() 编码的借用值；flags 是 count/zero 未移位值。
 * 出参/返回：返回保持 XA_VALUE payload、强制低位 shadow marker 并合并高 flags 的状态字。
 * 注意事项：shadow 不是拥有指针，无需释放；编译期验证 XA_VALUE 位宽/marker 完全兼容，
 * payload 侵入 flags 时警告。NULL 仍生成 marker=1 的有效 shadow 占位，不等于空槽。
 */
static inline unsigned long shadow_to_swp_tb(void *shadow, unsigned char flags)
{
	BUILD_BUG_ON((BITS_PER_XA_VALUE + 1) !=
		     BITS_PER_BYTE * sizeof(unsigned long));
	BUILD_BUG_ON((unsigned long)xa_mk_value(0) != SWP_TB_SHADOW_MARK);

	VM_WARN_ON_ONCE(shadow && !xa_is_value(shadow));
	VM_WARN_ON_ONCE(shadow && ((unsigned long)shadow & SWP_TB_FLAGS_MASK));

	return (unsigned long)shadow | SWP_TB_SHADOW_MARK | __flags_to_swp_tb(flags);
}

/*
 * Helpers for swap table entry type checking.
 */
/* 类型检查只观察 marker/常量，不获取 folio 引用或同步状态。 */
/*
 * swp_tb_is_null() - 识别可重新分配的空槽
 * 业务背景：分配/释放状态机以全零作为唯一 free 状态。入参：swp_tb 是原子快照。
 * 出参/返回：全零返回 true，否则 false，无副作用。注意事项：不验证 table 生命周期。
 */
static inline bool swp_tb_is_null(unsigned long swp_tb)
{
	return !swp_tb;
}

/*
 * swp_tb_is_folio() - 识别 swap-cache PFN 状态
 * 业务背景：查找者只有在 PFN marker 命中后才能反解 folio。入参：swp_tb 是原子快照。
 * 出参/返回：低 marker 位等于 10 返回 true，否则 false。注意事项：不取得 folio 引用。
 */
static inline bool swp_tb_is_folio(unsigned long swp_tb)
{
	return ((swp_tb & SWP_TB_PFN_MARK_MASK) == SWP_TB_PFN_MARK);
}

/*
 * swp_tb_is_shadow() - 识别已换出且未缓存的 shadow/占位状态
 * 业务背景：swapin 与 workingset 路径需区分 shadow payload。入参：swp_tb 是原子快照。
 * 出参/返回：xa_is_value() 认可低位 1 时返回 true。注意事项：不解析或拥有 payload。
 */
static inline bool swp_tb_is_shadow(unsigned long swp_tb)
{
	return xa_is_value((void *)swp_tb);
}

/*
 * swp_tb_is_bad() - 识别 swap header/设备洞保留槽
 * 业务背景：分配扫描必须跳过永久不可用位置。入参：swp_tb 是原子快照。
 * 出参/返回：精确等于 SWP_TB_BAD 返回 true。注意事项：普通高 flags 不会被误判。
 */
static inline bool swp_tb_is_bad(unsigned long swp_tb)
{
	return swp_tb == SWP_TB_BAD;
}

/*
 * swp_tb_is_countable() - 判断 entry 是否具有合法 count/flags 域
 * 入参：swp_tb 是单槽快照。返回：shadow、PFN 或 NULL 为 true，BAD/保留 pointer 为 false。
 * 注意事项：NULL 的 count=0 也可读取/构造；函数不证明类型与 count 的业务组合有效。
 */
static inline bool swp_tb_is_countable(unsigned long swp_tb)
{
	return (swp_tb_is_shadow(swp_tb) || swp_tb_is_folio(swp_tb) ||
		swp_tb_is_null(swp_tb));
}

/*
 * Helpers for retrieving info from swap table.
 */
/* 解码 helper 接受已由锁或原子读取取得的状态字，不自行延长其中 folio 的生命周期。 */
/*
 * swp_tb_to_folio() - 从 PFN entry 恢复 folio
 * 入参：swp_tb 应为 swp_tb_is_folio() 的状态字。
 * 出参/返回：清除高 flags、移除低 marker 后以 pfn_folio() 返回借用 folio；不增引用。
 * 注意事项：类型错误仅警告；调用者须以 cluster/swap-cache 协议保证 PFN 尚未被复用。
 */
static inline struct folio *swp_tb_to_folio(unsigned long swp_tb)
{
	VM_WARN_ON(!swp_tb_is_folio(swp_tb));
	return pfn_folio((swp_tb & ~SWP_TB_FLAGS_MASK) >> SWAP_CACHE_PFN_MARK_BITS);
}

/*
 * swp_tb_to_shadow() - 从 shadow entry 恢复原 XA_VALUE
 * 入参：swp_tb 应为 shadow 类型。返回：去除高 flags 后的借用 value 编码，无 ownership。
 * 注意事项：XA_VALUE 已按原低位格式存入，因此无需位移；类型错误仅 VM_WARN_ON。
 */
static inline void *swp_tb_to_shadow(unsigned long swp_tb)
{
	VM_WARN_ON(!swp_tb_is_shadow(swp_tb));
	/* No shift needed, xa_value is stored as it is in the lower bits. */
	/* shadow 的 XA_VALUE 低位保持原样，只需遮掉顶部 count/zero flags。 */
	return (void *)(swp_tb & ~SWP_TB_FLAGS_MASK);
}

/*
 * __swp_tb_get_count() - 读取可计数 entry 的内联 count
 * 入参：swp_tb 是 NULL/shadow/PFN 快照。返回：0..SWP_TB_COUNT_MAX 的 count。
 * 注意事项：max 可能只是 extend_table overflow 哨兵，真实大计数需由 swapfile.c 在锁下继续查。
 */
static inline unsigned char __swp_tb_get_count(unsigned long swp_tb)
{
	VM_WARN_ON(!swp_tb_is_countable(swp_tb));
	return ((swp_tb & SWP_TB_COUNT_MASK) >> SWP_TB_COUNT_SHIFT);
}

/*
 * __swp_tb_get_flags() - 读取完整高位 flags 域
 * 入参：swp_tb 是可计数快照。返回：右对齐的 count 加可选 zero bit，无副作用。
 * 注意事项：调用者若只需引用数应使用 count mask/helper，不能把 zero bit 当 count。
 */
static inline unsigned char __swp_tb_get_flags(unsigned long swp_tb)
{
	VM_WARN_ON(!swp_tb_is_countable(swp_tb));
	return ((swp_tb & SWP_TB_FLAGS_MASK) >> SWP_TB_FLAGS_SHIFT);
}

/*
 * swp_tb_get_count() - 对任意 entry 安全取得内联 count
 * 入参：swp_tb 是状态字快照。返回：可计数类型的非负 count；BAD/保留类型返回 -EINVAL。
 * 注意事项：与双下划线版本相比显式保留类型错误，调用者必须检查负值。
 */
static inline int swp_tb_get_count(unsigned long swp_tb)
{
	if (swp_tb_is_countable(swp_tb))
		return __swp_tb_get_count(swp_tb);
	return -EINVAL;
}

/*
 * __swp_tb_mk_count() - 仅替换状态字中的 count 位
 * 入参：swp_tb 是待保留类型/payload/zero 的可计数状态；count 是新内联计数。
 * 出参/返回：返回更新后的值，不发布到表。
 * 注意事项：调用者持 cluster lock 并负责 overflow/extend_table 状态机；越界由编码 helper 警告。
 */
static inline unsigned long __swp_tb_mk_count(unsigned long swp_tb, int count)
{
	return ((swp_tb & ~SWP_TB_COUNT_MASK) | __count_to_swp_tb(count));
}

/*
 * Helpers for accessing or modifying the swap table of a cluster,
 * the swap cluster must be locked.
 */
/* 下列 __ 访问器供持 ci->lock 的复合事务使用；无前缀 get 另提供 RCU 单槽快照。 */
/*
 * __swap_table_set() - 在 cluster 锁下覆盖一个槽
 * 入参：ci 是锁定 cluster；off 范围 [0,SWAPFILE_CLUSTER)；swp_tb 是完整新状态字。
 * 出参/返回：无直接返回；原子发布新值，旧值由调用者在写前自行读取。
 * 注意事项：protected RCU 解引用依赖 ci->lock，表必须已分配；不管理 folio/shadow ownership。
 */
static inline void __swap_table_set(struct swap_cluster_info *ci,
				    unsigned int off, unsigned long swp_tb)
{
	/* table 是锁保护下稳定的 RCU 指针，直到 cluster table 摘除事务结束。 */
	atomic_long_t *table = rcu_dereference_protected(ci->table, true);

	lockdep_assert_held(&ci->lock);
	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	atomic_long_set(&table[off], swp_tb);
}

/*
 * __swap_table_xchg() - 在 cluster 锁下原子替换槽并返回旧值
 * 入参：ci/off/new swp_tb 同 set。返回：交换前完整状态字，供状态转换/回滚判断。
 * 注意事项：ci->lock 已提供跨字段顺序，故 relaxed xchg 足够；表必须存在且 offset 合法。
 */
static inline unsigned long __swap_table_xchg(struct swap_cluster_info *ci,
					      unsigned int off, unsigned long swp_tb)
{
	atomic_long_t *table = rcu_dereference_protected(ci->table, true);

	lockdep_assert_held(&ci->lock);
	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	/* Ordering is guaranteed by cluster lock, relax */
	/* cluster lock 已为 swap_map/extend/memcg 等复合状态提供顺序，无需额外原子屏障。 */
	return atomic_long_xchg_relaxed(&table[off], swp_tb);
}

/*
 * __swap_table_get() - 在 cluster 锁或既有 RCU 读侧下读取一个槽
 * 入参：ci/off 是借用 cluster 与下标。返回：atomic_long 单槽快照。
 * 注意事项：rcu_dereference_check 要求调用者持 ci->lock 或处于合法 RCU 条件；若 table
 * 可为 NULL 的路径应改用 swap_table_get()，本函数直接解引用且不延长 folio 生命周期。
 */
static inline unsigned long __swap_table_get(struct swap_cluster_info *ci,
					     unsigned int off)
{
	atomic_long_t *table;

	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	table = rcu_dereference_check(ci->table, lockdep_is_held(&ci->lock));

	return atomic_long_read(&table[off]);
}

/*
 * swap_table_get() - 无 cluster 锁取得 RCU 安全的单槽快照
 * 入参：ci 是生命周期稳定的借用 cluster；off 是槽下标。
 * 出参/返回：表存在时返回原子值，尚未分配/已摘除时返回 NULL entry；无错误码。
 * 注意事项：RCU 只保证 table 内存读侧不释放，不保证返回后的状态不变，也不为 PFN folio
 * 取得引用；消费者若需长期使用必须 try_get 并按协议复核。
 */
static inline unsigned long swap_table_get(struct swap_cluster_info *ci,
					unsigned int off)
{
	/* table 仅在 RCU 临界区借用，swp_tb 把需要的信息复制到本地后再退出。 */
	atomic_long_t *table;
	unsigned long swp_tb;

	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);

	rcu_read_lock();
	/* free_table 先 rcu_assign_pointer(NULL)，旧页/folio 延迟到 grace period 后释放。 */
	table = rcu_dereference(ci->table);
	swp_tb = table ? atomic_long_read(&table[off]) : null_to_swp_tb();
	rcu_read_unlock();

	return swp_tb;
}

/*
 * __swap_table_set_zero() - 标记一个 swap slot 的内容为全零页
 * 业务背景：swap writeout 可省略实际 I/O，并在 swapin 时重建零页；架构位宽足够时标志
 * 内嵌 entry，否则使用 cluster 的平行 bitmap，语义保持一致。
 * 入参：ci 是已持 lock 的输入输出 cluster；ci_off 是有效槽下标。
 * 出参/返回：无直接返回；设置 zero flag/bitmap bit，保留类型、count、PFN/shadow 其余状态。
 * 注意事项：inline 分支经 __get/__set 依赖 cluster lock；状态必须可计数。配置在编译期选择。
 */
static inline void __swap_table_set_zero(struct swap_cluster_info *ci,
					 unsigned int ci_off)
{
#if SWAP_TABLE_HAS_ZEROFLAG
	/* 在完整状态字上读改写，cluster lock 防止并发 count/type 更新丢失。 */
	unsigned long swp_tb = __swap_table_get(ci, ci_off);

	BUILD_BUG_ON(SWP_TB_ZERO_FLAG & ~SWP_TB_FLAGS_MASK);
	VM_WARN_ON(!swp_tb_is_countable(swp_tb));
	swp_tb |= SWP_TB_ZERO_FLAG;
	__swap_table_set(ci, ci_off, swp_tb);
#else
	/* 位宽不足时单独 bitmap 仍由同一 cluster lock 与 entry 状态保持同步。 */
	lockdep_assert_held(&ci->lock);
	__set_bit(ci_off, ci->zero_bitmap);
#endif
}

/*
 * __swap_table_test_zero() - 查询 slot 是否使用零页优化
 * 入参：ci 是锁定 cluster；ci_off 是有效下标。
 * 出参/返回：inline flag 或 zero_bitmap 已设置返回 true，否则 false；无状态变化。
 * 注意事项：调用者须持 ci->lock，使 zero 状态与 count/type 作为同一事务观察。
 */
static inline bool __swap_table_test_zero(struct swap_cluster_info *ci,
					  unsigned int ci_off)
{
#if SWAP_TABLE_HAS_ZEROFLAG
	/* inline 读取先验证 entry 类型，避免把 BAD 高位误解释为 zero。 */
	unsigned long swp_tb = __swap_table_get(ci, ci_off);

	VM_WARN_ON(!swp_tb_is_countable(swp_tb));
	return !!(swp_tb & SWP_TB_ZERO_FLAG);
#else
	/* bitmap 生命周期和索引由已分配 cluster table/lock 保证。 */
	return test_bit(ci_off, ci->zero_bitmap);
#endif
}

/*
 * __swap_table_clear_zero() - 撤销 slot 的零页优化标记
 * 入参：ci 是锁定 cluster；ci_off 是有效下标。
 * 出参/返回：无直接返回；清除 inline flag/bitmap bit，保留其他状态字内容。
 * 注意事项：与 set/test 共享配置与锁契约，常在槽释放或真实写回取代 zero 状态时调用。
 */
static inline void __swap_table_clear_zero(struct swap_cluster_info *ci,
					   unsigned int ci_off)
{
#if SWAP_TABLE_HAS_ZEROFLAG
	/* 锁内 read-modify-write 避免覆盖同期 count 或 shadow/PFN 转换。 */
	unsigned long swp_tb = __swap_table_get(ci, ci_off);

	VM_WARN_ON(!swp_tb_is_countable(swp_tb));
	swp_tb &= ~SWP_TB_ZERO_FLAG;
	__swap_table_set(ci, ci_off, swp_tb);
#else
	/* 外置 bitmap 分支显式断言 cluster lock。 */
	lockdep_assert_held(&ci->lock);
	__clear_bit(ci_off, ci->zero_bitmap);
#endif
}

#ifdef CONFIG_MEMCG
/*
 * __swap_cgroup_set() - 给一段连续 swap slot 写入同一 memcg 私有 ID
 * 业务背景：换出大 folio/批次时，shadow entry 的回收记账需能恢复原 memcg。
 * 入参：ci 是锁定 cluster；ci_off 是首下标；nr 是非零连续槽数且不得越界；id 是待写 ID。
 * 出参/返回：无直接返回；逐槽覆盖 memcg_table，表缺失时警告并无副作用。
 * 注意事项：调用者保证整个范围同 cluster 且未与并发释放竞争；不取得 memcg/css 引用。
 */
static inline void __swap_cgroup_set(struct swap_cluster_info *ci,
		unsigned int ci_off, unsigned long nr, unsigned short id)
{
	lockdep_assert_held(&ci->lock);
	VM_WARN_ON_ONCE(ci_off >= SWAPFILE_CLUSTER);
	if (WARN_ON_ONCE(!ci->memcg_table))
		return;
	/* do/while 要求 nr>0；每轮同步推进同一 cluster 下标。 */
	do {
		ci->memcg_table->id[ci_off++] = id;
	} while (--nr);
}

/*
 * __swap_cgroup_get() - 读取单个 swap slot 的 memcg 私有 ID
 * 入参：ci 是锁定 cluster；ci_off 是有效下标。
 * 出参/返回：表存在时返回 ID，memcg 禁用/表未分配时返回 0；无引用与副作用。
 * 注意事项：ID 只是记账键，不保证对应 memcg 对象仍在线，解释由 memcontrol 路径完成。
 */
static inline unsigned short __swap_cgroup_get(struct swap_cluster_info *ci,
					       unsigned int ci_off)
{
	lockdep_assert_held(&ci->lock);
	VM_WARN_ON_ONCE(ci_off >= SWAPFILE_CLUSTER);
	if (unlikely(!ci->memcg_table))
		return 0;
	return ci->memcg_table->id[ci_off];
}

/*
 * __swap_cgroup_clear() - 清除一段连续槽并返回其共同旧 memcg ID
 * 业务背景：批量释放 swap 记账时调用者需要一次取回 ID，并验证整批原属同一 memcg。
 * 入参：ci 是锁定 cluster；ci_off 为首下标；nr 是非零且不越界的连续槽数。
 * 出参/返回：首槽 ID 为 0 时不改表并返回 0；否则逐槽清零并返回旧 ID。
 * 注意事项：后续槽 ID 不同只警告仍会清零；do/while 要求 nr>0，调用者负责范围合法。
 */
static inline unsigned short __swap_cgroup_clear(struct swap_cluster_info *ci,
						 unsigned int ci_off,
						 unsigned long nr)
{
	/* old 作为整批期望 ID，也决定是否存在需要撤销的 memcg 记账。 */
	unsigned short old = __swap_cgroup_get(ci, ci_off);

	if (!old)
		return 0;
	/* 锁内验证一致性并清零，防止释放后 ID 泄漏到下一次槽分配。 */
	do {
		VM_WARN_ON_ONCE(ci->memcg_table->id[ci_off] != old);
		ci->memcg_table->id[ci_off++] = 0;
	} while (--nr);

	return old;
}
#else
/*
 * CONFIG_MEMCG 关闭时三个 stub 保持调用点无需条件编译：set 无副作用，get/clear 返回 0。
 * 参数均为借用且不访问，因此无需 cluster lock，但调用者通常仍遵循启用配置的同一契约。
 */
/*
 * __swap_cgroup_set() - CONFIG_MEMCG 关闭时的无操作记账桩
 * 业务背景：保持 swap 调用点跨配置一致。入参：ci/off/nr/id 均借用且忽略。
 * 出参/返回：无直接返回且无副作用。注意事项：不访问参数，因而不要求实际表或锁。
 */
static inline void __swap_cgroup_set(struct swap_cluster_info *ci,
		unsigned int ci_off, unsigned long nr, unsigned short id)
{
}

/*
 * __swap_cgroup_get() - CONFIG_MEMCG 关闭时返回无归属 ID
 * 业务背景：让读取者把所有槽视为无 memcg 记账。入参：ci/off 借用且忽略。
 * 出参/返回：固定返回 0，无副作用。注意事项：不证明 cluster/table 是否存在。
 */
static inline unsigned short __swap_cgroup_get(struct swap_cluster_info *ci,
					       unsigned int ci_off)
{
	return 0;
}

/*
 * __swap_cgroup_clear() - CONFIG_MEMCG 关闭时的无操作清理桩
 * 业务背景：释放路径仍可取得统一“旧 ID”。入参：ci/off/nr 借用且忽略。
 * 出参/返回：固定返回 0，无状态变化。注意事项：不执行范围/锁检查。
 */
static inline unsigned short __swap_cgroup_clear(struct swap_cluster_info *ci,
						 unsigned int ci_off,
						 unsigned long nr)
{
	return 0;
}
#endif

#endif
