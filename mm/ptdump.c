// SPDX-License-Identifier: GPL-2.0

#include <linux/pagewalk.h>
#include <linux/debugfs.h>
#include <linux/ptdump.h>
#include <linux/kasan.h>
#include "internal.h"

/*
 * 通用 ptdump 层只负责安全地下钻 PGD→PTE，并把每个 leaf、hole 和层级权限
 * 交给体系结构提供的 ptdump_state 回调。x86/arm64/riscv 等调用者决定如何
 * 合并连续区间、打印属性及统计 W^X；本文件不解释体系结构位编码。
 */

#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
/*
 * This is an optimization for KASAN=y case. Since all kasan page tables
 * eventually point to the kasan_early_shadow_page we could call note_page()
 * right away without walking through lower level page tables. This saves
 * us dozens of seconds (minutes for 5-level config) while checking for
 * W+X mapping or reading kernel_page_tables debugfs file.
 */
/*
 * 上述优化利用 KASAN early shadow 的所有上级表最终共享同一个只读 PTE 页：
 * 一旦识别出该表页，继续逐层遍历只会重复报告相同映射。直接以统一 PTE 值
 * 通知调用者，并令 walker 跳过当前子树，可把五级页表的分钟级扫描降下来；
 * 它只适用于 GENERIC/SW_TAGS 的 early shadow，不适用于硬件标签模式。
 *
 * 业务背景：页表层回调识别 KASAN 共享子树后用本 helper 快速汇报整个区间。
 * 入参：walk 是同步 walker 借出的上下文，private 必须指向有效 ptdump_state；
 * addr 是当前子树起始虚拟地址。
 * 出参/返回：调用 note_page_pte() 后返回 0，并把 action 设为 ACTION_CONTINUE；
 * 不取得 state/mm 引用。
 * 注意事项：note_page_pte 必须非 NULL；ACTION_CONTINUE 表示当前层不再下钻，
 * 并非终止整个 walk。回调执行时遵循外层页表遍历和锁上下文，不能长期睡眠。
 */
static inline int note_kasan_page_table(struct mm_walk *walk,
					unsigned long addr)
{
	struct ptdump_state *st = walk->private;

	/* 用共享 shadow PTE 代表该地址后的同质子树，体系结构回调负责区间合并。 */
	st->note_page_pte(st, addr, kasan_early_shadow_pte[0]);

	walk->action = ACTION_CONTINUE;

	return 0;
}
#endif

/*
 * 业务背景：处理 walker 到达的 PGD 项，先累计顶层有效权限，再决定报告 leaf
 * 还是继续下钻 P4D；五级 KASAN 共享表可提前折叠。
 * 入参：pgd 为 walker 借出的当前项；addr/next 是其覆盖区间边界；walk 提供
 * mm、action 与借用的 ptdump_state，next 在本回调中无需单独使用。
 * 出参/返回：返回 0；leaf 时调用 note_page_pgd 并设置 ACTION_CONTINUE，普通
 * 非 leaf 保持默认 ACTION_SUBTREE；不修改页表、不转移引用。
 * 注意事项：note_page_pgd 必须有效，effective_prot_pgd 可选；pgdp_get() 生成
 * 一次值快照，页表稳定性依赖 ptdump_walk_pgd() 与体系结构附加同步。
 */
static int ptdump_pgd_entry(pgd_t *pgd, unsigned long addr,
			    unsigned long next, struct mm_walk *walk)
{
	struct ptdump_state *st = walk->private;
	pgd_t val = pgdp_get(pgd);

#if CONFIG_PGTABLE_LEVELS > 4 && \
		(defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS))
	/* early shadow P4D 的 linear-map alias 指向同一 page，命中即可跳过下级表。 */
	if (pgd_page(val) == virt_to_page(lm_alias(kasan_early_shadow_p4d)))
		return note_kasan_page_table(walk, addr);
