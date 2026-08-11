// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018-2020 Christoph Hellwig.
 *
 * DMA operations that map physical memory directly without using an IOMMU.
 */
/*
 * 本文件实现不经过 IOMMU、直接以系统物理内存建立 DMA 映射的核心操作。设备地址范围偏移、
 * 非一致性缓存、内存加密以及 SWIOTLB/P2P 等例外仍由下列分层路径显式处理。
 */
#include <linux/memblock.h> /* for max_pfn */
/* 上面的 memblock 头提供 max_pfn，用于计算系统最高物理地址及直接 DMA 所需掩码。 */
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/dma-map-ops.h>
#include <linux/scatterlist.h>
#include <linux/pfn.h>
#include <linux/vmalloc.h>
#include <linux/set_memory.h>
#include <linux/slab.h>
#include <linux/pci-p2pdma.h>
#include "direct.h"

/*
 * Most architectures use ZONE_DMA for the first 16 Megabytes, but some use
 * it for entirely different regions. In that case the arch code needs to
 * override the variable below for dma-direct to work properly.
 */
/*
 * 多数体系结构把 ZONE_DMA 定义为最低 16 MiB，但也有体系结构把它放在完全不同的物理区间；
 * 后一种体系结构必须覆盖下面的变量，直接 DMA 才能据此选择真正可寻址的低端内存区域。
 *
 * 这里保存“选择 GFP_DMA 时能够覆盖到的最高物理地址”，而不是设备自己的 DMA 掩码。
 * 它在只读初始化阶段之后冻结，供分配区选择和逐级回退共同读取。
 */
u64 zone_dma_limit __ro_after_init = DMA_BIT_MASK(24);

/*
 * 把 CPU 物理地址转换成该设备在总线上使用的 DMA 地址。
 *
 * @dev: 正在建立直接映射的设备；其 DMA 范围及内存加密约束决定换算规则。
 * @phys: CPU 视角的物理地址，调用者仍负责保证整个区间可被设备访问。
 * 返回值: 普通设备走 phys_to_dma()；必须使用未加密 DMA 的设备走体系结构专用换算。
 *
 * 本函数只做地址域转换，不创建映射、不转移所有权，也不执行缓存同步；其结果会被一致性分配、
 * 流式映射和能力检查路径继续验证。它不加锁、不睡眠，内联保持 dma-direct 热路径的低开销。
 */
static inline dma_addr_t phys_to_dma_direct(struct device *dev,
		phys_addr_t phys)
{
	if (force_dma_unencrypted(dev))
		return phys_to_dma_unencrypted(dev, phys);
	return phys_to_dma(dev, phys);
}

/*
 * 根据直接 DMA 地址找回承载它的 struct page。
 *
 * @dev: 地址所属设备，dma_to_phys() 需要使用它的 DMA 范围映射。
 * @dma_addr: 先前由直接映射路径产生且指向普通系统内存的 DMA 地址。
 * 返回值: 对应物理页的页描述符；不增加页引用计数。
 *
 * 调用者必须保证地址不是 P2P 总线地址等无普通 struct page 的特殊地址，并在原映射/分配仍存活时
 * 使用返回值。该辅助函数没有锁和睡眠，仅完成 DMA 地址、物理地址、PFN、page 的逐层换算。
 */
static inline struct page *dma_direct_to_page(struct device *dev,
		dma_addr_t dma_addr)
{
	return pfn_to_page(PHYS_PFN(dma_to_phys(dev, dma_addr)));
}

/*
 * 计算覆盖系统最高内存地址所需的最小“全 1”DMA 掩码。
 *
 * @dev: 查询设备；设备的 DMA 范围以及未加密地址别名会影响最高 DMA 地址。
 * 返回值: 形如 2^n-1 的掩码，足以表示翻译后的最后一个系统内存字节。
 *
 * max_pfn 描述系统物理内存上界，先转为最后一个字节的物理地址，再经 dma-direct 地址域换算；
 * fls64() 找最高有效位，随后向上扩成全 1 掩码。调用者通常据此选择 coherent_dma_mask；本函数
 * 不修改设备，也不保证该掩码已由硬件支持，且依赖系统至少存在一个可表示的物理页这一启动期前提。
 * 它只读启动期稳定的内存上界与设备地址配置，不加锁、不睡眠。
 */
u64 dma_direct_get_required_mask(struct device *dev)
{
	phys_addr_t phys = ((phys_addr_t)max_pfn << PAGE_SHIFT) - 1;
	u64 max_dma = phys_to_dma_direct(dev, phys);

	return (1ULL << (fls64(max_dma) - 1)) * 2 - 1;
}

/*
 * 为一致性 DMA 分配选择最有希望一次成功的内存区标志。
 *
 * @dev: 目标设备，其 coherent_dma_mask 与 bus_dma_limit 共同限定可见地址上界。
 * @phys_limit: 输出设备 DMA 上界换算到 CPU 物理地址域后的上限，供后续回退判断复用。
 * 返回值: GFP_DMA、GFP_DMA32 或 0；调用者把它并入原始 gfp。
 *
 * 这里只是优化初次尝试，并不证明所分配页面一定可寻址：非恒等 dma_range_map 等因素仍需由
 * dma_coherent_ok() 对实际页面复核。无对应 zone 的体系结构会把相关 GFP 标志视为空操作；本函数
 * 仅计算标志和输出上限，不加锁、不睡眠。
 */
static gfp_t dma_direct_optimal_gfp_mask(struct device *dev, u64 *phys_limit)
{
	u64 dma_limit = min_not_zero(
		dev->coherent_dma_mask,
		dev->bus_dma_limit);

	/*
	 * Optimistically try the zone that the physical address mask falls
	 * into first.  If that returns memory that isn't actually addressable
	 * we will fallback to the next lower zone and try again.
	 *
	 * Note that GFP_DMA32 and GFP_DMA are no ops without the corresponding
	 * zones.
	 */
	/*
	 * 先乐观尝试物理地址上限所在的内存区；若实际返回页面仍不可寻址，外层会逐级退到更低区域。
	 * 没有配置相应 ZONE 时，GFP_DMA32/GFP_DMA 本身不会改变分配行为。
	 */
	*phys_limit = dma_to_phys(dev, dma_limit);
	if (*phys_limit <= zone_dma_limit)
		return GFP_DMA;
	if (*phys_limit <= DMA_BIT_MASK(32))
		return GFP_DMA32;
	return 0;
}

