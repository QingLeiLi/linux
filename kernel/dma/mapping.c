// SPDX-License-Identifier: GPL-2.0
/*
 * arch-independent dma-mapping routines
 *
 * Copyright (c) 2006  SUSE Linux Products GmbH
 * Copyright (c) 2006  Tejun Heo <teheo@suse.de>
 */
/*
 * 本文件提供体系结构无关的 DMA mapping 公共入口：验证统一契约、选择 direct/IOMMU/dma_map_ops
 * 后端，并在真实操作周围接入内存检测、trace、debug 及设备资源管理。
 */
#include <linux/memblock.h> /* for max_pfn */
/* memblock 头提供 max_pfn，后续 required-mask 与寻址限制查询以系统物理内存上界为依据。 */
#include <linux/acpi.h>
#include <linux/dma-map-ops.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/iommu-dma.h>
#include <linux/kmsan.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "debug.h"
#include "direct.h"

#define CREATE_TRACE_POINTS
/* 本编译单元实例化 DMA tracepoint；其他文件只通过声明发出事件，避免重复定义。 */
#include <trace/events/dma.h>

#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE) || \
	defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU) || \
	defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL)
/*
 * 体系结构没有为每个设备给出明确属性时使用的默认一致性值；初始化后作为全局只读策略被
 * dev_is_dma_coherent() 路径查询。它不代表任意设备一定一致，设备级配置仍可覆盖。
 */
bool dma_default_coherent = IS_ENABLED(CONFIG_ARCH_DMA_DEFAULT_COHERENT);
#endif

/*
 * Managed DMA API
 */
/* 下列对象和回调把 coherent DMA 分配接入 devres，使设备解绑时能够自动按原属性释放。 */
/*
 * 一笔 managed DMA 分配的释放凭据。
 *
 * @size: 原请求字节数。
 * @vaddr: dma_alloc_attrs() 返回的 CPU 视图或不透明 cookie。
 * @dma_handle: 同次分配写出的设备地址。
 * @attrs: 原 DMA_ATTR_*，释放时必须原样复用。
 *
 * devres 节点本体包裹该记录；成功发布后由设备资源栈拥有，显式 dmam_free_coherent() 可先移除，
 * 否则解绑按 LIFO 调用 dmam_release()。记录不单独拥有 dev 引用，其寿命天然受所属设备约束。
 */
struct dma_devres {
	size_t		size;
	void		*vaddr;
	dma_addr_t	dma_handle;
	unsigned long	attrs;
};

/*
 * devres 回收一笔 managed DMA 分配的 release 回调。
 *
 * @dev: 拥有资源记录和 DMA 缓冲区的设备。
 * @res: 指向 struct dma_devres 的 devres 数据区。
 *
 * 按记录的 size/vaddr/handle/attrs 调用通用 dma_free_attrs()，后者识别原分配后端。回调不返回错误，
 * 且可能因页属性恢复或后端释放而睡眠，只在设备资源展开的可用上下文执行。
 */
static void dmam_release(struct device *dev, void *res)
{
	struct dma_devres *this = res;

	dma_free_attrs(dev, this->size, this->vaddr, this->dma_handle,
			this->attrs);
}

/*
 * 在设备 devres 栈中匹配要显式释放的 managed DMA 记录。
 *
 * @dev: 所属设备；匹配逻辑无需读取它。
 * @res: 候选 struct dma_devres。
 * @match_data: 由 dmam_free_coherent() 在栈上构造的期望记录。
 * 返回值: CPU 地址相同为 1，否则为 0；地址命中但 size/handle 不同会告警。
 *
 * vaddr 是资源身份主键，属性不参与 coherent 显式释放匹配。devres core 在其同步保护下调用本函数；
 * 本层不加锁、不睡眠，也不转移候选记录 ownership。
 */
static int dmam_match(struct device *dev, void *res, void *match_data)
{
	struct dma_devres *this = res, *match = match_data;

	if (this->vaddr == match->vaddr) {
		WARN_ON(this->size != match->size ||
			this->dma_handle != match->dma_handle);
		return 1;
	}
	return 0;
}

/**
 * dmam_free_coherent - Managed dma_free_coherent()
 * @dev: Device to free coherent memory for
 * @size: Size of allocation
 * @vaddr: Virtual address of the memory to free
 * @dma_handle: DMA handle of the memory to free
 *
 * Managed dma_free_coherent().
 */
/*
 * 显式提前释放 dmam_alloc_coherent()（即 attrs=0 的 dmam_alloc_attrs()）管理的 coherent 缓冲区。
 *
 * @dev: 原 managed 分配所属设备。
 * @size: 原分配大小。
 * @vaddr: 原 CPU 地址。
 * @dma_handle: 原设备地址。
 *
 * 先用临时匹配记录从 devres 栈销毁自动释放节点，避免设备解绑二次释放，再调用
 * dma_free_coherent() 真正归还内存。找不到记录会 WARN，但仍按调用参数执行释放；调用者必须保证
 * 四元组真实且只释放一次。底层 coherent free 可能睡眠，不能在 IRQ 上下文调用。
 */
void dmam_free_coherent(struct device *dev, size_t size, void *vaddr,
			dma_addr_t dma_handle)
{
	struct dma_devres match_data = { size, vaddr, dma_handle };

	WARN_ON(devres_destroy(dev, dmam_release, dmam_match, &match_data));
	dma_free_coherent(dev, size, vaddr, dma_handle);
}
EXPORT_SYMBOL(dmam_free_coherent);

/**
 * dmam_alloc_attrs - Managed dma_alloc_attrs()
 * @dev: Device to allocate non_coherent memory for
 * @size: Size of allocation
 * @dma_handle: Out argument for allocated DMA handle
 * @gfp: Allocation flags
 * @attrs: Flags in the DMA_ATTR_* namespace.
 *
 * Managed dma_alloc_attrs().  Memory allocated using this function will be
 * automatically released on driver detach.
 *
 * RETURNS:
 * Pointer to allocated memory on success, NULL on failure.
 */
/*
 * 分配 coherent DMA 缓冲区并把自动释放凭据挂入设备 devres。
 *
 * @dev: 目标设备及 devres owner。
 * @size: 所需字节数。
 * @dma_handle: 成功时输出设备地址。
 * @gfp: devres 记录和底层缓冲区共同采用的分配约束。
 * @attrs: 原样传给 alloc/free 的 DMA 属性。
 * 返回值: 成功返回 CPU 地址/属性规定的 cookie，失败为 NULL。
 *
 * 先分配未发布的 devres，再执行真正 DMA 分配；后者失败只释放记录。成功后完整填写四元组并最后
 * devres_add() 发布，设备解绑即可自动调用 dmam_release()。函数可按 gfp 睡眠；发布前没有其他线程
 * 能看到半初始化记录，显式释放必须使用同一设备和分配结果。
 */
void *dmam_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,
		gfp_t gfp, unsigned long attrs)
{
	struct dma_devres *dr;
	void *vaddr;

	dr = devres_alloc(dmam_release, sizeof(*dr), gfp);
	if (!dr)
		return NULL;

	vaddr = dma_alloc_attrs(dev, size, dma_handle, gfp, attrs);
	if (!vaddr) {
		devres_free(dr);
		return NULL;
	}

	dr->vaddr = vaddr;
	dr->dma_handle = *dma_handle;
	dr->size = size;
	dr->attrs = attrs;

	devres_add(dev, dr);

	return vaddr;
}
EXPORT_SYMBOL(dmam_alloc_attrs);

/*
 * 判断一次 DMA 操作能否绕过已安装 ops，直接使用 dma-direct。
 *
 * @dev: 目标设备。
 * @mask: 本次操作适用的地址掩码；coherent 分配和 streaming map 由两个包装器分别传入。
 * @ops: 当前设备 DMA 操作表，可为 NULL。
 * 返回值: 应走 direct 后端为 true，否则为 false。
 *
 * dma-iommu 设备绝不在这里直通；无 ops 表示原生 direct。存在 ops 时只有编译启用 bypass、设备已
 * 标记 bypass，且 mask 与 bus limit 足以覆盖 direct required mask 才可绕过。纯策略查询不加锁、
 * 不睡眠，要求 DMA 配置在设备初始化后保持稳定。
 */
static bool dma_go_direct(struct device *dev, dma_addr_t mask,
		const struct dma_map_ops *ops)
{
	if (use_dma_iommu(dev))
		return false;

	if (likely(!ops))
		return true;

	if (IS_ENABLED(CONFIG_DMA_OPS_BYPASS) && dev_dma_ops_bypass(dev))
		return min_not_zero(mask, dev->bus_dma_limit) >=
			    dma_direct_get_required_mask(dev);
	return false;
}


/*
 * Check if the devices uses a direct mapping for streaming DMA operations.
 * This allows IOMMU drivers to set a bypass mode if the DMA mask is large
 * enough.
 */
