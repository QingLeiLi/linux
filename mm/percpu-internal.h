/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_PERCPU_INTERNAL_H
#define _MM_PERCPU_INTERNAL_H

#include <linux/types.h>
#include <linux/percpu.h>
#include <linux/memcontrol.h>

/*
 * 本头文件是动态 percpu 分配器各实现文件共享的内部契约：percpu.c 维护
 * bitmap、chunk 槽位与分配状态，percpu-vm.c/percpu-km.c 提供虚拟映射或
 * 连续内存后端，percpu-stats.c 只读取这里的统计快照。它不属于模块可用 ABI。
 */

/*
 * pcpu_block_md is the metadata block struct.
 * Each chunk's bitmap is split into a number of full blocks.
 * All units are in terms of bits.
 *
 * The scan hint is the largest known contiguous area before the contig hint.
 * It is not necessarily the actual largest contig hint though.  There is an
 * invariant that the scan_hint_start > contig_hint_start iff
 * scan_hint == contig_hint.  This is necessary because when scanning forward,
 * we don't know if a new contig hint would be better than the current one.
 */
/*
 * 每个 pcpu_block_md 汇总一段分配 bitmap；chunk_md 汇总整个 chunk，
 * md_blocks[] 则把同一 bitmap 分块，以便 first-fit 扫描先排除不可能容纳
 * 请求的块。所有位置和长度均以 PCPU_MIN_ALLOC_SIZE 对应的 bitmap 位为单位。
 *
 * scan_hint/scan_hint_start 保存 contig_hint 之前的候选连续空闲段；
 * contig_hint/contig_hint_start 保存当前已知最佳段。left_free/right_free 让
 * 相邻块可拼接判断跨块空闲段，first_free 是最早空闲位，nr_bits 是本元数据
 * 实际负责的位数。字段都在 pcpu_lock 下随 alloc_map 更新，不能脱离 bitmap
 * 单独视为权威状态；原文中的 hint 不变量保证前向扫描不会漏掉更优候选。
 */
struct pcpu_block_md {
	int			scan_hint;	/* scan hint for block */
	int			scan_hint_start; /* block relative starting
						    position of the scan hint */
	int                     contig_hint;    /* contig hint for block */
	int                     contig_hint_start; /* block relative starting
						      position of the contig hint */
	int                     left_free;      /* size of free space along
						   the left side of the block */
	int                     right_free;     /* size of free space along
						   the right side of the block */
	int                     first_free;     /* block position of first free */
	int			nr_bits;	/* total bits responsible for */
};

/*
 * 每个最小 percpu 分配单元可选的旁路元数据。obj_exts[] 与 alloc_map 位使用
 * 相同索引：MEMCG 配置记录被持有的 obj_cgroup 引用，分配分析配置记录调用点
 * codetag；创建 chunk 时统一分配，释放对象时分别解除计费/记录。
 */
struct pcpuobj_ext {
#ifdef CONFIG_MEMCG
	struct obj_cgroup	*cgroup;
#endif
#ifdef CONFIG_MEM_ALLOC_PROFILING
	union codetag_ref	tag;
#endif
};

#if defined(CONFIG_MEMCG) || defined(CONFIG_MEM_ALLOC_PROFILING)
/* 任一消费者可能存在时保留 obj_exts 字段；运行期是否分配由下方 helper 决定。 */
#define NEED_PCPUOBJ_EXT
#endif

/*
 * pcpu_chunk 代表所有可能 CPU 上同一逻辑偏移范围。alloc_map/bound_map 和
 * 元数据描述逻辑对象，populated 描述哪些 backing page 已建立；后端拥有 data
 * 或映射资源，通用层把 chunk 挂入按可用空间分级的 pcpu_chunk_lists。
 * pcpu_lock 保护槽位、bitmap、容量计数和统计；chunk 从 active 槽隔离后仍可
 * 等待异步 depopulate，最终由后端销毁。
 */
struct pcpu_chunk {
#ifdef CONFIG_PERCPU_STATS
	/* 调试统计只在 pcpu_lock 下更新，不参与分配正确性。 */
	int			nr_alloc;	/* # of allocations */
	size_t			max_alloc_size; /* largest allocation size */
#endif

