// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 Generic/SW_TAGS KASAN shadow 页表初始化学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * KASAN 把每段内核内存映射到 shadow 地址，instrumented load/store 先查
 * shadow 决定访问是否合法。启动最早期尚无 memblock 页可用，整个 shadow
 * 暂时共享一个只读零页；内存拓扑就绪后，本文件为 kernel image 和实际
 * RAM 分配真实 shadow，并给永远不用的洞继续映射 early zero shadow。
 *
 * 最难的不变量是“重建 shadow 页表的代码本身也被 KASAN 插桩，切换过程
 * 任何时刻都必须有可访问 shadow”。因此先克隆 swapper_pg_dir 到临时
 * tmp_pg_dir，让 CPU 继续使用旧 early shadow；再从正式表清除/重建；
 * 完成后切回 swapper_pg_dir。所有页来自 memblock、生命周期覆盖运行期，
 * 分配失败无法降级而 panic。启动单 CPU 串行执行，无普通页表锁。
 */
/*
 * This file contains kasan initialization code for ARM64.
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 * Author: Andrey Ryabinin <ryabinin.a.a@gmail.com>
 */
/* 原说明界定本文件只负责 arm64 启动映射，KASAN 检测逻辑位于通用代码。 */

#define pr_fmt(fmt) "kasan: " fmt
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/sched/task.h>
#include <linux/memblock.h>
#include <linux/start_kernel.h>
#include <linux/mm.h>

#include <asm/mmu_context.h>
#include <asm/kernel-pgtable.h>
#include <asm/page.h>
#include <asm/pgalloc.h>
#include <asm/sections.h>
#include <asm/tlbflush.h>

#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)

/* 临时 TTBR1 根页表，使用后随 initdata 回收；切换期间内容克隆自正式根。 */
static pgd_t tmp_pg_dir[PTRS_PER_PTE] __initdata __aligned(PAGE_SIZE);

/*
 * The p*d_populate functions call virt_to_phys implicitly so they can't be used
 * directly on kernel symbols (bm_p*d). All the early functions are called too
 * early to use lm_alias so __p*d_populate functions must be used to populate
 * with the physical address from __pa_symbol.
 */
/*
 * early 静态页表属于 kernel image，不一定可按普通 linear-map 地址做
 * virt_to_phys；因此显式 __pa_symbol 并调用底层 __p*d_populate。等线性
 * 映射可用后，动态 memblock 页才走普通 offset helper。
 */

/*
 * 从指定 NUMA node 分配一页清零 shadow/page-table 存储，物理地址低于
 * MAX_DMA_ADDRESS 限界。返回物理地址；失败 panic。NOLEAKTRACE 防止 KASAN
 * 初始化内存被 kmemleak 递归跟踪，memblock 持有页面且运行期不释放。
 */
static phys_addr_t __init kasan_alloc_zeroed_page(int node)
{
	void *p = memblock_alloc_try_nid(PAGE_SIZE, PAGE_SIZE,
					      __pa(MAX_DMA_ADDRESS),
					      MEMBLOCK_ALLOC_NOLEAKTRACE, node);
	if (!p)
		panic("%s: Failed to allocate %lu bytes align=0x%lx nid=%d from=%llx\n",
		      __func__, PAGE_SIZE, PAGE_SIZE, node,
		      __pa(MAX_DMA_ADDRESS));

	return __pa(p);
}

/*
 * 分配未清零 raw 页，随后 shadow leaf 会显式 memset KASAN_SHADOW_INIT。
 * 避免 memblock 自动清零的重复成本；其余参数、所有权和 panic 语义同上。
 */
static phys_addr_t __init kasan_alloc_raw_page(int node)
{
	void *p = memblock_alloc_try_nid_raw(PAGE_SIZE, PAGE_SIZE,
						__pa(MAX_DMA_ADDRESS),
						MEMBLOCK_ALLOC_NOLEAKTRACE,
						node);
	if (!p)
		panic("%s: Failed to allocate %lu bytes align=0x%lx nid=%d from=%llx\n",
		      __func__, PAGE_SIZE, PAGE_SIZE, node,
		      __pa(MAX_DMA_ADDRESS));

	return __pa(p);
}

