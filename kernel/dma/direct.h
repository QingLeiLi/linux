/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018 Christoph Hellwig.
 *
 * DMA operations that map physical memory directly without using an IOMMU.
 */
/*
 * 本头文件描述“不经 IOMMU、直接由 CPU 物理地址换算设备 DMA 地址”的内部协议。
 * 它同时保留两种必要退化：设备地址能力不足或强制隔离时转入 SWIOTLB bounce buffer，
 * 非一致性设备则在 CPU/设备 ownership 切换处调用体系结构 cache 同步。
 */
#ifndef _KERNEL_DMA_DIRECT_H
#define _KERNEL_DMA_DIRECT_H

#include <linux/dma-direct.h>
#include <linux/memremap.h>

/*
 * dma_direct_get_sgtable() - 将 direct coherent 分配导出为单项 SG 表
 * @dev/@cpu_addr/@dma_addr/@size/@attrs 均借用且必须来自同一次分配；@sgt 为输出，成功后
 * 由调用方释放表节点。可睡眠；返回 0 或 sg_alloc_table() 的负 errno，不转移物理页所有权。
 * @dev: 纯输入、借用的分配所属设备，用于把设备地址恢复为页面。
 * @sgt: 纯输出 SG 表；成功后持有新表节点，释放责任交给调用方。
 * @cpu_addr: 纯输入、借用的 CPU 视图起点；当前实现不解引用。
 * @dma_addr: 纯输入的设备地址起点，用于定位物理首页。
 * @size: 原分配有效长度，单位字节；SG 长度向上取整到页。
 * @attrs: 原分配属性位图；当前实现保留接口但不据此改变导出。
 * 调用者：mapping.c 的 dma_get_sgtable_attrs()；成功后通常交给 dma-buf/用户导出路径。
 */
int dma_direct_get_sgtable(struct device *dev, struct sg_table *sgt,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		unsigned long attrs);

/*
 * dma_direct_can_mmap() - 查询 direct 分配是否允许创建用户映射
 * @dev 为借用设备；不睡眠、无副作用。设备一致或配置支持 noncoherent mmap 时返回 true，
 * 否则 false；调用者下一步仍须以 dma_direct_mmap() 的实际返回值为准。
 * @dev: 纯输入、借用设备；只读取一致性属性，不延长设备生命周期。
 * 调用者：dma_can_mmap()；true 后通常继续调用 dma_mmap_attrs() 建立实际映射。
 */
bool dma_direct_can_mmap(struct device *dev);

/*
 * dma_direct_mmap() - 把 direct coherent 分配的指定页区间映射到用户 VMA
 * @vma 是输入输出对象，其他参数借用且须与原分配一致；缓冲区 ownership 不转移，驱动必须
 * 保持其存活到 VMA 释放。进程上下文可睡眠；返回 0、池后端 errno、-ENXIO 或页表错误。
 * @dev: 纯输入、借用设备，决定地址反换算、页保护和 coherent 池。
 * @vma: 输入输出用户 VMA；vm_pgoff 给出页偏移，成功后页表可见。
 * @cpu_addr: 纯输入、借用的原 CPU 地址，供设备/全局 coherent 池识别。
 * @dma_addr: 纯输入的设备地址，转换为映射首页 PFN。
 * @size: 原分配长度，单位字节，用于边界检查。
 * @attrs: 原分配属性位图，决定 cache 和加密页保护。
 * 调用者：dma_mmap_attrs()；成功后由 VMA 生命周期继续持有页面映射。
 */
int dma_direct_mmap(struct device *dev, struct vm_area_struct *vma,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		unsigned long attrs);

