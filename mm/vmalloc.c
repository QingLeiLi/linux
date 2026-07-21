// SPDX-License-Identifier: GPL-2.0-only
/*
 * vmalloc/vmap 子系统学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。此标识仅说明新增中文注释的来源，
 * 不属于 Linux 上游源码署名，便于与其他模型生成的注释版本交叉比较。
 *
 * kmalloc 和伙伴系统擅长提供物理连续内存，但大块物理连续内存会受碎片影响。
 * vmalloc 的核心思路是把若干离散物理页映射到一段连续内核虚拟地址：调用者
 * 得到连续指针，页表负责把连续 VA 翻译到离散 PFN。
 *
 * 这带来三类额外工作：
 *
 *  1. 管理 VMALLOC_START..VMALLOC_END 中互不重叠的虚拟地址区间；
 *  2. 在 init_mm 中建立、拆除内核页表，并处理 cache/TLB 可见性；
 *  3. 分别管理 VA、页表、物理页和调试元数据的生命周期及失败回滚。
 *
 * 主要分配链路：
 *
 *   vmalloc/vzalloc
 *     -> __vmalloc_node_range_noprof   选择地址范围、页阶、NUMA/KASAN 策略
 *       -> __get_vm_area_node          预留 VA，创建 vmap_area/vm_struct
 *       -> __vmalloc_area_node         分配 backing pages 并建立页表
 *       -> clear_vm_uninitialized_flag 发布完整对象
 *
 * 主要释放链路：
 *
 *   vfree
 *     -> remove_vm_area                从 busy 索引取消发布并拆页表
 *     -> lazy purge                    批量 TLB flush 后才允许复用 VA
 *     -> vm_area_free_pages            归还 vmalloc 自己拥有的物理页
 *
 * 当前设计用增强红黑树降低空闲区间搜索成本，用有序链表快速合并邻居，用
 * vmap-node/per-CPU block 降低全局锁竞争，并用 lazy purge 摊薄跨 CPU TLB
 * shootdown。收益是扩展性和吞吐；代价是数据结构、并发状态和回收时序更复杂。
 */
/*
 *  Copyright (C) 1993  Linus Torvalds
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 *  SMP-safe vmalloc/vfree/ioremap, Tigran Aivazian <tigran@veritas.com>, May 2000
 *  Major rework to support vmap/vunmap, Christoph Hellwig, SGI, August 2002
 *  Numa awareness, Christoph Lameter, SGI, June 2005
 *  Improving global KVA allocator, Uladzislau Rezki, Sony, May 2019
 */

#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/set_memory.h>
#include <linux/debugobjects.h>
#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <linux/rbtree.h>
#include <linux/xarray.h>
#include <linux/io.h>
#include <linux/rcupdate.h>
#include <linux/pfn.h>
#include <linux/kmemleak.h>
#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/memcontrol.h>
#include <linux/llist.h>
#include <linux/uio.h>
#include <linux/bitops.h>
#include <linux/rbtree_augmented.h>
#include <linux/overflow.h>
#include <linux/pgtable.h>
#include <linux/hugetlb.h>
#include <linux/sched/mm.h>
#include <asm/tlbflush.h>
#include <asm/shmparam.h>
#include <linux/page_owner.h>

#define CREATE_TRACE_POINTS
#include <trace/events/vmalloc.h>

#include "internal.h"
#include "pgalloc-track.h"

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP
/*
 * ioremap 可以在架构允许时用 PMD/PUD 等大页表项映射连续设备物理地址，以减少
 * 页表和 TLB 压力。该变量是“允许尝试的最大页阶”，并不保证最终一定使用大页。
 * 启动参数 nohugeiomap 把上限降到 PAGE_SHIFT，常用于架构规避或问题诊断。
 * __ro_after_init 防止系统启动完成后再改变全局映射策略。
 */
static unsigned int __ro_after_init ioremap_max_page_shift = BITS_PER_LONG - 1;

/* 启动早期把 ioremap 候选粒度锁定为基本页；参数只改变性能策略，不改变映射属性。 */
static int __init set_nohugeiomap(char *str)
{
	ioremap_max_page_shift = PAGE_SHIFT;
	return 0;
}
early_param("nohugeiomap", set_nohugeiomap);
#else /* CONFIG_HAVE_ARCH_HUGE_VMAP */
static const unsigned int ioremap_max_page_shift = PAGE_SHIFT;
#endif	/* CONFIG_HAVE_ARCH_HUGE_VMAP */

#ifdef CONFIG_HAVE_ARCH_HUGE_VMALLOC
/*
 * 普通 RAM 的 huge-vmap 策略与设备 ioremap 分开控制，因为二者对 struct page、
 * 物理连续性和调用者兼容性的要求不同。关闭 huge-vmalloc 只禁用优化，后续代码
 * 仍必须能够使用 PAGE_SIZE 页表项完成相同分配。
 */
static bool __ro_after_init vmap_allow_huge = true;

/* 关闭普通 RAM 的 huge-vmap 尝试，便于定位大页映射相关的架构或驱动兼容问题。 */
static int __init set_nohugevmalloc(char *str)
{
	vmap_allow_huge = false;
	return 0;
}
early_param("nohugevmalloc", set_nohugevmalloc);
#else /* CONFIG_HAVE_ARCH_HUGE_VMALLOC */
static const bool vmap_allow_huge = false;
#endif	/* CONFIG_HAVE_ARCH_HUGE_VMALLOC */

/*
 * 判断一个数值地址是否属于 vmalloc 虚拟地址窗口。
 *
 * 这只是地址分类，不证明该位置当前存在映射。KASAN 硬件 tag 不属于地址区间，
 * 因此比较前必须去掉 tag；半开区间保证 VMALLOC_END 不会被误判为有效地址。
 */
bool is_vmalloc_addr(const void *x)
{
	unsigned long addr = (unsigned long)kasan_reset_tag(x);

	return addr >= VMALLOC_START && addr < VMALLOC_END;
}
EXPORT_SYMBOL(is_vmalloc_addr);

/*
 * 中断等原子上下文不能直接完成 vfree：拆映射、TLB 回收和释放页的路径可能睡眠。
 * 每个 CPU 先把请求加入无锁 llist，再由 workqueue 切换到可睡眠上下文完成回收。
 * 这里把“快速提交释放请求”和“真正释放资源”分成两个阶段。
 */
struct vfree_deferred {
	struct llist_head list;
	struct work_struct wq;
};
static DEFINE_PER_CPU(struct vfree_deferred, vfree_deferred);

/*** Page table manipulation functions ***/
/*
 * 页表操作层只负责 VA -> PFN/page 的硬件映射，不负责选择 VA，也不拥有物理页。
 * 上层必须已经保证 [addr, end) 没有与其他 vmap_area 重叠。本层修改 init_mm，
 * 即所有进程共享的内核地址空间；发现目标页表项已有映射说明软件 VA 索引和
 * 硬件页表不一致，属于内核不变量破坏，而不是普通的“地址忙”。
 *
 * Linux 保留 PGD -> P4D -> PUD -> PMD -> PTE 的统一五级接口。实际架构可以
 * 折叠某些层级，通用代码仍按同一调用链工作。每层先尝试安装 huge leaf，条件
 * 不满足就下降一级；huge 失败属于优化回退，不应改变映射能否成功的语义。
 */

/*
 * 在一个 PMD 覆盖范围内安装最终 PTE 映射。
 *
 * phys_addr 可能来自设备内存，不一定存在 struct page，所以主路径使用 PFN；
 * struct page 只在诊断重复映射且 PFN 有效时使用。函数可能分配 PTE 页，失败时
 * 返回 -ENOMEM；成功后 mask 告诉顶层 PTE 层发生修改，需要架构同步。
 */
static int vmap_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift, pgtbl_mod_mask *mask)
{
	pte_t *pte;
	u64 pfn;
	struct page *page;
	unsigned long size = PAGE_SIZE;

	if (WARN_ON_ONCE(!PAGE_ALIGNED(end - addr)))
		return -EINVAL;

	/* 把物理字节地址转换为本段起始 PFN，后续每安装一页就同步推进。 */
	pfn = phys_addr >> PAGE_SHIFT;

	/* 必要时创建下级 PTE 页；track 版本同时记录新建过哪些页表层级。 */
	pte = pte_alloc_kernel_track(pmd, addr, mask);
	if (!pte)
		return -ENOMEM;

	/* 某些架构/虚拟化后端可批量提交连续页表写入，降低逐项更新开销。 */
	lazy_mmu_mode_enable();

	do {
		/*
		 * 目标 PTE 必须为空。覆盖旧 entry 会让未知映射失去管理关系，因此有效
		 * 普通 RAM 先打印 page 诊断信息，然后用 BUG 阻止继续破坏页表。
		 */
		if (unlikely(!pte_none(ptep_get(pte)))) {
			if (pfn_valid(pfn)) {
				page = pfn_to_page(pfn);
				dump_page(page, "remapping already mapped page");
			}
			BUG();
		}

#ifdef CONFIG_HUGETLB_PAGE
		/*
		 * 部分架构可以在 PTE 层编码大于 PAGE_SIZE 的 hugetlb entry。架构 helper
		 * 综合剩余长度、地址/PFN 对齐和策略上限选择本轮大小；不能用时返回一页。
		 */
		size = arch_vmap_pte_range_map_size(addr, end, pfn, max_page_shift);
		if (size != PAGE_SIZE) {
			pte_t entry = pfn_pte(pfn, prot);

			entry = arch_make_huge_pte(entry, ilog2(size), 0);
			set_huge_pte_at(&init_mm, addr, pte, entry, size);
			pfn += PFN_DOWN(size);
			continue;
		}
#endif
		set_pte_at(&init_mm, addr, pte, pfn_pte(pfn, prot));
		pfn++;
	/*
	 * 按本轮实际映射大小推进 PTE 游标和 VA。size 可能是一页，也可能是特殊
	 * huge-PTE 范围；二者同步推进后，下一轮始终从第一个未映射地址继续。
	 */
	} while (pte += PFN_DOWN(size), addr += size, addr != end);

	lazy_mmu_mode_disable();
	*mask |= PGTBL_PTE_MODIFIED;
	return 0;
}

/*
 * 尝试用一个 PMD leaf 覆盖完整 PMD_SIZE 区间。
 *
 * 策略上限、架构能力、区间长度、VA 对齐和 PA 对齐缺一不可。若下级 PTE 页
 * 已存在，只能在它为空且可安全释放时替换为 PMD leaf。返回 0 表示“回退普通
 * PTE”，不是错误；只有 pmd_set_huge 成功才让调用者跳过下级页表。
 */
static int vmap_try_huge_pmd(pmd_t *pmd, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift)
{
	if (max_page_shift < PMD_SHIFT)
		return 0;

	if (!arch_vmap_pmd_supported(prot))
		return 0;

	if ((end - addr) != PMD_SIZE)
		return 0;

	if (!IS_ALIGNED(addr, PMD_SIZE))
		return 0;

	if (!IS_ALIGNED(phys_addr, PMD_SIZE))
		return 0;

	if (pmd_present(*pmd) && !pmd_free_pte_page(pmd, addr))
		return 0;

	return pmd_set_huge(pmd, phys_addr, prot);
}

/*
 * 遍历 PUD 下的 PMD 区间：每段先尝试 PMD huge leaf，失败再调用 PTE 层。
 *
 * pmd_addr_end 把本轮限制在一个 PMD entry 内。每轮结束时 VA 和 PA 按相同长度
 * 推进，维持整个映射的偏移关系；任一层失败就停止，由顶层统一处理已建前缀。
 */
static int vmap_pmd_range(pud_t *pud, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift, pgtbl_mod_mask *mask)
{
	pmd_t *pmd;
	unsigned long next;
	int err = 0;

	pmd = pmd_alloc_track(&init_mm, pud, addr, mask);
	if (!pmd)
		return -ENOMEM;
	do {
		next = pmd_addr_end(addr, end);

		if (vmap_try_huge_pmd(pmd, addr, next, phys_addr, prot,
					max_page_shift)) {
			*mask |= PGTBL_PMD_MODIFIED;
			continue;
		}

		err = vmap_pte_range(pmd, addr, next, phys_addr, prot, max_page_shift, mask);
		if (err)
			break;
	} while (pmd++, phys_addr += (next - addr), addr = next, addr != end);
	return err;
}

/*
 * 尝试在 PUD 层安装更大粒度的 leaf 映射。
 *
 * 逻辑与 PMD 版本相同，但覆盖范围和对齐要求提升到 PUD_SIZE。保留独立函数而
 * 不是用宏强行合并，是因为各架构对 PUD/PMD leaf、下级页表释放和 entry 构造
 * 的支持并不完全对称。失败仍只表示继续下降到 PMD/PTE。
 */
static int vmap_try_huge_pud(pud_t *pud, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift)
{
	if (max_page_shift < PUD_SHIFT)
		return 0;

	if (!arch_vmap_pud_supported(prot))
		return 0;

	if ((end - addr) != PUD_SIZE)
		return 0;

	if (!IS_ALIGNED(addr, PUD_SIZE))
		return 0;

	if (!IS_ALIGNED(phys_addr, PUD_SIZE))
		return 0;

	if (pud_present(*pud) && !pud_free_pmd_page(pud, addr))
		return 0;

	return pud_set_huge(pud, phys_addr, prot);
}

/*
 * 遍历 P4D 下的 PUD 区间。
 *
 * pud_alloc_track 在未折叠 PUD 的架构上可能创建页表页；折叠架构则退化成地址
 * 转换。每个子区间先尝试 PUD leaf，无法使用时交给 PMD 层，错误立即向上传播。
 */
static int vmap_pud_range(p4d_t *p4d, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift, pgtbl_mod_mask *mask)
{
	pud_t *pud;
	unsigned long next;
	int err = 0;

	pud = pud_alloc_track(&init_mm, p4d, addr, mask);
	if (!pud)
		return -ENOMEM;
	do {
		next = pud_addr_end(addr, end);

		if (vmap_try_huge_pud(pud, addr, next, phys_addr, prot,
					max_page_shift)) {
			*mask |= PGTBL_PUD_MODIFIED;
			continue;
		}

		err = vmap_pmd_range(pud, addr, next, phys_addr, prot, max_page_shift, mask);
		if (err)
			break;
	} while (pud++, phys_addr += (next - addr), addr = next, addr != end);
	return err;
}

/*
 * 尝试 P4D 级 huge leaf。只有真正支持五级页表并提供 P4D leaf 的架构才可能
 * 成功；四级页表通常折叠该层。保留统一层级让通用代码不必散布条件编译。
 */
static int vmap_try_huge_p4d(p4d_t *p4d, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift)
{
	if (max_page_shift < P4D_SHIFT)
		return 0;

	if (!arch_vmap_p4d_supported(prot))
		return 0;

	if ((end - addr) != P4D_SIZE)
		return 0;

	if (!IS_ALIGNED(addr, P4D_SIZE))
		return 0;

	if (!IS_ALIGNED(phys_addr, P4D_SIZE))
		return 0;

	if (p4d_present(*p4d) && !p4d_free_pud_page(p4d, addr))
		return 0;

	return p4d_set_huge(p4d, phys_addr, prot);
}

/*
 * PGD 下的最高公共递归层。mask 贯穿调用链，汇总哪些页表层发生结构变化，
 * 顶层据此一次性执行架构同步，避免每下降一级就重复做昂贵工作。
 */
static int vmap_p4d_range(pgd_t *pgd, unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift, pgtbl_mod_mask *mask)
{
	p4d_t *p4d;
	unsigned long next;
	int err = 0;

	p4d = p4d_alloc_track(&init_mm, pgd, addr, mask);
	if (!p4d)
		return -ENOMEM;
	do {
		next = p4d_addr_end(addr, end);

		if (vmap_try_huge_p4d(p4d, addr, next, phys_addr, prot,
					max_page_shift)) {
			*mask |= PGTBL_P4D_MODIFIED;
			continue;
		}

		err = vmap_pud_range(p4d, addr, next, phys_addr, prot, max_page_shift, mask);
		if (err)
			break;
	} while (p4d++, phys_addr += (next - addr), addr = next, addr != end);
	return err;
}

/*
 * 建立物理连续地址到内核 VA 的页表，但不负责最终 cache flush。
 *
 * 这是 PGD 级调度器：验证调用上下文，逐 PGD entry 调用下级递归。页表分配
 * 可能睡眠；noflush 设计让批量调用者把 cache/TLB 处理合并到更高层。
 */
static int vmap_range_noflush(unsigned long addr, unsigned long end,
			phys_addr_t phys_addr, pgprot_t prot,
			unsigned int max_page_shift)
{
	pgd_t *pgd;
	unsigned long start;
	unsigned long next;
	int err;
	pgtbl_mod_mask mask = 0;

	/*
	 * Might allocate pagetables (for most archs a more precise annotation
	 * would be might_alloc(GFP_PGTABLE_KERNEL)). Also might shootdown TLB
	 * (requires IRQs enabled on x86).
	 */
	might_sleep();
	BUG_ON(addr >= end);

	start = addr;
	pgd = pgd_offset_k(addr);
	do {
		/* 当前批次限制在一个 PGD entry 内，避免下级递归跨越顶层边界。 */
		next = pgd_addr_end(addr, end);
		err = vmap_p4d_range(pgd, addr, next, phys_addr, prot,
					max_page_shift, &mask);
		if (err)
			break;
	} while (pgd++, phys_addr += (next - addr), addr = next, addr != end);

	/* 只有架构关心的页表层发生变化时，才同步共享内核映射。 */
	if (mask & ARCH_PAGE_TABLE_SYNC_MASK)
		arch_sync_kernel_mappings(start, end);

	return err;
}

/*
 * 同步的物理连续映射入口。页表建立后刷新新 VA 别名的 cache 状态，并让 KMSAN
 * 建立对应元数据。pgprot_nx 默认移除执行权限，避免普通数据映射意外可执行。
 */
int vmap_page_range(unsigned long addr, unsigned long end,
		    phys_addr_t phys_addr, pgprot_t prot)
{
	int err;

	err = vmap_range_noflush(addr, end, phys_addr, pgprot_nx(prot),
				 ioremap_max_page_shift);
	flush_cache_vmap(addr, end);
	if (!err)
		err = kmsan_ioremap_page_range(addr, end, phys_addr, prot,
					       ioremap_max_page_shift);
	return err;
}

/*
 * ioremap 的受检包装。调用者必须先用 VM_IOREMAP 预留完整 vm_struct；传入范围
 * 还必须与预留范围完全一致，使 vm_struct、页表和后续释放始终描述同一对象。
 */
int ioremap_page_range(unsigned long addr, unsigned long end,
		phys_addr_t phys_addr, pgprot_t prot)
{
	struct vm_struct *area;

	area = find_vm_area((void *)addr);
	if (!area || !(area->flags & VM_IOREMAP)) {
		WARN_ONCE(1, "vm_area at addr %lx is not marked as VM_IOREMAP\n", addr);
		return -EINVAL;
	}
	if (addr != (unsigned long)area->addr ||
	    (void *)end != area->addr + get_vm_area_size(area)) {
		WARN_ONCE(1, "ioremap request [%lx,%lx) doesn't match vm_area [%lx, %lx)\n",
			  addr, end, (long)area->addr,
			  (long)area->addr + get_vm_area_size(area));
		return -ERANGE;
	}
	return vmap_page_range(addr, end, phys_addr, prot);
}

/*
 * 清除一个 PMD 下的叶子 PTE。
 *
 * 某些架构在 PTE 层也能编码多页 hugetlb entry，因此每轮先询问实际覆盖大小，
 * 再原子取出并清除旧 entry。函数只拆页表，不释放 backing page；最终 TLB flush
 * 由更高层统一完成，避免每个小范围都触发跨 CPU shootdown。
 */
static void vunmap_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			     pgtbl_mod_mask *mask)
{
	pte_t *pte;
	pte_t ptent;
	unsigned long size = PAGE_SIZE;

	pte = pte_offset_kernel(pmd, addr);
	lazy_mmu_mode_enable();

	do {
#ifdef CONFIG_HUGETLB_PAGE
		size = arch_vmap_pte_range_unmap_size(addr, pte);
		if (size != PAGE_SIZE) {
			if (WARN_ON(!IS_ALIGNED(addr, size))) {
				addr = ALIGN_DOWN(addr, size);
				pte = PTR_ALIGN_DOWN(pte, sizeof(*pte) * (size >> PAGE_SHIFT));
			}
			ptent = huge_ptep_get_and_clear(&init_mm, addr, pte, size);
			if (WARN_ON(end - addr < size))
				size = end - addr;
		} else
#endif
			ptent = ptep_get_and_clear(&init_mm, addr, pte);
		WARN_ON(!pte_none(ptent) && !pte_present(ptent));
	} while (pte += (size >> PAGE_SHIFT), addr += size, addr != end);

	lazy_mmu_mode_disable();
	*mask |= PGTBL_PTE_MODIFIED;
}

/*
 * 拆除一个 PUD 下的 PMD 范围。先尝试把 PMD 当作 huge leaf 清除；若不是 leaf，
 * 才把它解释为 PTE 页并继续下降。坏 entry 会被清除并跳过，绝不能作为页表指针
 * 解引用。大范围处理期间 cond_resched，避免 vmalloc 释放长时间独占 CPU。
 */
static void vunmap_pmd_range(pud_t *pud, unsigned long addr, unsigned long end,
			     pgtbl_mod_mask *mask)
{
	pmd_t *pmd;
	unsigned long next;
	int cleared;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);

		cleared = pmd_clear_huge(pmd);
		if (cleared || pmd_bad(*pmd))
			*mask |= PGTBL_PMD_MODIFIED;

		if (cleared) {
			WARN_ON(next - addr < PMD_SIZE);
			continue;
		}
		if (pmd_none_or_clear_bad(pmd))
			continue;
		vunmap_pte_range(pmd, addr, next, mask);

		cond_resched();
	} while (pmd++, addr = next, addr != end);
}

/*
 * PUD 级拆映射与 PMD 级保持相同状态机：leaf 整项清除，table 继续下降，none/bad
 * 直接跳过。mask 记录本层是否改变，供最终架构同步使用。
 */
static void vunmap_pud_range(p4d_t *p4d, unsigned long addr, unsigned long end,
			     pgtbl_mod_mask *mask)
{
	pud_t *pud;
	unsigned long next;
	int cleared;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);

		cleared = pud_clear_huge(pud);
		if (cleared || pud_bad(*pud))
			*mask |= PGTBL_PUD_MODIFIED;

		if (cleared) {
			WARN_ON(next - addr < PUD_SIZE);
			continue;
		}
		if (pud_none_or_clear_bad(pud))
			continue;
		vunmap_pmd_range(pud, addr, next, mask);
	} while (pud++, addr = next, addr != end);
}

/*
 * P4D 级拆映射。折叠 P4D 的架构会由 helper 保持正确退化；通用代码仍按完整
 * 层级向下遍历，因此建立和拆除路径具有镜像结构。
 */
static void vunmap_p4d_range(pgd_t *pgd, unsigned long addr, unsigned long end,
			     pgtbl_mod_mask *mask)
{
	p4d_t *p4d;
	unsigned long next;

	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);

		p4d_clear_huge(p4d);
		if (p4d_bad(*p4d))
			*mask |= PGTBL_P4D_MODIFIED;

		if (p4d_none_or_clear_bad(p4d))
			continue;
		vunmap_pud_range(p4d, addr, next, mask);
	} while (p4d++, addr = next, addr != end);
}

/*
 * vunmap_range_noflush is similar to vunmap_range, but does not
 * flush caches or TLBs.
 *
 * The caller is responsible for calling flush_cache_vmap() before calling
 * this function, and flush_tlb_kernel_range after it has returned
 * successfully (and before the addresses are expected to cause a page fault
 * or be re-mapped for something else, if TLB flushes are being delayed or
 * coalesced).
 *
 * This is an internal function only. Do not use outside mm/.
 */
void __vunmap_range_noflush(unsigned long start, unsigned long end)
{
	unsigned long next;
	pgd_t *pgd;
	unsigned long addr = start;
	pgtbl_mod_mask mask = 0;

	/* 空区间表示上层范围计算错误；继续清页表可能伤及相邻内核映射。 */
	BUG_ON(addr >= end);
	pgd = pgd_offset_k(addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_bad(*pgd))
			mask |= PGTBL_PGD_MODIFIED;
		if (pgd_none_or_clear_bad(pgd))
			continue;
		vunmap_p4d_range(pgd, addr, next, &mask);
	} while (pgd++, addr = next, addr != end);

	if (mask & ARCH_PAGE_TABLE_SYNC_MASK)
		arch_sync_kernel_mappings(start, end);
}

void vunmap_range_noflush(unsigned long start, unsigned long end)
{
	/* KMSAN 元数据必须与真实页表同步失效，避免后续复用 VA 时继承旧状态。 */
	kmsan_vunmap_range_noflush(start, end);
	__vunmap_range_noflush(start, end);
}

/**
 * vunmap_range - unmap kernel virtual addresses
 * @addr: start of the VM area to unmap
 * @end: end of the VM area to unmap (non-inclusive)
 *
 * Clears any present PTEs in the virtual address range, flushes TLBs and
 * caches. Any subsequent access to the address before it has been re-mapped
 * is a kernel bug.
 */
void vunmap_range(unsigned long addr, unsigned long end)
{
	/*
	 * 完整顺序是：先处理旧虚拟别名的 cache 状态，再清页表，最后 shootdown TLB。
	 * 返回后 VA 已不可访问，但 VA 描述符和物理页仍由上层释放路径负责。
	 */
	flush_cache_vunmap(addr, end);
	vunmap_range_noflush(addr, end);
	flush_tlb_kernel_range(addr, end);
}

/*
 * 把 struct page 指针数组逐项安装到 PTE。
 *
 * 与连续 phys_addr 路径不同，物理来源由 pages[nr] 决定；nr 是整个递归共享的
 * 消费进度。NULL page、无效普通 PFN 或已有 PTE 都表示调用契约/状态异常，函数
 * 停在已映射前缀并把错误交给上层统一回滚。
 */
static int vmap_pages_pte_range(pmd_t *pmd, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr,
		pgtbl_mod_mask *mask)
{
	int err = 0;
	pte_t *pte;

	/*
	 * nr is a running index into the array which helps higher level
	 * callers keep track of where we're up to.
	 */

	pte = pte_alloc_kernel_track(pmd, addr, mask);
	if (!pte)
		return -ENOMEM;

	lazy_mmu_mode_enable();

	do {
		struct page *page = pages[*nr];

		if (WARN_ON(!pte_none(ptep_get(pte)))) {
			err = -EBUSY;
			break;
		}
		if (WARN_ON(!page)) {
			err = -ENOMEM;
			break;
		}
		if (WARN_ON(!pfn_valid(page_to_pfn(page)))) {
			err = -EINVAL;
			break;
		}

		set_pte_at(&init_mm, addr, pte, mk_pte(page, prot));
		(*nr)++;
	} while (pte++, addr += PAGE_SIZE, addr != end);

	lazy_mmu_mode_disable();
	*mask |= PGTBL_PTE_MODIFIED;

	return err;
}

/*
 * page 数组路径的 PMD/PUD/P4D helper 只负责按页表边界切段，并把同一个 nr
 * 游标传到底层。它们不重新计算数组下标，从而保证跨页表边界时 page 顺序连续。
 */
