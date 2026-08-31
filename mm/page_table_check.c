// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2021, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#include <linux/kstrtox.h>
#include <linux/mm.h>
#include <linux/page_table_check.h>
#include <linux/swap.h>
#include <linux/leafops.h>

#undef pr_fmt
#define pr_fmt(fmt)	"page_table_check: " fmt

/*
 * 每个 struct page 的 page_ext 账本：匿名映射和文件映射必须互斥；匿名页允许多个
 * 只读映射，但至多一个可写映射。计数是页表安装/移除时的同步诊断，不是引用计数，
 * 违反不变量会 BUG，以便在破坏扩散前暴露错误页表操作。
 */
struct page_table_check {
	/* 当前用户页表中按匿名页类型登记的映射数。 */
	atomic_t anon_map_count;
	/* 当前用户页表中按文件页类型登记的映射数。 */
	atomic_t file_map_count;
};

/* 启动期最终开关；强制配置默认 true，early_param 可覆盖，初始化后内存可回收。 */
static bool __page_table_check_enabled __initdata =
				IS_ENABLED(CONFIG_PAGE_TABLE_CHECK_ENFORCED);

/* 热路径默认跳过检查；启用时 init 回调把该 static key 从 true 切换为 false。 */
DEFINE_STATIC_KEY_TRUE(page_table_check_disabled);
EXPORT_SYMBOL(page_table_check_disabled);

/*
 * early_page_table_check_param() - 解析 page_table_check= 启动参数。
 * @buf 为启动命令行借用字符串；返回 kstrtobool() 的 0 或负 errno，并把结果写入
 * __initdata 开关。只在 early boot 调用，无并发和长期 ownership。
 */
static int __init early_page_table_check_param(char *buf)
{
	return kstrtobool(buf, &__page_table_check_enabled);
}

/* 在 page_ext 计算布局前解析开关，确保 need 回调得到最终选择。 */
early_param("page_table_check", early_page_table_check_param);

/*
 * need_page_table_check() - 告知 page_ext 是否为每页预留检查账本。
 * 无参数，返回启动期最终开关；仅 page_ext 初始化阶段调用，返回后不再依赖该函数。
 */
static bool __init need_page_table_check(void)
{
	return __page_table_check_enabled;
}

/*
 * init_page_table_check() - 在 page_ext backing 就绪后发布热路径启用状态。
 * 无参数/返回值；关闭时保持默认 static key，启用时 disable “disabled” 分支。
 * 仅启动期调用，static key patch 完成后页表包装才进入本文件检查。
 */
static void __init init_page_table_check(void)
{
	if (!__page_table_check_enabled)
		return;
	static_branch_disable(&page_table_check_disabled);
}

/* page_ext 为每个有效 struct page 分配私有账本，并在共享 flags 之外单独存放。 */
struct page_ext_operations page_table_check_ops = {
	.size = sizeof(struct page_table_check),
	.need = need_page_table_check,
	.init = init_page_table_check,
	.need_shared_flags = false,
};

/*
 * get_page_table_check() - 从借用 page_ext 定位本功能的私有账本。
 * @page_ext 必须非 NULL 且已按 page_table_check_ops 布局；返回内部借用指针。
 * 缺失 page_ext 是初始化/调用时序缺陷，直接 BUG；无锁且不转移 ownership。
 */
static struct page_table_check *get_page_table_check(struct page_ext *page_ext)
{
	BUG_ON(!page_ext);
	return page_ext_data(page_ext, &page_table_check_ops);
}

/*
 * An entry is removed from the page table, decrement the counters for that page
 * verify that it is of correct type and counters do not become negative.
 */
/*
 * 页表项被移除时，递减对应页的计数，并验证页类型一致且计数不会变成负数。
 */
/*
 * page_table_check_clear() - 注销连续 PFN 区间的一次用户页表映射。
 * @pfn 为首物理页号，@pgcnt 为 base-page 数；无返回值。无效 PFN（如设备映射）
 * 不跟踪；有效页不得为 slab。函数按首页当前 PageAnon 类型选择整段账本，持
 * page_ext RCU 读锁逐页原子递减；类型混用或重复 clear 立即 BUG。
 */
