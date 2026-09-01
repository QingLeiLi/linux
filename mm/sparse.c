// SPDX-License-Identifier: GPL-2.0
/*
 * sparse memory mappings.
 */
/*
 * SPARSEMEM 把稀疏物理地址空间切成 section：先登记哪些 section 存在及 NUMA
 * 归属，再为其建立 struct page memmap 和 pageblock 使用位图，供 pfn_to_page 查询。
 */
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/mmzone.h>
#include <linux/memblock.h>
/* compiler/highmem/export 支撑早期段属性与公开符号；vmalloc/swap 连接 memmap 后端约束。 */
#include <linux/compiler.h>
#include <linux/highmem.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/bootmem_info.h>
/* internal.h 提供 section 最终提交 helper，asm/dma 给出非 vmemmap 分配地址下限。 */
#include <linux/vmstat.h>
#include "internal.h"
#include <asm/dma.h>

/*
 * Permanent SPARSEMEM data:
 *
 * 1) mem_section	- memory sections, mem_map's for valid memory
 */
/* 永久元数据只有 mem_section 索引；每个有效 section 再指向对应 memmap/usage。 */
#ifdef CONFIG_SPARSEMEM_EXTREME
/* EXTREME 两级索引按需分配 root 页，避免为巨大稀疏地址空间预留完整平面数组。 */
struct mem_section **mem_section;
#else
/* 普通模式在静态数组中一次保留所有 section descriptor，并按节点 cacheline 对齐。 */
struct mem_section mem_section[NR_SECTION_ROOTS][SECTIONS_PER_ROOT]
	____cacheline_internodealigned_in_smp;
#endif
EXPORT_SYMBOL(mem_section);

#ifdef NODE_NOT_IN_PAGE_FLAGS
/*
 * If we did not store the node number in the page then we have to
 * do a lookup in the section_to_node_table in order to find which
 * node the page belongs to.
 */
/* page flags 放不下 nid 时，以 section 号索引独立 NUMA 表；宽度按 MAX_NUMNODES 选择。 */
#if MAX_NUMNODES <= 256
static u8 section_to_node_table[NR_MEM_SECTIONS] __cacheline_aligned;
#else
static u16 section_to_node_table[NR_MEM_SECTIONS] __cacheline_aligned;
#endif

/*
 * 业务背景：memdesc flags 未编码 nid 的体系结构通过 section 旁表恢复 page 的 NUMA 节点。
 * 入参：mdf 是描述页/folio 的 flags 快照，必须含可解码的 section 编号。
 * 出参/返回：返回该 section 登记的 nid；不修改状态。
 * 注意事项：启动期 memory_present 先填表，运行期只读；不加锁、不睡眠。
 */
int memdesc_nid(memdesc_flags_t mdf)
{
	return section_to_node_table[memdesc_section(mdf)];
}
EXPORT_SYMBOL(memdesc_nid);

/*
 * 业务背景：登记 present section 时同步建立 section→NUMA 映射。
 * 入参：section_nr 为有效数组索引；nid 在配置允许的节点范围内。
 * 出参/返回：void；覆盖旁表元素，无 ownership 变化。
 * 注意事项：仅启动/热插拔串行路径写；读者依赖 section 发布前已完成该写入。
 */
static void set_section_nid(unsigned long section_nr, int nid)
{
	section_to_node_table[section_nr] = nid;
}
#else /* !NODE_NOT_IN_PAGE_FLAGS */
/* nid 已直接编码在 page flags 的配置不需要旁表；调用点保留为无操作统一接口。 */
/*
 * 业务背景：让 memory_present() 无需按 NODE_NOT_IN_PAGE_FLAGS 分叉。
 * 入参：section_nr/nid 均被该配置忽略。
 * 出参/返回：void，无副作用。
 * 注意事项：static inline 会被消除；nid 由其他编码路径提供。
 */
static inline void set_section_nid(unsigned long section_nr, int nid)
{
}
#endif