/*
 * dma_direct_need_sync() - 判断映射是否需要显式 CPU/设备同步
 * @dev 与 @dma_addr 均为借用输入；不睡眠、无副作用。非一致性设备或地址落入 SWIOTLB 池时
 * 返回 true，否则 false；它只回答同步需求，不执行同步或延长映射生命周期。
 * @dev: 纯输入、借用设备，提供一致性标志和 SWIOTLB 池集合。
 * @dma_addr: 纯输入的有效设备地址，用于反换算并查询 bounce 池。
 * 调用者：通用 dma_need_sync()；结果供驱动决定能否省略 sync/unmap。
 */
bool dma_direct_need_sync(struct device *dev, dma_addr_t dma_addr);

/*
 * dma_direct_map_sg() - 为原始 SG 表逐项建立 direct/P2PDMA/SWIOTLB 地址
 * @sgl 为输入输出数组，@nents 为原始项数，@dir/@attrs 控制 ownership 与退化策略；成功
 * 返回 nents 并发布各项 dma_address/length，失败返回 -EIO/-EREMOTEIO 并回滚已映射前缀。
 * @dev: 纯输入、借用的 DMA 设备。
 * @sgl: 输入输出原始 SG 表；成功写 DMA 地址/长度，失败撤销已写前缀。
 * @nents: 原始表项数，单位项，必须为可遍历范围。
 * @dir: 设备相对内存的数据方向，决定 cache/bounce copy 方向。
 * @attrs: 映射属性位图，控制同步、coherent 和特殊地址策略。
 * 调用者：mapping.c 的 __dma_map_sg_attrs()；成功后驱动把各 DMA 段提交给设备。
 */
int dma_direct_map_sg(struct device *dev, struct scatterlist *sgl, int nents,
		enum dma_data_direction dir, unsigned long attrs);

/*
 * dma_direct_all_ram_mapped() - 检查设备 dma-range-map 是否覆盖全部系统 RAM
 * @dev 为借用输入；遍历 RAM 资源但不修改设备，返回 true 表示所有 RAM 都可直接换算。
 * 调用者据此判断 direct 后端是否还能承诺任意内存可达。
 * @dev: 纯输入、借用设备；读取其只读 dma_range_map 快照。
 * 调用者：direct 后端能力判断；结果用于识别是否存在无法直接换算的系统 RAM。
 */
bool dma_direct_all_ram_mapped(struct device *dev);

/*
 * dma_direct_max_mapping_size() - 返回一次 direct 流式映射允许的最大字节数
 * @dev 为借用输入；SWIOTLB 真正承担受限/强制 bounce 时返回其槽位上限，否则 SIZE_MAX。
 * 不分配资源、不改变映射；结果是能力上限，不代表当前一定有足够 bounce 空间。
 * @dev: 纯输入、借用设备；用于判断寻址限制、强制 bounce 和池上限。
 * 调用者：dma_max_mapping_size()；返回值向驱动约束单次请求大小。
 */
size_t dma_direct_max_mapping_size(struct device *dev);

#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE) || \
    defined(CONFIG_SWIOTLB)
/*
 * dma_direct_sync_sg_for_device() - 把 SG 各段的最新 CPU 内容提交给设备域
 * @dev 借用；@sgl/@nents 描述仍存活的已映射原始表；@dir 必须与 map 一致。无返回值，
 * 逐段同步 SWIOTLB/体系结构 cache，最后批量 flush；不创建或销毁 DMA 地址。
 * @dev: 纯输入、借用设备，提供一致性和地址换算信息。
 * @sgl: 输入、借用的已映射 SG 表；函数不改变其 DMA 字段。
 * @nents: 要同步的原始项数，单位项。
 * @dir: 原映射方向，决定 cache 与 bounce copy 方向。
 * 调用者：dma_sync_sg_for_device() 的 direct 分支；返回后设备可重新访问各段。
 */
void dma_direct_sync_sg_for_device(struct device *dev, struct scatterlist *sgl,
		int nents, enum dma_data_direction dir);
