// SPDX-License-Identifier: GPL-2.0
/*
 * Dummy DMA ops that always fail.
 */
/*
 * 该文件实现一个“拒绝服务型”DMA 操作表：设备仍可经过统一 DMA API 分派，
 * 但任何建立 CPU/设备地址关系的请求都会得到明确失败。当前实际安装者是
 * acpi_dma_configure_id()；当固件把设备标记为 DEV_DMA_NOT_SUPPORTED 时，它把
 * dev->dma_ops 指向 dma_dummy_ops，避免该设备误落入直映射或其他 DMA 后端。
 *
 * 这里没有可分配对象、映射表或引用计数，因而所有回调都不转移缓冲区 ownership，
 * 也没有发布给设备的 DMA 地址。映射失败后驱动必须遵守 DMA API 契约，不得调用
 * 对应 unmap；两个 unmap 回调以一次性告警检查这一调用方不变量。
 */
#include <linux/dma-map-ops.h>

/*
 * dma_dummy_mmap() - 拒绝把 DMA 分配映射到用户虚拟地址空间
 *
 * 【宏观位置】dma_mmap_attrs() 选择设备操作表后调用本函数；这是 dummy 后端的
 * 用户映射终点，不会继续进入 remap_pfn_range() 等真正建立 VMA 页表的 helper。
 * 【入口与上下文】
 * @dev: 纯输入、借用的设备对象；其 dma_ops 已指向 dma_dummy_ops，函数不持有引用。
 * @vma: 纯输入、借用的目标 VMA；本函数不修改其页表、边界或标志。
 * @cpu_addr: 原 DMA 分配的 CPU 视图地址；可被上层传入，但这里不解引用、不接管。
 * @dma_addr: 同一分配的设备视图地址，单位为字节地址；这里不会把它暴露给用户态。
 * @size: 原分配大小，单位为字节；dummy 后端不检查范围，因为任何大小都不支持。
 * @attrs: 原分配的 DMA 属性位图；不论属性组合如何都不能把不支持 DMA 的设备变成
 *         可映射设备。
 * 调用发生在 mmap 一类进程上下文；本实现不取锁、不分配内存且不睡眠。
 * 【返回与副作用】始终返回 -ENXIO；VMA、DMA 缓冲区及其 ownership 均保持不变，
 * 调用者应把实际 mmap 失败返回给上层。注意 dma_can_mmap() 只按 mmap 回调是否存在
 * 判断能力，因此对该操作表会返回 true；真正建立映射仍必须以本函数返回值为准。
 */
static int dma_dummy_mmap(struct device *dev, struct vm_area_struct *vma,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		unsigned long attrs)
{
	/* -ENXIO 表示该设备后端不存在可供用户态映射的 DMA 地址空间。 */
	return -ENXIO;
}

/*
 * dma_dummy_map_phys() - 拒绝把一段 CPU 物理地址转换为设备可用 DMA 地址
 *
 * 【宏观位置】dma_map_phys()/dma_map_page_attrs()/dma_map_resource() 在排除直映射、
 * IOMMU 和体系结构直达路径后，经 dma_map_ops.map_phys 分派到这里。
 * 【参数】
 * @dev: 纯输入、借用的目标设备；不增加引用、不修改设备状态。
 * @phys: 待映射区域起始 CPU 物理地址，单位为字节；本函数不访问该内存。
 * @size: 区域长度，单位为字节；任何非零或边界组合都不会获得映射。
 * @dir: 设备相对内存的数据方向；上层已验证枚举有效，这里不建立 cache ownership。
 * @attrs: 映射属性位图；dummy 后端不支持 MMIO、共享内存或普通 RAM 的任何映射。
 * 可从 DMA API 允许的调用上下文进入；本函数不取锁、不睡眠。
 * 【返回与副作用】始终返回 DMA_MAPPING_ERROR。没有 DMA 地址发布给设备，没有
 * cache 同步或 ownership 转移；驱动应通过 dma_mapping_error() 识别失败，且不得
 * 对失败值调用 dma_unmap_phys()/dma_unmap_page_attrs()。
 */