/*
 * 判断一段物理内存能否作为该设备的一致性 DMA 缓冲区。
 *
 * @dev: 目标设备。
 * @phys: 缓冲区首字节的 CPU 物理地址。
 * @size: 缓冲区字节数；调用契约要求非零且区间端点运算不溢出。
 * 返回值: 地址可翻译且最后一个 DMA 字节不超过 coherent/bus 两类限制时为 true。
 *
 * min_not_zero() 使值为 0 的“未设置”限制不意外压低有效限制。该函数不保留映射也不触碰缓存，
 * 因而可作为 CMA、原子池等分配器的候选页过滤回调；并发安全性来自只读设备配置，且不会睡眠。
 */
bool dma_coherent_ok(struct device *dev, phys_addr_t phys, size_t size)
{
	dma_addr_t dma_addr = phys_to_dma_direct(dev, phys);

	if (dma_addr == DMA_MAPPING_ERROR)
		return false;
	return dma_addr + size - 1 <=
		min_not_zero(dev->coherent_dma_mask, dev->bus_dma_limit);
}

/*
 * 在设备要求共享未加密内存时，把 CPU 线性映射中的页切换为解密状态。
 *
 * @dev: 目标设备；force_dma_unencrypted() 决定是否需要转换。
 * @vaddr: 页对齐内核虚拟地址，覆盖即将交给设备的物理页。
 * @size: 转换字节数，向上取整为页数。
 * 返回值: 无需转换或转换成功时为 0，否则为体系结构 set_memory_decrypted() 错误码。
 *
 * 转换会改变页表/内存加密属性，可能涉及跨 CPU 同步，调用者必须处于可阻塞上下文；成功后释放
 * 前必须由 dma_set_encrypted() 恢复。函数本身不分配页面，也不改变 DMA 地址。
 */
static int dma_set_decrypted(struct device *dev, void *vaddr, size_t size)
{
	if (!force_dma_unencrypted(dev))
		return 0;
	return set_memory_decrypted((unsigned long)vaddr, PFN_UP(size));
}

/*
 * 把曾供未加密 DMA 使用的 CPU 线性映射恢复为加密状态。
 *
 * @dev: 原分配所属设备。
 * @vaddr: 与解密操作相同的页对齐内核虚拟地址。
 * @size: 要恢复的字节范围，向上取整为页数。
 * 返回值: 无需转换或恢复成功时为 0；失败时返回体系结构错误并限速告警。
 *
 * 失败意味着页面加密属性已不适合回到普通页分配器，因此上层刻意泄漏整块内存，而不是冒险让
 * 后续 CPU 或设备以错误属性复用。该安全失败策略是 free 路径的重要所有权边界。
 * 属性转换可能涉及页表和 TLB 同步，调用上下文须允许对应体系结构实现完成这些操作。
 */
static int dma_set_encrypted(struct device *dev, void *vaddr, size_t size)
{
	int ret;

	if (!force_dma_unencrypted(dev))
		return 0;
	ret = set_memory_encrypted((unsigned long)vaddr, PFN_UP(size));
	if (ret)
		pr_warn_ratelimited("leaking DMA memory that can't be re-encrypted\n");
	return ret;
}

/*
 * 释放 __dma_direct_alloc_pages() 返回的底层连续页。
 *
 * @dev: 分配所属设备。
 * @page: 连续区域首页，调用者已完成缓存/加密属性恢复。
 * @size: 原分配大小，用于识别和释放整个区域。
 *
 * 先让 SWIOTLB 判断页面是否来自其专用分配池；命中后它已完成释放。否则交给
 * dma_free_contiguous()，与 CMA/伙伴系统来源透明配对。函数可触发分配器锁操作，调用者不得继续
 * 使用 page，且不得重复释放。
 */
static void __dma_direct_free_pages(struct device *dev, struct page *page,
				    size_t size)
{
	if (swiotlb_free(dev, page, size))
		return;
	dma_free_contiguous(dev, page, size);
}

/*
 * 从“用于分配”的 SWIOTLB 区域取得设备可寻址的连续页。
 *
 * @dev: 启用了 is_swiotlb_for_alloc() 策略的设备。
 * @size: 页对齐分配大小。
 * 返回值: 同时满足 SWIOTLB 来源和 coherent/bus 地址限制的首页；失败返回 NULL。
 *
 * SWIOTLB 给出的页仍需经过 dma_coherent_ok() 验证，因为池的位置未必落在设备地址窗口内；验证
 * 失败立即归还，确保返回非空时所有权才转给上层。该路径分配的是最终缓冲页，不是流式映射 bounce。
 * 池内部负责并发保护；调用上下文必须满足 swiotlb_alloc() 的分配约束。
 */
static struct page *dma_direct_alloc_swiotlb(struct device *dev, size_t size)
{
	struct page *page = swiotlb_alloc(dev, size);

	if (page && !dma_coherent_ok(dev, page_to_phys(page), size)) {
		swiotlb_free(dev, page, size);
		return NULL;
	}

	return page;
}

/*
 * 为 dma-direct 的多种分配接口取得物理连续、设备可寻址的底层页。
 *
 * @dev: 目标设备，NUMA 节点、DMA 掩码和 SWIOTLB 策略均从中读取。
 * @size: 已按页对齐的字节数。
 * @gfp: 调用者的分配约束；函数会追加最合适的 DMA zone 标志。
 * @allow_highmem: 是否允许返回没有永久内核线性映射的高端页。
 * 返回值: 成功时为连续区域首页，所有权交给调用者；失败为 NULL。
 *
 * 路径先处理专用 SWIOTLB 分配，再尝试 dma_alloc_contiguous()。候选页必须同时满足实际 DMA
 * 可寻址性和高端页策略，否则立即释放。最后用伙伴系统重试；若分到了不可寻址页，则从普通区依次
 * 回退到 DMA32、DMA，每次先释放失败候选，避免泄漏。此函数只取得页面，不清零、不改缓存或加密
 * 属性；那些步骤由不同公开分配接口按其返回语义完成。分配动作可能睡眠，具体由 gfp 决定。
 */
