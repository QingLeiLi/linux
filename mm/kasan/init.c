// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains KASAN shadow initialization code.
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 * Author: Andrey Ryabinin <ryabinin.a.a@gmail.com>
 */
/*
 * 本文件包含 KASAN shadow 的初始化代码；版权与作者信息按许可证要求原样保留。
 * 这里的通用实现由各体系结构启动代码和运行期内存映射生命周期共同调用。
 */

#include <linux/memblock.h>
#include <linux/init.h>
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/pfn.h>
#include <linux/slab.h>
#include <linux/pgalloc.h>

#include <asm/page.h>

#include "kasan.h"

/*
 * 本文件建立 KASAN 最早可用、且可被多个虚拟区间共享的 shadow 映射。
 * 架构初始化先借这些静态页表把尚未准备真实 shadow 的范围指向同一只读页；
 * 内存热插拔和 devm_memremap_pages() 后续再按原内存范围拆除或恢复这层占位映射。
 */

/*
 * This page serves two purposes:
 *   - It used as early shadow memory. The entire shadow region populated
 *     with this page, before we will be able to setup normal shadow memory.
 *   - Latter it reused it as zero shadow to cover large ranges of memory
 *     that allowed to access, but not handled by kasan (vmalloc/vmemmap ...).
 */
/*
 * 该页兼任两个阶段：启动早期，所有未建立真实 shadow 的地址都读到它；启动后，
 * vmalloc、vmemmap 等允许访问却不由 KASAN 逐字节跟踪的范围仍用它作为零 shadow。
 * 多个 PTE 共享同一物理页，因此映射必须只读，避免一次 shadow 写污染所有别名。
 */
unsigned char kasan_early_shadow_page[PAGE_SIZE] __bss_pgtbl;

#if CONFIG_PGTABLE_LEVELS > 4
/* 五级页表配置下的共享 P4D 页；架构代码在正式分配器可用前填充并发布它。 */
p4d_t kasan_early_shadow_p4d[MAX_PTRS_PER_P4D] __bss_pgtbl;
/*
 * 业务背景：拆除零 shadow 时识别 PGD 是否仍指向全局共享 P4D，避免误释放静态表。
 * 入参：pgd 是借用的页表项值，不取得其所指页引用。
 * 出参/返回：指向共享表返回 true，否则 false；无状态副作用。
 * 注意事项：仅在五级页表配置有真实比较，调用者仍须负责地址对齐和页表稳定性。
 */
static inline bool kasan_p4d_table(pgd_t pgd)
{
	return pgd_page(pgd) == virt_to_page(lm_alias(kasan_early_shadow_p4d));
}
#else
/*
 * 业务背景：折叠 P4D 的配置不存在独立共享 P4D 页，给通用拆表代码提供恒假桩。
 * 入参：pgd 为未使用的借用值；出参/返回：恒为 false，无副作用。
 * 注意事项：页表折叠由体系结构 helper 处理，本桩不能被理解为 PGD 一定不是 early 映射。
 */
static inline bool kasan_p4d_table(pgd_t pgd)
{
	return false;
}
#endif
#if CONFIG_PGTABLE_LEVELS > 3
/* 四级及以上配置下的共享 PUD 页；其每项最终通向同一个 early shadow 页。 */
pud_t kasan_early_shadow_pud[MAX_PTRS_PER_PUD] __bss_pgtbl;
/*
 * 业务背景：判断 P4D 是否指向 KASAN 静态共享 PUD，以选择整层清项或逐级拆除。
 * 入参：p4d 是借用的页表项快照；出参/返回：物理页相同返回 true，否则 false。
 * 注意事项：只比较身份，不冻结页表内容，也不取得引用；调用者保证 init_mm 映射稳定。
 */
static inline bool kasan_pud_table(p4d_t p4d)
{
	return p4d_page(p4d) == virt_to_page(lm_alias(kasan_early_shadow_pud));
}
#else
/*
 * 业务背景：PUD 折叠时没有可独立识别的共享页，通用拆表逻辑按非共享路径继续。
 * 入参：p4d 未使用；出参/返回：恒 false；注意事项：无分配、释放或同步副作用。
 */
