// SPDX-License-Identifier: GPL-2.0-only
/*
 * This kernel test validates architecture page table helpers and
 * accessors and helps in verifying their continued compliance with
 * expected generic MM semantics.
 *
 * Copyright (C) 2019 ARM Ltd.
 *
 * Author: Anshuman Khandual <anshuman.khandual@arm.com>
 */
/*
 * 该启动期自测验证各体系结构提供的页表 helper/访问器是否持续遵守通用 MM 语义。
 * 测试只在自建的 mm、VMA 和页表页上改写条目；版权与作者行属于元数据，不参与翻译验收。
 */
#define pr_fmt(fmt) "debug_vm_pgtable: [%-25s]: " fmt, __func__

/* 通用头文件提供分配、MM 对象、页表/softleaf helper、日志、随机地址和各配置能力。 */
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/hugetlb.h>
#include <linux/kernel.h>
#include <linux/kconfig.h>
#include <linux/memblock.h>
/* 中间组定义 mm/VMA 类型、权限位和基础生命周期接口。 */
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mm_types.h>
#include <linux/module.h>
#include <linux/printk.h>
/* 下半组集中提供页表原语、锁、swap/leaf 编码、启动符号和页表页分配接口。 */
#include <linux/pgtable.h>
#include <linux/random.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/leafops.h>
#include <linux/start_kernel.h>
#include <linux/sched/mm.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/pgalloc.h>

/* 架构头补充 cache/TLB 同步契约，供真实槽位修改测试使用。 */
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

/*
 * Please refer Documentation/mm/arch_pgtable_helpers.rst for the semantics
 * expectations that are being validated here. All future changes in here
 * or the documentation need to be in sync.
 */
/*
 * 各 helper 的预期语义以 Documentation/mm/arch_pgtable_helpers.rst 为准；这里的断言与文档
 * 必须同步修改，否则测试通过也不能证明接口仍满足当前文档契约。
 */
#define RANDOM_NZVALUE	GENMASK(7, 0)

/*
 * 一次完整页表自测的私有上下文。init_args() 创建并拥有 mm/VMA、各级页表页以及可选的
 * 普通页/大页，destroy_args() 逆序回收；测试 helper 都只借用该结构，不接管资源。
 * pgdp..ptep 指向随机用户地址对应的当前条目，start_* 保存页表页基址供释放；fixed_*_pfn
 * 是只用于编码/判定的有效物理地址，pud/pmd/pte_pfn 则指向允许真实访问的已分配页。
 * vaddr 与两种 pgprot 驱动普通和 PROT_NONE 测试；swp_entry/leaf_entry 保存非 present 编码。
 * 初始化和销毁均在单线程 late_initcall 路径运行，无并发共享，页表修改阶段另行持对应锁。
 */
struct pgtable_debug_args {
	/* mm 是沙箱地址空间 owner，vma 是未发布但供权限 helper 读取的测试 VMA。 */
	struct mm_struct	*mm;
	struct vm_area_struct	*vma;

	pgd_t			*pgdp;
	p4d_t			*p4dp;
	pud_t			*pudp;
	pmd_t			*pmdp;
	pte_t			*ptep;

	/* start_* 指向各已分配页表页的基址，专供销毁时按层级释放。 */
	p4d_t			*start_p4dp;
	pud_t			*start_pudp;
	pmd_t			*start_pmdp;
	pgtable_t		start_ptep;

	unsigned long		vaddr;
	pgprot_t		page_prot;
	pgprot_t		page_prot_none;

	/* is_contiguous_page 记录大页来源；三种 PFN 可别名到同一份最高阶分配。 */
	bool			is_contiguous_page;
	unsigned long		pud_pfn;
	unsigned long		pmd_pfn;
	unsigned long		pte_pfn;

	/* fixed_alignment 说明可安全测试的最大物理对齐；fixed PFN 仅编码、不拥有物理页。 */
	unsigned long		fixed_alignment;
	unsigned long		fixed_pgd_pfn;
	unsigned long		fixed_p4d_pfn;
	unsigned long		fixed_pud_pfn;
	unsigned long		fixed_pmd_pfn;
	unsigned long		fixed_pte_pfn;

	/* swp_entry 测普通 swap PTE，leaf_entry 测 THP migration softleaf。 */
	swp_entry_t		swp_entry;
	swp_entry_t		leaf_entry;
};

/*
 * pte_basic_tests() - 验证 PTE 纯值转换 helper 的代数性质。
 * 业务背景：debug_vm_pgtable() 对每种 VM 权限组合调用它，确认架构位布局不会破坏通用 MM 判断。
 * 入参：args 为借用的自测上下文；idx 是 VM_* 权限位组合，用于生成初始 pgprot，不转移所有权。
 * 出参/返回：无直接返回值；仅在契约被破坏时触发 WARN，不写真实页表，也不改变 args。
 * 注意事项：启动期进程上下文可睡眠但本函数不睡眠；fixed_pte_pfn 只需是有效编码目标。
 */
static void __init pte_basic_tests(struct pgtable_debug_args *args, int idx)
{
	/* prot/pte 是当前权限样本；val/ptr 仅让 %pGv 以 VM flags 形式打印 idx。 */
	pgprot_t prot = vm_get_page_prot(idx);
	pte_t pte = pfn_pte(args->fixed_pte_pfn, prot);
	unsigned long val = idx, *ptr = &val;

	pr_debug("Validating PTE basic (%pGv)\n", ptr);

	/*
	 * This test needs to be executed after the given page table entry
	 * is created with pfn_pte() to make sure that vm_get_page_prot(idx)
	 * does not have the dirty bit enabled from the beginning. This is
	 * important for platforms like arm64 where (!PTE_RDONLY) indicate
	 * dirty bit being set.
	 */
	/*
	 * 必须先用 pfn_pte() 生成条目再检查：vm_get_page_prot(idx) 初始不能携带 dirty；arm64
	 * 用“非只读”编码 dirty，若初始权限已污染该位，后续 clean/dirty 互逆测试会失真。
	 */
	WARN_ON(pte_dirty(pte_wrprotect(pte)));

	/* 第一组验证 same、young、dirty、write 的置位与清位 helper 互为可观察的逆操作。 */
	WARN_ON(!pte_same(pte, pte));
	WARN_ON(!pte_young(pte_mkyoung(pte_mkold(pte))));
	WARN_ON(!pte_dirty(pte_mkdirty(pte_mkclean(pte))));
	WARN_ON(!pte_write(pte_mkwrite(pte_wrprotect(pte), args->vma)));
	WARN_ON(pte_young(pte_mkold(pte_mkyoung(pte))));
	WARN_ON(pte_dirty(pte_mkclean(pte_mkdirty(pte))));
	WARN_ON(pte_write(pte_wrprotect(pte_mkwrite(pte, args->vma))));
	WARN_ON(pte_dirty(pte_wrprotect(pte_mkclean(pte))));
	WARN_ON(!pte_dirty(pte_wrprotect(pte_mkdirty(pte))));

	/* 第二组专测无 VMA 写权限 helper，确保 write 与 dirty 两个位的组合仍彼此独立。 */
	WARN_ON(!pte_dirty(pte_mkwrite_novma(pte_mkdirty(pte))));
	WARN_ON(pte_dirty(pte_mkwrite_novma(pte_mkclean(pte))));
	WARN_ON(!pte_write(pte_mkdirty(pte_mkwrite_novma(pte))));
	WARN_ON(!pte_write(pte_mkwrite_novma(pte_wrprotect(pte))));
	WARN_ON(pte_write(pte_wrprotect(pte_mkwrite_novma(pte))));
}

/*
 * pte_advanced_tests() - 在真实 PTE 槽位上验证架构页表修改原语。
 * 业务背景：它位于持 PTE 锁的修改测试阶段，覆盖 set、写保护、access-flags、young 与 clear 协议。
 * 入参：args 为借用上下文；ptep 必须由 pte_offset_map_lock() 映射并加锁，pte_pfn 指向已分配页。
 * 出参/返回：无直接返回值；每轮结束把槽位恢复为 none，异常只通过 WARN 报告。
 * 注意事项：不接管 page/PTE；缺页或无映射槽时跳过，调用者仍负责解锁和最终释放。
 */