static struct page *__dma_direct_alloc_pages(struct device *dev, size_t size,
		gfp_t gfp, bool allow_highmem)
{
	int node = dev_to_node(dev);
	struct page *page;
	u64 phys_limit;

	WARN_ON_ONCE(!PAGE_ALIGNED(size));

	if (is_swiotlb_for_alloc(dev))
		return dma_direct_alloc_swiotlb(dev, size);

	gfp |= dma_direct_optimal_gfp_mask(dev, &phys_limit);
	page = dma_alloc_contiguous(dev, size, gfp);
	if (page) {
		if (dma_coherent_ok(dev, page_to_phys(page), size) &&
		    (allow_highmem || !PageHighMem(page)))
			return page;

		dma_free_contiguous(dev, page, size);
	}

	while ((page = alloc_pages_node(node, gfp, get_order(size)))
	       && !dma_coherent_ok(dev, page_to_phys(page), size)) {
		__free_pages(page, get_order(size));

		if (IS_ENABLED(CONFIG_ZONE_DMA32) &&
		    phys_limit < DMA_BIT_MASK(64) &&
		    !(gfp & (GFP_DMA32 | GFP_DMA)))
			gfp |= GFP_DMA32;
		else if (IS_ENABLED(CONFIG_ZONE_DMA) && !(gfp & GFP_DMA))
			gfp = (gfp & ~GFP_DMA32) | GFP_DMA;
		else
			return NULL;
	}

	return page;
}

/*
 * Check if a potentially blocking operations needs to dip into the atomic
 * pools for the given device/gfp.
 */
/*
 * 判断本次分配是否必须改走预留的原子一致性内存池。
 *
 * @dev: 目标设备，用于排除已经采用 SWIOTLB 专用分配策略的情形。
 * @gfp: 原始分配标志，表达当前上下文是否允许阻塞。
 * 返回值: 不允许阻塞且不是 SWIOTLB-for-alloc 时为 true。
 *
 * 潜在阻塞的重映射、内存属性转换不能出现在原子上下文，因此公开分配路径会在真正取页前调用本
 * 判断并转向预先建立的池。SWIOTLB 专用分配自有原子上下文策略，不能再被重复分流。本函数本身
 * 只读取 gfp 和设备策略，不加锁、不睡眠。
 */
static bool dma_direct_use_pool(struct device *dev, gfp_t gfp)
{
	return !gfpflags_allow_blocking(gfp) && !is_swiotlb_for_alloc(dev);
}

/*
 * 从全局原子一致性内存池分配一块 dma-direct 缓冲区。
 *
 * @dev: 目标设备。
 * @size: 页对齐的所需字节数。
 * @dma_handle: 成功时写入设备可用的 DMA 首地址；失败时不保证更新。
 * @gfp: 分配约束，函数会补充最优 DMA zone 标志。
 * 返回值: 成功时为可立即由 CPU 访问的池内虚拟地址，失败为 NULL。
 *
 * 仅在 CONFIG_DMA_COHERENT_POOL 启用时合法。dma_alloc_from_pool() 用 dma_coherent_ok 回调筛掉
 * 设备不可寻址的候选页；成功后池继续拥有整体区域，而调用者临时拥有子块，必须由
 * dma_free_from_pool() 配对归还。池内部负责并发序列化，本层不持锁。
 */
static void *dma_direct_alloc_from_pool(struct device *dev, size_t size,
		dma_addr_t *dma_handle, gfp_t gfp)
{
	struct page *page;
	u64 phys_limit;
	void *ret;

	if (WARN_ON_ONCE(!IS_ENABLED(CONFIG_DMA_COHERENT_POOL)))
		return NULL;

	gfp |= dma_direct_optimal_gfp_mask(dev, &phys_limit);
	page = dma_alloc_from_pool(dev, size, &ret, gfp, dma_coherent_ok);
	if (!page)
		return NULL;
	*dma_handle = phys_to_dma_direct(dev, page_to_phys(page));
	return ret;
}

/*
 * 为 DMA_ATTR_NO_KERNEL_MAPPING 请求分配页面并只返回不透明 cookie。
 *
 * @dev: 目标设备。
 * @size: 页对齐分配大小。
 * @dma_handle: 成功时写入设备 DMA 地址。
 * @gfp: 分配标志；这里主动移除 __GFP_ZERO，因为没有 CPU 映射语义可供调用者使用。
 * 返回值: 强制转换为 void * 的 struct page 指针，仅可原样传给 dma_direct_free()；失败为 NULL。
 *
 * 允许高端页；对存在直接内核别名的低端页先清理脏缓存线，避免设备看到旧数据。此接口明确不把
 * cookie 当可解引用虚拟地址，也不执行内存加密属性切换；页面所有权由成功调用转给 DMA API 用户。
 * 是否可睡眠由 gfp 及底层连续页/SWIOTLB 分配器决定，本层不额外持锁。
 */
static void *dma_direct_alloc_no_mapping(struct device *dev, size_t size,
		dma_addr_t *dma_handle, gfp_t gfp)
{
	struct page *page;

	page = __dma_direct_alloc_pages(dev, size, gfp & ~__GFP_ZERO, true);
	if (!page)
		return NULL;

	/* remove any dirty cache lines on the kernel alias */
	/* 低端页存在永久内核别名，交给设备前清掉该别名遗留的脏缓存线；高端页没有这个直接别名。 */
	if (!PageHighMem(page))
		arch_dma_prep_coherent(page, size);

	/* return the page pointer as the opaque cookie */
	/* 返回页描述符作为不可解引用的身份 cookie，释放路径会按同一属性把它还原。 */
	*dma_handle = phys_to_dma_direct(dev, page_to_phys(page));
	return page;
}

/*
 * dma-direct 的主要一致性内存分配入口。
 *
 * @dev: 目标设备，其一致性、地址限制、加密要求和专用池共同决定路径。
 * @size: 请求字节数；函数向上按页对齐，成功对象至少覆盖该范围。
 * @dma_handle: 成功时写入设备使用的 DMA 地址。
 * @gfp: 分配上下文与回收约束；函数追加 __GFP_ZERO、__GFP_NOWARN 等内部策略。
 * @attrs: DMA_ATTR_NO_WARN、NO_KERNEL_MAPPING 等 DMA 属性集合。
 * 返回值: 可供 CPU 使用的连续虚拟地址或 NO_KERNEL_MAPPING cookie；失败为 NULL。
 *
 * 决策顺序是接口契约的一部分：无内核映射请求、非一致性体系结构专用实现/全局池、原子池，最后
 * 才是普通连续页。普通页可能需要创建新 vmapping、切换为解密映射或修改直接映射的缓存属性。
 * 所有高风险步骤都有反向清理；唯独解密/重新加密失败时刻意走 leak 标签，避免把属性不确定的页
 * 交回分配器。成功后 CPU 与设备可并发看到一致性缓冲区，调用者负责互斥数据访问并最终配对 free。
 */