static inline bool kasan_pud_table(p4d_t p4d)
{
	return false;
}
#endif
#if CONFIG_PGTABLE_LEVELS > 2
/* 三级及以上配置下的共享 PMD 页，生命周期覆盖整个内核运行期。 */
pmd_t kasan_early_shadow_pmd[MAX_PTRS_PER_PMD] __bss_pgtbl;
/*
 * 业务背景：识别 PUD 指向的是否为静态共享 PMD，防止把 __bss_pgtbl 内存交给释放器。
 * 入参：pud 是借用的项值；出参/返回：共享页身份匹配返回 true，否则 false。
 * 注意事项：比较使用线性映射别名；它只回答身份，不保证并发页表内容不变。
 */
static inline bool kasan_pmd_table(pud_t pud)
{
	return pud_page(pud) == virt_to_page(lm_alias(kasan_early_shadow_pmd));
}
#else
/*
 * 业务背景：PMD 折叠配置的兼容桩。
 * 入参：pud 未使用；出参/返回：恒 false，无副作用。
 * 注意事项：上层仍通过体系结构的折叠页表 helper 到达 PTE。
 */
static inline bool kasan_pmd_table(pud_t pud)
{
	return false;
}
#endif
/* 共享 PTE 页；额外的 PTE_HWTABLE_PTRS 为需要硬件附属表的体系结构保留空间。 */
pte_t kasan_early_shadow_pte[MAX_PTRS_PER_PTE + PTE_HWTABLE_PTRS]
	__bss_pgtbl;

/*
 * 业务背景：拆除局部零 shadow 前识别 PMD 是否直接复用全局共享 PTE 表。
 * 入参：pmd 是借用快照；出参/返回：指向共享 PTE 页返回 true，否则 false。
 * 注意事项：不取得页表页引用，必须在 init_mm 页表由调用路径稳定时使用。
 */
static inline bool kasan_pte_table(pmd_t pmd)
{
	return pmd_page(pmd) == virt_to_page(lm_alias(kasan_early_shadow_pte));
}

/*
 * 业务背景：最末级拆表只允许清除指向 early shadow 页的占位项，保护真实 shadow。
 * 入参：pte 是借用的页表项快照；出参/返回：映射目标为共享零页时返回 true。
 * 注意事项：只比较物理页身份；权限位不同不影响结论，也不会修改页表。
 */
static inline bool kasan_early_shadow_page_entry(pte_t pte)
{
	return pte_page(pte) == virt_to_page(lm_alias(kasan_early_shadow_page));
}

/*
 * 业务背景：slab 尚不可用时，为 KASAN 页表从 memblock 取得启动期永久内存。
 * 入参：size 为字节数且同时作为对齐；node 是 NUMA 节点或 NUMA_NO_NODE。
 * 出参/返回：返回已清零、调用者持有的虚拟地址；失败直接 panic，不返回 NULL。
 * 注意事项：仅启动期调用、可修改 memblock；从 MAX_DMA_ADDRESS 之后优先分配，
 * 这些页没有本函数内的回滚路径，之后由 init_mm 页表生命周期接管。
 */
static __init void *early_alloc(size_t size, int node)
{
	/* ptr 在成功后把新分配页的所有权交给逐级 populate helper。 */
	void *ptr = memblock_alloc_try_nid(size, size, __pa(MAX_DMA_ADDRESS),
					   MEMBLOCK_ALLOC_ACCESSIBLE, node);

	if (!ptr)
		panic("%s: Failed to allocate %zu bytes align=%zx nid=%d from=%llx\n",
		      __func__, size, size, node, (u64)__pa(MAX_DMA_ADDRESS));

	return ptr;
}

/*
 * 业务背景：为一个 PMD 子区间安装只读共享零 shadow PTE，完成最末级映射发布。
 * 入参：pmd 借用且已指向有效 PTE 表；addr/end 是页对齐 shadow 半开区间。
 * 出参/返回：无直接返回值；逐项修改 init_mm PTE，不转移 pmd 所有权。
 * 注意事项：启动/热插拔串行页表上下文，可调用体系结构 set_pte_at；共享页只读，
 * 循环仅覆盖完整页面，调用者必须保证范围和表项已准备好。
 */