#endif

	if (st->effective_prot_pgd)
		st->effective_prot_pgd(st, val);

	/* leaf 在本层已直接映射，ACTION_CONTINUE 防止把叶项误作下一层表指针。 */
	if (pgd_leaf(val)) {
		st->note_page_pgd(st, addr, val);
		walk->action = ACTION_CONTINUE;
	}

	return 0;
}

/*
 * 业务背景：处理可折叠或真实存在的 P4D 层，延续上级权限累计并报告本层 leaf。
 * 入参：p4d 是借用项；addr/next 为覆盖范围；walk->private 是借用 state。
 * 出参/返回：始终 0；leaf/共享 KASAN 子树会消费当前子树，普通目录继续下钻。
 * 注意事项：note_page_p4d 必须非 NULL、effective_prot_p4d 可选；不写页表，
 * p4dp_get() 的快照仅在本次同步回调内使用。
 */
static int ptdump_p4d_entry(p4d_t *p4d, unsigned long addr,
			    unsigned long next, struct mm_walk *walk)
{
	struct ptdump_state *st = walk->private;
	p4d_t val = p4dp_get(p4d);

#if CONFIG_PGTABLE_LEVELS > 3 && \
		(defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS))
	/* 真实四级以上配置才可能在此识别共享 early-shadow PUD 表。 */
	if (p4d_page(val) == virt_to_page(lm_alias(kasan_early_shadow_pud)))
		return note_kasan_page_table(walk, addr);
#endif

	if (st->effective_prot_p4d)
		st->effective_prot_p4d(st, val);

	/* 本层直接承载映射时汇报 P4D 值，并阻止 walker 把它解释为 PUD 表。 */
	if (p4d_leaf(val)) {
		st->note_page_p4d(st, addr, val);
		walk->action = ACTION_CONTINUE;
	}

	return 0;
}

/*
 * 业务背景：在 PUD 层累计有效权限，报告体系结构支持的 PUD leaf（常见为
 * 大页映射），或让 walker 继续到 PMD。
 * 入参：pud/addr/next/walk 均由 walker 借用传入，next 本地未使用。
 * 出参/返回：返回 0；leaf 或 KASAN 共享子树时 action=CONTINUE，否则继续下钻。
 * 注意事项：effective_prot_pud 可选而 note_page_pud 必须存在；读取不赋予
 * 页表页引用，稳定范围由外层 mmap 写锁/体系结构协议提供。
 */
static int ptdump_pud_entry(pud_t *pud, unsigned long addr,
			    unsigned long next, struct mm_walk *walk)
{
	struct ptdump_state *st = walk->private;
	pud_t val = pudp_get(pud);

#if CONFIG_PGTABLE_LEVELS > 2 && \
		(defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS))
	/* early-shadow PMD 表的所有后代同质，直接走统一 PTE 汇报。 */
	if (pud_page(val) == virt_to_page(lm_alias(kasan_early_shadow_pmd)))
		return note_kasan_page_table(walk, addr);
#endif

	if (st->effective_prot_pud)
		st->effective_prot_pud(st, val);

	/* PUD leaf 已覆盖整个大页区间，记录后跳过并不存在的 PMD/PTE 子树。 */
	if (pud_leaf(val)) {
		st->note_page_pud(st, addr, val);
		walk->action = ACTION_CONTINUE;
	}

	return 0;
}

/*
 * 业务背景：在下钻 PTE 前处理 PMD 权限与 PMD leaf；KASAN early-shadow PTE
 * 表是最后一个可整体折叠的共享层。
 * 入参：pmd 为借用项，addr/next 描述当前区间，walk 携带借用 state。
 * 出参/返回：返回 0；leaf/共享 shadow 时汇报并跳过子树，普通目录继续至 PTE。
 * 注意事项：effective_prot_pmd 可选，note_page_pmd/note_page_pte 必须有效；
 * 回调不拆大页、不加引用，也不自行持有 PTE 锁。
 */