static void __init pte_advanced_tests(struct pgtable_debug_args *args)
{
	/* page 用于清理架构私有缓存标志，pte 是每个子测试的值快照。 */
	struct page *page;
	pte_t pte;

	/*
	 * Architectures optimize set_pte_at by avoiding TLB flush.
	 * This requires set_pte_at to be not used to update an
	 * existing pte entry. Clear pte before we do set_pte_at
	 *
	 * flush_dcache_page() is called after set_pte_at() to clear
	 * PG_arch_1 for the page on ARM64. The page flag isn't cleared
	 * when it's released and page allocation check will fail when
	 * the page is allocated again. For architectures other than ARM64,
	 * the unexpected overhead of cache flushing is acceptable.
	 */
	/*
	 * 某些架构的 set_pte_at() 不负责替换已有映射所需的 TLB 刷新，因此入口槽必须为空。
	 * 随后的 flush_dcache_page() 在 arm64 清 PG_arch_1，避免测试页释放后被分配器误判；
	 * 其它架构承担一次可接受的额外 cache flush。
	 */
	page = (args->pte_pfn != ULONG_MAX) ? pfn_to_page(args->pte_pfn) : NULL;
	if (!page)
		return;

	pr_debug("Validating PTE advanced\n");
	if (WARN_ON(!args->ptep))
		return;

	/* 阶段一：发布可写 PTE，再原地写保护并原子取走，最终槽位必须回到 none。 */
	pte = pfn_pte(args->pte_pfn, args->page_prot);
	set_pte_at(args->mm, args->vaddr, args->ptep, pte);
	flush_dcache_page(page);
	ptep_set_wrprotect(args->mm, args->vaddr, args->ptep);
	pte = ptep_get(args->ptep);
	WARN_ON(pte_write(pte));
	ptep_get_and_clear(args->mm, args->vaddr, args->ptep);
	pte = ptep_get(args->ptep);
	WARN_ON(!pte_none(pte));

	/* 阶段二：从只读且 clean 的旧值升级 access flags，验证 write/dirty 同时落入真实槽位。 */
	pte = pfn_pte(args->pte_pfn, args->page_prot);
	pte = pte_wrprotect(pte);
	pte = pte_mkclean(pte);
	set_pte_at(args->mm, args->vaddr, args->ptep, pte);
	flush_dcache_page(page);
	pte = pte_mkwrite(pte, args->vma);
	pte = pte_mkdirty(pte);
	ptep_set_access_flags(args->vma, args->vaddr, args->ptep, pte, 1);
	pte = ptep_get(args->ptep);
	/* 回读同时验证两个目标位，再用 full clear 模拟完整 mm teardown 语义。 */
	WARN_ON(!(pte_write(pte) && pte_dirty(pte)));
	ptep_get_and_clear_full(args->mm, args->vaddr, args->ptep, 1);
	pte = ptep_get(args->ptep);
	WARN_ON(!pte_none(pte));

	/* 阶段三：设置 young 后调用 test-and-clear，读取回来的条目必须已清 accessed 位。 */
	pte = pfn_pte(args->pte_pfn, args->page_prot);
	pte = pte_mkyoung(pte);
	set_pte_at(args->mm, args->vaddr, args->ptep, pte);
	flush_dcache_page(page);
	ptep_test_and_clear_young(args->vma, args->vaddr, args->ptep);
	pte = ptep_get(args->ptep);
	WARN_ON(pte_young(pte));

	/* 清掉最后一个测试条目，把空槽契约交还给后续销毁路径。 */
	ptep_get_and_clear_full(args->mm, args->vaddr, args->ptep, 1);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/*
 * pmd_basic_tests() - 验证透明大页 PMD 的纯值属性转换。
 * 业务背景：与 PTE 基础测试并行，保证 THP 叶条目在所有 VM 权限组合下满足通用 dirty/write/young 契约。
 * 入参：args 为借用上下文；idx 为 VM 权限位组合；均不发生 ownership 转移。
 * 出参/返回：无；仅 WARN，未启用运行期 THP 时跳过且不改变状态。
 * 注意事项：不触碰真实页表、无需页表锁；fixed_pmd_pfn 只用于构造测试值。
 */
static void __init pmd_basic_tests(struct pgtable_debug_args *args, int idx)
{
	/* prot/pmd 是权限样本；val/ptr 只服务于 VM flags 日志格式化。 */
	pgprot_t prot = vm_get_page_prot(idx);
	unsigned long val = idx, *ptr = &val;
	pmd_t pmd;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD basic (%pGv)\n", ptr);
	pmd = pfn_pmd(args->fixed_pmd_pfn, prot);

	/*
	 * This test needs to be executed after the given page table entry
	 * is created with pfn_pmd() to make sure that vm_get_page_prot(idx)
	 * does not have the dirty bit enabled from the beginning. This is
	 * important for platforms like arm64 where (!PTE_RDONLY) indicate
	 * dirty bit being set.
	 */
	/* 与 PTE 相同，先确认初始 pgprot 未借架构写权限编码意外带入 dirty。 */
	WARN_ON(pmd_dirty(pmd_wrprotect(pmd)));


	/* 验证状态位的置位、清位及组合不会破坏其它属性。 */
	WARN_ON(!pmd_same(pmd, pmd));
	WARN_ON(!pmd_young(pmd_mkyoung(pmd_mkold(pmd))));
	WARN_ON(!pmd_dirty(pmd_mkdirty(pmd_mkclean(pmd))));
	WARN_ON(!pmd_write(pmd_mkwrite(pmd_wrprotect(pmd), args->vma)));
	WARN_ON(pmd_young(pmd_mkold(pmd_mkyoung(pmd))));
	WARN_ON(pmd_dirty(pmd_mkclean(pmd_mkdirty(pmd))));
	WARN_ON(pmd_write(pmd_wrprotect(pmd_mkwrite(pmd, args->vma))));
	WARN_ON(pmd_dirty(pmd_wrprotect(pmd_mkclean(pmd))));
	WARN_ON(!pmd_dirty(pmd_wrprotect(pmd_mkdirty(pmd))));

	/* novma 版本同样必须保持 dirty 与 write 两个状态维度独立。 */
	WARN_ON(!pmd_dirty(pmd_mkwrite_novma(pmd_mkdirty(pmd))));
	WARN_ON(pmd_dirty(pmd_mkwrite_novma(pmd_mkclean(pmd))));
	WARN_ON(!pmd_write(pmd_mkdirty(pmd_mkwrite_novma(pmd))));
	WARN_ON(!pmd_write(pmd_mkwrite_novma(pmd_wrprotect(pmd))));
	WARN_ON(pmd_write(pmd_wrprotect(pmd_mkwrite_novma(pmd))));

	/*
	 * A huge page does not point to next level page table
	 * entry. Hence this must qualify as pmd_bad().
	 */
	/* 大页 PMD 是叶子而不是下一级表指针，因此按非叶校验规则观察时必须被判为 bad。 */
	WARN_ON(!pmd_bad(pmd_mkhuge(pmd)));
}

/*
 * pmd_advanced_tests() - 在真实 PMD 槽位上验证 THP 修改 helper。
 * 业务背景：debug_vm_pgtable() 持 pmd_lock 调用，覆盖写保护、访问位升级、young 清除及 deposited PTE 页协议。
 * 入参：args 为借用上下文；pmdp 受调用者持锁保护，pmd_pfn 指向已分配的大页或可访问连续页。
 * 出参/返回：无；每轮清空 PMD，末尾取回先前 deposit 的 PTE 页，失败只 WARN 或跳过。
 * 注意事项：本函数不释放页和页表；运行期 THP 不可用或大页未分配时安全跳过。
 */
static void __init pmd_advanced_tests(struct pgtable_debug_args *args)
{
	/* page 用于架构 cache 状态收尾；vaddr 对齐后才符合 PMD 叶映射范围。 */
	struct page *page;
	pmd_t pmd;
	unsigned long vaddr = args->vaddr;

	if (!has_transparent_hugepage())
		return;

	page = (args->pmd_pfn != ULONG_MAX) ? pfn_to_page(args->pmd_pfn) : NULL;
	if (!page)
		return;

	/*
	 * flush_dcache_page() is called after set_pmd_at() to clear
	 * PG_arch_1 for the page on ARM64. The page flag isn't cleared
	 * when it's released and page allocation check will fail when
	 * the page is allocated again. For architectures other than ARM64,
	 * the unexpected overhead of cache flushing is acceptable.
	 */
	/* set_pmd_at() 后清理 arm64 的 PG_arch_1，防止测试页归还后污染分配检查；其它架构仅多一次 flush。 */
	pr_debug("Validating PMD advanced\n");
	/* Align the address wrt HPAGE_PMD_SIZE */
	/* 将随机地址向下对齐到 PMD 大页边界，保证 helper 的地址参数与条目层级匹配。 */
	vaddr &= HPAGE_PMD_MASK;

	/* 先把 PTE 页 deposit 到 PMD，模拟 THP 拆分时可取回的下级页表资源。 */
	pgtable_trans_huge_deposit(args->mm, args->pmdp, args->start_ptep);

	/* 阶段一：安装 PMD 叶映射、写保护并取走，验证槽位最终为 none。 */
	pmd = pfn_pmd(args->pmd_pfn, args->page_prot);
	set_pmd_at(args->mm, vaddr, args->pmdp, pmd);
	flush_dcache_page(page);
	pmdp_set_wrprotect(args->mm, vaddr, args->pmdp);
	pmd = pmdp_get(args->pmdp);
	WARN_ON(pmd_write(pmd));
	pmdp_huge_get_and_clear(args->mm, vaddr, args->pmdp);
	pmd = pmdp_get(args->pmdp);
	WARN_ON(!pmd_none(pmd));

	/* 阶段二：从只读 clean 升级为可写 dirty，access-flags helper 必须真实更新槽位。 */
	pmd = pfn_pmd(args->pmd_pfn, args->page_prot);
	pmd = pmd_wrprotect(pmd);
	pmd = pmd_mkclean(pmd);
	set_pmd_at(args->mm, vaddr, args->pmdp, pmd);
	flush_dcache_page(page);
	pmd = pmd_mkwrite(pmd, args->vma);
	pmd = pmd_mkdirty(pmd);
	pmdp_set_access_flags(args->vma, vaddr, args->pmdp, pmd, 1);
	pmd = pmdp_get(args->pmdp);
	/* 回读验证升级已发布到槽位，再以 full clear 结束该子阶段。 */
	WARN_ON(!(pmd_write(pmd) && pmd_dirty(pmd)));
	pmdp_huge_get_and_clear_full(args->vma, vaddr, args->pmdp, 1);
	pmd = pmdp_get(args->pmdp);
	WARN_ON(!pmd_none(pmd));

	/* 阶段三：young 的 test-and-clear 必须对已标成 huge 的 PMD 生效。 */
	pmd = pmd_mkhuge(pfn_pmd(args->pmd_pfn, args->page_prot));
	pmd = pmd_mkyoung(pmd);
	set_pmd_at(args->mm, vaddr, args->pmdp, pmd);
	flush_dcache_page(page);
	pmdp_test_and_clear_young(args->vma, vaddr, args->pmdp);
	pmd = pmdp_get(args->pmdp);
	WARN_ON(pmd_young(pmd));

	/*  Clear the pte entries  */
	/* 清空最后一个 PMD 并取回 deposited PTE 页，使 destroy_args() 可以按原 ownership 释放它。 */
	pmdp_huge_get_and_clear(args->mm, vaddr, args->pmdp);
	pgtable_trans_huge_withdraw(args->mm, args->pmdp);
}

/*
 * pmd_leaf_tests() - 确认 THP PMD 被通用 walker 识别为叶条目。
 * 业务背景：页表遍历必须在 PMD 处停止，不能把大页物理地址误当成下级页表页。
 * 入参：args 为借用上下文，仅读取 fixed_pmd_pfn/page_prot。
 * 出参/返回：无；THP 不可用时跳过，否则契约违反触发 WARN。
 * 注意事项：纯值测试，不需锁、不访问映射内存、无 ownership 变化。
 */
static void __init pmd_leaf_tests(struct pgtable_debug_args *args)
{
	pmd_t pmd;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD leaf\n");
	pmd = pfn_pmd(args->fixed_pmd_pfn, args->page_prot);

	/*
	 * PMD based THP is a leaf entry.
	 */
	/* PMD 级透明大页直接指向数据页，必须设置 huge 后被 pmd_leaf() 判为叶子。 */
	pmd = pmd_mkhuge(pmd);
	WARN_ON(!pmd_leaf(pmd));
}

#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
/*
 * pud_basic_tests() - 验证 PUD 级透明大页的权限位转换。
 * 业务背景：在支持 PUD THP 的架构上补齐 PTE/PMD 同类契约，并处理下级折叠配置。
 * 入参：args 为借用上下文；idx 为 VM 权限组合，不转移任何资源。
 * 出参/返回：无；不支持 PUD THP 时跳过，断言失败只 WARN。
 * 注意事项：纯值测试无需锁；PMD 折叠时不执行依赖真实下一级的 bad 判定。
 */
static void __init pud_basic_tests(struct pgtable_debug_args *args, int idx)
{
	pgprot_t prot = vm_get_page_prot(idx);
	unsigned long val = idx, *ptr = &val;
	pud_t pud;

	if (!has_transparent_pud_hugepage())
		return;

	pr_debug("Validating PUD basic (%pGv)\n", ptr);
	pud = pfn_pud(args->fixed_pud_pfn, prot);

	/*
	 * This test needs to be executed after the given page table entry
	 * is created with pfn_pud() to make sure that vm_get_page_prot(idx)
	 * does not have the dirty bit enabled from the beginning. This is
	 * important for platforms like arm64 where (!PTE_RDONLY) indicate
	 * dirty bit being set.
	 */
	/* 先排除初始保护值自带 dirty，避免后续置位/清位测试产生假阳性。 */
	WARN_ON(pud_dirty(pud_wrprotect(pud)));

	WARN_ON(!pud_same(pud, pud));
	WARN_ON(!pud_young(pud_mkyoung(pud_mkold(pud))));
	WARN_ON(!pud_dirty(pud_mkdirty(pud_mkclean(pud))));
	WARN_ON(pud_dirty(pud_mkclean(pud_mkdirty(pud))));
	WARN_ON(!pud_write(pud_mkwrite(pud_wrprotect(pud))));
	WARN_ON(pud_write(pud_wrprotect(pud_mkwrite(pud))));
	WARN_ON(pud_young(pud_mkold(pud_mkyoung(pud))));
	WARN_ON(pud_dirty(pud_wrprotect(pud_mkclean(pud))));
	WARN_ON(!pud_dirty(pud_wrprotect(pud_mkdirty(pud))));

	/* 折叠 PMD 时没有独立的 PUD→PMD 非叶关系，后续 bad 判定不适用。 */
	if (mm_pmd_folded(args->mm))
		return;

	/*
	 * A huge page does not point to next level page table
	 * entry. Hence this must qualify as pud_bad().
	 */
	/* PUD 大页不指向 PMD 表，按非叶条目校验必须被视为 bad。 */
	WARN_ON(!pud_bad(pud_mkhuge(pud)));
}

/*
 * pud_advanced_tests() - 在真实 PUD 槽位上验证 PUD THP 修改原语。
 * 业务背景：由主测试在 pud_lock 下调用，检查写保护、access-flags、young 与清除语义。
 * 入参：args 为借用上下文；pudp 受调用者持锁保护，pud_pfn 指向已分配且可访问的大页。
 * 出参/返回：无；测试结束清空槽位，缺能力或缺页时跳过。
 * 注意事项：PMD 折叠配置跳过不适用的 get-and-clear 断言；页的释放仍由 destroy_args() 完成。
 */
static void __init pud_advanced_tests(struct pgtable_debug_args *args)
{
	struct page *page;
	unsigned long vaddr = args->vaddr;
	pud_t pud;

	if (!has_transparent_pud_hugepage())
		return;

	page = (args->pud_pfn != ULONG_MAX) ? pfn_to_page(args->pud_pfn) : NULL;
	if (!page)
		return;

	/*
	 * flush_dcache_page() is called after set_pud_at() to clear
	 * PG_arch_1 for the page on ARM64. The page flag isn't cleared
	 * when it's released and page allocation check will fail when
	 * the page is allocated again. For architectures other than ARM64,
	 * the unexpected overhead of cache flushing is acceptable.
	 */
	/* flush_dcache_page() 清理 arm64 测试页的 PG_arch_1，避免释放后触发分配器检查。 */
	pr_debug("Validating PUD advanced\n");
	/* Align the address wrt HPAGE_PUD_SIZE */
	/* PUD helper 的地址必须落在条目覆盖范围起点，故将随机地址向下对齐。 */
	vaddr &= HPAGE_PUD_MASK;

	/* 阶段一：安装条目并写保护；未折叠时再取走条目并确认 none。 */
	pud = pfn_pud(args->pud_pfn, args->page_prot);
	set_pud_at(args->mm, vaddr, args->pudp, pud);
	flush_dcache_page(page);
	pudp_set_wrprotect(args->mm, vaddr, args->pudp);
	pud = pudp_get(args->pudp);
	WARN_ON(pud_write(pud));

#ifndef __PAGETABLE_PMD_FOLDED
	/* 未折叠时取走 huge PUD 并确认槽位为空；折叠架构不具备该独立操作。 */
	pudp_huge_get_and_clear(args->mm, vaddr, args->pudp);
	pud = pudp_get(args->pudp);
	WARN_ON(!pud_none(pud));
#endif /* __PAGETABLE_PMD_FOLDED */
	/* 阶段二：把只读 clean 条目升级成可写 dirty，并从真实槽位回读验证。 */
	pud = pfn_pud(args->pud_pfn, args->page_prot);
	pud = pud_wrprotect(pud);
	pud = pud_mkclean(pud);
	set_pud_at(args->mm, vaddr, args->pudp, pud);
	flush_dcache_page(page);
	pud = pud_mkwrite(pud);
	pud = pud_mkdirty(pud);
	pudp_set_access_flags(args->vma, vaddr, args->pudp, pud, 1);
	pud = pudp_get(args->pudp);
	/* write/dirty 必须同时可见，随后 full clear 与生产 teardown 语义一致。 */
	WARN_ON(!(pud_write(pud) && pud_dirty(pud)));

#ifndef __PAGETABLE_PMD_FOLDED
	pudp_huge_get_and_clear_full(args->vma, vaddr, args->pudp, 1);
	pud = pudp_get(args->pudp);
	WARN_ON(!pud_none(pud));
#endif /* __PAGETABLE_PMD_FOLDED */

	/* 阶段三：设置 young 后由 test-and-clear 清除，最后清空 PUD 交还空槽。 */
	pud = pfn_pud(args->pud_pfn, args->page_prot);
	pud = pud_mkyoung(pud);
	set_pud_at(args->mm, vaddr, args->pudp, pud);
	flush_dcache_page(page);
	pudp_test_and_clear_young(args->vma, vaddr, args->pudp);
	pud = pudp_get(args->pudp);
	WARN_ON(pud_young(pud));

	pudp_huge_get_and_clear(args->mm, vaddr, args->pudp);
}

/*
 * pud_leaf_tests() - 确认 PUD 级 THP 是页表遍历的叶条目。
 * 业务背景：阻止 walker 将大页数据地址继续解释为 PMD 表。
 * 入参：args 为借用上下文，只读 fixed_pud_pfn/page_prot。
 * 出参/返回：无；能力缺失时跳过，错误通过 WARN 暴露。
 * 注意事项：纯值测试，无锁、不可睡眠路径、无副作用和 ownership 变化。
 */
static void __init pud_leaf_tests(struct pgtable_debug_args *args)
{
	pud_t pud;

	if (!has_transparent_pud_hugepage())
		return;

	pr_debug("Validating PUD leaf\n");
	pud = pfn_pud(args->fixed_pud_pfn, args->page_prot);
	/*
	 * PUD based THP is a leaf entry.
	 */
	/* PUD 大页直接映射数据页，设置 huge 后必须被 pud_leaf() 识别。 */
	pud = pud_mkhuge(pud);
	WARN_ON(!pud_leaf(pud));
}
#else  /* !CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
/* PUD THP 未实现时保留同一调用接口；args/idx 均未使用，无返回值、副作用、锁或 ownership 变化。 */
static void __init pud_basic_tests(struct pgtable_debug_args *args, int idx) { }
/* PUD THP 未实现时的高级测试空桩；借用 args 但不访问它，调用者可按统一流程继续。 */
static void __init pud_advanced_tests(struct pgtable_debug_args *args) { }
/* PUD THP 未实现时的叶判定空桩；无输入消费、返回值和可观察副作用。 */
static void __init pud_leaf_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
#else  /* !CONFIG_TRANSPARENT_HUGEPAGE */
/* 未配置 THP 时的 PMD 基础测试空桩；参数仅维持统一签名，不产生状态变化。 */
static void __init pmd_basic_tests(struct pgtable_debug_args *args, int idx) { }
/* 未配置 THP 时的 PUD 基础测试空桩；参数未使用，无锁、返回值或 ownership 变化。 */
static void __init pud_basic_tests(struct pgtable_debug_args *args, int idx) { }
/* 未配置 THP 时的 PMD 修改测试空桩；不访问借用的 args，也不要求页表锁。 */
static void __init pmd_advanced_tests(struct pgtable_debug_args *args) { }
/* 未配置 THP 时的 PUD 修改测试空桩；无返回值和副作用。 */
static void __init pud_advanced_tests(struct pgtable_debug_args *args) { }
/* 未配置 THP 时的 PMD 叶测试空桩；统一主调度路径而不改变状态。 */
static void __init pmd_leaf_tests(struct pgtable_debug_args *args) { }
/* 未配置 THP 时的 PUD 叶测试空桩；借用参数但不读取。 */
static void __init pud_leaf_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP
/*
 * pmd_huge_tests() - 验证 PMD 级 huge-vmap 的建立和清除协议。
 * 业务背景：体系结构 huge-vmap helper 必须拒绝不合适条目，并能把合法映射恢复为空。
 * 入参：args 为借用上下文；pmdp 是调用者持 pmd_lock 保护的槽，fixed_pmd_pfn 只提供物理地址。
 * 出参/返回：无；能力或对齐不足时跳过，失败以 WARN 报告，成功后槽位为 none。
 * 注意事项：不取得物理页 ownership，不负责锁；测试值不会被 CPU 实际访问。
 */
static void __init pmd_huge_tests(struct pgtable_debug_args *args)
{
	pmd_t pmd;

	if (!arch_vmap_pmd_supported(args->page_prot) ||
	    args->fixed_alignment < PMD_SIZE)
		return;

	pr_debug("Validating PMD huge\n");
	/*
	 * X86 defined pmd_set_huge() verifies that the given
	 * PMD is not a populated non-leaf entry.
	 */
	/* x86 的 pmd_set_huge() 还会验证目标不是已填充的非叶条目，所以先显式清空。 */
	pmd_clear(args->pmdp);
	WARN_ON(!pmd_set_huge(args->pmdp, __pfn_to_phys(args->fixed_pmd_pfn), args->page_prot));
	WARN_ON(!pmd_clear_huge(args->pmdp));
	pmd = pmdp_get(args->pmdp);
	WARN_ON(!pmd_none(pmd));
}

/*
 * pud_huge_tests() - 验证 PUD 级 huge-vmap 的建立和清除协议。
 * 业务背景：与 PMD 测试同属架构 huge-vmap 契约，覆盖更大映射粒度。
 * 入参：args 为借用上下文；pudp 在 pud_lock 下，物理地址必须达到 PUD 对齐。
 * 出参/返回：无；不支持或不对齐时跳过，最终槽位恢复 none。
 * 注意事项：不接管页或锁，所有异常通过 WARN 暴露。
 */
static void __init pud_huge_tests(struct pgtable_debug_args *args)
{
	pud_t pud;

	if (!arch_vmap_pud_supported(args->page_prot) ||
	    args->fixed_alignment < PUD_SIZE)
		return;

	pr_debug("Validating PUD huge\n");
	/*
	 * X86 defined pud_set_huge() verifies that the given
	 * PUD is not a populated non-leaf entry.
	 */
	/* x86 要求 huge 目标不是已有的下级表指针，先清空再验证 set/clear 对称性。 */
	pud_clear(args->pudp);
	WARN_ON(!pud_set_huge(args->pudp, __pfn_to_phys(args->fixed_pud_pfn), args->page_prot));
	WARN_ON(!pud_clear_huge(args->pudp));
	pud = pudp_get(args->pudp);
	WARN_ON(!pud_none(pud));
}
#else /* !CONFIG_HAVE_ARCH_HUGE_VMAP */
/* 架构无 huge-vmap PMD 支持时的空桩；不消费 args、无副作用。 */
static void __init pmd_huge_tests(struct pgtable_debug_args *args) { }
/* 架构无 huge-vmap PUD 支持时的空桩；维持统一测试调用表。 */
static void __init pud_huge_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_HAVE_ARCH_HUGE_VMAP */

/*
 * p4d_basic_tests() - 验证任意 P4D 值与自身比较保持相同。
 * 业务背景：高层目录没有权限派生测试，但 p4d_same() 仍须满足最基本的自反性。
 * 入参：args 仅为统一签名，函数不读取也不接管；出参/返回：无，失败触发 WARN。
 * 注意事项：局部纯值测试，无锁、无睡眠和外部状态变化。
 */
static void __init p4d_basic_tests(struct pgtable_debug_args *args)
{
	p4d_t p4d;

	pr_debug("Validating P4D basic\n");
	memset(&p4d, RANDOM_NZVALUE, sizeof(p4d_t));
	WARN_ON(!p4d_same(p4d, p4d));
}

/*
 * pgd_basic_tests() - 验证任意 PGD 值与自身比较保持相同。
 * 业务背景：为最顶层目录的 pgd_same() 提供架构一致性冒烟测试。
 * 入参：args 未使用且不转移所有权；出参/返回：无，仅 WARN。
 * 注意事项：纯局部值测试，不需锁且不会修改页表。
 */
static void __init pgd_basic_tests(struct pgtable_debug_args *args)
{
	pgd_t pgd;

	pr_debug("Validating PGD basic\n");
	memset(&pgd, RANDOM_NZVALUE, sizeof(pgd_t));
	WARN_ON(!pgd_same(pgd, pgd));
}

#ifndef __PAGETABLE_PUD_FOLDED
/*
 * pud_clear_tests() - 验证 pud_clear() 能把非空目录项恢复为 none。
 * 业务背景：主测试在 pud_lock 下调用，确保架构清除原语可供拆映射路径可靠使用。
 * 入参：args 为借用上下文且 pudp 已加锁；出参/返回：无，折叠下级时跳过，错误 WARN。
 * 注意事项：清除的是自测页表；随后 populate 测试会重新建立目录项。
 */
static void __init pud_clear_tests(struct pgtable_debug_args *args)
{
	/* pud 是锁保护下的入口快照；折叠 PMD 时没有独立清除语义。 */
	pud_t pud = pudp_get(args->pudp);

	if (mm_pmd_folded(args->mm))
		return;

	pr_debug("Validating PUD clear\n");
	WARN_ON(pud_none(pud));
	pud_clear(args->pudp);
	pud = pudp_get(args->pudp);
	WARN_ON(!pud_none(pud));
}

/*
 * pud_populate_tests() - 验证把 PMD 页表挂入 PUD 后不会被判为 bad。
 * 业务背景：它检查非叶目录项编码，与 huge 叶条目测试形成互补。
 * 入参：args 为借用上下文；start_pmdp 是已有下级表，pudp 由调用者持锁。
 * 出参/返回：无；成功后 PUD 指向该下级表，资源 ownership 仍由 args 保存。
 * 注意事项：PMD 层折叠时跳过，不分配或释放任何页表页。
 */
static void __init pud_populate_tests(struct pgtable_debug_args *args)
{
	pud_t pud;

	if (mm_pmd_folded(args->mm))
		return;

	pr_debug("Validating PUD populate\n");
	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as pud_bad().
	 */
	/* 指向下一级 PMD 表的 PUD 是合法非叶目录项，不能被 pud_bad() 拒绝。 */
	pud_populate(args->mm, args->pudp, args->start_pmdp);
	pud = pudp_get(args->pudp);
	WARN_ON(pud_bad(pud));
}
#else  /* !__PAGETABLE_PUD_FOLDED */
/* PUD 层折叠时清除操作没有独立槽位可测；空桩无副作用。 */
static void __init pud_clear_tests(struct pgtable_debug_args *args) { }
/* PUD 层折叠时 populate 由折叠 helper 吸收；空桩不访问 args。 */
static void __init pud_populate_tests(struct pgtable_debug_args *args) { }
#endif /* PAGETABLE_PUD_FOLDED */

#ifndef __PAGETABLE_P4D_FOLDED
/* p4d_clear_tests() 验证受 mm->page_table_lock 保护的非空 P4D 可清为 none；args 借用，无返回与资源转移。 */
static void __init p4d_clear_tests(struct pgtable_debug_args *args)
{
	/* p4d 是 page_table_lock 下的入口快照；PUD 折叠时跳过独立层测试。 */
	p4d_t p4d = p4dp_get(args->p4dp);

	if (mm_pud_folded(args->mm))
		return;

	pr_debug("Validating P4D clear\n");
	WARN_ON(p4d_none(p4d));
	p4d_clear(args->p4dp);
	p4d = p4dp_get(args->p4dp);
	WARN_ON(!p4d_none(p4d));
}

/*
 * p4d_populate_tests() - 把既有 PUD 表挂入 P4D 并验证非叶编码合法。
 * 业务背景：在未折叠的 P4D 层检查 populate/bad 契约；由主测试持 page_table_lock 调用。
 * 入参：args 借用，start_pudp 的 ownership 不变；出参/返回：无，成功后 p4dp 指向该表。
 * 注意事项：先清相关槽避免把旧层级状态混入断言，PUD 折叠时跳过。
 */
static void __init p4d_populate_tests(struct pgtable_debug_args *args)
{
	p4d_t p4d;

	if (mm_pud_folded(args->mm))
		return;

	pr_debug("Validating P4D populate\n");
	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as p4d_bad().
	 */
	/* 指向 PUD 表的 P4D 是合法目录项，不能被 p4d_bad() 当作损坏。 */
	pud_clear(args->pudp);
	p4d_clear(args->p4dp);
	p4d_populate(args->mm, args->p4dp, args->start_pudp);
	p4d = p4dp_get(args->p4dp);
	WARN_ON(p4d_bad(p4d));
}

/* pgd_clear_tests() 验证顶层目录项可在 page_table_lock 下清为 none；args 借用，无返回和 ownership 变化。 */
static void __init pgd_clear_tests(struct pgtable_debug_args *args)
{
	/* pgd 是顶层槽快照；P4D 折叠时目录清除已由折叠实现吸收。 */
	pgd_t pgd = pgdp_get(args->pgdp);

	if (mm_p4d_folded(args->mm))
		return;

	pr_debug("Validating PGD clear\n");
	WARN_ON(pgd_none(pgd));
	pgd_clear(args->pgdp);
	pgd = pgdp_get(args->pgdp);
	WARN_ON(!pgd_none(pgd));
}

/*
 * pgd_populate_tests() - 把既有 P4D 表挂入 PGD 并验证目录项合法。
 * 业务背景：覆盖最高层 populate/bad 契约；调用者持 mm->page_table_lock 排除并发修改。
 * 入参：args 借用，start_p4dp 仍归初始化上下文；出参/返回：无，成功后 PGD 指向它。
 * 注意事项：P4D 折叠时跳过，函数不分配或释放页表页。
 */
static void __init pgd_populate_tests(struct pgtable_debug_args *args)
{
	pgd_t pgd;

	if (mm_p4d_folded(args->mm))
		return;

	pr_debug("Validating PGD populate\n");
	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as pgd_bad().
	 */
	/* 指向 P4D 表的顶层项是合法非叶目录，pgd_bad() 必须返回 false。 */
	p4d_clear(args->p4dp);
	pgd_clear(args->pgdp);
	pgd_populate(args->mm, args->pgdp, args->start_p4dp);
	pgd = pgdp_get(args->pgdp);
	WARN_ON(pgd_bad(pgd));
}
#else  /* !__PAGETABLE_P4D_FOLDED */
/* P4D 折叠配置无独立 P4D 槽可清；空桩无副作用。 */
static void __init p4d_clear_tests(struct pgtable_debug_args *args) { }
/* P4D 折叠配置的 PGD 清除由其它层级覆盖；空桩不访问 args。 */
static void __init pgd_clear_tests(struct pgtable_debug_args *args) { }
/* P4D 折叠配置无需独立 populate；空桩保持统一主调用链。 */
static void __init p4d_populate_tests(struct pgtable_debug_args *args) { }
/* P4D 折叠配置无需独立 PGD populate；无返回值和状态变化。 */
static void __init pgd_populate_tests(struct pgtable_debug_args *args) { }
#endif /* PAGETABLE_P4D_FOLDED */

/*
 * pte_clear_tests() - 验证真实 PTE 槽可由 ptep_clear() 恢复为空。
 * 业务背景：由主测试在 PTE 锁下调用，覆盖拆映射依赖的最低层清除原语。
 * 入参：args 借用；ptep 已映射加锁，pte_pfn 指向已分配页。
 * 出参/返回：无；缺页或槽位时跳过，成功后 PTE 为 none，失败 WARN。
 * 注意事项：barrier 只阻止编译器重排测试步骤，不替代页表锁或 TLB flush。
 */
static void __init pte_clear_tests(struct pgtable_debug_args *args)
{
	struct page *page;
	pte_t pte = pfn_pte(args->pte_pfn, args->page_prot);

	page = (args->pte_pfn != ULONG_MAX) ? pfn_to_page(args->pte_pfn) : NULL;
	if (!page)
		return;

	/*
	 * flush_dcache_page() is called after set_pte_at() to clear
	 * PG_arch_1 for the page on ARM64. The page flag isn't cleared
	 * when it's released and page allocation check will fail when
	 * the page is allocated again. For architectures other than ARM64,
	 * the unexpected overhead of cache flushing is acceptable.
	 */
	/* set_pte_at() 后清理 arm64 的 PG_arch_1，避免页稍后释放时污染分配检查。 */
	pr_debug("Validating PTE clear\n");
	if (WARN_ON(!args->ptep))
		return;

	/* 安装非空条目、清理 cache 状态，再经编译器屏障后执行并回读 clear。 */
	set_pte_at(args->mm, args->vaddr, args->ptep, pte);
	WARN_ON(pte_none(pte));
	flush_dcache_page(page);
	barrier();
	ptep_clear(args->mm, args->vaddr, args->ptep);
	pte = ptep_get(args->ptep);
	WARN_ON(!pte_none(pte));
}

/* pmd_clear_tests() 在调用者持 pmd_lock 时验证非空 PMD 清为 none；args 借用，无返回或资源转移。 */
static void __init pmd_clear_tests(struct pgtable_debug_args *args)
{
	pmd_t pmd = pmdp_get(args->pmdp);

	pr_debug("Validating PMD clear\n");
	WARN_ON(pmd_none(pmd));
	pmd_clear(args->pmdp);
	pmd = pmdp_get(args->pmdp);
	WARN_ON(!pmd_none(pmd));
}

/*
 * pmd_populate_tests() - 把保存的 PTE 页挂回 PMD 并验证非叶编码合法。
 * 业务背景：与 PMD huge 叶测试互补，证明普通页表指针不会被 pmd_bad() 拒绝。
 * 入参：args 借用，start_ptep ownership 不变，pmdp 由调用者持锁。
 * 出参/返回：无；成功后 PMD 指向 PTE 表，供 destroy_args() 清除并释放。
 * 注意事项：不分配页表页，不改变计数；错误仅 WARN。
 */
static void __init pmd_populate_tests(struct pgtable_debug_args *args)
{
	pmd_t pmd;

	pr_debug("Validating PMD populate\n");
	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as pmd_bad().
	 */
	/* 指向最低级 PTE 表的 PMD 是合法非叶目录项。 */
	pmd_populate(args->mm, args->pmdp, args->start_ptep);
	pmd = pmdp_get(args->pmdp);
	WARN_ON(pmd_bad(pmd));
}

/* pte_special_tests() 验证架构 special 位可置位并识别；args 借用，纯值测试无锁、无返回和副作用。 */
static void __init pte_special_tests(struct pgtable_debug_args *args)
{
	pte_t pte = pfn_pte(args->fixed_pte_pfn, args->page_prot);

	if (!IS_ENABLED(CONFIG_ARCH_HAS_PTE_SPECIAL))
		return;

	pr_debug("Validating PTE special\n");
	WARN_ON(!pte_special(pte_mkspecial(pte)));
}

/*
 * pte_protnone_tests() - 验证 NUMA balancing 的 PROT_NONE PTE 仍按 present 语义编码。
 * 业务背景：自动 NUMA 采样要触发 fault，同时保留“页仍驻留”信息。
 * 入参：args 借用且只读固定 PFN/保护值；出参/返回：无，不支持时跳过，错误 WARN。
 * 注意事项：纯值测试，不安装映射、不持锁且不转移页 ownership。
 */
static void __init pte_protnone_tests(struct pgtable_debug_args *args)
{
	pte_t pte = pfn_pte(args->fixed_pte_pfn, args->page_prot_none);

	if (!IS_ENABLED(CONFIG_NUMA_BALANCING))
		return;

	pr_debug("Validating PTE protnone\n");
	WARN_ON(!pte_protnone(pte));
	WARN_ON(!pte_present(pte));
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/* pmd_protnone_tests() 对 THP PMD 验证 NUMA PROT_NONE 仍为 present；args 借用，纯值测试，仅 WARN/跳过。 */
static void __init pmd_protnone_tests(struct pgtable_debug_args *args)
{
	/* pmd 是局部编码；两个能力检查共同决定该语义是否存在。 */
	pmd_t pmd;

	if (!IS_ENABLED(CONFIG_NUMA_BALANCING))
		return;

	/* 只有同时具备 NUMA balancing 与运行期 THP，PMD PROT_NONE 编码才有意义。 */
	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD protnone\n");
	pmd = pmd_mkhuge(pfn_pmd(args->fixed_pmd_pfn, args->page_prot_none));
	WARN_ON(!pmd_protnone(pmd));
	WARN_ON(!pmd_present(pmd));
}
#else  /* !CONFIG_TRANSPARENT_HUGEPAGE */
/* 未配置 THP 时无 PMD PROT_NONE 叶条目可测；空桩无副作用。 */
static void __init pmd_protnone_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

/* pte_soft_dirty_tests() 验证普通 PTE soft-dirty 置位/清位互逆；args 借用，能力缺失时跳过，仅 WARN。 */
static void __init pte_soft_dirty_tests(struct pgtable_debug_args *args)
{
	pte_t pte = pfn_pte(args->fixed_pte_pfn, args->page_prot);

	if (!pgtable_supports_soft_dirty())
		return;

	pr_debug("Validating PTE soft dirty\n");
	WARN_ON(!pte_soft_dirty(pte_mksoft_dirty(pte)));
	WARN_ON(pte_soft_dirty(pte_clear_soft_dirty(pte)));
}

/*
 * pte_swap_soft_dirty_tests() - 验证 swap PTE 的 soft-dirty 位不破坏 swap payload。
 * 业务背景：非 present 编码要同时携带换出位置与用户态脏跟踪状态。
 * 入参：args 借用并只读 swp_entry；出参/返回：无，能力缺失时跳过，异常 WARN。
 * 注意事项：entry 是解码快照，无页引用、锁或 ownership 变化。
 */
static void __init pte_swap_soft_dirty_tests(struct pgtable_debug_args *args)
{
	/* pte 保存架构编码，entry 回读通用 softleaf 类型以确认 payload 仍是 swap。 */
	pte_t pte;
	softleaf_t entry;

	if (!pgtable_supports_soft_dirty())
		return;

	pr_debug("Validating PTE swap soft dirty\n");
	pte = swp_entry_to_pte(args->swp_entry);
	entry = softleaf_from_pte(pte);

	/* soft-dirty 置/清前后，通用解码仍必须把该 payload 识别为 swap。 */
	WARN_ON(!softleaf_is_swap(entry));
	WARN_ON(!pte_swp_soft_dirty(pte_swp_mksoft_dirty(pte)));
	WARN_ON(pte_swp_soft_dirty(pte_swp_clear_soft_dirty(pte)));
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/* pmd_soft_dirty_tests() 验证 THP PMD 的 soft-dirty 置/清互逆；args 借用，能力缺失时跳过，无资源转移。 */
static void __init pmd_soft_dirty_tests(struct pgtable_debug_args *args)
{
	/* 先验证编译期 soft-dirty，再验证运行期 THP，避免调用无意义 helper。 */
	pmd_t pmd;

	if (!pgtable_supports_soft_dirty())
		return;

	/* 编译期支持不等于机器启用 THP，两项都满足才构造 PMD 叶条目。 */
	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD soft dirty\n");
	pmd = pfn_pmd(args->fixed_pmd_pfn, args->page_prot);
	WARN_ON(!pmd_soft_dirty(pmd_mksoft_dirty(pmd)));
	WARN_ON(pmd_soft_dirty(pmd_clear_soft_dirty(pmd)));
}

/*
 * pmd_leaf_soft_dirty_tests() - 验证迁移 softleaf PMD 可独立携带 soft-dirty。
 * 业务背景：THP 迁移期间条目非 present，但仍须保存写跟踪且保持 huge/migration 类型可识别。
 * 入参：args 借用并只读 leaf_entry；出参/返回：无，配置或运行期能力不足时跳过。
 * 注意事项：纯编码测试，无锁、页引用或 ownership 变化，错误只 WARN。
 */
static void __init pmd_leaf_soft_dirty_tests(struct pgtable_debug_args *args)
{
	/* pmd 承接 migration softleaf 的架构编码，能力缺一即无该测试语义。 */
	pmd_t pmd;

	if (!pgtable_supports_soft_dirty() ||
	    !IS_ENABLED(CONFIG_ARCH_ENABLE_THP_MIGRATION))
		return;

	/* 运行期 THP 关闭时，即使架构能编码 migration PMD 也无需执行。 */
	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD swap soft dirty\n");
	pmd = swp_entry_to_pmd(args->leaf_entry);
	WARN_ON(!pmd_is_huge(pmd));
	WARN_ON(!pmd_is_valid_softleaf(pmd));

	/* 类型位通过后再验证 soft-dirty 专属位的置位和清位不改变 softleaf 合法性。 */
	WARN_ON(!pmd_swp_soft_dirty(pmd_swp_mksoft_dirty(pmd)));
	WARN_ON(pmd_swp_soft_dirty(pmd_swp_clear_soft_dirty(pmd)));
}
#else  /* !CONFIG_TRANSPARENT_HUGEPAGE */
/* 未配置 THP 时没有 PMD soft-dirty 叶条目；空桩不访问 args。 */
static void __init pmd_soft_dirty_tests(struct pgtable_debug_args *args) { }
/* 未配置 THP 时没有 PMD migration softleaf；空桩无返回与副作用。 */
static void __init pmd_leaf_soft_dirty_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

/*
 * pte_swap_exclusive_tests() - 验证 swap-exclusive 标志与 swap payload 相互独立。
 * 业务背景：匿名页换出条目用 exclusive 表达是否可原地复用，不能因此改变 type/offset 或 soft-dirty。
 * 入参：args 借用并只读 swp_entry；出参/返回：无，三阶段分别检查初始、置位和清位状态。
 * 注意事项：memcmp 比较 payload 位级保持一致；不访问 swap 后端、无锁和 ownership 变化。
 */
static void __init pte_swap_exclusive_tests(struct pgtable_debug_args *args)
{
	swp_entry_t entry;
	softleaf_t softleaf;
	pte_t pte;

	pr_debug("Validating PTE swap exclusive\n");
	entry = args->swp_entry;

	pte = swp_entry_to_pte(entry);
	softleaf = softleaf_from_pte(pte);

	/* 初始条目不是 exclusive，且 PTE 往返 softleaf 后仍等于原 swap payload。 */
	WARN_ON(pte_swp_exclusive(pte));
	WARN_ON(!softleaf_is_swap(softleaf));
	WARN_ON(memcmp(&entry, &softleaf, sizeof(entry)));

	pte = pte_swp_mkexclusive(pte);
	softleaf = softleaf_from_pte(pte);

	/* 置 exclusive 只能占用其专属标志位，不得伪造 soft-dirty 或改变 type/offset。 */
	WARN_ON(!pte_swp_exclusive(pte));
	WARN_ON(!softleaf_is_swap(softleaf));
	WARN_ON(pte_swp_soft_dirty(pte));
	WARN_ON(memcmp(&entry, &softleaf, sizeof(entry)));

	pte = pte_swp_clear_exclusive(pte);
	softleaf = softleaf_from_pte(pte);

	/* 清除后回到非 exclusive，payload 仍逐位保持原值。 */
	WARN_ON(pte_swp_exclusive(pte));
	WARN_ON(!softleaf_is_swap(softleaf));
	WARN_ON(memcmp(&entry, &softleaf, sizeof(entry)));
}

/*
 * pte_swap_tests() - 验证通用 swap entry 与架构 PTE 编码可无损往返。
 * 业务背景：换入、回收和 fault 路径依赖 type/offset 不被架构私有位布局破坏。
 * 入参：args 借用并只读 swp_entry；出参/返回：无，错误通过 WARN。
 * 注意事项：只比较编码，不读取 swap 设备，无锁、睡眠或引用变化。
 */
static void __init pte_swap_tests(struct pgtable_debug_args *args)
{
	/* arch_entry 是架构 payload 中间态，pte1/pte2 用逐位比较证明往返无损。 */
	swp_entry_t arch_entry;
	softleaf_t entry;
	pte_t pte1, pte2;

	pr_debug("Validating PTE swap\n");
	pte1 = swp_entry_to_pte(args->swp_entry);
	entry = softleaf_from_pte(pte1);

	/* 先确认通用层仍识别 swap，再经架构专用转换往返并比较全部位。 */
	WARN_ON(!softleaf_is_swap(entry));

	arch_entry = __pte_to_swp_entry(pte1);
	pte2 = __swp_entry_to_pte(arch_entry);
	WARN_ON(memcmp(&pte1, &pte2, sizeof(pte1)));
}

#ifdef CONFIG_ARCH_ENABLE_THP_MIGRATION
/*
 * pmd_softleaf_tests() - 验证 THP migration entry 与架构 PMD 编码无损往返。
 * 业务背景：迁移 fault 必须从非 present PMD 恢复完整 softleaf payload。
 * 入参：args 借用并只读 leaf_entry；出参/返回：无，THP 不可用时跳过。
 * 注意事项：纯值测试，无页引用或锁，类型/huge 属性和逐位值均由 WARN 检查。
 */
static void __init pmd_softleaf_tests(struct pgtable_debug_args *args)
{
	/* arch_entry 承接 PMD payload；两个 PMD 快照用于位级往返比较。 */
	swp_entry_t arch_entry;
	pmd_t pmd1, pmd2;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD swap\n");
	pmd1 = swp_entry_to_pmd(args->leaf_entry);
	WARN_ON(!pmd_is_huge(pmd1));
	WARN_ON(!pmd_is_valid_softleaf(pmd1));

	/* huge/softleaf 分类通过后，架构 entry 往返必须不丢任何标志或 offset。 */
	arch_entry = __pmd_to_swp_entry(pmd1);
	pmd2 = __swp_entry_to_pmd(arch_entry);
	WARN_ON(memcmp(&pmd1, &pmd2, sizeof(pmd1)));
}
#else  /* !CONFIG_ARCH_ENABLE_THP_MIGRATION */
/* 架构不支持 THP migration 时的 PMD softleaf 空桩；无副作用。 */
static void __init pmd_softleaf_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_ARCH_ENABLE_THP_MIGRATION */

/*
 * swap_migration_tests() - 验证普通页 migration entry 的读写类型转换。
 * 业务背景：迁移期间用非 present softleaf 暂代 PTE，fault 必须区分可写与只读迁移状态。
 * 入参：args 借用；pte_pfn 必须指向本测试独占分配的页。
 * 出参/返回：无；未配置迁移或缺页时跳过，末尾恢复 PageLocked 状态。
 * 注意事项：临时伪造页锁只因 helper 有锁断言，绝不能对映射内核文本的固定 PFN 使用。
 */
static void __init swap_migration_tests(struct pgtable_debug_args *args)
{
	struct page *page;
	softleaf_t entry;

	if (!IS_ENABLED(CONFIG_MIGRATION))
		return;

	/*
	 * swap_migration_tests() requires a dedicated page as it needs to
	 * be locked before creating a migration entry from it. Locking the
	 * page that actually maps kernel text ('start_kernel') can be real
	 * problematic. Lets use the allocated page explicitly for this
	 * purpose.
	 */
	/* migration helper 需要锁页；固定 PFN 可能指向内核文本，故只能使用自测实际分配的 page。 */
	page = (args->pte_pfn != ULONG_MAX) ? pfn_to_page(args->pte_pfn) : NULL;
	if (!page)
		return;

	pr_debug("Validating swap migration\n");

	/*
	 * make_[readable|writable]_migration_entry() expects given page to
	 * be locked, otherwise it stumbles upon a BUG_ON().
	 */
	/* helper 会 BUG_ON 未锁页；测试独占该页，可直接设置标志并在所有断言后清回。 */
	__SetPageLocked(page);
	entry = make_writable_migration_entry(page_to_pfn(page));
	WARN_ON(!softleaf_is_migration(entry));
	WARN_ON(!softleaf_is_migration_write(entry));

	/* 用前一条目的 offset 改造成只读迁移项，类型保留而 write 分类必须清除。 */
	entry = make_readable_migration_entry(swp_offset(entry));
	WARN_ON(!softleaf_is_migration(entry));
	WARN_ON(softleaf_is_migration_write(entry));

	entry = make_readable_migration_entry(page_to_pfn(page));
	WARN_ON(!softleaf_is_migration(entry));
	WARN_ON(softleaf_is_migration_write(entry));
	__ClearPageLocked(page);
}

#ifdef CONFIG_HUGETLB_PAGE
/*
 * hugetlb_basic_tests() - 验证 HugeTLB PTE 的 huge/dirty/write 基础属性。
 * 业务背景：显式大页使用架构转换后的 PTE，但仍须满足通用权限 helper 契约。
 * 入参：args 借用并只读固定 PFN/保护值；出参/返回：无，仅 WARN。
 * 注意事项：纯值测试，不创建 hugetlb folio、不持锁且无 ownership 变化。
 */
static void __init hugetlb_basic_tests(struct pgtable_debug_args *args)
{
	/* pte 从 PMD 对齐固定 PFN 构造，再由架构 helper 转成显式大页格式。 */
	pte_t pte;

	pr_debug("Validating HugeTLB basic\n");
	pte = pfn_pte(args->fixed_pmd_pfn, args->page_prot);
	pte = arch_make_huge_pte(pte, PMD_SHIFT, VM_ACCESS_FLAGS);

#ifdef CONFIG_ARCH_WANT_GENERAL_HUGETLB
	/* 采用通用 HugeTLB 接口的架构还必须显式报告 huge 分类。 */
	WARN_ON(!pte_huge(pte));
#endif
	WARN_ON(!huge_pte_dirty(huge_pte_mkdirty(pte)));
	WARN_ON(!huge_pte_write(huge_pte_mkwrite(huge_pte_wrprotect(pte))));
	WARN_ON(huge_pte_write(huge_pte_wrprotect(huge_pte_mkwrite(pte))));
}
#else  /* !CONFIG_HUGETLB_PAGE */
/* 未配置 HugeTLB 时的基础测试空桩；不访问 args、无副作用。 */
static void __init hugetlb_basic_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_HUGETLB_PAGE */

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/*
 * pmd_thp_tests() - 验证 THP PMD 在失效过渡期仍保持 huge/present/leaf 分类。
 * 业务背景：拆分会短暂清 present；walker 必须仍识别 THP，避免误走非 THP 路径或多取 pmd_lock。
 * 入参：args 借用并只读固定 PFN/保护值；出参/返回：无，不支持 THP 时跳过。
 * 注意事项：纯值测试；架构自定义 invalidate 时跳过通用 mkinvalid 断言。
 */
static void __init pmd_thp_tests(struct pgtable_debug_args *args)
{
	pmd_t pmd;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD based THP\n");
	/*
	 * pmd_trans_huge() and pmd_present() must return positive after
	 * MMU invalidation with pmd_mkinvalid(). This behavior is an
	 * optimization for transparent huge page. pmd_trans_huge() must
	 * be true if pmd_page() returns a valid THP to avoid taking the
	 * pmd_lock when others walk over non transhuge pmds (i.e. there
	 * are no THP allocated). Especially when splitting a THP and
	 * removing the present bit from the pmd, pmd_trans_huge() still
	 * needs to return true. pmd_present() should be true whenever
	 * pmd_trans_huge() returns true.
	 */
	/*
	 * THP 拆分临时清 present 后，pmd_trans_huge() 仍须为真，否则 walker 会把它当普通目录项；
	 * 通用实现还要求 present/leaf 与该分类一致，架构自定义 invalidate 则由架构自行保证。
	 */
	pmd = pfn_pmd(args->fixed_pmd_pfn, args->page_prot);
	WARN_ON(!pmd_trans_huge(pmd_mkhuge(pmd)));

#ifndef __HAVE_ARCH_PMDP_INVALIDATE
	WARN_ON(!pmd_trans_huge(pmd_mkinvalid(pmd_mkhuge(pmd))));
	WARN_ON(!pmd_present(pmd_mkinvalid(pmd_mkhuge(pmd))));
	WARN_ON(!pmd_leaf(pmd_mkinvalid(pmd_mkhuge(pmd))));
#endif /* __HAVE_ARCH_PMDP_INVALIDATE */
}

#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
/* pud_thp_tests() 验证 PUD THP 的 huge 分类；args 借用，能力缺失时跳过，纯值测试仅 WARN。 */
static void __init pud_thp_tests(struct pgtable_debug_args *args)
{
	pud_t pud;

	if (!has_transparent_pud_hugepage())
		return;

	pr_debug("Validating PUD based THP\n");
	pud = pfn_pud(args->fixed_pud_pfn, args->page_prot);
	WARN_ON(!pud_trans_huge(pud_mkhuge(pud)));

	/*
	 * pud_mkinvalid() has been dropped for now. Enable back
	 * these tests when it comes back with a modified pud_present().
	 *
	 * WARN_ON(!pud_trans_huge(pud_mkinvalid(pud_mkhuge(pud))));
	 * WARN_ON(!pud_present(pud_mkinvalid(pud_mkhuge(pud))));
	 */
	/* pud_mkinvalid() 当前已移除；保留注释中的断言，待 API 以配套 present 语义恢复后再启用。 */
}
#else  /* !CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
/* 架构无 PUD THP 时的分类测试空桩；无状态变化。 */
static void __init pud_thp_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
#else  /* !CONFIG_TRANSPARENT_HUGEPAGE */
/* 未配置 THP 时无 PMD THP 分类可测；空桩无副作用。 */
static void __init pmd_thp_tests(struct pgtable_debug_args *args) { }
/* 未配置 THP 时无 PUD THP 分类可测；空桩不访问 args。 */
static void __init pud_thp_tests(struct pgtable_debug_args *args) { }
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

/*
 * get_random_vaddr() - 选择一个页对齐的随机用户虚拟地址供自测建表。
 * 业务背景：测试不映射进程真实 VMA，但需要合法用户地址驱动各级 offset helper。
 * 入参：无；出参/返回：FIRST_USER_ADDRESS..TASK_SIZE 范围内的页起始地址。
 * 注意事项：仅作地址散列，不承诺密码学随机；不保留地址空间或取得任何引用。
 */
static unsigned long __init get_random_vaddr(void)
{
	unsigned long random_vaddr, random_pages, total_user_pages;

	total_user_pages = (TASK_SIZE - FIRST_USER_ADDRESS) / PAGE_SIZE;

	random_pages = get_random_long() % total_user_pages;
	random_vaddr = FIRST_USER_ADDRESS + random_pages * PAGE_SIZE;

	return random_vaddr;
}

/*
 * debug_vm_pgtable_free_huge_page() - 按分配来源归还自测大页。
 * 业务背景：高阶超过 buddy 上限时 init 可能改用 CMA 连续分配，释放接口必须与来源配对。
 * 入参：args 借用且提供来源标志；pfn/order 描述首页和 2^order 页，ownership 在此被消费。
 * 出参/返回：无；连续页走 free_contig_range，否则走 __free_pages。
 * 注意事项：调用者保证 PFN/order 与成功分配完全一致且仅释放一次；无并发共享。
 */
static void __init
debug_vm_pgtable_free_huge_page(struct pgtable_debug_args *args,
		unsigned long pfn, int order)
{
#ifdef CONFIG_CONTIG_ALLOC
	/* CMA/contig 来源必须按页数释放，不能交给只识别 buddy order 的 __free_pages()。 */
	if (args->is_contiguous_page) {
		free_contig_range(pfn, 1 << order);
		return;
	}
#endif
	__free_pages(pfn_to_page(pfn), order);
}

/*
 * destroy_args() - 逆序释放 init_args() 已取得的全部测试资源。
 * 业务背景：既服务初始化失败回滚，也服务完整自测结束，允许任意部分初始化状态。
 * 入参：args 为可修改的 owner 上下文；函数消费其中大页、普通页、页表页、VMA 和 mm ownership。
 * 出参/返回：无；PFN 哨兵被恢复，页表计数配对递减，调用后资源指针不得再使用。
 * 注意事项：启动期单线程且调用者未持页表锁；按 PUD 大页→PMD 大页→普通页避免别名重复释放。
 */
static void __init destroy_args(struct pgtable_debug_args *args)
{
	/* Free (huge) page */
	/* 若 PUD 大页成功，它同时供 PMD/PTE 测试复用；只释放一次并一次性失效三个 PFN。 */
	if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) &&
	    has_transparent_pud_hugepage() &&
	    args->pud_pfn != ULONG_MAX) {
		debug_vm_pgtable_free_huge_page(args, args->pud_pfn, HPAGE_PUD_ORDER);
		args->pud_pfn = ULONG_MAX;
		args->pmd_pfn = ULONG_MAX;
		args->pte_pfn = ULONG_MAX;
	}

	/* 未取得 PUD 大页时，PMD 大页可能同时作为 PTE backing；同样只释放一次。 */
	if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) &&
	    has_transparent_hugepage() &&
	    args->pmd_pfn != ULONG_MAX) {
		debug_vm_pgtable_free_huge_page(args, args->pmd_pfn, HPAGE_PMD_ORDER);
		args->pmd_pfn = ULONG_MAX;
		args->pte_pfn = ULONG_MAX;
	}

	if (args->pte_pfn != ULONG_MAX) {
		/* 前两级均未分配成功时，pte_pfn 独占一个 order-0 页。 */
		__free_page(pfn_to_page(args->pte_pfn));

		args->pte_pfn = ULONG_MAX;
	}

	/* Free page table entries */
	/* 从最低级向上清父项并释放页表页，且与分配路径增加的 mm 计数逐级配对。 */
	if (args->start_ptep) {
		pmd_clear(args->pmdp);
		pte_free(args->mm, args->start_ptep);
		mm_dec_nr_ptes(args->mm);
	}

	/* 每一级只在对应 start 指针存在时释放，因此同一路径也适用于部分初始化回滚。 */
	if (args->start_pmdp) {
		pud_clear(args->pudp);
		pmd_free(args->mm, args->start_pmdp);
		mm_dec_nr_pmds(args->mm);
	}

	if (args->start_pudp) {
		p4d_clear(args->p4dp);
		pud_free(args->mm, args->start_pudp);
		mm_dec_nr_puds(args->mm);
	}

	/* P4D 没有独立 mm 计数；清 PGD 链接后由架构 p4d_free() 处理折叠差异。 */
	if (args->start_p4dp) {
		pgd_clear(args->pgdp);
		p4d_free(args->mm, args->start_p4dp);
	}

	/* Free vma and mm struct */
	/* VMA 借用 mm；先释放 VMA，最后 mmput() 消费 init_args() 建立的 mm 引用。 */
	if (args->vma)
		vm_area_free(args->vma);

	if (args->mm)
		mmput(args->mm);
}

