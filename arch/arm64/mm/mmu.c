// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 内核页表构建、拆分、热插拔与 TTBR1 切换学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件把物理区间映射成内核 VA：启动时建立 idmap、kernel image、linear
 * map、fixmap/vmemmap；运行期支持权限修改前拆 block/contiguous leaf、内存
 * 热插拔建表/撤表以及安全替换 TTBR1。页表构造自顶向下，优先 PUD/PMD
 * block，其次 CONT PMD/PTE，最后基础 PTE，以减少页表页和 TLB 压力。
 *
 * live 映射修改必须满足 break-before-make 或 pgattr_change_is_safe；fixmap
 * 槽是访问尚未进入 linear map 的页表物理页的临时窗口，由 fixmap_lock
 * 串行。只读后的 swapper_pg_dir 通过专用 fixmap 写，受 spinlock 保护。
 * 启动 memblock 分配失败 panic，运行期 GFP 分配失败返回 -ENOMEM 并由上层回滚。
 */
/*
 * Based on arch/arm/mm/mmu.c
 *
 * Copyright (C) 1995-2005 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/cache.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/kexec.h>
#include <linux/libfdt.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/memblock.h>
#include <linux/memremap.h>
#include <linux/memory.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/set_memory.h>
#include <linux/kfence.h>
#include <linux/pkeys.h>
#include <linux/mm_inline.h>
#include <linux/pagewalk.h>
#include <linux/stop_machine.h>

#include <asm/barrier.h>
#include <asm/cputype.h>
#include <asm/fixmap.h>
#include <asm/kasan.h>
#include <asm/kernel-pgtable.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <linux/sizes.h>
#include <asm/tlb.h>
#include <asm/mmu_context.h>
#include <asm/ptdump.h>
#include <asm/tlbflush.h>
#include <asm/pgalloc.h>
#include <asm/kfence.h>

#define NO_BLOCK_MAPPINGS	BIT(0)
#define NO_CONT_MAPPINGS	BIT(1)
#define NO_EXEC_MAPPINGS	BIT(2)	/* assumes FEAT_HPDS is not used */
/* 构建策略位：禁止 block、禁止 CONT hint、或给所有上级 table 设置 PXN。 */

/* 页表 dump 存在时让并发修改路径采用可安全遍历的同步策略。 */
DEFINE_STATIC_KEY_FALSE(arm64_ptdump_lock_key);

/* kernel image VA-PA 偏移，KASLR/重定位后冻结，供 __pa_symbol 等转换。 */
u64 kimage_voffset __ro_after_init;
EXPORT_SYMBOL(kimage_voffset);

/* 启动 CPU 允许的异常级模式表，早期汇编按该顺序记录/校验 EL2/EL1。 */
u32 __boot_cpu_mode[] = { BOOT_CPU_MODE_EL2, BOOT_CPU_MODE_EL1 };

/* true 期间可直接写 swapper_pg_dir；mark_rodata_ro 后必须走 fixmap。 */
static bool rodata_is_rw __ro_after_init = true;

/*
 * The booting CPU updates the failed status @__early_cpu_boot_status,
 * with MMU turned off.
 */
/* 位于 MMU-off 可物理寻址写段，secondary CPU 启动失败时在开 MMU 前更新。 */
long __section(".mmuoff.data.write") __early_cpu_boot_status;

/* 只读后写 swapper 根用 spinlock；所有临时页表 fixmap 槽用可睡眠 mutex 串行。 */
static DEFINE_SPINLOCK(swapper_pgdir_lock);
static DEFINE_MUTEX(fixmap_lock);

/*
 * 原子语义更新一个 swapper PGD。rodata 尚可写时 WRITE_ONCE+DSB/ISB 直接
 * 发布；只读后在 spinlock 下把 pgdp 物理页临时映射可写，写入并 clear
 * fixmap，后者完成所需 TLB/屏障。noinstr 防止插桩递归依赖正在改的页表。
 */
void noinstr set_swapper_pgd(pgd_t *pgdp, pgd_t pgd)
{
	pgd_t *fixmap_pgdp;

	/*
	 * Don't bother with the fixmap if swapper_pg_dir is still mapped
	 * writable in the kernel mapping.
	 */
	/* 启动早期可直接写正式 VA，省去临时槽和锁；rodata 收紧后才走安全别名。 */
	if (rodata_is_rw) {
		WRITE_ONCE(*pgdp, pgd);
		dsb(ishst);
		isb();
		return;
	}

	spin_lock(&swapper_pgdir_lock);
	fixmap_pgdp = pgd_set_fixmap(__pa_symbol(pgdp));
	WRITE_ONCE(*fixmap_pgdp, pgd);
	/*
	 * We need dsb(ishst) here to ensure the page-table-walker sees
	 * our new entry before set_p?d() returns. The fixmap's
	 * flush_tlb_kernel_range() via clear_fixmap() does this for us.
	 */
	/* 清槽既撤销可写别名，也保证 table walker 在返回前观察新根项。 */
	pgd_clear_fixmap();
	spin_unlock(&swapper_pgdir_lock);
}

/*
 * 为 /dev/mem mmap 选择属性。pfn 不属 linear RAM 时强制 noncached；RAM 且
 * O_SYNC 时 writecombine；否则保留用户请求 prot。只返回 pgprot，不建映射。
 */
pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
			      unsigned long size, pgprot_t vma_prot)
{
	if (!pfn_is_map_memory(pfn))
		return pgprot_noncached(vma_prot);
	else if (file->f_flags & O_SYNC)
		return pgprot_writecombine(vma_prot);
	return vma_prot;
}
EXPORT_SYMBOL(phys_mem_access_prot);

/*
 * 启动期页表页 allocator。level 参数为统一回调签名但 memblock 不需区分；
 * 分配一页对齐物理内存，NOLEAKTRACE 避免早期误报。失败无法建立页表而 panic。
 */
static phys_addr_t __init early_pgtable_alloc(enum pgtable_level pgtable_level)
{
	phys_addr_t phys;

	phys = memblock_phys_alloc_range(PAGE_SIZE, PAGE_SIZE, 0,
					 MEMBLOCK_ALLOC_NOLEAKTRACE);
	if (!phys)
		panic("Failed to allocate page table page\n");

	return phys;
}

/*
 * 判断 live leaf 从 old 改 new 是否可不做 BBM。invalid 建/拆总安全；valid
 * 条目必须 PFN 不变、不能 NG->global，只允许 PXN/RO/WRITE/NG/软件位及
 * Normal<->Normal-Tagged 这类权限属性变化。返回 false 要求先 break+TLBI。
 */
bool pgattr_change_is_safe(pteval_t old, pteval_t new)
{
	/*
	 * The following mapping attributes may be updated in live
	 * kernel mappings without the need for break-before-make.
	 */
	/* mask 列出硬件允许 live 更新的权限/软件位，未列字段的变化一律拒绝。 */
	pteval_t mask = PTE_PXN | PTE_RDONLY | PTE_WRITE | PTE_NG |
			PTE_SWBITS_MASK;

	/* creating or taking down mappings is always safe */
	/* 至少一侧 invalid 时不存在两个有效翻译竞争，属于 BBM 的 break 或 make 单边。 */
	if (!pte_valid(__pte(old)) || !pte_valid(__pte(new)))
		return true;

	/* A live entry's pfn should not change */
	/* 同一 VA 的有效 PFN 替换可能与旧 TLB 并存，必须走显式 break+TLBI。 */
	if (pte_pfn(__pte(old)) != pte_pfn(__pte(new)))
		return false;

	/* Transitioning from Non-Global to Global is unsafe */
	/* global 扩大 TLB 跨 ASID 可见性，旧 NG 项仍缓存时可能别名。 */
	if (old & ~new & PTE_NG)
		return false;

	/*
	 * Changing the memory type between Normal and Normal-Tagged is safe
	 * since Tagged is considered a permission attribute from the
	 * mismatched attribute aliases perspective.
	 */
	/* Tagged 仍属 Normal 类型族，架构把差异当权限，允许为 MTE 原地切换。 */
	if (((old & PTE_ATTRINDX_MASK) == PTE_ATTRINDX(MT_NORMAL) ||
	     (old & PTE_ATTRINDX_MASK) == PTE_ATTRINDX(MT_NORMAL_TAGGED)) &&
	    ((new & PTE_ATTRINDX_MASK) == PTE_ATTRINDX(MT_NORMAL) ||
	     (new & PTE_ATTRINDX_MASK) == PTE_ATTRINDX(MT_NORMAL_TAGGED)))
		mask |= PTE_ATTRINDX_MASK;

	return ((old ^ new) & ~mask) == 0;
}

/* 清零新页表页并 DSB ishst 发布，确保 walker 不读到 allocator 旧数据。 */
static void init_clear_pgtable(void *table)
{
	clear_page(table);

	/* Ensure the zeroing is observed by page table walks. */
	/* 新父项发布前先让清零 store 对 inner-shareable walker 可见。 */
	dsb(ishst);
}

/*
 * 填充 [addr,end) 基础 PTE，把 phys 逐页递增。使用 nosync 批量写，屏障延后
 * 到外层清 fixmap；若槽已有 valid 项，只允许 pgattr_change_is_safe 的权限
 * 更新，否则 BUG 暴露调用者试图原地换 PFN/内存类型。
 */
static void init_pte(pte_t *ptep, unsigned long addr, unsigned long end,
		     phys_addr_t phys, pgprot_t prot)
{
	do {
		pte_t old_pte = __ptep_get(ptep);

		/*
		 * Required barriers to make this visible to the table walker
		 * are deferred to the end of alloc_init_cont_pte().
		 */
		/* 批量建表只在整组末尾同步，避免每个 PTE 都支付屏障成本。 */
		__set_pte_nosync(ptep, pfn_pte(__phys_to_pfn(phys), prot));

		/*
		 * After the PTE entry has been populated once, we
		 * only allow updates to the permission attributes.
		 */
		/* 先写后检查依赖此路径仅在构建期/受控更新；失败是不可恢复编程错误。 */
		BUG_ON(!pgattr_change_is_safe(pte_val(old_pte),
					      pte_val(__ptep_get(ptep))));

		phys += PAGE_SIZE;
	} while (ptep++, addr += PAGE_SIZE, addr != end);
}

/* 检查一个 CONT_PTE 对齐组是否已有 valid 非 CONT 项，防止部分组突然置 hint。 */
static bool pte_range_has_valid_noncont(pte_t *ptep)
{
	for (int i = 0; i < CONT_PTES; i++) {
		pte_t pte = __ptep_get(&ptep[i]);

		if (pte_valid(pte) && !pte_cont(pte))
			return true;
	}
	return false;
}

/*
 * 确保 pmdp 下 PTE table 存在，并映射 [addr,end)。新表由 pgtable_alloc
 * 返回物理页，经 fixmap 清零/填充；若 VA/PA/末端整组对齐、允许 CONT 且
 * 无既有非 CONT 项，则整段置 PTE_CONT。返回 0/-ENOMEM，离开前清 fixmap
 * 以发布所有表项。flags 控制 exec/table PXN 与映射粒度。
 */
static int alloc_init_cont_pte(pmd_t *pmdp, unsigned long addr,
			       unsigned long end, phys_addr_t phys,
			       pgprot_t prot,
			       phys_addr_t (*pgtable_alloc)(enum pgtable_level),
			       int flags)
{
	unsigned long next;
	pmd_t pmd = READ_ONCE(*pmdp);
	pte_t *ptep;

	BUG_ON(pmd_leaf(pmd));
	if (pmd_none(pmd)) {
		pmdval_t pmdval = PMD_TYPE_TABLE | PMD_TABLE_UXN | PMD_TABLE_AF;
		phys_addr_t pte_phys;

		if (flags & NO_EXEC_MAPPINGS)
			/* 上级 table PXN 可一次禁止其下所有 privileged execute。 */
			pmdval |= PMD_TABLE_PXN;
		BUG_ON(!pgtable_alloc);
		pte_phys = pgtable_alloc(PGTABLE_LEVEL_PTE);
		if (pte_phys == INVALID_PHYS_ADDR)
			return -ENOMEM;
		ptep = pte_set_fixmap(pte_phys);
		init_clear_pgtable(ptep);
		ptep += pte_index(addr);
		__pmd_populate(pmdp, pte_phys, pmdval);
	} else {
		BUG_ON(pmd_bad(pmd));
		ptep = pte_set_fixmap_offset(pmdp, addr);
	}

	do {
		pgprot_t __prot = prot;

		next = pte_cont_addr_end(addr, end);

		/* use a contiguous mapping if the range is suitably aligned */
		/* 三个边界都必须 CONT_PTE_SIZE 对齐，PFN 才能构成硬件合法连续组。 */
		if ((((addr | next | phys) & ~CONT_PTE_MASK) == 0) &&
		    (flags & NO_CONT_MAPPINGS) == 0 &&
		    !pte_range_has_valid_noncont(ptep))
			__prot = __pgprot(pgprot_val(prot) | PTE_CONT);

		init_pte(ptep, addr, next, phys, __prot);

		ptep += pte_index(next) - pte_index(addr);
		phys += next - addr;
	} while (addr = next, addr != end);

	/*
	 * Note: barriers and maintenance necessary to clear the fixmap slot
	 * ensure that all previous pgtable writes are visible to the table
	 * walker.
	 */
	/* clear_fixmap 是批量写提交点，不可在 init_pte 每项后重复执行。 */
	pte_clear_fixmap();

	return 0;
}

/*
 * 映射一个 PUD 内的 PMD 范围。优先在 VA/PA/范围均 PMD 对齐且允许时建立
 * block；否则下钻 PTE。已有 table 不能被 block 覆盖，已有 leaf 只允许
 * 安全权限更新。返回 0 或下层分配错误。
 */
