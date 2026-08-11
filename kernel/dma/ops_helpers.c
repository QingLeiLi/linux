// SPDX-License-Identifier: GPL-2.0
/*
 * Helpers for DMA ops implementations.  These generally rely on the fact that
 * the allocated memory contains normal pages in the direct kernel mapping.
 */
/*
 * 本文件为传统 dma_map_ops 后端提供公共实现。核心前提是分配结果由普通 struct page
 * 支撑，并且能从 CPU 地址恢复首页；get_sgtable/mmap 还假定后续物理页连续。若后端返回
 * 设备内存、无 struct page 内存或真正离散的 vmalloc 页面，就不能直接复用这些 helper。
 */
#include <linux/dma-map-ops.h>
#include <linux/iommu-dma.h>

/*
 * dma_common_vaddr_to_page() - 从 DMA 缓冲区 CPU 地址恢复其首页描述符
 *
 * 调用者是本文件的 sgtable/mmap helper。@cpu_addr 是纯输入借用地址，必须指向仍存活的
 * DMA 分配；函数不取得 page 引用、不修改映射且不睡眠。vmalloc/vmap 地址不能交给
 * virt_to_page()，故先按地址区间分流到 vmalloc_to_page()；直接映射地址走快速转换。
 * 返回借用的 struct page 指针，无错误编码；输入契约不成立时结果不可靠，调用者必须保证
 * 地址有效。下一步通常以该页构造 SG 表或用户 PTE。
 */
static struct page *dma_common_vaddr_to_page(void *cpu_addr)
{
	/* 两类 KVA 使用不同页表关系，但都只恢复首页而不增加引用。 */
	if (is_vmalloc_addr(cpu_addr))
		return vmalloc_to_page(cpu_addr);
	return virt_to_page(cpu_addr);
}

/*
 * Create scatter-list for the already allocated DMA buffer.
 */
/*
 * 为已经分配的 DMA 缓冲区创建 scatterlist 描述；它不重新进行 DMA 映射。
 *
 * dma_common_get_sgtable() - 把物理连续 DMA 分配导出为单项 SG 表
 *
 * 由 dma_get_sgtable_attrs() 经后端回调调用，常用于把 coherent 分配交给 dma-buf 等只
 * 接受 SG 表的接口。@dev、@dma_addr、@attrs 均为借用的上下文信息，当前实现不使用；
 * @sgt 是输出对象，成功前应未初始化，成功后由调用方用 sg_free_table() 释放其表节点；
 * @cpu_addr 是仍存活分配的 CPU 起始地址；@size 是有效字节数，表项长度按页向上取整。
 *
 * 本函数用 GFP_KERNEL 分配表节点，必须在可睡眠上下文调用且不持有禁止睡眠的锁。它先恢复
 * 首页，再申请恰好一个 SG 项；只有分配成功才写 sgt->sgl，从而不在失败时发布半成品。
 * 返回 0 表示 SG 表完整，负 errno 来自 sg_alloc_table()。函数不取得物理页 ownership，
 * 不改变 CPU/设备 cache ownership，也不创建新的 DMA 地址；原缓冲区必须活到 SG 使用结束。
 */
int dma_common_get_sgtable(struct device *dev, struct sg_table *sgt,
		 void *cpu_addr, dma_addr_t dma_addr, size_t size,
		 unsigned long attrs)
{
	/* page 是借用首页；ret 保存表节点分配结果，决定能否填写输出对象。 */
	struct page *page = dma_common_vaddr_to_page(cpu_addr);
	int ret;

	/* 单项表依赖“首页之后物理连续”的文件级前提，不能描述任意离散 vmalloc 内存。 */
	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (!ret)
		sg_set_page(sgt->sgl, page, PAGE_ALIGN(size), 0);
	return ret;
}

/*
 * Create userspace mapping for the DMA-coherent memory.
 */
