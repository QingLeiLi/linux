// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/nommu.c
 *
 *  Replacement code for mm functions to support CPU's that don't
 *  have any form of memory management unit (thus no virtual memory).
 *
 *  See Documentation/admin-guide/mm/nommu-mmap.rst
 *
 *  Copyright (c) 2004-2008 David Howells <dhowells@redhat.com>
 *  Copyright (c) 2000-2003 David McCullough <davidm@snapgear.com>
 *  Copyright (c) 2000-2001 D Jeff Dionne <jeff@uClinux.org>
 *  Copyright (c) 2002      Greg Ungerer <gerg@snapgear.com>
 *  Copyright (c) 2007-2010 Paul Mundt <lethal@linux-sh.org>
 */
/*
 * 本文件为没有 MMU 的体系结构提供内存管理公共接口的替代实现：用户地址
 * 不能依靠页表任意重排，mmap() 因而只能直接借用设备/页缓存中的连续地址，
 * 或分配一段物理连续内存保存私有副本。Documentation/admin-guide/mm/
 * nommu-mmap.rst 给出用户 ABI；这里则实现 region 共享、VMA 登记和回收协议。
 * 原文版权信息无需翻译，以上说明补足本文件在通用 mm 调用链中的位置。
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/* 公共 mm/VMA、进程地址空间、mmap ABI 与换页接口；NOMMU 仍复用其对象模型。 */
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/mman.h>
#include <linux/swap.h>
/* 文件、页缓存、slab/vmalloc 与后备设备接口支撑文件映射和私有副本分配。 */
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/backing-dev.h>
/* 编译器、挂载执行权限、personality、安全审计和系统调用入口辅助。 */
#include <linux/compiler.h>
#include <linux/mount.h>
#include <linux/personality.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/audit.h>
#include <linux/printk.h>

/* 用户拷贝/迭代器及体系结构缓存、TLB、mm 上下文钩子。 */
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>
#include <asm/mmu_context.h>
#include "internal.h"

unsigned long highest_memmap_pfn;
int heap_stack_gap = 0;

atomic_long_t mmap_pages_allocated;

/*
 * 全局实体地图：
 * highest_memmap_pfn 是体系结构初始化后可由 memmap 描述的最高 PFN；
 * heap_stack_gap 在 NOMMU 下保留通用接口所需的堆栈间隔值，初值 0 表示不额外留洞；
 * mmap_pages_allocated 统计私有复制映射实际占用的页数，由分配/释放路径原子更新；
 * vm_region_jar 是 region 元数据的 slab，mmap_init() 创建后存活至系统结束；
 * nommu_region_tree 按实际地址保存可共享 region，nommu_region_sem 同时串行化树结构、
 * region->vm_usage 以及共享 region 的创建/销毁，读写者不得把裸 region 指针带出保护期。
 */


/* list of mapped, potentially shareable regions */
/* 映射且可能共享的 region 列表；当前实现用下方红黑树按地址组织，而非线性链表。 */
static struct kmem_cache *vm_region_jar;
struct rb_root nommu_region_tree = RB_ROOT;
DECLARE_RWSEM(nommu_region_sem);

/*
 * generic_file_vm_ops 是 ramfs/shmem NOMMU mmap_prepare() 安装的空操作表：映射在
 * mmap 阶段已经直接可访问，无 fault/open/close 回调可做。文件系统持有静态表地址，
 * 对象存活至系统结束且无需同步；VMA 只借用该指针，典型场景是 ramfs 共享映射
 * 通过 ramfs_nommu_mmap_prepare() 选择它，随后由 do_mmap_shared_file() 完成 region。
 */
const struct vm_operations_struct generic_file_vm_ops = {
};

/*
 * Return the total memory allocated for this pointer, not
 * just what the caller asked for.
 *
 * Doesn't have to be accurate, i.e. may have races.
 */
/*
 * 返回该指针所属分配对象的总容量，而不是调用者最初请求的字节数。结果仅供
 * 诊断/容量估算，允许与并发释放或 VMA 改动竞争，调用者不能据此取得所有权。
 *
 * 业务背景：内核通用代码在不知道对象来自 slab、folio 还是 NOMMU VMA 时用
 * kobjsize() 查询可访问上界；本函数依次识别这些来源并选择相应容量接口。
 * 入参：objp 是只读借用的内核地址，可为 NULL；调用期间必须仍指向有效对象。
 * 出参/返回：返回容量字节数；无效地址返回 0，不增加 folio/VMA 引用，也不改变对象。
 * 注意事项：函数不加锁且结果可陈旧；current->mm 查询只适用于当前进程的 VMA，
 * 不能把返回值当作防止释放或越界访问的同步保证。
 */
unsigned int kobjsize(const void *objp)
{
	/* folio 只是由地址反查得到的借用指针，整个函数不会取得页引用。 */
	struct folio *folio;

	/*
	 * If the object we have should not have ksize performed on it,
	 * return size of 0
	 */
	/* 不可对 NULL 或非线性映射地址调用 ksize()；以 0 表示无法可靠识别。 */
	if (!objp || !virt_addr_valid(objp))
		return 0;

	folio = virt_to_folio(objp);

	/*
	 * If the allocator sets PageSlab, we know the pointer came from
	 * kmalloc().
	 */
	/* slab folio 明确证明来源是 kmalloc/slab，此时 ksize() 才有合法契约。 */
	if (folio_test_slab(folio))
		return ksize(objp);

	/*
	 * If it's not a large folio, see if we have a matching VMA
	 * region. This test is intentionally done in reverse order,
	 * so if there's no VMA, we still fall through and hand back
	 * PAGE_SIZE for 0-order folios.
	 */
	/*
	 * 普通 folio 可能承载 NOMMU 映射：若当前 mm 中存在对应 VMA，VMA 区间才是
	 * 业务对象容量；找不到时继续按 folio 大小返回，零阶 folio 即 PAGE_SIZE。
	 */
	if (!folio_test_large(folio)) {
		struct vm_area_struct *vma;

		vma = find_vma(current->mm, (unsigned long)objp);
		if (vma)
			return vma->vm_end - vma->vm_start;
	}

	/*
	 * The ksize() function is only guaranteed to work for pointers
	 * returned by kmalloc(). So handle arbitrary pointers here.
	 */
	/* 非 slab 地址最终按承载 folio 的总字节数回答，避免非法调用 ksize()。 */
	return folio_size(folio);
}

/*
 * 业务背景：通用 vmalloc 使用者通过 vfree() 释放内核缓冲区；NOMMU 没有独立
 * 虚拟映射层，__vmalloc_noprof() 实际由 kmalloc 分配，故释放也直接落到 kfree()。
 * 入参：addr 是 kmalloc/vmalloc 兼容接口返回的借用地址，可为 NULL；所有权在此释放。
 * 出参/返回：无直接返回值；非 NULL 对象在返回前已释放。
 * 注意事项：可能在 kfree() 允许的上下文调用；调用后不得再解引用 addr。
 */
void vfree(const void *addr)
{
	kfree(addr);
}
EXPORT_SYMBOL(vfree);

/*
 * 业务背景：为通用 vmalloc 调用链提供 NOMMU 分配后端；没有页表可拼接离散页，
 * 所以退化为一次物理连续的 kmalloc 分配。
 * 入参：size 为请求字节数；gfp_mask 为分配/回收上下文标志，__GFP_HIGHMEM 会被忽略。
 * 出参/返回：成功返回由调用者持有、须用 vfree()/kfree() 释放的地址，失败返回 NULL。
 * 注意事项：是否睡眠由 gfp_mask 决定；大对象受伙伴系统连续页可用性限制，语义不等同 MMU vmalloc。
 */
void *__vmalloc_noprof(unsigned long size, gfp_t gfp_mask)
{
	/*
	 *  You can't specify __GFP_HIGHMEM with kmalloc() since kmalloc()
	 * returns only a logical address.
	 */
	/*
	 * kmalloc() 只能返回内核可直接解引用的低端逻辑地址，因而必须剔除
	 * __GFP_HIGHMEM；__GFP_COMP 让多页复合分配可按一个对象管理。
	 */
	return kmalloc_noprof(size, (gfp_mask | __GFP_COMP) & ~__GFP_HIGHMEM);
}
EXPORT_SYMBOL(__vmalloc_noprof);

/*
 * 业务背景：krealloc/vrealloc 的通用包装需要在 NOMMU 上调整已有 vmalloc 风格对象，
 * 实际仍交给 slab/伙伴分配器的 krealloc_noprof()。
 * 入参：p 为原对象，可为 NULL且所有权仍由调用者持有至成功；size 为新字节数；align
 * 和 node 是兼容参数、此实现不保证指定对齐或 NUMA 节点；flags 是分配上下文标志。
 * 出参/返回：成功返回新对象并接管/释放旧对象，失败返回 NULL且旧对象仍有效。
 * 注意事项：剔除 __GFP_HIGHMEM；是否睡眠由 flags 决定，调用者须按 krealloc 规则处理返回值。
 */
void *vrealloc_node_align_noprof(const void *p, size_t size, unsigned long align,
				 gfp_t flags, int node)
{
	return krealloc_noprof(p, size, (flags | __GFP_COMP) & ~__GFP_HIGHMEM);
}

/*
 * 业务背景：需要限定虚拟区间、保护属性或节点的通用调用会进入本接口；NOMMU
 * 无虚拟地址分配器，故这些选址属性无法兑现，只保留大小和 GFP 分配语义。
 * 入参：size 为字节数；align/start/end 描述期望虚拟区间；gfp_mask 控制分配；
 * prot、vm_flags 描述映射属性；node/caller 用于 NUMA 与记账，后六者均不被本实现使用。
 * 出参/返回：同 __vmalloc_noprof()，成功地址归调用者，失败为 NULL。
 * 注意事项：调用者不得假定地址位于 [start,end)、满足 align/prot 或来自 node。
 */
void *__vmalloc_node_range_noprof(unsigned long size, unsigned long align,
		unsigned long start, unsigned long end, gfp_t gfp_mask,
		pgprot_t prot, unsigned long vm_flags, int node,
		const void *caller)
{
	return __vmalloc_noprof(size, gfp_mask);
}

/*
 * 业务背景：这是带对齐、节点和调用点信息的 vmalloc 节点包装；NOMMU 只能提供
 * 物理连续 kmalloc 对象，因此转交基础后端。
 * 入参：size 为字节数；gfp_mask 控制分配；align、node、caller 为兼容输入且被忽略。
 * 出参/返回：成功返回调用者持有的地址，失败返回 NULL，无其他输出。
 * 注意事项：不保证 NUMA 放置或额外对齐，睡眠能力取决于 gfp_mask。
 */
void *__vmalloc_node_noprof(unsigned long size, unsigned long align, gfp_t gfp_mask,
		int node, const void *caller)
{
	return __vmalloc_noprof(size, gfp_mask);
}

/*
 * 业务背景：用户可映射的 vmalloc 风格分配必须在对应 VMA 上标记 VM_USERMAP，
 * remap_vmalloc_range() 随后据此拒绝普通内核缓冲区。
 * 入参：size 为请求字节数；flags 为 GFP 标志，通常包含 __GFP_ZERO 防止数据泄漏。
 * 出参/返回：失败返回 NULL；成功返回调用者持有的缓冲区并给当前 mm 的 VMA 加标志。
 * 注意事项：分配后会取得 current->mm 的 mmap 写锁，故只能在可睡眠进程上下文调用；
 * 若未找到 VMA，仍返回内存但后续用户重映射会失败。
 */
static void *__vmalloc_user_flags(unsigned long size, gfp_t flags)
{
	/* ret 是新对象的拥有型指针；vma 只在 mmap 写锁内借用。 */
	void *ret;

	ret = __vmalloc(size, flags);
	if (ret) {
		struct vm_area_struct *vma;

		/* 写锁把查找与 VM_USERMAP 发布合并，避免并发 VMA 删除/调整。 */
		mmap_write_lock(current->mm);
		vma = find_vma(current->mm, (unsigned long)ret);
		if (vma)
			vm_flags_set(vma, VM_USERMAP);
		mmap_write_unlock(current->mm);
	}

	return ret;
}

/*
 * 业务背景：驱动申请可安全映射给用户态的零填充缓冲区时使用此公开入口。
 * 入参：size 为请求字节数，无地址/节点约束。
 * 出参/返回：成功返回归调用者所有的零填充地址，失败返回 NULL。
 * 注意事项：GFP_KERNEL 和 mmap 写锁均允许睡眠；释放使用 vfree()。
 */
void *vmalloc_user_noprof(unsigned long size)
{
	return __vmalloc_user_flags(size, GFP_KERNEL | __GFP_ZERO);
}
EXPORT_SYMBOL(vmalloc_user_noprof);

/*
 * 业务背景：通用代码需要从 vmalloc 风格地址取得承载页；NOMMU 地址本就直接映射物理页。
 * 入参：addr 是有效、页仍存活的借用内核地址，不可为无效指针。
 * 出参/返回：返回借用的 struct page，不增加引用，调用者不能无条件 put_page()。
 * 注意事项：对象必须来自可由 virt_to_page() 转换的线性映射。
 */
struct page *vmalloc_to_page(const void *addr)
{
	return virt_to_page(addr);
}
EXPORT_SYMBOL(vmalloc_to_page);

/*
 * 业务背景：需要把 vmalloc 风格地址写入 PFN 接口的调用者在 NOMMU 上直接转换承载页。
 * 入参：addr 是有效线性映射地址的借用指针。
 * 出参/返回：返回物理页帧号 PFN，不取得页引用且不改变映射。
 * 注意事项：页生命周期仍由原分配者保证，返回 PFN 后不能据此延长其有效期。
 */
unsigned long vmalloc_to_pfn(const void *addr)
{
	return page_to_pfn(virt_to_page(addr));
}
EXPORT_SYMBOL(vmalloc_to_pfn);

/*
 * 业务背景：/proc/kcore 等内核读取路径用迭代器从 vmalloc 风格区域取数据；
 * NOMMU 可直接从线性地址复制，不需逐页翻译。
 * 入参：iter 是输入输出迭代器，复制会推进位置；addr 是借用源地址；count 为字节数。
 * 出参/返回：返回实际复制字节数或短复制结果，iter 随之推进。
 * 注意事项：源区间须保持有效；溢出时截到地址空间末端，用户拷贝可能 fault/睡眠。
 */
long vread_iter(struct iov_iter *iter, const char *addr, size_t count)
{
	/* Don't allow overflow */
	/* 不允许 addr + count 回绕；发生回绕时只复制到地址空间末端。 */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;

	return copy_to_iter(addr, count, iter);
}

/*
 *	vmalloc  -  allocate virtually contiguous memory
 *
 *	@size:		allocation size
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator and map them into contiguous kernel virtual space.
 *
 *	For tight control over page level allocator and protection flags
 *	use __vmalloc() instead.
 */
