// SPDX-License-Identifier: GPL-2.0-only
/*
 * Linux 核心内存管理启动流程学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5.4（2026-07-27）。
 *
 * 本文件把架构通过 memblock、NUMA 节点和 PFN 区间交付的物理内存描述，
 * 转换成通用 MM 可运行的核心对象：
 *
 *   memblock/固件内存图
 *     -> 计算各 zone 与 ZONE_MOVABLE 边界
 *     -> 初始化 pg_data_t、zone、free_area 和每页 struct page
 *     -> 可选延迟/并行初始化高 PFN memmap
 *     -> memblock_free_all() 把可用页提交给 buddy
 *     -> slab、vmalloc、页表缓存和调试/加固设施接管
 *
 * 主要入口：
 *
 *   start_kernel()
 *     -> mm_core_init_early()
 *          巨页/CMA 早期预留 -> free_area_init()
 *     -> mm_core_init()
 *          zonelist/调试元数据 -> buddy 接管 -> slab/vmalloc
 *     -> page_alloc_init_late()
 *          完成 deferred struct page、页扩展和启动后收尾
 *
 * 本文件负责内存模型的“构造与发布”，不实现运行期伙伴分配/释放
 * 算法、reclaim、缺页处理或具体架构页表建立；这些分别位于
 * page_alloc.c、vmscan.c、memory.c 和 arch/<architecture>/mm。
 *
 * 核心对象与生命周期：
 *
 *   pg_data_t 和 zone 通常由架构/静态节点描述提供，本文件填充其 PFN 范围、
 *   水位相关基础状态、free_area 链表和 per-CPU 统计指针；struct page 数组
 *   由 FLATMEM/SPARSEMEM 模型提供，本文件逐页建立 flags、引用计数、链表
 *   与 pageblock 迁移类型。初始化成功后这些对象存活到系统结束或由内存
 *   hotplug 协议局部重建，启动期 __init 数据则在 initmem 回收时失效。
 *
 * 并发模型：
 *
 *   大部分路径运行在 SMP 启动前的单线程早期阶段，依靠调用顺序而
 *   不是普通运行期锁。少数内存 hotplug、deferred-init worker 和
 *   sysfs/initcall 路径具有显式锁、原子计数、completion 或
 *   notifier 协议；这些位置必须单独说明竞态双方。启动期“先构造
 *   元数据、再把页交给 buddy”是最重要的发布边界：页一旦进入
 *   freelist，就可能被其他子系统立即分配。
 *
 * 方案权衡：
 *
 *   统一初始化代码让不同架构共享 zone/buddy 不变量；deferred init 和
 *   padata 并行化缩短大内存机器启动时间，但增加“哪些 struct page 已可
 *   访问”的阶段状态。ZONE_MOVABLE 改善迁移、热移除和大块连续分配能力，
 *   代价是必须在多个节点间平衡不可迁移 kernelcore，并处理镜像内存、
 *   hotplug 区域和低地址受限 zone 等例外。
 */
/*
 * mm_init.c - Memory initialisation verification and debugging
 *
 * Copyright 2008 IBM Corporation, 2008
 * Author Mel Gorman <mel@csn.ul.ie>
 *
 */
/*
 * 本文件最初聚焦内存初始化验证与调试，当前还承载通用 zone、memmap、
 * buddy 发布及后期 MM 核心初始化；原作者和版权信息如上。
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/export.h>
#include <linux/memory.h>
#include <linux/notifier.h>
#include <linux/sched.h>
#include <linux/mman.h>
#include <linux/memblock.h>
#include <linux/page-isolation.h>
#include <linux/padata.h>
#include <linux/nmi.h>
#include <linux/buffer_head.h>
#include <linux/kmemleak.h>
#include <linux/kfence.h>
#include <linux/page_ext.h>
#include <linux/pti.h>
#include <linux/pgtable.h>
#include <linux/stackdepot.h>
#include <linux/swap.h>
#include <linux/cma.h>
#include <linux/crash_dump.h>
#include <linux/execmem.h>
#include <linux/vmstat.h>
#include <linux/kexec_handover.h>
#include <linux/hugetlb.h>
#include "internal.h"
#include "slab.h"
#include "shuffle.h"

#include <asm/setup.h>

#ifndef CONFIG_NUMA
/*
 * 非 NUMA FLATMEM 全局地址模型：
 *
 * max_mapnr 是 mem_map 可表示的最大 PFN 上界，mem_map 是全局 struct page
 * 数组基址。架构/内存模型在启动期写入，pfn_to_page/page_to_pfn 等读者在
 * 运行期借用；存储由内存模型拥有，导出符号不转移 ownership。NUMA 配置
 * 由节点 pgdat 提供等价信息，因此不定义这两个单一全局。
 */
unsigned long max_mapnr;
EXPORT_SYMBOL(max_mapnr);

struct page *mem_map;
EXPORT_SYMBOL(mem_map);
#endif

/*
 * high_memory defines the upper bound on direct map memory, then end
 * of ZONE_NORMAL.
 */
/*
 * high_memory 定义线性直接映射内存的上界，也就是 ZONE_NORMAL 的末端。
 * set_high_memory() 在 zone 边界确定后一次写入，vmalloc/ioremap 等代码
 * 启动后只读。它是地址界限而非已分配对象，不承载引用或释放责任。
 */
void *high_memory;
EXPORT_SYMBOL(high_memory);

/*
 * zero_page_pfn 缓存架构 ZERO_PAGE(0) 对应的 PFN。init_zero_page_pfn()
 * 在零页建立后写入，__ro_after_init 使启动完成后只读；缺页和映射路径可
 * 直接取 PFN，无需反复做 page_to_pfn。导出只共享数值，不转移零页所有权。
 */
unsigned long zero_page_pfn __ro_after_init;
EXPORT_SYMBOL(zero_page_pfn);

#ifndef __HAVE_COLOR_ZERO_PAGE
/*
 * 无 cache-color 专用零页的架构使用这一页对齐、永久全零的静态存储。
 * empty_zero_page 是实际字节页；__zero_page 是其 struct page 借用指针，
 * 由弱 arch_setup_zero_pages() 在启动期绑定并在 init 后只读。两者存活
 * 到系统结束，不通过伙伴分配器释放。
 */
const uint8_t empty_zero_page[PAGE_SIZE] __aligned(PAGE_SIZE);
EXPORT_SYMBOL(empty_zero_page);

struct page *__zero_page __ro_after_init;
EXPORT_SYMBOL(__zero_page);
#endif /* __HAVE_COLOR_ZERO_PAGE */
/* 有 color zero page 的架构自行提供零页选择与 ZERO_PAGE() 语义。 */

#ifdef CONFIG_DEBUG_MEMORY_INIT
/*
 * mminit_loglevel 控制启动期内存布局诊断密度，值对应 MMINIT_* 级别。
 * early_param 在普通分配器可用前写入；验证函数和 mminit_dprintk() 只读。
 * __meminitdata 允许不支持内存 hotplug 的构建在初始化后回收该存储。
 */
int __meminitdata mminit_loglevel;

/* The zonelists are simply reported, validation is manual. */
/*
 * zonelist 只被打印出来，代码不会自动判断顺序是否符合平台预期；
 * 开发者需根据 NUMA 距离和 zone fallback 规则人工检查输出。
 */
/*
 * mminit_verify_zonelist() - 输出所有 online node 的分配回退序列。
 *
 * 调用关系：build_all_zonelists() 完成后由 page allocator 调试路径调用。
 * 入参：无。返回：无直接返回值。函数只在
 * CONFIG_DEBUG_MEMORY_INIT=y 的早期启动上下文运行，不持锁、不会分配长期
 * 对象；此时 zonelist 已构造且尚无运行期热插拔并发修改。
 *
 * mminit_loglevel 低于 MMINIT_VERIFY 时无副作用快速返回。启用后按 node、
 * zonelist 类型和起始 zone 遍历，只打印 populated zone 的 fallback
 * 顺序。打印不会修改 pgdat/zone，也没有普通失败路径。
 */
void __init mminit_verify_zonelist(void)
{
	/*
	 * nid 是当前 online node；内层 i 把
	 * [zonelist 类型][起始 zone] 二维组合线性化。
	 */
	int nid;

	/*
	 * 日志级别是整个验证功能的启动期开关，避免默认启动产生
	 * 大量输出。
	 */
	if (mminit_loglevel < MMINIT_VERIFY)
		return;

	for_each_online_node(nid) {
		/*
		 * pgdat/zone/zonelist/z 均是全局节点拓扑中的借用指针；
		 * i/listid/zoneid 只描述当前组合，不跨越循环保存。
		 */
		pg_data_t *pgdat = NODE_DATA(nid);
		struct zone *zone;
		struct zoneref *z;
		struct zonelist *zonelist;
		int i, listid, zoneid;

		for (i = 0; i < MAX_ZONELISTS * MAX_NR_ZONES; i++) {

			/* Identify the zone and nodelist */
			/*
			 * 识别当前起始 zone 和 zonelist。取模得到 zone
			 * 类型，整除得到 general/thisnode 列表编号，随后
			 * 借用 pgdat 内数组。
			 */
			zoneid = i % MAX_NR_ZONES;
			listid = i / MAX_NR_ZONES;
			zonelist = &pgdat->node_zonelists[listid];
			zone = &pgdat->node_zones[zoneid];
			if (!populated_zone(zone))
				continue;

			/* Print information about the zonelist */
			/*
			 * 先打印被验证的 node、列表策略和起始 zone，pr_cont()
			 * 后续在同一日志记录中追加实际 fallback 项。
			 */
			printk(KERN_DEBUG "mminit::zonelist %s %d:%s = ",
				listid > 0 ? "thisnode" : "general", nid,
				zone->name);

			/* Iterate the zonelist */
			/*
			 * 从 zoneid 允许的最高 zone class 开始遍历该列表，
			 * 输出每个候选 zone 的 nid/name；遍历宏只读
			 * zoneref，不取得引用。
			 */
			for_each_zone_zonelist(zone, z, zonelist, zoneid)
				pr_cont("%d:%s ", zone_to_nid(zone), zone->name);
			pr_cont("\n");
		}
	}
}

/*
 * mminit_verify_pageflags_layout() - 验证 struct page::flags 位域布局。
 *
 * 调用关系：free_area_init() 在 zone/PFN 基础布局建立后调用。入参：无；
 * 返回：无直接返回值。函数运行于单线程 __init 阶段，不持锁、不睡眠，
 * 只读取编译期 WIDTH/SHIFT/MASK 常量并输出布局。
 *
 * 它从机器字高位向下复算 section/node/zone 字段位置，并用 BUG_ON 验证
 * 与生成宏一致；随后比较 OR 与加法组合结果，证明各 mask 不重叠。任何
 * 失败都表示编译配置生成了会破坏 page flags 的内部 ABI，无法回滚，
 * 因此立即终止，而不是让运行期静默错读 page 所属位置。
 */
void __init mminit_verify_pageflags_layout(void)
{
	/*
	 * shift 是从 BITS_PER_LONG 顶端向下移动的游标，width 是扣除非普通
	 * pageflag 元数据后的空间；or_mask/add_mask 用两种组合方式检测重叠。
	 */
	int shift, width;
	unsigned long or_mask, add_mask;

	shift = BITS_PER_LONG;
	width = shift - NR_NON_PAGEFLAG_BITS;
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_widths",
		"Section %d Node %d Zone %d Lastcpupid %d Kasantag %d Gen %d Tier %d Flags %d\n",
		SECTIONS_WIDTH,
		NODES_WIDTH,
		ZONES_WIDTH,
		LAST_CPUPID_WIDTH,
		KASAN_TAG_WIDTH,
		LRU_GEN_WIDTH,
		LRU_REFS_WIDTH,
		NR_PAGEFLAGS);
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_shifts",
		"Section %d Node %d Zone %d Lastcpupid %d Kasantag %d\n",
		SECTIONS_SHIFT,
		NODES_SHIFT,
		ZONES_SHIFT,
		LAST_CPUPID_SHIFT,
		KASAN_TAG_WIDTH);
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_pgshifts",
		"Section %lu Node %lu Zone %lu Lastcpupid %lu Kasantag %lu\n",
		(unsigned long)SECTIONS_PGSHIFT,
		(unsigned long)NODES_PGSHIFT,
		(unsigned long)ZONES_PGSHIFT,
		(unsigned long)LAST_CPUPID_PGSHIFT,
		(unsigned long)KASAN_TAG_PGSHIFT);
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_nodezoneid",
		"Node/Zone ID: %lu -> %lu\n",
		(unsigned long)(ZONEID_PGOFF + ZONEID_SHIFT),
		(unsigned long)ZONEID_PGOFF);
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_usage",
		"location: %d -> %d layout %d -> %d unused %d -> %d page-flags\n",
		shift, width, width, NR_PAGEFLAGS, NR_PAGEFLAGS, 0);
#ifdef NODE_NOT_IN_PAGE_FLAGS
	/* node id 另存时明确记录该配置，不能按 NODES_* 位域解读 flags。 */
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_nodeflags",
		"Node not in page flags");
#endif
#ifdef LAST_CPUPID_NOT_IN_PAGE_FLAGS
	/* last_cpupid 另存时同样输出，便于解释 flags 中为何没有对应宽度。 */
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_nodeflags",
		"Last cpupid not in page flags");
#endif

	/*
	 * 按 section -> node -> zone 的实际高位布局逐段扣减。宽度为 0
	 * 表示该维度未编码进 flags，不应改变游标。
	 */
	if (SECTIONS_WIDTH) {
		shift -= SECTIONS_WIDTH;
		BUG_ON(shift != SECTIONS_PGSHIFT);
	}
	if (NODES_WIDTH) {
		shift -= NODES_WIDTH;
		BUG_ON(shift != NODES_PGSHIFT);
	}
	if (ZONES_WIDTH) {
		shift -= ZONES_WIDTH;
		BUG_ON(shift != ZONES_PGSHIFT);
	}

	/* Check for bitmask overlaps */
	/*
	 * 检查位掩码重叠：互不相交时按位 OR 与算术相加结果相同；若两个
	 * 字段占用同一 bit，加法产生进位而与 OR 不同，BUG_ON 会捕获。
	 */
	or_mask = (ZONES_MASK << ZONES_PGSHIFT) |
			(NODES_MASK << NODES_PGSHIFT) |
			(SECTIONS_MASK << SECTIONS_PGSHIFT);
	add_mask = (ZONES_MASK << ZONES_PGSHIFT) +
			(NODES_MASK << NODES_PGSHIFT) +
			(SECTIONS_MASK << SECTIONS_PGSHIFT);
	BUG_ON(or_mask != add_mask);
}

/*
 * set_mminit_loglevel() - 解析 mminit_loglevel= 早期命令行参数。
 *
 * @str 是 early_param 框架借用的可写数字字符串，不转移 ownership；
 * get_option() 解析整数并写全局 mminit_loglevel，同时推进局部指针。
 * 返回 0 表示参数已处理。函数处于早期单线程 __init 上下文，不分配、
 * 不睡眠；格式异常沿 get_option 的默认结果体现，不另设错误回滚。
 */
static __init int set_mminit_loglevel(char *str)
{
	get_option(&str, &mminit_loglevel);
	return 0;
}
early_param("mminit_loglevel", set_mminit_loglevel);
#endif /* CONFIG_DEBUG_MEMORY_INIT */
/* 调试配置关闭时，验证入口由 internal.h 的空 stub 消除。 */

/*
 * mm_kobj 是 /sys/kernel/mm 的长期根 kobject 借用指针。mm_sysfs_init()
 * 创建并持有初始引用，KSM、THP、DAMON、page_idle、hugetlb 等子系统在其
 * 下添加属性/子目录。成功后存活到关机，不在本文件释放；初始化失败时
 * 保持 NULL，依赖它的后续 initcall 不应获得有效父对象。
 */
struct kobject *mm_kobj;

#ifdef CONFIG_SMP
/*
 * vm_committed_as_batch 是 percpu_counter 批量折算全局 committed memory
 * 的阈值，单位为 pages。启动、overcommit 策略变化和内存 hotplug 会重算；
 * 各 CPU 记账路径并发读取。较大 batch 降低全局锁竞争但放大暂时误差，
 * OVERCOMMIT_NEVER 因需严格限额而使用更小比例。初值 32 保证早期可用。
 */
s32 vm_committed_as_batch = 32;

/*
 * mm_compute_batch() - 根据 RAM、CPU 数和 overcommit 策略重算记账批量。
 *
 * @overcommit_policy 是 OVERCOMMIT_* 策略值，按值输入。调用者包括启动
 * initcall、内存 online/offline notifier 和 sysctl 策略更新。函数可能
 * 在启动或 hotplug 进程上下文执行，不分配、不睡眠，也不取得对象引用。
 *
 * 返回：无直接返回值。副作用是一次写 vm_committed_as_batch；严格策略
 * 取每 CPU RAM 的约 0.4%，其他策略取 25%，均封顶 INT_MAX，再与
 * max(2*present_cpus, 32) 取大值。短暂并发读到旧阈值只影响批量误差，
 * 不改变最终 percpu_counter 总量。
 */
void mm_compute_batch(int overcommit_policy)
{
	/*
	 * nr 是当前 present CPU 数；batch 是避免过于频繁刷新全局计数的
	 * 拓扑下限；ram_pages/memsized_batch 以 page 为单位。
	 */
	u64 memsized_batch;
	s32 nr = num_present_cpus();
	s32 batch = max_t(s32, nr*2, 32);
	unsigned long ram_pages = totalram_pages();

	/*
	 * For policy OVERCOMMIT_NEVER, set batch size to 0.4% of
	 * (total memory/#cpus), and lift it to 25% for other policies
	 * to ease the possible lock contention for percpu_counter
	 * vm_committed_as, while the max limit is INT_MAX
	 */
	/*
	 * OVERCOMMIT_NEVER 使用 `(RAM/CPU)/256`，把每 CPU 未折算误差约束到
	 * 0.4%；宽松策略可放大到 25% 以减少 vm_committed_as 全局锁竞争。
	 * u64 中间值避免大内存乘除溢出，最终限制为 s32 可表示的 INT_MAX。
	 */
	if (overcommit_policy == OVERCOMMIT_NEVER)
		memsized_batch = min_t(u64, ram_pages/nr/256, INT_MAX);
	else
		memsized_batch = min_t(u64, ram_pages/nr/4, INT_MAX);

	vm_committed_as_batch = max_t(s32, memsized_batch, batch);
}

/*
 * mm_compute_batch_notifier() - 在内存容量变化后刷新 committed batch。
 *
 * @self 是 notifier 链传入的静态 notifier_block 借用指针，本函数不用；
 * @action 是 MEM_* 事件；@arg 是事件数据借用指针，本函数不用且不保存。
 * MEM_ONLINE/MEM_OFFLINE 表示 totalram_pages 已变化，需按当前 sysctl 策略
 * 重算；其他通知无副作用。固定返回 NOTIFY_OK，不阻止 notifier 链继续，
 * 无资源获取或失败回滚。
 */
