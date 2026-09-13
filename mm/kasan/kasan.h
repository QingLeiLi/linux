/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MM_KASAN_KASAN_H
#define __MM_KASAN_KASAN_H

#include <linux/atomic.h>
#include <linux/kasan.h>
#include <linux/kasan-tags.h>
#include <linux/kfence.h>
#include <linux/stackdepot.h>

#if defined(CONFIG_KASAN_SW_TAGS) || defined(CONFIG_KASAN_HW_TAGS)

#include <linux/static_key.h>

DECLARE_STATIC_KEY_TRUE(kasan_flag_stacktrace);

/* 标签模式用 static key 在热路径开关栈采集；默认真但可在启动期关闭诊断成本。 */
/* 业务背景：分配/释放栈用于报告；入参：无；出参/返回：当前是否采集。
 * 注意事项：static key 是运行期开关，只读查询不保证对象或栈本身仍有效。
 */
static inline bool kasan_stack_collection_enabled(void)
{
	return static_branch_unlikely(&kasan_flag_stacktrace);
}

#else /* CONFIG_KASAN_SW_TAGS || CONFIG_KASAN_HW_TAGS */
/* Generic KASAN 没有该运行期开关，始终允许其 metadata 路径保存调用栈。 */

/* Generic 配置的恒真实现，保持所有调用点可无条件询问该能力。 */
static inline bool kasan_stack_collection_enabled(void)
{
	return true;
}

#endif /* CONFIG_KASAN_SW_TAGS || CONFIG_KASAN_HW_TAGS */

#ifdef CONFIG_KASAN_HW_TAGS

#include "../slab.h"

DECLARE_STATIC_KEY_TRUE(kasan_flag_vmalloc);

enum kasan_mode {
	/* 同步模式在违规指令处报告，适合精确归因。 */
	KASAN_MODE_SYNC,
	/* 异步模式允许硬件稍后递交异常，降低每次访问的同步代价。 */
	KASAN_MODE_ASYNC,
	/* 非对称模式混合两者：读可延迟、关键写仍应及时发现。 */
	KASAN_MODE_ASYMM,
};

/* 启动参数选定后只读；采样字段和 per-CPU skip 共同限制页分配 tag 开销。 */
extern enum kasan_mode kasan_mode __ro_after_init;

extern unsigned long kasan_page_alloc_sample;
extern unsigned int kasan_page_alloc_sample_order;
DECLARE_PER_CPU(long, kasan_page_alloc_skip);

/* 业务背景：vmalloc shadow 是可选成本；入参：无；返回本次构建是否会维护它。
 * 注意事项：仅描述 KASAN 能力，不代表某个 vmalloc 地址已经完成初始化。
 */
static inline bool kasan_vmalloc_enabled(void)
{
	/* Static branch is never enabled with CONFIG_KASAN_VMALLOC disabled. */
	/* 配置关闭时该 key 永不翻转，避免 vmalloc 路径误以为 metadata 已建立。 */
	return static_branch_likely(&kasan_flag_vmalloc);
}

/* 返回当前启动期 mode 是否允许硬件延迟递交 tag fault。 */
static inline bool kasan_async_fault_possible(void)
{
	return kasan_mode == KASAN_MODE_ASYNC || kasan_mode == KASAN_MODE_ASYMM;
}

/* 返回当前 mode 是否可能在访问现场同步报告，供调用路径选择恢复策略。 */
static inline bool kasan_sync_fault_possible(void)
{
	return kasan_mode == KASAN_MODE_SYNC || kasan_mode == KASAN_MODE_ASYMM;
}

/*
 * kasan_sample_page_alloc() - 决定本次高阶页是否写入硬件 KASAN tag
 * 业务背景：页级 tagging 很昂贵，采样让分配器在保留诊断概率的同时控制成本。
 * 入参：order 是页块阶数；出参/返回：true 表示本次必须检查/标记。注意事项：
 * per-CPU 计数无需跨 CPU 同步；小于阈值的高频小块始终采样以维持基本覆盖。
 */
static inline bool kasan_sample_page_alloc(unsigned int order)
{
	/* Fast-path for when sampling is disabled. */
	if (kasan_page_alloc_sample == 1)
		return true;

	if (order < kasan_page_alloc_sample_order)
		return true;

	/* 计数耗尽的 CPU 领取一个采样点并按周期重装；其他 CPU 各自独立。 */
	if (this_cpu_dec_return(kasan_page_alloc_skip) < 0) {
		this_cpu_write(kasan_page_alloc_skip,
			       kasan_page_alloc_sample - 1);
		return true;
	}

	return false;
}

#else /* CONFIG_KASAN_HW_TAGS */
/* 非硬件模式没有异步故障或页采样；inline 常量让调用方消除无关分支。 */

