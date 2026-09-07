// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic show_mem() implementation
 *
 * 通用 show_mem() 实现：把全局、节点和 zone 的内存统计快照整理成
 * 面向故障诊断的日志；这里的计数可能在打印期间继续变化，因此各行不是
 * 同一个原子时刻的事务快照，适合判断趋势和定位压力来源，不适合做精确记账。
 *
 * Copyright (C) 2008 Johannes Weiner <hannes@saeurebad.de>
 */

#include <linux/blkdev.h>
#include <linux/cma.h>
#include <linux/cpuset.h>
#include <linux/highmem.h>
#include <linux/hugetlb.h>
/* 核心页、zone 与 swap 统计接口提供本文件的计数来源和单位约定。 */
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/swap.h>
#include <linux/vmstat.h>

/* mm 私有头提供过滤标志、回收状态和 swap cache 诊断入口。 */
#include "internal.h"
#include "swap.h"

/* 系统当前受内存管理器管理的总页数；热插拔路径通过原子接口增减它。 */
atomic_long_t _totalram_pages __read_mostly;
EXPORT_SYMBOL(_totalram_pages);
/* 为水位和低端 zone 回退预留的全局页数，不能算作普通用户可用内存。 */
unsigned long totalreserve_pages __read_mostly;
/* CMA 保留的总页数；只在启用 CMA 的诊断输出中展示。 */
unsigned long totalcma_pages __read_mostly;

/*
 * 业务背景：zone 级日志在 NUMA 机器上需要带节点前缀，非 NUMA 构建则省略。
 * 入参：zone 是当前正在打印的有效内存域，只借用其节点编号。
 * 出参/返回：无返回值；CONFIG_NUMA 启用时向连续 printk 行写入 "Node N "。
 * 注意事项：不持有 zone 引用或锁；调用者负责保证 zone 在遍历期间有效。
 */
static inline void show_node(struct zone *zone)
{
	/* IS_ENABLED 使同一源码同时覆盖 NUMA 和非 NUMA 构建。 */
	if (IS_ENABLED(CONFIG_NUMA))
		printk("Node %d ", zone_to_nid(zone));
}

/*
 * 业务背景：/proc/meminfo 的 MemAvailable 需要估算“不触发 swap/OOM 可分配”页数。
 * 入参：无；读取当前全局 zone/node 统计、水位以及 totalreserve_pages。
 * 出参/返回：返回非负页数估计；它是瞬时启发式结果，不承诺随后分配成功。
 * 注意事项：无全局快照锁，统计可并发漂移；页缓存和可回收内核内存均保留余量。
 */
long si_mem_available(void)
{
	/* available 用有符号类型承接“空闲页减保留页”可能产生的负数。 */
	long available;
	/* pagecache 汇总 active/inactive file LRU 的可回收候选页。 */
	unsigned long pagecache;
	/* wmark_low 汇总所有 zone 的低水位，作为两类缓存的保守保留上限。 */
	unsigned long wmark_low = 0;
	/* reclaimable 汇总可回收 slab 字节折页数和其他可回收内核页。 */
	unsigned long reclaimable;
	/* zone 是只读遍历游标；遍历宏保证访问现存 zone。 */
	struct zone *zone;

	/* 第一阶段：累计所有 zone 的 low watermark。 */
	for_each_zone(zone)
		wmark_low += low_wmark_pages(zone);

	/*
	 * Estimate the amount of memory available for userspace allocations,
	 * without causing swapping or OOM.
	 *
	 * 估算用户空间在不引发换页或 OOM 的前提下还能申请多少内存。
	 * 先从全局空闲页中扣除内核为水位和跨 zone 回退保留的份额。
	 */
	available = global_zone_page_state(NR_FREE_PAGES) - totalreserve_pages;

	/*
	 * Not all the page cache can be freed, otherwise the system will
	 * start swapping or thrashing. Assume at least half of the page
	 * cache, or the low watermark worth of cache, needs to stay.
	 *
	 * 页缓存不能全部计为可用，否则系统可能进入 swap 或抖动；至少保留
	 * “缓存一半”和“low watermark”二者中较小的一份，再把其余计入估值。
	 */
	/* 第二阶段：估计 file LRU 中无需严重抖动即可回收的页。 */
	pagecache = global_node_page_state(NR_ACTIVE_FILE) +
		global_node_page_state(NR_INACTIVE_FILE);
	pagecache -= min(pagecache / 2, wmark_low);
	available += pagecache;

	/*
	 * Part of the reclaimable slab and other kernel memory consists of
	 * items that are in use, and cannot be freed. Cap this estimate at the
	 * low watermark.
	 *
	 * 可回收 slab 和其他内核内存中也有正在使用、暂时不能释放的对象；
	 * 同样按“一半或 low watermark 中较小者”预留，避免高估。
	 */
	/* 第三阶段：把保守折算后的内核可回收内存加入估值。 */
	reclaimable = global_node_page_state_pages(NR_SLAB_RECLAIMABLE_B) +
		global_node_page_state(NR_KERNEL_MISC_RECLAIMABLE);
	reclaimable -= min(reclaimable / 2, wmark_low);
	available += reclaimable;

	/* 保留页可能超过瞬时可回收量，对外接口不得返回负页数。 */
	if (available < 0)
		available = 0;
	return available;
}
EXPORT_SYMBOL_GPL(si_mem_available);

