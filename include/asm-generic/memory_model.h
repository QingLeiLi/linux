/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 学习注释：这个头文件解决一个 MM 子系统的基础业务问题：
 * 给定“物理页帧号 PFN”，如何找到描述该物理页的 struct page；
 * 反过来，给定 struct page，又如何知道它描述的是哪个 PFN。
 *
 * 名词先对齐：
 * - physical address：物理地址，字节粒度。
 * - PFN(Page Frame Number)：页帧号，等于物理地址按 PAGE_SIZE 切页后的编号。
 *   换算关系通常是 paddr >> PAGE_SHIFT。
 * - struct page：每个可由内核管理的物理页对应的元数据，记录引用计数、
 *   flags、LRU/伙伴系统状态、复合页关系等；它不是页内容本身。
 * - mem_map/vmemmap：struct page 数组的基址。不同内存模型决定这个数组在
 *   地址空间里是否连续、是否按 section 分段。
 * - section：SPARSEMEM 把物理地址空间切成的大块。一个 section 覆盖一段
 *   PFN，并有自己的 mem_section 元数据。
 *
 * 为什么需要多种 memory model：
 * 早期小系统物理内存通常比较连续，用一个全局 mem_map 数组就能描述
 * 所有页；现代系统可能有 NUMA、大洞、内存热插拔、设备私有内存。
 * 若仍强制为整个物理地址空间建立一条巨大的 struct page 数组，
 * 会浪费大量元数据内存。
 *
 * 三种模型的业务取舍：
 * - FLATMEM：把所有页看成一段连续 PFN 区间，转换最快、概念最简单。
 *   代价是要求内存布局足够平坦，不适合巨大空洞。
 * - SPARSEMEM：按 section 记录哪些物理范围存在，
 *   适合稀疏地址空间和热插拔。
 *   代价是 pfn/page 转换要先找到 section，再解码 section_mem_map。
 * - SPARSEMEM_VMEMMAP：物理内存仍可稀疏，但把 struct page 元数据映射到
 *   连续虚拟地址 vmemmap 中。转换重新变成基址加减，速度接近 FLATMEM；
 *   代价是要建立 vmemmap 页表映射。
 *
 * 这个文件的价值：上层 MM 代码只调用 page_to_pfn()/pfn_to_page()，不需要
 * 到处写 CONFIG_FLATMEM/SPARSEMEM 分支。模型差异被收敛在这里。
 */
#ifndef __ASM_MEMORY_MODEL_H
#define __ASM_MEMORY_MODEL_H

/*
 * pfn.h 提供物理地址和 PFN 的基础换算：
 * PHYS_PFN(x) 丢弃页内偏移，PFN_PHYS(x) 得到页帧起始物理地址。
 */
#include <linux/pfn.h>

/*
 * 这里排除汇编编译单元。
 *
 * 原因：下面的宏会使用 struct page 指针运算、statement expression 以及
 * C 类型检查。汇编侧只适合使用常量和架构定义，不能理解这些 C 语义。
 */
#ifndef __ASSEMBLY__

/*
 * supports 3 memory models.
 *
 * 中文翻译：支持 3 种内存模型。
 *
 * 中文补充：三种模型由 Kconfig 互斥选择。
 *
 * 逻辑重点：每个分支都必须实现两个底层原语：
 * - __pfn_to_page(pfn)：PFN -> struct page *
 * - __page_to_pfn(page)：struct page * -> PFN
 * 后面再把它们统一别名成 page_to_pfn()/pfn_to_page()。
 */
#if defined(CONFIG_FLATMEM)

/*
 * FLATMEM 的 mental model：
 *
 * mem_map 是一条连续 struct page 数组，mem_map[0] 不一定表示 PFN 0。
 * 有些架构的可管理物理内存从非零 PFN 开始，所以用 ARCH_PFN_OFFSET 表示
 * “mem_map[0] 对应哪个 PFN”。
 *
 * 例子：如果 ARCH_PFN_OFFSET 为 1000，则 PFN 1000 对应 mem_map[0]，
 * PFN 1001 对应 mem_map[1]。
 */