/*
 * 分配至少覆盖 size 的页，并在 MMU 系统中映射为连续内核虚拟区；精细控制应
 * 使用 __vmalloc()。NOMMU 无法拼接离散页，本入口转成 GFP_KERNEL 连续分配。
 * 业务背景：普通可睡眠内核调用者用 vmalloc() 获取大缓冲区，本函数维持其 API。
 * 入参：size 为请求字节数。出参/返回：成功地址归调用者并须 vfree()，失败为 NULL。
 * 注意事项：允许睡眠；大对象因要求物理连续而比 MMU vmalloc 更容易失败。
 */
void *vmalloc_noprof(unsigned long size)
{
	return __vmalloc_noprof(size, GFP_KERNEL);
}
EXPORT_SYMBOL(vmalloc_noprof);

/*
 *	vmalloc_huge_node  -  allocate virtually contiguous memory, on a node
 *
 *	@size:		allocation size
 *	@gfp_mask:	flags for the page level allocator
 *	@node:          node to use for allocation or NUMA_NO_NODE
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator and map them into contiguous kernel virtual space.
 *
 *	Due to NOMMU implications the node argument and HUGE page attribute is
 *	ignored.
 */
/*
 * 原接口在指定节点分配覆盖 size 的虚拟连续区域，并允许大页优化；NOMMU 下
 * node 与“大页 vmalloc”属性均被忽略，仅保留 gfp_mask 的分配约束。
 * 业务背景：通用节点版调用者无需为无 MMU 配置增加分支。
 * 入参：size 为字节数；gfp_mask 为分配标志；node 为期望节点或 NUMA_NO_NODE。
 * 出参/返回：成功对象归调用者，失败为 NULL。注意事项：不保证节点放置。
 */
void *vmalloc_huge_node_noprof(unsigned long size, gfp_t gfp_mask, int node)
{
	return __vmalloc_noprof(size, gfp_mask);
}

/*
 *	vzalloc - allocate virtually contiguous memory with zero fill
 *
 *	@size:		allocation size
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator and map them into contiguous kernel virtual space.
 *	The memory allocated is set to zero.
 *
 *	For tight control over page level allocator and protection flags
 *	use __vmalloc() instead.
 */
/*
 * 原接口分配覆盖 size 的虚拟连续区域并清零；NOMMU 以 __GFP_ZERO 连续分配
 * 实现相同保密语义。业务背景：调用者需要返回前全零的普通内核缓冲区。
 * 入参：size 为请求字节数。出参/返回：成功地址归调用者，失败为 NULL。
 * 注意事项：GFP_KERNEL 允许睡眠；释放用 vfree()，大对象仍要求物理连续。
 */
void *vzalloc_noprof(unsigned long size)
{
	return __vmalloc_noprof(size, GFP_KERNEL | __GFP_ZERO);
}
EXPORT_SYMBOL(vzalloc_noprof);

/**
 * vmalloc_node - allocate memory on a specific node
 * @size:	allocation size
 * @node:	numa node
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 */
/*
 * 上述接口语义是在指定 NUMA 节点分配虚拟连续空间，精细控制则使用
 * __vmalloc()；NOMMU 不具备节点选址的 vmalloc 层，直接采用普通 vmalloc。
 * 业务背景：节点感知调用者借此维持跨配置 ABI。入参：size 为字节数，node
 * 是期望节点但在此被忽略。出参/返回：成功对象归调用者，失败为 NULL。
 * 注意事项：允许睡眠，不保证 NUMA 局部性，释放使用 vfree()。
 */
void *vmalloc_node_noprof(unsigned long size, int node)
{
	return vmalloc_noprof(size);
}
EXPORT_SYMBOL(vmalloc_node_noprof);

/**
 * vzalloc_node - allocate memory on a specific node with zero fill
 * @size:	allocation size
 * @node:	numa node
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 * The memory allocated is set to zero.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 */
/*
 * 上述接口在指定节点分配并清零虚拟连续空间；NOMMU 退化到普通 vzalloc，
 * 仍保证清零但不保证节点放置。业务背景：为节点感知且要求零填充的调用者
 * 保留统一入口。入参：size 为字节数，node 在此被忽略。出参/返回：成功对象
 * 归调用者，失败为 NULL。注意事项：允许睡眠，释放使用 vfree()。
 */
void *vzalloc_node_noprof(unsigned long size, int node)
{
	return vzalloc_noprof(size);
}
EXPORT_SYMBOL(vzalloc_node_noprof);

/**
 * vmalloc_32  -  allocate virtually contiguous memory (32bit addressable)
 *	@size:		allocation size
 *
 *	Allocate enough 32bit PA addressable pages to cover @size from the
 *	page level allocator and map them into contiguous kernel virtual space.
 */
/*
 * 原接口要求底层页可由 32 位物理地址寻址；NOMMU 实现直接使用 GFP_KERNEL，
 * 不另加 DMA 区约束。业务背景：旧设备/ABI 调用者通过此入口请求低地址缓冲区。
 * 入参：size 为字节数。出参/返回：成功地址归调用者，失败为 NULL。
 * 注意事项：当前实现并不额外保证 32 位可寻址性，允许睡眠，释放用 vfree()。
 */
void *vmalloc_32_noprof(unsigned long size)
{
	return __vmalloc_noprof(size, GFP_KERNEL);
}
EXPORT_SYMBOL(vmalloc_32_noprof);

/**
 * vmalloc_32_user - allocate zeroed virtually contiguous 32bit memory
 *	@size:		allocation size
 *
 * The resulting memory area is 32bit addressable and zeroed so it can be
 * mapped to userspace without leaking data.
 *
 * VM_USERMAP is set on the corresponding VMA so that subsequent calls to
 * remap_vmalloc_range() are permissible.
 */
/*
 * 原接口返回 32 位可寻址且清零的区域，防止映射到用户态时泄漏旧数据；同时在
 * 对应 VMA 设置 VM_USERMAP，使 remap_vmalloc_range() 获准。NOMMU 复用
 * vmalloc_user_noprof()，保留清零和授权语义。
 * 业务背景：驱动为 32 位用户映射准备共享缓冲区时调用。
 * 入参：size 为字节数。出参/返回：成功对象归调用者，失败为 NULL。
 * 注意事项：允许睡眠；本实现没有独立的低 4GiB 放置保证。
 */
void *vmalloc_32_user_noprof(unsigned long size)
{
	/*
	 * We'll have to sort out the ZONE_DMA bits for 64-bit,
	 * but for now this can simply use vmalloc_user() directly.
	 */
	/* 64 位 NOMMU 的 ZONE_DMA 约束尚未细分，当前仅复用用户可映射分配路径。 */
	return vmalloc_user_noprof(size);
}
EXPORT_SYMBOL(vmalloc_32_user_noprof);

/*
 * 业务背景：MMU 内核用 vmap() 把任意 page 数组拼成连续虚拟区；NOMMU 无页表
 * 无法实现该语义。入参：pages 为借用页数组，count 为页数，flags/prot 为映射属性。
 * 出参/返回：无正常返回，BUG() 终止错误调用；形式上的 NULL 不可达。
 * 注意事项：调用者必须以配置条件排除此入口，不能把它当作可恢复的失败。
 */
void *vmap(struct page **pages, unsigned int count, unsigned long flags, pgprot_t prot)
{
	BUG();
	return NULL;
}
EXPORT_SYMBOL(vmap);

/*
 * 业务背景：vunmap() 撤销 vmap 建立的虚拟映射；NOMMU 不会存在这种映射。
 * 入参：addr 是形式上的借用映射地址。出参/返回：无直接返回值，BUG() 不返回。
 * 注意事项：真实调用表示上层遗漏 CONFIG_MMU 限制，应在调用点修正。
 */
void vunmap(const void *addr)
{
	BUG();
}
EXPORT_SYMBOL(vunmap);

/*
 * 业务背景：vm_map_ram() 快速把 page 数组映射为连续虚拟地址；NOMMU 无法拼接
 * 离散物理页。入参：pages/count 描述借用页数组，node 是期望分配节点。
 * 出参/返回：BUG() 终止，NULL 仅满足签名。注意事项：不可作为可探测功能调用。
 */
void *vm_map_ram(struct page **pages, unsigned int count, int node)
{
	BUG();
	return NULL;
}
EXPORT_SYMBOL(vm_map_ram);

/*
 * 业务背景：对应 vm_map_ram() 的撤销接口在 NOMMU 上同样不可成立。
 * 入参：mem 为映射地址，count 为页数，均只作 ABI 占位。出参/返回：BUG() 不返回。
 * 注意事项：调用者须在编译期排除此路径，而非期待运行时容错。
 */
void vm_unmap_ram(const void *mem, unsigned int count)
{
	BUG();
}
EXPORT_SYMBOL(vm_unmap_ram);

/*
 * 业务背景：MMU 实现会刷新可能残留的 vmalloc 别名；NOMMU 只有直接映射，不会
 * 产生此类别名。入参：无。出参/返回：无返回、无副作用。
 * 注意事项：这是通用调用链所需的安全空桩，可在任意不依赖额外同步的上下文调用。
 */
void vm_unmap_aliases(void)
{
}
EXPORT_SYMBOL_GPL(vm_unmap_aliases);

/*
 * 业务背景：free_vm_area() 释放 MMU vmalloc 区描述符；NOMMU 从不创建该对象。
 * 入参：area 是形式上的拥有型指针。出参/返回：BUG() 终止，不会释放任何对象。
 * 注意事项：到达这里说明上层错误地假定存在独立 vmalloc 虚拟区。
 */
void free_vm_area(struct vm_struct *area)
{
	BUG();
}
EXPORT_SYMBOL_GPL(free_vm_area);

/*
 * 业务背景：驱动在 fault 型 VMA 中用 vm_insert_page() 安装单页 PTE；NOMMU 没有
 * PTE 可安装。入参：vma/addr/page 均为借用，ownership 不变。
 * 出参/返回：固定 -EINVAL，表示该映射模型无效。注意事项：无锁和资源副作用。
 */
int vm_insert_page(struct vm_area_struct *vma, unsigned long addr,
		   struct page *page)
{
	return -EINVAL;
}
EXPORT_SYMBOL(vm_insert_page);

/*
 * 业务背景：批量 vm_insert_pages() 同样依赖页表，NOMMU 只能拒绝。
 * 入参：vma、pages 为借用；addr 为用户地址；num 指向待插入页数且不会被修改。
 * 出参/返回：固定 -EINVAL，所有输入 ownership 不变。注意事项：调用者应改用直接映射能力。
 */
int vm_insert_pages(struct vm_area_struct *vma, unsigned long addr,
			struct page **pages, unsigned long *num)
{
	return -EINVAL;
}
EXPORT_SYMBOL(vm_insert_pages);

/*
 * 业务背景：vm_map_pages() 为 VMA 批量建立 page 数组映射；该操作要求 MMU。
 * 入参：vma/pages 为借用，num 为页数。出参/返回：固定 -EINVAL，无部分成功。
 * 注意事项：不会获取页引用或改变 VMA，失败后资源仍归调用者。
 */
int vm_map_pages(struct vm_area_struct *vma, struct page **pages,
			unsigned long num)
{
	return -EINVAL;
}
EXPORT_SYMBOL(vm_map_pages);

/*
 * 业务背景：vm_map_pages_zero() 是从 VMA 零偏移批量建页表的便利接口；NOMMU
 * 无法实施。入参：vma/pages 为借用，num 为页数。出参/返回：固定 -EINVAL。
 * 注意事项：无状态变化、无引用转移，调用者必须选择 NOMMU 直接映射方案。
 */
int vm_map_pages_zero(struct vm_area_struct *vma, struct page **pages,
				unsigned long num)
{
	return -EINVAL;
}
EXPORT_SYMBOL(vm_map_pages_zero);

/*
 *  sys_brk() for the most part doesn't need the global kernel
 *  lock, except when an application is doing something nasty
 *  like trying to un-brk an area that has already been mapped
 *  to a regular file.  in this case, the unmapping will need
 *  to invoke file system routines that need the global lock.
 */
/*
 * sys_brk() 大多数时候无需旧式全局内核锁；历史上只有应用反向越过已映射文件等
 * 异常操作才会触发需要文件系统参与的取消映射。当前 NOMMU 实现把可用堆范围在
 * exec 时预先固定为 [start_brk, context.end_brk]，这里只移动逻辑末端。
 *
 * 业务背景：用户态 malloc 通过 brk 系统调用扩缩进程堆；这里不建立/拆除页表，
 * 只在预分配边界内发布 mm->brk，并在扩张前维护指令缓存一致性。
 * 入参：brk 是期望的新堆末端用户地址。出参/返回：始终返回实际堆末端；越界时
 * 保持旧值而非返回负 errno。注意事项：current->mm 由当前任务稳定持有；本路径
 * 不取得 mmap_lock，依赖 NOMMU exec 预设范围且同一 mm 的 brk 更新由 syscall 语义串行化。
 */
SYSCALL_DEFINE1(brk, unsigned long, brk)
{
	/* mm 是 current 生命周期内的借用指针；start/end_brk 构成不可越过的预留边界。 */
	struct mm_struct *mm = current->mm;

	/* 越界请求和重复请求都保持现状，符合 brk(2) 返回当前 break 的 ABI。 */
	if (brk < mm->start_brk || brk > mm->context.end_brk)
		return mm->brk;

	if (mm->brk == brk)
		return mm->brk;

	/*
	 * Always allow shrinking brk
	 */
	/* 收缩只减小逻辑可用范围，NOMMU 不能在此回收预留区中的独立页。 */
	if (brk <= mm->brk) {
		mm->brk = brk;
		return brk;
	}

	/*
	 * Ok, looks good - let it rip.
	 */
	/* 扩张发布前刷新新增区间的 I-cache，随后赋值即成为用户可观察的新 break。 */
	flush_icache_user_range(mm->brk, brk);
	return mm->brk = brk;
}

/* 私有复制映射尾部至少浪费这么多页时改用精确页数；sysctl 可在运行期调节。 */
static int sysctl_nr_trim_pages = CONFIG_NOMMU_INITIAL_TRIM_EXCESS;

/*
 * sysctl 字段清单：procname 暴露 /proc/sys/vm/nr_trim_pages；data 指向上述整数；
 * maxlen 是读写宽度；0644 允许特权写入；proc_dointvec_minmax 解析整数并用
 * extra1=SYSCTL_ZERO 约束下界为 0。表在 mmap_init() 注册后由 sysctl 核心只读遍历。
 */
static const struct ctl_table nommu_table[] = {
	{
		.procname	= "nr_trim_pages",
		.data		= &sysctl_nr_trim_pages,
		.maxlen		= sizeof(sysctl_nr_trim_pages),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
	},
};

