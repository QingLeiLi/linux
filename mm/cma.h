/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MM_CMA_H__
#define __MM_CMA_H__

#include <linux/debugfs.h>
#include <linux/kobject.h>

/*
 * cma_kobject 把 sysfs kobject 嵌入一个可动态释放的包装，并反向借用对应的静态
 * cma area。cma_sysfs_init() 分配/发布它，kobject 最后 put 时 release 回调释放包装
 * 并清空 cma->cma_kobj；kobj 引用只延长包装寿命，不延长全局 cma_areas。
 */
struct cma_kobject {
	/* sysfs core 持有引用的嵌入对象；必须通过 container_of 找回包装。 */
	struct kobject kobj;
	/* 指向内核全寿命 CMA 描述符的借用指针，不单独 get/put。 */
	struct cma *cma;
};

/*
 * Multi-range support. This can be useful if the size of the allocation
 * is not expected to be larger than the alignment (like with hugetlb_cma),
 * and the total amount of memory requested, while smaller than the total
 * amount of memory available, is large enough that it doesn't fit in a
 * single physical memory range because of memory holes.
 *
 * Fields:
 *   @base_pfn: physical address of range
 *   @early_pfn: first PFN not reserved through cma_reserve_early
 *   @count: size of range
 *   @bitmap: bitmap of allocated (1 << order_per_bit)-sized chunks.
 */
/*
 * 多区间让一个 CMA area 跨越物理内存洞：当单次请求不大于对齐粒度、但总预留量
 * 无法落进一个连续物理范围时，可在至多 CMA_MAX_RANGES 个范围间逐个尝试。
 * base_pfn/count 以页为单位描述半开区间；union 在启动激活前保存 early_pfn（已经
 * 被早期用户预留后的首个 PFN），激活后改为 bitmap，二者绝不能同时解释。
 * bitmap 的一个 bit 代表 2^order_per_bit 页，1 表示 CMA 已保留给分配者。
 */
struct cma_memrange {
	unsigned long base_pfn;
	unsigned long count;
	union {
		unsigned long early_pfn;
		unsigned long *bitmap;
	};
#ifdef CONFIG_CMA_DEBUGFS
	/* debugfs 只读展示 bitmap 的数组视图，生命周期从 area 激活持续到系统结束。 */
	struct debugfs_u32_array dfs_bitmap;
#endif
};
#define CMA_MAX_RANGES 8

/*
 * struct cma - 一个可从伙伴系统迁移出连续页的逻辑预留区。
 *
 * count/available_count 都以页为单位，后者与所有 range bitmap 的空闲位保持一致；
 * lock 保护 bitmap 与 available_count 的快速认领/归还，alloc_mutex 则串行化耗时的
 * alloc_contig_frozen_range() 迁移阶段。name/nid/ranges 在激活后只读，flags 记录
 * 区域验证和生命周期状态。debugfs mem_head 单独由 mem_head_lock 保护，sysfs 统计
 * 使用 atomic64，不要求取得 CMA 主锁。
 */
struct cma {
	unsigned long   count;
	unsigned long	available_count;
	unsigned int order_per_bit; /* Order of pages represented by one bit */
	/* 上述英文表示：一个 bitmap bit 覆盖 2^order_per_bit 个连续页。 */
	spinlock_t	lock;
	struct mutex alloc_mutex;
#ifdef CONFIG_CMA_DEBUGFS
	/* 记录 debugfs 主动分配项，使用独立锁避免延长 bitmap 临界区。 */
	struct hlist_head mem_head;
	spinlock_t mem_head_lock;
#endif
	char name[CMA_MAX_NAME];
	/* nranges 决定 ranges[] 的有效前缀；未使用尾项不可读取为有效物理范围。 */
	int nranges;
	struct cma_memrange ranges[CMA_MAX_RANGES];
#ifdef CONFIG_CMA_SYSFS
	/* the number of CMA page successful allocations */
	/* 成功分配的累计页数；是事件计数，不是当前占用量。 */
	atomic64_t nr_pages_succeeded;
	/* the number of CMA page allocation failures */
	/* 失败请求涉及的累计页数，失败不会减少 available_count。 */
	atomic64_t nr_pages_failed;
	/* the number of CMA page released */
	/* 成功归还的累计页数；与 succeeded 都可能长期单调增长。 */
	atomic64_t nr_pages_released;
	/* kobject requires dynamic object */
	/* kobject 要求承载对象可由 release 动态释放，因此这里只保存包装指针。 */
	struct cma_kobject *cma_kobj;
#endif
	unsigned long flags;
	/* NUMA node (NUMA_NO_NODE if unspecified) */
	/* 目标 NUMA 节点；NUMA_NO_NODE 表示不限定，实际范围仍由 base_pfn 决定。 */
	int nid;
};

/*
 * CMA_RESERVE_PAGES_ON_ERROR：激活失败也保留物理页，不归还 buddy；由早期依赖者设置。
 * CMA_ZONES_VALID/INVALID：缓存 cma_validate_zones() 结果，二者互斥且避免重复扫描。
 * CMA_ACTIVATED：bitmap、锁和 pageblock 已初始化，运行期分配/sysfs 才可使用该 area。
 */