/*
 * debug_vm_pgtable_alloc_huge_page() - 为需要真实访问的测试申请指定 order 连续页。
 * 业务背景：PUD order 可能超过 buddy 上限，配置允许时先尝试 CMA 连续页，再回退 buddy。
 * 入参：args 为可修改上下文，order 为以页为单位的二进制阶；不接受输出指针。
 * 出参/返回：成功返回由调用者拥有的首页，失败 NULL；连续来源会设置 is_contiguous_page。
 * 注意事项：返回页必须由 debug_vm_pgtable_free_huge_page() 按同一 order 释放，可睡眠。
 */
static struct page * __init
debug_vm_pgtable_alloc_huge_page(struct pgtable_debug_args *args, int order)
{
	struct page *page = NULL;

#ifdef CONFIG_CONTIG_ALLOC
	/* buddy 无法表示过高 order 时，向任意在线节点申请完全连续的页范围。 */
	if (order > MAX_PAGE_ORDER) {
		page = alloc_contig_pages((1 << order), GFP_KERNEL,
					  first_online_node, NULL);
		if (page) {
			args->is_contiguous_page = true;
			return page;
		}
	}
#endif

	/* buddy 能表达的 order 使用普通高阶分配；失败由上层降级到较小测试页。 */
	if (order <= MAX_PAGE_ORDER)
		page = alloc_pages(GFP_KERNEL, order);

	return page;
}

