// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 页表遍历、相邻区间压缩与 W^X 安全审计学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 通用 ptdump walker 逐 leaf 回调本文件；note_page() 不逐页打印，而把
 * “层级+受关注属性”相同的相邻映射合成一行。相同遍历器有两个消费者：
 * debugfs 输出 kernel_page_tables，以及不输出文本的 W+X/non-UXN 计数。
 *
 * 页表可能并发发生 live 更新。arm64_ptdump_lock_key 静态键通知页表修改
 * 路径采用与 dump 兼容的锁/同步策略；遍历开始前 inc，结束后 dec。状态
 * 对象只属于一次调用，kernel_pg_levels/markers 在 init 后只读。
 */
/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 * Debug helper to dump the current kernel pagetables of the system
 * so that we can see what the various memory ranges are set to.
 *
 * Derived from x86 and arm implementation:
 * (C) Copyright 2008 Intel Corporation
 *
 * Author: Arjan van de Ven <arjan@linux.intel.com>
 */
/* 上游说明强调这是诊断快照，不是稳定 ABI，也不用于修改页表。 */
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/ptdump.h>
#include <linux/sched.h>
#include <linux/seq_file.h>

#include <asm/fixmap.h>
#include <asm/kasan.h>
#include <asm/memory.h>
#include <asm/pgtable-hwdef.h>
#include <asm/ptdump.h>


#define pt_dump_seq_printf(m, fmt, args...)	\
({						\
	if (m)					\
		seq_printf(m, fmt, ##args);	\
})
/* seq_file 可为 NULL（W^X 审计）；两个宏在无输出消费者时消除格式化调用。 */

#define pt_dump_seq_puts(m, fmt)	\
({					\
	if (m)				\
		seq_printf(m, fmt);	\
})

/*
 * 描述需要从 leaf 原始位中解码的字段：mask/val 匹配时打印 set，否则打印
 * clear。多个 AttrIndx 项共享同一 mask，只会有匹配类型输出字符串。
 */
static const struct ptdump_prot_bits pte_bits[] = {
	{
		.mask	= PTE_VALID,
		.val	= PTE_VALID,
		.set	= " ",
		.clear	= "F",
	}, {
		.mask	= PTE_USER,
		.val	= PTE_USER,
		.set	= "USR",
		.clear	= "   ",
	}, {
		.mask	= PTE_RDONLY,
		.val	= PTE_RDONLY,
		.set	= "ro",
		.clear	= "RW",
	}, {
		.mask	= PTE_PXN,
		.val	= PTE_PXN,
		.set	= "NX",
		.clear	= "x ",
	}, {
		.mask	= PTE_SHARED,
		.val	= PTE_SHARED,
		.set	= "SHD",
		.clear	= "   ",
	}, {
		.mask	= PTE_AF,
		.val	= PTE_AF,
		.set	= "AF",
		.clear	= "  ",
	}, {
		.mask	= PTE_NG,
		.val	= PTE_NG,
		.set	= "NG",
		.clear	= "  ",
	}, {
		.mask	= PTE_CONT,
		.val	= PTE_CONT,
		.set	= "CON",
		.clear	= "   ",
	}, {
		.mask	= PMD_TYPE_MASK,
		.val	= PMD_TYPE_SECT,
		.set	= "BLK",
		.clear	= "   ",
	}, {
		.mask	= PTE_UXN,
		.val	= PTE_UXN,
		.set	= "UXN",
		.clear	= "   ",
	}, {
		.mask	= PTE_GP,
		.val	= PTE_GP,
		.set	= "GP",
		.clear	= "  ",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_DEVICE_nGnRnE),
		.set	= "DEVICE/nGnRnE",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_DEVICE_nGnRE),
		.set	= "DEVICE/nGnRE",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_NORMAL_NC),
		.set	= "MEM/NORMAL-NC",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_NORMAL),
		.set	= "MEM/NORMAL",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_NORMAL_TAGGED),
		.set	= "MEM/NORMAL-TAGGED",
	}
};

/*
 * 五个软件页表层级的显示元数据。mask 在 ptdump_initialize() 中由 bits
 * 汇总后冻结，用于判断相邻映射是否“显示属性相同”。运行时折叠层由
 * note_page 动态归一，数组仍保留统一索引。
 */