#else
/*
 * dma_direct_sync_sg_for_device() - 无设备同步和 SWIOTLB 能力时的空实现
 * 所有参数都是未使用的借用输入；无返回值、不睡眠、无副作用。配置保证此时没有隐藏的
 * bounce copy 或体系结构 cache 操作需要执行，空实现不能移植到启用任一能力的配置。
 * @dev: 未使用的借用设备。
 * @sgl: 未使用的借用 SG 表。
 * @nents: 未使用的表项数，单位项。
 * @dir: 未使用的原映射方向。
 * 调用者仍是通用 SG sync；返回后无需额外后端动作。
 */
static inline void dma_direct_sync_sg_for_device(struct device *dev,
		struct scatterlist *sgl, int nents, enum dma_data_direction dir)
{
}
#endif

#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL) || \
    defined(CONFIG_SWIOTLB)
/*
 * dma_direct_unmap_sg() - 逐项撤销 SG 映射并按 attrs 决定是否归还 CPU ownership
 * @dev/@sgl/@nents/@dir/@attrs 必须与 map 配对；无返回值。P2PDMA bus 地址只清标记，普通项
 * 进入 dma_direct_unmap_phys()，最后合并体系结构 flush；调用后 SG 的 DMA 映射生命周期结束。
 * @dev: 纯输入、借用设备。
 * @sgl: 输入输出 SG 表；清除 bus-address 标记并撤销普通项映射。
 * @nents: 原始表项数，单位项。
 * @dir: 原映射方向，必须与 map 一致。
 * @attrs: 原映射属性，决定是否跳过 CPU 同步。
 * 调用者：dma_unmap_sg_attrs() 及 map_sg 失败回滚；返回后上层结束 SG 生命周期。
 */
void dma_direct_unmap_sg(struct device *dev, struct scatterlist *sgl,
		int nents, enum dma_data_direction dir, unsigned long attrs);

/*
 * dma_direct_sync_sg_for_cpu() - 保留映射同时把 SG 内容交还 CPU 域
 * 参数均借用且须对应有效映射；无返回值。先执行非一致性 cache 同步，再把 SWIOTLB 槽内容
 * 复制回原缓冲区，最后完成批量 flush/all-CPU hook；不释放 DMA 地址。
 * @dev: 纯输入、借用设备。
 * @sgl: 输入、借用的仍有效 SG 映射。
 * @nents: 同步表项数，单位项。
 * @dir: 原映射方向，决定设备写入的可见性处理。
 * 调用者：dma_sync_sg_for_cpu()；返回后 CPU 可按方向契约访问原缓冲区。
 */
void dma_direct_sync_sg_for_cpu(struct device *dev,
		struct scatterlist *sgl, int nents, enum dma_data_direction dir);
#else
/*
 * dma_direct_unmap_sg() - 无 CPU 同步和 SWIOTLB 状态时的空解除实现
 * 参数仅维持统一接口；配置证明映射无需回收 bounce 状态或 cache ownership，故无返回值、
 * 无资源释放和副作用。映射地址本身是直接换算值，不占独立后端对象。
 * @dev: 未使用的借用设备。
 * @sgl: 未使用的借用 SG 表。
 * @nents: 未使用的原始项数，单位项。
 * @dir: 未使用的原映射方向。
 * @attrs: 未使用的属性位图。
 * 调用者：通用 unmap 或失败回滚；direct 地址无需独立释放。
 */
static inline void dma_direct_unmap_sg(struct device *dev,
		struct scatterlist *sgl, int nents, enum dma_data_direction dir,
		unsigned long attrs)
{
}

/*
 * dma_direct_sync_sg_for_cpu() - 无 CPU cache/SWIOTLB 同步需求时的空实现
 * @dev/@sgl/@nents/@dir 均为未使用的借用输入；无返回值、不睡眠、不改变 ownership 元数据。
 * @dev: 未使用的借用设备。
 * @sgl: 未使用的借用 SG 表。
 * @nents: 未使用的表项数，单位项。
 * @dir: 未使用的原映射方向。
 * 调用者：通用 CPU sync；返回即代表无需后端同步。
 */