/*
 * Check if a physical memory range described by <pstart, pend> contains
 * an area that is of size psize, and aligned to psize.
 *
 * Don't use address 0, an all-zeroes physical address might mask bugs, and
 * it's not used on x86.
 */
/*
 * 检查半开物理范围 [pstart, pend) 是否容得下一个大小和起点均按 psize 对齐的区域。
 * 物理地址 0 被主动跳过：全零编码可能掩盖页表 helper 的位操作缺陷，x86 也不使用该地址。
 */
/*
 * phys_align_check() - 尝试从物理范围选出一个指定粒度的对齐区域。
 * 入参：pstart/pend 为半开物理边界，psize 为大小兼对齐；physp/alignp 是成功时更新的输出参数。
 * 出参/返回：无直接返回；成功写出 aligned_start/psize，失败保持调用者旧值以便继续降级搜索。
 * 注意事项：不预留或访问内存，输出 ownership 不变；溢出通过 aligned_end > aligned_start 排除。
 */
static void  __init phys_align_check(phys_addr_t pstart,
				     phys_addr_t pend, unsigned long psize,
				     phys_addr_t *physp, unsigned long *alignp)
{
	/* aligned_start 是首个候选，aligned_end 同时用于容量检查和加法溢出检测。 */
	phys_addr_t aligned_start, aligned_end;

	if (pstart == 0)
		pstart = PAGE_SIZE;

	aligned_start = ALIGN(pstart, psize);
	aligned_end = aligned_start + psize;

	if (aligned_end > aligned_start && aligned_end <= pend) {
		*alignp = psize;
		*physp = aligned_start;
	}
}