static void __ref zero_pte_populate(pmd_t *pmd, unsigned long addr,
				unsigned long end)
{
	/* pte 随 addr 前进；zero_pte 是所有目标项复用的只读模板。 */
	pte_t *pte = pte_offset_kernel(pmd, addr);
	pte_t zero_pte;

	zero_pte = pfn_pte(PFN_DOWN(__pa_symbol(kasan_early_shadow_page)),
				PAGE_KERNEL);
	zero_pte = pte_wrprotect(zero_pte);

	/* 每次 set_pte_at() 都让对应 shadow 虚拟页开始读到同一个零页。 */
	while (addr + PAGE_SIZE <= end) {
		set_pte_at(&init_mm, addr, pte, zero_pte);
		addr += PAGE_SIZE;
		pte = pte_offset_kernel(pmd, addr);
	}
}

/*
 * 业务背景：在一个 PUD 范围内复用整张 early PTE 表，或为边缘区间分配独立 PTE 表。
 * 入参：pud 借用且已存在；addr/end 为 shadow 半开区间，函数可更新其中 PMD。
 * 出参/返回：成功返回 0；slab 分配失败返回 -ENOMEM，已建映射留给上层统一回滚。
 * 注意事项：可在 slab 前后调用；slab 前 memblock 失败会 panic，slab 后可能睡眠。
 */
static int __ref zero_pmd_populate(pud_t *pud, unsigned long addr,
				unsigned long end)
{
	/* pmd/next 描述当前 PMD 槽及不跨越其边界的子区间。 */
	pmd_t *pmd = pmd_offset(pud, addr);
	unsigned long next;

	/* 逐 PMD 切片，整块走共享表快速路径，首尾碎片逐 PTE 建映射。 */
	do {
		next = pmd_addr_end(addr, end);

		if (IS_ALIGNED(addr, PMD_SIZE) && end - addr >= PMD_SIZE) {
			/* 整个 PMD 可直接发布静态 PTE 表，无需分配页表页。 */
			pmd_populate_kernel(&init_mm, pmd,
					lm_alias(kasan_early_shadow_pte));
			continue;
		}

		if (pmd_none(*pmd)) {
			/* p 是新 PTE 表；populate 成功后由 init_mm 页表拥有。 */
			pte_t *p;

			/* slab 就绪后允许可恢复分配；启动早期改由必成功语义的 memblock。 */
			if (slab_is_available())
				p = pte_alloc_one_kernel(&init_mm);
			else {
				p = early_alloc(PAGE_SIZE, NUMA_NO_NODE);
				kernel_pte_init(p);
			}
			/* 只有 slab 分配用 NULL 表达失败；此时父 PMD 仍未发布新表。 */
			if (!p)
				return -ENOMEM;

			pmd_populate_kernel(&init_mm, pmd, p);
		}
		zero_pte_populate(pmd, addr, next);
	} while (pmd++, addr = next, addr != end);

	return 0;
}

/*
 * 业务背景：把 PUD 子区间下沉到 PMD/PTE，并对完整 PUD 复用两级 early 表。
 * 入参：p4d 为借用页表指针；addr/end 为 shadow 半开区间，可修改其下级项。
 * 出参/返回：本层 PMD 分配成功返回 0、失败返回 -ENOMEM；更低层失败当前不向上传播。
 * 注意事项：调用者 zero_p4d_populate() 也忽略此返回值；slab 前 memblock 失败 panic，
 * 折叠页表 helper 仍保持同一接口，已发布的部分映射不会在本层回滚。
 */
static int __ref zero_pud_populate(p4d_t *p4d, unsigned long addr,
				unsigned long end)
{
	/* pud/next 使每轮只处理当前 PUD 所覆盖的地址片段。 */
	pud_t *pud = pud_offset(p4d, addr);
	unsigned long next;

	do {
		next = pud_addr_end(addr, end);
		if (IS_ALIGNED(addr, PUD_SIZE) && end - addr >= PUD_SIZE) {
			/* 完整 PUD 同时接上共享 PMD 与 PTE，快速形成可遍历页表链。 */
			pmd_t *pmd;

			pud_populate(&init_mm, pud,
					lm_alias(kasan_early_shadow_pmd));
			pmd = pmd_offset(pud, addr);
			pmd_populate_kernel(&init_mm, pmd,
					lm_alias(kasan_early_shadow_pte));
			continue;
		}

		if (pud_none(*pud)) {
			/* 边缘片段需要私有 PMD 表，避免改写会被其他区间共享的静态表。 */
			pmd_t *p;

			/* 两个分配阶段最终都在 pud_populate() 处把新表交给 init_mm。 */
			if (slab_is_available()) {
				p = pmd_alloc(&init_mm, pud, addr);
				if (!p)
					return -ENOMEM;
			} else {
				p = early_alloc(PAGE_SIZE, NUMA_NO_NODE);
				pmd_init(p);
				pud_populate(&init_mm, pud, p);
			}
		}
		/* 当前源码不传播 zero_pmd_populate() 的返回值；只有本层分配失败会返回。 */
		zero_pmd_populate(pud, addr, next);
	} while (pud++, addr = next, addr != end);

	return 0;
}