void *dma_direct_alloc(struct device *dev, size_t size,
		dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs)
{
	bool remap = false, set_uncached = false;
	struct page *page;
	void *ret;

	size = PAGE_ALIGN(size);
	if (attrs & DMA_ATTR_NO_WARN)
		gfp |= __GFP_NOWARN;

	if ((attrs & DMA_ATTR_NO_KERNEL_MAPPING) &&
	    !force_dma_unencrypted(dev) && !is_swiotlb_for_alloc(dev))
		return dma_direct_alloc_no_mapping(dev, size, dma_handle, gfp);

	if (!dev_is_dma_coherent(dev)) {
		if (IS_ENABLED(CONFIG_ARCH_HAS_DMA_ALLOC) &&
		    !is_swiotlb_for_alloc(dev))
			return arch_dma_alloc(dev, size, dma_handle, gfp,
					      attrs);

		/*
		 * If there is a global pool, always allocate from it for
		 * non-coherent devices.
		 */
		/* 非一致性设备只要配置了全局一致性池，就优先且固定从该池分配。 */
		if (IS_ENABLED(CONFIG_DMA_GLOBAL_POOL))
			return dma_alloc_from_global_coherent(dev, size,
					dma_handle);

		/*
		 * Otherwise we require the architecture to either be able to
		 * mark arbitrary parts of the kernel direct mapping uncached,
		 * or remapped it uncached.
		 */
		/*
		 * 没有全局池时，体系结构必须能把任意直接映射区改成 uncached，或另建 uncached 映射；
		 * 两种能力都没有便无法兑现 dma_alloc_coherent() 对 CPU/设备一致可见性的承诺。
		 */
		set_uncached = IS_ENABLED(CONFIG_ARCH_HAS_DMA_SET_UNCACHED);
		remap = IS_ENABLED(CONFIG_DMA_DIRECT_REMAP);
		if (!set_uncached && !remap) {
			pr_warn_once("coherent DMA allocations not supported on this platform.\n");
			return NULL;
		}
	}

	/*
	 * Remapping or decrypting memory may block, allocate the memory from
	 * the atomic pools instead if we aren't allowed block.
	 */
	/* 重映射或解密可能阻塞；当前 gfp 禁止阻塞时，必须在触发这些操作前转向预留原子池。 */
	if ((remap || force_dma_unencrypted(dev)) &&
	    dma_direct_use_pool(dev, gfp))
		return dma_direct_alloc_from_pool(dev, size, dma_handle, gfp);

	/* we always manually zero the memory once we are done */
	/* 所有映射及属性转换成功后统一手工清零，因此底层取页刻意移除 __GFP_ZERO。 */
	page = __dma_direct_alloc_pages(dev, size, gfp & ~__GFP_ZERO, true);
	if (!page)
		return NULL;

	/*
	 * dma_alloc_contiguous can return highmem pages depending on a
	 * combination the cma= arguments and per-arch setup.  These need to be
	 * remapped to return a kernel virtual address.
	 */
	/*
	 * CMA 参数与体系结构设置可能让 dma_alloc_contiguous() 返回高端页；它没有永久内核虚拟地址，
	 * 因而必须另建映射，并且不能对不存在的直接映射调用 set_uncached。
	 */
	if (PageHighMem(page)) {
		remap = true;
		set_uncached = false;
	}

	if (remap) {
		pgprot_t prot = dma_pgprot(dev, PAGE_KERNEL, attrs);

		if (force_dma_unencrypted(dev))
			prot = pgprot_decrypted(prot);

		/* remove any dirty cache lines on the kernel alias */
		/* 新映射投入使用前，先清除底层页已有内核别名上的脏缓存线，避免别名一致性问题。 */
		arch_dma_prep_coherent(page, size);

		/* create a coherent mapping */
		/* 以设备所需页保护属性建立连续 CPU 映射；返回地址由释放路径用 vunmap() 撤销。 */
		ret = dma_common_contiguous_remap(page, size, prot,
				__builtin_return_address(0));
		if (!ret)
			goto out_free_pages;
	} else {
		ret = page_address(page);
		if (dma_set_decrypted(dev, ret, size))
			goto out_leak_pages;
	}

	memset(ret, 0, size);

	if (set_uncached) {
		arch_dma_prep_coherent(page, size);
		ret = arch_dma_set_uncached(ret, size);
		if (IS_ERR(ret))
			goto out_encrypt_pages;
	}

	*dma_handle = phys_to_dma_direct(dev, page_to_phys(page));
	return ret;

out_encrypt_pages:
	if (dma_set_encrypted(dev, page_address(page), size))
		return NULL;
out_free_pages:
	__dma_direct_free_pages(dev, page, size);
	return NULL;
out_leak_pages:
	return NULL;
}

/*
 * 释放 dma_direct_alloc() 返回的一致性缓冲区。
 *
 * @dev: 原分配所属设备。
 * @size: 原请求大小；各后端按需页对齐或计算阶数。
 * @cpu_addr: 分配返回的 CPU 地址，NO_KERNEL_MAPPING 时则是不透明 page cookie。
 * @dma_addr: 分配时写出的 DMA 地址，用于从普通路径找回底层页。
 * @attrs: 必须与分配时属性一致，尤其是 DMA_ATTR_NO_KERNEL_MAPPING。
 *
 * 函数严格按分配来源逆序识别：cookie、体系结构实现、全局池、原子池、vmapping/直接映射，最后
 * 释放底层连续页。直接映射先撤销 uncached 属性并恢复加密；恢复失败即保留整块页。调用返回后
 * 所有地址和 cookie 均失效。调用上下文必须满足对应后端的释放与页属性转换要求。
 */
