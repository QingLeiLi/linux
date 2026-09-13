/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MM_SLAB_H
#define MM_SLAB_H

/* include guard 使多层 mm/slab 头文件可重入，避免结构定义在一个编译单元重复出现。 */

#include <linux/reciprocal_div.h>
#include <linux/list_lru.h>
#include <linux/local_lock.h>
#include <linux/random.h>
#include <linux/kobject.h>
#include <linux/sched/mm.h>
#include <linux/memcontrol.h>
#include <linux/kfence.h>
#include <linux/kasan.h>
#include <linux/slab.h>

/* 本头文件位于 slab common 与 SLUB 实现之间：声明的字段布局是二者共享 ABI。 */

/*
 * Internal slab definitions
 */

/*
 * 分配器内部的短生命周期控制位：它们不是 GFP 语义，而是把调用上下文
 * （无锁、首次建立 slab-object-extension、禁止递归）传给 SLUB 后端。
 */
/* slab's alloc_flags definitions */
#define SLAB_ALLOC_DEFAULT	0x00 /* no flags */
#define SLAB_ALLOC_NOLOCK	0x01 /* a kmalloc_nolock() allocation */
#define SLAB_ALLOC_NEW_SLAB	0x02 /* a flag for alloc_slab_obj_exts() */
#define SLAB_ALLOC_NO_RECURSE	0x04 /* prevent kmalloc() recursion */

static inline bool alloc_flags_allow_spinning(const unsigned int alloc_flags)
{
	/* kmalloc_nolock 的调用方不能睡眠/自旋等待，因此快路径必须避开锁竞争。 */
	return !(alloc_flags & SLAB_ALLOC_NOLOCK);
}

/* 带 token 的底层 kmalloc ABI；token 供 alloc_hooks/性能归因保留调用点身份。 */
void *__kmalloc_flags_noprof(DECL_TOKEN_PARAMS(size, token), gfp_t flags,
				  unsigned int alloc_flags, int node)
				  __assume_kmalloc_alignment __alloc_size(1);

static __always_inline __alloc_size(1) void *_kmalloc_flags_noprof(size_t size,
		gfp_t flags, unsigned int alloc_flags, int node, kmalloc_token_t token)
{
	/* 统一把自动生成的 token 补到真实实现，调用者无需手工维护它。 */
	return __kmalloc_flags_noprof(PASS_TOKEN_PARAMS(size, token), flags, alloc_flags, node);
}
#define kmalloc_flags_noprof(...)	_kmalloc_flags_noprof(__VA_ARGS__, __kmalloc_token(__VA_ARGS__))
#define kmalloc_flags(...)		alloc_hooks(kmalloc_flags_noprof(__VA_ARGS__))

#ifdef CONFIG_64BIT
/* 与指针同宽的双字 CAS 容器；架构不支持时退化到带锁或单字更新路径。 */
# ifdef system_has_cmpxchg128
# define system_has_freelist_aba()	system_has_cmpxchg128()
# define try_cmpxchg_freelist		try_cmpxchg128
# endif
typedef u128 freelist_full_t;
#else /* CONFIG_64BIT */
/* 32 位平台若有 64 位 cmpxchg，同样把指针和计数绑定；否则不定义 ABA fastpath。 */
# ifdef system_has_cmpxchg64
# define system_has_freelist_aba()	system_has_cmpxchg64()
# define try_cmpxchg_freelist		try_cmpxchg64
# endif
typedef u64 freelist_full_t;
#endif /* CONFIG_64BIT */

#if defined(system_has_freelist_aba) && !defined(CONFIG_HAVE_ALIGNED_STRUCT_PAGE)
/* page 未保证所需对齐时，禁止宣称双字 CAS 安全，宁可放弃 ABA 优化。 */
#undef system_has_freelist_aba
#endif

/*
 * Freelist pointer and counter to cmpxchg together, avoids the typical ABA
 * problems with cmpxchg of just a pointer.
 *
 * 学习要点：空闲链表地址回到旧值并不表示状态未变；把地址与 inuse/objects
 * 作为一个机器字 CAS，才能让 lockless fastpath 识别中间的 pop/push 循环。
 */
struct freelist_counters {
	union {
		struct {
			void *freelist;
			/* freelist 指向下一个空闲对象，编码/解码细节由具体 SLUB 后端负责。 */
			union {
				unsigned long counters;
				struct {
					unsigned inuse:16;
					unsigned objects:15;
					/* inuse + 空闲链表长度隐含 objects，更新必须与 freelist 一起提交。 */
					/*
					 * If slab debugging is enabled then the
					 * frozen bit can be reused to indicate
					 * that the slab was corrupted
					 */
					unsigned frozen:1;
#ifdef CONFIG_64BIT
					/*
					 * Some optimizations use free bits in 'counters' field
					 * to save memory. In case ->stride field is not available,
					 * such optimizations are disabled.
					 */
					unsigned int stride;
#endif
				};
			};
		};
	/* freelist_counters 必须整体原子更新，拆开读写会破坏 freelist 与计数不变量。 */
#ifdef system_has_freelist_aba
		freelist_full_t freelist_counters;
#endif
	};
};