static int __meminit mm_compute_batch_notifier(struct notifier_block *self,
					unsigned long action, void *arg)
{
	switch (action) {
	case MEM_ONLINE:
	case MEM_OFFLINE:
		mm_compute_batch(sysctl_overcommit_memory);
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

/*
 * mm_compute_batch_init() - 建立 SMP committed batch 初值并注册热插拔回调。
 *
 * 入参：无。启动 initcall 进程上下文中先按当前内存/CPU/策略计算，再通过
 * hotplug_memory_notifier() 注册静态回调，优先级为 MM_COMPUTE_BATCH_PRI。
 * 返回 0；无输出参数和普通失败路径。注册后 notifier 生命周期为
 * 系统全程，本文件不注销。
 */
static int __init mm_compute_batch_init(void)
{
	mm_compute_batch(sysctl_overcommit_memory);
	hotplug_memory_notifier(mm_compute_batch_notifier, MM_COMPUTE_BATCH_PRI);
	return 0;
}

__initcall(mm_compute_batch_init);

#endif
/* UP 构建不需要按 CPU 批量折算，mman.h 提供相应空接口。 */

/*
 * mm_sysfs_init() - 创建内存管理子系统的 sysfs 根目录。
 *
 * 入参：无。postcore initcall 在 kernel_kobj 已建立后运行，可以睡眠和
 * 分配。成功时 mm_kobj 持有新 `/sys/kernel/mm` kobject 并返回 0，
 * 后续 MM 子系统借用它创建子项；分配/注册失败返回 -ENOMEM，mm_kobj
 * 为 NULL，没有需由本函数回滚的已发布子项。
 */
static int __init mm_sysfs_init(void)
{
	mm_kobj = kobject_create_and_add("mm", kernel_kobj);
	if (!mm_kobj)
		return -ENOMEM;

	return 0;
}
postcore_initcall(mm_sysfs_init);

/*
 * 启动期 zone 边界工作集，单位均为 PFN：
 *
 * arch_zone_lowest/highest_possible_pfn[zone] 描述架构允许每种非 movable
 * zone 覆盖的全局半开区间；free_area_init() 根据 max_zone_pfn 构造。
 * zone_movable_pfn[nid] 是每个 node 上 ZONE_MOVABLE 的起始 PFN，0 表示
 * 该节点不建立 movable zone。三者在 node/zone 初始化期间只读，随后随
 * __initdata 回收；它们不是实际 memblock 区间的 ownership 容器。
 */
static unsigned long arch_zone_lowest_possible_pfn[MAX_NR_ZONES] __initdata;
static unsigned long arch_zone_highest_possible_pfn[MAX_NR_ZONES] __initdata;
static unsigned long zone_movable_pfn[MAX_NUMNODES] __initdata;

/*
 * kernelcore=/movablecore= 命令行请求的启动期状态，单位为 pages：
 *
 * required_* 保存绝对页数，required_*_percent 保存尚待按总页数换算的
 * 百分比。find_zone_movable_pfns_for_nodes() 会把百分比解析为页数，并在
 * 跨节点分摊过程中递减 required_kernelcore，把它当作“剩余未满足量”。
 * 这些变量只在单线程 init 阶段有效，不需要锁。
 */
static unsigned long required_kernelcore __initdata;
static unsigned long required_kernelcore_percent __initdata;
static unsigned long required_movablecore __initdata;
static unsigned long required_movablecore_percent __initdata;

/*
 * nr_kernel_pages 是 memblock 当前可释放的非 HIGHMEM 完整页数，
 * nr_all_pages 是包含 HIGHMEM 的全部可释放完整页数；free_area_init()
 * 计算，后续 alloc_large_system_hash() 用它们估算默认哈希表规模。
 * 单位是 pages，启动后不再需要并随 __initdata 回收。
 */
static unsigned long nr_kernel_pages __initdata;
static unsigned long nr_all_pages __initdata;

/*
 * deferred_struct_pages 标记是否确实推迟了部分 struct page 初始化。
 * memmap_init_range() 在首次 defer 时置 true；mm_core_init() 和
 * page_alloc_init_late() 据此决定 page_ext 等元数据应立即还是延后建立。
 * __meminitdata 使无 hotplug 构建可在启动后回收。
 */
static bool deferred_struct_pages __meminitdata;

/*
 * boot_nodestats 是所有 pgdat 在正式 per-node/per-CPU 统计存储准备好前
 * 借用的启动期占位数组。free_area_init_core() 发布该指针；hotplug/
 * 后续初始化识别这一特殊地址，避免把静态 per-CPU 存储当成动态
 * 对象释放。
 */
static DEFINE_PER_CPU(struct per_cpu_nodestat, boot_nodestats);

/*
 * cmdline_parse_core() - 解析 kernelcore=/movablecore= 的数值或百分比。
 *
 * @p 是 early_param 借用的可写 NUL 字符串，可为 NULL；@core 指向调用者
 * 的页数输出，@percent 指向百分比输出，后二者不可为 NULL且 ownership
 * 不转移。百分比形式写 percent、保留 core；容量形式由 memparse() 解析
 * bytes，向下换算为 pages 写 core，并把 percent 清 0。
 *
 * 返回 0 表示已解析，p==NULL 返回 -EINVAL。超过 100% 或页数不能装入
 * unsigned long 通过 WARN 暴露异常，但仍保留解析结果，真正的边界收敛
 * 由后续 movable 计算完成。函数处于单线程 early boot，不睡眠、不分配。
 */
static int __init cmdline_parse_core(char *p, unsigned long *core,
				     unsigned long *percent)
{
	/*
	 * coremem 先承载无单位整数/最终 byte 数，endptr 区分 `%` 后缀；
	 * 两者只在本次解析有效。
	 */
	unsigned long long coremem;
	char *endptr;

	if (!p)
		return -EINVAL;

	/* Value may be a percentage of total memory, otherwise bytes */
	/*
	 * 值可以是总内存百分比，否则按带 K/M/G 后缀的字节容量处理。
	 * simple_strtoull() 只用于观察紧随数字的 `%`，容量分支再由 memparse
	 * 完整解释单位。
	 */
	coremem = simple_strtoull(p, &endptr, 0);
	if (*endptr == '%') {
		/* Paranoid check for percent values greater than 100 */
		/*
		 * 防御性警告百分比超过 100；后续计算仍会用总页数
		 * 进行限制。
		 */
		WARN_ON(coremem > 100);

		*percent = coremem;
	} else {
		coremem = memparse(p, &p);
		/* Paranoid check that UL is enough for the coremem value */
		/*
		 * 防御性确认 byte->page 后能装入 unsigned long；写入时以
		 * PAGE_SHIFT 向下取整到完整页。
		 */
		WARN_ON((coremem >> PAGE_SHIFT) > ULONG_MAX);

		*core = coremem >> PAGE_SHIFT;
		*percent = 0UL;
	}
	return 0;
}

/*
 * mirrored_kernelcore 表示 kernelcore=mirror：不可迁移 kernel allocations
 * 应优先位于固件标记的镜像内存，非镜像高地址区划入 ZONE_MOVABLE。
 * 命令行解析写入，movable 边界和 absent-page 计算读取；仅 memblock/init
 * 阶段有效，__initdata_memblock 遵循 memblock 注解的回收时机。
 */
bool mirrored_kernelcore __initdata_memblock;

/*
 * kernelcore=size sets the amount of memory for use for allocations that
 * cannot be reclaimed or migrated.
 */
/*
 * kernelcore=size 指定必须保留给不可回收或不可迁移分配的内存量。
 * 特殊值 mirror 不表示容量，而是启用镜像内存策略。
 */
/*
 * cmdline_parse_kernelcore() - 解析 kernelcore= 的镜像或容量策略。
 *
 * @p 是 early_param 借用字符串，不转移 ownership。精确选项 mirror
 * 置 mirrored_kernelcore 并返回 0；其他输入委托 cmdline_parse_core()
 * 写 required_kernelcore/percent，并原样返回 0 或 -EINVAL。
 * 运行于 early boot，无锁、不睡眠、无资源回滚。
 */
static int __init cmdline_parse_kernelcore(char *p)
{
	/* parse kernelcore=mirror */
	/*
	 * mirror 分支不再解析 movablecore 容量，后续边界算法以
	 * 镜像属性为准。
	 */
	if (parse_option_str(p, "mirror")) {
		mirrored_kernelcore = true;
		return 0;
	}

	return cmdline_parse_core(p, &required_kernelcore,
				  &required_kernelcore_percent);
}
early_param("kernelcore", cmdline_parse_kernelcore);

/*
 * movablecore=size sets the amount of memory for use for allocations that
 * can be reclaimed or migrated.
 */
/*
 * movablecore=size 指定希望可回收或可迁移分配使用的内存量；后续算法会
 * 反推至少需要保留多少 kernelcore，并按 node 平衡。
 */
/*
 * cmdline_parse_movablecore() - 解析 movablecore= 容量或百分比。
 *
 * @p 是借用命令行字符串；输出直接写 required_movablecore/percent。
 * 返回 cmdline_parse_core() 的 0/-EINVAL，无其他副作用、不会睡眠。
 */
static int __init cmdline_parse_movablecore(char *p)
{
	return cmdline_parse_core(p, &required_movablecore,
				  &required_movablecore_percent);
}
early_param("movablecore", cmdline_parse_movablecore);

/*
 * early_calculate_totalpages()
 * Sum pages in active regions for movable zone.
 * Populate N_MEMORY for calculating usable_nodes.
 */
/*
 * 统计 movable-zone 计算可使用的 memblock active pages，并设置 N_MEMORY
 * 节点状态，以便后续计算 usable_nodes。这里的 N_MEMORY 是临时借用状态，
 * find_zone_movable_pfns_for_nodes() 保存并最终恢复原 nodemask。
 */
/*
 * early_calculate_totalpages() - 汇总所有节点有效 memblock PFN 数量。
 *
 * 入参：无。运行在 memblock 仍有效的单线程 __init 阶段，不持锁、不
 * 睡眠。遍历每个 [start_pfn,end_pfn) 区间，把非空区间页数累加，并临时
 * 将其 nid 标为 N_MEMORY。返回总页数，单位 pages；无失败和 ownership
 * 转移。
 */
static unsigned long __init early_calculate_totalpages(void)
{
	/*
	 * totalpages 是累计页数；start/end 是当前半开 PFN 区间；i 是 memblock
	 * 迭代游标，nid 是区间所属 node，pages 是当前区间长度。
	 */
	unsigned long totalpages = 0;
	unsigned long start_pfn, end_pfn;
	int i, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid) {
		unsigned long pages = end_pfn - start_pfn;

		totalpages += pages;
		if (pages)
			node_set_state(nid, N_MEMORY);
	}
	return totalpages;
}

/*
 * This finds a zone that can be used for ZONE_MOVABLE pages. The
 * assumption is made that zones within a node are ordered in monotonic
 * increasing memory addresses so that the "highest" populated zone is used
 */
/*
 * 寻找承载 ZONE_MOVABLE 的基础 zone。算法依赖同一 node 内 zone 按物理
 * 地址单调递增，因此选择最高的非空非 MOVABLE zone；movable zone 将从
 * 该 zone 的高端切出，而不是拥有独立架构地址窗口。
 */
/*
 * find_usable_zone_for_movable() - 选择 ZONE_MOVABLE 所切分的 zone 类型。
 *
 * 入参：无。arch_zone_* 数组必须已经由 free_area_init() 建立。函数在
 * 单线程 __init 上下文从高 zone 向低 zone 查找首个非空候选，跳过
 * ZONE_MOVABLE 自身，最终写全局 movable_zone。
 *
 * 返回：无直接返回值。若架构没有任何可用普通 zone，VM_BUG_ON 表示
 * 启动布局不成立且不可回滚；成功后后续每节点 movable PFN 计算都以该
 * zone 的最低可能 PFN 为边界。
 */
static void __init find_usable_zone_for_movable(void)
{
	/* zone_index 是逆序扫描的 zone 类型，-1 表示没有合法候选。 */
	int zone_index;
	for (zone_index = MAX_NR_ZONES - 1; zone_index >= 0; zone_index--) {
		if (zone_index == ZONE_MOVABLE)
			continue;

		if (arch_zone_highest_possible_pfn[zone_index] >
				arch_zone_lowest_possible_pfn[zone_index])
			break;
	}

	VM_BUG_ON(zone_index == -1);
	movable_zone = zone_index;
}

/*
 * Find the PFN the Movable zone begins in each node. Kernel memory
 * is spread evenly between nodes as long as the nodes have enough
 * memory. When they don't, some nodes will have more kernelcore than
 * others
 */
/*
 * 为每个 node 寻找 ZONE_MOVABLE 起始 PFN。只要节点容量允许，kernelcore
 * 会在节点间均匀分摊；容量不足的节点先耗尽，剩余 kernelcore 由其他节点
 * 承担，因此最终某些节点可能保留更多不可迁移内存。
 */
/*
 * find_zone_movable_pfns_for_nodes() - 计算每节点 movable/kernelcore 分界。
 *
 * 入参：无。调用时 memblock 物理区间、node id、mirror/hotplug 属性以及
 * arch_zone_* 边界均已建立，普通 buddy 尚未接管。函数在 SMP 前单线程
 * __init 上下文运行，不持锁、不睡眠，不取得 memblock_region ownership。
 *
 * 输入策略按优先级互斥：
 *   1. movable_node：所有 hotpluggable region 从各自最低 PFN 起 movable；
 *   2. kernelcore=mirror：非镜像且位于 4GiB 以上的内存划为 movable；
 *   3. kernelcore=/movablecore=：换算页数并跨 N_MEMORY 节点均摊。
 *
 * 返回：无直接返回值。成功副作用是填写 zone_movable_pfn[nid] 和全局
 * movable_zone；0 表示该 node 不建立 ZONE_MOVABLE。函数临时借用并修改
 * N_MEMORY nodemask 来统计 usable node，所有出口都恢复 saved_node_state。
 * 无普通 errno；无有效请求、镜像条件不满足或 kdump 环境会安全保持无
 * movable zone，布局矛盾由前置 helper 的 VM_BUG_ON 暴露。
 */
static void __init find_zone_movable_pfns_for_nodes(void)
{
	/*
	 * 变量地图：
	 *   i/nid                 memblock 区间游标和当前 node。
	 *   usable_startpfn       movable_zone 架构允许的最低 PFN。
	 *   kernelcore_node       当前轮计划分给每个剩余 node 的 kernel pages。
	 *   kernelcore_remaining  当前 node 尚需容纳的 kernel pages。
	 *   saved_node_state      进入函数前真实 N_MEMORY 状态，所有出口恢复。
	 *   totalpages            active memblock 总页数。
	 *   usable_nodes          当前重平衡轮仍可承担 kernelcore 的 node 数。
	 *   r                     借用的当前 memblock.memory region。
	 */
	int i, nid;
	unsigned long usable_startpfn;
	unsigned long kernelcore_node, kernelcore_remaining;
	/* save the state before borrow the nodemask */
	/*
	 * 在临时借用 N_MEMORY nodemask 前保存原值；函数退出时必须完整恢复，
	 * 不能把为容量计算补入的 node 状态发布给后续子系统。
	 */
	nodemask_t saved_node_state = node_states[N_MEMORY];
	unsigned long totalpages = early_calculate_totalpages();
	int usable_nodes = nodes_weight(node_states[N_MEMORY]);
	struct memblock_region *r;

	/* Need to find movable_zone earlier when movable_node is specified. */
	/*
	 * movable_node 模式也需要知道 ZONE_MOVABLE 从哪种普通 zone 高端切分，
	 * 所以在任何策略分支前先发布 movable_zone。
	 */
	find_usable_zone_for_movable();

	/*
	 * If movable_node is specified, ignore kernelcore and movablecore
	 * options.
	 */
	/*
	 * movable_node 优先于 kernelcore/movablecore：逐个借用 memblock region，
	 * 只处理 firmware/架构标记为 hotpluggable 的区间，并记录每节点最小
	 * 起始 PFN。这样整个 hotplug 尾部落入 movable，便于日后内存热移除。
	 */
	if (movable_node_is_enabled()) {
		for_each_mem_region(r) {
			if (!memblock_is_hotpluggable(r))
				continue;

			nid = memblock_get_region_node(r);

			usable_startpfn = memblock_region_memory_base_pfn(r);
			zone_movable_pfn[nid] = zone_movable_pfn[nid] ?
				min(usable_startpfn, zone_movable_pfn[nid]) :
				usable_startpfn;
		}

		goto out2;
	}

	/*
	 * If kernelcore=mirror is specified, ignore movablecore option
	 */
	/*
	 * kernelcore=mirror 同样覆盖 movablecore。目标是让不可迁移内核分配
	 * 留在镜像内存，把非镜像高地址内存切入 ZONE_MOVABLE。
	 */
	if (mirrored_kernelcore) {
		/*
		 * 记录 4GiB 以下是否存在非镜像区。低地址可能服务
		 * DMA/受限 zone，不能简单切走，因此只警告最终仍会有
		 * 非镜像 kernel memory。
		 */
		bool mem_below_4gb_not_mirrored = false;

		/*
		 * 固件没有任何镜像 region 时该策略无从实现，保持无
		 * movable zone。
		 */
		if (!memblock_has_mirror()) {
			pr_warn("The system has no mirror memory, ignore kernelcore=mirror.\n");
			goto out;
		}

		/*
		 * crash kernel 使用的保留内存布局不能按原内核镜像假设
		 * 重切 zone，因而明确禁用此策略。
		 */
		if (is_kdump_kernel()) {
			pr_warn("The system is under kdump, ignore kernelcore=mirror.\n");
			goto out;
		}

		/*
		 * 找出每节点首个 4GiB 以上的非镜像 region，作为
		 * movable 候选。
		 */
		for_each_mem_region(r) {
			if (memblock_is_mirror(r))
				continue;

			nid = memblock_get_region_node(r);

			usable_startpfn = memblock_region_memory_base_pfn(r);

			/*
			 * 低 4GiB 非镜像区保留为 kernel zone，并在扫描后
			 * 给出风险提示。
			 */
			if (usable_startpfn < PHYS_PFN(SZ_4G)) {
				mem_below_4gb_not_mirrored = true;
				continue;
			}

			zone_movable_pfn[nid] = zone_movable_pfn[nid] ?
				min(usable_startpfn, zone_movable_pfn[nid]) :
				usable_startpfn;
		}

		if (mem_below_4gb_not_mirrored)
			pr_warn("This configuration results in unmirrored kernel memory.\n");

		goto out2;
	}

	/*
	 * If kernelcore=nn% or movablecore=nn% was specified, calculate the
	 * amount of necessary memory.
	 */
	/*
	 * 百分比在已知 totalpages 后才可换算。required_* 从这里开始统一以
	 * pages 表示，供后续容量和跨节点运算。
	 */
	if (required_kernelcore_percent)
		required_kernelcore = (totalpages * 100 * required_kernelcore_percent) /
				       10000UL;
	if (required_movablecore_percent)
		required_movablecore = (totalpages * 100 * required_movablecore_percent) /
					10000UL;

	/*
	 * If movablecore= was specified, calculate what size of
	 * kernelcore that corresponds so that memory usable for
	 * any allocation type is evenly spread. If both kernelcore
	 * and movablecore are specified, then the value of kernelcore
	 * will be used for required_kernelcore if it's greater than
	 * what movablecore would have allowed.
	 */
	/*
	 * movablecore 请求先反推允许的 kernelcore。若两个参数同时给出，
	 * 选择更大的 kernelcore，保证显式不可迁移容量下限不会被 movable
	 * 请求削弱。
	 */
	if (required_movablecore) {
		/* corepages 是满足对齐后的 movable 请求后剩余的 kernel pages。 */
		unsigned long corepages;

		/*
		 * Round-up so that ZONE_MOVABLE is at least as large as what
		 * was requested by the user
		 */
		/*
		 * 将 movable 请求向上对齐到 buddy 最大 order 的页块粒度并限制在
		 * totalpages 内，避免最终边界对齐让可移动容量低于用户请求。
		 */
		required_movablecore =
			round_up(required_movablecore, MAX_ORDER_NR_PAGES);
		required_movablecore = min(totalpages, required_movablecore);
		corepages = totalpages - required_movablecore;

		required_kernelcore = max(required_kernelcore, corepages);
	}

	/*
	 * If kernelcore was not specified or kernelcore size is larger
	 * than totalpages, there is no ZONE_MOVABLE.
	 */
	/*
	 * 未请求 kernelcore，或要求整个内存都保留给 kernel 时，不存在可切分
	 * 的 movable 尾部；直接恢复临时 nodemask。
	 */
	if (!required_kernelcore || required_kernelcore >= totalpages)
		goto out;

	/* usable_startpfn is the lowest possible pfn ZONE_MOVABLE can be at */
	/*
	 * ZONE_MOVABLE 不能侵入 DMA/DMA32 等低地址受限 zone，最低只能从
	 * movable_zone 对应架构区间起点开始。
	 */
	usable_startpfn = arch_zone_lowest_possible_pfn[movable_zone];

restart:
	/* Spread kernelcore memory as evenly as possible throughout nodes */
	/*
	 * 每轮把尚未满足的 required_kernelcore 均分给仍有承载能力的 nodes。
	 * 某 node 容量不足时，本轮之后减少 usable_nodes，再让其余 node
	 * 吸收剩余量。
	 */
	kernelcore_node = required_kernelcore / usable_nodes;
	for_each_node_state(nid, N_MEMORY) {
		/* start/end 是当前 node 的 memblock PFN 区间边界。 */
		unsigned long start_pfn, end_pfn;

		/*
		 * Recalculate kernelcore_node if the division per node
		 * now exceeds what is necessary to satisfy the requested
		 * amount of memory for the kernel
		 */
		/*
		 * 剩余总量下降后重新收紧每节点份额，避免后面的 node
		 * 过度保留。
		 */
		if (required_kernelcore < kernelcore_node)
			kernelcore_node = required_kernelcore / usable_nodes;

		/*
		 * As the map is walked, we track how much memory is usable
		 * by the kernel using kernelcore_remaining. When it is
		 * 0, the rest of the node is usable by ZONE_MOVABLE
		 */
		/*
		 * kernelcore_remaining 归零即建立本 node 的分界：后续更高 PFN
		 * 都可以属于 ZONE_MOVABLE。
		 */
		kernelcore_remaining = kernelcore_node;

		/* Go through each range of PFNs within this node */
		/*
		 * node 可能有空洞和多个 memblock range；扫描从上一轮已推进的
		 * zone_movable_pfn 开始，避免 restart 重复计算已确认的 kernel 页。
		 */
		for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, NULL) {
			/* size_pages 是当前 range 中继续划给 kernelcore 的页数。 */
			unsigned long size_pages;

			start_pfn = max(start_pfn, zone_movable_pfn[nid]);
			if (start_pfn >= end_pfn)
				continue;

			/* Account for what is only usable for kernelcore */
			/*
			 * movable_zone 最低 PFN 以下天然只能属于低地址 kernel zones；
			 * 这些页也应抵扣本 node 和全局 kernelcore 请求。
			 */
			if (start_pfn < usable_startpfn) {
				/*
				 * kernel_pages 是当前 range 落在低地址受限区
				 * 的页数。
				 */
				unsigned long kernel_pages;
				kernel_pages = min(end_pfn, usable_startpfn)
								- start_pfn;

				kernelcore_remaining -= min(kernel_pages,
							kernelcore_remaining);
				required_kernelcore -= min(kernel_pages,
							required_kernelcore);

				/* Continue if range is now fully accounted */
				/*
				 * 整个 range 都在低区时，本轮只推进游标，
				 * 无 movable 候选。
				 */
				if (end_pfn <= usable_startpfn) {

					/*
					 * Push zone_movable_pfn to the end so
					 * that if we have to rebalance
					 * kernelcore across nodes, we will
					 * not double account here
					 */
					/*
					 * 把 marker 推到 range 末端，restart 时
					 * 从下一段继续，防止再次扣减同一批低区
					 * kernel pages。
					 */
					zone_movable_pfn[nid] = end_pfn;
					continue;
				}
				start_pfn = usable_startpfn;
			}

			/*
			 * The usable PFN range for ZONE_MOVABLE is from
			 * start_pfn->end_pfn. Calculate size_pages as the
			 * number of pages used as kernelcore
			 */
			/*
			 * 当前 [start,end) 已具备 movable 资格，但仍先从低端取出
			 * kernelcore_remaining；marker 后的尾部才成为 ZONE_MOVABLE。
			 */
			size_pages = end_pfn - start_pfn;
			if (size_pages > kernelcore_remaining)
				size_pages = kernelcore_remaining;
			zone_movable_pfn[nid] = start_pfn + size_pages;

			/*
			 * Some kernelcore has been met, update counts and
			 * break if the kernelcore for this node has been
			 * satisfied
			 */
			/*
			 * 同步扣减全局和本 node 剩余量。当前 node 份额满足后
			 * 立即停止扫描，保留 marker 作为最终或下一轮
			 * 重平衡起点。
			 */
			required_kernelcore -= min(required_kernelcore,
								size_pages);
			kernelcore_remaining -= size_pages;
			if (!kernelcore_remaining)
				break;
		}
	}

	/*
	 * If there is still required_kernelcore, we do another pass with one
	 * less node in the count. This will push zone_movable_pfn[nid] further
	 * along on the nodes that still have memory until kernelcore is
	 * satisfied
	 */
	/*
	 * 若小节点无法承担均分份额，减少参与均分的 node 数并重新扫描。
	 * zone_movable_pfn 已记录前一轮进度，因此只会继续向高 PFN 推进。
	 */
	usable_nodes--;
	if (usable_nodes && required_kernelcore > usable_nodes)
		goto restart;

out2:
	/* Align start of ZONE_MOVABLE on all nids to MAX_ORDER_NR_PAGES */
	/*
	 * 最终边界向上对齐到 buddy 最大 order 页块，避免一个最大 pageblock
	 * 横跨 kernel/movable zone。对齐后越过 node 末端表示该 node 实际
	 * 没有 movable 容量，重新以 0 表示。
	 */
	for_each_node_state(nid, N_MEMORY) {
		/*
		 * start_pfn 仅用于 helper 输出占位，end_pfn 用于检查
		 * 对齐后边界。
		 */
		unsigned long start_pfn, end_pfn;

		zone_movable_pfn[nid] =
			round_up(zone_movable_pfn[nid], MAX_ORDER_NR_PAGES);

		get_pfn_range_for_nid(nid, &start_pfn, &end_pfn);
		if (zone_movable_pfn[nid] >= end_pfn)
			zone_movable_pfn[nid] = 0;
	}

out:
	/* restore the node_state */
	/*
	 * early_calculate_totalpages() 临时把含 active range 的节点加入
	 * N_MEMORY；所有策略出口在这里恢复调用前状态，不能把计算工作集
	 * 误发布为最终 node state。
	 */
	node_states[N_MEMORY] = saved_node_state;
}

/*
 * __init_single_page() - 把一个 struct page 建立为“已描述但仍保留”的基态。
 *
 * @page 是该 PFN 对应 memmap 槽的借用可写指针，不可为 NULL；@pfn 是物理
 * 页帧号，@zone 是 enum zone_type 数值，@nid 是所属 NUMA node id。
 * 调用者保证三者映射一致，page 尚未发布到 buddy 或其他并发索引。
 *
 * 函数清零整个描述符，写入 zone/node/section 链接，建立初始引用计数、
 * `_mapcount=-1`（没有用户映射）、CPU/KASAN 初值和空 lru 链表。需要
 * page->virtual 的架构只为非 HIGHMEM 页缓存直接映射地址。
 *
 * 返回：无直接返回值。page 存储 ownership 不转移；成功后描述符可由
 * 后续 pageblock、reserved/free 和 buddy 发布阶段继续设置。运行于早期
 * meminit 或受控 hotplug 上下文，不自行加锁、不睡眠、无失败回滚。
 */
void __meminit __init_single_page(struct page *page, unsigned long pfn,
				unsigned long zone, int nid)
{
	/*
	 * 阶段 1：先清除 memmap 中可能残留的字节，再建立位置和引用基态；
	 * 在这些字段完成前不能让任何 page allocator reader 观察该 page。
	 */
	mm_zero_struct_page(page);
	set_page_links(page, zone, nid, pfn);
	init_page_count(page);
	atomic_set(&page->_mapcount, -1);
	page_cpupid_reset_last(page);
	page_kasan_tag_reset(page);

	/* 阶段 2：建立未入任何 LRU/伙伴链表的自洽空链表状态。 */
	INIT_LIST_HEAD(&page->lru);
#ifdef WANT_PAGE_VIRTUAL
	/* The shift won't overflow because ZONE_NORMAL is below 4G. */
	/*
	 * PFN 左移 PAGE_SHIFT 不会溢出，因为需要 page->virtual 的该模型中
	 * ZONE_NORMAL 位于 4GiB 以下。HIGHMEM 没有永久直接映射，不能缓存
	 * __va 地址；普通 zone 则记录其线性映射地址。
	 */
	if (!is_highmem_idx(zone))
		set_page_address(page, __va(pfn << PAGE_SHIFT));
#endif
}

#ifdef CONFIG_NUMA
/*
 * During memory init memblocks map pfns to nids. The search is expensive and
 * this caches recent lookups. The implementation of __early_pfn_to_nid
 * treats start/end as pfns.
 */
/*
 * 内存初始化期间由 memblock 维护 PFN 到 nid 的映射，逐页搜索代价较高；
 * 该缓存保存最近命中的连续 PFN 区间。__early_pfn_to_nid() 把 start/end
 * 明确按 PFN（而非 byte address）解释。
 */
/*
 * mminit_pfnnid_cache - 最近一次 memblock PFN->nid 查询快照。
 *
 * @last_start/@last_end：最近命中区间的半开 PFN 边界；
 * @last_nid：该完整区间所属 node。
 *
 * 对象不拥有 memblock region，只缓存标量派生结果。全局实例由
 * early_pfn_lock 保护，因为 memblock 释放/kexec handover 等 meminit
 * 调用可能并发；缓存只在 __meminit 生命周期有效。
 */
struct mminit_pfnnid_cache {
	unsigned long last_start;
	unsigned long last_end;
	int last_nid;
};

/*
 * early_pfnnid_cache 是全局最近命中缓存，初始全零代表空区间。只能通过
 * early_pfn_to_nid() 持 early_pfn_lock 访问，避免两个初始化者交错发布
 * start/end/nid 而形成不匹配快照。
 */
static struct mminit_pfnnid_cache early_pfnnid_cache __meminitdata;

/*
 * Required by SPARSEMEM. Given a PFN, return what node the PFN is on.
 */
/*
 * SPARSEMEM 需要在初始化 section 的 struct page 时知道每个 PFN 所属 node；
 * 本 helper 提供带区间缓存的 memblock 查询。
 */
/*
 * __early_pfn_to_nid() - 在调用者提供的缓存中查询 PFN 所属 node。
 *
 * @pfn 是输入页帧号；@state 是调用者拥有的可写缓存借用指针，
 * 不可为 NULL。调用者负责串行化同一 state，本函数不加锁、不睡眠。
 * 命中缓存直接返回 last_nid；未命中调用 memblock_search_pfn_nid()，
 * 成功时在调用者同步协议内按 start/end/nid 顺序更新，失败时保持旧缓存。
 *
 * 返回合法 nid 或 NUMA_NO_NODE。无引用/ownership 转移；输出副作用仅是
 * 成功查询时更新 state，start/end 单位均为 PFN。
 */
static int __meminit __early_pfn_to_nid(unsigned long pfn,
					struct mminit_pfnnid_cache *state)
{
	/* start/end 接收 memblock 命中区间，nid 是本次结果。 */
	unsigned long start_pfn, end_pfn;
	int nid;

	/* 半开区间命中避免重复遍历 memblock region 索引。 */
	if (state->last_start <= pfn && pfn < state->last_end)
		return state->last_nid;

	nid = memblock_search_pfn_nid(pfn, &start_pfn, &end_pfn);
	/*
	 * 只缓存成功结果；NUMA_NO_NODE 可能来自内存空洞，不能用未定义
	 * start/end 覆盖上一份有效快照。
	 */
	if (nid != NUMA_NO_NODE) {
		state->last_start = start_pfn;
		state->last_end = end_pfn;
		state->last_nid = nid;
	}

	return nid;
}

/*
 * early_pfn_to_nid() - 线程安全地查询早期 PFN 所属 node。
 *
 * @pfn 是输入页帧号。调用者包括 SPARSEMEM/memblock 和 kexec handover
 * 的 page 初始化路径；这些路径可能共享全局缓存，因此用静态 spinlock
 * 保护完整查询和三字段更新。函数不可睡眠、不取得 node/page 引用。
 *
 * 返回合法 nid。memblock 未找到时防御性回退 first_online_node，确保后续
 * set_page_links() 有可编码 node；这会牺牲空洞归属精度，但避免负 nid
 * 破坏 page flags。锁只保护缓存一致性，不保护 memblock 本身生命周期。
 */
int __meminit early_pfn_to_nid(unsigned long pfn)
{
	/*
	 * early_pfn_lock 序列化全局 cache；nid 是锁内查询结果，解锁后仅作为
	 * 标量返回，不依赖缓存对象寿命。
	 */
	static DEFINE_SPINLOCK(early_pfn_lock);
	int nid;

	spin_lock(&early_pfn_lock);
	nid = __early_pfn_to_nid(pfn, &early_pfnnid_cache);
	if (nid < 0)
		nid = first_online_node;
	spin_unlock(&early_pfn_lock);

	return nid;
}

/*
 * hashdist 控制 alloc_large_system_hash() 是否把大型哈希表按 NUMA node
 * 分散分配。默认由 HASHDIST_DEFAULT 决定，hashdist= 启动参数可覆盖；
 * 单内存节点时 fixup_hashdist() 强制关闭，因为分布没有局部性收益。
 * 启动后 dcache/inode/xfrm 等初始化只读该策略。
 */
bool hashdist = HASHDIST_DEFAULT;