	/* list 决定当前容量槽；free_bytes 与 chunk_md 必须和 alloc_map 同步。 */
	struct list_head	list;		/* linked to pcpu_slot lists */
	int			free_bytes;	/* free bytes in the chunk */
	struct pcpu_block_md	chunk_md;
	unsigned long		*bound_map;	/* boundary map */

	/*
	 * base_addr is the base address of this chunk.
	 * To reduce false sharing, current layout is optimized to make sure
	 * base_addr locate in the different cacheline with free_bytes and
	 * chunk_md.
	 */
	/*
	 * base_addr 是 CPU 0 逻辑单元的基址，配合 per_cpu_offset() 形成各 CPU
	 * 地址。原文强调把它与频繁写入的 free_bytes/chunk_md 分开 cacheline，
	 * 避免地址查询和分配更新互相制造伪共享。
	 */
	void			*base_addr ____cacheline_aligned_in_smp;

	/* alloc_map 标出占用位，bound_map 标出对象边界，md_blocks 是分块摘要。 */
	unsigned long		*alloc_map;	/* allocation map */
	struct pcpu_block_md	*md_blocks;	/* metadata blocks */

	void			*data;		/* chunk data */
	/* immutable 禁止改变 backing；isolated 表示不再参与普通分配。 */
	bool			immutable;	/* no [de]population allowed */
	bool			isolated;	/* isolated from active chunk
						   slots */
	int			start_offset;	/* the overlap with the previous
						   region to have a page aligned
						   base_addr */
	int			end_offset;	/* additional area required to
						   have the region end page
						   aligned */
	/* nr_pages 是容量；后两个计数分别跟踪已映射页及其中完全空闲的页。 */
	int			nr_pages;	/* # of pages served by this chunk */
	int			nr_populated;	/* # of populated pages */
	int                     nr_empty_pop_pages; /* # of empty populated pages */
#ifdef NEED_PCPUOBJ_EXT
	/* 每个最小分配位一个扩展槽；字段存在不等于运行期一定分配。 */
	struct pcpuobj_ext	*obj_exts;	/* vector of object cgroups */
#endif

	/* 柔性数组按 nr_pages 取位，记录 backing page 的 population 状态。 */
	unsigned long		populated[];	/* populated bitmap */
};

/*
 * 业务背景：pcpu_alloc_chunk() 用它决定是否需要为每个最小对象位分配
 * pcpuobj_ext，统一承载 memcg 计费引用或 allocation profiling 标签。
 * 入参：无；只读取编译期开关和 memcg 运行期禁用状态。
 * 出参/返回：任一扩展消费者实际启用时返回 true，否则返回 false；无状态修改。
 * 注意事项：CONFIG_MEM_ALLOC_PROFILING 一旦编入便始终需要扩展；MEMCG 虽编入
 * 仍可能由启动参数禁用。调用者只据此分配数组，不转移任何 ownership。
 */
static inline bool need_pcpuobj_ext(void)
{
	if (IS_ENABLED(CONFIG_MEM_ALLOC_PROFILING))
		return true;
	if (!mem_cgroup_kmem_disabled())
		return true;
	return false;
}

/*
 * 以下状态由 percpu.c 定义。pcpu_lock 稳定 chunk 槽链、bitmap 与全局计数；
 * chunk_lists 的槽号按可用连续空间分级，两个特殊槽分别容纳旁路对象和待回收
 * 对象。first/reserved chunk 在启动阶段创建并长期持有，不按普通动态 chunk
 * 生命周期处理。只声明不授予调用者越过锁读取可变字段的权限。
 */
extern spinlock_t pcpu_lock;

extern struct list_head *pcpu_chunk_lists;
extern int pcpu_nr_slots;
extern int pcpu_sidelined_slot;
extern int pcpu_to_depopulate_slot;
extern int pcpu_nr_empty_pop_pages;

extern struct pcpu_chunk *pcpu_first_chunk;
extern struct pcpu_chunk *pcpu_reserved_chunk;

/**
 * pcpu_chunk_nr_blocks - converts nr_pages to # of md_blocks
 * @chunk: chunk of interest
 *
 * This conversion is from the number of physical pages that the chunk
 * serves to the number of bitmap blocks used.
 */