#ifdef CONFIG_SPARSEMEM_EXTREME
/*
 * 业务背景：为 EXTREME 两级 mem_section 索引分配一个 root 所含 descriptor 数组。
 * 入参：nid 指定优先分配节点；启动前期无法满足时 memblock 仍按节点请求。
 * 出参/返回：返回零化数组；slab 阶段 OOM 可返 NULL，早期 memblock OOM 直接 panic。
 * 注意事项：__ref 允许同时调用 init 和运行期 allocator；返回 ownership 交给全局索引且永久保留。
 */
static noinline struct mem_section __ref *sparse_index_alloc(int nid)
{
	/* array_size 固定覆盖一个 root 的 SECTIONS_PER_ROOT 个 descriptor。 */
	struct mem_section *section = NULL;
	unsigned long array_size = SECTIONS_PER_ROOT *
				   sizeof(struct mem_section);

	/* 已有 slab 时允许热插拔可恢复失败；更早启动期没有恢复手段，memblock 失败即 panic。 */
	if (slab_is_available()) {
		section = kzalloc_node(array_size, GFP_KERNEL, nid);
	} else {
		/* memblock 返回物理连续且按 cacheline 对齐的永久早期数组。 */
		section = memblock_alloc_node(array_size, SMP_CACHE_BYTES,
					      nid);
		if (!section)
			panic("%s: Failed to allocate %lu bytes nid=%d\n",
			      __func__, array_size, nid);
	}

	return section;
}

/*
 * 业务背景：确保指定 section 的 EXTREME 一级 root 已实例化，供 __nr_to_section 解引用。
 * 入参：section_nr 为待登记 section；nid 决定新 root 的内存归属。
 * 出参/返回：已存在或分配成功返 0，运行期分配失败返 -ENOMEM。
 * 注意事项：mem_hotplug_lock/启动单线程串行发布；root 一旦发布永久复用，不在此释放。
 */
int __meminit sparse_index_init(unsigned long section_nr, int nid)
{
	unsigned long root = SECTION_NR_TO_ROOT(section_nr);
	struct mem_section *section;

	/*
	 * An existing section is possible in the sub-section hotplug
	 * case. First hot-add instantiates, follow-on hot-add reuses
	 * the existing section.
	 *
	 * The mem_hotplug_lock resolves the apparent race below.
	 */
	/* 子 section 热添加可重复命中 root；mem_hotplug_lock 使检查、分配、发布不发生双分配。 */
	if (mem_section[root])
		return 0;

	/* 先完整分配零化数组，成功后一次写入一级指针形成发布边界。 */
	section = sparse_index_alloc(nid);
	if (!section)
		return -ENOMEM;

	mem_section[root] = section;

	return 0;
}
#else /* !SPARSEMEM_EXTREME */
/* 平面 mem_section 已静态存在，统一初始化接口不需要任何工作。 */
/*
 * 业务背景：让启动和热插拔调用者不必区分索引布局。
 * 入参：section_nr/nid 在静态数组配置下无需使用。
 * 出参/返回：恒返 0，无副作用。
 * 注意事项：不睡眠，所有 descriptor 已由 BSS 零化。
 */
int sparse_index_init(unsigned long section_nr, int nid)
{
	return 0;
}
#endif

/*
 * During early boot, before section_mem_map is used for an actual
 * mem_map, we use section_mem_map to store the section's NUMA
 * node.  This keeps us from having to use another data structure.  The
 * node information is cleared just before we store the real mem_map.
 */
/* 启动第一阶段借用 section_mem_map 的高位暂存 nid；真正 memmap 指针提交前会清除此编码。 */
/*
 * 业务背景：在尚无 memmap 时把 nid 编进已有字段，省去一份仅启动期使用的表。
 * 入参：nid 为待编码 NUMA 节点号。
 * 出参/返回：返回移到 SECTION_NID_SHIFT 的临时位模式，无副作用。
 * 注意事项：只能在 SECTION_MAP_MASK 尚未承载真实 memmap 时解释为 nid。
 */