/*
 * set_hashdist() - 解析 hashdist= 布尔启动参数。
 *
 * @str 是 __setup 框架借用字符串，不转移 ownership；kstrtobool() 成功
 * 时写全局 hashdist。返回 1 表示参数已被识别并成功消费，解析失败
 * 返回 0，让通用命令行代码按未处理参数诊断。早期单线程执行，
 * 不睡眠、不分配。
 */
static int __init set_hashdist(char *str)
{
	return kstrtobool(str, &hashdist) == 0;
}
__setup("hashdist=", set_hashdist);

/*
 * fixup_hashdist() - 根据最终 NUMA 内存节点数收敛哈希分布策略。
 *
 * 入参：无。free_area_init() 在 N_MEMORY 状态确定后调用；单节点时把
 * hashdist 清 false，多节点保留默认/命令行选择。返回无直接值，无失败、
 * 无锁；该写入发生在哈希表分配前。
 */
static inline void fixup_hashdist(void)
{
	if (num_node_state(N_MEMORY) == 1)
		hashdist = false;
}
#else
/*
 * CONFIG_NUMA=n 时 memblock.h 已把 hashdist 定义为 false；空 stub 保持
 * 调用点统一，入参和副作用均无。
 */
static inline void fixup_hashdist(void) {}
#endif /* CONFIG_NUMA */
/* NUMA 配置决定是否存在 PFN->nid 缓存和分布式大哈希分配策略。 */

#ifdef CONFIG_ZONE_DEVICE
/*
 * pageblock_migratetype_init_range() - 初始化设备内存区间的 pageblock 类型。
 *
 * @pfn 是起始 PFN，@nr_pages 是页数，@migratetype 是 MIGRATE_* 策略；
 * 三者按值输入。函数把起点向上对齐 pageblock，逐块调用
 * init_pageblock_migratetype()，不改变 struct page ownership。
 *
 * 调用处处于 ZONE_DEVICE memmap 初始化的可调度进程上下文；跨越每个
 * sparse section 边界时 cond_resched()，避免超大设备内存长时间独占 CPU。
 * 返回无直接值，无普通失败路径。
 */
static __meminit void pageblock_migratetype_init_range(unsigned long pfn,
		unsigned long nr_pages, int migratetype)
{
	/* end 是输入区间尾后 PFN；循环 pfn 随 pageblock 粒度推进。 */
	const unsigned long end = pfn + nr_pages;

	for (pfn = pageblock_align(pfn); pfn < end; pfn += pageblock_nr_pages) {
		init_pageblock_migratetype(pfn_to_page(pfn), migratetype, false);
		if (IS_ALIGNED(pfn, PAGES_PER_SECTION))
			cond_resched();
	}
}
#endif

/*
 * Initialize a reserved page unconditionally, finding its zone first.
 */
/*
 * 无条件初始化一个 reserved page，并先根据 pgdat zone span 找到其 zone。
 * 该入口用于 deferred/memblock handover 等必须补建单页元数据的路径。
 */
/*
 * __init_page_from_nid() - 按 PFN/nid 补建一个保留页的完整基础元数据。
 *
 * @pfn 是输入页帧号，@nid 是其已知 node id。NODE_DATA(nid) 和对应 memmap
 * 必须存在，调用者保证 PFN 位于该 node 某个 zone span；函数借用 pgdat、
 * zone 和 page，不转移 ownership。
 *
 * 先线性查找 zone id，再调用 __init_single_page()。若 PFN 位于 pageblock
 * 边界，还要发布该块迁移类型；kexec handover scratch 可覆盖默认
 * MIGRATE_MOVABLE。返回无直接值，无普通失败路径、不睡眠；若没有 zone
 * 覆盖，zid==MAX_NR_ZONES 会暴露调用者布局错误。
 */
void __meminit __init_page_from_nid(unsigned long pfn, int nid)
{
	/* pgdat 是 nid 的全局借用节点描述；zid 是待确定的 zone 类型。 */
	pg_data_t *pgdat;
	int zid;

	pgdat = NODE_DATA(nid);

	/* 阶段 1：在该 node 的固定 zone 数组中寻找覆盖 pfn 的 span。 */
	for (zid = 0; zid < MAX_NR_ZONES; zid++) {
		/* zone 是当前候选的借用指针，只在本次迭代使用。 */
		struct zone *zone = &pgdat->node_zones[zid];

		if (zone_spans_pfn(zone, pfn))
			break;
	}
	/* 阶段 2：建立 page 基态；此时仍是 reserved，不会进入 buddy。 */
	__init_single_page(pfn_to_page(pfn), pfn, zid, nid);

	/*
	 * 只在 pageblock 首 PFN 写整块迁移元数据，避免每页重复。KHO scratch
	 * 区需保持其特殊不可普通分配类型，否则默认采用 MOVABLE。
	 */
	if (pageblock_aligned(pfn)) {
		enum migratetype mt =
			kho_scratch_migratetype(pfn, MIGRATE_MOVABLE);
		init_pageblock_migratetype(pfn_to_page(pfn), mt, false);
	}
}

#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT
/*
 * pgdat_set_deferred_range() - 初始化 node 的“尚未开始延迟”哨兵。
 *
 * @pgdat 是正在构造的借用可写节点描述，不可为 NULL。将
 * first_deferred_pfn 设为 ULONG_MAX，表示当前没有 PFN 被延迟；
 * defer_init() 首次决定边界时再替换为真实 PFN。返回无直接值，
 * 早期单线程、无锁无失败。
 */
static inline void pgdat_set_deferred_range(pg_data_t *pgdat)
{
	pgdat->first_deferred_pfn = ULONG_MAX;
}

/* Returns true if the struct page for the pfn is initialised */
/*
 * 如果 pfn 对应的 struct page 已初始化则返回 true。online node 上从
 * first_deferred_pfn 起的尾部尚不可访问；offline/hotplug node 不使用这条
 * 启动期延迟边界，按已初始化处理。
 */
/*
 * early_page_initialised() - 查询 PFN 是否位于已建立的早期 memmap 前缀。
 *
 * @pfn 是输入页帧号，@nid 是所属 node id。函数只读 node online 状态和
 * first_deferred_pfn，不取引用、不睡眠。返回 false 仅表示该 online node
 * 的 PFN 位于 deferred 尾部；true 表示可安全操作 struct page。
 */
static inline bool __meminit early_page_initialised(unsigned long pfn, int nid)
{
	if (node_online(nid) && pfn >= NODE_DATA(nid)->first_deferred_pfn)
		return false;

	return true;
}

/*
 * Returns true when the remaining initialisation should be deferred until
 * later in the boot cycle when it can be parallelised.
 */
/*
 * 当剩余 struct page 初始化应推迟到稍后的可并行启动阶段时返回 true。
 * false 表示当前页必须在早期串行阶段立即初始化。
 */
/*
 * defer_init() - 为 node 的高端 memmap 选择唯一 deferred 起始 PFN。
 *
 * @nid 是目标 node；@pfn 是当前候选页帧；@end_pfn 是当前 zone/范围的
 * 尾后 PFN，均按值输入。函数运行在 SMP 启动前，由顺序 memmap 扫描调用，
 * 静态 prev_end_pfn/nr_initialised 因而无需锁。
 *
 * early page_ext 需要完整连续 page 元数据时禁止延迟；低地址受限 zone
 * 也必须全部建立，保证启动分配可用。最高 zone 至少串行初始化一个 sparse
 * section，并只在 section 边界发布 first_deferred_pfn。边界一旦发布，
 * 其后调用固定返回 true。
 *
 * 返回 true 表示当前及后续尾部交给 page_alloc_init_late() 并行完成，
 * false 表示调用者现在初始化。无资源分配和 errno 失败。
 */
static bool __meminit
defer_init(int nid, unsigned long pfn, unsigned long end_pfn)
{
	/*
	 * prev_end_pfn 标识当前扫描 zone，nr_initialised 统计该 zone 已串行
	 * 处理的页数；两者只在早期顺序调用期有效。
	 */
	static unsigned long prev_end_pfn, nr_initialised;

	/*
	 * early page_ext 必须与 struct page 同步建立，无法容忍中间一段描述符
	 * 尚未初始化，因此关闭 deferred 模式。
	 */
	if (early_page_ext_enabled())
		return false;

	/* Always populate low zones for address-constrained allocations */
	/*
	 * 始终填充低 zone，保证 DMA/DMA32 等地址受限启动分配有可用页。
	 * 只有 end_pfn 到达 node 最高 PFN 的最后 zone 才可延迟。
	 */
	if (end_pfn < pgdat_end_pfn(NODE_DATA(nid)))
		return false;

	if (NODE_DATA(nid)->first_deferred_pfn != ULONG_MAX)
		return true;

	/*
	 * prev_end_pfn static that contains the end of previous zone
	 * No need to protect because called very early in boot before smp_init.
	 */
	/*
	 * prev_end_pfn 保存前一个 zone 的末端；切换 zone 时重置计数。调用发生
	 * 在 smp_init 之前，没有并发写者，因此静态状态无需锁保护。
	 */
	if (prev_end_pfn != end_pfn) {
		prev_end_pfn = end_pfn;
		nr_initialised = 0;
	}

	/*
	 * We start only with one section of pages, more pages are added as
	 * needed until the rest of deferred pages are initialized.
	 */
	/*
	 * 启动阶段至少先建立一个 sparse section；超过该数量后仍等待下一个
	 * section 对齐 PFN，确保 early/deferred 两侧都以完整 section 为边界。
	 */
	nr_initialised++;
	if ((nr_initialised > PAGES_PER_SECTION) &&
	    (pfn & (PAGES_PER_SECTION - 1)) == 0) {
		NODE_DATA(nid)->first_deferred_pfn = pfn;
		return true;
	}
	return false;
}

/*
 * __init_deferred_page() - 按需补建一个尚在 deferred 尾部的 struct page。
 *
 * @pfn/@nid 标识目标页。若 early_page_initialised() 已为 true 则幂等
 * 返回；否则委托 __init_page_from_nid() 建立 page/zone/pageblock 基态。
 * 返回无直接值，不把 page 释放进 buddy，也不转移 ownership。
 */
static void __meminit __init_deferred_page(unsigned long pfn, int nid)
{
	if (early_page_initialised(pfn, nid))
		return;

	__init_page_from_nid(pfn, nid);
}
#else
/*
 * 未启用 CONFIG_DEFERRED_STRUCT_PAGE_INIT 时的等价策略组：
 *
 * pgdat 不记录 deferred 边界；所有 PFN 始终视为已初始化；defer_init()
 * 固定 false；单页补建 helper 无操作。各参数均被有意忽略，调用点无需
 * 散布条件编译，且不存在状态/ownership 副作用。
 */
static inline void pgdat_set_deferred_range(pg_data_t *pgdat) {}

static inline bool early_page_initialised(unsigned long pfn, int nid)
{
	return true;
}

static inline bool defer_init(int nid, unsigned long pfn, unsigned long end_pfn)
{
	return false;
}

static inline void __init_deferred_page(unsigned long pfn, int nid)
{
}
#endif /* CONFIG_DEFERRED_STRUCT_PAGE_INIT */
/* 配置只改变 memmap 初始化时机，最终每个可用 PFN 的 page 基态必须相同。 */

/*
 * init_deferred_page() - 向 memblock/KHO 暴露配置无关的单页补建入口。
 *
 * @pfn 是目标页帧号，@nid 是所属 node。启用 deferred 时只补建尚未完成的
 * page；关闭时为空操作。返回无直接值、不释放页面、不睡眠，调用者继续
 * 掌握该物理页和 struct page 的 ownership。
 */
void __meminit init_deferred_page(unsigned long pfn, int nid)
{
	__init_deferred_page(pfn, nid);
}

/* If zone is ZONE_MOVABLE but memory is mirrored, it is an overlapped init */
/*
 * kernelcore=mirror 可能让同一物理 span 同时落入 ZONE_MOVABLE 的连续范围
 * 和实际镜像 kernel region。后者已经/应由 kernel zone 初始化，movable
 * 扫描必须跳过，避免以错误 zone id 重复初始化同一 struct page。
 */
/*
 * overlap_memmap_init() - 跳过 movable span 中与镜像 region 重叠的 PFN。
 *
 * @zone 是当前 zone id；@pfn 是调用者拥有的输入输出 PFN 指针，不可为
 * NULL。仅 mirrored_kernelcore && ZONE_MOVABLE 时工作。静态 @r 缓存当前
 * memblock region，依赖早期 memmap 按 PFN 单调递增扫描，因此无需每页
 * 从头查找，也无需锁。
 *
 * 若 *pfn 位于镜像 region，将其推进到该 region 尾后 PFN并返回 true，
 * 告知调用者 continue；否则保持 *pfn 并返回 false。函数只借用 memblock
 * 元数据、不睡眠、不转移 ownership。
 */
static bool __meminit
overlap_memmap_init(unsigned long zone, unsigned long *pfn)
{
	/* r 是跨连续调用保留的最近 memblock region 借用指针。 */
	static struct memblock_region *r __meminitdata;

	if (mirrored_kernelcore && zone == ZONE_MOVABLE) {
		/*
		 * 缓存为空或当前 PFN 已越过 region 时，顺序推进到首个
		 * end_pfn 大于当前 PFN 的 region。
		 */
		if (!r || *pfn >= memblock_region_memory_end_pfn(r)) {
			for_each_mem_region(r) {
				if (*pfn < memblock_region_memory_end_pfn(r))
					break;
			}
		}
		/*
		 * 只有 PFN 同时不低于 region 起点且 region 带 mirror 属性，
		 * 才属于重复初始化区；一次跳到尾部而不是逐页 continue。
		 */
		if (*pfn >= memblock_region_memory_base_pfn(r) &&
		    memblock_is_mirror(r)) {
			*pfn = memblock_region_memory_end_pfn(r);
			return true;
		}
	}
	return false;
}

/*
 * Only struct pages that correspond to ranges defined by memblock.memory
 * are zeroed and initialized by going through __init_single_page() during
 * memmap_init_zone_range().
 *
 * But, there could be struct pages that correspond to holes in
 * memblock.memory. This can happen because of the following reasons:
 * - physical memory bank size is not necessarily the exact multiple of the
 *   arbitrary section size
 * - early reserved memory may not be listed in memblock.memory
 * - non-memory regions covered by the contiguous flatmem mapping
 * - memory layouts defined with memmap= kernel parameter may not align
 *   nicely with memmap sections
 *
 * Explicitly initialize those struct pages so that:
 * - PG_Reserved is set
 * - zone and node links point to zone and node that span the page if the
 *   hole is in the middle of a zone
 * - zone and node links point to adjacent zone/node if the hole falls on
 *   the zone boundary; the pages in such holes will be prepended to the
 *   zone/node above the hole except for the trailing pages in the last
 *   section that will be appended to the zone/node below.
 */
/*
 * memmap_init_zone_range() 只会清零并初始化 memblock.memory 明确定义范围
 * 对应的 struct page，但 memmap 数组按 section/flat span 分配，内部可能
 * 仍有物理内存空洞。来源包括：bank 大小不是 section 整数倍、早期保留区
 * 未列入 memblock.memory、FLATMEM 连续映射覆盖非内存区域，以及 memmap=
 * 参数边界未与 section 对齐。
 *
 * 这些空洞的 struct page 也必须显式建立：设置 PG_Reserved；若空洞位于
 * zone 中间，链接到覆盖它的 zone/node；若落在边界，链接到相邻 zone/node。
 * 通常空洞页前置到其上方 zone/node，只有最后 section 尾部页追加到下方
 * 最后 zone/node。它们永不作为可用 RAM 交给 buddy。
 */
/*
 * init_unavailable_range() - 初始化 memmap 中没有实际 RAM 的 PFN 空洞。
 *
 * @spfn/@epfn 是半开 PFN 区间；@zone/@node 指定这些描述符应编码的邻接
 * 归属。函数在单线程 __init 阶段遍历其中 `pfn_valid()` 的 memmap 槽，
 * 调用 __init_single_page() 后设置 PG_Reserved，防止任何释放路径把空洞
 * 当成物理页。
 *
 * 返回：无直接返回值。pgcnt 统计实际建立的描述符并打印诊断；无分配、
 * 不睡眠、无 ownership 转移或普通失败。
 */
static void __init init_unavailable_range(unsigned long spfn,
					  unsigned long epfn,
					  int zone, int node)
{
	/* pfn 是有效槽迭代游标；pgcnt 是已初始化空洞 page 数。 */
	unsigned long pfn;
	u64 pgcnt = 0;

	/*
	 * 只访问内存模型声明 pfn_valid 的 struct page 存储，跳过不存在的槽。
	 */
	for_each_valid_pfn(pfn, spfn, epfn) {
		__init_single_page(pfn_to_page(pfn), pfn, zone, node);
		__SetPageReserved(pfn_to_page(pfn));
		pgcnt++;
	}

	if (pgcnt)
		pr_info("On node %d, zone %s: %lld pages in unavailable ranges\n",
			node, zone_names[zone], pgcnt);
}

/*
 * Initially all pages are reserved - free ones are freed
 * up by memblock_free_all() once the early boot process is
 * done. Non-atomic initialization, single-pass.
 *
 * All aligned pageblocks are initialized to the specified migratetype
 * (usually MIGRATE_MOVABLE). Besides setting the migratetype, no related
 * zone stats (e.g., nr_isolate_pageblock) are touched.
 */
/*
 * 初始阶段所有 page 都保持保留/不可分配状态；早期启动完成后
 * memblock_free_all() 才把真正空闲的页交给 buddy。这里采用非原子、
 * 单遍初始化，因为早期没有并发 page 使用者。
 *
 * 每个对齐 pageblock 被设置为调用者指定的 migratetype（通常
 * MIGRATE_MOVABLE），避免不可迁移启动分配散落。这里只写 pageblock
 * 类型，不同步 nr_isolate_pageblock 等运行期 zone 统计。
 */
/*
 * memmap_init_range() - 初始化一个 PFN 区间的 struct page 与 pageblock。
 *
 * @size：区间页数，必须大于 0；@nid/@zone：归属 node 与 zone id；
 * @start_pfn：首 PFN；@zone_end_pfn：整个 zone 尾后 PFN，供 deferred
 * 判断最高 zone；@context：MEMINIT_EARLY 或 MEMINIT_HOTPLUG；
 * @altmap：ZONE_DEVICE 自托管 vmemmap 描述，可为 NULL，借用不转移；
 * @migratetype：对齐 pageblock 初始 MIGRATE_* 类型；
 * @isolate_pageblock：是否同时按隔离 pageblock 建立。
 *
 * 早期上下文可跳过镜像重叠并在高端触发 deferred；hotplug 上下文不允许
 * 物理洞，初始化后将普通页标 Offline、设备页标 Reserved。函数可在
 * hotplug 进程上下文 cond_resched()，不自行持锁，调用者负责隔离区间，
 * memmap/pageblock ownership 始终归内存模型和 zone。
 *
 * 返回无直接值。ZONE_DEVICE 且 altmap==NULL 时无操作返回；否则成功后
 * 已处理前缀的 page 描述符自洽，但仍未自动释放到 buddy。
 */
void __meminit memmap_init_range(unsigned long size, int nid, unsigned long zone,
		unsigned long start_pfn, unsigned long zone_end_pfn,
		enum meminit_context context,
		struct vmem_altmap *altmap, int migratetype,
		bool isolate_pageblock)
{
	/*
	 * pfn 是当前游标，end_pfn 是本次实际尾后边界；page 是当前 memmap
	 * 槽借用指针。size>0 保证 end_pfn-1 有效。
	 */
	unsigned long pfn, end_pfn = start_pfn + size;
	struct page *page;

	/* 维护全局已存在 memmap 的最高 PFN，供后续边界/诊断读者使用。 */
	if (highest_memmap_pfn < end_pfn - 1)
		highest_memmap_pfn = end_pfn - 1;

#ifdef CONFIG_ZONE_DEVICE
	/*
	 * Honor reservation requested by the driver for this ZONE_DEVICE
	 * memory. We limit the total number of pages to initialize to just
	 * those that might contain the memory mapping. We will defer the
	 * ZONE_DEVICE page initialization until after we have released
	 * the hotplug lock.
	 */
	/*
	 * 尊重设备驱动通过 altmap 预留的页：只初始化可能承载映射的
	 * 后半区。
	 * 设备 page 的完整关联会推迟到释放 hotplug lock 后，避免持锁执行
	 * 驱动相关初始化。
	 */
	if (zone == ZONE_DEVICE) {
		/* 没有 altmap 就没有本入口可初始化的 ZONE_DEVICE vmemmap 区间。 */
		if (!altmap)
			return;

		/*
		 * 从 altmap 基址开始时跳过 reserve 前缀；尾部由当前 altmap 已用
		 * offset 决定，而不是原始 size。
		 */
		if (start_pfn == altmap->base_pfn)
			start_pfn += altmap->reserve;
		end_pfn = altmap->base_pfn + vmem_altmap_offset(altmap);
	}
#endif

	for (pfn = start_pfn; pfn < end_pfn; ) {
		/*
		 * There can be holes in boot-time mem_map[]s handed to this
		 * function.  They do not exist on hotplugged memory.
		 */
		/*
		 * 启动期 mem_map span 可能含空洞/镜像重叠；hotplug 区间由调用者
		 * 严格描述，不存在这种洞。early 分支先整段跳过镜像 overlap，
		 * 再判断是否到达 deferred 边界。
		 */
		if (context == MEMINIT_EARLY) {
			if (overlap_memmap_init(zone, &pfn))
				continue;
			if (defer_init(nid, pfn, zone_end_pfn)) {
				/*
				 * 发布全局 deferred 标志并停止当前高端范围；
				 * 剩余 page 由 page_alloc_init_late() 的并行
				 * worker 建立。
				 */
				deferred_struct_pages = true;
				break;
			}
		}

		page = pfn_to_page(pfn);
		/* 当前 PFN 尚不可分配，安全执行非原子基础描述符初始化。 */
		__init_single_page(page, pfn, zone, nid);
		if (context == MEMINIT_HOTPLUG) {
#ifdef CONFIG_ZONE_DEVICE
			/* 设备页等待后续 onlining/pgmap 关联，先保持 Reserved。 */
			if (zone == ZONE_DEVICE)
				__SetPageReserved(page);
			else
#endif
				/*
				 * 普通 hotplug 页先标 Offline，只有 online_pages()
				 * 完成 zone/accounting 后才能交给伙伴系统。
				 */
				__SetPageOffline(page);
		}

		/*
		 * Usually, we want to mark the pageblock MIGRATE_MOVABLE,
		 * such that unmovable allocations won't be scattered all
		 * over the place during system boot.
		 */
		/*
		 * 通常把 pageblock 标 MIGRATE_MOVABLE，防止早期不可迁移分配
		 * 散布导致长期碎片。只在块首写一次；isolate_pageblock 由
		 * hotplug/设备调用者决定是否保持隔离。
		 */
		if (pageblock_aligned(pfn)) {
			init_pageblock_migratetype(page, migratetype,
					isolate_pageblock);
			cond_resched();
		}
		pfn++;
	}
}

/*
 * memmap_init_zone_range() - 初始化一个 memblock range 与单个 zone 的交集。
 *
 * @zone 是已计算 span 的借用可写 zone；@start_pfn/@end_pfn 是当前
 * memblock.memory 半开区间；@hole_pfn 是跨 range 的输入输出空洞游标；
 * @mt 是该 range 的初始迁移类型。所有权均不转移。
 *
 * 函数先把 range clamp 到 zone span；空交集无操作返回。非空时初始化真实
 * RAM，再把前一真实 range 尾到当前起点之间的有效 memmap 槽标 Reserved，
 * 最后把 *hole_pfn 发布为本次 end。早期单线程、无锁、无 errno。
 */
static void __init memmap_init_zone_range(struct zone *zone,
					  unsigned long start_pfn,
					  unsigned long end_pfn,
					  unsigned long *hole_pfn,
					  enum migratetype mt)
{
	/*
	 * zone_start/end 是 zone 自身半开 span；nid/zone_id 是写入 struct page
	 * links 的稳定标识。
	 */
	unsigned long zone_start_pfn = zone->zone_start_pfn;
	unsigned long zone_end_pfn = zone_start_pfn + zone->spanned_pages;
	int nid = zone_to_nid(zone), zone_id = zone_idx(zone);

	/* 阶段 1：只保留当前 memblock range 与 zone span 的真实交集。 */
	start_pfn = clamp(start_pfn, zone_start_pfn, zone_end_pfn);
	end_pfn = clamp(end_pfn, zone_start_pfn, zone_end_pfn);

	if (start_pfn >= end_pfn)
		return;

	/*
	 * 阶段 2：初始化交集中的真实 RAM，允许最高 zone 选择 deferred
	 * 尾部。
	 */
	memmap_init_range(end_pfn - start_pfn, nid, zone_id, start_pfn,
			  zone_end_pfn, MEMINIT_EARLY, NULL, mt, false);

	/*
	 * 阶段 3：若上一个有效 range 与当前 range 之间有 memmap 空洞，
	 * 先按当前 zone/node 建立为 Reserved，再推进全局 hole 游标。
	 */
	if (*hole_pfn < start_pfn)
		init_unavailable_range(*hole_pfn, start_pfn, zone_id, nid);

	*hole_pfn = end_pfn;
}

/*
 * memmap_init() - 按物理 PFN 顺序初始化所有 node/zone 的 memmap。
 *
 * 入参：无。调用时各 pgdat/zone span 已计算，memblock.memory 按 PFN
 * 有序且 KHO scratch 属性可查询。函数在 SMP 前 __init 上下文借用这些
 * 全局对象，不睡眠、不取得引用。
 *
 * 外层遍历每个真实 memblock range，内层与该 node 所有 populated zone
 * 求交；KHO scratch 可把默认 MIGRATE_MOVABLE 改为保留迁移类型。单一
 * hole_pfn 追踪全局 PFN 空洞。最后还要初始化真实 memory_end 到 memmap
 * 分配粒度末端的尾洞：SPARSEMEM 对齐 section，FLATMEM 对齐最大 order。
 *
 * 返回无直接值。完成后早期前缀的每个有效 struct page 已初始化，物理
 * 空洞均 Reserved；deferred 高端除外，稍后并行完成。页面仍未发布给 buddy。
 */