/*
 * 判断 coherent DMA 分配能否使用 direct 后端。
 *
 * @dev: 目标设备。
 * @ops: 当前 DMA 操作表。
 * 返回值: dma_go_direct() 以 coherent_dma_mask 判定的结果。
 *
 * 原英文说明所称 streaming 适用于下面的 map 包装器；本函数专门服务 alloc 路径。它只读配置，
 * 不创建映射、不加锁、不睡眠。
 */
static inline bool dma_alloc_direct(struct device *dev,
		const struct dma_map_ops *ops)
{
	return dma_go_direct(dev, dev->coherent_dma_mask, ops);
}

/*
 * 判断 streaming DMA map/unmap 是否可使用 direct 后端。
 *
 * @dev: 目标设备，要求 dev->dma_mask 已由上层验证存在。
 * @ops: 当前 DMA 操作表。
 * 返回值: 以 streaming `*dev->dma_mask` 调用 dma_go_direct() 的结果。
 *
 * 与 dma_alloc_direct() 分离可避免把 coherent 与 streaming 两个掩码混用；函数不加锁、不睡眠。
 */
static inline bool dma_map_direct(struct device *dev,
		const struct dma_map_ops *ops)
{
	return dma_go_direct(dev, *dev->dma_mask, ops);
}

/*
 * 把一段 CPU 物理地址映射为 streaming DMA 地址并统一接入观测钩子。
 *
 * @dev: 发起 DMA 的设备。
 * @phys: 区间首字节 CPU 物理地址，可表示 RAM 或带 DMA_ATTR_MMIO 的资源。
 * @size: 映射字节数。
 * @dir: DMA 方向，非法值触发 BUG。
 * @attrs: REQUIRE_COHERENT、MMIO、CC_SHARED 等映射属性。
 * 返回值: 成功为设备 DMA 地址，失败为 DMA_MAPPING_ERROR。
 *
 * 先验证 mask 与一致性要求，再按 direct/体系结构局部直通、CC shared 限制、dma-iommu、ops->map_phys
 * 的优先级分派。普通内存随后通知 KMSAN，并无论成败发 trace/debug；MMIO 不作为内存初始化数据
 * 处理。成功会按 dir 把区间 ownership 交给设备域，必须用相同参数 dma_unmap_phys()。热路径不睡眠，
 * 后端负责自身并发与 IOVA/缓存同步。
 */
dma_addr_t dma_map_phys(struct device *dev, phys_addr_t phys, size_t size,
		enum dma_data_direction dir, unsigned long attrs)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);
	bool is_mmio = attrs & DMA_ATTR_MMIO;
	bool is_cc_shared = attrs & DMA_ATTR_CC_SHARED;
	dma_addr_t addr = DMA_MAPPING_ERROR;

	BUG_ON(!valid_dma_direction(dir));

	if (WARN_ON_ONCE(!dev->dma_mask))
		return DMA_MAPPING_ERROR;

	if (!dev_is_dma_coherent(dev) && (attrs & DMA_ATTR_REQUIRE_COHERENT))
		return DMA_MAPPING_ERROR;

	if (dma_map_direct(dev, ops) ||
	    (!is_mmio && !is_cc_shared &&
	     arch_dma_map_phys_direct(dev, phys + size)))
		addr = dma_direct_map_phys(dev, phys, size, dir, attrs, true);
	else if (is_cc_shared)
		return DMA_MAPPING_ERROR;
	else if (use_dma_iommu(dev))
		addr = iommu_dma_map_phys(dev, phys, size, dir, attrs);
	else if (ops->map_phys)
		addr = ops->map_phys(dev, phys, size, dir, attrs);

	if (!is_mmio)
		kmsan_handle_dma(phys, size, dir);
	trace_dma_map_phys(dev, phys, addr, size, dir, attrs);
	debug_dma_map_phys(dev, phys, size, dir, addr, attrs);

	return addr;
}
EXPORT_SYMBOL_GPL(dma_map_phys);

/*
 * 把 struct page 内的一段普通内存映射为 streaming DMA 地址。
 *
 * @dev: 目标设备。
 * @page: 区间所在首页。
 * @offset: 相对 page 首字节偏移。
 * @size: 映射长度，调用者须保证区间有效。
 * @dir: DMA 方向。
 * @attrs: streaming 属性；MMIO 在 page API 中不合法。
 * 返回值: dma_map_phys() 的 DMA 地址或 DMA_MAPPING_ERROR。
 *
 * page+offset 只负责生成物理起点；DMA API debug 开启时拒绝 ZONE_DEVICE 页，避免用普通 page
 * streaming 契约处理设备内存。成功后的 ownership、同步和 unmap 责任完全继承 dma_map_phys()；
 * 函数不增加页引用且不睡眠，页必须由调用者保持存活。
 */
dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page,
		size_t offset, size_t size, enum dma_data_direction dir,
		unsigned long attrs)
{
	phys_addr_t phys = page_to_phys(page) + offset;

	if (unlikely(attrs & DMA_ATTR_MMIO))
		return DMA_MAPPING_ERROR;

	if (IS_ENABLED(CONFIG_DMA_API_DEBUG) &&
	    WARN_ON_ONCE(is_zone_device_page(page)))
		return DMA_MAPPING_ERROR;

	return dma_map_phys(dev, phys, size, dir, attrs);
}
EXPORT_SYMBOL(dma_map_page_attrs);

/*
 * 撤销 dma_map_phys() 建立的 streaming 物理映射。
 *
 * @dev: 原映射设备。
 * @addr: 原成功返回的 DMA 地址。
 * @size: 原映射长度。
 * @dir: 原 DMA 方向，非法值触发 BUG。
 * @attrs: 必须保留 MMIO/CC_SHARED 等原属性。
 *
 * 按与 map 对称的 direct/体系结构直通、CC shared、dma-iommu、ops 分派归还 IOVA/bounce/cache
 * ownership，随后发 trace/debug。CC_SHARED 非 direct 路径在 map 时已被拒绝，unmap 因而无操作返回。
 * 调用者须先停止设备访问且只撤销一次；streaming unmap 热路径不睡眠。
 */
void dma_unmap_phys(struct device *dev, dma_addr_t addr, size_t size,
		enum dma_data_direction dir, unsigned long attrs)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);
	bool is_mmio = attrs & DMA_ATTR_MMIO;
	bool is_cc_shared = attrs & DMA_ATTR_CC_SHARED;

	BUG_ON(!valid_dma_direction(dir));

	if (dma_map_direct(dev, ops) ||
	    (!is_mmio && !is_cc_shared &&
	     arch_dma_unmap_phys_direct(dev, addr + size)))
		dma_direct_unmap_phys(dev, addr, size, dir, attrs, true);
	else if (is_cc_shared)
		return;
	else if (use_dma_iommu(dev))
		iommu_dma_unmap_phys(dev, addr, size, dir, attrs);
	else if (ops->unmap_phys)
		ops->unmap_phys(dev, addr, size, dir, attrs);
	trace_dma_unmap_phys(dev, addr, size, dir, attrs);
	debug_dma_unmap_phys(dev, addr, size, dir, attrs);
}
EXPORT_SYMBOL_GPL(dma_unmap_phys);

/*
 * 撤销 dma_map_page_attrs() 返回的普通页 streaming 映射。
 *
 * @dev: 原设备。
 * @addr: 原 DMA 地址。
 * @size: 原长度。
 * @dir: 原方向。
 * @attrs: 原属性；若错误地带 MMIO 则直接忽略，与 page map 的拒绝行为对称。
 *
 * 实际后端分派交给 dma_unmap_phys()；函数不持有 page 指针或引用，调用者负责页与设备完成同步。
 * 与底层 streaming unmap 相同，本函数不睡眠。
 */
void dma_unmap_page_attrs(struct device *dev, dma_addr_t addr, size_t size,
		 enum dma_data_direction dir, unsigned long attrs)
{
	if (unlikely(attrs & DMA_ATTR_MMIO))
		return;

	dma_unmap_phys(dev, addr, size, dir, attrs);
}
EXPORT_SYMBOL(dma_unmap_page_attrs);

/*
 * SG 映射的内部错误保真入口。
 *
 * @dev: 目标设备。
 * @sg: 原输入 scatterlist。
 * @nents: 原输入项数。
 * @dir: DMA 方向，非法值触发 BUG。
 * @attrs: streaming 属性。
 * 返回值: 成功为正的映射段数；失败保留 -EINVAL/-ENOMEM/-EIO/-EREMOTEIO。
 *
 * 非一致性设备无法满足 REQUIRE_COHERENT 时立即失败；随后按 direct、dma-iommu 或 ops->map_sg
 * 分派。成功才通知 KMSAN、trace 和 DMA debug。后端若返回白名单外负值会 WARN 并归一化为 -EIO。
 * 成功后设备拥有缓冲区，unmap 必须使用原 sg 与原 nents；函数不睡眠，SG 生命周期由调用者保证。
 */