static int vmap_pages_pmd_range(pud_t *pud, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr,
		pgtbl_mod_mask *mask)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = pmd_alloc_track(&init_mm, pud, addr, mask);
	if (!pmd)
		return -ENOMEM;
	do {
		next = pmd_addr_end(addr, end);
		if (vmap_pages_pte_range(pmd, addr, next, prot, pages, nr, mask))
			return -ENOMEM;
	} while (pmd++, addr = next, addr != end);
	return 0;
}

static int vmap_pages_pud_range(p4d_t *p4d, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr,
		pgtbl_mod_mask *mask)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_alloc_track(&init_mm, p4d, addr, mask);
	if (!pud)
		return -ENOMEM;
	do {
		next = pud_addr_end(addr, end);
		if (vmap_pages_pmd_range(pud, addr, next, prot, pages, nr, mask))
			return -ENOMEM;
	} while (pud++, addr = next, addr != end);
	return 0;
}

static int vmap_pages_p4d_range(pgd_t *pgd, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr,
		pgtbl_mod_mask *mask)
{
	p4d_t *p4d;
	unsigned long next;

	p4d = p4d_alloc_track(&init_mm, pgd, addr, mask);
	if (!p4d)
		return -ENOMEM;
	do {
		next = p4d_addr_end(addr, end);
		if (vmap_pages_pud_range(p4d, addr, next, prot, pages, nr, mask))
			return -ENOMEM;
	} while (p4d++, addr = next, addr != end);
	return 0;
}

/*
 * 普通 PAGE_SIZE 映射的可靠基线。它禁止 huge leaf，逐页消费离散 pages 数组；
 * huge-vmap 不可用或调用者本来只有 order-0 页时都走这里。成功只表示页表完成，
 * cache 可见性仍由同步包装处理。
 */
static int vmap_small_pages_range_noflush(unsigned long addr, unsigned long end,
		pgprot_t prot, struct page **pages)
{
	unsigned long start = addr;
	pgd_t *pgd;
	unsigned long next;
	int err = 0;
	int nr = 0;
	pgtbl_mod_mask mask = 0;

	BUG_ON(addr >= end);
	pgd = pgd_offset_k(addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_bad(*pgd))
			mask |= PGTBL_PGD_MODIFIED;
		err = vmap_pages_p4d_range(pgd, addr, next, prot, pages, &nr, &mask);
		if (err)
			break;
	} while (pgd++, addr = next, addr != end);

	if (mask & ARCH_PAGE_TABLE_SYNC_MASK)
		arch_sync_kernel_mappings(start, end);

	return err;
}

/*
 * vmap_pages_range_noflush is similar to vmap_pages_range, but does not
 * flush caches.
 *
 * The caller is responsible for calling flush_cache_vmap() after this
 * function returns successfully and before the addresses are accessed.
 *
 * This is an internal function only. Do not use outside mm/.
 */
int __vmap_pages_range_noflush(unsigned long addr, unsigned long end,
		pgprot_t prot, struct page **pages, unsigned int page_shift)
{
	unsigned int i, nr = (end - addr) >> PAGE_SHIFT;

	WARN_ON(page_shift < PAGE_SHIFT);

	/* 没有架构支持或调用者只要求 base page 时，直接使用逐 PTE 基线路径。 */
	if (!IS_ENABLED(CONFIG_HAVE_ARCH_HUGE_VMALLOC) ||
			page_shift == PAGE_SHIFT)
		return vmap_small_pages_range_noflush(addr, end, prot, pages);

	/* 每轮处理一个 page_shift 大小的物理连续块，并同步推进 VA。 */
	for (i = 0; i < nr; i += 1U << (page_shift - PAGE_SHIFT)) {
		int err;

		err = vmap_range_noflush(addr, addr + (1UL << page_shift),
					page_to_phys(pages[i]), prot,
					page_shift);
		if (err)
			return err;

		addr += 1UL << page_shift;
	}

	return 0;
}

int vmap_pages_range_noflush(unsigned long addr, unsigned long end,
		pgprot_t prot, struct page **pages, unsigned int page_shift,
		gfp_t gfp_mask)
{
	/* KMSAN 先准备影子/origin 映射；失败时不能继续发布真实页表。 */
	int ret = kmsan_vmap_pages_range_noflush(addr, end, prot, pages,
						page_shift, gfp_mask);

	if (ret)
		return ret;
	return __vmap_pages_range_noflush(addr, end, prot, pages, page_shift);
}

/*
 * 同步包装在页表建立后刷新整个目标范围的 cache。即使深层只完成部分前缀后
 * 失败，统一 flush 仍安全；具体清理责任留给掌握完整范围和资源所有权的调用者。
 */
static int __vmap_pages_range(unsigned long addr, unsigned long end,
		pgprot_t prot, struct page **pages, unsigned int page_shift,
		gfp_t gfp_mask)
{
	int err;

	err = vmap_pages_range_noflush(addr, end, prot, pages, page_shift, gfp_mask);
	flush_cache_vmap(addr, end);
	return err;
}

/**
 * vmap_pages_range - map pages to a kernel virtual address
 * @addr: start of the VM area to map
 * @end: end of the VM area to map (non-inclusive)
 * @prot: page protection flags to use
 * @pages: pages to map (always PAGE_SIZE pages)
 * @page_shift: maximum shift that the pages may be mapped with, @pages must
 * be aligned and contiguous up to at least this shift.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
int vmap_pages_range(unsigned long addr, unsigned long end,
		pgprot_t prot, struct page **pages, unsigned int page_shift)
{
	/* 公共入口使用 GFP_KERNEL，允许页表和检测元数据分配进入正常睡眠回收。 */
	return __vmap_pages_range(addr, end, prot, pages, page_shift, GFP_KERNEL);
}

/*
 * 校验 sparse vm_area 的局部 map/unmap 请求。
 *
 * VM_SPARSE 表示区域只预留 VA，页表由调用者分段填充；普通 vmalloc 区域不能
 * 使用该接口，否则其统一 pages/lifecycle 会被破坏。这里同时拒绝权限重置、
 * 无 guard、超物理页总量及越过预留范围的请求。
 */
static int check_sparse_vm_area(struct vm_struct *area, unsigned long start,
				unsigned long end)
{
	might_sleep();
	if (WARN_ON_ONCE(area->flags & VM_FLUSH_RESET_PERMS))
		return -EINVAL;
	if (WARN_ON_ONCE(area->flags & VM_NO_GUARD))
		return -EINVAL;
	if (WARN_ON_ONCE(!(area->flags & VM_SPARSE)))
		return -EINVAL;
	if ((end - start) >> PAGE_SHIFT > totalram_pages())
		return -E2BIG;
	if (start < (unsigned long)area->addr ||
	    (void *)end > area->addr + get_vm_area_size(area))
		return -ERANGE;
	return 0;
}

/**
 * vm_area_map_pages - map pages inside given sparse vm_area
 * @area: vm_area
 * @start: start address inside vm_area
 * @end: end address inside vm_area
 * @pages: pages to map (always PAGE_SIZE pages)
 */
int vm_area_map_pages(struct vm_struct *area, unsigned long start,
		      unsigned long end, struct page **pages)
{
	/* 校验通过后，本次只建立 [start,end) 子范围；page 所有权仍属于调用者。 */
	int err;

	err = check_sparse_vm_area(area, start, end);
	if (err)
		return err;

	return vmap_pages_range(start, end, PAGE_KERNEL, pages, PAGE_SHIFT);
}

/**
 * vm_area_unmap_pages - unmap pages inside given sparse vm_area
 * @area: vm_area
 * @start: start address inside vm_area
 * @end: end address inside vm_area
 */
void vm_area_unmap_pages(struct vm_struct *area, unsigned long start,
			 unsigned long end)
{
	/* 同步拆除子范围，保证函数返回后调用者可以安全替换或释放对应 pages。 */
	if (check_sparse_vm_area(area, start, end))
		return;

	vunmap_range(start, end);
}

/*
 * 模块文本在部分架构拥有独立于 VMALLOC_START..END 的地址窗口，其他架构则直接
 * 使用 vmalloc 区。该 helper 统一两种布局，但仍只做地址分类，不证明映射存活。
 */
int is_vmalloc_or_module_addr(const void *x)
{
	/*
	 * ARM, x86-64 and sparc64 put modules in a special place,
	 * and fall back on vmalloc() if that fails. Others
	 * just put it in the vmalloc space.
	 */
#if defined(CONFIG_EXECMEM) && defined(MODULES_VADDR)
	unsigned long addr = (unsigned long)kasan_reset_tag(x);
	if (addr >= MODULES_VADDR && addr < MODULES_END)
		return 1;
#endif
	return is_vmalloc_addr(x);
}
EXPORT_SYMBOL_GPL(is_vmalloc_or_module_addr);

/*
 * Walk a vmap address to the struct page it maps. Huge vmap mappings will
 * return the tail page that corresponds to the base page address, which
 * matches small vmap mappings.
 */
/*
 * 软件遍历 init_mm 页表，把 vmalloc/module VA 解析为 struct page。
 *
 * direct-map 地址可用 virt_to_page 做算术换算，vmalloc VA 与 PFN 没有这种关系，
 * 必须逐层读取页表。遇到 P4D/PUD/PMD huge leaf 时，返回与 VA 偏移对应的 tail
 * page，使结果与普通 PTE 映射一致。函数不加引用，调用者必须保证映射生命周期。
 */
struct page *vmalloc_to_page(const void *vmalloc_addr)
{
	unsigned long addr = (unsigned long) vmalloc_addr;
	struct page *page = NULL;
	pgd_t *pgd = pgd_offset_k(addr);
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;

	/*
	 * XXX we might need to change this if we add VIRTUAL_BUG_ON for
	 * architectures that do not vmalloc module space
	 */
	VIRTUAL_BUG_ON(!is_vmalloc_or_module_addr(vmalloc_addr));

	if (pgd_none(*pgd))
		return NULL;
	if (WARN_ON_ONCE(pgd_leaf(*pgd)))
		return NULL; /* XXX: no allowance for huge pgd */
	if (WARN_ON_ONCE(pgd_bad(*pgd)))
		return NULL;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d))
		return NULL;
	if (p4d_leaf(*p4d))
		/* leaf 首页加上区间内 base-page 偏移，得到当前 VA 对应的 page。 */
		return p4d_page(*p4d) + ((addr & ~P4D_MASK) >> PAGE_SHIFT);
	if (WARN_ON_ONCE(p4d_bad(*p4d)))
		return NULL;

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud))
		return NULL;
	if (pud_leaf(*pud))
		return pud_page(*pud) + ((addr & ~PUD_MASK) >> PAGE_SHIFT);
	if (WARN_ON_ONCE(pud_bad(*pud)))
		return NULL;

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return NULL;
	if (pmd_leaf(*pmd))
		return pmd_page(*pmd) + ((addr & ~PMD_MASK) >> PAGE_SHIFT);
	if (WARN_ON_ONCE(pmd_bad(*pmd)))
		return NULL;

	ptep = pte_offset_kernel(pmd, addr);
	pte = ptep_get(ptep);
	if (pte_present(pte))
		page = pte_page(pte);

	return page;
}
EXPORT_SYMBOL(vmalloc_to_page);

/*
 * Map a vmalloc()-space virtual address to the physical page frame number.
 */
unsigned long vmalloc_to_pfn(const void *vmalloc_addr)
{
	/* 继承 vmalloc_to_page 的生命周期前置条件；设备 PFN 映射不一定有 struct page。 */
	return page_to_pfn(vmalloc_to_page(vmalloc_addr));
}
EXPORT_SYMBOL(vmalloc_to_pfn);


/*** Global kva allocator ***/

/*
 * 全局 KVA 分配器管理的是虚拟地址，不是物理页。空闲 vmap_area 同时进入增强
 * 红黑树和地址有序链表：树按 subtree_max_size 剪枝寻找最低可用洞，链表 O(1)
 * 取得左右邻居用于合并。两者是同一逻辑索引，必须在 free_vmap_area_lock 下
 * 原子更新；只更新一边会导致重复分配或永久碎片。
 *
 * busy/lazy 区域再按虚拟地址分散到多个 vmap_node，降低全局锁竞争。这里的 node
 * 是软件锁分片，不是 NUMA node。小型完整空闲区间还可进入精确尺寸 pool，省去
 * 树搜索；压力路径会衰减 pool，把地址重新交回全局合并树。
 */

#define DEBUG_AUGMENT_PROPAGATE_CHECK 0
#define DEBUG_AUGMENT_LOWEST_MATCH_CHECK 0


static DEFINE_SPINLOCK(free_vmap_area_lock);
static bool vmap_initialized __read_mostly;

/*
 * This kmem_cache is used for vmap_area objects. Instead of
 * allocating from slab we reuse an object from this cache to
 * make things faster. Especially in "no edge" splitting of
 * free block.
 */
static struct kmem_cache *vmap_area_cachep;

/* 描述符使用专用 slab cache；中间切割空闲块时可预取第二个描述符，避免锁内睡眠。 */

/*
 * This linked list is used in pair with free_vmap_area_root.
 * It gives O(1) access to prev/next to perform fast coalescing.
 */
static LIST_HEAD(free_vmap_area_list);

/*
 * This augment red-black tree represents the free vmap space.
 * All vmap_area objects in this tree are sorted by va->va_start
 * address. It is used for allocation and merging when a vmap
 * object is released.
 *
 * Each vmap_area node contains a maximum available free block
 * of its sub-tree, right or left. Therefore it is possible to
 * find a lowest match of free area.
 */
static struct rb_root free_vmap_area_root = RB_ROOT;

/*
 * Preload a CPU with one object for "no edge" split case. The
 * aim is to get rid of allocations from the atomic context, thus
 * to use more permissive allocation masks.
 */
static DEFINE_PER_CPU(struct vmap_area *, ne_fit_preload_node);

/*
 * This structure defines a single, solid model where a list and
 * rb-tree are part of one entity protected by the lock. Nodes are
 * sorted in ascending order, thus for O(1) access to left/right
 * neighbors a list is used as well as for sequential traversal.
 */
struct rb_list {
	struct rb_root root;
	struct list_head head;
	spinlock_t lock;
};

/* rb_list 把红黑树、同序链表和保护锁封装成一个不可分割的区间索引。 */

/*
 * A fast size storage contains VAs up to 1M size. A pool consists
 * of linked between each other ready to go VAs of certain sizes.
 * An index in the pool-array corresponds to number of pages + 1.
 */
#define MAX_VA_SIZE_PAGES 256

struct vmap_pool {
	struct list_head head;
	unsigned long len;
};

/*
 * An effective vmap-node logic. Users make use of nodes instead
 * of a global heap. It allows to balance an access and mitigate
 * contention.
 */
static struct vmap_node {
	/* Simple size segregated storage. */
	struct vmap_pool pool[MAX_VA_SIZE_PAGES];
	spinlock_t pool_lock;
	bool skip_populate;

	/* Bookkeeping data of this node. */
	struct rb_list busy;
	struct rb_list lazy;

	/*
	 * Ready-to-free areas.
	 */
	struct list_head purge_list;
	struct work_struct purge_work;
	unsigned long nr_purged;
} single;

/*
 * 启动早期只有 single node，避免初始化分配器本身时产生循环依赖；正式初始化后
 * 再按机器规模扩展分片。性能随初始化阶段变化，但地址分配语义保持不变。
 */

/*
 * Initial setup consists of one single node, i.e. a balancing
 * is fully disabled. Later on, after vmap is initialized these
 * parameters are updated based on a system capacity.
 */
static struct vmap_node *vmap_nodes = &single;
static __read_mostly unsigned int nr_vmap_nodes = 1;
static __read_mostly unsigned int vmap_zone_size = 1;

/* A simple iterator over all vmap-nodes. */
#define for_each_vmap_node(vn)	\
	for ((vn) = &vmap_nodes[0];	\
		(vn) < &vmap_nodes[nr_vmap_nodes]; (vn)++)

static inline unsigned int
addr_to_node_id(unsigned long addr)
{
	/* 地址按固定 zone 条带化，再轮转到 node；相邻 zone 因此落到不同锁分片。 */
	return (addr / vmap_zone_size) % nr_vmap_nodes;
}

static inline struct vmap_node *
addr_to_node(unsigned long addr)
{
	return &vmap_nodes[addr_to_node_id(addr)];
}

static inline struct vmap_node *
id_to_node(unsigned int id)
{
	return &vmap_nodes[id % nr_vmap_nodes];
}

static inline unsigned int
node_to_id(struct vmap_node *node)
{
	/* Pointer arithmetic. */
	unsigned int id = node - vmap_nodes;

	if (likely(id < nr_vmap_nodes))
		return id;

	WARN_ONCE(1, "An address 0x%p is out-of-bounds.\n", node);
	return 0;
}

/*
 * We use the value 0 to represent "no node", that is why
 * an encoded value will be the node-id incremented by 1.
 * It is always greater then 0. A valid node_id which can
 * be encoded is [0:nr_vmap_nodes - 1]. If a passed node_id
 * is not valid 0 is returned.
 */
static unsigned int
encode_vn_id(unsigned int node_id)
{
	/* 低字节保存 VMAP 类型；node id 加一后放高位，编码 0 保留为“无来源”。 */
	/* Can store U8_MAX [0:254] nodes. */
	if (node_id < nr_vmap_nodes)
		return (node_id + 1) << BITS_PER_BYTE;

	/* Warn and no node encoded. */
	WARN_ONCE(1, "Encode wrong node id (%u)\n", node_id);
	return 0;
}

/*
 * Returns an encoded node-id, the valid range is within
 * [0:nr_vmap_nodes-1] values. Otherwise nr_vmap_nodes is
 * returned if extracted data is wrong.
 */
static unsigned int
decode_vn_id(unsigned int val)
{
	/* 编码 0 减一后自然成为 UINT_MAX，最终由有效性检查转换成无效 node 哨兵。 */
	unsigned int node_id = (val >> BITS_PER_BYTE) - 1;

	/* Can store U8_MAX [0:254] nodes. */
	if (node_id < nr_vmap_nodes)
		return node_id;

	/* If it was _not_ zero, warn. */
	WARN_ONCE(node_id != UINT_MAX,
		"Decode wrong node id (%d)\n", node_id);

	return nr_vmap_nodes;
}

static bool
is_vn_id_valid(unsigned int node_id)
{
	if (node_id < nr_vmap_nodes)
		return true;

	return false;
}

static __always_inline unsigned long
va_size(struct vmap_area *va)
{
	return (va->va_end - va->va_start);
}

static __always_inline unsigned long
get_subtree_max_size(struct rb_node *node)
{
	struct vmap_area *va;

	va = rb_entry_safe(node, struct vmap_area, rb_node);
	return va ? va->subtree_max_size : 0;
}

RB_DECLARE_CALLBACKS_MAX(static, free_vmap_area_rb_augment_cb,
	struct vmap_area, rb_node, unsigned long, subtree_max_size, va_size)

static void reclaim_and_purge_vmap_areas(void);
static BLOCKING_NOTIFIER_HEAD(vmap_notify_list);
static void drain_vmap_area_work(struct work_struct *work);
static DECLARE_WORK(drain_vmap_work, drain_vmap_area_work);

static __cacheline_aligned_in_smp atomic_long_t vmap_lazy_nr;

/*
 * 在按 va_start 排序的树中查找“包含 addr”的区间。返回指针不自动获得引用，
 * 内部调用者通常必须持有对应 node 锁，避免 vfree 并发摘除并释放描述符。
 */
static struct vmap_area *__find_vmap_area(unsigned long addr, struct rb_root *root)
{
	struct rb_node *n = root->rb_node;

	addr = (unsigned long)kasan_reset_tag((void *)addr);

	while (n) {
		struct vmap_area *va;

		va = rb_entry(n, struct vmap_area, rb_node);
		if (addr < va->va_start)
			n = n->rb_left;
		else if (addr >= va->va_end)
			n = n->rb_right;
		else
			return va;
	}

	return NULL;
}

/* Look up the first VA which satisfies addr < va_end, NULL if none. */
static struct vmap_area *
__find_vmap_area_exceed_addr(unsigned long addr, struct rb_root *root)
{
	struct vmap_area *va = NULL;
	struct rb_node *n = root->rb_node;

	addr = (unsigned long)kasan_reset_tag((void *)addr);

	while (n) {
		struct vmap_area *tmp;

		tmp = rb_entry(n, struct vmap_area, rb_node);
		if (tmp->va_end > addr) {
			va = tmp;
			if (tmp->va_start <= addr)
				break;

			n = n->rb_left;
		} else
			n = n->rb_right;
	}

	return va;
}

/*
 * Returns a node where a first VA, that satisfies addr < va_end, resides.
 * If success, a node is locked. A user is responsible to unlock it when a
 * VA is no longer needed to be accessed.
 *
 * Returns NULL if nothing found.
 */
static struct vmap_node *
find_vmap_area_exceed_addr_lock(unsigned long addr, struct vmap_area **va)
{
	/*
	 * vread 需要找到所有分片中第一个 end 超过 addr 的区间。第一轮逐 node 只记录
	 * 最低候选地址并释放锁，第二轮重新锁定候选所属 node 并验证；若候选并发消失
	 * 就重搜。成功返回时锁保持持有，把查找与消费之间的 TOCTOU 窗口关闭。
	 */
	unsigned long va_start_lowest;
	struct vmap_node *vn;

repeat:
	va_start_lowest = 0;

	for_each_vmap_node(vn) {
		spin_lock(&vn->busy.lock);
		*va = __find_vmap_area_exceed_addr(addr, &vn->busy.root);

		if (*va)
			if (!va_start_lowest || (*va)->va_start < va_start_lowest)
				va_start_lowest = (*va)->va_start;
		spin_unlock(&vn->busy.lock);
	}

	/*
	 * Check if found VA exists, it might have gone away.  In this case we
	 * repeat the search because a VA has been removed concurrently and we
	 * need to proceed to the next one, which is a rare case.
	 */
	if (va_start_lowest) {
		vn = addr_to_node(va_start_lowest);

		spin_lock(&vn->busy.lock);
		*va = __find_vmap_area(va_start_lowest, &vn->busy.root);

		if (*va)
			return vn;

		spin_unlock(&vn->busy.lock);
		goto repeat;
	}

	return NULL;
}

/*
 * This function returns back addresses of parent node
 * and its left or right link for further processing.
 *
 * Otherwise NULL is returned. In that case all further
 * steps regarding inserting of conflicting overlap range
 * have to be declined and actually considered as a bug.
 */
static __always_inline struct rb_node **
find_va_links(struct vmap_area *va,
	struct rb_root *root, struct rb_node *from,
	struct rb_node **parent)
{
	/*
	 * 一次搜索同时确定 parent/左右 child 槽并检查区间重叠。返回 rb_node ** 让
	 * rb_link_node 可直接写入目标槽；NULL 表示发现 overlap，这是内部索引损坏，
	 * 不是普通的空间不足。
	 */
	struct vmap_area *tmp_va;
	struct rb_node **link;

	if (root) {
		link = &root->rb_node;
		if (unlikely(!*link)) {
			*parent = NULL;
			return link;
		}
	} else {
		link = &from;
	}

	/*
	 * Go to the bottom of the tree. When we hit the last point
	 * we end up with parent rb_node and correct direction, i name
	 * it link, where the new va->rb_node will be attached to.
	 */
	do {
		tmp_va = rb_entry(*link, struct vmap_area, rb_node);

		/*
		 * During the traversal we also do some sanity check.
		 * Trigger the BUG() if there are sides(left/right)
		 * or full overlaps.
		 */
		if (va->va_end <= tmp_va->va_start)
			link = &(*link)->rb_left;
		else if (va->va_start >= tmp_va->va_end)
			link = &(*link)->rb_right;
		else {
			WARN(1, "vmalloc bug: 0x%lx-0x%lx overlaps with 0x%lx-0x%lx\n",
				va->va_start, va->va_end, tmp_va->va_start, tmp_va->va_end);

			return NULL;
		}
	} while (*link);

	*parent = &tmp_va->rb_node;
	return link;
}

static __always_inline struct list_head *
get_va_next_sibling(struct rb_node *parent, struct rb_node **link)
{
	/* 根据待插入的 child 槽推导地址有序链表中的后继，供合并同时检查左右邻居。 */
	struct list_head *list;

	if (unlikely(!parent))
		/*
		 * The red-black tree where we try to find VA neighbors
		 * before merging or inserting is empty, i.e. it means
		 * there is no free vmap space. Normally it does not
		 * happen but we handle this case anyway.
		 */
		return NULL;

	list = &rb_entry(parent, struct vmap_area, rb_node)->list;
	return (&parent->rb_right == link ? list->next : list);
}

static __always_inline void
__link_va(struct vmap_area *va, struct rb_root *root,
	struct rb_node *parent, struct rb_node **link,
	struct list_head *head, bool augment)
{
	/*
	 * parent/link 不仅决定树位置，也能推导地址有序链表的前驱。先连接并平衡树，
	 * 再把同一对象插入链表；整个过程由调用者持锁，读者不会看到半更新索引。
	 * free tree 使用 augmented 插入，busy/lazy tree 使用普通红黑树插入。
	 */
	/*
	 * VA is still not in the list, but we can
	 * identify its future previous list_head node.
	 */
	if (likely(parent)) {
		head = &rb_entry(parent, struct vmap_area, rb_node)->list;
		if (&parent->rb_right != link)
			head = head->prev;
	}

	/* Insert to the rb-tree */
	rb_link_node(&va->rb_node, parent, link);
	if (augment) {
		/*
		 * Some explanation here. Just perform simple insertion
		 * to the tree. We do not set va->subtree_max_size to
		 * its current size before calling rb_insert_augmented().
		 * It is because we populate the tree from the bottom
		 * to parent levels when the node _is_ in the tree.
		 *
		 * Therefore we set subtree_max_size to zero after insertion,
		 * to let __augment_tree_propagate_from() puts everything to
		 * the correct order later on.
		 */
		rb_insert_augmented(&va->rb_node,
			root, &free_vmap_area_rb_augment_cb);
		va->subtree_max_size = 0;
	} else {
		rb_insert_color(&va->rb_node, root);
	}

	/* Address-sort this list */
	list_add(&va->list, head);
}

static __always_inline void
link_va(struct vmap_area *va, struct rb_root *root,
	struct rb_node *parent, struct rb_node **link,
	struct list_head *head)
{
	/* busy/lazy tree 不按空洞容量搜索，无需维护 subtree_max_size。 */
	__link_va(va, root, parent, link, head, false);
}

static __always_inline void
link_va_augment(struct vmap_area *va, struct rb_root *root,
	struct rb_node *parent, struct rb_node **link,
	struct list_head *head)
{
	/* free tree 插入必须走增强回调，否则 first-fit 可能错误剪掉可用子树。 */
	__link_va(va, root, parent, link, head, true);
}

static __always_inline void
__unlink_va(struct vmap_area *va, struct rb_root *root, bool augment)
{
	/*
	 * 删除必须与插入类型配对：free tree 的旋转需要同步修复最大空洞，普通树则不
	 * 维护增强值。最后清除 rb_node 链接状态，可检测同一描述符被重复删除。
	 */
	if (WARN_ON(RB_EMPTY_NODE(&va->rb_node)))
		return;

	if (augment)
		rb_erase_augmented(&va->rb_node,
			root, &free_vmap_area_rb_augment_cb);
	else
		rb_erase(&va->rb_node, root);

	list_del_init(&va->list);
	RB_CLEAR_NODE(&va->rb_node);
}

static __always_inline void
unlink_va(struct vmap_area *va, struct rb_root *root)
{
	/* 普通索引删除只维护红黑树平衡与地址链表。 */
	__unlink_va(va, root, false);
}