static void __init memmap_init(void)
{
	/*
	 * start/end 是当前真实 RAM range；hole_pfn 是上个已处理交集末端；
	 * i/j 是 memblock/zone 游标；zone_id/nid 保留最后有效归属供尾洞使用。
	 */
	unsigned long start_pfn, end_pfn;
	unsigned long hole_pfn = 0;
	int i, j, zone_id = 0, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid) {
		/* node 是该 range 所属 pgdat；mt 继承 KHO scratch 特殊迁移类型。 */
		struct pglist_data *node = NODE_DATA(nid);
		enum migratetype mt =
			kho_scratch_migratetype(start_pfn, MIGRATE_MOVABLE);

		for (j = 0; j < MAX_NR_ZONES; j++) {
			/* zone 是 node 固定 zone 数组中的当前借用候选。 */
			struct zone *zone = node->node_zones + j;

			if (!populated_zone(zone))
				continue;

			memmap_init_zone_range(zone, start_pfn, end_pfn,
					       &hole_pfn, mt);
			zone_id = j;
		}
	}

	/*
	 * Initialize the memory map for hole in the range [memory_end,
	 * section_end] for SPARSEMEM and in the range [memory_end, memmap_end]
	 * for FLATMEM.
	 * Append the pages in this hole to the highest zone in the last
	 * node.
	 */
	/*
	 * 初始化最后真实 memory_end 到 memmap 分配末端的空洞：
	 * SPARSEMEM 补齐当前 section，FLATMEM 补齐最大 buddy order 对齐范围；
	 * 这些 Reserved pages 归入最后 node 的最高已 populated zone。
	 */
#ifdef CONFIG_SPARSEMEM
	end_pfn = round_up(end_pfn, PAGES_PER_SECTION);
#else
	end_pfn = round_up(end_pfn, MAX_ORDER_NR_PAGES);
#endif
	if (hole_pfn < end_pfn)
		init_unavailable_range(hole_pfn, end_pfn, zone_id, nid);
}

#ifdef CONFIG_ZONE_DEVICE
/*
 * __init_zone_device_page() - 建立单个 ZONE_DEVICE 页的基础描述符。
 *
 * @page/@pfn：待初始化的 page 及其 PFN；@zone_idx/@nid：编码到 page
 * flags/links 的拓扑归属；@pgmap：设备内存映射，借用且必须覆盖该 PFN。
 *
 * 调用者是 memmap_init_zone_device() 及其复合页辅助函数。这里先复用普通
 * page 的基础初始化，再保持 Reserved、记录 pgmap，并按设备内存类型设置
 * 初始引用计数。它只建立可被后续上线路径识别的元数据，不把设备页
 * 加入普通 buddy，也不取得 pgmap 所有权。
 *
 * __ref 允许运行期内存热插拔代码调用含 __init 引用的初始化逻辑。调用者
 * 已隔离目标 vmemmap，无需页锁；本函数不睡眠、无返回值，非法组合应在
 * 上层拒绝。
 */
static void __ref __init_zone_device_page(struct page *page, unsigned long pfn,
					  unsigned long zone_idx, int nid,
					  struct dev_pagemap *pgmap)
{

	/* 先建立 flags、links、mapcount 等所有 page 类型共用的不变量。 */
	__init_single_page(page, pfn, zone_idx, nid);

	/*
	 * Mark page reserved as it will need to wait for onlining
	 * phase for it to be fully associated with a zone.
	 *
	 * We can use the non-atomic __set_bit operation for setting
	 * the flag as we are still initializing the pages.
	 */
	/*
	 * 页还必须等待 onlining 才完整归属 zone，因此先标 Reserved。目标页
	 * 尚未对并发分配者可见，可使用非原子的 __SetPageReserved()。
	 */
	__SetPageReserved(page);

	/*
	 * ZONE_DEVICE pages union ->lru with a ->pgmap back pointer
	 * and zone_device_data.  It is a bug if a ZONE_DEVICE page is
	 * ever freed or placed on a driver-private list.
	 */
	/*
	 * ZONE_DEVICE 复用 folio 的 lru union 保存 pgmap 反向指针，并把
	 * 驱动私有槽清空。该 union 不能再当普通 LRU/驱动链表使用；若这种
	 * page 被普通 free/LRU 路径接管，就是生命周期错误。
	 */
	page_folio(page)->pgmap = pgmap;
	page->zone_device_data = NULL;

	/*
	 * ZONE_DEVICE pages other than MEMORY_TYPE_GENERIC are released
	 * directly to the driver page allocator which will set the page count
	 * to 1 when allocating the page.
	 *
	 * MEMORY_TYPE_GENERIC and MEMORY_TYPE_FS_DAX pages automatically have
	 * their refcount reset to one whenever they are freed (ie. after
	 * their refcount drops to 0).
	 */
	/*
	 * 非 GENERIC 设备页由驱动分配器在分配时把引用从 0 置 1，故这里
	 * 明确从 0 开始。GENERIC 页保留基础初始化状态，其释放语义由通用
	 * devmap 路径管理。FS_DAX 虽会在释放时重置引用，初始仍属于 0 组。
	 */
	switch (pgmap->type) {
	case MEMORY_DEVICE_FS_DAX:
	case MEMORY_DEVICE_PRIVATE:
	case MEMORY_DEVICE_COHERENT:
	case MEMORY_DEVICE_PCI_P2PDMA:
		set_page_count(page, 0);
		break;

	case MEMORY_DEVICE_GENERIC:
		break;
	}
}

/*
 * With compound page geometry and when struct pages are stored in ram most
 * tail pages are reused. Consequently, the amount of unique struct pages to
 * initialize is a lot smaller that the total amount of struct pages being
 * mapped. This is a paired / mild layering violation with explicit knowledge
 * of how the sparse_vmemmap internals handle compound pages in the lack
 * of an altmap. See vmemmap_populate_compound_pages().
 */
/*
 * 使用复合页几何且 struct page 存于普通 RAM 时，sparse-vmemmap 会让
 * 多数 tail PFN 复用少量描述符；因此真正需要初始化的唯一 struct page
 * 数远小于设备 PFN 数。这里显式了解 vmemmap 内部布局，是与
 * vmemmap_populate_compound_pages() 成对的温和分层穿透。
 */
/*
 * compound_nr_pages() - 计算一个设备复合页需初始化的唯一描述符数。
 *
 * @pfn：复合页头 PFN；@altmap/@pgmap：vmemmap 存放策略与设备映射，
 * 均为借用。若早期 section 的未优化 memmap 被复用，或当前布局不能
 * 优化，返回复合页覆盖的完整 PFN 数；否则只返回优化布局保留的
 * VMEMMAP_RESERVE_NR 个页所能容纳的 struct page 数。
 *
 * 纯计算、无锁、无副作用；返回值描述的是描述符槽数，不是复合页的
 * 物理页数。
 */
static inline unsigned long compound_nr_pages(unsigned long pfn,
					      struct vmem_altmap *altmap,
					      struct dev_pagemap *pgmap)
{
	/*
	 * If DAX memory is hot-plugged into an unoccupied subsection
	 * of an early section, the unoptimized boot memmap is reused.
	 * See section_activate().
	 */
	/*
	 * DAX 若插入早期 section 的空 subsection，会复用启动时未优化的
	 * memmap；这种布局以及显式不能优化的布局都必须初始化全部
	 * 描述符。
	 */
	if (early_section(__pfn_to_section(pfn)) ||
	    !vmemmap_can_optimize(altmap, pgmap))
		return pgmap_vmemmap_nr(pgmap);

	/* 优化布局只触及为 head/tail 元数据预留并实际存在的描述符页。 */
	return VMEMMAP_RESERVE_NR * (PAGE_SIZE / sizeof(struct page));
}

/*
 * memmap_init_compound() - 把设备页组装为一个已初始化的复合页。
 *
 * @head/@head_pfn：头描述符与 PFN；@zone_idx/@nid/@pgmap：共同归属；
 * @nr_pages：本布局中必须逐一初始化的唯一描述符数。调用前头页已经由
 * __init_zone_device_page() 初始化，且 pgmap->vmemmap_shift 给出
 * compound order。
 *
 * 阶段是：先发布 head 标志；逐个初始化 tail 的设备属性和 compound
 * 链接并清零引用；最后由 prep_compound_head() 完成头部几何。目标范围
 * 尚未上线，无需普通复合页锁；无分配、无睡眠、无失败返回。
 */
static void __ref memmap_init_compound(struct page *head,
				       unsigned long head_pfn,
				       unsigned long zone_idx, int nid,
				       struct dev_pagemap *pgmap,
				       unsigned long nr_pages)
{
	unsigned long pfn, end_pfn = head_pfn + nr_pages;
	unsigned int order = pgmap->vmemmap_shift;

	/*
	 * We have to initialize the pages, including setting up page links.
	 * prep_compound_page() does not take care of that, so instead we
	 * open-code prep_compound_page() so we can take care of initializing
	 * the pages in the same go.
	 */
	/*
	 * prep_compound_page() 不负责基础 page links，因此不能直接使用。
	 * 这里展开其顺序，把每个 tail 的设备页初始化与 compound 链接建立
	 * 合并，确保任何已发布的 tail 都能追溯到有效 head。
	 */
	__SetPageHead(head);
	for (pfn = head_pfn + 1; pfn < end_pfn; pfn++) {
		struct page *page = pfn_to_page(pfn);

		/* 先建立设备页不变量，再让 tail 指向已经标记的 head。 */
		__init_zone_device_page(page, pfn, zone_idx, nid, pgmap);
		prep_compound_tail(page, head, order);
		set_page_count(page, 0);
	}
	/* tail 全部自洽后，补全 head 的 order/析构等复合页元数据。 */
	prep_compound_head(head, order);
}

/*
 * memmap_init_zone_device() - 完成一个 ZONE_DEVICE PFN 范围的页初始化。
 *
 * @zone：必须是 ZONE_DEVICE；@start_pfn/@nr_pages：设备半开区间；
 * @pgmap：描述设备类型、altmap 与 vmemmap compound 几何的借用映射。
 *
 * memmap_init_range() 已先处理 altmap 自托管 vmemmap 所占的页。本函数
 * 跳过该前缀，按每个设备 compound 的跨度初始化头页和实际存在的 tail
 * 描述符，最后把覆盖范围的 pageblock 标为 MIGRATE_MOVABLE。结果仍是
 * Reserved/待上线状态，不进入普通伙伴系统。
 *
 * 可由热插拔运行期调用，循环可 cond_resched()，所以调用上下文必须允许
 * 调度；调用者负责 hotplug 序列化和范围存活。参数错误 WARN 后无操作；
 * 无 errno、无局部分配，也无部分失败回滚路径。
 */
void __ref memmap_init_zone_device(struct zone *zone,
				   unsigned long start_pfn,
				   unsigned long nr_pages,
				   struct dev_pagemap *pgmap)
{
	unsigned long pfn, end_pfn = start_pfn + nr_pages;
	struct pglist_data *pgdat = zone->zone_pgdat;
	struct vmem_altmap *altmap = pgmap_altmap(pgmap);
	unsigned int pfns_per_compound = pgmap_vmemmap_nr(pgmap);
	unsigned long zone_idx = zone_idx(zone);
	unsigned long start = jiffies;
	int nid = pgdat->node_id;

	/* 防止普通 zone 或空 pgmap 进入设备页专用的 union/引用计数语义。 */
	if (WARN_ON_ONCE(!pgmap || zone_idx != ZONE_DEVICE))
		return;

	/*
	 * The call to memmap_init should have already taken care
	 * of the pages reserved for the memmap, so we can just jump to
	 * the end of that region and start processing the device pages.
	 */
	/*
	 * altmap 前缀已由 memmap_init_range() 初始化为承载 vmemmap 的
	 * Reserved 页；从当前 offset 后开始，避免把元数据存储误当设备页。
	 */
	if (altmap) {
		start_pfn = altmap->base_pfn + vmem_altmap_offset(altmap);
		nr_pages = end_pfn - start_pfn;
	}

	/*
	 * 每次跨过一个逻辑 compound 覆盖的设备 PFN；page 指向其 head。
	 * 描述符复用时，memmap_init_compound() 只访问实际唯一的 tail 槽。
	 */
	for (pfn = start_pfn; pfn < end_pfn; pfn += pfns_per_compound) {
		struct page *page = pfn_to_page(pfn);

		__init_zone_device_page(page, pfn, zone_idx, nid, pgmap);

		if (IS_ALIGNED(pfn, PAGES_PER_SECTION))
			cond_resched();

		/* order-0 设备页已完整，不需要构造 compound 元数据。 */
		if (pfns_per_compound == 1)
			continue;

		memmap_init_compound(page, pfn, zone_idx, nid, pgmap,
				     compound_nr_pages(pfn, altmap, pgmap));
	}

	/* 上线前统一建立 pageblock 可迁移属性；这里不改变 Reserved 状态。 */
	pageblock_migratetype_init_range(start_pfn, nr_pages, MIGRATE_MOVABLE);

	pr_debug("%s initialised %lu pages in %ums\n", __func__,
		nr_pages, jiffies_to_msecs(jiffies - start));
}
#endif

/*
 * The zone ranges provided by the architecture do not include ZONE_MOVABLE
 * because it is sized independent of architecture. Unlike the other zones,
 * the starting point for ZONE_MOVABLE is not fixed. It may be different
 * in each node depending on the size of each node and how evenly kernelcore
 * is distributed. This helper function adjusts the zone ranges
 * provided by the architecture for a given node by using the end of the
 * highest usable zone for ZONE_MOVABLE. This preserves the assumption that
 * zones within a node are in order of monotonic increases memory addresses
 */
/*
 * 架构给出的 zone 范围不含 ZONE_MOVABLE，因为它由 kernelcore/movablecore
 * 策略动态定容；各 node 的起点也可能不同。本辅助函数用该 node 的
 * zone_movable_pfn 切分架构 zone，使同一 node 内 zone 仍按物理地址
 * 单调排列。
 */
/*
 * adjust_zone_range_for_zone_movable() - 按可移动边界原地裁剪 zone span。
 *
 * @nid/@zone_type：目标 node/zone；@node_end_pfn：node 尾后边界；
 * @zone_start_pfn/@zone_end_pfn：输入为架构范围，输出为调整后的半开区间。
 *
 * ZONE_MOVABLE 从动态边界延伸到架构最高可用 zone 的结尾；普通 zone
 * 若跨越该边界则截断，若整体落在 movable 一侧则清空。mirror 模式通过
 * memblock region 属性区分，不按单一 PFN 截断普通 zone。早期单线程，
 * 无锁、无失败，指针参数是唯一副作用。
 */
static void __init adjust_zone_range_for_zone_movable(int nid,
					unsigned long zone_type,
					unsigned long node_end_pfn,
					unsigned long *zone_start_pfn,
					unsigned long *zone_end_pfn)
{
	/* Only adjust if ZONE_MOVABLE is on this node */
	/* 只有本 node 实际得到 movable 起点时才需要改架构边界。 */
	if (zone_movable_pfn[nid]) {
		/* Size ZONE_MOVABLE */
		/*
		 * ZONE_MOVABLE 使用动态起点，并受最高可用架构 zone
		 * 结尾限制。
		 */
		if (zone_type == ZONE_MOVABLE) {
			*zone_start_pfn = zone_movable_pfn[nid];
			*zone_end_pfn = min(node_end_pfn,
				arch_zone_highest_possible_pfn[movable_zone]);

		/* Adjust for ZONE_MOVABLE starting within this range */
		/* 非镜像模式下，普通 zone 跨越 movable 起点时在此截断。 */
		} else if (!mirrored_kernelcore &&
			*zone_start_pfn < zone_movable_pfn[nid] &&
			*zone_end_pfn > zone_movable_pfn[nid]) {
			*zone_end_pfn = zone_movable_pfn[nid];

		/* Check if this whole range is within ZONE_MOVABLE */
		/* 普通 zone 整体位于 movable 一侧时，用空半开区间表示无页。 */
		} else if (*zone_start_pfn >= zone_movable_pfn[nid])
			*zone_start_pfn = *zone_end_pfn;
	}
}

/*
 * Return the number of holes in a range on a node. If nid is MAX_NUMNODES,
 * then all holes in the requested range will be accounted for.
 */
/*
 * 返回指定 node 的 PFN 范围内不存在于 memblock.memory 的页数；
 * nid==MAX_NUMNODES 时跨所有 node 统计。
 */
/*
 * __absent_pages_in_range() - 用“总跨度减真实内存”计算物理空洞页数。
 *
 * @nid：node 过滤器或 MAX_NUMNODES；@range_start_pfn/@range_end_pfn：
 * 待统计半开区间。memblock range 可能跨边界，因此逐个 clamp 后从初始
 * 跨度扣除。调用时 memblock 拓扑稳定，函数只读、无锁、无失败。
 */
static unsigned long __init __absent_pages_in_range(int nid,
				unsigned long range_start_pfn,
				unsigned long range_end_pfn)
{
	/* nr_absent 从最坏情况“整段都是洞”开始，随后扣除真实 RAM。 */
	unsigned long nr_absent = range_end_pfn - range_start_pfn;
	unsigned long start_pfn, end_pfn;
	int i;

	for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, NULL) {
		/* 只扣除当前 memblock range 与请求范围的交集。 */
		start_pfn = clamp(start_pfn, range_start_pfn, range_end_pfn);
		end_pfn = clamp(end_pfn, range_start_pfn, range_end_pfn);
		nr_absent -= end_pfn - start_pfn;
	}
	return nr_absent;
}

/**
 * absent_pages_in_range - Return number of page frames in holes within a range
 * @start_pfn: The start PFN to start searching for holes
 * @end_pfn: The end PFN to stop searching for holes
 *
 * Return: the number of pages frames in memory holes within a range.
 */
/**
 * absent_pages_in_range - 统计跨所有 node 的 PFN 范围空洞
 * @start_pfn: 搜索起始 PFN（包含）
 * @end_pfn: 搜索结束 PFN（不包含）
 *
 * 这是 __absent_pages_in_range() 的导出启动期包装；返回不属于任何
 * memblock.memory range 的页框数。只读早期拓扑，无副作用。
 *
 * Return: 请求半开区间中物理内存空洞的页框数。
 */
unsigned long __init absent_pages_in_range(unsigned long start_pfn,
							unsigned long end_pfn)
{
	return __absent_pages_in_range(MAX_NUMNODES, start_pfn, end_pfn);
}

/* Return the number of page frames in holes in a zone on a node */
/* 返回 node 上给定 zone 范围内应视为“不在该 zone”的页框数。 */
/*
 * zone_absent_pages_in_node() - 计算 zone 的物理洞与策略排除页。
 *
 * 基础值来自当前 node 中不属于 memblock.memory 的 PFN。mirror
 * kernelcore 模式下，一个物理存在的 range 仍可能不属于当前逻辑 zone：
 * mirrored range 从 ZONE_MOVABLE 排除，非 mirrored range 从
 * ZONE_NORMAL 排除，因此把它们追加为 absent。
 *
 * 输入 zone 范围是半开区间；返回用于 present_pages=span-absent。
 * 启动期只读 memblock，无锁、无分配、无失败。
 */
static unsigned long __init zone_absent_pages_in_node(int nid,
					unsigned long zone_type,
					unsigned long zone_start_pfn,
					unsigned long zone_end_pfn)
{
	unsigned long nr_absent;

	/* zone is empty, we don't have any absent pages */
	/* 空 span 必须直接返回，避免后续策略统计制造虚假 absent。 */
	if (zone_start_pfn == zone_end_pfn)
		return 0;

	/* 第一层：扣除不属于该 node 的真实物理空洞。 */
	nr_absent = __absent_pages_in_range(nid, zone_start_pfn, zone_end_pfn);

	/*
	 * ZONE_MOVABLE handling.
	 * Treat pages to be ZONE_MOVABLE in ZONE_NORMAL as absent pages
	 * and vice versa.
	 */
	/*
	 * 镜像模式不是按单一 movable PFN 切割，而按每个 memblock region 的
	 * mirror 属性分区：对当前 zone 来说，属于另一策略类别的真实页等价
	 * 于逻辑空洞。
	 */
	if (mirrored_kernelcore && zone_movable_pfn[nid]) {
		unsigned long start_pfn, end_pfn;
		struct memblock_region *r;

		for_each_mem_region(r) {
			/* 仅累计 region 与当前 zone span 的交集。 */
			start_pfn = clamp(memblock_region_memory_base_pfn(r),
					  zone_start_pfn, zone_end_pfn);
			end_pfn = clamp(memblock_region_memory_end_pfn(r),
					zone_start_pfn, zone_end_pfn);

			if (zone_type == ZONE_MOVABLE &&
			    memblock_is_mirror(r))
				nr_absent += end_pfn - start_pfn;

			if (zone_type == ZONE_NORMAL &&
			    !memblock_is_mirror(r))
				nr_absent += end_pfn - start_pfn;
		}
	}

	return nr_absent;
}

/*
 * Return the number of pages a zone spans in a node, including holes
 * present_pages = zone_spanned_pages_in_node() - zone_absent_pages_in_node()
 */
/*
 * 返回 node 上 zone 从首 PFN 到尾 PFN 的跨度（包含洞）；present_pages
 * 稍后用 span 减 zone_absent_pages_in_node() 得到。
 */
/*
 * zone_spanned_pages_in_node() - 求架构 zone、node 与 movable 策略交集。
 *
 * @nid/@zone_type：目标；@node_start_pfn/@node_end_pfn：node 半开范围；
 * 输出参数返回最终 zone 半开边界，即使结果为空也供调用者记录。函数先
 * 以架构上下界 clamp，再应用动态 ZONE_MOVABLE 切分，最后收紧到 node。
 *
 * 返回跨度页数，包含物理洞；只修改两个输出参数，无锁、无失败。
 */
static unsigned long __init zone_spanned_pages_in_node(int nid,
					unsigned long zone_type,
					unsigned long node_start_pfn,
					unsigned long node_end_pfn,
					unsigned long *zone_start_pfn,
					unsigned long *zone_end_pfn)
{
	unsigned long zone_low = arch_zone_lowest_possible_pfn[zone_type];
	unsigned long zone_high = arch_zone_highest_possible_pfn[zone_type];

	/* Get the start and end of the zone */
	/* 阶段 1：架构 zone 与 node 粗范围相交。 */
	*zone_start_pfn = clamp(node_start_pfn, zone_low, zone_high);
	*zone_end_pfn = clamp(node_end_pfn, zone_low, zone_high);
	/* 阶段 2：叠加启动参数计算出的 movable 动态边界。 */
	adjust_zone_range_for_zone_movable(nid, zone_type, node_end_pfn,
					   zone_start_pfn, zone_end_pfn);

	/* Check that this node has pages within the zone's required range */
	/* 完全不相交时不再做减法，避免无符号边界产生下溢。 */
	if (*zone_end_pfn < node_start_pfn || *zone_start_pfn > node_end_pfn)
		return 0;

	/* Move the zone boundaries inside the node if necessary */
	/* 阶段 3：把最终边界严格限制在 node span 内。 */
	*zone_end_pfn = min(*zone_end_pfn, node_end_pfn);
	*zone_start_pfn = max(*zone_start_pfn, node_start_pfn);

	/* Return the spanned pages */
	/* 返回的是地址跨度，尚未扣除洞或镜像策略排除页。 */
	return *zone_end_pfn - *zone_start_pfn;
}

/*
 * reset_memoryless_node_totalpages() - 清空无内存 node 的 zone/pgdat 计数。
 *
 * @pgdat 的 node 可能因固件拓扑存在但不含 memblock.memory。函数清零每个
 * zone 的 span/present/start（以及热插拔的 early present），再清零 node
 * 汇总，避免后续 zonelist 把陈旧边界视为可分配内存。
 *
 * 仅在启动单线程统计阶段调用；不释放对象、不修改 node online 状态，
 * 无锁、无睡眠、无失败。
 */
static void __init reset_memoryless_node_totalpages(struct pglist_data *pgdat)
{
	struct zone *z;

	/* pgdat 内嵌固定 MAX_NR_ZONES 个 zone，逐一恢复空节点不变量。 */
	for (z = pgdat->node_zones; z < pgdat->node_zones + MAX_NR_ZONES; z++) {
		z->zone_start_pfn = 0;
		z->spanned_pages = 0;
		z->present_pages = 0;
#if defined(CONFIG_MEMORY_HOTPLUG)
		z->present_early_pages = 0;
#endif
	}

	pgdat->node_spanned_pages = 0;
	pgdat->node_present_pages = 0;
	pr_debug("On node %d totalpages: 0\n", pgdat->node_id);
}

/*
 * calc_nr_kernel_pages() - 汇总 memblock 当前可释放页及低端内核页。
 *
 * 遍历 for_each_free_mem_range() 给出的“memory 减 reserved”区间：
 * nr_all_pages 统计全部完整页；启用 HIGHMEM 时再把区间截到
 * ZONE_HIGHMEM 下界，令 nr_kernel_pages 只含内核可直接映射的低端页。
 * 未启用 HIGHMEM 时两者相同。结果供启动期内存统计与后续水位计算使用。
 *
 * memblock 拓扑已稳定、启动期单线程；无锁、无分配、无失败。
 */
static void __init calc_nr_kernel_pages(void)
{
	unsigned long start_pfn, end_pfn;
	phys_addr_t start_addr, end_addr;
	u64 u;
#ifdef CONFIG_HIGHMEM
	unsigned long high_zone_low = arch_zone_lowest_possible_pfn[ZONE_HIGHMEM];
#endif

	/*
	 * 只统计完全落入 free range 的页：PFN_UP 丢弃不完整首部，
	 * PFN_DOWN 丢弃不完整尾部，避免把保留字节当成整页。
	 */
	for_each_free_mem_range(u, NUMA_NO_NODE, MEMBLOCK_NONE, &start_addr, &end_addr, NULL) {
		start_pfn = PFN_UP(start_addr);
		end_pfn   = PFN_DOWN(end_addr);

		if (start_pfn < end_pfn) {
			/* 全部可释放页包含 HIGHMEM。 */
			nr_all_pages += end_pfn - start_pfn;
#ifdef CONFIG_HIGHMEM
			/*
			 * 内核页统计只保留 HIGHMEM 起点以下的交集；clamp
			 * 同时处理完全在高端或完全在低端的 range。
			 */
			start_pfn = clamp(start_pfn, 0, high_zone_low);
			end_pfn = clamp(end_pfn, 0, high_zone_low);
#endif
			nr_kernel_pages += end_pfn - start_pfn;
		}
	}
}

/*
 * calculate_node_totalpages() - 计算并发布一个 node 的 zone 容量。
 *
 * @pgdat：目标 node 描述符；@node_start_pfn/@node_end_pfn：node 的物理
 * 半开 span。对每个 zone，先求含洞跨度，再求物理洞及 movable/mirror
 * 策略排除页，得到 present_pages=spanned-absent；随后写回 zone 起点、
 * span、present 和 pgdat 汇总。
 *
 * present_pages 表示当前存在的物理页，并不等于已交给 buddy 的
 * managed_pages。MEMORY_HOTPLUG 的 present_early_pages 保存启动基线，
 * 供后续热插拔核算。启动期单线程、无锁、无分配、无错误返回。
 */
