// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2014 The Linux Foundation
 */
#include <linux/dma-map-ops.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

/*
 * dma_common_find_pages() - 从 DMA coherent vmap 地址找回原始页面指针数组
 *
 * 【宏观位置】32 位 ARM IOMMU 的 mmap/free 路径拿到驱动保存的 cpu_addr 后调用本函数，
 * 以恢复 dma_common_pages_remap() 在 vm_struct 中登记的 pages[]。它只负责查询，不建立
 * 或销毁映射。
 * 【入口与参数】
 * @cpu_addr: 纯输入、借用的内核虚拟地址，必须是仍然存活的 vmap 区域起始地址；调用方
 *            持有对应 DMA 分配，因而查询期间不得并发 dma_common_free_remap()。
 * 进程上下文中调用；本函数本身不睡眠、不取得页面引用。find_vm_area() 返回的 vm_struct
 * 也是借用指针，其生命周期由仍存活的映射保证，而不是由本函数延长。
 * 【返回与副作用】区域不存在或未标记 VM_DMA_COHERENT 时返回 NULL；有效时返回登记在
 * area->pages 的借用指针，数组及物理页 ownership 仍属于 DMA 分配路径。若区域还混有
 * 其他 flag，会发出告警但仍返回 pages，帮助发现构造协议漂移。连续页 remap 不登记
 * pages[]，所以即使区域有效也可能返回 NULL。函数不修改映射、页面或引用计数。
 */
struct page **dma_common_find_pages(void *cpu_addr)
{
	/* area 只在当前映射存活期有效；pages 是稍后用于 mmap/free 的原页面目录。 */
	struct vm_struct *area = find_vm_area(cpu_addr);

	/* 先拒绝任意 vmalloc 地址，避免把无关 vm_struct 的 pages 字段当成 DMA 元数据。 */
	if (!area || !(area->flags & VM_DMA_COHERENT))
		return NULL;
	/* 当前构造者只应设置这一用途位；额外位不阻断回收，但值得暴露。 */
	WARN(area->flags != VM_DMA_COHERENT,
	     "unexpected flags in area: %p\n", cpu_addr);
	return area->pages;
}

/*
 * Remaps an array of PAGE_SIZE pages into another vm_area.
 * Cannot be used in non-sleeping contexts
 */
/*
 * 把由 PAGE_SIZE 页面组成的数组映射到另一段连续内核虚拟区；该过程会分配 vmalloc
 * 元数据和页表，因此不能在不可睡眠上下文调用。
 *
 * dma_common_pages_remap() - 为离散 DMA 页面建立连续 CPU 虚拟视图
 *
 * 【宏观位置】ARM IOMMU coherent 分配已取得 pages[] 并建立设备侧 IOVA 后调用本函数；
 * 成功返回的 cpu_addr 随后交给驱动，mmap/free 又可由 dma_common_find_pages() 找回数组。
 * 【参数】
 * @pages: 纯输入并被登记的页面指针数组；至少覆盖 PAGE_ALIGN(size)/PAGE_SIZE 项。
 *         函数不取得数组或各 page 的引用，成功后调用方仍拥有并必须维持其生命周期。
 * @size: 请求映射的有效字节数；实际 KVA 映射向上取整到整页。
 * @prot: 新内核映射的页保护属性，通常携带 coherent/noncached 或 write-combine 语义。
 * @caller: 调用点诊断地址；当前实现未使用该参数，vmap() 记录的是本包装层调用点，
 *          因此它不影响映射、ownership 或返回值。
 * 必须处于可睡眠进程上下文；入口不要求 DMA 锁，调用方需独占尚未发布的分配对象。
 * 【阶段】先让 vmap() 分配 KVA 并安装指向 pages[] 的 PTE；成功后再把原数组指针登记到
 * vm_struct，供 ARM 后续恢复。登记发生在返回地址发布给驱动之前，无并发读者可见半成品。
 * 【返回与副作用】成功返回页对齐 KVA，失败返回 NULL 且 pages[]/物理页仍归调用方清理。
 * 成功仅创建 CPU 别名，不转移物理页或数组 ownership；必须用 dma_common_free_remap()
 * 销毁视图，再由分配后端释放 pages[] 和物理页。
 */
void *dma_common_pages_remap(struct page **pages, size_t size,
			 pgprot_t prot, const void *caller)
{
	/* vaddr 是即将发布给驱动的连续 CPU 视图；NULL 表示 vmap 完全失败。 */
	void *vaddr;

	/* 页数向上取整；VM_DMA_COHERENT 也允许该区域后续安全映射到用户 VMA。 */
	vaddr = vmap(pages, PAGE_ALIGN(size) >> PAGE_SHIFT,
		     VM_DMA_COHERENT, prot);
	/* vmap 默认不保存数组；显式登记借用指针，供 find_pages 恢复但不交出所有权。 */
	if (vaddr)
		find_vm_area(vaddr)->pages = pages;
	return vaddr;
}

/*
 * Remaps an allocated contiguous region into another vm_area.
 * Cannot be used in non-sleeping contexts
 */