/*
 * struct slab 与 folio 首个 struct page 做 ABI overlay，不能新增会破坏 page
 * 布局的字段；下面的静态断言把这一跨子系统约定变成编译期失败。
 */
/* Reuses the bits in struct page */
struct slab {
	/* slab_list 仅由节点/partial 管理持有；被 RCU 延迟释放时同一存储复用为 rcu_head。 */
	memdesc_flags_t flags;

	struct kmem_cache *slab_cache;
	/* slab_cache 决定对象步长、调试布局及释放应回到的 cache，不能按地址猜测。 */
	union {
		struct {
			struct list_head slab_list;
			/* Double-word boundary */
			struct freelist_counters;
		};
		struct rcu_head rcu_head;
	};

	unsigned int __page_type;
	/* page_type 的 PGTY_slab 是 page_slab 无锁筛选的唯一身份位。 */
	atomic_t __page_refcount;
	/* 引用归零不等于可立即复用：RCU/debug/obj-ext 等额外协议仍可能延后回收。 */
#ifdef CONFIG_SLAB_OBJ_EXT
	unsigned long obj_exts;
#endif
};

#define SLAB_MATCH(pg, sl)						\
	static_assert(offsetof(struct page, pg) == offsetof(struct slab, sl))
SLAB_MATCH(flags, flags);
SLAB_MATCH(compound_info, slab_cache);	/* Ensure bit 0 is clear */
SLAB_MATCH(_refcount, __page_refcount);
/* 每一个 SLAB_MATCH 都是 overlay 合法性的证据；字段偏移漂移不能靠运行时修复。 */
#ifdef CONFIG_MEMCG
SLAB_MATCH(memcg_data, obj_exts);
#elif defined(CONFIG_SLAB_OBJ_EXT)
SLAB_MATCH(_unused_slab_obj_exts, obj_exts);
#endif
#undef SLAB_MATCH
static_assert(sizeof(struct slab) <= sizeof(struct page));
/* 余下 page 字段仍由 compound folio 使用；slab overlay 只借用约定的前缀。 */
#if defined(system_has_freelist_aba)
static_assert(IS_ALIGNED(offsetof(struct slab, freelist), sizeof(struct freelist_counters)));
#endif

/**
 * slab_folio - The folio allocated for a slab
 * @s: The slab.
 *
 * Slabs are allocated as folios that contain the individual objects and are
 * using some fields in the first struct page of the folio - those fields are
 * now accessed by struct slab. It is occasionally necessary to convert back to
 * a folio in order to communicate with the rest of the mm.  Please use this
 * helper function instead of casting yourself, as the implementation may change
 * in the future.
 */
#define slab_folio(s)		(_Generic((s),				\
	const struct slab *:	(const struct folio *)s,		\
	struct slab *:		(struct folio *)s))

/* _Generic 保留 const 属性，避免调用者转换到 folio 后意外获得可写别名。 */

/**
 * page_slab - Converts from struct page to its slab.
 * @page: A page which may or may not belong to a slab.
 *
 * Return: The slab which contains this page or NULL if the page does
 * not belong to a slab.  This includes pages returned from large kmalloc.
 */
static inline struct slab *page_slab(const struct page *page)
{
	/* 先折叠 tail page，再用 page_type 的 racy 读取作无锁资格筛选；失败返回 NULL。 */
	page = compound_head(page);
	if (data_race(page->page_type >> 24) != PGTY_slab)
		page = NULL;

	return (struct slab *)page;
}

/**
 * slab_page - The first struct page allocated for a slab
 * @s: The slab.
 *
 * A convenience wrapper for converting slab to the first struct page of the
 * underlying folio, to communicate with code not yet converted to folio or
 * struct slab.
 */
#define slab_page(s) folio_page(slab_folio(s), 0)

/* legacy page API 的桥接只取 folio 第 0 页；tail page 不能承载 slab 元数据。 */

static inline void *slab_address(const struct slab *slab)
{
	/* 返回 folio 的首虚拟地址；对象索引与边界计算都以它为原点。 */
	return folio_address(slab_folio(slab));
}

static inline int slab_nid(const struct slab *slab)
{
	/* flags 中保存 memdesc NUMA 节点，避免为 slab 再维护一份 node 字段。 */
	return memdesc_nid(slab->flags);
}

static inline pg_data_t *slab_pgdat(const struct slab *slab)
{
	/* NODE_DATA 的结果只适用于 online node；热插拔路径需自行处理生命周期。 */
	return NODE_DATA(slab_nid(slab));
}

static inline struct slab *virt_to_slab(const void *addr)
{
	/* 这是地址到 slab 的便利反查，非 slab（含 large kmalloc）会得到 NULL。 */
	return page_slab(virt_to_page(addr));
}