static inline unsigned long sparse_encode_early_nid(int nid)
{
	return ((unsigned long)nid << SECTION_NID_SHIFT);
}

/*
 * 业务背景：sparse_init 分组时从 present 阶段的临时编码恢复 section 所属节点。
 * 入参：section 为已标记 present、尚未提交真实 memmap 的借用 descriptor。
 * 出参/返回：返回其启动期 nid，不修改字段。
 * 注意事项：sparse_init_one_section 后同一字段语义改变，不能再调用本 helper。
 */
static inline int sparse_early_nid(struct mem_section *section)
{
	return (section->section_mem_map >> SECTION_NID_SHIFT);
}

/* Validate the physical addressing limitations of the model */
/* 校验并裁剪架构传入 PFN 范围，使 section 编号不超过 SPARSEMEM 模型可寻址上限。 */
/*
 * 业务背景：固件/memblock 范围必须先适配 direct-map 与 sparse section 的共同物理上限。
 * 入参：start_pfn/end_pfn 为不可空输入输出边界，表示半开 PFN 区间。
 * 出参/返回：void；合法时不变，越界时告警并把一个或两个端点裁到最大 PFN。
 * 注意事项：启动期调用；裁剪避免后续数组越界，但异常内存将不可用。
 */
static void __meminit mminit_validate_memmodel_limits(unsigned long *start_pfn,
						unsigned long *end_pfn)
{
	unsigned long max_sparsemem_pfn = (DIRECT_MAP_PHYSMEM_END + 1) >> PAGE_SHIFT;

	/*
	 * Sanity checks - do not allow an architecture to pass
	 * in larger pfns than the maximum scope of sparsemem:
	 */
	/* 起点已越界时整个区间置空；仅终点越界时保留可表达前缀。 */
	if (*start_pfn > max_sparsemem_pfn) {
		/* 整段不可表示：记录原范围后把半开区间压成空区间。 */
		mminit_dprintk(MMINIT_WARNING, "pfnvalidation",
			"Start of range %lu -> %lu exceeds SPARSEMEM max %lu\n",
			*start_pfn, *end_pfn, max_sparsemem_pfn);
		WARN_ON_ONCE(1);
		*start_pfn = max_sparsemem_pfn;
		*end_pfn = max_sparsemem_pfn;
	} else if (*end_pfn > max_sparsemem_pfn) {
		/* 仅尾部越界：保留 [start,max) 可表达前缀并丢弃其余物理内存。 */
		mminit_dprintk(MMINIT_WARNING, "pfnvalidation",
			"End of range %lu -> %lu exceeds SPARSEMEM max %lu\n",
			*start_pfn, *end_pfn, max_sparsemem_pfn);
		WARN_ON_ONCE(1);
		*end_pfn = max_sparsemem_pfn;
	}
}

/*
 * There are a number of times that we loop over NR_MEM_SECTIONS,
 * looking for section_present() on each.  But, when we have very
 * large physical address spaces, NR_MEM_SECTIONS can also be
 * very large which makes the loops quite long.
 *
 * Keeping track of this gives us an easy way to break out of
 * those loops early.
 */
/* 记录最高 present section，令全局扫描在稀疏巨大物理地址空间中提前停止。 */
unsigned long __highest_present_section_nr;

/*
 * 业务背景：为后续按 present section 遍历取得第一个有效编号。
 * 入参：无。
 * 出参/返回：返回 next_present_section_nr(-1) 的结果；无副作用。
 * 注意事项：要求 memblocks_present 已建立 present 标志；不睡眠。
 */
static inline unsigned long first_present_section_nr(void)
{
	return next_present_section_nr(-1);
}