void dma_direct_free(struct device *dev, size_t size,
		void *cpu_addr, dma_addr_t dma_addr, unsigned long attrs)
{
	unsigned int page_order = get_order(size);

	if ((attrs & DMA_ATTR_NO_KERNEL_MAPPING) &&
	    !force_dma_unencrypted(dev) && !is_swiotlb_for_alloc(dev)) {
		/* cpu_addr is a struct page cookie, not a kernel address */
		/* 此时 cpu_addr 是 struct page 身份 cookie，绝不能按内核虚拟地址解引用。 */
		dma_free_contiguous(dev, cpu_addr, size);
		return;
	}

	if (IS_ENABLED(CONFIG_ARCH_HAS_DMA_ALLOC) &&
	    !dev_is_dma_coherent(dev) &&
	    !is_swiotlb_for_alloc(dev)) {
		arch_dma_free(dev, size, cpu_addr, dma_addr, attrs);
		return;
	}

	if (IS_ENABLED(CONFIG_DMA_GLOBAL_POOL) &&
	    !dev_is_dma_coherent(dev)) {
		if (!dma_release_from_global_coherent(page_order, cpu_addr))
			WARN_ON_ONCE(1);
		return;
	}

	/* If cpu_addr is not from an atomic pool, dma_free_from_pool() fails */
	/* 原子池释放接口同时承担来源探测；地址不属于任何原子池时只返回 false，不会误释放。 */
	if (IS_ENABLED(CONFIG_DMA_COHERENT_POOL) &&
	    dma_free_from_pool(dev, cpu_addr, PAGE_ALIGN(size)))
		return;

	if (is_vmalloc_addr(cpu_addr)) {
		vunmap(cpu_addr);
	} else {
		if (IS_ENABLED(CONFIG_ARCH_HAS_DMA_CLEAR_UNCACHED))
			arch_dma_clear_uncached(cpu_addr, size);
		if (dma_set_encrypted(dev, cpu_addr, size))
			return;
	}

	__dma_direct_free_pages(dev, dma_direct_to_page(dev, dma_addr), size);
}

/*
 * 为以 struct page 表示结果的非一致性 DMA API 分配直接映射页。
 *
 * @dev: 目标设备。
 * @size: 请求字节数，调用链按连续页分配契约传入。
 * @dma_handle: 成功时写入设备 DMA 地址。
 * @dir: 数据方向；本层取页阶段不据此同步缓存，但调用者必须按同一方向管理后续同步与释放。
 * @gfp: 分配上下文与回收约束。
 * 返回值: 成功时为连续区域首页，失败为 NULL。
 *
 * 强制未加密且不可阻塞时委托预留池；普通路径禁止高端页，从而 page_address() 必须有效。取得页后
 * 切换解密属性、清零并发布 DMA 地址。解密失败时状态不确定，故不释放页面。调用者最终必须走
 * dma_direct_free_pages()，不能混用返回 CPU 虚拟地址的 dma_direct_free() 契约。
 * 可睡眠性由 gfp 决定；不允许阻塞的加密设备必须命中预留池，本层不持调用者锁。
 */
struct page *dma_direct_alloc_pages(struct device *dev, size_t size,
		dma_addr_t *dma_handle, enum dma_data_direction dir, gfp_t gfp)
{
	struct page *page;
	void *ret;

	if (force_dma_unencrypted(dev) && dma_direct_use_pool(dev, gfp))
		return dma_direct_alloc_from_pool(dev, size, dma_handle, gfp);

	page = __dma_direct_alloc_pages(dev, size, gfp, false);
	if (!page)
		return NULL;

	ret = page_address(page);
	if (dma_set_decrypted(dev, ret, size))
		goto out_leak_pages;
	memset(ret, 0, size);
	*dma_handle = phys_to_dma_direct(dev, page_to_phys(page));
	return page;
out_leak_pages:
	return NULL;
}

/*
 * 释放 dma_direct_alloc_pages() 取得的非一致性直接 DMA 页。
 *
 * @dev: 原分配所属设备。
 * @size: 原分配大小。
 * @page: 连续区域首页。
 * @dma_addr: 对应 DMA 地址；此后端已有 page，故参数仅用于保持通用回调签名。
 * @dir: 原数据方向；本层释放代码不读取它，但通用 API 要求与分配、同步阶段保持一致。
 *
 * 先用 page 的线性地址探测并归还原子池；普通页则必须先恢复加密属性，再按 SWIOTLB/CMA/伙伴来源
 * 释放。加密恢复失败时有意泄漏。调用者必须确保设备已停止访问，函数本身不替驱动做并发同步。
 * 释放及属性恢复的上下文限制由原分配后端决定，调用者不能在不允许该后端工作的上下文中释放。
 */
void dma_direct_free_pages(struct device *dev, size_t size,
		struct page *page, dma_addr_t dma_addr,
		enum dma_data_direction dir)
{
	void *vaddr = page_address(page);

	/* If cpu_addr is not from an atomic pool, dma_free_from_pool() fails */
	/* 非原子池地址只会探测失败；命中时池已收回该范围，不能继续恢复属性或再次释放。 */
	if (IS_ENABLED(CONFIG_DMA_COHERENT_POOL) &&
	    dma_free_from_pool(dev, vaddr, size))
		return;

	if (dma_set_encrypted(dev, vaddr, size))
		return;
	__dma_direct_free_pages(dev, page, size);
}

#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE) || \
    defined(CONFIG_SWIOTLB)
/*
 * 把一组 SG 缓冲区的最新 CPU 内容同步到设备可见侧。
 *
 * @dev: 拥有这些映射的设备。
 * @sgl: 已由 dma-direct 映射的 scatterlist 首项。
 * @nents: 要处理的映射项数。
 * @dir: DMA 数据方向，决定复制和缓存维护语义。
 *
 * 每项先把 DMA 地址还原成物理地址；若用了 SWIOTLB，先将原缓冲内容复制到 bounce 区，再在
 * 非一致性系统上对设备实际访问的地址执行体系结构缓存同步。所有项完成后统一 flush，可把昂贵的
 * 硬件提交操作批处理。调用者必须在把所有权交给设备前调用，并保证 SG 映射仍有效。
 * 该同步路径不睡眠；列表及其映射字段的并发互斥由调用者保证。
 */
void dma_direct_sync_sg_for_device(struct device *dev,
		struct scatterlist *sgl, int nents, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i) {
		phys_addr_t paddr = dma_to_phys(dev, sg_dma_address(sg));

		swiotlb_sync_single_for_device(dev, paddr, sg->length, dir);

		if (!dev_is_dma_coherent(dev))
			arch_sync_dma_for_device(paddr, sg->length,
					dir);
	}
	if (!dev_is_dma_coherent(dev))
		arch_sync_dma_flush();
}
#endif

#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL) || \
    defined(CONFIG_SWIOTLB)
