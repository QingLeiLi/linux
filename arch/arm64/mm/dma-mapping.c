// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 非一致性 DMA cache 维护与设备 DMA 属性接入层。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * CPU cache 与非 coherent 设备观察同一物理内存时，需要在所有权从 CPU
 * 转给设备前 clean、从设备取回前 invalidate。函数只做本地范围维护，
 * DMA API 上层负责方向、屏障和映射生命周期；coherent 设备可绕过这些
 * 成本。Xen 最后还可替换/补充 DMA ops 以适配 guest 地址转换。
 */
/*
 * Copyright (C) 2012 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 */

#include <linux/gfp.h>
#include <linux/cache.h>
#include <linux/dma-map-ops.h>
#include <xen/xen.h>

#include <asm/cacheflush.h>
#include <asm/xen/xen-ops.h>

/*
 * CPU->device 所有权切换：paddr 是线性映射 RAM 的物理字节地址，size 是
 * 字节数，dir 由通用 DMA API 传入但 clean 对各方向均安全。把脏 cache
 * line 写到 PoC，使设备可见；nosync 省略最终屏障，由外层 DMA API 合并。
 */
void arch_sync_dma_for_device(phys_addr_t paddr, size_t size,
			      enum dma_data_direction dir)
{
	unsigned long start = (unsigned long)phys_to_virt(paddr);

	dcache_clean_poc_nosync(start, start + size);
}

/*
 * device->CPU 所有权切换。DMA_TO_DEVICE 表示设备未写内存，可直接返回；
 * 其他方向 invalidate [paddr,paddr+size)，丢弃 CPU 旧副本以读取设备结果。
 * paddr 必须属于 phys_to_virt 可覆盖的线性 RAM，范围生命周期由调用者保证。
 */
void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size,
			   enum dma_data_direction dir)
{
	unsigned long start = (unsigned long)phys_to_virt(paddr);

	if (dir == DMA_TO_DEVICE)
		return;

	dcache_inval_poc_nosync(start, start + size);
}

/*
 * 为 coherent DMA 分配的新页面建立干净的 PoC 状态。page 是具有稳定直接
 * 映射的首页，size 为字节范围；同步版 clean 在返回前完成必要排序，确保
 * 随后把页面交给设备时不会由旧 CPU 脏行覆盖设备数据。
 */
void arch_dma_prep_coherent(struct page *page, size_t size)
{
	unsigned long start = (unsigned long)page_address(page);

	dcache_clean_poc(start, start + size);
}

/*
 * 为设备安装体系结构 DMA 属性。dev 生命周期由设备核心管理，coherent
 * 来自固件/总线声明。若非一致设备的最大 cache writeback granule 大于
 * ARCH_DMA_MINALIGN，普通内存对齐不足可能发生相邻对象 cache-line 破坏，
 * 因而告警并 taint，但仍继续启动。最后让 Xen guest 接管所需 DMA ops。
 */
void arch_setup_dma_ops(struct device *dev, bool coherent)
{
	/* cls 是当前 CPU CTR_EL0.CWG 推导的 cache line/writeback granule 字节数。 */
	int cls = cache_line_size_of_cpu();

	WARN_TAINT(!coherent && cls > ARCH_DMA_MINALIGN,
		   TAINT_CPU_OUT_OF_SPEC,
		   "%s %s: ARCH_DMA_MINALIGN smaller than CTR_EL0.CWG (%d < %d)",
		   dev_driver_string(dev), dev_name(dev),
		   ARCH_DMA_MINALIGN, cls);

	dev_assign_dma_coherent(dev, coherent);

	xen_setup_dma_ops(dev);
}
