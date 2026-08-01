/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arm64 硬件页表描述符定义学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 * 本文件把页大小/VA 配置换算成各级覆盖范围，并给出 stage-1/stage-2 描述符和 TCR、
 * TTBR 的位编码。它是“硬件格式字典”，不分配或修改页表；mmu、fault、KVM 和早期
 * 建表代码组合这些常量后，才通过屏障/TLBI/TTBR 协议发布给 table walker。位位置
 * 来自 Arm 架构，错误组合不会返回 errno，而会造成 translation fault、权限绕过或
 * 同一位在 LPA2/不同页大小下被误解，因此条件编译是格式契约的一部分。
 */
/*
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_PGTABLE_HWDEF_H
#define __ASM_PGTABLE_HWDEF_H

#include <asm/memory.h>

#define PTDESC_ORDER 3
/* 每个描述符 2^3=8 字节；一页能容纳的索引位数因此是 PAGE_SHIFT-3。 */

/* Number of VA bits resolved by a single translation table level */
/* 单级表由 PAGE_SIZE/8 个表项组成，每级恰解析 PTDESC_TABLE_SHIFT 个 VA 位。 */
#define PTDESC_TABLE_SHIFT	(PAGE_SHIFT - PTDESC_ORDER)

/*
 * Number of page-table levels required to address 'va_bits' wide
 * address, without section mapping. We resolve the top (va_bits - PAGE_SHIFT)
 * bits with PTDESC_TABLE_SHIFT bits at each page table level. Hence:
 *
 *  levels = DIV_ROUND_UP((va_bits - PAGE_SHIFT), PTDESC_TABLE_SHIFT)
 *
 * where DIV_ROUND_UP(n, d) => (((n) + (d) - 1) / (d))
 *
 * We cannot include linux/kernel.h which defines DIV_ROUND_UP here
 * due to build issues. So we open code DIV_ROUND_UP here:
 *
 *	((((va_bits) - PAGE_SHIFT) + PTDESC_TABLE_SHIFT - 1) / PTDESC_TABLE_SHIFT)
 *
 * which gets simplified as :
 */
/*
 * 对 va_bits 位地址，页内偏移占 PAGE_SHIFT，其余位按每级 TABLE_SHIFT 向上取整。
 * 此底层头不能包含 linux/kernel.h，故把 DIV_ROUND_UP 代数展开并化简成下面的宏；
 * 结果只决定层数，不允许 section/block 映射带来的跳级优化。
 */
#define ARM64_HW_PGTABLE_LEVELS(va_bits) \
	(((va_bits) - PTDESC_ORDER - 1) / PTDESC_TABLE_SHIFT)

/*
 * Size mapped by an entry at level n ( -1 <= n <= 3)
 * We map PTDESC_TABLE_SHIFT at all translation levels and PAGE_SHIFT bits
 * in the final page. The maximum number of translation levels supported by
 * the architecture is 5. Hence, starting at level n, we have further
 * ((4 - n) - 1) levels of translation excluding the offset within the page.
 * So, the total number of bits mapped by an entry at level n is :
 *
 *  ((4 - n) - 1) * PTDESC_TABLE_SHIFT + PAGE_SHIFT
 *
 * Rearranging it a bit we get :
 *   (4 - n) * PTDESC_TABLE_SHIFT + PTDESC_ORDER
 */
/*
 * 架构层号 n 从 -1 到 3；越靠上单项覆盖越大。公式把剩余各级索引位与最终页内
 * 偏移相加，得到该层 block/table entry 的覆盖 shift，供 PMD/PUD/PGDIR 派生尺寸。
 */
#define ARM64_HW_PGTABLE_LEVEL_SHIFT(n)	(PTDESC_TABLE_SHIFT * (4 - (n)) + PTDESC_ORDER)

#define PTRS_PER_PTE		(1 << PTDESC_TABLE_SHIFT)
/* 各完整翻译表页的表项数量相同；顶层 PGD 可能因 VA_BITS 截短而只使用其中一部分。 */