/*
 * initialise the percpu counter for VM and region record slabs, initialise VMA
 * state.
 */
/*
 * 初始化 VM/region slab 所需的 per-CPU 计数器并建立 VMA 全局状态。
 * 业务背景：早期 mm 初始化调用本函数，为后续 mmap/overcommit/region 分配准备基础设施。
 * 入参：无。出参/返回：无直接返回值；发布 vm_committed_as、vm_region_jar、sysctl
 * 和 VMA 状态。注意事项：仅在 __init 阶段调用、允许睡眠且无并发 mmap；计数器或
 * slab 创建失败视为启动期致命错误，KMEM_CACHE(SLAB_PANIC) 不返回可恢复错误。
 */
void __init mmap_init(void)
{
	/* ret 只承接计数器初始化 errno；成功后该计数器存活至系统结束。 */
	int ret;

	/* 按依赖顺序先建统计，再建元数据缓存，最后向 sysctl/VMA 框架发布入口。 */
	ret = percpu_counter_init(&vm_committed_as, 0, GFP_KERNEL);
	VM_BUG_ON(ret);
	vm_region_jar = KMEM_CACHE(vm_region, SLAB_PANIC|SLAB_ACCOUNT);
	register_sysctl_init("vm", nommu_table);
	vma_state_init();
}

/*
 * validate the region tree
 * - the caller must hold the region lock
 */
/*
 * 校验全局 region 树；调用者必须已经持有 nommu_region_sem，避免遍历期间结构变化。
 * 业务背景：debug 配置在插入/删除边界检查地址区间不重叠且 end/top 顺序合法。
 * 入参：无。出参/返回：无；发现损坏以 BUG_ON() 停机，空树直接成功返回。
 * 注意事项：只读借用树节点，不取得 region 引用；noinline 保留清晰的调试栈。
 */
#ifdef CONFIG_DEBUG_NOMMU_REGIONS
static noinline void validate_nommu_regions(void)
{
	/* lastp/last 保存前一区间，p/region 保存当前区间，用于验证相邻不重叠。 */
	struct vm_region *region, *last;
	struct rb_node *p, *lastp;

	lastp = rb_first(&nommu_region_tree);
	if (!lastp)
		return;

	/* 首节点先验证自身区间：start < end <= top。 */
	last = rb_entry(lastp, struct vm_region, vm_rb);
	BUG_ON(last->vm_end <= last->vm_start);
	BUG_ON(last->vm_top < last->vm_end);

	/* 中序遍历同时验证每个节点自身及 current.start >= previous.top。 */
	while ((p = rb_next(lastp))) {
		region = rb_entry(p, struct vm_region, vm_rb);
		last = rb_entry(lastp, struct vm_region, vm_rb);

		BUG_ON(region->vm_end <= region->vm_start);
		BUG_ON(region->vm_top < region->vm_end);
		BUG_ON(region->vm_start < last->vm_top);

		lastp = p;
	}
}
#else
/*
 * 业务背景：关闭 CONFIG_DEBUG_NOMMU_REGIONS 时保留同名空桩，使修改路径无需条件分支。
 * 入参：无。出参/返回：无且无副作用。注意事项：调用者仍须遵守 region 锁协议，
 * 只是生产配置不执行昂贵的全树不变量检查。
 */
static void validate_nommu_regions(void)
{
}
#endif

/*
 * add a region into the global tree
 */
/*
 * 把一个 region 加入全局树。业务背景：do_mmap() 完成地址/后备存储构造后调用，
 * 使后续共享映射可按 vm_start 找到它。
 * 入参：region 是调用者持有的已初始化对象，要求 start < end <= top；ownership
 * 不转移，树只嵌入其 vm_rb 节点。出参/返回：无；成功后 region 对共享查找可见。
 * 注意事项：调用者必须持 nommu_region_sem 写锁；相同对象重复加入幂等，地址相同
 * 但对象不同说明不变量破坏并触发 BUG()。
 */
static void add_nommu_region(struct vm_region *region)
{
	/* pregion 是比较用借用对象；p 指向待链接子槽，parent 是其父节点。 */
	struct vm_region *pregion;
	struct rb_node **p, *parent;

	validate_nommu_regions();

	/* 阶段一：按起始地址下降，确定唯一插入槽并拒绝地址键冲突。 */
	parent = NULL;
	p = &nommu_region_tree.rb_node;
	while (*p) {
		parent = *p;
		pregion = rb_entry(parent, struct vm_region, vm_rb);
		/* 较小键走左、较大键走右；同地址只允许同一个已链接对象。 */
		if (region->vm_start < pregion->vm_start)
			p = &(*p)->rb_left;
		else if (region->vm_start > pregion->vm_start)
			p = &(*p)->rb_right;
		else if (pregion == region)
			return;
		else
			BUG();
	}

	/* 阶段二：先链接再着色平衡；此后共享查找者可在锁保护下看到新节点。 */
	rb_link_node(&region->vm_rb, parent, p);
	rb_insert_color(&region->vm_rb, &nommu_region_tree);

	validate_nommu_regions();
}

/*
 * delete a region from the global tree
 */
/*
 * 从全局共享树摘除 region。业务背景：最后引用释放或 VMA 分裂/收缩重定位前调用，
 * 防止查找者继续命中即将释放或改键的节点。
 * 入参：region 是树内借用对象，调用者仍负责其最终释放。出参/返回：无；返回后
 * 新查找者不可见该节点。注意事项：须持 nommu_region_sem 写锁，空树属于内核错误。
 */
static void delete_nommu_region(struct vm_region *region)
{
	BUG_ON(!nommu_region_tree.rb_node);

	validate_nommu_regions();
	rb_erase(&region->vm_rb, &nommu_region_tree);
	validate_nommu_regions();
}

/*
 * free a contiguous series of pages
 */
/*
 * 释放 [from,to) 的物理连续页序列。业务背景：私有复制映射由 alloc_pages_exact()
 * 建立，region 最后引用或收缩时在这里逐页归还并同步全局统计。
 * 入参：from/to 为页对齐内核地址，to 不含在内且 from <= to。
 * 出参/返回：无；每页引用与 mmap_pages_allocated 各减少一次。
 * 注意事项：调用者必须保证这些页归该 VM_MAPPED_COPY region 独占，释放后地址失效。
 */
static void free_page_series(unsigned long from, unsigned long to)
{
	/* page 是当前迭代地址对应的借用描述符；put_page() 消费 region 持有的页引用。 */
	for (; from < to; from += PAGE_SIZE) {
		struct page *page = virt_to_page((void *)from);

		atomic_long_dec(&mmap_pages_allocated);
		put_page(page);
	}
}

/*
 * release a reference to a region
 * - the caller must hold the region semaphore for writing, which this releases
 * - the region may not have been added to the tree yet, in which case vm_top
 *   will equal vm_start
 */
/*
 * 释放一个 region 引用；调用者必须持有 nommu_region_sem 写锁，本函数无论引用
 * 是否归零都会释放该锁。尚未入树的构造失败对象以 vm_top == vm_start 标识。
 * 业务背景：VMA 销毁把共享后备对象的最后引用交给这里，先摘除发布关系，再在锁外
 * 执行可能较重的 fput/页释放。入参：region 为拥有一个待释放引用的非空指针。
 * 出参/返回：无；非末引用只减计数，末引用还释放 file、私有页和元数据。
 * 注意事项：__releases 标注要求调用者不得再次 up_write；vm_usage 受同一写锁保护。
 */
static void __put_nommu_region(struct vm_region *region)
	__releases(nommu_region_sem)
{
	BUG_ON(!nommu_region_tree.rb_node);

	/* 先在锁内领取“最后释放者”身份，并在需要时从共享查找树摘除。 */
	if (--region->vm_usage == 0) {
		if (region->vm_top > region->vm_start)
			delete_nommu_region(region);
		/* 摘除后即可解锁；之后没有新 VMA 能取得此 region 引用。 */
		up_write(&nommu_region_sem);

		if (region->vm_file)
			fput(region->vm_file);

		/* IO memory and memory shared directly out of the pagecache
		 * from ramfs/tmpfs mustn't be released here */
		/*
		 * I/O 内存以及 ramfs/tmpfs 直接从页缓存共享的内存不归 region 所有，不能
		 * 在此释放；只有 VM_MAPPED_COPY 标记的私有副本页由本对象负责回收。
		 */
		if (region->vm_flags & VM_MAPPED_COPY)
			free_page_series(region->vm_start, region->vm_top);
		kmem_cache_free(vm_region_jar, region);
	} else {
		/* 仍有 VMA 共享该 region，仅结束本次引用释放并交还写锁。 */
		up_write(&nommu_region_sem);
	}
}

/*
 * release a reference to a region
 */
/*
 * 释放 region 引用的可直接调用包装。业务背景：delete_vma() 未持 region 锁，
 * 通过本函数进入上述“计数、摘除、最终释放”协议。
 * 入参：region 为拥有一个待释放引用的非空对象。出参/返回：无，引用被消费。
 * 注意事项：会取得可睡眠写信号量；返回后 region 可能已经释放，禁止继续访问。
 */
static void put_nommu_region(struct vm_region *region)
{
	down_write(&nommu_region_sem);
	__put_nommu_region(region);
}

/*
 * 业务背景：新 VMA 写入 mm 的 Maple Tree 前，先绑定所属 mm，并把文件映射加入
 * address_space 的 i_mmap 区间树，供 truncate/反向映射查询。
 * 入参：vma 是待发布对象，mm 是借用目标地址空间；二者生命周期由上层保证。
 * 出参/返回：无；设置 vm_mm，文件 VMA 还会发布到 mapping->i_mmap。
 * 注意事项：调用者持 mm 写锁；本函数另取 i_mmap 写锁，dcache flush 钩子包围树变更。
 */
static void setup_vma_to_mm(struct vm_area_struct *vma, struct mm_struct *mm)
{
	vma->vm_mm = mm;

	/* add the VMA to the mapping */
	/* 文件 VMA 加入 mapping 反向索引；匿名 VMA 没有此步骤。 */
	if (vma->vm_file) {
		struct address_space *mapping = vma->vm_file->f_mapping;

		i_mmap_lock_write(mapping);
		flush_dcache_mmap_lock(mapping);
		vma_interval_tree_insert(vma, &mapping->i_mmap);
		flush_dcache_mmap_unlock(mapping);
		i_mmap_unlock_write(mapping);
	}
}

/*
 * 业务背景：删除 VMA 前撤销 map_count 和文件 address_space 反向索引，阻止
 * truncate 等路径继续发现它；mm Maple Tree 的摘除由调用者另行完成。
 * 入参：vma 是仍绑定 vm_mm 的借用对象。出参/返回：无；map_count 减一，文件
 * VMA 从 i_mmap 摘除。注意事项：调用者持 mm 写锁，本函数内部串行化 mapping 树。
 */
static void cleanup_vma_from_mm(struct vm_area_struct *vma)
{
	vma->vm_mm->map_count--;
	/* remove the VMA from the mapping */
	/* 与 setup_vma_to_mm() 配对，先摘除反向索引再释放 file/VMA。 */
	if (vma->vm_file) {
		struct address_space *mapping;
		mapping = vma->vm_file->f_mapping;

		i_mmap_lock_write(mapping);
		flush_dcache_mmap_lock(mapping);
		vma_interval_tree_remove(vma, &mapping->i_mmap);
		flush_dcache_mmap_unlock(mapping);
		i_mmap_unlock_write(mapping);
	}
}

/*
 * delete a VMA from its owning mm_struct and address space
 */
/*
 * 从所属 mm 与文件 address_space 的索引中摘除 VMA，但暂不销毁对象。
 * 业务背景：do_munmap() 必须先完成可能失败的 Maple Tree 预分配，再跨过不可回滚
 * 的摘除边界。入参：vma 是仍在 mm 树中的借用对象。
 * 出参/返回：0 表示索引均已摘除；-ENOMEM 表示预分配失败且 VMA 保持原状。
 * 注意事项：调用者持 mm mmap 写锁；成功后必须紧接 delete_vma() 完成资源释放。
 */
static int delete_vma_from_mm(struct vm_area_struct *vma)
{
	/* vmi 在 VMA 起点定位；预分配保证后续 clear 不再因内存不足中途失败。 */
	VMA_ITERATOR(vmi, vma->vm_mm, vma->vm_start);

	vma_iter_config(&vmi, vma->vm_start, vma->vm_end);
	if (vma_iter_prealloc(&vmi, NULL)) {
		pr_warn("Allocation of vma tree for process %d failed\n",
		       current->pid);
		return -ENOMEM;
	}
	cleanup_vma_from_mm(vma);

	/* remove from the MM's tree and list */
	/* 从 mm Maple Tree 摘除是对地址查找者的最终不可见发布点。 */
	vma_iter_clear(&vmi);
	return 0;
}
/*
 * destroy a VMA record
 */
/*
 * 销毁一个已从所有索引摘除的 VMA。业务背景：munmap/exit_mmap 在不可见后依次
 * 通知驱动、释放文件引用、region 引用和 VMA 元数据。
 * 入参：mm 是兼容参数、当前实现不读取；vma 的 ownership 转入本函数。
 * 出参/返回：无；返回时 vma 已释放，region 也可能因最后引用归零而释放。
 * 注意事项：vma_close()/fput()/信号量路径可能睡眠；不得对仍在 mm 树中的 VMA 调用。
 */
static void delete_vma(struct mm_struct *mm, struct vm_area_struct *vma)
{
	/* 释放顺序保留回调可见的 file/region，直到 vma_close() 完成。 */
	vma_close(vma);
	if (vma->vm_file)
		fput(vma->vm_file);
	put_nommu_region(vma->vm_region);
	vm_area_free(vma);
}

/*
 * 业务背景：区间冲突检查需要查找与 [start_addr,end_addr) 相交的首个 VMA；
 * NOMMU 复用 mm Maple Tree 的范围查询。
 * 入参：mm 为借用地址空间；start_addr/end_addr 为半开用户地址区间，须满足 end>start。
 * 出参/返回：返回借用 VMA 或 NULL，不增加引用。注意事项：调用者至少持 mmap 读锁，
 * 裸返回指针只在该保护/其他生命周期保证内有效。
 */
struct vm_area_struct *find_vma_intersection(struct mm_struct *mm,
					     unsigned long start_addr,
					     unsigned long end_addr)
{
	unsigned long index = start_addr;

	mmap_assert_locked(mm);
	return mt_find(&mm->mm_mt, &index, end_addr - 1);
}
EXPORT_SYMBOL(find_vma_intersection);

/*
 * look up the first VMA in which addr resides, NULL if none
 * - should be called with mm->mmap_lock at least held readlocked
 */