/*
 * 把设备对一组 SG 缓冲区的写入同步回 CPU 可见侧。
 *
 * @dev: 拥有映射的设备。
 * @sgl: 仍处于已映射状态的 scatterlist。
 * @nents: 要同步的映射项数。
 * @dir: 原映射方向，决定哪些缓存线/字节需要回传。
 *
 * 顺序与 for_device 相反：非一致性系统先失效或同步设备实际访问的物理/bounce 缓存，再由 SWIOTLB
 * 把 bounce 内容复制回原页。循环后批量提交体系结构缓存操作，并在需要时执行全 CPU 同步。调用者
 * 应先确保设备 DMA 已完成；函数负责可见性，不负责等待硬件或保护 CPU 并发访问。
 * 该同步路径不睡眠；调用者须串行化 SG 字段和缓冲区 ownership 的变更。
 */
void dma_direct_sync_sg_for_cpu(struct device *dev,
		struct scatterlist *sgl, int nents, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i) {
		phys_addr_t paddr = dma_to_phys(dev, sg_dma_address(sg));

		if (!dev_is_dma_coherent(dev))
			arch_sync_dma_for_cpu(paddr, sg->length, dir);

		swiotlb_sync_single_for_cpu(dev, paddr, sg->length, dir);
	}

	if (!dev_is_dma_coherent(dev)) {
		arch_sync_dma_flush();
		arch_sync_dma_for_cpu_all();
	}
}

/*
 * Unmaps segments, except for ones marked as pci_p2pdma which do not
 * require any further action as they contain a bus address.
 */
/*
 * 撤销一组直接 SG 映射；标记为 pci_p2pdma 的项只保存总线地址，无需普通物理映射清理。
 *
 * @dev: 原映射目标设备。
 * @sgl: 已映射 scatterlist。
 * @nents: 原输入项数，而不是可能合并后的段数。
 * @dir: 原 DMA 方向。
 * @attrs: 原映射属性；失败回滚可额外带 DMA_ATTR_SKIP_CPU_SYNC。
 *
 * 总线地址项只清除身份标记；普通项逐个调用 dma_direct_unmap_phys()，并把末级体系结构 flush
 * 延后到整个列表结束。函数可能把设备写入同步给 CPU，调用者须先停止 DMA，返回后所有 sg DMA
 * 字段不再代表有效映射。
 * 路径不睡眠，SG 字段及设备完成状态的互斥由调用者负责。
 */
void dma_direct_unmap_sg(struct device *dev, struct scatterlist *sgl,
		int nents, enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *sg;
	int i;
	bool need_sync = false;

	for_each_sg(sgl,  sg, nents, i) {
		if (sg_dma_is_bus_address(sg)) {
			sg_dma_unmark_bus_address(sg);
		} else {
			need_sync = true;
			dma_direct_unmap_phys(dev, sg->dma_address,
					      sg_dma_len(sg), dir, attrs, false);
		}
	}
	if (need_sync && !dev_is_dma_coherent(dev))
		arch_sync_dma_flush();
}
#endif

/*
 * 把 scatterlist 映射成设备可使用的直接 DMA 地址列表。
 *
 * @dev: 目标设备，也用于判断每项 P2P 页面与其 PCI 拓扑关系。
 * @sgl: 输入页片段；成功时逐项写入 dma_address 与 dma_length。
 * @nents: 输入项数。
 * @dir: 数据传输方向。
 * @attrs: 映射属性。
 * 返回值: 成功返回 nents；普通映射失败返回 -EIO，P2P 拓扑不兼容返回 -EREMOTEIO。
 *
 * 每项可能是普通 RAM、必须穿过 host bridge 的 P2P 页，或可直接使用 PCI bus address 的 P2P 页。
 * 前两类走 dma_direct_map_phys()，后一类标记为 bus address 以便 unmap 特判。普通项缓存同步的末级
 * flush 在列表后批处理。任一项失败会撤销此前 i 项，并跳过 CPU 同步，因为映射尚未交给设备。
 * 映射路径不睡眠；调用者在成功或失败返回前都不得并发改写该 scatterlist。
 */
int dma_direct_map_sg(struct device *dev, struct scatterlist *sgl, int nents,
		enum dma_data_direction dir, unsigned long attrs)
{
	struct pci_p2pdma_map_state p2pdma_state = {};
	struct scatterlist *sg;
	int i, ret;
	bool need_sync = false;

	for_each_sg(sgl, sg, nents, i) {
		switch (pci_p2pdma_state(&p2pdma_state, dev, sg_page(sg))) {
		case PCI_P2PDMA_MAP_THRU_HOST_BRIDGE:
			/*
			 * Any P2P mapping that traverses the PCI host bridge
			 * must be mapped with CPU physical address and not PCI
			 * bus addresses.
			 */
			/* 穿越 PCI host bridge 的 P2P 访问必须按 CPU 物理地址映射，不能直接使用 PCI 总线地址。 */
			fallthrough;
		case PCI_P2PDMA_MAP_NONE:
			need_sync = true;
			sg->dma_address = dma_direct_map_phys(dev, sg_phys(sg),
					sg->length, dir, attrs, false);
			if (sg->dma_address == DMA_MAPPING_ERROR) {
				ret = -EIO;
				goto out_unmap;
			}
			break;
		case PCI_P2PDMA_MAP_BUS_ADDR:
			sg->dma_address = pci_p2pdma_bus_addr_map(
				p2pdma_state.mem, sg_phys(sg));
			sg_dma_len(sg) = sg->length;
			sg_dma_mark_bus_address(sg);
			continue;
		default:
			ret = -EREMOTEIO;
			goto out_unmap;
		}
		sg_dma_len(sg) = sg->length;
	}

	if (need_sync && !dev_is_dma_coherent(dev))
		arch_sync_dma_flush();
	return nents;

out_unmap:
	dma_direct_unmap_sg(dev, sgl, i, dir, attrs | DMA_ATTR_SKIP_CPU_SYNC);
	return ret;
}

/*
 * 用一个直接 DMA 一致性缓冲区构造单项 sg_table 描述。
 *
 * @dev: 缓冲区所属设备。
 * @sgt: 输出表；成功后调用者负责 sg_free_table()。
 * @cpu_addr: CPU 地址；直接后端无需使用，保留以匹配通用接口。
 * @dma_addr: 缓冲区 DMA 首地址，用于反查首页。
 * @size: 缓冲区字节数，SG 长度向上按页对齐。
 * @attrs: 分配属性；本后端无需据此改变单项描述。
 * 返回值: sg_alloc_table() 的 0 或负错误码。
 *
 * 该转换不复制数据、不增加底层 DMA 分配寿命；sg_table 只能在原缓冲区释放前使用。
 * sg_alloc_table(GFP_KERNEL) 可能睡眠，因而只能在允许阻塞的进程上下文调用。
 */