static int ptdump_pmd_entry(pmd_t *pmd, unsigned long addr,
			    unsigned long next, struct mm_walk *walk)
{
	struct ptdump_state *st = walk->private;
	pmd_t val = pmdp_get(pmd);

#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	/* 所有映射落到同一 kasan_early_shadow_pte 页时，无需逐 PTE 读取。 */
	if (pmd_page(val) == virt_to_page(lm_alias(kasan_early_shadow_pte)))
		return note_kasan_page_table(walk, addr);
#endif

	if (st->effective_prot_pmd)
		st->effective_prot_pmd(st, val);
	/* PMD leaf（如大页）在本层终止；非 leaf 才允许 walker 进入 PTE 表。 */
	if (pmd_leaf(val)) {
		st->note_page_pmd(st, addr, val);
		walk->action = ACTION_CONTINUE;
	}

	return 0;
}

/*
 * 业务背景：页表 walk 到最底层后，把 PTE 权限纳入累计并把实际映射交给
 * 体系结构 note 回调，后者可合并相邻同属性页或统计 W^X。
 * 入参：pte 是 walker 借出的项；addr/next 是页范围；walk 携带借用 state。
 * 出参/返回：调用可选 effective_prot_pte 和必需 note_page_pte 后返回 0；
 * 无 action 修改，因为 PTE 已无子树。
 * 注意事项：walk_page_range_debug() 明确不会为 no-VMA PTE 回调持普通 PTE 锁；
 * ptep_get() 只取得一致宽度快照，调用者必须接受调试视图的瞬时性质。
 */
static int ptdump_pte_entry(pte_t *pte, unsigned long addr,
			    unsigned long next, struct mm_walk *walk)
{
	struct ptdump_state *st = walk->private;
	pte_t val = ptep_get(pte);

	if (st->effective_prot_pte)
		st->effective_prot_pte(st, val);

	st->note_page_pte(st, addr, val);

	return 0;
}

/*
 * 业务背景：页表 walker 遇到缺失的某层项时仍需通知输出状态机，否则相邻
 * 已映射区间可能跨越 hole 被错误合并。
 * 入参：addr/next 是 hole 范围，depth 是折叠页表层级修正后的 0..4；walk
 * 携带借用 ptdump_state，next 由 walker 用于推进而不传给 note 回调。
 * 出参/返回：按 depth 调用对应 note_page_* 并传零值项，返回 0；未知 depth
 * 不通知但仍继续 walk。
 * 注意事项：零项表达“该层无映射”而非真实页表对象；所有 note 回调均必须
 * 初始化。函数不修改页表、action 或 ownership。
 */
static int ptdump_hole(unsigned long addr, unsigned long next,
		       int depth, struct mm_walk *walk)
{
	struct ptdump_state *st = walk->private;
	pte_t pte_zero = {0};
	pmd_t pmd_zero = {0};
	pud_t pud_zero = {0};
	p4d_t p4d_zero = {0};
	pgd_t pgd_zero = {0};

	/* depth 与 note_page 层级一一对应，使折叠层架构也得到正确的 hole 边界。 */
	switch (depth) {
	case 4:
		/* PTE 深度 hole：以全零 PTE 结束或开启最底层空洞区间。 */
		st->note_page_pte(st, addr, pte_zero);
		break;
	case 3:
		/* PMD 深度 hole：下级 PTE 表不存在。 */
		st->note_page_pmd(st, addr, pmd_zero);
		break;
	case 2:
		/* PUD 深度 hole：下级 PMD 子树不存在。 */
		st->note_page_pud(st, addr, pud_zero);
		break;
	case 1:
		/* P4D 深度 hole：下级 PUD 子树不存在。 */
		st->note_page_p4d(st, addr, p4d_zero);
		break;
	case 0:
		/* PGD 深度 hole：整个顶层地址片段未映射。 */
		st->note_page_pgd(st, addr, pgd_zero);
		break;
	default:
		/* 防御未知 depth；不伪造一个错误层级的边界事件。 */
		break;
	}
	return 0;
}

/*
 * 通用只读 walk 操作表：每层目录先观察，leaf 由回调设置 CONTINUE 阻止下钻，
 * 最底层和 hole 均送入同一体系结构状态机。未提供 install/pre/post 回调，
 * 因而遍历不创建页表、不修改 VMA。
 */