/*
 * 业务背景：sysinfo、/proc/meminfo 及若干子系统需要统一的全机内存摘要。
 * 入参：val 指向由调用者提供、可写的 struct sysinfo。
 * 出参/返回：无返回值；填充 RAM/shared/buffer/highmem 与页大小相关字段。
 * 注意事项：各计数独立读取并可能并发变化；除所列字段外不初始化整个结构体。
 */
void si_meminfo(struct sysinfo *val)
{
	/* 统计单位先保持为页，mem_unit 告诉消费者每单位等于 PAGE_SIZE 字节。 */
	val->totalram = totalram_pages();
	val->sharedram = global_node_page_state(NR_SHMEM);
	val->freeram = global_zone_page_state(NR_FREE_PAGES);
	/* 块设备 address_space 中的页单独作为传统 bufferram 口径。 */
	val->bufferram = nr_blockdev_pages();
	/* highmem 字段让只能直接使用低端内存的消费者扣除不可直映部分。 */
	val->totalhigh = totalhigh_pages();
	val->freehigh = nr_free_highpages();
	val->mem_unit = PAGE_SIZE;
}

EXPORT_SYMBOL(si_meminfo);

#ifdef CONFIG_NUMA
/*
 * 业务背景：DAMON 等 NUMA 消费者要按节点计算内存使用率，而非全机汇总。
 * 入参：val 为输出结构；nid 是在线/有效节点编号，由调用者保证可取 NODE_DATA。
 * 出参/返回：无返回值；写入该节点的 managed/free/shmem/highmem 页数和单位。
 * 注意事项：仅在 CONFIG_NUMA 下存在；totalram 使用 managed 而非 present 口径。
 */
void si_meminfo_node(struct sysinfo *val, int nid)
{
	/* 必须为有符号类型，以契合 zone 枚举循环和边界比较。 */
	int zone_type;		/* needs to be signed */
	/* managed_pages 排除固件保留洞等不能交给页分配器的 present 页。 */
	unsigned long managed_pages = 0;
	/* 以下两个累计量只覆盖该节点内 is_highmem() 的 zone。 */
	unsigned long managed_highpages = 0;
	unsigned long free_highpages = 0;
	/* pgdat 是节点描述符借用指针，本函数不接管其生命周期。 */
	pg_data_t *pgdat = NODE_DATA(nid);

	/* 第一阶段：逐 zone 汇总 managed，并额外拆出 highmem 总量与空闲量。 */
	for (zone_type = 0; zone_type < MAX_NR_ZONES; zone_type++) {
		/* zone 是 pgdat 内嵌数组元素的借用指针，不产生引用转移。 */
		struct zone *zone = &pgdat->node_zones[zone_type];

		managed_pages += zone_managed_pages(zone);
		if (is_highmem(zone)) {
			managed_highpages += zone_managed_pages(zone);
			free_highpages += zone_page_state(zone, NR_FREE_PAGES);
		}
	}

	/* 第二阶段：发布节点摘要；所有字段仍以页为计数单位。 */
	val->totalram = managed_pages;
	val->sharedram = node_page_state(pgdat, NR_SHMEM);
	val->freeram = sum_zone_node_page_state(nid, NR_FREE_PAGES);
	val->totalhigh = managed_highpages;
	val->freehigh = free_highpages;
	val->mem_unit = PAGE_SIZE;
}
#endif