int dma_direct_get_sgtable(struct device *dev, struct sg_table *sgt,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		unsigned long attrs)
{
	struct page *page = dma_direct_to_page(dev, dma_addr);
	int ret;

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (!ret)
		sg_set_page(sgt->sgl, page, PAGE_ALIGN(size), 0);
	return ret;
}

/*
 * 查询 dma-direct 分配结果是否具备用户态 mmap 实现。
 *
 * @dev: 目标设备。
 * 返回值: 硬件一致性设备总是支持；非一致性设备仅在体系结构启用 DMA_NONCOHERENT_MMAP 时支持。
 *
 * 这只是能力判断，不建立 VMA、不验证具体分配来源；上层应在调用 dma_direct_mmap() 前使用它筛选。
 * 它只读设备属性和编译期配置，不加锁、不睡眠。
 */
bool dma_direct_can_mmap(struct device *dev)
{
	return dev_is_dma_coherent(dev) ||
		IS_ENABLED(CONFIG_DMA_NONCOHERENT_MMAP);
}

/*
 * 把 dma-direct 一致性分配的一段页映射到用户 VMA。
 *
 * @dev: 原分配所属设备。
 * @vma: 用户请求区间；vm_pgoff 表示相对缓冲区的页偏移。
 * @cpu_addr: 内核 CPU 地址，供设备专用池或全局池识别来源。
 * @dma_addr: DMA 首地址，普通页路径据此反算物理 PFN。
 * @size: 原缓冲区大小。
 * @attrs: 原分配属性，用于生成相同缓存属性的页保护。
 * 返回值: 成功为 0；来源池或 remap_pfn_range() 的错误原样返回，越界返回 -ENXIO。
 *
 * 先设置与内核 DMA 映射一致的缓存/解密属性，再依次让设备池和全局池认领；普通路径严格检查
 * VMA 页偏移与长度后映射 PFN。VMA 只借用页，调用者必须保证 DMA 缓冲区至少存活到所有映射关闭，
 * 并自行协调用户、内核和设备的并发访问。
 * 调用发生在 mmap 进程上下文并遵循 mm/VMA 锁协议，页表建立可能睡眠。
 */
int dma_direct_mmap(struct device *dev, struct vm_area_struct *vma,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		unsigned long attrs)
{
	unsigned long user_count = vma_pages(vma);
	unsigned long count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	unsigned long pfn = PHYS_PFN(dma_to_phys(dev, dma_addr));
	int ret = -ENXIO;

	vma->vm_page_prot = dma_pgprot(dev, vma->vm_page_prot, attrs);
	if (force_dma_unencrypted(dev))
		vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);

	if (dma_mmap_from_dev_coherent(dev, vma, cpu_addr, size, &ret))
		return ret;
	if (dma_mmap_from_global_coherent(vma, cpu_addr, size, &ret))
		return ret;

	if (vma->vm_pgoff >= count || user_count > count - vma->vm_pgoff)
		return -ENXIO;
	return remap_pfn_range(vma, vma->vm_start, pfn + vma->vm_pgoff,
			user_count << PAGE_SHIFT, vma->vm_page_prot);
}

/*
 * 判断给定 DMA 掩码是否足以让 dma-direct 覆盖体系结构要求的低端内存。
 *
 * @dev: 待配置设备；未加密地址换算可能包含设备范围偏移。
 * @mask: 驱动/硬件声称支持的 DMA 地址掩码。
 * 返回值: 直接映射后端能够满足时为 1，否则为 0。
 *
 * 32 位及以上掩码依赖体系结构的全局约束直接视为可用；更窄掩码则以系统最高内存或 ZONE_DMA
 * 上限中较低者为最低覆盖要求，并排除 SME 加密位后比较。函数只验证能力，不写 dev->dma_mask。
 * 它不加锁、不睡眠，要求设备地址范围配置在查询期间保持稳定。
 */
int dma_direct_supported(struct device *dev, u64 mask)
{
	u64 min_mask = ((u64)max_pfn << PAGE_SHIFT) - 1;

	/*
	 * Because 32-bit DMA masks are so common we expect every architecture
	 * to be able to satisfy them - either by not supporting more physical
	 * memory, or by providing a ZONE_DMA32.  If neither is the case, the
	 * architecture needs to use an IOMMU instead of the direct mapping.
	 */
	/*
	 * 32 位 DMA 掩码极为常见，体系结构必须通过物理内存不越过 4 GiB 或提供 ZONE_DMA32 来满足；
	 * 两者皆无时应使用 IOMMU，不能让 dma-direct 假装支持。
	 */
	if (mask >= DMA_BIT_MASK(32))
		return 1;

	/*
	 * This check needs to be against the actual bit mask value, so use
	 * phys_to_dma_unencrypted() here so that the SME encryption mask isn't
	 * part of the check.
	 */
	/* 比较必须针对真实硬件地址位，因此使用未加密换算，避免把 SME 加密选择位误算进 DMA 掩码。 */
	if (IS_ENABLED(CONFIG_ZONE_DMA))
		min_mask = min_t(u64, min_mask, zone_dma_limit);
	return mask >= phys_to_dma_unencrypted(dev, min_mask);
}

/*
 * 在设备的零结尾 dma_range_map 中查找覆盖指定 CPU PFN 的范围。
 *
 * @dev: 已安装 dma_range_map 的设备。
 * @start_pfn: 要定位的 CPU 物理页号。
 * 返回值: 覆盖该页的只读 bus_dma_region 指针；无覆盖时为 NULL。
 *
 * size 向下取整为 0 的哨兵终止遍历；差值形式的边界判断避免直接计算范围末端。函数不加锁，要求
 * 固件解析/初始化阶段已经发布稳定映射，且调用期间设备范围数组仍存活。
 */
static const struct bus_dma_region *dma_find_range(struct device *dev,
						   unsigned long start_pfn)
{
	const struct bus_dma_region *m;

	for (m = dev->dma_range_map; PFN_DOWN(m->size); m++) {
		unsigned long cpu_start_pfn = PFN_DOWN(m->cpu_start);

		if (start_pfn >= cpu_start_pfn &&
		    start_pfn - cpu_start_pfn < PFN_DOWN(m->size))
			return m;
	}

	return NULL;
}

/*
 * To check whether all ram resource ranges are covered by dma range map
 * Returns 0 when further check is needed
 * Returns 1 if there is some RAM range can't be covered by dma_range_map
 */