static dma_addr_t dma_dummy_map_phys(struct device *dev, phys_addr_t phys,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	/* 全 1 哨兵由通用 DMA API 解释为映射失败，而不是可编程给设备的总线地址。 */
	return DMA_MAPPING_ERROR;
}

/*
 * dma_dummy_unmap_phys() - 捕获“失败的物理映射仍被解除”这一 DMA API 误用
 *
 * 【宏观位置】dma_unmap_phys()/dma_unmap_page_attrs() 通过 dummy 操作表进入这里；
 * 正确驱动只有在 map 成功后才应 unmap，而 dma_dummy_map_phys() 不可能成功。
 * 【参数】
 * @dev: 纯输入、借用的设备对象；不修改其 dma_ops 或引用。
 * @dma_handle: 调用方声称已映射的设备地址，单位为字节地址；dummy 后端从未生成它。
 * @size: 调用方声称的映射长度，单位为字节；没有对应资源可释放。
 * @dir: 原映射方向；此处没有 cache ownership 可交还 CPU。
 * @attrs: 原映射属性位图；不会触发任何体系结构或 IOMMU 清理。
 * 本函数不取 DMA 子系统锁、不睡眠；WARN_ON_ONCE 可从错误调用发生的原上下文报告。
 * 【返回与副作用】无直接返回值，不解除映射、不释放引用；仅在全局范围首次命中时
 * 报告告警。调用后系统资源状态不变，但该告警表明驱动的 map/unmap 配对已被破坏。
 */
static void dma_dummy_unmap_phys(struct device *dev, dma_addr_t dma_handle,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	/*
	 * Dummy ops doesn't support map_phys, so unmap_page should never be
	 * called.
	 */
	/*
	 * dummy 操作表不支持 map_phys，因此对应的页面/物理地址解除映射路径绝不应
	 * 到达这里。若到达，通常说明调用方没有先用 dma_mapping_error() 检查结果。
	 */
	WARN_ON_ONCE(true);
}

/*
 * dma_dummy_map_sg() - 拒绝建立 scatterlist 的流式 DMA 映射
 *
 * 【宏观位置】dma_map_sg_attrs()/dma_map_sgtable() 经 __dma_map_sg_attrs() 选择
 * dma_map_ops.map_sg 后调用本函数。它是 dummy 后端的批量映射失败出口。
 * 【参数】
 * @dev: 纯输入、借用的目标设备；不修改引用或 DMA 配置。
 * @sgl: 输入输出接口中的 scatterlist 首项；正常后端会写各项 DMA 地址/长度，
 *       本函数既不遍历也不修改，表项和其中页面仍完全归调用方/CPU 域管理。
 * @nelems: sgl 中原始表项数，单位为项；失败时不会合并表项或产生映射项数。
 * @dir: 设备相对内存的数据方向；不发生 CPU 与设备之间的 cache ownership 转移。
 * @attrs: 流式映射属性位图；dummy 后端不支持任何属性组合。
 * 可从通用 DMA 映射允许的上下文调用；本函数不取锁、不睡眠。
 * 【返回与副作用】始终返回 -EINVAL，表示该设备使用 DMA 映射这一请求本身无效、
 * 重试不能成功。dma_map_sgtable() 会保留该 errno；dma_map_sg_attrs() 则按公开契约
 * 把任何负错误折叠成 0 个映射项。两种入口下都没有可供设备访问的 SG 段。
 */
static int dma_dummy_map_sg(struct device *dev, struct scatterlist *sgl,
		int nelems, enum dma_data_direction dir,
		unsigned long attrs)
{
	/* -EINVAL 是 __dma_map_sg_attrs() 明确认可且不会改写为 -EIO 的错误类别。 */
	return -EINVAL;
}

