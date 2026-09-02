// SPDX-License-Identifier: GPL-2.0
/*
 * mm/debug.c
 *
 * mm/ specific debug routines.
 *
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/trace_events.h>
#include <linux/memcontrol.h>
#include <trace/events/mmflags.h>
#include <linux/migrate.h>
#include <linux/page_owner.h>
#include <linux/ctype.h>

#include "internal.h"
#include <trace/events/migrate.h>

/* 本文件只生成诊断快照：不取得 page/folio/VMA/mm 引用，也不承诺并发一致性。 */
/*
 * Define EM() and EMe() so that MIGRATE_REASON from trace/events/migrate.h can
 * be used to populate migrate_reason_names[].
 */
/* 中文翻译：重定义 EM/EMe，把迁移原因 X-macro 展开成名字数组的初始化项。 */
#undef EM
#undef EMe
#define EM(a, b)	b,
#define EMe(a, b)	b

/* 下标与 enum migrate_reason 一一对应，供 trace 输出把数值原因还原为稳定文本。 */
const char *migrate_reason_names[MR_TYPES] = {
	MIGRATE_REASON
};

/* 三张表均以 {0,NULL} 结尾，trace %pG* 格式化器借用它们，不接管字符串。 */
const struct trace_print_flags pageflag_names[] = {
	__def_pageflag_names,
	{0, NULL}
};

/* GFP bit 到名称的生成表，与 trace/events/mmflags.h 的宏定义保持单一来源。 */
const struct trace_print_flags gfpflag_names[] = {
	__def_gfpflag_names,
	{0, NULL}
};

/* VMA 位图名称表同样由 trace 头生成，避免诊断名称与实际位号漂移。 */
const struct trace_print_flags vmaflag_names[] = {
	__def_vmaflag_names,
	{0, NULL}
};

/* 指定初始化器把高字节 page_type 编码映射到稠密数组下标。 */
#define DEF_PAGETYPE_NAME(_name) [PGTY_##_name - 0xf0] =  __stringify(_name)

/* 字符串为静态只读存储；空洞下标不会由合法 PGTY_* 编码访问。 */
static const char *page_type_names[] = {
	DEF_PAGETYPE_NAME(slab),
	DEF_PAGETYPE_NAME(hugetlb),
	DEF_PAGETYPE_NAME(offline),
	DEF_PAGETYPE_NAME(guard),
	DEF_PAGETYPE_NAME(table),
	DEF_PAGETYPE_NAME(buddy),
	DEF_PAGETYPE_NAME(unaccepted),
};

/*
 * page_type_name() - 把 struct page 的高字节类型编码转成诊断名称。
 * @page_type 是瞬时值；返回静态字符串，不需释放。合法范围按 PGTY 基址换算，
 * 越界返回 "unknown"。函数无锁、无副作用，可在告警路径使用。
 */
static const char *page_type_name(unsigned int page_type)
{
	unsigned i = (page_type >> 24) - 0xf0;

	/* 先做无符号边界检查，低于基址的减法下溢也会自然落入 unknown。 */
	if (i >= ARRAY_SIZE(page_type_names))
		return "unknown";
	return page_type_names[i];
}

/*
 * __dump_folio() - 打印已经快照化的 folio/page 诊断主体。
 * @folio/@page 均只借用，@pfn 是原页帧号，@idx 是 page 在 folio 内的偏移；无
 * 返回值。调用者通常传 page_snapshot，因而无需页锁且允许数值近似；函数可能
 * 触发 printk/hex dump，不应用于要求低延迟的热路径。分基本计数、大 folio、
 * memcg/类型/flags、原始结构四阶段输出，不改变任何被观察对象。
 */