/* 非 HW_TAGS 由 Kconfig 常量决定，inline 使没有 vmalloc KASAN 的代码被消除。 */
static inline bool kasan_vmalloc_enabled(void)
{
	/* 配置查询只影响 vmalloc metadata 路径，不改变普通 slab shadow。 */
	return IS_ENABLED(CONFIG_KASAN_VMALLOC);
}

/* 软件 shadow 不存在硬件异步 fault 通道。 */
static inline bool kasan_async_fault_possible(void)
{
	return false;
}

/* 软件检查在插桩调用点执行，因此始终具备同步报告语义。 */
static inline bool kasan_sync_fault_possible(void)
{
	/* 非硬件模式始终在软件检查点同步判定，因此这里恒为真。 */
	return true;
}

/* 非硬件 tags 不需页级 tag 采样，所有 order 均视为覆盖。 */
static inline bool kasan_sample_page_alloc(unsigned int order)
{
	return true;
}

#endif /* CONFIG_KASAN_HW_TAGS */

#ifdef CONFIG_KASAN_GENERIC

/*
 * Generic KASAN uses per-object metadata to store alloc and free stack traces
 * and the quarantine link.
 */
/* Generic 模式将分配/释放栈和 quarantine 链接放进对象 redzone。 */
/* 返回 true 让 slab 为 Generic redzone metadata 预留布局空间。 */
static inline bool kasan_requires_meta(void)
{
	return true;
}

#else /* CONFIG_KASAN_GENERIC */
/* Tag 模式把历史保存在全局栈环，不为每个对象扩展 metadata。 */

/*
 * Tag-based KASAN modes do not use per-object metadata: they use the stack
 * ring to store alloc and free stack traces and do not use qurantine.
 */
/* Tag mode 以全局 ring 替代对象内 metadata，避免改变对象大小。 */
static inline bool kasan_requires_meta(void)
{
	return false;
}

#endif /* CONFIG_KASAN_GENERIC */

/* shadow granule 是一条 metadata 能描述的最小内存单位；硬件 MTE 使用架构粒度。 */
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
#define KASAN_GRANULE_SIZE	(1UL << KASAN_SHADOW_SCALE_SHIFT)
#else
#include <asm/mte-kasan.h>
#define KASAN_GRANULE_SIZE	MTE_GRANULE_SIZE
#endif

/* mask 用于检查地址/长度是否可安全映射到完整 granule。 */
#define KASAN_GRANULE_MASK	(KASAN_GRANULE_SIZE - 1)

#define KASAN_MEMORY_PER_SHADOW_PAGE	(KASAN_GRANULE_SIZE << PAGE_SHIFT)

/* Generic shadow 字节把不可访问原因编码为不同毒值，报告器据此分类。 */
#ifdef CONFIG_KASAN_GENERIC
#define KASAN_PAGE_FREE		0xFF  /* freed page */
#define KASAN_PAGE_REDZONE	0xFE  /* redzone for kmalloc_large allocation */
#define KASAN_SLAB_REDZONE	0xFC  /* redzone for slab object */
/* slab free 与 vmalloc invalid 分别表示对象生命周期终点和映射不可访问洞。 */
#define KASAN_SLAB_FREE		0xFB  /* freed slab object */
#define KASAN_VMALLOC_INVALID	0xF8  /* inaccessible space in vmap area */
#else
#define KASAN_PAGE_FREE		KASAN_TAG_INVALID
#define KASAN_PAGE_REDZONE	KASAN_TAG_INVALID
#define KASAN_SLAB_REDZONE	KASAN_TAG_INVALID
#define KASAN_SLAB_FREE		KASAN_TAG_INVALID
#define KASAN_VMALLOC_INVALID	KASAN_TAG_INVALID /* only used for SW_TAGS */
#endif

#ifdef CONFIG_KASAN_GENERIC

/* 每个值对应报告器的不同 UAF/OOB 类别；同一 shadow 字节只保存一种状态。 */
/* Generic 专属毒码补充页/对象状态，报告器用它们还原越界来源。 */
#define KASAN_SLAB_FREE_META	0xFA  /* freed slab object with free meta */
#define KASAN_GLOBAL_REDZONE	0xF9  /* redzone for global variable */

/* Stack redzone shadow values. Compiler ABI, do not change. */
/* 这些值是编译器插桩 ABI；修改会使已编译的检查代码解释错误。 */
#define KASAN_STACK_LEFT	0xF1
#define KASAN_STACK_MID		0xF2
#define KASAN_STACK_RIGHT	0xF3
#define KASAN_STACK_PARTIAL	0xF4