static int init_pmd(pmd_t *pmdp, unsigned long addr, unsigned long end,
		    phys_addr_t phys, pgprot_t prot,
		    phys_addr_t (*pgtable_alloc)(enum pgtable_level), int flags)
{
	unsigned long next;

	do {
		pmd_t old_pmd = READ_ONCE(*pmdp);

		next = pmd_addr_end(addr, end);

		/* try section mapping first */
		/* block 节省一页 PTE table 和大量 TLB 项，但牺牲逐页属性能力。 */
		if (((addr | next | phys) & ~PMD_MASK) == 0 &&
		    (flags & NO_BLOCK_MAPPINGS) == 0 &&
		    !pmd_table(old_pmd)) {
			WARN_ON(!pmd_set_huge(pmdp, phys, prot));

			/*
			 * After the PMD entry has been populated once, we
			 * only allow updates to the permission attributes.
			 */
			/* 已有 PMD 叶子不可原地改 PFN/类型；违反表示上层遗漏 BBM。 */
			BUG_ON(!pgattr_change_is_safe(pmd_val(old_pmd),
						      READ_ONCE(pmd_val(*pmdp))));
		} else {
			int ret;

			ret = alloc_init_cont_pte(pmdp, addr, next, phys, prot,
						  pgtable_alloc, flags);
			if (ret)
				return ret;

			VM_WARN_ON_ONCE(pmd_val(old_pmd) != 0 &&
					pmd_val(old_pmd) != READ_ONCE(pmd_val(*pmdp)));
		}
		phys += next - addr;
	} while (pmdp++, addr = next, addr != end);

	return 0;
}

/* 检查 CONT_PMDS 组内是否已有 valid 非 contiguous block。 */
static bool pmd_range_has_valid_noncont(pmd_t *pmdp)
{
	for (int i = 0; i < CONT_PMDS; i++) {
		pte_t pte = pmd_pte(READ_ONCE(pmdp[i]));

		if (pte_valid(pte) && !pte_cont(pte))
			return true;
	}
	return false;
}

/*
 * 确保 pudp 下 PMD table 存在并按 CONT_PMD_SIZE 分段调用 init_pmd。满足
 * 对齐/flags/既有项条件时给整个 PMD 组置 CONT；新表通过 fixmap 操作，
 * 所有出口统一 clear。返回 0/-ENOMEM。
 */
static int alloc_init_cont_pmd(pud_t *pudp, unsigned long addr,
			       unsigned long end, phys_addr_t phys,
			       pgprot_t prot,
			       phys_addr_t (*pgtable_alloc)(enum pgtable_level),
			       int flags)
{
	int ret;
	unsigned long next;
	pud_t pud = READ_ONCE(*pudp);
	pmd_t *pmdp;

	/*
	 * Check for initial section mappings in the pgd/pud.
	 */
	/* 上级若已是 leaf，不能无 BBM 直接改成 table，视为调用逻辑 BUG。 */
	BUG_ON(pud_leaf(pud));
	if (pud_none(pud)) {
		pudval_t pudval = PUD_TYPE_TABLE | PUD_TABLE_UXN | PUD_TABLE_AF;
		phys_addr_t pmd_phys;

		if (flags & NO_EXEC_MAPPINGS)
			pudval |= PUD_TABLE_PXN;
		BUG_ON(!pgtable_alloc);
		pmd_phys = pgtable_alloc(PGTABLE_LEVEL_PMD);
		if (pmd_phys == INVALID_PHYS_ADDR)
			return -ENOMEM;
		pmdp = pmd_set_fixmap(pmd_phys);
		init_clear_pgtable(pmdp);
		pmdp += pmd_index(addr);
		__pud_populate(pudp, pmd_phys, pudval);
	} else {
		BUG_ON(pud_bad(pud));
		pmdp = pmd_set_fixmap_offset(pudp, addr);
	}

	do {
		pgprot_t __prot = prot;

		next = pmd_cont_addr_end(addr, end);

		/* use a contiguous mapping if the range is suitably aligned */
		/* 地址/PA/长度整组对齐且没有普通有效 PMD 时，才可给整组设置 CONT。 */
		if ((((addr | next | phys) & ~CONT_PMD_MASK) == 0) &&
		    (flags & NO_CONT_MAPPINGS) == 0 &&
		    !pmd_range_has_valid_noncont(pmdp))
			__prot = __pgprot(pgprot_val(prot) | PTE_CONT);

		ret = init_pmd(pmdp, addr, next, phys, __prot, pgtable_alloc, flags);
		if (ret)
			goto out;

		pmdp += pmd_index(next) - pmd_index(addr);
		phys += next - addr;
	} while (addr = next, addr != end);

out:
	pmd_clear_fixmap();

	return ret;
}

/*
 * PUD 层构建。新 PUD table 按需分配；4K granule 且 1GiB 对齐时优先 PUD
 * block，否则下钻 CONT PMD。已有非零项只可安全权限更新，返回 0/-ENOMEM。
 */
static int alloc_init_pud(p4d_t *p4dp, unsigned long addr, unsigned long end,
			  phys_addr_t phys, pgprot_t prot,
			  phys_addr_t (*pgtable_alloc)(enum pgtable_level),
			  int flags)
{
	int ret = 0;
	unsigned long next;
	p4d_t p4d = READ_ONCE(*p4dp);
	pud_t *pudp;

	if (p4d_none(p4d)) {
		p4dval_t p4dval = P4D_TYPE_TABLE | P4D_TABLE_UXN | P4D_TABLE_AF;
		phys_addr_t pud_phys;

		if (flags & NO_EXEC_MAPPINGS)
			p4dval |= P4D_TABLE_PXN;
		BUG_ON(!pgtable_alloc);
		pud_phys = pgtable_alloc(PGTABLE_LEVEL_PUD);
		if (pud_phys == INVALID_PHYS_ADDR)
			return -ENOMEM;
		pudp = pud_set_fixmap(pud_phys);
		init_clear_pgtable(pudp);
		pudp += pud_index(addr);
		__p4d_populate(p4dp, pud_phys, p4dval);
	} else {
		BUG_ON(p4d_bad(p4d));
		pudp = pud_set_fixmap_offset(p4dp, addr);
	}

	do {
		pud_t old_pud = READ_ONCE(*pudp);

		next = pud_addr_end(addr, end);

		/*
		 * For 4K granule only, attempt to put down a 1GB block
		 */
		/* 其他 granule 的该硬件 level 不支持 block descriptor。 */
		if (pud_sect_supported() &&
		   ((addr | next | phys) & ~PUD_MASK) == 0 &&
		    (flags & NO_BLOCK_MAPPINGS) == 0 &&
		    !pud_table(old_pud)) {
			WARN_ON(!pud_set_huge(pudp, phys, prot));

			/*
			 * After the PUD entry has been populated once, we
			 * only allow updates to the permission attributes.
			 */
			/* PUD 块 live 更新同样只能落在 pgattr_change_is_safe 白名单。 */
			BUG_ON(!pgattr_change_is_safe(pud_val(old_pud),
						      READ_ONCE(pud_val(*pudp))));
		} else {
			ret = alloc_init_cont_pmd(pudp, addr, next, phys, prot,
						  pgtable_alloc, flags);
			if (ret)
				goto out;

			VM_WARN_ON_ONCE(pud_val(old_pud) != 0 &&
					pud_val(old_pud) != READ_ONCE(pud_val(*pudp)));
		}
		phys += next - addr;
	} while (pudp++, addr = next, addr != end);

out:
	pud_clear_fixmap();

	return ret;
}

/* 顶层 PGD->P4D 构建器；支持 5-level/折叠配置并统一用 fixmap 访问新表。 */
static int alloc_init_p4d(pgd_t *pgdp, unsigned long addr, unsigned long end,
			  phys_addr_t phys, pgprot_t prot,
			  phys_addr_t (*pgtable_alloc)(enum pgtable_level),
			  int flags)
{
	int ret;
	unsigned long next;
	pgd_t pgd = READ_ONCE(*pgdp);
	p4d_t *p4dp;

	if (pgd_none(pgd)) {
		pgdval_t pgdval = PGD_TYPE_TABLE | PGD_TABLE_UXN | PGD_TABLE_AF;
		phys_addr_t p4d_phys;

		if (flags & NO_EXEC_MAPPINGS)
			pgdval |= PGD_TABLE_PXN;
		BUG_ON(!pgtable_alloc);
		p4d_phys = pgtable_alloc(PGTABLE_LEVEL_P4D);
		if (p4d_phys == INVALID_PHYS_ADDR)
			return -ENOMEM;
		p4dp = p4d_set_fixmap(p4d_phys);
		init_clear_pgtable(p4dp);
		p4dp += p4d_index(addr);
		__pgd_populate(pgdp, p4d_phys, pgdval);
	} else {
		BUG_ON(pgd_bad(pgd));
		p4dp = p4d_set_fixmap_offset(pgdp, addr);
	}

	do {
		p4d_t old_p4d = READ_ONCE(*p4dp);

		next = p4d_addr_end(addr, end);

		ret = alloc_init_pud(p4dp, addr, next, phys, prot,
				     pgtable_alloc, flags);
		if (ret)
			goto out;

		VM_WARN_ON_ONCE(p4d_val(old_p4d) != 0 &&
				p4d_val(old_p4d) != READ_ONCE(p4d_val(*p4dp)));

		phys += next - addr;
	} while (p4dp++, addr = next, addr != end);

out:
	p4d_clear_fixmap();

	return ret;
}

/*
 * 已持 fixmap_lock 时在任意 pgdir 建 [virt,virt+size) -> phys 映射。物理和
 * 虚拟起点页内偏移必须相同；函数向下页对齐、末端向上对齐并逐 PGD 递归。
 * pgtable_alloc 可在需要新中间表时调用，返回 0/-EINVAL/-ENOMEM。
 */
static int __create_pgd_mapping_locked(pgd_t *pgdir, phys_addr_t phys,
				       unsigned long virt, phys_addr_t size,
				       pgprot_t prot,
				       phys_addr_t (*pgtable_alloc)(enum pgtable_level),
				       int flags)
{
	int ret;
	unsigned long addr, end, next;
	pgd_t *pgdp = pgd_offset_pgd(pgdir, virt);

	/*
	 * If the virtual and physical address don't have the same offset
	 * within a page, we cannot map the region as the caller expects.
	 */
	/* 不同页内偏移无法由同一页粒度映射同时保留，必须拒绝而非悄悄扩大错位。 */
	if (WARN_ON((phys ^ virt) & ~PAGE_MASK))
		return -EINVAL;

	phys &= PAGE_MASK;
	addr = virt & PAGE_MASK;
	end = PAGE_ALIGN(virt + size);

	do {
		next = pgd_addr_end(addr, end);
		ret = alloc_init_p4d(pgdp, addr, next, phys, prot, pgtable_alloc,
				     flags);
		if (ret)
			return ret;
		phys += next - addr;
	} while (pgdp++, addr = next, addr != end);

	return 0;
}

/* 获取 fixmap_lock 的公共内部包装，串行所有共享临时页表映射槽。 */
static int __create_pgd_mapping(pgd_t *pgdir, phys_addr_t phys,
				unsigned long virt, phys_addr_t size,
				pgprot_t prot,
				phys_addr_t (*pgtable_alloc)(enum pgtable_level),
				int flags)
{
	int ret;

	mutex_lock(&fixmap_lock);
	ret = __create_pgd_mapping_locked(pgdir, phys, virt, size, prot,
					  pgtable_alloc, flags);
	mutex_unlock(&fixmap_lock);

	return ret;
}

/* 启动关键映射包装：任何构建错误都 panic，因为无可用地址空间继续启动。 */
static void early_create_pgd_mapping(pgd_t *pgdir, phys_addr_t phys,
				     unsigned long virt, phys_addr_t size,
				     pgprot_t prot,
				     phys_addr_t (*pgtable_alloc)(enum pgtable_level),
				     int flags)
{
	int ret;

	ret = __create_pgd_mapping(pgdir, phys, virt, size, prot, pgtable_alloc,
				   flags);
	if (ret)
		panic("Failed to create page tables\n");
}

/*
 * 运行期为 mm 分配指定层页表页。去掉 __GFP_ZERO，因为 init_clear_pgtable
 * 随后统一清零；按层调用 ptdesc ctor 建立页表记账/锁元数据。成功返回
 * 物理地址，失败 INVALID_PHYS_ADDR；页面所有权进入 mm 页表树。
 */
static phys_addr_t __pgd_pgtable_alloc(struct mm_struct *mm, gfp_t gfp,
				       enum pgtable_level pgtable_level)
{
	/* Page is zeroed by init_clear_pgtable() so don't duplicate effort. */
	/* 避免 allocator 与 fixmap 初始化各清一次页；发布屏障由后者统一负责。 */
	struct ptdesc *ptdesc = pagetable_alloc(gfp & ~__GFP_ZERO, 0);
	phys_addr_t pa;

	if (!ptdesc)
		return INVALID_PHYS_ADDR;

	pa = page_to_phys(ptdesc_page(ptdesc));

	switch (pgtable_level) {
	case PGTABLE_LEVEL_PTE:
		BUG_ON(!pagetable_pte_ctor(mm, ptdesc));
		break;
	case PGTABLE_LEVEL_PMD:
		BUG_ON(!pagetable_pmd_ctor(mm, ptdesc));
		break;
	case PGTABLE_LEVEL_PUD:
		pagetable_pud_ctor(ptdesc);
		break;
	case PGTABLE_LEVEL_P4D:
		pagetable_p4d_ctor(ptdesc);
		break;
	case PGTABLE_LEVEL_PGD:
		VM_WARN_ON(1);
		break;
	}

	return pa;
}

/* 为 init_mm 分配指定层页表，调用者可传 GFP_ATOMIC 或可睡眠的 GFP 策略。 */
static phys_addr_t
pgd_pgtable_alloc_init_mm_gfp(enum pgtable_level pgtable_level, gfp_t gfp)
{
	/* init_mm 是内核全局地址空间；gfp 由拆分路径决定能否睡眠。 */
	return __pgd_pgtable_alloc(&init_mm, gfp, pgtable_level);
}

/* init_mm 的普通可睡眠分配器，供启动后创建内核页表层级使用。 */
static phys_addr_t __maybe_unused
pgd_pgtable_alloc_init_mm(enum pgtable_level pgtable_level)
{
	return pgd_pgtable_alloc_init_mm_gfp(pgtable_level, GFP_PGTABLE_KERNEL);
}