static __always_inline void
unlink_va_augment(struct vmap_area *va, struct rb_root *root)
{
	/* 空闲索引删除额外维护祖先最大洞信息。 */
	__unlink_va(va, root, true);
}

#if DEBUG_AUGMENT_PROPAGATE_CHECK
/*
 * Gets called when remove the node and rotate.
 */
static __always_inline unsigned long
compute_subtree_max_size(struct vmap_area *va)
{
	/* 调试构建从节点自身及两个子树独立重算真值，用来校验增量维护结果。 */
	return max3(va_size(va),
		get_subtree_max_size(va->rb_node.rb_left),
		get_subtree_max_size(va->rb_node.rb_right));
}

static void
augment_tree_propagate_check(void)
{
	/* 地址链表覆盖所有 free 节点，适合低频调试时做全树不变量审计。 */
	struct vmap_area *va;
	unsigned long computed_size;

	list_for_each_entry(va, &free_vmap_area_list, list) {
		computed_size = compute_subtree_max_size(va);
		if (computed_size != va->subtree_max_size)
			pr_emerg("tree is corrupted: %lu, %lu\n",
				va_size(va), va->subtree_max_size);
	}
}
#endif

/*
 * This function populates subtree_max_size from bottom to upper
 * levels starting from VA point. The propagation must be done
 * when VA size is modified by changing its va_start/va_end. Or
 * in case of newly inserting of VA to the tree.
 *
 * It means that __augment_tree_propagate_from() must be called:
 * - After VA has been inserted to the tree(free path);
 * - After VA has been shrunk(allocation path);
 * - After VA has been increased(merging path).
 *
 * Please note that, it does not mean that upper parent nodes
 * and their subtree_max_size are recalculated all the time up
 * to the root node.
 *
 *       4--8
 *        /\
 *       /  \
 *      /    \
 *    2--2  8--8
 *
 * For example if we modify the node 4, shrinking it to 2, then
 * no any modification is required. If we shrink the node 2 to 1
 * its subtree_max_size is updated only, and set to 1. If we shrink
 * the node 8 to 6, then its subtree_max_size is set to 6 and parent
 * node becomes 4--6.
 */
static __always_inline void
augment_tree_propagate_from(struct vmap_area *va)
{
	/*
	 * 区间 start/end 改变后，节点到根路径上的 subtree_max_size 可能失效。回调
	 * 自底向上重算，遇到值未变化即可提前停止，避免每次都扫描整棵树。
	 */
	/*
	 * Populate the tree from bottom towards the root until
	 * the calculated maximum available size of checked node
	 * is equal to its current one.
	 */
	free_vmap_area_rb_augment_cb_propagate(&va->rb_node, NULL);

#if DEBUG_AUGMENT_PROPAGATE_CHECK
	augment_tree_propagate_check();
#endif
}

static void
insert_vmap_area(struct vmap_area *va,
	struct rb_root *root, struct list_head *head)
{
	/* 查找唯一不重叠插槽并原子更新普通树/链表；重叠时 find_va_links 已报警。 */
	struct rb_node **link;
	struct rb_node *parent;

	link = find_va_links(va, root, NULL, &parent);
	if (link)
		link_va(va, root, parent, link, head);
}

static void
insert_vmap_area_augment(struct vmap_area *va,
	struct rb_node *from, struct rb_root *root,
	struct list_head *head)
{
	/* from 可把分裂后的新余块从已知邻近节点开始定位，避免重新从根搜索。 */
	struct rb_node **link;
	struct rb_node *parent;

	if (from)
		link = find_va_links(va, NULL, from, &parent);
	else
		link = find_va_links(va, root, NULL, &parent);

	if (link) {
		link_va_augment(va, root, parent, link, head);
		augment_tree_propagate_from(va);
	}
}

/*
 * Merge de-allocated chunk of VA memory with previous
 * and next free blocks. If coalesce is not done a new
 * free area is inserted. If VA has been merged, it is
 * freed.
 *
 * Please note, it can return NULL in case of overlap
 * ranges, followed by WARN() report. Despite it is a
 * buggy behaviour, a system can be alive and keep
 * ongoing.
 */
static __always_inline struct vmap_area *
__merge_or_add_vmap_area(struct vmap_area *va,
	struct rb_root *root, struct list_head *head, bool augment)
{
	/*
	 * 释放区间必须与左右相邻空闲块尽可能合并，维持“空闲区间互不相邻”的规范
	 * 形式。若先与 next 合并、随后还要与 prev 合并，必须先把 next 从树中摘除，
	 * 再扩展 prev；否则增强值传播可能基于一个已改变身份的树节点。
	 */
	struct vmap_area *sibling;
	struct list_head *next;
	struct rb_node **link;
	struct rb_node *parent;
	bool merged = false;

	/*
	 * Find a place in the tree where VA potentially will be
	 * inserted, unless it is merged with its sibling/siblings.
	 */
	link = find_va_links(va, root, NULL, &parent);
	if (!link)
		return NULL;

	/*
	 * Get next node of VA to check if merging can be done.
	 */
	next = get_va_next_sibling(parent, link);
	if (unlikely(next == NULL))
		goto insert;

	/*
	 * start            end
	 * |                |
	 * |<------VA------>|<-----Next----->|
	 *                  |                |
	 *                  start            end
	 */
	if (next != head) {
		sibling = list_entry(next, struct vmap_area, list);
		if (sibling->va_start == va->va_end) {
			sibling->va_start = va->va_start;

			/* Free vmap_area object. */
			kmem_cache_free(vmap_area_cachep, va);

			/* Point to the new merged area. */
			va = sibling;
			merged = true;
		}
	}

	/*
	 * start            end
	 * |                |
	 * |<-----Prev----->|<------VA------>|
	 *                  |                |
	 *                  start            end
	 */
	if (next->prev != head) {
		sibling = list_entry(next->prev, struct vmap_area, list);
		if (sibling->va_end == va->va_start) {
			/*
			 * If both neighbors are coalesced, it is important
			 * to unlink the "next" node first, followed by merging
			 * with "previous" one. Otherwise the tree might not be
			 * fully populated if a sibling's augmented value is
			 * "normalized" because of rotation operations.
			 */
			if (merged)
				__unlink_va(va, root, augment);

			sibling->va_end = va->va_end;

			/* Free vmap_area object. */
			kmem_cache_free(vmap_area_cachep, va);

			/* Point to the new merged area. */
			va = sibling;
			merged = true;
		}
	}

insert:
	if (!merged)
		__link_va(va, root, parent, link, head, augment);

	return va;
}

static __always_inline struct vmap_area *
merge_or_add_vmap_area(struct vmap_area *va,
	struct rb_root *root, struct list_head *head)
{
	/* 普通树版本用于不依赖空洞容量的索引。 */
	return __merge_or_add_vmap_area(va, root, head, false);
}

static __always_inline struct vmap_area *
merge_or_add_vmap_area_augment(struct vmap_area *va,
	struct rb_root *root, struct list_head *head)
{
	/* free tree 合并后以最终存活节点为起点修复祖先增强值。 */
	va = __merge_or_add_vmap_area(va, root, head, true);
	if (va)
		augment_tree_propagate_from(va);

	return va;
}

static __always_inline bool
is_within_this_va(struct vmap_area *va, unsigned long size,
	unsigned long align, unsigned long vstart)
{
	/* 把请求起点推进到 max(洞起点,vstart) 后对齐，并显式防范加法回绕。 */
	unsigned long nva_start_addr;

	if (va->va_start > vstart)
		nva_start_addr = ALIGN(va->va_start, align);
	else
		nva_start_addr = ALIGN(vstart, align);

	/* Can be overflowed due to big size or alignment. */
	if (nva_start_addr + size < nva_start_addr ||
			nva_start_addr < vstart)
		return false;

	return (nva_start_addr + size <= va->va_end);
}

/*
 * Find the first free block(lowest start address) in the tree,
 * that will accomplish the request corresponding to passing
 * parameters. Please note, with an alignment bigger than PAGE_SIZE,
 * a search length is adjusted to account for worst case alignment
 * overhead.
 */
static __always_inline struct vmap_area *
find_vmap_lowest_match(struct rb_root *root, unsigned long size,
	unsigned long align, unsigned long vstart, bool adjust_search_size)
{
	/*
	 * 搜索目标是地址最低的合法 first-fit。subtree_max_size 能排除容量不足的整棵
	 * 子树；对齐和 vstart 又可能使“容量够”的节点实际不可用，因此算法必要时
	 * 回溯祖先并进入尚未检查的右子树。size+align-1 是对齐损耗的保守上界。
	 */
	struct vmap_area *va;
	struct rb_node *node;
	unsigned long length;

	/* Start from the root. */
	node = root->rb_node;

	/* Adjust the search size for alignment overhead. */
	length = adjust_search_size ? size + align - 1 : size;

	while (node) {
		va = rb_entry(node, struct vmap_area, rb_node);

		if (get_subtree_max_size(node->rb_left) >= length &&
				vstart < va->va_start) {
			node = node->rb_left;
		} else {
			if (is_within_this_va(va, size, align, vstart))
				return va;

			/*
			 * Does not make sense to go deeper towards the right
			 * sub-tree if it does not have a free block that is
			 * equal or bigger to the requested search length.
			 */
			if (get_subtree_max_size(node->rb_right) >= length) {
				node = node->rb_right;
				continue;
			}

			/*
			 * OK. We roll back and find the first right sub-tree,
			 * that will satisfy the search criteria. It can happen
			 * due to "vstart" restriction or an alignment overhead
			 * that is bigger then PAGE_SIZE.
			 */
			while ((node = rb_parent(node))) {
				va = rb_entry(node, struct vmap_area, rb_node);
				if (is_within_this_va(va, size, align, vstart))
					return va;

				if (get_subtree_max_size(node->rb_right) >= length &&
						vstart <= va->va_start) {
					/*
					 * Shift the vstart forward. Please note, we update it with
					 * parent's start address adding "1" because we do not want
					 * to enter same sub-tree after it has already been checked
					 * and no suitable free block found there.
					 */
					vstart = va->va_start + 1;
					node = node->rb_right;
					break;
				}
			}
		}
	}

	return NULL;
}

#if DEBUG_AUGMENT_LOWEST_MATCH_CHECK
#include <linux/random.h>

static struct vmap_area *
find_vmap_lowest_linear_match(struct list_head *head, unsigned long size,
	unsigned long align, unsigned long vstart)
{
	struct vmap_area *va;

	list_for_each_entry(va, head, list) {
		if (!is_within_this_va(va, size, align, vstart))
			continue;

		return va;
	}

	return NULL;
}

static void
find_vmap_lowest_match_check(struct rb_root *root, struct list_head *head,
			     unsigned long size, unsigned long align)
{
	struct vmap_area *va_1, *va_2;
	unsigned long vstart;
	unsigned int rnd;

	get_random_bytes(&rnd, sizeof(rnd));
	vstart = VMALLOC_START + rnd;

	va_1 = find_vmap_lowest_match(root, size, align, vstart, false);
	va_2 = find_vmap_lowest_linear_match(head, size, align, vstart);

	if (va_1 != va_2)
		pr_emerg("not lowest: t: 0x%p, l: 0x%p, v: 0x%lx\n",
			va_1, va_2, vstart);
}
#endif

enum fit_type {
	NOTHING_FIT = 0,
	FL_FIT_TYPE = 1,	/* full fit */
	LE_FIT_TYPE = 2,	/* left edge fit */
	RE_FIT_TYPE = 3,	/* right edge fit */
	NE_FIT_TYPE = 4		/* no edge fit */
};

static __always_inline enum fit_type
classify_va_fit_type(struct vmap_area *va,
	unsigned long nva_start_addr, unsigned long size)
{
	/* 分类决定裁剪需要零、一个还是两个余块；NOTHING_FIT 表示搜索不变量已破坏。 */
	enum fit_type type;

	/* Check if it is within VA. */
	if (nva_start_addr < va->va_start ||
			nva_start_addr + size > va->va_end)
		return NOTHING_FIT;

	/* Now classify. */
	if (va->va_start == nva_start_addr) {
		if (va->va_end == nva_start_addr + size)
			type = FL_FIT_TYPE;
		else
			type = LE_FIT_TYPE;
	} else if (va->va_end == nva_start_addr + size) {
		type = RE_FIT_TYPE;
	} else {
		type = NE_FIT_TYPE;
	}

	return type;
}

static __always_inline int
va_clip(struct rb_root *root, struct list_head *head,
		struct vmap_area *va, unsigned long nva_start_addr,
		unsigned long size)
{
	/*
	 * 从命中的空闲区间扣除请求范围。完整命中删除节点；贴左/右边只缩一个边界；
	 * 从中间切割会产生两个余块，需要额外 vmap_area。成功后所有余块仍在树/链表
	 * 中有序且增强值正确，所请求范围则完全从 free 索引消失。
	 */
	struct vmap_area *lva = NULL;
	enum fit_type type = classify_va_fit_type(va, nva_start_addr, size);

	if (type == FL_FIT_TYPE) {
		/*
		 * No need to split VA, it fully fits.
		 *
		 * |               |
		 * V      NVA      V
		 * |---------------|
		 */
		unlink_va_augment(va, root);
		kmem_cache_free(vmap_area_cachep, va);
	} else if (type == LE_FIT_TYPE) {
		/*
		 * Split left edge of fit VA.
		 *
		 * |       |
		 * V  NVA  V   R
		 * |-------|-------|
		 */
		va->va_start += size;
	} else if (type == RE_FIT_TYPE) {
		/*
		 * Split right edge of fit VA.
		 *
		 *         |       |
		 *     L   V  NVA  V
		 * |-------|-------|
		 */
		va->va_end = nva_start_addr;
	} else if (type == NE_FIT_TYPE) {
		/*
		 * Split no edge of fit VA.
		 *
		 *     |       |
		 *   L V  NVA  V R
		 * |---|-------|---|
		 */
		lva = __this_cpu_xchg(ne_fit_preload_node, NULL);
		if (unlikely(!lva)) {
			/*
			 * For percpu allocator we do not do any pre-allocation
			 * and leave it as it is. The reason is it most likely
			 * never ends up with NE_FIT_TYPE splitting. In case of
			 * percpu allocations offsets and sizes are aligned to
			 * fixed align request, i.e. RE_FIT_TYPE and FL_FIT_TYPE
			 * are its main fitting cases.
			 *
			 * There are a few exceptions though, as an example it is
			 * a first allocation (early boot up) when we have "one"
			 * big free space that has to be split.
			 *
			 * Also we can hit this path in case of regular "vmap"
			 * allocations, if "this" current CPU was not preloaded.
			 * See the comment in alloc_vmap_area() why. If so, then
			 * GFP_NOWAIT is used instead to get an extra object for
			 * split purpose. That is rare and most time does not
			 * occur.
			 *
			 * What happens if an allocation gets failed. Basically,
			 * an "overflow" path is triggered to purge lazily freed
			 * areas to free some memory, then, the "retry" path is
			 * triggered to repeat one more time. See more details
			 * in alloc_vmap_area() function.
			 */
			lva = kmem_cache_alloc(vmap_area_cachep, GFP_NOWAIT);
			if (!lva)
				return -ENOMEM;
		}

		/*
		 * Build the remainder.
		 */
		lva->va_start = va->va_start;
		lva->va_end = nva_start_addr;

		/*
		 * Shrink this VA to remaining size.
		 */
		va->va_start = nva_start_addr + size;
	} else {
		return -EINVAL;
	}

	if (type != FL_FIT_TYPE) {
		augment_tree_propagate_from(va);

		if (lva)	/* type == NE_FIT_TYPE */
			insert_vmap_area_augment(lva, &va->rb_node, root, head);
	}

	return 0;
}

static unsigned long
va_alloc(struct vmap_area *va,
		struct rb_root *root, struct list_head *head,
		unsigned long size, unsigned long align,
		unsigned long vstart, unsigned long vend)
{
	/* 将候选洞转换为已对齐地址，并在裁剪 free tree 前最后验证调用者 vend 上界。 */
	unsigned long nva_start_addr;
	int ret;

	if (va->va_start > vstart)
		nva_start_addr = ALIGN(va->va_start, align);
	else
		nva_start_addr = ALIGN(vstart, align);

	/* Check the "vend" restriction. */
	if (nva_start_addr + size > vend)
		return -ERANGE;

	/* Update the free vmap_area. */
	ret = va_clip(root, head, va, nva_start_addr, size);
	if (WARN_ON_ONCE(ret))
		return ret;

	return nva_start_addr;
}

/*
 * Returns a start address of the newly allocated area, if success.
 * Otherwise an error value is returned that indicates failure.
 */
static __always_inline unsigned long
__alloc_vmap_area(struct rb_root *root, struct list_head *head,
	unsigned long size, unsigned long align,
	unsigned long vstart, unsigned long vend)
{
	/*
	 * 增强树搜索先得到最低候选洞，va_alloc 再执行不可逆裁剪。短且精确限定的窗口
	 * 不能用 align-1 扩大搜索长度，否则本来恰好可容纳的洞会被错误剪枝。
	 */
	bool adjust_search_size = true;
	unsigned long nva_start_addr;
	struct vmap_area *va;

	/*
	 * Do not adjust when:
	 *   a) align <= PAGE_SIZE, because it does not make any sense.
	 *      All blocks(their start addresses) are at least PAGE_SIZE
	 *      aligned anyway;
	 *   b) a short range where a requested size corresponds to exactly
	 *      specified [vstart:vend] interval and an alignment > PAGE_SIZE.
	 *      With adjusted search length an allocation would not succeed.
	 */
	if (align <= PAGE_SIZE || (align > PAGE_SIZE && (vend - vstart) == size))
		adjust_search_size = false;

	va = find_vmap_lowest_match(root, size, align, vstart, adjust_search_size);
	if (unlikely(!va))
		return -ENOENT;

	nva_start_addr = va_alloc(va, root, head, size, align, vstart, vend);

#if DEBUG_AUGMENT_LOWEST_MATCH_CHECK
	if (!IS_ERR_VALUE(nva_start_addr))
		find_vmap_lowest_match_check(root, head, size, align);
#endif

	return nva_start_addr;
}

/*
 * Free a region of KVA allocated by alloc_vmap_area
 */
static void free_vmap_area(struct vmap_area *va)
{
	/*
	 * 用于尚不需要 lazy TLB 协议的回滚/纯地址释放：先从所属 busy 分片摘除，再在
	 * 全局 free 锁下合并。调用者必须保证页表已不存在或从未建立。
	 */
	struct vmap_node *vn = addr_to_node(va->va_start);

	/*
	 * Remove from the busy tree/list.
	 */
	spin_lock(&vn->busy.lock);
	unlink_va(va, &vn->busy.root);
	spin_unlock(&vn->busy.lock);

	/*
	 * Insert/Merge it back to the free tree/list.
	 */
	spin_lock(&free_vmap_area_lock);
	merge_or_add_vmap_area_augment(va, &free_vmap_area_root, &free_vmap_area_list);
	spin_unlock(&free_vmap_area_lock);
}

static inline void
preload_this_cpu_lock(spinlock_t *lock, gfp_t gfp_mask, int node)
{
	/*
	 * 中间裁剪需要额外描述符，但 free-tree 自旋锁内不能睡眠分配。先在锁外预取，
	 * 再持锁以 cmpxchg 放入 per-CPU 单槽；竞争输掉的临时对象立即释放。
	 */
	struct vmap_area *va = NULL, *tmp;

	/*
	 * Preload this CPU with one extra vmap_area object. It is used
	 * when fit type of free area is NE_FIT_TYPE. It guarantees that
	 * a CPU that does an allocation is preloaded.
	 *
	 * We do it in non-atomic context, thus it allows us to use more
	 * permissive allocation masks to be more stable under low memory
	 * condition and high memory pressure.
	 */
	if (!this_cpu_read(ne_fit_preload_node))
		va = kmem_cache_alloc_node(vmap_area_cachep, gfp_mask, node);

	spin_lock(lock);

	tmp = NULL;
	if (va && !__this_cpu_try_cmpxchg(ne_fit_preload_node, &tmp, va))
		kmem_cache_free(vmap_area_cachep, va);
}

static struct vmap_pool *
size_to_va_pool(struct vmap_node *vn, unsigned long size)
{
	/* 小区间按精确页数分桶；超过上限不缓存，直接回全局 free tree 以利合并。 */
	unsigned int idx = (size - 1) / PAGE_SIZE;

	if (idx < MAX_VA_SIZE_PAGES)
		return &vn->pool[idx];

	return NULL;
}

static bool
node_pool_add_va(struct vmap_node *n, struct vmap_area *va)
{
	/* pool 保存已完成必要 flush、可立即复用的完整 VA；len 供无锁 shrinker 估算。 */
	struct vmap_pool *vp;

	vp = size_to_va_pool(n, va_size(va));
	if (!vp)
		return false;

	spin_lock(&n->pool_lock);
	list_add(&va->list, &vp->head);
	WRITE_ONCE(vp->len, vp->len + 1);
	spin_unlock(&n->pool_lock);

	return true;
}

static struct vmap_area *
node_pool_del_va(struct vmap_node *vn, unsigned long size,
		unsigned long align, unsigned long vstart,
		unsigned long vend)
{
	/*
	 * 只查精确尺寸桶以保持 O(1)。首项若不满足本次更强对齐，就轮转到尾部而不扫描
	 * 整桶，控制热路径延迟；范围校验失败意味着 pool 污染，报警且不返回该对象。
	 */
	struct vmap_area *va = NULL;
	struct vmap_pool *vp;
	int err = 0;

	vp = size_to_va_pool(vn, size);
	if (!vp || list_empty(&vp->head))
		return NULL;

	spin_lock(&vn->pool_lock);
	if (!list_empty(&vp->head)) {
		va = list_first_entry(&vp->head, struct vmap_area, list);

		if (IS_ALIGNED(va->va_start, align)) {
			/*
			 * Do some sanity check and emit a warning
			 * if one of below checks detects an error.
			 */
			err |= (va_size(va) != size);
			err |= (va->va_start < vstart);
			err |= (va->va_end > vend);

			if (!WARN_ON_ONCE(err)) {
				list_del_init(&va->list);
				WRITE_ONCE(vp->len, vp->len - 1);
			} else {
				va = NULL;
			}
		} else {
			list_move_tail(&va->list, &vp->head);
			va = NULL;
		}
	}
	spin_unlock(&vn->pool_lock);

	return va;
}

static struct vmap_area *
node_alloc(unsigned long size, unsigned long align,
		unsigned long vstart, unsigned long vend,
		unsigned long *addr, unsigned int *vn_id)
{
	/*
	 * 节点 pool 只服务完整标准 vmalloc 窗口，特殊窗口仍需全局树精确搜索。CPU id
	 * 选择回收亲和节点，编码后的 vn_id 随 allocation 传递，未来释放可回到同一 pool。
	 */
	struct vmap_area *va;

	*vn_id = 0;
	*addr = -EINVAL;

	/*
	 * Fallback to a global heap if not vmalloc or there
	 * is only one node.
	 */
	if (vstart != VMALLOC_START || vend != VMALLOC_END ||
			nr_vmap_nodes == 1)
		return NULL;

	*vn_id = raw_smp_processor_id() % nr_vmap_nodes;
	va = node_pool_del_va(id_to_node(*vn_id), size, align, vstart, vend);
	*vn_id = encode_vn_id(*vn_id);

	if (va)
		*addr = va->va_start;

	return va;
}

static inline void setup_vmalloc_vm(struct vm_struct *vm,
	struct vmap_area *va, unsigned long flags, const void *caller)
{
	/* 将底层 VA 与高层 vm_struct 双向关联；调用者随后再填 pages/nr_pages 等资源字段。 */
	vm->flags = flags;
	vm->addr = (void *)va->va_start;
	vm->size = vm->requested_size = va_size(va);
	vm->caller = caller;
	va->vm = vm;
}

/*
 * Allocate a region of KVA of the specified size and alignment, within the
 * vstart and vend. If vm is passed in, the two will also be bound.
 */
static struct vmap_area *alloc_vmap_area(unsigned long size,
				unsigned long align,
				unsigned long vstart, unsigned long vend,
				int node, gfp_t gfp_mask,
				unsigned long va_flags, struct vm_struct *vm)
{
	/*
	 * 完整 VA 分配策略：先尝试 node 精确尺寸 pool，miss 后分配描述符并搜索全局
	 * free tree；允许睡眠时，地址耗尽会先 purge lazy VA，再通知外部缓存释放空间。
	 * 成功区间最终进入 busy tree 并建立 KASAN shadow；失败不留下可发现对象。
	 */
	struct vmap_node *vn;
	struct vmap_area *va;
	unsigned long freed;
	unsigned long addr;
	unsigned int vn_id;
	bool allow_block;
	int purged = 0;
	int ret;

	if (unlikely(!size || offset_in_page(size) || !is_power_of_2(align)))
		return ERR_PTR(-EINVAL);

	if (unlikely(!vmap_initialized))
		return ERR_PTR(-EBUSY);

	/* Only reclaim behaviour flags are relevant. */
	gfp_mask = gfp_mask & GFP_RECLAIM_MASK;
	allow_block = gfpflags_allow_blocking(gfp_mask);
	might_sleep_if(allow_block);

	/*
	 * If a VA is obtained from a global heap(if it fails here)
	 * it is anyway marked with this "vn_id" so it is returned
	 * to this pool's node later. Such way gives a possibility
	 * to populate pools based on users demand.
	 *
	 * On success a ready to go VA is returned.
	 */
	va = node_alloc(size, align, vstart, vend, &addr, &vn_id);
	if (!va) {
		va = kmem_cache_alloc_node(vmap_area_cachep, gfp_mask, node);
		if (unlikely(!va))
			return ERR_PTR(-ENOMEM);

		/*
		 * Only scan the relevant parts containing pointers to other objects
		 * to avoid false negatives.
		 */
		kmemleak_scan_area(&va->rb_node, SIZE_MAX, gfp_mask);
	}

retry:
	/* 只有尚无可用地址时才持全局锁搜索；pool 命中直接跳过这一慢路径。 */
	if (IS_ERR_VALUE(addr)) {
		preload_this_cpu_lock(&free_vmap_area_lock, gfp_mask, node);
		addr = __alloc_vmap_area(&free_vmap_area_root, &free_vmap_area_list,
			size, align, vstart, vend);
		spin_unlock(&free_vmap_area_lock);

		/*
		 * This is not a fast path.  Check if yielding is needed. This
		 * is the only reschedule point in the vmalloc() path.
		 */
		if (allow_block)
			cond_resched();
	}

	trace_alloc_vmap_area(addr, size, align, vstart, vend, IS_ERR_VALUE(addr));

	/*
	 * If an allocation fails, the error value is
	 * returned. Therefore trigger the overflow path.
	 */
	if (IS_ERR_VALUE(addr)) {
		if (allow_block)
			goto overflow;

		/*
		 * We can not trigger any reclaim logic because
		 * sleeping is not allowed, thus fail an allocation.
		 */
		goto out_free_va;
	}

	/* 地址确定后初始化描述符，并把来源 node 编入 flags，供释放后优先回填 pool。 */
	va->va_start = addr;
	va->va_end = addr + size;
	va->vm = NULL;
	va->flags = (va_flags | vn_id);

	if (vm) {
		vm->addr = (void *)va->va_start;
		vm->size = va_size(va);
		va->vm = vm;
	}

	vn = addr_to_node(va->va_start);

	/* 插入 busy tree 是软件发布点：此后地址查询和诊断接口可以发现该区域。 */
	spin_lock(&vn->busy.lock);
	insert_vmap_area(va, &vn->busy.root, &vn->busy.head);
	spin_unlock(&vn->busy.lock);

	BUG_ON(!IS_ALIGNED(va->va_start, align));
	BUG_ON(va->va_start < vstart);
	BUG_ON(va->va_end > vend);

	ret = kasan_populate_vmalloc(addr, size, gfp_mask);
	if (ret) {
		/* shadow 创建失败时区域尚未交给调用者，可同步从 busy 撤销并归还 free tree。 */
		free_vmap_area(va);
		return ERR_PTR(ret);
	}

	return va;

overflow:
	/*
	 * 到这里仅持有未发布描述符，没有 VA、页表或物理页。回收只尝试有限阶段：
	 * purge 一次，再调用 notifier；只有确实释放了地址才重试，避免无限回收循环。
	 */
	if (!purged) {
		reclaim_and_purge_vmap_areas();
		purged = 1;
		goto retry;
	}

	freed = 0;
	blocking_notifier_call_chain(&vmap_notify_list, 0, &freed);

	if (freed > 0) {
		purged = 0;
		goto retry;
	}

	if (!(gfp_mask & __GFP_NOWARN) && printk_ratelimit())
		pr_warn("vmalloc_node_range for size %lu failed: Address range restricted to %#lx - %#lx\n",
				size, vstart, vend);

out_free_va:
	kmem_cache_free(vmap_area_cachep, va);
	return ERR_PTR(-EBUSY);
}