/*
 * 业务背景：把 PGD 子区间下沉到 P4D/PUD/PMD/PTE，供顶层按 PGD 分段建立零 shadow。
 * 入参：pgd 为借用页表指针；addr/end 是 shadow 半开区间并允许更新下级项。
 * 出参/返回：本层 PUD 分配成功返回 0、失败返回 -ENOMEM；下层 errno 当前不会传播。
 * 注意事项：顶层调用者忽略本函数返回值；完整 P4D 复用静态页表，边缘范围用私有表，
 * slab 前不可恢复的失败 panic，已发布的部分映射不在本层撤销。
 */
static int __ref zero_p4d_populate(pgd_t *pgd, unsigned long addr,
				unsigned long end)
{
	/* p4d/next 分别是当前槽和该槽内不会跨边界的结束地址。 */
	p4d_t *p4d = p4d_offset(pgd, addr);
	unsigned long next;

	do {
		next = p4d_addr_end(addr, end);
		if (IS_ALIGNED(addr, P4D_SIZE) && end - addr >= P4D_SIZE) {
			/* 整 P4D 快速路径一次串起三张共享表，避免启动期大量分配。 */
			pud_t *pud;
			pmd_t *pmd;

			p4d_populate_kernel(addr, p4d,
					lm_alias(kasan_early_shadow_pud));
			pud = pud_offset(p4d, addr);
			pud_populate(&init_mm, pud,
					lm_alias(kasan_early_shadow_pmd));
			/* 接上最末级共享表后，该完整 P4D 范围即可解析到只读零页。 */
			pmd = pmd_offset(pud, addr);
			pmd_populate_kernel(&init_mm, pmd,
					lm_alias(kasan_early_shadow_pte));
			continue;
		}

		if (p4d_none(*p4d)) {
			/* 非完整边缘区间取得私有 PUD 表，所有权随 populate 交给 init_mm。 */
			pud_t *p;

			/* slab 分配失败时尚未发布子表，可直接以 -ENOMEM 离开本层。 */
			if (slab_is_available()) {
				p = pud_alloc(&init_mm, p4d, addr);
				if (!p)
					return -ENOMEM;
			} else {
				p = early_alloc(PAGE_SIZE, NUMA_NO_NODE);
				pud_init(p);
				p4d_populate_kernel(addr, p4d, p);
			}
		}
		/* 当前源码忽略 zero_pud_populate() 的返回值，本层只报告自身分配失败。 */
		zero_pud_populate(p4d, addr, next);
	} while (p4d++, addr = next, addr != end);

	return 0;
}

/**
 * kasan_populate_early_shadow - populate shadow memory region with
 *                               kasan_early_shadow_page
 * @shadow_start: start of the memory range to populate
 * @shadow_end: end of the memory range to populate
 */
/*
 * 业务背景：架构启动代码及运行期零 shadow 用户通过本函数为 shadow 地址区间建立占位映射；
 * 调用链为架构 KASAN 初始化或 kasan_add_zero_shadow() → 本函数 → 各级 zero_*_populate()。
 * 入参：shadow_start/shadow_end 是待映射 shadow 虚拟地址的借用边界，组成左闭右开区间，
 * 调用者保有地址所有权；二者必须落在内核页表可表示范围且 start 不大于 end。
 * 出参/返回：顶层 p4d_alloc() 成功后返回 0，直接失败返回 -ENOMEM；会修改 init_mm 页表。
 * 当前源码不传播更低层 populate 的 errno，因此返回 0 只代表顶层遍历完成。
 * 注意事项：__ref 允许启动期和运行期入口共用；slab 前使用 memblock 且失败 panic，
 * slab 后分配可能睡眠。调用路径必须串行化页表更新，并在需要时负责 TLB/体系结构同步。
 */