/*
 * 确保 pmdp 下存在 PTE table 并返回 addr 对应槽。early=true 复用静态
 * kasan_early_shadow_pte 并用 kimg offset；false 时按 node 分配独立零页。
 * pmdp 只在 none 时发布，启动串行无需 cmpxchg；返回借用页表指针。
 */
static pte_t *__init kasan_pte_offset(pmd_t *pmdp, unsigned long addr, int node,
				      bool early)
{
	if (pmd_none(READ_ONCE(*pmdp))) {
		phys_addr_t pte_phys = early ?
				__pa_symbol(kasan_early_shadow_pte)
					: kasan_alloc_zeroed_page(node);
		__pmd_populate(pmdp, pte_phys, PMD_TYPE_TABLE);
	}

	return early ? pte_offset_kimg(pmdp, addr)
		     : pte_offset_kernel(pmdp, addr);
}

/* PUD->PMD 层版本；early 复用 kasan_early_shadow_pmd，正常模式分配新表。 */
static pmd_t *__init kasan_pmd_offset(pud_t *pudp, unsigned long addr, int node,
				      bool early)
{
	if (pud_none(READ_ONCE(*pudp))) {
		phys_addr_t pmd_phys = early ?
				__pa_symbol(kasan_early_shadow_pmd)
					: kasan_alloc_zeroed_page(node);
		__pud_populate(pudp, pmd_phys, PUD_TYPE_TABLE);
	}

	return early ? pmd_offset_kimg(pudp, addr) : pmd_offset(pudp, addr);
}

/* P4D->PUD 层版本，折叠页表配置由通用 offset/populate helper 吸收差异。 */
static pud_t *__init kasan_pud_offset(p4d_t *p4dp, unsigned long addr, int node,
				      bool early)
{
	if (p4d_none(READ_ONCE(*p4dp))) {
		phys_addr_t pud_phys = early ?
				__pa_symbol(kasan_early_shadow_pud)
					: kasan_alloc_zeroed_page(node);
		__p4d_populate(p4dp, pud_phys, P4D_TYPE_TABLE);
	}

	return early ? pud_offset_kimg(p4dp, addr) : pud_offset(p4dp, addr);
}

/* PGD->P4D 层版本；所有 early 静态符号均以 __pa_symbol 取得真实物理地址。 */
static p4d_t *__init kasan_p4d_offset(pgd_t *pgdp, unsigned long addr, int node,
				      bool early)
{
	if (pgd_none(READ_ONCE(*pgdp))) {
		phys_addr_t p4d_phys = early ?
				__pa_symbol(kasan_early_shadow_p4d)
					: kasan_alloc_zeroed_page(node);
		__pgd_populate(pgdp, p4d_phys, PGD_TYPE_TABLE);
	}

	return early ? p4d_offset_kimg(pgdp, addr) : p4d_offset(pgdp, addr);
}

/*
 * 填充单个 PMD 内 [addr,end) 的 shadow PTE。early 模式所有 PTE 指向同一
 * early zero page；正式模式逐 shadow 页分配 raw page 并初始化 poison 值。
 * 遇到下一个已存在 PTE 即停止，避免覆盖其他阶段已建立的映射。
 */
static void __init kasan_pte_populate(pmd_t *pmdp, unsigned long addr,
				      unsigned long end, int node, bool early)
{
	unsigned long next;
	pte_t *ptep = kasan_pte_offset(pmdp, addr, node, early);

	do {
		phys_addr_t page_phys = early ?
				__pa_symbol(kasan_early_shadow_page)
					: kasan_alloc_raw_page(node);
		if (!early)
			/* shadow 初值代表尚未被具体 allocator 标记的启动内存状态。 */
			memset(__va(page_phys), KASAN_SHADOW_INIT, PAGE_SIZE);
		next = addr + PAGE_SIZE;
		__set_pte(ptep, pfn_pte(__phys_to_pfn(page_phys), PAGE_KERNEL));
	} while (ptep++, addr = next, addr != end && pte_none(__ptep_get(ptep)));
}