static int __dma_map_sg_attrs(struct device *dev, struct scatterlist *sg,
	 int nents, enum dma_data_direction dir, unsigned long attrs)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);
	int ents;

	BUG_ON(!valid_dma_direction(dir));

	if (!dev_is_dma_coherent(dev) && (attrs & DMA_ATTR_REQUIRE_COHERENT))
		return -EOPNOTSUPP;

	if (WARN_ON_ONCE(!dev->dma_mask))
		return 0;

	if (dma_map_direct(dev, ops) ||
	    arch_dma_map_sg_direct(dev, sg, nents))
		ents = dma_direct_map_sg(dev, sg, nents, dir, attrs);
	else if (use_dma_iommu(dev))
		ents = iommu_dma_map_sg(dev, sg, nents, dir, attrs);
	else
		ents = ops->map_sg(dev, sg, nents, dir, attrs);

	if (ents > 0) {
		kmsan_handle_dma_sg(sg, nents, dir);
		trace_dma_map_sg(dev, sg, nents, ents, dir, attrs);
		debug_dma_map_sg(dev, sg, nents, ents, dir, attrs);
	} else if (WARN_ON_ONCE(ents != -EINVAL && ents != -ENOMEM &&
				ents != -EIO && ents != -EREMOTEIO)) {
		trace_dma_map_sg_err(dev, sg, nents, ents, dir, attrs);
		return -EIO;
	}

	return ents;
}

/**
 * dma_map_sg_attrs - Map the given buffer for DMA
 * @dev:	The device for which to perform the DMA operation
 * @sg:		The sg_table object describing the buffer
 * @nents:	Number of entries to map
 * @dir:	DMA direction
 * @attrs:	Optional DMA attributes for the map operation
 *
 * Maps a buffer described by a scatterlist passed in the sg argument with
 * nents segments for the @dir DMA operation by the @dev device.
 *
 * Returns the number of mapped entries (which can be less than nents)
 * on success. Zero is returned for any error.
 *
 * dma_unmap_sg_attrs() should be used to unmap the buffer with the
 * original sg and original nents (not the value returned by this funciton).
 */
/*
 * 面向传统调用者映射 scatterlist，并把所有错误折叠为 0。
 *
 * @dev: 目标 DMA 设备。
 * @sg: 描述缓冲区的原始列表。
 * @nents: 原始表项数。
 * @dir: DMA 方向。
 * @attrs: 可选 DMA 属性。
 * 返回值: 成功为可供设备遍历的映射段数，可能小于 nents；任意失败为 0。
 *
 * 内部入口保留的负错误在此为兼容旧 DMA API 统一转成 0。成功把缓冲区 ownership 交给设备；
 * dma_unmap_sg_attrs() 必须接收原 sg 和原 nents，绝不能把返回段数当作 unmap 的 nents。函数不睡眠，
 * 调用者负责 SG 与底层页在映射期存活。
 */
unsigned int dma_map_sg_attrs(struct device *dev, struct scatterlist *sg,
		    int nents, enum dma_data_direction dir, unsigned long attrs)
{
	int ret;

	ret = __dma_map_sg_attrs(dev, sg, nents, dir, attrs);
	if (ret < 0)
		return 0;
	return ret;
}
EXPORT_SYMBOL(dma_map_sg_attrs);

/**
 * dma_map_sgtable - Map the given buffer for DMA
 * @dev:	The device for which to perform the DMA operation
 * @sgt:	The sg_table object describing the buffer
 * @dir:	DMA direction
 * @attrs:	Optional DMA attributes for the map operation
 *
 * Maps a buffer described by a scatterlist stored in the given sg_table
 * object for the @dir DMA operation by the @dev device. After success, the
 * ownership for the buffer is transferred to the DMA domain.  One has to
 * call dma_sync_sgtable_for_cpu() or dma_unmap_sgtable() to move the
 * ownership of the buffer back to the CPU domain before touching the
 * buffer by the CPU.
 *
 * Returns 0 on success or a negative error code on error. The following
 * error codes are supported with the given meaning:
 *
 *   -EINVAL		An invalid argument, unaligned access or other error
 *			in usage. Will not succeed if retried.
 *   -ENOMEM		Insufficient resources (like memory or IOVA space) to
 *			complete the mapping. Should succeed if retried later.
 *   -EIO		Legacy error code with an unknown meaning. eg. this is
 *			returned if a lower level call returned
 *			DMA_MAPPING_ERROR.
 *   -EREMOTEIO		The DMA device cannot access P2PDMA memory specified
 *			in the sg_table. This will not succeed if retried.
 */
/*
 * 映射 sg_table 并保留可诊断的负错误码。
 *
 * @dev: 目标设备。
 * @sgt: 输入表；orig_nents 是原项数，成功时更新 nents 为实际 DMA 段数。
 * @dir: DMA 方向。
 * @attrs: 可选映射属性。
 * 返回值: 成功为 0；-EINVAL 表示契约错误，-ENOMEM 表示资源不足可稍后重试，-EIO 表示传统后端
 * 错误，-EREMOTEIO 表示设备拓扑无法访问 P2P 内存。
 *
 * 成功后缓冲区属于 DMA 域，CPU 触碰前必须 sync_sgtable_for_cpu() 或 unmap_sgtable()；失败时
 * sgt->nents 不发布新值。与传统接口不同，这里不丢失后端负错误。函数不睡眠，调用者保持表和页存活。
 */
int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
		    enum dma_data_direction dir, unsigned long attrs)
{
	int nents;

	nents = __dma_map_sg_attrs(dev, sgt->sgl, sgt->orig_nents, dir, attrs);
	if (nents < 0)
		return nents;
	sgt->nents = nents;
	return 0;
}
EXPORT_SYMBOL_GPL(dma_map_sgtable);

/*
 * 撤销传统 dma_map_sg_attrs() 建立的 SG 映射。
 *
 * @dev: 原映射设备。
 * @sg: 原始 scatterlist。
 * @nents: 原输入项数，而非 map 返回的合并段数。
 * @dir: 原 DMA 方向。
 * @attrs: 原映射属性。
 *
 * 在后端清理前先记录 trace/debug，然后按 direct、dma-iommu、ops 对称分派；成功 map 已保证存在
 * 对应能力。unmap 把 ownership 交回 CPU 并使 DMA 字段失效，调用者必须先停止设备且只调用一次。
 * streaming 路径不睡眠，列表并发由调用者串行化。
 */
void dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sg,
				      int nents, enum dma_data_direction dir,
				      unsigned long attrs)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	BUG_ON(!valid_dma_direction(dir));
	trace_dma_unmap_sg(dev, sg, nents, dir, attrs);
	debug_dma_unmap_sg(dev, sg, nents, dir, attrs);
	if (dma_map_direct(dev, ops) ||
	    arch_dma_unmap_sg_direct(dev, sg, nents))
		dma_direct_unmap_sg(dev, sg, nents, dir, attrs);
	else if (use_dma_iommu(dev))
		iommu_dma_unmap_sg(dev, sg, nents, dir, attrs);
	else if (ops->unmap_sg)
		ops->unmap_sg(dev, sg, nents, dir, attrs);
}
EXPORT_SYMBOL(dma_unmap_sg_attrs);

/*
 * 把 MMIO/总线资源物理区间映射给设备。
 *
 * @dev: 目标设备。
 * @phys_addr: 资源物理起点，不是普通 RAM page 地址。
 * @size: 资源长度。
 * @dir: DMA 方向。
 * @attrs: 额外属性。
 * 返回值: dma_map_phys() 以 DMA_ATTR_MMIO 分派后的 DMA 地址或错误哨兵。
 *
 * MMIO 标记避免 KMSAN 把资源当内存数据，并限制 direct/体系结构分派。成功后必须用
 * dma_unmap_resource() 配对；函数不睡眠，资源自身生命周期由调用者维持。
 */
dma_addr_t dma_map_resource(struct device *dev, phys_addr_t phys_addr,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	return dma_map_phys(dev, phys_addr, size, dir, attrs | DMA_ATTR_MMIO);
}
EXPORT_SYMBOL(dma_map_resource);

/*
 * 撤销 dma_map_resource() 的 MMIO DMA 映射。
 *
 * @dev: 原设备。
 * @addr: 原 DMA 地址。
 * @size: 原资源长度。
 * @dir: 原方向。
 * @attrs: 原附加属性，函数补回 DMA_ATTR_MMIO。
 *
 * 所有实际清理由 dma_unmap_phys() 对称完成；调用者须先停止设备访问，函数不睡眠。
 */
void dma_unmap_resource(struct device *dev, dma_addr_t addr, size_t size,
		enum dma_data_direction dir, unsigned long attrs)
{
	dma_unmap_phys(dev, addr, size, dir, attrs | DMA_ATTR_MMIO);
}
EXPORT_SYMBOL(dma_unmap_resource);

#ifdef CONFIG_DMA_NEED_SYNC
/*
 * 把单段 streaming 映射的 ownership/内容同步给 CPU。
 *
 * @dev: 映射所属设备。
 * @addr: 有效 DMA 地址。
 * @size: 同步长度。
 * @dir: 原 DMA 方向。
 *
 * 按 direct、dma-iommu 或 ops 回调执行缓存/bounce 同步，随后发 trace/debug。调用者须先确认设备
 * 已完成相关访问；同步后 CPU 可按方向触碰缓冲区。函数不睡眠，也不撤销 DMA 地址。
 */