static inline int slab_order(const struct slab *slab)
{
	/* order 决定 backing folio 页数，也决定对象总容量和 reclaim 粒度。 */
	return folio_order(slab_folio(slab));
}

static inline size_t slab_size(const struct slab *slab)
{
	/* 物理容量而非可用对象字节数；redzone/对齐会消耗其中一部分空间。 */
	return PAGE_SIZE << slab_order(slab);
}

/*
 * Word size structure that can be atomically updated or read and that
 * contains both the order and the number of objects that a slab of the
 * given order would contain.
 */
/* 打包 order 与 objects 的原子快照，避免读者看到不匹配的布局组合。 */
struct kmem_cache_order_objects {
	unsigned int x;
};

struct kmem_cache_per_node_ptrs {
	/* barn 服务跨 CPU sheaf 交接，node 承载该 NUMA 节点的 partial/full 状态。 */
	struct node_barn *barn;
	struct kmem_cache_node *node;
};

/*
 * Slab cache management.
 */
/*
 * cache 是对象格式和全局策略的所有者；per_node 指针指向节点级 partial
 * 管理，而 cpu_sheaves 属于每 CPU 快路径。销毁时必须先阻断这些使用者。
 */
struct kmem_cache {
	struct slub_percpu_sheaves __percpu *cpu_sheaves;
	/* Used for retrieving partial slabs, etc. */
	slab_flags_t flags;
	unsigned long min_partial;
	unsigned int size;		/* Object size including metadata */
	unsigned int object_size;	/* Object size without metadata */
	/* reciprocal_size 在创建 cache 时预计算，所有对象索引热点复用该除法倒数。 */
	struct reciprocal_value reciprocal_size;
	unsigned int offset;		/* Free pointer offset */
	unsigned int sheaf_capacity;
	/* size 包含 redzone/metadata，object_size 才是分配者请求对象的可用范围。 */
	struct kmem_cache_order_objects oo;

	/* Allocation and freeing of slabs */
	struct kmem_cache_order_objects min;
	gfp_t allocflags;		/* gfp flags to use on each alloc */
	int refcount;			/* Refcount for slab cache destroy */
	void (*ctor)(void *object);	/* Object constructor */
	unsigned int inuse;		/* Offset to metadata */
	unsigned int align;		/* Alignment */
	unsigned int red_left_pad;	/* Left redzone padding size */
	/* inuse 是对象后元数据起点，offset 指出空闲对象中链接指针的位置。 */
	const char *name;		/* Name (only for display!) */
	struct list_head list;		/* List of slab caches */
	/* list 受 slab_mutex 保护；name 主要诊断用，创建者须保证其生命周期。 */
#ifdef CONFIG_SYSFS
	struct kobject kobj;		/* For sysfs */
#endif
#ifdef CONFIG_SLAB_FREELIST_HARDENED
	unsigned long random;
#endif

#ifdef CONFIG_NUMA
	/* 远端 defrag 是空间利用率策略；它可能牺牲局部性以减少 partial slab 数量。 */
	/*
	 * Defragmentation by allocating from a remote node.
	 */
	unsigned int remote_node_defrag_ratio;
#endif

#ifdef CONFIG_SLAB_FREELIST_RANDOM
	/* random_seq 改变对象发放顺序；销毁 cache 时须由对应 destroy 释放其 backing。 */
	unsigned int *random_seq;
#endif

#ifdef CONFIG_KASAN_GENERIC
	/* kasan_info 是 cache 级 shadow/redzone 参数，分配/释放 hook 依赖它解释对象边界。 */
	struct kasan_cache kasan_info;
#endif

#ifdef CONFIG_HARDENED_USERCOPY
	/* useroffset/usersize 把可 copy_to/from_user 的子区显式白名单化。 */
	unsigned int useroffset;	/* Usercopy region offset */
	unsigned int usersize;		/* Usercopy region size */
#endif

#ifdef CONFIG_SLUB_STATS
	/* 统计为 per-CPU，读取聚合时必须接受并发更新造成的近似值。 */
	struct kmem_cache_stats __percpu *cpu_stats;
#endif

	struct kmem_cache_per_node_ptrs per_node[MAX_NUMNODES];
};

/* per_node 槽即使 NUMA 未启用仍提供统一索引形状，实际实现决定哪些槽会分配。 */

/*
 * Every cache has !NULL s->cpu_sheaves but they may point to the
 * bootstrap_sheaf temporarily during init, or permanently for the boot caches
 * and caches with debugging enabled, or all caches with CONFIG_SLUB_TINY. This
 * helper distinguishes whether cache has real non-bootstrap sheaves.
 */
static inline bool cache_has_sheaves(struct kmem_cache *s)
{
	/* bootstrap/debug/tiny 可有非 NULL 占位指针，容量才是“真实 sheaf”的判据。 */
	/* Test CONFIG_SLUB_TINY for code elimination purposes */
	return !IS_ENABLED(CONFIG_SLUB_TINY) && s->sheaf_capacity;
}