/* PMD 范围填充器：按 pmd_addr_end 分段递归到 PTE，已有下一 PMD 时停止。 */
static void __init kasan_pmd_populate(pud_t *pudp, unsigned long addr,
				      unsigned long end, int node, bool early)
{
	unsigned long next;
	pmd_t *pmdp = kasan_pmd_offset(pudp, addr, node, early);

	do {
		next = pmd_addr_end(addr, end);
		kasan_pte_populate(pmdp, addr, next, node, early);
	} while (pmdp++, addr = next, addr != end && pmd_none(READ_ONCE(*pmdp)));
}

/* PUD 范围填充器，维护半开区间并兼容折叠/非折叠页表。 */
static void __init kasan_pud_populate(p4d_t *p4dp, unsigned long addr,
				      unsigned long end, int node, bool early)
{
	unsigned long next;
	pud_t *pudp = kasan_pud_offset(p4dp, addr, node, early);

	do {
		next = pud_addr_end(addr, end);
		kasan_pmd_populate(pudp, addr, next, node, early);
	} while (pudp++, addr = next, addr != end && pud_none(READ_ONCE(*pudp)));
}

/* P4D 范围填充器；node/early 原样向下传递，决定分配来源。 */
static void __init kasan_p4d_populate(pgd_t *pgdp, unsigned long addr,
				      unsigned long end, int node, bool early)
{
	unsigned long next;
	p4d_t *p4dp = kasan_p4d_offset(pgdp, addr, node, early);

	do {
		next = p4d_addr_end(addr, end);
		kasan_pud_populate(p4dp, addr, next, node, early);
	} while (p4dp++, addr = next, addr != end && p4d_none(READ_ONCE(*p4dp)));
}

/* 从内核根页表为 [addr,end) 建 shadow 映射；范围必须按调用路径有效。 */
static void __init kasan_pgd_populate(unsigned long addr, unsigned long end,
				      int node, bool early)
{
	unsigned long next;
	pgd_t *pgdp;

	pgdp = pgd_offset_k(addr);
	do {
		next = pgd_addr_end(addr, end);
		kasan_p4d_populate(pgdp, addr, next, node, early);
	} while (pgdp++, addr = next, addr != end);
}

#if defined(CONFIG_ARM64_64K_PAGES) || CONFIG_PGTABLE_LEVELS > 4
#define SHADOW_ALIGN	P4D_SIZE
#else
#define SHADOW_ALIGN	PUD_SIZE
#endif
/* early shadow 起止必须按根下一层覆盖范围对齐，具体由页大小/最大级数决定。 */

/*
 * Return whether 'addr' is aligned to the size covered by a root level
 * descriptor.
 */
/* 返回 addr 是否位于当前 vabits_actual 根表项覆盖边界；无状态副作用。 */
static bool __init root_level_aligned(u64 addr)
{
	int shift = (ARM64_HW_PGTABLE_LEVELS(vabits_actual) - 1) * PTDESC_TABLE_SHIFT;

	return (addr % (PAGE_SIZE << shift)) == 0;
}

/* The early shadow maps everything to a single page of zeroes */
/*
 * 最早汇编/C 启动入口：验证 shadow 布局编译期对齐，并让整个 shadow VA
 * 指向共享 early zero page。若 shadow 起点与 linear region 共用根表项，
 * 先插入独立通用下一层表，避免后续建立 linear map 时改坏共享 KASAN 表。
 * 无返回；静态表已在 BSS 清零，失败属于构建/布局 BUG。
 */