#ifndef ARCH_PFN_OFFSET
#define ARCH_PFN_OFFSET		(0UL)
#endif

/*
 * __pfn_to_page() - FLATMEM 下把 PFN 转成 struct page *。
 * @pfn: 入参，待转换的页帧号。
 *
 * 返回值：mem_map 数组中对应 @pfn 的元素地址。
 *
 * 逻辑：数组下标 = pfn - ARCH_PFN_OFFSET。
 *
 * 为什么这么处理：FLATMEM 假设可管理内存在 PFN 视角近似连续，
 * 所以转换可以简化成一次指针加法。这是它速度快的来源。
 *
 * 注意：宏本身不做有效性检查。外部输入、设备地址或洞中的 PFN 应先走
 * pfn_valid()，否则可能得到越界的 struct page 指针。
 */
#define __pfn_to_page(pfn)	(mem_map + ((pfn) - ARCH_PFN_OFFSET))
/*
 * __page_to_pfn() - FLATMEM 下把 struct page * 转回 PFN。
 * @page: 入参，指向 mem_map 数组内的 page 描述符。
 *
 * 返回值：数组下标加上 ARCH_PFN_OFFSET 后得到的 PFN。
 *
 * 逻辑：PFN = (page - mem_map) + ARCH_PFN_OFFSET。
 * 这和 __pfn_to_page() 完全互逆。
 */
#define __page_to_pfn(page)	((unsigned long)((page) - mem_map) + \
				 ARCH_PFN_OFFSET)

/* avoid <linux/mm.h> include hell */
/*
 * 中文翻译：避免引入 <linux/mm.h> 造成包含依赖混乱。
 *
 * 业务含义：max_mapnr 是 FLATMEM 中 mem_map 覆盖的页数。
 *
 * 为什么只 extern：memory_model.h 是很多低层头文件会间接包含的基础头。
 * 如果为了 max_mapnr 去包含完整 <linux/mm.h>，容易形成头文件环和大规模
 * 隐式依赖，所以这里只声明变量。
 */
extern unsigned long max_mapnr;

#ifndef pfn_valid
/*
 * pfn_valid() - FLATMEM 默认的 PFN 有效性检查。
 * @pfn: 入参，待检查的页帧号。
 *
 * 返回值：非 0 表示 @pfn 落在 mem_map 覆盖范围内；0 表示不能安全转换为
 * struct page。
 *
 * 逻辑：有效 PFN 必须落在
 * [ARCH_PFN_OFFSET, ARCH_PFN_OFFSET + max_mapnr)。
 *
 * 注意：“有 struct page”不等于“可分配普通内存”。
 * pfn_valid() 只说明转换 page 描述符是安全的；该页是否 online、reserved、
 * device memory、属于哪个 zone，还要看更高层的内存管理状态。
 */
static inline int pfn_valid(unsigned long pfn)
{
	/* 缓存起始 PFN，避免宏重复展开，也让边界表达式更清楚。 */
	unsigned long pfn_offset = ARCH_PFN_OFFSET;

	/*
	 * 左边界：pfn 不能小于 mem_map[0] 对应的 PFN。
	 * 右边界：pfn 与起点的距离必须小于 mem_map 数组长度。
	 */
	return pfn >= pfn_offset && (pfn - pfn_offset) < max_mapnr;
}

/* 标记 pfn_valid 已由本头文件提供，避免后续通用代码再次定义。 */
#define pfn_valid pfn_valid

#ifndef for_each_valid_pfn
/*
 * for_each_valid_pfn() - FLATMEM 默认的有效 PFN 区间遍历器。
 * @pfn: 出参/循环变量，逐个接收有效 PFN。
 * @start_pfn: 入参，请求遍历的起始 PFN。
 * @end_pfn: 入参，请求遍历的结束 PFN，半开区间右边界。
 *
 * 输出语义：循环体中 @pfn 落在两个区间的交集：
 * - 调用者请求的 [start_pfn, end_pfn)
 * - FLATMEM 实际可转换的 mem_map 覆盖范围
 *
 * 为什么是半开区间：内核 PFN 遍历普遍使用 [start, end)，这样范围长度就是
 * end - start，拼接相邻范围时也不会重复处理边界页。
 */