void __dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr, size_t size,
		enum dma_data_direction dir)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	BUG_ON(!valid_dma_direction(dir));
	if (dma_map_direct(dev, ops))
		dma_direct_sync_single_for_cpu(dev, addr, size, dir, true);
	else if (use_dma_iommu(dev))
		iommu_dma_sync_single_for_cpu(dev, addr, size, dir);
	else if (ops->sync_single_for_cpu)
		ops->sync_single_for_cpu(dev, addr, size, dir);
	trace_dma_sync_single_for_cpu(dev, addr, size, dir);
	debug_dma_sync_single_for_cpu(dev, addr, size, dir);
}
EXPORT_SYMBOL(__dma_sync_single_for_cpu);

/*
 * 把单段 streaming 缓冲区的最新 CPU 内容同步给设备。
 *
 * @dev: 映射所属设备。
 * @addr: 有效 DMA 地址。
 * @size: 同步长度。
 * @dir: 原 DMA 方向。
 *
 * 按当前后端执行缓存维护或 bounce copy，再记录 trace/debug；调用者随后才能启动设备访问。函数
 * 不撤销映射、不睡眠，CPU/设备 ownership 切换的并发顺序由驱动保证。
 */
void __dma_sync_single_for_device(struct device *dev, dma_addr_t addr,
		size_t size, enum dma_data_direction dir)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	BUG_ON(!valid_dma_direction(dir));
	if (dma_map_direct(dev, ops))
		dma_direct_sync_single_for_device(dev, addr, size, dir);
	else if (use_dma_iommu(dev))
		iommu_dma_sync_single_for_device(dev, addr, size, dir);
	else if (ops->sync_single_for_device)
		ops->sync_single_for_device(dev, addr, size, dir);
	trace_dma_sync_single_for_device(dev, addr, size, dir);
	debug_dma_sync_single_for_device(dev, addr, size, dir);
}
EXPORT_SYMBOL(__dma_sync_single_for_device);

/*
 * 把已映射 SG 列表同步回 CPU 域。
 *
 * @dev: 映射所属设备。
 * @sg: 原 scatterlist。
 * @nelems: DMA API 要求的原输入项数。
 * @dir: 原方向。
 *
 * 后端按 direct/IOMMU/ops 执行逐项缓存或 bounce 同步，随后 trace/debug。调用者先等待设备完成，
 * 返回后 CPU 可访问相应方向的数据；映射仍有效。函数不睡眠，列表不得被并发改写。
 */
void __dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg,
		    int nelems, enum dma_data_direction dir)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	BUG_ON(!valid_dma_direction(dir));
	if (dma_map_direct(dev, ops))
		dma_direct_sync_sg_for_cpu(dev, sg, nelems, dir);
	else if (use_dma_iommu(dev))
		iommu_dma_sync_sg_for_cpu(dev, sg, nelems, dir);
	else if (ops->sync_sg_for_cpu)
		ops->sync_sg_for_cpu(dev, sg, nelems, dir);
	trace_dma_sync_sg_for_cpu(dev, sg, nelems, dir);
	debug_dma_sync_sg_for_cpu(dev, sg, nelems, dir);
}
EXPORT_SYMBOL(__dma_sync_sg_for_cpu);

/*
 * 把已映射 SG 列表的 CPU 更新同步给设备域。
 *
 * @dev: 映射所属设备。
 * @sg: 原 scatterlist。
 * @nelems: 原输入项数。
 * @dir: 原方向。
 *
 * 后端完成缓存清理/bounce copy 后才发观测事件；返回后驱动方可启动 DMA。函数不撤销映射且不睡眠，
 * ownership 与 SG 并发由调用者管理。
 */
void __dma_sync_sg_for_device(struct device *dev, struct scatterlist *sg,
		       int nelems, enum dma_data_direction dir)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	BUG_ON(!valid_dma_direction(dir));
	if (dma_map_direct(dev, ops))
		dma_direct_sync_sg_for_device(dev, sg, nelems, dir);
	else if (use_dma_iommu(dev))
		iommu_dma_sync_sg_for_device(dev, sg, nelems, dir);
	else if (ops->sync_sg_for_device)
		ops->sync_sg_for_device(dev, sg, nelems, dir);
	trace_dma_sync_sg_for_device(dev, sg, nelems, dir);
	debug_dma_sync_sg_for_device(dev, sg, nelems, dir);
}
EXPORT_SYMBOL(__dma_sync_sg_for_device);

/*
 * 对一个具体 DMA 地址做细粒度“是否需要同步”查询。
 *
 * @dev: 映射所属设备。
 * @dma_addr: 当前有效映射地址。
 * 返回值: direct 后端由 dma_direct_need_sync() 区分一致性与 SWIOTLB；其他后端保守返回 true。
 *
 * 设备级 dma_skip_sync 可能因首次 bounce 映射被清除，但并非此后所有地址都来自 bounce 池，因此
 * direct 情形必须查具体地址。函数不做同步、不睡眠，地址须仍在映射期。
 */
bool __dma_need_sync(struct device *dev, dma_addr_t dma_addr)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (dma_map_direct(dev, ops))
		/*
		 * dma_skip_sync could've been reset on first SWIOTLB buffer
		 * mapping, but @dma_addr is not necessary an SWIOTLB buffer.
		 * In this case, fall back to more granular check.
		 */
		/*
		 * 首个 SWIOTLB 映射可能已清除设备级 skip 标志，但当前地址未必也是 bounce；因此退回到
		 * dma-direct 的逐地址判断，避免对普通一致性地址执行多余同步。
		 */
		return dma_direct_need_sync(dev, dma_addr);
	return true;
}
EXPORT_SYMBOL_GPL(__dma_need_sync);

/**
 * dma_need_unmap - does this device need dma_unmap_* operations
 * @dev: device to check
 *
 * If this function returns %false, drivers can skip calling dma_unmap_* after
 * finishing an I/O.  This function must be called after all mappings that might
 * need to be unmapped have been performed.
 */
/*
 * 查询驱动在 I/O 完成后能否整体省略 dma_unmap_*。
 *
 * @dev: 已完成所有可能改变同步需求之映射的设备。
 * 返回值: 非 direct、不能跳过同步或启用 DMA API debug 时为 true；纯 direct 且可跳过时为 false。
 *
 * 必须在可能触发 SWIOTLB 的映射之后查询，因为首个 bounce 会清除 skip 状态。debug 模式即使硬件
 * 无需清理也要求 unmap，以维持配对追踪。纯查询不加锁、不睡眠。
 */
bool dma_need_unmap(struct device *dev)
{
	if (!dma_map_direct(dev, get_dma_ops(dev)))
		return true;
	if (!dev_dma_skip_sync(dev))
		return true;
	return IS_ENABLED(CONFIG_DMA_API_DEBUG);
}
EXPORT_SYMBOL_GPL(dma_need_unmap);

/*
 * 根据设备后端与一致性能力初始化 dma_skip_sync 快速路径状态。
 *
 * @dev: DMA 配置已基本建立的设备。
 *
 * direct/IOMMU 初值取设备一致性，后续 SWIOTLB 映射可把它清除；自定义 ops 若完全没有 sync 回调，
 * 表示同步不可能执行，设置 skip；否则清除。状态写入应发生在 mask/设备初始化串行阶段，函数不睡眠。
 */
static void dma_setup_need_sync(struct device *dev)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (dma_map_direct(dev, ops) || use_dma_iommu(dev))
		/*
		 * dma_skip_sync will be reset to %false on first SWIOTLB buffer
		 * mapping, if any. During the device initialization, it's
		 * enough to check only for the DMA coherence.
		 */
		/* 初始化时只需按硬件一致性设置；若后来首次使用 SWIOTLB，映射路径会把 skip 重置为 false。 */
		dev_assign_dma_skip_sync(dev, dev_is_dma_coherent(dev));
	else if (!ops->sync_single_for_device && !ops->sync_single_for_cpu &&
		 !ops->sync_sg_for_device && !ops->sync_sg_for_cpu)
		/*
		 * Synchronization is not possible when none of DMA sync ops
		 * is set.
		 */
		/* 四类 sync 回调全缺失时后端根本无法同步，故把设备标成可跳过以避免无效分派。 */
		dev_set_dma_skip_sync(dev);
	else
		dev_clear_dma_skip_sync(dev);
}
#else /* !CONFIG_DMA_NEED_SYNC */
/*
 * 未编译动态同步需求跟踪时的设置占位。
 *
 * @dev: 保留以维持两种配置下相同调用签名，函数不读取它。
 *
 * 所有调用点仍可无条件编译，但这里不写设备状态、不加锁、不睡眠。
 */
static inline void dma_setup_need_sync(struct device *dev) { }
#endif /* !CONFIG_DMA_NEED_SYNC */

/*
 * The whole dma_get_sgtable() idea is fundamentally unsafe - it seems
 * that the intention is to allow exporting memory allocated via the
 * coherent DMA APIs through the dma_buf API, which only accepts a
 * scattertable.  This presents a couple of problems:
 * 1. Not all memory allocated via the coherent DMA APIs is backed by
 *    a struct page
 * 2. Passing coherent DMA memory into the streaming APIs is not allowed
 *    as we will try to flush the memory through a different alias to that
 *    actually being used (and the flushes are redundant.)
 */
