/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2008 Advanced Micro Devices, Inc.
 *
 * Author: Joerg Roedel <joerg.roedel@amd.com>
 */

#ifndef _KERNEL_DMA_DEBUG_H
#define _KERNEL_DMA_DEBUG_H

/*
 * 本头把通用 DMA API 的状态转换镜像到 dma-debug：启用时，map/alloc 创建跟踪项，
 * sync 校验区间和方向，unmap/free 校验配对后删除；关闭时，完全相同的调用点编译为空函数，
 * 因而驱动无需散布条件编译。调试函数只观察并记录，不拥有真实 DMA 映射或缓冲区。
 * 启用版钩子使用预分配池、GFP_ATOMIC 补充项和自旋锁，不主动睡眠，可跟随原 DMA API
 * 所在上下文调用；锁只保护调试镜像，不为真实设备/缓冲区提供生命周期保护。
 */
#ifdef CONFIG_DMA_API_DEBUG
/*
 * debug_dma_map_phys() - 登记一次成功的物理区间 DMA 映射
 * @dev: 借用设备；@phys: CPU 物理起点；@size: 字节数；@direction: DMA 方向；
 * @dma_addr: 已返回的设备地址；@attrs: 映射属性。无返回值；失败地址不登记，成功时调试层
 * 取得独立 entry，真实映射 ownership 不变。调用者是 mapping.c 的 map 完成路径。
 */
extern void debug_dma_map_phys(struct device *dev, phys_addr_t phys,
			       size_t size, int direction, dma_addr_t dma_addr,
			       unsigned long attrs);

/*
 * debug_dma_unmap_phys() - 校验并删除物理映射跟踪项
 * @dev: 借用设备；@addr: 待解除设备地址；@size: 原长度字节数；@direction: 原方向；
 * @attrs: 原属性。无返回值；报告未登记、大小/方向/属性不匹配等错误，真实 unmap 已由通用层负责。
 */
extern void debug_dma_unmap_phys(struct device *dev, dma_addr_t addr,
				 size_t size, int direction,
				 unsigned long attrs);

/*
 * debug_dma_map_sg() - 为成功 SG 映射的每个输出段建立调试项
 * @dev: 借用设备；@sg: 输入输出原始表；@nents: 调用项数；@mapped_ents: 成功输出项数；
 * @direction: DMA 方向；@attrs: 属性。无返回值；检查非法源区和段边界，调试项不足时允许部分记录。
 */
extern void debug_dma_map_sg(struct device *dev, struct scatterlist *sg,
			     int nents, int mapped_ents, int direction,
			     unsigned long attrs);

/*
 * debug_dma_unmap_sg() - 按原始项数校验并删除 SG 映射记录
 * @dev: 借用设备；@sglist: 输入输出 SG 表；@nelems: map 调用时原始项数；@dir: 原方向；
 * @attrs: 原属性。无返回值；从首项恢复 mapped_ents，只处理实际映射段并报告配对错误。
 */
extern void debug_dma_unmap_sg(struct device *dev, struct scatterlist *sglist,
			       int nelems, int dir, unsigned long attrs);

/*
 * debug_dma_alloc_coherent() - 登记 coherent 分配的 CPU/物理/设备三种地址
 * @dev: 借用设备；@size: 分配字节数；@dma_addr: 设备地址；@virt: 借用 CPU 地址；
 * @attrs: 分配属性。无返回值；NULL/无效地址或调试项耗尽时不登记，不影响真实分配成功。
 */
extern void debug_dma_alloc_coherent(struct device *dev, size_t size,
				     dma_addr_t dma_addr, void *virt,
				     unsigned long attrs);

/*
 * debug_dma_free_coherent() - 校验 coherent 分配参数并删除跟踪项
 * @dev: 借用设备；@size: 原字节数；@virt: 原 CPU 地址；@addr: 原设备地址；
 * @attrs: 原属性。无返回值；无效 CPU 地址被忽略，其他不匹配由 check_unmap() 报告。
 */
extern void debug_dma_free_coherent(struct device *dev, size_t size, void *virt,
				    dma_addr_t addr, unsigned long attrs);