int __ref kasan_populate_early_shadow(const void *shadow_start,
					const void *shadow_end)
{
	/* addr/end 是 shadow 字节地址；pgd/next 驱动按顶级页表边界切片。 */
	unsigned long addr = (unsigned long)shadow_start;
	unsigned long end = (unsigned long)shadow_end;
	pgd_t *pgd = pgd_offset_k(addr);
	unsigned long next;

	/* 每轮只处理一个 PGD 覆盖片段，避免下层 helper 跨越父表项。 */
	do {
		next = pgd_addr_end(addr, end);

		if (IS_ALIGNED(addr, PGDIR_SIZE) && end - addr >= PGDIR_SIZE) {
			/* 完整 PGD 直接串接所有静态共享表，是无需分配的启动快速路径。 */
			p4d_t *p4d;
			pud_t *pud;
			pmd_t *pmd;

			/*
			 * kasan_early_shadow_pud should be populated with pmds
			 * at this moment.
			 * [pud,pmd]_populate*() below needed only for
			 * 3,2 - level page tables where we don't have
			 * puds,pmds, so pgd_populate(), pud_populate()
			 * is noops.
			 */
			/*
			 * 此时共享 PUD 理应已填入 PMD。下面仍调用各级 populate，是为了
			 * 让三层或两层页表的折叠 helper 执行其必要动作；在那些配置中相应
			 * 层级不存在，pgd_populate()/pud_populate() 会退化为空操作。
			 */
			pgd_populate_kernel(addr, pgd,
					lm_alias(kasan_early_shadow_p4d));
			p4d = p4d_offset(pgd, addr);
			p4d_populate_kernel(addr, p4d,
					lm_alias(kasan_early_shadow_pud));
			/* 中间两层同样使用静态表，最后由共享 PTE 项落到同一 shadow 页。 */
			pud = pud_offset(p4d, addr);
			pud_populate(&init_mm, pud,
					lm_alias(kasan_early_shadow_pmd));
			pmd = pmd_offset(pud, addr);
			pmd_populate_kernel(&init_mm, pmd,
					lm_alias(kasan_early_shadow_pte));
			continue;
		}

		if (pgd_none(*pgd)) {

			/* 边缘 PGD 需要私有 P4D；分配成功后其生命周期归 init_mm 页表。 */
			if (slab_is_available()) {
				if (!p4d_alloc(&init_mm, pgd, addr))
					return -ENOMEM;
			} else {
				pgd_populate_kernel(addr, pgd,
					early_alloc(PAGE_SIZE, NUMA_NO_NODE));
			}
		}
		/* 继续把当前顶层片段下沉到 P4D/PUD/PMD/PTE；已建表项立即生效。 */
		zero_p4d_populate(pgd, addr, next);
	} while (pgd++, addr = next, addr != end);

	return 0;
}

/*
 * 业务背景：下级拆除后，仅当一张私有 PTE 表已经全空时才释放它并清父 PMD。
 * 入参：pte_start 是待检查且由 init_mm 拥有的表首页；pmd 是指向它的借用父项。
 * 出参/返回：无直接返回值；非空时不变，全空时释放页表并清除父项。
 * 注意事项：调用者必须保证它不是静态共享 kasan_early_shadow_pte，并稳定页表；
 * 引用计数不参与这里的判断，任一有效 PTE 都会阻止释放。
 */
static void kasan_free_pte(pte_t *pte_start, pmd_t *pmd)
{
	/* pte/i 只在扫描期间有效，用于证明整张表已无映射。 */
	pte_t *pte;
	int i;

	for (i = 0; i < PTRS_PER_PTE; i++) {
		pte = pte_start + i;
		if (!pte_none(ptep_get(pte)))
			return;
	}

	/* 先释放空子表，再清父项，完成自底向上的 ownership 撤销。 */
	pte_free_kernel(&init_mm, pte_start);
	pmd_clear(pmd);
}