/*
 * 把 coherent 分配导出成 scatterlist 的思路本质上不安全：其目的通常是适配只接受 SG 的 dma-buf，
 * 但部分 coherent 内存根本没有 struct page；即使有，也不能再交给 streaming API，因为缓存维护会
 * 通过与实际 CPU 视图不同的别名刷新，不仅重复，还可能破坏别名一致性。
 */
/*
 * 请求后端为一笔 coherent DMA 分配构造 sg_table 描述。
 *
 * @dev: 原分配设备。
 * @sgt: 输出表，成功后由调用者释放表结构。
 * @cpu_addr: 原 CPU 视图/cookie。
 * @dma_addr: 原设备地址。
 * @size: 原分配大小。
 * @attrs: 原分配属性。
 * 返回值: 后端结果；不支持 get_sgtable 时为 -ENXIO。
 *
 * 依 direct、dma-iommu、ops 分派。成功表只借用底层分配，不延长其寿命；调用者还必须遵守上面的
 * page/alias 限制，不能据此把 coherent 内存当普通 streaming 缓冲。表分配可能使用 GFP_KERNEL，
 * 因而调用上下文应允许睡眠。
 */
int dma_get_sgtable_attrs(struct device *dev, struct sg_table *sgt,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		unsigned long attrs)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (dma_alloc_direct(dev, ops))
		return dma_direct_get_sgtable(dev, sgt, cpu_addr, dma_addr,
				size, attrs);
	if (use_dma_iommu(dev))
		return iommu_dma_get_sgtable(dev, sgt, cpu_addr, dma_addr,
				size, attrs);
	if (!ops->get_sgtable)
		return -ENXIO;
	return ops->get_sgtable(dev, sgt, cpu_addr, dma_addr, size, attrs);
}
EXPORT_SYMBOL(dma_get_sgtable_attrs);

#ifdef CONFIG_MMU
/*
 * Return the page attributes used for mapping dma_alloc_* memory, either in
 * kernel space if remapping is needed, or to userspace through dma_mmap_*.
 */
/*
 * 计算 dma_alloc_* 内存用于内核重映射或用户 mmap 的页保护属性。
 *
 * @dev: 分配所属设备。
 * @prot: 调用者提供的基础页保护。
 * @attrs: DMA_ATTR_WRITE_COMBINE 等分配属性。
 * 返回值: 一致性设备原样返回；非一致性设备优先使用请求的 write-combine，否则用 dma-coherent 属性。
 *
 * 只组合 pgprot 位，不建立页表、不加锁、不睡眠；返回值必须与原 DMA 分配的 CPU 可见属性一致，
 * 以免同一物理页产生冲突别名。
 */
pgprot_t dma_pgprot(struct device *dev, pgprot_t prot, unsigned long attrs)
{
	if (dev_is_dma_coherent(dev))
		return prot;
#ifdef CONFIG_ARCH_HAS_DMA_WRITE_COMBINE
	if (attrs & DMA_ATTR_WRITE_COMBINE)
		return pgprot_writecombine(prot);
#endif
	return pgprot_dmacoherent(prot);
}
#endif /* CONFIG_MMU */

/**
 * dma_can_mmap - check if a given device supports dma_mmap_*
 * @dev: device to check
 *
 * Returns %true if @dev supports dma_mmap_coherent() and dma_mmap_attrs() to
 * map DMA allocations to userspace.
 */
/*
 * 查询设备是否支持把 coherent DMA 分配 mmap 到用户态。
 *
 * @dev: 待查询设备。
 * 返回值: direct 后端按体系结构能力判断，dma-iommu 固定支持，自定义后端要求 mmap 回调非空。
 *
 * 纯能力查询，不验证具体分配来源或 VMA 范围，不加锁、不睡眠；调用方仍须使用同一后端创建和映射。
 */
bool dma_can_mmap(struct device *dev)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (dma_alloc_direct(dev, ops))
		return dma_direct_can_mmap(dev);
	if (use_dma_iommu(dev))
		return true;
	return ops->mmap != NULL;
}
EXPORT_SYMBOL_GPL(dma_can_mmap);

/**
 * dma_mmap_attrs - map a coherent DMA allocation into user space
 * @dev: valid struct device pointer, or NULL for ISA and EISA-like devices
 * @vma: vm_area_struct describing requested user mapping
 * @cpu_addr: kernel CPU-view address returned from dma_alloc_attrs
 * @dma_addr: device-view address returned from dma_alloc_attrs
 * @size: size of memory originally requested in dma_alloc_attrs
 * @attrs: attributes of mapping properties requested in dma_alloc_attrs
 *
 * Map a coherent DMA buffer previously allocated by dma_alloc_attrs into user
 * space.  The coherent DMA buffer must not be freed by the driver until the
 * user space mapping has been released.
 */
/*
 * 把 dma_alloc_attrs() 返回的 coherent 缓冲区映射进用户 VMA。
 *
 * @dev: 原分配设备。
 * @vma: 用户请求区间及页偏移。
 * @cpu_addr: 原 CPU 视图/cookie。
 * @dma_addr: 原设备地址。
 * @size: 原请求大小。
 * @attrs: 原分配属性。
 * 返回值: direct、dma-iommu 或 ops->mmap 的结果；后端无能力时为 -ENXIO。
 *
 * 映射只借用底层页，驱动必须等所有用户 VMA 释放后才能 dma_free_attrs()。函数在 mmap 进程上下文
 * 遵循 mm 锁协议并可能睡眠；用户、内核和设备并发访问仍由驱动定义。
 */
int dma_mmap_attrs(struct device *dev, struct vm_area_struct *vma,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		unsigned long attrs)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (dma_alloc_direct(dev, ops))
		return dma_direct_mmap(dev, vma, cpu_addr, dma_addr, size,
				attrs);
	if (use_dma_iommu(dev))
		return iommu_dma_mmap(dev, vma, cpu_addr, dma_addr, size,
				      attrs);
	if (!ops->mmap)
		return -ENXIO;
	return ops->mmap(dev, vma, cpu_addr, dma_addr, size, attrs);
}
EXPORT_SYMBOL(dma_mmap_attrs);

/*
 * 查询设备覆盖系统内存所建议的最小 DMA 掩码。
 *
 * @dev: 目标设备。
 * 返回值: direct 的精确 required mask、dma-iommu 的 32 位默认、ops 回调结果，或通用 32 位保底。
 *
 * 该值是能力建议而非写入操作；调用者仍须用 dma_set_mask()/dma_set_coherent_mask() 验证硬件。
 * 查询不分配资源、不睡眠，设备后端配置须保持稳定。
 */
u64 dma_get_required_mask(struct device *dev)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (dma_alloc_direct(dev, ops))
		return dma_direct_get_required_mask(dev);

	if (use_dma_iommu(dev))
		return DMA_BIT_MASK(32);

	if (ops->get_required_mask)
		return ops->get_required_mask(dev);

	/*
	 * We require every DMA ops implementation to at least support a 32-bit
	 * DMA mask (and use bounce buffering if that isn't supported in
	 * hardware).  As the direct mapping code has its own routine to
	 * actually report an optimal mask we default to 32-bit here as that
	 * is the right thing for most IOMMUs, and at least not actively
	 * harmful in general.
	 */
	/*
	 * 通用约定要求每个 DMA ops 至少借助硬件或 bounce 支持 32 位掩码。direct 有精确算法；多数
	 * IOMMU 也以 32 位为合理默认，所以缺少后端回调时返回 32 位既实用又不会主动放宽地址范围。
	 */
	return DMA_BIT_MASK(32);
}
EXPORT_SYMBOL_GPL(dma_get_required_mask);

/*
 * 通用 coherent DMA 分配入口。
 *
 * @dev: 目标设备，必须已设置 coherent_dma_mask。
 * @size: 请求字节数。
 * @dma_handle: 成功时输出设备地址。
 * @flag: 分配上下文；不允许 __GFP_COMP，zone/highmem 位由实现层自行选择。
 * @attrs: DMA_ATTR_* 分配属性。
 * 返回值: 成功为 CPU 视图或属性规定的 cookie，失败为 NULL。
 *
 * 先尝试设备专用 coherent pool，再按 direct/体系结构、dma-iommu、ops->alloc 分派；所有普通后端
 * 完成后统一 trace/debug。成功对象同时具有 CPU 和 DMA 两个视图，调用者负责数据并发并用完全相同
 * 的 size/handle/attrs 释放。可睡眠性由 flag 与后端决定；原子请求由支持的预留池路径承接。
 */