/*
 * debug_dma_sync_single_for_cpu() - 校验单缓冲区同步回 CPU 的区间和方向
 * @dev: 借用设备；@dma_handle: 有效设备地址；@size: 同步字节数；@direction: 原方向。
 * 无返回值、不改变真实 ownership；仅查找包含该区间的映射并报告越界/方向错误。
 */
extern void debug_dma_sync_single_for_cpu(struct device *dev,
					  dma_addr_t dma_handle, size_t size,
					  int direction);

/*
 * debug_dma_sync_single_for_device() - 校验单缓冲区重新同步到设备
 * @dev: 借用设备；@dma_handle: 有效设备地址；@size: 同步字节数；@direction: 原方向。
 * 无返回值；调用者是真实 sync 完成路径，调试层只检查记录而不执行 cache 操作。
 */
extern void debug_dma_sync_single_for_device(struct device *dev,
					     dma_addr_t dma_handle,
					     size_t size, int direction);

/*
 * debug_dma_sync_sg_for_cpu() - 校验 SG 实际映射段同步回 CPU
 * @dev: 借用设备；@sg: 借用的有效 SG 映射；@nelems: 原始项数；@direction: 原方向。
 * 无返回值；从记录恢复 mapped_ents 并逐段 check_sync，不释放或修改真实 SG 映射。
 */
extern void debug_dma_sync_sg_for_cpu(struct device *dev,
				      struct scatterlist *sg,
				      int nelems, int direction);

/*
 * debug_dma_sync_sg_for_device() - 校验 SG 实际映射段重新交给设备
 * @dev: 借用设备；@sg: 借用的有效 SG 映射；@nelems: 原始项数；@direction: 原方向。
 * 无返回值；只检查区间/方向和记录存在性，真实 cache/bounce 同步由调用点完成。
 */
extern void debug_dma_sync_sg_for_device(struct device *dev,
					 struct scatterlist *sg,
					 int nelems, int direction);

/*
 * debug_dma_alloc_pages() - 登记非一致性 dma_alloc_pages 分配
 * @dev: 借用设备；@page: 借用的分配首页；@size: 字节数；@direction: DMA 方向；
 * @dma_addr: 设备地址。无返回值；成功创建 noncoherent entry，调试项不足不影响真实分配。
 */
extern void debug_dma_alloc_pages(struct device *dev, struct page *page,
				  size_t size, int direction,
				  dma_addr_t dma_addr);

/*
 * debug_dma_free_pages() - 校验并删除非一致性页面分配记录
 * @dev: 借用设备；@page: 原首页；@size: 原字节数；@direction: 原方向；@dma_addr: 原设备地址。
 * 无返回值；check_unmap() 报告配对错误，真实设备映射和页面由 dma_free_pages() 路径释放。
 */
extern void debug_dma_free_pages(struct device *dev, struct page *page,
				 size_t size, int direction,
				 dma_addr_t dma_addr);
#else /* CONFIG_DMA_API_DEBUG */
/*
 * 关闭 DMA_API_DEBUG 时，下列同名 inline 保持调用 ABI 并由编译器消除。
 * 每个函数均不睡眠、无返回值、无副作用，也不取得任何参数引用。
 */
/* debug_dma_map_phys() - 空登记；@dev 设备，@phys 物理地址，@size 字节数，
 * @direction 方向，@dma_addr 设备地址，@attrs 属性，均为未使用输入；无直接返回值和副作用。
 */
static inline void debug_dma_map_phys(struct device *dev, phys_addr_t phys,
				      size_t size, int direction,
				      dma_addr_t dma_addr, unsigned long attrs)
{
}

/* debug_dma_unmap_phys() - 空校验；@dev 设备，@addr 设备地址，@size 字节数，
 * @direction 方向，@attrs 属性，均为未使用输入；无直接返回值和副作用。
 */
static inline void debug_dma_unmap_phys(struct device *dev, dma_addr_t addr,
					size_t size, int direction,
					unsigned long attrs)
{
}