/*
 * PMD_SHIFT determines the size a level 2 page table entry can map.
 */
/* 配置超过两级时才有独立 PMD；SHIFT/SIZE/MASK 描述单个 level-2 项覆盖范围。 */
#if CONFIG_PGTABLE_LEVELS > 2
#define PMD_SHIFT		ARM64_HW_PGTABLE_LEVEL_SHIFT(2)
#define PMD_SIZE		(_AC(1, UL) << PMD_SHIFT)
#define PMD_MASK		(~(PMD_SIZE-1))
#define PTRS_PER_PMD		(1 << PTDESC_TABLE_SHIFT)
#endif

/*
 * PUD_SHIFT determines the size a level 1 page table entry can map.
 */
/* 配置超过三级时才实例化 PUD，PTRS_PER_PUD 是一张完整 level-1 表的扇出。 */
#if CONFIG_PGTABLE_LEVELS > 3
#define PUD_SHIFT		ARM64_HW_PGTABLE_LEVEL_SHIFT(1)
#define PUD_SIZE		(_AC(1, UL) << PUD_SHIFT)
#define PUD_MASK		(~(PUD_SIZE-1))
#define PTRS_PER_PUD		(1 << PTDESC_TABLE_SHIFT)
#endif

#if CONFIG_PGTABLE_LEVELS > 4
/* 五级翻译配置增加 level-0 P4D；未启用时通用代码会折叠该层。 */
#define P4D_SHIFT		ARM64_HW_PGTABLE_LEVEL_SHIFT(0)
#define P4D_SIZE		(_AC(1, UL) << P4D_SHIFT)
#define P4D_MASK		(~(P4D_SIZE-1))
#define PTRS_PER_P4D		(1 << PTDESC_TABLE_SHIFT)
#endif

/*
 * PGDIR_SHIFT determines the size a top-level page table entry can map
 * (depending on the configuration, this level can be -1, 0, 1 or 2).
 */
/* PGDIR 是实际配置的顶层，可能对应硬件 -1/0/1/2；PTRS_PER_PGD 只覆盖 VA_BITS 所需项。 */
#define PGDIR_SHIFT		ARM64_HW_PGTABLE_LEVEL_SHIFT(4 - CONFIG_PGTABLE_LEVELS)
#define PGDIR_SIZE		(_AC(1, UL) << PGDIR_SHIFT)
#define PGDIR_MASK		(~(PGDIR_SIZE-1))
#define PTRS_PER_PGD		(1 << (VA_BITS - PGDIR_SHIFT))

/*
 * Contiguous page definitions.
 */
/*
 * contiguous hint 把一组相邻 PTE/PMD 告诉硬件可作为更大连续翻译缓存；SHIFT 是组
 * 覆盖量级，数量和 mask 用于校验 VA/PA/长度同时对齐。它不减少内存中的表项数量。
 */
#define CONT_PTE_SHIFT		(CONFIG_ARM64_CONT_PTE_SHIFT + PAGE_SHIFT)
#define CONT_PTES		(1 << (CONT_PTE_SHIFT - PAGE_SHIFT))
#define CONT_PTE_SIZE		(CONT_PTES * PAGE_SIZE)
#define CONT_PTE_MASK		(~(CONT_PTE_SIZE - 1))

#define CONT_PMD_SHIFT		(CONFIG_ARM64_CONT_PMD_SHIFT + PMD_SHIFT)
#define CONT_PMDS		(1 << (CONT_PMD_SHIFT - PMD_SHIFT))
#define CONT_PMD_SIZE		(CONT_PMDS * PMD_SIZE)
#define CONT_PMD_MASK		(~(CONT_PMD_SIZE - 1))

/*
 * Hardware page table definitions.
 *
 * Level -1 descriptor (PGD).
 */
/*
 * 以下按硬件层级列出 descriptor 位。TABLE/SECT/PAGE 类型位决定“指向下级表”还是
 * “直接映射”；table AF/PXN/UXN 是层次属性，可限制整个下级子树。FEAT_HAFT 缺失时
 * table AF 被硬件忽略，软件不能借它推断访问状态。
 */