static phys_addr_t
pgd_pgtable_alloc_special_mm(enum pgtable_level pgtable_level)
{
	/* 独立/临时 pgdir 不归某个 mm 记账，因此 ctor 接收 NULL。 */
	return  __pgd_pgtable_alloc(NULL, GFP_PGTABLE_KERNEL, pgtable_level);
}

/*
 * 将一个硬件 contiguous PTE 组降级为彼此独立的普通 PTE。硬件把 CONT_PTES
 * 个带 PTE_CONT 的项视作一个更大映射，故必须从组首遍历并一致清位，不能只改
 * 命中的一个项。调用者负责串行、BBM 条件和之后必要的 TLB 处理。
 */
static void split_contpte(pte_t *ptep)
{
	int i;

	ptep = PTR_ALIGN_DOWN(ptep, sizeof(*ptep) * CONT_PTES);
	for (i = 0; i < CONT_PTES; i++, ptep++)
		__set_pte(ptep, pte_mknoncont(__ptep_get(ptep)));
}

/*
 * 把一个 PMD 块映射替换成一页 PTE 表，保持 PFN 连续和原权限不变。
 * to_cont=true 时生成 contiguous PTE 组，作为“PMD 块 -> contpte -> PTE”
 * 的渐进拆分中间态。先完整填子表、dsb 发布，再让父 PMD 指向它，避免
 * table walker 看见半初始化页表。成功后新页表所有权归 init_mm。
 */
static int split_pmd(pmd_t *pmdp, pmd_t pmd, gfp_t gfp, bool to_cont)
{
	pmdval_t tableprot = PMD_TYPE_TABLE | PMD_TABLE_UXN | PMD_TABLE_AF;
	unsigned long pfn = pmd_pfn(pmd);
	pgprot_t prot = pmd_pgprot(pmd);
	phys_addr_t pte_phys;
	pte_t *ptep;
	int i;

	pte_phys = pgd_pgtable_alloc_init_mm_gfp(PGTABLE_LEVEL_PTE, gfp);
	if (pte_phys == INVALID_PHYS_ADDR)
		return -ENOMEM;
	ptep = (pte_t *)phys_to_virt(pte_phys);

	/* 块的 PXN 必须上提到 table 属性，约束其下所有叶子。 */
	if (pgprot_val(prot) & PMD_SECT_PXN)
		tableprot |= PMD_TABLE_PXN;

	prot = __pgprot((pgprot_val(prot) & ~PTE_TYPE_MASK) | PTE_TYPE_PAGE);
	/* 无效但带软件权限的叶子也要保留 invalid 状态，不能意外重新映射。 */
	if (!pmd_valid(pmd))
		prot = pte_pgprot(pte_mkinvalid(pfn_pte(0, prot)));
	prot = __pgprot(pgprot_val(prot) & ~PTE_CONT);
	if (to_cont)
		prot = __pgprot(pgprot_val(prot) | PTE_CONT);

	for (i = 0; i < PTRS_PER_PTE; i++, ptep++, pfn++)
		__set_pte(ptep, pfn_pte(pfn, prot));

	/*
	 * Ensure the pte entries are visible to the table walker by the time
	 * the pmd entry that points to the ptes is visible.
	 */
	/* DSB ishst 建立“子 PTE 全部初始化”先于“父 PMD 发布”的硬件可见顺序。 */
	dsb(ishst);
	__pmd_populate(pmdp, pte_phys, tableprot);

	return 0;
}

/* 清除整组 PMD 的 contiguous 提示；规则与 split_contpte 相同。 */
static void split_contpmd(pmd_t *pmdp)
{
	int i;

	pmdp = PTR_ALIGN_DOWN(pmdp, sizeof(*pmdp) * CONT_PMDS);
	for (i = 0; i < CONT_PMDS; i++, pmdp++)
		set_pmd(pmdp, pmd_mknoncont(pmdp_get(pmdp)));
}

/*
 * 把 PUD 块展开为一页 PMD 表。每个 PMD 的 PFN 前进 PMD_SIZE，属性由
 * PUD 叶子转换而来；to_cont 决定子项是否先组成 contpmd。发布顺序同
 * split_pmd：子表内容在父表项可见前必须完成。
 */
static int split_pud(pud_t *pudp, pud_t pud, gfp_t gfp, bool to_cont)
{
	pudval_t tableprot = PUD_TYPE_TABLE | PUD_TABLE_UXN | PUD_TABLE_AF;
	unsigned int step = PMD_SIZE >> PAGE_SHIFT;
	unsigned long pfn = pud_pfn(pud);
	pgprot_t prot = pud_pgprot(pud);
	phys_addr_t pmd_phys;
	pmd_t *pmdp;
	int i;

	pmd_phys = pgd_pgtable_alloc_init_mm_gfp(PGTABLE_LEVEL_PMD, gfp);
	if (pmd_phys == INVALID_PHYS_ADDR)
		return -ENOMEM;
	pmdp = (pmd_t *)phys_to_virt(pmd_phys);

	if (pgprot_val(prot) & PMD_SECT_PXN)
		tableprot |= PUD_TABLE_PXN;

	prot = __pgprot((pgprot_val(prot) & ~PMD_TYPE_MASK) | PMD_TYPE_SECT);
	if (!pud_valid(pud))
		prot = pmd_pgprot(pmd_mkinvalid(pfn_pmd(0, prot)));
	prot = __pgprot(pgprot_val(prot) & ~PTE_CONT);
	if (to_cont)
		prot = __pgprot(pgprot_val(prot) | PTE_CONT);

	for (i = 0; i < PTRS_PER_PMD; i++, pmdp++, pfn += step)
		set_pmd(pmdp, pfn_pmd(pfn, prot));

	/*
	 * Ensure the pmd entries are visible to the table walker by the time
	 * the pud entry that points to the pmds is visible.
	 */
	/* 同理，父 PUD 能被 walker 解引用前，整张 PMD 子表必须已经对其可见。 */
	dsb(ishst);
	__pud_populate(pudp, pmd_phys, tableprot);

	return 0;
}

/*
 * 将 addr 所在的内核叶子映射只拆到“addr 成为边界”所需的最细层级。
 * 例：从一个 PUD 块中单独改一页权限时，依次 PUD->contpmd、拆 contiguous
 * PMD、PMD->contpte、拆 contiguous PTE，最终目标页可独立修改。
 *
 * 返回 0 表示无需动作或成功，-ENOMEM 表示建子表失败。调用者必须持有
 * pgtable_split_lock 并开启 lazy MMU mode；本函数可能分配并睡眠。
 */
static int split_kernel_leaf_mapping_locked(unsigned long addr)
{
	pgd_t *pgdp, pgd;
	p4d_t *p4dp, p4d;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep, pte;
	int ret = 0;

	/*
	 * PGD: If addr is PGD aligned then addr already describes a leaf
	 * boundary. If not present then there is nothing to split.
	 */
	/* 对齐到 PGD 边界无需拆上层；根项为空也说明目标范围尚未映射。 */
	if (ALIGN_DOWN(addr, PGDIR_SIZE) == addr)
		goto out;
	pgdp = pgd_offset_k(addr);
	pgd = pgdp_get(pgdp);
	if (!pgd_present(pgd))
		goto out;

	/*
	 * P4D: If addr is P4D aligned then addr already describes a leaf
	 * boundary. If not present then there is nothing to split.
	 */
	/* P4D 层应用同一“已是边界或无映射即停止”的递归终止条件。 */
	if (ALIGN_DOWN(addr, P4D_SIZE) == addr)
		goto out;
	p4dp = p4d_offset(pgdp, addr);
	p4d = p4dp_get(p4dp);
	if (!p4d_present(p4d))
		goto out;

	/*
	 * PUD: If addr is PUD aligned then addr already describes a leaf
	 * boundary. If not present then there is nothing to split. Otherwise,
	 * if we have a pud leaf, split to contpmd.
	 */
	/* addr 落在 PUD 块内部时先展开为 contpmd，保留大粒度而获得下一层边界。 */
	if (ALIGN_DOWN(addr, PUD_SIZE) == addr)
		goto out;
	pudp = pud_offset(p4dp, addr);
	pud = pudp_get(pudp);
	if (!pud_present(pud))
		goto out;
	if (pud_leaf(pud)) {
		ret = split_pud(pudp, pud, GFP_PGTABLE_KERNEL, true);
		if (ret)
			goto out;
	}

	/*
	 * CONTPMD: If addr is CONTPMD aligned then addr already describes a
	 * leaf boundary. If not present then there is nothing to split.
	 * Otherwise, if we have a contpmd leaf, split to pmd.
	 */
	/* 若仍落在 CONT_PMD 组内部，先整组清 CONT；不存在项则无需继续。 */
	if (ALIGN_DOWN(addr, CONT_PMD_SIZE) == addr)
		goto out;
	pmdp = pmd_offset(pudp, addr);
	pmd = pmdp_get(pmdp);
	if (!pmd_present(pmd))
		goto out;
	if (pmd_leaf(pmd)) {
		if (pmd_cont(pmd))
			split_contpmd(pmdp);
		/*
		 * PMD: If addr is PMD aligned then addr already describes a
		 * leaf boundary. Otherwise, split to contpte.
		 */
		/* PMD 内部地址需要把块叶子展开到 contpte，下一步再按需要解组。 */
		if (ALIGN_DOWN(addr, PMD_SIZE) == addr)
			goto out;
		ret = split_pmd(pmdp, pmd, GFP_PGTABLE_KERNEL, true);
		if (ret)
			goto out;
	}

	/*
	 * CONTPTE: If addr is CONTPTE aligned then addr already describes a
	 * leaf boundary. If not present then there is nothing to split.
	 * Otherwise, if we have a contpte leaf, split to pte.
	 */
	/* 最后一层只需在地址切开 CONT_PTE 组时清整组 CONT 位。 */
	if (ALIGN_DOWN(addr, CONT_PTE_SIZE) == addr)
		goto out;
	ptep = pte_offset_kernel(pmdp, addr);
	pte = __ptep_get(ptep);
	if (!pte_present(pte))
		goto out;
	if (pte_cont(pte))
		split_contpte(ptep);

out:
	return ret;
}

static inline bool force_pte_mapping(void)
{
	/*
	 * 没有 BBML2_NOABORT 时，运行期改变块大小可能让并发 table walk abort；
	 * debug_pagealloc/KFENCE/realm/rodata_full 又需要细粒度改权限，因此启动时
	 * 直接使用 PTE。具备 BBML2 时则可保留大块映射，按需安全拆分。
	 */
	const bool bbml2 = system_capabilities_finalized() ?
		system_supports_bbml2_noabort() : cpu_supports_bbml2_noabort();

	if (debug_pagealloc_enabled())
		return true;
	if (bbml2)
		return false;
	return rodata_full || arm64_kfence_can_set_direct_map() || is_realm_world();
}

static DEFINE_MUTEX(pgtable_split_lock);
/* true 表示线性映射保留了块/contiguous 项，运行期拆分依赖 BBML2 语义。 */
static bool linear_map_requires_bbml2;

/*
 * 为 [start,end) 的权限变更准备叶子粒度。常见调用是 set_memory_ro() 修改
 * 单页：若线性映射原为 PMD 块，先拆父叶子，随后通用改权限代码才可定位 PTE。
 * 已全 PTE 化时是快速空操作；不支持安全 live split 的系统在能力确定后拒绝
 * 拆分。锁保护页表结构变化，lazy mode 将成批架构同步推迟到临界区末尾。
 */
int split_kernel_leaf_mapping(unsigned long start, unsigned long end)
{
	int ret;

	/*
	 * If the region is within a pte-mapped area, there is no need to try to
	 * split. Additionally, CONFIG_DEBUG_PAGEALLOC and CONFIG_KFENCE may
	 * change permissions from atomic context so for those cases (which are
	 * always pte-mapped), we must not go any further because taking the
	 * mutex below may sleep. Do not call force_pte_mapping() here because
	 * it could return a confusing result if called from a secondary cpu
	 * prior to finalizing caps. Instead, linear_map_requires_bbml2 gives us
	 * what we need.
	 */
	/* 已知全 PTE 的原子调用场景必须在取可睡眠 mutex 前返回；判断只用稳定缓存状态。 */
	if (!linear_map_requires_bbml2 || is_kfence_address((void *)start))
		return 0;

	if (!system_supports_bbml2_noabort()) {
		/*
		 * !BBML2_NOABORT systems should not be trying to change
		 * permissions on anything that is not pte-mapped in the first
		 * place. Just return early and let the permission change code
		 * raise a warning if not already pte-mapped.
		 */
		/* 能力最终确定且无 no-abort 保证时禁止 live 改层级，让后续权限代码告警。 */
		if (system_capabilities_finalized())
			return 0;

		/*
		 * Boot-time: split_kernel_leaf_mapping_locked() allocates from
		 * page allocator. Can't split until it's available.
		 */
		/* 子页表分配依赖 buddy；page_alloc_available 发布前只能返回 -EBUSY。 */
		if (WARN_ON(!page_alloc_available))
			return -EBUSY;

		/*
		 * Boot-time: Started secondary cpus but don't know if they
		 * support BBML2_NOABORT yet. Can't allow splitting in this
		 * window in case they don't.
		 */
		/* 多 CPU 已启动但 capability 尚未汇总时，任何 live split 都可能伤害弱 CPU。 */
		if (WARN_ON(num_online_cpus() > 1))
			return -EBUSY;
	}

	/*
	 * Ensure start and end are at least page-aligned since this is the
	 * finest granularity we can split to.
	 */
	/* PTE 是最小叶子，非页对齐端点没有可表达的独立映射边界。 */
	if (start != PAGE_ALIGN(start) || end != PAGE_ALIGN(end))
		return -EINVAL;

	mutex_lock(&pgtable_split_lock);
	lazy_mmu_mode_enable();

	/*
	 * The split_kernel_leaf_mapping_locked() may sleep, it is not a
	 * problem for ARM64 since ARM64's lazy MMU implementation allows
	 * sleeping.
	 *
	 * Optimize for the common case of splitting out a single page from a
	 * larger mapping. Here we can just split on the "least aligned" of
	 * start and end and this will guarantee that there must also be a split
	 * on the more aligned address since the both addresses must be in the
	 * same contpte block and it must have been split to ptes.
	 */
	/* 单页只拆对齐度更低的那个端点即可同时形成另一端边界，减少一次页表遍历。 */
	if (end - start == PAGE_SIZE) {
		start = __ffs(start) < __ffs(end) ? start : end;
		ret = split_kernel_leaf_mapping_locked(start);
	} else {
		ret = split_kernel_leaf_mapping_locked(start);
		if (!ret)
			ret = split_kernel_leaf_mapping_locked(end);
	}

	lazy_mmu_mode_disable();
	mutex_unlock(&pgtable_split_lock);
	return ret;
}