static void __dump_folio(const struct folio *folio, const struct page *page,
		unsigned long pfn, unsigned long idx)
{
	/* mapping 与计数都是无引用快照；page 类型编码借用 _mapcount 槽时归零显示。 */
	struct address_space *mapping = folio_mapping(folio);
	int mapcount = atomic_read(&page->_mapcount) + 1;
	char *type = "";

	if (page_mapcount_is_type(mapcount))
		mapcount = 0;

	/* 第一阶段输出定位页所需的引用、映射、索引和 PFN。 */
	pr_warn("page: refcount:%d mapcount:%d mapping:%p index:%#lx pfn:%#lx\n",
			folio_ref_count(folio), mapcount, mapping,
			folio->index + idx, pfn);
	/* 第二阶段仅对大 folio 补充 head 聚合 mapcount/pincount。 */
	if (folio_test_large(folio)) {
		int pincount = 0;

		/* 没有独立 pincount 字段的 folio 用 0，避免解释不存在的存储。 */
		if (folio_has_pincount(folio))
			pincount = atomic_read(&folio->_pincount);

		pr_warn("head: order:%u mapcount:%d entire_mapcount:%d nr_pages_mapped:%d pincount:%d\n",
				folio_order(folio),
				folio_mapcount(folio),
				folio_entire_mapcount(folio),
				folio_nr_pages_mapped(folio),
				pincount);
	}

#ifdef CONFIG_MEMCG
	/* memcg_data 是编码指针/标志的原始快照，不能在此解引用或取得 css 引用。 */
	if (folio->memcg_data)
		pr_warn("memcg:%lx\n", folio->memcg_data);
#endif
	/* 第三阶段区分 KSM/匿名/文件页；只有稳定的快照 mapping 才交给 dump_mapping。 */
	if (folio_test_ksm(folio))
		type = "ksm ";
	else if (folio_test_anon(folio))
		type = "anon ";
	else if (mapping)
		dump_mapping(mapping);
	/* 编译期保证 flags 名称表覆盖所有 pageflag 再加终止哨兵。 */
	BUILD_BUG_ON(ARRAY_SIZE(pageflag_names) != __NR_PAGEFLAGS + 1);

	/*
	 * Accessing the pageblock without the zone lock. It could change to
	 * "isolate" again in the meantime, but since we are just dumping the
	 * state for debugging, it should be fine to accept a bit of
	 * inaccuracy here due to racing.
	 */
	/* 中文翻译：未持 zone lock 读取 pageblock 可能与 isolate 竞争，调试输出容忍误差。 */
	pr_warn("%sflags: %pGp%s\n", type, &folio->flags,
		is_migrate_cma_folio(folio, pfn) ? " CMA" : "");
	/* page_type 只在复用该字段的特殊页上有效，普通页不得强行解码。 */
	if (page_has_type(&folio->page))
		pr_warn("page_type: %x(%s)\n", folio->page.page_type >> 24,
				page_type_name(folio->page.page_type));

	/* 最后输出 struct page 原始字节；大 folio 再显示 head 的两个 page 大小。 */
	print_hex_dump(KERN_WARNING, "raw: ", DUMP_PREFIX_NONE, 32,
			sizeof(unsigned long), page,
			sizeof(struct page), false);
	if (folio_test_large(folio))
		print_hex_dump(KERN_WARNING, "head: ", DUMP_PREFIX_NONE, 32,
			sizeof(unsigned long), folio,
			2 * sizeof(struct page), false);
}

/*
 * __dump_page() - 先捕获自洽 page_snapshot，再复用 folio 打印器。
 * @page 由调用者借用且可能正处于错误路径；无返回值。snapshot_page 尽力复制
 * head/page/PFN/index，若并发复合页转换导致不匹配则告警但仍打印快照；函数不
 * 获得页引用、不阻止释放，适合 VM_BUG/WARN 的观察用途。
 */
static void __dump_page(const struct page *page)
{
	struct page_snapshot ps;

	/* 快照是后续所有解引用的边界，避免混用实时 page 与复制出的 folio head。 */
	snapshot_page(&ps, page);
	if (!snapshot_page_is_faithful(&ps))
		pr_warn("page does not match folio\n");

	__dump_folio(&ps.folio_snapshot, &ps.page_snapshot, ps.pfn, ps.idx);
}

/*
 * dump_page() - 对外输出页状态、触发原因和 page_owner 分配栈。
 * @page 只借用，调用者须保证至少可安全测试 poison 标志；@reason 可空且不接管。
 * 无返回值，可能大量打印。poison 页的 struct page 尚未初始化，必须绕过字段
 * 解引用；正常页经快照打印，最后无论哪条路径都尝试输出 owner 信息。
 */
void dump_page(const struct page *page, const char *reason)
{
	/* poison 快速路径避免把填充模式误作有效的 refcount/mapping。 */
	if (PagePoisoned(page))
		pr_warn("page:%p is uninitialized and poisoned\n", page);
	else
		__dump_page(page);
	/* reason 和 owner 是补充上下文；owner helper 自行处理功能关闭/无记录情形。 */
	if (reason)
		pr_warn("page dumped because: %s\n", reason);
	dump_page_owner(page);
}
EXPORT_SYMBOL(dump_page);

#ifdef CONFIG_DEBUG_VM