/*
 * 业务背景：把 chunk 的 backing 页容量换算成分层搜索所需的 md_blocks 数量。
 * 入参：chunk 为借用且非 NULL 的已初始化 chunk；nr_pages 以页为单位。
 * 出参/返回：返回覆盖该 chunk 所需的元数据块数，不修改对象或引用。
 * 注意事项：换算依赖 PCPU_BITMAP_BLOCK_SIZE 可整齐覆盖 chunk bitmap；调用者
 * 用结果做数组边界或分配大小，chunk 生命周期必须覆盖计算过程。
 */
static inline int pcpu_chunk_nr_blocks(struct pcpu_chunk *chunk)
{
	return chunk->nr_pages * PAGE_SIZE / PCPU_BITMAP_BLOCK_SIZE;
}

/**
 * pcpu_nr_pages_to_map_bits - converts the pages to size of bitmap
 * @pages: number of physical pages
 *
 * This conversion is from physical pages to the number of bits
 * required in the bitmap.
 */
/*
 * 业务背景：动态 percpu 以 PCPU_MIN_ALLOC_SIZE 为最小粒度，需要把物理页容量
 * 转成 alloc_map/bound_map 的位数。
 * 入参：pages 是非负物理页数，仅作纯输入。
 * 出参/返回：返回对应 bitmap 位数；无输出参数、引用或全局副作用。
 * 注意事项：结果供数组寻址，调用者必须保证乘法范围和页/最小粒度的整除契约。
 */
static inline int pcpu_nr_pages_to_map_bits(int pages)
{
	return pages * PAGE_SIZE / PCPU_MIN_ALLOC_SIZE;
}

/**
 * pcpu_chunk_map_bits - helper to convert nr_pages to size of bitmap
 * @chunk: chunk of interest
 *
 * This conversion is from the number of physical pages that the chunk
 * serves to the number of bits in the bitmap.
 */
/*
 * 业务背景：为以 chunk 为入口的扫描路径封装页数到 bitmap 位数的换算。
 * 入参：chunk 是借用且非 NULL 的稳定对象，只读取 nr_pages。
 * 出参/返回：返回整个 chunk 的 map 位数，不改变 chunk 或 ownership。
 * 注意事项：它调用 pcpu_nr_pages_to_map_bits()；并发销毁时裸指针无生命周期保证，
 * 调用者仍须处于 pcpu_lock 或其他能稳定 chunk 的上下文。
 */
static inline int pcpu_chunk_map_bits(struct pcpu_chunk *chunk)
{
	return pcpu_nr_pages_to_map_bits(chunk->nr_pages);
}

/**
 * pcpu_obj_full_size - helper to calculate size of each accounted object
 * @size: size of area to allocate in bytes
 *
 * For each accounted object there is an extra space which is used to store
 * obj_cgroup membership if kmemcg is not disabled. Charge it too.
 */
/*
 * 业务背景：memcg 对一次逻辑 percpu 分配计费时，既要计所有可能 CPU 的数据，
 * 也要计保存各最小单元 obj_cgroup 指针的旁路空间，避免账面遗漏元数据成本。
 * 入参：size 是单 CPU 逻辑分配字节数，纯输入且应已按 percpu 粒度对齐。
 * 出参/返回：返回全部 possible CPU 数据加可选 memcg 指针数组的计费字节数；
 * 不实际分配、不取得引用。
 * 注意事项：MEMCG 未编入或 kmem 计费运行期禁用时 extra_size 为 0；profiling
 * 的 codetag 空间不在此 memcg 计费公式内。调用者负责保证算术不溢出。
 */
static inline size_t pcpu_obj_full_size(size_t size)
{
	size_t extra_size = 0;

#ifdef CONFIG_MEMCG
	/* 每个最小分配位保存一个 obj_cgroup 指针，因此按 size/粒度累加。 */
	if (!mem_cgroup_kmem_disabled())
		extra_size += size / PCPU_MIN_ALLOC_SIZE * sizeof(struct obj_cgroup *);
#endif

	return size * num_possible_cpus() + extra_size;
}

#ifdef CONFIG_PERCPU_STATS

#include <linux/spinlock.h>