/*
 * init_fixed_pfns() - 选择有效物理范围并派生各页表层级的固定 PFN。
 * 业务背景：纯编码测试需要真实有效且尽量大粒度对齐的 PFN，但无需拥有或访问对应内存。
 * 入参：args 为可修改 owner 上下文；出参/返回：无，填充 fixed_alignment 与五级 fixed_*_pfn。
 * 注意事项：优先 PUD 对齐、其次 PMD、最后 start_kernel；仅借用 memblock 视图，不分配内存。
 */
static void __init init_fixed_pfns(struct pgtable_debug_args *args)
{
	u64 idx;
	phys_addr_t phys, pstart, pend;

	/*
	 * Initialize the fixed pfns. To do this, try to find a
	 * valid physical range, preferably aligned to PUD_SIZE,
	 * but settling for aligned to PMD_SIZE as a fallback. If
	 * neither of those is found, use the physical address of
	 * the start_kernel symbol.
	 *
	 * The memory doesn't need to be allocated, it just needs to exist
	 * as usable memory. It won't be touched.
	 *
	 * The alignment is recorded, and can be checked to see if we
	 * can run the tests that require an actual valid physical
	 * address range on some architectures ({pmd,pud}_huge_test
	 * on x86).
	 */
	/*
	 * 在可用物理范围中优先找完整 PUD 对齐区，找不到则保留 PMD 对齐候选，再退回
	 * start_kernel 所在页；这些地址只编码进条目，测试不会触碰对应内存。
	 */

	phys = __pa_symbol(&start_kernel);
	args->fixed_alignment = PAGE_SIZE;

	for_each_mem_range(idx, &pstart, &pend) {
		/* First check for a PUD-aligned area */
		/* 每段先尝试最高粒度，成功后无需继续扫描。 */
		phys_align_check(pstart, pend, PUD_SIZE, &phys,
				 &args->fixed_alignment);

		/* If a PUD-aligned area is found, we're done */
		/* PUD 对齐同时满足所有较低层级需求。 */
		if (args->fixed_alignment == PUD_SIZE)
			break;

		/*
		 * If no PMD-aligned area found yet, check for one,
		 * but continue the loop to look for a PUD-aligned area.
		 */
		/* 尚无 PMD 候选才记录当前段，但继续扫描以争取后续 PUD 对齐区。 */
		if (args->fixed_alignment < PMD_SIZE)
			phys_align_check(pstart, pend, PMD_SIZE, &phys,
					 &args->fixed_alignment);
	}

	/* 对同一物理基址按各层覆盖范围向下对齐，形成对应条目可编码的 PFN。 */
	args->fixed_pgd_pfn = __phys_to_pfn(phys & PGDIR_MASK);
	args->fixed_p4d_pfn = __phys_to_pfn(phys & P4D_MASK);
	args->fixed_pud_pfn = __phys_to_pfn(phys & PUD_MASK);
	args->fixed_pmd_pfn = __phys_to_pfn(phys & PMD_MASK);
	args->fixed_pte_pfn = __phys_to_pfn(phys & PAGE_MASK);
	WARN_ON(!pfn_valid(args->fixed_pte_pfn));
}