/*
 * 检查一段系统 RAM 是否能由若干相邻的 dma_range_map 项连续覆盖。
 *
 * @start_pfn: 当前 RAM 资源段的起始 PFN。
 * @nr_pages: 资源段页数。
 * @data: walk_system_ram_range() 传入的 struct device 指针。
 * 返回值: 完整覆盖返回 0，让遍历继续；发现任何空洞立即返回 1，使遍历提前终止。
 *
 * 每次查到覆盖当前 PFN 的范围后跳到该范围末端，故能跨多个范围推进而不会逐页扫描。映射条目
 * 必须具有正的整页跨度，否则无法保证前进；它只读取启动期稳定数据，不改变 RAM 资源或设备映射。
 * 回调本身不睡眠、不加设备锁，资源遍历框架负责调用时的资源树访问协议。
 */
static int check_ram_in_range_map(unsigned long start_pfn,
				  unsigned long nr_pages, void *data)
{
	unsigned long end_pfn = start_pfn + nr_pages;
	struct device *dev = data;

	while (start_pfn < end_pfn) {
		const struct bus_dma_region *bdr;

		bdr = dma_find_range(dev, start_pfn);
		if (!bdr)
			return 1;

		start_pfn = PFN_DOWN(bdr->cpu_start) + PFN_DOWN(bdr->size);
	}

	return 0;
}

/*
 * 判断设备的 DMA 范围映射是否覆盖全部系统 RAM。
 *
 * @dev: 待检查设备。
 * 返回值: 没有范围限制或所有 RAM 资源段均被覆盖时为 true；发现空洞为 false。
 *
 * 无 dma_range_map 表示恒等/无限制语义，可直接成功；否则 walk_system_ram_range() 遍历整个 PFN
 * 空间，并把回调的“发现空洞”非零结果取反为布尔答案。该检查可能遍历全局资源树，不属于映射热路。
 * 本层不持设备锁；调用上下文须允许资源遍历器执行其内部同步。
 */
bool dma_direct_all_ram_mapped(struct device *dev)
{
	if (!dev->dma_range_map)
		return true;
	return !walk_system_ram_range(0, PFN_DOWN(ULONG_MAX) + 1, dev,
				      check_ram_in_range_map);
}

/*
 * 查询 dma-direct 单次流式映射允许的最大长度。
 *
 * @dev: 目标设备。
 * 返回值: 受限/强制 bounce 且 SWIOTLB 活跃时返回其可容纳上限，否则返回 SIZE_MAX。
 *
 * 上层据此拆分过大的请求，避免到 map 阶段才因 bounce slot 约束失败；它是瞬时能力快照，不预留池空间。
 * 查询不睡眠、不转移池 ownership；SWIOTLB 状态由其自身同步规则保护。
 */
size_t dma_direct_max_mapping_size(struct device *dev)
{
	/* If SWIOTLB is active, use its maximum mapping size */
	/* SWIOTLB 实际参与受限或强制 bounce 时，单次映射上限必须服从其连续槽位能力。 */
	if (is_swiotlb_active(dev) &&
	    (dma_addressing_limited(dev) || is_swiotlb_force_bounce(dev)))
		return swiotlb_max_mapping_size(dev);
	return SIZE_MAX;
}

/*
 * 判断某个 dma-direct 映射是否需要显式 CPU/设备同步。
 *
 * @dev: 映射所属设备。
 * @dma_addr: 正在查询的 DMA 地址。
 * 返回值: 非一致性设备，或该地址落在 SWIOTLB 池中时为 true；其余为 false。
 *
 * 即使设备硬件一致，bounce 缓冲区与原缓冲区之间仍需复制，因此不能只看 dev_is_dma_coherent()。
 * 本函数只查询映射性质，不同步缓存也不改变所有权，要求地址映射仍有效。
 * 查询不加锁、不睡眠；SWIOTLB 池必须在映射生命周期内保持可查找。
 */
bool dma_direct_need_sync(struct device *dev, dma_addr_t dma_addr)
{
	return !dev_is_dma_coherent(dev) ||
	       swiotlb_find_pool(dev, dma_to_phys(dev, dma_addr));
}

/**
 * dma_direct_set_offset - Assign scalar offset for a single DMA range.
 * @dev:	device pointer; needed to "own" the alloced memory.
 * @cpu_start:  beginning of memory region covered by this offset.
 * @dma_start:  beginning of DMA/PCI region covered by this offset.
 * @size:	size of the region.
 *
 * This is for the simple case of a uniform offset which cannot
 * be discovered by "dma-ranges".
 *
 * It returns -ENOMEM if out of memory, -EINVAL if a map
 * already exists, 0 otherwise.
 *
 * Note: any call to this from a driver is a bug.  The mapping needs
 * to be described by the device tree or other firmware interfaces.
 */
/*
 * 为仅含一个固定偏移的设备安装 dma_range_map。
 *
 * @dev: 拥有新映射及其分配内存的设备。
 * @cpu_start: 映射覆盖的 CPU 物理起点。
 * @dma_start: 同一区域在设备/PCI 地址域中的起点。
 * @size: 区域字节长度。
 * 返回值: 成功或无需偏移为 0；已有映射返回 -EINVAL，分配失败返回 -ENOMEM。
 *
 * 这是固件无法通过 dma-ranges 表达时的简单统一偏移辅助接口；驱动直接调用属于固件描述缺陷。
 * 零偏移无需创建数组。非零时分配两个清零条目，第二个以 size=0 充当遍历哨兵，完整填写首项后才
 * 发布到 dev->dma_range_map。设备核心负责其后续生命周期；调用者须在设备投入 DMA 前串行设置一次。
 * kzalloc_objs() 使用 GFP_KERNEL，因而该初始化接口可能睡眠，不能在原子上下文调用。
 */
int dma_direct_set_offset(struct device *dev, phys_addr_t cpu_start,
			 dma_addr_t dma_start, u64 size)
{
	struct bus_dma_region *map;
	u64 offset = (u64)cpu_start - (u64)dma_start;

	if (dev->dma_range_map) {
		dev_err(dev, "attempt to add DMA range to existing map\n");
		return -EINVAL;
	}

	if (!offset)
		return 0;

	map = kzalloc_objs(*map, 2);
	if (!map)
		return -ENOMEM;
	map[0].cpu_start = cpu_start;
	map[0].dma_start = dma_start;
	map[0].size = size;
	dev->dma_range_map = map;
	return 0;
}