/* 该判断不加锁，因为创建后 sheaf_capacity 不再改变；销毁路径先停止并发分配。 */

#if defined(CONFIG_SYSFS) && !defined(CONFIG_SLUB_TINY)
/* sysfs 仅在完整 SLUB 可见；tiny 配置用静态桩消除对象管理开销。 */
#define SLAB_SUPPORTS_SYSFS 1
void sysfs_slab_unlink(struct kmem_cache *s);
void sysfs_slab_release(struct kmem_cache *s);
int sysfs_slab_alias(struct kmem_cache *s, const char *name);
	/* unlink/release 处理可见性与 kobject 引用；alias 失败时创建者负责回滚。 */
#else
static inline void sysfs_slab_unlink(struct kmem_cache *s) { }
static inline void sysfs_slab_release(struct kmem_cache *s) { }
static inline int sysfs_slab_alias(struct kmem_cache *s, const char *name)
							{ return 0; }
#endif

void *fixup_red_left(struct kmem_cache *s, void *p);

/* red_left_pad 改变用户对象的可见起点，任何由槽地址回推对象的路径都要经此修正。 */

static inline void *nearest_obj(struct kmem_cache *cache,
				const struct slab *slab, void *x)
{
	/* 将任意地址向下量化到对象槽；尾部越界时钳到最后对象，再修正左 redzone。 */
	void *object = x - (x - slab_address(slab)) % cache->size;
	void *last_object = slab_address(slab) +
		(slab->objects - 1) * cache->size;
	void *result = (unlikely(object > last_object)) ? last_object : object;

	result = fixup_red_left(cache, result);
	return result;
}

/* 诊断调用传入的 x 允许落在 redzone；返回值是最接近的用户对象而不是原始槽地址。 */

/* Determine object index from a given position */
static inline unsigned int __obj_to_index(const struct kmem_cache *cache,
					  void *addr, const void *obj)
{
	/* 先剥 KASAN pointer tag，再以 reciprocal 乘法替代热点除法。 */
	return reciprocal_divide(kasan_reset_tag(obj) - addr,
				 cache->reciprocal_size);
}

static inline unsigned int obj_to_index(const struct kmem_cache *cache,
					const struct slab *slab, const void *obj)
{
	/* KFENCE 对象不属于连续 slab 布局，约定索引 0，禁止常规地址差计算。 */
	if (is_kfence_address(obj))
		return 0;
	return __obj_to_index(cache, slab_address(slab), obj);
}

static inline int objs_per_slab(const struct kmem_cache *cache,
				const struct slab *slab)
{
	/* objects 已是 slab 建立时固定的布局结果，不能由 size 反推。 */
	return slab->objects;
}

/* cache 参数保留接口一致性，未来若 objects 从布局字段重建不必改变调用点。 */

/*
 * State of the slab allocator.
 *
 * This is used to describe the states of the allocator during bootup.
 * Allocators use this to gradually bootstrap themselves. Most allocators
 * have the problem that the structures used for managing slab caches are
 * allocated from slab caches themselves.
 */
enum slab_state {
	DOWN,			/* No slab functionality yet */
	PARTIAL,		/* SLUB: kmem_cache_node available */
	UP,			/* Slab caches usable but not all extras yet */
	FULL			/* Everything is working */
};

/* DOWN→PARTIAL→UP→FULL 单向推进，读者据此选择可用的管理结构集合。 */

/* 启动状态机防止 bootstrap cache 尚不可用时发生递归分配。 */

extern enum slab_state slab_state;

/* The slab cache mutex protects the management structures during changes */
extern struct mutex slab_mutex;

/* The list of all slab caches on the system */
extern struct list_head slab_caches;

/* The slab cache that manages slab cache information */
extern struct kmem_cache *kmem_cache;

/* kmem_cache 自举时既是管理对象的 cache 又是依赖 slab 的消费者，状态机打破环依赖。 */

/* A table of kmalloc cache names and sizes */
extern const struct kmalloc_info_struct {
	const char *name[NR_KMALLOC_TYPES];
	unsigned int size;
} kmalloc_info[];

/* Kmalloc array related functions */
void setup_kmalloc_cache_index_table(void);
void create_kmalloc_caches(void);

extern u8 kmalloc_size_index[24];

/* 小尺寸表比 fls 更细粒度，192 以上恰好可用二进制阶数选择 bucket。 */

static inline unsigned int size_index_elem(unsigned int bytes)
{
	/* 8 字节分桶表以 size-1 实现上取整；调用者保证 bytes 非零。 */
	return (bytes - 1) / 8;
}

/*
 * Find the kmem_cache structure that serves a given size of
 * allocation
 *
 * This assumes size is larger than zero and not larger than
 * KMALLOC_MAX_CACHE_SIZE and the caller must check that.
 */