/* 页表 walk 回调：遇到 PUD 叶子就直接展开为普通 PMD（不制造 cont 项）。 */
static int split_to_ptes_pud_entry(pud_t *pudp, unsigned long addr,
				   unsigned long next, struct mm_walk *walk)
{
	gfp_t gfp = *(gfp_t *)walk->private;
	pud_t pud = pudp_get(pudp);
	int ret = 0;

	if (pud_leaf(pud))
		ret = split_pud(pudp, pud, gfp, false);

	return ret;
}

/* 页表 walk 回调：PMD/contpmd 叶子最终展开到普通 PTE。 */
static int split_to_ptes_pmd_entry(pmd_t *pmdp, unsigned long addr,
				   unsigned long next, struct mm_walk *walk)
{
	gfp_t gfp = *(gfp_t *)walk->private;
	pmd_t pmd = pmdp_get(pmdp);
	int ret = 0;

	if (pmd_leaf(pmd)) {
		if (pmd_cont(pmd))
			split_contpmd(pmdp);
		ret = split_pmd(pmdp, pmd, gfp, false);

		/*
		 * We have split the pmd directly to ptes so there is no need to
		 * visit each pte to check if they are contpte.
		 */
		/* split_pmd(..., false) 已生成普通 PTE，提示 walker 跳过冗余叶子回调。 */
		walk->action = ACTION_CONTINUE;
	}

	return ret;
}

/* 页表 walk 回调：已经到 PTE 层时只需解散 contiguous PTE 组。 */
static int split_to_ptes_pte_entry(pte_t *ptep, unsigned long addr,
				   unsigned long next, struct mm_walk *walk)
{
	pte_t pte = __ptep_get(ptep);

	if (pte_cont(pte))
		split_contpte(ptep);

	return 0;
}

static const struct mm_walk_ops split_to_ptes_ops = {
	.pud_entry	= split_to_ptes_pud_entry,
	.pmd_entry	= split_to_ptes_pmd_entry,
	.pte_entry	= split_to_ptes_pte_entry,
};

/*
 * 使用无锁内核页表 walker 将整个范围规范化为普通 PTE。结构修改的互斥由
 * 外层场景保证；gfp 通过 walk->private 传给可能发生的子页表分配。
 */
static int range_split_to_ptes(unsigned long start, unsigned long end, gfp_t gfp)
{
	int ret;

	lazy_mmu_mode_enable();
	ret = walk_kernel_page_table_range_lockless(start, end,
					&split_to_ptes_ops, NULL, &gfp);
	lazy_mmu_mode_disable();

	return ret;
}

/*
 * stop_machine 汇合变量：非启动 CPU 在 idmap 中等待 CPU0 完成线性映射拆分；
 * 汇编等待代码也访问它，故非 static，并用 READ/WRITE_ONCE 配合屏障发布。
 */
u32 idmap_kpti_bbml2_flag;

static void __init init_idmap_kpti_bbml2_flag(void)
{
	WRITE_ONCE(idmap_kpti_bbml2_flag, 1);
	/* Must be visible to other CPUs before stop_machine() is called. */
	/* 全屏障保证 stop_machine 辅助 CPU 进入汇合协议前能观察 flag 初值。 */
	smp_mb();
}

/*
 * stop_machine 回调。CPU0 是唯一预先确认支持 BBML2 的 CPU，由它拆除除内核
 * 固定 alias 外的线性映射大页；其他 CPU 切到 idmap 等待，避免用旧 init_mm
 * 页表并发访问正在改变层级的映射。完成 TLB 刷新后 flag 清零释放等待者。
 */
static int __init linear_map_split_to_ptes(void *__unused)
{
	/*
	 * Repainting the linear map must be done by CPU0 (the boot CPU) because
	 * that's the only CPU that we know supports BBML2. The other CPUs will
	 * be held in a waiting area with the idmap active.
	 */
	/* 仅 CPU0 的 BBML2 能力在此刻可信；其他 CPU 必须脱离 init_mm 等待。 */
	if (!smp_processor_id()) {
		unsigned long lstart = _PAGE_OFFSET(vabits_actual);
		unsigned long lend = PAGE_END;
		unsigned long kstart = (unsigned long)lm_alias(_stext);
		unsigned long kend = (unsigned long)lm_alias(__init_begin);
		int ret;

		/*
		 * Wait for all secondary CPUs to be put into the waiting area.
		 */
		/* acquire 等待计数达到在线 CPU 数，也接收各 CPU 已切入 idmap 的发布。 */
		smp_cond_load_acquire(&idmap_kpti_bbml2_flag, VAL == num_online_cpus());

		/*
		 * Walk all of the linear map [lstart, lend), except the kernel
		 * linear map alias [kstart, kend), and split all mappings to
		 * PTE. The kernel alias remains static throughout runtime so
		 * can continue to be safely mapped with large mappings.
		 */
		/* 跳过永不动态改权限的镜像 alias，保留其大页以节约 TLB 和页表。 */
		ret = range_split_to_ptes(lstart, kstart, GFP_ATOMIC);
		if (!ret)
			ret = range_split_to_ptes(kend, lend, GFP_ATOMIC);
		if (ret)
			panic("Failed to split linear map\n");
		flush_tlb_kernel_range(lstart, lend);

		/*
		 * Relies on dsb in flush_tlb_kernel_range() to avoid reordering
		 * before any page table split operations.
		 */
		/* TLBI 内含 DSB；在它完成前不能清 flag 让辅助 CPU 恢复使用 init_mm。 */
		WRITE_ONCE(idmap_kpti_bbml2_flag, 0);
	} else {
		typedef void (wait_split_fn)(void);
		extern wait_split_fn wait_linear_map_split_to_ptes;
		wait_split_fn *wait_fn;

		wait_fn = (void *)__pa_symbol(wait_linear_map_split_to_ptes);

		/*
		 * At least one secondary CPU doesn't support BBML2 so cannot
		 * tolerate the size of the live mappings changing. So have the
		 * secondary CPUs wait for the boot CPU to make the changes
		 * with the idmap active and init_mm inactive.
		 */
		/* 辅助 CPU 通过物理地址调用汇编等待函数，期间 TTBR1 不指向被修改页表。 */
		cpu_install_idmap();
		wait_fn();
		cpu_uninstall_idmap();
	}

	return 0;
}

/* 能力汇总后发现任一 CPU 不支持 BBML2 时，把线性映射一次性降到 PTE。 */
void __init linear_map_maybe_split_to_ptes(void)
{
	if (linear_map_requires_bbml2 && !system_supports_bbml2_noabort()) {
		init_idmap_kpti_bbml2_flag();
		stop_machine(linear_map_split_to_ptes, NULL, cpu_online_mask);
	}
}

/*
 * This function can only be used to modify existing table entries,
 * without allocating new levels of table. Note that this permits the
 * creation of new section or page entries.
 */
/* 只能利用已存在的中间表，pgtable_alloc=NULL；缺层级会导致启动失败。 */
void __init create_mapping_noalloc(phys_addr_t phys, unsigned long virt,
				   phys_addr_t size, pgprot_t prot)
{
	if (virt < PAGE_OFFSET) {
		pr_warn("BUG: not creating mapping for %pa at 0x%016lx - outside kernel range\n",
			&phys, virt);
		return;
	}
	early_create_pgd_mapping(init_mm.pgd, phys, virt, size, prot, NULL, 0);
}

/*
 * 为非 init_mm 的特殊地址空间建立映射并允许分配完整层级。典型例子是 idmap
 * 或临时页表；page_mappings_only 用于要求每页独立控制权限的区域。
 */
void __init create_pgd_mapping(struct mm_struct *mm, phys_addr_t phys,
			       unsigned long virt, phys_addr_t size,
			       pgprot_t prot, bool page_mappings_only)
{
	int flags = 0;

	BUG_ON(mm == &init_mm);

	if (page_mappings_only)
		flags = NO_BLOCK_MAPPINGS | NO_CONT_MAPPINGS;

	early_create_pgd_mapping(mm->pgd, phys, virt, size, prot,
				 pgd_pgtable_alloc_special_mm, flags);
}

/* 只重写已有内核映射属性，不分配层级；修改 live 页表后立即刷新目标 TLB。 */
static void update_mapping_prot(phys_addr_t phys, unsigned long virt,
				phys_addr_t size, pgprot_t prot)
{
	if (virt < PAGE_OFFSET) {
		pr_warn("BUG: not updating mapping for %pa at 0x%016lx - outside kernel range\n",
			&phys, virt);
		return;
	}

	early_create_pgd_mapping(init_mm.pgd, phys, virt, size, prot, NULL, 0);

	/* flush the TLBs after updating live kernel mappings */
	/* 属性写入 live init_mm 后必须立即按范围失效旧权限翻译。 */
	flush_tlb_kernel_range(virt, virt + size);
}

/* 把 memblock 物理区间按线性映射公式装入 swapper_pg_dir。 */
static void __init __map_memblock(phys_addr_t start, phys_addr_t end,
				  pgprot_t prot, int flags)
{
	early_create_pgd_mapping(swapper_pg_dir, start, __phys_to_virt(start),
				 end - start, prot, early_pgtable_alloc, flags);
}

void __init mark_linear_text_alias_ro(void)
{
	/*
	 * Remove the write permissions from the linear alias of .text/.rodata
	 */
	/* 镜像 alias 与 linear alias 指向同一 PA，两者都需收紧才真正阻止旁路写入。 */
	update_mapping_prot(__pa_symbol(_text), (unsigned long)lm_alias(_text),
			    (unsigned long)__init_begin - (unsigned long)_text,
			    PAGE_KERNEL_RO);
}

#ifdef CONFIG_KFENCE

/* 启动阶段是否预留并按 PTE 映射 KFENCE 池；初始化完成后冻结。 */
bool __ro_after_init kfence_early_init = !!CONFIG_KFENCE_SAMPLE_INTERVAL;

/* early_param() will be parsed before map_mem() below. */
/* 因此命令行可在物理池预留和线性映射布局确定前改变 early KFENCE 策略。 */
static int __init parse_kfence_early_init(char *arg)
{
	int val;

	if (get_option(&arg, &val))
		kfence_early_init = !!val;
	return 0;
}
early_param("kfence.sample_interval", parse_kfence_early_init);

/*
 * early KFENCE 必须在 map_mem 的大块映射前抢占物理池并单独建立 PTE 映射，
 * 否则后续 guard page 开关权限需要先拆块，早期阶段未必具备相应条件。
 */
static void __init arm64_kfence_map_pool(void)
{
	phys_addr_t kfence_pool;

	if (!kfence_early_init)
		return;

	kfence_pool = memblock_phys_alloc(KFENCE_POOL_SIZE, PAGE_SIZE);
	if (!kfence_pool) {
		pr_err("failed to allocate kfence pool\n");
		kfence_early_init = false;
		return;
	}

	/* KFENCE pool needs page-level mapping. */
	/* guard page 会频繁单页开关权限，若使用块/CONT 映射便无法独立控制。 */
	__map_memblock(kfence_pool, kfence_pool + KFENCE_POOL_SIZE,
			pgprot_tagged(PAGE_KERNEL),
			NO_BLOCK_MAPPINGS | NO_CONT_MAPPINGS | NO_EXEC_MAPPINGS);
	__kfence_pool = phys_to_virt(kfence_pool);
}

/*
 * 延迟启用 KFENCE 时保证池区间已是 PTE 粒度。若启动策略本就全 PTE 或 early
 * 池已处理则直接成功；否则在拆分锁下 walk。返回 false 让 KFENCE 放弃启用。
 */
bool arch_kfence_init_pool(void)
{
	unsigned long start = (unsigned long)__kfence_pool;
	unsigned long end = start + KFENCE_POOL_SIZE;
	int ret;

	/* Exit early if we know the linear map is already pte-mapped. */
	/* force_pte_mapping 已保证目标粒度，无需再次 walk 和分配子表。 */
	if (force_pte_mapping())
		return true;

	/* Kfence pool is already pte-mapped for the early init case. */
	/* early 路径在 map_mem 前单独映射过池，避免重复拆分。 */
	if (kfence_early_init)
		return true;

	mutex_lock(&pgtable_split_lock);
	ret = range_split_to_ptes(start, end, GFP_PGTABLE_KERNEL);
	mutex_unlock(&pgtable_split_lock);

	/*
	 * Since the system supports bbml2_noabort, tlb invalidation is not
	 * required here; the pgtable mappings have been split to pte but larger
	 * entries may safely linger in the TLB.
	 */
	/* no-abort 语义允许旧大页 TLB 与新 PTE 表短暂并存，后续权限变更会处理目标项。 */

	return !ret;
}
#else /* CONFIG_KFENCE */

static inline void arm64_kfence_map_pool(void) { }

#endif /* CONFIG_KFENCE */

/*
 * 构造 arm64 线性映射的主流程。先单映内核 text alias 与 data/bss，保证日后
 * 能独立收紧权限；再遍历 memblock RAM bank 映射全部物理内存。默认禁止执行，
 * MTE 系统使用 tagged 属性。force_pte_mapping 决定空间效率更高的块/contiguous
 * 映射，还是支持逐页调试和权限切换的 PTE 映射。
 */