static struct ptdump_pg_level kernel_pg_levels[] __ro_after_init = {
	{ /* pgd */
		.name	= "PGD",
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	}, { /* p4d */
		.name	= "P4D",
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	}, { /* pud */
		.name	= "PUD",
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	}, { /* pmd */
		.name	= "PMD",
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	}, { /* pte */
		.name	= "PTE",
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	},
};

/* 按 bits 表把 st->current_prot 格式化到 seq；seq=NULL 时宏安全空操作。 */
static void dump_prot(struct ptdump_pg_state *st, const struct ptdump_prot_bits *bits,
			size_t num)
{
	unsigned i;

	for (i = 0; i < num; i++, bits++) {
		const char *s;

		if ((st->current_prot & bits->mask) == bits->val)
			s = bits->set;
		else
			s = bits->clear;

		if (s)
			pt_dump_seq_printf(st->seq, " %s", s);
	}
}

/*
 * 审计刚结束的 [start_address,addr) 是否缺少 UXN。只在 check_wx 模式运行；
 * 命中时 WARN_ONCE 并按 PAGE_SIZE 累计 uxn_pages，不修改映射。
 */
static void note_prot_uxn(struct ptdump_pg_state *st, unsigned long addr)
{
	if (!st->check_wx)
		return;

	if ((st->current_prot & PTE_UXN) == PTE_UXN)
		return;

	WARN_ONCE(1, "arm64/mm: Found non-UXN mapping at address %p/%pS\n",
		  (void *)st->start_address, (void *)st->start_address);

	st->uxn_pages += (addr - st->start_address) / PAGE_SIZE;
}

/* 审计区间是否同时可写且 privileged executable，命中累计 wx_pages。 */
static void note_prot_wx(struct ptdump_pg_state *st, unsigned long addr)
{
	if (!st->check_wx)
		return;
	if ((st->current_prot & PTE_RDONLY) == PTE_RDONLY)
		return;
	if ((st->current_prot & PTE_PXN) == PTE_PXN)
		return;

	WARN_ONCE(1, "arm64/mm: Found insecure W+X mapping at address %p/%pS\n",
		  (void *)st->start_address, (void *)st->start_address);

	st->wx_pages += (addr - st->start_address) / PAGE_SIZE;
}

/*
 * ptdump 核心聚合回调。pt_st 嵌在 arm64 私有 state 中，用 container_of
 * 恢复宿主；addr 是当前映射起点，level 0..4 表示 PGD..PTE，-1 用于最终
 * flush，val 是 leaf 原始位。函数在属性/层级/marker 改变时结束上一段，
 * 做安全审计、输出范围大小与属性，再开始新段。无显式返回，更新 st 游标。
 */
void note_page(struct ptdump_state *pt_st, unsigned long addr, int level,
	       pteval_t val)
{
	struct ptdump_pg_state *st = container_of(pt_st, struct ptdump_pg_state, ptdump);
	struct ptdump_pg_level *pg_level = st->pg_level;
	static const char units[] = "KMGTPE";
	ptval_t prot = 0;

	/* check if the current level has been folded dynamically */
	/* 动态折叠的 P4D/PUD 不是真实 leaf 层，归到根层避免错误数组语义。 */
	if (st->mm && ((level == 1 && mm_p4d_folded(st->mm)) ||
	    (level == 2 && mm_pud_folded(st->mm))))
		level = 0;

	if (level >= 0)
		prot = val & pg_level[level].mask;

	if (st->level == -1) {
		/* 第一个回调只建立聚合状态，并输出当前布局 marker。 */
		st->level = level;
		st->current_prot = prot;
		st->start_address = addr;
		pt_dump_seq_printf(st->seq, "---[ %s ]---\n", st->marker->name);
	} else if (prot != st->current_prot || level != st->level ||
		   addr >= st->marker[1].start_address) {
		const char *unit = units;
		unsigned long delta;

		if (st->current_prot) {
			/* prot==0 表示洞/none，不参与 W^X/UXN 安全计数。 */
			note_prot_uxn(st, addr);
			note_prot_wx(st, addr);
		}

		pt_dump_seq_printf(st->seq, "0x%016lx-0x%016lx   ",
				   st->start_address, addr);

		delta = (addr - st->start_address) >> 10;
		/* 先转 KiB，再每能整除 1024 就提升 K/M/G/T/P/E 单位。 */
		while (!(delta & 1023) && unit[1]) {
			delta >>= 10;
			unit++;
		}
		pt_dump_seq_printf(st->seq, "%9lu%c %s", delta, *unit,
				   pg_level[st->level].name);
		if (st->current_prot && pg_level[st->level].bits)
			dump_prot(st, pg_level[st->level].bits,
				  pg_level[st->level].num);
		pt_dump_seq_puts(st->seq, "\n");

		if (addr >= st->marker[1].start_address) {
			/* marker 数组以 -1 哨兵结束，顺序必须严格递增。 */
			st->marker++;
			pt_dump_seq_printf(st->seq, "---[ %s ]---\n", st->marker->name);
		}

		st->start_address = addr;
		st->current_prot = prot;
		st->level = level;
	}

	if (addr >= st->marker[1].start_address) {
		st->marker++;
		pt_dump_seq_printf(st->seq, "---[ %s ]---\n", st->marker->name);
	}

}