static void __init calculate_node_totalpages(struct pglist_data *pgdat,
						unsigned long node_start_pfn,
						unsigned long node_end_pfn)
{
	/* totalpages 含洞跨度；realtotalpages 只含本 node 实际归属页。 */
	unsigned long realtotalpages = 0, totalpages = 0;
	enum zone_type i;

	for (i = 0; i < MAX_NR_ZONES; i++) {
		/* zone 是 pgdat 内嵌对象，整个初始化过程不转移其所有权。 */
		struct zone *zone = pgdat->node_zones + i;
		unsigned long zone_start_pfn, zone_end_pfn;
		unsigned long spanned, absent;
		unsigned long real_size;

		spanned = zone_spanned_pages_in_node(pgdat->node_id, i,
						     node_start_pfn,
						     node_end_pfn,
						     &zone_start_pfn,
						     &zone_end_pfn);
		absent = zone_absent_pages_in_node(pgdat->node_id, i,
						   zone_start_pfn,
						   zone_end_pfn);

		real_size = spanned - absent;

		/* 空 zone 的起点规范化为 0，避免保留无意义的 clamp 结果。 */
		if (spanned)
			zone->zone_start_pfn = zone_start_pfn;
		else
			zone->zone_start_pfn = 0;
		zone->spanned_pages = spanned;
		zone->present_pages = real_size;
#if defined(CONFIG_MEMORY_HOTPLUG)
		zone->present_early_pages = real_size;
#endif

		totalpages += spanned;
		realtotalpages += real_size;
	}

	/* 所有 zone 完成后一次发布 node 汇总，保持上下层统计一致。 */
	pgdat->node_spanned_pages = totalpages;
	pgdat->node_present_pages = realtotalpages;
	pr_debug("On node %d totalpages: %lu\n", pgdat->node_id, realtotalpages);
}

#ifdef CONFIG_COMPACTION
/*
 * pgdat_init_kcompactd() - 初始化每 node 的后台内存规整等待队列。
 *
 * kcompactd 线程稍后创建；此处只建立 waitqueue head，使唤醒路径在任何
 * 线程启动前都有合法对象。pgdat 生命周期覆盖 node，早期/热插拔初始化
 * 均由上层串行化，无失败。
 */
static void pgdat_init_kcompactd(struct pglist_data *pgdat)
{
	init_waitqueue_head(&pgdat->kcompactd_wait);
}
#else
/*
 * 未启用 CONFIG_COMPACTION 时没有 kcompactd 等待者，保留空 helper 让
 * pgdat_init_internals() 的阶段顺序不产生配置散点。
 */
static void pgdat_init_kcompactd(struct pglist_data *pgdat) {}
#endif

/*
 * pgdat_init_internals() - 建立 pgdat 的运行期同步与回收基础设施。
 *
 * @pgdat 可以来自启动 node，也可来自内存热插拔，因此标为 __meminit。
 * 函数依次初始化 resize/kswapd/compaction 锁与等待队列、各类扫描节流
 * 队列、page_ext 和根 lruvec。调用后后台回收、规整及 LRU 路径可以安全
 * 引用这些对象，但线程本身尚不在这里创建。
 *
 * 调用者保证 pgdat 尚未并发可见或已受热插拔锁保护；函数不返回错误，
 * 不负责撤销，也不改变 zone 容量。
 */
static void __meminit pgdat_init_internals(struct pglist_data *pgdat)
{
	int i;

	/* 阶段 1：建立会保护 pgdat 调整和后台线程状态的锁。 */
	pgdat_resize_init(pgdat);
	pgdat_kswapd_lock_init(pgdat);
	pgdat_init_kcompactd(pgdat);

	/* 阶段 2：建立 kswapd、紧急分配者以及 vmscan 节流的等待点。 */
	init_waitqueue_head(&pgdat->kswapd_wait);
	init_waitqueue_head(&pgdat->pfmemalloc_wait);

	for (i = 0; i < NR_VMSCAN_THROTTLE; i++)
		init_waitqueue_head(&pgdat->reclaim_wait[i]);

	/* 阶段 3：绑定 node 级 page 扩展信息并初始化根 memcg 的 LRU 容器。 */
	pgdat_page_ext_init(pgdat);
	lruvec_init(&pgdat->__lruvec);
}

/*
 * zone_init_internals() - 建立单个 zone 的身份、计数器和同步原语。
 *
 * @zone/@idx/@nid：目标内嵌 zone 及其类型/node；@remaining_pages：
 * 初始 managed_pages，表示预计最终归伙伴系统管理的页数。
 *
 * 函数设置拓扑反向链接和可读名称，初始化 zone 主锁、span seqlock 与
 * per-CPU pageset 基础状态。它不建立 buddy free lists，也不把 page
 * 释放进去；这些由 init_currently_empty_zone() 和 memblock_free_all()
 * 分阶段完成。
 *
 * 初始化时对象不可并发访问；原子计数是为运行期读写语义准备，不表示
 * 此刻存在竞争。无分配、无失败。
 */
static void __meminit zone_init_internals(struct zone *zone, enum zone_type idx, int nid,
							unsigned long remaining_pages)
{
	/* 先发布容量和身份，再建立后续读写这些字段所需的锁/PCP。 */
	atomic_long_set(&zone->managed_pages, remaining_pages);
	zone_set_nid(zone, nid);
	zone->name = zone_names[idx];
	zone->zone_pgdat = NODE_DATA(nid);
	spin_lock_init(&zone->lock);
	zone_seqlock_init(zone);
	zone_pcp_init(zone);
}

/*
 * zone_init_free_lists() - 把 zone 的伙伴系统空闲结构初始化为空。
 *
 * @zone 尚未接收任何可分配页。函数初始化每个 order/migratetype 链表并
 * 清零各 order 的 nr_free；若支持未接受内存，再建立对应待处理链表。
 * 它只建立容器，不改变 managed/present 计数，无锁、无分配、无失败。
 */
static void __meminit zone_init_free_lists(struct zone *zone)
{
	struct list_head *list;
	unsigned int order;

	/* for_each_free_list 覆盖所有 order 与迁移类型对应的链表头。 */
	for_each_free_list(list, zone, order)
		INIT_LIST_HEAD(list);

	/* nr_free 按 buddy block 数计数，必须与空链表同步归零。 */
	for (order = 0; order < NR_PAGE_ORDERS; order++)
		zone->free_area[order].nr_free = 0;

#ifdef CONFIG_UNACCEPTED_MEMORY
	/* 固件尚未接受的页单独排队，不能提前进入普通 free_area。 */
	INIT_LIST_HEAD(&zone->unaccepted_pages);
#endif
}

/*
 * init_currently_empty_zone() - 激活一个此前没有 buddy 状态的 zone。
 *
 * @zone：已完成 zone_init_internals() 的目标；@zone_start_pfn/@size：
 * 新 span 的起点与页数。函数扩展 pgdat->nr_zones 高水位，记录起点，
 * 初始化空 free lists，最后置 initialized，作为运行期读者可用的发布点。
 *
 * 它不会初始化 struct page，也不会把页加入 free lists；调用者必须先后
 * 保证 zone 拓扑和页描述符生命周期。启动或受内存热插拔串行化上下文，
 * 无 errno；一旦发布后不能靠本函数回滚。
 */
void __meminit init_currently_empty_zone(struct zone *zone,
					unsigned long zone_start_pfn,
					unsigned long size)
{
	struct pglist_data *pgdat = zone->zone_pgdat;
	int zone_idx = zone_idx(zone) + 1;

	/* nr_zones 表示最高已初始化 zone 下标加一，而不是非空 zone 个数。 */
	if (zone_idx > pgdat->nr_zones)
		pgdat->nr_zones = zone_idx;

	/* 起点先于 initialized 发布，确保观察到激活的读者得到完整边界。 */
	zone->zone_start_pfn = zone_start_pfn;

	mminit_dprintk(MMINIT_TRACE, "memmap_init",
			"Initialising map node %d zone %lu pfns %lu -> %lu\n",
			pgdat->node_id,
			(unsigned long)zone_idx(zone),
			zone_start_pfn, (zone_start_pfn + size));

	zone_init_free_lists(zone);
	/*
	 * free-list 容器和边界都已就绪后，才允许后续路径视其为
	 * 初始化完成。
	 */
	zone->initialized = 1;
}

#ifndef CONFIG_SPARSEMEM
/*
 * Calculate the size of the zone->pageblock_flags rounded to an unsigned long
 * Start by making sure zonesize is a multiple of pageblock_order by rounding
 * up. Then use 1 NR_PAGEBLOCK_BITS worth of bits per pageblock, finally
 * round what is now in bits to nearest long in bits, then return it in
 * bytes.
 */
/*
 * 计算 zone->pageblock_flags 所需字节数并按 unsigned long 向上取整：
 * 先把 zone 起点所在 pageblock 的前缀纳入跨度，再按 pageblock 数乘
 * NR_PAGEBLOCK_BITS，最终将 bit 数对齐到 BITS_PER_LONG 后换算为字节。
 */
/*
 * usemap_size() - 计算非 SPARSEMEM zone 的 pageblock 位图容量。
 *
 * 输入 zone 起点与含洞跨度；返回能覆盖所有相交 pageblock 的字节数。
 * 纯算术、无副作用。把起点偏移纳入 zonesize 是为了不漏掉首个部分块。
 */
static unsigned long __init usemap_size(unsigned long zone_start_pfn, unsigned long zonesize)
{
	unsigned long usemapsize;

	/* 把首个 pageblock 在 zone 起点前的部分加入覆盖长度。 */
	zonesize += zone_start_pfn & (pageblock_nr_pages-1);
	/* 先得到完整 pageblock 数，再换算每块的 flags bit 数。 */
	usemapsize = round_up(zonesize, pageblock_nr_pages);
	usemapsize = usemapsize >> pageblock_order;
	usemapsize *= NR_PAGEBLOCK_BITS;
	usemapsize = round_up(usemapsize, BITS_PER_LONG);

	return usemapsize / BITS_PER_BYTE;
}

/*
 * setup_usemap() - 为非 SPARSEMEM zone 分配 pageblock_flags。
 *
 * @zone 的起点/span/node 已确定。位图从目标 node 的 memblock 分配，
 * SMP_CACHE_BYTES 对齐以减少共享缓存线问题；所有权随 zone 存续，启动
 * 后不释放。零大小保持 NULL；非零分配失败会 panic，因为缺少该位图时
 * buddy 的迁移类型/隔离状态无法正确工作，系统不能安全继续。
 *
 * __ref 允许热插拔初始化路径复用；调用者负责串行化。
 */
static void __ref setup_usemap(struct zone *zone)
{
	unsigned long usemapsize = usemap_size(zone->zone_start_pfn,
					       zone->spanned_pages);
	/* 先建立空值不变量，使零容量和失败诊断路径状态明确。 */
	zone->pageblock_flags = NULL;
	if (usemapsize) {
		zone->pageblock_flags =
			memblock_alloc_node(usemapsize, SMP_CACHE_BYTES,
					    zone_to_nid(zone));
		if (!zone->pageblock_flags)
			panic("Failed to allocate %ld bytes for zone %s pageblock flags on node %d\n",
			      usemapsize, zone->name, zone_to_nid(zone));
	}
}
#else
/*
 * SPARSEMEM 的 pageblock flags 由 sparse section 元数据承载，不需要
 * per-zone memblock 位图；空 stub 保持上层初始化流程统一。
 */
static inline void setup_usemap(struct zone *zone) {}
#endif /* CONFIG_SPARSEMEM */

#ifdef CONFIG_HUGETLB_PAGE_SIZE_VARIABLE

/* Initialise the number of pages represented by NR_PAGEBLOCK_BITS */
/* 初始化一组 NR_PAGEBLOCK_BITS 所表示的页数。 */
/*
 * set_pageblock_order() - 在可变 hugepage 配置下最终确定 pageblock 阶数。
 *
 * 无入参/返回值。若架构已经设置非零 pageblock_order 则保持幂等；否则
 * 从 PAGE_BLOCK_MAX_ORDER 开始，在有效 hugepage order 更小时收缩，
 * 使迁移/规整基本块不超过最大感兴趣连续分配粒度。结果在后续 usemap、
 * CMA、deferred 初始化前发布。
 *
 * 启动期单线程、纯全局参数写入，无分配、无失败。
 */
void __init set_pageblock_order(void)
{
	/* PAGE_BLOCK_MAX_ORDER 是 pageblock 允许采用的编译期上限。 */
	unsigned int order = PAGE_BLOCK_MAX_ORDER;

	/* Check that pageblock_nr_pages has not already been setup */
	/* 架构或更早路径已经选定时保持幂等，不覆盖其决定。 */
	if (pageblock_order)
		return;

	/* Don't let pageblocks exceed the maximum allocation granularity. */
	/*
	 * 若运行时 hugetlb order 更小，则收缩 pageblock：规整和迁移的基本
	 * 粒度不应大于系统关心的最大连续大页粒度。
	 */
	if (HPAGE_SHIFT > PAGE_SHIFT && HUGETLB_PAGE_ORDER < order)
		order = HUGETLB_PAGE_ORDER;

	/*
	 * Assume the largest contiguous order of interest is a huge page.
	 * This value may be variable depending on boot parameters on powerpc.
	 */
	/*
	 * 以最大感兴趣连续分配（huge page）为基准；powerpc 等架构可因
	 * 启动参数改变它，所以必须在运行时最终发布。
	 */
	pageblock_order = order;
}
#else /* CONFIG_HUGETLB_PAGE_SIZE_VARIABLE */

/*
 * When CONFIG_HUGETLB_PAGE_SIZE_VARIABLE is not set, set_pageblock_order()
 * is unused as pageblock_order is set at compile-time. See
 * include/linux/pageblock-flags.h for the values of pageblock_order based on
 * the kernel config
 */
/*
 * 未启用可变 hugepage 大小时，pageblock_order 已由
 * pageblock-flags.h 根据配置编译期确定；空实现仅提供统一调用接口。
 */
/*
 * 该配置分支无输入、输出或副作用；调用者仍可保持统一阶段顺序。
 */
void __init set_pageblock_order(void)
{
}

#endif /* CONFIG_HUGETLB_PAGE_SIZE_VARIABLE */

/*
 * Set up the zone data structures
 * - init pgdat internals
 * - init all zones belonging to this node
 *
 * NOTE: this function is only called during memory hotplug
 */
/*
 * 建立 node/zone 数据结构：先初始化 pgdat 内部设施，再重置属于该 node
 * 的所有 zone。注意：本入口只用于内存热插拔的新 node。
 */
#ifdef CONFIG_MEMORY_HOTPLUG
/*
 * free_area_init_core_hotplug() - 为热添加的 node 重建分配器核心状态。
 *
 * @pgdat 可能曾以 memoryless node 身份存在。函数重新初始化 pgdat 同步
 * 对象，必要时把共享 boot_nodestats 升级为 node 私有 percpu 存储，清零
 * node/kswapd 高水位及所有在线 CPU 的 node 统计，并以 0 present/managed
 * 重建每个 zone。
 *
 * 新内存此时全部 Offline；online_pages()/offline_pages() 才会维护实际
 * present_pages 和 managed_pages。调用者持内存热插拔写侧串行化，可
 * 睡眠（alloc_percpu）；alloc_percpu 失败会留下 NULL，并由后续上层
 * 失败处理约束，本函数自身无返回值或局部回滚。
 */
void __ref free_area_init_core_hotplug(struct pglist_data *pgdat)
{
	int nid = pgdat->node_id;
	enum zone_type z;
	int cpu;

	/* 先让所有锁、等待队列、lruvec 可供后续 online 路径使用。 */
	pgdat_init_internals(pgdat);

	/*
	 * boot_nodestats 是启动期共享占位；真正承载内存前改用本 node 的
	 * percpu 统计，避免多个 node 写同一份数据。
	 */
	if (pgdat->per_cpu_nodestats == &boot_nodestats)
		pgdat->per_cpu_nodestats = alloc_percpu(struct per_cpu_nodestat);

	/*
	 * Reset the nr_zones, order and highest_zoneidx before reuse.
	 * Note that kswapd will init kswapd_highest_zoneidx properly
	 * when it starts in the near future.
	 */
	/*
	 * 清除作为 memoryless/旧生命周期遗留的 zone 高水位和 kswapd 请求；
	 * kswapd 启动后会依据新 zone 正确重建 highest_zoneidx。
	 */
	pgdat->nr_zones = 0;
	pgdat->kswapd_order = 0;
	pgdat->kswapd_highest_zoneidx = 0;
	pgdat->node_start_pfn = 0;
	pgdat->node_present_pages = 0;

	/* percpu 区刚分配或被复用，在线 CPU 的全部 node 计数从零开始。 */
	for_each_online_cpu(cpu) {
		struct per_cpu_nodestat *p;

		p = per_cpu_ptr(pgdat->per_cpu_nodestats, cpu);
		memset(p, 0, sizeof(*p));
	}

	/*
	 * When memory is hot-added, all the memory is in offline state. So
	 * clear all zones' present_pages and managed_pages because they will
	 * be updated in online_pages() and offline_pages().
	 */
	/*
	 * hot-add 内存仍是 Offline，不能计入 present/managed；这里只建立
	 * zone 身份和锁，在线/离线路径随后按实际成功页数增减计数。
	 */
	for (z = 0; z < MAX_NR_ZONES; z++) {
		struct zone *zone = pgdat->node_zones + z;

		zone->present_pages = 0;
		zone_init_internals(zone, z, nid, 0);
	}
}
#endif

/*
 * free_area_init_core() - 为启动 node 初始化 pgdat 与所有非空 zone。
 *
 * @pgdat 已由 calculate_node_totalpages() 填好各 zone span/present。
 * 函数建立 node 内部设施，暂用 boot_nodestats，然后对每个 zone 初始化
 * managed 计数、身份与锁；非空 zone 还分配 pageblock 位图并创建空 buddy
 * free lists。
 *
 * 此时 page 尚未由 memblock_free_all() 放入 buddy。代码以
 * present_pages 作为将被管理的初始容量，后续释放/保留核算会校正实际
 * managed 状态。启动期串行；usemap 分配失败会 panic。
 */
static void __init free_area_init_core(struct pglist_data *pgdat)
{
	enum zone_type j;
	int nid = pgdat->node_id;

	pgdat_init_internals(pgdat);
	/*
	 * 启动早期先共享静态占位，运行期会建立/切换正确的 percpu 统计。
	 */
	pgdat->per_cpu_nodestats = &boot_nodestats;

	for (j = 0; j < MAX_NR_ZONES; j++) {
		struct zone *zone = pgdat->node_zones + j;
		unsigned long size = zone->spanned_pages;

		/*
		 * Initialize zone->managed_pages as 0 , it will be reset
		 * when memblock allocator frees pages into buddy system.
		 */
		/*
		 * 原注释描述逻辑阶段：真正可管理量以 memblock 释放结果
		 * 为准。
		 * 当前传入 present_pages 建立容量基线，不能据此认为页已空闲。
		 */
		zone_init_internals(zone, j, nid, zone->present_pages);

		/* 空 zone 保留身份/锁，但不分配位图，也不发布 initialized。 */
		if (!size)
			continue;

		/* pageblock 元数据必须先于空 buddy 容器发布。 */
		setup_usemap(zone);
		init_currently_empty_zone(zone, zone->zone_start_pfn, size);
	}
}

/*
 * memmap_alloc() - 从 memblock 为 struct page 等 memmap 元数据分配内存。
 *
 * @size/@align/@min_addr：物理容量、对齐和最低地址；@nid：首选/强制 node；
 * @exact_nid 为真时禁止跨 node 回退，否则尝试指定 node 后可回退。
 *
 * 使用 *_raw 接口避免自动清零，并带 NOLEAKTRACE：kmemleak 会遍历所有
 * 有效 struct page 显式扫描 mem_map，若再登记 backing memblock 会重复
 * 扫描。成功且非零时用 page_init_poison() 写初始化毒值，帮助发现过早
 * 使用；返回虚拟指针，失败返回 NULL，所有权交给永久 memmap。
 *
 * 仅用于 memblock 可用阶段，可能修改早期分配状态；调用者决定失败是
 * panic 还是降级。
 */
void __init *memmap_alloc(phys_addr_t size, phys_addr_t align,
			  phys_addr_t min_addr, int nid, bool exact_nid)
{
	void *ptr;

	/*
	 * Kmemleak will explicitly scan mem_map by traversing all valid
	 * `struct *page`,so memblock does not need to be added to the scan list.
	 */
	/*
	 * kmemleak 将通过有效 struct page 遍历 mem_map，因此该 backing
	 * memblock 不加入通用扫描表，避免重复追踪和伪引用。
	 */
	if (exact_nid)
		ptr = memblock_alloc_exact_nid_raw(size, align, min_addr,
						   MEMBLOCK_ALLOC_NOLEAKTRACE,
						   nid);
	else
		ptr = memblock_alloc_try_nid_raw(size, align, min_addr,
						 MEMBLOCK_ALLOC_NOLEAKTRACE,
						 nid);

	if (ptr && size > 0)
		page_init_poison(ptr, size);

	return ptr;
}

#ifdef CONFIG_FLATMEM
/*
 * alloc_node_mem_map() - 为 FLATMEM node 分配连续 struct page 数组。
 *
 * @pgdat 的 node span 已计算。伙伴系统可能在 node 边缘组成最大 order
 * 块，所以 backing 数组的两端向 MAX_ORDER_NR_PAGES 对齐；返回的
 * node_mem_map 再加 offset，使 node_start_pfn 能直接索引到对应 page。
 *
 * 分配来自优先本 node、允许回退的 memblock，失败不可恢复而 panic。
 * 元数据永久归内存模型；函数还累计其启动占用，并在 node 0 建立全局
 * mem_map/max_mapnr。启动期单线程、无锁。
 */
static void __init alloc_node_mem_map(struct pglist_data *pgdat)
{
	unsigned long start, offset, size, end;
	struct page *map;

	/* Skip empty nodes */
	/* memoryless node 不需要也不能建立空数组索引。 */
	if (!pgdat->node_spanned_pages)
		return;

	/* 头部向下对齐，为 node 起点前的 buddy 对齐槽保留 backing。 */
	start = pgdat->node_start_pfn & ~(MAX_ORDER_NR_PAGES - 1);
	offset = pgdat->node_start_pfn - start;
	/*
	 * The zone's endpoints aren't required to be MAX_PAGE_ORDER
	 * aligned but the node_mem_map endpoints must be in order
	 * for the buddy allocator to function correctly.
	 */
	/*
	 * zone 边界无需按最大 order 对齐，但 node_mem_map 必须覆盖对齐后的
	 * 尾部，否则 buddy 在边缘计算伙伴描述符时可能越过 backing。
	 */
	end = ALIGN(pgdat_end_pfn(pgdat), MAX_ORDER_NR_PAGES);
	size =  (end - start) * sizeof(struct page);
	map = memmap_alloc(size, SMP_CACHE_BYTES, MEMBLOCK_LOW_LIMIT,
			   pgdat->node_id, false);
	if (!map)
		panic("Failed to allocate %ld bytes for node %d memory map\n",
		      size, pgdat->node_id);
	pgdat->node_mem_map = map + offset;
	/* 将元数据占用计入启动页统计，而不是可分配物理页。 */
	memmap_boot_pages_add(DIV_ROUND_UP(size, PAGE_SIZE));
	pr_debug("%s: node %d, pgdat %08lx, node_mem_map %08lx\n",
		 __func__, pgdat->node_id, (unsigned long)pgdat,
		 (unsigned long)pgdat->node_mem_map);

	/* the global mem_map is just set as node 0's */
	/* FLATMEM 的全局 mem_map 以 node 0 数组为锚，非 node 0 到此属异常。 */
	WARN_ON(pgdat != NODE_DATA(0));

	mem_map = pgdat->node_mem_map;
	/*
	 * 若 page_to_pfn 的架构偏移关系要求数组从对齐 start 开始，则回退
	 * offset，使全局 mem_map[pfn] 的索引公式保持成立。
	 */
	if (page_to_pfn(mem_map) != pgdat->node_start_pfn)
		mem_map -= offset;

	max_mapnr = end - start;
}
#else
/* 非 FLATMEM 的 struct page backing 由 sparse/vmemmap 路径建立。 */
static inline void alloc_node_mem_map(struct pglist_data *pgdat) { }
#endif /* CONFIG_FLATMEM */

/**
 * get_pfn_range_for_nid - Return the start and end page frames for a node
 * @nid: The nid to return the range for. If MAX_NUMNODES, the min and max PFN are returned.
 * @start_pfn: Passed by reference. On return, it will have the node start_pfn.
 * @end_pfn: Passed by reference. On return, it will have the node end_pfn.
 *
 * It returns the start and end page frame of a node based on information
 * provided by memblock_set_node(). If called for a node
 * with no available memory, the start and end PFNs will be 0.
 */
/**
 * get_pfn_range_for_nid - 从 memblock 求 node 的最小包围 PFN 区间
 * @nid: 目标 node；MAX_NUMNODES 表示跨所有 node 求全局范围
 * @start_pfn: 输出最小起始 PFN
 * @end_pfn: 输出最大尾后 PFN
 *
 * 遍历 memblock_set_node() 建立的 range，求覆盖所有片段的包围区间；
 * 中间可包含洞。没有可用 range 时两个输出均为 0。函数只读启动拓扑，
 * 无分配、无失败，调用者不得把 span 误当 present 页数。
 */
void __init get_pfn_range_for_nid(unsigned int nid,
			unsigned long *start_pfn, unsigned long *end_pfn)
{
	unsigned long this_start_pfn, this_end_pfn;
	int i;

	/* -1UL 作为“尚未看到任何 range”的最小值哨兵。 */
	*start_pfn = -1UL;
	*end_pfn = 0;

	for_each_mem_pfn_range(i, nid, &this_start_pfn, &this_end_pfn, NULL) {
		*start_pfn = min(*start_pfn, this_start_pfn);
		*end_pfn = max(*end_pfn, this_end_pfn);
	}

	if (*start_pfn == -1UL)
		*start_pfn = 0;
}