static void __init map_mem(void)
{
	static const u64 direct_map_end = _PAGE_END(VA_BITS_MIN);
	phys_addr_t kernel_start = __pa_symbol(_text);
	phys_addr_t init_begin = __pa_symbol(__init_begin);
	phys_addr_t init_end = __pa_symbol(__init_end);
	phys_addr_t kernel_end = __pa_symbol(__bss_stop);
	phys_addr_t start, end;
	int flags = NO_EXEC_MAPPINGS;
	u64 i;

	/*
	 * Setting hierarchical PXNTable attributes on table entries covering
	 * the linear region is only possible if it is guaranteed that no table
	 * entries at any level are being shared between the linear region and
	 * the vmalloc region. Check whether this is true for the PGD level, in
	 * which case it is guaranteed to be true for all other levels as well.
	 * (Unless we are running with support for LPA2, in which case the
	 * entire reduced VA space is covered by a single pgd_t which will have
	 * been populated without the PXNTable attribute by the time we get here.)
	 */
	/* 该 BUILD_BUG_ON 证明 linear/vmalloc 不共享根项，才可用 PXNTable 向下施加 NX。 */
	BUILD_BUG_ON(pgd_index(direct_map_end - 1) == pgd_index(direct_map_end) &&
		     pgd_index(_PAGE_OFFSET(VA_BITS_MIN)) != PTRS_PER_PGD - 1);

	arm64_kfence_map_pool();

	linear_map_requires_bbml2 = !force_pte_mapping() && can_set_direct_map();

	if (force_pte_mapping())
		flags |= NO_BLOCK_MAPPINGS | NO_CONT_MAPPINGS;

	/*
	 * Map the linear alias of the [_text, __init_begin) interval first
	 * so that its write permissions can be removed later without the need
	 * to split any block mappings created by the loop below.
	 *
	 * Write permissions are needed for alternatives patching, and will be
	 * removed later by mark_linear_text_alias_ro() above. This makes the
	 * contents of the region accessible to subsystems such as hibernate,
	 * but protects it from inadvertent modification or execution.
	 */
	/* 先单映该 alias 可在 patch 完成后原地收紧为 RO，而不拆随后生成的 RAM 大块。 */
	__map_memblock(kernel_start, init_begin, pgprot_tagged(PAGE_KERNEL),
		       flags);

	/* Map the kernel data/bss so it can be remapped later */
	/* data/bss 单独成映射边界，mark_rodata_ro 才能只收紧其 linear alias。 */
	__map_memblock(init_end, kernel_end, pgprot_tagged(PAGE_KERNEL),
		       flags);

	/* map all the memory banks */
	/* memblock 可能给出离散 bank；逐段建映射，洞保持 unmapped。 */
	for_each_mem_range(i, &start, &end) {
		/*
		 * The linear map must allow allocation tags reading/writing
		 * if MTE is present. Otherwise, it has the same attributes as
		 * PAGE_KERNEL.
		 */
		/* tagged 属性让 allocation tag 的读写合法；无 MTE 时编码退化为普通内存。 */
		__map_memblock(start, end, pgprot_tagged(PAGE_KERNEL),
			       flags);
	}
}

/*
 * alternatives 等早期改写结束后的 W^X 收口：rodata、入口前 text 以及线性
 * alias 中的 data/bss 改为只读，并通过 update_mapping_prot 刷新 live TLB。
 */
void mark_rodata_ro(void)
{
	unsigned long section_size;

	/*
	 * mark .rodata as read only. Use __init_begin rather than __end_rodata
	 * to cover NOTES and EXCEPTION_TABLE.
	 */
	/* 取到 __init_begin 可连同 notes/extable 一并保护，避免只保护显式 rodata 符号。 */
	section_size = (unsigned long)__init_begin - (unsigned long)__start_rodata;
	WRITE_ONCE(rodata_is_rw, false);
	update_mapping_prot(__pa_symbol(__start_rodata), (unsigned long)__start_rodata,
			    section_size, PAGE_KERNEL_RO);
	/* mark the range between _text and _stext as read only. */
	/* 入口前的只读头部不执行，也不能遗留启动期写权限。 */
	update_mapping_prot(__pa_symbol(_text), (unsigned long)_text,
			    (unsigned long)_stext - (unsigned long)_text,
			    PAGE_KERNEL_RO);

	/* Map the kernel data/bss read-only in the linear map */
	/* 关闭通过 linear alias 修改镜像 data/bss 的旁路，正式 image 映射仍按段属性。 */
	update_mapping_prot(__pa_symbol(__init_end),
			    (unsigned long)lm_alias(__init_end),
			    (unsigned long)__bss_stop - (unsigned long)__init_end,
			    PAGE_KERNEL_RO);
}

/*
 * 把链接脚本已有的内核区段登记进早期 vmlist，而非新建映射。登记后 vmalloc
 * 分配器知道这些 VA 已占用；默认额外计入一页 guard，VM_NO_GUARD 可禁用。
 */
static void __init declare_vma(struct vm_struct *vma,
			       void *va_start, void *va_end,
			       unsigned long vm_flags)
{
	phys_addr_t pa_start = __pa_symbol(va_start);
	unsigned long size = va_end - va_start;

	BUG_ON(!PAGE_ALIGNED(pa_start));
	BUG_ON(!PAGE_ALIGNED(size));

	if (!(vm_flags & VM_NO_GUARD))
		size += PAGE_SIZE;

	vma->addr	= va_start;
	vma->phys_addr	= pa_start;
	vma->size	= size;
	vma->flags	= VM_MAP | vm_flags;
	vma->caller	= __builtin_return_address(0);

	vm_area_add_early(vma);
}

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
#define KPTI_NG_TEMP_VA		(-(1UL << PMD_SHIFT))

static phys_addr_t kpti_ng_temp_alloc __initdata;

/* 从预留连续页的高地址向下切页，为 KPTI 临时页表逐层供给物理页。 */
static phys_addr_t __init kpti_ng_pgd_alloc(enum pgtable_level pgtable_level)
{
	kpti_ng_temp_alloc -= PAGE_SIZE;
	return kpti_ng_temp_alloc;
}

/*
 * stop_machine 下把 swapper 页表的 kernel 映射改成 nG。CPU0 构建一套最小
 * 临时页表，让物理地址执行的 idmap 汇编能逐页访问/改写原页表；所有 CPU
 * 切入 idmap，CPU0 完成转换并释放临时页，最后发布 arm64_use_ng_mappings。
 */
static int __init __kpti_install_ng_mappings(void *__unused)
{
	typedef void (kpti_remap_fn)(int, int, phys_addr_t, unsigned long);
	extern kpti_remap_fn idmap_kpti_install_ng_mappings;
	kpti_remap_fn *remap_fn;

	int cpu = smp_processor_id();
	int levels = CONFIG_PGTABLE_LEVELS;
	int order = order_base_2(levels);
	u64 kpti_ng_temp_pgd_pa = 0;
	pgd_t *kpti_ng_temp_pgd;
	u64 alloc = 0;

	if (levels == 5 && !pgtable_l5_enabled())
		levels = 4;
	else if (levels == 4 && !pgtable_l4_enabled())
		levels = 3;

	remap_fn = (void *)__pa_symbol(idmap_kpti_install_ng_mappings);

	if (!cpu) {
		int ret;

		alloc = __get_free_pages(GFP_ATOMIC | __GFP_ZERO, order);
		kpti_ng_temp_pgd = (pgd_t *)(alloc + (levels - 1) * PAGE_SIZE);
		kpti_ng_temp_alloc = kpti_ng_temp_pgd_pa = __pa(kpti_ng_temp_pgd);

		//
		// Create a minimal page table hierarchy that permits us to map
		// the swapper page tables temporarily as we traverse them.
		//
		// The physical pages are laid out as follows:
		//
		// +--------+-/-------+-/------ +-/------ +-\\\--------+
		// :  PTE[] : | PMD[] : | PUD[] : | P4D[] : ||| PGD[]  :
		// +--------+-\-------+-\------ +-\------ +-///--------+
		//      ^
		// The first page is mapped into this hierarchy at a PMD_SHIFT
		// aligned virtual address, so that we can manipulate the PTE
		// level entries while the mapping is active. The first entry
		// covers the PTE[] page itself, the remaining entries are free
		// to be used as a ad-hoc fixmap.
		//
		ret = __create_pgd_mapping_locked(kpti_ng_temp_pgd, __pa(alloc),
						  KPTI_NG_TEMP_VA, PAGE_SIZE, PAGE_KERNEL,
						  kpti_ng_pgd_alloc, 0);
		if (ret)
			panic("Failed to create page tables\n");
	}

	cpu_install_idmap();
	remap_fn(cpu, num_online_cpus(), kpti_ng_temp_pgd_pa, KPTI_NG_TEMP_VA);
	cpu_uninstall_idmap();

	if (!cpu) {
		free_pages(alloc, order);
		arm64_use_ng_mappings = true;
	}

	return 0;
}

/* 在确实启用 KPTI 且映射尚非 nG 时发起全 CPU 安全转换。 */
void __init kpti_install_ng_mappings(void)
{
	/* Check whether KPTI is going to be used */
	/* 未启用内核 EL0 unmap 时无需把 global 页表项改为 nG。 */
	if (!arm64_kernel_unmapped_at_el0())
		return;

	/*
	 * We don't need to rewrite the page-tables if either we've done
	 * it already or we have KASLR enabled and therefore have not
	 * created any global mappings at all.
	 */
	/* 已是 nG 或 KASLR 建表时已避免 global 项，重复 stop_machine 没有收益。 */
	if (arm64_use_ng_mappings)
		return;

	init_idmap_kpti_bbml2_flag();
	stop_machine(__kpti_install_ng_mappings, NULL, cpu_online_mask);
}

/* rodata 强制开启时内核可执行页遵循 W^X 为 ROX，否则启动兼容路径允许 RWX。 */
static pgprot_t __init kernel_exec_prot(void)
{
	return rodata_enabled ? PAGE_KERNEL_ROX : PAGE_KERNEL_EXEC;
}

/*
 * 为 KPTI 建立 EL0 异常入口 trampoline：独立 tramp_pg_dir 只映射最少的入口
 * text，完整内核页表则通过 fixmap alias 访问同一物理页。这样用户态运行时
 * TTBR1 不暴露整个内核，却仍能进入异常处理。relocatable 内核还映射紧邻数据。
 */
static int __init map_entry_trampoline(void)
{
	int i;

	if (!arm64_kernel_unmapped_at_el0())
		return 0;

	pgprot_t prot = kernel_exec_prot();
	phys_addr_t pa_start = __pa_symbol(__entry_tramp_text_start);

	/* The trampoline is always mapped and can therefore be global */
	/* trampoline 在所有 KPTI 地址空间相同，保留 global 不会泄露额外内核映射。 */
	pgprot_val(prot) &= ~PTE_NG;

	/* Map only the text into the trampoline page table */
	/* EL0 侧最小页表只需异常入口 text，缩小推测执行可见的内核映射面。 */
	memset(tramp_pg_dir, 0, PGD_SIZE);
	early_create_pgd_mapping(tramp_pg_dir, pa_start, TRAMP_VALIAS,
				 entry_tramp_text_size(), prot,
				 pgd_pgtable_alloc_init_mm, NO_BLOCK_MAPPINGS);

	/* Map both the text and data into the kernel page table */
	/* 完整内核页表通过 fixmap 槽覆盖入口页及 relocatable 情况所需相邻数据。 */
	for (i = 0; i < DIV_ROUND_UP(entry_tramp_text_size(), PAGE_SIZE); i++)
		__set_fixmap(FIX_ENTRY_TRAMP_TEXT1 - i,
			     pa_start + i * PAGE_SIZE, prot);

	if (IS_ENABLED(CONFIG_RELOCATABLE))
		__set_fixmap(FIX_ENTRY_TRAMP_TEXT1 - i,
			     pa_start + i * PAGE_SIZE, PAGE_KERNEL_RO);

	return 0;
}
core_initcall(map_entry_trampoline);
#endif

/*
 * Declare the VMA areas for the kernel
 */
/* 这些静态 vm_struct 只登记链接器区段占位，不分配或改变页表。 */
static void __init declare_kernel_vmas(void)
{
	static struct vm_struct vmlinux_seg[KERNEL_SEGMENT_COUNT];

	declare_vma(&vmlinux_seg[0], _text, _etext, VM_NO_GUARD);
	declare_vma(&vmlinux_seg[1], __start_rodata, __inittext_begin, VM_NO_GUARD);
	declare_vma(&vmlinux_seg[2], __inittext_begin, __inittext_end, VM_NO_GUARD);
	declare_vma(&vmlinux_seg[3], __initdata_begin, __initdata_end, VM_NO_GUARD);
	declare_vma(&vmlinux_seg[4], _data, _end, 0);
}

void __pi_map_range(phys_addr_t *pte, u64 start, u64 end, phys_addr_t pa,
		    pgprot_t prot, int level, pte_t *tbl, bool may_use_cont,
		    u64 va_offset);

/*
 * idmap 的预留下级页表页：不用常规分配器，保证切换 MMU/TTBR 时所需代码与
 * 同步变量按 PA==VA 可达；构建结束后只读。第二组专供 KPTI/BBML2 flag。
 */
static u8 idmap_ptes[IDMAP_LEVELS - 1][PAGE_SIZE] __aligned(PAGE_SIZE) __ro_after_init,
	  kpti_bbml2_ptes[IDMAP_LEVELS - 1][PAGE_SIZE] __aligned(PAGE_SIZE) __ro_after_init;