/* alloca redzone shadow values. */
/* 动态栈对象左右边界分别编码，报告可指出越界方向。 */
#define KASAN_ALLOCA_LEFT	0xCA
#define KASAN_ALLOCA_RIGHT	0xCB

/* alloca redzone size. Compiler ABI, do not change. */
#define KASAN_ALLOCA_REDZONE_SIZE	32

/* Stack frame marker. Compiler ABI, do not change. */
#define KASAN_CURRENT_STACK_FRAME_MAGIC 0x41B58AB3

/* Dummy value to avoid breaking randconfig/all*config builds. */
#ifndef KASAN_ABI_VERSION
#define KASAN_ABI_VERSION 1
#endif

#endif /* CONFIG_KASAN_GENERIC */

/* Metadata layout customization. */
/* 报告打印的 metadata 行按固定字节行映射回相邻真实内存。 */
#define META_BYTES_PER_BLOCK 1
#define META_BLOCKS_PER_ROW 16
#define META_BYTES_PER_ROW (META_BLOCKS_PER_ROW * META_BYTES_PER_BLOCK)
#define META_MEM_BYTES_PER_ROW (META_BYTES_PER_ROW * KASAN_GRANULE_SIZE)
#define META_ROWS_AROUND_ADDR 2

#define KASAN_STACK_DEPTH 64

/*
 * kasan_track 保存一次分配或释放的可追溯来源；stack 是 Stack Depot 的持久句柄，
 * pid/cpu/timestamp 只用于诊断，不能当作对象生命周期或并发同步机制。
 */
struct kasan_track {
	u32 pid;
	depot_stack_handle_t stack;
#ifdef CONFIG_KASAN_EXTRA_INFO
	u64 cpu:20;
	u64 timestamp:44;
#endif /* CONFIG_KASAN_EXTRA_INFO */
};

/* 报告入口先确定操作类别，公共 report 再补充对象和模式特有的证据。 */
enum kasan_report_type {
	KASAN_REPORT_ACCESS,
	KASAN_REPORT_INVALID_FREE,
	KASAN_REPORT_DOUBLE_FREE,
};

/*
 * report.c 在模式无关阶段填公共字段，generic/tags 回调再填写 bug_type 和两条 track；
 * 此对象只在一次报告调用内借用，不能在锁外长期保存其中的对象指针。
 */
struct kasan_report_info {
	/* type/access_* 是访问入口的输入快照，后续阶段不得篡改其归因。 */
	/* 字段分三段填充，避免 mode-specific 代码重复执行对象定位。 */
	/* Filled in by kasan_report_*(). */
	enum kasan_report_type type;
	const void *access_addr;
	size_t access_size;
/* is_write/ip 保留违规方向和调用点，供最终报告而不是 metadata 查找使用。 */
	bool is_write;
	unsigned long ip;

	/* Filled in by the common reporting code. */
	const void *first_bad_addr;
	struct kmem_cache *cache;
	void *object;
	size_t alloc_size;

	/* Filled in by the mode-specific reporting code. */
	const char *bug_type;
	struct kasan_track alloc_track;
	struct kasan_track free_track;
};

/* Do not change the struct layout: compiler ABI. */
/* 编译器生成的全局注册表直接按该布局写入，字段重排会破坏模块 ABI。 */
struct kasan_source_location {
	const char *filename;
	int line_no;
	int column_no;
};

/* Do not change the struct layout: compiler ABI. */
/* beg/size/redzone 描述真实全局对象及保护尾部，location/odr 由 ABI 版本追加。 */
struct kasan_global {
	/* 编译器注册全局时传入该描述符；KASAN 依它建立对象与 redzone 的 shadow。 */
	/* ABI 版本条件字段只能追加，旧编译器不会初始化较新字段。 */
	const void *beg;		/* Address of the beginning of the global variable. */
	size_t size;			/* Size of the global variable. */
	size_t size_with_redzone;	/* Size of the variable + size of the redzone. 32 bytes aligned. */
	const void *name;
	const void *module_name;	/* Name of the module where the global variable is declared. */
/* C++ 动态初始化与 ODR 指示器是编译器侧状态，KASAN 只据此丰富诊断。 */
	unsigned long has_dynamic_init;	/* This is needed for C++. */
#if KASAN_ABI_VERSION >= 4
	struct kasan_source_location *location;
#endif
#if KASAN_ABI_VERSION >= 5
	char *odr_indicator;
#endif
};

/* Structures for keeping alloc and free meta. */
/* 以下布局只在 Generic 模式存在，并与 slab object/redzone 的位置计算配对。 */

#ifdef CONFIG_KASAN_GENERIC