/*
 * free_area_init_node() - 完成一个 node 从 memblock 拓扑到分配器容器的转换。
 *
 * @nid 对应的 NODE_DATA 必须已分配并清零。函数取得物理包围范围，发布
 * pgdat 身份；有内存时计算所有 zone 容量，无内存时清空计数。随后依次
 * 分配 FLATMEM map、记录 deferred-init 范围、建立 zone/buddy 空容器，
 * 最后初始化 multigenerational LRU 的 node 状态。
 *
 * 它不初始化每个 struct page，也不释放页到 buddy；这些由 memmap_init()
 * 和更晚的 memblock_free_all()/deferred 路径完成。启动期单线程；
 * memmap/usemap 的不可恢复分配失败会 panic。
 */
static void __init free_area_init_node(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	unsigned long start_pfn = 0;
	unsigned long end_pfn = 0;

	/* pg_data_t should be reset to zero when it's allocated */
	/* 非零 zone/kswapd 高水位说明 pgdat 被重复使用或未按约定清零。 */
	WARN_ON(pgdat->nr_zones || pgdat->kswapd_highest_zoneidx);

	/* 阶段 1：从 memblock 建立 node 物理包围区间和基本身份。 */
	get_pfn_range_for_nid(nid, &start_pfn, &end_pfn);

	pgdat->node_id = nid;
	pgdat->node_start_pfn = start_pfn;
	pgdat->per_cpu_nodestats = NULL;

	if (start_pfn != end_pfn) {
		pr_info("Initmem setup node %d [mem %#018Lx-%#018Lx]\n", nid,
			(u64)start_pfn << PAGE_SHIFT,
			end_pfn ? ((u64)end_pfn << PAGE_SHIFT) - 1 : 0);

		/* 阶段 2a：把 node span 按 zone 策略拆解并统计 present 页。 */
		calculate_node_totalpages(pgdat, start_pfn, end_pfn);
	} else {
		pr_info("Initmem setup node %d as memoryless\n", nid);

		/* 阶段 2b：保留拓扑 node，但确保所有内存容量为零。 */
		reset_memoryless_node_totalpages(pgdat);
	}

	/*
	 * 阶段 3：建立内存模型 backing，并记录可能延迟初始化的高端区间。
	 */
	alloc_node_mem_map(pgdat);
	pgdat_set_deferred_range(pgdat);

	/* 阶段 4：建立 pgdat/zone 同步与空 free lists，再建立 LRU generations。 */
	free_area_init_core(pgdat);
	lru_gen_init_pgdat(pgdat);
}

/* Any regular or high memory on that node? */
/* 检查该 node 是否拥有普通或高端内存，并设置派生 node state。 */
/*
 * check_for_memory() - 依据 populated zone 发布 node 的内存能力位。
 *
 * 只扫描 ZONE_MOVABLE 之前的架构 zone：任一 populated zone 都可令启用
 * HIGHMEM 的系统设置 N_HIGH_MEMORY；若该 zone 不高于 ZONE_NORMAL，
 * 还设置 N_NORMAL_MEMORY。找到首个即可停止，因为这里只求能力存在性。
 *
 * 调用时 zone 容量已发布，启动期单线程；只置位不清位，无失败。
 */
static void __init check_for_memory(pg_data_t *pgdat)
{
	enum zone_type zone_type;

	for (zone_type = 0; zone_type <= ZONE_MOVABLE - 1; zone_type++) {
		struct zone *zone = &pgdat->node_zones[zone_type];
		if (populated_zone(zone)) {
			/*
			 * HIGHMEM 配置下，任意常规/高端内存都使 node 具备
			 * 高内存能力。
			 */
			if (IS_ENABLED(CONFIG_HIGHMEM))
				node_set_state(pgdat->node_id, N_HIGH_MEMORY);
			if (zone_type <= ZONE_NORMAL)
				node_set_state(pgdat->node_id, N_NORMAL_MEMORY);
			break;
		}
	}
}

#if MAX_NUMNODES > 1
/*
 * Figure out the number of possible node ids.
 */
/* 由 possible nodemask 求需要遍历的 node id 上界。 */
/*
 * setup_nr_node_ids() - 将 nr_node_ids 收缩为最高 possible nid 加一。
 *
 * node_possible_map 已由架构/固件建立。运行期循环可据此避免扫描固定的
 * MAX_NUMNODES 尾部。possible map 至少应含 boot node；函数无失败。
 */
void __init setup_nr_node_ids(void)
{
	unsigned int highest;

	/* find_last_bit 返回最高置位下标；计数/尾后上界因此加一。 */
	highest = find_last_bit(node_possible_map.bits, MAX_NUMNODES);
	nr_node_ids = highest + 1;
}
#endif

/*
 * Some architectures, e.g. ARC may have ZONE_HIGHMEM below ZONE_NORMAL. For
 * such cases we allow max_zone_pfn sorted in the descending order
 */
/*
 * 某些架构（如无 PAE40 的 ARC）可让 ZONE_HIGHMEM 位于 ZONE_NORMAL
 * 以下，因此允许架构 max_zone_pfn 按降序出现。
 */
/*
 * arch_has_descending_max_zone_pfns() - 判断是否采用特殊降序 zone 边界。
 *
 * 编译期常量判断，无输入、无副作用；当前仅匹配 ARC 非 PAE40 配置。
 */
static bool arch_has_descending_max_zone_pfns(void)
{
	return IS_ENABLED(CONFIG_ARC) && !IS_ENABLED(CONFIG_ARC_HAS_PAE40);
}

/*
 * set_high_memory() - 发布内核线性映射可访问的最高虚拟地址（尾后）。
 *
 * 从 DRAM 物理末端开始；HIGHMEM 系统通常截到 ZONE_HIGHMEM 下界，特殊
 * 降序架构也强制采用该边界。最终用最后一个有效物理字节转换再加一，
 * 避免 phys_to_virt() 直接处理尾后地址的边界问题。
 *
 * 某些架构更早设置 high_memory 并在架构初始化中使用，本函数保持该值
 * 不变。启动期单线程、无失败；结果是地址边界，不拥有所指内存。
 */
static void __init set_high_memory(void)
{
	phys_addr_t highmem = memblock_end_of_DRAM();

	/*
	 * Some architectures (e.g. ARM) set high_memory very early and
	 * use it in arch setup code.
	 * If an architecture already set high_memory don't overwrite it
	 */
	/* 架构已发布时必须保持幂等，避免改变其早期映射约束。 */
	if (high_memory)
		return;

#ifdef CONFIG_HIGHMEM
	/* 线性映射不得越过高端内存起点。 */
	if (arch_has_descending_max_zone_pfns() ||
	    highmem > PFN_PHYS(arch_zone_lowest_possible_pfn[ZONE_HIGHMEM]))
		highmem = PFN_PHYS(arch_zone_lowest_possible_pfn[ZONE_HIGHMEM]);
#endif

	/* highmem 是物理尾后地址；转换最后一字节后再恢复尾后语义。 */
	high_memory = phys_to_virt(highmem - 1) + 1;
}

/**
 * free_area_init - Initialise all pg_data_t and zone data
 *
 * This will call free_area_init_node() for each active node in the system.
 * Using the page ranges provided by memblock_set_node(), the size of each
 * zone in each node and their holes is calculated. If the maximum PFN
 * between two adjacent zones match, it is assumed that the zone is empty.
 * For example, if arch_max_dma_pfn == arch_max_dma32_pfn, it is assumed
 * that arch_max_dma32_pfn has no pages. It is also assumed that a zone
 * starts where the previous one ended. For example, ZONE_DMA32 starts
 * at arch_max_dma_pfn.
 */
/*
 * free_area_init - 初始化所有 NUMA 节点的内存管理数据结构（buddy 骨架）
 *
 * 调用时机：hugetlb_bootmem_alloc() 之后，memblock 仍然可用，
 * buddy 尚未接管物理内存（free_area[] freelist 初始化为空）。
 *
 * 本函数完成六个阶段：
 *   1. 建立各 zone 的 PFN 边界（arch_zone_limits_init + SPARSEMEM 初始化）
 *   2. 确定 ZONE_MOVABLE 的 per-node 起始 PFN
 *   3. 打印 zone/节点/内存范围诊断信息到 dmesg
 *   4. 全局校验与参数设置（pageflags 布局、nr_node_ids、pageblock_order）
 *   5. 逐节点初始化 pg_data_t 和 zone 数据结构
 *   6. 收尾：vmemmap 延迟映射、struct page 初始化、high_memory 指针设置
 *
 * 完成后 buddy 的数据结构骨架就绪，但物理页尚未移交。
 * 真正的"移交"发生在后续 mm_core_init() 中的 memblock_free_all()。
 *
 * 入参/返回：均无。函数借用并读取架构 zone 上界、memblock ranges 与
 * node maps，永久填充 pgdat/zone/memmap；不取得物理内存所有权。
 * 运行于单线程 early boot，memblock 可分配而 slab/vmalloc 尚不可依赖。
 * 必需的 pgdat、FLATMEM mem_map 或 usemap 分配失败时由下层 panic，
 * 已发布的全局内存拓扑没有可继续启动的回滚方案。
 */
/*
PFN（Page Frame Number，页帧号），就是物理内存按页大小切分后，每一页的编号。

---
计算方式

PFN = 物理地址 / PAGE_SIZE

例（PAGE_SIZE = 4KB = 4096）：
  物理地址 0x00000000 → PFN 0
  物理地址 0x00001000 → PFN 1
  物理地址 0x40000000 → PFN 0x40000 = 262144

反过来：物理地址 = PFN << PAGE_SHIFT

---
为什么用 PFN 而不直接用物理地址

1. 节省空间：物理地址 64 位，PFN 可以少 12 位（页内偏移不需要存），struct page 数组用 PFN 作下标
2. 统一单位：内存管理操作都以页为单位，PFN 是天然的页级索引
3. 方便计算：zone 边界、内存范围都用 PFN 表示，比较大小只需整数运算
*/
static void __init free_area_init(void)
{
	unsigned long max_zone_pfn[MAX_NR_ZONES] = { 0 };
	unsigned long start_pfn, end_pfn;
	int i, nid, zone;
	bool descending;

	/* 阶段1a：调用架构相关函数填写各 zone 的最高 PFN 上界。
	 * 例如 x86：DMA 上界=16MiB，DMA32 上界=4GiB，NORMAL 上界=最高物理内存。
	 * max_zone_pfn[] 决定了每个 zone 的物理地址范围上限。 */
	arch_zone_limits_init(max_zone_pfn);

	/* 阶段1b：初始化 SPARSEMEM 内存模型。
	 * 分配 mem_section 数组，建立 section->node 映射，
	 * 为后续 struct page 的 section 编号标记做准备。 */
	sparse_init();

	/* DRAM 起始 PFN：跳过固件/ROM 占用的低地址空洞（如 x86 的 0~640K 区域）。
	 * 各 zone 的 lowest_possible_pfn 不能低于此值。 */
	start_pfn = PHYS_PFN(memblock_start_of_DRAM());

	/* ARC 架构在无 PAE40 时 zone 顺序递降，其余架构递增。
	 * descending=true 时从高 zone 向低 zone 遍历，确保 zone 边界连续覆盖。 */
	descending = arch_has_descending_max_zone_pfns();

	/* 计算每个 zone 的 [lowest_possible_pfn, highest_possible_pfn]。
	 * 含义：该 zone 理论上可能包含的 PFN 范围（实际含多少页由 memblock 决定）。
	 * ZONE_MOVABLE 跳过，由 find_zone_movable_pfns_for_nodes() 单独计算。
	 * start_pfn 在循环中滚动推进，确保各 zone 的范围首尾相连、无重叠。
	 * end_pfn = max(max_zone_pfn[zone], start_pfn) 防止 zone 上界低于 DRAM 起点。 */
	for (i = 0; i < MAX_NR_ZONES; i++) {
		if (descending)
			zone = MAX_NR_ZONES - i - 1;
		else
			zone = i;

		if (zone == ZONE_MOVABLE)
			continue;

		end_pfn = max(max_zone_pfn[zone], start_pfn);
		arch_zone_lowest_possible_pfn[zone] = start_pfn;
		arch_zone_highest_possible_pfn[zone] = end_pfn;

		start_pfn = end_pfn;
	}

	/* 阶段2：确定 ZONE_MOVABLE 在每个 NUMA 节点的起始 PFN。
	 * 根据命令行参数 kernelcore=/movablecore=/movable_node 计算，
	 * ZONE_MOVABLE 中的页只能用于可迁移分配，支持内存热拔出。 */
	memset(zone_movable_pfn, 0, sizeof(zone_movable_pfn));
	find_zone_movable_pfns_for_nodes();

	/* 阶段3a：打印各 zone 的物理地址范围到 dmesg，供调试和问题排查使用。 */
	pr_info("Zone ranges:\n");
	for (i = 0; i < MAX_NR_ZONES; i++) {
		if (i == ZONE_MOVABLE)
			continue;
		pr_info("  %-8s ", zone_names[i]);
		if (arch_zone_lowest_possible_pfn[i] ==
				arch_zone_highest_possible_pfn[i])
			pr_cont("empty\n");
		else
			pr_cont("[mem %#018Lx-%#018Lx]\n",
				(u64)arch_zone_lowest_possible_pfn[i]
					<< PAGE_SHIFT,
				((u64)arch_zone_highest_possible_pfn[i]
					<< PAGE_SHIFT) - 1);
	}

	/* 阶段3b：打印每个节点上 ZONE_MOVABLE 的起始物理地址。 */
	pr_info("Movable zone start for each node\n");
	for (i = 0; i < MAX_NUMNODES; i++) {
		if (zone_movable_pfn[i])
			pr_info("  Node %d: %#018Lx\n", i,
			       (u64)zone_movable_pfn[i] << PAGE_SHIFT);
	}

	/*
	 * Print out the early node map, and initialize the
	 * subsection-map relative to active online memory ranges to
	 * enable future "sub-section" extensions of the memory map.
	 *
	 * 阶段3c：打印 memblock 中所有物理内存范围（node/PFN），
	 * 同时初始化 SPARSEMEM subsection 映射（比 section 更细粒度，
	 * 支持以 2MiB 为单位的内存热插拔，而非整个 section）。
	 */
	pr_info("Early memory node ranges\n");
	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid) {
		pr_info("  node %3d: [mem %#018Lx-%#018Lx]\n", nid,
			(u64)start_pfn << PAGE_SHIFT,
			((u64)end_pfn << PAGE_SHIFT) - 1);
		sparse_init_subsection_map(start_pfn, end_pfn - start_pfn);
	}

	/* 阶段4：全局校验与参数设置。 */

	/* 调试模式下断言 page->flags 中各字段（zone/node/section/LRU 等）
	 * 的位域布局合法，防止编译配置错误导致字段越界覆盖。 */
	mminit_verify_pageflags_layout();

	/* 设置 nr_node_ids = 最高节点号 + 1，用于各处循环上界。 */
	setup_nr_node_ids();

	/* 设置 pageblock_order：buddy 迁移类型块（pageblock）的阶数。
	 * 若 HUGETLB_PAGE_SIZE_VARIABLE，取 HUGETLB_PAGE_ORDER，
	 * 确保每个 pageblock 至少能容纳一个大页，避免大页跨越迁移类型边界。 */
	/*
	 * 更精确地说，当前实现取 PAGE_BLOCK_MAX_ORDER 与运行时
	 * HUGETLB_PAGE_ORDER 的较小者，使 pageblock 不超过最大感兴趣
	 * 连续大页粒度。
	 */
	set_pageblock_order();

	/* 阶段5：逐节点初始化 pg_data_t 和 zone 数据结构。 */
	for_each_node(nid) {
		pg_data_t *pgdat;

		/*
		 * If an architecture has not allocated node data for
		 * this node, presume the node is memoryless or offline.
		 *
		 * 若架构未为此节点分配 pg_data_t（无内存节点或离线节点），
		 * 用 memblock 分配一个空的结构体占位，
		 * 避免后续代码对 NODE_DATA(nid) 的空指针访问。
		 * 该节点真正有内存时，由 hotadd_init_pgdat() 完整初始化。
		 */
		if (!NODE_DATA(nid))
			alloc_offline_node_data(nid);

		pgdat = NODE_DATA(nid);

		/* free_area_init_node() 完成该节点的完整初始化：
		 *   - 填写 pgdat->node_id/node_start_pfn/nr_zones
		 *   - calculate_node_totalpages()：计算各 zone 的 spanned/present 页数
		 *   - alloc_node_mem_map()：FLATMEM 模型下分配 struct page 数组
		 *   - pgdat_set_deferred_range()：标记大内存系统的延迟初始化 PFN 范围
		 *   - free_area_init_core()：初始化 zone 内部字段和迁移类型 usemap
		 *   - lru_gen_init_pgdat()：初始化 MGLRU（多代 LRU）相关字段 */
		free_area_init_node(nid);

		/*
		 * No sysfs hierarchy will be created via register_node()
		 *for memory-less node because here it's not marked as N_MEMORY
		 *and won't be set online later. The benefit is userspace
		 *program won't be confused by sysfs files/directories of
		 *memory-less node. The pgdat will get fully initialized by
		 *hotadd_init_pgdat() when memory is hotplugged into this node.
		 *
		 * 只有实际含物理页的节点才标记为 N_MEMORY。
		 * 无内存节点不标记，sysfs 中不创建其节点目录，
		 * 防止用户空间对空节点目录感到困惑。
		 */
		if (pgdat->node_present_pages) {
			node_set_state(nid, N_MEMORY);
			/* 检查该节点是否含 HIGHMEM 或 NORMAL zone，
			 * 相应设置 N_HIGH_MEMORY/N_NORMAL_MEMORY 节点状态位。 */
			check_for_memory(pgdat);
		}
	}

	/* 阶段6a：SPARSEMEM vmemmap 延迟映射。
	 * 对有内存的节点完成 vmemmap 页表映射（将 struct page 数组映射到
	 * 内核虚拟地址空间的 vmemmap 区域），之前 sparse_init() 已建立 section
	 * 级别的映射，此处补充 subsection 级别的映射。 */
	for_each_node_state(nid, N_MEMORY)
		sparse_vmemmap_init_nid_late(nid);

	/* 阶段6b：统计 nr_kernel_pages（低端直接映射页数）和 nr_all_pages（总页数）。 */
	calc_nr_kernel_pages();

	/* 阶段6c：逐区间初始化所有 struct page：
	 * 设置 page->flags 中的 zone/node/section 编号字段，
	 * 对物理空洞（hole）页面调用 init_unavailable_range() 标记为不可用。 */
	memmap_init();

	/* disable hash distribution for systems with a single node.
	 * 单 NUMA 节点系统关闭哈希分布优化（numa_hash_distance 无意义）。 */
	fixup_hashdist();

	/* 阶段6d：设置 high_memory 全局变量（内核线性映射区的上限虚拟地址）。
	 * 若架构在 setup_arch() 中已提前设置则跳过，否则根据 ZONE_HIGHMEM
	 * 的边界或最高物理内存地址计算并设置。 */
	set_high_memory();
}

/**
 * node_map_pfn_alignment - determine the maximum internode alignment
 *
 * This function should be called after node map is populated and sorted.
 * It calculates the maximum power of two alignment which can distinguish
 * all the nodes.
 *
 * For example, if all nodes are 1GiB and aligned to 1GiB, the return value
 * would indicate 1GiB alignment with (1 << (30 - PAGE_SHIFT)).  If the
 * nodes are shifted by 256MiB, 256MiB.  Note that if only the last node is
 * shifted, 1GiB is enough and this function will indicate so.
 *
 * This is used to test whether pfn -> nid mapping of the chosen memory
 * model has fine enough granularity to avoid incorrect mapping for the
 * populated node map.
 *
 * Return: the determined alignment in pfn's.  0 if there is no alignment
 * requirement (single node).
 */
/**
 * node_map_pfn_alignment - 求内存模型区分相邻 NUMA node 所需的最大粒度
 *
 * node map 必须已按 PFN 排序。函数检查每次 nid 变化的边界，求一个二次
 * 幂 PFN 对齐，使所选内存模型的 pfn->nid 映射粒度仍能区分所有实际
 * node。相邻 range 属于同一 nid 时，内部边界无需贡献约束。
 *
 * 例如 node 均按 1GiB 对齐时返回对应 PFN 数；若边界移到 256MiB，
 * 粒度必须相应变细。只读 memblock 拓扑，无锁、无副作用。
 *
 * Return: PFN 单位的二次幂对齐；单 node/无跨 node 边界时返回 0。
 */
unsigned long __init node_map_pfn_alignment(void)
{
	/*
	 * accl_mask 合并所有跨 node 边界的低位约束；last_end/last_nid
	 * 记录前一 range，供判断更粗粒度是否仍能分开两侧。
	 */
	unsigned long accl_mask = 0, last_end = 0;
	unsigned long start, end, mask;
	int last_nid = NUMA_NO_NODE;
	int i, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, &nid) {
		/*
		 * 首段、从 PFN 0 开始的段以及同 node 的相邻/离散段无需新增
		 * internode 对齐约束，只推进前一段状态。
		 */
		if (!start || last_nid < 0 || last_nid == nid) {
			last_nid = nid;
			last_end = end;
			continue;
		}

		/*
		 * Start with a mask granular enough to pin-point to the
		 * start pfn and tick off bits one-by-one until it becomes
		 * too coarse to separate the current node from the last.
		 */
		/*
		 * 先用 start 最低置位构造可精确落在当前起点的对齐 mask；
		 * 在上一段尾仍不越过当前粗粒度桶时逐步左移，直到再粗会把
		 * 两个 node 混入同一映射单元。
		 */
		mask = ~((1 << __ffs(start)) - 1);
		while (mask && last_end <= (start & (mask << 1)))
			mask <<= 1;

		/* accumulate all internode masks */
		/* OR 合并全部边界，保留其中最严格的低位区分要求。 */
		accl_mask |= mask;
	}

	/* convert mask to number of pages */
	/* 二进制补码把对齐 mask 转回其最低粒度对应的 PFN 数。 */
	return ~accl_mask + 1;
}

#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT
/*
 * deferred_free_pages() - 把刚完成描述符初始化的连续页交给 buddy。
 *
 * @pfn/@nr_pages：已初始化且属于 memblock free range 的区间；
 * @mt：该范围的启动迁移类型。最大 order 且自然对齐时整块释放，并逐
 * pageblock 建立类型；其他情况先接受固件未接受内存，再逐页 order-0
 * 释放，同时只在 pageblock 边界写迁移类型。
 *
 * 调用者保证所有相邻 hole/reserved 描述符已有效，因此 buddy 合并检查
 * 安全。函数会改变 page 引用/flags、zone free lists 和 managed/free
 * 统计；运行在 deferred 初始化串行片段中，所需锁由 __free_pages_core()
 * 内部处理。nr_pages==0 无操作，无显式失败返回。
 */
static void __init deferred_free_pages(unsigned long pfn,
		unsigned long nr_pages, enum migratetype mt)
{
	struct page *page;
	unsigned long i;

	if (!nr_pages)
		return;

	page = pfn_to_page(pfn);

	/* Free a large naturally-aligned chunk if possible */
	/*
	 * 完整、自然对齐的最大 buddy 块可一次释放，减少逐页锁和合并
	 * 开销；但其内每个 pageblock 仍需先获得正确迁移类型。
	 */
	if (nr_pages == MAX_ORDER_NR_PAGES && IS_MAX_ORDER_ALIGNED(pfn)) {
		for (i = 0; i < nr_pages; i += pageblock_nr_pages)
			init_pageblock_migratetype(page + i, mt, false);
		__free_pages_core(page, MAX_PAGE_ORDER, MEMINIT_EARLY);
		return;
	}

	/* Accept chunks smaller than MAX_PAGE_ORDER upfront */
	/*
	 * 小块无法依赖大块释放路径批量接受，先让固件/机密计算平台确认
	 * 整段可由内核访问，再触碰 page 并逐页交给 buddy。
	 */
	accept_memory(PFN_PHYS(pfn), nr_pages * PAGE_SIZE);

	for (i = 0; i < nr_pages; i++, page++, pfn++) {
		if (pageblock_aligned(pfn))
			init_pageblock_migratetype(page, mt, false);
		__free_pages_core(page, 0, MEMINIT_EARLY);
	}
}

/* Completion tracking for deferred_init_memmap() threads */
/*
 * pgdat_init_n_undone 记录尚未完成 deferred memmap 的 N_MEMORY node 数；
 * pgdat_init_all_done_comp 在最后一个 worker 报告完成时唤醒启动线程。
 * 两者均为 __initdata，page_alloc_init_late() 汇合后即可回收。
 */
static atomic_t pgdat_init_n_undone __initdata;
static __initdata DECLARE_COMPLETION(pgdat_init_all_done_comp);

/*
 * pgdat_init_report_one_done() - 原子报告一个 node worker 完成。
 *
 * 多个 kthread 可并发调用；atomic_dec_and_test() 令恰好最后一个完成者
 * 执行 complete()，不会丢失 page_alloc_init_late() 的等待唤醒。
 */
static inline void __init pgdat_init_report_one_done(void)
{
	if (atomic_dec_and_test(&pgdat_init_n_undone))
		complete(&pgdat_init_all_done_comp);
}

/*
 * Initialize struct pages.  We minimize pfn page lookups and scheduler checks
 * by performing it only once every MAX_ORDER_NR_PAGES.
 * Return number of pages initialized.
 */
/*
 * 批量初始化 struct page；上层按 MAX_ORDER_NR_PAGES 切块，从而摊薄
 * pfn_to_page 查找和调度检查。返回实际初始化的描述符数。
 */
/*
 * deferred_init_pages() - 为同一 zone 的连续 PFN 建立基础 page 描述符。
 *
 * @zone 决定 nid/zid；@pfn/@end_pfn 是已确认存在的半开区间。函数只做
 * __init_single_page()，尚不设置 pageblock 类型或释放到 buddy，这使
 * 元数据有效先于分配器可见。目标区间由 worker 独占，无锁、
 * 无失败。
 */
static unsigned long __init deferred_init_pages(struct zone *zone,
		unsigned long pfn, unsigned long end_pfn)
{
	int nid = zone_to_nid(zone);
	unsigned long nr_pages = end_pfn - pfn;
	int zid = zone_idx(zone);
	struct page *page = pfn_to_page(pfn);

	/* 连续递增 page 指针，避免每个 PFN 重复执行内存模型查找。 */
	for (; pfn < end_pfn; pfn++, page++)
		__init_single_page(page, pfn, zid, nid);
	return nr_pages;
}