/* Record a memory area against a node. */
/* 把一个 NUMA 节点的 PFN 半开区间按 section 对齐并登记为 present/online。 */
/*
 * 业务背景：memblock 仍描述物理范围时，为第二阶段 memmap 分配建立 section 清单与临时 nid。
 * 入参：nid 为归属节点；start/end 是 PFN 半开区间，函数向下对齐 start 并裁剪物理上限。
 * 出参/返回：void；创建所需索引、写 nid，并首次发布 SECTION_MARKED_PRESENT/ONLINE。
 * 注意事项：启动单线程；重复覆盖同 section 时保留首次 section_mem_map 编码。
 */
static void __init memory_present(int nid, unsigned long start, unsigned long end)
{
	/* pfn 每次跨一个完整 section；section_nr/ms 是当前 descriptor 的借用定位值。 */
	unsigned long pfn;

	/* 阶段 1：纳入与范围起点相交的完整 section，并防止架构范围越界。 */
	start &= PAGE_SECTION_MASK;
	mminit_validate_memmodel_limits(&start, &end);
	/* 阶段 2：逐 section 确保索引存在，先写 nid 再发布 present 状态。 */
	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION) {
		unsigned long section_nr = pfn_to_section_nr(pfn);
		struct mem_section *ms;

		sparse_index_init(section_nr, nid);
		set_section_nid(section_nr, nid);

		ms = __nr_to_section(section_nr);
		/* 重叠 memblock 可能再次到达；只在首次登记时写临时编码并标 present。 */
		if (!ms->section_mem_map) {
			ms->section_mem_map = sparse_encode_early_nid(nid) |
							SECTION_IS_ONLINE;
			__section_mark_present(ms, section_nr);
		}
	}
}

/*
 * Mark all memblocks as present using memory_present().
 * This is a convenience function that is useful to mark all of the systems
 * memory as present during initialization.
 */
/* 遍历全部 memblock memory 范围并调用 memory_present()，形成系统 section 总清单。 */
/*
 * 业务背景：sparse_init 第一阶段把固件/架构建立的 memblock 视图转换为 SPARSEMEM 索引。
 * 入参：无；读取全局 memblock memory ranges。
 * 出参/返回：void；EXTREME 下先发布一级 root 指针数组，再登记所有节点范围。
 * 注意事项：仅启动期、可使用 memblock；分配失败 panic，因为没有 memmap 就无法继续启动。
 */
static void __init memblocks_present(void)
{
	unsigned long start, end;
	int i, nid;

#ifdef CONFIG_SPARSEMEM_EXTREME
	/* size/align 描述一级 root 指针表；该表永久存在并由 __nr_to_section 读取。 */
	unsigned long size, align;

	size = sizeof(struct mem_section *) * NR_SECTION_ROOTS;
	align = 1 << (INTERNODE_CACHE_SHIFT);
	mem_section = memblock_alloc_or_panic(size, align);
#endif

	/* 阶段 2：memblock 迭代给出 PFN 半开区间及 nid，重叠 section 由 memory_present 去重。 */
	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, &nid)
		memory_present(nid, start, end);
}

/*
 * 业务背景：计算每 section 的 pageblock flags 位图载荷大小。
 * 入参：无。
 * 出参/返回：返回容纳 SECTION_BLOCKFLAGS_BITS 所需 unsigned long 数组字节数。
 * 注意事项：纯算术、不睡眠；BITS_TO_LONGS 包含机器字对齐取整。
 */
static unsigned long usemap_size(void)
{
	return BITS_TO_LONGS(SECTION_BLOCKFLAGS_BITS) * sizeof(unsigned long);
}

/*
 * 业务背景：分配 usage descriptor 时需要把固定头和紧随其后的 usemap 一并预留。
 * 入参：无。
 * 出参/返回：返回单个 mem_section_usage 对象总字节数。
 * 注意事项：供启动与热插拔共同使用；不分配内存、无副作用。
 */
size_t mem_section_usage_size(void)
{
	return sizeof(struct mem_section_usage) + usemap_size();
}

#ifdef CONFIG_SPARSEMEM_VMEMMAP
/*
 * 业务背景：vmemmap 模式为一个 section 计算 PMD 对齐的虚拟 memmap 覆盖大小。
 * 入参：无。
 * 出参/返回：返回 PAGES_PER_SECTION 个 struct page 向 PMD_SIZE 上取整的字节数。
 * 注意事项：仅启动期查询；对齐便于建立大页 vmemmap 映射。
 */