int register_vmap_purge_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&vmap_notify_list, nb);
}
EXPORT_SYMBOL_GPL(register_vmap_purge_notifier);

int unregister_vmap_purge_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&vmap_notify_list, nb);
}
EXPORT_SYMBOL_GPL(unregister_vmap_purge_notifier);

/*
 * lazy_max_pages is the maximum amount of virtual address space we gather up
 * before attempting to purge with a TLB flush.
 *
 * There is a tradeoff here: a larger number will cover more kernel page tables
 * and take slightly longer to purge, but it will linearly reduce the number of
 * global TLB flushes that must be performed. It would seem natural to scale
 * this number up linearly with the number of CPUs (because vmapping activity
 * could also scale linearly with the number of CPUs), however it is likely
 * that in practice, workloads might be constrained in other ways that mean
 * vmap activity will not scale linearly with CPUs. Also, I want to be
 * conservative and not introduce a big latency on huge systems, so go with
 * a less aggressive log scale. It will still be an improvement over the old
 * code, and it will be simple to change the scale factor if we find that it
 * becomes a problem on bigger systems.
 */
static unsigned long lazy_max_pages(void)
{
	/*
	 * lazy 阈值按在线 CPU 数的对数增长。更大的批次可减少全局 TLB shootdown，
	 * 但会更久占住不可复用 VA；对数而非线性扩张限制超大机器的回收延迟尖峰。
	 */
	unsigned int log;

	log = fls(num_online_cpus());

	return log * (32UL * 1024 * 1024 / PAGE_SIZE);
}

/*
 * Serialize vmap purging.  There is no actual critical section protected
 * by this lock, but we want to avoid concurrent calls for performance
 * reasons and to make the pcpu_get_vm_areas more deterministic.
 */
static DEFINE_MUTEX(vmap_purge_lock);

/* for per-CPU blocks */
static void purge_fragmented_blocks_allcpus(void);

static void
reclaim_list_global(struct list_head *head)
{
	struct vmap_area *va, *n;

	if (list_empty(head))
		return;

	spin_lock(&free_vmap_area_lock);
	list_for_each_entry_safe(va, n, head, list)
		merge_or_add_vmap_area_augment(va,
			&free_vmap_area_root, &free_vmap_area_list);
	spin_unlock(&free_vmap_area_lock);
}

static void
decay_va_pool_node(struct vmap_node *vn, bool full_decay)
{
	/*
	 * 普通回收从每个尺寸 pool 抽走约四分之一，保留热缓存；地址耗尽时 full_decay
	 * 全部抽走。整条链表先在锁下交换到本地，再在锁外合并，避免长时间持锁。
	 */
	LIST_HEAD(decay_list);
	struct rb_root decay_root = RB_ROOT;
	struct vmap_area *va, *nva;
	unsigned long n_decay, pool_len;
	int i;

	for (i = 0; i < MAX_VA_SIZE_PAGES; i++) {
		LIST_HEAD(tmp_list);

		if (list_empty(&vn->pool[i].head))
			continue;

		/* Detach the pool, so no-one can access it. */
		spin_lock(&vn->pool_lock);
		list_replace_init(&vn->pool[i].head, &tmp_list);
		spin_unlock(&vn->pool_lock);

		pool_len = n_decay = vn->pool[i].len;
		WRITE_ONCE(vn->pool[i].len, 0);

		/* Decay a pool by ~25% out of left objects. */
		if (!full_decay)
			n_decay >>= 2;
		pool_len -= n_decay;

		list_for_each_entry_safe(va, nva, &tmp_list, list) {
			if (!n_decay--)
				break;

			list_del_init(&va->list);
			merge_or_add_vmap_area(va, &decay_root, &decay_list);
		}

		/*
		 * Attach the pool back if it has been partly decayed.
		 * Please note, it is supposed that nobody(other contexts)
		 * can populate the pool therefore a simple list replace
		 * operation takes place here.
		 */
		if (!list_empty(&tmp_list)) {
			spin_lock(&vn->pool_lock);
			list_replace_init(&tmp_list, &vn->pool[i].head);
			WRITE_ONCE(vn->pool[i].len, pool_len);
			spin_unlock(&vn->pool_lock);
		}
	}

	reclaim_list_global(&decay_list);
}

#define KASAN_RELEASE_BATCH_SIZE 32

static void
kasan_release_vmalloc_node(struct vmap_node *vn)
{
	struct vmap_area *va;
	unsigned long start, end;
	unsigned int batch_count = 0;

	start = list_first_entry(&vn->purge_list, struct vmap_area, list)->va_start;
	end = list_last_entry(&vn->purge_list, struct vmap_area, list)->va_end;

	list_for_each_entry(va, &vn->purge_list, list) {
		if (is_vmalloc_or_module_addr((void *) va->va_start))
			kasan_release_vmalloc(va->va_start, va->va_end,
				va->va_start, va->va_end,
				KASAN_VMALLOC_PAGE_RANGE);

		if (need_resched() || (++batch_count >= KASAN_RELEASE_BATCH_SIZE)) {
			cond_resched();
			batch_count = 0;
		}
	}

	kasan_release_vmalloc(start, end, start, end, KASAN_VMALLOC_TLB_FLUSH);
}

static void purge_vmap_node(struct work_struct *work)
{
	struct vmap_node *vn = container_of(work,
		struct vmap_node, purge_work);
	unsigned long nr_purged_pages = 0;
	struct vmap_area *va, *n_va;
	LIST_HEAD(local_list);

	if (IS_ENABLED(CONFIG_KASAN_VMALLOC))
		kasan_release_vmalloc_node(vn);

	vn->nr_purged = 0;

	list_for_each_entry_safe(va, n_va, &vn->purge_list, list) {
		unsigned long nr = va_size(va) >> PAGE_SHIFT;
		unsigned int vn_id = decode_vn_id(va->flags);

		list_del_init(&va->list);

		nr_purged_pages += nr;
		vn->nr_purged++;

		if (is_vn_id_valid(vn_id) && !vn->skip_populate)
			if (node_pool_add_va(vn, va))
				continue;

		/* Go back to global. */
		list_add(&va->list, &local_list);
	}

	atomic_long_sub(nr_purged_pages, &vmap_lazy_nr);

	reclaim_list_global(&local_list);
}

/*
 * Purges all lazily-freed vmap areas.
 */
static bool __purge_vmap_area_lazy(unsigned long start, unsigned long end,
		bool full_pool_decay)
{
	/*
	 * 先把各 node 的 lazy tree 原子摘到私有 purge_list，计算覆盖范围并执行一次
	 * 全局 TLB flush，之后才把 VA 放回 pool/free tree。顺序不能交换：若先复用
	 * VA，其他 CPU 的旧 TLB 项可能访问新对象或已经释放的物理页。
	 */
	unsigned long nr_purged_areas = 0;
	unsigned int nr_purge_helpers;
	static cpumask_t purge_nodes;
	unsigned int nr_purge_nodes;
	struct vmap_node *vn;
	int i;

	lockdep_assert_held(&vmap_purge_lock);

	/*
	 * Use cpumask to mark which node has to be processed.
	 */
	purge_nodes = CPU_MASK_NONE;

	for_each_vmap_node(vn) {
		INIT_LIST_HEAD(&vn->purge_list);
		vn->skip_populate = full_pool_decay;
		decay_va_pool_node(vn, full_pool_decay);

		if (RB_EMPTY_ROOT(&vn->lazy.root))
			continue;

		spin_lock(&vn->lazy.lock);
		WRITE_ONCE(vn->lazy.root.rb_node, NULL);
		list_replace_init(&vn->lazy.head, &vn->purge_list);
		spin_unlock(&vn->lazy.lock);

		start = min(start, list_first_entry(&vn->purge_list,
			struct vmap_area, list)->va_start);

		end = max(end, list_last_entry(&vn->purge_list,
			struct vmap_area, list)->va_end);

		cpumask_set_cpu(node_to_id(vn), &purge_nodes);
	}

	nr_purge_nodes = cpumask_weight(&purge_nodes);
	if (nr_purge_nodes > 0) {
		flush_tlb_kernel_range(start, end);

		/* One extra worker is per a lazy_max_pages() full set minus one. */
		nr_purge_helpers = atomic_long_read(&vmap_lazy_nr) / lazy_max_pages();
		nr_purge_helpers = clamp(nr_purge_helpers, 1U, nr_purge_nodes) - 1;

		for_each_cpu(i, &purge_nodes) {
			vn = &vmap_nodes[i];

			if (nr_purge_helpers > 0) {
				INIT_WORK(&vn->purge_work, purge_vmap_node);

				if (cpumask_test_cpu(i, cpu_online_mask))
					schedule_work_on(i, &vn->purge_work);
				else
					schedule_work(&vn->purge_work);

				nr_purge_helpers--;
			} else {
				vn->purge_work.func = NULL;
				purge_vmap_node(&vn->purge_work);
				nr_purged_areas += vn->nr_purged;
			}
		}

		for_each_cpu(i, &purge_nodes) {
			vn = &vmap_nodes[i];

			if (vn->purge_work.func) {
				flush_work(&vn->purge_work);
				nr_purged_areas += vn->nr_purged;
			}
		}
	}

	trace_purge_vmap_area_lazy(start, end, nr_purged_areas);
	return nr_purged_areas > 0;
}

/*
 * Reclaim vmap areas by purging fragmented blocks and purge_vmap_area_list.
 */
static void reclaim_and_purge_vmap_areas(void)

{
	mutex_lock(&vmap_purge_lock);
	purge_fragmented_blocks_allcpus();
	__purge_vmap_area_lazy(ULONG_MAX, 0, true);
	mutex_unlock(&vmap_purge_lock);
}

static void drain_vmap_area_work(struct work_struct *work)
{
	mutex_lock(&vmap_purge_lock);
	__purge_vmap_area_lazy(ULONG_MAX, 0, false);
	mutex_unlock(&vmap_purge_lock);
}

/*
 * Free a vmap area, caller ensuring that the area has been unmapped,
 * unlinked and flush_cache_vunmap had been called for the correct
 * range previously.
 */
static void free_vmap_area_noflush(struct vmap_area *va)
{
	/*
	 * 页表已拆但 TLB 尚未 flush，VA 只能进入 lazy tree，不能立即回 free tree。
	 * 全局计数达到阈值时只调度后台 drain，释放热路径不直接承担跨 CPU shootdown。
	 * flags 中保存的来源 node 让 purge 后的小区间优先回原 size pool。
	 */
	unsigned long nr_lazy_max = lazy_max_pages();
	unsigned long va_start = va->va_start;
	unsigned int vn_id = decode_vn_id(va->flags);
	struct vmap_node *vn;
	unsigned long nr_lazy;

	if (WARN_ON_ONCE(!list_empty(&va->list)))
		return;

	nr_lazy = atomic_long_add_return_relaxed(va_size(va) >> PAGE_SHIFT,
					 &vmap_lazy_nr);

	/*
	 * If it was request by a certain node we would like to
	 * return it to that node, i.e. its pool for later reuse.
	 */
	vn = is_vn_id_valid(vn_id) ?
		id_to_node(vn_id):addr_to_node(va->va_start);

	spin_lock(&vn->lazy.lock);
	insert_vmap_area(va, &vn->lazy.root, &vn->lazy.head);
	spin_unlock(&vn->lazy.lock);

	trace_free_vmap_area_noflush(va_start, nr_lazy, nr_lazy_max);

	/* After this point, we may free va at any time */
	if (unlikely(nr_lazy > nr_lazy_max))
		schedule_work(&drain_vmap_work);
}

/*
 * Free and unmap a vmap area
 */
static void free_unmap_vmap_area(struct vmap_area *va)
{
	/*
	 * 正常释放的映射阶段依次为：处理旧 cache 别名、清页表、进入 lazy VA 回收。
	 * backing pages 此时仍存在，随后由 vfree 的所有权路径释放；vunmap 则不拥有页。
	 */
	flush_cache_vunmap(va->va_start, va->va_end);
	vunmap_range_noflush(va->va_start, va->va_end);
	if (debug_pagealloc_enabled_static())
		flush_tlb_kernel_range(va->va_start, va->va_end);

	free_vmap_area_noflush(va);
}

struct vmap_area *find_vmap_area(unsigned long addr)
{
	struct vmap_node *vn;
	struct vmap_area *va;
	int i, j;

	if (unlikely(!vmap_initialized))
		return NULL;

	/*
	 * An addr_to_node_id(addr) converts an address to a node index
	 * where a VA is located. If VA spans several zones and passed
	 * addr is not the same as va->va_start, what is not common, we
	 * may need to scan extra nodes. See an example:
	 *
	 *      <----va---->
	 * -|-----|-----|-----|-----|-
	 *     1     2     0     1
	 *
	 * VA resides in node 1 whereas it spans 1, 2 an 0. If passed
	 * addr is within 2 or 0 nodes we should do extra work.
	 */
	i = j = addr_to_node_id(addr);
	do {
		vn = &vmap_nodes[i];

		spin_lock(&vn->busy.lock);
		va = __find_vmap_area(addr, &vn->busy.root);
		spin_unlock(&vn->busy.lock);

		if (va)
			return va;
	} while ((i = (i + nr_vmap_nodes - 1) % nr_vmap_nodes) != j);

	return NULL;
}

static struct vmap_area *find_unlink_vmap_area(unsigned long addr)
{
	struct vmap_node *vn;
	struct vmap_area *va;
	int i, j;

	/*
	 * Check the comment in the find_vmap_area() about the loop.
	 */
	i = j = addr_to_node_id(addr);
	do {
		vn = &vmap_nodes[i];

		spin_lock(&vn->busy.lock);
		va = __find_vmap_area(addr, &vn->busy.root);
		if (va)
			unlink_va(va, &vn->busy.root);
		spin_unlock(&vn->busy.lock);

		if (va)
			return va;
	} while ((i = (i + nr_vmap_nodes - 1) % nr_vmap_nodes) != j);

	return NULL;
}

/*** Per cpu kva allocator ***/

/*
 * vm_map_ram 常映射少量页且生命周期很短。如果每次都进入全局增强树，描述符分配、
 * 树锁和邻居合并成本会压过实际映射工作。这里先从全局分配 VMAP_BLOCK_SIZE 大块，
 * 再用 bitmap 按 2^order 页切分，形成针对虚拟地址的小对象分配器。
 *
 * block 内有三个概念：used_map 表示调用者仍拥有的子区间；free 表示从未映射、
 * 可立即分配的尾部；dirty 表示页表已拆但旧 TLB alias 可能存在的空间。dirty
 * 在 flush 前绝不能重新计入 free。收益是热路径低延迟，代价是块内碎片和更复杂
 * 的延迟回收状态机，因此大请求仍直接使用全局 VA 分配器。
 */

/*
 * vmap space is limited especially on 32 bit architectures. Ensure there is
 * room for at least 16 percpu vmap blocks per CPU.
 */
/*
 * If we had a constant VMALLOC_START and VMALLOC_END, we'd like to be able
 * to #define VMALLOC_SPACE		(VMALLOC_END-VMALLOC_START). Guess
 * instead (we just need a rough idea)
 */
#if BITS_PER_LONG == 32
#define VMALLOC_SPACE		(128UL*1024*1024)
#else
#define VMALLOC_SPACE		(128UL*1024*1024*1024)
#endif

#define VMALLOC_PAGES		(VMALLOC_SPACE / PAGE_SIZE)
#define VMAP_MAX_ALLOC		BITS_PER_LONG	/* 256K with 4K pages */
#define VMAP_BBMAP_BITS_MAX	1024	/* 4MB with 4K pages */
#define VMAP_BBMAP_BITS_MIN	(VMAP_MAX_ALLOC*2)
#define VMAP_MIN(x, y)		((x) < (y) ? (x) : (y)) /* can't use min() */
#define VMAP_MAX(x, y)		((x) > (y) ? (x) : (y)) /* can't use max() */
#define VMAP_BBMAP_BITS		\
		VMAP_MIN(VMAP_BBMAP_BITS_MAX,	\
		VMAP_MAX(VMAP_BBMAP_BITS_MIN,	\
			VMALLOC_PAGES / roundup_pow_of_two(NR_CPUS) / 16))

#define VMAP_BLOCK_SIZE		(VMAP_BBMAP_BITS * PAGE_SIZE)

/*
 * Purge threshold to prevent overeager purging of fragmented blocks for
 * regular operations: Purge if vb->free is less than 1/4 of the capacity.
 */
#define VMAP_PURGE_THRESHOLD	(VMAP_BBMAP_BITS / 4)

#define VMAP_RAM		0x1 /* indicates vm_map_ram area*/
#define VMAP_BLOCK		0x2 /* mark out the vmap_block sub-type*/
#define VMAP_FLAGS_MASK		0x3

struct vmap_block_queue {
	spinlock_t lock;
	struct list_head free;

	/*
	 * An xarray requires an extra memory dynamically to
	 * be allocated. If it is an issue, we can use rb-tree
	 * instead.
	 */
	struct xarray vmap_blocks;
};

/*
 * queue 的 free 链表供当前 CPU 快速扫描仍有容量的 block；xarray 则按地址反查
 * block，供任意 CPU 执行 free。per-CPU 数组在反查路径里被当作 hash buckets，
 * bucket 编号不表示释放操作必须运行在对应 CPU。
 */

struct vmap_block {
	spinlock_t lock;
	struct vmap_area *va;
	unsigned long free, dirty;
	DECLARE_BITMAP(used_map, VMAP_BBMAP_BITS);
	unsigned long dirty_min, dirty_max; /*< dirty range */
	struct list_head free_list;
	struct rcu_head rcu_head;
	struct list_head purge;
	unsigned int cpu;
};

/*
 * vb->lock 保护 bitmap、free/dirty 计数和 dirty 边界；queue lock 只保护 free_list。
 * 对象从 xarray/free_list 摘除后仍可能被 RCU 读者持有，因此最终使用 kfree_rcu。
 */

/* Queue of free and dirty vmap blocks, for allocation and flushing purposes */
static DEFINE_PER_CPU(struct vmap_block_queue, vmap_block_queue);

/*
 * In order to fast access to any "vmap_block" associated with a
 * specific address, we use a hash.
 *
 * A per-cpu vmap_block_queue is used in both ways, to serialize
 * an access to free block chains among CPUs(alloc path) and it
 * also acts as a vmap_block hash(alloc/free paths). It means we
 * overload it, since we already have the per-cpu array which is
 * used as a hash table. When used as a hash a 'cpu' passed to
 * per_cpu() is not actually a CPU but rather a hash index.
 *
 * A hash function is addr_to_vb_xa() which hashes any address
 * to a specific index(in a hash) it belongs to. This then uses a
 * per_cpu() macro to access an array with generated index.
 *
 * An example:
 *
 *  CPU_1  CPU_2  CPU_0
 *    |      |      |
 *    V      V      V
 * 0     10     20     30     40     50     60
 * |------|------|------|------|------|------|...<vmap address space>
 *   CPU0   CPU1   CPU2   CPU0   CPU1   CPU2
 *
 * - CPU_1 invokes vm_unmap_ram(6), 6 belongs to CPU0 zone, thus
 *   it access: CPU0/INDEX0 -> vmap_blocks -> xa_lock;
 *
 * - CPU_2 invokes vm_unmap_ram(11), 11 belongs to CPU1 zone, thus
 *   it access: CPU1/INDEX1 -> vmap_blocks -> xa_lock;
 *
 * - CPU_0 invokes vm_unmap_ram(20), 20 belongs to CPU2 zone, thus
 *   it access: CPU2/INDEX2 -> vmap_blocks -> xa_lock.
 *
 * This technique almost always avoids lock contention on insert/remove,
 * however xarray spinlocks protect against any contention that remains.
 */
static struct xarray *
addr_to_vb_xa(unsigned long addr)
{
	/*
	 * 地址按 block 大小散列到 possible CPU 对应的 xarray。possible mask 可能有洞，
	 * 因而需要跳到下一个有效编号；插入、查询和删除必须使用完全相同的映射规则。
	 */
	int index = (addr / VMAP_BLOCK_SIZE) % nr_cpu_ids;

	/*
	 * Please note, nr_cpu_ids points on a highest set
	 * possible bit, i.e. we never invoke cpumask_next()
	 * if an index points on it which is nr_cpu_ids - 1.
	 */
	if (!cpu_possible(index))
		index = cpumask_next(index, cpu_possible_mask);

	return &per_cpu(vmap_block_queue, index).vmap_blocks;
}

/*
 * We should probably have a fallback mechanism to allocate virtual memory
 * out of partially filled vmap blocks. However vmap block sizing should be
 * fairly reasonable according to the vmalloc size, so it shouldn't be a
 * big problem.
 */

static unsigned long addr_to_vb_idx(unsigned long addr)
{
	addr -= VMALLOC_START & ~(VMAP_BLOCK_SIZE-1);
	addr /= VMAP_BLOCK_SIZE;
	return addr;
}

static void *vmap_block_vaddr(unsigned long va_start, unsigned long pages_off)
{
	unsigned long addr;

	addr = va_start + (pages_off << PAGE_SHIFT);
	BUG_ON(addr_to_vb_idx(addr) != addr_to_vb_idx(va_start));
	return (void *)addr;
}

/**
 * new_vmap_block - allocates new vmap_block and occupies 2^order pages in this
 *                  block. Of course pages number can't exceed VMAP_BBMAP_BITS
 * @order:    how many 2^order pages should be occupied in newly allocated block
 * @gfp_mask: flags for the page level allocator
 *
 * Return: virtual address in a newly allocated block or ERR_PTR(-errno)
 */
static void *new_vmap_block(unsigned int order, gfp_t gfp_mask)
{
	/*
	 * 创建顺序是：分配 block 元数据、从全局 VA 取得整块、初始化首个已用子区间、
	 * 发布到地址 xarray、最后发布到 RCU free_list。任一步失败只回滚已取得的前缀
	 * 资源；两个索引都发布后，分配和释放路径才都能发现该 block。
	 */
	struct vmap_block_queue *vbq;
	struct vmap_block *vb;
	struct vmap_area *va;
	struct xarray *xa;
	unsigned long vb_idx;
	int node, err;
	void *vaddr;

	node = numa_node_id();

	vb = kmalloc_node(sizeof(struct vmap_block), gfp_mask, node);
	if (unlikely(!vb))
		return ERR_PTR(-ENOMEM);

	va = alloc_vmap_area(VMAP_BLOCK_SIZE, VMAP_BLOCK_SIZE,
					VMALLOC_START, VMALLOC_END,
					node, gfp_mask,
					VMAP_RAM|VMAP_BLOCK, NULL);
	if (IS_ERR(va)) {
		kfree(vb);
		return ERR_CAST(va);
	}

	vaddr = vmap_block_vaddr(va->va_start, 0);
	spin_lock_init(&vb->lock);
	vb->va = va;
	/* At least something should be left free */
	BUG_ON(VMAP_BBMAP_BITS <= (1UL << order));
	bitmap_zero(vb->used_map, VMAP_BBMAP_BITS);
	vb->free = VMAP_BBMAP_BITS - (1UL << order);
	vb->dirty = 0;
	vb->dirty_min = VMAP_BBMAP_BITS;
	vb->dirty_max = 0;
	bitmap_set(vb->used_map, 0, (1UL << order));
	INIT_LIST_HEAD(&vb->free_list);
	vb->cpu = raw_smp_processor_id();

	xa = addr_to_vb_xa(va->va_start);
	vb_idx = addr_to_vb_idx(va->va_start);
	err = xa_insert(xa, vb_idx, vb, gfp_mask);
	if (err) {
		kfree(vb);
		free_vmap_area(va);
		return ERR_PTR(err);
	}
	/*
	 * list_add_tail_rcu could happened in another core
	 * rather than vb->cpu due to task migration, which
	 * is safe as list_add_tail_rcu will ensure the list's
	 * integrity together with list_for_each_rcu from read
	 * side.
	 */
	vbq = per_cpu_ptr(&vmap_block_queue, vb->cpu);
	spin_lock(&vbq->lock);
	list_add_tail_rcu(&vb->free_list, &vbq->free);
	spin_unlock(&vbq->lock);

	return vaddr;
}

static void free_vmap_block(struct vmap_block *vb)
{
	/*
	 * 先从 xarray 取消地址反查，再从全局 busy tree 摘除整块 VA，随后进入 lazy
	 * TLB 回收。元数据延迟到 RCU 宽限期后释放，避免并发分配扫描发生 UAF。
	 */
	struct vmap_node *vn;
	struct vmap_block *tmp;
	struct xarray *xa;

	xa = addr_to_vb_xa(vb->va->va_start);
	tmp = xa_erase(xa, addr_to_vb_idx(vb->va->va_start));
	BUG_ON(tmp != vb);

	vn = addr_to_node(vb->va->va_start);
	spin_lock(&vn->busy.lock);
	unlink_va(vb->va, &vn->busy.root);
	spin_unlock(&vn->busy.lock);

	free_vmap_area_noflush(vb->va);
	kfree_rcu(vb, rcu_head);
}

static bool purge_fragmented_block(struct vmap_block *vb,
		struct list_head *purge_list, bool force_purge)
{
	/*
	 * 只有已无 used 子区间的 block 才能整体 purge。普通路径还要求可立即使用的
	 * free 少于容量四分之一，避免销毁仍有价值的缓存；强制回收忽略该阈值。
	 * 先把 free 置零、dirty 置满，作为门闩阻止新的分配和重复 purge。
	 */
	struct vmap_block_queue *vbq = &per_cpu(vmap_block_queue, vb->cpu);

	if (vb->free + vb->dirty != VMAP_BBMAP_BITS ||
	    vb->dirty == VMAP_BBMAP_BITS)
		return false;

	/* Don't overeagerly purge usable blocks unless requested */
	if (!(force_purge || vb->free < VMAP_PURGE_THRESHOLD))
		return false;

	/* prevent further allocs after releasing lock */
	WRITE_ONCE(vb->free, 0);
	/* prevent purging it again */
	WRITE_ONCE(vb->dirty, VMAP_BBMAP_BITS);
	vb->dirty_min = 0;
	vb->dirty_max = VMAP_BBMAP_BITS;
	spin_lock(&vbq->lock);
	list_del_rcu(&vb->free_list);
	spin_unlock(&vbq->lock);
	list_add_tail(&vb->purge, purge_list);
	return true;
}