struct percpu_stats {
	/* lifetime 计数单调累加；cur/max 描述同时存活对象的当前值和历史峰值。 */
	u64 nr_alloc;		/* lifetime # of allocations */
	u64 nr_dealloc;		/* lifetime # of deallocations */
	u64 nr_cur_alloc;	/* current # of allocations */
	u64 nr_max_alloc;	/* max # of live allocations */
	u32 nr_chunks;		/* current # of live chunks */
	u32 nr_max_chunks;	/* max # of live chunks */
	size_t min_alloc_size;	/* min allocation size */
	size_t max_alloc_size;	/* max allocation size */
};

/* pcpu_stats 与启动布局快照由 percpu-stats.c 展示，写侧均按下述锁协议更新。 */
extern struct percpu_stats pcpu_stats;
extern struct pcpu_alloc_info pcpu_stats_ai;

/*
 * For debug purposes. We don't care about the flexible array.
 */
/*
 * 业务背景：启动时保存 pcpu_alloc_info 固定头，供 debugfs 解释 first chunk 的
 * unit/group 布局；柔性数组内容不参与当前统计输出，所以原文明确忽略。
 * 入参：ai 是启动分配器借出的非 NULL 布局描述，函数只复制固定大小部分。
 * 出参/返回：void；覆盖全局 pcpu_stats_ai，并把最小分配大小初值设为 unit_size。
 * 注意事项：仅在早期 pcpu_setup_first_chunk() 的单线程初始化阶段调用，无需
 * pcpu_lock；复制后不持有 ai，柔性数组也没有 ownership 转移。
 */
static inline void pcpu_stats_save_ai(const struct pcpu_alloc_info *ai)
{
	memcpy(&pcpu_stats_ai, ai, sizeof(struct pcpu_alloc_info));

	/* initialize min_alloc_size to unit_size */
	/* 尚无样本时用单元大小作上界哨兵，首个更小分配会通过 min() 下调。 */
	pcpu_stats.min_alloc_size = pcpu_stats_ai.unit_size;
}

/*
 * pcpu_stats_area_alloc - increment area allocation stats
 * @chunk: the location of the area being allocated
 * @size: size of area to allocate in bytes
 *
 * CONTEXT:
 * pcpu_lock.
 */
/*
 * 业务背景：pcpu_alloc() 成功在 chunk bitmap 中认领区域后，记录全局与该
 * chunk 的分配次数、存活峰值和尺寸范围，供 percpu_stats 调试接口观察。
 * 入参：chunk 为当前持锁稳定的借用对象；size 是本次单 CPU 分配字节数。
 * 出参/返回：void；递增全局/局部统计，不取得引用，也不改变分配 bitmap。
 * 注意事项：调用者必须持有 pcpu_lock，函数通过 lockdep 验证；它是观测账本，
 * 不能替代真实 allocation 状态，且在后续 population 失败时由释放路径配平。
 */
static inline void pcpu_stats_area_alloc(struct pcpu_chunk *chunk, size_t size)
{
	lockdep_assert_held(&pcpu_lock);

	pcpu_stats.nr_alloc++;
	/* 先更新当前存活数，再用新值刷新历史峰值与尺寸边界。 */
	pcpu_stats.nr_cur_alloc++;
	pcpu_stats.nr_max_alloc =
		max(pcpu_stats.nr_max_alloc, pcpu_stats.nr_cur_alloc);
	pcpu_stats.min_alloc_size =
		min(pcpu_stats.min_alloc_size, size);
	pcpu_stats.max_alloc_size =
		max(pcpu_stats.max_alloc_size, size);

	chunk->nr_alloc++;
	chunk->max_alloc_size = max(chunk->max_alloc_size, size);
}

/*
 * pcpu_stats_area_dealloc - decrement allocation stats
 * @chunk: the location of the area being deallocated
 *
 * CONTEXT:
 * pcpu_lock.
 */
/*
 * 业务背景：pcpu_free_area() 释放已认领对象时配对撤销当前存活统计，同时保留
 * lifetime 与历史峰值供诊断碎片/负载。
 * 入参：chunk 是包含该对象且由 pcpu_lock 稳定的借用对象。
 * 出参/返回：void；递增 lifetime dealloc，递减全局和 chunk 当前存活数。
 * 注意事项：必须与一次成功的 area_alloc 配对；错误配对会产生下溢假象。
 * 函数不释放内存、不修改 bitmap，也不改变 chunk ownership。
 */