/*
 * 查找实际包含 addr 的 VMA；没有则返回 NULL。这里使用 vma_iter_load()，不同于
 * MMU 版本常见的“addr 之后首个 VMA”语义。调用者至少应持有
 * mm->mmap_lock 读锁，确保 Maple Tree 与返回对象生命周期稳定。
 * 业务背景：kobjsize、远程访问和映射管理用它把地址解析到 VMA。
 * 入参：mm 为借用地址空间，addr 为用户/线性地址。出参/返回：借用 VMA 或 NULL。
 * 注意事项：不取得 VMA 引用，解锁后不能继续使用裸指针。
 */
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
	VMA_ITERATOR(vmi, mm, addr);

	return vma_iter_load(&vmi);
}
EXPORT_SYMBOL(find_vma);

/*
 * expand a stack to a given address
 * - not supported under NOMMU conditions
 */
/*
 * 把栈 VMA 扩到 addr；NOMMU 没有按需页表扩栈，固定拒绝。
 * 业务背景：通用 fault 路径在地址落到 grows-down 栈边界时调用此锁内版本。
 * 入参：vma/addr 为借用目标及期望边界。出参/返回：固定 -ENOMEM，无状态变化。
 * 注意事项：调用者持 mmap 锁；返回后应按缺页/访问失败处理。
 */
int expand_stack_locked(struct vm_area_struct *vma, unsigned long addr)
{
	return -ENOMEM;
}

/*
 * 业务背景：通用无锁包装期望在尝试扩栈后交还 mmap 读锁；NOMMU 不能扩栈，
 * 因此只履行解锁契约并报告失败。
 * 入参：mm 为当前持有读锁的借用地址空间；addr 是未使用的期望边界。
 * 出参/返回：固定 NULL，并释放调用者持有的 mmap 读锁。
 * 注意事项：与普通 getter 不同，本函数有解锁副作用，调用者不得再次解锁。
 */
struct vm_area_struct *expand_stack(struct mm_struct *mm, unsigned long addr)
{
	mmap_read_unlock(mm);
	return NULL;
}

/*
 * look up the first VMA exactly that exactly matches addr
 * - should be called with mm->mmap_lock at least held readlocked
 */
/*
 * 查找起点恰为 addr、长度恰为 len 的 VMA；调用者至少持 mmap 读锁。
 * 业务背景：mremap 必须确认用户描述的是完整单一映射，不能误改相邻或子区间。
 * 入参：mm 为借用地址空间；addr/len 组成待匹配半开区间，len 以字节计。
 * 出参/返回：精确匹配时返回借用 VMA，否则 NULL；不取得引用。
 * 注意事项：addr+len 由上层完成溢出/对齐约束，返回指针仅在锁保护内有效。
 */
static struct vm_area_struct *find_vma_exact(struct mm_struct *mm,
					     unsigned long addr,
					     unsigned long len)
{
	struct vm_area_struct *vma;
	unsigned long end = addr + len;
	VMA_ITERATOR(vmi, mm, addr);

	vma = vma_iter_load(&vmi);
	if (!vma)
		return NULL;
	/* 必须同时匹配两端；只包含 addr 或长度相同都不足以授权 mremap 修改。 */
	if (vma->vm_start != addr)
		return NULL;
	if (vma->vm_end != end)
		return NULL;

	return vma;
}

/*
 * determine whether a mapping should be permitted and, if so, what sort of
 * mapping we're capable of supporting
 */
/*
 * 判定 mmap 请求是否允许，并计算 NOMMU 后端实际能提供的映射能力集合。
 * 业务背景：do_mmap() 在分配任何 VMA/region 前调用本函数，把文件类型、驱动
 * 回调、打开权限、MAP_SHARED/PRIVATE 与执行策略收敛成 NOMMU_MAP_* 位图。
 * 入参：file 为可空借用文件，NULL 表示匿名映射；addr 为用户提示地址；len 为
 * 字节长度；prot 是 PROT_* 权限；flags 是 MAP_* 策略；pgoff 是页单位文件偏移；
 * _capabilities 是非空输出指针，失败时内容未定义、成功时写入可用能力位。
 * 出参/返回：0 并填充能力位表示可继续；-EINVAL/-ENOMEM/-EOVERFLOW/-ENODEV/
 * -EACCES/-EPERM 或安全钩子 errno 表示拒绝，输入对象 ownership 均不变。
 * 注意事项：不持 mmap/region 锁且不分配内存；file 必须在调用期间保持引用稳定，
 * 能力判断不发布映射，最终 VMA 标志仍由 determine_vm_flags() 生成。
 */
static int validate_mmap_request(struct file *file,
				 unsigned long addr,
				 unsigned long len,
				 unsigned long prot,
				 unsigned long flags,
				 unsigned long pgoff,
				 unsigned long *_capabilities)
{
	/* capabilities 是逐层删减的能力位；rlen 是页对齐长度；ret 承接安全钩子 errno。 */
	unsigned long capabilities, rlen;
	int ret;

	/* do the simple checks first */
	/* 先拒绝 NOMMU 无法兑现的固定选址、非法共享类型和零长度请求。 */
	if (flags & MAP_FIXED)
		return -EINVAL;

	if ((flags & MAP_TYPE) != MAP_PRIVATE &&
	    (flags & MAP_TYPE) != MAP_SHARED)
		return -EINVAL;

	if (!len)
		return -EINVAL;

	/* Careful about overflows.. */
	/* PAGE_ALIGN 后为 0 表示回绕；超过 TASK_SIZE 也没有合法用户地址空间可容纳。 */
	rlen = PAGE_ALIGN(len);
	if (!rlen || rlen > TASK_SIZE)
		return -ENOMEM;

	/* offset overflow? */
	/* pgoff 加上映射页数不得回绕，否则文件区间检查会被绕过。 */
	if ((pgoff + (rlen >> PAGE_SHIFT)) < pgoff)
		return -EOVERFLOW;

	if (file) {
		/* files must support mmap */
		/* 文件必须声明可 mmap；否则不能从普通 read 能力推导映射语义。 */
		if (!can_mmap_file(file))
			return -ENODEV;

		/* work out if what we've got could possibly be shared
		 * - we support chardevs that provide their own "memory"
		 * - we support files/blockdevs that are memory backed
		 */
		/*
		 * 先取得文件可提供的能力：字符设备可暴露自身物理内存；普通文件/块设备
		 * 默认只能复制到新内存。驱动回调给出的显式集合优先于按 inode 类型推断。
		 */
		if (file->f_op->mmap_capabilities) {
			capabilities = file->f_op->mmap_capabilities(file);
		} else {
			/* no explicit capabilities set, so assume some
			 * defaults */
			/* 无显式能力回调时仅为已知 inode 类型赋保守默认值。 */
			switch (file_inode(file)->i_mode & S_IFMT) {
			case S_IFREG:
			case S_IFBLK:
				/* 普通文件/块设备通过私有副本满足映射，不能假定可直接共享。 */
				capabilities = NOMMU_MAP_COPY;
				break;

			case S_IFCHR:
				/* 字符设备默认允许驱动直接映射并提供读写，执行仍需显式声明。 */
				capabilities =
					NOMMU_MAP_DIRECT |
					NOMMU_MAP_READ |
					NOMMU_MAP_WRITE;
				break;

			default:
				/* 目录、socket 等没有 NOMMU 映射模型，立即拒绝。 */
				return -EINVAL;
			}
		}

		/* eliminate any capabilities that we can't support on this
		 * device */
		/* 缺少选址回调便无法直接映射；不可读文件也无法制作私有副本。 */
		if (!file->f_op->get_unmapped_area)
			capabilities &= ~NOMMU_MAP_DIRECT;
		if (!(file->f_mode & FMODE_CAN_READ))
			capabilities &= ~NOMMU_MAP_COPY;

		/* The file shall have been opened with read permission. */
		/* 不论最终直接映射还是复制，mmap ABI 都要求该 fd 以读权限打开。 */
		if (!(file->f_mode & FMODE_READ))
			return -EACCES;

		if (flags & MAP_SHARED) {
			/* do checks for writing, appending and locking */
			/* 共享可写映射会修改后备对象，必须具有写打开权限且不能违反 append-only。 */
			if ((prot & PROT_WRITE) &&
			    !(file->f_mode & FMODE_WRITE))
				return -EACCES;

			if (IS_APPEND(file_inode(file)) &&
			    (file->f_mode & FMODE_WRITE))
				return -EACCES;

			if (!(capabilities & NOMMU_MAP_DIRECT))
				return -ENODEV;

			/* we mustn't privatise shared mappings */
			/* MAP_SHARED 承诺修改对后备对象可见，绝不能静默退化成私有副本。 */
			capabilities &= ~NOMMU_MAP_COPY;
		} else {
			/* we're going to read the file into private memory we
			 * allocate */
			/* MAP_PRIVATE 至少需要 COPY 能力，失败时不能改为共享直接映射。 */
			if (!(capabilities & NOMMU_MAP_COPY))
				return -ENODEV;

			/* we don't permit a private writable mapping to be
			 * shared with the backing device */
			/* 私有可写必须隔离修改，故删除 DIRECT 候选，强制后续制作副本。 */
			if (prot & PROT_WRITE)
				capabilities &= ~NOMMU_MAP_DIRECT;
		}

		/* 逐个核对请求权限；直接映射能力不足时只能尝试 COPY 或拒绝共享请求。 */
		if (capabilities & NOMMU_MAP_DIRECT) {
			if (((prot & PROT_READ)  && !(capabilities & NOMMU_MAP_READ))  ||
			    ((prot & PROT_WRITE) && !(capabilities & NOMMU_MAP_WRITE)) ||
			    ((prot & PROT_EXEC)  && !(capabilities & NOMMU_MAP_EXEC))
			    ) {
				capabilities &= ~NOMMU_MAP_DIRECT;
				if (flags & MAP_SHARED) {
					/* 共享映射不能以副本补救，向调用者明确报告该权限组合不受支持。 */
					pr_warn("MAP_SHARED not completely supported on !MMU\n");
					return -EINVAL;
				}
			}
		}

		/* handle executable mappings and implied executable
		 * mappings */
		/* 最后处理挂载 noexec、READ_IMPLIES_EXEC personality 与文件执行能力。 */
		if (path_noexec(&file->f_path)) {
			if (prot & PROT_EXEC)
				return -EPERM;
		} else if ((prot & PROT_READ) && !(prot & PROT_EXEC)) {
			/* handle implication of PROT_EXEC by PROT_READ */
			/* 旧 ABI personality 把可读请求提升为可执行，但仅保留后端确实支持的能力。 */
			if (current->personality & READ_IMPLIES_EXEC) {
				if (capabilities & NOMMU_MAP_EXEC)
					prot |= PROT_EXEC;
			}
		} else if ((prot & PROT_READ) &&
			 (prot & PROT_EXEC) &&
			 !(capabilities & NOMMU_MAP_EXEC)
			 ) {
			/* backing file is not executable, try to copy */
			/* 请求执行而后端不能直接执行时撤销 DIRECT，后续若有 COPY 则用私有内存。 */
			capabilities &= ~NOMMU_MAP_DIRECT;
		}
	} else {
		/* anonymous mappings are always memory backed and can be
		 * privately mapped
		 */
		/* 匿名映射没有文件/驱动可直接共享，总是由内核分配私有后备内存。 */
		capabilities = NOMMU_MAP_COPY;

		/* handle PROT_EXEC implication by PROT_READ */
		/* 匿名映射同样保留 READ_IMPLIES_EXEC 的 ABI 计算，后续转成 VMA 权限。 */
		if ((prot & PROT_READ) &&
		    (current->personality & READ_IMPLIES_EXEC))
			prot |= PROT_EXEC;
	}

	/* allow the security API to have its say */
	/* LSM/体系结构低地址策略在所有能力检查后拥有最终否决权。 */
	ret = security_mmap_addr(addr);
	if (ret < 0)
		return ret;

	/* looks okay */
	/* 只有成功出口才发布输出能力位；失败路径不会让调用者误用半成品。 */
	*_capabilities = capabilities;
	return 0;
}

/*
 * we've determined that we can make the mapping, now translate what we
 * now know into VMA flags
 */
/*
 * 把已验证的 prot/flags/capabilities 翻译成 VMA 权限与共享属性。
 * 业务背景：validate_mmap_request() 只给出后端能力，do_mmap() 还需形成 VMA
 * 可读写/执行、未来可升级和共享/overlay 等持久状态。
 * 入参：file 为可空借用文件；prot/flags 是原 mmap 请求；capabilities 是验证后位图。
 * 出参/返回：返回完整 vm_flags 值，无输出参数和 ownership 变化。
 * 注意事项：读取 current->ptrace/personality 形成当前时刻策略；不加锁、不睡眠。
 */
static vm_flags_t determine_vm_flags(struct file *file,
		unsigned long prot,
		unsigned long flags,
		unsigned long capabilities)
{
	/* vm_flags 从通用 PROT/MAP 位开始，再补 NOMMU 特有“可实现能力”。 */
	vm_flags_t vm_flags;

	vm_flags = calc_vm_prot_bits(prot, 0) | calc_vm_flag_bits(file, flags);

	if (!file) {
		/*
		 * MAP_ANONYMOUS. MAP_SHARED is mapped to MAP_PRIVATE, because
		 * there is no fork().
		 */
		/* 匿名共享在无 fork 的 NOMMU 环境没有跨子进程区别，按私有内存实现。 */
		vm_flags |= VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;
	} else if (flags & MAP_PRIVATE) {
		/* MAP_PRIVATE file mapping */
		/* 可直接 overlay 时继承后端能力，否则私有副本允许常规读写执行升级。 */
		if (capabilities & NOMMU_MAP_DIRECT)
			vm_flags |= (capabilities & NOMMU_VMFLAGS);
		else
			vm_flags |= VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;

		if (!(prot & PROT_WRITE) && !current->ptrace)
			/*
			 * R/O private file mapping which cannot be used to
			 * modify memory, especially also not via active ptrace
			 * (e.g., set breakpoints) or later by upgrading
			 * permissions (no mprotect()). We can try overlaying
			 * the file mapping, which will work e.g., on chardevs,
			 * ramfs/tmpfs/shmfs and romfs/cramf.
			 */
			/*
			 * 只读私有且未被 ptrace 主动修改时可覆盖文件后备；NOMMU 又没有
			 * mprotect() 后续升级，因此 VM_MAYOVERLAY 不会破坏私有写隔离。
			 */
			vm_flags |= VM_MAYOVERLAY;
	} else {
		/* MAP_SHARED file mapping: NOMMU_MAP_DIRECT is set. */
		/* 共享文件映射已经验证 DIRECT，发布共享属性并继承后端允许权限。 */
		vm_flags |= VM_SHARED | VM_MAYSHARE |
			    (capabilities & NOMMU_VMFLAGS);
	}

	return vm_flags;
}