unsigned long __init section_map_size(void)
{
	return ALIGN(sizeof(struct page) * PAGES_PER_SECTION, PMD_SIZE);
}

#else
/*
 * 业务背景：非 vmemmap 模式分配实际连续 mem_map backing，只需页对齐。
 * 入参：无。
 * 出参/返回：返回单 section struct page 数组的 PAGE_SIZE 对齐字节数。
 * 注意事项：值随后同时作为 memmap_alloc 的 size/alignment。
 */
unsigned long __init section_map_size(void)
{
	return PAGE_ALIGN(sizeof(struct page) * PAGES_PER_SECTION);
}

/*
 * 业务背景：非 vmemmap 配置为一个 section 分配连续 struct page 数组。
 * 入参：pfn/nr_pages/altmap/pgmap 为统一接口参数但本实现按整 section 分配；nid 指定节点。
 * 出参/返回：成功返回由 sparse section 永久持有的 map；失败直接 panic，不返回 NULL。
 * 注意事项：启动期可睡眠；优先限制在 MAX_DMA_ADDRESS 之后的可用 memmap backing。
 */
struct page __init *__populate_section_memmap(unsigned long pfn,
		unsigned long nr_pages, int nid, struct vmem_altmap *altmap,
		struct dev_pagemap *pgmap)
{
	unsigned long size = section_map_size();
	struct page *map;
	phys_addr_t addr = __pa(MAX_DMA_ADDRESS);

	/* size 同时作为对齐，确保 section memmap 可按模型编码到 section_mem_map。 */
	map = memmap_alloc(size, size, addr, nid, false);
	if (!map)
		panic("%s: Failed to allocate %lu bytes align=0x%lx nid=%d from=%pa\n",
		      __func__, size, PAGE_SIZE, nid, &addr);

	return map;
}
#endif /* !CONFIG_SPARSEMEM_VMEMMAP */

/*
 * 业务背景：架构/vmemmap 实现可覆盖此弱钩子，在批量 populate 结束后打印最终范围。
 * 入参：无。
 * 出参/返回：void；通用默认实现无副作用。
 * 注意事项：弱符号只提供链接兜底；当前配置的强实现决定是否输出。
 */
void __weak __meminit vmemmap_populate_print_last(void)
{
}

/* 当前 NUMA 分组预分配 usage 连续缓冲的消费游标和末端，仅 sparse_init_nid 期间有效。 */
static void *sparse_usagebuf __meminitdata;
static void *sparse_usagebuf_end __meminitdata;

/*
 * Helper function that is used for generic section initialization, and
 * can also be used by any hooks added above.
 */
/* 从当前 usage 缓冲切下一项，并把 memmap/usage/flags 一次提交给指定 section。 */
/*
 * 业务背景：通用和架构预初始化路径都用它完成早期 section 从“present+nid”到“有 memmap”的转换。
 * 入参：nid 为节点；map 为该 section 的 struct page 起点；pnum 为 section 号；flags 为附加状态位。
 * 出参/返回：void；section 接管当前 usage 槽并标 SECTION_IS_EARLY，游标前进一项。
 * 注意事项：缓冲耗尽触发 BUG；仅启动串行调用，map/usage 随 section 永久有效。
 */
void __init sparse_init_early_section(int nid, struct page *map,
				      unsigned long pnum, unsigned long flags)
{
	/* 先验证预分配容量，再提交 section，防止多个 section 共享或越界 usage。 */
	BUG_ON(!sparse_usagebuf || sparse_usagebuf >= sparse_usagebuf_end);
	sparse_init_one_section(__nr_to_section(pnum), pnum, map,
			sparse_usagebuf, SECTION_IS_EARLY | flags);
	sparse_usagebuf = (void *)sparse_usagebuf + mem_section_usage_size();
}