/*
 * Determine whether the node should be displayed or not, depending on whether
 * SHOW_MEM_FILTER_NODES was passed to show_free_areas().
 *
 * 根据 show_free_areas() 是否收到 SHOW_MEM_FILTER_NODES，决定是否隐藏节点。
 */
/*
 * 业务背景：分配失败/OOM 日志可限制在本次分配允许访问的 NUMA 节点集合内。
 * 入参：flags 是展示策略；nid 是候选节点；nodemask 可显式指定允许节点集合。
 * 出参/返回：返回 true 表示跳过该节点，false 表示允许显示。
 * 注意事项：nodemask 为空时借用当前任务 cpuset；诊断路径有意接受无锁近似值。
 */
static bool show_mem_node_skip(unsigned int flags, int nid, nodemask_t *nodemask)
{
	/* 未要求按节点过滤时，任何节点都不应被隐藏。 */
	if (!(flags & SHOW_MEM_FILTER_NODES))
		return false;

	/*
	 * no node mask - aka implicit memory numa policy. Do not bother with
	 * the synchronization - read_mems_allowed_begin - because we do not
	 * have to be precise here.
	 *
	 * 未给节点掩码代表采用隐式 NUMA 策略；此处只是诊断展示，无需通过
	 * read_mems_allowed_begin() 获得精确同步，读取当前 cpuset 快照即可。
	 */
	if (!nodemask)
		nodemask = &cpuset_current_mems_allowed;

	/* 只要 nid 不在最终允许集合中，外层所有节点/zone 输出都统一跳过。 */
	return !node_isset(nid, *nodemask);
}

/*
 * 业务背景：buddy 各 order 的空闲块还需展示它们分布在哪些迁移类型链表。
 * 入参：type 是 MIGRATE_* 位图，由持有 zone->lock 的采样阶段生成。
 * 出参/返回：无返回值；以紧凑字母串打印已出现的迁移类型。
 * 注意事项：只读位图副本，不访问链表；配置未启用的 CMA/ISOLATE 字母不存在。
 */
static void show_migration_types(unsigned char type)
{
	/* 下标与 enum migratetype 对齐，字母是日志 ABI，而非可回译的完整名称。 */
	static const char types[MIGRATE_TYPES] = {
		[MIGRATE_UNMOVABLE]	= 'U',
		[MIGRATE_MOVABLE]	= 'M',
		[MIGRATE_RECLAIMABLE]	= 'E',
		[MIGRATE_HIGHATOMIC]	= 'H',
#ifdef CONFIG_CMA
		/* C 表示只允许 CMA 分配器使用的可迁移保留区。 */
		[MIGRATE_CMA]		= 'C',
#endif
#ifdef CONFIG_MEMORY_ISOLATION
		/* I 表示已从普通分配路径隔离、正供热插拔/CMA 等操作使用。 */
		[MIGRATE_ISOLATE]	= 'I',
#endif
	};
	/* 每种类型至多一个字符，末尾额外保留 NUL。 */
	char tmp[MIGRATE_TYPES + 1];
	/* p 始终指向下一写入位置，最终用于补字符串终止符。 */
	char *p = tmp;
	/* i 遍历所有编译进当前内核的迁移类型编号。 */
	int i;

	/* 按枚举顺序压缩设置的位，因此输出稳定且不含分隔符。 */
	for (i = 0; i < MIGRATE_TYPES; i++) {
		if (type & (1 << i))
			*p++ = types[i];
	}

	/* 即使 type 为零也形成合法空串，随后打印成空括号。 */
	*p = '\0';
	printk(KERN_CONT "(%s) ", tmp);
}

