// SPDX-License-Identifier: GPL-2.0
/*
 * SLUB: A slab allocator with low overhead percpu array caches and mostly
 * lockless freeing of objects to slabs in the slowpath.
 *
 * The allocator synchronizes using spin_trylock for percpu arrays in the
 * fastpath, and cmpxchg_double (or bit spinlock) for slowpath freeing.
 * Uses a centralized lock to manage a pool of partial slabs.
 *
 * (C) 2007 SGI, Christoph Lameter
 * (C) 2011 Linux Foundation, Christoph Lameter
 * (C) 2025 SUSE, Vlastimil Babka
 *
 * ============================================================
 * 【SLUB 分配器总体设计】
 *
 * SLUB 解决两个核心问题：
 *   1. 碎片：伙伴系统最小粒度 4KB，小对象浪费严重；SLUB 将一页切成
 *      等大槽位，多个对象共享同一页，消除内部碎片。
 *   2. 性能：per-CPU sheaf（对象指针数组）作为一级缓存，分配/释放只
 *      做数组下标增减，无全局锁，极低开销。
 *
 * 三层内存层次：
 *   Layer 1  per-CPU sheaf（main/spare）
 *            local_trylock 保护，失败即退出，不自旋
 *            分配 = objects[--size]，释放 = objects[size++]
 *
 *   Layer 2  per-NUMA-node barn + partial list
 *            barn：满/空 sheaf 的中转仓库（spinlock）
 *            partial list：部分空闲的 slab 页链表（list_lock）
 *
 *   Layer 3  伙伴系统（buddy allocator）
 *            只在 Layer 2 也耗尽时才申请新页
 *
 * 关键锁序（从外到内）：
 *   cpu_hotplug_lock > slab_mutex > cpu_sheaves->lock
 *   > barn->lock > node->list_lock > slab_lock > object_map_lock
 *
 * slab 状态机（SL_partial / full / frozen 三字段组合）：
 *   SL_partial && !full && !frozen  → 在 per-node partial list 上
 *  !SL_partial && !full && !frozen  → 从 partial list 摘下，正在处理
 *  !SL_partial &&  full && !frozen  → 满载，不在任何链表
 *  !SL_partial &&  full &&  frozen  → 一致性检查失败，冻结（泄漏对象）
 * ============================================================
 */

#include <linux/mm.h>
#include <linux/swap.h> /* mm_account_reclaimed_pages() */
#include <linux/module.h>
#include <linux/bit_spinlock.h>
#include <linux/interrupt.h>
#include <linux/swab.h>
#include <linux/bitops.h>
#include <linux/slab.h>
#include "slab.h"
#include <linux/vmalloc.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/kasan.h>
#include <linux/node.h>
#include <linux/kmsan.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/mempolicy.h>
#include <linux/ctype.h>
#include <linux/stackdepot.h>
#include <linux/debugobjects.h>
#include <linux/kallsyms.h>
#include <linux/kfence.h>
#include <linux/memory.h>
#include <linux/math64.h>
#include <linux/fault-inject.h>
#include <linux/kmemleak.h>
#include <linux/stacktrace.h>
#include <linux/prefetch.h>
#include <linux/memcontrol.h>
#include <linux/random.h>
#include <linux/prandom.h>
#include <kunit/test.h>
#include <kunit/test-bug.h>
#include <linux/sort.h>
#include <linux/irq_work.h>
#include <linux/kprobes.h>
#include <linux/debugfs.h>
#include <trace/events/kmem.h>

#include "internal.h"

/*
 * Lock order:
 *   0.  cpu_hotplug_lock
 *   1.  slab_mutex (Global Mutex)
 *   2a. kmem_cache->cpu_sheaves->lock (Local trylock)
 *   2b. barn->lock (Spinlock)
 *   2c. node->list_lock (Spinlock)
 *   3.  slab_lock(slab) (Only on some arches)
 *   4.  object_map_lock (Only for debugging)
 *
 *   slab_mutex
 *
 *   The role of the slab_mutex is to protect the list of all the slabs
 *   and to synchronize major metadata changes to slab cache structures.
 *   Also synchronizes memory hotplug callbacks.
 *
 *   slab_lock
 *
 *   The slab_lock is a wrapper around the page lock, thus it is a bit
 *   spinlock.
 *
 *   The slab_lock is only used on arches that do not have the ability
 *   to do a cmpxchg_double. It only protects:
 *
 *	A. slab->freelist	-> List of free objects in a slab
 *	B. slab->inuse		-> Number of objects in use
 *	C. slab->objects	-> Number of objects in slab
 *	D. slab->frozen		-> frozen state
 *
 *   SL_partial slabs
 *
 *   Slabs on node partial list have at least one free object. A limited number
 *   of slabs on the list can be fully free (slab->inuse == 0), until we start
 *   discarding them. These slabs are marked with SL_partial, and the flag is
 *   cleared while removing them, usually to grab their freelist afterwards.
 *   This clearing also exempts them from list management. Please see
 *   __slab_free() for more details.
 *
 *   Full slabs
 *
 *   For caches without debugging enabled, full slabs (slab->inuse ==
 *   slab->objects and slab->freelist == NULL) are not placed on any list.
 *   The __slab_free() freeing the first object from such a slab will place
 *   it on the partial list. Caches with debugging enabled place such slab
 *   on the full list and use different allocation and freeing paths.
 *
 *   Frozen slabs
 *
 *   If a slab is frozen then it is exempt from list management. It is used to
 *   indicate a slab that has failed consistency checks and thus cannot be
 *   allocated from anymore - it is also marked as full. Any previously
 *   allocated objects will be simply leaked upon freeing instead of attempting
 *   to modify the potentially corrupted freelist and metadata.
 *
 *   To sum up, the current scheme is:
 *   - node partial slab:            SL_partial && !full && !frozen
 *   - taken off partial list:      !SL_partial && !full && !frozen
 *   - full slab, not on any list:  !SL_partial &&  full && !frozen
 *   - frozen due to inconsistency: !SL_partial &&  full &&  frozen
 *
 *   node->list_lock (spinlock)
 *
 *   The list_lock protects the partial and full list on each node and
 *   the partial slab counter. If taken then no new slabs may be added or
 *   removed from the lists nor make the number of partial slabs be modified.
 *   (Note that the total number of slabs is an atomic value that may be
 *   modified without taking the list lock).
 *
 *   The list_lock is a centralized lock and thus we avoid taking it as
 *   much as possible. As long as SLUB does not have to handle partial
 *   slabs, operations can continue without any centralized lock.
 *
 *   For debug caches, all allocations are forced to go through a list_lock
 *   protected region to serialize against concurrent validation.
 *
 *   cpu_sheaves->lock (local_trylock)
 *
 *   This lock protects fastpath operations on the percpu sheaves. On !RT it
 *   only disables preemption and does no atomic operations. As long as the main
 *   or spare sheaf can handle the allocation or free, there is no other
 *   overhead.
 *
 *   barn->lock (spinlock)
 *
 *   This lock protects the operations on per-NUMA-node barn. It can quickly
 *   serve an empty or full sheaf if available, and avoid more expensive refill
 *   or flush operation.
 *
 *   Lockless freeing
 *
 *   Objects may have to be freed to their slabs when they are from a remote
 *   node (where we want to avoid filling local sheaves with remote objects)
 *   or when there are too many full sheaves. On architectures supporting
 *   cmpxchg_double this is done by a lockless update of slab's freelist and
 *   counters, otherwise slab_lock is taken. This only needs to take the
 *   list_lock if it's a first free to a full slab, or when a slab becomes empty
 *   after the free.
 *
 *   irq, preemption, migration considerations
 *
 *   Interrupts are disabled as part of list_lock or barn lock operations, or
 *   around the slab_lock operation, in order to make the slab allocator safe
 *   to use in the context of an irq.
 *   Preemption is disabled as part of local_trylock operations.
 *   kmalloc_nolock() and kfree_nolock() are safe in NMI context but see
 *   their limitations.
 *
 * SLUB assigns two object arrays called sheaves for caching allocations and
 * frees on each cpu, with a NUMA node shared barn for balancing between cpus.
 * Allocations and frees are primarily served from these sheaves.
 *
 * Slabs with free elements are kept on a partial list and during regular
 * operations no list for full slabs is used. If an object in a full slab is
 * freed then the slab will show up again on the partial lists.
 * We track full slabs for debugging purposes though because otherwise we
 * cannot scan all objects.
 *
 * Slabs are freed when they become empty. Teardown and setup is minimal so we
 * rely on the page allocators per cpu caches for fast frees and allocs.
 *
 * SLAB_DEBUG_FLAGS	Slab requires special handling due to debug
 * 			options set. This moves	slab handling out of
 * 			the fast path and disables lockless freelists.
 */

/**
 * enum slab_flags - slab 页标志位的含义
 * @SL_locked:    该 slab 已持有 slab_lock()（bit spinlock，仅不支持
 *                cmpxchg_double 的架构使用）
 * @SL_partial:   该 slab 挂在 per-node partial list 上（有空闲对象）
 * @SL_pfmemalloc:该 slab 页是从 PF_MEMALLOC 紧急储备分配的，只能
 *                回收给同样设置了 __GFP_MEMALLOC 的调用方
 *
 * slab 标志位复用 page 的 flags 字段，但语义不同（借用历史遗留 bit）。
 * 高位仍用于 zone/node/section 编码，不受影响。
 */
enum slab_flags {
	SL_locked = PG_locked,
	SL_partial = PG_workingset,	/* Historical reasons for this bit */
	SL_pfmemalloc = PG_active,	/* Historical reasons for this bit */
};

/*
 * __fastpath_inline：在完整内核（非 SLUB_TINY）中将快速路径函数强制内联，
 * 减少函数调用开销；SLUB_TINY 为嵌入式场景优化代码体积，不强制内联。
 */
#ifndef CONFIG_SLUB_TINY
#define __fastpath_inline __always_inline
#else
#define __fastpath_inline
#endif

#ifdef CONFIG_SLUB_DEBUG
#ifdef CONFIG_SLUB_DEBUG_ON
DEFINE_STATIC_KEY_TRUE(slub_debug_enabled);
#else
DEFINE_STATIC_KEY_FALSE(slub_debug_enabled);
#endif
#endif		/* CONFIG_SLUB_DEBUG */

#ifdef CONFIG_NUMA
static DEFINE_STATIC_KEY_FALSE(strict_numa);
#endif

/*
 * slab_alloc_context - 单次分配调用的附加参数
 *
 * 将零散的分配参数打包成结构体，避免函数签名随特性增减而频繁变化。
 * 由调用方在栈上构造，传入 slab_alloc_node() 及其下游函数。
 *
 * @caller_addr:  调用方的返回地址（_RET_IP_），用于 tracepoint 和
 *               KASAN 报告时定位分配来源
 * @orig_size:    用户请求的原始字节数（未经对齐），用于 KASAN 细粒度
 *               越界检测（可发现 [orig_size, object_size) 范围内的越界）
 * @alloc_flags:  SLAB_ALLOC_* 内部标志（如 SLAB_ALLOC_DEFAULT）
 * @lru:          非 NULL 时将分配的对象关联到该 list_lru（memcg LRU
 *               回收用，由 kmem_cache_alloc_lru 使用）
 */
struct slab_alloc_context {
	unsigned long caller_addr;
	size_t orig_size;
	unsigned int alloc_flags;
	struct list_lru *lru;
};

/*
 * partial_bulk_context - get_partial_node_bulk() 的批量参数
 *
 * 批量从 partial list 取对象时，一次性指定数量范围和 GFP 标志，
 * 减少多次调用的锁开销。
 *
 * @flags:       GFP 分配标志
 * @min_objects: 至少要取的对象数（低于此数时继续尝试）
 * @max_objects: 最多取的对象数（超过后停止）
 * @slabs:       输出链表，收集取出的 slab 页
 */
struct partial_bulk_context {
	gfp_t flags;
	unsigned int min_objects;
	unsigned int max_objects;
	struct list_head slabs;
};

/*
 * slab_obj_iter - slab 内对象迭代器
 *
 * 用于遍历一个 slab 页内的所有对象槽位（调试、shrink 等场景）。
 * SLAB_FREELIST_RANDOM 开启时，遍历顺序与初始随机化顺序一致。
 *
 * @pos:            当前遍历位置（对象索引或字节偏移）
 * @start:          slab 内存起始地址
 * @freelist_count: 随机模式下 freelist 总条目数
 * @page_limit:     随机模式下的页边界
 * @random:         true 表示使用随机化遍历顺序
 */
struct slab_obj_iter {
	unsigned long pos;
	void *start;
#ifdef CONFIG_SLAB_FREELIST_RANDOM
	unsigned long freelist_count;
	unsigned long page_limit;
	bool random;
#endif
};

/* 判断该 cache 是否开启了任意调试标志（SLAB_DEBUG_FLAGS 是调试相关标志的掩码） */
static inline bool kmem_cache_debug(struct kmem_cache *s)
{
	return kmem_cache_debug_flags(s, SLAB_DEBUG_FLAGS);
}

/*
 * fixup_red_left - 将指针从 slab 页起始调整到对象用户区起始
 *
 * 开启 SLAB_RED_ZONE 时，每个对象左侧有 red_left_pad 字节的 red zone
 * 填充区（用于检测左侧越界写）。slab 内存从 red zone 开始，
 * 用户可见的对象地址需跳过这段填充。
 *
 * 不开启调试时 red_left_pad == 0，该函数为 no-op。
 */
void *fixup_red_left(struct kmem_cache *s, void *p)
{
	if (kmem_cache_debug_flags(s, SLAB_RED_ZONE))
		p += s->red_left_pad;

	return p;
}

/*
 * Issues still to be resolved:
 *
 * - Support PAGE_ALLOC_DEBUG. Should be easy to do.
 *
 * - Variable sizing of the per node arrays
 */

/* Enable to log cmpxchg failures */
#undef SLUB_DEBUG_CMPXCHG

#ifndef CONFIG_SLUB_TINY
/*
 * MIN_PARTIAL：per-node partial list 上至少保留的 slab 数量。
 * 即使 slab 全空也不立即归还给伙伴系统，避免频繁申请/释放页的开销。
 * kmem_cache_shrink() 可以主动回收这部分"备用"空 slab。
 */
#define MIN_PARTIAL 5

/*
 * MAX_PARTIAL：per-node partial list 上"理想"的最大 slab 数量。
 * 超过此阈值时，kmem_cache_shrink() 会对 partial list 按 inuse 排序，
 * 将接近满的 slab 提前，尽量让空 slab 被回收。
 */
#define MAX_PARTIAL 10
#else
/* SLUB_TINY 不保留任何空 slab，内存最省但释放更积极 */
#define MIN_PARTIAL 0
#define MAX_PARTIAL 0
#endif

/* 调试模式默认开启的标志组合：一致性检查 + 左右 red zone + poison + 调用栈记录 */
#define DEBUG_DEFAULT_FLAGS (SLAB_CONSISTENCY_CHECKS | SLAB_RED_ZONE | \
				SLAB_POISON | SLAB_STORE_USER)

/*
 * SLAB_NO_CMPXCHG：这些调试标志不能与 cmpxchg_double 路径同用。
 * 原因：调试路径需要读/写对象内的元数据（红区、调用栈），
 * 而 cmpxchg_double 路径假设 freelist 操作是原子的，两者并发会引发
 * 一致性问题，因此开启这些标志时强制走加锁的慢速路径。
 */
#define SLAB_NO_CMPXCHG (SLAB_CONSISTENCY_CHECKS | SLAB_STORE_USER | \
				SLAB_TRACE)


/*
 * DEBUG_METADATA_FLAGS：需要在 slab 对象内存储额外元数据的调试标志。
 * 当 slab_debug=O（优化模式）且增加元数据会导致 slab order 变大时，
 * 这些标志会被自动禁用以避免内存浪费。
 */
#define DEBUG_METADATA_FLAGS (SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER)

/*
 * OO_SHIFT / OO_MASK：将 order 和 objects_per_slab 编码到同一个 u32 中。
 *   高 16 位（>= OO_SHIFT）存 order（2^order 页）
 *   低 16 位（< OO_SHIFT，即 & OO_MASK）存该 slab 内对象数
 * 用 oo_make/oo_order/oo_objects 编码/解码。
 */
#define OO_SHIFT	16
#define OO_MASK		((1 << OO_SHIFT) - 1)
/* slab.objects 字段是 u15，最多 32767 个对象 */
#define MAX_OBJS_PER_PAGE	32767 /* since slab.objects is u15 */

/* __OBJECT_POISON：内部标志，标记该 cache 的对象在释放时需要 poison 填充 */
#define __OBJECT_POISON		__SLAB_FLAG_BIT(_SLAB_OBJECT_POISON)

/*
 * __CMPXCHG_DOUBLE：内部标志，标记该 cache 可使用 cmpxchg_double
 * （即 freelist_aba）原子地同时更新 freelist 指针和计数器。
 * 不支持该指令的架构（__CMPXCHG_DOUBLE = UNUSED）则退回到 slab_lock
 * bit-spinlock 保护。
 */
#ifdef system_has_freelist_aba
#define __CMPXCHG_DOUBLE	__SLAB_FLAG_BIT(_SLAB_CMPXCHG_DOUBLE)
#else
#define __CMPXCHG_DOUBLE	__SLAB_FLAG_UNUSED
#endif

/*
 * struct track - 对象分配/释放的调用栈追踪记录
 *
 * 开启 SLAB_STORE_USER 时，每个对象尾部存储两个 track 结构（ALLOC + FREE），
 * 记录最近一次分配和释放的现场信息，供 slabinfo / KASAN 报告使用。
 *
 * TRACK_ADDRS_COUNT：保存调用栈的最大层数（未使用 stackdepot 时的数组大小）。
 *
 * @addr:   调用方的指令地址（通常是 _RET_IP_）
 * @handle: stackdepot 句柄，指向压缩保存的完整调用栈
 * @cpu:    发生操作时运行的 CPU 编号
 * @pid:    发生操作时的进程 PID
 * @when:   发生操作时的 jiffies 时间戳
 */
#define TRACK_ADDRS_COUNT 16
struct track {
	unsigned long addr;	/* Called from address */
#ifdef CONFIG_STACKDEPOT
	depot_stack_handle_t handle;
#endif
	int cpu;		/* Was running on cpu */
	int pid;		/* Pid context */
	unsigned long when;	/* When did the operation occur */
};

/* TRACK_ALLOC：记录分配时的现场；TRACK_FREE：记录释放时的现场 */
enum track_item { TRACK_ALLOC, TRACK_FREE };

#ifdef SLAB_SUPPORTS_SYSFS
static int sysfs_slab_add(struct kmem_cache *);
#else
static inline int sysfs_slab_add(struct kmem_cache *s) { return 0; }
#endif

#if defined(CONFIG_DEBUG_FS) && defined(CONFIG_SLUB_DEBUG)
static void debugfs_slab_add(struct kmem_cache *);
#else
static inline void debugfs_slab_add(struct kmem_cache *s) { }
#endif

/* add_partial() 将 slab 加入 partial list 的位置：头部（优先被分配）或尾部（最后被用） */
enum add_mode {
	ADD_TO_HEAD,
	ADD_TO_TAIL,
};

/*
 * enum stat_item - SLUB 性能统计计数器
 *
 * 开启 CONFIG_SLUB_STATS 时，每个 per-CPU 的 kmem_cache_stats 结构
 * 维护一组这些计数器，通过 /sys/kernel/slab/<cache>/stats 可读。
 * 正常内核不开启（零开销），调优时开启以定位瓶颈。
 */
enum stat_item {
	ALLOC_FASTPATH,		/* 从 per-CPU sheaf 直接弹出对象（理想路径） */
	ALLOC_SLOWPATH,		/* sheaf 空，从 partial list 或新 slab 取对象 */
	FREE_RCU_SHEAF,		/* 对象放入 rcu_free sheaf（kfree_rcu 批量路径） */
	FREE_RCU_SHEAF_FAIL,	/* rcu_free sheaf 不可用，退回普通释放 */
	FREE_FASTPATH,		/* 对象推入 per-CPU sheaf（理想路径） */
	FREE_SLOWPATH,		/* sheaf 满，对象直接写回 slab freelist */
	FREE_ADD_PARTIAL,	/* 释放后 slab 从满 → 部分空，加入 partial list */
	FREE_REMOVE_PARTIAL,	/* 释放后 slab 全空，从 partial list 移除 */
	ALLOC_SLAB,		/* 向伙伴系统申请了新 slab 页 */
	ALLOC_NODE_MISMATCH,	/* 快速路径：请求的 NUMA 节点与当前 CPU 不匹配 */
	FREE_SLAB,		/* slab 页全空，归还给伙伴系统 */
	ORDER_FALLBACK,		/* 理想 order 申请失败，降级到 min order */
	CMPXCHG_DOUBLE_FAIL,	/* slab freelist 的 CAS 操作失败（需重试） */
	SHEAF_FLUSH,		/* sheaf 中的对象被刷回 slab（flush_all 等） */
	SHEAF_REFILL,		/* sheaf 被从 partial list / 伙伴系统填满 */
	SHEAF_ALLOC,		/* 分配了新的空 sheaf 结构（含超大 sheaf） */
	SHEAF_FREE,		/* 释放了空 sheaf 结构 */
	BARN_GET,		/* 从 barn 成功取到满 sheaf */
	BARN_GET_FAIL,		/* barn 中没有满 sheaf */
	BARN_PUT,		/* 向 barn 成功存入满 sheaf */
	BARN_PUT_FAIL,		/* barn 已满，无法存入 */
	SHEAF_PREFILL_FAST,	/* prefill 时直接复用了 spare sheaf */
	SHEAF_PREFILL_SLOW,	/* prefill 时没有 spare，需分配新 sheaf */
	SHEAF_PREFILL_OVERSIZE,	/* prefill 分配了超出标准容量的大 sheaf */
	SHEAF_RETURN_FAST,	/* sheaf 归还时成功重新挂为 spare */
	SHEAF_RETURN_SLOW,	/* sheaf 归还时 spare 槽位已占用，只能释放 */
	NR_SLUB_STAT_ITEMS
};

#ifdef CONFIG_SLUB_STATS
/* per-CPU 统计数组，每个 stat_item 一个计数器 */
struct kmem_cache_stats {
	unsigned int stat[NR_SLUB_STAT_ITEMS];
};
#endif

/*
 * stat() / stat_add() - 记录一次统计事件
 *
 * 使用 raw_cpu_inc/add 而非 this_cpu_*，因为轻微的竞态是可接受的
 * （统计仅用于调优参考，不影响正确性），避免 this_cpu_* 的关中断开销。
 * CONFIG_SLUB_STATS 未开启时编译为空函数，零开销。
 */
static inline void stat(const struct kmem_cache *s, enum stat_item si)
{
#ifdef CONFIG_SLUB_STATS
	/*
	 * The rmw is racy on a preemptible kernel but this is acceptable, so
	 * avoid this_cpu_add()'s irq-disable overhead.
	 */
	raw_cpu_inc(s->cpu_stats->stat[si]);
#endif
}

static inline
void stat_add(const struct kmem_cache *s, enum stat_item si, int v)
{
#ifdef CONFIG_SLUB_STATS
	raw_cpu_add(s->cpu_stats->stat[si], v);
#endif
}

/*
 * per-node barn（仓库）中满/空 sheaf 的数量上限。
 * 超过上限时不再存入 barn，由调用方直接 refill 或释放，
 * 避免 barn 无限增长占用内存。
 */
#define MAX_FULL_SHEAVES	10
#define MAX_EMPTY_SHEAVES	10

/*
 * struct node_barn - per-NUMA-node 的 sheaf 中转仓库
 *
 * 位于 Layer 1（per-CPU sheaf）和 Layer 2（per-node partial list）之间，
 * 作为缓冲层减少直接访问 slab freelist 的频率。
 *
 * 当 per-CPU 的 main sheaf 耗尽（分配）或填满（释放）时：
 *   - 先尝试与 barn 交换满/空 sheaf（只需一次 spinlock）
 *   - barn 也没有时才走更慢的 partial list / 伙伴系统路径
 *
 * @lock:         保护链表操作的自旋锁
 * @sheaves_full: 满载 sheaf 链表（可直接用于分配）
 * @sheaves_empty:空 sheaf 链表（可直接用于接收释放的对象）
 * @nr_full:      当前满 sheaf 数量
 * @nr_empty:     当前空 sheaf 数量
 */
struct node_barn {
	spinlock_t lock;
	struct list_head sheaves_full;
	struct list_head sheaves_empty;
	unsigned int nr_full;
	unsigned int nr_empty;
};

/*
 * struct slab_sheaf - 对象指针束（sheaf）
 *
 * 核心数据结构：一个定长的对象指针数组，像栈一样操作。
 * 分配从尾部弹出（objects[--size]），释放向尾部压入（objects[size++]）。
 *
 * 同一个 slab_sheaf 在不同生命周期阶段有不同的使用模式：
 *
 * 1. 作为 per-CPU 的 main/spare sheaf（最常见）：
 *    union 字段未使用，objects[] 持有对象指针，size 表示当前数量。
 *
 * 2. 挂在 barn 链表上（barn_list）：
 *    通过 barn_list 节点挂入 node_barn.sheaves_full/empty。
 *
 * 3. 作为 rcu_free sheaf（kfree_rcu 批量延迟释放）：
 *    通过 rcu_head 在 RCU 宽限期后触发 rcu_free_sheaf 回调。
 *    node 字段记录对象来自哪个 NUMA 节点，释放时归还到正确节点。
 *
 * 4. 作为 prefilled sheaf（kmem_cache_prefill_sheaf）：
 *    capacity 记录本次预填充的容量上限，pfmemalloc 标记来源。
 *
 * @cache:    所属 kmem_cache（used by rcu_free sheaf 在回调时定位 cache）
 * @size:     objects[] 中当前有效对象数（0 ~ sheaf_capacity）
 * @node:     rcu_free sheaf 专用：对象来自哪个 NUMA 节点
 * @objects[]:柔性数组，存储对象指针
 */
struct slab_sheaf {
	union {
		struct rcu_head rcu_head;
		struct list_head barn_list;
		/* only used for prefilled sheafs */
		struct {
			unsigned int capacity;
			bool pfmemalloc;
		};
	};
	struct kmem_cache *cache;
	unsigned int size;
	int node; /* only used for rcu_sheaf */
	void *objects[];
};

/*
 * struct slub_percpu_sheaves - per-CPU sheaf 集合（Layer 1 快速路径核心）
 *
 * 每个 CPU 对每个 kmem_cache 维护一个此结构，通过 s->cpu_sheaves 访问。
 * 所有操作在 local_trylock 保护下进行：失败立即退出（不自旋），
 * 保证快速路径在有竞争时能快速降级到慢速路径。
 *
 * @lock:     per-CPU 轻量锁（非 RT：仅禁止抢占，无原子操作）
 * @main:     主 sheaf：解锁时永不为 NULL；分配/释放的第一候选
 * @spare:    备用 sheaf：NULL，或 size=0（空），或 size=capacity（满）；
 *            main 耗尽/填满时与 spare 交换，避免访问 barn
 * @rcu_free: 专用于 kfree_rcu() 批量延迟释放的 sheaf；
 *            满后一次 call_rcu 批量处理，减少 RCU 回调数
 */
struct slub_percpu_sheaves {
	local_trylock_t lock;
	struct slab_sheaf *main; /* never NULL when unlocked */
	struct slab_sheaf *spare; /* empty or full, may be NULL */
	struct slab_sheaf *rcu_free; /* for batching kfree_rcu() */
};

/*
 * struct kmem_cache_node - per-NUMA-node 的 slab 管理（Layer 2 慢速路径核心）
 *
 * 每个 kmem_cache 对每个 NUMA 节点维护一个此结构。
 * partial list 是"部分空闲 slab"的链表：有空闲对象但未全空的 slab 页
 * 挂在此处，供慢速路径批量取对象填充 sheaf。
 *
 * @list_lock:      保护 partial（和调试模式下的 full）链表的自旋锁；
 *                  持锁期间不允许增删链表元素或修改 nr_partial
 * @nr_partial:     partial 链表上的 slab 数量（含全空备用 slab）
 * @partial:        部分空闲 slab 链表（slab.slab_list 挂入此处）
 * @nr_slabs:       (调试) 本节点总 slab 数，用于检测泄漏
 * @total_objects:  (调试) 本节点总对象数
 * @full:           (调试) 满载 slab 链表，便于扫描所有对象
 */
struct kmem_cache_node {
	spinlock_t list_lock;
	unsigned long nr_partial;
	struct list_head partial;
#ifdef CONFIG_SLUB_DEBUG
	atomic_long_t nr_slabs;
	atomic_long_t total_objects;
	struct list_head full;
#endif
};

/* get_node() / get_barn_node()：通过节点号访问 per-node 管理结构 */
static inline struct kmem_cache_node *get_node(struct kmem_cache *s, int node)
{
	return s->per_node[node].node;
}

static inline struct node_barn *get_barn_node(struct kmem_cache *s, int node)
{
	return s->per_node[node].barn;
}

/*
 * get_barn() - 获取当前 CPU 所在 NUMA 节点的 barn
 *
 * 注意：当前 CPU 所在节点可能是 memoryless 节点（无本地内存），
 * 此时 barn 仍然存在（因为 slab_barn_nodes 对应 N_ONLINE），
 * 但对象实际来自其他节点。
 */
static inline struct node_barn *get_barn(struct kmem_cache *s)
{
	return get_barn_node(s, numa_node_id());
}

/*
 * for_each_kmem_cache_node() - 遍历所有已分配 kmem_cache_node 的 NUMA 节点
 *
 * 只有 slab_nodes 中的节点（即有内存的 online 节点）才有 kmem_cache_node，
 * get_node() 对其他节点返回 NULL，通过 if 子句过滤。
 */
#define for_each_kmem_cache_node(__s, __node, __n) \
	for (__node = 0; __node < nr_node_ids; __node++) \
		 if ((__n = get_node(__s, __node)))

/*
 * slab_nodes：记录哪些 NUMA 节点已分配 kmem_cache_node 结构。
 * 通常与 node_state[N_MEMORY] 一致，但 memory hotplug 过渡期间可能短暂不同。
 * 由 slab_mutex 保护。
 */
static nodemask_t slab_nodes;

/*
 * slab_barn_nodes：记录哪些 NUMA 节点已分配 node_barn 结构。
 * 对应 N_ONLINE（所有在线节点，包括 memoryless 节点），
 * 因为 CPU 在 memoryless 节点上运行时也需要 barn 来中转 sheaf。
 */
static nodemask_t slab_barn_nodes;

/*
 * flushwq：用于异步刷出 per-CPU sheaf 和 rcu_free sheaf 的工作队列。
 * flush_all() 向每个 CPU 投递 slub_flush work，等待所有 CPU 刷出完成。
 * 使用独立 workqueue 而非 system_wq，避免与内存回收路径死锁。
 */
static struct workqueue_struct *flushwq;

/* slub_flush_work：每个 CPU 一个，用于 flush_all() 的 per-CPU work 投递 */
struct slub_flush_work {
	struct work_struct work;
	struct kmem_cache *s;
	bool skip;
};

/* flush_lock：序列化多个并发的 flush_all() 调用，避免重复刷出 */
static DEFINE_MUTEX(flush_lock);
static DEFINE_PER_CPU(struct slub_flush_work, slub_flush);

/********************************************************************
 * 			Core slab cache functions
 *******************************************************************/

/*
 * freelist_ptr_encode() / freelist_ptr_decode() - freepointer 混淆编解码
 *
 * 开启 CONFIG_SLAB_FREELIST_HARDENED 时，freepointer 不以明文存储，
 * 而是通过 XOR 混淆：
 *
 *   encoded = ptr ^ s->random ^ swab(ptr_addr)
 *
 *   s->random：   cache 创建时生成的 per-cache 随机值（攻击者难以得知）
 *   ptr_addr：    存放 freepointer 的内存地址（每个对象不同）
 *   swab()：      字节序交换，使相邻对象的混淆值差异更大
 *
 * 安全目标：攻击者若想伪造 freelist 链（实现任意地址写），必须同时知道
 * s->random 和目标对象的地址，大幅提高利用难度。
 *
 * 未开启 HARDENED 时为 no-op，直接以明文存储指针。
 */
static inline freeptr_t freelist_ptr_encode(const struct kmem_cache *s,
					    void *ptr, unsigned long ptr_addr)
{
	unsigned long encoded;

#ifdef CONFIG_SLAB_FREELIST_HARDENED
	encoded = (unsigned long)ptr ^ s->random ^ swab(ptr_addr);
#else
	encoded = (unsigned long)ptr;
#endif
	return (freeptr_t){.v = encoded};
}

static inline void *freelist_ptr_decode(const struct kmem_cache *s,
					freeptr_t ptr, unsigned long ptr_addr)
{
	void *decoded;

#ifdef CONFIG_SLAB_FREELIST_HARDENED
	decoded = (void *)(ptr.v ^ s->random ^ swab(ptr_addr));
#else
	decoded = (void *)ptr.v;
#endif
	return decoded;
}

/*
 * get_freepointer() - 读取对象内存储的 freepointer（下一个空闲对象地址）
 *
 * freepointer 存储在 object + s->offset 处。
 * kasan_reset_tag() 先去除 KASAN tag，避免 tag-mismatch 导致的误报，
 * 因为 freepointer 字段在对象被释放时仍需要被访问。
 */
static inline void *get_freepointer(struct kmem_cache *s, void *object)
{
	unsigned long ptr_addr;
	freeptr_t p;

	object = kasan_reset_tag(object);
	ptr_addr = (unsigned long)object + s->offset;
	p = *(freeptr_t *)(ptr_addr);
	return freelist_ptr_decode(s, p, ptr_addr);
}

/*
 * set_freepointer() - 将 fp 写入 object 的 freepointer 字段
 *
 * HARDENED 模式下，object == fp 说明 double-free 或内存损坏，直接 BUG。
 * kasan_reset_tag() 去除写目标地址的 KASAN tag（原因同 get_freepointer）。
 * 写入前通过 freelist_ptr_encode() 编码，与明文存储的情况下 encode 为 no-op。
 */
static inline void set_freepointer(struct kmem_cache *s, void *object, void *fp)
{
	unsigned long freeptr_addr = (unsigned long)object + s->offset;

#ifdef CONFIG_SLAB_FREELIST_HARDENED
	BUG_ON(object == fp); /* naive detection of double free or corruption */
#endif

	freeptr_addr = (unsigned long)kasan_reset_tag((void *)freeptr_addr);
	*(freeptr_t *)freeptr_addr = freelist_ptr_encode(s, fp, freeptr_addr);
}

/*
 * freeptr_outside_object() - 判断 freepointer 是否在对象"用户区"之外
 *
 * s->offset < s->inuse：freepointer 在用户区内（对象中间），
 *   → 可能被用户数据覆盖，但故意如此（减少边界越界覆盖概率）
 * s->offset >= s->inuse：freepointer 在用户区之外（inuse 处或之后），
 *   → 调试模式、有 ctor、SLAB_TYPESAFE_BY_RCU 时采用此模式，
 *     保证 poison pattern 覆盖整个用户区
 *
 * 详见 calculate_sizes() 中的注释。
 */
static inline bool freeptr_outside_object(struct kmem_cache *s)
{
	return s->offset >= s->inuse;
}

/*
 * get_info_end() - 返回对象"信息区"末尾的偏移
 *
 * 信息区 = 用户数据区（inuse）+ 可选的外部 freepointer（sizeof(void*)）。
 * 调试元数据（track info 等）紧接信息区之后存放。
 * 若 freepointer 在对象内，信息区末尾就是 inuse；
 * 若 freepointer 在对象外，信息区末尾 = inuse + sizeof(void*)。
 */
static inline unsigned int get_info_end(struct kmem_cache *s)
{
	if (freeptr_outside_object(s))
		return s->inuse + sizeof(void *);
	else
		return s->inuse;
}

/*
 * for_each_object() - 遍历一个 slab 页内所有对象槽位
 *
 * fixup_red_left() 将起始指针跳过左侧 red zone（调试时）到达对象用户区头。
 * 步长为 s->size（含对齐填充），迭代 objects 次。
 */
#define for_each_object(__p, __s, __addr, __objects) \
	for (__p = fixup_red_left(__s, __addr); \
		__p < (__addr) + (__objects) * (__s)->size; \
		__p += (__s)->size)

/* order_objects() - 计算 2^order 个页能容纳多少个 size 大小的对象 */
static inline unsigned int order_objects(unsigned int order, unsigned int size)
{
	return ((unsigned int)PAGE_SIZE << order) / size;
}

/*
 * oo_make() / oo_order() / oo_objects() - order + objects 的编解码
 *
 * kmem_cache 用单个 u32（kmem_cache_order_objects.x）同时存储：
 *   高 OO_SHIFT（16）位：order（2^order 页）
 *   低 OO_SHIFT 位：   每 slab 对象数
 *
 * oo_make()：将 order 和 objects_per_slab 编码为 oo
 * oo_order()：从 oo 解码 order
 * oo_objects()：从 oo 解码对象数
 */
static inline struct kmem_cache_order_objects oo_make(unsigned int order,
		unsigned int size)
{
	struct kmem_cache_order_objects x = {
		(order << OO_SHIFT) + order_objects(order, size)
	};

	return x;
}

static inline unsigned int oo_order(struct kmem_cache_order_objects x)
{
	return x.x >> OO_SHIFT;
}

static inline unsigned int oo_objects(struct kmem_cache_order_objects x)
{
	return x.x & OO_MASK;
}

/*
 * pfmemalloc 标志操作：标记/清除/检测 slab 是否来自紧急内存储备。
 *
 * 基于网络的 swap（nbd、iSCSI 等）的内存回收路径需要分配内存来发送网络包，
 * 而网络包分配本身可能再触发内存回收，造成死锁。解决方案：
 * 回收路径使用 PF_MEMALLOC（允许使用紧急储备），分配的 slab 标记
 * SL_pfmemalloc；释放时只有同样设置了 __GFP_MEMALLOC 的调用方才能收到
 * 这些对象，避免普通分配"消耗"紧急储备对象。
 */
static inline bool slab_test_pfmemalloc(const struct slab *slab)
{
	return test_bit(SL_pfmemalloc, &slab->flags.f);
}

static inline void slab_set_pfmemalloc(struct slab *slab)
{
	set_bit(SL_pfmemalloc, &slab->flags.f);
}

static inline void __slab_clear_pfmemalloc(struct slab *slab)
{
	__clear_bit(SL_pfmemalloc, &slab->flags.f);
}

/*
 * slab_lock() / slab_unlock() - 基于 page lock 的 slab bit-spinlock
 *
 * 仅在不支持 cmpxchg_double（freelist_aba）的架构上使用，
 * 作为 slab freelist + counters 原子更新的回退锁。
 * 使用 SL_locked bit（= PG_locked）作为自旋位，
 * bit_spin_lock 在关中断的情况下自旋，因此调用方必须先关中断。
 */
static __always_inline void slab_lock(struct slab *slab)
{
	bit_spin_lock(SL_locked, &slab->flags.f);
}

static __always_inline void slab_unlock(struct slab *slab)
{
	bit_spin_unlock(SL_locked, &slab->flags.f);
}

/*
 * __update_freelist_fast() - 用 cmpxchg_double（freelist_aba）原子更新 slab 元数据
 *
 * 同时原子地更新 slab->freelist（指针）和 slab->counters（inuse/objects/frozen）。
 * 这两个字段紧邻，cmpxchg_double 可以在一条指令中完成，避免 ABA 问题。
 * 不支持该指令的架构返回 false，退回 __update_freelist_slow。
 */
static inline bool
__update_freelist_fast(struct slab *slab, struct freelist_counters *old,
		       struct freelist_counters *new)
{
#ifdef system_has_freelist_aba
	return try_cmpxchg_freelist(&slab->freelist_counters,
				    &old->freelist_counters,
				    new->freelist_counters);
#else
	return false;
#endif
}

/*
 * __update_freelist_slow() - 用 slab_lock bit-spinlock 更新 slab 元数据
 *
 * 回退路径：持 slab_lock，用读-比较-写代替 cmpxchg_double。
 * 写 counters 用 WRITE_ONCE，防止编译器将写操作拆分为多条指令，
 * 避免 get_partial_node_bulk() 在无锁读时看到撕裂值。
 */
static inline bool
__update_freelist_slow(struct slab *slab, struct freelist_counters *old,
		       struct freelist_counters *new)
{
	bool ret = false;

	slab_lock(slab);
	if (slab->freelist == old->freelist &&
	    slab->counters == old->counters) {
		slab->freelist = new->freelist;
		/* prevent tearing for the read in get_partial_node_bulk() */
		WRITE_ONCE(slab->counters, new->counters);
		ret = true;
	}
	slab_unlock(slab);

	return ret;
}

/*
 * __slab_update_freelist() - 原子更新 slab freelist + counters，失败可重试
 *
 * 调用约束：中断必须已禁用（irqsave 锁或等效操作）。
 * 例外：PREEMPT_RT 下 bit_spin_lock 内含 preempt_disable，hardirq 上下文
 * 不允许分配/释放，因此 lockdep 断言不适用，跳过检查。
 *
 * 优先用 __CMPXCHG_DOUBLE（fast 路径），不支持时用 slab_lock（slow 路径）。
 * 失败时调用 cpu_relax()（指令暂停/超线程让步）后返回 false，
 * 由调用方循环重试（do-while 模式）。
 */
static inline bool __slab_update_freelist(struct kmem_cache *s, struct slab *slab,
		struct freelist_counters *old, struct freelist_counters *new, const char *n)
{
	bool ret;

	if (!IS_ENABLED(CONFIG_PREEMPT_RT))
		lockdep_assert_irqs_disabled();

	if (s->flags & __CMPXCHG_DOUBLE)
		ret = __update_freelist_fast(slab, old, new);
	else
		ret = __update_freelist_slow(slab, old, new);

	if (likely(ret))
		return true;

	cpu_relax();
	stat(s, CMPXCHG_DOUBLE_FAIL);

#ifdef SLUB_DEBUG_CMPXCHG
	pr_info("%s %s: cmpxchg double redo ", n, s->name);
#endif

	return false;
}

/*
 * slab_update_freelist() - 外部调用的 slab freelist 原子更新入口
 *
 * 与 __slab_update_freelist() 的区别：
 * 本函数自行管理中断状态——slow 路径（不支持 cmpxchg_double）时自行
 * local_irq_save/restore，因此调用方无需预先关中断。
 * __slab_update_freelist 则要求调用方已关中断（用于已在 irqsave 锁内的场景）。
 */
static inline bool slab_update_freelist(struct kmem_cache *s, struct slab *slab,
		struct freelist_counters *old, struct freelist_counters *new, const char *n)
{
	bool ret;

	if (s->flags & __CMPXCHG_DOUBLE) {
		ret = __update_freelist_fast(slab, old, new);
	} else {
		unsigned long flags;

		local_irq_save(flags);
		ret = __update_freelist_slow(slab, old, new);
		local_irq_restore(flags);
	}
	if (likely(ret))
		return true;

	cpu_relax();
	stat(s, CMPXCHG_DOUBLE_FAIL);

#ifdef SLUB_DEBUG_CMPXCHG
	pr_info("%s %s: cmpxchg double redo ", n, s->name);
#endif

	return false;
}

/*
 * set_orig_size() / get_orig_size() - 保存/读取用户原始请求大小
 *
 * kmalloc 的 size 档位是固定的（通常是 2 的幂），kmalloc(200) 实际
 * 分配 256 字节的对象，多出的 56 字节是"隐藏空间"。KASAN 利用
 * orig_size 做细粒度越界检测：[orig_size, object_size) 范围内的访问
 * 会被标记为越界（即使物理上还在分配的对象内）。
 *
 * orig_size 存储位置：
 *   object + get_info_end(s) + 2 * sizeof(struct track)
 *   即：信息区末尾 + 两个 track 结构（ALLOC + FREE）之后的第一个 u64。
 *
 * slub_debug_orig_size(s) 为 false 时（不需要精确追踪）跳过存储，
 * get_orig_size() 直接返回 s->object_size 作为近似值。
 * KFENCE 分配的对象通过 kfence_ksize() 获取其精确大小。
 */
static inline void set_orig_size(struct kmem_cache *s,
				void *object, unsigned long orig_size)
{
	void *p = kasan_reset_tag(object);

	if (!slub_debug_orig_size(s))
		return;

	p += get_info_end(s);
	p += sizeof(struct track) * 2;

	*(unsigned long *)p = orig_size;
}

static inline unsigned long get_orig_size(struct kmem_cache *s, void *object)
{
	void *p = kasan_reset_tag(object);

	if (is_kfence_address(object))
		return kfence_ksize(object);

	if (!slub_debug_orig_size(s))
		return s->object_size;

	p += get_info_end(s);
	p += sizeof(struct track) * 2;

	return *(unsigned long *)p;
}

#ifdef CONFIG_SLAB_OBJ_EXT

/*
 * need_slab_obj_exts() - 判断该 cache 是否需要 per-object 扩展元数据
 *
 * slabobj_ext 是每个对象的附加元数据区，目前用于两个功能：
 *   1. memcg（内存 cgroup）计费：记录对象属于哪个 memcg
 *   2. 内存分配追踪（alloc_tag profiling）：记录分配调用点
 *
 * 优化策略：如果两个功能都未开启，跳过 obj_exts 分配，节省内存。
 * 注意：早于 memcg/profiling 子系统初始化创建的 cache 可能会遗漏这个优化，
 * 因为此时 memcg_kmem_online()/mem_alloc_profiling_enabled() 尚返回 false。
 *
 * SLAB_NO_OBJ_EXT：强制禁用 obj_exts（用于 obj_exts 自身的 cache，避免递归）。
 */
static inline bool need_slab_obj_exts(struct kmem_cache *s)
{
	if (s->flags & SLAB_NO_OBJ_EXT)
		return false;

	if (memcg_kmem_online() && (s->flags & SLAB_ACCOUNT))
		return true;

	if (mem_alloc_profiling_enabled())
		return true;

	return false;
}

/*
 * obj_exts_size_in_slab() - 计算该 slab 中 obj_exts 数组所需字节数
 *
 * 每个对象对应一个 slabobj_ext，整个数组紧跟在对象数据区之后。
 */
static inline unsigned int obj_exts_size_in_slab(struct slab *slab)
{
	return sizeof(struct slabobj_ext) * slab->objects;
}

/*
 * obj_exts_offset_in_slab() - 计算 obj_exts 数组在 slab 内存中的起始偏移
 *
 * obj_exts 数组紧接所有对象数据之后（s->size * objects），
 * 并对齐到 slabobj_ext 的对齐要求。
 */
static inline unsigned long obj_exts_offset_in_slab(struct kmem_cache *s,
						    struct slab *slab)
{
	unsigned long objext_offset;

	objext_offset = s->size * slab->objects;
	objext_offset = ALIGN(objext_offset, sizeof(struct slabobj_ext));
	return objext_offset;
}

/*
 * obj_exts_fit_within_slab_leftover() - 判断 obj_exts 是否能放入 slab 尾部剩余空间
 *
 * slab 页尾部通常有因对齐产生的剩余空间（leftover）。
 * 若 obj_exts 数组能塞入这段空间，就不需要额外分配内存（节省一次 kmalloc）。
 */
static inline bool obj_exts_fit_within_slab_leftover(struct kmem_cache *s,
						     struct slab *slab)
{
	unsigned long objext_offset = obj_exts_offset_in_slab(s, slab);
	unsigned long objext_size = obj_exts_size_in_slab(slab);

	return objext_offset + objext_size <= slab_size(slab);
}

/*
 * obj_exts_in_slab() - 判断该 slab 的 obj_exts 数组是否存放在 slab 页内部
 *
 * obj_exts 数组要么嵌入 slab 页的 leftover（in-slab），要么通过额外的
 * kmalloc 分配（out-of-slab）。本函数通过地址范围判断是哪种情况，
 * 用于 allocate_slab() 和 free_slab() 中正确地分配/释放 obj_exts。
 */
static inline bool obj_exts_in_slab(struct kmem_cache *s, struct slab *slab)
{
	unsigned long obj_exts;
	unsigned long start;
	unsigned long end;

	obj_exts = slab_obj_exts(slab);
	if (!obj_exts)
		return false;

	start = (unsigned long)slab_address(slab);
	end = start + slab_size(slab);
	return (obj_exts >= start) && (obj_exts < end);
}
#else
static inline bool need_slab_obj_exts(struct kmem_cache *s)
{
	return false;
}

static inline unsigned int obj_exts_size_in_slab(struct slab *slab)
{
	return 0;
}

static inline unsigned long obj_exts_offset_in_slab(struct kmem_cache *s,
						    struct slab *slab)
{
	return 0;
}

static inline bool obj_exts_fit_within_slab_leftover(struct kmem_cache *s,
						     struct slab *slab)
{
	return false;
}

static inline bool obj_exts_in_slab(struct kmem_cache *s, struct slab *slab)
{
	return false;
}

#endif

#if defined(CONFIG_SLAB_OBJ_EXT) && defined(CONFIG_64BIT)
/*
 * obj_exts_in_object() - 判断 obj_exts 是否内嵌在对象尾部（非 slab leftover）
 *
 * 第三种 obj_exts 存储方式：直接嵌入到每个对象尾部的元数据区（仅 64 位）。
 * 需要满足两个条件：
 *   1. obj_exts_in_slab() 为真（地址在 slab 页范围内）
 *   2. slab 步长 == s->size（步长未被拉伸，即 obj_exts 就紧跟在对象之后）
 *
 * 不能直接依赖 SLAB_OBJ_EXT_IN_OBJ flag，因为 cache 级别可能设置了该 flag，
 * 但实际某个 slab 实例可能用的是 leftover 方式（两种方式共存）。
 */
static bool obj_exts_in_object(struct kmem_cache *s, struct slab *slab)
{
	/*
	 * Note we cannot rely on the SLAB_OBJ_EXT_IN_OBJ flag here and need to
	 * check the stride. A cache can have SLAB_OBJ_EXT_IN_OBJ set, but
	 * allocations within_slab_leftover are preferred. And those may be
	 * possible or not depending on the particular slab's size.
	 */
	return obj_exts_in_slab(s, slab) &&
	       (slab_get_stride(slab) == s->size);
}

/*
 * obj_exts_offset_in_object() - 计算 obj_exts 在对象内部的字节偏移
 *
 * 对象内元数据区布局（从 object 起始地址）：
 *   [用户数据区: inuse]
 *   [外部 freepointer: sizeof(void*) 若 freeptr_outside_object()]
 *   [track × 2: SLAB_STORE_USER 时]
 *   [orig_size: slub_debug_orig_size() 时]
 *   [KASAN metadata: kasan_metadata_size()]
 *   [slabobj_ext: ← obj_exts_offset_in_object() 返回此处偏移]
 */
static unsigned int obj_exts_offset_in_object(struct kmem_cache *s)
{
	unsigned int offset = get_info_end(s);

	if (kmem_cache_debug_flags(s, SLAB_STORE_USER))
		offset += sizeof(struct track) * 2;

	if (slub_debug_orig_size(s))
		offset += sizeof(unsigned long);

	offset += kasan_metadata_size(s, false);

	return offset;
}
#else
static inline bool obj_exts_in_object(struct kmem_cache *s, struct slab *slab)
{
	return false;
}

static inline unsigned int obj_exts_offset_in_object(struct kmem_cache *s)
{
	return 0;
}
#endif

#ifdef CONFIG_SLUB_DEBUG

/* validate_slab_ptr() - 快速检查 slab 指针是否合法（调试上下文用） */
static inline bool validate_slab_ptr(struct slab *slab)
{
	return PageSlab(slab_page(slab));
}

/*
 * object_map / object_map_lock - 全局位图，用于标记 slab 内哪些对象是空闲的
 *
 * __fill_map() 遍历 slab->freelist 链表，将每个空闲对象的槽位索引在
 * obj_map 中置位。调用方用它来区分已分配对象与空闲对象，
 * 例如在 validate_slab_obj() 中跳过空闲槽位的 poison 检查。
 * 由于是全局共享位图，访问前必须持 object_map_lock。
 */
static unsigned long object_map[BITS_TO_LONGS(MAX_OBJS_PER_PAGE)];
static DEFINE_SPINLOCK(object_map_lock);

static void __fill_map(unsigned long *obj_map, struct kmem_cache *s,
		       struct slab *slab)
{
	void *addr = slab_address(slab);
	void *p;

	bitmap_zero(obj_map, slab->objects);

	for (p = slab->freelist; p; p = get_freepointer(s, p))
		set_bit(__obj_to_index(s, addr, p), obj_map);
}

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * slab_add_kunit_errors() / slab_in_kunit_test() - KUnit 测试框架集成
 *
 * KUnit 测试可以在 current->kunit_test 上注册名为 "slab_errors" 的资源，
 * 用于捕获 slab 检测到的错误数量，而不触发真正的 WARN_ON / panic。
 *
 * slab_add_kunit_errors()：若当前在 KUnit 测试中，递增错误计数并返回 true，
 *   告知调用方"已捕获，无需再打印错误信息或添加 taint"。
 * slab_in_kunit_test()：仅检查是否处于 KUnit 测试中（不递增计数）。
 * 非 KUnit 编译时均为 no-op inline。
 */
static bool slab_add_kunit_errors(void)
{
	struct kunit_resource *resource;

	if (!kunit_get_current_test())
		return false;

	resource = kunit_find_named_resource(current->kunit_test, "slab_errors");
	if (!resource)
		return false;

	(*(int *)resource->data)++;
	kunit_put_resource(resource);
	return true;
}

bool slab_in_kunit_test(void)
{
	struct kunit_resource *resource;

	if (!kunit_get_current_test())
		return false;

	resource = kunit_find_named_resource(current->kunit_test, "slab_errors");
	if (!resource)
		return false;

	kunit_put_resource(resource);
	return true;
}
#else
static inline bool slab_add_kunit_errors(void) { return false; }
#endif

/*
 * size_from_object() - 返回对象在 slab 内占用的净字节数（不含左侧 red zone）
 *
 * 开启 SLAB_RED_ZONE 时，slab 内每个槽位从左 red zone 开始，
 * 但用户"看到"的对象起始地址已跳过 red_left_pad 字节（fixup_red_left 处理）。
 * 此函数返回从用户地址到槽位末尾的长度，用于调试检查中的边界计算。
 */
static inline unsigned int size_from_object(struct kmem_cache *s)
{
	if (s->flags & SLAB_RED_ZONE)
		return s->size - s->red_left_pad;

	return s->size;
}

/*
 * restore_red_left() - 将对象地址从用户区头部退回到槽位起始（含左 red zone）
 *
 * fixup_red_left() 的逆操作：从用户可见地址减去 red_left_pad，
 * 得到 slab 内存中该槽位真正的起始地址，用于对左侧 red zone 做检查。
 */
static inline void *restore_red_left(struct kmem_cache *s, void *p)
{
	if (s->flags & SLAB_RED_ZONE)
		p -= s->red_left_pad;

	return p;
}

/*
 * Debug settings:
 */
#if defined(CONFIG_SLUB_DEBUG_ON)
static slab_flags_t slub_debug = DEBUG_DEFAULT_FLAGS;
#else
static slab_flags_t slub_debug;
#endif

static const char *slub_debug_string __ro_after_init;
static int disable_higher_order_debug;

/*
 * Object debugging
 */

/*
 * check_valid_pointer() - 验证对象指针是否在该 slab 合法范围内
 *
 * 三个检查：
 *   1. object >= base：不能低于 slab 页起始
 *   2. object < base + objects * size：不能超出 slab 末尾
 *   3. (object - base) % size == 0：必须对齐到槽位边界
 *
 * restore_red_left() 先将用户地址退回槽位真实起始（含左 red zone），
 * 再做范围计算，保证调试模式下地址对齐检查正确。
 * NULL 对象视为合法（调用方自行处理）。
 */
static inline int check_valid_pointer(struct kmem_cache *s,
				struct slab *slab, void *object)
{
	void *base;

	if (!object)
		return 1;

	base = slab_address(slab);
	object = kasan_reset_tag(object);
	object = restore_red_left(s, object);
	if (object < base || object >= base + slab->objects * s->size ||
		(object - base) % s->size) {
		return 0;
	}

	return 1;
}

/*
 * print_section() - 以 hex dump 形式打印内存区域（调试错误报告用）
 *
 * metadata_access_enable/disable() 临时允许访问 KASAN 标记为不可访问的内存
 * （如 red zone、poison 区域），在错误报告时需要打印这些区域的内容。
 */
static void print_section(char *level, char *text, u8 *addr,
			  unsigned int length)
{
	metadata_access_enable();
	print_hex_dump(level, text, DUMP_PREFIX_ADDRESS,
			16, 1, kasan_reset_tag((void *)addr), length, 1);
	metadata_access_disable();
}

/*
 * get_track() - 获取对象内 track 记录的指针
 *
 * track 结构存储在对象信息区末尾（get_info_end(s) 偏移处）：
 *   objects[get_info_end(s) + 0]                → TRACK_ALLOC（分配记录）
 *   objects[get_info_end(s) + sizeof(track)]     → TRACK_FREE（释放记录）
 *
 * kasan_reset_tag() 去除 KASAN tag，允许访问元数据区（该区域可能被
 * KASAN 标记为不可访问）。
 */
static struct track *get_track(struct kmem_cache *s, void *object,
	enum track_item alloc)
{
	struct track *p;

	p = object + get_info_end(s);

	return kasan_reset_tag(p + alloc);
}

/*
 * set_track_prepare() - 采集当前调用栈并存入 stackdepot，返回句柄
 *
 * stack_trace_save() 保存最多 TRACK_ADDRS_COUNT 层调用栈，
 * stack_depot_save() 将其压缩存入全局 stackdepot 并返回 32 位句柄。
 * 未开启 CONFIG_STACKDEPOT 时返回 0（无调用栈记录）。
 * skip=3 跳过 set_track_prepare 本身及其两层直接调用者，
 * 从真正的分配/释放调用点开始记录。
 */
#ifdef CONFIG_STACKDEPOT
static noinline depot_stack_handle_t set_track_prepare(gfp_t gfp_flags)
{
	depot_stack_handle_t handle;
	unsigned long entries[TRACK_ADDRS_COUNT];
	unsigned int nr_entries;

	nr_entries = stack_trace_save(entries, ARRAY_SIZE(entries), 3);
	handle = stack_depot_save(entries, nr_entries, gfp_flags);

	return handle;
}
#else
static inline depot_stack_handle_t set_track_prepare(gfp_t gfp_flags)
{
	return 0;
}
#endif

/*
 * set_track_update() - 将调用现场信息写入对象的 track 字段
 *
 * 记录：调用地址、stackdepot 句柄、CPU 编号、PID、jiffies 时间戳。
 * 与 set_track_prepare() 分离，允许在持锁期间调用（stackdepot 操作
 * 可能申请内存，不适合在某些锁内调用，所以拆分为两步）。
 */
static void set_track_update(struct kmem_cache *s, void *object,
			     enum track_item alloc, unsigned long addr,
			     depot_stack_handle_t handle)
{
	struct track *p = get_track(s, object, alloc);

#ifdef CONFIG_STACKDEPOT
	p->handle = handle;
#endif
	p->addr = addr;
	p->cpu = raw_smp_processor_id();
	p->pid = current->pid;
	p->when = jiffies;
}

/* set_track() - 一步完成调用栈采集 + track 写入（便捷包装） */
static __always_inline void set_track(struct kmem_cache *s, void *object,
				      enum track_item alloc, unsigned long addr, gfp_t gfp_flags)
{
	depot_stack_handle_t handle = set_track_prepare(gfp_flags);

	set_track_update(s, object, alloc, addr, handle);
}

/* init_tracking() - 将对象的两个 track 字段清零（新分配对象初始化用） */
static void init_tracking(struct kmem_cache *s, void *object)
{
	struct track *p;

	if (!(s->flags & SLAB_STORE_USER))
		return;

	p = get_track(s, object, TRACK_ALLOC);
	memset(p, 0, 2*sizeof(struct track));
}

/*
 * print_track() - 打印单条 track 记录（分配或释放的调用现场）
 *
 * 输出：操作类型、调用地址（符号化）、距今 jiffies 数、CPU、PID，
 * 以及完整调用栈（通过 stackdepot 反解）。
 */
static void print_track(const char *s, struct track *t, unsigned long pr_time)
{
	depot_stack_handle_t handle __maybe_unused;

	if (!t->addr)
		return;

	pr_err("%s in %pS age=%lu cpu=%u pid=%d\n",
	       s, (void *)t->addr, pr_time - t->when, t->cpu, t->pid);
#ifdef CONFIG_STACKDEPOT
	handle = READ_ONCE(t->handle);
	if (handle)
		stack_depot_print(handle);
	else
		pr_err("object allocation/free stack trace missing\n");
#endif
}

/* print_tracking() - 打印对象最近一次分配和释放的调用现场（错误报告入口） */
void print_tracking(struct kmem_cache *s, void *object)
{
	unsigned long pr_time = jiffies;
	if (!(s->flags & SLAB_STORE_USER))
		return;

	print_track("Allocated", get_track(s, object, TRACK_ALLOC), pr_time);
	print_track("Freed", get_track(s, object, TRACK_FREE), pr_time);
}

/* print_slab_info() - 打印 slab 页的基本状态（对象数、已用数、freelist、标志） */
static void print_slab_info(const struct slab *slab)
{
	pr_err("Slab 0x%p objects=%u used=%u fp=0x%p flags=%pGp\n",
	       slab, slab->objects, slab->inuse, slab->freelist,
	       &slab->flags.f);
}

/*
 * skip_orig_size_check() - 将对象的 orig_size 重置为 object_size
 *
 * 某些路径（如 krealloc 放大时）需要跳过 orig_size 越界检查，
 * 通过将 orig_size 设为 object_size 来禁用细粒度越界检测。
 */
void skip_orig_size_check(struct kmem_cache *s, const void *object)
{
	set_orig_size(s, (void *)object, s->object_size);
}

/*
 * __slab_bug() / slab_bug() - 打印 slab 错误报告头部
 *
 * 输出格式：
 *   ====...====
 *   BUG <cache_name> (<taint_flags>): <message>
 *   ----...----
 *
 * 不触发 panic，只打印。真正的 WARN_ON 由 object_err() / slab_err() 触发。
 */
static void __slab_bug(struct kmem_cache *s, const char *fmt, va_list argsp)
{
	struct va_format vaf;
	va_list args;

	va_copy(args, argsp);
	vaf.fmt = fmt;
	vaf.va = &args;
	pr_err("=============================================================================\n");
	pr_err("BUG %s (%s): %pV\n", s ? s->name : "<unknown>", print_tainted(), &vaf);
	pr_err("-----------------------------------------------------------------------------\n\n");
	va_end(args);
}

static void slab_bug(struct kmem_cache *s, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	__slab_bug(s, fmt, args);
	va_end(args);
}

/*
 * slab_fix() - 打印 slab 修复动作说明（自动修复了哪些损坏字段）
 *
 * KUnit 测试中：递增错误计数，不打印（避免污染测试输出）。
 * 输出格式：FIX <cache_name>: <message>
 */
__printf(2, 3)
static void slab_fix(struct kmem_cache *s, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (slab_add_kunit_errors())
		return;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	pr_err("FIX %s: %pV\n", s->name, &vaf);
	va_end(args);
}

/*
 * print_trailer() - 打印完整的对象错误报告（slab 一致性检查失败时）
 *
 * 输出内容（按布局顺序）：
 *   1. 分配/释放调用栈（print_tracking）
 *   2. slab 页基本信息（对象数、已用数、freelist、标志）
 *   3. 对象地址、在 slab 内的偏移、freepointer
 *   4. 左侧 red zone（若开启 SLAB_RED_ZONE）或前 16 字节
 *   5. 对象用户数据区
 *   6. 右侧 red zone（object_size ~ inuse 之间）
 *   7. Padding（size 末尾未被已知元数据覆盖的字节，通常是 freepointer 空间）
 *
 * off 累加各元数据区大小，若不等于 size_from_object(s) 说明有 padding，
 * padding 起始处通常存放了 freepointer。
 */
static void print_trailer(struct kmem_cache *s, struct slab *slab, u8 *p)
{
	unsigned int off;	/* Offset of last byte */
	u8 *addr = slab_address(slab);

	print_tracking(s, p);

	print_slab_info(slab);

	pr_err("Object 0x%p @offset=%tu fp=0x%p\n\n",
	       p, p - addr, get_freepointer(s, p));

	if (s->flags & SLAB_RED_ZONE)
		print_section(KERN_ERR, "Redzone  ", p - s->red_left_pad,
			      s->red_left_pad);
	else if (p > addr + 16)
		print_section(KERN_ERR, "Bytes b4 ", p - 16, 16);

	print_section(KERN_ERR,         "Object   ", p,
		      min_t(unsigned int, s->object_size, PAGE_SIZE));
	if (s->flags & SLAB_RED_ZONE)
		print_section(KERN_ERR, "Redzone  ", p + s->object_size,
			s->inuse - s->object_size);

	off = get_info_end(s);

	if (s->flags & SLAB_STORE_USER)
		off += 2 * sizeof(struct track);

	if (slub_debug_orig_size(s))
		off += sizeof(unsigned long);

	off += kasan_metadata_size(s, false);

	if (obj_exts_in_object(s, slab))
		off += sizeof(struct slabobj_ext);

	if (off != size_from_object(s))
		/* Beginning of the filler is the free pointer */
		print_section(KERN_ERR, "Padding  ", p + off,
			      size_from_object(s) - off);
}

/*
 * object_err() - 报告单个对象的 slab 一致性错误
 *
 * KUnit 测试中静默递增计数；否则打印完整报告并设置 TAINT_BAD_PAGE，
 * 最后 WARN_ON(1) 触发调用栈打印。
 * 指针无效时只打印 slab 信息，不尝试 print_trailer（避免二次 oops）。
 */
static void object_err(struct kmem_cache *s, struct slab *slab,
			u8 *object, const char *reason)
{
	if (slab_add_kunit_errors())
		return;

	slab_bug(s, reason);
	if (!object || !check_valid_pointer(s, slab, object)) {
		print_slab_info(slab);
		pr_err("Invalid pointer 0x%p\n", object);
	} else {
		print_trailer(s, slab, object);
	}
	add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);

	WARN_ON(1);
}

/* __slab_err() - 打印 slab 级别的错误信息（不依赖特定对象） */
static void __slab_err(struct slab *slab)
{
	if (slab_in_kunit_test())
		return;

	print_slab_info(slab);
	add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);

	WARN_ON(1);
}

/* slab_err() - 格式化打印 slab 错误（object_err 的 slab 级别版本） */
static __printf(3, 4) void slab_err(struct kmem_cache *s, struct slab *slab,
			const char *fmt, ...)
{
	va_list args;

	if (slab_add_kunit_errors())
		return;

	va_start(args, fmt);
	__slab_bug(s, fmt, args);
	va_end(args);

	__slab_err(slab);
}

/*
 * init_object() - 用 val 值填充对象的调试区域（red zone + poison）
 *
 * @val：SLUB_RED_ACTIVE（0xbb，对象被分配时）或 SLUB_RED_INACTIVE（0xcc，释放时）
 *
 * 填充逻辑（与对象内存布局对应）：
 *   1. 左侧 red zone（p - red_left_pad，长 red_left_pad）：填 val
 *   2. 对象用户区（p，长 poison_size）：
 *      - 前 poison_size-1 字节：POISON_FREE（0x6b）
 *      - 最后 1 字节：POISON_END（0xa5）
 *      → 标准 poison pattern，use-after-free 时此 pattern 被破坏
 *   3. 右侧 red zone（p+poison_size，长 inuse-poison_size）：填 val
 *
 * poison_size 通常等于 object_size，但当 slub_debug_orig_size 开启且
 * val==SLUB_RED_ACTIVE 时，poison_size = orig_size（kmalloc 请求的实际大小），
 * 多出的 [orig_size, object_size) 范围被 red zone 覆盖，检测 kmalloc 越界。
 *
 * memset_no_sanitize_memory() 写入时不更新 KMSAN shadow，
 * 保留未初始化标记，使 KMSAN 仍能区分"未初始化"和"use-after-free"。
 */
static void init_object(struct kmem_cache *s, void *object, u8 val)
{
	u8 *p = kasan_reset_tag(object);
	unsigned int poison_size = s->object_size;

	if (s->flags & SLAB_RED_ZONE) {
		/*
		 * Here and below, avoid overwriting the KMSAN shadow. Keeping
		 * the shadow makes it possible to distinguish uninit-value
		 * from use-after-free.
		 */
		memset_no_sanitize_memory(p - s->red_left_pad, val,
					  s->red_left_pad);

		if (slub_debug_orig_size(s) && val == SLUB_RED_ACTIVE) {
			/*
			 * Redzone the extra allocated space by kmalloc than
			 * requested, and the poison size will be limited to
			 * the original request size accordingly.
			 */
			poison_size = get_orig_size(s, object);
		}
	}

	if (s->flags & __OBJECT_POISON) {
		memset_no_sanitize_memory(p, POISON_FREE, poison_size - 1);
		memset_no_sanitize_memory(p + poison_size - 1, POISON_END, 1);
	}

	if (s->flags & SLAB_RED_ZONE)
		memset_no_sanitize_memory(p + poison_size, val,
					  s->inuse - poison_size);
}

/*
 * restore_bytes() - 将损坏的调试字节恢复为预期值（自修复）
 *
 * 发现 red zone 或 poison 被覆写后调用，打印 FIX 消息并用
 * memset 恢复原始值，使后续分配/检查能正常进行。
 * 自修复的目的是避免单次内存损坏引发后续雪崩式错误。
 */
static void restore_bytes(struct kmem_cache *s, const char *message, u8 data,
						void *from, void *to)
{
	slab_fix(s, "Restoring %s 0x%p-0x%p=0x%x", message, from, to - 1, data);
	memset(from, data, to - from);
}

/*
 * pad_check_attributes：KMSAN 下将 padding 检查函数标记为 noinline 且
 * 不做 KMSAN 检查（__no_kmsan_checks），因为 padding 区域本来就是
 * "未初始化"的，KMSAN 会误报，这里需要绕过。
 */
#ifdef CONFIG_KMSAN
#define pad_check_attributes noinline __no_kmsan_checks
#else
#define pad_check_attributes
#endif

/*
 * check_bytes_and_report() - 检查内存区域是否全为预期值，不符则报告并修复
 *
 * @what:          描述被检查区域的名称（如 "Left Redzone"、"Poison"）
 * @start:         被检查区域起始地址
 * @value:         预期字节值
 * @bytes:         区域长度
 * @slab_obj_print:发现损坏时是否附加打印完整对象信息
 *
 * 用 memchr_inv() 快速找到第一个不符预期的字节（fault），
 * 再从尾部反向收缩找到损坏区间末尾（end），打印精确的损坏范围，
 * 然后调用 restore_bytes() 修复（填回预期值），返回 0 表示发现错误。
 *
 * KMSAN 不检查（metadata_access_enable/disable 临时解除访问限制）。
 */
static pad_check_attributes int
check_bytes_and_report(struct kmem_cache *s, struct slab *slab,
		       u8 *object, const char *what, u8 *start, unsigned int value,
		       unsigned int bytes, bool slab_obj_print)
{
	u8 *fault;
	u8 *end;
	u8 *addr = slab_address(slab);

	metadata_access_enable();
	fault = memchr_inv(kasan_reset_tag(start), value, bytes);
	metadata_access_disable();
	if (!fault)
		return 1;

	end = start + bytes;
	while (end > fault && end[-1] == value)
		end--;

	if (slab_add_kunit_errors())
		goto skip_bug_print;

	pr_err("[%s overwritten] 0x%p-0x%p @offset=%tu. First byte 0x%x instead of 0x%x\n",
	       what, fault, end - 1, fault - addr, fault[0], value);

	if (slab_obj_print)
		object_err(s, slab, object, "Object corrupt");

skip_bug_print:
	restore_bytes(s, what, value, fault, end);
	return 0;
}

/*
 * Object field layout:
 *
 * [Left redzone padding] (if SLAB_RED_ZONE)
 *   - Field size: s->red_left_pad
 *   - Immediately precedes each object when SLAB_RED_ZONE is set.
 *   - Filled with 0xbb (SLUB_RED_INACTIVE) for inactive objects and
 *     0xcc (SLUB_RED_ACTIVE) for objects in use when SLAB_RED_ZONE.
 *
 * [Object bytes] (object address starts here)
 *   - Field size: s->object_size
 *   - Object payload bytes.
 *   - If the freepointer may overlap the object, it is stored inside
 *     the object (typically near the middle).
 *   - Poisoning uses 0x6b (POISON_FREE) and the last byte is
 *     0xa5 (POISON_END) when __OBJECT_POISON is enabled.
 *
 * [Word-align padding] (right redzone when SLAB_RED_ZONE is set)
 *   - Field size: s->inuse - s->object_size
 *   - If redzoning is enabled and ALIGN(size, sizeof(void *)) adds no
 *     padding, explicitly extend by one word so the right redzone is
 *     non-empty.
 *   - Filled with 0xbb (SLUB_RED_INACTIVE) for inactive objects and
 *     0xcc (SLUB_RED_ACTIVE) for objects in use when SLAB_RED_ZONE.
 *
 * [Metadata starts at object + s->inuse]
 *   - A. freelist pointer (if freeptr_outside_object)
 *   - B. alloc tracking (SLAB_STORE_USER)
 *   - C. free tracking (SLAB_STORE_USER)
 *   - D. original request size (SLAB_KMALLOC && SLAB_STORE_USER)
 *   - E. KASAN metadata (if enabled)
 *
 * [Mandatory padding] (if CONFIG_SLUB_DEBUG && SLAB_RED_ZONE)
 *   - One mandatory debug word to guarantee a minimum poisoned gap
 *     between metadata and the next object, independent of alignment.
 *   - Filled with 0x5a (POISON_INUSE) when SLAB_POISON is set.
 * [Final alignment padding]
 *   - Bytes added by ALIGN(size, s->align) to reach s->size.
 *   - When the padding is large enough, it can be used to store
 *     struct slabobj_ext for accounting metadata (obj_exts_in_object()).
 *   - The remaining bytes (if any) are filled with 0x5a (POISON_INUSE)
 *     when SLAB_POISON is set.
 *
 * Notes:
 * - Redzones are filled by init_object() with SLUB_RED_ACTIVE/INACTIVE.
 * - Object contents are poisoned with POISON_FREE/END when __OBJECT_POISON.
 * - The trailing padding is pre-filled with POISON_INUSE by
 *   setup_slab_debug() when SLAB_POISON is set, and is validated by
 *   check_pad_bytes().
 * - The first object pointer is slab_address(slab) +
 *   (s->red_left_pad if redzoning); subsequent objects are reached by
 *   adding s->size each time.
 *
 * If a slab cache flag relies on specific metadata to exist at a fixed
 * offset, the flag must be included in SLAB_NEVER_MERGE to prevent merging.
 * Otherwise, the cache would misbehave as s->object_size and s->inuse are
 * adjusted during cache merging (see __kmem_cache_alias()).
 */
/*
 * check_pad_bytes() - 检查对象尾部 padding 区域是否被破坏
 *
 * 对象布局末尾（info_end 到 size_from_object 之间）是对齐 padding，
 * 初始化时被填为 POISON_INUSE（0x5a）。此函数验证这段 padding 未被越界写。
 *
 * off 从 info_end 开始，依次跳过各已知元数据：
 *   track × 2（SLAB_STORE_USER）、orig_size（SLAB_KMALLOC）、
 *   KASAN metadata、slabobj_ext（obj_exts_in_object）。
 * 剩余部分即为 padding，应全为 POISON_INUSE。
 */
static int check_pad_bytes(struct kmem_cache *s, struct slab *slab, u8 *p)
{
	unsigned long off = get_info_end(s);	/* The end of info */

	if (s->flags & SLAB_STORE_USER) {
		/* We also have user information there */
		off += 2 * sizeof(struct track);

		if (s->flags & SLAB_KMALLOC)
			off += sizeof(unsigned long);
	}

	off += kasan_metadata_size(s, false);

	if (obj_exts_in_object(s, slab))
		off += sizeof(struct slabobj_ext);

	if (size_from_object(s) == off)
		return 1;

	return check_bytes_and_report(s, slab, p, "Object padding",
			p + off, POISON_INUSE, size_from_object(s) - off, true);
}

/*
 * slab_pad_check() - 检查 slab 页尾部剩余空间（页 padding）是否被破坏
 *
 * slab 页末尾（所有对象槽位之后）可能有因对齐产生的剩余字节（remainder），
 * 初始化时被填为 POISON_INUSE。此函数验证这段页级 padding 未被越界写。
 *
 * 若 obj_exts 以 in-slab 方式存储但不在对象内（leftover 方式），
 * 需要从 remainder 计算中减去 obj_exts 区域的大小。
 */
static pad_check_attributes void
slab_pad_check(struct kmem_cache *s, struct slab *slab)
{
	u8 *start;
	u8 *fault;
	u8 *end;
	u8 *pad;
	int length;
	int remainder;

	if (!(s->flags & SLAB_POISON))
		return;

	start = slab_address(slab);
	length = slab_size(slab);
	end = start + length;

	if (obj_exts_in_slab(s, slab) && !obj_exts_in_object(s, slab)) {
		remainder = length;
		remainder -= obj_exts_offset_in_slab(s, slab);
		remainder -= obj_exts_size_in_slab(slab);
	} else {
		remainder = length % s->size;
	}

	if (!remainder)
		return;

	pad = end - remainder;
	metadata_access_enable();
	fault = memchr_inv(kasan_reset_tag(pad), POISON_INUSE, remainder);
	metadata_access_disable();
	if (!fault)
		return;
	while (end > fault && end[-1] == POISON_INUSE)
		end--;

	slab_bug(s, "Padding overwritten. 0x%p-0x%p @offset=%tu",
		 fault, end - 1, fault - start);
	print_section(KERN_ERR, "Padding ", pad, remainder);
	__slab_err(slab);

	restore_bytes(s, "slab padding", POISON_INUSE, fault, end);
}

/*
 * check_object() - 对单个对象执行完整的调试一致性检查
 *
 * @val：预期状态值（SLUB_RED_ACTIVE=已分配，SLUB_RED_INACTIVE=空闲）
 *
 * 检查顺序（依开启的标志）：
 *
 * 1. SLAB_RED_ZONE：
 *    - 左侧 red zone（object - red_left_pad）是否全为 val
 *    - 右侧 red zone（object_size ~ inuse）是否全为 val
 *    - slub_debug_orig_size：若已分配（val==RED_ACTIVE），
 *      [orig_size, object_size) 额外 red zone 是否全为 val
 *      （检测 kmalloc 超出 orig_size 的越界写）
 *
 * 2. 无 RED_ZONE 时的对齐 padding：
 *    - SLAB_POISON 开启时检查 [object_size, inuse) 是否全为 POISON_INUSE
 *
 * 3. SLAB_POISON（对象已释放时）：
 *    - [kasan_meta_size, object_size-1) 是否全为 POISON_FREE（0x6b）
 *    - [object_size-1] 是否为 POISON_END（0xa5）
 *    - 跳过 [0, kasan_meta_size)：KASAN 会在对象头部存 free metadata，
 *      避免与 KASAN 的元数据冲突
 *    - 对象尾部 padding（check_pad_bytes）
 *
 * 4. freepointer 合法性检查：
 *    - 仅当 freepointer 在对象外，或对象处于空闲状态时检查
 *      （已分配且 freepointer 在对象内时无法安全检查）
 *    - 若损坏：强制置 NULL，宁愿丢失后续 freelist 也不继续用坏链表
 *
 * 返回 1 = 全部检查通过，0 = 发现至少一处错误（但尽可能继续检查所有项）
 */
static int check_object(struct kmem_cache *s, struct slab *slab,
					void *object, u8 val)
{
	u8 *p = object;
	u8 *endobject = object + s->object_size;
	unsigned int orig_size, kasan_meta_size;
	int ret = 1;

	if (s->flags & SLAB_RED_ZONE) {
		if (!check_bytes_and_report(s, slab, object, "Left Redzone",
			object - s->red_left_pad, val, s->red_left_pad, ret))
			ret = 0;

		if (!check_bytes_and_report(s, slab, object, "Right Redzone",
			endobject, val, s->inuse - s->object_size, ret))
			ret = 0;

		if (slub_debug_orig_size(s) && val == SLUB_RED_ACTIVE) {
			orig_size = get_orig_size(s, object);

			if (s->object_size > orig_size  &&
				!check_bytes_and_report(s, slab, object,
					"kmalloc Redzone", p + orig_size,
					val, s->object_size - orig_size, ret)) {
				ret = 0;
			}
		}
	} else {
		if ((s->flags & SLAB_POISON) && s->object_size < s->inuse) {
			if (!check_bytes_and_report(s, slab, p, "Alignment padding",
				endobject, POISON_INUSE,
				s->inuse - s->object_size, ret))
				ret = 0;
		}
	}

	if (s->flags & SLAB_POISON) {
		if (val != SLUB_RED_ACTIVE && (s->flags & __OBJECT_POISON)) {
			/*
			 * KASAN can save its free meta data inside of the
			 * object at offset 0. Thus, skip checking the part of
			 * the redzone that overlaps with the meta data.
			 */
			kasan_meta_size = kasan_metadata_size(s, true);
			if (kasan_meta_size < s->object_size - 1 &&
			    !check_bytes_and_report(s, slab, p, "Poison",
					p + kasan_meta_size, POISON_FREE,
					s->object_size - kasan_meta_size - 1, ret))
				ret = 0;
			if (kasan_meta_size < s->object_size &&
			    !check_bytes_and_report(s, slab, p, "End Poison",
					p + s->object_size - 1, POISON_END, 1, ret))
				ret = 0;
		}
		/*
		 * check_pad_bytes cleans up on its own.
		 */
		if (!check_pad_bytes(s, slab, p))
			ret = 0;
	}

	/*
	 * Cannot check freepointer while object is allocated if
	 * object and freepointer overlap.
	 */
	if ((freeptr_outside_object(s) || val != SLUB_RED_ACTIVE) &&
	    !check_valid_pointer(s, slab, get_freepointer(s, p))) {
		object_err(s, slab, p, "Freepointer corrupt");
		/*
		 * No choice but to zap it and thus lose the remainder
		 * of the free objects in this slab. May cause
		 * another error because the object count is now wrong.
		 */
		set_freepointer(s, p, NULL);
		ret = 0;
	}

	return ret;
}

/*
 * check_slab() - 检查 slab 页级别的元数据合法性
 *
 * 前提：slab 指针已通过 validate_slab_ptr() 验证合法。
 * 检查内容：
 *   - objects <= 该 order 理论最大值
 *   - inuse <= objects（已用不超过总量）
 *   - frozen == 0（frozen slab 已被标记为不可用，不应再被访问）
 *   - slab_pad_check：页尾 padding 未被越界写
 */
static int check_slab(struct kmem_cache *s, struct slab *slab)
{
	int maxobj;

	maxobj = order_objects(slab_order(slab), s->size);
	if (slab->objects > maxobj) {
		slab_err(s, slab, "objects %u > max %u",
			slab->objects, maxobj);
		return 0;
	}
	if (slab->inuse > slab->objects) {
		slab_err(s, slab, "inuse %u > max %u",
			slab->inuse, slab->objects);
		return 0;
	}
	if (slab->frozen) {
		slab_err(s, slab, "Slab disabled since SLUB metadata consistency check failed");
		return 0;
	}

	/* Slab_pad_check fixes things up after itself */
	slab_pad_check(s, slab);
	return 1;
}

/*
 * on_freelist() - 判断 search 对象是否在 slab 的 freelist 链表上
 *
 * 顺序遍历 freelist，同时执行链表完整性检查：
 *   - 每个 freepointer 必须通过 check_valid_pointer()
 *   - 遍历步数不超过 slab->objects（防止循环链表死循环）
 * 发现损坏时自修复（置 NULL freelist，重置 inuse）。
 * 最后还检查 slab->objects 是否与理论最大值一致，不一致则修正。
 * 调用方须持 slab_lock 保证 freelist 不被并发修改。
 */
static bool on_freelist(struct kmem_cache *s, struct slab *slab, void *search)
{
	int nr = 0;
	void *fp;
	void *object = NULL;
	int max_objects;

	fp = slab->freelist;
	while (fp && nr <= slab->objects) {
		if (fp == search)
			return true;
		if (!check_valid_pointer(s, slab, fp)) {
			if (object) {
				object_err(s, slab, object,
					"Freechain corrupt");
				set_freepointer(s, object, NULL);
				break;
			} else {
				slab_err(s, slab, "Freepointer corrupt");
				slab->freelist = NULL;
				slab->inuse = slab->objects;
				slab_fix(s, "Freelist cleared");
				return false;
			}
		}
		object = fp;
		fp = get_freepointer(s, object);
		nr++;
	}

	if (nr > slab->objects) {
		slab_err(s, slab, "Freelist cycle detected");
		slab->freelist = NULL;
		slab->inuse = slab->objects;
		slab_fix(s, "Freelist cleared");
		return false;
	}

	max_objects = order_objects(slab_order(slab), s->size);
	if (max_objects > MAX_OBJS_PER_PAGE)
		max_objects = MAX_OBJS_PER_PAGE;

	if (slab->objects != max_objects) {
		slab_err(s, slab, "Wrong number of objects. Found %d but should be %d",
			 slab->objects, max_objects);
		slab->objects = max_objects;
		slab_fix(s, "Number of objects adjusted");
	}
	if (slab->inuse != slab->objects - nr) {
		slab_err(s, slab, "Wrong object count. Counter is %d but counted were %d",
			 slab->inuse, slab->objects - nr);
		slab->inuse = slab->objects - nr;
		slab_fix(s, "Object count adjusted");
	}
	return search == NULL;
}

/*
 * trace() - 在 SLAB_TRACE 开启时逐对象打印分配/释放记录
 *
 * 用于极细粒度的调试追踪：每次分配或释放都打印对象地址、inuse 计数、
 * freelist 指针，释放时还 hex dump 对象内容，并附调用栈。
 * 仅在显式设置 SLAB_TRACE 标志时生效，生产内核不使用。
 */
static void trace(struct kmem_cache *s, struct slab *slab, void *object,
								int alloc)
{
	if (s->flags & SLAB_TRACE) {
		pr_info("TRACE %s %s 0x%p inuse=%d fp=0x%p\n",
			s->name,
			alloc ? "alloc" : "free",
			object, slab->inuse,
			slab->freelist);

		if (!alloc)
			print_section(KERN_INFO, "Object ", (void *)object,
					s->object_size);

		dump_stack();
	}
}

/*
 * add_full() / remove_full() - 将满载 slab 加入/移出 per-node full 链表
 *
 * 仅调试模式（SLAB_STORE_USER）下维护 full 链表，用于 validate_slab_cache()
 * 等调试操作需要遍历所有 slab（含满载的）时使用。
 * 正常模式下满载 slab 不挂任何链表（减少开销）。
 */
static void add_full(struct kmem_cache *s,
	struct kmem_cache_node *n, struct slab *slab)
{
	if (!(s->flags & SLAB_STORE_USER))
		return;

	lockdep_assert_held(&n->list_lock);
	list_add(&slab->slab_list, &n->full);
}

static void remove_full(struct kmem_cache *s, struct kmem_cache_node *n, struct slab *slab)
{
	if (!(s->flags & SLAB_STORE_USER))
		return;

	lockdep_assert_held(&n->list_lock);
	list_del(&slab->slab_list);
}

/* node_nr_slabs() / inc_slabs_node() / dec_slabs_node() - 调试统计计数 */
static inline unsigned long node_nr_slabs(struct kmem_cache_node *n)
{
	return atomic_long_read(&n->nr_slabs);
}

static inline void inc_slabs_node(struct kmem_cache *s, int node, int objects)
{
	struct kmem_cache_node *n = get_node(s, node);

	atomic_long_inc(&n->nr_slabs);
	atomic_long_add(objects, &n->total_objects);
}
static inline void dec_slabs_node(struct kmem_cache *s, int node, int objects)
{
	struct kmem_cache_node *n = get_node(s, node);

	atomic_long_dec(&n->nr_slabs);
	atomic_long_sub(objects, &n->total_objects);
}

/*
 * setup_object_debug() - 新分配对象的调试初始化
 *
 * 在分配路径中，对象被取出 freelist 后立即调用。
 * 将对象标记为 SLUB_RED_INACTIVE（未激活状态），并清空 track 字段。
 * 之后 alloc_debug_processing() 会将其标记为 SLUB_RED_ACTIVE。
 */
static void setup_object_debug(struct kmem_cache *s, void *object)
{
	if (!kmem_cache_debug_flags(s, SLAB_STORE_USER|SLAB_RED_ZONE|__OBJECT_POISON))
		return;

	init_object(s, object, SLUB_RED_INACTIVE);
	init_tracking(s, object);
}

/*
 * setup_slab_debug() - 新 slab 页的调试初始化
 *
 * 将整个 slab 内存区域（含对象、padding）全部填为 POISON_INUSE（0x5a）。
 * 之后各对象在被初始化时（setup_object_debug / init_object）会覆写
 * 各自的区域，仅剩对齐 padding 保持 POISON_INUSE，供 check_pad_bytes 检测。
 */
static
void setup_slab_debug(struct kmem_cache *s, struct slab *slab, void *addr)
{
	if (!kmem_cache_debug_flags(s, SLAB_POISON))
		return;

	metadata_access_enable();
	memset(kasan_reset_tag(addr), POISON_INUSE, slab_size(slab));
	metadata_access_disable();
}

/*
 * alloc_consistency_checks() - 分配前对 slab 和对象做一致性检查
 *
 * 三个检查：
 *   1. check_slab()：slab 页级元数据合法性
 *   2. check_valid_pointer()：对象地址在 slab 范围内且对齐
 *   3. check_object(SLUB_RED_INACTIVE)：对象处于空闲状态的 red zone/poison 完整
 */
static inline int alloc_consistency_checks(struct kmem_cache *s,
					struct slab *slab, void *object)
{
	if (!check_slab(s, slab))
		return 0;

	if (!check_valid_pointer(s, slab, object)) {
		object_err(s, slab, object, "Freelist Pointer check fails");
		return 0;
	}

	if (!check_object(s, slab, object, SLUB_RED_INACTIVE))
		return 0;

	return 1;
}

/*
 * alloc_debug_processing() - 分配路径的完整调试处理
 *
 * 调用时机：debug cache 的慢速分配路径，对象从 freelist 取出后。
 *
 * 成功路径：一致性检查通过 → trace → 写 orig_size → 标记 SLUB_RED_ACTIVE
 * 失败路径（检查不通过）：
 *   将 slab 标记为 frozen（不可再分配），设 inuse=objects/freelist=NULL，
 *   后续释放到该 slab 的对象会被泄漏，避免操作已损坏的 freelist。
 */
static noinline bool alloc_debug_processing(struct kmem_cache *s,
			struct slab *slab, void *object, int orig_size)
{
	if (s->flags & SLAB_CONSISTENCY_CHECKS) {
		if (!alloc_consistency_checks(s, slab, object))
			goto bad;
	}

	/* Success. Perform special debug activities for allocs */
	trace(s, slab, object, 1);
	set_orig_size(s, object, orig_size);
	init_object(s, object, SLUB_RED_ACTIVE);
	return true;

bad:
	/*
	 * Let's do the best we can to avoid issues in the future. Marking all
	 * objects as used avoids touching the remaining objects.
	 */
	slab_fix(s, "Marking all objects used");
	slab->inuse = slab->objects;
	slab->freelist = NULL;
	slab->frozen = 1; /* mark consistency-failed slab as frozen */

	return false;
}

/*
 * free_consistency_checks() - 释放路径的一致性检查
 *
 * 四个检查（任一失败返回 0）：
 *   1. 对象指针地址合法性（check_valid_pointer）
 *   2. 对象不在 freelist 上（检测 double-free）
 *   3. 对象处于已分配状态（check_object SLUB_RED_ACTIVE）
 *   4. 对象所属 slab_cache 与传入的 cache 一致
 *      （防止将对象释放到错误的 cache，如 use-after-free 后对象 header 被破坏）
 */
static inline int free_consistency_checks(struct kmem_cache *s,
		struct slab *slab, void *object, unsigned long addr)
{
	if (!check_valid_pointer(s, slab, object)) {
		slab_err(s, slab, "Invalid object pointer 0x%p", object);
		return 0;
	}

	if (on_freelist(s, slab, object)) {
		object_err(s, slab, object, "Object already free");
		return 0;
	}

	if (!check_object(s, slab, object, SLUB_RED_ACTIVE))
		return 0;

	if (unlikely(s != slab->slab_cache)) {
		if (!slab->slab_cache) {
			slab_err(NULL, slab, "No slab cache for object 0x%p",
				 object);
		} else {
			object_err(s, slab, object,
				   "page slab pointer corrupt.");
		}
		return 0;
	}
	return 1;
}

/*
 * parse_slub_debug_flags() - 解析一个 slab_debug 选项块
 *
 * slab_debug 内核参数格式：[flags][,slab_pattern][;[flags][,slab_pattern]...]
 *   - 多个块以 ';' 分隔
 *   - flags 是字母组合：f=一致性检查, z=red zone, p=poison, u=track user,
 *     t=trace, a=failslab, o=高 order 时禁用调试, -=清除所有标志
 *   - ,slab_pattern 限制只对匹配该 glob 模式的 cache 开启调试
 *   - 无 slab_pattern 时对所有 cache 生效（全局标志）
 *
 * @str:   当前块起始位置
 * @flags: 输出：解析到的标志组合
 * @slabs: 输出：slab 模式字符串起始，NULL 表示无限制
 * @init:  true 表示内核启动时解析（对未知字符打印警告）
 *
 * 返回：下一个块的起始位置，NULL 表示已到达字符串末尾
 */
static const char *
parse_slub_debug_flags(const char *str, slab_flags_t *flags, const char **slabs, bool init)
{
	bool higher_order_disable = false;

	/* Skip any completely empty blocks */
	while (*str && *str == ';')
		str++;

	if (*str == ',') {
		/*
		 * No options but restriction on slabs. This means full
		 * debugging for slabs matching a pattern.
		 */
		*flags = DEBUG_DEFAULT_FLAGS;
		goto check_slabs;
	}
	*flags = 0;

	/* Determine which debug features should be switched on */
	for (; *str && *str != ',' && *str != ';'; str++) {
		switch (tolower(*str)) {
		case '-':
			*flags = 0;
			break;
		case 'f':
			*flags |= SLAB_CONSISTENCY_CHECKS;
			break;
		case 'z':
			*flags |= SLAB_RED_ZONE;
			break;
		case 'p':
			*flags |= SLAB_POISON;
			break;
		case 'u':
			*flags |= SLAB_STORE_USER;
			break;
		case 't':
			*flags |= SLAB_TRACE;
			break;
		case 'a':
			*flags |= SLAB_FAILSLAB;
			break;
		case 'o':
			/*
			 * Avoid enabling debugging on caches if its minimum
			 * order would increase as a result.
			 */
			higher_order_disable = true;
			break;
		default:
			if (init)
				pr_err("slab_debug option '%c' unknown. skipped\n", *str);
		}
	}
check_slabs:
	if (*str == ',')
		*slabs = ++str;
	else
		*slabs = NULL;

	/* Skip over the slab list */
	while (*str && *str != ';')
		str++;

	/* Skip any completely empty blocks */
	while (*str && *str == ';')
		str++;

	if (init && higher_order_disable)
		disable_higher_order_debug = 1;

	if (*str)
		return str;
	else
		return NULL;
}

static int __init setup_slub_debug(const char *str, const struct kernel_param *kp)
{
	slab_flags_t flags;
	slab_flags_t global_flags;
	const char *saved_str;
	const char *slab_list;
	bool global_slub_debug_changed = false;
	bool slab_list_specified = false;

	global_flags = DEBUG_DEFAULT_FLAGS;
	if (!str || !*str)
		/*
		 * No options specified. Switch on full debugging.
		 */
		goto out;

	saved_str = str;
	while (str) {
		str = parse_slub_debug_flags(str, &flags, &slab_list, true);

		if (!slab_list) {
			global_flags = flags;
			global_slub_debug_changed = true;
		} else {
			slab_list_specified = true;
			if (flags & SLAB_STORE_USER)
				stack_depot_request_early_init();
		}
	}

	/*
	 * For backwards compatibility, a single list of flags with list of
	 * slabs means debugging is only changed for those slabs, so the global
	 * slab_debug should be unchanged (0 or DEBUG_DEFAULT_FLAGS, depending
	 * on CONFIG_SLUB_DEBUG_ON). We can extended that to multiple lists as
	 * long as there is no option specifying flags without a slab list.
	 */
	if (slab_list_specified) {
		if (!global_slub_debug_changed)
			global_flags = slub_debug;
		slub_debug_string = saved_str;
	}
out:
	slub_debug = global_flags;
	if (slub_debug & SLAB_STORE_USER)
		stack_depot_request_early_init();
	if (slub_debug != 0 || slub_debug_string)
		static_branch_enable(&slub_debug_enabled);
	else
		static_branch_disable(&slub_debug_enabled);
	if ((static_branch_unlikely(&init_on_alloc) ||
	     static_branch_unlikely(&init_on_free)) &&
	    (slub_debug & SLAB_POISON))
		pr_info("mem auto-init: SLAB_POISON will take precedence over init_on_alloc/init_on_free\n");
	return 0;
}

static const struct kernel_param_ops param_ops_slab_debug __initconst = {
	.flags = KERNEL_PARAM_OPS_FL_NOARG,
	.set = setup_slub_debug,
};
__core_param_cb(slab_debug, &param_ops_slab_debug, NULL, 0);
__core_param_cb(slub_debug, &param_ops_slab_debug, NULL, 0);

/*
 * kmem_cache_flags() - 在 cache 创建时将 slab_debug 参数合并到标志位
 *
 * 背景：
 *   内核调试时经常只想对某几个 cache 开启 red zone / poison，而不是
 *   全局开启（全局开启会让系统严重变慢，甚至因内存布局膨胀而启动失败）。
 *   因此 slab_debug 参数支持按名称精确匹配：
 *     slab_debug=FZP,task_struct,dentry
 *   意为只对 task_struct 和 dentry 两个 cache 开启一致性检查+red zone+poison。
 *
 * 这个函数在每个 kmem_cache_create() 时调用，把命令行参数的调试标志
 * "注入"到对应 cache 的 flags 中，之后这些标志会影响分配/释放的慢速路径。
 *
 * 特殊处理：
 *   SLAB_NO_USER_FLAGS：某些内部 cache（如 kmalloc-8 自身）不能接受用户调试
 *     标志，否则 object_size 变化会破坏 kmalloc 的固定大小假设。
 *   SLAB_NOLEAKTRACE：kmemleak 自身的 cache 不能开启 SLAB_STORE_USER，
 *     否则记录分配者栈帧时会触发 kmemleak 再分配，造成递归。
 *     但用户可以通过命令行强制覆盖（用于调试 kmemleak 本身）。
 *
 * 匹配逻辑：
 *   遍历 slub_debug_string 中所有 block（以 ';' 分隔），
 *   每个 block 有一个模式列表（以 ',' 分隔，支持 '*' 前缀匹配）。
 *   第一个匹配的 block 立即生效并返回，不继续匹配后续 block。
 *   无名称约束的全局 block 在循环结束后统一应用（slub_debug_local）。
 */
slab_flags_t kmem_cache_flags(slab_flags_t flags, const char *name)
{
	const char *iter;
	size_t len;
	const char *next_block;
	slab_flags_t block_flags;
	slab_flags_t slub_debug_local = slub_debug;

	if (flags & SLAB_NO_USER_FLAGS)
		return flags;

	/*
	 * If the slab cache is for debugging (e.g. kmemleak) then
	 * don't store user (stack trace) information by default,
	 * but let the user enable it via the command line below.
	 */
	if (flags & SLAB_NOLEAKTRACE)
		slub_debug_local &= ~SLAB_STORE_USER;

	len = strlen(name);
	next_block = slub_debug_string;
	/* Go through all blocks of debug options, see if any matches our slab's name */
	while (next_block) {
		next_block = parse_slub_debug_flags(next_block, &block_flags, &iter, false);
		if (!iter)
			continue;
		/* Found a block that has a slab list, search it */
		while (*iter) {
			const char *end, *glob;
			size_t cmplen;

			end = strchrnul(iter, ',');
			if (next_block && next_block < end)
				end = next_block - 1;

			glob = strnchr(iter, end - iter, '*');
			if (glob)
				cmplen = glob - iter;
			else
				cmplen = max_t(size_t, len, (end - iter));

			if (!strncmp(name, iter, cmplen)) {
				flags |= block_flags;
				return flags;
			}

			if (!*end || *end == ';')
				break;
			iter = end + 1;
		}
	}

	return flags | slub_debug_local;
}
#else /* !CONFIG_SLUB_DEBUG */
static inline void setup_object_debug(struct kmem_cache *s, void *object) {}
static inline
void setup_slab_debug(struct kmem_cache *s, struct slab *slab, void *addr) {}

static inline bool alloc_debug_processing(struct kmem_cache *s,
	struct slab *slab, void *object, int orig_size) { return true; }

static inline bool free_debug_processing(struct kmem_cache *s,
	struct slab *slab, void *head, void *tail, int *bulk_cnt,
	unsigned long addr, depot_stack_handle_t handle) { return true; }

static inline void slab_pad_check(struct kmem_cache *s, struct slab *slab) {}
static inline int check_object(struct kmem_cache *s, struct slab *slab,
			void *object, u8 val) { return 1; }
static inline depot_stack_handle_t set_track_prepare(gfp_t gfp_flags) { return 0; }
static inline void set_track(struct kmem_cache *s, void *object,
			     enum track_item alloc, unsigned long addr, gfp_t gfp_flags) {}
static inline void add_full(struct kmem_cache *s, struct kmem_cache_node *n,
					struct slab *slab) {}
static inline void remove_full(struct kmem_cache *s, struct kmem_cache_node *n,
					struct slab *slab) {}
slab_flags_t kmem_cache_flags(slab_flags_t flags, const char *name)
{
	return flags;
}
#define slub_debug 0

#define disable_higher_order_debug 0

static inline unsigned long node_nr_slabs(struct kmem_cache_node *n)
							{ return 0; }
static inline void inc_slabs_node(struct kmem_cache *s, int node,
							int objects) {}
static inline void dec_slabs_node(struct kmem_cache *s, int node,
							int objects) {}
#endif /* CONFIG_SLUB_DEBUG */

/*
 * OBJCGS_CLEAR_MASK - 为 slab 内部分配（obj_exts、sheaf）清除的 GFP 标志
 *
 * 背景：
 *   当用户调用 kmalloc(size, __GFP_DMA | __GFP_ACCOUNT) 时，SLUB 可能需要
 *   为该 slab 额外分配内部元数据（obj_exts 数组、sheaf 结构体等）。
 *   如果直接把用户的 GFP 标志透传给这些内部分配，会出现以下问题：
 *
 *   __GFP_DMA：obj_exts 不需要 DMA 内存，强制 DMA 分配会浪费稀缺的 DMA zone
 *   __GFP_RECLAIMABLE：obj_exts 是 slab 的内部管理结构，不是用户可回收数据
 *   __GFP_ACCOUNT：不应对 slab 内部结构收取 memcg 费用，否则 memcg 统计会
 *     递归：分配 obj_exts 本身又触发 memcg 计费，再分配 obj_exts...
 *   __GFP_NOFAIL：内部分配失败应当可降级处理，而不是触发 OOM-killer
 *   __GFP_THISNODE：用户要求在特定 NUMA 节点分配，但 slab 内部结构应放在
 *     最容易分配的节点，不应受此约束
 *   __GFP_COMP：仅用于 slab 页本身的申请，obj_exts 是普通小对象，不需要
 *     compound page 结构
 *
 *   解决方案：在为 slab 内部结构分配内存前，用这个掩码屏蔽掉上述标志。
 */
#define OBJCGS_CLEAR_MASK	(__GFP_DMA | __GFP_RECLAIMABLE | \
				__GFP_ACCOUNT | __GFP_NOFAIL | \
				__GFP_THISNODE | __GFP_COMP)

#ifdef CONFIG_SLAB_OBJ_EXT

#ifdef CONFIG_MEM_ALLOC_PROFILING_DEBUG

static inline void mark_obj_codetag_empty(const void *obj)
{
	struct slab *obj_slab;
	unsigned long slab_exts;

	obj_slab = virt_to_slab(obj);
	slab_exts = slab_obj_exts(obj_slab);
	if (slab_exts) {
		get_slab_obj_exts(slab_exts);
		unsigned int offs = obj_to_index(obj_slab->slab_cache,
						 obj_slab, obj);
		struct slabobj_ext *ext = slab_obj_ext(obj_slab,
						       slab_exts, offs);

		if (unlikely(is_codetag_empty(&ext->ref))) {
			put_slab_obj_exts(slab_exts);
			return;
		}

		/* codetag should be NULL here */
		WARN_ON(ext->ref.ct);
		set_codetag_empty(&ext->ref);
		put_slab_obj_exts(slab_exts);
	}
}

static inline bool mark_failed_objexts_alloc(struct slab *slab)
{
	return cmpxchg(&slab->obj_exts, 0, OBJEXTS_ALLOC_FAIL) == 0;
}

static inline void handle_failed_objexts_alloc(unsigned long obj_exts,
			struct slabobj_ext *vec, unsigned int objects)
{
	/*
	 * If vector previously failed to allocate then we have live
	 * objects with no tag reference. Mark all references in this
	 * vector as empty to avoid warnings later on.
	 */
	if (obj_exts == OBJEXTS_ALLOC_FAIL) {
		unsigned int i;

		for (i = 0; i < objects; i++)
			set_codetag_empty(&vec[i].ref);
	}
}

#else /* CONFIG_MEM_ALLOC_PROFILING_DEBUG */

static inline void mark_obj_codetag_empty(const void *obj) {}
static inline bool mark_failed_objexts_alloc(struct slab *slab) { return false; }
static inline void handle_failed_objexts_alloc(unsigned long obj_exts,
			struct slabobj_ext *vec, unsigned int objects) {}

#endif /* CONFIG_MEM_ALLOC_PROFILING_DEBUG */

static inline void init_slab_obj_exts(struct slab *slab)
{
	slab->obj_exts = 0;
}

/*
 * obj_exts_alloc_size() - 计算 obj_exts 数组的分配大小，规避自引用死锁
 *
 * 问题背景：
 *   obj_exts 数组本身需要从 kmalloc 分配（大小 = objects * sizeof(slabobj_ext)）。
 *   若这个大小恰好落在被追踪的 cache s 所属的 kmalloc 桶中，
 *   obj_exts 数组就会被分配到 s 的某个 slab 页里，
 *   导致该 slab 页始终有一个"已用"对象（obj_exts 自身），
 *   永远无法全空，因而永远无法被归还给伙伴系统——形成内存泄漏。
 *
 * 解决方案：
 *   检测到 obj_exts 的分配大小与 s->object_size 落在同一个 kmalloc 桶时，
 *   将分配大小加一（+1 byte），让它跳到下一个更大的桶，
 *   从而保证 obj_exts 来自不同的 cache，打破自引用。
 *
 *   注意不能直接比较 s 与目标 kmalloc cache 的指针，因为 kmalloc 的
 *   "partitioned caches"特性会根据调用地址/类型将同大小对象分配到
 *   不同的 cache 实例，需要比较 object_size 而非指针。
 */
static inline size_t obj_exts_alloc_size(struct kmem_cache *s,
					 struct slab *slab, gfp_t gfp)
{
	size_t sz = sizeof(struct slabobj_ext) * slab->objects;
	struct kmem_cache *obj_exts_cache;

	if (sz > KMALLOC_MAX_CACHE_SIZE)
		return sz;

	if (!is_kmalloc_normal(s))
		return sz;

	obj_exts_cache = kmalloc_slab(sz, NULL, gfp, __kmalloc_token(0));
	/*
	 * We can't simply compare s with obj_exts_cache, because partitioned kmalloc
	 * caches have multiple caches per size, selected by caller address or type.
	 * Since caller address or type may differ between kmalloc_slab() and actual
	 * allocation, bump size when sizes are equal.
	 */
	if (s->object_size == obj_exts_cache->object_size)
		return obj_exts_cache->object_size + 1;

	return sz;
}

/*
 * alloc_slab_obj_exts() - 为 slab 页分配 per-object 扩展元数据数组
 *
 * 背景：
 *   memcg 计费和 alloc_tag 追踪都需要对 slab 内每个对象记录额外信息
 *   （属于哪个 cgroup、是哪个调用点分配的）。这些信息以数组形式存储，
 *   大小 = objects * sizeof(slabobj_ext)，通过 slab->obj_exts 指向。
 *
 *   这个函数可能被多个 CPU 并发调用（slab 从 partial list 取出后，
 *   多个 CPU 可能同时发现 obj_exts 为空并尝试分配），
 *   因此用 cmpxchg 保证只有一个成功，其余的把自己分配的 vec 释放掉。
 *
 * 三种设置路径：
 *   1. new_slab=true（全新 slab 页，无并发）：直接赋值，不需要 CAS
 *   2. old_exts 已有值（并发路径先完成了）：释放自己的 vec，复用已有的
 *   3. CAS 竞争：有其他 CPU 在两次 READ_ONCE 之间修改了 obj_exts，重试
 *
 * SLAB_ALLOC_NO_RECURSE：防止 obj_exts 的分配本身又触发 obj_exts 分配，
 *   避免递归。加了此 flag 的分配路径不会再调用本函数。
 */
int alloc_slab_obj_exts(struct slab *slab, struct kmem_cache *s,
			gfp_t gfp, unsigned int alloc_flags)
{
	const bool allow_spin = alloc_flags_allow_spinning(alloc_flags);
	unsigned int objects = objs_per_slab(s, slab);
	bool new_slab = alloc_flags & SLAB_ALLOC_NEW_SLAB;
	unsigned long new_exts;
	unsigned long old_exts;
	struct slabobj_ext *vec;
	size_t sz;

	gfp &= ~OBJCGS_CLEAR_MASK;
	/* Prevent recursive extension vector allocation */
	alloc_flags |= SLAB_ALLOC_NO_RECURSE;
	alloc_flags &= ~SLAB_ALLOC_NEW_SLAB;

	sz = obj_exts_alloc_size(s, slab, gfp);

	/* This will use kmalloc_nolock() if alloc_flags say so */
	vec = kmalloc_flags(sz, gfp | __GFP_ZERO, alloc_flags, slab_nid(slab));

	if (!vec) {
		/*
		 * Try to mark vectors which failed to allocate.
		 * If this operation fails, there may be a racing process
		 * that has already completed the allocation.
		 */
		if (!mark_failed_objexts_alloc(slab) &&
		    slab_obj_exts(slab))
			return 0;

		return -ENOMEM;
	}

	VM_WARN_ON_ONCE(virt_to_slab(vec) != NULL &&
			virt_to_slab(vec)->slab_cache == s);

	new_exts = (unsigned long)vec;
#ifdef CONFIG_MEMCG
	new_exts |= MEMCG_DATA_OBJEXTS;
#endif
retry:
	old_exts = READ_ONCE(slab->obj_exts);
	handle_failed_objexts_alloc(old_exts, vec, objects);

	if (new_slab) {
		/*
		 * If the slab is brand new and nobody can yet access its
		 * obj_exts, no synchronization is required and obj_exts can
		 * be simply assigned.
		 */
		slab->obj_exts = new_exts;
	} else if (old_exts & ~OBJEXTS_FLAGS_MASK) {
		/*
		 * If the slab is already in use, somebody can allocate and
		 * assign slabobj_exts in parallel. In this case the existing
		 * objcg vector should be reused.
		 */
		mark_obj_codetag_empty(vec);
		if (unlikely(!allow_spin))
			kfree_nolock(vec);
		else
			kfree(vec);
		return 0;
	} else if (cmpxchg(&slab->obj_exts, old_exts, new_exts) != old_exts) {
		/* Retry if a racing thread changed slab->obj_exts from under us. */
		goto retry;
	}

	if (allow_spin)
		kmemleak_not_leak(vec);
	return 0;
}

/*
 * free_slab_obj_exts() - 释放 slab 页的 obj_exts 数组
 *
 * 对应 alloc_slab_obj_exts()，在 slab 页归还给伙伴系统前调用。
 * 三种情况：
 *   1. obj_exts == 0 或 OBJEXTS_ALLOC_FAIL（之前分配失败）：
 *      只需清零 obj_exts 字段，无内存需要释放
 *   2. obj_exts 在 slab 页内部（leftover 或 in-object 方式）：
 *      slab 页本身被伙伴系统回收时这块内存自然消失，只需清零指针
 *   3. obj_exts 是单独 kmalloc 的（out-of-slab 方式）：
 *      先 mark_obj_codetag_empty 避免 alloc_tag_sub 误报，再 kfree
 *
 * allow_spin=false 时使用 kfree_nolock（NMI 安全版本），
 * 用于无法自旋等待锁的上下文（如 NMI 处理程序中的 slab 释放）。
 */
static inline void free_slab_obj_exts(struct slab *slab, bool allow_spin)
{
	struct slabobj_ext *obj_exts;

	obj_exts = (struct slabobj_ext *)slab_obj_exts(slab);
	if (!obj_exts) {
		/*
		 * If obj_exts allocation failed, slab->obj_exts is set to
		 * OBJEXTS_ALLOC_FAIL. In this case, we end up here and should
		 * clear the flag.
		 */
		slab->obj_exts = 0;
		return;
	}

	if (obj_exts_in_slab(slab->slab_cache, slab)) {
		slab->obj_exts = 0;
		return;
	}

	/*
	 * obj_exts was created with SLAB_ALLOC_NO_RECURSE flag, therefore its
	 * corresponding extension will be NULL. alloc_tag_sub() will throw a
	 * warning if slab has extensions but the extension of an object is
	 * NULL, therefore replace NULL with CODETAG_EMPTY to indicate that
	 * the extension for obj_exts is expected to be NULL.
	 */
	mark_obj_codetag_empty(obj_exts);
	if (allow_spin)
		kfree(obj_exts);
	else
		kfree_nolock(obj_exts);
	slab->obj_exts = 0;
}

/*
 * alloc_slab_obj_exts_early() - 在新 slab 页刚分配后立刻初始化 obj_exts
 *
 * 背景：
 *   全新 slab 页尚未放入任何链表，不存在并发访问，因此可以简单地直接
 *   写 slab->obj_exts，无需 CAS。相比 alloc_slab_obj_exts() 的并发安全
 *   版本，这里能在无锁情况下完成初始化，性能更好。
 *
 * 三种存储策略（按优先级）：
 *   1. 优先：leftover 空间（slab 页尾部的对齐剩余）
 *      无额外内存开销，obj_exts 与 slab 数据共用同一张页
 *   2. 其次：in-object 存储（对象尾部的 padding 区域，仅 64 位）
 *      同样无额外开销，但需要拉伸 slab 步长（slab_set_stride）
 *      让每个对象尾部刚好有 sizeof(slabobj_ext) 的空间
 *   3. 最后：out-of-slab kmalloc（由 alloc_slab_obj_exts 在首次使用时惰性分配）
 *      此函数不处理该情况，返回后 slab->obj_exts 保持 0，
 *      后续访问会触发 alloc_slab_obj_exts() 的按需分配路径
 *
 * 必须在 slab 进入 freelist 前调用，否则 alloc_slab_obj_exts() 的
 * CAS 检测（new_slab=false 路径）会因并发问题出错。
 */
static void alloc_slab_obj_exts_early(struct kmem_cache *s, struct slab *slab)
{
	void *addr;
	unsigned long obj_exts;

	/* Initialize stride early to avoid memory ordering issues */
	slab_set_stride(slab, sizeof(struct slabobj_ext));

	if (!need_slab_obj_exts(s))
		return;

	if (obj_exts_fit_within_slab_leftover(s, slab)) {
		addr = slab_address(slab) + obj_exts_offset_in_slab(s, slab);
		addr = kasan_reset_tag(addr);
		obj_exts = (unsigned long)addr;

		get_slab_obj_exts(obj_exts);
		memset(addr, 0, obj_exts_size_in_slab(slab));
		put_slab_obj_exts(obj_exts);

#ifdef CONFIG_MEMCG
		obj_exts |= MEMCG_DATA_OBJEXTS;
#endif
		slab->obj_exts = obj_exts;
	} else if (s->flags & SLAB_OBJ_EXT_IN_OBJ) {
		unsigned int offset = obj_exts_offset_in_object(s);

		obj_exts = (unsigned long)slab_address(slab);
		obj_exts += s->red_left_pad;
		obj_exts += offset;

		get_slab_obj_exts(obj_exts);
		for_each_object(addr, s, slab_address(slab), slab->objects)
			memset(kasan_reset_tag(addr) + offset, 0,
			       sizeof(struct slabobj_ext));
		put_slab_obj_exts(obj_exts);

#ifdef CONFIG_MEMCG
		obj_exts |= MEMCG_DATA_OBJEXTS;
#endif
		slab->obj_exts = obj_exts;
		slab_set_stride(slab, s->size);
	}
}

#else /* CONFIG_SLAB_OBJ_EXT */

static inline void mark_obj_codetag_empty(const void *obj)
{
}

static inline void init_slab_obj_exts(struct slab *slab)
{
}

static int alloc_slab_obj_exts(struct slab *slab, struct kmem_cache *s,
			       gfp_t gfp, unsigned int alloc_flags)
{
	return 0;
}

static inline void free_slab_obj_exts(struct slab *slab, bool allow_spin)
{
}

static inline void alloc_slab_obj_exts_early(struct kmem_cache *s,
						       struct slab *slab)
{
}

#endif /* CONFIG_SLAB_OBJ_EXT */

#ifdef CONFIG_MEM_ALLOC_PROFILING

static inline unsigned long
prepare_slab_obj_exts_hook(struct kmem_cache *s, struct slab *slab,
			   gfp_t flags, unsigned int alloc_flags, void *p)
{
	if (!slab_obj_exts(slab) &&
	    alloc_slab_obj_exts(slab, s, flags, alloc_flags)) {
		pr_warn_once("%s, %s: Failed to create slab extension vector!\n",
			     __func__, s->name);
		return 0;
	}

	return slab_obj_exts(slab);
}


/* Should be called only if mem_alloc_profiling_enabled() */
static noinline void
__alloc_tagging_slab_alloc_hook(struct kmem_cache *s, void *object, gfp_t flags,
				unsigned int alloc_flags)
{
	unsigned long obj_exts;
	struct slabobj_ext *obj_ext;
	struct slab *slab;

	if (!object)
		return;

	if (s->flags & (SLAB_NO_OBJ_EXT | SLAB_NOLEAKTRACE))
		return;

	if (alloc_flags & SLAB_ALLOC_NO_RECURSE)
		return;

	slab = virt_to_slab(object);
	obj_exts = prepare_slab_obj_exts_hook(s, slab, flags, alloc_flags, object);
	/*
	 * Currently obj_exts is used only for allocation profiling.
	 * If other users appear then mem_alloc_profiling_enabled()
	 * check should be added before alloc_tag_add().
	 */
	if (obj_exts) {
		unsigned int obj_idx = obj_to_index(s, slab, object);

		get_slab_obj_exts(obj_exts);
		obj_ext = slab_obj_ext(slab, obj_exts, obj_idx);
		alloc_tag_add(&obj_ext->ref, current->alloc_tag, s->size);
		put_slab_obj_exts(obj_exts);
	} else {
		alloc_tag_set_inaccurate(current->alloc_tag);
	}
}

static inline void
alloc_tagging_slab_alloc_hook(struct kmem_cache *s, void *object, gfp_t flags,
			      unsigned int alloc_flags)
{
	if (mem_alloc_profiling_enabled())
		__alloc_tagging_slab_alloc_hook(s, object, flags, alloc_flags);
}

/* Should be called only if mem_alloc_profiling_enabled() */
static noinline void
__alloc_tagging_slab_free_hook(struct kmem_cache *s, struct slab *slab, void **p,
			       int objects)
{
	int i;
	unsigned long obj_exts;

	/* slab->obj_exts might not be NULL if it was created for MEMCG accounting. */
	if (s->flags & (SLAB_NO_OBJ_EXT | SLAB_NOLEAKTRACE))
		return;

	obj_exts = slab_obj_exts(slab);
	if (!obj_exts)
		return;

	get_slab_obj_exts(obj_exts);
	for (i = 0; i < objects; i++) {
		unsigned int off = obj_to_index(s, slab, p[i]);

		alloc_tag_sub(&slab_obj_ext(slab, obj_exts, off)->ref, s->size);
	}
	put_slab_obj_exts(obj_exts);
}

static inline void
alloc_tagging_slab_free_hook(struct kmem_cache *s, struct slab *slab, void **p,
			     int objects)
{
	if (mem_alloc_profiling_enabled())
		__alloc_tagging_slab_free_hook(s, slab, p, objects);
}

#else /* CONFIG_MEM_ALLOC_PROFILING */

static inline void
alloc_tagging_slab_alloc_hook(struct kmem_cache *s, void *object, gfp_t flags,
			      unsigned int alloc_flags)
{
}

static inline void
alloc_tagging_slab_free_hook(struct kmem_cache *s, struct slab *slab, void **p,
			     int objects)
{
}

#endif /* CONFIG_MEM_ALLOC_PROFILING */


#ifdef CONFIG_MEMCG

static void memcg_alloc_abort_single(struct kmem_cache *s, void *object);

static __fastpath_inline
bool memcg_slab_post_alloc_hook(struct kmem_cache *s, gfp_t flags,
				size_t size, void **p,
				const struct slab_alloc_context *ac)
{
	if (likely(!memcg_kmem_online()))
		return true;

	if (likely(!(flags & __GFP_ACCOUNT) && !(s->flags & SLAB_ACCOUNT)))
		return true;

	if (likely(__memcg_slab_post_alloc_hook(s, ac->lru, flags,
						ac->alloc_flags, size, p)))
		return true;

	if (likely(size == 1)) {
		memcg_alloc_abort_single(s, *p);
		*p = NULL;
	} else {
		kmem_cache_free_bulk(s, size, p);
	}

	return false;
}

static __fastpath_inline
void memcg_slab_free_hook(struct kmem_cache *s, struct slab *slab, void **p,
			  int objects)
{
	unsigned long obj_exts;

	if (!memcg_kmem_online())
		return;

	obj_exts = slab_obj_exts(slab);
	if (likely(!obj_exts))
		return;

	get_slab_obj_exts(obj_exts);
	__memcg_slab_free_hook(s, slab, p, objects, obj_exts);
	put_slab_obj_exts(obj_exts);
}

static __fastpath_inline
bool memcg_slab_post_charge(void *p, gfp_t flags)
{
	unsigned long obj_exts;
	struct slabobj_ext *obj_ext;
	struct kmem_cache *s;
	struct page *page;
	struct slab *slab;
	unsigned long off;

	page = virt_to_page(p);
	if (PageLargeKmalloc(page)) {
		unsigned int order;
		int size;

		if (PageMemcgKmem(page))
			return true;

		order = large_kmalloc_order(page);
		if (__memcg_kmem_charge_page(page, flags, order))
			return false;

		/*
		 * This page has already been accounted in the global stats but
		 * not in the memcg stats. So, subtract from the global and use
		 * the interface which adds to both global and memcg stats.
		 */
		size = PAGE_SIZE << order;
		mod_node_page_state(page_pgdat(page), NR_SLAB_UNRECLAIMABLE_B, -size);
		mod_lruvec_page_state(page, NR_SLAB_UNRECLAIMABLE_B, size);
		return true;
	}

	slab = page_slab(page);
	s = slab->slab_cache;

	/*
	 * Ignore KMALLOC_NORMAL cache to avoid possible circular dependency
	 * of slab_obj_exts being allocated from the same slab and thus the slab
	 * becoming effectively unfreeable.
	 */
	if (is_kmalloc_normal(s))
		return true;

	/* Ignore already charged objects. */
	obj_exts = slab_obj_exts(slab);
	if (obj_exts) {
		get_slab_obj_exts(obj_exts);
		off = obj_to_index(s, slab, p);
		obj_ext = slab_obj_ext(slab, obj_exts, off);
		if (unlikely(obj_ext->objcg)) {
			put_slab_obj_exts(obj_exts);
			return true;
		}
		put_slab_obj_exts(obj_exts);
	}

	return __memcg_slab_post_alloc_hook(s, NULL, flags, SLAB_ALLOC_DEFAULT,
					    1, &p);
}

#else /* CONFIG_MEMCG */
static inline bool memcg_slab_post_alloc_hook(struct kmem_cache *s,
					      gfp_t flags,
					      size_t size, void **p,
					      const struct slab_alloc_context *ac)
{
	return true;
}

static inline void memcg_slab_free_hook(struct kmem_cache *s, struct slab *slab,
					void **p, int objects)
{
}

static inline bool memcg_slab_post_charge(void *p, gfp_t flags)
{
	return true;
}
#endif /* CONFIG_MEMCG */

#ifdef CONFIG_SLUB_RCU_DEBUG
static void slab_free_after_rcu_debug(struct rcu_head *rcu_head);

struct rcu_delayed_free {
	struct rcu_head head;
	void *object;
};
#endif

/*
 * Hooks for other subsystems that check memory allocations. In a typical
 * production configuration these hooks all should produce no code at all.
 *
 * Returns true if freeing of the object can proceed, false if its reuse
 * was delayed by CONFIG_SLUB_RCU_DEBUG or KASAN quarantine, or it was returned
 * to KFENCE.
 *
 * For objects allocated via kmalloc_nolock(), only a subset of alloc hooks
 * are invoked, so some free hooks must handle asymmetric hook calls.
 *
 * Alloc hooks called for kmalloc_nolock():
 * - kmsan_slab_alloc()
 * - kasan_slab_alloc()
 * - memcg_slab_post_alloc_hook()
 * - alloc_tagging_slab_alloc_hook()
 *
 * Free hooks that must handle missing corresponding alloc hooks:
 * - kmemleak_free_recursive()
 * - kfence_free()
 *
 * Free hooks that have no alloc hook counterpart, and thus safe to call:
 * - debug_check_no_locks_freed()
 * - debug_check_no_obj_freed()
 * - __kcsan_check_access()
 */
static __always_inline
bool slab_free_hook(struct kmem_cache *s, void *x, bool init,
		    bool after_rcu_delay)
{
	/* Are the object contents still accessible? */
	bool still_accessible = (s->flags & SLAB_TYPESAFE_BY_RCU) && !after_rcu_delay;

	kmemleak_free_recursive(x, s->flags);
	kmsan_slab_free(s, x);

	debug_check_no_locks_freed(x, s->object_size);

	if (!(s->flags & SLAB_DEBUG_OBJECTS))
		debug_check_no_obj_freed(x, s->object_size);

	/* Use KCSAN to help debug racy use-after-free. */
	if (!still_accessible)
		__kcsan_check_access(x, s->object_size,
				     KCSAN_ACCESS_WRITE | KCSAN_ACCESS_ASSERT);

	if (kfence_free(x))
		return false;

	/*
	 * Give KASAN a chance to notice an invalid free operation before we
	 * modify the object.
	 */
	if (kasan_slab_pre_free(s, x))
		return false;

#ifdef CONFIG_SLUB_RCU_DEBUG
	if (still_accessible) {
		struct rcu_delayed_free *delayed_free;

		delayed_free = kmalloc_obj(*delayed_free, GFP_NOWAIT);
		if (delayed_free) {
			/*
			 * Let KASAN track our call stack as a "related work
			 * creation", just like if the object had been freed
			 * normally via kfree_rcu().
			 * We have to do this manually because the rcu_head is
			 * not located inside the object.
			 */
			kasan_record_aux_stack(x);

			delayed_free->object = x;
			call_rcu(&delayed_free->head, slab_free_after_rcu_debug);
			return false;
		}
	}
#endif /* CONFIG_SLUB_RCU_DEBUG */

	/*
	 * As memory initialization might be integrated into KASAN,
	 * kasan_slab_free and initialization memset's must be
	 * kept together to avoid discrepancies in behavior.
	 *
	 * The initialization memset's clear the object and the metadata,
	 * but don't touch the SLAB redzone.
	 *
	 * The object's freepointer is also avoided if stored outside the
	 * object.
	 */
	if (unlikely(init)) {
		int rsize;
		unsigned int inuse, orig_size;

		inuse = get_info_end(s);
		orig_size = get_orig_size(s, x);
		if (!kasan_has_integrated_init())
			memset(kasan_reset_tag(x), 0, orig_size);
		rsize = (s->flags & SLAB_RED_ZONE) ? s->red_left_pad : 0;
		memset((char *)kasan_reset_tag(x) + inuse, 0,
		       s->size - inuse - rsize);
		/*
		 * Restore orig_size, otherwise kmalloc redzone overwritten
		 * would be reported
		 */
		set_orig_size(s, x, orig_size);

	}
	/* KASAN might put x into memory quarantine, delaying its reuse. */
	return !kasan_slab_free(s, x, init, still_accessible, false);
}

static __fastpath_inline
bool slab_free_freelist_hook(struct kmem_cache *s, void **head, void **tail,
			     int *cnt)
{

	void *object;
	void *next = *head;
	void *old_tail = *tail;
	bool init;

	if (is_kfence_address(next)) {
		slab_free_hook(s, next, false, false);
		return false;
	}

	/* Head and tail of the reconstructed freelist */
	*head = NULL;
	*tail = NULL;

	init = slab_want_init_on_free(s);

	do {
		object = next;
		next = get_freepointer(s, object);

		/* If object's reuse doesn't have to be delayed */
		if (likely(slab_free_hook(s, object, init, false))) {
			/* Move object to the new freelist */
			set_freepointer(s, object, *head);
			*head = object;
			if (!*tail)
				*tail = object;
		} else {
			/*
			 * Adjust the reconstructed freelist depth
			 * accordingly if object's reuse is delayed.
			 */
			--(*cnt);
		}
	} while (object != old_tail);

	return *head != NULL;
}

static inline void *setup_object(struct kmem_cache *s, void *object)
{
	setup_object_debug(s, object);
	object = kasan_init_slab_obj(s, object);
	if (unlikely(s->ctor)) {
		kasan_unpoison_new_object(s, object);
		s->ctor(object);
		kasan_poison_new_object(s, object);
	}
	return object;
}

static struct slab_sheaf *__alloc_empty_sheaf(struct kmem_cache *s, gfp_t gfp,
				unsigned int alloc_flags, unsigned int capacity)
{
	struct slab_sheaf *sheaf;
	size_t sheaf_size;

	/*
	 * Prevent recursion to the same cache, or a deep stack of kmallocs of
	 * varying sizes (sheaf capacity might differ for each kmalloc size
	 * bucket)
	 */
	if (s->flags & SLAB_KMALLOC)
		alloc_flags |= SLAB_ALLOC_NO_RECURSE;

	sheaf_size = struct_size(sheaf, objects, capacity);
	sheaf = kmalloc_flags(sheaf_size, gfp | __GFP_ZERO, alloc_flags, NUMA_NO_NODE);

	if (unlikely(!sheaf))
		return NULL;

	sheaf->cache = s;

	stat(s, SHEAF_ALLOC);

	return sheaf;
}

static inline struct slab_sheaf *alloc_empty_sheaf(struct kmem_cache *s,
				gfp_t gfp, unsigned int alloc_flags)
{
	if (alloc_flags & SLAB_ALLOC_NO_RECURSE)
		return NULL;

	gfp &= ~OBJCGS_CLEAR_MASK;

	return __alloc_empty_sheaf(s, gfp, alloc_flags, s->sheaf_capacity);
}

static void free_empty_sheaf(struct kmem_cache *s, struct slab_sheaf *sheaf)
{
	/*
	 * If the sheaf was created with SLAB_ALLOC_NO_RECURSE flag then its
	 * corresponding extension is NULL and alloc_tag_sub() will throw a
	 * warning, therefore replace NULL with CODETAG_EMPTY to indicate
	 * that the extension for this sheaf is expected to be NULL.
	 */
	if (s->flags & SLAB_KMALLOC)
		mark_obj_codetag_empty(sheaf);

	VM_WARN_ON_ONCE(sheaf->size > 0);
	kfree(sheaf);

	stat(s, SHEAF_FREE);
}

static unsigned int
refill_objects(struct kmem_cache *s, void **p, gfp_t gfp, unsigned int min,
	       unsigned int max);

/*
 * refill_sheaf() - 将空/半空 sheaf 从 partial list 或伙伴系统填满
 *
 * 背景：per-CPU 快速路径依赖 sheaf 非空。当 main sheaf 耗尽且 barn 也
 * 没有满 sheaf 可换时，必须走这条路：从 partial slab 批量取对象填满 sheaf，
 * 为后续的高速分配做准备。
 *
 * to_fill = 需要填入的对象数（满容量 - 当前已有数）。
 * 调用 refill_objects() 按优先级取对象：
 *   本节点 partial list → 其他节点 partial list → 新 slab（伙伴系统）
 * 填充后更新 sheaf->size，若填入数少于 to_fill 返回 -ENOMEM。
 */
static int refill_sheaf(struct kmem_cache *s, struct slab_sheaf *sheaf,
			 gfp_t gfp)
{
	int to_fill = s->sheaf_capacity - sheaf->size;
	int filled;

	if (!to_fill)
		return 0;

	filled = refill_objects(s, &sheaf->objects[sheaf->size], gfp, to_fill,
				to_fill);

	sheaf->size += filled;

	stat_add(s, SHEAF_REFILL, filled);

	if (filled < to_fill)
		return -ENOMEM;

	return 0;
}

/*
 * PCS_BATCH_MAX：每次批量刷出的最大对象数
 *
 * 背景：刷出 sheaf 对象时需要对每个对象调用 __slab_free，这个操作
 * 不能在 cpu_sheaves->lock 持有期间进行（因为 __slab_free 可能触发
 * 内存分配、锁嵌套等）。解决方案：先在锁内把对象指针 memcpy 到栈上
 * 的临时数组，解锁后再批量释放。PCS_BATCH_MAX 限制栈上数组大小（每个
 * 指针 8 字节，32 个 = 256 字节），在栈空间和批量效率间取平衡。
 */
#define PCS_BATCH_MAX	32U

static void __kmem_cache_free_bulk(struct kmem_cache *s, size_t size, void **p);

/*
 * __sheaf_flush_main_batch() - 批量刷出 main sheaf 中的一批对象
 *
 * 在 cpu_sheaves->lock 内将最多 PCS_BATCH_MAX 个对象指针 memcpy 到栈上，
 * 然后解锁，再调用 __kmem_cache_free_bulk 真正执行释放。
 * 这样把"需要持锁的指针读取"和"可能休眠/加锁的释放操作"解耦。
 *
 * 调用方须已持 cpu_sheaves->lock；本函数返回时锁已释放。
 * 返回值：main sheaf 还剩多少对象需要刷出（0 表示刷干净了）。
 */
static unsigned int __sheaf_flush_main_batch(struct kmem_cache *s)
{
	struct slub_percpu_sheaves *pcs;
	unsigned int batch, remaining;
	void *objects[PCS_BATCH_MAX];
	struct slab_sheaf *sheaf;

	lockdep_assert_held(this_cpu_ptr(&s->cpu_sheaves->lock));

	pcs = this_cpu_ptr(s->cpu_sheaves);
	sheaf = pcs->main;

	batch = min(PCS_BATCH_MAX, sheaf->size);

	sheaf->size -= batch;
	memcpy(objects, sheaf->objects + sheaf->size, batch * sizeof(void *));

	remaining = sheaf->size;

	local_unlock(&s->cpu_sheaves->lock);

	__kmem_cache_free_bulk(s, batch, &objects[0]);

	stat_add(s, SHEAF_FLUSH, batch);

	return remaining;
}

/*
 * sheaf_flush_main() - 循环批量刷出直到 main sheaf 完全清空
 * sheaf_try_flush_main() - 尝试版本（trylock，失败立即返回）
 *
 * sheaf_flush_main：用于 flush_all 等确保必须完全刷出的场景（持久阻塞等锁）。
 * sheaf_try_flush_main：用于内存压力等不能阻塞的场景（拿不到锁就放弃本轮）。
 */
static void sheaf_flush_main(struct kmem_cache *s)
{
	unsigned int remaining;

	do {
		local_lock(&s->cpu_sheaves->lock);

		remaining = __sheaf_flush_main_batch(s);

	} while (remaining);
}

/*
 * Returns true if the main sheaf was at least partially flushed.
 */
static bool sheaf_try_flush_main(struct kmem_cache *s)
{
	unsigned int remaining;
	bool ret = false;

	do {
		if (!local_trylock(&s->cpu_sheaves->lock))
			return ret;

		ret = true;
		remaining = __sheaf_flush_main_batch(s);

	} while (remaining);

	return ret;
}

/*
 * sheaf_flush_unused() - 直接刷出一个未与任何 CPU 关联的 sheaf
 *
 * 背景：barn 中暂存的 sheaf、CPU 下线后的 sheaf，都不受任何 CPU 的
 * cpu_sheaves->lock 保护，也不会被其他 CPU 并发访问，因此可以直接
 * 无锁批量释放所有对象，无需分批处理。
 * CPU hotremove 时对 spare 和 main 也可以用这种方式（目标 CPU 已停止执行）。
 */
static void sheaf_flush_unused(struct kmem_cache *s, struct slab_sheaf *sheaf)
{
	if (!sheaf->size)
		return;

	stat_add(s, SHEAF_FLUSH, sheaf->size);

	__kmem_cache_free_bulk(s, sheaf->size, &sheaf->objects[0]);

	sheaf->size = 0;
}

static bool __rcu_free_sheaf_prepare(struct kmem_cache *s,
				     struct slab_sheaf *sheaf)
{
	bool init = slab_want_init_on_free(s);
	void **p = &sheaf->objects[0];
	unsigned int i = 0;
	bool pfmemalloc = false;

	while (i < sheaf->size) {
		struct slab *slab = virt_to_slab(p[i]);

		memcg_slab_free_hook(s, slab, p + i, 1);
		alloc_tagging_slab_free_hook(s, slab, p + i, 1);

		if (unlikely(!slab_free_hook(s, p[i], init, true))) {
			p[i] = p[--sheaf->size];
			continue;
		}

		if (slab_test_pfmemalloc(slab))
			pfmemalloc = true;

		i++;
	}

	return pfmemalloc;
}

static void rcu_free_sheaf_nobarn(struct rcu_head *head)
{
	struct slab_sheaf *sheaf;
	struct kmem_cache *s;

	sheaf = container_of(head, struct slab_sheaf, rcu_head);
	s = sheaf->cache;

	__rcu_free_sheaf_prepare(s, sheaf);

	sheaf_flush_unused(s, sheaf);

	free_empty_sheaf(s, sheaf);
}

/*
 * pcs_flush_all() - 刷出当前 CPU 的全部三个 sheaf（main / spare / rcu_free）
 *
 * 调用约束：必须禁用迁移（migration disabled），保证执行期间不换 CPU；
 * 不能在 irq 上下文中调用（sheaf_flush_main 可能休眠等待锁）。
 *
 * 设计选择：刷出直接走 slab freelist，跳过 barn。原因：
 *   flush_all 是低频操作（shrink / hotplug），不值得为了节省几次 spinlock
 *   去过 barn 这条路，直接刷到 slab 更简单，且能立即释放内存。
 *
 * 执行流程：
 *   1. 持锁取出 spare 和 rcu_free 指针，置 NULL（与快速路径解耦）
 *   2. 解锁后安全地释放 spare（sheaf_flush_unused）
 *   3. rcu_free sheaf 通过 call_rcu 延迟释放（保证 RCU 语义）
 *   4. sheaf_flush_main 循环刷出 main sheaf 直到清空
 */
static void pcs_flush_all(struct kmem_cache *s)
{
	struct slub_percpu_sheaves *pcs;
	struct slab_sheaf *spare, *rcu_free;

	local_lock(&s->cpu_sheaves->lock);
	pcs = this_cpu_ptr(s->cpu_sheaves);

	spare = pcs->spare;
	pcs->spare = NULL;

	rcu_free = pcs->rcu_free;
	pcs->rcu_free = NULL;

	local_unlock(&s->cpu_sheaves->lock);

	if (spare) {
		sheaf_flush_unused(s, spare);
		free_empty_sheaf(s, spare);
	}

	if (rcu_free)
		call_rcu(&rcu_free->rcu_head, rcu_free_sheaf_nobarn);

	sheaf_flush_main(s);
}

static void __pcs_flush_all_cpu(struct kmem_cache *s, unsigned int cpu)
{
	struct slub_percpu_sheaves *pcs;

	pcs = per_cpu_ptr(s->cpu_sheaves, cpu);

	/* The cpu is not executing anymore so we don't need pcs->lock */
	sheaf_flush_unused(s, pcs->main);
	if (pcs->spare) {
		sheaf_flush_unused(s, pcs->spare);
		free_empty_sheaf(s, pcs->spare);
		pcs->spare = NULL;
	}

	if (pcs->rcu_free) {
		call_rcu(&pcs->rcu_free->rcu_head, rcu_free_sheaf_nobarn);
		pcs->rcu_free = NULL;
	}
}

static void pcs_destroy(struct kmem_cache *s)
{
	int cpu;

	/*
	 * We may be unwinding cache creation that failed before or during the
	 * allocation of this.
	 */
	if (!s->cpu_sheaves)
		return;

	/* pcs->main can only point to the bootstrap sheaf, nothing to free */
	if (!cache_has_sheaves(s))
		goto free_pcs;

	for_each_possible_cpu(cpu) {
		struct slub_percpu_sheaves *pcs;

		pcs = per_cpu_ptr(s->cpu_sheaves, cpu);

		/* This can happen when unwinding failed cache creation. */
		if (!pcs->main)
			continue;

		/*
		 * We have already passed __kmem_cache_shutdown() so everything
		 * was flushed and there should be no objects allocated from
		 * slabs, otherwise kmem_cache_destroy() would have aborted.
		 * Therefore something would have to be really wrong if the
		 * warnings here trigger, and we should rather leave objects and
		 * sheaves to leak in that case.
		 */

		WARN_ON(pcs->spare);
		WARN_ON(pcs->rcu_free);

		if (!WARN_ON(pcs->main->size)) {
			free_empty_sheaf(s, pcs->main);
			pcs->main = NULL;
		}
	}

free_pcs:
	free_percpu(s->cpu_sheaves);
	s->cpu_sheaves = NULL;
}

/*
 * barn_get_empty_sheaf() - 从 barn 取一个空 sheaf（供释放对象时使用）
 *
 * 背景：per-CPU 要将对象推入 sheaf，需要一个有空间的 sheaf。
 * 若 main sheaf 已满且 spare 也满，可从 barn 取一个空 sheaf，
 * 同时把满的 main 存入 barn（见 barn_replace_full_sheaf）。
 * 这样避免立刻去操作 slab freelist，只做一次 spinlock 换手。
 *
 * data_race() 预检：先不加锁读 barn->nr_empty，为 0 则快速返回 NULL。
 * 这是故意的 racy read（另一个 CPU 可能恰好在加我们之前清空了 barn），
 * 加锁后再次检查（if (likely(barn->nr_empty))）作为真正的保护。
 * 预检避免了大量无意义的 spinlock 尝试，是常见的"乐观读"优化。
 *
 * allow_spin=false 时 trylock，失败立即返回 NULL（NMI/原子上下文安全）。
 */
static struct slab_sheaf *barn_get_empty_sheaf(struct node_barn *barn,
					       bool allow_spin)
{
	struct slab_sheaf *empty = NULL;
	unsigned long flags;

	if (!data_race(barn->nr_empty))
		return NULL;

	if (likely(allow_spin))
		spin_lock_irqsave(&barn->lock, flags);
	else if (!spin_trylock_irqsave(&barn->lock, flags))
		return NULL;

	if (likely(barn->nr_empty)) {
		empty = list_first_entry(&barn->sheaves_empty,
					 struct slab_sheaf, barn_list);
		list_del(&empty->barn_list);
		barn->nr_empty--;
	}

	spin_unlock_irqrestore(&barn->lock, flags);

	return empty;
}

/*
 * barn_put_empty_sheaf() / barn_put_full_sheaf() - 无条件将 sheaf 还给 barn
 *
 * 背景：这两个函数用于"撤销"场景——CPU 迁移或竞争导致之前的操作需要回退，
 * 比如：从 barn 取了一个 sheaf 准备用，但 CPU 被迁移到了另一个 NUMA 节点，
 * 此时无法使用这个来自原节点的 sheaf，需要还给 barn。
 *
 * 不检查 nr_empty/nr_full 上限（注释说明了原因：这是撤销操作，sheaf 总数
 * 不变，只是归还而已，无需担心 barn 溢出）。
 */
static void barn_put_empty_sheaf(struct node_barn *barn, struct slab_sheaf *sheaf)
{
	unsigned long flags;

	spin_lock_irqsave(&barn->lock, flags);

	list_add(&sheaf->barn_list, &barn->sheaves_empty);
	barn->nr_empty++;

	spin_unlock_irqrestore(&barn->lock, flags);
}

static void barn_put_full_sheaf(struct node_barn *barn, struct slab_sheaf *sheaf)
{
	unsigned long flags;

	spin_lock_irqsave(&barn->lock, flags);

	list_add(&sheaf->barn_list, &barn->sheaves_full);
	barn->nr_full++;

	spin_unlock_irqrestore(&barn->lock, flags);
}

/*
 * barn_get_full_or_empty_sheaf() - 从 barn 取任意 sheaf（shrink 场景用）
 *
 * 优先取满 sheaf（更有价值），其次取空 sheaf。
 * 用于 barn_shrink()：把 barn 里所有 sheaf 都取出来刷干净，
 * 最终让 slab 页有机会全空并归还给伙伴系统。
 */
static struct slab_sheaf *barn_get_full_or_empty_sheaf(struct node_barn *barn)
{
	struct slab_sheaf *sheaf = NULL;
	unsigned long flags;

	if (!data_race(barn->nr_full) && !data_race(barn->nr_empty))
		return NULL;

	spin_lock_irqsave(&barn->lock, flags);

	if (barn->nr_full) {
		sheaf = list_first_entry(&barn->sheaves_full, struct slab_sheaf,
					barn_list);
		list_del(&sheaf->barn_list);
		barn->nr_full--;
	} else if (barn->nr_empty) {
		sheaf = list_first_entry(&barn->sheaves_empty,
					 struct slab_sheaf, barn_list);
		list_del(&sheaf->barn_list);
		barn->nr_empty--;
	}

	spin_unlock_irqrestore(&barn->lock, flags);

	return sheaf;
}

/*
 * barn_replace_empty_sheaf() - 用空 sheaf 换一个满 sheaf（分配路径用）
 *
 * 背景：per-CPU main sheaf 耗尽需要补货时，如果 barn 有满 sheaf，
 * 最高效的做法是：把手里的空 sheaf 存进 barn，换出一个满 sheaf，
 * 整个过程只需一次 spinlock，不涉及 slab freelist 操作。
 *
 * 原子交换（在 barn->lock 内完成）：
 *   - 从 sheaves_full 取出一个满 sheaf
 *   - 把传入的 empty 加入 sheaves_empty
 *   - nr_full--，nr_empty++（总数不变）
 *
 * 若 barn 没有满 sheaf（nr_full == 0）则直接返回 NULL，调用方降级到
 * refill_sheaf()（从 partial list / 伙伴系统填充）。
 *
 * 注意：不检查 nr_empty 上限，因为总 sheaf 数不变，只是从 full 换到 empty。
 */
static struct slab_sheaf *
barn_replace_empty_sheaf(struct node_barn *barn, struct slab_sheaf *empty,
			 bool allow_spin)
{
	struct slab_sheaf *full = NULL;
	unsigned long flags;

	if (!data_race(barn->nr_full))
		return NULL;

	if (likely(allow_spin))
		spin_lock_irqsave(&barn->lock, flags);
	else if (!spin_trylock_irqsave(&barn->lock, flags))
		return NULL;

	if (likely(barn->nr_full)) {
		full = list_first_entry(&barn->sheaves_full, struct slab_sheaf,
					barn_list);
		list_del(&full->barn_list);
		list_add(&empty->barn_list, &barn->sheaves_empty);
		barn->nr_full--;
		barn->nr_empty++;
	}

	spin_unlock_irqrestore(&barn->lock, flags);

	return full;
}

/*
 * barn_replace_full_sheaf() - 用满 sheaf 换一个空 sheaf（释放路径用）
 *
 * 背景：per-CPU main sheaf 填满需要腾空时，如果 barn 有空 sheaf，
 * 最高效的做法是：把手里的满 sheaf 存进 barn，换出一个空 sheaf，
 * 同样只需一次 spinlock，不涉及 slab freelist 操作。
 *
 * 失败条件（锁外预检，不保证精确但能快速拒绝）：
 *   - nr_full >= MAX_FULL_SHEAVES：barn 满了，不接受更多满 sheaf，
 *     返回 ERR_PTR(-E2BIG)，调用方需要直接刷到 slab freelist
 *   - nr_empty == 0：barn 没有空 sheaf 可换，返回 ERR_PTR(-ENOMEM)
 *   - trylock 失败：返回 ERR_PTR(-EBUSY)
 *
 * 为什么限制 MAX_FULL_SHEAVES？barn 里的满 sheaf 持有已释放的对象，
 * 这些对象"悬浮"在内存中，对应的 slab 页无法变全空（无法归还给伙伴系统）。
 * 限制上限确保不会有太多内存被"锁在" barn 里无法回收。
 */
static struct slab_sheaf *
barn_replace_full_sheaf(struct node_barn *barn, struct slab_sheaf *full,
			bool allow_spin)
{
	struct slab_sheaf *empty;
	unsigned long flags;

	/* we don't repeat this check under barn->lock as it's not critical */
	if (data_race(barn->nr_full) >= MAX_FULL_SHEAVES)
		return ERR_PTR(-E2BIG);
	if (!data_race(barn->nr_empty))
		return ERR_PTR(-ENOMEM);

	if (likely(allow_spin))
		spin_lock_irqsave(&barn->lock, flags);
	else if (!spin_trylock_irqsave(&barn->lock, flags))
		return ERR_PTR(-EBUSY);

	if (likely(barn->nr_empty)) {
		empty = list_first_entry(&barn->sheaves_empty, struct slab_sheaf,
					 barn_list);
		list_del(&empty->barn_list);
		list_add(&full->barn_list, &barn->sheaves_full);
		barn->nr_empty--;
		barn->nr_full++;
	} else {
		empty = ERR_PTR(-ENOMEM);
	}

	spin_unlock_irqrestore(&barn->lock, flags);

	return empty;
}

static void barn_init(struct node_barn *barn)
{
	spin_lock_init(&barn->lock);
	INIT_LIST_HEAD(&barn->sheaves_full);
	INIT_LIST_HEAD(&barn->sheaves_empty);
	barn->nr_full = 0;
	barn->nr_empty = 0;
}

/*
 * barn_shrink() - 清空 barn 中所有 sheaf，让持有的对象流回 slab freelist
 *
 * 背景：barn 中缓存的满 sheaf 持有已释放但尚未回到 slab freelist 的对象，
 * 导致对应 slab 页的 inuse 计数偏高，无法变全空，无法被归还给伙伴系统。
 * 在 shrink 场景（kmem_cache_shrink / 内存压力）需要先清空 barn，
 * 让这些"悬浮"对象归位，再通过 partial list 的 shrink 回收空 slab 页。
 *
 * 两步操作：
 *   1. 持 barn->lock 一次性摘出所有满/空 sheaf 到本地链表（快速减少锁持有时间）
 *   2. 解锁后逐个刷出满 sheaf（sheaf_flush_unused → __slab_free），释放空 sheaf
 *      全程无 barn->lock，允许其他 CPU 继续使用 barn
 */
static void barn_shrink(struct kmem_cache *s, struct node_barn *barn)
{
	LIST_HEAD(empty_list);
	LIST_HEAD(full_list);
	struct slab_sheaf *sheaf, *sheaf2;
	unsigned long flags;

	spin_lock_irqsave(&barn->lock, flags);

	list_splice_init(&barn->sheaves_full, &full_list);
	barn->nr_full = 0;
	list_splice_init(&barn->sheaves_empty, &empty_list);
	barn->nr_empty = 0;

	spin_unlock_irqrestore(&barn->lock, flags);

	list_for_each_entry_safe(sheaf, sheaf2, &full_list, barn_list) {
		sheaf_flush_unused(s, sheaf);
		free_empty_sheaf(s, sheaf);
	}

	list_for_each_entry_safe(sheaf, sheaf2, &empty_list, barn_list)
		free_empty_sheaf(s, sheaf);
}

/*
 * alloc_slab_page() - 从伙伴系统申请 slab 用的物理页
 *
 * 三条路径对应三种上下文：
 *   !allow_spin（NMI/原子）：alloc_frozen_pages_nolock，不自旋，快速失败
 *   NUMA_NO_NODE：alloc_frozen_pages，普通路径（最近节点优先）
 *   指定 node：__alloc_frozen_pages，NUMA 感知路径，尽量从目标节点分配
 *
 * __SetPageSlab()：标记这是一个 slab 页，将其从伙伴系统的"可用页"集合中
 * 逻辑排除，防止 /proc/meminfo 等工具将其计入普通可用内存。
 *
 * pfmemalloc 标记继承：若伙伴系统返回的页本身来自紧急储备（pfmemalloc），
 * slab 也继承该标记，限制此 slab 只能给 __GFP_MEMALLOC 请求使用。
 */
static inline struct slab *alloc_slab_page(gfp_t flags, int node,
					   struct kmem_cache_order_objects oo,
					   bool allow_spin)
{
	struct page *page;
	struct slab *slab;
	unsigned int order = oo_order(oo);

	if (unlikely(!allow_spin))
		page = alloc_frozen_pages_nolock(0/* __GFP_COMP is implied */,
								  node, order);
	else if (node == NUMA_NO_NODE)
		page = alloc_frozen_pages(flags, order);
	else
		page = __alloc_frozen_pages(flags, order, node, NULL);

	if (!page)
		return NULL;

	__SetPageSlab(page);
	slab = page_slab(page);
	if (page_is_pfmemalloc(page))
		slab_set_pfmemalloc(slab);

	return slab;
}

#ifdef CONFIG_SLAB_FREELIST_RANDOM
/* Pre-initialize the random sequence cache */
static int init_cache_random_seq(struct kmem_cache *s)
{
	unsigned int count = oo_objects(s->oo);
	int err;

	/* Bailout if already initialised */
	if (s->random_seq)
		return 0;

	err = cache_random_seq_create(s, count, GFP_KERNEL);
	if (err) {
		pr_err("SLUB: Unable to initialize free list for %s\n",
			s->name);
		return err;
	}

	/* Transform to an offset on the set of pages */
	if (s->random_seq) {
		unsigned int i;

		for (i = 0; i < count; i++)
			s->random_seq[i] *= s->size;
	}
	return 0;
}

/* Initialize each random sequence freelist per cache */
static void __init init_freelist_randomization(void)
{
	struct kmem_cache *s;

	mutex_lock(&slab_mutex);

	list_for_each_entry(s, &slab_caches, list)
		init_cache_random_seq(s);

	mutex_unlock(&slab_mutex);
}

static DEFINE_PER_CPU(struct rnd_state, slab_rnd_state);

#else
static inline int init_cache_random_seq(struct kmem_cache *s)
{
	return 0;
}
static inline void init_freelist_randomization(void) { }
#endif /* CONFIG_SLAB_FREELIST_RANDOM */

/*
 * account_slab() / unaccount_slab() - slab 页的统计记账
 *
 * 背景：内核有两套统计需要维护：
 *   1. vmstat（/proc/vmstat）：按 cache 类型（可回收/不可回收/DMA 等）
 *      统计已分配给 slab 的页数，供内存压力感知和 OOM 决策使用。
 *   2. memcg（内存 cgroup）：对标记了 SLAB_ACCOUNT 的 cache，需要把
 *      slab 页占用的内存计入对应 cgroup 的配额。
 *
 * account_slab()：slab 页创建时调用，累加统计值，并为 memcg 分配 obj_exts。
 *   注意：alloc_slab_obj_exts_early() 在 account_slab 之前已优先使用
 *   leftover/in-object 方式初始化 obj_exts；此处仅在还没有 obj_exts 时
 *   才通过 alloc_slab_obj_exts（kmalloc）补充分配。
 *
 * unaccount_slab()：slab 页释放时调用，递减统计值，并释放 obj_exts。
 *   即使 profiling 在 slab 生命周期内被禁用，obj_exts 也必须释放
 *   （因为分配时可能已经建立了 obj_exts，无论当前开关状态如何都要清理）。
 */
static __always_inline void account_slab(struct slab *slab, int order,
					 struct kmem_cache *s, gfp_t gfp,
					 unsigned int alloc_flags)
{
	if (memcg_kmem_online() &&
			(s->flags & SLAB_ACCOUNT) &&
			!slab_obj_exts(slab))
		alloc_slab_obj_exts(slab, s, gfp,
				    alloc_flags | SLAB_ALLOC_NEW_SLAB);

	mod_node_page_state(slab_pgdat(slab), cache_vmstat_idx(s),
			    PAGE_SIZE << order);
}

static __always_inline void unaccount_slab(struct slab *slab, int order,
					   struct kmem_cache *s, bool allow_spin)
{
	/*
	 * The slab object extensions should now be freed regardless of
	 * whether mem_alloc_profiling_enabled() or not because profiling
	 * might have been disabled after slab->obj_exts got allocated.
	 */
	free_slab_obj_exts(slab, allow_spin);

	mod_node_page_state(slab_pgdat(slab), cache_vmstat_idx(s),
			    -(PAGE_SIZE << order));
}

/*
 * allocate_slab() - 向伙伴系统申请新 slab 页并完成基础初始化
 *
 * 背景：这是 SLUB 三层内存体系最底层的申请动作，只在 per-CPU sheaf 和
 * per-node partial list 都耗尽时才会到达这里。
 *
 * 两阶段尝试策略：
 *   第一阶段：用理想 order（s->oo，每 slab 对象数最多）尝试申请。
 *     附加 __GFP_NOWARN|__GFP_NORETRY，让失败立即返回而不触发 OOM killer，
 *     并关闭 RECLAIM（如果支持直接回收）避免在高 order 上触发同步回收
 *     造成长时延。高 order 连续页在碎片化严重时很可能失败。
 *   第二阶段（第一阶段失败时）：降级到最小 order（s->min），
 *     恢复完整 flags（允许回收），以更高代价确保成功。
 *     同时 stat(ORDER_FALLBACK) 记录降级次数，供性能调优观察。
 *
 * 初始化顺序（顺序很重要，错误会导致内存损坏）：
 *   1. 设置 slab 元数据（objects/inuse/frozen/slab_cache）
 *   2. kasan_poison_slab()：将整页标记为不可访问
 *   3. setup_slab_debug()：填 POISON_INUSE（需在 obj_exts 初始化前）
 *   4. init_slab_obj_exts()：清零 obj_exts 指针
 *   5. alloc_slab_obj_exts_early()：在 slab 进入链表前初始化 obj_exts
 *      （必须先于 account_slab，避免并发）
 *   6. account_slab()：vmstat 计账（可能惰性补分配 obj_exts）
 *
 * 注意：本函数不建立 freelist，调用方（alloc_from_new_slab/refill_objects）
 * 负责从 slab 内存中批量提取对象并组织 freelist。
 */
static struct slab *allocate_slab(struct kmem_cache *s, gfp_t flags,
				  unsigned int alloc_flags, int node)
{
	bool allow_spin = alloc_flags_allow_spinning(alloc_flags);
	struct slab *slab;
	struct kmem_cache_order_objects oo = s->oo;
	gfp_t alloc_gfp;
	void *start;

	flags &= gfp_allowed_mask;

	flags |= s->allocflags;

	/*
	 * Let the initial higher-order allocation fail under memory pressure
	 * so we fall-back to the minimum order allocation.
	 */
	alloc_gfp = (flags | __GFP_NOWARN | __GFP_NORETRY) & ~__GFP_NOFAIL;
	if ((alloc_gfp & __GFP_DIRECT_RECLAIM) && oo_order(oo) > oo_order(s->min))
		alloc_gfp = (alloc_gfp | __GFP_NOMEMALLOC) & ~__GFP_RECLAIM;

	slab = alloc_slab_page(alloc_gfp, node, oo, allow_spin);
	if (unlikely(!slab)) {
		oo = s->min;
		alloc_gfp = flags;
		/*
		 * Allocation may have failed due to fragmentation.
		 * Try a lower order alloc if possible
		 */
		slab = alloc_slab_page(alloc_gfp, node, oo, allow_spin);
		if (unlikely(!slab))
			return NULL;
		stat(s, ORDER_FALLBACK);
	}

	slab->objects = oo_objects(oo);
	slab->inuse = 0;
	slab->frozen = 0;

	slab->slab_cache = s;

	kasan_poison_slab(slab);

	start = slab_address(slab);

	setup_slab_debug(s, slab, start);
	init_slab_obj_exts(slab);
	/*
	 * Poison the slab before initializing the slabobj_ext array
	 * to prevent the array from being overwritten.
	 */
	alloc_slab_obj_exts_early(s, slab);
	account_slab(slab, oo_order(oo), s, flags, alloc_flags);

	return slab;
}

/*
 * new_slab() - allocate_slab() 的入口封装，负责 GFP 标志清理
 *
 * 问题背景：调用方传入的 flags 可能包含 SLAB 分配不应该使用的标志，
 * 或者存在 debug 场景下的异常标志组合（GFP_SLAB_BUG_MASK），
 * 直接透传给伙伴系统会导致行为异常甚至 BUG。
 *
 * GFP_SLAB_BUG_MASK 检测：某些标志组合对 slab 无意义（如 GFP_USER、
 * __GFP_HIGHMEM），出现时通过 kmalloc_fix_flags() 自动修正并打印 WARN，
 * 帮助定位调用方的错误。
 *
 * ctor + __GFP_ZERO 冲突：有构造函数的 cache 不应该用 __GFP_ZERO，
 * 因为构造函数负责初始化（可能初始化的内容比 zero-fill 更多），
 * 两者同时使用是调用方的 bug，WARN_ON_ONCE 提醒。
 *
 * GFP_RECLAIM_MASK | GFP_CONSTRAINT_MASK：只保留与"如何分配"相关的标志
 * （回收策略、NUMA 约束），剔除对 slab 内部分配无意义的标志。
 */
static struct slab *new_slab(struct kmem_cache *s, gfp_t flags,
			     unsigned int alloc_flags, int node)
{
	if (unlikely(flags & GFP_SLAB_BUG_MASK))
		flags = kmalloc_fix_flags(flags);

	WARN_ON_ONCE(s->ctor && (flags & __GFP_ZERO));

	flags &= GFP_RECLAIM_MASK | GFP_CONSTRAINT_MASK;

	return allocate_slab(s, flags, alloc_flags, node);
}

/*
 * __free_slab() - slab 页的底层释放：清理元数据并归还给伙伴系统
 *
 * 释放流程（顺序不能颠倒，各步骤依赖前一步的状态）：
 *   1. __slab_clear_pfmemalloc()：清除 SL_pfmemalloc 标志，
 *      确保归还给伙伴系统的页不带任何 slab 专有标记
 *   2. page->mapping = NULL：清除 mapping 字段（slab 复用 page->lru
 *      做链表节点时 mapping 字段可能被借用）
 *   3. __ClearPageSlab()：清除 PG_slab 标志，让伙伴系统正确识别该页已空闲
 *   4. mm_account_reclaimed_pages()：向 vmstat 记录归还的页数（用于 OOM 评分）
 *   5. unaccount_slab()：释放 obj_exts、更新 vmstat slab 计数
 *   6. free_frozen_pages()：实际归还给伙伴系统
 *
 * allow_spin=false：NMI/原子上下文中不能自旋等待，使用 nolock 版本。
 */
static void __free_slab(struct kmem_cache *s, struct slab *slab, bool allow_spin)
{
	struct page *page = slab_page(slab);
	int order = compound_order(page);
	int pages = 1 << order;

	__slab_clear_pfmemalloc(slab);
	page->mapping = NULL;
	__ClearPageSlab(page);
	mm_account_reclaimed_pages(pages);
	unaccount_slab(slab, order, s, allow_spin);
	if (allow_spin)
		free_frozen_pages(page, order);
	else
		free_frozen_pages_nolock(page, order);
}

/*
 * free_new_slab_nolock() - 释放刚分配失败的全新 slab（无锁上下文用）
 *
 * 全新 slab 尚未进入任何链表，所以可以跳过 discard_slab() 的 dec_slabs_node
 * 和 free_slab() 的调试检查，直接调用 __free_slab 清理资源。
 * 在 allow_spin=false 的 NMI/原子上下文中使用。
 */
static void free_new_slab_nolock(struct kmem_cache *s, struct slab *slab)
{
	/*
	 * Since it was just allocated, we can skip the actions in
	 * discard_slab() and free_slab().
	 */
	__free_slab(s, slab, false);
}

/*
 * rcu_free_slab() - RCU 宽限期结束后的延迟 slab 释放回调
 *
 * 背景：SLAB_TYPESAFE_BY_RCU 的 cache 允许在对象释放后的 RCU 宽限期内，
 * 持有 RCU 读锁的代码仍然安全地读取对象内存（但需验证对象仍有效）。
 * 为了保证这一语义，slab 页必须在 RCU 宽限期结束后才真正归还给伙伴系统。
 * call_rcu 注册本函数为回调，宽限期后在 RCU 软中断上下文执行真正的释放。
 */
static void rcu_free_slab(struct rcu_head *h)
{
	struct slab *slab = container_of(h, struct slab, rcu_head);

	__free_slab(slab->slab_cache, slab, true);
}

/*
 * free_slab() - slab 页的公开释放接口（调试检查 + RCU 路由）
 *
 * 在 __free_slab 之前做两件事：
 *   1. 调试检查（SLAB_CONSISTENCY_CHECKS）：对 slab 内所有对象做最终检查，
 *      确认释放时对象处于 SLUB_RED_INACTIVE 状态（poison 完整、无越界写），
 *      有助于在归还给伙伴系统前最后一次捕获 use-after-free / UAF 问题。
 *   2. SLAB_TYPESAFE_BY_RCU 路由：延迟释放，保证 RCU 读者的内存安全。
 */
static void free_slab(struct kmem_cache *s, struct slab *slab)
{
	if (kmem_cache_debug_flags(s, SLAB_CONSISTENCY_CHECKS)) {
		void *p;

		slab_pad_check(s, slab);
		for_each_object(p, s, slab_address(slab), slab->objects)
			check_object(s, slab, p, SLUB_RED_INACTIVE);
	}

	if (unlikely(s->flags & SLAB_TYPESAFE_BY_RCU))
		call_rcu(&slab->rcu_head, rcu_free_slab);
	else
		__free_slab(s, slab, true);
}

/*
 * discard_slab() - slab 页进入最终释放流程的入口
 *
 * 在 free_slab 之前递减节点的 slab 计数（nr_slabs / total_objects），
 * 确保统计数据与实际 slab 数量同步。
 * 调用方（__slab_free 的"slab 全空"路径）在此之前已持 n->list_lock，
 * dec_slabs_node 用原子操作，无需再加锁。
 */
static void discard_slab(struct kmem_cache *s, struct slab *slab)
{
	dec_slabs_node(s, slab_nid(slab), slab->objects);
	free_slab(s, slab);
}

/* SL_partial 位操作：标记/清除/检测 slab 是否挂在 partial list 上 */
static inline bool slab_test_node_partial(const struct slab *slab)
{
	return test_bit(SL_partial, &slab->flags.f);
}

static inline void slab_set_node_partial(struct slab *slab)
{
	set_bit(SL_partial, &slab->flags.f);
}

static inline void slab_clear_node_partial(struct slab *slab)
{
	clear_bit(SL_partial, &slab->flags.f);
}

/*
 * add_partial() / __add_partial() - 将 slab 加入 per-node partial list
 *
 * 背景：partial list 上的 slab 处于"部分空闲"状态（有空闲对象可分配，
 * 但还有对象被使用）。add_partial 在两种情况下调用：
 *   1. 全新 slab 页第一次被切割后（alloc_from_new_slab），剩余的
 *      空闲对象留在 slab 内，slab 挂入 partial list 备用
 *   2. 全满 slab（was_full）释放第一个对象后（__slab_free），
 *      slab 从"全满不在链表"变为"有空闲对象"，需要加入 partial list
 *
 * ADD_TO_HEAD vs ADD_TO_TAIL：
 *   HEAD：新鲜的 slab 优先被使用（内存最近被访问，缓存热）
 *   TAIL：空 slab 放到末尾，优先消耗 partial list 头部的活跃 slab，
 *         让空 slab 有机会在后续内存压力下被 shrink 回收
 *
 * __add_partial 是不持锁的内部版本（由 alloc_from_new_slab 等已持锁的
 * 调用方使用）；add_partial 断言 list_lock 已持有。
 */
static inline void set_node_partial_state(struct kmem_cache_node *n,
					struct slab *slab)
{
	slab_set_node_partial(slab);
	n->nr_partial++;
}

static inline void
__add_partial(struct kmem_cache_node *n, struct slab *slab, enum add_mode mode)
{
	if (mode == ADD_TO_TAIL)
		list_add_tail(&slab->slab_list, &n->partial);
	else
		list_add(&slab->slab_list, &n->partial);
	set_node_partial_state(n, slab);
}

static inline void add_partial(struct kmem_cache_node *n,
				struct slab *slab, enum add_mode mode)
{
	lockdep_assert_held(&n->list_lock);
	__add_partial(n, slab, mode);
}

/* clear_node_partial_state() / remove_partial() - 从 partial list 摘除 slab */
static inline void clear_node_partial_state(struct kmem_cache_node *n,
					struct slab *slab)
{
	slab_clear_node_partial(slab);
	n->nr_partial--;
}

/*
 * remove_partial() - 将 slab 从 partial list 摘除
 *
 * 调用时机：
 *   1. slab 的最后一个空闲对象被取走（slab 变为满载），不再需要挂在 partial list
 *   2. slab 全空且 partial list 已够充裕，即将被 discard_slab() 归还给伙伴系统
 *   3. 一致性检查失败，slab 被 freeze（冻结），必须从链表移除
 *
 * 摘除后 slab 的 SL_partial bit 清零，__slab_free() 通过检测这个 bit
 * 来判断是否需要操作 partial list，避免重复 list_del。
 */
static inline void remove_partial(struct kmem_cache_node *n,
					struct slab *slab)
{
	lockdep_assert_held(&n->list_lock);
	list_del(&slab->slab_list);
	clear_node_partial_state(n, slab);
}

/*
 * alloc_single_from_partial() - 调试 cache 专用的 partial list 单对象分配
 *
 * 背景：调试 cache 不允许批量操作（无法在不持锁情况下安全地批量
 * CAS freelist），所以走单对象路径：持 list_lock，每次只取一个对象，
 * 执行完整的 alloc_debug_processing() 检查，再决定 slab 是留在 partial
 * list 还是移入 full list。
 *
 * 与 get_partial_node_bulk() 的区别：后者批量摘 slab，之后调用方再从
 * slab 的 freelist 里取对象，速度更快但不做逐对象调试检查。
 */
static void *alloc_single_from_partial(struct kmem_cache *s,
		struct kmem_cache_node *n, struct slab *slab, int orig_size)
{
	void *object;

	lockdep_assert_held(&n->list_lock);

#ifdef CONFIG_SLUB_DEBUG
	if (s->flags & SLAB_CONSISTENCY_CHECKS) {
		if (!validate_slab_ptr(slab)) {
			slab_err(s, slab, "Not a valid slab page");
			return NULL;
		}
	}
#endif

	object = slab->freelist;
	slab->freelist = get_freepointer(s, object);
	slab->inuse++;

	if (!alloc_debug_processing(s, slab, object, orig_size)) {
		remove_partial(n, slab);
		return NULL;
	}

	if (slab->inuse == slab->objects) {
		remove_partial(n, slab);
		add_full(s, n, slab);
	}

	return object;
}

/*
 * next_slab_obj() - 按分配顺序返回 slab 内下一个对象的地址
 *
 * 背景：SLAB_FREELIST_RANDOM 是一种安全加固手段。问题是：若 freelist
 * 始终按固定顺序（槽位 0→1→2→...）分配，攻击者可以通过观察连续分配的
 * 对象地址差值，推断出 slab 的 size 和布局，进而预测下一个对象的地址，
 * 辅助堆喷射（heap spray）或 use-after-free 利用。
 *
 * 解决方案：在新 slab 初始化 freelist 时，用随机序列 s->random_seq[]
 * 打乱槽位顺序。迭代器通过随机起始位置（get_random_u32_below）和环形
 * 索引（pos % freelist_count）遍历全部槽位，保证：
 *   - 每个槽位都会被访问且恰好一次（不遗漏、不重复）
 *   - 访问顺序对攻击者不可预测
 *
 * 不支持 allow_spin 时（原子/NMI 上下文）退回到 per-CPU 的 prandom_state，
 * 性能略差但不阻塞。
 *
 * 无 FREELIST_RANDOM 时：顺序遍历（pos++ * size），最简单高效。
 */
static inline void *next_slab_obj(struct kmem_cache *s,
				  struct slab_obj_iter *iter)
{
#ifdef CONFIG_SLAB_FREELIST_RANDOM
	if (iter->random) {
		unsigned long idx;

		/*
		 * If the target page allocation failed, the number of objects on the
		 * page might be smaller than the usual size defined by the cache.
		 */
		do {
			idx = s->random_seq[iter->pos];
			iter->pos++;
			if (iter->pos >= iter->freelist_count)
				iter->pos = 0;
		} while (unlikely(idx >= iter->page_limit));

		return setup_object(s, (char *)iter->start + idx);
	}
#endif
	return setup_object(s, (char *)iter->start + iter->pos++ * s->size);
}

/*
 * build_slab_freelist() - 从新 slab 页中构建 freelist 链表
 *
 * 背景：allocate_slab() 只从伙伴系统拿到页，不建立 freelist；
 * 实际把 slab 内的空槽位串成单链表的工作在这里完成。
 *
 * 遍历 iter 按（可能随机化的）顺序返回 slab 内的对象地址，
 * 用 set_freepointer 将它们串成链表（头→尾→NULL），
 * slab->freelist 指向链表头。
 *
 * slab->inuse 已由调用方提前设置（表示有多少对象已被取走），
 * nr = objects - inuse 即为需要串进 freelist 的空槽位数。
 */
static inline void build_slab_freelist(struct kmem_cache *s, struct slab *slab,
				       struct slab_obj_iter *iter)
{
	unsigned int nr = slab->objects - slab->inuse;
	unsigned int i;
	void *cur, *next;

	if (!nr) {
		slab->freelist = NULL;
		return;
	}

	cur = next_slab_obj(s, iter);
	slab->freelist = cur;

	for (i = 1; i < nr; i++) {
		next = next_slab_obj(s, iter);
		set_freepointer(s, cur, next);
		cur = next;
	}

	set_freepointer(s, cur, NULL);
}

/* Initialize an iterator over free objects in allocation order. */
static inline void init_slab_obj_iter(struct kmem_cache *s, struct slab *slab,
				      struct slab_obj_iter *iter,
				      bool allow_spin)
{
	iter->pos = 0;
	iter->start = fixup_red_left(s, slab_address(slab));

#ifdef CONFIG_SLAB_FREELIST_RANDOM
	iter->random = (slab->objects >= 2 && s->random_seq);
	if (!iter->random)
		return;

	iter->freelist_count = oo_objects(s->oo);
	iter->page_limit = slab->objects * s->size;

	if (allow_spin) {
		iter->pos = get_random_u32_below(iter->freelist_count);
	} else {
		struct rnd_state *state;

		/*
		 * An interrupt or NMI handler might interrupt and change
		 * the state in the middle, but that's safe.
		 */
		state = &get_cpu_var(slab_rnd_state);
		iter->pos = prandom_u32_state(state) % iter->freelist_count;
		put_cpu_var(slab_rnd_state);
	}
#endif
}

/*
 * Called only for kmem_cache_debug() caches to allocate from a freshly
 * allocated slab. Allocate a single object instead of whole freelist
 * and put the slab to the partial (or full) list.
 */
static void *alloc_single_from_new_slab(struct kmem_cache *s, struct slab *slab,
					const struct slab_alloc_context *ac)
{
	bool allow_spin = alloc_flags_allow_spinning(ac->alloc_flags);
	struct kmem_cache_node *n;
	struct slab_obj_iter iter;
	bool needs_add_partial;
	unsigned long flags;
	void *object;

	init_slab_obj_iter(s, slab, &iter, allow_spin);
	object = next_slab_obj(s, &iter);
	slab->inuse = 1;

	needs_add_partial = (slab->objects > 1);
	build_slab_freelist(s, slab, &iter);

	/* alloc_debug_processing() always expects a valid freepointer */
	set_freepointer(s, object, slab->freelist);

	if (!alloc_debug_processing(s, slab, object, ac->orig_size)) {
		/*
		 * It's not really expected that this would fail on a
		 * freshly allocated slab, but a concurrent memory
		 * corruption in theory could cause that.
		 * Leak memory of allocated slab.
		 */
		return NULL;
	}

	n = get_node(s, slab_nid(slab));
	if (allow_spin) {
		spin_lock_irqsave(&n->list_lock, flags);
	} else if (!spin_trylock_irqsave(&n->list_lock, flags)) {
		/*
		 * Unlucky, discard newly allocated slab.
		 * The slab is not fully free, but it's fine as
		 * objects are not allocated to users.
		 */
		free_new_slab_nolock(s, slab);
		return NULL;
	}

	if (needs_add_partial)
		add_partial(n, slab, ADD_TO_HEAD);
	else
		add_full(s, n, slab);

	/*
	 * Debug caches require nr_slabs updates under n->list_lock so validation
	 * cannot race with slab (de)allocations and observe inconsistent state.
	 */
	inc_slabs_node(s, slab_nid(slab), slab->objects);
	spin_unlock_irqrestore(&n->list_lock, flags);

	return object;
}

static inline bool pfmemalloc_match(struct slab *slab, gfp_t gfpflags);

static bool get_partial_node_bulk(struct kmem_cache *s,
				  struct kmem_cache_node *n,
				  struct partial_bulk_context *pc,
				  bool allow_spin)
{
	struct slab *slab, *slab2;
	struct slab *first = NULL, *last = NULL;
	unsigned int total_free = 0;
	unsigned long flags;

	/* Racy check to avoid taking the lock unnecessarily. */
	if (!n || data_race(!n->nr_partial))
		return false;

	INIT_LIST_HEAD(&pc->slabs);

	if (allow_spin)
		spin_lock_irqsave(&n->list_lock, flags);
	else if (!spin_trylock_irqsave(&n->list_lock, flags))
		return false;

	list_for_each_entry_safe(slab, slab2, &n->partial, slab_list) {
		struct freelist_counters flc;
		unsigned int slab_free;

		if (!pfmemalloc_match(slab, pc->flags)) {
			if (first) {
				list_bulk_move_tail(&pc->slabs,
						    &first->slab_list,
						    &last->slab_list);
				first = NULL;
			}
			continue;
		}

		/*
		 * determine the number of free objects in the slab racily
		 *
		 * slab_free is a lower bound due to possible subsequent
		 * concurrent freeing, so the caller may get more objects than
		 * requested and must handle that
		 */
		flc.counters = data_race(READ_ONCE(slab->counters));
		slab_free = flc.objects - flc.inuse;

		/* we have already min and this would get us over the max */
		if (total_free >= pc->min_objects
		    && total_free + slab_free > pc->max_objects)
			break;

		if (!first)
			first = slab;
		last = slab;
		clear_node_partial_state(n, slab);

		total_free += slab_free;
		if (total_free >= pc->max_objects)
			break;
	}

	if (first)
		list_bulk_move_tail(&pc->slabs, &first->slab_list,
				    &last->slab_list);

	spin_unlock_irqrestore(&n->list_lock, flags);
	return total_free > 0;
}

/*
 * get_from_partial_node() - 从指定 NUMA 节点的 partial list 取一个对象
 *
 * 背景：这是慢速路径（per-CPU sheaf 已空）的第一选择，优先消耗本节点的
 * partial slab，避免跨节点访问带来的 NUMA 延迟。
 *
 * 设计要点：
 *
 * 1. 无锁预检（racy check）：
 *    先不加锁检查 n->nr_partial，如果为 0 直接返回 NULL。
 *    这是有意为之的 data race（标注了注释），原因是：
 *    - 如果误判为空（刚好有另一个 CPU 在加我们之前用完了最后一个 slab），
 *      后续会从新 slab 分配，正确但略微浪费。
 *    - 如果误判为非空（另一个 CPU 在我们读 nr_partial 后立刻清空了 list），
 *      加锁后 list_for_each 找不到合适的 slab，循环退出，同样正确。
 *    避免每次都无条件加锁，减少在 partial list 为空时的锁争用。
 *
 * 2. pfmemalloc_match()：
 *    slab 来自紧急储备（SL_pfmemalloc）时，只能给同样申请了紧急储备的
 *    分配请求使用（__GFP_MEMALLOC）。不匹配则跳过该 slab。
 *
 * 3. 正常路径（非调试 cache）——持锁 + CAS 取单对象：
 *    在 list_lock 保护下，用 __slab_update_freelist（cmpxchg_double）
 *    原子地将 freelist 前移并递增 inuse。
 *    看起来已经持了 list_lock，为什么还要 CAS？
 *    因为 __slab_free() 的快速路径也在无锁地修改同一个 slab 的 freelist
 *    （它用 cmpxchg_double 而不先拿 list_lock），只有当 slab 变全空时
 *    __slab_free 才会去拿 list_lock。所以这里取对象时可能与一个并发的
 *    __slab_free 发生竞争，用 CAS 保证原子性。
 *
 * 4. 取完对象后 freelist 为空则 remove_partial：slab 变满载，从链表摘除。
 */
static void *get_from_partial_node(struct kmem_cache *s,
				   struct kmem_cache_node *n,
				   gfp_t gfp_flags,
				   const struct slab_alloc_context *ac)
{
	struct slab *slab, *slab2;
	unsigned long flags;
	void *object = NULL;

	/*
	 * Racy check. If we mistakenly see no partial slabs then we
	 * just allocate an empty slab. If we mistakenly try to get a
	 * partial slab and there is none available then get_from_partial()
	 * will return NULL.
	 */
	if (!n || !n->nr_partial)
		return NULL;

	if (alloc_flags_allow_spinning(ac->alloc_flags))
		spin_lock_irqsave(&n->list_lock, flags);
	else if (!spin_trylock_irqsave(&n->list_lock, flags))
		return NULL;
	list_for_each_entry_safe(slab, slab2, &n->partial, slab_list) {

		struct freelist_counters old, new;

		if (!pfmemalloc_match(slab, gfp_flags))
			continue;

		if (IS_ENABLED(CONFIG_SLUB_TINY) || kmem_cache_debug(s)) {
			object = alloc_single_from_partial(s, n, slab,
							ac->orig_size);
			if (object)
				break;
			continue;
		}

		/*
		 * get a single object from the slab. This might race against
		 * __slab_free(), which however has to take the list_lock if
		 * it's about to make the slab fully free.
		 */
		do {
			old.freelist = slab->freelist;
			old.counters = slab->counters;

			new.freelist = get_freepointer(s, old.freelist);
			new.counters = old.counters;
			new.inuse++;

		} while (!__slab_update_freelist(s, slab, &old, &new, "get_from_partial_node"));

		object = old.freelist;
		if (!new.freelist)
			remove_partial(n, slab);

		break;
	}
	spin_unlock_irqrestore(&n->list_lock, flags);
	return object;
}

/*
 * get_from_any_partial() - 跨 NUMA 节点搜索 partial slab（碎片整理）
 *
 * 背景：多 NUMA 节点的服务器上，某个节点的 partial list 耗尽后，其他节点
 * 可能还有大量 partial slab。从远端节点取对象虽然有 NUMA 延迟，但能
 * 避免向伙伴系统申请新页（新页的 NUMA 局部性由伙伴系统决定，未必更好），
 * 且能减少全局的碎片（让远端的 partial slab 被消耗掉，而不是越积越多）。
 *
 * remote_node_defrag_ratio（通过 /sys/kernel/slab/xx/remote_node_defrag_ratio
 * 可调）控制跨节点碎片整理的积极程度：
 *   = 0   ：完全不做跨节点搜索（本节点优先，牺牲碎片整理效果）
 *   = 1000：每次都先扫描所有节点（最激进，适合碎片严重的大 NUMA 机器）
 *   概率判断：get_cycles() % 1024 > defrag_ratio 时跳过，
 *   即 defrag_ratio/1024 的概率触发跨节点搜索，自然地在局部性和碎片整理间权衡。
 *
 * 遍历策略：按 zonelist 顺序（从 NUMA 距离近到远）搜索，第一个找到对象的
 * 节点即返回，不继续扫描更远的节点。
 *
 * 仅对 nr_partial > s->min_partial 的节点搜索：保留各节点的最低备用量，
 * 不把别人的 partial list 搜干净。
 */
static void *get_from_any_partial(struct kmem_cache *s, gfp_t gfp_flags,
				  const struct slab_alloc_context *ac)
{
#ifdef CONFIG_NUMA
	struct zonelist *zonelist;
	struct zoneref *z;
	struct zone *zone;
	enum zone_type highest_zoneidx = gfp_zone(gfp_flags);
	unsigned int cpuset_mems_cookie;
	bool allow_spin = alloc_flags_allow_spinning(ac->alloc_flags);

	/*
	 * The defrag ratio allows a configuration of the tradeoffs between
	 * inter node defragmentation and node local allocations. A lower
	 * defrag_ratio increases the tendency to do local allocations
	 * instead of attempting to obtain partial slabs from other nodes.
	 *
	 * If the defrag_ratio is set to 0 then kmalloc() always
	 * returns node local objects. If the ratio is higher then kmalloc()
	 * may return off node objects because partial slabs are obtained
	 * from other nodes and filled up.
	 *
	 * If /sys/kernel/slab/xx/remote_node_defrag_ratio is set to 100
	 * (which makes defrag_ratio = 1000) then every (well almost)
	 * allocation will first attempt to defrag slab caches on other nodes.
	 * This means scanning over all nodes to look for partial slabs which
	 * may be expensive if we do it every time we are trying to find a slab
	 * with available objects.
	 */
	if (!s->remote_node_defrag_ratio ||
			get_cycles() % 1024 > s->remote_node_defrag_ratio)
		return NULL;

	do {
		/*
		 * read_mems_allowed_begin() accesses current->mems_allowed_seq,
		 * a seqcount_spinlock_t that is not NMI-safe. Do not access
		 * current->mems_allowed_seq and avoid retry when GFP flags
		 * indicate spinning is not allowed.
		 */
		if (allow_spin)
			cpuset_mems_cookie = read_mems_allowed_begin();

		zonelist = node_zonelist(mempolicy_slab_node(), gfp_flags);
		for_each_zone_zonelist(zone, z, zonelist, highest_zoneidx) {
			struct kmem_cache_node *n;

			n = get_node(s, zone_to_nid(zone));

			if (n && cpuset_zone_allowed(zone, gfp_flags) &&
					n->nr_partial > s->min_partial) {

				void *object = get_from_partial_node(s, n,
								gfp_flags, ac);

				if (object) {
					/*
					 * Don't check read_mems_allowed_retry()
					 * here - if mems_allowed was updated in
					 * parallel, that was a harmless race
					 * between allocation and the cpuset
					 * update
					 */
					return object;
				}
			}
		}
	} while (allow_spin && read_mems_allowed_retry(cpuset_mems_cookie));
#endif	/* CONFIG_NUMA */
	return NULL;
}

/*
 * get_from_partial() - partial list 分配的统一入口（本节点优先，可跨节点回退）
 *
 * 策略：
 *   1. 先找本节点（NUMA_NO_NODE 时取 numa_mem_id()）的 partial list
 *   2. 本节点无对象 且 调用方允许跨节点（未设 __GFP_THISNODE）时，
 *      调用 get_from_any_partial() 扫描其他节点
 *   3. 设了 __GFP_THISNODE（严格要求本节点）或已找到对象则直接返回
 *
 * 这种两阶段设计保证了 NUMA 局部性的同时，在本节点实在没有可用 slab 时
 * 能优先利用已有内存（已分配但部分空闲的远端 slab），而不是立刻去伙伴系统
 * 申请新页（申请新页会触发更多的内存分配操作）。
 */
static void *get_from_partial(struct kmem_cache *s, int node, gfp_t flags,
			      const struct slab_alloc_context *ac)
{
	int searchnode = node;
	void *object;

	if (node == NUMA_NO_NODE)
		searchnode = numa_mem_id();

	object = get_from_partial_node(s, get_node(s, searchnode), flags, ac);
	if (object || (node != NUMA_NO_NODE && (flags & __GFP_THISNODE)))
		return object;

	return get_from_any_partial(s, flags, ac);
}

/* has_pcs_used() - 检查指定 CPU 是否有任何非空 sheaf，用于 flush 时的快速跳过 */
static bool has_pcs_used(int cpu, struct kmem_cache *s)
{
	struct slub_percpu_sheaves *pcs;

	if (!cache_has_sheaves(s))
		return false;

	pcs = per_cpu_ptr(s->cpu_sheaves, cpu);

	return (pcs->spare || pcs->rcu_free || pcs->main->size);
}

/*
 * flush_cpu_sheaves() / flush_all_cpus_locked() / flush_all() -
 * 强制刷出所有 CPU 的 per-CPU sheaf
 *
 * 背景：per-CPU sheaf 中持有的对象是"暂时扣押"在某个 CPU 上的，
 * 这些对象虽然已被释放（用户调用了 kmem_cache_free），但尚未回到
 * slab 的 freelist 或 partial list。在以下场景需要强制刷出：
 *
 *   1. kmem_cache_shrink / kmem_cache_destroy：需要精确统计空闲对象，
 *      并尽量将 slab 页归还给伙伴系统，sheaf 里的对象必须先归还。
 *   2. CPU 下线（cpu_down）：该 CPU 的 sheaf 不会再被使用，
 *      必须把对象归还，否则内存永远泄漏。
 *   3. NUMA 节点热插拔：类似原因。
 *
 * 实现方式：向每个在线 CPU 投递一个 work（queue_work_on），work 在
 * 目标 CPU 上执行 pcs_flush_all（将 sheaf 里所有对象归还给 slab 的
 * freelist，然后将 slab 加入 partial list）。
 *
 * flush_lock mutex 防止并发的多个 flush_all 调用互相干扰（work 结构体
 * 是 per-CPU 的，并发使用 INIT_WORK 会破坏其状态）。
 *
 * has_pcs_used() 预先检查 sheaf 是否为空，跳过无需刷出的 CPU，
 * 减少不必要的 work 投递开销。
 */
static void flush_cpu_sheaves(struct work_struct *w)
{
	struct kmem_cache *s;
	struct slub_flush_work *sfw;

	sfw = container_of(w, struct slub_flush_work, work);

	s = sfw->s;

	if (cache_has_sheaves(s))
		pcs_flush_all(s);
}

static void flush_all_cpus_locked(struct kmem_cache *s)
{
	struct slub_flush_work *sfw;
	unsigned int cpu;

	lockdep_assert_cpus_held();
	mutex_lock(&flush_lock);

	for_each_online_cpu(cpu) {
		sfw = &per_cpu(slub_flush, cpu);
		if (!has_pcs_used(cpu, s)) {
			sfw->skip = true;
			continue;
		}
		INIT_WORK(&sfw->work, flush_cpu_sheaves);
		sfw->skip = false;
		sfw->s = s;
		queue_work_on(cpu, flushwq, &sfw->work);
	}

	for_each_online_cpu(cpu) {
		sfw = &per_cpu(slub_flush, cpu);
		if (sfw->skip)
			continue;
		flush_work(&sfw->work);
	}

	mutex_unlock(&flush_lock);
}

static void flush_all(struct kmem_cache *s)
{
	cpus_read_lock();
	flush_all_cpus_locked(s);
	cpus_read_unlock();
}

static void flush_rcu_sheaf(struct work_struct *w)
{
	struct slub_percpu_sheaves *pcs;
	struct slab_sheaf *rcu_free;
	struct slub_flush_work *sfw;
	struct kmem_cache *s;

	sfw = container_of(w, struct slub_flush_work, work);
	s = sfw->s;

	local_lock(&s->cpu_sheaves->lock);
	pcs = this_cpu_ptr(s->cpu_sheaves);

	rcu_free = pcs->rcu_free;
	pcs->rcu_free = NULL;

	local_unlock(&s->cpu_sheaves->lock);

	if (rcu_free)
		call_rcu(&rcu_free->rcu_head, rcu_free_sheaf_nobarn);
}


/* needed for kvfree_rcu_barrier() */
void flush_rcu_sheaves_on_cache(struct kmem_cache *s)
{
	struct slub_flush_work *sfw;
	unsigned int cpu;

	lockdep_assert_cpus_held();
	mutex_lock(&flush_lock);

	for_each_online_cpu(cpu) {
		sfw = &per_cpu(slub_flush, cpu);

		/*
		 * we don't check if rcu_free sheaf exists - racing
		 * __kfree_rcu_sheaf() might have just removed it.
		 * by executing flush_rcu_sheaf() on the cpu we make
		 * sure the __kfree_rcu_sheaf() finished its call_rcu()
		 */

		INIT_WORK(&sfw->work, flush_rcu_sheaf);
		sfw->s = s;
		queue_work_on(cpu, flushwq, &sfw->work);
	}

	for_each_online_cpu(cpu) {
		sfw = &per_cpu(slub_flush, cpu);
		flush_work(&sfw->work);
	}

	mutex_unlock(&flush_lock);
}

void flush_all_rcu_sheaves(void)
{
	struct kmem_cache *s;

	cpus_read_lock();
	mutex_lock(&slab_mutex);

	list_for_each_entry(s, &slab_caches, list) {
		if (!cache_has_sheaves(s))
			continue;
		flush_rcu_sheaves_on_cache(s);
	}

	mutex_unlock(&slab_mutex);
	cpus_read_unlock();

	rcu_barrier();
}

static int slub_cpu_setup(unsigned int cpu)
{
	int nid = cpu_to_node(cpu);
	struct kmem_cache *s;
	int ret = 0;

	/*
	 * we never clear a nid so it's safe to do a quick check before taking
	 * the mutex, and then recheck to handle parallel cpu hotplug safely
	 */
	if (node_isset(nid, slab_barn_nodes))
		return 0;

	mutex_lock(&slab_mutex);

	if (node_isset(nid, slab_barn_nodes))
		goto out;

	list_for_each_entry(s, &slab_caches, list) {
		struct node_barn *barn;

		/*
		 * barn might already exist if a previous callback failed midway
		 */
		if (!cache_has_sheaves(s) || get_barn_node(s, nid))
			continue;

		barn = kmalloc_node(sizeof(*barn), GFP_KERNEL, nid);

		if (!barn) {
			ret = -ENOMEM;
			goto out;
		}

		barn_init(barn);
		s->per_node[nid].barn = barn;
	}
	node_set(nid, slab_barn_nodes);

out:
	mutex_unlock(&slab_mutex);

	return ret;
}

/*
 * Use the cpu notifier to insure that the cpu slabs are flushed when
 * necessary.
 */
static int slub_cpu_dead(unsigned int cpu)
{
	struct kmem_cache *s;

	mutex_lock(&slab_mutex);
	list_for_each_entry(s, &slab_caches, list) {
		if (cache_has_sheaves(s))
			__pcs_flush_all_cpu(s, cpu);
	}
	mutex_unlock(&slab_mutex);
	return 0;
}

#ifdef CONFIG_SLUB_DEBUG
static int count_free(struct slab *slab)
{
	return slab->objects - slab->inuse;
}

static inline unsigned long node_nr_objs(struct kmem_cache_node *n)
{
	return atomic_long_read(&n->total_objects);
}

/* Supports checking bulk free of a constructed freelist */
static inline bool free_debug_processing(struct kmem_cache *s,
	struct slab *slab, void *head, void *tail, int *bulk_cnt,
	unsigned long addr, depot_stack_handle_t handle)
{
	bool checks_ok = false;
	void *object = head;
	int cnt = 0;

	if (s->flags & SLAB_CONSISTENCY_CHECKS) {
		if (!check_slab(s, slab))
			goto out;
	}

	if (slab->inuse < *bulk_cnt) {
		slab_err(s, slab, "Slab has %d allocated objects but %d are to be freed\n",
			 slab->inuse, *bulk_cnt);
		goto out;
	}

next_object:

	if (++cnt > *bulk_cnt)
		goto out_cnt;

	if (s->flags & SLAB_CONSISTENCY_CHECKS) {
		if (!free_consistency_checks(s, slab, object, addr))
			goto out;
	}

	if (s->flags & SLAB_STORE_USER)
		set_track_update(s, object, TRACK_FREE, addr, handle);
	trace(s, slab, object, 0);
	/* Freepointer not overwritten by init_object(), SLAB_POISON moved it */
	init_object(s, object, SLUB_RED_INACTIVE);

	/* Reached end of constructed freelist yet? */
	if (object != tail) {
		object = get_freepointer(s, object);
		goto next_object;
	}
	checks_ok = true;

out_cnt:
	if (cnt != *bulk_cnt) {
		slab_err(s, slab, "Bulk free expected %d objects but found %d\n",
			 *bulk_cnt, cnt);
		*bulk_cnt = cnt;
	}

out:

	if (!checks_ok)
		slab_fix(s, "Object at 0x%p not freed", object);

	return checks_ok;
}
#endif /* CONFIG_SLUB_DEBUG */

#if defined(CONFIG_SLUB_DEBUG) || defined(SLAB_SUPPORTS_SYSFS)
static unsigned long count_partial(struct kmem_cache_node *n,
					int (*get_count)(struct slab *))
{
	unsigned long flags;
	unsigned long x = 0;
	struct slab *slab;

	spin_lock_irqsave(&n->list_lock, flags);
	list_for_each_entry(slab, &n->partial, slab_list)
		x += get_count(slab);
	spin_unlock_irqrestore(&n->list_lock, flags);
	return x;
}
#endif /* CONFIG_SLUB_DEBUG || SLAB_SUPPORTS_SYSFS */

#ifdef CONFIG_SLUB_DEBUG
#define MAX_PARTIAL_TO_SCAN 10000

static unsigned long count_partial_free_approx(struct kmem_cache_node *n)
{
	unsigned long flags;
	unsigned long x = 0;
	struct slab *slab;

	spin_lock_irqsave(&n->list_lock, flags);
	if (n->nr_partial <= MAX_PARTIAL_TO_SCAN) {
		list_for_each_entry(slab, &n->partial, slab_list)
			x += slab->objects - slab->inuse;
	} else {
		/*
		 * For a long list, approximate the total count of objects in
		 * it to meet the limit on the number of slabs to scan.
		 * Scan from both the list's head and tail for better accuracy.
		 */
		unsigned long scanned = 0;

		list_for_each_entry(slab, &n->partial, slab_list) {
			x += slab->objects - slab->inuse;
			if (++scanned == MAX_PARTIAL_TO_SCAN / 2)
				break;
		}
		list_for_each_entry_reverse(slab, &n->partial, slab_list) {
			x += slab->objects - slab->inuse;
			if (++scanned == MAX_PARTIAL_TO_SCAN)
				break;
		}
		x = mult_frac(x, n->nr_partial, scanned);
		x = min(x, node_nr_objs(n));
	}
	spin_unlock_irqrestore(&n->list_lock, flags);
	return x;
}

static noinline void
slab_out_of_memory(struct kmem_cache *s, gfp_t gfpflags, int nid)
{
	static DEFINE_RATELIMIT_STATE(slub_oom_rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);
	int cpu = raw_smp_processor_id();
	int node;
	struct kmem_cache_node *n;

	if ((gfpflags & __GFP_NOWARN) || !__ratelimit(&slub_oom_rs))
		return;

	pr_warn("SLUB: Unable to allocate memory on CPU %u (of node %d) on node %d, gfp=%#x(%pGg)\n",
		cpu, cpu_to_node(cpu), nid, gfpflags, &gfpflags);
	pr_warn("  cache: %s, object size: %u, buffer size: %u, default order: %u, min order: %u\n",
		s->name, s->object_size, s->size, oo_order(s->oo),
		oo_order(s->min));

	if (oo_order(s->min) > get_order(s->object_size))
		pr_warn("  %s debugging increased min order, use slab_debug=O to disable.\n",
			s->name);

	for_each_kmem_cache_node(s, node, n) {
		unsigned long nr_slabs;
		unsigned long nr_objs;
		unsigned long nr_free;

		nr_free  = count_partial_free_approx(n);
		nr_slabs = node_nr_slabs(n);
		nr_objs  = node_nr_objs(n);

		pr_warn("  node %d: slabs: %ld, objs: %ld, free: %ld\n",
			node, nr_slabs, nr_objs, nr_free);
	}
}
#else /* CONFIG_SLUB_DEBUG */
static inline void
slab_out_of_memory(struct kmem_cache *s, gfp_t gfpflags, int nid) { }
#endif

/*
 * pfmemalloc_match() - 检查分配请求是否可以使用来自紧急储备的 slab
 *
 * 背景：见 slab_set_pfmemalloc() 的注释。
 * 普通 slab（无 SL_pfmemalloc）任何请求都可用；
 * pfmemalloc slab 只能给有 __GFP_MEMALLOC（即 PF_MEMALLOC 上下文）的请求用。
 */
static inline bool pfmemalloc_match(struct slab *slab, gfp_t gfpflags)
{
	if (unlikely(slab_test_pfmemalloc(slab)))
		return gfp_pfmemalloc_allowed(gfpflags);

	return true;
}

/*
 * get_freelist_nofreeze() - 原子地取出 slab 的完整 freelist 并将 inuse 设为满
 *
 * 背景：refill_objects() 需要从 partial slab 批量取出所有空闲对象，
 * 而不是逐个 CAS。get_freelist_nofreeze 一次性用 CAS 将 slab 标记为
 * "全用完"（inuse=objects, freelist=NULL），同时拿回旧的 freelist 指针，
 * 后续再逐个遍历链表把对象装进 sheaf。
 *
 * "nofreeze"：不设置 frozen bit（旧版 SLUB 会在取 freelist 时设 frozen
 * 表示该 slab 被某 CPU "持有"；现在的 sheaf 设计不再需要 frozen 语义，
 * 因此保持 frozen=0，slab 加入 partial list 后可被任意 CPU 取走）。
 *
 * 前提：调用方已将 slab 从 partial list 摘出（SL_partial 已清除）且
 * slab 未被 frozen，否则 VM_WARN_ON_ONCE 会触发。
 *
 * @count 输出：被取出的空闲对象数（old.objects - old.inuse）。
 */
static inline void *get_freelist_nofreeze(struct kmem_cache *s, struct slab *slab,
					  unsigned int *count)
{
	struct freelist_counters old, new;

	do {
		old.freelist = slab->freelist;
		old.counters = slab->counters;

		new.freelist = NULL;
		new.counters = old.counters;
		VM_WARN_ON_ONCE(new.frozen);

		new.inuse = old.objects;

	} while (!slab_update_freelist(s, slab, &old, &new, "get_freelist_nofreeze"));

	*count = old.objects - old.inuse;
	return old.freelist;
}

/*
 * maybe_wipe_obj_freeptr() - 在对象交给用户前清零 freepointer 字段
 *
 * 背景：当 init_on_free 开启时，对象在释放时会被 memset 清零，
 * 包括 freepointer 所在的字节。但 freepointer 字段（object + offset）
 * 在 freelist 链表时存放了下一个对象的地址，这个值本不属于用户。
 * 如果 freepointer 在对象内部（!freeptr_outside_object），且用户拿到
 * 对象后读取了这个位置，会读到本该被清零的 freelist 地址。
 * 因此在交给用户前把这个字段清零，确保用户看到的是干净的 zero bytes。
 *
 * 仅在 freepointer 在对象内部时需要清零（外部时该字段不在对象范围内）。
 */
static __always_inline void maybe_wipe_obj_freeptr(struct kmem_cache *s,
						   void *obj)
{
	if (unlikely(slab_want_init_on_free(s)) && obj &&
	    !freeptr_outside_object(s))
		memset((void *)((char *)kasan_reset_tag(obj) + s->offset),
			0, sizeof(void *));
}

/*
 * alloc_from_new_slab() - 从刚从伙伴系统申请的全新 slab 中批量取出对象
 *
 * 背景：新 slab 页（allocate_slab 刚返回）尚无 freelist，也不在任何
 * partial list 上。本函数完成两件事：
 *   1. 按（可能随机化的）顺序从 slab 中取出 count 个对象（写入 p[]）
 *   2. 把剩余空闲对象串成 freelist，将 slab 挂入 partial list（若还有剩余）
 *
 * 若 count >= slab->objects（全部取走），则 slab 不进入 partial list
 * （needs_add_partial=false），减少一次 spinlock 操作。
 *
 * trylock 失败（allow_spin=false，原子上下文）：
 *   没法持 list_lock，只能丢弃刚分配的 slab（free_new_slab_nolock），
 *   返回 0 告知调用方本次分配失败，由调用方重试或换策略。
 *   代价是浪费了一次伙伴系统分配，但保证了不死锁。
 */
static unsigned int alloc_from_new_slab(struct kmem_cache *s, struct slab *slab,
		void **p, unsigned int count, bool allow_spin)
{
	unsigned int allocated = 0;
	struct slab_obj_iter iter;
	bool needs_add_partial = true;
	unsigned long flags;

	/*
	 * Are we going to put the slab on the partial list?
	 * Note slab->inuse is 0 on a new slab.
	 */
	if (count >= slab->objects) {
		needs_add_partial = false;
		count = slab->objects;
	}

	init_slab_obj_iter(s, slab, &iter, allow_spin);

	while (allocated < count) {
		p[allocated] = next_slab_obj(s, &iter);
		allocated++;
	}
	slab->inuse = count;
	build_slab_freelist(s, slab, &iter);

	if (needs_add_partial) {
		struct kmem_cache_node *n = get_node(s, slab_nid(slab));

		if (allow_spin) {
			spin_lock_irqsave(&n->list_lock, flags);
		} else if (!spin_trylock_irqsave(&n->list_lock, flags)) {
			/*
			 * Unlucky, discard newly allocated slab.
			 * The slab is not fully free, but it's fine as
			 * objects are not allocated to users.
			 */
			free_new_slab_nolock(s, slab);
			return 0;
		}
		add_partial(n, slab, ADD_TO_HEAD);
		spin_unlock_irqrestore(&n->list_lock, flags);
	}

	inc_slabs_node(s, slab_nid(slab), slab->objects);
	return allocated;
}

/*
 * ___slab_alloc() - 分配慢速路径：从 partial list 或新 slab 取对象
 *
 * 背景：per-CPU sheaf 快速路径失败（sheaf 空、锁竞争、NUMA 不匹配、
 * 调试 cache 等）时走到这里。相比快速路径，这里的操作涉及全局锁，
 * 代价高得多，但频率也低得多（理想情况下每 sheaf_capacity 次分配触发一次）。
 *
 * NUMA 三阶段策略（node != NUMA_NO_NODE 且未设 __GFP_THISNODE 时）：
 *   阶段 1：try_thisnode=true，trynode_flags 加 __GFP_THISNODE
 *     → 优先从目标节点的 partial list 取
 *     → 目标节点申请新 slab（GFP_NOWAIT，不阻塞，快速失败）
 *   阶段 2（阶段 1 全失败）：try_thisnode=false，trynode_flags 恢复原始
 *     → 允许跨节点 partial list（get_from_any_partial）
 *     → 允许从任意节点申请新 slab（原始 gfpflags，可阻塞）
 *
 * 设计意图：
 *   "先尽力本地，再允许跨节点"。第一阶段用 GFP_NOWAIT 是为了不在目标节点
 *   触发耗时的内存回收，若快速失败就立即尝试跨节点方案，总延迟更可控。
 *
 * success 标签：统一的分配后处理（alloc_tagging / kasan / memcg hook）。
 */
static void *___slab_alloc(struct kmem_cache *s, gfp_t gfpflags, int node,
			   const struct slab_alloc_context *ac)
{
	bool allow_spin = alloc_flags_allow_spinning(ac->alloc_flags);
	gfp_t trynode_flags;
	void *object;
	struct slab *slab;
	bool try_thisnode = true;

	stat(s, ALLOC_SLOWPATH);

new_objects:

	trynode_flags = gfpflags;
	/*
	 * When a preferred node is indicated but no __GFP_THISNODE
	 *
	 * 1) try to get a partial slab from target node only by having
	 *    __GFP_THISNODE in trynode_flags for get_from_partial()
	 * 2) if 1) failed, try to allocate a new slab from target node with
	 *    (at most) GFP_NOWAIT | __GFP_THISNODE opportunistically
	 * 3) if 2) failed, retry with original gfpflags which will allow
	 *    get_from_partial() try partial lists of other nodes before
	 *    potentially allocating new page from other nodes
	 */
	if (unlikely(node != NUMA_NO_NODE && !(gfpflags & __GFP_THISNODE)
		     && try_thisnode)) {
		trynode_flags &= GFP_NOWAIT | __GFP_NOMEMALLOC | __GFP_ACCOUNT;
		trynode_flags |= __GFP_NOWARN | __GFP_THISNODE;
	}

	object = get_from_partial(s, node, trynode_flags, ac);
	if (object)
		goto success;

	slab = new_slab(s, trynode_flags, ac->alloc_flags, node);

	if (unlikely(!slab)) {
		if (node != NUMA_NO_NODE && !(gfpflags & __GFP_THISNODE)
		    && try_thisnode) {
			try_thisnode = false;
			goto new_objects;
		}
		slab_out_of_memory(s, gfpflags, node);
		return NULL;
	}

	stat(s, ALLOC_SLAB);

	if (IS_ENABLED(CONFIG_SLUB_TINY) || kmem_cache_debug(s)) {
		object = alloc_single_from_new_slab(s, slab, ac);

		if (likely(object))
			goto success;
	} else {
		/* we don't need to check SLAB_STORE_USER here */
		if (alloc_from_new_slab(s, slab, &object, 1, allow_spin))
			return object;
	}

	if (allow_spin)
		goto new_objects;

	/* This could cause an endless loop. Fail instead. */
	return NULL;

success:
	if (kmem_cache_debug_flags(s, SLAB_STORE_USER))
		set_track(s, object, TRACK_ALLOC, ac->caller_addr, gfpflags);

	return object;
}

/*
 * __slab_alloc_node() - 慢速路径入口：处理 NUMA 内存策略后转 ___slab_alloc
 *
 * 背景：Linux 的 NUMA 内存策略（mempolicy）允许进程通过 set_mempolicy()
 * 控制内存从哪个节点分配。strict_numa 是一个静态分支（static key），
 * 在启用严格 NUMA 策略时才激活（避免在单节点或不关心 NUMA 的系统上有开销）。
 *
 * MPOL_BIND 特殊处理：
 *   BIND 策略将进程绑定到一组允许的节点。如果本地节点在允许集中，
 *   无需重定向（直接用 NUMA_NO_NODE，让 alloc 在本地节点分配）；
 *   若本地节点不在允许集中，才调用 mempolicy_slab_node() 选出一个
 *   允许的节点。这样保证 BIND 策略被尊重，同时尽量维持 NUMA 局部性。
 */
static void *__slab_alloc_node(struct kmem_cache *s, gfp_t gfpflags, int node,
			       const struct slab_alloc_context *ac)
{
	void *object;

#ifdef CONFIG_NUMA
	if (static_branch_unlikely(&strict_numa) &&
			node == NUMA_NO_NODE) {

		struct mempolicy *mpol = current->mempolicy;

		if (mpol) {
			/*
			 * Special BIND rule support. If the local node
			 * is in permitted set then do not redirect
			 * to a particular node.
			 * Otherwise we apply the memory policy to get
			 * the node we need to allocate on.
			 */
			if (mpol->mode != MPOL_BIND ||
					!node_isset(numa_mem_id(), mpol->nodes))
				node = mempolicy_slab_node();
		}
	}
#endif

	object = ___slab_alloc(s, gfpflags, node, ac);

	return object;
}

/*
 * slab_pre_alloc_hook() - 分配前的公共检查
 *
 * might_alloc()：提示 lockdep 当前上下文允许分配内存，检测在不该分配内存
 *   的上下文（如持某些锁时）调用 kmalloc 的 bug。
 * should_failslab()：fault injection 支持——在测试中按配置的概率让分配失败，
 *   用于测试 OOM 处理路径的正确性。
 *
 * 返回 NULL 表示分配被拒绝（fault injection），调用方应返回 NULL。
 */
static __fastpath_inline
struct kmem_cache *slab_pre_alloc_hook(struct kmem_cache *s, gfp_t flags)
{
	flags &= gfp_allowed_mask;

	might_alloc(flags);

	if (unlikely(should_failslab(s, flags)))
		return NULL;

	return s;
}

/*
 * slab_post_alloc_hook() - 分配后的公共清理与安全初始化
 *
 * 背景：SLUB 自身只完成分配（找到空闲槽位），但现代内核有多个子系统
 * 需要在每次分配后做一些工作，这些工作统一在此处完成：
 *
 * 1. init_on_alloc（zero-fill）：
 *    若开启 init_on_alloc，将对象清零，防止信息泄漏（堆数据残留）。
 *    zero_size 通常是 object_size，但若追踪了 orig_size，只清零用户
 *    请求的部分（多余的空间是 kmalloc 槽位对齐碎片，不属于用户）；
 *    若有 red zone，也不清零红区（否则破坏调试标记）。
 *
 * 2. KASAN（内核地址消毒剂）：
 *    kasan_slab_alloc 标记对象为"可访问"，同时可以做 shadow 初始化。
 *    ARM64 HW_TAGS KASAN 能用一条指令同时设置 tag 和清零，所以与
 *    init_on_alloc 合并（kasan_init=true 时 KASAN 负责清零，SLUB 不重复）。
 *    调试模式下 KASAN 不清零（避免覆盖 red zone），由上层显式 memset。
 *
 * 3. kmemleak：注册分配记录，供内存泄漏检测扫描。
 * 4. KMSAN：标记内存为"未初始化"（若未 zero-fill），检测未初始化读。
 * 5. alloc_tagging：更新 per-callsite 分配计数（内存分配 profiling）。
 * 6. memcg_slab_post_alloc_hook：将对象计入对应 memcg 的内存配额。
 */
static __fastpath_inline
bool slab_post_alloc_hook(struct kmem_cache *s, gfp_t flags, size_t size,
			  void **p, const struct slab_alloc_context *ac)
{
	bool init = slab_want_init_on_alloc(flags, s);
	unsigned int zero_size = s->object_size;
	gfp_t init_flags = flags & gfp_allowed_mask;
	bool kasan_init = false;

	/*
	 * For kmalloc object, the allocated size (object_size) can be larger
	 * than the requested size (orig_size). We however need to zero the
	 * whole object_size to handle possible later krealloc() with
	 *__GFP_ZERO properly.
	 *
	 * But if we keep track of the requested size, krealloc() uses that
	 * information. Additionally if red zoning is enabled, the extra space
	 * is also red zone, so we should not overwrite it. So limit zeroing to
	 * orig_size if we track it.
	 */
	if (slub_debug_orig_size(s))
		zero_size = ac->orig_size;

	/*
	 * ARM64 can set memory tags and zero the memory using a single
	 * instruction. Since HW_TAGS KASAN uses that while tagging the object,
	 * separate zeroing is unnecessary.
	 *
	 * However, KASAN never zeroes memory when slab_debug is enabled to
	 * avoid overwriting SLUB redzones. This does not lead to a performance
	 * penalty on production builds, as slab_debug is not intended to be
	 * enabled there.
	 */
	if (kasan_has_integrated_init() && !__slub_debug_enabled()) {
		kasan_init = init;
		init = false;
	}

	for (size_t i = 0; i < size; i++) {
		p[i] = kasan_slab_alloc(s, p[i], init_flags, kasan_init);

		/*
		 * memset and hooks come after KASAN as p[i] might get tagged
		 *
		 * kfence zeroes the object instead of SLUB to avoid overwriting
		 * its own redzone starting at orig_size, which could happen
		 * with SLUB zeroing full s->object_size
		 */
		if (init && p[i] && !is_kfence_address(p[i]))
			memset(p[i], 0, zero_size);

		if (alloc_flags_allow_spinning(ac->alloc_flags))
			kmemleak_alloc_recursive(p[i], s->object_size, 1,
						 s->flags, init_flags);
		kmsan_slab_alloc(s, p[i], init_flags);
		alloc_tagging_slab_alloc_hook(s, p[i], flags, ac->alloc_flags);
	}

	return memcg_slab_post_alloc_hook(s, flags, size, p, ac);
}

/*
 * __pcs_replace_empty_main() - 用一个满/半满 sheaf 替换耗尽的 main sheaf
 *
 * 背景：alloc_from_pcs() 发现 main->size == 0 时调用本函数，
 * 目标是找到一个有对象的 sheaf 来继续快速分配。
 *
 * 调用约束：必须已持 cpu_sheaves->lock。成功返回时锁仍持有（可能换了 CPU）；
 * 失败返回 NULL 时锁已释放（让调用方直接返回 NULL 降级到慢速路径）。
 *
 * 分级策略（从快到慢）：
 *
 * 1. spare 有对象（spare->size > 0）：
 *    swap(main, spare)，一次指针交换，全程持锁，最快。
 *    场景：main 刚被分配耗尽，上一轮补货时多填了一个 spare。
 *
 * 2. barn 有满 sheaf（barn_replace_empty_sheaf 成功）：
 *    把空 main 存入 barn，取出一个满 sheaf 作为新 main，
 *    只需一次 spinlock（barn->lock），无 slab freelist 操作。
 *
 * 3. barn 没有满 sheaf，需要手动填充（慢路径）：
 *    - 解锁 cpu_sheaves->lock（后续操作可能休眠）
 *    - 取一个空 sheaf（从 spare 或 barn 或新分配）
 *    - 调用 refill_sheaf() 从 partial list / 伙伴系统填满 sheaf
 *    - 重新 trylock cpu_sheaves->lock，将填满的 sheaf 装回 pcs
 *
 * 并发情况处理（步骤 3 解锁后可能 CPU 迁移或 spare 被其他路径用掉）：
 *    重新 trylock 后检查当前 pcs 状态：
 *    - main 仍空 → 新 full 设为 main，旧空 main 放 spare 或归还 barn
 *    - main 非空但 spare 空 → 新 full 放 spare（备用）
 *    - spare 为空 sheaf → 把空 spare 存 barn，新 full 放 spare
 *    - 都有对象 → barn_put_full_sheaf（竞争下产生了多余的满 sheaf）
 *
 * 注意：barn 满时（barn_replace_empty_sheaf 返回 NULL）直接解锁降级，
 * 因为此时连临时存放 empty 的空间都没有，只能靠慢速路径。
 */
static struct slub_percpu_sheaves *
__pcs_replace_empty_main(struct kmem_cache *s, struct slub_percpu_sheaves *pcs,
			 gfp_t gfp, unsigned int alloc_flags)
{
	struct slab_sheaf *empty = NULL;
	struct slab_sheaf *full;
	struct node_barn *barn;
	bool allow_spin;

	lockdep_assert_held(this_cpu_ptr(&s->cpu_sheaves->lock));

	/* Bootstrap or debug cache, back off */
	if (unlikely(!cache_has_sheaves(s))) {
		local_unlock(&s->cpu_sheaves->lock);
		return NULL;
	}

	if (pcs->spare && pcs->spare->size > 0) {
		swap(pcs->main, pcs->spare);
		return pcs;
	}

	barn = get_barn(s);
	if (!barn) {
		local_unlock(&s->cpu_sheaves->lock);
		return NULL;
	}

	allow_spin = alloc_flags_allow_spinning(alloc_flags);

	full = barn_replace_empty_sheaf(barn, pcs->main, allow_spin);

	if (full) {
		stat(s, BARN_GET);
		pcs->main = full;
		return pcs;
	}

	stat(s, BARN_GET_FAIL);

	if (allow_spin) {
		if (pcs->spare) {
			empty = pcs->spare;
			pcs->spare = NULL;
		} else {
			empty = barn_get_empty_sheaf(barn, true);
		}
	}

	local_unlock(&s->cpu_sheaves->lock);
	pcs = NULL;

	if (!allow_spin)
		return NULL;

	if (!empty) {
		empty = alloc_empty_sheaf(s, gfp, alloc_flags);
		if (!empty)
			return NULL;
	}

	if (refill_sheaf(s, empty, gfp | __GFP_NOMEMALLOC | __GFP_NOWARN)) {
		/*
		 * we must be very low on memory so don't bother
		 * with the barn
		 */
		sheaf_flush_unused(s, empty);
		free_empty_sheaf(s, empty);

		return NULL;
	}

	full = empty;
	empty = NULL;

	if (!local_trylock(&s->cpu_sheaves->lock))
		goto barn_put;
	pcs = this_cpu_ptr(s->cpu_sheaves);

	/*
	 * If we put any empty or full sheaf to the barn below, it's due to
	 * racing or being migrated to a different cpu. Breaching the barn's
	 * sheaf limits should be thus rare enough so just ignore them to
	 * simplify the recovery.
	 */

	if (pcs->main->size == 0) {
		if (!pcs->spare)
			pcs->spare = pcs->main;
		else
			barn_put_empty_sheaf(barn, pcs->main);
		pcs->main = full;
		return pcs;
	}

	if (!pcs->spare) {
		pcs->spare = full;
		return pcs;
	}

	if (pcs->spare->size == 0) {
		barn_put_empty_sheaf(barn, pcs->spare);
		pcs->spare = full;
		return pcs;
	}

barn_put:
	barn_put_full_sheaf(barn, full);
	stat(s, BARN_PUT);

	return pcs;
}

/*
 * alloc_from_pcs() - 分配快速路径：从 per-CPU sheaf 弹出一个对象
 *
 * 这是整个 SLUB 的性能最关键路径，应当尽可能快地完成。
 * 在非调试、非 NUMA 约束的理想情况下，最终只做：
 *   local_trylock → size-- → object = objects[size] → local_unlock
 *
 * NUMA 两阶段检查设计：
 *   为什么需要两次检查？
 *   per-CPU sheaf 的设计假设"当前 CPU 上的 sheaf 主要含本地对象"，
 *   但这不是绝对保证：在无锁区间（__pcs_replace_empty_main 解锁 + 填充
 *   期间，或上一次 free_to_pcs 的无锁区间），CPU 可能被迁移到另一个节点。
 *
 *   第一次检查（软件 node ID）：node != numa_mem_id()
 *     若请求的 node 与当前 CPU 所在节点不同，sheaf 里的对象大概率来自
 *     错误节点，直接返回 NULL，避免无谓的 trylock + NUMA 验证。
 *     这是"乐观过滤"：有极小概率误判（CPU 刚好迁移），但代价低。
 *
 *   第二次检查（物理 page_to_nid）：实际验证取出的对象来自哪个 node
 *     真正的物理验证，在 trylock 内完成。
 *     为什么在第一次检查后还需要？
 *     因为在 trylock 前的瞬间 CPU 可能再次迁移，导致 sheaf 里混有
 *     其他节点的对象。物理验证保证最终的正确性。
 *
 * local_trylock 语义：
 *   非 RT 内核：仅禁止抢占（preempt_disable），无内存屏障，极低开销。
 *   PREEMPT_RT 内核：使用真正的自旋锁（允许中断但不允许其他 RT 任务抢占）。
 *   失败时立即返回 NULL（不自旋等待），降级到慢速路径，避免在快速路径上浪费时间。
 */
static __fastpath_inline
void *alloc_from_pcs(struct kmem_cache *s, gfp_t gfp, unsigned int alloc_flags, int node)
{
	struct slub_percpu_sheaves *pcs;
	bool node_requested;
	void *object;

#ifdef CONFIG_NUMA
	if (static_branch_unlikely(&strict_numa) &&
			 node == NUMA_NO_NODE) {

		struct mempolicy *mpol = current->mempolicy;

		if (mpol) {
			/*
			 * Special BIND rule support. If the local node
			 * is in permitted set then do not redirect
			 * to a particular node.
			 * Otherwise we apply the memory policy to get
			 * the node we need to allocate on.
			 */
			if (mpol->mode != MPOL_BIND ||
					!node_isset(numa_mem_id(), mpol->nodes))

				node = mempolicy_slab_node();
		}
	}
#endif

	node_requested = IS_ENABLED(CONFIG_NUMA) && node != NUMA_NO_NODE;

	/*
	 * We assume the percpu sheaves contain only local objects although it's
	 * not completely guaranteed, so we verify later.
	 */
	if (unlikely(node_requested && node != numa_mem_id())) {
		stat(s, ALLOC_NODE_MISMATCH);
		return NULL;
	}

	if (!local_trylock(&s->cpu_sheaves->lock))
		return NULL;

	pcs = this_cpu_ptr(s->cpu_sheaves);

	if (unlikely(pcs->main->size == 0)) {
		pcs = __pcs_replace_empty_main(s, pcs, gfp, alloc_flags);
		if (unlikely(!pcs))
			return NULL;
	}

	object = pcs->main->objects[pcs->main->size - 1];

	if (unlikely(node_requested)) {
		/*
		 * Verify that the object was from the node we want. This could
		 * be false because of cpu migration during an unlocked part of
		 * the current allocation or previous freeing process.
		 */
		if (page_to_nid(virt_to_page(object)) != node) {
			local_unlock(&s->cpu_sheaves->lock);
			stat(s, ALLOC_NODE_MISMATCH);
			return NULL;
		}
	}

	pcs->main->size--;

	local_unlock(&s->cpu_sheaves->lock);

	stat(s, ALLOC_FASTPATH);

	return object;
}

static __fastpath_inline
unsigned int alloc_from_pcs_bulk(struct kmem_cache *s, size_t size, void **p)
{
	struct slub_percpu_sheaves *pcs;
	struct slab_sheaf *main;
	unsigned int allocated = 0;
	unsigned int batch;

next_batch:
	if (!local_trylock(&s->cpu_sheaves->lock))
		return allocated;

	pcs = this_cpu_ptr(s->cpu_sheaves);

	if (unlikely(pcs->main->size == 0)) {

		struct slab_sheaf *full;
		struct node_barn *barn;

		if (unlikely(!cache_has_sheaves(s))) {
			local_unlock(&s->cpu_sheaves->lock);
			return allocated;
		}

		if (pcs->spare && pcs->spare->size > 0) {
			swap(pcs->main, pcs->spare);
			goto do_alloc;
		}

		barn = get_barn(s);
		if (!barn) {
			local_unlock(&s->cpu_sheaves->lock);
			return allocated;
		}

		full = barn_replace_empty_sheaf(barn, pcs->main,
						/* allow_spin = */ true);

		if (full) {
			stat(s, BARN_GET);
			pcs->main = full;
			goto do_alloc;
		}

		stat(s, BARN_GET_FAIL);

		local_unlock(&s->cpu_sheaves->lock);

		/*
		 * Once full sheaves in barn are depleted, let the bulk
		 * allocation continue from slab pages, otherwise we would just
		 * be copying arrays of pointers twice.
		 */
		return allocated;
	}

do_alloc:

	main = pcs->main;
	batch = min(size, main->size);

	main->size -= batch;
	memcpy(p, main->objects + main->size, batch * sizeof(void *));

	local_unlock(&s->cpu_sheaves->lock);

	stat_add(s, ALLOC_FASTPATH, batch);

	allocated += batch;

	if (batch < size) {
		p += batch;
		size -= batch;
		goto next_batch;
	}

	return allocated;
}


/*
 * slab_alloc_node() - 所有 kmem_cache_alloc* 的内联分配总入口
 *
 * 设计目标：把快速路径内联到 kmem_cache_alloc() 的调用点，消除函数调用开销。
 * 编译器可以把整个 alloc_from_pcs() 快速路径直接展开在调用处，
 * 若 sheaf 非空则只有几条指令，接近裸指针操作的性能。
 *
 * 执行流程：
 *   1. slab_pre_alloc_hook()：fault injection 检查、might_alloc() 提示。
 *      返回 NULL 时本次分配被 fault injection 故意失败。
 *
 *   2. kfence_alloc()：KFENCE 采样分配（~1/512 概率）。
 *      KFENCE 用专用页（每对象独占一页 + guard page）精确检测
 *      use-after-free 和越界写，无需全量 shadow 内存。
 *      概率触发避免性能开销，同时保证统计意义上的覆盖率。
 *      若 KFENCE 接管了分配，跳过正常路径直接去 out。
 *
 *   3. alloc_from_pcs()：per-CPU sheaf 快速路径。
 *      返回 NULL 表示 sheaf 空、锁竞争或 NUMA 不匹配。
 *
 *   4. __slab_alloc_node()：慢速路径（partial list → 伙伴系统）。
 *
 *   5. maybe_wipe_obj_freeptr()：init_on_free 场景下清零 freepointer 字段。
 *
 *   out:
 *   6. slab_post_alloc_hook()：KASAN 标记、zero-fill、kmemleak、memcg 计费。
 *      若 memcg 计费失败（OOM 情况），object 被设为 NULL，返回 NULL。
 */
static __fastpath_inline void *slab_alloc_node(struct kmem_cache *s,
		gfp_t gfpflags, int node, const struct slab_alloc_context *ac)
{
	void *object;

	s = slab_pre_alloc_hook(s, gfpflags);
	if (unlikely(!s))
		return NULL;

	object = kfence_alloc(s, ac->orig_size, gfpflags);
	if (unlikely(object))
		goto out;

	object = alloc_from_pcs(s, gfpflags, ac->alloc_flags, node);

	if (unlikely(!object))
		object = __slab_alloc_node(s, gfpflags, node, ac);

	maybe_wipe_obj_freeptr(s, object);

out:
	/*
	 * In case this fails due to memcg_slab_post_alloc_hook(),
	 * object is set to NULL
	 */
	slab_post_alloc_hook(s, gfpflags, 1, &object, ac);

	return object;
}

void *kmem_cache_alloc_noprof(struct kmem_cache *s, gfp_t gfpflags)
{
	void *ret;
	const struct slab_alloc_context ac = {
		.caller_addr = _RET_IP_,
		.orig_size = s->object_size,
		.alloc_flags = SLAB_ALLOC_DEFAULT,
	};

	ret = slab_alloc_node(s, gfpflags, NUMA_NO_NODE, &ac);

	trace_kmem_cache_alloc(_RET_IP_, ret, s, gfpflags, NUMA_NO_NODE);

	return ret;
}
EXPORT_SYMBOL(kmem_cache_alloc_noprof);

void *kmem_cache_alloc_lru_noprof(struct kmem_cache *s, struct list_lru *lru,
			   gfp_t gfpflags)
{
	void *ret;
	const struct slab_alloc_context ac = {
		.caller_addr = _RET_IP_,
		.orig_size = s->object_size,
		.alloc_flags = SLAB_ALLOC_DEFAULT,
		.lru = lru,
	};

	ret = slab_alloc_node(s, gfpflags, NUMA_NO_NODE, &ac);

	trace_kmem_cache_alloc(_RET_IP_, ret, s, gfpflags, NUMA_NO_NODE);

	return ret;
}
EXPORT_SYMBOL(kmem_cache_alloc_lru_noprof);

bool kmem_cache_charge(void *objp, gfp_t gfpflags)
{
	if (!memcg_kmem_online())
		return true;

	return memcg_slab_post_charge(objp, gfpflags);
}
EXPORT_SYMBOL(kmem_cache_charge);

/**
 * kmem_cache_alloc_node - Allocate an object on the specified node
 * @s: The cache to allocate from.
 * @gfpflags: See kmalloc().
 * @node: node number of the target node.
 *
 * Identical to kmem_cache_alloc but it will allocate memory on the given
 * node, which can improve the performance for cpu bound structures.
 *
 * Fallback to other node is possible if __GFP_THISNODE is not set.
 *
 * Return: pointer to the new object or %NULL in case of error
 */
void *kmem_cache_alloc_node_noprof(struct kmem_cache *s, gfp_t gfpflags, int node)
{
	void *ret;
	const struct slab_alloc_context ac = {
		.caller_addr = _RET_IP_,
		.orig_size = s->object_size,
		.alloc_flags = SLAB_ALLOC_DEFAULT,
	};

	ret = slab_alloc_node(s, gfpflags, node, &ac);

	trace_kmem_cache_alloc(_RET_IP_, ret, s, gfpflags, node);

	return ret;
}
EXPORT_SYMBOL(kmem_cache_alloc_node_noprof);

static int __prefill_sheaf_pfmemalloc(struct kmem_cache *s,
				      struct slab_sheaf *sheaf, gfp_t gfp)
{
	gfp_t gfp_nomemalloc;
	int ret;

	gfp_nomemalloc = gfp | __GFP_NOMEMALLOC;
	if (gfp_pfmemalloc_allowed(gfp))
		gfp_nomemalloc |= __GFP_NOWARN;

	ret = refill_sheaf(s, sheaf, gfp_nomemalloc);

	if (likely(!ret || !gfp_pfmemalloc_allowed(gfp)))
		return ret;

	/*
	 * if we are allowed to, refill sheaf with pfmemalloc but then remember
	 * it for when it's returned
	 */
	ret = refill_sheaf(s, sheaf, gfp);
	sheaf->pfmemalloc = true;

	return ret;
}

static bool __kmem_cache_alloc_bulk(struct kmem_cache *s, gfp_t flags,
		size_t size, void **p);

/*
 * returns a sheaf that has at least the requested size
 * when prefilling is needed, do so with given gfp flags
 *
 * return NULL if sheaf allocation or prefilling failed
 */
struct slab_sheaf *
kmem_cache_prefill_sheaf(struct kmem_cache *s, gfp_t gfp, unsigned int size)
{
	struct slub_percpu_sheaves *pcs;
	struct slab_sheaf *sheaf = NULL;
	struct node_barn *barn;

	if (unlikely(!size))
		return NULL;

	if (unlikely(size > s->sheaf_capacity)) {

		sheaf = __alloc_empty_sheaf(s, gfp, SLAB_ALLOC_DEFAULT, size);
		if (!sheaf)
			return NULL;

		stat(s, SHEAF_PREFILL_OVERSIZE);
		sheaf->capacity = size;

		/*
		 * we do not need to care about pfmemalloc here because oversize
		 * sheaves are always flushed and freed when returned
		 */
		if (!__kmem_cache_alloc_bulk(s, gfp, size,
					     &sheaf->objects[0])) {
			free_empty_sheaf(s, sheaf);
			return NULL;
		}

		sheaf->size = size;

		return sheaf;
	}

	local_lock(&s->cpu_sheaves->lock);
	pcs = this_cpu_ptr(s->cpu_sheaves);

	if (pcs->spare) {
		sheaf = pcs->spare;
		pcs->spare = NULL;
		stat(s, SHEAF_PREFILL_FAST);
	} else {
		barn = get_barn(s);

		stat(s, SHEAF_PREFILL_SLOW);
		if (barn)
			sheaf = barn_get_full_or_empty_sheaf(barn);
		if (sheaf && sheaf->size)
			stat(s, BARN_GET);
		else
			stat(s, BARN_GET_FAIL);
	}

	local_unlock(&s->cpu_sheaves->lock);


	if (!sheaf)
		sheaf = alloc_empty_sheaf(s, gfp, SLAB_ALLOC_DEFAULT);

	if (sheaf) {
		sheaf->capacity = s->sheaf_capacity;
		sheaf->pfmemalloc = false;

		if (sheaf->size < size &&
		    __prefill_sheaf_pfmemalloc(s, sheaf, gfp)) {
			sheaf_flush_unused(s, sheaf);
			free_empty_sheaf(s, sheaf);
			sheaf = NULL;
		}
	}

	return sheaf;
}

/*
 * Use this to return a sheaf obtained by kmem_cache_prefill_sheaf()
 *
 * If the sheaf cannot simply become the percpu spare sheaf, but there's space
 * for a full sheaf in the barn, we try to refill the sheaf back to the cache's
 * sheaf_capacity to avoid handling partially full sheaves.
 *
 * If the refill fails because gfp is e.g. GFP_NOWAIT, or the barn is full, the
 * sheaf is instead flushed and freed.
 */
void kmem_cache_return_sheaf(struct kmem_cache *s, gfp_t gfp,
			     struct slab_sheaf *sheaf)
{
	struct slub_percpu_sheaves *pcs;
	struct node_barn *barn;

	if (unlikely((sheaf->capacity != s->sheaf_capacity)
		     || sheaf->pfmemalloc)) {
		sheaf_flush_unused(s, sheaf);
		free_empty_sheaf(s, sheaf);
		return;
	}

	local_lock(&s->cpu_sheaves->lock);
	pcs = this_cpu_ptr(s->cpu_sheaves);
	barn = get_barn(s);

	if (!pcs->spare) {
		pcs->spare = sheaf;
		sheaf = NULL;
		stat(s, SHEAF_RETURN_FAST);
	}

	local_unlock(&s->cpu_sheaves->lock);

	if (!sheaf)
		return;

	stat(s, SHEAF_RETURN_SLOW);

	/*
	 * If the barn has too many full sheaves or we fail to refill the sheaf,
	 * simply flush and free it.
	 */
	if (!barn || data_race(barn->nr_full) >= MAX_FULL_SHEAVES ||
	    refill_sheaf(s, sheaf, gfp)) {
		sheaf_flush_unused(s, sheaf);
		free_empty_sheaf(s, sheaf);
		return;
	}

	barn_put_full_sheaf(barn, sheaf);
	stat(s, BARN_PUT);
}

/*
 * Refill a sheaf previously returned by kmem_cache_prefill_sheaf to at least
 * the given size.
 *
 * Return: 0 on success. The sheaf will contain at least @size objects.
 * The sheaf might have been replaced with a new one if more than
 * sheaf->capacity objects are requested.
 *
 * Return: -ENOMEM on failure. Some objects might have been added to the sheaf
 * but the sheaf will not be replaced.
 *
 * In practice we always refill to full sheaf's capacity.
 */
int kmem_cache_refill_sheaf(struct kmem_cache *s, gfp_t gfp,
			    struct slab_sheaf **sheafp, unsigned int size)
{
	struct slab_sheaf *sheaf;

	/*
	 * TODO: do we want to support *sheaf == NULL to be equivalent of
	 * kmem_cache_prefill_sheaf() ?
	 */
	if (!sheafp || !(*sheafp))
		return -EINVAL;

	sheaf = *sheafp;
	if (sheaf->size >= size)
		return 0;

	if (likely(sheaf->capacity >= size)) {
		if (likely(sheaf->capacity == s->sheaf_capacity))
			return __prefill_sheaf_pfmemalloc(s, sheaf, gfp);

		if (!__kmem_cache_alloc_bulk(s, gfp, sheaf->capacity - sheaf->size,
					     &sheaf->objects[sheaf->size]))
			return -ENOMEM;
		sheaf->size = sheaf->capacity;

		return 0;
	}

	/*
	 * We had a regular sized sheaf and need an oversize one, or we had an
	 * oversize one already but need a larger one now.
	 * This should be a very rare path so let's not complicate it.
	 */
	sheaf = kmem_cache_prefill_sheaf(s, gfp, size);
	if (!sheaf)
		return -ENOMEM;

	kmem_cache_return_sheaf(s, gfp, *sheafp);
	*sheafp = sheaf;
	return 0;
}

/*
 * Allocate from a sheaf obtained by kmem_cache_prefill_sheaf()
 *
 * Guaranteed not to fail as many allocations as was the requested size.
 * After the sheaf is emptied, it fails - no fallback to the slab cache itself.
 *
 * The gfp parameter is meant only to specify __GFP_ZERO or __GFP_ACCOUNT
 * memcg charging is forced over limit if necessary, to avoid failure.
 *
 * It is possible that the allocation comes from kfence and then the sheaf
 * size is not decreased.
 */
void *
kmem_cache_alloc_from_sheaf_noprof(struct kmem_cache *s, gfp_t gfp,
				   struct slab_sheaf *sheaf)
{
	void *ret = NULL;
	const struct slab_alloc_context ac = {
		.orig_size = s->object_size,
		.alloc_flags = SLAB_ALLOC_DEFAULT,
	};

	if (sheaf->size == 0)
		goto out;

	ret = kfence_alloc(s, s->object_size, gfp);

	if (likely(!ret))
		ret = sheaf->objects[--sheaf->size];

	/* add __GFP_NOFAIL to force successful memcg charging */
	slab_post_alloc_hook(s, gfp | __GFP_NOFAIL, 1, &ret, &ac);
out:
	trace_kmem_cache_alloc(_RET_IP_, ret, s, gfp, NUMA_NO_NODE);

	return ret;
}

unsigned int kmem_cache_sheaf_size(struct slab_sheaf *sheaf)
{
	return sheaf->size;
}
/*
 * To avoid unnecessary overhead, we pass through large allocation requests
 * directly to the page allocator. We use __GFP_COMP, because we will need to
 * know the allocation order to free the pages properly in kfree.
 */
static void *___kmalloc_large_node(size_t size, gfp_t flags, int node)
{
	struct page *page;
	void *ptr = NULL;
	unsigned int order = get_order(size);

	if (unlikely(flags & GFP_SLAB_BUG_MASK))
		flags = kmalloc_fix_flags(flags);

	flags |= __GFP_COMP;

	if (node == NUMA_NO_NODE)
		page = alloc_frozen_pages_noprof(flags, order);
	else
		page = __alloc_frozen_pages_noprof(flags, order, node, NULL);

	if (page) {
		ptr = page_address(page);
		mod_lruvec_page_state(page, NR_SLAB_UNRECLAIMABLE_B,
				      PAGE_SIZE << order);
		__SetPageLargeKmalloc(page);
	}

	ptr = kasan_kmalloc_large(ptr, size, flags);
	/* As ptr might get tagged, call kmemleak hook after KASAN. */
	kmemleak_alloc(ptr, size, 1, flags);
	kmsan_kmalloc_large(ptr, size, flags);

	return ptr;
}

void *__kmalloc_large_noprof(size_t size, gfp_t flags)
{
	void *ret = ___kmalloc_large_node(size, flags, NUMA_NO_NODE);

	trace_kmalloc(_RET_IP_, ret, size, PAGE_SIZE << get_order(size),
		      flags, NUMA_NO_NODE);
	return ret;
}
EXPORT_SYMBOL(__kmalloc_large_noprof);

void *__kmalloc_large_node_noprof(size_t size, gfp_t flags, int node)
{
	void *ret = ___kmalloc_large_node(size, flags, node);

	trace_kmalloc(_RET_IP_, ret, size, PAGE_SIZE << get_order(size),
		      flags, node);
	return ret;
}
EXPORT_SYMBOL(__kmalloc_large_node_noprof);

static __always_inline
void *__do_kmalloc_node(kmem_buckets *b, gfp_t flags, int node,
			kmalloc_token_t token, const struct slab_alloc_context *ac)
{
	const size_t size = ac->orig_size;
	struct kmem_cache *s;
	void *ret;

	if (unlikely(size > KMALLOC_MAX_CACHE_SIZE)) {
		ret = __kmalloc_large_node_noprof(size, flags, node);
		trace_kmalloc(ac->caller_addr, ret, size,
			      PAGE_SIZE << get_order(size), flags, node);
		return ret;
	}

	if (unlikely(!size))
		return ZERO_SIZE_PTR;

	s = kmalloc_slab(size, b, flags, token);

	ret = slab_alloc_node(s, flags, node, ac);
	ret = kasan_kmalloc(s, ret, size, flags);
	trace_kmalloc(ac->caller_addr, ret, size, s->size, flags, node);
	return ret;
}
void *__kmalloc_node_noprof(DECL_KMALLOC_PARAMS(size, b, token), gfp_t flags, int node)
{
	const struct slab_alloc_context ac = {
		.caller_addr = _RET_IP_,
		.orig_size = size,
		.alloc_flags = SLAB_ALLOC_DEFAULT,
	};

	return __do_kmalloc_node(PASS_BUCKET_PARAM(b), flags, node,
				 PASS_TOKEN_PARAM(token), &ac);
}
EXPORT_SYMBOL(__kmalloc_node_noprof);

void *__kmalloc_noprof(DECL_TOKEN_PARAMS(size, token), gfp_t flags)
{
	const struct slab_alloc_context ac = {
		.caller_addr = _RET_IP_,
		.orig_size = size,
		.alloc_flags = SLAB_ALLOC_DEFAULT,
	};

	return __do_kmalloc_node(NULL, flags,  NUMA_NO_NODE,
				 PASS_TOKEN_PARAM(token), &ac);
}
EXPORT_SYMBOL(__kmalloc_noprof);

static void *__kmalloc_nolock_noprof(DECL_TOKEN_PARAMS(size, token), gfp_t gfp_flags,
				     int node, const struct slab_alloc_context *ac)
{
	struct kmem_cache *s;
	bool can_retry = true;
	void *ret;

	VM_WARN_ON_ONCE(alloc_flags_allow_spinning(ac->alloc_flags));
	VM_WARN_ON_ONCE(gfp_flags & ~(__GFP_ACCOUNT | __GFP_ZERO |
				      __GFP_NOWARN | __GFP_NOMEMALLOC));

	gfp_flags |= __GFP_NOWARN | __GFP_NOMEMALLOC;

	if (unlikely(!size))
		return ZERO_SIZE_PTR;

	/*
	 * See the comment for the same check in
	 * alloc_frozen_pages_nolock_noprof()
	 */
	if (IS_ENABLED(CONFIG_PREEMPT_RT) && (in_nmi() || in_hardirq()))
		return NULL;

	/* On UP, spin_trylock() always succeeds even when it is locked */
	if (!IS_ENABLED(CONFIG_SMP) && in_nmi())
		return NULL;

retry:
	if (unlikely(size > KMALLOC_MAX_CACHE_SIZE))
		return NULL;
	s = kmalloc_slab(size, NULL, gfp_flags, PASS_TOKEN_PARAM(token));

	if (!(s->flags & __CMPXCHG_DOUBLE) && !kmem_cache_debug(s))
		/*
		 * kmalloc_nolock() is not supported on architectures that
		 * don't implement cmpxchg16b and thus need slab_lock()
		 * which could be preempted by a nmi.
		 * But debug caches don't use that and only rely on
		 * kmem_cache_node->list_lock, so kmalloc_nolock() can attempt
		 * to allocate from debug caches by
		 * spin_trylock_irqsave(&n->list_lock, ...)
		 */
		return NULL;

	ret = alloc_from_pcs(s, gfp_flags, ac->alloc_flags, node);
	if (ret)
		goto success;

	/*
	 * Do not call slab_alloc_node(), since trylock mode isn't
	 * compatible with slab_pre_alloc_hook/should_failslab and
	 * kfence_alloc. Hence call __slab_alloc_node() (at most twice)
	 * and slab_post_alloc_hook() directly.
	 */
	ret = __slab_alloc_node(s, gfp_flags, node, ac);

	/*
	 * It's possible we failed due to trylock as we preempted someone with
	 * the sheaves locked, and the list_lock is also held by another cpu.
	 * But it should be rare that multiple kmalloc buckets would have
	 * sheaves locked, so try a larger one.
	 */
	if (!ret && can_retry) {
		/* pick the next kmalloc bucket */
		size = s->object_size + 1;
		/*
		 * Another alternative is to
		 * if (memcg) gfp_flags &= ~__GFP_ACCOUNT;
		 * else if (!memcg) gfp_flags |= __GFP_ACCOUNT;
		 * to retry from bucket of the same size.
		 */
		can_retry = false;
		goto retry;
	}

success:
	maybe_wipe_obj_freeptr(s, ret);
	slab_post_alloc_hook(s, gfp_flags, 1, &ret, ac);

	ret = kasan_kmalloc(s, ret, ac->orig_size, gfp_flags);
	return ret;
}

void *_kmalloc_nolock_noprof(DECL_TOKEN_PARAMS(size, token), gfp_t gfp_flags, int node)
{
	const struct slab_alloc_context ac = {
		.caller_addr = _RET_IP_,
		.orig_size = size,
		.alloc_flags = SLAB_ALLOC_NOLOCK,
	};

	return __kmalloc_nolock_noprof(PASS_TOKEN_PARAMS(size, token),
				       gfp_flags, node, &ac);
}
EXPORT_SYMBOL_GPL(_kmalloc_nolock_noprof);

void *__kmalloc_node_track_caller_noprof(DECL_KMALLOC_PARAMS(size, b, token), gfp_t flags,
					 int node, unsigned long caller)
{
	const struct slab_alloc_context ac = {
		.caller_addr = caller,
		.orig_size = size,
		.alloc_flags = SLAB_ALLOC_DEFAULT,
	};

	return __do_kmalloc_node(PASS_BUCKET_PARAM(b), flags, node,
				 PASS_TOKEN_PARAM(token), &ac);
}
EXPORT_SYMBOL(__kmalloc_node_track_caller_noprof);

void *__kmalloc_cache_noprof(struct kmem_cache *s, gfp_t gfpflags, size_t size)
{
	void *ret;
	const struct slab_alloc_context ac = {
		.caller_addr = _RET_IP_,
		.orig_size = size,
		.alloc_flags = SLAB_ALLOC_DEFAULT,
	};

	ret = slab_alloc_node(s, gfpflags, NUMA_NO_NODE, &ac);

	trace_kmalloc(_RET_IP_, ret, size, s->size, gfpflags, NUMA_NO_NODE);

	ret = kasan_kmalloc(s, ret, size, gfpflags);
	return ret;
}
EXPORT_SYMBOL(__kmalloc_cache_noprof);

void *__kmalloc_cache_node_noprof(struct kmem_cache *s, gfp_t gfpflags,
				  int node, size_t size)
{
	void *ret;
	const struct slab_alloc_context ac = {
		.caller_addr = _RET_IP_,
		.orig_size = size,
		.alloc_flags = SLAB_ALLOC_DEFAULT,
	};

	ret = slab_alloc_node(s, gfpflags, node, &ac);

	trace_kmalloc(_RET_IP_, ret, size, s->size, gfpflags, node);

	ret = kasan_kmalloc(s, ret, size, gfpflags);
	return ret;
}
EXPORT_SYMBOL(__kmalloc_cache_node_noprof);

/*
 * The only version of kmalloc_node() that takes alloc_flags and thus can
 * determine on its own whether to handle the allocation via kmalloc_nolock() or
 * normally
 */
void *__kmalloc_flags_noprof(DECL_TOKEN_PARAMS(size, token), gfp_t flags,
			     unsigned int alloc_flags, int node)
{
	const struct slab_alloc_context ac = {
		.caller_addr = _RET_IP_,
		.orig_size = size,
		.alloc_flags = alloc_flags,
	};

	if (alloc_flags_allow_spinning(alloc_flags)) {
		return __do_kmalloc_node(NULL, flags, node,
				PASS_TOKEN_PARAM(token), &ac);
	} else {
		return __kmalloc_nolock_noprof(PASS_TOKEN_PARAMS(size, token),
					       flags, node, &ac);
	}
}


static noinline void free_to_partial_list(
	struct kmem_cache *s, struct slab *slab,
	void *head, void *tail, int bulk_cnt,
	unsigned long addr)
{
	struct kmem_cache_node *n = get_node(s, slab_nid(slab));
	struct slab *slab_free = NULL;
	int cnt = bulk_cnt;
	unsigned long flags;
	depot_stack_handle_t handle = 0;

	/*
	 * We cannot use GFP_NOWAIT as there are callsites where waking up
	 * kswapd could deadlock
	 */
	if (s->flags & SLAB_STORE_USER)
		handle = set_track_prepare(__GFP_NOWARN);

	spin_lock_irqsave(&n->list_lock, flags);

	if (free_debug_processing(s, slab, head, tail, &cnt, addr, handle)) {
		void *prior = slab->freelist;

		/* Perform the actual freeing while we still hold the locks */
		slab->inuse -= cnt;
		set_freepointer(s, tail, prior);
		slab->freelist = head;

		/*
		 * If the slab is empty, and node's partial list is full,
		 * it should be discarded anyway no matter it's on full or
		 * partial list.
		 */
		if (slab->inuse == 0 && n->nr_partial >= s->min_partial)
			slab_free = slab;

		if (!prior) {
			/* was on full list */
			remove_full(s, n, slab);
			if (!slab_free) {
				add_partial(n, slab, ADD_TO_TAIL);
				stat(s, FREE_ADD_PARTIAL);
			}
		} else if (slab_free) {
			remove_partial(n, slab);
			stat(s, FREE_REMOVE_PARTIAL);
		}
	}

	if (slab_free) {
		/*
		 * Update the counters while still holding n->list_lock to
		 * prevent spurious validation warnings
		 */
		dec_slabs_node(s, slab_nid(slab_free), slab_free->objects);
	}

	spin_unlock_irqrestore(&n->list_lock, flags);

	if (slab_free) {
		stat(s, FREE_SLAB);
		free_slab(s, slab_free);
	}
}

/*
 * Try returning (remainder of) the freelist that we just detached from the
 * slab.  Optimistically assume the slab is still full, so we don't need to find
 * the tail of the detached freelist.
 *
 * Fail if the slab isn't full anymore due to a concurrent free.
 */
static bool __slab_try_return_freelist(struct kmem_cache *s, struct slab *slab,
				       void *head, int cnt)
{
	struct freelist_counters old, new;

	old.freelist = slab->freelist;
	old.counters = slab->counters;

	if (old.freelist)
		return false;

	new.freelist = head;
	new.counters = old.counters;
	new.inuse -= cnt;

	if (!slab_update_freelist(s, slab, &old, &new, "__slab_try_return_freelist"))
		return false;

	return true;
}

/*
 * Slow path handling. This may still be called frequently since objects
 * have a longer lifetime than the cpu slabs in most processing loads.
 *
 * So we still attempt to reduce cache line usage. Just take the slab
 * lock and free the item. If there is no additional partial slab
 * handling required then we can return immediately.
 */
/*
 * __slab_free() - 释放慢速路径：将对象归还到 slab freelist 并维护 partial list
 *
 * 背景：当 free_to_pcs() 无法使用快速路径时走这里：
 *   - 对象来自远端 NUMA 节点（不应塞进本地 sheaf，否则下次分配给本地 CPU 会
 *     产生跨节点访问；且 barn 也是 per-node 的，不适合接收跨节点对象）
 *   - pfmemalloc 对象（需要维持储备平衡）
 *   - 调试 cache（需要单对象检查，不能批量）
 *
 * 设计核心：无锁 CAS 更新 freelist + 投机性预取 list_lock
 *
 * 无锁路径（最常见情况）：
 *   slab 在 partial list 上且 inuse 仍 > 0（不会变全空）时，
 *   用 cmpxchg_double 原子地：头插法更新 freelist（tail->freepointer = old.freelist，
 *   freelist = head），同时递减 inuse。全程无锁，只有 CAS 失败时重试。
 *
 * 为什么 CAS 失败后需要先解锁 list_lock 再重试？
 *   CAS 循环重试时，若 n（即将需要 list_lock）非 NULL，说明上一轮已投机加锁。
 *   CAS 失败意味着 slab 状态被并发修改，此时持有的 list_lock 基于过时状态，
 *   必须先 unlock 让其他 CPU 有机会处理，再 re-snapshot 重试。
 *
 * 投机性预取 list_lock（针对 inuse→0 或 was_full 两种需要修改 partial list 的情况）：
 *   在 CAS 之前提前 spin_lock_irqsave，是为了让 list_lock 的持有时间最短：
 *   CAS 成功后立即基于已持有的锁处理 partial list，避免先 CAS 成功再加锁
 *   的窗口期（窗口期内 slab 状态可能再次被修改）。
 *
 * CAS 成功后的三条路径：
 *   A. n == NULL（最常见）：slab 在 partial list 且未变全空，无需操作链表，直接返回。
 *   B. !was_full && !on_node_partial：slab 不在 partial list（可能被 barn 持有
 *      或从 partial list 摘下正在处理中），不做链表操作，解锁返回。
 *   C. 需要修改 partial list：
 *      - new.inuse > 0 且 was_full → 从满转部分空：add_partial(ADD_TO_TAIL)
 *      - new.inuse == 0 且 nr_partial < min_partial → 空 slab 留作备用
 *      - new.inuse == 0 且 nr_partial >= min_partial → slab_empty 路径：
 *        从 partial list 摘除，discard_slab() 归还给伙伴系统
 *
 * @head: 待释放链表的头对象（通常单对象时 head == tail）
 * @tail: 待释放链表的尾对象
 * @cnt:  链表中对象数量（批量释放时 > 1）
 */
static void __slab_free(struct kmem_cache *s, struct slab *slab,
			void *head, void *tail, int cnt,
			unsigned long addr)

{
	bool was_full;
	struct freelist_counters old, new;
	struct kmem_cache_node *n = NULL;
	unsigned long flags;
	bool on_node_partial;

	if (IS_ENABLED(CONFIG_SLUB_TINY) || kmem_cache_debug(s)) {
		free_to_partial_list(s, slab, head, tail, cnt, addr);
		return;
	}

	do {
		if (unlikely(n)) {
			spin_unlock_irqrestore(&n->list_lock, flags);
			n = NULL;
		}

		old.freelist = slab->freelist;
		old.counters = slab->counters;

		was_full = (old.freelist == NULL);

		set_freepointer(s, tail, old.freelist);

		new.freelist = head;
		new.counters = old.counters;
		new.inuse -= cnt;

		/*
		 * Might need to be taken off (due to becoming empty) or added
		 * to (due to not being full anymore) the partial list.
		 * Unless it's frozen.
		 */
		if (!new.inuse || was_full) {

			n = get_node(s, slab_nid(slab));
			/*
			 * Speculatively acquire the list_lock.
			 * If the cmpxchg does not succeed then we may
			 * drop the list_lock without any processing.
			 *
			 * Otherwise the list_lock will synchronize with
			 * other processors updating the list of slabs.
			 */
			spin_lock_irqsave(&n->list_lock, flags);

			on_node_partial = slab_test_node_partial(slab);
		}

	} while (!slab_update_freelist(s, slab, &old, &new, "__slab_free"));

	if (likely(!n)) {
		/*
		 * We didn't take the list_lock because the slab was already on
		 * the partial list and will remain there.
		 */
		return;
	}

	/*
	 * This slab was partially empty but not on the per-node partial list,
	 * in which case we shouldn't manipulate its list, just return.
	 */
	if (!was_full && !on_node_partial) {
		spin_unlock_irqrestore(&n->list_lock, flags);
		return;
	}

	/*
	 * If slab became empty, should we add/keep it on the partial list or we
	 * have enough?
	 */
	if (unlikely(!new.inuse && n->nr_partial >= s->min_partial))
		goto slab_empty;

	/*
	 * Objects left in the slab. If it was not on the partial list before
	 * then add it.
	 */
	if (unlikely(was_full)) {
		add_partial(n, slab, ADD_TO_TAIL);
		stat(s, FREE_ADD_PARTIAL);
	}
	spin_unlock_irqrestore(&n->list_lock, flags);
	return;

slab_empty:
	/*
	 * The slab could have a single object and thus go from full to empty in
	 * a single free, but more likely it was on the partial list. Remove it.
	 */
	if (likely(!was_full)) {
		remove_partial(n, slab);
		stat(s, FREE_REMOVE_PARTIAL);
	}

	spin_unlock_irqrestore(&n->list_lock, flags);
	stat(s, FREE_SLAB);
	discard_slab(s, slab);
}

/*
 * __pcs_install_empty_sheaf() - 将新取得的空 sheaf 安装到 pcs
 *
 * 背景：在 __pcs_replace_full_main 解锁期间获取了空 sheaf，但重新持锁后
 * pcs 状态可能已被改变（CPU 迁移、其他 CPU 操作）。此函数负责将空 sheaf
 * 安装到最合理的位置，并妥善处理各种并发情况。
 *
 * 预期情况（最常见）：spare == NULL
 *   → main 降为 spare，empty 升为 main（满足原始目的）
 *
 * 并发情况 1：main 已有空间（size < capacity）
 *   → 说明中间有其他操作腾出了空间，不需要新 empty，归还 barn
 *
 * 并发情况 2：main 满但 spare 有空间
 *   → swap(main, spare)，归还 empty 到 barn（spare 能接受对象就够了）
 *
 * 并发情况 3：main 和 spare 都满（从 barn 没有空 sheaf、手动分配了 empty）
 *   → 把满的 main 存入 barn（完成 barn_replace_full_sheaf 的未竟之功），
 *     将 empty 设为新 main
 *
 * 投入 barn 时不检查 barn 上限（注释中说明：此处是竞争恢复路径，概率极低，
 * 简化处理优先于严格限制）。
 */
static void __pcs_install_empty_sheaf(struct kmem_cache *s,
		struct slub_percpu_sheaves *pcs, struct slab_sheaf *empty,
		struct node_barn *barn)
{
	lockdep_assert_held(this_cpu_ptr(&s->cpu_sheaves->lock));

	/* This is what we expect to find if nobody interrupted us. */
	if (likely(!pcs->spare)) {
		pcs->spare = pcs->main;
		pcs->main = empty;
		return;
	}

	/*
	 * Unlikely because if the main sheaf had space, we would have just
	 * freed to it. Get rid of our empty sheaf.
	 */
	if (pcs->main->size < s->sheaf_capacity) {
		barn_put_empty_sheaf(barn, empty);
		return;
	}

	/* Also unlikely for the same reason */
	if (pcs->spare->size < s->sheaf_capacity) {
		swap(pcs->main, pcs->spare);
		barn_put_empty_sheaf(barn, empty);
		return;
	}

	/*
	 * We probably failed barn_replace_full_sheaf() due to no empty sheaf
	 * available there, but we allocated one, so finish the job.
	 */
	barn_put_full_sheaf(barn, pcs->main);
	stat(s, BARN_PUT);
	pcs->main = empty;
}

/*
 * __pcs_replace_full_main() - 用空/半空 sheaf 替换已满的 main sheaf（释放路径）
 *
 * 对应分配路径的 __pcs_replace_empty_main，但方向相反：
 * 分配 = 需要有对象的 sheaf；释放 = 需要有空间的 sheaf。
 *
 * 调用约束：已持 cpu_sheaves->lock。成功时锁仍持有；失败返回 NULL 时锁释放。
 *
 * 分级策略（从快到慢，对称于分配路径）：
 *
 * 1. spare 为 NULL → 从 barn 取一个空 sheaf：
 *    spare = main（把满 main 存为 spare 备用），main = empty（接收新释放的对象）
 *
 * 2. spare->size < capacity（spare 有空间）：
 *    swap(main, spare)，让 spare（未满）成为新 main，原满 main 变 spare
 *
 * 3. barn_replace_full_sheaf() 成功：
 *    把满 main 存入 barn，换出空 sheaf 作为新 main
 *
 * 4. barn 满了（-E2BIG）：
 *    spare 存在且满，把 spare 的对象全部刷出（sheaf_flush_unused），
 *    腾出一个空 sheaf 用作新 main
 *
 * 5. barn 没有空 sheaf（-ENOMEM）：
 *    alloc_empty_sheaf() 新分配一个空 sheaf
 *    若新分配也失败：sheaf_try_flush_main（直接刷 main 而不换），
 *    刷完后 main 有空间了，重新 trylock 检查
 *
 * 每次升级到更慢的策略前都先解锁（alloc 可能休眠），解锁后重新 trylock，
 * 并用 __pcs_install_empty_sheaf 处理并发恢复。
 *
 * allow_spin=false：NMI/原子上下文，跳过所有可能休眠的路径（直接返回 NULL），
 *   让调用方直接调用 __slab_free() 绕过 pcs。
 */
static struct slub_percpu_sheaves *
__pcs_replace_full_main(struct kmem_cache *s, struct slub_percpu_sheaves *pcs,
			bool allow_spin)
{
	struct slab_sheaf *empty;
	struct node_barn *barn;
	bool put_fail;

restart:
	lockdep_assert_held(this_cpu_ptr(&s->cpu_sheaves->lock));

	/* Bootstrap or debug cache, back off */
	if (unlikely(!cache_has_sheaves(s))) {
		local_unlock(&s->cpu_sheaves->lock);
		return NULL;
	}

	barn = get_barn(s);
	if (!barn) {
		local_unlock(&s->cpu_sheaves->lock);
		return NULL;
	}

	put_fail = false;

	if (!pcs->spare) {
		empty = barn_get_empty_sheaf(barn, allow_spin);
		if (empty) {
			pcs->spare = pcs->main;
			pcs->main = empty;
			return pcs;
		}
		goto alloc_empty;
	}

	if (pcs->spare->size < s->sheaf_capacity) {
		swap(pcs->main, pcs->spare);
		return pcs;
	}

	empty = barn_replace_full_sheaf(barn, pcs->main, allow_spin);

	if (!IS_ERR(empty)) {
		stat(s, BARN_PUT);
		pcs->main = empty;
		return pcs;
	}

	/* sheaf_flush_unused() doesn't support !allow_spin */
	if (PTR_ERR(empty) == -E2BIG && allow_spin) {
		/* Since we got here, spare exists and is full */
		struct slab_sheaf *to_flush = pcs->spare;

		stat(s, BARN_PUT_FAIL);

		pcs->spare = NULL;
		local_unlock(&s->cpu_sheaves->lock);

		sheaf_flush_unused(s, to_flush);
		empty = to_flush;
		goto got_empty;
	}

	/*
	 * We could not replace full sheaf because barn had no empty
	 * sheaves. We can still allocate it and put the full sheaf in
	 * __pcs_install_empty_sheaf(), but if we fail to allocate it,
	 * make sure to count the fail.
	 */
	put_fail = true;

alloc_empty:
	local_unlock(&s->cpu_sheaves->lock);

	/*
	 * alloc_empty_sheaf() doesn't support !allow_spin and it's
	 * easier to fall back to freeing directly without sheaves
	 * than add the support (and to sheaf_flush_unused() above)
	 */
	if (!allow_spin)
		return NULL;

	empty = alloc_empty_sheaf(s, GFP_NOWAIT, SLAB_ALLOC_DEFAULT);
	if (empty)
		goto got_empty;

	if (put_fail)
		 stat(s, BARN_PUT_FAIL);

	if (!sheaf_try_flush_main(s))
		return NULL;

	if (!local_trylock(&s->cpu_sheaves->lock))
		return NULL;

	pcs = this_cpu_ptr(s->cpu_sheaves);

	/*
	 * we flushed the main sheaf so it should be empty now,
	 * but in case we got preempted or migrated, we need to
	 * check again
	 */
	if (pcs->main->size == s->sheaf_capacity)
		goto restart;

	return pcs;

got_empty:
	if (!local_trylock(&s->cpu_sheaves->lock)) {
		barn_put_empty_sheaf(barn, empty);
		return NULL;
	}

	pcs = this_cpu_ptr(s->cpu_sheaves);
	__pcs_install_empty_sheaf(s, pcs, empty, barn);

	return pcs;
}

/*
 * free_to_pcs() - 释放快速路径：将对象推入 per-CPU sheaf
 *
 * 这是释放路径中最快的部分，与 alloc_from_pcs() 对称。
 * 调用前提：对象已通过 slab_free_hook()（KASAN 标记不可访问、poison 填充等）。
 *
 * 核心操作（理想情况）：
 *   local_trylock → main->objects[size++] = object → local_unlock
 *
 * 若 main 已满（size == sheaf_capacity），调用 __pcs_replace_full_main
 * 换入一个有空间的 sheaf（与分配路径的 __pcs_replace_empty_main 对称）。
 *
 * 返回 false 的情况（调用方降级到 __slab_free）：
 *   - local_trylock 失败（锁竞争）
 *   - __pcs_replace_full_main 失败（barn 和分配都失败）
 *
 * allow_spin 参数传递给 __pcs_replace_full_main，在 NMI 上下文中
 * 禁止等待自旋锁，快速失败降级。
 */
static __fastpath_inline
bool free_to_pcs(struct kmem_cache *s, void *object, bool allow_spin)
{
	struct slub_percpu_sheaves *pcs;

	if (!local_trylock(&s->cpu_sheaves->lock))
		return false;

	pcs = this_cpu_ptr(s->cpu_sheaves);

	if (unlikely(pcs->main->size == s->sheaf_capacity)) {

		pcs = __pcs_replace_full_main(s, pcs, allow_spin);
		if (unlikely(!pcs))
			return false;
	}

	pcs->main->objects[pcs->main->size++] = object;

	local_unlock(&s->cpu_sheaves->lock);

	stat(s, FREE_FASTPATH);

	return true;
}

static void rcu_free_sheaf(struct rcu_head *head)
{
	struct slab_sheaf *sheaf;
	struct node_barn *barn = NULL;
	struct kmem_cache *s;

	sheaf = container_of(head, struct slab_sheaf, rcu_head);

	s = sheaf->cache;

	/*
	 * This may remove some objects due to slab_free_hook() returning false,
	 * so that the sheaf might no longer be completely full. But it's easier
	 * to handle it as full (unless it became completely empty), as the code
	 * handles it fine. The only downside is that sheaf will serve fewer
	 * allocations when reused. It only happens due to debugging, which is a
	 * performance hit anyway.
	 *
	 * If it returns true, there was at least one object from pfmemalloc
	 * slab so simply flush everything.
	 */
	if (__rcu_free_sheaf_prepare(s, sheaf))
		goto flush;

	barn = get_barn_node(s, sheaf->node);
	if (!barn)
		goto flush;

	/* due to slab_free_hook() */
	if (unlikely(sheaf->size == 0))
		goto empty;

	/*
	 * Checking nr_full/nr_empty outside lock avoids contention in case the
	 * barn is at the respective limit. Due to the race we might go over the
	 * limit but that should be rare and harmless.
	 */

	if (data_race(barn->nr_full) < MAX_FULL_SHEAVES) {
		stat(s, BARN_PUT);
		barn_put_full_sheaf(barn, sheaf);
		return;
	}

flush:
	stat(s, BARN_PUT_FAIL);
	sheaf_flush_unused(s, sheaf);

empty:
	if (barn && data_race(barn->nr_empty) < MAX_EMPTY_SHEAVES) {
		barn_put_empty_sheaf(barn, sheaf);
		return;
	}

	free_empty_sheaf(s, sheaf);
}

/*
 * kvfree_call_rcu() can be called while holding a raw_spinlock_t. Since
 * __kfree_rcu_sheaf() may acquire a spinlock_t (sleeping lock on PREEMPT_RT),
 * this would violate lock nesting rules. Therefore, kvfree_call_rcu() avoids
 * this problem by bypassing the sheaves layer entirely on PREEMPT_RT.
 *
 * However, lockdep still complains that it is invalid to acquire spinlock_t
 * while holding raw_spinlock_t, even on !PREEMPT_RT where spinlock_t is a
 * spinning lock. Tell lockdep that acquiring spinlock_t is valid here
 * by temporarily raising the wait-type to LD_WAIT_CONFIG.
 */
static DEFINE_WAIT_OVERRIDE_MAP(kfree_rcu_sheaf_map, LD_WAIT_CONFIG);

bool __kfree_rcu_sheaf(struct kmem_cache *s, void *obj)
{
	struct slub_percpu_sheaves *pcs;
	struct slab_sheaf *rcu_sheaf;

	if (WARN_ON_ONCE(IS_ENABLED(CONFIG_PREEMPT_RT)))
		return false;

	lock_map_acquire_try(&kfree_rcu_sheaf_map);

	if (!local_trylock(&s->cpu_sheaves->lock))
		goto fail;

	pcs = this_cpu_ptr(s->cpu_sheaves);

	if (unlikely(!pcs->rcu_free)) {

		struct slab_sheaf *empty;
		struct node_barn *barn;

		/* Bootstrap or debug cache, fall back */
		if (unlikely(!cache_has_sheaves(s))) {
			local_unlock(&s->cpu_sheaves->lock);
			goto fail;
		}

		if (pcs->spare && pcs->spare->size == 0) {
			pcs->rcu_free = pcs->spare;
			pcs->spare = NULL;
			goto do_free;
		}

		barn = get_barn(s);
		if (!barn) {
			local_unlock(&s->cpu_sheaves->lock);
			goto fail;
		}

		empty = barn_get_empty_sheaf(barn, true);

		if (empty) {
			pcs->rcu_free = empty;
			goto do_free;
		}

		local_unlock(&s->cpu_sheaves->lock);

		empty = alloc_empty_sheaf(s, GFP_NOWAIT, SLAB_ALLOC_DEFAULT);

		if (!empty)
			goto fail;

		if (!local_trylock(&s->cpu_sheaves->lock)) {
			barn_put_empty_sheaf(barn, empty);
			goto fail;
		}

		pcs = this_cpu_ptr(s->cpu_sheaves);

		if (unlikely(pcs->rcu_free))
			barn_put_empty_sheaf(barn, empty);
		else
			pcs->rcu_free = empty;
	}

do_free:

	rcu_sheaf = pcs->rcu_free;

	/*
	 * Since we flush immediately when size reaches capacity, we never reach
	 * this with size already at capacity, so no OOB write is possible.
	 */
	rcu_sheaf->objects[rcu_sheaf->size++] = obj;

	if (likely(rcu_sheaf->size < s->sheaf_capacity)) {
		rcu_sheaf = NULL;
	} else {
		pcs->rcu_free = NULL;
		rcu_sheaf->node = numa_node_id();
	}

	/*
	 * we flush before local_unlock to make sure a racing
	 * flush_all_rcu_sheaves() doesn't miss this sheaf
	 */
	if (rcu_sheaf)
		call_rcu(&rcu_sheaf->rcu_head, rcu_free_sheaf);

	local_unlock(&s->cpu_sheaves->lock);

	stat(s, FREE_RCU_SHEAF);
	lock_map_release(&kfree_rcu_sheaf_map);
	return true;

fail:
	stat(s, FREE_RCU_SHEAF_FAIL);
	lock_map_release(&kfree_rcu_sheaf_map);
	return false;
}

/*
 * can_free_to_pcs() - 决定对象是否可以放入 per-CPU sheaf（而非走慢速路径）
 *
 * 背景：per-CPU sheaf 是 NUMA 感知的——sheaf 里的对象应当来自本 CPU 最近的
 * 内存节点，这样下次从 sheaf 分配时能保持 NUMA 局部性。
 * 如果把远端节点的对象缓存到 sheaf，将来分配时用这个对象反而会造成
 * 跨节点访问，抵消 sheaf 的性能优势。
 *
 * 两种 NUMA 处理路径：
 *
 * CONFIG_HAVE_MEMORYLESS_NODES（有些 CPU 节点没有本地内存，典型如 POWER 架构）：
 *   用 numa_mem_id() 指向"最近的有内存节点"，精确判断节点匹配。
 *
 * 无 MEMORYLESS_NODES 支持（普通 SMP 系统）：
 *   用 numa_node_id()（当前 CPU 所属节点）做主判断。
 *   特殊情况：若当前节点是 memoryless（只有 ZONE_MOVABLE，slab 不能从中分配），
 *   则接受来自任意节点的对象（无论如何都没有本地对象，不必拒绝远端对象）。
 *
 * pfmemalloc 检查：
 *   来自紧急储备（SL_pfmemalloc）的对象不能进 sheaf——sheaf 里的对象会被
 *   任意普通分配使用，但 pfmemalloc 对象只能给 __GFP_MEMALLOC 请求使用。
 *   若进 sheaf，普通分配拿到 pfmemalloc 对象后会破坏储备平衡。
 */
static __always_inline bool can_free_to_pcs(struct slab *slab)
{
	int slab_node;
	int numa_node;

	if (!IS_ENABLED(CONFIG_NUMA))
		goto check_pfmemalloc;

	slab_node = slab_nid(slab);

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
	/*
	 * numa_mem_id() points to the closest node with memory so only allow
	 * objects from that node to the percpu sheaves
	 */
	numa_node = numa_mem_id();

	if (likely(slab_node == numa_node))
		goto check_pfmemalloc;
#else

	/*
	 * numa_mem_id() is only a wrapper to numa_node_id() which is where this
	 * cpu belongs to, but it might be a memoryless node anyway. We don't
	 * know what the closest node is.
	 */
	numa_node = numa_node_id();

	/* freed object is from this cpu's node, proceed */
	if (likely(slab_node == numa_node))
		goto check_pfmemalloc;

	/*
	 * Freed object isn't from this cpu's node, but that node is memoryless
	 * or only has ZONE_MOVABLE memory, which slab cannot allocate from.
	 * Proceed as it's better to cache remote objects than falling back to
	 * the slowpath for everything. The allocation side can never obtain
	 * a local object anyway, if none exist. We don't have numa_mem_id() to
	 * point to the closest node as we would on a proper memoryless node
	 * setup.
	 */
	if (unlikely(!node_state(numa_node, N_NORMAL_MEMORY)))
		goto check_pfmemalloc;
#endif

	return false;

check_pfmemalloc:
	return likely(!slab_test_pfmemalloc(slab));
}

/*
 * Bulk free objects to the percpu sheaves.
 * Unlike free_to_pcs() this includes the calls to all necessary hooks
 * and the fallback to freeing to slab pages.
 */
static void free_to_pcs_bulk(struct kmem_cache *s, size_t size, void **p)
{
	struct slub_percpu_sheaves *pcs;
	struct slab_sheaf *main, *empty;
	bool init = slab_want_init_on_free(s);
	unsigned int batch, i = 0;
	struct node_barn *barn;
	void *remote_objects[PCS_BATCH_MAX];
	unsigned int remote_nr = 0;

next_remote_batch:
	while (i < size) {
		struct slab *slab = virt_to_slab(p[i]);

		memcg_slab_free_hook(s, slab, p + i, 1);
		alloc_tagging_slab_free_hook(s, slab, p + i, 1);

		if (unlikely(!slab_free_hook(s, p[i], init, false))) {
			p[i] = p[--size];
			continue;
		}

		if (unlikely(!can_free_to_pcs(slab))) {
			remote_objects[remote_nr] = p[i];
			p[i] = p[--size];
			if (++remote_nr >= PCS_BATCH_MAX)
				goto flush_remote;
			continue;
		}

		i++;
	}

	if (!size)
		goto flush_remote;

next_batch:
	if (!local_trylock(&s->cpu_sheaves->lock))
		goto fallback;

	pcs = this_cpu_ptr(s->cpu_sheaves);

	if (likely(pcs->main->size < s->sheaf_capacity))
		goto do_free;

	barn = get_barn(s);
	if (!barn)
		goto no_empty;

	if (!pcs->spare) {
		empty = barn_get_empty_sheaf(barn, true);
		if (!empty)
			goto no_empty;

		pcs->spare = pcs->main;
		pcs->main = empty;
		goto do_free;
	}

	if (pcs->spare->size < s->sheaf_capacity) {
		swap(pcs->main, pcs->spare);
		goto do_free;
	}

	empty = barn_replace_full_sheaf(barn, pcs->main, true);
	if (IS_ERR(empty)) {
		stat(s, BARN_PUT_FAIL);
		goto no_empty;
	}

	stat(s, BARN_PUT);
	pcs->main = empty;

do_free:
	main = pcs->main;
	batch = min(size, s->sheaf_capacity - main->size);

	memcpy(main->objects + main->size, p, batch * sizeof(void *));
	main->size += batch;

	local_unlock(&s->cpu_sheaves->lock);

	stat_add(s, FREE_FASTPATH, batch);

	if (batch < size) {
		p += batch;
		size -= batch;
		goto next_batch;
	}

	if (remote_nr)
		goto flush_remote;

	return;

no_empty:
	local_unlock(&s->cpu_sheaves->lock);

	/*
	 * if we depleted all empty sheaves in the barn or there are too
	 * many full sheaves, free the rest to slab pages
	 */
fallback:
	__kmem_cache_free_bulk(s, size, p);
	stat_add(s, FREE_SLOWPATH, size);

flush_remote:
	if (remote_nr) {
		__kmem_cache_free_bulk(s, remote_nr, &remote_objects[0]);
		stat_add(s, FREE_SLOWPATH, remote_nr);
		if (i < size) {
			remote_nr = 0;
			goto next_remote_batch;
		}
	}
}

struct defer_free {
	struct llist_head objects;
	struct irq_work work;
};

static void free_deferred_objects(struct irq_work *work);

static DEFINE_PER_CPU(struct defer_free, defer_free_objects) = {
	.objects = LLIST_HEAD_INIT(objects),
	.work = IRQ_WORK_INIT(free_deferred_objects),
};

/*
 * In PREEMPT_RT irq_work runs in per-cpu kthread, so it's safe
 * to take sleeping spin_locks from __slab_free().
 * In !PREEMPT_RT irq_work will run after local_unlock_irqrestore().
 */
static void free_deferred_objects(struct irq_work *work)
{
	struct defer_free *df = container_of(work, struct defer_free, work);
	struct llist_head *objs = &df->objects;
	struct llist_node *llnode, *pos, *t;

	if (llist_empty(objs))
		return;

	llnode = llist_del_all(objs);
	llist_for_each_safe(pos, t, llnode) {
		struct kmem_cache *s;
		struct slab *slab;
		void *x = pos;

		slab = virt_to_slab(x);
		s = slab->slab_cache;

		/* Point 'x' back to the beginning of allocated object */
		x -= s->offset;

		/*
		 * We used freepointer in 'x' to link 'x' into df->objects.
		 * Clear it to NULL to avoid false positive detection
		 * of "Freepointer corruption".
		 */
		set_freepointer(s, x, NULL);

		__slab_free(s, slab, x, x, 1, _THIS_IP_);
		stat(s, FREE_SLOWPATH);
	}
}

static void defer_free(struct kmem_cache *s, void *head)
{
	struct defer_free *df;

	guard(preempt)();

	head = kasan_reset_tag(head);

	df = this_cpu_ptr(&defer_free_objects);
	if (llist_add(head + s->offset, &df->objects))
		irq_work_queue(&df->work);
}

void defer_free_barrier(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		irq_work_sync(&per_cpu_ptr(&defer_free_objects, cpu)->work);
}

static __fastpath_inline
void slab_free(struct kmem_cache *s, struct slab *slab, void *object,
	       unsigned long addr)
{
	memcg_slab_free_hook(s, slab, &object, 1);
	alloc_tagging_slab_free_hook(s, slab, &object, 1);

	if (unlikely(!slab_free_hook(s, object, slab_want_init_on_free(s), false)))
		return;

	if (likely(can_free_to_pcs(slab)) && likely(free_to_pcs(s, object, true)))
		return;

	__slab_free(s, slab, object, object, 1, addr);
	stat(s, FREE_SLOWPATH);
}

#ifdef CONFIG_MEMCG
/* Do not inline the rare memcg charging failed path into the allocation path */
static noinline
void memcg_alloc_abort_single(struct kmem_cache *s, void *object)
{
	struct slab *slab = virt_to_slab(object);

	alloc_tagging_slab_free_hook(s, slab, &object, 1);

	if (likely(slab_free_hook(s, object, slab_want_init_on_free(s), false)))
		__slab_free(s, slab, object, object, 1, _RET_IP_);
}
#endif

static __fastpath_inline
void slab_free_bulk(struct kmem_cache *s, struct slab *slab, void *head,
		    void *tail, void **p, int cnt, unsigned long addr)
{
	memcg_slab_free_hook(s, slab, p, cnt);
	alloc_tagging_slab_free_hook(s, slab, p, cnt);
	/*
	 * With KASAN enabled slab_free_freelist_hook modifies the freelist
	 * to remove objects, whose reuse must be delayed.
	 */
	if (likely(slab_free_freelist_hook(s, &head, &tail, &cnt))) {
		__slab_free(s, slab, head, tail, cnt, addr);
		stat_add(s, FREE_SLOWPATH, cnt);
	}
}

#ifdef CONFIG_SLUB_RCU_DEBUG
static void slab_free_after_rcu_debug(struct rcu_head *rcu_head)
{
	struct rcu_delayed_free *delayed_free =
			container_of(rcu_head, struct rcu_delayed_free, head);
	void *object = delayed_free->object;
	struct slab *slab = virt_to_slab(object);
	struct kmem_cache *s;

	kfree(delayed_free);

	if (WARN_ON(is_kfence_address(object)))
		return;

	/* find the object and the cache again */
	if (WARN_ON(!slab))
		return;
	s = slab->slab_cache;
	if (WARN_ON(!(s->flags & SLAB_TYPESAFE_BY_RCU)))
		return;

	/* resume freeing */
	if (slab_free_hook(s, object, slab_want_init_on_free(s), true)) {
		__slab_free(s, slab, object, object, 1, _THIS_IP_);
		stat(s, FREE_SLOWPATH);
	}
}
#endif /* CONFIG_SLUB_RCU_DEBUG */

#ifdef CONFIG_KASAN_GENERIC
void ___cache_free(struct kmem_cache *cache, void *x, unsigned long addr)
{
	__slab_free(cache, virt_to_slab(x), x, x, 1, addr);
	stat(cache, FREE_SLOWPATH);
}
#endif

/*
 * warn_free_bad_obj() - 报告"将对象释放到错误 cache"的警告
 *
 * 两种常见情况（都是 bug）：
 *   1. slab == NULL：对象不在任何 slab 页里（可能是栈/全局变量/已释放内存）
 *   2. slab->slab_cache != s：对象来自另一个 cache（常见于 kmalloc 对象误用
 *      kmem_cache_free，或 use-after-free 后 slab_cache 字段被破坏）
 *
 * 发现问题后"故意泄漏"（不调用实际释放），原因：
 *   若强行释放到错误的 freelist，会损坏另一个 cache 的数据结构，
 *   后果比泄漏严重得多。泄漏只是丢失内存，不会引发立即崩溃或安全问题。
 */
static noinline void warn_free_bad_obj(struct kmem_cache *s, void *obj)
{
	struct kmem_cache *cachep;
	struct slab *slab;

	slab = virt_to_slab(obj);
	if (WARN_ONCE(!slab,
			"kmem_cache_free(%s, %p): object is not in a slab page\n",
			s->name, obj))
		return;

	cachep = slab->slab_cache;

	if (WARN_ONCE(cachep != s,
			"kmem_cache_free(%s, %p): object belongs to different cache %s\n",
			s->name, obj, cachep ? cachep->name : "(NULL)")) {
		if (cachep)
			print_tracking(cachep, obj);
		return;
	}
}

/**
 * kmem_cache_free - Deallocate an object
 * @s: The cache the allocation was from.
 * @x: The previously allocated object.
 *
 * 公开的释放接口，对应 kmem_cache_alloc()。
 *
 * 安全检查（SLAB_FREELIST_HARDENED 或 SLAB_CONSISTENCY_CHECKS 开启时）：
 *   验证对象确实属于 cache s，防止以下两种攻击/bug：
 *   - 攻击者伪造对象地址并调用 kmem_cache_free，污染无关 cache 的 freelist
 *   - 代码 bug 将 kmalloc 对象误用 kmem_cache_free 释放
 *   检查失败时 warn_free_bad_obj() 打印警告并故意泄漏（不实际释放）。
 *
 * trace_kmem_cache_free：触发 tracepoint，供 perf/ftrace 追踪分配行为。
 */
void kmem_cache_free(struct kmem_cache *s, void *x)
{
	struct slab *slab;

	slab = virt_to_slab(x);

	if (IS_ENABLED(CONFIG_SLAB_FREELIST_HARDENED) ||
	    kmem_cache_debug_flags(s, SLAB_CONSISTENCY_CHECKS)) {

		/*
		 * Intentionally leak the object in these cases, because it
		 * would be too dangerous to continue.
		 */
		if (unlikely(!slab || (slab->slab_cache != s))) {
			warn_free_bad_obj(s, x);
			return;
		}
	}

	trace_kmem_cache_free(_RET_IP_, x, s);
	slab_free(s, slab, x, _RET_IP_);
}
EXPORT_SYMBOL(kmem_cache_free);

/*
 * slab_ksize() - 返回对象实际可用的字节数（用于 krealloc 判断是否需要重分配）
 *
 * 问题背景：kmalloc(200) 实际分配 256 字节的槽位，多出的 56 字节理论上可以用。
 * krealloc(p, 210) 时如果旧对象就是 256 字节槽位，可以直接返回原指针，
 * 无需重新分配。slab_ksize 返回的就是"最大可安全使用的字节数"。
 *
 * 但"可安全使用"受多个约束限制：
 *
 * 1. 调试模式（RED_ZONE/POISON）：不能超出 object_size，
 *    因为 [object_size, s->size) 区间可能有 red zone 或 poison 标记，
 *    越界访问会触发调试检测。
 *
 * 2. KASAN：内部用 object_size 做 shadow 边界，超出则触发 KASAN 报告。
 *
 * 3. SLAB_TYPESAFE_BY_RCU / SLAB_STORE_USER：
 *    [inuse, s->size) 区间存放 freepointer 或调试 track info，
 *    不能被用户数据覆盖，因此最多只能用到 inuse。
 *
 * 4. obj_exts_in_object：obj_exts 存在对象尾部，同样限制在 inuse 以内。
 *
 * 5. 无上述约束时：可以使用到 s->size（含对齐 padding），最大化空间利用率。
 */
static inline size_t slab_ksize(struct slab *slab)
{
	struct kmem_cache *s = slab->slab_cache;

#ifdef CONFIG_SLUB_DEBUG
	/*
	 * Debugging requires use of the padding between object
	 * and whatever may come after it.
	 */
	if (s->flags & (SLAB_RED_ZONE | SLAB_POISON))
		return s->object_size;
#endif
	if (s->flags & SLAB_KASAN)
		return s->object_size;
	/*
	 * If we have the need to store the freelist pointer
	 * or any other metadata back there then we can
	 * only use the space before that information.
	 */
	if (s->flags & (SLAB_TYPESAFE_BY_RCU | SLAB_STORE_USER))
		return s->inuse;
	else if (obj_exts_in_object(s, slab))
		return s->inuse;
	/*
	 * Else we can use all the padding etc for the allocation
	 */
	return s->size;
}

/*
 * __ksize() - 返回指针所指分配的实际字节大小（内部使用）
 *
 * 注意：这不是"用户可以安全使用的大小"的官方接口。
 * 若想查询可用大小，应使用 kmalloc_size_roundup()（分配前）。
 * 分配后调用 ksize() 查询再使用额外空间会触发 KASAN/UBSAN/FORTIFY 检查。
 *
 * ZERO_SIZE_PTR：kmalloc(0) 返回的特殊非 NULL 指针，ksize 返回 0。
 * PageLargeKmalloc：超大对象（> KMALLOC_MAX_CACHE_SIZE）用伙伴系统整页分配，
 *   通过 large_kmalloc_size() 返回实际页大小。
 */
static size_t __ksize(const void *object)
{
	struct page *page;
	struct slab *slab;

	if (unlikely(object == ZERO_SIZE_PTR))
		return 0;

	page = virt_to_page(object);

	if (unlikely(PageLargeKmalloc(page)))
		return large_kmalloc_size(page);

	slab = page_slab(page);
	/* Delete this after we're sure there are no users */
	if (WARN_ON(!slab))
		return page_size(page);

#ifdef CONFIG_SLUB_DEBUG
	skip_orig_size_check(slab->slab_cache, object);
#endif

	return slab_ksize(slab);
}

/**
 * ksize -- Report full size of underlying allocation
 * @objp: pointer to the object
 *
 * This should only be used internally to query the true size of allocations.
 * It is not meant to be a way to discover the usable size of an allocation
 * after the fact. Instead, use kmalloc_size_roundup(). Using memory beyond
 * the originally requested allocation size may trigger KASAN, UBSAN_BOUNDS,
 * and/or FORTIFY_SOURCE.
 *
 * Return: size of the actual memory used by @objp in bytes
 */
size_t ksize(const void *objp)
{
	/*
	 * We need to first check that the pointer to the object is valid.
	 * The KASAN report printed from ksize() is more useful, then when
	 * it's printed later when the behaviour could be undefined due to
	 * a potential use-after-free or double-free.
	 *
	 * We use kasan_check_byte(), which is supported for the hardware
	 * tag-based KASAN mode, unlike kasan_check_read/write().
	 *
	 * If the pointed to memory is invalid, we return 0 to avoid users of
	 * ksize() writing to and potentially corrupting the memory region.
	 *
	 * We want to perform the check before __ksize(), to avoid potentially
	 * crashing in __ksize() due to accessing invalid metadata.
	 */
	if (unlikely(ZERO_OR_NULL_PTR(objp)) || !kasan_check_byte(objp))
		return 0;

	return kfence_ksize(objp) ?: __ksize(objp);
}
EXPORT_SYMBOL(ksize);

/*
 * free_large_kmalloc() - 释放超大 kmalloc 对象（直接用伙伴系统分配的整页）
 *
 * 背景：当请求大小超过 KMALLOC_MAX_CACHE_SIZE（通常 8KB），kmalloc 不用
 * slab cache，而是直接调用 __kmalloc_large_noprof() 分配整页（2^order 页），
 * 并设置 PageLargeKmalloc 标记加以区分。
 * 此函数对应释放这类大对象，流程：
 *   1. kmemleak_free / kasan_kfree_large / kmsan_kfree_large：清理各检测工具的记录
 *   2. mod_lruvec_page_state：更新 vmstat 中 NR_SLAB_UNRECLAIMABLE_B 计数
 *   3. __ClearPageLargeKmalloc：清除大对象标记
 *   4. free_frozen_pages：归还给伙伴系统
 */
static void free_large_kmalloc(struct page *page, void *object)
{
	unsigned int order = compound_order(page);

	if (WARN_ON_ONCE(!PageLargeKmalloc(page))) {
		dump_page(page, "Not a kmalloc allocation");
		return;
	}

	if (WARN_ON_ONCE(order == 0))
		pr_warn_once("object pointer: 0x%p\n", object);

	kmemleak_free(object);
	kasan_kfree_large(object);
	kmsan_kfree_large(object);

	mod_lruvec_page_state(page, NR_SLAB_UNRECLAIMABLE_B,
			      -(PAGE_SIZE << order));
	__ClearPageLargeKmalloc(page);
	free_frozen_pages(page, order);
}

/*
 * Given an rcu_head embedded within an object obtained from kvmalloc at an
 * offset < 4k, free the object in question.
 */
void kvfree_rcu_cb(struct rcu_head *head)
{
	void *obj = head;
	struct page *page;
	struct slab *slab;
	struct kmem_cache *s;
	void *slab_addr;

	if (is_vmalloc_addr(obj)) {
		obj = (void *) PAGE_ALIGN_DOWN((unsigned long)obj);
		vfree(obj);
		return;
	}

	page = virt_to_page(obj);
	slab = page_slab(page);
	if (!slab) {
		/*
		 * rcu_head offset can be only less than page size so no need to
		 * consider allocation order
		 */
		obj = (void *) PAGE_ALIGN_DOWN((unsigned long)obj);
		free_large_kmalloc(page, obj);
		return;
	}

	s = slab->slab_cache;
	slab_addr = slab_address(slab);

	if (is_kfence_address(obj)) {
		obj = kfence_object_start(obj);
	} else {
		unsigned int idx = __obj_to_index(s, slab_addr, obj);

		obj = slab_addr + s->size * idx;
		obj = fixup_red_left(s, obj);
	}

	slab_free(s, slab, obj, _RET_IP_);
}

/**
 * kfree - free previously allocated memory
 * @object: pointer returned by kmalloc(), kmalloc_nolock(), or kmem_cache_alloc()
 *
 * kfree 是通用释放接口，能自动判断对象来自哪个 kmalloc cache 或大对象页：
 *
 * 路由逻辑：
 *   - ZERO_OR_NULL_PTR（NULL 或 ZERO_SIZE_PTR）：直接返回，不做任何操作
 *   - page_slab() == NULL（PageLargeKmalloc）：超大对象，走 free_large_kmalloc
 *   - 否则：从 slab->slab_cache 找到对应 cache，走 slab_free
 *
 * 与 kmem_cache_free 的区别：
 *   kfree 无需调用方知道 cache 名字，自动从对象所在 page 推断；
 *   kmem_cache_free 需要显式传入 cache，稍快（少一次 page→slab→cache 查找），
 *   用于已知 cache 类型的场景。
 *
 * If @object is NULL, no operation is performed.
 */
void kfree(const void *object)
{
	struct page *page;
	struct slab *slab;
	struct kmem_cache *s;
	void *x = (void *)object;

	trace_kfree(_RET_IP_, object);

	if (unlikely(ZERO_OR_NULL_PTR(object)))
		return;

	page = virt_to_page(object);
	slab = page_slab(page);
	if (!slab) {
		/* kmalloc_nolock() doesn't support large kmalloc */
		free_large_kmalloc(page, (void *)object);
		return;
	}

	s = slab->slab_cache;
	slab_free(s, slab, x, _RET_IP_);
}
EXPORT_SYMBOL(kfree);

/*
 * kfree_nolock() - 在持锁/IRQ/NMI 上下文中释放对象（受限版 kfree）
 *
 * 问题背景：标准 kfree() 在某些上下文中不能调用（如持 raw_spinlock_t、
 * 硬中断、NMI），因为它会触发 kmemleak、kfence 等需要休眠或自旋的操作。
 *
 * kfree_nolock 解决方案：
 *   - 跳过 kmemleak/kfence/debug 记录（分配时就跳过了，所以释放时也跳过）
 *   - 使用 free_to_pcs(..., allow_spin=false)，trylock 失败直接走 __slab_free
 *   - __slab_free 内部也使用 trylock 而非阻塞 spinlock
 *
 * 两个强制约束：
 *   1. 必须配对使用：只能释放由 kmalloc_nolock() 分配的对象。
 *      若用 kmalloc() 分配后 kfree_nolock() 释放，会跳过 kmemleak/kfence
 *      的 free 记录，产生误报（kmemleak 认为内存泄漏，kfence 认为越界）。
 *   2. 不支持大对象（large_kmalloc）。
 */
void kfree_nolock(const void *object)
{
	struct slab *slab;
	struct kmem_cache *s;
	void *x = (void *)object;

	if (unlikely(ZERO_OR_NULL_PTR(object)))
		return;

	slab = virt_to_slab(object);
	if (unlikely(!slab)) {
		WARN_ONCE(1, "large_kmalloc is not supported by kfree_nolock()");
		return;
	}

	s = slab->slab_cache;

	memcg_slab_free_hook(s, slab, &x, 1);
	alloc_tagging_slab_free_hook(s, slab, &x, 1);
	/*
	 * Unlike slab_free() do NOT call the following:
	 * kmemleak_free_recursive(x, s->flags);
	 * debug_check_no_locks_freed(x, s->object_size);
	 * debug_check_no_obj_freed(x, s->object_size);
	 * __kcsan_check_access(x, s->object_size, ..);
	 * kfence_free(x);
	 * since they take spinlocks or not safe from any context.
	 */
	kmsan_slab_free(s, x);
	/*
	 * If KASAN finds a kernel bug it will do kasan_report_invalid_free()
	 * which will call raw_spin_lock_irqsave() which is technically
	 * unsafe from NMI, but take chance and report kernel bug.
	 * The sequence of
	 * kasan_report_invalid_free() -> raw_spin_lock_irqsave() -> NMI
	 *  -> kfree_nolock() -> kasan_report_invalid_free() on the same CPU
	 * is double buggy and deserves to deadlock.
	 */
	if (kasan_slab_pre_free(s, x))
		return;
	/*
	 * memcg, kasan_slab_pre_free are done for 'x'.
	 * The only thing left is kasan_poison without quarantine,
	 * since kasan quarantine takes locks and not supported from NMI.
	 */
	kasan_slab_free(s, x, false, false, /* skip quarantine */true);

	if (likely(can_free_to_pcs(slab)) && likely(free_to_pcs(s, x, false)))
		return;

	/*
	 * __slab_free() can locklessly cmpxchg16 into a slab, but then it might
	 * need to take spin_lock for further processing.
	 * Avoid the complexity and simply add to a deferred list.
	 */
	defer_free(s, x);
}
EXPORT_SYMBOL_GPL(kfree_nolock);

static __always_inline __realloc_size(2) void *
__do_krealloc(const void *p, size_t new_size, unsigned long align, gfp_t flags, int nid, kmalloc_token_t token)
{
	void *ret;
	size_t ks = 0;
	int orig_size = 0;
	struct kmem_cache *s = NULL;

	if (unlikely(ZERO_OR_NULL_PTR(p)))
		goto alloc_new;

	/* Check for double-free. */
	if (!kasan_check_byte(p))
		return NULL;

	if (is_kfence_address(p)) {
		ks = orig_size = kfence_ksize(p);
	} else {
		struct page *page = virt_to_page(p);
		struct slab *slab = page_slab(page);

		if (!slab) {
			/* Big kmalloc object */
			ks = page_size(page);
			WARN_ON(ks <= KMALLOC_MAX_CACHE_SIZE);
			WARN_ON(p != page_address(page));
		} else {
			s = slab->slab_cache;
			orig_size = get_orig_size(s, (void *)p);
			ks = s->object_size;
		}
	}

	/*
	 * If reallocation is not necessary (e. g. the new size is less
	 * than the current allocated size), the current allocation will be
	 * preserved unless __GFP_THISNODE is set. In the latter case a new
	 * allocation on the requested node will be attempted.
	 */
	if (unlikely(flags & __GFP_THISNODE) && nid != NUMA_NO_NODE &&
		     nid != page_to_nid(virt_to_page(p)))
		goto alloc_new;

	/* If the old object doesn't fit, allocate a bigger one */
	if (new_size > ks)
		goto alloc_new;

	/* If the old object doesn't satisfy the new alignment, allocate a new one */
	if (!IS_ALIGNED((unsigned long)p, align))
		goto alloc_new;

	/* Zero out spare memory. */
	if (want_init_on_alloc(flags)) {
		kasan_disable_current();
		if (orig_size && orig_size < new_size)
			memset(kasan_reset_tag(p) + orig_size, 0, new_size - orig_size);
		else
			memset(kasan_reset_tag(p) + new_size, 0, ks - new_size);
		kasan_enable_current();
	}

	/* Setup kmalloc redzone when needed */
	if (s && slub_debug_orig_size(s)) {
		set_orig_size(s, (void *)p, new_size);
		if (s->flags & SLAB_RED_ZONE && new_size < ks)
			memset_no_sanitize_memory(kasan_reset_tag(p) + new_size,
						SLUB_RED_ACTIVE, ks - new_size);
	}

	p = kasan_krealloc(p, new_size, flags);
	return (void *)p;

alloc_new:
	ret = __kmalloc_node_track_caller_noprof(PASS_KMALLOC_PARAMS(new_size, NULL, token), flags, nid, _RET_IP_);
	if (ret && p) {
		/* Disable KASAN checks as the object's redzone is accessed. */
		kasan_disable_current();
		memcpy(ret, kasan_reset_tag(p), min(new_size, (size_t)(orig_size ?: ks)));
		kasan_enable_current();
	}

	return ret;
}

/*
 * krealloc_node_align_noprof() - 重分配内存（底层实现）
 *
 * 三条路径：
 *   1. new_size == 0：等价于 kfree，返回 ZERO_SIZE_PTR（不返回 NULL，
 *      这样 krealloc 可以区分"成功分配大小为 0"和"分配失败"）
 *   2. __do_krealloc 返回了原指针（就地扩展，不需要重新分配）：
 *      不释放旧指针，直接返回
 *   3. __do_krealloc 返回了新指针（重新分配）：
 *      kfree 旧指针（tag 不同说明是真正的新分配），返回新指针
 *
 * kasan_reset_tag(p) != kasan_reset_tag(ret) 的判断：
 *   KASAN HW_TAGS 模式下每次分配得到的指针带有不同的随机 tag，
 *   若 __do_krealloc 返回的地址去掉 tag 后与原地址相同（就地扩展），
 *   两者的"去 tag 后的原始地址"相同，不应 kfree；
 *   若地址不同说明重新分配了，需要释放旧内存。
 */
void *krealloc_node_align_noprof(const void *p, DECL_TOKEN_PARAMS(new_size, token), unsigned long align,
				 gfp_t flags, int nid)
{
	void *ret;

	if (unlikely(!new_size)) {
		kfree(p);
		return ZERO_SIZE_PTR;
	}

	ret = __do_krealloc(p, new_size, align, flags, nid, PASS_TOKEN_PARAM(token));
	if (ret && kasan_reset_tag(p) != kasan_reset_tag(ret))
		kfree(p);

	return ret;
}
EXPORT_SYMBOL(krealloc_node_align_noprof);

/*
 * kmalloc_gfp_adjust() - 为超过 PAGE_SIZE 的 kvmalloc 请求调整 GFP 标志
 *
 * 背景：kvmalloc 先尝试 kmalloc（物理连续），失败再回退到 vmalloc（分散页）。
 * 对超页大小的请求，物理连续内存很难获得，直接触发内存回收代价高、对系统影响大。
 *
 * 策略调整：
 *   - __GFP_NOWARN：kmalloc 失败时不打印警告（因为有 vmalloc 兜底，失败预期内）
 *   - 去掉 __GFP_DIRECT_RECLAIM（除非用了 __GFP_RETRY_MAYFAIL）：
 *     不做同步直接回收，让内核可以触发 kswapd/kcompactd 异步工作，
 *     但 kmalloc 本次调用不阻塞等待回收结果（快速失败，立即去 vmalloc）
 *   - 去掉 __GFP_NOFAIL：nofail 语义由 vmalloc 回退路径保证，
 *     kmalloc 层不需要无限重试
 */
static gfp_t kmalloc_gfp_adjust(gfp_t flags, size_t size)
{
	/*
	 * We want to attempt a large physically contiguous block first because
	 * it is less likely to fragment multiple larger blocks and therefore
	 * contribute to a long term fragmentation less than vmalloc fallback.
	 * However make sure that larger requests are not too disruptive - i.e.
	 * do not direct reclaim unless physically continuous memory is preferred
	 * (__GFP_RETRY_MAYFAIL mode). We still kick in kswapd/kcompactd to
	 * start working in the background
	 */
	if (size > PAGE_SIZE) {
		flags |= __GFP_NOWARN;

		if (!(flags & __GFP_RETRY_MAYFAIL))
			flags &= ~__GFP_DIRECT_RECLAIM;

		/* nofail semantic is implemented by the vmalloc fallback */
		flags &= ~__GFP_NOFAIL;
	}

	return flags;
}

/*
 * __kvmalloc_node_noprof() - kvmalloc 底层实现（kmalloc 优先，vmalloc 兜底）
 *
 * 背景：某些场景需要大块内存（> 几 KB），但不要求物理连续。kvmalloc 先用
 * kmalloc 尝试物理连续分配（缓存热，TLB 压力低），失败再用 vmalloc（分散页，
 * 可以分配任意大小，但 TLB 开销更高）。
 *
 * sub-page 请求（<= PAGE_SIZE）不用 vmalloc（vmalloc 最小粒度是页，浪费太多），
 * kmalloc 失败直接返回 NULL。
 *
 * > INT_MAX：拒绝，vmalloc 的内部实现用 int 类型计算，超出会溢出。
 *
 * allow_block：vmalloc 的大页映射路径（VM_ALLOW_HUGE_VMAP）包含 might_sleep，
 * 只有 gfpflags_allow_blocking() 的上下文才允许。
 */
void *__kvmalloc_node_noprof(DECL_KMALLOC_PARAMS(size, b, token), unsigned long align,
			     gfp_t flags, int node)
{
	bool allow_block;
	void *ret;
	const struct slab_alloc_context ac = {
		.caller_addr = _RET_IP_,
		.orig_size = size,
		.alloc_flags = SLAB_ALLOC_DEFAULT,
	};

	/*
	 * It doesn't really make sense to fallback to vmalloc for sub page
	 * requests
	 */
	ret = __do_kmalloc_node(PASS_BUCKET_PARAM(b),
				kmalloc_gfp_adjust(flags, size),
				node, PASS_TOKEN_PARAM(token), &ac);
	if (ret || size <= PAGE_SIZE)
		return ret;

	/* Don't even allow crazy sizes */
	if (unlikely(size > INT_MAX)) {
		WARN_ON_ONCE(!(flags & __GFP_NOWARN));
		return NULL;
	}

	/*
	 * For non-blocking the VM_ALLOW_HUGE_VMAP is not used
	 * because the huge-mapping path in vmalloc contains at
	 * least one might_sleep() call.
	 *
	 * TODO: Revise huge-mapping path to support non-blocking
	 * flags.
	 */
	allow_block = gfpflags_allow_blocking(flags);

	/*
	 * kvmalloc() can always use VM_ALLOW_HUGE_VMAP,
	 * since the callers already cannot assume anything
	 * about the resulting pointer, and cannot play
	 * protection games.
	 */
	return __vmalloc_node_range_noprof(size, align, VMALLOC_START, VMALLOC_END,
			flags, PAGE_KERNEL, allow_block ? VM_ALLOW_HUGE_VMAP:0,
			node, __builtin_return_address(0));
}
EXPORT_SYMBOL(__kvmalloc_node_noprof);

/**
 * kvfree() - Free memory.
 * @addr: Pointer to allocated memory.
 *
 * kvfree frees memory allocated by any of vmalloc(), kmalloc() or kvmalloc().
 * It is slightly more efficient to use kfree() or vfree() if you are certain
 * that you know which one to use.
 *
 * Context: Either preemptible task context or not-NMI interrupt.
 */
void kvfree(const void *addr)
{
	if (is_vmalloc_addr(addr))
		vfree(addr);
	else
		kfree(addr);
}
EXPORT_SYMBOL(kvfree);

/**
 * kvfree_atomic() - Free memory.
 * @addr: Pointer to allocated memory.
 *
 * Same as kvfree(), but uses vfree_atomic() for vmalloc
 * backed memory. Must not be called from NMI context.
 */
void kvfree_atomic(const void *addr)
{
	if (is_vmalloc_addr(addr))
		vfree_atomic(addr);
	else
		kfree(addr);
}
EXPORT_SYMBOL(kvfree_atomic);

/**
 * kvfree_sensitive - Free a data object containing sensitive information.
 * @addr: address of the data object to be freed.
 * @len: length of the data object.
 *
 * Use the special memzero_explicit() function to clear the content of a
 * kvmalloc'ed object containing sensitive data to make sure that the
 * compiler won't optimize out the data clearing.
 */
void kvfree_sensitive(const void *addr, size_t len)
{
	if (likely(!ZERO_OR_NULL_PTR(addr))) {
		memzero_explicit((void *)addr, len);
		kvfree(addr);
	}
}
EXPORT_SYMBOL(kvfree_sensitive);

void *kvrealloc_node_align_noprof(const void *p, DECL_TOKEN_PARAMS(size, token), unsigned long align,
				  gfp_t flags, int nid)
{
	void *n;

	if (is_vmalloc_addr(p))
		return vrealloc_node_align_noprof(p, size, align, flags, nid);

	n = krealloc_node_align_noprof(p, PASS_TOKEN_PARAMS(size, token), align, kmalloc_gfp_adjust(flags, size), nid);
	if (!n) {
		/* We failed to krealloc(), fall back to kvmalloc(). */
		n = __kvmalloc_node_noprof(PASS_KMALLOC_PARAMS(size, NULL, token), align, flags, nid);
		if (!n)
			return NULL;

		if (p) {
			/* We already know that `p` is not a vmalloc address. */
			kasan_disable_current();
			memcpy(n, kasan_reset_tag(p), min(size, ksize(p)));
			kasan_enable_current();

			kfree(p);
		}
	}

	return n;
}
EXPORT_SYMBOL(kvrealloc_node_align_noprof);

struct detached_freelist {
	struct slab *slab;
	void *tail;
	void *freelist;
	int cnt;
	struct kmem_cache *s;
};

/*
 * This function progressively scans the array with free objects (with
 * a limited look ahead) and extract objects belonging to the same
 * slab.  It builds a detached freelist directly within the given
 * slab/objects.  This can happen without any need for
 * synchronization, because the objects are owned by running process.
 * The freelist is build up as a single linked list in the objects.
 * The idea is, that this detached freelist can then be bulk
 * transferred to the real freelist(s), but only requiring a single
 * synchronization primitive.  Look ahead in the array is limited due
 * to performance reasons.
 */
static inline
int build_detached_freelist(struct kmem_cache *s, size_t size,
			    void **p, struct detached_freelist *df)
{
	int lookahead = 3;
	void *object;
	struct page *page;
	struct slab *slab;
	size_t same;

	object = p[--size];
	page = virt_to_page(object);
	slab = page_slab(page);
	if (!s) {
		/* Handle kalloc'ed objects */
		if (!slab) {
			free_large_kmalloc(page, object);
			df->slab = NULL;
			return size;
		}
		/* Derive kmem_cache from object */
		df->slab = slab;
		df->s = slab->slab_cache;
	} else {
		df->slab = slab;
		df->s = s;
	}

	/* Start new detached freelist */
	df->tail = object;
	df->freelist = object;
	df->cnt = 1;

	if (is_kfence_address(object))
		return size;

	set_freepointer(df->s, object, NULL);

	same = size;
	while (size) {
		object = p[--size];
		/* df->slab is always set at this point */
		if (df->slab == virt_to_slab(object)) {
			/* Opportunity build freelist */
			set_freepointer(df->s, object, df->freelist);
			df->freelist = object;
			df->cnt++;
			same--;
			if (size != same)
				swap(p[size], p[same]);
			continue;
		}

		/* Limit look ahead search */
		if (!--lookahead)
			break;
	}

	return same;
}

/*
 * Internal bulk free of objects that were not initialised by the post alloc
 * hooks and thus should not be processed by the free hooks
 */
static void __kmem_cache_free_bulk(struct kmem_cache *s, size_t size, void **p)
{
	if (!size)
		return;

	do {
		struct detached_freelist df;

		size = build_detached_freelist(s, size, p, &df);
		if (!df.slab)
			continue;

		if (kfence_free(df.freelist))
			continue;

		__slab_free(df.s, df.slab, df.freelist, df.tail, df.cnt,
			     _RET_IP_);
	} while (likely(size));
}

/* Note that interrupts must be enabled when calling this function. */
void kmem_cache_free_bulk(struct kmem_cache *s, size_t size, void **p)
{
	if (!size)
		return;

	/*
	 * freeing to sheaves is so incompatible with the detached freelist so
	 * once we go that way, we have to do everything differently
	 */
	if (s && cache_has_sheaves(s)) {
		free_to_pcs_bulk(s, size, p);
		return;
	}

	do {
		struct detached_freelist df;

		size = build_detached_freelist(s, size, p, &df);
		if (!df.slab)
			continue;

		slab_free_bulk(df.s, df.slab, df.freelist, df.tail, &p[size],
			       df.cnt, _RET_IP_);
	} while (likely(size));
}
EXPORT_SYMBOL(kmem_cache_free_bulk);

/*
 * __refill_objects_node() - 从指定 NUMA 节点的 partial list 批量取对象
 *
 * 这是 refill_sheaf / bulk_alloc 的核心：一次性从 partial list 取出多个 slab，
 * 批量提取对象，填入 p[] 数组。
 *
 * 流程：
 *   1. get_partial_node_bulk()：持 list_lock，批量摘出若干 slab（总空闲对象数
 *      在 [min, max] 范围内），摘出的 slab 暂存于 pc.slabs 链表。
 *      摘出时清除 SL_partial 标志（slab 暂时不在 partial list 上）。
 *
 *   2. 遍历 pc.slabs，对每个 slab：
 *      a. get_freelist_nofreeze()：CAS 原子取走整条 freelist，将 inuse 设为满
 *      b. 遍历 freelist，取出对象写入 p[]，直到 refilled >= max
 *      c. 若 freelist 有剩余（count > 0）：
 *         - 尝试 __slab_try_return_freelist 乐观归还（slab 刚刚被我们设为满，
 *           若没有并发释放，CAS 能快速恢复 freelist）
 *         - 失败则找到链表尾，调用 __slab_free 完整归还（会处理 partial list 状态）
 *
 *   3. pc.slabs 中剩余未消耗完的 slab 批量加回 partial list（list_splice_tail，
 *      保持顺序，SET SL_partial 标志，更新 n->nr_partial）。
 *
 * 批量摘 slab 的优势：只需一次 list_lock，而不是每取一个对象就加一次锁；
 * 批量归还也只需一次 list_lock，大幅减少锁竞争。
 */
static unsigned int
__refill_objects_node(struct kmem_cache *s, void **p, gfp_t gfp, unsigned int min,
		      unsigned int max, struct kmem_cache_node *n,
		      bool allow_spin)
{
	struct partial_bulk_context pc;
	struct slab *slab, *slab2;
	unsigned int refilled = 0;
	unsigned long flags;
	void *object;

	pc.flags = gfp;
	pc.min_objects = min;
	pc.max_objects = max;

	if (!get_partial_node_bulk(s, n, &pc, allow_spin))
		return 0;

	list_for_each_entry_safe(slab, slab2, &pc.slabs, slab_list) {

		unsigned int count;

		list_del(&slab->slab_list);

		object = get_freelist_nofreeze(s, slab, &count);

		while (count && refilled < max) {
			p[refilled] = object;
			object = get_freepointer(s, object);
			maybe_wipe_obj_freeptr(s, p[refilled]);

			refilled++;
			count--;
		}

		/*
		 * Freelist had more objects than we can accommodate, we need to
		 * free them back. First we try to be optimistic and assume the
		 * slab is still full since we just detached its freelist.
		 * Otherwise we must find the tail object.
		 */
		if (unlikely(count)) {
			void *head = object;
			void *tail;

			if (__slab_try_return_freelist(s, slab, head, count)) {
				list_add(&slab->slab_list, &pc.slabs);
				break;
			}

			do {
				tail = object;
				object = get_freepointer(s, object);
			} while (object);
			__slab_free(s, slab, head, tail, count, _RET_IP_);
		}

		if (refilled >= max)
			break;
	}

	if (!list_empty(&pc.slabs)) {
		spin_lock_irqsave(&n->list_lock, flags);

		list_for_each_entry(slab, &pc.slabs, slab_list)
			set_node_partial_state(n, slab);

		list_splice_tail(&pc.slabs, &n->partial);

		spin_unlock_irqrestore(&n->list_lock, flags);
	}

	return refilled;
}

#ifdef CONFIG_NUMA
static unsigned int
__refill_objects_any(struct kmem_cache *s, void **p, gfp_t gfp, unsigned int min,
		     unsigned int max)
{
	struct zonelist *zonelist;
	struct zoneref *z;
	struct zone *zone;
	enum zone_type highest_zoneidx = gfp_zone(gfp);
	unsigned int cpuset_mems_cookie;
	unsigned int refilled = 0;

	/* see get_from_any_partial() for the defrag ratio description */
	if (!s->remote_node_defrag_ratio ||
			get_cycles() % 1024 > s->remote_node_defrag_ratio)
		return 0;

	do {
		cpuset_mems_cookie = read_mems_allowed_begin();
		zonelist = node_zonelist(mempolicy_slab_node(), gfp);
		for_each_zone_zonelist(zone, z, zonelist, highest_zoneidx) {
			struct kmem_cache_node *n;
			unsigned int r;

			n = get_node(s, zone_to_nid(zone));

			if (!n || !cpuset_zone_allowed(zone, gfp) ||
					n->nr_partial <= s->min_partial)
				continue;

			r = __refill_objects_node(s, p, gfp, min, max, n,
						  /* allow_spin = */ false);
			refilled += r;

			if (r >= min) {
				/*
				 * Don't check read_mems_allowed_retry() here -
				 * if mems_allowed was updated in parallel, that
				 * was a harmless race between allocation and
				 * the cpuset update
				 */
				return refilled;
			}
			p += r;
			min -= r;
			max -= r;
		}
	} while (read_mems_allowed_retry(cpuset_mems_cookie));

	return refilled;
}
#else
static inline unsigned int
__refill_objects_any(struct kmem_cache *s, void **p, gfp_t gfp, unsigned int min,
		     unsigned int max)
{
	return 0;
}
#endif

/*
 * refill_objects() - 按优先级批量取对象（本节点 → 跨节点 → 新 slab）
 *
 * 这是 refill_sheaf 和 __kmem_cache_alloc_bulk 的通用填充入口。
 * 目标：取出 [min, max] 个对象填入 p[]。
 *
 * 三级策略：
 *   1. 本节点 partial list（__refill_objects_node，本地，快）
 *   2. 其他节点 partial list（__refill_objects_any，跨 NUMA，慢但节省内存）
 *   3. 向伙伴系统申请新 slab（new_slab + alloc_from_new_slab，最慢）
 *      若新 slab 仍不够（如 min > 每 slab 对象数），循环重试
 *
 * 返回实际取到的对象数（可能 < min，表示内存不足）。
 */
static unsigned int
refill_objects(struct kmem_cache *s, void **p, gfp_t gfp, unsigned int min,
	       unsigned int max)
{
	int local_node = numa_mem_id();
	unsigned int refilled;
	struct slab *slab;

	refilled = __refill_objects_node(s, p, gfp, min, max,
					 get_node(s, local_node),
					 /* allow_spin = */ true);
	if (refilled >= min)
		return refilled;

	refilled += __refill_objects_any(s, p + refilled, gfp, min - refilled,
					 max - refilled);
	if (refilled >= min)
		return refilled;

new_slab:

	slab = new_slab(s, gfp, SLAB_ALLOC_DEFAULT, local_node);
	if (!slab)
		goto out;

	stat(s, ALLOC_SLAB);

	refilled += alloc_from_new_slab(s, slab, p + refilled, max - refilled,
					/* allow_spin = */ true);

	if (refilled < min)
		goto new_slab;

out:
	return refilled;
}

/*
 * __kmem_cache_alloc_bulk() - 批量分配的慢速路径（sheaf 快速路径失败后）
 *
 * 调试 cache / SLUB_TINY：逐个调用 ___slab_alloc（无 sheaf 机制）。
 * 正常 cache：调用 refill_objects 批量从 partial list / 伙伴系统取对象。
 *
 * 任一对象分配失败时，已分配的部分通过 __kmem_cache_free_bulk 全部释放，
 * 返回 false（全或无语义，调用方无需处理部分成功）。
 */
static bool __kmem_cache_alloc_bulk(struct kmem_cache *s, gfp_t flags,
		size_t size, void **p)
{
	int i;

	if (IS_ENABLED(CONFIG_SLUB_TINY) || kmem_cache_debug(s)) {
		const struct slab_alloc_context ac = {
			.caller_addr = _RET_IP_,
			.orig_size = s->object_size,
			.alloc_flags = SLAB_ALLOC_DEFAULT,
		};
		for (i = 0; i < size; i++) {

			p[i] = ___slab_alloc(s, flags, NUMA_NO_NODE, &ac);
			if (unlikely(!p[i]))
				goto error;

			maybe_wipe_obj_freeptr(s, p[i]);
		}
	} else {
		i = refill_objects(s, p, flags, size, size);
		if (i < size)
			goto error;
		stat_add(s, ALLOC_SLOWPATH, i);
	}

	return true;

error:
	__kmem_cache_free_bulk(s, i, p);
	return false;
}

/**
 * kmem_cache_alloc_bulk - Allocate multiple objects
 * @s:		The cache to allocate from
 * @flags:	GFP_* flags. See kmalloc().
 * @size:	Number of objects to allocate
 * @p:		Array of allocated objects
 *
 * Allocate @size objects from @s and places them into @p.  @size must be larger
 * than 0.
 *
 * Interrupts must be enabled when calling this function.
 *
 * Unlike alloc_pages_bulk(), this function does not check for already allocated
 * objects in @p, and thus the caller does not need to zero it.
 *
 * Return: %true if the allocation succeeded, or %false if it failed.
 */
bool kmem_cache_alloc_bulk_noprof(struct kmem_cache *s, gfp_t flags,
		size_t size, void **p)
{
	unsigned int i = 0;
	void *kfence_obj;
	const struct slab_alloc_context ac = {
		.orig_size = s->object_size,
		.alloc_flags = SLAB_ALLOC_DEFAULT,
	};

	if (!size)
		return false;

	s = slab_pre_alloc_hook(s, flags);
	if (unlikely(!s))
		return false;

	/*
	 * to make things simpler, only assume at most once kfence allocated
	 * object per bulk allocation and choose its index randomly
	 */
	kfence_obj = kfence_alloc(s, s->object_size, flags);

	if (unlikely(kfence_obj)) {
		if (unlikely(size == 1)) {
			p[0] = kfence_obj;
			goto out;
		}
		size--;
	}

	i = alloc_from_pcs_bulk(s, size, p);
	if (i < size) {
		/*
		 * If we ran out of memory, don't bother with freeing back to
		 * the percpu sheaves, we have bigger problems.
		 */
		if (unlikely(!__kmem_cache_alloc_bulk(s, flags, size - i,
				p + i))) {
			if (i > 0)
				__kmem_cache_free_bulk(s, i, p);
			if (kfence_obj)
				__kfence_free(kfence_obj);
			return false;
		}
	}

	if (unlikely(kfence_obj)) {
		int idx = get_random_u32_below(size + 1);

		if (idx != size)
			p[size] = p[idx];
		p[idx] = kfence_obj;

		size++;
	}

out:
	/* memcg and kmem_cache debug support and memory initialization */
	return likely(slab_post_alloc_hook(s, flags, size, p, &ac));
}
EXPORT_SYMBOL(kmem_cache_alloc_bulk_noprof);

/*
 * Object placement in a slab is made very easy because we always start at
 * offset 0. If we tune the size of the object to the alignment then we can
 * get the required alignment by putting one properly sized object after
 * another.
 *
 * Notice that the allocation order determines the sizes of the per cpu
 * caches. Each processor has always one slab available for allocations.
 * Increasing the allocation order reduces the number of times that slabs
 * must be moved on and off the partial lists and is therefore a factor in
 * locking overhead.
 */

/*
 * Minimum / Maximum order of slab pages. This influences locking overhead
 * and slab fragmentation. A higher order reduces the number of partial slabs
 * and increases the number of allocations possible without having to
 * take the list_lock.
 */
static unsigned int slub_min_order;
static unsigned int slub_max_order =
	IS_ENABLED(CONFIG_SLUB_TINY) ? 1 : PAGE_ALLOC_COSTLY_ORDER;
static unsigned int slub_min_objects;

/*
 * Calculate the order of allocation given an slab object size.
 *
 * The order of allocation has significant impact on performance and other
 * system components. Generally order 0 allocations should be preferred since
 * order 0 does not cause fragmentation in the page allocator. Larger objects
 * be problematic to put into order 0 slabs because there may be too much
 * unused space left. We go to a higher order if more than 1/16th of the slab
 * would be wasted.
 *
 * In order to reach satisfactory performance we must ensure that a minimum
 * number of objects is in one slab. Otherwise we may generate too much
 * activity on the partial lists which requires taking the list_lock. This is
 * less a concern for large slabs though which are rarely used.
 *
 * slab_max_order specifies the order where we begin to stop considering the
 * number of objects in a slab as critical. If we reach slab_max_order then
 * we try to keep the page order as low as possible. So we accept more waste
 * of space in favor of a small page order.
 *
 * Higher order allocations also allow the placement of more objects in a
 * slab and thereby reduce object handling overhead. If the user has
 * requested a higher minimum order then we start with that one instead of
 * the smallest order which will fit the object.
 */
/*
 * calc_slab_order() - 找到满足碎片约束的最小 slab order
 *
 * 对于给定的对象 size，遍历 [min_order, max_order]，找到第一个满足
 * "尾部浪费 ≤ slab_size / fract_leftover" 的 order。
 *
 * 浪费 = slab_size % size（无法放下完整对象的剩余字节）。
 * fract_leftover 越大要求越严格（容许的浪费比例越小）。
 * 例：fract_leftover=16 → 最多允许 1/16 = 6.25% 的空间浪费。
 *
 * 为什么 order 越大浪费比例只会越小？
 *   slab_size = 2^order * PAGE_SIZE，rem = slab_size % size。
 *   增大 order 使 slab_size 翻倍，而 rem < size 不变，
 *   浪费比例 rem/slab_size 减半。因此只需找最小满足条件的 order。
 */
static inline unsigned int calc_slab_order(unsigned int size,
		unsigned int min_order, unsigned int max_order,
		unsigned int fract_leftover)
{
	unsigned int order;

	for (order = min_order; order <= max_order; order++) {

		unsigned int slab_size = (unsigned int)PAGE_SIZE << order;
		unsigned int rem;

		rem = slab_size % size;

		if (rem <= slab_size / fract_leftover)
			break;
	}

	return order;
}

/*
 * calculate_order() - 为给定对象 size 选择最优的 slab order
 *
 * 核心设计哲学：在"per-slab 对象数够用"和"内存碎片尽可能少"之间取平衡。
 *
 * min_objects 的计算逻辑：
 *   每个 slab 需要容纳足够多的对象，才能摊薄 sheaf 补货（refill_sheaf）时
 *   的 partial list 操作开销。CPU 数越多，潜在并发量越大，每 slab 需要的
 *   对象数也应越多（避免 partial list 过于频繁地被并发竞争）。
 *   公式：min_objects = 4 * (fls(nr_cpus) + 1)
 *   CPU=1→8, CPU=4→12, CPU=16→20, CPU=64→28
 *
 *   注意：num_present_cpus() 在某些架构上不可靠（可能只有 1，而实际有更多），
 *   因此取 num_present_cpus() 和 nr_cpu_ids 的折衷。
 *
 * 渐进式碎片容忍策略（四轮尝试）：
 *   第 1 轮：fract=16，容忍 ≤ 1/16 浪费，要求最严格
 *   第 2 轮：fract=8， 容忍 ≤ 1/8  浪费
 *   第 3 轮：fract=4， 容忍 ≤ 1/4  浪费
 *   第 4 轮：fract=2， 容忍 ≤ 1/2  浪费（几乎接受任何情况）
 *   每轮都找 [min_order, slub_max_order] 内的最小满足 order。
 *
 * 若所有轮次都超过 slub_max_order（默认 order 3 = 32KB）：
 *   直接用 get_order(size) —— 单个对象刚好装得下的最小 order，
 *   不管碎片多少，优先让分配成功。
 *
 * 返回 -ENOSYS 表示连单对象都超过 MAX_PAGE_ORDER，cache 无法创建。
 */
static inline int calculate_order(unsigned int size)
{
	unsigned int order;
	unsigned int min_objects;
	unsigned int max_objects;
	unsigned int min_order;

	min_objects = slub_min_objects;
	if (!min_objects) {
		/*
		 * Some architectures will only update present cpus when
		 * onlining them, so don't trust the number if it's just 1. But
		 * we also don't want to use nr_cpu_ids always, as on some other
		 * architectures, there can be many possible cpus, but never
		 * onlined. Here we compromise between trying to avoid too high
		 * order on systems that appear larger than they are, and too
		 * low order on systems that appear smaller than they are.
		 */
		unsigned int nr_cpus = num_present_cpus();
		if (nr_cpus <= 1)
			nr_cpus = nr_cpu_ids;
		min_objects = 4 * (fls(nr_cpus) + 1);
	}
	/* min_objects can't be 0 because get_order(0) is undefined */
	max_objects = max(order_objects(slub_max_order, size), 1U);
	min_objects = min(min_objects, max_objects);

	min_order = max_t(unsigned int, slub_min_order,
			  get_order(min_objects * size));
	if (order_objects(min_order, size) > MAX_OBJS_PER_PAGE)
		return get_order(size * MAX_OBJS_PER_PAGE) - 1;

	/*
	 * Attempt to find best configuration for a slab. This works by first
	 * attempting to generate a layout with the best possible configuration
	 * and backing off gradually.
	 *
	 * We start with accepting at most 1/16 waste and try to find the
	 * smallest order from min_objects-derived/slab_min_order up to
	 * slab_max_order that will satisfy the constraint. Note that increasing
	 * the order can only result in same or less fractional waste, not more.
	 *
	 * If that fails, we increase the acceptable fraction of waste and try
	 * again. The last iteration with fraction of 1/2 would effectively
	 * accept any waste and give us the order determined by min_objects, as
	 * long as at least single object fits within slab_max_order.
	 */
	for (unsigned int fraction = 16; fraction > 1; fraction /= 2) {
		order = calc_slab_order(size, min_order, slub_max_order,
					fraction);
		if (order <= slub_max_order)
			return order;
	}

	/*
	 * Doh this slab cannot be placed using slab_max_order.
	 */
	order = get_order(size);
	if (order <= MAX_PAGE_ORDER)
		return order;
	return -ENOSYS;
}

/* init_kmem_cache_node() - 初始化 per-node 管理结构的各字段 */
static void
init_kmem_cache_node(struct kmem_cache_node *n)
{
	n->nr_partial = 0;
	spin_lock_init(&n->list_lock);
	INIT_LIST_HEAD(&n->partial);
#ifdef CONFIG_SLUB_DEBUG
	atomic_long_set(&n->nr_slabs, 0);
	atomic_long_set(&n->total_objects, 0);
	INIT_LIST_HEAD(&n->full);
#endif
}

#ifdef CONFIG_SLUB_STATS
static inline int alloc_kmem_cache_stats(struct kmem_cache *s)
{
	BUILD_BUG_ON(PERCPU_DYNAMIC_EARLY_SIZE <
			NR_KMALLOC_TYPES * KMALLOC_SHIFT_HIGH *
			sizeof(struct kmem_cache_stats));

	s->cpu_stats = alloc_percpu(struct kmem_cache_stats);

	if (!s->cpu_stats)
		return 0;

	return 1;
}
#endif

/*
 * init_percpu_sheaves() - 为每个 CPU 初始化 per-CPU sheaf 结构
 *
 * 设计难点：在 cache 创建时，kmalloc 可能尚未完全初始化（尤其是 kmalloc
 * 自身的 cache 创建过程中），因此不能随意分配内存。
 *
 * bootstrap_sheaf 的作用：
 *   一个静态全局的零容量 sheaf，所有"尚不需要真实 sheaf"的 cache 共用它。
 *   它的特性使快速路径自然失败并降级到慢速路径：
 *     - size=0 → 分配快速路径立即失败（"sheaf 空了，需要补货"）
 *     - size=0 == sheaf_capacity=0 → 释放快速路径立即失败（"sheaf 满了，需要腾空"）
 *   慢速路径通过检查 s->sheaf_capacity == 0 来识别 bootstrap 状态，
 *   不会尝试真正操作这个静态 sheaf。
 *
 * 哪些 cache 使用 bootstrap_sheaf（即 sheaf_capacity=0）：
 *   - kmem_cache 和 kmem_cache_node 自身（它们是所有 cache 的基础）
 *   - 开启调试的 cache（调试 cache 不用 sheaf 快速路径）
 *   - SLUB_TINY 模式下所有 cache
 *   - kmalloc cache 在初始化阶段临时使用，之后替换为真实 sheaf
 *
 * 共享一个静态 bootstrap_sheaf 是安全的，因为它的 objects[] 数组大小为 0，
 * 永远不会被写入，且 cache 指针为 NULL（便于 destroy 时识别并跳过）。
 */
static int init_percpu_sheaves(struct kmem_cache *s)
{
	static struct slab_sheaf bootstrap_sheaf = {};
	int cpu;

	for_each_possible_cpu(cpu) {
		struct slub_percpu_sheaves *pcs;

		pcs = per_cpu_ptr(s->cpu_sheaves, cpu);

		local_trylock_init(&pcs->lock);

		/*
		 * Bootstrap sheaf has zero size so fast-path allocation fails.
		 * It has also size == s->sheaf_capacity, so fast-path free
		 * fails. In the slow paths we recognize the situation by
		 * checking s->sheaf_capacity. This allows fast paths to assume
		 * s->cpu_sheaves and pcs->main always exists and are valid.
		 * It's also safe to share the single static bootstrap_sheaf
		 * with zero-sized objects array as it's never modified.
		 *
		 * Bootstrap_sheaf also has NULL pointer to kmem_cache so we
		 * recognize it and not attempt to free it when destroying the
		 * cache.
		 *
		 * We keep bootstrap_sheaf for kmem_cache and kmem_cache_node,
		 * caches with debug enabled, and all caches with SLUB_TINY.
		 * For kmalloc caches it's used temporarily during the initial
		 * bootstrap.
		 */
		if (!s->sheaf_capacity)
			pcs->main = &bootstrap_sheaf;
		else
			pcs->main = alloc_empty_sheaf(s, GFP_KERNEL, SLAB_ALLOC_DEFAULT);

		if (!pcs->main)
			return -ENOMEM;
	}

	return 0;
}

static struct kmem_cache *kmem_cache_node;

/*
 * early_kmem_cache_node_alloc() - 引导阶段在新 NUMA 节点上分配第一个 kmem_cache_node
 *
 * 背景：kmem_cache_node 自身也用 slab 分配，但在内核初始化极早期，
 * kmalloc_node() 还不可用（因为 kmem_cache_node 的 cache 本身尚未完全建立）。
 * 这里绕过正常分配路径，直接：
 *   1. new_slab() 向伙伴系统申请新 slab 页
 *   2. init_slab_obj_iter + next_slab_obj：手动取出第一个对象槽位
 *   3. 设置 inuse=1，构建剩余 freelist
 *   4. 用该对象初始化 kmem_cache_node 结构，注册到 kmem_cache_node->per_node[node]
 *
 * 为什么必须特殊处理？
 *   kmem_cache_node 是 SLUB 最基础的结构，它的 slab 用于管理所有其他 slab，
 *   包括 kmem_cache_node 自身的 slab。在这个"鸡生蛋"的引导阶段，
 *   必须手工构建第一个实例，之后才能进入正常分配循环。
 *
 * 节点不匹配时（slab_nid != node）打印错误但继续：
 *   内存紧张时伙伴系统可能返回其他节点的页，此时 kmem_cache_node 会位于
 *   "错误"节点，性能次优但功能仍然正确，避免启动失败。
 */
static void early_kmem_cache_node_alloc(int node)
{
	struct slab *slab;
	struct kmem_cache_node *n;
	struct slab_obj_iter iter;

	BUG_ON(kmem_cache_node->size < sizeof(struct kmem_cache_node));

	slab = new_slab(kmem_cache_node, GFP_NOWAIT, SLAB_ALLOC_DEFAULT, node);

	BUG_ON(!slab);
	if (slab_nid(slab) != node) {
		pr_err("SLUB: Unable to allocate memory from node %d\n", node);
		pr_err("SLUB: Allocating a useless per node structure in order to be able to continue\n");
	}

	init_slab_obj_iter(kmem_cache_node, slab, &iter, true);

	n = next_slab_obj(kmem_cache_node, &iter);
	BUG_ON(!n);

	slab->inuse = 1;
	build_slab_freelist(kmem_cache_node, slab, &iter);

#ifdef CONFIG_SLUB_DEBUG
	init_object(kmem_cache_node, n, SLUB_RED_ACTIVE);
#endif
	n = kasan_slab_alloc(kmem_cache_node, n, GFP_KERNEL, false);
	kmem_cache_node->per_node[node].node = n;
	init_kmem_cache_node(n);
	inc_slabs_node(kmem_cache_node, node, slab->objects);

	/*
	 * No locks need to be taken here as it has just been
	 * initialized and there is no concurrent access.
	 */
	__add_partial(n, slab, ADD_TO_HEAD);
}

static void free_kmem_cache_nodes(struct kmem_cache *s)
{
	int node;
	struct kmem_cache_node *n;

	for_each_node(node) {
		struct node_barn *barn = get_barn_node(s, node);

		if (!barn)
			continue;

		WARN_ON(barn->nr_full);
		WARN_ON(barn->nr_empty);
		kfree(barn);
		s->per_node[node].barn = NULL;
	}

	for_each_kmem_cache_node(s, node, n) {
		s->per_node[node].node = NULL;
		kmem_cache_free(kmem_cache_node, n);
	}
}

void __kmem_cache_release(struct kmem_cache *s)
{
	cache_random_seq_destroy(s);
	pcs_destroy(s);
#ifdef CONFIG_SLUB_STATS
	free_percpu(s->cpu_stats);
#endif
	free_kmem_cache_nodes(s);
}

/*
 * init_kmem_cache_nodes() - 为每个 NUMA 节点分配并初始化 per-node 管理结构
 *
 * 两阶段初始化（slab_state 控制行为）：
 *
 * 阶段 1（slab_state == DOWN，内核最早的引导期）：
 *   kmalloc 完全不可用。对每个有内存的节点（slab_nodes）调用
 *   early_kmem_cache_node_alloc()，手工从伙伴系统直接取 slab 页并切出
 *   第一个 kmem_cache_node 对象。此阶段不分配 barn（sheaf 机制未启用）。
 *
 * 阶段 2（slab_state > DOWN，正常运行时）：
 *   用 kmem_cache_alloc_node() 正常分配 kmem_cache_node 对象，
 *   用 kmalloc_node() 分配 node_barn 结构，两者都放到各自的 NUMA 本地节点。
 *
 * slab_nodes vs slab_barn_nodes：
 *   - slab_nodes：有内存的 NUMA 节点（N_MEMORY），才有 partial list 的意义
 *   - slab_barn_nodes：所有在线节点（N_ONLINE），包括无内存节点；
 *     barn 需要覆盖所有在线节点，因为 CPU 可能运行在 memoryless 节点上，
 *     也需要本地 barn 进行 sheaf 交换。
 *
 * cache_has_sheaves(s) 为 false 时（调试 cache / bootstrap cache）跳过 barn。
 */
static int init_kmem_cache_nodes(struct kmem_cache *s)
{
	int node;

	for_each_node_mask(node, slab_nodes) {
		struct kmem_cache_node *n;

		if (slab_state == DOWN) {
			early_kmem_cache_node_alloc(node);
			continue;
		}

		n = kmem_cache_alloc_node(kmem_cache_node,
						GFP_KERNEL, node);
		if (!n)
			return 0;

		init_kmem_cache_node(n);
		s->per_node[node].node = n;
	}

	if (slab_state == DOWN || !cache_has_sheaves(s))
		return 1;

	for_each_node_mask(node, slab_barn_nodes) {
		struct node_barn *barn;

		barn = kmalloc_node(sizeof(*barn), GFP_KERNEL, node);

		if (!barn)
			return 0;

		barn_init(barn);
		s->per_node[node].barn = barn;
	}

	return 1;
}

/*
 * calculate_sheaf_capacity() - 确定 per-CPU sheaf 的对象容量
 *
 * 以下情况 capacity = 0（不使用 sheaf）：
 *   - SLUB_TINY：嵌入式精简模式，去掉 sheaf 以节省内存
 *   - SLAB_DEBUG_FLAGS：调试 cache 不走快速路径，sheaf 没有意义
 *   - SLAB_NO_OBJ_EXT：引导阶段的基础 cache，sheaf 自身的分配会递归
 *   - SLAB_NOLEAKTRACE（如 kmemleak 的 object_cache）：
 *     sheaf 结构本身用 kmalloc 分配，分配时会触发 kmemleak 追踪，
 *     而 kmemleak 追踪又可能分配 kmemleak 对象，形成递归。
 *
 * 基础容量计算：
 *   参考旧版 SLUB 的 per-CPU partial slab 数量公式（除以 2 因为有两个 sheaf：
 *   main 和 spare），目标是使 barn/list_lock 的竞争程度与旧版类似。
 *   对象越大，每个 slab 页里的对象越少，sheaf 容量也相应减少，
 *   避免 sheaf 持有对象时间过长导致 slab 页无法被 shrink 回收。
 *
 * kmalloc 桶对齐优化：
 *   slab_sheaf 结构 + capacity 个指针的总大小，向上对齐到最近的 kmalloc 桶。
 *   这样 sheaf 自身的内存分配不会产生碎片，且 sheaf 能充分利用分配到的
 *   全部内存（避免 kmalloc(128) 实际拿到 128 字节却只用 100 字节的情况）。
 *
 * args->sheaf_capacity：允许调用方（通过 kmem_cache_create_args）显式指定
 *   最小容量，主要用于 kmem_cache_prefill_sheaf() 的使用者，
 *   避免因默认容量过小而频繁使用低效的大号 sheaf（oversize sheaf）。
 */
static unsigned int calculate_sheaf_capacity(struct kmem_cache *s,
					     struct kmem_cache_args *args)

{
	unsigned int capacity;
	size_t size;


	if (IS_ENABLED(CONFIG_SLUB_TINY) || s->flags & SLAB_DEBUG_FLAGS)
		return 0;

	/*
	 * Bootstrap caches can't have sheaves for now (SLAB_NO_OBJ_EXT).
	 * SLAB_NOLEAKTRACE caches (e.g., kmemleak's object_cache) must not
	 * have sheaves to avoid recursion when sheaf allocation triggers
	 * kmemleak tracking.
	 */
	if (s->flags & (SLAB_NO_OBJ_EXT | SLAB_NOLEAKTRACE))
		return 0;

	/*
	 * For now we use roughly similar formula (divided by two as there are
	 * two percpu sheaves) as what was used for percpu partial slabs, which
	 * should result in similar lock contention (barn or list_lock)
	 */
	if (s->size >= PAGE_SIZE)
		capacity = 4;
	else if (s->size >= 1024)
		capacity = 12;
	else if (s->size >= 256)
		capacity = 26;
	else
		capacity = 60;

	/* Increment capacity to make sheaf exactly a kmalloc size bucket */
	size = struct_size_t(struct slab_sheaf, objects, capacity);
	size = kmalloc_size_roundup(size);
	capacity = (size - struct_size_t(struct slab_sheaf, objects, 0)) / sizeof(void *);

	/*
	 * Respect an explicit request for capacity that's typically motivated by
	 * expected maximum size of kmem_cache_prefill_sheaf() to not end up
	 * using low-performance oversize sheaves
	 */
	return max(capacity, args->sheaf_capacity);
}

/*
 * calculate_sizes() - 确定 slab 对象的内存布局（最核心的初始化函数之一）
 *
 * 这个函数决定了 slab 内每个对象槽位的精确内存布局，是影响性能、安全性、
 * 调试能力的关键配置点。布局从低到高依次为：
 *
 *   [left red zone]  ← red_left_pad（SLAB_RED_ZONE）
 *   [user data]      ← object_size
 *   [right red zone] ← inuse - object_size（SLAB_RED_ZONE，对齐填充）
 *   [freepointer]    ← offset（可能在对象内部 或 inuse 处之后）
 *   [track × 2]      ← SLAB_STORE_USER
 *   [orig_size]      ← SLAB_KMALLOC + SLAB_STORE_USER
 *   [KASAN metadata] ← kasan_cache_create 追加
 *   [alignment pad]  ← ALIGN(size, s->align) → s->size
 *
 * freepointer 位置的三种决策：
 *
 *   1. 强制放在对象外（s->offset = size → s->offset >= s->inuse）：
 *      条件：SLAB_TYPESAFE_BY_RCU、SLAB_POISON、有 ctor、或特殊 red zone 情况
 *      原因：
 *        - TYPESAFE_BY_RCU：RCU 宽限期内对象内存仍有效，ctor 可能访问对象任意字段，
 *          若 freepointer 在对象内会被 ctor 覆写，破坏 freelist
 *        - POISON：poison 填充整个 object_size 区域，若 freepointer 在内会被覆写
 *        - 小对象 red zone / orig_size red zone：右侧 red zone 可能延伸到 freepointer 处
 *
 *   2. 使用调用方指定的 freeptr_offset（args->use_freeptr_offset = true）：
 *      专为有特殊需求的 cache 设计（如嵌入特定字段的场景）
 *
 *   3. 默认：放在对象中间（s->offset = ALIGN_DOWN(object_size/2, sizeof(void*)）
 *      安全考虑：越界访问通常从对象边界开始，放中间减少被随机越界覆写的概率
 *
 * __OBJECT_POISON 标志的条件：
 *   SLAB_POISON 开启 且 没有 TYPESAFE_BY_RCU 且 没有 ctor
 *   原因：有 RCU 或 ctor 时，对象在"空闲"状态下可能仍被访问，
 *   poison 填充会与这些访问冲突（poison 会破坏 ctor 预设的初始值）。
 *
 * 右侧 red zone 保证非空的特殊处理：
 *   若 ALIGN(object_size, sizeof(void*)) == object_size（无自然填充），
 *   强制多加一个 void* 的空间作为右侧 red zone，确保有字节可以验证越界。
 *
 * SLAB_OBJ_EXT_IN_OBJ：
 *   若 cache 不可合并（unmergeable）且对齐后的尾部 padding ≥ sizeof(slabobj_ext)，
 *   可以把 slabobj_ext 直接存在对象尾部的 padding 里（零额外开销）。
 */
static int calculate_sizes(struct kmem_cache_args *args, struct kmem_cache *s)
{
	slab_flags_t flags = s->flags;
	unsigned int size = s->object_size;
	unsigned int aligned_size;
	unsigned int order;

	/*
	 * Round up object size to the next word boundary. We can only
	 * place the free pointer at word boundaries and this determines
	 * the possible location of the free pointer.
	 */
	size = ALIGN(size, sizeof(void *));

#ifdef CONFIG_SLUB_DEBUG
	/*
	 * Determine if we can poison the object itself. If the user of
	 * the slab may touch the object after free or before allocation
	 * then we should never poison the object itself.
	 */
	if ((flags & SLAB_POISON) && !(flags & SLAB_TYPESAFE_BY_RCU) &&
			!s->ctor)
		s->flags |= __OBJECT_POISON;
	else
		s->flags &= ~__OBJECT_POISON;


	/*
	 * If we are Redzoning and there is no space between the end of the
	 * object and the following fields, add one word so the right Redzone
	 * is non-empty.
	 */
	if ((flags & SLAB_RED_ZONE) && size == s->object_size)
		size += sizeof(void *);
#endif

	/*
	 * With that we have determined the number of bytes in actual use
	 * by the object and redzoning.
	 */
	s->inuse = size;

	if (((flags & SLAB_TYPESAFE_BY_RCU) && !args->use_freeptr_offset) ||
	    (flags & SLAB_POISON) ||
	    (s->ctor && !args->use_freeptr_offset) ||
	    ((flags & SLAB_RED_ZONE) &&
	     (s->object_size < sizeof(void *) || slub_debug_orig_size(s)))) {
		/*
		 * Relocate free pointer after the object if it is not
		 * permitted to overwrite the first word of the object on
		 * kmem_cache_free.
		 *
		 * This is the case if we do RCU, have a constructor, are
		 * poisoning the objects, or are redzoning an object smaller
		 * than sizeof(void *) or are redzoning an object with
		 * slub_debug_orig_size() enabled, in which case the right
		 * redzone may be extended.
		 *
		 * The assumption that s->offset >= s->inuse means free
		 * pointer is outside of the object is used in the
		 * freeptr_outside_object() function. If that is no
		 * longer true, the function needs to be modified.
		 */
		s->offset = size;
		size += sizeof(void *);
	} else if (((flags & SLAB_TYPESAFE_BY_RCU) || s->ctor) &&
			args->use_freeptr_offset) {
		s->offset = args->freeptr_offset;
	} else {
		/*
		 * Store freelist pointer near middle of object to keep
		 * it away from the edges of the object to avoid small
		 * sized over/underflows from neighboring allocations.
		 */
		s->offset = ALIGN_DOWN(s->object_size / 2, sizeof(void *));
	}

#ifdef CONFIG_SLUB_DEBUG
	if (flags & SLAB_STORE_USER) {
		/*
		 * Need to store information about allocs and frees after
		 * the object.
		 */
		size += 2 * sizeof(struct track);

		/* Save the original kmalloc request size */
		if (flags & SLAB_KMALLOC)
			size += sizeof(unsigned long);
	}
#endif

	kasan_cache_create(s, &size, &s->flags);
#ifdef CONFIG_SLUB_DEBUG
	if (flags & SLAB_RED_ZONE) {
		/*
		 * Add some empty padding so that we can catch
		 * overwrites from earlier objects rather than let
		 * tracking information or the free pointer be
		 * corrupted if a user writes before the start
		 * of the object.
		 */
		size += sizeof(void *);

		s->red_left_pad = sizeof(void *);
		s->red_left_pad = ALIGN(s->red_left_pad, s->align);
		size += s->red_left_pad;
	}
#endif

	/*
	 * SLUB stores one object immediately after another beginning from
	 * offset 0. In order to align the objects we have to simply size
	 * each object to conform to the alignment.
	 */
	aligned_size = ALIGN(size, s->align);
#if defined(CONFIG_SLAB_OBJ_EXT) && defined(CONFIG_64BIT)
	if (slab_args_unmergeable(args, s->flags) &&
			(aligned_size - size >= sizeof(struct slabobj_ext)))
		s->flags |= SLAB_OBJ_EXT_IN_OBJ;
#endif
	size = aligned_size;

	s->size = size;
	s->reciprocal_size = reciprocal_value(size);
	order = calculate_order(size);

	if ((int)order < 0)
		return 0;

	s->allocflags = __GFP_COMP;

	if (s->flags & SLAB_CACHE_DMA)
		s->allocflags |= GFP_DMA;

	if (s->flags & SLAB_CACHE_DMA32)
		s->allocflags |= GFP_DMA32;

	if (s->flags & SLAB_RECLAIM_ACCOUNT)
		s->allocflags |= __GFP_RECLAIMABLE;

	/*
	 * For KMALLOC_NORMAL caches we enable sheaves later by
	 * bootstrap_kmalloc_sheaves() to avoid recursion
	 */
	if (!is_kmalloc_normal(s))
		s->sheaf_capacity = calculate_sheaf_capacity(s, args);

	/*
	 * Determine the number of objects per slab
	 */
	s->oo = oo_make(order, size);
	s->min = oo_make(get_order(size), size);

	return !!oo_objects(s->oo);
}

static void list_slab_objects(struct kmem_cache *s, struct slab *slab)
{
#ifdef CONFIG_SLUB_DEBUG
	void *addr = slab_address(slab);
	void *p;

	if (!slab_add_kunit_errors())
		slab_bug(s, "Objects remaining on __kmem_cache_shutdown()");

	spin_lock(&object_map_lock);
	__fill_map(object_map, s, slab);

	for_each_object(p, s, addr, slab->objects) {

		if (!test_bit(__obj_to_index(s, addr, p), object_map)) {
			if (slab_add_kunit_errors())
				continue;
			pr_err("Object 0x%p @offset=%tu\n", p, p - addr);
			print_tracking(s, p);
		}
	}
	spin_unlock(&object_map_lock);

	__slab_err(slab);
#endif
}

/*
 * Attempt to free all partial slabs on a node.
 * This is called from __kmem_cache_shutdown(). We must take list_lock
 * because sysfs file might still access partial list after the shutdowning.
 */
static void free_partial(struct kmem_cache *s, struct kmem_cache_node *n)
{
	LIST_HEAD(discard);
	struct slab *slab, *h;

	BUG_ON(irqs_disabled());
	spin_lock_irq(&n->list_lock);
	list_for_each_entry_safe(slab, h, &n->partial, slab_list) {
		if (!slab->inuse) {
			remove_partial(n, slab);
			list_add(&slab->slab_list, &discard);
		} else {
			list_slab_objects(s, slab);
		}
	}
	spin_unlock_irq(&n->list_lock);

	list_for_each_entry_safe(slab, h, &discard, slab_list)
		discard_slab(s, slab);
}

bool __kmem_cache_empty(struct kmem_cache *s)
{
	int node;
	struct kmem_cache_node *n;

	for_each_kmem_cache_node(s, node, n)
		if (n->nr_partial || node_nr_slabs(n))
			return false;
	return true;
}

/*
 * Release all resources used by a slab cache.
 */
int __kmem_cache_shutdown(struct kmem_cache *s)
{
	int node;
	struct kmem_cache_node *n;

	flush_all_cpus_locked(s);

	/* we might have rcu sheaves in flight */
	if (cache_has_sheaves(s))
		rcu_barrier();

	for_each_node(node) {
		struct node_barn *barn = get_barn_node(s, node);

		if (barn)
			barn_shrink(s, barn);
	}

	/* Attempt to free all objects */
	for_each_kmem_cache_node(s, node, n) {
		free_partial(s, n);
		if (n->nr_partial || node_nr_slabs(n))
			return 1;
	}
	return 0;
}

#ifdef CONFIG_PRINTK
void __kmem_obj_info(struct kmem_obj_info *kpp, void *object, struct slab *slab)
{
	void *base;
	int __maybe_unused i;
	unsigned int objnr;
	void *objp;
	void *objp0;
	struct kmem_cache *s = slab->slab_cache;
	struct track __maybe_unused *trackp;

	kpp->kp_ptr = object;
	kpp->kp_slab = slab;
	kpp->kp_slab_cache = s;
	base = slab_address(slab);
	objp0 = kasan_reset_tag(object);
#ifdef CONFIG_SLUB_DEBUG
	objp = restore_red_left(s, objp0);
#else
	objp = objp0;
#endif
	objnr = obj_to_index(s, slab, objp);
	kpp->kp_data_offset = (unsigned long)((char *)objp0 - (char *)objp);
	objp = base + s->size * objnr;
	kpp->kp_objp = objp;
	if (WARN_ON_ONCE(objp < base || objp >= base + slab->objects * s->size
			 || (objp - base) % s->size) ||
	    !(s->flags & SLAB_STORE_USER))
		return;
#ifdef CONFIG_SLUB_DEBUG
	objp = fixup_red_left(s, objp);
	trackp = get_track(s, objp, TRACK_ALLOC);
	kpp->kp_ret = (void *)trackp->addr;
#ifdef CONFIG_STACKDEPOT
	{
		depot_stack_handle_t handle;
		unsigned long *entries;
		unsigned int nr_entries;

		handle = READ_ONCE(trackp->handle);
		if (handle) {
			nr_entries = stack_depot_fetch(handle, &entries);
			for (i = 0; i < KS_ADDRS_COUNT && i < nr_entries; i++)
				kpp->kp_stack[i] = (void *)entries[i];
		}

		trackp = get_track(s, objp, TRACK_FREE);
		handle = READ_ONCE(trackp->handle);
		if (handle) {
			nr_entries = stack_depot_fetch(handle, &entries);
			for (i = 0; i < KS_ADDRS_COUNT && i < nr_entries; i++)
				kpp->kp_free_stack[i] = (void *)entries[i];
		}
	}
#endif
#endif
}
#endif

/********************************************************************
 *		Kmalloc subsystem
 *******************************************************************/

static int __init setup_slub_min_order(const char *str, const struct kernel_param *kp)
{
	int ret;

	ret = kstrtouint(str, 0, &slub_min_order);
	if (ret)
		return ret;

	if (slub_min_order > slub_max_order)
		slub_max_order = slub_min_order;

	return 0;
}

static const struct kernel_param_ops param_ops_slab_min_order __initconst = {
	.set = setup_slub_min_order,
};
__core_param_cb(slab_min_order, &param_ops_slab_min_order, &slub_min_order, 0);
__core_param_cb(slub_min_order, &param_ops_slab_min_order, &slub_min_order, 0);

static int __init setup_slub_max_order(const char *str, const struct kernel_param *kp)
{
	int ret;

	ret = kstrtouint(str, 0, &slub_max_order);
	if (ret)
		return ret;

	slub_max_order = min_t(unsigned int, slub_max_order, MAX_PAGE_ORDER);

	if (slub_min_order > slub_max_order)
		slub_min_order = slub_max_order;

	return 0;
}

static const struct kernel_param_ops param_ops_slab_max_order __initconst = {
	.set = setup_slub_max_order,
};
__core_param_cb(slab_max_order, &param_ops_slab_max_order, &slub_max_order, 0);
__core_param_cb(slub_max_order, &param_ops_slab_max_order, &slub_max_order, 0);

core_param(slab_min_objects, slub_min_objects, uint, 0);
core_param(slub_min_objects, slub_min_objects, uint, 0);

#ifdef CONFIG_NUMA
static int __init setup_slab_strict_numa(const char *str, const struct kernel_param *kp)
{
	if (nr_node_ids > 1) {
		static_branch_enable(&strict_numa);
		pr_info("SLUB: Strict NUMA enabled.\n");
	} else {
		pr_warn("slab_strict_numa parameter set on non NUMA system.\n");
	}

	return 0;
}

static const struct kernel_param_ops param_ops_slab_strict_numa __initconst = {
	.flags = KERNEL_PARAM_OPS_FL_NOARG,
	.set = setup_slab_strict_numa,
};
__core_param_cb(slab_strict_numa, &param_ops_slab_strict_numa, NULL, 0);
#endif


#ifdef CONFIG_HARDENED_USERCOPY
/*
 * Rejects incorrectly sized objects and objects that are to be copied
 * to/from userspace but do not fall entirely within the containing slab
 * cache's usercopy region.
 *
 * Returns NULL if check passes, otherwise const char * to name of cache
 * to indicate an error.
 */
void __check_heap_object(const void *ptr, unsigned long n,
			 const struct slab *slab, bool to_user)
{
	struct kmem_cache *s;
	unsigned int offset;
	bool is_kfence = is_kfence_address(ptr);

	ptr = kasan_reset_tag(ptr);

	/* Find object and usable object size. */
	s = slab->slab_cache;

	/* Reject impossible pointers. */
	if (ptr < slab_address(slab))
		usercopy_abort("SLUB object not in SLUB page?!", NULL,
			       to_user, 0, n);

	/* Find offset within object. */
	if (is_kfence)
		offset = ptr - kfence_object_start(ptr);
	else
		offset = (ptr - slab_address(slab)) % s->size;

	/* Adjust for redzone and reject if within the redzone. */
	if (!is_kfence && kmem_cache_debug_flags(s, SLAB_RED_ZONE)) {
		if (offset < s->red_left_pad)
			usercopy_abort("SLUB object in left red zone",
				       s->name, to_user, offset, n);
		offset -= s->red_left_pad;
	}

	/* Allow address range falling entirely within usercopy region. */
	if (offset >= s->useroffset &&
	    offset - s->useroffset <= s->usersize &&
	    n <= s->useroffset - offset + s->usersize)
		return;

	usercopy_abort("SLUB object", s->name, to_user, offset, n);
}
#endif /* CONFIG_HARDENED_USERCOPY */

/*
 * SHRINK_PROMOTE_MAX：shrink 时"提升到 partial list 头部"的分组数量上限。
 * 按 free（空闲对象数）分 32 组，free=1（最满）排最前，free=32 排最后。
 * 超过 32 个空闲对象的 slab 不参与提升，位置不变。
 */
#define SHRINK_PROMOTE_MAX 32

/*
 * __kmem_cache_do_shrink() - 回收空 slab 并整理 partial list 顺序
 *
 * 背景：长时间运行后，partial list 可能积累大量"几乎全空"的 slab，
 * 这些 slab 因为还有少量已分配对象而无法归还给伙伴系统，浪费内存。
 * shrink 的目标是：让内存尽快集中到少数几个 slab 上，使其他 slab 能变全空。
 *
 * 两阶段操作：
 *
 * 阶段 1：barn_shrink()（对每个 NUMA 节点）
 *   清空 barn 中所有满/空 sheaf，让对象流回 slab freelist，
 *   增加 slab 页变全空的机会。
 *
 * 阶段 2：partial list 整理（对每个 NUMA 节点的 kmem_cache_node）
 *   遍历 partial list，按空闲对象数分类：
 *   - full empty（free == objects）→ 加入 discard，稍后 free_slab()
 *   - free ∈ [1, SHRINK_PROMOTE_MAX]：按 free 分桶，free 越小（slab 越满）
 *     排在越靠前的桶
 *   - free > SHRINK_PROMOTE_MAX：位置不变
 *
 *   最后把各桶按 free 从小到大（最满优先）拼接到 partial list 头部。
 *   效果：后续分配优先使用接近满的 slab，让它们快速变全满（脱离 partial list），
 *   而空/半空的 slab 聚集在尾部，在下次对象释放时更容易变全空并被 discard。
 *
 * barrier()：防止编译器缓存 slab->inuse，因为持 list_lock 期间可能有并发 free
 * 修改 inuse（通过 CAS 无锁路径），需要每次读取最新值。
 *
 * 返回 1 表示仍有未释放的 slab（非零 nr_slabs），0 表示 cache 完全清空。
 */
static int __kmem_cache_do_shrink(struct kmem_cache *s)
{
	int node;
	int i;
	struct kmem_cache_node *n;
	struct slab *slab;
	struct slab *t;
	struct list_head discard;
	struct list_head promote[SHRINK_PROMOTE_MAX];
	unsigned long flags;
	int ret = 0;

	for_each_node(node) {
		struct node_barn *barn = get_barn_node(s, node);

		if (barn)
			barn_shrink(s, barn);
	}

	for_each_kmem_cache_node(s, node, n) {
		INIT_LIST_HEAD(&discard);
		for (i = 0; i < SHRINK_PROMOTE_MAX; i++)
			INIT_LIST_HEAD(promote + i);

		spin_lock_irqsave(&n->list_lock, flags);

		/*
		 * Build lists of slabs to discard or promote.
		 *
		 * Note that concurrent frees may occur while we hold the
		 * list_lock. slab->inuse here is the upper limit.
		 */
		list_for_each_entry_safe(slab, t, &n->partial, slab_list) {
			int free = slab->objects - slab->inuse;

			/* Do not reread slab->inuse */
			barrier();

			/* We do not keep full slabs on the list */
			BUG_ON(free <= 0);

			if (free == slab->objects) {
				list_move(&slab->slab_list, &discard);
				clear_node_partial_state(n, slab);
				dec_slabs_node(s, node, slab->objects);
			} else if (free <= SHRINK_PROMOTE_MAX)
				list_move(&slab->slab_list, promote + free - 1);
		}

		/*
		 * Promote the slabs filled up most to the head of the
		 * partial list.
		 */
		for (i = SHRINK_PROMOTE_MAX - 1; i >= 0; i--)
			list_splice(promote + i, &n->partial);

		spin_unlock_irqrestore(&n->list_lock, flags);

		/* Release empty slabs */
		list_for_each_entry_safe(slab, t, &discard, slab_list)
			free_slab(s, slab);

		if (node_nr_slabs(n))
			ret = 1;
	}

	return ret;
}

/*
 * __kmem_cache_shrink() - 公开的 shrink 入口（flush + shrink 两步）
 *
 * 先 flush_all() 将所有 CPU 的 sheaf 中对象刷回 slab freelist，
 * 再 __kmem_cache_do_shrink() 整理 partial list 并回收空 slab。
 * 这是 kmem_cache_shrink()（用户可调接口）的最终实现。
 */
int __kmem_cache_shrink(struct kmem_cache *s)
{
	flush_all(s);
	return __kmem_cache_do_shrink(s);
}

/*
 * slab_mem_going_offline_callback() - NUMA 节点下线时清理所有 cache
 *
 * 节点下线（NODE_REMOVING_LAST_MEMORY）时，需要将该节点上的所有 slab 对象
 * 迁移或释放，否则内存无法被移除。对每个 cache 执行 flush_all + shrink，
 * 尽量清空节点上的 slab 页。
 * 注意：flush_all_cpus_locked 比 flush_all 少一层 cpus_read_lock，
 * 因为调用方已经持有 cpu_hotplug_lock（由 hotplug 框架保证）。
 */
static int slab_mem_going_offline_callback(void)
{
	struct kmem_cache *s;

	mutex_lock(&slab_mutex);
	list_for_each_entry(s, &slab_caches, list) {
		flush_all_cpus_locked(s);
		__kmem_cache_do_shrink(s);
	}
	mutex_unlock(&slab_mutex);

	return 0;
}

/*
 * slab_mem_going_online_callback() - NUMA 节点上线时为所有 cache 分配 per-node 结构
 *
 * 新节点上线（NODE_ADDING_FIRST_MEMORY）时，该节点还没有内存可用，
 * kmem_cache_node 和 barn 必须从其他节点分配（此时 kmem_cache_alloc_node
 * 会回退到其他节点）。
 *
 * 两步处理：
 *   1. 为每个 cache 分配 node_barn（有 sheaf 的 cache 才需要）和 kmem_cache_node，
 *      注册到 s->per_node[nid]。幂等：若之前已上线过此节点，结构已存在则跳过。
 *   2. 将 nid 加入 slab_nodes 和 slab_barn_nodes，此后新建的 cache 也会
 *      自动为该节点初始化 per-node 结构。
 *
 * XXX：如注释所说，kmem_cache_alloc_node 在此时会回退到其他节点分配，
 * 所以 node 上的第一个 kmem_cache_node 对象可能来自远端节点（性能次优但正确）。
 */
static int slab_mem_going_online_callback(int nid)
{
	struct kmem_cache_node *n;
	struct kmem_cache *s;
	int ret = 0;

	/*
	 * We are bringing a node online. No memory is available yet. We must
	 * allocate a kmem_cache_node structure in order to bring the node
	 * online.
	 */
	mutex_lock(&slab_mutex);
	list_for_each_entry(s, &slab_caches, list) {
		struct node_barn *barn = NULL;

		/*
		 * The structure may already exist if the node was previously
		 * onlined and offlined.
		 */
		if (get_node(s, nid))
			continue;

		if (cache_has_sheaves(s) && !get_barn_node(s, nid)) {

			barn = kmalloc_node(sizeof(*barn), GFP_KERNEL, nid);

			if (!barn) {
				ret = -ENOMEM;
				goto out;
			}
		}

		/*
		 * XXX: kmem_cache_alloc_node will fallback to other nodes
		 *      since memory is not yet available from the node that
		 *      is brought up.
		 */
		n = kmem_cache_alloc(kmem_cache_node, GFP_KERNEL);
		if (!n) {
			kfree(barn);
			ret = -ENOMEM;
			goto out;
		}

		init_kmem_cache_node(n);
		s->per_node[nid].node = n;

		if (barn) {
			barn_init(barn);
			s->per_node[nid].barn = barn;
		}
	}
	/*
	 * Any cache created after this point will also have kmem_cache_node
	 * and barn initialized for the new node.
	 */
	node_set(nid, slab_nodes);
	node_set(nid, slab_barn_nodes);
out:
	mutex_unlock(&slab_mutex);
	return ret;
}

static int slab_memory_callback(struct notifier_block *self,
				unsigned long action, void *arg)
{
	struct node_notify *nn = arg;
	int nid = nn->nid;
	int ret = 0;

	switch (action) {
	case NODE_ADDING_FIRST_MEMORY:
		ret = slab_mem_going_online_callback(nid);
		break;
	case NODE_REMOVING_LAST_MEMORY:
		ret = slab_mem_going_offline_callback();
		break;
	}
	if (ret)
		ret = notifier_from_errno(ret);
	else
		ret = NOTIFY_OK;
	return ret;
}

/********************************************************************
 *			Basic setup of slabs
 *******************************************************************/

/*
 * Used for early kmem_cache structures that were allocated using
 * the page allocator. Allocate them properly then fix up the pointers
 * that may be pointing to the wrong kmem_cache structure.
 */

static struct kmem_cache * __init bootstrap(struct kmem_cache *static_cache)
{
	int node;
	struct kmem_cache *s = kmem_cache_zalloc(kmem_cache, GFP_NOWAIT);
	struct kmem_cache_node *n;

	memcpy(s, static_cache, kmem_cache->object_size);

	for_each_kmem_cache_node(s, node, n) {
		struct slab *p;

		list_for_each_entry(p, &n->partial, slab_list)
			p->slab_cache = s;

#ifdef CONFIG_SLUB_DEBUG
		list_for_each_entry(p, &n->full, slab_list)
			p->slab_cache = s;
#endif
	}
	list_add(&s->list, &slab_caches);
	return s;
}

/*
 * bootstrap_cache_sheaves() / bootstrap_kmalloc_sheaves() -
 * 为 kmalloc cache 补充初始化 sheaf 和 barn
 *
 * 背景：sheaf 和 barn 自身需要用 kmalloc 分配，而 kmalloc cache 是
 * 在内核引导极早期创建的——当时 kmalloc 还不可用，所以 do_kmem_cache_create
 * 的 init_percpu_sheaves / init_kmem_cache_nodes 对 kmalloc cache 只创建了
 * bootstrap_sheaf（零容量占位符），没有真正分配 sheaf 和 barn。
 *
 * 等 kmalloc 可用后（slab_state >= UP），在 kmem_cache_init() 末尾调用
 * bootstrap_kmalloc_sheaves()，遍历所有 kmalloc cache，逐个调用
 * bootstrap_cache_sheaves 补分配真正的 sheaf 和 barn。
 *
 * capacity 为 0 时（调试模式或 SLUB_TINY）跳过，bootstrap_sheaf 继续充当占位符。
 *
 * 失败时直接 panic：此时内核还处于 __init 阶段，内存不足是致命错误，
 * 不存在"优雅降级"的余地。
 */
static void __init bootstrap_cache_sheaves(struct kmem_cache *s)
{
	struct kmem_cache_args empty_args = {};
	unsigned int capacity;
	bool failed = false;
	int node, cpu;

	capacity = calculate_sheaf_capacity(s, &empty_args);

	/* capacity can be 0 due to debugging or SLUB_TINY */
	if (!capacity)
		return;

	for_each_node_mask(node, slab_barn_nodes) {
		struct node_barn *barn;

		barn = kmalloc_node(sizeof(*barn), GFP_KERNEL, node);

		if (!barn) {
			failed = true;
			goto out;
		}

		barn_init(barn);
		s->per_node[node].barn = barn;
	}

	for_each_possible_cpu(cpu) {
		struct slub_percpu_sheaves *pcs;

		pcs = per_cpu_ptr(s->cpu_sheaves, cpu);

		pcs->main = __alloc_empty_sheaf(s, GFP_KERNEL,
				SLAB_ALLOC_DEFAULT, capacity);

		if (!pcs->main) {
			failed = true;
			break;
		}
	}

out:
	/*
	 * It's still early in boot so treat this like same as a failure to
	 * create the kmalloc cache in the first place
	 */
	if (failed)
		panic("Out of memory when creating kmem_cache %s\n", s->name);

	s->sheaf_capacity = capacity;
}

static void __init bootstrap_kmalloc_sheaves(void)
{
	enum kmalloc_cache_type type;

	for (type = KMALLOC_NORMAL; type <= KMALLOC_PARTITION_END; type++) {
		for (int idx = 0; idx < KMALLOC_SHIFT_HIGH + 1; idx++) {
			if (kmalloc_caches[type][idx])
				bootstrap_cache_sheaves(kmalloc_caches[type][idx]);
		}
	}
}

/*
 * kmem_cache_init() - SLUB 分配器的内核引导初始化入口
 *
 * 这是整个 SLUB 系统的"鸡生蛋"问题解决过程。SLUB 需要 slab 内存来管理
 * slab 内存，必须用临时的静态结构打破循环依赖。
 *
 * 引导序列（slab_state 状态机）：
 *
 * 阶段 1（DOWN → PARTIAL）：
 *   用栈上静态结构（boot_kmem_cache_node / boot_kmem_cache）作为临时 cache。
 *   create_boot_cache 用这些静态结构调用 do_kmem_cache_create：
 *     - kmem_cache_node cache：此时 slab_state==DOWN，init_kmem_cache_nodes
 *       走 early_kmem_cache_node_alloc 路径（直接从伙伴系统分配，不依赖 kmalloc）
 *   注册 NUMA hotplug notifier（slab_memory_callback）
 *   slab_state = PARTIAL：表示可以分配 per-node 结构了
 *
 * 阶段 2（PARTIAL → UP）：
 *   create_boot_cache kmem_cache cache（用于管理所有 kmem_cache 对象）
 *   bootstrap()：将栈上静态结构"迁移"到真正的 slab 管理中：
 *     - 用新 cache 分配真正的 kmem_cache 结构，复制 boot_kmem_cache 的内容
 *     - 更新 slab 页的 slab_cache 指针指向新的 kmem_cache 对象
 *     - 加入全局 slab_caches 链表
 *   setup_kmalloc_cache_index_table：初始化 kmalloc size → cache 的映射表
 *   create_kmalloc_caches：创建所有 kmalloc-N cache（8, 16, 32, ... 字节）
 *   bootstrap_kmalloc_sheaves：为 kmalloc cache 补充分配 sheaf 和 barn
 *
 * debug_guardpage_minorder()：若开启了调试 guard page，限制 slub_max_order=0，
 *   确保每个 slab 只占一页，与 guard page 兼容。
 *
 * SLAB_NO_OBJ_EXT：bootstrap cache 不使用 obj_exts，避免在 obj_exts 自身的
 *   cache 未就绪时发生递归分配。
 */
void __init kmem_cache_init(void)
{
	static __initdata struct kmem_cache boot_kmem_cache,
		boot_kmem_cache_node;
	int node;

	if (debug_guardpage_minorder())
		slub_max_order = 0;

	/* Inform pointer hashing choice about slub debugging state. */
	hash_pointers_finalize(__slub_debug_enabled());

	kmem_cache_node = &boot_kmem_cache_node;
	kmem_cache = &boot_kmem_cache;

	/*
	 * Initialize the nodemask for which we will allocate per node
	 * structures. Here we don't need taking slab_mutex yet.
	 */
	for_each_node_state(node, N_MEMORY)
		node_set(node, slab_nodes);

	for_each_online_node(node)
		node_set(node, slab_barn_nodes);

	create_boot_cache(kmem_cache_node, "kmem_cache_node",
			sizeof(struct kmem_cache_node),
			SLAB_HWCACHE_ALIGN | SLAB_NO_OBJ_EXT, 0, 0);

	hotplug_node_notifier(slab_memory_callback, SLAB_CALLBACK_PRI);

	/* Able to allocate the per node structures */
	slab_state = PARTIAL;

	create_boot_cache(kmem_cache, "kmem_cache",
			offsetof(struct kmem_cache, per_node) +
				nr_node_ids * sizeof(struct kmem_cache_per_node_ptrs),
			SLAB_HWCACHE_ALIGN | SLAB_NO_OBJ_EXT, 0, 0);

	kmem_cache = bootstrap(&boot_kmem_cache);
	kmem_cache_node = bootstrap(&boot_kmem_cache_node);

	/* Now we can use the kmem_cache to allocate kmalloc slabs */
	setup_kmalloc_cache_index_table();
	create_kmalloc_caches();

	bootstrap_kmalloc_sheaves();

	/* Setup random freelists for each cache */
	init_freelist_randomization();

	cpuhp_setup_state_nocalls(CPUHP_SLUB_DEAD, "slub:dead", slub_cpu_setup,
				  slub_cpu_dead);

	pr_info("SLUB: HWalign=%d, Order=%u-%u, MinObjects=%u, CPUs=%u, Nodes=%u\n",
		cache_line_size(),
		slub_min_order, slub_max_order, slub_min_objects,
		nr_cpu_ids, nr_node_ids);
}

void __init kmem_cache_init_late(void)
{
	flushwq = alloc_workqueue("slub_flushwq", WQ_MEM_RECLAIM | WQ_PERCPU,
				  0);
	WARN_ON(!flushwq);
#ifdef CONFIG_SLAB_FREELIST_RANDOM
	prandom_init_once(&slab_rnd_state);
#endif
}

/*
 * do_kmem_cache_create() - kmem_cache 初始化核心（所有 kmem_cache_create* 的终点）
 *
 * 此函数在已分配好的 kmem_cache 结构上完成全部字段初始化。调用方
 *（create_cache() 或 kmem_cache_init()）负责分配结构体本身。
 *
 * 初始化步骤（顺序敏感）：
 *
 * 1. 基本字段：name、object_size、flags（含 slab_debug 注入）、align、ctor
 *    s->random：SLAB_FREELIST_HARDENED 的 per-cache 随机种子，用于混淆 freepointer
 *
 * 2. calculate_sizes()：确定对象布局（inuse、offset、size、oo、min）
 *    disable_higher_order_debug 二次计算：
 *    若调试元数据导致 slab order 升高（calculate_sizes 后 size 变大），
 *    且用户设置了 slab_debug=O（optimize），则禁用 DEBUG_METADATA_FLAGS
 *    后重新计算，避免调试选项意外导致大量内存浪费。
 *
 * 3. __CMPXCHG_DOUBLE：若硬件支持 freelist_aba（cmpxchg_double），启用快速路径
 *
 * 4. min_partial：公式 ilog2(size)/2，对象越大每 slab 对象越少，
 *    需要维持更多备用 partial slab（避免频繁向伙伴系统申请/归还）；
 *    限制在 [MIN_PARTIAL, MAX_PARTIAL] 内
 *
 * 5. cpu_sheaves：alloc_percpu，为每个 CPU 分配 slub_percpu_sheaves 结构
 *
 * 6. remote_node_defrag_ratio=1000：默认允许跨节点碎片整理（见 get_from_any_partial）
 *
 * 7. init_cache_random_seq()：SLAB_FREELIST_RANDOM 的随机化 freelist 顺序表
 *    只在 slab_state >= UP 后才初始化（UP 之前 get_random_* 不可用）
 *
 * 8. init_kmem_cache_nodes()：分配 per-node kmem_cache_node + node_barn
 *
 * 9. init_percpu_sheaves()：为每个 CPU 分配初始 sheaf
 *
 * 10. sysfs_slab_add / debugfs_slab_add：注册到 /sys/kernel/slab/ 和 debugfs
 *    失败不致命（只打印 warning，继续创建 cache）
 *
 * 失败时调用 __kmem_cache_release() 释放已分配的资源（pcs、nodes、stats 等）。
 */
int do_kmem_cache_create(struct kmem_cache *s, const char *name,
			 unsigned int size, struct kmem_cache_args *args,
			 slab_flags_t flags)
{
	int err = -EINVAL;

	s->name = name;
	s->size = s->object_size = size;

	s->flags = kmem_cache_flags(flags, s->name);
#ifdef CONFIG_SLAB_FREELIST_HARDENED
	s->random = get_random_long();
#endif
	s->align = args->align;
	s->ctor = args->ctor;
#ifdef CONFIG_HARDENED_USERCOPY
	s->useroffset = args->useroffset;
	s->usersize = args->usersize;
#endif

	if (!calculate_sizes(args, s))
		goto out;
	if (disable_higher_order_debug) {
		/*
		 * Disable debugging flags that store metadata if the min slab
		 * order increased.
		 */
		if (get_order(s->size) > get_order(s->object_size)) {
			s->flags &= ~DEBUG_METADATA_FLAGS;
			s->offset = 0;
			if (!calculate_sizes(args, s))
				goto out;
		}
	}

#ifdef system_has_freelist_aba
	if (system_has_freelist_aba() && !(s->flags & SLAB_NO_CMPXCHG)) {
		/* Enable fast mode */
		s->flags |= __CMPXCHG_DOUBLE;
	}
#endif

	/*
	 * The larger the object size is, the more slabs we want on the partial
	 * list to avoid pounding the page allocator excessively.
	 */
	s->min_partial = min_t(unsigned long, MAX_PARTIAL, ilog2(s->size) / 2);
	s->min_partial = max_t(unsigned long, MIN_PARTIAL, s->min_partial);

	s->cpu_sheaves = alloc_percpu(struct slub_percpu_sheaves);
	if (!s->cpu_sheaves) {
		err = -ENOMEM;
		goto out;
	}

#ifdef CONFIG_NUMA
	s->remote_node_defrag_ratio = 1000;
#endif

	/* Initialize the pre-computed randomized freelist if slab is up */
	if (slab_state >= UP) {
		if (init_cache_random_seq(s))
			goto out;
	}

	if (!init_kmem_cache_nodes(s))
		goto out;

#ifdef CONFIG_SLUB_STATS
	if (!alloc_kmem_cache_stats(s))
		goto out;
#endif

	err = init_percpu_sheaves(s);
	if (err)
		goto out;

	err = 0;

	/* Mutex is not taken during early boot */
	if (slab_state <= UP)
		goto out;

	/*
	 * Failing to create sysfs files is not critical to SLUB functionality.
	 * If it fails, proceed with cache creation without these files.
	 */
	if (sysfs_slab_add(s))
		pr_err("SLUB: Unable to add cache %s to sysfs\n", s->name);

	if (s->flags & SLAB_STORE_USER)
		debugfs_slab_add(s);

out:
	if (err)
		__kmem_cache_release(s);
	return err;
}

/* count_inuse / count_total：sysfs show 函数的 per-slab 回调，
 * 用于 show_slab_objects() 遍历所有 slab 时累计 inuse/objects 统计值。*/
#ifdef SLAB_SUPPORTS_SYSFS
static int count_inuse(struct slab *slab)
{
	return slab->inuse;
}

static int count_total(struct slab *slab)
{
	return slab->objects;
}
#endif

#ifdef CONFIG_SLUB_DEBUG
/*
 * validate_slab() - 对单个 slab 页执行完整一致性检查（调试用）
 *
 * 步骤：
 *   1. validate_slab_ptr()：确认 page 确实是 slab 页（PageSlab 标志）
 *   2. check_slab()：slab 元数据合法性（inuse <= objects、未 frozen、页 padding 完整）
 *   3. on_freelist(NULL)：遍历 freelist 链表，验证链表无环、各 freepointer 合法，
 *      同时修正 slab->inuse 和 slab->objects 若与实际不符
 *   4. __fill_map()：将 freelist 中所有空闲对象在 obj_map 位图中标记
 *   5. for_each_object()：遍历 slab 内所有槽位，按"在位图中 = 空闲 = RED_INACTIVE,
 *      不在位图 = 已分配 = RED_ACTIVE"调用 check_object() 验证 red zone / poison
 *
 * @obj_map：调用方提供的位图缓冲（bitmap_alloc(oo_objects, GFP_KERNEL)），
 *   避免在每次验证时重新分配，提高批量验证的性能。
 */
static void validate_slab(struct kmem_cache *s, struct slab *slab,
			  unsigned long *obj_map)
{
	void *p;
	void *addr = slab_address(slab);

	if (!validate_slab_ptr(slab)) {
		slab_err(s, slab, "Not a valid slab page");
		return;
	}

	if (!check_slab(s, slab) || !on_freelist(s, slab, NULL))
		return;

	/* Now we know that a valid freelist exists */
	__fill_map(obj_map, s, slab);
	for_each_object(p, s, addr, slab->objects) {
		/* 在位图中 = 空闲对象，期望 RED_INACTIVE；否则 = 已分配，期望 RED_ACTIVE */
		u8 val = test_bit(__obj_to_index(s, addr, p), obj_map) ?
			 SLUB_RED_INACTIVE : SLUB_RED_ACTIVE;

		if (!check_object(s, slab, p, val))
			break;
	}
}

/*
 * validate_slab_node() - 验证 per-node partial（及 full）链表上所有 slab
 *
 * 在 list_lock 保护下遍历，确保 freelist 在验证期间不被修改。
 * 同时验证链表长度与计数器（nr_partial / nr_slabs）是否一致，
 * 不一致说明有 bug 导致计数器失步。
 *
 * SLAB_STORE_USER cache 维护 full 链表，也一并验证。
 * 普通 cache 不维护 full 链表，跳过。
 */
static int validate_slab_node(struct kmem_cache *s,
		struct kmem_cache_node *n, unsigned long *obj_map)
{
	unsigned long count = 0;
	struct slab *slab;
	unsigned long flags;

	spin_lock_irqsave(&n->list_lock, flags);

	list_for_each_entry(slab, &n->partial, slab_list) {
		validate_slab(s, slab, obj_map);
		count++;
	}
	if (count != n->nr_partial) {
		pr_err("SLUB %s: %ld partial slabs counted but counter=%ld\n",
		       s->name, count, n->nr_partial);
		slab_add_kunit_errors();
	}

	if (!(s->flags & SLAB_STORE_USER))
		goto out;

	list_for_each_entry(slab, &n->full, slab_list) {
		validate_slab(s, slab, obj_map);
		count++;
	}
	if (count != node_nr_slabs(n)) {
		pr_err("SLUB: %s %ld slabs counted but counter=%ld\n",
		       s->name, count, node_nr_slabs(n));
		slab_add_kunit_errors();
	}

out:
	spin_unlock_irqrestore(&n->list_lock, flags);
	return count;
}

/*
 * validate_slab_cache() - 对整个 cache 的所有 slab 执行完整验证（公开接口）
 *
 * 先 flush_all() 将 per-CPU sheaf 中的对象刷回 slab freelist，
 * 再对所有 NUMA 节点的所有 slab 调用 validate_slab_node()。
 *
 * 返回验证过的 slab 总数，或 -ENOMEM（位图分配失败）。
 * 可通过 /sys/kernel/slab/<cache>/validate 触发，或直接调用。
 */
long validate_slab_cache(struct kmem_cache *s)
{
	int node;
	unsigned long count = 0;
	struct kmem_cache_node *n;
	unsigned long *obj_map;

	obj_map = bitmap_alloc(oo_objects(s->oo), GFP_KERNEL);
	if (!obj_map)
		return -ENOMEM;

	flush_all(s);
	for_each_kmem_cache_node(s, node, n)
		count += validate_slab_node(s, n, obj_map);

	bitmap_free(obj_map);

	return count;
}
EXPORT_SYMBOL(validate_slab_cache);

#ifdef CONFIG_DEBUG_FS
/*
 * ============================================================
 * debugfs 调用点追踪（SLAB_STORE_USER + CONFIG_DEBUG_FS）
 *
 * 背景：开启 SLAB_STORE_USER 的 cache，每个对象尾部存储了最近一次
 * 分配/释放的调用栈（struct track）。debugfs 把这些信息汇总成
 * "哪些代码地址分配了多少对象、平均存活多长时间"的报告，
 * 通过 /sys/kernel/debug/slab/<cache>/alloc_traces 和 free_traces 呈现。
 *
 * 这对定位内存泄漏、对象寿命异常非常有用：若某个调用点的分配量
 * 持续增加而 free_traces 里没有对应的释放，很可能有泄漏。
 * ============================================================
 *
 * Generate lists of code addresses where slabcache objects are allocated
 * and freed.
 */

/*
 * struct location - 单个调用点的聚合统计记录
 *
 * 多个来自同一调用地址（addr）+ 同一调用栈（handle）+ 同一 waste 的 track
 * 合并为一条 location 记录，统计分配/释放次数、时间分布、来源 CPU 和节点。
 *
 * @handle:   stackdepot 句柄（指向完整调用栈）
 * @count:    该调用点的分配/释放次数
 * @addr:     调用方指令地址
 * @waste:    object_size - orig_size（kmalloc 槽位浪费的字节数）
 * @sum_time: 所有对象从分配到释放的时间总和（jiffies）
 * @min/max_time: 最短/最长存活时间
 * @min/max_pid:  最小/最大 PID（帮助定位哪个进程在此分配）
 * @cpus:     在哪些 CPU 上发生过分配（位图）
 * @nodes:    在哪些 NUMA 节点上发生过分配（掩码）
 */
struct location {
	depot_stack_handle_t handle;
	unsigned long count;
	unsigned long addr;
	unsigned long waste;
	long long sum_time;
	long min_time;
	long max_time;
	long min_pid;
	long max_pid;
	DECLARE_BITMAP(cpus, NR_CPUS);
	nodemask_t nodes;
};

/*
 * struct loc_track - location 记录的动态数组（有序，按 addr/handle/waste 排序）
 *
 * @max:   当前分配的最大记录数（页为单位动态扩容）
 * @count: 当前有效记录数
 * @loc:   记录数组指针（用 __get_free_pages 分配，允许大 GFP_ATOMIC 分配）
 * @idx:   debugfs seq_file 的读取游标
 */
struct loc_track {
	unsigned long max;
	unsigned long count;
	struct location *loc;
	loff_t idx;
};

static struct dentry *slab_debugfs_root;

/* free_loc_track()：释放 loc_track 的记录数组（按页分配，按页释放） */
static void free_loc_track(struct loc_track *t)
{
	if (t->max)
		free_pages((unsigned long)t->loc,
			get_order(sizeof(struct location) * t->max));
}

/*
 * alloc_loc_track() - 扩容 loc_track 数组到 max 条记录
 *
 * 用 __get_free_pages（支持 GFP_ATOMIC，可在 spinlock 下调用）分配新数组，
 * 若有旧数据先 memcpy 复制再释放旧数组（realloc 语义）。
 * 失败时不破坏原有数据，返回 0。
 */
static int alloc_loc_track(struct loc_track *t, unsigned long max, gfp_t flags)
{
	struct location *l;
	int order;

	order = get_order(sizeof(struct location) * max);

	l = (void *)__get_free_pages(flags, order);
	if (!l)
		return 0;

	if (t->count) {
		memcpy(l, t->loc, sizeof(struct location) * t->count);
		free_loc_track(t);
	}
	t->max = max;
	t->loc = l;
	return 1;
}

/*
 * add_location() - 将一条 track 记录合并或插入到 loc_track 有序数组
 *
 * 三维排序键：(addr, handle, waste)，用二分查找定位插入/合并位置。
 *
 * 找到相同键的记录（已存在）：
 *   合并：count++，更新 sum/min/max_time、pid 范围、cpus 位图、nodes 掩码
 *
 * 未找到（新调用点）：
 *   若数组已满（count >= max）：GFP_ATOMIC 扩容（倍增），失败则丢弃本条记录
 *   memmove 将 pos 之后的记录后移一位，插入新记录
 *
 * @orig_size：kmalloc 原始请求大小，waste = object_size - orig_size
 *   waste > 0 说明 kmalloc 分配了比请求更大的槽位，这段空间被浪费了。
 *   按 waste 分组便于发现哪些调用点请求了不对齐的大小（浪费多）。
 */
static int add_location(struct loc_track *t, struct kmem_cache *s,
				const struct track *track,
				unsigned int orig_size)
{
	long start, end, pos;
	struct location *l;
	unsigned long caddr, chandle, cwaste;
	unsigned long age = jiffies - track->when;
	depot_stack_handle_t handle = 0;
	unsigned int waste = s->object_size - orig_size;

#ifdef CONFIG_STACKDEPOT
	handle = READ_ONCE(track->handle);
#endif
	start = -1;
	end = t->count;

	for ( ; ; ) {
		pos = start + (end - start + 1) / 2;

		/*
		 * There is nothing at "end". If we end up there
		 * we need to add something to before end.
		 */
		if (pos == end)
			break;

		l = &t->loc[pos];
		caddr = l->addr;
		chandle = l->handle;
		cwaste = l->waste;
		if ((track->addr == caddr) && (handle == chandle) &&
			(waste == cwaste)) {
			/* 找到相同调用点，合并统计 */
			l->count++;
			if (track->when) {
				l->sum_time += age;
				if (age < l->min_time)
					l->min_time = age;
				if (age > l->max_time)
					l->max_time = age;

				if (track->pid < l->min_pid)
					l->min_pid = track->pid;
				if (track->pid > l->max_pid)
					l->max_pid = track->pid;

				cpumask_set_cpu(track->cpu,
						to_cpumask(l->cpus));
			}
			node_set(page_to_nid(virt_to_page(track)), l->nodes);
			return 1;
		}

		/* 二分搜索：按 (addr, handle, waste) 三维排序 */
		if (track->addr < caddr)
			end = pos;
		else if (track->addr == caddr && handle < chandle)
			end = pos;
		else if (track->addr == caddr && handle == chandle &&
				waste < cwaste)
			end = pos;
		else
			start = pos;
	}

	/*
	 * Not found. Insert new tracking element.
	 */
	if (t->count >= t->max && !alloc_loc_track(t, 2 * t->max, GFP_ATOMIC))
		return 0;

	l = t->loc + pos;
	if (pos < t->count)
		memmove(l + 1, l,
			(t->count - pos) * sizeof(struct location));
	t->count++;
	l->count = 1;
	l->addr = track->addr;
	l->sum_time = age;
	l->min_time = age;
	l->max_time = age;
	l->min_pid = track->pid;
	l->max_pid = track->pid;
	l->handle = handle;
	l->waste = waste;
	cpumask_clear(to_cpumask(l->cpus));
	cpumask_set_cpu(track->cpu, to_cpumask(l->cpus));
	nodes_clear(l->nodes);
	node_set(page_to_nid(virt_to_page(track)), l->nodes);
	return 1;
}

/*
 * process_slab() - 扫描单个 slab，将已分配对象的 track 记录加入 loc_track
 *
 * __fill_map 建立空闲对象位图，不在位图中的对象（已分配）才有 track 信息。
 * 对每个已分配对象，读取 track（alloc 或 free 方向），调用 add_location 聚合。
 * 分配追踪时传 orig_size（用于 waste 计算），释放追踪时 waste 无意义传 object_size。
 */
static void process_slab(struct loc_track *t, struct kmem_cache *s,
		struct slab *slab, enum track_item alloc,
		unsigned long *obj_map)
{
	void *addr = slab_address(slab);
	bool is_alloc = (alloc == TRACK_ALLOC);
	void *p;

	__fill_map(obj_map, s, slab);

	/* 只处理已分配对象（不在空闲位图中的对象） */
	for_each_object(p, s, addr, slab->objects)
		if (!test_bit(__obj_to_index(s, addr, p), obj_map))
			add_location(t, s, get_track(s, p, alloc),
				     is_alloc ? get_orig_size(s, p) :
						s->object_size);
}
#endif  /* CONFIG_DEBUG_FS   */
#endif	/* CONFIG_SLUB_DEBUG */

#ifdef SLAB_SUPPORTS_SYSFS
enum slab_stat_type {
	SL_ALL,			/* All slabs */
	SL_PARTIAL,		/* Only partially allocated slabs */
	SL_CPU,			/* Only slabs used for cpu caches */
	SL_OBJECTS,		/* Determine allocated objects not slabs */
	SL_TOTAL		/* Determine object capacity not slabs */
};

#define SO_ALL		(1 << SL_ALL)
#define SO_PARTIAL	(1 << SL_PARTIAL)
#define SO_CPU		(1 << SL_CPU)
#define SO_OBJECTS	(1 << SL_OBJECTS)
#define SO_TOTAL	(1 << SL_TOTAL)

static ssize_t show_slab_objects(struct kmem_cache *s,
				 char *buf, unsigned long flags)
{
	unsigned long total = 0;
	int node;
	int x;
	unsigned long *nodes;
	int len = 0;

	nodes = kcalloc(nr_node_ids, sizeof(unsigned long), GFP_KERNEL);
	if (!nodes)
		return -ENOMEM;

	/*
	 * It is impossible to take "mem_hotplug_lock" here with "kernfs_mutex"
	 * already held which will conflict with an existing lock order:
	 *
	 * mem_hotplug_lock->slab_mutex->kernfs_mutex
	 *
	 * We don't really need mem_hotplug_lock (to hold off
	 * slab_mem_going_offline_callback) here because slab's memory hot
	 * unplug code doesn't destroy the kmem_cache->node[] data.
	 */

#ifdef CONFIG_SLUB_DEBUG
	if (flags & SO_ALL) {
		struct kmem_cache_node *n;

		for_each_kmem_cache_node(s, node, n) {

			if (flags & SO_TOTAL)
				x = node_nr_objs(n);
			else if (flags & SO_OBJECTS)
				x = node_nr_objs(n) - count_partial(n, count_free);
			else
				x = node_nr_slabs(n);
			total += x;
			nodes[node] += x;
		}

	} else
#endif
	if (flags & SO_PARTIAL) {
		struct kmem_cache_node *n;

		for_each_kmem_cache_node(s, node, n) {
			if (flags & SO_TOTAL)
				x = count_partial(n, count_total);
			else if (flags & SO_OBJECTS)
				x = count_partial(n, count_inuse);
			else
				x = n->nr_partial;
			total += x;
			nodes[node] += x;
		}
	}

	len += sysfs_emit_at(buf, len, "%lu", total);
#ifdef CONFIG_NUMA
	for (node = 0; node < nr_node_ids; node++) {
		if (nodes[node])
			len += sysfs_emit_at(buf, len, " N%d=%lu",
					     node, nodes[node]);
	}
#endif
	len += sysfs_emit_at(buf, len, "\n");
	kfree(nodes);

	return len;
}

/*
 * sysfs 属性框架
 *
 * 每个 kmem_cache 在 /sys/kernel/slab/<name>/ 下有一组属性文件，
 * 通过 slab_attribute + SLAB_ATTR_RO/SLAB_ATTR 宏定义。
 *
 * slab_attribute：封装了 sysfs attribute（name/mode）以及对应的
 * show（读）和 store（写）回调，回调函数以 struct kmem_cache * 为参数，
 * 比标准 kobject show/store 更方便直接访问 cache 字段。
 *
 * SLAB_ATTR_RO：只读属性（mode 0400）
 * SLAB_ATTR：读写属性（mode 0600），需要同时定义 _show 和 _store
 *
 * to_slab_attr / to_slab：从 kobject 反查 slab_attribute / kmem_cache 指针
 */
#define to_slab_attr(n) container_of_const(n, struct slab_attribute, attr)
#define to_slab(n) container_of(n, struct kmem_cache, kobj)

struct slab_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kmem_cache *s, char *buf);
	ssize_t (*store)(struct kmem_cache *s, const char *x, size_t count);
};

#define SLAB_ATTR_RO(_name) \
	static const struct slab_attribute _name##_attr = __ATTR_RO_MODE(_name, 0400)

#define SLAB_ATTR(_name) \
	static const struct slab_attribute _name##_attr = __ATTR_RW_MODE(_name, 0600)

/*
 * 以下各 _show/_store 函数是 /sys/kernel/slab/<cache>/ 各属性文件的读写回调。
 * 只读属性：slab_size（对象步长）、align（对齐）、object_size（用户大小）、
 *           objs_per_slab（每 slab 对象数）、order（slab order）、
 *           sheaf_capacity（per-CPU sheaf 容量）
 * 可写属性：min_partial（可调，控制 partial list 保留量）
 */

static ssize_t slab_size_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", s->size);
}
SLAB_ATTR_RO(slab_size);

static ssize_t align_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", s->align);
}
SLAB_ATTR_RO(align);

static ssize_t object_size_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", s->object_size);
}
SLAB_ATTR_RO(object_size);

static ssize_t objs_per_slab_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", oo_objects(s->oo));
}
SLAB_ATTR_RO(objs_per_slab);

static ssize_t order_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", oo_order(s->oo));
}
SLAB_ATTR_RO(order);

static ssize_t sheaf_capacity_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", s->sheaf_capacity);
}
SLAB_ATTR_RO(sheaf_capacity);

static ssize_t min_partial_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%lu\n", s->min_partial);
}

static ssize_t min_partial_store(struct kmem_cache *s, const char *buf,
				 size_t length)
{
	unsigned long min;
	int err;

	err = kstrtoul(buf, 10, &min);
	if (err)
		return err;

	s->min_partial = min;
	return length;
}
SLAB_ATTR(min_partial);

/*
 * cpu_partial：旧版 SLUB 的 per-CPU partial list 接口，现已被 sheaf 机制取代。
 * 读取始终返回 0（无 per-CPU partial），写入只接受 0（写入非 0 返回 -EINVAL），
 * 保留接口是为了向后兼容依赖此属性的脚本和工具。
 */
static ssize_t cpu_partial_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "0\n");
}

static ssize_t cpu_partial_store(struct kmem_cache *s, const char *buf,
				 size_t length)
{
	unsigned int objects;
	int err;

	err = kstrtouint(buf, 10, &objects);
	if (err)
		return err;
	if (objects)
		return -EINVAL;

	return length;
}
SLAB_ATTR(cpu_partial);

static ssize_t ctor_show(struct kmem_cache *s, char *buf)
{
	if (!s->ctor)
		return 0;
	return sysfs_emit(buf, "%pS\n", s->ctor);
}
SLAB_ATTR_RO(ctor);

static ssize_t aliases_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", s->refcount < 0 ? 0 : s->refcount - 1);
}
SLAB_ATTR_RO(aliases);

static ssize_t partial_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_PARTIAL);
}
SLAB_ATTR_RO(partial);

static ssize_t cpu_slabs_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_CPU);
}
SLAB_ATTR_RO(cpu_slabs);

static ssize_t objects_partial_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_PARTIAL|SO_OBJECTS);
}
SLAB_ATTR_RO(objects_partial);

static ssize_t slabs_cpu_partial_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "0(0)\n");
}
SLAB_ATTR_RO(slabs_cpu_partial);

static ssize_t reclaim_account_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_RECLAIM_ACCOUNT));
}
SLAB_ATTR_RO(reclaim_account);

static ssize_t hwcache_align_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_HWCACHE_ALIGN));
}
SLAB_ATTR_RO(hwcache_align);

#ifdef CONFIG_ZONE_DMA
static ssize_t cache_dma_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_CACHE_DMA));
}
SLAB_ATTR_RO(cache_dma);
#endif

#ifdef CONFIG_HARDENED_USERCOPY
static ssize_t usersize_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", s->usersize);
}
SLAB_ATTR_RO(usersize);
#endif

static ssize_t destroy_by_rcu_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_TYPESAFE_BY_RCU));
}
SLAB_ATTR_RO(destroy_by_rcu);

#ifdef CONFIG_SLUB_DEBUG
static ssize_t slabs_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_ALL);
}
SLAB_ATTR_RO(slabs);

static ssize_t total_objects_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_ALL|SO_TOTAL);
}
SLAB_ATTR_RO(total_objects);

static ssize_t objects_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_ALL|SO_OBJECTS);
}
SLAB_ATTR_RO(objects);

static ssize_t sanity_checks_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_CONSISTENCY_CHECKS));
}
SLAB_ATTR_RO(sanity_checks);

static ssize_t trace_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_TRACE));
}
SLAB_ATTR_RO(trace);

static ssize_t red_zone_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_RED_ZONE));
}

SLAB_ATTR_RO(red_zone);

static ssize_t poison_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_POISON));
}

SLAB_ATTR_RO(poison);

static ssize_t store_user_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_STORE_USER));
}

SLAB_ATTR_RO(store_user);

static ssize_t validate_show(struct kmem_cache *s, char *buf)
{
	return 0;
}

/*
 * validate_store()：写入 "1" 触发 validate_slab_cache()，对 cache 所有 slab 做
 * 完整一致性检查。只有调试 cache（kmem_cache_debug(s)）才有意义（非调试 cache
 * 没有 red zone / poison 元数据，validate 不会发现问题）。
 */
static ssize_t validate_store(struct kmem_cache *s,
			const char *buf, size_t length)
{
	int ret = -EINVAL;

	if (buf[0] == '1' && kmem_cache_debug(s)) {
		ret = validate_slab_cache(s);
		if (ret >= 0)
			ret = length;
	}
	return ret;
}
SLAB_ATTR(validate);

#endif /* CONFIG_SLUB_DEBUG */

#ifdef CONFIG_FAILSLAB
static ssize_t failslab_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_FAILSLAB));
}

/*
 * failslab_store()：动态开启/关闭 SLAB_FAILSLAB（fault injection）。
 * refcount > 1 时拒绝（cache 被多个使用方共用/合并，单独改 flag 会影响所有使用方）。
 * WRITE_ONCE 保证标志修改对其他 CPU 可见（避免编译器优化掉写操作）。
 */
static ssize_t failslab_store(struct kmem_cache *s, const char *buf,
				size_t length)
{
	if (s->refcount > 1)
		return -EINVAL;

	if (buf[0] == '1')
		WRITE_ONCE(s->flags, s->flags | SLAB_FAILSLAB);
	else
		WRITE_ONCE(s->flags, s->flags & ~SLAB_FAILSLAB);

	return length;
}
SLAB_ATTR(failslab);
#endif

static ssize_t shrink_show(struct kmem_cache *s, char *buf)
{
	return 0;
}

static ssize_t shrink_store(struct kmem_cache *s,
			const char *buf, size_t length)
{
	if (buf[0] == '1')
		kmem_cache_shrink(s);
	else
		return -EINVAL;
	return length;
}
SLAB_ATTR(shrink);

#ifdef CONFIG_NUMA
static ssize_t remote_node_defrag_ratio_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", s->remote_node_defrag_ratio / 10);
}

/*
 * remote_node_defrag_ratio：控制跨 NUMA 节点碎片整理的积极程度（0~100）。
 * 读取时除以 10（内部存储 0~1000），写入时乘以 10。
 * 0 = 完全不跨节点；100 = 几乎每次分配都扫描其他节点的 partial list。
 * 详见 get_from_any_partial() 中的概率判断逻辑。
 */
static ssize_t remote_node_defrag_ratio_store(struct kmem_cache *s,
				const char *buf, size_t length)
{
	unsigned int ratio;
	int err;

	err = kstrtouint(buf, 10, &ratio);
	if (err)
		return err;
	if (ratio > 100)
		return -ERANGE;

	s->remote_node_defrag_ratio = ratio * 10;

	return length;
}
SLAB_ATTR(remote_node_defrag_ratio);
#endif

/*
 * SLUB 统计属性（CONFIG_SLUB_STATS）
 *
 * show_stat()：汇总所有 CPU 的指定计数器值，格式为 "总数 C0=x C1=x ..."
 * clear_stat()：将所有 CPU 的指定计数器清零
 *
 * STAT_ATTR(si, text) 宏：为每个 stat_item 自动生成 sysfs 属性：
 *   读取 → 返回汇总统计值
 *   写入 "0" → 清零计数器（只允许写 "0"，任何其他值返回 -EINVAL）
 *   这些属性暴露在 /sys/kernel/slab/<cache>/<text> 下，
 *   用于性能分析：如 alloc_fastpath/alloc_slowpath 的比例反映 sheaf 命中率。
 */
#ifdef CONFIG_SLUB_STATS
static int show_stat(struct kmem_cache *s, char *buf, enum stat_item si)
{
	unsigned long sum  = 0;
	int cpu;
	int len = 0;
	int *data = kmalloc_objs(int, nr_cpu_ids);

	if (!data)
		return -ENOMEM;

	for_each_online_cpu(cpu) {
		unsigned int x = per_cpu_ptr(s->cpu_stats, cpu)->stat[si];

		data[cpu] = x;
		sum += x;
	}

	len += sysfs_emit_at(buf, len, "%lu", sum);

#ifdef CONFIG_SMP
	for_each_online_cpu(cpu) {
		if (data[cpu])
			len += sysfs_emit_at(buf, len, " C%d=%u",
					     cpu, data[cpu]);
	}
#endif
	kfree(data);
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

static void clear_stat(struct kmem_cache *s, enum stat_item si)
{
	int cpu;

	for_each_online_cpu(cpu)
		per_cpu_ptr(s->cpu_stats, cpu)->stat[si] = 0;
}

#define STAT_ATTR(si, text) 					\
static ssize_t text##_show(struct kmem_cache *s, char *buf)	\
{								\
	return show_stat(s, buf, si);				\
}								\
static ssize_t text##_store(struct kmem_cache *s,		\
				const char *buf, size_t length)	\
{								\
	if (buf[0] != '0')					\
		return -EINVAL;					\
	clear_stat(s, si);					\
	return length;						\
}								\
SLAB_ATTR(text);						\

STAT_ATTR(ALLOC_FASTPATH, alloc_fastpath);
STAT_ATTR(ALLOC_SLOWPATH, alloc_slowpath);
STAT_ATTR(FREE_RCU_SHEAF, free_rcu_sheaf);
STAT_ATTR(FREE_RCU_SHEAF_FAIL, free_rcu_sheaf_fail);
STAT_ATTR(FREE_FASTPATH, free_fastpath);
STAT_ATTR(FREE_SLOWPATH, free_slowpath);
STAT_ATTR(FREE_ADD_PARTIAL, free_add_partial);
STAT_ATTR(FREE_REMOVE_PARTIAL, free_remove_partial);
STAT_ATTR(ALLOC_SLAB, alloc_slab);
STAT_ATTR(ALLOC_NODE_MISMATCH, alloc_node_mismatch);
STAT_ATTR(FREE_SLAB, free_slab);
STAT_ATTR(ORDER_FALLBACK, order_fallback);
STAT_ATTR(CMPXCHG_DOUBLE_FAIL, cmpxchg_double_fail);
STAT_ATTR(SHEAF_FLUSH, sheaf_flush);
STAT_ATTR(SHEAF_REFILL, sheaf_refill);
STAT_ATTR(SHEAF_ALLOC, sheaf_alloc);
STAT_ATTR(SHEAF_FREE, sheaf_free);
STAT_ATTR(BARN_GET, barn_get);
STAT_ATTR(BARN_GET_FAIL, barn_get_fail);
STAT_ATTR(BARN_PUT, barn_put);
STAT_ATTR(BARN_PUT_FAIL, barn_put_fail);
STAT_ATTR(SHEAF_PREFILL_FAST, sheaf_prefill_fast);
STAT_ATTR(SHEAF_PREFILL_SLOW, sheaf_prefill_slow);
STAT_ATTR(SHEAF_PREFILL_OVERSIZE, sheaf_prefill_oversize);
STAT_ATTR(SHEAF_RETURN_FAST, sheaf_return_fast);
STAT_ATTR(SHEAF_RETURN_SLOW, sheaf_return_slow);
#endif	/* CONFIG_SLUB_STATS */

#ifdef CONFIG_KFENCE
static ssize_t skip_kfence_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_SKIP_KFENCE));
}

static ssize_t skip_kfence_store(struct kmem_cache *s,
			const char *buf, size_t length)
{
	int ret = length;

	if (buf[0] == '0')
		s->flags &= ~SLAB_SKIP_KFENCE;
	else if (buf[0] == '1')
		s->flags |= SLAB_SKIP_KFENCE;
	else
		ret = -EINVAL;

	return ret;
}
SLAB_ATTR(skip_kfence);
#endif

/* slab_attrs[]：cache 的所有 sysfs 属性列表，条件编译决定哪些属性可见 */
static const struct attribute *const slab_attrs[] = {
	&slab_size_attr.attr,
	&object_size_attr.attr,
	&objs_per_slab_attr.attr,
	&order_attr.attr,
	&sheaf_capacity_attr.attr,
	&min_partial_attr.attr,
	&cpu_partial_attr.attr,
	&objects_partial_attr.attr,
	&partial_attr.attr,
	&cpu_slabs_attr.attr,
	&ctor_attr.attr,
	&aliases_attr.attr,
	&align_attr.attr,
	&hwcache_align_attr.attr,
	&reclaim_account_attr.attr,
	&destroy_by_rcu_attr.attr,
	&shrink_attr.attr,
	&slabs_cpu_partial_attr.attr,
#ifdef CONFIG_SLUB_DEBUG
	&total_objects_attr.attr,
	&objects_attr.attr,
	&slabs_attr.attr,
	&sanity_checks_attr.attr,
	&trace_attr.attr,
	&red_zone_attr.attr,
	&poison_attr.attr,
	&store_user_attr.attr,
	&validate_attr.attr,
#endif
#ifdef CONFIG_ZONE_DMA
	&cache_dma_attr.attr,
#endif
#ifdef CONFIG_NUMA
	&remote_node_defrag_ratio_attr.attr,
#endif
#ifdef CONFIG_SLUB_STATS
	&alloc_fastpath_attr.attr,
	&alloc_slowpath_attr.attr,
	&free_rcu_sheaf_attr.attr,
	&free_rcu_sheaf_fail_attr.attr,
	&free_fastpath_attr.attr,
	&free_slowpath_attr.attr,
	&free_add_partial_attr.attr,
	&free_remove_partial_attr.attr,
	&alloc_slab_attr.attr,
	&alloc_node_mismatch_attr.attr,
	&free_slab_attr.attr,
	&order_fallback_attr.attr,
	&cmpxchg_double_fail_attr.attr,
	&sheaf_flush_attr.attr,
	&sheaf_refill_attr.attr,
	&sheaf_alloc_attr.attr,
	&sheaf_free_attr.attr,
	&barn_get_attr.attr,
	&barn_get_fail_attr.attr,
	&barn_put_attr.attr,
	&barn_put_fail_attr.attr,
	&sheaf_prefill_fast_attr.attr,
	&sheaf_prefill_slow_attr.attr,
	&sheaf_prefill_oversize_attr.attr,
	&sheaf_return_fast_attr.attr,
	&sheaf_return_slow_attr.attr,
#endif
#ifdef CONFIG_FAILSLAB
	&failslab_attr.attr,
#endif
#ifdef CONFIG_HARDENED_USERCOPY
	&usersize_attr.attr,
#endif
#ifdef CONFIG_KFENCE
	&skip_kfence_attr.attr,
#endif

	NULL
};

ATTRIBUTE_GROUPS(slab);

/*
 * slab_attr_show / slab_attr_store：kobject 的通用 sysfs 读写分发器。
 * 通过 to_slab_attr() 找到具体的 slab_attribute，再调用其 show/store 回调。
 * 属性未实现 show/store 时返回 -EIO（不是 -ENODEV，保持 kobject 约定）。
 */
static ssize_t slab_attr_show(struct kobject *kobj,
				struct attribute *attr,
				char *buf)
{
	const struct slab_attribute *attribute;
	struct kmem_cache *s;

	attribute = to_slab_attr(attr);
	s = to_slab(kobj);

	if (!attribute->show)
		return -EIO;

	return attribute->show(s, buf);
}

static ssize_t slab_attr_store(struct kobject *kobj,
				struct attribute *attr,
				const char *buf, size_t len)
{
	const struct slab_attribute *attribute;
	struct kmem_cache *s;

	attribute = to_slab_attr(attr);
	s = to_slab(kobj);

	if (!attribute->store)
		return -EIO;

	return attribute->store(s, buf, len);
}

/* kmem_cache_release：kobject 引用计数归零时调用，触发 cache 最终释放 */
static void kmem_cache_release(struct kobject *k)
{
	slab_kmem_cache_release(to_slab(k));
}

static const struct sysfs_ops slab_sysfs_ops = {
	.show = slab_attr_show,
	.store = slab_attr_store,
};

/* slab_ktype：cache kobject 的类型描述符，绑定 sysfs_ops 和 release 回调 */
static const struct kobj_type slab_ktype = {
	.sysfs_ops = &slab_sysfs_ops,
	.release = kmem_cache_release,
	.default_groups = slab_groups,
};

/* slab_kset：/sys/kernel/slab/ 目录对应的 kset，所有 cache kobject 挂于此 */
static struct kset *slab_kset;

static inline struct kset *cache_kset(struct kmem_cache *s)
{
	return slab_kset;
}

#define ID_STR_LENGTH 32

/*
 * create_unique_id() - 为可合并 cache 生成唯一的 sysfs 目录名
 *
 * 问题背景：多个大小和标志相同的 cache 可能被合并为同一个 cache（别名机制）。
 * 合并后的 cache 用真实名字（如 "kmalloc-64"）作为 kobject 名，
 * 其他被合并的 cache 通过符号链接指向它。为避免真实目录名与别名冲突，
 * 内部使用 ":[flags-]size" 格式的唯一 ID 作为真实目录名。
 *
 * 格式：:[d][D][a][F][A]-XXXXXXX（7 位对齐的 size）
 *   d = SLAB_CACHE_DMA，D = SLAB_CACHE_DMA32，a = SLAB_RECLAIM_ACCOUNT
 *   F = SLAB_CONSISTENCY_CHECKS，A = SLAB_ACCOUNT
 *   这些标志都是合并时需要匹配的，编码进 ID 保证唯一性。
 *
 * 不可合并的 cache（有 ctor、特殊对齐等）直接使用 s->name，不调用此函数。
 */
static char *create_unique_id(struct kmem_cache *s)
{
	char *name = kmalloc(ID_STR_LENGTH, GFP_KERNEL);
	char *p = name;

	if (!name)
		return ERR_PTR(-ENOMEM);

	*p++ = ':';
	/*
	 * First flags affecting slabcache operations. We will only
	 * get here for aliasable slabs so we do not need to support
	 * too many flags. The flags here must cover all flags that
	 * are matched during merging to guarantee that the id is
	 * unique.
	 */
	if (s->flags & SLAB_CACHE_DMA)
		*p++ = 'd';
	if (s->flags & SLAB_CACHE_DMA32)
		*p++ = 'D';
	if (s->flags & SLAB_RECLAIM_ACCOUNT)
		*p++ = 'a';
	if (s->flags & SLAB_CONSISTENCY_CHECKS)
		*p++ = 'F';
	if (s->flags & SLAB_ACCOUNT)
		*p++ = 'A';
	if (p != name + 1)
		*p++ = '-';
	p += snprintf(p, ID_STR_LENGTH - (p - name), "%07u", s->size);

	if (WARN_ON(p > name + ID_STR_LENGTH - 1)) {
		kfree(name);
		return ERR_PTR(-EINVAL);
	}
	kmsan_unpoison_memory(name, p - name);
	return name;
}

/*
 * sysfs_slab_add() - 将 cache 注册到 /sys/kernel/slab/ 目录
 *
 * 两种模式：
 *   不可合并（unmergeable）：使用 s->name 作为目录名（不会有重名冲突），
 *     先 sysfs_remove_link 清除可能存在的旧别名链接，再 kobject_init_and_add。
 *
 *   可合并：用 create_unique_id() 生成 ":[flags-]size" 格式目录名，
 *     再调用 sysfs_slab_alias(s, s->name) 创建 s->name → 唯一 ID 的符号链接，
 *     用户看到的是 cache 原名，实际目录是唯一 ID（多个同大小 cache 共享一个目录）。
 *
 * 失败不致命（do_kmem_cache_create 调用时只打印 warning 继续）。
 */
static int sysfs_slab_add(struct kmem_cache *s)
{
	int err;
	const char *name;
	struct kset *kset = cache_kset(s);
	int unmergeable = slab_unmergeable(s);

	if (!unmergeable && disable_higher_order_debug &&
			(slub_debug & DEBUG_METADATA_FLAGS))
		unmergeable = 1;

	if (unmergeable) {
		/*
		 * Slabcache can never be merged so we can use the name proper.
		 * This is typically the case for debug situations. In that
		 * case we can catch duplicate names easily.
		 */
		sysfs_remove_link(&slab_kset->kobj, s->name);
		name = s->name;
	} else {
		/*
		 * Create a unique name for the slab as a target
		 * for the symlinks.
		 */
		name = create_unique_id(s);
		if (IS_ERR(name))
			return PTR_ERR(name);
	}

	s->kobj.kset = kset;
	err = kobject_init_and_add(&s->kobj, &slab_ktype, NULL, "%s", name);
	if (err)
		goto out;

	if (!unmergeable) {
		/* Setup first alias */
		sysfs_slab_alias(s, s->name);
	}
out:
	if (!unmergeable)
		kfree(name);
	return err;
}

/* sysfs_slab_unlink()：从 sysfs 移除 cache 的 kobject（cache 销毁时调用） */
void sysfs_slab_unlink(struct kmem_cache *s)
{
	if (s->kobj.state_in_sysfs)
		kobject_del(&s->kobj);
}

/* sysfs_slab_release()：释放 cache kobject 的引用计数（配合 sysfs_slab_unlink 使用） */
void sysfs_slab_release(struct kmem_cache *s)
{
	kobject_put(&s->kobj);
}

/*
 * saved_alias / alias_list：引导期间的 sysfs 别名缓冲
 *
 * sysfs 在内核引导很早期可能还未就绪，但 cache 创建（含别名注册）在
 * sysfs 初始化之前就开始了。用 saved_alias 链表暂存这些别名请求，
 * 等 slab_sysfs_init() 时统一处理。
 */
struct saved_alias {
	struct kmem_cache *s;
	const char *name;
	struct saved_alias *next;
};

static struct saved_alias *alias_list;

/*
 * sysfs_slab_alias() - 为 cache 创建 sysfs 符号链接别名
 *
 * slab_state == FULL（sysfs 已就绪）：
 *   先 sysfs_remove_link 清理旧链接（上一次启动/热插拔可能遗留），
 *   再 sysfs_create_link 创建 name → cache kobject 的符号链接。
 *   原始 cache kobject 可能失败未建立，此时 create_link 返回 -ENOENT，跳过。
 *
 * slab_state < FULL（sysfs 未就绪）：
 *   kmalloc 一个 saved_alias 节点，头插法加入 alias_list，
 *   等 slab_sysfs_init() 时批量处理。
 */
int sysfs_slab_alias(struct kmem_cache *s, const char *name)
{
	struct saved_alias *al;

	if (slab_state == FULL) {
		/*
		 * If we have a leftover link then remove it.
		 */
		sysfs_remove_link(&slab_kset->kobj, name);
		/*
		 * The original cache may have failed to generate sysfs file.
		 * In that case, sysfs_create_link() returns -ENOENT and
		 * symbolic link creation is skipped.
		 */
		return sysfs_create_link(&slab_kset->kobj, &s->kobj, name);
	}

	al = kmalloc_obj(struct saved_alias);
	if (!al)
		return -ENOMEM;

	al->s = s;
	al->name = name;
	al->next = alias_list;
	alias_list = al;
	kmsan_unpoison_memory(al, sizeof(*al));
	return 0;
}

/*
 * slab_sysfs_init() - 初始化 /sys/kernel/slab/ 目录并补注册引导期间创建的 cache
 *
 * 以 late_initcall 注册，在 sysfs 完全就绪后执行。
 * 步骤：
 *   1. 创建 /sys/kernel/slab kset
 *   2. 设 slab_state = FULL（此后新建 cache 会直接注册 sysfs，不再缓冲）
 *   3. 遍历 slab_caches 链表，为引导期间创建的 cache 补注册 sysfs 目录
 *   4. 处理 alias_list：将缓冲的别名符号链接全部创建
 */
static int __init slab_sysfs_init(void)
{
	struct kmem_cache *s;
	int err;

	mutex_lock(&slab_mutex);

	slab_kset = kset_create_and_add("slab", NULL, kernel_kobj);
	if (!slab_kset) {
		mutex_unlock(&slab_mutex);
		pr_err("Cannot register slab subsystem.\n");
		return -ENOMEM;
	}

	slab_state = FULL;

	list_for_each_entry(s, &slab_caches, list) {
		err = sysfs_slab_add(s);
		if (err)
			pr_err("SLUB: Unable to add boot slab %s to sysfs\n",
			       s->name);
	}

	while (alias_list) {
		struct saved_alias *al = alias_list;

		alias_list = alias_list->next;
		err = sysfs_slab_alias(al->s, al->name);
		if (err)
			pr_err("SLUB: Unable to add boot slab alias %s to sysfs\n",
			       al->name);
		kfree(al);
	}

	mutex_unlock(&slab_mutex);
	return 0;
}
late_initcall(slab_sysfs_init);
#endif /* SLAB_SUPPORTS_SYSFS */

#if defined(CONFIG_SLUB_DEBUG) && defined(CONFIG_DEBUG_FS)
/*
 * slab_debugfs_show() - seq_file 的 show 回调，输出单条 location 记录
 *
 * 输出格式（每行一条调用点）：
 *   <次数> <函数名+偏移> [waste=总浪费/单次浪费] [age=min/avg/max] [pid=x-y]
 *   [cpus=位图] [nodes=位图]
 *   <调用栈（每帧缩进 8 空格）>
 *
 * t->idx 是当前行的游标（由 slab_debugfs_next 递增）。
 * sum_time == min_time 说明所有实例存活时间相同，只打印一个值。
 * waste 为 0 时（object_size == orig_size）不打印 waste 字段。
 */
static int slab_debugfs_show(struct seq_file *seq, void *v)
{
	struct loc_track *t = seq->private;
	struct location *l;
	unsigned long idx;

	idx = (unsigned long) t->idx;
	if (idx < t->count) {
		l = &t->loc[idx];

		seq_printf(seq, "%7ld ", l->count);

		if (l->addr)
			seq_printf(seq, "%pS", (void *)l->addr);
		else
			seq_puts(seq, "<not-available>");

		if (l->waste)
			seq_printf(seq, " waste=%lu/%lu",
				l->count * l->waste, l->waste);

		if (l->sum_time != l->min_time) {
			seq_printf(seq, " age=%ld/%llu/%ld",
				l->min_time, div_u64(l->sum_time, l->count),
				l->max_time);
		} else
			seq_printf(seq, " age=%ld", l->min_time);

		if (l->min_pid != l->max_pid)
			seq_printf(seq, " pid=%ld-%ld", l->min_pid, l->max_pid);
		else
			seq_printf(seq, " pid=%ld",
				l->min_pid);

		if (num_online_cpus() > 1 && !cpumask_empty(to_cpumask(l->cpus)))
			seq_printf(seq, " cpus=%*pbl",
				 cpumask_pr_args(to_cpumask(l->cpus)));

		if (nr_online_nodes > 1 && !nodes_empty(l->nodes))
			seq_printf(seq, " nodes=%*pbl",
				 nodemask_pr_args(&l->nodes));

#ifdef CONFIG_STACKDEPOT
		{
			depot_stack_handle_t handle;
			unsigned long *entries;
			unsigned int nr_entries, j;

			handle = READ_ONCE(l->handle);
			if (handle) {
				nr_entries = stack_depot_fetch(handle, &entries);
				seq_puts(seq, "\n");
				for (j = 0; j < nr_entries; j++)
					seq_printf(seq, "        %pS\n", (void *)entries[j]);
			}
		}
#endif
		seq_puts(seq, "\n");
	}

	if (!idx && !t->count)
		seq_puts(seq, "No data\n");

	return 0;
}

/* slab_debugfs_stop：seq_file stop 回调，此处无需释放资源（open 时一次性构建完毕） */
static void slab_debugfs_stop(struct seq_file *seq, void *v)
{
}

/* slab_debugfs_next：推进游标，返回下一条记录（超出范围返回 NULL 结束迭代） */
static void *slab_debugfs_next(struct seq_file *seq, void *v, loff_t *ppos)
{
	struct loc_track *t = seq->private;

	t->idx = ++(*ppos);
	if (*ppos <= t->count)
		return ppos;

	return NULL;
}

/* cmp_loc_by_count：按 count 降序排列（最常被分配的调用点排在最前） */
static int cmp_loc_by_count(const void *a, const void *b)
{
	struct location *loc1 = (struct location *)a;
	struct location *loc2 = (struct location *)b;

	return cmp_int(loc2->count, loc1->count);
}

/* slab_debugfs_start：seq_file start 回调，设置初始游标并返回迭代起点 */
static void *slab_debugfs_start(struct seq_file *seq, loff_t *ppos)
{
	struct loc_track *t = seq->private;

	t->idx = *ppos;
	return ppos;
}

static const struct seq_operations slab_debugfs_sops = {
	.start  = slab_debugfs_start,
	.next   = slab_debugfs_next,
	.stop   = slab_debugfs_stop,
	.show   = slab_debugfs_show,
};

/*
 * slab_debug_trace_open() - 打开 alloc_traces 或 free_traces 文件时的初始化
 *
 * 在 open 时（而非 read 时）一次性扫描所有 slab 并构建 loc_track：
 *   1. 分配 loc_track 私有结构（seq_file private data）
 *   2. 分配对象位图（判断空闲/已分配）
 *   3. 通过 debugfs_get_aux_num() 获取 alloc/free 方向（创建文件时写入）
 *   4. 初始化 loc_track 数组（PAGE_SIZE / sizeof(location) 条记录起始容量）
 *   5. 遍历所有节点的 partial + full 链表，调用 process_slab 聚合 track 记录
 *   6. 按 count 降序排序（热点调用点排前面）
 *
 * 一次性构建的好处：读取时无需再加锁，seq_file 可以安全地分页输出；
 * 代价是打开时可能耗时（遍历所有 slab），但 trace 文件仅在调试时使用。
 */
static int slab_debug_trace_open(struct inode *inode, struct file *filep)
{

	struct kmem_cache_node *n;
	enum track_item alloc;
	int node;
	struct loc_track *t = __seq_open_private(filep, &slab_debugfs_sops,
						sizeof(struct loc_track));
	struct kmem_cache *s = file_inode(filep)->i_private;
	unsigned long *obj_map;

	if (!t)
		return -ENOMEM;

	obj_map = bitmap_alloc(oo_objects(s->oo), GFP_KERNEL);
	if (!obj_map) {
		seq_release_private(inode, filep);
		return -ENOMEM;
	}

	alloc = debugfs_get_aux_num(filep);

	if (!alloc_loc_track(t, PAGE_SIZE / sizeof(struct location), GFP_KERNEL)) {
		bitmap_free(obj_map);
		seq_release_private(inode, filep);
		return -ENOMEM;
	}

	for_each_kmem_cache_node(s, node, n) {
		unsigned long flags;
		struct slab *slab;

		if (!node_nr_slabs(n))
			continue;

		spin_lock_irqsave(&n->list_lock, flags);
		list_for_each_entry(slab, &n->partial, slab_list)
			process_slab(t, s, slab, alloc, obj_map);
		list_for_each_entry(slab, &n->full, slab_list)
			process_slab(t, s, slab, alloc, obj_map);
		spin_unlock_irqrestore(&n->list_lock, flags);
	}

	/* Sort locations by count */
	sort(t->loc, t->count, sizeof(struct location),
	     cmp_loc_by_count, NULL);

	bitmap_free(obj_map);
	return 0;
}

/* slab_debug_trace_release：关闭文件，释放 loc_track 数组内存 */
static int slab_debug_trace_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct loc_track *t = seq->private;

	free_loc_track(t);
	return seq_release_private(inode, file);
}

static const struct file_operations slab_debugfs_fops = {
	.open    = slab_debug_trace_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = slab_debug_trace_release,
};

/*
 * debugfs_slab_add() - 为 cache 在 /sys/kernel/debug/slab/<name>/ 下创建调试文件
 *
 * 创建两个文件：
 *   alloc_traces（aux=TRACK_ALLOC）：显示按调用点聚合的分配热点统计
 *   free_traces（aux=TRACK_FREE）：  显示按调用点聚合的释放热点统计
 *
 * debugfs_create_file_aux_num 将 TRACK_ALLOC/FREE 整数嵌入文件，
 * open 时通过 debugfs_get_aux_num 读取，区分两个文件的行为。
 */
static void debugfs_slab_add(struct kmem_cache *s)
{
	struct dentry *slab_cache_dir;

	if (unlikely(!slab_debugfs_root))
		return;

	slab_cache_dir = debugfs_create_dir(s->name, slab_debugfs_root);

	debugfs_create_file_aux_num("alloc_traces", 0400, slab_cache_dir, s,
					TRACK_ALLOC, &slab_debugfs_fops);

	debugfs_create_file_aux_num("free_traces", 0400, slab_cache_dir, s,
					TRACK_FREE, &slab_debugfs_fops);
}

/* debugfs_slab_release()：cache 销毁时删除对应的 debugfs 目录树 */
void debugfs_slab_release(struct kmem_cache *s)
{
	debugfs_lookup_and_remove(s->name, slab_debugfs_root);
}

/*
 * slab_debugfs_init() - 初始化 /sys/kernel/debug/slab/ 目录
 *
 * 以 __initcall（默认优先级）注册，在 debugfs 挂载后执行。
 * 只为开启了 SLAB_STORE_USER 的 cache 创建调试目录（其他 cache 没有 track 记录）。
 * 后续新建的 SLAB_STORE_USER cache 由 do_kmem_cache_create() 中的
 * debugfs_slab_add() 直接注册，不需要再次遍历。
 */
static int __init slab_debugfs_init(void)
{
	struct kmem_cache *s;

	slab_debugfs_root = debugfs_create_dir("slab", NULL);

	list_for_each_entry(s, &slab_caches, list)
		if (s->flags & SLAB_STORE_USER)
			debugfs_slab_add(s);

	return 0;

}
__initcall(slab_debugfs_init);
#endif
/*
 * get_slabinfo() - 填充 /proc/slabinfo 的每行数据
 *
 * /proc/slabinfo 提供内核 slab 分配器的统计信息，供用户空间工具（如 slabtop）使用。
 * 遍历所有 NUMA 节点累计：
 *   nr_slabs：该 cache 的 slab 页总数
 *   nr_objs：所有 slab 页的对象总容量（= nr_slabs × oo_objects）
 *   nr_free：各节点 partial list 上的估算空闲对象数
 *
 * active_objs = nr_objs - nr_free（已分配对象数，near 实时但不精确）
 * count_partial_free_approx：不精确统计（不加锁），避免 /proc 读取时的高开销。
 */
#ifdef CONFIG_SLUB_DEBUG
void get_slabinfo(struct kmem_cache *s, struct slabinfo *sinfo)
{
	unsigned long nr_slabs = 0;
	unsigned long nr_objs = 0;
	unsigned long nr_free = 0;
	int node;
	struct kmem_cache_node *n;

	for_each_kmem_cache_node(s, node, n) {
		nr_slabs += node_nr_slabs(n);
		nr_objs += node_nr_objs(n);
		nr_free += count_partial_free_approx(n);
	}

	sinfo->active_objs = nr_objs - nr_free;
	sinfo->num_objs = nr_objs;
	sinfo->active_slabs = nr_slabs;
	sinfo->num_slabs = nr_slabs;
	sinfo->objects_per_slab = oo_objects(s->oo);
	sinfo->cache_order = oo_order(s->oo);
}
#endif /* CONFIG_SLUB_DEBUG */