/*
 * dump_vma() - 输出一个 VMA 的边界、后端对象、引用和完整 flags。
 * @vma 只借用且不可空；无返回值。调用者通常在持 mmap/vma 锁或致命诊断路径
 * 调用，函数本身不加锁、不取 file/anon_vma 引用，故各字段只是竞态快照。
 * CONFIG_PER_VMA_LOCK 下额外输出 vm_refcnt，%pGv 使用全局名称表解码位图。
 */
void dump_vma(const struct vm_area_struct *vma)
{
	/* 格式串和实参的配置分支必须同步，避免可选字段改变后续参数位置。 */
	pr_emerg("vma %px start %px end %px mm %px\n"
		"prot %lx anon_vma %px vm_ops %px\n"
		"pgoff %lx file %px private_data %px\n"
#ifdef CONFIG_PER_VMA_LOCK
		"refcnt %x\n"
#endif
		"flags: %#lx(%pGv)\n",
		/* 首组实参定位 VMA 与地址边界，次组给出保护、backing 和私有数据。 */
		vma, (void *)vma->vm_start, (void *)vma->vm_end, vma->vm_mm,
		(unsigned long)pgprot_val(vma->vm_page_prot),
		vma->anon_vma, vma->vm_ops, vma->vm_pgoff,
		/* 可选 refcnt 和最终 flags 必须与上方条件格式严格保持同序。 */
		vma->vm_file, vma->vm_private_data,
#ifdef CONFIG_PER_VMA_LOCK
		refcount_read(&vma->vm_refcnt),
#endif
		vma->vm_flags, &vma->vm_flags);
}
EXPORT_SYMBOL(dump_vma);

/*
 * dump_mm() - 输出地址空间布局、计数、资源边界和配置相关子系统状态。
 * @mm 是调用者保持存活的借用指针；无返回值。函数只做原子/READ 风格快照，不
 * 获取 mmap_lock，因此 map_count、VMA 统计及可选指针可能来自相邻时刻；用于
 * 崩溃诊断而非一致性决策。输出完成后调用者仍拥有 mm，通常继续 dump VMA。
 */
void dump_mm(const struct mm_struct *mm)
{
	/* 第一组是地址空间基址、页表和双引用计数；mm_users 与 mm_count 含义不同。 */
	pr_emerg("mm %px task_size %lu\n"
		"mmap_base %lu mmap_legacy_base %lu\n"
		"pgd %px mm_users %d mm_count %d pgtables_bytes %lu map_count %d\n"
		"hiwater_rss %lx hiwater_vm %lx total_vm %lx locked_vm %lx\n"
		"pinned_vm %llx data_vm %lx exec_vm %lx stack_vm %lx\n"
		/* 第二组记录 ELF/堆栈/参数环境边界，单位均是虚拟地址或页数。 */
		"start_code %lx end_code %lx start_data %lx end_data %lx\n"
		"start_brk %lx brk %lx start_stack %lx\n"
		"arg_start %lx arg_end %lx env_start %lx env_end %lx\n"
		"binfmt %px flags %*pb\n"
#ifdef CONFIG_AIO
		/* AIO 表仅在相应配置存在，保持格式串与下方实参同条件编译。 */
		"ioctx_table %px\n"
#endif
#ifdef CONFIG_MEMCG
		/* owner 是 memcg 归属任务的借用快照，不在诊断路径解引用。 */
		"owner %px "
#endif
		"exe_file %px\n"
#ifdef CONFIG_MMU_NOTIFIER
		/* notifier 订阅头用于判断外部 MMU 是否参与，不遍历可能并发变化的链。 */
		"notifier_subscriptions %px\n"
#endif
#ifdef CONFIG_NUMA_BALANCING
		/* NUMA 三元组描述下次扫描时间、地址游标和轮次。 */
		"numa_next_scan %lu numa_scan_offset %lu numa_scan_seq %d\n"
#endif
		"tlb_flush_pending %d\n"
		"def_flags: %#lx(%pGv)\n",

		/* 实参按上述物理分组排列，任何配置新增字段必须成对修改两处。 */
		mm, mm->task_size,
		mm->mmap_base, mm->mmap_legacy_base,
		mm->pgd, atomic_read(&mm->mm_users),
		atomic_read(&mm->mm_count),
		mm_pgtables_bytes(mm),
		mm->map_count,
		/* VM 计数单位为页；pinned_vm 用 atomic64 快照以容纳长期 pin 总量。 */
		mm->hiwater_rss, mm->hiwater_vm, mm->total_vm, mm->locked_vm,
		(u64)atomic64_read(&mm->pinned_vm),
		mm->data_vm, mm->exec_vm, mm->stack_vm,
		mm->start_code, mm->end_code, mm->start_data, mm->end_data,
		mm->start_brk, mm->brk, mm->start_stack,
		mm->arg_start, mm->arg_end, mm->env_start, mm->env_end,
		mm->binfmt, NUM_MM_FLAG_BITS, __mm_flags_get_bitmap(mm),
#ifdef CONFIG_AIO
		/* 以下可选实参与格式串采用完全相同的 Kconfig 条件。 */
		mm->ioctx_table,
#endif
#ifdef CONFIG_MEMCG
		/* memcg owner 只作地址打印；这里不尝试稳定或解引用 task_struct。 */
		mm->owner,
#endif
		mm->exe_file,
#ifdef CONFIG_MMU_NOTIFIER
		/* notifier/AIO/NUMA 字段均是配置存在时的裸快照。 */
		mm->notifier_subscriptions,
#endif
#ifdef CONFIG_NUMA_BALANCING
		mm->numa_next_scan, mm->numa_scan_offset, mm->numa_scan_seq,
#endif
		/* tlb_flush_pending 反映批量页表修改是否仍欠一次失效。 */
		atomic_read(&mm->tlb_flush_pending),
		mm->def_flags, &mm->def_flags
	);
}
EXPORT_SYMBOL(dump_mm);