/*
 * 把一段已分配的连续物理页重新映射到另一段连续内核虚拟区；分配临时页面目录和
 * vmalloc 页表都可能阻塞，所以不能在不可睡眠上下文使用。
 *
 * dma_common_contiguous_remap() - 为连续 DMA 页面建立指定保护属性的 KVA 别名
 *
 * 【宏观位置】dma-direct、DMA 原子池初始化以及 32 位 ARM coherent 分配在取得连续页、
 * 完成必要 cache 准备后调用本函数；成功 KVA 最终发布给驱动或 gen_pool。
 * 【参数】
 * @page: 纯输入、借用的连续区域首页；调用方拥有从该页开始的足够连续物理页。
 * @size: 有效区域长度，单位为字节；实际映射页数向上取整，尾页也进入 KVA。
 * @prot: 新别名的页保护属性，决定 coherent、uncached、write-combine 或解密视图语义。
 * @caller: 调用点诊断地址；当前实现没有使用该参数，因而不影响映射或错误处理。
 * 必须在可睡眠进程上下文调用；入口不要求持锁，调用方在发布 KVA 前独占分配状态。
 * 【阶段】先分配临时 pages[]，按物理连续顺序填入每个 struct page，再由 vmap() 安装
 * PTE。vmap 只在建表期间借用该数组，所以无论成功失败都立即 kvfree 临时数组；物理页
 * 始终归调用方，局部 page++ 也只移动形参副本。
 * 【返回与副作用】数组分配或 vmap 失败返回 NULL，调用方仍负责释放原连续页；成功返回
 * 页对齐 KVA，但不保存 pages[]，也不取得物理页引用。调用方用
 * dma_common_free_remap() 销毁 KVA 后，再按原分配路径释放页面。
 */
void *dma_common_contiguous_remap(struct page *page, size_t size,
			pgprot_t prot, const void *caller)
{
	/* count 是覆盖 size 所需的整页数；pages 暂存逐页目录，i 是其构造游标。 */
	int count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	struct page **pages;
	void *vaddr;
	int i;

	/* kvmalloc_objs 允许大目录退化到 vmalloc；失败时尚未建立任何 KVA。 */
	pages = kvmalloc_objs(struct page *, count);
	if (!pages)
		return NULL;
	/* 连续物理页可由首页逐项递增恢复；这里不增加任何 page 引用。 */
	for (i = 0; i < count; i++)
		pages[i] = page++;
	/* vmap 完成后 PTE 已保存页面关系，不再需要临时数组本身。 */
	vaddr = vmap(pages, count, VM_DMA_COHERENT, prot);
	kvfree(pages);

	return vaddr;
}

/*
 * Unmaps a range previously mapped by dma_common_*_remap
 */
/*
 * 解除先前由 dma_common_*_remap 建立的虚拟映射范围。
 *
 * dma_common_free_remap() - 验证并销毁 DMA coherent KVA 别名
 *
 * 【宏观位置】dma-direct、DMA 原子池和 ARM coherent/IOMMU 回收路径在停止使用 CPU
 * 别名后调用本函数；返回后它们继续释放页面数组、IOVA 或物理页。
 * 【参数】
 * @cpu_addr: 输入、借用的映射起始 KVA，必须仍指向 VM_DMA_COHERENT 区域；函数成功时
 *            使该地址失效，调用方不得再解引用。
 * @size: 调用方记录的有效字节数；当前实现不使用它，vunmap() 根据 vm_struct 销毁整个
 *        区域，因此错误 size 不会造成部分解除，也不会被本函数检测。
 * 必须在可睡眠且非中断上下文调用；调用方必须先阻止并发 CPU/设备用户，并保证没有另一
 * 回收者同时摘除该区域。本函数不管理 DMA 设备侧 IOVA，也不替调用方执行 cache 同步。
 * 【阶段与返回】先用 find_vm_area() 验证地址确为 coherent remap，防止误拆普通 vmalloc
 * 区域；无效时告警并原样返回。有效时 vunmap() 摘除 KVA、拆页表并释放 vm_struct。
 * 【副作用与 ownership】无直接返回值。成功后只销毁虚拟视图；pages[] 与物理页引用从未
 * 转移给 vmap/vunmap，仍由上层随后释放。失败时地址和所有资源保持原状，告警提示配对错误。
 */
void dma_common_free_remap(void *cpu_addr, size_t size)
{
	/* area 是由仍存活映射保证有效的借用描述符，仅用于本次校验。 */
	struct vm_struct *area = find_vm_area(cpu_addr);

	/* 标志检查把本 helper 的销毁权限限制在自己创建的 DMA coherent KVA。 */
	if (!area || !(area->flags & VM_DMA_COHERENT)) {
		WARN(1, "trying to free invalid coherent area: %p\n", cpu_addr);
		return;
	}

	/* 不传 size：vunmap 按区域起始地址摘除并销毁完整映射，底层允许睡眠。 */
	vunmap(cpu_addr);
}