static void free_purged_blocks(struct list_head *purge_list)
{
	/*
	 * purge_list 是本轮扫描的私有工作队列：扫描阶段只在各 block 锁下摘除，
	 * 真正修改全局 VA 树和安排 RCU 释放放到锁外执行，缩短细粒度锁持有时间。
	 */
	struct vmap_block *vb, *n_vb;

	list_for_each_entry_safe(vb, n_vb, purge_list, purge) {
		list_del(&vb->purge);
		free_vmap_block(vb);
	}
}

static void purge_fragmented_blocks(int cpu)
{
	/*
	 * 对一个 queue 做强制碎片回收。RCU 保证遍历期间 block 不消失；无锁读取仅作
	 * 快速筛选，命中后必须在 vb->lock 下重新判断，不能把 READ_ONCE 当作提交条件。
	 */
	LIST_HEAD(purge);
	struct vmap_block *vb;
	struct vmap_block_queue *vbq = &per_cpu(vmap_block_queue, cpu);

	rcu_read_lock();
	list_for_each_entry_rcu(vb, &vbq->free, free_list) {
		unsigned long free = READ_ONCE(vb->free);
		unsigned long dirty = READ_ONCE(vb->dirty);

		if (free + dirty != VMAP_BBMAP_BITS ||
		    dirty == VMAP_BBMAP_BITS)
			continue;

		spin_lock(&vb->lock);
		purge_fragmented_block(vb, &purge, true);
		spin_unlock(&vb->lock);
	}
	rcu_read_unlock();
	/* free_vmap_block 会触碰全局索引，故不在 RCU 遍历和 vb 锁的嵌套区执行。 */
	free_purged_blocks(&purge);
}

static void purge_fragmented_blocks_allcpus(void)
{
	/* possible CPU 的 queue 即使当前离线也可能保存历史 block，不能只扫 online CPU。 */
	int cpu;

	for_each_possible_cpu(cpu)
		purge_fragmented_blocks(cpu);
}

static void *vb_alloc(unsigned long size, gfp_t gfp_mask)
{
	/*
	 * 分配只使用 block 尚未触碰的连续尾部，不搜索中间 dirty 洞。这样扫描和分配
	 * 都是常量级 bitmap 更新，但内部碎片要等 purge 才能回收。RCU 稳定链表存储，
	 * vb->lock 在命中后重新验证容量并提交 bitmap/计数变化。
	 */
	struct vmap_block_queue *vbq;
	struct vmap_block *vb;
	void *vaddr = NULL;
	unsigned int order;

	BUG_ON(offset_in_page(size));
	BUG_ON(size > PAGE_SIZE*VMAP_MAX_ALLOC);
	if (WARN_ON(size == 0)) {
		/*
		 * Allocating 0 bytes isn't what caller wants since
		 * get_order(0) returns funny result. Just warn and terminate
		 * early.
		 */
		return ERR_PTR(-EINVAL);
	}
	order = get_order(size);

	rcu_read_lock();
	vbq = raw_cpu_ptr(&vmap_block_queue);
	list_for_each_entry_rcu(vb, &vbq->free, free_list) {
		unsigned long pages_off;

		if (READ_ONCE(vb->free) < (1UL << order))
			continue;

		spin_lock(&vb->lock);
		if (vb->free < (1UL << order)) {
			spin_unlock(&vb->lock);
			continue;
		}

		pages_off = VMAP_BBMAP_BITS - vb->free;
		vaddr = vmap_block_vaddr(vb->va->va_start, pages_off);
		WRITE_ONCE(vb->free, vb->free - (1UL << order));
		bitmap_set(vb->used_map, pages_off, (1UL << order));
		if (vb->free == 0) {
			spin_lock(&vbq->lock);
			list_del_rcu(&vb->free_list);
			spin_unlock(&vbq->lock);
		}

		spin_unlock(&vb->lock);
		break;
	}

	rcu_read_unlock();

	/* Allocate new block if nothing was found */
	if (!vaddr)
		vaddr = new_vmap_block(order, gfp_mask);

	return vaddr;
}

static void vb_free(unsigned long addr, unsigned long size)
{
	/*
	 * 释放先清 used_map 并拆页表，再把范围加入 dirty，而不是 free。只有统一 TLB
	 * flush 后地址才可复用。debug_pagealloc 要求更强的立即失效保证，因此单独 flush。
	 */
	unsigned long offset;
	unsigned int order;
	struct vmap_block *vb;
	struct xarray *xa;

	BUG_ON(offset_in_page(size));
	BUG_ON(size > PAGE_SIZE*VMAP_MAX_ALLOC);

	flush_cache_vunmap(addr, addr + size);

	order = get_order(size);
	offset = (addr & (VMAP_BLOCK_SIZE - 1)) >> PAGE_SHIFT;

	xa = addr_to_vb_xa(addr);
	vb = xa_load(xa, addr_to_vb_idx(addr));

	spin_lock(&vb->lock);
	bitmap_clear(vb->used_map, offset, (1UL << order));
	spin_unlock(&vb->lock);

	vunmap_range_noflush(addr, addr + size);

	if (debug_pagealloc_enabled_static())
		flush_tlb_kernel_range(addr, addr + size);

	spin_lock(&vb->lock);

	/* Expand the not yet TLB flushed dirty range */
	vb->dirty_min = min(vb->dirty_min, offset);
	vb->dirty_max = max(vb->dirty_max, offset + (1UL << order));

	WRITE_ONCE(vb->dirty, vb->dirty + (1UL << order));
	if (vb->dirty == VMAP_BBMAP_BITS) {
		BUG_ON(vb->free);
		spin_unlock(&vb->lock);
		free_vmap_block(vb);
	} else
		spin_unlock(&vb->lock);
}

static void _vm_unmap_aliases(unsigned long start, unsigned long end, int flush)
{
	/*
	 * 这是 block dirty 状态与全局 lazy VA 回收的统一屏障。扫描把所有待失效区间
	 * 合并成一个 [start,end)，先完成可整体销毁 block 的摘除，再让一次 TLB flush
	 * 覆盖它们。vmap_purge_lock 串行化“收集 dirty 边界—清边界—执行 flush”，否则
	 * 两个回收者可能都认为对方会负责同一批旧翻译。
	 */
	LIST_HEAD(purge_list);
	int cpu;

	if (unlikely(!vmap_initialized))
		return;

	mutex_lock(&vmap_purge_lock);

	for_each_possible_cpu(cpu) {
		struct vmap_block_queue *vbq = &per_cpu(vmap_block_queue, cpu);
		struct vmap_block *vb;
		unsigned long idx;

		rcu_read_lock();
		/* xarray 按地址覆盖所有 block；这里不能只扫描 queue 的可分配链表。 */
		xa_for_each(&vbq->vmap_blocks, idx, vb) {
			spin_lock(&vb->lock);

			/*
			 * Try to purge a fragmented block first. If it's
			 * not purgeable, check whether there is dirty
			 * space to be flushed.
			 */
			if (!purge_fragmented_block(vb, &purge_list, false) &&
			    vb->dirty_max && vb->dirty != VMAP_BBMAP_BITS) {
				unsigned long va_start = vb->va->va_start;
				unsigned long s, e;

				s = va_start + (vb->dirty_min << PAGE_SHIFT);
				e = va_start + (vb->dirty_max << PAGE_SHIFT);

				/* 扩大公共 flush 包络；可能多刷空洞，以换取一次跨 CPU shootdown。 */
				start = min(s, start);
				end   = max(e, end);

				/* Prevent that this is flushed again */
				vb->dirty_min = VMAP_BBMAP_BITS;
				vb->dirty_max = 0;

				flush = 1;
			}
			spin_unlock(&vb->lock);
		}
		rcu_read_unlock();
	}
	free_purged_blocks(&purge_list);

	/*
	 * 全局 lazy purge 若已经执行了覆盖性 flush，就无需再刷 block 范围；否则只要
	 * 扫描发现 dirty block，便在此补上 flush。清 dirty 边界发生在它之前。
	 */
	if (!__purge_vmap_area_lazy(start, end, false) && flush)
		flush_tlb_kernel_range(start, end);
	mutex_unlock(&vmap_purge_lock);
}

/**
 * vm_unmap_aliases - unmap outstanding lazy aliases in the vmap layer
 *
 * The vmap/vmalloc layer lazily flushes kernel virtual mappings primarily
 * to amortize TLB flushing overheads. What this means is that any page you
 * have now, may, in a former life, have been mapped into kernel virtual
 * address by the vmap layer and so there might be some CPUs with TLB entries
 * still referencing that page (additional to the regular 1:1 kernel mapping).
 *
 * vm_unmap_aliases flushes all such lazy mappings. After it returns, we can
 * be sure that none of the pages we have control over will have any aliases
 * from the vmap layer.
 */
void vm_unmap_aliases(void)
{
	/* 反向的初始区间使扫描到的第一个 dirty 范围自然成为最终包络。 */
	_vm_unmap_aliases(ULONG_MAX, 0, 0);
}
EXPORT_SYMBOL_GPL(vm_unmap_aliases);

/**
 * vm_unmap_ram - unmap linear kernel address space set up by vm_map_ram
 * @mem: the pointer returned by vm_map_ram
 * @count: the count passed to that vm_map_ram call (cannot unmap partial)
 */
void vm_unmap_ram(const void *mem, unsigned int count)
{
	/*
	 * 调用者必须用与 vm_map_ram 完全相同的 base/count 释放，接口不保存独立长度。
	 * 小映射由 block 元数据定位，大映射由全局 busy tree 定位；两条路径最终都只
	 * 延迟回收虚拟地址，pages 的生命周期始终归调用者。
	 */
	unsigned long size = (unsigned long)count << PAGE_SHIFT;
	unsigned long addr = (unsigned long)kasan_reset_tag(mem);
	struct vmap_area *va;

	might_sleep();
	BUG_ON(!addr);
	BUG_ON(addr < VMALLOC_START);
	BUG_ON(addr > VMALLOC_END);
	BUG_ON(!PAGE_ALIGNED(addr));

	/* 先撤销检测器可访问性，避免页表拆除窗口内的 use-after-unmap 被漏报。 */
	kasan_poison_vmalloc(mem, size);

	if (likely(count <= VMAP_MAX_ALLOC)) {
		/* block 路径由地址反查所属 block，并把子区间转为 dirty。 */
		debug_check_no_locks_freed(mem, size);
		vb_free(addr, size);
		return;
	}

	/* 大映射在 busy tree 中是独立 vmap_area，先摘除以阻止再次查到。 */
	va = find_unlink_vmap_area(addr);
	if (WARN_ON_ONCE(!va))
		return;

	debug_check_no_locks_freed((void *)va->va_start, va_size(va));
	free_unmap_vmap_area(va);
}
EXPORT_SYMBOL(vm_unmap_ram);

/**
 * vm_map_ram - map pages linearly into kernel virtual address (vmalloc space)
 * @pages: an array of pointers to the pages to be mapped
 * @count: number of pages
 * @node: prefer to allocate data structures on this node
 *
 * If you use this function for less than VMAP_MAX_ALLOC pages, it could be
 * faster than vmap so it's good.  But if you mix long-life and short-life
 * objects with vm_map_ram(), it could consume lots of address space through
 * fragmentation (especially on a 32bit machine).  You could see failures in
 * the end.  Please use this function for short-lived objects.
 *
 * Returns: a pointer to the address that has been mapped, or %NULL on failure
 */
void *vm_map_ram(struct page **pages, unsigned int count, int node)
{
	/*
	 * 此 API 只建立 pages[] 到连续 KVA 的临时视图，不分配也不接管物理页。小请求
	 * 走 per-CPU block 缓存；大请求绕过 block，避免一个长寿命对象钉住整块空间。
	 * 映射失败必须调用对称 unmap 路径，因为此时 VA 已经发布到相应分配器。
	 */
	unsigned long size = (unsigned long)count << PAGE_SHIFT;
	unsigned long addr;
	void *mem;

	if (likely(count <= VMAP_MAX_ALLOC)) {
		/* 阈值以内优先减少全局树锁竞争，但代价是可能留下 block 内 dirty 碎片。 */
		mem = vb_alloc(size, GFP_KERNEL);
		if (IS_ERR(mem))
			return NULL;
		addr = (unsigned long)mem;
	} else {
		struct vmap_area *va;

		/* 大对象拥有独立描述符，node 只影响管理结构的 NUMA 放置，不改变 pages。 */
		va = alloc_vmap_area(size, PAGE_SIZE,
				VMALLOC_START, VMALLOC_END,
				node, GFP_KERNEL, VMAP_RAM,
				NULL);
		if (IS_ERR(va))
			return NULL;

		addr = va->va_start;
		mem = (void *)addr;
	}

	/* 到这里仅保留了 KVA；此调用才真正安装指向调用者 pages[] 的页表项。 */
	if (vmap_pages_range(addr, addr + size, PAGE_KERNEL,
				pages, PAGE_SHIFT) < 0) {
		vm_unmap_ram(mem, count);
		return NULL;
	}

	/*
	 * Mark the pages as accessible, now that they are mapped.
	 * With hardware tag-based KASAN, marking is skipped for
	 * non-VM_ALLOC mappings, see __kasan_unpoison_vmalloc().
	 */
	mem = kasan_unpoison_vmalloc(mem, size, KASAN_VMALLOC_PROT_NORMAL);

	return mem;
}
EXPORT_SYMBOL(vm_map_ram);

static struct vm_struct *vmlist __initdata;

/*
 * vm_struct 是面向调用者的映射描述，vmap_area 是底层地址空间索引节点。启动早期
 * 尚未建立并发树，只能把固定区域挂到临时有序 vmlist；vmalloc_init 后会迁移。
 */

static inline unsigned int vm_area_page_order(struct vm_struct *vm)
{
	/* page_order 描述 backing page/页表可采用的粒度；不支持 huge vmalloc 时恒为 0。 */
#ifdef CONFIG_HAVE_ARCH_HUGE_VMALLOC
	return vm->page_order;
#else
	return 0;
#endif
}

unsigned int get_vm_area_page_order(struct vm_struct *vm)
{
	return vm_area_page_order(vm);
}

static inline void set_vm_area_page_order(struct vm_struct *vm, unsigned int order)
{
#ifdef CONFIG_HAVE_ARCH_HUGE_VMALLOC
	vm->page_order = order;
#else
	BUG_ON(order != 0);
#endif
}

/**
 * vm_area_add_early - add vmap area early during boot
 * @vm: vm_struct to add
 *
 * This function is used to add fixed kernel vm area to vmlist before
 * vmalloc_init() is called.  @vm->addr, @vm->size, and @vm->flags
 * should contain proper values and the other fields should be zero.
 *
 * DO NOT USE THIS FUNCTION UNLESS YOU KNOW WHAT YOU'RE DOING.
 */
void __init vm_area_add_early(struct vm_struct *vm)
{
	/*
	 * 调用者已经选定地址，本函数只验证按地址排序且无重叠并插入。此阶段没有并发，
	 * BUG_ON 表示启动期静态布局错误，而不是可恢复的运行时资源不足。
	 */
	struct vm_struct *tmp, **p;

	BUG_ON(vmap_initialized);
	for (p = &vmlist; (tmp = *p) != NULL; p = &tmp->next) {
		if (tmp->addr >= vm->addr) {
			BUG_ON(tmp->addr < vm->addr + vm->size);
			break;
		} else
			BUG_ON(tmp->addr + tmp->size > vm->addr);
	}
	vm->next = *p;
	*p = vm;
}

/**
 * vm_area_register_early - register vmap area early during boot
 * @vm: vm_struct to register
 * @align: requested alignment
 *
 * This function is used to register kernel vm area before
 * vmalloc_init() is called.  @vm->size and @vm->flags should contain
 * proper values on entry and other fields should be zero.  On return,
 * vm->addr contains the allocated address.
 *
 * DO NOT USE THIS FUNCTION UNLESS YOU KNOW WHAT YOU'RE DOING.
 */
void __init vm_area_register_early(struct vm_struct *vm, size_t align)
{
	/*
	 * 从 VMALLOC_START 做 first-fit 扫描：跨过每个已登记区并重新对齐，找到首个洞。
	 * 与运行期分配器分离可避免初始化它自身所依赖的数据结构时出现循环依赖。
	 */
	unsigned long addr = ALIGN(VMALLOC_START, align);
	struct vm_struct *cur, **p;

	BUG_ON(vmap_initialized);

	for (p = &vmlist; (cur = *p) != NULL; p = &cur->next) {
		if ((unsigned long)cur->addr - addr >= vm->size)
			break;
		addr = ALIGN((unsigned long)cur->addr + cur->size, align);
	}

	BUG_ON(addr > VMALLOC_END - vm->size);
	/* 插入后立即建立 KASAN shadow；真实映射可稍后由早期调用者安装。 */
	vm->addr = (void *)addr;
	vm->next = *p;
	*p = vm;
	kasan_populate_early_vm_area_shadow(vm->addr, vm->size);
}

void clear_vm_uninitialized_flag(struct vm_struct *vm)
{
	/*
	 * Before removing VM_UNINITIALIZED,
	 * we should make sure that vm has proper values.
	 * Pair with smp_rmb() in vread_iter() and vmalloc_info_show().
	 */
	/* 发布协议：先令 addr/pages 等字段全局可见，再让无锁读者看到“已初始化”。 */
	smp_wmb();
	vm->flags &= ~VM_UNINITIALIZED;
}

struct vm_struct *__get_vm_area_node(unsigned long size,
		unsigned long align, unsigned long shift, unsigned long flags,
		unsigned long start, unsigned long end, int node,
		gfp_t gfp_mask, const void *caller)
{
	/*
	 * 本层只保留 KVA 并创建 vm_struct，不安装页表。requested_size 保留用户语义，
	 * size 则会因映射粒度向上取整并可能追加 guard page；后续释放必须使用后者。
	 * 返回的 area 已由 vmap_area->vm 关联，因而地址树查询能回到高层描述符。
	 */
	struct vmap_area *va;
	struct vm_struct *area;
	unsigned long requested_size = size;

	BUG_ON(in_nmi() || in_hardirq());
	/* 页表映射粒度决定可表示的最小范围；溢出到 0 也按失败处理。 */
	size = ALIGN(size, 1ul << shift);
	if (unlikely(!size))
		return NULL;

	if (flags & VM_IOREMAP)
		/* I/O 映射按规模提高对齐，为架构使用更大页表项保留机会，同时设上限。 */
		align = 1ul << clamp_t(int, get_count_order_long(size),
				       PAGE_SHIFT, IOREMAP_MAX_ORDER);

	/* 描述符分配不能继承与 slab 无关的 GFP 位，只保留 reclaim 语义。 */
	area = kzalloc_node(sizeof(*area), gfp_mask & GFP_RECLAIM_MASK, node);
	if (unlikely(!area))
		return NULL;

	if (!(flags & VM_NO_GUARD))
		/* 尾部 guard page 保留但不映射，使顺序越界尽早 fault；它计入 VA 消耗。 */
		size += PAGE_SIZE;

	area->flags = flags;
	area->caller = caller;
	area->requested_size = requested_size;

	/* 成功后底层 busy tree 已公开该区间，失败路径只需销毁尚未发布的 vm_struct。 */
	va = alloc_vmap_area(size, align, start, end, node, gfp_mask, 0, area);
	if (IS_ERR(va)) {
		kfree(area);
		return NULL;
	}

	/*
	 * Mark pages for non-VM_ALLOC mappings as accessible. Do it now as a
	 * best-effort approach, as they can be mapped outside of vmalloc code.
	 * For VM_ALLOC mappings, the pages are marked as accessible after
	 * getting mapped in __vmalloc_node_range().
	 * With hardware tag-based KASAN, marking is skipped for
	 * non-VM_ALLOC mappings, see __kasan_unpoison_vmalloc().
	 */
	if (!(flags & VM_ALLOC))
		area->addr = kasan_unpoison_vmalloc(area->addr, requested_size,
						    KASAN_VMALLOC_PROT_NORMAL);

	return area;
}

struct vm_struct *__get_vm_area_caller(unsigned long size, unsigned long flags,
				       unsigned long start, unsigned long end,
				       const void *caller)
{
	/* 保留调用者指定窗口中的 KVA，默认基本页粒度、普通内核可回收分配语义。 */
	return __get_vm_area_node(size, 1, PAGE_SHIFT, flags, start, end,
				  NUMA_NO_NODE, GFP_KERNEL, caller);
}

/**
 * get_vm_area - reserve a contiguous kernel virtual area
 * @size:	 size of the area
 * @flags:	 %VM_IOREMAP for I/O mappings or VM_ALLOC
 *
 * Search an area of @size in the kernel virtual mapping area,
 * and reserved it for out purposes.  Returns the area descriptor
 * on success or %NULL on failure.
 *
 * Return: the area descriptor on success or %NULL on failure.
 */
struct vm_struct *get_vm_area(unsigned long size, unsigned long flags)
{
	/* 公共包装器限定在标准 vmalloc 窗口，并记录直接调用点以供 vmallocinfo 诊断。 */
	return __get_vm_area_node(size, 1, PAGE_SHIFT, flags,
				  VMALLOC_START, VMALLOC_END,
				  NUMA_NO_NODE, GFP_KERNEL,
				  __builtin_return_address(0));
}

struct vm_struct *get_vm_area_caller(unsigned long size, unsigned long flags,
				const void *caller)
{
	/* 与 get_vm_area 相同，但允许中间封装层保留真正资源所有者的 caller。 */
	return __get_vm_area_node(size, 1, PAGE_SHIFT, flags,
				  VMALLOC_START, VMALLOC_END,
				  NUMA_NO_NODE, GFP_KERNEL, caller);
}

/**
 * find_vm_area - find a continuous kernel virtual area
 * @addr:	  base address
 *
 * Search for the kernel VM area starting at @addr, and return it.
 * It is up to the caller to do all required locking to keep the returned
 * pointer valid.
 *
 * Return: the area descriptor on success or %NULL on failure.
 */
struct vm_struct *find_vm_area(const void *addr)
{
	/* 只接受区域起始地址语义；返回值借用 busy tree 中对象，调用者需自行保证生命周期。 */
	struct vmap_area *va;

	va = find_vmap_area((unsigned long)addr);
	if (!va)
		return NULL;

	return va->vm;
}

/**
 * remove_vm_area - find and remove a continuous kernel virtual area
 * @addr:	    base address
 *
 * Search for the kernel VM area starting at @addr, and remove it.
 * This function returns the found VM area, but using it is NOT safe
 * on SMP machines, except for its size or flags.
 *
 * Return: the area descriptor on success or %NULL on failure.
 */
struct vm_struct *remove_vm_area(const void *addr)
{
	/*
	 * 释放的线性化点是从 busy tree 摘除 vmap_area：此后新查询不会获得该映射。
	 * 随后先撤销调试器/KASAN 元数据和页表，最后把 VA 放进 lazy 回收；返回的
	 * vm_struct 仍由上层决定是仅释放描述符，还是连同 backing pages 一并释放。
	 */
	struct vmap_area *va;
	struct vm_struct *vm;

	might_sleep();

	if (WARN(!PAGE_ALIGNED(addr), "Trying to vfree() bad address (%p)\n",
			addr))
		return NULL;

	va = find_unlink_vmap_area((unsigned long)addr);
	if (!va || !va->vm)
		return NULL;
	vm = va->vm;

	/* 在内存失效前检查是否遗留锁或已跟踪对象，错误报告才能指向原地址。 */
	debug_check_no_locks_freed(vm->addr, get_vm_area_size(vm));
	debug_check_no_obj_freed(vm->addr, get_vm_area_size(vm));
	kasan_free_module_shadow(vm);
	kasan_poison_vmalloc(vm->addr, get_vm_area_size(vm));

	free_unmap_vmap_area(va);
	return vm;
}

static inline void set_area_direct_map(const struct vm_struct *area,
				       int (*set_direct_map)(struct page *page))
{
	/* vmalloc 可用大页映射，但 direct map 权限 API 仍逐个 base page 处理。 */
	int i;

	/* HUGE_VMALLOC passes small pages to set_direct_map */
	for (i = 0; i < area->nr_pages; i++)
		if (page_address(area->pages[i]))
			set_direct_map(area->pages[i]);
}

/*
 * Flush the vm mapping and reset the direct map.
 */
static void vm_reset_perms(struct vm_struct *area)
{
	/*
	 * VM_FLUSH_RESET_PERMS 用于曾修改 backing page direct-map 权限的映射。必须先把
	 * direct map 设为 invalid，再统一清除 vmalloc alias 与 direct-map TLB，最后恢复
	 * 默认权限；否则 CPU 可能在恢复过程中重新缓存一个旧的宽松翻译。
	 */
	unsigned long start = ULONG_MAX, end = 0;
	unsigned int page_order = vm_area_page_order(area);
	int flush_dmap = 0;
	int i;

	/*
	 * Find the start and end range of the direct mappings to make sure that
	 * the vm_unmap_aliases() flush includes the direct map.
	 */
	/* pages[] 按 base page 记账，但相邻 2^order 项属于同一物理分配块。 */
	for (i = 0; i < area->nr_pages; i += 1U << page_order) {
		unsigned long addr = (unsigned long)page_address(area->pages[i]);

		if (addr) {
			unsigned long page_size;

			page_size = PAGE_SIZE << page_order;
			start = min(addr, start);
			end = max(addr + page_size, end);
			flush_dmap = 1;
		}
	}

	/*
	 * Set direct map to something invalid so that it won't be cached if
	 * there are any accesses after the TLB flush, then flush the TLB and
	 * reset the direct map permissions to the default.
	 */
	set_area_direct_map(area, set_direct_map_invalid_noflush);
	_vm_unmap_aliases(start, end, flush_dmap);
	set_area_direct_map(area, set_direct_map_default_noflush);
}

static void delayed_vfree_work(struct work_struct *w)
{
	/* 原子上下文只入无锁链表；worker 在可睡眠上下文复用完整 vfree 路径。 */
	struct vfree_deferred *p = container_of(w, struct vfree_deferred, wq);
	struct llist_node *t, *llnode;

	llist_for_each_safe(llnode, t, llist_del_all(&p->list))
		vfree(llnode);
}

/**
 * vfree_atomic - release memory allocated by vmalloc()
 * @addr:	  memory base address
 *
 * This one is just like vfree() but can be called in any atomic context
 * except NMIs.
 */
void vfree_atomic(const void *addr)
{
	/*
	 * 地址本身的首字被临时当作 llist_node，因此调用后对象内容立即无效。只有空链表
	 * 到非空的入队者调度 work，可将一批释放合并，且 lockless add 容忍任务迁移。
	 */
	struct vfree_deferred *p = raw_cpu_ptr(&vfree_deferred);

	BUG_ON(in_nmi());
	kmemleak_free(addr);

	/*
	 * Use raw_cpu_ptr() because this can be called from preemptible
	 * context. Preemption is absolutely fine here, because the llist_add()
	 * implementation is lockless, so it works even if we are adding to
	 * another cpu's list. schedule_work() should be fine with this too.
	 */
	if (addr && llist_add((struct llist_node *)addr, &p->list))
		schedule_work(&p->wq);
}