/*
 * Alloc meta contains the allocation-related information about a slab object.
 * Alloc meta is saved when an object is allocated and is kept until either the
 * object returns to the slab freelist (leaves quarantine for quarantined
 * objects or gets freed for the non-quarantined ones) or reallocated via
 * krealloc or through a mempool.
 * Alloc meta is stored inside of the object's redzone.
 * Alloc meta is considered valid whenever it contains non-zero data.
 */
/* alloc_meta 从对象分配起有效，到重分配或离开 quarantine 后由分配器复用。 */
struct kasan_alloc_meta {
	struct kasan_track alloc_track;
	/* Free track is stored in kasan_free_meta. */
	depot_stack_handle_t aux_stack[2];
};

struct qlist_node {
	/* quarantine 链表嵌入对象 metadata，节点不单独分配也不拥有对象。 */
	struct qlist_node *next;
};

/*
 * Free meta is stored either in the object itself or in the redzone after the
 * object. In the former case, free meta offset is 0. In the latter case, the
 * offset is between 0 and INT_MAX. INT_MAX marks that free meta is not present.
 */
/* INT_MAX 是“没有 free metadata”哨兵，不能被当作对象内合法偏移。 */
#define KASAN_NO_FREE_META INT_MAX

/*
 * Free meta contains the freeing-related information about a slab object.
 * Free meta is only kept for quarantined objects and for mempool objects until
 * the object gets allocated again.
 * Free meta is stored within the object's memory.
 * Free meta is considered valid whenever the value of the shadow byte that
 * corresponds to the first 8 bytes of the object is KASAN_SLAB_FREE_META.
 */
/* free_meta 仅在 quarantine/mempool 保存期有效；重新分配会覆盖其中的历史。 */
struct kasan_free_meta {
	struct qlist_node quarantine_link;
	struct kasan_track free_track;
};

#endif /* CONFIG_KASAN_GENERIC */

#if defined(CONFIG_KASAN_SW_TAGS) || defined(CONFIG_KASAN_HW_TAGS)

/* 标签模式的每一槽记录一个地址范围和最近分配/释放事件，报告端在读锁下检索。 */
struct kasan_stack_ring_entry {
	void *ptr;
	size_t size;
	struct kasan_track track;
	bool is_free;
};

/* rwlock 防止写者覆盖报告正在读取的条目；atomic pos 负责选择循环槽位。 */
struct kasan_stack_ring {
	rwlock_t lock;
	size_t size;
	atomic64_t pos;
	struct kasan_stack_ring_entry *entries;
};

#endif /* CONFIG_KASAN_SW_TAGS || CONFIG_KASAN_HW_TAGS */

#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)

/* 判断地址是否已经落在 shadow 本身，防止插桩递归访问 KASAN metadata。 */
/* 业务背景：防止检查 shadow 自身递归；入参：借用地址；返回是否在 shadow 映射。
 * 注意事项：仅地址范围判断，不验证页已映射或对象生命周期。
 */
static __always_inline bool addr_in_shadow(const void *addr)
{
	return addr >= (void *)KASAN_SHADOW_START &&
		addr < (void *)KASAN_SHADOW_END;
}

#ifndef kasan_shadow_to_mem
/* shadow 地址按比例和偏移反算到其描述的首个真实字节。 */
/* 将 shadow 起点反算为真实内存起点；调用者保证参数来自当前 KASAN shadow ABI。 */
static inline const void *kasan_shadow_to_mem(const void *shadow_addr)
{
	return (void *)(((unsigned long)shadow_addr - KASAN_SHADOW_OFFSET)
		<< KASAN_SHADOW_SCALE_SHIFT);
}
#endif

#ifndef addr_has_metadata
/* 有 metadata 的下界排除 early shadow 前的无效洞；reset_tag 保证比较真实地址。 */
/* 判断地址能否对应 KASAN metadata；tag 先清除，比较不把逻辑 tag 当真实地址。 */
static __always_inline bool addr_has_metadata(const void *addr)
{
	return (kasan_reset_tag(addr) >=
		kasan_shadow_to_mem((void *)KASAN_SHADOW_START));
}
#endif

/**
 * kasan_check_range - Check memory region, and report if invalid access.
 * @addr: the accessed address
 * @size: the accessed size
 * @write: true if access is a write access
 * @ret_ip: return address
 * @return: true if access was valid, false if invalid
 */
/* 插桩 load/store 最终进入这里；false 只代表已报告非法访问，不取得 addr 生命周期。 */
bool kasan_check_range(const void *addr, size_t size, bool write,
				unsigned long ret_ip);

#else /* CONFIG_KASAN_GENERIC || CONFIG_KASAN_SW_TAGS */
/* 硬件 tags 通过实际地址可访问性推断 metadata 覆盖，而非软件 shadow 范围。 */