/*
 * dump_vmg() - 展开一次 VMA merge 决策的输入、计划状态和相邻对象。
 * @vmg 可空，@reason 可空且均只借用；无返回值。调用者应持有使 mm/VMA/vmi
 * 存活的锁，本函数不加锁。先打印聚合结构，再分别递归 dump mm/prev/middle/
 * next，DEBUG_VM_MAPLE_TREE 下还打印迭代器树；NULL 分支显式保留缺失关系。
 */
void dump_vmg(const struct vma_merge_struct *vmg, const char *reason)
{
	if (reason)
		pr_warn("vmg %px dumped because: %s\n", vmg, reason);

	/* NULL 是合法诊断输入；报告原因后必须在任何字段解引用前返回。 */
	if (!vmg) {
		pr_warn("vmg %px state: (NULL)\n", vmg);
		return;
	}

	/* 第一阶段打印 merge 游标、四个 VMA 角色、目标范围和 backing 策略。 */
	pr_warn("vmg %px state: mm %px pgoff %lx\n"
		"vmi %px [%lx,%lx)\n"
		"prev %px middle %px next %px target %px\n"
		"start %lx end %lx flags %lx\n"
		"file %px anon_vma %px policy %px\n"
		"uffd_ctx %px\n"
		"anon_name %px\n"
		/* state/布尔字段描述规划器将扩展、调界或移除哪些相邻 VMA。 */
		"state %x\n"
		"just_expand %d\n"
		"__adjust_middle_start %d __adjust_next_start %d\n"
		"__remove_middle %d __remove_next %d\n",
		/* vmi 可空，条件表达式避免为了打印范围而解引用空迭代器。 */
		vmg, vmg->mm, vmg->pgoff,
		vmg->vmi, vmg->vmi ? vma_iter_addr(vmg->vmi) : 0,
		vmg->vmi ? vma_iter_end(vmg->vmi) : 0,
		vmg->prev, vmg->middle, vmg->next, vmg->target,
		vmg->start, vmg->end, vmg->vm_flags,
		vmg->file, vmg->anon_vma, vmg->policy,
#ifdef CONFIG_USERFAULTFD
		/* 关闭 userfaultfd 时用显式空指针占住格式参数位置。 */
		vmg->uffd_ctx.ctx,
#else
		(void *)0,
#endif
		vmg->anon_name,
		(int)vmg->state,
		vmg->just_expand,
		vmg->__adjust_middle_start, vmg->__adjust_next_start,
		vmg->__remove_middle, vmg->__remove_next);

	/* 第二阶段逐个展开关联对象；每个分支都打印存在或 NULL，便于还原拓扑。 */
	if (vmg->mm) {
		pr_warn("vmg %px mm:\n", vmg);
		dump_mm(vmg->mm);
	} else {
		pr_warn("vmg %px mm: (NULL)\n", vmg);
	}

	/* prev 是目标范围之前的候选相邻 VMA，只借用到 dump_vma 返回。 */
	if (vmg->prev) {
		pr_warn("vmg %px prev:\n", vmg);
		dump_vma(vmg->prev);
	} else {
		pr_warn("vmg %px prev: (NULL)\n", vmg);
	}

	/* middle 通常覆盖修改起点，可能在计划中被调界或完全移除。 */
	if (vmg->middle) {
		pr_warn("vmg %px middle:\n", vmg);
		dump_vma(vmg->middle);
	} else {
		pr_warn("vmg %px middle: (NULL)\n", vmg);
	}

	/* next 是范围之后的候选相邻 VMA，和 prev 分开显示以识别双向合并。 */
	if (vmg->next) {
		pr_warn("vmg %px next:\n", vmg);
		dump_vma(vmg->next);
	} else {
		pr_warn("vmg %px next: (NULL)\n", vmg);
	}

#ifdef CONFIG_DEBUG_VM_MAPLE_TREE
	/* 深度树 dump 成本高，只在专用调试配置且 vmi 有效时执行。 */
	if (vmg->vmi) {
		pr_warn("vmg %px vmi:\n", vmg);
		vma_iter_dump_tree(vmg->vmi);
	} else {
		pr_warn("vmg %px vmi: (NULL)\n", vmg);
	}
#endif
}
EXPORT_SYMBOL(dump_vmg);