/*
 * vm_area_free_pages - free a range of pages from a vmalloc allocation
 * @vm: the vm_struct containing the pages
 * @start_idx: first page index to free (inclusive)
 * @end_idx: last page index to free (exclusive)
 *
 * Free pages [start_idx, end_idx) updating NR_VMALLOC stat accounting.
 * Freed vm->pages[] entries are set to NULL.
 * Caller is responsible for unmapping (vunmap_range) and KASAN
 * poisoning before calling this.
 */
static void vm_area_free_pages(struct vm_struct *vm, unsigned int start_idx,
			       unsigned int end_idx)
{
	/* VM_MAP_PUT_PAGES 表示页来自调用者并转移引用，相关记账由对应所有权路径处理。 */
	unsigned int i;

	if (!(vm->flags & VM_MAP_PUT_PAGES)) {
		for (i = start_idx; i < end_idx; i++)
			mod_lruvec_page_state(vm->pages[i], NR_VMALLOC, -1);
	}
	/* bulk helper 释放各 base-page 引用；随后清槽位，支持部分收缩后的幂等清理。 */
	free_pages_bulk(vm->pages + start_idx, end_idx - start_idx);

	for (i = start_idx; i < end_idx; i++)
		vm->pages[i] = NULL;
}

/**
 * vfree - Release memory allocated by vmalloc()
 * @addr:  Memory base address
 *
 * Free the virtually continuous memory area starting at @addr, as obtained
 * from one of the vmalloc() family of APIs.  This will usually also free the
 * physical memory underlying the virtual allocation, but that memory is
 * reference counted, so it will not be freed until the last user goes away.
 *
 * If @addr is NULL, no operation is performed.
 *
 * Context:
 * May sleep if called *not* from interrupt context.
 * Must not be called in NMI context (strictly speaking, it could be
 * if we have CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG, but making the calling
 * conventions for vfree() arch-dependent would be a really bad idea).
 */
void vfree(const void *addr)
{
	/*
	 * vfree 是“解除映射 + 释放 backing pages”的所有权 API。中断上下文不能执行树锁、
	 * TLB 回收和页释放中的可睡眠操作，故转交 worker；进程上下文则同步撤销映射，
	 * 必要时恢复 direct-map 权限，再按 pages[] 释放物理页与两层元数据。
	 */
	struct vm_struct *vm;

	if (unlikely(in_interrupt())) {
		vfree_atomic(addr);
		return;
	}

	BUG_ON(in_nmi());
	kmemleak_free(addr);
	might_sleep();

	if (!addr)
		return;

	/* remove_vm_area 已让地址不可查询并拆掉页表，但尚未释放 vm/pages。 */
	vm = remove_vm_area(addr);
	if (unlikely(!vm)) {
		WARN(1, KERN_ERR "Trying to vfree() nonexistent vm area (%p)\n",
				addr);
		return;
	}

	if (unlikely(vm->flags & VM_FLUSH_RESET_PERMS))
		vm_reset_perms(vm);

	vm_area_free_pages(vm, 0, vm->nr_pages);
	kvfree(vm->pages);
	kfree(vm);
}
EXPORT_SYMBOL(vfree);

/**
 * vunmap - release virtual mapping obtained by vmap()
 * @addr:   memory base address
 *
 * Free the virtually contiguous memory area starting at @addr,
 * which was created from the page array passed to vmap().
 *
 * Must not be called in interrupt context.
 */
void vunmap(const void *addr)
{
	/*
	 * vunmap 只销毁由 vmap 建立的视图，默认不拥有 pages[] 或物理页；因此与 vfree
	 * 共用 remove_vm_area 后只释放 vm_struct。误把 vmalloc 地址传来会泄漏 backing 页。
	 */
	struct vm_struct *vm;

	BUG_ON(in_interrupt());
	might_sleep();

	if (!addr)
		return;
	vm = remove_vm_area(addr);
	if (unlikely(!vm)) {
		WARN(1, KERN_ERR "Trying to vunmap() nonexistent vm area (%p)\n",
				addr);
		return;
	}
	kfree(vm);
}
EXPORT_SYMBOL(vunmap);

/**
 * vmap - map an array of pages into virtually contiguous space
 * @pages: array of page pointers
 * @count: number of pages to map
 * @flags: vm_area->flags
 * @prot: page protection for the mapping
 *
 * Maps @count pages from @pages into contiguous kernel virtual space.
 * If @flags contains %VM_MAP_PUT_PAGES the ownership of the pages array itself
 * (which must be kmalloc or vmalloc memory) and one reference per pages in it
 * are transferred from the caller to vmap(), and will be freed / dropped when
 * vfree() is called on the return value.
 *
 * Return: the address of the area or %NULL on failure
 */
void *vmap(struct page **pages, unsigned int count,
	   unsigned long flags, pgprot_t prot)
{
	/*
	 * vmap 将既有、可离散的 pages[] 投影成连续 KVA。默认所有权仍在调用者；仅当
	 * VM_MAP_PUT_PAGES 被设置，描述符才保存数组并约定未来用 vfree 归还页面引用。
	 * 映射强制 NX 变体，调用者不能借普通 vmap 随意制造可执行别名。
	 */
	struct vm_struct *area;
	unsigned long addr;
	unsigned long size;		/* In bytes */

	might_sleep();

	if (WARN_ON_ONCE(flags & VM_FLUSH_RESET_PERMS))
		return NULL;

	/*
	 * Your top guard is someone else's bottom guard. Not having a top
	 * guard compromises someone else's mappings too.
	 */
	if (WARN_ON_ONCE(flags & VM_NO_GUARD))
		flags &= ~VM_NO_GUARD;

	/* 同时避免 count 左移溢出，并拒绝显然不可能满足的物理页规模。 */
	if (count > totalram_pages())
		return NULL;

	size = (unsigned long)count << PAGE_SHIFT;
	/* 第一阶段仅保留带 guard page 的地址；第二阶段才安装传入 pages 的 PTE。 */
	area = get_vm_area_caller(size, flags, __builtin_return_address(0));
	if (!area)
		return NULL;

	addr = (unsigned long)area->addr;
	if (vmap_pages_range(addr, addr + size, pgprot_nx(prot),
				pages, PAGE_SHIFT) < 0) {
		vunmap(area->addr);
		return NULL;
	}

	if (flags & VM_MAP_PUT_PAGES) {
		/* 到映射成功后才提交所有权，失败时调用者仍可安全清理原 pages[]。 */
		area->pages = pages;
		area->nr_pages = count;
	}
	return area->addr;
}
EXPORT_SYMBOL(vmap);

#ifdef CONFIG_VMAP_PFN
struct vmap_pfn_data {
	unsigned long	*pfns;
	pgprot_t	prot;
	unsigned int	idx;
};

static int vmap_pfn_apply(pte_t *pte, unsigned long addr, void *private)
{
	/*
	 * PFN 路径面向没有 struct page 的设备/特殊物理地址；有效普通 RAM PFN 被拒绝，
	 * 防止绕开正常页引用和 cache 属性规则。special PTE 告诉 VM 不按普通页管理它。
	 */
	struct vmap_pfn_data *data = private;
	unsigned long pfn = data->pfns[data->idx];
	pte_t ptent;

	if (WARN_ON_ONCE(pfn_valid(pfn)))
		return -EINVAL;

	ptent = pte_mkspecial(pfn_pte(pfn, data->prot));
	set_pte_at(&init_mm, addr, pte, ptent);

	data->idx++;
	return 0;
}

/**
 * vmap_pfn - map an array of PFNs into virtually contiguous space
 * @pfns: array of PFNs
 * @count: number of pages to map
 * @prot: page protection for the mapping
 *
 * Maps @count PFNs from @pfns into contiguous kernel virtual space and returns
 * the start address of the mapping.
 */
void *vmap_pfn(unsigned long *pfns, unsigned int count, pgprot_t prot)
{
	/* 先保留 VM_IOREMAP 地址，再由通用页表 walker 逐 PTE 消费 PFN；失败整体回滚。 */
	struct vmap_pfn_data data = { .pfns = pfns, .prot = pgprot_nx(prot) };
	struct vm_struct *area;

	area = get_vm_area_caller(count * PAGE_SIZE, VM_IOREMAP,
			__builtin_return_address(0));
	if (!area)
		return NULL;
	if (apply_to_page_range(&init_mm, (unsigned long)area->addr,
			count * PAGE_SIZE, vmap_pfn_apply, &data)) {
		free_vm_area(area);
		return NULL;
	}

	flush_cache_vmap((unsigned long)area->addr,
			 (unsigned long)area->addr + count * PAGE_SIZE);

	return area->addr;
}
EXPORT_SYMBOL_GPL(vmap_pfn);
#endif /* CONFIG_VMAP_PFN */

/*
 * Helper for vmalloc to adjust the gfp flags for certain allocations.
 */
static inline gfp_t vmalloc_gfp_adjust(gfp_t flags, const bool large)
{
	/*
	 * vmalloc 本身有分级回退并会在最终失败处报告，底层高阶尝试无需刷屏；高阶
	 * __GFP_NOFAIL 可能触发长期压缩/OOM，故只允许最小粒度阶段承担 nofail 语义。
	 */
	flags |= __GFP_NOWARN;
	if (large)
		flags &= ~__GFP_NOFAIL;
	return flags;
}

static inline unsigned int
vm_area_alloc_pages(gfp_t gfp, int nid,
		unsigned int order, unsigned int nr_pages, struct page **pages)
{
	/*
	 * 物理页分配采用三级策略：先非阻塞地试较大 order 以利 huge mapping；order-0
	 * 使用批量分配降低锁开销；剩余部分用更宽松的单次分配补齐。pages[] 始终按
	 * base page 展开，令 vmalloc_to_page、统计及部分释放不依赖实际映射粒度。
	 */
	unsigned int nr_allocated = 0;
	unsigned int nr_remaining = nr_pages;
	unsigned int max_attempt_order = MAX_PAGE_ORDER;
	struct page *page;
	int i;
	unsigned int large_order = ilog2(nr_remaining);
	gfp_t large_gfp = vmalloc_gfp_adjust(gfp, large_order) & ~__GFP_DIRECT_RECLAIM;

	large_order = min(max_attempt_order, large_order);

	/*
	 * Initially, attempt to have the page allocator give us large order
	 * pages. Do not attempt allocating smaller than order chunks since
	 * __vmap_pages_range() expects physically contigous pages of exactly
	 * order long chunks.
	 */
	/* 大阶尝试不做 direct reclaim，失败就逐阶下降，避免优化性尝试造成抖动。 */
	while (large_order > order && nr_remaining) {
		if (nid == NUMA_NO_NODE)
			page = alloc_pages_noprof(large_gfp, large_order);
		else
			page = alloc_pages_node_noprof(nid, large_gfp, large_order);

		if (unlikely(!page)) {
			max_attempt_order = --large_order;
			continue;
		}

		mod_lruvec_page_state(page, NR_VMALLOC, 1 << large_order);

		/* 拆分复合分配的引用语义，但物理连续性仍在，可供后续大页页表映射。 */
		split_page(page, large_order);
		for (i = 0; i < (1U << large_order); i++)
			pages[nr_allocated + i] = page + i;

		nr_allocated += 1U << large_order;
		nr_remaining = nr_pages - nr_allocated;

		large_order = ilog2(nr_remaining);
		large_order = min(max_attempt_order, large_order);
	}

	/*
	 * For order-0 pages we make use of bulk allocator, if
	 * the page array is partly or not at all populated due
	 * to fails, fallback to a single page allocator that is
	 * more permissive.
	 */
	if (!order) {
		/* bulk 只优化 order-0；部分成功合法，余量留给后面的单页慢路径。 */
		while (nr_allocated < nr_pages) {
			unsigned int nr, nr_pages_request;
			int i;

			/*
			 * A maximum allowed request is hard-coded and is 100
			 * pages per call. That is done in order to prevent a
			 * long preemption off scenario in the bulk-allocator
			 * so the range is [1:100].
			 */
			nr_pages_request = min(100U, nr_pages - nr_allocated);

			/* memory allocation should consider mempolicy, we can't
			 * wrongly use nearest node when nid == NUMA_NO_NODE,
			 * otherwise memory may be allocated in only one node,
			 * but mempolicy wants to alloc memory by interleaving.
			 */
			if (IS_ENABLED(CONFIG_NUMA) && nid == NUMA_NO_NODE)
				nr = alloc_pages_bulk_mempolicy_noprof(gfp,
							nr_pages_request,
							pages + nr_allocated);
			else
				nr = alloc_pages_bulk_node_noprof(gfp, nid,
							nr_pages_request,
							pages + nr_allocated);

			for (i = nr_allocated; i < nr_allocated + nr; i++)
				mod_lruvec_page_state(pages[i], NR_VMALLOC, 1);

			nr_allocated += nr;

			/*
			 * If zero or pages were obtained partly,
			 * fallback to a single page allocator.
			 */
			if (nr != nr_pages_request)
				break;
		}
	}

	/* High-order pages or fallback path if "bulk" fails. */
	/* 这是高阶目标的正式分配路径，也是 bulk 未补齐时的兜底路径。 */
	while (nr_allocated < nr_pages) {
		if (!(gfp & __GFP_NOFAIL) && fatal_signal_pending(current))
			break;

		if (nid == NUMA_NO_NODE)
			page = alloc_pages_noprof(gfp, order);
		else
			page = alloc_pages_node_noprof(nid, gfp, order);

		if (unlikely(!page))
			break;

		mod_lruvec_page_state(page, NR_VMALLOC, 1 << order);

		/*
		 * High-order allocations must be able to be treated as
		 * independent small pages by callers (as they can with
		 * small-page vmallocs). Some drivers do their own refcounting
		 * on vmalloc_to_page() pages, some use page->mapping,
		 * page->lru, etc.
		 */
		if (order)
			split_page(page, order);

		/*
		 * Careful, we allocate and map page-order pages, but
		 * tracking is done per PAGE_SIZE page so as to keep the
		 * vm_struct APIs independent of the physical/mapped size.
		 */
		for (i = 0; i < (1U << order); i++)
			pages[nr_allocated + i] = page + i;

		nr_allocated += 1U << order;
	}

	return nr_allocated;
}

static LLIST_HEAD(pending_vm_area_cleanup);
static void cleanup_vm_area_work(struct work_struct *work)
{
	/*
	 * 错误清理可能来自受限 reclaim/原子语义的调用链，统一推迟到 worker。pages
	 * 尚未分配时仅撤销 VA；一旦存在 pages[]，vfree 可按 nr_pages 清理部分成果。
	 */
	struct vm_struct *area, *tmp;
	struct llist_node *head;

	head = llist_del_all(&pending_vm_area_cleanup);
	if (!head)
		return;

	llist_for_each_entry_safe(area, tmp, head, llnode) {
		if (!area->pages)
			free_vm_area(area);
		else
			vfree(area->addr);
	}
}

/*
 * Helper for __vmalloc_area_node() to defer cleanup
 * of partially initialized vm_struct in error paths.
 */
static DECLARE_WORK(cleanup_vm_area, cleanup_vm_area_work);
static void defer_vm_area_cleanup(struct vm_struct *area)
{
	/* 只由首个把全局链表从空变非空的提交者调度 worker，天然合并并发失败。 */
	if (llist_add(&area->llnode, &pending_vm_area_cleanup))
		schedule_work(&cleanup_vm_area);
}

/*
 * Page tables allocations ignore external GFP. Enforces it by
 * the memalloc scope API. It is used by vmalloc internals and
 * KASAN shadow population only.
 *
 * GFP to scope mapping:
 *
 * non-blocking (no __GFP_DIRECT_RECLAIM) - memalloc_noreclaim_save()
 * GFP_NOFS - memalloc_nofs_save()
 * GFP_NOIO - memalloc_noio_save()
 * __GFP_RETRY_MAYFAIL, __GFP_NORETRY - memalloc_noreclaim_save()
 * to prevent OOMs
 *
 * Returns a flag cookie to pair with restore.
 */
unsigned int
memalloc_apply_gfp_scope(gfp_t gfp_mask)
{
	/*
	 * 页表分配器不直接接收外层 GFP，此处把 NOFS/NOIO/不回收约束写入 current，
	 * 使嵌套分配遵守调用者的递归边界。cookie 必须在同一执行流中配对恢复。
	 */
	unsigned int flags = 0;

	if (!gfpflags_allow_blocking(gfp_mask) ||
			(gfp_mask & (__GFP_RETRY_MAYFAIL | __GFP_NORETRY)))
		flags = memalloc_noreclaim_save();
	else if ((gfp_mask & (__GFP_FS | __GFP_IO)) == __GFP_IO)
		flags = memalloc_nofs_save();
	else if ((gfp_mask & (__GFP_FS | __GFP_IO)) == 0)
		flags = memalloc_noio_save();

	/* 0 - no scope applied. */
	return flags;
}

void
memalloc_restore_scope(unsigned int flags)
{
	if (flags)
		memalloc_flags_restore(flags);
}

static void *__vmalloc_area_node(struct vm_struct *area, gfp_t gfp_mask,
				 pgprot_t prot, unsigned int page_shift,
				 int node)
{
	/*
	 * area 已占有 KVA；本函数完成 pages[] 元数据、物理页和页表三个后续阶段。
	 * 任一阶段失败都不能就地做可能递归/睡眠的复杂释放，而把部分初始化对象交给
	 * cleanup worker。成功前 VM_UNINITIALIZED 仍保留，无锁诊断读者会跳过它。
	 */
	const gfp_t nested_gfp = (gfp_mask & GFP_RECLAIM_MASK) | __GFP_ZERO;
	bool nofail = gfp_mask & __GFP_NOFAIL;
	unsigned long addr = (unsigned long)area->addr;
	unsigned long size = get_vm_area_size(area);
	unsigned long array_size;
	unsigned int nr_small_pages = size >> PAGE_SHIFT;
	unsigned int page_order;
	unsigned int flags;
	int ret;

	array_size = (unsigned long)nr_small_pages * sizeof(struct page *);

	/* __GFP_NOFAIL and "noblock" flags are mutually exclusive. */
	if (!gfpflags_allow_blocking(gfp_mask))
		nofail = false;

	if (!(gfp_mask & (GFP_DMA | GFP_DMA32)))
		gfp_mask |= __GFP_HIGHMEM;

	/* Please note that the recursion is strictly bounded. */
	/* 大 pages[] 本身用 vmalloc，形成递归；其规模每层快速缩小，因此递归有界。 */
	if (array_size > PAGE_SIZE) {
		area->pages = __vmalloc_node_noprof(array_size, 1, nested_gfp, node,
					area->caller);
	} else {
		area->pages = kmalloc_node_noprof(array_size, nested_gfp, node);
	}

	if (!area->pages) {
		warn_alloc(gfp_mask, NULL,
			"vmalloc error: size %lu, failed to allocated page array size %lu",
			nr_small_pages * PAGE_SIZE, array_size);
		goto fail;
	}

	set_vm_area_page_order(area, page_shift - PAGE_SHIFT);
	page_order = vm_area_page_order(area);

	/*
	 * High-order nofail allocations are really expensive and
	 * potentially dangerous (pre-mature OOM, disruptive reclaim
	 * and compaction etc.
	 *
	 * Please note, the __vmalloc_node_range_noprof() falls-back
	 * to order-0 pages if high-order attempt is unsuccessful.
	 */
	/* nr_pages 随成功进度更新，失败清理据此只释放实际取得的页面。 */
	area->nr_pages = vm_area_alloc_pages(
			vmalloc_gfp_adjust(gfp_mask, page_order), node,
			page_order, nr_small_pages, area->pages);

	/*
	 * If not enough pages were obtained to accomplish an
	 * allocation request, free them via vfree() if any.
	 */
	if (area->nr_pages != nr_small_pages) {
		/*
		 * vm_area_alloc_pages() can fail due to insufficient memory but
		 * also:-
		 *
		 * - a pending fatal signal
		 * - insufficient huge page-order pages
		 *
		 * Since we always retry allocations at order-0 in the huge page
		 * case a warning for either is spurious.
		 */
		if (!fatal_signal_pending(current) && page_order == 0)
			warn_alloc(gfp_mask, NULL,
				"vmalloc error: size %lu, failed to allocate pages",
				nr_small_pages * PAGE_SIZE);
		goto fail;
	}

	/*
	 * page tables allocations ignore external gfp mask, enforce it
	 * by the scope API
	 */
	/* 页表安装也会分配内存，先把外层 GFP 的 reclaim 限制传播到 current。 */
	flags = memalloc_apply_gfp_scope(gfp_mask);
	do {
		ret = __vmap_pages_range(addr, addr + size, prot, area->pages,
				page_shift, nested_gfp);
		if (nofail && (ret < 0))
			schedule_timeout_uninterruptible(1);
	} while (nofail && (ret < 0));
	memalloc_restore_scope(flags);

	if (ret < 0) {
		warn_alloc(gfp_mask, NULL,
			"vmalloc error: size %lu, failed to map pages",
			area->nr_pages * PAGE_SIZE);
		goto fail;
	}

	return area->addr;

fail:
	/* area 仍挂在 busy tree；异步清理会选择 free_vm_area 或完整 vfree。 */
	defer_vm_area_cleanup(area);
	return NULL;
}

/*
 * See __vmalloc_node_range() for a clear list of supported vmalloc flags.
 * This gfp lists all flags currently passed through vmalloc. Currently,
 * __GFP_ZERO is used by BPF and __GFP_NORETRY is used by percpu. Both drm
 * and BPF also use GFP_USER. Additionally, various users pass
 * GFP_KERNEL_ACCOUNT. Xfs uses __GFP_NOLOCKDEP.
 */
#define GFP_VMALLOC_SUPPORTED (GFP_KERNEL | GFP_ATOMIC | GFP_NOWAIT |\
				__GFP_NOFAIL | __GFP_ZERO |\
				__GFP_NORETRY | __GFP_RETRY_MAYFAIL |\
				GFP_NOFS | GFP_NOIO | GFP_KERNEL_ACCOUNT |\
				GFP_USER | __GFP_NOLOCKDEP | __GFP_SKIP_KASAN)

static gfp_t vmalloc_fix_flags(gfp_t flags)
{
	gfp_t invalid_mask = flags & ~GFP_VMALLOC_SUPPORTED;

	flags &= GFP_VMALLOC_SUPPORTED;
	WARN_ONCE(1, "Unexpected gfp: %#x (%pGg). Fixing up to gfp: %#x (%pGg). Fix your code!\n",
		  invalid_mask, &invalid_mask, flags, &flags);
	return flags;
}

/**
 * __vmalloc_node_range - allocate virtually contiguous memory
 * @size:		  allocation size
 * @align:		  desired alignment
 * @start:		  vm area range start
 * @end:		  vm area range end
 * @gfp_mask:		  flags for the page level allocator
 * @prot:		  protection mask for the allocated pages
 * @vm_flags:		  additional vm area flags (e.g. %VM_NO_GUARD)
 * @node:		  node to use for allocation or NUMA_NO_NODE
 * @caller:		  caller's return address
 *
 * Allocate enough pages to cover @size from the page level
 * allocator with @gfp_mask flags and map them into contiguous
 * virtual range with protection @prot.
 *
 * Supported GFP classes: %GFP_KERNEL, %GFP_ATOMIC, %GFP_NOWAIT,
 * %__GFP_RETRY_MAYFAIL, %__GFP_NORETRY, %GFP_NOFS and %GFP_NOIO.
 * Zone modifiers are not supported.
 * Please note %GFP_ATOMIC and %GFP_NOWAIT are supported only
 * by __vmalloc().
 *
 * Retry modifiers: only %__GFP_NOFAIL is fully supported;
 * %__GFP_NORETRY and %__GFP_RETRY_MAYFAIL are supported with limitation,
 * i.e. page tables are allocated with NOWAIT semantic so they might fail
 * under moderate memory pressure.
 *
 * %__GFP_NOWARN can be used to suppress failure messages.
 *
 * %__GFP_SKIP_KASAN can be used to skip unpoisoning of mapped pages
 * (when prot=%PAGE_KERNEL).
 *
 * Can not be called from interrupt nor NMI contexts.
 * Return: the address of the area or %NULL on failure
 */
void *__vmalloc_node_range_noprof(unsigned long size, unsigned long align,
			unsigned long start, unsigned long end, gfp_t gfp_mask,
			pgprot_t prot, unsigned long vm_flags, int node,
			const void *caller)
{
	/*
	 * vmalloc 的总控事务：先决定页表/物理页粒度，再保留 VA，随后分配 backing pages
	 * 并安装页表，最后才向 KASAN、无锁诊断读者和 kmemleak 发布完整对象。大页只是
	 * 优化路径，任何阶段失败都会把 shift 降回 PAGE_SHIFT 重做，保证功能不依赖大页。
	 */
	struct vm_struct *area;
	void *ret;
	kasan_vmalloc_flags_t kasan_flags = KASAN_VMALLOC_NONE;
	unsigned long original_align = align;
	unsigned int shift = PAGE_SHIFT;
	bool skip_vmalloc_kasan = kasan_hw_tags_enabled() && (gfp_mask & __GFP_SKIP_KASAN);

	if (WARN_ON_ONCE(!size))
		return NULL;

	/* 在任何乘法和元数据分配前拒绝超过系统总页数的明显不可能请求。 */
	if ((size >> PAGE_SHIFT) > totalram_pages()) {
		warn_alloc(gfp_mask, NULL,
			"vmalloc error: size %lu, exceeds total pages",
			size);
		return NULL;
	}

	if (vmap_allow_huge && (vm_flags & VM_ALLOW_HUGE_VMAP)) {
		/*
		 * Try huge pages. Only try for PAGE_KERNEL allocations,
		 * others like modules don't yet expect huge pages in
		 * their allocations due to apply_to_page_range not
		 * supporting them.
		 */

		/* 架构能力、保护属性和大小共同决定候选 shift；并同步提高 VA 对齐。 */
		if (arch_vmap_pmd_supported(prot) && size >= PMD_SIZE)
			shift = PMD_SHIFT;
		else
			shift = arch_vmap_pte_supported_shift(size);

		align = max(original_align, 1UL << shift);
	}

again:
	/* VM_UNINITIALIZED 是发布屏障的一部分：busy tree 可先看到 area，但不能读取半成品。 */
	area = __get_vm_area_node(size, align, shift, VM_ALLOC |
				  VM_UNINITIALIZED | vm_flags, start, end, node,
				  gfp_mask & ~__GFP_SKIP_KASAN, caller);
	if (!area) {
		bool nofail = gfp_mask & __GFP_NOFAIL;
		warn_alloc(gfp_mask, NULL,
			"vmalloc error: size %lu, vm_struct allocation failed%s",
			size, (nofail) ? ". Retrying." : "");
		if (nofail) {
			/* nofail 通过让出 CPU 后重试实现，不在地址树锁内忙等。 */
			schedule_timeout_uninterruptible(1);
			goto again;
		}
		goto fail;
	}

	/*
	 * Prepare arguments for __vmalloc_area_node() and
	 * kasan_unpoison_vmalloc().
	 */
	if (pgprot_val(prot) == pgprot_val(PAGE_KERNEL)) {
		if (kasan_hw_tags_enabled() && !skip_vmalloc_kasan) {
			/*
			 * Modify protection bits to allow tagging.
			 * This must be done before mapping.
			 */
			/* 页表属性必须先支持 tag；物理页的 poison/zero 延后到统一 KASAN 步骤。 */
			prot = arch_vmap_pgprot_tagged(prot);

			/*
			 * Skip page_alloc poisoning and zeroing for physical
			 * pages backing VM_ALLOC mapping. Memory is instead
			 * poisoned and zeroed by kasan_unpoison_vmalloc().
			 */
			gfp_mask |= __GFP_SKIP_KASAN | __GFP_SKIP_ZERO;
		}

		/* Take note that the mapping is PAGE_KERNEL. */
		kasan_flags |= KASAN_VMALLOC_PROT_NORMAL;
	}

	/* Allocate physical pages and map them into vmalloc space. */
	/* 此调用提交物理资源与页表，但 area 对观察者仍标记为未初始化。 */
	ret = __vmalloc_area_node(area, gfp_mask, prot, shift, node);
	if (!ret)
		goto fail;

	/*
	 * Mark the pages as accessible, now that they are mapped.
	 * The condition for setting KASAN_VMALLOC_INIT should complement the
	 * one in post_alloc_hook() with regards to the __GFP_SKIP_ZERO check
	 * to make sure that memory is initialized under the same conditions.
	 * Tag-based KASAN modes only assign tags to normal non-executable
	 * allocations, see __kasan_unpoison_vmalloc().
	 */
	kasan_flags |= KASAN_VMALLOC_VM_ALLOC;
	if (!want_init_on_free() && want_init_on_alloc(gfp_mask) &&
	    (gfp_mask & __GFP_SKIP_ZERO))
		kasan_flags |= KASAN_VMALLOC_INIT;
	/* KASAN_VMALLOC_PROT_NORMAL already set if required. */
	/* 返回带 tag 的地址可能不同于 area 原始指针，必须保存它供后续对称释放。 */
	if (!skip_vmalloc_kasan)
		area->addr = kasan_unpoison_vmalloc(area->addr, size, kasan_flags);

	/*
	 * In this function, newly allocated vm_struct has VM_UNINITIALIZED
	 * flag. It means that vm_struct is not fully initialized.
	 * Now, it is fully initialized, so remove this flag here.
	 */
	/* release 式发布全部字段；vread/proc 侧以读屏障配对。 */
	clear_vm_uninitialized_flag(area);

	if (!(vm_flags & VM_DEFER_KMEMLEAK))
		kmemleak_vmalloc(area, PAGE_ALIGN(size), gfp_mask);

	return area->addr;

fail:
	if (shift > PAGE_SHIFT) {
		/* 高阶失败不改变 API 语义：恢复调用者对齐并从保留 VA 阶段完整重试。 */
		shift = PAGE_SHIFT;
		align = original_align;
		goto again;
	}

	return NULL;
}