/* 硬件模式以正常虚拟地址有效性作为 metadata 可达性的近似前置条件。 */
static __always_inline bool addr_has_metadata(const void *addr)
{
	return (is_vmalloc_addr(addr) || virt_addr_valid(addr));
}

#endif /* CONFIG_KASAN_GENERIC || CONFIG_KASAN_SW_TAGS */

/* report 公共层与 mode-specific 层的接口：调用者借用地址/cache，报告期间保证其有效。 */
const void *kasan_find_first_bad_addr(const void *addr, size_t size);
size_t kasan_get_alloc_size(void *object, struct kmem_cache *cache);
void kasan_complete_mode_report_info(struct kasan_report_info *info);
void kasan_metadata_fetch_row(char *buffer, void *row);

#if defined(CONFIG_KASAN_SW_TAGS) || defined(CONFIG_KASAN_HW_TAGS)
/* 仅 tag 模式有地址 tag 与内存 tag 的可视化输出。 */
void kasan_print_tags(u8 addr_tag, const void *addr);
#else
static inline void kasan_print_tags(u8 addr_tag, const void *addr) { }
#endif

#if defined(CONFIG_KASAN_STACK)
/* 栈对象报告需由架构/Generic 栈 layout 支持；关闭时保持无副作用桩。 */
void kasan_print_address_stack_frame(const void *addr);
#else
static inline void kasan_print_address_stack_frame(const void *addr) { }
#endif

#ifdef CONFIG_KASAN_GENERIC
/* Generic 对象 metadata 可保存辅助分配栈；标签模式没有对应 per-object 数据。 */
void kasan_print_aux_stacks(struct kmem_cache *cache, const void *object);
#else
static inline void kasan_print_aux_stacks(struct kmem_cache *cache, const void *object) { }
#endif

/* 统一报告入口：调用者传入访问现场，返回 true 表示诊断已被处理。 */
bool kasan_report(const void *addr, size_t size,
		bool is_write, unsigned long ip);
void kasan_report_invalid_free(void *object, unsigned long ip, enum kasan_report_type type);

struct slab *kasan_addr_to_slab(const void *addr);

/* 这组三个 helper 只解析 Generic redzone 内布局，cache/object 均为调用者借用。 */
#ifdef CONFIG_KASAN_GENERIC
struct kasan_alloc_meta *kasan_get_alloc_meta(struct kmem_cache *cache,
						const void *object);
struct kasan_free_meta *kasan_get_free_meta(struct kmem_cache *cache,
						const void *object);
void kasan_init_object_meta(struct kmem_cache *cache, const void *object);
#else
static inline void kasan_init_object_meta(struct kmem_cache *cache, const void *object) { }
#endif

/* 保存 track 的实现见 common.c/tags.c；GFP 决定是否允许 Stack Depot 分配。 */
depot_stack_handle_t kasan_save_stack(gfp_t flags, depot_flags_t depot_flags);
void kasan_set_track(struct kasan_track *track, depot_stack_handle_t stack);
void kasan_save_track(struct kasan_track *track, gfp_t flags);
void kasan_save_alloc_info(struct kmem_cache *cache, void *object, gfp_t flags);

void kasan_save_free_info(struct kmem_cache *cache, void *object);

/* quarantine 延迟对象重新进入 slab freelist，以扩大 UAF 被发现的时间窗口。 */
#ifdef CONFIG_KASAN_GENERIC
bool kasan_quarantine_put(struct kmem_cache *cache, void *object);
void kasan_quarantine_reduce(void);
void kasan_quarantine_remove_cache(struct kmem_cache *cache);
#else
static inline bool kasan_quarantine_put(struct kmem_cache *cache, void *object) { return false; }
static inline void kasan_quarantine_reduce(void) { }
static inline void kasan_quarantine_remove_cache(struct kmem_cache *cache) { }
#endif

/* 架构可覆盖 tag 编解码；通用默认不改变地址，适用于无 tag 的构建。 */
#ifndef arch_kasan_set_tag
/* 默认架构没有地址 tag 编码，返回原借用地址；tag 参数在此配置无可观察副作用。 */
static inline const void *arch_kasan_set_tag(const void *addr, u8 tag)
{
	return addr;
}
#endif
#ifndef arch_kasan_get_tag
#define arch_kasan_get_tag(addr)	0
#endif

/* 宏保持调用点类型灵活，但仅转换地址表示，不分配、不验证也不延长生命周期。 */
#define set_tag(addr, tag)	((void *)arch_kasan_set_tag((addr), (tag)))
#define get_tag(addr)		arch_kasan_get_tag(addr)

#ifdef CONFIG_KASAN_HW_TAGS