/*
 * Initialize and free pages.
 *
 * At this point reserved pages and struct pages that correspond to holes in
 * memblock.memory are already initialized so every free range has a valid
 * memory map around it.
 * This ensures that access of pages that are ahead of the range being
 * initialized (computing buddy page in __free_one_page()) always reads a valid
 * struct page.
 *
 * In order to try and improve CPU cache locality we have the loop broken along
 * max page order boundaries.
 */
/*
 * 初始化并释放页。reserved 页以及 memblock.memory 洞对应的描述符已经
 * 提前有效，因此 __free_one_page() 向前查看潜在 buddy 时不会读取未
 * 初始化的 struct page。按最大 order 边界切块还改善 CPU cache 局部性。
 */
/*
 * deferred_init_memmap_chunk() - 处理 deferred PFN 窗口中的真实空闲页。
 *
 * @start_pfn/@end_pfn：任务窗口；@zone：唯一可能延迟的最高 zone；
 * @can_resched：后台线程为真，紧急同步 grow 路径为假。函数遍历该 node
 * 的 memblock free ranges，与窗口求交，并按最大 buddy 边界分块执行
 * “描述符初始化 -> pageblock 类型 -> 释放”。
 *
 * KHO scratch range 可覆盖默认 MIGRATE_MOVABLE 类型。可调度路径在块间
 * cond_resched()；不可调度路径触碰 NMI watchdog 防止长循环误报。返回
 * 实际初始化/释放页数；空洞和 reserved 页不计入，也没有回滚。
 */
static unsigned long __init
deferred_init_memmap_chunk(unsigned long start_pfn, unsigned long end_pfn,
			   struct zone *zone, bool can_resched)
{
	int nid = zone_to_nid(zone);
	unsigned long nr_pages = 0;
	phys_addr_t start, end;
	u64 i = 0;

	for_each_free_mem_range(i, nid, 0, &start, &end, NULL) {
		/*
		 * 只取完整物理页，并让 KHO 保留区决定必要的迁移类型覆盖。
		 */
		unsigned long spfn = PFN_UP(start);
		unsigned long epfn = PFN_DOWN(end);
		enum migratetype mt =
			kho_scratch_migratetype(spfn, MIGRATE_MOVABLE);

		if (spfn >= end_pfn)
			break;

		/* 把 memblock free range 裁到当前 worker/section 任务窗口。 */
		spfn = max(spfn, start_pfn);
		epfn = min(epfn, end_pfn);

		while (spfn < epfn) {
			/*
			 * ALIGN(spfn + 1) 保证游标已对齐时仍推进到下一个边界，
			 * 从而形成非空且不跨最大 order 边界的块。
			 */
			unsigned long mo_pfn = ALIGN(spfn + 1, MAX_ORDER_NR_PAGES);
			unsigned long chunk_end = min(mo_pfn, epfn);

			nr_pages += deferred_init_pages(zone, spfn, chunk_end);
			deferred_free_pages(spfn, chunk_end - spfn, mt);

			spfn = chunk_end;

			if (can_resched)
				cond_resched();
			else
				/* 持 pgdat resize 自旋锁的同步 grow 不能调度。 */
				touch_nmi_watchdog();
		}
	}

	return nr_pages;
}

/*
 * deferred_init_memmap_job() - padata 子任务适配器。
 *
 * padata 传入不重叠的 PFN 子区间，@arg 是共享只读 zone；worker 允许
 * 调度。返回值由框架忽略，完成同步由 padata_do_multithreaded() 负责。
 */
static void __init
deferred_init_memmap_job(unsigned long start_pfn, unsigned long end_pfn,
			 void *arg)
{
	struct zone *zone = arg;

	deferred_init_memmap_chunk(start_pfn, end_pfn, zone, true);
}

/*
 * deferred_page_init_max_threads() - 选择 node deferred 初始化并行上限。
 *
 * 每个 node 最多使用其 CPU mask 中的 CPU 数；memoryless CPU mask 也
 * 至少返回 1，保证任务可由通用执行上下文推进。纯计算、无副作用。
 */
static unsigned int __init
deferred_page_init_max_threads(const struct cpumask *node_cpumask)
{
	return max(cpumask_weight(node_cpumask), 1U);
}

/* Initialise remaining memory on a node */
/* 初始化一个 node 尚未建立 struct page 的剩余高端内存。 */
/*
 * deferred_init_memmap() - 每 node 的 deferred struct page 主线程。
 *
 * @data 是 pgdat。线程尽量绑定本 node CPU，在 pgdat resize lock 下取得
 * 并消费 first_deferred_pfn；ULONG_MAX 表示无需工作。解锁后 zone 不再
 * 允许 grow，随后用 padata 按 section 对齐并行初始化最高 zone。
 *
 * 所有退出路径都调用 pgdat_init_report_one_done()。边界破坏属于启动期
 * 内核不变量错误并 BUG；任务框架无 errno 传播，返回 0 表示 kthread
 * 正常结束。
 */
static int __init deferred_init_memmap(void *data)
{
	pg_data_t *pgdat = data;
	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);
	int max_threads = deferred_page_init_max_threads(cpumask);
	unsigned long first_init_pfn, last_pfn, flags;
	unsigned long start = jiffies;
	struct zone *zone;

	/* Bind memory initialisation thread to a local node if possible */
	/* NUMA 本地绑定改善写 struct page 和释放 free lists 时的缓存局部性。 */
	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(current, cpumask);

	pgdat_resize_lock(pgdat, &flags);
	first_init_pfn = pgdat->first_deferred_pfn;
	if (first_init_pfn == ULONG_MAX) {
		/* 可能已被同步 deferred_grow_zone() 提前全部完成。 */
		pgdat_resize_unlock(pgdat, &flags);
		pgdat_init_report_one_done();
		return 0;
	}

	/* Sanity check boundaries */
	/* deferred 起点必须位于当前 pgdat 的物理包围 span 内。 */
	BUG_ON(pgdat->first_deferred_pfn < pgdat->node_start_pfn);
	BUG_ON(pgdat->first_deferred_pfn > pgdat_end_pfn(pgdat));
	pgdat->first_deferred_pfn = ULONG_MAX;

	/*
	 * Once we unlock here, the zone cannot be grown anymore, thus if an
	 * interrupt thread must allocate this early in boot, zone must be
	 * pre-grown prior to start of deferred page initialization.
	 */
	/*
	 * 将标志置 ULONG_MAX 后解锁，相当于关闭按需 grow 所有权；此后若
	 * 中断早期需要内存，zone 必须已经在这里之前预增长。
	 */
	pgdat_resize_unlock(pgdat, &flags);

	/* Only the highest zone is deferred */
	/* defer_init() 只会截断 node 的最高 populated zone。 */
	zone = pgdat->node_zones + pgdat->nr_zones - 1;
	last_pfn = SECTION_ALIGN_UP(zone_end_pfn(zone));

	struct padata_mt_job job = {
		.thread_fn   = deferred_init_memmap_job,
		.fn_arg      = zone,
		.start       = first_init_pfn,
		.size        = last_pfn - first_init_pfn,
		.align       = PAGES_PER_SECTION,
		.min_chunk   = PAGES_PER_SECTION,
		.max_threads = max_threads,
		.numa_aware  = false,
	};

	padata_do_multithreaded(&job);

	/* Sanity check that the next zone really is unpopulated */
	/* 若后一个 zone populated，前述“仅最高 zone 延迟”假设已被破坏。 */
	WARN_ON(pgdat->nr_zones < MAX_NR_ZONES && populated_zone(++zone));

	pr_info("node %d deferred pages initialised in %ums\n",
		pgdat->node_id, jiffies_to_msecs(jiffies - start));

	pgdat_init_report_one_done();
	return 0;
}

/*
 * If this zone has deferred pages, try to grow it by initializing enough
 * deferred pages to satisfy the allocation specified by order, rounded up to
 * the nearest PAGES_PER_SECTION boundary.  So we're adding memory in increments
 * of SECTION_SIZE bytes by initializing struct pages in increments of
 * PAGES_PER_SECTION * sizeof(struct page) bytes.
 *
 * Return true when zone was grown, otherwise return false. We return true even
 * when we grow less than requested, to let the caller decide if there are
 * enough pages to satisfy the allocation.
 */
/*
 * 若 zone 尚有 deferred 页，按 section 粒度同步初始化至少满足 order 的
 * 数量。即便释放量少于请求仍返回 true，让调用者重新检查实际水位。
 */
/*
 * deferred_grow_zone() - 在后台 worker 接管前按需同步扩展最高 zone。
 *
 * @zone 必须是 pgdat 最后 zone；@order 是触发分配阶数。函数持
 * pgdat_resize_lock 防止多个申请者重复消费 first_deferred_pfn，并按
 * section 连续调用 deferred_init_memmap_chunk(..., false)。
 *
 * 锁是 irqsave 自旋锁，故内部不能调度；chunk 用 touch_nmi_watchdog()
 * 保活。返回 true 表示本次或竞争者推进过边界，不保证高阶分配一定
 * 成功，因为 memblock 保留区可使新页碎片化；false 表示无增长。
 */
bool __init deferred_grow_zone(struct zone *zone, unsigned int order)
{
	unsigned long nr_pages_needed = SECTION_ALIGN_UP(1 << order);
	pg_data_t *pgdat = zone->zone_pgdat;
	unsigned long first_deferred_pfn = pgdat->first_deferred_pfn;
	unsigned long spfn, epfn, flags;
	unsigned long nr_pages = 0;

	/* Only the last zone may have deferred pages */
	/* 非最高 zone 不可能是 defer_init() 截断的对象。 */
	if (zone_end_pfn(zone) != pgdat_end_pfn(pgdat))
		return false;

	pgdat_resize_lock(pgdat, &flags);

	/*
	 * If someone grew this zone while we were waiting for spinlock, return
	 * true, as there might be enough pages already.
	 */
	/*
	 * first_deferred_pfn 在加锁前快照；若锁等待期间已变化，另一个执行者
	 * 已增长或完成 zone，返回 true 促使调用者重新尝试分配。
	 */
	if (first_deferred_pfn != pgdat->first_deferred_pfn) {
		pgdat_resize_unlock(pgdat, &flags);
		return true;
	}

	/*
	 * Initialize at least nr_pages_needed in section chunks.
	 * If a section has less free memory than nr_pages_needed, the next
	 * section will be also initialized.
	 * Note, that it still does not guarantee that allocation of order can
	 * be satisfied if the sections are fragmented because of memblock
	 * allocations.
	 */
	/*
	 * 每次至少覆盖一个 section；某 section 中 free 页不足时继续下一
	 * section，直到累计目标或到 zone 末端。
	 */
	for (spfn = first_deferred_pfn, epfn = SECTION_ALIGN_UP(spfn + 1);
	     nr_pages < nr_pages_needed && spfn < zone_end_pfn(zone);
	     spfn = epfn, epfn += PAGES_PER_SECTION) {
		nr_pages += deferred_init_memmap_chunk(spfn, epfn, zone, false);
	}

	/*
	 * There were no pages to initialize and free which means the zone's
	 * memory map is completely initialized.
	 */
	/*
	 * 有释放页时发布下个未处理 section 起点；完全找不到 free 页说明
	 * 剩余仅为已初始化洞/保留区，可永久关闭按需初始化。
	 */
	pgdat->first_deferred_pfn = nr_pages ? spfn : ULONG_MAX;

	pgdat_resize_unlock(pgdat, &flags);

	return nr_pages > 0;
}

#endif /* CONFIG_DEFERRED_STRUCT_PAGE_INIT */

#ifdef CONFIG_CMA
/*
 * init_cma_reserved_pageblock() - 将启动期 Reserved pageblock 移交给 CMA。
 *
 * @page 必须是完整、对齐 pageblock 的首 page，范围此前为 CMA 预留且
 * 尚未分配。函数逐页清 Reserved 并把引用归零，设置 MIGRATE_CMA；随后
 * 临时令 head 引用有效并以 pageblock_order 整块释放到 buddy，最后把
 * 页数加入 managed_pages 与 zone->cma_pages。
 *
 * 关键顺序是不让半初始化块提前进入 buddy。clear_page_tag_ref() 清除
 * 启动保留标签，避免调试记账误认仍被引用。启动期无并发调用者；
 * __free_pages() 内部负责 zone 锁，无失败返回。
 */
void __init init_cma_reserved_pageblock(struct page *page)
{
	/* i/p 覆盖一个固定 pageblock；page 保留块首用于最终整块释放。 */
	unsigned i = pageblock_nr_pages;
	struct page *p = page;

	/* 所有页先从固件/启动保留态转成未引用态，暂不对 buddy 可见。 */
	do {
		__ClearPageReserved(p);
		set_page_count(p, 0);
	} while (++p, --i);

	/* 块级迁移类型必须在进入 free list 前发布。 */
	init_pageblock_migratetype(page, MIGRATE_CMA, false);
	/* __free_pages() 要求传入页具有一个有效调用者引用。 */
	set_page_refcounted(page);
	/* pages were reserved and not allocated */
	/* 这些页只是被预留而非普通分配，清除相应引用跟踪标签。 */
	clear_page_tag_ref(page);
	__free_pages(page, pageblock_order);

	/* 释放完成后再把该块纳入 zone 可管理容量和 CMA 专项统计。 */
	adjust_managed_page_count(page, pageblock_nr_pages);
	page_zone(page)->cma_pages += pageblock_nr_pages;
}
/*
 * Similar to above, but only set the migrate type and stats.
 */
/*
 * 与上面类似，但页已经由其他路径正确上线/释放，因此这里只设置
 * MIGRATE_CMA 并补 managed/CMA 统计，不触碰 Reserved、引用或 free list。
 */
/*
 * init_cma_pageblock() - 把已就绪 pageblock 纳入 CMA 分类和容量统计。
 *
 * 调用者保证整个块状态一致且不会并发迁移；无失败返回。
 */
void __init init_cma_pageblock(struct page *page)
{
	init_pageblock_migratetype(page, MIGRATE_CMA, false);
	adjust_managed_page_count(page, pageblock_nr_pages);
	page_zone(page)->cma_pages += pageblock_nr_pages;
}
#endif

/*
 * set_zone_contiguous() - 验证 zone 每个 pageblock 都无洞并发布连续标志。
 *
 * @zone 的 struct page 已全部初始化。函数按 pageblock 扫描 zone span，
 * __pageblock_pfn_to_page() 若发现区间没有完整有效的首尾 page，立即返回
 * 并保持 contiguous=false。全部通过后置 true。
 *
 * 这是一次保守的派生属性计算，可在块间 cond_resched()，调用上下文必须
 * 可调度；内存热插拔若改变 span，需要其串行化规则维护/重算该属性。
 */
void set_zone_contiguous(struct zone *zone)
{
	unsigned long block_start_pfn = zone->zone_start_pfn;
	unsigned long block_end_pfn;

	block_end_pfn = pageblock_end_pfn(block_start_pfn);
	for (; block_start_pfn < zone_end_pfn(zone);
			block_start_pfn = block_end_pfn,
			 block_end_pfn += pageblock_nr_pages) {

		/* 首尾块可小于完整 pageblock，先裁到 zone 尾。 */
		block_end_pfn = min(block_end_pfn, zone_end_pfn(zone));

		/* 任一块含洞便不能应用依赖 zone 物理连续性的快速路径。 */
		if (!__pageblock_pfn_to_page(block_start_pfn,
					     block_end_pfn, zone))
			return;
		cond_resched();
	}

	/* We confirm that there is no hole */
	/* 完整扫描成功后才发布，避免部分验证产生假阳性。 */
	zone->contiguous = true;
}

/*
 * Check if a PFN range intersects multiple zones on one or more
 * NUMA nodes. Specify the @nid argument if it is known that this
 * PFN range is on one node, NUMA_NO_NODE otherwise.
 */
/*
 * 检查 PFN 范围是否与两个或更多 zone 相交；已知位于单 node 时传 nid
 * 可跳过其他 node，否则传 NUMA_NO_NODE。
 */
/*
 * pfn_range_intersects_zones() - 判断区间是否跨越 zone/node 边界。
 *
 * @start_pfn/@nr_pages 描述范围；@nid 是可选 node 过滤器。遍历所有 zone，
 * 第一次相交只记录，第二次相交立即返回 true。零或一次相交返回 false。
 *
 * 函数只读稳定 zone span，不取得引用、不睡眠；调用者若处于热插拔并发
 * 环境，应已持有使 zone 边界稳定的锁。
 */
bool pfn_range_intersects_zones(int nid, unsigned long start_pfn,
			   unsigned long nr_pages)
{
	struct zone *zone, *izone = NULL;

	for_each_zone(zone) {
		/*
		 * 已知 node 时避免同 PFN 数值在其他 node zone 上造成
		 * 无关检查。
		 */
		if (nid != NUMA_NO_NODE && zone_to_nid(zone) != nid)
			continue;

		if (zone_intersects(zone, start_pfn, nr_pages)) {
			/* izone 非空表示此前已有另一个相交 zone。 */
			if (izone != NULL)
				return true;
			izone = zone;
		}

	}

	return false;
}

/* 前置声明：最终稳定内存统计的打印实现在文件后部。 */
static void __init mem_init_print_info(void);
/*
 * page_alloc_init_late() - 汇合 deferred 初始化并完成页分配器晚期收尾。
 *
 * mm_core_init() 已建立 buddy 并释放启动内存后调用。若启用 deferred
 * struct page，先为每个 N_MEMORY node 启动 worker 并等待全部完成，
 * 随后永久关闭按需初始化 static key、重算文件数上限。
 *
 * 接着打印稳定内存统计、初始化 buffer cache、丢弃 memblock 私有元数据，
 * 随机化空闲链表、探测 zone 连续性、必要时初始化 page_ext，最后注册
 * page allocator sysctl。
 *
 * 本函数可睡眠并创建/等待 kthread；完成后所有 struct page 均有效，
 * memblock 不再可供普通早期分配。worker 创建失败在当前接口没有显式
 * errno/回滚通道，因此依赖启动线程创建成功这一不变量。
 */
void __init page_alloc_init_late(void)
{
	struct zone *zone;
	int nid;

#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT

	/* There will be num_node_state(N_MEMORY) threads */
	/*
	 * 完成计数必须在线程启动前设置，防止快速 worker 提前减到错误值。
	 */
	atomic_set(&pgdat_init_n_undone, num_node_state(N_MEMORY));
	for_each_node_state(nid, N_MEMORY) {
		kthread_run(deferred_init_memmap, NODE_DATA(nid), "pgdatinit%d", nid);
	}

	/* Block until all are initialised */
	/* 每个 worker 的所有 struct page/free-list 写入都在 completion 前完成。 */
	wait_for_completion(&pgdat_init_all_done_comp);

	/*
	 * We initialized the rest of the deferred pages.  Permanently disable
	 * on-demand struct page initialization.
	 */
	/*
	 * 全部 node 已汇合，关闭 static key 后分配热路径不再检查/触发
	 * deferred_grow_zone()。
	 */
	static_branch_disable(&deferred_pages);

	/* Reinit limits that are based on free pages after the kernel is up */
	/* 现在 free 页总量稳定，可据此重算系统文件句柄上限。 */
	files_maxfiles_init();
#endif

	/* Accounting of total+free memory is stable at this point. */
	/* 统计与 buffer cache 初始化必须看到 deferred 页全部释放后的容量。 */
	mem_init_print_info();
	buffer_init();

	/* Discard memblock private memory */
	/* 从此不再依赖 memblock 的私有数组，可回收其 backing。 */
	memblock_discard();

	/* 在每 node 内随机化伙伴 free lists，降低物理分配模式可预测性。 */
	for_each_node_state(nid, N_MEMORY)
		shuffle_free_memory(NODE_DATA(nid));

	for_each_populated_zone(zone)
		set_zone_contiguous(zone);

	/* Initialize page ext after all struct pages are initialized. */
	/* deferred 模式下 page_ext 不能早于其对应 struct page 全部存在。 */
	if (deferred_struct_pages)
		page_ext_init();

	page_alloc_sysctl_init();
}

/*
 * Adaptive scale is meant to reduce sizes of hash tables on large memory
 * machines. As memory size is increased the scale is also increased but at
 * slower pace.  Starting from ADAPT_SCALE_BASE (64G), every time memory
 * quadruples the scale is increased by one, which means the size of hash table
 * only doubles, instead of quadrupling as well.
 * Because 32-bit systems cannot have large physical memory, where this scaling
 * makes sense, it is disabled on such platforms.
 */
/*
 * 自适应 scale 用于抑制大内存机器上启动哈希表的增长速度：从 64GiB
 * 开始，内存每扩大 4 倍只把 scale 增加 1，因此 bucket 数仅扩大 2 倍，
 * 而不是随内存扩大 4 倍。32 位地址空间不适用这种超大内存策略。
 */
#if __BITS_PER_LONG > 32
/* 自适应缩放起点、每轮内存倍率的位移以及对应 PFN 数。 */
#define ADAPT_SCALE_BASE	(64ul << 30)
#define ADAPT_SCALE_SHIFT	2
#define ADAPT_SCALE_NPAGES	(ADAPT_SCALE_BASE >> PAGE_SHIFT)
#endif

/*
 * allocate a large system hash table from bootmem
 * - it is assumed that the hash table must contain an exact power-of-2
 *   quantity of entries
 * - limit is the number of hash buckets, not the total allocation size
 */
/*
 * 从启动期内存分配一个大型系统哈希表。bucket 数必须为 2 的幂；
 * low/high_limit 的单位都是 bucket 数，而不是字节。
 */
/*
 * alloc_large_system_hash() - 估算、限幅并分配启动期大型哈希表。
 *
 * @tablename：诊断名称；@bucketsize：单 bucket 字节数；@numentries：
 * 非零时是命令行/调用者指定数量，零时按 nr_kernel_pages 与 @scale
 * 自动估算；@flags：HASH_EARLY 选择 memblock，HASH_ZERO 要求清零；
 * @_hash_shift/@_hash_mask：可选输出，供调用者执行幂次哈希索引；
 * @low_limit/@high_limit：bucket 数下/上界，high=0 使用默认内存比例。
 *
 * 分配策略：
 *   1. 自动估算时按低端内存和自适应 scale 得到 bucket 数；
 *   2. 向 2 次幂取整并限制在下界及“最多总内存 1/16”等上界内；
 *   3. early 用 memblock；晚些时候超大或 hashdist 用 vmalloc_huge，
 *      否则用物理连续 alloc_pages_exact；
 *   4. 失败时不断把 log2qty 减一，最小尝试到一页。
 *
 * 成功返回表并把永久所有权交给调用子系统；不可恢复失败 panic。非 early
 * 分支用 GFP_ATOMIC，不依赖可睡眠回收；输出 shift/mask 只在成功后写入。
 */
void *__init alloc_large_system_hash(const char *tablename,
				     unsigned long bucketsize,
				     unsigned long numentries,
				     int scale,
				     int flags,
				     unsigned int *_hash_shift,
				     unsigned int *_hash_mask,
				     unsigned long low_limit,
				     unsigned long high_limit)
{
	unsigned long long max = high_limit;
	unsigned long log2qty, size;
	void *table;
	gfp_t gfp_flags;
	bool virt;
	bool huge;

	/* allow the kernel cmdline to have a say */
	/* 非零 numentries 表示上层/命令行已决定容量，跳过内存比例估算。 */
	if (!numentries) {
		/* round applicable memory size up to nearest megabyte */
		/*
		 * 先以低端内核页数为基数；小页系统向 1MiB 页数倍数取整，
		 * 减少很小内存差异造成的表大小抖动。
		 */
		numentries = nr_kernel_pages;

		/* It isn't necessary when PAGE_SIZE >= 1MB */
		/* PAGE_SIZE 已达到 1MiB 时，一个 page 本身就是所需取整粒度。 */
		if (PAGE_SIZE < SZ_1M)
			numentries = round_up(numentries, SZ_1M / PAGE_SIZE);

#if __BITS_PER_LONG > 32
		if (!high_limit) {
			unsigned long adapt;

			/*
			 * 只有未显式给上界时应用自适应收缩；每跨过 4 倍内存
			 * 增加一个 scale bit，使 bucket 增长更温和。
			 */
			for (adapt = ADAPT_SCALE_NPAGES; adapt < numentries;
			     adapt <<= ADAPT_SCALE_SHIFT)
				scale++;
		}
#endif

		/* limit to 1 bucket per 2^scale bytes of low memory */
		/*
		 * numentries 当前单位是 page；结合 PAGE_SHIFT 转成“每
		 * 2^scale 字节一个 bucket”的目标数量。
		 */
		if (scale > PAGE_SHIFT)
			numentries >>= (scale - PAGE_SHIFT);
		else
			numentries <<= (PAGE_SHIFT - scale);

		if (unlikely((numentries * bucketsize) < PAGE_SIZE))
			/* 自动表至少占一页，避免退化成极小分配。 */
			numentries = PAGE_SIZE / bucketsize;
	}
	/* shift/mask 索引要求 bucket 数严格为 2 的幂。 */
	numentries = roundup_pow_of_two(numentries);

	/* limit allocation size to 1/16 total memory by default */
	/*
	 * 未给 high_limit 时，用全部页物理字节的 1/16 换算最大 bucket 数；
	 * 这是容量预算，不代表最终分配必然占满。
	 */
	if (max == 0) {
		max = ((unsigned long long)nr_all_pages << PAGE_SHIFT) >> 4;
		max = div64_ul(max, bucketsize);
	}
	/* 再把 bucket 上限压到 2^31，约束后续 shift/mask 与平台算术。 */
	max = min(max, 0x80000000ULL);

	/*
	 * 先应用调用者下界，再应用最终上界；上界对冲突配置具有
	 * 优先权。
	 */
	if (numentries < low_limit)
		numentries = low_limit;
	if (numentries > max)
		numentries = max;

	/* log2qty 是当前尝试的 bucket 阶数，失败时逐级减半。 */
	log2qty = ilog2(numentries);

	/* 该启动分配不进入常规睡眠回收；HASH_ZERO 追加清零语义。 */
	gfp_flags = (flags & HASH_ZERO) ? GFP_ATOMIC | __GFP_ZERO : GFP_ATOMIC;
	do {
		virt = false;
		size = bucketsize << log2qty;
		if (flags & HASH_EARLY) {
			/* buddy/slab 尚不可用时只能从 memblock 永久分配。 */
			if (flags & HASH_ZERO)
				table = memblock_alloc(size, SMP_CACHE_BYTES);
			else
				table = memblock_alloc_raw(size,
							   SMP_CACHE_BYTES);
		} else if (get_order(size) > MAX_PAGE_ORDER || hashdist) {
			/*
			 * 物理连续 order 过大或要求 NUMA 分布时改用 vmalloc；
			 * vmalloc_huge 会在可能时用大页降低页表/TLB 开销。
			 */
			table = vmalloc_huge(size, gfp_flags);
			virt = true;
			if (table)
				huge = is_vm_area_hugepages(table);
		} else {
			/*
			 * If bucketsize is not a power-of-two, we may free
			 * some pages at the end of hash table which
			 * alloc_pages_exact() automatically does
			 */
			/*
			 * bucket 非 2 次幂字节时，总 size 不一定是整页/2 次幂；
			 * alloc_pages_exact() 会自动释放末尾多分配的页。
			 */
			table = alloc_pages_exact(size, gfp_flags);
			/* alloc_pages_exact 不自动登记，显式交给 kmemleak 跟踪。 */
			kmemleak_alloc(table, size, 1, gfp_flags);
		}
		/* 非最小页级尝试失败时把 bucket 数减半并重试。 */
	} while (!table && size > PAGE_SIZE && --log2qty);

	if (!table)
		panic("Failed to allocate %s hash table\n", tablename);

	pr_info("%s hash table entries: %ld (order: %d, %lu bytes, %s)\n",
		tablename, 1UL << log2qty, get_order(size), size,
		virt ? (huge ? "vmalloc hugepage" : "vmalloc") : "linear");

	if (_hash_shift)
		/* 调用者可用 hash >> shift/掩码组合构造快速 bucket 索引。 */
		*_hash_shift = log2qty;
	if (_hash_mask)
		*_hash_mask = (1 << log2qty) - 1;

	return table;
}