#define PGD_TYPE_TABLE		(_AT(pgdval_t, 3) << 0)
#define PGD_TYPE_MASK		(_AT(pgdval_t, 3) << 0)
#define PGD_TABLE_AF		(_AT(pgdval_t, 1) << 10)	/* Ignored if no FEAT_HAFT */
/* 无 FEAT_HAFT 时该 table access flag 不参与硬件行为。 */
#define PGD_TABLE_PXN		(_AT(pgdval_t, 1) << 59)
#define PGD_TABLE_UXN		(_AT(pgdval_t, 1) << 60)

/*
 * Level 0 descriptor (P4D).
 */
/* P4D 可为下级表或 level-0 block；AP[2] 只读位和层次 XN 位约束整段权限。 */
#define P4D_TYPE_TABLE		(_AT(p4dval_t, 3) << 0)
#define P4D_TYPE_MASK		(_AT(p4dval_t, 3) << 0)
#define P4D_TYPE_SECT		(_AT(p4dval_t, 1) << 0)
#define P4D_SECT_RDONLY		(_AT(p4dval_t, 1) << 7)		/* AP[2] */
#define P4D_TABLE_AF		(_AT(p4dval_t, 1) << 10)	/* Ignored if no FEAT_HAFT */
/* P4D table AF 同样只在 FEAT_HAFT 下有效。 */
#define P4D_TABLE_PXN		(_AT(p4dval_t, 1) << 59)
#define P4D_TABLE_UXN		(_AT(p4dval_t, 1) << 60)

/*
 * Level 1 descriptor (PUD).
 */
/* PUD 的类型、只读、AF 和层次执行禁止编码与 P4D 同构，但使用 pudval_t 保持类型安全。 */
#define PUD_TYPE_TABLE		(_AT(pudval_t, 3) << 0)
#define PUD_TYPE_MASK		(_AT(pudval_t, 3) << 0)
#define PUD_TYPE_SECT		(_AT(pudval_t, 1) << 0)
#define PUD_SECT_RDONLY		(_AT(pudval_t, 1) << 7)		/* AP[2] */
#define PUD_TABLE_AF		(_AT(pudval_t, 1) << 10)	/* Ignored if no FEAT_HAFT */
/* PUD table AF 在没有 FEAT_HAFT 的实现上为忽略位。 */
#define PUD_TABLE_PXN		(_AT(pudval_t, 1) << 59)
#define PUD_TABLE_UXN		(_AT(pudval_t, 1) << 60)

/*
 * Level 2 descriptor (PMD).
 */
/* PMD 可指向 PTE 表或直接形成 block；TYPE_MASK 用于在修改权限时保留/替换类型。 */
#define PMD_TYPE_MASK		(_AT(pmdval_t, 3) << 0)
#define PMD_TYPE_TABLE		(_AT(pmdval_t, 3) << 0)
#define PMD_TYPE_SECT		(_AT(pmdval_t, 1) << 0)
#define PMD_TABLE_AF		(_AT(pmdval_t, 1) << 10)	/* Ignored if no FEAT_HAFT */
/* PMD table AF 的可用性仍取决于 FEAT_HAFT。 */

/*
 * Section
 */
/*
 * PMD block 权限组：USER/READONLY 来自 AP，S 是 shareability，AF 表示已访问，NG
 * 控制 ASID/global 作用域，CONT 是连续提示，PXN/UXN 分别禁止特权/用户执行；
 * TABLE_PXN/UXN 仅在 PMD 作为下级表指针时约束整棵子树。
 */