static void page_table_check_clear(unsigned long pfn, unsigned long pgcnt)
{
	struct page_ext_iter iter;
	struct page_ext *page_ext;
	struct page *page;
	bool anon;

	/* 与 clear 对称：没有 struct page 的 PFN 既无 page_ext，也不进入类型检查。 */
	if (!pfn_valid(pfn))
		/* 无 struct page/page_ext 的映射不在本检查器覆盖范围内。 */
		return;

	page = pfn_to_page(pfn);
	/* 把 slab 页映射进用户页表本身就是不可恢复的内核不变量破坏。 */
	BUG_ON(PageSlab(page));
	anon = PageAnon(page);

	rcu_read_lock();
	/* page_ext 迭代器处理跨 section 区间，RCU 保证 backing 在遍历中稳定。 */
	for_each_page_ext(page, pgcnt, page_ext, iter) {
		struct page_table_check *ptc = get_page_table_check(page_ext);

		if (anon) {
			/* 当前匿名类型要求文件计数为零，再消费一份匿名映射登记。 */
			BUG_ON(atomic_read(&ptc->file_map_count));
			BUG_ON(atomic_dec_return(&ptc->anon_map_count) < 0);
		} else {
			/* 文件页与匿名页镜像互斥；负数暴露未配对 clear。 */
			BUG_ON(atomic_read(&ptc->anon_map_count));
			BUG_ON(atomic_dec_return(&ptc->file_map_count) < 0);
		}
	}
	rcu_read_unlock();
}

/*
 * A new entry is added to the page table, increment the counters for that page
 * verify that it is of correct type and is not being mapped with a different
 * type to a different process.
 */
/*
 * 新页表项安装时，递增对应页计数，并验证页类型正确且未以另一类型跨进程共享。
 */
/*
 * page_table_check_set() - 登记连续 PFN 区间的一次用户页表映射。
 * @pfn/@pgcnt 指定 base-page 区间，@rw 表示新映射可写；无返回值。无效 PFN 跳过，
 * slab 页 BUG。匿名/文件计数必须互斥；匿名可写映射递增后若超过 1 则 BUG，
 * 只读匿名和文件映射可多重存在。page_ext backing 在 RCU 读侧借用。
 */
static void page_table_check_set(unsigned long pfn, unsigned long pgcnt,
				 bool rw)
{
	struct page_ext_iter iter;
	struct page_ext *page_ext;
	struct page *page;
	bool anon;

	/* 没有 struct page 的 PFN 也没有 page_ext，设备类映射保持原样跳过。 */
	if (!pfn_valid(pfn))
		return;

	page = pfn_to_page(pfn);
	BUG_ON(PageSlab(page));
	anon = PageAnon(page);

	rcu_read_lock();
	/* 每个 base page 独立记账，huge leaf 因而能覆盖整段物理页。 */
	for_each_page_ext(page, pgcnt, page_ext, iter) {
		struct page_table_check *ptc = get_page_table_check(page_ext);

		if (anon) {
			/* 文件映射必须为零；只有可写匿名映射要求全局唯一。 */
			BUG_ON(atomic_read(&ptc->file_map_count));
			BUG_ON(atomic_inc_return(&ptc->anon_map_count) > 1 && rw);
		} else {
			/* 文件页不得同时保留匿名登记，inc 溢出成负数也视为损坏。 */
			BUG_ON(atomic_read(&ptc->anon_map_count));
			BUG_ON(atomic_inc_return(&ptc->file_map_count) < 0);
		}
	}
	rcu_read_unlock();
}

/*
 * page is on free list, or is being allocated, verify that counters are zeroes
 * crash if they are not.
 */
/*
 * 页进入 free list 或刚被分配时，所有映射计数都必须为零，否则立即崩溃。
 */
/*
 * __page_table_check_zero() - 在 buddy ownership 交接边界验证无残留用户映射。
 * @page 为借用的 order 对齐首页，@order 定义 2^order 个 base page；无返回值。
 * slab 页、任一匿名/文件非零计数均 BUG。调用点已由 static key 门控；函数持
 * page_ext RCU 读锁遍历，但不修改账本，适用于 alloc 与 free 两个方向。
 */
void __page_table_check_zero(struct page *page, unsigned int order)
{
	struct page_ext_iter iter;
	struct page_ext *page_ext;

	BUG_ON(PageSlab(page));

	rcu_read_lock();
	/* alloc 检查旧 owner 是否清干净，free 检查当前 owner 是否先撤完页表。 */
	for_each_page_ext(page, 1 << order, page_ext, iter) {
		struct page_table_check *ptc = get_page_table_check(page_ext);

		BUG_ON(atomic_read(&ptc->anon_map_count));
		BUG_ON(atomic_read(&ptc->file_map_count));
	}
	rcu_read_unlock();
}

/*
 * __page_table_check_pte_clear() - 在移除一个用户 PTE 前注销其物理页登记。
 * @mm/@addr/@pte 均为页表操作路径借用快照；无返回值。init_mm 不跟踪，非用户可
 * 访问或非 page-backed PTE 跳过；其余清除一个 base page 计数。
 */