/*
 * memblock_free_pages() - 把 memblock 释放出的一个 buddy 块交给页分配器。
 *
 * @pfn/@order 指定此前 Reserved、并非普通分配得到的连续块。deferred
 * 模式下，如果该 PFN 的 struct page 尚未初始化就直接返回：后续 deferred
 * worker 会在遍历 memblock free ranges 时完成初始化与释放，避免触碰
 * 无效描述符或重复释放。
 *
 * KMSAN 可接管该块以建立 shadow/origin，返回 false 时由 KMSAN 后续
 * 释放；否则清除“启动保留而非分配”引用标签，并以 MEMINIT_EARLY 语义
 * 进入 buddy。函数改变 free/managed 状态，无 errno；内部处理 zone 锁。
 */
void __init memblock_free_pages(unsigned long pfn, unsigned int order)
{
	struct page *page = pfn_to_page(pfn);

	if (IS_ENABLED(CONFIG_DEFERRED_STRUCT_PAGE_INIT)) {
		int nid = early_pfn_to_nid(pfn);

		/* 尚无有效 struct page 的块留给 deferred worker，不能现在释放。 */
		if (!early_page_initialised(pfn, nid))
			return;
	}

	if (!kmsan_memblock_free_pages(page, order)) {
		/* KMSAN will take care of these pages. */
		/*
		 * KMSAN 已取得初始化责任，本路径不得再次把同一块加入 buddy。
		 */
		return;
	}

	/* pages were reserved and not allocated */
	/*
	 * 清除启动保留标签后，使用不要求普通分配者引用的 core
	 * 释放接口。
	 */
	clear_page_tag_ref(page);
	__free_pages_core(page, order, MEMINIT_EARLY);
}

/*
 * init_on_alloc/init_on_free 是运行期热路径 static key：配置默认值决定
 * 初始分支方向，早期参数解析后由 mem_debugging_and_hardening_init()
 * 最终启停。导出符号允许分配器外的内核代码共享同一策略判断。
 */
DEFINE_STATIC_KEY_MAYBE(CONFIG_INIT_ON_ALLOC_DEFAULT_ON, init_on_alloc);
EXPORT_SYMBOL(init_on_alloc);

DEFINE_STATIC_KEY_MAYBE(CONFIG_INIT_ON_FREE_DEFAULT_ON, init_on_free);
EXPORT_SYMBOL(init_on_free);

/*
 * 早期布尔值收集命令行最终意图；__read_mostly 减少运行期误共享。
 * 在所有互斥策略解析完成前，不能直接据此切 static key。
 */
static bool _init_on_alloc_enabled_early __read_mostly
				= IS_ENABLED(CONFIG_INIT_ON_ALLOC_DEFAULT_ON);
/*
 * early_init_on_alloc() - 解析 init_on_alloc=<bool>。
 *
 * kstrtobool() 返回 0 或负 errno，early_param 框架负责诊断无效值。
 */
static int __init early_init_on_alloc(char *buf)
{

	return kstrtobool(buf, &_init_on_alloc_enabled_early);
}
early_param("init_on_alloc", early_init_on_alloc);

/* init_on_free 的早期请求状态及对应命令行解析器，语义与 alloc 成对。 */
static bool _init_on_free_enabled_early __read_mostly
				= IS_ENABLED(CONFIG_INIT_ON_FREE_DEFAULT_ON);
/*
 * early_init_on_free() - 解析 init_on_free=<bool> 到早期请求状态。
 *
 * 输入字符串由 early_param 借用；返回 0 或 kstrtobool() 的负 errno，
 * 不在这里切换 static key。
 */
static int __init early_init_on_free(char *buf)
{
	return kstrtobool(buf, &_init_on_free_enabled_early);
}
early_param("init_on_free", early_init_on_free);

/*
 * check_pages_enabled 控制分配/释放时的 struct page 健全性检查；DEBUG_VM
 * 配置默认打开，否则可由任一调试/硬化策略在启动时开启。
 */
DEFINE_STATIC_KEY_MAYBE(CONFIG_DEBUG_VM, check_pages_enabled);

/* check_pages=<bool> 的早期请求仅在 __init 阶段保存。 */
static bool check_pages_enabled_early __initdata;

/*
 * early_check_pages() - 解析 check_pages=<bool>。
 *
 * 输入字符串由 early_param 借用；成功写 check_pages_enabled_early，
 * 返回 0，非法布尔值返回负 errno。最终 static key 在汇总阶段发布。
 */
static int __init early_check_pages(char *buf)
{
	return kstrtobool(buf, &check_pages_enabled_early);
}
early_param("check_pages", early_check_pages);

/*
 * Enable static keys related to various memory debugging and hardening options.
 * Some override others, and depend on early params that are evaluated in the
 * order of appearance. So we need to first gather the full picture of what was
 * enabled, and then make decisions.
 */
/*
 * 启用内存调试/硬化 static key。部分功能互相覆盖，且 early params 按
 * 命令行出现顺序解析，因此必须先收集完整意图，再统一决定最终组合。
 */
/*
 * mem_debugging_and_hardening_init() - 解析优先级并发布页调试热路径策略。
 *
 * PAGE_POISONING 优先于 init_on_alloc/init_on_free，避免重复填充与冲突
 * 检查；任一实际调试/清零策略都会要求 check_pages。DEBUG_PAGEALLOC
 * 还可启用 guardpage。最后仅在 CONFIG_DEBUG_VM 未天然启用时按需求打开
 * check_pages static key。
 *
 * 仅在 SMP/分配热路径开始前调用，无需并发锁；static_branch_enable()
 * 会修补跳转标签。函数无失败返回，冲突通过禁用较低优先级选项并
 * 打印。
 */
static void __init mem_debugging_and_hardening_init(void)
{
	bool page_poisoning_requested = false;
	bool want_check_pages = check_pages_enabled_early;

#ifdef CONFIG_PAGE_POISONING
	/*
	 * Page poisoning is debug page alloc for some arches. If
	 * either of those options are enabled, enable poisoning.
	 */
	/*
	 * 某些不支持原生 DEBUG_PAGEALLOC 的架构以 poisoning 实现其调试
	 * 语义；两种请求任一成立都启用同一 poisoning static key。
	 */
	if (page_poisoning_enabled() ||
	     (!IS_ENABLED(CONFIG_ARCH_SUPPORTS_DEBUG_PAGEALLOC) &&
	      debug_pagealloc_enabled())) {
		static_branch_enable(&_page_poisoning_enabled);
		page_poisoning_requested = true;
		want_check_pages = true;
	}
#endif

	if ((_init_on_alloc_enabled_early || _init_on_free_enabled_early) &&
	    page_poisoning_requested) {
		pr_info("mem auto-init: CONFIG_PAGE_POISONING is on, "
			"will take precedence over init_on_alloc and init_on_free\n");
		_init_on_alloc_enabled_early = false;
		_init_on_free_enabled_early = false;
	}

	/* 分配时清零会触及 page 状态，因此同时启用健全性检查。 */
	if (_init_on_alloc_enabled_early) {
		want_check_pages = true;
		static_branch_enable(&init_on_alloc);
	} else {
		static_branch_disable(&init_on_alloc);
	}

	if (_init_on_free_enabled_early) {
		want_check_pages = true;
		static_branch_enable(&init_on_free);
	} else {
		static_branch_disable(&init_on_free);
	}

	if (IS_ENABLED(CONFIG_KMSAN) &&
	    (_init_on_alloc_enabled_early || _init_on_free_enabled_early))
		/*
		 * KMSAN 需要观察未初始化状态，自动清零会掩盖缺陷；这里只
		 * 警告，因为用户显式策略仍被尊重。
		 */
		pr_info("mem auto-init: please make sure init_on_alloc and init_on_free are disabled when running KMSAN\n");

#ifdef CONFIG_DEBUG_PAGEALLOC
	if (debug_pagealloc_enabled()) {
		want_check_pages = true;
		static_branch_enable(&_debug_pagealloc_enabled);

		if (debug_guardpage_minorder())
			/* 非零最小 order 表示还需要 guard page 热路径。 */
			static_branch_enable(&_debug_guardpage_enabled);
	}
#endif

	/*
	 * Any page debugging or hardening option also enables sanity checking
	 * of struct pages being allocated or freed. With CONFIG_DEBUG_VM it's
	 * enabled already.
	 */
	/*
	 * 任一页调试/硬化功能都要求分配与释放时验证 struct page；DEBUG_VM
	 * 已将 static key 默认打开，因此只在非 DEBUG_VM 配置补开。
	 */
	if (!IS_ENABLED(CONFIG_DEBUG_VM) && want_check_pages)
		static_branch_enable(&check_pages_enabled);
}

/* Report memory auto-initialization states for this boot. */
/* 报告本次启动最终生效的栈、heap 分配和 heap 释放初始化策略。 */
/*
 * report_meminit() - 将编译期栈策略与运行期页清零 static key 汇总到日志。
 *
 * want_init_on_alloc(GFP_KERNEL)/want_init_on_free() 读取最终策略，而不是
 * 尚未消解冲突的 early bool。若 free 清零开启，再提示启动可能因遍历
 * 大量内存而耗时。只读状态、无失败。
 */
static void __init report_meminit(void)
{
	const char *stack;

	/* 栈初始化是编译期互斥配置，按 pattern、zero、off 选择标签。 */
	if (IS_ENABLED(CONFIG_INIT_STACK_ALL_PATTERN))
		stack = "all(pattern)";
	else if (IS_ENABLED(CONFIG_INIT_STACK_ALL_ZERO))
		stack = "all(zero)";
	else
		stack = "off";

	pr_info("mem auto-init: stack:%s, heap alloc:%s, heap free:%s\n",
		stack, str_on_off(want_init_on_alloc(GFP_KERNEL)),
		str_on_off(want_init_on_free()));
	if (want_init_on_free())
		pr_info("mem auto-init: clearing system memory may take some time...\n");
}

/*
 * mem_init_print_info() - 打印最终可用内存与内核各链接段占用。
 *
 * page_alloc_init_late() 在 deferred 页完成后调用，因此 nr_free_pages、
 * totalram_pages 和 CMA/HIGHMEM 统计已稳定。函数从链接器符号计算 text、
 * rwdata、rodata、init、bss 大小，并消除 init/rodata 嵌套于其他段时的
 * 重复计数，最后输出经典的 "Memory:" 汇总。
 *
 * 只读全局计数和链接符号，无锁、无分配、无失败；结果用于诊断而不
 * 改变分配器状态。
 */
static void __init mem_init_print_info(void)
{
	unsigned long physpages, codesize, datasize, rosize, bss_size;
	unsigned long init_code_size, init_data_size;

	physpages = get_num_physpages();
	codesize = _etext - _stext;
	datasize = _edata - _sdata;
	rosize = __end_rodata - __start_rodata;
	bss_size = __bss_stop - __bss_start;
	init_data_size = __init_end - __init_begin;
	init_code_size = _einittext - _sinittext;

	/*
	 * Detect special cases and adjust section sizes accordingly:
	 * 1) .init.* may be embedded into .data sections
	 * 2) .init.text.* may be out of [__init_begin, __init_end],
	 *    please refer to arch/tile/kernel/vmlinux.lds.S.
	 * 3) .rodata.* may be embedded into .text or .data sections.
	 */
	/*
	 * 链接脚本在不同架构上可把 .init 嵌入 .data、把 .init.text 放到
	 * 通常 init 边界外，或把 .rodata 嵌入 text/data。下面宏仅在 @pos
	 * 落入 [start,end) 且当前 size 足够时扣除 @adj，防止重复和下溢。
	 */
#define adj_init_size(start, end, size, pos, adj) \
	do { \
		if (&start[0] <= &pos[0] && &pos[0] < &end[0] && size > adj) \
			size -= adj; \
	} while (0)

	adj_init_size(__init_begin, __init_end, init_data_size,
		     _sinittext, init_code_size);
	adj_init_size(_stext, _etext, codesize, _sinittext, init_code_size);
	adj_init_size(_sdata, _edata, datasize, __init_begin, init_data_size);
	adj_init_size(_stext, _etext, codesize, __start_rodata, rosize);
	adj_init_size(_sdata, _edata, datasize, __start_rodata, rosize);

	/* 宏只服务本函数的链接段修正，立即取消以免污染后续源码。 */
#undef	adj_init_size

	/*
	 * available=buddy 当前空闲页；总量=物理页；reserved 用物理总量减
	 * totalram 与 CMA（CMA 可供迁移分配，需单列而非重复算 reserved）。
	 */
	pr_info("Memory: %luK/%luK available (%luK kernel code, %luK rwdata, %luK rodata, %luK init, %luK bss, %luK reserved, %luK cma-reserved"
#ifdef	CONFIG_HIGHMEM
		", %luK highmem"
#endif
		")\n",
		K(nr_free_pages()), K(physpages),
		codesize / SZ_1K, datasize / SZ_1K, rosize / SZ_1K,
		(init_data_size + init_code_size) / SZ_1K, bss_size / SZ_1K,
		K(physpages - totalram_pages() - totalcma_pages),
		K(totalcma_pages)
#ifdef	CONFIG_HIGHMEM
		, K(totalhigh_pages())
#endif
		);
}

#ifndef __HAVE_COLOR_ZERO_PAGE
/*
 * architectures that __HAVE_COLOR_ZERO_PAGE must define this function
 */
/*
 * 支持按 cache color 选择零页的架构必须自行实现；普通架构用弱默认实现
 * 将全局 __zero_page 指向链接器提供的 empty_zero_page。
 */
/*
 * arch_setup_zero_pages() - 建立架构零页选择机制的默认实现。
 *
 * 启动期只转换永久静态页的地址，无分配、无失败。架构强符号可覆盖。
 */
void __init __weak arch_setup_zero_pages(void)
{
	__zero_page = virt_to_page(empty_zero_page);
}
#endif

/*
 * init_zero_page_pfn() - 初始化架构零页并缓存其标准 PFN。
 *
 * arch_setup_zero_pages() 可建立单零页或按虚拟地址着色的选择；随后以
 * ZERO_PAGE(0) 取得基准页并写 zero_page_pfn，供 is_zero_pfn() 等快速
 * 判断。必须在普通 MM 路径使用零页前调用；无失败。
 */
static void __init init_zero_page_pfn(void)
{
	arch_setup_zero_pages();
	zero_page_pfn = page_to_pfn(ZERO_PAGE(0));
}

/*
 * arch_mm_preinit() - 架构在通用 mm_core_init() 之前的可选弱钩子。
 *
 * 默认无操作；架构强实现可补足通用分配器接管前的 MM 状态。调用时
 * memblock 仍有效、buddy 尚未完成移交，钩子不得依赖更晚的 slab/vmalloc。
 */
void __init __weak arch_mm_preinit(void)
{
}

/*
 * mem_init() - 架构释放剩余启动内存并完成架构 MM 收尾的可选弱钩子。
 *
 * 默认无操作；调用点位于 memblock_free_all() 之后、slab 初始化之前。
 * 架构实现必须遵守这一资源边界，且没有 errno 返回通道。
 */
void __init __weak mem_init(void)
{
}

/*
 * mm_core_init_early - 内存管理子系统最早期初始化（buddy 接管之前）
 *
 * 调用时机：setup_arch() 之后，mm_core_init() 之前，memblock 仍然可用，
 * buddy 分配器尚未建立，SMP 尚未启动，不能调用 kmalloc/vmalloc。
 *
 * 三步必须按此顺序执行：
 *   1. hugetlb_cma_reserve()：向 memblock 登记 gigantic 巨页所需的 CMA 保留区域，
 *      必须在 free_area_init() 之前，因为 zone 边界建立时需要读取这些保留信息。
 *   2. hugetlb_bootmem_alloc()：通过 memblock 分配 gigantic 巨页物理内存，
 *      必须在 buddy 接管之前完成，否则无法保证大块连续物理内存的可用性。
 *   3. free_area_init()：建立所有 NUMA 节点的 pg_data_t/zone 数据结构骨架，
 *      为后续 memblock_free_all() 将物理内存移交 buddy 做好准备。
 *
 * 本函数完成后 buddy 的数据结构骨架已就绪，但 free_area[] freelist 仍为空，
 * 真正的"物理页移交"发生在 mm_core_init() 中的 memblock_free_all()。
 *
 * 入参/返回：均无。调用者是 start_kernel() 的早期启动主线程；函数借用
 * memblock/架构拓扑并永久建立巨大页预留及分配器元数据。此阶段无运行期
 * 并发，不依赖 slab/vmalloc；连续内存或关键元数据无法满足时由下层
 * panic/启动失败，不能在部分 zone 已发布后回滚继续。
 */
void __init mm_core_init_early(void)
{
	/* 向 CMA 框架预留 gigantic 巨页所需的连续物理内存区域。
	 * gigantic 页（如 x86 上的 1GiB 页，order > MAX_PAGE_ORDER）无法通过
	 * buddy 分配器获取，必须在 memblock 阶段抢先占住连续物理内存，
	 * 否则内存碎片化后再也无法凑出足够大的连续区域。
	 * 结果写入 hugetlb_cma[nid] 数组，每个 NUMA 节点一个 CMA 区域指针, 即为每个 NUMA 节点（保证本地分配，减少跨节点延迟）分别预留 CMA 区域（保证物理连续，满足 gigantic 巨页的需求）。 */
	/*
		NUMA（Non-Uniform Memory Access，非均匀内存访问）

		多 CPU 系统中，每个 CPU 有自己"本地"的内存控制器，访问本地内存快，访问其他 CPU 的内存要跨总线，延迟更高：

		CPU 0 ──── 本地内存 A        CPU 1 ──── 本地内存 B
		│                              │
		└──────────── 互联总线 ─────────┘

		CPU 0 访问内存 A：延迟 ~50ns（本地）
		CPU 0 访问内存 B：延迟 ~150ns（跨节点）

		内核把每个"CPU + 本地内存"的组合叫一个 NUMA 节点（node），用 pg_data_t 结构体描述。调度器和内存分配器尽量让进程用本地节点的内存，减少跨节点访问。

		单 CPU 服务器或普通 PC 只有一个节点（node 0），NUMA 机制存在但无实际效果。
	*/
	/*
		CMA（Contiguous Memory Allocator，连续内存分配器）

		解决一个特定问题：某些硬件设备（DMA、GPU、大页）需要物理上连续的大块内存，但系统运行一段时间后内存碎片化，buddy 分配器凑不出大块连续内存。

		CMA 的做法：

		系统启动时，从 memblock 预留一块连续区域（如 1GB）

		普通情况：这块区域交给 buddy，可以分配给普通用户页（可迁移类型）
		需要大块时：把这块区域里的普通页迁移走，腾出连续空间给设备使用

		用完后：把区域还给 buddy，继续供普通分配使用

		关键在于"平时借出去用，需要时迁移回来"，避免提前锁死大块内存。
	*/
	hugetlb_cma_reserve();

	/* 通过 memblock 分配命令行（hugepages=N）指定的 gigantic 巨页。
	 * 同时解析所有巨页相关命令行参数（hugepagesz=/default_hugepagesz=），
	 * 并初始化 huge_boot_pages[nid] 链表存放已分配的页，
	 * 普通大页（2MiB/4MiB）的实际分配延迟到后期 hugetlb_init_hstates()。 */
	hugetlb_bootmem_alloc();

	/* 初始化所有 NUMA 节点的 pg_data_t、zone 数据结构及全部 struct page，
	 * 建立 buddy allocator 的骨架（zone 边界、迁移类型 usemap 等），
	 * 但不向 buddy 移交任何物理页（free_area[] freelist 仍为空）。 */
	free_area_init();
}

/*
 * Set up kernel memory allocators
 */
/* 建立内核内存分配器及依赖其阶段边界的调试、映射和缓存子系统。 */
/*
 * mm_core_init() - 从 memblock/buddy 骨架过渡到完整运行期 MM。
 *
 * 前置条件：mm_core_init_early()/free_area_init() 已建立 pgdat、zone、
 * memmap 与空 buddy 容器，SMP 基础已可用，memblock 仍持有未移交页。
 *
 * 主要阶段：
 *   1. 架构预处理、零页、zonelist、CPU hotplug 与 alloc tag；
 *   2. page_ext/硬化/KFENCE/KMSAN/stack depot 等必须在移交前准备的元数据；
 *   3. KHO 最后使用 memblock，随后 memblock_free_all() 把空闲物理页交
 *      给 buddy，这是本文件最关键的所有权转换点；
 *   4. 架构 mem_init、slab、page owner/kmemleak、页表锁缓存；
 *   5. debug objects、vmalloc、page_ext、espfix/PTI/KMSAN runtime、
 *      mm cache 与 execmem。
 *
 * 顺序是接口契约：前半不能依赖 slab/vmalloc，后半必须看到 buddy/slab
 * 就绪；函数无返回值，关键基础设施失败通常由各子系统 panic/禁用处理。
 */
void __init mm_core_init(void)
{
	/* 阶段 1a：让架构补足通用 MM 接管前状态，并建立共享零页身份。 */
	arch_mm_preinit();
	init_zero_page_pfn();

	/* Initializations relying on SMP setup */
	/* 以下初始化依赖 SMP/CPU 拓扑已建立。 */
	/* zonelist 实现当前只允许最多两个列表模式，编译期强制不变量。 */
	BUILD_BUG_ON(MAX_ZONELISTS > 2);
	/* 依据 node distance/zone 容量建立分配回退顺序。 */
	build_all_zonelists(NULL);
	/* 注册 page allocator 的 CPU online/offline 回调并准备 per-CPU 状态。 */
	page_alloc_init_cpuhp();
	/* 为带分配标签的链接段建立运行期元数据。 */
	alloc_tag_sec_init();
	/*
	 * page_ext requires contiguous pages,
	 * bigger than MAX_PAGE_ORDER unless SPARSEMEM.
	 */
	/*
	 * 非 SPARSEMEM 的 page_ext backing 可能要求超过 buddy 最大 order 的
	 * 连续页，必须趁 memblock 仍可提供大连续区时先分配 flatmem 部分。
	 */
	page_ext_init_flatmem();
	/* 汇总 early params 后一次启用页填充、poison、guard/check static keys。 */
	mem_debugging_and_hardening_init();
	/* 在页全面移交前预留 KFENCE 对象池及其元数据。 */
	kfence_alloc_pool_and_metadata();
	/* 报告最终硬化策略，便于解释后续启动耗时与页内容。 */
	report_meminit();
	/* 建立 KMSAN 的 shadow backing，必须先于普通页广泛使用。 */
	kmsan_init_shadow();
	/* stack depot 的早期存储供 page owner/KMSAN 等记录调用栈。 */
	stack_depot_early_init();

	/*
	 * KHO memory setup must happen while memblock is still active, but
	 * as close as possible to buddy initialization
	 */
	/*
	 * KHO 仍需要查询/预留 memblock，但应尽量靠近移交点，避免其快照与
	 * 最终物理布局之间出现更多变化。
	 */
	kho_memory_init();

	/*
	 * 核心所有权转换：memblock 中未保留且描述符已就绪的页进入 buddy；
	 * 此后通用分配应使用 alloc_pages/slab，而非新增 memblock 预留。
	 */
	memblock_free_all();
	/* 允许架构释放最后的启动页并完成高端内存等架构特有状态。 */
	mem_init();
	/* buddy 可分配页稳定后创建 slab caches，启用 kmalloc 家族。 */
	kmem_cache_init();
	/*
	 * page_owner must be initialized after buddy is ready, and also after
	 * slab is ready so that stack_depot_init() works properly
	 */
	/*
	 * page owner 的晚期 page_ext 既依赖 buddy 页，也依赖 slab 支撑完整
	 * stack depot，故严格位于两者之后。
	 */
	page_ext_init_flatmem_late();
	/* slab/buddy 就绪后启动通用内存泄漏跟踪。 */
	kmemleak_init();
	/* 建立页表锁和架构页表对象的 slab cache。 */
	ptlock_cache_init();
	pgtable_cache_init();
	/* debugobjects 可开始使用成熟的内存分配器。 */
	debug_objects_mem_init();
	/* 建立完整 vmap/vmalloc 地址空间和管理树。 */
	vmalloc_init();
	/* If no deferred init page_ext now, as vmap is fully initialized */
	/*
	 * 未延迟 struct page 时，现在 vmap 已就绪，可初始化通用 page_ext；
	 * deferred 情况必须等 page_alloc_init_late() 的全部 page 完成。
	 */
	if (!deferred_struct_pages)
		page_ext_init();
	/* Should be run before the first non-init thread is created */
	/* BSP espfix 必须早于第一个普通线程，保证异常返回栈隔离。 */
	init_espfix_bsp();
	/* Should be run after espfix64 is set up. */
	/* PTI 的最终页表布局依赖 espfix64 映射已存在。 */
	pti_init();
	/* shadow 与核心分配器均就绪后切换 KMSAN 到运行期。 */
	kmsan_init_runtime();
	/* 创建 MM 专用 slab/cache 等通用后期设施。 */
	mm_cache_init();
	/* 最后建立可执行内存分配区域及其架构映射策略。 */
	execmem_init();
}