#define PMD_SECT_USER		(_AT(pmdval_t, 1) << 6)		/* AP[1] */
#define PMD_SECT_RDONLY		(_AT(pmdval_t, 1) << 7)		/* AP[2] */
#define PMD_SECT_S		(_AT(pmdval_t, 3) << 8)
#define PMD_SECT_AF		(_AT(pmdval_t, 1) << 10)
#define PMD_SECT_NG		(_AT(pmdval_t, 1) << 11)
#define PMD_SECT_CONT		(_AT(pmdval_t, 1) << 52)
#define PMD_SECT_PXN		(_AT(pmdval_t, 1) << 53)
#define PMD_SECT_UXN		(_AT(pmdval_t, 1) << 54)
#define PMD_TABLE_PXN		(_AT(pmdval_t, 1) << 59)
#define PMD_TABLE_UXN		(_AT(pmdval_t, 1) << 60)

/*
 * AttrIndx[2:0] encoding (mapping attributes defined in the MAIR* registers).
 */
/* AttrIndx 不是缓存策略本身，而是索引 MAIR_ELx 中预先编程的 8 个内存属性槽。 */
#define PMD_ATTRINDX(t)		(_AT(pmdval_t, (t)) << 2)
#define PMD_ATTRINDX_MASK	(_AT(pmdval_t, 7) << 2)

/*
 * Level 3 descriptor (PTE).
 */
/*
 * 叶 PTE 的 VALID/TYPE 决定描述符有效性；AP/SH/AF/nG 与 PMD block 同义；GP 为 BTI
 * guarded page，DBM 支持硬件脏位管理，CONT 提示连续组，PXN/UXN 落实 W^X；
 * SWBITS_MASK 保留给 Linux 软件状态，硬件遍历不得把它们当物理地址或权限。
 */
#define PTE_VALID		(_AT(pteval_t, 1) << 0)
#define PTE_TYPE_MASK		(_AT(pteval_t, 3) << 0)
#define PTE_TYPE_PAGE		(_AT(pteval_t, 3) << 0)
#define PTE_USER		(_AT(pteval_t, 1) << 6)		/* AP[1] */
#define PTE_RDONLY		(_AT(pteval_t, 1) << 7)		/* AP[2] */
#define PTE_SHARED		(_AT(pteval_t, 3) << 8)		/* SH[1:0], inner shareable */
#define PTE_AF			(_AT(pteval_t, 1) << 10)	/* Access Flag */
#define PTE_NG			(_AT(pteval_t, 1) << 11)	/* nG */
#define PTE_GP			(_AT(pteval_t, 1) << 50)	/* BTI guarded */
#define PTE_DBM			(_AT(pteval_t, 1) << 51)	/* Dirty Bit Management */
#define PTE_CONT		(_AT(pteval_t, 1) << 52)	/* Contiguous range */
#define PTE_PXN			(_AT(pteval_t, 1) << 53)	/* Privileged XN */
#define PTE_UXN			(_AT(pteval_t, 1) << 54)	/* User XN */
/* 行尾 AP/SH/AF/nG/BTI/DBM/CONT/XN 名称对应上述权限与状态组，而非独立软件标志。 */
#define PTE_SWBITS_MASK		_AT(pteval_t, (BIT(63) | GENMASK(58, 55)))

/* 传统叶描述符物理地址低域从 PAGE_SHIFT 延伸到 bit49，页内偏移不写入 PTE。 */
#define PTE_ADDR_LOW		(((_AT(pteval_t, 1) << (50 - PAGE_SHIFT)) - 1) << PAGE_SHIFT)
#ifdef CONFIG_ARM64_PA_BITS_52
/*
 * 52 位 PA 超出传统 descriptor 低地址域：64K 页把高四位编码到 [15:12]，其他页
 * 大小在 [9:8] 承载高位并使用不同搬移量；PHYS_TO_PTE_ADDR_MASK 汇总可写地址位。
 */
#ifdef CONFIG_ARM64_64K_PAGES
#define PTE_ADDR_HIGH		(_AT(pteval_t, 0xf) << 12)
#define PTE_ADDR_HIGH_SHIFT	36
#define PHYS_TO_PTE_ADDR_MASK	(PTE_ADDR_LOW | PTE_ADDR_HIGH)
#else
#define PTE_ADDR_HIGH		(_AT(pteval_t, 0x3) << 8)
#define PTE_ADDR_HIGH_SHIFT	42
#define PHYS_TO_PTE_ADDR_MASK	GENMASK_ULL(49, 8)
#endif
#endif