/*
 * 业务背景：为同一 NUMA 连续分组一次分配 map_count 个 usage，减少逐 section memblock 碎片。
 * 入参：nid 指定内存节点；map_count 是该分组需初始化的 present section 数。
 * 出参/返回：成功返 0并设置 [buf,end)；失败返 -ENOMEM 且 end=NULL。
 * 注意事项：启动期 memblock 分配，缓冲 ownership 随各槽分派给 section，不在 fini 释放。
 */
static int __init sparse_usage_init(int nid, unsigned long map_count)
{
	unsigned long size;

	/* 乘积是整个节点分组的线性缓冲；每次 early_section 精确消费一个对象跨度。 */
	size = mem_section_usage_size() * map_count;
	sparse_usagebuf = memblock_alloc_node(size, SMP_CACHE_BYTES, nid);
	if (!sparse_usagebuf) {
		sparse_usagebuf_end = NULL;
		return -ENOMEM;
	}

	sparse_usagebuf_end = sparse_usagebuf + size;
	return 0;
}

/*
 * 业务背景：节点分组完成或中途失败后关闭临时分配游标，防止误复用旧区间。
 * 入参：无。
 * 出参/返回：void；两个临时全局指针清 NULL，不释放已分派的 memblock 内存。
 * 注意事项：ownership 已转给各 mem_section->usage；这不是资源回滚函数。
 */
static void __init sparse_usage_fini(void)
{
	sparse_usagebuf = sparse_usagebuf_end = NULL;
}

/*
 * Initialize sparse on a specific node. The node spans [pnum_begin, pnum_end)
 * And number of present sections in this node is map_count.
 */
/* 初始化同一 nid 的 [pnum_begin,pnum_end)；map_count 是 present section 数并作为 usage 容量上界。 */
/*
 * 业务背景：按 NUMA 分组批量准备 usage，再为每个尚未由架构预初始化的 section 建立 memmap。
 * 入参：nid 为节点；pnum_begin/end 是 section 半开区间；map_count 为其中 present section 数，
 *       架构预初始化 section 会让对应预留槽保持未消费，但不会造成越界。
 * 出参/返回：void；成功提交所有可用 section；失败清除尚未预初始化 section 的 present 编码。
 * 注意事项：启动期；已 preinit 的 vmemmap section 必须保留，普通分配失败会使后续内存不可用。
 */
static void __init sparse_init_nid(int nid, unsigned long pnum_begin,
				   unsigned long pnum_end,
				   unsigned long map_count)
{
	unsigned long pnum;
	struct page *map;
	struct mem_section *ms;

	/* 阶段 1：先准备整个节点的 usage 账本；失败时没有 section 可安全提交。 */
	if (sparse_usage_init(nid, map_count)) {
		pr_err("%s: node[%d] usemap allocation failed", __func__, nid);
		goto failed;
	}

	/* 架构可在这里预建节点 vmemmap；非 vmemmap 配置为空桩。 */
	sparse_vmemmap_init_nid_early(nid);

	/* 阶段 2：只遍历 present section，并在到达当前节点半开终点时停止。 */
	for_each_present_section_nr(pnum_begin, pnum) {
		unsigned long pfn = section_nr_to_pfn(pnum);

		if (pnum >= pnum_end)
			break;

		ms = __nr_to_section(pnum);
		/* 架构已预初始化的 section 跳过；其 memmap/usage 不由本缓冲重复覆盖。 */
		if (!preinited_vmemmap_section(ms)) {
			map = __populate_section_memmap(pfn, PAGES_PER_SECTION,
					nid, NULL, NULL);
			/* vmemmap 实现可恢复地返回 NULL；从当前 pnum 起进入失败清理。 */
			if (!map) {
				pr_err("%s: node[%d] memory map backing failed. Some memory will not be available.",
				       __func__, nid);
				pnum_begin = pnum;
				sparse_usage_fini();
				goto failed;
			}
			/* 记启动期 memmap 页数，再提交 map 与当前 usage 槽到 section。 */
			memmap_boot_pages_add(DIV_ROUND_UP(PAGES_PER_SECTION * sizeof(struct page),
							   PAGE_SIZE));
			sparse_init_early_section(nid, map, pnum, 0);
		}
	}
	/* 成功出口只关闭临时游标；所有 usage 槽已由 section 持有。 */
	sparse_usage_fini();
	return;
failed:
	/*
	 * We failed to allocate, mark all the following pnums as not present,
	 * except the ones already initialized earlier.
	 */
	/*
	 * 回滚从失败点开始清除未 preinit section 的临时 nid/present 编码；此前已提交
	 * 的 section 与架构预初始化 section 保留，使可用内存前缀仍能启动。
	 */
	for_each_present_section_nr(pnum_begin, pnum) {
		if (pnum >= pnum_end)
			break;
		ms = __nr_to_section(pnum);
		if (!preinited_vmemmap_section(ms))
			ms->section_mem_map = 0;
	}
}