static inline void dma_direct_sync_sg_for_cpu(struct device *dev,
		struct scatterlist *sgl, int nents, enum dma_data_direction dir)
{
}
#endif

/*
 * dma_direct_sync_single_for_device() - 将一个仍映射缓冲区从 CPU 域同步到设备域
 * @dev: 借用设备，决定地址换算、一致性和 SWIOTLB 池；@addr: 有效设备地址；@size: 字节数；
 * @dir: 原映射方向。无返回值，不改变映射 lifetime；调用者须保证区间有效且 CPU 不再并发访问。
 * @dev: 纯输入、借用设备；不取得引用。
 * @addr: 纯输入的有效 DMA 地址起点。
 * @size: 同步区间长度，单位字节，不能越过原映射。
 * @dir: 原映射方向，决定复制与 cache 操作语义。
 * 调用者：dma_sync_single_for_device()；返回后下一步通常启动设备 DMA。
 * 先让 SWIOTLB 在需要时把原缓冲区复制到 bounce 槽，再对非一致性设备同步实际 DMA 物理区间；
 * arch_dma_sync_flush() 提交可能批处理的 cache 操作。函数不睡眠，适用于 DMA sync 调用上下文。
 */
static inline void dma_direct_sync_single_for_device(struct device *dev,
		dma_addr_t addr, size_t size, enum dma_data_direction dir)
{
	/* paddr 指向设备地址当前实际对应的原页或 bounce 槽，不取得页面引用。 */
	phys_addr_t paddr = dma_to_phys(dev, addr);

	/* bounce 写入必须先于对设备可见区间的 cache 提交。 */
	swiotlb_sync_single_for_device(dev, paddr, size, dir);

	/* 一致性设备由硬件维持可见性；非一致性设备需要体系结构 hook 和批量提交。 */
	if (!dev_is_dma_coherent(dev)) {
		arch_sync_dma_for_device(paddr, size, dir);
		arch_sync_dma_flush();
	}
}

/*
 * dma_direct_sync_single_for_cpu() - 将一个仍映射缓冲区的设备写入同步回 CPU 域
 * @dev/@addr/@size/@dir 与原映射一致且均为借用输入；@flush 控制是否立即提交可批处理的
 * 体系结构 cache 操作。无返回值，不解除映射；调用方取得 CPU ownership 后才能读写缓冲区。
 * @dev: 纯输入、借用设备；提供一致性和 SWIOTLB 状态。
 * @addr: 纯输入的有效 DMA 地址起点。
 * @size: 同步区间长度，单位字节。
 * @dir: 原映射方向，决定设备写回的处理方式。
 * @flush: true 立即提交体系结构批处理，false 由外层循环末尾统一提交。
 * 调用者：通用 single sync 或 unmap；返回后 CPU 获得相应访问权。
 * 非一致性路径先让 CPU cache 看见设备对实际 DMA 区间的写入，再执行全 CPU hook；最后
 * SWIOTLB 把 bounce 数据复制回原缓冲区。顺序反转会把旧 bounce 内容暴露给 CPU。
 */
static inline void dma_direct_sync_single_for_cpu(struct device *dev,
		dma_addr_t addr, size_t size, enum dma_data_direction dir,
		bool flush)
{
	/* paddr 是设备实际访问区间；flush=false 供 SG 循环把昂贵提交合并到末尾。 */
	phys_addr_t paddr = dma_to_phys(dev, addr);

	/* 先处理设备写入的 cache 可见性，再允许 bounce copy 被 CPU 正确观察。 */
	if (!dev_is_dma_coherent(dev)) {
		arch_sync_dma_for_cpu(paddr, size, dir);
		if (flush)
			arch_sync_dma_flush();
		arch_sync_dma_for_cpu_all();
	}

	/* 若地址来自 SWIOTLB，这一步才把设备写入从槽位复制回调用方原缓冲区。 */
	swiotlb_sync_single_for_cpu(dev, paddr, size, dir);
}