#define for_each_valid_pfn(pfn, start_pfn, end_pfn)			 \
	for ((pfn) = max_t(unsigned long, (start_pfn), ARCH_PFN_OFFSET); \
	     (pfn) < min_t(unsigned long, (end_pfn),			 \
			   ARCH_PFN_OFFSET + max_mapnr);		 \
	     (pfn)++)
#endif /* for_each_valid_pfn */
#endif /* valid_pfn */

#elif defined(CONFIG_SPARSEMEM_VMEMMAP)

/* memmap is virtually contiguous.  */
/*
 * 中文翻译：memmap 在虚拟地址空间中是连续的。
 *
 * 业务含义：物理内存可以有洞，但 struct page 元数据在虚拟地址上被组织成
 * 一条连续数组，基址是 vmemmap。
 *
 * 为什么这么做：普通 SPARSEMEM 每次转换要查 section；vmemmap 用页表把分散
 * 的 struct page 后端内存映射成连续虚拟视图，于是转换重新变成
 * vmemmap + pfn。
 *
 * 优点：转换快，page_to_pfn()/pfn_to_page() 不需要访问 section 元数据。
 * 代价：需要为 vmemmap 建立页表映射，
 * 并占用一部分虚拟地址空间和页表内存。
 */
/*
 * @pfn 是入参，返回 vmemmap[pfn] 对应的 struct page *。
 * 调用者仍需先确认 PFN 有效；宏本身只做地址换算。
 */
#define __pfn_to_page(pfn)	(vmemmap + (pfn))
/*
 * @page 是入参，返回它相对 vmemmap 基址的下标，即 PFN。
 * 这依赖 vmemmap 虚拟数组的连续性。
 */
#define __page_to_pfn(page)	(unsigned long)((page) - vmemmap)

#elif defined(CONFIG_SPARSEMEM)
/*
 * Note: section's mem_map is encoded to reflect its start_pfn.
 * section[i].section_mem_map == mem_map's address - start_pfn;
 *
 * 中文翻译：注意，section 的 mem_map 会编码进该 section 的 start_pfn。
 * section[i].section_mem_map 等于 mem_map 地址减去 start_pfn。
 *
 * 名词解释：
 * - section：一段固定大小的 PFN 区间。
 * - mem_section：描述某个 section 是否 present/online、它的 mem_map 在哪。
 * - section_mem_map：不是单纯指针。它的高位编码 mem_map - start_pfn，
 *   低位还借来保存 SECTION_* 状态位。
 *
 * 为什么要存 mem_map - start_pfn：
 * 给定某个 section 内的 PFN，想要 page 指针时可以直接：
 *     encoded_base + pfn
 * 如果存裸 mem_map，则还要先算 pfn - section_start_pfn。
 * 这个编码把 section 起点提前折进 base，换来热路径少做一步运算。
 *
 * 代价：section_mem_map 不再是可直接解引用的裸指针。使用前必须通过
 * __section_mem_map_addr() 清掉低位 flags。
 */
/*
 * __page_to_pfn() - 普通 SPARSEMEM 下由 struct page * 反推出 PFN。
 * @pg: 入参，待转换的 struct page 指针。
 *
 * 返回值：@pg 对应的页帧号。
 *
 * 逻辑：
 * 1. page->flags 中编码了它所属的 section 编号。
 * 2. __nr_to_section(__sec) 找到该 section 的 mem_section。
 * 3. __section_mem_map_addr() 取出编码后的 mem_map 基址。
 * 4. page - encoded_base 得到 PFN。
 *
 * 为什么 page->flags 能反查 section：
 * 普通 SPARSEMEM 的 struct page 元数据不是全局连续数组，单靠 page 指针减
 * 一个全局基址无法知道 PFN。因此每个 page 必须能找到自己的 section。
 */
#define __page_to_pfn(pg)					\
({	const struct page *__pg = (pg);				\
	int __sec = memdesc_section(__pg->flags);		\
	(unsigned long)(__pg - __section_mem_map_addr(__nr_to_section(__sec)));	\
})