static inline struct kmem_cache *
kmalloc_slab(size_t size, kmem_buckets *b, gfp_t flags, kmalloc_token_t token)
{
	unsigned int index;

	/* bucket 随 GFP/token 类型（普通、DMA、account 等）分流，随后按大小选 cache。 */
	if (!b)
		b = &kmalloc_caches[kmalloc_type(flags, token)];
	if (size <= 192)
		index = kmalloc_size_index[size_index_elem(size)];
	else
		index = fls(size - 1);

	return (*b)[index];
}

gfp_t kmalloc_fix_flags(gfp_t flags);

/* 此规范化会补齐架构/策略约束；上层应传原始 GFP，让后端集中解释。 */

/* Functions provided by the slab allocators */
int do_kmem_cache_create(struct kmem_cache *s, const char *name,
			 unsigned int size, struct kmem_cache_args *args,
			 slab_flags_t flags);

void __init kmem_cache_init(void);

/* init 完成后 slab_state 逐级发布；非启动路径不应调用 create_boot_cache。 */
extern void create_boot_cache(struct kmem_cache *, const char *name,
			unsigned int size, slab_flags_t flags,
			unsigned int useroffset, unsigned int usersize);

int slab_unmergeable(struct kmem_cache *s);
bool slab_args_unmergeable(struct kmem_cache_args *args, slab_flags_t flags);

slab_flags_t kmem_cache_flags(slab_flags_t flags, const char *name);

/* merge 判定依赖最终 flags/args；一旦 merge，两个名称共享同一对象布局与隔离边界。 */

static inline bool is_kmalloc_cache(struct kmem_cache *s)
{
	/* 该位标记通用 kmalloc cache，与用户显式 kmem_cache_create 的 cache 区分。 */
	return (s->flags & SLAB_KMALLOC);
}

static inline bool is_kmalloc_normal(struct kmem_cache *s)
{
	/* “normal” 排除 DMA、memcg-account 和 reclaim-account 专用 cache。 */
	if (!is_kmalloc_cache(s))
		return false;
	return !(s->flags & (SLAB_CACHE_DMA|SLAB_ACCOUNT|SLAB_RECLAIM_ACCOUNT));
}

/* 注意：cache flags 是创建后稳定的布局契约，运行期不能随意切换 debug/类型位。 */

bool __kfree_rcu_sheaf(struct kmem_cache *s, void *obj);
void flush_all_rcu_sheaves(void);
void flush_rcu_sheaves_on_cache(struct kmem_cache *s);

/* RCU sheaf 使释放对象延后可复用；cache 销毁前必须 flush，避免回调触达已释放 cache。 */

/* core 控制布局/生命周期，debug 控制诊断；校验入口仅接受这两类公开位。 */
#define SLAB_CORE_FLAGS (SLAB_HWCACHE_ALIGN | SLAB_CACHE_DMA | \
			 SLAB_CACHE_DMA32 | SLAB_PANIC | \
			 SLAB_TYPESAFE_BY_RCU | SLAB_DEBUG_OBJECTS | \
			 SLAB_NOLEAKTRACE | SLAB_RECLAIM_ACCOUNT | \
			 SLAB_TEMPORARY | SLAB_ACCOUNT | \
			 SLAB_NO_USER_FLAGS | SLAB_KMALLOC | SLAB_NO_MERGE)

#define SLAB_DEBUG_FLAGS (SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER | \
			  SLAB_TRACE | SLAB_CONSISTENCY_CHECKS)

#define SLAB_FLAGS_PERMITTED (SLAB_CORE_FLAGS | SLAB_DEBUG_FLAGS)

/* 向 kmem_cache_create 传入未知位必须在统一入口拒绝，防止后端各自静默解释。 */

bool __kmem_cache_empty(struct kmem_cache *);
int __kmem_cache_shutdown(struct kmem_cache *);
void __kmem_cache_release(struct kmem_cache *);
int __kmem_cache_shrink(struct kmem_cache *);
void slab_kmem_cache_release(struct kmem_cache *);

/* shutdown/empty/shrink 分别回答可否关闭、是否无对象、能否回收 backing slab，语义不可混用。 */

struct seq_file;
struct file;

/* /proc/slabinfo 的快照载体；数值可能在采样后变化，不能作为强一致统计。 */
struct slabinfo {
	unsigned long active_objs;
	unsigned long num_objs;
	unsigned long active_slabs;
	unsigned long num_slabs;
	unsigned long shared_avail;
	/* shared_avail/limit/batchcount 来自后端兼容统计，未必等同于当前 SLUB 本地缓存。 */
	unsigned int limit;
	unsigned int batchcount;
	unsigned int shared;
	unsigned int objects_per_slab;
	unsigned int cache_order;
};

/* active_* 是正在使用的对象/slab，num_* 是总量；shared 字段保留给兼容统计接口。 */

void get_slabinfo(struct kmem_cache *s, struct slabinfo *sinfo);

/* 采样者提供 sinfo 存储；实现负责在相应锁或统计协议下填充，而非转移 cache 所有权。 */