/* 五个类型安全包装把具体页表类型解码为原始值并固定层级编号。 */
void note_page_pte(struct ptdump_state *pt_st, unsigned long addr, pte_t pte)
{
	note_page(pt_st, addr, 4, pte_val(pte));
}

void note_page_pmd(struct ptdump_state *pt_st, unsigned long addr, pmd_t pmd)
{
	note_page(pt_st, addr, 3, pmd_val(pmd));
}

void note_page_pud(struct ptdump_state *pt_st, unsigned long addr, pud_t pud)
{
	note_page(pt_st, addr, 2, pud_val(pud));
}

void note_page_p4d(struct ptdump_state *pt_st, unsigned long addr, p4d_t p4d)
{
	note_page(pt_st, addr, 1, p4d_val(p4d));
}

void note_page_pgd(struct ptdump_state *pt_st, unsigned long addr, pgd_t pgd)
{
	note_page(pt_st, addr, 0, pgd_val(pgd));
}

/*
 * walker 结束回调：用 level=-1 的零项强制 note_page 输出最后一个聚合区间。
 */
void note_page_flush(struct ptdump_state *pt_st)
{
	pte_t pte_zero = {0};

	note_page(pt_st, 0, -1, pte_val(pte_zero));
}

/*
 * 在一次完整 PGD walk 周围开启 arm64 页表 dump 静态键。inc/dec 必须配对，
 * 使并发页表修改者知道存在 lock-sensitive walker；mm 在遍历期间由调用者
 * 持有引用，函数不取得/释放 mm。
 */
static void arm64_ptdump_walk_pgd(struct ptdump_state *st, struct mm_struct *mm)
{
	static_branch_inc(&arm64_ptdump_lock_key);
	ptdump_walk_pgd(st, mm, NULL);
	static_branch_dec(&arm64_ptdump_lock_key);
}

/*
 * 根据 info 创建一次 dump 状态并遍历。s 可为 NULL；info->mm/markers 必须
 * 长期有效。base_addr 若在用户区则终点 TASK_SIZE_64，否则遍历到 ULONG_MAX。
 * range 复合字面量生命周期覆盖同步 walker 调用，不可被异步保存。
 */
void ptdump_walk(struct seq_file *s, struct ptdump_info *info)
{
	unsigned long end = ~0UL;
	struct ptdump_pg_state st;

	if (info->base_addr < TASK_SIZE_64)
		end = TASK_SIZE_64;

	st = (struct ptdump_pg_state){
		.seq = s,
		.marker = info->markers,
		.mm = info->mm,
		.pg_level = &kernel_pg_levels[0],
		.level = -1,
		.ptdump = {
			.note_page_pte = note_page_pte,
			.note_page_pmd = note_page_pmd,
			.note_page_pud = note_page_pud,
			.note_page_p4d = note_page_p4d,
			.note_page_pgd = note_page_pgd,
			.note_page_flush = note_page_flush,
			.range = (struct ptdump_range[]){
				{info->base_addr, end},
				{0, 0}
			}
		}
	};

	arm64_ptdump_walk_pgd(&st.ptdump, info->mm);
}