static inline void pcpu_stats_area_dealloc(struct pcpu_chunk *chunk)
{
	lockdep_assert_held(&pcpu_lock);

	pcpu_stats.nr_dealloc++;
	pcpu_stats.nr_cur_alloc--;

	chunk->nr_alloc--;
}

/*
 * pcpu_stats_chunk_alloc - increment chunk stats
 */
/*
 * 业务背景：VM/KM 后端成功创建一个动态 chunk 后记录当前数量与历史峰值。
 * 入参：无。
 * 出参/返回：void；在内部取得 pcpu_lock 并更新全局 chunk 统计。
 * 注意事项：可在尚未持有 pcpu_lock 的创建路径调用；irqsave 防止本 CPU 中断
 * 上下文重入同一锁。它不发布 chunk，槽链插入由通用分配路径另行完成。
 */
static inline void pcpu_stats_chunk_alloc(void)
{
	unsigned long flags;
	/* flags 保存本 CPU 原中断状态，解锁时精确恢复而非无条件开中断。 */
	spin_lock_irqsave(&pcpu_lock, flags);

	pcpu_stats.nr_chunks++;
	pcpu_stats.nr_max_chunks =
		max(pcpu_stats.nr_max_chunks, pcpu_stats.nr_chunks);

	spin_unlock_irqrestore(&pcpu_lock, flags);
}

/*
 * pcpu_stats_chunk_dealloc - decrement chunk stats
 */
/*
 * 业务背景：后端最终销毁动态 chunk 时与 chunk_alloc 配对，更新当前存活数。
 * 入参：无。
 * 出参/返回：void；在内部持 pcpu_lock 递减 nr_chunks，历史峰值保持不变。
 * 注意事项：必须在每个已统计 chunk 上恰好调用一次；函数只记账，不负责释放
 * chunk 内存。irqsave 允许来自不同中断状态的销毁路径安全复用。
 */
static inline void pcpu_stats_chunk_dealloc(void)
{
	unsigned long flags;
	spin_lock_irqsave(&pcpu_lock, flags);

	pcpu_stats.nr_chunks--;

	spin_unlock_irqrestore(&pcpu_lock, flags);
}

#else

/*
 * CONFIG_PERCPU_STATS=n 时这些同签名空桩让分配热路径无需散布条件编译。
 * 所有参数均为借用纯输入，返回均为 void；空桩不读取参数、不取得锁、不改变
 * allocation/ownership，因此启停统计只影响可观测性，不影响分配器语义。
 */
static inline void pcpu_stats_save_ai(const struct pcpu_alloc_info *ai)
{
}

/*
 * 业务背景：关闭统计时保留“区域分配记账”调用接口，使 percpu.c 共用同一源码。
 * 入参：chunk 与 size 均为未读取的借用输入，ownership 不变。
 * 出参/返回：void；不产生统计或分配副作用。
 * 注意事项：无需 pcpu_lock；该桩不改变真实分配结果。
 */
static inline void pcpu_stats_area_alloc(struct pcpu_chunk *chunk, size_t size)
{
}

/*
 * 业务背景：关闭统计时保留“区域释放记账”接口。
 * 入参：chunk 是未读取的借用指针。
 * 出参/返回：void；不修改对象、引用或全局状态。
 * 注意事项：无需 pcpu_lock，真实 bitmap 释放仍由调用者完成。
 */
static inline void pcpu_stats_area_dealloc(struct pcpu_chunk *chunk)
{
}

/*
 * 业务背景：关闭统计时保留动态 chunk 创建记账接口。
 * 入参：无。
 * 出参/返回：void；无任何副作用。
 * 注意事项：不取得锁，也不负责发布或持有 chunk。
 */
static inline void pcpu_stats_chunk_alloc(void)
{
}

/*
 * 业务背景：关闭统计时保留动态 chunk 销毁记账接口。
 * 入参：无。
 * 出参/返回：void；无任何副作用。
 * 注意事项：不取得锁，chunk 内存仍由后端释放。
 */
static inline void pcpu_stats_chunk_dealloc(void)
{
}

#endif /* !CONFIG_PERCPU_STATS */

#endif