#ifdef CONFIG_SLUB_DEBUG
/* 调试 static key 避免生产快路径为每次分配支付检查成本。 */
#ifdef CONFIG_SLUB_DEBUG_ON
DECLARE_STATIC_KEY_TRUE(slub_debug_enabled);
#else
DECLARE_STATIC_KEY_FALSE(slub_debug_enabled);
#endif

/* 未编译 SLUB debug 时 print_tracking 是无副作用桩，调用点无需 #ifdef。 */
extern void print_tracking(struct kmem_cache *s, void *object);
long validate_slab_cache(struct kmem_cache *s);
static inline bool __slub_debug_enabled(void)
{
	return static_branch_unlikely(&slub_debug_enabled);
}

/* validate_slab_cache 用于诊断而非普通分配；其返回错误不能替代对象所有权检查。 */
#else
static inline void print_tracking(struct kmem_cache *s, void *object)
{
}
static inline bool __slub_debug_enabled(void)
{
	return false;
}
#endif

/* static key 在 boot 参数解析后切换，不能仅凭 s->flags 推断调试路径一定已激活。 */

/*
 * Returns true if any of the specified slab_debug flags is enabled for the
 * cache. Use only for flags parsed by setup_slub_debug() as it also enables
 * the static key.
 */
static inline bool kmem_cache_debug_flags(struct kmem_cache *s, slab_flags_t flags)
{
	/* static key 关闭时直接为 false，既省分支也确保未启用调试位不会被误解。 */
	if (IS_ENABLED(CONFIG_SLUB_DEBUG))
		VM_WARN_ON_ONCE(!(flags & SLAB_DEBUG_FLAGS));
	if (__slub_debug_enabled())
		return s->flags & flags;
	return false;
}

#if IS_ENABLED(CONFIG_SLUB_DEBUG) && IS_ENABLED(CONFIG_KUNIT)
/* KUnit 标记让测试可启用特定诊断例外，生产构建返回 false。 */
bool slab_in_kunit_test(void);
#else
static inline bool slab_in_kunit_test(void) { return false; }
#endif

/*
 * slub is about to manipulate internal object metadata.  This memory lies
 * outside the range of the allocated object, so accessing it would normally
 * be reported by kasan as a bounds error.  metadata_access_enable() is used
 * to tell kasan that these accesses are OK.
 */
static inline void metadata_access_enable(void)
{
	/* 成对临界区：暂抑 KASAN/KMSAN，允许 allocator 访问对象边界外的元数据。 */
	kasan_disable_current();
	kmsan_disable_current();
}

static inline void metadata_access_disable(void)
{
	/* 恢复顺序与 enable 对称；不可跨可能调度的路径泄漏此状态。 */
	kmsan_enable_current();
	kasan_enable_current();
}

/* sanitizer 屏蔽只覆盖 allocator 自己的 metadata，绝不可围住用户构造函数。 */

#ifdef CONFIG_SLAB_OBJ_EXT

/* object extension 与对象本体分配隔离，供 KASAN/memcg 等附加状态使用，不改变用户对象 ABI。 */

/*
 * slab_obj_exts - get the pointer to the slab object extension vector
 * associated with a slab.
 * @slab: a pointer to the slab struct
 *
 * Returns the address of the object extension vector associated with the slab,
 * or zero if no such vector has been associated yet.
 * Do not dereference the return value directly; use get/put_slab_obj_exts()
 * pair and slab_obj_ext() to access individual elements.
 *
 * Example usage:
 *
 * obj_exts = slab_obj_exts(slab);
 * if (obj_exts) {
 *         get_slab_obj_exts(obj_exts);
 *         obj_ext = slab_obj_ext(slab, obj_exts, obj_to_index(s, slab, obj));
 *         // do something with obj_ext
 *         put_slab_obj_exts(obj_exts);
 * }
 *
 * Note that the get/put semantics does not involve reference counting.
 * Instead, it updates kasan/kmsan depth so that accesses to slabobj_ext
 * won't be reported as access violations.
 */
static inline unsigned long slab_obj_exts(struct slab *slab)
{
	/* READ_ONCE 与释放路径并发：取到的地址只可在下面 get/put 范围内使用。 */
	unsigned long obj_exts = READ_ONCE(slab->obj_exts);
	/* 0 表示尚未分配 extension vector；调用方应把它当普通的可选功能。 */

#ifdef CONFIG_MEMCG
	/* memcg 使用低位/辅助数据标识 extension 的归属，释放时需走相同解释规则。 */
	/*
	 * obj_exts should be either NULL, a valid pointer with
	 * MEMCG_DATA_OBJEXTS bit set or be equal to OBJEXTS_ALLOC_FAIL.
	 */
	VM_BUG_ON_PAGE(obj_exts && !(obj_exts & MEMCG_DATA_OBJEXTS) &&
		       obj_exts != OBJEXTS_ALLOC_FAIL, slab_page(slab));
	VM_BUG_ON_PAGE(obj_exts & MEMCG_DATA_KMEM, slab_page(slab));
#endif

	return obj_exts & ~OBJEXTS_FLAGS_MASK;
}