void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,
		gfp_t flag, unsigned long attrs)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);
	void *cpu_addr;

	WARN_ON_ONCE(!dev->coherent_dma_mask);

	/*
	 * DMA allocations can never be turned back into a page pointer, so
	 * requesting compound pages doesn't make sense (and can't even be
	 * supported at all by various backends).
	 */
	/*
	 * coherent API 的返回值不保证能还原为 struct page，许多后端也无法提供 compound page；
	 * 因此 __GFP_COMP 没有可兑现的语义，直接拒绝。
	 */
	if (WARN_ON_ONCE(flag & __GFP_COMP))
		return NULL;

	if (dma_alloc_from_dev_coherent(dev, size, dma_handle, &cpu_addr)) {
		trace_dma_alloc(dev, cpu_addr, *dma_handle, size,
				DMA_BIDIRECTIONAL, flag, attrs);
		return cpu_addr;
	}

	/* let the implementation decide on the zone to allocate from: */
	/* 清除调用者指定的 DMA/DMA32/HIGHMEM 区域位，让实际后端依据设备地址能力选择正确 zone。 */
	flag &= ~(__GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM);

	if (dma_alloc_direct(dev, ops) || arch_dma_alloc_direct(dev)) {
		cpu_addr = dma_direct_alloc(dev, size, dma_handle, flag, attrs);
	} else if (use_dma_iommu(dev)) {
		cpu_addr = iommu_dma_alloc(dev, size, dma_handle, flag, attrs);
	} else if (ops->alloc) {
		cpu_addr = ops->alloc(dev, size, dma_handle, flag, attrs);
	} else {
		trace_dma_alloc(dev, NULL, 0, size, DMA_BIDIRECTIONAL, flag,
				attrs);
		return NULL;
	}

	trace_dma_alloc(dev, cpu_addr, *dma_handle, size, DMA_BIDIRECTIONAL,
			flag, attrs);
	debug_dma_alloc_coherent(dev, size, *dma_handle, cpu_addr, attrs);
	return cpu_addr;
}
EXPORT_SYMBOL(dma_alloc_attrs);

/*
 * 释放 dma_alloc_attrs() 返回的 coherent DMA 缓冲区。
 *
 * @dev: 原分配设备。
 * @size: 原请求大小。
 * @cpu_addr: 原 CPU 视图/cookie，可为 NULL。
 * @dma_handle: 原设备地址。
 * @attrs: 原分配属性。
 *
 * 先让设备专用 coherent pool 认领；否则警告 IRQ 上下文误用，记录 trace/debug，再按 direct/体系结构、
 * dma-iommu、ops->free 对称释放。非一致性 coherent 实现可能 vunmap 或修改页属性而睡眠。调用者必须
 * 先停止设备和用户映射，且只释放一次；返回后两个地址都失效。
 */
void dma_free_attrs(struct device *dev, size_t size, void *cpu_addr,
		dma_addr_t dma_handle, unsigned long attrs)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (dma_release_from_dev_coherent(dev, get_order(size), cpu_addr))
		return;
	/*
	 * On non-coherent platforms which implement DMA-coherent buffers via
	 * non-cacheable remaps, ops->free() may call vunmap(). Thus getting
	 * this far in IRQ context is a) at risk of a BUG_ON() or trying to
	 * sleep on some machines, and b) an indication that the driver is
	 * probably misusing the coherent API anyway.
	 */
	/*
	 * 非一致性平台常以 non-cacheable remap 实现 coherent 缓冲，free 可能调用 vunmap、触发 BUG_ON
	 * 或睡眠；走到这里仍处于 IRQ 上下文本身就表明驱动误用了 coherent API，因此发出警告。
	 */
	WARN_ON(irqs_disabled());

	trace_dma_free(dev, cpu_addr, dma_handle, size, DMA_BIDIRECTIONAL,
		       attrs);
	if (!cpu_addr)
		return;

	debug_dma_free_coherent(dev, size, cpu_addr, dma_handle, attrs);
	if (dma_alloc_direct(dev, ops) || arch_dma_free_direct(dev, dma_handle))
		dma_direct_free(dev, size, cpu_addr, dma_handle, attrs);
	else if (use_dma_iommu(dev))
		iommu_dma_free(dev, size, cpu_addr, dma_handle, attrs);
	else if (ops->free)
		ops->free(dev, size, cpu_addr, dma_handle, attrs);
}
EXPORT_SYMBOL(dma_free_attrs);

/*
 * 非一致性 page DMA 分配的内部后端分派。
 *
 * @dev: 目标设备，必须具有 coherent_dma_mask。
 * @size: 请求字节数，函数按页对齐。
 * @dma_handle: 成功时输出设备地址。
 * @dir: 后续 DMA ownership/sync 使用的方向。
 * @gfp: 分配约束；调用者不得指定 DMA/DMA32/HIGHMEM 或 __GFP_COMP。
 * 返回值: direct/common-IOMMU/ops 后端返回的连续首页，失败为 NULL。
 *
 * 本层只验证统一契约并选择后端，不发 trace/debug，供公开 page API 和 single-SGT fallback 复用。
 * 可睡眠性由 gfp 决定；成功页必须由 __dma_free_pages() 走相同后端释放。
 */
static struct page *__dma_alloc_pages(struct device *dev, size_t size,
		dma_addr_t *dma_handle, enum dma_data_direction dir, gfp_t gfp)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (WARN_ON_ONCE(!dev->coherent_dma_mask))
		return NULL;
	if (WARN_ON_ONCE(gfp & (__GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM)))
		return NULL;
	if (WARN_ON_ONCE(gfp & __GFP_COMP))
		return NULL;

	size = PAGE_ALIGN(size);
	if (dma_alloc_direct(dev, ops))
		return dma_direct_alloc_pages(dev, size, dma_handle, dir, gfp);
	if (use_dma_iommu(dev))
		return dma_common_alloc_pages(dev, size, dma_handle, dir, gfp);
	if (!ops->alloc_pages_op)
		return NULL;
	return ops->alloc_pages_op(dev, size, dma_handle, dir, gfp);
}

/*
 * 分配以 struct page 表示的非一致性 DMA 缓冲区。
 *
 * @dev: 目标设备。
 * @size: 请求大小。
 * @dma_handle: 成功时输出设备地址。
 * @dir: DMA 方向。
 * @gfp: 分配上下文。
 * 返回值: 成功为连续首页，失败为 NULL。
 *
 * 内部成功后统一发 trace 与 DMA debug，失败也记录空结果。返回 page 可由 page_address() 访问，因为
 * 内部拒绝 HIGHMEM；调用者须按方向执行所需 sync，并最终用 dma_free_pages() 配对。函数可按 gfp 睡眠。
 */
struct page *dma_alloc_pages(struct device *dev, size_t size,
		dma_addr_t *dma_handle, enum dma_data_direction dir, gfp_t gfp)
{
	struct page *page = __dma_alloc_pages(dev, size, dma_handle, dir, gfp);

	if (page) {
		trace_dma_alloc_pages(dev, page_to_virt(page), *dma_handle,
				      size, dir, gfp, 0);
		debug_dma_alloc_pages(dev, page, size, dir, *dma_handle);
	} else {
		trace_dma_alloc_pages(dev, NULL, 0, size, dir, gfp, 0);
	}
	return page;
}
EXPORT_SYMBOL_GPL(dma_alloc_pages);

/*
 * 非一致性 page DMA 分配的内部释放分派。
 *
 * @dev: 原设备。
 * @size: 原大小，函数按页对齐。
 * @page: 原连续首页。
 * @dma_handle: 原设备地址。
 * @dir: 原方向。
 *
 * 按 direct、dma-iommu common 或 ops->free_pages 与分配来源配对；本层不发观测事件。调用者已确保
 * 设备停止访问，后端可进入页分配器并按其上下文约束释放。
 */
static void __dma_free_pages(struct device *dev, size_t size, struct page *page,
		dma_addr_t dma_handle, enum dma_data_direction dir)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	size = PAGE_ALIGN(size);
	if (dma_alloc_direct(dev, ops))
		dma_direct_free_pages(dev, size, page, dma_handle, dir);
	else if (use_dma_iommu(dev))
		dma_common_free_pages(dev, size, page, dma_handle, dir);
	else if (ops->free_pages)
		ops->free_pages(dev, size, page, dma_handle, dir);
}

/*
 * 释放 dma_alloc_pages() 返回的非一致性 DMA 页。
 *
 * @dev: 原设备。
 * @size: 原请求大小。
 * @page: 原首页。
 * @dma_handle: 原 DMA 地址。
 * @dir: 原方向。
 *
 * 在真实释放前统一发 trace/debug，再由 __dma_free_pages() 对称分派。调用者必须完成设备访问及必要
 * ownership 切换，返回后 page 和 dma_handle 均无效。
 */
void dma_free_pages(struct device *dev, size_t size, struct page *page,
		dma_addr_t dma_handle, enum dma_data_direction dir)
{
	trace_dma_free_pages(dev, page_to_virt(page), dma_handle, size, dir, 0);
	debug_dma_free_pages(dev, page, size, dir, dma_handle);
	__dma_free_pages(dev, size, page, dma_handle, dir);
}
EXPORT_SYMBOL_GPL(dma_free_pages);