asmlinkage void __init kasan_early_init(void)
{
	BUILD_BUG_ON(KASAN_SHADOW_OFFSET !=
		KASAN_SHADOW_END - (1UL << (64 - KASAN_SHADOW_SCALE_SHIFT)));
	BUILD_BUG_ON(!IS_ALIGNED(_KASAN_SHADOW_START(VA_BITS), SHADOW_ALIGN));
	BUILD_BUG_ON(!IS_ALIGNED(_KASAN_SHADOW_START(VA_BITS_MIN), SHADOW_ALIGN));
	BUILD_BUG_ON(!IS_ALIGNED(KASAN_SHADOW_END, SHADOW_ALIGN));

	if (!root_level_aligned(KASAN_SHADOW_START)) {
		/*
		 * The start address is misaligned, and so the next level table
		 * will be shared with the linear region. This can happen with
		 * 4 or 5 level paging, so install a generic pte_t[] as the
		 * next level. This prevents the kasan_pgd_populate call below
		 * from inserting an entry that refers to the shared KASAN zero
		 * shadow pud_t[]/p4d_t[], which could end up getting corrupted
		 * when the linear region is mapped.
		 */
		/* tbl 只隔离共享的根下一层，具体 shadow leaf 仍由 early populate 建立。 */
		static pte_t tbl[PTRS_PER_PTE] __bss_pgtbl;
		pgd_t *pgdp = pgd_offset_k(KASAN_SHADOW_START);

		set_pgd(pgdp, __pgd(__pa_symbol(tbl) | PGD_TYPE_TABLE));
	}

	kasan_pgd_populate(KASAN_SHADOW_START, KASAN_SHADOW_END, NUMA_NO_NODE,
			   true);
}

/* Set up full kasan mappings, ensuring that the mapped pages are zeroed */
/* 将任意 shadow 字节范围扩成整页后分配正式 shadow；node 决定 NUMA 归属。 */
static void __init kasan_map_populate(unsigned long start, unsigned long end,
				      int node)
{
	kasan_pgd_populate(start & PAGE_MASK, PAGE_ALIGN(end), node, false);
}

/*
 * Return the descriptor index of 'addr' in the root level table
 */
/*
 * 计算 addr 在 TTBR1 根表中的索引。64K 页+52-bit 扩展根表即使 CPU 只用
 * 48-bit 也按 VA_BITS 布局，其他配置按 vabits_actual 屏蔽高位。
 */
static int __init root_level_idx(u64 addr)
{
	/*
	 * On 64k pages, the TTBR1 range root tables are extended for 52-bit
	 * virtual addressing, and TTBR1 will simply point to the pgd_t entry
	 * that covers the start of the 48-bit addressable VA space if LVA is
	 * not implemented. This means we need to index the table as usual,
	 * instead of masking off bits based on vabits_actual.
	 */
	/* vabits 的选择必须与硬件 TTBR1 指向扩展 PGD 中哪个入口的规则一致。 */
	u64 vabits = IS_ENABLED(CONFIG_ARM64_64K_PAGES) ? VA_BITS
							: vabits_actual;
	int shift = (ARM64_HW_PGTABLE_LEVELS(vabits) - 1) * PTDESC_TABLE_SHIFT;

	return (addr & ~_PAGE_OFFSET(vabits)) >> (shift + PAGE_SHIFT);
}

/*
 * Clone a next level table from swapper_pg_dir into tmp_pg_dir
 */
/*
 * 克隆 addr 根项指向的下一层整页到 pud 临时缓冲，再让 tmp_pg_dir 对应项
 * 指向副本。用于 shadow 边界只覆盖部分根项时保留同项内的 linear mapping。
 * tmp_pg_dir/pud 均为调用者提供的 init 静态页，函数不分配或释放。
 */
static void __init clone_next_level(u64 addr, pgd_t *tmp_pg_dir, pud_t *pud)
{
	int idx = root_level_idx(addr);
	pgd_t pgd = READ_ONCE(swapper_pg_dir[idx]);
	pud_t *pudp = (pud_t *)__phys_to_kimg(__pgd_to_phys(pgd));

	memcpy(pud, pudp, PAGE_SIZE);
	tmp_pg_dir[idx] = __pgd(__phys_to_pgd_val(__pa_symbol(pud)) |
				PUD_TYPE_TABLE);
}

/*
 * Return the descriptor index of 'addr' in the next level table
 */