/*
 * 业务背景：PTE 层回收后尝试折叠一张已空私有 PMD 表。
 * 入参：pmd_start 为 init_mm 持有的表首页；pud 为借用父项且应指向该表。
 * 出参/返回：无；发现非空项立即保留，全部为空则释放 PMD 表并清 PUD。
 * 注意事项：共享 early PMD 由调用者先识别，不能交给 pmd_free；页表更新须串行。
 */
static void kasan_free_pmd(pmd_t *pmd_start, pud_t *pud)
{
	pmd_t *pmd;
	int i;

	/* 全表扫描是释放的正确性门槛，防止丢掉同表内仍由别的范围使用的映射。 */
	for (i = 0; i < PTRS_PER_PMD; i++) {
		pmd = pmd_start + i;
		if (!pmd_none(*pmd))
			return;
	}

	pmd_free(&init_mm, pmd_start);
	pud_clear(pud);
}

/*
 * 业务背景：PMD 子表清空后，回收不再承载任何映射的私有 PUD 表。
 * 入参：pud_start 是候选表首页；p4d 是借用父项。
 * 出参/返回：无；非空保持原状，全空则释放表并清父项。
 * 注意事项：静态共享 kasan_early_shadow_pud 生命周期为永久，必须由上层排除。
 */
static void kasan_free_pud(pud_t *pud_start, p4d_t *p4d)
{
	pud_t *pud;
	int i;

	/* 任一非空 PUD 都表示同表仍服务其他地址范围，必须停止回收。 */
	for (i = 0; i < PTRS_PER_PUD; i++) {
		pud = pud_start + i;
		if (!pud_none(*pud))
			return;
	}

	pud_free(&init_mm, pud_start);
	p4d_clear(p4d);
}

/*
 * 业务背景：PUD 子树移除后，回收整张空私有 P4D 并断开 PGD 指针。
 * 入参：p4d_start 为候选表首页；pgd 为借用父项。
 * 出参/返回：无；仅在全部 P4D 项为空时释放并清父项。
 * 注意事项：共享 kasan_early_shadow_p4d 不可释放；折叠层级由体系结构 helper 处理。
 */
static void kasan_free_p4d(p4d_t *p4d_start, pgd_t *pgd)
{
	p4d_t *p4d;
	int i;

	/* 先验证整表为空，随后 free/clear 顺序与建表时的 populate 相反。 */
	for (i = 0; i < PTRS_PER_P4D; i++) {
		p4d = p4d_start + i;
		if (!p4d_none(*p4d))
			return;
	}

	p4d_free(&init_mm, p4d_start);
	pgd_clear(pgd);
}

/*
 * 业务背景：移除一个 PTE 级 shadow 区间，但只清 KASAN 共享零页占位映射。
 * 入参：pte 为起始项借用指针；addr/end 是 shadow 半开区间且逐页推进。
 * 出参/返回：无；目标项缺失则跳过，匹配共享零页则从 init_mm 清除。
 * 注意事项：若发现 present PTE 指向真实 shadow，WARN 并保留，避免破坏检测元数据；
 * 调用者负责页表同步和随后尝试释放空表。
 */
static void kasan_remove_pte_table(pte_t *pte, unsigned long addr,
				unsigned long end)
{
	/* next 避免末段越过 end；ptent 是当前 PTE 的一致读取快照。 */
	unsigned long next;
	pte_t ptent;

	for (; addr < end; addr = next, pte++) {
		next = (addr + PAGE_SIZE) & PAGE_MASK;
		if (next > end)
			next = end;

		ptent = ptep_get(pte);

		/* 空洞无需处理；真实 shadow 项通过身份检查被明确拒绝。 */
		if (!pte_present(ptent))
			continue;

		if (WARN_ON(!kasan_early_shadow_page_entry(ptent)))
			continue;
		pte_clear(&init_mm, addr, pte);
	}
}

/*
 * 业务背景：在 PMD 层优先整项断开共享 PTE 表，否则递归清局部 PTE 并回收空私有表。
 * 入参：pmd 是起始借用项；addr/end 为 shadow 半开区间。
 * 出参/返回：无；只移除零 shadow 覆盖，真实或非 present 映射保持不变。
 * 注意事项：完整对齐范围才能直接清共享 PMD；部分范围会逐 PTE 处理，调用者必须保证
 * 目标共享项不再被其他零-shadow 别名依赖，否则清项会影响所有复用该静态表的父项。
 */