/**
 * __vmalloc_node - allocate virtually contiguous memory
 * @size:	    allocation size
 * @align:	    desired alignment
 * @gfp_mask:	    flags for the page level allocator
 * @node:	    node to use for allocation or NUMA_NO_NODE
 * @caller:	    caller's return address
 *
 * Allocate enough pages to cover @size from the page level allocator with
 * @gfp_mask flags.  Map them into contiguous kernel virtual space.
 *
 * Semantics of @gfp_mask (including reclaim/retry modifiers such as
 * __GFP_NOFAIL) are the same as in __vmalloc_node_range_noprof().
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *__vmalloc_node_noprof(unsigned long size, unsigned long align,
			    gfp_t gfp_mask, int node, const void *caller)
{
	/* 把常用全 vmalloc 窗口/PAGE_KERNEL 策略收敛到 range 总控函数。 */
	return __vmalloc_node_range_noprof(size, align, VMALLOC_START, VMALLOC_END,
				gfp_mask, PAGE_KERNEL, 0, node, caller);
}
/*
 * This is only for performance analysis of vmalloc and stress purpose.
 * It is required by vmalloc test module, therefore do not use it other
 * than that.
 */
#ifdef CONFIG_TEST_VMALLOC_MODULE
EXPORT_SYMBOL_GPL(__vmalloc_node_noprof);
#endif

void *__vmalloc_noprof(unsigned long size, gfp_t gfp_mask)
{
	/* 通用可控 GFP 入口会过滤 vmalloc 无法兑现的标志，避免静默产生错误语义。 */
	if (unlikely(gfp_mask & ~GFP_VMALLOC_SUPPORTED))
		gfp_mask = vmalloc_fix_flags(gfp_mask);
	return __vmalloc_node_noprof(size, 1, gfp_mask, NUMA_NO_NODE,
				__builtin_return_address(0));
}
EXPORT_SYMBOL(__vmalloc_noprof);

/**
 * vmalloc - allocate virtually contiguous memory
 * @size:    allocation size
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_noprof(unsigned long size)
{
	/* 默认接口选择 GFP_KERNEL、任意 NUMA 节点和最小对齐，适合绝大多数内核对象。 */
	return __vmalloc_node_noprof(size, 1, GFP_KERNEL, NUMA_NO_NODE,
				__builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_noprof);

/**
 * vmalloc_huge_node - allocate virtually contiguous memory, allow huge pages
 * @size:      allocation size
 * @gfp_mask:  flags for the page level allocator
 * @node:	    node to use for allocation or NUMA_NO_NODE
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 * If @size is greater than or equal to PMD_SIZE, allow using
 * huge pages for the memory
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_huge_node_noprof(unsigned long size, gfp_t gfp_mask, int node)
{
	/* VM_ALLOW_HUGE_VMAP 仅授权尝试；不连续或架构不支持时总控路径自动退回基本页。 */
	if (unlikely(gfp_mask & ~GFP_VMALLOC_SUPPORTED))
		gfp_mask = vmalloc_fix_flags(gfp_mask);
	return __vmalloc_node_range_noprof(size, 1, VMALLOC_START, VMALLOC_END,
					   gfp_mask, PAGE_KERNEL, VM_ALLOW_HUGE_VMAP,
					   node, __builtin_return_address(0));
}
EXPORT_SYMBOL_GPL(vmalloc_huge_node_noprof);

/**
 * vzalloc - allocate virtually contiguous memory with zero fill
 * @size:    allocation size
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 * The memory allocated is set to zero.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vzalloc_noprof(unsigned long size)
{
	/* 零填充通过 GFP 语义贯穿物理页与 KASAN 初始化路径，不在映射后另做一次 memset。 */
	return __vmalloc_node_noprof(size, 1, GFP_KERNEL | __GFP_ZERO, NUMA_NO_NODE,
				__builtin_return_address(0));
}
EXPORT_SYMBOL(vzalloc_noprof);

/**
 * vmalloc_user - allocate zeroed virtually contiguous memory for userspace
 * @size: allocation size
 *
 * The resulting memory area is zeroed so it can be mapped to userspace
 * without leaking data.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_user_noprof(unsigned long size)
{
	/* 用户可映射内存必须清零防信息泄漏，并按 SHMLBA 对齐满足架构共享映射约束。 */
	return __vmalloc_node_range_noprof(size, SHMLBA,  VMALLOC_START, VMALLOC_END,
				    GFP_KERNEL | __GFP_ZERO, PAGE_KERNEL,
				    VM_USERMAP, NUMA_NO_NODE,
				    __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_user_noprof);

/**
 * vmalloc_node - allocate memory on a specific node
 * @size:	  allocation size
 * @node:	  numa node
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc() instead.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_node_noprof(unsigned long size, int node)
{
	/* node 是优选放置而非绝对约束；需要强制节点时应在底层 GFP 中加入 THISNODE。 */
	return __vmalloc_node_noprof(size, 1, GFP_KERNEL, node,
			__builtin_return_address(0));
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
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vzalloc_node_noprof(unsigned long size, int node)
{
	/* NUMA 优选与零初始化的组合包装器，失败/回退语义仍由统一总控路径决定。 */
	return __vmalloc_node_noprof(size, 1, GFP_KERNEL | __GFP_ZERO, node,
				__builtin_return_address(0));
}
EXPORT_SYMBOL(vzalloc_node_noprof);

/**
 * vrealloc_node_align - reallocate virtually contiguous memory; contents
 * remain unchanged
 * @p: object to reallocate memory for
 * @size: the size to reallocate
 * @align: requested alignment
 * @flags: the flags for the page level allocator
 * @nid: node number of the target node
 *
 * If @p is %NULL, vrealloc_XXX() behaves exactly like vmalloc_XXX(). If @size
 * is 0 and @p is not a %NULL pointer, the object pointed to is freed.
 *
 * If the caller wants the new memory to be on specific node *only*,
 * __GFP_THISNODE flag should be set, otherwise the function will try to avoid
 * reallocation and possibly disregard the specified @nid.
 *
 * If __GFP_ZERO logic is requested, callers must ensure that, starting with the
 * initial memory allocation, every subsequent call to this API for the same
 * memory allocation is flagged with __GFP_ZERO. Otherwise, it is possible that
 * __GFP_ZERO is not fully honored by this API.
 *
 * Requesting an alignment that is bigger than the alignment of the existing
 * allocation will fail.
 *
 * In any case, the contents of the object pointed to are preserved up to the
 * lesser of the new and old sizes.
 *
 * This function must not be called concurrently with itself or vfree() for the
 * same memory allocation.
 *
 * Return: pointer to the allocated memory; %NULL if @size is zero or in case of
 *         failure
 */
void *vrealloc_node_align_noprof(const void *p, size_t size, unsigned long align,
				 gfp_t flags, int nid)
{
	/*
	 * vrealloc 优先保持地址稳定：逻辑收缩只更新 requested_size，跨页收缩在安全条件
	 * 下拆尾部映射并归还页，重新增长若仍落在已保留/已映射容量内也原地完成；只有
	 * 容量、NUMA 强约束不满足时才分配复制。失败保持旧对象有效，符合 realloc 契约。
	 */
	struct vm_struct *vm = NULL;
	size_t alloced_size = 0;
	size_t old_size = 0;
	void *n;

	if (!size) {
		vfree(p);
		return NULL;
	}

	if (p) {
		/* find_vm_area 返回借用对象；接口明确禁止与同一地址的 vfree/vrealloc 并发。 */
		vm = find_vm_area(p);
		if (unlikely(!vm)) {
			WARN(1, "Trying to vrealloc() nonexistent vm area (%p)\n", p);
			return NULL;
		}

		/* alloced_size 含当前可用映射容量，requested_size 是调用者可见的逻辑长度。 */
		alloced_size = get_vm_area_size(vm);
		old_size = vm->requested_size;
		if (WARN(alloced_size < old_size,
			 "vrealloc() has mismatched area vs requested sizes (%p)\n", p))
			return NULL;
		if (WARN(!IS_ALIGNED((unsigned long)p, align),
			 "will not reallocate with a bigger alignment (0x%lx)\n", align))
			return NULL;
		if (unlikely(flags & __GFP_THISNODE) && nid != NUMA_NO_NODE &&
			     nid != page_to_nid(vmalloc_to_page(p)))
			goto need_realloc;
	} else {
		/*
		 * If p is NULL, vrealloc behaves exactly like vmalloc.
		 * Skip the shrink and in-place grow paths.
		 */
		goto need_realloc;
	}

	if (size <= old_size) {
		unsigned int new_nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;

		/* Zero out "freed" memory, potentially for future realloc. */
		/* 即使暂不归还整页，也清除缩短尾部，防止未来原地扩展暴露旧内容。 */
		if (want_init_on_free() || want_init_on_alloc(flags))
			memset((void *)p + size, 0, old_size - size);

		/*
		 * Free tail pages when shrink crosses a page boundary.
		 *
		 * Skip huge page allocations (page_order > 0) as partial
		 * freeing would require splitting.
		 *
		 * Skip VM_FLUSH_RESET_PERMS, as direct-map permissions must
		 * be reset before pages are returned to the allocator.
		 *
		 * Skip VM_USERMAP, as remap_vmalloc_range_partial() validates
		 * mapping requests against the unchanged vm->size; freeing
		 * tail pages would cause vmalloc_to_page() to return NULL for
		 * the unmapped range.
		 *
		 * Skip if either GFP_NOFS or GFP_NOIO are used.
		 * kmemleak_free_part() internally allocates with
		 * GFP_KERNEL, which could trigger a recursive deadlock
		 * if we are under filesystem or I/O reclaim.
		 */
		if (new_nr_pages < vm->nr_pages && !vm_area_page_order(vm) &&
		    !(vm->flags & (VM_FLUSH_RESET_PERMS | VM_USERMAP)) &&
		    gfp_has_io_fs(flags)) {
			unsigned long addr = (unsigned long)kasan_reset_tag(p);
			unsigned int old_nr_pages = vm->nr_pages;

			/*
			 * Use the node lock to synchronize with concurrent
			 * readers (vmalloc_info_show).
			 */
			struct vmap_node *vn = addr_to_node(addr);

			/* 先缩短公开 nr_pages，诊断读者便不会进入即将解除映射的尾部。 */
			spin_lock(&vn->busy.lock);
			vm->nr_pages = new_nr_pages;
			spin_unlock(&vn->busy.lock);

			/* Notify kmemleak of the reduced allocation size before unmapping. */
			kmemleak_free_part(
				(void *)addr + ((unsigned long)new_nr_pages
						<< PAGE_SHIFT),
				(unsigned long)(old_nr_pages - new_nr_pages)
					<< PAGE_SHIFT);

			/* kmemleak、页表、物理页按观察层次依次收缩，避免任何层引用已释放页。 */
			vunmap_range(addr + ((unsigned long)new_nr_pages
					     << PAGE_SHIFT),
				     addr + ((unsigned long)old_nr_pages
					     << PAGE_SHIFT));

			vm_area_free_pages(vm, new_nr_pages, old_nr_pages);
		}
		vm->requested_size = size;
		kasan_vrealloc(p, old_size, size);
		return (void *)p;
	}

	/*
	 * We already have the bytes available in the allocation; use them.
	 */
	if (size <= vm->nr_pages << PAGE_SHIFT) {
		/* 先前按页向上取整或收缩保留的容量足够，只恢复逻辑可见长度。 */
		/*
		 * No need to zero memory here, as unused memory will have
		 * already been zeroed at initial allocation time or during
		 * realloc shrink time.
		 */
		vm->requested_size = size;
		kasan_vrealloc(p, old_size, size);
		return (void *)p;
	}

need_realloc:
	/* TODO: Grow the vm_area, i.e. allocate and map additional pages. */
	/* 当前实现不能扩展相邻 VA，故分配新对象；成功后复制再释放，确保失败原子性。 */
	n = __vmalloc_node_noprof(size, align, flags, nid, __builtin_return_address(0));

	if (!n)
		return NULL;

	if (p) {
		memcpy(n, p, min(size, old_size));
		vfree(p);
	}

	return n;
}
EXPORT_SYMBOL(vrealloc_node_align_noprof);

#if defined(CONFIG_64BIT) && defined(CONFIG_ZONE_DMA32)
#define GFP_VMALLOC32 (GFP_DMA32 | GFP_KERNEL)
#elif defined(CONFIG_64BIT) && defined(CONFIG_ZONE_DMA)
#define GFP_VMALLOC32 (GFP_DMA | GFP_KERNEL)
#else
/*
 * 64b systems should always have either DMA or DMA32 zones. For others
 * GFP_DMA32 should do the right thing and use the normal zone.
 */
#define GFP_VMALLOC32 (GFP_DMA32 | GFP_KERNEL)
#endif

/**
 * vmalloc_32 - allocate virtually contiguous memory (32bit addressable)
 * @size:	allocation size
 *
 * Allocate enough 32bit PA addressable pages to cover @size from the
 * page level allocator and map them into contiguous kernel virtual space.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_32_noprof(unsigned long size)
{
	/* “32”约束 backing page 的物理可寻址范围，返回的 KVA 本身仍位于 vmalloc 窗口。 */
	return __vmalloc_node_noprof(size, 1, GFP_VMALLOC32, NUMA_NO_NODE,
			__builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_32_noprof);

/**
 * vmalloc_32_user - allocate zeroed virtually contiguous 32bit memory
 * @size:	     allocation size
 *
 * The resulting memory area is 32bit addressable and zeroed so it can be
 * mapped to userspace without leaking data.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
void *vmalloc_32_user_noprof(unsigned long size)
{
	/* 面向用户映射的 DMA 可寻址内存同时要求 SHMLBA 对齐和清零，避免数据泄漏。 */
	return __vmalloc_node_range_noprof(size, SHMLBA,  VMALLOC_START, VMALLOC_END,
				    GFP_VMALLOC32 | __GFP_ZERO, PAGE_KERNEL,
				    VM_USERMAP, NUMA_NO_NODE,
				    __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_32_user_noprof);

/*
 * Atomically zero bytes in the iterator.
 *
 * Returns the number of zeroed bytes.
 */
static size_t zero_iter(struct iov_iter *iter, size_t count)
{
	/* hole 不直接 memset 用户目标，而通过 nofault iterator API，避免调试读取触发 fault。 */
	size_t remains = count;

	while (remains > 0) {
		size_t num, copied;

		num = min_t(size_t, remains, PAGE_SIZE);
		copied = copy_page_to_iter_nofault(ZERO_PAGE(0), 0, num, iter);
		remains -= copied;

		if (copied < num)
			break;
	}

	return count - remains;
}

/*
 * small helper routine, copy contents to iter from addr.
 * If the page is not present, fill zero.
 *
 * Returns the number of copied bytes.
 */
static size_t aligned_vread_iter(struct iov_iter *iter,
				 const char *addr, size_t count)
{
	/*
	 * 每页重新做 vmalloc_to_page，并以 nofault 局部映射复制。它牺牲调试读取性能，
	 * 换取不长期持有 vmalloc 全局锁；并发解除映射时缺页按洞返回零而不是解引用 KVA。
	 */
	size_t remains = count;
	struct page *page;

	while (remains > 0) {
		unsigned long offset, length;
		size_t copied = 0;

		offset = offset_in_page(addr);
		length = PAGE_SIZE - offset;
		if (length > remains)
			length = remains;
		page = vmalloc_to_page(addr);
		/*
		 * To do safe access to this _mapped_ area, we need lock. But
		 * adding lock here means that we need to add overhead of
		 * vmalloc()/vfree() calls for this _debug_ interface, rarely
		 * used. Instead of that, we'll use an local mapping via
		 * copy_page_to_iter_nofault() and accept a small overhead in
		 * this access function.
		 */
		if (page)
			copied = copy_page_to_iter_nofault(page, offset,
							   length, iter);
		else
			copied = zero_iter(iter, length);

		addr += copied;
		remains -= copied;

		if (copied != length)
			break;
	}

	return count - remains;
}

/*
 * Read from a vm_map_ram region of memory.
 *
 * Returns the number of copied bytes.
 */
static size_t vmap_ram_vread_iter(struct iov_iter *iter, const char *addr,
				  size_t count, unsigned long flags)
{
	/*
	 * 一个 VMAP_BLOCK 的 VA 包含多个活跃 bitmap 区间以及 free/dirty 洞。持有 vb 锁
	 * 固定 bitmap 快照，逐段复制活跃页、对洞补零，避免把旧 alias 内容暴露给 kcore。
	 */
	char *start;
	struct vmap_block *vb;
	struct xarray *xa;
	unsigned long offset;
	unsigned int rs, re;
	size_t remains, n;

	/*
	 * If it's area created by vm_map_ram() interface directly, but
	 * not further subdividing and delegating management to vmap_block,
	 * handle it here.
	 */
	if (!(flags & VMAP_BLOCK))
		return aligned_vread_iter(iter, addr, count);

	remains = count;

	/*
	 * Area is split into regions and tracked with vmap_block, read out
	 * each region and zero fill the hole between regions.
	 */
	xa = addr_to_vb_xa((unsigned long) addr);
	vb = xa_load(xa, addr_to_vb_idx((unsigned long)addr));
	if (!vb)
		goto finished_zero;

	spin_lock(&vb->lock);
	if (bitmap_empty(vb->used_map, VMAP_BBMAP_BITS)) {
		spin_unlock(&vb->lock);
		goto finished_zero;
	}

	/* set-bit range 表示仍归调用者所有的连续子映射；range 之间全部视为不可读。 */
	for_each_set_bitrange(rs, re, vb->used_map, VMAP_BBMAP_BITS) {
		size_t copied;

		if (remains == 0)
			goto finished;

		start = vmap_block_vaddr(vb->va->va_start, rs);

		if (addr < start) {
			size_t to_zero = min_t(size_t, start - addr, remains);
			size_t zeroed = zero_iter(iter, to_zero);

			addr += zeroed;
			remains -= zeroed;

			if (remains == 0 || zeroed != to_zero)
				goto finished;
		}

		/*it could start reading from the middle of used region*/
		offset = offset_in_page(addr);
		n = ((re - rs + 1) << PAGE_SHIFT) - offset;
		if (n > remains)
			n = remains;

		copied = aligned_vread_iter(iter, start + offset, n);

		addr += copied;
		remains -= copied;

		if (copied != n)
			goto finished;
	}

	spin_unlock(&vb->lock);

finished_zero:
	/* zero-fill the left dirty or free regions */
	return count - remains + zero_iter(iter, remains);
finished:
	/* We couldn't copy/zero everything */
	spin_unlock(&vb->lock);
	return count - remains;
}

/**
 * vread_iter() - read vmalloc area in a safe way to an iterator.
 * @iter:         the iterator to which data should be written.
 * @addr:         vm address.
 * @count:        number of bytes to be read.
 *
 * This function checks that addr is a valid vmalloc'ed area, and
 * copies data from that area to a given iterator. If the given memory range
 * of [addr...addr+count) includes some valid address, data is copied to
 * proper area of @iter. If there are memory holes, they'll be zero-filled.
 * IOREMAP area is treated as memory hole and no copy is done.
 *
 * If [addr...addr+count) doesn't includes any intersects with alive
 * vm_struct area, returns 0.
 *
 * Note: In usual ops, vread_iter() is never necessary because the caller
 * should know vmalloc() area is valid and can use memcpy().
 * This is for routines which have to access vmalloc area without
 * any information, as /proc/kcore.
 *
 * Return: number of bytes for which addr and iter should be advanced
 * (same number as @count) or %0 if [addr...addr+count) doesn't
 * include any intersection with valid vmalloc area
 */
long vread_iter(struct iov_iter *iter, const char *addr, size_t count)
{
	/*
	 * 这是面向 /proc/kcore 等“不掌握对象生命周期”的容错遍历器，不是普通数据路径。
	 * 它按 busy tree 顺序覆盖请求区间：活跃普通映射安全复制，IO/sparse/地址洞补零，
	 * 未完成发布的 vm_struct 跳过。每跨过一个 area 都释放节点锁后重新查找，避免
	 * 长时间阻塞分配/释放；代价是只能提供安全快照语义，而非全区间原子快照。
	 */
	struct vmap_node *vn;
	struct vmap_area *va;
	struct vm_struct *vm;
	char *vaddr;
	size_t n, size, flags, remains;
	unsigned long next;

	addr = kasan_reset_tag(addr);

	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;

	remains = count;

	/* helper 返回时持有对应 busy.lock，va 生命周期在本轮检查和复制期间稳定。 */
	vn = find_vmap_area_exceed_addr_lock((unsigned long) addr, &va);
	if (!vn)
		goto finished_zero;

	/* no intersects with alive vmap_area */
	if ((unsigned long)addr + remains <= va->va_start)
		goto finished_zero;

	do {
		size_t copied;

		if (remains == 0)
			goto finished;

		vm = va->vm;
		flags = va->flags & VMAP_FLAGS_MASK;
		/*
		 * VMAP_BLOCK indicates a sub-type of vm_map_ram area, need
		 * be set together with VMAP_RAM.
		 */
		WARN_ON(flags == VMAP_BLOCK);

		if (!vm && !flags)
			goto next_va;

		/* 跳过已进树但物理页/页表/KASAN 尚未全部发布的分配。 */
		if (vm && (vm->flags & VM_UNINITIALIZED))
			goto next_va;

		/* Pair with smp_wmb() in clear_vm_uninitialized_flag() */
		smp_rmb();

		vaddr = (char *) va->va_start;
		if (vm)
			/*
			 * For VM_ALLOC areas, use nr_pages rather than
			 * get_vm_area_size() because vrealloc() may shrink
			 * the mapping without updating area->size. Other
			 * mapping types (vmap, ioremap) don't set nr_pages.
			 */
			size = (vm->flags & VM_ALLOC && vm->nr_pages) ?
				       (vm->nr_pages << PAGE_SHIFT) :
				       get_vm_area_size(vm);
		else
			size = va_size(va);

		if (addr >= vaddr + size)
			goto next_va;

		/* 当前游标到下一个活跃 area 之间的 VA 洞必须显式输出零。 */
		if (addr < vaddr) {
			size_t to_zero = min_t(size_t, vaddr - addr, remains);
			size_t zeroed = zero_iter(iter, to_zero);

			addr += zeroed;
			remains -= zeroed;

			if (remains == 0 || zeroed != to_zero)
				goto finished;
		}

		n = vaddr + size - addr;
		if (n > remains)
			n = remains;

		/* vm_map_ram 需解释 block bitmap；设备和 sparse 映射绝不能作为普通 RAM 读取。 */
		if (flags & VMAP_RAM)
			copied = vmap_ram_vread_iter(iter, addr, n, flags);
		else if (!(vm && (vm->flags & (VM_IOREMAP | VM_SPARSE))))
			copied = aligned_vread_iter(iter, addr, n);
		else /* IOREMAP | SPARSE area is treated as memory hole */
			copied = zero_iter(iter, n);

		addr += copied;
		remains -= copied;

		if (copied != n)
			goto finished;

	next_va:
		next = va->va_end;
		spin_unlock(&vn->busy.lock);
	} while ((vn = find_vmap_area_exceed_addr_lock(next, &va)));

finished_zero:
	if (vn)
		spin_unlock(&vn->busy.lock);

	/* zero-fill memory holes */
	return count - remains + zero_iter(iter, remains);
finished:
	/* Nothing remains, or We couldn't copy/zero everything. */
	if (vn)
		spin_unlock(&vn->busy.lock);

	return count - remains;
}

/**
 * remap_vmalloc_range_partial - map vmalloc pages to userspace
 * @vma:		vma to cover
 * @uaddr:		target user address to start at
 * @kaddr:		virtual address of vmalloc kernel memory
 * @pgoff:		offset from @kaddr to start at
 * @size:		size of map area
 *
 * Returns:	0 for success, -Exxx on failure
 *
 * This function checks that @kaddr is a valid vmalloc'ed area,
 * and that it is big enough to cover the range starting at
 * @uaddr in @vma. Will return failure if that criteria isn't
 * met.
 *
 * Similar to remap_pfn_range() (see mm/memory.c)
 */
int remap_vmalloc_range_partial(struct vm_area_struct *vma, unsigned long uaddr,
				void *kaddr, unsigned long pgoff,
				unsigned long size)
{
	/*
	 * 将内核 vmalloc backing pages 逐页插入用户 VMA。只有显式 VM_USERMAP 或
	 * VM_DMA_COHERENT 区域可导出；对 offset/size 的溢出与边界检查必须先于任何
	 * vm_insert_page，避免部分映射后才发现越界。成功后禁止 VMA 扩展及 core dump。
	 */
	struct vm_struct *area;
	unsigned long off;
	unsigned long end_index;

	if (check_shl_overflow(pgoff, PAGE_SHIFT, &off))
		return -EINVAL;

	size = PAGE_ALIGN(size);

	if (!PAGE_ALIGNED(uaddr) || !PAGE_ALIGNED(kaddr))
		return -EINVAL;

	/* kaddr 必须是登记区域的 base，pgoff 单独表达内部偏移，避免接受任意内点。 */
	area = find_vm_area(kaddr);
	if (!area)
		return -EINVAL;

	if (!(area->flags & (VM_USERMAP | VM_DMA_COHERENT)))
		return -EINVAL;

	if (check_add_overflow(size, off, &end_index) ||
	    end_index > get_vm_area_size(area))
		return -EINVAL;
	kaddr += off;

	/* vm_insert_page 为每个用户 PTE 获取所需页引用；中途失败保留已插入前缀供 VMA 清理。 */
	do {
		struct page *page = vmalloc_to_page(kaddr);
		int ret;

		ret = vm_insert_page(vma, uaddr, page);
		if (ret)
			return ret;

		uaddr += PAGE_SIZE;
		kaddr += PAGE_SIZE;
		size -= PAGE_SIZE;
	} while (size > 0);

	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

	return 0;
}