/*
 * 为 DMA coherent 内存创建用户态映射。
 *
 * dma_common_mmap() - 将已有 DMA 分配的选定页区间映射进用户 VMA
 *
 * 由 dma_mmap_attrs() 经后端回调调用。@dev 是借用设备并决定页保护/设备专属池；@vma 是
 * 输入输出对象，vm_pgoff 指定缓冲区内页偏移，成功后获得用户 PTE；@cpu_addr/@dma_addr
 * 分别是原分配的 CPU/设备地址（后者当前不用）；@size 是原分配字节数；@attrs 必须与分配
 * 属性一致。缓冲区 ownership 不转移，驱动必须等 VMA 释放后才能释放它。
 *
 * 必须在 mmap 进程上下文调用并允许睡眠。MMU 路径先计算用户页数、分配页数和偏移，设置与
 * DMA 一致性属性匹配的 vm_page_prot；设备专属 coherent 池有优先处理权。普通路径在减法前
 * 同时验证 offset 和长度，避免下溢/越界，再用 remap_pfn_range() 建立 PTE。
 * 返回 0 成功；设备池返回值原样传递；范围非法或无 MMU 返回 -ENXIO；页表建立错误原样返回。
 * 失败不会建立超界映射，但 vm_page_prot 已可能被更新。成功后用户映射成为新的可观察别名。
 */
int dma_common_mmap(struct device *dev, struct vm_area_struct *vma,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		unsigned long attrs)
{
#ifdef CONFIG_MMU
	/* 变量地图：user_count 为用户请求页数，count 为缓冲区页数，off 为缓冲区内页偏移。 */
	unsigned long user_count = vma_pages(vma);
	unsigned long count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	unsigned long off = vma->vm_pgoff;
	/* page 是借用首页；ret 是设备专属 coherent 池通过输出参数给出的处理结果。 */
	struct page *page = dma_common_vaddr_to_page(cpu_addr);
	int ret = -ENXIO;

	/* 页保护必须先匹配 DMA 属性，避免用户别名采用与内核 coherent 视图冲突的 cache 类型。 */
	vma->vm_page_prot = dma_pgprot(dev, vma->vm_page_prot, attrs);

	/* 设备声明的 coherent 池知道自身地址布局；命中时由它完整决定成功或错误。 */
	if (dma_mmap_from_dev_coherent(dev, vma, cpu_addr, size, &ret))
		return ret;

	/* 先检查 off，再做 count - off，可同时阻止偏移越界和算术下溢。 */
	if (off >= count || user_count > count - off)
		return -ENXIO;

	/* 发布用户 PTE；PFN 从缓冲区首页加 vm_pgoff 得到，长度以用户请求页数计。 */
	return remap_pfn_range(vma, vma->vm_start,
			page_to_pfn(page) + vma->vm_pgoff,
			user_count << PAGE_SHIFT, vma->vm_page_prot);
#else
	/* 无页表系统无法建立独立用户虚拟别名，所有参数保持不变。 */
	return -ENXIO;
#endif /* CONFIG_MMU */
	/* 上述条件块分别覆盖有页表的真实映射和无页表的固定失败实现。 */
}

/*
 * dma_common_alloc_pages() - 分配非一致性页面并建立设备可用 DMA 地址
 *
 * 由 dma_alloc_pages() 在 IOMMU 或传统 dma_map_ops 后端调用。@dev 是借用设备；@size
 * 为请求字节数，实际物理分配按 order/页粒度扩大；@dma_handle 是必需输出，成功后保存
 * 设备地址；@dir 决定以后显式同步方向；@gfp 控制睡眠/回收，通用入口已拒绝 DMA zone、
 * HIGHMEM 和 compound 等不合约标志。成功返回首页并把物理页 ownership 交给调用方，
 * 失败返回 NULL 且不留下页面或 IOMMU/ops 映射。页面分配失败时 @dma_handle 保持调用前
 * 内容；设备映射失败时它保留 DMA_MAPPING_ERROR 哨兵，调用方都必须以 NULL 返回为准。
 *
 * 阶段一优先从设备/NUMA/全局 CMA 得到连续页，不可用时退回设备 NUMA 节点 buddy 分配；
 * 阶段二把首页物理地址经 dma-iommu 或 ops->map_phys 转成设备地址。两条映射都带
 * DMA_ATTR_SKIP_CPU_SYNC，因为 dma_alloc_pages API 把 cache ownership 同步显式留给调用方，
 * 映射创建本身不能假装已经把新缓冲区交给设备。映射失败用 dma_free_contiguous() 统一识别
 * CMA 或 buddy 来源并回滚。成功后清零有效 size，调用方仍须在设备访问前调用相应 sync。
 *
 * 函数可能因 CMA/buddy/IOMMU 分配而睡眠，必须遵守 @gfp 上下文；不持有本地锁。返回的 page、
 * dma_handle、size、dir 和 dev 构成必须原样传给 dma_common_free_pages() 的生命周期元组。
 */