/* 启动期发布后只读：决定新 struct page 是否用固定模式填充以暴露漏初始化。 */
static bool page_init_poisoning __read_mostly = true;

/*
 * setup_vm_debug() - 解析早期 `vm_debug[=...]` 命令行选项。
 * @str 是启动参数解析器提供的可遍历字符串；返回 1 表示已消费。无参数默认开启
 * 所有可控项，`=-` 全关，字符 p 开启 page struct poisoning，未知字符仅报错。
 * 只在单线程早期启动执行，无锁；最后一次赋值发布热路径只读开关。
 */
static int __init setup_vm_debug(char *str)
{
	bool __page_init_poisoning = true;

	/*
	 * Calling vm_debug with no arguments is equivalent to requesting
	 * to enable all debugging options we can control.
	 */
	/* 中文翻译：不带参数调用 vm_debug，等价于开启这里能够控制的全部调试项。 */
	if (*str++ != '=' || !*str)
		goto out;

	/* 显式选项采用“先全关再逐字符开启”，`-` 因而成为关闭全部的快速路径。 */
	__page_init_poisoning = false;
	if (*str == '-')
		goto out;

	/* 每个字符是独立开关；tolower 允许 P/p，未知项不阻止解析后续字符。 */
	while (*str) {
		switch (tolower(*str)) {
		case 'p':
			/* p 唯一改变候选状态，表示初始化 struct page 时写 poison。 */
			__page_init_poisoning = true;
			break;
		default:
			pr_err("vm_debug option '%c' unknown. skipped\n",
			       *str);
		}

		str++;
	}
out:
	/* 仅从默认开启变成关闭时告警，避免正常启用路径产生启动噪声。 */
	if (page_init_poisoning && !__page_init_poisoning)
		pr_warn("Page struct poisoning disabled by kernel command line option 'vm_debug'\n");

	/* 启动期最终发布点；之后 page_init_poison() 只读该值，无需同步。 */
	page_init_poisoning = __page_init_poisoning;

	return 1;
}
__setup("vm_debug", setup_vm_debug);

/*
 * page_init_poison() - 按启动策略填充一段新建 struct page 存储。
 * @page 指向调用者拥有且尚未发布的 memmap，@size 为字节数；无返回值。开启时
 * memset 固定 PAGE_POISON_PATTERN，关闭时为空操作。调用者保证可写和长度有效，
 * 函数不加锁；后续真正初始化字段会覆盖 poison，残留模式用于发现漏初始化。
 */
void page_init_poison(struct page *page, size_t size)
{
	if (page_init_poisoning)
		memset(page, PAGE_POISON_PATTERN, size);
}

/*
 * vma_iter_dump_tree() - 在可用时打印迭代器 maple state 和整棵 VMA 树。
 * @vmi 是调用者锁内借用的非空迭代器；无返回值。DEBUG_VM_MAPLE_TREE 开启时
 * mas_dump/mt_dump 只观察结构，关闭时编译为空桩；调用者不能依赖它提供同步。
 */
void vma_iter_dump_tree(const struct vma_iterator *vmi)
{
#if defined(CONFIG_DEBUG_VM_MAPLE_TREE)
	/* 先显示游标内部状态，再以十六进制节点内容显示其所属 maple tree。 */
	mas_dump(&vmi->mas);
	mt_dump(vmi->mas.tree, mt_dump_hex);
#endif	/* CONFIG_DEBUG_VM_MAPLE_TREE */
}

#endif		/* CONFIG_DEBUG_VM */