/*
 * 把一段连续 DMA pages 映射进用户 VMA。
 *
 * @dev: 原分配设备；此简单 PFN 路径无需读取。
 * @vma: 用户区间及相对页偏移。
 * @size: 原分配大小。
 * @page: 连续区域首页。
 * 返回值: 映射成功为 0，范围越界为 -ENXIO，或 remap_pfn_range() 错误。
 *
 * 先用减法形式验证偏移和长度以避免溢出，再映射对应 PFN。VMA 只借用页，驱动必须延长分配寿命
 * 至所有映射关闭；函数在 mmap/mm 锁上下文执行并可能睡眠。
 */
int dma_mmap_pages(struct device *dev, struct vm_area_struct *vma,
		size_t size, struct page *page)
{
	unsigned long count = PAGE_ALIGN(size) >> PAGE_SHIFT;

	if (vma->vm_pgoff >= count || vma_pages(vma) > count - vma->vm_pgoff)
		return -ENXIO;
	return remap_pfn_range(vma, vma->vm_start,
			       page_to_pfn(page) + vma->vm_pgoff,
			       vma_pages(vma) << PAGE_SHIFT, vma->vm_page_prot);
}
EXPORT_SYMBOL_GPL(dma_mmap_pages);

/*
 * 在非 dma-iommu 后端用一段连续 pages 构造单项 noncontiguous API 结果。
 *
 * @dev: 目标设备。
 * @size: 请求大小。
 * @dir: DMA 方向。
 * @gfp: 分配约束。
 * 返回值: 成功为含一个 SG 项的表，失败为 NULL。
 *
 * 依次分配 sgt 对象、单项表和底层 DMA pages；任一步失败按逆序释放已完成资源。成功项同时填入
 * page、页对齐长度、DMA 地址和 dma_len，所有权整体转给调用者，必须由 free_single_sgt() 配对。
 * 分配可按 gfp 睡眠，半初始化对象从不发布。
 */
static struct sg_table *alloc_single_sgt(struct device *dev, size_t size,
		enum dma_data_direction dir, gfp_t gfp)
{
	struct sg_table *sgt;
	struct page *page;

	sgt = kmalloc_obj(*sgt, gfp);
	if (!sgt)
		return NULL;
	if (sg_alloc_table(sgt, 1, gfp))
		goto out_free_sgt;
	page = __dma_alloc_pages(dev, size, &sgt->sgl->dma_address, dir, gfp);
	if (!page)
		goto out_free_table;
	sg_set_page(sgt->sgl, page, PAGE_ALIGN(size), 0);
	sg_dma_len(sgt->sgl) = sgt->sgl->length;
	return sgt;
out_free_table:
	sg_free_table(sgt);
out_free_sgt:
	kfree(sgt);
	return NULL;
}

/*
 * 分配 DMA 地址连续、物理页可不连续的非一致性 SG 缓冲区。
 *
 * @dev: 目标设备。
 * @size: 请求大小。
 * @dir: DMA 方向。
 * @gfp: 分配约束。
 * @attrs: 当前仅允许 DMA_ATTR_ALLOC_SINGLE_PAGES。
 * 返回值: 成功为 sg_table，失败为 NULL。
 *
 * dma-iommu 可用离散页建立连续 IOVA；其他后端退化为一段物理连续页的单项表。成功统一令 nents=1
 * 并发 trace/debug，失败记录错误事件。调用者可 vmap/mmap，但必须用 dma_free_noncontiguous() 配对；
 * 可睡眠性由 gfp/后端决定，__GFP_COMP 被拒绝。
 */
struct sg_table *dma_alloc_noncontiguous(struct device *dev, size_t size,
		enum dma_data_direction dir, gfp_t gfp, unsigned long attrs)
{
	struct sg_table *sgt;

	if (WARN_ON_ONCE(attrs & ~DMA_ATTR_ALLOC_SINGLE_PAGES))
		return NULL;
	if (WARN_ON_ONCE(gfp & __GFP_COMP))
		return NULL;

	if (use_dma_iommu(dev))
		sgt = iommu_dma_alloc_noncontiguous(dev, size, dir, gfp, attrs);
	else
		sgt = alloc_single_sgt(dev, size, dir, gfp);

	if (sgt) {
		sgt->nents = 1;
		trace_dma_alloc_sgt(dev, sgt, size, dir, gfp, attrs);
		debug_dma_map_sg(dev, sgt->sgl, sgt->orig_nents, 1, dir, attrs);
	} else {
		trace_dma_alloc_sgt_err(dev, NULL, 0, size, dir, gfp, attrs);
	}
	return sgt;
}
EXPORT_SYMBOL_GPL(dma_alloc_noncontiguous);

/*
 * 释放 alloc_single_sgt() 构造的单项 fallback 对象。
 *
 * @dev: 原设备。
 * @size: 原大小。
 * @sgt: 单项表。
 * @dir: 原方向。
 *
 * 先用 SG 保存的 page/DMA 地址释放底层 pages，再销毁表和外层对象；调用者已撤销所有 vmap/mmap
 * 并停止设备访问。返回后 sgt 全部字段失效。
 */
static void free_single_sgt(struct device *dev, size_t size,
		struct sg_table *sgt, enum dma_data_direction dir)
{
	__dma_free_pages(dev, size, sg_page(sgt->sgl), sgt->sgl->dma_address,
			 dir);
	sg_free_table(sgt);
	kfree(sgt);
}

/*
 * 释放 dma_alloc_noncontiguous() 返回的 SG 缓冲区。
 *
 * @dev: 原设备。
 * @size: 原请求大小。
 * @sgt: 原表。
 * @dir: 原方向。
 *
 * 先发 trace/debug unmap，再按 dma-iommu 或单项 fallback 对称释放。调用者必须先 vunmap、关闭用户
 * VMA、停止 DMA 并完成 ownership 回收；函数只允许调用一次，后端释放可能睡眠。
 */
void dma_free_noncontiguous(struct device *dev, size_t size,
		struct sg_table *sgt, enum dma_data_direction dir)
{
	trace_dma_free_sgt(dev, sgt, size, dir);
	debug_dma_unmap_sg(dev, sgt->sgl, sgt->orig_nents, dir, 0);

	if (use_dma_iommu(dev))
		iommu_dma_free_noncontiguous(dev, size, sgt, dir);
	else
		free_single_sgt(dev, size, sgt, dir);
}
EXPORT_SYMBOL_GPL(dma_free_noncontiguous);

/*
 * 为 noncontiguous DMA 分配建立连续内核虚拟视图。
 *
 * @dev: 原分配设备。
 * @size: 原大小。
 * @sgt: 仍存活的分配表。
 * 返回值: dma-iommu 返回新 vmap；单项 fallback 直接返回首页永久线性地址。
 *
 * 返回视图不拥有底层页，必须在释放 sgt 前用 dma_vunmap_noncontiguous() 配对；IOMMU 路径可能
 * 分配页表并睡眠，fallback 不创建新映射。CPU/设备并发访问仍须遵循 noncoherent sync 契约。
 */
void *dma_vmap_noncontiguous(struct device *dev, size_t size,
		struct sg_table *sgt)
{

	if (use_dma_iommu(dev))
		return iommu_dma_vmap_noncontiguous(dev, size, sgt);

	return page_address(sg_page(sgt->sgl));
}
EXPORT_SYMBOL_GPL(dma_vmap_noncontiguous);

/*
 * 撤销 dma_vmap_noncontiguous() 建立的内核虚拟视图。
 *
 * @dev: 原设备。
 * @vaddr: vmap 返回地址。
 *
 * dma-iommu 路径撤销真实 vmap；单项 fallback 使用永久 page_address()，无需操作。函数不释放底层
 * SG/pages，调用者随后仍须 dma_free_noncontiguous()；IOMMU vunmap 可能要求可睡眠上下文。
 */
void dma_vunmap_noncontiguous(struct device *dev, void *vaddr)
{
	if (use_dma_iommu(dev))
		iommu_dma_vunmap_noncontiguous(dev, vaddr);
}
EXPORT_SYMBOL_GPL(dma_vunmap_noncontiguous);

/*
 * 把 noncontiguous DMA 分配映射到用户 VMA。
 *
 * @dev: 原设备。
 * @vma: 用户请求区间。
 * @size: 原分配大小。
 * @sgt: 仍存活的分配表。
 * 返回值: dma-iommu 离散页映射或单项连续页 dma_mmap_pages() 的结果。
 *
 * 用户映射只借用底层页，必须在所有 VMA 关闭后才能释放 sgt；函数在 mmap 进程/mm 锁上下文执行，
 * 具体缓存属性和 CPU/设备同步责任仍由调用者保持。
 */
int dma_mmap_noncontiguous(struct device *dev, struct vm_area_struct *vma,
		size_t size, struct sg_table *sgt)
{
	if (use_dma_iommu(dev))
		return iommu_dma_mmap_noncontiguous(dev, vma, size, sgt);
	return dma_mmap_pages(dev, vma, size, sg_page(sgt->sgl));
}
EXPORT_SYMBOL_GPL(dma_mmap_noncontiguous);