/* 建立最小恒等映射：idmap text 为 ROX，必要时另给同步 flag 建 RW 映射。 */
static void __init create_idmap(void)
{
	phys_addr_t start = __pa_symbol(__idmap_text_start);
	phys_addr_t end   = __pa_symbol(__idmap_text_end);
	phys_addr_t ptep  = __pa_symbol(idmap_ptes);

	__pi_map_range(&ptep, start, end, start, PAGE_KERNEL_ROX,
		       IDMAP_ROOT_LEVEL, (pte_t *)idmap_pg_dir, false,
		       __phys_to_virt(ptep) - ptep);

	if (linear_map_requires_bbml2 ||
	    (IS_ENABLED(CONFIG_UNMAP_KERNEL_AT_EL0) && !arm64_use_ng_mappings)) {
		phys_addr_t pa = __pa_symbol(&idmap_kpti_bbml2_flag);

		/*
		 * The KPTI G-to-nG conversion code needs a read-write mapping
		 * of its synchronization flag in the ID map. This is also used
		 * when splitting the linear map to ptes if a secondary CPU
		 * doesn't support bbml2.
		 */
		/* flag 必须在 idmap 中 RW 可达，因为正常 TTBR1 已卸载时等待代码仍会访问。 */
		ptep = __pa_symbol(kpti_bbml2_ptes);
		__pi_map_range(&ptep, pa, pa + sizeof(u32), pa, PAGE_KERNEL,
			       IDMAP_ROOT_LEVEL, (pte_t *)idmap_pg_dir, false,
			       __phys_to_virt(ptep) - ptep);
	}
}

/*
 * arm64 启动内存映射的宏观入口：构建线性映射后允许 memblock 元数据扩容，
 * 再建立 TTBR 切换所需 idmap，最后向 vmalloc 子系统声明内核镜像占用区。
 */
void __init paging_init(void)
{
	map_mem();

	memblock_allow_resize();

	create_idmap();
	declare_kernel_vmas();
}

#ifdef CONFIG_MEMORY_HOTPLUG
/*
 * 释放热插拔映射背后的物理存储。altmap 表示 struct page 元数据来自设备自带
 * 预留区，只归还 altmap 配额；普通内存则按映射尺寸释放 buddy 复合页。
 */
static void free_hotplug_page_range(struct page *page, size_t size,
				    struct vmem_altmap *altmap)
{
	if (altmap) {
		vmem_altmap_free(altmap, size >> PAGE_SHIFT);
	} else {
		WARN_ON(PageReserved(page));
		__free_pages(page, get_order(size));
	}
}

/* 页表页先撤销 ptdesc/锁等构造状态，再作为普通单页归还。 */
static void free_hotplug_pgtable_page(struct page *page)
{
	pagetable_dtor(page_ptdesc(page));
	free_hotplug_page_range(page, PAGE_SIZE, NULL);
}

/*
 * 判断本次删除范围是否完整覆盖 mask 对应的父页表槽，且没有越过允许清理的
 * floor/ceiling。只有完整覆盖，才能进一步释放承载整张子页表的物理页。
 */
static bool pgtable_range_aligned(unsigned long start, unsigned long end,
				  unsigned long floor, unsigned long ceiling,
				  unsigned long mask)
{
	start &= mask;
	if (start < floor)
		return false;

	if (ceiling) {
		ceiling &= mask;
		if (!ceiling)
			return false;
	}

	if (end - 1 > ceiling - 1)
		return false;
	return true;
}

/*
 * 清除 PTE 叶子。free_mapped=true 用于 vmemmap：映射本身拥有 backing page，
 * 每清一项需先做重叠 TLB 失效再释放；false 用于线性映射，只撤映射，外层
 * 统一按范围刷 TLB，物理页由 memory hotplug core 管理。
 */
static void unmap_hotplug_pte_range(pmd_t *pmdp, unsigned long addr,
				    unsigned long end, bool free_mapped,
				    struct vmem_altmap *altmap)
{
	pte_t *ptep, pte;

	do {
		ptep = pte_offset_kernel(pmdp, addr);
		pte = __ptep_get(ptep);
		if (pte_none(pte))
			continue;

		WARN_ON(!pte_present(pte));
		__pte_clear(&init_mm, addr, ptep);
		if (free_mapped) {
			/* CONT blocks are not supported in the vmemmap */
			/* vmemmap backing 按 base page 或 huge block 管理，不产生 CONT PTE 组。 */
			WARN_ON(pte_cont(pte));
			flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
			free_hotplug_page_range(pte_page(pte),
						PAGE_SIZE, altmap);
		}
		/* unmap_hotplug_range() flushes TLB for !free_mapped */
		/* 仅撤线性映射时延迟到顶层一次性范围 TLBI，避免逐 PTE 成本。 */
	} while (addr += PAGE_SIZE, addr < end);
}

/* PMD 层递归撤映射；块叶子直接清除并按 PMD_SIZE 释放，表项则下钻 PTE。 */
static void unmap_hotplug_pmd_range(pud_t *pudp, unsigned long addr,
				    unsigned long end, bool free_mapped,
				    struct vmem_altmap *altmap)
{
	unsigned long next;
	pmd_t *pmdp, pmd;

	do {
		next = pmd_addr_end(addr, end);
		pmdp = pmd_offset(pudp, addr);
		pmd = READ_ONCE(*pmdp);
		if (pmd_none(pmd))
			continue;

		WARN_ON(!pmd_present(pmd));
		if (pmd_leaf(pmd)) {
			pmd_clear(pmdp);
			if (free_mapped) {
				/* CONT blocks are not supported in the vmemmap */
				/* PMD huge vmemmap 可存在，但 PMD CONT 组不在该分配/释放协议内。 */
				WARN_ON(pmd_cont(pmd));
				/*
				 * Invalidating a block entry requires just
				 * a single overlapping TLB invalidation,
				 * so limit the range of the flush to a single
				 * page.
				 */
				/* 块描述符任意重叠 VA 的 TLBI 即可使该块翻译失效，无需刷满 PMD 范围。 */
				flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
				free_hotplug_page_range(pmd_page(pmd),
							PMD_SIZE, altmap);
			}
			/* unmap_hotplug_range() flushes TLB for !free_mapped */
			/* free_mapped=false 的统一范围 flush 在全部叶子清除后执行。 */
			continue;
		}
		WARN_ON(!pmd_table(pmd));
		unmap_hotplug_pte_range(pmdp, addr, next, free_mapped, altmap);
	} while (addr = next, addr < end);
}

/* PUD 层递归撤映射；同时兼容 PUD 块叶子和指向 PMD 表的非叶子项。 */
static void unmap_hotplug_pud_range(p4d_t *p4dp, unsigned long addr,
				    unsigned long end, bool free_mapped,
				    struct vmem_altmap *altmap)
{
	unsigned long next;
	pud_t *pudp, pud;

	do {
		next = pud_addr_end(addr, end);
		pudp = pud_offset(p4dp, addr);
		pud = READ_ONCE(*pudp);
		if (pud_none(pud))
			continue;

		WARN_ON(!pud_present(pud));
		if (pud_leaf(pud)) {
			pud_clear(pudp);
			if (free_mapped) {
				/* See comment in unmap_hotplug_pmd_range(). */
				/* PUD 块同样只需一次重叠失效，再按 PUD_SIZE 归还 backing。 */
				flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
				free_hotplug_page_range(pud_page(pud),
							PUD_SIZE, altmap);
			}
			/* unmap_hotplug_range() flushes TLB for !free_mapped */
			/* 线性映射撤销仍由顶层批量 TLBI。 */
			continue;
		}
		WARN_ON(!pud_table(pud));
		unmap_hotplug_pmd_range(pudp, addr, next, free_mapped, altmap);
	} while (addr = next, addr < end);
}

/* P4D 在 arm64 此路径只作为表级，逐段转交 PUD 层。 */
static void unmap_hotplug_p4d_range(pgd_t *pgdp, unsigned long addr,
				    unsigned long end, bool free_mapped,
				    struct vmem_altmap *altmap)
{
	unsigned long next;
	p4d_t *p4dp, p4d;

	do {
		next = p4d_addr_end(addr, end);
		p4dp = p4d_offset(pgdp, addr);
		p4d = READ_ONCE(*p4dp);
		if (p4d_none(p4d))
			continue;

		WARN_ON(!p4d_present(p4d));
		unmap_hotplug_pud_range(p4dp, addr, next, free_mapped, altmap);
	} while (addr = next, addr < end);
}

/*
 * 热移除撤映射的顶层 walker。它只清叶子，不在同一遍中释放中间页表，避免
 * 遍历指针失效；随后 free_empty_tables 自底向上回收空表。调用者持有内存
 * 热插拔排他语义。free_mapped 决定 TLB 刷新和 backing page 所有权策略。
 */
static void unmap_hotplug_range(unsigned long addr, unsigned long end,
				bool free_mapped, struct vmem_altmap *altmap)
{
	unsigned long start = addr;
	unsigned long next;
	pgd_t *pgdp, pgd;

	/*
	 * altmap can only be used as vmemmap mapping backing memory.
	 * In case the backing memory itself is not being freed, then
	 * altmap is irrelevant. Warn about this inconsistency when
	 * encountered.
	 */
	/* altmap 只描述被释放的 vmemmap backing；不释放 backing 时传入它是调用错误。 */
	WARN_ON(!free_mapped && altmap);

	do {
		next = pgd_addr_end(addr, end);
		pgdp = pgd_offset_k(addr);
		pgd = READ_ONCE(*pgdp);
		if (pgd_none(pgd))
			continue;

		WARN_ON(!pgd_present(pgd));
		unmap_hotplug_p4d_range(pgdp, addr, next, free_mapped, altmap);
	} while (addr = next, addr < end);

	if (!free_mapped)
		flush_tlb_kernel_range(start, end);
}

/*
 * 验证目标 PTE 已清空；仅当删除范围完整覆盖父槽且整张 PTE 表无其他使用者时，
 * 清父 PMD、失效页表 walk cache/TLB，再释放页表页。floor/ceiling 防止误收
 * 相邻仍属于其他逻辑区域的页表。
 */
static void free_empty_pte_table(pmd_t *pmdp, unsigned long addr,
				 unsigned long end, unsigned long floor,
				 unsigned long ceiling)
{
	pte_t *ptep, pte;
	unsigned long i, start = addr;

	do {
		ptep = pte_offset_kernel(pmdp, addr);
		pte = __ptep_get(ptep);

		/*
		 * This is just a sanity check here which verifies that
		 * pte clearing has been done by earlier unmap loops.
		 */
		/* 回收阶段不负责撤叶子；发现非 none 表示两阶段 unmap/free 协议被破坏。 */
		WARN_ON(!pte_none(pte));
	} while (addr += PAGE_SIZE, addr < end);

	if (!pgtable_range_aligned(start, end, floor, ceiling, PMD_MASK))
		return;

	/*
	 * Check whether we can free the pte page if the rest of the
	 * entries are empty. Overlap with other regions have been
	 * handled by the floor/ceiling check.
	 */
	/* 先证明范围完整覆盖父槽，再扫描整表，避免释放仍承载相邻映射的 PTE 页。 */
	ptep = pte_offset_kernel(pmdp, 0UL);
	for (i = 0; i < PTRS_PER_PTE; i++) {
		if (!pte_none(__ptep_get(&ptep[i])))
			return;
	}

	pmd_clear(pmdp);
	__flush_tlb_kernel_pgtable(start);
	free_hotplug_pgtable_page(virt_to_page(ptep));
}

/* 自底向上回收：先尝试释放覆盖范围内 PTE 表，再检查整张 PMD 表是否为空。 */
static void free_empty_pmd_table(pud_t *pudp, unsigned long addr,
				 unsigned long end, unsigned long floor,
				 unsigned long ceiling)
{
	pmd_t *pmdp, pmd;
	unsigned long i, next, start = addr;

	do {
		next = pmd_addr_end(addr, end);
		pmdp = pmd_offset(pudp, addr);
		pmd = READ_ONCE(*pmdp);
		if (pmd_none(pmd))
			continue;

		WARN_ON(!pmd_present(pmd) || !pmd_table(pmd));
		free_empty_pte_table(pmdp, addr, next, floor, ceiling);
	} while (addr = next, addr < end);

	if (CONFIG_PGTABLE_LEVELS <= 2)
		return;

	if (!pgtable_range_aligned(start, end, floor, ceiling, PUD_MASK))
		return;

	/*
	 * Check whether we can free the pmd page if the rest of the
	 * entries are empty. Overlap with other regions have been
	 * handled by the floor/ceiling check.
	 */
	/* 所有 PMD 项为空且边界完整时，才清 PUD 父项并释放 PMD 页。 */
	pmdp = pmd_offset(pudp, 0UL);
	for (i = 0; i < PTRS_PER_PMD; i++) {
		if (!pmd_none(READ_ONCE(pmdp[i])))
			return;
	}

	pud_clear(pudp);
	__flush_tlb_kernel_pgtable(start);
	free_hotplug_pgtable_page(virt_to_page(pmdp));
}

/* 仅在运行时确有 P4D 层时，才可能释放承载 PUD 项的页表页。 */
static void free_empty_pud_table(p4d_t *p4dp, unsigned long addr,
				 unsigned long end, unsigned long floor,
				 unsigned long ceiling)
{
	pud_t *pudp, pud;
	unsigned long i, next, start = addr;

	do {
		next = pud_addr_end(addr, end);
		pudp = pud_offset(p4dp, addr);
		pud = READ_ONCE(*pudp);
		if (pud_none(pud))
			continue;

		WARN_ON(!pud_present(pud) || !pud_table(pud));
		free_empty_pmd_table(pudp, addr, next, floor, ceiling);
	} while (addr = next, addr < end);

	if (!pgtable_l4_enabled())
		return;

	if (!pgtable_range_aligned(start, end, floor, ceiling, P4D_MASK))
		return;

	/*
	 * Check whether we can free the pud page if the rest of the
	 * entries are empty. Overlap with other regions have been
	 * handled by the floor/ceiling check.
	 */
	/* 四级页表下确认整张 PUD 表无剩余映射，再从 P4D 摘除。 */
	pudp = pud_offset(p4dp, 0UL);
	for (i = 0; i < PTRS_PER_PUD; i++) {
		if (!pud_none(READ_ONCE(pudp[i])))
			return;
	}

	p4d_clear(p4dp);
	__flush_tlb_kernel_pgtable(start);
	free_hotplug_pgtable_page(virt_to_page(pudp));
}