/*
 * AttrIndx[2:0] encoding (mapping attributes defined in the MAIR* registers).
 */
/* 叶 PTE 的 AttrIndx 与 PMD block 一样索引 MAIR，MASK 用于替换时先清旧索引。 */
#define PTE_ATTRINDX(t)		(_AT(pteval_t, (t)) << 2)
#define PTE_ATTRINDX_MASK	(_AT(pteval_t, 7) << 2)

/*
 * PIIndex[3:0] encoding (Permission Indirection Extension)
 */
/* PIE 把四个分散的传统权限位重解释为 PIIndex，索引 PIR/PIRE0 中的间接权限模板。 */
#define PTE_PI_IDX_0	6	/* AP[1], USER */
#define PTE_PI_IDX_1	51	/* DBM */
#define PTE_PI_IDX_2	53	/* PXN */
#define PTE_PI_IDX_3	54	/* UXN */

/*
 * POIndex[2:0] encoding (Permission Overlay Extension)
 */
/* POE 使用软件高位 [62:60] 形成 overlay 索引，在基础权限之上施加额外限制。 */
#define PTE_PO_IDX_0	(_AT(pteval_t, 1) << 60)
#define PTE_PO_IDX_1	(_AT(pteval_t, 1) << 61)
#define PTE_PO_IDX_2	(_AT(pteval_t, 1) << 62)

#define PTE_PO_IDX_MASK		GENMASK_ULL(62, 60)


/*
 * Memory Attribute override for Stage-2 (MemAttr[3:0])
 */
/* stage-2 MemAttr 直接编码 guest IPA->PA 的内存属性覆盖，不通过 stage-1 MAIR 索引。 */
#define PTE_S2_MEMATTR(t)	(_AT(pteval_t, (t)) << 2)
/* @t 是架构定义的 4 位 MemAttr[3:0] 原始编码；调用方须保证高位为零，宏不做掩码校验。 */

/*
 * Hierarchical permission for Stage-1 tables
 */
/* stage-1 表描述符的高两位 AP 可一次限制整个子树，不能被叶项放宽。 */
#define S1_TABLE_AP		(_AT(pmdval_t, 3) << 61)

/*
 * TCR flags.
 */
/*
 * T0SZ/T1SZ 由 VA 位数换算为“未参与翻译的高位数”；其余别名按 TTBR0/TTBR1 分组
 * 描述 walker 开关、table-walk cacheability/shareability、4K/16K/64K granule、
 * 物理地址宽度、ASID、top-byte-ignore、硬件 AF/dirty、层次权限、E0PD 和 LPA2 DS。
 * 这些值只构造 TCR，真正写系统寄存器后还需 ISB/TLB 协议。
 */
#define TCR_T0SZ(x)		((UL(64) - (x)) << TCR_EL1_T0SZ_SHIFT)
#define TCR_T1SZ(x)		((UL(64) - (x)) << TCR_EL1_T1SZ_SHIFT)

#define TCR_T0SZ_MASK		TCR_EL1_T0SZ_MASK
#define TCR_T1SZ_MASK		TCR_EL1_T1SZ_MASK
/*
 * x 是对应 TTBR 区域实际使用的 VA 位数，受 granule 和硬件能力限制；宏返回已移位的
 * TnSZ 编码 64-x。T0SZ_MASK/T1SZ_MASK 用来先清除旧宽度，再安装新值；它们不会验证
 * x 是否为当前 CPU 支持的范围，调用者必须先依据 ID 寄存器完成能力裁剪。
 */