/* debug_dma_map_sg() - 空登记；@dev 设备，@sg SG 表，@nents 原始项数，
 * @mapped_ents 映射项数，@direction 方向，@attrs 属性，均为未使用输入；无返回值和副作用。
 */
static inline void debug_dma_map_sg(struct device *dev, struct scatterlist *sg,
				    int nents, int mapped_ents, int direction,
				    unsigned long attrs)
{
}

/* debug_dma_unmap_sg() - 空校验；@dev 设备，@sglist SG 表，@nelems 原始项数，
 * @dir 方向，@attrs 属性，均为未使用输入；无直接返回值和副作用。
 */
static inline void debug_dma_unmap_sg(struct device *dev,
				      struct scatterlist *sglist, int nelems,
				      int dir, unsigned long attrs)
{
}

/* debug_dma_alloc_coherent() - 空登记；@dev 设备，@size 字节数，@dma_addr 设备地址，
 * @virt CPU 地址，@attrs 属性，均为未使用输入；无直接返回值和副作用。
 */
static inline void debug_dma_alloc_coherent(struct device *dev, size_t size,
					    dma_addr_t dma_addr, void *virt,
					    unsigned long attrs)
{
}

/* debug_dma_free_coherent() - 空校验；@dev 设备，@size 字节数，@virt CPU 地址，
 * @addr 设备地址，@attrs 属性，均为未使用输入；无直接返回值和副作用。
 */
static inline void debug_dma_free_coherent(struct device *dev, size_t size,
					   void *virt, dma_addr_t addr,
					   unsigned long attrs)
{
}

/* debug_dma_sync_single_for_cpu() - 空 CPU sync 校验；@dev 设备，@dma_handle 设备地址，
 * @size 字节数，@direction 方向，均为未使用输入；无直接返回值和副作用。
 */
static inline void debug_dma_sync_single_for_cpu(struct device *dev,
						 dma_addr_t dma_handle,
						 size_t size, int direction)
{
}

/* debug_dma_sync_single_for_device() - 空设备 sync 校验；@dev 设备，@dma_handle 设备地址，
 * @size 字节数，@direction 方向，均为未使用输入；无直接返回值和副作用。
 */
static inline void debug_dma_sync_single_for_device(struct device *dev,
						    dma_addr_t dma_handle,
						    size_t size, int direction)
{
}

/* debug_dma_sync_sg_for_cpu() - 空 SG CPU sync 校验；@dev 设备，@sg SG 表，
 * @nelems 原始项数，@direction 方向，均为未使用输入；无直接返回值和副作用。
 */
static inline void debug_dma_sync_sg_for_cpu(struct device *dev,
					     struct scatterlist *sg,
					     int nelems, int direction)
{
}

/* debug_dma_sync_sg_for_device() - 空 SG 设备 sync 校验；@dev 设备，@sg SG 表，
 * @nelems 原始项数，@direction 方向，均为未使用输入；无直接返回值和副作用。
 */
static inline void debug_dma_sync_sg_for_device(struct device *dev,
						struct scatterlist *sg,
						int nelems, int direction)
{
}

/* debug_dma_alloc_pages() - 空页面登记；@dev 设备，@page 首页，@size 字节数，
 * @direction 方向，@dma_addr 设备地址，均为未使用输入；无直接返回值和副作用。
 */
static inline void debug_dma_alloc_pages(struct device *dev, struct page *page,
					 size_t size, int direction,
					 dma_addr_t dma_addr)
{
}

/* debug_dma_free_pages() - 空页面释放校验；@dev 设备，@page 首页，@size 字节数，
 * @direction 方向，@dma_addr 设备地址，均为未使用输入；无直接返回值和副作用。
 */
static inline void debug_dma_free_pages(struct device *dev, struct page *page,
					size_t size, int direction,
					dma_addr_t dma_addr)
{
}
#endif /* CONFIG_DMA_API_DEBUG */
/* 上述条件块在启用时记录/校验 DMA 生命周期，关闭时仅保留零成本接口。 */
#endif /* _KERNEL_DMA_DEBUG_H */
/* include guard 防止同一编译单元重复定义这些 inline stub 或 extern 声明。 */