/* 仅 5 级页表需要回收独立 P4D 页；folded 配置在更低层已经完成。 */
static void free_empty_p4d_table(pgd_t *pgdp, unsigned long addr,
				 unsigned long end, unsigned long floor,
				 unsigned long ceiling)
{
	p4d_t *p4dp, p4d;
	unsigned long i, next, start = addr;

	do {
		next = p4d_addr_end(addr, end);
		p4dp = p4d_offset(pgdp, addr);
		p4d = READ_ONCE(*p4dp);
		if (p4d_none(p4d))
			continue;

		WARN_ON(!p4d_present(p4d));
		free_empty_pud_table(p4dp, addr, next, floor, ceiling);
	} while (addr = next, addr < end);

	if (!pgtable_l5_enabled())
		return;

	if (!pgtable_range_aligned(start, end, floor, ceiling, PGDIR_MASK))
		return;

	/*
	 * Check whether we can free the p4d page if the rest of the
	 * entries are empty. Overlap with other regions have been
	 * handled by the floor/ceiling check.
	 */
	/* 五级页表的最终子层回收；根 PGD 数组本身属于 init_mm，始终保留。 */
	p4dp = p4d_offset(pgdp, 0UL);
	for (i = 0; i < PTRS_PER_P4D; i++) {
		if (!p4d_none(READ_ONCE(p4dp[i])))
			return;
	}

	pgd_clear(pgdp);
	__flush_tlb_kernel_pgtable(start);
	free_hotplug_pgtable_page(virt_to_page(p4dp));
}

/* 从 PGD 遍历 [addr,end)，驱动各层自底向上的空页表回收。PGD 本身不释放。 */
static void free_empty_tables(unsigned long addr, unsigned long end,
			      unsigned long floor, unsigned long ceiling)
{
	unsigned long next;
	pgd_t *pgdp, pgd;

	do {
		next = pgd_addr_end(addr, end);
		pgdp = pgd_offset_k(addr);
		pgd = READ_ONCE(*pgdp);
		if (pgd_none(pgd))
			continue;

		WARN_ON(!pgd_present(pgd));
		free_empty_p4d_table(pgdp, addr, next, floor, ceiling);
	} while (addr = next, addr < end);
}
#endif

/*
 * 为 sparsemem 的 struct page 数组建立 vmemmap backing。4K 页且恰好整 section
 * 时可用大页降低页表成本；其他粒度/非整段范围退回 base page，便于局部管理。
 * altmap 允许设备内存用自身空间存放元数据。
 */
int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node,
		struct vmem_altmap *altmap)
{
	WARN_ON((start < VMEMMAP_START) || (end > VMEMMAP_END));
	/* [start, end] should be within one section */
	/* 通用 sparsemem 以 section 为 populate 单位，跨 section 必须由上层拆调用。 */
	WARN_ON_ONCE(end - start > PAGES_PER_SECTION * sizeof(struct page));

	if (!IS_ENABLED(CONFIG_ARM64_4K_PAGES) ||
	    (end - start < PAGES_PER_SECTION * sizeof(struct page)))
		return vmemmap_populate_basepages(start, end, node, altmap);
	else
		return vmemmap_populate_hugepages(start, end, node, altmap);
}

#ifdef CONFIG_MEMORY_HOTPLUG
/* 撤销 vmemmap、释放其 backing，再回收已经空掉的中间页表。 */
void vmemmap_free(unsigned long start, unsigned long end,
		struct vmem_altmap *altmap)
{
	WARN_ON((start < VMEMMAP_START) || (end > VMEMMAP_END));

	unmap_hotplug_range(start, end, true, altmap);
	free_empty_tables(start, end, VMEMMAP_START, VMEMMAP_END);
}
#endif /* CONFIG_MEMORY_HOTPLUG */

/* 设置 PUD 级巨大映射；已有项只允许安全的权限变更，不允许原地换 PFN/类型。 */
int pud_set_huge(pud_t *pudp, phys_addr_t phys, pgprot_t prot)
{
	pud_t new_pud = pfn_pud(__phys_to_pfn(phys), mk_pud_sect_prot(prot));

	/* Only allow permission changes for now */
	/* 已占用槽必须保持原 PFN/类型；不安全变化返回 0 让通用 huge-vmap 选择其他路径。 */
	if (!pgattr_change_is_safe(READ_ONCE(pud_val(*pudp)),
				   pud_val(new_pud)))
		return 0;

	VM_BUG_ON(phys & ~PUD_MASK);
	set_pud(pudp, new_pud);
	return 1;
}

/* PMD 级 huge/vmalloc 映射版本，物理地址必须按 PMD_SIZE 对齐。 */
int pmd_set_huge(pmd_t *pmdp, phys_addr_t phys, pgprot_t prot)
{
	pmd_t new_pmd = pfn_pmd(__phys_to_pfn(phys), mk_pmd_sect_prot(prot));

	/* Only allow permission changes for now */
	/* PMD huge 设置遵循同一 live 属性白名单，返回 1 才表示成功安装。 */
	if (!pgattr_change_is_safe(READ_ONCE(pmd_val(*pmdp)),
				   pmd_val(new_pmd)))
		return 0;

	VM_BUG_ON(phys & ~PMD_MASK);
	set_pmd(pmdp, new_pmd);
	return 1;
}

#ifndef __PAGETABLE_P4D_FOLDED
/* arm64 不支持 P4D 叶子；提供空钩子满足通用 huge-vmap 接口。 */
void p4d_clear_huge(p4d_t *p4dp)
{
}
#endif

/* 仅当当前项确为 PUD 叶子时清除；返回值告诉通用代码是否已处理。 */
int pud_clear_huge(pud_t *pudp)
{
	if (!pud_leaf(READ_ONCE(*pudp)))
		return 0;
	pud_clear(pudp);
	return 1;
}

/* 仅清 PMD 叶子，表项必须由专门的页表释放路径处理。 */
int pmd_clear_huge(pmd_t *pmdp)
{
	if (!pmd_leaf(READ_ONCE(*pmdp)))
		return 0;
	pmd_clear(pmdp);
	return 1;
}

/*
 * 拆除 PMD->PTE table 并释放 PTE 页。先清父项和刷新页表缓存，保证新 walker
 * 不再进入；若 ptdump static key 已开启，再借 init_mm.mmap_lock 与正在读取
 * 旧表的 walker 建立同步，之后才释放。返回 1 符合通用 huge-vmap 回收接口。
 */
static int __pmd_free_pte_page(pmd_t *pmdp, unsigned long addr,
			       bool acquire_mmap_lock)
{
	pte_t *table;
	pmd_t pmd;

	pmd = READ_ONCE(*pmdp);

	if (!pmd_table(pmd)) {
		VM_WARN_ON(1);
		return 1;
	}

	/* See comment in pud_free_pmd_page for static key logic */
	/* static key 表示 ptdump 可能并发；清父项后用 mmap_lock rendezvous 等旧 walker。 */
	table = pte_offset_kernel(pmdp, addr);
	pmd_clear(pmdp);
	__flush_tlb_kernel_pgtable(addr);
	if (static_branch_unlikely(&arm64_ptdump_lock_key) && acquire_mmap_lock) {
		mmap_read_lock(&init_mm);
		mmap_read_unlock(&init_mm);
	}

	pte_free_kernel(NULL, table);
	return 1;
}

/* 外部入口必须考虑 ptdump 并发，因此允许内部路径获取 mmap_lock。 */
int pmd_free_pte_page(pmd_t *pmdp, unsigned long addr)
{
	/* If ptdump is walking the pagetables, acquire init_mm.mmap_lock */
	/* 公共入口无法证明父表已隔离，因此必须启用诊断 walker 同步。 */
	return __pmd_free_pte_page(pmdp, addr, /* acquire_mmap_lock = */ true);
}

/*
 * 拆除一整张 PMD 表：先隔离父 PUD，让 ptdump 无法再发现它；随后逐项释放
 * 仍存在的 PTE 子表，最后释放 PMD 页。父项隔离后子路径无需重复 mmap_lock。
 */
int pud_free_pmd_page(pud_t *pudp, unsigned long addr)
{
	pmd_t *table;
	pmd_t *pmdp;
	pud_t pud;
	unsigned long next, end;

	pud = READ_ONCE(*pudp);

	if (!pud_table(pud)) {
		VM_WARN_ON(1);
		return 1;
	}

	table = pmd_offset(pudp, addr);

	/*
	 * Our objective is to prevent ptdump from reading a PMD table which has
	 * been freed. In this race, if pud_free_pmd_page observes the key on
	 * (which got flipped by ptdump) then the mmap lock sequence here will,
	 * as a result of the mmap write lock/unlock sequence in ptdump, give
	 * us the correct synchronization. If not, this means that ptdump has
	 * yet not started walking the pagetables - the sequence of barriers
	 * issued by __flush_tlb_kernel_pgtable() guarantees that ptdump will
	 * observe an empty PUD.
	 */
	/* “先清父项+flush，再按 static key 锁会合”覆盖 ptdump 已开始和尚未开始两种竞态。 */
	pud_clear(pudp);
	__flush_tlb_kernel_pgtable(addr);
	if (static_branch_unlikely(&arm64_ptdump_lock_key)) {
		mmap_read_lock(&init_mm);
		mmap_read_unlock(&init_mm);
	}

	pmdp = table;
	next = addr;
	end = addr + PUD_SIZE;
	do {
		if (pmd_present(pmdp_get(pmdp)))
			/*
			 * PMD has been isolated, so ptdump won't see it. No
			 * need to acquire init_mm.mmap_lock.
			 */
			/* 父 PUD 已不可达，新的 ptdump 不会进入此 PMD；旧 walker 已在前面会合。 */
			__pmd_free_pte_page(pmdp, next, /* acquire_mmap_lock = */ false);
	} while (pmdp++, next += PMD_SIZE, next != end);

	pmd_free(NULL, table);
	return 1;
}

#ifdef CONFIG_MEMORY_HOTPLUG
/* 撤销 init_mm 线性映射并回收空页表；不释放被映射 RAM 本身。 */
static void __remove_pgd_mapping(pgd_t *pgdir, unsigned long start, u64 size)
{
	unsigned long end = start + size;

	WARN_ON(pgdir != init_mm.pgd);
	WARN_ON((start < PAGE_OFFSET) || (end > PAGE_END));

	unmap_hotplug_range(start, end, false, NULL);
	free_empty_tables(start, end, PAGE_OFFSET, PAGE_END);
}

/*
 * 把当前实际 VA 位宽下线性映射两端换算成可热插拔 PA 范围。KASLR 可能使
 * 换算结果跨物理地址回绕，此时从 0 起仍是能覆盖全部可寻址 PA 的保守范围。
 */
struct range arch_get_mappable_range(void)
{
	struct range mhp_range;
	phys_addr_t start_linear_pa = __pa(_PAGE_OFFSET(vabits_actual));
	phys_addr_t end_linear_pa = __pa(PAGE_END - 1);

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
		/*
		 * Check for a wrap, it is possible because of randomized linear
		 * mapping the start physical address is actually bigger than
		 * the end physical address. In this case set start to zero
		 * because [0, end_linear_pa] range must still be able to cover
		 * all addressable physical addresses.
		 */
		/* 随机线性偏移可让端点 PA 次序回绕；从 0 起取保守连续可映射集合。 */
		if (start_linear_pa > end_linear_pa)
			start_linear_pa = 0;
	}

	WARN_ON(start_linear_pa > end_linear_pa);

	/*
	 * Linear mapping region is the range [PAGE_OFFSET..(PAGE_END - 1)]
	 * accommodating both its ends but excluding PAGE_END. Max physical
	 * range which can be mapped inside this linear mapping range, must
	 * also be derived from its end points.
	 */
	/* PAGE_END 为 exclusive，故末端用 PAGE_END-1 再换算，避免越界一字节。 */
	mhp_range.start = start_linear_pa;
	mhp_range.end =  end_linear_pa;

	return mhp_range;
}

/*
 * 热添加事务顺序：先建线性映射，再清 memblock NOMAP，最后把 PFN 交给通用
 * memory hotplug。任何后续失败都撤回页表映射；只有全部成功才扩展 max_pfn。
 */
int arch_add_memory(int nid, u64 start, u64 size,
		    struct mhp_params *params)
{
	int ret, flags = NO_EXEC_MAPPINGS;

	VM_BUG_ON(!mhp_range_allowed(start, size, true));

	if (force_pte_mapping())
		flags |= NO_BLOCK_MAPPINGS | NO_CONT_MAPPINGS;

	ret = __create_pgd_mapping(swapper_pg_dir, start, __phys_to_virt(start),
				   size, params->pgprot, pgd_pgtable_alloc_init_mm,
				   flags);
	if (ret)
		goto err;

	memblock_clear_nomap(start, size);

	ret = __add_pages(nid, start >> PAGE_SHIFT, size >> PAGE_SHIFT,
			   params);
	if (ret)
		goto err;

	/* Address of hotplugged memory can be smaller */
	/* 新增区可能位于当前最高 PFN 以下，max 只允许单调扩张。 */
	max_pfn = max(max_pfn, PFN_UP(start + size));
	max_low_pfn = max_pfn;

	return 0;

err:
	__remove_pgd_mapping(swapper_pg_dir,
			     __phys_to_virt(start), size);
	return ret;
}

/* 先由通用层移除页面/设备映射元数据，再撤销对应的 arm64 线性映射。 */
void arch_remove_memory(u64 start, u64 size, struct vmem_altmap *altmap,
			struct dev_pagemap *pgmap)
{
	unsigned long start_pfn = start >> PAGE_SHIFT;
	unsigned long nr_pages = size >> PAGE_SHIFT;

	__remove_pages(start_pfn, nr_pages, altmap, pgmap);
	__remove_pgd_mapping(swapper_pg_dir, __phys_to_virt(start), size);
}


/*
 * 判断 addr 是否落在某个现存叶子映射内部而非边界。逐级先检查对齐，再检查
 * 实际 present/leaf/cont 状态；用于热移除前证明区间端点无需拆分 live 映射。
 */
