# SPDX-License-Identifier: GPL-2.0

# Kbuild 将 obj-$(CONFIG_FOO) 中的配置值展开为 obj-y 或空变量：前者把右侧 .o
# 编入当前目录的 built-in.a，后者完全跳过。这里没有可加载模块分支；这些能力要么
# 作为内核核心 DMA 基础设施随内核链接，要么在配置期不存在。

# HAS_DMA 是目录的总入口。mapping.o 提供驱动可见的通用 DMA API 与后端分派，
# direct.o 实现 CPU 物理地址和设备 DMA 地址可直接换算时的默认后端；两者必须同时存在，
# 否则通用层可能选择直映射却没有对应实现。
obj-$(CONFIG_HAS_DMA)			+= mapping.o direct.o

# 只有采用传统 dma_map_ops 的体系结构才选择 DMA_OPS_HELPERS；该对象提供可复用的
# 页面分配、释放等回调，避免每个架构重复实现相同的操作表辅助逻辑。
obj-$(CONFIG_DMA_OPS_HELPERS)		+= ops_helpers.o

# ARCH_HAS_DMA_OPS 表示体系结构允许 dev->dma_ops/架构操作表分派。dummy.o 随之编入，
# 让固件明确声明“不支持 DMA”的设备仍可安装一个所有映射均失败的安全后端。
obj-$(CONFIG_ARCH_HAS_DMA_OPS)		+= dummy.o

# DMA_CMA 打开物理连续内存分配器的 DMA 接口；contiguous.o 管理默认、设备专属和
# NUMA CMA 区域，在不支持 IOMMU/SG 的硬件上提供大块连续页。
obj-$(CONFIG_DMA_CMA)			+= contiguous.o

# DMA_DECLARE_COHERENT 允许设备声明预留的一致性 DMA 内存池；coherent.o 负责登记、
# 位图分配、mmap 与释放这类设备专属区域。
obj-$(CONFIG_DMA_DECLARE_COHERENT)	+= coherent.o

# DMA_API_DEBUG 是有运行时成本的驱动查错层；debug.o 跟踪映射生命周期，以发现
# 重复解除、方向/大小不匹配和从未建立却被释放的映射。
obj-$(CONFIG_DMA_API_DEBUG)		+= debug.o

# SWIOTLB 为地址受限或受机密计算约束的设备建立软件 bounce buffer；swiotlb.o 管理
# 启动期/动态内存池，并在 CPU 原缓冲区与设备可达槽位之间同步数据。
obj-$(CONFIG_SWIOTLB)			+= swiotlb.o

# DMA_COHERENT_POOL 提供不可睡眠分配路径使用的预建原子池；pool.o 在启动期扩展
# gen_pool，并把连续页准备成设备可一致访问的 CPU 映射。
obj-$(CONFIG_DMA_COHERENT_POOL)		+= pool.o

# MMU 存在时才需要 remap.o：它借助 vmap/vunmap 为连续或离散 DMA 页面建立带指定
# 页保护属性的连续 KVA。无 MMU 系统没有页表，也就不存在这种虚拟别名重映射。
obj-$(CONFIG_MMU)			+= remap.o

# DMA_MAP_BENCHMARK 依赖 debugfs，map_benchmark.o 暴露测试控制面，反复执行流式
# dma_map_page()/dma_unmap_page() 并汇总耗时；它只用于测量，不参与正常 DMA 数据路径。
obj-$(CONFIG_DMA_MAP_BENCHMARK)		+= map_benchmark.o