/*
 * set up a shared mapping on a file (the driver or filesystem provides and
 * pins the storage)
 */
/*
 * 建立文件共享直接映射，存储由驱动/文件系统提供并固定。
 * 业务背景：do_mmap() 对 MAP_SHARED 或复用共享 region 时调用 mmap_file()，不能
 * 用私有复制替代其可见性承诺。入参：vma 是尚未发布的拥有型构造对象。
 * 出参/返回：0 表示驱动完成映射并设置 region 顶界；其他驱动 errno 原样返回，
 * -ENOSYS 被规范化为 -ENODEV。失败时 vma/region ownership 仍归调用者。
 * 注意事项：可睡眠；驱动回调负责地址与固定存储，成功后 vm_top=vm_end。
 */
static int do_mmap_shared_file(struct vm_area_struct *vma)
{
	int ret;

	/* mmap_file() 是实际文件系统/驱动分派点，可能改写 VMA 地址与私有数据。 */
	ret = mmap_file(vma->vm_file, vma);
	if (ret == 0) {
		vma->vm_region->vm_top = vma->vm_region->vm_end;
		return 0;
	}
	if (ret != -ENOSYS)
		return ret;

	/* getting -ENOSYS indicates that direct mmap isn't possible (as
	 * opposed to tried but failed) so we can only give a suitable error as
	 * it's not possible to make a private copy if MAP_SHARED was given */
	/* -ENOSYS 表示“未实现”而非一次映射失败；共享 ABI 禁止改走私有副本。 */
	return -ENODEV;
}

/*
 * set up a private mapping or an anonymous shared mapping
 */
/*
 * 为私有文件映射或匿名“共享”映射选择直接 overlay，或分配物理连续私有副本。
 * 业务背景：do_mmap() 已建 VMA/region 后调用；优先让支持 DIRECT 的文件回调
 * 提供后备，明确返回 -ENOSYS 才退化为 COPY。
 * 入参：vma/region 是调用者拥有的未发布对象；len 为页对齐字节数；capabilities
 * 是验证后的 NOMMU_MAP_* 位。出参/返回：0 并填好地址/标志；驱动 errno 或
 * -ENOMEM。失败时对象仍归调用者，本函数只回滚自己已分配的页。
 * 注意事项：GFP_KERNEL/kernel_read 可睡眠；VM_MAPPED_COPY 表示页归 region 所有。
 */
static int do_mmap_private(struct vm_area_struct *vma,
			   struct vm_region *region,
			   unsigned long len,
			   unsigned long capabilities)
{
	/* total/point 以页计，base 是拥有型页序列，ret/order 承接读结果与伙伴阶数。 */
	unsigned long total, point;
	void *base;
	int ret, order;

	/*
	 * Invoke the file's mapping function so that it can keep track of
	 * shared mappings on devices or memory. VM_MAYOVERLAY will be set if
	 * it may attempt to share, which will make is_nommu_shared_mapping()
	 * happy.
	 */
	/* DIRECT 候选先调用文件回调；成功必须真正形成共享/overlay VMA。 */
	if (capabilities & NOMMU_MAP_DIRECT) {
		ret = mmap_file(vma->vm_file, vma);
		/* shouldn't return success if we're not sharing */
		/* 回调若声称成功却未设置共享语义，按 -ENOSYS 强制进入安全副本路径。 */
		if (WARN_ON_ONCE(!is_nommu_shared_mapping(vma->vm_flags)))
			ret = -ENOSYS;
		if (ret == 0) {
			vma->vm_region->vm_top = vma->vm_region->vm_end;
			return 0;
		}
		if (ret != -ENOSYS)
			return ret;

		/* getting an ENOSYS error indicates that direct mmap isn't
		 * possible (as opposed to tried but failed) so we'll try to
		 * make a private copy of the data and map that instead */
		/* 只有 -ENOSYS 代表能力缺失可回退；其他 errno 表示真实失败，必须保留。 */
	}


	/* allocate some memory to hold the mapping
	 * - note that this may not return a page-aligned address if the object
	 *   we're allocating is smaller than a page
	 */
	/* 计算伙伴阶数和实际页数；小于一页的对象地址不承诺天然页对齐。 */
	order = get_order(len);
	total = 1 << order;
	point = len >> PAGE_SHIFT;

	/* we don't want to allocate a power-of-2 sized page set */
	/* 尾部浪费达到 sysctl 阈值时改用精确页数，降低高阶连续分配压力。 */
	if (sysctl_nr_trim_pages && total - point >= sysctl_nr_trim_pages)
		total = point;

	/* 从此处取得 total 个页的 ownership；失败尚无页需要回滚。 */
	base = alloc_pages_exact(total << PAGE_SHIFT, GFP_KERNEL);
	if (!base)
		goto enomem;

	atomic_long_add(total, &mmap_pages_allocated);

	/* 发布前把页所有权和精确/容量边界同时写入 region/VMA。 */
	vm_flags_set(vma, VM_MAPPED_COPY);
	region->vm_flags = vma->vm_flags;
	region->vm_start = (unsigned long) base;
	region->vm_end   = region->vm_start + len;
	region->vm_top   = region->vm_start + (total << PAGE_SHIFT);

	vma->vm_start = region->vm_start;
	vma->vm_end   = region->vm_start + len;

	if (vma->vm_file) {
		/* read the contents of a file into the copy */
		/* 文件偏移由页单位转字节，kernel_read() 把快照填入私有副本。 */
		loff_t fpos;

		fpos = vma->vm_pgoff;
		fpos <<= PAGE_SHIFT;

		ret = kernel_read(vma->vm_file, base, len, &fpos);
		if (ret < 0)
			goto error_free;

		/* clear the last little bit */
		/* 短读后的尾部必须清零，既提供匿名零语义也避免泄漏旧页内容。 */
		if (ret < len)
			memset(base + ret, 0, len - ret);

	} else {
		/* 匿名标记供后续 VMA 分类；实际清零还由 do_mmap() 按 flag 决定。 */
		vma_set_anonymous(vma);
	}

	return 0;

error_free:
	/* 已拥有全部私有页但文件读取失败：释放页并清空地址，防止上层重复释放。 */
	free_page_series(region->vm_start, region->vm_top);
	region->vm_start = vma->vm_start = 0;
	region->vm_end   = vma->vm_end = 0;
	region->vm_top   = 0;
	return ret;

enomem:
	/* 分配失败只记录诊断并返回 -ENOMEM，region/VMA 仍由 do_mmap() 释放。 */
	pr_err("Allocation of length %lu from process %d (%s) failed\n",
	       len, current->pid, current->comm);
	show_mem();
	return -ENOMEM;
}

/*
 * handle mapping creation for uClinux
 */
/*
 * 为 uClinux/NOMMU 创建映射，是 vm_mmap_pgoff() 进入体系结构后端的核心状态转换点。
 * 业务背景：在无法用页表重排地址时，本函数复用已有共享 region、让驱动提供直接
 * 地址，或建立私有物理副本，最后把 VMA 发布到 current->mm。
 * 入参：file 为可空借用文件；addr 为提示地址（NOMMU 忽略）；len 为字节数；prot/
 * flags/vm_flags 为权限策略；pgoff 为页偏移；populate 是非空输出且固定写 0；uf
 * 是通用 userfaultfd 清单参数，本实现不使用、不接管。
 * 出参/返回：成功返回映射起始地址并由 mm/region 树持有 VMA；失败返回编码为
 * unsigned long 的负 errno，释放本函数取得的 file 引用和元数据，不留下半映射。
 * 注意事项：调用者持 current->mm 的 mmap 写锁；本函数可睡眠，并以
 * nommu_region_sem 串行化共享查找、引用和发布，文件引用在 VMA/region 各持一份。
 */
unsigned long do_mmap(struct file *file,
			unsigned long addr,
			unsigned long len,
			unsigned long prot,
			unsigned long flags,
			vm_flags_t vm_flags,
			unsigned long pgoff,
			unsigned long *populate,
			struct list_head *uf)
{
	/* vma/region 是构造期拥有型对象；rb 遍历共享树；capabilities/result/ret 保存阶段结果。 */
	struct vm_area_struct *vma;
	struct vm_region *region;
	struct rb_node *rb;
	unsigned long capabilities, result;
	int ret;
	VMA_ITERATOR(vmi, current->mm, 0);

	*populate = 0;

	/* decide whether we should attempt the mapping, and if so what sort of
	 * mapping */
	/* 阶段一：任何分配前完成 ABI、权限、后端能力和 LSM 校验。 */
	ret = validate_mmap_request(file, addr, len, prot, flags, pgoff,
				    &capabilities);
	if (ret < 0)
		return ret;

	/* we ignore the address hint */
	/* NOMMU 不能选择任意虚拟地址；清零提示并把长度提升到整页。 */
	addr = 0;
	len = PAGE_ALIGN(len);

	/* we've determined that we can make the mapping, now translate what we
	 * now know into VMA flags */
	/* 阶段二：把已验证能力固化成 VMA 后续权限/共享不变量。 */
	vm_flags |= determine_vm_flags(file, prot, flags, capabilities);


	/* we're going to need to record the mapping */
	/* 阶段三：先分配两个元数据对象；任一点失败都按取得顺序回滚。 */
	region = kmem_cache_zalloc(vm_region_jar, GFP_KERNEL);
	if (!region)
		goto error_getting_region;

	vma = vm_area_alloc(current->mm);
	if (!vma)
		goto error_getting_vma;

	/* 新 region 初始由待发布 VMA 持有一个引用，地址随后由具体后端填充。 */
	region->vm_usage = 1;
	region->vm_flags = vm_flags;
	region->vm_pgoff = pgoff;

	vm_flags_init(vma, vm_flags);
	vma->vm_pgoff = pgoff;

	if (file) {
		/* region 与 VMA 生命周期可分离，故分别取得独立 file 引用。 */
		region->vm_file = get_file(file);
		vma->vm_file = get_file(file);
	}

	/* 从共享查找到 region 发布全程持写锁，避免重复构造和 vm_usage 竞态。 */
	down_write(&nommu_region_sem);

	/* if we want to share, we need to check for regions created by other
	 * mmap() calls that overlap with our proposed mapping
	 * - we can only share with a superset match on most regular files
	 * - shared mappings on character devices and memory backed files are
	 *   permitted to overlap inexactly as far as we are concerned for in
	 *   these cases, sharing is handled in the driver or filesystem rather
	 *   than here
	 */
	/*
	 * 阶段四：共享候选先查已有 region。普通文件只允许新区间完全落在既有区间内；
	 * 字符设备/内存文件的重叠由其回调裁决，所以 DIRECT 能力可跳过不精确候选。
	 */
	if (is_nommu_shared_mapping(vm_flags)) {
		/* pglen/pgend 描述请求页区间，r* 描述候选，start 是复用后的实际地址。 */
		struct vm_region *pregion;
		unsigned long pglen, rpglen, pgend, rpgend, start;

		pglen = (len + PAGE_SIZE - 1) >> PAGE_SHIFT;
		pgend = pgoff + pglen;

		/* 在写锁下中序扫描，region 指针和引用计数均保持稳定。 */
		for (rb = rb_first(&nommu_region_tree); rb; rb = rb_next(rb)) {
			pregion = rb_entry(rb, struct vm_region, vm_rb);

			if (!is_nommu_shared_mapping(pregion->vm_flags))
				continue;

			/* search for overlapping mappings on the same file */
			/* 先按 inode 和文件页区间筛出同一后备对象的重叠候选。 */
			if (file_inode(pregion->vm_file) !=
			    file_inode(file))
				continue;

			if (pregion->vm_pgoff >= pgend)
				continue;

			rpglen = pregion->vm_end - pregion->vm_start;
			rpglen = (rpglen + PAGE_SIZE - 1) >> PAGE_SHIFT;
			rpgend = pregion->vm_pgoff + rpglen;
			if (pgoff >= rpgend)
				continue;

			/* handle inexactly overlapping matches between
			 * mappings */
			/* 不完全相等时，新请求必须是候选子集；否则 COPY region 无法安全共享。 */
			if ((pregion->vm_pgoff != pgoff || rpglen != pglen) &&
			    !(pgoff >= pregion->vm_pgoff && pgend <= rpgend)) {
				/* new mapping is not a subset of the region */
				/* 非 DIRECT 后端没有驱动可调解重叠，必须报告共享冲突。 */
				if (!(capabilities & NOMMU_MAP_DIRECT))
					goto sharing_violation;
				continue;
			}

			/* we've found a region we can share */
			/* 领取 region 引用并按页偏移计算 VMA 子区间，此后失败必须撤销计数。 */
			pregion->vm_usage++;
			vma->vm_region = pregion;
			start = pregion->vm_start;
			start += (pgoff - pregion->vm_pgoff) << PAGE_SHIFT;
			vma->vm_start = start;
			vma->vm_end = start + len;

			/* 私有副本直接复用内存；直接映射则仍让驱动为新 VMA 完成登记。 */
			if (pregion->vm_flags & VM_MAPPED_COPY)
				vm_flags_set(vma, VM_MAPPED_COPY);
			else {
				ret = do_mmap_shared_file(vma);
				if (ret < 0) {
					/* 撤销本次临时绑定与引用领取，统一清理新建 region/VMA。 */
					vma->vm_region = NULL;
					vma->vm_start = 0;
					vma->vm_end = 0;
					pregion->vm_usage--;
					pregion = NULL;
					goto error_just_free;
				}
			}
			/* 新 region 已被既有对象取代，释放其 file 引用和 slab ownership。 */
			fput(region->vm_file);
			kmem_cache_free(vm_region_jar, region);
			region = pregion;
			result = start;
			goto share;
		}

		/* obtain the address at which to make a shared mapping
		 * - this is the hook for quasi-memory character devices to
		 *   tell us the location of a shared mapping
		 */
		/* 未找到可复用对象时，由准内存字符设备返回真实可直接访问地址。 */
		if (capabilities & NOMMU_MAP_DIRECT) {
			addr = file->f_op->get_unmapped_area(file, addr, len,
							     pgoff, flags);
			if (IS_ERR_VALUE(addr)) {
				ret = addr;
				if (ret != -ENOSYS)
					goto error_just_free;

				/* the driver refused to tell us where to site
				 * the mapping so we'll have to attempt to copy
				 * it */
				/* -ENOSYS 表示驱动不提供直接选址；有 COPY 能力才允许退化到副本。 */
				ret = -ENODEV;
				if (!(capabilities & NOMMU_MAP_COPY))
					goto error_just_free;

				capabilities &= ~NOMMU_MAP_DIRECT;
			} else {
				/* 地址成功即同时成为 VMA 可见区间和 region 后备区间。 */
				vma->vm_start = region->vm_start = addr;
				vma->vm_end = region->vm_end = addr + len;
			}
		}
	}