static bool addr_splits_kernel_leaf(unsigned long addr)
{
	pgd_t *pgdp, pgd;
	p4d_t *p4dp, p4d;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep, pte;

	/*
	 * If the given address points at a the start address of
	 * a possible leaf, we certainly won't split. Otherwise,
	 * check if we would actually split a leaf by traversing
	 * the page tables further.
	 */
	/* 每层“先看边界、再读表项”避免对不存在/折叠层误判，直到实际叶子或 PAGE_SIZE。 */
	if (IS_ALIGNED(addr, PGDIR_SIZE))
		return false;

	pgdp = pgd_offset_k(addr);
	pgd = pgdp_get(pgdp);
	if (!pgd_present(pgd))
		return false;

	if (IS_ALIGNED(addr, P4D_SIZE))
		return false;

	p4dp = p4d_offset(pgdp, addr);
	p4d = p4dp_get(p4dp);
	if (!p4d_present(p4d))
		return false;

	if (IS_ALIGNED(addr, PUD_SIZE))
		return false;

	pudp = pud_offset(p4dp, addr);
	pud = pudp_get(pudp);
	if (!pud_present(pud))
		return false;

	if (pud_leaf(pud))
		return true;

	if (IS_ALIGNED(addr, CONT_PMD_SIZE))
		return false;

	pmdp = pmd_offset(pudp, addr);
	pmd = pmdp_get(pmdp);
	if (!pmd_present(pmd))
		return false;

	if (pmd_cont(pmd))
		return true;

	if (IS_ALIGNED(addr, PMD_SIZE))
		return false;

	if (pmd_leaf(pmd))
		return true;

	if (IS_ALIGNED(addr, CONT_PTE_SIZE))
		return false;

	ptep = pte_offset_kernel(pmdp, addr);
	pte = __ptep_get(ptep);
	if (!pte_present(pte))
		return false;

	if (pte_cont(pte))
		return true;

	return !IS_ALIGNED(addr, PAGE_SIZE);
}

/* 线性映射和 vmemmap 两套 VA 的首尾都必须恰好位于叶子边界，才允许热移除。 */
static bool can_unmap_without_split(unsigned long pfn, unsigned long nr_pages)
{
	unsigned long phys_start, phys_end, start, end;

	phys_start = PFN_PHYS(pfn);
	phys_end = phys_start + nr_pages * PAGE_SIZE;

	/* PFN range's linear map edges are leaf entry aligned */
	/* 任一端落在块/CONT 叶子内部都会要求 live split，热移除策略直接拒绝。 */
	start = __phys_to_virt(phys_start);
	end =  __phys_to_virt(phys_end);
	if (addr_splits_kernel_leaf(start) || addr_splits_kernel_leaf(end)) {
		pr_warn("[%lx %lx] splits a leaf entry in linear map\n",
			phys_start, phys_end);
		return false;
	}

	/* PFN range's vmemmap edges are leaf entry aligned */
	/* struct page 数组的映射也必须能整叶撤销，否则无法安全回收 backing。 */
	BUILD_BUG_ON(!IS_ENABLED(CONFIG_SPARSEMEM_VMEMMAP));
	start = (unsigned long)pfn_to_page(pfn);
	end = (unsigned long)pfn_to_page(pfn + nr_pages);
	if (addr_splits_kernel_leaf(start) || addr_splits_kernel_leaf(end)) {
		pr_warn("[%lx %lx] splits a leaf entry in vmemmap\n",
			phys_start, phys_end);
		return false;
	}
	return true;
}

/*
 * This memory hotplug notifier helps prevent boot memory from being
 * inadvertently removed as it blocks pfn range offlining process in
 * __offline_pages(). Hence this prevents both offlining as well as
 * removal process for boot memory which is initially always online.
 * In future if and when boot memory could be removed, this notifier
 * should be dropped and free_hotplug_page_range() should handle any
 * reserved pages allocated during boot.
 *
 * This also blocks any memory remove that would have caused a split
 * in leaf entry in kernel linear or vmemmap mapping.
 */
/* notifier 同时保护启动 RAM 生命周期和“不在热移除时拆 live 叶子”约束。 */
static int prevent_memory_remove_notifier(struct notifier_block *nb,
					   unsigned long action, void *data)
{
	struct mem_section *ms;
	struct memory_notify *arg = data;
	unsigned long end_pfn = arg->start_pfn + arg->nr_pages;
	unsigned long pfn = arg->start_pfn;

	if ((action != MEM_GOING_OFFLINE) && (action != MEM_OFFLINE))
		return NOTIFY_OK;

	for (; pfn < end_pfn; pfn += PAGES_PER_SECTION) {
		unsigned long start = PFN_PHYS(pfn);
		unsigned long end = start + (1UL << PA_SECTION_SHIFT);

		ms = __pfn_to_section(pfn);
		if (!early_section(ms))
			continue;

		if (action == MEM_GOING_OFFLINE) {
			/*
			 * Boot memory removal is not supported. Prevent
			 * it via blocking any attempted offline request
			 * for the boot memory and just report it.
			 */
			/* early section 可能含启动期 reserved 页，当前释放路径无法逐一正确归还。 */
			pr_warn("Boot memory [%lx %lx] offlining attempted\n", start, end);
			return NOTIFY_BAD;
		} else if (action == MEM_OFFLINE) {
			/*
			 * This should have never happened. Boot memory
			 * offlining should have been prevented by this
			 * very notifier. Probably some memory removal
			 * procedure might have changed which would then
			 * require further debug.
			 */
			/* MEM_OFFLINE 到达说明 GOING_OFFLINE 阶段的 veto 被绕过，只能记录严重错误。 */
			pr_err("Boot memory [%lx %lx] offlined\n", start, end);

			/*
			 * Core memory hotplug does not process a return
			 * code from the notifier for MEM_OFFLINE events.
			 * The error condition has been reported. Return
			 * from here as if ignored.
			 */
			/* 该事件返回码不会回滚 offline，NOTIFY_DONE 仅结束本 notifier 处理。 */
			return NOTIFY_DONE;
		}
	}

	if (!can_unmap_without_split(pfn, arg->nr_pages))
		return NOTIFY_BAD;

	return NOTIFY_OK;
}

static struct notifier_block prevent_memory_remove_nb = {
	.notifier_call = prevent_memory_remove_notifier,
};

/*
 * This ensures that boot memory sections on the platform are online
 * from early boot. Memory sections could not be prevented from being
 * offlined, unless for some reason they are not online to begin with.
 * This helps validate the basic assumption on which the above memory
 * event notifier works to prevent boot memory section offlining and
 * its possible removal.
 */
/* DEBUG_VM 下验证 notifier 的前提：所有 early/boot section 初始必须 online。 */
static void validate_bootmem_online(void)
{
	phys_addr_t start, end, addr;
	struct mem_section *ms;
	u64 i;

	/*
	 * Scanning across all memblock might be expensive
	 * on some big memory systems. Hence enable this
	 * validation only with DEBUG_VM.
	 */
	/* 大内存逐 section 扫描成本高，生产构建依赖初始化协议而不重复验证。 */
	if (!IS_ENABLED(CONFIG_DEBUG_VM))
		return;

	for_each_mem_range(i, &start, &end) {
		for (addr = start; addr < end; addr += (1UL << PA_SECTION_SHIFT)) {
			ms = __pfn_to_section(PHYS_PFN(addr));

			/*
			 * All memory ranges in the system at this point
			 * should have been marked as early sections.
			 */
			/* memblock RAM 在此阶段都应已转成 sparsemem early section。 */
			WARN_ON(!early_section(ms));

			/*
			 * Memory notifier mechanism here to prevent boot
			 * memory offlining depends on the fact that each
			 * early section memory on the system is initially
			 * online. Otherwise a given memory section which
			 * is already offline will be overlooked and can
			 * be removed completely. Call out such sections.
			 */
			/* 已离线 early section 不再触发一次正常 offline veto，可能被直接 remove。 */
			if (!online_section(ms))
				pr_err("Boot memory [%llx %llx] is offline, can be removed\n",
					addr, addr + (1UL << PA_SECTION_SHIFT));
		}
	}
}

/* 仅支持 hotremove 时注册启动内存保护 notifier。 */
static int __init prevent_memory_remove_init(void)
{
	int ret = 0;

	if (!IS_ENABLED(CONFIG_MEMORY_HOTREMOVE))
		return ret;

	validate_bootmem_online();
	ret = register_memory_notifier(&prevent_memory_remove_nb);
	if (ret)
		pr_err("%s: Notifier registration failed %d\n", __func__, ret);

	return ret;
}
early_initcall(prevent_memory_remove_init);
#endif

/*
 * 用户 PTE 改权限的 BBM“break”阶段：原子取出并清除 nr 个项，返回旧首项供
 * 调用者合成新属性。受 Cortex-A510 erratum 2645198 影响且从可执行改为
 * 不可执行时，必须在 make 前显式失效 TLB，避免旧执行权限残留。
 */
pte_t modify_prot_start_ptes(struct vm_area_struct *vma, unsigned long addr,
			     pte_t *ptep, unsigned int nr)
{
	pte_t pte = get_and_clear_ptes(vma->vm_mm, addr, ptep, nr);

	if (alternative_has_cap_unlikely(ARM64_WORKAROUND_2645198)) {
		/*
		 * Break-before-make (BBM) is required for all user space mappings
		 * when the permission changes from executable to non-executable
		 * in cases where cpu is affected with errata #2645198.
		 */
		/* 清有效项后立即 TLBI，防止受影响 CPU 在 make 前继续以旧 X 权限取指。 */
		if (pte_accessible(vma->vm_mm, pte) && pte_user_exec(pte))
			__flush_tlb_range(vma, addr, nr * PAGE_SIZE,
					  PAGE_SIZE, 3, TLBF_NOWALKCACHE);
	}

	return pte;
}

/* 通用单 PTE API，复用批量 break 路径。 */
pte_t ptep_modify_prot_start(struct vm_area_struct *vma, unsigned long addr, pte_t *ptep)
{
	return modify_prot_start_ptes(vma, addr, ptep, 1);
}

/* BBM“make”阶段：把调用者已计算的新 PTE 批量安装回页表。 */
void modify_prot_commit_ptes(struct vm_area_struct *vma, unsigned long addr,
			     pte_t *ptep, pte_t old_pte, pte_t pte,
			     unsigned int nr)
{
	set_ptes(vma->vm_mm, addr, ptep, pte, nr);
}

/* 通用单 PTE commit 包装。old_pte 为接口对称保留，实际安装只需新值。 */
void ptep_modify_prot_commit(struct vm_area_struct *vma, unsigned long addr, pte_t *ptep,
			     pte_t old_pte, pte_t pte)
{
	modify_prot_commit_ptes(vma, addr, ptep, old_pte, pte, 1);
}

/*
 * Atomically replaces the active TTBR1_EL1 PGD with a new VA-compatible PGD,
 * avoiding the possibility of conflicting TLB entries being allocated.
 */
/* 中文过程：切到 idmap、屏蔽全部异常，在物理执行 helper 完成 TTBR1 break/make。 */
void __cpu_replace_ttbr1(pgd_t *pgdp, bool cnp)
{
	typedef void (ttbr_replace_func)(phys_addr_t);
	extern ttbr_replace_func idmap_cpu_replace_ttbr1;
	ttbr_replace_func *replace_phys;
	unsigned long daif;

	/* phys_to_ttbr() zeros lower 2 bits of ttbr with 52-bit PA */
	/* CnP 位位于低位，必须在 PA 编码完成后再显式叠加。 */
	phys_addr_t ttbr1 = phys_to_ttbr(virt_to_phys(pgdp));

	if (cnp)
		ttbr1 |= TTBRx_EL1_CnP;

	replace_phys = (void *)__pa_symbol(idmap_cpu_replace_ttbr1);

	cpu_install_idmap();

	/*
	 * We really don't want to take *any* exceptions while TTBR1 is
	 * in the process of being replaced so mask everything.
	 */
	/* 任一异常若依赖半切换的 TTBR1 都不可恢复，因此暂时保存并屏蔽完整 DAIF。 */
	daif = local_daif_save();
	replace_phys(ttbr1);
	local_daif_restore(daif);

	cpu_uninstall_idmap();
}

#ifdef CONFIG_ARCH_HAS_PKEYS
/*
 * 把通用 pkey 禁止位翻译为 arm64 Permission Overlay Extension 的 RWX nibble，
 * 只替换 POR_EL0 中目标 key 的字段。该寄存器控制当前线程用户访问权限，任务
 * 切换保存恢复由架构线程状态代码负责；硬件不支持 POE 时返回 -ENOSPC。
 */
int arch_set_user_pkey_access(int pkey, unsigned long init_val)
{
	u64 new_por;
	u64 old_por;

	if (!system_supports_poe())
		return -ENOSPC;

	/*
	 * This code should only be called with valid 'pkey'
	 * values originating from in-kernel users.  Complain
	 * if a bad value is observed.
	 */
	/* pkey 来自内核管理接口仍做防御检查，避免移位越出 POR 字段。 */
	if (WARN_ON_ONCE(pkey >= arch_max_pkey()))
		return -EINVAL;

	/* Set the bits we need in POR:  */
	/* 从全 RWX 权限开始逐项清位，可表达通用 PKEY_DISABLE_* 组合。 */
	new_por = POE_RWX;
	if (init_val & PKEY_DISABLE_WRITE)
		new_por &= ~POE_W;
	if (init_val & PKEY_DISABLE_ACCESS)
		new_por &= ~POE_RW;
	if (init_val & PKEY_DISABLE_READ)
		new_por &= ~POE_R;
	if (init_val & PKEY_DISABLE_EXECUTE)
		new_por &= ~POE_X;

	/* Shift the bits in to the correct place in POR for pkey: */
	/* PREP 将该 key 的权限 nibble 放到 POR_EL0 对应槽。 */
	new_por = POR_ELx_PERM_PREP(pkey, new_por);

	/* Get old POR and mask off any old bits in place: */
	/* 保留其他 pkey 槽，只清目标槽旧权限。 */
	old_por = read_sysreg_s(SYS_POR_EL0);
	old_por &= ~(POE_MASK << POR_ELx_PERM_SHIFT(pkey));

	/* Write old part along with new part: */
	/* 单次寄存器写发布组合后的整张权限表。 */
	write_sysreg_s(old_por | new_por, SYS_POR_EL0);

	return 0;
}
#endif