#define TCR_EPD0_MASK		TCR_EL1_EPD0_MASK
#define TCR_EPD1_MASK		TCR_EL1_EPD1_MASK
/*
 * EPD0/EPD1 分别控制 TTBR0_EL1 与 TTBR1_EL1 区域的 table walk 禁止位；置位后相应
 * 区域的地址转换直接 fault，而不是继续读取根页表。两个 mask 用于精确清除或替换
 * 该策略，避免更新一侧时误伤另一侧地址空间。
 */

#define TCR_IRGN0_MASK		TCR_EL1_IRGN0_MASK
#define TCR_IRGN0_WBWA		(TCR_EL1_IRGN0_WBWA << TCR_EL1_IRGN0_SHIFT)

#define TCR_ORGN0_MASK		TCR_EL1_ORGN0_MASK
#define TCR_ORGN0_WBWA		(TCR_EL1_ORGN0_WBWA << TCR_EL1_ORGN0_SHIFT)
/*
 * IRGN0/ORGN0 只描述 TTBR0 table walk 对页表内存的内层/外层 cacheability；WBWA
 * 选择 write-back、write-allocate，MASK 用于替换旧编码。它们不改变最终映射页面
 * 自身的缓存属性，后者由描述符 AttrIndx 与 MAIR 决定。
 */

#define TCR_SH0_MASK		TCR_EL1_SH0_MASK
#define TCR_SH0_INNER		(TCR_EL1_SH0_INNER << TCR_EL1_SH0_SHIFT)

#define TCR_SH1_MASK		TCR_EL1_SH1_MASK
/*
 * SH0 指定 TTBR0 walker 的 shareability，SH0_INNER 令页表遍历在 inner-shareable 域
 * 内一致；SH1_MASK 对应 TTBR1 walker 的同名字段，供上层代码保留或替换其编码。
 * 该属性约束“页表描述符怎样被硬件读取”，不等同于叶映射的 PTE_SHARED。
 */

#define TCR_TG0_SHIFT		TCR_EL1_TG0_SHIFT
#define TCR_TG0_MASK		TCR_EL1_TG0_MASK
#define TCR_TG0_4K		(TCR_EL1_TG0_4K << TCR_EL1_TG0_SHIFT)
#define TCR_TG0_64K		(TCR_EL1_TG0_64K << TCR_EL1_TG0_SHIFT)
#define TCR_TG0_16K		(TCR_EL1_TG0_16K << TCR_EL1_TG0_SHIFT)

#define TCR_TG1_SHIFT		TCR_EL1_TG1_SHIFT
#define TCR_TG1_MASK		TCR_EL1_TG1_MASK
#define TCR_TG1_16K		(TCR_EL1_TG1_16K << TCR_EL1_TG1_SHIFT)
#define TCR_TG1_4K		(TCR_EL1_TG1_4K << TCR_EL1_TG1_SHIFT)
#define TCR_TG1_64K		(TCR_EL1_TG1_64K << TCR_EL1_TG1_SHIFT)
/*
 * TG0/TG1 分别选择 TTBR0/TTBR1 翻译表的 4K、16K 或 64K granule；SHIFT 暴露字段
 * 位置，MASK 用于清旧值，各尺寸常量是已经移位的可直接 OR 编码。TG0 与 TG1 的
 * 架构数值排列不同，必须使用对应一组常量，不能因为页大小相同就交叉复用。
 */

#define TCR_IPS_SHIFT		TCR_EL1_IPS_SHIFT
#define TCR_IPS_MASK		TCR_EL1_IPS_MASK
/* IPS 给 stage-1 walker 选择中间/输出物理地址宽度；SHIFT/MASK 供能力探测结果装入 TCR。 */
#define TCR_A1			TCR_EL1_A1
#define TCR_ASID16		TCR_EL1_AS
/*
 * A1 决定当前 ASID 取自 TTBR1_EL1 还是 TTBR0_EL1；切换错误会让地址空间带上错误
 * 标签并复用陈旧 TLB 项。ASID16 选择 16 位 ASID 编码，未启用时只使用 8 位范围；
 * 它必须与 CPU ASID 能力和 TTBR 写入格式保持一致。
 */