/*
 * 业务背景：节点摘要应忽略在本次分配最高 zone 限制内完全没有 managed 页的节点。
 * 入参：pgdat 是节点描述符；max_zone_idx 是本次允许展示的最高 zone 下标。
 * 出参/返回：范围内任一 zone 有 managed 页则返回 true，否则返回 false。
 * 注意事项：只做无锁诊断采样；热插拔并发变化可能令结果成为近似快照。
 */
static bool node_has_managed_zones(pg_data_t *pgdat, int max_zone_idx)
{
	/* zone_idx 是从最低 zone 到调用者上界的扫描游标。 */
	int zone_idx;

	/* managed 页比 present 页更能说明 buddy 是否实际管理该 zone。 */
	for (zone_idx = 0; zone_idx <= max_zone_idx; zone_idx++)
		if (zone_managed_pages(pgdat->node_zones + zone_idx))
			return true;
	return false;
}

/*
 * Show free area list (used inside shift_scroll-lock stuff)
 * We also calculate the percentage fragmentation. We do this by counting the
 * memory on each free list with the exception of the first item on the list.
 *
 * 展示空闲区链表（历史上用于 Shift+ScrollLock 诊断）；同时通过各 order
 * 空闲链表的块数和迁移类型分布呈现外部碎片线索。当前实现打印绝对分布，
 * 不对这些并发变化的统计提供事务一致性。
 *
 * Bits in @filter:
 * SHOW_MEM_FILTER_NODES: suppress nodes that are not allowed by current's
 *   cpuset.
 *
 * @filter 位含义：SHOW_MEM_FILTER_NODES 隐藏当前任务 cpuset 不允许的节点。
 */
/*
 * 业务背景：分配失败和 OOM 路径需要从全局 LRU 一直下钻到 buddy order 的诊断图。
 * 入参：filter 控制节点过滤；nodemask 可覆盖 cpuset；max_zone_idx 限制分配可达 zone。
 * 出参/返回：无返回值；向内核日志打印全局、节点、zone、buddy、hugetlb 与 swap 信息。
 * 注意事项：绝大多数计数无锁读取；仅复制 free_area 时短持 zone->lock 且关本地 IRQ。
 */