/* 以下宏是 HW_TAGS 对架构 MTE/tag 指令的薄包装，公共层不直接依赖 arch 名称。 */

/* 同步、异步、抑制和读取 tag 都经由这些宏保持架构独立的调用面。 */

#define hw_enable_tag_checks_sync()		arch_enable_tag_checks_sync()
#define hw_enable_tag_checks_async()		arch_enable_tag_checks_async()
#define hw_enable_tag_checks_asymm()		arch_enable_tag_checks_asymm()
#define hw_suppress_tag_checks_start()		arch_suppress_tag_checks_start()
#define hw_suppress_tag_checks_stop()		arch_suppress_tag_checks_stop()
#define hw_force_async_tag_fault()		arch_force_async_tag_fault()
/* write-only 与随机/读取 tag 分别服务测试、分配和访问检查路径。 */
#define hw_enable_tag_checks_write_only()	arch_enable_tag_checks_write_only()
#define hw_get_random_tag()			arch_get_random_tag()
#define hw_get_mem_tag(addr)			arch_get_mem_tag(addr)
#define hw_set_mem_tag_range(addr, size, tag, init) \
			arch_set_mem_tag_range((addr), (size), (tag), (init))

void kasan_enable_hw_tags(void);

#else /* CONFIG_KASAN_HW_TAGS */

/* 关闭硬件 tags 时 enable 是无副作用桩，其他路径无需增加条件判断。 */
/* 配置桩：无硬件 tag 时不改变 CPU 状态，也不返回错误。 */
static inline void kasan_enable_hw_tags(void) { }

#endif /* CONFIG_KASAN_HW_TAGS */

#if defined(CONFIG_KASAN_SW_TAGS) || defined(CONFIG_KASAN_HW_TAGS)
/* 标签模式启动期建立栈环和 tag 状态；Generic 不需要此独立入口。 */
void __init kasan_init_tags(void);
#endif /* CONFIG_KASAN_SW_TAGS || CONFIG_KASAN_HW_TAGS */

#if defined(CONFIG_KASAN_HW_TAGS) && IS_ENABLED(CONFIG_KASAN_KUNIT_TEST)

/* 这两个测试接口只改变诊断模式，不能作为正常内存管理同步手段。 */
void kasan_force_async_fault(void);
bool kasan_write_only_enabled(void);

#else /* CONFIG_KASAN_HW_TAGS && CONFIG_KASAN_KUNIT_TEST */

/* 测试关闭时空桩防止调用点分裂；不能据此推断硬件真的支持 async。 */
static inline void kasan_force_async_fault(void) { }

/* 测试关闭时恒 false，表示不存在 write-only 检查模式。 */
static inline bool kasan_write_only_enabled(void)
{
	return false;
}

#endif /* CONFIG_KASAN_HW_TAGS && CONFIG_KASAN_KUNIT_TEST */

#ifdef CONFIG_KASAN_SW_TAGS
/* 随机 tag 防止对象复用后旧指针持续匹配；无 tags 构建固定返回零。 */
u8 kasan_random_tag(void);
#elif defined(CONFIG_KASAN_HW_TAGS)
/* 硬件随机 tag 由 arch 提供，返回值随后写入新对象/页的内存 tag。 */
static inline u8 kasan_random_tag(void) { return hw_get_random_tag(); }
#else
/* 无 tag 构建以零保持调用 ABI；它不提供复用对象区分能力。 */
static inline u8 kasan_random_tag(void) { return 0; }
#endif

#ifdef CONFIG_KASAN_HW_TAGS

/* 硬件模式把 poison/unpoison 映射为按 granule 写内存 tag 的架构操作。 */

static inline void kasan_poison(const void *addr, size_t size, u8 value, bool init)
{
	/* 不对齐就拒绝部分写入，以免同一对象处在混合 tag 状态。 */
	if (WARN_ON((unsigned long)addr & KASAN_GRANULE_MASK))
		return;
	if (WARN_ON(size & KASAN_GRANULE_MASK))
		return;

/* reset_tag 去掉指针逻辑 tag，底层写入以物理地址和新的内存 tag 为准。 */
	hw_set_mem_tag_range(kasan_reset_tag(addr), size, value, init);
}

/* 开放时保留地址携带的逻辑 tag，并将长度向上扩展到完整 granule。 */
static inline void kasan_unpoison(const void *addr, size_t size, bool init)
{
	u8 tag = get_tag(addr);

	if (WARN_ON((unsigned long)addr & KASAN_GRANULE_MASK))
		return;
	size = round_up(size, KASAN_GRANULE_SIZE);

	hw_set_mem_tag_range(kasan_reset_tag(addr), size, tag, init);
}