/*
 * dma_dummy_unmap_sg() - 捕获“失败的 SG 映射仍被解除”这一配对错误
 *
 * 【宏观位置】dma_unmap_sg_attrs() 在非直映射、非 IOMMU 路径经操作表调用本函数；
 * 但 dma_dummy_map_sg() 永不产生成功映射，所以正确驱动不应到达这里。
 * 【参数】
 * @dev: 纯输入、借用的设备对象；不改变设备配置或引用。
 * @sgl: 调用方声称已映射的 scatterlist；本函数不读取、不恢复其 DMA 字段。
 * @nelems: 原始表项数，单位为项；没有相应后端状态可按此数量释放。
 * @dir: 原映射方向；没有 cache ownership 需要交还 CPU。
 * @attrs: 原映射属性；不会触发同步或后端清理。
 * 本函数不取 DMA 子系统锁、不睡眠；可在错误调用发生的原上下文执行告警。
 * 【返回与副作用】无直接返回值，不修改 SG、不释放资源；仅用 WARN_ON_ONCE 在首次
 * 命中时暴露调用方没有检查 map 返回值或错误维护 map/unmap 生命周期的问题。
 */
static void dma_dummy_unmap_sg(struct device *dev, struct scatterlist *sgl,
		int nelems, enum dma_data_direction dir,
		unsigned long attrs)
{
	/*
	 * Dummy ops doesn't support map_sg, so unmap_sg should never be called.
	 */
	/*
	 * dummy 操作表不支持 map_sg，因此 unmap_sg 不应被调用；到达这里意味着调用方
	 * 把“0 个映射项”或负错误误当作一次已经发布给设备的成功映射。
	 */
	WARN_ON_ONCE(true);
}

/*
 * dma_dummy_supported() - 声明设备不支持任何宽度的 DMA 地址掩码
 *
 * 【宏观位置】dma_set_mask()/dma_set_coherent_mask() 经内部 dma_supported() 调用
 * dma_map_ops.dma_supported；本函数阻断设备初始化阶段的 DMA 能力协商。
 * 【参数】
 * @hwdev: 纯输入、借用的待配置设备；函数不写 dma_mask/coherent_dma_mask。
 * @mask: 驱动希望设备可寻址的 DMA 地址位掩码；不论 32 位、64 位或其他范围都拒绝。
 * 初始化上下文中调用；本函数不取锁、不睡眠，也不改变 dma_ops bypass 状态。
 * 【返回与副作用】始终返回 0（false）。通用层随后使 dma_set_mask() 或
 * dma_set_coherent_mask() 返回 -EIO，并保留原掩码；没有地址能力被发布给驱动。
 */
static int dma_dummy_supported(struct device *hwdev, u64 mask)
{
	/* false 是能力查询结果，不是 errno；错误码由上层 dma_set_*_mask() 生成。 */
	return 0;
}

/*
 * dma_dummy_ops - 不支持 DMA 的设备共享的只读分派表
 *
 * 该 const 全局对象具有内核镜像生命周期，不含可变字段，也无需锁或引用计数。
 * acpi_dma_configure_id() 只把其地址借给 dev->dma_ops；设备不拥有也不释放操作表。
 * mmap/map_phys/map_sg 明确返回各自 API 所需的失败形式，两个 unmap 回调用告警检查
 * 配对不变量，dma_supported 阻止掩码协商。未列出的 alloc/free、sync 等回调均为
 * NULL，通用层会按各 API 的缺省失败/无操作规则处理，而不会从本表获得 DMA 资源。
 */
const struct dma_map_ops dma_dummy_ops = {
	.mmap                   = dma_dummy_mmap,
	.map_phys               = dma_dummy_map_phys,
	.unmap_phys             = dma_dummy_unmap_phys,
	.map_sg                 = dma_dummy_map_sg,
	.unmap_sg               = dma_dummy_unmap_sg,
	.dma_supported          = dma_dummy_supported,
};