void __page_table_check_pte_clear(struct mm_struct *mm, unsigned long addr,
				  pte_t pte)
{
	if (&init_mm == mm)
		return;

	if (pte_user_accessible_page(mm, addr, pte))
		page_table_check_clear(pte_pfn(pte), PAGE_SIZE >> PAGE_SHIFT);
}
EXPORT_SYMBOL(__page_table_check_pte_clear);

/*
 * __page_table_check_pmd_clear() - 注销一个用户可访问 PMD leaf 的整段映射。
 * @mm/@addr/@pmd 为借用快照；init_mm 或非用户 page leaf 跳过，否则递减
 * PMD_SIZE/PAGE_SIZE 个账本。调用者负责页表锁和“先记账再替换”的顺序。
 */
void __page_table_check_pmd_clear(struct mm_struct *mm, unsigned long addr,
				  pmd_t pmd)
{
	if (&init_mm == mm)
		return;

	if (pmd_user_accessible_page(mm, addr, pmd))
		page_table_check_clear(pmd_pfn(pmd), PMD_SIZE >> PAGE_SHIFT);
}
EXPORT_SYMBOL(__page_table_check_pmd_clear);

/*
 * __page_table_check_pud_clear() - 注销一个用户可访问 PUD leaf 的整段映射。
 * 参数/ownership/锁约束与 PMD 版本相同，覆盖 PUD_SIZE/PAGE_SIZE 个 base page；
 * 无返回值，init_mm 与非 page-backed 项不参与检查。
 */
void __page_table_check_pud_clear(struct mm_struct *mm, unsigned long addr,
				  pud_t pud)
{
	if (&init_mm == mm)
		return;

	if (pud_user_accessible_page(mm, addr, pud))
		page_table_check_clear(pud_pfn(pud), PUD_SIZE >> PAGE_SHIFT);
}
EXPORT_SYMBOL(__page_table_check_pud_clear);

/* Whether the swap entry cached writable information */
/* 判断非 present softleaf/swap 项是否缓存了可写设备私有页或迁移项信息。 */
/*
 * softleaf_cached_writable() - 合并两类“可写状态被缓存”判断。
 * @entry 按值输入；返回 true 表示 device-private-write 或 migration-write。
 * 无锁无副作用，用于校验 uffd-wp 与可写缓存不会同时成立。
 */
static inline bool softleaf_cached_writable(softleaf_t entry)
{
	return softleaf_is_device_private_write(entry) ||
		softleaf_is_migration_write(entry);
}

/*
 * page_table_check_pte_flags() - 检查 PTE 的 userfaultfd write-protect 编码不变量。
 * @pte 按值输入；无返回值。present 项不能同时 uffd-wp 且 writable；非 present
 * uffd-wp 项的 softleaf 也不能缓存 writable。冲突仅 WARN_ON_ONCE，随后仍记账。
 */
static void page_table_check_pte_flags(pte_t pte)
{
	if (pte_present(pte)) {
		WARN_ON_ONCE(pte_uffd_wp(pte) && pte_write(pte));
	} else if (pte_swp_uffd_wp(pte)) {
		const softleaf_t entry = softleaf_from_pte(pte);

		WARN_ON_ONCE(softleaf_cached_writable(entry));
	}
}

/*
 * __page_table_check_ptes_set() - 用同一新 PTE 批量替换并登记 @nr 个连续槽位。
 * @mm/@addr 定位用户范围，@ptep 借用旧 PTE 数组，@pte 是新值，@nr 是项数。
 * init_mm 跳过；先校验新 flags，再逐项注销旧映射，最后若新项用户可访问则从
 * pte_pfn(@pte) 起登记 @nr 页。无返回值；调用者必须持相应页表锁并随后真实写项。
 */
void __page_table_check_ptes_set(struct mm_struct *mm, unsigned long addr,
				 pte_t *ptep, pte_t pte, unsigned int nr)
{
	unsigned int i;

	if (&init_mm == mm)
		return;

	page_table_check_pte_flags(pte);

	/* 必须先撤旧账再加新账，原地替换同一页时也不会瞬间形成重复可写映射。 */
	for (i = 0; i < nr; i++)
		__page_table_check_pte_clear(mm, addr + PAGE_SIZE * i, ptep_get(ptep + i));
	if (pte_user_accessible_page(mm, addr, pte))
		page_table_check_set(pte_pfn(pte), nr, pte_write(pte));
}
EXPORT_SYMBOL(__page_table_check_ptes_set);