#define TCR_TBI0		TCR_EL1_TBI0
#define TCR_TBI1		TCR_EL1_TBI1
/*
 * TBI0/TBI1 分别允许 TTBR0/TTBR1 区域在地址转换时忽略 VA 顶字节，使软件可承载
 * tag；它们只影响地址解释，不验证 tag，也不自动启用 MTE。
 */
#define TCR_HA			TCR_EL1_HA
#define TCR_HD			TCR_EL1_HD
/*
 * HA 允许硬件在 table walk 中更新 Access Flag；HD 在 HA 基础上允许硬件依据 DBM
 * 管理脏状态。关闭时相应 fault/软件路径必须补做记账；开启后描述符会被硬件并发写入，
 * 页表管理代码必须按架构规定执行原子更新与 TLB 同步。
 */
#define TCR_HPD0		TCR_EL1_HPD0
#define TCR_HPD1		TCR_EL1_HPD1
/* HPD0/HPD1 分别禁用 TTBR0/TTBR1 子树的层次权限，使权限只按最终 block/page 项解释。 */
#define TCR_TBID0		TCR_EL1_TBID0
#define TCR_TBID1		TCR_EL1_TBID1
/*
 * TBID0/TBID1 在对应区域把 TBI 限制为数据地址：置位时指令取址仍检查顶字节，避免
 * tagged instruction pointer 绕过取指地址规范；两位必须与各自 TBI0/TBI1 配对理解。
 */
#define TCR_E0PD0		TCR_EL1_E0PD0
#define TCR_E0PD1		TCR_EL1_E0PD1
/*
 * E0PD0/E0PD1 让 EL0 对对应 TTBR 区域的访问在开始 table walk 前即产生 fault，既可
 * 隔离不应由用户态触达的区域，也减少利用页表遍历时序探测内核映射的机会；EL1 访问
 * 不因此被整体关闭。
 */
#define TCR_DS			TCR_EL1_DS
/*
 * DS 选择 FEAT_LPA2 的 stage-1 描述符/地址格式，使合适 granule 能表达 52 位输出地址，
 * 并连带改变部分 descriptor、TTBR 与 shareability 位的解释。它不能只因 PA_BITS=52
 * 就盲目置位，必须由 LPA2 能力和当前页大小共同决定，并在切换格式时遵守 TTBR/TLBI。
 */

/*
 * TTBR.
 */
/* TTBR 组定义根页表基址编码以及 48/52 位 VA 共存时 swapper_pg_dir 内的偏移。 */
#ifdef CONFIG_ARM64_PA_BITS_52
/*
 * TTBR_ELx[1] is RES0 in this configuration.
 */
/* 52 位 PA 配置下 TTBR bit[1] 必须为 0，基址 mask 因而只允许 [47:2]。 */
#define TTBR_BADDR_MASK_52	GENMASK_ULL(47, 2)
#endif

#ifdef CONFIG_ARM64_VA_BITS_52
#define PTRS_PER_PGD_52_VA (UL(1) << (52 - PGDIR_SHIFT))
#define PTRS_PER_PGD_48_VA (UL(1) << (48 - PGDIR_SHIFT))
#define PTRS_PER_PGD_EXTRA (PTRS_PER_PGD_52_VA - PTRS_PER_PGD_48_VA)
/*
 * 前两个宏分别给出 52 位与 48 位内核 VA 模式实际使用的 PGD 项数，EXTRA 是两种
 * 模式之间多出的表项数。启动代码把该差值乘 8 字节描述符宽度，得到 TTBR1 根基址
 * 在同一 swapper_pg_dir 内需要跨过的前缀；这些都是构建期容量/偏移，不是运行期计数。
 */

/* Must be at least 64-byte aligned to prevent corruption of the TTBR */
/* 48/52 VA 根表切换偏移至少 64 字节对齐，避免低 TTBR 控制位被地址增量污染。 */
#define TTBR1_BADDR_4852_OFFSET (PTRS_PER_PGD_EXTRA << PTDESC_ORDER)
#endif

#endif