/*
 * init_args() - 构造隔离的 mm/VMA/页表与测试页上下文。
 * 业务背景：所有架构 helper 必须在不污染 current->mm 的沙箱中测试，并允许大页分配失败后降级。
 * 入参：args 为纯输出 owner；调用前无需初始化，成功后持有全部资源，失败时本函数已回滚。
 * 出参/返回：0 表示基础上下文可用（真实测试页仍可能缺失而由各测试跳过），-ENOMEM 表示核心对象失败。
 * 注意事项：进程上下文可睡眠；资源按 mm→VMA→逐级页表→可选大页取得，error 统一逆序释放。
 */
static int __init init_args(struct pgtable_debug_args *args)
{
	/* max_swap_offset 构造最宽 payload；page 在 PUD→PMD→PTE 三档分配中临时承接 ownership。 */
	unsigned long max_swap_offset;
	struct page *page = NULL;
	int ret = 0;

	/*
	 * Initialize the debugging data.
	 *
	 * vm_get_page_prot(VM_NONE) or vm_get_page_prot(VM_SHARED|VM_NONE)
	 * will help create page table entries with PROT_NONE permission as
	 * required for pxx_protnone_tests().
	 */
	/* 清零后建立随机地址、普通/PROT_NONE 保护值及所有 PFN 未取得哨兵。 */
	memset(args, 0, sizeof(*args));
	args->vaddr              = get_random_vaddr();
	args->page_prot          = vm_get_page_prot(VM_ACCESS_FLAGS);
	args->page_prot_none     = vm_get_page_prot(VM_NONE);
	args->is_contiguous_page = false;
	args->pud_pfn            = ULONG_MAX;
	args->pmd_pfn            = ULONG_MAX;
	args->pte_pfn            = ULONG_MAX;
	/* fixed PFN 也先设哨兵，随后 init_fixed_pfns() 才一次性发布有效编码值。 */
	args->fixed_pgd_pfn      = ULONG_MAX;
	args->fixed_p4d_pfn      = ULONG_MAX;
	args->fixed_pud_pfn      = ULONG_MAX;
	args->fixed_pmd_pfn      = ULONG_MAX;
	args->fixed_pte_pfn      = ULONG_MAX;
	/* 至此所有可能被 destroy_args() 读取的字段都处于明确的“未取得”状态。 */

	/* Allocate mm and vma */
	/* mm 是根 owner；VMA 绑定它但尚未插入任何进程地址空间。 */
	args->mm = mm_alloc();
	if (!args->mm) {
		pr_err("Failed to allocate mm struct\n");
		ret = -ENOMEM;
		goto error;
	}

	/* VMA 分配失败仍由统一 error 路径释放已经拥有的 mm。 */
	args->vma = vm_area_alloc(args->mm);
	if (!args->vma) {
		pr_err("Failed to allocate vma\n");
		ret = -ENOMEM;
		goto error;
	}

	/*
	 * Allocate page table entries. They will be modified in the tests.
	 * Lets save the page table entries so that they can be released
	 * when the tests are completed.
	 */
	/*
	 * 沿随机 vaddr 自顶向下分配页表；start_* 保存各页表页基址，而 *dp 保存该地址的槽位。
	 * 任一级失败都跳到 error，由 destroy_args() 按已非空字段判断并逆序回收。
	 */
	args->pgdp = pgd_offset(args->mm, args->vaddr);
	args->p4dp = p4d_alloc(args->mm, args->pgdp, args->vaddr);
	if (!args->p4dp) {
		pr_err("Failed to allocate p4d entries\n");
		ret = -ENOMEM;
		goto error;
	}
	args->start_p4dp = p4d_offset(args->pgdp, 0UL);
	WARN_ON(!args->start_p4dp);

	/* 成功分配下一层后立即保存基址，使后续任一点失败都能准确回滚。 */
	args->pudp = pud_alloc(args->mm, args->p4dp, args->vaddr);
	if (!args->pudp) {
		pr_err("Failed to allocate pud entries\n");
		ret = -ENOMEM;
		goto error;
	}
	args->start_pudp = pud_offset(args->p4dp, 0UL);
	WARN_ON(!args->start_pudp);

	/* PMD 分配沿用同一事务模式：先取得，再发布对应 start 指针。 */
	args->pmdp = pmd_alloc(args->mm, args->pudp, args->vaddr);
	if (!args->pmdp) {
		pr_err("Failed to allocate pmd entries\n");
		ret = -ENOMEM;
		goto error;
	}
	args->start_pmdp = pmd_offset(args->pudp, 0UL);
	WARN_ON(!args->start_pmdp);

	/* 最低级 PTE 页由 pte_alloc() 建立，再从 PMD 中恢复其释放句柄。 */
	if (pte_alloc(args->mm, args->pmdp)) {
		pr_err("Failed to allocate pte entries\n");
		ret = -ENOMEM;
		goto error;
	}
	args->start_ptep = pmd_pgtable(pmdp_get(args->pmdp));
	WARN_ON(!args->start_ptep);

	/* 固定 PFN 只用于编码，和下面真正持有的可访问测试页是两套不同资源。 */
	init_fixed_pfns(args);

	/* See generic_max_swapfile_size(): probe the maximum offset */
	/* 仿照 generic_max_swapfile_size() 往返全 1 payload，探测架构实际保留的最大 offset。 */
	max_swap_offset = swp_offset(softleaf_from_pte(softleaf_to_pte(swp_entry(0, ~0UL))));
	/* Create a swp entry with all possible bits set while still being swap. */
	/* 使用最大 type/offset 压测编码边界，同时保持条目仍是合法 swap 类型。 */
	args->swp_entry = swp_entry(MAX_SWAPFILES - 1, max_swap_offset);
	/* Create a non-present migration entry. */
	/* leaf_entry 覆盖 THP migration softleaf 的非 present 编码路径。 */
	args->leaf_entry = make_writable_migration_entry(~0UL);

	/*
	 * Allocate (huge) pages because some of the tests need to access
	 * the data in the pages. The corresponding tests will be skipped
	 * if we fail to allocate (huge) pages.
	 */
	/*
	 * 优先申请 PUD 大页并让三个层级复用同一首页；失败降级 PMD，再失败降级单页。
	 * 这些分配失败不影响纯编码测试，因此仍返回 0，由需要真实 page 的测试自行跳过。
	 */
	if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) &&
	    has_transparent_pud_hugepage()) {
		page = debug_vm_pgtable_alloc_huge_page(args, HPAGE_PUD_ORDER);
		if (page) {
			args->pud_pfn = page_to_pfn(page);
			args->pmd_pfn = args->pud_pfn;
			args->pte_pfn = args->pud_pfn;
			return 0;
		}
	}

	/* PUD 档失败后尝试成本较低的 PMD 大页，并同时供 PTE 测试复用。 */
	if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) &&
	    has_transparent_hugepage()) {
		page = debug_vm_pgtable_alloc_huge_page(args, HPAGE_PMD_ORDER);
		if (page) {
			args->pmd_pfn = page_to_pfn(page);
			args->pte_pfn = args->pmd_pfn;
			return 0;
		}
	}

	/* 最后只申请 order-0 页；即使失败，固定 PFN 与纯值测试仍可执行。 */
	page = alloc_page(GFP_KERNEL);
	if (page)
		args->pte_pfn = page_to_pfn(page);

	return 0;