	vma->vm_region = region;

	/* set up the mapping
	 * - the region is filled in if NOMMU_MAP_DIRECT is still set
	 */
	/* 阶段五：共享文件走驱动直接映射，其余走 overlay/私有复制；成功后加入 region 树。 */
	if (file && vma->vm_flags & VM_SHARED)
		ret = do_mmap_shared_file(vma);
	else
		ret = do_mmap_private(vma, region, len, capabilities);
	if (ret < 0)
		goto error_just_free;
	add_nommu_region(region);

	/* clear anonymous mappings that don't ask for uninitialized data */
	/* 除非配置和 MAP_UNINITIALIZED 同时许可，否则匿名页发布前必须清零。 */
	if (!vma->vm_file &&
	    (!IS_ENABLED(CONFIG_MMAP_ALLOW_UNINITIALIZED) ||
	     !(flags & MAP_UNINITIALIZED)))
		memset((void *)region->vm_start, 0,
		       region->vm_end - region->vm_start);

	/* okay... we have a mapping; now we have to register it */
	/* 阶段六：后备对象已完成，开始更新 mm 统计并准备发布 VMA。 */
	result = vma->vm_start;

	current->mm->total_vm += len >> PAGE_SHIFT;

share:
	/* 新建与复用路径在此汇合；预分配失败仍可完整回滚，尚未写入 mm 树。 */
	BUG_ON(!vma->vm_region);
	vma_iter_config(&vmi, vma->vm_start, vma->vm_end);
	if (vma_iter_prealloc(&vmi, vma))
		goto error_just_free;

	setup_vma_to_mm(vma, current->mm);
	current->mm->map_count++;
	/* add the VMA to the tree */
	/* vma_iter_store_new() 是地址查找者可见 VMA 的发布点。 */
	vma_iter_store_new(&vmi, vma);

	/* we flush the region from the icache only when the first executable
	 * mapping of it is made  */
	/* 同一 region 首次可执行发布时刷新一次 I-cache，标志受 region 写锁保护。 */
	if (vma->vm_flags & VM_EXEC && !region->vm_icache_flushed) {
		flush_icache_user_range(region->vm_start, region->vm_end);
		region->vm_icache_flushed = true;
	}

	up_write(&nommu_region_sem);

	return result;

error_just_free:
	/* 已持 region 写锁但尚未发布：先解锁，再走统一元数据/引用回滚。 */
	up_write(&nommu_region_sem);
error:
	/* vmi 预分配、两个 file 引用、region 和 VMA 按构造逆序释放。 */
	vma_iter_free(&vmi);
	if (region->vm_file)
		fput(region->vm_file);
	kmem_cache_free(vm_region_jar, region);
	if (vma->vm_file)
		fput(vma->vm_file);
	vm_area_free(vma);
	return ret;

sharing_violation:
	/* 不可由 DIRECT 后端调解的重叠先解锁，再转换为 -EINVAL 统一清理。 */
	up_write(&nommu_region_sem);
	pr_warn("Attempt to share mismatched mappings\n");
	ret = -EINVAL;
	goto error;

error_getting_vma:
	/* region 已分配、VMA 未分配：只释放 region，尚未取得 file 引用。 */
	kmem_cache_free(vm_region_jar, region);
	pr_warn("Allocation of vma for %lu byte allocation from process %d failed\n",
			len, current->pid);
	show_mem();
	return -ENOMEM;

error_getting_region:
	/* 最早失败点没有本地资源；记录诊断后直接返回 -ENOMEM。 */
	pr_warn("Allocation of vm region for %lu byte allocation from process %d failed\n",
			len, current->pid);
	show_mem();
	return -ENOMEM;
}

/*
 * 业务背景：mmap_pgoff 系统调用的 fd 包装层负责审计、把用户 fd 稳定成 file 引用，
 * 再进入 vm_mmap_pgoff() 的通用锁与安全调用链。
 * 入参：addr/len/prot/flags 为 mmap ABI；fd 为文件描述符；pgoff 为页单位偏移。
 * 出参/返回：成功映射地址或负 errno 的 unsigned long 编码；匿名映射忽略 fd。
 * 注意事项：fget() 成功取得的 file 引用无论映射成败均在返回前 fput()；可睡眠。
 */
unsigned long ksys_mmap_pgoff(unsigned long addr, unsigned long len,
			      unsigned long prot, unsigned long flags,
			      unsigned long fd, unsigned long pgoff)
{
	/* file 在非匿名路径持有引用；retval 初值保证无效 fd 返回 -EBADF。 */
	struct file *file = NULL;
	unsigned long retval = -EBADF;

	/* 审计先记录原始 fd/flags，随后才可能因取引用失败退出。 */
	audit_mmap_fd(fd, flags);
	if (!(flags & MAP_ANONYMOUS)) {
		file = fget(fd);
		if (!file)
			goto out;
	}

	/* 通用层取得 mmap 锁并最终分派到本文件 do_mmap()。 */
	retval = vm_mmap_pgoff(file, addr, len, prot, flags, pgoff);

	if (file)
		fput(file);
out:
	/* 所有出口在此汇合；NULL file 表示匿名或 fget 失败，无引用可释放。 */
	return retval;
}

/*
 * 业务背景：这是页偏移版本 mmap 的体系结构可见 syscall 入口，参数已由 syscall
 * 宏按 ABI 解包，实际 fd/引用处理委托 ksys_mmap_pgoff()。
 * 入参：addr/len 字节区间，prot/flags 策略，fd 描述符，pgoff 页偏移。
 * 出参/返回：透传映射地址或负 errno；无额外 ownership。
 * 注意事项：进程上下文可睡眠，MAP_ANONYMOUS 时 fd 不参与映射。
 */
SYSCALL_DEFINE6(mmap_pgoff, unsigned long, addr, unsigned long, len,
		unsigned long, prot, unsigned long, flags,
		unsigned long, fd, unsigned long, pgoff)
{
	return ksys_mmap_pgoff(addr, len, prot, flags, fd, pgoff);
}

#ifdef __ARCH_WANT_SYS_OLD_MMAP
/*
 * 旧 mmap ABI 字段清单：addr/len 是字节地址与长度；prot/flags 是权限/策略；fd
 * 是用户描述符；offset 是字节偏移，old_mmap() 校验页对齐后转换为 pgoff。
 * 该结构仅是 copy_from_user() 的栈快照，不持有 file 或其他资源。
 */
struct mmap_arg_struct {
	unsigned long addr;
	unsigned long len;
	unsigned long prot;
	unsigned long flags;
	unsigned long fd;
	unsigned long offset;
};

/*
 * 业务背景：启用 __ARCH_WANT_SYS_OLD_MMAP 的体系结构把六个参数装在用户结构中，
 * 本入口复制并转换后复用现代 ksys_mmap_pgoff()。
 * 入参：arg 是可空/可故障的用户只读指针，ownership 不转移。
 * 出参/返回：用户复制失败 -EFAULT，字节偏移未页对齐 -EINVAL，否则透传 mmap 结果。
 * 注意事项：先复制到栈上快照避免用户并发改字段；函数可因用户访问和映射而睡眠。
 */
SYSCALL_DEFINE1(old_mmap, struct mmap_arg_struct __user *, arg)
{
	/* a 是一次性内核快照，后续校验与调用不再访问用户指针。 */
	struct mmap_arg_struct a;

	if (copy_from_user(&a, arg, sizeof(a)))
		return -EFAULT;
	if (offset_in_page(a.offset))
		return -EINVAL;

	return ksys_mmap_pgoff(a.addr, a.len, a.prot, a.flags, a.fd,
			       a.offset >> PAGE_SHIFT);
}
#endif /* __ARCH_WANT_SYS_OLD_MMAP */

/*
 * split a vma into two pieces at address 'addr', a new vma is allocated either
 * for the first part or the tail.
 */
/*
 * 在 addr 把匿名 VMA/region 分成两段；new_below 决定新对象承载前段还是后段。
 * 业务背景：do_munmap() 删除匿名映射中段前先切出边界，使随后收缩只处理端部。
 * 入参：vmi 是持有预分配状态的输入输出迭代器；vma 是 mm 中借用对象；addr 为
 * 页对齐切点；new_below 为真时新 VMA 是低段。出参/返回：0 并发布新 VMA，或
 * -ENOMEM 且原 VMA 保持未分裂；成功后 mm/region 树各多一个对象。
 * 注意事项：调用者持 mmap 写锁；仅允许独占匿名 region，可睡眠；驱动 open 回调
 * 在发布前取得新 VMA 私有状态，region 写锁保护重设地址键的摘除/重插。
 */
static int split_vma(struct vma_iterator *vmi, struct vm_area_struct *vma,
		     unsigned long addr, int new_below)
{
	/* new/region 是新对象 ownership；npages 修正文件页偏移；mm 是借用所属地址空间。 */
	struct vm_area_struct *new;
	struct vm_region *region;
	unsigned long npages;
	struct mm_struct *mm;

	/* we're only permitted to split anonymous regions (these should have
	 * only a single usage on the region) */
	/* 文件映射可能由驱动固定/共享，不能由通用 NOMMU 代码任意切后备区。 */
	if (vma->vm_file)
		return -ENOMEM;

	mm = vma->vm_mm;
	if (mm->map_count >= get_sysctl_max_map_count())
		return -ENOMEM;

	/* 阶段一：先取得两个元数据对象；失败时尚未修改原 VMA。 */
	region = kmem_cache_alloc(vm_region_jar, GFP_KERNEL);
	if (!region)
		return -ENOMEM;

	new = vm_area_dup(vma);
	if (!new)
		goto err_vma_dup;

	/* most fields are the same, copy all, and then fixup */
	/* 复制共同状态后立即改掉嵌入 rb 节点所属对象的区间字段，尚未入树。 */
	*region = *vma->vm_region;
	new->vm_region = region;

	npages = (addr - vma->vm_start) >> PAGE_SHIFT;

	/* new_below 决定哪一半由新对象承担，并同步 region/VMA 边界与页偏移。 */
	if (new_below) {
		region->vm_top = region->vm_end = new->vm_end = addr;
	} else {
		region->vm_start = new->vm_start = addr;
		region->vm_pgoff = new->vm_pgoff += npages;
	}

	/* 阶段二：预分配 Maple Tree 节点，确保后面的不可回滚发布不再失败。 */
	vma_iter_config(vmi, new->vm_start, new->vm_end);
	if (vma_iter_prealloc(vmi, vma)) {
		pr_warn("Allocation of vma tree for process %d failed\n",
			current->pid);
		goto err_vmi_preallocate;
	}

	/* 驱动 open 为复制 VMA 建立独立私有状态；与最终 delete 的 vma_close() 配对。 */
	if (new->vm_ops && new->vm_ops->open)
		new->vm_ops->open(new);

	/* 阶段三：原 region 改键前先摘树，再把两个互不重叠的新区间一起发布。 */
	down_write(&nommu_region_sem);
	delete_nommu_region(vma->vm_region);
	/* 同步修改原 VMA/region 所承担的另一半，确保两段无缝且不重叠。 */
	if (new_below) {
		vma->vm_region->vm_start = vma->vm_start = addr;
		vma->vm_region->vm_pgoff = vma->vm_pgoff += npages;
	} else {
		vma->vm_region->vm_end = vma->vm_end = addr;
		vma->vm_region->vm_top = addr;
	}
	add_nommu_region(vma->vm_region);
	add_nommu_region(new->vm_region);
	up_write(&nommu_region_sem);

	/* 阶段四：更新反向映射并把新 VMA 发布到 mm Maple Tree。 */
	setup_vma_to_mm(vma, mm);
	setup_vma_to_mm(new, mm);
	vma_iter_store_new(vmi, new);
	mm->map_count++;
	return 0;

err_vmi_preallocate:
	/* Maple 预分配失败：新 VMA 尚未 open/发布，可直接释放。 */
	vm_area_free(new);
err_vma_dup:
	/* 两个失败标签最终释放新 region，原 VMA/region 完全未改。 */
	kmem_cache_free(vm_region_jar, region);
	return -ENOMEM;
}

/*
 * shrink a VMA by removing the specified chunk from either the beginning or
 * the end
 */
/*
 * 从匿名 VMA 首端或尾端移除 [from,to)，同步缩短唯一 region 并释放对应页。
 * 业务背景：do_munmap() 已确认区间是单一匿名 VMA 子集，必要时已先 split_vma()。
 * 入参：vmi 为 mm 写锁下迭代器；vma 为借用独占对象；from/to 为页对齐半开区间。
 * 出参/返回：0 表示 VMA/region/页均收缩，-ENOMEM 表示 Maple 更新失败且未释放页。
 * 注意事项：region->vm_usage 必须为 1；成功释放页后旧地址立即失效。
 */
static int vmi_shrink_vma(struct vma_iterator *vmi,
		      struct vm_area_struct *vma,
		      unsigned long from, unsigned long to)
{
	struct vm_region *region;

	/* adjust the VMA's pointers, which may reposition it in the MM's tree
	 * and list */
	/* 先让待删除范围从 mm 查找结构消失；失败则保持 VMA 和页 ownership 不变。 */
	if (from > vma->vm_start) {
		if (vma_iter_clear_gfp(vmi, from, vma->vm_end, GFP_KERNEL))
			return -ENOMEM;
		vma->vm_end = from;
	} else {
		if (vma_iter_clear_gfp(vmi, vma->vm_start, to, GFP_KERNEL))
			return -ENOMEM;
		vma->vm_start = to;
	}

	/* cut the backing region down to size */
	/* region 是 VMA 独占后备；共享对象无法安全释放其中一段。 */
	region = vma->vm_region;
	BUG_ON(region->vm_usage != 1);

	/* 地址键改变必须在 region 树中执行“摘除—修改—重插”的原子序列。 */
	down_write(&nommu_region_sem);
	delete_nommu_region(region);
	if (from > region->vm_start) {
		to = region->vm_top;
		region->vm_top = region->vm_end = from;
	} else {
		region->vm_start = to;
	}
	add_nommu_region(region);
	up_write(&nommu_region_sem);

	/* 两棵索引均已更新后才最终释放页，避免查找者命中已归还内存。 */
	free_page_series(from, to);
	return 0;
}

/*
 * release a mapping
 * - under NOMMU conditions the chunk to be unmapped must be backed by a single
 *   VMA, though it need not cover the whole VMA
 */