static void show_free_areas(unsigned int filter, nodemask_t *nodemask, int max_zone_idx)
{
	/* free_pcp 先汇总过滤范围内所有 zone，之后复用于单个 zone 的统计。 */
	unsigned long free_pcp = 0;
	/* cpu/nid 是在线 CPU、节点的遍历游标。 */
	int cpu, nid;
	/* zone/pgdat 均为遍历期间借用的内存拓扑对象。 */
	struct zone *zone;
	pg_data_t *pgdat;

	/* 第一阶段：累计允许 zone 的 per-CPU page-list 页数。 */
	for_each_populated_zone(zone) {
		if (zone_idx(zone) > max_zone_idx)
			continue;
		if (show_mem_node_skip(filter, zone_to_nid(zone), nodemask))
			continue;

		/* CPU 热插拔与计数更新可并发发生，结果仅用于诊断。 */
		for_each_online_cpu(cpu)
			free_pcp += per_cpu_ptr(zone->per_cpu_pageset, cpu)->count;
	}

	/* 第二阶段：输出全机 LRU、回写、slab、页表和空闲页摘要。 */
	printk("active_anon:%lu inactive_anon:%lu isolated_anon:%lu\n"
		/* 先列匿名/文件 LRU 与隔离队列，观察回收扫描中的在途页面。 */
		" active_file:%lu inactive_file:%lu isolated_file:%lu\n"
		" unevictable:%lu dirty:%lu writeback:%lu\n"
		/* 再列不可回收、脏页/回写和可回收性不同的 slab。 */
		" slab_reclaimable:%lu slab_unreclaimable:%lu\n"
		" mapped:%lu shmem:%lu pagetables:%lu\n"
		/* 最后列页表、其他可回收内核页以及三种空闲页口径。 */
		" sec_pagetables:%lu bounce:%lu\n"
		" kernel_misc_reclaimable:%lu\n"
		" free:%lu free_pcp:%lu free_cma:%lu\n",
		/* 匿名与文件 LRU 统计均来自全局 node vmstat 汇总。 */
		global_node_page_state(NR_ACTIVE_ANON),
		global_node_page_state(NR_INACTIVE_ANON),
		global_node_page_state(NR_ISOLATED_ANON),
		global_node_page_state(NR_ACTIVE_FILE),
		global_node_page_state(NR_INACTIVE_FILE),
		global_node_page_state(NR_ISOLATED_FILE),
		/* 脏页、回写和 slab 值反映当前回收前可释放/需等待的规模。 */
		global_node_page_state(NR_UNEVICTABLE),
		global_node_page_state(NR_FILE_DIRTY),
		global_node_page_state(NR_WRITEBACK),
		global_node_page_state_pages(NR_SLAB_RECLAIMABLE_B),
		global_node_page_state_pages(NR_SLAB_UNRECLAIMABLE_B),
		/* 映射、共享内存与两类页表帮助解释 file pages 的实际占用。 */
		global_node_page_state(NR_FILE_MAPPED),
		global_node_page_state(NR_SHMEM),
		global_node_page_state(NR_PAGETABLE),
		global_node_page_state(NR_SECONDARY_PAGETABLE),
		/* bounce 当前无独立统计，保留 0 以维持历史日志字段。 */
		0UL,
		global_node_page_state(NR_KERNEL_MISC_RECLAIMABLE),
		/* free 是 buddy 总空闲；free_pcp 是其分散在 PCP 中的诊断子集。 */
		global_zone_page_state(NR_FREE_PAGES),
		free_pcp,
		global_zone_page_state(NR_FREE_CMA_PAGES));

	/* 第三阶段：逐在线节点输出 node_page_state 及回收是否已判定无望。 */
	for_each_online_pgdat(pgdat) {
		if (show_mem_node_skip(filter, pgdat->node_id, nodemask))
			continue;
		if (!node_has_managed_zones(pgdat, max_zone_idx))
			continue;

		/* 所有 K(...) 项把页数换算成 KiB；*_KB 原生字段不再重复换算。 */
		printk("Node %d"
			/* 节点 LRU 工作集：匿名、文件、不可驱逐以及隔离中的页面。 */
			" active_anon:%lukB"
			" inactive_anon:%lukB"
			" active_file:%lukB"
			" inactive_file:%lukB"
			" unevictable:%lukB"
			" isolated(anon):%lukB"
			" isolated(file):%lukB"
			/* 映射、脏页、回写和 shmem 揭示 file LRU 的可回收约束。 */
			" mapped:%lukB"
			" dirty:%lukB"
			" writeback:%lukB"
			" shmem:%lukB"
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
			/* THP 构建追加 shmem THP/PMD 映射及匿名 THP 三种口径。 */
			" shmem_thp:%lukB"
			" shmem_pmdmapped:%lukB"
			" anon_thp:%lukB"
#endif
			/* 内核栈和可选 shadow call stack 本身已按 KiB 维护。 */
			" kernel_stack:%lukB"
#ifdef CONFIG_SHADOW_CALL_STACK
			" shadow_call_stack:%lukB"
#endif
			/* 页表、回收失败状态、balloon 与 GPU 统计补足内核占用来源。 */
			" pagetables:%lukB"
			" sec_pagetables:%lukB"
			" all_unreclaimable? %s"
			" Balloon:%lukB"
			" gpu_active:%lukB"
			" gpu_reclaim:%lukB"
			"\n",
			/* 参数顺序必须与上面条件编译后的格式字段逐项对应。 */
			pgdat->node_id,
			K(node_page_state(pgdat, NR_ACTIVE_ANON)),
			K(node_page_state(pgdat, NR_INACTIVE_ANON)),
			K(node_page_state(pgdat, NR_ACTIVE_FILE)),
			K(node_page_state(pgdat, NR_INACTIVE_FILE)),
			/* 不可驱逐与隔离页不应直接被当作立即可回收容量。 */
			K(node_page_state(pgdat, NR_UNEVICTABLE)),
			K(node_page_state(pgdat, NR_ISOLATED_ANON)),
			K(node_page_state(pgdat, NR_ISOLATED_FILE)),
			K(node_page_state(pgdat, NR_FILE_MAPPED)),
			K(node_page_state(pgdat, NR_FILE_DIRTY)),
			K(node_page_state(pgdat, NR_WRITEBACK)),
			K(node_page_state(pgdat, NR_SHMEM)),
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
			/* 配置守卫确保 THP 三个参数与三个格式占位符同时出现。 */
			K(node_page_state(pgdat, NR_SHMEM_THPS)),
			K(node_page_state(pgdat, NR_SHMEM_PMDMAPPED)),
			K(node_page_state(pgdat, NR_ANON_THPS)),
#endif
			/* NR_KERNEL_STACK_KB 和 NR_KERNEL_SCS_KB 不经过 K()。 */
			node_page_state(pgdat, NR_KERNEL_STACK_KB),
#ifdef CONFIG_SHADOW_CALL_STACK
			node_page_state(pgdat, NR_KERNEL_SCS_KB),
#endif
			/* hopeless 表示 kswapd 失败次数达到阈值，并非永久不可回收判决。 */
			K(node_page_state(pgdat, NR_PAGETABLE)),
			K(node_page_state(pgdat, NR_SECONDARY_PAGETABLE)),
			str_yes_no(kswapd_test_hopeless(pgdat)),
			K(node_page_state(pgdat, NR_BALLOON_PAGES)),
			K(node_page_state(pgdat, NR_GPU_ACTIVE)),
			K(node_page_state(pgdat, NR_GPU_RECLAIM)));
	}

	/* 第四阶段：逐 zone 展示水位、保留量、工作集和 PCP/CMA 状态。 */
	for_each_populated_zone(zone) {
		/* i 用于打印该 zone 面向各高层 zone 的 lowmem_reserve 数组。 */
		int i;

		if (zone_idx(zone) > max_zone_idx)
			continue;
		if (show_mem_node_skip(filter, zone_to_nid(zone), nodemask))
			continue;

		/* 重新计算当前 zone 的所有 CPU PCP 总量，覆盖第一阶段的全局汇总值。 */
		free_pcp = 0;
		for_each_online_cpu(cpu)
			free_pcp += per_cpu_ptr(zone->per_cpu_pageset, cpu)->count;

		/* show_node 与 KERN_CONT 共同组成同一条 zone 日志行。 */
		show_node(zone);
		printk(KERN_CONT
			"%s"
			/* zone 身份、空闲量、水位 boost 与 min/low/high 决定分配快慢路径。 */
			" free:%lukB"
			" boost:%lukB"
			" min:%lukB"
			" low:%lukB"
			" high:%lukB"
			/* highatomic 保留和实际空闲量用于解释高阶原子分配余量。 */
			" reserved_highatomic:%luKB"
			" free_highatomic:%luKB"
			/* zone-local LRU 与 writepending 描述回收候选和 I/O 阻塞。 */
			" active_anon:%lukB"
			" inactive_anon:%lukB"
			" active_file:%lukB"
			" inactive_file:%lukB"
			" unevictable:%lukB"
			" writepending:%lukB"
			/* zsmalloc 可选统计之后，比较 present 与 managed 可见保留洞。 */
			" zspages:%lukB"
			" present:%lukB"
			" managed:%lukB"
			" mlocked:%lukB"
			/* bounce 保留历史字段；PCP 拆分总量/当前 CPU，最后报告 CMA。 */
			" bounce:%lukB"
			" free_pcp:%lukB"
			" local_pcp:%ukB"
			" free_cma:%lukB"
			"\n",
			/* 参数严格对应上述字段；水位和原子保留均由 zone 直接读取。 */
			zone->name,
			K(zone_page_state(zone, NR_FREE_PAGES)),
			K(zone->watermark_boost),
			K(min_wmark_pages(zone)),
			K(low_wmark_pages(zone)),
			K(high_wmark_pages(zone)),
			K(zone->nr_reserved_highatomic),
			K(zone->nr_free_highatomic),
			/* NR_ZONE_* 避免把其他节点或 zone 的 LRU 状态混入此行。 */
			K(zone_page_state(zone, NR_ZONE_ACTIVE_ANON)),
			K(zone_page_state(zone, NR_ZONE_INACTIVE_ANON)),
			K(zone_page_state(zone, NR_ZONE_ACTIVE_FILE)),
			K(zone_page_state(zone, NR_ZONE_INACTIVE_FILE)),
			K(zone_page_state(zone, NR_ZONE_UNEVICTABLE)),
			K(zone_page_state(zone, NR_ZONE_WRITE_PENDING)),
#if IS_ENABLED(CONFIG_ZSMALLOC)
			/* 无 zsmalloc 时仍传 0，占位并保持日志列稳定。 */
			K(zone_page_state(zone, NR_ZSPAGES)),
#else
			0UL,
#endif
			/* present/managed 揭示不可管理页，mlocked 揭示不能回收的用户页。 */
			K(zone->present_pages),
			K(zone_managed_pages(zone)),
			K(zone_page_state(zone, NR_MLOCK)),
			/* bounce 无统计传 0；this_cpu_read 只代表执行诊断的当前 CPU。 */
			0UL,
			K(free_pcp),
			K(this_cpu_read(zone->per_cpu_pageset->count)),
			K(zone_page_state(zone, NR_FREE_CMA_PAGES)));
		/* lowmem_reserve[i] 防止高 zone 分配耗尽低 zone，单位为页。 */
		printk("lowmem_reserve[]:");
		for (i = 0; i < MAX_NR_ZONES; i++)
			printk(KERN_CONT " %ld", zone->lowmem_reserve[i]);
		printk(KERN_CONT "\n");
	}

	/* 第五阶段：在锁内复制 buddy 各 order 的块数和迁移类型，锁外打印。 */
	for_each_populated_zone(zone) {
		/* order 是 buddy 阶数；每个块包含 2^order 页。 */
		unsigned int order;
		/* nr/types 是锁内快照，flags 保存本地 IRQ 状态，total 累计页数。 */
		unsigned long nr[NR_PAGE_ORDERS], flags, total = 0;
		unsigned char types[NR_PAGE_ORDERS];

		if (zone_idx(zone) > max_zone_idx)
			continue;
		if (show_mem_node_skip(filter, zone_to_nid(zone), nodemask))
			continue;
		show_node(zone);
		printk(KERN_CONT "%s: ", zone->name);

		/* free_area 链表与 nr_free 由 zone->lock 保护；锁内只复制，避免慢速打印。 */
		spin_lock_irqsave(&zone->lock, flags);
		for (order = 0; order < NR_PAGE_ORDERS; order++) {
			/* area 是当前阶空闲区数组元素的借用指针。 */
			struct free_area *area = &zone->free_area[order];
			/* type 扫描该阶的每条 MIGRATE_* freelist。 */
			int type;

			nr[order] = area->nr_free;
			total += nr[order] << order;

			types[order] = 0;
			for (type = 0; type < MIGRATE_TYPES; type++) {
				if (!free_area_empty(area, type))
					types[order] |= 1 << type;
			}
		}
		spin_unlock_irqrestore(&zone->lock, flags);
		/* 打印阶段只使用局部副本，不延长 zone 锁/IRQ-off 临界区。 */
		for (order = 0; order < NR_PAGE_ORDERS; order++) {
			printk(KERN_CONT "%lu*%lukB ",
			       nr[order], K(1UL) << order);
			if (nr[order])
				show_migration_types(types[order]);
		}
		printk(KERN_CONT "= %lukB\n", K(total));
	}

	/* 第六阶段：沿用相同节点过滤规则，追加每节点 hugetlb 池信息。 */
	for_each_online_node(nid) {
		if (show_mem_node_skip(filter, nid, nodemask))
			continue;
		hugetlb_show_meminfo_node(nid);
	}

	/* 最后追加 file page 总量及 swap cache/容量，完成诊断横截面。 */
	printk("%ld total pagecache pages\n", global_node_page_state(NR_FILE_PAGES));

	show_swap_cache_info();
}

