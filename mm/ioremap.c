// SPDX-License-Identifier: GPL-2.0
/*
 * Re-map IO memory to kernel address space so that we can access it.
 * This is needed for high PCI addresses that aren't mapped in the
 * 640k-1MB IO memory area on PC's
 *
 * (C) Copyright 1995 1996 Linus Torvalds
 */
/*
 * 把设备物理 I/O 区间映射进内核虚拟地址空间，供驱动通过 __iomem 指针访问；
 * 这尤其服务于 PC 传统 640 KiB--1 MiB 窗口之外的高位 PCI 地址。映射只建立
 * CPU 页表与 vmalloc 区域，不替驱动管理设备资源所有权，也不允许普通内存解引用。
 */
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/export.h>
#include <linux/ioremap.h>

/*
 * generic_ioremap_prot() - 用指定页保护属性建立通用 I/O 虚拟映射。
 *
 * 业务背景：未提供专用实现的架构 ioremap_prot()/ioremap 包装层在选好缓存属性后
 * 调用这里；本函数负责校验物理区间、分配 VM_IOREMAP 地址段并创建页表映射。
 * 入参：phys_addr 是设备区间起始物理字节地址；size 是大于零的字节长度；prot 是
 * 架构构造的页保护/缓存属性值，三者均为纯输入，函数不会取得设备资源所有权。
 * 出参/返回：成功返回指向原始字节 offset 的 __iomem 借用句柄，调用者最终必须
 * iounmap；slab 未就绪、区间溢出/为空、虚拟区分配或页表建立失败均返回 NULL。
 * 注意事项：会走 vmalloc 和页表分配，可能睡眠，不能在早期启动或原子上下文调用；
 * 成功后映射对 CPU 可见，但设备访问顺序仍须使用 readl/writel 等 I/O accessor。
 */
void __iomem *generic_ioremap_prot(phys_addr_t phys_addr, size_t size,
				   pgprot_t prot)
{
	/* offset 保留首地址页内偏移；vaddr 是页对齐映射基址，last_addr 用于溢出检查。 */
	unsigned long offset, vaddr;
	phys_addr_t last_addr;
	struct vm_struct *area;

	/* An early platform driver might end up here */
	/* 早期平台驱动若误入，slab/vmalloc 尚不可用；告警一次并返回可处理的 NULL。 */
	if (WARN_ON_ONCE(!slab_is_available()))
		return NULL;

	/* Disallow wrap-around or zero size */
	/* 先形成包含式末地址；size==0 会下溢，末地址回绕则表示物理区间不可表示。 */
	last_addr = phys_addr + size - 1;
	if (!size || last_addr < phys_addr)
		return NULL;

	/* Page-align mappings */
	/*
	 * 页表只能映射整页：保存原起点的页内 offset，把物理起点向下对齐，并把
	 * 覆盖 offset 的总长度向上取整；成功出口再加回 offset 保持调用者视图。
	 */
	offset = phys_addr & (~PAGE_MASK);
	phys_addr -= offset;
	size = PAGE_ALIGN(size + offset);

	/* 在专用 ioremap 虚拟窗口预留区间；area 由当前函数持有，失败尚无资源。 */
	area = __get_vm_area_caller(size, VM_IOREMAP, IOREMAP_START,
				    IOREMAP_END, __builtin_return_address(0));
	if (!area)
		return NULL;
	vaddr = (unsigned long)area->addr;
	area->phys_addr = phys_addr;

	/*
	 * 把对齐后的物理页映射到刚预留的虚拟区。失败时页表 helper 已清理其部分工作，
	 * 当前层释放 vm_struct/地址区；成功后 area 由 vunmap 路径接管。
	 */
	if (ioremap_page_range(vaddr, vaddr + size, phys_addr, prot)) {
		free_vm_area(area);
		return NULL;
	}

	/* 发布原请求字节位置的 I/O 句柄；强制 __iomem 提醒调用者不能普通解引用。 */
	return (void __iomem *)(vaddr + offset);
}

#ifndef ioremap_prot
/*
 * ioremap_prot() - 为未覆盖该接口的架构提供导出的薄包装。
 * 业务背景：驱动传入架构 pgprot 后进入通用映射核心；有同名架构宏时本定义不编译。
 * 入参：phys_addr 为物理字节起点，size 为字节长度，prot 为映射属性，均纯输入。
 * 出参/返回：完全转交 generic_ioremap_prot() 的 __iomem 成功句柄或 NULL；成功句柄
 * 由调用者持有并须 iounmap。注意事项：继承核心的可睡眠、溢出和 early-boot 限制。
 */
void __iomem *ioremap_prot(phys_addr_t phys_addr, size_t size,
			   pgprot_t prot)
{
	return generic_ioremap_prot(phys_addr, size, prot);
}
EXPORT_SYMBOL(ioremap_prot);
#endif

/*
 * generic_iounmap() - 撤销通用 ioremap 创建的虚拟映射。
 * 业务背景：架构 iounmap 包装层把可能含页内 offset 的设备句柄交回这里；函数
 * 对齐到 vmalloc 区起点并让 vunmap 拆页表、释放 vm_struct 和虚拟地址范围。
 * 入参：addr 是调用者此前持有的有效 __iomem 映射句柄，允许含页内偏移；输入后
 * 仅借用用于定位，成功撤销后该句柄及其派生指针全部失效。
 * 出参/返回：无直接返回值；属于 ioremap 区域时撤销映射，否则无副作用。
 * 注意事项：vunmap 可能睡眠，不得在原子上下文；本函数不释放底层设备物理资源，
 * 调用者必须先停止并发 I/O，且不能把任意普通指针当作可安全释放的映射句柄。
 */
void generic_iounmap(volatile void __iomem *addr)
{
	/* ioremap 返回值可能偏离页首；PAGE_MASK 恢复 __get_vm_area 的真实基址。 */
	void *vaddr = (void *)((unsigned long)addr & PAGE_MASK);

	/* 只让 vmalloc 管理器处理通用 ioremap 窗口，避免误撤销其他地址种类。 */
	if (is_ioremap_addr(vaddr))
		vunmap(vaddr);
}

#ifndef iounmap
/*
 * iounmap() - 为未覆盖卸载接口的架构导出通用包装。
 * 业务背景：驱动资源释放路径调用它与成功 ioremap 配对。
 * 入参：addr 为此前映射得到的 __iomem 句柄；调用者负责保证无并发访问。
 * 出参/返回：无直接返回值；generic_iounmap() 可能撤销映射并使句柄失效。
 * 注意事项：继承可睡眠约束；架构定义 iounmap 宏时条件编译排除本包装。
 */
void iounmap(volatile void __iomem *addr)
{
	generic_iounmap(addr);
}
EXPORT_SYMBOL(iounmap);
#endif