static inline void get_slab_obj_exts(unsigned long obj_exts)
{
	/* 这不是 refcount；KASAN/KMSAN 深度使 extension metadata 暂时可访问。 */
	VM_WARN_ON_ONCE(!obj_exts);
	metadata_access_enable();
}

/* obj_exts 的位模式由配置决定，调用者只传递 opaque 值，不能私自做指针算术。 */

static inline void put_slab_obj_exts(unsigned long obj_exts)
{
	/* 必须与 get 精确配对，避免 sanitizer 禁用深度泄漏到后续 allocator 操作。 */
	metadata_access_disable();
}

#ifdef CONFIG_64BIT
static inline void slab_set_stride(struct slab *slab, unsigned int stride)
{
	/* 32 位固定布局仅校验调用者没有传入错误步长，不保存该参数。 */
	/* 64 位复用 counters 的空闲位保存扩展元素步长，节省独立字段。 */
	slab->stride = stride;
}
static inline unsigned int slab_get_stride(struct slab *slab)
{
	/* 读取布局固定后的 stride；调用方据此从 vector 定位第 index 个元素。 */
	return slab->stride;
}
#else

/* 32 位没有可复用的 counters 高位，stride 固定为 struct slabobj_ext 大小。 */
static inline void slab_set_stride(struct slab *slab, unsigned int stride)
{
	VM_WARN_ON_ONCE(stride != sizeof(struct slabobj_ext));
}
static inline unsigned int slab_get_stride(struct slab *slab)
{
	/* 固定 sizeof 保证 32 位与分配端的 vector 紧密排布一致。 */
	return sizeof(struct slabobj_ext);
}
#endif

/*
 * slab_obj_ext - get the pointer to the slab object extension metadata
 * associated with an object in a slab.
 * @slab: a pointer to the slab struct
 * @obj_exts: a pointer to the object extension vector
 * @index: an index of the object
 *
 * Returns a pointer to the object extension associated with the object.
 * Must be called within a section covered by get/put_slab_obj_exts().
 */
static inline struct slabobj_ext *slab_obj_ext(struct slab *slab,
					       unsigned long obj_exts,
					       unsigned int index)
{
	/* WARN 捕捉“vector 已更换仍使用旧快照”；返回前复位 tag 供普通解引用。 */
	struct slabobj_ext *obj_ext;

	VM_WARN_ON_ONCE(obj_exts != slab_obj_exts(slab));

	obj_ext = (struct slabobj_ext *)(obj_exts +
					 slab_get_stride(slab) * index);
	return kasan_reset_tag(obj_ext);
}

/* 返回值只在 get_slab_obj_exts 覆盖期有效；put 后不能缓存到对象私有字段。 */

/* index 必须来自同一 slab 的 obj_to_index；跨 slab 混用会通过 WARN 暴露而非自动修复。 */

int alloc_slab_obj_exts(struct slab *slab, struct kmem_cache *s,
			gfp_t gfp, unsigned int alloc_flags);

/* allocation 的失败不应留下半发布 obj_exts；实现需在释放 slab 前撤销或完整初始化。 */

#else /* CONFIG_SLAB_OBJ_EXT */

/* 未启用扩展元数据时保留同名零成本桩，调用点不必散布条件编译。 */

static inline unsigned long slab_obj_exts(struct slab *slab)
{
	/* feature-off 返回 0，方便同一调用流在运行时以 if (obj_exts) 短路。 */
	return 0;
}

static inline struct slabobj_ext *slab_obj_ext(struct slab *slab,
					       unsigned long obj_exts,
					       unsigned int index)
{
	/* 参数在 feature-off 下故意未使用；NULL 迫使调用者不解引用扩展。 */
	return NULL;
}

/* 这两个桩保留 set/get 契约，使公共代码无需知道目标字长或 feature 配置。 */
static inline void slab_set_stride(struct slab *slab, unsigned int stride) { }
static inline unsigned int slab_get_stride(struct slab *slab) { return 0; }


#endif /* CONFIG_SLAB_OBJ_EXT */

/* 关闭 feature 的桩返回安全零值，调用者据返回值跳过 metadata 工作。 */

static inline enum node_stat_item cache_vmstat_idx(struct kmem_cache *s)
{
	/* reclaim-account cache 计入可回收 slab，其余计入不可回收口径。 */
	return (s->flags & SLAB_RECLAIM_ACCOUNT) ?
		NR_SLAB_RECLAIMABLE_B : NR_SLAB_UNRECLAIMABLE_B;
}

#ifdef CONFIG_MEMCG

/* memcg hooks 在对象发布/回收边界接管记账，p 允许 hook 替换或拒绝对象指针。 */
bool __memcg_slab_post_alloc_hook(struct kmem_cache *s, struct list_lru *lru,
				  gfp_t flags, unsigned int slab_alloc_flags,
				  size_t size, void **p);
void __memcg_slab_free_hook(struct kmem_cache *s, struct slab *slab,
			    void **p, int objects, unsigned long obj_exts);