/*
 * 业务背景：OOM、分配失败和显式 show_mem() 最终都由此组织完整内存诊断。
 * 入参：filter/nodemask 控制 NUMA 可见范围；max_zone_idx 限定本次分配可达 zone。
 * 出参/返回：无返回值；先打印细分空闲区，再汇总 RAM/reserved/CMA/hwpoison/热点。
 * 注意事项：诊断必须避免阻塞故障路径；分配画像用 trylock，竞争时宁可跳过。
 */
void __show_mem(unsigned int filter, nodemask_t *nodemask, int max_zone_idx)
{
	/* total/reserved/highmem 均以页为单位，在遍历中按 present 口径累计。 */
	unsigned long total = 0, reserved = 0, highmem = 0;
	/* zone 是 populated zone 遍历游标，不获得额外引用。 */
	struct zone *zone;

	/* 第一阶段：输出可回收状态、节点/zone 水位和 buddy 碎片分布。 */
	printk("Mem-Info:\n");
	show_free_areas(filter, nodemask, max_zone_idx);

	/* 第二阶段：汇总所有 populated zone；这里有意不套用诊断过滤上界。 */
	for_each_populated_zone(zone) {

		/* present 包括 zone 内存在但未交给 buddy 管理的页。 */
		total += zone->present_pages;
		/* present-managed 代表固件/元数据等永久或当前不可管理部分。 */
		reserved += zone->present_pages - zone_managed_pages(zone);

		if (is_highmem(zone))
			highmem += zone->present_pages;
	}

	/* 第三阶段：按页打印全机物理摘要，并按配置追加专用保留量。 */
	printk("%lu pages RAM\n", total);
	printk("%lu pages HighMem/MovableOnly\n", highmem);
	printk("%lu pages reserved\n", reserved);
#ifdef CONFIG_CMA
	printk("%lu pages cma reserved\n", totalcma_pages);
#endif
#ifdef CONFIG_MEMORY_FAILURE
	printk("%lu pages hwpoisoned\n", atomic_long_read(&num_poisoned_pages));
#endif
#ifdef CONFIG_MEM_ALLOC_PROFILING
	/* 多个并发 OOM/分配警告只允许一个执行昂贵的 top-user 排序与打印。 */
	static DEFINE_SPINLOCK(mem_alloc_profiling_spinlock);

	/* trylock 失败直接省略画像，保证诊断路径不会彼此等待。 */
	if (spin_trylock(&mem_alloc_profiling_spinlock)) {
		/* tags 保存按当前占用量选出的前十个 allocation codetag。 */
		struct codetag_bytes tags[10];
		/* i 为打印游标，nr 是实际返回的有效热点数。 */
		size_t i, nr;

		/* false 要求模块列表锁只做 trylock；竞争或无统计类型时返回 0。 */
		nr = alloc_tag_top_users(tags, ARRAY_SIZE(tags), false);
		if (nr) {
			pr_notice("Memory allocations (profiling is currently turned %s):\n",
				mem_alloc_profiling_enabled() ? "on" : "off");
			for (i = 0; i < nr; i++) {
				struct codetag *ct = tags[i].ct;
				/* tag 承载计数器，ct 承载源码位置和模块/函数名。 */
				struct alloc_tag *tag = ct_to_alloc_tag(ct);
				/* 读取计数副本，避免格式化期间反复观察变化。 */
				struct alloc_tag_counters counter = alloc_tag_read(tag);
				/* bytes 是足以容纳人类可读容量短串的栈缓冲区。 */
				char bytes[10];

				string_get_size(counter.bytes, 1, STRING_UNITS_2, bytes, sizeof(bytes));

				/* 与 alloc_tag_to_text() 等价，但省去中间缓冲区。 */
				/* Same as alloc_tag_to_text() but w/o intermediate buffer */
				if (ct->modname)
					pr_notice("%12s %8llu %s:%u [%s] func:%s\n",
						  bytes, counter.calls, ct->filename,
						  ct->lineno, ct->modname, ct->function);
				else
					pr_notice("%12s %8llu %s:%u func:%s\n",
						  bytes, counter.calls, ct->filename,
						  ct->lineno, ct->function);
			}
		}
		/* 计数对象由 profiling 子系统拥有，本函数只释放串行化打印的锁。 */
		spin_unlock(&mem_alloc_profiling_spinlock);
	}
#endif
}