static const struct mm_walk_ops ptdump_ops = {
	.pgd_entry	= ptdump_pgd_entry,
	.p4d_entry	= ptdump_p4d_entry,
	.pud_entry	= ptdump_pud_entry,
	.pmd_entry	= ptdump_pmd_entry,
	.pte_entry	= ptdump_pte_entry,
	.pte_hole	= ptdump_hole,
};

/*
 * 业务背景：体系结构 ptdump/W^X 实现以此为统一入口，在指定 mm/PGD 上遍历
 * st->range 的每个虚拟地址区间，并把事件同步送入 st 回调。
 * 入参：st 是调用者拥有的可写状态，range 必须以 start==end 哨兵结束且所有
 * note 回调有效；mm 为借用且非 NULL；pgd 可为 NULL（使用 mm->pgd）或借用的
 * 替代顶层表（如 KPTI user PGD）。
 * 出参/返回：void；遍历期间更新 st 的体系结构私有统计/输出，最后无条件调用
 * note_page_flush；不取得 mm/pgd/st 的长期引用。
 * 注意事项：get_online_mems() 阻止内存热插拔改变被转储布局，mmap 写锁满足
 * no-VMA walker 对页表拆除的要求；函数可睡眠。底层错误当前未向调用者传播，
 * range 与状态必须覆盖整个同步调用。
 */
void ptdump_walk_pgd(struct ptdump_state *st, struct mm_struct *mm, pgd_t *pgd)
{
	const struct ptdump_range *range = st->range;

	get_online_mems();
	/* no-VMA walk 可能跨越已摘除 VMA 的页表，读锁不足，必须取 mmap 写锁。 */
	mmap_write_lock(mm);
	/* start==end 是数组终止符；各有效 range 独立调用 walker，但共享聚合状态。 */
	while (range->start != range->end) {
		walk_page_range_debug(mm, range->start, range->end,
				      &ptdump_ops, pgd, st);
		range++;
	}
	mmap_write_unlock(mm);
	put_online_mems();

	/* Flush out the last page */
	/* walker 只在属性变化时推进输出，最后显式 flush 才会提交末个聚合区间。 */
	st->note_page_flush(st);
}

/*
 * 业务背景：debugfs check_wx_pages 的 show 回调触发一次体系结构内核页表 W^X
 * 自检，并把安全结论简化为单行文本。
 * 入参：m 是 seq_file 输出目标；v 为 seq 框架私有游标，本实现不使用。
 * 出参/返回：写入 SUCCESS 或 FAILED 后返回 0；实际违规细节可由体系结构检查
 * 另行打印，无 ownership 转移。
 * 注意事项：ptdump_check_wx() 返回 true 表示检查通过、false 表示发现违规；
 * 读取文件会执行完整页表 walk，可能耗时且结果只是当时快照。
 */
static int check_wx_show(struct seq_file *m, void *v)
{
	if (ptdump_check_wx())
		seq_puts(m, "SUCCESS\n");
	else
		seq_puts(m, "FAILED\n");

	return 0;
}

/* 生成 check_wx_open/read/llseek/release，私有数据为空且每次 open 重新检查。 */
DEFINE_SHOW_ATTRIBUTE(check_wx);

/*
 * 业务背景：设备初始化阶段发布 /sys/kernel/debug/check_wx_pages，只允许所有者
 * 读取以按需复查内核页表权限。
 * 入参：无。
 * 出参/返回：返回 0；debugfs 节点创建结果不传播，失败不阻止启动。
 * 注意事项：这是内建 initcall，无模块卸载清理；debugfs 持有 fops 静态对象，
 * 文件不存在或 debugfs 未挂载都不影响启动期 debug_checkwx 路径。
 */
static int ptdump_debugfs_init(void)
{
	debugfs_create_file("check_wx_pages", 0400, NULL, NULL, &check_wx_fops);

	return 0;
}

/* 在设备初始化阶段创建节点，此时体系结构页表 dump 支持已经初始化。 */
device_initcall(ptdump_debugfs_init);