/* 启动期汇总每层所有可显示字段 mask，之后 note_page 只比较关心的位。 */
static void __init ptdump_initialize(void)
{
	unsigned i, j;

	for (i = 0; i < ARRAY_SIZE(kernel_pg_levels); i++)
		if (kernel_pg_levels[i].bits)
			for (j = 0; j < kernel_pg_levels[i].num; j++)
				kernel_pg_levels[i].mask |= kernel_pg_levels[i].bits[j].mask;
}

/* debugfs 默认转储 init_mm；markers/base_addr 由 ptdump_init 完成后冻结。 */
static struct ptdump_info kernel_ptdump_info __ro_after_init = {
	.mm		= &init_mm,
};

/*
 * 遍历整个内核 TTBR1 范围检查 W+X 和缺少 UXN 的映射。无输出 seq，使用
 * 两个哨兵 marker；返回 true 表示两项计数均为 0，否则打印汇总并 false。
 * 它是启动安全验证快照，不锁住映射在返回后永久不变。
 */
bool ptdump_check_wx(void)
{
	struct ptdump_pg_state st = {
		.seq = NULL,
		.marker = (struct addr_marker[]) {
			{ 0, NULL},
			{ -1, NULL},
		},
		.pg_level = &kernel_pg_levels[0],
		.level = -1,
		.check_wx = true,
		.ptdump = {
			.note_page_pte = note_page_pte,
			.note_page_pmd = note_page_pmd,
			.note_page_pud = note_page_pud,
			.note_page_p4d = note_page_p4d,
			.note_page_pgd = note_page_pgd,
			.note_page_flush = note_page_flush,
			.range = (struct ptdump_range[]) {
				{_PAGE_OFFSET(vabits_actual), ~0UL},
				{0, 0}
			}
		}
	};

	arm64_ptdump_walk_pgd(&st.ptdump, &init_mm);

	if (st.wx_pages || st.uxn_pages) {
		pr_warn("Checked W+X mappings: FAILED, %lu W+X pages found, %lu non-UXN pages found\n",
			st.wx_pages, st.uxn_pages);

		return false;
	} else {
		pr_info("Checked W+X mappings: passed, no W+X pages found\n");

		return true;
	}
}

/*
 * 初始化内核 VA 布局 marker、字段 mask 和 debugfs 文件。局部 m 根据实际
 * vabits/KASAN 配置构造，复制到 __ro_after_init 静态数组后才发布给全局
 * info，避免保存栈地址。返回 0；debugfs 创建失败不阻断启动。
 */
static int __init ptdump_init(void)
{
	u64 page_offset = _PAGE_OFFSET(vabits_actual);
	u64 vmemmap_start = (u64)virt_to_page((void *)page_offset);
	struct addr_marker m[] = {
		{ PAGE_OFFSET,		"Linear Mapping start" },
		{ PAGE_END,		"Linear Mapping end" },
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
		{ KASAN_SHADOW_START,   "Kasan shadow start" },
		{ KASAN_SHADOW_END,     "Kasan shadow end" },
#endif
		{ MODULES_VADDR,	"Modules start" },
		{ MODULES_END,		"Modules end" },
		{ VMALLOC_START,	"vmalloc() area" },
		{ VMALLOC_END,		"vmalloc() end" },
		{ vmemmap_start,	"vmemmap start" },
		{ VMEMMAP_END,		"vmemmap end" },
		{ PCI_IO_START,		"PCI I/O start" },
		{ PCI_IO_END,		"PCI I/O end" },
		{ FIXADDR_TOT_START,    "Fixmap start" },
		{ FIXADDR_TOP,	        "Fixmap end" },
		{ -1,			NULL },
	};
	static struct addr_marker address_markers[ARRAY_SIZE(m)] __ro_after_init;
	/* address_markers 的大小随本构建 KASAN 条件固定，复制后生命周期永久。 */

	kernel_ptdump_info.markers = memcpy(address_markers, m, sizeof(m));
	kernel_ptdump_info.base_addr = page_offset;

	ptdump_initialize();
	ptdump_debugfs_register(&kernel_ptdump_info, "kernel_page_tables");
	return 0;
}
device_initcall(ptdump_init);