struct page *dma_common_alloc_pages(struct device *dev, size_t size,
		dma_addr_t *dma_handle, enum dma_data_direction dir, gfp_t gfp)
{
	/* ops 是设备当前借用操作表；page/phys 依次表示 CPU 页对象和其物理起点。 */
	const struct dma_map_ops *ops = get_dma_ops(dev);
	struct page *page;
	phys_addr_t phys;

	/* CMA 优先满足物理连续性；失败后 buddy 按 size 的最小 order 在设备节点重试。 */
	page = dma_alloc_contiguous(dev, size, gfp);
	if (!page)
		page = alloc_pages_node(dev_to_node(dev), gfp, get_order(size));
	if (!page)
		return NULL;

	/* 此时页面归当前函数持有，只有设备侧地址建立成功后才向调用者发布。 */
	phys = page_to_phys(page);
	if (use_dma_iommu(dev))
		*dma_handle = iommu_dma_map_phys(dev, phys, size, dir,
						 DMA_ATTR_SKIP_CPU_SYNC);
	else
		*dma_handle = ops->map_phys(dev, phys, size, dir,
					    DMA_ATTR_SKIP_CPU_SYNC);
	/* 映射失败时设备从未获得地址；统一释放 helper 会按页面真实来源选择 CMA 或 buddy。 */
	if (*dma_handle == DMA_MAPPING_ERROR) {
		dma_free_contiguous(dev, page, size);
		return NULL;
	}

	/* 清零发生在 CPU 域；后续交给设备前的 cache 同步由公开 API 调用者显式完成。 */
	memset(page_address(page), 0, size);
	return page;
}

/*
 * dma_common_free_pages() - 撤销设备映射并释放 dma_common_alloc_pages() 页面
 *
 * 由 dma_free_pages() 调用。@dev、@size、@dma_handle、@dir 必须与分配时完全一致；@page
 * 必须是分配返回的首页，均为输入，调用完成后 page 与 DMA 地址同时失效。函数无直接返回值。
 * 调用方必须已停止设备 DMA，并按非一致性 API 契约完成所需同步；这里使用
 * DMA_ATTR_SKIP_CPU_SYNC，只撤销地址转换而不隐式改变 cache ownership。
 *
 * 先按分配时相同的 use_dma_iommu() 决策解除 IOMMU 映射，否则在传统后端提供 unmap_phys
 * 时调用它；随后无条件 dma_free_contiguous()，由其依次识别设备 CMA、NUMA/全局 CMA，均
 * 不匹配时回退 __free_pages。这个顺序不可交换：先释放物理页会让仍存活的设备地址指向可能
 * 已重新分配的内存。函数可能进入后端和页面释放路径，不应在不满足后端约束的上下文调用；
 * 不取得新引用，也没有失败返回或部分保留语义。
 */
void dma_common_free_pages(struct device *dev, size_t size, struct page *page,
		dma_addr_t dma_handle, enum dma_data_direction dir)
{
	/* ops 只为非 dma-iommu 分支提供与 alloc 时配对的 unmap_phys。 */
	const struct dma_map_ops *ops = get_dma_ops(dev);

	/* 第一阶段先摘除设备可见地址，阻止释放后的物理页仍被 DMA 访问。 */
	if (use_dma_iommu(dev))
		iommu_dma_unmap_phys(dev, dma_handle, size, dir,
				     DMA_ATTR_SKIP_CPU_SYNC);
	else if (ops->unmap_phys)
		ops->unmap_phys(dev, dma_handle, size, dir,
				DMA_ATTR_SKIP_CPU_SYNC);
	/* 第二阶段释放 CPU 物理页；helper 内部处理 CMA 与 buddy 两种来源。 */
	dma_free_contiguous(dev, page, size);
}