enum cma_flags {
	CMA_RESERVE_PAGES_ON_ERROR,
	/* cma_reserve_pages_on_error() 为不可归还预留设置，cma_activate_area() 失败分支读取。 */
	CMA_ZONES_VALID,
	/* cma_validate_zones() 首次成功后设置，后续验证调用据此跳过重复 zone 遍历。 */
	CMA_ZONES_INVALID,
	/* zone 跨越等验证失败时设置，后续调用快速拒绝同一不合法 area。 */
	CMA_ACTIVATED,
	/* cma_activate_area() 完成 bitmap/pageblock 初始化后发布，cma_reserve_early() 据此拒绝迟到预留。 */
};

/* cma_areas[0..cma_area_count) 是启动期登记、运行期只读定位的全局 area 表。 */
extern struct cma cma_areas[MAX_CMA_AREAS];
extern unsigned int cma_area_count;

/*
 * cma_bitmap_maxno() - 计算一个 range 的有效 bitmap 位数。
 * 业务背景：分配器与 debugfs 用同一换算避免页数/bit 粒度不一致。
 * 入参：cma/cmr 均为借用、非 NULL 且已登记对象；只读取 count/order_per_bit。
 * 出参/返回：返回 cmr->count >> order_per_bit 个 bit，无副作用；范围大小须已按粒度对齐。
 */
static inline unsigned long cma_bitmap_maxno(struct cma *cma,
		struct cma_memrange *cmr)
{
	return cmr->count >> cma->order_per_bit;
}

#ifdef CONFIG_CMA_SYSFS
/*
 * cma_sysfs_account_success_pages() - 在 cma_alloc() 成功发布页后累计成功页数。
 * 业务背景：sysfs 只展示历史事件，核心分配路径用此 helper 隔离可选统计实现。
 * 入参：cma 为已激活 area 的借用指针；nr_pages 是本次成功页数，单位 page，须与请求一致。
 * 返回/副作用：void；原子累加 succeeded，不转移页或 area ownership，也不改变 bitmap。
 * 注意事项：可并发且不睡眠；必须只在最终成功分支调用，否则统计会重复或虚增。
 */
void cma_sysfs_account_success_pages(struct cma *cma, unsigned long nr_pages);
/*
 * cma_sysfs_account_fail_pages() - 在 cma_alloc() 穷尽候选后累计失败请求页数。
 * 业务背景：与成功统计共同向 sysfs 展示 CMA 压力，但不参与分配决策。
 * 入参：cma 是借用 area；nr_pages 是失败请求规模，单位 page，允许大于当前可用量。
 * 返回/副作用：void；只原子累加 failed，不改变 bitmap/available_count 或错误返回。
 * 注意事项：可并发且不睡眠；一次外部请求只记一次，内部候选重试不得分别计数。
 */
void cma_sysfs_account_fail_pages(struct cma *cma, unsigned long nr_pages);
/*
 * cma_sysfs_account_release_pages() - 在 cma_release_frozen() 完成真实归还后累计页数。
 * 业务背景：调用者已先归还 contiguous pages 并清 bitmap，本函数只记录历史事件。
 * 入参：cma 是借用 area；nr_pages 为本次成功释放页数，单位 page。
 * 返回/副作用：void；原子累加 released，不接管 pages/cma ownership。
 * 注意事项：可并发且不睡眠；若释放校验失败或尚未清 bitmap，不得提前调用。
 */
void cma_sysfs_account_release_pages(struct cma *cma, unsigned long nr_pages);
#else
/*
 * cma_sysfs_account_success_pages() - CONFIG_CMA_SYSFS=n 的成功计账空桩。
 * 业务背景：让 cma_alloc() 保持无条件调用；cma/nr_pages 分别是借用 area 和页数。
 * 返回/副作用：void，无统计或 ownership 变化；可在任意核心调用上下文直接消除。
 * 注意事项：参数表达式会按 C 调用规则求值，调用者仍不可依赖此桩做真实分配发布。
 */
static inline void cma_sysfs_account_success_pages(struct cma *cma,
						   unsigned long nr_pages) {};
/*
 * cma_sysfs_account_fail_pages() - CONFIG_CMA_SYSFS=n 的失败计账空桩。
 * 业务背景/入参：cma_alloc() 传入借用 area 与失败请求页数，保持启用分支相同调用链。
 * 返回/副作用：void，不修改真实分配错误、bitmap 或统计；不睡眠且不转移 ownership。
 * 注意事项：它只消除可选观测，不能替代调用者自己的失败清理。
 */
static inline void cma_sysfs_account_fail_pages(struct cma *cma,
						unsigned long nr_pages) {};
/*
 * cma_sysfs_account_release_pages() - CONFIG_CMA_SYSFS=n 的释放计账空桩。
 * 业务背景/入参：释放路径传入借用 area 和已归还页数，单位 page，仅统一配置调用点。
 * 返回/副作用：void；不清 bitmap、不归还页、不计账、不睡眠，ownership 保持不变。
 * 注意事项：真实释放仍必须在调用本桩前由 CMA 核心完成。
 */
static inline void cma_sysfs_account_release_pages(struct cma *cma,
						   unsigned long nr_pages) {};
#endif
/* 结束 CMA 私有对象与计账接口的防重复包含范围。 */
#endif