/* 内核 tag 是特权例外；普通 tagged 指针必须与内存 tag 一致。 */
static inline bool kasan_byte_accessible(const void *addr)
{
	u8 ptr_tag = get_tag(addr);
	u8 mem_tag = hw_get_mem_tag((void *)addr);

	return ptr_tag == KASAN_TAG_KERNEL || ptr_tag == mem_tag;
}

#else /* CONFIG_KASAN_HW_TAGS */

/* 软件 shadow 实现在 common/generic；这里保留所有模式共用的分配器接口。 */

/**
 * kasan_poison - mark the memory range as inaccessible
 * @addr: range start address, must be aligned to KASAN_GRANULE_SIZE
 * @size: range size, must be aligned to KASAN_GRANULE_SIZE
 * @value: value that's written to metadata for the range
 * @init: whether to initialize the memory range (only for hardware tag-based)
 */
void kasan_poison(const void *addr, size_t size, u8 value, bool init);

/**
 * kasan_unpoison - mark the memory range as accessible
 * @addr: range start address, must be aligned to KASAN_GRANULE_SIZE
 * @size: range size, can be unaligned
 * @init: whether to initialize the memory range (only for hardware tag-based)
 *
 * For the tag-based modes, the @size gets aligned to KASAN_GRANULE_SIZE before
 * marking the range.
 * For the generic mode, the last granule of the memory range gets partially
 * unpoisoned based on the @size.
 */
void kasan_unpoison(const void *addr, size_t size, bool init);

bool kasan_byte_accessible(const void *addr);

#endif /* CONFIG_KASAN_HW_TAGS */

#ifdef CONFIG_KASAN_GENERIC

/* Generic 可编码对象最后一个部分可访问 granule，tag 模式只能处理完整粒度。 */

/**
 * kasan_poison_last_granule - mark the last granule of the memory range as
 * inaccessible
 * @address: range start address, must be aligned to KASAN_GRANULE_SIZE
 * @size: range size
 *
 * This function is only available for the generic mode, as it's the only mode
 * that has partially poisoned memory granules.
 */
void kasan_poison_last_granule(const void *address, size_t size);

#else /* CONFIG_KASAN_GENERIC */

static inline void kasan_poison_last_granule(const void *address, size_t size) { }

#endif /* CONFIG_KASAN_GENERIC */

#ifndef kasan_arch_is_ready
static inline bool kasan_arch_is_ready(void)	{ return true; }
#elif !defined(CONFIG_KASAN_GENERIC) || !defined(CONFIG_KASAN_OUTLINE)
#error kasan_arch_is_ready only works in KASAN generic outline mode!
#endif

#if IS_ENABLED(CONFIG_KASAN_KUNIT_TEST)

/* 测试 suite 边界保存和恢复全局诊断状态，生产构建由下方桩消除。 */

/* suite 入口与退出成对；测试框架负责即使失败也恢复共享状态。 */

void kasan_kunit_test_suite_start(void);
void kasan_kunit_test_suite_end(void);

/* Rust 测试符号可选，C-only 构建返回零避免链接器要求 Rust runtime。 */
#ifdef CONFIG_RUST
char kasan_test_rust_uaf(void);
#else
static inline char kasan_test_rust_uaf(void) { return '\0'; }
#endif

#else /* CONFIG_KASAN_KUNIT_TEST */

static inline void kasan_kunit_test_suite_start(void) { }
static inline void kasan_kunit_test_suite_end(void) { }

#endif /* CONFIG_KASAN_KUNIT_TEST */

#if IS_ENABLED(CONFIG_KASAN_KUNIT_TEST)

/* multi-shot 保存旧设置，restore 必须收到这份返回状态才能撤销临时改变。 */
bool kasan_save_enable_multi_shot(void);
void kasan_restore_multi_shot(bool enabled);

#endif

/*
 * Exported functions for interfaces called from assembly or from generated
 * code. Declared here to avoid warnings about missing declarations.
 */
/* 以下符号属于编译器插桩 ABI，生成代码直接调用，不能改变名称、签名或访问宽度。 */

void __asan_register_globals(void *globals, ssize_t size);
void __asan_unregister_globals(void *globals, ssize_t size);
void __asan_handle_no_return(void);
void __asan_alloca_poison(void *, ssize_t size);
void __asan_allocas_unpoison(void *stack_top, ssize_t stack_bottom);

/* global 注册/注销与 alloca poison/unpoison 分别建立静态和动态栈对象的边界。 */