/*
 * 向当前后端询问给定 DMA mask 是否可支持。
 *
 * @dev: 目标设备。
 * @mask: 已按 dma_addr_t 宽度裁剪或待验证的地址掩码。
 * 返回值: 后端支持为 true，否则为 false。
 *
 * dma-iommu 固定支持且不应同时存在 ops；自定义 ops 的 dma_supported 可能设置/清除 bypass，因此
 * 即使当前 bypass 也必须调用它。无 ops 时交给 dma-direct。查询可能改变后端 bypass 状态，调用者
 * 应在设备配置串行阶段执行。
 */
static int dma_supported(struct device *dev, u64 mask)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (use_dma_iommu(dev)) {
		if (WARN_ON(ops))
			return false;
		return true;
	}

	/*
	 * ->dma_supported sets and clears the bypass flag, so ignore it here
	 * and always call into the method if there is one.
	 */
	/* dma_supported 回调自身维护 bypass 标志，故这里不能因当前 bypass 而跳过；存在回调就始终调用。 */
	if (ops) {
		if (!ops->dma_supported)
			return true;
		return ops->dma_supported(dev, mask);
	}

	return dma_direct_supported(dev, mask);
}

/*
 * 判断设备后端是否支持 PCI P2PDMA 页面映射。
 *
 * @dev: 目标设备。
 * 返回值: 未安装 dma_map_ops 时为 true，存在任意 ops 时保守为 false。
 *
 * 不能因某设备当前 bypass 就放行：同一 ops 后端若没有 P2P 语义，即便特定请求直通也不能安全使用。
 * 无 ops 覆盖 dma-direct 以及默认 dma-iommu 支持。纯查询不睡眠。
 */
bool dma_pci_p2pdma_supported(struct device *dev)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	/*
	 * Note: dma_ops_bypass is not checked here because P2PDMA should
	 * not be used with dma mapping ops that do not have support even
	 * if the specific device is bypassing them.
	 */
	/* 即使设备正在绕过 ops，只要该 ops 不支持 P2P 就不能启用；因此这里刻意不检查 bypass 标志。 */

	/* if ops is not set, dma direct and default IOMMU support P2PDMA */
	/* ops 为空表示 dma-direct 或默认 dma-iommu 路径，两者具备此处要求的 P2P 支持。 */
	return !ops;
}
EXPORT_SYMBOL_GPL(dma_pci_p2pdma_supported);

/*
 * 设置设备 streaming DMA 地址掩码并刷新同步快速路径。
 *
 * @dev: 目标设备，dev->dma_mask 必须指向可写存储。
 * @mask: 驱动请求的地址位范围。
 * 返回值: 后端接受时为 0；无 mask 存储或不支持时为 -EIO。
 *
 * 先裁剪到实际 dma_addr_t 宽度，避免生成本体系结构无法表示的地址；验证成功后让体系结构更新
 * 附加状态，再发布 `*dev->dma_mask`，最后依据新后端能力初始化 need-sync。应在 probe/config 串行
 * 阶段调用，不能与活动映射或并发 mask 修改交错；函数本身不分配内存。
 */
int dma_set_mask(struct device *dev, u64 mask)
{
	/*
	 * Truncate the mask to the actually supported dma_addr_t width to
	 * avoid generating unsupportable addresses.
	 */
	/* 把调用者的 u64 掩码裁剪到本体系结构 dma_addr_t 实际位宽，禁止发布不可表示地址。 */
	mask = (dma_addr_t)mask;

	if (!dev->dma_mask || !dma_supported(dev, mask))
		return -EIO;

	arch_dma_set_mask(dev, mask);
	*dev->dma_mask = mask;
	dma_setup_need_sync(dev);

	return 0;
}
EXPORT_SYMBOL(dma_set_mask);

/*
 * 设置设备 coherent DMA 分配使用的地址掩码。
 *
 * @dev: 目标设备。
 * @mask: 请求的 coherent 地址范围。
 * 返回值: 后端支持为 0，否则为 -EIO。
 *
 * 同样先裁剪到 dma_addr_t 位宽，再经 dma_supported() 验证，最后一次性写 coherent_dma_mask；与
 * streaming mask 不同，这里不调用 arch_dma_set_mask() 或 need-sync 初始化。应在分配发生前串行配置。
 */
int dma_set_coherent_mask(struct device *dev, u64 mask)
{
	/*
	 * Truncate the mask to the actually supported dma_addr_t width to
	 * avoid generating unsupportable addresses.
	 */
	/* coherent 掩码也必须先限制为本机 dma_addr_t 可表达的位数。 */
	mask = (dma_addr_t)mask;

	if (!dma_supported(dev, mask))
		return -EIO;

	dev->coherent_dma_mask = mask;
	return 0;
}
EXPORT_SYMBOL(dma_set_coherent_mask);

/*
 * 内部判断设备能否直接覆盖全部系统 RAM。
 *
 * @dev: 待查询设备。
 * 返回值: mask/bus limit 小于 required mask，或纯 direct 的 dma_range_map 存在 RAM 空洞时为 true。
 *
 * 存在自定义 ops 或 dma-iommu 时，required-mask 比较通过后由其地址转换能力承担覆盖，不再扫描
 * direct range map；只有原生 direct 需要验证所有 RAM 范围。查询不修改设备、不睡眠。
 */
static bool __dma_addressing_limited(struct device *dev)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (min_not_zero(dma_get_mask(dev), dev->bus_dma_limit) <
			 dma_get_required_mask(dev))
		return true;

	if (unlikely(ops) || use_dma_iommu(dev))
		return false;
	return !dma_direct_all_ram_mapped(dev);
}

/**
 * dma_addressing_limited - return if the device is addressing limited
 * @dev:	device to check
 *
 * Return %true if the devices DMA mask is too small to address all memory in
 * the system, else %false.  Lack of addressing bits is the prime reason for
 * bounce buffering, but might not be the only one.
 */
/*
 * 对外报告设备是否因 DMA 地址能力不足而受限。
 *
 * @dev: 待查询设备。
 * 返回值: 能覆盖全部系统内存为 false；掩码或 direct range 有限制为 true，并发 dev_dbg 诊断。
 *
 * 地址位不足是使用 bounce 的主要但非唯一原因，因此 false 不保证永不 bounce。纯查询不加锁、不睡眠。
 */
bool dma_addressing_limited(struct device *dev)
{
	if (!__dma_addressing_limited(dev))
		return false;

	dev_dbg(dev, "device is DMA addressing limited\n");
	return true;
}
EXPORT_SYMBOL_GPL(dma_addressing_limited);

/*
 * 查询后端允许的单次 DMA 映射硬上限。
 *
 * @dev: 目标设备。
 * 返回值: direct、dma-iommu 或 ops 回调给出的最大字节数；无约束时为 SIZE_MAX。
 *
 * 上层应用该值拆分请求；它是瞬时能力查询，不预留 IOVA/SWIOTLB 资源，不加锁、不睡眠。
 */
size_t dma_max_mapping_size(struct device *dev)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);
	size_t size = SIZE_MAX;

	if (dma_map_direct(dev, ops))
		size = dma_direct_max_mapping_size(dev);
	else if (use_dma_iommu(dev))
		size = iommu_dma_max_mapping_size(dev);
	else if (ops && ops->max_mapping_size)
		size = ops->max_mapping_size(dev);

	return size;
}
EXPORT_SYMBOL_GPL(dma_max_mapping_size);

/*
 * 查询兼顾性能的推荐 DMA 映射大小。
 *
 * @dev: 目标设备。
 * 返回值: dma-iommu 或 ops 的性能建议与 dma_max_mapping_size() 硬上限中的较小值。
 *
 * direct 没有额外优化建议时使用 SIZE_MAX，但仍受硬上限约束。该值用于分段策略而非正确性证明，
 * 不分配资源、不睡眠。
 */
size_t dma_opt_mapping_size(struct device *dev)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);
	size_t size = SIZE_MAX;

	if (use_dma_iommu(dev))
		size = iommu_dma_opt_mapping_size();
	else if (ops && ops->opt_mapping_size)
		size = ops->opt_mapping_size();

	return min(dma_max_mapping_size(dev), size);
}
EXPORT_SYMBOL_GPL(dma_opt_mapping_size);

/*
 * 查询 SG 合并时不可跨越的后端边界掩码。
 *
 * @dev: 目标设备。
 * 返回值: dma-iommu 或 ops 提供的 merge boundary；无能力/无 ops 时为 0，表示不能合并。
 *
 * 调用者据此决定相邻段是否可组成更大 DMA 段。纯查询不加锁、不睡眠，0 不是“无限制”。
 */
unsigned long dma_get_merge_boundary(struct device *dev)
{
	const struct dma_map_ops *ops = get_dma_ops(dev);

	if (use_dma_iommu(dev))
		return iommu_dma_get_merge_boundary(dev);

	if (!ops || !ops->get_merge_boundary)
		/* 返回 0 明确表示不能合并，而不是没有边界限制。 */
		return 0;	/* can't merge */

	return ops->get_merge_boundary(dev);
}
EXPORT_SYMBOL_GPL(dma_get_merge_boundary);