/* 计算 addr 在根下一层表中的槽号，返回 [0,PTRS_PER_PTE)。 */
static int __init next_level_idx(u64 addr)
{
	int shift = (ARM64_HW_PGTABLE_LEVELS(vabits_actual) - 2) * PTDESC_TABLE_SHIFT;

	return (addr >> (shift + PAGE_SHIFT)) % PTRS_PER_PTE;
}

/*
 * Dereference the table descriptor at 'pgd_idx' and clear the entries from
 * 'start' to 'end' (exclusive) from the table.
 */
/*
 * 从正式 swapper_pg_dir[pgd_idx] 解引用下一层表，并把 [start,end) 项清零。
 * 只用于启动单线程且当前 CPU 正运行临时根，故无需锁/TLBI。
 */
static void __init clear_next_level(int pgd_idx, int start, int end)
{
	pgd_t pgd = READ_ONCE(swapper_pg_dir[pgd_idx]);
	pud_t *pudp = (pud_t *)__phys_to_kimg(__pgd_to_phys(pgd));

	memset(&pudp[start], 0, (end - start) * sizeof(pud_t));
}

/*
 * 清除正式根中 [start,end) shadow 映射。边界不按根项对齐时只清对应下一
 * 层片段，中间完整根项直接 memset；调用时 CPU 必须使用 tmp_pg_dir。
 */
static void __init clear_shadow(u64 start, u64 end)
{
	int l = root_level_idx(start), m = root_level_idx(end);

	if (!root_level_aligned(start))
		clear_next_level(l++, next_level_idx(start), PTRS_PER_PTE);
	if (!root_level_aligned(end))
		clear_next_level(m, 0, next_level_idx(end));
	memset(&swapper_pg_dir[l], 0, (m - l) * sizeof(pgd_t));
}

/*
 * 完整 shadow 重建主流程。计算 kernel image/module/vmalloc/RAM 的 shadow
 * 边界，克隆临时根并切 TTBR1，清旧 early shadow，然后为真实可访问内存
 * 分配独立 shadow、为洞复用 early zero page，最后把共享 early leaf 设为
 * 只读并切回 swapper_pg_dir。所有地址均为 shadow VA 半开区间。
 */