/*
 * page_table_check_pmd_flags() - 校验 PMD leaf 的 uffd-wp 与 writable 互斥。
 * @pmd 按值输入；present 与 softleaf 两种编码分别检查，冲突 WARN_ON_ONCE。
 * 无 ownership/锁操作，调用者仍继续完成旧账清除和新账登记。
 */
static inline void page_table_check_pmd_flags(pmd_t pmd)
{
	if (pmd_present(pmd)) {
		if (pmd_uffd_wp(pmd))
			WARN_ON_ONCE(pmd_write(pmd));
	} else if (pmd_swp_uffd_wp(pmd)) {
		const softleaf_t entry = softleaf_from_pmd(pmd);

		WARN_ON_ONCE(softleaf_cached_writable(entry));
	}
}

/*
 * __page_table_check_pmds_set() - 批量替换 PMD leaf 并同步每个 base-page 账本。
 * @pmdp 借用旧项数组，@pmd 是首个新 leaf，@nr 是连续项数；@mm/@addr 定位范围。
 * init_mm 跳过。先校验 flags、逐 PMD 清旧账，再从新 PFN 起登记 stride*@nr 页，
 * @rw 取 pmd_write()。无返回值；页表锁和真实写入顺序由架构包装保证。
 */
void __page_table_check_pmds_set(struct mm_struct *mm, unsigned long addr,
		pmd_t *pmdp, pmd_t pmd, unsigned int nr)
{
	unsigned long stride = PMD_SIZE >> PAGE_SHIFT;
	unsigned int i;

	if (&init_mm == mm)
		return;

	page_table_check_pmd_flags(pmd);

	/* stride 把 PMD 项数换算成 page_ext 的 base-page 数。 */
	for (i = 0; i < nr; i++)
		__page_table_check_pmd_clear(mm, addr + PMD_SIZE * i, *(pmdp + i));
	if (pmd_user_accessible_page(mm, addr, pmd))
		page_table_check_set(pmd_pfn(pmd), stride * nr, pmd_write(pmd));
}
EXPORT_SYMBOL(__page_table_check_pmds_set);

/*
 * __page_table_check_puds_set() - 批量替换 PUD leaf 并同步整段物理页账本。
 * 参数与 PMD 版本对应，stride=PUD_SIZE/PAGE_SIZE；先逐项清旧账，再按新 PUD
 * 的 PFN/write 属性登记 stride*@nr 页。init_mm/非用户 leaf 跳过相应操作；
 * 无返回值，调用者负责页表锁及随后发布真实 PUD。
 */
void __page_table_check_puds_set(struct mm_struct *mm, unsigned long addr,
		pud_t *pudp, pud_t pud,	unsigned int nr)
{
	unsigned long stride = PUD_SIZE >> PAGE_SHIFT;
	unsigned int i;

	if (&init_mm == mm)
		return;

	/* PUD 当前没有本文件专属 flags 检查，仍严格执行先 clear 后 set 的账本顺序。 */
	for (i = 0; i < nr; i++)
		__page_table_check_pud_clear(mm, addr + PUD_SIZE * i, *(pudp + i));
	if (pud_user_accessible_page(mm, addr, pud))
		page_table_check_set(pud_pfn(pud), stride * nr, pud_write(pud));
}
EXPORT_SYMBOL(__page_table_check_puds_set);

/*
 * __page_table_check_pte_clear_range() - 在折叠/替换 PMD 前清除其整张 PTE 表账本。
 * @mm/@addr 定位范围，@pmd 是借用的旧非 leaf 表项快照；无返回值。init_mm、bad
 * 或 leaf PMD 跳过。成功映射 PTE 页后遍历 PTRS_PER_PTE 项逐一 clear，并以原
 * 起点对称 pte_unmap；映射失败告警后返回。调用者负责 mmap/page-table 锁稳定性。
 */
void __page_table_check_pte_clear_range(struct mm_struct *mm,
					unsigned long addr,
					pmd_t pmd)
{
	if (&init_mm == mm)
		return;

	if (!pmd_bad(pmd) && !pmd_leaf(pmd)) {
		/* 临时映射页表页只在本分支有效，最终必须用初始指针解除。 */
		pte_t *ptep = pte_offset_map(&pmd, addr);
		unsigned long i;

		if (WARN_ON(!ptep))
			return;
		for (i = 0; i < PTRS_PER_PTE; i++) {
			/* clear 自行过滤非 present/非用户项；地址与指针保持逐页同步推进。 */
			__page_table_check_pte_clear(mm, addr, ptep_get(ptep));
			addr += PAGE_SIZE;
			ptep++;
		}
		pte_unmap(ptep - PTRS_PER_PTE);
	}
}