static void kasan_remove_pmd_table(pmd_t *pmd, unsigned long addr,
				unsigned long end)
{
	/* next 限制当前 PMD 片段；pte 仅借用当前下级表。 */
	unsigned long next;

	for (; addr < end; addr = next, pmd++) {
		pte_t *pte;

		next = pmd_addr_end(addr, end);

		if (!pmd_present(*pmd))
			continue;

		if (kasan_pte_table(*pmd)) {
			/* 整个共享表覆盖都在删除范围内时，仅清父项，静态表本身永久保留。 */
			if (IS_ALIGNED(addr, PMD_SIZE) &&
			    IS_ALIGNED(next, PMD_SIZE)) {
				pmd_clear(pmd);
				continue;
			}
		}
		/* 私有表或共享表的局部范围逐项清理，随后只回收真正空的私有表。 */
		pte = pte_offset_kernel(pmd, addr);
		kasan_remove_pte_table(pte, addr, next);
		kasan_free_pte(pte_offset_kernel(pmd, 0), pmd);
	}
}

/*
 * 业务背景：PUD 层执行与 PMD 相同的“整块断链或递归拆分”策略。
 * 入参：pud 为起始借用项；addr/end 是 shadow 半开区间。
 * 出参/返回：无；清除目标零 shadow，并可能释放已空私有 PMD 表。
 * 注意事项：共享 PMD 表完整覆盖时只断开父项；局部删除会继续下沉，范围生命周期
 * 必须保证被清的共享子项不再由其他父项依赖，静态表内存本身不能交给释放器。
 */
static void kasan_remove_pud_table(pud_t *pud, unsigned long addr,
				unsigned long end)
{
	/* pmd 指当前片段，pmd_base 用于递归后检查整张私有表是否为空。 */
	unsigned long next;

	for (; addr < end; addr = next, pud++) {
		pmd_t *pmd, *pmd_base;

		next = pud_addr_end(addr, end);

		if (!pud_present(*pud))
			continue;

		if (kasan_pmd_table(*pud)) {
			/* 地址两端覆盖完整 PUD 时清父项即可，静态共享 PMD 不被修改。 */
			if (IS_ALIGNED(addr, PUD_SIZE) &&
			    IS_ALIGNED(next, PUD_SIZE)) {
				pud_clear(pud);
				continue;
			}
		}
		/* 部分覆盖递归到 PMD，并在返回后尝试自底向上回收空表。 */
		pmd = pmd_offset(pud, addr);
		pmd_base = pmd_offset(pud, 0);
		kasan_remove_pmd_table(pmd, addr, next);
		kasan_free_pmd(pmd_base, pud);
	}
}

/*
 * 业务背景：P4D 层拆除零 shadow 子树，为顶层 PGD 遍历提供递归边界。
 * 入参：p4d 为起始借用项；addr/end 为 shadow 半开区间。
 * 出参/返回：无；完整共享 PUD 覆盖直接断链，局部覆盖递归并回收空私有表。
 * 注意事项：必须保留共享静态表内容，因为其他虚拟区间仍可能引用它。
 */
static void kasan_remove_p4d_table(p4d_t *p4d, unsigned long addr,
				unsigned long end)
{
	/* pud 指当前子表；循环按 P4D 边界推进，任何空洞都安全跳过。 */
	unsigned long next;

	for (; addr < end; addr = next, p4d++) {
		pud_t *pud;

		next = p4d_addr_end(addr, end);

		if (!p4d_present(*p4d))
			continue;

		if (kasan_pud_table(*p4d)) {
			/* 完整 P4D 覆盖只清当前父项，不释放或改写共享 PUD 表。 */
			if (IS_ALIGNED(addr, P4D_SIZE) &&
			    IS_ALIGNED(next, P4D_SIZE)) {
				p4d_clear(p4d);
				continue;
			}
		}
		/* 局部覆盖下沉到 PUD，返回后回收不再含任何项的私有表。 */
		pud = pud_offset(p4d, addr);
		kasan_remove_pud_table(pud, addr, next);
		kasan_free_pud(pud_offset(p4d, 0), p4d);
	}
}