/*
 * __pfn_to_page() - 普通 SPARSEMEM 下由 PFN 找到 struct page *。
 * @pfn: 入参，待转换的页帧号。
 *
 * 返回值：该 PFN 在所属 section 中对应的 struct page 指针。
 *
 * 逻辑：
 * 1. __pfn_to_section(__pfn) 通过 PFN 高位找到所属 section。
 * 2. __section_mem_map_addr(__sec) 得到该 section 的 encoded base。
 * 3. encoded base + pfn 得到 struct page *。
 *
 * 注意：这里假设 @pfn 所属 section 已存在且已初始化 mem_map。
 * 对洞、离线内存、设备内存或用户输入 PFN，
 * 应先做 pfn_valid() 或更具体检查。
 */
#define __pfn_to_page(pfn)				\
({	unsigned long __pfn = (pfn);			\
	struct mem_section *__sec = __pfn_to_section(__pfn);	\
	__section_mem_map_addr(__sec) + __pfn;		\
})
#endif /* CONFIG_FLATMEM/SPARSEMEM */

/*
 * Convert a physical address to a Page Frame Number and back
 *
 * 中文翻译：把物理地址转换为页帧号，并支持反向转换。
 *
 * 业务含义：物理地址是字节粒度，PFN 是页粒度。
 *
 * __phys_to_pfn(paddr) 会丢弃页内 offset。
 * __pfn_to_phys(pfn) 返回页帧起始地址，而不是原始物理地址。
 *
 * 如果调用者需要精确地址，
 * 必须额外保存 paddr & ~PAGE_MASK 这样的页内偏移。
 */
#define	__phys_to_pfn(paddr)	PHYS_PFN(paddr)
#define	__pfn_to_phys(pfn)	PFN_PHYS(pfn)

/*
 * 对外统一名称。
 *
 * 设计目的：上层 MM 代码只依赖 page_to_pfn()/pfn_to_page()。
 * 哪种内存模型、是否经过 section、是否走 vmemmap，都在本文件内收敛。
 */
#define page_to_pfn __page_to_pfn
#define pfn_to_page __pfn_to_page

#ifdef CONFIG_DEBUG_VIRTUAL
/*
 * DEBUG_VIRTUAL 下 page_to_phys() 会先验证 page_to_pfn() 的结果。
 * @page: 入参，待转换的 struct page。
 *
 * 返回值：@page 对应页帧的起始物理地址。
 *
 * 调试语义：先把 page 转成 PFN，再用 pfn_valid() 验证。
 *
 * 为什么只在 DEBUG_VIRTUAL 下检查：
 * page_to_phys() 是热路径基础宏，生产配置不能每次都付出检查成本。
 * 调试配置则更重视尽早发现“把非法 struct page 当普通页”的错误。
 */
#define page_to_phys(page)						\
({									\
	unsigned long __pfn = page_to_pfn(page);			\
									\
	WARN_ON_ONCE(!pfn_valid(__pfn));				\
	PFN_PHYS(__pfn);						\
})
#else
/*
 * 非 DEBUG_VIRTUAL 构建省掉 pfn_valid()，直接组合转换。
 *
 * 优点：最短路径，适合频繁调用。
 * 风险：调用者传入非法 page 时，问题不会在这里被捕获。
 */
#define page_to_phys(page)	PFN_PHYS(page_to_pfn(page))
#endif /* CONFIG_DEBUG_VIRTUAL */
/*
 * phys_to_page() - 物理地址到 struct page 的组合转换。
 * @phys: 入参，物理地址；页内偏移会在 PHYS_PFN() 中被截断。
 *
 * 返回值：包含 @phys 所在页帧的 struct page 指针。
 *
 * 注意：它返回的是页描述符，不携带页内偏移。
 * 若调用者需要恢复精确地址，必须另外保存 offset_in_page(phys)。
 *
 * 例子：phys 指向某页中间，phys_to_page(phys) 只告诉你是哪一页；
 * 页内第几个字节不在 struct page 指针里表达。
 */
#define phys_to_page(phys)	pfn_to_page(PHYS_PFN(phys))

#endif /* __ASSEMBLY__ */

#endif