/*
 * dma_direct_map_phys() - 把 CPU 物理区间映射为设备可访问的 direct DMA 地址
 * @dev: 借用设备，提供 mask/bus limit/一致性和 SWIOTLB 策略；@phys: CPU 物理起点；
 * @size: 字节数；@dir: 设备读写方向；@attrs: MMIO、CC_SHARED、REQUIRE_COHERENT、
 * SKIP_CPU_SYNC 等策略；@flush: 是否立即提交体系结构批量 cache 操作。
 * @dev: 纯输入、借用设备；不改变其 mask、limit 或策略字段。
 * @phys: 纯输入的 CPU 物理起点，函数不持有对应页面引用。
 * @size: 映射长度，单位字节；能力检查覆盖完整半开区间。
 * @dir: 设备相对内存的数据方向。
 * @attrs: 映射属性位图，控制地址类型、coherent 要求和同步。
 * @flush: true 立即提交 cache 批处理，false 由 SG 外层统一提交。
 * 调用者：dma_map_phys() 或 dma_direct_map_sg()；成功地址随后写入设备描述符。
 *
 * 不持有调用方资源引用，通常不睡眠；SWIOTLB 分支可能领取 bounce 槽。成功返回可发布给设备的
 * dma_addr，失败返回 DMA_MAPPING_ERROR，且 swiotlb_map() 自行保证失败不遗留槽位。
 * 阶段一处理强制 bounce/机密计算共享属性的互斥条件；阶段二分别换算 MMIO、未加密共享内存或
 * 普通 RAM，并验证整个区间不越过 dma_mask/bus limit。普通 RAM 若设备不可达或 kmalloc cache
 * 对齐不安全则退化到 SWIOTLB。阶段三在发布地址前把非一致性 CPU cache ownership 交给设备。
 */
static inline dma_addr_t dma_direct_map_phys(struct device *dev,
		phys_addr_t phys, size_t size, enum dma_data_direction dir,
		unsigned long attrs, bool flush)
{
	/* dma_addr 保存各地址换算分支的候选设备地址，也用于溢出告警。 */
	dma_addr_t dma_addr;

	/* 强制 bounce 仅处理普通私有 RAM；MMIO/强一致性请求不能用复制槽伪装。 */
	if (is_swiotlb_force_bounce(dev)) {
		if (!(attrs & DMA_ATTR_CC_SHARED)) {
			if (attrs & (DMA_ATTR_MMIO | DMA_ATTR_REQUIRE_COHERENT))
				return DMA_MAPPING_ERROR;

			return swiotlb_map(dev, phys, size, dir, attrs);
		}
	} else if (attrs & DMA_ATTR_CC_SHARED) {
		/* 未处于强制共享转换策略时，不能凭属性直接制造 CC shared 地址。 */
		return DMA_MAPPING_ERROR;
	}

	/* 三类地址必须使用各自换算规则；每类都在发布前验证设备完整可达。 */
	if (attrs & DMA_ATTR_MMIO) {
		dma_addr = phys;
		if (unlikely(!dma_capable(dev, dma_addr, size, false)))
			goto err_overflow;
	} else if (attrs & DMA_ATTR_CC_SHARED) {
		dma_addr = phys_to_dma_unencrypted(dev, phys);
		if (unlikely(!dma_capable(dev, dma_addr, size, false)))
			goto err_overflow;
	} else {
		dma_addr = phys_to_dma(dev, phys);
		/* 地址越界或非一致性 kmalloc cacheline 不安全时，优先用 SWIOTLB 隔离。 */
		if (unlikely(!dma_capable(dev, dma_addr, size, true)) ||
		    dma_kmalloc_needs_bounce(dev, size, dir)) {
			if (is_swiotlb_active(dev) &&
			    !(attrs & DMA_ATTR_REQUIRE_COHERENT))
				return swiotlb_map(dev, phys, size, dir, attrs);

			goto err_overflow;
		}
	}

	/* SKIP_CPU_SYNC 表示调用方已管理 ownership；MMIO 也不执行普通 RAM cache 维护。 */
	if (!dev_is_dma_coherent(dev) &&
	    !(attrs & (DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_MMIO))) {
		arch_sync_dma_for_device(phys, size, dir);
		if (flush)
			arch_sync_dma_flush();
	}
	/* 到这里地址、bounce 内容和 cache 可见性都已准备完成，才把 DMA 地址返回给上层。 */
	return dma_addr;

err_overflow:
	/* 失败路径没有发布地址；一次性设备告警保留候选地址、长度和两个可达上限供诊断。 */
	dev_WARN_ONCE(
		dev, 1,
		"DMA addr %pad+%zu overflow (mask %llx, bus limit %llx).\n",
		&dma_addr, size, *dev->dma_mask, dev->bus_dma_limit);
	return DMA_MAPPING_ERROR;
}