/*
 * 业务背景：内存上线或 dev_pagemap 建立真实 shadow 前，撤掉原内存范围对应的零 shadow。
 * 调用链包括 memory_hotplug/memremap → 本函数 → 各级 kasan_remove_*_table()。
 * 入参：start 是原内存虚拟起点的借用地址；size 为原内存字节数，二者均须按
 * KASAN_MEMORY_PER_SHADOW_PAGE 对齐，函数不取得底层内存所有权。
 * 出参/返回：无直接返回值；成功清除 init_mm 中对应 shadow 占位项并回收空私有页表；
 * 对齐错误仅 WARN 后返回，页表保持不变。
 * 注意事项：只允许拆共享零页，遇到真实 shadow 会 WARN 并保留；调用者需在映射生命周期
 * 的串行阶段调用，并负责体系结构要求的 TLB 同步。
 */
void kasan_remove_zero_shadow(void *start, unsigned long size)
{
	/* addr/end 从原内存区间换算到 shadow 半开区间；pgd/next 驱动顶层遍历。 */
	unsigned long addr, end, next;
	pgd_t *pgd;

	addr = (unsigned long)kasan_mem_to_shadow(start);
	end = addr + (size >> KASAN_SHADOW_SCALE_SHIFT);

	/* 一个 shadow 页覆盖固定粒度的原内存；不对齐会导致误删相邻范围的共享项。 */
	if (WARN_ON((unsigned long)start % KASAN_MEMORY_PER_SHADOW_PAGE) ||
	    WARN_ON(size % KASAN_MEMORY_PER_SHADOW_PAGE))
		return;

	/* 逐 PGD 片段拆除；不存在的表项表示该 shadow 区间本就未映射。 */
	for (; addr < end; addr = next) {
		p4d_t *p4d;

		next = pgd_addr_end(addr, end);

		pgd = pgd_offset_k(addr);
		if (!pgd_present(*pgd))
			continue;

		if (kasan_p4d_table(*pgd)) {
			/* 完整 PGD 覆盖可直接断开共享 P4D，静态表继续服务其他地址。 */
			if (IS_ALIGNED(addr, PGDIR_SIZE) &&
			    IS_ALIGNED(next, PGDIR_SIZE)) {
				pgd_clear(pgd);
				continue;
			}
		}

		/* 局部范围递归下沉，最后仅回收已经完全空掉的私有 P4D。 */
		p4d = p4d_offset(pgd, addr);
		kasan_remove_p4d_table(p4d, addr, next);
		kasan_free_p4d(p4d_offset(pgd, 0), pgd);
	}
}

/*
 * 业务背景：内存下线或 dev_pagemap 回滚后，为不再由真实 KASAN shadow 管理的原内存
 * 恢复只读零 shadow；调用链为 hotplug/memremap → 本函数 → kasan_populate_early_shadow()。
 * 入参：start 为原内存虚拟起点借用地址；size 为字节数，二者须按一个 shadow 页可覆盖的
 * 原内存粒度对齐；不转移原内存 ownership。
 * 出参/返回：成功返回 0；对齐错误 -EINVAL；顶层可见的页表分配失败返回 -ENOMEM，
 * 并尽力撤销本次区间中已建立的零 shadow，不影响调用者持有的原内存对象。
 * 注意事项：可能因页表分配睡眠；失败回滚调用 remove，要求该范围进入前没有应保留的
 * 零 shadow 子区间，且调用者串行化映射生命周期。
 */
int kasan_add_zero_shadow(void *start, unsigned long size)
{
	/* shadow_start/end 是由原内存比例缩放得到的页表操作范围。 */
	int ret;
	void *shadow_start, *shadow_end;

	shadow_start = kasan_mem_to_shadow(start);
	shadow_end = shadow_start + (size >> KASAN_SHADOW_SCALE_SHIFT);

	/* 先拒绝不能完整映射为 shadow 页的范围，避免回滚越过调用者边界。 */
	if (WARN_ON((unsigned long)start % KASAN_MEMORY_PER_SHADOW_PAGE) ||
	    WARN_ON(size % KASAN_MEMORY_PER_SHADOW_PAGE))
		return -EINVAL;

	/* populate 可能部分成功；失败时 remove 按同一原内存范围逆向撤销。 */
	ret = kasan_populate_early_shadow(shadow_start, shadow_end);
	if (ret)
		kasan_remove_zero_shadow(start, size);
	return ret;
}