/* 固定宽度 load/store 入口按插桩访问宽度分派，N 版本额外接收运行期长度。 */
void __asan_load1(void *);
void __asan_store1(void *);
void __asan_load2(void *);
void __asan_store2(void *);
void __asan_load4(void *);
void __asan_store4(void *);
/* 16 字节访问通常对应向量/结构操作，N 处理不能静态确定的长度。 */
void __asan_load8(void *);
void __asan_store8(void *);
void __asan_load16(void *);
void __asan_store16(void *);
void __asan_loadN(void *, ssize_t size);
void __asan_storeN(void *, ssize_t size);

/* load 与 store 分开保留，报告器借此区分读越界和写越界。 */

/* noabort 入口在报告后继续执行，适配 multi-shot 诊断策略。 */
void __asan_load1_noabort(void *);
void __asan_store1_noabort(void *);
void __asan_load2_noabort(void *);
void __asan_store2_noabort(void *);
void __asan_load4_noabort(void *);
void __asan_store4_noabort(void *);
/* 固定宽度 noabort 仍需保持与原访问相同的读写方向。 */
void __asan_load8_noabort(void *);
void __asan_store8_noabort(void *);
void __asan_load16_noabort(void *);
void __asan_store16_noabort(void *);
void __asan_loadN_noabort(void *, ssize_t size);
void __asan_storeN_noabort(void *, ssize_t size);

/* noabort 系列不拥有地址；调用者恢复后仍必须保证访问对象未被释放。 */

/* report-only 入口由已完成访问检查的插桩调用，仍需保留宽度构造报告。 */
void __asan_report_load1_noabort(void *);
void __asan_report_store1_noabort(void *);
void __asan_report_load2_noabort(void *);
void __asan_report_store2_noabort(void *);
void __asan_report_load4_noabort(void *);
void __asan_report_store4_noabort(void *);
/* report N 与固定宽度版本共享报告器，只是由参数携带访问长度。 */
void __asan_report_load8_noabort(void *);
void __asan_report_store8_noabort(void *);
void __asan_report_load16_noabort(void *);
void __asan_report_store16_noabort(void *);
void __asan_report_load_n_noabort(void *, ssize_t size);
void __asan_report_store_n_noabort(void *, ssize_t size);

/* report 函数不执行原访问，只消费插桩提供的地址与访问类别。 */

/* shadow helper 写入编译器 ABI 规定的毒码，主要服务栈 redzone。 */
void __asan_set_shadow_00(const void *addr, ssize_t size);
void __asan_set_shadow_f1(const void *addr, ssize_t size);
void __asan_set_shadow_f2(const void *addr, ssize_t size);
void __asan_set_shadow_f3(const void *addr, ssize_t size);
void __asan_set_shadow_f5(const void *addr, ssize_t size);
void __asan_set_shadow_f8(const void *addr, ssize_t size);

/* 每个 f* 值代表预定义 shadow 状态，具体语义由前面的 ABI 常量定义。 */

/* 内存包装必须保留 KASAN 检查与 shadow/tag 语义，不能直接替换为 libc。 */
void *__asan_memset(void *addr, int c, ssize_t len);
void *__asan_memmove(void *dest, const void *src, ssize_t len);
void *__asan_memcpy(void *dest, const void *src, ssize_t len);

/* HWASAN 对应的 noabort ABI，和 ASAN 版本一样由生成代码按宽度选择。 */
void __hwasan_load1_noabort(void *);
void __hwasan_store1_noabort(void *);
void __hwasan_load2_noabort(void *);
void __hwasan_store2_noabort(void *);
void __hwasan_load4_noabort(void *);
void __hwasan_store4_noabort(void *);
/* HWASAN 宽度分派也必须完整，遗漏声明会让插桩对象链接失败。 */
void __hwasan_load8_noabort(void *);
void __hwasan_store8_noabort(void *);
void __hwasan_load16_noabort(void *);
void __hwasan_store16_noabort(void *);
void __hwasan_loadN_noabort(void *, ssize_t size);
void __hwasan_storeN_noabort(void *, ssize_t size);

/* 为一段内存赋逻辑 tag；调用者仍负责该内存生命周期和并发排他。 */
void __hwasan_tag_memory(void *, u8 tag, ssize_t size);

/* tag-aware 内存包装的 ownership 与普通 memcpy/memmove 相同。 */
void *__hwasan_memset(void *addr, int c, ssize_t len);
void *__hwasan_memmove(void *dest, const void *src, ssize_t len);
void *__hwasan_memcpy(void *dest, const void *src, ssize_t len);

/* tag mismatch 是 HWASAN 的统一失败出口；这里不负责释放或修复访问。 */

/* mismatch 解码访问描述并以 ret_ip 归因，随后进入公共报告流程。 */
void kasan_tag_mismatch(void *addr, unsigned long access_info,
			unsigned long ret_ip);

#endif /* __MM_KASAN_KASAN_H */