/*
 * 释放一个映射区间；NOMMU 要求请求由单一 VMA 后备，匿名 VMA 可按页边界拆/缩，
 * 文件 VMA 只能整体移除。业务背景：vm_munmap() 持 mmap 写锁后进入本核心。
 * 入参：mm 为借用地址空间；start/len 是字节区间；uf 是未使用的通用 userfaultfd
 * 清单，不接管。出参/返回：0 成功；-EINVAL 表示区间/对齐/映射不合法；-ENOMEM
 * 表示元数据预分配失败。成功会释放 VMA、file/region 引用及可能的私有页。
 * 注意事项：调用者持 mmap 写锁，函数可睡眠；失败在不可回滚边界前保持映射有效。
 */
int do_munmap(struct mm_struct *mm, unsigned long start, size_t len, struct list_head *uf)
{
	/* vmi 从 start 查找；vma 是锁内借用；end 是半开终点；ret 记录最终删除状态。 */
	VMA_ITERATOR(vmi, mm, start);
	struct vm_area_struct *vma;
	unsigned long end;
	int ret = 0;

	/* 页对齐回绕为 0 也按非法请求处理。 */
	len = PAGE_ALIGN(len);
	if (len == 0)
		return -EINVAL;

	end = start + len;

	/* find the first potentially overlapping VMA */
	/* 阶段一：定位 end 前首个候选；未映射诊断限报五次，limit 仅作近似抑制。 */
	vma = vma_find(&vmi, end);
	if (!vma) {
		static int limit;
		/* limit 的竞态只影响日志条数，不参与映射正确性，故无需锁。 */
		if (limit < 5) {
			pr_warn("munmap of memory not mmapped by process %d (%s): 0x%lx-0x%lx\n",
					current->pid, current->comm,
					start, start + len - 1);
			limit++;
		}
		return -EINVAL;
	}

	/* we're allowed to split an anonymous VMA but not a file-backed one */
	/* 阶段二：文件映射只接受恰好到某个 VMA 末端的整体删除，不切驱动后备。 */
	if (vma->vm_file) {
		do {
			if (start > vma->vm_start)
				return -EINVAL;
			if (end == vma->vm_end)
				goto erase_whole_vma;
			vma = vma_find(&vmi, end);
		} while (vma);
		return -EINVAL;
	} else {
		/* the chunk must be a subset of the VMA found */
		/* 匿名请求必须完全包含于单一 VMA，内部边界必须页对齐。 */
		if (start == vma->vm_start && end == vma->vm_end)
			goto erase_whole_vma;
		if (start < vma->vm_start || end > vma->vm_end)
			return -EINVAL;
		if (offset_in_page(start))
			return -EINVAL;
		if (end != vma->vm_end && offset_in_page(end))
			return -EINVAL;
		if (start != vma->vm_start && end != vma->vm_end) {
			/* 删除中段先在 start 分裂，随后对保留高段的 VMA 收缩其低端。 */
			ret = split_vma(&vmi, vma, start, 1);
			if (ret < 0)
				return ret;
		}
		return vmi_shrink_vma(&vmi, vma, start, end);
	}

erase_whole_vma:
	/* 整体删除先从索引摘除；成功后 delete_vma() 跨过最终资源释放边界。 */
	if (delete_vma_from_mm(vma))
		ret = -ENOMEM;
	else
		delete_vma(mm, vma);
	return ret;
}

/*
 * 业务背景：内核调用者释放 current 地址空间映射时使用此加锁包装。
 * 入参：addr/len 为字节区间。出参/返回：透传 do_munmap() 的 0 或负 errno。
 * 注意事项：内部取得 mmap 写锁并可睡眠；函数返回后成功区间不可再访问。
 */
int vm_munmap(unsigned long addr, size_t len)
{
	struct mm_struct *mm = current->mm;
	int ret;

	mmap_write_lock(mm);
	ret = do_munmap(mm, addr, len, NULL);
	mmap_write_unlock(mm);
	return ret;
}
EXPORT_SYMBOL(vm_munmap);

/*
 * 业务背景：munmap(2) 的系统调用入口，复用 vm_munmap() 的锁与删除协议。
 * 入参：addr 为起始用户地址，len 为字节长度。出参/返回：0 或负 errno。
 * 注意事项：只作用于 current->mm，可睡眠；NOMMU 文件映射不支持部分解除。
 */
SYSCALL_DEFINE2(munmap, unsigned long, addr, size_t, len)
{
	return vm_munmap(addr, len);
}

/*
 * release all the mappings made in a process's VM space
 */
/*
 * 释放进程地址空间中的全部映射。业务背景：mm 最后用户退出时 teardown 路径调用，
 * 逐个撤销反向映射、驱动状态、file/region 引用和私有页，最后销毁 Maple Tree。
 * 入参：mm 为 teardown 所拥有的地址空间，可为 NULL；ownership 本身不在此释放。
 * 出参/返回：无；非 NULL 时所有 VMA 消失且 total_vm 归零。
 * 注意事项：虽通常已无并发用户，仍取 mmap 写锁满足 helper 断言；循环可主动调度。
 */
void exit_mmap(struct mm_struct *mm)
{
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma;

	if (!mm)
		return;

	mm->total_vm = 0;

	/*
	 * Lock the mm to avoid assert complaining even though this is the only
	 * user of the mm
	 */
	/* 即使 mm 已独占，也按常规锁协议操作，避免 Maple/VMA helper 的锁断言失真。 */
	mmap_write_lock(mm);
	for_each_vma(vmi, vma) {
		/* 当前 vma 仍在迭代树中；先撤销外部索引再销毁其全部 ownership。 */
		cleanup_vma_from_mm(vma);
		delete_vma(mm, vma);
		cond_resched();
	}
	__mt_destroy(&mm->mm_mt);
	mmap_write_unlock(mm);
}

/*
 * expand (or shrink) an existing mapping, potentially moving it at the same
 * time (controlled by the MREMAP_MAYMOVE flag and available VM space)
 *
 * under NOMMU conditions, we only permit changing a mapping's size, and only
 * as long as it stays within the region allocated by do_mmap_private() and the
 * block is not shareable
 *
 * MREMAP_FIXED is not supported under NOMMU conditions
 */
/*
 * 原 mremap 可调整并移动映射；NOMMU 只允许在 do_mmap_private() 已分配的 region
 * 容量内改变非共享 VMA 的可见长度，不能移动，MREMAP_FIXED 也仅容忍原地址。
 * 业务背景：mremap syscall 持写锁后调用本核心，避免改变地址键时并发查找。
 * 入参：addr/old_len/new_len 为字节地址和长度；flags 为 MREMAP_*；new_addr 仅用于
 * 校验固定地址请求。出参/返回：成功返回原起点；失败返回 -EINVAL/-EFAULT/
 * -EPERM/-ENOMEM 的 unsigned long 编码，无部分修改。
 * 注意事项：调用者持 mmap 写锁；不分配/移动页，也不改变 region ownership。
 */
static unsigned long do_mremap(unsigned long addr,
			unsigned long old_len, unsigned long new_len,
			unsigned long flags, unsigned long new_addr)
{
	struct vm_area_struct *vma;

	/* insanity checks first */
	/* 阶段一：长度页对齐后不得为零，起点必须页对齐，固定移动请求直接拒绝。 */
	old_len = PAGE_ALIGN(old_len);
	new_len = PAGE_ALIGN(new_len);
	if (old_len == 0 || new_len == 0)
		return (unsigned long) -EINVAL;

	if (offset_in_page(addr))
		return -EINVAL;

	if (flags & MREMAP_FIXED && new_addr != addr)
		return (unsigned long) -EINVAL;

	/* 阶段二：只接受精确覆盖单一 VMA 的描述，避免修改错误映射。 */
	vma = find_vma_exact(current->mm, addr, old_len);
	if (!vma)
		return (unsigned long) -EINVAL;

	if (vma->vm_end != vma->vm_start + old_len)
		return (unsigned long) -EFAULT;

	/* 共享后备的可见范围由其他 VMA/驱动共同约束，不能单边改变。 */
	if (is_nommu_shared_mapping(vma->vm_flags))
		return (unsigned long) -EPERM;

	if (new_len > vma->vm_region->vm_end - vma->vm_region->vm_start)
		return (unsigned long) -ENOMEM;

	/* all checks complete - do it */
	/* 唯一提交点：只改 VMA 逻辑末端，必须仍落在已拥有 region 容量内。 */
	vma->vm_end = vma->vm_start + new_len;
	return vma->vm_start;
}

/*
 * 业务背景：mremap(2) 系统调用入口负责以 mmap 写锁串行化 VMA 长度更新。
 * 入参：addr、old_len、new_len、flags、new_addr 均来自用户 ABI。
 * 出参/返回：透传 do_mremap() 的原地址或负 errno 编码。
 * 注意事项：NOMMU 不移动映射；锁保证返回前其他地址查找者不见中间状态。
 */
SYSCALL_DEFINE5(mremap, unsigned long, addr, unsigned long, old_len,
		unsigned long, new_len, unsigned long, flags,
		unsigned long, new_addr)
{
	unsigned long ret;

	mmap_write_lock(current->mm);
	ret = do_mremap(addr, old_len, new_len, flags, new_addr);
	mmap_write_unlock(current->mm);
	return ret;
}

/*
 * 业务背景：驱动用 remap_pfn_range() 声明 VMA 直接对应物理 PFN；NOMMU 不建 PTE，
 * 只能验证用户地址就是该 PFN 的直接映射地址并标记 VMA 类型。
 * 入参：vma 为待配置借用对象；addr 为映射地址；pfn 为页帧号；size/prot 为兼容
 * 参数且不被本实现读取。出参/返回：地址匹配返回 0 并设置 IO/PFNMAP/不可扩展/
 * 不转储标志，否则 -EINVAL 且 VMA 不变。注意事项：调用者负责 VMA 锁与物理资源生命周期。
 */
int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
		unsigned long pfn, unsigned long size, pgprot_t prot)
{
	if (addr != (pfn << PAGE_SHIFT))
		return -EINVAL;

	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	return 0;
}
EXPORT_SYMBOL(remap_pfn_range);

/*
 * 业务背景：驱动把物理 I/O 区间映射进整个 VMA 时，用此包装叠加 VMA 页偏移并
 * 调用体系结构 io_remap_pfn_range()。
 * 入参：vma 为借用目标；start/len 是物理起点和可用长度，当前实现由下层校验长度。
 * 出参/返回：透传下层 0/errno；成功时 VMA 标志/物理关联由下层完成。
 * 注意事项：pfn 加 vm_pgoff 后须仍在设备资源内，资源生命周期由驱动保证。
 */
int vm_iomap_memory(struct vm_area_struct *vma, phys_addr_t start, unsigned long len)
{
	/* pfn 是应用 VMA 偏移后的物理页号，vm_len 是完整用户可见字节数。 */
	unsigned long pfn = start >> PAGE_SHIFT;
	unsigned long vm_len = vma->vm_end - vma->vm_start;

	pfn += vma->vm_pgoff;
	return io_remap_pfn_range(vma, vma->vm_start, pfn, vm_len, vma->vm_page_prot);
}
EXPORT_SYMBOL(vm_iomap_memory);

/*
 * 业务背景：把 vmalloc_user 系列授权的缓冲区暴露给用户；NOMMU 不建新页表，
 * 而是把 VMA 地址改成缓冲区加页偏移后的直接地址。
 * 入参：vma 为待配置借用对象；addr 为有效 VM_USERMAP 缓冲区；pgoff 为页偏移。
 * 出参/返回：未授权返回 -EINVAL；成功返回 0，并保持原长度更新 start/end。
 * 注意事项：不取得 addr ownership；调用者保证范围未越过分配且持适当 VMA 锁。
 */
int remap_vmalloc_range(struct vm_area_struct *vma, void *addr,
			unsigned long pgoff)
{
	unsigned int size = vma->vm_end - vma->vm_start;

	/* VM_USERMAP 是 __vmalloc_user_flags() 写入的显式用户映射授权。 */
	if (!(vma->vm_flags & VM_USERMAP))
		return -EINVAL;

	vma->vm_start = (unsigned long)(addr + (pgoff << PAGE_SHIFT));
	vma->vm_end = vma->vm_start + size;

	return 0;
}
EXPORT_SYMBOL(remap_vmalloc_range);

/*
 * 业务背景：MMU 页缓存缺页回调在 fault 时装入 PTE；NOMMU 映射在 mmap 时已经
 * 直接可访问，不应触发缺页。入参：vmf 为形式借用参数。
 * 出参/返回：BUG() 终止，0 不可达。注意事项：到达此空桩表示 VMA 配置错误。
 */
vm_fault_t filemap_fault(struct vm_fault *vmf)
{
	BUG();
	return 0;
}
EXPORT_SYMBOL(filemap_fault);

/*
 * 业务背景：filemap_map_pages() 是批量 fault-around 回调，同样依赖 MMU 页表。
 * 入参：vmf 为借用 fault 上下文，start_pgoff/end_pgoff 为页索引范围。
 * 出参/返回：BUG() 终止，0 不可达。注意事项：NOMMU 调用者必须避开此路径。
 */
vm_fault_t filemap_map_pages(struct vm_fault *vmf,
		pgoff_t start_pgoff, pgoff_t end_pgoff)
{
	BUG();
	return 0;
}
EXPORT_SYMBOL(filemap_map_pages);

/*
 * 业务背景：ptrace/proc/BPF 等内核路径需要在已持有目标 mm 引用时复制其一段地址；
 * NOMMU 地址可直接访问，但仍须用 mmap 读锁验证 VMA 权限和生命周期。
 * 入参：mm 为持引用的借用地址空间；addr 为目标地址；buf 为内核源/目的缓冲区；
 * len 为请求字节数；gup_flags 的 FOLL_WRITE 决定写目标还是从目标读。
 * 出参/返回：返回单个 VMA 内实际复制字节数；锁被信号中断、无映射或无权限返回 0。
 * 注意事项：不跨 VMA；调用者先排除 addr+len 溢出，复制钩子可能维护缓存一致性。
 */
static int __access_remote_vm(struct mm_struct *mm, unsigned long addr,
			      void *buf, int len, unsigned int gup_flags)
{
	/* vma 是读锁内借用；write 非零表示 buf -> 目标，否则目标 -> buf。 */
	struct vm_area_struct *vma;
	int write = gup_flags & FOLL_WRITE;

	if (mmap_read_lock_killable(mm))
		return 0;

	/* the access must start within one of the target process's mappings */
	/* find_vma() 的 load 语义保证起点实际落在目标 VMA 内，而非仅返回后继 VMA。 */
	vma = find_vma(mm, addr);
	if (vma) {
		/* don't overrun this mapping */
		/* 一次调用最多处理当前 VMA 尾端，返回短长度让上层决定是否继续。 */
		if (addr + len >= vma->vm_end)
			len = vma->vm_end - addr;

		/* only read or write mappings where it is permitted */
		/* VM_MAY* 表示 VMA 可允许的最大权限；不满足时不产生任何复制副作用。 */
		if (write && vma->vm_flags & VM_MAYWRITE)
			copy_to_user_page(vma, NULL, addr,
					 (void *) addr, buf, len);
		/* 读方向把目标直接地址复制进内核 buf；两个 helper 都不转移 ownership。 */
		else if (!write && vma->vm_flags & VM_MAYREAD)
			copy_from_user_page(vma, NULL, addr,
					    buf, (void *) addr, len);
		else
			len = 0;
	} else {
		len = 0;
	}

	/* 复制已结束，释放 VMA 生命周期保护；返回值随后只依赖局部长度。 */
	mmap_read_unlock(mm);

	return len;
}