static void __init kasan_init_shadow(void)
{
	static pud_t pud[2][PTRS_PER_PUD] __initdata __aligned(PAGE_SIZE);
	u64 kimg_shadow_start, kimg_shadow_end;
	u64 mod_shadow_start;
	u64 vmalloc_shadow_end;
	phys_addr_t pa_start, pa_end;
	u64 i;

	kimg_shadow_start = (u64)kasan_mem_to_shadow(KERNEL_START) & PAGE_MASK;
	kimg_shadow_end = PAGE_ALIGN((u64)kasan_mem_to_shadow(KERNEL_END));

	mod_shadow_start = (u64)kasan_mem_to_shadow((void *)MODULES_VADDR);

	vmalloc_shadow_end = (u64)kasan_mem_to_shadow((void *)VMALLOC_END);

	/*
	 * We are going to perform proper setup of shadow memory.
	 * At first we should unmap early shadow (clear_pgds() call below).
	 * However, instrumented code couldn't execute without shadow memory.
	 * tmp_pg_dir used to keep early shadow mapped until full shadow
	 * setup will be finished.
	 */
	/* tmp 根是过渡安全网：没有它，clear_shadow 后当前函数下一次插桩访问会 fault。 */
	memcpy(tmp_pg_dir, swapper_pg_dir, sizeof(tmp_pg_dir));

	/*
	 * If the start or end address of the shadow region is not aligned to
	 * the root level size, we have to allocate a temporary next-level table
	 * in each case, clone the next level of descriptors, and install the
	 * table into tmp_pg_dir. Note that with 5 levels of paging, the next
	 * level will in fact be p4d_t, but that makes no difference in this
	 * case.
	 */
	/* 只克隆边界根项，中间项继续共享，降低临时静态页需求。 */
	if (!root_level_aligned(KASAN_SHADOW_START))
		clone_next_level(KASAN_SHADOW_START, tmp_pg_dir, pud[0]);
	if (!root_level_aligned(KASAN_SHADOW_END))
		clone_next_level(KASAN_SHADOW_END, tmp_pg_dir, pud[1]);
	dsb(ishst);
	/* 发布临时表内容后再替换 TTBR1，防止硬件看到未完成 descriptor。 */
	cpu_replace_ttbr1(lm_alias(tmp_pg_dir));

	clear_shadow(KASAN_SHADOW_START, KASAN_SHADOW_END);

	kasan_map_populate(kimg_shadow_start, kimg_shadow_end,
			   early_pfn_to_nid(virt_to_pfn(lm_alias(KERNEL_START))));

	kasan_populate_early_shadow(kasan_mem_to_shadow((void *)PAGE_END),
				   (void *)mod_shadow_start);

	BUILD_BUG_ON(VMALLOC_START != MODULES_END);
	kasan_populate_early_shadow((void *)vmalloc_shadow_end,
				    (void *)KASAN_SHADOW_END);

	for_each_mem_range(i, &pa_start, &pa_end) {
		/* 只给真实 memblock RAM 建独立 shadow，洞保持共享 early shadow。 */
		void *start = (void *)__phys_to_virt(pa_start);
		void *end = (void *)__phys_to_virt(pa_end);

		if (start >= end)
			break;

		kasan_map_populate((unsigned long)kasan_mem_to_shadow(start),
				   (unsigned long)kasan_mem_to_shadow(end),
				   early_pfn_to_nid(virt_to_pfn(start)));
	}

	/*
	 * KAsan may reuse the contents of kasan_early_shadow_pte directly,
	 * so we should make sure that it maps the zero page read-only.
	 */
	/* 共享一页若可写，任一洞的 shadow store 会污染所有其他洞，必须 RO。 */
	for (i = 0; i < PTRS_PER_PTE; i++)
		__set_pte(&kasan_early_shadow_pte[i],
			pfn_pte(sym_to_pfn(kasan_early_shadow_page),
				PAGE_KERNEL_RO));

	memset(kasan_early_shadow_page, KASAN_SHADOW_INIT, PAGE_SIZE);
	cpu_replace_ttbr1(lm_alias(swapper_pg_dir));
}

/* 为初始任务建立正常 KASAN 嵌套深度基线；0 表示检测启用。 */
static void __init kasan_init_depth(void)
{
	init_task.kasan_depth = 0;
}

#ifdef CONFIG_KASAN_VMALLOC
/*
 * KASAN_VMALLOC 早期钩子：仅对 vmalloc/module VA 的 [start,start+size)
 * 分配页对齐 shadow。普通线性地址由主初始化覆盖；无返回，失败会 panic。
 */
void __init kasan_populate_early_vm_area_shadow(void *start, unsigned long size)
{
	unsigned long shadow_start, shadow_end;

	if (!is_vmalloc_or_module_addr(start))
		return;

	shadow_start = (unsigned long)kasan_mem_to_shadow(start);
	shadow_start = ALIGN_DOWN(shadow_start, PAGE_SIZE);
	shadow_end = (unsigned long)kasan_mem_to_shadow(start + size);
	shadow_end = ALIGN(shadow_end, PAGE_SIZE);
	kasan_map_populate(shadow_start, shadow_end, NUMA_NO_NODE);
}
#endif

/*
 * arm64 KASAN 正式初始化入口：先建立完整 shadow，再初始化 init_task 深度
 * 和通用 Generic KASAN。无返回；SW/HW tag 模式仍由各自后续 hook 完成，
 * 本函数结束只代表 Generic shadow 基础可用。
 */
void __init kasan_init(void)
{
	kasan_init_shadow();
	kasan_init_depth();
	kasan_init_generic();
	/*
	 * Generic KASAN is now fully initialized.
	 * Software and Hardware Tag-Based modes still require
	 * kasan_init_sw_tags() and kasan_init_hw_tags() correspondingly.
	 */
	/* 三种模式共享部分启动基础，但 tag 模式不能把 generic 完成误当最终完成。 */
}

#endif /* CONFIG_KASAN_GENERIC || CONFIG_KASAN_SW_TAGS */