/**
 * remap_vmalloc_range - map vmalloc pages to userspace
 * @vma:		vma to cover (map full range of vma)
 * @addr:		vmalloc memory
 * @pgoff:		number of pages into addr before first page to map
 *
 * Returns:	0 for success, -Exxx on failure
 *
 * This function checks that addr is a valid vmalloc'ed area, and
 * that it is big enough to cover the vma. Will return failure if
 * that criteria isn't met.
 *
 * Similar to remap_pfn_range() (see mm/memory.c)
 */
int remap_vmalloc_range(struct vm_area_struct *vma, void *addr,
						unsigned long pgoff)
{
	/* 全区间包装器以 VMA 长度为 size，所有验证和逐页插入仍集中在 partial 版本。 */
	return remap_vmalloc_range_partial(vma, vma->vm_start,
					   addr, pgoff,
					   vma->vm_end - vma->vm_start);
}
EXPORT_SYMBOL(remap_vmalloc_range);

void free_vm_area(struct vm_struct *area)
{
	/* 仅释放地址映射描述，不释放 backing pages；严格校验摘除的是调用者传入对象。 */
	struct vm_struct *ret;
	ret = remove_vm_area(area->addr);
	BUG_ON(ret != area);
	kfree(area);
}
EXPORT_SYMBOL_GPL(free_vm_area);

#ifdef CONFIG_SMP
static struct vmap_area *node_to_va(struct rb_node *n)
{
	/* rb_entry_safe 允许逆向扫描越过树首时把 NULL 直接传播给终止逻辑。 */
	return rb_entry_safe(n, struct vmap_area, rb_node);
}

/**
 * pvm_find_va_enclose_addr - find the vmap_area @addr belongs to
 * @addr: target address
 *
 * Returns: vmap_area if it is found. If there is no such area
 *   the first highest(reverse order) vmap_area is returned
 *   i.e. va->va_start < addr && va->va_end < addr or NULL
 *   if there are no any areas before @addr.
 */
static struct vmap_area *
pvm_find_va_enclose_addr(unsigned long addr)
{
	/*
	 * 在全局 free tree 中找包含 addr 的洞；若不包含，则返回 addr 下方最近的洞。
	 * per-CPU allocator 随后从高地址向低地址扫描，因此这个 predecessor 语义正合适。
	 */
	struct vmap_area *va, *tmp;
	struct rb_node *n;

	n = free_vmap_area_root.rb_node;
	va = NULL;

	while (n) {
		tmp = rb_entry(n, struct vmap_area, rb_node);
		if (tmp->va_start <= addr) {
			va = tmp;
			if (tmp->va_end >= addr)
				break;

			n = n->rb_right;
		} else {
			n = n->rb_left;
		}
	}

	return va;
}

/**
 * pvm_determine_end_from_reverse - find the highest aligned address
 * of free block below VMALLOC_END
 * @va:
 *   in - the VA we start the search(reverse order);
 *   out - the VA with the highest aligned end address.
 * @align: alignment for required highest address
 *
 * Returns: determined end address within vmap_area
 */
static unsigned long
pvm_determine_end_from_reverse(struct vmap_area **va, unsigned long align)
{
	/* 跳过对齐后为空的洞，返回不超过 VMALLOC_END 的最高可用对齐终点。 */
	unsigned long vmalloc_end = VMALLOC_END & ~(align - 1);
	unsigned long addr;

	if (likely(*va)) {
		list_for_each_entry_from_reverse((*va),
				&free_vmap_area_list, list) {
			addr = min((*va)->va_end & ~(align - 1), vmalloc_end);
			if ((*va)->va_start < addr)
				return addr;
		}
	}

	return 0;
}

/**
 * pcpu_get_vm_areas - allocate vmalloc areas for percpu allocator
 * @offsets: array containing offset of each area
 * @sizes: array containing size of each area
 * @nr_vms: the number of areas to allocate
 * @align: alignment, all entries in @offsets and @sizes must be aligned to this
 *
 * Returns: kmalloc'd vm_struct pointer array pointing to allocated
 *	    vm_structs on success, %NULL on failure
 *
 * Percpu allocator wants to use congruent vm areas so that it can
 * maintain the offsets among percpu areas.  This function allocates
 * congruent vmalloc areas for it with GFP_KERNEL.  These areas tend to
 * be scattered pretty far, distance between two areas easily going up
 * to gigabytes.  To avoid interacting with regular vmallocs, these
 * areas are allocated from top.
 *
 * Despite its complicated look, this allocator is rather simple. It
 * does everything top-down and scans free blocks from the end looking
 * for matching base. While scanning, if any of the areas do not fit the
 * base address is pulled down to fit the area. Scanning is repeated till
 * all the areas fit and then all necessary data structures are inserted
 * and the result is returned.
 */
struct vm_struct **pcpu_get_vm_areas(const unsigned long *offsets,
				     const size_t *sizes, int nr_vms,
				     size_t align)
{
	/*
	 * percpu 需要多段 KVA 共享同一 base，使各段间 offsets 在所有 CPU chunk 中保持
	 * 同余。普通逐段分配无法保证这个整体约束，因此这里在全局 free tree 上做一次
	 * 自顶向下的多区间装箱：不断下移 base，直到所有 [base+offset, +size) 同时落洞。
	 * 搜索成功后才批量裁剪 free tree、建立 KASAN shadow 并发布 busy 节点，近似事务。
	 */
	const unsigned long vmalloc_start = ALIGN(VMALLOC_START, align);
	const unsigned long vmalloc_end = VMALLOC_END & ~(align - 1);
	struct vmap_area **vas, *va;
	struct vm_struct **vms;
	int area, area2, last_area, term_area;
	unsigned long base, start, size, end, last_end, orig_start, orig_end;
	bool purged = false;

	/* verify parameters and allocate data structures */
	/* 先验证对齐、互不重叠，并找到相对终点最高的 area 作为反向扫描锚点。 */
	BUG_ON(offset_in_page(align) || !is_power_of_2(align));
	for (last_area = 0, area = 0; area < nr_vms; area++) {
		start = offsets[area];
		end = start + sizes[area];

		/* is everything aligned properly? */
		BUG_ON(!IS_ALIGNED(offsets[area], align));
		BUG_ON(!IS_ALIGNED(sizes[area], align));

		/* detect the area with the highest address */
		if (start > offsets[last_area])
			last_area = area;

		for (area2 = area + 1; area2 < nr_vms; area2++) {
			unsigned long start2 = offsets[area2];
			unsigned long end2 = start2 + sizes[area2];

			BUG_ON(start2 < end && start < end2);
		}
	}
	last_end = offsets[last_area] + sizes[last_area];

	if (vmalloc_end - vmalloc_start < last_end) {
		WARN_ON(true);
		return NULL;
	}

	/* 所有描述符预分配，保证持有 free tree 锁的提交阶段不因普通内存分配失败。 */
	vms = kzalloc_objs(vms[0], nr_vms);
	vas = kzalloc_objs(vas[0], nr_vms);
	if (!vas || !vms)
		goto err_free2;

	for (area = 0; area < nr_vms; area++) {
		vas[area] = kmem_cache_zalloc(vmap_area_cachep, GFP_KERNEL);
		vms[area] = kzalloc_obj(struct vm_struct);
		if (!vas[area] || !vms[area])
			goto err_free;
	}
retry:
	spin_lock(&free_vmap_area_lock);

	/* start scanning - we scan from the top, begin with the last area */
	area = term_area = last_area;
	start = offsets[area];
	end = start + sizes[area];

	/* 令最高相对终点贴近最高可用洞，再环形检查其余 area 是否都能容纳。 */
	va = pvm_find_va_enclose_addr(vmalloc_end);
	base = pvm_determine_end_from_reverse(&va, align) - end;

	/* 每次冲突只会把 base 向下拉，搜索单调且最终到达成功或地址空间下界。 */
	while (true) {
		/*
		 * base might have underflowed, add last_end before
		 * comparing.
		 */
		if (base + last_end < vmalloc_start + last_end)
			goto overflow;

		/*
		 * Fitting base has not been found.
		 */
		if (va == NULL)
			goto overflow;

		/*
		 * If required width exceeds current VA block, move
		 * base downwards and then recheck.
		 */
		if (base + end > va->va_end) {
			base = pvm_determine_end_from_reverse(&va, align) - end;
			term_area = area;
			continue;
		}

		/*
		 * If this VA does not fit, move base downwards and recheck.
		 */
		if (base + start < va->va_start) {
			va = node_to_va(rb_prev(&va->rb_node));
			base = pvm_determine_end_from_reverse(&va, align) - end;
			term_area = area;
			continue;
		}

		/*
		 * This area fits, move on to the previous one.  If
		 * the previous one is the terminal one, we're done.
		 */
		area = (area + nr_vms - 1) % nr_vms;
		if (area == term_area)
			break;

		start = offsets[area];
		end = start + sizes[area];
		va = pvm_find_va_enclose_addr(base + end);
	}

	/* we've found a fitting base, insert all va's */
	/* 已找到共同 base；在同一 free-tree 锁临界区逐段裁剪，其他分配者看不到半提交。 */
	for (area = 0; area < nr_vms; area++) {
		int ret;

		start = base + offsets[area];
		size = sizes[area];

		va = pvm_find_va_enclose_addr(start);
		if (WARN_ON_ONCE(va == NULL))
			/* It is a BUG(), but trigger recovery instead. */
			goto recovery;

		ret = va_clip(&free_vmap_area_root,
			&free_vmap_area_list, va, start, size);
		if (WARN_ON_ONCE(unlikely(ret)))
			/* It is a BUG(), but trigger recovery instead. */
			goto recovery;

		/* Allocated area. */
		va = vas[area];
		va->va_start = start;
		va->va_end = start + size;
	}

	spin_unlock(&free_vmap_area_lock);

	/* populate the kasan shadow space */
	/* free tree 已占位但 busy tree 尚未发布，先准备全部 shadow；失败仍可整体回滚。 */
	for (area = 0; area < nr_vms; area++) {
		if (kasan_populate_vmalloc(vas[area]->va_start, sizes[area], GFP_KERNEL))
			goto err_free_shadow;
	}

	/* insert all vm's */
	/* 最终逐节点发布到分片 busy tree，并建立 vmap_area->vm 的高层关联。 */
	for (area = 0; area < nr_vms; area++) {
		struct vmap_node *vn = addr_to_node(vas[area]->va_start);

		spin_lock(&vn->busy.lock);
		insert_vmap_area(vas[area], &vn->busy.root, &vn->busy.head);
		setup_vmalloc_vm(vms[area], vas[area], VM_ALLOC,
				 pcpu_get_vm_areas);
		spin_unlock(&vn->busy.lock);
	}

	/*
	 * Mark allocated areas as accessible. Do it now as a best-effort
	 * approach, as they can be mapped outside of vmalloc code.
	 * With hardware tag-based KASAN, marking is skipped for
	 * non-VM_ALLOC mappings, see __kasan_unpoison_vmalloc().
	 */
	kasan_unpoison_vmap_areas(vms, nr_vms, KASAN_VMALLOC_PROT_NORMAL);

	kfree(vas);
	return vms;

recovery:
	/*
	 * Remove previously allocated areas. There is no
	 * need in removing these areas from the busy tree,
	 * because they are inserted only on the final step
	 * and when pcpu_get_vm_areas() is success.
	 */
	/* va_clip 中途异常时，把此前裁剪出的前缀逐一并回 free tree。 */
	while (area--) {
		orig_start = vas[area]->va_start;
		orig_end = vas[area]->va_end;
		va = merge_or_add_vmap_area_augment(vas[area], &free_vmap_area_root,
				&free_vmap_area_list);
		if (va)
			kasan_release_vmalloc(orig_start, orig_end,
				va->va_start, va->va_end,
				KASAN_VMALLOC_PAGE_RANGE | KASAN_VMALLOC_TLB_FLUSH);
		vas[area] = NULL;
	}

overflow:
	spin_unlock(&free_vmap_area_lock);
	if (!purged) {
		/* lazy VA 可能造成假性空间不足；只允许一次全局 purge 后从头搜索。 */
		reclaim_and_purge_vmap_areas();
		purged = true;

		/* Before "retry", check if we recover. */
		for (area = 0; area < nr_vms; area++) {
			if (vas[area])
				continue;

			vas[area] = kmem_cache_zalloc(
				vmap_area_cachep, GFP_KERNEL);
			if (!vas[area])
				goto err_free;
		}

		goto retry;
	}

err_free:
	for (area = 0; area < nr_vms; area++) {
		if (vas[area])
			kmem_cache_free(vmap_area_cachep, vas[area]);

		kfree(vms[area]);
	}
err_free2:
	kfree(vas);
	kfree(vms);
	return NULL;

err_free_shadow:
	/* shadow 阶段失败时所有 VA 都已裁剪但尚未进 busy tree，可在 free 锁下整体归还。 */
	spin_lock(&free_vmap_area_lock);
	/*
	 * We release all the vmalloc shadows, even the ones for regions that
	 * hadn't been successfully added. This relies on kasan_release_vmalloc
	 * being able to tolerate this case.
	 */
	for (area = 0; area < nr_vms; area++) {
		orig_start = vas[area]->va_start;
		orig_end = vas[area]->va_end;
		va = merge_or_add_vmap_area_augment(vas[area], &free_vmap_area_root,
				&free_vmap_area_list);
		if (va)
			kasan_release_vmalloc(orig_start, orig_end,
				va->va_start, va->va_end,
				KASAN_VMALLOC_PAGE_RANGE | KASAN_VMALLOC_TLB_FLUSH);
		vas[area] = NULL;
		kfree(vms[area]);
	}
	spin_unlock(&free_vmap_area_lock);
	kfree(vas);
	kfree(vms);
	return NULL;
}

/**
 * pcpu_free_vm_areas - free vmalloc areas for percpu allocator
 * @vms: vm_struct pointer array returned by pcpu_get_vm_areas()
 * @nr_vms: the number of allocated areas
 *
 * Free vm_structs and the array allocated by pcpu_get_vm_areas().
 */
void pcpu_free_vm_areas(struct vm_struct **vms, int nr_vms)
{
	/* 这些 area 只代表地址预留，backing 映射由 percpu 子系统另行管理。 */
	int i;

	for (i = 0; i < nr_vms; i++)
		free_vm_area(vms[i]);
	kfree(vms);
}
#endif	/* CONFIG_SMP */

#ifdef CONFIG_PRINTK
bool vmalloc_dump_obj(void *object)
{
	/*
	 * printk 错误路径不能等待可能由当前上下文持有的 busy 锁，所以 trylock 失败即放弃。
	 * 锁内只快照诊断字段，符号解析和打印放在锁外，避免递归进入复杂基础设施。
	 */
	const void *caller;
	struct vm_struct *vm;
	struct vmap_area *va;
	struct vmap_node *vn;
	unsigned long addr;
	unsigned int nr_pages;

	addr = PAGE_ALIGN((unsigned long) object);
	vn = addr_to_node(addr);

	if (!spin_trylock(&vn->busy.lock))
		return false;

	va = __find_vmap_area(addr, &vn->busy.root);
	if (!va || !va->vm) {
		spin_unlock(&vn->busy.lock);
		return false;
	}

	vm = va->vm;
	addr = (unsigned long) vm->addr;
	caller = vm->caller;
	nr_pages = vm->nr_pages;
	spin_unlock(&vn->busy.lock);

	pr_cont(" %u-page vmalloc region starting at %#lx allocated at %pS\n",
		nr_pages, addr, caller);

	return true;
}
#endif

#ifdef CONFIG_PROC_FS

/*
 * Print number of pages allocated on each memory node.
 *
 * This function can only be called if CONFIG_NUMA is enabled
 * and VM_UNINITIALIZED bit in v->flags is disabled.
 */
static void show_numa_info(struct seq_file *m, struct vm_struct *v,
				 unsigned int *counters)
{
	/* pages[] 按 base page 展开；大页映射按 page_order 跨步并一次累计整块页数。 */
	unsigned int nr;
	unsigned int step = 1U << vm_area_page_order(v);

	if (!counters)
		return;

	memset(counters, 0, nr_node_ids * sizeof(unsigned int));

	for (nr = 0; nr < v->nr_pages; nr += step)
		counters[page_to_nid(v->pages[nr])] += step;
	for_each_node_state(nr, N_HIGH_MEMORY)
		if (counters[nr])
			seq_printf(m, " N%u=%u", nr, counters[nr]);
}

static void show_purge_info(struct seq_file *m)
{
	/* lazy tree 中的区间已从 busy 视图消失，但 TLB flush 前仍占用 VA，单独展示。 */
	struct vmap_node *vn;
	struct vmap_area *va;

	for_each_vmap_node(vn) {
		spin_lock(&vn->lazy.lock);
		list_for_each_entry(va, &vn->lazy.head, list) {
			seq_printf(m, "0x%pK-0x%pK %7ld unpurged vm_area\n",
				(void *)va->va_start, (void *)va->va_end,
				va_size(va));
		}
		spin_unlock(&vn->lazy.lock);
	}
}

static int vmalloc_info_show(struct seq_file *m, void *p)
{
	/*
	 * /proc/vmallocinfo 逐个分片持锁生成弱一致快照。它跳过未发布对象，并依赖
	 * clear_vm_uninitialized_flag 的写屏障读取完整字段；输出期间持锁的代价可接受，
	 * 因为这是低频诊断接口。最后追加 lazy 区间解释“地址已释放却尚不可复用”。
	 */
	struct vmap_node *vn;
	struct vmap_area *va;
	struct vm_struct *v;
	unsigned int *counters;

	if (IS_ENABLED(CONFIG_NUMA))
		counters = kmalloc_array(nr_node_ids, sizeof(unsigned int), GFP_KERNEL);

	for_each_vmap_node(vn) {
		spin_lock(&vn->busy.lock);
		list_for_each_entry(va, &vn->busy.head, list) {
			if (!va->vm) {
				if (va->flags & VMAP_RAM)
					seq_printf(m, "0x%pK-0x%pK %7ld vm_map_ram\n",
						(void *)va->va_start, (void *)va->va_end,
						va_size(va));

				continue;
			}

			v = va->vm;
			if (v->flags & VM_UNINITIALIZED)
				continue;

			/* Pair with smp_wmb() in clear_vm_uninitialized_flag() */
			smp_rmb();

			seq_printf(m, "0x%pK-0x%pK %7ld",
				v->addr, v->addr + v->size, v->size);

			if (v->caller)
				seq_printf(m, " %pS", v->caller);

			if (v->nr_pages)
				seq_printf(m, " pages=%d", v->nr_pages);

			if (v->phys_addr)
				seq_printf(m, " phys=%pa", &v->phys_addr);

			if (v->flags & VM_IOREMAP)
				seq_puts(m, " ioremap");

			if (v->flags & VM_SPARSE)
				seq_puts(m, " sparse");

			if (v->flags & VM_ALLOC)
				seq_puts(m, " vmalloc");

			if (v->flags & VM_MAP)
				seq_puts(m, " vmap");

			if (v->flags & VM_USERMAP)
				seq_puts(m, " user");

			if (v->flags & VM_DMA_COHERENT)
				seq_puts(m, " dma-coherent");

			if (is_vmalloc_addr(v->pages))
				seq_puts(m, " vpages");

			if (IS_ENABLED(CONFIG_NUMA))
				show_numa_info(m, v, counters);

			seq_putc(m, '\n');
		}
		spin_unlock(&vn->busy.lock);
	}

	/*
	 * As a final step, dump "unpurged" areas.
	 */
	show_purge_info(m);
	if (IS_ENABLED(CONFIG_NUMA))
		kfree(counters);
	return 0;
}

static int __init proc_vmalloc_init(void)
{
	proc_create_single("vmallocinfo", 0400, NULL, vmalloc_info_show);
	return 0;
}
module_init(proc_vmalloc_init);

#endif

static void __init vmap_init_free_space(void)
{
	/*
	 * early vmlist 是按地址排序的 busy 集合。本函数取其补集，构造运行期增强 free
	 * tree/list：从地址 1 而非 0 开始保留 NULL 语义，并在最后补上尾部大洞。
	 */
	unsigned long vmap_start = 1;
	const unsigned long vmap_end = ULONG_MAX;
	struct vmap_area *free;
	struct vm_struct *busy;

	/*
	 *     B     F     B     B     B     F
	 * -|-----|.....|-----|-----|-----|.....|-
	 *  |           The KVA space           |
	 *  |<--------------------------------->|
	 */
	for (busy = vmlist; busy; busy = busy->next) {
		if ((unsigned long) busy->addr - vmap_start > 0) {
			free = kmem_cache_zalloc(vmap_area_cachep, GFP_NOWAIT);
			if (!WARN_ON_ONCE(!free)) {
				free->va_start = vmap_start;
				free->va_end = (unsigned long) busy->addr;

				insert_vmap_area_augment(free, NULL,
					&free_vmap_area_root,
						&free_vmap_area_list);
			}
		}

		vmap_start = (unsigned long) busy->addr + busy->size;
	}

	if (vmap_end - vmap_start > 0) {
		free = kmem_cache_zalloc(vmap_area_cachep, GFP_NOWAIT);
		if (!WARN_ON_ONCE(!free)) {
			free->va_start = vmap_start;
			free->va_end = vmap_end;

			insert_vmap_area_augment(free, NULL,
				&free_vmap_area_root,
					&free_vmap_area_list);
		}
	}
}

static void vmap_init_nodes(void)
{
	/*
	 * busy/lazy 索引按地址 zone 分片以降低大机器锁竞争，节点数最多 128；数组分配
	 * 失败就退化为静态单节点，正确性不依赖分片。每节点另有按大小分桶的 VA 缓存。
	 */
	struct vmap_node *vn;
	int i;

#if BITS_PER_LONG == 64
	/*
	 * A high threshold of max nodes is fixed and bound to 128,
	 * thus a scale factor is 1 for systems where number of cores
	 * are less or equal to specified threshold.
	 *
	 * As for NUMA-aware notes. For bigger systems, for example
	 * NUMA with multi-sockets, where we can end-up with thousands
	 * of cores in total, a "sub-numa-clustering" should be added.
	 *
	 * In this case a NUMA domain is considered as a single entity
	 * with dedicated sub-nodes in it which describe one group or
	 * set of cores. Therefore a per-domain purging is supposed to
	 * be added as well as a per-domain balancing.
	 */
	int n = clamp_t(unsigned int, num_possible_cpus(), 1, 128);

	if (n > 1) {
		vn = kmalloc_objs(*vn, n, GFP_NOWAIT);
		if (vn) {
			/* Node partition is 16 pages. */
			vmap_zone_size = (1 << 4) * PAGE_SIZE;
			nr_vmap_nodes = n;
			vmap_nodes = vn;
		} else {
			pr_err("Failed to allocate an array. Disable a node layer\n");
		}
	}
#endif

	for_each_vmap_node(vn) {
		vn->busy.root = RB_ROOT;
		INIT_LIST_HEAD(&vn->busy.head);
		spin_lock_init(&vn->busy.lock);

		vn->lazy.root = RB_ROOT;
		INIT_LIST_HEAD(&vn->lazy.head);
		spin_lock_init(&vn->lazy.lock);

		for (i = 0; i < MAX_VA_SIZE_PAGES; i++) {
			INIT_LIST_HEAD(&vn->pool[i].head);
			WRITE_ONCE(vn->pool[i].len, 0);
		}

		spin_lock_init(&vn->pool_lock);
	}
}

static unsigned long
vmap_node_shrink_count(struct shrinker *shrink, struct shrink_control *sc)
{
	/* shrinker 只统计节点 pool 中缓存的描述符数量，不把仍在 busy/lazy 的区间算作可回收。 */
	unsigned long count = 0;
	struct vmap_node *vn;
	int i;

	for_each_vmap_node(vn) {
		for (i = 0; i < MAX_VA_SIZE_PAGES; i++)
			count += READ_ONCE(vn->pool[i].len);
	}

	return count ? count : SHRINK_EMPTY;
}

static unsigned long
vmap_node_shrink_scan(struct shrinker *shrink, struct shrink_control *sc)
{
	/* 全局 purge mutex 与正常衰减串行，强制把各节点过期 pool 项归还到底层。 */
	struct vmap_node *vn;

	guard(mutex)(&vmap_purge_lock);
	for_each_vmap_node(vn)
		decay_va_pool_node(vn, true);

	return SHRINK_STOP;
}

void __init vmalloc_init(void)
{
	/*
	 * 初始化顺序存在依赖：先建描述符 cache 和 per-CPU 快路径，再建分片节点；随后
	 * 把 early vmlist 导入 busy tree，以其补集生成 free tree，最后才置 initialized。
	 * 这个布尔值是运行期 API 可访问树结构的发布点；shrinker 注册失败只损失回收
	 * 效率，不影响已经可用的 vmalloc 分配器。
	 */
	struct shrinker *vmap_node_shrinker;
	struct vmap_area *va;
	struct vmap_node *vn;
	struct vm_struct *tmp;
	int i;

	/*
	 * Create the cache for vmap_area objects.
	 */
	vmap_area_cachep = KMEM_CACHE(vmap_area, SLAB_PANIC);

	/* 离线但 possible 的 CPU 将来也可能上线，queue/work/xarray 必须预先完整初始化。 */
	for_each_possible_cpu(i) {
		struct vmap_block_queue *vbq;
		struct vfree_deferred *p;

		vbq = &per_cpu(vmap_block_queue, i);
		spin_lock_init(&vbq->lock);
		INIT_LIST_HEAD(&vbq->free);
		p = &per_cpu(vfree_deferred, i);
		init_llist_head(&p->list);
		INIT_WORK(&p->wq, delayed_vfree_work);
		xa_init(&vbq->vmap_blocks);
	}

	/*
	 * Setup nodes before importing vmlist.
	 */
	vmap_init_nodes();

	/* Import existing vmlist entries. */
	/* 此时仍无并发运行期访问，导入 early busy 区域不需要逐节点加锁。 */
	for (tmp = vmlist; tmp; tmp = tmp->next) {
		va = kmem_cache_zalloc(vmap_area_cachep, GFP_NOWAIT);
		if (WARN_ON_ONCE(!va))
			continue;

		va->va_start = (unsigned long)tmp->addr;
		va->va_end = va->va_start + tmp->size;
		va->vm = tmp;

		vn = addr_to_node(va->va_start);
		insert_vmap_area(va, &vn->busy.root, &vn->busy.head);
	}

	/*
	 * Now we can initialize a free vmap space.
	 */
	/* free tree 完成后再发布 initialized，避免查询者看到只有 busy、没有 free 的半状态。 */
	vmap_init_free_space();
	vmap_initialized = true;

	vmap_node_shrinker = shrinker_alloc(0, "vmap-node");
	if (!vmap_node_shrinker) {
		pr_err("Failed to allocate vmap-node shrinker!\n");
		return;
	}

	vmap_node_shrinker->count_objects = vmap_node_shrink_count;
	vmap_node_shrinker->scan_objects = vmap_node_shrink_scan;
	shrinker_register(vmap_node_shrinker);
}