#endif

void kvfree_rcu_cb(struct rcu_head *head);

/* kvfree 的 RCU 回调把真正释放推迟到 grace period，head 所在对象在此前仍须有效。 */

static inline unsigned int large_kmalloc_order(const struct page *page)
{
	/* large kmalloc 把 order 编码在 compound 后续 page 的 flags 低字节，属内部 ABI。 */
	return page[1].flags.f & 0xff;
}

static inline size_t large_kmalloc_size(const struct page *page)
{
	/* 只适用于 large kmalloc 的首 page；普通 slab 对象不可拿它求大小。 */
	return PAGE_SIZE << large_kmalloc_order(page);
}

#ifdef CONFIG_SLUB_DEBUG
void dump_unreclaimable_slab(void);
#else
static inline void dump_unreclaimable_slab(void)
{
}
#endif

void ___cache_free(struct kmem_cache *cache, void *x, unsigned long addr);

/* 这是已知 cache 的底层释放入口；addr 记录调用点用于 debug/store-user。 */

#ifdef CONFIG_SLAB_FREELIST_RANDOM

/* 创建失败时 cache 不应发布 random_seq；destroy 只接管成功创建后的序列。 */
int cache_random_seq_create(struct kmem_cache *cachep, unsigned int count,
			gfp_t gfp);
void cache_random_seq_destroy(struct kmem_cache *cachep);

/* 随机序列只影响 freelist 次序，不改变对象所有权：分配/释放仍必须严格配对。 */
#else
static inline int cache_random_seq_create(struct kmem_cache *cachep,
					unsigned int count, gfp_t gfp)
{
	return 0;
}
static inline void cache_random_seq_destroy(struct kmem_cache *cachep) { }
#endif /* CONFIG_SLAB_FREELIST_RANDOM */

static inline bool slab_want_init_on_alloc(gfp_t flags, struct kmem_cache *c)
{
	/* ctor/RCU-safe/poison cache 不可被全局初始化策略破坏，只接受显式 __GFP_ZERO。 */
	if (static_branch_maybe(CONFIG_INIT_ON_ALLOC_DEFAULT_ON,
				&init_on_alloc)) {
		if (c->ctor)
			/* ctor 自行定义初始化，通用 memset 可能破坏其前置状态。 */
			return false;
		if (c->flags & (SLAB_TYPESAFE_BY_RCU | SLAB_POISON))
			return flags & __GFP_ZERO;
		return true;
	}
	return flags & __GFP_ZERO;
}

static inline bool slab_want_init_on_free(struct kmem_cache *c)
{
	/* free 初始化同样避开 ctor 与调试语义；这是安全策略选择，不保证调用时机。 */
	if (static_branch_maybe(CONFIG_INIT_ON_FREE_DEFAULT_ON,
				&init_on_free))
		return !(c->ctor ||
			 (c->flags & (SLAB_TYPESAFE_BY_RCU | SLAB_POISON)));
	return false;
}

/* init-on-alloc/free 为不同攻击面服务，二者均受 ctor 和 poison 的语义约束。 */

#if defined(CONFIG_DEBUG_FS) && defined(CONFIG_SLUB_DEBUG)
/* debugfs 节点与 cache 生命周期绑定，release 必须在 cache 内存回收之前完成。 */
void debugfs_slab_release(struct kmem_cache *);
#else
static inline void debugfs_slab_release(struct kmem_cache *s) { }
#endif

#ifdef CONFIG_PRINTK
#define KS_ADDRS_COUNT 16
struct kmem_obj_info {
	/* printk 诊断快照：对象、所属 slab/cache 与 alloc/free 调用栈由实现填充。 */
	void *kp_ptr;
	struct slab *kp_slab;
	void *kp_objp;
	unsigned long kp_data_offset;
	struct kmem_cache *kp_slab_cache;
	void *kp_ret;
	void *kp_stack[KS_ADDRS_COUNT];
	void *kp_free_stack[KS_ADDRS_COUNT];
};

/* 调用栈数量有上限，诊断信息是截断快照，不保证完整历史。 */
void __kmem_obj_info(struct kmem_obj_info *kpp, void *object, struct slab *slab);
#endif

void __check_heap_object(const void *ptr, unsigned long n,
			 const struct slab *slab, bool to_user);

void defer_free_barrier(void);

/* barrier 等待延迟 free 工作排空，供 teardown/test 在检查“已释放”前建立边界。 */

static inline bool slub_debug_orig_size(struct kmem_cache *s)
{
	/* STORE_USER 才保存原请求大小；仅 kmalloc cache 能据此做精确边界诊断。 */
	return (kmem_cache_debug_flags(s, SLAB_STORE_USER) &&
			(s->flags & SLAB_KMALLOC));
}

#ifdef CONFIG_SLUB_DEBUG
void skip_orig_size_check(struct kmem_cache *s, const void *object);
#endif

#endif /* MM_SLAB_H */