error:
	/* 核心结构失败不可继续；destroy_args() 能识别并清理由前序阶段取得的子集。 */
	destroy_args(args);
	return ret;
}

/*
 * debug_vm_pgtable() - 编排一次完整的架构页表 helper 启动自测。
 * 业务背景：late_initcall 在内存管理已可分配、系统正式运行前集中发现架构 helper 语义偏差。
 * 入参：无；出参/返回：0 表示编排完成（单项失败由 WARN 表达），负 errno 仅表示初始化失败。
 * 注意事项：先做无锁纯值测试，再按 PTE→PMD→PUD→顶层锁层级做修改测试，最后必定销毁沙箱。
 */
static int __init debug_vm_pgtable(void)
{
	/* args 独占所有测试资源；ptl 接收各级动态锁，idx 遍历权限组合，ret 传播初始化 errno。 */
	struct pgtable_debug_args args;
	spinlock_t *ptl = NULL;
	int idx, ret;

	pr_info("Validating architecture page table helpers\n");
	ret = init_args(&args);
	if (ret)
		return ret;

	/*
	 * Iterate over each possible vm_flags to make sure that all
	 * the basic page table transformation validations just hold
	 * true irrespective of the starting protection value for a
	 * given page table entry.
	 *
	 * Protection based vm_flags combinations are always linear
	 * and increasing i.e starting from VM_NONE and going up to
	 * (VM_SHARED | READ | WRITE | EXEC).
	 */
	/* 遍历线性 VM 权限位组合，让 PTE/PMD/PUD 基础变换不依赖某个幸运初始保护值。 */
#define VM_FLAGS_START	(VM_NONE)
#define VM_FLAGS_END	(VM_SHARED | VM_EXEC | VM_WRITE | VM_READ)

	for (idx = VM_FLAGS_START; idx <= VM_FLAGS_END; idx++) {
		pte_basic_tests(&args, idx);
		pmd_basic_tests(&args, idx);
		pud_basic_tests(&args, idx);
	}

	/*
	 * Both P4D and PGD level tests are very basic which do not
	 * involve creating page table entries from the protection
	 * value and the given pfn. Hence just keep them out from
	 * the above iteration for now to save some test execution
	 * time.
	 */
	/* P4D/PGD 不从权限和 PFN 构造条目，移出循环可避免重复无意义断言。 */
	p4d_basic_tests(&args);
	pgd_basic_tests(&args);

	/* 纯值分类阶段：依次覆盖叶、PROT_NONE、soft-dirty、swap/migration 与大页类型。 */
	pmd_leaf_tests(&args);
	pud_leaf_tests(&args);

	pte_special_tests(&args);
	pte_protnone_tests(&args);
	pmd_protnone_tests(&args);

	pte_soft_dirty_tests(&args);
	pmd_soft_dirty_tests(&args);
	pte_swap_soft_dirty_tests(&args);
	pmd_leaf_soft_dirty_tests(&args);

	/* swap exclusive 与编码往返分开检查，防止某一标志掩盖 payload 损坏。 */
	pte_swap_exclusive_tests(&args);

	pte_swap_tests(&args);
	pmd_softleaf_tests(&args);

	/* migration 测试会临时操作独占测试页的锁标志，其余分类仍只处理局部条目值。 */
	swap_migration_tests(&args);

	pmd_thp_tests(&args);
	pud_thp_tests(&args);

	hugetlb_basic_tests(&args);

	/*
	 * Page table modifying tests. They need to hold
	 * proper page table lock.
	 */
	/*
	 * 以下 helper 会改真实页表槽：每一级都持与生产路径相同的锁，并在进入更高层前释放低层锁，
	 * 避免锁层级倒置。各子测试负责把临时叶条目清空或重新挂回保存的下级页表。
	 */

	/* PTE 映射与锁由组合 helper 同时取得；映射失败时子测试跳过，解锁也受非 NULL 保护。 */
	args.ptep = pte_offset_map_lock(args.mm, args.pmdp, args.vaddr, &ptl);
	pte_clear_tests(&args);
	pte_advanced_tests(&args);
	if (args.ptep)
		pte_unmap_unlock(args.ptep, ptl);

	/* PMD 阶段依次验证 clear、THP 修改、huge-vmap 与重新 populate。 */
	ptl = pmd_lock(args.mm, args.pmdp);
	pmd_clear_tests(&args);
	pmd_advanced_tests(&args);
	pmd_huge_tests(&args);
	pmd_populate_tests(&args);
	spin_unlock(ptl);

	/* PUD 阶段保持同样顺序，并由折叠/能力检查在不适用架构上跳过。 */
	ptl = pud_lock(args.mm, args.pudp);
	pud_clear_tests(&args);
	pud_advanced_tests(&args);
	pud_huge_tests(&args);
	pud_populate_tests(&args);
	spin_unlock(ptl);

	/* P4D/PGD 共享 mm->page_table_lock；测试结束重新建立目录链，供统一销毁。 */
	spin_lock(&(args.mm->page_table_lock));
	p4d_clear_tests(&args);
	pgd_clear_tests(&args);
	p4d_populate_tests(&args);
	pgd_populate_tests(&args);
	spin_unlock(&(args.mm->page_table_lock));

	/* 所有测试无论 WARN 与否都走到这里，消费 args 中的完整 ownership。 */
	destroy_args(&args);
	return 0;
}
/* 在 late init 阶段运行，既可使用分配器，又早于普通用户负载依赖这些 helper。 */
late_initcall(debug_vm_pgtable);