/*
 * Allocate the accumulated non-linear sections, allocate a mem_map
 * for each and record the physical to section mapping.
 */
/* 为所有 present section 按 NUMA 分组建立 memmap/usage，完成 SPARSEMEM 启动发布。 */
/*
 * 业务背景：mm_core_init 在 buddy 初始化前调用；输入是 memblock，输出是可用 pfn_to_page 映射。
 * 入参：无。
 * 出参/返回：void；发布全局 section 索引、pageblock order、memmap 与 usage。
 * 注意事项：仅启动单线程；分组依赖 early nid 编码，完成后该字段转为真实 memmap 语义。
 */
void __init sparse_init(void)
{
	/* pnum_begin/end 描述当前 nid 分组，map_count 是组内 present section 个数。 */
	unsigned long pnum_end, pnum_begin, map_count = 1;
	int nid_begin;

	/* see include/linux/mmzone.h 'struct mem_section' definition */
	/* section descriptor 大小必须为 2 的幂，才能让两级索引与地址计算保持位移/掩码语义。 */
	BUILD_BUG_ON(!is_power_of_2(sizeof(struct mem_section)));
	memblocks_present();

	/* 复合 folio 信息编码进 memdesc 时，零 PFN 的 vmemmap 基址还必须满足最大 folio 对齐。 */
	if (compound_info_has_mask()) {
		VM_WARN_ON_ONCE(!IS_ALIGNED((unsigned long) pfn_to_page(0),
				    MAX_FOLIO_VMEMMAP_ALIGN));
	}

	/* 阶段 1：取得首个 present section，并从尚未覆盖的字段解出首组 nid。 */
	pnum_begin = first_present_section_nr();
	nid_begin = sparse_early_nid(__nr_to_section(pnum_begin));

	/* Setup pageblock_order for HUGETLB_PAGE_SIZE_VARIABLE */
	/* 可变 HugeTLB 页大小会影响 pageblock 粒度，必须在分配 usemap 前确定。 */
	set_pageblock_order();

	/* 阶段 2：遇到 nid 边界就提交上一组；同 nid 的稀疏 section 仍归同一批 usage 分配。 */
	for_each_present_section_nr(pnum_begin + 1, pnum_end) {
		int nid = sparse_early_nid(__nr_to_section(pnum_end));

		if (nid == nid_begin) {
			map_count++;
			continue;
		}
		/* Init node with sections in range [pnum_begin, pnum_end) */
		/* 当前 pnum_end 属于新节点，因此上一节点分组严格是半开区间。 */
		sparse_init_nid(nid_begin, pnum_begin, pnum_end, map_count);
		nid_begin = nid;
		pnum_begin = pnum_end;
		map_count = 1;
	}
	/* cover the last node */
	/* 循环只在边界提交前组，最终显式提交尾组并结束 vmemmap 范围输出。 */
	sparse_init_nid(nid_begin, pnum_begin, pnum_end, map_count);
	vmemmap_populate_print_last();
}