/*
 * dma_direct_unmap_phys() - 结束单个 direct/SWIOTLB 映射并按需归还 CPU ownership
 * @dev/@addr/@size/@dir/@attrs 必须与 map 配对；@flush 决定非一致性 cache 操作是否立即提交。
 * @dev: 纯输入、借用设备。
 * @addr: 待失效的 DMA 地址起点。
 * @size: 原映射长度，单位字节。
 * @dir: 原映射方向。
 * @attrs: 原映射属性，尤其保留 SKIP_CPU_SYNC/MMIO/REQUIRE_COHERENT。
 * @flush: true 立即提交 cache 操作，false 由外层批量提交。
 * 调用者：dma_unmap_phys() 或 SG 回滚/解除；返回后上层不得再使用 @addr。
 * 无直接返回值。MMIO 或 REQUIRE_COHERENT 映射没有 bounce/cache 回收工作，直接返回；普通路径
 * 先在未设置 SKIP_CPU_SYNC 时同步回 CPU，再释放可能存在的 SWIOTLB 槽。传给槽回收的 attrs
 * 强制加入 SKIP_CPU_SYNC，避免已经执行的同步被重复一次。调用后 DMA 地址不得再交给设备。
 * 函数不取得页面引用；调用方必须已停止 DMA，且不能与同一映射的 sync/unmap 并发。
 */
static inline void dma_direct_unmap_phys(struct device *dev, dma_addr_t addr,
		size_t size, enum dma_data_direction dir, unsigned long attrs,
		bool flush)
{
	/* phys 用于识别 addr 是否来自 SWIOTLB 池；在无需处理的属性分支中不必计算。 */
	phys_addr_t phys;

	if (attrs & (DMA_ATTR_MMIO | DMA_ATTR_REQUIRE_COHERENT))
		/* nothing to do: uncached and no swiotlb */
		/* 这两类映射不缓存且未使用 SWIOTLB，因此没有 cache 或槽位状态需要回收。 */
		return;

	/* 先恢复实际物理区间，再按 ownership 契约同步；SKIP 表示调用方另行负责。 */
	phys = dma_to_phys(dev, addr);
	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		dma_direct_sync_single_for_cpu(dev, addr, size, dir, flush);

	/* 最后释放 bounce 槽；附加 SKIP 防止底层再次执行已经完成的 CPU 同步。 */
	swiotlb_tbl_unmap_single(dev, phys, size, dir,
					 attrs | DMA_ATTR_SKIP_CPU_SYNC);
}
#endif /* _KERNEL_DMA_DIRECT_H */
/* 上述 include guard 使本文件的声明和内联定义在每个编译单元中只展开一次。 */