/**
 * access_remote_vm - access another process' address space
 * @mm:		the mm_struct of the target address space
 * @addr:	start address to access
 * @buf:	source or destination buffer
 * @len:	number of bytes to transfer
 * @gup_flags:	flags modifying lookup behaviour
 *
 * The caller must hold a reference on @mm.
 */
/*
 * 访问另一地址空间：mm 是调用者持有引用的目标；addr 是起始地址；buf 是内核态
 * 源/目的缓冲区；len 是字节数；gup_flags 控制查找方向，FOLL_WRITE 表示写目标。
 *
 * 业务背景：这是已有稳定 mm 的公开包装，ptrace 等路径用它进入 NOMMU 单 VMA
 * 复制核心。出参/返回：实际复制字节数，0 表示中断/无映射/无权限；不转移 mm/buf
 * ownership。注意事项：调用者必须持 mm 引用并自行处理地址加法溢出与短复制。
 */
int access_remote_vm(struct mm_struct *mm, unsigned long addr,
		void *buf, int len, unsigned int gup_flags)
{
	return __access_remote_vm(mm, addr, buf, len, gup_flags);
}

/*
 * Access another process' address space.
 * - source/target buffer must be kernel space
 */
/*
 * 访问另一进程地址空间，源/目标 buf 必须位于内核空间。
 * 业务背景：调用者只有 task 引用时，本函数先用 get_task_mm() 跨越任务退出边界，
 * 再复用 __access_remote_vm()，最后对称 mmput()。
 * 入参：tsk 为持引用目标任务；addr/len 为目标区间；buf 为内核缓冲区；gup_flags
 * 以 FOLL_WRITE 区分方向。出参/返回：实际复制字节数，溢出或无 mm 返回 0。
 * 注意事项：可睡眠；mm 引用仅在本函数内持有，不跨返回转移。
 */
int access_process_vm(struct task_struct *tsk, unsigned long addr, void *buf, int len,
		unsigned int gup_flags)
{
	/* mm 是临时持有引用；ret 保存内部复制长度/errno。 */
	struct mm_struct *mm;

	/* 在取得 mm 前排除地址回绕，避免错误 VMA 校验。 */
	if (addr + len < addr)
		return 0;

	/* get_task_mm() 可能因内核线程/已退出任务返回 NULL；成功引用与 mmput 配对。 */
	mm = get_task_mm(tsk);
	if (!mm)
		return 0;

	len = __access_remote_vm(mm, addr, buf, len, gup_flags);

	mmput(mm);
	return len;
}
EXPORT_SYMBOL_GPL(access_process_vm);

#ifdef CONFIG_BPF_SYSCALL
/*
 * Copy a string from another process's address space as given in mm.
 * If there is any error return -EFAULT.
 */
/*
 * 从给定 mm 的另一进程地址复制字符串；任意错误返回 -EFAULT。
 * 业务背景：BPF 系统调用辅助路径需要安全取得用户字符串，又要保证输出始终 NUL
 * 终止。入参：mm 为持引用借用对象；addr 为源地址；buf 为可写内核缓冲区；len
 * 为含终止符容量。出参/返回：成功返回不含 NUL 的字节数；失败 -EFAULT；截断时
 * 返回 len-1。注意事项：只读单一 VMA并可被信号中断，len 应大于 0。
 */
static int __copy_remote_vm_str(struct mm_struct *mm, unsigned long addr,
				void *buf, int len)
{
	/* addr_end 防溢出；vma 在读锁内借用；ret 默认错误并仅在成功复制时改写。 */
	unsigned long addr_end;
	struct vm_area_struct *vma;
	int ret = -EFAULT;

	/* 先写空串保证所有早退路径仍向调用者提供 NUL 终止缓冲区。 */
	*(char *)buf = '\0';

	if (mmap_read_lock_killable(mm))
		return ret;

	/* the access must start within one of the target process's mappings */
	/* 找不到起点所属映射时保持 -EFAULT。 */
	vma = find_vma(mm, addr);
	if (!vma)
		goto out;

	/* 显式检测 addr+len，避免回绕绕过 VMA 末端裁剪。 */
	if (check_add_overflow(addr, len, &addr_end))
		goto out;

	/* don't overrun this mapping */
	/* 最多读到当前 VMA 末端，不跨越权限/生命周期可能不同的下一映射。 */
	if (addr_end > vma->vm_end)
		len = vma->vm_end - addr;

	/* only read mappings where it is permitted */
	/* 只有 VM_MAYREAD 才访问源；strscpy 的截断 errno 转成已复制容量 len-1。 */
	if (vma->vm_flags & VM_MAYREAD) {
		ret = strscpy(buf, (char *)addr, len);
		if (ret < 0)
			ret = len - 1;
	}

out:
	/* 所有锁后错误在此汇合，确保 mmap 读锁恰好释放一次。 */
	mmap_read_unlock(mm);
	return ret;
}

/**
 * copy_remote_vm_str - copy a string from another process's address space.
 * @tsk:	the task of the target address space
 * @addr:	start address to read from
 * @buf:	destination buffer
 * @len:	number of bytes to copy
 * @gup_flags:	flags modifying lookup behaviour (unused)
 *
 * The caller must hold a reference on @mm.
 *
 * Return: number of bytes copied from @addr (source) to @buf (destination);
 * not including the trailing NUL. Always guaranteed to leave NUL-terminated
 * buffer. On any error, return -EFAULT.
 */
/*
 * 从目标任务复制字符串：tsk 是调用者持引用的任务；addr 为源地址；buf 为目标
 * 内核缓冲区；len 为总容量；gup_flags 是通用 API 兼容参数，本实现不使用。
 * 调用者应持目标任务引用（原英文写 @mm，当前签名实际接收 @tsk）。成功返回不含
 * 尾 NUL 的字节数并保证缓冲区终止；任意错误 -EFAULT，len==0 特例返回 0。
 *
 * 业务背景：BPF 路径只有 task 时由此取得临时 mm 引用。注意事项：可睡眠；
 * get_task_mm()/mmput() 严格配对，无 mm 时也先把非空 buf 置为空串。
 */
int copy_remote_vm_str(struct task_struct *tsk, unsigned long addr,
		       void *buf, int len, unsigned int gup_flags)
{
	struct mm_struct *mm;
	int ret;

	if (unlikely(len == 0))
		return 0;

	/* 跨越目标退出边界先取得 mm 引用；无 mm 时仍兑现非空缓冲区 NUL 保证。 */
	mm = get_task_mm(tsk);
	if (!mm) {
		*(char *)buf = '\0';
		return -EFAULT;
	}

	ret = __copy_remote_vm_str(mm, addr, buf, len);

	/* 字符串复制完成后不再访问目标地址，立即归还 mm 生命周期引用。 */
	mmput(mm);

	return ret;
}
EXPORT_SYMBOL_GPL(copy_remote_vm_str);
#endif /* CONFIG_BPF_SYSCALL */

/**
 * nommu_shrink_inode_mappings - Shrink the shared mappings on an inode
 * @inode: The inode to check
 * @size: The current filesize of the inode
 * @newsize: The proposed filesize of the inode
 *
 * Check the shared mappings on an inode on behalf of a shrinking truncate to
 * make sure that any outstanding VMAs aren't broken and then shrink the
 * vm_regions that extend beyond so that do_mmap() doesn't
 * automatically grant mappings that are too large.
 */
/*
 * 为缩小 truncate 检查 inode 的共享映射：inode 是待截断文件；size/newsize 是
 * 当前/目标字节长度。先拒绝死区内仍存在的共享 VMA，再收紧越过新 EOF 的 region
 * 上界，避免后续 do_mmap() 复用旧 region 而授予过大范围。
 *
 * 业务背景：文件截断在真正缩小后备存储前调用。出参/返回：0 表示映射兼容并已
 * 调整元数据；-ETXTBSY 表示共享 VMA 会被截断破坏，未修改 region。
 * 注意事项：先取 nommu_region_sem 写锁、再取 i_mmap 读锁，所有相关路径必须保持
 * 相同锁序；不释放物理页，VMA/region/file ownership 均不转移。
 */
int nommu_shrink_inode_mappings(struct inode *inode, size_t size,
				size_t newsize)
{
	/* low/high 是页索引死区；r_size/r_top 以字节计描述 region 对应文件顶端。 */
	struct vm_area_struct *vma;
	struct vm_region *region;
	pgoff_t low, high;
	size_t r_size, r_top;

	low = newsize >> PAGE_SHIFT;
	high = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;

	/* 两把锁共同稳定 region 字段与 inode 的 VMA 区间树。 */
	down_write(&nommu_region_sem);
	i_mmap_lock_read(inode->i_mapping);

	/* search for VMAs that fall within the dead zone */
	/* 阶段一：任何共享 VMA 覆盖将删除文件区间时拒绝 truncate，先不改元数据。 */
	vma_interval_tree_foreach(vma, &inode->i_mapping->i_mmap, low, high) {
		/* found one - only interested if it's shared out of the page
		 * cache */
		/* 只有 VM_SHARED 对后备缩小敏感；私有副本仍由自身页承载。 */
		if (vma->vm_flags & VM_SHARED) {
			i_mmap_unlock_read(inode->i_mapping);
			up_write(&nommu_region_sem);
			/* errno 并非字面“文本忙”，但为 NOMMU mmap ABI 约定的冲突报告。 */
			return -ETXTBSY; /* not quite true, but near enough */
			/* 原注释意为 errno 并非完全精确但足够接近；上方已说明实际冲突语义。 */
		}
	}

	/* reduce any regions that overlap the dead zone - if in existence,
	 * these will be pointed to by VMAs that don't overlap the dead zone
	 *
	 * we don't check for any regions that start beyond the EOF as there
	 * shouldn't be any
	 */
	/*
	 * 阶段二：剩余共享 VMA 不落入死区，但其 region 容量可能越过新 EOF；收紧
	 * vm_top/vm_end，防止以后相邻 mmap 通过 region 共享重新暴露被截断部分。
	 */
	vma_interval_tree_foreach(vma, &inode->i_mapping->i_mmap, 0, ULONG_MAX) {
		if (!(vma->vm_flags & VM_SHARED))
			continue;

		region = vma->vm_region;
		r_size = region->vm_top - region->vm_start;
		r_top = (region->vm_pgoff << PAGE_SHIFT) + r_size;

		/* region 以文件偏移加内存容量换算顶端，仅对实际越界对象做单调收缩。 */
		if (r_top > newsize) {
			region->vm_top -= r_top - newsize;
			if (region->vm_end > region->vm_top)
				region->vm_end = region->vm_top;
		}
	}

	i_mmap_unlock_read(inode->i_mapping);
	up_write(&nommu_region_sem);
	return 0;
}

/*
 * Initialise sysctl_user_reserve_kbytes.
 *
 * This is intended to prevent a user from starting a single memory hogging
 * process, such that they cannot recover (kill the hog) in OVERCOMMIT_NEVER
 * mode.
 *
 * The default value is min(3% of free memory, 128MB)
 * 128MB is enough to recover with sshd/login, bash, and top/kill.
 */
/*
 * 初始化 sysctl_user_reserve_kbytes，用于 OVERCOMMIT_NEVER 下防止普通用户单进程
 * 吃尽内存后连 kill 都无法启动；默认取空闲内存约 3% 与 128MiB 的较小值，后者
 * 足够运行 sshd/login、shell 和 top/kill 等恢复工具。
 * 业务背景：subsys_initcall 在内存统计可用后调用。入参：无。出参/返回：固定 0，
 * 副作用是发布 KiB 单位用户保留量。注意事项：仅初始化一次，无并发写者。
 */
static int __meminit init_user_reserve(void)
{
	/* free_kbytes 是当前全局空闲页换算的 KiB 快照。 */
	unsigned long free_kbytes;

	free_kbytes = K(global_zone_page_state(NR_FREE_PAGES));

	sysctl_user_reserve_kbytes = min(free_kbytes / 32, 1UL << 17);
	return 0;
}
subsys_initcall(init_user_reserve);

/*
 * Initialise sysctl_admin_reserve_kbytes.
 *
 * The purpose of sysctl_admin_reserve_kbytes is to allow the sys admin
 * to log in and kill a memory hogging process.
 *
 * Systems with more than 256MB will reserve 8MB, enough to recover
 * with sshd, bash, and top in OVERCOMMIT_GUESS. Smaller systems will
 * only reserve 3% of free pages by default.
 */
/*
 * 初始化 sysctl_admin_reserve_kbytes，让管理员在内存耗尽时仍可登录并终止内存
 * 大户；空闲内存超过约 256MiB 时上限 8MiB，否则默认保留约 3%。该额度面向
 * OVERCOMMIT_GUESS 下 sshd、shell 和 top 等恢复工具。
 * 业务背景：subsys_initcall 启动阶段调用。入参：无。出参/返回：固定 0，并发布
 * KiB 单位管理员保留量。注意事项：一次性初始化，不分配内存、不失败。
 */
static int __meminit init_admin_reserve(void)
{
	/* free_kbytes 是初始化时全局空闲页的 KiB 快照，不随之后内存变化自动更新。 */
	unsigned long free_kbytes;

	free_kbytes = K(global_zone_page_state(NR_FREE_PAGES));

	sysctl_admin_reserve_kbytes = min(free_kbytes / 32, 1UL << 13);
	return 0;
}
subsys_initcall(init_admin_reserve);

/*
 * 业务背景：fork 的 dup_mm() 路径要求复制父 mm；NOMMU 没有页表/VMA fork 复制，
 * 这里只在父 mm 写锁下复制可执行文件引用，维持通用 mm 生命周期契约。
 * 入参：mm 为新地址空间的拥有型构造对象；oldmm 为父地址空间借用对象，二者非空。
 * 出参/返回：固定 0；dup_mm_exe_file() 更新 mm->exe_file 引用，其他映射不复制。
 * 注意事项：内部取得 oldmm mmap 写锁并可睡眠；新引用最终由 mm teardown 释放。
 */
int dup_mmap(struct mm_struct *mm, struct mm_struct *oldmm)
{
	mmap_write_lock(oldmm);
	dup_mm_exe_file(mm, oldmm);
	mmap_write_unlock(oldmm);
	return 0;
}
